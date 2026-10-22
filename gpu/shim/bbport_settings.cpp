// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"
#include "party/party_traversal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace BbSettings {

const PartySwitch PartySwitches[] = {
    {"party_host_revive", "BB_PARTY_B2_HOST_REVIVE", &Values::party_host_revive},
    {"party_host_limbo_revive", "BB_PARTY_HOST_LIMBO_REVIVE", &Values::party_host_limbo_revive},
    {"party_guest_revive_inplace", "BB_PARTY_GUEST_REVIVE_INPLACE", &Values::party_guest_revive_inplace},
    {"party_guest_respawn", "BB_PARTY_GUEST_RESPAWN", &Values::party_guest_respawn},
    {"party_guest_refill", "BB_PARTY_GUEST_REFILL", &Values::party_guest_refill},
    {"party_chalice_rejoin", "BB_PARTY_CHALICE_REJOIN", &Values::party_chalice_rejoin},
    {"party_guest_mark", "BB_PARTY_GUEST_MARK", &Values::party_guest_mark},
    {"party_full_rewards", "BB_PARTY_FULL_REWARDS", &Values::party_full_rewards},
    {"party_items", "BB_PARTY_ITEMS", &Values::party_items},
    {"party_grant_bells", "BB_PARTY_GRANT_BELLS", &Values::party_grant_bells},
    {"party_open_world", "BB_PARTY_OPEN_WORLD", &Values::party_open_world},
    {"party_progress", "BB_PARTY_PROGRESS", &Values::party_progress},
    {"party_progress_npc", "BB_PARTY_PROGRESS_NPC", &Values::party_progress_npc},
    {"party_story", "BB_PARTY_STORY", &Values::party_story},
    {"party_story_mirror", "BB_PARTY_STORY_MIRROR", &Values::party_story_mirror},
    // 0.7.1: read live from Values (BB_PARTY_GATHER is also the director's pre-ring lamp gather,
    // BB_PARTY_KEY_ITEMS_SUMMONED=1 means more without the key view: never exported).
    {"party_kill_record", "BB_PARTY_KILL_RECORD", &Values::party_kill_record, false},
    {"party_npc_mirror", "BB_PARTY_NPC_MIRROR", &Values::party_npc_mirror, false},
    {"party_gather", "BB_PARTY_GATHER", &Values::party_gather, false},
    // 0.7.3: lamp travel voting (party_vote.h reads it live; the host's rule, off by default).
    {"party_travel_vote", "BB_PARTY_TRAVEL_VOTE", &Values::party_travel_vote, false},
    {"party_key_view", "BB_PARTY_KEY_VIEW", &Values::party_key_view, false},
    {"party_key_items_summoned", "BB_PARTY_KEY_ITEMS_SUMMONED", &Values::party_key_items_summoned, false},
    // 0.7.2: the rot cleanses (party_balance reads them live; host's rules).
    {"party_rot_cleanse_boss", "BB_PARTY_ROT_CLEANSE_BOSS", &Values::party_rot_cleanse_boss, false},
    {"party_rot_cleanse_killer", "BB_PARTY_ROT_CLEANSE_KILLER", &Values::party_rot_cleanse_killer, false},
    {"party_rot_cleanse_echoes", "BB_PARTY_ROT_CLEANSE_ECHOES", &Values::party_rot_cleanse_echoes, false},
};
const int PartySwitchCount = int(std::size(PartySwitches));

const char* GuestInsightName(int mode) {
    return mode == 1 ? "full" : mode == 2 ? "0" : "parity";
}

