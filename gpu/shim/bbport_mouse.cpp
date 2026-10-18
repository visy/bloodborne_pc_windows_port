// See bbport_mouse.h. Owns SDL relative-state sampling: the window pump never
// reads it, so motion is neither split nor replayed between the two threads.
#include "bbport_mouse.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <SDL3/SDL.h>

#include "bloodborne_cam.h"
#include "bbport_settings.h"
#include "common/logging/log.h"
#include "common/thread.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace BbMouse {
namespace {

std::thread g_thread;
std::atomic<bool> g_running{false};
std::atomic<bool> g_active{false};

#ifdef _WIN32
HANDLE g_stopEvent = nullptr;
HANDLE g_timer = nullptr;
#endif

struct TestPanConfig {
    float rate = 0.0f;
    double ab_seconds = 0.0;
};

const TestPanConfig& TestPanSettings() {
    static const TestPanConfig config = [] {
        TestPanConfig c;
        if (const char* v = std::getenv("BB_CAM_TEST_PAN")) {
            char* end = nullptr;
            c.rate = std::strtof(v, &end);
            if (end && *end == ',') {
                c.ab_seconds = std::strtod(end + 1, nullptr);
            }
        }
        return c;
    }();
    return config;
}

// BB_CAM_TEST_PAN: the steady pan's counts for this sample. With SECONDS: from 5 s after the
// camera first updates, SECONDS applied in the game's update, then SECONDS from the mouse thread,
// then the pan stops (one A/B per session).
float TestPanCounts() {
    const TestPanConfig& c = TestPanSettings();
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last = Clock::now(), start{};
    static int phase = -1;
    const Clock::time_point now = Clock::now();
    const double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    if (c.ab_seconds > 0.0) {
        auto& cam = Core::BloodborneCam::Instance();
        if (start == Clock::time_point{}) {
            if (!cam.CameraLive()) {
                return 0.0f;
            }
            start = now + std::chrono::seconds(5);
        }
        const double t = std::chrono::duration<double>(now - start).count();
        const int want = t < 0.0 ? -1 : std::min(2, static_cast<int>(t / c.ab_seconds));
        if (want != phase) {
            phase = want;
            if (phase >= 0 && phase < 2) {
                cam.SetApplyInGame(phase == 0);
            }
            std::printf("Camera test pan: %s\n", phase < 0 ? "waiting" : phase == 0 ? "apply in the game's update"
                                                 : phase == 1 ? "apply from the mouse thread" : "done");
            std::fflush(stdout);
        }
        if (phase != 0 && phase != 1) {
            return 0.0f;
        }
    }
    return dt < 0.1 ? static_cast<float>(c.rate * dt) : 0.0f;
}

void Sample() {
    if (g_active.load(std::memory_order_acquire)) {
        float xrel = 0.0f;
        float yrel = 0.0f;
        SDL_GetRelativeMouseState(&xrel, &yrel);
        if (xrel != 0.0f || yrel != 0.0f) {
            // bbport: the mouse & keyboard look settings (sensitivity, invert) apply here too.
            const auto& s = BbSettings::Get();
            xrel *= s.mk_sens_x.load() * (s.mk_invert_x.load() ? -1.0f : 1.0f);
            yrel *= s.mk_sens_y.load() * (s.mk_invert_y.load() ? -1.0f : 1.0f);
        }
        if (TestPan()) {
            xrel += TestPanCounts();
        }
        if (xrel != 0.0f || yrel != 0.0f) {
            Core::BloodborneCam::Instance().OnMouseMotion(xrel, yrel);
        }
    } else {
        // Gated pumps must not bank motion for a later sample.
        SDL_GetRelativeMouseState(nullptr, nullptr);
    }
}

#ifdef _WIN32
void ThreadMain() {
    Common::SetCurrentThreadName("bb:mouse");
    if (g_timer != nullptr) {
        HANDLE handles[2] = {g_stopEvent, g_timer};
        for (;;) {
            const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (wait != WAIT_OBJECT_0 + 1) {
                break;
            }
            Sample();
        }
    } else {
        while (WaitForSingleObject(g_stopEvent, 1) == WAIT_TIMEOUT) {
            Sample();
        }
    }
}
#else
void ThreadMain() {
    Common::SetCurrentThreadName("bb:mouse");
    while (g_running.load(std::memory_order_acquire)) {
        Sample();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
#endif

} // namespace

void Start() {
    if (g_running.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
#ifdef _WIN32
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                     TIMER_ALL_ACCESS);
    if (g_stopEvent == nullptr) {
        g_running.store(false, std::memory_order_release);
        LOG_ERROR(Frontend, "BbMouse: stop event creation failed");
        return;
    }
    if (g_timer != nullptr) {
        LARGE_INTEGER due{};
        due.QuadPart = -10000; // 1 ms, relative
        if (!SetWaitableTimer(g_timer, &due, 1, nullptr, nullptr, FALSE)) {
            CloseHandle(g_timer);
            g_timer = nullptr;
        }
    }
#endif
    g_thread = std::thread(ThreadMain);
}

void Stop() {
    if (!g_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
#ifdef _WIN32
    if (g_stopEvent != nullptr) {
        SetEvent(g_stopEvent);
    }
#endif
    if (g_thread.joinable()) {
        g_thread.join();
    }
#ifdef _WIN32
    if (g_timer != nullptr) {
        CloseHandle(g_timer);
        g_timer = nullptr;
    }
    if (g_stopEvent != nullptr) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
#endif
}

bool TestPan() {
    return TestPanSettings().rate != 0.0f;
}

void SetActive(bool active) {
    g_active.store(active, std::memory_order_release);
}

} // namespace BbMouse
