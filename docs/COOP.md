# Party co-op: full player guide

Back to the [README](../README.md). Setup, hosting and troubleshooting in depth: [PARTY.md](PARTY.md). Progress vs. Elden Ring Seamless Co-op: [party/ERSC_PARITY.md](party/ERSC_PARITY.md).

Play the whole game with friends in one session, from the first lamp to the end credits.
Modelled on Elden Ring Seamless Co-op, adapted to Bloodborne. **In development:** tested on one
PC (several game copies, simulated internet lag), **between two PCs on a LAN** (summon, combat,
shared kills and doors, per-player pickups) and **over the Internet** in two real sessions between
PCs in different homes:

| Session | Build | What it showed |
|---|---|---|
| Playtest 1: 2.5 h, two campaigns, prologue to Old Yharnam | 0.6.0 | Fixed in 0.6.1 ([docs/party/playtest1_triage.md](party/playtest1_triage.md)) |
| Playtest 2: 2 h, Old Yharnam, the Blood-starved Beast and the Hunter's Dream | 0.6.9 | Joining, travel, deaths and boss kills held up; a boss the guest finished while the host lay dead stayed dead. Its reports are fixed for 0.7.0: guests frozen while the host died, the spectator camera, one-time shop items sold out for guests, gestures and key items not kept by guests, an uncollectable shared drop, guests unable to talk to Gehrman, captions at the workbench and shops, a new party code on every host launch |

