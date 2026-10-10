# Party co-op (playing together)

> **Status: in development.** Party co-op is new. Its parts have been tested on one PC only:
> unit tests, several copies of the game connecting to each other, and single-copy checks in
> the game. It has **not been tested between two PCs** yet, a guest summoned into a host's world
> by the party layer has not been seen end to end, and no playthrough together has been done.
> Expect bugs; keep the save backups (below) and report problems with the logs.

Party co-op lets 2 to 4 players play the Bloodborne campaign together, from character creation
to the ending. One player **hosts**: their game acts as the server, so there is no account, no
PSN and no central server. The others **join** with a short **party code** the host sends them.

Every player keeps their own character and save. Guests are summoned into the host's world as
in the normal game's co-op, but the party layer removes most of co-op's limits: guests are
summoned automatically, stay through boss kills and area changes, follow the host when the host
travels, and the host's story progress, item rewards and cutscenes are carried over into each
guest's own save.

## Contents

1. [What you need](#what-you-need)
2. [Hosting](#hosting)
3. [Joining](#joining)
4. [The party code](#the-party-code)
5. [Ports, UPnP, firewall, VPN](#ports-upnp-firewall-vpn)
6. [What happens in the game](#what-happens-in-the-game)
7. [What is shared, and what is not](#what-is-shared-and-what-is-not)
8. [Settings (Rules & sync)](#settings-rules--sync)
9. [Saves and backups](#saves-and-backups)
10. [Crashes and disconnects](#crashes-and-disconnects)
11. [Four players](#four-players)
12. [Troubleshooting](#troubleshooting)
13. [Known limitations](#known-limitations)

## What you need

- Every player: this port, set up and working single-player, with the **1.09** game files.
- The **same gameplay patches and gameplay mods** on every PC. Graphics, FPS, resolution and
  other cosmetic patches and texture / sound mods do not matter; anything that changes gameplay
  must be the same, or joining is refused (the message says which patch or mod differs).
- The **same Max players** setting on every PC (Party tab).
- Ideally the same build of the port: an older build may use another party protocol version.
- The Old Hunters DLC on all PCs or on none (the port turns it on automatically when the game
  files contain it, so this normally matches).

## Hosting

1. Start the launcher (`launch_gui.bat`) and open the **Party** tab.
2. **Mode: Host a party.** Enter **Your name** (1-16 letters, digits, `_` or `-`; the others see
   it in the game).
3. Pick **Max players** (3 is the game's normal co-op size; 4 is experimental).
4. Leave **Automatic summoning**, **Seamless party rules** and **Separate party saves** on.
5. Optional: a **Password** (every player must then enter the same one).
6. Click **Launch Bloodborne**. With *Separate party saves* on, the first time the launcher
   offers to copy your single-player save into the party save folder (or start with an empty
   one for a new character).
7. Once the game runs, your **party code** appears under **Your code** (the launcher log shows it
   too). Click **Copy code** and send it to your friends (chat, mail ...).
8. Play. In party mode the game confirms **Continue** on the title screen by itself a few
   seconds after the party is up, so your last character loads without input (with an empty
   party save, create a character as usual). The others are summoned into your world
   automatically when they are ready.

The code stays the same when the game restarts after a crash; it changes when your public
address or port changes.

## Joining

1. **Party** tab, **Mode: Join a party**, enter **Your name** (must differ from the other
   players' names).
2. Paste the host's code into **Party code** (the **Paste** button takes it from the clipboard).
3. Set **Max players** (and the **Password**, if the host uses one) to the same values as the host.
4. Start the game **after the host's game is running**. Once connected, the game confirms
   **Continue** on the title screen by itself (with an empty party save, create a character).
5. Play the prologue in your own world (see below). Once you are ready, the host's game summons
   you; you appear as a helper in the host's world.

The launcher shows a ✓ or ✗ hint under the code field if the name, port or code look wrong.

In the game, the overlay menu (**Insert**, or **L3 + R3** if enabled) has a **Party** tab: the
party's state, the members, the code (copy), **Rejoin**, **Leave** and, for the host, **Kick**.
Players joining and leaving are shown as short messages.

## The party code

A code looks like `BBP1-XXXX-XXXX-...`. It holds the host's address and port and a secret; the
secret is what lets a guest in, so share the code only with the people you play with. Letters are
not case-sensitive. The game writes two codes to `user\party_code.txt`: an **Internet** code (your
public address, found through the STUN server or the router's UPnP) and a **LAN** code (for
players on the same home network). The launcher shows the Internet code.

Instead of a code, a guest can also enter the host's address directly as `host:port`, e.g.
`192.168.1.20:9307` on a LAN or `100.64.1.5:9307` over a VPN.

## Ports, UPnP, firewall, VPN

- The party uses **one port on the host, UDP and TCP, default 9307** (Party tab, *Port*). Guests
  connect to it; guests do not need any open port.
- **UPnP** (on by default) asks the host's router to open that port while the game runs. Many
  routers allow this; if yours does not, the log says *"UPnP unavailable: forward UDP+TCP port
  9307 to this PC in your router"*. Then forward the port by hand in the router's settings
  (UDP and TCP, to the host PC's LAN address).
- **Windows Firewall**: the first time you host, Windows may ask whether `bbport.exe` may use the
  network. Allow it (at least on *Private* networks; also *Public* if Windows calls your home
  network public). If you clicked *Cancel*, allow the program in *Windows Security > Firewall >
  Allow an app through firewall*.
- **Public address**: normally found automatically (STUN server `stun.l.google.com:19302`, or
  UPnP). For a fixed IP, a DNS name or a VPN address, enter it in *Public address* (host only).
  *STUN server* `off` turns the lookup off (LAN / VPN only).
- **Carrier-grade NAT / no port forwarding possible**: use a VPN that puts all players in one
  virtual network, such as Tailscale or ZeroTier. The host shares its VPN address as
  `address:9307` (or sets it as *Public address*, so the code contains it).
- Guests talk to each other directly when they can; when a direct path does not work, their game
  traffic goes through the host automatically.

## What happens in the game

1. **Prologue, each player alone.** Everyone creates their character (or loads one) and plays the
   short prologue in their own world: the opening, the clinic, the first visit to the Hunter's
   Dream and taking a weapon. Until then the others see *(prologue)* next to your name. The
   party gives each player the Beckoning Bell and the Small Resonant Bell when the prologue is done.
2. **Summons.** When a guest is ready and in their own world, their game rings the Small
   Resonant Bell and the host's game rings the Beckoning Bell, every 30 seconds, until the guest
   is in the host's world. No Insight is needed or spent, and it works in areas where the normal
   game forbids co-op (including after the area's boss is dead and in the Hunter's Dream).
3. **Together.** Guests keep their full HP (the normal game gives helpers 70 %), are not sent home
   when a boss dies, and keep the session when the map reloads.
4. **Travel.** When the host uses a lamp, goes to the Hunter's Dream, dies or uses a Hunter's
   Mark, the guests follow: they travel to the same place in their own world and are summoned
   again. This is "follow and rejoin", not a seamless shared loading screen - expect a short
   moment alone after each trip.
5. **A guest dies.** The guest comes back at the host's last lamp and is summoned again (instead
   of being sent home), and gets the vials and bullets refilled.
6. **Bosses.** As in the normal game, the host has to walk through a boss fog first.

## What is shared, and what is not

**Shared from the host to every guest (into the guest's own save):**

- **Progress**: bosses killed, lamps lit, shortcuts, doors, elevators and ladders opened, key
  story events. A guest's world catches up with the host's when the guest goes home.
- **Items**: boss drops, key items, keys and gifts the host receives, which helpers never get in
  the normal game, are given to each guest once, in their own world.
- **Rewards**: guests get the full Blood Echoes for kills and boss kills and full enemy drops
  (the normal game halves echoes and drops almost nothing for helpers), and the same Insight
  for a boss as the host.
- **Cutscenes**: each cutscene the host triggers is also shown to every guest - live if the guest
  is in the host's world, otherwise later in the guest's own world. The **time of day** (evening,
  night, Blood Moon) follows the host.
- **Endings**: the ending the host gets is played for every guest too.

**Not shared (each player's own):**

- Character level, stats, inventory, equipment, Blood Echoes and Insight you already have.
- Shop purchases, the Doll, Hunter's Dream upgrades, runes, chalices.
- **NPC quests**: off by default, because a quest is a chain of steps and a partly copied quest
  can get stuck (Rules & sync: *Also NPC quests*, experimental). Gifts NPCs give in dialogue are
  not handled.
- **Chalice Dungeons** are not part of party play.
- World treasure a guest picks up while in the host's world does not exist for guests in the
  normal game; the host's pickups are handed to guests through the item sharing above.

## Settings (Rules & sync)

The Party tab's **Rules & sync...** button opens the rest of the options. The defaults are the
recommended settings; hover any option in the launcher for details. Settings marked *guest*
only matter on the PC of a player while they are a guest; *both* need to be on for the host
and the guest.

| Option | Default | What it does |
|---|---|---|
| Party starts | After the prologue | When a player can be summoned. *Right away* = right after the opening cutscene (inside the clinic, experimental). Keep it the same for everyone. |
| Give the bells | on | Gives each player both bells after the prologue (for ringing by hand). |
| Open world | on | Removes the co-op walls on area borders. Keep it the same for everyone. |
| Respawn at the host's lamp | on | *Guest*: after death, back at the host's last lamp and summoned again. Off: sent home as in the normal game. |
| Refill vials and bullets | on | *Guest*: refilled when the host rests, dies or goes to the Dream, and after the guest's own respawn. |
| Guests can use Hunter's Mark | off | *Guest*: a Mark takes the guest to the host's last lamp. |
| Full rewards for guests | on | *Guest*: full echoes and drops. |
| Share the host's item rewards | on | *Both*: boss drops, keys and gifts given to each guest. |
| Insight for bosses | Same as the host | *Guest*: Insight for a host boss kill. *Full bonus* = that much on top of the game's +1; *Game's own* = only +1. |
| Share story progress | on | *Both*: progress copied into the guests' saves. |
| Also NPC quests | off | *Guest*: NPC quest steps too (experimental). |
| Share cutscenes and endings | on | *Both*: cutscenes, time of day and endings. |
| Play them live with the host | on | *Guest*: watch with the host while in the host's world. |
| Back up every (min) | 10 | Save backup period during a party session (0 = only at start). |
| Backups to keep | 10 | Number of backups kept. |
| Restart the game after a crash | on | Restart, continue and rejoin automatically. |

Party tab options: **Automatic summoning** (off: ring the bells yourself), **Seamless party
rules** (off: the normal game's co-op rules; the host's setting counts), **Separate party
saves**, **Max players**, connection settings. The environment variables behind each option are
listed in [party/DEBUG_SWITCHES.md](party/DEBUG_SWITCHES.md).

## Saves and backups

- **Separate party saves** (on by default): a party session plays from `user\savedata_party`;
  your single-player save in `user\savedata` is never written by it. The launcher offers to
  copy your single-player save the first time, and the **Copy single-player save to party save**
  button does it any time (an existing party save is backed up first). Off: the party plays on
  your normal save.
- Progress the host shares is written into **each guest's own save**, so after a session your
  character's world has the bosses, lamps and shortcuts of the party's progress.
- **Backups**: during every party session the game copies the save folder to
  `user\save_backups\savedata_party\` (or `...\savedata\`) at the start, after a crash restart and
  every 10 minutes. The newest 10 are kept; a copy identical to the last one is skipped. The game
  checks the save at every start (*"Saves: check OK"* in the log); a backup that fails the check
  is named `...-suspect` and never pushes good backups out.
- **Restoring a backup**:
  1. Close the game.
  2. Rename the save folder, e.g. `user\savedata_party` to `user\savedata_party.broken` (keep it
     until the restored save works).
  3. Create an empty `user\savedata_party` and copy everything from the backup folder into it
     **except** `BACKUP_INFO.txt` (that is the numbered folder, e.g. `1\CUSA03173\...`).
  4. Start the game; the log line *"Saves: check OK"* confirms the restored files.

  `user\save_backups\README.txt` has the same steps.

## Crashes and disconnects

- **Your game crashes**: it restarts by itself after 3 seconds, presses *Continue* on the title
  screen, loads your save and rejoins the party. The host keeps your place for 60 seconds. At
  most 5 restarts in 10 minutes (Rules & sync: *Restart the game after a crash*).
- **The host's game crashes**: guests return to their own worlds and keep trying to reconnect.
  The host's game restarts with the **same party code** and the same member list, and the guests
  are summoned again.
- **Connection lost**: the same as a host crash for the guest; it reconnects automatically
  (after 1, 2, 4, 8 seconds, then every 8 seconds).
- Progress made while someone was away is caught up when they are back.
- The overlay's Party tab has **Rejoin** (try again now) and **Leave** (stop; the game no longer
  reconnects). A kicked player cannot rejoin that session.

## Four players

**Max players 4** (host + 3 guests) is experimental: the game was made for at most 3 players.
The party patches the game to allow a third helper and to scale boss HP for it. **Every player
must set Max players to 4**, the host and everyone joining; a different value is refused with
*"party rules differ"*. With 4 players there is no free slot for an invader. Four players
connecting has been checked (on one PC); a third helper actually summoned into a world has not been tested yet.

## Troubleshooting

The launcher log (and `launcher.log`) shows the game's `Party:` lines. The overlay's Party tab
shows the state and the last error.

| Message / symptom | Cause | Fix |
|---|---|---|
| *version mismatch with the host: eboot.bin differs* | Another game version or dump | Everyone needs the 1.09 `eboot.bin`. |
| *... gameplay patches differ (host only: X; yours only: Y)* | Different XML patches that change gameplay | Enable the same patches (Game tab, *All XML Patches...*). Cosmetic patches do not count. |
| *... gameplay mods differ* | Different mods in `mods\` | Use the same gameplay mods, or turn mods off on all PCs. |
| *... party rules differ (host max 4 players ...; yours max 3 ...)* | Different *Max players* (or 4-player switches) | Set the same Max players everywhere. |
| *different party protocol version ...: update the port* | Different builds of the port | Everyone updates to the same build. |
| *wrong password or party code* | Typo, old code, or password differs | Copy the code again; same Password on all sides (or none). |
| *the name X is already in the party* / *invalid name* | Duplicate or invalid name | Pick another name (1-16 of A-Z a-z 0-9 _ -). |
| *the party is full* | Max players reached | Raise Max players (on everyone's PC) or wait. |
| *kicked by the host* | The host kicked you | Ask the host. |
| Guest stays at *Connecting* | The host's port is not reachable | Host: check UPnP in the log, forward UDP+TCP 9307, allow the firewall prompt; or use the LAN code / a VPN. |
| Connected, but never summoned | The guest is still in the prologue, in a menu, or the host is in a loading screen / boss fight | Finish the prologue; wait for the next bell (every 30 s); use *Rejoin* in the overlay. |
| No code under *Your code* | The game has not started hosting yet | Wait until the game is at the title screen; check the log for `Party:` errors. |
| Save looks wrong after a session | - | Restore a backup (above) and report it with the logs. |

When reporting a problem, include `launcher.log` (and `out\run.log` if present) from every
player, and say who was the host.

## Known limitations

- **Not tested between two PCs.** Everything so far ran on one PC with several game copies; real
  Internet connections, routers and firewalls have not been tried.
- A full campaign has not been played through as a party. Several parts (guest respawn, open
  world, refills, the progress and item sharing during a real session, endings for guests) are
  built and unit-tested but have not run in a real two-player session yet.
- Travel is "follow and rejoin": guests load separately and are summoned again after the host's
  lamp trips, deaths and area changes. A shared seamless loading screen is not planned for now.
- The host must enter a boss fog first; guests cannot use lamps, shops or the Doll while in the
  host's world (they can at home).
- NPC quests are not shared by default; dialogue gifts from NPCs are not handled.
- Chalice Dungeons are not supported in party play.
- 4 players: experimental, no slot for invaders.
- An item a guest picks up in the host's world just before a crash may be given twice.
- The online play of the original game (PSN messages, bloodstains, invasions from strangers) is
  not available; the party only connects the players who have the code.

For developers: the design and status are in [PARTY_COOP_PLAN.md](PARTY_COOP_PLAN.md) and
[party/](party); test and debug switches in [party/DEBUG_SWITCHES.md](party/DEBUG_SWITCHES.md).
