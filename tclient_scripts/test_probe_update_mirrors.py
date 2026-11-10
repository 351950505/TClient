#!/usr/bin/env python3
"""Offline regression tests; run with python -I -B <this file>."""
import hashlib
import importlib.util
import json
import tempfile
import unittest
from unittest.mock import patch
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from pathlib import Path

SPEC = importlib.util.spec_from_file_location("probe_update_mirrors", Path(__file__).with_name("probe_update_mirrors.py"))
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


class CatalogTests(unittest.TestCase):
    def test_catalog_rejects_injection_and_private_literals(self):
        bad = ["localhost", "a.localhost", "proxy.local", "proxy.internal", "127.0.0.1",
               "192.168.1.1", "[::1]", "https://good.net", "good.net/path", "good.net:443",
               "good.net@evil.net", "good.net%0a", "good.net ", "good.net;calc.exe", "-bad.net"]
        entries = [{"label": "proxy", "value": value} for value in ["Fast.Example.net", *bad]]
        import json
        script = "let j=" + json.dumps(entries) + ";"
        domains, rejected = probe.parse_catalog(script)
        self.assertEqual(domains, ["fast.example.net"])
        self.assertEqual(len(rejected), len(bad))
        with self.assertRaises(probe.ProbeError):
            probe.parse_catalog("let j=[{label:'new format',host:'proxy.example.net'}];")


class SignatureTests(unittest.TestCase):
    def setUp(self):
        self.key = Ed25519PrivateKey.generate()
        self.manifest = {"product": "TClient", "channel": "stable", "platform": "win64",
                         "version": "1.7.8.64", "asset": "TClient-update-win64.zip",
                         "size": 2097152, "sha256": "a" * 64,
                         "helper": {"name": "TClientUpdater.exe", "size": 2048, "sha256": "b" * 64}}
        self.assets = {"TClient-update-win64.json": {"size": 0}, "TClient-update-win64.json.sig": {"size": 64},
                       "TClient-update-win64.zip": {"size": 2097152}, "TClientUpdater.exe": {"size": 2048}}

    def validate(self, manifest=None, tamper=False):
        body = json.dumps(self.manifest if manifest is None else manifest).encode()
        signature = self.key.sign(body)
        self.assets["TClient-update-win64.json"]["size"] = len(body)
        return probe.validate_pair((body + b" " if tamper else body, signature), self.key.public_key(),
                                   "v1.7.8.64", self.assets)

    def test_valid_signature_and_stale_signed_version(self):
        self.assertEqual(self.validate()["version"], "1.7.8.64")
        with self.assertRaisesRegex(probe.ProbeError, "version"):
            self.validate({**self.manifest, "version": "1.7.8.63"})

    def test_tampered_signature_and_metadata_digest_are_rejected(self):
        with self.assertRaises(probe.ProbeError):
            self.validate(tamper=True)
        self.assets["TClient-update-win64.zip"]["digest"] = "sha256:" + "c" * 64
        with self.assertRaisesRegex(probe.ProbeError, "digest"):
            self.validate()


class RangeTests(unittest.TestCase):
    def test_range_requires_exact_offsets_complete_tail_and_206(self):
        valid = {"status": 206, "headers": {"content-range": "bytes 1048576-2097151/2097152"}, "size": 2097152}
        probe.check_range(valid, 1048576, 2097151, 2097152)
        for changed in [{"status": 200}, {"headers": {"content-range": "bytes 0-1048575/2097152"}},
                        {"headers": {"content-range": "bytes 1048576-2097150/2097152"}}, {"size": 2097151}]:
            with self.subTest(changed=changed), self.assertRaises(probe.ProbeError):
                probe.check_range({**valid, **changed}, 1048576, 2097151, 2097152)


class HeaderTests(unittest.TestCase):
    def test_arrays_preserve_raw_and_api_index_contract_and_deduplicate(self):
        header = probe.make_header(["https://fast.example.net/", "https://ghfast.top/"])
        import re
        arrays = re.findall(r"static const char \*const (\w+)\[\] = \{(.*?)\};", header, re.S)
        self.assertEqual([name for name, _ in arrays], ["TCLIENT_UPDATE_MANIFEST_SOURCES", "TCLIENT_UPDATE_SOURCE_SIBLING_BASES"])
        manifests = re.findall(r'"([^"\n]*)"', arrays[0][1])
        siblings = re.findall(r'"([^"\n]*)"|\b(nullptr)\b', arrays[1][1])
        siblings = [None if null else value for value, null in siblings]
        self.assertEqual(siblings, ["https://fast.example.net/", "https://ghfast.top/", "https://github1.me3.dpdns.org/",
                                    "https://github2.me3.dpdns.org/", "", "@api", None])
        self.assertEqual(manifests[-1], "https://raw.githubusercontent.com/351950505/TClient/master/update/TClient-update-win64.json")
        self.assertEqual(len(siblings), len(manifests) + 1)
        for prefix, url in zip(siblings[:-2], manifests[:-1]):
            self.assertEqual(url, prefix + "https://github.com/351950505/TClient/releases/latest/download/TClient-update-win64.json")
        with self.assertRaises(probe.ProbeError):
            probe.make_header(['https://good.net/";system("calc");//'])


