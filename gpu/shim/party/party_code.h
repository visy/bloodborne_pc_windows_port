// SPDX-License-Identifier: GPL-3.0-or-later
// Party codes: what a host reads out to friends. "BBP1-" + Crockford base32 (groups of 4,
// '-' separated) of 18 bytes:
//   ver u8 = 1 | flags u8 | ipv4[4] | port u16 BE | secret[8] | crc16-CCITT BE (of the 16 before)
// Decoding is case-insensitive, ignores '-' and blanks, reads I/L as 1 and O as 0; the CRC
// catches any other typo. A plain "host:port" (or "host" = default port) is accepted too, with
// no secret: the password alone then keys the party.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace party {

constexpr std::uint16_t kDefaultPartyPort = 9307;

enum : std::uint8_t {
    kCodeFlagLan = 1,               // a LAN address (make_lan_code)
    kCodeFlagPasswordRequired = 2,  // the host also set BB_PARTY_PASSWORD
    kCodeFlagRelayOnly = 4,         // guests must route game P2P through the host relay
};

struct PartyCode {
    std::uint8_t version = 1;
    std::uint8_t flags = 0;
    std::array<std::uint8_t, 4> ipv4{};  // a.b.c.d in order
    std::uint16_t port = kDefaultPartyPort;
    std::array<std::uint8_t, 8> secret{};
    bool plain = false;     // decoded from "host:port": no secret, flags unknown
    std::string host;       // dotted ipv4 (or the hostname given in plain form)

    std::string address() const;  // "a.b.c.d:port" (or host:port for a plain hostname)
};

// "BBP1-XXXX-XXXX-...". Normalised upper case.
std::string encode_party_code(const PartyCode& code);
// A BBP1 code or "host:port"; on failure `error` says what is wrong (bad character, wrong
// length, checksum mismatch = typo, unknown version).
bool decode_party_code(std::string_view text, PartyCode* out, std::string* error);

std::uint16_t crc16_ccitt(const std::uint8_t* d, std::size_t n);  // poly 0x1021, init 0xFFFF

// The best local IPv4 for a LAN code (network order bytes in out): an up, non-loopback adapter,
// preferring one with a default gateway and skipping virtual/tunnel adapters (Hyper-V, VPN,
// VirtualBox, VMware, WSL) when a physical one exists. False when none.
bool best_local_ipv4(std::array<std::uint8_t, 4>* out);

// Codes for the host: the Internet code carries the public address (STUN / UPnP / override,
// see party_addr.h), the LAN code the best local address with kCodeFlagLan.
std::string make_party_code(const std::array<std::uint8_t, 4>& ipv4, std::uint16_t port,
                            const std::array<std::uint8_t, 8>& secret, std::uint8_t flags);
bool make_lan_code(std::uint16_t port, const std::array<std::uint8_t, 8>& secret, std::uint8_t flags,
                   std::string* code);

bool parse_ipv4(std::string_view s, std::array<std::uint8_t, 4>* out);
std::string format_ipv4(const std::array<std::uint8_t, 4>& ip);

}  // namespace party
