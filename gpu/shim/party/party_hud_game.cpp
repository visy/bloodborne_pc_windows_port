// SPDX-License-Identifier: GPL-3.0-or-later
// Party HUD, the game side (party_hud.h): this player's vitals out over PartyLink, the remote
// players' PlayerIns read in this world, the board composed and published for the overlay.
// Main thread (the director's tick), ~10 Hz; every read goes through SafeRead.
//
// Offsets (ours = bbhost - 0x400000, 1.09 eboot), as the party code already reads them:
//   WorldChrMan slot 0x553e878: +0x60 the local PlayerIns, +0x30 player count, +0x38 the player
//     set (entries 0x38 apart, PlayerIns at +0): party_b2 PlayerForPeer
//   PlayerIns +0x3b0 -> +0x20 data module: HP +0xf8, max HP +0xfc (0x18bf9a0 / 0x1a0d7d0), stamina
//     +0x134, max +0x138
//     (party_b2 guest HP keep, docs/party/npc_peer.md Return step)
//   PlayerIns +0x58 -> +8 -> +0x3b0 -> +0x68 physics: position +0x1e0, rotation +0x1d0 (yaw [1])
//     (party_b2 ChrPos / the B2 destination)
// A remote PlayerIns does not say whose it is in a readable way (its PlayerId at +0x3d8 holds
// pointers, not the online id: BB_PARTY_HUD_LOG run, 2026-10-11), so chrs are matched to members
// by position: each to the member in this world whose own game reported the nearest position
// (VITALS, <= 8 m); a single chr left over goes to the single member left over. Its HP here reads
// 1 / 1 (the remote data module is not kept up to date): HP comes from VITALS, the position (exact,
// every frame) from the chr.
#include "party_hud.h"
#include "party_balance.h"
#include "game_state.h"
#include "coop_hooks.h"
#include "party_async_io.h"
#include "../bbport_camera.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace coop {

namespace {

using u32 = std::uint32_t;
using u16 = std::uint16_t;
using u8 = std::uint8_t;
constexpr u64 kSlotWorldChrMan = 0x553e878;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    asyncio::Print(std::string("Party HUD: ") + line + "\n");
}

template <class T>
T Field(u64 base, u64 off, T fallback = T{}) {
    T v{};
    return base && SafeGet(base + off, &v) ? v : fallback;
}

u64 Slot(u64 off) {
    u64 v = 0;
    const u64 at = Guest(off);
    return at && SafeGet(at, &v) ? v : 0;
}

struct ChrRead {
    bool have_hp = false;
    int hp = 0, max_hp = 0, stamina = 0, max_stamina = 0;
    bool have_pos = false;
    float pos[3] = {0, 0, 0};
    float yaw = 0;
};

// HP 0..99999 and max > 1: a live data module (a remote player's reads 1 / 1) (anything else is a half-built or freed chr).
bool PlausibleHp(int hp, int mx) {
    return mx > 1 && mx <= 99999 && hp >= 0 && hp <= mx;
}

ChrRead ReadChr(u64 chr) {
    ChrRead r;
    const u64 dm = Field<u64>(Field<u64>(chr, 0x3b0), 0x20);
    // HP +0xf8 / max +0xfc: the HP struct at +0xf0 that the game's kill (0x18bf9a0 writes +0xf8 = 0)
    // and per-frame HP update (0x1a0d7d0) use; +0x128 / +0x12c (read before 2026-10-11) stayed at
    // 106 / 106 on a 719 HP hunter (party_balance live run), so the bars never moved.
    const int hp = Field<int>(dm, 0xf8, -1), mx = Field<int>(dm, 0xfc, -1);
    if (dm && PlausibleHp(hp, mx)) {
        r.have_hp = true;
        r.hp = hp;
        r.max_hp = mx;
        const int st = Field<int>(dm, 0x134, -1), smx = Field<int>(dm, 0x138, -1);
        if (smx > 0 && smx <= 9999 && st >= 0 && st <= smx) {
            r.stamina = st;
            r.max_stamina = smx;
        }
    }
    const u64 phys = Field<u64>(Field<u64>(Field<u64>(Field<u64>(chr, 0x58), 8), 0x3b0), 0x68);
    float p[4] = {}, rot[4] = {};
    if (phys && SafeRead(phys + 0x1e0, p, sizeof p)) {
        bool ok = true;
        for (int i = 0; i < 3; ++i) ok = ok && std::isfinite(p[i]) && std::fabs(p[i]) < 100000.0f;
        if (ok) {
            r.have_pos = true;
            std::memcpy(r.pos, p, sizeof r.pos);
            if (SafeRead(phys + 0x1d0, rot, sizeof rot) && std::isfinite(rot[1])) r.yaw = rot[1];
        }
    }
    return r;
}

