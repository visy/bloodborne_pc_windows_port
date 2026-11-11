### Changed
- **Faster loading screens on big maps** (cwfast081) The game admits at most 20 map pieces per frame while a
  loading screen is up, so a warp into a place with thousands of pieces in view (Cathedral Ward, Old Yharnam)
  waited hundreds of frames for a walk over the list while the disk and the unpack threads sat idle. While the
  loading screen is up the game now admits many more pieces per frame; in the world the game's own pacing is
  kept, so streaming while walking is unchanged. `BB_LOAD_PARTS_BUDGET=0` restores the old loading (`=N` sets the
  per-frame count).
