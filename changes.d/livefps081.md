### Fixed

- **Live FPS stuck low after a respawn** (GitHub #35) with the frame rate uncapped, the game could stay at 64-70 FPS instead of 90-100+ after dying and respawning until the next loading screen; minimising the window and restoring it was the only cure. The game now notices a frame rate that stays well below what it was before the load and refreshes the display connection once, the same thing minimising did, without a visible flicker. `BB_LIVE_FPS_REFRESH=0` turns it off.
