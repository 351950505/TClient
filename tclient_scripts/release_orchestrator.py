"""TClient one-command release orchestrator (two-phase).

Default phase performs the complete LOCAL release: preflight, mirror probe, build,
test, stage, Authenticode evidence, packaging, Ed25519 manifest signature and full
byte verification — exactly the flow used for v1.7.8.55-v1.7.8.59, but driven
from one spec file instead of hand-derived per-version scripts.

With ``--publish`` it additionally creates a DRAFT GitHub release, uploads the
six Windows assets, re-downloads and hashes every asset, publishes the release
only after all assets match, syncs the signed master manifest pair via the Git
Data API, and re-verifies the raw URLs.

Usage:
    python release_orchestrator.py run --spec <spec.json> [--publish] [--dry-run] [--resume]

Secrets never live in the spec or the state file: the Ed25519 key path comes
from the spec, the GitHub token is obtained via ``git credential fill`` at the
publish phase only.

Exit codes: 0 success, 1 failed requirement, 2 failed requirement during a
resumed phase.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from urllib.parse import quote

DEFAULT_REPO = Path("D:/TaterClient-src")
DEFAULT_WORK_ROOT = Path("D:/kehuduan")
DEFAULT_PRIVATE_KEY = Path("D:/kehuduan/tclient-update-ed25519-private.pem")
GH_EXE = Path(shutil.which("gh.exe") or shutil.which("gh") or (Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "GitHub CLI" / "gh.exe"))
HELPER_LOCK = Path(__file__).resolve().parent.parent / "other" / "island_media_monitor" / "helper-lock.json"
GITHUB_REPO = "351950505/TClient"
RAW_MANIFEST_URL = "https://raw.githubusercontent.com/" + GITHUB_REPO + "/master/update/TClient-update-win64.json"
REQUIRED_SPEC_KEYS = ("version", "target_commit", "parent_commit", "changed_files", "notes_path")
OPTIONAL_SPEC_DEFAULTS = {
    "repo": str(DEFAULT_REPO),
    "work_root": str(DEFAULT_WORK_ROOT),
    "build_dir": str(DEFAULT_REPO / "build-vs2026"),
    "private_key": str(DEFAULT_PRIVATE_KEY),
    "monitor_path": str(DEFAULT_WORK_ROOT / "release-17856" / "island_media_monitor.exe"),
    "watcher_path": str(DEFAULT_WORK_ROOT / "tools" / "netease-watcher" / "netease-watcher.exe"),
    "hook_dll_path": str(DEFAULT_WORK_ROOT / "tools" / "netease-watcher" / "wndhok.dll"),
    "resources_dir": "",  # required: previous version stage directory
    "signed_expected": False,
    # RFC 3161 timestamp authority used by the Authenticode Sign phase.
    "timestamp_url": "https://timestamp.digicert.com",
    "publish": False,
    "expected_master": "",  # optional: assert master at this SHA before the sync push
    "mirror_probe_nodes": [],  # additional regional probes (e.g. [{'region': 'cn-guangzhou', 'host': 'aliyun'}])
}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        for block in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True, timeout=120).stdout.decode().strip()


def previous_version(version):
    """Return the previous version, rolling a zero suffix back to .99."""
    parts = version.split(".")
    require(len(parts) >= 2 and all(part.isdigit() for part in parts), "cannot derive the previous version from " + version)
    for index in range(len(parts) - 1, 0, -1):
        if int(parts[index]) == 0:
            continue
        parts[index] = str(int(parts[index]) - 1)
        for suffix in range(index + 1, len(parts)):
            parts[suffix] = "99"
        return ".".join(parts)
    raise RuntimeError("cannot derive the previous version from " + version)


def check_helper_lock(spec):
    lock = json.loads(HELPER_LOCK.read_text(encoding="utf-8-sig"))
    for name, record in lock["files"].items():
        path = Path(spec["watcher_path"] if name == "netease-watcher.exe" else spec["hook_dll_path"])
        require(path.is_file(), f"helper missing: {path}")
        require(path.stat().st_size == record["size"], f"helper size differs from lock: {name}")
        digest = sha256_of(path)
        require(digest == record["sha256"], f"helper sha256 differs from lock: {name} ({digest})")


def phase_preflight(spec, paths, state):
    repo = Path(spec["repo"])
    require(git(repo, "rev-parse", "HEAD") == spec["target_commit"], "HEAD differs from the target commit")
    require(not git(repo, "status", "--porcelain=v1", "--untracked-files=all"), "source worktree is not clean")
    require(
        subprocess.run(["git", "-C", str(repo), "merge-base", "--is-ancestor", spec["parent_commit"], spec["target_commit"]], capture_output=True, timeout=60).returncode == 0,
        "parent_commit is not an ancestor of the target commit",
    )
    changed = set(git(repo, "diff", "--name-only", spec["parent_commit"], spec["target_commit"]).splitlines())
    require(changed == set(spec["changed_files"]), "changed-file set differs from the spec (cumulative diff " + spec["parent_commit"][:12] + ".." + spec["target_commit"][:12] + ")")
    version_h = (repo / "src/game/version.h").read_text(encoding="utf-8")
    require(f'#define TCLIENT_APP_VERSION "{spec["version"]}"' in version_h, "version.h does not declare the spec version")
    # 审计修复（2026-11-20 P2）：data_version.txt 与内嵌版本不一致时客户端运行
    # 时弹 mismatch 弹窗，链上必须提前拦下
    data_version_txt = (repo / "data/tclient/data_version.txt").read_text(encoding="utf-8-sig").strip()
    require(data_version_txt == f'TClient {spec["version"]}', f"data_version.txt says '{data_version_txt}', expected 'TClient {spec['version']}'")
    probe_nodes = spec.get("mirror_probe_nodes", [])
    require(isinstance(probe_nodes, list) and len(probe_nodes) <= 4, "release spec supports at most 4 regional mirror probes")
    probe_regions = set()
    for node in probe_nodes:
        require(isinstance(node, dict), "regional mirror probe must be an object")
        region, host = node.get("region", ""), node.get("host", "")
        require(re.fullmatch(r"[a-z0-9][a-z0-9-]{0,31}", region) and re.fullmatch(r"[A-Za-z0-9_.-]+", host), "invalid regional mirror probe region/SSH alias")
        require(region not in probe_regions, "duplicate regional mirror probe: " + region)
        probe_regions.add(region)
    if probe_nodes:
        require(any(region.startswith("cn-") for region in probe_regions), "regional mirror probes must include a China region")
    check_helper_lock(spec)
    translation_gate = Path(__file__).resolve().parent / "check_translations.py"
    require(translation_gate.is_file(), f"translation gate missing: {translation_gate}")
    gate = subprocess.run(
        [sys.executable, "-I", str(translation_gate), "--repo", str(repo)],
        capture_output=True, text=True, encoding="utf-8", timeout=300,
    )
    require(gate.returncode == 0, "translation gate failed:\n" + gate.stdout[-2000:] + gate.stderr[-2000:])
    build_options_test = Path(__file__).resolve().parent / "test_release_build_options.py"
    require(build_options_test.is_file(), f"release build options test missing: {build_options_test}")
    build_options = subprocess.run(
        [sys.executable, "-I", str(build_options_test)],
        capture_output=True, text=True, encoding="utf-8", timeout=60,
    )
    require(build_options.returncode == 0, "release build options test failed:\n" + build_options.stdout[-2000:] + build_options.stderr[-2000:])
    notes = Path(spec["notes_path"])
    require(notes.is_file() and notes.read_text(encoding="utf-8").strip(), "release notes missing or empty")
    require(Path(spec["monitor_path"]).is_file(), "monitor exe missing")
    for key in ("watcher_path", "hook_dll_path"):
        require(Path(spec[key]).is_file(), f"{key} missing")
    return {"tree": git(repo, "rev-parse", f'{spec["target_commit"]}^{{tree}}')}


def mirror_files(spec, paths):
    return paths["root"] / "update-mirrors.json", Path(spec["build_dir"]) / "src/generated/tclient_update_mirrors.h"


def verify_mirror_inputs(spec, paths, state, client=None, fresh=False):
    report_path, header = mirror_files(spec, paths)
    require(report_path.is_file() and header.is_file(), "release mirror evidence is missing; run mirror_probe before building")
    report = json.loads(report_path.read_text(encoding="utf-8"))
    require(report.get("schema") == 1 and report.get("catalog_url") == "https://github.akams.cn/", "mirror report has an unexpected source or schema")
    require(report.get("release_version") == spec["version"] and report.get("source_commit") == spec["target_commit"], "mirror report belongs to a different release or commit")
    required_regions = {"local", *(node["region"] for node in spec["mirror_probe_nodes"])}
    measured_regions = set(report.get("probe_regions", []))
    require(required_regions.issubset(measured_regions), "mirror report lacks required regional measurements")
    require(report.get("header_sha256") == sha256_of(header), "mirror header differs from its tested report; rebuild from a fresh probe")
    selected = report.get("selected", [])
    require(1 <= len(selected) <= 5, "no verified release mirrors selected")
    for item in selected:
        regional = item.get("regional_results", {})
        require(all(regional.get(region, {}).get("package_ok") for region in required_regions - {"local"}),
                "selected mirror lacks complete package/resume verification in a required region")
    prefixes = [item["prefix"] for item in selected]
    require(len(set(prefixes)) == len(prefixes) and all(re.fullmatch(r"https://[a-z0-9.-]+/", prefix) for prefix in prefixes), "invalid or repeated mirror prefix")
    record = {"report": str(report_path), "report_sha256": sha256_of(report_path), "header_sha256": sha256_of(header), "selected": prefixes, "reference": report["reference"]}
    frozen = state.get("phases", {}).get("mirror_probe")
    if frozen:
        require(record == frozen, "mirror inputs changed after the probe phase; start a new release build")
    if fresh:
        checked = datetime.fromisoformat(report["fetched_at"].replace("Z", "+00:00"))
        require(checked.tzinfo is not None, "mirror report timestamp lacks a timezone")
        age = (datetime.now(timezone.utc) - checked).total_seconds()
        require(0 <= age <= 24 * 60 * 60, "mirror probe is older than 24 hours; run a new probe and rebuild")
    if client is not None:
        for prefix in prefixes:
            url = prefix + "https://github.com/" + GITHUB_REPO + "/releases/latest/download/TClient-update-win64.json"
            require(url.encode("ascii") in client, "client lacks a tested update mirror: " + prefix)
    return record


def release_mirror_probe_command(python, script, repo, report, header, spec):
    command = [
        str(python), "-I", str(script), "--repo", str(repo),
        "--output", str(report), "--header", str(header),
        "--release-version", spec["version"], "--source-commit", spec["target_commit"],
    ]
    for node in spec["mirror_probe_nodes"]:
        command += ["--probe-node", f'{node["region"]}={node["host"]}']
    return command


def phase_mirror_probe(spec, paths, state):
    script = Path(spec["repo"]) / "tclient_scripts/probe_update_mirrors.py"
    require(script.is_file(), f"mirror probe missing: {script}")
    report, header = mirror_files(spec, paths)
    subprocess.run(release_mirror_probe_command(sys.executable, script, spec["repo"], report, header, spec),
                   check=True, timeout=5400)
    record = verify_mirror_inputs(spec, paths, state, fresh=True)
    shutil.copyfile(header, paths["root"] / "update-mirrors.h")
    return record


def release_cmake_configure_command(cmake, repo, build_dir):
    return [str(cmake), "-S", str(repo), "-B", str(build_dir), "-DTCLIENT_RELEASE_MIRRORS=ON", "-DVULKAN=ON"]


def phase_build(spec, paths, state):
    cmake = Path("C:/VS2026/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")
    require(cmake.is_file(), "VS2026 cmake not found")
    verify_mirror_inputs(spec, paths, state, fresh=True)
    build_dir = Path(spec["build_dir"])
    subprocess.run(release_cmake_configure_command(cmake, spec["repo"], build_dir), check=True, timeout=600)
    cache = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8-sig")
    require(re.search(r"^VULKAN:BOOL=ON$", cache, re.M), "release build did not enable the Vulkan backend")
    subprocess.run([str(cmake), "--build", str(build_dir), "--config", "Release", "--target", "game-client", "TClientUpdater", "testrunner", "--parallel"], check=True, timeout=3600)
    repo = Path(spec["repo"])
    libs = repo / "ddnet-libs"
    env = dict(os.environ)
    for sub in ("sqlite3", "curl", "freetype", "opus", "opusfile", "png", "sdl", "zlib"):
        env["PATH"] = str(libs / sub / "windows" / "lib64") + os.pathsep + env["PATH"]
    env["PATH"] = str(build_dir) + os.pathsep + str(build_dir / "Release") + os.pathsep + env["PATH"]
    result = subprocess.run([str(build_dir / "Release" / "testrunner.exe")], capture_output=True, timeout=1800, env=env, cwd=str(repo))
    if result.returncode != 0:
        tail = (result.stdout or b"")[-1500:].decode(errors="replace")
        err = (result.stderr or b"")[-1500:].decode(errors="replace")
        raise RuntimeError(f"testrunner failed: {result.returncode}\nSTDOUT:{tail}\nSTDERR:{err}")
    client = build_dir / "Release" / "DDNet.exe"
    data = client.read_bytes()
    verify_mirror_inputs(spec, paths, state, client=data)
    require(f'{spec["version"]}\0'.encode() in data, "client does not embed the release version")
    short = git(repo, "rev-parse", "--short", spec["target_commit"])
    require(short.encode() in data, "client does not embed the release commit")
    return {"client_sha256": sha256_of(client), "client_size": client.stat().st_size, "testrunner": "pass"}


def verify_client_phase(state, phase, client_path):
    expected = state.get("phases", {}).get(phase, {}).get("client_sha256")
    require(expected and sha256_of(client_path) == expected, f"client differs from the frozen {phase} phase; rebuild the release")


def phase_stage(spec, paths, state):
    client_path = Path(spec["build_dir"]) / "Release/DDNet.exe"
    verify_client_phase(state, "build", client_path)
    verify_mirror_inputs(spec, paths, state, client=client_path.read_bytes(), fresh=True)
    stage = paths["stage"]
    require(not stage.exists(), f"stage directory already exists: {stage}")
    script = Path(spec["repo"]) / "tclient_scripts" / "prepare_release_stage.py"
    command = [
        sys.executable, "-I", str(script),
        "--build-dir", str(Path(spec["build_dir"]) / "Release"),
        "--runtime-dir", str(Path(spec["build_dir"])),
        "--runtime-dir", str(Path(spec["build_dir"]).parent / "ddnet-libs" / "discord" / "windows" / "lib64"),
        "--runtime-dir", str(Path(spec["build_dir"]).parent / "ddnet-libs" / "vulkan" / "windows" / "lib64"),
        "--output", str(stage),
        "--report", str(paths["stage_inventory"]),
        "--monitor", str(Path(spec["monitor_path"])),
        "--watcher", str(Path(spec["watcher_path"])),
        "--hook-dll", str(Path(spec["hook_dll_path"])),
    ]
    if spec.get("resources_dir"):
        command += ["--resources", str(Path(spec["resources_dir"]))]
    if Path(spec["repo"]).joinpath("data").is_dir():
        command += ["--data-dir", str(Path(spec["repo"]) / "data")]
    subprocess.run(command, check=True, timeout=1800)
    # 递归计数必须是「每个 rglob 命中是否普通文件」；旧写法用 Path(_).name 拼回
    # stage 根目录，嵌套文件全部被判为不存在，1031 个文件会报成 31。
    staged_client = stage / "DDNet.exe"
    verify_client_phase(state, "build", staged_client)
    return {"stage_files": sum(1 for path in stage.rglob("*") if path.is_file()), "client_sha256": sha256_of(staged_client)}


def phase_package(spec, paths, state):
    repo = Path(spec["repo"])
    version = spec["version"]
    stage = paths["stage"]
    output = paths["assets"]
    full = paths["full_zip"]
    require(not output.exists() and not full.exists(), "asset destination already exists")

    assets = load_module("release_assets", repo / "tclient_scripts" / "make_release_assets.py")
    helper = load_module("release_stage", repo / "tclient_scripts" / "prepare_release_stage.py")
    inventory = helper.validate_stage(stage, paths["stage_inventory"])
    require(Path(inventory["roots"]["monitor"]) == Path(spec["monitor_path"]), "stage monitor origin differs from the spec")
    full_files = assets.collect_files(stage, None)
    full_archive_files = assets.prefix_files(full_files, version)
    update_files = assets.collect_files(stage, assets.UPDATE_DATA_SUBDIRS)
    require({name for name, _ in full_files} == {entry["path"] for entry in inventory["files"]}, "full inventory differs from stage")
    require({name for name, _ in update_files}.issubset({name for name, _ in full_files}), "update package is not a subset")

    for entry in inventory["files"]:
        if entry["path"] in helper.FIRST_PARTY_FILES:
            origin = Path(entry["origin"])
            require(origin.is_file() and assets.sha256_of(origin) == entry["sha256"], "first-party build origin differs: " + entry["path"])

    client_path = stage / "DDNet.exe"
    verify_client_phase(state, "stage", client_path)
    client = client_path.read_bytes()
    mirrors = verify_mirror_inputs(spec, paths, state, client=client, fresh=True)
	# 版本串绑定：previous 从 version 自身推导；末段归零时回退到前段的 .99。
    previous = previous_version(version)
    require(f"{version}\0".encode() in client and f"{previous}\0".encode() not in client, "stage client version is wrong")
    require(git(repo, "rev-parse", "--short", spec["target_commit"]).encode() in client, "stage client lacks the release commit")

    before = signatures(Path(spec["repo"]), stage, "unsigned")
    if spec.get("signed_expected"):
        thumbprint = os.environ.get("TCLIENT_CERT_THUMBPRINT", "")
        require(thumbprint, "TCLIENT_CERT_THUMBPRINT required for signed releases")
        sign = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-File", str(repo / "tclient_scripts" / "windows_sign_release.ps1"), "-Directory", str(stage), "-Mode", "Sign", "-CertificateThumbprint", thumbprint, "-CertificateStoreLocation", "CurrentUser", "-TimestampUrl", spec["timestamp_url"]], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=600)
        require(sign.returncode == 0, "Authenticode signing failed: " + sign.stderr)
    after = signatures(Path(spec["repo"]), stage, "signed" if spec.get("signed_expected") else "unsigned")

    private_key = assets.load_signing_key(Path(spec["private_key"]))
    metadata = {"product": "TClient", "channel": "stable", "version": version, "platform": "win64"}
    output.mkdir()
    assets.write_zip(update_files, output / assets.ASSET_NAME)
    assets.write_zip(full_archive_files, full)
    for name in ("DDNet.exe", assets.HELPER_NAME):
        shutil.copyfile(stage / name, output / name)
    notes = Path(spec["notes_path"]).read_text(encoding="utf-8").strip()
    manifest = {
        **metadata,
        "asset": assets.ASSET_NAME,
        "size": (output / assets.ASSET_NAME).stat().st_size,
        "sha256": assets.sha256_of(output / assets.ASSET_NAME),
        "notes": notes,
        "helper": {"name": assets.HELPER_NAME, "size": (output / assets.HELPER_NAME).stat().st_size, "sha256": assets.sha256_of(output / assets.HELPER_NAME)},
    }
    manifest_bytes = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    signature = private_key.sign(manifest_bytes)
    del private_key
    assets.validate_manifest(manifest_bytes, signature, metadata)
    assets.write_manifest_pair(output, (manifest_bytes, signature))
    assets.write_manifest_pair(output / "update", (manifest_bytes, signature))
    pair = assets.verify_release_assets(output, metadata)
    require(pair == (manifest_bytes, signature), "manifest pair mismatch")
    assets.verify_zip_contents(output / assets.ASSET_NAME, update_files)
    assets.verify_zip_contents(full, full_archive_files)
    # 签名版 stage 的一方 EXE 字节已被 Authenticode 改写；只在该阶段放行 first-party
    # 差异（build origin 校验已在 package 开头完成）。
    helper.validate_stage(stage, paths["stage_inventory"], allow_first_party_changes=bool(spec.get("signed_expected")))
    for path, expected in ((output / assets.ASSET_NAME, update_files), (full, full_archive_files)):
        with zipfile.ZipFile(path) as archive:
            entries = assets.archive_entries(archive)
            require(archive.testzip() is None, "ZIP CRC failed: " + str(path))
            require(set(entries) == {name for name, _ in expected}, "ZIP entry set differs")
            for name, source in expected:
                require(archive.read(entries[name]) == source.read_bytes(), "ZIP bytes differ: " + name)

    record = {
        "version": version,
        "source_commit": spec["target_commit"],
        "source_tree": git(Path(spec["repo"]), "rev-parse", spec["target_commit"] + "^{tree}"),
        "parent": spec["parent_commit"],
        "update_mirrors": mirrors,
        "helper_lock": "passed",
        "assets": [
            {"name": path.name, "path": str(path), "size": path.stat().st_size, "sha256": sha256_of(path)}
            for path in (output / assets.ASSET_NAME, full, output / assets.MANIFEST_NAME, output / assets.MANIFEST_SIG_NAME, output / "DDNet.exe", output / assets.HELPER_NAME)
        ],
        "authenticode_before": before,
        "authenticode_after": after,
    }
    (paths["root"] / "local-assets.json").write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return {"assets": record["assets"]}


def signatures(root, directory, expected="unsigned"):
    script = Path(root) / "tclient_scripts" / "check_signatures.ps1"  # root here = repo
    require(script.is_file(), f"missing {script}")
    result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-File", str(script), "-Directory", str(directory), "-Expected", expected], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=300)
    require(result.returncode == 0, "Authenticode evidence failed: " + result.stderr)
    return json.loads(result.stdout)


def github_token(repo):
    result = subprocess.run(["git", "-C", str(repo), "credential", "fill"], input=b"protocol=https\nhost=github.com\n\n", capture_output=True, timeout=60)
    require(result.returncode == 0, "git credential fill failed")
    values = dict(line.split(b"=", 1) for line in result.stdout.splitlines() if b"=" in line)
    require(bool(values.get(b"password")), "no GitHub credential available")
    return values[b"password"].decode()


def gh(repo, *args, token, input_text=None):
    require(GH_EXE.is_file(), f"gh.exe not found: {GH_EXE}")
    env = dict(os.environ, GH_HOST="github.com", GH_TOKEN=token, GH_PROMPT_DISABLED="1")
    # gh api 没有 --repo（endpoint 写成 repos/owner/repo/...）；--repo 只对 release 等子命令合法
    cmd = [str(GH_EXE), *args]
    if args[0] != "api":
        cmd += ["--repo", GITHUB_REPO]
    # 必须显式指定 UTF-8。Windows 上 text=True 走 locale（cp936/GBK），中文 payload
    # 会被 GBK 编码后交给 gh，gh 按 UTF-8 解析就报 "Problems parsing JSON"；
    # 读取响应时 GBK 解码同样会在中文字节上抛 UnicodeDecodeError。
    result = subprocess.run(cmd, env=env, capture_output=True, text=True, encoding="utf-8",
                            input=input_text, timeout=300)
    require(result.returncode == 0, "gh failed: " + (result.stderr or result.stdout))
    return result.stdout


def release_body(repo, notes_path):
    """Release 正文的原始文本。

    v1.7.8.61/62 的正文在 GitHub 上是一串 ``\\u2014`` 字面量，根因是旧代码用
    ``json.dumps()``（默认 ensure_ascii=True）拼 ``-F body=...``：中文被转义成
    ``\\uXXXX``，换行被转义成字面 ``\\n``，还多出一层引号，gh 把整串当纯字符串提交。
    现在改为把整个 payload 以 UTF-8 JSON 从 stdin 交给 ``gh api --input -``，
    shell 与参数层都不再参与转义。
    """
    text = Path(notes_path).read_text(encoding="utf-8").strip()
    require(bool(text), "release notes are empty")
    return text


def gh_api_json(repo, endpoint, token, method, payload):
    """以 JSON body 调用 GitHub API，返回解析后的响应。

    正文按原字节提交：``json.dumps(ensure_ascii=False)`` 保证中文与换行保持
    可见字符，``--input -`` 保证不经 shell/argv 二次转义。
    """
    body = json.dumps(payload, ensure_ascii=False)
    out = gh(repo, "api", endpoint, "-X", method, "--input", "-", token=token, input_text=body)
    return json.loads(out) if out.strip() else {}


def gh_api_pages(root, endpoint, token):
    items = []
    page = 1
    while True:
        batch = json.loads(gh(root, "api", f"{endpoint}?per_page=100&page={page}", token=token))
        require(isinstance(batch, list), "unexpected GitHub list response: " + endpoint)
        items.extend(batch)
        if len(batch) < 100:
            return items
        page += 1


def remote_tag_commit(root, tag, token):
    base = f"repos/{GITHUB_REPO}/git/"
    refs = gh_api_pages(root, base + "matching-refs/tags/" + quote(tag, safe=""), token)
    matches = [ref for ref in refs if ref.get("ref") == "refs/tags/" + tag]
    require(len(matches) <= 1, "multiple refs for tag " + tag)
    if not matches:
        return None
    obj = matches[0]["object"]
    seen = set()
    while obj.get("type") == "tag":
        sha = obj["sha"]
        require(sha not in seen and len(seen) < 32, "invalid annotated tag chain: " + tag)
        seen.add(sha)
        obj = json.loads(gh(root, "api", base + "tags/" + sha, token=token))["object"]
    require(obj.get("type") == "commit", "tag does not resolve to a commit: " + tag)
    return obj["sha"]


def verify_release_target(spec, root, release, token, tag_required=False):
    tag = f'v{spec["version"]}'
    require(release.get("tag_name") == tag, "release tag differs from the spec")
    commit = remote_tag_commit(root, tag, token)
    if commit is not None:
        require(commit == spec["target_commit"], "tag commit differs from the frozen target: " + tag)
    else:
        require(not tag_required, "release tag is missing: " + tag)
        target = quote(release.get("target_commitish", ""), safe="")
        require(bool(target), "draft release target is missing")
        resolved = json.loads(gh(root, "api", f"repos/{GITHUB_REPO}/commits/{target}", token=token))
        require(resolved.get("sha") == spec["target_commit"], "draft target differs from the frozen target")


def verify_remote_assets(root, release_id, frozen, token, complete):
    endpoint = f"repos/{GITHUB_REPO}/releases/{release_id}/assets"
    assets = gh_api_pages(root, endpoint, token)
    names = [asset["name"] for asset in assets]
    require(len(names) == len(set(names)), "duplicate remote asset names")
    require(set(names).issubset(frozen), "unexpected remote assets: " + ", ".join(sorted(set(names) - frozen.keys())))
    if complete:
        require(set(names) == set(frozen), "remote asset set differs from the frozen package")
    for asset in assets:
        record = frozen[asset["name"]]
        require(asset["size"] == record["size"], "remote asset size differs: " + asset["name"])
        digest = asset.get("digest")
        if not digest:
            # Older GitHub assets may lack digest metadata; hash the bytes instead.
            result = subprocess.run([str(GH_EXE), "api", f'repos/{GITHUB_REPO}/releases/assets/{asset["id"]}', "-H", "Accept: application/octet-stream"], env=dict(os.environ, GH_TOKEN=token, GH_HOST="github.com"), capture_output=True, timeout=1800)
            require(result.returncode == 0, "remote asset download failed: " + asset["name"])
            require(len(result.stdout) == record["size"], "remote asset download size differs: " + asset["name"])
            digest = "sha256:" + hashlib.sha256(result.stdout).hexdigest()
        require(digest == "sha256:" + record["sha256"], "remote asset digest differs: " + asset["name"])
    return set(names)


def phase_publish(spec, paths, state):
    root = paths["root"]
    assets_record = json.loads((root / "local-assets.json").read_text(encoding="utf-8-sig"))
    require(assets_record.get("version") == spec["version"] and assets_record.get("source_commit") == spec["target_commit"], "frozen package belongs to a different release or commit")
    records = assets_record["assets"]
    frozen = {record["name"]: record for record in records}
    require(bool(frozen) and len(frozen) == len(records), "empty or duplicate frozen asset names")
    asset_paths = {name: Path(record["path"]) for name, record in frozen.items()}
    require("DDNet.exe" in asset_paths, "frozen package lacks DDNet.exe")
    for name, path in asset_paths.items():
        require(path.name == name and path.is_file(), "packaged asset path differs or is missing: " + name)
        require(path.stat().st_size == frozen[name]["size"] and sha256_of(path) == frozen[name]["sha256"], "packaged asset differs from the frozen package record: " + name)
    mirrors = verify_mirror_inputs(spec, paths, state, client=asset_paths["DDNet.exe"].read_bytes(), fresh=False)
    require(assets_record.get("update_mirrors") == mirrors, "packaged assets do not match the frozen mirror probe")
    token = github_token(Path(spec["repo"]))
    tag = f'v{spec["version"]}'
    endpoint = f"repos/{GITHUB_REPO}/releases"
    existing = [release for release in gh_api_pages(root, endpoint, token) if release.get("tag_name") == tag]
    require(len(existing) <= 1, "multiple releases for tag " + tag)
    release = existing[0] if existing else {"tag_name": tag, "target_commitish": spec["target_commit"], "draft": True}
    verify_release_target(spec, root, release, token, tag_required=not release.get("draft"))
    if release.get("draft"):
        verify_mirror_inputs(spec, paths, state, fresh=True)
    if not existing:
        release = gh_api_json(root, endpoint, token, "POST", {
            "tag_name": tag,
            "target_commitish": spec["target_commit"],
            "name": f'TClient-v{spec["version"]}',
            "draft": True,
            "body": release_body(root, spec["notes_path"]),
        })
        verify_release_target(spec, root, release, token)
    release_id = release["id"]
    present = verify_remote_assets(root, release_id, frozen, token, complete=not release["draft"])
    if release["draft"]:
        for name, path in asset_paths.items():
            if name in present:
                continue
            require(path.stat().st_size == frozen[name]["size"] and sha256_of(path) == frozen[name]["sha256"], "packaged asset changed before upload: " + name)
            upload = subprocess.run([str(GH_EXE), "release", "upload", tag, "--repo", GITHUB_REPO, str(path)], env=dict(os.environ, GH_TOKEN=token, GH_HOST="github.com"), capture_output=True, text=True, encoding="utf-8", timeout=1800)
            require(upload.returncode == 0, f"asset upload failed: {name}: " + upload.stderr)
        verify_release_target(spec, root, release, token)
        verify_remote_assets(root, release_id, frozen, token, complete=True)
        publish = subprocess.run([str(GH_EXE), "release", "edit", tag, "--repo", GITHUB_REPO, "--draft=false", "--latest"], env=dict(os.environ, GH_TOKEN=token, GH_HOST="github.com"), capture_output=True, text=True, encoding="utf-8", timeout=300)
        require(publish.returncode == 0, "publishing the release failed: " + publish.stderr)
    state["remote_digests"] = {name: record["sha256"] for name, record in frozen.items()}
    return {"release_id": release_id, "published": True}


def phase_master_sync(spec, paths, state):
    # 用仓库内参数化的脚本；脚本自身幂等：master 已带同一对清单/签名时只回读
    # 并写记录，不会二次推送。
    # 审计修复（2026-11-20 P3）：删掉 work_root 下克隆的首选分支——将来出现
    # 旧克隆会静默用错版本的 sync 脚本。
    script_path = Path(spec["repo"]) / "tclient_scripts" / "sync_master_manifest.py"
    require(script_path.is_file(), f"missing {script_path}")
    command = [
        sys.executable, "-I", str(script_path),
        "--root", str(paths["root"]),
        "--assets", str(paths["assets"]),
        "--tag", f'v{spec["version"]}',
        "--version", spec["version"],
        "--target", spec["target_commit"],
        "--parent", spec["parent_commit"],
        "--repo", str(spec["repo"]),
    ]
    if spec.get("expected_master"):
        command += ["--expected-master", spec["expected_master"]]
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", timeout=900)
    require(result.returncode == 0, "master manifest sync failed: " + result.stdout[-2000:] + result.stderr[-2000:])
    return json.loads(result.stdout) if result.stdout.strip().startswith("{") else {"synced": True}


LOCAL_PHASES = ("preflight", "mirror_probe", "build", "stage", "package")
PUBLISH_PHASES = ("publish", "master_sync")
PHASES = LOCAL_PHASES + PUBLISH_PHASES


def validate_resume_state(state):
    completed = state.get("completed", [])
    require(completed == list(PHASES[:len(completed)]), "release state uses an incompatible phase order; start a new build with mirror_probe")
    require(all(phase in state.get("phases", {}) for phase in completed), "release state is missing completed phase evidence")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("run",))
    parser.add_argument("--spec", required=True)
    parser.add_argument("--publish", action="store_true", help="create and publish the GitHub release after local verification")
    parser.add_argument("--dry-run", action="store_true", help="preflight only; no build, no secrets, no writes")
    parser.add_argument("--resume", action="store_true", help="reuse an existing state file and skip completed phases")
    args = parser.parse_args()

    spec_path = Path(args.spec)
    spec = json.loads(spec_path.read_text(encoding="utf-8-sig"))
    for key in REQUIRED_SPEC_KEYS:
        require(key in spec, f"spec missing key: {key}")
    for key, default in OPTIONAL_SPEC_DEFAULTS.items():
        spec.setdefault(key, default)
    publish = args.publish or spec.get("publish")

    version_suffix = spec["version"].replace(".", "")  # 1.7.8.60 -> 17860（沿用 v17859-stage 命名惯例）
    root = Path(spec["work_root"]) / f"release-{version_suffix}"
    paths = {
        "root": root,
        "stage": Path(spec["work_root"]) / f"v{version_suffix}-stage",
        "assets": Path(spec["work_root"]) / f"v{version_suffix}-assets",
        "full_zip": Path(spec["work_root"]) / f"TClient-v{spec['version']}-full-win64.zip",
        "stage_inventory": root / "stage-inventory.json",
    }
    root.mkdir(parents=True, exist_ok=True)

    state_path = root / "orchestrator-state.json"
    state = json.loads(state_path.read_text(encoding="utf-8-sig")) if args.resume and state_path.exists() else {}

    completed = set(state.get("completed", []))
    plan = list(LOCAL_PHASES) + (list(PUBLISH_PHASES) if publish else [])

    if args.dry_run:
        phase_preflight(spec, paths, state)
        print(json.dumps({"dry_run": True, "phases_planned": plan}, indent=2))
        return 0

    if args.resume:
        validate_resume_state(state)
        if "mirror_probe" in completed:
            verify_mirror_inputs(spec, paths, state, fresh=False)

    for phase in plan:
        if args.resume and phase in completed:
            print(f"[skip] {phase}")
            continue
        print(f"[phase] {phase}", flush=True)
        result = globals()[f"phase_{phase}"](spec, paths, state)
        state.setdefault("phases", {})[phase] = result
        state.setdefault("completed", []).append(phase)
        state_path.write_text(json.dumps(state, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print(json.dumps(state.get("phases", {}), ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as error:
        print("RELEASE FAILED:", error, file=sys.stderr)
        sys.exit(1)
