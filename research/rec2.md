# rec2: boss AI with the host dead (ERSC parity C2), root cause and fix design, 0.8.1

Agent rec2, branch agent/rec2. Static RE on the 1.09 eboot (Ghidra, our offsets = bbhost RVAs - 0x400000), the
party code of 0.8.1, the smoke logs of re081 / amelia2078 and the host log of the 4-PC playtest of 2026-11-01
(player_logs/2026-11-03/NW-80b250f). No game data is quoted; entity ids appear only as examples from logs.

## 0. Short answer

* **Who drives the boss while the host is dead: the host's game, always.** The boss chr's AI runs on the ComManipulator
  of the game whose WorldChrMan owner table holds it (`NpcIns::SetManipulator` 0x18E6540; enemy_ai.md). Every guest's
  game replays the host's NetAI frames. The host's death does not move that ownership and does not pause it: the
  limbo (HostDeathIntercept, party_b2.cpp, plan `BossLimbo`) keeps the host's game in the world with its chr at HP 0,
  so the boss keeps thinking on the host. Nothing in the engine stops or re-homes an enemy's AI when the local
  player dies: the death script (SoloPlayDeath 0x1381B60 -> SoloPlayDeath_2 0x1381DE0) only ends the world with
  the respawn load, which the limbo never lets run.
* **Why the boss then goes idle: it has nobody left to perceive.** The boss's goals follow its AI target list; the
  list is rebuilt every perception tick (0x12AB4C0) from the SprjTargetBankManager vector (`*0x553B098`) through the
  bank search 0x12ADC50 in mode 0, which drops every player chr whose per-chr state byte `[[entry +0x18] +0x30] +0x18`
  has **bit 0 (the dead mark)** set, and the sight filter 0x12AF2F0. Hit records (0x12A9230) and ChangeTarget
  (0x1299CD0 -> 0x12AD1B0) only mark an entry for one tick (0x12B2760 flags |0x80); the list update 0x12B17C0 prunes
  everything not re-marked (0x12B2490). So a boss on the host's game fights exactly the player chrs the host's game
  perceives; with the host's own chr dead, those are the members' copies, and only the copies without the dead mark.
* **The dead mark is the host's copy problem, not the network's.** The bit is the chr's dead state (the Lua
  `IsDisable` 0x1337E70 reads the same bit as "dead", section 3). A member's copy that the party code rebuilds without
  a load (`RewindRemoteTasks`: the task rewound to STEP_Create after a guest's in-place revive, a new look, the host's
  revive, the wipe's re-join) comes up with the bit SET from its first frame although its HP is full, and the step of
  the first join that clears it is not repeated (live amelia2078 / re081 amp: `task mode 1 (step 6) rewound to
  STEP_Create` at 117.478 s, `living target: chr 10044001 (HP 719) ... still had the dead mark (+0x18 bit 0, 01)` at
  117.514 s, 36 ms later; on 0.7.7 the bit stayed set for the whole fight).
  Real sessions differ from the one-PC smokes only in their history: players die, get up without a load, change
  looks, wipe and re-join many times before the host dies in a boss fight, so by then the host's copies of the
  guests are rebuilt copies. The one-PC smokes (hdp, bh, hg, bg) use the first copy of the join, which was cleared.
  The 0.7.8 LivingTargetMarkTick covers it by clearing the bit every tick on every living player chr.

Confidence: high on the mechanism (static, matches the live 0 / 1 readings of amelia2078); medium on "nothing else"
(the clear step of the first join is identified below only by elimination; the three real-session failures each
have a cause that is fixed, none has been re-tested by players).

## 1. Vanilla: the session host dies with phantoms in the world

Static, the death script's native callbacks (SprjLuaEventMan states; names from the string table, bbhost
engine/cs/death_events.hpp credits the same entry points):

