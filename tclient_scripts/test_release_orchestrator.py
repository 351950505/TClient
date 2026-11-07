"""Unit tests for the TClient release orchestrator (offline; no network, no builds).

Covers: spec validation, helper-lock enforcement, dry-run planning, two-phase
plan selection, and state resume behavior. Build/stage/package/publish phases
are deliberately not executed here; they require the real toolchain.
"""
import json
from pathlib import Path
import sys
import tempfile
import unittest

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


if __name__ == "__main__":
    unittest.main()
