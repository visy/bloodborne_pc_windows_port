# 4-player playtest of 2026-11-01 (report NW-80b250f): the 11 bugs as a list

Written for 0.8.1 by agent bugs081, read-only analysis of the four session logs plus the code on master `cd92169a`.
No player names or addresses in this file. Nothing was written to Iosefka: the proposed item texts are at the end,
for the owner to approve.

Logs (all under `player_logs/2026-11-03/NW-80b250f/x/NW-80b250f/logs/`; the host's long log is H, the three guests are
G1, G2, G3; line numbers are in the file as stored):

| tag | file | role | lines |
|---|---|---|---|
| H | `463d1c2c_last_session24.log` | host (the main session) | 251 490 |
| H2 | `433f1498_last_session.log` | host (the shorter restart after the crash) | 75 718 |
| G1 | `abf8e0a4_last_session.log` | guest | 258 951 |
| G2 | `last_session.7z/last_session.log` | guest | 233 971 |
| G3 | `last_session.zip/last_session.log` | guest | 233 139 |

All four ran the Nightwatch Co-op 0.2.6 beta 4 build with our 0.7.7 party code. The report's wording is the reporter's
bullet list in `bug.json`; the Iosefka item ids (CL-...) come from the owner's note on the bug (2026-11-01 15:46).

## Summary

| # | Symptom (one line) | Iosefka item | State on master (0.8.0 + 0.8.1 branch) | Priority now |
|---|---|---|---|---|
| 1 | DLC could be entered right after the Blood-starved Beast | CL-7683a1 | fixed 0.7.8, smoke `sgb2`, retest pending | P3 (retest) |
| 2 | Character creation: glasses and preview glitch, menu stops responding | CL-8567d0 (existing) | open | P2 |
| 3 | Chalice items (Chikage, Plague Doctor Mask ...) in the shop before any ritual | CL-9bdbdc | fixed 0.7.8 (+0.7.9 follow-up), smoke `sgb`, `sgb2`, `sgb3`, `sgb4` | P3 (retest) |
| 4 | Gehrman greets a guest with his first dialogue before the host talked to him | CL-f572a3 | fixed 0.7.8, retest pending | P3 |
| 5 | A guest's gaol spawn was the Cathedral Ward lamp, before any gaol lamp was lit | CL-00a95e | fixed 0.7.8 (`BB_PARTY_GAOL_ALWAYS`), smoke `kg` | P3 |
| 6 | Key item picked up by a guest reached the host twice (second-item warning, ground copy stays) | CL-60f2d5 | fixed 0.7.8, smoke (Orphanage Key lot) | P3 |
| 7 | Material from a special monster: a guest far away saw nothing but got the drop | CL-a6a2a7 | fixed 0.7.8 (`BB_PARTY_FAR_DROP`), smoke `fdr` | P3 |
| 8 | Memory Altar usable by guests before the host could unlock it | CL-ee41ff | fixed 0.7.8 (`BB_PARTY_ALTAR_GATE`), smoke | P3 |
| 9 | Gaol message ("woke in a cell") shown again on a later grab though the gaol lamp was a target already | CL-11e308 | fixed 0.7.8 (waking scene once per character), smoke `kg` | P3 |
| 10 | Cathedral Ward side-door lift out of sync for the host until a guest rode it | CL-90785a | fixed 0.7.8, smoke | P3 |
| 11 | Vicar Amelia does nothing after half health (second try after a wipe) | CL-2c1eef | fixed 0.7.8 (stale dead mark after a wipe), smoke `amp`, retest pending | P2 (retest, was the worst) |

Only #2 is open. Every other row is fixed and carries a CHANGES entry; none was re-tested by the players
(`docs/party/ERSC_PARITY.md` 2026-11-02 entry). The priorities above are for what is left to do: a retest asked of the
reporter for the P2/P3 fixed rows, work for #2.

## The bugs

