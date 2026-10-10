// SPDX-License-Identifier: GPL-3.0-or-later
// Party co-op C2: host-authoritative progress sync of event flags (design:
// docs/party/event_flags.md). The host's flag changes in the syncable categories
// (party_flags.inc) are captured and sent to the guests, who write them into their OWN save pool,
// so a guest's save keeps up with the host's world even though the game itself never progresses
// a summoned phantom's save (load mode 1, see the design doc section 3).
//
//   host:  hooks SetEventFlag 0x13cfcc0, SetEventFlagValue 0x13d0060, Lua CompleteEvent
//          (BroadcastSetFlag) 0x132aad0 and the EMEVD completion writer
//          SprjEmkEventIns::EndOrRestart 0x12ed100 note candidate ids; on the main-thread tick,
//          at safe points, the noted ids and (every BB_PARTY_PROGRESS_SCAN_MS, default 1000) every
//          syncable block are compared with a shadow copy, so writers that bypass the hooks are
//          caught too. Changes -> PopHostFlagChanges(); joins -> FullSnapshot().
//   guest: ApplyHostFlags / ApplyHostSnapshot queue (any thread); the main-thread tick applies
//          them at safe points (world up, no loading screen, store stable): load mode 1 ->
//          the guest's own pool directly (pool + key*125, MSB-first); load mode 0 -> SetEventFlag
//          (only in the window right after a map load, where event triggers are least harmful).
//
// Policy (guest side, per category; host sends every candidate category):
//   set-only:  boss_defeated, lamp_unlocked, shortcut_door, key_event (not the endings),
//              cutscene_seen, world_state
//   state:     elevators / toggling gates (both 0 and 1 follow the host)
//   off:       item_lot_picked (always: C3, party_items, owns item pickups - the host side hands
//              every item-pickup flag it sees set to the SetItemFlagObserver), npc_quest
//              (switchable, BB_PARTY_PROGRESS_NPC=1), boss_area,
//              session_runtime, time_of_day (C4 cutscene replay only), never_sync (incl. the area
//              gate flags of 0x47304B0/0x47301B0 - ON blocks the bells - and the ending flags),
//              ids the game randomises / counts / rebuilds each load.
//   never written from outside: 9020-9026 (chalice slot), chalice / area 99 groups.
//   C3's ledger flags 60009000+ (type-6 global group) are never_sync here and never touched.
//
// Env: BB_PARTY_PROGRESS=0 disables everything (no hooks, nothing sent or applied).
//      BB_PARTY_FLAG_DUMP=1 writes BBPF v1 snapshots (tools/party/flag_tool.py) to
//        <user dir>/party/flags_<label>_<frame>.bbpf at session start / end and on each sync
//        (at most one per 2 s), plus the raw own pool ("<label>-pool").
//      BB_PARTY_PROGRESS_NPC=1 applies npc_quest (set-only) on guests. BB_PARTY_PROGRESS_MIRROR=1 also writes set-only flags into the
//        host-world overlay while in load mode 1 (visible at once; default off: the overlay
//        already follows the host through the vanilla snapshot / CompleteEvent channels).
//      BB_PARTY_PROGRESS_SCAN_MS (host diff period, default 1000, min 100).
//
// Addresses are our offsets (raw ELF VA of eboot.elf). Everything touching the game is
// main-thread only unless noted. The pure parts (categories, keys, bits, the store walker over
// injectable memory ops, policy, diff, JSON, BBPF) are testable without the game
// (tests/test_party_progress.cpp, target party-progress-test).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace json {
struct Value;
}

namespace coop {
struct GameSnapshot;
}

