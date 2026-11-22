#!/usr/bin/env python3
"""Probe the live catalog using signed, complete update assets; Python 3.11+.

Run with python -I (cryptography is required). Downloads stay in disposable,
per-request directories outside this script's directory and are never executed.
"""
import argparse
import base64
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
from urllib.parse import urljoin, urlsplit

CATALOG_URL = "https://github.akams.cn/"
METADATA_URL = "https://api.github.com/repos/351950505/TClient/releases/latest"
GITHUB = "https://github.com/351950505/TClient/releases/"
MANIFEST = "TClient-update-win64.json"
PACKAGE = "TClient-update-win64.zip"
HELPER = "TClientUpdater.exe"
RAW = "https://raw.githubusercontent.com/351950505/TClient/master/update/" + MANIFEST
FALLBACKS = ("https://github1.me3.dpdns.org/", "https://github2.me3.dpdns.org/", "https://ghfast.top/")
LIMITS = {MANIFEST: 65536, MANIFEST + ".sig": 64, PACKAGE: 512 * 1024**2, HELPER: 32 * 1024**2}
VERSION = r"[0-9]+(?:\.[0-9]+){1,5}"

# Sent to the regional SSH host on stdin. Downloaded update assets are written
# only to a fresh temporary directory, verified by size/hash/range, and never run.
REMOTE_REGION_PROBE = r'''import base64, hashlib, json, re, shutil, subprocess, sys, tempfile, time
from pathlib import Path
from urllib.parse import urlsplit

Config = json.loads(base64.b64decode("__PAYLOAD__").decode("utf-8"))
Curl = shutil.which("curl")
if not Curl:
    raise RuntimeError("curl is required on regional probe host")


def Fetch(Url, Limit, OutPath, ByteRange=None, Resume=False):
    if urlsplit(Url).scheme != "https":
        raise RuntimeError("HTTPS required")
    OutPath = Path(OutPath)
    HeaderPath = Path(str(OutPath) + ".headers")
    Command = [Curl, "-q", "--ipv4", "--proto", "=https", "--proto-redir", "=https",
               "--tlsv1.2", "--location", "--max-redirs", "4", "--connect-timeout", "5",
               "--max-time", "30", "--max-filesize", str(Limit), "--retry", "0",
               "--silent", "--show-error", "--fail", "--user-agent", Config["user_agent"],
               "--header", "Cache-Control: no-cache", "--header", "Accept-Encoding: identity",
               "--dump-header", str(HeaderPath), "--output", str(OutPath),
               "--write-out", "%{http_code}\\n%{url_effective}\\n%{time_total}"]
    if Resume:
        Command += ["-C", "-"]
    if ByteRange is not None:
        Command += ["--range", ByteRange]
    Command += ["--url", Url]
    Started = time.monotonic()
    Result = subprocess.run(Command, capture_output=True, timeout=37)
    if Result.returncode:
        raise RuntimeError("curl exit %d: %s" % (Result.returncode, Result.stderr.decode("utf-8", "replace")[-200:]))
    Output = Result.stdout.decode("utf-8", "replace").splitlines()
    if len(Output) != 3 or urlsplit(Output[1]).scheme != "https":
        raise RuntimeError("invalid curl response or non-HTTPS redirect")
    Blocks = re.split(r"(?m)^HTTP/\S+\s+\d{3}[^\r\n]*\r?\n", HeaderPath.read_text(encoding="latin1"))
    Fields = [Line.split(":", 1) for Line in Blocks[-1].splitlines() if ":" in Line]
    if len({Key.lower() for Key, _ in Fields}) != len(Fields):
        raise RuntimeError("duplicate response headers")
    Response = {"path": str(OutPath), "headers": {Key.lower(): Value.strip() for Key, Value in Fields},
                "status": int(Output[0]), "size": OutPath.stat().st_size,
                "seconds": max(time.monotonic() - Started, 0.000001)}
    if Response["size"] > Limit or Response["headers"].get("content-encoding", "identity").lower() != "identity":
        raise RuntimeError("response exceeds size limit or uses content encoding")
    if Response["status"] not in (200, 206) or (not Resume and ByteRange is None and Response["status"] != 200):
        raise RuntimeError("unexpected HTTP status")
    return Response


def Digest(PathName):
    Hash = hashlib.sha256()
    with open(PathName, "rb") as File:
        for Block in iter(lambda: File.read(1024 * 1024), b""):
            Hash.update(Block)
    return Hash.hexdigest()


def Verify(Response, Info):
    if Response["size"] != Info["size"] or Digest(Response["path"]) != Info["sha256"]:
        raise RuntimeError("download size/SHA256 mismatch")


def CheckRange(Response, Start, End, Total):
    if (Response["status"] != 206 or Response["headers"].get("content-range") !=
            "bytes %d-%d/%d" % (Start, End, Total) or Response["size"] != End + 1):
        raise RuntimeError("invalid Range 206/Content-Range or truncated body")


def Probe(Candidate):
    Result = {"prefix": Candidate["prefix"], "package_ok": False}
    try:
        with tempfile.TemporaryDirectory(prefix="tclient-mirror-probe-") as Temporary:
            Base = Candidate["prefix"] + Config["github_prefix"]
            Manifest = Fetch(Base + "latest/download/" + Config["manifest"], 65536, Path(Temporary) / "manifest")
            Signature = Fetch(Base + "latest/download/" + Config["signature"], 64, Path(Temporary) / "signature")
            if Digest(Manifest["path"]) != Config["manifest_sha256"] or Digest(Signature["path"]) != Config["signature_sha256"]:
                raise RuntimeError("manifest/signature differs from locally authenticated pair")
            Reference = Config["reference"]
            DownloadBase = Base + "download/" + Reference["tag"] + "/"
            Size, Offset = Reference["size"], 1024 * 1024
            Full = Fetch(DownloadBase + Reference["asset"], Size, Path(Temporary) / "full.zip")
            Verify(Full, Reference)
            Helper = Fetch(DownloadBase + Reference["helper"]["name"], Reference["helper"]["size"], Path(Temporary) / "helper.exe")
            Verify(Helper, Reference["helper"])
            Partial = Fetch(DownloadBase + Reference["asset"], Offset, Path(Temporary) / "part.zip", ByteRange="0-%d" % (Offset - 1))
            CheckRange(Partial, 0, Offset - 1, Size)
            ResumedPath = Path(Temporary) / "resumed.zip"
            shutil.copyfile(Partial["path"], ResumedPath)
            Resumed = Fetch(DownloadBase + Reference["asset"], Size, ResumedPath, Resume=True)
            CheckRange(Resumed, Offset, Size - 1, Size)
            Verify(Resumed, Reference)
            Result.update(package_ok=True, full_seconds=Full["seconds"],
                          full_bytes_per_second=Size / Full["seconds"],
                          resume_seconds=Resumed["seconds"],
                          resume_bytes_per_second=(Size - Offset) / Resumed["seconds"],
                          combined_bytes_per_second=(2 * Size - Offset) / (Full["seconds"] + Resumed["seconds"]),
                          range_verified=True, sha256_verified=True, helper_verified=True,
                          **({"edge": "cloudflare", "edge_code": Full["headers"].get("cf-ray", "").rsplit("-", 1)[-1]} if Full["headers"].get("server", "").lower() == "cloudflare" else {"edge": Full["headers"].get("server", "unknown").lower() or "unknown", "edge_code": ""}))
    except Exception as Error:
        Result["error"] = str(Error)[:300]
    return Result


Results = [Probe(Candidate) for Candidate in Config["candidates"]]
print(json.dumps(Results, separators=(",", ":")))
'''


