# Bloodborne PC — native Windows port

Run the PS4 version of *Bloodborne* (v1.09, any retail release) natively on Windows with Vulkan.
Based on [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28 (Linux) and the original
[Windows port](https://github.com/Mrsuss60/bloodborne_pc_windows_port) by Mrsuss60.

**Version 0.7.5** · Playable · Party co-op in development

> No game files are included: you need your own decrypted dump with update 1.09 (any retail
> release: CUSA03173, CUSA00900, ...; they share the same 1.09 executable).
> Not affiliated with Sony, FromSoftware, AMD or NVIDIA. Not related to shadPS4: questions about
> the Linux port go to its [Discord](https://discord.gg/KYZRKk9CB).

**Contents:** [Features](#what-this-port-adds) · [Requirements](#requirements) ·
[Quick start](#quick-start) · [Controls](#controls) · [Custom key bindings](#custom-key-bindings) ·
[Party co-op](#party-co-op) · [Documentation](#documentation) · [Credits](#credits-and-licenses)

---

## What this port adds

| Feature | What you get |
|---|---|
| **Native on Windows** | No emulator, no Wine. The game's own code runs on your CPU; the graphics go through Vulkan. |
| **Party co-op, no server** | Play the whole campaign with friends over a direct connection: no PSN, no server. Modelled on Elden Ring Seamless Co-op. [More](#party-co-op) |
| **Settings in the game's menu** | Every port setting is a page in the game's own *System* menu, with gamepad navigation. |
| **Keyboard, mouse and any gamepad** | Classic keyboard layout or mouse & keyboard mode, every key rebindable; button prompts show your keys, Xbox or Nintendo buttons. [Controls](#controls) |
| **High frame rates** | 30 / 60 / 90 / uncapped, with correct game speed: hooks in the game's code follow each frame's real time (deadinside's 0.6-pre1 work; *Frame rate by game hooks* in the launcher, off = the community FPS++ patches), plus bbhost's high-FPS fixes for effects, grass wind and the Bloodletter's L2. |
| **Upscaling** | FSR 3.1, FSR 4, FSR 4.1.1 and DLSS, with motion vectors added for this game. |
| **Frame generation** | Optional, off by default: DLSS FG (2x-4x, RTX 40/50) or FSR FG (2x), paced, HUD kept sharp; NVIDIA Reflex. [More](docs/FG_REFLEX.md) |
| **Finer motion blur** | Optional, off by default: the game builds its motion blur from half-resolution buffers; render them at 2x the pixels or at full resolution (*System > Display > Motion blur resolution*). [More](docs/SETTINGS.md) |
| **HDR output** | Optional, off by default: HDR10 on an HDR display, highlights expanded to your display's peak while mid-tones and menus stay at paper white; calibration in *System > HDR*. [More](docs/SETTINGS.md#hdr-output) |
| **No shader stutter** | Known shaders are built before the intro. New ones compile in the background. |
| **Shareable shader cache** | Export / import one `.zip`, works across GPUs; drop a shared one into `user\cache_import\`. A built-in *Shader cache builder* travels the whole game by itself on a copy of your save to fill it. [More](docs/SETTINGS.md#shader-cache-builder-world-warper) |
| **Save safety** | Crash-safe saving, save checks at start, automatic backups in party play. |
| **The Old Hunters** | DLC enabled automatically (its data ships with patch 1.09). |
| **Mods and patches** | Loose-file mods and shadPS4-format XML patches, with load order. [More](docs/MODS.md) |

## Requirements

| | |
|---|---|
| OS | Windows 10 / 11, 64-bit |
| GPU | Vulkan 1.3: NVIDIA GTX 10-series+, AMD RX 400+, Intel Arc |
| CPU | x86-64 with AVX2 (Intel Haswell / AMD Ryzen or newer); FSGSBASE is not needed |
| Python | Bundled with the release (3.12, includes the launcher's Tk); building from source needs Python 3.10+ |
| Game | Decrypted game folder (`CUSA03173` or another retail release), v1.09 (update merged into the base game); an `eboot.bin` with a 60 fps patch baked in is refused |

## Quick start

1. Get a release, or [build from source](docs/BUILDING.md).
2. Put your game folder next to the port (`..\CUSA03173`), or pick it in the launcher.
3. Start **`launch_gui.bat`**, choose your settings, click **Launch Bloodborne**.
   Or run `run.bat "C:\Games\Bloodborne\CUSA03173"`.

Saves and the shader cache are stored in `user\`. Settings are stored in `bbport.ini` in the port
folder (next to `launch_gui.bat`).

## Controls

Gamepads work out of the box. The keyboard always works too, also next to a connected gamepad.
There are two keyboard layouts: the **classic layout** (keyboard only, camera on IJKL) and
**mouse & keyboard mode** (mouse camera and attacks; turn it on in the launcher's *Input & Audio*
tab or *System > Mouse & keyboard* in game).

| Action | Gamepad | Classic keyboard | Mouse & keyboard mode |
|---|---|---|---|
| Move | Left stick | `W` `A` `S` `D` | `W` `A` `S` `D` |
| Camera | Right stick | `I` `J` `K` `L` | Mouse |
| Walk (hold while moving) | Push the stick part way | `Left Alt` | `Left Alt` |
| Interact / confirm (Cross) | Bottom button | `Space`, `Enter` | `E`, `Enter` |
| Dodge / run / back (Circle) | Right button | `Left Shift`, `Esc` | `Space` |
| Quick item (Square) | Left button | `E` | `R` |
| Blood vial (Triangle) | Top button | `Q` | `X`, `F`, `V` |
| Attack (R1) / strong attack (R2) | `R1` / `R2` | `3` / `F` | Left click / `Shift` + left click |
| Firearm (L2) / transform (L1) | `L2` / `L1` | `R` / `1` | Right click / `Shift` + right click |
| Lock on (R3) | Right stick click | `C` | Middle click, `Q` |
| Jump while running (L3) | Left stick click | `Z` | `C`, `Z` |
| Items and weapons (d-pad) | D-pad | Arrow keys | Arrow keys, `1`-`4`, mouse wheel (quick items) |
| Menu (Options) | `Start` | `F1`, `O` | `Esc`, `F1` |
| Gestures / key items (touchpad) | Touchpad left / right half (Xbox pads: `Back` + d-pad left / right, `Back` alone the left half) | `Tab` or `G` / `Backspace` | `Tab` or `G` / `Backspace` |
| Party: ping / gather the party (host) | Hold the touchpad, press `R3` / `L3` (Xbox pads: hold `Back`) | `V` / `H` (or `Tab` + `C` / `Tab` + `Z`) | `V` / `H` (or `Tab` + `Q` / `Tab` + `Z`) |
| Port settings | *System* in the game's menu | *System* in the game's menu | *System* in the game's menu |
| Overlay menu (fallback) | `L3 + R3` (enable in the launcher) | `Insert` | `Insert` |
| Fullscreen | — | `Alt + Enter` | `Alt + Enter` |

In mouse & keyboard mode the side buttons are L1 (`X1`) and R2 (`X2`).

**Button prompts** follow the device you used last: your keys and mouse buttons, Xbox letters or
Nintendo letters (PlayStation pads keep the game's icons). Launcher *Input & Audio > Button
prompts* (or *System > Mouse & keyboard* in game) picks a set, e.g. *Nintendo* for an 8BitDo pad
in X-input mode; what each button does never changes.

Mouse sensitivity, free camera, debug menu and more: [docs/SETTINGS.md](docs/SETTINGS.md).

### Custom key bindings

Every input can be moved to other keys, separately for the classic layout and for mouse & keyboard
mode. Gamepad buttons can be remapped too.

**In the game (easiest):**

1. Press **`Insert`** and open the **Controls** tab. It lists every input of the layout you are using,
   with its keys.
2. Select a row (click it, or `Enter` / Cross), then press the new key. In mouse & keyboard mode a
   mouse button or the wheel works too.
3. The new key replaces that input's keys and works at once. If another input had that key, the two
   swap (the screen says so, e.g. "Q taken from R3 (lock-on); it gets X").

`Esc` cancels, `Delete` unbinds the row, and **Default keys** puts the whole layout back.

**In `bbport.ini` (several keys per input, gamepad buttons):** the in-game menu writes these lines
for you, and you can edit them by hand while the game is closed. Each line replaces that input's
default keys:

```ini
# Classic layout: key.<input>=<keys>
key.touchpad=Tab, rshift
key.touchpad_right=enter

# Mouse & keyboard mode: mkkey.<input>=<keys or mouse buttons>
mkkey.square=R, Mouse X1
mkkey.walk=lctrl

# Gamepad: pad.<input>=<buttons>
pad.touchpad=back, misc1
```

| Part | What you can write |
|---|---|
| `<input>` | `cross` `circle` `square` `triangle` `l1` `r1` `l2` `r2` `l3` `r3` `options` `touchpad` (left half, gestures) `touchpad_right` (right half, key items) `up` `down` `left` `right` (d-pad); keyboard only: `move_up` `move_down` `move_left` `move_right` `look_up` `look_down` `look_left` `look_right` `walk` |
| Keys | Key names as on the keycap, any case: `Q`, `F1`, `Tab`, `Space`, `Backspace`, `Return`, `Escape`, `Left Shift`, `Right Ctrl`, `Up`, `Keypad 5`, ... Short names work too: `rshift` `lshift` `shift` `enter` `return` `esc` `escape` `ctrl` `lctrl` `rctrl` `alt` `lalt` `ralt` `altgr` `bksp` `del` `ins` `pgup` `pgdn` |
| Mouse (mouse & keyboard mode) | `Mouse Left` `Mouse Right` `Mouse Middle` `Mouse X1` `Mouse X2` `Wheel Up` `Wheel Down`. A mouse button bound here leaves its built-in role (and its `Shift` + click). |
| Gamepad buttons | `a` `b` `x` `y` (by position: bottom, right, left, top) `back` `start` `leftstick` `rightstick` `leftshoulder` `rightshoulder` `lefttrigger` `righttrigger` `dpup` `dpdown` `dpleft` `dpright` `touchpad` `misc1` `paddle1`-`paddle4` |

- Separate several keys with commas (up to 4 per input). An empty value (`key.l3=`) unbinds the input.
- `key.` lines only change the classic layout and `mkkey.` lines only mouse & keyboard mode.
- A name the game does not know is skipped and logged (`controls: unknown key ...` in the game log).
- To go back to the defaults, delete the lines (or press **Default keys** in the Controls tab).

All the details: [docs/SETTINGS.md](docs/SETTINGS.md#controls).

## Party co-op

Play the whole game with friends in one session, from the first lamp to the end credits.
Modelled on Elden Ring Seamless Co-op, adapted to Bloodborne. **In development:** tested on one
PC (several game copies, simulated internet lag), **between two PCs on a LAN** (summon, combat,
shared kills and doors, per-player pickups) and **over the Internet** in two real sessions between
PCs in different homes:

| Session | Build | What it showed |
|---|---|---|
| Playtest 1: 2.5 h, two campaigns, prologue to Old Yharnam | 0.6.0 | Fixed in 0.6.1 ([docs/party/playtest1_triage.md](docs/party/playtest1_triage.md)) |
| Playtest 2: 2 h, Old Yharnam, the Blood-starved Beast and the Hunter's Dream | 0.6.9 | Joining, travel, deaths and boss kills held up; a boss the guest finished while the host lay dead stayed dead. Its reports are fixed for 0.7.0: guests frozen while the host died, the spectator camera, one-time shop items sold out for guests, gestures and key items not kept by guests, an uncollectable shared drop, guests unable to talk to Gehrman, captions at the workbench and shops, a new party code on every host launch |

Players running bigger parties sent reports and fixes too (Chalice Dungeon entry, VPN joins, 3-4 player
parties: guests drawn with one another's look, pickups animating every other player). A scripted
two-player run from two new characters to the end credits passed, and a three-player travel run
([docs/party/campaign_soak.md](docs/party/campaign_soak.md)).

**Party sections:** [First time? Step by step](#first-time-step-by-step) ·
[Starting a party](#start-from-the-title-screen-recommended) ·
[How it plays](#how-it-plays) · [Deaths](#deaths) · [Progress vs. Seamless Co-op](#seamless-co-op-progress) ·
[Coming next](#coming-next) · [Known issues](#known-issues) ·
[Connecting over the internet](#connecting-over-the-internet) · [Reporting a bug](#reporting-a-co-op-bug)

The host shares a **party code** such as `BBP3-NJR0-E43N-WWN9-2C2P`: it holds the host's address and a secret,
so share it only with the people you play with. With a password the code is shorter (19
characters without one); players on the host's own network can use the shorter **LAN code**
(e.g. `BBP3-PAJY-EAMH-7TD`). Everyone needs the same bbport version and game files. Party play uses its
own save; your single-player save is never touched.

### First time? Step by step

One host and two guests, from nothing to playing together. Do the steps in this order.

**1. Everyone:** install the **same release** of this port (the latest one).

**2. Host:**

1. Start `launch_gui.bat` and open the **Party** tab.
2. *Mode* = **Host a party**. Type **Your name**.
3. **Max players: 3**. Leave the other options as they are. A **Password** is optional; if you set
   one, every player must type the same one.
4. Click **Launch Bloodborne**. If Windows asks whether `bbport.exe` may use the network, click **Allow**.
5. Once the game runs, the launcher shows two codes. Click **Copy** next to the right one and send it
   to both friends:
   - **Internet code**: friends in other homes.
   - **LAN code**: players on your own Wi-Fi / router.
6. Just play. When your friends are ready, the game brings them into your world by itself.

**3. Each guest:**

1. Open the **Party** tab and choose *Mode* = **Join a party**. Each player needs a **different name**.
2. Paste the host's code into **Party code** (the **Paste** button does it). A ✓ under it means the code is valid.
3. Same **Max players** (3) and the same **Password** as the host.
4. Click **Launch Bloodborne** **after** the host's game is already running.
5. Play the start of the game (the prologue) in your own world. After that the host's game summons you
   automatically. Short of time? The host can turn on *Skip the prologue* (*System > Party rules:
   Phantoms* or the launcher's Party Rules tab): a brand-new character then starts at the Central
   Yharnam lamp with the Saw Cleaver, the Hunter Pistol and Iosefka's Blood Vial, ready to be summoned.
   Characters past the clinic are never touched.

**4. The guests stay at *Connecting*?**

- The host's PC must be reachable on **port 9307, UDP and TCP**. The game asks the router to open it
  (UPnP). If the host's log says *"UPnP unavailable"*, open port 9307 (UDP and TCP) to the host's PC
  in the router's settings.
- **Shared provider address (CGNAT)**, common with many providers (for example in Brazil): the router's
  outside address starts with `100.64` to `100.127`, and opening the port on the router does not help.
  The *Party* page's *Network* line tells you when this is the case. The easiest fix is a free VPN
  that all three players install, such as **Radmin VPN**, **Tailscale** or **ZeroTier**; the guests then
  type the host's VPN address into **Party code** (the port 9307 is added if left out). The host's
  launcher finds the VPN adapter by itself and shows the address to copy (*Host over a VPN*: Auto / Off
  / adapter name). Or let a player with an open port host.
  The host's game also announces the party on the VPN / LAN (never the Internet), and the launcher's
  *Parties found* list shows it with a **Join selected** button (*Party discovery*, on by default).
- **Windows Firewall**: allow `bbport.exe` (*Windows Security → Firewall → Allow an app through firewall*).
- More options (guest codes, DHT, IPv6): [Connecting over the internet](#connecting-over-the-internet).

**5. Still not working?** On every PC: launcher → *Advanced / Debug* → **Save support bundle**, and send the
zips with an issue, together with which step each player reached (see [Reporting a co-op bug](#reporting-a-co-op-bug)).
The lines starting with `NAT:`, `UPnP:` and `Party:` in the logs usually tell what went wrong.

After the first session, the title screen's **Party → Continue party game** is all anyone needs:
code and password are remembered, and the host's code stays the same.

### Start from the title screen (recommended)

Nothing to set up outside the game, and later sessions are a single press.

| | Host | Guest |
|---|---|---|
| 1 | Title screen → **Party** → set a **Password** (optional, gives a shorter code) | Title screen → **Party** → **Host address**: paste the host's party code (**Ctrl+V**, e.g. `BBP3-NJR0-E43N-WWN9-2C2P`) → **Password** (the host's, if it set one) |
| 2 | **New party game** (first time: new character) or **Continue party game** | **Join party game** (first time) or **Continue party game**, once the host's game is running |
| 3 | Allow the Windows Firewall prompt once; copy your code (*System → Party → Copy Internet code*) and send it | The game restarts into party mode and puts you beside the host |

Code and password are remembered, so later sessions are just *Continue party game* for everyone
(the options that restart the game ask for a second press). The host's party code stays the
same every time it hosts, so friends can keep it; *System → Party mode → New party code* (or **New
code** in the launcher's Party tab) makes a new one and the old one stops working. Setting or
removing a password also changes the code once. *Continue party game* shows whenever
the party saves hold a character, also if you started party games from the launcher.
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
  (see [Connecting over the internet](#connecting-over-the-internet)).

### How it plays

**Joining and leaving**

- **No PSN, no server.** One player hosts; up to 3 guests join with the host's party code
  (4 players), all in the host's world. Every guest sees every other guest.
- **Fast join.** No bell, beckoner or summon-sign waits: a guest stands beside the host within
  seconds (about 5 s in the same area; a join that stalls restarts by itself). *Continue party
  game*, or joining from another map (the Hunter's Dream, say), takes one loading screen. If the
  host travels while you join, its travel waits until you are in and you travel together.
- **Host in a boss fight or cutscene?** Your join waits in your own world until it is over (the
  Party status says why), then puts you beside the host with one loading screen.
- **Leave any time.** *System → Party → Leave the party* takes a guest home at once and it keeps
  playing online alone; the host's *Leave* sends every guest home (one loading screen, no "Session
  lost"). A guest can rejoin at any time, also right after a boss kill (*System → Party mode → Join
  party*; *Host party* and *Single player* are there too).
- **The session never ends by itself.** Boss kills, area clears, deaths and travel don't send
  anyone home.
- **Same version for everyone.** A player whose bbport build, game files, patches, mods or party
  options differ is refused on joining; the message says what differs and what to do. The
  launcher tells you when a new version is out (at most one check every 6 hours, off in
  *Advanced / Debug*).

**Exploring and travel**

- **No multiplayer walls, no leash.** The fog walls that keep phantoms near the host are gone.
  The party can split up: a guest can walk far from the host (checked at 140 m), and host and guest
  can each play on in another part of the city for minutes (fight, use items, die there) without
  being pulled back.
- **Apart and back together.** Players in different maps see only those in their own map; meeting
  again shows them at once, without a loading screen. If the host dies while you are in another
  area you play on where you are; if you die there you get up at your own lamp.
- **Everyone can use lamps** (vanilla disables them in co-op), and **lamps are shared**: the host's
  world keeps its own lamps, and a lamp anyone lights there is lit for everyone, in everyone's own
  save too; the host's lamps also reach each guest's save. A guest's lamps from its own world stay
  there unless the host turns on *Guests bring their lamps* (then a lamp past a boss the host has
  not beaten yet waits until the host beats it). The host can turn sharing off (*Shared lamps*).
- **Travel from any lamp to any lamp.** A lamp offers the headstones' list (the Hunter's Dream and
  every lit lamp) and takes you straight there with one loading screen (launcher: "Direct lamp
  travel", on by default).
- **Travel together or alone.** Lamps, headstones and the Hunter's Mark ask "Travel with the party?".
  *Yes* moves the whole party with one loading screen; *No* moves only you. The question closes by
  itself after 30 s. A guest's own save keeps its own spot for solo play later.
- **Vote on travel** (host's switch, off by default: *Party votes on travel* on *System → Party rules:
  World*, or the launcher). The party then travels only when everyone agrees: after *Yes* to
  "Travel with the party?" every other player gets the game's *Yes* / *No* question with who wants
  to go and where. One *No*, or no answer within 20 s, cancels the trip for everyone, and the notice
  says who said no or didn't answer. A guest's own *Yes* is a vote too (the host and the other
  guests are asked). Travelling alone never needs a vote.
- **Gather the party.** The host's *System → Party → Gather the party* (or hold the touchpad and
  press L3; Xbox pads: hold Back, press L3; keyboard: `H`) brings every guest to the host. Each guest is asked first (*Yes* / *No*): in the host's map you appear beside the host with
  no loading screen; from another map (the Hunter's Dream, say) it is one loading screen. A guest
  who is dead, in a menu, in a cutscene or in a boss fight is not moved (both see why); the host
  cannot gather during its own boss fight or cutscene, on a ladder or in mid-air, and only once every
  30 s. If there is no ground beside the host, the guest comes with one loading screen instead. The host can turn
  gathering off (*Host can gather* on *System → Party rules: World*).
- **No loading screen inside the same map for the host.** A host's lamp or Hunter's Mark to a spot
  in its current map is an instant warp, for the whole party too. Alone (no one to travel with),
  lamps, headstones and the Hunter's Mark work exactly as in single player.
- **Guests can use the Hunter's Mark** (on by default; launcher switch "Guests can use Hunter's
  Mark"): alone it takes you to your own last lamp, with the party to the host's.
- **Resting resets the world for the party.** Enemies respawn; everyone's HP, vials and bullets
  refill. A guest who rests at a lamp and travels alone gives the host a short loading screen so its
  world resets too; a guest leaving the Hunter's Dream alone by a headstone is just a trip (no reset).
  Resting while the host is in a boss fight takes everyone out of the boss room.
- **Kidnapped by a Snatcher?** Whoever is grabbed, host or guest, the whole party wakes up together
  in the Hypogean Gaol (Yahar'gul) after one loading screen, and everyone watches the waking scene
  (+1 Insight each). Only the first kidnap does this, as in the game. The host can turn off
  *Anyone can be abducted* (*System > Party rules: Deaths*): then only a grabbed host takes the party
  there and a grabbed guest just dies. A kidnap never follows a guest home into its own world.
- **Any level can play together.** Nobody is turned away for their level. As in the game's own co-op, a
  hunter far above the host's level is scaled down while in the host's world (less health, stamina and
  defence, a blue icon by the name on the game's own name plate), and everyone who joins has 30 % less
  maximum health. The host can turn both off (*System > Party rules: Phantoms*: *Level sync*, *Phantom
  health cut*), then everyone keeps full stats. Both are on by default.
- **Chalice dungeons together.** Guests join the host's dungeon; levers, doors, the layer boss, its
  loot (for each player) and the lifts work for the whole party, layer after layer. Any player can
  perform the ritual: the host's altar then offers *Enter the party's dungeon*. Chalices are shared
  between all players (Root Chalices, which a ritual uses up, stay each player's own). The dungeon's
  lamps work with a party (back to the Dream, and from the altar to any lamp you lit). A guest who played the host's dungeon takes it home: it shows at the guest's own altar with the lamps and shortcuts the party reached (with Shared lamps on). A guest who dies
  in the dungeon gets up beside the party; a guest pulled out by a death or a forced trip is summoned
  back in automatically, one who chose to leave returns with *Party > Rejoin* (System > Party rules: Travel).

**Bosses, cutscenes and endings**

- **Anyone can go through a boss fog first**, a first fight included: the fight starts for
  everyone and the others follow through the fog. A guest's first fight plays the intro cutscene on
  every screen. The DLC bosses work the same way. Ebrietas, Lady Maria and Laurence start only with
  what the host does in their room (a hit, walking up to Maria, Laurence's Skull at the altar):
  until then their room is open to everyone, and a guest at that fog is told what starts it. Rom
  starts with anyone's hit.
- **Guests can hit NPCs.** A guest's blows land on the host world's NPCs as the host's do (the friendly
  hunters and the others the host could hurt): they take damage and a killing blow counts for
  everyone. An NPC that turns hostile after a few hits (Alfred) does so on a guest's blows too.
  Nothing is added that the host could not do.
- **NPC hunters fight the whole party.** Eileen and Henryk at the Tomb of Oedon, Alfred, the
  blood-drunk Eileen, Valtr and the DLC hunters are there for everyone in the host's world:
  anyone can hurt them, and their death and the quest step after it count for every player's own
  save. A hunter that turns on the host turns on the guests too (*NPC hunters fight all*, a host
  setting on *System → Party rules: World*, on by default). Covenant outcomes, such as the host's Alfred
  killing Annalise, stay each player's own. Brador and the Fishing Hamlet's hunters
  should show for guests too, but that is not tried in a real session yet.
- **Boss fights need the host's area.** A guest who travelled alone cannot start a boss fight
  where the host is not (the fog stays closed, with a message); if the host leaves during a fight,
  the guest is taken to the host with one loading screen.
- **Story cutscenes play for everyone**: the Blood Moon, Amelia's skull, Mergo's Wet Nurse's arena,
  Gehrman's scenes and the ending with the end credits. A scene a guest triggers plays for the
  whole party too (opening the Cathedral Ward door turns day into evening for everyone; a guest can
  start the Blood Moon at Moonside Lake). After the Blood Moon the host is asked "Travel with the
  party?" and everyone goes to Yahar'gul together.
- **Your ending, your choice.** When the host reaches an ending, every guest is asked first:
  *Yes* plays the same ending the host got in your own save (its cutscene, the end credits, then
  New Game+ once), *No* takes you home with your save as it was.
- **One NG cycle per world.** In the host's world everyone plays at the host's NG cycle; your own
  save keeps your own cycle.

**Progress and rewards (shared both ways)**

- **Progress counts for everyone.** Bosses, items, doors, shortcuts, lamps, story and endings go
  into every player's own save; a guest who is behind catches up, including the Blood Moon.
- **Covenants are your own.** How the covenant quests end stays in the world it happened in:
  Alfred killing Queen Annalise, her revival, Valtr's death and the other covenant leaders' fates
  never reach another player's save (in the host's world you see the host's state). A covenant
  rune an NPC hands you (Valtr's Impurity, for example) is yours, in your own save. A host setting,
  on by default (*Covenants per player* on *System → Party rules: World*).
- **The host's world is shared live, both ways.** A door, lever or shortcut opened by anyone is
  open for everyone, in the host's world and in each guest's own save; a lift ridden or a ladder
  lowered by anyone is in the same place for everyone, also for a player who was far away. Guests open gates with their
  own keys (e.g. the Cathedral Ward gate with the Hunter Chief Emblem).
- **Key items are never lost.** A summoned guest sees its own key items in the host's world too
  (the game alone shows a visiting hunter an empty Key Items tab). Key items a guest earns in the
  host's world (Oedon Tomb Key, badges, workshop tools, chalices, umbilical cords) are given at once,
  and checked again when its own world loads and given again if missing; one you already hold is
  never handed out twice, and one you used up (the Gold Pendant, the Tear Stone) never comes back.
  Both are your own switches on *System → My party: Your items* (*Own key items as guest*, *Key items at once*).
  Keys, tools and quest items found lying around (the daughter's ribbons and brooch, the Orphanage Key,
  the Hunter Badges) go to the whole party, so nobody is locked out of a door or a questline.
- **Early items for every player.** Each player picks its own starting weapons from the Dream's
  messengers (another player's pick never takes yours away); Iosefka's Blood Vial and the Old
  Hunter Bell go to every player, whoever got them in the host's world.
- **Gestures are shared.** A gesture anyone in the party learns (from Gehrman, the Messengers, an
  NPC's talk) is learned by every player at once, with the game's "item acquired" notice, and kept
  in each player's own save, also when it was learned in someone else's world.
- **Enemy drops for each player, one roll.** Everyone gets their own copy of each enemy's drop from
  one shared roll per kill; each player's own Item Discovery counts.
- **Blood Echoes for every kill**, whoever made it, and **treasure for each player**: items in the
  world can be picked up by every player, each into their own game.
- **Insight for everyone.** Whatever Insight the host gets (bosses, story moments, Chalice Dungeon
  bosses) every guest present gets too; a boss's first-sight Insight comes once per player.

**Guests are full players**

- The Doll (level up), shops, the workbench, storage, the Memory Altar (runes, covenants), lamps and
  headstones all work for guests. A guest can rest alone (lamp, "No"), level up and shop in the
  Dream and come back while the others play on. Shop stock is per player: one-of-a-kind items
  (armour sets, the Insight shop's attire) bought by one player stay in stock for the others.
- **Talk to NPCs.** Guests can talk to NPCs in the host's world, Gehrman in the Hunter's Dream too
  (his last offer, the ending choice, stays the host's); quest steps are shared by the whole
  party (the covenant quests of Annalise, Alfred and Valtr stay each player's own), and NPCs look
  solid for guests. When a companion near you talks to an NPC you see the
  subtitles and hear the NPC, with a small "[Hunter talking to Iosefka]" caption (range in *System →
  My party: Look & notices → NPC dialogue range*, default 25 m). The workbench, the Bath Messengers' shops and
  headstones show no caption. One-time NPC gifts go to every party member;
  repeatable ones (Blood of Arianna, Iosefka's vials, ...) and Valtr's League gifts to whoever asked.
- **Change your appearance at the Dream's mirror.** The workshop mirror opens the appearance editor
  (a feature the game left unfinished, restored as in bbhost), in single player and for every party
  member; the others see your new look at once. Launcher switch "Hunter's Dream mirror", on by
  default; everyone in a party needs the same setting.

**Rules, difficulty and HUD**

- **Death can have a cost (optional).** With "rot" on, each death stacks a penalty, picked at random,
  up to 5: −10% max HP, −10% max stamina, +10% stamina use, −15% rally or −10 Item Discovery. Using a
  lamp (even just opening its travel menu) or levelling up clears it. Killing a boss clears it for the
  whole party; when the enemy that killed you dies (whoever kills it), or when you pick up your Blood
  Echoes, the rot from that death goes. Each save slot keeps its own. Your current rot and what it
  does ("Rot 2/5: max HP -20 %") show on the *Party* page and under the party HUD, and a notice says
  when some is cleansed. Off by default; the host turns it and each of the three cleanses on or off on
  the *Party rules: Rot* page (or in the launcher's *Party Rules* tab).
- **Enemies scale with the party, harder than vanilla.** Bosses get more health (and a little more
  damage) per extra hunter; normal enemies get tougher too. Six values (boss / enemy health, damage,
  poise) on the *Party rules: Enemies* page.
- **Names over party members**, in the game's own style, at any distance, with level, health,
  deaths or ping (your choice; *Party name tags* turns them off). A member standing behind your own
  character has no tag until it steps out (walls do not hide tags).
  A small blue marker by a name shows a hunter scaled down by *Level sync*; in PvP our health bar is red. Party text steps aside while the
  game's own menus are open; party messages are centred in their box.
- **Companions look like normal hunters.** Summoned hunters are drawn without the game's blue,
  glowing phantom look (they are still cooperators for the game). Each player chooses for their own
  screen: *No companion glow* on *System → My party: Look & notices* or *Companions without glow* on the launcher's *My Party* tab (on by
  default; switches at once, also while playing).
- **Pings.** Hold the touchpad and press R3 (Xbox pads: hold Back, press R3; keyboard: `Tab` + `Q`,
  or just `V`) to mark a spot: everyone in your world sees it through walls for 10 seconds, with your
  name and its distance. *Show party through walls* puts a small marker on every companion. Both on
  *System → My party: Display* and the launcher's *My Party* tab.
- **Friendly fire / PvP** (host's switch, off by default): party members can hurt each other, both
  ways, like invaders (*System → Party rules: PvP & lamps*). A party kill is a normal party death; the killer
  gets no Echoes or Insight.
- **Connection warnings.** When the Windows Firewall, the router or a wrong address keeps your party from working (nobody reaches a host without an open port, a guest's game is not heard, a guest cannot reach the host), the game shows a notice with the shortest fix and logs it; at most once per cause per session (*System > Party members > Connection warnings*, on by default).
- **Party HUD.** Party members and events in a small HUD; each companion's ping in ms after the name (green / yellow / red; *My party: Look & notices > Ping in the party list*, on by default).
- **Every party setting in the game's own menu, in two groups.** *Party rules* holds what the host
  decides for the whole party: *Party rules: World* (enemies the party killed stay dead, gathering the
  party, NPCs as in the host's world, covenants, voting on travel, NPC hunters), *PvP & lamps*,
  *Deaths* (getting up and respawning, also for guests), *Travel* (lamps and Hunter's Mark for guests,
  open world, Chalice Dungeons, cutscenes with the host), *Rewards* (guests' echoes and drops, boss
  Insight), *Phantoms*, *Rot* and *Enemies* (scaling); guests see the host's values read-only, so the
  whole party plays one way. *My party* holds what each player chooses for itself: *Your save* (what
  the party brings into your own character), *Your items* (item rewards, your key items as a guest),
  *Display* and *Look & notices*. The launcher's *Party Rules* and *My Party* tabs show the same settings
  (`bbport.ini`); its *Party* tab is for hosting, joining and the connection.

**Safety and recovery**

- **Crashes recover.** A crashed game restarts and rejoins by itself, next to the host (about
  20-27 s after the crash on one PC). If the host's game crashes, the guests go to their own world
  and wait ("Waiting for the host to come back (N s)..."), then are summoned back once it has
  restarted (about 20 s on one PC; after 3 minutes a guest plays on alone).
- **Network drops recover.** An outage of up to about 2 minutes keeps the session: everyone carries
  on where they stood once the network is back. After a longer one a guest goes home and is
  summoned back by itself. Every 15 s the party checks that everyone sees the same world and
  progress and re-sends whatever differs.
- **Wrong passwords are locked out**: after 5 wrong passwords in a minute the host refuses that
  address for a while.
- **Your single-player save is safe.** Party play uses a separate save, backed up every 10 minutes.
- **Undo a party session.** Before every party session the game copies both save folders aside.
  The launcher's *Party* tab has **Undo last party session** (itself undoable). After a session the
  launcher also checks the party save (characters load, no boss kill or lamp lost, level and play
  time never go down) and offers the undo if something looks wrong.
- **Save editors.** A save the game made while you were a guest in someone else's world cannot be read
  by save editors (the game leaves out your own world's state there). The launcher's *Party* tab has
  **Export for save editor** (any single-player or party save, any slot: a copy the editor can open,
  your save is not touched) and **Import from save editor** (the edited copy back into its slot, your
  saves backed up first, put back the way the game wrote them so party progress stays).

### Deaths

| Who dies | What happens |
|---|---|
| A guest | Gets up again at its own lamp, with no loading screen when that lamp is in the loaded area (a lamp in another area: a short respawn loading screen); its Blood Echoes wait in a bloodstain where it fell (an on-screen marker for it: *System → Pings → Bloodstain marker*, off by default); the others keep playing. **A fall** (off a ledge, or out of the world): it gets up at its own lamp, or at the host's lamp when its own is in another area, with no loading screen when that lamp is in the area, never in the Hunter's Dream; its Blood Echoes wait on the last solid ground before the fall, never under the map |
| The host | Gets up again at its lamp with no loading screen while the guests play on (a lamp in another area: the host alone loads it); the host's echoes stay in a bloodstain. After a fall (off a ledge or out of the world) it also gets up at its lamp (no loading screen when the lamp is in the area, never in the Hunter's Dream); its echoes wait on the last solid ground before the fall. A death that takes the host somewhere else by the story (a Snatcher's kidnap to the Hypogean Gaol) takes the whole party there instead: one loading screen for everyone, the guests wake beside the host. In a boss fight the guests kill: the host gets up in the arena (see below). `BB_PARTY_B2_HOST_REVIVE=0`: everyone gets one respawn loading screen at the host's lamp and the world resets, as in the original game |
| Anyone, during a boss fight | Watches a living party member fight, with the camera following them as in normal play (the right stick turns it; press Cross to watch the next one) until the boss dies, everyone dies, or someone travels. When the host is down and the last hunter still fighting leaves the fight (Hunter's Mark, a lamp, its death, *Leave* or a lost connection), that counts as everyone dying: the whole party gets the respawn loading screen at the host's lamp. When the boss is killed, everyone who died in the fight - the host and the guests alike, also one still in its death animation when the boss fell - gets up right there in the arena, where they fell (after a fall: on the last solid ground before it, where their bloodstain is; or beside a living hunter), never under the map, no load, their echoes still in their bloodstain. the host's *System → Party rules: Deaths → Up after boss kill* off: the host's death takes the party's respawn load and a guest gets up at the host's lamp instead |
| Everyone | Everyone respawns together at the host's lamp with one loading screen; the session stays. Also when the last hunter falls long after the others (the ones already down wait for it, however long the fight goes on); its echoes wait in a bloodstain where it fell |

A death (host or guest) brings no enemies back; a rest at a lamp (anyone's) or a party wipe resets them.
An enemy killed by anyone in the party is dead on every game, also one of the next area seen across a
border; walking away from it and coming back does not bring it back (single player does, for the next
area's enemies).

### Seamless co-op progress

Parity with Elden Ring Seamless Co-op, measured row by row in [ERSC_PARITY.md](docs/party/ERSC_PARITY.md).
Two numbers: **plan done** is how much of the plan is built and seen working at least once;
**ERSC parity (evidence-weighted)** caps every row checked on one PC only at 92%, at 94% when it also ran
over the real Internet from one PC (through a VPN), until it has been checked on two real PCs over the
Internet (two real Internet sessions so far, on 0.6.0 and 0.6.9; every other check ran on one PC); a row
that failed in a real session and is fixed on one PC only scores lower still (84-88%), and rows where we
chose to differ from ERSC (a wipe respawns everyone together, 4 players instead of 6, one shared drop roll)
are capped at 95% too.

| Area | Plan done | | ERSC parity (evidence-weighted) | |
|---|---|--:|---|--:|
| **Overall** | `████████████████████` | 98% | `██████████████████░░` | 89% |
| Session & joining | `████████████████████` | 99% | `██████████████████░░` | 91% |
| World & travel | `████████████████████` | 100% | `██████████████████░░` | 89% |
| Death & respawn | `████████████████████` | 100% | `██████████████████░░` | 88% |
| Progress (anyone can progress) | `███████████████████░` | 95% | `█████████████████░░░` | 85% |
| Services & UI | `████████████████████` | 100% | `██████████████████░░` | 90% |
| Network & robustness | `███████████████████░` | 95% | `█████████████████░░░` | 87% |
| Bloodborne-specific (chalice, Insight) | `████████████████████` | 100% | `██████████████████░░` | 92% |
| Feels like single player in a party | `████████████████████` | 100% | `██████████████████░░` | 90% |

### Coming next

| Change | Status |
|---|---|
| FSR 4 white or grey picture fixed | Protective fixes built; the cause (FSR 4 with HDR on in Windows) is still being confirmed |
| Guests load at the same time as the host on party travel (about 10-15 s less per trip) | Planned (opt-in first) |
| Host death the ERSC way: the host loads alone, guests play on | Planned (opt-in first; today the whole party reloads) |
| Game-style fades instead of blinks for in-world moves and revives | Planned |
| Shader cache builder: bosses, gestures and hunter tools; a fuller shared cache | In progress |
| Playtest 3 over the Internet on 0.7.0 (the fixes from playtest 2, VPN joins, 3-4 players) | Needs players: see `docs/party/playtest2_checklist.md` |

### Known issues

- **FSR 4 can show a white or grey picture** on some PCs. The cause is under investigation (suspected:
  FSR 4 with HDR turned on in Windows). Meanwhile switch to FSR 3.1, DLSS or no upscaler.
- **The fixes since playtest 2 were tested on one PC** (several game copies, simulated internet lag),
  not yet in a real Internet session. Please send support bundles if something goes wrong (see
  [Reporting a co-op bug](#reporting-a-co-op-bug)).
- **Reported in real sessions and fixed in 0.7.6 or not yet, none confirmed live on two PCs:** after an ending nobody could join (fixed 0.7.6), a guest's save advancing two New Game+ cycles (fixed 0.7.6), joins over a VPN failing for a guest behind a strict NAT, a 4-player lobby with guests stuck loading, 4-10 s outages on Hamachi or a phone hotspot sending guests home, guests not seeing each other until a teleport, a wrong ending cutscene and late credits for a guest, the boss standing idle while the host is dead and a guest fights, talking to an NPC next to a lamp opening the lamp's travel prompt for a guest, the Moon Presence music missing after Gehrman, and a clip on the edge of descending lifts. Open ones are tracked in [ERSC_PARITY.md](docs/party/ERSC_PARITY.md).
- **The Bath Messengers may pop in and out** (with their sound repeating) for a player watching while
  another browses their shop. Being fixed.

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
*System > Party network*) on **both** PCs: the guest's address is then exchanged through the public
BitTorrent DHT under a hash only the party knows. It is off by default because DHT nodes see your
IP address, as with any torrent client.
If the guest's game gets no UDP answer from the host ("UDP to the guest is blocked" /
"UDP to the host is blocked"), nobody rings until it does: it keeps retrying and the summon
then starts by itself.
If the host cannot be reached at all but another member can, a guest joins **through that member**:
it copies the member's guest code and presses *Join through a member* on its *Party* page (with the
DHT option on, members are found by themselves); this is on by default. **IPv6** is off by default:
turned on on both PCs (launcher *Party* tab, *System > Party network*), the party tries it before any
relay, and a host without an open port then has a longer code that **contains its IPv6 address** (a
temporary privacy address where Windows has one, so the code changes when Windows rotates it).
The *Party* page's *Network* line says which NAT you have, what to do about it and which path the
link took (TCP, UDP, IPv6 or a member's relay). Two symmetric / carrier-grade NATs with no IPv6 and no
reachable member still cannot meet; there, let another player host or use a VPN such as Tailscale. Details: [docs/party/network_robustness.md](docs/party/network_robustness.md).

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
| deadinside28 | Original Linux port (bbport): flat memory image, two-stage Vulkan pipeline, motion vectors; Linux mouse look capture (via Mrsuss60); in 0.6.2 fixes from 0.6-pre1 (Supermedo/bloodborne_pc 72e5c8ce): idle VRAM copies released on 4 GB cards, NVIDIA start without DLSS, unresolved Vulkan commands, pipeline cache stages, write-trap race, occlusion pass scale, sampler anisotropy clamp; next release: button prompts for the keyboard, Xbox and Nintendo pads (72e5c8ce) and the frame rate by hooks in the game's code (0.6-pre1) |
| Mrsuss60 | Original Windows port: Win32 runtime, Vulkan on Windows, build scripts, character creation and keyboard fixes, touchpad Gestures and Personal Effects menus, performance HUD; audio pop fix, paths with parentheses in `run.bat`, parallel `build.bat`; CPUs without FSGSBASE (Windows TLS fallback), driver pipeline binaries kept between sessions, more background shader compile threads; SnormNz crash fix (packed formats such as 11_11_10), texture collector never evicting bound targets or presented surfaces; stream buffer placement switch (`BB_STREAM_MEMORY`), no draw-queue wait at `SET_PREDICATION`; mouse sensitivity fix (stick dead zone compensation), in-game key rebinding, focus click not a game press; next release: clear error for a missing or truncated `sce_module/libc.prx`; crash logs that name failed allocations and the game's own assertions, crash exit codes by cause, a stripped (smaller) `bbport.exe` |
| shadPS4 team | Vulkan video core and shader recompiler (GPL-2.0+), sirit; next release: first-page texture lookup (145932bf, via Mrsuss60's port) |
| droogie and the bbhost team | Long-session stability fixes; menu button rows and the menu cursor's offsets; direct summon delivery; the Hunter's Dream mirror (appearance editor); next release: the high-FPS fixes for effects, grass wind and the Bloodletter's L2 (Kyoskii, perf/next 6f735f4, b2818b4) ([bbhost](https://github.com/droogie/bbhost), GPL-3.0+) |
| FireBurn | FSR-Vulkan (MIT); AMD FidelityFX SDK (MIT) |
| Supermedo, IFreemz | DLSS bridge and Windows loader |
| FIocker (Supermedo/bloodborne_pc PR #69) | Frame generation (DLSS FG, FSR FG) with paced presentation, NVIDIA Reflex, conservative object motion for frame generation, DLSS bridge ABI 2, FG diagnostics tools |
| Supermedo and contributors ([bloodborne_pc](https://github.com/Supermedo/bloodborne_pc)) | Intel 12th gen+ crash fix, FPS-preset physics step (found by FIocker), frame limiter pacing, texture-cache, pipeline-cache and shader-walker fixes, skipped draws for failed pipelines, keyboard stick movement, allocating past a full VRAM budget, object motion off on AMD before RDNA3, the memory log line once a minute, Back + d-pad touchpad halves on Xbox pads (idea by parsifal295), skipped draws with a null vertex buffer table, retried boot-file swaps, the unclean-eboot.bin warning, a bad PM4 header dropping its buffer instead of stopping the game and the GPU line flushed at start (c07f0ffa, from parsifal295's PRs #57 and #61); in 0.6.3 the flip IRQ ahead of its label (76f5b5f6, from Awaken's port), a keyboard sprint kept through direction changes (4b930b10, measured by cassioKenji, PR #74), read-only game files and exFAT / FAT32 drives no longer dropping mods (ba34a3a2 from emurphy04's PR #77, bf0878bf); checked against Supermedo's five PR fixes (3e3b760f): parsifal295's stale texture format (#55), title-menu flicker above 1080p (#56), bmy's loading-screen item picture (#62) and DLSS device fallback (#58, its reason now names the Vulkan result), dGstone18's build path with spaces (#72), all already here; imedved (direct mouse camera, ported by mcrib884); andrecabralgit (stereo audio level), Jgamrot (overlay checkboxes and sliders), dGstone18 (mods on Windows); parsifal295 (game check passes over locked files), RafaelHGOliveira (exclusive fullscreen), fliperama86 (HDR desktop black level), emurphy04 (ultrawide output, video queue fix) |
| ALuaNegra (via Mrsuss60's port) | Idle depth targets freed (VRAM growth) |
| HatsyRei (via Mrsuss60's port, PR #152) | Windows system DLLs no longer shipped; licence notices for the shipped DLLs and built-in libraries (`out/licenses`) |
| Vorshim92 | Pipeline cache kept between starts, stale `bbport.exe` guard, crash reaper, `run.bat` resolution fix, Reset shader cache button |
| Thealexbarney | LibAtrac9 audio (MIT) |
| ocornut | Dear ImGui (MIT) |
| Kyo, Lance McDonald, illusion, emoose, auser1337 | 60 FPS, camera and engine patches |
| vladtepes22 | Boss stays dead when the guests finish it while the host lies dead in the fight (PR #6); VPN joins with a host behind a closed NAT (PR #7); Chalice Dungeon entry with a party (PR #8); co-op bug reports |
| zackcage6 | Testing and bug reports |
| Noxde, Valentino Amato | [Bloodborne-save-editor](https://github.com/Noxde/Bloodborne-save-editor) (GPL-3.0): its save parsing, ported in `tools/party/editor_check.py` to check that exported saves open in it |

Also used: half, DejaVu fonts, Libre Baskerville (SIL OFL 1.1), dxil-spirv (MIT). AMD's FSR 4 files and NVIDIA's DLSS SDK are not
included here. Release packages include NVIDIA's `nvngx_dlss.dll` and `nvngx_dlssg.dll` under its license
(`out/NVIDIA_DLSS_LICENSE.txt`). The licences of every other shipped DLL and built-in library are in `out/licenses` (`msys2/PACKAGES.txt` lists each DLL's package and version).
