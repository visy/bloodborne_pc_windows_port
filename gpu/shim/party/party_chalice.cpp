// SPDX-License-Identifier: GPL-3.0-or-later
// Chalice Dungeons in party play: the local game's dungeon (party_chalice.h).
#include "party_chalice.h"

#include "net/json.h"

#include <algorithm>
#include <iterator>

namespace coop::chalice {

bool IsChaliceGoods(std::uint32_t goods_id) {
    return std::binary_search(std::begin(kChaliceGoods), std::end(kChaliceGoods), goods_id);
}

std::string GoodsToJsonText(const std::vector<std::uint32_t>& ids) {
    json::Value v = json::Value::make_object();
    json::Value a = json::Value::make_array();
    for (std::uint32_t id : ids) {
        a.push(id);
    }
    v.set("goods", a);
    return json::dump(v, 0);
}

bool GoodsFromJsonText(const std::string& text, std::vector<std::uint32_t>* out, std::string* error) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err) || v.type != json::Value::Type::Object) {
        if (error) *error = err.empty() ? "not an object" : err;
        return false;
    }
    const json::Value* a = v.find("goods");
    if (!a || a->type != json::Value::Type::Array || a->array.size() > 64) {
        if (error) *error = "goods: not an array of at most 64";
        return false;
    }
    out->clear();
    for (const json::Value& x : a->array) {
        if (x.type == json::Value::Type::Number && x.number >= 0 && x.number < 100000 &&
            IsChaliceGoods(static_cast<std::uint32_t>(x.number)) &&
            std::find(out->begin(), out->end(), static_cast<std::uint32_t>(x.number)) == out->end()) {
            out->push_back(static_cast<std::uint32_t>(x.number));
        }
    }
    return true;
}

} // namespace coop::chalice

#ifndef BB_PARTY_CHALICE_NO_GAME
#include "coop_hooks.h"
#include "game_state.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace coop::chalice {

namespace {

constexpr std::uint64_t kEventFlagManSlot = 0x553b100;
constexpr std::uint64_t kLoadModeOff = 0x80;

std::atomic<std::uint64_t> g_local{0};
std::mutex g_mu;
std::vector<std::uint32_t> g_host_goods, g_host_pending, g_guest_goods;
bool g_host_dirty = false, g_guest_dirty = false;
double g_host_sent_at = 0;

double Seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

// A summon message as a guest's game receives it (0x14ba340(SSM, ?, wrapper); wrapper +8 -> message):
// +1 type (1 = invited, 2 = ...), +4/+8 ids, +0x10 the host's map (FrpgNetMan +0xa7c), +0x2c the
// host's ChannelId (FrpgNetMan +0xa88 when the map is in area 29, else 0x8000000000000000; the
// sender 0x14b8625), +0x34 room data size. A ChannelId makes the guest fetch and quick-join it.
BB_COOP_SYSV void SummonMessageProbe(u64, u64, u64 wrapper, u64, u64, u64) {
    std::uint64_t msg = 0;
    if (!wrapper || !SafeGet(wrapper + 8, &msg) || !msg) {
        return;
    }
    std::uint8_t type = 0;
    std::uint32_t map = 0, size = 0;
    std::uint64_t channel = 0;
    SafeGet(msg + 1, &type);
    SafeGet(msg + 0x10, &map);
    SafeGet(msg + 0x2c, &channel);
    SafeGet(msg + 0x34, &size);
    std::printf("Party: chalice: summon message type %u, host map 0x%08x, ChannelId 0x%llx, room data %u bytes\n", type,
                map, static_cast<unsigned long long>(channel), size);
    std::fflush(stdout);
}

void InstallProbes() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    HookPrologue(0x14ba340, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &SummonMessageProbe, "chalice: summon message probe");
}

State Read() {
    State s;
    std::uint64_t man = 0;
    const std::uint64_t slot_at = Guest(kEventFlagManSlot);
    if (!slot_at || !SafeGet(slot_at, &man) || man < 0x10000) {
        return s;
    }
    std::int32_t mode = -1;
    if (SafeGet(man + kLoadModeOff, &mode)) {
        s.load_mode = mode;
    }
    std::uint32_t bits = 0;
    if (!ReadEventFlagValue(kSlotFlag, kSlots, &bits)) {
        return s;
    }
    s.valid = true;
    s.slot = SlotFromBits(bits);
    if (s.slot >= 0) {
        const std::uint32_t base = kRecordBase + static_cast<std::uint32_t>(s.slot) * kRecordStride;
        std::uint32_t lo = 0, hi = 0x80000000u;
        if (ReadEventFlagValue(base + kChannelLoBit, 32, &lo) && ReadEventFlagValue(base + kChannelHiBit, 32, &hi)) {
            s.channel = ChannelFromDwords(lo, hi);
        }
    }
    return s;
}