| step | offset | what it does |
|---|---|---|
| local player HP 0 | death handler 0x13813D0 | sets event flag 4000 and the death request ([[0x553B108] +0x10] +8 = 4); runs once until a load |
| SoloPlayDeath | 0x1381B60 | emits the dead event 0xFFA with the own chr type (vfunc +0x558) and summon type (+0x598) to the session (the guests' packet 0x0E), then the Lua timer 4000 "SoloPlayDeath_TextWait" (YOU DIED) and, after it, SoloPlayDeath_2 (or SoloPlayDeath_1 when 0x1331DDC0 says the death penalty is skipped) |
| SoloPlayDeath_2 | 0x1381DE0 | `*0x553B0D8 = 1`, clears the GameStateMan transition sections (+0x1690 / +0x16B0), picks the respawn lamp (0x1332BC0), requests the SessionWorldTransition (0x13CDE30: the respawn load), sets the death-pending byte `[0x553B130] +0x70`, commits the bloodstain (0x13D2910) |
| guest side: HostDead state | 0x1381870 | entered by the 0x0E packet (event 4000 on the guest): returns at once when SprjSessionManager +0x124 == 3 or the own chr type is 8; chr type 2 / summon 0x12 (an invader): "DeadInfoMsg_Host" + the PK success text; else flag 4047 (0x132E5A0), the "host died" banner (0x1386960), the Lua timer 4000 "EventMenuBrake" (5 s menu brake) and then HostDead_1 |
| HostDead_1 | 0x1381A60 | `*0x553B0D8 = 2`, GSM +0x1520 = 1, the last-lamp selector 0x1332BC0(-1), the send-home 0x13CDE30 (the phantom goes home: the ~5 s "phantom banish timer" is the EventMenuBrake timer, there is no other) |
| guest's own death | PartyGhostDeath wait 0x1382070 -> PartyGhostDeath_2 0x13824C0 | the summoned player's death: bloodstain for chr types 2 / 12, GSM +0x1520 = 1 and the send-home |

What vanilla does with the boss's targeting at the host's death: **nothing explicit**. Scanned every EMEVD (re081):
no event reacts to the player's death with an AI instruction. The engine side: the host's own chr gets the dead mark
(bit 0 of the state byte, the per-chr object at `[chr +0x3B0] +0xB0`, see section 3) and leaves perception at the
next tick; the boss's hate record of the host (type 3, 10 s, from the hits) decays; the boss's list then holds only
the phantoms it still perceives, and it fights them for the ~5-10 s until the host's respawn load tears the world
down and sends everyone home. Vanilla never has a boss fight with the host dead for longer than its death sequence.
So there is no vanilla "host dead -> end co-op" rule in the AI at all; the AI just loses a target it can no longer
see, as it does when a phantom dies.

A phantom's death in vanilla is the mirror image: its chr (a copy on the host's game) gets the dead mark on the
host's game through the same per-chr object, perception drops it, the boss turns to the host. The engine therefore
already contains everything the party needs; the party's job is to keep the host's copies of the living members in
the perceivable state.

## 2. Our party model: which process simulates the boss, and what the host's death changes

* **Owner**: the host's game (manip Com, owner table 1 on the host; NetAI on every guest; smoke `hg` and `la` probes,
  enemy_ai.md). The lone-guest take-over (party_enemyai.cpp, `b2::MemberApart`) never applies in a boss fight: the
  fight needs the host in the arena.
* **At the host's death in a fight** (HostDeathIntercept, plan `BossLimbo`): the funnel (SoloPlayDeath_2's native
  site) is taken, `g_host_down` / `g_host_limbo` set, SpectateBegin("host"): the host's game stays in the world, its
  chr at HP 0 (the death animation played; the chr is carried onto the followed member for the camera), no load. The
  guests get the 0x0E packet; K12 (HostDeadHook on 0x1381870) returns at once for a party guest, K9 drops the
  send-home at +0x1381A93. So the host's game keeps running the boss's AI, keeps sending NetAI frames, keeps
  applying the guests' damage packets (0x151C9F0 resolves the attacker by handle through WorldChrMan +0x850 and calls
  0x1291BF0 -> the hit record 0x12A9230 on the boss's AI: remote attackers get hate records exactly as the host).
