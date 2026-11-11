### Added

- **Faster loading screens with converted game files (optional)** the launcher gets a *Faster loading* option with a button, *Convert
  game files for faster loading (one-time, about 14 GB extra)*. It recompresses the game's zlib-packed map, model and texture files
  once with zstd into a separate folder next to the game folder (15 GB, 5 to 15 minutes depending on the CPU, resumable, every file
  checked against the original; your game folder is never touched), and the game unpacks them about twice as fast. A lamp trip takes
  3.64 s instead of 3.80 s on a 16-thread PC (three sessions of 12 trips each way; the Great Bridge, Yahar'gul and Tomb of Oedon lamps gain
  0.3 to 0.4 s) and 3.86 s instead of 4.09 s on a PC limited to 6 threads; Continue and death respawns are unchanged. Off by default;
  delete the folder or turn the option off to go back. A mod that replaces a game file still wins, and the co-op version check ignores
  the option. `BB_ZSTD_DIR` is the folder, `BB_INFLATE_BENCH=1` a diagnostic that times unpacking.
