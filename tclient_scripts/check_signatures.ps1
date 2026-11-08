param([Parameter(Mandatory=$true)][string]$Directory)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$root = Get-Item -LiteralPath $Directory -Force
if(-not $root.PSIsContainer -or ($root.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Signature directory must be a regular directory' }
$result = @()
foreach($name in @('DDNet.exe','TClientUpdater.exe','island_media_monitor.exe','steam_api.dll','discord_game_sdk.dll','vulkan-1.dll')) {
    $path = Join-Path $Directory $name
    $item = Get-Item -LiteralPath $path -Force
    if($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $item.Length -eq 0) { throw "Not a nonempty regular file: $name" }
    $sig = Get-AuthenticodeSignature -LiteralPath $path
    $expected = if($name.EndsWith('.dll')) {'Valid'} else {'NotSigned'}
    if([string]$sig.Status -ne $expected) { throw "Unexpected signature status: $name $($sig.Status)" }
    $result += [PSCustomObject]@{name=$name;status=[string]$sig.Status;signer=$(if($sig.SignerCertificate){$sig.SignerCertificate.Subject}else{$null});sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
}
ConvertTo-Json -InputObject $result -Depth 3 -Compress
