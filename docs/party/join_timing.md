# Join / summon timing (fast join)

Where the time of an automatic party summon goes, which pauses were removed, and what is left.
Switches: `BB_PARTY_FAST_JOIN` (game pacing patches, sign polling, director pauses, room
delays) and `BB_PARTY_FAST_MESSAGES` (banners); both on by default in party mode, `0` / `off`
turns each off (gpu/party_env.h `bb_party_switch_on`, scripts/party_env.py `party_switch_on`).
Addresses are our offsets (XML `Address` = offset + 0x400000).

## Measuring

`tools/mp/instances.py run --timestamps` prefixes every log line of every instance with
`[+SSS.mmm]` (seconds since the harness started the first instance, one clock for all).
`tools/mp/join_timeline.py --root <root>` prints the milestones of the summon in time order
(`--from 1:"Launching Bloodborne":2` measures a crash rejoin from the guest's restart). The
director logs the game's sign polling state when it rings (`Party: bell: sign polling: ...`, and
again 2.5 s later); `BB_PARTY_TRACE_BELL=1` adds entry traces of the sign chain (0x1900500,
0x130cde0, 0x1901010, SendSign, CreateSign, SSM tick, the summon_messenger builders).

Runs: loopback, 2 instances, the same save, guest started 8 s after the host (harness stagger).

## Automatic summon: before / after

Both players in their world -> the guest in the host's world (host `cooperators 0 -> 1`).

| Step | Before (fast off) | After | What decides it |
|---|---|---|---|
| guest in world -> guest rings | 5.0 s | 1.0 s | director: `ring_delay` (`BB_PARTY_RING_DELAY`) |
| guest rings -> sign created (`summon_messenger/create`) | 5.1 s | 5.0 s | game, open (see below) |
| sign on the board -> host's poll lists it | 1.6 s (up to 5) | 1.0 s (up to 1) | ss.info `SummonDataGetListInterval` 5 -> 1 |
| poll -> `summon_messenger/request` / `guest_invite` | 0.6 s | 0.4 s | game (room creation) |
| invite -> guest `JoinRoom` (summon data received) | 3.3 s | 0.8-1.3 s | game: host summon-message retry 3.0 -> 0.5 s |
| `JoinRoom` -> session client (6) | 3.7 s | 3.4 s | game transport (first connect retried after ~3 s), open |
| client (6) -> guest loading screen | 5.7 s | 1.2 s | game: guest self-prologue 5.0 -> 0.5 s |
| loading the host's world | 3.3 s | 3.3 s | real load (kept) |
| **both in world -> cooperators 1** | **28.7 s** | **16.9 s** | |

Evidence (`E:\games\bbport\agents\joinfast\mp`, `join_timeline.py`):

- before (`BB_PARTY_FAST_JOIN` off build, `base_inst*.log`): guest in world 38.174, guest rings
  43.196, create 48.304, host poll 49.952, request / invite 50.550, JoinRoom 53.835, client 57.571,
  loading 63.290 -> 66.609, host cooperators 1 at 66.927.
- after (increment 1): guest in world 34.536, rings 35.568, create 40.598, poll 41.575 (`sign
  polling: interval 1 s`), request 41.974, JoinRoom 43.300, client 46.733, loading 47.953 ->
  51.270, cooperators 1 at 51.412.

## What was changed

### Our code

| Item | Before | Now | Where |
|---|---|---|---|
| First bell after the world is steady (and the bell grant) | 5 s | 1 s | `party_director.cpp` `ring_delay` |
| Title: settle before the first Continue / between presses | 4 s / 2.5 s | 1 s / 1 s | `party_director.cpp` `AutoContinue` |
| ss.info `SummonDataGetListInterval` / `SummonDataCreateInterval` | 5 / 5 | 1 / 1 | `from_api_formats.inc` `kSsFastJoinValues` |
| Matching2 room answer / signaling ESTABLISHED after create / join / member joined | 300 / 800 / 500 / 300+800 ms | 100 / 300 / 200 / 100+300 ms | `np_matching2.cpp` `join_ms` |
| Party link reconnect backoff cap (a crashed host comes back) | 8 s | 2 s | `party_link.h` `backoff_max_ms` |

Not changed: 0 for the polling interval (the get-list builder 0x1e946e0 has no in-flight guard:
a request per frame); the sign lifetime (30 s patch) and the host-lost grace (2 s): they guard
correctness.

### Game patches (patches/Bloodborne.xml, byte-verified, reported by seamless_rules.cpp)

`Party: Fast join` (`BB_PARTY_FAST_JOIN`):

