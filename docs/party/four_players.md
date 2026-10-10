# Four-player co-op (host + 3 cooperators) — RE result (Bloodborne 1.09)

Addresses are **our offsets** (raw ELF VA of `out/eboot.elf`; bbhost = ours + 0x400000; bbhost
`.hpp` `Rva{}` = ours). Confidence: **C** confirmed in disassembly/decompile, **L** likely, **G** guess.
Tools: `tools/re/decomp.py`, `tools/re/refs.py`, capstone (`scratchpad/re/bb.py`), EMEVD/EMEDF and
LuaPlus parsers (bbhost `tools/luabnd.py`, `tools/bbparam.py`) over the 1.09 dump.

## Verdict: **feasible** (cap = 3 cooperators, then at most 1 invader)

Every engine table is already sized for host + 4 remote players, and the summon selector's own
"max cooperators" field is **3** in vanilla. The only thing that holds co-op at 2 is a 4-slot
"session slot summary" with two cooperator slots (0x186fe40) and its readers. One post-hook on
that function lifts the cap to 3; the selector then stops at 3 by itself. Boss scaling needs two
small hooks plus a 1-byte patch (unused SpEffect rows 7502/7503 already exist in the params).
A 4th cooperator is **not** possible without rewriting fixed-size tables (5-slot presentation
table, 4 GameDataMan rows, player ChrSet capacity 5, Matching2 room size 5).

## 1. Every cap on cooperators

