// SPDX-License-Identifier: GPL-3.0-or-later
// Party HUD, the game side (party_hud.h): this player's vitals out over PartyLink, the remote
// players' PlayerIns read in this world, the board composed and published for the overlay.
// Main thread (the director's tick), ~10 Hz; every read goes through SafeRead.
//
// Offsets (ours = bbhost - 0x400000, 1.09 eboot), as the party code already reads them:
//   WorldChrMan slot 0x553e878: +0x60 the local PlayerIns, +0x30 player count, +0x38 the player
//     set (entries 0x38 apart, PlayerIns at +0): party_b2 PlayerForPeer
//   PlayerIns +0x3b0 -> +0x20 data module: HP +0x128, max HP +0x12c, stamina +0x134, max +0x138
//     (party_b2 guest HP keep, docs/party/npc_peer.md Return step)
//   PlayerIns +0x58 -> +8 -> +0x3b0 -> +0x68 physics: position +0x1e0, rotation +0x1d0 (yaw [1])
//     (party_b2 ChrPos / the B2 destination)
//   PlayerIns +0x3d8 the peer's PlayerId (party_b2 PlayerForPeer: IdEquals): it holds the member's
//     online id (= its PartyLink name, net/bbnet_internal.h), which matches a chr to a member.
#include "party_hud.h"
#include "game_state.h"
#include "coop_hooks.h"
#include "party_async_io.h"
#include "../bbport_camera.h"

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
constexpr u64 kSlotWorldChrMan = 0x553e878;
constexpr u64 kPeerIdOff = 0x3d8;
constexpr std::size_t kPeerIdBytes = 0x40;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[1024];
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

// HP 0..99999 and max > 0: a live data module (anything else is a half-built or freed chr).
bool PlausibleHp(int hp, int mx) {
    return mx > 0 && mx <= 99999 && hp >= 0 && hp <= mx;
}

ChrRead ReadChr(u64 chr) {
    ChrRead r;
    const u64 dm = Field<u64>(Field<u64>(chr, 0x3b0), 0x20);
    const int hp = Field<int>(dm, 0x128, -1), mx = Field<int>(dm, 0x12c, -1);
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

// The member whose online id the chr's PlayerId holds (a NUL-terminated copy of the name).
int MatchName(const std::uint8_t* id, std::size_t n, const party::RosterEntry* roster, int roster_n) {
    for (int i = 0; i < roster_n; ++i) {
        const std::string& name = roster[i].name;
        if (name.empty() || name.size() >= n) continue;
        for (std::size_t at = 0; at + name.size() < n; ++at)
            if (std::memcmp(id + at, name.data(), name.size()) == 0 && id[at + name.size()] == 0 &&
                (at == 0 || id[at - 1] == 0 || id[at - 1] < 0x20))
                return i;
    }
    return -1;
}

std::string Hex(const std::uint8_t* b, std::size_t n) {
    std::string s;
    char t[4];
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(t, sizeof t, "%02x", b[i]);
        s += t;
    }
    return s;
}

struct HudState {
    std::chrono::steady_clock::time_point next{}, next_log{};
    bool published_inactive = false;
    bool log = false, log_init = false;
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
    if (log_now) st.next_log = now + std::chrono::seconds(5);

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
    }
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
            std::uint8_t id[kPeerIdBytes] = {};
            if (!SafeRead(chr + kPeerIdOff, id, sizeof id)) continue;
            const int m = MatchName(id, sizeof id, st.roster.data(), roster_n);
            const ChrRead r = ReadChr(chr);
            if (log_now) {
                char b[256];
                std::snprintf(b, sizeof b, " [chr %d: %s, HP %d/%d st %d/%d, pos %s(%.1f %.1f %.1f), id %s]", i,
                              m >= 0 ? st.roster[m].name.c_str() : "?", r.hp, r.max_hp, r.stamina, r.max_stamina,
                              r.have_pos ? "" : "none ", r.pos[0], r.pos[1], r.pos[2], Hex(id, 0x30).c_str());
                log_chrs += b;
            }
            party::hud::LocalChr& l = chrs[chrs_n++];
            if (m >= 0) std::snprintf(l.name, sizeof l.name, "%s", st.roster[m].name.c_str());
            l.have_hp = r.have_hp;
            l.hp = r.hp;
            l.max_hp = r.max_hp;
            l.stamina = r.stamina;
            l.max_stamina = r.max_stamina;
            l.have_pos = r.have_pos;
            std::memcpy(l.pos, r.pos, sizeof l.pos);
        }
    }
    // A chr whose PlayerId did not name anyone: when exactly one chr and exactly one member in
    // this world are left unmatched, they are each other (a two-player party always).
    {
        int open_chr = -1, open_chrs = 0, open_member = -1, open_members = 0;
        for (int i = 0; i < chrs_n; ++i)
            if (!chrs[i].name[0]) open_chr = i, ++open_chrs;
        for (int i = 0; i < roster_n; ++i) {
            const party::RosterEntry& e = st.roster[i];
            if (!e.connected || !party::hud::SameWorld(link->local_slot(), local_state, e.slot, e.state)) continue;
            bool taken = false;
            for (int k = 0; k < chrs_n; ++k) taken = taken || e.name == chrs[k].name;
            if (!taken) open_member = i, ++open_members;
        }
        if (open_chrs == 1 && open_members == 1)
            std::snprintf(chrs[open_chr].name, sizeof chrs[open_chr].name, "%s", st.roster[open_member].name.c_str());
    }
    party::MemberVitals vit[party::kMaxVitals];
    const int vit_n = link->vitals(vit, party::kMaxVitals);

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
