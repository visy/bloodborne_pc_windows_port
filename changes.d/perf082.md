### Changed
- **Low-latency frame pacing no longer adds to streaming hitches** (NW-69aae50). The pacing gate that holds the game before it reads the pad is skipped on the frame after a long frame (over 1.5 frame periods) and never waits longer than one frame period. `BB_LL_HITCH=0` restores the old gate (up to 40 ms every frame); `ll_pacing` / `BB_LL_PACING` still switch the whole gate.

### Fixed
- **Auto-hide HUD no longer touches the HUD while a map loads** (NW-c679fc2, NW-38b6c6f, NW-cdb2308). The fade could run during a map load and the renderer then crashed seconds later. The HUD is now left alone while the world is down, a loading screen is up or the map is unknown, and for 3 seconds after. `BB_AUTO_HUD_LOADGATE=0` restores the old behaviour.
