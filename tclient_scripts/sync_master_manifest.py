"""Synchronize a published signed manifest pair to master without moving its tag.

Generic replacement for the per-release copies that used to live in
``release-<ver>/scripts/``. Everything that was hardcoded there — tag, version,
expected master, paths — now comes from arguments, so a new release no longer
needs a hand-edited clone of this file.

Idempotent in the two cases that actually happen:

* ``master-sync.json`` already exists       -> print it, exit 0.
* master already carries this exact pair    -> print a synthesized record,
  exit 0. This is the case that bit v1.7.8.62: the push had succeeded and only
  the parent orchestrator died, so re-running must not push a second commit.

Usage:
    python sync_master_manifest.py --root <release-dir> --assets <assets-dir> \\
        --tag v1.7.8.63 --version 1.7.8.63 --target <40-hex> [--parent <40-hex>] \\
        [--expected-master <40-hex>] [--repo <repo>]
"""
import argparse
import base64
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys

GH = Path("D:/kehuduan/release-17856/extract/bin/gh.exe")
REMOTE = "repos/351950505/TClient"
ALLOWED = (".gitattributes", "update/TClient-update-win64.json", "update/TClient-update-win64.json.sig")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def git(repo, *args, input=None):
    result = subprocess.run(["git", "-C", str(repo), *args], input=input, capture_output=True, timeout=60,
                            env={**os.environ, "GIT_TERMINAL_PROMPT": "0"})
    require(result.returncode == 0, "Local Git command failed: " + args[0])
    return result.stdout


def blob_sha(content):
    return hashlib.sha1(b"blob " + str(len(content)).encode() + b"\0" + content).hexdigest()


