// SPDX-License-Identifier: GPL-3.0-or-later
// Six players: the patch set and the read-only probe (party_sixp.h, docs/party/six_players.md).
#include "party_sixp.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_async_io.h"
#include "../../party_env.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>

namespace coop::sixp {

namespace {

std::atomic<bool> g_active{false};

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("Party 6p: ") + line + "\n");
}

constexpr std::uint64_t kWorldChrMan = 0x553e878, kGameDataMan = 0x553b130, kNetFlow = 0x5556678;
unsigned long long ull(std::uint64_t v) { return static_cast<unsigned long long>(v); }

// Stage 2: the GameDataMan init 0x14e4e80(this) builds the rows; its allocation size and count are patched to 5 rows, this
// post-init hook constructs the 5th row (PlayerGameData ctor 0x14f1510(row, 0, -1), the same call as the four unrolled ones) and
// replaces the 4-byte occupancy array at +0x18 by a zeroed 8-byte one (slot 4 = byte 4). All fresh at this point.
using GdmInit = void(BB_COOP_SYSV*)(std::uint64_t);
using RowCtor = void(BB_COOP_SYSV*)(std::uint64_t, std::uint64_t, std::uint64_t);
using Alloc = std::uint64_t(BB_COOP_SYSV*)(std::uint64_t, std::uint64_t, std::uint64_t);
GdmInit g_gdm_original = nullptr;
BB_COOP_SYSV void GdmInitHook(std::uint64_t gdm) {
    g_gdm_original(gdm);
    std::uint64_t rows = 0;
    if (!SafeGet(gdm + 0x10, &rows) || !rows) {
        Log("GameDataMan init: no rows pointer; the 5th row is not made");
        return;
    }
    const std::uint64_t row4 = rows + 4 * 0x6a0;
    reinterpret_cast<RowCtor>(Guest(0x14f1510))(row4, 0, 0xffffffffull);
    std::uint64_t alloc_obj = 0, vtbl = 0, fn = 0;
    SafeGet(Guest(0x5540408), &alloc_obj);
    if (alloc_obj) SafeGet(alloc_obj, &vtbl);
    if (vtbl) SafeGet(vtbl + 0x58, &fn);
    std::uint64_t occ = fn ? reinterpret_cast<Alloc>(fn)(alloc_obj, 8, 1) : 0;
    if (!occ) {
        Log("GameDataMan init: the occupancy array could not be allocated");
        return;
    }
    const std::uint64_t zero = 0;
    SafeWrite(occ, &zero, 8);
    SafeWrite(gdm + 0x18, &occ, 8);
    Log("GameDataMan: row 4 built at %#llx, occupancy array %#llx (5 rows)", ull(row4), ull(occ));
}

// Stage 3: the presentation object. Slot 5 (+0x80..+0x93) takes {handle -1, type 1, state 0, -1, -1} like every empty slot, the tracked
// handles (+0xb0..+0xbf) are -1 and their count (+0xc0) 0.
void InitExtra(std::uint64_t obj) {
    const std::uint32_t slot5[5] = {0xffffffffu, 1u, 0u, 0xffffffffu, 0xffffffffu};
    const std::uint32_t handles[4] = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};
    const std::uint32_t zero = 0;
    SafeWrite(obj + 0x80, slot5, sizeof slot5);
    SafeWrite(obj + 0xb0, handles, sizeof handles);
    SafeWrite(obj + 0xc0, &zero, 4);
}
using Ctor = void(BB_COOP_SYSV*)(std::uint64_t, std::uint64_t);
Ctor g_pres_ctor_original = nullptr;
BB_COOP_SYSV void PresCtorHook(std::uint64_t obj, std::uint64_t arg) {
    g_pres_ctor_original(obj, arg);
    InitExtra(obj);
}
using Reset = void(BB_COOP_SYSV*)(std::uint64_t);
BB_COOP_SYSV void PresResetHook(std::uint64_t obj) {
    reinterpret_cast<Reset>(Guest(0x15bcfc0))(obj);
    InitExtra(obj);
}
// The game keeps the slots packed (readers stop at the first empty one): its release 0x15bc080 compacts slots 0-3 from later
// ones; slot 5 into slot 4 is the one move it has no block for. After the original: slot 4 empty and slot 5 used -> move it.
using Release = std::uint64_t(BB_COOP_SYSV*)(std::uint64_t, std::uint64_t);
Release g_release_original = nullptr;
BB_COOP_SYSV std::uint64_t PresReleaseHook(std::uint64_t obj, std::uint64_t handle) {
    const std::uint64_t r = g_release_original(obj, handle);
    std::uint32_t s4 = 0, s5 = 0;
    if (SafeGet(obj + 0x6c, &s4) && SafeGet(obj + 0x80, &s5) && s4 == 0xffffffffu && s5 != 0xffffffffu) {
        std::uint32_t slot[5] = {};
        if (SafeRead(obj + 0x80, slot, sizeof slot)) {
            SafeWrite(obj + 0x6c, slot, sizeof slot);
            const std::uint32_t empty[5] = {0xffffffffu, 1u, 0u, 0xffffffffu, 0xffffffffu};
            SafeWrite(obj + 0x80, empty, sizeof empty);
            reinterpret_cast<Reset>(Guest(0x15bce10))(obj);  // the member counts again
            Log("presentation: slot 5 moved to slot 4 (handle %#x)", slot[0]);
        }
    }
    return r;
}

} // namespace

bool SixpActive() {
    return g_active.load();
}

