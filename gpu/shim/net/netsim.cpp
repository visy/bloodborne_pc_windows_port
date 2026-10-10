// SPDX-License-Identifier: GPL-3.0-or-later
// Network simulator (netsim.h): a delay line in front of sendto.
#include "netsim.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace bbnet::netsim {

namespace {

struct Preset {
    const char* name;
    const char* spec;
};
constexpr Preset kPresets[] = {
    {"lan", "lat=1,jitter=0.5"},
    {"wifi", "lat=5,jitter=5,loss=0.5"},
    {"dsl", "lat=25,jitter=8,loss=0.5,reorder=0.2"},
    {"mobile", "lat=60,jitter=25,loss=2,dup=0.2,reorder=1"},
    {"bad", "lat=150,jitter=60,loss=8,dup=2,reorder=4"},
};

bool heap_later(const Packet& a, const Packet& b) {
    if (a.due != b.due) return a.due > b.due;
    return a.seq > b.seq;
}

}  // namespace

bool parse(const char* spec, Config* out, std::string* error) {
    *out = Config{};
    if (!spec) return true;
    std::string s(spec);
    if (s.empty() || s == "0" || s == "off") return true;
    for (const Preset& p : kPresets) {
        if (s == p.name) s = p.spec;
    }
    std::size_t at = 0;
    while (at < s.size()) {
        std::size_t end = s.find(',', at);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(at, end - at);
        at = end + 1;
        if (item.empty()) continue;
        const std::size_t eq = item.find('=');
        if (eq == std::string::npos) {
            if (error) *error = "netsim: expected key=value, got '" + item + "'";
            return false;
        }
        const std::string key = item.substr(0, eq), value = item.substr(eq + 1);
        char* rest = nullptr;
        const double v = std::strtod(value.c_str(), &rest);
        if (value.empty() || (rest && *rest) || v < 0) {
            if (error) *error = "netsim: bad value for " + key + ": '" + value + "'";
            return false;
        }
        if (key == "lat") out->lat_ms = v;
        else if (key == "jitter") out->jitter_ms = v;
        else if (key == "loss") out->loss_pct = std::min(v, 100.0);
        else if (key == "dup") out->dup_pct = std::min(v, 100.0);
        else if (key == "reorder") out->reorder_pct = std::min(v, 100.0);
        else if (key == "seed") out->seed = static_cast<std::uint64_t>(v);
        else {
            if (error) *error = "netsim: unknown key '" + key + "'";
            return false;
        }
    }
    out->enabled = true;
    return true;
}

std::string describe(const Config& c) {
    if (!c.enabled) return "off";
    char buf[160];
    std::snprintf(buf, sizeof(buf), "lat=%g,jitter=%g,loss=%g,dup=%g,reorder=%g,seed=%llu", c.lat_ms, c.jitter_ms,
                  c.loss_pct, c.dup_pct, c.reorder_pct, static_cast<unsigned long long>(c.seed));
    return buf;
}

Queue::Queue(const Config& c) : cfg_(c), rng_(c.seed) {}

double Queue::uniform() {
    return std::uniform_real_distribution<double>(0.0, 1.0)(rng_);
}

Clock::duration Queue::delay() {
    double ms = cfg_.lat_ms;
    if (cfg_.jitter_ms > 0) ms += (uniform() * 2.0 - 1.0) * cfg_.jitter_ms;
    if (ms < 0) ms = 0;
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(ms));
}

