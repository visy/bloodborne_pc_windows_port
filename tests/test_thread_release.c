/* A finished guest thread must give back its duplicated Windows handle (runtime_thread.c attach):
 * ~18 guest threads per loading screen leaked one handle each (perf081). BB_THREAD_RELEASE=0 is the old behaviour. */
#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void runtime_restart(void) { abort(); }
void runtime_relaunch(int exit_code) { (void)exit_code; abort(); }
void runtime_note_guest_assert(uint32_t code, uint64_t argument, uint64_t guest_offset) { (void)code; (void)argument; (void)guest_offset; }
#ifdef _WIN32
#include <windows.h>
ABI void restore_guest_fs(void) {}
volatile int runtime_restarting;
#endif

typedef int32_t (ABI *Create)(void **, void *, GuestEntry, void *, const char *);
typedef int32_t (ABI *Join)(void *, void **);
#define GET(t, n) ((t)runtime_resolve(n, 0))

static void *ABI entry(void *arg) { return arg; }

int main(int argc, char **argv) {
    const int expect_leak = argc > 1 && !strcmp(argv[1], "--old");
    if (expect_leak) putenv("BB_THREAD_RELEASE=0");
    runtime_start(1);
    Create create = GET(Create, "6UgtwV+0zb4#p#J");
    Join join = GET(Join, "onNY9Byn-W8#p#J");
    assert(create && join);
#ifdef _WIN32
    const int n = 40;
    DWORD before = 0, after = 0;
    /* one warm-up thread: first-use allocations (key tables, name buffers) are not the leak */
    void *t = NULL, *r = NULL;
    assert(create(&t, NULL, entry, (void *)1, "warm") == 0 && join(t, &r) == 0 && r == (void *)1);
    GetProcessHandleCount(GetCurrentProcess(), &before);
    for (int i = 0; i < n; ++i) {
        assert(create(&t, NULL, entry, (void *)(intptr_t)(i + 2), "worker") == 0);
        assert(join(t, &r) == 0 && r == (void *)(intptr_t)(i + 2));
    }
    GetProcessHandleCount(GetCurrentProcess(), &after);
    printf("handles %lu -> %lu over %d threads (%s)\n", (unsigned long)before, (unsigned long)after, n, expect_leak ? "old" : "fixed");
    if (expect_leak) assert(after >= before + n / 2);
    else assert(after < before + 8);
#endif
    puts("PASS: guest thread handles");
    return 0;
}
