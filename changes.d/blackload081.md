### Fixed

- **Black world after a lamp warp** after several area changes, a lamp warp to the Hunter's Dream could leave the screen black (the HUD and the area title showing) until the next warp. The texture collector, with the 1.5 GiB cap, dropped GPU-drawn images that the game never draws again; they now stay (the memory use is the same). `BB_GC_DROP_TILED=1` restores the old drop.
