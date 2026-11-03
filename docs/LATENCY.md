# Input latency (0.7.8, CL-7ae614)

Back to the [README](../README.md). Reflex's own page: [FG_REFLEX.md](FG_REFLEX.md).

Five things shorten the way from a button press to the picture. Each has a switch (launcher, `bbport.ini`,
and the game's *System > Latency* page or, for Reflex, *System > Frame generation*).

| What | Default | `bbport.ini` / env | When it applies |
| --- | --- | --- | --- |
| Low-latency frame pacing | on | `ll_pacing` / `BB_LL_PACING` | live |
| Fresh gamepad read | on | `pad_fresh` / `BB_PAD_FRESH` | live |
| NVIDIA Reflex | **Auto**: on for an NVIDIA GPU whose driver has `VK_NV_low_latency2` revision 3 and `VK_KHR_present_id`, off elsewhere | `reflex=auto/off/on/boost` / `BB_REFLEX` | next start |
| AMD Anti-Lag 2 | **Auto**: on for an AMD GPU whose driver has `VK_AMD_anti_lag`, off elsewhere | `anti_lag=auto/off/on` / `BB_ANTI_LAG` | next start |
| Variable refresh rate (G-Sync / FreeSync) | off (a game cannot tell whether the display does VRR) | `vrr` / `BB_VRR` | next start |

## Low-latency frame pacing

The game submits a flip and goes on with the next frame while the presenter still holds the earlier ones (the frame
limit holds each flip for its slot; the presenter thread then waits for the swap chain). The game runs a little
faster than the limit, so the backlog creeps up until the flip queue is full: at 60 FPS that is about 39 ms between the
game reading the pad and the CPU's present call, up from 13 ms at the start of a session.

With the switch on, the first pad read after each submitted flip waits until the frames before it have gone out
(flips waiting for their slot plus frames on the presenter thread; at most 40 ms, never during a fast loading screen).
The game then simulates its next frame from input that is that much fresher. Frame rate and game speed do not change:
the game's step is still the measured frame time.

Nothing ahead of the frame (depth 0) needs the game's own work to fit inside the limit's period with a margin; when it does
not, a frame that misses its slot waits for the next and the rate falls (a scene whose frames take 15.4 ms under a 59 FPS limit,
17 ms, lost a fifth of its frames). So the depth adapts: 45 frames with more than 12% of them later than 1.3 periods raise it
(to 1, then 2) and hold it for a minute, doubling the hold each time it has to be raised again (8 minutes at most), then 0 is
tried again; the log says `Low-latency pacing: N of M frames late at X FPS, K frame(s) may stay ahead`. The pacing is off while
there is no frame limit (uncapped) or the limit is 240+. `BB_LL_DEPTH=n` pins the depth, `BB_LL_TIMEOUT_MS` changes the 40 ms bound.

## Fresh gamepad read

SDL holds the pad state as of the window thread's last event pump, which runs every ~2 ms. At the game's read the
gamepads are updated again on the game's thread, so the state is the device's at that moment (rate limited to one
update per 0.25 ms). Measured: the state the game would have read was 1.2 ms old on average and up to 3 ms. The keyboard
state still comes from the window thread's pump (SDL needs the window's thread for key events).

## NVIDIA Reflex

`VK_NV_low_latency2` was already there (0.6.1, Supermedo/bloodborne_pc PR #69), complete in its markers: sleep mode set
on the swap chain, latency sleep before the game samples input, input / simulation / render-submit / present markers
with present IDs, the present queue marked out of band, `vkGetLatencyTimingsNV` reports in the log
(`BB_REFLEX_TIMING=1`). It was off by default, and its sleep had no minimum frame interval, so the driver did not know
the rate the presenter holds. 0.7.8 completes it: the sleep mode's `minimumIntervalUs` follows the frame limit (Live
FPS target, or the VRR cap), as NVIDIA recommends for a limiter that lives in the application, and the default is **Auto**
(on for NVIDIA with the extension at revision 3). A driver that reports revision 2 (581.57 on an RTX 3070 did) keeps it off,
and the log says so (`Reflex: not supported by this GPU or driver`). *Boost* keeps the clocks up.

## AMD Anti-Lag 2

`VK_AMD_anti_lag` (`gpu/shadps4/video_core/renderer_vulkan/vk_antilag.cpp`): one `vkAntiLagUpdateAMD` with the INPUT
stage per simulation frame, before the first pad read of the frame, and one with the PRESENT stage and the same frame index
before each real present. The extension and its feature are enabled only when the driver offers them and the setting
resolves to on (Auto: AMD GPU). **It compiles and links here but was not run: this PC has an NVIDIA GPU.** The log line
`Low latency: reflex=... anti-lag-2=...` shows what a start resolved to.