| Site | 1.09 | Patched | What |
|---|---|---|---|
| 0x196d3a8 | `c7051e77c0030000a040` | `...0000003f` | CSEnterleaveDirectionStep static init: guest self-prologue 0x5574ad0 5.0 -> 0.5 s (STEP_C2H_Wait_forSelfPrologue 0x196b500 gates the guest's load via STEP_C2H_Init_forSelfLoading 0x196b580 / +0x134) |
| 0x196d3b2 | `c7051c77c0030000a040` | `...0000803f` | others-epilogue cap 0x5574ad8 5.0 -> 1.0 s (0x196be90 ends earlier when the effect is done) |
| 0x196d3c6 | `c7051077c0030000a040` | `...0000803f` | return-home wait 0x5574ae0 5.0 -> 1.0 s (STEP_H2C_Wait_forEffect 0x196c2d0) |
| 0x196d3d0 | `c7050e77c00300000040` | `...0000003f` | its fade 0x5574ae8 2.0 -> 0.5 s (`alpha -= dt / x`) |
| 0x14b80c4 | `41c7451c00004040` | `41c7451c0000003f` | SummonStepManager tick: the host re-checks whether the summoned guest is a connected peer before it sends the summon data every 0.5 s instead of 3.0 s |
| 0x14b4975 | `41c744241801000500` | `...01001e00` | that queue entry's tries 5 -> 30 (the same 15 s window) |
| 0x14bb067 | `c7431801000500` | `c7431801001e00` | the same for the second creator 0x14baec0 |

`Party: Fast multiplayer messages` / `Party: Hide bell searching messages`
(`BB_PARTY_FAST_MESSAGES`): the multiplayer banner is one Scaleform dialog (SummonMessage,
`menu/summonmessage.gfx`, update 0x1bff910, FMG EventText 100001..200000). Its fades are the
movie's own (FadeIn 5 frames, FadeOut 7 frames at 30 fps: ~0.17 / 0.23 s); the hold is code:

| Site | 1.09 | Patched | What |
|---|---|---|---|
| 0x1bfface | `41c745000000a040` | `41c745000000c03f` | hold of a new message 5.0 -> 1.5 s |
| 0x4928a98 | `0000a040` | `0000c03f` | rodata 5.0 read only at 0x1bffa7d (re-show after a cutscene) |
| 0x1389cfc | `e97f618700` | `c390909090` | host "Searching for the chime of a Beckoning Bell..." (tail jmp to ShowSummonMessage 0x1bffe80) -> ret |
| 0x17c3cea | `e891c14300` | `0f1f440000` | guest "Searching for the chime of a Small Resonant Bell..." call (result unused) -> nop |

All three are `Party="cosmetic"` (left out of the party version check): they change only local
pacing, so a host and a guest with different settings still match.

## Kept (correctness)

`[0x553d6d0]+0xb8` summon / room timeouts (55 / 85 s), pending-summon lifetimes in 0x1872360
(55 s, 180 -> 30 s), the sign lifetime SSM+0x194 (30 s), CSMultiPlayerInsTask
FirstSyncCompleteWait / CreateWait (network flags), SprjMoveMapListStep::Wait_forData,
Wait_forSelfEpilogue (effect state), the world load itself, the 2 s host-lost grace.

## Open

- **Guest ring -> sign create (5.0 s).** SpEffectParam 9005 has no duration / interval; no 5.0
  on SendSign / CreateSign. Bisect with `BB_PARTY_TRACE_BELL=1` (0x1900500, 0x130cde0, 0x1901010).
- **JoinRoom -> client (3.4 s).** The guest's first 15-byte vport-30 connect is answered only by
  the second one ~3.06 s later; the NexusRevolution transport factory 0xcdb250 has a 3000 ms
  default (0xcdb2e8), not proven to be the resend interval.
- **Crash rejoin.** See below.

## Crash rejoin (guest crash)

Baseline (fast off, `crashbase_inst*.log`): guest killed at 58.7; party link sees it at 58.77;
our room service drops the member at 72.65 (15 s heartbeat timeout); the host's game leaves its
session at 82.86 (its own detection, 24 s after the crash). The guest restarts at 62.4, is in its
world at 79.0 and rings at 84.0. The host summons at 90.99 and again at 120.8 - both fail (the
host never sends the summon data) - and only the third attempt at 151.0 works: cooperators 1 at
167.3, **105 s after the restart**. Static RE: the host's peer pool (0x147b350) keeps a
reservation for the guest's online id; every summon retry (0x147af20 from the SSM tick) renews
its 30 s keep and reuses it although its connection is the crashed process's, so the summon data
goes to a dead connection until the keep lapses.
