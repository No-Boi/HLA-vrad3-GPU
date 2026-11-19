# Builds the optional OpenImageDenoise.dll shim into the dist oidn-shim folder.
param(
    [string]$VisualStudio = ''    # blank = find automatically
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'BuildTools.ps1')
$VisualStudio = Resolve-VisualStudio $VisualStudio
$taskRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$buildDir = Join-Path $taskRoot 'artifacts\oidn-shim-build'
$dist = Join-Path $taskRoot 'dist\oidn-shim'
New-Item -ItemType Directory -Force -Path $buildDir,$dist | Out-Null
$vcvars = Join-Path $VisualStudio 'VC\Auxiliary\Build\vcvars64.bat'
$source = Join-Path $PSScriptRoot 'oidn_shim.cpp'
$batch = Join-Path $buildDir 'build-oidn-shim.cmd'
@"
@echo off
call "$vcvars"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /LD /Brepro /d1trimfile:`"$PSScriptRoot\\`" "$source" /Fe:OpenImageDenoise.dll /link /NOLOGO /Brepro
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $batch -Encoding ascii
Push-Location $buildDir
try {
    & cmd.exe /c $batch
    if ($LASTEXITCODE -ne 0) { throw 'OIDN shim build failed.' }
} finally { Pop-Location }
Copy-Item -Force (Join-Path $buildDir 'OpenImageDenoise.dll') $dist
