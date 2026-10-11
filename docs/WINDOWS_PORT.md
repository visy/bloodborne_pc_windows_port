# How the Windows port works

Back to the [README](../README.md).

This project is a Windows adaptation of the original Linux port ([`bbport`](https://github.com/deadinside28/bloodborne_pc) by `deadinside28`). It replaces the Linux-specific kernel, memory mapping, and POSIX threading implementation with a native Win32 runtime, allowing the game to run directly on Windows with Vulkan.

bbport is the counterpart of Wine + DXVK for a single game: the game's original executable runs directly on the PC.

- **As in Wine**, the game's x86-64 code runs on the CPU directly, and a runtime written for this one game replaces the PS4 system libraries.
- **As in DXVK**, the game's graphics are translated to Vulkan by a renderer derived from [shadPS4](https://github.com/shadps4-emu/shadPS4) and heavily extended for this game, including temporal upscaling with AMD FSR 3.1, FSR 4 and FSR 4.1.1 (and NVIDIA DLSS when built with NVIDIA's SDK).


## Windows port changes

This repository adapts the original Linux codebase specifically for Windows systems:

- **Win32 Memory & Synchronization**: Replaced Linux `mmap`, `mprotect`, and POSIX primitives with Windows `VirtualAlloc`, `VirtualProtect`, and native Win32 synchronization events (`src/win32_compat.c`, `src/win32_memory.c`, `src/runtime_host.c`).
- **Graphical Launcher**: Includes `launch_gui.bat` (powered by `launcher.py`), a desktop control panel to configure target frame rates (30, 60, 90, uncap), rendering resolution, FSR upscaling, monitor choice, and all toggleable community patches from `Bloodborne.xml`.
- **Command Line Launcher**: `run.bat` provides direct scripted startup, automatic patch compilation, and mod staging on Windows. Started on its own, it applies the launcher's last saved settings (`launcher_settings.json`; `BB_NO_LAUNCHER_SETTINGS=1` skips them).
- **Vulkan Frame Pacing**: Integrated presenter queue pacing (`BB_FRAMES_AHEAD=2`) to bound GPU run-ahead, stabilizing frametimes and smoothing 0.1% and 1.0% stutter lows.
- **Texture Barrier Fix**: Fixed dynamic texture streaming race conditions in `tile_manager.cpp`, ensuring item icons in the HUD and loading screens render correctly without static noise.
- **AT9 Audio Playback**: Bundles LibAtrac9 compilation to decode AT9 game audio natively on Windows.
- **Windows Build System**: Standalone `build.bat` script that compiles the complete project with MinGW-w64 (GCC), CMake, and Ninja.
- **Launcher with categories and tooltips**: settings grouped in tabs (Display, Graphics, Performance & Shaders, Game, Input & Audio, Advanced / Debug); hover any option for what it does, its trade-off, default and environment variable.
- **No shader stutter in known areas**: before the intro logos the port builds every pipeline in the shader cache on all cores, with a progress screen the first time (`BB_SHADER_PRECOMPILE`, on by default; Esc skips). *Async shader compilation* (`BB_ASYNC_SHADERS`, on by default) and the optional *graphics pipeline library* (`BB_GPL`) handle shaders the cache does not have yet. A small "Compiling shaders: NN%" note appears only on loading screens and menus, never in gameplay (`BB_COMPILE_INDICATOR`).
- **Shareable, GPU-independent shader cache**: export and import it as one `.zip` from the launcher; a cache made on another GPU is rebuilt for yours before the intro (see [Sharing the shader cache](#sharing-the-shader-cache)).
- **The Old Hunters automatically**: its data ships with patch 1.09; `run.bat` reports the add-on as installed on a fresh install (`BB_AUTO_DLC=0` skips this).
- **Window and audio**: `Alt+Enter` switches fullscreen at any time; the game is muted while its window is in the background (launcher option, `BB_MUTE_UNFOCUSED`); the mouse cursor shows only while the overlay menu is open.
- **Clean exit**: closing the window, the in-game restart and timeouts end the process without unloading the Vulkan driver under the loader lock, which could leave a hung process holding the game's memory.
- **VRAM guard**: *Full* VRAM pre-upload drops to normal on GPUs with less than ~11 GB (`BB_PREUPLOAD_FORCE=1` keeps it).
- **Upstream 0.5** ([changes](docs/CHANGES_0.5.md)): the game's own *System → Display / Effects* pages with slider-style choices (applied once a slider rests for 0.4 s), the GPU command decoder and game profiles, GPU timestamps, fewer hitches from background pipeline compiles (full-screen passes and indirect draws now wait for their pipeline: no grey frames), the texture collector's emergency pass on low-VRAM cards, the damaged-extraction check and the shared write traps of the memory layer, all built natively for Windows. Upstream's *Auto* memory model (the new model on AMD) is Linux-only: it needs the game's memory in dma-buf/memfd chunks that the Windows runtime cannot provide, so Windows keeps the 0.3 memory model with every fix since (`BB_PC_MODEL` is still accepted). The AppImage-only parts (NVIDIA EGL vendor files, RADV `nohiz`, MangoHud) do not apply on Windows. The overlay menu keeps this port's game-styled tabbed layout.
- **Upstream 0.5-pre4** ([changes](docs/CHANGES_0.5-pre4.md)): the memory module's fixes, the command-recording crash fix (#100, reported on Windows), the damaged-extraction check (#81: `run.bat` and the launcher report game files whose shaders do not unpack; `python scripts\game_check.py GAME_DIR` checks every file) are included. The settings menu keeps this port's game-styled layout (tabs, rows like the game's dialogs) rather than upstream's return to the 0.3-style window. The drivers' shader-cache variables `run.sh` sets (`__GL_SHADER_DISK_CACHE*`, `MESA_SHADER_CACHE_MAX_SIZE`) are Linux-only; the NVIDIA driver on Windows uses its control-panel *Shader Cache Size* setting.

---

## Technical highlights

- **Native Execution**: The game eboot is converted offline into a flat memory image; PS4 libc and libSceFios2 functions are linked directly into the binary. No CPU emulation or instruction translation occurs at run time.
- **High Framerate Support**: Community simulation patches allow running at 60 FPS, 90 FPS, or uncapped with accurate physics and movement speeds.
- **Synthetic Motion Vectors & Temporal Upscaling**: Because Bloodborne lacks an internal velocity buffer, the port computes camera motion from depth buffers and object motion from previous frame vertex positions. The scene can render at lower internal resolutions (e.g., 720p or 1080p) while FSR 3.1 / 4 / 4.1.1 (or DLSS) upscales to your display resolution, leaving UI elements crisp at native resolution.
- **Multi-threaded Draw Pipeline**: PS4 command buffers are decoded on one thread while draw calls are bound and recorded on another, preventing CPU bottlenecks.
- **No online/offline screen**: there is no PSN, so the *Play online / offline* choice is skipped by a game patch (*Skip Online/Offline Choice*) and the game opens its main menu offline. The launcher's *Skip online/offline choice* switch, or `BB_SKIP_NETWORK_CHOICE=0`, shows the screen again.

## Wine + DXVK for one game

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

## How it differs from shadPS4

| | shadPS4 | bbport |
|---|---|---|
| Scope | General PS4 emulator, many games | One game: Bloodborne v1.09 |
| Loading | Its own ELF loader and kernel emulation at run time | The eboot is converted offline (`scripts/`) into an image with PS4 libc/Fios2 linked in; a C loader maps it and jumps into the game |
| System libraries | Broad HLE of the PS4 OS | A small runtime (`src/runtime_*.c`) that implements exactly what Bloodborne calls: memory, threads, sync, files, audio (incl. ATRAC9), pad, saves, AppContent |
| GPU | shadPS4 video core and shader recompiler | The same core (vendored, GPL) with marked changes (`bbport:`) plus new modules: two-stage draw pipeline, render-state and texture-set memoization, render-scale proxies, motion vectors, FSR 3.1/4/4.1.1, DLSS bridge, frame capture and GPU profiler |
| GPU thread | One thread processes the whole command stream (the bottleneck in Bloodborne) | Decode and draw recording run on separate threads |
| Upscaling | — | Temporal (FSR 3.1, FSR 4, FSR 4.1.1, DLSS) with the game's own motion vectors and jitter |
| Game patches | Patch files applied by the emulator | The same community patches, compiled at start (`scripts/patches.py`); render resolution, effects and FPS from the launcher |


> Upstream (Linux) also ships a GTK4 launcher, an AppImage for the Steam Deck, MangoHud integration and `run.sh`/`build.sh`; those are kept in this repository for reference but are not used on Windows.