namespace {

int ParseGuestInsight(const std::string& value) {
    return value == "full" ? 1 : value == "0" || value == "off" ? 2 : 0;
}

void PutEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

const char* Path() {
    const char* env = std::getenv("BB_CONFIG");
    return env && env[0] ? env : "bbport.ini";
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

// The file's modification time when this process last read or wrote it (ReloadMouseTuning).
std::mutex file_mutex;
std::filesystem::file_time_type file_time{};

std::filesystem::file_time_type FileTime() {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(std::filesystem::path(Path()), error);
    return error ? std::filesystem::file_time_type{} : time;
}

void NoteFileTime() {
    std::scoped_lock lock{file_mutex};
    file_time = FileTime();
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    if (key == "menu_language") {
        if (value == "en") v.menu_language = MenuLanguage::English;
        else if (value == "ru") v.menu_language = MenuLanguage::Russian;
        else std::printf("Settings: unknown menu_language '%s' (expected en or ru)\n", value.c_str());
    } else if (key == "frame_generation") {
        v.frame_generation = ParseFrameGen(value.c_str());
        if (v.fg_backend == FgFsr && v.frame_generation > 2) v.frame_generation = 2;
    } else if (key == "fg_backend") {
        v.fg_backend = ParseFgBackend(value.c_str());
        if (v.fg_backend == FgFsr && v.frame_generation > 2) v.frame_generation = 2;
    } else if (key == "reflex") {
        v.reflex = ParseReflex(value.c_str());
    } else if (key == "prompt_style") {
        v.prompt_style = ParsePromptStyle(value.c_str());
    } else if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "preset") {
        v.preset = std::clamp(i, 0, PresetCount - 1);
    } else if (key == "sharpen") {
        v.sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 2.0f);
    } else if (key == "jitter") {
        v.jitter = i != 0;
    } else if (key == "reactive") {
        v.reactive = i != 0;
    } else if (key == "object_motion") {
        v.object_motion = i != 0;
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "show_hud") {
        v.show_hud = i != 0;
    } else if (key == "hud_position" || key == "hud_quadrant") {
        static constexpr const char* names[HudPositionCount] = {"top_left", "top_right", "bottom_left",
                                                                "bottom_right"};
        v.hud_position = std::clamp(i, 0, HudPositionCount - 1);
        for (int p = 0; p < HudPositionCount; ++p) {
            if (value == names[p]) v.hud_position = p;
        }
    } else if (key == "hud_opacity") {
        v.hud_opacity = Clamp(f, 0.1f, 1.0f);
    } else if (key == "hud_scale") {
        v.hud_scale = Clamp(f, 0.5f, 2.0f);
    } else if (key == "hud_graph") {
        v.hud_graph = i != 0;
    } else if (key == "hud_vram") {
        v.hud_vram = i != 0;
    } else if (key == "hud_gpu") {
        v.hud_gpu = i != 0;
    } else if (key == "hud_cpu") {
        v.hud_cpu = i != 0;
    } else if (key == "hud_ram") {
        v.hud_ram = i != 0;
    } else if (key == "mk_enabled") {
        v.mk_enabled = i != 0;
    } else if (key == "mk_sens_x") {
        v.mk_sens_x = Clamp(f, 0.1f, 5.0f);
    } else if (key == "mk_sens_y") {
        v.mk_sens_y = Clamp(f, 0.1f, 5.0f);
    } else if (key == "mk_invert_x") {
        v.mk_invert_x = i != 0;
    } else if (key == "mk_invert_y") {
        v.mk_invert_y = i != 0;
    } else if (key == "mk_deadzone") {
        v.mk_deadzone = Clamp(f, 0.0f, 0.2f);
    } else if (key == "audio_boost") {
        v.audio_boost = Clamp(f, 0.5f, 3.0f);
    } else if (key == "mk_smoothing") {
        v.mk_smoothing = Clamp(f, 0.0f, 0.8f);
    } else if (key == "mk_wheel") {
        v.mk_wheel = i != 0;
    } else if (key == "mk_direct_camera") {
        v.mk_direct_camera = i != 0;
    } else if (key == "async_pipelines") {
        v.async_shaders = i != 0;
    } else if (key == "async_shaders") {
        // Saved before async compilation became the default (0.5-pre4), when off meant only
        // "never switched on": an explicit off is kept under async_pipelines from now on.
        if (i != 0) {
            v.async_shaders = true;
        }
    } else if (key == "gpl") {
        v.gpl = i != 0;
    } else if (key == "compile_indicator") {
        v.compile_indicator = i != 0;
    } else if (const auto sw = std::find_if(std::begin(PartySwitches), std::end(PartySwitches),
                                            [&](const PartySwitch& p) { return key == p.key; });
               sw != std::end(PartySwitches)) {
        v.*(sw->member) = i != 0;
    } else if (key == "party_toasts") {
        v.party_toasts = i != 0;
    } else if (key == "party_hud") {
        v.party_hud = i != 0;
    } else if (key == "party_guest_lamps") {
        v.party_guest_lamps = i != 0;
    } else if (key == "party_rot") {
        v.party_rot = i != 0;
    } else if (key == "settings_rev") {
        v.settings_rev = i;
    } else if (key == "party_boss_hp") {
        v.party_boss_hp = std::clamp(i, 0, 400);
    } else if (key == "party_boss_damage") {
        v.party_boss_damage = std::clamp(i, 0, 100);
    } else if (key == "party_boss_poise") {
        v.party_boss_poise = std::clamp(i, 0, 300);  // below 0 staggered less (2026-10-21)
    } else if (key == "party_enemy_hp") {
        v.party_enemy_hp = std::clamp(i, 0, 400);
    } else if (key == "party_enemy_damage") {
        v.party_enemy_damage = std::clamp(i, 0, 100);
    } else if (key == "party_enemy_poise") {
        v.party_enemy_poise = std::clamp(i, 0, 300);
    } else if (key == "party_name_tags") {
        v.party_name_tags = i != 0;
    } else if (key == "party_glow_off") {
        v.party_glow_off = i != 0;
    } else if (key == "party_npc_talk") {
        v.party_npc_talk = i != 0;
    } else if (key == "party_pings") {
        v.party_pings = i != 0;
    } else if (key == "party_through_walls") {
        v.party_through_walls = i != 0;
    } else if (key == "party_stain_marker") {
        v.party_stain_marker = i != 0;
    } else if (key == "party_friendly_fire") {
        v.party_friendly_fire = i != 0;
    } else if (key == "party_share_lamps") {
        v.party_share_lamps = i != 0;
    } else if (key == "party_guests_bring_lamps") {
        v.party_guests_bring_lamps = i != 0;
    } else if (key == "party_rendezvous") {
        v.party_rendezvous = party::traversal::rendezvous_switch(value.c_str(), false);
    } else if (key == "party_member_relay") {
        v.party_member_relay = party::traversal::rendezvous_switch(value.c_str(), true);
    } else if (key == "party_ipv6") {
        v.party_ipv6 = party::traversal::rendezvous_switch(value.c_str(), false);
    } else if (key == "party_npc_talk_range") {
        v.party_npc_talk_range = std::clamp(i, 0, 500);
    } else if (key == "party_guest_insight") {
        v.party_guest_insight = ParseGuestInsight(value);
    } else if (key == "party_overhead") {
        v.party_overhead = std::clamp(i, 0, 5);
    } else if (key == "hdr") {
        v.hdr = i != 0 || value == "on" || value == "true";
    } else if (key == "hdr_peak_nits") {
        v.hdr_peak_nits = value == "auto" || i <= 0 ? 0 : std::clamp(i, 100, 10000);
    } else if (key == "hdr_paper_white") {
        v.hdr_paper_white = std::clamp(i, 80, 500);
    } else if (key == "hdr_strength") {
        v.hdr_strength = Clamp(f, 0.0f, 1.5f);
    } else if (key == "hdr_saturation") {
        v.hdr_saturation = Clamp(f, 0.5f, 1.5f);
    } else if (key == "menu_pos") {
        float x = -1.0f, y = -1.0f;
        if (std::sscanf(value.c_str(), "%f,%f", &x, &y) == 2 && x >= 0.0f && x <= 1.0f && y >= 0.0f &&
            y <= 1.0f) {
            v.menu_x = x;
            v.menu_y = y;
        }
    } else if (key == "fsr4_auto_exposure") {
        v.fsr4_auto_exposure = i != 0;
    } else if (key == "fsr4_invert_jitter") {
        v.fsr4_invert_jitter = i != 0;
    } else if (key == "volumetric_res") {
        // 0 game (half resolution), 1 = 2x the pixels, 2 = the scene's resolution; also by name.
        v.volumetric_res = value == "full" ? 2 : value == "2x" || value == "double" ? 1
                         : value == "game" || value == "off" ? 0 : std::clamp(i, 0, 2);
    } else if (key == "model_lod") {
        // 2 (the community patch "Model LOD 2 (Lowest)") is read as 1: the game draws nothing
        // for a model without a third detail level, and some floors have none (the Lecture
        // Building's floor showed the background through it). -2 (the community patch "Model
        // LOD -2 (Highest)") is read as 0: its bias reaches only one of the game's three model draw
        // paths, and many objects past their nearest detail distance (10-15 m) were then not drawn
        // at all, so Central Yharnam's corpses popped in close and vanished again further away.
        v.model_lod = std::clamp(i, 0, 1);
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "output_res") {
        for (int r = 0; r < OutputCount; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
            }
        }
    } else {
        for (int e = 0; e < EffectCount; ++e) {
            if (key == Effects[e].key) {
                v.effects[e] = i != 0;
            }
        }
    }
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

