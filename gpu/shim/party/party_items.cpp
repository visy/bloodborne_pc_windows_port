// SPDX-License-Identifier: GPL-3.0-or-later
// Party items, phase C3 (see party_items.h; the RE is docs/party/items.md).
//
// Every site below was disassembled from smoketest/out/eboot.elf (1.09) and is compared byte for
// byte before anything is written. Offsets are ours (raw ELF VA). The game's code is System V:
// every call into it goes through a BB_COOP_SYSV pointer at Guest(off).
#include "party_items.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

#ifndef BB_PARTY_ITEMS_NO_GAME
#include "coop_hooks.h"
#include "game_state.h"
#include "party_director.h"
#include "party_progress.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <mutex>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#endif

namespace coop {

// ---------------------------------------------------------------------------------------------
// Pure part (unit-tested)
// ---------------------------------------------------------------------------------------------

namespace {

struct LotFlag {
    std::int32_t lot;
    std::int32_t flag;
};
constexpr LotFlag kLotFlags[] = {
#define ITEM_LOT_FLAG(lot, flag) {lot, flag},
#include "party_items.inc"
#undef ITEM_LOT_FLAG
};

struct LedgerRow {
    std::int32_t idx;
    std::int32_t lot;
    std::int32_t done_flag;
    const char* note;
};
constexpr LedgerRow kLedger[] = {
#define ITEM_LEDGER(idx, lot, done, note) {idx, lot, done, note},
#include "party_items.inc"
#undef ITEM_LEDGER
};

struct SourceName {
    ItemSource source;
    const char* name;
};
constexpr SourceName kSourceNames[] = {
    {ItemSource::Unknown, "unknown"}, {ItemSource::Award, "award"}, {ItemSource::Flag, "flag"},
    {ItemSource::Full, "full"},       {ItemSource::Mark, "mark"},   {ItemSource::Test, "test"},
};

const std::unordered_map<std::int32_t, std::int32_t>& FlagToLot() {
    // The lowest lot per flag: the table is sorted by lot, so the first row wins (a chain head
    // before its continuation rows, the NG variant 34000 before 34030).
    static const std::unordered_map<std::int32_t, std::int32_t> m = [] {
        std::unordered_map<std::int32_t, std::int32_t> r;
        for (const LotFlag& e : kLotFlags) {
            r.emplace(e.flag, e.lot);
        }
        return r;
    }();
    return m;
}

constexpr std::int32_t kTreasureBase = 50000000;

bool InRange(std::int32_t v, std::int32_t lo, std::int32_t hi) {
    return v >= lo && v <= hi;
}

} // namespace

const char* ItemSourceName(ItemSource s) {
    for (const SourceName& n : kSourceNames) {
        if (n.source == s) {
            return n.name;
        }
    }
    return "unknown";
}

ItemSource ItemSourceFromName(const std::string& name) {
    for (const SourceName& n : kSourceNames) {
        if (name == n.name) {
            return n.source;
        }
    }
    return ItemSource::Unknown;
}

std::int64_t ItemGrant::DoneFlag() const {
    if (flag >= 0) {
        return flag;
    }
    if (ledger >= 0 && ledger < kItemLedgerSize) {
        return std::int64_t(kItemLedgerBase) + ledger;
    }
    return -1;
}

std::string DescribeItem(const ItemGrant& g) {
    char text[160];
    if (g.flag >= 0) {
        std::snprintf(text, sizeof text, "#%llu lot %d (flag %d, %s)", static_cast<unsigned long long>(g.seq), g.lot,
                      g.flag, ItemSourceName(g.source));
    } else {
        std::snprintf(text, sizeof text, "#%llu lot %d (ledger %d = flag %lld, %s)",
                      static_cast<unsigned long long>(g.seq), g.lot, g.ledger, static_cast<long long>(g.DoneFlag()),
                      ItemSourceName(g.source));
    }
    return text;
}

std::int32_t ItemLotFlag(std::int32_t lot) {
    const LotFlag* b = std::begin(kLotFlags);
    const LotFlag* e = std::end(kLotFlags);
    const LotFlag* it = std::lower_bound(b, e, lot, [](const LotFlag& x, std::int32_t v) { return x.lot < v; });
    return it != e && it->lot == lot ? it->flag : kItemNone;
}

std::int32_t ItemLotForFlag(std::int32_t flag) {
    if (flag <= 0) {
        return kItemNone;
    }
    if (flag >= kTreasureBase && ItemLotFlag(flag - kTreasureBase) == flag) {
        return flag - kTreasureBase;
    }
    const auto& m = FlagToLot();
    const auto it = m.find(flag);
    return it != m.end() ? it->second : kItemNone;
}

std::int32_t ItemLedgerIndex(std::int32_t lot) {
    for (const LedgerRow& r : kLedger) {
        if (r.lot == lot) {
            return r.idx;
        }
    }
    return kItemNone;
}

std::int32_t ItemLedgerDoneFlag(std::int32_t idx) {
    for (const LedgerRow& r : kLedger) {
        if (r.idx == idx) {
            return r.done_flag;
        }
    }
    return kItemNone;
}

std::int32_t ItemLedgerLot(std::int32_t idx) {
    for (const LedgerRow& r : kLedger) {
        if (r.idx == idx) {
            return r.lot;
        }
    }
    return kItemNone;
}

std::size_t ItemLotFlagCount() {
    return std::size(kLotFlags);
}

std::size_t ItemLedgerCount() {
    return std::size(kLedger);
}

bool ItemLotDenied(std::int32_t lot, std::int32_t flag) {
    if (lot <= 0 || lot >= 100000000) {
        return true; // unresolved; Chalice Dungeon lots (1100xxxxx, 2001xxxxx)
    }
    if (lot / 100000 == 29) {
        return true; // area 29 treasure (Chalice Dungeons)
    }
    if (flag > 0 && (flag / 10000000 == 1 || flag / 10000000 == 5) && (flag / 100000) % 100 == 29) {
        return true; // area 29 flags
    }
    // DLC boss rewards whose flags live in the personal block 6000-8999 (Ludwig 3401800 -> 6674,
    // Laurence 3401850 -> 6673): a phantom never gets them (2003[4] is skipped on clients) and
    // the flag is the guest's own, so a replay is exact. Every other lot with a personal flag is
    // a per-player gift (Messenger 10000/10010/10050, DLC NPCs 43120/43130, 43800).
    if (lot == 3401800 || lot == 3401850) {
        return false;
    }
    if (InRange(flag, 6000, 8999)) {
        return true;
    }
    switch (lot) {
    case 10000: case 10010: case 10011: case 10040: case 10050: // Messenger gifts (per player)
    case 10500: case 11500: case 12500:                         // covenant gems (common 9440)
    case 17010: case 24060: case 28000: case 32020:             // common 9100 (repeatable)
    case 43000:                                                 // 13501940 (repeatable)
    case 2100900: case 2100910: case 2100920:                   // 12105064 DLC messenger hats
    case 43802:                                                 // cycle variant of 43800
    case 5020: case 16581:                                      // PvP / NPC kill texts
        return true;
    default:
        break;
    }
    if (InRange(lot, 100000, 101999)) {
        return true; // rune / gem use (common 9500)
    }
    if (InRange(lot, 5500, 5959)) {
        return true; // covenant / PvP lots 5500-5950 and their rows
    }
    return false;
}

std::optional<ItemGrant> ClassifyAward(std::int32_t lot, bool host_only) {
    if (!host_only) {
        return std::nullopt; // clients get it in vanilla (2003[36], GetRateItem_IgnoreMultiPlay)
    }
    ItemGrant g;
    g.lot = lot;
    g.flag = ItemLotFlag(lot);
    g.source = ItemSource::Award;
    if (ItemLotDenied(lot, g.flag)) {
        return std::nullopt;
    }
    if (g.flag < 0) {
        g.ledger = ItemLedgerIndex(lot);
        if (g.ledger < 0) {
            return std::nullopt; // flagless and not in the ledger: no way to give it exactly once
        }
    }
    return g;
}

std::optional<ItemGrant> ClassifyFlag(std::int32_t flag, const std::map<std::int32_t, std::int32_t>& captured) {
    ItemGrant g;
    const auto it = captured.find(flag);
    g.lot = it != captured.end() ? it->second : ItemLotForFlag(flag);
    if (g.lot < 0) {
        return std::nullopt;
    }
    g.flag = flag;
    g.source = ItemSource::Flag;
    if (ItemLotDenied(g.lot, flag)) {
        return std::nullopt;
    }
    return g;
}

json::Value ItemsToJson(const std::vector<ItemGrant>& items) {
    json::Value v = json::Value::make_object();
    json::Value a = json::Value::make_array();
    for (const ItemGrant& g : items) {
        json::Value row = json::Value::make_array();
        row.push(static_cast<unsigned long long>(g.seq)); // < 2^53: exact as a double
        row.push(g.lot);
        row.push(g.flag);
        row.push(g.ledger);
        row.push(ItemSourceName(g.source));
        a.push(row);
    }
    v.set("items", a);
    return v;
}

bool ItemsFromJson(const json::Value& v, std::vector<ItemGrant>* out, std::string* error) {
    try {
        if (v.type != json::Value::Type::Object) {
            throw std::runtime_error("not an object");
        }
        std::vector<ItemGrant> r;
        for (const json::Value& row : json::arr(v, "items")) {
            if (row.type != json::Value::Type::Array || row.array.size() != 5) {
                throw std::runtime_error("items: a row needs [seq, lot, flag, ledger, source]");
            }
            auto integer = [](const json::Value& x, const char* what) -> double {
                if (x.type != json::Value::Type::Number || x.number != static_cast<double>(static_cast<long long>(x.number))) {
                    throw std::runtime_error(std::string("items: ") + what + " is not an integer");
                }
                return x.number;
            };
            ItemGrant g;
            g.seq = static_cast<std::uint64_t>(integer(row.array[0], "seq"));
            const double lot = integer(row.array[1], "lot"), flag = integer(row.array[2], "flag"),
                         ledger = integer(row.array[3], "ledger");
            if (lot <= 0 || lot > 2147483647.0 || flag < -1 || flag > 2147483647.0 || ledger < -1 ||
                ledger >= kItemLedgerSize) {
                throw std::runtime_error("items: value out of range");
            }
            g.lot = static_cast<std::int32_t>(lot);
            g.flag = static_cast<std::int32_t>(flag);
            g.ledger = static_cast<std::int32_t>(ledger);
            if (row.array[4].type != json::Value::Type::String) {
                throw std::runtime_error("items: source is not a string");
            }
            g.source = ItemSourceFromName(row.array[4].string);
            if (g.DoneFlag() < 0) {
                throw std::runtime_error("items: a row needs a flag or a ledger index");
            }
            r.push_back(g);
        }
        *out = std::move(r);
        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
}

std::string ItemsToJsonText(const std::vector<ItemGrant>& items) {
    return json::dump(ItemsToJson(items), 0);
}

bool ItemsFromJsonText(const std::string& text, std::vector<ItemGrant>* out, std::string* error) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err)) {
        if (error) {
            *error = err;
        }
        return false;
    }
    return ItemsFromJson(v, out, error);
}

