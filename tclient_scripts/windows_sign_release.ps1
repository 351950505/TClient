#requires -Version 5.1
<#
.SYNOPSIS
Strict Authenticode gate for a complete Windows release stage.
.DESCRIPTION
Verify is read-only (except an explicitly requested new report outside the stage).
All six exact top-level names below are mandatory. No recursive or wildcard signing.
Sign requires an explicit SHA-1 certificate thumbprint, My store location and HTTPS
RFC 3161 timestamp URL. It overwrites signatures only on the three first-party EXEs.
Third-party signatures and SHA-256 bytes are checked before and after signing.

Windows Authenticode Valid is authoritative for signature and timestamp trust under
Windows' configured trust/revocation policy. First-party files also need an embedded
signature, explicit code signing EKU and a timestamp certificate with time stamping
EKU. Third-party Valid signatures keep their publisher's timestamp/expiry policy.
The Sign certificate preflight uses existing local trust without revocation retrieval,
AIA downloads or automatic root installation; an incomplete local chain fails closed.
No certificate is created, imported, exported or added to any trust store.

Sign is in-place: a later failure can leave earlier EXEs signed. Use a disposable
stage, discard it on any failure, and package only after exit 0. ReportPath must be
a new file in an existing directory outside the stage. Only success writes a report;
without ReportPath, stdout is exactly one compact JSON object. Errors use stderr.
.EXAMPLE
powershell.exe -NoProfile -NonInteractive -File windows_sign_release.ps1 -Directory D:/stage -Mode Verify
.EXAMPLE
powershell.exe -NoProfile -NonInteractive -File windows_sign_release.ps1 -Directory D:/stage -Mode Sign -CertificateThumbprint <40-hex-digits> -CertificateStoreLocation CurrentUser -TimestampUrl https://tsa.example.invalid -SignToolPath 'C:/SDK/signtool.exe'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$Directory,
    [Parameter(Mandatory = $true)][ValidateSet('Verify', 'Sign')][string]$Mode,
    [string]$CertificateThumbprint,
    [ValidateSet('CurrentUser', 'LocalMachine')][string]$CertificateStoreLocation,
    [string]$TimestampUrl,
    [string]$SignToolPath,
    [string]$ReportPath
)

function Assert-ReleasePlainPath {
    param([System.IO.FileSystemInfo]$Item)
    $current = $Item
    while ($null -ne $current) {
        if (($current.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw 'Release paths must not traverse reparse points.'
        }
        if ($current -is [System.IO.DirectoryInfo]) { $current = $current.Parent }
        else { $current = $current.Directory }
    }
}

function Get-ReleaseFiles {
    param([string]$Stage)
    $root = Get-Item -LiteralPath $Stage -Force -ErrorAction Stop
    if ($root.PSProvider.Name -ne 'FileSystem' -or -not $root.PSIsContainer) {
        throw 'Directory must be an existing filesystem directory.'
    }
    Assert-ReleasePlainPath $root
    $files = @()
    $children = @(Get-ChildItem -LiteralPath $root.FullName -Force -ErrorAction Stop)
    foreach ($role in 'firstParty', 'thirdParty') {
        $names = if ($role -eq 'firstParty') {
            @('DDNet.exe', 'TClientUpdater.exe', 'island_media_monitor.exe')
        } else { @('steam_api.dll', 'discord_game_sdk.dll', 'vulkan-1.dll') }
        foreach ($name in $names) {
            $path = Join-Path -Path $root.FullName -ChildPath $name
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                throw "Required release file missing: $name"
            }
            $exact = @($children | Where-Object { $_.Name -ceq $name })
            if ($exact.Count -ne 1) { throw "Required release file must have exact name: $name" }
            $item = $exact[0]
            if ($item -isnot [System.IO.FileInfo] -or $item.Length -le 0) {
                throw "Required release file must be a nonempty regular file with exact name: $name"
            }
            Assert-ReleasePlainPath $item
            $files += [pscustomobject]@{ name = $name; role = $role; path = $item.FullName }
        }
    }
    return [pscustomobject]@{ directory = $root.FullName; files = $files }
}

function Assert-ReleaseEku {
    param($Certificate, [string]$Oid)
    if ($null -eq $Certificate) { throw 'Required signing certificate is missing.' }
    foreach ($extension in $Certificate.Extensions) {
        if ($extension.Oid.Value -eq '2.5.29.37') {
            foreach ($usage in $extension.EnhancedKeyUsages) {
                if ($usage.Value -eq $Oid) { return }
            }
        }
    }
    throw "Certificate lacks required explicit EKU $Oid."
}