Players running bigger parties sent reports and fixes too (Chalice Dungeon entry, VPN joins, 3-4 player
parties: guests drawn with one another's look, pickups animating every other player). A scripted
two-player run from two new characters to the end credits passed, and a three-player travel run
([docs/party/campaign_soak.md](party/campaign_soak.md)).

**Party sections:** [First time? Step by step](#first-time-step-by-step) ·
[Starting a party](#start-from-the-title-screen-recommended) ·
[How it plays](#how-it-plays) · [Deaths](#deaths) · [Progress vs. Seamless Co-op](#progress-vs-seamless-co-op) ·
[Coming next](#coming-next) · [Known issues](#known-issues) ·
[Connecting over the internet](#connecting-over-the-internet) · [Reporting a bug](#reporting-a-co-op-bug)

The host shares a **party code** such as `BBP3-NJR0-E43N-WWN9-2C2P`: it holds the host's address and a secret,
so share it only with the people you play with. With a password the code is shorter (19
characters without one); players on the host's own network can use the shorter **LAN code**
(e.g. `BBP3-PAJY-EAMH-7TD`). Everyone needs the same bbport version and game files. Party play uses its
own save; your single-player save is never touched.

## First time? Step by step

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
3. The same **Password** as the host. **Max players** is the host's: you take it when you join (use *Join selected*
   in *Parties found* and the game also starts with the host's other party options).
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
  On the opt-in Nightwatch VPN a host with a password can also turn on *Nightwatch VPN lobby* (off by default):
  the launcher's *Nightwatch VPN games* list then shows the host's character name and **Join** asks for the password. The host also announces a short label of its mods and gameplay patches (hashes) to the lobby, for clients that show it (Nightwatch) (0.7.9).
  While connected to the Nightwatch VPN the game uses its tunnel address for all party traffic by itself (no
  *party_vpn* setting needed).
  Over the Nightwatch VPN the host's code carries its full tunnel address (0.8.1; the short LAN code only holds the last
  number, which fails when the guest's VPN address is in another 10.8.x range). Old short codes still work, and the game
  also tries the other 10.8.x ranges for them. If a code fails, join from the *Nightwatch VPN games* list or type the host's
  full VPN address. `BB_PARTY_VPN_FULL_CODE=0` brings the short code back.
  Hateful player names (slurs, hate codes, hostile phrases about groups of people) are shown as *Hunter* and a short
  code, in the games lists, the party list, the name tags and the notices (*Hide offensive names*, **on** by default:
  launcher Party tab, or *System > My party: Names*). The Nightwatch VPN lobby does the same for everyone.
  A host with **no password** can be joined by plain address, with no party code, by players on its own LAN or VPN
  (*Open party*, off by default: launcher Party tab checkbox, or *System > Party access*). Anyone on that network can
  join, so use it only on networks you trust; a listed game marked *open* in the lists is joined without any password. A player who joins an open party outside the host's
  level range (`floor(level * 0.8 - 20)` to `floor(level * 1.2 + 20)`; host level 100: 60 to 140) is refused with the range named
  (0.7.9); a party code or password ignores the range.
- **Windows Firewall**: allow `bbport.exe` (*Windows Security → Firewall → Allow an app through firewall*). If the game says the firewall *blocks* it (you cancelled the prompt once), press **Fix firewall issues** in the launcher's party help (one admin prompt: removes the game's inbound Block rules, adds Allow rules for UDP + TCP on the party port), or delete the Block entries by hand (`wf.msc` → Inbound Rules, red block icon).
- More options (guest codes, DHT, IPv6): [Connecting over the internet](#connecting-over-the-internet).

**5. Still not working?** On every PC: launcher → *Advanced / Debug* → **Save support bundle**, and send the
zips with an issue, together with which step each player reached (see [Reporting a co-op bug](#reporting-a-co-op-bug)).
The lines starting with `NAT:`, `UPnP:` and `Party:` in the logs usually tell what went wrong.

After the first session, the title screen's **Party → Continue party game** is all anyone needs:
code and password are remembered, and the host's code stays the same.

## Start from the title screen (recommended)

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

## Start from the launcher

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

Faster loading (the launcher's converted game files, [SETTINGS.md](SETTINGS.md#faster-loading-converted-game-files)) is not part of
the party's version check: players with and without it can play together.

## How it plays

**Joining and leaving**

- **No PSN, no server.** One player hosts; up to 3 guests join with the host's party code
  (4 players; 5 or 6 are experimental and off by default, behind the launcher's *Large parties* switch and *Max players*: an invader takes one of the seats, see docs/party/six_players.md), all in the host's world. Every guest sees every other guest.
- **Fast join.** No bell, beckoner or summon-sign waits: a guest stands beside the host within
  seconds (about 5 s in the same area; a join that stalls restarts by itself). *Continue party
  game*, or joining from another map (the Hunter's Dream, say), takes one loading screen. If the
  host travels while you join, its travel waits until you are in and you travel together.
- **Slow sign-in at the title.** In a party the title waits for the game's online sign-in: the automatic *Continue* counts it as running, and
  a *Continue* you press by hand is held back until it is over (30 s at most), so a slow server no longer leaves the game offline
  for the whole session (where no Chalice summon works) (0.7.9).
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
  not beaten yet waits until the host beats it). A lamp a guest lights is lit whole in every save: it appears in the travel list and can be rested at, also after a restart; a lamp left half-lit by an older save is repaired when your own world loads. The host can turn sharing off (*Shared lamps*).
- **Travel from any lamp to any lamp.** A lamp offers the headstones' list (the Hunter's Dream and
  every lit lamp) and takes you straight there with one loading screen (launcher: "Direct lamp
  travel", on by default).
- **The map shows the party and travels.** With the in-game map on (*System > Map*), **M** shows every party member on your map as a named dot, and a click on a lit lamp travels there like a lamp's own list: the host's trip asks "Travel with the party?", a guest's moves only the guest.
- **Travel together or alone.** Lamps, headstones and the Hunter's Mark ask "Travel with the party?".
  *Yes* moves the whole party with one loading screen; *No* moves only you. The question closes by
  itself after 30 s. A guest's own save keeps its own spot for solo play later. A guest who travels
  alone into a map the host is not in has that map's enemies awake: the guest's own game runs them
  (they used to stand still until the host came).
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
  world resets too; a guest leaving the Hunter's Dream alone by a headstone, or travelling from a map
  (no lamp used), is just a trip (no reset, the others keep playing).
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
  between all players (Root Chalices, which a ritual uses up, stay each player's own). The dungeon's Bath Messengers (the shop) show for every guest, also one with a Chalice Dungeon of their own. The dungeon's
  lamps work with a party (back to the Dream, and from the altar to any lamp you lit). A Cursed dungeon halves every member's maximum health, guests too, whatever the Phantom health cut rule says. A guest who played the host's dungeon takes it home: it shows at the guest's own altar with the lamps and shortcuts the party reached (with Shared lamps on). A guest whose game is offline (no online login) stays in the host's world with the notice "This game is in offline mode" (restart the game and wait at the title until it goes online) instead of being sent home, and follows the host out of the dungeon. A guest who dies
  in the dungeon gets up beside the party; a guest pulled out by a death or a forced trip is summoned
  back in automatically, one who chose to leave returns with *Party > Rejoin* (System > Party rules: Travel).

**Bosses, cutscenes and endings**

- **Anyone can go through a boss fog first**, a first fight included: the fight starts for
  everyone and the others follow through the fog. A guest's first fight plays the intro cutscene on
  every screen. The DLC bosses work the same way. Ebrietas, Lady Maria and Laurence start only with
  what the host does in their room (a hit, walking up to Maria, Laurence's Skull at the altar):
  until then their room is open to everyone, and a guest at that fog is told what starts it. Rom
  starts with anyone's hit. The Celestial Emissary's small emissaries appear on every screen, whoever
  goes through the fog first (0.7.9).
- **Boss music for everyone.** Every player hears a boss's music once the fight is on and they stand in
  the boss area, also in fights they were already inside when they started (Ebrietas, Rom, Laurence,
  Lady Maria, Gehrman, the Moon Presence) (0.8.1).
- **Guests can hit NPCs.** A guest's blows land on the host world's NPCs as the host's do (the friendly
  hunters and the others the host could hurt): they take damage and a killing blow counts for
  everyone. An NPC that turns hostile after a few hits (Alfred) does so on a guest's blows too.
  Nothing is added that the host could not do.
- **The Mad Ones show for guests.** The Insight madness spirits (in Hemwick Charnel Lane, say) appear when a guest with enough Insight
  comes near them at night, as for the host, with the guest's own Insight; they show on every screen (0.7.9).
- **NPC hunters fight the whole party.** Eileen and Henryk at the Tomb of Oedon, Alfred, the
  blood-drunk Eileen, Valtr and the DLC hunters are there for everyone in the host's world:
  anyone can hurt them, and their death and the quest step after it count for every player's own
  save. A hunter that turns on the host turns on the guests too (*NPC hunters fight all*, a host
  setting on *System → Party rules: World*, on by default). Covenant outcomes, such as the host's Alfred
  killing Annalise, stay each player's own. A hunter summoned with the Old Hunter Bell goes into the
  boss arena with the party even when a guest goes through the fog first. Brador and the Fishing Hamlet's hunters
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
  never reach another player's save (in the host's world the covenant leaders still show your own
  state, so you can talk to Valtr and join after the host did). A guest can kneel to Queen Annalise in the host's world and swear the Vileblood oath, in their own save (0.7.9): after her greeting stand in the circle before her throne and press the action button; the Caryll rune is the guest's own, once per player. A covenant
  rune an NPC hands you (Valtr's Impurity, for example) is yours, in your own save. A host setting,
  on by default (*Covenants per player* on *System → Party rules: World*).
- **The host's world is shared live, both ways.** A door, lever or shortcut opened by anyone is
  open for everyone, in the host's world and in each guest's own save; a lift ridden or a ladder
  lowered by anyone is in the same place for everyone, also for a player who was far away. Guests open gates with their
  own keys (e.g. the Cathedral Ward gate with the Hunter Chief Emblem).
- **Bosses share their attention.** A boss in a party fight turns on every player within reach in turn (about every eight seconds), not only the nearest one or the host. The host's rule *Bosses share attention* (*System → My party: Bosses*, the launcher, `party_boss_share`), on by default.
- **Boss drops for guests.** The item a boss drops for the host (Kin Coldblood from Rom, ...) is also given to every guest
  who is there when it dies, once per kill, in their own save (like the Chalice Dungeon boss rewards). The host's rule *Boss item
  drops for guests* (*System → Party rules: Rewards*, on by default); guests see it read-only.
- **Key items are never lost.** A summoned guest sees its own key items in the host's world too
  (the game alone shows a visiting hunter an empty Key Items tab). Key items a guest earns in the
  host's world (Oedon Tomb Key, badges, workshop tools, chalices, umbilical cords) are given at once,
  and checked again when its own world loads and given again if missing; one you already hold is
  never handed out twice, and one you used up (the Gold Pendant, the Tear Stone) never comes back.
  Both are your own switches on *System → My party: Your items* (*Own key items as guest*, *Key items at once*).
  Keys, tools and quest items found lying around (the daughter's ribbons and brooch, the Orphanage Key,
  the Hunter Badges) go to the whole party, so nobody is locked out of a door or a questline.
  The Bath Messenger's badge rows open with your own badges (also one you get while summoned in the host's world: the row opens at once and stays), and the Eye of a Blood-drunk Hunter (the DLC entry)
  comes from the party only once your own world has reached night; Amygdala's grab takes only the player who
  holds the Eye.
- **Chalice glyph list.** With *Chalice glyph list* on (*System -> My party: Chalice*, the launcher's My Party tab, `party_glyph_list`, off by default) the Ritual Altar's *Search by Chalice Glyph* opens with a list of the party's glyphs to pick from (your dungeons, the guests', and `patches/chalice_glyphs.json`) above the code keyboard; pick one and the whole party enters that dungeon together. An unknown code gets a clear notice. See docs/party/chalice.md 11.
- **Community Chalice glyphs (Noxde's list).** With *Community Chalice glyphs (Noxde's list)* on (*System -> My party: Chalice*, the launcher's My Party tab, `chalice_glyphdb`, off by default; works solo and as the party's host) the Ritual Altar's *Search by Chalice Glyph* lists about 200 community glyphs by category (farming, equipment, blood gems, testing) with Noxde's descriptions, above the code keyboard (PageUp / PageDown or L1 / R1 jump ten rows). Pick one, or type its glyph, and the game makes and enters that dungeon its own way, offline; in a party the guests follow like into any dungeon of the host. The launcher downloads the list from Noxde's gist while the setting is on (when missing or a week old) and keeps it in your user folder; without internet the last download is used, and without one nothing changes. Nothing of the list ships with bbport. With no Chalice in the inventory the altar still opens (see docs/party/chalice.md 13). Credit: Noxde (docs/CREDITS.md). See docs/party/chalice.md 13.
- **Ritual Altar as a guest.** With *Ritual Altar as a guest* on (*System -> My party: Chalice*, the launcher's My Party tab, `party_guest_altar`, off by default, your own setting) the Ritual Altar in the host's Hunter's Dream answers you while you are a guest: conduct a Chalice ritual with your own chalice and materials. The dungeon is made in your own save (a free slot), the host's altar offers it as *Enter the party's dungeon*, and you follow the host in.
- **Chalice layer travel.** With the host's rule *Chalice layer travel* (*System -> Party rules: PvP & lamps*, the launcher's Party Rules tab, `party_layer_travel`, off by default) the host's Chalice Dungeon lamp lists the dungeon's other lit lamps (the layers) next to the Hunter's Dream, and the host's choice takes the party along (a guest with a dungeon of its own is summoned back in next to the host; a guest without one is carried too, by the summon or by the same lamp warp on its own game). Not for guests staying in their own world. See docs/party/chalice_layers.md.
- **Guests can gather the party.** With the host's rule *Guests can gather* (*System -> Party rules: Travel*, the launcher's Party Rules tab, `party_guest_gather`, off by default) a guest sees *System -> Party -> Gather the party*: it asks the host and the other guests to come to it (each is asked and may say no; an in-world warp beside the guest, only players in the same map, never a loading screen). Same refusals and 30 s pause as the host's gather; not while the guest stays in its own world. See docs/party/guest_gather.md.
- **Any level can join my open game.** An open game (no password, a plain address join) is held to the game's co-op level range (host 100: 60 to 140). With *Any level can join* on (*System -> Party access*, the launcher's Party tab, `party_open_any_level`, env `BB_PARTY_OPEN_ANY_LEVEL`, off by default, the host's own setting, live) every level joins and none is removed later. Password and party-code joins never had a range; invaders keep theirs.
- **Room for a rejoining guest.** A guest that restarts its game and rejoins from the title (Fast rejoin off) used to find the host's room already closed, because the game closes an empty room after about 5 s. The host now holds an empty room 10 s more while that guest joins (*System -> My party: Your save*, the launcher's My Party tab, `party_title_room_hold`, env `BB_PARTY_TITLE_ROOM_HOLD`, on by default).
- **Invasions into open public games.** The host decides: *Allow invaders* (*System -> Party access*, the launcher's Party tab,
  `party_invaders`, off by default, live) lets other players join a game from the launcher's lists as a hostile phantom
  (**Invade**, next to Join, no password): you and your guests are told, fight it, and it goes home when it dies or when the host
  dies. An invader takes one loading screen and a few seconds to arrive, also one that starts in another map (the Hunter's
  Dream, say) than the host (0.7.9), and is put away from you at a lamp the host lit, 30 to 250 m off in the same map, like a vanilla
  invader (beside the host where there is none, e.g. in the Hunter's Dream or a Chalice Dungeon) (0.7.9). Only players within
  the host's level range can invade, `floor(level * 0.8 - 20)` to `floor(level * 1.1 + 10)` (host level 100: 60 to 120); the refused
  player is told the range (0.7.9). One invader at a time, or two with the host's *Max invaders* (*System -> Party access*, the
  launcher's Party tab, `party_max_invaders`, 1 or 2, default 1, live) (0.7.9); never in a boss
  fight, a loading screen or a cutscene (such a refusal is not final: the invader waits, the party status says
  "Waiting for the host's world", and it asks again by itself; a final refusal reads "Can't invade: ...") (0.8.1), never into a full game (every seat taken: 4 players, 5 or 6 with *Large parties*); you can block the invaders
  for the session (*System -> Party -> Block the invader*). An invasion that cannot get into the host's world (no world snapshot within 45 s) ends with a notice and the invader goes home; it is never turned into a co-op phantom (0.7.9). The invader is drawn with the game's red phantom glow, and the game's own
  rewards are paid on each player's own game: an invader that defeats the host gets Blood Echoes and +1 Insight, the host and
  guests get Blood Echoes for defeating it. The host's *Full health in PvP* rule also covers an invasion: with it on (the
  default) the invader is not cut to 70 % maximum health (0.7.9). See docs/party/invasions.md.
- **Stay in my world.** As a guest you can pause the automatic joining without leaving the party, e.g. for your own
  Chalice ritual or shopping: *System -> Party -> Stay in my world* (a guest's row; the launcher's My Party tab,
  `party_stay`, off by default, live). You ring no bell, are not gathered and not asked to vote; the host does not wait
  for you and sees *Staying home*. Turn it off and you join again at the next opportunity. See docs/party/stay_home.md.
- **Splitscreen mode.** For several game windows on one screen (the Nucleus Co-op splitscreen mod): one switch,
  *Splitscreen mode* (*System -> My party: Splitscreen*, the launcher's My Party tab, `party_splitscreen`, off by
  default, your own setting) makes the party HUD show only the compass (a direction arrow and the distance per
  companion), hides the name tags above the companions and cuts party messages to errors. *Party messages* (same
  page, `party_messages`: All / Errors only / Off) works on its own too. See docs/party/splitscreen.md.
- **Give items to a party member.** *Give items to the party* (*System -> My party: Giving*, the launcher's My Party
  tab, `party_give_items`, off by default, your own setting and your consent to receive) adds *Give* to the command menu of an
  item in the game's own inventory (Use / Discard / Give): pick a hunter, then an amount. Consumables and materials (the blood
  stones too), never key, unique or story items; nothing moves while loading, in a cutscene, a boss fight or a travel; the giver loses the items only
  when the receiver has them, and a broken link puts them back. See docs/party/give_items.md.
- **Give weapons and armor to a party member (0.7.9).** *Give weapons and armor* (*System -> My party: Giving*, the launcher's My
  Party tab, `party_give_gear`, off by default, your own setting and your consent to receive; separate from *Give items to the
  party*) adds *Give* to a weapon's or armor's command menu in the inventory: pick a hunter. A weapon keeps its upgrade level and its
  blood gems, armor moves as it is. Not worn gear, and not the gear the party shares anyway (story and unique weapons, Gascoigne's
  set ...); the receiver needs a free inventory entry. The giver loses the gear only when the receiver has it, and a broken link
  puts it back with its gems. See docs/party/give_items.md section 5.
- **What is shared, what is yours.** Whoever picks it up or lands the kill, a shared item reaches every member
  of the party once - also a member who joins later or reconnects - and a short notice on every screen says so
  ("Living String: shared with the party"; *Shared item notices* on *System -> My party: Your items*, the launcher's My
  Party tab, `party_item_notices`, on by default, shown while the party notices are on, at most one every few
  seconds, never for your own pickups).

  | Shared with the whole party | Stays with the player who found it |
  |---|---|
  | Key items (keys, badges, workshop tools, chalices, umbilical cords, the daughter's ribbons and brooch) | Ordinary items: Blood Vials, Quicksilver Bullets, materials, Pebbles, blood gems |
  | Unique items (hunter tools such as Executioner's Gloves, Choir Bell, Beast Roar) | Armor and rings found in the world |
  | Weapons, from corpses, NPC gifts and bosses | Messenger gifts, covenant gems and runes, repeatable NPC gifts |
  | Caryll runes, the world bosses' reward runes | Enemy drops (each player rolls their own copy) |
  | Boss rewards the party earned together (Gold Pendant, badges, the Living String from the Brain of Mensis) | |

  A boss killed by a guest pays the host as well. The list of lots is derived from your own game data at load.
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
  *Share look changes* (System > My party > Names, off by default) also sends a look changed any other way
  (the editor opened without its frame, for example) once it has held for 1.5 s.

**Map markers**

- *Shared map markers* (System > My party > Chat & markers, launcher Party tab, off by default; needs the in-game map): with
  the map open, a right click (P with the pointer on it, R3 on a pad) puts your marker on that spot; every member with
  the setting on sees it on their map as a pin in your party colour with your name. One per player (a new one
  replaces it; a click on your own pin takes it down; it goes when you leave).

**Optional switches (off by default unless marked *On by default*)**

Each is a System menu row, a launcher setting and a `bbport.ini` key; a switch the host decides is marked *host*.

| Switch | Where | What it does |
|---|---|---|
| *Share look changes* | System > My party > Names, launcher Party tab, `party_look_sync` | A look changed in any editor reaches the others without a reload |
| *Shared map markers* | System > My party > Chat & markers, launcher Party tab, `party_map_marks` | A right click / P / R3 on the open map sets your marker for every member (needs the in-game map) |
| *Rot: stamina recovery* (*Death may slow stamina recovery*), *host* | System > Party rules: Rot, launcher Party Rules tab, `party_rot_extra` | The death debuff may also take -15% of the stamina you get back per stack |
| *Load with the host* | System > My party: Loading, launcher My Party tab, `party_parallel_follow` | On the host's party travel to a lamp (the Dream, a headstone, a Hunter's Mark) your game loads the same lamp at once, in parallel with the host, behind one loading screen, and puts you beside the host once it is in (before: your load started only after the host's). A death, a boss fight or a Chalice Dungeon keeps the old way. *On by default* (0.8.1) |
| *Hold the screen until smooth* | System > My party: Loading, launcher My Party tab, `party_settled_drop` | In a party the loading screen stays up after a load until the frames have run under 50 ms for 1 s without a break (at most 3 s more), so the first heavy frames after a load are not seen as a frozen world |
| *Arrive beside the host* | System > My party: Loading, launcher My Party tab, `party_load_beside` | When your game follows the host's party travel, its load puts you beside the host itself: no jump to the host's side right after the loading screen; the position is written into the load itself (not a warp after it), so the screen no longer waits for a move. If the load cannot take it (a spot far from the lamp) the screen is held and the move hidden, as before. *On by default* (0.8.1) |
| *Rejoin in place* | System > My party: Loading, launcher My Party tab, `party_quick_rejoin` | *Party > Rejoin* pressed while you are in the host's world reloads you beside the host with one loading screen, instead of *Leave* (a load into your own world) and *Rejoin* (a second load); a member that has not reappeared is asked for its record with the game's own request. *On by default* (0.8.1) |
| *Allow direct connection (lower ping, party members see your IP)* | System > My party: Loading, launcher My Party tab, `party_nw_direct` | Over the Nightwatch VPN only: when you and another member both allow it, the game tests a direct connection for 3 s and uses it only if it is clearly faster than the tunnel (once, at a loading screen; back to the tunnel for the session if it drops). The other member sees your public IP. Off: everything stays on the tunnel |
| *Large parties* (experimental) | Launcher Party tab, `party_large` | *Max players* may be set to 5 or 6; six games on one PC need about 24 GB of memory; 6-player travel and boss fights are not yet tested |
| *Allow invaders* (*host*), *Party chat*, *Coloured name tags*, *Auto-hide HUD* | described in this guide | Earlier optional features, also off by default |

**Rules, difficulty and HUD**

- **Death can have a cost (optional).** With "rot" on, each death stacks a penalty, picked at random,
  up to 5: −10% max HP, −10% max stamina, +10% stamina use, −15% rally or −10 Item Discovery. Using a
  lamp (even just opening its travel menu) or levelling up clears it. Killing a boss clears it for the
  whole party; when the enemy that killed you dies (whoever kills it), or when you pick up your Blood
  Echoes, the rot from that death goes. Each save slot keeps its own. Your current rot and what it
  does ("Rot 2/5: max HP -20 %") show on the *Party* page and under the party HUD, and a notice says
  when some is cleansed. Off by default; the host turns it and each of the three cleanses on or off on
  the *Party rules: Rot* page (or in the launcher's *Party Rules* tab). A fourth host switch there,
  *Rot: stamina recovery* (off by default), adds a sixth penalty to the random pick: −15% of the stamina
  you get back, per stack, so the bar refills slower.
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
- **Coloured name tags** (off by default, your own setting: *System → My party: Display → Coloured name tags* or the launcher's *My Party* tab): the name above each companion in that hunter's party colour (the colour of the party panel and the chat), with a dark outline.
- **Auto-hide HUD** (off by default, your own setting, solo too: *System → Game HUD* or the launcher's game settings): the game's HP and stamina bars, item slots and echoes counter fade out while nothing happens and return on damage, stamina use, a fight or lock-on, an equipment change or a menu, like Elden Ring's *HUD: Auto*. Details: docs/auto_hud.md.
- **Party text chat** (off by default, your own setting: *System → My party: Chat & markers → Party chat* or the launcher's *My Party* tab). Press
  `T` (change it with `party_chat_key=<key name>` in `bbport.ini`), type a line (up to 120 characters) and press Enter; on a pad hold
  the touchpad (Xbox pads: `Back`) and press `L1` to pick a short line (*Help!*, *Follow me*, *Wait*, *Boss here*, *Thanks*,
  *Going to the Dream*) with the d-pad and Cross. Lines of every member (also from one guest to another) show for a few seconds at the
  left of the screen, each name in that member's party colour. The host's *Party chat* switch decides whether the party has chat;
  *Show others' chat* mutes the other hunters for you only. With *Hide offensive names* on (the default) offensive words in
  other players' lines are replaced by asterisks. Chat text is never written to the logs. Details: docs/party/chat.md.
- **Friendly fire / PvP** (host's switch, off by default): party members can hurt each other, both
  ways, like invaders (*System → Party rules: PvP & lamps*). A party kill is a normal party death; the killer
  gets no Echoes or Insight. While it is on, hunters who joined keep their full maximum health for the duel
  (*Full health in PvP*, on by default, same page; the 30 % phantom health cut returns when friendly fire goes
  off, each hunter's health kept at the same fraction)). Only the player hunters can hurt each other: a summoned NPC hunter stays the friend of everyone.
- **Connection warnings.** When the Windows Firewall, the router or a wrong address keeps your party from working (nobody reaches a host without an open port, a guest's game is not heard, a guest cannot reach the host), the game shows a notice with the shortest fix and logs it; at most once per cause per session (*System > Party members > Connection warnings*, on by default).
- **Party HUD.** Party members and events in a small HUD; each companion's ping in ms after the name (green / yellow / red; *My party: Look & notices > Ping in the party list*, on by default).
- **Every party setting in the game's own menu, in two groups.** *Party rules* holds what the host
  decides for the whole party: *Party rules: World* (enemies the party killed stay dead, gathering the
  party, NPCs as in the host's world, covenants, voting on travel, NPC hunters), *PvP & lamps*,
  *Deaths* (getting up and respawning, also for guests), *Travel* (lamps and Hunter's Mark for guests,
  open world, Chalice Dungeons, cutscenes with the host), *Rewards* (guests' echoes and drops, boss
  Insight, boss item drops for guests), *Phantoms*, *Rot* and *Enemies* (scaling); guests see the host's values read-only, so the
  whole party plays one way. *My party* holds what each player chooses for itself: *Your save* (what
  the party brings into your own character), *Your items* (item rewards, your key items as a guest), *Giving* (giving items, weapons and armor),
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
- **No freezes from a slow log reader.** The game writes its log through a background thread (launcher *Advanced / Debug*: *Log output
  through a background thread*, `log_async`, on by default), so a launcher or an antivirus that stops reading for a moment no longer freezes the
  game for seconds (0.7.9).
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
- **Party character offline.** The *Party* tab's **Copy party character to offline...** copies a party
  character into a single-player slot and cleans it for playing alone (the summoned-guest body type and the
  host's Chalice Dungeon left in the network slot go; your own dungeons and progress stay). The launcher
  lists what it cleaned; the party save is not changed and your single-player saves are backed up first.

## Deaths

| Who dies | What happens |
|---|---|
| A guest | Gets up again at its own lamp, with no loading screen when that lamp is in the loaded area (a lamp in another area: a short respawn loading screen); its Blood Echoes wait in a bloodstain where it fell (an on-screen marker for it: *System → Pings → Bloodstain marker*, off by default); the others keep playing. **A fall** (off a ledge, or out of the world): it gets up at its own lamp, or at the host's lamp when its own is in another area, with no loading screen when that lamp is in the area, never in the Hunter's Dream; its Blood Echoes wait on the last solid ground before the fall, never under the map |
| The host | Gets up again at its lamp with no loading screen while the guests play on (a lamp in another area: the host alone loads it); the host's echoes stay in a bloodstain. After a fall (off a ledge or out of the world) it also gets up at its lamp (no loading screen when the lamp is in the area, never in the Hunter's Dream); its echoes wait on the last solid ground before the fall. A death that takes the host somewhere else by the story (a Snatcher's kidnap to the Hypogean Gaol) takes the whole party there instead: one loading screen for everyone, the guests wake beside the host. In a boss fight the guests kill: the host gets up in the arena (see below). `BB_PARTY_B2_HOST_REVIVE=0`: everyone gets one respawn loading screen at the host's lamp and the world resets, as in the original game |
| Anyone, during a boss fight | The boss turns to the hunters still standing (it no longer stands over a dead hunter). Watches a living party member fight, with the camera following them as in normal play (the right stick turns it; press Cross to watch the next one) until the boss dies, everyone dies, or someone travels. When the host is down and the last hunter still fighting leaves the fight (Hunter's Mark, a lamp, its death, *Leave* or a lost connection), that counts as everyone dying: the whole party gets the respawn loading screen at the host's lamp. When the boss is killed, everyone who died in the fight - the host and the guests alike, also one still in its death animation when the boss fell - gets up right there in the arena, where they fell (after a fall: on the last solid ground before it, where their bloodstain is; or beside a living hunter), never under the map, no load, their echoes still in their bloodstain. the host's *System → Party rules: Deaths → Up after boss kill* off: the host's death takes the party's respawn load and a guest gets up at the host's lamp instead |
| Everyone | Everyone respawns together at the host's lamp with one loading screen; the session stays. Also when the last hunter falls long after the others (the ones already down wait for it, however long the fight goes on); its echoes wait in a bloodstain where it fell |

A death (host or guest) brings no enemies back; a rest at a lamp (anyone's) or a party wipe resets them.
An enemy killed by anyone in the party is dead on every game, also one of the next area seen across a
border; walking away from it and coming back does not bring it back (single player does, for the next
area's enemies).

## Progress vs. Seamless Co-op


Parity with Elden Ring Seamless Co-op, measured row by row in [ERSC_PARITY.md](party/ERSC_PARITY.md).
Two numbers: **plan done** is how much of the plan is built and seen working at least once;
**ERSC parity (evidence-weighted)** caps every row checked on one PC only at 92%, at 94% when it also ran
over the real Internet from one PC (through a VPN), until it has been checked on two real PCs over the
Internet (three real Internet sessions so far: 0.6.0, 0.6.9 and a 5-hour 4-player session on 0.7.7 on 2026-11-01, which lifted the rows it passed to at most 96%; every other check ran on one PC); a row
that failed in a real session and is fixed on one PC only scores lower still (80-88%), and rows where we
chose to differ from ERSC (a wipe respawns everyone together, 4 players by default instead of 6, one shared drop roll)
are capped at 95% too.

The score bars are in the [README](../README.md#seamless-co-op-progress).

## Coming next

Features marked (0.7.9) are on the development build and come with the next release.

| Change | Status |
|---|---|
| Character creation: choosing glasses no longer locks the editor; a clean Favourites portrait | In progress |
| A guest joining the host in Yahar'gul loads in (no endless loading screen) | Needs the host's log |
| FSR 4 white or grey picture fixed | Protective fixes built; the cause (FSR 4 with HDR on in Windows) is still being confirmed |
| Annalise: the guest's character plays the kneel animation (0.7.9) | Built (both players see it) |
| Guests load at the same time as the host on party travel (about 10-15 s less per trip) | Planned (opt-in first) |
| Host death the ERSC way: the host loads alone, guests play on | Planned (opt-in first; today the whole party reloads) |

## Known issues

- **FSR 4 can show a white or grey picture** on some PCs. The cause is under investigation (suspected:
  FSR 4 with HDR turned on in Windows). Meanwhile switch to FSR 3.1, DLSS or no upscaler.
- **Real sessions:** a 4-player playtest over the Internet (2026-11-01, 5 hours) found the bugs fixed in 0.7.8;
  the newest features (invasions, item giving, chat, the map, look sync, map markers, 5 and 6 players) were tested on one PC with several game copies and
  simulated internet lag; the 0.8.0 build also ran a party and invasions over the real Internet from one PC (a router with UPnP, a guest through a VPN exit and a member relay), which is not a second household. Please send support bundles if something goes wrong (see
  [Reporting a co-op bug](#reporting-a-co-op-bug)).
- **Reported in real sessions and fixed in 0.7.6 or not yet, none confirmed live on two PCs:** after an ending nobody could join (fixed 0.7.6), a guest's save advancing two New Game+ cycles (fixed 0.7.6), joins over a VPN failing for a guest behind a strict NAT, a 4-player lobby with guests stuck loading, 4-10 s outages on Hamachi or a phone hotspot sending guests home, guests not seeing each other until a teleport, a wrong ending cutscene and late credits for a guest, the boss standing idle while the host is dead and a guest fights, talking to an NPC next to a lamp opening the lamp's travel prompt for a guest, and a clip on the edge of descending lifts. Open ones are tracked in [ERSC_PARITY.md](party/ERSC_PARITY.md).

## Connecting over the internet

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
reachable member still cannot meet; there, let another player host or use a VPN such as Tailscale. Details: [docs/party/network_robustness.md](party/network_robustness.md).

## Reporting a co-op bug

| Step | How |
|---|---|
| Before playing | Launcher → *Advanced / Debug* → *Party logging*: **Verbose** on every PC |
| When it happens | Press **F9** (or *Party tab → Mark an issue*): every player's log gets `=== USER MARK n ===` |
| After playing | *Advanced / Debug* → **Save support bundle** on every PC and send the zips (passwords, codes and addresses are removed). A crash writes one by itself to `logs\` |

Details: [docs/party/logging.md](party/logging.md).
