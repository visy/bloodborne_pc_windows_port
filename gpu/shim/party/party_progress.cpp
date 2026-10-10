// SPDX-License-Identifier: GPL-3.0-or-later
// Party co-op C2: host-authoritative progress sync of event flags (see party_progress.h and
// docs/party/event_flags.md). The first half is pure (tables, keys, store walker over injectable
// memory ops, policy, diff, JSON, BBPF) and builds without the game (BB_PARTY_PROGRESS_NO_GAME,
// tests/test_party_progress.cpp); the second half is the game glue (hooks, tick, globals).
#include "party_progress.h"

#include "../net/json.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <stdexcept>

namespace coop::progress {

// =============================================================================================
// categories

namespace {

struct Row {
    u32 first, last;
    Category cat;
    const char* note;
};

const Row kRows[] = {
#include "party_flags.inc"
};
constexpr std::size_t kRowCount = sizeof kRows / sizeof kRows[0];

const char* const kCategoryNames[kCategoryCount] = {
    "never_sync", "boss_defeated", "boss_area",  "lamp_unlocked", "shortcut_door",  "npc_quest",
    "item_lot_picked", "key_event", "cutscene_seen", "world_state", "session_runtime", "time_of_day",
};

// First-match table -> disjoint sorted ranges: sweep the row boundaries; on each elementary
// interval the lowest row index among the covering rows wins.
std::vector<CategoryRange> BuildRanges() {
    std::vector<std::pair<u64, int>> events;  // (position, +row+1 open / -(row+1) close)
    events.reserve(kRowCount * 2);
    for (std::size_t i = 0; i < kRowCount; ++i) {
        events.emplace_back(kRows[i].first, int(i) + 1);
        events.emplace_back(u64(kRows[i].last) + 1, -(int(i) + 1));
    }
    std::sort(events.begin(), events.end());
    std::set<int> active;
    std::vector<CategoryRange> out;
    std::size_t k = 0;
    while (k < events.size()) {
        const u64 pos = events[k].first;
        for (; k < events.size() && events[k].first == pos; ++k) {
            if (events[k].second > 0) {
                active.insert(events[k].second - 1);
            } else {
                active.erase(-events[k].second - 1);
            }
        }
        if (active.empty() || k >= events.size()) {
            continue;
        }
        const u64 end = events[k].first;  // exclusive
        const Category c = kRows[*active.begin()].cat;
        if (!out.empty() && out.back().cat == c && u64(out.back().last) + 1 == pos) {
            out.back().last = u32(end - 1);
        } else {
            out.push_back({u32(pos), u32(end - 1), c});
        }
    }
    return out;
}

} // namespace

const char* CategoryName(Category c) {
    const int i = static_cast<int>(c);
    return i >= 0 && i < kCategoryCount ? kCategoryNames[i] : "?";
}

bool CategoryFromName(const std::string& name, Category* out) {
    for (int i = 0; i < kCategoryCount; ++i) {
        if (name == kCategoryNames[i]) {
            *out = static_cast<Category>(i);
            return true;
        }
    }
    return false;
}

const std::vector<CategoryRange>& CategoryRanges() {
    static const std::vector<CategoryRange> ranges = BuildRanges();
    return ranges;
}

Category CategoryOf(u32 id) {
    const auto& r = CategoryRanges();
    auto it = std::upper_bound(r.begin(), r.end(), id, [](u32 v, const CategoryRange& x) { return v < x.first; });
    if (it == r.begin()) {
        return Category::never_sync;
    }
    --it;
    return id <= it->last ? it->cat : Category::never_sync;
}

Category CategoryOfLinear(u32 id) {
    for (const Row& r : kRows) {
        if (r.first <= id && id <= r.last) {
            return r.cat;
        }
    }
    return Category::never_sync;
}

// =============================================================================================
// group keys (resolve_group_key 0x13bc710; tables at 0x4730bf0, 0x4730c10, 0x47314d0, read from
// the 1.09 eboot)

namespace {
constexpr int kGlobalTypes[5] = {0, 1, 5, 6, 7};
constexpr int kMapTypes[5] = {1, 5, 6, 7, 9};
constexpr int kMaps[17][2] = {{21, 0}, {21, 1}, {22, 0}, {23, 0}, {24, 0}, {24, 1}, {24, 2}, {25, 0}, {26, 0},
                              {27, 0}, {28, 0}, {29, 0}, {32, 0}, {33, 0}, {34, 0}, {35, 0}, {36, 0}};
} // namespace

int GroupKey(int type, int area, int block, int zone) {
    const u32 z = static_cast<u32>(zone);
    if (block == 0 && area == 0) {
        for (int i = 0; i < 5; ++i) {
            if (kGlobalTypes[i] == type) {
                return z < 10 ? i + int(z) * 5 : -1;
            }
        }
        return -1;
    }
    if (area == 99 && block == 0) {
        int idx = -1;  // the game's quirk: an unknown type yields -1 + 0x4b0 + zone * 5
        for (int i = 0; i < 5; ++i) {
            if (kMapTypes[i] == type) {
                idx = i;
                break;
            }
        }
        return idx + 0x4b0 + int(z) * 5;
    }
    int map = -1;
    for (int i = 0; i < 17; ++i) {
        if (kMaps[i][0] == area && kMaps[i][1] == block) {
            map = i;
            break;
        }
    }
    if (area == 40) {
        map = 11;
    } else if (u32(area - 41) < 7) {
        map = area - 24;
    }
    int t = -1;
    for (int i = 0; i < 5; ++i) {
        if (kMapTypes[i] == type) {
            t = i;
            break;
        }
    }
    const int tk = (t >= 0 && z < 10) ? int(z) * 115 + 50 + 23 * t : -1;
    if (map < 23 && map >= 0 && tk >= 0) {
        return tk + map;
    }
    return -1;
}

int PoolKeyOfBlock(u32 block) {
    if (block >= 100000) {
        return -1;
    }
    const int type = int(block / 10000), area = int(block / 100 % 100), mblock = int(block / 10 % 10),
              zone = int(block % 10);
    if (area == 99) {
        return -1;
    }
    const int key = GroupKey(type, area, mblock, zone);
    return key >= 0 && key < int(kPoolBlocks) ? key : -1;
}

namespace {
const std::array<u32, kPoolBlocks>& InverseKeys() {
    static const std::array<u32, kPoolBlocks> inv = [] {
        std::array<u32, kPoolBlocks> a;
        a.fill(0xffffffffu);
        auto put = [&](u32 block) {
            const int k = PoolKeyOfBlock(block);
            if (k >= 0 && a[k] == 0xffffffffu) {
                a[k] = block;
            }
        };
        for (int t : kGlobalTypes) {
            for (u32 z = 0; z < 10; ++z) {
                put(u32(t) * 10000 + z);
            }
        }
        for (int t : kMapTypes) {
            for (const auto& m : kMaps) {
                for (u32 z = 0; z < 10; ++z) {
                    put(u32(t) * 10000 + u32(m[0]) * 100 + u32(m[1]) * 10 + z);
                }
            }
            for (u32 area = 40; area <= 46; ++area) {
                for (u32 z = 0; z < 10; ++z) {
                    put(u32(t) * 10000 + area * 100 + z);
                }
            }
        }
        return a;
    }();
    return inv;
}
} // namespace

u32 BlockOfPoolKey(int key) {
    return key >= 0 && key < int(kPoolBlocks) ? InverseKeys()[key] : 0xffffffffu;
}

bool NeverWritable(u32 id) {
    if (id >= 9020 && id <= 9026) {
        return true;
    }
    const u32 area = id / 100000 % 100;
    if (id >= 10000 && (area == 29 || (area >= 40 && area <= 47) || area == 99)) {
        return true;
    }
    return PoolKeyOfId(id) < 0;
}

// =============================================================================================
// policy

namespace {

struct IdRange {
    u32 first, last;
};

// Ids whose current state follows the host (both 0 and 1): elevators and gates the game itself
// moves back and forth. From the EMEVD scan (flag_tool.py emevd: SetEventFlag state 0 / toggle on
// shortcut_door / world_state ids outside constructors), plus the elevator events 12400147/148
// named in the design doc.
constexpr IdRange kStateIds[] = {
    {12200125, 12200126}, {12400147, 12400148}, {12400150, 12400151}, {12400157, 12400157},
    {12400167, 12400169}, {12400177, 12400178}, {12400827, 12400828}, {12410350, 12410351},
    {12420154, 12420155}, {12500074, 12500074}, {12601250, 12601256}, {12700134, 12700135},
    {12700144, 12700145}, {12700173, 12700174}, {12800610, 12800611}, {13300115, 13300116},
    {13501106, 13501107}, {13501116, 13501117}, {13501126, 13501127}, {13601106, 13601107},
    {62411300, 62411300},
};

// Candidate-category ids that are never synced: the endings (player-owned), flags the game
// clears every load / randomises / counts (value ops: partial sync corrupts them), and boss-fight
// state that sits in a boss_defeated row.
constexpr IdRange kExcluded[] = {
    {21, 23},             // endings A/B/C reached (never_sync rows too; kept as a second guard)
    {2100, 2100},         // area gate flags (0x47304B0 / 0x47301B0): never_sync rows; second guard
    {2410, 2410},
    {9800, 9802},         // time of day: C4 cutscene replay only
    {72100130, 72100131}, // Gehrman's offer -> A ending
    {6000, 8999},         // personal blocks 6-8 (own pool even in load mode 1; item-lot rows there too)
    {9180, 9180},         // first-death cutscene flag, cleared by the preconstructor (common 50)
    {12100000, 12100000}, // ending events (m21)
    {12100002, 12100002},
    {12100122, 12100122}, // Doll key-item check (runtime)
    {12100180, 12100180}, // ending event (m21)
    {12100500, 12100510}, // Doll position (random)
    {12100520, 12100524}, // Doll random talk
    {12104809, 12104809}, // Gehrman fight state (cleared on defeat)
    {12300751, 12300751}, // talk suppression
    {12400733, 12400734}, // value counter (shelter residents)
    {12401000, 12401000}, // warp marker
    {12410490, 12410491}, // hanging corpse runtime
    {12410646, 12410649}, // value counter (fake doctor residents)
    {12500250, 12500279}, // Cainhurst church marks / effect timers
    {12600080, 12600081}, // giant idle (random)
    {13200101, 13200101}, // Patches "went behind" check
    {13500947, 13500947}, // Lady Maria fight state
};

template <std::size_t N>
bool InRanges(const IdRange (&r)[N], u32 id) {
    for (const IdRange& x : r) {
        if (x.first <= id && id <= x.last) {
            return true;
        }
    }
    return false;
}

bool CandidateCategory(Category c) {
    switch (c) {
    case Category::boss_defeated:
    case Category::lamp_unlocked:
    case Category::shortcut_door:
    case Category::key_event:
    case Category::cutscene_seen:
    case Category::world_state:
    case Category::item_lot_picked:
    case Category::npc_quest:
        return true;
    default:
        return false;
    }
}

bool EnvOn(const char* name, bool fallback) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) {
        return fallback;
    }
    return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F' ||
             ((v[0] == 'o' || v[0] == 'O') && (v[1] == 'f' || v[1] == 'F')));
}

} // namespace

