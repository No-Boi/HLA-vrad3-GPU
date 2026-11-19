# Shared build helpers: locate Visual Studio C++ tools (vcvars64.bat) and the DirectX shader compiler (dxc.exe).
function Resolve-VisualStudio([string]$Requested) {
    if ($Requested) {
        if (Test-Path -LiteralPath (Join-Path $Requested 'VC\Auxiliary\Build\vcvars64.bat')) { return $Requested }
        throw "No vcvars64.bat under '$Requested\VC\Auxiliary\Build'. Pass the Visual Studio installation folder (the one containing 'VC')."
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $found = & $vswhere -latest -products * -version '[16.0,' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null |
            Select-Object -First 1
        if ($found -and (Test-Path -LiteralPath (Join-Path $found 'VC\Auxiliary\Build\vcvars64.bat'))) { return $found }
    }
    throw @"
Could not find Visual Studio with the C++ x64 build tools.
Install either:
  - Visual Studio 2022 (any edition, e.g. the free Community edition) with the
    'Desktop development with C++' workload, or
  - Build Tools for Visual Studio 2022 with 'Desktop development with C++'.
Or pass the folder explicitly: -VisualStudio "C:\path\to\Visual Studio"
"@
}

function Resolve-Dxc([string]$Requested) {
    if ($Requested) {
        if (Test-Path -LiteralPath $Requested) { return $Requested }
        throw "dxc.exe not found at '$Requested'."
    }
    $roots = New-Object System.Collections.Generic.List[string]
    foreach ($key in 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots',
                     'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots') {
        try {
            $root = (Get-ItemProperty -LiteralPath $key -ErrorAction Stop).KitsRoot10
            if ($root) { $roots.Add($root) }
        } catch { }
    }
    $roots.Add((Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'))
    foreach ($root in ($roots | Select-Object -Unique)) {
        $bin = Join-Path $root 'bin'
        if (-not (Test-Path -LiteralPath $bin)) { continue }
        $versions = Get-ChildItem -LiteralPath $bin -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' -and [version]$_.Name -ge [version]'10.0.19041.0' } |
            Sort-Object { [version]$_.Name } -Descending
        foreach ($v in $versions) {
            $candidate = Join-Path $v.FullName 'x64\dxc.exe'
            if (Test-Path -LiteralPath $candidate) { return $candidate }
        }
    }
    $onPath = Get-Command dxc.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($onPath) { return $onPath.Source }
    throw @"
Could not find dxc.exe (DirectX shader compiler).
Install a Windows 10/11 SDK, version 10.0.19041 or newer. It is included with
Visual Studio's 'Desktop development with C++' workload (check that a
'Windows 11 SDK' or 'Windows 10 SDK' component is ticked), or available as a
standalone download from Microsoft.
Or pass the file explicitly: -Dxc "C:\path\to\dxc.exe"
"@
}
