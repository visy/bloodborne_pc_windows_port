// SPDX-License-Identifier: GPL-3.0-or-later
// The event flag category table (the Python tools' flag_tool.category_rows). Only the category of a flag is
// needed by the party tables (the item award spots), so the rows' notes are not kept.
#include "derive.h"

namespace coop::gamedata {
namespace {

struct CatRow {
    std::int64_t lo, hi;
    Cat cat;
};

using enum Cat;
#include "flag_category_rows.inc"

std::int64_t FDiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}
std::int64_t FMod(std::int64_t a, std::int64_t b) { return a - FDiv(a, b) * b; }

bool Has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }
template <std::size_t N>
bool HasAny(const std::string& hay, const char* const (&words)[N]) {
    for (const char* w : words) {
        if (Has(hay, w)) return true;
    }
    return false;
}

constexpr const char* kDoorKw[] = {"扉", "門", "エレベ", "ハシゴ", "梯子", "はしご", "ショートカット", "レバー", "跳ね橋", "隠し", "仕掛け", "開放", "鍵を開け", "リフト"};
constexpr const char* kDoorSkip[] = {"msg", "Msg", "メッセージ", "動かない", "PlayLog", "プレイログ", "ナビメッシュ", "初期化"};
constexpr const char* kNpcKw[] = {"NPC", "人形", "老人", "メッセンジャー", "乞食", "娼婦", "少女", "神父", "女医", "復讐者", "血族狩り", "クモ男",
                                  "老婆", "偏屈", "ヘンリック", "門番", "連盟", "学長", "司祭", "アルフレート", "アイリーン", "ヴァルトール", "シモン"};
constexpr const char* kKeyKw[] = {"時間帯変化", "エンド", "人さらい", "異様な月"};

// the flag store's map table: every map with pooled groups
constexpr int kStoreMaps[][2] = {{21, 0}, {21, 1}, {22, 0}, {23, 0}, {24, 0}, {24, 1}, {24, 2}, {25, 0}, {26, 0},
                                 {27, 0}, {28, 0}, {29, 0}, {32, 0}, {33, 0}, {34, 0}, {35, 0}, {36, 0}};

Cat ZoneDefault(int typ, int zone) {
    if (typ == 5) return item_lot_picked;
    switch (typ * 10 + zone) {
        case 10: case 11: return world_state;
        case 14: return boss_area;
        case 15: case 16: case 17: return session_runtime;
        case 60: case 61: return world_state;
        case 70: return npc_quest;
        default: return never_sync;
    }
}

const std::vector<CatRow>& Structural() {
    static const std::vector<CatRow> rows = [] {
        std::vector<CatRow> r(std::begin(kGlobalRows), std::end(kGlobalRows));
        const int types[] = {1, 5, 6, 7, 9};
        for (int typ : types) {
            for (const auto& m : kStoreMaps) {
                for (int zone = 0; zone < 10; ++zone) {
                    const std::int64_t lo = typ * 10000000LL + m[0] * 100000LL + m[1] * 10000LL + zone * 1000LL;
                    const Cat c = (typ == 9 || m[0] == 29) ? never_sync : ZoneDefault(typ, zone);
                    r.push_back({lo, lo + 999, c});
                }
            }
        }
        for (int typ : types) {  // m29 blocks 1..9: chalice layouts
            const std::int64_t lo = typ * 10000000LL + 29 * 100000LL + 10000;
            r.push_back({lo, lo + 89999, never_sync});
        }
        for (int area = 41; area < 47; ++area) {  // chalice instance areas
            for (int typ : types) {
                const std::int64_t lo = typ * 10000000LL + area * 100000LL;
                r.push_back({lo, lo + 9999, never_sync});
            }
        }
        for (int typ : {1, 5, 6, 7}) {  // area 99
            const std::int64_t lo = typ * 10000000LL + 99 * 100000LL;
            r.push_back({lo, lo + 9999, never_sync});
        }
        return r;
    }();
    return rows;
}

Cat Scan(const CatRow* b, const CatRow* e, std::int64_t f) {
    for (; b != e; ++b) {
        if (b->lo <= f && f <= b->hi) return b->cat;
    }
    return Cat::none;
}

enum class PSrc { ItemLot, ItemLotCumulate, ShopEvent, ShopQwc };
struct PFlag {
    std::int64_t flag;
    PSrc src;
};

std::vector<PFlag> ParamFlags(const ParamSet& P) {
    std::vector<PFlag> res;
    const bbdata::ParamTable& lot = P.item_lot;
    int cols[9];
    cols[0] = lot.Col("getItemFlagId");
    for (int i = 1; i <= 8; ++i) cols[i] = lot.Col("getItemFlagId0" + std::to_string(i));
    const int cum = lot.Col("cumulateNumFlagId");
    for (std::size_t r = 0; r < lot.Rows(); ++r) {
        for (int c : cols) {
            const std::int64_t f = lot.At(r, c);
            if (f > 0) res.push_back({f, PSrc::ItemLot});
        }
        const std::int64_t f = lot.At(r, cum);
        if (f > 0) res.push_back({f, PSrc::ItemLotCumulate});
    }
    const bbdata::ParamTable& shop = P.shop;
    const int ev = shop.Col("eventFlag"), qwc = shop.Col("qwcId");
    for (std::size_t r = 0; r < shop.Rows(); ++r) {
        if (shop.At(r, ev) > 0) res.push_back({shop.At(r, ev), PSrc::ShopEvent});
        if (shop.At(r, qwc) > 0) res.push_back({shop.At(r, qwc), PSrc::ShopQwc});
    }
    return res;
}

}  // namespace

