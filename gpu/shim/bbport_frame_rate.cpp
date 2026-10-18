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
#include "bbport_frame_rate.h"
#include "bbport_frame_rate_logic.h"
#include "bbport_guest_code.h"
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
#include <iterator>
#include <new>
#include <thread>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
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
    if (sweep_step_pending.load(std::memory_order_relaxed)) SweepOnStep();
    game_seconds.store(game_seconds.load(std::memory_order_relaxed) + dt, std::memory_order_relaxed);
    steps.fetch_add(1, std::memory_order_relaxed);
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
    {0x2313d8d, 0x4929e70, LIVE(dt)},  // the grass wind's step (0x2313870): its 60 Hz mode path
                                       // (bbhost b2818b4's frame-time site 0x2713d8d)
    {0x2313dad, 0x4929e70, LIVE(dt)},  // a simulation step: 1/60 in the game's 60 Hz mode,
    {0x2313db7, 0x4929e74, LIVE(dt)},  // 1/30 otherwise
    {0x2315d71, 0x4929e74, LIVE(dt)},
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
    if (length < 5 || length > 16 || std::memcmp(code, expected, length) != 0 || e.overflow ||
        stub_used + e.size > StubPageSize) {
        return false;
    }
    u8* stub = stub_page + stub_used;
    const std::int64_t rel = std::int64_t(reinterpret_cast<u64>(stub)) - std::int64_t(image_base + site + 5);
    if (rel != std::int32_t(rel)) return false;
    std::memcpy(stub, e.code.data(), e.size);
    stub_used += (e.size + 15) & ~std::size_t(15);
    const std::int32_t rel32 = std::int32_t(rel);
    u8 jump[16];
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
        double last_game = 0, last_real = 0;
        u64 last_steps = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::duration<double>(period));
            const double real = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const double game = game_seconds.load(std::memory_order_relaxed);
            const u64 n = steps.load(std::memory_order_relaxed);
            const double dr = real - last_real, dg = game - last_game;
            std::printf("Frame rate: %.1f s real, %llu steps (%.1f per s), game time %.3f s per real s, "
                        "last dt %.5f s\n",
                        real, (unsigned long long)(n - last_steps), dr > 0 ? double(n - last_steps) / dr : 0.0,
                        dr > 0 ? dg / dr : 0.0, live ? double(live->dt) : 0.0);
            std::fflush(stdout);
            last_game = game, last_real = real, last_steps = n;
        }
    }).detach();
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
    if (!HooksWanted(fps, std::getenv("BB_FPS_PATCH"), std::getenv("BB_FRAME_HOOKS"))) {
        const char* hooks = std::getenv("BB_FRAME_HOOKS");
        std::printf("Frame rate: hooks off (BB_FRAME_HOOKS=%s): the FPS++ patches (BB_FPS=%s)\n",
                    hooks ? hooks : "(unset)", fps);
        StubWritable(true);
        HighRateFixes(false);
        StubWritable(false);
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
    // One aligned float: the pacer stub reads it once a frame.
    reinterpret_cast<std::atomic<float>*>(&live->shortest)->store(1.0f / float(std::clamp(rate, 30, MaxRate)),
                                                                 std::memory_order_relaxed);
    return true;
}

float LastDt() {
    return live && Unlocked() ? live->dt : 0.0f;
}
} // namespace BbFrameRate