/// BB_PARTY_HUD_POSDIAG=1 (diagnostics for the game's phantom name plate, which was seen floating
/// away from its chr): every position-like (x, y, z, 0|1) quad in a remote PlayerIns and the
/// objects its first 0x400 bytes point to, that lies 1..500 m from the physics position.
bool PosDiag() {
    static const bool on = [] {
        const char* v = std::getenv("BB_PARTY_HUD_POSDIAG");
        return v && v[0] == '1';
    }();
    return on;
}

void ScanPositions(u64 base, std::size_t size, const float pos[3], const char* what, std::string* out) {
    std::vector<float> buf(size / 4);
    if (!base || !SafeRead(base, buf.data(), size)) return;
    for (std::size_t i = 0; i + 4 <= buf.size(); i += 4) {
        const float* q = &buf[i];
        if (!(q[3] == 0.0f || q[3] == 1.0f)) continue;
        bool ok = true;
        for (int k = 0; k < 3; ++k) ok = ok && std::isfinite(q[k]) && std::fabs(q[k]) < 5000.0f && std::fabs(q[k]) > 0.01f;
        if (!ok) continue;
        const float dx = q[0] - pos[0], dy = q[1] - pos[1], dz = q[2] - pos[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < 1.0f || d > 500.0f) continue;
        char b[96];
        std::snprintf(b, sizeof b, " %s+%zx (%.1f %.1f %.1f) d %.1f;", what, i * 4, q[0], q[1], q[2], d);
        *out += b;
    }
}

void DumpPositions(u64 chr, const float pos[3]) {
    std::string out;
    ScanPositions(chr, 0x1000, pos, "chr", &out);
    std::vector<u64> ptrs(0x400 / 8);
    if (SafeRead(chr, ptrs.data(), 0x400)) {
        for (std::size_t i = 0; i < ptrs.size(); ++i) {
            const u64 p = ptrs[i];
            if (p < 0x10000 || (p & 7)) continue;
            char what[24];
            std::snprintf(what, sizeof what, "[chr+%zx]", i * 8);
            ScanPositions(p, 0x400, pos, what, &out);
        }
    }
    Log("posdiag chr %llx at (%.1f %.1f %.1f):%s", static_cast<unsigned long long>(chr), pos[0], pos[1], pos[2],
        out.empty() ? " nothing else" : out.c_str());
}

struct HudState {
    std::chrono::steady_clock::time_point next{}, next_log{};
    bool published_inactive = false;
    bool log = false, log_init = false;
    long log_ms = 5000;  // BB_PARTY_HUD_LOG interval
    bool together = false;  // a companion's PlayerIns was read here (since together_at)
    std::chrono::steady_clock::time_point together_at{};
    std::vector<party::RosterEntry> roster;  // reused: no allocation once warm
};

HudState& S() {
    static HudState s;
    return s;
}

}  // namespace

