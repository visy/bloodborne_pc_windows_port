/* Exit deadline. Idea after droogie/bbhost 16e6da5 (GPL-3, ported by intent, no code copied).
 * A present stuck in the graphics driver holds the renderer, the presenting thread and the driver's
 * lock for good, and the exit's later steps (pipeline cache save, window end, device wait) then wait
 * on one of them: the window reads "Not Responding" until it is ended from Task Manager.
 * So: saves first (wait while a save file is open for writing or a commit is queued, up to 30 s),
 * then a deadline (15 s; BB_EXIT_DEADLINE=0 off, =N seconds). Past it a helper thread waits for
 * saves once more (up to 10 s), flushes the log drain and ends the process (TerminateProcess).
 * A normal exit takes 1-3 s and never meets it. */
#ifndef RUNTIME_EXITGUARD_H
#define RUNTIME_EXITGUARD_H
#ifdef __cplusplus
extern "C" {
#endif
#define RUNTIME_EXITGUARD_DEFAULT_SECONDS 15
#define RUNTIME_EXITGUARD_MAX_SECONDS 600

/* Pure: the deadline in seconds for a BB_EXIT_DEADLINE value (NULL or empty or not a number: the
 * default; "0": 0 = off; N: N, at most 600). */
int runtime_exitguard_parse_seconds(const char *value);

typedef struct {
    int (*saves_busy)(void);   /* save files open for writing + queued commits; NULL = none */
    void (*end)(int code);     /* ends the process (tests); NULL = flush log drain + TerminateProcess/_exit */
} ExitGuardOps;
void runtime_exitguard_configure(const ExitGuardOps *ops);

/* Waits while saves are busy, at most max_ms. Returns 1 when none is busy at the end. */
int runtime_exitguard_wait_saves(unsigned max_ms);
/* The deadline: starts a helper that ends the process after `seconds` (BB_EXIT_DEADLINE when
 * seconds < 0). Once per process. Returns the seconds armed, 0 when off or already armed. */
int runtime_exitguard_arm(int seconds, int code);
/* Saves first, then the deadline with the env's seconds: call at the start of any exit path. */
void runtime_exitguard_begin(int code);
/* Test aid BB_TEST_HANG_PRESENT_AT_EXIT=1: never returns (a present stuck in the driver). */
void runtime_exitguard_test_hang(void);
#ifdef __cplusplus
}
#endif
#endif