int Queue::submit(Clock::time_point now, std::uintptr_t fd, std::uint32_t addr, std::uint16_t port, const void* data,
                  std::size_t len) {
    ++stats_.submitted;
    if (cfg_.loss_pct > 0 && uniform() * 100.0 < cfg_.loss_pct) {
        ++stats_.dropped;
        return 0;
    }
    const int copies = (cfg_.dup_pct > 0 && uniform() * 100.0 < cfg_.dup_pct) ? 2 : 1;
    if (copies == 2) ++stats_.duplicated;
    const std::uint64_t dest = addr | (static_cast<std::uint64_t>(port) << 32);
    for (int i = 0; i < copies; ++i) {
        Packet p;
        p.due = now + delay();
        p.seq = seq_++;
        p.fd = fd;
        p.addr = addr;
        p.port = port;
        p.data.assign(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + len);
        Clock::time_point& last = last_due_[dest];
        if (cfg_.reorder_pct > 0 && uniform() * 100.0 < cfg_.reorder_pct) {
            // Held back past the datagrams that follow it.
            const double extra = std::max(5.0, cfg_.jitter_ms * 2.0 + cfg_.lat_ms * 0.5 + 5.0);
            p.due += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(extra));
            ++stats_.reordered;
        } else if (p.due < last) {
            p.due = last;  // in order: never ahead of the destination's previous datagram
        } else {
            last = p.due;
        }
        heap_.push_back(std::move(p));
        std::push_heap(heap_.begin(), heap_.end(), heap_later);
    }
    return copies;
}

void Queue::take_due(Clock::time_point now, std::vector<Packet>& out) {
    while (!heap_.empty() && heap_.front().due <= now) {
        std::pop_heap(heap_.begin(), heap_.end(), heap_later);
        out.push_back(std::move(heap_.back()));
        heap_.pop_back();
        ++stats_.delivered;
    }
}

bool Queue::next_due(Clock::time_point* when) const {
    if (heap_.empty()) return false;
    *when = heap_.front().due;
    return true;
}

// --- The process-wide simulator ----------------------------------------------

namespace {

struct Sim {
    std::mutex mu;
    std::condition_variable cv;
    Queue* queue = nullptr;  // never freed: the sender thread lives as long as the process
    bool thread_started = false;
};
Sim& sim() {
    static Sim* s = new Sim;
    return *s;
}

void sender() {
    Sim& s = sim();
    std::vector<Packet> due;
    std::unique_lock<std::mutex> lk(s.mu);
    for (;;) {
        Clock::time_point when;
        if (!s.queue->next_due(&when)) {
            s.cv.wait(lk);
            continue;
        }
        const Clock::time_point now = Clock::now();
        if (when > now) {
            s.cv.wait_until(lk, when);
            continue;
        }
        due.clear();
        s.queue->take_due(now, due);
        lk.unlock();
        for (const Packet& p : due) {
            sockaddr_in sa{};
            sa.sin_family = AF_INET;
            sa.sin_addr.s_addr = p.addr;
            sa.sin_port = p.port;
#if defined(_WIN32)
            ::sendto(static_cast<SOCKET>(p.fd), reinterpret_cast<const char*>(p.data.data()),
                     static_cast<int>(p.data.size()), 0, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
#else
            ::sendto(static_cast<int>(p.fd), p.data.data(), p.data.size(), 0, reinterpret_cast<const sockaddr*>(&sa),
                     sizeof(sa));
#endif
        }
        lk.lock();
    }
}

}  // namespace

void configure(const Config& c) {
    Sim& s = sim();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.queue) return;  // once per process
    if (!c.enabled) return;
    s.queue = new Queue(c);
    std::printf("Net: network simulator on (%s)\n", describe(c).c_str());
}

bool enabled() {
    Sim& s = sim();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.queue != nullptr;
}

Stats stats() {
    Sim& s = sim();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.queue ? s.queue->stats() : Stats{};
}

int sendto(std::uintptr_t fd, const void* data, std::size_t len, std::uint32_t addr, std::uint16_t port) {
    Sim& s = sim();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.queue) return -1;
        s.queue->submit(Clock::now(), fd, addr, port, data, len);
        if (!s.thread_started) {
            s.thread_started = true;
            std::thread(sender).detach();
        }
    }
    s.cv.notify_all();
    return static_cast<int>(len);
}

}  // namespace bbnet::netsim
