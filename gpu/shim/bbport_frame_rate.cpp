// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the game's frame rate set free by hooks in its code (bbport_frame_rate.h).
// Written by deadinside for Supermedo/bloodborne_pc 0.6-pre1 (72e5c8ce); ported to the Windows
// port (near-image memory and protection by VirtualAlloc / VirtualProtect through the runtime's
// low area, flags and xmm kept around the per-frame call, the BB_FRAME_HOOKS switch,
// diagnostics counters).
//
// The game's main loop (0x2018d20) runs its frame pacer (0x2034770: it waits for the frame time
// its mode asks, 1/30 s, and measures the frame at [pacer + 0x264]) and then steps the game by a
// fixed 1/30 s (0x2018e36). The hooks keep the pacer's mode at one vblank and no frame time to
// wait for beyond the shortest frame, and step the game by the measured frame time (1/480 to
// 1/30 s: below 30 FPS the game slows down, as it always did).
//
// What the game ties to a 30 FPS frame besides (found by the frame time each function is called
// with, and by what its code does with the constant): subsystems stepped by a fixed 1/30 s,
// per-frame steps and approaches, per-frame damping. Their instructions read the constant from a
// page next to the image instead (the instruction's own address operand moved; the game's code is
// otherwise as it was), where FrameStep writes the value for this frame's time: a step of 1/30 s
// becomes the frame's time, a per-frame factor f becomes f^(frames) for the frame's length in
// 30 FPS frames. Durations the game converts from 30 FPS frames into seconds stay as they are:
// what counts them down uses the real time.
//
// The party co-op layer hooks the same pacer's entry (0x2034770: party_director, lamp_travel,
// world_warper) and fast loading writes its +0x273 "no sleep" byte; these hooks are inside the
// function (0x203493d, 0x2034d96) and in the main loop (0x2018e36), so the bytes do not overlap.
#include "bbport_stability.h"
#include "bbport_frame_rate.h"
#include "bbport_frame_rate_logic.h"
#include "bbport_guest_code.h"
#include "bbport_settings.h"
#include "party/game_state.h"
#include "party/seamless_rules.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <new>
#include <thread>
#include <unordered_map>
#include <vector>
#include <string>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

extern "C" void* runtime_low_map(size_t size, int prot); // src/runtime_memory.c

#define BB_FR_SYSV __attribute__((sysv_abi))

