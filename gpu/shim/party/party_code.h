// SPDX-License-Identifier: GPL-3.0-or-later
// Party codes: what a host reads out to friends. "BBP2-" + Crockford base32 (groups of 4,
// '-' separated) of
//   flags u8 | ipv4[4] (LAN code: only the last octet) | port u16 BE (absent with
//   kCodeFlagDefaultPort = 9307) | secret[6] | crc8 (poly 0x07, init 0, of the bytes before)
// e.g. Internet "BBP2-XXXX-XXXX-XXXX-XXXX-XXXX" (20 symbols), LAN "BBP2-XXXX-XXXX-XXXX-XXX" (15).
// The 6-byte secret sits in the 8-byte slot zero padded (secret[6] = secret[7] = 0), so the party
// key (party_crypto.h) is the same as for the BBP1 code of that secret. A LAN code's guest takes
// the first three octets from its own best local IPv4 (fill_lan_prefix), or asks the LAN
// (party_lan.h) when it has none. The older "BBP1-" form (29 symbols of ver u8 = 1 | flags u8 |
// ipv4[4] | port u16 BE | secret[8] | crc16-CCITT BE) is still decoded.
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
    kCodeFlagDefaultPort = 8,       // BBP2 wire only: no port bytes, port 9307 (never in PartyCode::flags)
};

struct PartyCode {
    std::uint8_t version = 2;
    std::uint8_t flags = 0;
    std::array<std::uint8_t, 4> ipv4{};  // a.b.c.d in order
    std::uint16_t port = kDefaultPartyPort;
    std::array<std::uint8_t, 8> secret{};
    bool plain = false;     // decoded from "host:port": no secret, flags unknown
    bool lan_partial = false;  // a BBP2 LAN code: only ipv4[3] came with it (fill_lan_prefix)
    std::string host;       // dotted ipv4 (or the hostname given in plain form)

    std::string address() const;  // "a.b.c.d:port" (or host:port for a plain hostname)
};

// "BBP2-XXXX-XXXX-...". Normalised upper case. A secret that does not fit BBP2 (secret[6] or
// secret[7] set: an old host's) gives the BBP1 form.
std::string encode_party_code(const PartyCode& code);
// A BBP2 / BBP1 code or "host:port"; on failure `error` says what is wrong (bad character, wrong
// length, checksum mismatch = typo, unknown version).
bool decode_party_code(std::string_view text, PartyCode* out, std::string* error);

std::uint16_t crc16_ccitt(const std::uint8_t* d, std::size_t n);  // poly 0x1021, init 0xFFFF
std::uint8_t crc8(const std::uint8_t* d, std::size_t n);           // poly 0x07, init 0 ("123456789" = 0xF4)

// A LAN code's (lan_partial) first three octets from the guest's own address: ipv4 and host set.
void fill_lan_prefix(PartyCode* code, const std::array<std::uint8_t, 4>& local);

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

// A guest joining with this code uses no password: a party code whose host set none
// (no kCodeFlagPasswordRequired). A plain host:port carries no flags: the password is kept.
inline bool code_ignores_password(const PartyCode& code) {
    return !code.plain && !(code.flags & kCodeFlagPasswordRequired);
}

bool parse_ipv4(std::string_view s, std::array<std::uint8_t, 4>* out);
std::string format_ipv4(const std::array<std::uint8_t, 4>& ip);

}  // namespace party
