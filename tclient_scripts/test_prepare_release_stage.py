#!/usr/bin/env python3
"""Temporary-directory regressions: python -I test_prepare_release_stage.py."""

import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).with_name("prepare_release_stage.py")
SPEC = importlib.util.spec_from_file_location("tclient_prepare_release_stage_tests", SCRIPT)
stage = importlib.util.module_from_spec(SPEC)
# Deliberately do not register in sys.modules: this is also the packager's -I loader.
SPEC.loader.exec_module(stage)


class PrepareReleaseStageTest(unittest.TestCase):
	def setUp(self):
		self.temporary = tempfile.TemporaryDirectory(prefix="tclient-stage-test-")
		self.addCleanup(self.temporary.cleanup)
		self.root = Path(self.temporary.name)
		self.build = self.root / "Release"
		self.resources = self.root / "old-stage"
		self.data = self.root / "source-data"
		self.helpers = self.root / "explicit-helpers"
		for path in (self.build, self.resources, self.data, self.helpers):
			path.mkdir()
		self.output = self.root / "new-stage"
		self.report = self.root / "inventory.json"
		# Include a trailing certificate-table stand-in. Whole-file pinning must
		# keep these bytes as well as the executable's earlier PE data.
		self.vendor_bytes = b"MZ\x90\x00vendor-code\x00\x00WIN_CERTIFICATE-original-vendor-signature"
		for name, content in (
			("DDNet.exe", b"MZfresh-client"),
			("TClientUpdater.exe", b"MZfresh-updater"),
			("steam_api.dll", self.vendor_bytes),
			("SDL2.dll", b"MZfresh-sdl-certificate"),
			("DDNet-Server.exe", b"MZnot-part-of-client"),
			("testrunner.exe", b"MZnot-part-of-client"),
			("DDNet.pdb", b"debug-symbols"),
		):
			(self.build / name).write_bytes(content)
		self.monitor = self.helpers / "monitor-from-explicit-source.exe"
		self.watcher = self.helpers / "watcher-upstream.exe"
		self.hook = self.helpers / "original-hook.dll"
		self.monitor.write_bytes(b"MZfirst-party-monitor")
		self.watcher.write_bytes(b"MZvendor-watcher-certificate")
		self.hook.write_bytes(b"MZvendor-hook-certificate")
		self.put(self.resources, "data/tclient/ui.txt", b"old-ui")
		self.put(self.resources, "data/game.png", b"game-texture")
		self.put(self.resources, "README.md", b"game instructions")
		self.put(self.resources, "license.txt", b"license")
		self.put(self.resources, "autoexec.cfg", b"echo hello")
		self.put(self.resources, "steam_appid.txt", b"412220")

	def put(self, root, name, content):
		path = root / name
		path.parent.mkdir(parents=True, exist_ok=True)
		path.write_bytes(content)
		return path

	def prepare(self, **changes):
		kwargs = dict(build_dir=self.build, output=self.output, report=self.report,
			resources=self.resources, data_dir=self.data, monitor=self.monitor,
			watcher=self.watcher, hook_dll=self.hook)
		kwargs.update(changes)
		return stage.prepare_stage(**kwargs)

	def assert_no_outputs(self):
		self.assertFalse(os.path.lexists(self.output))
		self.assertFalse(os.path.lexists(self.report))

	def make_link(self, path, target, *, directory=False):
		try:
			path.symlink_to(target, target_is_directory=directory)
			return
		except (OSError, NotImplementedError) as error:
			if directory and os.name == "nt":
				result = subprocess.run(
					[os.environ.get("COMSPEC", "cmd.exe"), "/d", "/c", "mklink", "/J", str(path), str(target)],
					capture_output=True, text=True, timeout=10,
				)
				if result.returncode == 0:
					return
			self.skipTest(f"host cannot create requested test link: {error}")

	def test_fresh_stage_uses_only_build_and_explicit_binary_sources(self):
		for name in ("DDNet.exe", "TClientUpdater.exe", "steam_api.dll", "SDL2.dll", "unknown.exe", "other.dll", "debug.pdb"):
			self.put(self.resources, name, b"stale-unsigned-old-stage")
		self.put(self.resources, "data/nested/unknown.exe", b"old unsigned PE")
		self.put(self.build, "tools/tool.dll", b"must-not-copy-build-subdirectories")
		self.put(self.helpers, "unrequested.dll", b"must-not-copy-whole-helper-directory")
		self.put(self.build, "wndhok.dll", self.hook.read_bytes())
		inventory = self.prepare()
		records = {entry["path"]: entry for entry in inventory["files"]}
		self.assertEqual(set(records), {
			"DDNet.exe", "TClientUpdater.exe", "steam_api.dll", "SDL2.dll",
			"island_media_monitor.exe", "netease-watcher.exe", "wndhok.dll",
			"README.md", "license.txt", "autoexec.cfg", "steam_appid.txt",
			"data/game.png", "data/tclient/ui.txt",
		})
		for name in ("DDNet.exe", "TClientUpdater.exe", "steam_api.dll", "SDL2.dll"):
			self.assertEqual((self.output / name).read_bytes(), (self.build / name).read_bytes())
		self.assertEqual((self.output / "wndhok.dll").read_bytes(), self.hook.read_bytes())
		self.assertEqual(records["steam_api.dll"]["origin"], str(self.build / "steam_api.dll"))
		self.assertEqual(stage.validate_stage(self.output, self.report), inventory)

	def test_runtime_directories_copy_top_level_dlls_with_explicit_origins(self):
		runtime = self.root / "runtime-libs"
		runtime.mkdir()
		self.put(runtime, "avcodec-61.dll", b"MZoriginal-runtime-certificate")
		self.put(runtime, "SDL2.dll", (self.build / "SDL2.dll").read_bytes())
		self.put(runtime, "tool.exe", b"MZnot-requested")
		self.put(runtime, "nested/extra.dll", b"MZnot-recursive")
		inventory = self.prepare(runtime_dirs=(runtime,))
		records = {entry["path"]: entry for entry in inventory["files"]}
		self.assertEqual(records["avcodec-61.dll"]["origin"], str(runtime / "avcodec-61.dll"))
		self.assertEqual(records["avcodec-61.dll"]["role"], "vendor")
		self.assertEqual(records["SDL2.dll"]["origin"], str(self.build / "SDL2.dll"))
		self.assertEqual(inventory["roots"]["runtime_dirs"], [str(runtime)])
		self.assertNotIn("tool.exe", records)
		self.assertNotIn("nested/extra.dll", records)
		stage.validate_stage(self.output, self.report)
		shutil.rmtree(runtime)
		stage.validate_files(self.output, inventory)

	def test_mismatching_duplicate_runtime_or_explicit_hook_dll_is_rejected(self):
		runtime = self.root / "runtime-libs"
		runtime.mkdir()
		for name in ("SDL2.dll", "wndhok.dll"):
			with self.subTest(name=name):
				path = self.put(runtime, name, b"MZwrong-vendor-copy")
				with self.assertRaisesRegex(stage.StageError, "conflicting vendor DLL bytes"):
					self.prepare(runtime_dirs=(runtime,))
				self.assert_no_outputs()
				path.unlink()

	def test_whole_vendor_bytes_and_certificate_trailer_are_preserved(self):
		inventory = self.prepare()
		self.assertEqual((self.output / "steam_api.dll").read_bytes(), self.vendor_bytes)
		record = next(item for item in inventory["files"] if item["path"] == "steam_api.dll")
		self.assertEqual(record["size"], len(self.vendor_bytes))
		self.assertEqual(record["sha256"], hashlib.sha256(self.vendor_bytes).hexdigest())
		(self.output / "steam_api.dll").write_bytes(self.vendor_bytes.replace(b"original-vendor-signature", b"modified-vendor-signature"))
		with self.assertRaisesRegex(stage.StageError, "sha256 mismatch: steam_api.dll"):
			stage.validate_stage(self.output, self.report, allow_first_party_changes=True)

	def test_helpers_have_exact_canonical_names_and_ownership(self):
		inventory = self.prepare()
		records = {entry["path"]: entry for entry in inventory["files"]}
		self.assertEqual({name for name, record in records.items() if record["role"] == "first_party"}, {
			"DDNet.exe", "TClientUpdater.exe", "island_media_monitor.exe",
		})
		for name, original in (("netease-watcher.exe", self.watcher), ("wndhok.dll", self.hook)):
			with self.subTest(name=name):
				self.assertEqual(records[name]["role"], "vendor")
				self.assertEqual(records[name]["origin"], str(original))
				content = (self.output / name).read_bytes()
				(self.output / name).write_bytes(content + b"must-not-resign-vendor")
				with self.assertRaisesRegex(stage.StageError, f"size mismatch: {name}"):
					stage.validate_stage(self.output, self.report, allow_first_party_changes=True)
				(self.output / name).write_bytes(content)

	def test_data_overlay_and_resource_allowlist_exclude_user_state_and_code(self):
		self.put(self.data, "tclient/ui.txt", b"fresh-source-overlay")
		self.put(self.data, "languages/en.txt", b"source-language")
		for name in (
			"settings_ddnet.cfg", "storage.cfg", "unknown.txt", "config_directory.bat",
			"screenshots/screen.png", "update/TClient-update-win64.json", "update_backup_old/config.txt",
			"data/settings_local.cfg", "data/a/update_backup_123/backup.png",
			"data/a/update_staging_123/staging.png", "data/screenshots/screen.png",
			"data/logs/log.txt", "data/crash.dmp", "data/trace.log", "data/asset.old_123",
			"data/asset.bak-123", "data/backup.bak/assets.png", "data/report.log/assets.png",
			"data/nested/helper.EXE", "data/nested/steam.DLL", "data/nested/debug.PDB",
			"data/nested/legacy.exe.bak-still-executable", "data/nested/script.ps1",
		):
			self.put(self.resources, name, b"do-not-copy")
		for name in ("embedded-image.png", "unknown-resource"):
			self.put(self.resources, "data/" + name, b"MZdisguised-unsigned-executable")
		self.put(self.data, "nested/extra.dll", b"source-code-not-resource")
		self.put(self.data, "nested/disguised.png", b"\x7fELFcode")
		inventory = self.prepare()
		records = {entry["path"]: entry for entry in inventory["files"]}
		self.assertEqual({name for name, record in records.items() if record["role"] == "resource"}, {
			"README.md", "license.txt", "autoexec.cfg", "steam_appid.txt", "data/game.png",
			"data/tclient/ui.txt", "data/languages/en.txt",
		})
		self.assertEqual((self.output / "data/tclient/ui.txt").read_bytes(), b"fresh-source-overlay")
		self.assertEqual(records["data/tclient/ui.txt"]["origin"], str(self.data / "tclient/ui.txt"))
		self.assertEqual(records["data/game.png"]["origin"], str(self.resources / "data/game.png"))

	def test_settings_game_art_is_preserved_while_user_settings_are_excluded(self):
		png = (SCRIPT.parent.parent / "data/menuimages/settings.png").read_bytes()
		self.put(self.resources, "data/menuimages/settings.png", png)
		self.put(self.data, "menuimages/settings.png", png)
		self.put(self.resources, "data/menuimages/settings_active.png", png)
		self.put(self.data, "menuimages/settings_icon.png", png)
		excluded = (
			"settings_ddnet.cfg", "data/settings_ddnet.cfg", "data/settings.cfg",
			"data/settings_local.ini", "data/settings_state.json", "data/settings_state.dat",
			"data/settings_local.state", "data/nested/SETTINGS_DDNET.CFG",
		)
		for name in excluded:
			self.put(self.resources, name, b"user settings")
			if name.startswith("data/"):
				self.put(self.data, name[5:], b"user settings overlay")
		inventory = self.prepare()
		records = {entry["path"]: entry for entry in inventory["files"]}
		for name in ("settings.png", "settings_active.png", "settings_icon.png"):
			relative = "data/menuimages/" + name
			self.assertEqual((self.output / relative).read_bytes(), png)
			self.assertEqual(records[relative]["role"], "resource")
			self.assertEqual(records[relative]["sha256"], hashlib.sha256(png).hexdigest())
		for name in excluded:
			self.assertNotIn(name, records)
			self.assertFalse((self.output / name).exists())
		stage.validate_stage(self.output, self.report)

	def test_each_missing_or_empty_required_build_file_fails_without_outputs(self):
		for name in stage.REQUIRED_BUILD_FILES:
			path = self.build / name
			content = path.read_bytes()
			for problem in ("missing", "empty"):
				with self.subTest(name=name, problem=problem):
					if problem == "missing":
						path.unlink()
					else:
						path.write_bytes(b"")
					# An old resource copy cannot satisfy a missing build binary.
					self.put(self.resources, name, b"stale-resource-binary")
					with self.assertRaisesRegex(stage.StageError, "required build file (missing|is empty)"):
						self.prepare()
					self.assert_no_outputs()
					path.write_bytes(content)

	def test_all_explicit_helpers_are_required_and_nonempty(self):
		for argument, path in (("monitor", self.monitor), ("watcher", self.watcher), ("hook_dll", self.hook)):
			with self.subTest(argument=argument):
				with self.assertRaisesRegex(stage.StageError, "explicit .* input is required"):
					self.prepare(**{argument: None})
				content = path.read_bytes()
				path.write_bytes(b"")
				with self.assertRaisesRegex(stage.StageError, "file is empty"):
					self.prepare()
				path.write_bytes(content)
				self.assert_no_outputs()

	def test_preexisting_destination_and_report_are_never_overwritten(self):
		self.output.mkdir()
		with self.assertRaisesRegex(stage.StageError, "must not already exist"):
			self.prepare()
		self.assertTrue(self.output.is_dir())
		self.assertEqual(list(self.output.iterdir()), [])
		self.output.rmdir()
		self.output.write_bytes(b"existing-file")
		with self.assertRaises(stage.StageError):
			self.prepare()
		self.assertEqual(self.output.read_bytes(), b"existing-file")
		self.output.unlink()
		self.report.write_bytes(b"prior-report")
		with self.assertRaisesRegex(stage.StageError, "report must not already exist"):
			self.prepare()
		self.assertEqual(self.report.read_bytes(), b"prior-report")
		self.assertFalse(self.output.exists())

	def test_report_must_be_external_to_stage_and_input_roots(self):
		for report in (self.output / "report.json", self.build / "report.json", self.resources / "report.json", self.data / "report.json"):
			with self.subTest(report=report):
				with self.assertRaises(stage.StageError):
					self.prepare(report=report)
				self.assertFalse(report.exists())
				self.assert_no_outputs()

	def test_source_destination_overlap_and_path_traversal_fail(self):
		for output in (self.build / "new", self.resources / "new", self.data / "new"):
			with self.subTest(output=output):
				with self.assertRaisesRegex(stage.StageError, "output overlaps"):
					self.prepare(output=output)
				self.assertFalse(output.exists())
		for changes in ({"output": self.root / "absent" / ".." / "new-stage"}, {"resources": self.root / "x" / ".." / "old-stage"}):
			with self.subTest(changes=changes):
				with self.assertRaisesRegex(stage.StageError, "path traversal"):
					self.prepare(**changes)
				self.assert_no_outputs()

	def test_symlink_or_junction_in_allowed_resource_tree_is_rejected(self):
		outside = self.root / "outside-data"
		outside.mkdir()
		self.put(outside, "injected.txt", b"outside-input")
		self.make_link(self.resources / "data/linked", outside, directory=True)
		with self.assertRaisesRegex(stage.StageError, "symlink, junction or reparse point"):
			self.prepare()
		self.assert_no_outputs()

	def test_symlink_build_dll_is_rejected(self):
		self.make_link(self.build / "extra.dll", self.hook)
		with self.assertRaisesRegex(stage.StageError, "symlink, junction or reparse point"):
			self.prepare()
		self.assert_no_outputs()

	def test_symlink_in_source_or_output_ancestor_is_rejected(self):
		alias = self.root / "linked-root"
		self.make_link(alias, self.resources, directory=True)
		for changes in ({"resources": alias}, {"output": alias / "new"}):
			with self.subTest(changes=changes):
				with self.assertRaisesRegex(stage.StageError, "symlink, junction or reparse point"):
					self.prepare(**changes)
				self.assertFalse((self.resources / "new").exists())
		self.assert_no_outputs()

	def test_case_collision_and_file_directory_conflict_fail_before_creation(self):
		self.put(self.resources, "README", b"old-case")
		# Case-colliding copies cannot be created on the same Windows directory,
		# but can occur across two overlaid resource trees.
		self.put(self.data, "GAME.PNG", b"different-case-source")
		with self.assertRaisesRegex(stage.StageError, "case-colliding"):
			self.prepare()
		self.assert_no_outputs()
		(self.data / "GAME.PNG").unlink()
		self.put(self.data, "tclient", b"file-colliding-with-old-directory")
		with self.assertRaisesRegex(stage.StageError, "file/directory conflict"):
			self.prepare()
		self.assert_no_outputs()

	def test_copy_failure_cleans_only_new_output_and_leaves_inputs(self):
		original_copy = stage._copy_file
		count = 0
		def interrupted_copy(source, destination, record):
			nonlocal count
			count += 1
			original_copy(source, destination, record)
			if count == 2:
				raise OSError("simulated disk write failure")
		with mock.patch.object(stage, "_copy_file", side_effect=interrupted_copy):
			with self.assertRaisesRegex(stage.StageError, "simulated disk write failure"):
				self.prepare()
		self.assert_no_outputs()
		self.assertEqual((self.build / "steam_api.dll").read_bytes(), self.vendor_bytes)
		self.assertEqual((self.resources / "data/tclient/ui.txt").read_bytes(), b"old-ui")

	def test_report_write_failure_cleans_created_stage_and_partial_report(self):
		with mock.patch.object(stage.json, "dump", side_effect=OSError("simulated report write failure")):
			with self.assertRaisesRegex(stage.StageError, "simulated report write failure"):
				self.prepare()
		self.assert_no_outputs()

	def test_source_changed_between_hash_and_copy_is_rejected(self):
		original_copy = stage._copy_file
		def changed_copy(source, destination, record):
			if source.name == "steam_api.dll":
				source.write_bytes(self.vendor_bytes + b"unexpected-resign")
			return original_copy(source, destination, record)
		with mock.patch.object(stage, "_copy_file", side_effect=changed_copy):
			with self.assertRaisesRegex(stage.StageError, "source changed while copying"):
				self.prepare()
		self.assert_no_outputs()

	def test_first_party_signing_allowance_cannot_disable_vendor_or_resource_hashes(self):
		self.prepare()
		for name in stage.FIRST_PARTY_FILES:
			path = self.output / name
			path.write_bytes(path.read_bytes() + b"appended-first-party-certificate")
		with self.assertRaisesRegex(stage.StageError, "size mismatch"):
			stage.validate_stage(self.output, self.report)
		stage.validate_stage(self.output, self.report, allow_first_party_changes=True)
		path = self.output / "data/game.png"
		path.write_bytes(b"X" * len(path.read_bytes()))
		with self.assertRaisesRegex(stage.StageError, "sha256 mismatch: data/game.png"):
			stage.validate_stage(self.output, self.report, allow_first_party_changes=True)

	def test_allowance_still_rejects_empty_or_missing_first_party(self):
		self.prepare()
		path = self.output / "island_media_monitor.exe"
		path.write_bytes(b"")
		with self.assertRaisesRegex(stage.StageError, "stage file is empty"):
			stage.validate_stage(self.output, self.report, allow_first_party_changes=True)
		path.unlink()
		with self.assertRaisesRegex(stage.StageError, "file set mismatch"):
			stage.validate_stage(self.output, self.report, allow_first_party_changes=True)

	def test_validator_rejects_extra_files_even_in_excluded_directories(self):
		self.prepare()
		for name in ("unexpected.exe", "update_backup_123/payload.txt", "data/update_staging_123/payload.txt"):
			with self.subTest(name=name):
				path = self.put(self.output, name, b"unexpected")
				with self.assertRaisesRegex(stage.StageError, "unexpected="):
					stage.validate_stage(self.output, self.report)
				path.unlink()

	def test_extracted_archive_validation_never_reads_original_sources(self):
		inventory = self.prepare()
		extracted = self.root / "extracted-full"
		shutil.copytree(self.output, extracted)
		for path in (self.build, self.resources, self.data, self.helpers):
			shutil.rmtree(path)
		with self.assertRaisesRegex(stage.StageError, "stage_root mismatch"):
			stage.validate_files(extracted, inventory)
		with mock.patch.object(stage.Path, "write_bytes", side_effect=AssertionError("verification must not write")):
			self.assertIs(stage.validate_files(extracted, inventory, require_stage_root=False, allow_first_party_changes=True), inventory)

	def test_inventory_schema_rejects_traversal_duplicate_ownership_and_wrong_types(self):
		inventory = self.prepare()
		mutations = [
			("schema", lambda doc: doc.__setitem__("schema_version", True)),
			("traversal", lambda doc: doc["files"][0].__setitem__("path", "../outside.exe")),
			("absolute", lambda doc: doc["files"][0].__setitem__("path", "C:/outside.exe")),
			("backslash", lambda doc: doc["files"][0].__setitem__("path", "data\\outside.txt")),
			("empty-part", lambda doc: doc["files"][0].__setitem__("path", "data//outside.txt")),
			("device", lambda doc: doc["files"][0].__setitem__("path", "data/CON.txt")),
			("duplicate", lambda doc: doc["files"].append(dict(doc["files"][0]))),
			("role", lambda doc: next(entry for entry in doc["files"] if entry["path"] == "wndhok.dll").__setitem__("role", "first_party")),
			("origin", lambda doc: doc["files"][0].__setitem__("origin", str(self.root / "unrelated.exe"))),
			("size", lambda doc: doc["files"][0].__setitem__("size", True)),
			("hash", lambda doc: doc["files"][0].__setitem__("sha256", "f" * 63)),
			("required", lambda doc: doc.__setitem__("files", [entry for entry in doc["files"] if entry["path"] != "steam_api.dll"])),
		]
		for label, mutate in mutations:
			with self.subTest(label=label):
				changed = copy.deepcopy(inventory)
				mutate(changed)
				with self.assertRaises(stage.StageError):
					stage.validate_files(self.output, changed, allow_first_party_changes=True)

	def test_invalid_or_duplicate_report_json_and_packaged_report_are_rejected(self):
		self.prepare()
		content = self.report.read_bytes()
		for changed in (b"{", b"[]", b'\xff', content.replace(b"{", b'{"schema_version":1,', 1)):
			with self.subTest(changed=changed[:40]):
				self.report.write_bytes(changed)
				with self.assertRaises(stage.StageError):
					stage.validate_stage(self.output, self.report)
		self.report.write_bytes(content)
		packaged = self.put(self.output, "report.json", content)
		with self.assertRaisesRegex(stage.StageError, "report must be external"):
			stage.validate_stage(self.output, packaged)

	def test_tampered_stage_link_is_rejected_even_with_first_party_allowance(self):
		self.prepare()
		client = self.output / "DDNet.exe"
		client.unlink()
		self.make_link(client, self.build / "DDNet.exe")
		with self.assertRaisesRegex(stage.StageError, "symlink, junction or reparse point"):
			stage.validate_stage(self.output, self.report, allow_first_party_changes=True)

	def test_resources_are_optional_and_report_metadata_is_external(self):
		inventory = self.prepare(resources=None, data_dir=None)
		self.assertEqual(len(inventory["files"]), 7)
		self.assertEqual(inventory["schema_version"], 1)
		self.assertEqual(inventory["stage_root"], str(self.output))
		self.assertIsNone(inventory["roots"]["resources"])
		self.assertIsNone(inventory["roots"]["data_dir"])
		self.assertFalse((self.output / self.report.name).exists())
		self.assertEqual(json.loads(self.report.read_text(encoding="utf-8")), inventory)

	def test_shared_complete_ci_artifact_root_has_explicit_binary_origins(self):
		monitor = self.put(self.build, "island_media_monitor.exe", self.monitor.read_bytes())
		watcher = self.put(self.build, "netease-watcher.exe", self.watcher.read_bytes())
		hook = self.put(self.build, "wndhok.dll", self.hook.read_bytes())
		self.put(self.build, "README.md", b"CI artifact instructions")
		self.put(self.build, "data/game.png", b"CI artifact game texture")
		inventory = self.prepare(resources=self.build, monitor=monitor, watcher=watcher, hook_dll=hook)
		records = {entry["path"]: entry for entry in inventory["files"]}
		self.assertEqual(records["netease-watcher.exe"]["role"], "vendor")
		self.assertEqual(records["wndhok.dll"]["origin"], str(hook))
		self.assertEqual(records["data/game.png"]["role"], "resource")
		stage.validate_stage(self.output, self.report)

	def test_cli_stages_and_verifies_with_python_isolation(self):
		args = [sys.executable, "-I", str(SCRIPT), "--build-dir", str(self.build), "--output", str(self.output),
			"--report", str(self.report), "--monitor", str(self.monitor), "--watcher", str(self.watcher), "--hook-dll", str(self.hook)]
		result = subprocess.run(args, capture_output=True, text=True, timeout=20)
		self.assertEqual(result.returncode, 0, result.stderr)
		verify_args = [sys.executable, "-I", str(SCRIPT), "--verify-stage", str(self.output), "--report", str(self.report)]
		result = subprocess.run(verify_args, capture_output=True, text=True, timeout=20)
		self.assertEqual(result.returncode, 0, result.stderr)
		(self.output / "netease-watcher.exe").write_bytes(b"MZmodified-vendor")
		result = subprocess.run(verify_args + ["--allow-first-party-changes"], capture_output=True, text=True, timeout=20)
		self.assertEqual(result.returncode, 1)
		self.assertIn("netease-watcher.exe", result.stderr)

	def test_cli_rejects_incomplete_inputs_and_inappropriate_verify_options(self):
		for args in (
			["--build-dir", str(self.build), "--output", str(self.output), "--report", str(self.report)],
			["--verify-stage", str(self.output), "--report", str(self.report), "--resources", str(self.resources)],
		):
			with self.subTest(args=args):
				stdout, stderr = io.StringIO(), io.StringIO()
				with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
					self.assertEqual(stage.main(args), 1)
				self.assertIn("error:", stderr.getvalue())
				self.assert_no_outputs()


if __name__ == "__main__":
	unittest.main()
