# Bloodborne PC — native Windows port

Run the PS4 version of *Bloodborne* (CUSA03173, v1.09) natively on Windows with Vulkan.
Based on [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28 (Linux).

**Version 0.5** · Playable · Party co-op in development

> No game files are included: you need your own decrypted dump (CUSA03173, v1.09).
> Not affiliated with Sony, FromSoftware, AMD or NVIDIA. Not related to shadPS4: questions about
> the Linux port go to its [Discord](https://discord.gg/KYZRKk9CB).

---

## What this port adds

| Feature | What you get |
|---|---|
| **Native on Windows** | No emulator, no Wine. The game's own code runs on your CPU; the graphics go through Vulkan. |
| **Party co-op, no server** | Play the whole campaign with friends over a direct connection: no PSN, no server. Modelled on Elden Ring Seamless Co-op. [More](#party-co-op) |
| **Settings in the game's menu** | Every port setting is a page in the game's own *System* menu, with gamepad navigation. |
| **High frame rates** | 30 / 60 / 90 / uncapped, with correct game speed (community patches). |
| **Upscaling** | FSR 3.1, FSR 4, FSR 4.1.1 and DLSS, with motion vectors added for this game. |
| **No shader stutter** | Known shaders are built before the intro. New ones compile in the background. |
| **Shareable shader cache** | Export / import one `.zip`, works across GPUs. |
| **Save safety** | Crash-safe saving, save checks at start, automatic backups in party play. |
| **The Old Hunters** | DLC enabled automatically (its data ships with patch 1.09). |
| **Mods and patches** | Loose-file mods and shadPS4-format XML patches, with load order. |

## Requirements

| | |
|---|---|
| OS | Windows 10 / 11, 64-bit |
| GPU | Vulkan 1.3: NVIDIA GTX 10-series+, AMD RX 400+, Intel Arc |
| CPU | x86-64 with AVX2 |
| Python | 3.10+ (launcher and patch compiler) |
| Game | Decrypted `CUSA03173` folder, v1.09 (update merged into the base game) |

## Quick start

1. Get a release, or [build from source](docs/BUILDING.md).
2. Put your game folder next to the port (`..\CUSA03173`), or pick it in the launcher.
3. Start **`launch_gui.bat`**, choose your settings, click **Launch Bloodborne**.
   Or run `run.bat "C:\Games\Bloodborne\CUSA03173"`.

Saves and the shader cache are stored in `user\`. Settings are stored in `bbport.ini`.

## Controls

| Action | Gamepad | Keyboard |
|---|---|---|
| Port settings | *System* in the game's menu | *System* in the game's menu |
| Overlay menu (fallback) | `L3 + R3` (enable in launcher) | `Insert` |
| Gestures / key items | Touchpad left / right half | `Tab` / `Backspace` |
| Fullscreen | — | `Alt + Enter` |

Mouse and keyboard, free camera, debug menu: [docs/SETTINGS.md](docs/SETTINGS.md).

## Party co-op

| | |
|---|---|
| Players | Up to 4 (host + 3; 3 tested so far) |
| Connection | Direct P2P with a password. No PSN, no server. The host may need port 9307 open (UPnP does this on most routers). |
| Start | Title screen → **Party** → *New / Join / Continue party game*. The address and password are remembered. |
| Travel | One loading screen for the whole party. Bosses, deaths and lamps don't end the session. |
| Progress | Bosses, items, doors, lamps and story go into every player's own save. |
| Guests | Can use the Doll, shops, workbench, storage, lamps and headstones. |
| Saves | Separate party save. Your single-player save is never touched. |
| Status | In development. Tested on one PC (several game copies, simulated lag); **not yet tested between two PCs**. |

Guide: [docs/PARTY.md](docs/PARTY.md) · Progress vs. Elden Ring Seamless Co-op:
[docs/party/ERSC_PARITY.md](docs/party/ERSC_PARITY.md) · Internet / NAT:
[docs/party/network_robustness.md](docs/party/network_robustness.md)

## Documentation

| Topic | File |
|---|---|
| Building from source, DLSS, FSR 4 assets | [docs/BUILDING.md](docs/BUILDING.md) |
| Settings, launcher, controls, environment variables | [docs/SETTINGS.md](docs/SETTINGS.md) |
| Shader compilation and sharing the cache | [docs/SHADERS.md](docs/SHADERS.md) |
| Saves, backups, restoring, game file check | [docs/SAVES.md](docs/SAVES.md) |
| Mods, XML patches, DLC | [docs/MODS.md](docs/MODS.md) |
| How the port works, vs. shadPS4, upstream merges | [docs/WINDOWS_PORT.md](docs/WINDOWS_PORT.md) |
| Upscalers | [docs/upscaler.md](docs/upscaler.md) |
| Changes | [CHANGES.md](CHANGES.md), [docs/](docs) |

## Repository layout

| Path | Contents |
|---|---|
| `src/` | Win32 loader, memory management, PS4 runtime |
| `gpu/` | Vulkan renderer, upscalers, DLSS bridge, party co-op (`gpu/shim/party`) |
| `scripts/` | Eboot preparation, game check, patch compiler |
| `patches/` | Community XML patches for v1.09 |
| `launcher.py`, `launch_gui.bat`, `run.bat`, `build.bat` | Windows launcher and scripts |
| `tools/`, `tests/`, `docs/` | Developer tools, tests, documentation |
| `launcher/`, `run.sh`, `build.sh` | Upstream Linux scripts (reference only) |

## Credits and licenses

**GPL-3.0-or-later** ([LICENSE](LICENSE)). Third-party parts keep their own licenses. Full text and license history: [docs/CREDITS.md](docs/CREDITS.md).

| Who | What |
|---|---|
| deadinside28 | Original Linux port (bbport): flat memory image, two-stage Vulkan pipeline, motion vectors |
| shadPS4 team | Vulkan video core and shader recompiler (GPL-2.0+), sirit |
| droogie and the bbhost team | Long-session stability fixes ([bbhost](https://github.com/droogie/bbhost), GPL-3.0+) |
| FireBurn | FSR-Vulkan (MIT); AMD FidelityFX SDK (MIT) |
| Supermedo, IFreemz | DLSS bridge and Windows loader |
| Thealexbarney | LibAtrac9 audio (MIT) |
| ocornut | Dear ImGui (MIT) |
| Kyo, Lance McDonald, illusion, emoose, auser1337 | 60 FPS, camera and engine patches |
| zackcage6 | Testing and bug reports |

Also used: half, DejaVu fonts, dxil-spirv (MIT). AMD's FSR 4 files and NVIDIA's DLSS SDK are not
included here. Release packages include NVIDIA's `nvngx_dlss.dll` under its license
(`out/NVIDIA_DLSS_LICENSE.txt`).