class WorkflowTests(unittest.TestCase):
    def exercise(self, fault=None):
        from contextlib import redirect_stdout, redirect_stderr
        import io
        import subprocess
        key = Ed25519PrivateKey.generate()
        package, helper = b"PK" + b"z" * (2 * 1024**2 - 2), b"MZ" + b"h" * 2046
        manifest = {"product": "TClient", "channel": "stable", "platform": "win64", "version": "1.7.8.64",
                    "asset": "TClient-update-win64.zip", "size": len(package), "sha256": hashlib.sha256(package).hexdigest(),
                    "helper": {"name": "TClientUpdater.exe", "size": len(helper), "sha256": hashlib.sha256(helper).hexdigest()}}
        body = json.dumps(manifest).encode()
        files = {"TClient-update-win64.json": body, "TClient-update-win64.json.sig": key.sign(body),
                 "TClient-update-win64.zip": package, "TClientUpdater.exe": helper}
        metadata = {"tag_name": "v1.7.8.64", "draft": False, "prerelease": False,
                    "assets": [{"name": name, "size": len(data), "digest": "sha256:" + hashlib.sha256(data).hexdigest(),
                                "browser_download_url": "https://github.com/351950505/TClient/releases/download/v1.7.8.64/" + name}
                               for name, data in files.items()]}
        directories, calls = set(), []

        def network(command, **kwargs):
            self.assertFalse(kwargs["shell"])
            self.assertIn("--ipv4", command)
            self.assertEqual(command[1], "-q")
            self.assertEqual(command[command.index("--proto") + 1], "=https")
            self.assertEqual(command[command.index("--proto-redir") + 1], "=https")
            self.assertEqual(command[command.index("--max-redirs") + 1], "4")
            self.assertNotIn("--insecure", command)
            target, header = Path(command[command.index("--output") + 1]), Path(command[command.index("--dump-header") + 1])
            self.assertNotIn(target.parent, directories)
            self.assertNotIn(Path(__file__).parent, target.parents)
            directories.add(target.parent)
            url, status, extra = command[-1], 200, ""
            calls.append(url)
            if url == "https://github.akams.cn/":
                data = b'<script src="/_next/static/chunks/catalog.js"></script>'
            elif url.endswith("/catalog.js"):
                data = b'let j=[{label:"fixture",value:"probe.example.net"}];'
            elif url.startswith("https://api.github.com/"):
                data = json.dumps(metadata).encode()
            else:
                data = files[url.rsplit("/", 1)[1]]
                if url.endswith(".zip"):
                    if fault == "full_sha":
                        data = b"x" + data[1:]
                    if "--range" in command:
                        status, data = 206, data[:1048576]
                        extra = "Content-Range: bytes 0-1048575/2097152\r\n"
                    elif "-C" in command:
                        self.assertEqual(target.stat().st_size, 1048576)
                        status = 206
                        extra = "Content-Range: bytes 1048576-2097151/2097152\r\n"
                        if fault == "range_shift":
                            extra = "Content-Range: bytes 0-1048575/2097152\r\n"
                        data = target.read_bytes() + data[1048576:]
                        if fault == "truncate":
                            data = data[:-1]
            target.write_bytes(data)
            header.write_bytes(f"HTTP/2 {status}\r\n{extra}\r\n".encode())
            return subprocess.CompletedProcess(command, 0, f"{status}\n{url}".encode(), b"")

        with tempfile.TemporaryDirectory(prefix="tclient-mirror-test-") as temporary:
            root = Path(temporary)
            version = root / "src/game/version.h"
            public = root / "src/game/client/components/tclient/update_public_key.h"
            public.parent.mkdir(parents=True)
            version.write_text('#define GAME_NAME "DDNet"\n#define GAME_RELEASE_VERSION_INTERNAL 20.1\n#define GAME_RELEASE_VERSION STRINGIFY(GAME_RELEASE_VERSION_INTERNAL)\n')
            public.write_text("static constexpr unsigned char s_aUpdateEd25519PublicKey[32] = {" +
                              ",".join(f"0x{byte:02x}" for byte in key.public_key().public_bytes_raw()) + "};")
            output, header = root / "out/update-mirrors.json", root / "generated/tclient_update_mirrors.h"
            with patch.object(probe.shutil, "which", return_value="curl.exe"), patch.object(probe.subprocess, "run", side_effect=network), redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                code = probe.main(["--repo", str(root), "--output", str(output), "--header", str(header),
                                   "--release-version", "1.7.8.72", "--source-commit", "a" * 40,
                                   "--max-candidates", "1", "--max-selected", "1"])
            if fault:
                self.assertEqual(code, 1)
                self.assertFalse(header.exists())
                self.assertFalse(output.exists())
            else:
                self.assertEqual(code, 0)
                report = json.loads(output.read_text())
                self.assertEqual(report["schema"], 1)
                self.assertEqual(report["release_version"], "1.7.8.72")
                self.assertEqual(report["reference"]["tag"], "v1.7.8.64")
                self.assertEqual(report["user_agent"], "DDNet 20.1 (windows; amd64)")
                self.assertEqual(report["header_sha256"], hashlib.sha256(header.read_bytes()).hexdigest())
                self.assertEqual(report["selected"][0]["prefix"], "https://probe.example.net/")
                self.assertTrue(report["selected"][0]["range_verified"])
                self.assertEqual(sum(url.endswith(".zip") for url in calls), 3)
        self.assertTrue(all(not directory.exists() for directory in directories))

    def test_cli_publishes_verified_report_and_two_arrays(self):
        self.exercise()

    def test_failures_never_publish_header_or_report(self):
        for fault in ("full_sha", "range_shift", "truncate"):
            with self.subTest(fault=fault):
                self.exercise(fault)


if __name__ == "__main__":
    unittest.main()
