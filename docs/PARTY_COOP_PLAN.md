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

Tests on one PC: `tools/mp/instances.py` (separate instance folders, 30 fps, VRAM cap),
`tools/party/*` (pad driver, simguest from bbhost simclient, verdict, flag diff). Two-PC tests
wait for the user's go-ahead.

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
   are replayed (done in A4).
4. When the guest's world is up the director rings the bell; the host's director sees a waiting
   member and summons them. The host's game treats the vanished phantom as a disconnect.
5. Host crash: guests return to their own worlds, keep reconnecting (backoff), and are summoned
   again once the host is back.
6. Progress made meanwhile is caught up by the progress sync (C2) on return.
Test: kill a guest instance mid-session in the local harness; verdict = back in the host's world
within ~60 s with no input.
