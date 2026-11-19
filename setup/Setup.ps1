# Installs, updates or removes GPU vrad3 in Half-Life: Alyx.
param(
    [ValidateSet('', 'Install', 'Uninstall', 'Status')]
    [string]$Action = '',
    [string]$GameBin = '',    # blank = find Half-Life: Alyx through Steam
    [switch]$Preview,         # with -Action Install: also install vrad2.exe without asking
    [switch]$NoPause,         # don't wait for Enter before closing
    [switch]$Elevated
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

# vrad3.exe builds that the GPU code supports (SHA-256), as in vrad3_capture.cpp.
$supported = @{
    '6cf516c14515c0898619fa2e3b86265ca596ea4938006a4768cdd2738a1d9eda' = 'public 2026-10-27'
    'bc178f594768fcf792b7c5c7dce07a5b2877a90da2f6ea9944db7dac6c860d6c' = 'previous_release 2020-06-13'
}

function Say([string]$text, [string]$colour = 'Gray') { Write-Host $text -ForegroundColor $colour }
function Ask([string]$question, [bool]$default) {
    $hint = if ($default) { '[Y/n]' } else { '[y/N]' }
    $answer = "$(Read-Host "$question $hint")".Trim()
    if (-not $answer) { return $default }
    return $answer -match '^[yY]'
}
function Finish([int]$code) {
    if (-not $NoPause) { Read-Host 'Press Enter to close' | Out-Null }
    exit $code
}
function Fail([string]$text) { Say "ERROR: $text" Red; Finish 1 }

function Hash([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
function IsValve([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    $signature = Get-AuthenticodeSignature -LiteralPath $path
    return $signature.Status -eq 'Valid' -and $signature.SignerCertificate.Subject -match 'O=Valve Corp\.'
}

function FindGameBins {
    $steam = $null
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        $item = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
        if ($item -and $item.SteamPath) { $steam = $item.SteamPath; break }
        if ($item -and $item.InstallPath) { $steam = $item.InstallPath; break }
    }
    if (-not $steam) { return @() }
    $libraries = @($steam.Replace('/', '\'))
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $vdf) {
        foreach ($match in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) {
            $libraries += $match.Groups[1].Value.Replace('\\', '\')
        }
    }
    $bins = @()
    foreach ($library in ($libraries | Sort-Object -Unique)) {
        $manifest = Join-Path $library 'steamapps\appmanifest_546560.acf'
        if (-not (Test-Path -LiteralPath $manifest)) { continue }
        $folder = 'Half-Life Alyx'
        $match = [regex]::Match((Get-Content -LiteralPath $manifest -Raw), '"installdir"\s+"([^"]+)"')
        if ($match.Success) { $folder = $match.Groups[1].Value }
        $bin = Join-Path $library "steamapps\common\$folder\game\bin\win64"
        if (Test-Path -LiteralPath (Join-Path $bin 'hlvr.exe')) { $bins += (Resolve-Path -LiteralPath $bin).Path }
    }
    return @($bins | Sort-Object -Unique)
}

# One of: missing, valve, valve+stale (Steam put Valve's file back over ours), ours, ours-old, broken.
function ToolState([string]$name) {
    $current = Join-Path $GameBin "$name.exe"
    $real = Join-Path $GameBin "${name}_real.exe"
    $package = Join-Path $here "$name.exe"
    $hasCurrent = Test-Path -LiteralPath $current
    $hasReal = Test-Path -LiteralPath $real
    if (-not $hasCurrent -and -not $hasReal) { return 'missing' }
    if ($hasCurrent -and (IsValve $current)) {
        if (-not $hasReal) { return 'valve' }
        if (IsValve $real) { return 'valve+stale' }
        return 'broken'
    }
    if (-not (IsValve $real)) { return 'broken' }
    if (-not $hasCurrent) { return 'valve-renamed' }
    if ((Test-Path -LiteralPath $package) -and (Hash $current) -eq (Hash $package)) { return 'ours' }
    return 'ours-old'
}

function ShowStatus {
    Say "Half-Life: Alyx: $GameBin"
    foreach ($name in 'vrad3', 'vrad2') {
        $text = switch (ToolState $name) {
            'missing'       { "not found (is this the right folder?)" }
            'valve'         { "Valve's original, GPU version not installed" }
            'valve+stale'   { "Valve's original (a Steam update or file check replaced it). Run Install to put the GPU version back" }
            'valve-renamed' { "${name}.exe missing, Valve's original is ${name}_real.exe. Run Install or Uninstall" }
            'ours'          { "GPU version installed (this version)" }
            'ours-old'      { "GPU version installed (a different version). Run Install to update" }
            'broken'        { "${name}_real.exe is missing or isn't Valve's. Verify the game files in Steam, then run Setup again" }
        }
        Say "  ${name}.exe: $text"
    }
    $real = Join-Path $GameBin 'vrad3_real.exe'
    $valve = if (IsValve $real) { $real } else { Join-Path $GameBin 'vrad3.exe' }
    if (IsValve $valve) {
        $build = $supported[(Hash $valve)]
        if ($build) { Say "  Valve's compiler: $build (supported)" }
        else { Say "  Valve's compiler: a build this version doesn't support. Compiles will run on the CPU" Yellow }
    }
    $oidn = Join-Path $GameBin 'OpenImageDenoise.dll'
    if (Test-Path -LiteralPath $oidn) {
        $signature = Get-AuthenticodeSignature -LiteralPath $oidn
        $text = if ($signature.SignerCertificate.Subject -match 'O=Intel Corporation') { "Valve's (CPU)" } else { 'GPU shim installed' }
        Say "  OpenImageDenoise.dll: $text"
    }
}

function CheckNotRunning {
    $running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
        try { $_.Path -and $_.Path.StartsWith($GameBin + '\', [StringComparison]::OrdinalIgnoreCase) } catch { $false }
    })
    if ($running.Count) {
        Fail ("Close Hammer and Half-Life: Alyx first (running: " + (($running.Name | Sort-Object -Unique) -join ', ') + ')')
    }
}

# Replaces a file without leaving it half-written: copy beside it, then rename over it.
function PutFile([string]$source, [string]$target) {
    $temp = "$target.setup-tmp"
    Copy-Item -LiteralPath $source -Destination $temp -Force
    Move-Item -LiteralPath $temp -Destination $target -Force
}

function InstallTool([string]$name) {
    $current = Join-Path $GameBin "$name.exe"
    $real = Join-Path $GameBin "${name}_real.exe"
    $package = Join-Path $here "$name.exe"
    $state = ToolState $name
    switch ($state) {
        'missing' { Fail "$name.exe not found in $GameBin" }
        'broken'  { Fail "${name}_real.exe is missing or isn't Valve's. Verify the game files in Steam, then run Setup again" }
    }
    $renamed = $false
    try {
        if ($state -eq 'valve+stale') { Remove-Item -LiteralPath $real -Force }
        if ($state -eq 'valve' -or $state -eq 'valve+stale') {
            Move-Item -LiteralPath $current -Destination $real
            $renamed = $true
        }
        PutFile $package $current
    } catch {
        if ($renamed -and -not (Test-Path -LiteralPath $current)) { Move-Item -LiteralPath $real -Destination $current }
        Fail "Installing $name.exe failed: $($_.Exception.Message)"
    }
    Start-Sleep -Milliseconds 500
    if (-not (Test-Path -LiteralPath $current) -or (Hash $current) -ne (Hash $package)) {
        Say "  $name.exe was removed or changed after copying, probably by antivirus. Allow it there and run Setup again" Yellow
    } else {
        Say "  $name.exe installed (Valve's original kept as ${name}_real.exe)" Green
    }
}

function UninstallTool([string]$name) {
    $current = Join-Path $GameBin "$name.exe"
    $real = Join-Path $GameBin "${name}_real.exe"
    switch (ToolState $name) {
        'ours'          { Remove-Item -LiteralPath $current -Force; Move-Item -LiteralPath $real -Destination $current; Say "  $name.exe: Valve's original restored" Green }
        'ours-old'      { Remove-Item -LiteralPath $current -Force; Move-Item -LiteralPath $real -Destination $current; Say "  $name.exe: Valve's original restored" Green }
        'valve-renamed' { Move-Item -LiteralPath $real -Destination $current; Say "  $name.exe: Valve's original restored" Green }
        'valve+stale'   { Remove-Item -LiteralPath $real -Force; Say "  $name.exe: already Valve's - removed the old ${name}_real.exe" Green }
        'valve'         { Say "  $name.exe: already Valve's" }
        'missing'       { Say "  $name.exe: not found" Yellow }
        'broken'        { Say "  $name.exe: left alone, ${name}_real.exe is missing or isn't Valve's. Verify the game files in Steam" Yellow }
    }
}

function EnsureWritable {
    $probe = Join-Path $GameBin ('vrad3-setup-' + [guid]::NewGuid().ToString('N') + '.tmp')
    try { [IO.File]::WriteAllText($probe, ''); Remove-Item -LiteralPath $probe -Force; return }
    catch { }
    if ($Elevated) { Fail "Can't write to $GameBin, even as administrator" }
    Say 'Windows will ask for administrator permission (the game folder is under Program Files).'
    $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Action $Action -GameBin `"$GameBin`" -Elevated"
    if ($Preview) { $arguments += ' -Preview' }
    try { $process = Start-Process powershell -Verb RunAs -ArgumentList $arguments -Wait -PassThru }
    catch { Fail 'Administrator permission was refused.' }
    exit $process.ExitCode
}

# Files from a downloaded zip carry a "from the internet" mark that can block them.
Get-ChildItem -LiteralPath $here -File | Unblock-File -ErrorAction SilentlyContinue

foreach ($file in 'vrad3.exe', 'vrad2.exe', 'vrad3_settings.exe') {
    if (-not (Test-Path -LiteralPath (Join-Path $here $file))) { Fail "$file is missing next to Setup. Make sure the zip file is fully extracted." }
}

if ($GameBin) {
    $GameBin = $GameBin.TrimEnd('\')
} else {
    $bins = @(FindGameBins)
    if ($bins.Count -eq 1) { $GameBin = $bins[0] }
    elseif ($bins.Count -gt 1) {
        Say 'Half-Life: Alyx was found in more than one place:'
        for ($i = 0; $i -lt $bins.Count; ++$i) { Say "  $($i + 1). $($bins[$i])" }
        $pick = Read-Host 'Which one? (number)'
        if (-not ($pick -match '^\d+$') -or [int]$pick -lt 1 -or [int]$pick -gt $bins.Count) { Fail 'No folder chosen.' }
        $GameBin = $bins[[int]$pick - 1]
    } else {
        Say "Half-Life: Alyx wasn't found through Steam."
        $GameBin = "$(Read-Host 'Paste the path of its game\bin\win64 folder')".Trim('" ').TrimEnd('\')
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $GameBin 'vrad3.exe')) -and -not (Test-Path -LiteralPath (Join-Path $GameBin 'vrad3_real.exe'))) {
    Fail "No vrad3.exe in $GameBin. Pick Half-Life Alyx\game\bin\win64"
}

if (-not $Action) {
    ShowStatus
    Say ''
    Say '  1. Install or update'
    Say '  2. Uninstall'
    Say '  3. Quit'
    switch ("$(Read-Host 'Choose')".Trim()) {
        '1' { $Action = 'Install' }
        '2' { $Action = 'Uninstall' }
        default { Finish 0 }
    }
}

switch ($Action) {
    'Status' { ShowStatus; Finish 0 }
    'Install' {
        CheckNotRunning
        EnsureWritable
        $previewState = ToolState 'vrad2'
        if (-not $Preview) {
            $Preview = ($previewState -eq 'ours' -or $previewState -eq 'ours-old') -or
                (Ask "Also bake Hammer's lighting preview on the GPU (vrad2.exe)?" $true)
        }
        InstallTool 'vrad3'
        if ($Preview) { InstallTool 'vrad2' }
        PutFile (Join-Path $here 'vrad3_settings.exe') (Join-Path $GameBin 'vrad3_settings.exe')
        $example = Join-Path $here 'vrad3_gpu.example.ini'
        if (Test-Path -LiteralPath $example) { PutFile $example (Join-Path $GameBin 'vrad3_gpu.example.ini') }
        Say '  vrad3_settings.exe installed' Green
        Say ''
        ShowStatus
        Say ''
        Say 'Done. Compile in Hammer as usual.'
        Say 'After a Steam update, run Setup again.'
        Finish 0
    }
    'Uninstall' {
        CheckNotRunning
        $settings = Join-Path $GameBin 'vrad3_gpu.ini'
        EnsureWritable
        $dropSettings = (Test-Path -LiteralPath $settings) -and (Ask 'Also delete your settings (vrad3_gpu.ini)?' $false)
        UninstallTool 'vrad3'
        UninstallTool 'vrad2'
        foreach ($file in 'vrad3_settings.exe', 'vrad3_gpu.example.ini') {
            $path = Join-Path $GameBin $file
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
        }
        if ($dropSettings) { Remove-Item -LiteralPath $settings -Force; Say '  vrad3_gpu.ini deleted' }
        elseif (Test-Path -LiteralPath $settings) { Say '  vrad3_gpu.ini kept (your settings)' }
        Say ''
        Say 'Done. Valve''s compiler is back.'
        Finish 0
    }
}
