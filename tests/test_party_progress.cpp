// SPDX-License-Identifier: GPL-3.0-or-later
// Party progress sync (gpu/shim/party/party_progress, C2) against a fake in-memory
// SprjEventFlagMan: group keys, bit order, category lookup (sorted ranges == first-match table),
// policy filters, the flag store walker (mode 0 tree over the pool, mode 1 overlay), own-save
// writes, the host diff, seq de-duplication, JSON round trips, snapshot apply, BBPF files
// (read back here and, when python is available, by tools/party/flag_tool.py dump).
// Build and run: ninja -C out/gpu party-progress-test && out/gpu/party-progress-test.exe
#include "party/party_progress.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace coop::progress;

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                              \
    } while (0)

// ---- a fake SprjEventFlagMan in host memory ----

static bool MemRead(u64 a, void* o, std::size_t n) {
    if (!a) return false;
    std::memcpy(o, reinterpret_cast<const void*>(a), n);
    return true;
}
static bool MemWrite(u64 a, const void* i, std::size_t n) {
    if (!a) return false;
    std::memcpy(reinterpret_cast<void*>(a), i, n);
    return true;
}

struct FakeNode {  // MSVC std::map node layout (left +0, parent +8, right +0x10, is_nil +0x19,
                   // key +0x20, kind +0x28, storage +0x30)
    u64 left = 0, parent = 0, right = 0;
    u8 color = 0, is_nil = 0;
    u8 pad[6] = {};
    u32 key = 0, pad2 = 0;
    std::int32_t kind = 0, pad3 = 0;
    u64 storage = 0;
};
static_assert(offsetof(FakeNode, is_nil) == 0x19);
static_assert(offsetof(FakeNode, key) == 0x20);
static_assert(offsetof(FakeNode, kind) == 0x28);
static_assert(offsetof(FakeNode, storage) == 0x30);

struct FakeStore {
    std::vector<u8> man = std::vector<u8>(0x100);
    std::vector<u8> pool = std::vector<u8>(kPoolBlocks * kBlockBytes);
    std::map<u32, std::vector<u8>> overlay;  // block -> 125 bytes (mode 1)
    std::vector<std::unique_ptr<FakeNode>> nodes;
    FakeNode head;
    int mode = 0;

    u64 addr() const { return reinterpret_cast<u64>(man.data()); }
    MemOps ops() const {
        MemOps o;
        o.read = MemRead;
        o.write = MemWrite;
        return o;
    }
    FlagStore view() const { return FlagStore(addr(), ops()); }

    // Mode 0: every pool block through the tree (kind 1); mode 1: blocks 6..8 stay in the pool,
    // everything else points at a zeroed overlay buffer (kind 2), as SetLoadMode(1).
    void Build(int load_mode) {
        mode = load_mode;
        std::map<u32, std::pair<int, u64>> entries;
        for (int key = 0; key < int(kPoolBlocks); ++key) {
            const u32 block = BlockOfPoolKey(key);
            if (block == 0xffffffffu) continue;
            if (load_mode == 1 && !(block >= 6 && block <= 8)) {
                auto& buf = overlay[block];
                buf.assign(kBlockBytes, 0);
                entries[block] = {2, reinterpret_cast<u64>(buf.data())};
            } else {
                entries[block] = {1, u64(key)};
            }
        }
        nodes.clear();
        head = FakeNode{};
        head.is_nil = 1;
        std::vector<std::pair<u32, std::pair<int, u64>>> sorted(entries.begin(), entries.end());
        const u64 nil = reinterpret_cast<u64>(&head);
        std::function<u64(int, int, u64)> build = [&](int lo, int hi, u64 parent) -> u64 {
            if (lo > hi) return nil;
            const int mid = (lo + hi) / 2;
            nodes.push_back(std::make_unique<FakeNode>());
            FakeNode* n = nodes.back().get();
            n->key = sorted[mid].first;
            n->kind = sorted[mid].second.first;
            n->storage = sorted[mid].second.second;
            n->parent = parent;
            const u64 self = reinterpret_cast<u64>(n);
            n->left = build(lo, mid - 1, self);
            n->right = build(mid + 1, hi, self);
            return self;
        };
        head.parent = build(0, int(sorted.size()) - 1, nil);
        auto put32 = [&](std::size_t off, u32 v) { std::memcpy(man.data() + off, &v, 4); };
        auto put64 = [&](std::size_t off, u64 v) { std::memcpy(man.data() + off, &v, 8); };
        put32(0x10, 5);
        put32(0x18, 3);
        put32(0x1c, 1000);
        put32(0x20, 125);
        put32(0x24, 1200);
        put64(0x28, reinterpret_cast<u64>(pool.data()));
        put64(0x38, nil);
        put32(0x80, u32(load_mode));
    }
    bool PoolBit(u32 id) const {
        const int key = PoolKeyOfId(id);
        return key >= 0 && GetBit(pool.data() + key * kBlockBytes, id % 1000);
    }
    void SetPoolBit(u32 id, bool v) {
        PutBit(pool.data() + PoolKeyOfId(id) * kBlockBytes, id % 1000, v);
    }
    bool OverlayBit(u32 id) const {
        auto it = overlay.find(id / 1000);
        return it != overlay.end() && GetBit(it->second.data(), id % 1000);
    }
    void SetOverlayBit(u32 id, bool v) { PutBit(overlay[id / 1000].data(), id % 1000, v); }
};

