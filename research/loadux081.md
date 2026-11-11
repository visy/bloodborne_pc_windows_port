# loadux081: loading speed-ups on by default (follow-up of fastload081.md, workers081.md)

Offsets are guest offsets. Harness: `tools/mp/loadab.py` (`--cycles 2`, `--shots --loadshots`), `BB_FAST_FADES_LOG=1`.

## A. Unpaced loading that still animates (`BB_FAST_LOADING`, default on)

Why the old opt-in froze the screen: `SprjFlipper::Update` (+0x2034770, once per game frame on the game thread) waits for
the frame's time unless `flipper+0x273` ("no sleep") is set, and the game's draw tasks (+0x1d92030, +0x1e3e6a0,
+0x1e3ed90 ...) run only when `+0x273 == 0`. The old code set the byte from the present thread every ~2 ms, so nearly every
frame was stepped without being drawn.

Now the byte is set on the game thread by a call-site hook on the main loop's call of Update (+0x2018e22; the pacer's entry
is already hooked by the party code, hooking it again stopped the title from continuing): set before the call (no wait),
and after it either left set (stepped only) or, when 16 ms passed since the last real frame, cleared together with
`+0x270` (late flag) and the flip interval `+0x10` Update zeroed put back, so the draw tasks run and the frame is flipped.
Not at the 30 FPS preset, not with a cutscene queued or playing. Measured: 32-43 presented frames per second during loads
(the game's own step is the limit between two real frames), no black frame in 171 shots of 8 loads.

## B. Fades twice as fast (`BB_FAST_FADES`, default on)

`SprjFade` singleton (*0x5540068): `+0x10 / +0x20 / +0x28 / +0x38 / +0x40` are fade objects (all one vtable; Update is
vtable slot 2, guest +0x1d2aa10, called from +0x1d924c0 / +0x1ed80c0). A map load's black fades use the object at +0x20:
floats at +0xc.. current RGBA / target RGBA, `+0x38` the remaining time counting down from 0.5 s by the game's own
step each frame, alpha = remaining / 0.5 fading in, 1 - remaining / 0.5 fading out. At the start of each frame the
countdown is taken down once more by the step the game made last frame (policy `FadeExtra`, unit tested): twice as fast
at any frame rate (the step is the live clock's or the fixed one), nothing is derived from game tables. Only black fades,
only outside cutscenes. `BB_FAST_FADES_LOG=1` logs the objects and `Fast fades: fade ... ran S s` per fade.
Not proven: whether other black fades on object +0x20 outside a map load exist (cutscene fades are excluded by the remo test).

## C. 4 load threads (`BB_LOAD_WORKERS`, default min(4, max(3, usable CPUs - 2)))

The guard's byte checks stay all-or-nothing. Fewer than 6 usable CPUs: the game's own 3.

## Results (loadab --cycles 2, 60 FPS preset; mean seconds on the loading screen)

See the report of the branch; the tables are in the commit message.
