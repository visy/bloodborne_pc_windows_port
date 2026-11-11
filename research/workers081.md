# workers081: more resource post-processor threads (follow-up of fastload081.md)

Offsets are guest offsets (raw ELF VA of eboot; bbhost - 0x400000). Source paths come from the engine's own asserts
(Dantelion2 `Core/Resource/Source/*.cpp`). Vtables read as 0 in the static image, so the owners of virtual calls were
resolved at runtime (hooks) where needed.

## The registry keys (what the game runs with)

Read by the resource repository init `FUN_020c4240` (called from `FUN_020bf4d0`, the `DLResourceManager` init, which
the game calls through `FUN_020bea70` from `FUN_00f7f8d0` at +0xf80502). Logged at runtime by the `BB_LOAD_WORKERS`
hooks:

| key | call site | reader | engine default | the game's value | consumer |
|---|---|---|---|---|---|
| `Core.Resource.MaxFileLoadNum` | +0x20c4374 | `FUN_020ec0f0` (float) | 16 | **128** | repository +0x78 |
| `Core.Resource.WorkBufferNum` | +0x20c444c | `FUN_020ec0f0` (float) | 2 | **116** | work-buffer pool: `WorkBufferNum * 2` buffers |
| `Core.Resource.OnceReadSize` | +0x20c4532 | `FUN_020eba00` (int) | 65536 | 65536 | chunk size of every file read / work buffer |
| `Core.Resource.PostProcessorNum` | +0x20c4761 | `FUN_020ebb50` (int) | 1 | **3** | `PostProcessor` array (+0x5f8, count +0x600) |
| `Core.Resource.PostProcessorThreadAffinityMask_%d` | +0x20c49bb | int, -1 | -1 | (PP 0 falls back to the unnumbered key) | thread affinity |
| `Core.Resource.TaskManagerThreadAffinityMask` | +0x20c55f9 | int | -1 | | "Core.Res.TaskManager" thread |
| `Core.Resource.{HDD,ROM}FileLoaderThreadAffinityMask` | +0x20cf705 | int | -1 | | the two file loader threads |
| `Core.Resource.PostProcessorTempHeapSize` | +0x20d94cb | int | 4096 | | per PP thread temp heap |
| `Core.Resource.FileLoaderTempHeapSize` | +0x20cf839 | int | 4096 | | per loader thread temp heap |

The engine defaults are set by `DLRuntimeImpl` (`FUN_020e01e0`); the game's values come from the eboot's own registry
setup (no data file holds the keys: a scan of the game tree finds them only in `eboot.bin`).

## The pipeline (decompiled)

```
game -> DLResourceManager (FUN_020bf4d0, one heap block of a size from the game's config object -> resource heap +0x630)
  -> repository FUN_020c4240 (repo = manager+0x680)
       +0x650  work-buffer pool    FUN_020ce840: (OnceReadSize+0x10) * WorkBufferNum*2 bytes from the resource heap,
                                   a DLSemaphore (FUN_0263aad0, count = max = WorkBufferNum*2) + lock-free free list
                                   (+0x38, 20-bit index / 12-bit tag; "FileLoader.cpp(178) out of offset" if empty)
       +0x608  pending-stream list FUN_020cec00: intrusive list of streams no PP owns yet (nodes 0x40 bytes, each with a
                                   vector of queued chunks and the task's owner-PP slot)
       +0x5f8  PostProcessor[N]    0x4c8 bytes each (ctor FUN_020d8770, init FUN_020d8eb0, thread loop FUN_020d9990,
                                   thread "Core.Res.PostProcessor_%d", stack 0x2000)
       +0xe0   HDD loader          FUN_020cbad0 -> FUN_020cf360: "Core.Res.CacheableFileLoader" thread (loop FUN_020cc060)
       +0x3f8  ROM loader          FUN_020cf360: "Core.Res.FileLoader" thread (loop FUN_020cfa10)
       +0x10   TaskManager         FUN_0208afe0 "Core.Res.TaskManager" (TransferTaskManager loop FUN_020c7080)
```

Per load task the file loader (`FUN_020d38c0`, one chunk per task per pass over its active deque `FUN_020d2710`)
takes a work buffer (semaphore wait, then the free list), reads `OnceReadSize` bytes and hands the chunk on:

- decrypt chunks: always to **PP 0** (`FUN_020daba0`, queue +0x3f0, items 0x70) - three decrypt units at PP+0x30
  (0x68 each); a fourth stream panics `PostProcessor.cpp(442) pWork could not be allocated`, an unknown stream
  `PostProcessor.cpp(486) pWork could not be found`.
