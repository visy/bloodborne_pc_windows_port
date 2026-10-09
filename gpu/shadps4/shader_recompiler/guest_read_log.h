// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstring>
#include <vector>
#include "common/types.h"

namespace Shader {

/**
 * bbport: guest memory that the shader translator reads besides the program code.
 *
 * Translation (TranslateProgram, EmitSPIRV) and StageSpecialization read guest memory through
 * pointers held in user data: V# tables of the fetch shader, the fetch shader code itself, the
 * tessellation constant buffer and the resource tables walked by the SRT walker. While a shader
 * is compiled in game, a Record log active on that thread keeps a copy of every such read. The
 * pipeline cache stores it with the shader (ShaderSource blob) and, to rebuild the cache for
 * another GPU without the game, a Replay log serves the same reads from the copy.
 *
 * Every guest memory access of the translator must go through ReadGuest / the hooks below
 * (Info::ReadUdReg, Info::ReadTessConstantBuffer, Info::RefreshFlatBuf, ParseFetchShader).
 * A read that misses the log during a replay marks it failed and the shader is not rebuilt.
 */
class GuestReadLog {
public:
    enum class Mode : u8 { Record, Replay };

    struct Range {
        VAddr addr{};
        std::vector<u8> bytes;
    };

    /// Result of one SRT walker run: the flattened user data and the walker code that produced
    /// it (the code fixes the layout of the buffer).
    struct FlatBuf {
        std::vector<u8> walker;
        std::vector<u32> data;
    };

    explicit GuestReadLog(Mode mode_) : mode{mode_} {}

    /// Log of the current thread, or null (regular translation without recording).
    static inline thread_local GuestReadLog* active = nullptr;

    struct Scope {
        explicit Scope(GuestReadLog* log) : prev{active} {
            active = log;
        }
        ~Scope() {
            active = prev;
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        GuestReadLog* prev;
    };

    void Read(void* dst, VAddr addr, size_t size) {
        if (mode == Mode::Record) {
            std::memcpy(dst, reinterpret_cast<const void*>(addr), size);
            Add(addr, dst, size);
            return;
        }
        if (const u8* src = Find(addr, size)) {
            std::memcpy(dst, src, size);
        } else {
            failed = true;
            std::memset(dst, 0, size);
        }
    }

    /// Record: keeps a copy of [addr, addr + size). The same bytes read twice with different
    /// contents (the guest wrote them meanwhile) make the log inconsistent.
    void Add(VAddr addr, const void* src, size_t size) {
        if (const u8* known = Find(addr, size)) {
            if (std::memcmp(known, src, size) != 0) {
                inconsistent = true;
            }
            return;
        }
        const auto* bytes = static_cast<const u8*>(src);
        ranges.push_back({addr, std::vector<u8>(bytes, bytes + size)});
    }

    /// Recorded bytes covering [addr, addr + size), or null.
    [[nodiscard]] const u8* Find(VAddr addr, size_t size) const {
        for (const auto& range : ranges) {
            if (addr >= range.addr && addr - range.addr + size <= range.bytes.size()) {
                return range.bytes.data() + (addr - range.addr);
            }
        }
        return nullptr;
    }

    /// Recorded range starting at addr (fetch shader code, whose length is known only after
    /// parsing it), or null.
    [[nodiscard]] const Range* FindStart(VAddr addr) const {
        for (const auto& range : ranges) {
            if (range.addr == addr) {
                return &range;
            }
        }
        return nullptr;
    }

    /// Runs (Record) or replays (Replay) the SRT walker that fills `dst`.
    template <typename Walker>
    void Walk(Walker walker, const u8* walker_code, size_t walker_size, const u32* user_data,
              u32* dst, size_t dst_size) {
        if (mode == Mode::Record) {
            walker(user_data, dst);
            flat_bufs.push_back({std::vector<u8>(walker_code, walker_code + walker_size),
                                 std::vector<u32>(dst, dst + dst_size)});
            return;
        }
        if (next_flat >= flat_bufs.size()) {
            failed = true;
            return;
        }
        const auto& recorded = flat_bufs[next_flat++];
        if (recorded.data.size() != dst_size ||
            !SameWalker(recorded.walker, walker_code, walker_size)) {
            failed = true;
            return;
        }
        std::memcpy(dst, recorded.data.data(), dst_size * sizeof(u32));
    }

    Mode mode;
    bool failed{};       ///< Replay: a read was not recorded
    bool inconsistent{}; ///< Record: guest memory changed during the compilation
    std::vector<Range> ranges;
    std::vector<FlatBuf> flat_bufs;
    size_t next_flat{};

private:
    /// Walker code equality. A walker that faulted in game had the faulting load patched into
    /// `xor reg, reg` + nops (flatten_extended_userdata_pass.cpp); it fills the same layout.
    static bool SameWalker(const std::vector<u8>& recorded, const u8* code, size_t size) {
        if (recorded.size() != size) {
            return false;
        }
        size_t i = 0;
        while (i < size) {
            if (recorded[i] == code[i]) {
                ++i;
                continue;
            }
            const bool patch =
                i + 3 <= size &&
                ((recorded[i] == 0x48 && recorded[i + 1] == 0x31 && recorded[i + 2] == 0xFF) ||
                 (recorded[i] == 0x45 && recorded[i + 1] == 0x31 && recorded[i + 2] == 0xD2));
            // The original load: REX, 8B /r with a base register (+SIB) and a displacement.
            if (!patch || i + 3 > size || (code[i] & 0xF0) != 0x40 || code[i + 1] != 0x8B) {
                return false;
            }
            const u8 modrm = code[i + 2];
            const u32 mod = modrm >> 6;
            if (mod == 3) {
                return false;
            }
            const size_t length =
                3 + ((modrm & 7) == 4 ? 1 : 0) + (mod == 1 ? 1 : (mod == 2 ? 4 : 0));
            if (i + length > size) {
                return false;
            }
            for (size_t j = i + 3; j < i + length; ++j) {
                if (recorded[j] != 0x90) {
                    return false;
                }
            }
            i += length;
        }
        return true;
    }
};

/// Guest memory read of the translator: direct, recorded or replayed (see GuestReadLog).
inline void ReadGuest(void* dst, VAddr addr, size_t size) {
    if (auto* log = GuestReadLog::active) [[unlikely]] {
        log->Read(dst, addr, size);
        return;
    }
    std::memcpy(dst, reinterpret_cast<const void*>(addr), size);
}

} // namespace Shader
