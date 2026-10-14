# Bloodborne PC — native Windows port

Run the PS4 version of *Bloodborne* (v1.09, any retail release) natively on Windows with Vulkan.
Based on [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28 (Linux) and the original
[Windows port](https://github.com/Mrsuss60/bloodborne_pc_windows_port) by Mrsuss60.

**Version 0.6.1** · Playable · Party co-op in development

> No game files are included: you need your own decrypted dump with update 1.09 (any retail
> release: CUSA03173, CUSA00900, ...; they share the same 1.09 executable).
> Not affiliated with Sony, FromSoftware, AMD or NVIDIA. Not related to shadPS4: questions about
> the Linux port go to its [Discord](https://discord.gg/KYZRKk9CB).

### Seamless co-op progress

Parity with Elden Ring Seamless Co-op, measured row by row in [ERSC_PARITY.md](docs/party/ERSC_PARITY.md).

| Area | Progress | |
|---|---|--:|
| **Overall** | `█████████████████░░░` | 84% |
| Session & joining | `██████████████████░░` | 89% |
| World & travel | `██████████████████░░` | 88% |
| Death & respawn | `██████████████████░░` | 89% |
| Progress (anyone can progress) | `█████████████████░░░` | 87% |
| Services & UI | `██████████████████░░` | 90% |
| Network & robustness | `█████████████████░░░` | 86% |
| Bloodborne-specific (chalice, Insight) | `████████████████░░░░` | 81% |
| Feels like single player in a party | `█████████████████░░░` | 87% |

---

## What this port adds

| Feature | What you get |
|---|---|
| **Native on Windows** | No emulator, no Wine. The game's own code runs on your CPU; the graphics go through Vulkan. |
| **Party co-op, no server** | Play the whole campaign with friends over a direct connection: no PSN, no server. Modelled on Elden Ring Seamless Co-op. [More](#party-co-op) |
| **Settings in the game's menu** | Every port setting is a page in the game's own *System* menu, with gamepad navigation. |
| **High frame rates** | 30 / 60 / 90 / uncapped, with correct game speed (community patches). |
| **Upscaling** | FSR 3.1, FSR 4, FSR 4.1.1 and DLSS, with motion vectors added for this game. |
| **Frame generation** | Optional, off by default: DLSS FG (2x-4x, RTX 40/50) or FSR FG (2x), paced, HUD kept sharp; NVIDIA Reflex. [More](docs/FG_REFLEX.md) |
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
| CPU | x86-64 with AVX2 (Intel Haswell / AMD Ryzen or newer); FSGSBASE is not needed |
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
| Gestures / key items | Touchpad left / right half (Xbox pads: `Back` + d-pad left / right; `Back` alone the left half) | `Tab` / `Backspace` |
| Fullscreen | — | `Alt + Enter` |

Mouse and keyboard, free camera, debug menu: [docs/SETTINGS.md](docs/SETTINGS.md).

## Party co-op

Play the whole game with friends in one session, from the first lamp to the end credits.
Modelled on Elden Ring Seamless Co-op, adapted to Bloodborne. **In development:** tested on one
PC (several game copies, simulated internet lag), **between two PCs on a LAN** (summon, combat,
shared kills and doors, per-player pickups) and **over the Internet**: a 2.5 h session between two
PCs in different homes (two campaigns, prologue to Old Yharnam, several hosts and joins, one crash with
a working rejoin). The bugs reported from it are fixed in 0.6.1
([docs/party/playtest1_triage.md](docs/party/playtest1_triage.md)). A scripted two-player
run from two new characters to the end credits passed, and a three-player travel run
([docs/party/campaign_soak.md](docs/party/campaign_soak.md)).

The host shares a **party code** such as `BBP3-NJR0-E43N-WWN9-2C2P`: it holds the host's address and a secret,
so share it only with the people you play with. With a password the code is shorter (19
characters without one); players on the host's own network can use the shorter **LAN code**
(e.g. `BBP3-PAJY-EAMH-7TD`). Everyone needs the same bbport version and game files. Party play uses its
own save; your single-player save is never touched.

### Start from the title screen (recommended)

Nothing to set up outside the game, and later sessions are a single press.

| | Host | Guest |
|---|---|---|
| 1 | Title screen → **Party** → set a **Password** (optional, gives a shorter code) | Title screen → **Party** → **Host address**: paste the host's party code (**Ctrl+V**, e.g. `BBP3-NJR0-E43N-WWN9-2C2P`) → **Password** (the host's, if it set one) |
| 2 | **New party game** (first time: new character) or **Continue party game** | **Join party game** (first time) or **Continue party game**, once the host's game is running |
| 3 | Allow the Windows Firewall prompt once; copy your code (*System → Party → Copy Internet code*) and send it | The game restarts into party mode and puts you beside the host |

Code and password are remembered, so later sessions are just *Continue party game* for everyone.
Settings: *System → Party* pages in the game's menu.

### Start from the launcher

The same party, set up in `launch_gui.bat` → **Party** tab (handy to see and copy the code
before anyone is in the game):

- **Host:** *Mode* = **Host a party**, enter **Your name**, optionally a **Password**, click
  **Launch Bloodborne**. Once the game is hosting, the codes appear under **Internet code** (friends elsewhere)
  and **LAN code** (same router); click **Copy** next to one and send it. Allow the Windows Firewall
  prompt once.
- **Guest:** *Mode* = **Join a party**, enter **Your name**, click **Paste** next to **Party code**
  (e.g. `BBP3-NJR0-E43N-WWN9-2C2P`), enter the same **Password** as the host, and click **Launch Bloodborne** after the
  host's game is running. A ✓ / ✗ line under the code says whether it is valid.
- At home (same router) use the **LAN code**: most routers cannot reach their own public address.
- Friends cannot connect? Leave **UPnP** on, or forward the port (default 9307, UDP and TCP) to the
  host PC in the router, or use a VPN such as Tailscale and give the VPN address as `host:port`
  (see *Connecting over the internet* below).

### How it plays

- **No PSN, no server.** One player hosts, the others join with the host's party code.
- **Start from the title screen:** *Party* → *New / Join / Continue party game* (press Cross; the
  ones that restart the game ask for a second press). The code and password are remembered, so
  later sessions are one press. *Continue party game* shows whenever the party saves hold a
  character, also if you started party games from the launcher.
- **Fast join.** The bell, beckoner and summon-sign waits are gone: a guest is in the host's world
  within seconds. *Continue party game* takes a guest straight into the host's world with a single
  loading screen. If the host travels while you are joining, its travel waits until you are in and
  you travel together.
- **Leave any time.** *System → Party → Leave the party* takes a guest home at once; the host's
  *Leave* ends the party and every guest goes home. *System → Party mode → Join party* brings a
  guest back; *Host party* and *Single player* are there too.
- **The session never ends by itself.** Boss kills, area clears, deaths and travel don't send
  anyone home.
- **No multiplayer walls.** The fog walls that keep phantoms near the host are gone; the party can
  split up and explore. There is no leash: a guest can walk far from the host (checked at 140 m)
  and stays in the party.
- **Anyone can go through a boss fog first.** A guest's fog no longer waits for the host: the
  fight starts for everyone, and the others follow through the fog. A boss's first fight with an
  intro cutscene still starts with the host; a guest at that fog is told so on screen.
- **Travel from any lamp to any lamp.** A lamp offers the headstones' list (the Hunter's Dream and every lit
  lamp) and takes you straight there with one loading screen, solo or with the party (launcher: "Direct lamp
  travel", on by default).
- **Travel together or alone.** Lamps, headstones and the Hunter's Mark ask "Travel with the party?".
  Yes moves the whole party with one loading screen; No moves only you (when a guest rests that way,
  the host gets a short rest load so its world resets). The question closes by itself after 30 s.
  A guest's own save keeps its own spot: playing it solo later starts where it was, not at the
  lamp it travelled to with the party.
- **No loading screen inside the same map for the host.** The host's lamp or Hunter's Mark to a spot
  in the map it is already in is an instant warp, for the whole party too; a guest's gets its own
  short loading screen and the host stays where it is; at the lamp you already stand at, nothing
  moves at all. With nobody to travel with (a host alone, or "Join a party" while the host is not
  there) lamps, headstones and the Hunter's Mark work exactly as in single player.
- **Guests can use the Hunter's Mark** (on by default; launcher switch "Guests can use Hunter's
  Mark"): alone it takes you to your own last lamp, with the party to the host's.
- **Apart and back together.** While players are in different maps each sees only the others in
  its own map; meeting again shows them at once, without a loading screen.
- **Everyone can use lamps.** Vanilla disables lamps during co-op; here they work for everyone.
  A lamp lit by anyone is lit for the party.
- **Resting resets the world for the party.** Enemies respawn; everyone's HP, vials and bullets
  refill. A guest who rests and travels alone gives the host a short loading screen where it
  stands, so the host's world resets too; a guest resting its way back into the host's area
  arrives after that reset with a single loading screen.
- **Guests are full players.** The Doll (level up), shops, the workbench, storage, the Memory
  Altar (runes, covenants), lamps and headstones all work for guests.
- **Change your appearance at the Dream's mirror.** The mirror in the Hunter's Dream workshop opens
  the appearance editor ("Put on Disguise", a feature the game left unfinished, restored as in
  bbhost), in single player and for every party member on their own hunter. Launcher switch
  "Hunter's Dream mirror", on by default; everyone in a party needs the same setting.
- **Progress counts for everyone.** Bosses, items, doors, shortcuts, lamps, story and endings go
  into every player's own save. A guest who is behind catches up, including the Blood Moon.
  Key items a guest earned in the host's world (Oedon Tomb Key, badges, workshop tools) are checked
  each time its own world loads and given again if its save lacks them; a key item you already
  hold is never handed out twice (no "inventory or storage max" box).
- **Story cutscenes and endings play for everyone.** The Blood Moon, Amelia's skull (evening to
  night), Mergo's Wet Nurse's arena, Gehrman's scenes and the ending with the end credits
  play for every party member.
- **The host's world is shared live, both ways.** A door or shortcut opened by the host is open
  for the guests too, and a lever or door a guest uses opens in the host's world (and in the
  guest's own save).
- **Guests can talk to NPCs** in the host's world (Gilbert, Iosefka, Eileen ...); quest choices
  are shared by the whole party: a quest step the host makes also moves the NPC in every guest's
  own save (no stale NPC state left behind). NPCs look solid for guests (no see-through
  "ghosts"), and a level-up at the Doll stays in the levelling player's own game.
- **Hear each other's NPC talks.** When a companion near you talks to an NPC you see the game's
  subtitles and hear the NPC too, with a small "[Hunter talking to Iosefka]" caption and the
  choices they pick; nothing opens on your screen. Range in *System → Party rules → NPC dialogue
  range* (default 25 m), off with *Share NPC dialogue*.
- **NPC gifts are shared**: an item an NPC gives a guest in the host's world (e.g. Eileen's Bold
  Hunter's Marks) goes to the guest, the host and every other party member, once (the NPC has moved
  on for everyone, so nobody misses it).
- **Kidnapped by a Snatcher?** If the host is grabbed, the whole party wakes up together in the
  Hypogean Gaol (Yahar'gul) after one loading screen. Guests cannot be kidnapped in the host's world.
- **Enemy drops for each player, one roll.** Every player gets their own copy of each enemy's drop,
  and it is the same item for everyone (one shared roll per kill).
- **Blood Echoes for every kill.** Whoever kills an enemy, every party member gets its full
  Blood Echoes.
- **Treasure for each player.** Items lying in the world can be picked up by every player, each into their own game.
  Keys and workshop tools found lying around (Hunter Chief Emblem, Upper Cathedral Key, Iron Door Key,
  Lecture Theatre Key, Rune and Blood Gem Workshop Tools, ...) and the Eye of a Blood-drunk Hunter go to
  the whole party as soon as the host has them, so nobody is locked out of a door.
- **Cutscenes for everyone.** A story scene a guest triggers in the host's world plays for the
  whole party too.
- **Up to 4 players** (host + 3), all in the host's world. With 3 guests, a guest does not yet see
  the guests who joined after it.
- **Same version for everyone.** A player whose bbport build, game files, patches, mods or party
  options differ is refused on joining; the message says what differs and what to do.
- **Chalice dungeons together.** Guests join the host's dungeon; levers, doors, the layer boss, its
  loot (for each player) and the lift work for the whole party. Any player can perform the ritual:
  the host's altar then offers *Enter the party's dungeon* (no glyph typing), and chalices are
  shared between all players.
- **Crashes recover.** A crashed game restarts and rejoins by itself (back in the host's world about 12 s after the restart), next to the host: a guest whose own save is in another area first travels to the host's lamp with one loading screen. Nobody is summoned into a prologue. If the host's game crashes, the guests go home at once and come back by themselves as soon as the host is back in the world; items a guest got in the host's world stay in its own save.
- **Connection drops recover.** A short network outage (under ~25 s) keeps the session. In a longer one a guest goes home once (one loading screen) and is summoned back beside the host a few seconds after the network returns.
- **Network drops recover.** A short drop (seconds) goes unnoticed; after a longer one (30 s) a guest goes home with a loading screen and is summoned back beside the host by itself once the network is back. The party also checks every 15 s that everyone sees the same world and progress and quietly re-sends whatever differs.
- **Wrong passwords are locked out.** A few typos are fine; after 5 wrong passwords in a minute the host refuses that address for a while.
- **Your single-player save is safe.** Party play uses a separate save, backed up every 10 minutes.
- **Death has a cost.** Each death stacks "rot" (−10% max HP or stamina, picked at random, up to
  5); using a lamp (even just opening its travel menu and cancelling) or levelling up clears it.
  It belongs to your character: each save slot keeps its own, and a new game starts without it. On by default; the host can turn it off on the *Party rules* page.
- **Enemies scale with the party, harder than vanilla.** Bosses get more health (and a little more
  damage) per extra hunter than the game gives them, normal enemies get tougher too; all six values
  (boss / enemy health, damage, poise) are on the *Enemy scaling* page, the host's count.
- **Names over party members**, in the game's own style (replacing its plate, which hid behind walls), at any distance, bigger when near, with level, health or deaths (your choice); the *Party name tags* setting (launcher or *Party rules*) turns them off for the game's own plate. Names, the party panel and party notices step aside while the game's own menus (inventory, item descriptions, level up, travel) are open; for a YES / NO box only the names do.
- **Pings.** Hold the touchpad and press R3 (Xbox pads: hold Back, press R3; keyboard: Tab + Q, or
  just V) to mark the spot you are looking at: every party member in your world sees the marker
  through walls for 10 seconds, in your colour, with your name and its distance in metres (at the
  screen edge with an arrow when it is out of view). The touchpad alone still opens the gestures
  (when you let go). Optional: *Show party through walls* puts a small marker with the distance on
  every companion on your map. Both on the game's *System -> Pings* page and the launcher's Party tab;
  hidden under the game's menus like the other party text.
- **Party HUD.** Party members and events are shown in a small HUD; all party settings are on the
  *Party* pages of the game's *System* menu.

### Deaths

| Who dies | What happens |
|---|---|
| A guest | Gets up again at its own lamp with no loading screen when that lamp is in the loaded area (else at the host's lamp); its Blood Echoes wait in a bloodstain at that spot in its own world (pick them up there); the others keep playing |
| The host | Everyone gets one respawn loading screen at the host's lamp, as in the original game, and the world resets; the host's echoes stay in a bloodstain where it fell |
| Anyone, during a boss fight | Watches a living party member fight (press Cross to watch the next one) until the boss dies (then gets up at the host's lamp, with no loading screen when it is in the area), everyone dies, or someone travels |
| Everyone | Everyone respawns together at the host's lamp; the session stays |

A guest's death brings no enemies back; a host death, a rest at a lamp (anyone's) or a party wipe resets them.

### Coming next

| Change | Status |
|---|---|
| Item pickups and enemy drops per player: everyone gets their own (enemy drops: the same item for everyone) | Done |
| Every cutscene for the whole party (story cutscenes, endings and guest-triggered scenes already work) | In progress |
| Joining during a boss fight or cutscene waits outside until it ends | Works for boss fights with every join path (bell, title join, direct summon; verified live), the Party status says why you wait; cutscenes not yet tested live |
| With 3 guests, every guest sees every other guest | In progress |
| Chalice dungeons: deeper layers checked in a party; entering the party's dungeon in one press (the altar still asks a few prompts) | In progress |
| PvP / friendly fire the host can switch on and off | Later |
| Pings: party members and marked spots visible through walls, with distance and player colours | Later |

### Connecting over the internet

The host needs port 9307 (UDP and TCP) reachable. Most routers open it automatically (UPnP);
otherwise forward it by hand. Allow the Windows Firewall prompt once. Guests need nothing.

The host gets two party codes (launcher, *System → Party*):

| Code | For |
|---|---|
| **Internet code** (e.g. `BBP3-NJR0-E43N-WWN9-2C2P`) | Friends elsewhere |
| **LAN code** (e.g. `BBP3-PAJY-EAMH-7TD`) | Players on the host's network (same router) |

A guest on the host's network with the Internet code finds the host on the LAN by itself (~2 s).

No UPnP and no port forward? Each guest copies its **guest code** (`BBG1-...`, *Party* page) and
sends it to the host, who presses *Add a guest code*: the game then punches through both routers.
To skip the code, turn on *Find each other through the public DHT* (launcher *Party* tab or
*System > Party rules*) on **both** PCs: the guest's address is then exchanged through the public
BitTorrent DHT under a hash only the party knows. It is off by default because DHT nodes see your
IP address, as with any torrent client.
The *Party* page's *Network* line says which NAT you have and what to do about it. This fails behind symmetric / carrier-grade NAT (some mobile and ISP
connections); there, let another player host or use a VPN such as Tailscale. Details: [docs/party/network_robustness.md](docs/party/network_robustness.md).

### Reporting a co-op bug

| Step | How |
|---|---|
| Before playing | Launcher → *Advanced / Debug* → *Party logging*: **Verbose** on every PC |
| When it happens | Press **F9** (or *Party tab → Mark an issue*): every player's log gets `=== USER MARK n ===` |
| After playing | *Advanced / Debug* → **Save support bundle** on every PC and send the zips (passwords, codes and addresses are removed). A crash writes one by itself to `logs\` |

Details: [docs/party/logging.md](docs/party/logging.md).

Guide: [docs/PARTY.md](docs/PARTY.md) · Progress vs. Elden Ring Seamless Co-op:
[docs/party/ERSC_PARITY.md](docs/party/ERSC_PARITY.md)

## Documentation

| Topic | File |
|---|---|
| Building from source, DLSS, FSR 4 assets | [docs/BUILDING.md](docs/BUILDING.md) |
| Settings, launcher, controls, environment variables | [docs/SETTINGS.md](docs/SETTINGS.md) |
| Frame generation (DLSS FG, FSR FG) and NVIDIA Reflex | [docs/FG_REFLEX.md](docs/FG_REFLEX.md) |
| Shader compilation and sharing the cache | [docs/SHADERS.md](docs/SHADERS.md) |
| Saves, backups, restoring, game file check | [docs/SAVES.md](docs/SAVES.md) |
| Mods, XML patches, DLC | [docs/MODS.md](docs/MODS.md) |
| How the port works, vs. shadPS4, upstream merges | [docs/WINDOWS_PORT.md](docs/WINDOWS_PORT.md) |
| Upscalers | [docs/upscaler.md](docs/upscaler.md) |
| Co-op and game patches other forks can reuse | [docs/PATCHES_FOR_FORKS.md](docs/PATCHES_FOR_FORKS.md) |
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
| Mrsuss60 | Original Windows port: Win32 runtime, Vulkan on Windows, build scripts, character creation and keyboard fixes, touchpad Gestures and Personal Effects menus, performance HUD; audio pop fix, paths with parentheses in `run.bat`, parallel `build.bat`; CPUs without FSGSBASE (Windows TLS fallback), driver pipeline binaries kept between sessions, more background shader compile threads; SnormNz crash fix (packed formats such as 11_11_10), texture collector never evicting bound targets or presented surfaces; stream buffer placement switch (`BB_STREAM_MEMORY`), no draw-queue wait at `SET_PREDICATION` |
| shadPS4 team | Vulkan video core and shader recompiler (GPL-2.0+), sirit |
| droogie and the bbhost team | Long-session stability fixes; menu button rows and the menu cursor's offsets; direct summon delivery; the Hunter's Dream mirror (appearance editor) ([bbhost](https://github.com/droogie/bbhost), GPL-3.0+) |
| FireBurn | FSR-Vulkan (MIT); AMD FidelityFX SDK (MIT) |
| Supermedo, IFreemz | DLSS bridge and Windows loader |
| FIocker (Supermedo/bloodborne_pc PR #69) | Frame generation (DLSS FG, FSR FG) with paced presentation, NVIDIA Reflex, conservative object motion for frame generation, DLSS bridge ABI 2, FG diagnostics tools |
| Supermedo and contributors ([bloodborne_pc](https://github.com/Supermedo/bloodborne_pc)) | Intel 12th gen+ crash fix, FPS-preset physics step (found by FIocker), frame limiter pacing, texture-cache, pipeline-cache and shader-walker fixes, skipped draws for failed pipelines, keyboard stick movement, allocating past a full VRAM budget, object motion off on AMD before RDNA3, the memory log line once a minute, Back + d-pad touchpad halves on Xbox pads (idea by parsifal295), skipped draws with a null vertex buffer table, retried boot-file swaps, the unclean-eboot.bin warning; imedved (direct mouse camera, ported by mcrib884); andrecabralgit (stereo audio level), Jgamrot (overlay checkboxes and sliders), dGstone18 (mods on Windows); parsifal295 (game check passes over locked files), RafaelHGOliveira (exclusive fullscreen), fliperama86 (HDR desktop black level), emurphy04 (ultrawide output, video queue fix) |
| ALuaNegra (via Mrsuss60's port) | Idle depth targets freed (VRAM growth) |
| Vorshim92 | Pipeline cache kept between starts, stale `bbport.exe` guard, crash reaper, `run.bat` resolution fix, Reset shader cache button |
| Thealexbarney | LibAtrac9 audio (MIT) |
| ocornut | Dear ImGui (MIT) |
| Kyo, Lance McDonald, illusion, emoose, auser1337 | 60 FPS, camera and engine patches |
| zackcage6 | Testing and bug reports |

Also used: half, DejaVu fonts, Libre Baskerville (SIL OFL 1.1), dxil-spirv (MIT). AMD's FSR 4 files and NVIDIA's DLSS SDK are not
included here. Release packages include NVIDIA's `nvngx_dlss.dll` and `nvngx_dlssg.dll` under its license
(`out/NVIDIA_DLSS_LICENSE.txt`).
