#!/usr/bin/env python3
"""Public-contract tests for the pure verification planner."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from verification_policy import plan_tests


CATALOG = [
    {"id": "foundation", "groups": ["foundation"], "quick": False},
    {"id": "smoke", "groups": ["runtime"], "quick": True},
    *({"id": group, "groups": [group], "quick": False}
      for group in ("translation", "ui", "input", "runtime", "music",
                    "updater", "native", "build", "rust")),
    {"id": "optional", "groups": [], "quick": False},
]
ALL_IDS = [suite["id"] for suite in CATALOG]
FOUNDATION_IDS = ["foundation"]
QUICK_IDS = ["foundation", "smoke"]


class VerificationPolicyTests(unittest.TestCase):
    def plan(self, paths, *, mode="affected", trusted=True, suites=None, **kwargs):
        catalog = CATALOG if suites is None else suites
        result = plan_tests(mode, paths, catalog,
                            trusted_baseline=trusted, **kwargs)
        self.assertEqual(set(result), {"full", "selected", "not_selected",
                                       "manual", "escalation"})
        self.assertIs(type(result["full"]), bool)
        self.assertIsInstance(result["escalation"], str)
        self.assertIsInstance(result["manual"], list)
        self.assertTrue(all(isinstance(item, str) and item.strip()
                            for item in result["manual"]))
        self.assertEqual(len(result["manual"]), len(set(result["manual"])))
        decisions = []
        for key in ("selected", "not_selected"):
            self.assertIsInstance(result[key], list)
            for item in result[key]:
                self.assertEqual(set(item), {"id", "reason"})
                self.assertIsInstance(item["reason"], str)
                self.assertTrue(item["reason"].strip())
            ids = [item["id"] for item in result[key]]
            self.assertEqual(ids, [suite["id"] for suite in catalog
                                   if suite["id"] in ids])
            decisions.extend(ids)
        self.assertCountEqual(decisions, [suite["id"] for suite in catalog])
        self.assertEqual(len(decisions), len(set(decisions)))
        return result

    def assert_selected(self, result, ids):
        self.assertEqual([item["id"] for item in result["selected"]],
                         [suite_id for suite_id in ALL_IDS if suite_id in ids])

    def test_missing_baseline_keeps_scope_bounded_and_reports_the_gap(self):
        result = self.plan(["docs/usage.md"], trusted=False,
                           baseline_reason="没有可复用的验证记录")
        self.assertIs(result["full"], False)
        self.assert_selected(result, FOUNDATION_IDS)
        self.assertEqual(len(result["not_selected"]), len(ALL_IDS) - len(FOUNDATION_IDS))
        self.assertIn("无可信 full 基线", result["escalation"])
        self.assertIn("没有可复用的验证记录", result["escalation"])
        self.assertTrue(any("范围" in text for text in result["manual"]))

    def test_trusted_docs_or_zero_changes_select_minimum_foundation(self):
        for paths in ([], ["README.md"], ["docs/usage.md"],
                      ["docs/guide/setup.rst", "docs/notes.txt"],
                      ["docs/中文说明.md", "README.md"]):
            with self.subTest(paths=paths):
                result = self.plan(paths)
                self.assertIs(result["full"], False)
                self.assert_selected(result, FOUNDATION_IDS)
                self.assertEqual(result["manual"], [])
                self.assertEqual(result["escalation"], "")

    def test_modes_define_scope_and_plan_is_an_affected_preview(self):
        cases = (
            ("full", True, [], True, ALL_IDS),
            ("full", True, ["README.md"], True, ALL_IDS),
            ("full", False, [], True, ALL_IDS),
            ("quick", True, ["src/base/system.cpp"], False, QUICK_IDS),
            ("quick", False, [], False, QUICK_IDS),
            ("quick", True, ["README.md"], False, QUICK_IDS),
            ("plan", True, [], False, FOUNDATION_IDS),
            ("plan", False, ["README.md"], False, FOUNDATION_IDS),
            ("plan", True, ["unknown.file"], False, FOUNDATION_IDS),
        )
        for mode, trusted, paths, full, selected in cases:
            with self.subTest(mode=mode, trusted=trusted, paths=paths):
                result = self.plan(paths, mode=mode, trusted=trusted)
                self.assertIs(result["full"], full)
                self.assert_selected(result, selected)
                if mode == "quick":
                    self.assertIn("quick", result["escalation"])
                    self.assertIn("不作交付证明", result["escalation"])
                if mode == "plan":
                    affected = self.plan(paths, trusted=trusted)
                    for key in ("full", "selected", "not_selected", "manual"):
                        self.assertEqual(result[key], affected[key])
                    self.assertIn("仅预览", result["escalation"])
                    self.assertIn("不执行", result["escalation"])
                if mode == "full" or paths == ["src/base/system.cpp"]:
                    self.assertTrue(result["manual"])
                if not trusted and mode not in ("quick", "full"):
                    self.assertIn("不构成 full", result["escalation"])

    def test_invalid_mode_is_not_silently_treated_as_affected(self):
        with self.assertRaises(ValueError):
            plan_tests("typo", [], CATALOG, trusted_baseline=True)

    def test_known_paths_select_dependencies_and_relevant_manual_checks(self):
        code = {"native", "build", "runtime"}
        cases = (
            ("data/tclient/languages/simplified_chinese.txt",
             {"translation", "ui"}, ("翻译", "UI")),
            ("data/tclient/languages/traditional_chinese.txt",
             {"translation", "ui"}, ("翻译", "UI")),
            ("src/game/client/components/hud.cpp", code | {"ui"}, ("UI", "网络")),
            ("src/game/client/components/binds.cpp",
             code | {"input", "ui"}, ("输入", "UI", "网络")),
            ("src/game/client/components/tclient/island_music.cpp",
             code | {"music", "ui"}, ("音乐", "UI", "网络")),
            ("src/game/client/localization.cpp",
             code | {"translation", "ui"}, ("翻译", "UI", "网络")),
            ("src/game/client/components/tclient/updater.cpp",
             code | {"updater", "ui"}, ("更新", "UI", "网络")),
            ("src/game/server/gamecontext.cpp", code, ("网络",)),
            ("src/game/collision.cpp", code | {"input", "ui"}, ("输入", "网络")),
            ("src/test/example.cpp", code | {"ui"}, ("UI", "网络")),
            ("src/test/sound.cpp", code | {"music", "ui"}, ("音乐", "网络")),
        )
        for path, groups, manual in cases:
            with self.subTest(path=path):
                result = self.plan([path])
                self.assertIs(result["full"], False)
                expected = set(FOUNDATION_IDS) | {
                    suite["id"] for suite in CATALOG if set(suite["groups"]) & groups
                }
                self.assert_selected(result, expected)
                self.assertEqual(result["escalation"], "")
                checks = "\n".join(result["manual"])
                for text in manual:
                    self.assertIn(text, checks)
                if "music" not in groups:
                    self.assertNotIn("音乐", checks)
                if "input" not in groups:
                    self.assertNotIn("输入", checks)
                for decision in result["selected"]:
                    if decision["id"] in groups:
                        self.assertIn(decision["id"], decision["reason"])
                preview = self.plan([path], mode="plan")
                for key in ("full", "selected", "not_selected", "manual"):
                    self.assertEqual(result[key], preview[key])

    def assert_full(self, result):
        self.assertIs(result["full"], True)
        self.assert_selected(result, ALL_IDS)
        self.assertEqual(result["not_selected"], [])
        self.assertIn("全量", result["escalation"])
        for text in ("UI", "输入", "音乐", "网络"):
            self.assertIn(text, "\n".join(result["manual"]))

    def test_shared_core_build_runners_and_mapping_changes_are_bounded(self):
        paths = (
            "src/base/system.cpp", "src/engine/client/client.cpp",
            "src/engine/shared/config_variables_tclient.h",
            "src/game/client/components/controls.cpp",
            "src/game/client/components/controls.h",
            "src/game/client/components/controls_pipeline.h",
            "src/game/gamecore.cpp", "src/game/gamecore.h",
            "src/game/gamecore_helpers.cpp", "src/game/shared/protocol.h",
            "src/game/generated/protocol.h", "CMakeLists.txt", "CMakePresets.json",
            "CMakeUserPresets.json", "cmake/FindExample.cmake", "deps/zlib/zlib.c",
            "Cargo.toml", "Cargo.lock", "rust-toolchain", "rust-toolchain.toml",
            ".cargo/config.toml", "toolchains/msvc.cmake", "src/game/Cargo.toml",
            "src/base/color.rs", "src/mastersrv/src/main.rs",
            "src/game/client/cmake/FindExample.cmake",
            "src/game/toolchain/config.cmake",
        )
        for path in paths:
            with self.subTest(path=path):
                result = self.plan([path])
                self.assertIs(result["full"], False)
                expected = set(QUICK_IDS) | {"native", "build", "runtime"}
                name = path.lower().rsplit("/", 1)[-1]
                if (name.endswith(".rs") or name in ("cargo.toml", "cargo.lock", "rust-toolchain", "rust-toolchain.toml")
                        or path.lower().startswith((".cargo/", "src/rust-bridge/", "src/masterping/", "src/mastersrv/"))):
                    expected.add("rust")
                self.assert_selected(result, expected)
                self.assertTrue(result["manual"])

    def test_cpp_changes_do_not_select_rust_tests(self):
        for path in ("src/base/system.cpp", "src/engine/client/client.cpp",
                     "src/game/client/components/tclient/tclient.cpp"):
            with self.subTest(path=path):
                result = self.plan([path])
                self.assertNotIn("rust", {item["id"] for item in result["selected"]})

    def test_test_wiring_changes_select_foundation_and_matching_feature_tests(self):
        cases = (
            ("scripts/test_input.js", set(FOUNDATION_IDS) | {"input"}),
            ("tclient_scripts/test_verification_policy.py", set(FOUNDATION_IDS)),
            ("tclient_scripts/verification_policy.py", set(FOUNDATION_IDS)),
            ("tclient_scripts/catalog.json", set(FOUNDATION_IDS)),
            ("tclient_scripts/release_orchestrator.py", set(FOUNDATION_IDS) | {"updater"}),
            ("tclient_scripts/prepare_release_stage.py", set(FOUNDATION_IDS) | {"updater"}),
            ("tclient_scripts/make_release_assets.py", set(FOUNDATION_IDS) | {"updater"}),
            ("tclient_scripts/probe_update_mirrors.py", set(FOUNDATION_IDS) | {"updater"}),
            (".github/workflows/tests.yml", set(FOUNDATION_IDS)),
        )
        for path, expected in cases:
            with self.subTest(path=path):
                result = self.plan([path])
                self.assertIs(result["full"], False)
                self.assert_selected(result, expected)
                self.assertNotIn("全量", result["escalation"])
        result = self.plan(["tclient_scripts/verification_policy.py",
                            "tclient_scripts/test_verification_policy.py",
                            "tclient_scripts/verify_tclient.py"])
        self.assertEqual(result["escalation"].count("验证工具/测试接线变更"), 1)
        self.assert_selected(result, FOUNDATION_IDS)

    def test_unknown_paths_and_non_documentation_under_docs_warn_without_full(self):
        for path in ("unknown.file", "src/experimental/client.cpp", "docs/check.py",
                     "docs/check.js", "docs/check.ps1", "docs/check.sh", "docs/site.yml",
                     "data/tclient/unknown.txt", "data/audio/example.opus",
                     "data/tclient/language/not_the_known_directory.txt"):
            with self.subTest(path=path):
                result = self.plan(["README.md", path])
                self.assertIs(result["full"], False)
                self.assert_selected(result, FOUNDATION_IDS)
                self.assertIn("人工", result["escalation"])

    def test_unsafe_or_odd_paths_cannot_enter_the_documentation_allowlist(self):
        paths = (
            "../docs/guide.md", "docs/../guide.md", "docs/sub/../../README.md",
            "/docs/guide.md", "D:/docs/guide.md", "D:docs/guide.md",
            r"D:\docs\guide.md", r"\\server\docs\guide.md", r"docs\guide.md",
            "", ".", "./README.md", "docs//guide.md", "docs/./guide.md",
            "docs/guide.md/", "docs/guide.md ", " docs/guide.md", "docs /guide.md",
            "docs/\x00guide.md", "docs/\nguide.md", "docs/\tguide.md",
            "docs/%2e%2e/guide.md", "docs/guide?.md", "docs/guide*.md",
            "docs/name:stream.md", "docs/old.md -> docs/new.md", None, 7,
        )
        for path in paths:
            with self.subTest(path=path):
                result = self.plan([path])
                self.assertIs(result["full"], False)
                self.assert_selected(result, FOUNDATION_IDS)
                self.assertIn("人工", result["escalation"])

    def test_native_build_quick_flags_still_require_code_or_full_scope(self):
        catalog = copy.deepcopy(CATALOG)
        for suite in catalog:
            if suite["id"] in ("native", "build"):
                suite["quick"] = True
        for mode in ("affected", "plan", "quick"):
            for paths in ([], ["README.md"], ["data/tclient/languages/deleted.txt"]):
                with self.subTest(mode=mode, paths=paths):
                    result = self.plan(paths, mode=mode, suites=catalog)
                    ids = {item["id"] for item in result["selected"]}
                    self.assertFalse(ids & {"native", "build"})
        result = self.plan(["src/game/client/components/hud.cpp"], suites=catalog)
        self.assert_selected(result, set(QUICK_IDS) | {"native", "build", "runtime", "ui"})
        self.assert_full(self.plan([], mode="full", suites=catalog))

    def test_missing_baseline_is_not_full_and_does_not_grant_trust(self):
        expected = {
            (): set(FOUNDATION_IDS),
            ("README.md",): set(FOUNDATION_IDS),
            ("src/game/client/components/hud.cpp",): set(QUICK_IDS) | {"native", "build", "runtime", "ui"},
        }
        for paths, selected in expected.items():
            with self.subTest(paths=paths):
                for mode in ("affected", "plan"):
                    result = plan_tests(mode, paths, CATALOG, trusted_baseline=False,
                                        baseline_reason="无兼容基线")
                    self.assertIs(result["full"], False)
                    self.assert_selected(result, selected)
                    self.assertIn("不构成 full", result["escalation"])
                    self.assertIn("无兼容基线", result["escalation"])
        self.assertIs(plan_tests("full", [], CATALOG, trusted_baseline=False)["full"], True)

    def test_deleted_paths_are_classified_without_needing_files_to_exist(self):
        self.assert_selected(self.plan(["data/tclient/languages/deleted_language.txt"]),
                             set(FOUNDATION_IDS) | {"translation", "ui"})
        self.assert_selected(self.plan(["src/game/client/components/deleted_music.cpp"]),
                             set(QUICK_IDS) | {"native", "build", "runtime", "ui", "music"})
        result = self.plan(["retired/unknown.cpp"])
        self.assertIs(result["full"], False)
        self.assert_selected(result, FOUNDATION_IDS)
        self.assertIn("人工", result["escalation"])

    def test_rename_uses_both_old_and_new_paths_and_unions_dependencies(self):
        paths = ["src/game/client/components/deleted_music.cpp",
                 "src/game/client/components/new_binds.cpp"]
        result = self.plan(paths)
        self.assert_selected(result, set(QUICK_IDS) |
                             {"native", "build", "runtime", "ui", "music", "input"})
        self.assertEqual(result, self.plan(list(reversed(paths)) + [paths[0]]))
        for paths in (["data/tclient/languages/old.txt", "data/retired/new.txt"],
                      ["scripts/old_runner.py", "docs/runner.md"]):
            with self.subTest(paths=paths):
                result = self.plan(paths)
                self.assertIs(result["full"], False)
                self.assertIn("人工", result["escalation"])

    def test_catalog_order_overlap_extra_fields_and_inputs_are_preserved(self):
        catalog = [
            {"id": "overlap", "groups": ["ui", "input", "ui"], "quick": True,
             "command": ["must-not-run"], "cwd": "must-not-be-read", "enabled": False},
            {"id": "foundation", "groups": ["foundation"], "quick": False},
            {"id": "music", "groups": ["music"], "quick": False},
            {"id": "native", "groups": ["native"], "quick": False},
        ]
        paths = ["data/tclient/languages/new.txt"]
        before = copy.deepcopy((paths, catalog))
        result = self.plan(paths, suites=catalog)
        self.assertEqual([item["id"] for item in result["selected"]], ["overlap", "foundation"])
        self.assertEqual([item["id"] for item in result["not_selected"]], ["music", "native"])
        self.assertEqual((paths, catalog), before)
        self.assertEqual(result, self.plan(paths, suites=catalog))
        result["selected"][0]["reason"] = "caller edit"
        self.assertNotEqual(result, self.plan(paths, suites=catalog))

    def test_empty_catalog_keeps_scope_and_manual_requirements(self):
        for mode in ("plan", "quick", "affected", "full"):
            with self.subTest(mode=mode):
                result = self.plan([], mode=mode, trusted=False, suites=[])
                self.assertEqual(result["selected"], [])
                self.assertEqual(result["not_selected"], [])
                self.assertIs(result["full"], mode == "full")
                if mode == "full":
                    self.assertTrue(result["manual"])
                elif mode in ("plan", "affected"):
                    self.assertEqual(len(result["manual"]), 1)
                    self.assertIn("不代表完整回归", result["manual"][0])
                else:
                    self.assertEqual(result["manual"], [])

    def test_quick_retains_impact_based_manual_acceptance(self):
        result = self.plan(["src/game/client/components/tclient/island_music.cpp"], mode="quick")
        self.assert_selected(result, QUICK_IDS)
        self.assertIs(result["full"], False)
        for text in ("UI", "音乐", "网络"):
            self.assertIn(text, "\n".join(result["manual"]))
        result = self.plan(["README.md"], trusted=False)
        self.assertIs(result["full"], False)
        self.assert_selected(result, FOUNDATION_IDS)
        self.assertIn("不构成 full", result["escalation"])


if __name__ == "__main__":
    unittest.main()
