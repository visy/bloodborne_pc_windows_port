### Fixed

- **Starts under Proton / Wine on Linux and the Steam Deck:** the game no longer points the CPU's FS register at the guest's thread block when it runs under Wine, where Linux needs FS for its own thread data; the first fault of the game used to kill the process before any crash report. The old behaviour is back with `BB_FSGSBASE=1`.
