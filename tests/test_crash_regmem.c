/* src/crash_regmem.h: the crash dump's register memory and the 0x1a21100 site decode, on a fake
 * memory (synthetic addresses and ids). Built as out/crash-regmem-test, run by test_crash_regmem.py. */
#include "crash_regmem.h"

#include <assert.h>
#include <stdlib.h>

/* A fake address space: one block at kBase. */
enum { kSize = 0x2000 };
static const uint64_t kBase = 0x100000000ULL;
static unsigned char mem[kSize];

static int fake_read(uintptr_t address, void *out, size_t size) {
    if (address < kBase || address + size > kBase + kSize) return 0;
    memcpy(out, mem + (address - kBase), size);
    return 1;
}
static void put64(uint64_t at, uint64_t v) { memcpy(mem + (at - kBase), &v, 8); }
static void put32(uint64_t at, uint32_t v) { memcpy(mem + (at - kBase), &v, 4); }

static char *dump(FILE *f) {
    static char text[8192];
    fflush(f);
    rewind(f);
    size_t n = fread(text, 1, sizeof text - 1, f);
    text[n] = 0;
    return text;
}

int main(void) {
    const uint64_t chr = kBase + 0x100, modules = kBase + 0x800, module = kBase + 0x900;
    const uint64_t anim = kBase + 0x1000, table = kBase + 0x1100;
    put64(chr + 0x3b0, modules);
    put64(modules + 0x18, module);
    put32(chr + 0x1f0, 1234567);
    put32(chr + 0x3b8, 7654321);
    put64(module + 0x08, chr);                /* the owner, near the module's top */
    put64(module + 0x10, table);
    put64(module + 0x18, module + 0xe0);       /* the listener */
    put32(table + 0x30, 42);
    put32(module + 0xc0, 3);                   /* the next slot to write: the oldest event */
    for (uint32_t k = 0; k < 10; ++k) put32(module + 0x20 + k * 16, 100 + k);
    put64(anim + 0x18, 1);                     /* the stale field the crash read */

    /* The owner is found through chr +0x3b0 -> +0x18 == module, and not from a wrong pointer. */
    assert(crash_find_module_owner(fake_read, module, 0x40) == chr);
    put64(module + 0x08, kBase + 0x40);        /* a pointer whose +0x3b0 is not our module list */
    assert(crash_find_module_owner(fake_read, module, 0x40) == 0);
    put64(module + 0x08, chr);
    assert(crash_find_module_owner(fake_read, 0x10, 0x40) == 0); /* unreadable module */

    FILE *f = tmpfile();
    assert(f);
    assert(crash_decode_hkb_event(f, fake_read, anim, module) == 1);
    char *t = dump(f);
    assert(strstr(t, "+0x18 = 0000000000000001 (not a pointer: freed or reused memory)"));
    assert(strstr(t, "animation table: 42 entries"));
    assert(strstr(t, "last events (oldest first; the faulting one is not in it yet): 103@"));
    assert(strstr(t, "map entity id (+0x1f0) 1234567, entity id (+0x3b8) 7654321"));
    fclose(f);

    /* Registers: only readable pointers are dumped; null, small and unmapped values are skipped. */
    f = tmpfile();
    const char *const names[] = {"RAX", "RBX", "RCX", "R15"};
    const uint64_t values[] = {1, chr, 0x7fff00000000ULL, module};
    crash_dump_register_memory(f, fake_read, names, values, 4);
    t = dump(f);
    assert(!strstr(t, "RAX "));
    assert(!strstr(t, "RCX "));
    assert(strstr(t, "RBX 0000000100000100:"));
    assert(strstr(t, "R15 0000000100000900: 0000000000000000 0000000100000100 0000000100001100"));
    fclose(f);

    /* An unreadable animation object and module: the decode says so and does not fault. */
    f = tmpfile();
    assert(crash_decode_hkb_event(f, fake_read, 0x20, 0x30) == 0);
    t = dump(f);
    assert(strstr(t, "<unreadable>"));
    assert(strstr(t, "owner chr: not found"));
    fclose(f);

    printf("crash-regmem-test: OK\n");
    return 0;
}