namespace BbFrameRate {
void StartSweep(); // bbport_frame_sweep.cpp
void SweepOnStep(); // bbport_frame_sweep.cpp: its main-thread work (a warp home), when asked
extern std::atomic<bool> sweep_step_pending;
namespace {
using BbGuestCode::Emitter;
using u64 = std::uint64_t;
using u8 = std::uint8_t;

std::atomic<bool> unlocked{false};
u64 image_base = 0;
u64 image_size = 0;

Live* live = nullptr;
// Diagnostics: game time stepped (sum of dt) and steps, written by the main thread in FrameStep.
std::atomic<double> game_seconds{0.0};
std::atomic<u64> steps{0};

bool EnvOff(const char* name) {
    const char* v = std::getenv(name);
    return v && (!std::strcmp(v, "0") || !std::strcmp(v, "off") || !std::strcmp(v, "false"));
}

/// Test aid: BB_TEST_FRAME_STALL_FILE + BB_TEST_FRAME_STALL_MS=N: when the file exists (the harness's
/// host_gfx=stallcap creates it) it is removed and this frame stalls N ms, as a real stutter does (the
/// next frame measures N ms, then short frames catch up). For the steady-time proof (cloth).
void TestStall() {
    static const char* file = std::getenv("BB_TEST_FRAME_STALL_FILE");
    if (!file || !*file) return;
    static const int ms = [] {
        const char* v = std::getenv("BB_TEST_FRAME_STALL_MS");
        return v ? std::clamp(std::atoi(v), 0, 2000) : 0;
    }();
    static u64 n = 0;
    static int trace = 0;
    if (trace > 0) { // the cloth's time around the stall: as given / as stepped (the last update of the previous frame)
        std::printf("Frame rate: test stall trace: dt %.5f s, steady %.5f s, cloth time given %.5f s, stepped %.5f s\n",
                    double(live->dt), double(live->steady), double(live->cloth_in), double(live->cloth_out));
        --trace;
    }
    if (ms <= 0 || (++n % 4) != 0) return;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || !std::filesystem::remove(file, ec)) return;
    std::printf("Frame rate: test stall %d ms (step %llu, last dt %.5f s, steady %.5f s)\n", ms,
                (unsigned long long)steps.load(std::memory_order_relaxed), double(live->dt), double(live->steady));
    std::fflush(stdout);
    trace = 14;
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::atomic<bool> cloth_hook_on{false};
float cloth_exponent = -1.725f; ///< Havok's stiffness adaptation exponent (0x48d7bbc), read at start
std::atomic<u64> cloth_calls{0};

/// Havok's stiffness factor of a constraint set for one substep (0x1167a80, replaced). Ported from
/// deadinside28's ClothStiffnessFactor (bloodborne_pc 0fc2921c, their issue #58; GPL-2.0-or-later).
/// `mode` 2 adapts the constraints' stiffness to how often they are solved, (substeps x a x b)^-1.725
/// (a, b the instance's scales at +0x1d0/+0x1d4); for a set of type 8 the iteration's share instead,
/// for types 5 and 10 only the last substep. The substeps are per frame, so at a high frame rate the
/// cloth is solved more often by the same factor and is stiffer by as much (cloaks and hair flutter).
/// The factor here is the game's x frames (the frame's length in 30 FPS frames): the cloth is pulled
/// as far per second as at 30 FPS. At 30 FPS it is the game's.
float BB_FR_SYSV ClothStiffnessFactor(const u8* set, int mode, int iteration, int substeps, float a,
                                      float b) {
    const std::uint32_t type = *reinterpret_cast<const std::uint32_t*>(set + 0x18);
    const u64 n = cloth_calls.fetch_add(1, std::memory_order_relaxed);
    if (n == 20000) {
        std::printf("Frame rate: cloth stiffness factor: constraint type %u, %.0f FPS: factor x%.3f\n", type,
                    1.0f / live->dt, live->dt * 30.0f);
    }
    const bool adapts = mode == 2 && !(substeps == 1 && a == 1.0f && b == 1.0f);
    if (adapts && type == 8) return float(iteration + 1) / float(substeps);
    if (adapts && (type == 5 || type == 10)) return substeps - 1 == iteration ? 1.0f : 0.0f;
    const float game = adapts ? std::pow(float(substeps) * a * b, cloth_exponent) : 1.0f;
    if (type == 5 || type == 8 || type == 10) return game;
    return game * (live->dt * 30.0f);
}

/// The pacer at the main loop's step (rdi: it): this frame's values.
void BB_FR_SYSV FrameStep(u64 pacer) {
    const float measured = *reinterpret_cast<const float*>(pacer + 0x264);
    float dt = StepFor(measured, live->shortest);
    // BB_FRAME_DT_SMOOTH=a (diagnostics, 0 < a < 1): the step follows the measured frame time by
    // that fraction a frame, so frame-to-frame jitter of the measurement does not reach the
    // simulation (time stays right on average).
    static const float smooth = [] {
        const char* env = std::getenv("BB_FRAME_DT_SMOOTH");
        const float a = env && *env ? std::strtof(env, nullptr) : 0.0f;
        return a > 0.0f && a < 1.0f ? a : 0.0f;
    }();
    if (smooth > 0.0f) {
        static float smoothed = 0.0f;
        smoothed = smoothed > 0.0f ? smoothed + smooth * (dt - smoothed) : dt;
        dt = std::clamp(smoothed, live->shortest, Frame30);
    }
    // It is the upper bound the cloth setups clamp their Stiffness, Rigid Factor and Allowed
    // Compression to when they load. BB_CLOTH_STIFFNESS=value (diagnostics): that bound at any
    // frame rate.
    static const float cloth_override = [] {
        const char* env = std::getenv("BB_CLOTH_STIFFNESS");
        return env && *env ? std::strtof(env, nullptr) : -1.0f;
    }();
    Compute(*live, dt, cloth_override);
    // The range checks only warn; the cloth follows the frame rate by ClothStiffnessFactor, so they get
    // the game's 1.0 while that hook is in (BB_CLOTH_STIFFNESS=value: that value instead).
    if (cloth_hook_on && !(cloth_override > 0.0f)) live->cloth = 1.0f;
    // BB_STEADY_TIME=0 (diagnostics): the cloth's and the plants' time is the frame's, as before.
    static const bool steady_off = EnvOff("BB_STEADY_TIME");
    if (steady_off) {
        live->steady = dt;
        live->cloth_ratio = 1.0f;
    }
    if (sweep_step_pending.load(std::memory_order_relaxed)) SweepOnStep();
    game_seconds.store(game_seconds.load(std::memory_order_relaxed) + dt, std::memory_order_relaxed);
    steps.fetch_add(1, std::memory_order_relaxed);
    TestStall();
}

// ---- Redirected constants: instruction (its RIP-relative operand), the constant, the value ----
struct Redirect {
    u64 site;
    u64 constant;
    std::size_t value; ///< offsetof(Live, ..)
};
#define LIVE(field) offsetof(Live, field)
constexpr Redirect Redirects[] = {
    // A fixed 1/30 s step or "one frame" (the patches' lists, checked one by one):
    {0xab9f0d, 0x47efdcc, LIVE(dt)},   // a time step, one frame when unset
    {0xdca3ed, 0x4888688, LIVE(dt)},   // a per-frame budget
    {0xee596a, 0x48ad65c, LIVE(dt)},   // effects: a fade of at least one frame (three releases)
    {0xee5a8a, 0x48ad660, LIVE(dt)},
    {0xee5baa, 0x48ad664, LIVE(dt)},
    {0xef06f1, 0x48adc08, LIVE(dt)},   // at least one frame
    {0xef6992, 0x48add28, LIVE(dt)},
    {0x1044943, 0x48cc3f4, LIVE(dt)},  // an update by one frame
    {0x12a4416, 0x492577c, LIVE(dt)},  // a timer advanced by one frame each frame
    {0x15dc1f7, 0x4926c9c, LIVE(dt)},
    {0x173456e, 0x49270e4, LIVE(dt)},  // an update by one frame
    {0x17f9d26, 0x4927554, LIVE(dt)},
    {0x180f647, 0x49275f4, LIVE(dt)},  // a timer advanced by one frame each frame
    {0x18bc4f7, 0x4927c4c, LIVE(dt)},  // an update by one frame (twice)
    {0x18bc50d, 0x4927c4c, LIVE(dt)},
    {0x1dbc308, 0x4928e98, LIVE(dt)},  // frames counted at one frame's time
    // 0x1f77d4a (SprjSfx::Update's step, 0x4929170): not here, the effects step in whole 1/60 s
    // (HookSfxStep, both modes).
    {0x227784b, 0x4929b80, LIVE(dt)},  // timers advanced by one frame (0xebbda0)
    {0x2277892, 0x4929b80, LIVE(dt)},
    // The plants' wind (0x2313870: the grass wind's step, its 60 Hz mode path; bbhost b2818b4's
    // frame-time site 0x2713d8d): a simulation step, 1/60 in the game's 60 Hz mode, 1/30 otherwise.
    // The steady time, not the frame's (Supermedo e0bf9f48): frames of uneven length made them jolt.
    {0x2313d8d, 0x4929e70, LIVE(steady)},
    {0x2313dad, 0x4929e70, LIVE(steady)},
    {0x2313db7, 0x4929e74, LIVE(steady)},
    {0x2315d71, 0x4929e74, LIVE(steady)},
    // Body parts the physics moves (0x1515ec0, each part every frame): their timers advanced by the
    // step they are given (0x180df90 at +0xf0, 0x1811650 at +0xd0), 1/30 s; not in the patches.
    {0x1515fab, 0x4926364, LIVE(dt)},
    {0x1516030, 0x4926364, LIVE(dt)},
    // Steps and approaches per frame:
    {0x19084f5, 0x4927fd4, LIVE(step_down)}, // a value ramped by 0.1 a frame
    {0x1908522, 0x4927fd4, LIVE(step_down)},
    {0x1908518, 0x4927fe0, LIVE(step_up)},
    {0x190854c, 0x4927fe0, LIVE(step_up)},
    {0x143e2b2, 0x4925e94, LIVE(approach30)}, // the follow camera's approaches
    {0x143af8c, 0x4925e68, LIVE(approach10)},
    // Damping per frame:
    {0x1a4abca, 0x490b5b0, LIVE(decay95)}, // a character's motion (0x1a4a730, x0.95 a frame)
    {0x1514c4d, 0x4926334, LIVE(decay08)}, // the run's wall check (x0.8 a frame while blocked)
    // Cloth constraints' stiffness:
    {0x47c6dc, 0x47693f0, LIVE(cloth)},
    {0x7e45be, 0x47b63e4, LIVE(cloth)},
    {0x7e4642, 0x47b63e4, LIVE(cloth)},
    {0x7e6239, 0x47b63e4, LIVE(cloth)},
    {0x7f706d, 0x47b8474, LIVE(cloth)},
    {0x7f70ad, 0x47b8474, LIVE(cloth)},
    {0x7f79e5, 0x47b8474, LIVE(cloth)},
    {0x7f7be8, 0x47b8474, LIVE(cloth)},
    {0x7f7dbd, 0x47b8474, LIVE(cloth)},
    {0x805898, 0x47b91c0, LIVE(cloth)},
    {0x805d18, 0x47b91c0, LIVE(cloth)},
    {0x806269, 0x47b91c0, LIVE(cloth)},
    {0x8062f0, 0x47b91c0, LIVE(cloth)},
    {0x809d62, 0x47b98b8, LIVE(cloth)},
    {0x80a4c9, 0x47b98b8, LIVE(cloth)},
    {0x80a876, 0x47b98b8, LIVE(cloth)},
};
#undef LIVE

/// Writes `n` bytes at `at` in the image (protection restored afterwards).
bool WriteCode(u8* at, const void* bytes, std::size_t n) {
#ifdef _WIN32
    DWORD old = 0, unused = 0;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &unused);
    FlushInstructionCache(GetCurrentProcess(), at, n);
#else
    const u64 page = u64(sysconf(_SC_PAGESIZE));
    const u64 start = reinterpret_cast<u64>(at) & ~(page - 1);
    mprotect(reinterpret_cast<void*>(start), reinterpret_cast<u64>(at) + n - start,
             PROT_READ | PROT_WRITE | PROT_EXEC);
    std::memcpy(at, bytes, n);
#endif
    return true;
}

/// The instruction at `r.site` reads `r.constant` RIP-relative (a VEX instruction, no immediate
/// after the operand): it reads the live value instead.
bool RedirectOperand(const Redirect& r) {
    u8* code = reinterpret_cast<u8*>(image_base + r.site);
    std::size_t disp_at = 0, length = 0;
    if (code[0] == 0xc5 && (code[3] & 0xc7) == 0x05) {
        disp_at = 4, length = 8;
    } else if (code[0] == 0xc4 && (code[4] & 0xc7) == 0x05) {
        disp_at = 5, length = 9;
    } else {
        return false;
    }
    std::int32_t disp = 0;
    std::memcpy(&disp, code + disp_at, 4);
    const u64 next = image_base + r.site + length;
    if (next + std::int64_t(disp) != image_base + r.constant) return false;
    const std::int64_t to = std::int64_t(reinterpret_cast<u64>(live) + r.value) - std::int64_t(next);
    if (to != std::int32_t(to)) return false;
    const std::int32_t to32 = std::int32_t(to);
    return WriteCode(code + disp_at, &to32, 4);
}

// ---- Detours: the instructions at a site replaced by a jump to a stub ----
u8* stub_page = nullptr;
std::size_t stub_used = 0;
constexpr std::size_t StubPageSize = 4096;

void StubWritable(bool writable) {
#ifdef _WIN32
    DWORD unused = 0;
    VirtualProtect(stub_page, StubPageSize, writable ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ, &unused);
    if (!writable) FlushInstructionCache(GetCurrentProcess(), stub_page, StubPageSize);
#else
    mprotect(stub_page, StubPageSize, writable ? PROT_READ | PROT_WRITE | PROT_EXEC : PROT_READ | PROT_EXEC);
#endif
}

/// `e` into the stub page; a jump to it at `site` over `length` bytes (the rest int3) if the site
/// holds `expected`.
bool Detour(u64 site, const u8* expected, std::size_t length, const Emitter& e) {
    u8* code = reinterpret_cast<u8*>(image_base + site);
    if (length < 5 || length > 64 || std::memcmp(code, expected, length) != 0 || e.overflow ||
        stub_used + e.size > StubPageSize) {
        return false;
    }
    u8* stub = stub_page + stub_used;
    const std::int64_t rel = std::int64_t(reinterpret_cast<u64>(stub)) - std::int64_t(image_base + site + 5);
    if (rel != std::int32_t(rel)) return false;
    std::memcpy(stub, e.code.data(), e.size);
    stub_used += (e.size + 15) & ~std::size_t(15);
    const std::int32_t rel32 = std::int32_t(rel);
    u8 jump[64];
    jump[0] = 0xe9;
    std::memcpy(jump + 1, &rel32, 4);
    for (std::size_t i = 5; i < length; ++i) jump[i] = 0xcc;
    return WriteCode(code, jump, length);
}

/// The pacer's mode, after the game chose it (0x203493d, `lea r15, [r12 + 0x268]`; r12 the
/// pacer): one vblank (+0x10), no flip skipping (+0x14), the shortest frame as the frame time
/// target (+0x18), no vblank counts to wait for (+0x268, +0x26c).
bool HookPacerMode() {
    constexpr u8 code[] = {0x4d, 0x8d, 0xbc, 0x24, 0x68, 0x02, 0x00, 0x00};
    Emitter e;
    e.Bytes({0x41, 0xc7, 0x44, 0x24, 0x10, 0x01, 0x00, 0x00, 0x00}); // mov dword [r12 + 0x10], 1
    e.Bytes({0x41, 0xc6, 0x44, 0x24, 0x14, 0x00});                   // mov byte [r12 + 0x14], 0
    e.Bytes({0x50, 0x48, 0xb8});                                       // push rax; movabs rax,
    e.U64(reinterpret_cast<u64>(&live->shortest));                     //   &shortest
    e.Bytes({0x8b, 0x00});                                             // mov eax, [rax]
    e.Bytes({0x41, 0x89, 0x44, 0x24, 0x18, 0x58});                     // mov [r12 + 0x18], eax; pop rax
    e.Bytes({0x49, 0xc7, 0x84, 0x24, 0x68, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}); // mov qword [r12 + 0x268], 0
    for (const u8 b : code) e.Bytes({b});
    e.JmpAbs(image_base + 0x203493d + sizeof(code));
    return Detour(0x203493d, code, sizeof(code), e);
}

/// The pacer keeps its one vblank when a frame came late (0x2034d96: `test dl, dl; je; mov eax,
/// [r12 + 0x10]`, then eax is stored back: 0 when late).
bool HookPacerLate() {
    constexpr u8 code[] = {0x84, 0xd2, 0x74, 0x05, 0x41, 0x8b, 0x44, 0x24, 0x10};
    Emitter e;
    e.Bytes({0x84, 0xd2});                   // test dl, dl
    e.Bytes({0x41, 0x8b, 0x44, 0x24, 0x10}); // mov eax, [r12 + 0x10]
    e.JmpAbs(image_base + 0x2034d9f);
    return Detour(0x2034d96, code, sizeof(code), e);
}

/// The main loop's step (0x2018e36: `mov dword [rbp - 0x38], 1/30`, its time; rbx the pacer):
/// FrameStep, then the frame's time. Windows port: the flags and xmm0-15 are kept too (FrameStep
/// is host code; the game's code around the site is left as it found it).
bool HookStep() {
    constexpr u8 code[] = {0xc7, 0x45, 0xc8, 0x89, 0x88, 0x08, 0x3d};
    Emitter e;
    e.Bytes({0x9c}); // pushfq
    e.SaveVolatile();
    e.SaveXmm();
    e.Bytes({0x48, 0x89, 0xdf}); // mov rdi, rbx
    e.CallAbs(reinterpret_cast<const void*>(&FrameStep));
    e.RestoreXmm();
    e.RestoreVolatile();
    e.Bytes({0x9d}); // popfq
    e.StoreToFrame(&live->dt, -0x38);
    e.JmpAbs(image_base + 0x2018e36 + sizeof(code));
    return Detour(0x2018e36, code, sizeof(code), e);
}

/// A subsystem stepped by a fixed 1/30 s (`mov dword [rbp + offset], 1/30` at `site`): by the
/// frame's time.
bool HookFixedStep(u64 site, std::int8_t offset) {
    const u8 code[] = {0xc7, 0x45, u8(offset), 0x89, 0x88, 0x08, 0x3d};
    Emitter e;
    e.StoreToFrame(&live->dt, offset);
    e.JmpAbs(image_base + site + sizeof(code));
    return Detour(site, code, sizeof(code), e);
}

/// The cloth update (0x76E370, Havok: it splits the frame's time into steps) saves the time it was
/// given at 0x76E384 (`vmovss [rbp - 0x78], xmm0`). Above 30 FPS a stutter (a 15-35 ms frame, then
/// 4 ms frames catching up) kicked the cloth even standing still; the time becomes the steady time
/// (Live::steady: the frame's, 5% a frame at most, never above 1/30 s) by scaling it with
/// steady / dt, so a time scale the game gave it stays. Supermedo/bloodborne_pc 7a39487f
/// (cloth_time.py); its code lived in the eboot's last page (0d38fc9b: it overlapped PC Settings'
/// page table and crashed save loading); this one is in the stub page, written at startup.
bool HookClothTime() {
    constexpr u64 site = 0x76e384;
    constexpr u8 code[] = {0xc5, 0xfa, 0x11, 0x45, 0x88}; // vmovss [rbp - 0x78], xmm0
    Emitter e;
    e.Bytes({0x50, 0x48, 0xb8});                       // push rax; movabs rax,
    e.U64(reinterpret_cast<u64>(&live->cloth_ratio));  //   &cloth_ratio (cloth_max follows)
    e.Bytes({0xc5, 0xfa, 0x11, 0x40, 0x08});           // vmovss [rax + 8], xmm0 (cloth_in: diagnostics)
    e.Bytes({0xc5, 0xfa, 0x59, 0x00});                 // vmulss xmm0, xmm0, [rax]
    e.Bytes({0xc5, 0xfa, 0x5d, 0x40, 0x04});           // vminss xmm0, xmm0, [rax + 4]
    e.Bytes({0xc5, 0xfa, 0x11, 0x40, 0x0c});           // vmovss [rax + 12], xmm0 (cloth_out)
    e.Bytes({0x58});                                   // pop rax
    for (const u8 b : code) e.Bytes({b});              // the site's own instruction
    e.JmpAbs(image_base + site + sizeof(code));
    return Detour(site, code, sizeof(code), e);
}

/// Havok's stiffness factor (0x1167a80: `push rbp; mov rbp, rsp; push r15`) by ClothStiffnessFactor
/// (its arguments as they are, a tail call). Off with BB_CLOTH_STIFFNESS=0.
bool HookClothStiffness() {
    constexpr u64 site = 0x1167a80;
    constexpr u8 code[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    const char* v = std::getenv("BB_CLOTH_STIFFNESS");
    if (v && *v && std::strtof(v, nullptr) == 0.0f) {
        std::printf("Frame rate: cloth stiffness factor off (BB_CLOTH_STIFFNESS=0)\n");
        return false;
    }
    if (std::memcmp(reinterpret_cast<const void*>(image_base + site), code, sizeof(code)) != 0) {
        std::printf("Frame rate: %#llx is not the expected code: cloth stiffness factor not installed\n",
                    (unsigned long long)site);
        return false;
    }
    std::memcpy(&cloth_exponent, reinterpret_cast<const void*>(image_base + 0x48d7bbc), sizeof(float));
    if (!(cloth_exponent < 0.0f && cloth_exponent > -4.0f)) {
        std::printf("Frame rate: cloth exponent %.3f unexpected: cloth stiffness factor not installed\n",
                    double(cloth_exponent));
        return false;
    }
    Emitter e;
    e.JmpAbs(reinterpret_cast<u64>(&ClothStiffnessFactor));
    return Detour(site, code, sizeof(code), e);
}

/// The run's wall check (0x1514c24: the distance moved this frame x 30 = a speed at 30 FPS): the
/// distance divided by the frame's time ([rbp - 0x74], the character update's), a speed at any
/// frame rate. Above ~90 FPS the per-frame distance fell under the check's threshold and the run
/// dropped to half speed.
/// 1: done by the hook, 2: the community's Sprint Fix patch already did it, 0: other code.
int HookRunSpeed() {
    constexpr u64 site = 0x1514c24;
    const u8* code = reinterpret_cast<const u8*>(image_base + site);
    constexpr u8 patched[] = {0xc5, 0xfa, 0x5e, 0x45, 0x8c, 0x0f, 0x1f, 0x00}; // vdivss; nop
    if (std::memcmp(code, patched, sizeof(patched)) == 0) {
        return 2;
    }
    std::int32_t disp = 0;
    std::memcpy(&disp, code + 4, 4);
    if (code[0] != 0xc5 || code[1] != 0xfa || code[2] != 0x59 || code[3] != 0x05 ||
        image_base + site + 8 + std::int64_t(disp) != image_base + 0x492632c) {
        return 0;
    }
    u8 expected[8];
    std::memcpy(expected, code, 8);
    Emitter e;
    e.Bytes({0xc5, 0xfa, 0x5e, 0x45, 0x8c}); // vdivss xmm0, xmm0, [rbp - 0x74]
    e.JmpAbs(image_base + site + 8);
    return Detour(site, expected, 8, e) ? 1 : 0;
}

// ---- High-frame-rate fixes from bbhost (droogie/bbhost perf/next 6f735f4, b2818b4; GPL-3.0-or-later;
// their addresses minus 0x400000). Above 30 FPS in both modes: the hooks and the FPS++ patches. ----

/// Where the effects' step comes from (HookSfxStep).
enum class SfxSource { Hooks, Constant, Measured };
SfxSource sfx_source = SfxSource::Hooks;
float* sfx_step = nullptr; ///< in the near data page: what SprjSfx::Update's load reads
double sfx_carry = 0.0;
std::atomic<double> sfx_seconds{0.0}; ///< the effects' steps so far (diagnostics: BB_FRAME_RATE_LOG)
constexpr u64 SfxStepConstant = 0x4929170; // the game's 1/30 (the 60/90 FPS++ patches: 1/60, 1/90)
constexpr u64 FlipperSlot = 0x55404f8;     // SprjFlipper* (its +0x264: the measured frame time)

// The Bloodletter's transformed L2 gives its wielder a frenzy aura through one attack that hits
// only them: AtkParam_Pc 2900931, a 0.4 m sphere live for one 30 FPS frame of the slam. Posed with
// the hit bodies it overlaps the right hand by 5 cm; when those trail the pose by even 1/120 s it
// misses, and above 30 the aura never came. At 0.6 m (bbhost 6f735f4) it overlaps by 25 cm.
constexpr std::uint32_t BloodletterSelfHit = 2900931;
constexpr std::uint32_t SelfHitRadiusWas = 0x3ecccccd, SelfHitRadiusNow = 0x3f19999a; // 0.4, 0.6
int param_countdown = 0;
bool param_logged = false;

/// The Bloodletter's self-hit radius, written over the game's own 0.4 whenever the table is (re)loaded
/// (checked every 2 s or so, main thread).
void CheckBloodletter() {
    const u64 row = coop::GameParamRow("AtkParam_Pc", "ATK_PARAM_ST", BloodletterSelfHit);
    std::uint32_t radius = 0;
    if (!row || !coop::SafeRead(row, &radius, 4)) return; // hit0_Radius: ATK_PARAM_ST's first field
    if (radius == SelfHitRadiusWas && coop::SafeWrite(row, &SelfHitRadiusNow, 4)) {
        std::printf("Frame rate: AtkParam_Pc %u hit0_Radius 0.4 -> 0.6 m (the Bloodletter's L2 self-hit above "
                    "30 FPS, bbhost)\n",
                    BloodletterSelfHit);
        std::fflush(stdout);
    } else if (radius != SelfHitRadiusNow && !param_logged) {
        param_logged = true;
        float f;
        std::memcpy(&f, &radius, 4);
        std::printf("Frame rate: AtkParam_Pc %u hit0_Radius is %g, not the game's 0.4: left as it is\n",
                    BloodletterSelfHit, double(f));
        std::fflush(stdout);
    }
}

/// SprjSfx::Update, once a frame on the main thread, before it steps the effects: their step.
void BB_FR_SYSV SfxStepNow() {
    float step = Frame30;
    if (sfx_source == SfxSource::Hooks) {
        step = live->dt;
    } else if (sfx_source == SfxSource::Constant) {
        step = *reinterpret_cast<const float*>(image_base + SfxStepConstant);
    } else {
        u64 flipper = 0;
        std::memcpy(&flipper, reinterpret_cast<const void*>(image_base + FlipperSlot), 8);
        float measured = Frame30;
        if (flipper) coop::SafeRead(flipper + 0x264, &measured, 4);
        step = measured;
    }
    *sfx_step = SfxStep(sfx_carry, step);
    sfx_seconds.store(sfx_seconds.load(std::memory_order_relaxed) + *sfx_step, std::memory_order_relaxed);
    if (--param_countdown <= 0) {
        param_countdown = 120;
        CheckBloodletter();
    }
}

/// SprjSfx::Update (0x1f77bd0) hands the effect manager's update the one 1/30 it loads at 0x1f77d4a
/// (0 for a still frame: the jne at 0x1f77d48 skips it). That load gets our step instead. The
/// Uncap FPS++ patch turns the jne into a jump to its own load of the measured frame time: put back.
bool HookSfxStep(bool hooks) {
    constexpr u64 site = 0x1f77d4a;
    u8* code = reinterpret_cast<u8*>(image_base + site);
    constexpr u8 load[] = {0xc5, 0xfa, 0x10, 0x05, 0x1e, 0x14, 0x9b, 0x02}; // vmovss xmm0, [0x4929170]
    constexpr u8 jne[] = {0x75, 0x08}, uncap_jump[] = {0xeb, 0xa2};
    if (std::memcmp(code, load, sizeof(load)) != 0) return false;
    if (hooks) {
        if (std::memcmp(code - 2, jne, 2) != 0) return false;
        sfx_source = SfxSource::Hooks;
    } else if (std::memcmp(code - 2, jne, 2) == 0) {
        sfx_source = SfxSource::Constant;
    } else if (std::memcmp(code - 2, uncap_jump, 2) == 0) {
        sfx_source = SfxSource::Measured;
    } else {
        return false;
    }
    Emitter e;
    e.SaveVolatile();
    e.SaveXmm();
    e.CallAbs(reinterpret_cast<const void*>(&SfxStepNow));
    e.RestoreXmm();
    e.RestoreVolatile();
    e.Bytes({0x50, 0x48, 0xb8}); // push rax; movabs rax, &sfx_step (rax: the update to call)
    e.U64(reinterpret_cast<u64>(sfx_step));
    e.Bytes({0xc5, 0xfa, 0x10, 0x00, 0x58}); // vmovss xmm0, [rax]; pop rax
    e.JmpAbs(image_base + site + sizeof(load));
    if (!Detour(site, load, sizeof(load), e)) return false;
    return sfx_source != SfxSource::Measured || WriteCode(code - 2, jne, 2);
}

/// The grass wind (0x2313870 builds the block the grass vertex shader reads) uses its one step twice:
/// as the wave time (t += step) and as the force (V * step) the shader bends the grass by. With the
/// frame's time in both the grass bent by each frame's length (22% of the authored bend at 134 FPS,
/// shaking from frame to frame); with the patches' fixed 1/60, by half. bbhost b2818b4: the force
/// takes the authored 1/30 (0x4926364) and only the time the step ([rbp - 0x170]).
bool GrassWind() {
    // vmovdqa xmm14, [rbp - 0x170] -> vmovss xmm14, [0x4926364]
    constexpr u64 force_site = 0x23149ea;
    constexpr u8 force_was[] = {0xc5, 0x79, 0x6f, 0xb5, 0x90, 0xfe, 0xff, 0xff};
    constexpr u8 force_now[] = {0xc5, 0x7a, 0x10, 0x35, 0x72, 0x19, 0x61, 0x02};
    // lea rax, [0x5540df9]; cmp byte [rax], 0; vxorps xmm10, xmm10, xmm10; mov r8d, [rbp - 0x1b8];
    // jne +5; vmovdqa xmm10, xmm14 -> mov r8d, [rbp - 0x1b8]; vxorps xmm10, xmm10, xmm10;
    // cmp byte [0x5540df9], 0; jne +8; vmovdqa xmm10, [rbp - 0x170] (rax is written before it is read)
    constexpr u64 time_site = 0x2314b26;
    constexpr u8 time_was[] = {0x48, 0x8d, 0x05, 0xcc, 0xc2, 0x22, 0x03, 0x80, 0x38, 0x00, 0xc4, 0x41, 0x28, 0x57, 0xd2,
                               0x44, 0x8b, 0x85, 0x48, 0xfe, 0xff, 0xff, 0x75, 0x05, 0xc4, 0x41, 0x79, 0x6f, 0xd6};
    constexpr u8 time_now[] = {0x44, 0x8b, 0x85, 0x48, 0xfe, 0xff, 0xff, 0xc4, 0x41, 0x28, 0x57, 0xd2, 0x80, 0x3d, 0xc0,
                               0xc2, 0x22, 0x03, 0x00, 0x75, 0x08, 0xc5, 0x79, 0x6f, 0x95, 0x90, 0xfe, 0xff, 0xff};
    static_assert(sizeof(time_was) == sizeof(time_now));
    u8* force = reinterpret_cast<u8*>(image_base + force_site);
    u8* time = reinterpret_cast<u8*>(image_base + time_site);
    float authored = 0;
    std::memcpy(&authored, reinterpret_cast<const void*>(image_base + 0x4926364), 4);
    if (std::memcmp(force, force_was, sizeof(force_was)) != 0 || std::memcmp(time, time_was, sizeof(time_was)) != 0 ||
        std::fabs(authored - Frame30) > 1e-6f) {
        return false;
    }
    return WriteCode(time, time_now, sizeof(time_now)) && WriteCode(force, force_now, sizeof(force_now));
}

/// The bbhost fixes (both modes); `hooks`: the frame rate is the hooks'. Needs the stub page.
void HighRateFixes(bool hooks) {
    const bool sfx = sfx_step && HookSfxStep(hooks);
    const bool grass = GrassWind();
    std::printf("Frame rate: high-FPS fixes (bbhost): effects step in whole 1/60 s %s, grass wind force at "
                "1/30 %s, the Bloodletter's L2 self-hit 0.6 m when its table loads\n",
                !sfx ? "NOT installed (other code at 0x1f77d4a)"
                : sfx_source == SfxSource::Hooks    ? "(from the frame's time)"
                : sfx_source == SfxSource::Constant ? "(from the patch's fixed step)"
                                                    : "(from the measured frame time, Uncap FPS++)",
                grass ? "on" : "NOT installed (other code or constant)");
}

/// A mapping within rel32 reach of the image (the redirected operands are RIP-relative).
u8* MapNear(std::size_t size) {
#ifdef _WIN32
    // The image is the first block of the runtime's low guest area: the next low block lies right
    // after it (as coop_hooks.cpp / bbport_stability.cpp do).
    if (void* p = runtime_low_map(size, 3 /* read | write */)) {
        const u64 at = reinterpret_cast<u64>(p);
        if (at >= image_base && at + size <= image_base + (2ull << 30) - (1ull << 20)) {
            return static_cast<u8*>(p);
        }
    }
    for (u64 k = 1; k <= 64; ++k) {
        for (const u64 hint : {image_base - k * (16ull << 20), image_base + image_size + k * (16ull << 20)}) {
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(hint & ~u64(0xffff)), size,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
                return static_cast<u8*>(p);
            }
        }
    }
#else
    for (u64 k = 1; k <= 64; ++k) {
        const u64 hint = image_base - k * (8ull << 20);
        void* p = mmap(reinterpret_cast<void*>(hint), size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED) return static_cast<u8*>(p);
    }
#endif
    return nullptr;
}

/// BB_FRAME_RATE_LOG=N (diagnostics, seconds): every N s of real time a line with the steps, the
/// game time they stepped and the real time (the frame-rate measurement, tools/mp/fps_sweep.py).
void LogThread(double period) {
    // Started lazily from FrameStep's first call would add work to the main thread: a plain
    // detached thread instead.
    std::thread([period] {
        const auto t0 = std::chrono::steady_clock::now();
        double last_game = 0, last_real = 0, last_sfx = 0;
        u64 last_steps = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::duration<double>(period));
            const double real = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const double game = game_seconds.load(std::memory_order_relaxed);
            const u64 n = steps.load(std::memory_order_relaxed);
            const double sfx = sfx_seconds.load(std::memory_order_relaxed);
            const double dr = real - last_real, dg = game - last_game, ds = sfx - last_sfx;
            std::printf("Frame rate: %.1f s real, %llu steps (%.1f per s), game time %.3f s per real s, "
                        "last dt %.5f s; effects time %.3f s per real s\n",
                        real, (unsigned long long)(n - last_steps), dr > 0 ? double(n - last_steps) / dr : 0.0,
                        dr > 0 ? dg / dr : 0.0, Unlocked() ? double(live->dt) : 0.0, dr > 0 ? ds / dr : 0.0);
            std::fflush(stdout);
            last_game = game, last_real = real, last_steps = n, last_sfx = sfx;
        }
    }).detach();
}
// ---- Live clock (Mrsuss60/bloodborne_pc_windows_port a7cbfae6, 83a168ee; GPL-2.0-or-later) ----
// The "60 FPS++ (Deltatime)" patch (patches/Bloodborne.xml, applied by scripts/patches.py) leaves the
// main loop's step block at 0x2018e3d in a known form and makes the cloth, animation and platform
// code read the pacer's measured frame time ([pacer + 0x264]) and a guest-global step (image +
// 0x535A908). LiveClockTick replaces that block: it sets the pacer's frame time target and the
// measured time from the target rate (LiveClockStep: the frame's time, clamped) and returns the step.
// Upstream installed it from probe.c with a 25-byte stub; here it is a detour with the registers
// kept, like HookStep. Live mode and the hooks above never run together (Install picks one).
std::atomic<bool> live_requested{false};
std::atomic<bool> live_supported{false};
LiveClockState live_state;
bool live_deltatime_cave = false;
bool live_pacer_ok = false;
std::atomic<int> live_logged_target{-1};