* **Nothing freezes on the host's game**: no death / load state is entered (smoke hdp: Laurence keeps attacking after
  the host's death with every party fix off). The candidates of the brief that are NOT the cause: the owner does not
  change (no ownership transfer, no HP-sync role change); latency only delays the guests' positions and hits; the
  guests do not drive the boss and never need to.

## 3. The dead mark: what it is, who reads it, who writes it

**Where it is.** Every chr's target-bank entry is `[[chr +0x3B0] +0x40] +8` (the bank vector `*0x553B098`, begin +8,
end +0x10; the teardown 0x18BC110 erases it). The bank search 0x12ADC50 reads the entry's `+0x18` object `A` (the
chr's update/enable record: `A +0x10` the update state, `A +0x20` flag bits) and `X = [A +0x30]` (the chr's state
record: `X +0xD`, `X +0x18` flag bytes):

```
include the entry iff  entry vfunc +0x78 (valid)
                   and (A+0x20 & 5) == 0
                   and ((A+0x20 & 2) == 0  or  (X and (X+0x18 & 1)))      <- "disabled unless dead"
                   and A+0x10 == 4                                        <- enabled
                   and (X == 0  or  (X+0xD & 0x10) == 0)
                   and mode 0: (X == 0 or (X+0x18 & 1) == 0)              <- the living (perception uses this)
                       mode 1: X and (X+0x18 & 1)                         <- the dead only
                       mode 2: both
```

**Proof that bit 0 of `X +0x18` is the dead bit** (static): the Lua `IsDisable(entity)` 0x1337E70 returns
"disabled" when `A+0x20 & 1`, or when `A+0x20 & 2` and NOT `(X+0x18 & 1)`, or when `A+0x10 != 4`: a chr flagged
"disabled while alive" (bit 1) counts as enabled once this bit is set, i.e. once it is dead (a corpse). Live
(amelia2078 and re081 amp, BossTargetDump's `o18` column): the host's own entry goes `00 -> 01` at its death; a member's
rebuilt copy reads `01` from its first frame while its HP is 719; the first-join copy reads `00`.

**Who sets it.** Not a direct `OR byte [..+0x18],1` on `X` anywhere in the chr code: the only such writes in the game
range are the chr init virtual `+0x60` (ChrIns 0x18BB670; NpcIns 0x18E5610 and PlayerIns 0x18F3D50 end in it) and the
four module factories (0x18D6310 / 0x18E7E80 / 0x1906ED0 inline, 0x1A5BCA0 / 0x1A5BD00 / 0x1A5BD60), and all of
them write the `+0x18` byte of the 0x20-byte module at `[chr +0x3B0] +0xB0` (vtable 0x5398A50 / 0x5335780 /
0x53357B0: +0 vtable, +8 chr, +0x10 = -1, +0x18 flags), not `X` (whose `+0xD` byte is tested too, so `X` is a larger
record). The death write and the clear are therefore reached through a computed address or a register copy; the
instruction-pattern searches (direct OR / AND, read-modify-write of a `+0x18` byte, dword / BTR forms) did not find
them in the 0x12xxxxx-0x20xxxxx range. Open: the exact clear site. What is established without it:

* a chr that was never "made alive" by the join / load flow keeps bit 0 set (the rebuilt copies);
* nothing in STEP_Create 0x1E50090, STEP_CreateWait 0x1E505E0, STEP_FirstSyncCompleteWait 0x1E4FF70 or
  InGameStart 0x193A3E0's per-player step (draw bit + vfunc +0x610(1)) clears it: `RewindRemoteTasks` re-runs
  STEP_Create and `ShowLikeInGameStart` re-does InGameStart's step, and the rebuilt copy still reads `01` (0.7.7
  amp, where both ran and the bit stayed);
* the member's first copy of a join reads `00`, so the clear happens in the join's first world set-up (the load's
  chr build, before STEP_Create), which the rewind skips by design.

