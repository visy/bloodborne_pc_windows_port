# Settings, launcher and controls

Back to the [README](../README.md).

## In-game settings

- **Settings inside the game's own menu**: every setting of the port is a page of the game's *System* menu (after *Environment*, i.e. Screen/Sound, on the title screen and in the pause menu), built and drawn by the game itself, in the game's language (English or Russian): *Display* (output resolution, preset, sharpness, FPS counter, model detail, upscaler), *Effects* (the graphics patches), *Upscaler* (jitter, character motion vectors, reactive mask, FSR 4 options, live resolution), *Advanced* (reactive mask tuning, async shaders, pipeline libraries, debug view), *Frame generation* (DLSS / FSR frame generation, NVIDIA Reflex; after a restart, see [FG_REFLEX.md](FG_REFLEX.md)), *HDR* (HDR output, peak brightness, paper white, highlights, saturation, calibration pattern; see [HDR output](#hdr-output)), *Performance HUD* (on/off, opacity, size, shader indicator, the port's language, position), *HUD details* (graph, VRAM, GPU, CPU, RAM), *Mouse & keyboard* (on/off, sensitivity, invert), *Mouse tuning* (smoothing, dead zone, the wheel as quick items), *Game patches* (the game's own AA, skip intros, free camera, debug menu, *Restart the game*, *Quit the game*), *Party* and *Party members* (see [docs/PARTY.md](docs/PARTY.md)). Changes apply at once and are saved to `bbport.ini`; the bottom line explains each row (slider positions included). Buttons are rows switched with left/right; *Leave*, *Kick*, *Restart* and *Quit* need a second press. The game's *Default* button restores the port's defaults. `BB_GAME_MENU=0` leaves the game's menu untouched.
- **In-Game Overlay**: Press `Insert` (or `L3 + R3` on your controller, when enabled in the launcher) to open the overlay menu, styled like the game's dialogs: the same settings with longer descriptions, with gamepad, keyboard and mouse. It is a fallback (and a debugging aid); normal settings are all in the game's own menu.
- **Mouse & keyboard mode** (opt-in: launcher, `BB_MOUSE_KEYBOARD=1`, `mk_enabled=1`, or the menu): the mouse turns the camera and attacks, the wheel steps the quick items and tools (d-pad up / down, one press per notch; `mk_wheel=0` turns it off). `mkkey.<input>=` lines in `bbport.ini` rebind this layout; mouse buttons and the wheel are names there too (`Mouse Left`, `Mouse Right`, `Mouse Middle`, `Mouse X1`, `Mouse X2`, `Wheel Up`, `Wheel Down`), and a mouse button bound this way leaves the built-in mouse scheme (e.g. `mkkey.square=R, Mouse X1`). From upstream 0.51 (#51, #52); see [UPSTREAM_0.51_INTEGRATION.md](UPSTREAM_0.51_INTEGRATION.md).

Output resolution (in-game Display page and System menu) also offers ultrawide sizes, 21:9 (2560x1080, 3440x1440, 3840x1600, 5120x2160) and 32:9 (3840x1080, 5120x1440); they apply after a restart (startup patch, never live) and need an upscaler other than TAA.

### HDR output

Optional, **off by default**: an HDR10 picture (SMPTE ST 2084 PQ, Rec.2020 primaries) for an HDR display. Turn it on in the launcher (*Graphics > HDR output (HDR10)*), in the game's *System > HDR* page or the Insert menu (`hdr=1` in `bbport.ini`, `BB_HDR=1`); it applies at the next start and needs **HDR on in Windows** (*Settings > Display > Use HDR*) for the game's display. Bloodborne itself renders an SDR picture; the port keeps it as it is and expands it at the very end:

- the game's colours are decoded as the TV gamma 2.2 they were graded for, then only the highlights are expanded: everything below about 73 % of the SDR signal (linear luminance 0.5) stays where **paper white** puts it, brighter parts rise smoothly towards the **peak**, on luminance only (a lamp's orange stays orange, no hue shift, no channel clipping), converted to Rec.2020 and PQ encoded; colours above the peak are scaled down as a whole;
- the port's own overlay (menus, party HUD and name tags, performance HUD, notices and captions) is drawn at paper white, never blown out, and blends as it does in SDR;
- while one of the game's full-screen menus is up (pause / inventory, equipment, status, System, the lamp's travel list) the highlights fade out over a quarter second, so the game's white menu text sits at paper white like any HDR game's UI, and fade back in when it closes (`BB_HDR_MENU_SDR=0` keeps them; the calibration pattern always shows the curve as set);
- works with every upscaler (TAA, FSR 3.1, FSR 4, DLSS), with DLSS / FSR frame generation and with the ultrawide outputs: the HDR pass runs after them on the final picture;
- when the display or driver offers no HDR10 swapchain, or HDR is off in Windows, the game presents SDR as before (with the `BB_HDR_GAMMA` correction on an HDR desktop) and *System > HDR* says why (log: `HDR: output asked for but not used: ...`).

