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

## Round 2: ringing and finding the peer

Measured with `join_timeline.py` (`E:\games\bbport\agents\joinfast\mp\r2i*_inst*.log`,
`r2k*_inst*.log`). Both in their worlds -> host `cooperators 1`:

| Step | Increment 1 | Round 2 | What |
|---|---|---|---|
| guest in world -> rings | 1.0 s | 0.3 s | `ring_delay` 0.3 s |
| rings -> sign created | 5.0 s | 2.0 s | `Party: Fast bell sign` |
| sign -> host's poll lists it | 1.0 s | 0.04 s | the host director zeroes the game's poll countdown when a sign lands on our board |
| request -> guest JoinRoom | 1.3 s | 0.8 s | signaling ESTABLISHED / ACTIVE +50 / +100 ms |
| JoinRoom -> client (6) | 3.4 s | 1.7 s | NexusRevolution first retransmission timeout 3000 -> 1000 ms |
| client (6) -> loading screen | 1.2 s | 1.2 s | (round 1: 0.5 s prologue) |
| loading the host's world | 3.3 s | 3.3 s | real load |
| **both in world -> cooperators 1** | **16.9 s** | **~10 s** (33.73 -> 43.63, 33.40 -> 43.76) | |

**Bell -> sign: 5.0 s.** The traced bisect (`BB_PARTY_TRACE_BELL=1`) showed that SendSign 0x1901320
is not reached through 0x1900500 / 0x130cde0 / 0x1901010 but from the bell sign sender, a small
object (vtable 0x532f390, ctor 0x18a9050). Its update 0x18a9350 counts `+0x28` down while
`+0x20` bit 0 is set. At 0 it sends the sign of kind `+8` (0x18a9900: type 7 for the Small
Resonant Bell, or a natural-enemy type by the ss.info percentage) and re-arms 30 s for the
resend. Every start writes 5.0: the functions 0x18a9250 / 0x18a9280 (0x18a9264 / 0x18a9324) and
ten inlined starts, one per sign kind 1..10, at 0x189a095 + n*0x110.

`Party: Fast bell sign` sets the timer to 2.0 s. It is on with fast join; `BB_PARTY_FAST_SIGN=0`
turns it off alone. 0.5 s was tried and rejected: in 2 of 6 runs the guest was summoned while its
bell was barely up, and the join stalled (the host never sent the summon data, or the guest
stopped after client (6)). 2.0 s passed every run.

**Host bell while a summon runs.** The host director no longer rings within 20 s after its game
asked to summon someone (`summon_messenger/request`). A bell in that window restarted the search,
and the summon failed into the game's 55 s summon timeout.

**Kept:** the guest's 1.2 s from client (6) to the loading screen (0.5 s prologue plus the effect
start) and the 3.3 s load.

## Guest crash rejoin: before / after

| | Baseline (fast off) | Round 2 |
|---|---|---|
| guest restart -> host cooperators 1 | 104.9 s (62.40 -> 167.33) | 30.4 s (46.10 -> 76.51; 45.10 -> 75.48) |
| crash -> cooperators 1 | 108.6 s | 34.0 s |
| failed summons before the good one | 2 (host never sends the summon data) | 0 |

Why it failed (static RE, confirmed live). The host's game reuses what it had for the crashed
guest's online id and address:

- **Stale peer reservation.** The peer pool 0x147b350 (SSM+0x48) keeps a peer reserved for that
  online id. 0x147af20 renews and reuses it on every summon retry, even when its connection
  belongs to the dead process.
- **Stale transport connection.** The NexusRevolution connection to the same address/port lives
  until the transport's own timeout (~25 s, the defaults at 0xcdb309 / 0xcdb314). A summon in that
  window sends the summon data over the old connection; the restarted game waits and gives up.

Fixes:

1. `PartyHostService::context_start`: a member registering a new context while it is still in a
   room is a restarted game. Its old membership ends at once (MEMBER_LEFT + signaling DEAD on the
   host instead of the 15 s heartbeat timeout), and it is reported to the host director.
2. Host director `ClearStalePeers`: once the host's game has left the session with the dead peer,
   the peers reserved for that online id lose the id (first byte `#`) and the keep flag. The next
   summon reserves a fresh peer.