Policy Policy::Defaults() {
    Policy p;
    p.mode.fill(SyncMode::Off);
    for (Category c : {Category::boss_defeated, Category::lamp_unlocked, Category::shortcut_door,
                       Category::key_event, Category::cutscene_seen, Category::world_state}) {
        p.mode[static_cast<int>(c)] = SyncMode::SetOnly;
    }
    return p;
}

Policy Policy::FromEnv() {
    Policy p = Defaults();
    if (EnvOn("BB_PARTY_PROGRESS_NPC", false)) {
        p.mode[static_cast<int>(Category::npc_quest)] = SyncMode::SetOnly;
    }
    p.mirror_overlay = EnvOn("BB_PARTY_PROGRESS_MIRROR", false);
    return p;
}

bool HostCandidate(u32 id, Category cat) {
    return CandidateCategory(cat) && !InRanges(kExcluded, id) && !NeverWritable(id);
}

SyncMode ModeFor(const Policy& p, u32 id, Category cat) {
    if (!HostCandidate(id, cat)) {
        return SyncMode::Off;
    }
    if (cat == Category::item_lot_picked) {
        return SyncMode::Off;  // C3 (party_items) replays item pickups; C2 never writes them
    }
    const SyncMode m = p.mode[static_cast<int>(cat)];
    if (m == SyncMode::Off) {
        return m;
    }
    return InRanges(kStateIds, id) ? SyncMode::State : m;
}

namespace {
struct MaskTable {
    std::map<u32, std::array<u8, kBlockBytes>> masks;
    std::vector<u32> blocks;
};
const MaskTable& Masks() {
    static const MaskTable t = [] {
        MaskTable m;
        for (int key = 0; key < int(kPoolBlocks); ++key) {
            const u32 block = BlockOfPoolKey(key);
            if (block == 0xffffffffu) {
                continue;
            }
            std::array<u8, kBlockBytes> mask{};
            bool any = false;
            const u32 base = block * kBitsPerBlock;
            for (const CategoryRange& r : CategoryRanges()) {
                if (r.last < base || r.first >= base + kBitsPerBlock || !CandidateCategory(r.cat)) {
                    continue;
                }
                const u32 lo = std::max(r.first, base), hi = std::min(r.last, base + kBitsPerBlock - 1);
                for (u32 id = lo; id <= hi; ++id) {
                    if (HostCandidate(id, r.cat)) {
                        PutBit(mask.data(), id - base, true);
                        any = true;
                    }
                }
            }
            if (any) {
                m.masks[block] = mask;
                m.blocks.push_back(block);
            }
        }
        std::sort(m.blocks.begin(), m.blocks.end());
        return m;
    }();
    return t;
}
} // namespace

const std::array<u8, kBlockBytes>* CandidateMask(u32 block) {
    const auto& m = Masks().masks;
    const auto it = m.find(block);
    return it == m.end() ? nullptr : &it->second;
}

const std::vector<u32>& CandidateBlocks() {
    return Masks().blocks;
}

// =============================================================================================
// host tracker, guest cursor, snapshot

