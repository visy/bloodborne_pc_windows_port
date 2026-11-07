# Six players (CL-10e56b): limits, why 5 is the ceiling today, and the design for 6

Verdict: **5 seats (host + 4 remote, invaders included) is the engine's hard ceiling and we can give it to players safely
now. 6 seats is NOT a flag: it needs five fixed-size engine tables extended (about 60-100 patch sites, one of them a
struct layout change), which cannot be done blind. Design below; recommend it as its own item with a probe first.**

"Seat" = host + every remote player the game holds, a phantom or an invader. The game has 5 seats (host + 4 remote). Our
code called it "four game slots" (`kGameSlots = 4`, host + 3), which is why a 5-party can never take an invader today.

## 1. Limits table

| # | Limit | Where | Value | Whose | Evidence | For 5 | For 6 |
|---|---|---|---|---|---|---|---|
| 1 | `Capacity(party_max)` = min(max, 4) | party_invade_rules.h:33,166 | 4 | ours | source; the invader path (HELLO role 1) reaches the world by direct summon, no SOS filter | change 4 -> 5 (one constant) | 6 |
| 2 | `kMaxPartyPlayers` (link slots, VITALS, HUD rows, Party members page, BB_PARTY_MAX clamp) | party_link.h:58, party_fourp.h:44 | 5 | ours | one constant sizes them (four_players.md A7) | none | 6 + menu page (see 6) |
| 3 | Launcher offers 2-4; adopt keys 2-5; ini `party_max` | launcher.py:71,922 | 2-4 | ours | source | add 5 behind a toggle | add 6 |
| 4 | HELLO / WELCOME / admit | party_link.cpp:725,983,1803,1934 | `max_players` 2..kMax, slots 1..max-1 | ours | invaders take link slots too, so they count against max already | none | follows #2 |
| 5 | Matching2 emulation, host room cap, discovery / VPN lobby `max` | np_matching2.cpp:524 (clamp 8), party_host_service.cpp:25 (2..8), party_discovery.cpp:141 (<=8), nwlobby.py:173 (<=8) | 8 | ours | source | none | none |
| 6 | System menu Party members page: 6 visible rows | bbport_game_menu_pages.cpp:253,1253 | 6 rows | ours/game UI | page is at the game's 12-page limit | already shows 5 + 1 (Last event steps aside) | 6 members fill the page, no notices row |
| 7 | Net bandwidth / CPU | network_robustness.md:118,383 | ~3 KB/s per relayed guest, ~180 KB/min UDP per guest | measured | full mesh: 5 seats = 10 pairs, 6 = 15 (each player sends to 5 peers, still < 20 KB/s) | fine | fine; RAM per game unchanged (one PC each); one PC with 5 games = 20 GB WS (fp5 note), 6 games = stress, test only headless |
| 8 | SOS filter literal `coop+inv+other+outgoing < 4` | 0x1874bbe `cmp esi,4` | 4 -> 5 by H6 | game, **already patched** | four_players.md "Five players" | done (H6) | `cmp esi,5` -> 6 (1 byte) |
| 9 | Selector max coop / max inv | sel+0x1fc/0x200 set by ctor 0x1871259/64 (3/3) | 3/3 -> N by H5 | game, **already patched** | four_players.md H5 | done | H5 writes N (field already generic) |
| 10 | SessionSlotSummary 4 records (2 coop + 2 invader) | 0x186fe40 | 2+2 | game, H1 lifts coop | four_players.md #1 | done (H1 reads the game's own count, invaders rec2/rec3 give Max invaders 2) | H1 rule is generic; invader records stay 2 (Max invaders cap 2 holds) |
| 11 | **Presentation object**: 5 slots (stride 0x14 at +0x1c), tracked handles at +0x80..0x8c, tail fields to +0xa8, allocated 0xb0 by the flow ctor (`0x1567864`, ctor 0x15ba630) | 0x15bc590 (loop `4 < i`), 0x15bbc00 (`!= 5`), 0x15bce10 (`!= 0x80`), 0x15bc080, 0x15bcd30 ... about 20 functions 0x15ba630-0x15be047, 50 tail-offset sites in them, plus external readers of +0x90/+0x98/+0xa8 | **5** | game, **hard** | decompile 0x15bc590 / 0x15bbc00 / 0x15bce10 (in this repo's ghidra cache); bbhost `network_script_state.hpp` presentations[5] + tracked_chr_handles[4] at 0x80 | fits | extend to 6: alloc 0xb0 -> 0xc4, shift every tail access +0x14, loop bounds 5 -> 6. Layout change; also the object is serialised in the world-transition snapshot (every peer patched alike) |
| 12 | **Player ChrSet**: capacity 5 (local + 4), alloc 0x118 = 5 x 0x38 | init 0x18da8d0; free-slot scan in 0x18db500 | **5** | game, hard | bbhost `world_chr_man.hpp` (`NATIVE_PLAYER_CHR_SET_CAPACITY = 5`, `ALLOCATION_SIZE = 0x118`) | fits | alloc 0x118 -> 0x150, count 5 -> 6 (few sites, plus the handle table is 0x3e entries, ok) |
| 13 | **GameDataMan remote rows**: 4 rows x 0x6a0, occupancy bytes at `*(GDM+0x18)`, `slot_flags[4]` inline at GDM+0xa9, pointer table of 0x28 session records | rows alloc 0x1a90; readers in 0x178b5e0 case 0xe (slot pick), 0x1e50090 (`slot < 4`), 0x18d8100 alloc, 0x17e46e0 level sync scans all 4 rows, reset / cleanup loops, `SprjSessionMemberSlot::MAX_PLAYER_SLOTS = 4` | **4 remote** | game, hard | bbhost `game_data_man.hpp` (`PLAYER_SLOT_COUNT = 4`), `session_manager.hpp:250`, `world_session_object_man.hpp:131`; decompile 0x1e50090 `uVar2 < 4` | fits (host + 4) | rows alloc + `slot_flags` (inline in a struct: layout change) + every `< 4` check |
| 14 | Matching2 room max the game itself passes (`room+0xc`, from our emulation, literal 5 in the game's room create) | 0x1788ed0 -> 0x1ecfcd0 | 5 | game value, our emulation answers it | four_players.md #6 | ok | our Matching2 reply can say 6; the game's `slots+0x14 < room max` checks then pass |
| 15 | Boss scaling rows 7500..7503 (1.5/2.0/2.5/3.0) | H2-H4 | 3 rows | game data | four_players.md section 4 | H2 clamps 4 cooperators to the 7502 row (fp5 log) | 5 cooperators would map to 7503 (3.0x) - balance call |
| 16 | NPC sign / maiden EMEVD counts (`3[29]` `<2` / `<3`) | E6 + bbhost maiden rewrite | coop < 3, inv < 2 | game data, ours rewrites | four_players.md #10 | ok | counts are `<N` thresholds, generic |
| 17 | Unknown per-player arrays (name-plate pool `FrpgMenuDlgFloatingPCGauge`, per-peer net buffers 0x63F8, Lua `GetWhiteGhostCount`, UI member lists) | not mapped | >= 5 proven by fp5 run | game, unknown for 6 | fp5 smoke | proven | **not proven** - only a probe can show |

Ours-side total for 5 with invaders: item 1 (one constant) plus the launcher choice. For 6: items 2, 3, 6 are small.
Game-side for 6: items 8, 11, 12, 13 (+14 free, 17 unknown).

## 2. Why 6 is not "one more patch"

The engine was built for host + 4 remote and every table that matters is a fixed array whose size is baked in as an
allocation size, a loop bound and a literal compare, with other fields placed right behind it (item 11's tail fields start at
the end of slot 4; item 13's `slot_flags` sits inline in GameDataMan). A 6th remote player (seat 6) therefore hits, in order:
(a) the presentation add (0x15bc590) finds no free slot and returns 0, (b) the player ChrSet has no free index, (c) the
GameDataMan row pick (`slot < 4`) fails. Each is a silent miss, not a crash, until a table is extended half way, and a half
extension corrupts the neighbouring field. I did not patch any of them blind.

## 3. Proposed design for 6 (new item, needs its own agent and a probe)

1. **Probe first (no game files changed)**: BB_PARTY_SIX_PROBE logs, with 5 seats full, what the game does when a 6th member
   joins the room (Matching2 maxSlot 6): which of (a)(b)(c) fires and the callers. One 6-instance run, headless.
2. **Relocate, do not extend in place.** For items 12 and 13 allocate the bigger arrays ourselves in a hook on their
   constructors (alloc size and count are immediates in 0x18da8d0 and the GameDataMan init) and patch the compare literals
   (`cmp ..., 4/5`) found by a capstone scan of exactly the functions named above (the scan list is the work product:
   `tools/re` script, verified byte by byte like the other `PatchBytes`).
3. **Item 11 (layout)**: allocate 0xc4, keep slots 0-4 where they are, put slot 5 at +0x80 (the old tail start) and move the
   tail fields +0x14; patch the ~50 tail-offset sites in the class's functions with a table-driven patcher (old disp -> new
   disp, instruction by instruction, refusing to run if any decode differs). External readers of +0x90/+0x98/+0xa8: found
   by scanning for those displacements next to the object pointer load (`*(flow+0x16f8)`, ~290 byte matches to triage).
4. Every peer must have the same layout (add the set to the party-patch hash, as H1-H6), all behind `BB_PARTY_LARGE` (off).
5. Prove in order: ChrSet + rows with 5 seats (no behaviour change), then the 6th seat, then boss fog and travel.

Risk: high (memory layout of live engine objects, 20+ functions, unknown readers); effort estimate 3-5 agent days; do it
only after the probe shows (a)(b)(c) are the only misses.

## 4. What shipped (0.8.0)

- `Capacity` = min(Max players, 5 seats): a 5-party takes an invader (host + 3 guests + 1 invader); a direct summon counts
  invaders against the seats (party_b2 dsummon HostView).
- Toggle `BB_PARTY_LARGE` / launcher *Large parties* (off by default): without it Max players is held at 4 locally; with it 5.
  A guest always takes the host's value up to 5 (host-owned).
- Smoke `fp5` (with the toggle) and `fpi5` (host + 3 guests + invader); unit tests `party-invade-test`, `party-link-test`.
- 6 stays a documented, designed follow-up (`kMaxPartyPlayers` stays 5; nothing offers 6).

## 5. Proof (0.8.0, six080, 2026-11-07, loopback, 5 games: host + one guest drawn, three headless-lite)

- `fpi5`: three guests in (`cooperators 2 -> 3`), the invader (instance 4) dials at 150 s and is admitted into slot 4 with
  five seats full, told to every guest, never refused for room; it falls 25 s after it is in and goes home (host
  `invasion of slot 4 ends: invader_died`). 19 steps pass. Host and guest screenshots show five hunters, the invader red, four HUD rows.
- `fp5` with `BB_PARTY_LARGE=1`: four title joins, party lamp travel (all four guests in m24_01), Cleric Beast fight (H2 clamp), a
  guest's death and arena revive, boss kill, Leave and Rejoin (4 -> 3 -> 4). 26 steps pass; the drawn windows ran 9-10 FPS (five
  games on one PC, a stress test). Harness fix: fp5's `until` stopped at the travel's new session (`0 -> 3 -> 4`) before the fight.
- Not run: six players (not possible, see above), two invaders into a 5-party (needs a 6th game; unit-tested only).

## 6. Probe and census for 6 seats (six080, round 2, static; no game run)

`python tools/re/six_census.py` (capstone over the 1.09 eboot, addresses only) lists every function that touches the fixed tables.
What it and the decompiles (ghidra cache) show:

| Table | How it is built (evidence) | Sites a 6th remote player touches |
|---|---|---|
| Player ChrSet | ctor 0x18da8d0 allocates 0x118 = 5 x 0x38 and writes count 5; the free-slot scan 0x18db500 reads the count (`*param_1 <= i`), so no other literal | 1 hook (post-ctor: allocate 6 x 0x38 zeroed, count 6). Cheap, safe. |
| GameDataMan remote rows | init 0x14e4e80: `mov esi,0x1a90` at 0x14e5561 (0x10 + 4 x 0x6a0), four unrolled `PlayerGameData` ctor calls (0x14f1510), `[rbx+8] = 4` at 0x14e55c5, occupancy bytes: 4-byte allocation at GDM+0x18 (0x14e55db) | allocation size, count, 5th ctor and occupancy (post-init hook is enough: the rows are fresh) PLUS about 25 loops / checks `< 4` / `3 <` over the rows: 0x131d470, 0x131ddc0 (two), 0x1336260 (two), 0x133aa20, 0x13835f0 (two), 0x13cda50, 0x15077c0, 0x1783160 (three), 0x178b2c0, 0x178b5e0 (two + the slot pick loop at the end of case 0xe), 0x17e46e0 / 0x17e4880 / 0x17e4a20 / 0x17e76b0 / 0x17e8110 (level sync and friends), 0x1906480, 0x18d8100, 0x1e50090 (`uVar2 < 4`), 0x1710a30 (a 0x1a80 byte loop). `GameDataManPlayerSlotFlags[4]` sits inline at GDM+0xa9 (bbhost game_data_man.hpp): a 5th flag would overwrite the next field (needs a relocation or a side table). 46 functions use the 0x6a0 stride in total |
| Presentation object | allocated 0xb0 at 0x1567864 (flow +0x16f8), ctor 0x15ba630; 5 slots at +0x1c (stride 0x14), tracked handles +0x80..0x8c, state / count fields +0x90, +0x94, +0x98, +0x9c, +0xa0, +0xa8 | the class's own ~20 functions (0x15ba630-0x15be047) plus **104 functions that load the object** (census), about 40 of them read the tail field +0x98 ("is presented"), `+0x14` is read in about 50. A 6th slot moves the whole tail by 0x14: every one of those displacements must change in place (same length: disp32), each verified as this object and not another with a field at the same offset |
| Matching2 room | answered by our emulation (maxSlot) | none |
| SOS filter / selector | H5 / H6 | one more byte each (H6 `cmp esi,5` -> 6) |

So the 6-seat patch set is: ChrSet (1), GameDataMan (about 30), presentation (about 70 including the displacement moves),
filter (1), plus whatever the first 6-game run shows among the arrays this census cannot see (name plates, per-peer buffers).
About 100 byte-checked sites. Design: one table-driven patcher (`party_sixp.{h,cpp}`, rules tag `6p:vN`, all behind
`BB_PARTY_LARGE=1` and Max players 6; nothing patched otherwise): a site is (address, expected bytes, new bytes, owner function), applied
with `Matches()` first and refused as a whole if any site differs; the displacement moves are generated by `six_census.py` and
reviewed per function against the decompile, never applied by pattern alone. Stages, each proven with `fp5` / `fpi5` before the next:
(1) ChrSet + room max + filter literals (still 5 players, nothing visible), (2) GameDataMan rows and their `< 4` sites (5 players:
same), (3) presentation tail move (5 players: same), (4) the 6th seat: `fp6` and `fpi6`.

## 7. Where round 2 stopped, and why (read before scheduling the work)

Stages 1-4 are mechanical but about 100 sites across 150 functions, with no way to prove a site except a run with the table full.
A wrong displacement or bound is not a refusal: it is memory corruption in the game's session objects (a neighbouring field
overwritten, an out-of-range row read), and the symptoms show only at the 6th join. A 6-game run needs about 24 GB of working
set (fp5: five games held about 20 GB on this 32 GB PC and drove free memory to 0.4 GB, drawn windows at 9-10 FPS); a sixth game
means paging, so every iteration is long and noisy. I stopped before patching anything: nothing in the repo changes behaviour for 6,
the launcher offers 5 at most, and 5 stays as shipped. Recommended next step: stage 1 and 2 together as one agent day with a
read-only runtime probe first (log the ChrSet count, the GameDataMan row count and the occupancy bytes at the title, to confirm the
offsets above on a live game), then a decision on the presentation tail (70 sites) against its value (a 6th player).