- decompress chunks: to the stream's owner PP (`FUN_020dacf0`, queue +0x420, items 0x48, mutex +0x488, event +0x450)
  when the task's owner slot (`task[0x14]`) is set; otherwise the chunk goes to a pending node in the +0x608 list
  (first chunk: new node + the task state set to 6; later chunks: appended to the node found by path) and **every**
  PP's event is signalled.
- a PP whose two queues are empty (`FUN_020d9990`, both mutexes held) resets its event, then `FUN_020cee30(list, pp)`
  takes the first pending node when the PP's "accepting" int at +0x4c0 (ctor value 2, `FUN_020daef0` sets it) is not
  0: the node's chunks are appended to the PP's queue and the PP writes itself into the stream's owner slot. Nothing
  taken: the PP waits on its event.

### PostProcessor layout (FUN_020d9990)

| offset | what |
|---|---|
| +0x08 heap, +0x10 thread, +0x18 running, +0x20 work-buffer pool, +0x28 pending list |
| +0x30 .. +0x168 | 3 decrypt units (0x68): [0] decryptor, [1] param, [6] vIn, key at unit+0x40 |
| +0x140 / +0x218 / +0x2f0 | 3 decompress units (0xd8; the inflate object at unit+0x28, key = stream path at unit+0xf8: +0x238 / +0x310 / +0x3e8) |
| +0x3f0 | decrypt queue vector (begin +0x3f8, end +0x400, cap +0x408), reserve 16 |
| +0x420 | decompress queue vector (begin +0x428, end +0x430, cap +0x438), reserve 32 |
| +0x450 | DLConditionSignal (wake event); +0x470 / +0x488 / +0x4a0 mutexes (decrypt queue, decompress queue, take) |
| +0x4b8 | OnceReadSize; +0x4c0 "accepting" |

Decompress queue item: [0] buffer (0 = close), [1] path, [2] bytes, [3] != 0 on a stream's first chunk, [6] owner
slot, [7] int* state (6 queued, 10 decompressing, 0xb done, 0x12 closed), [8] int* state2 (0x10 on error).

Inside the PP loop, per decompress item:

- close (buffer 0): unit found by path -> released (`FUN_02619880`), state 0x12; none -> panic `(958) invalid parameter on pUnit`.
- first chunk: first unit with key 0 -> allocated, `FUN_02619390` opens the stream; **none free -> panic
  `PostProcessor.cpp(580) Logical error. pWork could not be allocated.`**
