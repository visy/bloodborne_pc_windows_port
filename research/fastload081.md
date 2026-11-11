# fastload081: shorter loading screens (follow-up of reload081.md)

Harness: `tools/mp/loadab.py` (one solo session: Continue, then per cycle the six big lamps and a death respawn; the
loading screen "up" -> "gone" time of every load; `--shots` takes 1 shot/s for 10 s after every "gone"). Same exe for
every variant, switched by env. 2 cycles = 1 Continue, 12 lamp warps, 2 deaths per session. 60 FPS preset, 720p.

## Results (seconds on the loading screen, mean of the session)

| variant | sessions | lamp warps | Continue | death respawn |
|---|---|---|---|---|
| all off (game's inflate, 3 threads) | 3 (A, A2, A4) | 4.00 / 3.96 / 3.86 | 2.19 / 2.20 / 2.20 | 2.22 |
| zlib-ng inflate (`BB_FAST_INFLATE`) | 2 (Z1, Z2) | 3.55 / 3.56 | 2.12 | 2.36 |
| 4 post-processor threads | 2 (G1, G2) | 3.60 / 3.59 | 1.98 / 2.05 | 2.22 |
| 6 threads + zlib-ng | 2 (D, D2) | 3.50 / 3.46 | 1.95 / 1.87 | 2.21 |

(The first session after a build is colder: the A_base table above is its cycle 2 for comparison, 3.3 -> 3.0 for the
Great Bridge.) Per lamp, zlib-ng: Great Bridge -0.7, Yahar'gul -0.8, Tomb of Oedon -0.9, 2102950 -0.1; Cathedral Ward
+0.25 and Old Yharnam (2302950) +0.2 (they stay at 6.2 s / 3.8 s with any thread count: another limit), death respawn +0.14.

An earlier zlib-ng build never ran: the engine's streams are zlib-wrapped (windowBits 15), not raw as reload081 said;
sessions C / D / D2 measured only the thread count. The hook counts its work (`Load speed: fast inflate: ... MB in`).

## More post-processor threads: unsafe, off

`Core.Resource.PostProcessorNum` (registry read at +0x20c4761) is 3. 4 threads were clean in 2 sessions; 6 threads: a hung
load in 2 of 4 sessions (B_w6, H1_w6: MoveMapStep 3 -> 9, 60 FPS game, every `Core.Res.*` thread asleep in a condition wait
and the TaskManager thread busy; black screen with the area title), `Work could not be allocated` panic in a third
(R6, `PostProcessor.cpp(580) pWork could not be allocated`), 8 threads panic at once (F_w8). Growing the work buffer pool
(`WorkBufferNum`, +0x20c444c) with the thread count makes `DLResourceManager` init fail (its heap is sized for the game's pool).
So the engine's per-processor work budget is the limit. `BB_LOAD_WORKERS=N` stays as an opt-in experiment, default 0.

## Loading screen frame cap

Dropped (code removed). Core-limited run (`BB_AFFINITY_MASK=0x3F`, 6 logical CPUs), all else off: 5.20 s per lamp warp uncapped, 7.22 s with a 30 FPS cap on the loading screen (Continue 2.83 -> 3.95, death respawn 2.56 -> 3.67): the loader steps once per frame, so a cap slows it. With zlib-ng on the same 6 CPUs: 4.18 s (-1.0 s, 20 %).

## Tools added

`BB_LOAD_TRACE`: a `Load trace: STALL:` line when a MoveMapStep step does not advance for 30 s (threads by name, how many
`Core.Res.*` were busy), and a 3 s watch of the SprjFade objects (+0x20 / +0x38 of *0x5540068) after each load.
