// SPDX-License-Identifier: GPL-3.0-or-later
// Guest services (gpu/shim/party/party_guestsvc.h): the gate and the record change text.
// ninja -C out/gpu party-guestsvc-test && out/gpu/party-guestsvc-test
#include "party/party_guestsvc.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace coop;

static GuestServicesInput Guest(std::uint32_t map) {
    GuestServicesInput in;
    in.enabled = true;
    in.world_up = true;
    in.loading = false;
    in.session_role = kSessionRoleClient;
    in.map_id = map;
    return in;
}

int main() {
    // Dream maps.
    assert(IsDreamMap(0x15000000u)); // m21_00_00_00
    assert(IsDreamMap(0x15010000u)); // m21_01_00_00
    assert(!IsDreamMap(0x16000000u)); // m22
    assert(!IsDreamMap(0x18010000u)); // m24_01
    assert(!IsDreamMap(0xffffffffu));
    assert(!IsDreamMap(0));

    // The gate: a client in the Dream only.
    assert(GuestServicesGate(Guest(0x15000000u)));
    assert(GuestServicesGate(Guest(0x15010000u)));
    assert(!GuestServicesGate(Guest(0x18000000u))); // host's world outside the Dream: vanilla
    {
        GuestServicesInput in = Guest(0x15000000u);
        in.enabled = false;
        assert(!GuestServicesGate(in));
        in = Guest(0x15000000u);
        in.session_role = 3; // host
        assert(!GuestServicesGate(in));
        in.session_role = 0; // solo / own world
        assert(!GuestServicesGate(in));
        in.session_role = 7; // leaving
        assert(!GuestServicesGate(in));
        in = Guest(0x15000000u);
        in.loading = true;
        assert(!GuestServicesGate(in));
        in = Guest(0x15000000u);
        in.world_up = false;
        assert(!GuestServicesGate(in));
    }

    // Talk scripts.
    assert(GuestServiceTalk(210304));  // Doll
    assert(GuestServiceTalk(210692));  // workbench
    assert(GuestServiceTalk(210698));  // Insight messenger
    assert(!GuestServiceTalk(210305)); // Gehrman (last choice)
    assert(!GuestServiceTalk(210000)); // headstone
    assert(!GuestServiceTalk(210721)); // Dream warp menu
    assert(!GuestServiceTalk(210100)); // chalice altar
    assert(!GuestServiceTalk(0));
    assert(GuestServiceTalkAllowed(210304, ""));
    assert(!GuestServiceTalkAllowed(210305, ""));
    assert(GuestServiceTalkAllowed(210305, "all"));
    assert(GuestServiceTalkAllowed(210305, "123,210305"));
    assert(!GuestServiceTalkAllowed(210305, "123,2103050"));
    assert(!GuestServiceTalkAllowed(210305, "x,,210305x"));

    // Env.
    assert(GuestServicesEnabledFromEnv(nullptr));
    assert(GuestServicesEnabledFromEnv(""));
    assert(GuestServicesEnabledFromEnv("1"));
    assert(!GuestServicesEnabledFromEnv("0"));
    assert(GuestServicesEnabledFromEnv("00"));

    // Record changes.
    PlayerRecord a{10, 5000, 3}, b{11, 3200, 3};
    assert(DescribeRecordChange(a, b) == "level 10 -> 11, echoes 5000 -> 3200");
    assert(DescribeRecordChange(a, a).empty());
    PlayerRecord c{10, 5000, 1};
    assert(DescribeRecordChange(a, c) == "Insight 3 -> 1");
    PlayerRecord unknown;
    assert(DescribeRecordChange(unknown, a).empty());
    assert(a != b && a == a);

    std::puts("party-guestsvc-test: ok");
    return 0;
}
