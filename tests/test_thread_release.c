/* A finished guest thread must give back its duplicated Windows handle (runtime_thread.c attach):
 * ~18 guest threads per loading screen leaked one handle each (perf081). BB_THREAD_RELEASE=0 is the old behaviour. */
#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdalign.h>
#include <string.h>

void runtime_restart(void) { abort(); }
void runtime_relaunch(int exit_code) { (void)exit_code; abort(); }
void runtime_note_guest_assert(uint32_t code, uint64_t argument, uint64_t guest_offset) { (void)code; (void)argument; (void)guest_offset; }
#ifdef _WIN32
#include <windows.h>
#include <winternl.h>
/* Thread-type handles only: other kinds (registry keys opened per new thread by system DLLs) come and go lazily
 * and made a plain GetProcessHandleCount comparison flaky. */
static unsigned typed_handles(const wchar_t *type) {
    typedef LONG (NTAPI *Query)(HANDLE, int, PVOID, ULONG, PULONG);
    static Query query;
    if (!query) query=(Query)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject");
    unsigned n=0;
    for (uintptr_t h=4; h<0x40000; h+=4) {
        DWORD flags; if (!GetHandleInformation((HANDLE)h,&flags)) continue;
        alignas(8) char buf[512]; ULONG ret=0;
        if (query((HANDLE)h,2,buf,sizeof(buf),&ret)<0) continue;
        const UNICODE_STRING *name=(const UNICODE_STRING *)buf;
        if (name->Length==wcslen(type)*2 && !wcsncmp(name->Buffer,type,wcslen(type))) ++n;
    }
    return n;
}
static unsigned thread_handles(void) { return typed_handles(L"Thread"); }
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
    const int n = getenv("TR_N") ? atoi(getenv("TR_N")) : 40;
    unsigned before = 0, after = 0;
    /* one warm-up thread: first-use allocations (key tables, name buffers) are not the leak */
    void *t = NULL, *r = NULL;
    assert(create(&t, NULL, entry, (void *)1, "warm") == 0 && join(t, &r) == 0 && r == (void *)1);
    before = thread_handles(); const unsigned ev0 = typed_handles(L"Event");
    for (int i = 0; i < n; ++i) {
        assert(create(&t, NULL, entry, (void *)(intptr_t)(i + 2), "worker") == 0);
        assert(join(t, &r) == 0 && r == (void *)(intptr_t)(i + 2));
    }
    if (getenv("TR_SLEEP")) Sleep((DWORD)atoi(getenv("TR_SLEEP")));
    after = thread_handles(); const unsigned ev1 = typed_handles(L"Event");
    printf("event handles %u -> %u\n", ev0, ev1);
    printf("thread handles %u -> %u over %d threads (%s)\n", before, after, n, expect_leak ? "old" : "fixed");
    if (expect_leak) assert(after >= before + n / 2);
    else assert(after < before + 8);
#endif
    puts("PASS: guest thread handles");
    return 0;
}
