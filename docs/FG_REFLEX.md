# Frame generation and NVIDIA Reflex

Back to the [README](../README.md).

From [Supermedo/bloodborne_pc](https://github.com/Supermedo/bloodborne_pc) PR #69 by **FIocker**,
adapted to bbport (0.6.1). The frame pacing (`fg_timing.h`) follows bbhost's frame generation
cadence (GPL-3.0-or-later). Everything here is **off by default** and costs nothing when off.

## What it does

- **DLSS frame generation** (2x, 3x, 4x where the runtime allows): one to three generated frames
  between two rendered ones, through NVIDIA's DLSS-G (`nvngx_dlssg.dll`, shipped with the release
  under the DLSS SDK license, `out\NVIDIA_DLSS_LICENSE.txt`). Needs an RTX 40 or 50 series GPU.
- **FSR frame generation** (2x): AMD FSR 3.1.6 frame interpolation through FSR-Vulkan. Needs
  `VK_KHR_compute_shader_derivatives` with linear groups (current NVIDIA, AMD and Intel drivers).
  FSR presents on a second queue of the graphics family when there is one, after its copy has
  completed (`BB_FSR_PRESENT_QUEUE=0`: the shared queue; `BB_FSR_GPU_PACING=0`: CPU-timed only).
- **Paced presentation**: generated frames go out at their slots between the previous and the
  current rendered frame, on a clock taken from GPU timestamps; a slot that has already passed is
  skipped, never presented in a burst. A present is never held more than 100 ms.
- **The HUD** of a generated frame is the rendered frame's own (`fg_hud.comp`): generation works
  on the scene without the HUD, the HUD is laid over the result, so text and bars do not ghost.
- **NVIDIA Reflex** (Off / On / On + Boost): `VK_NV_low_latency2` revision 3 with
  `VK_KHR_present_id`: the driver's latency sleep before the game samples input, simulation /
  render / present markers. The PR's own measurements showed no meaningful latency reduction; it
  is offered as an option. Drivers with an older revision of the extension (e.g. 581.57 on an
  RTX 3070 reports revision 2) keep it off and say so.
- **Object motion** while frame generation runs: instanced batches get camera motion only (no
  per-instance history matched by draw order, which culling can reorder).

Frame generation and Reflex apply at the next start. Without support the game presents normally
and the log and the Insert menu (*Upscaler* tab) say why.

## Settings

| Where | Frame generation | Reflex |
| --- | --- | --- |
| Launcher | *Graphics > Frame generation*: Off, DLSS FG 2x/3x/4x, FSR FG 2x | *Graphics > NVIDIA Reflex*: Off, On, On + Boost |
| Game's System menu | *Frame generation* page | same page |
| Insert menu | *Upscaler* tab | *Upscaler* tab |
| `bbport.ini` | `frame_generation=off/2x/3x/4x`, `fg_backend=dlss/fsr` | `reflex=off/on/boost` |
| Environment | `BB_FRAME_GENERATION`, `BB_FG_BACKEND` | `BB_REFLEX` |

FSR is 2x only (a higher factor is lowered to 2x). HDR output is presented without generation.
The FPS counter and the performance HUD show presented and rendered frames per second.

## Diagnostics

- `BB_FG_TIMING=1`: once a second, rendered and presented rates, generated and skipped frames.
- `BB_FG_TRACE=1`: per rendered frame, its GPU timestamps and slot deadlines.
- `BB_FG_CAPTURE_DIR=<folder>` (`BB_FG_CAPTURE_FRAME`, default 120): one generated frame's guides
  and outputs as raw files; `python tools/windows/analyze_fg_guides.py <folder>/frame_<n>`.
- `BB_REFLEX_TIMING=1`: the driver's latency reports and a CPU pad-sample-to-present interval
  (`python tools/windows/analyze_reflex.py <log>`); neither is input-to-photon latency.
- `tools/windows/analyze_presentmon.py` (PresentMon CSVs), `analyze_stability.py` (memory samples).

## Checked on bbport (RTX 3070, driver 581.57, 60 Hz display, 1280x720 test windows)

One in-world session per mode (the test save at a Yharnam lamp, camera turns and short walks scripted), from the
game's own logs (`BB_FRAME_STATS`, `BB_FG_TIMING`):

| Mode | Rendered FPS | Presented FPS | Game frame pacing (median / p99) |
| --- | ---: | ---: | --- |
| Off, 60 cap | 60.0 | 60 | 16.69 / 17.82 ms |
| FSR FG 2x, 60 cap | 60.0 | 120 (min 118) | 16.69 / 17.82 ms |
| FSR FG 2x, uncapped | 97.8 | 195 (min 174) | 10.26 / 15.75 ms |
| DLSS FG 2x | 60 | 60 (unavailable on RTX 30: clean fallback) | - |
| Reflex On, 60 cap | 60.0 | 60 (extension revision 2: off) | 16.69 / 17.80 ms |

Screenshots of every session were checked for HUD ghosting, doubled UI and tearing: none seen.
DLSS frame generation and Reflex run only on hardware this PC lacks; they are covered by the
unit tests (`fg-settings-test`, `fg-timing-test`, `reflex-cpu-test`) and the fallback paths.
