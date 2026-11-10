# perf081: 0.8.0 performance / stability baseline (0.8.1 P0-1)

A fixed, scripted, unattended 60 minute route, solo (1 game) and 2-player party (2 games, loopback), measured on
two executables of 0.8.0 (master 05fb2aaf). Later fixes are measured against these numbers: same route, same
settings, same command.

## Rerun (one command)

```
python tools/mp/perf_route.py run --variant solo --build <dir with out\bbport.exe> --out <result dir>
python tools/mp/perf_route.py run --variant 2p   --build <dir with out\bbport.exe> --out <result dir>
python tools/mp/perf_route.py report --variant solo --out <result dir>      # rebuild summary.txt / fps.csv / loads.csv
```

Extra switches for an A/B on the same route: `--env BB_ZPASS_IN_ORDER=0` (repeatable, recorded in
`settings.txt`), `--fps 60`, `--live-fps` (BB_LIVE_FPS=1, GitHub #35), `--minutes N`. The run takes both game slots
(`--reserve 2`: nobody else's game shares the GPU) and ends when the route's last step is done, ~60 min.
About 60 min each; do not run two variants at once. Solo and 2p used the default test save
(`E:\games\bbport\test_saves\savedata`, a fresh copy per run), `BB_PARTY_PERF=1` (every 60 s), `BB_FRAME_STATS=1` (every 5 s),
BB_AUDIO=none, BB_WINDOW_BACKGROUND=1, no invincibility, no enemy clearing.

## The route (7.5 cycles x ~8 min, all by the in-process director, no human input)

Per cycle: lamp warp to the Great Bridge (m24_01), walk the bridge arena <-> lamp twice by the pad stick, die
(`host_die`) and respawn, lamp warps Cathedral Ward, 2302950, 2102950, Yahar'gul, Tomb of Oedon, then 255 s idle in
the Tomb of Oedon, next cycle. 8 cycles. Every warp is `host_lamp` (the same in both variants; in 2p the host's
"travel with the party?" is answered YES and the guest follows). Solo: the death reloads the lamp (vanilla loading
screen, 57 loads per run); in 2p a death is the party's in-place revive (no load, 42 loads per run per game). Both
games of the 2p run are rendered; the guest only follows.

## Settings

Frame cap: `--fps uncap` (the game's patch preset), Live FPS off. The harness's `Live clock: target 240 FPS` line
shows in the log, but the party/game step rate sat at 60-64 steps/s in 2p (two rendered games on one GPU). Measured
machine: the one the agents share; no other game ran (slots reserved) during the four runs.
Sampling: soak_sample.py every 30 s (private, working set, handles, threads, GPU dedicated/shared), Frame stats every 5 s.

## Results (FPS = the "Frame stats" 5 s windows; windows are minutes 3-8, 28-33, 55-60 after the harness start)

| run | FPS mean / p1 @5 | @30 | @60 | drift 5->60 | idle-dwell FPS first -> last |
|---|---|---|---|---|---|
| solo, own build (no LTO) | 93.5 / 64.8 | 96.3 / 89.6 | 94.6 / 76.6 | +1.1 % | 94.4 -> 87.2 (-7.6 %, last dwell dips, p1 68.6) |
| solo, rel080 (PGO+LTO) | 104.9 / 64.0 | 109.8 / 101.1 | 106.6 / 79.3 | +1.6 % | 111.8 -> 109.7 (-1.9 %) |
| 2p own, inst0 / inst1 | 64.7 / 52.6, 65.1 / 52.5 | 62.5 / 49.4, 63.5 / 50.3 | 61.5 / 58.0, 63.1 / 58.2 | -4.9 % / -3.1 % | 61.4 -> 61.4, 62.8 -> 63.1 (flat) |
| 2p rel080, inst0 / inst1 | 67.9 / 56.2, 69.8 / 58.1 | 70.4 / 61.2, 70.8 / 60.3 | 67.4 / 58.5, 69.9 / 61.2 | -0.8 % / +0.2 % | 65.4 -> 67.3, 66.7 -> 69.6 (+3-4 %) |

Memory (private MB / working set MB / handles / GPU dedicated MB, at about 5 / 30 / 60 min):

| run | 5 min | 30 min | 60 min | growth 5->60 |
|---|---|---|---|---|
| solo own | 3538 / 4660 / 3380 / 1956 | 4086 / 4901 / 3807 / 2447 | 4579 / 5026 / 4357 / 2901 | private +1041, ws +367, handles +977, GPU +946 |
| solo rel080 | 3591 / 4551 / 3383 / 2113 | 4097 / 4773 / 3810 / 2517 | 4521 / 4922 / 4389 / 2883 | private +930, ws +371, handles +1006, GPU +770 |
| 2p own inst0 | 3588 / 4581 / 3387 / 2045 | 4080 / 4803 / 3725 / 2449 | 4496 / 4914 / 4059 / 2844 | private +908, ws +333, handles +672, GPU +799 |
| 2p own inst1 | 3498 / 4668 / 3355 / 1898 | 3985 / 4871 / 3693 / 2299 | 4383 / 4970 / 3989 / 2666 | private +885, ws +303, handles +634, GPU +768 |
| 2p rel080 inst0 | 3435 / 4576 / 3387 / 1908 | 4041 / 4776 / 3719 / 2442 | 4325 / 4915 / 4087 / 2691 | private +891, ws +340, handles +700, GPU +783 |
| 2p rel080 inst1 | 3521 / 4664 / 3357 / 1905 | 4092 / 4877 / 3688 / 2385 | 4485 / 4987 / 4045 / 2735 | private +964, ws +323, handles +688, GPU +830 |

Load times (loading screen up -> gone, from the log; the first one is title -> world): solo mean 5.0 s (rel 4.9),
median 3.9 s, max 12.1 s (the death respawn); 2p mean 5.0 / 4.9 s (rel 4.4 / 4.3 s), max 7.3 s. Per-load rows in `loads_*.csv`.

Crashes / asserts / device lost: none in any of the four runs (the harness ended each by the route's last step).

## Findings

- No FPS drift over 60 min on this route (|drift| under 5 % everywhere; the solo own-build last idle dwell, minute 52, dipped to 87 vs 94-99 before).
  Acceptance for the occlusion port (P0-2, drift <= 5 %) is therefore measured against the idle-dwell column.
- Private bytes and handles grow steadily: about +0.9-1.0 GB private, +0.35 GB working set, +1000 handles (solo) / +650-700 (2p) and
  +0.8-0.9 GB GPU dedicated per hour, linearly over the 8 cycles (each cycle = 6-7 loads). Working set levels off,
  private and GPU memory do not. This is the stability item to fix (P1-8 and a leak hunt); handles per load are about 18 solo, 15 in 2p.
- FPS in 2p sits at about 60-70 with both games rendered on one GPU; solo 94 (own) vs 107 (PGO+LTO). rel080 is ~11 % (solo) and ~8 % (2p) faster.
- GitHub #35 (FPS stuck ~64): not reproduced as a drift here; in an earlier (void, not kept) solo run the first ~10 min sat at 64-68 FPS in the idle
  dwell and later at 95. Run this route with `--live-fps` to measure #35.
- Screenshots (every 120 s, `perf/*/shots`, not committed) looked at: no glitches, no black or grey frames (grey.log and capture.log empty).
- The earlier 55-minute solo run with the old 210 s idle (`summary_solo_55min_oldroute_note.txt`) is not comparable.

## Files

`baseline_solo.csv`, `baseline_2p.csv`: soak_sample.py rows of the own build (columns: time, elapsed_s, inst, pid, private_mb,
working_set_mb, handles, threads, gpu_dedicated_mb, gpu_shared_mb, cpu_s). `*_rel080.csv`: the same for the release exe.
`fps_*.csv`: every Frame stats window. `loads_*.csv`: every load. `summary_*.txt`: the report output (includes the route's director chain in
the first run's settings.txt, i.e. `tools/mp/perf_route.py chain()`).
The own build is a non-LTO build of master 05fb2aaf (`python tools/agent/build.py`); the release exe is `E:\games\bbport\rel080\out\bbport.exe` (PGO+LTO).