// ---- tests ----

static void TestKeys() {
    for (int z = 0; z < 10; ++z) {
        CHECK(GroupKey(0, 0, 0, z) == 5 * z);
        CHECK(GroupKey(1, 0, 0, z) == 1 + 5 * z);
        CHECK(GroupKey(7, 0, 0, z) == 4 + 5 * z);
    }
    CHECK(GroupKey(9, 0, 0, 0) == -1);           // type 9 has no global group
    CHECK(PoolKeyOfId(12401800) == 115 + 50 + 4);  // T1 m24_00 zone 1 -> 169
    CHECK(PoolKeyOfId(73601999) == 115 + 50 + 23 * 3 + 16);  // T7 m36_00 zone 1 -> 250
    CHECK(PoolKeyOfId(73609000) == 9 * 115 + 50 + 23 * 3 + 16);  // T7 m36_00 zone 9 -> 1170
    CHECK(PoolKeyOfId(73609000) == 1170);
    CHECK(PoolKeyOfId(2200) == 10);               // global zone 2
    CHECK(PoolKeyOfId(10002500) == 1 + 2 * 5);    // T1 global zone 2
    CHECK(PoolKeyOfId(14000000) == 50 + 11);      // area 40 -> map index 11 (shares 29_00)
    CHECK(PoolKeyOfId(14100000) == 50 + 17);      // area 41 -> 17
    CHECK(PoolKeyOfId(14600000) == 50 + 22);      // area 46 -> 22
    CHECK(PoolKeyOfId(14700000) == -1);           // area 47 -> 23: out of range
    CHECK(PoolKeyOfId(19900000) == -1);           // area 99: not pooled
    CHECK(PoolKeyOfId(13000000) == -1);           // m30: not in the table
    CHECK(PoolKeyOfId(12430000) == -1);           // m24_03: not in the table
    CHECK(PoolKeyOfId(10000) == -1);              // block 10: area 0 block 1, type 0
    CHECK(GroupKey(5, 99, 0, 2) == 0x4b0 + 1 + 10);
    // the inverse covers all 1200 keys exactly once and round-trips
    std::set<u32> blocks;
    for (int k = 0; k < int(kPoolBlocks); ++k) {
        const u32 b = BlockOfPoolKey(k);
        CHECK(b != 0xffffffffu);
        CHECK(PoolKeyOfBlock(b) == k);
        blocks.insert(b);
    }
    CHECK(blocks.size() == kPoolBlocks);
    CHECK(BlockOfPoolKey(61) == 12900);  // 29_00 wins over area 40 for key 61
    CHECK(NeverWritable(9020) && NeverWritable(9026) && !NeverWritable(9019) && !NeverWritable(9027));
    CHECK(NeverWritable(12900100) && NeverWritable(14100000) && NeverWritable(19900000));
    CHECK(!NeverWritable(12401800));
}