| Setting (System > HDR) | `bbport.ini` | Environment | Default |
|---|---|---|---|
| HDR output (after a restart) | `hdr=0/1` | `BB_HDR` | 0 |
| Peak brightness (cd/m2; Auto = what the display reports to Windows, else 1000) | `hdr_peak_nits=auto/400..10000` | `BB_HDR_PEAK` | auto |
| Paper white (mid-tones and the port's menus) | `hdr_paper_white=80..500` | `BB_HDR_PAPER_WHITE` | 200 |
| Highlights (0 = the SDR look at paper white, 1 = the game's white reaches the peak, up to 1.5) | `hdr_strength` | `BB_HDR_STRENGTH` | 1.0 |
| Saturation (1 = the game's colours; above 1 uses the wider Rec.2020 gamut) | `hdr_saturation=0.5..1.5` | `BB_HDR_SATURATION` | 1.0 |
| Calibration pattern (not saved) | - | `BB_HDR_PATTERN=1` | off |

The **calibration pattern** fills a band at the top of the screen (the System menu stays usable below it): a 10000-nit field with a square at the peak setting (raise *Peak brightness* until the square just disappears into the field: that is your display's real peak), a paper white patch, a ramp from black to the peak, and the game's grey levels 0, 0.1 ... 1.0 through the current curve. Test aids: `BB_HDR_FORCE=1` creates the HDR10 swapchain even with HDR off in Windows (Windows then maps it down), `BB_HDR_FORMAT=scrgb` prefers scRGB (`R16G16B16A16_SFLOAT`, extended linear sRGB) over HDR10, `BB_HDR_DUMP_TRIGGER=<file>` saves the SDR frame and the encoded HDR image once the file appears (checked by `tools/windows/hdr_check.py`), `BB_HDR_TEST_FILE=<file>` changes the curve from a file (`pattern`, `peak=N`, `paper=N`, `strength=F`, `saturation=F`). The log lists the swapchain formats the display offers (`Swapchain: formats offered:`) and the display's reported peak (`Display: HDR on in Windows ..., peak N nits reported`). shadPS4's passthrough for PS4 games that output HDR themselves is `BB_GAME_HDR=1` (Bloodborne does not).

## Launcher

Double-click `launch_gui.bat` or run:
```cmd
launch_gui.bat
```
- Select your `eboot.bin` file or game folder.
- **Display**: target frame rate (60 FPS recommended), render resolution, window mode (`BB_WINDOW_MODE`: *Windowed*, *Borderless* or exclusive *Fullscreen*), monitor (`BB_DISPLAY`), frames ahead (1 = least input lag, 2 = default, 3 = smoothest).
- **Graphics**: upscaler, object motion vectors, anisotropic filtering, frame generation (Off / DLSS FG 2x-4x / FSR FG 2x) and NVIDIA Reflex (Off / On / On + Boost; both game settings in `bbport.ini`, off by default, see [FG_REFLEX.md](FG_REFLEX.md)), HDR output with its peak brightness and paper white ([HDR output](#hdr-output); off by default), VRAM pre-upload (*Full* needs a 12 GB+ GPU).
- **Performance & Shaders**: *Performance Patch*, draw preparation workers, shader precompile, async shaders, GPL, the compile indicator, and **Export / Import shader cache**.
- **Game**: skip intro, skip the online/offline choice, settings in the game's System menu, mods, and *All XML Patches...*.
- **Input & Audio**: controller (`BB_GAMEPAD`), overlay on `L3 + R3`, mute in background.
- **Advanced / Debug**: diagnostics, watchdog, party logging, support bundle, and the [shader cache builder](#shader-cache-builder-world-warper).
- **Party**: host or join a co-op party, connection settings, *Rules & sync...* and *Party help* ([guide](docs/PARTY.md)).
- Hover any option for details, then click **Launch Bloodborne**. `run.bat` started on its own uses the same saved settings.

### Shader cache builder (world warper)

*Advanced / Debug > Shader cache builder > Build shader cache (world warper)...* fills the shader cache by playing the game for you, so later play stutters less in places you have not been yet:

- **What it does**: the game starts in a test mode (`BB_WORLD_WARPER=tour`), continues your save on the title screen and then travels through every ReturnPointParam row with a spot - the Hunter's Dream, all 44 lamps and 23 boss-arena / event respawn points (68 places in all; the Chalice Dungeons are left out) - with the game's own lamp warp and one normal loading screen each. In each place it turns the camera all the way round, walks a loop, fights and kills the two nearest enemies (within 60 m), and in the first place of each area uses the weapon in both forms (R1, R2, charged R2, firearm, transform, dodge); once per run it switches through the other weapons and quick items. Your HP is kept full meanwhile.
- **Your saves are never changed**: it plays on a **copy** of your save folder (`user\world_warper\savedata`, made when you start; the warper moves the copy's character, uses its items and changes its last lamp). The shader cache it fills is your own (`user\cache`).
- **Progress**: a panel at the top of the game window (place n / 68, the area, what it does, the pipelines new to the cache here and in all) and a line per place in the launcher log (`World warper: [12/68] Old Yharnam, row 2302950 (m23_00_00_00): +N new pipelines (in the world: enemy ..., look ...)`).
- **Stop and resume**: *Stop* ends it; pressing the button again asks whether to continue where it stopped (`user\world_warper\world_warper_state.txt`) or start over. If the game crashes the launcher starts it again and it continues; a place it crashed in twice is skipped.
- **The result**: at the end the game closes and the launcher exports the shader cache as one `.zip` (the *Export shader cache* format) where you chose when starting. It is made from the game's shaders: use it yourself or share it only with people who own the game. The project never publishes such caches.
- It takes about an hour (measured: 56 places in 50 minutes on a test PC, plus the loading screens). Leave the window alone meanwhile; mouse, keyboard and pad input would mix with the warper's own.

Measured (test PC, 30 FPS test settings, an empty cache): the tour made 425 pipelines new to the cache in 56 places (most from enemies, 154, and the camera sweeps, 114; the rest on the loading screens and while walking and attacking). A replay of the same walk through two places before and after the tour in one session: 61 -> 1 new pipelines, 53 -> 1 draws skipped for a pipeline still compiling, 342 -> 2 ms of compiling on the GPU thread. Long frames right after a loading screen (texture and map streaming) are not shader stutter and stay; the log counts both (`hitches over 50 ms`, and those with a pipeline made in the frame).

Test aids: `BB_WORLD_WARPER=tour|replay|measure` with `,lamps=A+B`, `,replay=A+B`, `,max=N`, `,noact`, `,noenemy`, `,nokill`; `BB_WORLD_WARPER_STATE=<file>` (see [party/DEBUG_SWITCHES.md](party/DEBUG_SWITCHES.md)). `measure` runs the replay, the tour, and the replay again in one session and logs `stutter check: replay before / after the tour`.

### Command line
Run `run.bat` pointing to your game directory:
```cmd
run.bat "C:\Games\Bloodborne\CUSA03173"
```

## Controls

- **Gamepad**: Controllers are supported out of the box through SDL3. With several connected, `BB_GAMEPAD=<GUID or part of the name>` picks one. The keyboard works too, also next to a connected gamepad. The character name is typed on the keyboard in a box over the game.
- **Touchpad**: its left half (`Tab`, Back/Select) opens the gestures, the right half (`Backspace`) the key items. Pads without a touch surface (Xbox and other XInput pads): `Back` + d-pad left / right clicks the left / right half for as long as the d-pad is held, and `Back` alone clicks the left half when it is released (so `Back` + right never opens the gestures first). While `Back` is held, d-pad left / right do not switch items. This applies while `Back` is bound to nothing but the touchpad (the default); a `pad.<input>=` line that moves `Back` elsewhere keeps the plain rule, and pads with a real touchpad are unchanged.
- **In-Game Settings**: *System* in the game's own menu (*Display*, *Effects*, *Upscaler*, *Advanced*, *HDR*, *Performance HUD*, *HUD details*, *Mouse & keyboard*, *Mouse tuning*, *Game patches*, *Party*, *Party members*), or press `Insert` on keyboard (`L3 + R3` on gamepad when enabled in the launcher) to toggle the overlay menu (its last row, *Quit the game*, closes the game after a second press).
- **Free Camera**: When enabled in the launcher or overlay, hold `Cross` and press `L3` (or hold `Space` and press `Z` on keyboard) to cycle camera modes. Uses Lance McDonald's GoldHEN patch; conflicts with *Enemy Control*.
- **Debug Menu**: If debug fonts (`DbgFont14h.ccm` and `DbgFont14h.tpf`, from [Debug Menu and XML Patch](https://www.nexusmods.com/bloodborne/mods/253)) are installed into `dvdroot_ps4/font/` (or `adhoc/font/`), open the menu using `Tab` or the left half of the controller touchpad. Startup rejects missing or empty font files instead of launching the unsafe patch.

## Environment variables

`BB_FRAME_STATS=1` (frame statistics), `BB_TRACE=1` (heartbeat and hang dumps in `out\`, cheap), `BB_TRACE_IMPORTS=1` (also logs every import call: much slower), `BB_SAVE_LOG=1` (frame and readback statistics into `logs\<time>.frames.csv` / `.readbacks.csv`; the launcher's own log is `launcher.log`), `BB_ANISO=N` (anisotropic filtering; 16 by default, 0 = the game's own), `BB_UPSCALER=taa|fsr3|fsr4|fsr411|dlss|off`, `BB_FRAMES_AHEAD=N`, `BB_PRESENT_THREAD=0`, `BB_LIVE_RES=1`, `BB_PREUPLOAD=0|1|2` (background upload of GPU memory into VRAM: 1 by default, 2 = all of it, ~3 GB more VRAM, 0 = off), `BB_READBACKS=0|1|2`, `BB_GPU_PROFILE=1`, `BB_BREADCRUMBS=0`, `BB_DISPLAY`, `BB_HDR_GAMMA=0` (no gamma 2.2 correction of the SDR picture when Windows runs the display in HDR), `BB_HDR=1`, `BB_HDR_PEAK`, `BB_HDR_PAPER_WHITE`, `BB_HDR_STRENGTH`, `BB_HDR_SATURATION`, `BB_HDR_PATTERN=1`, `BB_HDR_FORCE=1`, `BB_HDR_FORMAT=scrgb`, `BB_HDR_MENU_SDR=0` (native HDR output, see [HDR output](#hdr-output)), `BB_GAMEPAD`, `BB_SKIP_NETWORK_CHOICE=0`, `BB_GAME_MENU=0`, `BB_SKIP_GAME_CHECK=1`, `BB_WINDOW_MODE=windowed|borderless|fullscreen` (fullscreen is exclusive at the desktop display mode), `BB_FULLSCREEN=1` (desktop fullscreen when `BB_WINDOW_MODE` is unset), `BB_MUTE_UNFOCUSED=0`, `BB_OVERLAY_PAD=1` (overlay on `L3 + R3`), `BB_AUTO_DLC=0`, `BB_PREUPLOAD_FORCE=1`, `BB_STREAM_MEMORY=device|cached|host` (where the per-draw stream buffer lives: VRAM by default; `cached` / `host` put it in cached / uncached system memory, which were slower on an RTX 3070), `BB_SHADER_PRECOMPILE=0`, `BB_DRIVER_PIPELINE_CACHE=0` (the driver's pipeline binaries are not kept between sessions), `BB_ASYNC_SHADERS=0` / `BB_ASYNC_PIPELINES=0` (async compilation off; with `BB_ASYNC_SHADERS_POLICY=balanced|aggressive`, `BB_ASYNC_SHADERS_MAX_SKIP_FRAMES`, `BB_ASYNC_SHADERS_QUEUE`, `BB_SHADER_THREADS`, `BB_ASYNC_PIPELINE_THREADS` (background workers while playing: half the threads, 2 to 8, by default), `BB_ASYNC_PIPELINE_STREAK=N` (a pass may go without its new pipeline once its targets were drawn in each of the last N frames; 2 by default)), `BB_GPL=1` (`BB_GPL_STATS=1`), `BB_COMPILE_INDICATOR=0`, `BB_COMPILE_PROGRESS_TEST=1`.
More in [docs/](docs); recent upstream changes: [docs/CHANGES_after_0.4.md](docs/CHANGES_after_0.4.md), [docs/CHANGES_0.4.md](docs/CHANGES_0.4.md) (in Russian), [docs/CHANGES_2026-10-06.md](docs/CHANGES_2026-10-06.md).