class ProbeError(Exception):
    pass


def normalize_domain(value):
    if not isinstance(value, str) or len(value) > 253 or not value.isascii():
        raise ProbeError("invalid catalog domain")
    domain = value.lower()
    if not re.fullmatch(r"(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z][a-z0-9-]{0,61}[a-z0-9]", domain):
        raise ProbeError("invalid catalog domain")
    if domain.endswith((".localhost", ".localdomain", ".local", ".internal", ".lan", ".home", ".arpa", ".test", ".invalid", ".example", ".onion")):
        raise ProbeError("private/reserved catalog domain")
    return domain


def parse_catalog(script):
    match = re.search(r'''\b(?:let|const|var)\s+j\s*=\s*\[((?:"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[^\[\]"'])*)\]''', script)
    if not match:
        raise ProbeError("catalog format changed: missing literal j array")
    objects = re.findall(r"\{[^{}]*\}", match[1])
    if not objects or len(objects) > 200 or re.sub(r"\{[^{}]*\}|[\s,]", "", match[1]):
        raise ProbeError("catalog format changed or exceeds 200 entries")
    domains, rejected = [], []
    for obj in objects:
        value = re.search(r'''(?:\bvalue|["']value["'])\s*:\s*(["'])([^"'\\]*)\1''', obj)
        if not value or not re.search(r'''(?:\blabel|["']label["'])\s*:''', obj):
            raise ProbeError("catalog format changed: expected literal label/value entries")
        try:
            domain = normalize_domain(value[2])
            if domain not in domains:
                domains.append(domain)
        except ProbeError as error:
            rejected.append({"value": value[2], "error": str(error)})
    if not domains:
        raise ProbeError("catalog contains no valid public domains")
    return domains, rejected