bool FlagSnapshot::Get(u32 id, bool* known) const {
    const auto it = blocks.find(id / kBitsPerBlock);
    if (it == blocks.end()) {
        if (known) {
            *known = false;
        }
        return false;
    }
    if (known) {
        *known = true;
    }
    return std::binary_search(it->second.begin(), it->second.end(), id % kBitsPerBlock);
}

void HostTracker::ObserveBlock(u32 block, const u8* data, std::vector<FlagChange>* out) {
    const auto* mask = CandidateMask(block);
    if (!mask) {
        return;
    }
    std::array<u8, kBlockBytes> now;
    for (u32 i = 0; i < kBlockBytes; ++i) {
        now[i] = static_cast<u8>(data[i] & (*mask)[i]);
    }
    auto it = shadow_.find(block);
    if (it == shadow_.end()) {
        if (baseline_ && out) {
            for (u32 b = 0; b < kBitsPerBlock; ++b) {
                if (GetBit(now.data(), b)) {
                    const u32 id = block * kBitsPerBlock + b;
                    out->push_back({id, true, CategoryOf(id), ++seq_});
                }
            }
        }
        shadow_.emplace(block, now);
        return;
    }
    std::array<u8, kBlockBytes>& old = it->second;
    std::vector<FlagChange> found;  // ascending ids (bytes ascending, bits MSB first)
    for (u32 i = 0; i < kBlockBytes; ++i) {
        const u8 diff = static_cast<u8>(old[i] ^ now[i]);
        for (u32 k = 0; diff && k < 8; ++k) {
            const u32 b = i * 8 + k;
            if ((diff & (0x80u >> k)) && b < kBitsPerBlock) {
                const u32 id = block * kBitsPerBlock + b;
                found.push_back({id, GetBit(now.data(), b), CategoryOf(id), 0});
            }
        }
    }
    old = now;
    if (out) {
        for (FlagChange& c : found) {
            c.seq = ++seq_;
            out->push_back(c);
        }
    }
}

void HostTracker::ObserveId(u32 id, bool value, std::vector<FlagChange>* out) {
    const u32 block = id / kBitsPerBlock, bit = id % kBitsPerBlock;
    const auto* mask = CandidateMask(block);
    if (!mask || !GetBit(mask->data(), bit)) {
        return;
    }
    auto it = shadow_.find(block);
    if (it == shadow_.end()) {
        return;  // the next scan takes the block
    }
    if (GetBit(it->second.data(), bit) == value) {
        return;
    }
    PutBit(it->second.data(), bit, value);
    if (out) {
        out->push_back({id, value, CategoryOf(id), ++seq_});
    }
}

FlagSnapshot HostTracker::Snapshot(u32 map_id) const {
    FlagSnapshot s;
    s.epoch = epoch_;
    s.seq = seq_;
    s.map_id = map_id;
    for (const auto& [block, data] : shadow_) {
        std::vector<u32>& bits = s.blocks[block];
        for (u32 b = 0; b < kBitsPerBlock; ++b) {
            if (GetBit(data.data(), b)) {
                bits.push_back(b);
            }
        }
    }
    return s;
}

std::vector<FlagChange> GuestCursor::Filter(u64 epoch, const std::vector<FlagChange>& in) {
    if (!any_ || epoch != epoch_) {
        epoch_ = epoch;
        last_ = 0;
        any_ = true;
    }
    std::vector<FlagChange> out;
    for (const FlagChange& c : in) {
        if (c.seq > last_) {
            out.push_back(c);
            last_ = c.seq;
        }
    }
    return out;
}

void GuestCursor::Reset(u64 epoch, u64 seq) {
    epoch_ = epoch;
    last_ = seq;
    any_ = true;
}

// =============================================================================================
// JSON

std::string ChangesToJson(u64 epoch, const std::vector<FlagChange>& changes) {
    json::Value v = json::Value::make_object();
    v.set("v", 1);
    v.set("epoch", json::hex(epoch));
    json::Value arr = json::Value::make_array();
    for (const FlagChange& c : changes) {
        json::Value e = json::Value::make_array();
        e.push(static_cast<unsigned>(c.id));
        e.push(c.value ? 1 : 0);
        e.push(CategoryName(c.cat));
        e.push(static_cast<unsigned long long>(c.seq));
        arr.push(std::move(e));
    }
    v.set("c", std::move(arr));
    return json::dump(v, 0);
}

bool ChangesFromJson(const std::string& text, u64* epoch, std::vector<FlagChange>* out, std::string* error) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err)) {
        if (error) {
            *error = err;
        }
        return false;
    }
    try {
        if (json::u32(v, "v") != 1) {
            throw std::runtime_error("unsupported version");
        }
        *epoch = json::as_u64(json::member(v, "epoch"), "epoch");
        out->clear();
        if (json::arr(v, "c").size() > kMaxChangesPerEvent) {
            throw std::runtime_error("too many changes");
        }
        for (const json::Value& e : json::arr(v, "c")) {
            if (e.type != json::Value::Type::Array || e.array.size() != 4) {
                throw std::runtime_error("bad change entry");
            }
            FlagChange c;
            const u64 id = json::as_u64(e.array[0], "id");
            if (id > 0xffffffffull) {
                throw std::runtime_error("id out of range");
            }
            c.id = static_cast<u32>(id);
            c.value = json::as_u64(e.array[1], "value") != 0;
            if (e.array[2].type != json::Value::Type::String || !CategoryFromName(e.array[2].string, &c.cat)) {
                throw std::runtime_error("bad category");
            }
            c.seq = json::as_u64(e.array[3], "seq");
            out->push_back(c);
        }
    } catch (const std::exception& ex) {
        if (error) {
            *error = ex.what();
        }
        return false;
    }
    return true;
}

std::string SnapshotToJson(const FlagSnapshot& s) {
    json::Value v = json::Value::make_object();
    v.set("v", 1);
    v.set("epoch", json::hex(s.epoch));
    v.set("seq", static_cast<unsigned long long>(s.seq));
    v.set("map", static_cast<unsigned>(s.map_id));
    json::Value blocks = json::Value::make_array();
    for (const auto& [block, bits] : s.blocks) {
        json::Value b = json::Value::make_array();  // [block, [set bits...]]
        b.push(static_cast<unsigned>(block));
        json::Value set = json::Value::make_array();
        for (u32 x : bits) {
            set.push(static_cast<unsigned>(x));
        }
        b.push(std::move(set));
        blocks.push(std::move(b));
    }
    v.set("b", std::move(blocks));
    return json::dump(v, 0);
}

