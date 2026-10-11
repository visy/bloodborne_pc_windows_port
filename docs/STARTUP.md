# Startup time

How long a start takes, where the time goes, and what is cached. Every start logs a timeline:

* `Boot: run.bat started at <clock>` / `Boot: launching at <clock>`: run.bat's own steps.
* `Boot: <phase> at <s> s`: seconds since `bbport.exe` was created (`gpu/shim/boot_marks.h`;
  `BB_BOOT_TIMES=0` silences them): process start, boot image read, Vulkan instance / device,
  GPU ready, the party director's first tick, the first Continue press, the first loading screen
  up and gone (in the world).
* `tools/mp/instances.py run --timestamps` prefixes every line with `[+s]` since the harness
  started the first instance, and prints `[harness] N s from the game slot to the first start`.

## What changed

| Step | Before | Now | How |
|---|---|---|---|
| Boot image (`prepare.py`, `link_libc.py`, `link_modules.py`) | 10-15 s every launch | 0.01 s reused, 3-5 s when built | `scripts/boot_image.py`: one process; reused while `eboot.bin`, `sce_module/*`, `param.sfo`, the shader folder (game check) and the scripts are unchanged (sizes + times; `BB_BOOT_CACHE=0` rebuilds). `prepare.py`'s file count of `dvdroot_ps4` used pathlib (15 s for 28,000 files), now `os.scandir` (0.1 s) |
| Game check (launcher, before each start) | ~1 s | 0.01 s | `game_check.problem(cache=out/game_check.json)`: a pass is remembered for the same files |
| run.bat scripts in total | ~14 s | ~1 s | the above; mods, patches, content profile ~0.25 s each (Python start-up) |
| Test instances: setup | ~3 s, a 360 MB exe copy per instance | one copy per build (`ROOT\exe`), hard links | `instances.py snapshot_exes` |
| Test instances: boot image | built in each instance | built once per root | `BB_BOOT_CACHE_DIR=ROOT\bootcache`, hard-linked into each `out\` |
| Test instances: leftover check | 0.9 s (PowerShell) | ~0.05 s | the run.bat shells of the last run are recorded (`harness_pids.json`); PowerShell only without that file |
| Instance stagger | 8 s | 2 s (`--stagger`, `BB_MP_STAGGER`) | guests wait for the host's party code file anyway |
| Title: first Continue press | 1 s settle, then every 1 s | at once, every 0.35 s | `party_director.cpp AutoContinue` |
| Title: after the FROM sign-in | next press 1 s later | at once | |

## Where the time goes now (test instance, party host, warm caches)

| Phase | Time | Notes |
|---|---|---|
| game slot -> run.bat starts | 0.1-0.3 s (2.8 s right after a rebuild: setup) | |
| run.bat -> bbport.exe | ~1 s | Python start-ups |
| process -> boot image loaded | ~0.5 s | |
| Vulkan instance + device | 0.7 s alone; **4-9 s while other games run** | the NVIDIA driver serialises instance / device creation with the other processes' GPU work: a minimal vkCreateDevice test took 2-14 s with two other test games loading, 0.03 s right after. Not in our code |
| pipeline cache precompile | 0.2-2.7 s | blocking by design (`BB_SHADER_PRECOMPILE`); grows with the instance's cache |
| GPU ready -> title first tick | ~0.3 s | |
| title -> title takes input | ~3.8 s | the game's own title loading |
| FROM sign-in | ~1.9 s | the game's request pacing (each request answers in < 1 ms) |
| Continue -> loading screen | ~1.7 s | |
| loading -> in the world | 2-8 s | the save's map; shaders compiled on demand the first time |

## Measured (2026-10-11, loopback, 30 fps test settings, other agents' games running)

| Run | slot -> first start | run.bat -> bbport.exe | process -> GPU ready | GPU ready -> first Continue | Continue -> loading | loading -> in the world | **slot -> in the world** |
|---|---|---|---|---|---|---|---|
| before: hostdeath party host (task table) | ~2 s | ~14 s | ~7 s | ~4 s (1 s steps) | ~4 s | ~1.5-6 s | **~32-35 s** |
| before: base1 solo (new instance) | 1.1 s | 14.6 s | 15 s (Vulkan stall 13 s) | 5 s | ~9 s | n/a | **~47 s to world up** |
| after: press1 solo (just rebuilt: setup) | 2.8 s | 0.9 s | 8.1 s (4.6 s Vulkan stall, 2.7 s precompile) | 0.8 s | 8.2 s | 3.5 s | **24.7 s** |
| after: party1 host | 2.6 s | 0.9 s | 4.5 s (3.3 s precompile) | 0.9 s | 8.1 s | 3.4 s | **20.8 s** |
| after: party1 guest (started 2.1 s later) | 2.6 s | 0.7 s | n/a | n/a | n/a | n/a | **19.5 s** (in its own world before the bell join) |

party1 also checked the party flow with screenshots: the guest joined by the bell (`joined #1`
23 s after the slot), the host's lamp travel took the guest along (`(re)joined #2`), both players
visible with their name tags, no stray HP bars. Single-player through run.bat alone (no harness, no
launcher settings): Hunter's Dream reached, boot image built on that first start in 4.4 s.

The 12 s target is not reached: what remains is the driver's Vulkan start-up while other games run
(0.5-9 s), the pipeline precompile of each instance's growing cache (0.2-3 s), and the game's own
title (~3.8 s to take input), FROM sign-in (~1.9 s) and map load (~3.5 s).

`BB_PARTY_TITLE_TRACE=1` logs the title's screens by their message ids (the main menu shows
Continue 0x61F95; the sign-in screens are 0x61EE0 / 0x61EFE / 0x61F08).

## Not cached on purpose

* `patches.bin` (0.45 s): depends on settings, party mode and the restart flow; cheap.
* The game's `eboot.bin` SHA-256 in the party version check (~0.5 s on a thread, hidden behind the
  title loading).
