# bloodborne_pc_windows_port: Native Windows Port of Bloodborne

> This project is not related to shadPS4. Questions about the upstream Linux port go to its
> Discord server (https://discord.gg/KYZRKk9CB), not to the shadPS4 server.

`bloodborne_pc_windows_port` is a native 64-bit Windows port of the PlayStation 4 executable of *Bloodborne* (CUSA03173, version 1.09). 

This project is a Windows adaptation of the original Linux port ([`bbport`](https://github.com/deadinside28/bloodborne_pc) by `deadinside28`). It replaces the Linux-specific kernel, memory mapping, and POSIX threading implementation with a native Win32 runtime, allowing the game to run directly on Windows with Vulkan.

bbport is the counterpart of Wine + DXVK for a single game: the game's original executable runs directly on the PC.

- **As in Wine**, the game's x86-64 code runs on the CPU directly, and a runtime written for this one game replaces the PS4 system libraries.
- **As in DXVK**, the game's graphics are translated to Vulkan by a renderer derived from [shadPS4](https://github.com/shadps4-emu/shadPS4) and heavily extended for this game, including temporal upscaling with AMD FSR 3.1, FSR 4 and FSR 4.1.1 (and NVIDIA DLSS when built with NVIDIA's SDK).

> **No game files or copyrighted assets are included.** You must provide your own decrypted dump of Bloodborne (CUSA03173, version 1.09).
> This project is not affiliated with Sony Interactive Entertainment, FromSoftware, AMD, or NVIDIA.

**Status: Playable on Windows.** The game boots, loads saves, and runs with audio, gamepad support, and state persistence.

---

## Windows Port Changes

This repository adapts the original Linux codebase specifically for Windows systems:

- **Win32 Memory & Synchronization**: Replaced Linux `mmap`, `mprotect`, and POSIX primitives with Windows `VirtualAlloc`, `VirtualProtect`, and native Win32 synchronization events (`src/win32_compat.c`, `src/win32_memory.c`, `src/runtime_host.c`).
- **Graphical Launcher**: Includes `launch_gui.bat` (powered by `launcher.py`), a desktop control panel to configure target frame rates (30, 60, 90, uncap), rendering resolution, FSR upscaling, monitor choice, and all toggleable community patches from `Bloodborne.xml`.
- **Command Line Launcher**: `run.bat` provides direct scripted startup, automatic patch compilation, and mod staging on Windows. Started on its own, it applies the launcher's last saved settings (`launcher_settings.json`; `BB_NO_LAUNCHER_SETTINGS=1` skips them).
- **Vulkan Frame Pacing**: Integrated presenter queue pacing (`BB_FRAMES_AHEAD=2`) to bound GPU run-ahead, stabilizing frametimes and smoothing 0.1% and 1.0% stutter lows.
- **Texture Barrier Fix**: Fixed dynamic texture streaming race conditions in `tile_manager.cpp`, ensuring item icons in the HUD and loading screens render correctly without static noise.
- **AT9 Audio Playback**: Bundles LibAtrac9 compilation to decode AT9 game audio natively on Windows.
- **Windows Build System**: Standalone `build.bat` script that compiles the complete project with MinGW-w64 (GCC), CMake, and Ninja.

---

## Technical Highlights

- **Native Execution**: The game eboot is converted offline into a flat memory image; PS4 libc and libSceFios2 functions are linked directly into the binary. No CPU emulation or instruction translation occurs at run time.
- **High Framerate Support**: Community simulation patches allow running at 60 FPS, 90 FPS, or uncapped with accurate physics and movement speeds.
- **Synthetic Motion Vectors & Temporal Upscaling**: Because Bloodborne lacks an internal velocity buffer, the port computes camera motion from depth buffers and object motion from previous frame vertex positions. The scene can render at lower internal resolutions (e.g., 720p or 1080p) while FSR 3.1 / 4 / 4.1.1 (or DLSS) upscales to your display resolution, leaving UI elements crisp at native resolution.
- **Multi-threaded Draw Pipeline**: PS4 command buffers are decoded on one thread while draw calls are bound and recorded on another, preventing CPU bottlenecks.
- **Settings inside the game's own menu**: *System → Display / Game effects / Game patches* (after *Screen/Sound*), built and drawn by the game itself: output resolution, upscaler, preset, sharpness, FPS counter, model detail and the effect patches, in the game's language. The game's *Default* button restores the port's defaults. `BB_GAME_MENU=0` leaves the game's menu untouched.
- **In-Game Overlay**: Press `Insert` (or `L3 + R3` on your controller, when enabled in the launcher) to open the overlay menu, styled like the game's dialogs: everything above plus the advanced settings, with gamepad, keyboard and mouse.
- **No online/offline screen**: there is no PSN, so the *Play online / offline* choice is skipped by a game patch (*Skip Online/Offline Choice*) and the game opens its main menu offline. The launcher's *Skip online/offline choice* switch, or `BB_SKIP_NETWORK_CHOICE=0`, shows the screen again.
- **DLC**: add-on dumps in `user\addcont\<title id>\<entitlement label>\` (shadPS4's layout, e.g. `user\addcont\CUSA03173\SPEXPANSIONDLC03` for The Old Hunters) are reported to the game as installed add-ons.
- **Save safety**: save files are written to a temporary copy and moved into place when the game closes them, so a crash in the middle of saving leaves the previous save intact (`BB_SAVE_TRACE=1` logs every save file operation).
- **Game file check**: the launcher and `run.bat` check that the game folder holds the 1.09 executable (the base game 1.00 crashes at start) and say what is missing; `BB_SKIP_GAME_CHECK=1` skips the check. A dumped update is a separate folder: copy it over the base game, replacing files. Plain ELF `eboot.bin` dumps (without the SELF wrapper) are accepted too.

### Wine + DXVK for one game

- **CPU.** The PS4 CPU is x86-64, so the game's code runs directly on the PC's CPU, with no
  emulation and no instruction translation.
- **System libraries.** As Wine replaces the Windows API, the bbport runtime (`src/runtime_*.c`)
  implements exactly the PS4 OS functions Bloodborne calls: memory, threads, files, audio, pad,
  saves.
- **Memory.** Upstream has an experimental *New memory and translation model* (`BB_PC_MODEL=1`,
  off by default): the game's data in system RAM, VRAM used the way a PC game uses it. It is
  developed and tested on Linux; on this Windows port the default (old) memory model is the one
  that is supported.
- **Graphics.** The PS4 GPU's command stream (PM4) is a recording of the game's graphics API
  calls: it is written by 99 functions of the statically linked libGnm, so decoding the stream
  and translating the calls themselves come to the same thing. GCN shaders are translated to
  SPIR-V. Resource descriptors (textures, buffers) are read by the translator on the CPU, as DXVK
  reads resource bindings: a PS4 texture has to be converted from the PS4 tiling into a Vulkan
  image before the draw runs. The game's occlusion queries are Vulkan occlusion queries;
  predication, `COPY_DATA` and `COND_EXEC` are translated too (check: `BB_PM4_SELFTEST=1`).
  Details: [docs/EMULATION_REMOVAL_PLAN.ru.md](docs/EMULATION_REMOVAL_PLAN.ru.md), "Шаг 4" (Russian).

"Port" here means a build for this one game, not a rewrite of its source code, which the project
neither has nor includes.

### How it differs from shadPS4

| | shadPS4 | bbport |
|---|---|---|
| Scope | General PS4 emulator, many games | One game: Bloodborne v1.09 |
| Loading | Its own ELF loader and kernel emulation at run time | The eboot is converted offline (`scripts/`) into an image with PS4 libc/Fios2 linked in; a C loader maps it and jumps into the game |
| System libraries | Broad HLE of the PS4 OS | A small runtime (`src/runtime_*.c`) that implements exactly what Bloodborne calls: memory, threads, sync, files, audio (incl. ATRAC9), pad, saves, AppContent |
| GPU | shadPS4 video core and shader recompiler | The same core (vendored, GPL) with marked changes (`bbport:`) plus new modules: two-stage draw pipeline, render-state and texture-set memoization, render-scale proxies, motion vectors, FSR 3.1/4/4.1.1, DLSS bridge, frame capture and GPU profiler |
| GPU thread | One thread processes the whole command stream (the bottleneck in Bloodborne) | Decode and draw recording run on separate threads |
| Upscaling | — | Temporal (FSR 3.1, FSR 4, FSR 4.1.1, DLSS) with the game's own motion vectors and jitter |
| Game patches | Patch files applied by the emulator | The same community patches, compiled at start (`scripts/patches.py`); render resolution, effects and FPS from the launcher |

---

## Requirements

- **Operating System**: Windows 10 or Windows 11 (64-bit).
- **Graphics Card**: A Vulkan 1.3 capable GPU with current vendor drivers:
  - NVIDIA GeForce GTX 10-series or newer (DLSS: GeForce RTX).
  - AMD Radeon RX 400-series or newer.
  - Intel Arc A-series or newer.
  - FSR 4 / 4.1.1 require shader Float16, Int8/Int16, integer dot products, linear compute derivatives and extended storage image formats; FSR 4.1.1 additionally requires `VK_VALVE_shader_mixed_float_dot_product`. Unsupported choices fall back to FSR 3.1 before the first frame and are disabled in the in-game menu.
- **Processor**: x86-64 processor with AVX2 support.
- **Python**: Python 3.10 or newer (needed for the patch compiler and launcher).
- **Game Files**: Decrypted `CUSA03173` directory containing `eboot.bin` (version 1.09, the update merged into the base game).

### Build Dependencies (Only if compiling from source)
- MinGW-w64 GCC (via [w64devkit](https://github.com/skeeto/w64devkit) or MSYS2 MinGW64).
- CMake 3.25+.
- Ninja build tool.
- SDL3 development libraries.
- Vulkan SDK or Vulkan-Headers.
- Optional, for DLSS: the [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) and Visual Studio 2022 (MSVC), see below.

---

## Building and Running

### 1. Build from Source

Clone the repository recursively:
```cmd
git clone --recursive https://github.com/Mrsuss60/bloodborne_pc_windows_port.git
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
`build.bat` then builds the bridge `out\bbport_dlss.dll` (`gpu/dlss_bridge`, the only code that uses the SDK; it needs Visual Studio 2022, since the SDK's library is MSVC-only) and copies NVIDIA's `nvngx_dlss.dll` next to `out\bbport.exe`. Pick *DLSS* in the in-game menu (or `upscaler=dlss` in `bbport.ini`): the preset sets the render size, Native AA is DLAA. Without the DLLs, on other GPUs or with `BB_DLSS=0` it is listed as unavailable and a DLSS setting falls back to FSR 3.1.

### 2. Launch the Game

By default the game folder is expected next to the repository (`..\CUSA03173`). Saves and the shader cache go to `user\`; settings to `bbport.ini`.

#### Option A: Using the Graphical Launcher (Recommended)
Double-click `launch_gui.bat` or run:
```cmd
launch_gui.bat
```
- Select your `eboot.bin` file or game folder.
- Select your target frame rate (60 FPS recommended for smooth frame pacing).
- Select your internal resolution (e.g., 1280x720 with FSR enabled for high performance).
- With several monitors, pick the one for the game under *Monitor* (`BB_DISPLAY=<number or part of the name>`).
- Enable desired patches under the Patches tab (e.g., *Performance Patch*, *Disable Motion Blur*, *Skip Intro*).
- Click **Launch Bloodborne**.

#### Option B: Using the Command Line
Run `run.bat` pointing to your game directory:
```cmd
run.bat "C:\Games\Bloodborne\CUSA03173"
```

Save files and shader caches are stored in `user\`. Settings are saved to `bbport.ini`.

### Mods, patches and DLC

**Mods:** loose-file mod folders in `mods\` (with `dvdroot_ps4\`, an extra wrapper folder, or the game folders such as `chr\` directly; file name case does not matter), with enable switches and load order in `mods.json`. A sibling `CUSA03173-mods\` overlay also works. The original game is preserved; later mods override conflicting files.

**Third-party patches:** shadPS4-format XML patch files in `patches\`, switched on and off in `patches.json`. See [mods and patches](docs/MODS.md).

**DLC:** put your add-on dumps in `user\addcont\<title id>\<entitlement label>\`, e.g. `user\addcont\CUSA03173\SPEXPANSIONDLC03` for The Old Hunters.

### Upscaler assets

FSR 3.1 needs none. FSR 4 (v07) and FSR 4.1.1 need assets that are not included: `tools/fetch_fsr4_assets.sh` downloads FSR 4 v07, and FSR 4.1.1 is built from your own AMD `amd_fidelityfx_upscaler_dx12.dll` (4.1.x) by `tools/fsr4cap` (upstream runs this under Proton on Linux; the resulting `fsr4_411` folder can be copied here). On RDNA4 the FP8 variant is picked automatically; `BB_FSR411_VARIANT=int8` forces INT8.

---

## Controls and In-Game Features

- **Gamepad**: Controllers are supported out of the box through SDL3. With several connected, `BB_GAMEPAD=<GUID or part of the name>` picks one. The keyboard works too, also next to a connected gamepad. The character name is typed on the keyboard in a box over the game.
- **Touchpad**: its left half (`Tab`, Back/Select) opens the gestures, the right half (`Backspace`) the key items.
- **In-Game Settings**: *System → Display / Game effects / Game patches* in the game's own menu, or press `Insert` on keyboard (`L3 + R3` on gamepad when enabled in the launcher) to toggle the overlay menu.
- **Free Camera**: When enabled in the launcher or overlay, hold `Cross` and press `L3` (or hold `Space` and press `Z` on keyboard) to cycle camera modes. Uses Lance McDonald's GoldHEN patch; conflicts with *Enemy Control*.
- **Debug Menu**: If debug fonts (`DbgFont14h.ccm` and `DbgFont14h.tpf`, from [Debug Menu and XML Patch](https://www.nexusmods.com/bloodborne/mods/253)) are installed into `dvdroot_ps4/font/` (or `adhoc/font/`), open the menu using `Tab` or the left half of the controller touchpad. Startup rejects missing or empty font files instead of launching the unsafe patch.

### Sharing the shader cache

The pipeline cache in `user\cache\CUSA03173\` can be copied as-is to another PC, also one with a different GPU. Next to the compiled shaders it keeps each shader's source (`.src` files: the game's shader code and the inputs of its translation). When the cache was made for another GPU (or by a build with a changed shader translator), the first launch translates every shader again from these sources for the current GPU ("Shader cache: rebuilding N shaders for this GPU..." in the log) instead of throwing the cache away. `BB_SHADER_CACHE_REBUILD=1` forces this rebuild, `BB_SHADER_CACHE_REBUILD_THREADS=N` sets its thread count. `BB_SHADER_CACHE_SELFTEST=1` translates each newly compiled shader a second time from its stored source and logs any difference (a developer check). The cache contains data derived from the game's shaders: share it only with people who own the game, and never commit it to this repository.

### Useful environment variables

`BB_FRAME_STATS=1` (frame statistics), `BB_SAVE_LOG=1` (frame and readback statistics into `logs\<time>.frames.csv` / `.readbacks.csv`; the launcher's own log is `launcher.log`), `BB_ANISO=N` (anisotropic filtering; 16 by default, 0 = the game's own), `BB_UPSCALER=taa|fsr3|fsr4|fsr411|dlss|off`, `BB_FRAMES_AHEAD=N`, `BB_PRESENT_THREAD=0`, `BB_LIVE_RES=1`, `BB_PREUPLOAD=0|1|2` (background upload of GPU memory into VRAM: 1 by default, 2 = all of it, ~3 GB more VRAM, 0 = off), `BB_READBACKS=0|1|2`, `BB_GPU_PROFILE=1`, `BB_BREADCRUMBS=0`, `BB_DISPLAY`, `BB_GAMEPAD`, `BB_SKIP_NETWORK_CHOICE=0`, `BB_GAME_MENU=0`, `BB_SKIP_GAME_CHECK=1`.
More in [docs/](docs); recent upstream changes: [docs/CHANGES_after_0.4.md](docs/CHANGES_after_0.4.md), [docs/CHANGES_0.4.md](docs/CHANGES_0.4.md) (in Russian), [docs/CHANGES_2026-10-06.md](docs/CHANGES_2026-10-06.md).

> Upstream (Linux) also ships a GTK4 launcher, an AppImage for the Steam Deck, MangoHud integration and `run.sh`/`build.sh`; those are kept in this repository for reference but are not used on Windows.

---

## Repository Layout

| Path | Description |
|---|---|
| `src/` | Win32 loader (`probe.c`), memory management, and PS4 HLE runtime |
| `gpu/` | Vulkan video core, multi-stage command pipeline, upscaler integration, DLSS bridge (`gpu/dlss_bridge`) |
| `scripts/` | Eboot preparation, game file check, patch compiler (`patches.py`), and DLL staging |
| `patches/` | Community XML patches for Bloodborne version 1.09 |
| `launcher.py` | Tkinter desktop launcher for Windows |
| `launch_gui.bat` | One-click shortcut for the graphical launcher |
| `build.bat` | Windows build script |
| `run.bat` | Direct execution batch script |
| `launcher/`, `run.sh`, `build.sh` | Upstream's Linux launcher and scripts |
| `tools/` | Developer utilities and patch extraction scripts |
| `tests/` | Loader, runtime, patch and renderer tests (`build.bat --test`) |
| `docs/` | Architecture notes, upscaler documentation, and change logs |

---

## Credits and Licenses

This project is licensed under the **GNU General Public License v2 or later** ([LICENSE](LICENSE)) due to its use of shadPS4 components. Third-party components keep their licenses: [shadPS4](https://github.com/shadps4-emu/shadPS4) video core and shader recompiler (GPL-2.0+), [sirit](https://github.com/shadps4-emu/sirit), [half](https://half.sourceforge.net/), [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) by FireBurn (MIT), AMD FidelityFX SDK (MIT), [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) (MIT), [Dear ImGui](https://github.com/ocornut/imgui) (MIT), DejaVu fonts, [dxil-spirv](https://github.com/HansKristian-Work/dxil-spirv) (MIT, used to build the FSR 4.1.1 assets). The DLSS bridge (`gpu/dlss_bridge`, MIT) and loader come from [Supermedo's Windows port](https://github.com/Supermedo/bloodborne_pc), adapted from [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR). AMD's FSR 4 DLLs and model data and NVIDIA's DLSS SDK and libraries are not distributed here.

Special thanks to the original creators and open source projects that made this port possible:

- **deadinside28**: Creator of the original Linux port (`bbport`), pioneering the flat memory image architecture, custom Vulkan two-stage pipeline, and synthetic motion vector generation.
- **shadPS4 Team**: Base Vulkan video core and shader recompiler.
- **FireBurn**: FSR-Vulkan runtime and temporal upscaling backend.
- **Thealexbarney**: LibAtrac9 audio decoding library.
- **ocornut**: Dear ImGui library for the in-game settings overlay.
- **Supermedo** and **IFreemz**: DLSS bridge.
- **Community Patch Authors**: Kyo, Lance McDonald, illusion, emoose, auser1337, and contributors for the 60 FPS, camera, and engine patches.
- **zackcage6**: testing and bug reporting.