void Tick(bool host) {
    static double next = 0;
    static State last;
    const double now = Seconds();
    if (now < next) {
        return;
    }
    next = now + 2.0;
    const GameSnapshot g = ReadGameState(false);
    if (!g.world_up || g.loading) {
        return;
    }
    const State s = Read();
    if (!s.valid) {
        return;
    }
    {
        // The game's current ChannelId (FrpgNetMan +0xa88: what summon messages and signs carry).
        static std::uint64_t last_net_channel = 1;
        std::uint64_t net = 0, ch = 0;
        const std::uint64_t at = Guest(0x553b120);
        if (at && SafeGet(at, &net) && net && SafeGet(net + 0xa88, &ch) && ch != last_net_channel) {
            last_net_channel = ch;
            std::printf("Party: chalice: FrpgNetMan ChannelId 0x%llx\n", static_cast<unsigned long long>(ch));
            std::fflush(stdout);
        }
    }
    if (host) {
        // The host's chalices: sent when they change, and every 60 s for members that (re)joined.
        std::vector<std::uint32_t> own;
        for (std::uint32_t id : kChaliceGoods) {
            if (GoodsCount(id) > 0) {
                own.push_back(id);
            }
        }
        std::lock_guard<std::mutex> lk(g_mu);
        if (own != g_host_goods || now - g_host_sent_at >= 60.0) {
            if (own != g_host_goods) {
                std::printf("Party: chalice: the host owns %zu chalice(s)\n", own.size());
                std::fflush(stdout);
            }
            g_host_goods = own;
            g_host_pending = own;
            g_host_dirty = true;
            g_host_sent_at = now;
        }
    } else if (s.load_mode == 0) {
        // A guest in its own world: the host's chalices it lacks.
        std::vector<std::uint32_t> want;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_guest_dirty) {
                want = g_guest_goods;
                g_guest_dirty = false;
            }
        }
        for (std::uint32_t id : want) {
            if (IsChaliceGoods(id) && GoodsCount(id) == 0) {
                const bool ok = GiveGoods(id, 1);
                std::printf("Party: chalice: the host's chalice %u %s\n", id, ok ? "given" : "NOT given");
                std::fflush(stdout);
            }
        }
    }
    if (s.load_mode == 0) {
        g_local.store(s.slot >= 0 ? s.channel : 0);
    }
    // The summon request a guest's game got (SummonStepManager [FrpgNetMan+0xc50] +0x158, the
    // passive list: entry +0xc8 state (2 = invited), +0x38 the host's map, +0xb0/+0xb8 room data,
    // +0xc0 the channel request made from the message's ChannelId (0x14ba340), whose +0x10 is the id).
    {
        std::uint64_t net = 0, ssm = 0, head = 0;
        const std::uint64_t at = Guest(0x553b120);
        if (at && SafeGet(at, &net) && net && SafeGet(net + 0xc50, &ssm) && ssm && SafeGet(ssm + 0x158, &head) && head) {
            std::uint64_t node = 0;
            SafeGet(head, &node);
            static std::string last_seen;
            std::string now_seen;
            for (int n = 0; node && node != head && n < 8; ++n) {
                std::uint64_t entry = 0, next = 0, chan = 0, chan_id = 0;
                std::int32_t state = 0, map = 0, size = 0;
                SafeGet(node + 0x10, &entry);
                SafeGet(node, &next);
                if (entry) {
                    SafeGet(entry + 0xc8, &state);
                    SafeGet(entry + 0x38, &map);
                    SafeGet(entry + 0xb0, &size);
                    SafeGet(entry + 0xc0, &chan);
                    if (chan) SafeGet(chan + 0x10, &chan_id);
                    char b[160];
                    std::snprintf(b, sizeof b, " [state %d, map 0x%08x, room data %d bytes, channel request %s (id %llu)]",
                                  state, static_cast<std::uint32_t>(map), size, chan ? "yes" : "none",
                                  static_cast<unsigned long long>(chan_id));
                    now_seen += b;
                }
                node = next;
            }
            if (now_seen != last_seen) {
                last_seen = now_seen;
                std::printf("Party: chalice: summon requests:%s\n", now_seen.empty() ? " none" : now_seen.c_str());
                std::fflush(stdout);
            }
        }
    }
    if (s.slot != last.slot || s.channel != last.channel || s.load_mode != last.load_mode) {
        std::printf("Party: chalice: slot %d, dungeon %llu, load mode %d (map %s)\n", s.slot,
                    static_cast<unsigned long long>(s.channel), s.load_mode, MapName(g.map_id).c_str());
        std::fflush(stdout);
        last = s;
    }
}

std::uint64_t LocalChannel() {
    return g_local.load();
}

bool PopHostGoods(std::vector<std::uint32_t>* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_host_dirty) {
        return false;
    }
    g_host_dirty = false;
    *out = g_host_pending;
    return true;
}

void RequestGuestGoods(const std::vector<std::uint32_t>& ids) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_guest_goods.clear();
    for (std::uint32_t id : ids) {
        if (IsChaliceGoods(id)) {
            g_guest_goods.push_back(id);
        }
    }
    g_guest_dirty = true;
}

} // namespace coop::chalice
#endif