def write_json(path, value):
    with path.open("w", encoding="utf-8") as handle:
        json.dump(value, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, help="release working directory (evidence, local-assets.json)")
    parser.add_argument("--assets", required=True, help="packaged assets directory")
    parser.add_argument("--tag", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--target", required=True, help="40-hex release commit")
    parser.add_argument("--parent", default=None, help="expected source parent (merge-base check)")
    parser.add_argument("--expected-master", default=None,
                        help="assert master is exactly this before syncing; omitted = use current master")
    parser.add_argument("--repo", default="D:/TaterClient-src")
    args = parser.parse_args()

    repo = Path(args.repo)
    root = Path(args.root)
    assets_dir = Path(args.assets)
    tag = args.tag
    target = args.target
    require(re.fullmatch(r"[0-9a-f]{40}", target) is not None, "target must be a full 40-hex SHA")

    snapshot = root / "master-sync-response"
    record_path = root / "master-sync.json"

    # ---- idempotency: the record already describes this exact release
    if record_path.exists():
        record = json.loads(record_path.read_text(encoding="utf-8"))
        require(record.get("release_tag") == tag and record.get("release_commit") == target,
                f"existing {record_path.name} describes {record.get('release_tag')}@{record.get('release_commit')}, not {tag}@{target}")
        record["already_synced"] = True
        print(json.dumps(record, ensure_ascii=False, indent=2), flush=True)
        return 0

    local = json.loads((root / "local-assets.json").read_text(encoding="utf-8"))
    require(local["version"] == args.version and local["source_commit"] == target,
            "Packaged source/version differs from the requested release")

    def check_source():
        require(git(repo, "rev-parse", "HEAD").decode().strip() == target, "HEAD differs from target")
        require(not git(repo, "status", "--porcelain=v1", "--untracked-files=all").strip(),
                "Source worktree/index is not clean")
        if args.parent:
            require(subprocess.run(["git", "-C", str(repo), "merge-base", "--is-ancestor", args.parent, target],
                                   capture_output=True, timeout=60).returncode == 0, "Unexpected source parent")
        tree = git(repo, "rev-parse", target + "^{tree}").decode().strip()
        require(not local.get("source_tree") or local["source_tree"] == tree, "Packaged source tree differs")
        return tree

    tree = check_source()

    credential = git(repo, "credential", "fill", input=b"protocol=https\nhost=github.com\n\n")
    values = dict(line.split(b"=", 1) for line in credential.splitlines() if b"=" in line)
    require(bool(values.get(b"password")), "No GitHub credential available")
    env = os.environ.copy()
    env.pop("GH_DEBUG", None)
    env.update(GH_HOST="github.com", GH_REPO="github.com/351950505/TClient", GH_PROMPT_DISABLED="1",
               GH_TOKEN=values[b"password"].decode())
    del credential, values

    def api(endpoint, payload=None, method="POST"):
        command = [str(GH), "api", "--hostname", "github.com", REMOTE + endpoint]
        data = None
        if payload is not None:
            command.extend(["--method", method, "--input", "-"])
            data = json.dumps(payload).encode()
        result = subprocess.run(command, input=data, env=env, capture_output=True, timeout=90)
        require(result.returncode == 0,
                "GitHub API failed at " + endpoint + " (exit " + str(result.returncode) + "; credential-bearing output suppressed)")
        return json.loads(result.stdout)

    def read_blob_at(relative, revision):
        metadata = api("/contents/" + relative + "?ref=" + revision)
        response = api("/git/blobs/" + metadata["sha"])
        require(response["encoding"] == "base64", "Unexpected Git blob encoding")
        content = base64.b64decode(response["content"])
        require(len(content) == response["size"] == metadata["size"], "Remote blob size differs")
        require(blob_sha(content) == response["sha"] == metadata["sha"], "Remote Git blob hash differs")
        return content

    release = api("/releases/tags/" + tag)
    require(release["tag_name"] == tag and not release["draft"] and not release["prerelease"],
            f"{tag} is not a public stable release")
    require(api("/releases/latest")["id"] == release["id"], f"{tag} is not the latest release")
    require(api("/git/commits/" + target)["tree"]["sha"] == tree, "Remote release tree differs")

    tag_before = api("/git/ref/tags/" + tag)["object"]
    if tag_before["type"] == "tag":  # annotated tag: resolve the inner commit
        tag_before = {"type": "tag-object", "sha": tag_before["sha"],
                      "commit": api("/git/tags/" + tag_before["sha"])["object"]["sha"]}
        require(tag_before["commit"] == target, "Annotated tag does not resolve to the source commit")
    else:
        require(tag_before["sha"] == target, "Release tag differs from exact source commit")

    remote_assets = {asset["name"]: asset for asset in release["assets"]}
    require(len(remote_assets) == len(release["assets"]) == len(local["assets"]) == 6,
            "Expected six unique release assets")
    for asset in local["assets"]:
        path = Path(asset["path"])
        require(path.is_file() and path.stat().st_size == asset["size"],
                "Local release asset size differs: " + asset["name"])
        with path.open("rb") as handle:
            require(hashlib.file_digest(handle, "sha256").hexdigest() == asset["sha256"],
                    "Local release asset hash differs: " + asset["name"])
        remote_asset = remote_assets[asset["name"]]
        require(remote_asset["state"] == "uploaded" and remote_asset["size"] == asset["size"]
                and remote_asset["digest"] == "sha256:" + asset["sha256"],
                "Remote asset differs: " + asset["name"])

    spec = importlib.util.spec_from_file_location("release_assets", repo / "tclient_scripts/make_release_assets.py")
    packager = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(packager)
    metadata = {"product": "TClient", "channel": "stable", "version": args.version, "platform": "win64"}
    manifest, signature = packager.verify_release_assets(assets_dir, metadata)
    require(packager.read_manifest_pair(assets_dir / "update") == (manifest, signature),
            "Nested manifest pair differs")

    master_now = api("/git/ref/heads/master")["object"]["sha"]
    if args.expected_master:
        require(master_now == args.expected_master,
                f"master has moved ({master_now}); review before syncing")

    old_files = {relative: read_blob_at(relative, master_now) for relative in ALLOWED}
    attributes = old_files[".gitattributes"]
    required_lines = (b"update/TClient-update-win64.json -text", b"update/TClient-update-win64.json.sig -text")
    missing = [line for line in required_lines if line not in attributes.splitlines()]
    if missing:
        attributes = (attributes.rstrip(b"\r\n")
                      + b"\n\n# Signatures cover the manifest's exact bytes on every platform.\n"
                      + b"\n".join(missing) + b"\n")
    desired = {".gitattributes": attributes, ALLOWED[1]: manifest, ALLOWED[2]: signature}
    updates = {relative: content for relative, content in desired.items() if content != old_files[relative]}

    if not updates:
        # A previous run pushed master and then died before writing the record.
        record = {"release_id": release["id"], "release_tag": tag, "release_commit": target,
                  "master_before": master_now, "master_after": master_now, "tree": tree,
                  "changed_files": [], "remote_pair_matches_assets": True, "ed25519_verified": True,
                  "line_endings_protected": True, "release_tag_unchanged": True, "force_push": False,
                  "already_synced": True,
                  "release_assets_verified_by_remote_digest": 6,
                  "manifest_sha256": hashlib.sha256(manifest).hexdigest(),
                  "signature_sha256": hashlib.sha256(signature).hexdigest()}
        write_json(record_path, record)
        print(json.dumps(record, ensure_ascii=False, indent=2), flush=True)
        return 0

    require(set(updates).issubset(ALLOWED) and set(ALLOWED[1:]).issubset(updates),
            "Expected manifest/signature changes only, with optional attributes protection")

    snapshot.mkdir(exist_ok=True)
    for relative, content in old_files.items():
        destination = snapshot / "before" / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open("wb") as handle:
            handle.write(content)
    write_json(snapshot / "published-release.json", release)

    base_commit = api("/git/commits/" + master_now)
    entries = []
    for relative, content in updates.items():
        created = api("/git/blobs", {"content": base64.b64encode(content).decode(), "encoding": "base64"})
        require(created["sha"] == blob_sha(content), "Uploaded blob differs: " + relative)
        entries.append({"path": relative, "mode": "100644", "type": "blob", "sha": created["sha"]})
    created_tree = api("/git/trees", {"base_tree": base_commit["tree"]["sha"], "tree": entries})
    now = datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")
    identity = {"name": "351950505", "email": "351950505@users.noreply.github.com", "date": now}
    commit = api("/git/commits", {"message": f"chore(update): sync signed {args.version} fallback manifest",
                                  "tree": created_tree["sha"], "parents": [master_now],
                                  "author": identity, "committer": identity})
    require([parent["sha"] for parent in commit["parents"]] == [master_now]
            and commit["tree"]["sha"] == created_tree["sha"], "Master sync commit parent/tree differs")
    write_json(snapshot / "created-commit.json", commit)

    comparison = api("/compare/" + master_now + "..." + commit["sha"])
    require(comparison["total_commits"] == 1
            and {item["filename"] for item in comparison["files"]} == set(updates),
            "Master sync changes unexpected files")
    require(all(item["status"] == "modified" for item in comparison["files"]),
            "Master sync changes file types")

    require(check_source() == tree, "Source changed before master update")
    require(api("/git/ref/heads/master")["object"]["sha"] == master_now, "master moved before update")
    require(api("/git/ref/tags/" + tag)["object"]["sha"] == tag_before["sha"], "Release tag moved before update")
    updated_ref = api("/git/refs/heads/master", {"sha": commit["sha"], "force": False}, method="PATCH")
    require(updated_ref["object"]["sha"] == commit["sha"], "master ref update differs")
    for relative, content in desired.items():
        require(read_blob_at(relative, commit["sha"]) == content, "Synced bytes differ: " + relative)
    require(api("/git/ref/heads/master")["object"]["sha"] == commit["sha"], "master moved after update")
    require(api("/git/ref/tags/" + tag)["object"]["sha"] == tag_before["sha"], "Release tag changed")
    require(check_source() == tree, "Local source changed during master sync")

    record = {"release_id": release["id"], "release_tag": tag, "release_commit": target,
              "master_before": master_now, "master_after": commit["sha"], "tree": created_tree["sha"],
              "changed_files": list(updates), "remote_pair_matches_assets": True, "ed25519_verified": True,
              "line_endings_protected": True, "release_tag_unchanged": True, "force_push": False,
              "release_assets_verified_by_remote_digest": 6,
              "manifest_sha256": hashlib.sha256(manifest).hexdigest(),
              "signature_sha256": hashlib.sha256(signature).hexdigest()}
    write_json(record_path, record)
    env.pop("GH_TOKEN", None)
    print(json.dumps(record, ensure_ascii=False, indent=2), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
