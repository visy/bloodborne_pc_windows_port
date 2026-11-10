### Changed
- **Fewer GPU thread stalls when binding samplers** (Lantern Engine by Awaken, https://github.com/Awakenchan/LanternEngine, Nightwatch `BB_SAMPLER_MEMO`, modified) samplers seen again are remembered per draw instead of hashed and looked up under a lock every time. `BB_SAMPLER_MEMO=0` restores the old lookup.
- **Auxiliary shader modules are shared** (idea from Mrsuss60 bb91ba05) the rect/quad-list tessellation helpers and discard fragment shaders are created once per distinct code instead of once per pipeline, and no longer pile up. `BB_AUX_MODULE_CACHE=0` restores one module per use.

### Fixed
- **Min/max blending with zero/one factors** (Mrsuss60 0a71dd17, shadPS4 c5dd8b8b) on UNORM and sRGB targets is rewritten to an equivalent add for colour and alpha, so the result matches the console. `BB_BLEND_MINMAX_NORM=0` restores the older alpha-only rewrite.