### 1. DLC open after the Blood-starved Beast (CL-7683a1)
- Evidence (13401000 is the DLC-entered flag, `kFlagDlcEntered` in `party_items.cpp`): H 88032 `host encounter / story Insight: event 13401000 starts 9350 ...` then H 88140 `host flag 13401000 = 1
  (world_state)` (the Amygdala grab ran for the party's DLC entry flag); G1 87685, G2 82115, G3 71931 each log the same event
  as `+2 here (mode parity)`: the guests took the host's entry as their own progress.
- Suspected code path: the Old Hunters entry is event 13401000 (Amygdala's statue grab). A guest that held the Eye of a
  Blood-drunk Hunter used the statue in the host's world and the host's copy of the event ran too. `party_items.cpp`
  (`EyeToRepair`, the Eye sale gates 12101100/12101101, `kFlagNight`), `party_guestsvc.cpp` item-gate events
  (`PerPlayerItemGateEvent`).
- Iosefka: CL-7683a1. Fixed 0.7.8 (CHANGES "the Old Hunters DLC could be entered right after the Blood-starved Beast"):
  each player's grab is its own, the Eye is not handed out before the own night, saves are repaired once
  (`BB_PARTY_EYE_REPAIR=0`).
- Priority: P3. Ask the reporter to retest: a fresh guest with the Eye before the Beast's death must not get the DLC for the host.

### 2. Character creation glasses / preview glitch (CL-8567d0, existing)
- Evidence: the report is the only source ("glitches and stops the menu interactions there"); no log line of the glitch
  exists. All four logs show `Engine: Dream mirror enabled: native appearance editor and ChrMake_BG preview` (H 259,
  H 188385, G1 265) so the creation screen ran through the engine's mirror/preview assets, not the retail path.
- Suspected code path: the appearance editor and the ChrMake_BG preview (`scripts/dream_mirror.py`, the engine's dream-mirror
  assets; the Nightwatch build ships its own copy of that engine part). The glasses entry is one of the character
  creation face-accessory rows; the preview model draws with a stale or invisible accessory slot and the menu focus is
  lost. No bbport source mentions glasses, so this is RE work (which row/param id, which preview slot).
- Iosefka: CL-8567d0 stays the item; nothing new.
- Priority: P2 (a first-run experience bug, but cosmetic and without data loss; needs RE, not a quick fix).

### 3. Chalice items in the Messenger shop before any ritual (CL-9bdbdc)
- Evidence: H 33656-33658 `host flag 12101007/12101006/12101005 = 1 (npc_quest)`: the badge row flags 12101000-12101009
  were shared between players as NPC quest progress; the Messenger then sold the badge weapons and masks (Chikage,
  Plague Doctor Mask, ...) to players who held no badge. The host's rows opened one second after a guest with seven
  badges arrived.
- Suspected code path: m21 event 12101000 (one slot per badge) ran from another player's state (EMEVD sync packet 0x31)
  and the flags were then shared as `npc_quest`. `party_guestsvc.cpp` ~1622-1660 (`PerPlayerItemGateEvent`),
  `party_progress.cpp` `kBadgeRowFirst` writers, `party_items.cpp` `RepairBadgeGatesTick`, `BadgeGuardTick`,
  `ShopGateTick`.
- Iosefka: CL-9bdbdc (fixed 0.7.8, repaired on load), plus the same family: CL-3b4da5 (0.7.9, a badge got while summoned
  opens its rows) and NW-2ed6d82 (a guest cannot buy; see the Messenger section of the 0.8.1 notes: it is the 0.7.9 fix).
- Priority: P3. Four smoke phases cover it (`sgb`, `sgb2`, `sgb3`, `sgb4` + the control `sgb4n`).

### 4. Gehrman greets guests with the first dialogue (CL-f572a3)
- Evidence: G1 30777 `Gehrman (t210305): a guest may talk to him (his ordinary lines) (flags 1028 0, 1029 0, 1030 0)`:
  the guest's own talk progress is read from the guest's own flags, which were at the start; H 34503 `npc_talk ... talk
  1500050 ... npc 210305 (Gehrman) ... share` shows the host's talk being shared as a line only.
- Suspected code path: the talk progress flags 72100120-129 and 72100133-135 were per player; the progress sync
  (`party_progress.cpp`, world_state set) skipped them. `party_guestsvc.cpp` Gehrman services.
- Iosefka: CL-f572a3. Fixed 0.7.8 (`BB_PARTY_GEHRMAN_SYNC`). Ending choice stays the host's.
- Priority: P3.

### 5. Gaol spawn at the Cathedral Ward lamp (CL-00a95e)
- Evidence: G1 155058 `kidnap: a Snatcher grabbed this guest (event 12400780, m24_00_00_00): 9420 not set here; asked the
  host to take the party to the Hypogean Gaol` and G1 155061 `the party's kidnap travel to the Gaol (lamp 2802959, m28)`;
  G1 215971 shows a second grab: the game's own gaol event is finished after the first, so the later grab fell back to the
  guest's last rested lamp (Cathedral Ward) while the party stayed.
- Suspected code path: `party_b2.cpp` kidnap event rewrite (12700500 / 12400780 gate 0 -> 1, repeated 22 times in G1),
  `party_kidnap_rules.h`, `party_story.cpp`.
- Iosefka: CL-00a95e. Fixed 0.7.8 (`BB_PARTY_GAOL_ALWAYS=0` turns it off), smoke `kg`.
- Priority: P3.

### 6. Key item reached the host twice (CL-60f2d5)
- Evidence: the report only (the duplicate warning is a game message, no log line); the smoke phase asserts the award
  once per lot (`tools/mp/smoke_party.py` near line 7265). Fixed in 331d0eee (guest pickup hides the host's ground
  copy, awarded once per lot).
- Suspected code path: `party_items.cpp` lot-award path for a guest pickup in the host's world.
- Iosefka: CL-60f2d5. Fixed 0.7.8, smoke (Orphanage Key lot 2420900).
- Priority: P3.

### 7. Special monster drop with a guest far away (CL-a6a2a7)
- Evidence: the report; the shared roll lines are in H (e.g. H 17993 `shared drop roll (role 3): map m24_01_00_00 entity
  ... row 1 slot 3`) and the guest's pile spawned at the monster, invisible from far away. Fixed in b12efeba: kills more than
  60 m away are paid into the inventory with a notice (`BB_PARTY_FAR_DROP`).
- Suspected code path: `party_droproll.cpp` shared drop roll, the per-chr distance based spawn of a drop pile.
- Iosefka: CL-a6a2a7. Fixed 0.7.8, smoke `fdr`.
- Priority: P3.

### 8. Memory Altar before the host's unlock (CL-ee41ff)
- Evidence: G1 31659 and 44696 `talk script t210699 ... asks IsClientPlayer: allowed` (the altar was opened for the guest
  without the Rune Workshop Tool unlock flag). Fixed 331d0eee: the altar is vanilla for a guest until the tool unlock flag
  (52200360) or the tool itself is held (`BB_PARTY_ALTAR_GATE=0` turns it off); the log line `Memory Altar (t210699): kept
  vanilla for a guest` shows the decision (NW-2ed6d82's log has it).
- Suspected code path: `party_guestsvc.cpp` Memory Altar service (t210699).
- Iosefka: CL-ee41ff. Priority: P3.

### 9. Gaol message repeated (CL-11e308)
- Evidence: G1 155058 vs G1 215971, two grabs of the same guest; the waking scene and its message played again. Fixed in
  the same change as #5: the waking scene plays once per character (flag kept per character), smoke `kg`.
- Suspected code path: `party_story.cpp` waking-scene replay (`party_story` waking scene once per character),
  `party_b2.cpp` event 12400780 rewrite.
- Iosefka: CL-11e308. Priority: P3.

### 10. Cathedral Ward lift out of sync for the host (CL-90785a)
- Evidence: the report (host's lift stayed where a guest had left it until a guest rode it again); no single log line.
  Fixed 1fedf612: a guest's ride was refused by a host standing in another area because its object table still showed the
  lift car; now written into the host world.
- Suspected code path: `party_b2.cpp` / world code lift table (`world_warper.cpp`).
- Iosefka: CL-90785a. Fixed 0.7.8 (the follow-up: host crash on a lamp warp right after a lift ride, liftcrash080, 0.8.0).
- Priority: P3.

### 11. Vicar Amelia second phase (CL-2c1eef)
- Evidence: H 242515 `Boss phase: event 12404807's state from a guest not applied (the host's own boss chr decides when the
  phase scene plays)`; the owner reproduced it locally (host dies early, spectates, she attacks no living guest after a
  wipe). Fixed 89363813: a living player's entry in the target bank lost its stale dead mark after the wipe's respawn.
- Suspected code path: enemy AI target bank (`party_enemyai.cpp`), the wipe respawn path. Diagnostics: `AI_LOG`.
- Iosefka: CL-2c1eef. Fixed 0.7.8 (retest by the reporter asked), smoke group `amelia_phase` (`amp`).
- Priority: P2 only as a retest; it was the one that blocked a boss for the whole party.

## Proposed Iosefka item texts (not written; for the owner's OK)

1. CL-8567d0 (existing), append: "4-player playtest 2026-11-01 (NW-80b250f): the creation screen glitch happened with the
   engine's Dream mirror assets on all four PCs; it stops the menu interactions. Needs RE of the creation face-accessory
   row and the ChrMake_BG preview slot. Priority 2, not a quick fix."
2. New, "retest asked": "4-player playtest follow-up: ask the reporter to retest on 0.8.1: (a) a guest with the Eye before
   the Blood-starved Beast dies; (b) Vicar Amelia's second phase after a wipe; (c) the Messenger shop with a guest in the
   host's world holding the badges; (d) a second Snatcher grab of the same guest. Logs of the idle moment if she stops
   again."
3. NW-2ed6d82 (guest cannot buy from Messenger shops despite owning badges): "Same cause as CL-3b4da5: a summoned guest's
   shop rows were reset to the own save's flags every half second; its own save only learns of a badge when the game
   event runs in its own world. Fixed 0.7.9 (rows open for a held badge or owned weapon, own pool written too), smoke
   `sgb4`/`sgb4n`. 0.8.1 adds the kill switch `BB_PARTY_SHOP_HELD_OPEN=0`. The reporter's log (v0.2.6 beta 6) predates it:
   it holds badges 4110/4114/4115 as key items and the overlay shows 12101000/04/05 = 0."
4. NW-019b146 (logs contain IP addresses): "0.8.1: every IPv4 and IPv6 address in the game's log, the launcher log and
   the net doctor's posted output is masked (1.2.3.4 -> 1.2.x.x). Switch: launcher 'Keep network addresses in the log'
   (`log_raw_ips`, `BB_LOG_RAW_IPS=1`). The net doctor reads masked logs (classification by the network part; it cannot
   tell two PCs of the same /16 apart, so the 'talks to itself' finding needs a raw log). Support bundle already masked.
   Unit test `tests/test_ip_masking.py` scans a smoke run's logs."