const char* MenuText(const char* english, const char* russian) {
    return Get().menu_language == MenuLanguage::Russian ? russian : english;
}

void Load() {
    auto& v = Get();
    v.frame_generation = 0; // off unless bbport.ini or the environment chooses it
    v.fg_backend = FgDlss;
    v.reflex = ReflexOff;
    for (int e = 0; e < EffectCount; ++e) {
        v.effects[e] = Effects[e].default_on;
    }
    // bbport: the menu's language until one is chosen in it (menu_language in bbport.ini): the
    // launcher's (BB_MENU_LANGUAGE), else the system's, as the launcher picks its own.
    {
        const char* lang = std::getenv("BB_MENU_LANGUAGE");
#ifndef _WIN32
        if (!lang || !*lang) {
            for (const char* key : {"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"}) {
                if (const char* value = std::getenv(key); value && *value) {
                    lang = value;
                    break;
                }
            }
        }
#endif
        // bbport (Windows fork): English unless BB_MENU_LANGUAGE=ru (or menu_language=ru in
        // bbport.ini); POSIX locale variables (e.g. from an MSYS shell) are not consulted.
        v.menu_language = lang && (lang[0] == 'r' || lang[0] == 'R') && (lang[1] == 'u' || lang[1] == 'U')
                              ? MenuLanguage::Russian
                              : MenuLanguage::English;
    }
    v.settings_rev = 0;
    bool have_file = false;
    if (FILE* file = std::fopen(Path(), "r")) {
        have_file = true;
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", Path());
    }
    // One-time changes of saved settings (kSettingsRev; the launcher does the same when it starts
    // first). A new bbport.ini starts at the current revision.
    if (!have_file) {
        v.settings_rev = kSettingsRev;
    } else if (v.settings_rev < kSettingsRev) {
        const int was = v.settings_rev.load();
        if (was < 1) v.party_rot = false;  // rev 1 (0.7.2): rot off by default, the old saved 1 was the old default
        v.settings_rev = kSettingsRev;
        WriteBindingLines({{"party_rot", std::string(v.party_rot ? "1" : "0")},
                           {"settings_rev", std::to_string(kSettingsRev)}});
        std::printf("Settings: bbport.ini settings_rev %d -> %d (rot off by default: party_rot=0 once)\n", was,
                    kSettingsRev);
    }
    NoteFileTime();
    // Environment overrides (scripts, A/B tests).
    if (const char* env = std::getenv("BB_UPSCALER")) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(env, UpscalerName(u)) == 0) v.upscaler = u;
        }
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"BB_FG_BACKEND", "fg_backend"},          {"BB_FRAME_GENERATION", "frame_generation"},
        {"BB_REFLEX", "reflex"},                  {"BB_PROMPT_STYLE", "prompt_style"},
        {"BB_FSR_SHARPNESS", "sharpness"},        {"BB_JITTER", "jitter"},
        {"BB_REACTIVE", "reactive"},              {"BB_REACTIVE_SCALE", "reactive_scale"},
        {"BB_REACTIVE_THRESHOLD", "reactive_threshold"}, {"BB_REACTIVE_MAX", "reactive_max"},
        {"BB_UPSCALE_PRESET", "preset"},            {"BB_OBJECT_MOTION", "object_motion"},
        {"BB_GPL", "gpl"},                          {"BB_COMPILE_INDICATOR", "compile_indicator"},
        {"BB_HUD", "show_hud"},                     {"BB_HUD_POSITION", "hud_position"},
        {"BB_MOUSE_KEYBOARD", "mk_enabled"},
        {"BB_MOUSE_DIRECT_CAMERA", "mk_direct_camera"},
        {"BB_HDR", "hdr"},                          {"BB_HDR_PEAK", "hdr_peak_nits"},
        {"BB_HDR_PAPER_WHITE", "hdr_paper_white"},  {"BB_HDR_STRENGTH", "hdr_strength"},
        {"BB_HDR_SATURATION", "hdr_saturation"},   {"BB_PARTY_OVERHEAD", "party_overhead"},
        {"BB_VOLUMETRIC_RES", "volumetric_res"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env)) {
            Set(v, key, value);
        }
    }
    // The DHT rendezvous opt-in: the launcher's Party tab (BB_PARTY_RENDEZVOUS) wins over the ini.
    v.party_rendezvous = party::traversal::rendezvous_switch(std::getenv("BB_PARTY_RENDEZVOUS"), v.party_rendezvous);
    // F2 (0.7.1): the member relay and IPv6 (on by default): the launcher's Party tab wins over the ini.
    v.party_member_relay = party::traversal::rendezvous_switch(std::getenv("BB_PARTY_MEMBER_RELAY"), v.party_member_relay);
    v.party_ipv6 = party::traversal::rendezvous_switch(std::getenv("BB_PARTY_IPV6"), v.party_ipv6);
    // I1 friendly fire (tests, scripted hosts): BB_PARTY_FRIENDLY_FIRE=0/1 wins over the ini.
    if (const char* ff = std::getenv("BB_PARTY_FRIENDLY_FIRE"); ff && (ff[0] == '0' || ff[0] == '1'))
        v.party_friendly_fire = ff[0] == '1';
    if (const char* sl = std::getenv("BB_PARTY_SHARE_LAMPS"); sl && (sl[0] == '0' || sl[0] == '1'))
        v.party_share_lamps = sl[0] == '1';
    if (const char* gb = std::getenv("BB_PARTY_GUESTS_BRING_LAMPS"); gb && (gb[0] == '0' || gb[0] == '1'))
        v.party_guests_bring_lamps = gb[0] == '1';
    // C4 rot (tests, scripted hosts): BB_PARTY_ROT=0/1 wins over the ini.
    if (const char* ro = std::getenv("BB_PARTY_ROT"); ro && (ro[0] == '0' || ro[0] == '1')) v.party_rot = ro[0] == '1';
    // The party gameplay switches: BB_PARTY_*=0/1 (tools/mp, scripts) wins over the ini; an unset
    // variable gets the ini's value, for the party modules that read their switch at start.
    // BB_PARTY_SWITCH_ENV lists the variables put there: a restart from the menu (runtime_restart
    // inherits this environment) reads the ini again instead of the old exported values.
    {
        const char* exported_before = std::getenv("BB_PARTY_SWITCH_ENV");
        const std::string before = exported_before ? "," + std::string(exported_before) + "," : "";
        std::string exported;
        const auto take = [&](const char* env, auto&& apply, const std::string& ini_value) {
            const char* e = std::getenv(env);
            if (e && e[0] && before.find("," + std::string(env) + ",") == std::string::npos) {
                apply(e);
                return;
            }
            PutEnv(env, ini_value.c_str());
            exported += (exported.empty() ? "" : ",") + std::string(env);
        };
        for (const PartySwitch& sw : PartySwitches) {
            if (!sw.env_export) {
                if (const char* e = std::getenv(sw.env); e && e[0]) v.*(sw.member) = e[0] != '0';
                continue;
            }
            take(sw.env, [&](const char* e) { v.*(sw.member) = e[0] != '0'; },
                 (v.*(sw.member)).load() ? "1" : "0");
        }
        take("BB_PARTY_GUEST_INSIGHT", [&](const char* e) { v.party_guest_insight = ParseGuestInsight(e); },
             GuestInsightName(v.party_guest_insight));
        PutEnv("BB_PARTY_SWITCH_ENV", exported.c_str());
    }
    // Async pipelines: BB_ASYNC_SHADERS (this port) wins over BB_ASYNC_PIPELINES (upstream's name).
    if (const char* value = std::getenv("BB_ASYNC_SHADERS"); value && value[0]) {
        Set(v, "async_pipelines", value);
    } else if (const char* alias = std::getenv("BB_ASYNC_PIPELINES"); alias && alias[0]) {
        Set(v, "async_pipelines", alias);
    }
    v.startup_preset = v.preset;
    v.startup_upscaler = v.upscaler;
    v.startup_object_motion = v.object_motion;
    for (int e = 0; e < EffectCount; ++e) {
        v.startup_effects[e] = v.effects[e];
    }
    v.startup_model_lod = v.model_lod;
    v.startup_output_res = v.output_res;
    v.startup_live_resolution = v.live_resolution;
    v.startup_gpl = v.gpl;
    v.startup_frame_generation = v.frame_generation;
    v.startup_fg_backend = v.fg_backend;
    v.startup_reflex = v.reflex;
    v.startup_hdr = v.hdr;
    if (const char* pattern = std::getenv("BB_HDR_PATTERN")) v.hdr_calibration = pattern[0] == '1';
}