namespace coop::progress {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// ---- categories (party_flags.inc: first match wins; no row = never_sync) ----

enum class Category : u8 {
    never_sync = 0,
    boss_defeated,
    boss_area,
    lamp_unlocked,
    shortcut_door,
    npc_quest,
    item_lot_picked,
    key_event,
    cutscene_seen,
    world_state,
    session_runtime,
    time_of_day,  ///< 9800-9802: only through the cutscene replay path (C4), never silently
    kCount
};
constexpr int kCategoryCount = static_cast<int>(Category::kCount);
const char* CategoryName(Category c);
bool CategoryFromName(const std::string& name, Category* out);

/// Disjoint sorted ranges equivalent to the first-match table.
struct CategoryRange {
    u32 first, last;
    Category cat;
};
const std::vector<CategoryRange>& CategoryRanges();
/// Binary search over CategoryRanges(); never_sync when no row covers the id.
Category CategoryOf(u32 id);
/// The first-match linear scan of the raw table (reference for tests).
Category CategoryOfLinear(u32 id);

// ---- flag ids, group keys, bits ----

constexpr u32 kBitsPerBlock = 1000;
constexpr u32 kBlockBytes = 125;
constexpr u32 kPoolBlocks = 1200;

/// resolve_group_key 0x13bc710, mirrored exactly (type digit, area, map block, zone).
int GroupKey(int type, int area, int block, int zone);
/// The pool index of a flag block (id / 1000), or -1 when the block is not in the saved pool
/// (area 99, unknown map, invalid type).
int PoolKeyOfBlock(u32 block);
inline int PoolKeyOfId(u32 id) { return PoolKeyOfBlock(id / kBitsPerBlock); }
/// The canonical block of a pool key (inverse of PoolKeyOfBlock; chalice instance keys use map
/// block 0), or 0xffffffff.
u32 BlockOfPoolKey(int key);
/// MSB-first: flag f of a block is bit (7 - f % 8) of byte f / 8.
inline bool GetBit(const u8* data, u32 bit) { return (data[bit >> 3] & (0x80u >> (bit & 7))) != 0; }
inline void PutBit(u8* data, u32 bit, bool on) {
    const u8 m = static_cast<u8>(0x80u >> (bit & 7));
    data[bit >> 3] = on ? static_cast<u8>(data[bit >> 3] | m) : static_cast<u8>(data[bit >> 3] & ~m);
}
/// Ids nothing outside the game may write (9020-9026 chalice slot, chalice areas 40-47 / 29,
/// area 99, block-less ids).
bool NeverWritable(u32 id);

// ---- policy ----

enum class SyncMode : u8 { Off = 0, SetOnly = 1, State = 2 };
struct Policy {
    std::array<SyncMode, kCategoryCount> mode{};
    bool mirror_overlay = false;
    static Policy Defaults();
    /// Defaults plus BB_PARTY_PROGRESS_NPC / _MIRROR. item_lot_picked stays Off (C3 owns items).
    static Policy FromEnv();
};
/// What the guest does with `id` (category, the state list, the exclusions, NeverWritable).
SyncMode ModeFor(const Policy& p, u32 id, Category cat);
inline SyncMode ModeFor(const Policy& p, u32 id) { return ModeFor(p, id, CategoryOf(id)); }
/// The host captures and sends this id (any category a guest may enable, minus the exclusions).
bool HostCandidate(u32 id, Category cat);
inline bool HostCandidate(u32 id) { return HostCandidate(id, CategoryOf(id)); }
/// Per-block mask of the host-candidate bits (nullptr when the block has none).
const std::array<u8, kBlockBytes>* CandidateMask(u32 block);
/// Every block that has candidate bits, ascending.
const std::vector<u32>& CandidateBlocks();

// ---- changes and snapshots ----

struct FlagChange {
    u32 id = 0;
    bool value = false;
    Category cat = Category::never_sync;
    u64 seq = 0;
    bool operator==(const FlagChange& o) const {
        return id == o.id && value == o.value && cat == o.cat && seq == o.seq;
    }
};

/// Host's candidate flags: per block the set bit offsets; a listed block's other candidate bits
/// are 0 (state-synced ids follow that too).
struct FlagSnapshot {
    u64 epoch = 0;    ///< host session id (a guest drops its seq cursor when it changes)
    u64 seq = 0;      ///< the last change seq this snapshot includes
    u32 map_id = 0xffffffffu;
    std::map<u32, std::vector<u32>> blocks;  ///< block -> set bits (ascending)
    bool Get(u32 id, bool* known) const;
    bool operator==(const FlagSnapshot& o) const {
        return epoch == o.epoch && seq == o.seq && map_id == o.map_id && blocks == o.blocks;
    }
};

/// Shadow of the host's candidate bits, the diff and the seq counter (pure; the game side feeds
/// it blocks). Not thread-safe by itself.
class HostTracker {
public:
    explicit HostTracker(u64 epoch = 0) : epoch_(epoch) {}
    u64 epoch() const { return epoch_; }
    u64 seq() const { return seq_; }
    bool has_baseline() const { return baseline_; }
    /// The block as the game holds it now. The first time a block is seen, nothing is emitted
    /// unless the baseline is already complete (EndScan ran once) - a block that appears later
    /// (a new map's groups) emits its set candidate bits.
    void ObserveBlock(u32 block, const u8* data, std::vector<FlagChange>* out);
    /// One id read from the game (the hooks' confirmation path).
    void ObserveId(u32 id, bool value, std::vector<FlagChange>* out);
    /// A full scan finished: the baseline is complete.
    void EndScan() { baseline_ = true; }
    FlagSnapshot Snapshot(u32 map_id) const;

private:
    u64 epoch_ = 0, seq_ = 0;
    bool baseline_ = false;
    std::map<u32, std::array<u8, kBlockBytes>> shadow_;  // candidate-masked
};

/// Guest-side cursor: drops changes it already has (same epoch, seq <= last).
class GuestCursor {
public:
    /// Keeps the changes not seen yet (in order), advances the cursor.
    std::vector<FlagChange> Filter(u64 epoch, const std::vector<FlagChange>& in);
    /// A snapshot resets the cursor to its seq (and epoch).
    void Reset(u64 epoch, u64 seq);
    u64 epoch() const { return epoch_; }
    u64 last_seq() const { return last_; }

private:
    u64 epoch_ = 0, last_ = 0;
    bool any_ = false;
};

class FlagStore;

/// Guest-side queue: changes / snapshots in arrival order, de-duplicated by the cursor, applied to
/// a store by policy. Not thread-safe by itself.
class GuestApplier {
public:
    struct Counters {
        u64 received = 0, applied = 0, already = 0, skipped = 0, dropped_seq = 0, failed = 0;
    };
    void QueueChanges(u64 epoch, const std::vector<FlagChange>& changes);
    void QueueSnapshot(const FlagSnapshot& snap);
    std::size_t Pending() const { return items_.size(); }
    /// Applies everything queued. In load mode 0 nothing is applied (the queue is kept) unless
    /// `allow_mode0`. Returns the number of flags written.
    std::size_t Apply(const FlagStore& store, const Policy& policy, bool allow_mode0);
    const Counters& counters() const { return n_; }
    const GuestCursor& cursor() const { return cursor_; }

private:
    bool ApplyOne(const FlagStore& store, const Policy& policy, u32 id, bool value);
    struct Item {
        bool snapshot = false;
        std::vector<FlagChange> changes;
        FlagSnapshot snap;
    };
    std::vector<Item> items_;
    GuestCursor cursor_;
    Counters n_;
};

// JSON bodies of the PartyLink EVENTs "flags" and "flag_snapshot".
constexpr const char* kEventFlags = "flags";
constexpr const char* kEventFlagSnapshot = "flag_snapshot";
std::string ChangesToJson(u64 epoch, const std::vector<FlagChange>& changes);
bool ChangesFromJson(const std::string& text, u64* epoch, std::vector<FlagChange>* out, std::string* error);
std::string SnapshotToJson(const FlagSnapshot& s);
bool SnapshotFromJson(const std::string& text, FlagSnapshot* out, std::string* error);

// ---- the store (SprjEventFlagMan 0x553b100) over injectable memory access ----

struct MemOps {
    bool (*read)(u64 address, void* out, std::size_t n) = nullptr;
    bool (*write)(u64 address, const void* in, std::size_t n) = nullptr;
    /// The game's SetEventFlag(man, id, on) (main thread); nullptr = write through the tree.
    void (*set_event_flag)(u64 manager, u32 id, int on) = nullptr;
};

/// SprjEventFlagMan field offsets (docs/party/event_flags.md section 2; checked in 0x13cfcc0).
namespace man {
constexpr u64 kBits = 0x1c, kStride = 0x20, kCount = 0x24, kPool = 0x28, kTreeHead = 0x38, kLoadMode = 0x80;
// MSVC std::map node: left +0, parent +8, right +0x10, is_nil +0x19, key +0x20, kind +0x28,
// storage +0x30 (kind 1: u32 pool index; kind 2: buffer pointer). The head's parent is the root.
constexpr u64 kNodeLeft = 0, kNodeParent = 8, kNodeRight = 0x10, kNodeIsNil = 0x19, kNodeKey = 0x20,
              kNodeKind = 0x28, kNodeStorage = 0x30;
} // namespace man

struct BlockRef {
    u32 block = 0;
    int kind = 0;    ///< 1 pooled, 2 pointer (overlay / dynamic)
    u64 data = 0;    ///< address of the block's 125 bytes (0 = absent)
};

class FlagStore {
public:
    FlagStore(u64 manager, const MemOps& ops);
    /// Header plausible: 1000 bits, stride 125, 1200 pool blocks, pool and tree present, mode 0/1.
    bool Valid() const { return valid_; }
    u64 manager() const { return man_; }
    int LoadMode() const { return mode_; }
    u64 Pool() const { return pool_; }
    /// The visible tree (what Get/SetEventFlag see).
    bool FindBlock(u32 block, BlockRef* out) const;
    /// In-order walk of the visible tree (ascending blocks); false when it looked corrupt.
    bool ForEachBlock(const std::function<void(const BlockRef&)>& fn, std::size_t max_nodes = 8192) const;
    bool ReadVisible(u32 id, bool* value) const;
    bool ReadBlockBytes(const BlockRef& r, u8* out) const;
    /// The saved pool (pool + key * 125).
    u64 PoolBlockAddress(int key) const;
    bool ReadOwnSave(u32 id, bool* value) const;
    /// Writes the own save: mode 1 -> the pool directly; mode 0 -> SetEventFlag (or the tree).
    bool WriteOwnSave(u32 id, bool value) const;
    /// Writes through the visible tree (mode 1: the overlay). SetEventFlag when provided.
    bool WriteVisible(u32 id, bool value) const;
    bool WritePoolBit(u32 id, bool value) const;

private:
    u64 man_ = 0;
    MemOps ops_;
    bool valid_ = false;
    int mode_ = -1;
    u64 pool_ = 0, head_ = 0;
};

// ---- BBPF v1 (tools/party/flag_tool.py) ----

struct BbpfBlock {
    u32 block = 0;
    u8 kind = 1;
    std::array<u8, kBlockBytes> data{};
};
struct BbpfHeader {
    u32 map_id = 0xffffffffu;
    int load_mode = 0;
    int role = -1;
    u64 unix_ms = 0;
    u64 frame = 0;
    std::string label;
};
std::vector<u8> BbpfEncode(const BbpfHeader& h, const std::vector<BbpfBlock>& blocks);
bool BbpfDecode(const std::vector<u8>& bytes, BbpfHeader* h, std::vector<BbpfBlock>* blocks);
bool BbpfWriteFile(const std::string& path, const BbpfHeader& h, const std::vector<BbpfBlock>& blocks);
/// The visible tree / the raw own pool of a store as BBPF blocks.
std::vector<BbpfBlock> CollectVisible(const FlagStore& s);
std::vector<BbpfBlock> CollectPool(const FlagStore& s);

// ---- the game side (gpu/shim/party only; main thread unless noted) ----

enum class Role { None, Host, Guest };

/// Reads the env (BB_PARTY_PROGRESS, BB_PARTY_FLAG_DUMP, ...) and builds the tables. Idempotent.
void Init();
bool Enabled();
/// After coop::HooksInit (bbgpu_patch_image, before the game runs): installs the four
/// byte-verified entry hooks. Returns how many were installed.
int InstallHooks();
/// Host or Guest of the party session (any thread). Host starts capturing.
void SetRole(Role role);
Role CurrentRole();
/// The directory for BB_PARTY_FLAG_DUMP files ("<user dir>/party"); default from the env.
void SetDumpDir(const std::string& dir);
/// Once a frame on the main thread (CoopTick); `s` = ReadGameState(...) of this frame.
void Tick(const GameSnapshot& s);
/// Session boundaries (dumps; guest: the cursor is kept across rejoins of the same host epoch).
void SessionStarted(const char* label);
void SessionEnded();

// Host.
/// C3 hook point (party_items): called on the main thread, outside every progress lock, once for
/// each item_lot_picked flag the host's hooks / diff see go 0 -> 1 (incl. treasure flags
/// 5AABZnnn and the global 5000-5299). nullptr unregisters. The director wires it to
/// coop::OnHostFlagSet once party_items lands; either side can be merged first.
using ItemFlagObserver = void (*)(u32 id);
void SetItemFlagObserver(ItemFlagObserver fn);
/// Changes captured since the last call (in seq order; item_lot_picked changes are included for
/// logs/diffs but guests never apply them). Any thread.
std::vector<FlagChange> PopHostFlagChanges();
/// The host's candidate flags as of the last scan; false before the first full scan. Any thread.
bool FullSnapshot(FlagSnapshot* out);
u64 HostEpoch();

// Guest. Any thread (the link callback): queued, applied by Tick at a safe point.
void ApplyHostFlags(u64 epoch, const std::vector<FlagChange>& changes);
void ApplyHostSnapshot(const FlagSnapshot& snap);
/// Parses the EVENT body and queues it; false (and *error) on a bad body or another name.
bool OnLinkEvent(const std::string& name, const std::string& body, std::string* error = nullptr);
void SetPolicy(const Policy& p);
Policy CurrentPolicy();

// Flag access (main thread; the live game store).
bool ReadFlag(u32 id, bool* value);
bool WriteFlagOwnSave(u32 id, bool value);

struct Stats {
    u64 hook_hits = 0, hook_candidates = 0, scans = 0, host_changes = 0;
    u64 guest_received = 0, guest_applied = 0, guest_skipped = 0, guest_dropped_seq = 0;
    std::size_t guest_pending = 0;
};
Stats GetStats();

} // namespace coop::progress
