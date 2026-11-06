#!/usr/bin/env python3
"""Dependency-free Windows Authenticode boundary tests; no keys, certs or network.

Run: python -I tclient_scripts/test_windows_sign_release.py
The fake certificates are plain PowerShell objects, never X509 certificates. Real
PowerShell parsing, file handling, validation and signing argument construction run;
only certificate-store access, trust results and SignTool execution are replaced.
"""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("windows_sign_release.ps1")
POWERSHELL = shutil.which("powershell.exe")
FIRST_PARTY = ("DDNet.exe", "TClientUpdater.exe", "island_media_monitor.exe")
THIRD_PARTY = ("steam_api.dll", "discord_game_sdk.dll", "vulkan-1.dll")
THUMBPRINT = "A1" * 20
OTHER_THUMBPRINT = "B2" * 20
TIMESTAMP_URL = "https://tsa.example.invalid/rfc3161"


MOCK_PREAMBLE = r"""
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$inputData = $env:TCLIENT_SIGN_TEST_INPUT | ConvertFrom-Json
. $inputData.script -Directory $inputData.stage -Mode Verify
$script:events = New-Object 'System.Collections.Generic.List[object]'
$script:signatureOverrides = @{}
$script:untrusted = @()
$script:toolFailure = ''
$script:mutateVendor = $false
$script:tsaThumb = 'CC' * 20
function New-TestCertificate {
    param([string]$Thumbprint = ('A1' * 20), [string]$Eku = '1.3.6.1.5.5.7.3.3',
        [bool]$HasKey = $true, [datetime]$NotBefore = [datetime]::UtcNow.AddDays(-1),
        [datetime]$NotAfter = [datetime]::UtcNow.AddDays(30))
    [pscustomobject]@{
        Thumbprint = $Thumbprint; HasPrivateKey = $HasKey; NotBefore = $NotBefore; NotAfter = $NotAfter
        Extensions = @([pscustomobject]@{
            Oid = [pscustomobject]@{ Value = '2.5.29.37' }
            EnhancedKeyUsages = @([pscustomobject]@{ Value = $Eku })
        })
    }
}
$script:certificates = @(New-TestCertificate)
function Get-ReleaseStoreCertificates {
    param([string]$Location)
    $script:events.Add([pscustomobject]@{ event = 'store'; location = $Location })
    $script:certificates
}
function Assert-ReleaseCertificateChain {
    param($Certificate)
    $script:events.Add([pscustomobject]@{ event = 'chain'; thumbprint = $Certificate.Thumbprint })
    if ($Certificate.Thumbprint -in $script:untrusted) { throw 'Mock untrusted certificate chain.' }
}
function Get-AuthenticodeSignature {
    [CmdletBinding()]
    param([string]$LiteralPath)
    $name = [System.IO.Path]::GetFileName($LiteralPath)
    $script:events.Add([pscustomobject]@{ event = 'signature'; name = $name })
    if ($script:signatureOverrides.ContainsKey($name)) { return $script:signatureOverrides[$name] }
    [pscustomobject]@{ Status = 'Valid'; SignatureType = 'Authenticode'
        SignerCertificate = (New-TestCertificate)
        TimeStamperCertificate = (New-TestCertificate -Thumbprint $script:tsaThumb -Eku '1.3.6.1.5.5.7.3.8') }
}
function New-TestSignature {
    param([string]$Status = 'Valid', [string]$Thumbprint = ('A1' * 20),
        [string]$SignerEku = '1.3.6.1.5.5.7.3.3', [string]$TimestampEku = '1.3.6.1.5.5.7.3.8',
        [bool]$Timestamp = $true, [string]$SignatureType = 'Authenticode', [bool]$Expired = $false)
    $certificate = New-TestCertificate -Thumbprint $Thumbprint -Eku $SignerEku
    if ($Expired) { $certificate.NotAfter = [datetime]::UtcNow.AddYears(-1) }
    $tsa = if ($Timestamp) {
        New-TestCertificate -Thumbprint $script:tsaThumb -Eku $TimestampEku -NotAfter ([datetime]::UtcNow.AddYears(-1))
    } else { $null }
    [pscustomobject]@{ Status = $Status; SignatureType = $SignatureType
        SignerCertificate = $certificate; TimeStamperCertificate = $tsa }
}
function Invoke-ReleaseSignTool {
    param([string]$Tool, [string[]]$Arguments)
    $script:events.Add([pscustomobject]@{ event = 'tool'; tool = $Tool; arguments = @($Arguments) })
    if ($Arguments[0] -eq $script:toolFailure) { throw 'Mock SignTool failure.' }
    if ($Arguments[0] -eq 'sign') {
        [System.IO.File]::AppendAllText($Arguments[-1], '|mock signed')
        if ($script:mutateVendor) {
            [System.IO.File]::AppendAllText((Join-Path $inputData.stage 'steam_api.dll'), '|tampered')
            $script:mutateVendor = $false
        }
    }
}
$options = @{ Directory = $inputData.stage; Mode = 'Verify' }
$extra = @{}
"""

