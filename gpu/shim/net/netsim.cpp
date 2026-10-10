// SPDX-License-Identifier: GPL-3.0-or-later
// Network simulator (netsim.h): a delay line in front of sendto.
#include "netsim.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <algorithm>
#include <cmath>
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
    {"dsl", "lat=80,jitter=20,loss=2,dup=0.5,reorder=1"},
    {"mobile", "lat=60,jitter=25,loss=2,dup=0.2,reorder=1"},
    {"bad", "lat=200,jitter=80,loss=8"},
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
    // A preset name, alone or first: "dsl" or "dsl,outage=5000,every=60000".
    {
        const std::size_t comma = s.find(',');
        const std::string head = s.substr(0, comma);
        for (const Preset& p : kPresets) {
            if (head == p.name) s = std::string(p.spec) + (comma == std::string::npos ? "" : s.substr(comma));
        }
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
        else if (key == "outage") out->outage_ms = v;
        else if (key == "every") out->outage_every_ms = v;
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
    char buf[200];
    std::snprintf(buf, sizeof(buf), "lat=%g,jitter=%g,loss=%g,dup=%g,reorder=%g,seed=%llu", c.lat_ms, c.jitter_ms,
                  c.loss_pct, c.dup_pct, c.reorder_pct, static_cast<unsigned long long>(c.seed));
    std::string s = buf;
    if (c.outage_ms > 0 && c.outage_every_ms > 0) {
        std::snprintf(buf, sizeof(buf), ",outage=%g,every=%g", c.outage_ms, c.outage_every_ms);
        s += buf;
    }
    return s;
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

void Queue::blackout(Clock::time_point from, Clock::time_point until) {
    black_from_ = from;
    black_until_ = until;
}

bool Queue::in_outage(Clock::time_point now) {
    if (!started_) {
        started_ = true;
        first_ = now;
    }
    if (now >= black_from_ && now < black_until_) return true;
    if (cfg_.outage_ms <= 0 || cfg_.outage_every_ms <= 0) return false;
    const double since = std::chrono::duration<double, std::milli>(now - first_).count();
    if (since < cfg_.outage_every_ms) return false;
    return std::fmod(since, cfg_.outage_every_ms) < cfg_.outage_ms;
}

int Queue::submit(Clock::time_point now, std::uintptr_t fd, std::uint32_t addr, std::uint16_t port, const void* data,
                  std::size_t len) {
    ++stats_.submitted;
    if (in_outage(now)) {
        ++stats_.dropped;
        ++stats_.blacked_out;
        return 0;
    }
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

// --- Waiter ---------------------------------------------------------------------

#if defined(_WIN32)
Waiter::Waiter() {
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer_) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);  // before Windows 10 1803
}
Waiter::~Waiter() {
    if (timer_) CloseHandle(timer_);
    if (event_) CloseHandle(event_);
}
void Waiter::wait_until(Clock::time_point t) {
    const auto now = Clock::now();
    if (t <= now) return;
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t - now).count();
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(us) * 10;  // relative, 100 ns units
    HANDLE hs[2] = {event_, timer_};
    if (timer_ && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        CancelWaitableTimer(timer_);
    } else {
        WaitForSingleObject(event_, static_cast<DWORD>((us + 999) / 1000));
    }
}
void Waiter::notify() { SetEvent(event_); }
#else
Waiter::Waiter() = default;
Waiter::~Waiter() = default;
void Waiter::wait_until(Clock::time_point t) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_until(lk, t, [&] { return flag_; });
    flag_ = false;
}
void Waiter::notify() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        flag_ = true;
    }
    cv_.notify_one();
}
#endif

// --- The process-wide simulator ----------------------------------------------

namespace {

struct Sim {
    std::mutex mu;
    Waiter waiter;
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
        const Clock::time_point now = Clock::now();
        if (!s.queue->next_due(&when)) when = now + std::chrono::seconds(1);
        if (when > now) {
            lk.unlock();
            s.waiter.wait_until(when);  // a submit wakes it (the event stays set if it came first)
            lk.lock();
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
    s.waiter.notify();
    return static_cast<int>(len);
}

}  // namespace bbnet::netsim