def unique_object(pairs):
    result = {}
    for name, value in pairs:
        if name in result:
            raise ProbeError(f"duplicate JSON field: {name}")
        result[name] = value
    return result


def read_json(data):
    try:
        return json.loads(data.decode("utf-8"), object_pairs_hook=unique_object)
    except (ValueError, UnicodeError) as error:
        raise ProbeError("invalid UTF-8 JSON") from error


def repo_settings(repo):
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    text = (repo / "src/game/version.h").read_text(encoding="utf-8")
    name = re.search(r'^\s*#define\s+GAME_NAME\s+"([^"\r\n]+)"', text, re.M)
    version = re.search(r'^\s*#define\s+GAME_RELEASE_VERSION\s+"([0-9.]+)"', text, re.M)
    if not version and re.search(r"#define\s+GAME_RELEASE_VERSION\s+STRINGIFY\(GAME_RELEASE_VERSION_INTERNAL\)", text):
        version = re.search(r"^\s*#define\s+GAME_RELEASE_VERSION_INTERNAL\s+([0-9.]+)\s*$", text, re.M)
    if not name or not version or not re.fullmatch(VERSION, version[1]):
        raise ProbeError("cannot read GAME_NAME/GAME_RELEASE_VERSION")
    header = (repo / "src/game/client/components/tclient/update_public_key.h").read_text(encoding="utf-8")
    match = re.search(r"s_aUpdateEd25519PublicKey\[32\]\s*=\s*\{([^}]+)\}", header)
    key = bytes(int(v, 16) for v in re.findall(r"0x([a-fA-F0-9]{2})\b", match[1])) if match else b""
    if len(key) != 32:
        raise ProbeError("invalid embedded Ed25519 public key")
    return f"{name[1]} {version[1]} (windows; amd64)", Ed25519PublicKey.from_public_bytes(key)


def validate_pair(pair, public_key, tag, assets):
    from cryptography.exceptions import InvalidSignature
    body, signature = pair
    if not 0 < len(body) <= LIMITS[MANIFEST] or len(signature) != 64:
        raise ProbeError("invalid manifest/signature length")
    try:
        public_key.verify(signature, body)
    except InvalidSignature as error:
        raise ProbeError("manifest signature verification failed") from error
    manifest = read_json(body)
    expected = {"product": "TClient", "channel": "stable", "platform": "win64", "version": tag[1:], "asset": PACKAGE}
    if not isinstance(manifest, dict):
        raise ProbeError("manifest is not an object")
    for field, value in expected.items():
        if manifest.get(field) != value:
            raise ProbeError(f"manifest {field} mismatch")
    helper = manifest.get("helper")
    if not isinstance(helper, dict) or helper.get("name") != HELPER:
        raise ProbeError("manifest helper name mismatch")
    for name, info, minimum in [(PACKAGE, manifest, 1024 * 1024 + 1), (HELPER, helper, 1024)]:
        if type(info.get("size")) is not int or not minimum <= info["size"] <= LIMITS[name]:
            raise ProbeError(f"unreasonable {name} size")
        if not isinstance(info.get("sha256"), str) or not re.fullmatch(r"[a-f0-9]{64}", info["sha256"]):
            raise ProbeError(f"invalid {name} SHA256")
    for name, data in [(MANIFEST, body), (MANIFEST + ".sig", signature), (PACKAGE, manifest), (HELPER, helper)]:
        size = len(data) if isinstance(data, bytes) else data["size"]
        digest = hashlib.sha256(data).hexdigest() if isinstance(data, bytes) else data["sha256"]
        if assets[name].get("size") != size:
            raise ProbeError(f"official asset size mismatch: {name}")
        official_digest = assets[name].get("digest")
        if official_digest is not None and official_digest != "sha256:" + digest:
            raise ProbeError(f"official asset digest mismatch: {name}")
    return manifest


