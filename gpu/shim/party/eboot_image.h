// SPDX-License-Identifier: GPL-3.0-or-later
// The game executable as the co-op identity sees it: the loaded image (the load segments at their
// virtual addresses, zeros between and after), not the file. A SELF-wrapped eboot.bin and the raw
// ELF of the same game (eboot.elf, an Itemzflow-style dump; Mrsuss60 / facundo1701, PR #189) give
// the same bytes, as in scripts/game_check.py image_sha256, so players on either file can join
// each other. Header only so a unit test can use it (tests/test_eboot_image.cpp).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace bbparty {

// Calls sink(const void* data, size_t size) with the image in order. False when the file is
// neither a plain-text SELF nor an ELF, or its load segments cannot be read.
template <class Sink>
bool eboot_image_stream(const std::string& path, Sink&& sink) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::uint64_t file_size = static_cast<std::uint64_t>(f.tellg());
    auto read_at = [&](std::uint64_t off, void* dst, std::uint64_t n) {
        if (off > file_size || n > file_size - off) return false;
        f.clear();
        f.seekg(static_cast<std::streamoff>(off));
        f.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
        return f.gcount() == static_cast<std::streamsize>(n);
    };
    auto u16 = [](const std::uint8_t* p) { return static_cast<std::uint32_t>(p[0] | p[1] << 8); };
    auto u32 = [](const std::uint8_t* p) {
        return static_cast<std::uint32_t>(p[0] | p[1] << 8 | p[2] << 16 | std::uint32_t(p[3]) << 24);
    };
    auto u64 = [&](const std::uint8_t* p) {
        return static_cast<std::uint64_t>(u32(p)) | static_cast<std::uint64_t>(u32(p + 4)) << 32;
    };

    std::uint8_t magic[4];
    if (!read_at(0, magic, 4)) return false;
    std::uint64_t elf_base = 0;
    std::vector<std::uint8_t> self_table;  // SELF segment entries: flags, offset, size, mem (4 x u64)
    std::uint32_t self_count = 0;
    if (std::memcmp(magic, "\x7f" "ELF", 4) == 0) {
        elf_base = 0;
    } else if (std::memcmp(magic, "O\x15=\x1d", 4) == 0) {
        std::uint8_t c[2];
        if (!read_at(24, c, 2)) return false;
        self_count = u16(c);
        self_table.resize(std::size_t(self_count) * 32);
        if (self_count && !read_at(32, self_table.data(), self_table.size())) return false;
        elf_base = 32 + std::uint64_t(self_count) * 32;
    } else {
        return false;
    }
    std::uint8_t eh[64];
    if (!read_at(elf_base, eh, 64) || std::memcmp(eh, "\x7f" "ELF\x02\x01\x01", 7) != 0 ||
        u16(eh + 18) != 62 || u16(eh + 54) != 56)
        return false;
    const std::uint64_t phoff = u64(eh + 32);
    const std::uint32_t phnum = u16(eh + 56);
    if (phnum == 0 || phnum >= 256) return false;

    struct Load { std::uint64_t vaddr, filesz, memsz, file_off; };
    std::vector<Load> loads;
    for (std::uint32_t i = 0; i < phnum; ++i) {
        std::uint8_t ph[56];
        if (!read_at(elf_base + phoff + std::uint64_t(i) * 56, ph, 56)) return false;
        const std::uint32_t type = u32(ph);
        if (type != 1 && type != 0x61000010) continue;
        Load l{u64(ph + 16), u64(ph + 32), u64(ph + 40), u64(ph + 8)};
        if (l.filesz > l.memsz) return false;
        if (self_count) {
            // The data of program header i sits where the SELF segment entry for it says.
            bool found = false;
            for (std::uint32_t s = 0; s < self_count && !found; ++s) {
                const std::uint8_t* e = self_table.data() + std::size_t(s) * 32;
                const std::uint64_t flags = u64(e);
                if (!(flags & 0x800) || (flags & 10) || ((flags >> 20) & 4095) != i) continue;
                if (u64(e + 16) != l.filesz || u64(e + 24) != l.filesz) return false;
                l.file_off = u64(e + 8);
                found = true;
            }
            if (!found && l.filesz) return false;
        }
        loads.push_back(l);
    }
    if (loads.empty()) return false;
    std::sort(loads.begin(), loads.end(), [](const Load& a, const Load& b) { return a.vaddr < b.vaddr; });
    std::uint64_t pos_end = 0;
    std::vector<char> buf(1 << 20);
    auto zeros = [&](std::uint64_t n) {
        std::vector<char> z(1 << 20, 0);
        while (n) {
            const std::uint64_t k = std::min<std::uint64_t>(n, z.size());
            sink(z.data(), static_cast<std::size_t>(k));
            n -= k;
        }
    };
    for (const Load& l : loads) {
        if (pos_end > l.vaddr || l.memsz > (512ull << 20) || l.vaddr + l.memsz > (512ull << 20)) return false;
        zeros(l.vaddr - pos_end);
        for (std::uint64_t done = 0; done < l.filesz;) {
            const std::uint64_t k = std::min<std::uint64_t>(l.filesz - done, buf.size());
            if (!read_at(l.file_off + done, buf.data(), k)) return false;
            sink(buf.data(), static_cast<std::size_t>(k));
            done += k;
        }
        zeros(l.memsz - l.filesz);
        pos_end = l.vaddr + l.memsz;
    }
    return true;
}

}  // namespace bbparty
