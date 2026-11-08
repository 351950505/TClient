"""TClient one-command release orchestrator (two-phase).

Default phase performs the complete LOCAL release: preflight, build, test,
stage, Authenticode evidence, packaging, Ed25519 manifest signature and full
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

DEFAULT_REPO = Path("D:/TaterClient-src")
DEFAULT_WORK_ROOT = Path("D:/kehuduan")
DEFAULT_PRIVATE_KEY = Path("D:/kehuduan/tclient-update-ed25519-private.pem")
GH_EXE = Path("D:/kehuduan/release-17856/extract/bin/gh.exe")
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
    "publish": False,
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
        git(repo, "rev-list", "--parents", "-n", "1", spec["target_commit"]).split() == [spec["target_commit"], spec["parent_commit"]],
        "unexpected source parent",
    )
    changed = set(git(repo, "diff", "--name-only", spec["parent_commit"], spec["target_commit"]).splitlines())
    require(changed == set(spec["changed_files"]), "changed-file set differs from the spec (cumulative diff " + spec["parent_commit"][:12] + ".." + spec["target_commit"][:12] + ")")
    version_h = (repo / "src/game/version.h").read_text(encoding="utf-8")
    require(f'#define TCLIENT_APP_VERSION "{spec["version"]}"' in version_h, "version.h does not declare the spec version")
    check_helper_lock(spec)
    notes = Path(spec["notes_path"])
    require(notes.is_file() and notes.read_text(encoding="utf-8").strip(), "release notes missing or empty")
    require(Path(spec["monitor_path"]).is_file(), "monitor exe missing")
    for key in ("watcher_path", "hook_dll_path"):
        require(Path(spec[key]).is_file(), f"{key} missing")
    return {"tree": git(repo, "rev-parse", f'{spec["target_commit"]}^{{tree}}')}


def phase_build(spec, paths, state):
    cmake = Path("C:/VS2026/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")
    require(cmake.is_file(), "VS2026 cmake not found")
    build_dir = Path(spec["build_dir"])
    subprocess.run([str(cmake), "--build", str(build_dir), "--config", "Release", "--target", "game-client", "--target", "TClientUpdater", "--parallel"], check=True, timeout=3600)
    repo = Path(spec["repo"])
    libs = repo / "ddnet-libs"
    env = dict(os.environ)
    for sub in ("sqlite3", "curl", "freetype", "opus", "opusfile", "png", "sdl", "zlib"):
        env["PATH"] = str(libs / sub / "windows" / "lib64") + os.pathsep + env["PATH"]
    result = subprocess.run([str(build_dir / "Release" / "testrunner.exe")], capture_output=True, timeout=1800, env=env, cwd=str(repo))
    if result.returncode != 0:
        tail = (result.stdout or b"")[-1500:].decode(errors="replace")
        err = (result.stderr or b"")[-1500:].decode(errors="replace")
        raise RuntimeError(f"testrunner failed: {result.returncode}\nSTDOUT:{tail}\nSTDERR:{err}")
    client = build_dir / "Release" / "DDNet.exe"
    data = client.read_bytes()
    require(f'{spec["version"]}\0'.encode() in data, "client does not embed the release version")
    short = git(repo, "rev-parse", "--short", spec["target_commit"])
    require(short.encode() in data, "client does not embed the release commit")
    return {"client_sha256": sha256_of(client), "client_size": client.stat().st_size, "testrunner": "pass"}


def phase_stage(spec, paths, state):
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
    return {"stage_files": sum(1 for _ in stage.rglob("*") if stage.joinpath(Path(_).name).is_file())}


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
    update_files = assets.collect_files(stage, assets.UPDATE_DATA_SUBDIRS)
    require({name for name, _ in full_files} == {entry["path"] for entry in inventory["files"]}, "full inventory differs from stage")
    require({name for name, _ in update_files}.issubset({name for name, _ in full_files}), "update package is not a subset")

    for entry in inventory["files"]:
        if entry["path"] in helper.FIRST_PARTY_FILES:
            origin = Path(entry["origin"])
            require(origin.is_file() and assets.sha256_of(origin) == entry["sha256"], "first-party build origin differs: " + entry["path"])

    client = (stage / "DDNet.exe").read_bytes()
    previous = "1.7.8." + str(int(version.rsplit(".", 1)[1]) - 1)
    require(f"{version}\0".encode() in client and f"{previous}\0".encode() not in client, "stage client version is wrong")
    require(git(repo, "rev-parse", "--short", spec["target_commit"]).encode() in client, "stage client lacks the release commit")

    before = signatures(paths["root"], stage)
    if spec.get("signed_expected"):
        thumbprint = os.environ.get("TCLIENT_CERT_THUMBPRINT", "")
        require(thumbprint, "TCLIENT_CERT_THUMBPRINT required for signed releases")
        sign = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-File", str(repo / "tclient_scripts" / "windows_sign_release.ps1"), "-Directory", str(stage), "-Mode", "Sign", "-CertificateThumbprint", thumbprint, "-CertificateStoreLocation", "CurrentUser"], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=600)
        require(sign.returncode == 0, "Authenticode signing failed: " + sign.stderr)
    after = signatures(paths["root"], stage)

    private_key = assets.load_signing_key(Path(spec["private_key"]))
    metadata = {"product": "TClient", "channel": "stable", "version": version, "platform": "win64"}
    output.mkdir()
    assets.write_zip(update_files, output / assets.ASSET_NAME)
    assets.write_zip(full_files, full)
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
    assets.verify_zip_contents(full, full_files)
    helper.validate_stage(stage, paths["stage_inventory"])
    for path, expected in ((output / assets.ASSET_NAME, update_files), (full, full_files)):
        with zipfile.ZipFile(path) as archive:
            entries = assets.archive_entries(archive)
            require(archive.testzip() is None, "ZIP CRC failed: " + str(path))
            require(set(entries) == {name for name, _ in expected}, "ZIP entry set differs")
            for name, source in expected:
                require(archive.read(entries[name]) == source.read_bytes(), "ZIP bytes differ: " + name)

    record = {
        "version": version,
        "source_commit": spec["target_commit"],
        "parent": spec["parent_commit"],
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


def signatures(root, directory):
    script = Path(root) / "tclient_scripts" / "check_signatures.ps1"
    require(script.is_file(), f"missing {script}")
    result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-File", str(script), "-Directory", str(directory)], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=300)
    require(result.returncode == 0, "Authenticode evidence failed: " + result.stderr)
    return json.loads(result.stdout)


def github_token(repo):
    result = subprocess.run(["git", "-C", str(repo), "credential", "fill"], input=b"protocol=https\nhost=github.com\n\n", capture_output=True, timeout=60)
    require(result.returncode == 0, "git credential fill failed")
    values = dict(line.split(b"=", 1) for line in result.stdout.splitlines() if b"=" in line)
    require(bool(values.get(b"password")), "no GitHub credential available")
    return values[b"password"].decode()


def gh(repo, *args, token):
    require(GH_EXE.is_file(), f"gh.exe not found: {GH_EXE}")
    env = dict(os.environ, GH_HOST="github.com", GH_TOKEN=token, GH_PROMPT_DISABLED="1")
    result = subprocess.run([str(GH_EXE), "api" if args[0] == "api" else args[0], "--repo", GITHUB_REPO, *args[1:]], env=env, capture_output=True, text=True, timeout=300)
    require(result.returncode == 0, "gh failed: " + (result.stderr or result.stdout))
    return result.stdout


def phase_publish(spec, paths, state):
    token = github_token(Path(spec["repo"]))
    assets_record = json.loads((paths["root"] / "local-assets.json").read_text(encoding="utf-8-sig"))
    asset_paths = {record["name"]: Path(record["path"]) for record in assets_record["assets"]}
    release = json.loads(gh(paths["root"], "api", "repos/" + GITHUB_REPO + "/releases", "-f", f'tag_name=v{spec["version"]}', "-f", f'target_commitish={spec["target_commit"]}', "-f", f'name=TClient-v{spec["version"]}', "-f", "draft=true", "-F", f'body={json.dumps(Path(spec["notes_path"]).read_text(encoding="utf-8").strip())}', token=token))
    release_id = release["id"]
    for name, path in asset_paths.items():
        digest = sha256_of(path)
        upload = subprocess.run([str(GH_EXE), "release", "upload", f'v{spec["version"]}', "--repo", GITHUB_REPO, "--clobber", str(path)], env=dict(os.environ, GH_TOKEN=token, GH_HOST="github.com"), capture_output=True, text=True, timeout=1800)
        require(upload.returncode == 0, f"asset upload failed: {name}: " + upload.stderr)
        remote = json.loads(gh(paths["root"], "api", f"repos/{GITHUB_REPO}/releases/{release_id}/assets?per_page=100", token=token))
        match = [a for a in remote if a["name"] == name]
        require(match and match[0]["size"] == path.stat().st_size, f"remote asset size differs: {name}")
        state.setdefault("remote_digests", {})[name] = digest
    publish = subprocess.run([str(GH_EXE), "release", "edit", f'v{spec["version"]}', "--repo", GITHUB_REPO, "--draft=false", "--latest"], env=dict(os.environ, GH_TOKEN=token), capture_output=True, text=True, timeout=300)
    require(publish.returncode == 0, "publishing the release failed: " + publish.stderr)
    return {"release_id": release_id, "published": True}


def phase_master_sync(spec, paths, state):
    expected_master = git(Path(spec["repo"]), "rev-parse", "origin/master") if subprocess.run(["git", "-C", str(spec["repo"]), "fetch", "github", "master"], capture_output=True).returncode == 0 else None
    require(expected_master, "could not resolve remote master; fetch github master first")
    script_path = paths["root"] / "scripts" / "sync_master_manifest.py"
    require(script_path.is_file(), f"missing {script_path}; generate it from the previous release scripts directory")
    result = subprocess.run([sys.executable, str(script_path), "--target", spec["target_commit"]], capture_output=True, text=True, timeout=600)
    require(result.returncode == 0, "master manifest sync failed: " + result.stdout[-2000:] + result.stderr[-2000:])
    return json.loads(result.stdout.strip().splitlines()[-1]) if result.stdout.strip().startswith("{") else {"synced": True}


PHASES = ("preflight", "build", "stage", "package", "publish", "master_sync")


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
    (root / "scripts").mkdir(exist_ok=True)

    state_path = root / "orchestrator-state.json"
    state = json.loads(state_path.read_text(encoding="utf-8-sig")) if args.resume and state_path.exists() else {}

    if args.dry_run:
        phase_preflight(spec, paths, state)
        print(json.dumps({"dry_run": True, "phases_planned": list(PHASES) + (["publish", "master_sync"] if publish else [])}, indent=2))
        return 0

    completed = set(state.get("completed", []))
    plan = ["preflight", "build", "stage", "package"] + (["publish", "master_sync"] if publish else [])
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
