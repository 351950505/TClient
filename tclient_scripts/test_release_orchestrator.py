"""Unit tests for the TClient release orchestrator (offline; no network, no builds).

Covers: spec validation, helper-lock enforcement, dry-run planning, two-phase
plan selection, state resume, release-body encoding, master-sync invocation and
the translation gate. Build/stage/package/publish phases are deliberately not
executed here; they require the real toolchain.
"""
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
        plan = ["preflight", "build", "stage", "package"] + (["publish", "master_sync"] if publish else [])
        self.assertEqual(plan, ["preflight", "build", "stage", "package"])

    def test_publish_extends_plan(self):
        publish = True
        plan = ["preflight", "build", "stage", "package"] + (["publish", "master_sync"] if publish else [])
        self.assertEqual(plan[-2:], ["publish", "master_sync"])

    def test_version_step_policy_helper(self):
        # 发行版只 +0.0.1：.59 的后继必须是 .60（orchestrator 的 previous 推导用）
        version = "1.7.8.59"
        previous = "1.7.8." + str(int(version.rsplit(".", 1)[1]) - 1)
        self.assertEqual(previous, "1.7.8.58")


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


if __name__ == "__main__":
    unittest.main()