static void TestBits() {
    u8 d[kBlockBytes] = {};
    PutBit(d, 0, true);
    CHECK(d[0] == 0x80);
    PutBit(d, 9, true);
    CHECK(d[1] == 0x40);
    PutBit(d, 999, true);
    CHECK(d[124] == 0x01);
    CHECK(GetBit(d, 0) && GetBit(d, 9) && GetBit(d, 999) && !GetBit(d, 1));
    PutBit(d, 0, false);
    CHECK(d[0] == 0 && !GetBit(d, 0));
}

static void TestCategories() {
    const auto& r = CategoryRanges();
    CHECK(!r.empty());
    for (std::size_t i = 1; i < r.size(); ++i) {
        CHECK(r[i - 1].last < r[i].first);
    }
    CHECK(CategoryOf(2200) == Category::never_sync);   // area gate (0x47304B0)
    CHECK(CategoryOf(2410) == Category::never_sync);
    CHECK(CategoryOf(2405) == Category::never_sync);   // TBL0 gate
    CHECK(CategoryOf(9801) == Category::time_of_day);
    CHECK(CategoryOf(9180) == Category::never_sync);   // cutscene playing
    CHECK(CategoryOf(72100130) == Category::never_sync);
    CHECK(CategoryOf(12100180) == Category::never_sync);
    CHECK(CategoryOf(12101800) == Category::boss_defeated);
    CHECK(CategoryOf(12101802) == Category::cutscene_seen);
    CHECK(CategoryOf(9021) == Category::never_sync);
    CHECK(CategoryOf(6610) == Category::never_sync);
    CHECK(CategoryOf(21) == Category::never_sync);     // ending
    CHECK(CategoryOf(1005) == Category::npc_quest);
    CHECK(CategoryOf(5000) == Category::item_lot_picked);
    CHECK(CategoryOf(2000) == Category::session_runtime);
    CHECK(CategoryOf(12400150) == Category::shortcut_door);
    CHECK(CategoryOf(99999999) == Category::never_sync);
    // exhaustive against the first-match table: every range edge +-1 and random ids
    std::vector<u32> probe;
    for (const auto& x : r) {
        for (u32 d : {0u, 1u}) {
            probe.push_back(x.first - d);
            probe.push_back(x.first + d);
            probe.push_back(x.last - d);
            probe.push_back(x.last + d);
        }
    }
    std::mt19937 rng(7);
    for (int i = 0; i < 20000; ++i) {
        probe.push_back(rng() % 100000000u);
    }
    for (u32 b = 0; b < 10000; ++b) {
        probe.push_back(b);
    }
    std::size_t mismatches = 0;
    for (u32 id : probe) {
        if (CategoryOf(id) != CategoryOfLinear(id)) {
            if (++mismatches < 5) std::fprintf(stderr, "category mismatch at %u\n", id);
        }
    }
    CHECK(mismatches == 0);
    Category c;
    CHECK(CategoryFromName("lamp_unlocked", &c) && c == Category::lamp_unlocked);
    CHECK(!CategoryFromName("nope", &c));
    std::printf("categories: %zu disjoint ranges, %zu ids probed\n", r.size(), probe.size());
}