FlagCategories::FlagCategories(const std::vector<FileEvents>& events, const ParamSet& params) {
    using bbdata::FlagOp;
    using bbdata::MarkerKind;
    auto put = [&](std::int64_t f, Cat cat, int prio) {
        if (f < 0) return;
        const auto it = spec_.find(f);
        if (it == spec_.end() || prio > it->second.prio) spec_[f] = {cat, prio};
    };
    for (const FileEvents& fe : events) {
        for (const bbdata::EventRec& r : fe.recs) {
            const std::string& name = r.name;
            const std::int64_t done = r.event >= 10000000 ? r.event + r.slot : 0;
            if (r.Has(MarkerKind::BossDefeat) || r.Has(MarkerKind::MidbossDefeat)) {
                if (done) put(done, boss_defeated, 9);
                for (const bbdata::Op& o : r.ops) {
                    if (o.StateIs(1) && o.first == o.last) {
                        const std::int64_t f = o.first;
                        if (f < 2100 || (5000 <= f && f <= 8999) || (9000 <= f && f <= 9451)) continue;
                        put(f, boss_defeated, 8);
                    }
                }
            }
            if (name.rfind("ワープOBJ_", 0) == 0) {
                std::vector<std::int64_t> ids;
                for (const auto& rd : r.reads) ids.insert(ids.end(), rd.begin(), rd.end());
                for (const bbdata::Op& o : r.ops) {
                    if (o.first == o.last) ids.push_back(o.first);
                }
                for (const std::int64_t f : ids) {
                    if (FDiv(f, 10000000) != 7 || FMod(FDiv(f, 1000), 10) != 0) continue;
                    if (name.rfind("ワープOBJ_起動", 0) == 0 && FDiv(FMod(f, 1000), 100) == 2) {
                        put(f, lamp_unlocked, 9);
                    } else {
                        put(f, session_runtime, 6);
                    }
                }
            }
            for (const bbdata::Op& o : r.ops) {
                if (o.op == FlagOp::Lamp && o.first > 0) {
                    put(o.first, lamp_unlocked, 9);
                } else if (o.op == FlagOp::Ladder && o.first > 0) {
                    put(o.first, shortcut_door, 7);
                }
            }
            if (!done) continue;
            const std::int64_t zone = FMod(FDiv(done, 1000), 10), typ = FDiv(done, 10000000);
            if (typ != 1) continue;
            if (r.rest == 1 && (zone == 0 || zone == 1)) {
                put(done, session_runtime, 3);
                continue;
            }
            if (zone != 0 && zone != 1) continue;
            auto sets_in_zone01 = [&](Cat c) {
                for (const bbdata::Op& o : r.ops) {
                    const std::int64_t f = o.first;
                    if (o.StateIs(1) && f == o.last && f >= 10000000 && FDiv(f, 10000000) == 1 &&
                        (FMod(FDiv(f, 1000), 10) == 0 || FMod(FDiv(f, 1000), 10) == 1)) {
                        put(f, c, 5);
                    }
                }
            };
            if (HasAny(name, kKeyKw)) {
                put(done, key_event, 6);
                sets_in_zone01(key_event);
            } else if (HasAny(name, kDoorKw) && !HasAny(name, kDoorSkip)) {
                put(done, shortcut_door, 6);
                sets_in_zone01(shortcut_door);
            } else if (Has(name, "宝箱") || r.Has(MarkerKind::ItemLot) || r.Has(MarkerKind::ItemLotClient)) {
                put(done, item_lot_picked, 5);
            } else if (r.Has(MarkerKind::Cutscene)) {
                put(done, cutscene_seen, 4);
            } else if (HasAny(name, kNpcKw)) {
                put(done, npc_quest, 4);
            }
        }
    }
    const std::vector<PFlag> pflags = ParamFlags(params);
    for (const PFlag& p : pflags) {
        if (p.src == PSrc::ItemLot) {
            put(p.flag, item_lot_picked, 2);
        } else if (p.src == PSrc::ShopEvent) {
            put(p.flag, never_sync, 2);
        }
    }
    // Messenger shop gates that are story / boss progress without an item lot: shared set-only.
    std::map<std::int64_t, bool> lot_flags;
    for (const PFlag& p : pflags) {
        if (p.src == PSrc::ItemLot || p.src == PSrc::ItemLotCumulate) lot_flags[p.flag] = true;
    }
    const std::vector<CatRow>& stat = Structural();
    for (const PFlag& p : pflags) {
        const std::int64_t f = p.flag;
        if (p.src != PSrc::ShopQwc || lot_flags.count(f) || (5500 <= f && f <= 5521) || (6000 <= f && f <= 8999) || f >= 90000000) {
            continue;
        }
        Cat have;
        const auto old = spec_.find(f);
        if (old != spec_.end()) {
            have = old->second.cat;
        } else {
            have = Scan(stat.data(), stat.data() + stat.size(), f);
            if (have == Cat::none) have = never_sync;
        }
        if (have == item_lot_picked || have == never_sync || have == session_runtime) put(f, world_state, 10);
    }
}

Cat FlagCategories::Lookup(std::int64_t f) const {
    Cat c = Scan(std::begin(kCovenantRows), std::end(kCovenantRows), f);
    if (c != Cat::none) return c;
    c = Scan(std::begin(kOverrideRows), std::end(kOverrideRows), f);
    if (c != Cat::none) return c;
    const auto it = spec_.find(f);
    if (it != spec_.end()) return it->second.cat;
    const std::vector<CatRow>& s = Structural();
    return Scan(s.data(), s.data() + s.size(), f);
}

}  // namespace coop::gamedata
