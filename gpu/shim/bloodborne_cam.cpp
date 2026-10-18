// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne mouse camera by imedved (Nexus Mods), ported to bbport by mcrib884 (#3, bbmouse).

#include "bloodborne_cam.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/logging/log.h"
#include "party/game_state.h"

#ifdef _WIN32
#include <windows.h>
#endif

// The camera hook (Windows; from Supermedo/bloodborne_pc d3a7584c): the game's camera update
// jumps here with the camera in r13, just before it loads the camera's pitch. The stub stamps
// the camera and calls bbcam_apply, which turns it by the mouse motion gathered since the last
// update, then runs the two loads it replaced. Applied here, once per update, every frame gets
// exactly the motion since the one before; written from the mouse thread at any moment, the
// motion was split unevenly between frames (a pan that judders by a pixel). The game's
// registers are kept: the red zone is skipped, flags and the registers the Windows ABI lets
// bbcam_apply change are saved, and all 16 ymm registers in full (the replaced loads are VEX,
// so AVX is there; a host build with -march may use 256-bit registers and vzeroupper).
#ifdef _WIN32
extern "C" {
// Used only from the stub's asm: without `used`, LTO discards them (GCC 16; the release build).
__attribute__((used)) uintptr_t bbcam_return;
__attribute__((used)) void bbcam_apply(uintptr_t camera);
extern char bbcam_stub[];
}
__asm__(".text\n"
        ".p2align 4\n"
        ".globl bbcam_stub\nbbcam_stub:\n"
        "  test %r13, %r13\n  jz 1f\n"
        "  movl $0x3F333333, 0x130(%r13)\n" // 0.7f liveness stamp
        "  lea -128(%rsp), %rsp\n  pushfq\n"
        "  push %rax\n  push %rcx\n  push %rdx\n  push %r8\n  push %r9\n  push %r10\n"
        "  push %r11\n  push %rbx\n"
        "  mov %rsp, %rbx\n  and $-32, %rsp\n  sub $0x220, %rsp\n"
        "  vmovdqu %ymm0, 0x20(%rsp)\n  vmovdqu %ymm1, 0x40(%rsp)\n  vmovdqu %ymm2, 0x60(%rsp)\n"
        "  vmovdqu %ymm3, 0x80(%rsp)\n  vmovdqu %ymm4, 0xa0(%rsp)\n  vmovdqu %ymm5, 0xc0(%rsp)\n"
        "  vmovdqu %ymm6, 0xe0(%rsp)\n  vmovdqu %ymm7, 0x100(%rsp)\n  vmovdqu %ymm8, 0x120(%rsp)\n"
        "  vmovdqu %ymm9, 0x140(%rsp)\n  vmovdqu %ymm10, 0x160(%rsp)\n  vmovdqu %ymm11, 0x180(%rsp)\n"
        "  vmovdqu %ymm12, 0x1a0(%rsp)\n  vmovdqu %ymm13, 0x1c0(%rsp)\n  vmovdqu %ymm14, 0x1e0(%rsp)\n"
        "  vmovdqu %ymm15, 0x200(%rsp)\n"
        "  mov %r13, %rcx\n  call bbcam_apply\n"
        "  vmovdqu 0x20(%rsp), %ymm0\n  vmovdqu 0x40(%rsp), %ymm1\n  vmovdqu 0x60(%rsp), %ymm2\n"
        "  vmovdqu 0x80(%rsp), %ymm3\n  vmovdqu 0xa0(%rsp), %ymm4\n  vmovdqu 0xc0(%rsp), %ymm5\n"
        "  vmovdqu 0xe0(%rsp), %ymm6\n  vmovdqu 0x100(%rsp), %ymm7\n  vmovdqu 0x120(%rsp), %ymm8\n"
        "  vmovdqu 0x140(%rsp), %ymm9\n  vmovdqu 0x160(%rsp), %ymm10\n  vmovdqu 0x180(%rsp), %ymm11\n"
        "  vmovdqu 0x1a0(%rsp), %ymm12\n  vmovdqu 0x1c0(%rsp), %ymm13\n  vmovdqu 0x1e0(%rsp), %ymm14\n"
        "  vmovdqu 0x200(%rsp), %ymm15\n"
        "  mov %rbx, %rsp\n"
        "  pop %rbx\n  pop %r11\n  pop %r10\n  pop %r9\n  pop %r8\n  pop %rdx\n  pop %rcx\n"
        "  pop %rax\n  popfq\n  lea 128(%rsp), %rsp\n"
        "  vmovss 0x140(%r13), %xmm0\n  vmovss 0x150(%r13), %xmm1\n" // the replaced loads
        "1:\n  jmp *bbcam_return(%rip)\n"); // no camera: skipped, as before