static void TestPolicy() {
    Policy p = Policy::Defaults();
    CHECK(ModeFor(p, 12101800) == SyncMode::SetOnly);  // boss_defeated
    CHECK(ModeFor(p, 12101802) == SyncMode::SetOnly);  // cutscene_seen
    CHECK(ModeFor(p, 21) == SyncMode::Off);            // ending: player-owned
    CHECK(ModeFor(p, 9802) == SyncMode::Off);          // time of day: C4 only
    CHECK(!HostCandidate(9800) && !HostCandidate(2410) && !HostCandidate(3601));
    CHECK(ModeFor(p, 12100180) == SyncMode::Off);      // ending event
    CHECK(ModeFor(p, 12400150) == SyncMode::State);    // elevator
    CHECK(ModeFor(p, 12400147) == SyncMode::State);
    CHECK(ModeFor(p, 5000) == SyncMode::Off);          // item lot: C3
    CHECK(ModeFor(p, 1005) == SyncMode::Off);          // npc quest
    CHECK(ModeFor(p, 2000) == SyncMode::Off);          // session runtime
    CHECK(ModeFor(p, 9021) == SyncMode::Off);          // chalice slot
    CHECK(ModeFor(p, 9180) == SyncMode::Off);          // cleared every load
    CHECK(ModeFor(p, 12100505) == SyncMode::Off);      // Doll position (random)
    CHECK(!HostCandidate(2000) && HostCandidate(5000) && HostCandidate(1005) && HostCandidate(12101800));
    p.mode[int(Category::item_lot_picked)] = SyncMode::SetOnly;
    CHECK(ModeFor(p, 5000) == SyncMode::Off);          // C3 owns item pickups, whatever the policy says
    CHECK(!HostCandidate(60009000));                   // C3 ledger flags
    // candidate masks agree with HostCandidate
    const auto& blocks = CandidateBlocks();
    CHECK(!blocks.empty());
    std::size_t bad = 0, bits = 0;
    for (u32 b : blocks) {
        const auto* m = CandidateMask(b);
        for (u32 i = 0; i < 1000; ++i) {
            const bool want = HostCandidate(b * 1000 + i);
            bits += want;
            bad += want != GetBit(m->data(), i);
        }
    }
    CHECK(bad == 0);
    CHECK(CandidateMask(6) == nullptr);  // 6000-6999 personal
    std::printf("policy: %zu candidate blocks, %zu candidate flags\n", blocks.size(), bits);
}

static void TestStore() {
    FakeStore f;
    f.Build(0);
    FlagStore s = f.view();
    CHECK(s.Valid() && s.LoadMode() == 0);
    BlockRef r;
    CHECK(s.FindBlock(12401, &r) && r.kind == 1 && r.data == reinterpret_cast<u64>(f.pool.data()) + 169 * 125);
    CHECK(!s.FindBlock(12430, &r));
    std::size_t n = 0;
    u32 prev = 0;
    bool ordered = true;
    CHECK(s.ForEachBlock([&](const BlockRef& b) {
        if (n && b.block <= prev) ordered = false;
        prev = b.block;
        ++n;
    }));
    CHECK(ordered && n == kPoolBlocks);
    // mode 0: own-save writes go through the tree = the pool
    CHECK(s.WriteOwnSave(12401800, true));
    CHECK(f.PoolBit(12401800));
    bool v = false;
    CHECK(s.ReadVisible(12401800, &v) && v);
    CHECK(s.ReadOwnSave(12401800, &v) && v);
    CHECK(!s.WriteOwnSave(9021, true));  // never
    CHECK(!f.PoolBit(9021));

    // mode 1: the visible tree is the overlay; own-save writes go to the pool only
    FakeStore g;
    g.Build(1);
    FlagStore t = g.view();
    CHECK(t.Valid() && t.LoadMode() == 1);
    CHECK(t.FindBlock(12401, &r) && r.kind == 2);
    CHECK(t.FindBlock(6, &r) && r.kind == 1);
    CHECK(t.WriteOwnSave(12401800, true));
    CHECK(g.PoolBit(12401800) && !g.OverlayBit(12401800));
    CHECK(t.ReadVisible(12401800, &v) && !v);
    CHECK(t.ReadOwnSave(12401800, &v) && v);
    CHECK(t.WriteVisible(2200, true));  // the overlay
    CHECK(g.OverlayBit(2200) && !g.PoolBit(2200));
    CHECK(t.WriteOwnSave(12401800, false) && !g.PoolBit(12401800));
    // a broken header is rejected
    FakeStore h;
    h.Build(0);
    const u32 bad = 999;
    std::memcpy(h.man.data() + 0x20, &bad, 4);
    CHECK(!h.view().Valid());
}

static std::vector<FlagChange> Scan(HostTracker& tr, const FlagStore& s) {
    std::vector<FlagChange> out;
    s.ForEachBlock([&](const BlockRef& r) {
        u8 d[kBlockBytes];
        if (CandidateMask(r.block) && s.ReadBlockBytes(r, d)) tr.ObserveBlock(r.block, d, &out);
    });
    tr.EndScan();
    return out;
}

