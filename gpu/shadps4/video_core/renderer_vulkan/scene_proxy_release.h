// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from Nightwatch (NW a624786, 0.3.0), whose engine is based on "Lantern Engine by Awaken
// (https://github.com/Awakenchan/LanternEngine)" (0.9.39 beta 31), GPL-3 with the GPL section 7
// additional terms of its NOTICE (kept in licenses/). MODIFIED for bbport: no functional change
// from the original.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace Vulkan {

class ProxyReleases {
public:
    void Add(std::uint64_t uid) {
        std::scoped_lock lock{mutex};
        uids.push_back(uid);
        waiting.store(true, std::memory_order_release);
    }
    std::vector<std::uint64_t> Take() {
        if (!waiting.load(std::memory_order_acquire)) {
            return {};
        }
        std::scoped_lock lock{mutex};
        waiting.store(false, std::memory_order_relaxed);
        return std::exchange(uids, {});
    }

private:
    std::mutex mutex;
    std::vector<std::uint64_t> uids;
    std::atomic<bool> waiting{false};
};

template <class Map, class KeyOf, class Out>
std::size_t TakeProxies(Map& entries, std::uint64_t uid, std::uint32_t levels, KeyOf key_of, Out& out) {
    std::size_t taken = 0;
    for (std::uint32_t level = 0; level < levels; ++level) {
        const auto it = entries.find(key_of(uid, level));
        if (it == entries.end()) {
            continue;
        }
        out.push_back(std::move(it->second));
        entries.erase(it);
        ++taken;
    }
    return taken;
}

}
