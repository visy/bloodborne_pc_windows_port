# Bloodborne PC — native Windows port

Run the PS4 version of *Bloodborne* (v1.09, any retail release) natively on Windows with Vulkan.
Based on [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28 (Linux) and the original
[Windows port](https://github.com/Mrsuss60/bloodborne_pc_windows_port) by Mrsuss60.

**Version 0.5.1** · Playable · Party co-op in development

> No game files are included: you need your own decrypted dump with update 1.09 (any retail
> release: CUSA03173, CUSA00900, ...; they share the same 1.09 executable).
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
| Game | Decrypted game folder (`CUSA03173` or another retail release), v1.09 (update merged into the base game); an `eboot.bin` with a 60 fps patch baked in is refused |

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

Play the whole game with friends in one session, from the first lamp to the end credits.
Modelled on Elden Ring Seamless Co-op, adapted to Bloodborne. **In development:** tested on one
PC (several game copies, simulated internet lag), not yet between two PCs. A scripted two-player
run from two new characters to the end credits passed, and a three-player travel run
([docs/party/campaign_soak.md](docs/party/campaign_soak.md)).

### How it plays

- **No PSN, no server.** One player hosts, the others join with the host's address and a password.
- **Start from the title screen:** *Party* → *New / Join / Continue party game*. The address and
  password are remembered, so later sessions are one press.
- **Fast join.** The bell, beckoner and summon-sign waits are gone: a guest is in the host's world
  within seconds.
- **Leave any time.** *System → Party → Leave the party* takes a guest home at once; the host's
  *Leave* ends the party and every guest goes home. *Join party* brings a guest back.
- **The session never ends by itself.** Boss kills, area clears, deaths and travel don't send
  anyone home.
- **No multiplayer walls.** The fog walls that keep phantoms near the host are gone; the party can
  split up and explore.
- **Travel together or alone.** Lamps, headstones and the Hunter's Mark ask "Travel with the party?".
  Yes moves the whole party with one loading screen; No moves only you, and the others don't load.
- **No loading screen inside the same map.** A lamp or Hunter's Mark to a spot in the map you are
  already in is an instant warp, for the whole party too.
- **Everyone can use lamps.** Vanilla disables lamps during co-op; here they work for everyone.
  A lamp lit by anyone is lit for the party.
- **Resting resets the world for the party.** Enemies respawn; everyone's HP, vials and bullets
  refill.
- **Guests are full players.** The Doll (level up), shops, the workbench, storage, lamps and
  headstones all work for guests.
- **Progress counts for everyone.** Bosses, items, doors, shortcuts, lamps, story and endings go
  into every player's own save. A guest who is behind catches up, including the Blood Moon.
- **Story cutscenes and endings play for everyone.** The Blood Moon, Gehrman's scenes and the
  ending with the end credits play for every party member.
- **The host's world is shared live, both ways.** A door or shortcut opened by the host is open
  for the guests too, and a lever or door a guest uses opens in the host's world (and in the
  guest's own save).
- **Guests can talk to NPCs** in the host's world (Gilbert, Iosefka, Eileen ...); quest choices
  are shared by the whole party.
- **Loot is per player.** Each player can take each world item once into their own save, and
  every player gets their own roll of each enemy's drop.
- **Up to 4 players** (host + 3), all in the host's world. The 4th player's summon can take a
  minute or two.
- **Same version for everyone.** A player whose bbport build, game files, patches, mods or party
  options differ is refused on joining; the message says what differs and what to do.
- **Chalice dungeons together.** Guests join the host's dungeon; levers, doors, the layer boss, its
  loot (for each player) and the lift work for the whole party. Any player can perform the ritual:
  the host's altar then offers *Enter the party's dungeon* (no glyph typing), and chalices are
  shared between all players.
- **Crashes recover.** A crashed game restarts and rejoins by itself.
- **Your single-player save is safe.** Party play uses a separate save, backed up every 10 minutes.
- **Death has a cost.** Each death stacks "rot" (−10% max HP or stamina, picked at random, up to
  5); using a lamp clears it.
  On by default; the host can turn it off on the *Party rules* page.
- **Enemies scale with the party, harder than vanilla.** Bosses get more health (and a little more
  damage) per extra hunter than the game gives them, normal enemies get tougher too; all six values
  (boss / enemy health, damage, poise) are on the *Enemy scaling* page, the host's count.
- **Names over party members**, in each player's colour, with level, health or deaths (your choice).
- **Party HUD.** Party members and events are shown in a small HUD; all party settings are on the
  *Party* pages of the game's *System* menu.

### Deaths

| Who dies | What happens |
|---|---|
| A guest | Respawns alone at a lit lamp in the party's area; the others keep playing |
| The host | The session stays; the party moves to the host's lamp with one loading screen |
| Everyone | Everyone respawns together; the session stays |

### Coming next

| Change | Status |
|---|---|
| Every player, host included, respawns at their own last lamp | In progress |
| Dying in a boss fight: spectate until the fight ends, then everyone respawns at one shared lamp | In progress |
| Bloodstains for everyone, like single-player | In progress |
| Guests open doors, pull levers, talk to NPCs; NPC choices shared | In progress |
| Item pickups and enemy drops per player: everyone gets their own | In progress |
| Every cutscene for the whole party (story cutscenes and endings already work) | In progress |
| Joining during a boss fight or cutscene waits outside until it ends | Works for boss fights (verified live); cutscenes in progress |
| The 4th player joins as fast as the others | In progress |
| Chalice dungeons: deeper layers checked in a party; entering the party's dungeon in one press | In progress |
| PvP / friendly fire the host can switch on and off | Later |
| Pings: party members and marked spots visible through walls, with distance and player colours | Later |

### Connecting over the internet

The host needs port 9307 (UDP and TCP) reachable. Most routers open it automatically (UPnP);
otherwise forward it by hand. Allow the Windows Firewall prompt once. Guests need nothing.

No UPnP and no port forward? Each guest copies its **guest code** (`BBG1-...`, *Party* page) and
sends it to the host, who presses *Add a guest code*: the game then connects through both routers
(hole punching). This fails behind symmetric / carrier-grade NAT (some mobile and ISP
connections); there, let another player host or use a VPN such as Tailscale. Details: [docs/party/network_robustness.md](docs/party/network_robustness.md).

Guide: [docs/PARTY.md](docs/PARTY.md) · Progress vs. Elden Ring Seamless Co-op:
[docs/party/ERSC_PARITY.md](docs/party/ERSC_PARITY.md)

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
| Mrsuss60 | Original Windows port: Win32 runtime, Vulkan on Windows, build scripts, character creation and keyboard fixes |
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