float HdrPeakNits() {
    const auto& v = Get();
    const int setting = v.hdr_peak_nits;
    float peak = setting > 0 ? float(setting) : v.hdr_display_peak > 0.0f ? v.hdr_display_peak.load() : 1000.0f;
    return std::clamp(peak, 100.0f, 10000.0f);
}

const char* ReflexModeName(int mode) {
    return mode == ReflexBoost ? "boost" : mode == ReflexOn ? "on" : "off";
}

const char* PromptStyleName(int style) {
    static constexpr const char* names[PromptStyleCount] = {"auto", "playstation", "xbox", "nintendo",
                                                            "keyboard"};
    return names[std::clamp(style, 0, PromptStyleCount - 1)];
}

int ParsePromptStyle(const char* value) {
    std::string text{value ? value : ""};
    for (auto& ch : text) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    for (int s = 0; s < PromptStyleCount; ++s) {
        if (text == PromptStyleName(s) || text == std::to_string(s)) return s;
    }
    if (text == "ps" || text == "ps4" || text == "ps5") return PromptPlayStation;
    if (text == "switch") return PromptNintendo;
    if (text == "kbm" || text == "mouse") return PromptKeyboard;
    if (!text.empty()) std::printf("Settings: unknown prompt_style '%s' (auto, playstation, xbox, nintendo, keyboard)\n", text.c_str());
    return PromptAuto;
}

