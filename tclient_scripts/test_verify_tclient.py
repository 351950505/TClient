#!/usr/bin/env python3
"""Public result, process and baseline contracts for the local verifier."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import verify_tclient as verify


class ResultTests(unittest.TestCase):
    def test_translation_report_requires_complete_simplified_coverage_and_reports_traditional_warning(self):
        output = "\n".join([
            "scanned 20 files, 100 keys, 100 translated, 0 missing",
            "OK: every TCLocalize key has a simplified Chinese translation",
            "traditional coverage: 90/100 keys, 10 missing, 1 reverse-drift",
        ])
        result = verify.evaluate_output("translations", output, 0)
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["metrics"]["keys"], 100)
        self.assertEqual(result["metrics"]["traditional_missing"], 10)
        self.assertIn("繁体 90/100", result["reason"])
        self.assertEqual(verify.evaluate_output("translations", output.replace("\n", "\r\n"), 0)["status"], "PASS")
        missing = output.replace("100 translated, 0 missing", "99 translated, 1 missing")
        self.assertEqual(verify.evaluate_output("translations", missing, 0)["status"], "BLOCKED")

    def test_unittest_requires_nonzero_execution_and_no_silent_skips(self):
        self.assertEqual(verify.evaluate_output("unittest", "Ran 3 tests in 0.1s\n\nOK", 0)["status"], "PASS")
        self.assertEqual(verify.evaluate_output("unittest", "Ran 0 tests in 0.1s\n\nOK", 0)["status"], "BLOCKED")
        self.assertEqual(verify.evaluate_output("unittest", "Ran 3 tests in 0.1s\n\nOK (skipped=1)", 0)["status"], "BLOCKED")
        self.assertEqual(verify.evaluate_output("unittest", "Ran 3 tests in 0.1s\nFAILED", 1)["status"], "FAIL")

    def test_cargo_requires_counted_successful_tests_and_rejects_ignored(self):
        output_with_empty_targets = "\n".join([
            "Running unittests lib.rs (target\\debug\\deps\\ddnet_base-abcd.exe)", "running 0 tests",
            "test result: ok. 0 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out",
            "Doc-tests ddnet_base", "running 2 tests", "test one ... ok", "test two ... ok",
            "test result: ok. 2 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out",
            "Doc-tests ddnet_engine_shared", "running 0 tests",
            "test result: ok. 0 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out",
        ])
        expected = {"ddnet_base:unit": 0, "ddnet_base:doc": 2, "ddnet_engine_shared:doc": 0}
        def parse(text):
            return verify.evaluate_output("cargo", text, 0, cargo_targets=expected)
        aggregate = parse(output_with_empty_targets)
        self.assertEqual(aggregate["status"], "PASS")
        self.assertEqual(aggregate["test_count"], 2)
        self.assertEqual(aggregate["zero_test_harnesses"], ["ddnet_base:unit", "ddnet_engine_shared:doc"])
        self.assertEqual([item["id"] for item in aggregate["harnesses"]], list(expected))
        for malformed in (output_with_empty_targets.rsplit("Doc-tests", 1)[0],
                          output_with_empty_targets.replace("2 passed", "1 passed"),
                          output_with_empty_targets.replace("0 ignored", "1 ignored"),
                          output_with_empty_targets.replace("0 filtered out", "1 filtered out"),
                          output_with_empty_targets + "\n" + output_with_empty_targets):
            self.assertEqual(parse(malformed)["status"], "BLOCKED")
        bad_doc = output_with_empty_targets.replace("test two ... ok", "test two ... FAILED")
        self.assertEqual(parse(bad_doc)["status"], "BLOCKED")

    def test_rust_harness_parser_matches_current_build_log_and_zero_targets(self):
        lines = []
        expected = {}
        for target, doc_count in (("ddnet_base", 10), ("ddnet_engine", 8),
                                  ("ddnet_engine_shared", 0), ("ddnet_test", 0)):
            expected[target + ":unit"] = 0
            expected[target + ":doc"] = doc_count
            lines.extend([f"Running unittests lib.rs (build\\debug\\deps\\{target}-abcdef.exe)", "running 0 tests",
                          "test result: ok. 0 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out"])
            lines.append("Doc-tests " + target)
            lines.append(f"running {doc_count} tests")
            lines.extend(f"test doctest_{index} ... ok" for index in range(doc_count))
            lines.append(f"test result: ok. {doc_count} passed; 0 failed; 0 ignored; 0 measured; 0 filtered out")
        text = "\n".join(lines)
        result = verify.evaluate_output("cargo", text, 0, cargo_targets={
            **expected,
        })
        self.assertEqual(result["test_count"], 18)
        self.assertEqual(len(result["zero_test_harnesses"]), 6)
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(verify.evaluate_output("cargo", "test result: ok. 0 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out", 0)["status"], "BLOCKED")
        self.assertEqual(verify.evaluate_output("cargo", "test result: ok. 4 passed; 0 failed; 1 ignored; 0 measured; 0 filtered out", 0)["status"], "BLOCKED")
        self.assertEqual(verify.evaluate_output("cargo", "test result: FAILED. 3 passed; 1 failed; 0 ignored; 0 measured; 0 filtered out", 101)["status"], "FAIL")

    def test_successful_exit_without_expected_evidence_is_blocked(self):
        result = verify.evaluate_output("marker", "everything is fine", 0, marker=r"^VERIFIED 5$")
        self.assertEqual(result["status"], "BLOCKED")
        result = verify.evaluate_output("marker", "VERIFIED 5\n", 0, marker=r"^VERIFIED 5$")
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(verify.evaluate_output("marker", "VERIFIED 5\r\n", 0,
                                                 marker=r"^VERIFIED 5$" )["status"], "PASS")
        self.assertIsNone(result["test_count"])

    def test_failure_cannot_be_hidden_by_a_pass_marker(self):
        self.assertEqual(verify.evaluate_output("marker", "OK", 1, marker="OK")["status"], "FAIL")

    def test_gtest_xml_rejects_empty_failed_and_skipped_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "result.xml"
            for xml, expected in [
                ('<testsuites tests="2" failures="0" disabled="0" errors="0"><testsuite><testcase status="run" result="completed"/><testcase status="run" result="completed"/></testsuite></testsuites>', "PASS"),
                ('<testsuites tests="0" failures="0" disabled="0" errors="0"/>', "BLOCKED"),
                ('<testsuites tests="1" failures="1" disabled="0" errors="0"><testsuite><testcase status="run" result="completed"><failure/></testcase></testsuite></testsuites>', "FAIL"),
                ('<testsuites tests="1" failures="0" disabled="0" errors="0"><testsuite><testcase status="run" result="skipped"><skipped/></testcase></testsuite></testsuites>', "BLOCKED"),
            ]:
                path.write_text(xml, encoding="utf-8")
                self.assertEqual(verify.evaluate_gtest(path, expected_disabled=0)["status"], expected)

    def test_reduced_case_count_cannot_silently_pass(self):
        current = {"id": "example", "status": "PASS", "test_count": 2, "reason": "OK"}
        baseline = {"results": [{"id": "example", "status": "PASS", "test_count": 3}]}
        self.assertEqual(verify.check_count_regression(current, baseline)["status"], "BLOCKED")
        self.assertEqual(verify.check_count_regression({**current, "test_count": 4}, baseline)["status"], "PASS")
        self.assertEqual(verify.check_count_regression(current, None)["status"], "PASS")

    def test_missing_gtest_report_is_not_success(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(verify.evaluate_gtest(Path(directory) / "missing.xml")["status"], "BLOCKED")


class ProcessTests(unittest.TestCase):
    def test_missing_program_and_timeout_are_not_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            missing = verify.run_command([str(directory / "missing-program")], directory, directory / "missing.log", timeout=2)
            self.assertEqual(missing["status"], "BLOCKED")
            slow = verify.run_command([sys.executable, "-I", "-B", "-c", "import time; time.sleep(15)"], directory, directory / "timeout.log", timeout=0.1)
            self.assertEqual(slow["status"], "FAIL")
            self.assertIn("超时", slow["reason"])

    def test_successful_parent_with_live_child_is_not_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            code = "import subprocess,sys; subprocess.Popen([sys.executable,'-I','-c','import time; time.sleep(30)'])"
            result = verify.run_command([sys.executable, "-I", "-B", "-c", code], directory, directory / "child.log", timeout=5)
            self.assertEqual(result["status"], "FAIL")
            self.assertIn("子进程", result["reason"])

    def test_real_command_output_and_failure_are_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            result = verify.run_command([sys.executable, "-I", "-B", "-c", "print('probe'); raise SystemExit(3)"], directory, directory / "result.log", timeout=5)
            self.assertEqual(result["status"], "FAIL")
            self.assertEqual(result["returncode"], 3)
            self.assertIn("probe", (directory / "result.log").read_text(encoding="utf-8"))


class IdentityTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.repo = Path(self.directory.name)
        subprocess.run(["git", "init", "-q", str(self.repo)], check=True)
        (self.repo / "source.txt").write_text("first\n", encoding="utf-8")
        self.git("add", "source.txt")
        self.git("-c", "user.name=Verifier Test", "-c", "user.email=test@example.invalid", "commit", "-qm", "fixture")

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.repo), *args], check=True, capture_output=True)

    def test_snapshot_covers_untracked_dirty_deleted_and_renamed_inputs(self):
        before = verify.snapshot_repository(self.repo)
        (self.repo / "source.txt").write_text("changed\n", encoding="utf-8")
        (self.repo / "new.cpp").write_text("new", encoding="utf-8")
        after = verify.snapshot_repository(self.repo)
        self.assertNotEqual(before["fingerprint"], after["fingerprint"])
        self.assertEqual(set(verify.changed_inputs(before, after)), {"source.txt", "new.cpp"})
        (self.repo / "source.txt").rename(self.repo / "renamed.txt")
        renamed = verify.snapshot_repository(self.repo)
        self.assertEqual(set(verify.changed_inputs(after, renamed)), {"source.txt", "renamed.txt"})

    def test_git_changed_paths_include_tracked_untracked_and_both_rename_sides(self):
        (self.repo / "source.txt").write_text("changed\n", encoding="utf-8")
        (self.repo / "new.cpp").write_text("new", encoding="utf-8")
        self.assertEqual(set(verify.git_changed_paths(self.repo)), {"source.txt", "new.cpp"})
        (self.repo / "source.txt").rename(self.repo / "renamed.cpp")
        self.assertEqual(set(verify.git_changed_paths(self.repo)), {"source.txt", "new.cpp", "renamed.cpp"})

    def test_baseline_requires_complete_full_run_and_matching_tool(self):
        identity = verify.snapshot_repository(self.repo)
        report = {
            "schema_version": 1, "mode": "full", "automatic_status": "PASS",
            "complete": True, "tool_fingerprint": "tool", "identity": identity,
            "results": [{"id": "one", "status": "PASS"}], "required_ids": ["one"],
        }
        self.assertTrue(verify.validate_baseline(report, identity, "tool", ["one"])[0])
        self.assertFalse(verify.validate_baseline({**report, "identity": {**identity, "fingerprint": "bad"}},
                                                  identity, "tool", ["one"])[0])
        self.assertFalse(verify.validate_baseline({**report, "identity": {**identity, "repo": "elsewhere"}},
                                                  identity, "tool", ["one"])[0])
        for update in [{"mode": "quick"}, {"complete": False}, {"results": []}, {"tool_fingerprint": "old"}, {"automatic_status": "BLOCKED"}]:
            self.assertFalse(verify.validate_baseline({**report, **update}, identity, "tool", ["one"])[0])

    def test_cli_plan_does_not_execute_and_mutating_run_is_invalidated(self):
        tools = self.repo / "tools"
        tools.mkdir()
        for name in ("verify_tclient.py", "verification_policy.py"):
            shutil.copyfile(Path(__file__).with_name(name), tools / name)
        (tools / "verification_catalog.py").write_text(
            "SUITES = [{'id':'fixture','type':'python','path':'mutate.py','groups':['foundation'],"
            "'quick':True,'evidence':'synthetic','contract':'marker','marker':'^EXECUTED$'}]\nCOVERAGE_GAPS=[]\n", encoding="utf-8")
        (self.repo / "mutate.py").write_text(
            "from pathlib import Path\nPath('source.txt').write_text('changed')\nprint('EXECUTED')\n", encoding="utf-8")
        (self.repo / "source.txt").write_text("working tree change\n", encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stale_snapshot = verify.snapshot_repository(self.repo)
            stale_report = {
                "schema_version": verify.SCHEMA_VERSION, "mode": "full", "automatic_status": "PASS",
                "complete": True, "tool_fingerprint": "stale-tool", "identity": stale_snapshot,
                "results": [{"id": "fixture", "status": "PASS"}], "required_ids": ["fixture"],
            }
            baseline = root / "stale-baseline.json"
            baseline.write_text(json.dumps(stale_report), encoding="utf-8")
            for mode, code in (("plan", 0), ("affected", 2)):
                output = root / mode
                result = subprocess.run([sys.executable, "-X", "utf8", "-I", "-B", str(tools / "verify_tclient.py"),
                                         mode, "--repo", str(self.repo), "--baseline", str(baseline), "--output-dir", str(output)],
                                        capture_output=True, timeout=30)
                self.assertEqual(result.returncode, code,
                                 mode + ": " + result.stderr.decode("utf-8", errors="replace"))
                report = json.loads((output / "report.json").read_text(encoding="utf-8"))
                self.assertFalse(report["complete"])
                self.assertFalse(report["baseline_valid"])
                self.assertFalse(report["selection"]["full"])
                self.assertEqual([item["id"] for item in report["selection"]["selected"]], ["fixture"])
                self.assertIn("source.txt", report["changes"])
                if mode == "plan":
                    self.assertEqual(report["automatic_status"], "NOT_RUN")
                    self.assertIn("- fixture：", (output / "summary.md").read_text(encoding="utf-8"))
                    self.assertEqual((self.repo / "source.txt").read_text(encoding="utf-8"), "working tree change\n")
                    self.assertEqual(report["results"], [])
                else:
                    self.assertEqual(report["automatic_status"], "BLOCKED")
                    self.assertIn("输入", report["error"])
                    self.assertEqual(report["results"][0]["log"], "fixture.log")

    def test_baseline_from_other_repository_or_different_catalog_is_rejected(self):
        identity = verify.snapshot_repository(self.repo)
        report = {"schema_version": 1, "mode": "full", "automatic_status": "PASS", "complete": True,
                  "tool_fingerprint": "tool", "identity": {**identity, "repo": "elsewhere"},
                  "results": [{"id": "one", "status": "PASS"}], "required_ids": ["one"]}
        self.assertFalse(verify.validate_baseline(report, identity, "tool", ["one"])[0])
        report["identity"] = identity
        self.assertFalse(verify.validate_baseline(report, identity, "tool", ["one", "two"])[0])


class BuildCommandTests(unittest.TestCase):
    def test_release_binary_uses_generated_target_not_old_default_filename(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            (root / "DDNet.exe").touch()
            fields = {"OutDir": str(root), "TargetName": "candidate", "TargetExt": ".exe"}
            (root / "game-client.vcxproj").write_text("<Project><PropertyGroup>" + "".join(
                f'''<{key} Condition="'$(Configuration)|$(Platform)'=='Release|x64'">{value}</{key}>'''
                for key, value in fields.items()) + "</PropertyGroup></Project>", encoding="utf8")
            self.assertEqual(verify.vs_release_exe(root, "game-client"), root / "candidate.exe")

    def test_rejects_persistent_database_test_config_before_any_build(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory).resolve()
            for value in ("ON", "1", "YES", "unexpected"):
                (path / "CMakeCache.txt").write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={path}\nTEST_MYSQL:BOOL={value}\n", encoding="utf8")
                result = verify.execute_suite({"id": "build", "type": "build"}, path, path, path, False, {})
                self.assertEqual(result["status"], "BLOCKED")
                self.assertIn("TEST_MYSQL", result["reason"])
                self.assertFalse((path / "build.log").exists())

    def test_candidate_build_rejects_release_mirror_probe_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory).resolve()
            cache = "\n".join((
                f"CMAKE_HOME_DIRECTORY:INTERNAL={path}",
                "TEST_MYSQL:BOOL=OFF",
                "TCLIENT_RELEASE_MIRRORS:BOOL=ON",
            )) + "\n"
            (path / "CMakeCache.txt").write_text(cache, encoding="utf8")
            result = verify.execute_suite({"id": "build", "type": "build"}, path, path, path, False, {})
            self.assertEqual(result["status"], "BLOCKED")
            self.assertIn("TCLIENT_RELEASE_MIRRORS=OFF", result["reason"])
            self.assertFalse((path / "build.log").exists())

    def test_filtered_or_sharded_shell_cannot_reduce_full_native_suite(self):
        env = verify.test_environment("tmp", {"GTEST_FILTER": "A.*", "gtest_total_shards": "3", "GTEST_REPEAT": "0"})
        self.assertFalse(any(k.upper().startswith("GTEST_") for k in env))

    def test_test_environment_forces_cargo_offline_and_capped_builders(self):
        env = verify.test_environment("temp-dir", {"CARGO_NET_OFFLINE": "false", "CL_MPCount": "8"})
        self.assertEqual(env["CARGO_NET_OFFLINE"], "true")
        self.assertEqual(env["CARGO_BUILD_JOBS"], "2")
        self.assertEqual(env["RUST_TEST_THREADS"], "2")
        self.assertEqual(env["GIT_ALLOW_PROTOCOL"], "file")
        self.assertEqual(env["CMAKE_BUILD_PARALLEL_LEVEL"], "2")
        self.assertEqual(env["CL_MPCount"], "1")
        self.assertEqual((env["TEMP"], env["TMP"], env["TMPDIR"]), ("temp-dir",) * 3)

    def test_rust_tests_are_blocked_without_current_candidate(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            suite = {"id": "rust", "type": "cmake-target", "target": "run_rust_tests", "contract": "cargo"}
            result = verify.execute_suite(suite, path, path, path, False, os.environ.copy())
            self.assertEqual(result["status"], "BLOCKED")
            self.assertIn("不运行", result["reason"])

    def test_native_tests_are_blocked_without_current_build(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            result = verify.execute_suite({"id": "native", "type": "native"}, path, path, path, False, os.environ.copy())
            self.assertEqual(result["status"], "BLOCKED")
            self.assertIn("不运行旧", result["reason"])

    def test_build_from_different_source_is_blocked_before_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "CMakeCache.txt").write_text("CMAKE_HOME_DIRECTORY:INTERNAL=/different/source\n", encoding="utf-8")
            result = verify.execute_suite({"id": "build", "type": "build"}, path, path, path, False, os.environ.copy())
            self.assertEqual(result["status"], "BLOCKED")
            self.assertFalse((path / "build.log").exists())

    def test_msvc_build_limits_both_parallelism_levels(self):
        command = verify.build_command("cmake", Path("build"), "Visual Studio 18 2026", ["testrunner"])
        self.assertEqual(command[command.index("--parallel") + 1], "2")
        self.assertIn("/p:CL_MPCount=1", command[command.index("--") + 1:])
        self.assertIn("/nr:false", command[command.index("--") + 1:])

    def test_non_msvc_build_does_not_receive_msbuild_arguments(self):
        command = verify.build_command("cmake", Path("build"), "Ninja", ["testrunner"])
        self.assertEqual(command[command.index("--parallel") + 1], "2")
        self.assertNotIn("/p:CL_MPCount=1", command)


if __name__ == "__main__":
    unittest.main()
