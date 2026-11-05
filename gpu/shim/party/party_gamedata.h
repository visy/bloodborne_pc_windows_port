// SPDX-License-Identifier: GPL-3.0-or-later
// The party's tables of game data, read at run time from the player's own game (no game data is
// committed): which maps / lamps / cutscenes / NPCs exist, which item lots carry a pickup flag and
// which awards are host-only, the Beckoning-Bell helpers, the world warper's gear.
//
// scripts/party_tables.py (run by run.bat / run.sh after the mods overlay is merged, so mods are
// respected) derives them from the game folder and writes <user dir>/party_tables.txt, keyed by a
// hash of its inputs and rebuilt when they change. This header parses that file the first time a
// table is asked for. Missing or unreadable file: every table is empty, Loaded() is false and the
// reason is logged once and kept in Status(); the consumers then say no (an unknown map, lamp or
// entity id is refused, no item is shared by lot) and never crash.
//
// File format (UTF-8, "\n" or "\r\n"): `BBPARTYTABLES 1`, `key <sha1>`, then per section `[name] <rows>`
// and that many lines of tab-separated fields. Section names and columns: scripts/party_tables.py.
// Header only, no game dependency; tests install their own tables with Install().
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace coop::gamedata {

struct LotFlag {
    std::int32_t lot;
    std::int32_t flag;
};
struct LedgerRow {
    std::int32_t idx;
    std::int32_t lot;
    std::int32_t done_flag;
    std::string note;
};
struct AwardSpot {
    std::int32_t lot;
    std::int32_t spot_flag;
    std::string note;
};
struct SharedTreasure {
    std::int32_t lot;
    std::int32_t catchup;  // 1: the key catch-up re-gives it
    std::string note;
};
struct BossDropRow {
    std::int32_t lot;
    int area;
    std::string note;
};
struct AuditRowData {
    std::int32_t lot;
    bool required;     // true: ITEM_REQUIRE_SHARED (kinds), false: ITEM_PER_PLAYER (reason)
    std::string note;
};
struct KillAward {
    std::int32_t lot;
    int area;
    std::uint32_t entity;
    std::uint32_t event;
    std::uint32_t spot_flag;
};
/// {npc entity, join flag, leave flag, host-entered flag, arena entry region, x, y, z, yaw}
struct NpcFogRow {
    std::uint32_t npc, join, leave, host_flag, region;
    float x, y, z, yaw;
};

struct Tables {
    bool loaded = false;
    std::string status = "party tables not loaded";
    // ids (sorted, unique)
    std::vector<std::uint32_t> maps, return_points, entities, remos, npc_mirror;
    std::vector<std::uint32_t> return_spot_ids, warp_spot_ids;
    std::vector<float> return_spots, warp_spots;  // x, y, z, yaw per id
    // npc fog
    std::vector<NpcFogRow> npc_fog;
    // world warper gear
    std::vector<std::uint32_t> warper_right, warper_left, warper_armor;
    // items
    std::vector<LotFlag> lot_flags;
    std::vector<LedgerRow> ledger;
    std::vector<AwardSpot> award_spots;
    std::vector<std::int32_t> unshared;
    std::vector<SharedTreasure> shared_treasure;
    std::vector<BossDropRow> boss_drops;
    std::vector<AuditRowData> audit_rows;
    std::vector<KillAward> kill_awards;
};

namespace detail {

inline std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> out;
    std::size_t at = 0;
    for (;;) {
        const std::size_t t = line.find('\t', at);
        if (t == std::string::npos) {
            out.push_back(line.substr(at));
            return out;
        }
        out.push_back(line.substr(at, t - at));
        at = t + 1;
    }
}

inline bool ToU32(const std::string& s, std::uint32_t* v) {
    char* end = nullptr;
    const unsigned long long x = std::strtoull(s.c_str(), &end, 10);
    if (s.empty() || *end != '\0' || x > 0xffffffffull) {
        return false;
    }
    *v = static_cast<std::uint32_t>(x);
    return true;
}
inline bool ToI32(const std::string& s, std::int32_t* v) {
    char* end = nullptr;
    const long long x = std::strtoll(s.c_str(), &end, 10);
    if (s.empty() || *end != '\0' || x < INT32_MIN || x > INT32_MAX) {
        return false;
    }
    *v = static_cast<std::int32_t>(x);
    return true;
}
inline bool ToF(const std::string& s, float* v) {
    char* end = nullptr;
    const float x = std::strtof(s.c_str(), &end);
    if (s.empty() || *end != '\0') {
        return false;
    }
    *v = x;
    return true;
}

}  // namespace detail