bool SnapshotFromJson(const std::string& text, FlagSnapshot* out, std::string* error) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err)) {
        if (error) {
            *error = err;
        }
        return false;
    }
    try {
        if (json::u32(v, "v") != 1) {
            throw std::runtime_error("unsupported version");
        }
        FlagSnapshot s;
        s.epoch = json::as_u64(json::member(v, "epoch"), "epoch");
        s.seq = json::u64(v, "seq");
        s.map_id = json::u32(v, "map");
        if (json::arr(v, "b").size() > kMaxSnapshotBlocks) {
            throw std::runtime_error("too many blocks");
        }
        for (const json::Value& b : json::arr(v, "b")) {
            if (b.type != json::Value::Type::Array || b.array.size() != 2 ||
                b.array[1].type != json::Value::Type::Array) {
                throw std::runtime_error("bad block entry");
            }
            const u64 block64 = json::as_u64(b.array[0], "block");
            if (block64 > 0xffffffffull || b.array[1].array.size() > kBitsPerBlock) {
                throw std::runtime_error("bad block");
            }
            const u32 block = static_cast<u32>(block64);
            std::vector<u32>& bits = s.blocks[block];
            for (const json::Value& x : b.array[1].array) {
                const u64 bit = json::as_u64(x, "bit");
                if (bit >= kBitsPerBlock) {
                    throw std::runtime_error("bit out of range");
                }
                bits.push_back(static_cast<u32>(bit));
            }
            std::sort(bits.begin(), bits.end());
            bits.erase(std::unique(bits.begin(), bits.end()), bits.end());
        }
        *out = std::move(s);
    } catch (const std::exception& ex) {
        if (error) {
            *error = ex.what();
        }
        return false;
    }
    return true;
}

// =============================================================================================
// the store

namespace {
template <class T>
bool Rd(const MemOps& ops, u64 at, T* v) {
    return at && ops.read && ops.read(at, v, sizeof(T));
}
} // namespace

FlagStore::FlagStore(u64 manager, const MemOps& ops) : man_(manager), ops_(ops) {
    if (!man_ || !ops_.read) {
        return;
    }
    u32 bits = 0, stride = 0, count = 0;
    std::int32_t mode = -1;
    if (!Rd(ops_, man_ + man::kBits, &bits) || !Rd(ops_, man_ + man::kStride, &stride) ||
        !Rd(ops_, man_ + man::kCount, &count) || !Rd(ops_, man_ + man::kPool, &pool_) ||
        !Rd(ops_, man_ + man::kTreeHead, &head_) || !Rd(ops_, man_ + man::kLoadMode, &mode)) {
        return;
    }
    if (bits != kBitsPerBlock || stride != kBlockBytes || count != kPoolBlocks || !pool_ || !head_ ||
        (mode != 0 && mode != 1)) {
        return;
    }
    mode_ = mode;
    valid_ = true;
}

namespace {
struct NodeView {
    u64 left = 0, right = 0;
    u8 is_nil = 1;
    u32 key = 0;
    std::int32_t kind = 0;
    u64 storage = 0;
};
bool ReadNode(const MemOps& ops, u64 n, NodeView* v) {
    return Rd(ops, n + man::kNodeLeft, &v->left) && Rd(ops, n + man::kNodeRight, &v->right) &&
           Rd(ops, n + man::kNodeIsNil, &v->is_nil) && (v->is_nil || (Rd(ops, n + man::kNodeKey, &v->key) &&
                                                                      Rd(ops, n + man::kNodeKind, &v->kind) &&
                                                                      Rd(ops, n + man::kNodeStorage, &v->storage)));
}
} // namespace

namespace {
BlockRef MakeRef(u64 pool, u32 block, const NodeView& v) {
    BlockRef r;
    r.block = block;
    r.kind = v.kind;
    if (v.kind == 2) {
        r.data = v.storage;
    } else if (v.kind == 1) {
        // as the game: (u32)(stride * index) + pool
        r.data = pool + u64(u32(kBlockBytes * u32(v.storage)));
    }
    return r;
}
} // namespace

bool FlagStore::FindBlock(u32 block, BlockRef* out) const {
    if (!valid_) {
        return false;
    }
    u64 root = 0;
    if (!Rd(ops_, head_ + man::kNodeParent, &root)) {
        return false;
    }
    u64 cand = head_, n = root;
    NodeView cv;
    for (int depth = 0; depth < 96; ++depth) {
        NodeView v;
        if (!ReadNode(ops_, n, &v)) {
            return false;
        }
        if (v.is_nil) {
            break;
        }
        if (block <= v.key) {
            cand = n;
            cv = v;
            n = v.left;
        } else {
            n = v.right;
        }
    }
    if (cand == head_ || cv.key != block) {
        return false;
    }
    *out = MakeRef(pool_, block, cv);
    return out->data != 0;
}

bool FlagStore::ForEachBlock(const std::function<void(const BlockRef&)>& fn, std::size_t max_nodes) const {
    if (!valid_) {
        return false;
    }
    u64 n = 0;
    if (!Rd(ops_, head_ + man::kNodeParent, &n)) {
        return false;
    }
    std::vector<u64> stack;
    std::size_t visited = 0;
    bool have_prev = false;
    u32 prev = 0;
    for (;;) {
        for (;;) {
            NodeView v;
            if (!ReadNode(ops_, n, &v)) {
                return false;
            }
            if (v.is_nil) {
                break;
            }
            if (stack.size() > 96) {
                return false;
            }
            stack.push_back(n);
            n = v.left;
        }
        if (stack.empty()) {
            return true;
        }
        n = stack.back();
        stack.pop_back();
        NodeView v;
        if (!ReadNode(ops_, n, &v) || ++visited > max_nodes || (have_prev && v.key <= prev)) {
            return false;
        }
        have_prev = true;
        prev = v.key;
        const BlockRef r = MakeRef(pool_, v.key, v);
        if (r.data) {
            fn(r);
        }
        n = v.right;
    }
}

bool FlagStore::ReadBlockBytes(const BlockRef& r, u8* out) const {
    return r.data && ops_.read && ops_.read(r.data, out, kBlockBytes);
}

bool FlagStore::ReadVisible(u32 id, bool* value) const {
    BlockRef r;
    if (!FindBlock(id / kBitsPerBlock, &r)) {
        return false;
    }
    const u32 bit = id % kBitsPerBlock;
    u8 byte = 0;
    if (!Rd(ops_, r.data + (bit >> 3), &byte)) {
        return false;
    }
    *value = (byte & (0x80u >> (bit & 7))) != 0;
    return true;
}

u64 FlagStore::PoolBlockAddress(int key) const {
    if (!valid_ || key < 0 || key >= int(kPoolBlocks)) {
        return 0;
    }
    return pool_ + u64(key) * kBlockBytes;
}

bool FlagStore::ReadOwnSave(u32 id, bool* value) const {
    const u64 at = PoolBlockAddress(PoolKeyOfId(id));
    if (!at) {
        return false;
    }
    const u32 bit = id % kBitsPerBlock;
    u8 byte = 0;
    if (!Rd(ops_, at + (bit >> 3), &byte)) {
        return false;
    }
    *value = (byte & (0x80u >> (bit & 7))) != 0;
    return true;
}

bool FlagStore::WritePoolBit(u32 id, bool value) const {
    if (NeverWritable(id) || !ops_.write) {
        return false;
    }
    const u64 at = PoolBlockAddress(PoolKeyOfId(id));
    if (!at) {
        return false;
    }
    const u32 bit = id % kBitsPerBlock;
    u8 byte = 0;
    if (!Rd(ops_, at + (bit >> 3), &byte)) {
        return false;
    }
    PutBit(&byte, bit & 7, value);
    return ops_.write(at + (bit >> 3), &byte, 1);
}

