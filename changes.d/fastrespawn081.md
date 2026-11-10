### Changed
- **Faster respawn after death** the loading screen after a death no longer holds for a fixed 12 s: the death respawn takes about 3.4 s instead of about 12.4 s (3 deaths each, solo, Cathedral Ward lamp), most of it the real map load. `BB_FAST_RESPAWN=0` restores the 12 s hold, `BB_FAST_RESPAWN_SECONDS=S` sets it (default 2.0, 0.5 to 12).