function Assert-ReleaseCertificateChain {
    param($Certificate)
    if ($null -eq ('TClientRelease.OfflineTrust' -as [type])) {
        # CertGetCertificateChain flags are from the locally installed Windows SDK.
        # X509Chain on Windows PowerShell 5.1 cannot disable AIA/root auto-updates.
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Cryptography.X509Certificates;
namespace TClientRelease {
    public static class OfflineTrust {
        [StructLayout(LayoutKind.Sequential)] struct Usage {
            public uint Count; public IntPtr Oids;
        }
        [StructLayout(LayoutKind.Sequential)] struct UsageMatch {
            public uint Type; public Usage Usage;
        }
        [StructLayout(LayoutKind.Sequential)] struct ChainParameters {
            public uint Size; public UsageMatch RequestedUsage;
        }
        [StructLayout(LayoutKind.Sequential)] struct TrustStatus {
            public uint Errors; public uint Information;
        }
        [StructLayout(LayoutKind.Sequential)] struct ChainPrefix {
            public uint Size; public TrustStatus Status;
        }
        [DllImport("crypt32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool CertGetCertificateChain(IntPtr engine, IntPtr certificate,
            IntPtr time, IntPtr store, ref ChainParameters parameters,
            uint flags, IntPtr reserved, out IntPtr chain);
        [DllImport("crypt32.dll")]
        static extern void CertFreeCertificateChain(IntPtr chain);
        public static bool IsTrusted(X509Certificate2 certificate) {
            var parameters = new ChainParameters();
            parameters.Size = (uint)Marshal.SizeOf(typeof(ChainParameters));
            IntPtr chain;
            // CACHE_ONLY_URL_RETRIEVAL | DISABLE_AUTH_ROOT_AUTO_UPDATE | DISABLE_AIA.
            if (!CertGetCertificateChain(IntPtr.Zero, certificate.Handle, IntPtr.Zero,
                IntPtr.Zero, ref parameters, 0x00002104, IntPtr.Zero, out chain))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            try {
                var context = (ChainPrefix)Marshal.PtrToStructure(chain, typeof(ChainPrefix));
                return context.Status.Errors == 0;
            } finally { CertFreeCertificateChain(chain); }
        }
    }
}
'@ -ErrorAction Stop
    }
    if (-not [TClientRelease.OfflineTrust]::IsTrusted($Certificate)) {
        throw 'Certificate chain is not trusted by existing local trust (offline, no revocation retrieval).'
    }
}

function Get-ReleaseStoreCertificates {
    param([string]$Location)
    $store = New-Object System.Security.Cryptography.X509Certificates.X509Store(
        'My', [System.Security.Cryptography.X509Certificates.StoreLocation]::$Location)
    try {
        $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadOnly -bor
            [System.Security.Cryptography.X509Certificates.OpenFlags]::OpenExistingOnly)
        foreach ($certificate in $store.Certificates) { $certificate }
    } finally { $store.Close() }
}

function Get-ReleaseSigningCertificate {
    param([string]$Thumbprint, [string]$Location)
    $matching = @(Get-ReleaseStoreCertificates $Location | Where-Object {
        $_.Thumbprint -eq $Thumbprint
    })
    if ($matching.Count -ne 1) {
        throw 'Explicit certificate thumbprint must match exactly one certificate in the selected My store.'
    }
    $certificate = $matching[0]
    if (-not $certificate.HasPrivateKey) { throw 'Selected certificate has no private key.' }
    $now = [datetime]::UtcNow
    if ($now -lt $certificate.NotBefore.ToUniversalTime() -or $now -gt $certificate.NotAfter.ToUniversalTime()) {
        throw 'Selected signing certificate is expired or not yet valid.'
    }
    Assert-ReleaseEku $certificate '1.3.6.1.5.5.7.3.3'
    Assert-ReleaseCertificateChain $certificate
    return $certificate
}