int ParseReflex(const char* value) {
    const std::string_view text{value ? value : ""};
    return text == "boost" || text == "2" ? ReflexBoost : text == "on" || text == "1" ? ReflexOn
                                                                                       : ReflexOff;
}

const char* FgBackendName(int backend) {
    return backend == FgFsr ? "fsr" : "dlss";
}

const char* FrameGenName(int factor) {
    return factor == 2 ? "2x" : factor == 3 ? "3x" : factor == 4 ? "4x" : "off";
}

int ParseFrameGen(const char* value) {
    const std::string_view text{value ? value : ""};
    return text == "2x" || text == "2" ? 2 : text == "3x" || text == "3" ? 3
         : text == "4x" || text == "4" ? 4 : 0;
}

int ParseFgBackend(const char* value) {
    const std::string_view text{value ? value : ""};
    return text == "fsr" || text == "1" ? FgFsr : FgDlss;
}

void ConfigureUpscalerSupport(bool fsr4, bool fsr411) {
    auto& v = Get();
    v.fsr4_supported = fsr4;
    v.fsr411_supported = fsr4 && fsr411;
    const int requested = v.upscaler;
    if ((requested == UpscalerFsr4 && !v.fsr4_supported) ||
        (requested == UpscalerFsr411 && !v.fsr411_supported)) {
        v.fsr4_problem = "GPU does not support the selected FSR 4 shaders; using FSR 3.1";
        std::printf("Upscaler: %s unsupported on this GPU; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested));
        v.upscaler = UpscalerFsr3;
    }
}

void ConfigureDlssSupport(bool available, const char* problem) {
    auto& v = Get();
    v.dlss_supported = available;
    static std::string kept;
    kept = problem ? problem : "";
    v.dlss_problem = available || kept.empty() ? nullptr : kept.c_str();
    if (v.upscaler == UpscalerDlss && !available) {
        std::printf("Upscaler: DLSS unavailable (%s); falling back to FSR 3.1\n",
                    kept.empty() ? "the DLSS bridge or NVIDIA's DLSS library is missing" : kept.c_str());
        v.upscaler = UpscalerFsr3;
    }
}

bool FixedRenderSession() {
    const char* size = std::getenv("BB_RENDER_RES");
    return size && size[0];
}

int RenderPreset() {
    const auto& v = Get();
    return FixedRenderSession() ? v.startup_preset :
        v.upscaler == UpscalerTaa ? NativeAA : v.preset.load();
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    // TAA needs the live path (native guest targets): run.sh selects it on restart. Ultrawide
    // outputs patch the camera and HUD at startup.
    if (Ultrawide(v.output_res) != Ultrawide(v.startup_output_res)) return true;
    return FixedRenderSession() &&
        (v.preset != v.startup_preset || v.output_res != v.startup_output_res ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa));
}