static void TestDiffAndDedupe() {
    FakeStore f;
    f.Build(0);
    f.SetPoolBit(12101800, true);  // already dead before the session: baseline, not a change
    FlagStore s = f.view();
    HostTracker tr(0x1234);
    CHECK(Scan(tr, s).empty());
    CHECK(tr.has_baseline());
    f.SetPoolBit(12401800, true);  // boss
    f.SetPoolBit(12400150, true);  // elevator
    f.SetPoolBit(2000, true);      // session runtime: ignored
    f.SetPoolBit(9021, true);      // never: ignored
    auto ch = Scan(tr, s);
    CHECK(ch.size() == 2);
    CHECK(ch.size() == 2 && ch[0].id == 12400150 && ch[0].value && ch[0].seq == 1 &&
          ch[0].cat == Category::shortcut_door);
    CHECK(ch.size() == 2 && ch[1].id == 12401800 && ch[1].seq == 2 && ch[1].cat == Category::boss_defeated);
    CHECK(Scan(tr, s).empty());
    // the hook path: an id confirmed once, the scan then sees no change
    f.SetPoolBit(12400150, false);
    std::vector<FlagChange> hook;
    tr.ObserveId(12400150, false, &hook);
    tr.ObserveId(12400150, false, &hook);
    tr.ObserveId(2000, false, &hook);  // not a candidate
    CHECK(hook.size() == 1 && hook[0].seq == 3 && !hook[0].value);
    CHECK(Scan(tr, s).empty());
    // snapshot = shadow
    FlagSnapshot snap = tr.Snapshot(0x18000000);
    bool known = false;
    CHECK(snap.epoch == 0x1234 && snap.seq == 3 && snap.map_id == 0x18000000);
    CHECK(snap.Get(12401800, &known) && known);
    CHECK(snap.Get(12101800, &known) && known);
    CHECK(!snap.Get(12400150, &known) && known);
    CHECK(!snap.Get(2000, &known));

    // guest cursor: duplicates and replays dropped, a new epoch resets
    GuestCursor c;
    auto a = c.Filter(1, {{1, true, Category::boss_defeated, 1}, {2, true, Category::boss_defeated, 2}});
    CHECK(a.size() == 2 && c.last_seq() == 2);
    a = c.Filter(1, {{2, true, Category::boss_defeated, 2}, {3, true, Category::boss_defeated, 3}});
    CHECK(a.size() == 1 && a[0].seq == 3);
    a = c.Filter(1, {{1, true, Category::boss_defeated, 1}});
    CHECK(a.empty());
    a = c.Filter(2, {{1, true, Category::boss_defeated, 1}});  // host restarted
    CHECK(a.size() == 1 && c.epoch() == 2);
    c.Reset(2, 10);
    CHECK(c.Filter(2, {{5, true, Category::boss_defeated, 9}}).empty());
}

static void TestJson() {
    std::vector<FlagChange> in = {{12401800, true, Category::boss_defeated, 1},
                                  {12400150, false, Category::shortcut_door, 2},
                                  {72400200, true, Category::lamp_unlocked, 9007199254740991ull}};
    const std::string text = ChangesToJson(0xfedcba9876543210ull, in);
    u64 epoch = 0;
    std::vector<FlagChange> out;
    std::string err;
    CHECK(ChangesFromJson(text, &epoch, &out, &err));
    CHECK(epoch == 0xfedcba9876543210ull && out == in);
    CHECK(!ChangesFromJson("{\"v\":1,\"epoch\":\"0x1\",\"c\":[[1,1,\"bogus\",1]]}", &epoch, &out, &err));
    CHECK(!ChangesFromJson("{\"v\":2,\"epoch\":\"0x1\",\"c\":[]}", &epoch, &out, &err));
    CHECK(!ChangesFromJson("not json", &epoch, &out, &err));

    FlagSnapshot s;
    s.epoch = 0x8000000000000001ull;
    s.seq = 42;
    s.map_id = 0x18010000;
    s.blocks[12401] = {800, 801};
    s.blocks[12400] = {};
    s.blocks[2] = {200};
    const std::string st = SnapshotToJson(s);
    FlagSnapshot r;
    CHECK(SnapshotFromJson(st, &r, &err));
    CHECK(r == s);
    CHECK(!SnapshotFromJson("{\"v\":1,\"epoch\":\"0x1\",\"seq\":1,\"map\":0,\"b\":[[1,[1000]]]}", &r, &err));
    std::printf("json: changes %zu bytes, snapshot %zu bytes\n", text.size(), st.size());
}

