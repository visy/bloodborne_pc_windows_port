// SPDX-License-Identifier: GPL-3.0-or-later
// Network simulator for the party transport: BB_NET_SIM="lat=80,jitter=20,loss=2,dup=0.5,reorder=1,seed=7"
// (or a preset: lan, wifi, dsl, mobile, bad; a preset may be followed by overrides, "dsl,seed=3")
// delays, drops, duplicates and reorders every UDP datagram the game's sockets send
// (net_socket.cpp). lat/jitter are one-way milliseconds, loss/dup/reorder percentages. Without
// reorder a destination's datagrams keep their order (jitter only spreads them); a reordered one
// is held back so later ones overtake it. Burst outages: outage=<ms>,every=<ms> drops everything
// for `outage` ms once every `every` ms (the first one `every` ms after the first datagram).
// Presets: lan = lat 1 jitter 0.5; wifi = lat 5 jitter 5 loss 0.5; dsl = lat 80 jitter 20
// loss 2 dup 0.5 reorder 1; mobile = lat 60 jitter 25 loss 2 dup 0.2 reorder 1; bad = lat 200
// jitter 80 loss 8.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace bbnet::netsim {

using Clock = std::chrono::steady_clock;

struct Config {
    bool enabled = false;
    double lat_ms = 0, jitter_ms = 0, loss_pct = 0, dup_pct = 0, reorder_pct = 0;
    double outage_ms = 0, outage_every_ms = 0;
    std::uint64_t seed = 1;
};

// Parses a BB_NET_SIM value. Empty, "0" and "off" give a disabled config.
bool parse(const char* spec, Config* out, std::string* error);
std::string describe(const Config& c);

struct Packet {
    Clock::time_point due;
    std::uint64_t seq = 0;
    std::uintptr_t fd = 0;
    std::uint32_t addr = 0;  // network order
    std::uint16_t port = 0;  // network order
    std::vector<std::uint8_t> data;
};

struct Stats {
    std::uint64_t submitted = 0, dropped = 0, duplicated = 0, reordered = 0, delivered = 0;
    std::uint64_t blacked_out = 0;  // dropped by an outage (counted in dropped too)
};

// The delay line, without threads or sockets (tests drive it with their own clock).
class Queue {
public:
    explicit Queue(const Config& c);
    // Schedules a datagram submitted at `now`; returns the copies queued (0: lost).
    int submit(Clock::time_point now, std::uintptr_t fd, std::uint32_t addr, std::uint16_t port, const void* data,
               std::size_t len);
    // Appends every packet due at `now` to `out`, earliest first.
    void take_due(Clock::time_point now, std::vector<Packet>& out);
    // Drops everything submitted in [from, until) (tests: a burst outage on demand).
    void blackout(Clock::time_point from, Clock::time_point until);
    // True when `now` is inside an outage (configured or blackout()).
    bool in_outage(Clock::time_point now);
    // The earliest due time; false when empty.
    bool next_due(Clock::time_point* when) const;
    std::size_t size() const { return heap_.size(); }
    const Stats& stats() const { return stats_; }
    const Config& config() const { return cfg_; }

private:
    double uniform();
    Clock::duration delay();
    Config cfg_;
    std::mt19937_64 rng_;
    std::vector<Packet> heap_;  // min-heap on (due, seq)
    std::unordered_map<std::uint64_t, Clock::time_point> last_due_;  // per destination
    std::uint64_t seq_ = 0;
    Stats stats_;
    Clock::time_point first_{};
    bool started_ = false;
    Clock::time_point black_from_{}, black_until_{};
};

// A wait that honours millisecond deadlines. On Windows (MinGW) std::condition_variable and
// sleep_for round every timeout up to the 15.6 ms scheduler tick, even under timeBeginPeriod(1),
// which would add ~15 ms to every simulated delay: this waits on an event and a
// high-resolution waitable timer instead. notify() wakes the current or the next wait.
class Waiter {
public:
    Waiter();
    ~Waiter();
    Waiter(const Waiter&) = delete;
    Waiter& operator=(const Waiter&) = delete;
    void wait_until(Clock::time_point t);
    void notify();

private:
#if defined(_WIN32)
    void* event_ = nullptr;
    void* timer_ = nullptr;
#else
    std::mutex mu_;
    std::condition_variable cv_;
    bool flag_ = false;
#endif
};

// Process-wide simulator (configured once by bbnet_configure / the first party socket).
void configure(const Config& c);
bool enabled();
Stats stats();
// Hands a datagram to the simulator's sender thread; returns len (a lost datagram was "sent").
int sendto(std::uintptr_t fd, const void* data, std::size_t len, std::uint32_t addr, std::uint16_t port);

}  // namespace bbnet::netsim