#endif

namespace Core {

static constexpr uintptr_t ADDR_CAM_MAIN_ORIG = 0x80143CEAA;
static constexpr uintptr_t ADDR_CAM_NOP1_ORIG = 0x80143C6E8;
static constexpr uintptr_t ADDR_CAM_NOP2_ORIG = 0x80143C984;
static constexpr uintptr_t ADDR_CAM_NOP3_ORIG = 0x80143DDE6;
static constexpr uintptr_t ADDR_CAM_NOP4_ORIG = 0x80143C870;
static constexpr uintptr_t MONOCULAR_BASE_ORIG = 0x80553E8D0;

// The guest image is allocated as one of these direct-memory regions on the
// Windows runtime. Restrict the AOB search to these committed candidates so a
// coincidental instruction sequence in another module cannot be hooked.
static constexpr size_t CAMERA_REGION_SIZE_MIN = 0x5000000;
static constexpr size_t CAMERA_REGION_SIZE_MAX = 0x5200000;

#ifdef _WIN32
static bool CanAccess(uintptr_t addr, size_t len, bool write) {
    if (!addr || !len || addr + len < addr) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        return false;
    }
    const uintptr_t region_start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    if (addr < region_start || addr + len > region_start + mbi.RegionSize) {
        return false;
    }
    if (!write) {
        return true;
    }
    const DWORD protection = mbi.Protect & 0xff;
    return protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
           protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

template <typename T>
static T ReadMem(uintptr_t addr) {
    if (!CanAccess(addr, sizeof(T), false)) {
        return T{};
    }
    return *reinterpret_cast<const volatile T*>(addr);
}

template <typename T>
static bool WriteMem(uintptr_t addr, T val) {
    if (!CanAccess(addr, sizeof(T), true)) {
        return false;
    }
    *reinterpret_cast<volatile T*>(addr) = val;
    return true;
}

// The camera pointer is validated once per motion sample below. Keep the hot
// path itself as cheap as shadPS4's guarded direct reads/writes; querying the
// Windows page table for every float makes high-polling-rate mice backlog.
template <typename T>
static T ReadMemFast(uintptr_t addr) {
    return *reinterpret_cast<const volatile T*>(addr);
}

template <typename T>
static void WriteMemFast(uintptr_t addr, T val) {
    *reinterpret_cast<volatile T*>(addr) = val;
}

static bool SetRW(uintptr_t addr, size_t len, DWORD* old_protect) {
    return VirtualProtect(reinterpret_cast<LPVOID>(addr), len, PAGE_EXECUTE_READWRITE, old_protect) != 0;
}

static void RestoreProtect(uintptr_t addr, size_t len, DWORD old_protect) {
    DWORD tmp;
    VirtualProtect(reinterpret_cast<LPVOID>(addr), len, old_protect, &tmp);
}

static void Flush(uintptr_t addr, size_t len) {
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), len);
}

static void WriteJump(uintptr_t target, uintptr_t dest, size_t len) {
    u8 buf[64] = {0};
    buf[0] = 0xFF;
    buf[1] = 0x25;
    buf[2] = 0x00;
    buf[3] = 0x00;
    buf[4] = 0x00;
    buf[5] = 0x00;
    std::memcpy(buf + 6, &dest, 8);
    for (size_t i = 14; i < len; ++i) {
        buf[i] = 0x90;
    }
    std::memcpy(reinterpret_cast<void*>(target), buf, len);
}
#else
template <typename T>
static T ReadMem(uintptr_t addr) {
    if (!addr) {
        return T{};
    }
    return *reinterpret_cast<const volatile T*>(addr);
}

template <typename T>
static bool WriteMem(uintptr_t addr, T val) {
    if (!addr) {
        return false;
    }
    *reinterpret_cast<volatile T*>(addr) = val;
    return true;
}

template <typename T>
static T ReadMemFast(uintptr_t addr) {
    return *reinterpret_cast<const volatile T*>(addr);
}

template <typename T>
static void WriteMemFast(uintptr_t addr, T val) {
    *reinterpret_cast<volatile T*>(addr) = val;
}
#endif