def check_range(response, start, end, total):
    if (response["status"] != 206 or response["headers"].get("content-range") != f"bytes {start}-{end}/{total}"
            or response["size"] != end + 1):
        raise ProbeError("invalid Range 206/Content-Range or truncated body")


class Curl:
    def __init__(self, user_agent):
        self.executable = shutil.which("curl.exe")
        if not self.executable:
            raise ProbeError("curl.exe is required")
        self.user_agent = user_agent

    @contextmanager
    def fetch(self, url, limit, seconds=8, seed=None, byte_range=None):
        if urlsplit(url).scheme != "https":
            raise ProbeError("HTTPS is required")
        with tempfile.TemporaryDirectory(prefix="tclient-mirror-request-") as temporary:
            directory = Path(temporary).resolve()
            if Path(__file__).resolve().parent in directory.parents:
                raise ProbeError("download temporary directory must be outside script directory")
            path, headers = directory / "body", directory / "headers"
            if seed is not None:
                shutil.copyfile(seed, path)
            command = [self.executable, "-q", "--ipv4", "--proto", "=https", "--proto-redir", "=https",
                       "--tlsv1.2", "--location", "--max-redirs", "4", "--connect-timeout", "5",
                       "--max-time", str(seconds), "--max-filesize", str(limit), "--retry", "0",
                       "--silent", "--show-error", "--fail", "--user-agent", self.user_agent,
                       "--header", "Cache-Control: no-cache", "--header", "Accept-Encoding: identity",
                       "--dump-header", str(headers), "--output", str(path), "--write-out", "%{http_code}\n%{url_effective}"]
            if seed is not None:
                command += ["-C", "-"]
            if byte_range is not None:
                command += ["--range", byte_range]
            command += ["--url", url]
            started = time.monotonic()
            try:
                result = subprocess.run(command, shell=False, capture_output=True, timeout=seconds + 2)
            except (OSError, subprocess.TimeoutExpired) as error:
                raise ProbeError(f"curl request failed: {error}") from error
            if result.returncode:
                raise ProbeError(f"curl exit {result.returncode}: {result.stderr.decode('utf-8', 'replace')[-300:]}")
            output = result.stdout.decode("utf-8", "replace").splitlines()
            if len(output) != 2 or urlsplit(output[1]).scheme != "https":
                raise ProbeError("invalid curl response or non-HTTPS redirect")
            blocks = re.split(r"(?m)^HTTP/\S+\s+\d{3}[^\r\n]*\r?\n", headers.read_text(encoding="latin1"))
            fields = [line.split(":", 1) for line in blocks[-1].splitlines() if ":" in line]
            if len({k.lower() for k, _ in fields}) != len(fields):
                raise ProbeError("duplicate response headers")
            response = {"path": path, "headers": {k.lower(): v.strip() for k, v in fields},
                        "status": int(output[0]), "size": path.stat().st_size,
                        "seconds": max(time.monotonic() - started, 0.000001)}
            if response["size"] > limit or response["headers"].get("content-encoding", "identity").lower() != "identity":
                raise ProbeError("response exceeds size limit or uses content encoding")
            if response["status"] not in (200, 206):
                raise ProbeError(f"unexpected HTTP {response['status']}")
            if seed is None and byte_range is None and response["status"] != 200:
                raise ProbeError("unexpected partial response")
            yield response

    def read(self, url, limit, seconds=8):
        with self.fetch(url, limit, seconds) as response:
            return response["path"].read_bytes()