static void TestApplier() {
    // host world -> snapshot + changes -> a guest in load mode 1
    FakeStore host;
    host.Build(0);
    host.SetPoolBit(12401800, true);  // boss
    host.SetPoolBit(12400151, true);  // elevator up
    host.SetPoolBit(72400200, true);  // lamp lit (lamp_unlocked)
    host.SetPoolBit(5000, true);      // item lot (off by default)
    FlagStore hs = host.view();
    HostTracker tr(77);
    Scan(tr, hs);
    const FlagSnapshot snap = tr.Snapshot(0);

    FakeStore guest;
    guest.Build(1);
    guest.SetPoolBit(12400150, true);  // guest's own elevator state: follows the host (0)
    guest.SetPoolBit(12200125, true);  // another elevator, 0 on the host
    guest.SetPoolBit(2200, true);      // guest killed something the host has not: set-only keeps it
    FlagStore gs = guest.view();
    GuestApplier ap;
    // round trip through JSON as the link would
    FlagSnapshot wire;
    std::string err;
    CHECK(SnapshotFromJson(SnapshotToJson(snap), &wire, &err));
    ap.QueueSnapshot(wire);
    Policy p = Policy::Defaults();
    CHECK(ap.Apply(gs, p, false) > 0);
    CHECK(guest.PoolBit(12401800));
    CHECK(guest.PoolBit(12400151) && !guest.PoolBit(12400150));
    CHECK(!guest.PoolBit(12200125));
    CHECK(guest.PoolBit(2200));
    CHECK(!guest.PoolBit(5000));                                  // items off
    CHECK(!guest.OverlayBit(12401800));                           // no mirror by default
    CHECK(ModeFor(p, 72400200) == SyncMode::SetOnly && guest.PoolBit(72400200));
    CHECK(ap.cursor().epoch() == 77 && ap.cursor().last_seq() == snap.seq);

    // live changes, with a replayed duplicate; mirror on
    p.mirror_overlay = true;
    host.SetPoolBit(12411800, true);
    auto ch = Scan(tr, hs);
    CHECK(ch.size() == 1);
    ap.QueueChanges(77, ch);
    ap.QueueChanges(77, ch);  // replay: dropped
    CHECK(ap.counters().dropped_seq == ch.size());
    CHECK(ap.Apply(gs, p, false) == 1);
    CHECK(guest.PoolBit(12411800) && guest.OverlayBit(12411800));
    // set-only: a host clear does not clear the guest
    host.SetPoolBit(12411800, false);
    ch = Scan(tr, hs);
    CHECK(ch.size() == 1 && !ch[0].value);
    ap.QueueChanges(77, ch);
    ap.Apply(gs, p, false);
    CHECK(guest.PoolBit(12411800));

    // load mode 0: kept until the post-load window
    FakeStore own;
    own.Build(0);
    FlagStore os = own.view();
    GuestApplier ap0;
    ap0.QueueChanges(5, {{12401800, true, Category::boss_defeated, 1}});
    CHECK(ap0.Apply(os, Policy::Defaults(), false) == 0 && ap0.Pending() == 1 && !own.PoolBit(12401800));
    CHECK(ap0.Apply(os, Policy::Defaults(), true) == 1 && ap0.Pending() == 0 && own.PoolBit(12401800));
    // never-writable ids are refused even when a (bad) host sends them
    ap0.QueueChanges(5, {{9021, true, Category::world_state, 2}});
    ap0.Apply(os, Policy::Defaults(), true);
    CHECK(!own.PoolBit(9021));
}

