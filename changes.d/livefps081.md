### Fixed

- **Live FPS stuck low after a respawn** (GitHub #35): with the frame rate uncapped, the game could stay at 64-70 FPS instead of 90-100+ after a respawn until the next loading screen, and minimising and restoring the window cured it. The game now notices a frame rate that stays well below the rate before the load for 6 seconds and refreshes its display connection once by itself, the same thing minimising did. This is a safety net; the cause is not found yet. `BB_LIVE_FPS_REFRESH=0` turns it off.