bool RestartNeeded() {
    const auto& s = Get();
    bool restart = s.object_motion != s.startup_object_motion || s.model_lod != s.startup_model_lod ||
                   s.live_resolution != s.startup_live_resolution || s.gpl != s.startup_gpl ||
                   s.frame_generation != s.startup_frame_generation ||
                   (s.frame_generation >= 2 && s.fg_backend != s.startup_fg_backend) ||
                   s.reflex != s.startup_reflex || s.hdr != s.startup_hdr ||
                   ResolutionNeedsRestart();
    for (int e = 0; e < EffectCount; ++e) {
        restart |= s.effects[e] != s.startup_effects[e];
    }
    return restart;
}

namespace {
/// Rewrites the file with `keys` set (a value) or removed (none), in the order given for new
/// ones; every other line stays as it is. Callers hold save_mutex.
bool TraceIni() {
    static const bool on = [] {
        const char* env = std::getenv("BB_GAME_MENU_TRACE");
        return env && env[0] == '1';
    }();
    return on;
}

void WriteMergedLocked(const std::vector<std::pair<std::string, std::optional<std::string>>>& keys) {
    std::string out;
    bool had_lines = false;
    std::vector<bool> written(keys.size());
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof(line), file)) {
            had_lines = true;
            std::string text{line};
            text.erase(text.find_last_not_of("\r\n") + 1);
            const auto eq = text.find('=');
            if (!text.empty() && text[0] != '#' && eq != std::string::npos) {
                std::string key = text.substr(0, eq);
                key.erase(key.find_last_not_of(" \t") + 1);
                key.erase(0, key.find_first_not_of(" \t"));
                const auto it = std::find_if(keys.begin(), keys.end(),
                                             [&](const auto& k) { return k.first == key; });
                if (it != keys.end()) {
                    if (it->second) {
                        out += it->first + "=" + *it->second + "\n";
                        // BB_GAME_MENU_TRACE=1 (scripted menu tests, smoke_party "rules"): what changed.
                        if (TraceIni() && text.substr(eq + 1) != *it->second)
                            std::printf("Settings: bbport.ini %s=%s\n", it->first.c_str(), it->second->c_str());
                    }
                    written[size_t(it - keys.begin())] = true;
                    continue;
                }
            }
            out += text + "\n";
        }
        std::fclose(file);
    }
    if (!had_lines) {
        out = "# bbport settings (in-game menu: Insert; L3+R3 with BB_OVERLAY_PAD=1)\n";
    }
    for (size_t k = 0; k < keys.size(); ++k) {
        if (!written[k] && keys[k].second) {
            out += keys[k].first + "=" + *keys[k].second + "\n";
            if (TraceIni()) std::printf("Settings: bbport.ini %s=%s\n", keys[k].first.c_str(), keys[k].second->c_str());
        }
    }
    // Replaced whole: a crash while writing leaves the old file.
    const std::string temporary = std::string(Path()) + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "w");
    bool ok = file && std::fwrite(out.data(), 1, out.size(), file) == out.size();
    ok = file && std::fclose(file) == 0 && ok;
