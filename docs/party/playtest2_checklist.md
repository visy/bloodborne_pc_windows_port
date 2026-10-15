# Playtest 2 checklist (real internet, two or more PCs)

What it is for: the rows that one PC cannot finish. Everything below has passed on one PC (several
game copies over loopback, with the network simulator adding internet latency, loss and NAT); what
is left needs **real PCs in different homes, real routers and real internet**:

| Row | What one PC already proved | What only this session can prove |
|---|---|---|
| F1 internet play | 250 ms round trip, 2-3 % loss, reordering, 5-30 s outages: joins, travel, deaths, bosses, kills, desync digests (network_robustness.md 3a, 7b) | the same on a real line (real jitter, Wi-Fi, other traffic on the line), for a full evening |
| F2 NAT | 25 simulated NAT pairs, the DHT rendezvous on a fake DHT, UPnP code paths (network_robustness.md 3b-3d) | UPnP on real routers, a port forward by hand, the guest code (`BBG1-`) hole punch, the public DHT, a symmetric / CGNAT line |
| F3 security | fuzzed parsers, password lockout over loopback (section 6) | an open port on the internet for a few hours (scanner traffic in the host's log) |
| F5 outages | 10-120 s simulated outages keep the session (loopback) | a real cable pull / Wi-Fi off, a NAT mapping or address change during a long cut |
| A/B/C/D rows | scripted runs | real players doing unscripted things (the bug list) |

Playtest 1 (2026-10-13, 0.6.0) was such a session: [playtest1_triage.md](playtest1_triage.md). Its
gaps: no guest logs for parts of the session, no per-line timestamps, nobody measured the ping.
This checklist closes those gaps.

Never paste a real IP address, party code or password into a bug note or a chat that ends up in the
repo: the support bundle removes them; notes should say "the host", "guest 1".

## 0. Before the session (each PC, ~10 min)

1. Same build on every PC: the release zip of the same version (title screen shows `bbport 0.6.2`);
   same game files (update 1.09), same mods and launcher party options. A mismatch is refused on join
   with a message that says what differs: fix that first.
2. Launcher -> *Advanced / Debug* -> *Party logging*: **Verbose**. Leave *Tracing* **off** (it slows
   the game and floods the log; playtest 1 guest ran with it on).
3. Launcher -> *Party* tab: **Your name** set (different for everybody).
4. Host only: leave **UPnP** on. Note the router model (for the F2 table below).
5. Everybody: note your connection (fibre / cable / DSL / mobile / Wi-Fi or cable to the PC).
6. Everybody: a save past the opening is best (party saves are separate from single-player ones).
7. Agree a voice channel, and one person who writes the notes (time + what happened + who).

## 1. Connecting (F2, ~15 min) - record the path each try took

Each step: the host starts first (*Party* -> *Continue/New party game*), then the guest. Write down
whether it worked, how long the guest waited, and the **Network** line of the guest's *System ->
Party* page (it says the NAT type and the path the link took). After each step, *Leave the party*
on the guest and go on.

| # | Setup | Expected | Result / path / time |
|---|---|---|---|
| 1a | Host UPnP on (default), guest pastes the **Internet code** | link over TCP to the host's port, guest beside the host in ~15 s | |
| 1b | Host turns UPnP **off** (launcher *Party* tab), no port forward, guest uses the Internet code | fails at TCP; the failure message names the guest code / DHT options | |
| 1c | Same as 1b, guest sends its **guest code** (`BBG1-...`, *Party* page) to the host, host presses *Add a guest code* | UDP hole punch, link over UDP | |
| 1d | Same as 1b, **both** turn on *Find each other through the public DHT*, no guest code | the host finds the guest through the DHT within ~1 min, link over UDP | |
| 1e | Host forwards port 9307 UDP+TCP by hand in the router, UPnP off | link over TCP | |
| 1f | (if anyone has a mobile hotspot / CGNAT line) that PC as the guest, then as the host | guest on CGNAT: should work via 1a; host on CGNAT: expected to fail; the Network line says "symmetric" / "let another player host" | |
| 1g | Wrong password typed 6 times on purpose by a guest, then the right one | "too many wrong passwords ... try again in 30 s", the right one works after it | |

F2 table to fill in (router model, NAT type the Network line showed, which of 1a-1f worked):

| PC | Router / ISP type | NAT type shown | 1a | 1b | 1c | 1d | 1e | 1f |
|---|---|---|---|---|---|---|---|---|
| host | | | | | | | | |
| guest 1 | | | | | | | | |
| guest 2 | | | | | | | | |

## 2. Playing (F1, 60-120 min) - just play, but cover these

Turn on *System -> Party rules -> Above companions -> Name and ping* for the whole session: the ping
over each companion is the number to write down every 15 min (a few values, e.g. "host 80-95 ms").

Tick each when it happened at least once; press **F9** the moment anything looks wrong (it marks every
player's log at the same moment) and say out loud what you saw.

- [ ] Guest joins with *Continue party game* (one loading screen, beside the host)
- [ ] Party travel by lamp ("Travel with the party?" -> YES): one loading screen each
- [ ] A guest travels alone (NO), uses the Doll / shops in the Dream, comes back
- [ ] Each player kills enemies; everybody gets the echoes
- [ ] A guest dies (gets up at its own lamp, no loading screen when it is in the area)
- [ ] The host dies (one loading screen for everyone at the host's lamp)
- [ ] Everyone dies (wipe)
- [ ] A boss fight: a death in it (watch a living member, Cross switches with 3+ players), the kill
- [ ] A guest opens a door / lever / shortcut; it is open on the host
- [ ] A guest picks up an item and talks to an NPC
- [ ] Ping over each companion written down 4+ times
- [ ] With 3+ players: everybody sees everybody (known gap: a later guest may not see an earlier one)

## 3. Outages (F5, 10 min) - real network cuts

Do these in a quiet spot (no enemies), with everybody in the same area. The player cutting the
network says "now" and "back".

| # | What | Expected | Result |
|---|---|---|---|
| 3a | Guest switches Wi-Fi off (or pulls the cable) for ~10 s | the others freeze in place, "Connection lost" notice; back: everyone carries on, no loading screen | |
| 3b | The same for ~25 s | the same | |
| 3c | The same for ~60 s | as 3a (loopback: 60 / 90 / 120 s kept the session, network_robustness.md 7c) | |
| 3e | The same for ~2 min 30 s (beyond the 125 s hold) | the guest goes home with one loading screen, then is summoned back beside the host by itself once the network is back | |
| 3f | Guest's router restarted (new public address) during a cut | note what happens: the game's P2P link may not resume on a new address | |
| 3d | The **host** cuts for ~10 s | the same as 3a, for every guest | |

## 4. Crash rejoin (C6, 5 min)

| # | What | Expected | Result |
|---|---|---|---|
| 4a | A guest kills its game (Task Manager -> bbport.exe -> End task) and starts it again (*Continue party game*) | back in the host's room about 15 s and beside the host about 20-30 s after the kill | |
| 4b | The host does the same | the guests go home at once and come back by themselves once the host is in the world again | |

## 5. Open port for a while (F3, optional, passive)

If the host is happy to keep the port open (UPnP or forward) for an hour or more: nothing to do, the
host's log records any stranger traffic (`refused`, `handshake`, `too many`), which shows what the
open internet sends to the party port. Say in the notes how long the port was open.

## 6. After the session (each PC, 2 min) - the most important step

1. Launcher -> *Advanced / Debug* -> **Save support bundle** on **every** PC (host and every guest),
   even if nothing went wrong on that PC. It removes passwords, codes and addresses.
2. Send the zips plus the notes (times, F9 marks, the tables above filled in).
3. Write down the build's version string and the time zone each PC is in (the logs carry local
   times).

## What the result turns into

- F1 goes to 100 when section 2 ran for an hour with no lost session and no stuck state, and the
  pings are recorded; each bug found becomes a row in a `playtest2_triage.md` (as playtest 1).
- F2 goes up by what the F2 table shows: every method that worked on a real router is verified; a
  method that failed gets a triage row with the router type.
- F5's "a real network" note is closed by section 3.
