# Settings, launcher and controls

Back to the [README](../README.md).

## In-game settings

- **Settings inside the game's own menu**: every setting of the port is a page of the game's *System* menu (after *Environment*, i.e. Screen/Sound, on the title screen and in the pause menu), built and drawn by the game itself, in the game's language (English or Russian): *Display* (output resolution, preset, sharpness, FPS counter, model detail, upscaler), *Effects* (the graphics patches), *Upscaler* (jitter, character motion vectors, reactive mask, FSR 4 options, live resolution), *Advanced* (reactive mask tuning, async shaders, pipeline libraries, debug view), *Performance HUD* (on/off, opacity, size, shader indicator, the port's language, position), *HUD details* (graph, VRAM, GPU, CPU, RAM), *Mouse & keyboard* (on/off, sensitivity, invert), *Mouse tuning* (smoothing, dead zone, the wheel as quick items), *Game patches* (the game's own AA, skip intros, free camera, debug menu, *Restart the game*, *Quit the game*), *Party* and *Party members* (see [docs/PARTY.md](docs/PARTY.md)). Changes apply at once and are saved to `bbport.ini`; the bottom line explains each row (slider positions included). Buttons are rows switched with left/right; *Leave*, *Kick*, *Restart* and *Quit* need a second press. The game's *Default* button restores the port's defaults. `BB_GAME_MENU=0` leaves the game's menu untouched.
- **In-Game Overlay**: Press `Insert` (or `L3 + R3` on your controller, when enabled in the launcher) to open the overlay menu, styled like the game's dialogs: the same settings with longer descriptions, with gamepad, keyboard and mouse. It is a fallback (and a debugging aid); normal settings are all in the game's own menu.
- **Mouse & keyboard mode** (opt-in: launcher, `BB_MOUSE_KEYBOARD=1`, `mk_enabled=1`, or the menu): the mouse turns the camera and attacks, the wheel steps the quick items and tools (d-pad up / down, one press per notch; `mk_wheel=0` turns it off). `mkkey.<input>=` lines in `bbport.ini` rebind this layout; mouse buttons and the wheel are names there too (`Mouse Left`, `Mouse Right`, `Mouse Middle`, `Mouse X1`, `Mouse X2`, `Wheel Up`, `Wheel Down`), and a mouse button bound this way leaves the built-in mouse scheme (e.g. `mkkey.square=R, Mouse X1`). From upstream 0.51 (#51, #52); see [UPSTREAM_0.51_INTEGRATION.md](UPSTREAM_0.51_INTEGRATION.md).

Output resolution (in-game Display page and System menu) also offers ultrawide sizes, 21:9 (2560x1080, 3440x1440, 3840x1600, 5120x2160) and 32:9 (3840x1080, 5120x1440); they apply after a restart (startup patch, never live) and need an upscaler other than TAA.

## Launcher

Double-click `launch_gui.bat` or run:
```cmd
launch_gui.bat
```
- Select your `eboot.bin` file or game folder.
- **Display**: target frame rate (60 FPS recommended), render resolution, window mode (`BB_WINDOW_MODE`: *Windowed*, *Borderless* or exclusive *Fullscreen*), monitor (`BB_DISPLAY`), frames ahead (1 = least input lag, 2 = default, 3 = smoothest).
- **Graphics**: upscaler, object motion vectors, anisotropic filtering, VRAM pre-upload (*Full* needs a 12 GB+ GPU).
- **Performance & Shaders**: *Performance Patch*, draw preparation workers, shader precompile, async shaders, GPL, the compile indicator, and **Export / Import shader cache**.
- **Game**: skip intro, skip the online/offline choice, settings in the game's System menu, mods, and *All XML Patches...*.
- **Input & Audio**: controller (`BB_GAMEPAD`), overlay on `L3 + R3`, mute in background.
- **Party**: host or join a co-op party, connection settings, *Rules & sync...* and *Party help* ([guide](docs/PARTY.md)).
- Hover any option for details, then click **Launch Bloodborne**. `run.bat` started on its own uses the same saved settings.

### Command line
Run `run.bat` pointing to your game directory:
```cmd
run.bat "C:\Games\Bloodborne\CUSA03173"
```

## Controls

- **Gamepad**: Controllers are supported out of the box through SDL3. With several connected, `BB_GAMEPAD=<GUID or part of the name>` picks one. The keyboard works too, also next to a connected gamepad. The character name is typed on the keyboard in a box over the game.
- **Touchpad**: its left half (`Tab`, Back/Select) opens the gestures, the right half (`Backspace`) the key items.
- **In-Game Settings**: *System* in the game's own menu (*Display*, *Effects*, *Upscaler*, *Advanced*, *Performance HUD*, *HUD details*, *Mouse & keyboard*, *Mouse tuning*, *Game patches*, *Party*, *Party members*), or press `Insert` on keyboard (`L3 + R3` on gamepad when enabled in the launcher) to toggle the overlay menu (its last row, *Quit the game*, closes the game after a second press).
- **Free Camera**: When enabled in the launcher or overlay, hold `Cross` and press `L3` (or hold `Space` and press `Z` on keyboard) to cycle camera modes. Uses Lance McDonald's GoldHEN patch; conflicts with *Enemy Control*.
- **Debug Menu**: If debug fonts (`DbgFont14h.ccm` and `DbgFont14h.tpf`, from [Debug Menu and XML Patch](https://www.nexusmods.com/bloodborne/mods/253)) are installed into `dvdroot_ps4/font/` (or `adhoc/font/`), open the menu using `Tab` or the left half of the controller touchpad. Startup rejects missing or empty font files instead of launching the unsafe patch.

## Environment variables

`BB_FRAME_STATS=1` (frame statistics), `BB_TRACE=1` (heartbeat and hang dumps in `out\`, cheap), `BB_TRACE_IMPORTS=1` (also logs every import call: much slower), `BB_SAVE_LOG=1` (frame and readback statistics into `logs\<time>.frames.csv` / `.readbacks.csv`; the launcher's own log is `launcher.log`), `BB_ANISO=N` (anisotropic filtering; 16 by default, 0 = the game's own), `BB_UPSCALER=taa|fsr3|fsr4|fsr411|dlss|off`, `BB_FRAMES_AHEAD=N`, `BB_PRESENT_THREAD=0`, `BB_LIVE_RES=1`, `BB_PREUPLOAD=0|1|2` (background upload of GPU memory into VRAM: 1 by default, 2 = all of it, ~3 GB more VRAM, 0 = off), `BB_READBACKS=0|1|2`, `BB_GPU_PROFILE=1`, `BB_BREADCRUMBS=0`, `BB_DISPLAY`, `BB_HDR_GAMMA=0` (no gamma 2.2 correction of the SDR picture when Windows runs the display in HDR), `BB_GAMEPAD`, `BB_SKIP_NETWORK_CHOICE=0`, `BB_GAME_MENU=0`, `BB_SKIP_GAME_CHECK=1`, `BB_WINDOW_MODE=windowed|borderless|fullscreen` (fullscreen is exclusive at the desktop display mode), `BB_FULLSCREEN=1` (desktop fullscreen when `BB_WINDOW_MODE` is unset), `BB_MUTE_UNFOCUSED=0`, `BB_OVERLAY_PAD=1` (overlay on `L3 + R3`), `BB_AUTO_DLC=0`, `BB_PREUPLOAD_FORCE=1`, `BB_SHADER_PRECOMPILE=0`, `BB_DRIVER_PIPELINE_CACHE=0` (the driver's pipeline binaries are not kept between sessions), `BB_ASYNC_SHADERS=0` / `BB_ASYNC_PIPELINES=0` (async compilation off; with `BB_ASYNC_SHADERS_POLICY=balanced|aggressive`, `BB_ASYNC_SHADERS_MAX_SKIP_FRAMES`, `BB_ASYNC_SHADERS_QUEUE`, `BB_SHADER_THREADS`, `BB_ASYNC_PIPELINE_THREADS` (background workers while playing: half the threads, 2 to 8, by default), `BB_ASYNC_PIPELINE_STREAK=N` (a pass may go without its new pipeline once its targets were drawn in each of the last N frames; 2 by default)), `BB_GPL=1` (`BB_GPL_STATS=1`), `BB_COMPILE_INDICATOR=0`, `BB_COMPILE_PROGRESS_TEST=1`.
More in [docs/](docs); recent upstream changes: [docs/CHANGES_after_0.4.md](docs/CHANGES_after_0.4.md), [docs/CHANGES_0.4.md](docs/CHANGES_0.4.md) (in Russian), [docs/CHANGES_2026-10-06.md](docs/CHANGES_2026-10-06.md).