def load_catalog(curl):
    html = curl.read(CATALOG_URL, 2 * 1024**2).decode("utf-8")
    chunks = list(dict.fromkeys(re.findall(r'<script\b[^>]*\bsrc=["\'](/_next/static/chunks/[A-Za-z0-9_./-]+\.js)["\']', html)))
    if not chunks or len(chunks) > 32 or any(".." in chunk for chunk in chunks):
        raise ProbeError("catalog format changed: no safe Next.js chunks")
    scripts = []
    with ThreadPoolExecutor(max_workers=6) as pool:
        for script in pool.map(lambda chunk: curl.read(urljoin(CATALOG_URL, chunk), 2 * 1024**2).decode("utf-8"), chunks):
            if re.search(r"\b(?:let|const|var)\s+j\s*=\s*\[", script):
                scripts.append(script)
    if len(scripts) != 1:
        raise ProbeError("catalog format changed: expected one directory array")
    return parse_catalog(scripts[0])


def load_metadata(curl):
    metadata = read_json(curl.read(METADATA_URL, 2 * 1024**2))
    if (not isinstance(metadata, dict) or not re.fullmatch("v" + VERSION, str(metadata.get("tag_name", "")))
            or metadata.get("draft") is not False or metadata.get("prerelease") is not False):
        raise ProbeError("official latest release metadata is invalid")
    tag, assets = metadata["tag_name"], {}
    for asset in metadata.get("assets", []):
        name = asset.get("name")
        if name in LIMITS:
            if name in assets or type(asset.get("size")) is not int or not 0 < asset["size"] <= LIMITS[name]:
                raise ProbeError(f"invalid/duplicate official asset: {name}")
            if asset.get("browser_download_url") != GITHUB + "download/" + tag + "/" + name:
                raise ProbeError(f"unexpected official asset URL: {name}")
            assets[name] = asset
    if set(assets) != set(LIMITS):
        raise ProbeError("official release lacks manifest/signature/package/helper assets")
    return tag, assets


def edge_info(response):
    """从响应头判定镜像的边缘类型。

    Cloudflare 前置的镜像在全球都有接入点，海外玩家走它也能拿到文件；
    区域自建镜像本机快，海外未必可达。测速只反映国内视角，因此把
    全球可达性作为选择时的配额维度，而不是排序维度。
    """
    headers = response["headers"]
    server = headers.get("server", "").lower()
    if server == "cloudflare":
        ray = headers.get("cf-ray", "")
        code = ray.rsplit("-", 1)[-1] if "-" in ray else ""
        return {"edge": "cloudflare", "edge_code": code}
    return {"edge": server or "unknown", "edge_code": ""}


def probe_pair(curl, prefix, tag, assets, key, latest=True):
    started = time.monotonic()
    row = {"prefix": prefix, "manifest_ok": False}
    try:
        base = prefix + GITHUB + ("latest/download/" if latest else "download/" + tag + "/")
        with curl.fetch(base + MANIFEST, LIMITS[MANIFEST]) as manifest_response:
            body = manifest_response["path"].read_bytes()
            row.update(**edge_info(manifest_response))
        signature = curl.read(base + MANIFEST + ".sig", 64)
        validate_pair((body, signature), key, tag, assets)
        row["manifest_ok"] = True
        row["manifest_seconds"] = time.monotonic() - started
        return row, (body, signature)
    except (ProbeError, OSError) as error:
        row["error"] = str(error)
        return row, None


def verify_file(response, info):
    with response["path"].open("rb") as file:
        digest = hashlib.file_digest(file, "sha256").hexdigest()
    if response["size"] != info["size"] or digest != info["sha256"]:
        raise ProbeError("download size/SHA256 mismatch")


