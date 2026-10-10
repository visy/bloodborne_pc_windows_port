// SPDX-License-Identifier: GPL-3.0-or-later
#include "party_code.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#endif

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace party {

namespace {

constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr std::size_t kRawSize = 18;
constexpr std::size_t kSymbols = (kRawSize * 8 + 4) / 5;  // 29
constexpr char kPrefix[] = "BBP1";

int symbol_value(char c) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (c == 'I' || c == 'L') c = '1';
    if (c == 'O') c = '0';
    for (int i = 0; i < 32; ++i)
        if (kAlphabet[i] == c) return i;
    return -1;
}

bool parse_port(std::string_view s, std::uint16_t* port) {
    if (s.empty() || s.size() > 5) return false;
    unsigned v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<unsigned>(c - '0');
    }
    if (v == 0 || v > 65535) return false;
    *port = static_cast<std::uint16_t>(v);
    return true;
}

std::string trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

}  // namespace

std::uint16_t crc16_ccitt(const std::uint8_t* d, std::size_t n) {
    std::uint16_t crc = 0xFFFF;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= static_cast<std::uint16_t>(d[i] << 8);
        for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021) : static_cast<std::uint16_t>(crc << 1);
    }
    return crc;
}

bool parse_ipv4(std::string_view s, std::array<std::uint8_t, 4>* out) {
    std::array<std::uint8_t, 4> ip{};
    int part = 0;
    unsigned v = 0;
    int digits = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '.') {
            if (digits == 0 || v > 255 || part > 3) return false;
            ip[part++] = static_cast<std::uint8_t>(v);
            v = 0;
            digits = 0;
        } else if (s[i] >= '0' && s[i] <= '9') {
            v = v * 10 + static_cast<unsigned>(s[i] - '0');
            if (++digits > 3) return false;
        } else {
            return false;
        }
    }
    if (part != 4) return false;
    *out = ip;
    return true;
}

std::string format_ipv4(const std::array<std::uint8_t, 4>& ip) {
    char buf[20];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return buf;
}

std::string PartyCode::address() const {
    std::string h = host.empty() ? format_ipv4(ipv4) : host;
    return h + ":" + std::to_string(port);
}

