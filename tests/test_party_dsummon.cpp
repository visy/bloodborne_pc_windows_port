// SPDX-License-Identifier: GPL-3.0-or-later
// Direct summon delivery (gpu/shim/party/party_dsummon): wire format, type-1 payload, decisions.
// Build and run: ninja -C out/gpu party-dsummon-test && out/gpu/party-dsummon-test.exe
#include "party/party_dsummon.h"

#include <cstdio>
#include <cstring>

using namespace coop::dsummon;

static int g_failures = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main() {
    CHECK(SwitchOn(nullptr, true) && !SwitchOn(nullptr, false) && !SwitchOn("0", true) && !SwitchOn("off", true) && SwitchOn("1", false));
    CHECK(KindOf(EncodeReq()) == Kind::Req && KindOf(EncodeDone(true)) == Kind::Done && KindOf("{\"op\":\"title_req\"}") == Kind::None);
    Room r;
    for (int i = 0; i < 209; ++i) r.data.push_back(std::uint8_t(i * 7));
    r.area = 0x18010000; r.pos[0] = -12.5f; r.pos[1] = 3.25f; r.pos[2] = 100.0f; r.level = 42;
    Room d; bool refused = true; std::string err;
    CHECK(DecodeRoom(EncodeRoom(r), &d, &refused, &err) && !refused);
    CHECK(d.data == r.data && d.area == r.area && d.level == 42 && d.pos[0] == -12.5f && d.pos[2] == 100.0f);
    CHECK(!DecodeRoom(EncodeRefuse(), &d, &refused, &err) && refused);
    CHECK(!DecodeRoom("{\"op\":\"dsum_room\",\"room\":\"zz\",\"area\":1,\"pos\":[0,0,0],\"level\":1}", &d, &refused, &err) && !refused);
    CHECK(!DecodeRoom("{\"op\":\"dsum_room\",\"room\":\"00112233445566778899\",\"area\":1,\"pos\":[1e9,0,0],\"level\":1}", &d, &refused, &err));
    CHECK(!DecodeRoom("{\"op\":\"dsum_room\",\"room\":\"0011\",\"area\":1,\"pos\":[0,0,0],\"level\":1}", &d, &refused, &err));
    bool ok = false;
    CHECK(DecodeDone(EncodeDone(true), &ok) && ok && DecodeDone(EncodeDone(false), &ok) && !ok);
    const std::uint8_t id[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const auto p = BuildPayload(id, r);
    CHECK(p.size() == kHeadSize + 209);
    std::uint32_t u = 0; std::uint64_t q = 0; float f = 0;
    CHECK(std::memcmp(p.data(), id, 8) == 0);
    std::memcpy(&u, &p[0x0c], 4); CHECK(u == 0x18010000);
    std::memcpy(&f, &p[0x10], 4); CHECK(f == -12.5f);
    std::memcpy(&u, &p[0x1c], 4); CHECK(u == 42);
    std::memcpy(&q, &p[0x28], 8); CHECK(q == 0x8000000000000000ull);
    std::memcpy(&u, &p[0x30], 4); CHECK(u == 209);
    CHECK(std::memcmp(&p[kHeadSize], r.data.data(), 209) == 0);
    HostView v; v.world_up = true;
    CHECK(HostRefusal(v) == nullptr);
    v.loading = true; CHECK(HostRefusal(v)); v.loading = false;
    v.cooperators = 3; CHECK(HostRefusal(v)); v.cooperators = 1;
    v.ms_since_sign_summon = 5000; CHECK(HostRefusal(v)); v.ms_since_sign_summon = 30000;
    v.other_hold_ms = 100; CHECK(HostRefusal(v)); v.other_hold_ms = 0; CHECK(!HostRefusal(v));
    // A11: a boss fight / cutscene holds every join (direct summon included), with the reason
    CHECK(HostBusy(false, false) == Busy::None && HostBusy(true, false) == Busy::Boss &&
          HostBusy(false, true) == Busy::Cutscene && HostBusy(true, true) == Busy::Boss);
    v.busy = Busy::Boss; CHECK(HostRefusal(v) && std::strstr(HostRefusal(v), "boss"));
    v.busy = Busy::Cutscene; CHECK(HostRefusal(v) && std::strstr(HostRefusal(v), "cutscene"));
    v.busy = Busy::None; CHECK(!HostRefusal(v));
    CHECK(!DecodeRoom(EncodeRefuse(Busy::Boss), &d, &refused, &err) && refused);
    CHECK(WaitOf(EncodeRefuse(Busy::Boss)) == Busy::Boss && WaitOf(EncodeRefuse()) == Busy::None);
    CHECK(KindOf(EncodeBusy(Busy::Cutscene)) == Kind::Busy && WaitOf(EncodeBusy(Busy::Cutscene)) == Busy::Cutscene);
    CHECK(KindOf(EncodeBusy(Busy::None)) == Kind::Busy && WaitOf(EncodeBusy(Busy::None)) == Busy::None);
    CHECK(BusyText(Busy::Boss, false) == "Waiting: the host is in a boss fight" && BusyText(Busy::None, true).empty());
    // guest: no answer -> failed quietly
    Guest g; OnAsked(g, 0);
    CHECK(Step(g, 1, false, false) == Action::None && g.phase == Phase::Asked);
    CHECK(Step(g, kAnswerWait + 1, false, false) == Action::None && g.phase == Phase::Failed);
    // refused
    g = Guest{}; OnAsked(g, 0); OnRoom(g, 0.1, true); CHECK(g.phase == Phase::Failed);
    // late room ignored
    OnRoom(g, 0.2, false); CHECK(g.phase == Phase::Failed);
    // room -> wait for the entry -> deliver -> joined
    g = Guest{}; OnAsked(g, 0); OnRoom(g, 0.1, false);
    CHECK(Step(g, 1, false, false) == Action::None);
    CHECK(Step(g, 2, true, false) == Action::Deliver);
    OnDelivered(g, 2, true); CHECK(g.phase == Phase::Delivered);
    CHECK(Step(g, 3, false, false) == Action::None);
    CHECK(Step(g, 4, false, true) == Action::ReportOk && g.phase == Phase::Done);
    // delivered but no join
    g = Guest{}; OnAsked(g, 0); OnRoom(g, 0, false); OnDelivered(g, 1, true);
    CHECK(Step(g, 1 + kJoinWait + 0.1, false, false) == Action::ReportFail && g.phase == Phase::Failed);
    // the entry never comes
    g = Guest{}; OnAsked(g, 0); OnRoom(g, 0, false);
    CHECK(Step(g, kEntryWait + 0.1, false, false) == Action::ReportFail);
    // three refused deliveries
    g = Guest{}; OnAsked(g, 0); OnRoom(g, 0, false);
    for (int i = 0; i < 3; ++i) { CHECK(Step(g, 1, true, false) == Action::Deliver); OnDelivered(g, 1, false); }
    CHECK(Step(g, 1, true, false) == Action::ReportFail);
    // the sign path joined first
    g = Guest{}; OnAsked(g, 0); OnRoom(g, 0, false);
    CHECK(Step(g, 1, true, true) == Action::ReportOk);
    // A5: gather before ringing only when the direct summon cannot carry the guest across maps
    CHECK(GatherFirst(false, Phase::Idle));
    CHECK(!GatherFirst(true, Phase::Idle));
    CHECK(!GatherFirst(true, Phase::Done));
    CHECK(GatherFirst(true, Phase::Failed));
    std::printf("party-dsummon-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