bool FlagStore::WriteVisible(u32 id, bool value) const {
    if (NeverWritable(id)) {
        return false;
    }
    BlockRef r;
    if (!FindBlock(id / kBitsPerBlock, &r)) {
        return false;  // the game's own setter is a no-op for an absent block too
    }
    if (ops_.set_event_flag) {
        ops_.set_event_flag(man_, id, value ? 1 : 0);
    } else {
        const u32 bit = id % kBitsPerBlock;
        u8 byte = 0;
        if (!ops_.write || !Rd(ops_, r.data + (bit >> 3), &byte)) {
            return false;
        }
        PutBit(&byte, bit & 7, value);
        if (!ops_.write(r.data + (bit >> 3), &byte, 1)) {
            return false;
        }
    }
    bool now = !value;
    return ReadVisible(id, &now) && now == value;
}

bool FlagStore::WriteOwnSave(u32 id, bool value) const {
    if (!valid_ || NeverWritable(id)) {
        return false;
    }
    if (mode_ == 1) {
        return WritePoolBit(id, value);
    }
    BlockRef r;
    if (FindBlock(id / kBitsPerBlock, &r) && r.kind == 1) {
        return WriteVisible(id, value);
    }
    return WritePoolBit(id, value);
}

// =============================================================================================
// guest applier

void GuestApplier::QueueChanges(u64 epoch, const std::vector<FlagChange>& changes) {
    n_.received += changes.size();
    std::vector<FlagChange> fresh = cursor_.Filter(epoch, changes);
    n_.dropped_seq += changes.size() - fresh.size();
    if (!fresh.empty()) {
        Item it;
        it.changes = std::move(fresh);
        items_.push_back(std::move(it));
    }
    Trim();
}

void GuestApplier::QueueSnapshot(const FlagSnapshot& snap) {
    cursor_.Reset(snap.epoch, snap.seq);
    // A newer snapshot supersedes a queued one (a host's stream of them must not pile up).
    items_.erase(std::remove_if(items_.begin(), items_.end(), [](const Item& i) { return i.snapshot; }), items_.end());
    Item it;
    it.snapshot = true;
    it.snap = snap;
    items_.push_back(std::move(it));
    Trim();
}

void GuestApplier::Trim() {
    // Bounded while the store cannot take them (mode 0 outside the post-load window).
    if (items_.size() > kMaxPendingItems) {
        n_.dropped_seq += items_.size() - kMaxPendingItems;
        items_.erase(items_.begin(), items_.begin() + static_cast<std::ptrdiff_t>(items_.size() - kMaxPendingItems));
    }
}

bool GuestApplier::ApplyOne(const FlagStore& store, const Policy& policy, u32 id, bool value) {
    const SyncMode m = ModeFor(policy, id);
    if (m == SyncMode::Off || (m == SyncMode::SetOnly && !value)) {
        ++n_.skipped;
        return false;
    }
    bool cur = false;
    if (store.ReadOwnSave(id, &cur) && cur == value) {
        ++n_.already;
    } else if (store.WriteOwnSave(id, value)) {
        ++n_.applied;
    } else {
        ++n_.failed;
        return false;
    }
    if (store.LoadMode() == 1 && policy.mirror_overlay && m == SyncMode::SetOnly) {
        BlockRef r;
        bool vis = false;
        if (store.FindBlock(id / kBitsPerBlock, &r) && r.kind == 2 && store.ReadVisible(id, &vis) && !vis) {
            store.WriteVisible(id, true);
        }
    }
    return true;
}

std::size_t GuestApplier::Apply(const FlagStore& store, const Policy& policy, bool allow_mode0) {
    if (!store.Valid() || items_.empty() || (store.LoadMode() == 0 && !allow_mode0)) {
        return 0;
    }
    const u64 before = n_.applied;
    for (const Item& it : items_) {
        if (!it.snapshot) {
            for (const FlagChange& c : it.changes) {
                ApplyOne(store, policy, c.id, c.value);
            }
            continue;
        }
        for (const auto& [block, bits] : it.snap.blocks) {
            const auto* mask = CandidateMask(block);
            if (!mask) {
                continue;
            }
            std::array<u8, kBlockBytes> set{};
            for (u32 b : bits) {
                if (b < kBitsPerBlock) {
                    PutBit(set.data(), b, true);
                }
            }
            for (u32 b = 0; b < kBitsPerBlock; ++b) {
                if (!GetBit(mask->data(), b)) {
                    continue;
                }
                const u32 id = block * kBitsPerBlock + b;
                const bool v = GetBit(set.data(), b);
                const SyncMode m = ModeFor(policy, id);
                if (m == SyncMode::State || (m == SyncMode::SetOnly && v)) {
                    ApplyOne(store, policy, id, v);
                }
            }
        }
    }
    items_.clear();
    return std::size_t(n_.applied - before);
}

// =============================================================================================
// BBPF v1

namespace {
void PutLe(std::vector<u8>& out, u64 v, int n) {
    for (int i = 0; i < n; ++i) {
        out.push_back(static_cast<u8>(v >> (8 * i)));
    }
}
u64 GetLe(const u8* p, int n) {
    u64 v = 0;
    for (int i = 0; i < n; ++i) {
        v |= u64(p[i]) << (8 * i);
    }
    return v;
}
} // namespace

std::vector<u8> BbpfEncode(const BbpfHeader& h, const std::vector<BbpfBlock>& blocks_in) {
    std::vector<BbpfBlock> blocks = blocks_in;
    std::sort(blocks.begin(), blocks.end(), [](const BbpfBlock& a, const BbpfBlock& b) { return a.block < b.block; });
    std::vector<u8> out;
    out.reserve(64 + blocks.size() * (8 + kBlockBytes));
    out.insert(out.end(), {'B', 'B', 'P', 'F'});
    PutLe(out, 1, 2);
    PutLe(out, 64, 2);
    PutLe(out, kBitsPerBlock, 4);
    PutLe(out, blocks.size(), 4);
    PutLe(out, h.map_id, 4);
    PutLe(out, u32(h.load_mode), 4);
    PutLe(out, u32(h.role), 4);
    PutLe(out, 0, 4);
    PutLe(out, h.unix_ms, 8);
    PutLe(out, h.frame, 8);
    for (std::size_t i = 0; i < 16; ++i) {
        out.push_back(i < h.label.size() ? static_cast<u8>(h.label[i]) : 0);
    }
    for (const BbpfBlock& b : blocks) {
        PutLe(out, b.block, 4);
        PutLe(out, kBlockBytes, 2);
        out.push_back(b.kind);
        out.push_back(0);
        out.insert(out.end(), b.data.begin(), b.data.end());
    }
    return out;
}

