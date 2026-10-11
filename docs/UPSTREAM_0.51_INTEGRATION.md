# Upstream 0.51 integration (deadinside28/bloodborne_pc 3c57da4)

Upstream commit `3c57da4` "0.51: online play (shadNet module), mouse look, DLSS in the AppImage,
fixes, 399 MB AppImage" (deadinside28; 170 files, +70k lines) on top of our merge base `c7e3e55`
(0.5). It was ported by hand onto `windows-upstream-0.5` (branch `agent/up051`), not merged: most
of its size is the shadNet module that this fork deliberately does not use. Every ported piece
says "upstream 0.51" in a comment; the commit credits upstream ("Ported from upstream 3c57da4
(deadinside28)"). Upstream's own notes are kept as [CHANGES_0.51.md](CHANGES_0.51.md) and
[CHANGES_after_0.5.md](CHANGES_after_0.5.md) (they describe upstream's Linux build, online play
included; this fork's differences are below).

## Taken as is (generic fixes)

| Upstream change | Files | Notes |
|---|---|---|
| Every retail release with update 1.09 gets the game profile (#119, PR #117) | `gpu/games/bloodborne.cpp` (serials), `scripts/game_check.py` (`SUPPORTED_TITLES`), `scripts/mods.py`, `scripts/patches.py`, `src/runtime_savedata.c` (CUSA03179 in the sound fix), `tests/test_game_check.py` | Without its serial a region ran on the generic paths and the new memory model crashed on entering the world. The check also reports an `eboot.bin` with Lance McDonald's 60 fps patch baked in (`patched_eboot`). |
| Motion blur smear in the new memory model (#122) | `buffer_cache.cpp` (`IsRegionGpuModified` counts GPU-written VRAM copies, `BB_GPU_DATA_CHECK=0` off), `texture_cache.cpp` (`DownloadImageMemory` also copies into the VRAM copy) | Diagnostics `BB_GARLIC_VRAM_RANGE=begin,end`. |
| 4 GB VRAM "Failed to allocate 64 MiB for guest memory copies" stop | `buffer_cache.cpp` `AllocateResidency` (smaller block, other memory types), `texture_cache.{h,cpp}` `NoteVramShort` | |
| Title menu flicker above 1080p | `ui_composition.h` (`MovieShader` hashes), `vk_temporal_upscaler.{h,cpp}` (`BB_UI_TRACE=1`) | |
| Swapchain recreated through `oldSwapchain` (PR #93) | `vk_swapchain.cpp` | Written for NVIDIA Wayland; standard Vulkan practice, harmless on Windows. |
| DLSS bridge log without format arguments | `gpu/dlss_bridge/bbport_dlss_bridge.cpp` | |
| `--read-input` names mouse buttons and the wheel | `tools/gpu_capabilities.c` | Upstream's garbled comment fixed. |
| Linux launcher's game-check texts (the `patched_eboot` kind would otherwise raise `KeyError`) | `launcher/bbport_launcher.py`, `launcher/bbport_i18n.py` (those strings only) | |

## Adapted

* **Garbage collector under VRAM shortage** (`texture_cache.cpp`). Upstream bypasses its
  seconds-based eviction while `vram_short_until` is set; our Windows fork has its own tick ladder
  (pressure / critical / emergency). Merged as one more rung: while VRAM is short (an allocation
  failed, or usage is over 92 % of the driver's budget), a pressured or critical pass evicts by
  submissions (80 / 160 ticks) as before 0.5.
* **DLSS runtime choice** (`vk_dlss.cpp`, `launcher.py`). Upstream's `UserDlssDirectory` /
  `NgxDirectory` / `BridgeDirectory` were moved out of the Linux-only block so Windows gets them:
  NVIDIA's `nvngx_dlss.dll` is looked for in `<user>\dlss` first, then next to `bbport.exe`; the
  bridge `bbport_dlss.dll` next to the exe, else in `<user>\dlss`. The Windows launcher got a
  *DLSS library: Choose... / Remove* row (Graphics → Image quality) that copies the chosen DLL to
  `user\dlss\nvngx_dlss.dll`. The path is printed as UTF-8 (`u8string`, non-ASCII user folders).
  `HasNgx` no longer throws on an unreadable folder.
* **Mouse look and mouse / wheel bindings (#51, #52)**. Our fork already had mouse look: the
  opt-in *mouse & keyboard* mode (`mk_*` settings, from Mrsuss60's port): relative mouse while the
  game has the input and the window has focus, speed-to-stick look with sensitivity, invert,
  smoothing and dead zone, and the mouse buttons as attacks. Upstream's separate click-to-capture
  mouse look (`mouse_look`, `mouse_sensitivity`, `mouse_invert_y`, F1 releases) would duplicate it
  and, on by default, change single-player for everyone; F1 is *Options* in our layouts. So only
  what ours lacked was ported, into the mouse & keyboard mode:
  * the **wheel** as d-pad up / down (quick items and tools), one 60 ms press per notch
    (upstream's `mouse_wheel_step`); setting `mk_wheel` (default on), rows in the game's System menu
    (*Mouse tuning → Mouse wheel*), the overlay's Controls tab and the launcher's mouse & keyboard
    dialog; the launcher's tuning reload picks it up while the game runs;
  * **mouse names in bindings**: `Mouse Left`, `Mouse Right`, `Mouse Middle`, `Mouse X1`,
    `Mouse X2`, `Wheel Up`, `Wheel Down` in `mkkey.<input>=` lines (upstream's `key_from_name`); a
    mouse button named in a binding leaves the built-in scheme (e.g. `mkkey.square=R, Mouse X1`
    makes X1 Square instead of L1). The mouse is only read in mouse & keyboard mode, so such names
    in classic `key.` lines do nothing.

  The System menu's *Mouse & keyboard* page holds six rows (the game's layout), so smoothing, dead
  zone and the wheel moved to a new page *Mouse tuning* (11 pages of the 12 the menu allows).
  With mouse & keyboard mode off nothing changes: the wheel is not read and the classic layout has
  no mouse bindings.
* **Quit from the overlay (#74, #116)**. Upstream added a *Quit game* button (press twice) beside
  *Close*. Ours: a *Quit the game* row at the bottom of every overlay tab (second press within
  5 s), and the same confirmed button on the System menu's *Game patches* page next to *Restart
  the game*. Both call `bbgpu_request_quit()`, which pushes `SDL_EVENT_QUIT`: the window thread
  then ends the process exactly as the window's close button does, including the party's goodbye
  (`bbnet_shutdown`, BYE + UPnP unmap). Settings are saved first.
* **Test aids** (needed to check the above without OS input): `BB_PAD_FILE` takes
  `mouse=DX,DY` (pixels per second while listed), `mouse_left|right|middle|x1|x2`,
  `wheel_up|wheel_down` (one notch per file change) and `key=NAME` (an SDL key press queued into
  the game window, e.g. `key=Insert`, no focus needed); the pad file is now read before the
  overlay capture check so `key=` steps also drive the open menu. `BB_MOUSE_TRACE=1` logs the look
  stick and mouse state once a second. The overlay logs `Overlay: menu opened/closed`,
  `Overlay: quit armed`, `Overlay: quit`.
* **Docs**: README (any retail release, the 60 fps eboot warning, *Mouse tuning*, *Quit*).

## Skipped

| Upstream change | Why |
|---|---|
| shadNet online module: `gpu/bbnet` (`libbbnet.so`, `bbnet_glue.cpp`, vendored shadPS4/shadp2p network and NP libraries, the shadNet client and protobuf), `gpu/third_party/httplib`, `src/runtime_net.c`, the `runtime_services_resolve` hook, `runtime_thread_spawn`/`join_spawned`, the `probe.c` call, `tests/test_online.c`, `scripts/online.py`, `BB_NET` in `gpu/CMakeLists.txt`, the launcher's *Online* page | It needs a central server (shadNet `srv.shadps4.net`, The Hunter's Dream for messages and bloodstains). Our multiplayer is the serverless party co-op (`gpu/shim/party`, `gpu/shim/net`): the host's process emulates FROM's server and matching in-process, guests join by party code. The upstream module answers every `sceNet*` / `sceNp*` / `sceHttp*` / `sceSsl*` import itself when `BB_ONLINE=1`; it would bypass our NP and network layer entirely, so it cannot coexist with the party layer. It is Linux-only as written (`dlopen`, `/proc/self/exe`). |
| Its UPnP mapping (#121) | Part of the shadNet module (miniupnpc, `net_upnp.cpp`); the party layer already maps its port by UPnP (`BB_PARTY_UPNP`, unmapped in `bbnet_shutdown`). |
| NP / network stub changes | None outside the module: upstream's only change to the runtime's imports is the `BB_ONLINE` redirect in `runtime_services_resolve`, not taken. Our NP layer and party stubs are unchanged. |
| Upstream's click-to-capture mouse look, `mouse_look` / `mouse_sensitivity` / `mouse_invert_y` keys, F1 release, classic-layout mouse bindings by default | Duplicates our mouse & keyboard mode (above); default-on would change single-player. |
| AppImage packaging (`packaging/appimage.sh`, `packaging/default.nix`, `shell.nix`), `build.sh` (#124 FSR patch reset, `bbnet` target), `run.sh` (online env), `launcher/bb-launcher.sh` (nix-shell, PR #120), `tools/fsr4cap/capture_all.sh` (SteamOS, PR #89), HTTPS certificates | Linux packaging and scripts; the Windows build uses `build.bat` / CMake with MinGW. |
| Linux launcher's mouse-look rows and DLSS `.so` chooser, upstream README / README.ru changes | Linux launcher only; our README was updated by hand. |

## Verification (2026-10-11)

* Build: `tools/agent/build.py out/gpu` (MinGW GCC 16, non-LTO), all targets and every test target.
* Unit tests: every `*-test.exe` in `out` and `out/gpu` passes (35 binaries, all `party-*-test`,
  `party-security-test`, `game-menu-pages-test` (11 pages, 61 fields, the *Quit the game* button
  needs two presses), `settings-test`, `pad-test` with new checks: mouse names, a bound side button
  leaving the built-in scheme, the wheel's press/release cycle, pad-file mouse tokens). `pytest
  tests`: 109 passed, 34 skipped (`test_game_check.py` with upstream's new cases).
* Live, solo (one instance, `BB_MOUSE_KEYBOARD=1`, `BB_MOUSE_TRACE=1`, pad-file steps, no OS input,
  background window): `mouse=3000,0 mouse_right` → `right stick 255,128 buttons 0x2 (pad 0x100)`
  (camera full right, right button = L2); `mouse=0,-2000` → `right stick 128,1`; `wheel_down` →
  `wheel 0/1`; `key=Insert` → `Overlay: menu opened`; `key=Up`, `key=Return` →
  `Overlay: quit armed (press again)`; `key=Return` → `Overlay: quit` and the game exited with
  code 0 at once.
* Live, party smoke (two loopback instances, `instances.py run --count 2 --party`, test saves in the
  Hunter's Dream, `BB_PARTY_DIRECTOR_TEST=host_warp=2412951@12`, build with the merged
  `windows-upstream-0.5`): bell join - guest `Net: np: joined room 1001 as member 2`, `test: travel
  steps: joined #1` (12:51:00.8), host `Hunter1 joined room 1001`; lamp travel - host lamp warp to
  Central Yharnam (`lamp 2412951`, m24_01), guest `Party B2: guest: host travels ... lamp 2412951`,
  `Party travel: replay now ... exact` (12:51:25.3), host `cooperators 0 -> 1` again in m24_01 at
  12:51:32.9 (15.5 s after the warp), both role host(3) / client(6) with each other's chr present.
  Ended by the harness' idle reaper (no errors).
