# Integration of Mrsuss60/bloodborne_pc_windows_port (origin/master)

Reviewed `windows-upstream-0.5..origin/master` (23 commits). Credit lines in commit messages read
"Ported from Mrsuss60/bloodborne_pc_windows_port <sha>".

## Taken / adapted

- e2a647d window mode: ADAPTED. `BB_WINDOW_MODE` = windowed / borderless / fullscreen (window.cpp), launcher "Window mode" combo
  (saves `window_mode`, still honours the old `fullscreen` setting and sets `BB_FULLSCREEN`). Borderless is a frameless
  window over the whole chosen display (BB_DISPLAY). Their raw Win32 style hack was not needed: SDL windowed is already
  decorated. `BB_WINDOW_X/Y/W/H` and `BB_WINDOW_BACKGROUND` still win in windowed mode.
- c7ca01a audio boost: ADAPTED. Default 1.4x (the user found the volume too low), `BB_AUDIO_BOOST` overrides (1.0 = original),
  new "Volume boost" slider on the in-game Display page (`audio_boost` in bbport.ini). Their base-gain change (0.75/1.25) was
  not taken; the multiplier applies to the original gains. The limiter was replaced: their `soft_clip` (also ours) fell to 0
  at x=1.0 (discontinuity); now linear to 0.75 and a tanh knee that stays below full scale, so boosted loud scenes cannot clip.
- c7ca01a CUSA03179 sound-hack whitelist: already present.
- 931eba9 Store Python: TAKEN for launch_gui.bat, ported equivalently to run.bat (real Python 3.10+ check, 3.14 paths).
- c8354b7 M&K layout: PARTLY. Added F and V as Triangle (heal) next to X. Skipped the radial acceleration curve (ours is
  velocity based with configurable sensitivity/dead zone/smoothing and menu pages) and the Tab = L1 move (Tab is Touchpad in
  our documented layout; Shift+RMB already gives L1).
- 196ba93 padding guard: TAKEN. Trailing all-zero dwords in a DCB/ACB chunk end the chunk instead of hitting UNREACHABLE.

## Already present (equivalent code in our tree)

- c7ca01a VRAM heaps: our telemetry already sums device-local heaps only (`VmaDeviceUsage`).
- a28589a overlay reverting menu changes: we reload only the mouse tuning keys and never while the menu is open.
- 8e8de99 English/Russian overlay: `menu_language` (default English, `BB_MENU_LANGUAGE`) exists.
- db7edde, cdbf89b, 66a47ca: hand-ported earlier.
- 196ba93 atomic shader-cache replacement (`.tmp` + rename), corrupt-cache handling (`Serialization::CorruptData`), SPIR-V
  magic check, 64 KiB-style WRITE_DATA/DMA host-copy tracking (`WaitHostCopiesFor`, `NoteHostCopySource`), indirect bounds guard
  (`vk_indirect_guard.cpp`), async graphics pipelines (`BB_ASYNC_SHADERS`), texture eviction tiers.

## Skipped

- 196ba93 async compile pool, LRU eviction/defragmentation, host-copy tracking, indirect guard: cherry-pick conflicts in all 15
  touched files because ours is a rework of the same areas (see above); porting would replace working systems, so no A/B was run.
- a6ad9a8 launcher window size, README and credits commits (4579296, 18a2970, 191b8a9, e4d46d1, 78bc0c1, 268c560, 71c4e04,
  3c4bb35): our launcher/README differ; the author is already credited.

Suggested: `git merge -s ours origin/master` to record the integration without changing the tree.

## Verification

Build OK; pytest 128 passed / 20 skipped; no `*-test` executables are built in this tree. A loopback 2-instance party
(150 s) connected and joined with no crash; a solo borderless run of 60 s exited cleanly; screenshot via tools/mp/shot.py
(world rendering, shots in agents/mrsuss/shots). The requested 196ba93 stutter/VRAM A/B was not run: its pieces do not
apply on top of our tree (15-file conflicts) and the equivalents already exist, so there was no second variant to measure.

## 0.6.2 review: 2b6d6dab..c851c08d (6 commits)

- 90008b39 input (mouse look, sliders, rebinding): ADAPTED in three parts.
  - Sensitivity sliders: their finding (the sliders did not work) confirmed by measurement with the new
    `BB_CAM_PROBE=1` aid: the game's stick dead zone is 46.7/127 (round), not the assumed 16/127, the curve
    is linear above it up to 120 and full deflection is a fast turn. Our velocity mapping now goes through the
    inverse of the measured curve (src/runtime_pad.c `look_curve`); measured before 0 deg/s below ~360 px/s at
    sensitivity 1, after 83-87 mdeg per count from 100 to 800 px/s (final curve; 46-49 at 0.5, 176-200 at 2 with the
    first table), linear in the slider. Their impulse/decay
    look model and the larger slider ranges were not taken (ours is frame-rate independent already).
  - Relative capture: the capture-click discard is taken (a click arriving before the mouse is captured, or
    within 250 ms of the focus, is no game press). Release on focus loss and while the overlay or the name box
    is open was already here; ClipCursor is not needed (SDL's relative mode keeps the cursor in the window);
    capture-on-click and F1-release are not taken (we capture on focus, F1 is a game key).
  - Rebinding: click-to-assign rows with exchange and mouse buttons in the Insert menu's Controls tab, on our
    existing `mkkey.*` / `key.*` lines (src/pad_bindings.h shared with runtime_pad.c, reread at once). Their
    fixed 20-action table, Shift + click chord codes and the removal of the classic layout's fallbacks were not
    taken (our two layouts, pad.* lines, pings and Back combos stay as they are).
- 2b6d6dab PySide6 launcher rewrite (launcher.py deleted): SKIPPED; our Tk launcher.py carries the party,
  graphics, HDR and world-warper features, and we ship no Python runtime / PySide6.
- 8c3f4a19 launcher logo and title, db30ede3 launcher icon: SKIPPED (their PySide6 launcher's branding; the icon
  is their project's artwork, not a generic asset).
- 9048ed55 delete launch_gui.bat: SKIPPED (we keep launch_gui.bat for our Tk launcher).
- c851c08d run.bat prefers a bundled embedded Python: SKIPPED (we do not bundle Python).
