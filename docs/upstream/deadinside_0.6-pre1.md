# Upstream 0.6-pre1 (deadinside, Supermedo/bloodborne_pc 72e5c8ce): review for the Windows port

Upstream commit 72e5c8ce (deadinside, 2026-10-15) is one squashed commit: 825 files,
+107.5k / -102.5k lines, on top of 0.51 (3c57da41, already merged here). Its notes are in
upstream `docs/CHANGES_0.6-pre1.md` and `docs/CHANGES_after_0.5.md` (Russian). The server-based
online work (supermedo/windows 9eeabba3 / 54fb0374 / 170a3d79: server pool, version report,
launcher UPnP for their server) was skipped. This port's party co-op needs no server.

**Licence:** the new and changed upstream sources are `GPL-2.0-or-later`. The bbsc_* files are
`Copyright 2026 CLS_AMG`, also GPL-2.0-or-later, and the BBSC game server script is MIT. All of
these can be used in this GPL-3.0-or-later tree. Upstream's new renderer files dropped the
shadPS4 copyright lines (the attribution is only in `gpu/VENDOR.txt`). If we import those files,
we put the notices back.

## Fixes: ported, already fixed, or not applicable

| Upstream item | Here | Evidence |
|---|---|---|
| NVIDIA start, `CreateDevice: ErrorInitializationFailed` (#135 #147 #152) | **Ported**: the device is created a second time without the extensions only DLSS asked for, and DLSS turns off with a logged reason. Upstream's other part, `libcuda` in the AppImage, is Linux only: on Windows `nvcuda.dll` comes with the driver. | Before, `vk_instance.cpp` returned "Failed to create device" when DLSS's extensions failed. |
| RIP=0 crashes (#161 #177 #181) | **Ported**: buffer marker, vertex input dynamic state, extended dynamic state 3 and external memory host turn off when the dispatcher does not resolve their commands. `vkCmdPushDescriptorSetKHR` is now checked. | Same unchecked flags as 0.51. |
| Old model: VRAM copies pile up until "VRAM is full" (#142) | **Ported**. This is our default model (`run.bat` sets `BB_PC_MODEL=0`). On cards with up to 8 GB, idle copies leave VRAM after 60 s; on any card they leave after 5 s while VRAM is short; a failed chunk allocation first evicts copies unused for 1 s; empty chunks go back to the driver. | `ProcessIdleBlocks` returned at once without `GuestInPlace`, the same as 0.51. Checked live with `BB_VRAM_IDLE_SECONDS=5`: the log shows `Guest memory: N idle VRAM copies released`. |
| Write-trap race (stale VRAM copies, "exploding" vertices) | **Ported**: `LayerUnprotect` now runs under `layer_trap_mutex`. This only matters with `BB_PC_MODEL=1`. | Same code as 0.51. |
| Pipeline-cache stages | **Ported** two of three. (1) Geometry-shader pipelines from the disk cache got an empty fetch shader: the GS stage, loaded after the VS, reset it. (2) A draw without a pixel shader used the previous draw's fragment runtime info (clip discard shader, NVIDIA). (3) The discard shader not being saved was **already fixed** here (pipeline key v6). | `LoadShaderMeta` assigned the fetch shader for every stage. `RefreshGraphicsStages` left `runtime_infos[Fragment]` stale. |
| Real occlusion queries | **Ported** the per-pass sample scale (8.8 factor per query segment, applied in the resolve shader). This port keeps the queries opt-in (`BB_OCCLUSION_QUERIES=1`). Upstream turns them on by default only with the in-place model (`GuestInPlace()`), and our default is the 0.3 model. | Our `vk_occlusion.cpp` was the 0.51 version. |
| Decoder and format asserts during draw preparation (#144 #165 #166) | #166 SnormNz: **already fixed** (Mrsuss60's fallback). Out-of-range anisotropy on a stale S#: **ported** (clamp to 16x). The "illegal encoding" decoder asserts (#144, #165): **not ported**. Upstream reports them with the new model's block moves; the fix is a new `LooksLikeProgram` / `ProgramDecodes` pre-check plus a GPU read-back, built on its new translator. Port it if a Windows log ever shows `decode.cpp:115` / `:267`. | No such assert in our logs or docs. |
| Lamp-travel loading hang, thread handles above 40 bits (PR #193) | **Already fixed**: since the first Windows port, thread records come from `runtime_low_calloc` with `CHECK_LOW_ADDR` (below 1 << 40). | `src/runtime_thread.c` `new_thread`. |
| Crash at `0x2199310`, GX per-frame heap full (#162 #192) | **Already fixed** (playtest 1, `bbport_stability.cpp` guard 2, 1 MiB frame arena). Difference: upstream skips past the pending-flag clear, so the command goes out next frame; ours drops it for that frame. That could be refined later, but only after RE of `+0x3ab`. | `docs/party/playtest1_triage.md`. |
| Empty character-creator preview (#178) | **Already fixed**: hashes are kept for small, storage and target images (Supermedo 136a72d). | `texture_cache.cpp` refresh `track`. |
| #149 frame-sized buffer over the bus at 4K, FPS drop after long lamp travel | **Not applicable** by default: both are in the layer memory module (`BB_PC_MODEL=1`). Cherry-pick (edge blocks, reload reset) only if model 1 becomes a Windows option. | |
| #134 menu language with `--play` | **Not applicable**: our launcher does not pass `BB_MENU_LANGUAGE`; the in-game choice is the only source. | |
| #172 Steam Deck pressure-vessel, #136 #145 Debian / online-module build, AppImage `--play` | **Not applicable** (Linux packaging). | |
| #186 FSR 4.1.1 FP8 passes | **Not applicable**: the change is in the Proton capture script under a spoofed RX 9070 XT id. | |
| Exit crash in `libbbnet.so` | **Not applicable**: the shadNet module is not shipped here. | |
| Mouse: camera jump on capture | **Not applicable**: our mouse camera is the BbMouse sampler plus imedved's camera, not SDL `xrel` at capture. | |
| HUD white streaks at 21:9 | Part of the hook-based aspect rewrite (below). | |

## The big pieces (not ported in this pass)

### 1. Own renderer, command processor, shader translator and caches (`gpu/shadps4` removed)
- **What:** `gpu/shadps4` (399 files) is replaced by `gpu/cp`, `gcn`, `shader`, `render`,
  `textures`, `buffers`, `memory`, `vulkan`, `platform` and `common` (about 300 files, about 75k
  lines). The translator gives byte-identical SPIR-V, so this is mostly a restructure and rename.
  Upstream now also builds a MinGW `bb-probe.exe` (`gpu/platform/win32`: POSIX-over-Win32
  headers, pagefile-section memory), but there is no Windows release, launcher or evidence of
  testing.
- **Would it help:** no user-visible speed claim. The value is strategic: every later upstream GPU
  fix lands in the new layout, so each port gets more manual the longer we stay on `gpu/shadps4`.
- **Risk: high.** We have about 9k lines of our own changes in `gpu/shadps4` (frame generation,
  Reflex, HDR, driver pipeline binaries, async compiler, texture collector, ultrawide / UI
  composition, device fault, motion vectors). Seven renderer files touch party code (present hold,
  compile progress, new-game test). There are two competing Windows platform layers.
- **Effort:** 12-18 days, as a dedicated rebase project.

### 2. Memory model switch (layer memory module on every GPU)
- **What:** "Auto" means `BB_PC_MODEL=1` everywhere. On Windows it would import the pagefile views
  through `VK_EXT_external_memory_host`. Upstream has not shown that working on Windows.
- **Would it help:** little compared with our 0.3 model. Upstream's numbers compare the module
  with the AMD sparse arena, not with the copies model (at 4K: new 132 vs old 130 FPS). On NVIDIA,
  host import was slower (GTX 1660 Ti: 30-35 vs 60-70 FPS with the 0.3 model).
- **Risk: medium-high.** New fault and write-trap paths on Windows next to our VEH order, party
  SafeWrite and the red-zone handler. Upstream has not tested it on NVIDIA or Intel.
- **Effort:** 2-4 days to cherry-pick the edge-block and reload-reset fixes. Making it the default
  is a further 5-8 days of testing.

### 3. Aspect ratio and frame rate by hooks
- **Frame rate** (`gpu/shim/bbport_frame_rate.cpp`, about 370 lines plus a small x86 emitter):
  it hooks the main loop's pacer and step (`0x203493d`, `0x2034d96`, `0x2018e36`) and moves 48
  frame-tied constants onto a per-frame page (dt, `k^n`, `1-(1-f)^n`). It fixes bugs our
  community patches still have: body-part physics timers 6-7x too fast at 200 FPS, running
  dropping to walking above about 90 FPS, run-into-wall damping, and durations the patches
  shortened 2-8x. **Risk: low-medium.** Party hooks the same pacer's prologue (`0x2034770`,
  party_director / lamp_travel). The bytes do not overlap, but party timing must be re-checked
  at variable dt. **Effort: 2-4 days, independent of the renderer.**
- **Aspect ratio** (about 1k lines of hooks: camera projection copies, Scaleform viewport, HUD
  anchoring, markers, Display menu rows, plus renderer proxy targets and a UI-fill vertex shader):
  live 4:3 to 32:9 without patching game code, and it fixes the 21:9 HUD streaks and marker
  placement. **Risk: high** (tied to piece 1, to our ultrawide and UI composition, HDR and frame
  generation, and to the party's ping and HUD projection). **Effort: 6-9 days here, or about 3
  days after piece 1.**

### 4. Keyboard and mouse rewrite
- **What:** a Dark Souls III-like layout with chord bindings (`runtime_pad.c`), a native mouse
  camera detour at `0x143cde0`, a menu mouse pointer, and button prompts for the device in use,
  including the HUD atlas (`bbport_button_prompts.cpp`).
- **Would it help:** yes. No dead zone or acceleration on mouse look, a menu pointer, and keyboard
  or Xbox prompts. Upstream's camera hook would also replace the Nexus-derived
  `bloodborne_cam.cpp`.
- **Risk: medium.** Their camera site is inside the range our camera NOP-patches, so we must pick
  one. The layout differs from our mouse and keyboard defaults and from users' saved `key.` /
  `mkkey.` bindings. The overlay rebinding rows and `pad_bindings.h` need chord syntax.
- **Effort:** 4-6 days, with a migration of the bindings in bbport.ini.
- **Status (next release after 0.6.4):** the button prompts are ported (`gpu/shim/bbport_button_prompts.*`,
  `renderer_vulkan/vk_button_prompts.*`, the texture-cache upload and `BindTextures` hooks, the prompt part of
  `runtime_pad.c` as `src/pad_prompts.h`), with Xbox / Nintendo / Keyboard sets and `prompt_style` added.
  Not taken from the rewrite: the DS III-like default layout (a different muscle memory, users' saved
  `key.` / `mkkey.` lines; no player gain), the chord bindings (our only chords are the built-in
  Shift + click R2 / L1, which work already; chords need a new capture UI in the rebinding rows), the native
  camera detour (our *Direct mouse camera*, imedved's, already turns the camera 1:1, and the stick path
  keeps the measured curve of docs/SETTINGS.md), and the menu mouse pointer (not part of this task).

**Suggested order:** frame-rate hooks first, then the mouse camera and button prompts. Leave the
memory model off by default on Windows. Plan the renderer import as its own rebase project.

## BBSC ideas for party

BBSC (seamless co-op in upstream's online module, `gpu/bbnet/shadps4/core/bbsc_*` and
`bloodborne_re.cpp`; GPL-2.0-or-later, CLS_AMG) is server-based. Only game addresses and facts are
listed here; no code was copied. Addresses are eboot offsets of CUSA03173 1.09. None of these has
been checked here yet.

| Address / flag | What it does in BBSC | Our ERSC_PARITY row and how it would help |
|---|---|---|
| `HostDead` 0x1381870 and `HostDead_1` 0x1381A60 made `ret` | On a guest, the host's death sets flag 4047, the death-pending byte `[0x553B130]+0x70`, the menu brake `MenuMan+0x2DC`, `*0x553B0D8=2` and `GSM+0x1520=1`, then sends the guest home. BBSC skips both functions. | **C2**: skip where the locks come from instead of only the send-home call (0x18C4D7D). This could make the no-load host revive (`BB_PARTY_B2_HOST_REVIVE`) the default and fix playtest 1's pickup and ladder locks after a host revive. |
| Death-lock clear for 15 s after any revive in place | Clears `MenuMan[0x5562878]+0x2DC`, `[0x553B130]+0x70`, flag 4047 (via SetEventFlag 0x132E5A0), and the slot "leaving" bytes `[0x553B130]+0xA9+slot*2` (set via 0x14EB860). | **C1 / C2 / H4**: today we only zero GSM +0x1522 and +0x1523. |
| Death request `[[0x553B108]+0x10]+8` (4) and flag 4000 set back to 0 after a kept death | 0x13813D0 sets them. While they are set, the stage flow does not arm map changes and the death handler returns early. | **C1**: the game's own death script runs on the next death, with no 11 s dead-watchdog wait. Lamp travel and map changes no longer stall after a kept death. |
| `GSM+0x1520`, `+0x1524` and `*0x553B0D8` zeroed after a kept death | This is what the reload 0x13129C0 does. With +0x1520 left set, a later load put the player at (0, y, 0), under the map. | **C2 / C6**. |
| SprjFade plate `[[0x5540068]+0x20]` (+0x1C RGBA target, +0x38 seconds) and NowLoading state 0x176D320 | A "death curtain": fade out, the game's loading screen, revive and warp in the dark, fade back in, with no real load. The same plate gives a black cover from the summon until the guest is placed. | **C2 / H1**: a death that looks vanilla, with no load. **A4 / B4**: no map flash or void on join. We already use 0x176D380 / 0x176D3C0. |
| Remote revive in place: 0x18BCF30(chr, 1), Idle_wild 0x1A19F60, `chr+0x208`=(1,1), clear `[[chr+0x58]+0x38]+0x48` and `+0x114` | Revives the remote copy when its synced HP comes back, instead of re-creating it. | **C1**: the host's view of a re-created guest can be a ghost. |
| `0x17897AE`: `E9 03 00 00` -> `30 75 00 00` | Raises the unsent-message queue limit in 0x17894E0 from 1001 to 30000. Hitting the limit raises OnIrregularLeaveSession. | **F5 / C6**: long outages or a member who stops answering no longer end the session. Today `nr_outage.h` works around it. |
| `0x18C52F9`: `0F 84` -> `EB 04` | The kill reward 0x18C5070 also pays the boss's Insight to a guest's own copy of the kill. | **G2**: each player gets the boss Insight natively instead of from the value the host sends. |
| EMEVD condition evaluator 0x17A3BE0 (cond +0x28 entity, +0x2C type, result in bit 0 of +0x10) | Answers "yes" for chosen host-only events (Doll level-up 12100112/113, messengers). | **B2 / D5 / D10**: a per-event way to let the guest's copy of an event run. |
| SetBossGauge 0x1336C80 / ClearBossGauge 0x1336D00; `Failed_BossAreaMission` 0x138B2D0 | The game clears the boss bars before the death step, so BBSC records the bar every tick. | **C1 / C3**: a "died in a boss fight" test that still holds after the bar is cleared. |
| More host-death sites: 0x18C4DAC (RemoveMemberTwin 0x1E55050) and 0x18C4DBA (MarkSlotsLeaving 0x14EB860); boss-area leave 0x138B410 / 0x138B437 | The rest of the host-death kick, and the BossArea stop and cleanup. | **C2 / C3**: we patch only 0x18C4D7D. |
| Banner rewrites (role table 0x553D750, banner fill 0x1AE78B0, text at 0x5581410) | The game's own banner lines (103101, 103001, 105200-105500) are rewritten instead of hidden. | **E4 / H**: polish. |

**Already used by us:** the summon and remove-member calls (party_b2), the death reload
0x1381EDE / 0x1382663, the talk and lamp checks, MultiDoping 0x138BCC2, the floor guard
0x18CB0E0, the event flag layout, and the echo halving (our 0x18FC93F / 0x1335582 do the same as
BBSC's 0x18FC94B / 0x133558D). BBSC's personal-loot patches (0x17D3498 and others) are reference
only, because our D4 shares one roll between players.

**Most valuable:**
1. C2 / C1: make `HostDead` and `HostDead_1` return at once, and clear the death locks and leftover globals.
2. C1 / H1: re-arm the game's death script after a kept death, and use the SprjFade curtain.
3. F5 / C6: raise the unsent-message queue limit at 0x17897AE from 1001 to 30000.
