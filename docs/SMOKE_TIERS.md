# Party smoke: tiers, fail-fast and flakiness (0.7.9)

`tools/mp/smoke_party.py` runs the co-op phases (one scripted game session each). Which phases run is a tier:

| Tier | Flag | Phases | Time (one lane, no queue wait) | Use |
|---|---|---|---|---|
| fast | `--tier fast` | 20, one per feature area (`FAST_PHASES`) | see "Measured" | every merge into master, every agent before handing back, a changed test harness |
| quick | `--tier quick` (default) | 40, the core party flow | about 1.5 to 2.8 h | a game-code change with no `CHANGED_MAP` entry |
| changed | `--changed-since TAG` | the phases the changed files map to | varies | a release candidate, to find what to rerun |
| release | `--tier release` | every phase but the variants in `RELEASE_SKIP` | about 5 h | the release gate (docs/RELEASE_CHECKLIST.md) |
| full | `--tier full` / `--release` | all phases | about 5.5 h | the release gate when nothing may be hidden |

A tier only chooses which phases run. Every phase keeps all of its checks.

## What the fast tier covers

One phase per area, the cheapest that proves the area's core claim: `s1` join / death / travel / boss / wipe, `s2` items
and kills, `ga` gather, `lf` lifts and ladders, `lt` two riders on a lift, `fd` deep fall, `kw` story warp, `cpr` chalice
progress, `bf` fight load, `br` boss rooms and fog walls, `bd` boss drops, `tv` travel vote, `nfa` NPC fights, `hlq` host
lamp, `rf` Rom's fog, `px` poison, `csh` chalice shop, `gvm` give menu, `eh` echo hold, `gdr` guest restart. (`ivl` is not in it: it failed its 3 level-range rows on master 23704188 when run by the smoke079 harness, 2026-11-04, and stays in the release tier until its owner has looked.) Areas it skips (chalice dungeon entry, lift walk-ins, covenants, NPC quests, PvP,
shops, music, ...) are in the release tier and are picked by `--changed-since`.

`cim` (group `chalice_madman`, 0.7.9 cim079: the Lower Hintertomb Chalice's twin boss sets the Madman unlock 5091 for the host and the guest, about 5 min) and `cis` are in the release and full tiers (not quick / fast: the Ritual Altar menu walk is flaky, both retry up to 3 times) and in `--changed-since` for chalice code.

## Fail fast

A phase ends as soon as its outcome is decided, not at its `--seconds` cap:

* all `--until` markers seen (plus the phase's `--until-hold`);
* `UNPLANNED CRASH`: a game crashed and was restarted;
* `STUCK` (new, `instances.py --stuck S`): the scripted chains are unfinished and nothing moved for S seconds. Progress
  is a chain step that ran, a load or map change, a join / leave, a boss bar or clear, a revive. Polling lines (target
  pos / anim, boss bar dumps) are not progress. S is 150 s, or 1.5 x the chain's longest step delay + 90 s, or the
  phase's entry in `STUCK_S` (phases whose quiet stretches are long by design, measured on the 0.7.8 logs);
* `DEAD` (new): a chain step has waited 90 s for a dead own chr and nothing else moved (a guest dead in a boss fight's
  limbo, a host that never gets up);
* `UNTIL NOT MET`: every chain is done and a marker never showed (60 s after the last step);
* `LEASE EXPIRED` when another run waits for the slots.

`BB_SMOKE_NO_STUCK=1` turns STUCK / DEAD off. The verdict line names the last "waits:" reason of each instance.

## Flaky phases

* A retry that fails the same rows as the try before is deterministic (a real fault or a harness bug) and is not repeated.
* A phase that failed and then passed is FLAKY: it is listed in `<out>/flaky.txt` and at the end of `verdict.txt`, in a green
  run too. Fix or root-cause it; a flaky phase is not a pass.
* `FLAKY_PHASES` in `smoke_party.py` says per group why a phase is there. A phase with no evidence is not added.

## Measuring a run

`python tools/mp/smoke_stats.py <out dir> [...]` prints, per phase run, the queue wait, boot to first step ("ready"),
the scripted steps, the tail (until-hold, quiet wait, timeout) and how the run ended, then totals by end kind and the
phases that needed a retry. Measured on the 0.7.8 release smoke (223 runs): boot to the first scripted step is about 10 to
25 s of a median 190 s phase, scripted step delays are 85 % of the passing time, and 24 % of all wall time went to runs
that did not end on their markers.

## Why phases do not share a game pair

Reusing a running pair across phases was measured and rejected: the boot to the first step costs 10 to 25 s per phase
(under 10 % of the time), while every phase sets its own fixture (save copy, `BB_PARTY_PROGRESS_TEST_CLEAR`, env flags,
test aids read at boot). A pair that carries state across phases would make a failure depend on the phase before it.
