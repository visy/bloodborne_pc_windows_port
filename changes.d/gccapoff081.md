### Fixed
- The Hunter's Dream (and other scenes) no longer render tinted red with a cyan sky: the texture cache collector budget cap added earlier in 0.8.1 (leak081c) is off by default until it stops evicting textures the GPU wrote (`BB_GC_BUDGET_CAP=1` or `BB_GC_BUDGET_CAP_MB=N` turns it on for testing).