BloodborneCam& BloodborneCam::Instance() {
    static BloodborneCam instance;
    return instance;
}

BloodborneCam::~BloodborneCam() {
    Shutdown();
}

void BloodborneCam::Shutdown() {
    // The trampoline is deliberately not freed: the guest may still be executing it.
    SetEnabled(false);
}

void BloodborneCam::Configure(bool direct, float sensitivity) {
    if (!(sensitivity > 0.0f) || sensitivity > 20.0f) {
        sensitivity = 1.0f;
    }
    m_scale.store(sensitivity, std::memory_order_relaxed);
    m_configured.store(direct, std::memory_order_relaxed);
}

bool BloodborneCam::InitializeAddresses() {
    if (m_addressesResolved) {
        return true;
    }
    if (m_addressSearchFailed) {
        return false; // scanned once already; the result will not change
    }

#ifdef _WIN32
    static constexpr size_t CAM_PATTERN_LEN = 18;
    const u8 pattern[CAM_PATTERN_LEN] = {
        0xC4, 0xC1, 0x7A, 0x10, 0x85, 0x40, 0x01, 0x00, 0x00,
        0xC4, 0xC1, 0x7A, 0x10, 0x8D, 0x50, 0x01, 0x00, 0x00
    };

    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);

    u8* cur = reinterpret_cast<u8*>(sysInfo.lpMinimumApplicationAddress);
    u8* maxAddr = reinterpret_cast<u8*>(sysInfo.lpMaximumApplicationAddress);

    uintptr_t foundCamMain = 0;
    MEMORY_BASIC_INFORMATION mbi;

    while (cur < maxAddr) {
        if (VirtualQuery(cur, &mbi, sizeof(mbi)) == 0) {
            break;
        }

        // This mirrors the Cheat Engine table: the size filter applies to the
        // committed VirtualQuery region itself, not to an address interval.
        const bool isCameraCandidate = mbi.State == MEM_COMMIT &&
                                       mbi.RegionSize >= CAMERA_REGION_SIZE_MIN &&
                                       mbi.RegionSize <= CAMERA_REGION_SIZE_MAX;

        if (isCameraCandidate && mbi.RegionSize >= CAM_PATTERN_LEN) {
            // ReadProcessMemory gives us the same process-wide AOB scan
            // semantics as aobscanregion, without dereferencing a protected
            // page directly while walking the candidate.
            std::vector<u8> bytes(mbi.RegionSize);
            SIZE_T bytesRead = 0;
            if (ReadProcessMemory(GetCurrentProcess(), mbi.BaseAddress, bytes.data(),
                                  bytes.size(), &bytesRead) &&
                bytesRead == bytes.size()) {
                const u8* pStart = bytes.data();
                const u8* pEnd = pStart + bytesRead - CAM_PATTERN_LEN;
                for (const u8* p = pStart; p <= pEnd; ++p) {
                    if (std::memcmp(p, pattern, CAM_PATTERN_LEN) == 0) {
                        // First verified hit in the first committed candidate wins.
                        foundCamMain = reinterpret_cast<uintptr_t>(mbi.BaseAddress) +
                                       static_cast<size_t>(p - pStart);
                        break;
                    }
                }
            }
        }

        if (foundCamMain != 0) {
            break;
        }

        u8* next = reinterpret_cast<u8*>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= cur) {
            break;
        }
        cur = next;
    }

    if (foundCamMain == 0) {
        m_addressesResolved = false;
        m_addressSearchFailed = true;
        return false;
    }

    m_cameraOffset = static_cast<intptr_t>(foundCamMain) - static_cast<intptr_t>(ADDR_CAM_MAIN_ORIG);
    m_addrCamMain = foundCamMain;
    m_addrCamNop[0] = static_cast<uintptr_t>(static_cast<intptr_t>(ADDR_CAM_NOP1_ORIG) + m_cameraOffset);
    m_addrCamNop[1] = static_cast<uintptr_t>(static_cast<intptr_t>(ADDR_CAM_NOP2_ORIG) + m_cameraOffset);
    m_addrCamNop[2] = static_cast<uintptr_t>(static_cast<intptr_t>(ADDR_CAM_NOP3_ORIG) + m_cameraOffset);
    m_addrCamNop[3] = static_cast<uintptr_t>(static_cast<intptr_t>(ADDR_CAM_NOP4_ORIG) + m_cameraOffset);
    m_monocularBase = static_cast<uintptr_t>(static_cast<intptr_t>(MONOCULAR_BASE_ORIG) + m_cameraOffset);

    m_addressesResolved = true;
    return true;