def probe_package(curl, row, reference):
    result = {**row, "package_ok": False}
    base = row["prefix"] + GITHUB + "download/" + reference["tag"] + "/"
    try:
        size, offset = reference["size"], 1024 * 1024
        with curl.fetch(base + PACKAGE, size, 30) as full:
            verify_file(full, reference)
            result.update(full_seconds=full["seconds"], full_bytes_per_second=size / full["seconds"], **edge_info(full))
        with curl.fetch(base + HELPER, reference["helper"]["size"], 30) as helper:
            verify_file(helper, reference["helper"])
            result["helper_seconds"] = helper["seconds"]
        with curl.fetch(base + PACKAGE, offset, 30, byte_range=f"0-{offset - 1}") as partial:
            check_range(partial, 0, offset - 1, size)
            result["range_seconds"] = partial["seconds"]
            with curl.fetch(base + PACKAGE, size, 30, seed=partial["path"]) as resumed:
                check_range(resumed, offset, size - 1, size)
                verify_file(resumed, reference)
                result.update(resume_seconds=resumed["seconds"], resume_bytes_per_second=(size - offset) / resumed["seconds"],
                              resume_offset=offset, range_verified=True, sha256_verified=True, helper_verified=True)
        result["combined_bytes_per_second"] = (2 * size - offset) / (result["full_seconds"] + result["resume_seconds"])
        result["package_ok"] = True
    except (ProbeError, OSError) as error:
        result["error"] = str(error)
    return result


def multi_region_score(row, regions):
    """只给所有测速地区均通过完整性检查的镜像评分，使用最慢地区速度。"""
    if not row.get("package_ok"):
        return None
    speeds = [row.get("combined_bytes_per_second")]
    for region in regions:
        probe = row.get("regional_results", {}).get(region)
        if not probe or not probe.get("package_ok"):
            return None
        speeds.append(probe.get("combined_bytes_per_second"))
    if any(type(speed) not in (int, float) or speed <= 0 for speed in speeds):
        return None
    return min(speeds)


MIN_GLOBAL_SELECTED = 2


def select_mirrors(rows, max_selected, min_global=MIN_GLOBAL_SELECTED):
    """按所有已测区域中的最慢速度排名，并为 Cloudflare 镜像保留名额。

    每个镜像必须在本机和所有指定区域均通过完整包、helper、SHA256 与续传校验；
    用最慢区域的下载速度排序，避免单个地区的高速度掩盖其他地区的慢线路。
    未提供远端探测时兼容单点排名。海外玩家优先走官方直链，镜像列表仍保留
    全球型（Cloudflare）节点名额。
    """
    def speed(row):
        return row.get("selection_bytes_per_second", row.get("combined_bytes_per_second"))

    ranked = sorted((row for row in rows if speed(row) is not None), key=lambda row: (-speed(row), row["prefix"]))
    global_ranked = [row for row in ranked if row.get("edge") == "cloudflare"]
    if not global_ranked:
        return ranked[:max_selected]
    selected, seen = [], set()

    def take(row):
        if len(selected) < max_selected and row["prefix"] not in seen:
            selected.append(row)
            seen.add(row["prefix"])

    take(ranked[0])
    for row in global_ranked:
        if sum(1 for chosen in selected if chosen.get("edge") == "cloudflare") >= min_global:
            break
        take(row)
    for row in ranked:
        if len(selected) >= max_selected:
            break
        take(row)
    return sorted(selected, key=lambda row: (-speed(row), row["prefix"]))


def parse_probe_nodes(values):
    nodes, seen = [], set()
    for value in values:
        region, separator, host = value.partition("=")
        if not separator or not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,31}", region) or not re.fullmatch(r"[A-Za-z0-9_.-]+", host):
            raise ProbeError("regional probe must be REGION=SSH_ALIAS")
        if region in seen:
            raise ProbeError("duplicate regional probe: " + region)
        seen.add(region)
        nodes.append({"region": region, "host": host})
    return nodes


