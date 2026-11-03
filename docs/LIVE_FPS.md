# Live FPS clock (Mrsuss60's live FPS patching, ported in 0.7.8)

Ported from **Mrsuss60**'s Windows port ([bloodborne_pc_windows_port](https://github.com/Mrsuss60/bloodborne_pc_windows_port),
GPL): a7cbfae6 (live clock, "60 FPS++ (Deltatime)" base, launcher lock-outs), 83a168ee (pacer ceiling 240 FPS,
the deadband / slew clock, `BB_FPS_PATCH` fix) and the live-FPS / camera part of dcdb834f (the 41 artificial camera damping
lines cleaned out of the Deltatime patch). The Vulkan, shader-recipe and NVAPI parts of dcdb834f were not taken.
**Off by default** (`live_fps=0`); the frame rate by hooks (`gpu/shim/bbport_frame_rate.cpp`) stays the default.

## What it is

Two frame-rate systems exist in this tree and they never run together:

| | Frame rate by hooks (default, deadinside) | Live FPS clock (this page, Mrsuss60) |
|---|---|---|
| Patches | none (BB_FRAME_HOOKS=0: the FPS++ patches) | one XML patch, `60 FPS++ (Deltatime)` (`patches/Bloodborne.xml`) |
| Where the step comes from | `FrameStep` at `0x2018e36`, the measured frame time clamped to [1/rate, 1/30 s] | `LiveClockTick` replaces the step block at `0x2018e3d` (46 bytes, checked against the patch's exact bytes) |
| Per-frame constants | 48 redirected operands recomputed every frame (`Live` struct), cloth stiffness factor, steady time | the patch's ~30 code caves read the pacer's measured time (`[pacer + 0x264]`) and a guest global (`image + 0x535A908`); the clock writes both |
| Time filter | steady time (+-5% a frame) only for cloth and plants | the step is the measured frame time, clamped to [1/240 s, 1/`max_dt_hz`]; only +-1.5% micro-jitter is locked to the target, the difference carried to the next frame (Mrsuss60's +-8% deadband and +12% / -15% slew limit made the game run at 0.83 of real time under frame jitter and were dropped) |
| Rate change | `SetTargetRate` (sweep tool only) | System menu > Live FPS > Target frame rate, at once; the presenter limit follows |

Live clock per frame (`LiveClockStep`, `gpu/shim/bbport_frame_rate_logic.h`, unit test `frame-rate-test`): the pacer's frame
time target (`[pacer + 0x18]`) is set to 1/target (1/240 for uncapped); the measured time is replaced by the filtered step,
which is also what the game steps by (`[rbp - 0x38]`). The pacer's four mode setups carry 1/240 s as their frame time (the
patch since 83a168ee; older patch bytes are lowered at start), so 90 / 120 / uncapped can exceed 60 FPS. The presenter
(`videoout/driver.cpp`) takes its frame limit from `BbFrameRate::LivePresenterLimit()` every iteration. The effects' step in
whole 1/60 s (bbhost fix) runs from the live step; the plants' wind force is fixed at the authored 1/30 as in the hooks mode (`GrassWind()`, installed in live mode too since
0.7.8: the Deltatime patch had made the force V * the live step, so every long frame bent the plants by its own length and they shook;
the wave time keeps the live step).

## Switches

| Switch | Meaning |
|---|---|
| `live_fps` (bbport.ini), `BB_LIVE_FPS` (wins) | the live clock on / off; read at start by `run.bat` -> `scripts/patches.py` (`live_fps_wanted`) and the game (`LiveWanted`), which must agree. Never at `BB_FPS=30` or `BB_FPS_PATCH=0` |
| `BB_FPS`, `BB_FPS_LIMIT` | the starting target (60, 90, uncap, or uncap + limit N for 120 / 144); the launcher's *Frame rate target* now offers 120 and 144 this way |
| `target_fps` (bbport.ini, 30..239, `uncap`) | the target, changed live in System > Live FPS |
| `max_dt_hz` / `BB_MAX_DT_HZ` | the physics step's upper clamp (default 30: 1/30 s, as the game) |
| Launcher | Graphics > Frame rate > *Live FPS clock (experimental)*; the Performance Patch is greyed out and off while it is on |
| System menu | *Live FPS*: *Live FPS clock* (after a restart), *Target frame rate* (at once) |

While live mode is on, `scripts/patches.py` drops `30 FPS++`, `60 FPS++`, `90 FPS++`, `Uncap FPS++`, `Sprint Fix (High FPS)`,
`60FPS (no deltatime)` and `Performance Patch (perf increase)` (Mrsuss60's `LIVE_FPS_CONFLICTS`), and external patches
whose name contains `fps++`, `no deltatime` or `performance patch`. If the eboot does not match (another dump, or a patch
that rewrote the step block) the game logs `Live clock: incompatible eboot signature` and keeps the patch's static 60 FPS
(the presenter then limits at 60).

## Party

Frame rate is local: `60 FPS++ (Deltatime)` is in `COSMETIC_PATCHES` (left out of the patch hash of the version check), the
live clock installs no party hook (`coop::InstalledHooks()` is untouched), so players with different modes and rates
(host 60 hooks, guest 120 live) join each other and the version check is unchanged. Each game steps by its own dt as before;
the party layer already handles different rates (lift guard, ladder and fall rules).

## Decisions where the two systems meet

* The live clock and the hooks use the same instruction (`0x2018e36` / `0x2018e3d`): live mode is chosen at start, the
  hooks are not installed then (`Install` returns after `InstallLive`).
* The Wind Phase Fix patch of 83a168ee (the two NOPs at `0x01D084EF` / `0x01D08512`) rides inside the Deltatime patch
  list; it was not added as a separate patch for the static presets because the hooks already handle the grass wind.
* dcdb834f's camera change (no artificial damping overrides) is already in the Deltatime patch text taken from that commit.
* Mrsuss60's `bbgpu_get_live_clock` / `bbgpu_note_live_clock` C ABI and the overlay diagnostic line were not ported: the
  clock reads `BbSettings` directly and logs its target and step to the game log (`Live clock: ...`).

## Evidence

Physics smoke at 30 / 60 / 60/48 / 120, live vs `BB_LIVE_FPS=0`: all rows green with live on (docs/party/world_actions.md 10.1.5). The
lift drop-through of CL-eab030 was not reproduced either way.

Speed (median game steps a second / game seconds per real second, in-world): see 10.1.5. After the fix a single game at a target of 60 makes
59.6 / 0.996 (hooks 59.8 / 1.000); two games on one PC match the hooks (56.8 / 0.950 vs 56.9 / 0.952, the load is the PC's).
What made the first port slow: (1) the slew limiter lagged every long frame (game time 0.83); (2) the game's pacer asked for 1/240 s so
nothing held the target but the presenter, and with a 480 Hz vblank that ran 49 steps a second at a target of 60; the pacer mode hook of
the hooks mode now waits for the target (`live->shortest`, set each frame by the clock) and `run.bat` keeps the presets' vblank (60 Hz for 60,
90 Hz for 90, 480 Hz otherwise: at 60 Hz exactly 60 steps, at 480 / 240 / 120 Hz 49-54).
Unproven: the fallbacks (an eboot the patch does not fit), a party with live on one side only in a real session, the System menu target switch
in game (a target above the start vblank, 60 or 90, is not reached: start at 120 or more).