void SixpInit() {
    if (!bb_party_enabled() || !EnabledFromEnv()) {
        return;
    }
    if (!Image()) {
        Log("no image; the 6-seat set is off");
        return;
    }
    const int stage = StageFromEnv();
    const std::vector<Site> sites = AllSites(stage);
    std::string why;
    if (!SitesSane(sites, &why)) {
        Log("the site list is not sane (%s); the 6-seat set is off", why.c_str());
        return;
    }
    // Whole or not at all: every site must hold the 1.09 bytes (or already the patched ones) before the first write.
    for (const Site& s : sites) {
        if (!Matches(s.off, s.from.data(), s.from.size()) && !Matches(s.off, s.to.data(), s.to.size())) {
            Log("site %#llx (%s) differs from 1.09: the 6-seat set is off (the party stays at 5 seats)", ull(s.off), s.what);
            return;
        }
    }
    if (stage >= 2) {  // the hook first: its prologue is compared by ReplacePrologue, nothing is written when it differs
        void* original = nullptr;
        if (!ReplacePrologue(0x14e4e80, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53},
                             reinterpret_cast<const void*>(&GdmInitHook), &original, "6p GameDataMan init (5th row)")) {
            Log("the GameDataMan init differs from 1.09: the 6-seat set is off");
            return;
        }
        g_gdm_original = reinterpret_cast<GdmInit>(original);
    }
    if (stage >= 3) {
        const std::initializer_list<std::uint8_t> p13 = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53};
        void* o1 = nullptr;
        void* o2 = nullptr;
        if (!ReplacePrologue(0x15ba630, p13, reinterpret_cast<const void*>(&PresCtorHook), &o1, "6p presentation ctor (slot 5, tracked handles)") ||
            !ReplacePrologue(0x15bc080, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53},
                             reinterpret_cast<const void*>(&PresReleaseHook), &o2, "6p presentation release (slot 5 -> 4)") ||
            !HookCallSite(0x1942312, 0x15bcfc0, reinterpret_cast<const void*>(&PresResetHook), "6p presentation reset (slot 5, tracked handles)")) {
            Log("a presentation hook differs from 1.09: the 6-seat set is off");
            return;
        }
        g_pres_ctor_original = reinterpret_cast<Ctor>(o1);
        g_release_original = reinterpret_cast<Release>(o2);
    }
    int n = 0;
    for (const Site& s : sites) {
        if (!PatchBytes(s.off, s.from.data(), s.to.data(), s.from.size(), s.what)) {
            Log("write of %#llx (%s) failed after the check: the 6-seat set is half in, the party must not run", ull(s.off), s.what);
            return;
        }
        ++n;
    }
    g_active.store(true);
    Log("%d site(s) patched, stage %d (%s)", n, stage, kRulesTag);
}

void SixpTick() {
    static const bool probe = [] {
        const char* e = std::getenv("BB_PARTY_SIX_PROBE");
        return e && e[0] == '1';
    }();
    if (!probe) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last{};
    static int logs = 0;
    const auto now = Clock::now();
    if (logs >= 12 || now - last < std::chrono::seconds(10)) {
        return;
    }
    last = now;
    std::uint64_t wcm = 0, gdm = 0, flow = 0;
    SafeGet(Guest(kWorldChrMan), &wcm);
    SafeGet(Guest(kGameDataMan), &gdm);
    SafeGet(Guest(kNetFlow), &flow);
    std::int64_t row_count = -1;
    std::uint64_t chr_entries = 0, row_base = 0, occ_ptr = 0, pres = 0;
    std::int32_t chr_count = -1, pres_count = -1, pres_state = -1;
    std::uint32_t occ = 0xffffffffu;
    if (wcm) {
        SafeGet(wcm + 0x30, &chr_count);
        SafeGet(wcm + 0x38, &chr_entries);
    }
    if (gdm) {
        SafeGet(gdm + 0x10, &row_base);
        SafeGet(gdm + 0x18, &occ_ptr);
        if (row_base) {
            SafeGet(row_base - 8, &row_count);
        }
        if (occ_ptr) {
            SafeGet(occ_ptr, &occ);
        }
    }
    if (flow) {
        SafeGet(flow + 0x16f8, &pres);
        if (pres) {
            SafeGet(pres + 0x14, &pres_count);
            SafeGet(pres + 0x98, &pres_state);
        }
    }
    char slots[160] = "";
    if (pres) {
        int n = 0;
        for (int i = 0; i < 6 && n < 150; ++i) {
            std::uint32_t h = 0xdeadbeefu;
            SafeGet(pres + 0x1c + i * 0x14, &h);
            n += std::snprintf(slots + n, sizeof slots - n, "%s%x", i ? " " : "", h);
        }
        std::uint32_t th[4] = {}, tc = 0;
        SafeRead(pres + 0xb0, th, sizeof th);
        SafeGet(pres + 0xc0, &tc);
        std::snprintf(slots + n, sizeof slots - n, " | tracked %x %x %x %x n%u", th[0], th[1], th[2], th[3], tc);
    }
    ++logs;
    Log("probe: WorldChrMan %#llx ChrSet count %d entries %#llx; GameDataMan %#llx rows %#llx row count %lld occupancy %#010x; "
        "presentation %#llx members %d state %d slots [%s]; set %s",
        ull(wcm), chr_count, ull(chr_entries), ull(gdm), ull(row_base), static_cast<long long>(row_count), occ, ull(pres),
        pres_count, pres_state, slots, g_active.load() ? "ON" : "off");
}

} // namespace coop::sixp