/// The block at 0x2018e3d as the patch leaves it (46 bytes up to 0x2018e6b): movss xmm0,[rbx+0x264];
/// maxss xmm0,[rbx+0x18]; minss xmm0,[1/30]; movss [rbp-0x38],xmm0; movss [0x535A908],xmm0; lea rax;
/// 5 nops. Anything else (another eboot, a patch of the same code): not installed.
constexpr u8 LiveBlock[46] = {0xf3, 0x0f, 0x10, 0x83, 0x64, 0x02, 0x00, 0x00, 0xf3, 0x0f, 0x5f, 0x43, 0x18, 0xf3, 0x0f, 0x5d,
                              0x05, 0x12, 0xd5, 0x90, 0x02, 0xf3, 0x0f, 0x11, 0x45, 0xc8, 0xf3, 0x0f, 0x11, 0x05, 0xa9, 0x1a,
                              0x34, 0x03, 0x48, 0x8d, 0x05, 0xaa, 0x76, 0x52, 0x03, 0x90, 0x90, 0x90, 0x90, 0x90};
constexpr u8 LiveResume[5] = {0x48, 0x8d, 0x75, 0xc0, 0xe8}; // 0x2018e6b: lea rsi,[rbp-0x40]; call
constexpr u64 LiveBlockSite = 0x2018e3d, LiveGlobalDt = 0x535a908, LiveCave = 0x5090000;

