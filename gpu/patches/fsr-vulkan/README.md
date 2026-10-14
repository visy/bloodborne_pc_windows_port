# Local changes to gpu/third_party/fsr-vulkan

The submodule points at upstream FireBurn/FSR-Vulkan (`c64f093`). `build.sh` applies the
patches here to its working tree when they are not applied yet:

- `0001-...`: `BB_FSR4_PROFILE` (GPU time per FSR 4 pass) and `BB_FSR4_STATS` (driver
  statistics of each pass) in the FSR 4 v07 provider.
- `0003-...`: orders repeated transfer clears of FSR history images, including clears that do
  not change the image layout.
- `0005-...`: associates FSR frame-generation transient views, descriptor sets and constant
  buffers with their render serial and retires them after GPU completion instead of retaining
  anonymous per-frame allocations.

0003 and 0005 are from Supermedo/bloodborne_pc PR #69 by FIocker (FSR frame generation; its 0004, a debug view flag nothing here uses, is not taken).
`tools/agent/build.py` applies them too (agent worktrees hold a plain copy of the submodule).
