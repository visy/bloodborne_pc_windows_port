# Cloth above 30 FPS (GitHub #37: the Noble Dress clips through the player)

The game's cloth (Havok) is tuned for 30 FPS. The Live FPS clock solves it once a frame with the frame's own time, so
above 30 FPS the constraints pull the garment back to its shape more often a second than the game was tuned for. On the
Noble Dress the skirt collapses against the legs: its hem rides up and the inner particles fall inside the body radius.

## Finding

Before 0.8.2 the **Live FPS path (the default) installed no cloth hook at all**: `ClothStiffnessFactor` (deadinside28)
and the steady cloth time only ran in the older frame-rate-hooks mode. So the default only had the Deltatime patch's
single substep at the measured frame time, with the game's full stiffness.

## What 0.8.2 does

`BB_CLOTH_FIX` (default `pow2`, `0` = off = the 0.8.1 behaviour) installs `ClothStiffnessFactor` in the Live path too, with
the constraint stiffness scaled by `(30 x frame time)^2` (1 at 30 FPS and below; 0.25 at 60 FPS, 0.0625 at 120 FPS).
The spec is a comma list (`bbport_frame_rate_logic.h` `ParseClothTune`): `lin` / `quad` / `powN` (the scale), `all` /
`legacy` (also constraint types 5, 8, 10), `hook` / `caves` (the result of the factor, or the six hcl kernels' stiffness
input), `bend`, `damp`, `smooth`; `cur` = what the hooks mode always had, `jg` = Kyo's caves, `nw` = bend + damp.

## How it was measured (tools/mp/cloth37.py)

One game session, one save, the Noble Dress put on by the director's `armor=SET` (the id is looked up by name in the game's
own files, nothing stored). At each rate and variant: idle, two forward rolls, a stop, a backstep, a run and a stop
(`BB_FRAME_SWEEP_CLOTH`, `gpu/shim/bbport_frame_sweep.cpp`). From the game's memory: the skirt's 234 cloth particles (the
Verlet step's data) as their horizontal distance from the player's axis (r) and height (y) over the player's origin,
sampled every 4 ms; screenshots at fixed moments (`BB_SHOT_TRIGGER`). The proxy that tracks the bug: **hem height**
(lowest particle y) and **rmin** (innermost particle) in the settled windows (idle, 1.6 s after the rolls, 1.6 s after the
run). Stock at 30 FPS: hem y -0.23, rmin 0.08. Rates are what the game really ran (steps/s): 26.5, 49.5, 78, 113.5, 109.5.

Hem height (m, mean of idle / after rolls / after run), mean of 3 trials:

| variant | 30 | 60 | 90 | 120 | uncapped |
|---|---|---|---|---|---|
| stock (0.8.1 Live) | -0.23 | -0.17 | -0.15 | +0.06 | +0.04 |
| cur (30/fps, the hooks mode's) | -0.23 | -0.21 | -0.21 | -0.17 | -0.16 |
| **pow2 (30/fps squared)** | -0.23 | -0.23 | -0.23 | -0.22 | -0.23 |
| pow3 | -0.22 | -0.24 | -0.25 | -0.27 | -0.28 |
| lin, smoothed | -0.22 | -0.21 | -0.20 | -0.16 | -0.16 |
| pow2 + damping | -0.22 | -0.23 | -0.23 | -0.21 | -0.21 |

Settled records with the hem above -0.15 m (the "clipped" state): stock 3/9 at 60, 3/9 at 90, 9/9 at 120 and uncapped;
cur 0, 0, 3/9, 3/9; **pow2 0 at every rate**. Other tried variants (run 2, 1-2 trials): Jgamrot's kernel caves with
(60/fps)^2 (`jg`) do little here (hem still up at 120); Nightwatch's bend softening and damping (`nw`, `bend`, `damp`) help only
at 60; all types (5/8/10) and a smoothed scale change nothing measurable. Hair and the other cloth on the character moved
as under stock (differences within the 30 FPS noise).

## Credits

- Kyo (the six hcl kernel sites, "90 FPS++ rate base") via Jgamrot/yharnam-pc 7b716abe32, 7428741, 6c10f43962
  (GPL-2.0-or-later): `jg`, `caves` and `smooth` are after that work.
- Supermedo, Nightwatch `engine/scripts/cloth.py` (3ab1870, GPL-2.0-or-later): the bend softening and damping stubs
  (`bend`, `damp`; off by default).
- deadinside28 (bloodborne_pc 0fc2921c, GPL-2.0-or-later): `ClothStiffnessFactor`.
- No Lantern Engine code is used.
