# zstd081: game files recompressed with zstd once, decoded by the inflate hook

Branch `agent/zstd081`. Follow-up of `fastload081.md` / `reload081.md`. No game data here, only layouts and offsets.

## 1. Phase 1: is it worth it (BB_INFLATE_BENCH)

`BB_INFLATE_BENCH=1` (off by default, `gpu/shim/bbport_inflate_bench.h`): every hooked inflate call is timed; every
`BB_INFLATE_BENCH_EVERY`-th stream (default 4) is captured in memory (input and output), recompressed with zstd
(`BB_INFLATE_BENCH_LEVEL`, default 19) on a background thread, and zstd decode is timed against zlib-ng decode of the
same stream (best of 3, one-shot, same thread). Nothing is written to disk.

One `loadab` session (Continue, six lamps, a death respawn), 16 threads, then the same with `BB_AFFINITY_MASK=0x3F`:

- A session inflates 4.3 GB in to 8.1 GB out (about 18800 streams, 287k calls) through the hook; zlib-ng spends 25 s of CPU
  in it on 16 threads (44 s inside the calls on 6 CPUs: the threads are preempted).
- Sampled streams (455, 550 MB out): zstd -19 is 81 % of the zlib size; zstd decode takes 0.65 of zlib-ng's time
  (0.746 s against 1.15 s; 740 MB/s against 480 MB/s). A first, larger sample (2686 streams, 1.35 GB out) gave 0.71.
- Bytes of zlib-ng time per lamp warp (the inflate CPU of a big load): 2 to 3.4 s. A 30-40 % cut of that, on the two
  threads the world-ready wait runs on, projected to 0.4-0.6 s per lamp warp on 16 threads and 0.5-0.8 s on 6 CPUs
  (the zlib-ng switch gained 0.45 s / 1.0 s from a similar cut): above the 0.3 s / 0.5 s bar, so go.

A later offline measurement on whole files (`bb-zstdpack bench`, one-shot decode, 80 of the biggest DCX files): zstd
decodes in 39-45 % of zlib-ng's time at every level (880-925 MB/s against 340-380 MB/s). The level changes the size,
not the speed; zstd even decodes the nearly incompressible texture files in 38-41 % of the zlib-ng time.

## 2. Format

The game folder is loose files, not BHD5/BDT archives: `dvdroot_ps4/**/*.dcx` (26913 files, 16.5 GB) plus the `tpfbhd` /
`tpfbdt` pairs, which are not touched. Every DCX of the dump is a 0x4c byte big-endian header and one zlib stream
(windowBits 15, `78 DA`) up to the end of the file, no padding:

| offset | content |
|---|---|
| +0x00 | `DCX\0`, +0x04 `00 01 00 00`, +0x08 `00 00 00 18`, +0x0c `00 00 00 24` |
| +0x10, +0x14 | `00 00 00 44`, `00 00 00 4c` (59 files: `24`, `2c`) |
| +0x18 | `DCS\0`, +0x1c uncompressed size, +0x20 compressed size (the zlib stream's length) |
| +0x24 | `DCP\0`, `DFLT`, `00 00 00 20`, `09 00 00 00`, 12 zero bytes, `00 01 01 00` |
| +0x44 | `DCA\0`, `00 00 00 08` (the header ends at +0x4c) |
| +0x4c | the zlib stream |

The converter keeps all of it byte for byte except +0x20 and puts a zstd frame (content size known, no checksum) in
place of the zlib stream. The engine's stream readers (`FUN_02065cf0`, `FUN_02066b70`: a file stream with a segment table;
they call `inflateInit2_` / `inflate(z, 3)` / `inflateReset` / `inflateEnd` in chunks into the caller's buffer) work on the
file as before; only the bytes the hooked inflate sees are zstd.

## 3. Game side

- `bbport_zstd_inflate.h`: a stream whose first input starts with `28 B5 2F FD` (a zlib stream never does; a first chunk
  shorter than 4 bytes counts when it is a prefix of the magic) is decoded with `ZSTD_decompressStream` behind the engine's
  z_stream (avail_in/out, total_in/out, Z_OK / Z_STREAM_END / Z_BUF_ERROR / Z_DATA_ERROR, `msg`). `z_stream.reserved` bit 1
  marks a zstd stream (the pointer is the state, which keeps the zlib-ng stream). A reset stream decides again on its next
  first input. Everything else goes to zlib-ng or the game's inflate as before. Unit test `zstd-inflate-test`.
- `bbport_zstd_files.cpp`: `BB_ZSTD_DIR` (set by the launcher) names the converted folder. `runtime_file.c` `translate()` calls
  a hook for paths under /app0: a `*.dcx` listed in `bbzstd.manifest` goes to the converted copy while the original's size and
  modification time still match the manifest (a mod that replaces the file wins; the dream mirror's `m21_00_00_00.msb.dcx`
  is one). Only installed when the inflate hooks are. Host-side readers (party tables, gameparam hash, `party_mods`, the
  launcher's scripts) read the game folder itself, so every identity check gives the same result on a converted install.

## 4. Converter

`tools/zstdpack/zstdpack.cpp` -> `out/bb-zstdpack.exe` (static zstd + zlib-ng): `convert` (free space check first, multithreaded
over the files biggest first, progress line, resumable through `bbzstd.manifest`, each frame decoded and compared before it is
written), `verify` (re-reads both folders from disk and compares header and content for every entry), `bench`. Level 9
by default: 15 and 19 shrink the files by 0.5 % and 4 % more and take 3x and 7x longer; decode speed does not depend on it.
Files whose zstd frame would be larger than 1.3x the original stay in the game folder.

## 5. Results

See the end of this file (filled in after the A/B).