- later chunk: unit found by path -> inflate (`FUN_02619910`; 1 = stream end: unit released, state 0xb, completion
  callback); **no unit with that path -> the buffer is returned to the pool and the chunk is dropped without any
  message** (the stream's state stays 6 / 10: a lost job).

So each PP serves at most 3 open decompress streams and 3 open decrypt streams, and those limits are compiled in
(loop bounds `> 2`, fixed unit offsets), not registry values.

## First test: workers 8, WorkBufferNum raised

Session `w8_first` (8 PPs, the game's pool): Continue + 3 lamp warps clean. Session `w8_wb256` (8 PPs,
WorkBufferNum 256): panic at the first load before the title:

```
[GAME PANIC] ...\Core\Resource\Source\DLResourceManagerImpl.cpp(491): "DLResourceManager..." from +0x20c1279,
thread 'main'
rbp chain: +0x20c1279 (FUN_020c1250) <- +0xf8050c (FUN_00f7f8d0, right after the FUN_020bea70 call at +0xf80502)
```

`FUN_020c1250` checks the manager's "initialised" bit and panics when `FUN_020bf4d0` failed. It fails because
`FUN_020c4240` returned 0: the work-buffer pool `FUN_020ce840` asks the resource heap (`+0x630`, built from one block
the game sizes) for `(0x10000+0x10) * 2 * WorkBufferNum` bytes = 14.9 MB for the game's 116 and 33.6 MB for 256, and
the block has no room for the bigger pool (`FUN_020ce840` returns 0 when the heap returns 0, the repository init
unwinds). The value that has to grow with the pool is the resource manager's heap block (the two sizes passed to
`FUN_020bea70` from the game's config object: `BB_LOAD_HEAP_SCALE` hooks them).

## Runtime trace (BB_LOAD_WORKERS_TRACE=1)

Hooks (all logging only): prologues of `FUN_020cee30` (take), `FUN_020dacf0` / `FUN_020daba0` (push), `FUN_020d2c50`
(cancel), a value filter at +0x20da6e3 (the PP loop's silent-drop branch), replace-prologues on `FUN_020daef0` and
`FUN_020bec80` (the "accepting" setters), a 10 s summary and a live line (pending streams, every PP's open units,
queue lengths and accepting flag).

The resource manager's heap block (`FUN_020bea70` args): parent heap, sizes 0 / 16252928 (15.5 MB), flags 256/256/10.
The game's work-buffer pool alone is 232 * 0x10010 = 14.5 MB of it.

| session | PPs | loads | panics / stalls | max open units per PP | real drops | take with 3 busy |
|---|---|---|---|---|---|---|
| w3_trace | 3 | 15 | 0 | 1 / 1 / 1 | 0 | 0 |
| w6_trace1 | 6 | 15 | 0 | (older build) | - | 0 |
| w8_trace | 8 | 15 | 0 | 1 for all 8 | 0 | 0 |

So in practice a PP never holds more than one open decompress stream: the loaders feed big files one stream at a
time per loader thread (the parallel deque holds small files, which open and close a unit within one chunk), and the
pending list is drained by whichever PP wakes first (the loader signals PP 0 first, which is why PP 0 gets about half
of all chunks with any count). The compiled-in 3 units per PP are not what limits the count.

The game toggles a PP's accepting flag (+0x4c0): at rest exactly one of PP 1 / PP 2 is 0 (it rotates between loads),
PP 0 and PPs 3+ stay 2. Setter chain: `FUN_020bec80(index, v)` (static game API, bounds-checked against the PP count)
-> `FUN_020c1c20(manager, index, v)` -> `FUN_020ca150(repository, index, v)`; `FUN_020daef0(pp, v)` is the PP's own
slot. A PP with accepting 0 still processes chunks pushed to it; it only stops taking pending streams.

The game's own inflate (`BB_FAST_INFLATE=0`, PPs busier) with 6 PPs: sessions w6_slow1 / w6_slow2, 16 loads each,
clean, max units 1. Four more traced 6-PP sessions (w6_t1..3, w6_trace1) and two 8-PP ones: clean. The fastload081
failures (hangs in 2 of 5 six-PP sessions, one `pWork` panic) did not reproduce in 128 loads with the trace on.

## Values that depend on the worker count (and what does not)

| value | where | scales with PostProcessorNum? |
|---|---|---|
| PostProcessor array | repository +0x5f8 (N * 0x4c8 + 0x10 from the resource heap), count +0x600 | yes, allocated from the registry value |
| threads "Core.Res.PostProcessor_%d" | `FUN_020d8eb0`, stack 0x2000, temp heap `PostProcessorTempHeapSize` | yes |
| affinity masks `..._%d` | registry, the game sets 0..2 only; others read -1 (any core) | harmless |
| wake-all loop after a pending push | `FUN_020d38c0` end: `for i < repo+0x600` | yes |
| accepting setters | `FUN_020bec80` / `FUN_020c1c20` / `FUN_020ca150`: bounds-checked against +0x600 | yes |
| destructor | `FUN_020c5720`: loops +0x600 | yes |
| decompress / decrypt units | 3 + 3 per PP, compiled in (`FUN_020d9990` loop bounds, fixed unit offsets) | no: per PP |
| work-buffer pool | `WorkBufferNum * 2` buffers of `OnceReadSize`; the loaders block on its semaphore | no: shared by the loaders, not by PP count |
| pending-stream list | one, unbounded, nodes from the resource heap | no |
| resource manager heap | one 15.5 MB block; 14.5 MB is the pool, PP objects 0x4c8 each, loader queues | PPs take 1.2 KB each: no |
| decrypt target | always PP 0 (`FUN_020d38c0`: `loader+0x10`) | no (3 decrypt units on PP 0 whatever N) |

Nothing is sized by a hard-coded 3 except the game's affinity keys; the engine is generic in N. The one structure that
breaks with N > 3 is dynamic: the self-service take (`FUN_020cee30`) does not look at the PP's units. With 3 PPs the
loaders out-run the PPs (a PP owning a big stream gets its next chunk before it goes idle, so it never takes a second
one: max units 1); with more PPs each one is idle more often, takes further pending streams while it still owns
some, and the fourth first chunk on one PP panics (fastload081's R6). A hang needs a chunk to reach a PP that has no
unit for it (the silent drop at +0x20da6e3), which only follows from the same over-commit.

## The fix (BB_LOAD_WORKERS, gpu/shim/bbport_load_speed.cpp)

`FUN_020cee30` is replaced (prologue check `55 48 89 e5 41 57`) by `GuardedTake(list, pp)`:

1. nothing pending, or `pp` has a free decompress unit (open units + queued chunks < 3): the engine's own take.
2. `pp` full: the engine's take is run with another accepting PP that has a free unit (round robin from pp+1), which
   fills that PP's queue and the stream's owner slot exactly as a self-take would; that PP's event (+0x450) is then
   set through `FUN_0207e780` as a push does.
3. every PP full: refuse (return 0); the stream stays pending until a unit frees up (a PP's loop re-runs the take
   when it goes idle again). The engine would panic here.

All or nothing: the worker count hook (`+0x20c4761`) is installed only after the guard is in and the four registry
call sites and `FUN_0207e780`'s prologue match. `BB_LOAD_WORKERS` unset / 0 changes nothing. `BB_LOAD_WORKBUF` and
`BB_LOAD_HEAP_SCALE` remain experiments (both off): the pool is not what limits the count, and a bigger pool needs
the game's resource-manager heap block grown with it (`FUN_020bea70` second size argument, 16252928).

## Proof and A/B (the guarded take in, trace off, zlib-ng on, 60 FPS, 720p)

Three rounds of 3 / 8 / 6 / 4 workers interleaved (`tools/mp/loadab.py run --cycles 2`: Continue, 12 lamp warps,
2 death respawns per session; `BB_LOAD_TRACE=1` stall guard on). 12 sessions, 192 loads: **zero hangs, zero panics,
zero stalls**; the hand-over branch of the guard never ran (no PP was ever full). Earlier traced sessions (6 x six
workers incl. 2 with the game's inflate, 2 x eight, 2 x three): also clean.

Loading-screen seconds (mean of the session's 12 lamp warps; round 3 ran under load from something else on the
machine: 3.87 / 3.93 / 3.80 / 4.37, every count slower, so rounds 1-2 carry the comparison):

| workers | round 1 | round 2 | round 3 | Great Bridge (cold / warm) | Yahar'gul | Tomb of Oedon | Cathedral Ward | Old Yharnam | Continue | death |
|---|---|---|---|---|---|---|---|---|---|---|
| 3 (the game's) | 3.82 | 3.82 | 3.87 | 3.69 / 2.69 | 3.37 | 3.29 | 6.59 | 4.19 | 2.13 | 2.48 |
| 4 | 3.66 | 3.53 | 4.37 | 3.27 / 2.35 | 3.01 | 2.91 | 6.49 | 4.13 | 2.07 | 2.42 |
| 6 | 3.68 | 3.71 | 3.80 | 3.30 / 2.55 | 3.07 | 2.93 | 6.68 | 4.23 | 2.10 | 2.48 |
| 8 | 3.76 | 3.74 | 3.93 | 3.27 / 2.49 | 3.18 | 3.07 | 6.71 | 4.30 | 2.15 | 2.50 |

(per-lamp columns: mean of rounds 1-2.) The CPU-bound warps gain 0.3-0.5 s (Great Bridge, Yahar'gul, Tomb of Oedon),
the two long ones gain nothing (Cathedral Ward 6.5-6.7 s and Old Yharnam 4.1-4.3 s are not limited by the
post-processors: fastload081 saw the same), Continue and death respawns nothing. Mean lamp warp: 3.82 -> 3.60 (4),
3.70 (6), 3.75 (8): 0.1-0.2 s, with 4 the best of the three. 8 workers are slower than 4 (more threads wake on every
pending push and PP 0 still takes half the chunks).

## Recommendation

Safe: proven for 4, 6 and 8 in 3 sessions each (plus 10 earlier sessions), nothing lost, nothing panicked, the stall
guard never fired. Faster: by 0.1-0.2 s per lamp warp on the mean (up to 0.5 s on CPU-bound warps), not more, with
zlib-ng already in. That is under the 0.2 s bar for a default change, so `BB_LOAD_WORKERS` stays **opt-in (off)**;
`BB_LOAD_WORKERS=4` is the setting to recommend to players who want it (least threads for the gain; the count is
clamped to 1..8). If it is ever made default the safe count is `min(4, cores - 2)` and at least 3: on a 4-core PC it
stays at the game's 3. What is still missing for a default: a run on a low-core machine (6 logical CPUs:
`BB_AFFINITY_MASK=0x3F`) to show 4 does not cost there, and a cold-disk session (the fastload081 failures may have come
from a slow-loader regime that the 128 cached loads here never entered; the guard covers it by construction, not by
measurement).