3. `Party: Fast rejoin`: the peer keep 30 -> 10 s (0x147afe4, 0x147b0a7, 0x1480cf7, 0x1480e14).
4. `FromApi::hold_signs`: a member that was in, joining or loading into the host's world when its
   link dropped has its new signs held for 26 s after the drop, so the host's game cannot summon
   it over the old connection. In the runs the hold ended 2-5 s after the restarted guest's sign
   went up, so it costs about that much. Lowering the transport timeout would remove it, but would
   also drop players on a real network hiccup that short.

### Profile and what was tried (2026-10-11, C6)

Run `crash rejoin` (loopback, 2 instances, guest killed right after it reached the host's world,
`join_timeline.py --from '1:Launching Bloodborne:2'`):

| Step | Time | Note |
|---|---|---|
| crash -> host link drop | ~0.6 s | TCP RST from the dead process |
| guest restart -> party link | 2.5-2.9 s | game boot |
| -> Continue pressed | +1.0 s | |
| -> world up -> load done | +9.0 +2.2 s | the guest's own save loads |
| -> bell rung, sign up | +1.3 +2.0 s | same as a normal join |
| sign up -> hold ends | 2-5 s | **the 26 s sign hold** (from the drop) |
| summon -> client -> in the host's world | +0.4 +2.1 +4.5 s | same as a normal join |
| **total, restart -> cooperators** | **~30 s** (crash -> cooperators ~34 s) | world up -> cooperators ~15 s vs ~4.5 s for a normal join |

So everything but the hold is the normal join plus the guest's own boot and load. Tried:

1. **Another UDP port for the restarted guest** (so the host's old transport connection would not
   match): failed. The host's game sends the summon data to the address it keeps for that online
   id (`send 3 -> 127.0.0.1:47961` after the guest moved to 47965); the restarted game never got
   it and the join took 60 s. Reverted.
2. **Expire the dead connection early**: when the link ends with *connection reset* (a crash,
   not a network hiccup), lower the live copies of the transport config's idle timeouts
   (25000 / 20000 ms, pattern `0x4d0, 3000, 5000, 6000, 25000, 20000, 200`) to 4 / 3 s for 8 s
   and shorten the hold to 5 s from then. The copies (mapped heap, e.g. `0x110dcbxxxx`) were found
   in 2 of 3 runs by a 35 s background scan of the resident memory, none in the third, and no
   run showed the connection actually expiring early (in the run where the patch came in time the
   25 s timeout ended at the same moment). Kept **off by default** (`BB_PARTY_NR_FAST_EXPIRE=1`).
   The hold is dynamic now: 26 s at the drop, shortened only once such a patch is in.

Result: crash rejoin stays ~30 s after the restart (~34 s after the crash). The floor without
the hold is ~22 s after the crash (boot + own load + a normal join).

## Open

- Finding where the NexusRevolution transport keeps its idle timeout per connection (the config
  built at 0xcdb250 is handed to the transport factory `param_3->vtable[0x10]`) would let the host
  drop one dead connection on a crash (RST) and cut ~4-8 s from C6.
- A lower transport idle timeout (0xcdb309 25000 / 0xcdb314 20000 ms) would end the crash hold
  sooner, and the host would notice a crashed guest sooner too (today ~16 s). Not done: it is also
  the tolerance for real network hiccups.
- Host crash rejoin was not measured; the guest's link reconnect cap is now 2 s.

## 2026-10-11 profile (flowlive, after the pre-ring spacing fix)

`tools/mp/flow_live.py world --count 3` (both start straight in the party game, pre-made
characters, lowest-cost settings, 3 games on the machine), `join_timeline.py`:

| Step | Time |
|---|---|
| guest in world (load done) -> rings | 1.2 s (pre-ring spacing 0.8 s per side try + 0.3 s; was 6.9 s with 30 ticks per try at a low frame rate) |
| rings -> `summon_messenger/create` | 2.0 s (`Party: Fast bell sign`) |
| create -> `guest_invite` | 0.5 s |
| invite -> `JoinRoom` | 0.8 s |
| `JoinRoom` -> client (6) | 1.8 s |
| client (6) -> loading screen | 1.2 s |
| loading the host's world | 4.3 s |
| **guest in world -> in the host's world** | **11.8 s** (50.53 -> 62.30) |

What is left for "instant" joining: the 2 s bell timer (0.5 s broke 2 of 6 joins, above), the
transport's first connect (1.8 s), the guest's prologue / effect (1.2 s) and the load of the host's
world. A guest already in the host's map still loads it (split travel removes same-map loads,
docs/party/split_travel.md when it lands); Continue still loads the guest's own world first (the
sign is put up from there).
