/* Crash dumps: memory at the registers, and the decode of known crash sites (restab081).
 *
 * A guest crash dump had only the register values: for a stale-object read (0x1a21134, a
 * READ of 0x1 in a Havok behaviour event on HavokWorkerThread, CL-078659) the dump could not
 * say which object it was or whose. These helpers read through a callback (the crash handler's
 * ReadProcessMemory wrapper; a fake in tests/test_crash_regmem.c), so a bad pointer never faults
 * again inside the handler. Header-only C, no allocation.
 *
 * Known site (guest 1.09 offsets, our raw ELF VA):
 *   0x1a27fb0 (edi: animation id, rsi: chr) looks the id up in the chr's behaviour module
 *   (chr +0x3b0 -> +0x18, kept in r15) table (+0x10: count +0x30, sorted pointers +0x38; an
 *   entry {u32 id, ..., +0x10: animation object}) and calls 0x1a21100(rdi = module +0x18 ->
 *   listener, rsi = the animation object, kept in r14). 0x1a21100 reads the object's +0x18
 *   (a pointer, then its first dword): 1 there was the crash. On return the caller writes the
 *   event {id, three floats} into a ring of 10 at module +0x20, index +0xc0.
 */
#ifndef BB_CRASH_REGMEM_H
#define BB_CRASH_REGMEM_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* 1: `size` bytes at `address` copied to `out`; 0: unreadable. */
typedef int (*crash_read_fn)(uintptr_t address, void *out, size_t size);

/* A plausible user-space data pointer (guest heaps and host heaps are below 2^47). */
static inline int crash_plausible_pointer(uint64_t v) {
    return v >= 0x10000 && v < (1ULL << 47);
}

static inline int crash_read_u64(crash_read_fn rd, uintptr_t at, uint64_t *out) {
    return rd(at, out, sizeof *out);
}

/* The chr that owns behaviour module `module` (chr +0x3b0 -> +0x18 == module): searched among the
 * module's first `scan` bytes of qwords (FD4 modules keep their owner near the top). 0 if none. */
static inline uint64_t crash_find_module_owner(crash_read_fn rd, uint64_t module, unsigned scan) {
    for (unsigned off = 0; off < scan; off += 8) {
        uint64_t p = 0, modules = 0, back = 0;
        if (!crash_read_u64(rd, (uintptr_t)(module + off), &p) || !crash_plausible_pointer(p)) continue;
        if (!crash_read_u64(rd, (uintptr_t)(p + 0x3b0), &modules) || !crash_plausible_pointer(modules)) continue;
        if (crash_read_u64(rd, (uintptr_t)(modules + 0x18), &back) && back == module) return p;
    }
    return 0;
}

/* 64 bytes at every register that holds a readable pointer. */
static inline void crash_dump_register_memory(FILE *f, crash_read_fn rd, const char *const names[],
                                              const uint64_t values[], int count) {
    fprintf(f, "\n--- MEMORY AT REGISTERS (64 bytes each, readable pointers only; BB_CRASH_REGMEM=0 off) ---\n");
    for (int i = 0; i < count; ++i) {
        uint64_t q[8];
        if (!crash_plausible_pointer(values[i]) || !rd((uintptr_t)values[i], q, sizeof q)) continue;
        fprintf(f, "  %-3s %016llx:", names[i], (unsigned long long)values[i]);
        for (int k = 0; k < 8; ++k) fprintf(f, " %016llx", (unsigned long long)q[k]);
        fprintf(f, "\n");
    }
}

/* The site decode for a fault inside 0x1a21100 (r14: the animation object, r15: the behaviour
 * module). Writes what it can read; returns 1 when the owner chr was found. */
static inline int crash_decode_hkb_event(FILE *f, crash_read_fn rd, uint64_t r14, uint64_t r15) {
    fprintf(f, "\n--- SITE: Havok behaviour event 0x1a21100 (animation annotations; caller 0x1a27fb0) ---\n");
    uint64_t track = 0, listener = 0, table = 0;
    const int have_track = crash_read_u64(rd, (uintptr_t)(r14 + 0x18), &track);
    fprintf(f, "  animation object (r14) %016llx: +0x18 = %s%016llx%s\n", (unsigned long long)r14,
            have_track ? "" : "<unreadable> ", (unsigned long long)track,
            have_track && !crash_plausible_pointer(track) && track ? " (not a pointer: freed or reused memory)" : "");
    crash_read_u64(rd, (uintptr_t)(r15 + 0x18), &listener);
    crash_read_u64(rd, (uintptr_t)(r15 + 0x10), &table);
    fprintf(f, "  behaviour module (r15) %016llx: listener +0x18 %016llx, animation table +0x10 %016llx\n",
            (unsigned long long)r15, (unsigned long long)listener, (unsigned long long)table);
    int32_t count = 0;
    if (crash_plausible_pointer(table) && rd((uintptr_t)(table + 0x30), &count, sizeof count))
        fprintf(f, "  animation table: %d entries\n", count);
    int32_t index = 0;
    if (rd((uintptr_t)(r15 + 0xc0), &index, sizeof index) && index >= 0 && index < 10) {
        fprintf(f, "  last events (oldest first; the faulting one is not in it yet):");
        for (int k = 0; k < 10; ++k) {
            const int slot = (index + k) % 10; /* +0xc0 is the next slot to write: the oldest */
            uint32_t ev[4];
            if (!rd((uintptr_t)(r15 + 0x20 + (uint64_t)slot * 16), ev, sizeof ev)) break;
            float t[3];
            memcpy(t, &ev[1], sizeof t);
            fprintf(f, " %u@%.3f", ev[0], (double)t[0]);
        }
        fprintf(f, "\n");
    }
    const uint64_t chr = crash_find_module_owner(rd, r15, 0x40);
    if (!chr) {
        fprintf(f, "  owner chr: not found among the module's first 0x40 bytes\n");
        return 0;
    }
    uint32_t map_entity = 0, entity = 0;
    rd((uintptr_t)(chr + 0x1f0), &map_entity, sizeof map_entity);
    rd((uintptr_t)(chr + 0x3b8), &entity, sizeof entity);
    fprintf(f, "  owner chr %016llx: map entity id (+0x1f0) %u, entity id (+0x3b8) %u\n", (unsigned long long)chr,
            map_entity, entity);
    return 1;
}

#endif
