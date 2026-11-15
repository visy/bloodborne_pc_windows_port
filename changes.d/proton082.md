### Fixed

- **Starts under Proton / Wine on Linux and the Steam Deck:** the game no longer points the CPU's FS register at the guest's thread block when it runs under Wine, where Linux needs FS for its own thread data; the first fault of the game used to kill the process before any crash report. The old behaviour is back with `BB_FSGSBASE=1`.
- **Devices that cannot create a 4 GiB buffer:** the game's sparse memory arenas use 2 GiB pages when the GPU driver reports a maximum buffer size below 4 GiB (Mesa's software Vulkan driver does, which stopped the game with an out-of-device-memory error). Other GPUs keep 4 GiB pages. `BB_ARENA_PAGE_BITS=32` forces 4 GiB pages, `=31` forces 2 GiB.