| # | Where | What | Value (vanilla) | Conf. |
|---|---|---|---|---|
| 1 | **0x186fe40** SessionSlotSummary(out, selector, debug) | Builds 4 records `{kind, status, -1}` at out+0/+0xc/+0x18/+0x24 (status at **out+4, +0x10, +0x1c, +0x28**). Init status 3 (unavailable); 0 free, 2 used. Cooperators (members counted by `0x18796c0(sel, kind)` for kinds 7, 1, 0x15/0x16/0x17) fill **rec1 (out+0x10)** then **rec0 (out+4)**; invaders (8, 0x12) fill rec3 (out+0x28) then rec2 (out+0x1c). Byte out+0x33 = total counted. | **2 coop slots** | C |
| 2 | **0x18708f0** IsKindSlotFree(summary, kind) | For coop kinds (7, 0x13, 0x15–0x1a, 0x1d–0x21): `if (out+4 != 0 && out+0x10 != 0) return 0`; invader kinds (8, 0x12, 0x1c) test out+0x1c/+0x28. Callers: 0x1872775 (sign selection state 0x1872360, bbhost sub_1c72360) and 0x1877052 (world sign display 0x1876f30). | from #1 | C |
| 3 | **0x1878d90** create summon/sign request | Calls #1 (0x187935e); if both coop slots used clears capability bits `0x80 / 0x80000 / 0xe00000` of the kind's mask (desc table +4), both invader slots → `0x100 / 0x40000`; then runs the filter #4 with `param3=1`. | from #1 | C |
| 4 | **0x1874710** SOS filter (bbhost sub_1c74710) | (a) `slots+0x14 (all members incl. host) < room max *(0x553d6d0)+0xc`; (b) `coop+inv+other (slots+8/+0xc/+0x10) + outgoing (sel+0x1c8) < 4` (literal 4); (c) desc team (0x553d760+kind*0x80) == 1: reject if `sel+0x1fc <= pending_coop + slots+8` (cmp at **0x1874c2b** `3b b2 fc 01 00 00`, `jge` 0x1874c31); team 2: `pending_inv + slots+0xc < sel+0x200`. | room max **5**, guests < **4**, max coop **3**, max inv **3** | C |
| 5 | **0x1870c50** selector ctor (`*(*(SprjEventMan 0x553b108+0x60)+0x30)`) | `0x1871259: 41 c7 86 fc 01 00 00 03 00 00 00` (max coop = 3), `0x1871264: 41 c7 86 00 02 00 00 03 00 00 00` (max inv = 3). **No other writer** anywhere (all +0x1fc/+0x200 accesses reached from SprjEventMan scanned: only reads at 0x1874c2b, 0x1876b10, 0x18c5f55/0x18c5fbc, 0x1e4119c/0x1e417e9). | 3 / 3 | C |
| 6 | **0x1876060** selector update | Starts a session only while `slots+0x18 (connecting) < room max`; at 0x1876b10 `cmp ecx,[r13+0x1fc]; jl` stops inviting once `pending+coop >= 3`; team 0xc: `room max - 1 <= pending + slots+0x10`. Room created by 0x1788ed0 → `0x1ecfcd0(..., SprjSessionManager, room+0xc)` = Matching2 maxSlot (5, bbhost log). | 3 / 5 | C |
| 7 | **0x15bc590** add presentation member | `slots+0x14 < room max`, 5 slots (`+0x1c + i*0x14`, i ≤ 4). Recount 0x15bce10: +8 = members with ChrType (ChrIns+0x78) 1, +0xc ChrType 2, +0x10 ChrType 12, +0x14 all (host = ChrType 0, counted only in +0x14). | 5 | C |
| 8 | **0x1e946e0** `summon_messenger/get` builder | `CoopOrNaturalEnemyRecruitNum = 2 - count(0x15)-count(0x16)-count(0x17)-count(7)`: **0x1e97770 `b9 02 00 00 00`** (mov ecx,2). Only told to the server; our in-process FromApi ignores it. | 2 | C |
| 9 | Lua `SetPartyRestrictNum(4)` in `eventcommon.luabnd`/`global_event.lua` `g_Initialize` (pc 565) | Binding registered at 0x1341a7c (struct 0x5548f60, vtable 0x531f530) → native **0x1334e30 = `ret`** (DS1 relic, no-op). No map script calls it. | none | C |
| 10 | EMEVD `3[29]` "client count by type" (0x14510a0 / 0x1450fe0 → 0x15bdc20 coop {ChrType-invade types 1,5,7,19} / 0x15bde10 invaders {8}) | Coop uses: `01000302`/`02000302` (**coop < 2**) in the NPC-summon sign events xx04400–xx04406 (22 events: m23, m24_00/01/02, m27, m28, m29 12906962, m32, m34 ×6, m35) gated on goods 4312 + multiplayer state; common 9181 (`>=1` → flag 9185); 9220 / xx04700 (`>=1`, Insight/maiden). Invader uses: maidens xx04710/xx04720 (bbhost five-players rewrite). | NPC signs hidden at 2 coop | C |
| 11 | EMEVD `1003[109]` "label jump by co-op client count" (handler 0x17bedd0 case 0x6d, count call **0x17bf187 `e8 94 ea df ff`** → 0x15bdc20) | 28 boss events (xx04802/xx04702/…): `01000000` (==0 → label 1, no doping), `02000100` (==1 → 7500), `03000200` (==2 → 7501). **3 cooperators fall through to label 1 = no HP scaling.** | 0/1/2 only | C |
| 12 | Native Lua_MultiDoping **0x138bc70** (bbhost: compiled callback; bytecode twin in global_event.lua) | `0x138bcbf: 83 f8 02 75 32` (==2 → `mov esi,0x1d4d` 0x138bcef = 7501), `0x138bcf6: 83 f8 01 75 54` (==1 → 7500 at 0x138bd22); 3+ → nothing. Used by EMEVD `2009[4]` (m29 chalice, 6 sites) and Lua. | 0/1/2 only | C |

Other limits that are **not** cooperator caps but bound the total: GameDataMan 4 remote rows
(`*(GameDataMan+0x10) + slot*0x6a0`, alloc 0x1a90), presentation table 5 slots, tracked handles 4
(slots+0x80..0x8c, 0x15bcd30), player ChrSet capacity 5 (WorldChrMan, bbhost), Matching2 room 5,
"guests < 4" in the filter. Host + 3 coop + 1 invader = 5 = every table full.

## 2. What a 3rd cooperator uses

- **Presentation slot**: next free of the 5 (`flow 0x5556678 → +0x16f8`, `+0x1c + i*0x14`:
  handle, member type 0 Local/1 Net/2 AI, lifecycle state, init/end flags); host is one of them.
- **GameDataMan remote row**: one of 4 (`slot*0x6a0`, occupancy bytes at `*(GameDataMan+0x18)`),
  STEP_Create 0x1e50090 also checks `slots+0x14 < room max`.
