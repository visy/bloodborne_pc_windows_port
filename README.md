# Native Windows Port of Bloodborne

![Platform](https://img.shields.io/badge/PLATFORM-WINDOWS-0078D6?style=for-the-badge&logo=windows&logoColor=white&labelColor=1a0f0f)
[![Latest release](https://img.shields.io/github/v/release/Mrsuss60/bloodborne_pc_windows_port?style=for-the-badge&logo=github&logoColor=white&labelColor=1a0f0f&color=8b0000&label=RELEASE)](https://github.com/Mrsuss60/bloodborne_pc_windows_port/releases)
[![Downloads](https://img.shields.io/github/downloads/Mrsuss60/bloodborne_pc_windows_port/total?style=for-the-badge&logo=github&logoColor=white&labelColor=1a0f0f&color=b22222&label=DOWNLOADS)](https://github.com/Mrsuss60/bloodborne_pc_windows_port/releases)
[![Stars](https://img.shields.io/github/stars/Mrsuss60/bloodborne_pc_windows_port?style=for-the-badge&logo=github&logoColor=white&labelColor=1a0f0f&color=c9a227&label=STARS)](https://github.com/Mrsuss60/bloodborne_pc_windows_port/stargazers)
[![Last commit](https://img.shields.io/github/last-commit/Mrsuss60/bloodborne_pc_windows_port?style=for-the-badge&logo=git&logoColor=white&labelColor=1a0f0f&color=6b6b6b&label=LAST%20COMMIT)](https://github.com/Mrsuss60/bloodborne_pc_windows_port/commits)
#
`bloodborne_pc_windows_port` is a native 64-bit Windows port of the PlayStation 4 executable of *Bloodborne* (CUSA03173, version 1.09). 

This project is a Windows adaptation of the original Linux port ([bbport](https://github.com/deadinside28/bloodborne_pc) by [deadinside28](https://github.com/deadinside28)). It replaces the Linux-specific kernel, memory mapping, and POSIX threading implementation with a native Win32 runtime, allowing the game to run directly on Windows with Vulkan.

The game's x86-64 code executes natively on your CPU without general emulation. System library calls are handled by a dedicated lightweight runtime, and GPU commands are translated directly to Vulkan with support for AMD FSR 3.1 and FSR 4 temporal upscaling.

> **No game files or copyrighted assets are included.** You must provide your own decrypted dump of Bloodborne (CUSA03173, version 1.09).
> This project is not affiliated with Sony Interactive Entertainment, FromSoftware, or AMD.

**Status: Playable on Windows.** The game boots, loads saves, and runs with audio, gamepad support, and state persistence.

---

## Windows Port Changes

This repository adapts the original Linux codebase specifically for Windows systems:

- **Win32 Memory & Synchronization**: Replaced Linux `mmap`, `mprotect`, and POSIX primitives with Windows `VirtualAlloc`, `VirtualProtect`, and native Win32 synchronization events (`src/win32_compat.c`, `src/win32_memory.c`, `src/runtime_host.c`).
- **Graphical Launcher**: Modular PySide6 launcher (`BloodborneLauncher.exe` / `bblauncherAPP/`), providing real-time configuration for target frame rates (30, 60, 90, uncap), render resolutions, FSR upscaling, community patches, and full interactive keybind remapping.
- **Command Line Launcher**: `run.bat` provides direct scripted startup, automatic patch compilation, and mod staging on Windows. Automatically prioritizes bundled portable Python runtimes (`python-embed/`).
- **Modern Mouse & Keyboard Support**: Relative cursor capture and mouse look adapted from the Linux port (`deadinside28`), engine deadzone jump compensation (~16/127), smoothing filters, and a modern press-to-rebind keybinding system with conflict auto-swapping.
- **Vulkan Frame Pacing**: Integrated presenter queue pacing (`BB_FRAMES_AHEAD=2`) to bound GPU run-ahead, stabilizing frametimes and smoothing 0.1% and 1.0% stutter lows.
- **Texture Barrier Fix**: Fixed dynamic texture streaming race conditions in `tile_manager.cpp`, ensuring item icons in the HUD and loading screens render correctly without static noise.
- **AT9 Audio Playback**: Bundles LibAtrac9 compilation to decode AT9 game audio natively on Windows with soft-clipping saturation curves to eliminate audio pops.
- **Windows Build System**: Standalone `build.bat` script that compiles the complete project with MinGW-w64 (GCC), CMake, and Ninja with multi-core parallel job support.

---

## Technical Highlights

- **Native Execution**: The game eboot is converted offline into a flat memory image; PS4 libc and libSceFios2 functions are linked directly into the binary. No CPU emulation or instruction translation occurs at run time.
- **High Framerate Support**: Community simulation patches allow running at 60 FPS, 90 FPS, or uncapped with accurate physics and movement speeds.
- **Synthetic Motion Vectors & Temporal Upscaling**: Because Bloodborne lacks an internal velocity buffer, the port computes camera motion from depth buffers and object motion from previous frame vertex positions. The scene can render at lower internal resolutions (e.g., 720p or 1080p) while FSR 3.1 / 4 upscales to your display resolution, leaving UI elements crisp at native resolution.
- **Persistent Pipeline Binary Caches**: Hardware-compiled Vulkan driver pipeline binaries (`.vkp`) are saved to disk and reloaded on startup, eliminating recurring shader compilation stutter.
- **Multi-threaded Draw Pipeline**: PS4 command buffers are decoded on one thread while draw calls are bound and recorded on another, preventing CPU bottlenecks.
- **In-Game Overlay**: Press `F1`, `Insert`, or `L3 + R3` on your controller to open the real-time configuration menu for resolution, sharpness, keybinds, and graphics toggles.

---

## Requirements

- **Operating System**: Windows 10 or Windows 11 (64-bit).
- **Graphics Card**: A Vulkan 1.3 capable GPU with current vendor drivers:
  - NVIDIA GeForce GTX 10-series or newer.
  - AMD Radeon RX 400-series or newer.
  - Intel Arc A-series or newer.
- **Processor**: x86-64 processor supporting AVX, SSE4.2, and BMI1 (AVX2 no longer mandatory; non-FSGSBASE CPUs gracefully fall back to Windows TLS).
- **Game Files**: Decrypted `CUSA03173` directory containing `eboot.bin` (version 1.09).

*(Note: Release archives include a pre-configured `python-embed/` runtime; manual Python installation is not required for release users).*

### Build Dependencies (Only if compiling from source)
- MinGW-w64 GCC (via [w64devkit](https://github.com/skeeto/w64devkit) or MSYS2 MinGW64).
- CMake 3.25+.
- Ninja build tool.
- SDL3 development libraries.
- Vulkan SDK or Vulkan-Headers.

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

### 2. Launch the Game

#### Option A: Using the Graphical Launcher (Recommended)
Double-click `BloodborneLauncher.exe` (or run it from the root directory):
- Select your `eboot.bin` file or game folder.
- Select your target frame rate (60 FPS recommended for smooth frame pacing).
- Select your internal resolution (e.g., 1280x720 with FSR enabled for high performance).
- Enable desired patches under the Patches tab (e.g., *Skip Intro*, *Performance Patch*).
- Configure mouse look sliders and customize keybinds under the Settings tab.
- Click **Launch Bloodborne**.

#### Option B: Using the Command Line
Run `run.bat` pointing to your game directory:
```cmd
run.bat "C:\Games\Bloodborne\CUSA03173"
```

Save files and shader caches are stored in `user/`. Settings are saved to `bbport.ini`.

---

## Controls and In-Game Features

- **Mouse & Keyboard**:
  - **Mouse Look**: Relative cursor capture with seamless release when opening menus or alt-tabbing.
  - **Light Attack (R1)**: Mouse Left
  - **Heavy / Charged Attack (R2)**: Shift + Mouse Left
  - **Firearm / Gun (L2)**: Mouse Right
  - **Weapon Transform (L1)**: Tab
  - **Lock-On / Reset Camera (R3)**: Mouse Middle
  - **Move**: W / A / S / D
  - **Dodge / Roll / Sprint**: Space (hold to sprint, tap while sprinting to jump)
  - **Interact / Talk**: E (or Enter)
  - **Use Item**: R
  - **Heal / Blood Vial**: F
  - **Blood Bullets (Up)**: 1
  - **Switch Item (Down)**: 2
  - **Switch Left Weapon (Left)**: 3
  - **Switch Right Weapon (Right)**: 4
  - **Gestures Menu (Touchpad L)**: G
  - **Personal Effects (Touchpad R)**: Backspace
  - **System Menu (Options)**: Escape
- **Gamepad**: Controllers are supported out of the box through SDL3.
- **In-Game Settings**: Press `F1`, `Insert` on keyboard, or `L3 + R3` on gamepad to toggle the overlay menu. Changes write to `bbport.ini`. Language can be toggled between English and Russian under *Other → Language*.
- **Free Camera**: When enabled in the launcher or overlay, hold `Cross` and press `L3` to cycle camera modes.
- **Debug Menu**: If debug fonts (`DbgFont14h.ccm` and `DbgFont14h.tpf`) are installed into `dvdroot_ps4/font/`, open the menu using the controller touchpad.

---

## Repository Layout

| Path | Description |
|---|---|
| `src/` | Win32 loader (`probe.c`), memory virtualization, and PS4 HLE runtime |
| `gpu/` | Vulkan video core, multi-stage command pipeline, and upscaler integration |
| `bblauncherAPP/` | Modular PySide6 desktop launcher application source and packaging scripts |
| `scripts/` | Eboot preparation, patch compiler (`patches.py`), and instance guard (`bb_guard.py`) |
| `patches/` | Community XML patches for Bloodborne version 1.09 |
| `build.bat` | Multi-core Windows compilation script |
| `run.bat` | Direct execution batch script with embedded Python fallback |
| `tools/` | Developer utilities and patch extraction scripts |
| `docs/` | Architecture notes, upscaler documentation, and change logs |

---

## Credits and Licenses

This project is licensed under the **GNU General Public License v2 or later** ([LICENSE](LICENSE)) due to its use of shadPS4 components.

Special thanks to the original creators and open source projects that made this port possible:

- **deadinside28**: Creator of the original Linux port (`bbport`), pioneering the flat memory image architecture, custom Vulkan two-stage pipeline, relative mouse look implementation, and synthetic motion vector generation.
- **shadPS4 Team**: Base Vulkan video core and shader recompiler.
- **FireBurn**: FSR-Vulkan runtime and temporal upscaling backend.
- **Thealexbarney**: LibAtrac9 audio decoding library.
- **ocornut**: Dear ImGui library for the in-game settings overlay.
- **Community Patch Authors**: Kyo, Lance McDonald, illusion, emoose, auser1337, and contributors for the 60 FPS, camera, and engine patches.
- **zackcage6**: testing and bug reporting.
- **[ALuaNegra](https://github.com/ALuaNegra)**: pull request [#89](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/89) (transient depth target reclamation and VRAM pressure mitigation).
- **[parsifal295](https://github.com/parsifal295)**: pull requests [#35](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/35), [#36](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/36), [#37](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/37).
- **[wizardnun](https://github.com/wizardnun)**: pull request [#39](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/39).
- **[Vorshim92](https://github.com/Vorshim92)**: pull requests [#65](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/65), [#66](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/66), [#67](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/67), [#73](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/73), [#74](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/74), [#76](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/76), [#77](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/77).
