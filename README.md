# Bloodborne PC — native Windows port

Run the PS4 version of *Bloodborne* (v1.09, any retail release) natively on Windows with Vulkan.
Based on [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28 (Linux) and the original
[Windows port](https://github.com/Mrsuss60/bloodborne_pc_windows_port) by Mrsuss60.

**Version 0.6.5** · Playable · Party co-op in development

> No game files are included: you need your own decrypted dump with update 1.09 (any retail
> release: CUSA03173, CUSA00900, ...; they share the same 1.09 executable).
> Not affiliated with Sony, FromSoftware, AMD or NVIDIA. Not related to shadPS4: questions about
> the Linux port go to its [Discord](https://discord.gg/KYZRKk9CB).

### Seamless co-op progress

Parity with Elden Ring Seamless Co-op, measured row by row in [ERSC_PARITY.md](docs/party/ERSC_PARITY.md).
Two numbers: **plan done** is how much of the plan is built and seen working at least once;
**ERSC parity (evidence-weighted)** caps every row at 95% until it has been checked on two real PCs over
the Internet (every check of the current build ran on one PC; the only Internet session so far was on 0.6.0), and caps rows where we chose to differ from ERSC (the
whole party respawns with the host, 4 players instead of 6, one shared drop roll) at 95% too.

| Area | Plan done | | ERSC parity (evidence-weighted) | |
|---|---|--:|---|--:|
| **Overall** | `████████████████████` | 99% | `███████████████████░` | 95% |
| Session & joining | `████████████████████` | 100% | `███████████████████░` | 95% |
| World & travel | `████████████████████` | 100% | `███████████████████░` | 95% |
| Death & respawn | `████████████████████` | 99% | `███████████████████░` | 94% |
| Progress (anyone can progress) | `████████████████████` | 99% | `███████████████████░` | 95% |
| Services & UI | `████████████████████` | 100% | `███████████████████░` | 95% |
| Network & robustness | `██████████████████░░` | 93% | `██████████████████░░` | 92% |
| Bloodborne-specific (chalice, Insight) | `████████████████████` | 100% | `███████████████████░` | 95% |
| Feels like single player in a party | `████████████████████` | 100% | `███████████████████░` | 95% |

---

## What this port adds

| Feature | What you get |
|---|---|
| **Native on Windows** | No emulator, no Wine. The game's own code runs on your CPU; the graphics go through Vulkan. |
| **Party co-op, no server** | Play the whole campaign with friends over a direct connection: no PSN, no server. Modelled on Elden Ring Seamless Co-op. [More](#party-co-op) |
| **Settings in the game's menu** | Every port setting is a page in the game's own *System* menu, with gamepad navigation. |
| **High frame rates** | 30 / 60 / 90 / uncapped, with correct game speed: hooks in the game's code follow the real frame time (deadinside's, 0.6-pre1; the community FPS++ patches as a fallback, `BB_FRAME_HOOKS=0`). |
| **Upscaling** | FSR 3.1, FSR 4, FSR 4.1.1 and DLSS, with motion vectors added for this game. |
| **Frame generation** | Optional, off by default: DLSS FG (2x-4x, RTX 40/50) or FSR FG (2x), paced, HUD kept sharp; NVIDIA Reflex. [More](docs/FG_REFLEX.md) |
| **Finer motion blur** | Optional, off by default: the game's motion blur, which it builds from half-resolution buffers, at 2x the pixels or at full resolution (*System > Display > Motion blur resolution*; 0.6.2 called it *Volumetric effects*, but smoke and mist are already drawn at full resolution). [More](docs/SETTINGS.md) |
| **HDR output** | Optional, off by default: HDR10 (PQ, Rec.2020) on an HDR display, the game's highlights expanded up to your display's peak while mid-tones and menus stay at paper white; peak, paper white, highlights, saturation and a calibration pattern in *System > HDR*. [More](docs/SETTINGS.md#hdr-output) |
| **No shader stutter** | Known shaders are built before the intro. New ones compile in the background. |
| **Shareable shader cache** | Export / import one `.zip`, works across GPUs; drop a shared one into `user\cache_import\`. |
| **Shader cache builder** | *Advanced / Debug > Shader cache builder > Build shader cache (world warper)*: the game travels through every lamp and area by itself (looks round, walks, changes armor and weapons, fights every kind of enemy, plays the cutscenes, day / evening / Blood Moon), on a copy of your save, then exports the cache `.zip`. Two to three hours; Stop and resume. Opt-in faster loading screens: `BB_FAST_LOADING=1` (3-8 % shorter, but the loading animation stops). [More](docs/SETTINGS.md#shader-cache-builder-world-warper) |
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
| Gestures / key items | Touchpad left / right half (Xbox pads: `Back` + d-pad left / right; `Back` alone the left half) | `Tab` or `G` / `Backspace` |
| Walk (hold while moving) | Push the stick part way | `Left Alt` |
| Fullscreen | — | `Alt + Enter` |

Button prompts follow the device you used last: your keys and mouse buttons, Xbox letters or Nintendo letters (PlayStation pads keep the game's icons). Launcher *Input & Audio > Button prompts* (or *System > Mouse & keyboard* in game) picks a set, e.g. *Nintendo* for an 8BitDo pad in X-input mode; button functions never change.

Mouse and keyboard, button prompts, free camera, debug menu: [docs/SETTINGS.md](docs/SETTINGS.md).

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
  within seconds (in the same area about 5 s from the bell to standing beside the host; a join that
  stalls is restarted by itself). *Continue party game* takes a guest straight into the host's world with a single
  loading screen. A guest whose own world is in another map (the Hunter's Dream, say) also joins with a single loading screen, beside the host. If the host travels while you are joining, its travel waits until you are in and
  you travel together.
- **Joining while the host fights a boss or watches a cutscene** waits in your own world until it is over (the Party status says why), then puts you beside the host with one loading screen.
- **Leave any time.** *System → Party → Leave the party* takes a guest home at once; the host's
  *Leave* ends the party and every guest goes home at once (one loading screen, no "Session lost"). A guest who left can rejoin at any time, also right after a boss kill. After leaving, a guest keeps playing online alone in its own world. *System → Party mode → Join party* brings a
  guest back; *Host party* and *Single player* are there too.
- **The session never ends by itself.** Boss kills, area clears, deaths and travel don't send
  anyone home.
- **No multiplayer walls.** The fog walls that keep phantoms near the host are gone; the party can
  split up and explore. There is no leash: a guest can walk far from the host (checked at 140 m)
  and stays in the party, and host and guest can each walk on into another part of the city (the
  Cathedral Ward and Central Yharnam are streamed separately), fight, use items and die there for
  minutes while the other plays elsewhere: nobody is pulled back.
- **Anyone can go through a boss fog first**, a boss's first fight included: the fight starts for
  everyone and the others follow through the fog. When a guest starts a first fight, the intro
  cutscene plays on every screen; the host's own picture does not jump (its character visits the
  arena entrance unseen, stays there through the intro and is put back where it stood). The DLC
  bosses work the same way; Ebrietas and Lady Maria still start with the host (a guest at that fog
  is told so on screen), and Rom starts with anyone's hit.
- **Travel from any lamp to any lamp.** A lamp offers the headstones' list (the Hunter's Dream and every lit
  lamp) and takes you straight there with one loading screen, solo or with the party (launcher: "Direct lamp
  travel", on by default).
- **Travel together or alone.** Lamps, headstones and the Hunter's Mark ask "Travel with the party?".
  Yes moves the whole party with one loading screen; No moves only you (when a guest rests that way,
  the host gets a short rest load so its world resets). The question closes by itself after 30 s.
  A guest's own save keeps its own spot: playing it solo later starts where it was, not at the
  lamp it travelled to with the party.
- **No loading screen inside the same map for the host.** The host's lamp or Hunter's Mark to a spot
  in the map it is already in is an instant warp (the game's own move, as after its cutscenes, so the
  camera and the area look as after a loading screen), for the whole party too; a guest's gets its own
  short loading screen and the host stays where it is; at the lamp you already stand at, nothing
  moves at all. With nobody to travel with (a host alone, or "Join a party" while the host is not
  there) lamps, headstones and the Hunter's Mark work exactly as in single player.
- **Guests can use the Hunter's Mark** (on by default; launcher switch "Guests can use Hunter's
  Mark"): alone it takes you to your own last lamp, with the party to the host's.
- **Apart and back together.** While players are in different maps each sees only the others in
  its own map; meeting again shows them at once, without a loading screen. If the host dies while
  you are in another area you play on where you are (no loading screen for you); if you die there
  you get up at your own lamp. A guest who joined from the title stays put too when the host travels
  alone; enemies it killed while the host was away are back once the host returns (the host's world
  counts, as after a reload).
- **Everyone can use lamps.** Vanilla disables lamps during co-op; here they work for everyone.
- **Lamps are shared.** A lamp anyone in the party lights is lit for everyone, in everyone's own
  save too: when you join, the lamps you lit on your own light up in the host's world (the party
  can travel there), and the host's lamps go into your save. A lamp past a boss the host has not
  beaten yet waits until the host beats it. The host can turn this off (*Shared lamps*).
- **Resting resets the world for the party.** Enemies respawn; everyone's HP, vials and bullets
  refill. A guest who rests and travels alone gives the host a short loading screen where it
  stands, so the host's world resets too; a guest resting its way back into the host's area
  arrives after that reset with a single loading screen (also when a guest uses a lamp of the area
  the party is in). Resting while the host is in a boss fight
  takes everyone out of the boss room (the host's world loads at its last lamp, the fight resets).
- **One NG cycle per world.** In the host's world everyone plays at the host's NG cycle (enemy
  health, damage and Blood Echoes are the same on every game, as in the original game); your own
  save keeps your own cycle, and your own world is back at it when you go home.
- **Guests are full players.** The Doll (level up), shops, the workbench, storage, the Memory
  Altar (memorize your own Caryll Runes; covenants), lamps and headstones all work for guests. A guest can also rest alone
  (lamp, "No"), level up and shop in the Dream and come back, while the others play on.
- **Change your appearance at the Dream's mirror.** The mirror in the Hunter's Dream workshop opens
  the appearance editor ("Put on Disguise", a feature the game left unfinished, restored as in
  bbhost) in character creation's own frame (the book and a live preview of your hunter), in single
  player and for every party member on their own hunter; the others see your new look as soon as
  you close it, with no loading screen. Launcher switch "Hunter's Dream mirror", on by default;
  everyone in a party needs the same setting.
- **Progress counts for everyone.** Bosses, items, doors, shortcuts, lamps, story and endings go
  into every player's own save. A guest who is behind catches up, including the Blood Moon.
  Key items a guest earned in the host's world (Oedon Tomb Key, badges, workshop tools) are checked
  each time its own world loads and given again if its save lacks them, and boss key items (the
  chalices, the umbilical cords) a guest could not take in the host's world are kept for it until
  its own world loads, even if it always joins from the title; a key item you already hold is never
  handed out twice (no "inventory or storage max" box).
- **Story cutscenes and endings play for everyone.** The Blood Moon, Amelia's skull (evening to
  night), Mergo's Wet Nurse's arena, Gehrman's scenes and the ending with the end credits
  play for every party member; a guest still loading in when a scene starts sees it once it has
  arrived. A guest who opens the Cathedral Ward door first turns the day into evening for the whole
  party (the scene plays for everyone). After the Blood Moon the host is asked "Travel with the party?" and everyone goes to
  Yahar'gul together. A guest can start the Blood Moon too, by walking up to Yharnam at Moonside
  Lake, and the story messages only the host's game shows (Yahar'gul's "Ritual secret broken")
  appear on every guest's screen with an OK.
- **Your ending, your choice.** When the host reaches an ending, every guest is asked first:
  *Yes* plays that ending in your own save (end credits, then New Game+), *No* takes you home to
  your own Hunter's Dream and your save stays as it was, to reach the ending later. A guest who
  used three Umbilical Cords gets Childhood's Beginning when the host refused Gehrman.
- **Guests open gates with their own keys.** The Cathedral Ward main gate opens for a guest
  carrying the Hunter Chief Emblem, for the whole party.
- **Insight for everyone.** Whatever Insight the host gets (a boss's first sight and kill, story
  moments, the kidnap's waking, Chalice Dungeon bosses) every guest present gets too; a boss's
  first-sight Insight comes once per player, whoever walks in first. What Insight
  shows (Amygdalas, whispers) is up to each player's own Insight, as in the original game.
- **The host's world is shared live, both ways.** A door or shortcut opened by the host is open
  for the guests too, and a lever or door a guest uses opens in the host's world (and in the
  guest's own save).
- **Guests can talk to NPCs** in the host's world (Gilbert, Iosefka, Eileen ...); quest choices
  are shared by the whole party: a quest step the host makes also moves the NPC in every guest's
  own save (no stale NPC state left behind). NPCs look solid for guests (no see-through
  "ghosts", the suspicious beggar, Willem and the DLC's talking NPCs included), and a level-up at
  the Doll stays in the levelling player's own game.
- **Hear each other's NPC talks.** When a companion near you talks to an NPC you see the game's
  subtitles and hear the NPC too, with a small "[Hunter talking to Iosefka]" caption and the
  choices they pick and the items the NPC gives them; nothing opens on your screen and nothing is
  drawn over your own menus. Further away you get a quiet "Hunter is talking to the Doll". Range in
  *System → Party rules → NPC dialogue range* (default 25 m), off with *Share NPC dialogue*.
- **NPC gifts are shared**: an item an NPC gives a guest in the host's world (e.g. Eileen's Bold
  Hunter's Marks) goes to the guest, the host and every other party member, once (the NPC has moved
  on for everyone, so nobody misses it). Gifts an NPC hands out again and again (Blood of Arianna,
  Blood of Adella, Iosefka's Blood Vial, Blue Elixirs, Queenly Flesh) go to whoever asked.
- **Kidnapped by a Snatcher?** If the host is grabbed, the whole party wakes up together in the
  Hypogean Gaol (Yahar'gul) after one loading screen, and the guests get the host's Insight for
  it too. Guests cannot be kidnapped in the host's world.
- **Enemy drops for each player, one roll.** Every player gets their own copy of each enemy's drop
  from one shared roll per kill (the same dice for everyone), and each player's own Item Discovery
  counts: a player with more Discovery gets a rare drop whenever one with less does, and sometimes
  when they do not.
- **Blood Echoes for every kill.** Whoever kills an enemy, every party member gets its full
  Blood Echoes.
- **Treasure for each player.** Items lying in the world can be picked up by every player, each into their own game.
  Keys and workshop tools found lying around (Hunter Chief Emblem, Upper Cathedral Key, Iron Door Key,
  Lecture Theatre Key, Rune and Blood Gem Workshop Tools, ...) and the Eye of a Blood-drunk Hunter go to
  the whole party as soon as the host has them (or a guest picks one up), so nobody is locked out of a door.
- **Cutscenes for everyone.** A story scene a guest triggers in the host's world plays for the
  whole party too, a boss intro a guest starts included; a guest can also examine Vicar Amelia's
  skull (the night comes for everyone).
- **Boss fights need the host's area.** A guest who travelled alone cannot start a boss fight
  where the host is not (the fog stays closed, with a message); if the host leaves during a fight,
  the guest is taken to the host with one loading screen.
- **Up to 4 players** (host + 3), all in the host's world; every guest sees every other guest, and
  anyone can leave and rejoin while the others play on.
- **Same version for everyone.** A player whose bbport build, game files, patches, mods or party
  options differ is refused on joining; the message says what differs and what to do.
- **Chalice dungeons together.** Guests join the host's dungeon; levers, doors, the layer boss, its
  loot (for each player) and the lift with its call levers at the top and the bottom work for the whole party, layer after layer; a guest can also join from its title straight into the host's dungeon. Any player can perform the ritual:
  the host's altar then offers *Enter the party's dungeon*, also with guests standing in its Dream,
  and one press does the rest (the guests follow in with one loading screen). Chalices are shared
  between all players, and a boss reward the game keeps for the host goes to every guest in the
  dungeon too.
- **Crashes recover.** A crashed game restarts and rejoins by itself (on one PC: in the host's party about 15 s after the crash, standing beside the host about 20-27 s after it, nearly all of that the game's own start, its online sign-in at the title and its loading screen), next to the host: a guest whose own save is in another area first travels to the host's lamp with one loading screen. Nobody is summoned into a prologue. If the host's game crashes, the guests go to their own world and wait ("Waiting for the host to come back (N s)...") until the host's game has restarted, then are summoned back beside it by themselves (about 20 s after the crash on one PC; after 3 minutes without the host a guest plays on alone); items a guest got in the host's world stay in its own save.
- **Network drops recover.** A network outage of up to about 2 minutes keeps the session (checked on one PC with 60, 90 and 120 s outages; 50 s with three players): no loading screen, everyone carries on where they stood once the network is back (the others freeze in place meanwhile). If a longer outage loses the session, a guest goes home at once with one loading screen and is summoned back beside the host by itself once the network is back. The party also checks every 15 s that everyone sees the same world and progress and quietly re-sends whatever differs.
- **Wrong passwords are locked out.** A few typos are fine; after 5 wrong passwords in a minute the host refuses that address for a while.
- **Your single-player save is safe.** Party play uses a separate save, backed up every 10 minutes.
- **Undo a party session.** Right before every party session (host or guest) the game copies both
  save folders aside. The launcher's *Party* tab has **Undo last party session**: it shows what it
  will replace, keeps your current saves aside the same way (so the undo can be undone) and puts
  the saves back byte for byte. After a party session the launcher also checks the party save
  (every character loads, the NG cycle only changes with an ending, no boss kill or lamp from
  before the session is lost, level and play time never go down) and offers the undo if something
  looks wrong.
- **New version out?** The launcher says so ("Version X is out - every PC in the party needs the
  same version", with the download link; at most one check every 6 hours, off in *Advanced /
  Debug*). A join refused for a version mismatch names the host's version and yours.
- **Death has a cost.** Each death stacks "rot" (−10% max HP, −10% max stamina, +10% stamina
  use, −15% HP won back by hitting after a hit (rally) or −10 Item Discovery on enemy drops, picked at random, up to 5); using a lamp (even just opening its travel menu and cancelling) or levelling up clears it.
  It belongs to your character: each save slot keeps its own, and a new game starts without it. On by default; the host can turn it off on the *Party rules* page.
- **Enemies scale with the party, harder than vanilla.** Bosses get more health (and a little more
  damage) per extra hunter than the game gives them, normal enemies get tougher too; all six values
  (boss / enemy health, damage, poise) are on the *Enemy scaling* page, the host's count.
- **Names over party members**, in the game's own style (replacing its plate, which hid behind walls), at any distance, bigger when near, with level, health, deaths or ping (your choice); the *Party name tags* setting (launcher or *Party rules*) turns them off for the game's own plate. Names, the party panel and party notices step aside while the game's own menus (inventory, item descriptions, level up, travel) are open; for a YES / NO box only the names do.
- **Pings.** Hold the touchpad and press R3 (Xbox pads: hold Back, press R3; keyboard: Tab + Q, or
  just V) to mark the spot you are looking at: every party member in your world sees the marker
  through walls for 10 seconds, in your colour, with your name and its distance in metres (at the
  screen edge with an arrow when it is out of view). The touchpad alone still opens the gestures
  (when you let go). Optional: *Show party through walls* puts a small marker with the distance on
  every companion on your map. Both on the game's *System -> Pings* page and the launcher's Party tab;
  hidden under the game's menus like the other party text.
- **Friendly fire / PvP (host's switch, off by default).** The host can let party members hurt
  each other, both ways, like invaders: the game's *System -> Friendly fire* page, the Insert menu's
  Party tab or the launcher's Party tab (host). Everyone gets a notice when it changes; guests see
  the host's setting on their page but cannot change it. A party kill is a normal party death (the
  rot applies as for any death); the killer gets no Echoes or Insight from it. Checked live with
  three players: guests can hurt each other too, guns and thrown items hit (Poison Knives poison),
  and a host killed by a guest goes through the party's normal host-death respawn.
- **Party HUD.** Party members and events are shown in a small HUD; all party settings are on the
  *Party* pages of the game's *System* menu.

### Deaths

| Who dies | What happens |
|---|---|
| A guest | Gets up again at its own lamp: with no loading screen when that lamp is in the loaded area, else with its own loading screen at that lamp (also in another area; the host stays where it is); its Blood Echoes wait in a bloodstain where it fell, marked on screen in the host's world (walk onto it to get them back; it also stays in its own world); the others keep playing |
| The host | Everyone gets one respawn loading screen at the host's lamp, as in the original game, and the world resets; the host's echoes stay in a bloodstain where it fell |
| Anyone, during a boss fight | Watches a living party member fight from behind them (the screen says whose; press Cross to watch the next one) until the boss dies (then gets up at the host's lamp, with no loading screen when it is in the area), everyone dies, or someone travels |
| Everyone | Everyone respawns together at the host's lamp; the session stays |

A guest's death brings no enemies back; a host death, a rest at a lamp (anyone's) or a party wipe resets them.

### Coming next

| Change | Status |
|---|---|
| FSR 4 white or grey picture fixed | In progress: protective fixes built; the cause (FSR 4 with HDR on in Windows) is being confirmed |
| Frame rate through game hooks: physics, running and timers correct at 60-144 FPS and uncapped | In progress (port of deadinside's 0.6-pre1 work) |
| Guests load at the same time as the host on party travel (about 10-15 s less per trip) | Planned (seamless research, opt-in first) |
| Host death the ERSC way: the host loads alone, guests play on | Planned (opt-in first; today the whole party reloads) |
| Game-style fades instead of blinks for in-world moves and revives | Planned |
| Sharper smoke and fog checked with every upscaler, HDR and 21:9 | In progress |
| Shader cache builder: bosses, gestures and hunter tools; a fuller shared cache | In progress |
| Fixes from Supermedo 1.8.2 (keyboard sprint through direction changes, exFAT / FAT32 drives, read-only game files, VRAM fallback on 4-8 GB cards) | In progress |
| Upstream renderer and memory model: measured evaluation | In progress |
| A long unattended two-player test before every release; party code cost per frame; a co-op guide page in the launcher | Planned |
| A real two-PC Internet session on the current build | Needs players: see `docs/party/playtest2_checklist.md` |

### Known issues

- **FSR 4 can show a white or grey picture** on some PCs. The cause is under investigation (suspected:
  FSR 4 with HDR turned on in Windows). Meanwhile switch to FSR 3.1, DLSS or no upscaler.
- **Co-op on the current build was tested on one PC** (several game copies, simulated internet lag);
  a real two-PC Internet session on 0.6.2 is still to come. Please send support bundles if something goes
  wrong (see *Reporting a co-op bug*).

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
| deadinside28 | Original Linux port (bbport): flat memory image, two-stage Vulkan pipeline, motion vectors; Linux mouse look capture (via Mrsuss60); in 0.6.2 fixes from 0.6-pre1 (Supermedo/bloodborne_pc 72e5c8ce): idle VRAM copies released on 4 GB cards, NVIDIA start without DLSS, unresolved Vulkan commands, pipeline cache stages, write-trap race, occlusion pass scale, sampler anisotropy clamp; next release: button prompts for the keyboard, Xbox and Nintendo pads (72e5c8ce); the frame rate by hooks in the game's code (0.6-pre1) |
| Mrsuss60 | Original Windows port: Win32 runtime, Vulkan on Windows, build scripts, character creation and keyboard fixes, touchpad Gestures and Personal Effects menus, performance HUD; audio pop fix, paths with parentheses in `run.bat`, parallel `build.bat`; CPUs without FSGSBASE (Windows TLS fallback), driver pipeline binaries kept between sessions, more background shader compile threads; SnormNz crash fix (packed formats such as 11_11_10), texture collector never evicting bound targets or presented surfaces; stream buffer placement switch (`BB_STREAM_MEMORY`), no draw-queue wait at `SET_PREDICATION`; mouse sensitivity fix (stick dead zone compensation), in-game key rebinding, focus click not a game press |
| shadPS4 team | Vulkan video core and shader recompiler (GPL-2.0+), sirit |
| droogie and the bbhost team | Long-session stability fixes; menu button rows and the menu cursor's offsets; direct summon delivery; the Hunter's Dream mirror (appearance editor) ([bbhost](https://github.com/droogie/bbhost), GPL-3.0+) |
| FireBurn | FSR-Vulkan (MIT); AMD FidelityFX SDK (MIT) |
| Supermedo, IFreemz | DLSS bridge and Windows loader |
| FIocker (Supermedo/bloodborne_pc PR #69) | Frame generation (DLSS FG, FSR FG) with paced presentation, NVIDIA Reflex, conservative object motion for frame generation, DLSS bridge ABI 2, FG diagnostics tools |
| Supermedo and contributors ([bloodborne_pc](https://github.com/Supermedo/bloodborne_pc)) | Intel 12th gen+ crash fix, FPS-preset physics step (found by FIocker), frame limiter pacing, texture-cache, pipeline-cache and shader-walker fixes, skipped draws for failed pipelines, keyboard stick movement, allocating past a full VRAM budget, object motion off on AMD before RDNA3, the memory log line once a minute, Back + d-pad touchpad halves on Xbox pads (idea by parsifal295), skipped draws with a null vertex buffer table, retried boot-file swaps, the unclean-eboot.bin warning, a bad PM4 header dropping its buffer instead of stopping the game and the GPU line flushed at start (c07f0ffa, from parsifal295's PRs #57 and #61); in 0.6.3 the flip IRQ ahead of its label (76f5b5f6, from Awaken's port), a keyboard sprint kept through direction changes (4b930b10, measured by cassioKenji, PR #74), read-only game files and exFAT / FAT32 drives no longer dropping mods (ba34a3a2 from emurphy04's PR #77, bf0878bf); checked against Supermedo's five PR fixes (3e3b760f): parsifal295's stale texture format (#55), title-menu flicker above 1080p (#56), bmy's loading-screen item picture (#62) and DLSS device fallback (#58, its reason now names the Vulkan result), dGstone18's build path with spaces (#72), all already here; imedved (direct mouse camera, ported by mcrib884); andrecabralgit (stereo audio level), Jgamrot (overlay checkboxes and sliders), dGstone18 (mods on Windows); parsifal295 (game check passes over locked files), RafaelHGOliveira (exclusive fullscreen), fliperama86 (HDR desktop black level), emurphy04 (ultrawide output, video queue fix) |
| ALuaNegra (via Mrsuss60's port) | Idle depth targets freed (VRAM growth) |
| Vorshim92 | Pipeline cache kept between starts, stale `bbport.exe` guard, crash reaper, `run.bat` resolution fix, Reset shader cache button |
| Thealexbarney | LibAtrac9 audio (MIT) |
| ocornut | Dear ImGui (MIT) |
| Kyo, Lance McDonald, illusion, emoose, auser1337 | 60 FPS, camera and engine patches |
| zackcage6 | Testing and bug reports |

Also used: half, DejaVu fonts, Libre Baskerville (SIL OFL 1.1), dxil-spirv (MIT). AMD's FSR 4 files and NVIDIA's DLSS SDK are not
included here. Release packages include NVIDIA's `nvngx_dlss.dll` and `nvngx_dlssg.dll` under its license
(`out/NVIDIA_DLSS_LICENSE.txt`).