/// BB_LIVE_FPS_LOG=1 (diagnostics, off by default): once a second of real time one line with the clock's state, the
/// pacer's mode fields and the presenter's limit (issue #35: Live FPS stuck near 64 FPS after a respawn load).
#ifdef _WIN32
/// Once a second under BB_LIVE_FPS_LOG: the CPU time every thread of the process used in the last `secs` seconds
/// (user + kernel, ms per real second), the busy ones only, with the thread's name and priority; the process affinity
/// and thread count once. Issue #35: which thread is the extra ~6 ms a frame in a stuck stay.
void ThreadCpuLog(double secs) {
    struct Prev { std::uint64_t ticks; };
    static std::unordered_map<DWORD, Prev> prev;
    static bool first = true;
    if (first) {
        first = false;
        DWORD_PTR pm = 0, sm = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
        std::printf("Live clock threads: process affinity %#llx of %#llx, priority class %#lx\n", (unsigned long long)pm,
                    (unsigned long long)sm, (unsigned long)GetPriorityClass(GetCurrentProcess()));
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    const DWORD pid = GetCurrentProcessId();
    struct Row { double ms; DWORD tid; int prio; std::string name; };
    std::vector<Row> rows;
    int total = 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        ++total;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        FILETIME c, e, k, u;
        if (GetThreadTimes(h, &c, &e, &k, &u)) {
            auto to_u64 = [](const FILETIME& f) { return (std::uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
            const std::uint64_t ticks = to_u64(k) + to_u64(u); // 100 ns
            auto it = prev.find(te.th32ThreadID);
            const double ms = it == prev.end() ? 0.0 : double(ticks - it->second.ticks) / 10000.0 / secs;
            prev[te.th32ThreadID] = {ticks};
            if (ms >= 20.0) {
                std::string name = "?";
                PWSTR w = nullptr;
                if (SUCCEEDED(GetThreadDescription(h, &w)) && w) {
                    name.clear();
                    for (const wchar_t* p = w; *p && name.size() < 40; ++p) name += *p < 128 ? char(*p) : '?';
                    LocalFree(w);
                }
                rows.push_back({ms, te.th32ThreadID, GetThreadPriority(h), name});
            }
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ms > b.ms; });
    std::printf("Live clock threads: %d threads; busy (ms CPU per s):", total);
    for (std::size_t i = 0; i < rows.size() && i < 14; ++i) {
        std::printf(" [%s %lu p%d %.0f]", rows[i].name.c_str(), (unsigned long)rows[i].tid, rows[i].prio, rows[i].ms);
    }
    std::printf("\n");
}
#endif

void LiveDiag(const u8* pacer, float measured, int target, float interval, float dt) {
    static const bool on = [] {
        const char* v = std::getenv("BB_LIVE_FPS_LOG");
        return v && *v && std::atoi(v) > 0;
    }();
    if (!on) return;
    using clock = std::chrono::steady_clock;
    static clock::time_point last = clock::now();
    static u64 frames = 0, last_frames = 0;
    ++frames;
    const auto now = clock::now();
    const double secs = std::chrono::duration<double>(now - last).count();
    if (secs < 1.0) return;
#ifdef _WIN32
    ThreadCpuLog(secs);
#endif
    BbStability::LogCpuSave();
    std::int32_t vblanks = 0;
    std::uint8_t flip_skip = 0, no_sleep = 0;
    float pacer_target = 0;
    std::memcpy(&vblanks, pacer + 0x10, 4);
    std::memcpy(&flip_skip, pacer + 0x14, 1);
    std::memcpy(&pacer_target, pacer + 0x18, 4);
    std::memcpy(&no_sleep, pacer + 0x273, 1);
    // The system timer resolution (100 ns units; 10000 = 1 ms, 156250 = the 15.6 ms default) and what a 1 ms sleep
    // really takes: a 64 FPS lock is the default tick, whoever gave the millisecond back.
    double timer_ms = 0, sleep_ms = 0;
#ifdef _WIN32
    using NtQueryFn = long(__stdcall*)(unsigned long*, unsigned long*, unsigned long*);
    static const auto query = reinterpret_cast<NtQueryFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryTimerResolution")));
    unsigned long lo = 0, hi = 0, cur = 0;
    if (query && query(&lo, &hi, &cur) == 0) timer_ms = double(cur) / 10000.0;
    const auto t0 = clock::now();
    Sleep(1);
    sleep_ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
#endif
    // bbhost's "Enable Task Stealing" byte (its 0x2695d04 is a guest address: image + 0x2295d04), set by the Performance
    // Patch, which Live FPS locks out: logged once.
    static bool task_logged = false;
    if (!task_logged && image_base && image_size > 0x2295d05) {
        task_logged = true;
        std::printf("Live clock diag: task stealing byte (image+0x2295d04) = %u\n",
                    unsigned(*reinterpret_cast<const u8*>(image_base + 0x2295d04)));
    }
    // BB_LIVE_FPS_TEST_TOGGLE=1 (test aid): the reporter's remedy done by the game itself. After 5 s in a 55-75 FPS
    // state the target goes to 120 for 3 s and back, and the line shows what the rate did.
    {
        static const bool toggle = [] { const char* v = std::getenv("BB_LIVE_FPS_TEST_TOGGLE"); return v && *v == '1'; }();
        static int slow = 0, phase = 0, hold = 0;
        static int saved = 0;
        const double f = double(frames - last_frames) / secs;
        if (toggle) {
            if (phase == 0) {
                const auto gs = coop::ReadGameState(false);
                const bool in_world = gs.world_up && !gs.loading && gs.in_game_frame;
                slow = (in_world && f >= 55.0 && f <= 75.0 && measured < 0.05f) ? slow + 1 : 0;
                if (slow >= 5) {
                    saved = BbSettings::Get().target_fps.load();
                    BbSettings::Get().target_fps = 120;
                    std::printf("Live clock diag: TOGGLE test: stuck at %.1f FPS, target %d -> 120\n", f, saved);
                    phase = 1, hold = 0;
                }
            } else if (phase == 1 && ++hold >= 3) {
                BbSettings::Get().target_fps = saved;
                std::printf("Live clock diag: TOGGLE test: target back to %d\n", saved);
                phase = 2, hold = 0;
            } else if (phase == 2 && ++hold >= 4) {
                std::printf("Live clock diag: TOGGLE test: 4 s after the toggle %.1f FPS\n", f);
                phase = 3;
            }
        }
    }
    std::printf("Live clock diag: timer %.2f ms, Sleep(1) %.2f ms; ", timer_ms, sleep_ms);
    std::printf("%.1f frames/s, target %d (setting %d), interval %.5f, measured %.5f, step %.5f, "
                "carry %.5f, pacer vblanks %d skip %u target %.5f no_sleep %u, presenter limit %u\n",
                double(frames - last_frames) / secs, target, BbSettings::Get().target_fps.load(), double(interval),
                double(measured), double(dt), double(live_state.smoothed), vblanks, unsigned(flip_skip),
                double(pacer_target), unsigned(no_sleep), LivePresenterLimit());
    std::fflush(stdout);
    last = now;
    last_frames = frames;
}

/// The main loop's step (rdi: the pacer; rsi: where the step goes, [rbp - 0x38]).
void BB_FR_SYSV LiveClockTick(u8* pacer, float* out) {
    const float measured = *reinterpret_cast<const float*>(pacer + 0x264);
    auto& s = BbSettings::Get();
    // vrr: a target at or above the display's refresh runs just under it (0 = uncapped goes to the cap too).
    const int target = int(ApplyVrrCap(unsigned(std::max(0, s.target_fps.load(std::memory_order_relaxed)))));
    float interval = Frame30;
    const float dt = LiveClockStep(live_state, measured, target, s.max_dt_hz.load(std::memory_order_relaxed), interval);
    *reinterpret_cast<float*>(pacer + 0x18) = interval;
    live->shortest = interval; // the pacer-mode hook (HookPacerMode) reads it: the pacer waits for the target
    // All of the patch's code caves sample the pacer's measured time: they get the filtered one.
    *reinterpret_cast<float*>(pacer + 0x264) = dt;
    if (live_deltatime_cave) *reinterpret_cast<float*>(image_base + LiveGlobalDt) = dt;
    *out = dt;
    live->dt = dt;
    live->steady = dt;
    game_seconds.store(game_seconds.load(std::memory_order_relaxed) + dt, std::memory_order_relaxed);
    steps.fetch_add(1, std::memory_order_relaxed);
    LiveDiag(pacer, measured, target, interval, dt);
    if (live_logged_target.exchange(target, std::memory_order_relaxed) != target) {
        std::printf("Live clock: target %d FPS%s, step %.5f s (max 1/%d s)\n", target > 0 ? target : LiveCeiling,
                    target > 0 ? "" : " (uncapped)", double(dt), s.max_dt_hz.load(std::memory_order_relaxed));
    }
}

/// Live mode: the eboot checked, the step block replaced, the pacer's four frame time constants
/// (1/60 s in the patch's mode setup, 0x2034845 ...) lowered to 1/240 s so 90 / 120 / uncapped can
/// exceed 60 FPS. True when the live clock runs; false leaves the patch's static 60 FPS.
bool InstallLive() {
    if (image_size <= 0x2035000 ||
        std::memcmp(reinterpret_cast<const u8*>(image_base) + LiveBlockSite, LiveBlock, sizeof(LiveBlock)) != 0 ||
        std::memcmp(reinterpret_cast<const u8*>(image_base) + 0x2018e6b, LiveResume, sizeof(LiveResume)) != 0) {
        std::printf("Live clock: incompatible eboot signature (the \"60 FPS++ (Deltatime)\" patch is not applied or "
                    "the code differs): static 60 FPS\n");
        return false;
    }
    Emitter e;
    e.Bytes({0x9c}); // pushfq
    e.SaveVolatile();
    e.SaveXmm();
    e.Bytes({0x48, 0x89, 0xdf});       // mov rdi, rbx (the pacer)
    e.Bytes({0x48, 0x8d, 0x75, 0xc8}); // lea rsi, [rbp - 0x38]
    e.CallAbs(reinterpret_cast<const void*>(&LiveClockTick));
    e.RestoreXmm();
    e.RestoreVolatile();
    e.Bytes({0x9d}); // popfq
    e.JmpAbs(image_base + 0x2018e6b);
    if (!Detour(LiveBlockSite, LiveBlock, sizeof(LiveBlock), e)) {
        std::printf("Live clock: the step block could not be replaced: static 60 FPS\n");
        return false;
    }
    // The game's own pacer must wait for the target: the patch's mode setups ask for 1/240 s (measured: 107 steps a
    // second at a target of 60 without it), so the pacer mode hook of the hooks mode (one vblank, no flip skipping,
    // the frame time target from live->shortest, which the clock sets every frame) is installed here too.
    live_pacer_ok = HookPacerMode() && HookPacerLate();
    if (!live_pacer_ok) {
        std::printf("Live clock: the pacer mode hook is not installed: the presenter holds the target instead\n");
    }
    // The pacer's frame time target in its four mode setups: mov dword [r12 + 0x18], 1/60 (the patch).
    constexpr std::uint32_t Was = 0x3c888889, Now = 0x3b888889; // 1/60 s, 1/240 s
    const u64 sites[] = {0x2034845, 0x203485f, 0x2034883, 0x20348b8};
    int lowered = 0;
    for (const u64 site : sites) {
        std::uint32_t value = 0;
        std::memcpy(&value, reinterpret_cast<const void*>(image_base + site), 4);
        if (value == Now || (value == Was && WriteCode(reinterpret_cast<u8*>(image_base + site), &Now, 4))) ++lowered;
    }
    static const u8 cave[7] = {0x48, 0x8b, 0x05, 0xf1, 0x04, 0x4b, 0x00};
    live_deltatime_cave = image_size > LiveCave + 7 &&
                          std::memcmp(reinterpret_cast<const u8*>(image_base) + LiveCave, cave, sizeof(cave)) == 0;
    std::printf("Live clock: hook installed at image+%#llx, pacer ceiling 240 FPS (%d of 4 constants), deltatime cave %s, "
                "target %d FPS\n",
                (unsigned long long)LiveBlockSite, lowered, live_deltatime_cave ? "yes" : "NO",
                BbSettings::Get().target_fps.load());
    return true;
}
} // namespace

namespace {
void Install(unsigned char* image, std::uint64_t size) {
    const char* fps = std::getenv("BB_FPS");
    if (!HighRate(fps, std::getenv("BB_FPS_PATCH"))) return; // the game's own 30 FPS
    image_base = reinterpret_cast<u64>(image);
    image_size = size;
    u8* data = MapNear(4096);
    stub_page = MapNear(StubPageSize);
    if (!data || !stub_page) {
        std::printf("Frame rate: no memory next to the game's code: no hooks, no high-FPS fixes\n");
        return;
    }
    live = new (data) Live{};
    sfx_step = reinterpret_cast<float*>(data + sizeof(Live)); // a float of its own after Live
    *sfx_step = Frame30;
    if (LiveWanted(fps, std::getenv("BB_FPS_PATCH"), BbSettings::Get().live_fps.load())) {
        live_requested = true;
        // The launcher's target (BB_FPS / BB_FPS_LIMIT) is the starting one; the menu changes it live.
        if (fps && *fps) {
            BbSettings::Get().target_fps = LiveTargetFromRate(TargetRate(fps, std::getenv("BB_FPS_LIMIT")));
        }
        StubWritable(true);
        const bool ok = InstallLive();
        if (ok) {
            // The effects' step: the Deltatime patch redirects SprjSfx::Update's load (0x1f77d4e) to the live
            // global step itself, so the hooks' whole-1/60-s step (HookSfxStep) is not installed in live mode.
            std::printf("Live clock: effects step from the patch's global step (no 1/60 s quantising)\n");
            // The plants' wind force: the Deltatime patch hands the wind code the live step, which the wind uses
            // as the force too (V * step), so every long or short frame bent the plants by its own length (they
            // shook). The force takes the authored 1/30 (bbhost b2818b4), the wave time keeps the live step.
            std::printf("Live clock: grass wind force at the authored 1/30 %s\n",
                        GrassWind() ? "on" : "NOT installed (other code or constant)");
        }
        StubWritable(false);
        if (ok) {
            live_supported = true;
            unlocked = true;
            if (const char* log = std::getenv("BB_FRAME_RATE_LOG"); log && std::atof(log) > 0) {
                LogThread(std::atof(log));
            }
            std::fflush(stdout);
            return;
        }
        // Not this eboot / not the patch's bytes: the usual frame rate (hooks or FPS++ patches) below, so a
        // default-on live clock never leaves a start at 30 FPS. The presenter's limit is the normal one.
        live_requested = false;
        std::printf("Live clock: not installed, falling back to the usual frame rate\n");
    }
    if (!HooksWanted(fps, std::getenv("BB_FPS_PATCH"), std::getenv("BB_FRAME_HOOKS"))) {
        const char* hooks = std::getenv("BB_FRAME_HOOKS");
        std::printf("Frame rate: hooks off (BB_FRAME_HOOKS=%s): the FPS++ patches (BB_FPS=%s)\n",
                    hooks ? hooks : "(unset)", fps);
        StubWritable(true);
        HighRateFixes(false);
        StubWritable(false);
        if (const char* log = std::getenv("BB_FRAME_RATE_LOG"); log && std::atof(log) > 0) {
            LogThread(std::atof(log)); // the effects' time only (no steps of ours)
        }
        std::fflush(stdout);
        return;
    }
    // The mode's rate and BB_FPS_LIMIT (the launcher's frame rate limit): the game's pacer waits for
    // it itself, before the frame's simulation (lower latency than holding finished frames back at
    // the present).
    const int rate = TargetRate(fps, std::getenv("BB_FPS_LIMIT"));
    live->shortest = 1.0f / float(rate);
    StubWritable(true);
    // The core first: without all three the game keeps its own frame rate.
    const bool mode_ok = HookPacerMode();
    const bool late_ok = mode_ok && HookPacerLate();
    const bool step_ok = late_ok && HookStep();
    if (!step_ok) {
        // A partial install would leave the pacer at one vblank with fixed 1/30 s steps (a fast
        // game): HookPacerMode is the first, so a failure after it is reported loudly.
        std::printf("Frame rate: the game's frame pacer is not the expected code (%s): 30 FPS%s\n",
                    !mode_ok ? "0x203493d" : !late_ok ? "0x2034d96" : "0x2018e36",
                    mode_ok ? " (WARNING: partly hooked)" : "");
        StubWritable(false);
        return;
    }
    unlocked = true;
    const int fixed = HookFixedStep(0x17f9b71, -0x48) + HookFixedStep(0x1572a7d, -0x30);
    const int run = HookRunSpeed();
    const bool cloth_time = HookClothTime();
    cloth_hook_on = HookClothStiffness();
    int redirected = 0;
    for (const Redirect& r : Redirects) {
        if (RedirectOperand(r)) {
            ++redirected;
        } else {
            std::printf("Frame rate: %#llx is not the expected code, left as it is\n", (unsigned long long)r.site);
        }
    }
    HighRateFixes(true);
    StubWritable(false);
    std::printf("Frame rate: steady time step (cloth %s, plants via the redirects; BB_STEADY_TIME=0 turns it off)\n",
                cloth_time ? "on" : "NOT installed");
    std::printf("Frame rate: cloth stiffness factor %s (BB_CLOTH_STIFFNESS=0 turns it off)\n",
                cloth_hook_on ? "on" : "NOT installed");
    std::printf("Frame rate: free (hooks, no patches): the game steps by each frame's time "
                "(1/%d..1/30 s); %d of 2 fixed steps, %d of %zu per-frame values follow the frame's "
                "time, the run's wall check %s\n",
                rate, fixed, redirected, std::size(Redirects),
                run == 1 ? "by speed" : run == 2 ? "by speed (a Sprint Fix patch did it already)"
                                                 : "unchanged (not found)");
    if (const char* log = std::getenv("BB_FRAME_RATE_LOG"); log && std::atof(log) > 0) {
        LogThread(std::atof(log));
    }
    std::fflush(stdout);
}

} // namespace

void PatchImage(unsigned char* image, std::uint64_t size) {
    Install(image, size);
    StartSweep(); // BB_FRAME_SWEEP (diagnostics, bbport_frame_sweep.cpp; with the patches too)
}

unsigned LivePresenterLimit() {
    if (!live_requested.load(std::memory_order_relaxed)) return 0;
    if (!live_supported.load(std::memory_order_relaxed)) return 60; // the patch's static rate
    // The game's pacer holds the target (live_pacer_ok): the presenter only keeps the 240 FPS ceiling, so the two
    // limiters do not beat against each other. Without the pacer hook the presenter holds the target (upstream).
    if (live_pacer_ok) return unsigned(LiveCeiling);
    const int target = BbSettings::Get().target_fps.load(std::memory_order_relaxed);
    return target <= 0 ? unsigned(LiveCeiling) : unsigned(std::clamp(target, 30, LiveCeiling));
}

namespace {
std::atomic<float> display_refresh_hz{0.0f};
}

void SetDisplayRefresh(float hz) {
    display_refresh_hz.store(hz, std::memory_order_relaxed);
}

unsigned VrrCap() {
    if (!BbSettings::Get().startup_vrr) return 0;
    return unsigned(VrrFrameCap(display_refresh_hz.load(std::memory_order_relaxed)));
}

unsigned ApplyVrrCap(unsigned limit) {
    const unsigned cap = VrrCap();
    return cap && (limit == 0 || limit > cap) ? cap : limit;
}

bool LiveActive() {
    return live_supported.load(std::memory_order_relaxed);
}

bool Unlocked() {
    return unlocked.load(std::memory_order_relaxed);
}

double GameSeconds() {
    return game_seconds.load(std::memory_order_relaxed);
}

std::uint64_t Steps() {
    return steps.load(std::memory_order_relaxed);
}

bool SetTargetRate(int rate) {
    if (!Unlocked() || !live) return false;
    if (live_supported.load(std::memory_order_relaxed)) { // live clock: the setting the clock reads
        BbSettings::Get().target_fps = LiveTargetFromRate(std::clamp(rate, 30, MaxRate));
        return true;
    }
    // One aligned float: the pacer stub reads it once a frame.
    reinterpret_cast<std::atomic<float>*>(&live->shortest)->store(1.0f / float(std::clamp(rate, 30, MaxRate)),
                                                                 std::memory_order_relaxed);
    return true;
}

float LastDt() {
    return live && Unlocked() ? live->dt : 0.0f;
}
} // namespace BbFrameRate