#ifdef _WIN32
    // std::rename does not replace an existing file on Windows: every save after the first failed
    // and the menu's choices (upscaler, preset, resolution...) were lost at the next start.
    ok = ok && MoveFileExA(temporary.c_str(), Path(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    ok = ok && std::rename(temporary.c_str(), Path()) == 0;
#endif
    if (!ok) {
        std::printf("Settings: cannot write %s\n", Path());
    }
    NoteFileTime();
}

std::mutex save_mutex; // the menu (present or window thread) and F11 (window thread) both save
std::atomic<int> bindings_generation{0};
} // namespace

void Save() {
    std::scoped_lock save_lock{save_mutex};
    const auto& v = Get();
    // The keys the menu writes, in this order; the rest of the file stays as it is: the
    // launcher's controls (key.* / pad.*) and its other keys, comments.
    std::vector<std::pair<std::string, std::optional<std::string>>> keys;
    const auto put = [&](const char* key, std::string value) { keys.emplace_back(key, std::move(value)); };
    const auto fixed = [](float value, int digits) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.*f", digits, value);
        return std::string(text);
    };
    const auto flag = [](bool value) { return std::string(value ? "1" : "0"); };
    put("menu_language", v.menu_language == MenuLanguage::Russian ? "ru" : "en");
    put("upscaler", UpscalerName(v.upscaler));
    put("frame_generation", FrameGenName(v.frame_generation));
    put("fg_backend", FgBackendName(v.fg_backend));
    put("reflex", ReflexModeName(v.reflex));
    put("preset", std::to_string(v.preset.load()));
    put("sharpen", flag(v.sharpen));
    put("sharpness", fixed(v.sharpness, 2));
    put("jitter", flag(v.jitter));
    put("reactive", flag(v.reactive));
    put("object_motion", flag(v.object_motion));
    put("reactive_scale", fixed(v.reactive_scale, 2));
    put("reactive_threshold", fixed(v.reactive_threshold, 2));
    put("reactive_max", fixed(v.reactive_max, 2));
    put("debug_view", std::to_string(v.debug_view.load()));
    put("show_fps", flag(v.show_fps));
    put("show_hud", flag(v.show_hud));
    put("hud_position", std::to_string(v.hud_position.load()));
    put("hud_opacity", fixed(v.hud_opacity, 2));
    put("hud_scale", fixed(v.hud_scale, 2));
    put("hud_graph", flag(v.hud_graph));
    put("hud_vram", flag(v.hud_vram));
    put("hud_gpu", flag(v.hud_gpu));
    put("hud_cpu", flag(v.hud_cpu));
    put("hud_ram", flag(v.hud_ram));
    put("mk_enabled", flag(v.mk_enabled));
    put("mk_sens_x", fixed(v.mk_sens_x, 2));
    put("mk_sens_y", fixed(v.mk_sens_y, 2));
    put("mk_invert_x", flag(v.mk_invert_x));
    put("mk_invert_y", flag(v.mk_invert_y));
    put("mk_deadzone", fixed(v.mk_deadzone, 2));
    put("mk_smoothing", fixed(v.mk_smoothing, 2));
    put("audio_boost", fixed(v.audio_boost, 2));
    put("mk_wheel", flag(v.mk_wheel));
    put("mk_direct_camera", flag(v.mk_direct_camera));
    put("prompt_style", PromptStyleName(v.prompt_style));
    put("async_pipelines", flag(v.async_shaders));
    put("gpl", flag(v.gpl));
    put("compile_indicator", flag(v.compile_indicator));
    put("party_toasts", flag(v.party_toasts));
    put("party_hud", flag(v.party_hud));
    put("party_name_tags", flag(v.party_name_tags));
    put("party_glow_off", flag(v.party_glow_off));
    put("party_npc_talk", flag(v.party_npc_talk));
    put("party_pings", flag(v.party_pings));
    put("party_through_walls", flag(v.party_through_walls));
    put("party_stain_marker", flag(v.party_stain_marker));
    put("party_friendly_fire", flag(v.party_friendly_fire));
    put("party_share_lamps", flag(v.party_share_lamps));
    put("party_guests_bring_lamps", flag(v.party_guests_bring_lamps));
    put("party_rendezvous", flag(v.party_rendezvous));
    put("party_member_relay", flag(v.party_member_relay));
    put("party_ipv6", flag(v.party_ipv6));
    for (const PartySwitch& sw : PartySwitches) put(sw.key, flag(v.*(sw.member)));
    put("party_guest_insight", GuestInsightName(v.party_guest_insight));
    put("party_npc_talk_range", std::to_string(v.party_npc_talk_range.load()));
    put("party_guest_lamps", flag(v.party_guest_lamps));
    put("party_rot", flag(v.party_rot));
    put("settings_rev", std::to_string(v.settings_rev.load()));
    put("party_boss_hp", std::to_string(v.party_boss_hp.load()));
    put("party_boss_damage", std::to_string(v.party_boss_damage.load()));
    put("party_boss_poise", std::to_string(v.party_boss_poise.load()));
    put("party_enemy_hp", std::to_string(v.party_enemy_hp.load()));
    put("party_enemy_damage", std::to_string(v.party_enemy_damage.load()));
    put("party_enemy_poise", std::to_string(v.party_enemy_poise.load()));
    put("party_overhead", std::to_string(v.party_overhead.load()));
    put("hdr", flag(v.hdr));
    put("hdr_peak_nits", v.hdr_peak_nits > 0 ? std::to_string(v.hdr_peak_nits.load()) : std::string("auto"));
    put("hdr_paper_white", std::to_string(v.hdr_paper_white.load()));
    put("hdr_strength", fixed(v.hdr_strength, 2));
    put("hdr_saturation", fixed(v.hdr_saturation, 2));
    put("fsr4_auto_exposure", flag(v.fsr4_auto_exposure));
    put("fsr4_invert_jitter", flag(v.fsr4_invert_jitter));
    // Read by patches.py at start.
    for (int e = 0; e < EffectCount; ++e) {
        put(Effects[e].key, flag(v.effects[e]));
    }
    put("model_lod", std::to_string(v.model_lod.load()));
    put("volumetric_res", std::to_string(v.volumetric_res.load()));
    put("output_res", std::to_string(OutputWidths[v.output_res]) + "x" +
                          std::to_string(OutputHeights[v.output_res]));
    // Read by run.sh at start.
    put("live_resolution", v.live_resolution < 0 ? "auto" : flag(v.live_resolution != 0));
    if (v.menu_x >= 0.0f && v.menu_y >= 0.0f) {
        put("menu_pos", fixed(v.menu_x, 4) + "," + fixed(v.menu_y, 4));
    }

    WriteMergedLocked(keys);
}

