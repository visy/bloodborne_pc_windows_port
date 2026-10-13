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
    // Elevator state flags: never (the lift's own sync moves it on the host).
    assert(!WorldActionAllowed(12410350, false, m2401) && !WorldActionAllowed(12410351, true, m2401));
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
                   c == pg::Category::key_event || c == pg::Category::npc_quest || c == pg::Category::cutscene_seen);
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
    // Gate catch-up table: boss flags are boss_defeated, gates are 2000-3999 never_sync flags.
    for (const GateRule& g : GateRules()) {
        assert(pg::CategoryOf(g.boss) == pg::Category::boss_defeated);
        assert(g.gate >= 2000 && g.gate < 4000);
        assert(!g.also || pg::CategoryOf(g.also) == pg::Category::boss_defeated);
    }
    // B2 boss gates: a guest may go through a boss fog before the host.
    for (const BossGate& g : BossGates()) {
        assert(pg::CategoryOf(g.boss_flag) == pg::Category::boss_defeated);
        assert(g.guest_flag == g.host_flag + 1);
        assert(g.host_flag / 10000 == g.boss_flag / 10000);  // same area block
        assert(BossGateByEvent(static_cast<std::int32_t>(g.guest_event)) == &g);
    }
    // Cleric Beast: only the host-entered condition of its guest-entry event is skipped.
    assert(BossGateSkipCondition(12414731, 3, 0, 12414700));
    assert(!BossGateSkipCondition(12414731, 3, 0, 12411700));  // boss alive check stays
    assert(!BossGateSkipCondition(12414731, 3, 0, 12411702));  // first fight (intro) done stays
    assert(!BossGateSkipCondition(12414731, 1003, 2, 12414700));
    assert(!BossGateSkipCondition(12414702, 3, 0, 12414700));  // the boss-start event itself
    assert(!BossGateSkipCondition(12414730, 3, 0, 12414700));  // the host's own entry
    assert(!BossGateSkipCondition(13204831, 3, 0, 13204800));  // Amygdala: left out
    const BossGate& cb = *BossGateByEvent(12414731);
    assert(BossGateHostOpens(cb, true, false, false, m2401));
    assert(!BossGateHostOpens(cb, false, false, false, m2401));  // no guest in
    assert(!BossGateHostOpens(cb, true, true, false, m2401));    // host already in
    assert(!BossGateHostOpens(cb, true, false, true, m2401));    // boss dead
    assert(!BossGateHostOpens(cb, true, false, false, m2400));   // other block
    assert(!BossGateHostOpens(cb, true, false, false, dream));
    assert(!BossGateHostOpens(cb, true, false, false, 0xffffffffu));
    assert(BossGateByGuestFlag(12414701) == &cb && !BossGateByGuestFlag(12414700));
    {
        Request fr;
        fr.map = m2401;
        fr.fog = {12414701};
        Request back;
        std::string why;
        assert(RequestFromJson(RequestToJson(fr), &back, &why) && back == fr);
        // Only guest-entered flags of the table; at most kMaxFog; no duplicates.
        assert(!RequestFromJson("{\"v\":1,\"map\":0,\"set\":[],\"clr\":[],\"fog\":[12414700]}", &back, &why));
        assert(!RequestFromJson("{\"v\":1,\"map\":0,\"set\":[],\"clr\":[],\"fog\":[12414701,12414701]}", &back, &why));
        assert(!RequestFromJson("{\"v\":1,\"map\":0,\"set\":[],\"clr\":[],\"fog\":[12414701,12414801,12204801]}", &back, &why));
        assert(!RequestFromJson("{\"v\":1,\"map\":0,\"set\":[],\"clr\":[],\"fog\":7}", &back, &why));
    }
    std::printf("party-worldact-test: ok\n");
    return 0;
}