def probe_remote_region(node, candidates, user_agent, reference, canonical_pair):
    ssh = shutil.which("ssh.exe") or shutil.which("ssh")
    if not ssh:
        for candidate in ("C:/Program Files/Git/usr/bin/ssh.exe", "C:/Windows/System32/OpenSSH/ssh.exe"):
            if Path(candidate).is_file():
                ssh = candidate
                break
    if not ssh:
        raise ProbeError("ssh.exe is required for regional mirror probes")
    payload = {
        "candidates": [{"prefix": row["prefix"]} for row in candidates],
        "github_prefix": GITHUB,
        "manifest": MANIFEST,
        "signature": MANIFEST + ".sig",
        "manifest_sha256": hashlib.sha256(canonical_pair[0]).hexdigest(),
        "signature_sha256": hashlib.sha256(canonical_pair[1]).hexdigest(),
        "reference": reference,
        "user_agent": user_agent,
    }
    encoded = base64.b64encode(json.dumps(payload, separators=(",", ":")).encode("utf-8")).decode("ascii")
    program = REMOTE_REGION_PROBE.replace("__PAYLOAD__", encoded)
    try:
        result = subprocess.run([ssh, "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", node["host"], "python3 -"],
                                input=program, capture_output=True, text=True, encoding="utf-8", timeout=3600)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ProbeError(f'regional probe {node["region"]} failed: {error}') from error
    if result.returncode:
        raise ProbeError(f'regional probe {node["region"]} failed: {result.stderr[-500:]}')
    try:
        rows = json.loads(result.stdout)
    except ValueError as error:
        raise ProbeError(f'regional probe {node["region"]} returned invalid JSON') from error
    if not isinstance(rows, list) or len(rows) != len(candidates):
        raise ProbeError(f'regional probe {node["region"]} returned an incomplete result set')
    if {row.get("prefix") for row in rows} != {row["prefix"] for row in candidates}:
        raise ProbeError(f'regional probe {node["region"]} candidate set differs')
    return {row["prefix"]: row for row in rows}


def make_header(selected):
    prefixes = []
    for prefix in [*selected, *FALLBACKS]:
        parsed = urlsplit(prefix)
        safe = "https://" + normalize_domain(parsed.netloc) + "/"
        if prefix != safe:
            raise ProbeError("header prefix is not a normalized domain")
        if safe not in prefixes:
            prefixes.append(safe)
    manifests = [prefix + GITHUB + "latest/download/" + MANIFEST for prefix in [*prefixes, ""]] + [RAW]
    siblings = [*prefixes, "", "@api", None]
    arrays = []
    for name, values in [("TCLIENT_UPDATE_MANIFEST_SOURCES", manifests), ("TCLIENT_UPDATE_SOURCE_SIBLING_BASES", siblings)]:
        arrays.append("static const char *const " + name + "[] = {\n" +
                      "".join("\t" + ("nullptr" if value is None else json.dumps(value)) + ",\n" for value in values) + "};\n")
    return "#ifndef TCLIENT_UPDATE_MIRRORS_H\n#define TCLIENT_UPDATE_MIRRORS_H\n\n" + "\n".join(arrays) + "\n#endif\n"


def publish(output, header, report):
    header_bytes = make_header([row["prefix"] for row in report["selected"]]).encode("utf-8")
    report["header_sha256"] = hashlib.sha256(header_bytes).hexdigest()
    staged = []
    try:
        for destination, content in [(output, (json.dumps(report, indent=2) + "\n").encode()), (header, header_bytes)]:
            destination.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(dir=destination.parent, prefix=destination.name + ".", delete=False) as file:
                staged.append((Path(file.name), destination))
                file.write(content)
                file.flush()
                os.fsync(file.fileno())
        for temporary, destination in staged:
            os.replace(temporary, destination)
    finally:
        for temporary, _ in staged:
            temporary.unlink(missing_ok=True)


