# Building from source

Back to the [README](../README.md).

## Build dependencies
- MinGW-w64 GCC (via [w64devkit](https://github.com/skeeto/w64devkit) or MSYS2 MinGW64).
- CMake 3.25+.
- Ninja build tool.
- SDL3 development libraries.
- Vulkan SDK or Vulkan-Headers.
- Optional, for DLSS: the [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) and Visual Studio 2022 (MSVC), see below.

## Build

Clone the repository recursively:
```cmd
git clone --recursive https://github.com/visy/bloodborne_pc_windows_port.git
cd bloodborne_pc_windows_port
```

> **Windows Path Length Warning**: FidelityFX contains deeply nested shader files that can exceed Windows' default 260-character path limit (`MAX_PATH`). If your project folder is located deep in your directory structure, either enable long paths in Git via `git config --system core.longpaths true` (or clone close to a drive root like `C:\bbport`), or set up a mapped drive with `subst X: .`.

Ensure MinGW, CMake, and Ninja are accessible in your environment or PATH (set `W64DEVKIT_DIR`, `SDL3_DIR`, `VULKAN_SDK` as needed), then run:
```cmd
build.bat
```

This compiles LibAtrac9, the Vulkan video core (`out/gpu/bbgpu.dll`), and the main loader executable (`out/bbport.exe`).

**DLSS (NVIDIA GeForce RTX, optional).** NVIDIA's DLSS SDK is not included. Check out
[github.com/NVIDIA/DLSS](https://github.com/NVIDIA/DLSS) and build with `DLSS_SDK_ROOT` set:
```cmd
set DLSS_SDK_ROOT=C:\src\DLSS
build.bat
```
`build.bat` then builds the bridge `out\bbport_dlss.dll` (`gpu/dlss_bridge`, the only code that uses the SDK; it needs Visual Studio 2022, since the SDK's library is MSVC-only) and copies NVIDIA's `nvngx_dlss.dll` next to `out\bbport.exe`, with `nvngx_dlssg.dll` (DLSS frame generation's runtime; in the SDK since 3.5, the checkout used here is 310.9.1) when the SDK has it. Both come from the SDK's `lib\Windows_x86_64\rel` and are redistributed under the SDK's license (`out\NVIDIA_DLSS_LICENSE.txt`), which covers every runtime in the SDK. The player's `user\dlss` folder overrides either one (the launcher's *DLSS library* row); the bridge also finds `nvngx_dlssg.dll` next to `bbport.exe` when `user\dlss` holds only `nvngx_dlss.dll`. Pick *DLSS* in the in-game menu (or `upscaler=dlss` in `bbport.ini`): the preset sets the render size, Native AA is DLAA. Without the DLLs, on other GPUs or with `BB_DLSS=0` it is listed as unavailable and a DLSS setting falls back to FSR 3.1.

**Faster builds for your own PC (optional).**
- `-DBB_MARCH=native` (or a CPU name such as `skylake`) when configuring `out/gpu` tunes the code for your processor; the result may not start on older CPUs. Release builds leave `BB_MARCH` empty (generic x86-64, as Supermedo's port builds): the program itself then starts on CPUs without AVX2 (AMD FX, Intel Sandy / Ivy Bridge), where the game's own code runs through the Legacy CPU translation (`scripts/cpu_compat.py`).
- Profile-guided optimization: configure a second build folder with `-DBB_PGO=generate -DBB_OUT_DIR=<folder>`, play about 20 minutes with that `bbport.exe` (it writes a profile into `pgo\` every 30 s), then reconfigure the same build folder with `-DBB_PGO=use` and build again. Only the port's own code is optimized (GPU command translation, caches, runtime), so it helps in CPU-limited scenes.

## Bundled Python for a release

Releases ship a trimmed CPython 3.12 x64 with Tk in `python\`, so players need no installed Python: `launch_gui.bat` runs the launcher on `python\pythonw.exe` and `run.bat` runs the scripts on `python\python.exe`, falling back to an installed Python 3.10+ when the folder is missing (a source checkout). python.org's embeddable zip has no tkinter, so the bundle is made from a full python.org install (3.12.x, 64-bit):
```cmd
python tools\release\make_python_bundle.py --src C:\Python312 --out <release folder>\python
```
It copies the interpreter, `DLLs\`, `tcl\` and `Lib\` without tests, IDLE, pip, lib2to3 and site-packages (about 47 MB, 17.5 MB zipped), and precompiles the standard library to unchecked-hash `.pyc` files, so the interpreter never writes into the folder and two builds from the same install are identical. The bats set `PYTHONDONTWRITEBYTECODE=1` and clear `PYTHONHOME` / `PYTHONPATH` for it. `launch_gui.bat --print-python` shows which Python the launcher would use. The scripts use the standard library only; a new script must too (check with `python\python.exe -I -c "import <module>"`).

## Upscaler assets

FSR 3.1 needs none. FSR 4 (v07) and FSR 4.1.1 need assets that are not included: `tools/fetch_fsr4_assets.sh` downloads FSR 4 v07, and FSR 4.1.1 is built from your own AMD `amd_fidelityfx_upscaler_dx12.dll` (4.1.x) by `tools/fsr4cap` (upstream runs this under Proton on Linux; the resulting `fsr4_411` folder can be copied here). On RDNA4 the FP8 variant is picked automatically; `BB_FSR411_VARIANT=int8` forces INT8.