static void TestBbpf(const char* source_dir) {
    FakeStore f;
    f.Build(1);
    f.SetPoolBit(12401800, true);
    f.SetPoolBit(6100, true);  // personal block 6 is pooled in mode 1
    f.SetOverlayBit(2200, true);
    f.SetOverlayBit(12400999, true);
    FlagStore s = f.view();
    BbpfHeader h;
    h.map_id = 0x18000000;
    h.load_mode = 1;
    h.role = 6;
    h.unix_ms = 1234567890123ull;
    h.frame = 77;
    h.label = "guest1";
    const auto vis = CollectVisible(s);
    const auto pool = CollectPool(s);
    CHECK(vis.size() == kPoolBlocks && pool.size() == kPoolBlocks);
    const auto bytes = BbpfEncode(h, vis);
    CHECK(bytes.size() == 64 + kPoolBlocks * (8 + 125));
    BbpfHeader h2;
    std::vector<BbpfBlock> back;
    CHECK(BbpfDecode(bytes, &h2, &back));
    CHECK(h2.map_id == h.map_id && h2.load_mode == 1 && h2.role == 6 && h2.unix_ms == h.unix_ms && h2.frame == 77 &&
          h2.label == "guest1" && back.size() == vis.size());
    bool kinds_ok = true;
    for (const auto& b : back) {
        kinds_ok &= (b.block >= 6 && b.block <= 8) ? b.kind == 1 : b.kind == 2;
    }
    CHECK(kinds_ok);

    char path[512], ppath[512];
    std::snprintf(path, sizeof path, "%s/out/party_progress_test.bbpf", source_dir);
    std::snprintf(ppath, sizeof ppath, "%s/out/party_progress_test_pool.bbpf", source_dir);
    CHECK(BbpfWriteFile(path, h, vis));
    h.label = "guest1-pool";
    CHECK(BbpfWriteFile(ppath, h, pool));
    // tools/party/flag_tool.py dump must read them
    char cmd[1600];
    std::snprintf(cmd, sizeof cmd, "python -I \"%s/tools/party/flag_tool.py\" dump \"%s\" --set-only 2>&1", source_dir,
                  path);
    std::FILE* p = popen(cmd, "r");
    if (!p) {
        std::printf("bbpf: python not run (popen failed); the files are %s, %s\n", path, ppath);
        return;
    }
    std::string text;
    char line[512];
    while (std::fgets(line, sizeof line, p)) text += line;
    const int rc = pclose(p);
    if (text.find("not recognized") != std::string::npos || text.find("not found") != std::string::npos) {
        std::printf("bbpf: python not available; skipped the flag_tool check\n");
        return;
    }
    std::printf("bbpf: flag_tool.py dump (rc %d):\n%s", rc, text.c_str());
    CHECK(rc == 0);
    CHECK(text.find("label='guest1'") != std::string::npos);
    CHECK(text.find("load_mode=1") != std::string::npos && text.find("role=6") != std::string::npos);
    CHECK(text.find("\n2200\n") != std::string::npos);
    CHECK(text.find("\n6100\n") != std::string::npos);
    CHECK(text.find("\n12400999\n") != std::string::npos);
    CHECK(text.find("12401800") == std::string::npos);  // in the pool, not the visible tree
    std::snprintf(cmd, sizeof cmd, "python -I \"%s/tools/party/flag_tool.py\" dump \"%s\" --set-only 2>&1", source_dir,
                  ppath);
    p = popen(cmd, "r");
    text.clear();
    while (p && std::fgets(line, sizeof line, p)) text += line;
    CHECK(p && pclose(p) == 0);
    CHECK(text.find("label='guest1-pool'") != std::string::npos);
    CHECK(text.find("\n12401800\n") != std::string::npos && text.find("\n2200\n") == std::string::npos);
}

int main(int argc, char** argv) {
    const char* source_dir = argc > 1 ? argv[1] : BB_SOURCE_DIR;
    TestKeys();
    TestBits();
    TestCategories();
    TestPolicy();
    TestStore();
    TestDiffAndDedupe();
    TestJson();
    TestApplier();
    TestBbpf(source_dir);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