A live write-watch on `X +0x18` of a guest's copy during a first join (a page guard or a per-frame diff with the
call stack) would name the function in one run; it is the next step if anyone needs the vanilla site rather than
the one-shot clear of section 5.

## 4. Why the real sessions failed and the one-PC smokes did not

The three real failures each have a found cause, all on the host's game, none in the network:

| session | party code | what the boss lost | cause (host's game) | fix |
|---|---|---|---|---|
| 0.7.6 week reports (NW 0.2.3) | 0.7.4 | every target: idle beside the living guest | the dead host chr (and dead guests' copies) stayed in the target bank with no HP check (0x12ADC50 has none), nearest to the boss, carried by the spectate onto the followed guest; the boss "fought" the corpse; a guest further away was never taken (the search keeps the nearest) | 0.7.7 DeadPlayerUntargetTick + BossAttentionTick, 0.7.8 BossLockEntry (hate record) |
| 4-PC playtest 2026-11-01, Amelia try 2 | 0.7.7 | every guest: she walked and animated but attacked nobody once the host was down | the guests' copies on the host were **rebuilt copies** (host log: 21 `rewound to STEP_Create` lines between the wipe and the second try; 251 in the session) and a rebuilt copy is born with the dead mark (section 3): perception never listed them; while the host lived his own (first-join, cleared) entry fed the fight | 0.7.8 LivingTargetMarkTick (per-tick clear) |
| 4-PC playtest, the other 7 host deaths | 0.7.7 | nothing: the limbo and the turning worked, 3 bosses died with the host down | | |

Why loopback hid them: the smokes start from a fresh join, so the host's copies of the guests are first-join copies
(cleared), the scripted guest stands on the corpse's spot (the carried host) and takes the blows aimed at it, and
nobody died, changed looks or wiped before the host's death. The amp phase (amelia2078) was the first to script the
real-session history (a wipe and a re-join) and reproduced the second failure on one PC at once; the hdp phase of
re081 cannot tell the fixes apart because Laurence keeps swinging over the carried corpse.

Candidates of the brief that are ruled out (static + logs): chr ownership timing / latency (ownership never
changes; the 4-PC host log shows the boss HP falling from the guests' hits throughout the idle fight, so the damage
packets, the hit records and the owner's AI all ran); remote hate entries with remote handles (0x151C9F0 resolves
the attacker by handle through WorldChrMan +0x850 and 0x12A9230 keys the record by the chr handle `+0x50`: the same
path for remote and local attackers); the boss AI run by someone else (never); HP sync / ownership transfer (none);
the host's game in a death or load state (the limbo keeps it in the world; hdp shows the boss acting after the death
with every fix off).

## 5. Fix design

The boss must be driven by the host's game as now; what has to be right is the host's view of the living members.
Three things, in order of value:

1. **Clear the dead mark once, where the copy is rebuilt** (root-cause fix of the second failure instead of the
   per-tick patch): in `RewindRemoteTasks` / the `g_rewound` follow-up (party_b2.cpp ~1280-1300, where
   `ShowLikeInGameStart` already gives a rebuilt chr InGameStart's two steps), once the rebuilt chr exists and its HP
   is above 0: `X = [[chr +0x18] +0x30]`, `X +0x18 &= ~1` (and log it with the value seen). Same on the peers' copy
   re-creation after a member's in-place revive (`RecreateMemberNear`, "member revived"). Keep LivingTargetMarkTick
   as the safety net; its line `still had the dead mark` then becomes a diagnostic that something else set it.
2. **Perception diagnostics in the limbo** (host, `BB_PARTY_AI_LOG=1` or always at a low rate): every 5 s while
   `g_host_limbo` and a boss bar is up, for each living member copy the bank-search view of 0x12ADC50:
   `A = [chr +0x18]`: `A +0x10` (must be 4), `A +0x20` (bits 1 / 4 exclude), `X +0x18` bit 0 (dead), `X +0xD` bit
   0x10, the team byte, and whether the boss's list has a kind-0 (perceived) entry for it; one warning line
   `boss <h> perceives none of N living members for 5 s (host dead)` when the list holds no player entry. This is
   what a real-session report needs: the first failure's idle and the second one's "born dead" look the same on a
   screenshot and different in this line.
3. **No ownership change, no guest-driven AI.** Transferring the boss to a guest would need its chr, AI, hit
   bodies, HP and the fight's EMEVD events moved (host-only copies, split_travel.md): out of scope and not needed.

Exact writes (host game, main thread): `chr +0x18 -> +0x30 -> +0x18 bit 0 := 0` for a living member copy
(what LivingTargetMarkTick writes now, once at the rebuild instead of every tick). Nothing else is written; the
0.7.7 / 0.7.8 ticks stay as they are.

## 6. Smoke spec: `hdr` (group `boss_host_dead_rebuilt`), the real-session shape on one PC

Reproduces the second failure's precondition without a wipe (cheaper than amp, no fixture): the guest dies ONCE
outside the fight and gets up without a load, so the host's copy of it is a rebuilt one (`rewound to STEP_Create`),
then the host dies in a boss fight and the guest stands passive and mortal out of the boss's reach.

* Players: 2 (host + 1 guest), Cleric Beast (BA / bh setup: lamp 2412952, `CLERIC_TRIGGER_HOST`, `BB_PARTY_PROGRESS_TEST_CLEAR=CLERIC_CLEAR`),
  `BB_PARTY_ROT_TEST_FRAIL=1` on the host, `BB_PARTY_TARGET_ANIM=1`, `BB_PARTY_AI_LOG=1`, `BB_BOSS_LOG=1`.
* Director, guest: `guest_die@20%m24_01` at the lamp (its in-place revive: `died alone: revived at its own last
  lamp ... no load`; the host logs `member revived: task mode 1 (step 6) rewound to STEP_Create` and
  `remote chr ... shown as InGameStart does (re-created by a rewind)`), then `guest_press=0x0@25%m24_01`,
  `guest_move=CLERIC_SPOT_GUEST@1%m24_01` (outside the fog, as bh), `guest_chr=2410800:8@3%m24_01?host=spec` (8 m from the
  boss once the host spectates), passive (`guest_press=0x0@90%m24_01`), `god_bar=20` (mortal 20 s after the bar).
* Director, host: as bh (`host_warp=2412952@8^1`, `host_move=CLERIC_TRIGGER_HOST@45%m24_01` (after the guest's revive),
  `host_chr=2410800:2@10%m24_01`, `host_die@3%m24_01`).
* Pass: on the host, after `host death in a boss fight`, within 60 s: `Party balance: hit: HP` on the guest's game or
  the guest's `local death: HP 0`, AND the host's `boss target dump` shows a kind-0 (perceived) entry for the guest's
  handle, AND no `boss state: ... may be stuck` line. Fail otherwise.
* A/B that proves the cause: `BB_HDR_ENV="BB_PARTY_LIVING_TARGET=0"` (the per-tick clear off) must FAIL on current
  master (the rebuilt copy keeps the dead mark; the boss idles or stays on the corpse's spot; amp's shape) and PASS
  with fix 1 (the one-shot clear at the rebuild). With master defaults the phase passes (LivingTargetMarkTick covers
  it): the row's remaining gap on master is proof, not a known defect.
* Variants for the real-session conditions, same pass rule: `BB_HDR_NET=mobile` / `bad` (`BB_NET_SIM` presets, the
  guest's revive and the boss turn under 150-300 ms and loss), `BB_HDR_LOOKS=1` (the guest changes armor at the
  lamp, `armor=SET`: a rebuild by a new look instead of a death), and the 3-player form (`fp`-style third instance,
  two guests revived once each, the host dead: the boss must hit both in turn, BossAttentionTick's 8 s laps).

Expected run time ~5 min per variant; 2 instances (3 for the 3-player form) through the game queue with
`BB_MP_LEASE=1500`.
