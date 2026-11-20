"""Unit tests for the TClient release orchestrator (offline; no network, no builds).

Covers: spec validation, helper-lock enforcement, dry-run planning, two-phase
plan selection, state resume, release-body encoding, master-sync invocation and
the translation gate. Publish runs against mocked GitHub reads and writes;
build/stage/package require the real toolchain.
"""
from datetime import datetime, timedelta, timezone
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_orchestrator


class SpecValidationTests(unittest.TestCase):
    def make_spec(self, tmp, **overrides):
        tmp = Path(tmp)
        spec = {
            "version": "1.7.8.60",
            "target_commit": "a" * 40,
            "parent_commit": "b" * 40,
            "changed_files": ["src/game/version.h"],
            "notes_path": str(tmp / "notes.txt"),
            "resources_dir": str(tmp / "prev-stage"),
            "monitor_path": str(tmp / "island_media_monitor.exe"),
            "watcher_path": str(tmp / "netease-watcher.exe"),
            "hook_dll_path": str(tmp / "wndhok.dll"),
        }
        spec.update(overrides)
        return spec

    def test_missing_required_key_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            spec = self.make_spec(tmp)
            del spec["version"]
            with self.assertRaises(RuntimeError):
                # main() would reject this before phases run
                for key in release_orchestrator.REQUIRED_SPEC_KEYS:
                    if key not in spec:
                        raise RuntimeError(f"spec missing key: {key}")

    def test_helper_lock_detects_size_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            watcher = Path(tmp) / "netease-watcher.exe"
            watcher.write_bytes(b"wrong size")
            lock = json.loads(release_orchestrator.HELPER_LOCK.read_text(encoding="utf-8-sig"))
            record = lock["files"]["netease-watcher.exe"]
            self.assertNotEqual(watcher.stat().st_size, record["size"])

    def test_helper_lock_detects_hash_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            watcher = Path(tmp) / "netease-watcher.exe"
            watcher.write_bytes(b"x" * 3418112)
            digest = release_orchestrator.sha256_of(watcher)
            lock = json.loads(release_orchestrator.HELPER_LOCK.read_text(encoding="utf-8-sig"))
            self.assertNotEqual(digest, lock["files"]["netease-watcher.exe"]["sha256"])

    def test_helper_lock_passes_on_real_vendored_files(self):
        watcher = Path("D:/kehuduan/tools/netease-watcher/netease-watcher.exe")
        hook = Path("D:/kehuduan/tools/netease-watcher/wndhok.dll")
        if not watcher.is_file() or not hook.is_file():
            self.skipTest("vendored helpers not present on this machine")
        lock = json.loads(release_orchestrator.HELPER_LOCK.read_text(encoding="utf-8-sig"))
        self.assertEqual(release_orchestrator.sha256_of(watcher), lock["files"]["netease-watcher.exe"]["sha256"])
        self.assertEqual(release_orchestrator.sha256_of(hook), lock["files"]["wndhok.dll"]["sha256"])

    def test_two_phase_plan_without_publish(self):
        # without --publish the plan must stop after "package"
        publish = False
        plan = list(release_orchestrator.LOCAL_PHASES) + (list(release_orchestrator.PUBLISH_PHASES) if publish else [])
        self.assertEqual(plan, ["preflight", "mirror_probe", "build", "stage", "package"])

    def test_publish_extends_plan(self):
        publish = True
        plan = list(release_orchestrator.LOCAL_PHASES) + (list(release_orchestrator.PUBLISH_PHASES) if publish else [])
        self.assertEqual(plan[-2:], ["publish", "master_sync"])

    def test_version_step_policy_helper(self):
        # 发行版只 +0.0.1：.59 的后继必须是 .60（orchestrator 的 previous 推导用）
        self.assertEqual(release_orchestrator.previous_version("1.7.8.59"), "1.7.8.58")
        self.assertEqual(release_orchestrator.previous_version("2.0.10.3"), "2.0.10.2")
        with self.assertRaises(RuntimeError):
            release_orchestrator.previous_version("1.7.9.0")
        with self.assertRaises(RuntimeError):
            release_orchestrator.previous_version("1.7.8.x")


