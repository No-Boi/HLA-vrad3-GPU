# Packages dist\ into artifacts\release: the download zip, SHA256SUMS.txt and the release notes.
# Run after Build.cmd.
param(
    [string]$Commit = $env:GITHUB_SHA   # blank = ask git
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'BuildTools.ps1')
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root 'dist'
$out = Join-Path $root 'artifacts\release'
$version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim()
if (-not $Commit) { $Commit = (& git -C $root rev-parse HEAD 2>$null) }
$name = "vrad3-gpu-$version"

$files = 'vrad3.exe', 'vrad2.exe', 'vrad3_settings.exe', 'vrad3_gpu.example.ini', 'Setup.cmd', 'Setup.ps1', 'oidn-shim\OpenImageDenoise.dll'
foreach ($file in $files) {
    if (-not (Test-Path -LiteralPath (Join-Path $dist $file))) { throw "dist\$file is missing: build first" }
}

if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
$stage = Join-Path $out $name
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'oidn-shim') | Out-Null
foreach ($file in $files) { Copy-Item -LiteralPath (Join-Path $dist $file) -Destination (Join-Path $stage $file) }
$zip = Join-Path $out "$name.zip"
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Copy-Item -LiteralPath (Join-Path $dist 'vrad3.map') -Destination (Join-Path $out "$name.map")

function Sum([string]$path, [string]$label) { '{0}  {1}' -f (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant(), $label }
$sums = @()
foreach ($file in $files) { $sums += Sum (Join-Path $stage $file) ($file.Replace('\', '/')) }
$sums += Sum $zip "$name.zip"
$sums += Sum (Join-Path $out "$name.map") "$name.map"
$sums | Set-Content -LiteralPath (Join-Path $out 'SHA256SUMS.txt') -Encoding ascii

$vs = Resolve-VisualStudio ''
$msvc = (Get-Content -LiteralPath (Join-Path $vs 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
$vsVersion = & (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe') -path $vs -property catalog_productDisplayVersion
$dxc = Resolve-Dxc ''
$sdk = Split-Path -Leaf (Split-Path -Parent (Split-Path -Parent $dxc))
@"
GPU vrad3 $version

Built from commit $Commit with Visual Studio $vsVersion (MSVC $msvc) and Windows SDK $sdk.
To check the exe matches the source, build that commit with the same versions and compare the hashes.

SHA-256 of the files in the zip and of the release files:

``````
$($sums -join "`n")
``````

``$name.map`` is for reading crash reports, not needed to run anything.
"@ | Set-Content -LiteralPath (Join-Path $out 'notes.md') -Encoding utf8
Write-Host "Packaged $zip"