/// Parses the cache text into `out` (replacing it). False with `err` set when the text is not a cache of
/// this format or a row is malformed (then `out` is left empty).
inline bool Parse(const std::string& text, Tables* out, std::string* err) {
    using detail::Split;
    *out = Tables{};
    // No iostreams: the fuzz harness replaces operator new, and a stream's buffers come from the C++ runtime's own.
    std::size_t pos = 0;
    std::string line;
    auto next_line = [&]() {
        if (pos >= text.size()) {
            return false;
        }
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            nl = text.size();
        }
        line.assign(text, pos, nl - pos);
        pos = nl + 1;
        return true;
    };
    auto fail = [&](const std::string& m) {
        *out = Tables{};
        out->status = m;
        if (err) {
            *err = m;
        }
        return false;
    };
    if (!next_line() || line.rfind("BBPARTYTABLES 1", 0) != 0) {
        return fail("not a party tables file (wrong header or format version)");
    }
    Tables t;
    std::string section;
    std::size_t left = 0;
    bool any = false;
    auto u32row = [&](std::vector<std::uint32_t>& v, const std::vector<std::string>& f) {
        std::uint32_t x = 0;
        if (f.size() < 1 || !detail::ToU32(f[0], &x)) {
            return false;
        }
        v.push_back(x);
        return true;
    };
    auto spot = [&](std::vector<std::uint32_t>& ids, std::vector<float>& xyz, const std::vector<std::string>& f) {
        std::uint32_t id = 0;
        float v[4];
        if (f.size() < 5 || !detail::ToU32(f[0], &id)) {
            return false;
        }
        for (int k = 0; k < 4; ++k) {
            if (!detail::ToF(f[static_cast<std::size_t>(k) + 1], &v[k])) {
                return false;
            }
        }
        ids.push_back(id);
        xyz.insert(xyz.end(), v, v + 4);
        return true;
    };
    while (next_line()) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (left == 0) {
            if (line.empty() || line.rfind("key ", 0) == 0) {
                continue;
            }
            if (line[0] != '[') {
                return fail("party tables: unexpected line outside a section");
            }
            const std::size_t close = line.find(']');
            if (close == std::string::npos) {
                return fail("party tables: bad section header");
            }
            section = line.substr(1, close - 1);
            std::uint32_t n = 0;
            if (!detail::ToU32(line.size() > close + 2 ? line.substr(close + 2) : std::string(), &n)) {
                return fail("party tables: bad row count in [" + section + "]");
            }
            left = n;
            any = true;
            continue;
        }
        --left;
        const std::vector<std::string> f = Split(line);
        bool ok = true;
        std::int32_t a = 0, b = 0, c = 0;
        std::uint32_t u[5];
        if (section == "maps") ok = u32row(t.maps, f);
        else if (section == "returnPoints") ok = u32row(t.return_points, f);
        else if (section == "entities") ok = u32row(t.entities, f);
        else if (section == "remos") ok = u32row(t.remos, f);
        else if (section == "npcMirror") ok = u32row(t.npc_mirror, f);
        else if (section == "warperRight") ok = u32row(t.warper_right, f);
        else if (section == "warperLeft") ok = u32row(t.warper_left, f);
        else if (section == "warperArmor") ok = u32row(t.warper_armor, f);
        else if (section == "returnSpots") ok = spot(t.return_spot_ids, t.return_spots, f);
        else if (section == "warpSpots") ok = spot(t.warp_spot_ids, t.warp_spots, f);
        else if (section == "npcFog") {
            NpcFogRow r{};
            ok = f.size() >= 9 && detail::ToU32(f[0], &r.npc) && detail::ToU32(f[1], &r.join) &&
                 detail::ToU32(f[2], &r.leave) && detail::ToU32(f[3], &r.host_flag) && detail::ToU32(f[4], &r.region) &&
                 detail::ToF(f[5], &r.x) && detail::ToF(f[6], &r.y) && detail::ToF(f[7], &r.z) && detail::ToF(f[8], &r.yaw);
            if (ok) t.npc_fog.push_back(r);
        } else if (section == "itemLotFlag") {
            ok = f.size() >= 2 && detail::ToI32(f[0], &a) && detail::ToI32(f[1], &b);
            if (ok) t.lot_flags.push_back({a, b});
        } else if (section == "itemLedger") {
            ok = f.size() >= 3 && detail::ToI32(f[0], &a) && detail::ToI32(f[1], &b) && detail::ToI32(f[2], &c);
            if (ok) t.ledger.push_back({a, b, c, f.size() > 3 ? f[3] : std::string()});
        } else if (section == "itemAwardSpot") {
            ok = f.size() >= 2 && detail::ToI32(f[0], &a) && detail::ToI32(f[1], &b);
            if (ok) t.award_spots.push_back({a, b, f.size() > 2 ? f[2] : std::string()});
        } else if (section == "itemUnshared") {
            ok = f.size() >= 1 && detail::ToI32(f[0], &a);
            if (ok) t.unshared.push_back(a);
        } else if (section == "itemSharedTreasure") {
            ok = f.size() >= 2 && detail::ToI32(f[0], &a) && detail::ToI32(f[1], &b);
            if (ok) t.shared_treasure.push_back({a, b, f.size() > 2 ? f[2] : std::string()});
        } else if (section == "itemBossDrop") {
            ok = f.size() >= 2 && detail::ToI32(f[0], &a) && detail::ToI32(f[1], &b);
            if (ok) t.boss_drops.push_back({a, b, f.size() > 2 ? f[2] : std::string()});
        } else if (section == "itemRequireShared" || section == "itemPerPlayer") {
            ok = f.size() >= 1 && detail::ToI32(f[0], &a);
            if (ok) t.audit_rows.push_back({a, section == "itemRequireShared", f.size() > 1 ? f[1] : std::string()});
        } else if (section == "itemKillAward") {
            ok = f.size() >= 5 && detail::ToI32(f[0], &a) && detail::ToI32(f[1], &b) && detail::ToU32(f[2], &u[0]) &&
                 detail::ToU32(f[3], &u[1]) && detail::ToU32(f[4], &u[2]);
            if (ok) t.kill_awards.push_back({a, b, u[0], u[1], u[2]});
        }  // an unknown section is skipped: a newer cache read by an older game
        if (!ok) {
            return fail("party tables: malformed row in [" + section + "]");
        }
    }
    if (!any || left != 0) {
        return fail("party tables: file is cut short");
    }
    // The consumers binary-search these: keep the contract even for a hand-edited file.
    for (auto* v : {&t.maps, &t.return_points, &t.entities, &t.remos, &t.npc_mirror}) {
        std::sort(v->begin(), v->end());
        v->erase(std::unique(v->begin(), v->end()), v->end());
    }
    std::sort(t.unshared.begin(), t.unshared.end());
    std::sort(t.lot_flags.begin(), t.lot_flags.end(), [](const LotFlag& x, const LotFlag& y) {
        return x.lot != y.lot ? x.lot < y.lot : x.flag < y.flag;
    });
    t.loaded = true;
    t.status = "party tables loaded";
    *out = std::move(t);
    return true;
}

