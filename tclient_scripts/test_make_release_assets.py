#!/usr/bin/env python3
"""Isolated release-asset regressions; all signing keys are generated in memory."""

import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import zipfile

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


SCRIPT = Path(__file__).with_name("make_release_assets.py")
SPEC = importlib.util.spec_from_file_location("make_release_assets", SCRIPT)
assets = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(assets)


class ReleaseAssetsTest(unittest.TestCase):
	def setUp(self):
		self.temporary = tempfile.TemporaryDirectory(prefix="tclient-release-test-")
		self.addCleanup(self.temporary.cleanup)
		self.root = Path(self.temporary.name)
		self.source = self.root / "client"
		self.source.mkdir()
		(self.source / "DDNet.exe").write_bytes(b"test client")
		(self.source / assets.HELPER_NAME).write_bytes(b"test standalone helper")
		(self.source / "data/tclient").mkdir(parents=True)
		(self.source / "data/tclient/resource.txt").write_text("resource", encoding="utf-8")
		(self.source / "cfg").mkdir()
		(self.source / "cfg/private.cfg").write_text("user settings", encoding="utf-8")
		self.inventory_path = self.root / "stage-inventory.json"
		self.inventory = {"files": [
			{"path": name} for name, _ in assets.collect_files(self.source, None)
		]}
		# Packaging tests use synthetic bytes and never create a trusted Windows
		# certificate. The production gate is exercised separately below.
		stage_gate = mock.patch.object(assets, "verify_windows_stage", return_value=self.inventory)
		self.stage_gate = stage_gate.start()
		self.addCleanup(stage_gate.stop)
		self.key = Ed25519PrivateKey.generate()
		self.pem = self.key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
		self.key_path = self.root / "in-memory-test-key.pem"
		public = self.key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
		header = self.root / "update_public_key.h"
		header.write_text("static constexpr unsigned char s_aUpdateEd25519PublicKey[32] = {" + ",".join(f"0x{value:02x}" for value in public) + "};", encoding="utf-8")
		patcher = mock.patch.object(assets, "PUBLIC_KEY_HEADER", header)
		patcher.start()
		self.addCleanup(patcher.stop)

	def package(self, output, *, signed=True, pem=None, full_zip=None, raw_dir=None, inventory=True):
		args = ["--source", str(self.source), "--version", "9.8.7", "--output", str(output), "--quiet"]
		if inventory:
			args += ["--stage-inventory", str(self.inventory_path)]
		if signed:
			args += ["--private-key", str(self.key_path)]
		if full_zip:
			args += ["--full-zip", str(full_zip)]
		if raw_dir:
			args += ["--raw-manifest-dir", str(raw_dir)]
		original_read = Path.read_bytes
		key_bytes = self.pem if pem is None else pem
		# Exercise the real PEM parser without reading or writing a private key file.
		def read_bytes(path):
			return key_bytes if path == self.key_path else original_read(path)
		stdout, stderr = io.StringIO(), io.StringIO()
		with mock.patch.object(Path, "read_bytes", read_bytes), contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
			result = assets.main(args)
		self.assertNotIn("PRIVATE KEY", stdout.getvalue() + stderr.getvalue())
		return result, stderr.getvalue()

	def verify(self, output, *, version="9.8.7", raw_dir=None, raw_url=None):
		args = ["--verify", "--output", str(output), "--version", version]
		if raw_dir:
			args += ["--raw-manifest-dir", str(raw_dir)]
		if raw_url:
			args += ["--raw-manifest-url", raw_url]
		stdout, stderr = io.StringIO(), io.StringIO()
		with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
			result = assets.main(args)
		return result, stderr.getvalue()

	def rewrite_manifest(self, output, transform):
		manifest = json.loads((output / assets.MANIFEST_NAME).read_bytes())
		transform(manifest)
		content = (json.dumps(manifest, indent=2) + "\n").encode("utf-8")
		(output / assets.MANIFEST_NAME).write_bytes(content)
		(output / assets.MANIFEST_SIG_NAME).write_bytes(self.key.sign(content))

	def test_missing_signing_material_fails_before_writing_assets(self):
		output = self.root / "unsigned"
		result, error = self.package(output, signed=False)
		self.assertEqual(result, 1)
		self.assertIn("release signing requires --private-key", error)
		self.assertFalse(output.exists())

	def test_invalid_or_wrong_signing_key_fails_before_writing_assets(self):
		other_key = Ed25519PrivateKey.generate().private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
		for name, pem, message in [("malformed", b"not a PEM", "unencrypted Ed25519 PEM"), ("wrong", other_key, "does not match")]:
			with self.subTest(name=name):
				output = self.root / name
				result, error = self.package(output, pem=pem)
				self.assertEqual(result, 1)
				self.assertIn(message, error)
				self.assertFalse(output.exists())

	def test_missing_or_empty_helper_fails_before_writing_assets(self):
		helper = self.source / assets.HELPER_NAME
		helper.unlink()
		for name in ["missing", "empty"]:
			with self.subTest(name=name):
				if name == "empty":
					helper.write_bytes(b"")
				output = self.root / name
				result, error = self.package(output)
				self.assertEqual(result, 1)
				self.assertIn("bootstrap helper missing", error)
				self.assertFalse(output.exists())

	def test_signed_artifact_contract_and_determinism(self):
		output = self.root / "signed"
		full_zip = self.root / "full.zip"
		result, error = self.package(output, full_zip=full_zip)
		self.assertEqual((result, error), (0, ""))
		manifest_bytes = (output / assets.MANIFEST_NAME).read_bytes()
		signature = (output / assets.MANIFEST_SIG_NAME).read_bytes()
		self.assertEqual(len(signature), 64)
		self.key.public_key().verify(signature, manifest_bytes)
		manifest = json.loads(manifest_bytes)
		for filename, metadata in [(assets.ASSET_NAME, manifest), (assets.HELPER_NAME, manifest["helper"])]:
			content = (output / filename).read_bytes()
			self.assertEqual(len(content), metadata["size"])
			self.assertEqual(hashlib.sha256(content).hexdigest(), metadata["sha256"])
		self.assertEqual(manifest["helper"]["name"], assets.HELPER_NAME)
		self.assertEqual((output / assets.HELPER_NAME).read_bytes(), (self.source / assets.HELPER_NAME).read_bytes())
		for filename in [assets.MANIFEST_NAME, assets.MANIFEST_SIG_NAME]:
			self.assertEqual((output / "update" / filename).read_bytes(), (output / filename).read_bytes())
		for archive in [output / assets.ASSET_NAME, full_zip]:
			with zipfile.ZipFile(archive) as package:
				self.assertIn("DDNet.exe", package.namelist())
				self.assertEqual(package.read(assets.HELPER_NAME), (output / assets.HELPER_NAME).read_bytes())
				self.assertNotIn("cfg/private.cfg", package.namelist())
		second = self.root / "second"
		self.assertEqual(self.package(second), (0, ""))
		for filename in [assets.ASSET_NAME, assets.MANIFEST_NAME, assets.MANIFEST_SIG_NAME, assets.HELPER_NAME]:
			self.assertEqual((output / filename).read_bytes(), (second / filename).read_bytes())
	def test_raw_export_uses_identical_signed_bytes_and_preserves_other_files(self):
		output = self.root / "release"
		raw_dir = self.root / "repo/update"
		raw_dir.mkdir(parents=True)
		(raw_dir / "unrelated.txt").write_bytes(b"keep")
		for name in [assets.MANIFEST_NAME, assets.MANIFEST_SIG_NAME]:
			(raw_dir / name).write_bytes(b"old release")
		self.assertEqual(self.package(output, raw_dir=raw_dir), (0, ""))
		self.assertEqual(assets.read_manifest_pair(raw_dir), assets.read_manifest_pair(output))
		self.assertEqual((raw_dir / "unrelated.txt").read_bytes(), b"keep")
		self.assertEqual(self.verify(output, raw_dir=raw_dir), (0, ""))

	def test_raw_export_validates_real_assets_before_writing_raw(self):
		output = self.root / "release"
		raw_dir = self.root / "repo/update"
		raw_dir.mkdir(parents=True)
		for name in [assets.MANIFEST_NAME, assets.MANIFEST_SIG_NAME]:
			(raw_dir / name).write_bytes(b"previous")
		# A source helper that changes while being copied must not reach repo/update.
		original_copy = assets.shutil.copyfile
		def copy_helper_with_change(source, destination):
			if Path(destination).name == assets.HELPER_NAME:
				return destination.write_bytes(b"changed helper")
			return original_copy(source, destination)
		with mock.patch.object(assets.shutil, "copyfile", side_effect=copy_helper_with_change):
			result, error = self.package(output, raw_dir=raw_dir)
		self.assertEqual(result, 1)
		self.assertIn("release asset size mismatch", error)
		for name in [assets.MANIFEST_NAME, assets.MANIFEST_SIG_NAME]:
			self.assertEqual((raw_dir / name).read_bytes(), b"previous")
		self.assertFalse((output / "update").exists())

	def test_verify_is_read_only_without_source_or_private_key(self):
		output = self.root / "release"
		self.assertEqual(self.package(output), (0, ""))
		with mock.patch.object(assets, "load_signing_key", side_effect=AssertionError("must not sign")), \
			mock.patch.object(Path, "write_bytes", side_effect=AssertionError("must not write")), \
			mock.patch.object(Path, "mkdir", side_effect=AssertionError("must not create directories")):
			self.assertEqual(self.verify(output), (0, ""))
		missing = self.root / "missing"
		self.assertEqual(self.verify(missing)[0], 1)
		self.assertFalse(missing.exists())

	def test_verify_rejects_valid_old_release_even_when_all_copies_agree(self):
		output = self.root / "release"
		self.assertEqual(self.package(output), (0, ""))
		result, error = self.verify(output, version="9.8.8")
		self.assertEqual(result, 1)
		self.assertIn("version mismatch", error)

	def test_verify_rejects_tampered_wrong_key_or_truncated_signature(self):
		output = self.root / "release"
		self.assertEqual(self.package(output), (0, ""))
		content, signature = assets.read_manifest_pair(output)
		for name, changed_content, changed_signature in [
			("content", content + b" ", signature),
			("wrong key", content, Ed25519PrivateKey.generate().sign(content)),
			("truncated", content, signature[:-1]),
		]:
			with self.subTest(name=name):
				(output / assets.MANIFEST_NAME).write_bytes(changed_content)
				(output / assets.MANIFEST_SIG_NAME).write_bytes(changed_signature)
				result, error = self.verify(output)
				self.assertEqual(result, 1)
				self.assertIn("signature", error)

	def test_verify_rejects_signed_metadata_mismatches(self):
		output = self.root / "release"
		self.assertEqual(self.package(output), (0, ""))
		original, signature = assets.read_manifest_pair(output)
		cases = [(field, lambda manifest, field=field: manifest.__setitem__(field, "wrong")) for field in ["product", "channel", "version", "platform", "asset"]]
		cases += [("helper", lambda manifest: manifest["helper"].__setitem__("name", "other.exe"))]
		cases += [("size", lambda manifest: manifest.__setitem__("size", True)), ("sha256", lambda manifest: manifest.__setitem__("sha256", "0" * 63))]
		for field, transform in cases:
			with self.subTest(field=field):
				(output / assets.MANIFEST_NAME).write_bytes(original)
				(output / assets.MANIFEST_SIG_NAME).write_bytes(signature)
				self.rewrite_manifest(output, transform)
				result, error = self.verify(output)
				self.assertEqual(result, 1)
				self.assertIn(field, error)

	def test_verify_rejects_bad_signed_json_and_duplicate_fields(self):
		output = self.root / "release"
		self.assertEqual(self.package(output), (0, ""))
		original, _ = assets.read_manifest_pair(output)
		for name, content in [("invalid JSON", b"{"), ("non object", b"[]"), ("duplicate", original.replace(b"{", b'{"version":"9.8.7",', 1))]:
			with self.subTest(name=name):
				(output / assets.MANIFEST_NAME).write_bytes(content)
				(output / assets.MANIFEST_SIG_NAME).write_bytes(self.key.sign(content))
				self.assertEqual(self.verify(output)[0], 1)

	def test_verify_checks_real_zip_and_helper_size_and_sha256(self):
		output = self.root / "release"
		self.assertEqual(self.package(output), (0, ""))
		for name in [assets.ASSET_NAME, assets.HELPER_NAME]:
			path = output / name
			content = path.read_bytes()
			for problem, changed in [("size", content + b"x"), ("sha256", bytes([content[0] ^ 1]) + content[1:])]:
				with self.subTest(name=name, problem=problem):
					path.write_bytes(changed)
					result, error = self.verify(output)
					self.assertEqual(result, 1)
					self.assertIn(f"release asset {problem} mismatch", error)
				path.write_bytes(content)

	def test_verify_checks_external_raw_not_just_output_update(self):
		output = self.root / "release"
		raw_dir = self.root / "repo/update"
		self.assertEqual(self.package(output, raw_dir=raw_dir), (0, ""))
		original = assets.read_manifest_pair(output)
		self.rewrite_manifest(raw_dir, lambda manifest: manifest.__setitem__("version", "9.8.6"))
		result, error = self.verify(output, raw_dir=raw_dir)
		self.assertEqual(result, 1)
		self.assertIn("version mismatch", error)
		for name, content in zip([assets.MANIFEST_NAME, assets.MANIFEST_SIG_NAME], original):
			(raw_dir / name).write_bytes(content)
		self.rewrite_manifest(raw_dir, lambda manifest: manifest.__setitem__("notes", "same release, different signed bytes"))
		result, error = self.verify(output, raw_dir=raw_dir)
		self.assertEqual(result, 1)
		self.assertIn("signed manifest bytes differ", error)
		(raw_dir / assets.MANIFEST_SIG_NAME).unlink()
		self.assertEqual(self.verify(output, raw_dir=raw_dir)[0], 1)

	def test_verify_checks_output_raw_copy_if_present(self):
		output = self.root / "release"
		self.assertEqual(self.package(output), (0, ""))
		(output / "update" / assets.MANIFEST_SIG_NAME).write_bytes(b"bad")
		self.assertEqual(self.verify(output)[0], 1)
		(output / "update" / assets.MANIFEST_SIG_NAME).unlink()
		(output / "update" / assets.MANIFEST_NAME).unlink()
		(output / "update").rmdir()
		self.assertEqual(self.verify(output), (0, ""))

	def test_verify_remote_pair_is_read_only_and_checks_deployed_bytes(self):
		output = self.root / "release"
		self.assertEqual(self.package(output), (0, ""))
		pair = assets.read_manifest_pair(output)
		url = "https://raw.example.test/repo/master/update/" + assets.MANIFEST_NAME
		def response(content, address):
			stream = io.BytesIO(content)
			stream.geturl = lambda: address
			return stream
		with mock.patch.object(assets.urllib.request, "urlopen", side_effect=[response(pair[0], url), response(pair[1], url + ".sig")]) as fetch:
			self.assertEqual(self.verify(output, raw_url=url), (0, ""))
			self.assertEqual(fetch.call_count, 2)
			for call in fetch.call_args_list:
				self.assertEqual(call.args[0].get_method(), "GET")
				self.assertEqual(call.kwargs["timeout"], 30)
			self.assertEqual([call.args[0].full_url for call in fetch.call_args_list], [url, url + ".sig"])
		old = json.loads(pair[0])
		old["version"] = "9.8.6"
		stale = json.dumps(old).encode("utf-8")
		for name, remote in [("stale", (stale, self.key.sign(stale))), ("tampered", (pair[0], b"x" * 64))]:
			with self.subTest(name=name), mock.patch.object(assets.urllib.request, "urlopen", side_effect=[response(remote[0], url), response(remote[1], url + ".sig")]):
				self.assertEqual(self.verify(output, raw_url=url)[0], 1)

	def test_remote_reader_rejects_unsafe_urls_and_oversized_responses(self):
		url = "https://raw.example.test/update/" + assets.MANIFEST_NAME
		for address in [url.replace("https:", "http:"), url + "?token=x", url + "#fragment", url.replace("raw.example.test", "user:password@raw.example.test"), "file:///" + assets.MANIFEST_NAME]:
			with self.subTest(address=address), mock.patch.object(assets.urllib.request, "urlopen") as fetch:
				with self.assertRaises(assets.PackageError):
					assets.read_remote_manifest_pair(address)
				fetch.assert_not_called()
		stream = io.BytesIO(b"x" * (1024 * 1024 + 1))
		stream.geturl = lambda: url
		with mock.patch.object(assets.urllib.request, "urlopen", return_value=stream):
			with self.assertRaisesRegex(assets.PackageError, "exceeds"):
				assets.read_remote_manifest_pair(url)
		with mock.patch.object(assets.urllib.request, "urlopen", side_effect=assets.urllib.error.URLError("offline")):
			with self.assertRaisesRegex(assets.PackageError, "could not read deployed"):
				assets.read_remote_manifest_pair(url)

	def test_windows_inventory_and_signature_fail_before_outputs_or_raw_changes(self):
		output = self.root / "blocked"
		full_zip = self.root / "blocked-full.zip"
		raw_dir = self.root / "raw"
		raw_dir.mkdir()
		for name in [assets.MANIFEST_NAME, assets.MANIFEST_SIG_NAME]:
			(raw_dir / name).write_bytes(b"existing published pair")
		result, error = self.package(output, full_zip=full_zip, raw_dir=raw_dir, inventory=False)
		self.assertEqual(result, 1)
		self.assertIn("requires --stage-inventory", error)
		self.stage_gate.side_effect = assets.PackageError("Windows code signature verification failed: NotSigned")
		result, error = self.package(output, full_zip=full_zip, raw_dir=raw_dir)
		self.assertEqual(result, 1)
		self.assertIn("NotSigned", error)
		self.assertFalse(output.exists())
		self.assertFalse(full_zip.exists())
		for name in [assets.MANIFEST_NAME, assets.MANIFEST_SIG_NAME]:
			self.assertEqual((raw_dir / name).read_bytes(), b"existing published pair")

	def test_windows_verifier_requires_explicit_success_report(self):
		with mock.patch.object(assets.os, "name", "nt"), mock.patch.object(assets.shutil, "which", return_value="powershell.exe"):
			for status, stdout, stderr, message in [
				(1, "", "NotSigned", "NotSigned"),
				(0, "not JSON", "", "valid report"),
				(0, '{"verified":false,"mode":"Verify"}', "", "confirm verification"),
			]:
				with self.subTest(message=message), mock.patch.object(assets.subprocess, "run", return_value=assets.subprocess.CompletedProcess([], status, stdout, stderr)):
					with self.assertRaisesRegex(assets.PackageError, message):
						assets.verify_windows_authenticode(self.source)
			with mock.patch.object(assets.subprocess, "run", return_value=assets.subprocess.CompletedProcess([], 0, '{"verified":true,"mode":"Verify"}', "")) as run:
				self.assertTrue(assets.verify_windows_authenticode(self.source)["verified"])
				command = run.call_args.args[0]
				self.assertEqual(command[-4:], ["-Directory", str(self.source), "-Mode", "Verify"])
				self.assertNotIn("-Command", command)

	def test_release_zip_preserves_every_byte_including_vendor_signature_tail(self):
		vendor = self.source / "steam_api.dll"
		vendor.write_bytes(b"MZvendor-program" + bytes(range(256)) * 4 + b"AUTHENTICODE-CERTIFICATE-TABLE")
		self.inventory["files"].append({"path": vendor.name})
		output = self.root / "signed"
		full_zip = self.root / "full.zip"
		self.assertEqual(self.package(output, full_zip=full_zip), (0, ""))
		for path in [output / assets.ASSET_NAME, full_zip]:
			with zipfile.ZipFile(path) as archive:
				self.assertEqual(archive.read(vendor.name), vendor.read_bytes())
		self.assertEqual((output / "DDNet.exe").read_bytes(), (self.source / "DDNet.exe").read_bytes())

	def test_zip_byte_gate_rejects_changes_even_when_name_and_size_match(self):
		path = self.root / "changed.zip"
		with zipfile.ZipFile(path, "w") as archive:
			archive.writestr("DDNet.exe", b"bad! client")
		with self.assertRaisesRegex(assets.PackageError, "byte mismatch"):
			assets.verify_zip_contents(path, [("DDNet.exe", self.source / "DDNet.exe")])

	def test_archive_paths_cannot_escape_or_alias_files(self):
		for name in ["../escape.dll", "/absolute.dll", "C:/root.dll", "a\\b.dll", "a./file.dll", "a /file.dll", "a//file.dll"]:
			with self.subTest(name=name):
				stream = io.BytesIO()
				with zipfile.ZipFile(stream, "w") as archive:
					entry = zipfile.ZipInfo("placeholder")
					# Windows ZipInfo normally replaces backslashes while creating
					# the entry; preserve an archive written on another platform.
					entry.filename = name
					archive.writestr(entry, b"bad")
				with zipfile.ZipFile(stream) as archive:
					with self.assertRaises(assets.PackageError):
						assets.archive_entries(archive)
		stream = io.BytesIO()
		with zipfile.ZipFile(stream, "w") as archive:
			archive.writestr("same.dll", b"a")
			archive.writestr("SAME.dll", b"b")
		with zipfile.ZipFile(stream) as archive:
			with self.assertRaisesRegex(assets.PackageError, "duplicate"):
				assets.archive_entries(archive)

	def test_signed_stage_mutation_cannot_export_raw_manifest(self):
		output = self.root / "mutated"
		raw_dir = self.root / "raw"
		original_write = assets.write_zip
		def write_and_change(files, destination):
			original_write(files, destination)
			(self.source / "DDNet.exe").write_bytes(b"different client")
		with mock.patch.object(assets, "write_zip", side_effect=write_and_change):
			result, error = self.package(output, raw_dir=raw_dir)
		self.assertEqual(result, 1)
		self.assertIn("stage changed", error)
		self.assertFalse(raw_dir.exists())

	def test_verify_rejects_packaging_options(self):
		for option in [["--private-key", "unused"], ["--source", "unused"], ["--full-zip", "unused.zip"]]:
			with self.subTest(option=option), contextlib.redirect_stderr(io.StringIO()):
				with self.assertRaises(SystemExit) as exit:
					assets.main(["--verify", "--output", str(self.root / "missing"), "--version", "9.8.7"] + option)
				self.assertEqual(exit.exception.code, 2)


if __name__ == "__main__":
	unittest.main()
