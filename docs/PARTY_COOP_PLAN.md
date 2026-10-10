# Serverless party co-op — implementation plan

Goal: the whole campaign together, from character creation to the end credits, every player in
sync and every save progressing. Serverless "party P2P": the host's game process does everything
a server would; friends join with a party code (host address:port + secret). No central server,
no relay server (the host itself relays between guests when needed). Up to 4 players if the
engine allows (start with host + 2).

Research: `../reports/Bloodborne seamless coop feasibility.md` (outside the repo) and
`research_notes/`. Main code reference: droogie/bbhost (GPL-3.0-or-later, @8f2746c) — files
copied from it carry `SPDX-License-Identifier: GPL-3.0-or-later` and
`derived from droogie/bbhost <path> @8f2746c`.

## Address conventions (verified against eboot.elf; vaddr 0 at file offset 0x4000)

- **Our offset** = guest address − image base 0x800000000 = raw ELF VA.
- **bbhost source addresses** = our offset + 0x400000. bbhost `include/.../*.hpp` `Rva{}`
  constants are already our offsets.
- **shadp2p addresses** = our offset.
- **patches/Bloodborne.xml `Address`** = our offset + 0x400000.

| Item | bbhost / XML | Our offset |
|---|---|---|
| FrpgNetMan slot | 0x593b120 | 0x553b120 |
| SummonStepManager | FrpgNetMan+0xc50; passive list +0x158 | |
| SummonData make wrapper | 0x1886a10 | 0x1486a10 |
| SummonDataWrapper vtable | 0x5722570 | 0x5322570 |
| Matching2 signaling gate (u8 = 1) | 0x586cc09 | 0x546cc09 |
| LuaEvent_DispatchByName | 0x1739870 | 0x1339870 |
| SprjLuaEventMan slot (ctx = *(*(slot)+8)) | 0x593b0c8 | 0x553b0c8 |
| GameDataMan slot (+8 player: +0x84 Insight, +0x90 level) | 0x593b130 | 0x553b130 |
| SprjSessionManager slot (+0x124 state) | 0x5940290 | 0x5540290 |
| WorldChrMan slot | 0x593e878 | 0x553e878 |
| NP manager slot (object IsOnline +0xd8) | 0x5ac7048 | 0x56c7048 (object at 0x56c7058 per bbhost) |
| Network flow (+0x1590 online mode, +0x16f8 slot table) | 0x5956678 | 0x5556678 |
| Cooperator count fn | 0x19bdc20 | 0x15bdc20 |
| SOS sign timeout float 180.0 | 0x4d27a5c | 0x4927a5c |
| SOS filter call / filter | 0x1c7338a / 0x1c74710 | 0x187338a / 0x1874710 |
| EMEVD instruction dispatch | 0x1bb93a0 | 0x17b93a0 |
| SetEventFlag / GetEventFlagValue / SetEventFlagValue | 0x17cfcc0 / 0x17cfd80 / 0x17d0060 | 0x13cfcc0 / 0x13cfd80 / 0x13d0060 |
| EventFlagMan slot | 0x593b100 | 0x553b100 |
| BroadcastSetFlag; snapshot serialize / apply | | 0x132aad0; 0x13be3c0 / 0x13beca0 |
| WarpNextStage / _Bonfire / Kick | | 0x132e010 / 0x132e050 / 0x1332b80 |
| SessionWorldTransition (leave only while WorldTransitionState+0x1592 == 0) | | 0x13cde30 |
| MoveMapStep ctor | | 0x1937570 |
| SummonedMapReload serialize; SetSessionTargetMap (+0x14c4) | | 0x131e5c0; 0x156ce80 |
| SprjSessionManager stop (assert string; CSMultiPlayMan is a separate singleton at 0x5540230); wrapper (0xFF000023) | | 0x1ED07A0; 0xC8ED70 |
| Travel funnel (19 callers) / lamp warp by id / Dream area-table gate dword | | 0x13CDE30 / 0x13CDF30 / 0x47304B0 |
| Map-reload stop call → retarget to 0x1ED00A0 | | 0x19471B1 |
| Boss-cleared rejection (flag 2410), NOP | | 0x18749E8 |
| Negative event id; responder area; builder jump | | 0x18749F0; 0x14B714A; 0x1874B79 |
| Bell gates (shadp2p) | | 0x157F8C8, 0x157F960, 0x157F6D1/6D3, 0x15068BB/0x15068FC, updater 0x1506820, 0x191A8C3, 0x18700D3, area validator 0x131D7B0 |
| Lamp prompt gate (host +0x49) | | 0x12F8315 |
| MapItemMan lot→items | 0x1e8b080 | 0x1a8b080 |
| Skip-online dialog builder; offline item operator() | | 0x1b39030; 0x1b49cd0 (vtable 0x533be30) |
| SprjFlipper (main-thread tick candidate) | 0x2434770 | 0x2034770 |