def run(args):
    user_agent, key = repo_settings(args.repo)
    curl = Curl(user_agent)
    domains, rejected = load_catalog(curl)
    tag, assets = load_metadata(curl)
    print(f"catalog: {len(domains)} domains; official reference: {tag}", flush=True)
    canonical_results = []
    with ThreadPoolExecutor(max_workers=6) as pool:
        official = list(pool.map(lambda prefix: probe_pair(curl, prefix, tag, assets, key, False), ["", *FALLBACKS]))
        probed = list(pool.map(lambda domain: probe_pair(curl, "https://" + domain + "/", tag, assets, key), domains))
    canonical = next((pair for _, pair in [*official, *probed] if pair is not None), None)
    canonical_results = [row for row, _ in official]
    if canonical is None:
        raise ProbeError("no authenticated canonical pair matching official metadata; refusing old header")
    manifest = validate_pair(canonical, key, tag, assets)
    reference = {"tag": tag, "version": manifest["version"], "asset": PACKAGE, "sha256": manifest["sha256"],
                 "size": manifest["size"], "helper": manifest["helper"]}
    results = []
    for row, pair in probed:
        if pair is not None and pair != canonical:
            row.update(manifest_ok=False, error="stale/different signed manifest pair")
        results.append(row)
    candidates = sorted((row for row in results if row["manifest_ok"]), key=lambda row: (row["manifest_seconds"], row["prefix"]))[:args.max_candidates]
    print(f"authenticated manifests: {sum(row['manifest_ok'] for row in results)}; complete package candidates: {len(candidates)}", flush=True)
    with ThreadPoolExecutor(max_workers=3) as pool:
        for future in as_completed([pool.submit(probe_package, curl, row, reference) for row in candidates]):
            row = future.result()
            results[domains.index(urlsplit(row["prefix"]).netloc)] = row
            print(f"{row['prefix']}: {'verified' if row['package_ok'] else row['error']}", flush=True)

    Regions = [node["region"] for node in args.probe_nodes]
    for node in args.probe_nodes:
        Regional = probe_remote_region(node, candidates, user_agent, reference, canonical)
        for row in candidates:
            RegionalResult = Regional[row["prefix"]]
            row.setdefault("regional_results", {})[node["region"]] = RegionalResult
            Speed = RegionalResult.get("combined_bytes_per_second")
            Status = f"verified {Speed / 1024**2:.1f} MB/s" if RegionalResult.get("package_ok") else RegionalResult.get("error", "failed")
            print(f"{node['region']} {row['prefix']}: {Status}", flush=True)

    for row in results:
        row["selection_bytes_per_second"] = multi_region_score(row, Regions)
    Qualified = [row for row in results if row.get("selection_bytes_per_second") is not None]
    selected = select_mirrors(Qualified, args.max_selected)
    if not selected:
        raise ProbeError("no mirror passed full package/helper/SHA256/real resume verification in every probe region; refusing old header")
    RegionsMeasured = ["local", *Regions]
    Ranking = "minimum combined throughput across probe regions; local-only fallback" if Regions else "(full_bytes + resumed_tail_bytes) / (full_seconds + resume_seconds)"
    report = {"schema": 1, "catalog_url": CATALOG_URL, "fetched_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
              "release_version": args.release_version, "source_commit": args.source_commit, "reference": reference,
              "selected": selected, "user_agent": user_agent, "catalog_count": len(domains), "catalog_rejected": rejected,
              "canonical_results": canonical_results, "candidates": [row["prefix"] for row in candidates], "results": results,
              "probe_regions": RegionsMeasured,
              "test_scope": {"manifest_concurrency": 6, "package_concurrency": 3, "max_candidates": args.max_candidates,
                            "max_selected": args.max_selected, "request_timeout_seconds": 30, "range_offset": 1048576,
                            "checks": ["ed25519", "official_metadata", "canonical_pair", "full_sha256_size", "helper_sha256_size", "range_206_exact", "resumed_sha256_size"],
                            "ranking": Ranking,
                            "selection_policy": {"order": "worst-region speed, ties by prefix" if Regions else "speed, ties by prefix",
                                                 "measured_regions": RegionsMeasured,
                                                 "global_edge_quota": f"cloudflare >= {min(MIN_GLOBAL_SELECTED, args.max_selected)} when available"},
                            "transient_failures": "this run only"}}
    publish(args.output, args.header, report)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("repo", "output", "header"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--release-version", required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--max-candidates", type=int, default=12)
    parser.add_argument("--max-selected", type=int, default=5)
    parser.add_argument("--probe-node", action="append", default=[], metavar="REGION=SSH_ALIAS",
                        help="run the full package/helper/Range/SHA256 probe from an additional region via SSH")
    args = parser.parse_args(argv)
    if (not 1 <= args.max_candidates <= 12 or not 1 <= args.max_selected <= 5 or len(args.probe_node) > 4
            or not re.fullmatch(VERSION, args.release_version) or not re.fullmatch(r"[a-fA-F0-9]{40}", args.source_commit)):
        parser.error("require candidates=1..12, selected=1..5, probe nodes=0..4, numeric dotted version and full 40-hex commit")
    args.repo, args.output, args.header = args.repo.resolve(), args.output.resolve(), args.header.resolve()
    if args.output == args.header:
        parser.error("output and header must be different paths")
    try:
        args.probe_nodes = parse_probe_nodes(args.probe_node)
        run(args)
        return 0
    except (ProbeError, OSError, ValueError, ImportError) as error:
        print(f"error: {error}; no successful mirror outputs published", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