function Get-ReleaseVerification {
    param([object[]]$Files, [string]$ExpectedThumbprint)
    $reports = @()
    $failures = @()
    foreach ($file in $Files) {
        try {
            $signature = Get-AuthenticodeSignature -LiteralPath $file.path -ErrorAction Stop
            if ([string]$signature.Status -ne 'Valid') { throw "Authenticode status is $($signature.Status)." }
            if ($file.role -eq 'firstParty') {
                if ([string]$signature.SignatureType -ne 'Authenticode') { throw 'An embedded Authenticode signature is required.' }
                Assert-ReleaseEku $signature.SignerCertificate '1.3.6.1.5.5.7.3.3'
                if ($ExpectedThumbprint -and $signature.SignerCertificate.Thumbprint -ne $ExpectedThumbprint) {
                    throw 'Signed first-party file does not match the explicitly selected certificate.'
                }
                if ($null -eq $signature.TimeStamperCertificate) { throw 'A trusted Authenticode timestamp is required.' }
                Assert-ReleaseEku $signature.TimeStamperCertificate '1.3.6.1.5.5.7.3.8'
                # Windows WinVerifyTrust (Status Valid) validates timestamp trust and
                # historical validity. A separate offline TSA chain can lack embedded
                # intermediates and incorrectly reject a legitimate timestamp.
            }
            $hash = (Get-FileHash -LiteralPath $file.path -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
            $reports += [pscustomobject][ordered]@{
                name = $file.name; role = $file.role; status = 'Valid'; sha256 = $hash
                signerThumbprint = $(if ($signature.SignerCertificate) { $signature.SignerCertificate.Thumbprint } else { $null })
                timestampSignerThumbprint = $(if ($signature.TimeStamperCertificate) { $signature.TimeStamperCertificate.Thumbprint } else { $null })
            }
        } catch { $failures += "$($file.name): $($_.Exception.Message)" }
    }
    if ($failures.Count -gt 0) { throw ($failures -join [Environment]::NewLine) }
    return $reports
}

function Resolve-ReleaseSignTool {
    param([string]$Path)
    if (-not $Path) {
        $kits = Join-Path -Path ${env:ProgramFiles(x86)} -ChildPath 'Windows Kits/10/bin'
        $candidates = @()
        if (Test-Path -LiteralPath $kits -PathType Container) {
            $candidates = @(Get-ChildItem -LiteralPath $kits -Directory | ForEach-Object {
                $version = $null
                if ([version]::TryParse($_.Name, [ref]$version)) {
                    $candidate = Join-Path -Path $_.FullName -ChildPath 'x64/signtool.exe'
                    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                        [pscustomobject]@{ version = $version; path = $candidate }
                    }
                }
            } | Sort-Object -Property version -Descending)
        }
        if ($candidates.Count -eq 0) { throw 'SignTool not found in the Windows SDK; supply SignToolPath explicitly.' }
        $Path = $candidates[0].path
    }
    $tool = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($tool -isnot [System.IO.FileInfo] -or $tool.Extension -ine '.exe') {
        throw 'SignToolPath must be an existing executable file.'
    }
    Assert-ReleasePlainPath $tool
    return $tool.FullName
}

function Invoke-ReleaseSignTool {
    param([string]$Tool, [string[]]$Arguments)
    # Capture native diagnostics: never mix SignTool text/URLs into the JSON stream.
    $ErrorActionPreference = 'Continue'
    $PSNativeCommandUseErrorActionPreference = $false
    $global:LASTEXITCODE = $null
    & $Tool @Arguments 2>&1 | Out-Null
    $toolExitCode = $global:LASTEXITCODE
    if ($null -eq $toolExitCode) { throw 'SignTool could not be executed.' }
    if ($toolExitCode -ne 0) { throw "SignTool $($Arguments[0]) failed (exit $toolExitCode)." }
}

function Resolve-ReleaseReportPath {
    param([string]$Path, [string]$Stage)
    if (-not $Path) { return $null }
    $full = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    $full = [System.IO.Path]::GetFullPath($full)
    $prefix = $Stage.TrimEnd([char[]]'\/') + [System.IO.Path]::DirectorySeparatorChar
    if ($full.Equals($Stage, [StringComparison]::OrdinalIgnoreCase) -or
        $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'ReportPath must be outside the release stage.'
    }
    if (Test-Path -LiteralPath $full) { throw 'ReportPath must be a new file; refusing to overwrite an existing path.' }
    $parent = Get-Item -LiteralPath ([System.IO.Path]::GetDirectoryName($full)) -Force -ErrorAction Stop
    if ($parent.PSProvider.Name -ne 'FileSystem' -or -not $parent.PSIsContainer) {
        throw 'ReportPath parent must be an existing filesystem directory.'
    }
    Assert-ReleasePlainPath $parent
    return $full
}