Bell events (main thread only, through LuaEvent_DispatchByName): host `OnEvent_Call_SOS`
(Beckoning Bell), guest `OnEvent_SendSoulSign_NormalCoop` (Small Resonant Bell).

## Architecture

- Every `sceNet* sceNetCtl* sceHttp* sceSsl* sceNp*` import goes through
  `runtime_services_resolve` (src/runtime_services.c); no network module is LLE. With `BB_PARTY`
  set, names (except NpTrophy/Commerce/ProfileDialog/Score) resolve to a new C++ library in
  `gpu/shim/net/` + `gpu/shim/party/` (sysv_abi functions), else the offline stubs.
- bbhost's `net/session.h` server seam: replace its HTTP `np_post` with `PartyTransport`
  (`LocalHost` in-process on the host, `RemoteGuest` RPC over PartyLink on guests).
- Host: in-process `FromApi` (ss.info XML, basic_utils/*, summon_messenger/* party-filtered,
  others empty/OK, play-log dropped) + `PartyHostService` (Matching2 rooms, signaling resolve,
  per-member event queue pushed over PartyLink) + STUN Binding responder and guest↔guest relay
  on the party UDP port.
- PartyLink: TCP control `[u32 len][u8 type][payload]`, HELLO (version, name, eboot SHA-256,
  gameplay patch hash + names from out/party_patch_hash.txt (scripts/patches.py; graphics / FPS /
  resolution patches excluded), gameplay mod hash + names from out/party_mods.txt (scripts/mods.py;
  texture / shader / sound files excluded); a Mismatch REJECT says which of the three differs and
  which names only one side has) / CHALLENGE / AUTH (keyed BLAKE2b) / WELCOME / REJECT, then XChaCha20-Poly1305
  (key = Argon2i(password ‖ code secret), monocypher). PING/PONG 1 Hz, ROSTER, RPC_REQ/RESP,
  EVENT/ACK, PARTY_CMD, PROGRESS, BYE.
- Party code: `BBP1-` + Crockford base32 {ver, flags, ipv4, port, secret[8], crc16}; Internet
  (STUN public address / UPnP via IUPnPNAT) and LAN codes; plain host:port also accepted.
- Game P2P traffic: real Winsock UDP with bbhost's vport framing `[0xff][flags][src][dst]`,
  probe `fe 'bbhp'`, relay framing `[0xfb]['R'|'r']`; port remapped to `BB_PARTY_PORT`.
- Guest callbacks: one host thread attached with `runtime_thread_attach_host`, calling
  `restore_guest_fs()` before every guest call.
- Network simulator `BB_NET_SIM="lat=80,jitter=20,loss=2,dup=0.5,reorder=1,seed=7"`.

## Phases

**A — v0.4 foundation:** A0 RE spike (FROM API response schemas via response-key string
xrefs; PLAY ONLINE item functor; Insight check on the bell Lua path; P2P port/vports) ∥ A1
transport (net.cpp, stun, json, glue, netsim) → A2 NP signed-in (online id = party name) → A3
host service (FromApi, PartyHostService, http_hle, np_session/matching2/signaling ports) → A4
PartyLink + party code + UPnP + public address → A5 automatic summoning (main-thread tick hook,
Lua bell events, PartyDirector state machine; party-only sign board; inject fallback) → A6
session persistence (boss-kill rejection NOPs, EMEVD dispatch filter, map-reload retarget,
HP 0.7→1.0 via SpEffect 9006/9026, auto-rejoin after any session end, API-level suppression of
member-left while reloading) → A7 UI (launcher Party tab, overlay Party tab, ini keys, logs).
Milestones: host boots ONLINE alone → guest boots ONLINE via host → manual bells co-op (2
instances) → auto-summon 2 then 3 → auto-rejoin after guest death/boss/host death → all under
BB_NET_SIM=dsl and forced relay.

**B — travel together:** B1 follow-and-rejoin (hook WarpNextStage(_Bonfire) on the host,
guests warp to the same lamp, auto-rejoin; Hunter's Dream as a lobby) → B2 true seamless warp
(hold +0x1592, transition-snapshot MoveMapStep, SetSessionTargetMap, lamp gates; research).

**C — full campaign:** C1 start (characters in own saves; party from the first lamp) → C2
progress sync (host-authoritative event flags via SetEventFlag hooks, allowlist, applied at
safe points with snapshots) → C3 items/rewards (lot-to-items replay) → C4 cutscenes and endings
(Dream summon rejection, ending flags/EMEVD replay) → C5 save policy (separate party saves,
backups, world reset on rest), 4 players (presentation table has 5 slots; coop cap RE).

**C5 save policy (done: folder, backups, check; world reset on rest open):**
`src/runtime_savepolicy.c` / `save_policy.h`. `BB_PARTY=host|join` + `BB_PARTY_SAVE=separate`
(launcher default; the runtime default without it is shared, so test harnesses keep using
`user\savedata`) maps every save path (`root()`, memory.dat, the sound hack) to
`user\savedata_party`. The launcher (`scripts/party_saves.py`) offers the explicit copy of
`user\savedata` on the first separate-save launch and has a copy button; the copy goes through
a `.copying` folder and backs up a replaced party save. At start the runtime checks the folder
(userdata0000-0009 = 0x140000 bytes, userdata0010/backup0010 = 0x40000 and not all zeros,
param.bin = 1328; vs the newest good backup: emptied slots, lost files, changed headers) and,
in a party session (`BB_SAVE_BACKUP=1` forces it), copies it to
`user\save_backups\<folder>\<time>-start|restart|timer[-NN][-suspect]` at start and every
`BB_SAVE_BACKUP_MINUTES` (10). Copies are taken only after 5 quiet seconds and discarded if
`runtime_file_save_activity` (generation of /savedataN changes, open save writers) moved
during the copy; a copy is written as `.partial` and renamed when complete; identical copies
are dropped; a copy failing the check is `-suspect` with its own quota (`BB_SAVE_BACKUP_KEEP`,
10), so a crash-restart with a damaged save never rotates good backups away. Tests:
`ninja -C out/gpu save-policy-test`, `tests/test_party_saves.py`.

Tests on one PC: `tools/mp/instances.py` (separate instance folders, 30 fps, VRAM cap),
`tools/party/*` (pad driver, simguest from bbhost simclient, verdict, flag diff). Two-PC tests
wait for the user's go-ahead.

## Status per phase (2026-10-10)

Sources: `docs/party/*.md` (implementation / results sections) and the commit log. Columns:
**Code + unit tests** = implemented and covered by the named test target (no game needed);
**In the game** = what has actually been seen running in the game, and how (one instance, or
several local instances on one PC over loopback); **Open** = not done or not yet observed.
Nothing has run between two PCs yet, and no human guest has yet been seen summoned into a
host's world by the party layer (the instance harness cannot leave the title screen without
input, so the in-world summon flow has not run there).

| Phase | Code + unit tests | In the game | Open |
|---|---|---|---|
| A0 RE spike | done (`docs/party/from_api_schema.md`, FromApi formats confirmed) | - | - |
| A1 transport | net layer, STUN, relay, netsim; `party-net-test`; `party-soak` (host + 3 guests, lan/dsl/bad, outages, crashes: all PASS); PeerPaths relay fallback | - (party-soak runs without the game) | game traffic between game instances; real NATs / routers |
| A2 NP signed in | online id = party name; `party-net-test` | - | the FROM login reaching the online main menu through the party NP layer (seamless_rules.md "still needs") |
| A3 host service | FromApi, Matching2, signaling, http_hle; `party-host-test` (also with 4 players) | - | a game-created room joined by a game client end to end |
| A4 PartyLink, code, UPnP, public address | `party-link-test` (95+ checks, 4 players, kick by name, host restart); version check covers gameplay patches / mods / rules | 4 loopback instances reach the host's roster (`Hunter0..Hunter3`, title) | UPnP / STUN against real routers (not testable on one PC) |
| A5 auto summoning | director, tick hook, Lua bell events, game state | single instance: tick once per frame, Lua bells accepted, map / role / Insight reads (section below) | an automatic summon between two games |
| A6 seamless rules | XML party patches, param rules (full HP, bells without Insight), EMEVD filter, online title skip; patch report per site | single instance (NPC fixture, npc_peer.md 5.2): with `BB_PARTY` set, NPC summons go through in a boss-cleared area | every item of seamless_rules.md "What still needs a live 2-instance session" |
| A7 UI | launcher Party tab + Rules & sync + Party help; overlay Party tab, toasts, status bridge (Leave / Rejoin / Kick); `party-status-test`, `party-status-bridge-test`, `tests/test_party_saves.py` | - | user guide `docs/PARTY.md` written; two-PC use |
| Crash recovery | run.bat restart loop, auto-continue, rejoin by token / name, host keeps code and members; `party-link-test`, `party-soak` | instance harness (`--crash-instance 0`): the host killed and restarted with the same code, the guest back on slot 1 by token 7.1 s after the kill | host crash with a guest in the host's world |
| B1 follow and rejoin | host travel capture, guest replay, send-home retarget, Dream gate; `party-travel-test` | - | a real host warp followed by a guest |
| B2 seamless warp | research only (travel.md 4: research-grade, highest crash risk) | - | not planned now |
| C1 campaign start | readiness (`prologue_solo` / `immediate`), Prologue roster state, bell grants; `party-start-test` | single instance: prologue flags read, Small Resonant Bell granted (goods 205 0 -> 1) | PR1 (does an automatic sign need the bell?) undecided at runtime |
| C2 progress sync | host flag capture, guest apply into its own save, snapshots, BBPF dumps; `party-progress-test` | - | a host flag change applied on a real guest; NPC quests (off by default) |
| C3 items | lot capture, guest replay, ledger, parity patches; `party-items-test` | single instance: lots awarded through `BB_PARTY_ITEMS_TEST`, ledger flag set, parity patches `applied` | replay from a real host; NG+ ledger flags; mark lost on a crash |
| C4 cutscenes / endings | capture, live mirror, replay queue, time of day, endings; `party-story-test` | single instance: remo replay (85 s) and mirror call (mode 2), queue survives a restart | a host-triggered cutscene on a real guest; endings for guests |
| C5 save policy | separate folder, launcher copy, rotating backups, save check; `save-policy-test`, `tests/test_party_saves.py` | - | world reset on rest |
| Guests as full players (phantom limits) | respawn at host lamp, refill, open world, boss Insight, opt-in Mark; `party-phantom-test` | single instance probes for Insight / refill only | every item, in a real session |
| 4 players | H1-H4, E6, P5, rules in the version check; `party-fourp-test`, `party-link-test`, `party-host-test` with `BB_PARTY_MAX=4` | 4 loopback instances install the patches and reach a 4-member roster at the title | a 3rd cooperator in the world, boss scaling, NPC signs, invader slot (cap 5) |

## Verified in A5 (single instance)

- Tick hook SprjFlipper::Update 0x2034770 runs once per frame on the game's main thread (title,
  loading, world). LuaEvent_DispatchByName 0x1339870 (ctx, name) returns 0 for unknown names.
- Map id: PlayerIns_GetBloodMarkMap 0x19046c0 → *(*(WorldChrMan+0x60)+0x400)+0x48.
- NowLoading byte 0x556286b (set 0x176d380, cleared 0x176d3c0).
- Session manager +0x124 role: 0 idle … 3 host, 4 trying to join, 6 client, 7 leaving.
- Cooperator count 0x15bdc20 asserts without WorldChrMan / session — call only in the world.
- Bells via Lua are accepted offline; the Insight gate is inside the Lua body.

## Requirement: seamless crash recovery

A crashed player (guest or host) must end up back in the party without doing anything:
1. Party mode restarts the game after a crash (non-zero exit) with the same settings
   (launcher / run.bat loop, BB_PARTY_RESTART=0 disables).
2. On a party restart the director confirms the title screen "Continue" itself and loads the
   save (BB_PARTY_AUTOCONTINUE, on by default in party mode).
3. PartyLink keeps a dropped member's slot 60 s and resumes it by token or name; missed events
   are replayed (done in A4) when the link resumes by token (the same game process). A crashed
   and restarted game rejoins by name into its old slot with a fresh event stream.
4. When the guest's world is up the director rings the bell; the host's director sees a waiting
   member and summons them. The host's game treats the vanished phantom as a disconnect.
5. Host crash: guests return to their own worlds, keep reconnecting (backoff), and are summoned
   again once the host is back.
   Done (agent/hostcrash): the restarted host keeps its party code (secret from the crash marker
   <user>/party_state.json, then party_secret.txt, then party_code.txt) and its member table
   (slot, name, token in the marker -> PartyLink::restore_members); guests notice the TCP reset
   at once, end the game's room with the host after 2 s (BB_PARTY_HOST_LOST_MS; ROOM_DESTROYED
   0x1104 cause LEAVE, as when a host leaves) and reconnect with 1, 2, 4, 8 s backoff
   (then every 8 s, +-20 %). Harness
   run (--crash-instance 0): guest back on slot 1 by token 7.1 s after the kill, same code.
6. Progress made meanwhile is caught up by the progress sync (C2) on return.
Test: kill a guest instance mid-session in the local harness; verdict = back in the host's world
within ~60 s with no input.

## Transport robustness (party-soak)

`cmake --build out/gpu --target party-soak && out/gpu/party-soak.exe [scenario...]` runs a host
and three guests in one process over loopback, no game: the real PartyLink (each guest through
a TCP proxy applying the profile: latency, jitter, a lost segment costing one more round trip,
outages), the host service's event queues pumped into PartyLink as party_runtime.cpp does and
delivered through RemoteGuest, game UDP with the party port's framing and relay
(`shim/net/party_udp.h`, shared with net_socket.cpp) through `netsim::Queue`, each guest sending
every player a 30 Hz stream directly and the other guests a second one through the host relay
(BB_PARTY_FORCE_RELAY routing), and a stand-in main thread calling what the director calls every
frame. One `VERDICT <scenario>: PASS|FAIL` line per scenario; details in `party_soak.log`.

Profiles (BB_NET_SIM presets): lan = lat 1 jitter 0.5; dsl = lat 80 jitter 20 loss 2 dup 0.5
reorder 1; bad = lat 200 jitter 80 loss 8; bursts `outage=<ms>,every=<ms>`.

Results (2026-10-10, one loaded PC; one-way latency, so a relayed datagram crosses two links):

| scenario | events host->guest p50 / p99 | RPC rtt p50 / p99 | UDP direct p50 / p99, delivered | UDP relay p50 / p99, delivered | reconnect |
|---|---|---|---|---|---|
| lan | 2.4 / 14 ms | 4.5 / 15 ms | 1.4 / 20 ms, 100 % | 3.1 / 23 ms, 100 % | - |
| dsl | 86 / 280 ms | 175 / 490 ms | 84 / 157 ms, 97.9 % | 181 / 259 ms, 96.3 % | - |
| bad | 347 / 756 ms | 752 / 1357 ms | 220 / 282 ms, 92.0 % | 474 / 554 ms, 86.1 % | - |
| outage 5 s (dsl) | no event lost; link not dropped (lost_timeout 10 s) | | back 0.1 s after | back 0.2 s after | none needed |
| outage 30 s (dsl) | no event lost; replayed on resume | | back 0.1 s after | back 0.2 s after | 0.6-4.5 s after the network is back (was 11 s with 16 s backoff steps) |
| connection reset (dsl) | replayed, resumed by token | | | | 1.1-1.5 s |
| guest crash, restart 20 s later | same slot by name | | | | 0.4 s after its restart |
| host crash, back 10 s later | new host's events all delivered | | | relay ports kept | 0.4-2.2 s after the host is up |

Relay overhead: one more link (the host's) per datagram, i.e. about one extra one-way delay and
that link's loss (dsl: +95 ms, 96.3 % vs 97.9 % delivered); 12 bytes more to the host
([fb]['R'][token][port]) and 4 more to the receiver ([fb]['r'][port]) per datagram (about 17 % /
6 % of a 70-byte game datagram); the host forwards each one once (send + receive).

Bugs the soak found and fixed:
- After a host restart every event of the new host was dropped by RemoteGuest as a resend (its
  EventIds start at 1 again): `RemoteGuest::reset_event_cursor` on a non-resumed WELCOME
  (`PartyLink::session_resumed`), which also re-sends context_start and the relay HELLO
  (`np_session::host_session_reset`).
- A restarted host had a new random party secret: every guest was rejected (wrong code) and
  stopped retrying (fixed in parallel by agent/hostcrash: secret and member table restored).
- The relay forgot every client on a host restart and handed out new ports: relayed traffic
  stopped until the next keepalive and the other members' addresses went stale. Relay ports are
  derived from the token, and a frame or HELLO with an unknown token re-registers it.
- Every datagram to a port on the host's address was relay-framed, also a player's own port
  when it shares the host's address (one machine, or one NAT): only relay ports the guest routes
  to are framed now.
- Nothing ever chose the relay (the published RelayPort was never used): guest-to-guest paths
  now fall back to it per peer (below); BB_PARTY_FORCE_RELAY=1 relays every guest pair.
- A resumed member's backlog (600 events after 30 s) was replayed with one send() per frame
  while holding the link lock: 37 ms stalls of the main thread's `state()`. Frames are batched;
  the per-frame getters read a snapshot and never wait for the IO thread.
- The reconnect backoff reached 16-30 s steps (11 s idle after a 30 s outage ended); now
  1, 2, 4, 8 s with +-20 % jitter.
- MinGW condition-variable waits and sleeps round to the 15.6 ms tick (timeBeginPeriod does not
  help): BB_NET_SIM added ~15 ms to every simulated delay. `netsim::Waiter` (event +
  high-resolution waitable timer).

## Guest-to-guest paths: direct, or the host relay (PeerPaths)

Each guest probes every other guest's direct address twice a second (`fe 'bbhp' 0 0 0`); a
bbport port answers every probe with `fe 'bbhp' 1 0 0`. An answer proves the direct path both
ways. A peer that never answered within 2.5 s, or that stopped answering for 2.5 s (5 probes), is
reached through its relay port on the host until an answer comes back (then direct again,
within ~0.5 s). The game never sees it: it keeps the peer's direct address; a relayed datagram
leaves framed for the peer's relay port and relay deliveries from that port are presented from
the peer's direct address (`party_udp.h` PeerPaths, used by net_socket.cpp; peers and relay
ports come from the host's member records in np_session). The host is always direct (its port
is the relay). The status line lists each peer as direct / relay / probing.

party-soak `nat-pair` (dsl): Alice and Bob blocked both ways from the start - their traffic
flows through the relay after 2.7 s (95 % delivered over two lossy dsl legs); the path opens -
direct again after 0.3 s; it closes mid-session - relayed after 2.3 s, datagrams flowing again
after 2.4 s; the pairs with Carol never switch. Under `bad` (8 % loss) no pair switched; in the
outages every pair went to the relay and back (both dead meanwhile).

## Threads and waits: nothing blocks the game's main thread

The director (main thread, SprjFlipper tick) uses only PartyLink calls that never wait for the
network: `state()`, `roster()`, `local_slot()` read a snapshot (a mutex held for a copy);
`set_local_state()` returns at once unless the state changed; `send_event()`,
`send_party_cmd()`, `send_progress()`, `kick()` take the link lock and queue frames on
non-blocking sockets. The IO thread holds that lock for at most ~1.6 ms of its own CPU in the
soak (a 30 s backlog replay); name resolution, socket() and connect() run outside it.
party-soak measures every main-thread call (p99 0.04-0.11 ms, max under 10 ms on a loaded
machine) and the IO thread's lock holds.

Every other wait, by thread:

| wait | where | bound |
|---|---|---|
| PartyLink::rpc_call | session thread / http worker (RemoteGuest) | its timeout (3-4 s; http 15 s); fails at once while disconnected |
| sceNpSignalingActivateConnection resolve | session thread (was the game's caller: an RPC of up to 4 s) | 4 s |
| Matching2 calls, heartbeat, context_start, keepalive STUN | session thread (`dispatch_after`) | 4 s per RPC, STUN 3 x 1 s |
| sceHttp blocking request | the game's own network thread, by its choice of a blocking request | 15 s RPC, 35 s wait |
| sceHttpWaitRequest, P2P recv without SO_RCVTIMEO, sceNetEpollWait | the game's own threads, as the game asked (abortable) | the game's timeout |
| sceNetResolverStartNtoa | the game's thread, as asked | the OS resolver |
| sceNetSocketClose of the last P2P socket | game thread; the party port stays pinned in party mode | reader join <= 200 ms otherwise |
| host startup (Argon2, UPnP 3 s, STUN 3 x 2 s) | the runtime's startup thread | as listed |
| shutdown | exit path | 2.5 s overall |
| netsim sender, PartyHostService pumps, link IO/callback threads | their own threads | - |
