# Requirements and install

Back to the [README](../README.md).

## Requirements

| | |
|---|---|
| OS | Windows 10 / 11, 64-bit |
| GPU | Vulkan 1.3: NVIDIA GTX 10-series+, AMD RX 400+, Intel Arc |
| CPU | x86-64 with AVX (Intel Sandy Bridge / AMD FX or newer); FSGSBASE is not needed. Without MOVBE, BMI1 or LZCNT (AMD FX, Intel 2nd / 3rd generation Core) the game's code is translated at start, best effort and slower; Intel Haswell / AMD Ryzen or newer run it directly |
| Python | Bundled with the release (3.12, includes the launcher's Tk); building from source needs Python 3.10+ |
| Game | Decrypted game folder (`CUSA03173` or another retail release), v1.09 (update merged into the base game); an `eboot.bin` with a 60 fps patch baked in is refused. The executable can be named `eboot.bin` or `eboot.elf` (a raw ELF, as some dump tools write it; `eboot.bin` is used when both exist) |

## Quick start

1. Get a release, or [build from source](BUILDING.md).
2. Put your game folder next to the port (`..\CUSA03173`), or pick it in the launcher.
3. Start **`launch_gui.bat`**, choose your settings, click **Launch Bloodborne**.
   Or run `run.bat "C:\Games\Bloodborne\CUSA03173"`.

Saves and the shader cache are stored in `user\`. Settings are stored in `bbport.ini` in the port
folder (next to `launch_gui.bat`).
