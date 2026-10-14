// SPDX-License-Identifier: GPL-3.0-or-later
// Party desync detector (F5): see party_desync.h.
#include "party_desync.h"

#include "net/json.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <stdexcept>

namespace coop::desync {

int WorldBucket(int key) {
    if (key < 0) return 0;
    const int b = static_cast<int>(static_cast<long long>(key) * kBuckets / progress::kPoolBlocks);
    return std::min(b, kBuckets - 1);
}

int FlagBucket(u32 block) { return static_cast<int>((block * 2654435761u) >> 27) & (kBuckets - 1); }

Buckets WorldDigest(const progress::WorldImage& full) {
    return WorldDigestOf([&](int key) -> const progress::GroupBytes* {
        const auto it = full.groups.find(static_cast<u32>(key));
        return it == full.groups.end() ? nullptr : &it->second;
    });
}

Buckets FlagsDigest(const std::map<u32, std::vector<u32>>& blocks) {
    Buckets b;
    b.fill(kFnvBasis);
    for (const auto& [block, bits] : blocks) {
        if (bits.empty()) continue;
        u32& h = b[static_cast<std::size_t>(FlagBucket(block))];
        h = Fnv(h, &block, sizeof block);
        for (u32 bit : bits) h = Fnv(h, &bit, sizeof bit);
    }
    return b;
}

// ---- FlagModel ----

void FlagModel::Reset(const progress::FlagSnapshot& s) {
    valid_ = true;
    gap_ = false;
    epoch_ = s.epoch;
    seq_ = s.seq;
    blocks_.clear();
    for (const auto& [block, bits] : s.blocks) {
        if (bits.empty()) continue;
        std::vector<u32> v = bits;
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        blocks_[block] = std::move(v);
    }
}

void FlagModel::Apply(u64 epoch, const std::vector<progress::FlagChange>& changes) {
    if (!valid_ || epoch != epoch_) return;
    for (const progress::FlagChange& c : changes) {
        if (c.seq <= seq_) continue;
        if (c.seq != seq_ + 1) gap_ = true;
        seq_ = c.seq;
        const u32 block = c.id / progress::kBitsPerBlock, bit = c.id % progress::kBitsPerBlock;
        std::vector<u32>& v = blocks_[block];
        const auto it = std::lower_bound(v.begin(), v.end(), bit);
        const bool has = it != v.end() && *it == bit;
        if (c.value && !has) v.insert(it, bit);
        if (!c.value && has) v.erase(it);
        if (v.empty()) blocks_.erase(block);
    }
}

bool FlagModel::ForgetOne() {
    for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
        if (it->second.empty()) continue;
        it->second.erase(it->second.begin());
        if (it->second.empty()) blocks_.erase(it);
        return true;
    }
    return false;
}

std::size_t FlagModel::SetBits() const {
    std::size_t n = 0;
    for (const auto& [block, bits] : blocks_) n += bits.size();
    return n;
}

// ---- JSON ----

