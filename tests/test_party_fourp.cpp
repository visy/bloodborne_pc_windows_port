// SPDX-License-Identifier: GPL-3.0-or-later
// Four-player rules (gpu/shim/party/party_fourp), the parts without the game: the rule tag from
// the environment, max players in force (the host's wins on a guest), the H1 slot decision, the
// H2 boss count, the H4 SpEffect choice and the E6 NPC sign condition rewrite.
// Build and run: ninja -C out/gpu party-fourp-test && out/gpu/party-fourp-test.exe
#include "party/party_fourp.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace coop::fourp;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                              \
    } while (0)

static void set_env(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1);
    else unsetenv(k);
#endif
}

static void test_config_and_tag() {
    set_env("BB_PARTY", nullptr);
    set_env("BB_PARTY_FOURP", nullptr);
    set_env("BB_PARTY_FOURP_SCALING", nullptr);
    set_env("BB_PARTY_FOURP_NPC_SIGNS", nullptr);
    set_env("BB_PARTY_MAX", nullptr);
    Config c = ConfigFromEnv();
    CHECK(!c.on);
    CHECK(c.local_max == 3);
    CHECK(FourpRulesTag() == "4p:off");

    set_env("BB_PARTY", "host");
    set_env("BB_PARTY_MAX", "4");
    c = ConfigFromEnv();
    CHECK(c.on && c.scaling && c.npc_signs && c.recruit);
    CHECK(c.local_max == 4);
    CHECK(FourpRulesTag() == "4p:v1:H1,H2,H3,H4,E6");

    // Max players does not change the tag: the host's value is sent in WELCOME.
    set_env("BB_PARTY_MAX", "3");
    CHECK(FourpRulesTag() == "4p:v1:H1,H2,H3,H4,E6");
    set_env("BB_PARTY_MAX", "9");
    CHECK(ConfigFromEnv().local_max == 4);
    set_env("BB_PARTY_MAX", "1");
    CHECK(ConfigFromEnv().local_max == 2);

    set_env("BB_PARTY_FOURP_SCALING", "0");
    CHECK(FourpRulesTag() == "4p:v1:H1,E6");
    set_env("BB_PARTY_FOURP_NPC_SIGNS", "0");
    CHECK(FourpRulesTag() == "4p:v1:H1");
    set_env("BB_PARTY_FOURP", "0");
    CHECK(FourpRulesTag() == "4p:off");
    set_env("BB_PARTY_FOURP", "1");
    CHECK(ConfigFromEnv().on);
    set_env("BB_PARTY_FOURP", nullptr);
    set_env("BB_PARTY_FOURP_SCALING", nullptr);
    set_env("BB_PARTY_FOURP_NPC_SIGNS", nullptr);
    set_env("BB_PARTY_MAX", nullptr);
    set_env("BB_PARTY", nullptr);
}

static void test_effective_max() {
    CHECK(EffectiveMaxPlayers(4, false, false, 0) == 4);  // host
    CHECK(EffectiveMaxPlayers(3, false, true, 2) == 3);   // host ignores host_max (its own)
    CHECK(EffectiveMaxPlayers(3, true, false, 4) == 3);   // guest not connected yet
    CHECK(EffectiveMaxPlayers(3, true, true, 4) == 4);    // guest takes the host's
    CHECK(EffectiveMaxPlayers(4, true, true, 2) == 2);
    CHECK(EffectiveMaxPlayers(4, true, true, 0) == 4);    // no WELCOME value
    CHECK(EffectiveMaxPlayers(4, true, true, 7) == 4);    // clamped
}

static void test_h1() {
    // Max 4: the 2nd slot (status 2) is free while < 3 cooperators.
    CHECK(LiftCoopSlot(2, 2, 4));
    CHECK(LiftCoopSlot(2, 0, 4));
    CHECK(!LiftCoopSlot(2, 3, 4));  // full: the 4th is refused
    CHECK(!LiftCoopSlot(2, 4, 4));
    CHECK(!LiftCoopSlot(3, 2, 4));  // unavailable (not host, blocked area): left alone
    CHECK(!LiftCoopSlot(0, 1, 4));  // already free
    CHECK(!LiftCoopSlot(2, -1, 4));
    // Max 2/3: vanilla.
    CHECK(!LiftCoopSlot(2, 2, 3));
    CHECK(!LiftCoopSlot(2, 1, 2));
}

static void test_scaling() {
    CHECK(BossCount(0) == 0);
    CHECK(BossCount(1) == 1);
    CHECK(BossCount(2) == 2);
    CHECK(BossCount(3) == 2);
    CHECK(DopingSpEffect(7501, 3) == 7502);
    CHECK(DopingSpEffect(7501, 2) == 7501);
    CHECK(DopingSpEffect(7501, -1) == 7501);
    CHECK(DopingSpEffect(7500, 3) == 7500);
    CHECK(DopingSpEffect(9006, 3) == 9006);
}

static void test_npc_signs() {
    CHECK(IsNpcSignEvent(12304400));
    CHECK(IsNpcSignEvent(13404406));
    CHECK(IsNpcSignEvent(12906962));
    CHECK(!IsNpcSignEvent(12304407));
    CHECK(!IsNpcSignEvent(12304399));
    CHECK(!IsNpcSignEvent(12304710));  // maiden (invader) events are not touched
    CHECK(!IsNpcSignEvent(4400));
    const std::uint8_t lt2[4] = {1, 0, 3, 2}, lt2b[4] = {2, 0, 3, 2}, lt3[4] = {1, 0, 3, 3};
    const std::uint8_t inv[4] = {1, 1, 3, 2}, ge1[4] = {0xff, 0, 4, 1}, eq0[4] = {1, 0, 0, 0};
    CHECK(NpcSignCount(lt2, 4) == 3);
    CHECK(NpcSignCount(lt2b, 4) == 3);
    CHECK(NpcSignCount(lt2, 3) == -1);
    CHECK(NpcSignCount(lt3, 4) == -1);
    CHECK(NpcSignCount(lt3, 3) == 2);  // revert (only bytes we wrote, checked by the caller)
    CHECK(NpcSignCount(inv, 4) == -1);
    CHECK(NpcSignCount(ge1, 4) == -1);
    CHECK(NpcSignCount(eq0, 4) == -1);
}

int main() {
    test_config_and_tag();
    test_effective_max();
    test_h1();
    test_scaling();
    test_npc_signs();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
