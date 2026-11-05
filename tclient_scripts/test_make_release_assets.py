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
		self.key = Ed25519PrivateKey.generate()
		self.pem = self.key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
		self.key_path = self.root / "in-memory-test-key.pem"
		public = self.key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
		header = self.root / "update_public_key.h"
		header.write_text("static constexpr unsigned char s_aUpdateEd25519PublicKey[32] = {" + ",".join(f"0x{value:02x}" for value in public) + "};", encoding="utf-8")
		patcher = mock.patch.object(assets, "PUBLIC_KEY_HEADER", header)
		patcher.start()
		self.addCleanup(patcher.stop)

	def package(self, output, *, signed=True, pem=None, full_zip=None):
		args = ["--source", str(self.source), "--version", "9.8.7", "--output", str(output), "--quiet"]
		if signed:
			args += ["--private-key", str(self.key_path)]
		if full_zip:
			args += ["--full-zip", str(full_zip)]
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


if __name__ == "__main__":
	unittest.main()