MOCK_RESULT = r"""
$result = [ordered]@{ ok = $false; output = @(); error = ''; events = @(); extra = $extra }
try {
    $result.output = @(Invoke-WindowsSignRelease @options)
    $result.ok = $true
} catch { $result.error = $_.Exception.Message }
$result.events = @($script:events.ToArray())
$result | ConvertTo-Json -Depth 12 -Compress
"""


@unittest.skipUnless(os.name == "nt" and POWERSHELL, "Windows PowerShell is required")
class WindowsSignReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="tclient-authenticode-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.stage = self.root / "stage [literal] \u6d4b\u8bd5"
        self.stage.mkdir()
        for name in FIRST_PARTY + THIRD_PARTY + ("curl.dll", "DDNet-extra.exe"):
            (self.stage / name).write_bytes(("unsigned fixture " + name).encode("ascii"))
        self.tool = self.root / "tool [literal]" / "signtool.exe"
        self.tool.parent.mkdir()
        self.tool.write_bytes(b"never executed: mocked tool boundary")
        self.input = {"script": str(SCRIPT), "stage": str(self.stage), "tool": str(self.tool),
                      "report": str(self.root / "report [literal].json")}
        self.original = self.snapshot()

    def snapshot(self):
        return {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                for path in self.stage.iterdir() if path.is_file()}

    def powershell(self, code):
        import base64
        code = "$ProgressPreference = 'SilentlyContinue'\n" + code
        env = os.environ.copy()
        env["TCLIENT_SIGN_TEST_INPUT"] = json.dumps(self.input)
        result = subprocess.run(
            [POWERSHELL, "-NoProfile", "-NonInteractive", "-EncodedCommand",
             base64.b64encode(code.encode("utf-16le")).decode("ascii")],
            cwd=self.root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=35,
        )
        stdout = result.stdout.decode("utf-8-sig", errors="replace")
        stderr = result.stderr.decode("utf-8-sig", errors="replace")
        self.assertEqual(result.returncode, 0, stdout + stderr)
        self.assertEqual(stderr, "", stderr)
        return json.loads(stdout)

    def mock(self, setup="", *, sign=False):
        if sign:
            setup = r"""
$options.Mode = 'Sign'
$options.CertificateThumbprint = 'A1' * 20
$options.CertificateStoreLocation = 'CurrentUser'
$options.TimestampUrl = 'https://tsa.example.invalid/rfc3161'
$options.SignToolPath = $inputData.tool
""" + setup
        result = self.powershell(MOCK_PREAMBLE + setup + MOCK_RESULT)
        self.assertNotIn("PRIVATE KEY", result["error"] + "".join(result["output"]))
        return result

    def assert_no_tool(self, result):
        self.assertFalse(any(event["event"] == "tool" for event in result["events"]))

    def assert_failure(self, result, message):
        self.assertFalse(result["ok"], result)
        self.assertEqual(result["output"], [])
        self.assertIn(message, result["error"])

    def test_powershell_syntax_and_native_offline_chain_compilation(self):
        result = self.powershell(r"""
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$d = $env:TCLIENT_SIGN_TEST_INPUT | ConvertFrom-Json
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($d.script, [ref]$tokens, [ref]$errors)
if (@($errors).Count) { throw ($errors.Message -join '; ') }
$literal = $ast.Find({ param($node)
    $node -is [System.Management.Automation.Language.StringConstantExpressionAst] -and
        $node.Value.Contains('namespace TClientRelease {') }, $true)
if ($null -eq $literal) { throw 'Offline trust implementation missing.' }
Add-Type -TypeDefinition $literal.Value -ErrorAction Stop
# Read an existing public root certificate to exercise the native ABI offline.
# ReadOnly/OpenExistingOnly forbids creating a store; no certificates are added.
$store = New-Object System.Security.Cryptography.X509Certificates.X509Store('Root', 'LocalMachine')
try {
    $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadOnly -bor
        [System.Security.Cryptography.X509Certificates.OpenFlags]::OpenExistingOnly)
    $nativeTrusted = $false
    foreach ($certificate in $store.Certificates) {
        if ([TClientRelease.OfflineTrust]::IsTrusted($certificate)) { $nativeTrusted = $true; break }
    }
} finally { $store.Close() }
@{ syntaxErrors = @($errors).Count; nativeChainCompiled = ($null -ne ('TClientRelease.OfflineTrust' -as [type]));
    existingRootTrusted = $nativeTrusted } | ConvertTo-Json -Compress
""")
        self.assertEqual(result, {"syntaxErrors": 0, "nativeChainCompiled": True, "existingRootTrusted": True})

    def test_verify_deterministic_json_read_only_and_no_certificate_store(self):
        first = self.mock()
        second = self.mock()
        self.assertTrue(first["ok"], first)
        self.assertEqual(first["output"], second["output"])
        self.assertEqual(len(first["output"]), 1)
        report = json.loads(first["output"][0])
        self.assertEqual(report["mode"], "Verify")
        self.assertEqual(Path(report["directory"]), self.stage)
        self.assertTrue(report["verified"])
        self.assertEqual([file["name"] for file in report["files"]], list(FIRST_PARTY + THIRD_PARTY))
        for file in report["files"]:
            self.assertEqual(file["sha256"], self.original[file["name"]])
        self.assertEqual(self.snapshot(), self.original)
        self.assert_no_tool(first)
        self.assertFalse(any(event["event"] == "store" for event in first["events"]))
        self.assertFalse(any(event["event"] == "chain" for event in first["events"]))

    def test_every_exact_name_is_mandatory(self):
        for name in FIRST_PARTY + THIRD_PARTY:
            with self.subTest(name=name):
                path = self.stage / name
                content = path.read_bytes()
                path.unlink()
                try:
                    result = self.mock()
                    self.assert_failure(result, "Required release file missing: " + name)
                    self.assert_no_tool(result)
                finally:
                    path.write_bytes(content)

    def test_empty_file_or_wrong_case_is_rejected(self):
        (self.stage / FIRST_PARTY[0]).write_bytes(b"")
        self.assert_failure(self.mock(), "nonempty regular file")
        (self.stage / FIRST_PARTY[0]).write_bytes(b"fixture")
        (self.stage / FIRST_PARTY[0]).rename(self.stage / "ddnet.exe")
        self.assert_failure(self.mock(), "exact name: DDNet.exe")

    def test_reparse_point_rejected(self):
        target = self.root / "redirected"
        target.mkdir()
        junction = self.stage / "junction"
        result = self.powershell(MOCK_PREAMBLE + r"""
$junction = Join-Path $inputData.stage 'junction'
$target = Join-Path (Split-Path $inputData.stage -Parent) 'redirected'
$null = New-Item -ItemType Junction -Path $junction -Target $target
try {
    $item = Get-Item -LiteralPath $junction
    $errorText = ''
    try { Assert-ReleasePlainPath $item } catch { $errorText = $_.Exception.Message }
    @{ error = $errorText } | ConvertTo-Json -Compress
} finally { [System.IO.Directory]::Delete($junction) }
""")
        self.assertIn("reparse points", result["error"])
        self.assertTrue(target.is_dir())
        self.assertFalse(junction.exists())

    def test_verify_rejects_bad_signatures_and_aggregates_names(self):
        result = self.mock(r"""
$script:signatureOverrides['DDNet.exe'] = New-TestSignature -Status 'NotSigned'
$script:signatureOverrides['TClientUpdater.exe'] = New-TestSignature -Status 'HashMismatch'
$script:signatureOverrides['steam_api.dll'] = New-TestSignature -Status 'NotTrusted'
""")
        for name, status in [("DDNet.exe", "NotSigned"), ("TClientUpdater.exe", "HashMismatch"),
                             ("steam_api.dll", "NotTrusted")]:
            self.assert_failure(result, name + ": Authenticode status is " + status)
        self.assertEqual(len([event for event in result["events"] if event["event"] == "signature"]), 6)

    def test_verify_requires_timestamp_eku_embedded_signature_and_windows_trust(self):
        cases = [
            ("-Timestamp $false", "timestamp is required"),
            ("-SignerEku '1.3.6.1.5.5.7.3.1'", "EKU 1.3.6.1.5.5.7.3.3"),
            ("-TimestampEku '1.3.6.1.5.5.7.3.3'", "EKU 1.3.6.1.5.5.7.3.8"),
            ("-SignatureType Catalog", "embedded Authenticode"),
        ]
        for options, message in cases:
            with self.subTest(options=options):
                result = self.mock("$script:signatureOverrides['DDNet.exe'] = New-TestSignature " + options)
                self.assert_failure(result, message)
        result = self.mock("$script:signatureOverrides['DDNet.exe'] = New-TestSignature -Status NotTrusted")
        self.assert_failure(result, "Authenticode status is NotTrusted")

    def test_verify_vendor_valid_is_authoritative_without_extra_timestamp_or_eku_policy(self):
        result = self.mock(r"""
foreach ($name in @('steam_api.dll', 'discord_game_sdk.dll', 'vulkan-1.dll')) {
    $script:signatureOverrides[$name] = New-TestSignature -Timestamp $false -SignerEku '1.3.6.1.5.5.7.3.1' -Expired $true
}
$script:untrusted = @($script:tsaThumb)
""")
        self.assertTrue(result["ok"], result)
        self.assertFalse(any(event["event"] == "chain" for event in result["events"]))
        report = json.loads(result["output"][0])
        for file in report["files"][3:]:
            self.assertIsNone(file["timestampSignerThumbprint"])

    def test_verify_historical_expired_signer_is_valid_when_authenticode_valid(self):
        result = self.mock(r"""
$script:signatureOverrides['discord_game_sdk.dll'] = New-TestSignature -Expired $true
""")
        self.assertTrue(result["ok"], result)

    def test_verify_first_party_publishers_must_match(self):
        result = self.mock("$script:signatureOverrides['TClientUpdater.exe'] = New-TestSignature -Thumbprint ('B2' * 20)")
        self.assert_failure(result, "same signer thumbprint")

    def test_verify_optional_thumbprint_pin(self):
        result = self.mock("$options.CertificateThumbprint = ('a1 ' * 20)")
        self.assertTrue(result["ok"], result)
        self.assertFalse(any(event["event"] == "store" for event in result["events"]))
        result = self.mock("$options.CertificateThumbprint = 'B2' * 20")
        self.assert_failure(result, "explicitly selected certificate")
        self.assert_failure(self.mock("$options.CertificateThumbprint = 'invalid'"), "40 hex digits")

    def test_verify_rejects_sign_only_options(self):
        for option, value in [("CertificateStoreLocation", "CurrentUser"),
                              ("TimestampUrl", TIMESTAMP_URL), ("SignToolPath", "unused.exe")]:
            with self.subTest(option=option):
                self.assert_failure(self.mock(f"$options.{option} = '{value}'"), "only accepted in Sign mode")

    def test_sign_requires_explicit_parameters_before_tool(self):
        cases = [
            ("$options.Remove('CertificateThumbprint')", "explicit 40-hex-digit"),
            ("$options.CertificateThumbprint = 'bad'", "explicit 40-hex-digit"),
            ("$options.Remove('CertificateStoreLocation')", "requires CertificateStoreLocation"),
            ("$options.Remove('TimestampUrl')", "HTTPS TimestampUrl"),
            ("$options.TimestampUrl = 'http://tsa.example.invalid'", "HTTPS TimestampUrl"),
            ("$options.TimestampUrl = 'https://user:pass@tsa.example.invalid'", "HTTPS TimestampUrl"),
            ("$options.TimestampUrl = 'https://tsa.example.invalid/?token=secret'", "HTTPS TimestampUrl"),
            ("$options.TimestampUrl = 'https://tsa.example.invalid/#part'", "HTTPS TimestampUrl"),
        ]
        for setup, message in cases:
            with self.subTest(setup=setup):
                result = self.mock("$null = " + setup if ".Remove(" in setup else setup, sign=True)
                self.assert_failure(result, message)
                self.assert_no_tool(result)
                self.assertEqual(self.snapshot(), self.original)

    def test_sign_certificate_preflight_rejects_before_any_tool(self):
        cases = [
            ("$script:certificates = @()", "exactly one certificate"),
            ("$script:certificates = @(New-TestCertificate -Thumbprint ('B2' * 20))", "exactly one certificate"),
            ("$script:certificates = @((New-TestCertificate), (New-TestCertificate))", "exactly one certificate"),
            ("$script:certificates = @(New-TestCertificate -HasKey $false)", "no private key"),
            ("$script:certificates = @(New-TestCertificate -NotAfter ([datetime]::UtcNow.AddDays(-1)))", "expired or not yet valid"),
            ("$script:certificates = @(New-TestCertificate -NotBefore ([datetime]::UtcNow.AddDays(1)))", "expired or not yet valid"),
            ("$script:certificates = @(New-TestCertificate -Eku '1.3.6.1.5.5.7.3.1')", "required explicit EKU"),
            ("$script:untrusted = @('A1' * 20)", "Mock untrusted certificate chain"),
        ]
        for setup, message in cases:
            with self.subTest(setup=setup):
                result = self.mock(setup, sign=True)
                self.assert_failure(result, message)
                self.assert_no_tool(result)
                self.assertEqual(self.snapshot(), self.original)

    def test_sign_exact_sequence_arguments_reverification_and_vendor_invariant(self):
        for location in ["CurrentUser", "LocalMachine"]:
            with self.subTest(location=location):
                before = self.snapshot()
                result = self.mock(f"$options.CertificateStoreLocation = '{location}'", sign=True)
                self.assertTrue(result["ok"], result)
                tool_events = [event for event in result["events"] if event["event"] == "tool"]
                self.assertEqual(len(tool_events), 6)
                for name, sign_event, verify_event in zip(FIRST_PARTY, tool_events[::2], tool_events[1::2]):
                    expected = ["sign", "/s", "My", "/sha1", THUMBPRINT]
                    if location == "LocalMachine":
                        expected += ["/sm"]
                    expected += ["/fd", "SHA256", "/tr", TIMESTAMP_URL, "/td", "SHA256", str(self.stage / name)]
                    self.assertEqual(sign_event["arguments"], expected)
                    self.assertEqual(verify_event["arguments"], ["verify", "/pa", "/all", "/tw", str(self.stage / name)])
                    self.assertNotIn("/a", sign_event["arguments"])
                self.assertEqual(result["events"][0], {"event": "store", "location": location})
                report = json.loads(result["output"][0])
                self.assertEqual(report["mode"], "Sign")
                signature_names = [event["name"] for event in result["events"] if event["event"] == "signature"]
                self.assertEqual(signature_names, list(THIRD_PARTY + FIRST_PARTY + THIRD_PARTY))
                after = self.snapshot()
                for name in before:
                    self.assertEqual(before[name] != after[name], name in FIRST_PARTY, name)

    def test_sign_rejects_unsigned_vendor_before_tool(self):
        result = self.mock("$script:signatureOverrides['steam_api.dll'] = New-TestSignature -Status NotSigned", sign=True)
        self.assert_failure(result, "steam_api.dll: Authenticode status is NotSigned")
        self.assert_no_tool(result)
        self.assertEqual(self.snapshot(), self.original)

    def test_sign_requires_complete_stage_before_tool(self):
        (self.stage / "island_media_monitor.exe").unlink()
        result = self.mock(sign=True)
        self.assert_failure(result, "Required release file missing: island_media_monitor.exe")
        self.assert_no_tool(result)

    def test_sign_native_warning_or_error_never_succeeds(self):
        # Exercise the real native wrapper via the benign Windows cmd.exe, never SignTool.
        result = self.powershell(r"""
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$d = $env:TCLIENT_SIGN_TEST_INPUT | ConvertFrom-Json
. $d.script -Directory $d.stage -Mode Verify
$results = @()
foreach ($exitCode in @(0, 1, 2)) {
    $errorText = ''
    $output = @()
    try { $output = @(Invoke-ReleaseSignTool -Tool $env:ComSpec -Arguments @('/d', '/c', ('exit ' + $exitCode))) }
    catch { $errorText = $_.Exception.Message }
    $results += @{ exit = $exitCode; error = $errorText; output = $output }
}
ConvertTo-Json -InputObject @($results) -Compress
""")
        self.assertEqual(result[0]["error"], "")
        for item in result:
            self.assertEqual(item["output"], [])
            if item["exit"]:
                self.assertIn("exit " + str(item["exit"]), item["error"])

    def test_sign_stops_on_sign_or_verify_failure(self):
        for operation, count in [("sign", 1), ("verify", 2)]:
            with self.subTest(operation=operation):
                result = self.mock(f"$script:toolFailure = '{operation}'", sign=True)
                self.assert_failure(result, "Mock SignTool failure")
                self.assertEqual(len([event for event in result["events"] if event["event"] == "tool"]), count)

    def test_sign_postverification_rejects_wrong_signer_missing_timestamp_and_vendor_tampering(self):
        cases = [
            ("$script:signatureOverrides['DDNet.exe'] = New-TestSignature -Thumbprint ('B2' * 20)", "explicitly selected certificate"),
            ("$script:signatureOverrides['DDNet.exe'] = New-TestSignature -Timestamp $false", "timestamp is required"),
            ("$script:mutateVendor = $true", "Third-party bytes changed during signing: steam_api.dll"),
        ]
        for setup, message in cases:
            with self.subTest(setup=setup):
                result = self.mock(setup, sign=True)
                self.assert_failure(result, message)

    def test_report_is_new_utf8_json_outside_stage_and_stdout_empty(self):
        result = self.mock("$options.ReportPath = $inputData.report")
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["output"], [])
        path = Path(self.input["report"])
        self.assertFalse(path.read_bytes().startswith(b"\xef\xbb\xbf"))
        self.assertTrue(json.loads(path.read_bytes())["verified"])
        self.assertEqual(self.snapshot(), self.original)
        result = self.mock("$options.ReportPath = $inputData.report")
        self.assert_failure(result, "refusing to overwrite")
        result = self.mock("$options.ReportPath = (Join-Path $inputData.stage 'report.json')")
        self.assert_failure(result, "outside the release stage")

    def test_report_not_written_after_failure(self):
        result = self.mock(r"""
$options.ReportPath = $inputData.report
$script:signatureOverrides['DDNet.exe'] = New-TestSignature -Status NotSigned
""")
        self.assert_failure(result, "NotSigned")
        self.assertFalse(Path(self.input["report"]).exists())

    def test_real_cli_rejects_unsigned_fixture_without_writing_or_stdout(self):
        result = subprocess.run(
            [POWERSHELL, "-NoProfile", "-NonInteractive", "-File", str(SCRIPT),
             "-Directory", str(self.stage), "-Mode", "Verify"],
            cwd=self.root, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=35,
        )
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, b"")
        for name in FIRST_PARTY + THIRD_PARTY:
            self.assertIn(name, result.stderr.decode("utf-8", errors="replace"))
        self.assertEqual(self.snapshot(), self.original)

    def test_real_missing_certificate_sign_refuses_without_timestamp_request(self):
        # This explicit impossible thumbprint is not a certificate. Selection must fail
        # before any executable or HTTPS URL can be used; .invalid never gets requested.
        result = subprocess.run(
            [POWERSHELL, "-NoProfile", "-NonInteractive", "-File", str(SCRIPT),
             "-Directory", str(self.stage), "-Mode", "Sign",
             "-CertificateThumbprint", "0" * 40, "-CertificateStoreLocation", "CurrentUser",
             "-TimestampUrl", TIMESTAMP_URL, "-SignToolPath", str(self.tool)],
            cwd=self.root, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=35,
        )
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, b"")
        self.assertIn("exactly one certificate", result.stderr.decode("utf-8", errors="replace"))
        self.assertEqual(self.snapshot(), self.original)


if __name__ == "__main__":
    unittest.main(verbosity=2)