- **PlayerIns**: player ChrSet index 1..4; `PlayerIns+0x3b8` player number (0x18f1700),
  `+0x3c0` game data row, `+0x3d8` peer ObjectRef. Handles resolve through WorldChrMan+0x850.
- **ChrType** (ChrIns+0x78) **1** (white phantom), **team** (ChrIns+0x88) from the session
  descriptor table 0x553d750 (0x22 × 0x80; +0x10 category 1 = coop, 2 = invader, 0xc = other;
  +0x4 capability mask; +0x15 invade type), invade type (vfunc +0x558) 7 = NormalCoop (1/5/19
  other friendly kinds). Same descriptor as the 1st/2nd cooperator.
- **Phantom look**: set in 0x15bc590 for ChrType 1: `phantom id 0x1b27` (6951), or `0x1b28` when
  vfunc +0x598 returns 3 — identical for every cooperator, so no new colour is needed.
- **HP rule**: SpEffect 9006/9026 (maxHpRate 0.7) as for any phantom; SpEffect 9200 (0x23f0) level
  sync in 0x17e46e0 scans all 4 rows + 0x28 session records.
- **Network**: Matching2 member id (room of 5), vport P2P framing per member — PartyLink already
  relays N guests. Name plates/"summoned" messages come from the descriptor message ids
  (+0x28/+0x2c/+0x34) per member — nothing per-slot-index found. Lua `GetWhiteGhostCount`
  (0x1337970) returns slots+8 (will report 3).

## 3. What bbhost changed for "five players"

Not more cooperators: **host + 2 cooperators + 2 invaders** in the late areas. `src/engine/
five_players.cpp` + `maiden_events.cpp` rewrite the dump's EMEVD (DCX→EMEVD→DCX, SHA-256 in/out
checked, served from an overlay root `<data>/bbhost/invasion-assets`): in the Chime Maiden events
xx04710 (ring) / xx04720 (stop) of m26, m33, m34, m35, m36 the `3[29]` argument
`[group][client type 1 = invader][cmp][n]` goes `01 01 00 00` (==0) → `01 01 03 02` (<2) and
`ff 01 04 01` (>=1) → `ff 01 04 02` (>=2), plus two entity/flag typo fixes (m34 3400701→3400791,
m35 12604712→13504712). Every peer needs it. No code patch; the selector's invader max is already
3 and the chalice m29 12907400 already uses `<2`. Room maxSlot stays 5.

## 4. Enemy scaling

- **Normal enemies**: none for co-op (0x17e46e0 only multiplies SpEffect maxHpRate, NG+ table
  0x4731ed0 by ClearCount, and the 9200 level-sync).
- **Bosses** (28 EMEVD events, `1003[109]` → `2004[8]` SetSpEffect + `2004[53]` HP refresh) and
  chalice/Lua bodies (`2009[4]` / `Lua_MultiDoping`, refresh key 4070 `ForceUpdate`):

| SpEffect | maxHpRate | status resist rate | superArmor | used |
|---|---|---|---|---|
| 7500 | 1.5 | 1.5 | +30 | 1 cooperator |
| 7501 | 2.0 | 2.0 | +60 | 2 cooperators |
| **7502** | **2.5** | 2.5 | +90 | unused, ready for 3 |
| 7503 | 3.0 | 3.0 | +120 | unused |

Extension: count 3 → 7502 (see hooks H2–H4).

## 5. Patch / hook list for "max cooperators = 3" (switch `BB_PARTY_MAX_COOP=3`, ini `party.max_coop`)

All with `coop::` helpers (coop_hooks.h), installed in bbgpu_patch_image only when the switch is
3; every peer must run the same set (add it to the PartyLink patch hash).

