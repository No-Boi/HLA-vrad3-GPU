# GPU vrad3 for Half-Life: Alyx

Bakes final and preview lighting using your GPU.

It replaces the lighting compilers (`vrad3.exe` and `vrad2.exe`) with wrappers that do the lighting on your graphics card. Anything the GPU can't do will fall back to the CPU.

Unofficial, not affiliated with Valve. Contains no Valve code.

## Requirements
- Windows 10/11, 64-bit
- DirectX 12 GPU with hardware ray tracing (DXR 1.1): NVIDIA RTX 20, AMD RX 6000, Intel Arc
  or newer. If you do not have a suitable GPU everything falls back to the CPU.
- HLA build: The current Steam version or `previous_release`. With any other
  build everything runs on the CPU.

## What's in the download
- `vrad3.exe`: the GPU lighting compiler
- `vrad2.exe`: optional, Hammer's preview lighting on the GPU
- `vrad3_settings.exe`: optional settings window
- `Setup.cmd`, `Setup.ps1`: installation scripts
- `oidn-shim\OpenImageDenoise.dll`: optional, GPU denoiser (see below)

## Install

### Automatic

Put all the downloaded files in one folder, then double-click `Setup.cmd` and choose Install.
If the game is installed in a restricted location (e.g. Program Files)
it might ask for administrator permission.

### Manual

Copy files manually to `Half-Life Alyx\game\bin\win64`:
1. Rename `vrad3.exe` to `vrad3_real.exe`.
2. Copy our `vrad3.exe` in.
3. Optional: `vrad3_settings.exe` (settings GUI)

Preview lighting on GPU (needs the steps above too):
1. Rename `vrad2.exe` to `vrad2_real.exe`.
2. Copy our `vrad2.exe` in.

To check it works, compile a map in Hammer. The end of the compile will
name the graphics card used ("VRAD3 GPU: graphics card: ...")
and will mention if anything ran on the CPU.

Antivirus may flag it, because it runs Valve's compiler under a debugger. Windows may also
warn that the files are from an unknown publisher, because they aren't code-signed.

## Optional: denoiser on GPU
Valve's denoiser (Intel Open Image Denoise 1) only runs on the CPU. Our
`OpenImageDenoise.dll` lets it use Open Image Denoise 2.5.1 on the GPU instead. The result
differs very slightly from Valve's. Setup doesn't install this because it's a third-party tool.
1. Back up Valve's OpenImageDenoise.dll (for example as OpenImageDenoise.valve.dll).
2. Download OIDN 2.5.1 for Windows (oidn-2.5.1.x64.windows.zip, from
   https://github.com/RenderKit/oidn/releases) and copy the .dll files from its bin
   folder into game\bin\win64. The three .exe files there aren't needed.
3. Rename OIDN's OpenImageDenoise.dll (the one you just copied) to
   OpenImageDenoise_real.dll.
4. Copy our OpenImageDenoise.dll into game\bin\win64.

GPU denoising needs a recent graphics driver; an older one will quietly run on the CPU:
- NVIDIA: driver 528.33 or newer
- AMD: a recent Adrenalin driver (tested with 26.10.1)
- Intel: driver 31.0.101.4953 or newer

## Optional: newer Embree
The parts that still run on Valve's CPU code use Intel Embree 3.6.1 for ray tracing.
Embree 3.13.5 is a drop-in replacement and a little faster. Results may differ very
slightly. Back up Valve's `embree3.dll`, then copy `embree3.dll` and `tbb12.dll` from the
bin folder of Intel's embree-3.13.5 Windows zip (https://github.com/RenderKit/embree/releases)
into game\bin\win64. If you installed the GPU denoiser, keep its `tbb12.dll` instead of
Embree's.

## Uninstall
Run Setup again and choose Uninstall. It puts Valve's files back and asks whether to keep
your settings.

Or by hand: delete our `vrad3.exe` and `vrad2.exe`, and rename `vrad3_real.exe` and
`vrad2_real.exe` back. For the denoiser or Embree, delete the files you copied and rename
the backups back.

You can also verify your game files through Steam to redownload original files.

## After a Steam update
Steam may put Valve's `vrad3.exe` and other files back (this also happens when you verify
the game files). Run Setup again to reinstall. The denoiser and Embree shouldn't be removed by a Steam update or verifying files,
but if they somehow are, then you'll have to copy them back.

## Settings
Run `vrad3_settings.exe` from game\bin\win64 to easily customize the compiler to your liking,
or create `vrad3_gpu.ini` next to `vrad3.exe` in game\bin\win64 and edit it by hand.
[vrad3_gpu.example.ini](tools/vrad3_gpu.example.ini)) has an explanation of every setting.
The main choice is the estimator mode:
- `estimator = cs2` (default): works like Counter-Strike 2's GPU baker. Less noise, faster.
- `estimator = hla`: follows Half-Life: Alyx's baker, closest to original result.

`enabled = false` turns the GPU off and runs Valve's compiler only.

## Not on GPU yet
- Probe volume direct light.
- Fog volumes.

## Reporting a problem
1. Set logging to detailed (in `vrad3_settings.exe`, or `logging = detailed` in `vrad3_gpu.ini`).
2. Compile the map again.
3. Send the files in `%LOCALAPPDATA%\vrad3-gpu\logs`, the end of the Hammer compile output, and your graphics card.

## Checking the download
Each release lists the SHA-256 hash of every file. To check yours, open PowerShell in the
folder with the files and run:

    Get-FileHash *.exe, *.ps1, *.cmd, oidn-shim\*.dll

The hashes should match the ones on the release page.

You can also check the exe was built from this source: build it yourself (below) with the
Visual Studio version named in the release, and compare the hash of `dist\vrad3.exe`.

## Building from source
You need Visual Studio 2022 or newer (or its free Build Tools) with the
"Desktop development with C++" workload, and a Windows SDK 10.0.19041 or newer.

Run `Build.cmd`. The files are written to `dist\`. If Visual Studio or the SDK isn't found
automatically, pass them with: `Build.cmd -VisualStudio <folder> -Dxc <path to dxc.exe>`.

## License
Open Image Denoise and Embree are Intel's (Apache-2.0) and are not included.
`tools/sample_exact.h` contains steps from Microsoft's STL `std::sort` (Apache-2.0 WITH LLVM-exception).
