/* See runtime_exitguard.h. */
#include "runtime_exitguard.h"
#include "runtime_logdrain.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define MSLEEP(ms) Sleep(ms)
#else
#include <unistd.h>
#define MSLEEP(ms) usleep((ms)*1000)
#endif

static ExitGuardOps ops;
static volatile int armed;

int runtime_exitguard_parse_seconds(const char *value) {
    if (!value || !*value) return RUNTIME_EXITGUARD_DEFAULT_SECONDS;
    char *end = NULL;
    long n = strtol(value, &end, 10);
    if (end == value || *end || n < 0) return RUNTIME_EXITGUARD_DEFAULT_SECONDS;
    return n > RUNTIME_EXITGUARD_MAX_SECONDS ? RUNTIME_EXITGUARD_MAX_SECONDS : (int)n;
}
void runtime_exitguard_configure(const ExitGuardOps *o) { ops = o ? *o : (ExitGuardOps){0}; }

int runtime_exitguard_wait_saves(unsigned max_ms) {
    if (!ops.saves_busy) return 1;
    for (unsigned t = 0; t < max_ms && ops.saves_busy() > 0; t += 50) MSLEEP(50);
    return ops.saves_busy() <= 0;
}
static void end_process(int code) {
    if (ops.end) { ops.end(code); return; }
    fflush(NULL);
#ifdef _WIN32
    runtime_logdrain_flush(3000);
    TerminateProcess(GetCurrentProcess(), (UINT)code);
#endif
    _exit(code);
}
typedef struct { int seconds, code; } Arm;
static void *deadline_thread(void *p) {
    Arm a = *(Arm *)p;
    free(p);
    for (int i = 0; i < a.seconds * 20; ++i) MSLEEP(50);
    printf("exit: not done %d s on - the rest skipped (the renderer, the GPU or the driver is stuck); saves first, then the end\n", a.seconds);
    if (!runtime_exitguard_wait_saves(10000)) printf("exit: a save is still being written after 10 s; ending without waiting more\n");
    end_process(a.code);
    return NULL;
}
int runtime_exitguard_arm(int seconds, int code) {
    if (seconds < 0) seconds = runtime_exitguard_parse_seconds(getenv("BB_EXIT_DEADLINE"));
    if (seconds <= 0) return 0;
    if (__sync_lock_test_and_set(&armed, 1)) return 0;
    Arm *a = malloc(sizeof(*a));
    pthread_t t;
    if (!a) { armed = 0; return 0; }
    a->seconds = seconds; a->code = code;
    if (pthread_create(&t, NULL, deadline_thread, a)) { free(a); armed = 0; return 0; }
    pthread_detach(t);
    return seconds;
}
void runtime_exitguard_begin(int code) {
    if (runtime_exitguard_parse_seconds(getenv("BB_EXIT_DEADLINE")) <= 0) return;
    if (!runtime_exitguard_wait_saves(30000)) printf("exit: a save is still being written after 30 s; ending without waiting more\n");
    runtime_exitguard_arm(-1, code);
}
void runtime_exitguard_test_hang(void) {
    const char *e = getenv("BB_TEST_HANG_PRESENT_AT_EXIT");
    if (!e || *e != '1') return;
    printf("exit: BB_TEST_HANG_PRESENT_AT_EXIT - this step never returns (a present stuck in the driver)\n");
    fflush(stdout);
    for (;;) MSLEEP(1000);
}
