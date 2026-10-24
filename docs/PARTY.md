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
2. [Starting a party game in the game (no launcher)](#starting-a-party-game-in-the-game-no-launcher)
3. [Hosting](#hosting)
4. [Joining](#joining)
5. [The party code](#the-party-code)
6. [Ports, UPnP, firewall, VPN](#ports-upnp-firewall-vpn)
7. [What happens in the game](#what-happens-in-the-game)
8. [What is shared, and what is not](#what-is-shared-and-what-is-not)
9. [Settings (Party Rules and My Party tabs)](#settings-party-rules-and-my-party-tabs)
10. [Saves and backups](#saves-and-backups)
11. [Crashes and disconnects](#crashes-and-disconnects)
12. [Four players](#four-players)
13. [Troubleshooting](#troubleshooting)
14. [Known limitations](#known-limitations)

## What you need

- Every player: this port, set up and working single-player, with the **1.09** game files.
- The **same gameplay patches and gameplay mods** on every PC. Graphics, FPS, resolution and
  other cosmetic patches and texture / sound mods do not matter; anything that changes gameplay
  must be the same, or joining is refused (the message says which patch or mod differs).
- The **same Max players** setting on every PC (Party tab).
- Ideally the same build of the port: an older build may use another party protocol version.
- The Old Hunters DLC on all PCs or on none (the port turns it on automatically when the game
  files contain it, so this normally matches).

## Starting a party game in the game (no launcher)

The launcher's Party tab is optional: a party game can be started, joined and continued from the
game's own title screen. The title menu has a **Party** item right after *New Game* (the same page
as *System → Party*). Rows are pressed with left/right; the ones that restart the game need a
second press.

**The first session (fresh characters for everyone):**

- **Host**: *Party → New party game*. The game restarts by itself (a few seconds) into the party
  game: the party saves (`user\savedata_party`), the online title, hosting. Choose **New Game** and
  make your hunter. The party code is under *System → Party → Copy party code* (and in
  `user\party_code.txt`); send it to your friends. It stays the same in later sessions and
  launches until the host makes a new one (*System → Party mode → New party code*, or **New code** in
  the launcher's Party tab while the game is closed); setting or removing a password also gives a
  new code once.
- **Friends**: *Party → Host address* (type the host's party code, or `address:port`; **Ctrl+V**
  pastes; Enter confirms), *Password* if the host set one (the host sets it on the same page before
  *New party game*), then *Join party game*. The game restarts, joined to the host: choose
  **New Game** and make your hunter. A plain `address:port` (instead of the code) works only when
  the host uses a password.
- Everyone plays the prologue on their own; once ready, the party gathers by itself (below).

**Later sessions:** host and friends alike: *Party → Continue party game*. The game restarts, loads
your party character by itself and hosts or joins (your last role) with the saved address and
password; the summons follow automatically.

**In the world** (party game), *System → Party* also has **Host party** / **Join party** (switch
role: a quick restart, your character continues) and **Single player** (back to the
single-player game and saves). With a single-player character loaded there are no party actions:
party games always start from the title screen, so co-op can never write into a single-player save.

The title's *Continue* / *Load Game* / *New Game* are always the single-player game. The settings
are kept in `user\party.json`: your party name, last role, the host address, the password (stored
protected with Windows DPAPI: only your Windows user on this PC can read it back), whether you
have a party character. Details: [party/ingame_flow.md](party/ingame_flow.md).

## Hosting

1. Start the launcher (`launch_gui.bat`) and open the **Party** tab.
2. **Mode: Host a party.** Enter **Your name** (1-16 letters, digits, `_` or `-`; the others see
   it in the game).
3. Pick **Max players** (3 is the game's normal co-op size; 4 is experimental).
4. Leave **Automatic summoning** and **Seamless party rules** on (*Separate party saves* is always on).
5. Optional: a **Password** (every player must then enter the same one).
6. Click **Launch Bloodborne**. Party play always uses its own saves; the first time the launcher
   offers to copy your single-player save into the party save folder (or start with an empty
   one for a new character).
7. Once the game runs, your two **party codes** appear (the launcher log shows them too):
   **Internet code** for friends elsewhere and **LAN code** for players on your own network.
   Click **Copy** next to one and send it (chat, mail ...).
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

In the game, the game's own menu has two party pages under **System** (on the title
screen and in the pause menu, after *Environment* (Screen/Sound)): **Party** (the state and your role, *Copy party
code* to the clipboard, *Rejoin* / *Summon again*, *Leave the party*, plus the party flow's
rows to start or join a party game) and **Party members** (each hunter with their state and ping and
the last event; the join / leave notices switch is on *My party: Look & notices*; the host kicks a
hunter by pressing left/right twice on their row). *Leave* also asks for a second press. The
bottom line of each row explains it (the party code is shown there too). The overlay menu
(**Insert**, or **L3 + R3** if enabled) still has the same **Party** tab. Players joining and
leaving are shown as short messages.

**Party HUD.** In party mode a small panel in the bottom left corner lists your companions: name,
a thin HP bar (stamina under it) and, while they are on the same map, how far away they are and an
arrow pointing their way (relative to the camera). Companions who are not in your world (at home,
loading, in their prologue, disconnected) are greyed and say where they are. Companions in your
world and in view also get a small name tag with their HP above their head. The numbers come from
the game itself when the companion stands in your world, otherwise from their own game (sent about
4 times a second through the host). Switch it in the game (**System > My party: Display > Party HUD**),
the overlay's Party tab or the launcher's My Party tab (bbport.ini `party_hud`, default on; never shown
in single-player). The panel stays clear of the game's HUD and stacks above the performance HUD
when that sits in the bottom left; join / leave notices appear above the panel.

## The party code

A code looks like `BBP3-NJR0-E43N-WWN9-2C2P` (Internet code of a host with a password) or `BBP3-PAJY-EAMH-7TD` (LAN
code). It holds the host's address, the port (only when it is not 9307) and a secret; the secret is
what lets a guest in, so share the code only with the people you play with. Letters are not
case-sensitive, I/L count as 1 and O as 0, and a checksum catches a mistyped character. The LAN
code carries only the last number of the host's address: the guest takes the first three from its
own address (same home network), or searches the LAN when it has none.

| Code (port 9307) | With a password | Without |
|---|---|---|
| Internet | 16 characters after `BBP3-`, e.g. `BBP3-NJR0-E43N-WWN9-2C2P` | 19, e.g. `BBP3-HJR0-E43N-WWN9-2F28-VCA` |
| LAN | 11, e.g. `BBP3-PAJY-EAMH-7TD` | 14 |

Another port adds 3 characters. Older `BBP2-...` and `BBP1-...` codes still work.

**Why a password makes the code shorter.** Without a password the code's secret (48 bits) is the
only thing that keeps strangers out, so it stays that long: guessing it would take about 2^48
tries. With a password the password does that job (a guest who knows the password and the host's
address gets in even without the code), so the code carries only a 32-bit secret. The party is
then exactly as safe as its password: pick one that is not easy to guess. The host's code changes
once when a password is set or removed. Details: `gpu/shim/party/party_code.h`. The host has two codes (`user\party_code.txt`, the launcher, *System → Party*
and the overlay's Party tab):

| Code | Holds | Use it for |
|---|---|---|
| **Internet code** | Your public address (STUN or the router's UPnP) | Friends elsewhere |
| **LAN code** | Your address on your home network | Players on the same network (same router) |

Most home routers cannot reach their own public address from inside (no "hairpin NAT"). A guest on
your network who uses the Internet code therefore searches the LAN when the public address does not
answer within 1.5 s (or its own public address is yours): it broadcasts a small probe on the party
port, your game answers with its LAN address, and the guest joins that (log: `LAN fallback: found
host at ...`). This adds about 2 s. The probe carries only a hash made from the code's secret, never
the secret; the host answers only probes for its own party, at most 5 a second.
`BB_PARTY_LAN_DISCOVERY=0` turns it off. With `host:port` it works only when a password is set.

The code's secret is new for each hosting session, except when the game restarts after a crash or
the party is started again from the title screen's *Party* page. A guest using a code without a
password ignores a password saved in its launcher or *Party* page (log: `code needs no password;
saved password ignored`).

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
- **No UPnP and no port forward: guest codes.** Each guest's overlay *Party* page shows *Your
  guest code* (`BBG1-XXXX-XXXX-XXXX`, its public UDP address; also `user/party_guest_code.txt`).
  The guest copies it and sends it to the host; the host copies it and presses *Add a guest code
  (paste)* (or sets `BB_PARTY_GUEST_CODES`). The host then punches a path through its router
  towards that guest, and the party link switches to UDP when TCP gets no answer. Works with most
  home routers; not with symmetric NAT (see next point). Details:
  [party/network_robustness.md](party/network_robustness.md) §4.
  Since 0.6.1 this can happen **by itself**, opt-in: with *Find each other through the public
  DHT* on (launcher *Party* tab or *System > Party network*; **both** host and guest must turn it on;
  `BB_PARTY_RENDEZVOUS=1`, bbport.ini `party_rendezvous=1`), a guest that cannot reach the host
  leaves its address in the public BitTorrent DHT under a hash only the party can compute, and the
  host finds it there and opens the way (no guest code to pass on). It is **off by default**: DHT
  nodes see your IP address, as with any torrent client. Off, the *Network* line and the guest's
  failure message say "automatic punching is off: send the host your guest code, or turn on the
  DHT option on both PCs".
- **Which NAT do I have?** The *Party* page's *Network* line (and the log's `NAT:` line) says it:
  a *cone* NAT can be punched; a *symmetric* NAT (some mobile / carrier connections) cannot, and
  the line tells you what to do instead.
- **Carrier-grade NAT / no port forwarding possible**: use a VPN that puts all players in one
  virtual network, such as Tailscale or ZeroTier. The host shares its VPN address as
  `address:9307` (or sets it as *Public address*, so the code contains it).
- **Hosting over a VPN (0.7.5).** With Radmin VPN, Hamachi, ZeroTier or Tailscale running, the
  game finds the VPN adapter by itself (setting *Host over a VPN*: **Auto**, **Off**, or an adapter
  picked by name; launcher *Party* tab, *System > Party network*, bbport.ini `party_vpn`,
  `BB_PARTY_VPN`; the host's choice, read when the game starts). The party code's LAN code then holds
  your VPN address (Radmin 26.x, Hamachi 25.x, Tailscale 100.64/10, ZeroTier whatever its network
  assigns); the Internet code is unchanged. The *Party* tab shows *Hosting over Radmin VPN:
  26.x.y.z* and a *VPN address* line with **Copy** (always `address:port`, your party port). A friend
  in the same VPN pastes that into **Party code**; a bare address without `:port` gets the
  party port (9307) added. Commercial VPNs (NordVPN, ...) are never picked in Auto, only by name.
  `BB_PARTY_LOCAL_IP` still overrides everything.
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
6. **Bosses.** Anyone may walk through a boss fog first (a guest's walk-through starts the fight for everyone); a first fight with an intro cutscene still starts with the host.

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
- **NPC quests** are shared (on by default, My Party tab: *Also NPC quests*): the first choice
  anyone makes - host or guest, guests can talk to NPCs in the host's world - applies to the
  host's world and to every player's save. Gifts an NPC gives in dialogue go to whoever talked.
- **Chalice Dungeons** are played together: guests join the host's dungeon (see the README's party section). Checked live with the fixed Pthumeru dungeon (layers 1-2); Root (randomly generated) and glyph dungeons use the host's dungeon id too but have not been checked live yet.
- World treasure a guest picks up while in the host's world does not exist for guests in the
  normal game; the host's pickups are handed to guests through the item sharing above.
  Enemy drops are per player: every player's game rolls each enemy's drop and keeps the whole of
  its own roll, guests included. World treasure is per player too (on by default,
  `BB_PARTY_GUEST_PICKUP=0` turns it off): every player picks up their own copy into their own
  save, and the host's treasure pickups are not handed to the guests.

## Settings (Party Rules and My Party tabs)

The launcher's **Party** tab is for hosting, joining and the connection. The party's settings
are on two more tabs, and on the same pages of the game's *System* menu:

- **Party Rules** (*System > Party rules: ...*): what the **host** decides for the whole party -
  enemies stay dead, gathering, NPCs as in the host's world, covenants per player, travel votes,
  NPC hunters fighting everyone, friendly fire, shared lamps, the host's deaths, rot. They apply
  when you host; as a guest you follow the host's (the game shows them read-only).
- **My Party** (*System > My party: ...*): what **each player** chooses for itself - the table
  below, plus what you see on your screen (party HUD, name tags, pings, glow, notices).

The defaults are the recommended settings; hover any option in the launcher for details.
Settings marked *guest* only matter on the PC of a player while they are a guest; *both* need
to be on for the host and the guest.

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
| Also NPC quests | on | *Guest*: NPC quest steps too (shared NPC quest choices). |
| Share cutscenes and endings | on | *Both*: cutscenes, time of day and endings. |
| Play them live with the host | on | *Guest*: watch with the host while in the host's world. |
| Back up every (min) | 10 | Save backup period during a party session (0 = only at start). |
| Backups to keep | 10 | Number of backups kept. |
| Restart the game after a crash | on | Restart, continue and rejoin automatically. |

Party tab options: **Automatic summoning** (off: ring the bells yourself), **Seamless party
rules** (off: the normal game's co-op rules; the host's setting counts), **Separate party
saves** (always on), **Max players**, connection settings. The environment variables behind each option are
listed in [party/DEBUG_SWITCHES.md](party/DEBUG_SWITCHES.md).

## Saves and backups

- **Party saves, always**: every party session (launcher or in-game) plays from
  `user\savedata_party`; your single-player save in `user\savedata` is never written by it, so the
  party's progress and item sync can never reach a single-player character. The launcher offers
  to copy your single-player save the first time, and the **Copy single-player save to party
  save** button does it any time (an existing party save is backed up first). The old *shared*
  choice (`BB_PARTY_SAVE=shared`) is gone.
- Progress the host shares is written into **each guest's own save**, so after a session your
  character's world has the bosses, lamps and shortcuts of the party's progress.
- **Backups**: during every party session the game copies the save folder to
  `user\save_backups\savedata_party\` at the start, after a crash restart and
  every 10 minutes. The newest 10 are kept; a copy identical to the last one is skipped. The game
  checks the save at every start (*"Saves: check OK"* in the log); a backup that fails the check
  is named `...-suspect` and never pushes good backups out.
- **Undo last party session** (0.6.2, launcher *Party* tab): right before every party session
  (host or guest; not a crash restart, which continues the same session) the game copies
  `user\savedata` and `user\savedata_party` to
  `user\save_backups\party_sessions\<date>-<time>-host` (or `-guest`); the newest 5 are kept
  (`BB_SAVE_SESSION_KEEP`). The button shows what it will replace (per folder, per character: name,
  level, NG cycle, play time now and then), copies your current saves to a `...-before-undo`
  snapshot (pressing the button again brings them back) and restores the snapshot byte for byte.
  After a party session (when the game closes, or at the next launcher start) the launcher checks
  the party save: every character loads, its NG cycle only changes when an ending was played, no
  boss kill or lit lamp from before the session is gone, level and play time did not go down, no
  character vanished. On a problem it says what and offers the undo. Details:
  [party/save_safety.md](party/save_safety.md).
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
  most 5 restarts in 10 minutes (My Party tab: *Restart the game after a crash*).
- **The host's game crashes**: guests return to their own worlds and keep trying to reconnect.
  The host's game restarts with the **same party code** and the same member list, and the guests
  are summoned again.
- **Connection lost**: the same as a host crash for the guest; it reconnects automatically
  (after 1, 2, 4, 8 seconds, then every 8 seconds).
- Progress made while someone was away is caught up when they are back.
- *System → Party* (or the overlay's Party tab) has **Rejoin** (try again now) and **Leave** (stop;
  the game no longer reconnects). A kicked player cannot rejoin that session.

## Four players

**Max players 4** (host + 3 guests) is experimental: the game was made for at most 3 players.
The party patches the game to allow a third helper and to scale boss HP for it. **Every player
must set Max players to 4**, the host and everyone joining; a different value is refused with
*"party rules differ"*. With 4 players there is no free slot for an invader. Four players
connecting has been checked (on one PC); a third helper actually summoned into a world has not been tested yet.

## Troubleshooting

The launcher log (and `launcher.log`) shows the game's `Party:` lines. *System → Party* in the
game's menu (and the overlay's Party tab) shows the state and the last error. When the host
refuses a guest (wrong or missing password, old code, full party, different version), the guest
gets one message box saying why and what to do, and its game stays online alone in its own world
(no *Failed to acquire Bloodborne server information* box). Fix the cause, then *System → Party →
Rejoin the host*: it sends the password currently set in *System → Party → Password* (when you
changed it there; otherwise the one you started with). A changed party code or address still
needs *Join party* (restart).

| Message / symptom | Cause | Fix |
|---|---|---|
| *version mismatch with the host: eboot.bin differs* | Another game version or dump | Everyone needs the 1.09 `eboot.bin`. |
| *... gameplay patches differ (host only: X; yours only: Y)* | Different XML patches that change gameplay | Enable the same patches (Game tab, *All XML Patches...*). Cosmetic patches do not count. |
| *... gameplay mods differ* | Different mods in `mods\` | Use the same gameplay mods, or turn mods off on all PCs. |
| *... party rules differ (host max 4 players ...; yours max 3 ...)* | Different *Max players* (or 4-player switches) | Set the same Max players everywhere. |
| *different party protocol version ...: update the port* | Different builds of the port | Everyone updates to the same build. |
| *Old party code: ask the host for the current one* | The host made a new code (*New party code*, or it set / removed a password), or the code is from an older build (every launch made a new code then) | Ask the host for the current code. The host's code otherwise stays the same from one launch to the next. |
| *UDP to the guest is blocked* / *UDP to the host is blocked* (notice) | The guest's game has no public UDP address: the STUN on the host's party port gave it no answer, so the host knows only its private LAN address | Nothing at first: neither side rings until the guest's game gets an answer (it retries every 40 s) and the summon then starts by itself. If it stays blocked, restart the guest's game, or use a VPN (Tailscale) / let the other player host. |
| *This party needs a password* (launcher hint, or in the game) | The host set a password; yours is empty | Enter the host's password (launcher *Password*, or *Party → Password* on the title screen). |
| *Wrong party password (or an old party code ...)* | Your password differs from the host's | Same Password on all sides; copy the current code again. |
| *Wrong or missing party password* | Joined by `host:port` and the password differs | Same Password on all sides. |
| *Failed to acquire Bloodborne server information* when joining a host on the same network with the Internet code | The router cannot loop back to its own public address | Update the port (the guest now finds the host on the LAN by itself), or use the host's **LAN code**. |
| *the name X is already in the party* / *invalid name* | Duplicate or invalid name | Pick another name (1-16 of A-Z a-z 0-9 _ -). |
| *the party is full* | Max players reached | Raise Max players (on everyone's PC) or wait. |
| *kicked by the host* | The host kicked you | Ask the host. |
| Guest stays at *Connecting* | The host's port is not reachable | Host: check UPnP in the log, forward UDP+TCP 9307, allow the firewall prompt; or use the LAN code / a VPN. |
| LAN: connected, the bell join times out (~48 s) again and again | 0.5.7 and older: the guest sent game traffic to the host's public address, which the router does not loop back | Update. Workaround for 0.5.7: the host sets *STUN server* to `off` and unchecks *UPnP* (or sets *Public address* to its LAN IP) for LAN play. |
| Connected, but never summoned | The guest is still in the prologue, in a menu, or the host is in a loading screen / boss fight | Finish the prologue; wait for the next bell (every 30 s); use *Rejoin* (*System → Party*). |
| No code under *Internet code* / *LAN code* | The game has not started hosting yet | Wait until the game is at the title screen; check the log for `Party:` errors. |
| Save looks wrong after a session | - | Restore a backup (above) and report it with the logs. |

When reporting a problem, include `launcher.log` (and `out\run.log` if present) from every
player, and say who was the host.

## Known limitations

- **2-PC LAN (first real session, 0.5.7):** worked with the host's STUN server `off` and UPnP off.
  The summon took ~63 s from the host opening the party to the cooperator arriving (~20-30 s on
  one PC). Open item.

- **Not tested between two PCs.** Everything so far ran on one PC with several game copies; real
  Internet connections, routers and firewalls have not been tried.
- A full campaign has not been played through as a party. Several parts (guest respawn, open
  world, refills, the progress and item sharing during a real session, endings for guests) are
  built and unit-tested but have not run in a real two-player session yet.
- Travel is "follow and rejoin": guests load separately and are summoned again after the host's
  lamp trips, deaths and area changes. A shared seamless loading screen is not planned for now.
- A first boss fight with an intro cutscene starts with the host; otherwise anyone may enter a boss fog first. In the host's Hunter's Dream a guest can level up at the
  Doll and use the Bath Messengers' shops, the Insight shop, the workbench and storage on its own
  character; lamps and headstones stay host-only for now (experimental opt-in `BB_PARTY_GUEST_LAMPS=1`).
- NPC quests are not shared by default; dialogue gifts from NPCs are not handled.
- Chalice Dungeons: checked live only with the fixed Pthumeru dungeon; Root and glyph dungeons are expected to work but are untested.
- 4 players: experimental, no slot for invaders.
- An item a guest picks up in the host's world just before a crash may be given twice.
- The online play of the original game (PSN messages, bloodstains, invasions from strangers) is
  not available; the party only connects the players who have the code.

For developers: the design and status are in [PARTY_COOP_PLAN.md](PARTY_COOP_PLAN.md) and
[party/](party); test and debug switches in [party/DEBUG_SWITCHES.md](party/DEBUG_SWITCHES.md).