/// The director, once a frame (main thread). Cheap: works every 100 ms.
void PartyHudTick(const GameSnapshot& s, party::PartyLink* link, party::MemberState local_state) {
    HudState& st = S();
    if (!st.log_init) {
        st.log_init = true;
        const char* v = std::getenv("BB_PARTY_HUD_LOG");
        st.log = v && v[0] && v[0] != '0';
        // "1": every 5 s; a number above 1: every that many ms (position tracing, A7).
        const long ms = v ? std::strtol(v, nullptr, 10) : 0;
        st.log_ms = ms > 1 ? std::max(200L, ms) : 5000;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < st.next) return;
    st.next = now + std::chrono::milliseconds(100);
    if (!link) {
        if (!st.published_inactive) {
            party::hud::Publish(party::hud::Board{});
            st.published_inactive = true;
        }
        return;
    }
    st.published_inactive = false;
    const bool in_world = s.world_up && !s.loading && s.player;
    const bool log_now = st.log && now >= st.next_log;
    if (log_now) st.next_log = now + std::chrono::milliseconds(st.log_ms);

    // This player's vitals (sent whether or not its own HUD is on: the others' HUD needs them).
    party::MemberVitals me;
    ChrRead mine;
    if (in_world) {
        mine = ReadChr(s.player);
        me.flags = (mine.have_hp ? party::kVitalsValid : 0) | (mine.have_hp && mine.hp > 0 ? party::kVitalsAlive : 0) |
                   (mine.have_pos ? party::kVitalsHasPos : 0);
        me.hp = static_cast<u32>(mine.hp);
        me.max_hp = static_cast<u32>(mine.max_hp);
        me.stamina = static_cast<std::uint16_t>(mine.stamina);
        me.max_stamina = static_cast<std::uint16_t>(mine.max_stamina);
        me.map_id = s.map_id != 0xffffffffu ? s.map_id : 0;
        // Positions to a tenth of a millimetre are noise: a standing player sends keepalives only.
        for (int i = 0; i < 3; ++i) me.pos[i] = std::round(mine.pos[i] * 100.0f) / 100.0f;
        me.yaw = std::round(mine.yaw * 100.0f) / 100.0f;
        me.level = static_cast<std::uint16_t>(s.level > 0 && s.level < 1000 ? s.level : 0);
    }
    me.deaths = static_cast<std::uint16_t>(std::min(balance::LocalDeaths(), 65535));
    me.rot = static_cast<std::uint8_t>(balance::LocalRotStacks());
    link->set_local_vitals(me);

    st.roster = link->roster();
    const int roster_n = static_cast<int>(st.roster.size());
    // The remote players the game has in this world.
    party::hud::LocalChr chrs[party::kMaxVitals];
    int chrs_n = 0;
    std::string log_chrs;
    if (in_world) {
        const u64 wcm = Slot(kSlotWorldChrMan);
        const int n = Field<int>(wcm, 0x30, 0);
        const u64 entries = Field<u64>(wcm, 0x38);
        for (int i = 0; entries && n > 0 && n <= 8 && i < n && chrs_n < party::kMaxVitals; ++i) {
            const u64 chr = Field<u64>(entries + u64(i) * 0x38, 0);
            if (!chr || chr == s.player) continue;
            const ChrRead r = ReadChr(chr);
            if (!r.have_pos) continue;
            // A disabled chr (ChrIns_SetDisable 0x18c6390: [chr+0x18]+0x20 bit 0, [chr+0x58]+0x114
            // bit 0x40) is not drawn: it is not "here" for the HUD or the tags (split travel removes
            // and rebuilds remote chrs; a hidden copy must not get a tag).
            if ((Field<u8>(Field<u64>(chr, 0x18), 0x20) & 1) || (Field<u16>(Field<u64>(chr, 0x58), 0x114) & 0x40)) continue;
            if (log_now && PosDiag()) DumpPositions(chr, r.pos);
            party::hud::LocalChr& l = chrs[chrs_n++];
            l.have_hp = r.have_hp;
            l.hp = r.hp;
            l.max_hp = r.max_hp;
            l.stamina = r.stamina;
            l.max_stamina = r.max_stamina;
            l.have_pos = r.have_pos;
            std::memcpy(l.pos, r.pos, sizeof l.pos);
        }
    }
    party::MemberVitals vit[party::kMaxVitals];
    const int vit_n = link->vitals(vit, party::kMaxVitals);
    // Chrs to members by the positions the members' games reported (nearest first, <= 8 m).
    for (;;) {
        float best = 8.0f * 8.0f;
        int bc = -1, bm = -1;
        for (int c = 0; c < chrs_n; ++c) {
            if (chrs[c].name[0]) continue;
            for (int v = 0; v < vit_n; ++v) {
                if (!(vit[v].flags & party::kVitalsHasPos) || vit[v].slot == link->local_slot()) continue;
                const party::RosterEntry* e = nullptr;
                for (const party::RosterEntry& r : st.roster)
                    if (r.slot == vit[v].slot) e = &r;
                if (!e || !e->connected || !party::hud::SameWorld(link->local_slot(), local_state, e->slot, e->state))
                    continue;
                bool taken = false;
                for (int k = 0; k < chrs_n; ++k) taken = taken || e->name == chrs[k].name;
                if (taken) continue;
                const float d2 = (chrs[c].pos[0] - vit[v].pos[0]) * (chrs[c].pos[0] - vit[v].pos[0]) +
                                 (chrs[c].pos[1] - vit[v].pos[1]) * (chrs[c].pos[1] - vit[v].pos[1]) +
                                 (chrs[c].pos[2] - vit[v].pos[2]) * (chrs[c].pos[2] - vit[v].pos[2]);
                if (d2 < best) best = d2, bc = c, bm = static_cast<int>(e - st.roster.data());
            }
        }
        if (bc < 0) break;
        std::snprintf(chrs[bc].name, sizeof chrs[bc].name, "%s", st.roster[bm].name.c_str());
    }
    // A remote chr here that no member's own report is near: this game's copy of that player is
    // stale (no position updates: seen as the game's phantom name plate floating away from where
    // the player really is, host side, 3 players). Logged once a minute per chr with the nearest report.
    {
        static std::chrono::steady_clock::time_point next_stale{};
        if (now >= next_stale) {
            for (int c = 0; c < chrs_n; ++c) {
                if (chrs[c].name[0]) continue;
                float best = 1e18f;
                int bs = -1;
                for (int v = 0; v < vit_n; ++v) {
                    if (!(vit[v].flags & party::kVitalsHasPos) || vit[v].slot == link->local_slot()) continue;
                    const float dx = chrs[c].pos[0] - vit[v].pos[0], dy = chrs[c].pos[1] - vit[v].pos[1],
                                dz = chrs[c].pos[2] - vit[v].pos[2];
                    const float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < best) best = d2, bs = vit[v].slot;
                }
                if (bs >= 0 && best > 8.0f * 8.0f) {
                    next_stale = now + std::chrono::seconds(60);
                    Log("stale remote chr? one at (%.1f %.1f %.1f) HP %d/%d is %.1f m from the nearest member report "
                        "(slot %d); no tag for it",
                        chrs[c].pos[0], chrs[c].pos[1], chrs[c].pos[2], chrs[c].hp, chrs[c].max_hp, std::sqrt(best), bs);
                }
            }
        }
    }
    // One chr and one member in this world left over: each other (no VITALS position yet).
    {
        int open_chr = -1, open_chrs = 0, open_member = -1, open_members = 0;
        for (int i = 0; i < chrs_n; ++i)
            if (!chrs[i].name[0]) open_chr = i, ++open_chrs;
        for (int i = 0; i < roster_n; ++i) {
            const party::RosterEntry& e = st.roster[i];
            if (!e.connected || !party::hud::SameWorld(link->local_slot(), local_state, e.slot, e.state)) continue;
            bool taken = false;
            for (int k = 0; k < chrs_n; ++k) taken = taken || e.name == chrs[k].name;
            // A member whose own game reports a position (VITALS) that no chr here is near is not
            // standing here (it respawned elsewhere; the chr left over is a corpse / a stale copy):
            // no fallback match, or its tag would float over the old spot.
            bool reported = false;
            for (int v = 0; v < vit_n; ++v)
                if (vit[v].slot == e.slot && (vit[v].flags & party::kVitalsHasPos)) reported = true;
            if (!taken && !reported) open_member = i, ++open_members;
        }
        if (open_chrs == 1 && open_members == 1)
            std::snprintf(chrs[open_chr].name, sizeof chrs[open_chr].name, "%s", st.roster[open_member].name.c_str());
    }
    // Ghost check (the host-cannot-see-guest bug, 2026-10-11): where this game draws a member's chr
    // vs where that member's own game says it stands (VITALS, fresh). Over 2 m: a "GAP" line, at
    // most every 5 s per member (always on: a cheap loop over at most 3 chrs).
    {
        static std::chrono::steady_clock::time_point gap_logged[party::kMaxVitals]{};
        for (int c = 0; c < chrs_n; ++c) {
            if (!chrs[c].name[0]) continue;
            for (int v = 0; v < vit_n; ++v) {
                const party::RosterEntry* e = nullptr;
                for (const party::RosterEntry& r : st.roster)
                    if (r.slot == vit[v].slot) e = &r;
                if (!e || e->name != chrs[c].name || !(vit[v].flags & party::kVitalsHasPos) || vit[v].age_ms > 1500)
                    continue;
                const float dx = chrs[c].pos[0] - vit[v].pos[0], dy = chrs[c].pos[1] - vit[v].pos[1],
                            dz = chrs[c].pos[2] - vit[v].pos[2];
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                const int slot = vit[v].slot;
                if (d > 2.0f && slot >= 0 && slot < party::kMaxVitals && now - gap_logged[slot] >= std::chrono::seconds(5)) {
                    gap_logged[slot] = now;
                    Log("GAP %s (slot %d): this game draws its chr at (%.2f %.2f %.2f), it reports (%.2f %.2f %.2f) "
                        "map %08x: %.1f m apart",
                        chrs[c].name, slot, chrs[c].pos[0], chrs[c].pos[1], chrs[c].pos[2], vit[v].pos[0],
                        vit[v].pos[1], vit[v].pos[2], vit[v].map_id, d);
                }
            }
        }
    }
    if (log_now)
        for (int c = 0; c < chrs_n; ++c) {
            char b[160];
            std::snprintf(b, sizeof b, " [chr %s: HP %d/%d st %d/%d, pos (%.1f %.1f %.1f)]",
                          chrs[c].name[0] ? chrs[c].name : "?", chrs[c].hp, chrs[c].max_hp, chrs[c].stamina,
                          chrs[c].max_stamina, chrs[c].pos[0], chrs[c].pos[1], chrs[c].pos[2]);
            log_chrs += b;
        }

    party::hud::ComposeIn in;
    in.local_slot = link->local_slot();
    in.local_state = local_state;
    in.local_map = me.map_id;
    in.me_have_pos = mine.have_pos;
    std::memcpy(in.me_pos, mine.pos, sizeof in.me_pos);
    in.me_yaw = mine.yaw;
    in.roster = st.roster.data();
    in.roster_n = roster_n;
    in.vitals = vit;
    in.vitals_n = vit_n;
    in.chrs = chrs;
    in.chrs_n = chrs_n;
    party::hud::Board b;
    party::hud::Compose(in, &b);
    party::hud::Publish(b);
    bool any_local = false;
    for (int i = 0; i < b.count; ++i) any_local = any_local || b.c[i].local_read;
    if (any_local && !st.together) st.together_at = now;
    st.together = any_local;

    if (log_now) {
        // What each member's own game reports (VITALS): compare with the chr positions above.
        for (int v = 0; v < vit_n; ++v) {
            char t[120];
            std::snprintf(t, sizeof t, " <slot %d says (%.2f %.2f %.2f) map %08x age %u ms>", vit[v].slot, vit[v].pos[0],
                          vit[v].pos[1], vit[v].pos[2], vit[v].map_id, vit[v].age_ms);
            log_chrs += t;
        }
        std::string comp;
        for (int i = 0; i < b.count; ++i) {
            const party::hud::Companion& c = b.c[i];
            char t[200];
            std::snprintf(t, sizeof t, " {%s slot %d %s%s%s HP %d/%d map %08x%s age %u ms}", c.name, c.slot,
                          party::member_state_name(c.state), c.same_world ? " same-world" : "",
                          c.local_read ? " local-read" : "", c.hp, c.max_hp, c.map_id,
                          c.same_map ? " same-map" : "", c.age_ms);
            comp += t;
        }
        const BbCamera::State cam = BbCamera::Latest();
        const auto cp = BbCamera::Position(cam);
        Log("me HP %d/%d st %d/%d map %08x pos %s(%.2f %.2f %.2f) yaw %.2f; camera %s(%.2f %.2f %.2f) fwd (%.2f %.2f %.2f)"
            " proj %.2f %.2f; %d players in the set%s; vitals %d; together %.0f s; board%s",
            mine.hp, mine.max_hp, mine.stamina, mine.max_stamina, me.map_id, mine.have_pos ? "" : "none ",
            mine.pos[0], mine.pos[1], mine.pos[2], mine.yaw, cam.valid ? "" : "none ", cp[0], cp[1], cp[2],
            cam.view[8], cam.view[9], cam.view[10], cam.proj[0], cam.proj[1], Field<int>(Slot(kSlotWorldChrMan), 0x30, 0),
            log_chrs.c_str(), vit_n,
            st.together ? std::chrono::duration<double>(now - st.together_at).count() : -1.0, comp.c_str());
    }
}

}  // namespace coop