## Variable refresh rate

Turn it on for a G-Sync / FreeSync display (a program cannot see whether VRR is active, so there is no auto). At the next
start the frame rate is held at `refresh - refresh^2/3600` (60 -> 59, 120 -> 116, 144 -> 138, 240 -> 224), the figure NVIDIA and
Blur Busters give for staying inside the VRR window: the display refreshes when a frame arrives, and never at its top, where
a present would have to wait for the display or tear. The display's refresh rate is read from SDL twice a second
(`BB_DISPLAY_REFRESH=hz` pretends another, for tests). The present mode stays Mailbox (a finished frame goes out at once, nothing
queues; that is also what a VRR display wants). Immediate was tried: in a window it made the present wait for the compositor and the
run lost 10% of its frame rate (47 instead of 59 FPS in the same scene), so it is not the default;
`BB_VRR_PRESENT_MODE=Immediate|FifoRelaxed|Fifo|Mailbox` lets the owner of a VRR display in fullscreen try one.

How it meets Live FPS: Live FPS's clock has the pacer target (30..239, uncapped) and the presenter holds a limit
(`BbFrameRate::LivePresenterLimit`). With VRR on both are lowered to the cap when they would exceed it (an uncapped target
becomes the cap), and Reflex's sleep interval follows the same figure. Pick a frame rate setting that matches: with a start preset of 120+ (vblank 480 Hz) a limit of 59-60 makes the game's
own pacing miss every fourth slot in the scene measured here (49 instead of 60 FPS, with or without VRR, and without any of these
changes); a limit of 64 held 62. Below the cap nothing changes; the game still steps by
its measured frame time, so the speed of the game is the same as without VRR. Frame generation is separate (its generated
frames are paced by `fg_timing.h` as before).

## Measuring

`BB_LATENCY_LOG=1` prints once a second `latency: n=... pad_age_us ... read_to_present_ms ... read_to_flip_ms ...
flip_to_present_ms ...`; `python -I tools/windows/analyze_latency.py <log> --skip 35` summarises. `pad_age` is how old the
pad state was when the game read it (before the fresh update), `read_to_present` runs from the game's pad read to the CPU
call that queues the present (excludes the swap chain's own queue, the display and the physical device; with `BB_REFLEX_TIMING=1`
the driver's own report is printed beside it).

### Results (RTX 3070, driver 617.42, 720p window, 120 Hz display, Hunter's Dream test save, 100 s per run, seconds 35-100)

Read -> present is the probe's CPU number in ms. The "before" run (everything off) drifts: the game runs a little faster than the
limit, so the queue in front of the display grows by itself, from 13 ms in the first seconds to 39 ms after 85 s, and stays there.

| Run (`BB_LL_DEPTH` pinned to 0 where it says pacing) | read -> present, ms | flips/s |
| --- | ---: | ---: |
| 60 FPS, all off (before), whole window / last 10 s | 18.8 / 38.9 | 60.4 |
| 60 FPS, pacing with 1 frame allowed ahead | 33.3 | 60.5 |
| 60 FPS, pacing, nothing ahead | 19.1 (16-23, flat) | 60.0 |
| 60 FPS, pacing + Reflex (the NVIDIA default) | 15.2 (flat) | 60.2 |
| 60 FPS, Reflex alone | 33.7 | 60.4 |
| `--fps 120` (this scene runs at 65 FPS), all off | 41.7 | 64.6 |
| `--fps 120`, pacing, nothing ahead | 21.9 | 64.7 |
| `--fps 120`, pacing + Reflex | 21.4 | 65.0 |
| 60 FPS, shipped defaults (pacing adapts), whole window / first 10 s / last 10 s | 19.2 / 14.4 / 29.4 | 60.2 |
| `--fps 120`, shipped defaults | 21.4 (flat) | 65.0 |

Reflex alone left the queue (33.7 ms): the backlog sits in front of the swap chain, where the driver cannot see it. The pacing
removes it, and Reflex then takes about 4 ms more at 60 FPS and nothing where the GPU is the limit. The pad-age column of every
run was 1.2 ms on average (95th percentile 2.5, maximum 3.8): what the fresh gamepad read saves.

The shipped defaults adapt the depth (see above). On this shared PC, with other test games running, the 60 FPS scene's frames
took 21 ms against the 16.7 ms limit for a stretch near the end of a run; the pacing then let a frame stay ahead (the last 10 s),
which brings the lag back towards the "before" figure and not above it, with the frame rate at the limit. Where the margin is
there (the 120 target here, a GPU-bound 65 FPS) nothing needed to stay ahead.
