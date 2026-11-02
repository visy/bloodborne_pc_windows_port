// SPDX-License-Identifier: GPL-3.0-or-later
// eboot_image.h: a SELF-wrapped eboot.bin and the raw ELF eboot.elf of the same synthetic game give
// the same image bytes (the co-op identity hashes them), and damaged files are refused.
#include "../gpu/shim/party/eboot_image.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

static void put(Bytes& b, std::size_t at, std::uint64_t v, int n) {
    if (b.size() < at + n) b.resize(at + n);
    for (int i = 0; i < n; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

struct Seg { std::uint32_t type; std::uint64_t vaddr, filesz, memsz; Bytes data; };

// Two load segments (one with bss) and a non-load one; raw ELF layout, data after the headers.
static Bytes elf_header(const std::vector<Seg>& segs, std::uint64_t* offsets) {
    Bytes h;
    const unsigned char id[7] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
    h.assign(id, id + 7);
    h.resize(64);
    put(h, 16, 2, 2); put(h, 18, 62, 2); put(h, 32, 64, 8); put(h, 52, 64, 2); put(h, 54, 56, 2);
    put(h, 56, segs.size(), 2);
    std::uint64_t off = 64 + 56 * segs.size();
    for (std::size_t i = 0; i < segs.size(); ++i) {
        offsets[i] = off;
        const std::size_t e = 64 + 56 * i;
        put(h, e, segs[i].type, 4); put(h, e + 8, off, 8); put(h, e + 16, segs[i].vaddr, 8);
        put(h, e + 32, segs[i].filesz, 8); put(h, e + 40, segs[i].memsz, 8);
        off += segs[i].filesz;
    }
    h.resize(64 + 56 * segs.size());
    return h;
}

static void write(const fs::path& p, const Bytes& b) {
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()),
                                              static_cast<std::streamsize>(b.size()));
}

static Bytes image_of(const fs::path& p, bool* ok) {
    Bytes out;
    *ok = bbparty::eboot_image_stream(p.string(), [&](const void* d, std::size_t n) {
        const auto* c = static_cast<const std::uint8_t*>(d);
        out.insert(out.end(), c, c + n);
    });
    return out;
}

#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main() {
    const fs::path dir = fs::temp_directory_path() / "bb_eboot_image_test";
    fs::create_directories(dir);
    std::vector<Seg> segs = {
        {1, 0x0, 0x20, 0x20, Bytes(0x20, 0xAA)},
        {0x61000010, 0x100, 0x10, 0x30, Bytes(0x10, 0xBB)},  // 0x20 bytes of bss
        {2, 0x400, 0x8, 0x8, Bytes(8, 0xCC)},                // not a load segment
    };
    Bytes expect(0x130, 0);
    std::fill(expect.begin(), expect.begin() + 0x20, 0xAA);
    std::fill(expect.begin() + 0x100, expect.begin() + 0x110, 0xBB);

    std::uint64_t off[3];
    Bytes elf = elf_header(segs, off);
    for (std::size_t i = 0; i < segs.size(); ++i) elf.insert(elf.end(), segs[i].data.begin(), segs[i].data.end());
    write(dir / "eboot.elf", elf);

    // SELF: magic, count at 24, entries at 32, the ELF header + program headers, then the data.
    Bytes self(32 + 3 * 32);
    const unsigned char sm[4] = {'O', 0x15, '=', 0x1d};
    std::copy(sm, sm + 4, self.begin());
    put(self, 24, 3, 2);
    Bytes hdr = elf_header(segs, off);
    hdr.resize(64 + 56 * 3);
    const std::size_t hdr_at = self.size();
    self.insert(self.end(), hdr.begin(), hdr.end());
    while (self.size() % 16) self.push_back(0);
    for (std::size_t i = 0; i < 3; ++i) {
        put(self, 32 + i * 32, 0x800 | (i << 20), 8);
        put(self, 32 + i * 32 + 8, self.size(), 8);
        put(self, 32 + i * 32 + 16, segs[i].filesz, 8);
        put(self, 32 + i * 32 + 24, segs[i].filesz, 8);
        self.insert(self.end(), segs[i].data.begin(), segs[i].data.end());
    }
    (void)hdr_at;
    write(dir / "eboot.bin", self);

    bool ok = false;
    Bytes a = image_of(dir / "eboot.elf", &ok);
    CHECK(ok);
    CHECK(a.size() == expect.size());
    CHECK(a == expect);
    Bytes b = image_of(dir / "eboot.bin", &ok);
    CHECK(ok);
    CHECK(b == expect);

    write(dir / "junk.bin", Bytes(100, 7));
    image_of(dir / "junk.bin", &ok);
    CHECK(!ok);
    write(dir / "cut.elf", Bytes(elf.begin(), elf.end() - 4));  // the last data cut off
    image_of(dir / "cut.elf", &ok);
    CHECK(ok);  // the cut is in the non-load segment
    write(dir / "cut2.elf", Bytes(elf.begin(), elf.begin() + 64 + 56 * 3 + 0x20 + 4));
    image_of(dir / "cut2.elf", &ok);
    CHECK(!ok);
    image_of(dir / "missing.elf", &ok);
    CHECK(!ok);
    fs::remove_all(dir);
    std::printf("eboot-image-test: ok\n");
    return 0;
}