| Id | Site | Verify bytes | Change |
|---|---|---|---|
| **H1** (required) | 0x186fe40 SessionSlotSummary | `55 48 89 e5 41 57 41 56 41 55 41 54 53` (13, PIC) | `ReplacePrologue`: call original(out, sel, dbg); then if `sel && out+4 == 2` and `Σ 0x18796c0(sel,k), k∈{7,1,0x15,0x16,0x17} < 3` → `*(int*)(out+4) = 0`. Leaves status 3 (blocked area / client / not host) alone. Fixes #2, #3 and every other reader (0x130b960, 0x157f200, 0x15800f0, 0x17b21a0, 0x1872360, 0x1876f30, 0x1879b80). The filter (#4c, max 3) then stops a 4th. |
| H2 (scaling) | 0x17bf187 (1003[109] count) | `e8 94 ea df ff` (call 0x15bdc20) | `HookCallSite` → `min(0x15bdc20(slots), 2)`, so 3 cooperators take the 7501 branch. |
| H3 (scaling) | 0x138bcc2 (native MultiDoping) | `83 f8 02 75 32` at 0x138bcbf | `75` → `7c` (jne → jl): count ≥ 2 → 7501 branch. |
| H4 (scaling) | 0x18c6db0 ApplySpEffect wrapper (vfunc +0x3f0 for chr; MultiDoping calls `[r10+0x3f0]` at 0x138bd48 — C; 2004[8] (0x17ba6a0 case 8 → LAB_017bcac8) goes through the same vfunc — L, check before relying on it) | `55 48 89 e5 41 57 41 56 41 55 41 54 53` | asm stub: `cmp esi,7501; jne orig;` save regs + xmm0–4 (stack arg untouched), `call coop_count_ge3` (0x15bdc20 ≥ 3), restore, `mov esi,7502`, `jmp orig`. Optional: without H4, 3 coop get 2.0×. |
| P5 (optional) | 0x1e97770 | `b9 02 00 00 00` | `b9 03 00 00 00` (RecruitNum; cosmetic for serverless). |
| E6 (optional) | EMEVD overlay (bbhost five-players method) | `3[29]` `01000302`/`02000302` in the 22 NPC-sign events | `…0303` (coop < 3) so NPC summon signs still show with 2 players. |
| — | 0x1871259 / 0x1871264 | `41 c7 86 fc 01 00 00 03 00 00 00` / `…00 02 00 00 03…` | **no change** (already 3). For a vanilla-2 build with H1 on, write `02` here instead of hooking. |
| — | 0x1874c2b, room max, "<4" | as above | no change. |

Byte verification above is from the 1.09 eboot (capstone over `out/eboot.elf`); `Matches()` must
guard each write.

## 6. Risks

- **Desync**: boss HP scaling and EMEVD branches run on every peer; a guest counts itself (state 6
  path of 0x15bdc20) so all peers see 3 — only if all run H2–H4. Mixed builds → different boss HP.
  Enforce via the PartyLink HELLO patch hash. Unknown: any EMEVD that keys on "exactly 2
  cooperators" beyond the scanned 3[29]/1003[9,10,14,15,16,109] set (scan found none).
- **Invasions**: with 3 cooperators only 1 invader fits (room 5, "<4 guests", ChrSet 5). bbhost's
  five-player maiden rewrite becomes "up to 2 invaders while free slots allow" — fine, the filter
  refuses the 5th guest. Hunter of Hunters/Bounty (kinds 0x12/0x15–0x17) share the coop slots
  for 0x15–0x17: H1's count includes them.
- **HUD / name plates**: per-ChrIns floating plates; no fixed 2-entry party HUD found, but untested
  with 3 (bbhost notes plate positioning at 720p). Join/leave text uses descriptor messages.
- **Lock-on / targeting**: phantoms are not lock-on targets of each other; enemy AI target
  selection over more players is untested (L: generic).
- **Boss arenas / fog**: fog-entry and boss-start events are per-player and count-agnostic in the
  scanned EMEVD; Lua `Failed_BossAreaMission` paths unchanged. The 2.0× vs 2.5× choice is balance.
- **NPC summons** (2003[51], ChrType 1) take a coop slot: with 2 players + NPC, a 3rd player can
  still join (cap 3 counts NPCs).
- **Performance / bandwidth**: 5-peer full mesh already exists in chalice dungeons (2 invaders).
- **Lua bytecode** `Lua_MultiDoping` in global_event.lua has the same ==1/==2 branches; if the
  engine ever runs the bytecode instead of 0x138bc70 (L: it runs the native one), H3 does not
  cover it — H4 still turns 7501 into 7502 but 3 coop would hit "No Doping".