bool HostItems::Offer(ItemGrant g) {
    const std::int64_t key = g.Key();
    if (key < 0 || !sent_.insert(key).second) {
        return false;
    }
    g.seq = NextSeq();
    if (q_.size() >= kMaxQueue) {
        q_.pop_front();
    }
    q_.push_back(g);
    return true;
}

bool HostItems::Pop(ItemGrant* out) {
    if (q_.empty()) {
        return false;
    }
    *out = q_.front();
    q_.pop_front();
    return true;
}

bool ItemApplyAllowedNow(const ItemApplyState& s) {
    return s.world_up && !s.loading && s.load_mode == 0 && s.own_world == 1 && s.game_data &&
           (s.session_role == 0 /* idle */ || s.session_role == 3 /* hosting in the own world */);
}

std::size_t GuestItems::Offer(const std::vector<ItemGrant>& items) {
    std::size_t taken = 0;
    for (const ItemGrant& g : items) {
        const std::int64_t key = g.Key();
        if (key < 0 || done_.count(key)) {
            continue;
        }
        if (g.source == ItemSource::Mark) {
            q_.push_front(g); // before any replay of the same flag
            ++taken;
            continue;
        }
        if (!queued_.insert(key).second) {
            continue;
        }
        if (q_.size() >= kMaxQueue) {
            continue; // the host's next full list brings it again
        }
        q_.push_back(g);
        ++taken;
    }
    return taken;
}