function Invoke-WindowsSignRelease {
    [CmdletBinding()]
    param(
        [string]$Directory, [string]$Mode, [string]$CertificateThumbprint,
        [string]$CertificateStoreLocation, [string]$TimestampUrl,
        [string]$SignToolPath, [string]$ReportPath
    )
    $ErrorActionPreference = 'Stop'
    $ProgressPreference = 'SilentlyContinue'
    Set-StrictMode -Version 3.0
    [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
    [Console]::Error.Write('')
    if ($env:OS -ne 'Windows_NT') { throw 'This release gate requires Windows Authenticode.' }
    if ($Mode -notin @('Sign', 'Verify')) { throw 'Mode must be Sign or Verify.' }
    $thumbprint = $null
    if ($Mode -eq 'Sign') {
        $thumbprint = ($CertificateThumbprint -replace '\s', '').ToUpperInvariant()
        if ($thumbprint -notmatch '\A[0-9A-F]{40}\z') { throw 'Sign requires an explicit 40-hex-digit CertificateThumbprint.' }
        if ($CertificateStoreLocation -notin @('CurrentUser', 'LocalMachine')) {
            throw 'Sign requires CertificateStoreLocation CurrentUser or LocalMachine.'
        }
        $uri = $null
        if (-not [uri]::TryCreate($TimestampUrl, [UriKind]::Absolute, [ref]$uri) -or
            $uri.Scheme -ne 'https' -or -not $uri.Host -or $uri.UserInfo -or $uri.Query -or $uri.Fragment -or
            $TimestampUrl -match '[\s"\x00-\x1F]') {
            throw 'Sign requires an absolute HTTPS TimestampUrl without credentials, query, fragment or whitespace.'
        }
        # No tool invocation (and no timestamp request) before exact certificate validation.
        $null = Get-ReleaseSigningCertificate $thumbprint $CertificateStoreLocation
    } elseif ($CertificateStoreLocation -or $TimestampUrl -or $SignToolPath) {
        throw 'Store, timestamp and SignTool options are only accepted in Sign mode.'
    } elseif ($CertificateThumbprint) {
        $thumbprint = ($CertificateThumbprint -replace '\s', '').ToUpperInvariant()
        if ($thumbprint -notmatch '\A[0-9A-F]{40}\z') { throw 'CertificateThumbprint must contain exactly 40 hex digits.' }
    }
    $stage = Get-ReleaseFiles $Directory
    $reportFile = Resolve-ReleaseReportPath $ReportPath $stage.directory
    if ($Mode -eq 'Sign') {
        $tool = Resolve-ReleaseSignTool $SignToolPath
        $vendors = @($stage.files | Where-Object { $_.role -eq 'thirdParty' })
        $before = @(Get-ReleaseVerification $vendors)
        foreach ($file in $stage.files | Where-Object { $_.role -eq 'firstParty' }) {
            $signArguments = @('sign', '/s', 'My', '/sha1', $thumbprint)
            if ($CertificateStoreLocation -eq 'LocalMachine') { $signArguments += '/sm' }
            $signArguments += @('/fd', 'SHA256', '/tr', $TimestampUrl, '/td', 'SHA256', $file.path)
            Invoke-ReleaseSignTool -Tool $tool -Arguments $signArguments
            Invoke-ReleaseSignTool -Tool $tool -Arguments @('verify', '/pa', '/all', '/tw', $file.path)
        }
    }
    $files = @(Get-ReleaseVerification -Files $stage.files -ExpectedThumbprint $thumbprint)
    $firstPartySigners = @($files | Where-Object { $_.role -eq 'firstParty' } |
        Select-Object -ExpandProperty signerThumbprint -Unique)
    if ($firstPartySigners.Count -ne 1) { throw 'First-party release files must share the same signer thumbprint.' }
    if ($Mode -eq 'Sign') {
        foreach ($original in $before) {
            $after = @($files | Where-Object { $_.name -ceq $original.name })
            if ($after.Count -ne 1 -or $after[0].sha256 -cne $original.sha256) {
                throw "Third-party bytes changed during signing: $($original.name)"
            }
        }
    }
    $json = [pscustomobject][ordered]@{
        schemaVersion = 1; mode = $Mode; verified = $true; directory = $stage.directory
        timestampPolicy = 'Windows-Authenticode-first-party-timestamp-required'; files = $files
    } | ConvertTo-Json -Depth 5 -Compress
    if ($reportFile) {
        $stream = [System.IO.File]::Open($reportFile, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write)
        try {
            $bytes = (New-Object System.Text.UTF8Encoding($false)).GetBytes($json + [Environment]::NewLine)
            $stream.Write($bytes, 0, $bytes.Length)
        } finally { $stream.Dispose() }
    } else { Write-Output $json }
}

# Dot-sourcing loads functions for dependency-free boundary-mocked tests only.
# The production CLI has no mock/skip-trust switches.
if ($MyInvocation.InvocationName -ne '.') {
    try {
        Invoke-WindowsSignRelease @PSBoundParameters
        exit 0
    } catch {
        [Console]::Error.WriteLine($_.Exception.Message)
        exit 1
    }
}
