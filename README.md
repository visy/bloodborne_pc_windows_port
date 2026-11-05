# Bloodborne PC: native Windows port

Run the PS4 version of *Bloodborne* (v1.09, any retail release) natively on Windows with Vulkan. No emulator.
Based on [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28 (Linux) and the original
[Windows port](https://github.com/Mrsuss60/bloodborne_pc_windows_port) by Mrsuss60. GPL-3.0-or-later.

**Version 0.7.8** · Playable · Party co-op in development

> No game files are included: you need your own decrypted dump with update 1.09 (CUSA03173, CUSA00900, ...).
> Not affiliated with Sony, FromSoftware, AMD or NVIDIA. Not related to shadPS4 (the Linux port's questions go to its
> [Discord](https://discord.gg/KYZRKk9CB)).

## Highlights

- **Party co-op, no server**: the whole campaign with up to 3 friends over a direct connection. [Details](#party-co-op)
- **Settings in the game's own System menu**, gamepad friendly.
- **Keyboard, mouse and any gamepad**, every key rebindable. [Controls](docs/CONTROLS.md)
- **30 to 120 fps or uncapped** with correct game speed (Live FPS).
- **Upscaling and frame generation**: FSR 3.1 / 4 / 4.1.1, DLSS, DLSS FG, FSR FG, Reflex, Anti-Lag 2, HDR.
- **No shader stutter**, shareable shader cache, crash-safe saves, mods and XML patches, The Old Hunters DLC.

Full list: [docs/FEATURES.md](docs/FEATURES.md).

## Requirements

| | |
|---|---|
| OS | Windows 10 / 11, 64-bit |
| GPU | Vulkan 1.3: NVIDIA GTX 10-series+, AMD RX 400+, Intel Arc |
| CPU | x86-64 with AVX (Intel Sandy Bridge / AMD FX or newer); older ones run slower |
| Game | Decrypted game folder, v1.09 (e.g. `CUSA03173`) |

Details (CPU, Python, game folder): [docs/INSTALL.md](docs/INSTALL.md).

## Quick start

1. Get a release, or [build from source](docs/BUILDING.md).
2. Put your game folder next to the port (`..\CUSA03173`), or pick it in the launcher.
3. Start **`launch_gui.bat`**, choose your settings, click **Launch Bloodborne**
   (or `run.bat "C:\Games\Bloodborne\CUSA03173"`).

Saves and the shader cache live in `user\`; settings in `bbport.ini` next to `launch_gui.bat`.

## Controls

Gamepad works out of the box; the keyboard always works too.
**Insert** opens the in-game overlay (Controls tab: rebind any key). Full tables, mouse & keyboard
mode, button prompts and `bbport.ini` bindings: [docs/CONTROLS.md](docs/CONTROLS.md).

## Settings

Every port setting is a page in the game's **System** menu; the launcher has the same options.
Mouse, graphics, latency, HDR, map, shader cache, environment variables: [docs/SETTINGS.md](docs/SETTINGS.md).

## Party co-op

Play from the first lamp to the credits with friends, modelled on Elden Ring Seamless Co-op.
Direct connection, your single-player save is never touched. Everyone needs the same version and game files.

| | |
|---|---|
| Host | Title screen, **Party**, **New party game**; send the party code (*System > Party > Copy Internet code*) |
| Guest | Title screen, **Party**, paste the code, **Join party game** (after the host is running) |
| Later | **Continue party game**: code and password are remembered |
| Network | Host needs port 9307 UDP + TCP (UPnP is automatic); behind CGNAT use a VPN (Radmin, Tailscale, ZeroTier) |

**What it does:** up to 4 players in the host's world, fast join, shared kills and doors, shared progress
(lamps, quests, bosses), per-player pickups, boss-aware death and respawn, ping, gather, chat, item giving, invasions.

Step by step, every feature with its toggle, deaths, network options, known issues:
**[docs/COOP.md](docs/COOP.md)** · hosting, firewall, saves: [docs/PARTY.md](docs/PARTY.md).

### Seamless co-op progress

Plan done vs. evidence-weighted parity with Elden Ring Seamless Co-op ([how it is scored](docs/COOP.md#progress-vs-seamless-co-op), [row by row](docs/party/ERSC_PARITY.md)).

| Area | Plan done | | ERSC parity (evidence-weighted) | |
|---|---|--:|---|--:|
| **Overall** | `████████████████████` | 98% | `██████████████████░░` | 90% |
| Session & joining | `████████████████████` | 99% | `███████████████████░` | 94% |
| World & travel | `████████████████████` | 100% | `███████████████████░` | 93% |
| Death & respawn | `████████████████████` | 100% | `██████████████████░░` | 90% |
| Progress (anyone can progress) | `███████████████████░` | 95% | `█████████████████░░░` | 85% |
| Services & UI | `████████████████████` | 100% | `██████████████████░░` | 89% |
| Network & robustness | `███████████████████░` | 95% | `██████████████████░░` | 88% |
| Bloodborne-specific (chalice, Insight) | `████████████████████` | 100% | `██████████████████░░` | 90% |
| Feels like single player in a party | `████████████████████` | 100% | `██████████████████░░` | 90% |

## Troubleshooting

Game will not start, co-op will not connect, white FSR 4 picture, bug reports: [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md).

## Building

Windows, MinGW-w64, CMake, Ninja: [docs/BUILDING.md](docs/BUILDING.md) (also DLSS, FSR 4 assets, repository layout).

## Documentation

| Topic | File |
|---|---|
| Install and requirements | [docs/INSTALL.md](docs/INSTALL.md) |
| Full feature list | [docs/FEATURES.md](docs/FEATURES.md) |
| Controls and key bindings | [docs/CONTROLS.md](docs/CONTROLS.md) |
| Settings, launcher, environment variables | [docs/SETTINGS.md](docs/SETTINGS.md) |
| Co-op player guide | [docs/COOP.md](docs/COOP.md), [docs/PARTY.md](docs/PARTY.md) |
| Troubleshooting | [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) |
| Building from source | [docs/BUILDING.md](docs/BUILDING.md) |
| Frame generation, Reflex, latency | [docs/FG_REFLEX.md](docs/FG_REFLEX.md), [docs/LATENCY.md](docs/LATENCY.md) |
| Shaders and the shader cache | [docs/SHADERS.md](docs/SHADERS.md) |
| Saves and backups | [docs/SAVES.md](docs/SAVES.md) |
| Mods, XML patches, DLC | [docs/MODS.md](docs/MODS.md) |
| Upscalers | [docs/upscaler.md](docs/upscaler.md) |
| How the port works | [docs/WINDOWS_PORT.md](docs/WINDOWS_PORT.md) |
| Patches other forks can reuse | [docs/PATCHES_FOR_FORKS.md](docs/PATCHES_FOR_FORKS.md) |
| Changes | [CHANGES.md](CHANGES.md), [docs/](docs) |

## Credits

**GPL-3.0-or-later** ([LICENSE](LICENSE)); third-party parts keep their own licenses. Per-contribution detail:
[docs/CREDITS.md](docs/CREDITS.md).

| Who | What |
|---|---|
| deadinside28 | Original Linux port (bbport) |
| Mrsuss60 | Original Windows port, Live FPS, texture quality, many fixes |
| shadPS4 team | Vulkan video core, shader recompiler |
| droogie and the bbhost team | Stability fixes, menus, summons, appearance editor, high-FPS fixes ([bbhost](https://github.com/droogie/bbhost)) |
| Supermedo, IFreemz | DLSS bridge, Windows loader, many ported fixes ([bloodborne_pc](https://github.com/Supermedo/bloodborne_pc)) |
| FIocker | Frame generation, Reflex |
| T0ug, parsifal295, cassioKenji, emurphy04, imedved, mcrib884, andrecabralgit, Jgamrot, dGstone18, RafaelHGOliveira, fliperama86, bmy, Awaken | Fixes ported from the Supermedo fork and PRs |
| Vorshim92 | Pipeline cache, stale-exe guard, crash reaper, Reset shader cache |
| ALuaNegra, HatsyRei | VRAM fix, licence notices (via Mrsuss60's port) |
| iannitram, Kyoskii, facundo1701 | FSR memory-type fix, high-FPS fixes, `eboot.elf` support |
| FireBurn | FSR-Vulkan, AMD FidelityFX SDK (MIT) |
| MikeTat1988 | [Hunter's Atlas](https://github.com/MikeTat1988/HuntersAtlas): the in-game map |
| Noxde, Valentino Amato | [Bloodborne-save-editor](https://github.com/Noxde/Bloodborne-save-editor) |
| Thealexbarney, ocornut | LibAtrac9, Dear ImGui |
| Kyo, Lance McDonald, illusion, emoose, auser1337 | 60 FPS, camera and engine patches |
| vladtepes22, zackcage6 | Co-op fixes (PRs #6 to #8), testing and bug reports |