#else
    return false;
#endif
}

bool BloodborneCam::SetEnabled(bool enabled) {
    if (enabled == m_enabled.load(std::memory_order_relaxed)) {
        return enabled;
    }

#ifdef _WIN32
    static const u8 NOP9[CAM_NOP_LEN] = {
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
    };
    static const u8 kExpectedMain[CAM_MAIN_LEN] = {
        0xC4, 0xC1, 0x7A, 0x10, 0x85, 0x40, 0x01, 0x00, 0x00,
        0xC4, 0xC1, 0x7A, 0x10, 0x8D, 0x50, 0x01, 0x00, 0x00
    };

    DWORD old_protect = 0;

    if (!enabled) {
        // Stop applying motion first, then restore the original instructions.
        m_enabled.store(false, std::memory_order_release);
        m_lockedOn.store(false, std::memory_order_relaxed);
        if (m_addressesResolved) {
            if (SetRW(m_addrCamMain, CAM_MAIN_LEN, &old_protect)) {
                std::memcpy(reinterpret_cast<void*>(m_addrCamMain), m_camMainOrig, CAM_MAIN_LEN);
                RestoreProtect(m_addrCamMain, CAM_MAIN_LEN, old_protect);
                Flush(m_addrCamMain, CAM_MAIN_LEN);
            }
            for (int i = 0; i < 4; ++i) {
                if (SetRW(m_addrCamNop[i], CAM_NOP_LEN, &old_protect)) {
                    std::memcpy(reinterpret_cast<void*>(m_addrCamNop[i]), m_camNopOrig[i], CAM_NOP_LEN);
                    RestoreProtect(m_addrCamNop[i], CAM_NOP_LEN, old_protect);
                    Flush(m_addrCamNop[i], CAM_NOP_LEN);
                }
            }
        }
        LOG_INFO(Core, "Mouse camera: direct mode off");
        return false;
    }

    if (!InitializeAddresses()) {
        LOG_WARNING(Core, "Mouse camera: camera code not found (game 1.09 needed); using the stick mode");
        return false;
    }

    // Only hook code that still looks like the original camera update.
    if (std::memcmp(reinterpret_cast<const void*>(m_addrCamMain), kExpectedMain, CAM_MAIN_LEN) != 0) {
        LOG_WARNING(Core, "Mouse camera: camera code changed; using the stick mode");
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        if (!CanAccess(m_addrCamNop[i], CAM_NOP_LEN, false)) {
            LOG_WARNING(Core, "Mouse camera: camera code not readable; using the stick mode");
            return false;
        }
    }

    std::memcpy(m_camMainOrig, reinterpret_cast<const void*>(m_addrCamMain), CAM_MAIN_LEN);
    for (int i = 0; i < 4; ++i) {
        std::memcpy(m_camNopOrig[i], reinterpret_cast<const void*>(m_addrCamNop[i]), CAM_NOP_LEN);
    }

    bbcam_return = static_cast<uintptr_t>(m_addrCamMain + CAM_MAIN_LEN);
    m_trampoline = reinterpret_cast<void*>(bbcam_stub);
    m_pendingX.store(0.0f, std::memory_order_relaxed);
    m_pendingY.store(0.0f, std::memory_order_relaxed);

    if (!SetRW(m_addrCamMain, CAM_MAIN_LEN, &old_protect)) {
        LOG_ERROR(Core, "Mouse camera: camera code not writable");
        return false;
    }
    WriteJump(m_addrCamMain, reinterpret_cast<uintptr_t>(m_trampoline), CAM_MAIN_LEN);
    RestoreProtect(m_addrCamMain, CAM_MAIN_LEN, old_protect);
    Flush(m_addrCamMain, CAM_MAIN_LEN);

    for (int i = 0; i < 4 && !m_probe; ++i) {
        if (SetRW(m_addrCamNop[i], CAM_NOP_LEN, &old_protect)) {
            std::memcpy(reinterpret_cast<void*>(m_addrCamNop[i]), NOP9, CAM_NOP_LEN);
            RestoreProtect(m_addrCamNop[i], CAM_NOP_LEN, old_protect);
            Flush(m_addrCamNop[i], CAM_NOP_LEN);
        }
    }

    m_lockedOn.store(false, std::memory_order_relaxed);
    if (m_probe) {
        m_probeHooked = true;
        LOG_INFO(Core, "Mouse camera: probe on (the stick turns the camera)");
        return false;
    }
    m_enabled.store(true, std::memory_order_release);
    LOG_INFO(Core, "Mouse camera: direct mode on");
    return true;
#else
    (void)enabled;
    return false;
#endif
}