class SignatureEvidenceTests(unittest.TestCase):
    def test_signatures_passes_expected_status_to_script(self):
        root = Path(release_orchestrator.__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory() as tmp:
            fake = mock.Mock(returncode=0, stdout="[]", stderr="")
            with mock.patch.object(release_orchestrator.subprocess, "run", return_value=fake) as run:
                self.assertEqual(release_orchestrator.signatures(root, Path(tmp), "signed"), [])
            command = run.call_args[0][0]
        self.assertIn("-Expected", command)
        self.assertEqual(command[command.index("-Expected") + 1], "signed")
        self.assertEqual(command[command.index("-Directory") + 1], str(Path(tmp)))


class MirrorEvidenceTests(unittest.TestCase):
    def fixture(self, tmp):
        root = Path(tmp)
        spec = {"version": "1.7.8.73", "target_commit": "a" * 40, "build_dir": str(root / "build")}
        paths = {"root": root}
        report_path, header = release_orchestrator.mirror_files(spec, paths)
        header.parent.mkdir(parents=True)
        header.write_text("// tested mirrors\n", encoding="utf-8")
        report = {"schema": 1, "catalog_url": "https://github.akams.cn/", "fetched_at": datetime.now(timezone.utc).isoformat(), "release_version": spec["version"], "source_commit": spec["target_commit"], "header_sha256": release_orchestrator.sha256_of(header), "selected": [{"prefix": "https://mirror.example/"}], "reference": {"tag": "v1.7.8.72"}}
        report_path.write_text(json.dumps(report), encoding="utf-8")
        return spec, paths, report, report_path, header

    def test_probe_is_required_before_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            spec = {"build_dir": tmp}
            with self.assertRaisesRegex(RuntimeError, "evidence is missing"):
                release_orchestrator.verify_mirror_inputs(spec, {"root": Path(tmp)}, {})

    def test_untested_or_previous_release_inputs_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            spec, paths, report, report_path, header = self.fixture(tmp)
            report["release_version"] = "1.7.8.72"
            report_path.write_text(json.dumps(report), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "different release"):
                release_orchestrator.verify_mirror_inputs(spec, paths, {})
            report["release_version"] = spec["version"]
            report_path.write_text(json.dumps(report), encoding="utf-8")
            header.write_text("// modified after testing\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "header differs"):
                release_orchestrator.verify_mirror_inputs(spec, paths, {})

    def test_resume_rejects_changed_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            spec, paths, report, report_path, _ = self.fixture(tmp)
            frozen = release_orchestrator.verify_mirror_inputs(spec, paths, {})
            report["reference"]["tag"] = "v1.7.8.71"
            report_path.write_text(json.dumps(report), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "changed after the probe"):
                release_orchestrator.verify_mirror_inputs(spec, paths, {"phases": {"mirror_probe": frozen}})

    def test_stale_probe_blocks_new_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            spec, paths, report, report_path, _ = self.fixture(tmp)
            report["fetched_at"] = (datetime.now(timezone.utc) - timedelta(days=2)).isoformat()
            report_path.write_text(json.dumps(report), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "older than 24 hours"):
                release_orchestrator.verify_mirror_inputs(spec, paths, {}, fresh=True)

    def test_resume_requires_current_phase_prefix(self):
        for completed in (["preflight", "build", "stage", "package"], ["mirror_probe"], ["preflight", "preflight"]):
            with self.subTest(completed=completed), self.assertRaisesRegex(RuntimeError, "incompatible phase order"):
                release_orchestrator.validate_resume_state({"completed": completed})
        completed = ["preflight", "mirror_probe", "build"]
        release_orchestrator.validate_resume_state({"completed": completed, "phases": {phase: {} for phase in completed}})
        with self.assertRaisesRegex(RuntimeError, "missing completed phase evidence"):
            release_orchestrator.validate_resume_state({"completed": completed, "phases": {}})

    def test_client_cannot_change_between_phases(self):
        with tempfile.TemporaryDirectory() as tmp:
            client = Path(tmp) / "DDNet.exe"
            client.write_bytes(b"tested build")
            state = {"phases": {"build": {"client_sha256": release_orchestrator.sha256_of(client)}}}
            release_orchestrator.verify_client_phase(state, "build", client)
            client.write_bytes(b"different executable with the same mirror URLs")
            with self.assertRaisesRegex(RuntimeError, "differs from the frozen build"):
                release_orchestrator.verify_client_phase(state, "build", client)

    def test_packaged_client_must_embed_selected_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            spec, paths, _, _, _ = self.fixture(tmp)
            with self.assertRaisesRegex(RuntimeError, "client lacks"):
                release_orchestrator.verify_mirror_inputs(spec, paths, {}, client=b"old client")
            data = b"https://mirror.example/https://github.com/351950505/TClient/releases/latest/download/TClient-update-win64.json"
            record = release_orchestrator.verify_mirror_inputs(spec, paths, {}, client=data, fresh=True)
            self.assertEqual(record["selected"], ["https://mirror.example/"])


class ReleaseBodyEncodingTests(unittest.TestCase):
    """v1.7.8.61/62 的 GitHub Release 正文出现 \\uXXXX 字面量与字面 \\n。

    根因是旧代码 ``json.dumps()``（默认 ensure_ascii=True）+ ``gh api -F body=``：
    中文被转义、换行变成字面反斜杠 n、还多一层引号。这些用例锁住修复。
    """

    NOTES = "# TClient v1.7.8.63 \u2014 \u4e2d\u6587\u6d4b\u8bd5\n\n- \u7b2c\u4e8c\u884c\n- \u7b2c\u4e09\u884c"

    def write_notes(self, tmp):
        path = Path(tmp) / "notes.txt"
        path.write_text(self.NOTES, encoding="utf-8")
        return path

    def test_body_keeps_chinese_and_newlines(self):
        with tempfile.TemporaryDirectory() as tmp:
            body = release_orchestrator.release_body("D:/TaterClient-src", str(self.write_notes(tmp)))
            self.assertEqual(body, self.NOTES)
            self.assertIn("\u4e2d\u6587\u6d4b\u8bd5", body)
            self.assertIn("\u2014", body)
            self.assertIn("\n", body)
            self.assertNotIn("\\u2014", body)

    def test_empty_notes_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "notes.txt"
            path.write_text("   \n\n  ", encoding="utf-8")
            with self.assertRaises(RuntimeError):
                release_orchestrator.release_body("D:/TaterClient-src", str(path))

    def test_gh_api_json_payload_is_not_double_encoded(self):
        captured = {}

        def fake_gh(repo, *args, token, input_text=None):
            captured["args"] = args
            captured["input"] = input_text
            return json.dumps({"id": 1})

        with mock.patch.object(release_orchestrator, "gh", fake_gh):
            result = release_orchestrator.gh_api_json("repo", "repos/x/y/releases", "t", "POST", {"body": self.NOTES, "draft": True})

        self.assertEqual(result, {"id": 1})
        # 必须走 --input -，不再把正文塞进 argv
        self.assertIn("--input", captured["args"])
        self.assertNotIn("-F", captured["args"])
        self.assertNotIn("-f", captured["args"])
        sent = captured["input"]
        self.assertIn("\u4e2d\u6587\u6d4b\u8bd5", sent)
        self.assertNotIn("\\u2014", sent)
        # 换行在 JSON 字符串里必须是转义序列（合法 JSON），解码后要还原成真实换行
        self.assertEqual(json.loads(sent)["body"], self.NOTES)

    def test_gh_forces_utf8_not_locale(self):
        """Windows 上 text=True 默认用 cp936；中文 payload 会被 GBK 编码，gh 报
        "Problems parsing JSON"，而且读响应时 GBK 解码还会抛 UnicodeDecodeError。
        所有与 gh 的交互都必须显式指定 utf-8。
        """
        seen = {}

        def fake_run(cmd, **kwargs):
            seen.update(kwargs)
            return subprocess.CompletedProcess(cmd, 0, stdout='{"ok": true}', stderr="")

        with mock.patch.object(release_orchestrator, "GH_EXE", Path(sys.executable)), \
             mock.patch.object(release_orchestrator.subprocess, "run", side_effect=fake_run):
            release_orchestrator.gh("repo", "api", "repos/x/y/releases", token="t", input_text="{}")

        self.assertEqual(seen.get("encoding"), "utf-8")


class MasterSyncParsingTests(unittest.TestCase):
    """v1.7.8.62：master_sync 子脚本输出 pretty JSON，父进程只解析最后一行 ``}``。"""

    SPEC = {
        "version": "1.7.8.63",
        "target_commit": "a" * 40,
        "parent_commit": "b" * 40,
        "repo": "D:/TaterClient-src",
    }

    def make_paths(self, tmp):
        root = Path(tmp) / "release-17863"
        root.mkdir()
        return {"root": root, "assets": root / "assets"}

    def test_pretty_printed_subprocess_output_is_parsed(self):
        pretty = json.dumps({"before": "1" * 40, "after": "2" * 40, "force_push": False}, indent=2) + "\n"
        # 这正是 .62 崩溃的形状：末行只是 "}"
        self.assertTrue(pretty.rstrip().endswith("}"))
        self.assertGreater(len(pretty.strip().splitlines()), 1)

        with tempfile.TemporaryDirectory() as tmp:
            paths = self.make_paths(tmp)
            with mock.patch.object(release_orchestrator.subprocess, "run",
                                   return_value=subprocess.CompletedProcess([], 0, stdout=pretty, stderr="")):
                result = release_orchestrator.phase_master_sync(self.SPEC, paths, {})

        self.assertEqual(result["after"], "2" * 40)
        self.assertFalse(result["force_push"])

    def test_single_line_output_still_parses(self):
        one_line = json.dumps({"synced": True})
        with tempfile.TemporaryDirectory() as tmp:
            paths = self.make_paths(tmp)
            with mock.patch.object(release_orchestrator.subprocess, "run",
                                   return_value=subprocess.CompletedProcess([], 0, stdout=one_line, stderr="")):
                result = release_orchestrator.phase_master_sync(self.SPEC, paths, {})
        self.assertEqual(result, {"synced": True})

    def test_command_carries_release_identity(self):
        """参数化脚本必须拿到 tag/version/target/parent，不能再有每版硬编码。"""
        captured = {}

        def fake_run(command, **kwargs):
            captured["command"] = command
            return subprocess.CompletedProcess(command, 0, stdout='{"synced": true}\n', stderr="")

        spec = dict(self.SPEC, expected_master="c" * 40)
        with tempfile.TemporaryDirectory() as tmp:
            paths = self.make_paths(tmp)
            with mock.patch.object(release_orchestrator.subprocess, "run", side_effect=fake_run):
                release_orchestrator.phase_master_sync(spec, paths, {})

        command = captured["command"]
        script = command[command.index("-I") + 1]
        self.assertTrue(script.endswith("sync_master_manifest.py"))
        self.assertIn("--tag", command)
        self.assertEqual(command[command.index("--tag") + 1], "v1.7.8.63")
        self.assertEqual(command[command.index("--version") + 1], "1.7.8.63")
        self.assertEqual(command[command.index("--target") + 1], "a" * 40)
        self.assertEqual(command[command.index("--parent") + 1], "b" * 40)
        self.assertEqual(command[command.index("--expected-master") + 1], "c" * 40)


class PublishTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.spec = {"repo": str(self.root), "version": "1.7.8.99", "target_commit": "a" * 40, "notes_path": str(self.root / "notes.txt")}
        Path(self.spec["notes_path"]).write_text("release notes", encoding="utf-8")
        self.paths = {"root": self.root}
        self.state = {}
        self.base = "repos/" + release_orchestrator.GITHUB_REPO
        self.tag = "v" + self.spec["version"]
        self.release = {"id": 7, "tag_name": self.tag, "draft": True, "target_commitish": self.spec["target_commit"]}
        self.releases = [self.release]
        self.refs = [{"ref": "refs/tags/" + self.tag, "object": {"type": "commit", "sha": self.spec["target_commit"]}}]
        self.tag_objects = {}
        self.content = {"DDNet.exe": b"client", "update.zip": b"update"}
        self.records = []
        for name, data in self.content.items():
            path = self.root / name
            path.write_bytes(data)
            self.records.append({"name": name, "path": str(path), "size": len(data), "sha256": release_orchestrator.sha256_of(path)})
        self.record = {"version": self.spec["version"], "source_commit": self.spec["target_commit"], "assets": self.records, "update_mirrors": {"frozen": True}}
        self.save_record()
        self.assets = []
        self.reads = []
        self.writes = []
        self.fail_endpoint = None
        self.corrupt_upload = False
        self.final_extra = False
        self.asset_reads = 0
        self.resolved_target = self.spec["target_commit"]
        self.real_verify_mirrors = release_orchestrator.verify_mirror_inputs
        for target, replacement in (("github_token", lambda repo: "offline"),
                                    ("verify_mirror_inputs", lambda *a, **k: {"frozen": True}),
                                    ("gh", self.fake_gh)):
            patcher = mock.patch.object(release_orchestrator, target, replacement)
            patcher.start()
            self.addCleanup(patcher.stop)
        patcher = mock.patch.object(release_orchestrator.subprocess, "run", side_effect=self.fake_run)
        patcher.start()
        self.addCleanup(patcher.stop)
        # All process launches must use fake_run; deny accidental network access.
        for target in ("subprocess.Popen", "socket.socket", "socket.create_connection"):
            patcher = mock.patch(target, side_effect=AssertionError("external I/O prohibited"))
            patcher.start()
            self.addCleanup(patcher.stop)

    def save_record(self):
        (self.root / "local-assets.json").write_text(json.dumps(self.record), encoding="utf-8")

    def asset(self, record, digest=True):
        return {"id": 20 + self.records.index(record), "name": record["name"], "size": record["size"], "digest": "sha256:" + record["sha256"] if digest else None}

    def fake_gh(self, root, *args, token, input_text=None):
        self.assertEqual(token, "offline")
        self.assertEqual(args[0], "api")
        endpoint = args[1]
        self.reads.append(endpoint)
        if endpoint == self.fail_endpoint:
            raise RuntimeError("gh failed: HTTP 503")
        if "-X" in args:
            self.assertEqual(args[args.index("-X") + 1], "POST")
            self.assertEqual(endpoint, self.base + "/releases")
            self.writes.append("create")
            self.release.update(json.loads(input_text))
            self.releases = [self.release]
            return json.dumps(self.release)
        if "?per_page=100&page=" in endpoint:
            path, page = endpoint.split("?per_page=100&page=")
            page = int(page)
            if path == self.base + "/releases":
                items = self.releases
            elif path == self.base + "/git/matching-refs/tags/" + self.tag:
                items = self.refs
            elif path == self.base + "/releases/7/assets":
                self.asset_reads += 1
                items = list(self.assets)
                if self.final_extra and self.asset_reads > 1:
                    items.append({"name": "old.zip", "size": 1})
            else:
                self.fail("Unexpected list endpoint: " + endpoint)
            return json.dumps(items[(page - 1) * 100:page * 100])
        if endpoint.startswith(self.base + "/git/tags/"):
            return json.dumps({"object": self.tag_objects[endpoint.rsplit("/", 1)[1]]})
        if endpoint.startswith(self.base + "/commits/"):
            return json.dumps({"sha": self.resolved_target})
        self.fail("Unexpected GitHub call: " + repr(args))

    def fake_run(self, command, **kwargs):
        self.assertEqual(command[0], str(release_orchestrator.GH_EXE))
        self.assertEqual(kwargs["env"]["GH_TOKEN"], "offline")
        if command[1:3] == ["release", "upload"]:
            name = Path(command[-1]).name
            self.assertNotIn("--clobber", command)
            self.writes.append("upload:" + name)
            asset = self.asset(next(record for record in self.records if record["name"] == name))
            if self.corrupt_upload:
                asset["digest"] = "sha256:" + "0" * 64
            self.assets.append(asset)
        elif command[1:3] == ["release", "edit"]:
            self.writes.append("publish")
            self.release["draft"] = False
        elif command[1] == "api" and command[-1] == "Accept: application/octet-stream":
            asset_id = int(command[2].rsplit("/", 1)[1])
            name = self.records[asset_id - 20]["name"]
            return subprocess.CompletedProcess(command, 0, stdout=self.content[name], stderr=b"")
        else:
            self.fail("Unexpected process: " + repr(command))
        return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

    def publish(self):
        return release_orchestrator.phase_publish(self.spec, self.paths, self.state)

    def rejected_without_writes(self, message):
        with self.assertRaisesRegex(RuntimeError, message):
            self.publish()
        self.assertEqual(self.writes, [])

    def test_all_local_assets_checked_before_remote_access(self):
        (self.root / "update.zip").write_bytes(b"broken")
        self.rejected_without_writes("packaged asset differs")
        self.assertEqual(self.reads, [])

    def test_frozen_identity_and_duplicate_names_rejected(self):
        self.record["source_commit"] = "b" * 40
        self.save_record()
        self.rejected_without_writes("different release or commit")
        self.record["source_commit"] = self.spec["target_commit"]
        self.record["assets"].append(self.records[0])
        self.save_record()
        self.rejected_without_writes("duplicate frozen")

    def test_draft_wrong_target_without_tag_rejected(self):
        self.refs = []
        self.release["target_commitish"] = "wrong-branch"
        self.resolved_target = "b" * 40
        self.rejected_without_writes("draft target differs")

    def test_existing_tag_mismatch_overrides_target_commitish(self):
        self.refs[0]["object"]["sha"] = "b" * 40
        self.rejected_without_writes("tag commit differs")

    def test_annotated_tag_dereferenced(self):
        self.refs[0]["object"] = {"type": "tag", "sha": "c" * 40}
        self.tag_objects["c" * 40] = {"type": "tag", "sha": "d" * 40}
        self.tag_objects["d" * 40] = {"type": "commit", "sha": self.spec["target_commit"]}
        self.publish()
        self.assertEqual(self.writes[-1], "publish")

    def test_extra_asset_rejected(self):
        self.assets = [{"name": "old.zip", "size": 1}]
        self.rejected_without_writes("unexpected remote assets")

    def test_wrong_existing_asset_rejected(self):
        for field, value in (("size", 100), ("digest", "sha256:wrong")):
            with self.subTest(field=field):
                self.assets = [self.asset(self.records[0])]
                self.assets[0][field] = value
                self.rejected_without_writes("remote asset " + ("size" if field == "size" else "digest") + " differs")

    def test_correct_partial_draft_only_uploads_missing(self):
        self.assets = [self.asset(self.records[0])]
        self.assertEqual(self.publish(), {"release_id": 7, "published": True})
        self.assertEqual(self.writes, ["upload:update.zip", "publish"])

    def test_complete_draft_does_not_reupload(self):
        self.assets = [self.asset(record) for record in self.records]
        self.publish()
        self.assertEqual(self.writes, ["publish"])

    def test_published_state_lost_returns_without_writes(self):
        self.release["draft"] = False
        self.assets = [self.asset(record) for record in self.records]
        self.assertEqual(self.publish(), {"release_id": 7, "published": True})
        self.assertEqual(self.writes, [])
        self.assertEqual(self.state["remote_digests"], {record["name"]: record["sha256"] for record in self.records})
        self.assertNotIn("master_sync", self.state.get("phases", {}))

    def test_published_requires_tag_and_complete_assets(self):
        self.release["draft"] = False
        self.rejected_without_writes("remote asset set differs")
        self.refs = []
        self.rejected_without_writes("release tag is missing")

    def test_pagination_finds_release_and_duplicate(self):
        self.releases = [{"tag_name": "other"} for _ in range(100)] + [self.release]
        self.assets = [self.asset(record) for record in self.records]
        self.publish()
        self.assertNotIn("create", self.writes)
        self.assertIn(self.base + "/releases?per_page=100&page=2", self.reads)
        self.writes.clear()
        self.releases.append(dict(self.release))
        self.rejected_without_writes("multiple releases")

    def test_assets_pagination_detects_extra_on_second_page(self):
        frozen = {}
        self.assets = []
        for index in range(100):
            name = str(index)
            frozen[name] = {"size": 1, "sha256": "a" * 64}
            self.assets.append({"name": name, "size": 1, "digest": "sha256:" + "a" * 64})
        self.assets.append({"name": "old.zip", "size": 1})
        with self.assertRaisesRegex(RuntimeError, "unexpected remote assets"):
            release_orchestrator.verify_remote_assets(self.root, 7, frozen, "offline", complete=True)
        self.assertEqual(self.writes, [])

    def test_list_and_tag_api_errors_have_no_writes(self):
        for endpoint in (self.base + "/releases?per_page=100&page=1",
                         self.base + "/git/matching-refs/tags/" + self.tag + "?per_page=100&page=1",
                         self.base + "/releases/7/assets?per_page=100&page=1"):
            with self.subTest(endpoint=endpoint):
                self.fail_endpoint = endpoint
                self.rejected_without_writes("HTTP 503")

    def test_published_wrong_identity_or_assets_never_written(self):
        self.release["draft"] = False
        self.assets = [self.asset(record) for record in self.records]
        self.refs[0]["object"]["sha"] = "b" * 40
        self.rejected_without_writes("tag commit differs")
        self.refs[0]["object"]["sha"] = self.spec["target_commit"]
        self.assets[0]["digest"] = "sha256:wrong"
        self.rejected_without_writes("digest differs")
        self.assets = [self.asset(record) for record in self.records] + [{"name": "old.zip", "size": 1}]
        self.rejected_without_writes("unexpected remote assets")

    def test_tag_refs_pagination_uses_exact_tag(self):
        self.refs = [{"ref": "refs/tags/" + self.tag + "-old", "object": {"type": "commit", "sha": "b" * 40}} for _ in range(100)] + self.refs
        self.publish()
        self.assertIn(self.base + "/git/matching-refs/tags/" + self.tag + "?per_page=100&page=2", self.reads)

    def test_new_release_and_existing_tag(self):
        self.releases = []
        self.publish()
        self.assertEqual(self.writes, ["create", "upload:DDNet.exe", "upload:update.zip", "publish"])

    def test_new_release_without_tag(self):
        self.refs = []
        self.test_new_release_and_existing_tag()

    def test_final_asset_digest_and_set_checked_before_publish(self):
        for corrupt, extra, message in ((True, False, "digest differs"), (False, True, "unexpected remote assets")):
            with self.subTest(message=message):
                self.assets = []
                self.asset_reads = 0
                self.writes.clear()
                self.corrupt_upload, self.final_extra = corrupt, extra
                with self.assertRaisesRegex(RuntimeError, message):
                    self.publish()
                self.assertNotIn("publish", self.writes)

    def test_published_resume_continues_master_sync(self):
        self.release["draft"] = False
        self.assets = [self.asset(record) for record in self.records]
        root = self.root / "release-17899"
        root.mkdir()
        (root / "local-assets.json").write_text(json.dumps(self.record), encoding="utf-8")
        state_path = root / "orchestrator-state.json"
        initial = {"completed": list(release_orchestrator.LOCAL_PHASES),
                   "phases": {phase: {} for phase in release_orchestrator.LOCAL_PHASES}}
        state_path.write_text(json.dumps(initial), encoding="utf-8")
        spec = dict(self.spec, work_root=str(self.root), parent_commit="b" * 40, changed_files=[])
        spec_path = self.root / "spec.json"
        spec_path.write_text(json.dumps(spec), encoding="utf-8")
        with mock.patch.object(sys, "argv", ["orchestrator", "run", "--spec", str(spec_path), "--resume", "--publish"]), \
             mock.patch.object(release_orchestrator, "phase_master_sync", side_effect=RuntimeError("mock master sync failure")) as sync, \
             mock.patch("builtins.print"):
            with self.assertRaisesRegex(RuntimeError, "mock master sync failure"):
                release_orchestrator.main()
        sync.assert_called_once()
        saved = json.loads(state_path.read_text(encoding="utf-8"))
        self.assertEqual(saved["phases"]["publish"], {"release_id": 7, "published": True})
        self.assertIn("publish", saved["completed"])
        self.assertNotIn("master_sync", saved["completed"])
        self.assertEqual(self.writes, [])

    def use_expired_mirror_evidence(self, root=None):
        root = root or self.root
        self.spec["build_dir"] = str(self.root / "build")
        report_path, header = release_orchestrator.mirror_files(self.spec, {"root": root})
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text("// tested mirrors\n", encoding="utf-8")
        report = {
            "schema": 1, "catalog_url": "https://github.akams.cn/",
            "fetched_at": (datetime.now(timezone.utc) - timedelta(days=2)).isoformat(),
            "release_version": self.spec["version"], "source_commit": self.spec["target_commit"],
            "header_sha256": release_orchestrator.sha256_of(header),
            "selected": [{"prefix": "https://mirror.example/"}], "reference": {"tag": "v1.7.8.98"},
        }
        report_path.write_text(json.dumps(report), encoding="utf-8")
        frozen = self.real_verify_mirrors(self.spec, {"root": root}, {})
        self.state = {"phases": {"mirror_probe": frozen}}
        self.content["DDNet.exe"] = b"https://mirror.example/https://github.com/351950505/TClient/releases/latest/download/TClient-update-win64.json"
        client = self.root / "DDNet.exe"
        client.write_bytes(self.content["DDNet.exe"])
        self.records[0].update(size=client.stat().st_size, sha256=release_orchestrator.sha256_of(client))
        self.record["update_mirrors"] = frozen
        (root / "local-assets.json").write_text(json.dumps(self.record), encoding="utf-8")
        return report_path

    def resume_expired_published(self, publish_saved=False, changed_report=False):
        self.release["draft"] = False
        root = self.root / "release-17899"
        root.mkdir()
        report_path = self.use_expired_mirror_evidence(root)
        self.assets = [self.asset(record) for record in self.records]
        phases = {phase: {} for phase in release_orchestrator.LOCAL_PHASES}
        phases["mirror_probe"] = self.state["phases"]["mirror_probe"]
        completed = list(release_orchestrator.LOCAL_PHASES)
        if publish_saved:
            completed.append("publish")
            phases["publish"] = {"release_id": 7, "published": True}
        state_path = root / "orchestrator-state.json"
        state_path.write_text(json.dumps({"completed": completed, "phases": phases}), encoding="utf-8")
        spec = dict(self.spec, work_root=str(self.root), parent_commit="b" * 40, changed_files=[])
        spec_path = self.root / "spec.json"
        spec_path.write_text(json.dumps(spec), encoding="utf-8")
        if changed_report:
            report = json.loads(report_path.read_text(encoding="utf-8"))
            report["reference"]["tag"] = "v1.7.8.90"
            report_path.write_text(json.dumps(report), encoding="utf-8")
        with mock.patch.object(release_orchestrator, "verify_mirror_inputs", wraps=self.real_verify_mirrors), \
             mock.patch.object(sys, "argv", ["orchestrator", "run", "--spec", str(spec_path), "--resume", "--publish"]), \
             mock.patch.object(release_orchestrator, "phase_master_sync", return_value={"synced": True}) as sync, \
             mock.patch("builtins.print"):
            if changed_report:
                with self.assertRaisesRegex(RuntimeError, "changed after the probe"):
                    release_orchestrator.main()
                sync.assert_not_called()
            else:
                self.assertEqual(release_orchestrator.main(), 0)
                sync.assert_called_once()
                saved = json.loads(state_path.read_text(encoding="utf-8"))
                self.assertEqual(saved["completed"], list(release_orchestrator.PHASES))
                self.assertEqual(saved["phases"]["publish"], {"release_id": 7, "published": True})
        self.assertEqual(self.writes, [])
        if publish_saved:
            self.assertEqual(self.reads, [])

    def test_expired_published_state_lost_continues_sync(self):
        self.resume_expired_published()

    def test_expired_saved_publish_continues_sync(self):
        self.resume_expired_published(publish_saved=True)

    def test_expired_changed_report_still_blocks_resume(self):
        self.resume_expired_published(changed_report=True)

    def test_expired_changed_report_blocks_saved_publish_resume(self):
        self.resume_expired_published(publish_saved=True, changed_report=True)

    def test_expired_draft_and_new_release_rejected_without_writes(self):
        self.use_expired_mirror_evidence()
        with mock.patch.object(release_orchestrator, "verify_mirror_inputs", wraps=self.real_verify_mirrors):
            for existing in ([self.release], []):
                with self.subTest(existing=bool(existing)):
                    self.releases = existing
                    self.rejected_without_writes("older than 24 hours")

    def test_expired_published_changed_report_rejected_without_writes(self):
        report_path = self.use_expired_mirror_evidence()
        self.release["draft"] = False
        self.assets = [self.asset(record) for record in self.records]
        report = json.loads(report_path.read_text(encoding="utf-8"))
        report["reference"]["tag"] = "v1.7.8.90"
        report_path.write_text(json.dumps(report), encoding="utf-8")
        with mock.patch.object(release_orchestrator, "verify_mirror_inputs", wraps=self.real_verify_mirrors):
            self.rejected_without_writes("changed after the probe")
        self.assertEqual(self.reads, [])

    def test_legacy_assets_without_digest_are_hashed(self):
        self.release["draft"] = False
        self.assets = [self.asset(record, digest=False) for record in self.records]
        self.publish()
        self.assertEqual(self.writes, [])
        self.content["update.zip"] = b"UPdate"
        self.rejected_without_writes("digest differs")


if __name__ == "__main__":
    unittest.main()