std::optional<ItemGrant> GuestItems::Step(const ItemApplyState& s, double now) {
    stable_ = ItemApplyAllowedNow(s) ? stable_ + 1 : 0;
    if (q_.empty() || stable_ < kStableTicks) {
        return std::nullopt;
    }
    // Marks only set a flag (no popup): no spacing for them.
    if (q_.front().source != ItemSource::Mark && now - last_apply_ < kSpacing) {
        return std::nullopt;
    }
    ItemGrant g = q_.front();
    q_.pop_front();
    if (g.source != ItemSource::Mark) {
        queued_.erase(g.Key());
        last_apply_ = now;
    }
    return g;
}

// ---------------------------------------------------------------------------------------------
// Game part
// ---------------------------------------------------------------------------------------------
#ifndef BB_PARTY_ITEMS_NO_GAME

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using ull = unsigned long long;
using Clock = std::chrono::steady_clock;

constexpr u64 kAwardLot = 0x17ddc50;   // (MapItemMan*, int lot, char hostOnly)
constexpr u64 kGiveList = 0x17d89f0;   // (MapItemMan*, ItemList*): inventory add + popup
constexpr u64 kMapItemMan = 0x553d6e0; // slot: *(u64*)slot is the first argument of every lot call
constexpr u64 kFlagMan = 0x553b100;    // SprjEventFlagMan slot (+0x80 load mode)
constexpr u64 kIsFlag = 0x13cfc00;     // u8 (man, u32 id, u8* exists)
constexpr u64 kSetFlag = 0x13cfcc0;    // void (man, u32 id, u32 on)
constexpr u64 kWts = 0x5556678;        // WorldTransitionState / GameStateMan slot
constexpr u64 kWtsOwnWorld = 0x1592;   // 1 in the own world, 0 summoned into another