bool BloodborneCam::StartProbe() {
    if (m_probeHooked) {
        return true;
    }
    if (m_enabled.load(std::memory_order_relaxed)) {
        return false;
    }
    m_probe = true;
    SetEnabled(true);
    return m_probeHooked;
}

bool BloodborneCam::ReadAngles(float& yaw, float& pitch) const {
    if (!m_probeHooked || !m_trampoline) {
        return false;
    }
    const uintptr_t cam = m_camera.load(std::memory_order_acquire);
#ifdef _WIN32
    if (!cam || !CanAccess(cam + 0x130, 0x144, false)) {
        return false;
    }
#else
    if (!cam) {
        return false;
    }
#endif
    pitch = ReadMemFast<float>(cam + 0x140);
    yaw = ReadMemFast<float>(cam + 0x144);
    return true;
}

static int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static void AddFloat(std::atomic<float>& value, float add) {
    float old = value.load(std::memory_order_relaxed);
    while (!value.compare_exchange_weak(old, old + add, std::memory_order_relaxed)) {
    }
}

void BloodborneCam::SetApplyInGame(bool in_game) {
    m_applyInGame.store(in_game, std::memory_order_relaxed);
}

bool BloodborneCam::ApplyInGame() const {
    return m_applyInGame.load(std::memory_order_relaxed);
}

bool BloodborneCam::CameraLive() const {
    return NowMs() - m_lastApplyMs.load(std::memory_order_relaxed) <= 100;
}

void BloodborneCam::OnMouseMotion(float xrel, float yrel) {
    if (!m_enabled.load(std::memory_order_acquire) || !m_trampoline) {
        return;
    }
    // While the camera is not updated (a loading screen), motion is not saved up for later.
    const bool stale = NowMs() - m_lastApplyMs.load(std::memory_order_relaxed) > 100;
    if (StepLog()) {
        AddFloat(m_logCounts, xrel);
    }
    if (m_applyInGame.load(std::memory_order_relaxed)) {
        if (stale) {
            m_pendingX.store(xrel, std::memory_order_relaxed);
            m_pendingY.store(yrel, std::memory_order_relaxed);
            return;
        }
        AddFloat(m_pendingX, xrel);
        AddFloat(m_pendingY, yrel);
        return;
    }
    // BB_MOUSE_CAM_APPLY=thread (A/B): the old way, the angles written from the mouse thread.
    const uintptr_t camera = m_camera.load(std::memory_order_acquire);
    if (!camera || stale || m_lockedOn.load(std::memory_order_relaxed)) {
        return;
    }
#ifdef _WIN32
    if (!CanAccess(camera + 0x130, 0x144, true)) {
        return;
    }
#endif
    const float stamp = ReadMemFast<float>(camera + 0x130);
    if (stamp > 0.699f && stamp < 0.701f) {
        Turn(camera, xrel, yrel);
    }
}

extern "C" __attribute__((used)) void bbcam_apply(uintptr_t camera) {
    BloodborneCam::Instance().Apply(camera);
}

std::FILE* BloodborneCam::StepLog() {
    // BB_CAM_STEP_LOG=<file> (measurement aid): one line per camera update with the time (us),
    // the apply mode (g = game update, t = mouse thread), the camera's yaw as the frame uses it,
    // and the mouse counts gathered since the update before.
    static std::FILE* file = [] {
        const char* path = std::getenv("BB_CAM_STEP_LOG");
        return path && *path ? std::fopen(path, "w") : nullptr;
    }();
    return file;
}