/// <BB_PARTY_TABLES>, else <BB_USER_DIR or "user">/party_tables.txt.
inline std::string CachePath() {
    if (const char* e = std::getenv("BB_PARTY_TABLES")) {
        if (*e) {
            return e;
        }
    }
    std::string dir = "user";
    if (const char* u = std::getenv("BB_USER_DIR")) {
        if (*u) {
            dir = u;
        }
    }
    return dir + "/party_tables.txt";
}

/// Loads `path`; on failure `out` is empty and `err` says why.
inline bool LoadFile(const std::string& path, Tables* out, std::string* err) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        *out = Tables{};
        out->status = "party tables missing (" + path + ")";
        if (err) {
            *err = out->status;
        }
        return false;
    }
    std::string text;
    char buf[1 << 16];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) {
        text.append(buf, n);
    }
    std::fclose(f);
    return Parse(text, out, err);
}

namespace detail {
inline Tables& Store() {
    static Tables t;
    return t;
}
inline std::once_flag& Once() {
    static std::once_flag o;
    return o;
}
}  // namespace detail

/// The tables, loaded on first use. Never null, never throws; empty when the cache is missing.
inline const Tables& Get() {
    std::call_once(detail::Once(), [] {
        std::string err;
        Tables t;
        if (!LoadFile(CachePath(), &t, &err)) {
            std::fprintf(stderr,
                         "[party] game-data tables unavailable: %s. Party features that need them (travel, "
                         "shared items, NPC fog, world warper) refuse until the launcher has built them from "
                         "your game files (scripts/party_tables.py).\n",
                         err.c_str());
        }
        detail::Store() = std::move(t);
    });
    return detail::Store();
}

inline bool Loaded() {
    return Get().loaded;
}

/// Replaces the tables (tests, or a caller that built them itself). Not thread safe with Get().
inline void Install(Tables t) {
    std::call_once(detail::Once(), [] {});
    detail::Store() = std::move(t);
}

/// Test helper: loads the cache named by BB_PARTY_TABLES (or the default path) into the store. False
/// when it is absent: the caller prints "skipped" and returns.
inline bool InstallFromEnv() {
    Tables t;
    std::string err;
    if (!LoadFile(CachePath(), &t, &err)) {
        return false;
    }
    Install(std::move(t));
    return true;
}

}  // namespace coop::gamedata