using AwardFn = void(BB_COOP_SYSV*)(u64, u32, u32);
using IsFlagFn = u8(BB_COOP_SYSV*)(u64, u32, u8*);
using SetFlagFn = void(BB_COOP_SYSV*)(u64, u32, u32);

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::printf("Party items: %s\n", line);
    std::fflush(stdout);
}

bool EnvOff(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '0' && !v[1];
}

double Now() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

std::atomic<bool> g_on{false};
std::atomic<bool> g_in_apply{false}; // our own 0x17DDC50 call: the hook stands aside
std::atomic<u32> g_added{0};         // items 0x17D89F0 added during our own award

std::mutex g_host_mu;
HostItems g_host;            // under g_host_mu
std::deque<int> g_full_due;  // under g_host_mu
std::mutex g_guest_mu;
GuestItems g_guest;          // under g_guest_mu
std::vector<ItemGrant> g_test; // BB_PARTY_ITEMS_TEST lots, offered once the tick runs
bool g_test_offered = false;

u64 Ptr(u64 slot_off) {
    u64 p = 0;
    return SafeGet(Guest(slot_off), &p) ? p : 0;
}

int OwnWorld() {
    const u64 wts = Ptr(kWts);
    u8 v = 0xff;
    return wts && SafeGet(wts + kWtsOwnWorld, &v) ? v : -1;
}

int LoadMode(u64 man) {
    std::int32_t m = -1;
    return man && SafeGet(man + 0x80, &m) ? m : -1;
}

PartyRole Role() {
    return PartyDirector::Get().Role();
}

bool IsFlag(u64 man, std::int64_t id) {
    if (!man || id < 0) {
        return false;
    }
    u8 exists = 0;
    return reinterpret_cast<IsFlagFn>(Guest(kIsFlag))(man, static_cast<u32>(id), &exists) != 0;
}

void SetFlag(u64 man, std::int64_t id, bool on = true) {
    if (man && id >= 0) {
        reinterpret_cast<SetFlagFn>(Guest(kSetFlag))(man, static_cast<u32>(id), on ? 1 : 0);
    }
}

void SeedHostSeq() {
    // Wall-clock seeded like travel: a restarted host keeps counting upward.
    const u64 seed = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    std::lock_guard<std::mutex> lk(g_host_mu);
    g_host.SeedSeq(seed);
}

// ---- The award hook (main thread: EMEVD, Lua) ----

