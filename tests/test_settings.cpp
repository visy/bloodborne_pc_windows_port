// SPDX-License-Identifier: GPL-2.0-or-later
// bbport_settings.cpp: the menu saves its keys into bbport.ini and keeps the launcher's (controls).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "gpu/shim/bbport_settings.h"
#include "gpu/shim/party/party_traversal.h"
#include "test_portable.h"

static std::string Read(const char* path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

int main() {
    char path[1024];
    const int created = bb_test_temp_file(path, sizeof(path), "bbport-settings-test-");
    assert(created == 0);
    const char ini[] = "# launcher\nupscaler=fsr3\nkey.cross=X, Space\npad.circle=a\nshow_fps=0\n"
                       "fullscreen_hint=1\nparty_enemy_poise=-50\nparty_boss_poise=-100\nmodel_lod=2\n";
    const int written = bb_test_write_file(path, ini, sizeof(ini) - 1);
    assert(written == 0);
    const int env = bb_test_setenv("BB_CONFIG", path);
    assert(env == 0);

    assert(bb_test_setenv("BB_PARTY_RENDEZVOUS", "") == 0);
    BbSettings::Load();
    auto& s = BbSettings::Get();
    assert(!s.party_rendezvous);  // the DHT rendezvous is opt-in: off without the ini key / env
    assert(s.upscaler == BbSettings::UpscalerFsr3 && !s.show_fps && s.menu_x < 0.0f);
    // Poise starts at 0 (2026-10-21: below 0 enemies staggered less): old negative values read as 0.
    assert(s.party_enemy_poise == 0 && s.party_boss_poise == 0);
    // Model detail "Lowest" (2) drops map pieces without a third level of detail: read as Lower.
    assert(s.model_lod == 1);
    s.show_fps = true;
    s.upscaler = BbSettings::UpscalerFsr411;
    s.menu_x = 0.625f;
    s.menu_y = 0.125f;
    BbSettings::Save();

    const std::string saved = Read(path);
    // The launcher's controls, its other keys and comments stay; the menu's keys are replaced
    // in place, new ones appended.
    assert(saved.find("# launcher\nupscaler=fsr411\nkey.cross=X, Space\npad.circle=a\nshow_fps=1\n"
                      "fullscreen_hint=1\n") == 0);
    assert(saved.find("menu_pos=0.6250,0.1250\n") != std::string::npos);
    assert(saved.find("upscaler=fsr3") == std::string::npos);

    s.menu_x = -1.0f;
    s.menu_y = -1.0f;
    BbSettings::Load();
    assert(s.menu_x == 0.625f && s.menu_y == 0.125f && s.upscaler == BbSettings::UpscalerFsr411);

    // BB_PARTY_RENDEZVOUS (the launcher's Party tab) wins over bbport.ini party_rendezvous.
    using party::traversal::rendezvous_switch;
    assert(!rendezvous_switch(nullptr, false) && rendezvous_switch(nullptr, true));
    assert(!rendezvous_switch("", false) && rendezvous_switch("1", false) && rendezvous_switch("ON", false));
    assert(rendezvous_switch("yes", false) && rendezvous_switch("true", false));
    assert(!rendezvous_switch("0", true) && !rendezvous_switch("off", true) && !rendezvous_switch("No", true));
    assert(rendezvous_switch("maybe", true) && !rendezvous_switch("maybe", false));
    assert(Read(path).find("party_rendezvous=0\n") != std::string::npos);
    assert(bb_test_setenv("BB_PARTY_RENDEZVOUS", "1") == 0);
    BbSettings::Load();
    assert(s.party_rendezvous);
    s.party_rendezvous = true;
    BbSettings::Save();  // System > Party members: saved to the ini
    assert(bb_test_setenv("BB_PARTY_RENDEZVOUS", "") == 0);
    BbSettings::Load();
    assert(s.party_rendezvous);  // from the ini now
    assert(bb_test_setenv("BB_PARTY_RENDEZVOUS", "0") == 0);
    BbSettings::Load();
    assert(!s.party_rendezvous);  // the launcher's off wins

    // F2 (0.7.1): the member relay and IPv6, on by default; the launcher's env wins over the ini.
    assert(bb_test_setenv("BB_PARTY_MEMBER_RELAY", "") == 0 && bb_test_setenv("BB_PARTY_IPV6", "") == 0);
    BbSettings::Load();
    assert(s.party_member_relay && !s.party_ipv6);  // relay on, IPv6 off by default (0.7.1 decision)
    assert(Read(path).find("party_member_relay=1\n") != std::string::npos);
    assert(Read(path).find("party_ipv6=0\n") != std::string::npos);
    s.party_ipv6 = true;
    BbSettings::Save();  // System > Party network
    BbSettings::Load();
    assert(s.party_ipv6 && Read(path).find("party_ipv6=1\n") != std::string::npos);
    s.party_ipv6 = false;
    BbSettings::Save();
    assert(bb_test_setenv("BB_PARTY_IPV6", "1") == 0 && bb_test_setenv("BB_PARTY_MEMBER_RELAY", "0") == 0);
    BbSettings::Load();
    assert(s.party_ipv6 && !s.party_member_relay);
    assert(bb_test_setenv("BB_PARTY_IPV6", "") == 0 && bb_test_setenv("BB_PARTY_MEMBER_RELAY", "") == 0);
    // 0.7.1 party switches with rows (System > Shared world / Party rewards, the launcher's Party
    // Rules & Sync): on by default, saved under their key, an explicit variable wins at start and
    // nothing is put into the environment (the modules read the setting live).
    const char* const new_envs[] = {"BB_PARTY_KILL_RECORD", "BB_PARTY_NPC_MIRROR", "BB_PARTY_GATHER",
                                    "BB_PARTY_KEY_VIEW", "BB_PARTY_KEY_ITEMS_SUMMONED", "BB_PARTY_NPC_FIGHTS",
                                    "BB_PARTY_ANY_KIDNAP"};
    for (const char* e : new_envs) assert(bb_test_setenv(e, "") == 0);
    BbSettings::Load();
    assert(s.party_kill_record && s.party_npc_mirror && s.party_gather && s.party_key_view &&
           s.party_key_items_summoned && s.party_npc_fights && s.party_any_kidnap);
    for (const char* e : new_envs) assert(!std::getenv(e) || !std::getenv(e)[0]);  // not exported
    s.party_kill_record = false;
    s.party_key_items_summoned = false;
    BbSettings::Save();
    assert(Read(path).find("party_kill_record=0\n") != std::string::npos);
    assert(Read(path).find("party_key_items_summoned=0\n") != std::string::npos);
    assert(Read(path).find("party_gather=1\n") != std::string::npos);
    BbSettings::Load();
    assert(!s.party_kill_record && !s.party_key_items_summoned && s.party_gather);
    assert(bb_test_setenv("BB_PARTY_KILL_RECORD", "1") == 0);
    assert(bb_test_setenv("BB_PARTY_GATHER", "0") == 0);
    BbSettings::Load();
    assert(s.party_kill_record && !s.party_gather);  // the variable wins over the ini
    assert(std::string(std::getenv("BB_PARTY_GATHER")) == "0");
    for (const char* e : new_envs) assert(bb_test_setenv(e, "") == 0);
    s.party_kill_record = true;
    s.party_gather = true;
    s.party_key_items_summoned = true;
    BbSettings::Save();

    // The in-game menu's key rows (after Mrsuss60 90008b39): set, change and remove binding
    // lines, keep the rest of the file, bump the generation runtime_pad.c watches.
    const int generation = BbSettings::BindingsGeneration();
    BbSettings::WriteBindingLines({{"mkkey.cross", std::string("Q")}, {"mkkey.r3", std::string("")}});
    assert(BbSettings::BindingsGeneration() == generation + 1);
    auto lines = BbSettings::ReadLines("mkkey.");
    assert(lines.size() == 2 && lines[0].first == "mkkey.cross" && lines[0].second == "Q" &&
           lines[1].first == "mkkey.r3" && lines[1].second.empty());
    BbSettings::WriteBindingLines({{"mkkey.cross", std::string("Mouse X1,E")}});
    lines = BbSettings::ReadLines("mkkey.");
    assert(lines.size() == 2 && lines[0].second == "Mouse X1,E");
    BbSettings::WriteBindingLines({{"mkkey.cross", std::nullopt}, {"mkkey.r3", std::nullopt},
                                   {"mkkey.square", std::nullopt}});  // Default keys
    assert(BbSettings::ReadLines("mkkey.").empty());
    assert(Read(path).find("party_rendezvous=") != std::string::npos);  // the rest is kept
    // Model detail "Highest" (-2) left objects past their nearest detail distance undrawn
    // (Central Yharnam's corpses popped in and out): read as the game's own detail.
    const char lod_ini[] = "model_lod=-2\n";
    assert(bb_test_write_file(path, lod_ini, sizeof(lod_ini) - 1) == 0);
    BbSettings::Load();
    assert(BbSettings::Get().model_lod == 0);
    std::remove(path);

    // 0.7.2 settings_rev 1: a 0.7.1 bbport.ini's party_rot=1 (the game wrote its old default at
    // every save) goes to 0 once; a later explicit 1 stays; a new file starts at the revision.
    {
        const char old_ini[] = "upscaler=fsr3\nparty_rot=1\nshow_fps=1\n";
        assert(bb_test_write_file(path, old_ini, sizeof(old_ini) - 1) == 0);
        BbSettings::Load();
        assert(!s.party_rot && s.settings_rev == BbSettings::kSettingsRev);
        std::string text = Read(path);
        assert(text.find("party_rot=0\n") != std::string::npos && text.find("party_rot=1") == std::string::npos &&
               text.find("settings_rev=1\n") != std::string::npos && text.find("show_fps=1\n") != std::string::npos);
        s.party_rot = true;  // the player turns it on again
        BbSettings::Save();
        BbSettings::Load();
        assert(s.party_rot && s.settings_rev == BbSettings::kSettingsRev);  // once only
        text = Read(path);
        assert(text.find("party_rot=1\n") != std::string::npos && text.find("settings_rev=1\n") != std::string::npos);
        std::remove(path);
        BbSettings::Load();  // no file: nothing to migrate (rot as it was), the revision is current
        assert(s.party_rot && s.settings_rev == BbSettings::kSettingsRev);
        BbSettings::Save();
        BbSettings::Load();
        assert(s.party_rot && Read(path).find("settings_rev=1\n") != std::string::npos);
    }
    std::remove(path);
    std::puts("PASS: settings save keeps other keys, menu position, DHT rendezvous opt-in, poise clamp, 0.7.1 party switches, key binding lines, model detail -2 read as 0, 0.7.2 rot migration");
}
