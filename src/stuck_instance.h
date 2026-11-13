/* An earlier bbport.exe stuck for good: a present that never came back out of the graphics driver
 * keeps its process from ending (End task ends every thread but the one in the kernel wait) and the
 * memory it holds stays until Windows restarts. Told at start. A working copy (a second game, co-op
 * on one machine) has many threads and is left alone.
 * By intent from droogie/bbhost 9df4166 (GPL-3), "warn about a stuck earlier instance". */
#ifndef BBPORT_STUCK_INSTANCE_H
#define BBPORT_STUCK_INSTANCE_H
#include <stdint.h>

/* At most 2 threads and 1 GiB or more in use counts as stuck. */
static inline int bb_instance_is_stuck(uint32_t threads, uint64_t working_set_bytes) {
    return threads <= 2 && working_set_bytes >= (UINT64_C(1) << 30);
}
#endif