bool BbpfDecode(const std::vector<u8>& b, BbpfHeader* h, std::vector<BbpfBlock>* blocks) {
    if (b.size() < 64 || std::memcmp(b.data(), "BBPF", 4) != 0 || GetLe(&b[4], 2) != 1) {
        return false;
    }
    const std::size_t hsz = std::size_t(GetLe(&b[6], 2));
    if (GetLe(&b[8], 4) != kBitsPerBlock || hsz < 64) {
        return false;
    }
    const u32 n = u32(GetLe(&b[0x0c], 4));
    h->map_id = u32(GetLe(&b[0x10], 4));
    h->load_mode = std::int32_t(u32(GetLe(&b[0x14], 4)));
    h->role = std::int32_t(u32(GetLe(&b[0x18], 4)));
    h->unix_ms = GetLe(&b[0x20], 8);
    h->frame = GetLe(&b[0x28], 8);
    h->label.assign(reinterpret_cast<const char*>(&b[0x30]), strnlen(reinterpret_cast<const char*>(&b[0x30]), 16));
    blocks->clear();
    std::size_t pos = hsz;
    for (u32 i = 0; i < n; ++i) {
        if (pos + 8 > b.size()) {
            return false;
        }
        BbpfBlock blk;
        blk.block = u32(GetLe(&b[pos], 4));
        const std::size_t len = std::size_t(GetLe(&b[pos + 4], 2));
        blk.kind = b[pos + 6];
        pos += 8;
        if (len != kBlockBytes || pos + len > b.size()) {
            return false;
        }
        std::memcpy(blk.data.data(), &b[pos], len);
        pos += len;
        blocks->push_back(blk);
    }
    return true;
}

bool BbpfWriteFile(const std::string& path, const BbpfHeader& h, const std::vector<BbpfBlock>& blocks) {
    const std::vector<u8> bytes = BbpfEncode(h, blocks);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok;
}

std::vector<BbpfBlock> CollectVisible(const FlagStore& s) {
    std::vector<BbpfBlock> out;
    s.ForEachBlock([&](const BlockRef& r) {
        BbpfBlock b;
        b.block = r.block;
        b.kind = static_cast<u8>(r.kind);
        if (s.ReadBlockBytes(r, b.data.data())) {
            out.push_back(b);
        }
    });
    return out;
}

std::vector<BbpfBlock> CollectPool(const FlagStore& s) {
    std::vector<BbpfBlock> out;
    BlockRef r;
    for (int key = 0; key < int(kPoolBlocks); ++key) {
        const u32 block = BlockOfPoolKey(key);
        if (block == 0xffffffffu) {
            continue;
        }
        r.block = block;
        r.kind = 1;
        r.data = s.PoolBlockAddress(key);
        BbpfBlock b;
        b.block = block;
        b.kind = 1;
        if (s.ReadBlockBytes(r, b.data.data())) {
            out.push_back(b);
        }
    }
    std::sort(out.begin(), out.end(), [](const BbpfBlock& a, const BbpfBlock& b) { return a.block < b.block; });
    return out;
}

} // namespace coop::progress

#ifndef BB_PARTY_PROGRESS_NO_GAME
// =============================================================================================
// the game side

#include "coop_hooks.h"
#include "game_state.h"
#include "../net/bbnet_internal.h"

#include <filesystem>
#include <random>

namespace coop::progress {
namespace {

// SprjEventFlagMan singleton slot; hooked functions (our offsets, prologues checked in the 1.09
// eboot with capstone):
constexpr u64 kEventFlagManSlot = 0x553b100;
// SetEventFlag(man, id, on): mov r8d, edx; mov ecx, [rdi+0x1c]  (41 89 d0 / 8b 4f 1c)
constexpr u64 kSetEventFlag = 0x13cfcc0;
// SetEventFlagValue(man, first, bits, v): push rbp; push r15; push r14  (55 / 41 57 / 41 56)
constexpr u64 kSetEventFlagValue = 0x13d0060;
// Lua CompleteEvent = BroadcastSetFlag(L?, id): push rbp; mov rbp, rsp; push r15
constexpr u64 kBroadcastSetFlag = 0x132aad0;
// SprjEmkEventIns::EndOrRestart(this, restart): push rbp; mov rbp, rsp; push r15. Sets flag
// (event id u32 +0x28) + (slot s16 +0x2c) when the sum > 98, set-only, both paths. The design
// doc's 0x16ed100 is not an instruction boundary; this is the function its decompile describes.
constexpr u64 kEmkEndOrRestart = 0x12ed100;
constexpr u64 kEmkEventId = 0x28, kEmkSlot = 0x2c;

constexpr std::size_t kMaxHookIds = 1 << 16;
constexpr u64 kStableFrames = 30;         // world up, no loading, same store for this long
constexpr u64 kMode0Window = 600;         // frames after that in which mode 0 applies
constexpr double kDumpMinGap = 2.0;       // seconds between sync dumps

using Clock = std::chrono::steady_clock;

void Log(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::printf("Party progress: %s\n", line);
    std::fflush(stdout);
}

struct Global {
    std::once_flag init_once;
    std::atomic<bool> enabled{false};
    std::atomic<bool> dump{false};
    std::atomic<int> role{0};  // Role
    std::atomic<bool> capture{false};
    std::atomic<ItemFlagObserver> item_observer{nullptr};
    thread_local static bool self_write;

    // hooks -> tick (any thread)
    std::mutex hook_mu;
    std::vector<u32> hook_ids;
    std::atomic<bool> hook_overflow{false};
    std::atomic<u64> hook_hits{0}, hook_candidates{0};

    // host state (tracker under mu; the tick and FullSnapshot)
    std::mutex mu;
    HostTracker tracker;
    std::vector<FlagChange> outbox;
    u32 map_id = 0xffffffffu;
    u64 scans = 0, host_changes = 0;

    // guest state (under mu)
    GuestApplier applier;
    Policy policy;

