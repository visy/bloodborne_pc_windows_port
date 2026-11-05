# What the port adds (full list)

Back to the [README](../README.md).

| Feature | What you get |
|---|---|
| **Native on Windows** | No emulator, no Wine. The game's own code runs on your CPU; the graphics go through Vulkan. |
| **Party co-op, no server** | Play the whole campaign with friends over a direct connection: no PSN, no server. Modelled on Elden Ring Seamless Co-op. [More](COOP.md) |
| **Settings in the game's menu** | Every port setting is a page in the game's own *System* menu, with gamepad navigation. |
| **Keyboard, mouse and any gamepad** | Classic keyboard layout or mouse & keyboard mode, every key rebindable; button prompts show your keys, Xbox or Nintendo buttons. [Controls](CONTROLS.md) |
| **High frame rates** | 30 / 60 / 90 / 120 / uncapped with correct game speed. **Live FPS** (on by default, Mrsuss60's work): the game's clock follows the real frame time, so physics, cloth and the camera stay steady at any rate, and the target can be changed in *System > Live FPS*. Off = deadinside's frame-rate hooks or the community FPS++ patches; plus bbhost's high-FPS fixes for effects, grass wind and the Bloodletter's L2. [More](LIVE_FPS.md) |
| **Texture quality** | *Auto / full / half / quarter*: large textures can skip their top mip level to save video memory; *auto* only steps down on cards with 3 GB or less. |
| **Upscaling** | FSR 3.1, FSR 4, FSR 4.1.1 and DLSS, with motion vectors added for this game. |
| **Frame generation** | Optional, off by default: DLSS FG (2x-4x, RTX 40/50) or FSR FG (2x), paced, HUD kept sharp; NVIDIA Reflex. [More](FG_REFLEX.md) |
| **Input lag** | Frames no longer queue before the display, the gamepad is read at the moment the game asks (both on), NVIDIA Reflex / AMD Anti-Lag 2 automatic on those GPUs, G-Sync / FreeSync switch. [More](LATENCY.md) |
| **Finer motion blur** | Optional, off by default: the game builds its motion blur from half-resolution buffers; render them at 2x the pixels or at full resolution (*System > Display > Motion blur resolution*). [More](SETTINGS.md) |
| **HDR output** | Optional, off by default: HDR10 on an HDR display, highlights expanded to your display's peak while mid-tones and menus stay at paper white; calibration in *System > HDR*. [More](SETTINGS.md#hdr-output) |
| **In-game map** | Optional, off by default: **M** opens a map of the area you are in with its floors, the lit lamps, you and your party; click a lamp to travel to it. Made from your own game files once (launcher: *Prepare the map*). Hunter's Atlas by MikeTat1988. [More](SETTINGS.md#in-game-map) |
| **No shader stutter** | Known shaders are built before the intro. New ones compile in the background. |
| **Shareable shader cache** | Export / import one `.zip`, works across GPUs; drop a shared one into `user\cache_import\`. A built-in *Shader cache builder* travels the whole game by itself on a copy of your save to fill it. [More](SETTINGS.md#shader-cache-builder-world-warper) |
| **Save safety** | Crash-safe saving, save checks at start, automatic backups in party play. |
| **The Old Hunters** | DLC enabled automatically (its data ships with patch 1.09). |
| **Mods and patches** | Loose-file mods and shadPS4-format XML patches, with load order. [More](MODS.md) |