BB_COOP_SYSV void AwardEntry(u64, u64 lot_arg, u64 host_only_arg, u64, u64, u64) {
    if (!g_on.load() || g_in_apply.load()) {
        return;
    }
    const std::int32_t lot = static_cast<std::int32_t>(static_cast<u32>(lot_arg));
    const bool host_only = static_cast<u8>(host_only_arg) != 0;
    const PartyRole role = Role();
    const int own = OwnWorld();
    const std::int32_t flag = ItemLotFlag(lot);
    Log("award lot %d (hostOnly %d, flag %d) in %s world, role %s", lot, host_only ? 1 : 0, flag,
        own == 1 ? "the own" : own == 0 ? "another's" : "an unknown",
        role == PartyRole::Host ? "host" : role == PartyRole::Guest ? "guest" : "none");
    if (role == PartyRole::Host && own == 1) {
        const std::optional<ItemGrant> g = ClassifyAward(lot, host_only);
        bool queued = false;
        {
            std::lock_guard<std::mutex> lk(g_host_mu);
            if (flag > 0) {
                g_host.Capture(flag, lot);
            }
            queued = g && g_host.Offer(*g);
        }
        if (g) {
            Log("host award %s %s", DescribeItem(*g).c_str(), queued ? "queued for the party" : "already sent");
        } else if (host_only && flag < 0 && !ItemLotDenied(lot, flag)) {
            Log("host award lot %d: flagless and not in the ledger table; not replayed", lot);
        }
    } else if (role == PartyRole::Guest && own == 0 && !host_only && flag > 0 && !ItemLotDenied(lot, flag)) {
        // We got it in the host's world; its flag went to the overlay only. Set it at home so the
        // host's replay of the same lot gives nothing twice.
        ItemGrant m;
        m.lot = lot;
        m.flag = flag;
        m.source = ItemSource::Mark;
        std::lock_guard<std::mutex> lk(g_guest_mu);
        g_guest.Offer({m});
        Log("guest received lot %d while summoned: flag %d will be set in the own world", lot, flag);
    }
}

// Our own award's inventory adds (evidence for the apply log): ItemList {u32 n, then n x
// {u32 gaitem, u32 itemId | category << 28, u32 count, u32 -1}} from +4 (0x17D8A42..0x17D8A8A).
BB_COOP_SYSV void GiveListEntry(u64, u64 list, u64, u64, u64, u64) {
    if (!g_in_apply.load() || !list) {
        return;
    }
    u32 n = 0;
    if (!SafeGet(list, &n)) {
        return;
    }
    g_added.fetch_add(n);
    for (u32 i = 0; i < n && i < 8; ++i) {
        u32 e[4] = {};
        if (SafeRead(list + 4 + 16ull * i, e, sizeof e)) {
            Log("  inventory add %u/%u: item %u category %u count %u (gaitem 0x%x)", i + 1, n, e[1] & 0x0fffffffu,
                e[1] >> 28, e[2], e[0]);
        }
    }
}

// ---- Parity patches ----

bool WriteImage(u64 off, const u8* bytes, std::size_t n) {
    unsigned char* at = Image() + off;
#ifdef _WIN32
    DWORD old = 0;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    std::memcpy(at, bytes, n);
    DWORD unused = 0;
    VirtualProtect(at, n, old, &unused);
    FlushInstructionCache(GetCurrentProcess(), at, n);
#else
    std::memcpy(at, bytes, n);
#endif
    return true;
}

void Patch(const char* name, u64 off, std::initializer_list<u8> original, std::initializer_list<u8> patched) {
    if (Matches(off, patched.begin(), patched.size())) {
        Log("%s (+0x%llx): already applied", name, ull(off));
    } else if (!Matches(off, original.begin(), original.size())) {
        Log("%s (+0x%llx): MISMATCH, not applied", name, ull(off));
    } else if (!WriteImage(off, patched.begin(), patched.size())) {
        Log("%s (+0x%llx): could not be written", name, ull(off));
    } else {
        Log("%s (+0x%llx): applied", name, ull(off));
    }
}

void ParityPatches() {
    // docs/party/items.md 5, "optional parity patches"; each verified against the 1.09 bytes.
    // 0x18FC93D test cl, cl (isClient); 0x18FC93F jne +6 -> the 0.5 kill-echo multiplier.
    Patch("full kill echoes for clients", 0x18fc93f, {0x75, 0x06}, {0x90, 0x90});
    // 0x133557F cmp eax, 8 (chr type | 8 == 8: host or own-world grey); je skips the halving.
    Patch("full boss clear bonus for phantoms", 0x1335582, {0x74, 0x12}, {0xeb, 0x12});
    // 0x17DD18E setne cl; 0x17DD191 or cl, al -> xor cl, cl: the isGuestDrop filter's only input
    // ([rbp-0x208], read once at 0x17DD75D) is 0, so a client keeps the whole enemy drop.
    Patch("full enemy drops for clients", 0x17dd191, {0x08, 0xc1}, {0x30, 0xc9});
}

