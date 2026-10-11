// SPDX-License-Identifier: GPL-3.0-or-later
// Guest world actions (gpu/shim/party/party_worldact.h): the whitelist, the request JSON and its
// validation, the host's rate limit, the NPC talk allowance.
// cmake --build out/gpu --target party-worldact-test && out/gpu/party-worldact-test
#include "party/party_progress.h"
#include "party/party_worldact.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace coop::worldact;
namespace pg = coop::progress;

int main() {
    const std::uint32_t m2401 = 0x18010000u, m2400 = 0x18000000u, m2200 = 0x16000000u, dream = 0x15000000u;
    // Doors / levers of the host's area: set and clear (elevators toggle).
    assert(pg::CategoryOf(12410200) == pg::Category::shortcut_door);
    assert(WorldActionAllowed(12410200, false, m2401));
    assert(WorldActionAllowed(12410200, true, m2401));
    assert(WorldActionAllowed(12410200, false, m2400));  // same area (Yharnam blocks 24_00 / 24_01)
    assert(!WorldActionAllowed(12410200, false, m2200)); // another area: not the host's world
    assert(!WorldActionAllowed(12410200, false, 0xffffffffu));
    // Not a guest's to change: boss kills, item pickups (instanced), session runtime, endings,
    // the chalice slot, ids outside the mirror.
    assert(!WorldActionAllowed(12411800, false, m2401)); // Gascoigne defeated
    assert(!WorldActionAllowed(5000, false, m2401));     // item lot pickup
    assert(!WorldActionAllowed(2000, false, m2401));     // session
    assert(!WorldActionAllowed(9020, false, m2401));     // chalice slot
    assert(!WorldActionAllowed(21, false, m2401));       // ending
    assert(!WorldActionAllowed(6600, false, m2401));     // own pool (personal)
    assert(!WorldActionAllowed(0, false, m2401));
    assert(!WorldActionAllowed(0xffffffffu, false, m2401));
    // NPC quests: shared (the user's decision); set and clear (a quest step moves a state flag).
    assert(pg::CategoryOf(12100110) == pg::Category::npc_quest);
    assert(WorldActionAllowed(12100110, false, dream));
    assert(WorldActionAllowed(12100110, true, dream));
    assert(pg::CategoryOf(1141) == pg::Category::npc_quest);
    assert(WorldActionAllowed(1141, true, m2401) && WorldActionAllowed(1143, false, m2200));
    assert(!WorldActionAllowed(12100110, false, m2401));
    // Every id the whitelist allows is a written mirror bit of a whitelisted category.
    for (std::uint32_t id = 0; id < 80000000u; id += (id < 20000 ? 1 : 7)) {
        if (WorldActionAllowed(id, false, (id / 100000 % 100) << 24) ||
            WorldActionAllowed(id, true, (id / 100000 % 100) << 24)) {
            const auto c = pg::CategoryOf(id);
            assert(c == pg::Category::shortcut_door || c == pg::Category::world_state ||
                   c == pg::Category::key_event || c == pg::Category::npc_quest);
            assert(!pg::NeverWritable(id));
            assert(pg::WorldMirrorKey(pg::PoolKeyOfId(id)));
        }
    }

    // JSON round trip and validation.
    Request r;
    r.map = m2401;
    r.set = {12410200, 12410202};
    r.clr = {12410203};
    Request back;
    std::string err;
    assert(RequestFromJson(RequestToJson(r), &back, &err) && back == r);
    Request bad = r;
    bad.set.push_back(12411800);  // a boss kill
    assert(!RequestFromJson(RequestToJson(bad), &back, &err));
    bad = r;
    bad.set.push_back(12410200);  // duplicate
    assert(!RequestFromJson(RequestToJson(bad), &back, &err));
    bad = r;
    bad.clr.push_back(12410200);  // set and cleared
    assert(!RequestFromJson(RequestToJson(bad), &back, &err));
    bad = r;
    bad.clr = {12411800};  // a boss kill is never cleared
    assert(!RequestFromJson(RequestToJson(bad), &back, &err));
    Request many;
    many.map = m2401;
    for (std::uint32_t i = 0; i <= kMaxSet; ++i) {
        many.set.push_back(12410200 + i);
    }
    assert(!RequestFromJson(RequestToJson(many), &back, &err));
    assert(!RequestFromJson(RequestToJson(Request{}), &back, &err));  // empty
    Request kick;  // an ObjAct kick alone (the lever's flag follows when its animation ends)
    kick.map = m2401;
    kick.kick = {2410000};
    assert(RequestFromJson(RequestToJson(kick), &back, &err) && back == kick);
    assert(!RequestFromJson("{\"v\":1,\"map\":1,\"set\":[],\"clr\":[],\"kick\":[0]}", &back, &err));
    assert(!RequestFromJson("{\"v\":1,\"map\":1,\"set\":[],\"clr\":[],\"kick\":[5,5]}", &back, &err));
    assert(!RequestFromJson("{\"v\":1,\"map\":1,\"set\":[],\"clr\":[],\"kick\":[1,2,3,4,5]}", &back, &err));
    assert(!RequestFromJson("{\"v\":1,\"map\":1,\"set\":[],\"clr\":[],\"kick\":7}", &back, &err));
    assert(!RequestFromJson("{\"v\":1,\"map\":1,\"set\":[1,2", &back, &err));
    assert(!RequestFromJson("{\"v\":2,\"map\":1,\"set\":[12410200],\"clr\":[]}", &back, &err));
    assert(!RequestFromJson("{\"v\":1,\"map\":1,\"set\":[-5],\"clr\":[]}", &back, &err));
    assert(!RequestFromJson("{\"v\":1,\"map\":1,\"set\":[4294967296],\"clr\":[]}", &back, &err));
    assert(!RequestFromJson("[]", &back, &err));
    assert(!RequestFromJson(std::string(5000, ' '), &back, &err));

    // Rate limit: per slot, per window.
    RateLimit rl;
    assert(rl.Take(1, kRateFlags, 100.0));
    assert(!rl.Take(1, 1, 101.0));
    assert(rl.Take(2, 5, 101.0));
    assert(rl.Take(1, 1, 100.0 + kRateWindow));
    assert(!rl.Take(8, 1, 0.0) && !rl.Take(-1, 1, 0.0) && !rl.Take(1, -1, 0.0) && !rl.Take(1, kRateFlags + 1, 500.0));

    // NPC talk: world NPCs yes; Dream scripts, lamps, nonsense no.
    assert(WorldTalkAllowed(241100));   // an m24_01 NPC script
    assert(WorldTalkAllowed(240200));
    assert(!WorldTalkAllowed(241001));  // a world lamp (t241001)
    assert(!WorldTalkAllowed(210305));  // Gehrman's ending choice
    assert(!WorldTalkAllowed(210304));  // the Doll (guest services' list)
    assert(!WorldTalkAllowed(0) && !WorldTalkAllowed(99999) && !WorldTalkAllowed(1000000));
    std::printf("party-worldact-test: ok\n");
    return 0;
}