namespace {

json::Value BucketsJson(const Buckets& b) {
    json::Value a = json::Value::make_array();
    for (u32 x : b) a.push(static_cast<unsigned>(x));
    return a;
}

bool BucketsFrom(const json::Value& v, Buckets* out) {
    if (v.type != json::Value::Type::Array || v.array.size() != kBuckets) return false;
    for (std::size_t i = 0; i < kBuckets; ++i) {
        const json::Value& x = v.array[i];
        if (x.type != json::Value::Type::Number || !(x.number >= 0) || x.number > 4294967295.0 ||
            x.number != static_cast<double>(static_cast<u32>(x.number)))
            return false;
        (*out)[i] = static_cast<u32>(x.number);
    }
    return true;
}

json::Value IndexJson(const std::vector<int>& v) {
    json::Value a = json::Value::make_array();
    for (int x : v) a.push(x);
    return a;
}

bool IndexFrom(const json::Value* v, std::vector<int>* out) {
    out->clear();
    if (!v) return true;
    if (v->type != json::Value::Type::Array || v->array.size() > kBuckets) return false;
    for (const json::Value& x : v->array) {
        if (x.type != json::Value::Type::Number || !(x.number >= 0) || x.number >= kBuckets ||
            x.number != static_cast<double>(static_cast<int>(x.number)))
            return false;
        out->push_back(static_cast<int>(x.number));
    }
    std::sort(out->begin(), out->end());
    out->erase(std::unique(out->begin(), out->end()), out->end());
    return true;
}

template <class F>
bool Parse(const std::string& text, std::string* error, F body) {
    json::Value v;
    std::string err;
    if (text.size() > 64 * 1024) {
        if (error) *error = "too long";
        return false;
    }
    if (!json::parse(text, v, err)) {
        if (error) *error = err;
        return false;
    }
    try {
        if (v.type != json::Value::Type::Object || json::u32(v, "v") != 1) throw std::runtime_error("unsupported version");
        if (!body(v)) throw std::runtime_error("bad body");
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

}  // namespace

std::string DigestToJson(const Digest& d) {
    json::Value v = json::Value::make_object();
    v.set("v", 1);
    v.set("epoch", json::hex(d.epoch));
    v.set("seq", json::hex(d.seq));
    if (d.has_world) v.set("w", BucketsJson(d.world));
    if (d.has_flags) v.set("f", BucketsJson(d.flags));
    return json::dump(v, 0);
}

bool DigestFromJson(const std::string& text, Digest* out, std::string* error) {
    return Parse(text, error, [&](const json::Value& v) {
        Digest d;
        d.epoch = json::as_u64(json::member(v, "epoch"), "epoch");
        d.seq = json::as_u64(json::member(v, "seq"), "seq");
        if (const json::Value* w = v.find("w")) {
            if (!BucketsFrom(*w, &d.world)) return false;
            d.has_world = true;
        }
        if (const json::Value* f = v.find("f")) {
            if (!BucketsFrom(*f, &d.flags)) return false;
            d.has_flags = true;
        }
        *out = d;
        return true;
    });
}

std::string ResyncToJson(const Resync& r) {
    json::Value v = json::Value::make_object();
    v.set("v", 1);
    v.set("epoch", json::hex(r.epoch));
    v.set("w", IndexJson(r.world));
    v.set("f", IndexJson(r.flags));
    v.set("gap", r.gap ? 1 : 0);
    return json::dump(v, 0);
}

bool ResyncFromJson(const std::string& text, Resync* out, std::string* error) {
    return Parse(text, error, [&](const json::Value& v) {
        Resync r;
        r.epoch = json::as_u64(json::member(v, "epoch"), "epoch");
        if (!IndexFrom(v.find("w"), &r.world) || !IndexFrom(v.find("f"), &r.flags)) return false;
        const json::Value* g = v.find("gap");
        r.gap = g && g->type == json::Value::Type::Number && g->number != 0;
        *out = r;
        return true;
    });
}

progress::WorldImage WorldBuckets(const progress::WorldImage& full, const std::vector<int>& buckets) {
    progress::WorldImage img;
    img.epoch = full.epoch;
    img.full = false;
    bool want[kBuckets] = {};
    for (int b : buckets)
        if (b >= 0 && b < kBuckets) want[b] = true;
    for (int key : progress::WorldMirrorKeys()) {
        if (!want[WorldBucket(key)]) continue;
        const auto it = full.groups.find(static_cast<u32>(key));
        img.groups[static_cast<u32>(key)] = it == full.groups.end() ? progress::GroupBytes{} : it->second;
    }
    return img;
}

// ---- Checker ----

const char* VerdictName(Checker::Verdict v) {
    switch (v) {
    case Checker::Verdict::Skipped: return "skipped";
    case Checker::Verdict::Match: return "match";
    case Checker::Verdict::Resolved: return "resolved";
    case Checker::Verdict::Mismatch: return "mismatch";
    case Checker::Verdict::Asked: return "asked";
    case Checker::Verdict::Persistent: return "persistent";
    }
    return "?";
}

namespace {
std::string List(const std::vector<int>& v) {
    std::string s;
    for (int x : v) s += (s.empty() ? "" : ",") + std::to_string(x);
    return s.empty() ? "-" : s;
}
}  // namespace

Checker::Result Checker::OnDigest(const Digest& host, const Mine& mine, double now_s) {
    ++n_.digests;
    Result r;
    Resync diff;
    diff.epoch = host.epoch;
    bool compared = false;
    if (host.has_world && mine.has_world) {
        compared = true;
        for (std::size_t i = 0; i < kBuckets; ++i)
            if (host.world[i] != mine.world[i]) diff.world.push_back(static_cast<int>(i));
    }
    const FlagModel* m = mine.model;
    if (host.has_flags && m && m->valid() && m->epoch() == host.epoch) {
        if (m->seq() == host.seq) {
            compared = true;
            const Buckets mf = m->Digest();
            for (std::size_t i = 0; i < kBuckets; ++i)
                if (host.flags[i] != mf[i]) diff.flags.push_back(static_cast<int>(i));
            diff.gap = m->gap() && !diff.flags.empty();
        } else if (m->seq() < host.seq) {
            // The digest comes after every change the host sent before it (one ordered stream):
            // a model behind it missed changes.
            compared = true;
            diff.gap = true;
        }
        // Ahead (seq past the host's): the host's tracker restarted within an epoch; its next
        // snapshot resets the model.
    }
    if (!compared) {
        ++n_.skipped;
        r.verdict = Verdict::Skipped;
        r.detail = !mine.has_world && !(m && m->valid()) ? "nothing received yet" : "not comparable yet";
        return r;
    }
    if (diff.empty()) {
        ++n_.matched;
        r.verdict = open_ ? Verdict::Resolved : Verdict::Match;
        if (open_) {
            ++n_.resolved;
            r.detail = "in sync again after " + std::to_string(strikes_) + " resync(s)";
        }
        open_ = false;
        strikes_ = 0;
        persistent_logged_ = false;
        return r;
    }
    ++n_.mismatched;
    r.detail = "world buckets " + List(diff.world) + ", flag buckets " + List(diff.flags) + (diff.gap ? ", seq gap" : "") +
               " (host seq " + std::to_string(host.seq) + ", mine " +
               std::to_string(m && m->valid() ? m->seq() : 0) + ")";
    if (strikes_ >= kMaxStrikes && now_s - first_strike_ >= kStrikeResetS) strikes_ = 0;  // try again
    if (strikes_ >= kMaxStrikes) {
        r.verdict = persistent_logged_ ? Verdict::Mismatch : Verdict::Persistent;
        if (!persistent_logged_) ++n_.persistent;
        persistent_logged_ = true;
        return r;
    }
    if (now_s - last_ask_ < kAskGapS) {
        r.verdict = Verdict::Mismatch;
        return r;
    }
    if (strikes_ == 0) first_strike_ = now_s;
    ++strikes_;
    ++n_.asked;
    open_ = true;
    last_ask_ = now_s;
    r.verdict = Verdict::Asked;
    r.ask = diff;
    return r;
}

bool ServeLimit::Take(int slot, double now_s) {
    auto it = last_.find(slot);
    if (it != last_.end() && now_s - it->second < kServeGapS) return false;
    last_[slot] = now_s;
    return true;
}

// ---- the game side ----

double PeriodFromEnv() {
    const char* v = std::getenv("BB_PARTY_DESYNC_S");
    if (!v || !*v) return kDefaultPeriodS;
    const double s = std::atof(v);
    return s <= 0 ? 0 : std::max(1.0, s);
}

int TestAfterFromEnv() {
    const char* v = std::getenv("BB_PARTY_DESYNC_TEST");
    return v && *v ? std::max(0, std::atoi(v)) : 0;
}

namespace {

struct GuestState {
    std::mutex mu;
    FlagModel model;
    Checker checker;
    std::string pending;
    int test_after = -1;  // -1: not read yet
    u64 matches = 0;
    bool test_fired = false;
};
GuestState& GS() {
    static GuestState s;
    return s;
}

struct HostState {
    std::mutex mu;
    std::vector<std::pair<int, Resync>> queue;
};
HostState& HS() {
    static HostState s;
    return s;
}

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

}  // namespace

void GuestOnChanges(u64 epoch, const std::vector<progress::FlagChange>& changes) {
    GuestState& g = GS();
    std::lock_guard<std::mutex> lk(g.mu);
    g.model.Apply(epoch, changes);
}

void GuestOnSnapshot(const progress::FlagSnapshot& s) {
    GuestState& g = GS();
    std::lock_guard<std::mutex> lk(g.mu);
    g.model.Reset(s);
}

bool GuestOnDigest(const std::string& body, bool has_world, const Buckets& world, Checker::Result* res,
                   bool* test_corrupt, std::string* error) {
    Digest d;
    if (!DigestFromJson(body, &d, error)) return false;
    GuestState& g = GS();
    std::lock_guard<std::mutex> lk(g.mu);
    Mine mine;
    mine.has_world = has_world;
    mine.world = world;
    mine.model = &g.model;
    *res = g.checker.OnDigest(d, mine, Now());
    if (res->verdict == Checker::Verdict::Asked) g.pending = ResyncToJson(res->ask);
    *test_corrupt = false;
    if (g.test_after < 0) g.test_after = TestAfterFromEnv();
    if (res->verdict == Checker::Verdict::Match && g.test_after > 0 && !g.test_fired &&
        ++g.matches >= static_cast<u64>(g.test_after)) {
        g.test_fired = true;
        g.model.ForgetOne();
        *test_corrupt = true;
    }
    return true;
}

bool GuestPopResync(std::string* body) {
    GuestState& g = GS();
    std::lock_guard<std::mutex> lk(g.mu);
    if (g.pending.empty()) return false;
    body->swap(g.pending);
    g.pending.clear();
    return true;
}

void GuestReset() {
    GuestState& g = GS();
    std::lock_guard<std::mutex> lk(g.mu);
    g.model = FlagModel{};
    g.checker = Checker{};
    g.pending.clear();
}

bool HostQueueResync(int slot, const std::string& body, std::string* error) {
    Resync r;
    if (!ResyncFromJson(body, &r, error)) return false;
    HostState& h = HS();
    std::lock_guard<std::mutex> lk(h.mu);
    // One pending request per member: a newer one replaces it (the main thread serves them).
    h.queue.erase(std::remove_if(h.queue.begin(), h.queue.end(), [&](const auto& e) { return e.first == slot; }),
                  h.queue.end());
    if (h.queue.size() >= 16) {
        if (error) *error = "too many resync requests queued";
        return false;
    }
    h.queue.emplace_back(slot, std::move(r));
    return true;
}

std::vector<std::pair<int, Resync>> HostPopResyncs() {
    HostState& h = HS();
    std::lock_guard<std::mutex> lk(h.mu);
    std::vector<std::pair<int, Resync>> out;
    out.swap(h.queue);
    return out;
}

}  // namespace coop::desync
