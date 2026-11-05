// bbport: GPU-side assertion failures stop the port with exit code 23.
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "common/assert.h"
#include "common/logging/log.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

// bb-probe (probe.c): set while the port restarts itself through run.sh. The device fd is closed
// before exec; Vulkan calls failing then are not errors: this thread waits for the exec instead.
extern "C" __attribute__((weak)) volatile int runtime_restarting; // absent in the tests
extern "C" __attribute__((weak)) void runtime_logdrain_flush(unsigned timeout_ms); // src/runtime_logdrain.c, absent in the tests

namespace Common {
// bbport: set on threads that rebuild shaders from the cache's sources: an assertion there means
// that one stored shader does not replay with this build, which must skip it, not stop the game.
thread_local bool assert_throws = false;
void SetAssertThrowsOnThisThread(bool value) {
    assert_throws = value;
}
} // namespace Common

void assert_fail_impl() {
    if (Common::assert_throws) {
        throw std::runtime_error("assertion failed");
    }
    if (&runtime_restarting && runtime_restarting) {
        for (;;) {
#ifdef _WIN32
            Sleep(INFINITE);
#else
            pause();
#endif
        }
    }
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
    std::fflush(stderr);
#ifdef _WIN32
    // Not _Exit: unloading the Vulkan driver while GPU threads are inside it can hang the process.
    if (&runtime_logdrain_flush) {
        runtime_logdrain_flush(3000);  // the last log lines (the GPU log above, the STOP line) reach the log first
    }
    TerminateProcess(GetCurrentProcess(), 23);
#endif
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