std::vector<std::pair<std::string, std::string>> ReadLines(const std::string& prefix) {
    std::vector<std::pair<std::string, std::string>> lines;
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            std::string key = text.substr(0, eq);
            key.erase(key.find_last_not_of(" \t") + 1);
            key.erase(0, key.find_first_not_of(" \t"));
            if (key.compare(0, prefix.size(), prefix) == 0) {
                lines.emplace_back(key, text.substr(eq + 1));
            }
        }
        std::fclose(file);
    }
    return lines;
}

void WriteBindingLines(const std::vector<std::pair<std::string, std::optional<std::string>>>& lines) {
    {
        std::scoped_lock save_lock{save_mutex};
        WriteMergedLocked(lines);
    }
    bindings_generation.fetch_add(1);
}

int BindingsGeneration() {
    return bindings_generation.load();
}

void ReloadMouseTuning() {
    {
        std::scoped_lock lock{file_mutex};
        const auto time = FileTime();
        if (time == file_time) {
            return;
        }
        file_time = time;
    }
    // Only the tuning: mk_enabled comes from the launcher (BB_MOUSE_KEYBOARD) or the menu, and
    // the other keys are the menu's (a full re-read would undo its unsaved changes).
    static constexpr const char* tuning[] = {"mk_sens_x", "mk_sens_y", "mk_invert_x", "mk_invert_y",
                                             "mk_deadzone", "mk_smoothing", "mk_wheel",
                                             "mk_direct_camera", "prompt_style"};
    auto& v = Get();
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            const std::string key = text.substr(0, eq);
            if (std::find(std::begin(tuning), std::end(tuning), key) != std::end(tuning)) {
                Set(v, key, text.substr(eq + 1));
            }
        }
        std::fclose(file);
    }
}

float PresetScale(int preset) {
    static constexpr float scales[PresetCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return scales[std::clamp(preset, 0, PresetCount - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* names[PresetCount] = {"Native AA", "Quality", "Balanced",
                                                       "Performance", "Ultra Performance"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr3", "fsr4", "fsr411", "taa", "dlss"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

} // namespace BbSettings