std::string encode_party_code(const PartyCode& code) {
    std::uint8_t raw[kRawSize];
    raw[0] = code.version;
    raw[1] = code.flags;
    std::memcpy(raw + 2, code.ipv4.data(), 4);
    raw[6] = static_cast<std::uint8_t>(code.port >> 8);
    raw[7] = static_cast<std::uint8_t>(code.port);
    std::memcpy(raw + 8, code.secret.data(), 8);
    std::uint16_t crc = crc16_ccitt(raw, 16);
    raw[16] = static_cast<std::uint8_t>(crc >> 8);
    raw[17] = static_cast<std::uint8_t>(crc);

    std::string sym;
    std::uint32_t acc = 0;
    int bits = 0;
    for (std::uint8_t b : raw) {
        acc = (acc << 8) | b;
        bits += 8;
        while (bits >= 5) {
            sym += kAlphabet[(acc >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0) sym += kAlphabet[(acc << (5 - bits)) & 31];

    std::string out = kPrefix;
    for (std::size_t i = 0; i < sym.size(); ++i) {
        if (i % 4 == 0) out += '-';
        out += sym[i];
    }
    return out;
}

bool decode_party_code(std::string_view text_in, PartyCode* out, std::string* error) {
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        return false;
    };
    std::string text = trim(text_in);
    if (text.empty()) return fail("empty party code");

    std::string upper;
    for (char c : text) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    // "BBP1" / "BBPI" / "BBPL" (1 typed as I or L) prefix.
    bool coded = upper.size() >= 4 && upper.compare(0, 3, "BBP") == 0 && symbol_value(upper[3]) == 1 &&
                 (upper.size() == 4 || upper[4] == '-' || upper[4] == ' ');
    if (!coded) {
        // host:port, or a bare host (default port).
        PartyCode pc;
        pc.plain = true;
        std::string host = text;
        std::size_t colon = text.rfind(':');
        if (colon != std::string::npos) {
            host = text.substr(0, colon);
            if (!parse_port(std::string_view(text).substr(colon + 1), &pc.port))
                return fail("bad port in host:port (expected 1-65535)");
        }
        if (host.empty()) return fail("missing host in host:port");
        for (char c : host)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_'))
                return fail("not a party code (BBP1-...) or host:port");
        pc.host = host;
        if (parse_ipv4(host, &pc.ipv4)) pc.host = format_ipv4(pc.ipv4);
        *out = pc;
        return true;
    }

    std::vector<int> vals;
    for (std::size_t i = 4; i < upper.size(); ++i) {
        char c = upper[i];
        if (c == '-' || c == ' ' || c == '\t') continue;
        int v = symbol_value(c);
        if (v < 0) {
            if (error) *error = std::string("invalid character '") + c + "' in party code";
            return false;
        }
        vals.push_back(v);
    }
    if (vals.size() != kSymbols) {
        if (error)
            *error = "party code has " + std::to_string(vals.size()) + " characters after BBP1, expected " +
                     std::to_string(kSymbols);
        return false;
    }
    std::uint8_t raw[kRawSize];
    std::size_t n = 0;
    std::uint32_t acc = 0;
    int bits = 0;
    for (int v : vals) {
        acc = (acc << 5) | static_cast<std::uint32_t>(v);
        bits += 5;
        if (bits >= 8) {
            if (n < kRawSize) raw[n++] = static_cast<std::uint8_t>(acc >> (bits - 8));
            bits -= 8;
        }
    }
    if (n != kRawSize || (acc & ((1u << bits) - 1)) != 0) return fail("party code checksum mismatch (typo?)");
    std::uint16_t crc = static_cast<std::uint16_t>((raw[16] << 8) | raw[17]);
    if (crc != crc16_ccitt(raw, 16)) return fail("party code checksum mismatch (typo?)");
    if (raw[0] != 1) return fail("party code from a newer version of the port");

    PartyCode pc;
    pc.version = raw[0];
    pc.flags = raw[1];
    std::memcpy(pc.ipv4.data(), raw + 2, 4);
    pc.port = static_cast<std::uint16_t>((raw[6] << 8) | raw[7]);
    std::memcpy(pc.secret.data(), raw + 8, 8);
    pc.host = format_ipv4(pc.ipv4);
    if (pc.port == 0) return fail("party code has port 0");
    *out = pc;
    return true;
}

std::string make_party_code(const std::array<std::uint8_t, 4>& ipv4, std::uint16_t port,
                            const std::array<std::uint8_t, 8>& secret, std::uint8_t flags) {
    PartyCode pc;
    pc.flags = flags;
    pc.ipv4 = ipv4;
    pc.port = port;
    pc.secret = secret;
    return encode_party_code(pc);
}

bool make_lan_code(std::uint16_t port, const std::array<std::uint8_t, 8>& secret, std::uint8_t flags,
                   std::string* code) {
    std::array<std::uint8_t, 4> ip{};
    if (!best_local_ipv4(&ip)) return false;
    *code = make_party_code(ip, port, secret, static_cast<std::uint8_t>(flags | kCodeFlagLan));
    return true;
}

#if defined(_WIN32)

static bool contains_ci(const wchar_t* hay, const wchar_t* needle) {
    if (!hay) return false;
    std::wstring h(hay), n(needle);
    for (auto& c : h) c = static_cast<wchar_t>(towlower(c));
    for (auto& c : n) c = static_cast<wchar_t>(towlower(c));
    return h.find(n) != std::wstring::npos;
}

bool best_local_ipv4(std::array<std::uint8_t, 4>* out) {
    ULONG size = 16 * 1024;
    std::vector<std::uint8_t> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 4 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET,
                                  GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                      GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return false;
    int best_score = -1;
    static const wchar_t* kVirtual[] = {L"virtual", L"hyper-v", L"vmware", L"virtualbox", L"vethernet",
                                        L"wsl", L"tap-", L"wireguard", L"tunnel", L"loopback", L"vpn"};
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
        bool is_virtual = false;
        for (const wchar_t* v : kVirtual)
            if (contains_ci(a->Description, v) || contains_ci(a->FriendlyName, v)) is_virtual = true;
        int score = 0;
        if (a->FirstGatewayAddress) score += 4;
        if (!is_virtual) score += 2;
        if (a->IfType == IF_TYPE_ETHERNET_CSMACD || a->IfType == IF_TYPE_IEEE80211) score += 1;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            const auto* b = reinterpret_cast<const std::uint8_t*>(&sin->sin_addr);
            if (b[0] == 127 || (b[0] == 169 && b[1] == 254) || b[0] == 0) continue;
            if (score > best_score) {
                best_score = score;
                std::memcpy(out->data(), b, 4);
            }
        }
    }
    return best_score >= 0;
}

#else

bool best_local_ipv4(std::array<std::uint8_t, 4>*) { return false; }  // stub: not needed off Windows yet

#endif

}  // namespace party