void BloodborneCam::Apply(uintptr_t camera) {
    m_camera.store(camera, std::memory_order_release);
    m_lastApplyMs.store(NowMs(), std::memory_order_relaxed);
    if (!m_enabled.load(std::memory_order_acquire)) {
        return;
    }
    // Locked on: the game drives the camera; the window thread routes motion to the stick
    // instead, so a flick switches targets. Checked every update.
    const bool locked = ReadMemFast<float>(camera + 0x154) == 1.0f;
    m_lockedOn.store(locked, std::memory_order_relaxed);
    const bool in_game = m_applyInGame.load(std::memory_order_relaxed);
    if (in_game) {
        const float xrel = m_pendingX.exchange(0.0f, std::memory_order_relaxed);
        const float yrel = m_pendingY.exchange(0.0f, std::memory_order_relaxed);
        if (!locked && (xrel != 0.0f || yrel != 0.0f)) {
            Turn(camera, xrel, yrel, true);
        }
    }
    if (std::FILE* log = StepLog()) {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        std::fprintf(log, "%lld %c %.6f %.3f\n", static_cast<long long>(us), in_game ? 'g' : 't',
                     ReadMemFast<float>(camera + 0x144), m_logCounts.exchange(0.0f));
    }
}

void BloodborneCam::Turn(uintptr_t pitch, float xrel, float yrel, bool game_thread) {
    const float scale = m_scale.load(std::memory_order_relaxed);
    const float deltaPitch = (yrel / kCountsPerRadian) * scale;
    const float deltaYaw = (xrel / kCountsPerRadian) * scale;

    if (m_monocularBase) {
        // The game thread reads through the fault-recovering reader: a VirtualQuery there can
        // take milliseconds in a running game (issue #3).
        auto read = [game_thread](uintptr_t addr, auto fallback) {
            decltype(fallback) v = fallback;
            if (game_thread) {
                return coop::SafeGet(addr, &v) ? v : fallback;
            }
            return ReadMem<decltype(fallback)>(addr);
        };
        uintptr_t p1 = read(m_monocularBase, uintptr_t{0});
        if (p1) {
            uintptr_t p2 = read(p1 + 0x68, uintptr_t{0});
            if (p2) {
                int monocular = read(p2 + 0x84, 0);
                if (monocular != 0) {
                    uintptr_t p3 = read(p2 + 0x68, uintptr_t{0});
                    if (p3) {
                        float monoPitch = read(p3 + 0x148, 0.0f) + deltaPitch * 0.5f;
                        float monoYaw = read(p3 + 0x14C, 0.0f) + deltaYaw * 0.5f;

                        if (monoPitch > 1.71f) {
                            monoPitch = 1.71f;
                        } else if (monoPitch < -1.94f) {
                            monoPitch = -1.94f;
                        }

                        while (monoYaw > 3.14159265f) {
                            monoYaw -= 6.28318531f;
                        }
                        while (monoYaw < -3.14159265f) {
                            monoYaw += 6.28318531f;
                        }

                        if (game_thread) {
                            coop::SafeWrite(p3 + 0x148, &monoPitch, sizeof monoPitch);
                            coop::SafeWrite(p3 + 0x14C, &monoYaw, sizeof monoYaw);
                        } else {
                            WriteMem<float>(p3 + 0x148, monoPitch);
                            WriteMem<float>(p3 + 0x14C, monoYaw);
                        }
                        return;
                    }
                }
            }
        }
    }

    float curPitch = ReadMemFast<float>(pitch + 0x140);
    float curYaw = ReadMemFast<float>(pitch + 0x144);

    curPitch += deltaPitch;

    float minPitch = ReadMemFast<float>(pitch + 0x1F0);
    float maxPitch = ReadMemFast<float>(pitch + 0x1EC);
    if (minPitch >= maxPitch || minPitch < -3.0f || maxPitch > 3.0f) {
        minPitch = -1.94f;
        maxPitch = 1.71f;
    }
    if (curPitch > maxPitch) {
        curPitch = maxPitch;
    } else if (curPitch < minPitch) {
        curPitch = minPitch;
    }

    curYaw += deltaYaw;
    while (curYaw > 3.14159265f) {
        curYaw -= 6.28318531f;
    }
    while (curYaw < -3.14159265f) {
        curYaw += 6.28318531f;
    }

    // Synchronize target pitch, smoothed render pitch, and convergence pitch.
    WriteMemFast<float>(pitch + 0x140, curPitch);
    WriteMemFast<float>(pitch + 0x150, curPitch);
    WriteMemFast<float>(pitch + 0x26C, curPitch);

    // Synchronize target yaw and convergence yaw.
    WriteMemFast<float>(pitch + 0x144, curYaw);
    WriteMemFast<float>(pitch + 0x270, curYaw);
}

} // namespace Core