void ParseTestLots() {
    const char* v = std::getenv("BB_PARTY_ITEMS_TEST");
    if (!v || !*v) {
        return;
    }
    std::string s(v);
    std::size_t at = 0;
    while (at < s.size()) {
        std::size_t end = s.find(',', at);
        if (end == std::string::npos) {
            end = s.size();
        }
        const long lot = std::strtol(s.substr(at, end - at).c_str(), nullptr, 10);
        at = end + 1;
        if (lot <= 0) {
            continue;
        }
        ItemGrant g;
        g.lot = static_cast<std::int32_t>(lot);
        g.flag = ItemLotFlag(g.lot);
        g.ledger = g.flag < 0 ? ItemLedgerIndex(g.lot) : kItemNone;
        g.source = ItemSource::Test;
        g.seq = g_test.size() + 1;
        if (g.DoneFlag() < 0) {
            Log("BB_PARTY_ITEMS_TEST: lot %d has neither a flag nor a ledger index; skipped", g.lot);
            continue;
        }
        g_test.push_back(g);
        Log("BB_PARTY_ITEMS_TEST: will apply %s in the own world", DescribeItem(g).c_str());
    }
}

void Apply(const ItemGrant& g) {
    const u64 man = Ptr(kFlagMan);
    const u64 mim = Ptr(kMapItemMan);
    const std::int64_t done = g.DoneFlag();
    if (g.source == ItemSource::Mark) {
        const bool was = IsFlag(man, done);
        SetFlag(man, done);
        Log("mark lot %d: flag %lld %s", g.lot, static_cast<long long>(done), was ? "was already set" : "set");
        return;
    }
    if (IsFlag(man, done)) {
        Log("skip %s: already given (flag %lld set)", DescribeItem(g).c_str(), static_cast<long long>(done));
        return;
    }
    if (!mim) {
        Log("skip %s: no MapItemMan", DescribeItem(g).c_str());
        return;
    }
    // The ledger bit goes in first: the give can start a save (run A of the C3 check lost a
    // ledger bit written after the award), so the save that holds the item holds the bit too.
    // A lot that gives nothing clears it again.
    if (g.flag < 0) {
        SetFlag(man, done);
    }
    g_added = 0;
    g_in_apply = true;
    reinterpret_cast<AwardFn>(Guest(kAwardLot))(mim, static_cast<u32>(g.lot), 0);
    g_in_apply = false;
    const u32 added = g_added.load();
    if (g.flag < 0 && added == 0) {
        SetFlag(man, done, false);
    }
    const bool now_set = IsFlag(man, done);
    Log("gave %s: 0x17DDC50(lot %d, 0) added %u item entr%s; flag %lld is %s", DescribeItem(g).c_str(), g.lot, added,
        added == 1 ? "y" : "ies", static_cast<long long>(done), now_set ? "set" : "CLEAR (nothing given)");
}

} // namespace

void InstallItemsPatches() {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) {
        return;
    }
    if (!Image()) {
        Log("no image; items off");
        return;
    }
    const char* party = std::getenv("BB_PARTY");
    if (party && party[0] && !EnvOff("BB_PARTY_FULL_REWARDS")) {
        ParityPatches();
    } else {
        Log("parity patches off (%s)", party && party[0] ? "BB_PARTY_FULL_REWARDS=0" : "not in party mode");
    }
    if (EnvOff("BB_PARTY_ITEMS")) {
        Log("BB_PARTY_ITEMS=0: no item replay");
        return;
    }
    // push rbp; mov rbp, rsp; push r15, r14, r13, r12, rbx (13 bytes; then sub rsp, 0x1a8).
    if (!HookPrologue(kAwardLot, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53},
                      &AwardEntry, "item award (0x17DDC50)")) {
        Log("no award hook: host-only lots are not reported (flag reports still work)");
    }
    HookPrologue(kGiveList, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53},
                 &GiveListEntry, "item give log (0x17D89F0)");
    SeedHostSeq();
    ParseTestLots();
    // C2 hands every host item_lot_picked flag 0 -> 1 to OnHostFlagSet (main thread).
    progress::SetItemFlagObserver(&OnHostFlagSet);
    Log("C2 item-flag observer registered");
    Log("%zu flagged lots, %zu ledger lots (flags %u..%u)", ItemLotFlagCount(), ItemLedgerCount(), kItemLedgerBase,
        kItemLedgerBase + kItemLedgerSize - 1);
    g_on = true;
}