    // main thread only
    u64 frame = 0, stable = 0;
    u64 last_man = 0;
    int last_mode = -2;
    Clock::time_point last_scan{}, last_dump{};
    bool dump_session = false;   // a session boundary dump is due
    bool dump_sync = false;      // a sync dump is due
    std::string label = "party";
    std::string dump_dir;
    int scan_ms = 1000;
};
thread_local bool Global::self_write = false;

Global& G() {
    static Global g;
    return g;
}

u64 NewEpoch() {
    std::random_device rd;
    const u64 t = u64(std::chrono::system_clock::now().time_since_epoch().count());
    return (u64(rd()) << 32 ^ u64(rd())) ^ t;
}

void CallSetEventFlag(u64 man, u32 id, int on) {
    using Fn = void(BB_COOP_SYSV*)(u64, u32, int);
    const u64 fn = Guest(kSetEventFlag);
    if (!fn) {
        return;
    }
    Global::self_write = true;
    reinterpret_cast<Fn>(fn)(man, id, on);
    Global::self_write = false;
}

void SetFlagThunk(u64 man, u32 id, int on) {
    CallSetEventFlag(man, id, on);
}

MemOps GameOps() {
    MemOps ops;
    ops.read = [](u64 a, void* o, std::size_t n) { return SafeRead(a, o, n); };
    ops.write = [](u64 a, const void* i, std::size_t n) { return SafeWrite(a, i, n); };
    ops.set_event_flag = &SetFlagThunk;
    return ops;
}

u64 Manager() {
    u64 v = 0;
    const u64 at = Guest(kEventFlagManSlot);
    return at && SafeGet(at, &v) ? v : 0;
}

void NoteId(u32 id) {
    Global& g = G();
    g.hook_hits.fetch_add(1, std::memory_order_relaxed);
    if (!g.capture.load(std::memory_order_relaxed) || Global::self_write || !HostCandidate(id)) {
        return;
    }
    g.hook_candidates.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g.hook_mu);
    if (g.hook_ids.size() >= kMaxHookIds) {
        g.hook_overflow = true;  // the next scan catches them
        return;
    }
    g.hook_ids.push_back(id);
}

bool Capturing() {
    return G().capture.load(std::memory_order_relaxed) && !Global::self_write;
}

BB_COOP_SYSV void OnSetEventFlag(u64 man, u64 id, u64, u64, u64, u64) {
    if (Capturing() && man == Manager()) {
        NoteId(u32(id));
    }
}

BB_COOP_SYSV void OnSetEventFlagValue(u64 man, u64 first, u64 bits, u64, u64, u64) {
    if (!Capturing() || man != Manager() || u32(bits) == 0 || u32(bits) > 32) {
        return;
    }
    for (u32 k = 0; k < u32(bits); ++k) {
        NoteId(u32(first) + k);
    }
}

BB_COOP_SYSV void OnBroadcastSetFlag(u64, u64 id, u64, u64, u64, u64) {
    if (Capturing() && std::int32_t(u32(id)) >= 0) {
        NoteId(u32(id));
    }
}

BB_COOP_SYSV void OnEmkEndOrRestart(u64 self, u64, u64, u64, u64, u64) {
    if (!Capturing()) {
        return;
    }
    u32 event = 0;
    std::int16_t slot = 0;
    if (!SafeGet(self + kEmkEventId, &event) || !SafeGet(self + kEmkSlot, &slot)) {
        return;
    }
    const std::int32_t id = std::int32_t(event) + slot;
    if ((std::int32_t(event) | slot) >= 0 && id > 0x62) {
        NoteId(u32(id));
    }
}

std::string DefaultDumpDir() {
    std::string base = bbnet::settings().user_dir;
    if (base.empty()) {
        if (const char* u = std::getenv("BB_GPU_USER_DIR")) {
            base = u;
        } else if (const char* u2 = std::getenv("BB_USER_DIR")) {
            base = u2;
        } else {
            const char* d = std::getenv("BB_DATA_DIR");
            base = std::string(d && d[0] ? d : ".") + "/user";
        }
    }
    return base + "/party";
}

void WriteDumps(Global& g, const FlagStore& store, const GameSnapshot& s) {
    std::error_code ec;
    if (g.dump_dir.empty()) {
        g.dump_dir = DefaultDumpDir();
    }
    std::filesystem::create_directories(g.dump_dir, ec);
    BbpfHeader h;
    h.map_id = s.map_id;
    h.load_mode = store.LoadMode();
    h.role = s.session_role;
    h.unix_ms = u64(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
    h.frame = g.frame;
    for (int pass = 0; pass < 2; ++pass) {
        h.label = pass ? (g.label + "-pool").substr(0, 16) : g.label.substr(0, 16);
        const std::vector<BbpfBlock> blocks = pass ? CollectPool(store) : CollectVisible(store);
        char name[96];
        std::snprintf(name, sizeof name, "/flags_%s_%llu.bbpf", h.label.c_str(), static_cast<unsigned long long>(g.frame));
        const std::string path = g.dump_dir + name;
        if (BbpfWriteFile(path, h, blocks)) {
            Log("dump %s (%zu blocks, mode %d)", path.c_str(), blocks.size(), h.load_mode);
        } else {
            Log("dump %s failed", path.c_str());
        }
    }
    g.last_dump = Clock::now();
}

void HostTick(Global& g, const FlagStore& store, const GameSnapshot& s, const Clock::time_point now) {
    std::vector<u32> ids;
    {
        std::lock_guard<std::mutex> lk(g.hook_mu);
        ids.swap(g.hook_ids);
    }
    const bool overflow = g.hook_overflow.exchange(false);
    const bool scan_due = overflow || g.scans == 0 ||
                          std::chrono::duration<double, std::milli>(now - g.last_scan).count() >= g.scan_ms ||
                          g.stable == kStableFrames;  // right after a load
    std::vector<FlagChange> changes;
    std::vector<std::pair<u32, std::array<u8, kBlockBytes>>> blocks;
    if (scan_due) {
        const auto& cands = CandidateBlocks();
        store.ForEachBlock([&](const BlockRef& r) {
            if (!std::binary_search(cands.begin(), cands.end(), r.block)) {
                return;
            }
            std::array<u8, kBlockBytes> data;
            if (store.ReadBlockBytes(r, data.data())) {
                blocks.emplace_back(r.block, data);
            }
        });
    }
    std::vector<std::pair<u32, bool>> values;
    values.reserve(ids.size());
    for (u32 id : ids) {
        bool v = false;
        if (store.ReadVisible(id, &v)) {
            values.emplace_back(id, v);
        }
    }
    std::unique_lock<std::mutex> lk(g.mu);
    const bool baseline = g.tracker.has_baseline();
    for (const auto& [id, v] : values) {
        g.tracker.ObserveId(id, v, &changes);
    }
    if (scan_due) {
        for (const auto& [block, data] : blocks) {
            g.tracker.ObserveBlock(block, data.data(), &changes);
        }
        g.tracker.EndScan();
        g.last_scan = now;
        ++g.scans;
        if (!baseline) {
            Log("host baseline: %zu candidate blocks in the store (epoch %016llx)", blocks.size(),
                static_cast<unsigned long long>(g.tracker.epoch()));
        }
    }
    g.map_id = s.map_id;
    if (!changes.empty()) {
        g.host_changes += changes.size();
        for (const FlagChange& c : changes) {
            if (c.cat != Category::item_lot_picked) {
                Log("host flag %u = %d (%s) seq %llu", c.id, int(c.value), CategoryName(c.cat),
                    static_cast<unsigned long long>(c.seq));
            }
        }
        g.outbox.insert(g.outbox.end(), changes.begin(), changes.end());
        g.dump_sync = true;
    }
    lk.unlock();
    if (const ItemFlagObserver obs = g.item_observer.load()) {
        for (const FlagChange& c : changes) {
            if (c.cat == Category::item_lot_picked && c.value) {
                obs(c.id);
            }
        }
    }
}

void GuestTick(Global& g, const FlagStore& store) {
    const bool mode0_window = g.stable >= kStableFrames && g.stable < kStableFrames + kMode0Window;
    std::lock_guard<std::mutex> lk(g.mu);
    const std::size_t pending = g.applier.Pending();
    if (!pending) {
        return;
    }
    const auto before = g.applier.counters();
    const std::size_t n = g.applier.Apply(store, g.policy, mode0_window);
    if (g.applier.Pending() == pending) {
        return;  // kept for later (mode 0 outside the post-load window)
    }
    const auto& c = g.applier.counters();
    Log("guest applied %zu flags in load mode %d (already %llu, skipped %llu, failed %llu; cursor %llu)", n,
        store.LoadMode(), static_cast<unsigned long long>(c.already - before.already),
        static_cast<unsigned long long>(c.skipped - before.skipped),
        static_cast<unsigned long long>(c.failed - before.failed),
        static_cast<unsigned long long>(g.applier.cursor().last_seq()));
    g.dump_sync = true;
}

} // namespace

void Init() {
    Global& g = G();
    std::call_once(g.init_once, [&g] {
        g.enabled = EnvOn("BB_PARTY_PROGRESS", true);
        g.dump = EnvOn("BB_PARTY_FLAG_DUMP", false);
        if (const char* v = std::getenv("BB_PARTY_PROGRESS_SCAN_MS")) {
            g.scan_ms = std::max(100, std::atoi(v));
        }
        g.policy = Policy::FromEnv();
        g.tracker = HostTracker(NewEpoch());
        if (!g.enabled) {
            Log("off (BB_PARTY_PROGRESS=0)");
            return;
        }
        const auto t0 = Clock::now();
        const std::size_t ranges = CategoryRanges().size();
        const std::size_t blocks = CandidateBlocks().size();
        Log("on: %zu category ranges, %zu candidate blocks (%.0f ms); items via %s, npc %s, mirror %s, dump %s",
            ranges, blocks, std::chrono::duration<double, std::milli>(Clock::now() - t0).count(),
            "C3",
            g.policy.mode[int(Category::npc_quest)] != SyncMode::Off ? "on" : "off",
            g.policy.mirror_overlay ? "on" : "off", g.dump ? "on" : "off");
    });
}

bool Enabled() {
    Init();
    return G().enabled;
}

int InstallHooks() {
    if (!Enabled()) {
        return 0;
    }
    int n = 0;
    n += HookPrologue(kSetEventFlag, {0x41, 0x89, 0xd0, 0x8b, 0x4f, 0x1c}, &OnSetEventFlag,
                      "progress: SetEventFlag") ? 1 : 0;
    n += HookPrologue(kSetEventFlagValue, {0x55, 0x41, 0x57, 0x41, 0x56}, &OnSetEventFlagValue,
                      "progress: SetEventFlagValue") ? 1 : 0;
    n += HookPrologue(kBroadcastSetFlag, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &OnBroadcastSetFlag,
                      "progress: CompleteEvent (BroadcastSetFlag)") ? 1 : 0;
    n += HookPrologue(kEmkEndOrRestart, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &OnEmkEndOrRestart,
                      "progress: EMEVD EndOrRestart") ? 1 : 0;
    Log("%d of 4 flag hooks installed%s", n, n < 4 ? " (the periodic scan still catches every change)" : "");
    return n;
}

void SetRole(Role role) {
    Init();
    Global& g = G();
    g.role = int(role);
    g.capture = g.enabled && role == Role::Host;
    Log("role %s", role == Role::Host ? "host" : role == Role::Guest ? "guest" : "none");
}

Role CurrentRole() {
    return Role(G().role.load());
}

void SetDumpDir(const std::string& dir) {
    G().dump_dir = dir;
}

void SessionStarted(const char* label) {
    Global& g = G();
    if (label && label[0]) {
        g.label = label;
    }
    g.dump_session = true;
}

void SessionEnded() {
    G().dump_session = true;
}

void Tick(const GameSnapshot& s) {
    Global& g = G();
    if (!g.enabled) {
        return;
    }
    ++g.frame;
    if (!s.world_up || s.loading) {
        g.stable = 0;
        return;
    }
    const u64 m = Manager();
    const FlagStore store(m, GameOps());
    if (!store.Valid()) {
        g.stable = 0;
        return;
    }
    if (m != g.last_man || store.LoadMode() != g.last_mode) {
        g.last_man = m;
        g.last_mode = store.LoadMode();
        g.stable = 0;
        return;
    }
    if (++g.stable < kStableFrames) {
        return;
    }
    const auto now = Clock::now();
    const Role role = CurrentRole();
    if (role == Role::Host && store.LoadMode() == 0) {
        HostTick(g, store, s, now);
    } else if (role == Role::Guest) {
        GuestTick(g, store);
    }
    if (g.dump) {
        const bool gap = std::chrono::duration<double>(now - g.last_dump).count() >= kDumpMinGap;
        if (g.dump_session || (g.dump_sync && gap)) {
            WriteDumps(g, store, s);
            g.dump_session = g.dump_sync = false;
        }
    } else {
        g.dump_session = g.dump_sync = false;
    }
}

void SetItemFlagObserver(ItemFlagObserver fn) {
    G().item_observer = fn;
}

std::vector<FlagChange> PopHostFlagChanges() {
    Global& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    std::vector<FlagChange> out;
    out.swap(g.outbox);
    return out;
}

bool FullSnapshot(FlagSnapshot* out) {
    Global& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    if (!g.tracker.has_baseline()) {
        return false;
    }
    *out = g.tracker.Snapshot(g.map_id);
    return true;
}

u64 HostEpoch() {
    Init();
    std::lock_guard<std::mutex> lk(G().mu);
    return G().tracker.epoch();
}

void ApplyHostFlags(u64 epoch, const std::vector<FlagChange>& changes) {
    Init();
    Global& g = G();
    if (!g.enabled) {
        return;
    }
    std::lock_guard<std::mutex> lk(g.mu);
    g.applier.QueueChanges(epoch, changes);
}

void ApplyHostSnapshot(const FlagSnapshot& snap) {
    Init();
    Global& g = G();
    if (!g.enabled) {
        return;
    }
    std::lock_guard<std::mutex> lk(g.mu);
    g.applier.QueueSnapshot(snap);
    Log("host snapshot queued: %zu blocks, seq %llu", snap.blocks.size(), static_cast<unsigned long long>(snap.seq));
}

bool OnLinkEvent(const std::string& name, const std::string& body, std::string* error) {
    if (name == kEventFlags) {
        u64 epoch = 0;
        std::vector<FlagChange> changes;
        if (!ChangesFromJson(body, &epoch, &changes, error)) {
            return false;
        }
        ApplyHostFlags(epoch, changes);
        return true;
    }
    if (name == kEventFlagSnapshot) {
        FlagSnapshot snap;
        if (!SnapshotFromJson(body, &snap, error)) {
            return false;
        }
        ApplyHostSnapshot(snap);
        return true;
    }
    if (error) {
        *error = "not a progress event";
    }
    return false;
}

void SetPolicy(const Policy& p) {
    Init();
    std::lock_guard<std::mutex> lk(G().mu);
    G().policy = p;
}

Policy CurrentPolicy() {
    Init();
    std::lock_guard<std::mutex> lk(G().mu);
    return G().policy;
}

bool ReadFlag(u32 id, bool* value) {
    const FlagStore store(Manager(), GameOps());
    return store.Valid() && store.ReadVisible(id, value);
}

bool WriteFlagOwnSave(u32 id, bool value) {
    const FlagStore store(Manager(), GameOps());
    return store.Valid() && store.WriteOwnSave(id, value);
}

Stats GetStats() {
    Global& g = G();
    Stats s;
    s.hook_hits = g.hook_hits.load();
    s.hook_candidates = g.hook_candidates.load();
    std::lock_guard<std::mutex> lk(g.mu);
    s.scans = g.scans;
    s.host_changes = g.host_changes;
    const auto& c = g.applier.counters();
    s.guest_received = c.received;
    s.guest_applied = c.applied;
    s.guest_skipped = c.skipped;
    s.guest_dropped_seq = c.dropped_seq;
    s.guest_pending = g.applier.Pending();
    return s;
}

} // namespace coop::progress
#endif // BB_PARTY_PROGRESS_NO_GAME
