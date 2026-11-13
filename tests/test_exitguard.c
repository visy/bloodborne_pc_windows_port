/* Exit guard (src/runtime_exitguard.c).
 *   exitguard-test                 pure parse + wait/arm logic with a fake end
 *   exitguard-test --hang DIR      saves a file, then hangs; the guard must end the process (exit code 7) */
#include "runtime_exitguard.h"
#include <assert.h>
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
static volatile int busy, ended = -1;
static int busy_fn(void) { return busy; }
static void end_fn(int code) { ended = code; }
int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "--hang")) { /* real end: the guard terminates this process */
        char p[600]; snprintf(p, sizeof p, "%s/save.dat", argv[2]);
        FILE *f = fopen(p, "wb"); assert(f); fputs("whole", f); fclose(f);
        runtime_exitguard_arm(1, 7);
        for (;;) MSLEEP(1000);
    }
    assert(runtime_exitguard_parse_seconds(NULL) == 15);
    assert(runtime_exitguard_parse_seconds("") == 15);
    assert(runtime_exitguard_parse_seconds("abc") == 15);
    assert(runtime_exitguard_parse_seconds("-3") == 15);
    assert(runtime_exitguard_parse_seconds("0") == 0);
    assert(runtime_exitguard_parse_seconds("5") == 5);
    assert(runtime_exitguard_parse_seconds("99999") == 600);
    ExitGuardOps ops = {busy_fn, end_fn};
    runtime_exitguard_configure(&ops);
    assert(runtime_exitguard_wait_saves(200) == 1);
    busy = 1;
    assert(runtime_exitguard_wait_saves(200) == 0);
    /* arm: waits for the busy save (it clears after 1.5 s), then ends with the code, once */
    assert(runtime_exitguard_arm(1, 9) == 1);
    assert(runtime_exitguard_arm(1, 9) == 0);
    MSLEEP(1500);
    assert(ended == -1); /* the deadline passed but a save is still busy */
    busy = 0;
    for (int i = 0; i < 40 && ended < 0; ++i) MSLEEP(50);
    assert(ended == 9);
    puts("exitguard-test ok");
    return 0;
}