void OnHostFlagSet(std::uint32_t flag) {
    if (!g_on.load() || Role() != PartyRole::Host || flag > 0x7fffffffu) {
        return;
    }
    std::optional<ItemGrant> g;
    bool queued = false;
    {
        std::lock_guard<std::mutex> lk(g_host_mu);
        g = ClassifyFlag(static_cast<std::int32_t>(flag), g_host.Captured());
        queued = g && g_host.Offer(*g);
    }
    if (queued) {
        Log("host flag %u -> %s queued for the party", flag, DescribeItem(*g).c_str());
    }
}

bool PopHostItems(ItemGrant* out) {
    std::lock_guard<std::mutex> lk(g_host_mu);
    return g_host.Pop(out);
}

void RequestHostFullItems(int slot) {
    if (!g_on.load()) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_host_mu);
    if (std::find(g_full_due.begin(), g_full_due.end(), slot) == g_full_due.end()) {
        g_full_due.push_back(slot);
    }
}

bool TakeHostFullItems(int* slot, std::vector<ItemGrant>* out) {
    if (!g_on.load()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_host_mu);
        if (g_full_due.empty()) {
            return false;
        }
    }
    // The host's own save must be live: world up, flag store in mode 0, own world.
    const GameSnapshot gs = ReadGameState(false);
    const u64 man = Ptr(kFlagMan);
    if (!gs.world_up || gs.loading || LoadMode(man) != 0 || OwnWorld() != 1) {
        return false;
    }
    std::vector<ItemGrant> list;
    std::map<std::int32_t, std::int32_t> captured;
    {
        std::lock_guard<std::mutex> lk(g_host_mu);
        *slot = g_full_due.front();
        g_full_due.pop_front();
        captured = g_host.Captured();
    }
    std::set<std::int32_t> seen;
    for (const LotFlag& e : kLotFlags) {
        if (!seen.insert(e.flag).second || !IsFlag(man, e.flag)) {
            continue;
        }
        if (std::optional<ItemGrant> g = ClassifyFlag(e.flag, captured)) {
            g->source = ItemSource::Full;
            list.push_back(*g);
        }
    }
    for (const LedgerRow& r : kLedger) {
        if (IsFlag(man, r.done_flag) && !ItemLotDenied(r.lot, kItemNone)) {
            ItemGrant g;
            g.lot = r.lot;
            g.ledger = r.idx;
            g.source = ItemSource::Full;
            list.push_back(g);
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_host_mu);
        for (ItemGrant& g : list) {
            g.seq = g_host.NextSeq();
        }
    }
    Log("full item list for slot %d: %zu lots", *slot, list.size());
    *out = std::move(list);
    return true;
}

void RequestGuestItems(const std::vector<ItemGrant>& items) {
    if (!g_on.load()) {
        return;
    }
    std::size_t taken, pending;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        taken = g_guest.Offer(items);
        pending = g_guest.Pending();
    }
    Log("guest got %zu grants from the host, %zu new; %zu pending", items.size(), taken, pending);
}

void ItemsTick() {
    if (!g_on.load()) {
        return;
    }
    if (!g_test_offered && !g_test.empty()) {
        g_test_offered = true;
        std::lock_guard<std::mutex> lk(g_guest_mu);
        g_guest.Offer(g_test);
    }
    const GameSnapshot gs = ReadGameState(false);
    ItemApplyState s;
    s.world_up = gs.world_up && gs.map_id != 0xffffffffu; // the player stands in a map
    s.loading = gs.loading;
    s.session_role = gs.session_role;
    s.load_mode = LoadMode(Ptr(kFlagMan));
    s.own_world = OwnWorld();
    s.game_data = gs.player_rec != 0 && Ptr(kMapItemMan) != 0;
    static bool ready_logged = false;
    if (ItemApplyAllowedNow(s) != ready_logged) {
        ready_logged = !ready_logged;
        Log("own-world apply gate %s (%s, load mode %d, own world %d)", ready_logged ? "open" : "closed",
            Describe(gs).c_str(), s.load_mode, s.own_world);
    }
    std::optional<ItemGrant> g;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        g = g_guest.Step(s, Now());
    }
    if (!g) {
        return;
    }
    Apply(*g);
    std::lock_guard<std::mutex> lk(g_guest_mu);
    g_guest.Done(g->Key());
}

#endif // BB_PARTY_ITEMS_NO_GAME

} // namespace coop
