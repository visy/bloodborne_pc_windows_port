// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: user settings changed at run time from the in-game menu (bbport_overlay.h) and kept
// in bbport.ini (BB_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace BbSettings {

enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerDlss = 5, UpscalerCount };
/// FSR 4 v07 or FSR 4.1.1: the same inputs, settings and placement in the frame.
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411;
}
/// Frame generation (from Supermedo/bloodborne_pc PR #69 by FIocker): the provider, and the
/// factor (0 off, 2..4: presented frames per rendered one; FSR: 2x only). On restart.
enum FgBackend : int { FgDlss = 0, FgFsr = 1, FgBackendCount };
const char* FgBackendName(int backend);
const char* FrameGenName(int factor);
int ParseFrameGen(const char* value);
int ParseFgBackend(const char* value);
/// NVIDIA Reflex (VK_NV_low_latency2; from Supermedo/bloodborne_pc PR #69 by FIocker). On restart.
/// ReflexAuto (the default, 0.7.8): on when the GPU is NVIDIA and the driver has the extension, off elsewhere.
enum ReflexMode : int { ReflexOff = 0, ReflexOn = 1, ReflexBoost = 2, ReflexAuto = 3, ReflexModeCount };
/// AMD Anti-Lag 2 (VK_AMD_anti_lag; 0.7.8): Auto (default) = on when the GPU is AMD and the driver has the extension.
enum AntiLagMode : int { AntiLagOff = 0, AntiLagOn = 1, AntiLagAuto = 2, AntiLagModeCount };
const char* AntiLagModeName(int mode);
int ParseAntiLag(const char* value);
const char* ReflexModeName(int mode);
int ParseReflex(const char* value);
/// Button prompts' icon set (src/pad_prompts.h PROMPT_STYLE_*): Auto (the device used last and the
/// gamepad's type), or one set: PlayStation, Xbox, Nintendo (the gamepad's prompts), Keyboard.
enum PromptStyle : int { PromptAuto = 0, PromptPlayStation, PromptXbox, PromptNintendo, PromptKeyboard,
                         PromptStyleCount };
const char* PromptStyleName(int style);
int ParsePromptStyle(const char* value);
/// Texture quality (texture_quality in bbport.ini; BB_TEXTURE_QUALITY overrides; applies after a
/// restart): block-compressed materials loaded without their largest mip levels to save VRAM.
/// Mip skipping from Mrsuss60/bloodborne_pc_windows_port 8841db5b, after deadinside28/bloodborne_pc.
enum TextureQuality : int { TqAuto = 0, TqFull = 1, TqHalf = 2, TqQuarter = 3, TqCount = 4 };
inline const char* TextureQualityName(int quality) {
    switch (quality) {
    case TqAuto: return "auto";
    case TqFull: return "full";
    case TqHalf: return "half";
    case TqQuarter: return "quarter";
    default: return "auto";
    }
}
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCount };
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };
enum class MenuLanguage { English, Russian };
/// Corner of the performance HUD (ini hud_position; hud_quadrant is the same key in
/// Mrsuss60/bloodborne_pc_windows_port, read too).
enum HudPosition : int { HudTopLeft = 0, HudTopRight = 1, HudBottomLeft = 2, HudBottomRight = 3,
                         HudPositionCount };

/// Game effects switched by the community patches at start (patches.py EFFECTS): ini key,
/// menu label, default (the game's own behaviour).
struct Effect {
    const char* key;
    const char* label;
    const char* label_ru;
    bool default_on;
};
inline constexpr Effect Effects[] = {
    {"effect_chromatic_aberration", "Chromatic aberration", "Хроматическая аберрация", true},
    {"effect_dof", "Depth of field (DoF)", "Глубина резкости (DoF)", true},
    {"effect_motion_blur", "Motion blur", "Размытие в движении", true},
    {"effect_ssao", "Ambient occlusion (SSAO)", "Затенение SSAO", true},
    {"effect_game_aa", "Game's own anti-aliasing", "Собственное сглаживание игры", true},
    {"effect_dynamic_shadows", "Shadows from dynamic lights", "Тени от динамических источников", true},
    {"effect_ssr", "Screen-space reflections (not in original game)", "Отражения SSR (не было в игре)", false},
    {"skip_intro", "Skip startup intros", "Пропуск заставок при запуске", false},
    {"debug_camera", "Free camera (Cross + L3)", "Свободная камера (Cross + L3)", false},
    {"debug_menu", "Debug menu (requires font files)", "Debug menu (нужны файлы шрифтов)", false},
};
inline constexpr int EffectCount = int(sizeof(Effects) / sizeof(Effects[0]));
/// Output resolutions: the upscaler's output and the UI host targets. The 16:9 ones change
/// live; the ultrawide ones (21:9, 32:9; from Supermedo/bloodborne_pc PR #70) need the startup
/// patch (patches.py ultrawide_writes, run.bat) and a restart.
inline constexpr int OutputWidths[] = {1280, 1920, 2560, 3840, 2560, 3440, 3840, 5120, 3840, 5120};
inline constexpr int OutputHeights[] = {720, 1080, 1440, 2160, 1080, 1440, 1600, 2160, 1080, 1440};
inline constexpr int OutputCount = 10;
inline constexpr int OutputDefault = 1; ///< 1920x1080, the game's own size
/// Wider than 16:9: camera and HUD patched at startup.
inline bool Ultrawide(int output) {
    return output >= 0 && output < OutputCount &&
           OutputWidths[output] * 9 > OutputHeights[output] * 16;
}

struct Values {
    /// bbport (Windows fork): the menu defaults to English; Russian only when chosen
    /// (menu_language=ru in bbport.ini or BB_MENU_LANGUAGE=ru).
    std::atomic<MenuLanguage> menu_language{MenuLanguage::English};
    std::atomic<int> upscaler{UpscalerOff};
    /// Frame generation: 0 off (default), 2/3/4x; the provider FgDlss / FgFsr (bbport.ini
    /// frame_generation / fg_backend, BB_FRAME_GENERATION / BB_FG_BACKEND). On restart.
    std::atomic<int> frame_generation{0};
    std::atomic<int> fg_backend{FgDlss};
    /// Measured while frame generation runs: rendered (source) and presented frames per second.
    std::atomic<float> source_fps{0.0f}, display_fps{0.0f};
    /// NVIDIA Reflex: auto (default), off, on, on + boost (bbport.ini reflex, BB_REFLEX). On restart.
    std::atomic<int> reflex{ReflexAuto};
    /// Reflex runs this session (chosen at start and the driver supports it; set by the renderer).
    std::atomic<bool> reflex_active{false};
    /// What reflex resolves to this session (Off / On / Boost; Auto -> On on NVIDIA with the extension), set by the renderer.
    std::atomic<int> reflex_resolved{ReflexOff};
    /// AMD Anti-Lag 2 (bbport.ini anti_lag, BB_ANTI_LAG): auto (default), on, off. On restart.
    std::atomic<int> anti_lag{AntiLagAuto};
    std::atomic<bool> anti_lag_active{false};
    /// Variable refresh rate (G-Sync / FreeSync; bbport.ini vrr, BB_VRR; default off, no portable way to detect it):
    /// the frame limit stays under the display's refresh and presents are not vblank-synced. On restart.
    std::atomic<bool> vrr{false};
    /// Fresh gamepad state at the game's read (bbport.ini pad_fresh, BB_PAD_FRESH; default on; live): the pad is read again
    /// from the device at that moment instead of the last event-loop pump (up to ~2 ms old).
    std::atomic<bool> pad_fresh{true};
    /// Low-latency frame pacing (bbport.ini ll_pacing, BB_LL_PACING; default on; live): the game starts reading input for
    /// a frame only when the frames before it have been presented, so flips do not pile up in front of the display (a saturated queue held 39 ms of lag at 60 FPS).
    std::atomic<bool> ll_pacing{true};
    /// Frame generation runs this session: chosen at start and its provider available (set by
    /// the presenter before the first frame).
    std::atomic<bool> fg_active{false};
    /// Why the chosen frame generation does not run (null while it runs or is off).
    std::atomic<const char*> fg_problem{nullptr};
    std::atomic<int> preset{NativeAA};
    std::atomic<bool> sharpen{true};
    std::atomic<float> sharpness{0.3f};
    std::atomic<bool> jitter{false};
    std::atomic<bool> reactive{false};
    std::atomic<bool> object_motion{false};
    std::atomic<float> reactive_scale{1.0f};
    std::atomic<float> reactive_threshold{0.2f};
    std::atomic<float> reactive_max{0.9f};
    std::atomic<int> debug_view{DebugNone};
    std::atomic<bool> show_fps{false};
    /// Performance HUD (F11, menu, BB_HUD; from Mrsuss60/bloodborne_pc_windows_port): FPS and
    /// frame time always, the other metrics each switchable. Replaces the FPS counter while on.
    std::atomic<bool> show_hud{false};
    std::atomic<int> hud_position{HudBottomRight};
    std::atomic<float> hud_opacity{0.75f};
    std::atomic<float> hud_scale{1.0f};
    std::atomic<bool> hud_graph{true}, hud_vram{true}, hud_gpu{true}, hud_cpu{true}, hud_ram{true};
    /// Mouse & keyboard controls (BB_MOUSE_KEYBOARD, menu; scheme from
    /// Mrsuss60/bloodborne_pc_windows_port): the mouse turns the camera and attacks, the keyboard
    /// takes a Souls-PC layout (runtime_pad.c). Off: the classic keyboard layout, mouse unused.
    std::atomic<bool> mk_enabled{false};
    std::atomic<float> mk_sens_x{1.0f}, mk_sens_y{1.0f};
    std::atomic<bool> mk_invert_x{false}, mk_invert_y{false};
    std::atomic<float> mk_deadzone{0.05f};  ///< fraction of full stick deflection ignored
    std::atomic<float> audio_boost{1.4f};   ///< master volume multiplier (1.0 = original); BB_AUDIO_BOOST overrides
    std::atomic<float> mk_smoothing{0.2f};  ///< 0 raw .. 0.8 heavy smoothing of the mouse look
    /// The wheel as d-pad up / down (quick items), one short press per notch (upstream 0.51, #52).
    std::atomic<bool> mk_wheel{true};
    /// Mouse turns the camera directly (1:1, no stick deadzone/curve; window.cpp, bloodborne_cam)
    std::atomic<bool> mk_direct_camera{false};
    /// Button prompts (bbport.ini prompt_style, BB_PROMPT_STYLE, menus; live): PromptStyle.
    std::atomic<int> prompt_style{PromptAuto};
    /// Keyboard movement ramp (ini key_ramp, BB_KEY_RAMP; live): the move stick rises over 70 ms and falls over 45 ms.
    std::atomic<bool> key_ramp{true};
    /// The settings menu's position (fraction of the screen), -1 until it is moved.
    std::atomic<float> menu_x{-1.0f}, menu_y{-1.0f};
    // FSR 4 checks (menu): the provider's auto exposure, the jitter sign it is given.
    std::atomic<bool> fsr4_auto_exposure{true};
    std::atomic<bool> fsr4_invert_jitter{false};
    std::atomic<int> active_render_width{1920}, active_render_height{1080};
    /// The scene is larger than the output (render resolution above it): the upscaler does not
    /// run, the scene is scaled down by a copy (RunScaled; menu note, party arrival log).
    std::atomic<bool> upscaler_scene_above_output{false};
    /// Applied at start (patches.py); the menu shows when a restart is needed.
    std::atomic<bool> effects[EffectCount]{};
    std::atomic<int> model_lod{0}; ///< 0 the game's, 1 lower (2 is read as 1, -2 as 0)
    std::atomic<int> texture_quality{TqAuto}; ///< TextureQuality: auto (half on <= 3 GB cards), full, half, quarter
    /// Motion blur resolution (0.6.2's "Volumetric effects"; the game's half-resolution motion blur
    /// buffers; volumetric_resolution.h; idea from a playtester): 0 game default, 1 = 2x the
    /// pixels, 2 = the scene's resolution (ini volumetric_res, BB_VOLUMETRIC_RES). Live.
    std::atomic<int> volumetric_res{0};
    /// Live FPS clock (Mrsuss60's a7cbfae6; bbport_frame_rate_logic.h): the "60 FPS++ (Deltatime)" patch
    /// and a measured-frame-time hook instead of the frame rate hooks / FPS++ patches. live_fps is
    /// read at start (ini live_fps, BB_LIVE_FPS wins); target_fps (30..239, 0 = uncapped) changes live
    /// (ini target_fps; the launcher's BB_FPS / BB_FPS_LIMIT set it at start); max_dt_hz is the
    /// physics step's upper clamp (1/30 s). On by default (owner decision, 0.7.8).
    std::atomic<bool> live_fps{true};
    std::atomic<int> target_fps{60};
    std::atomic<int> max_dt_hz{30};
    std::atomic<int> output_res{OutputDefault}; ///< index into OutputWidths
    /// Live resolution and preset changes (run.sh): 0 off by default (startup patch, fastest
    /// on the Steam Deck and older GPUs), -1 auto (strong discrete GPUs), 1 on. On restart.
    std::atomic<int> live_resolution{0};
    /// Why FSR 4 cannot run (assets, device features), or null. Set by the renderer.
    std::atomic<const char*> fsr4_problem{nullptr};
    std::atomic<bool> fsr4_supported{false}, fsr411_supported{false};
    /// DLSS (gpu/dlss_bridge, NVIDIA RTX) is ready, or why not (null before the device exists).
    std::atomic<bool> dlss_supported{false};
    std::atomic<const char*> dlss_problem{nullptr};
    /// Graphics pipelines compiled on worker threads while their first draws are skipped
    /// (BB_ASYNC_SHADERS, alias BB_ASYNC_PIPELINES; ini key async_pipelines). On by default
    /// since 0.5-pre4. Live.
    std::atomic<bool> async_shaders{true};
    /// Graphics pipelines linked from VK_EXT_graphics_pipeline_library parts (BB_GPL). On restart.
    std::atomic<bool> gpl{false};
    /// Pixel shaders write 0 for a NaN / Inf colour (stops one bad pixel being spread by bloom into
    /// a white frame; BB_NAN_GUARD, ini nan_guard). Off by default. On restart (shaders rebuilt).
    std::atomic<bool> nan_guard{false};
    /// Diagnostic: guarded pixels get a magenta marker instead of 0 (BB_NAN_GUARD_MARK, ini
    /// nan_guard_mark; launcher Advanced / Debug). Needs nan_guard. On restart.
    std::atomic<bool> nan_guard_mark{false};
    /// "Compiling shaders: N%" in the bottom right corner (BB_COMPILE_INDICATOR). Live.
    std::atomic<bool> compile_indicator{true};
    /// Party mode: short join / leave / travel notices in a top corner (ini party_toasts). Live.
    std::atomic<bool> party_toasts{true};
    /// Party mode: the party HUD, companions' names / health / whereabouts in the bottom left
    /// corner (party_hud.h; ini party_hud). Never shown solo. Live.
    std::atomic<bool> party_hud{true};
    /// Hateful member names (slurs, hate codes; party/name_filter.h) are shown as "Hunter 3F2" on this PC: the party
    /// list, name tags, notices. Display only: the party itself still sees the real name. ini party_hide_bad_names,
    /// env BB_PARTY_HIDE_BAD_NAMES. Live. ON by default (safety), unlike other new party features.
    std::atomic<bool> party_hide_bad_names{true};
    /// Party mode: guests may use world lamps and Dream headstones too; their use moves the whole
    /// party, host-led (ini party_guest_lamps; env BB_PARTY_GUEST_LAMPS overrides). On restart.
    std::atomic<bool> party_guest_lamps{true};
    /// Party rules (party_balance.h; the host's values are the party's, sent to every guest. Live).
    /// C4: the death debuff "rot": each death stacks one of five penalties (5 stacks), a lamp rest
    /// cleanses it (ini party_rot; env BB_PARTY_ROT=0/1 overrides at start). Off by default (user
    /// decision 2026-10-22; on 0.6.0 - 0.7.1): only an unset key is off, a saved value stays.
    std::atomic<bool> party_rot{false};
    /// bbport.ini settings_rev: the one-time changes of saved settings done so far (kSettingsRev),
    /// shared with the launcher (launcher.py migrate_ini_rev; whichever starts first does them).
    std::atomic<int> settings_rev{0};
    /// D7: enemy scaling per extra hunter (coop::balance::Rules): HP / poise in % of Bloodborne's
    /// boss step (100 = vanilla), damage + % (ini party_boss_hp ... party_enemy_poise).
    std::atomic<int> party_boss_hp{150}, party_boss_damage{10}, party_boss_poise{100};
    std::atomic<int> party_enemy_hp{50}, party_enemy_damage{0}, party_enemy_poise{0};
    /// E2: what is shown above party members (coop::balance::Overhead: 0 off, 1 name, 2 name +
    /// level, 3 name + health bar, 4 name + deaths; ini party_overhead). Live, local.
    std::atomic<int> party_overhead{3};
    /// Party name tags: on (default) = in a party our name plate replaces the game's phantom plate
    /// (which hides behind any occluder); off = the game's own plate as in vanilla, ours not drawn
    /// (ini party_name_tags; env BB_PARTY_HUD_GAME_PLATE=1 keeps the game's plate for tests). Live.
    std::atomic<bool> party_name_tags{true};
    /// Companions drawn as plain hunters: on (default) = the other players' characters on this
    /// screen without the game's blue, glowing phantom look; off = that look as in vanilla
    /// (ini party_glow_off; env BB_PARTY_GLOW_OFF=0/1 overrides). Display only: nothing about the
    /// game's cooperator state changes. Live, local.
    std::atomic<bool> party_glow_off{true};
    /// D8: a party member's NPC conversation (subtitles, voice, choices, gifts) is shared with the
    /// members near it (ini party_npc_talk; env BB_PARTY_NPC_TALK overrides). Live, local.
    std::atomic<bool> party_npc_talk{true};
    /// I2: pings - hold the touchpad and press R3 (or V) to mark the spot looked at for the party;
    /// everyone sees it through walls with its distance for 10 s (ini party_pings; env
    /// BB_PARTY_PINGS overrides). Live, local: off = no combo, no sending, others' pings not drawn.
    std::atomic<bool> party_pings{true};
    /// Party text chat (0.7.8, party_chat.h): the chat key (T) or touchpad + L1 opens a box / preset list; lines reach every
    /// member in a small fading log in the sender's colour. Off by default: off = no key, no sending, others' lines
    /// ignored (ini party_chat; env BB_PARTY_CHAT overrides). Live, local.
    std::atomic<bool> party_chat{false};
    /// Chat on: show the lines of the other members (ini party_chat_receive, default on); off = they are dropped on arrival.
    std::atomic<bool> party_chat_receive{true};
    /// Overhead name tags in each member's party colour (the party panel's / chat's MemberColour) with a strong outline,
    /// instead of the plate's blue (ini party_name_colours; off by default, 0.7.8). Live, local.
    std::atomic<bool> party_name_colours{false};
    /// H5 look sync: a change of this player's appearance data (any editor, not only the Dream mirror's close)
    /// re-sends the player record and the peers rebuild this hunter's chr (ini party_look_sync; env
    /// BB_PARTY_LOOK_SYNC=0/1 over it; off by default, 0.8.0). Live, local.
    std::atomic<bool> party_look_sync{false};
    /// 0.8.1 seamless join A: at the host's party lamp travel this guest loads the same lamp at once, in parallel
    /// with the host, behind one loading screen (party_seam.cpp; ini party_parallel_follow; env
    /// BB_PARTY_PARALLEL_FOLLOW=0/1 over it; off by default). Live, local (a guest's own load).
    std::atomic<bool> party_parallel_follow{false};
    /// 0.8.1 seamless join B: in a party, the loading screen stays up after a load until the frames ran under 50 ms
    /// for 1 s without a break (at most 3 s), and the drop runs at the frame end's quiet point (party_seam.cpp; ini
    /// party_settled_drop; env BB_PARTY_SETTLED_DROP=0/1 over it; off by default). Live, local.
    std::atomic<bool> party_settled_drop{false};
    /// 0.8.1 seamless join C: a guest's follow load writes the spot beside the host into the summoned placement, so
    /// the load itself puts it there (no warp after the screen drops unless it landed elsewhere) (ini
    /// party_load_beside; env BB_PARTY_LOAD_BESIDE=0/1 over it; off by default). Live, local.
    std::atomic<bool> party_load_beside{false};
    /// 0.8.1 seamless join D: Party > Rejoin pressed while in the host's world reloads beside the host with one loading
    /// screen (no Leave, no trip home) (ini party_quick_rejoin; env BB_PARTY_QUICK_REJOIN=0/1 over it; off by
    /// default). Live, local.
    std::atomic<bool> party_quick_rejoin{false};
    /// 0.8.1 (CL-4a20f7): over the Nightwatch VPN, allow a direct connection to a party member who allows it too, used
    /// only when clearly faster than the tunnel (party/party_nwdirect.h; ini party_nw_direct; env BB_PARTY_NW_DIRECT=0/1
    /// over it; off by default). The peer sees your public IP. Read when the party link comes up.
    std::atomic<bool> party_nw_direct{false};
    /// I7/E3 map markers: a spot a player puts on the in-game map (right click / R3 / P) is seen on every party
    /// member's map (party_ping.h; ini party_map_marks; env BB_PARTY_MAP_MARKS=0/1 over it; off by default, 0.8.0). Live.
    std::atomic<bool> party_map_marks{false};
    /// I2: every party member on this map drawn through walls as a small marker with its distance
    /// in its colour (ini party_through_walls; off by default: busy next to the name plates). Live.
    std::atomic<bool> party_through_walls{false};
    /// The marker on this player's own lost-echoes bloodstain in the host's world ("Blood Echoes N"
    /// with the distance, through walls; party_ping StainMark). Off by default (user decision
    /// 2026-10-20); the bloodstain and its echoes are there either way (ini party_stain_marker; env
    /// BB_PARTY_STAIN_MARKER overrides). Live, local.
    std::atomic<bool> party_stain_marker{false};
    /// The in-game map (Hunter's Atlas by MikeTat1988; shim/map/map_view.h): M, or hold L1 + R1 + Start,
    /// opens it. Off by default (ini map_enabled; env BB_MAP=0/1 overrides). Needs the map data the
    /// launcher makes from the player's game files (Prepare the map). Live, local.
    std::atomic<bool> map_enabled{false};
    /// Auto-hide HUD (0.7.8, CL-7d51fe; party/auto_hud.h): the game's HP / stamina bars, item slots and the echoes /
    /// Insight counters fade out while nothing happens and come back on damage, stamina use, a fight, a lock-on, an
    /// equipment change or a menu, like Elden Ring's "HUD: Auto". Off by default (ini auto_hide_hud; env BB_AUTO_HUD=0/1). Live.
    std::atomic<bool> auto_hide_hud{false};
    /// The party HUD board shows each member's round-trip time in ms after the name, green / yellow / red
    /// by quality (ini party_hud_ping, default on). Live, local.
    std::atomic<bool> party_hud_ping{true};
    /// I1: friendly fire / PvP - party members can hurt each other (party_pvp.h). A party rule: the
    /// host's value is sent to every guest, a guest's own value never counts. Off by default (ini
    /// party_friendly_fire; env BB_PARTY_FRIENDLY_FIRE=0/1 overrides at start). Live.
    std::atomic<bool> party_friendly_fire{false};
    /// Statsaudit finding 5: lamps are shared by the whole party (a lamp any member lit is lit for
    /// everyone, in each member's own save too). A party rule: the host's value counts. On by
    /// default (ini party_share_lamps; env BB_PARTY_SHARE_LAMPS=0/1 overrides at start).
    std::atomic<bool> party_share_lamps{true};
    /// 0.7.2 (user decision 2026-10-21: "host state saves lamps, anybody can light them"): a guest's
    /// lamps from its own world light up in the host's world when it joins (needs Shared lamps). A
    /// party rule: the host's value counts. Off by default (ini party_guests_bring_lamps; env
    /// BB_PARTY_GUESTS_BRING_LAMPS=0/1 overrides at start).
    std::atomic<bool> party_guests_bring_lamps{false};
    /// ... within this many metres of the NPC (0 = anywhere on the same map; ini
    /// party_npc_talk_range; env BB_PARTY_NPC_TALK_RANGE overrides). Live, local.
    std::atomic<int> party_npc_talk_range{25};
    /// F2: the automatic hole-punching rendezvous over the public BitTorrent DHT (party_rdv.h):
    /// OPT-IN, off by default (it posts the guest's IP on the DHT; user decision 2026-10-14). Both
    /// host and guest need it on. ini party_rendezvous; env BB_PARTY_RENDEZVOUS (the launcher's
    /// Party tab) overrides at start. Read when the party starts (System > Party members).
    std::atomic<bool> party_rendezvous{false};
    /// F2 (0.7.1, network_robustness.md 4a): relay for a party member that cannot reach the host
    /// (and use such a member when we cannot): party members only, capped. On by default. ini
    /// party_member_relay; env BB_PARTY_MEMBER_RELAY overrides at start. Serving follows it live.
    std::atomic<bool> party_member_relay{true};
    /// F2 (0.7.1): IPv6 for the party link and the game's UDP when both PCs have a global IPv6
    /// address (tried before any relay). OFF by default (user decision 0.7.1: the party code would
    /// carry the PC's IPv6 address). ini party_ipv6; env BB_PARTY_IPV6 overrides at start. Read when
    /// the party starts.
    std::atomic<bool> party_ipv6{false};
    /// 0.7.5: host over a VPN (Radmin, Hamachi, ZeroTier, Tailscale): 0 auto (a VPN adapter that is
    /// up, else the usual address), 1 off, 2 a named adapter (party_vpn_name). ini party_vpn =
    /// auto|off|<adapter name>; env BB_PARTY_VPN overrides. The host's choice; read when the game
    /// starts (the address is chosen once). The name itself lives in bbnet (set_vpn_preference).
    std::atomic<int> party_vpn{0};
    /// 0.7.5: party discovery (net/party_discovery.h): the host announces itself on the VPN and LAN adapters
    /// only; the launcher's Join page lists what it hears. ini party_discovery, env BB_PARTY_DISCOVERY. Live.
    std::atomic<bool> party_discovery{true};
    /// 0.7.7: Nightwatch VPN lobby (net/party_vpnlobby.h): a host with a password on the Nightwatch VPN (a 10.8.0.x
    /// address) announces its character name, slots and version to the lobby on the VPN server, so VPN players
    /// see the game in the launcher's Join list. ini party_vpn_lobby, env BB_PARTY_VPN_LOBBY. Live. Off by default.
    std::atomic<bool> party_vpn_lobby{false};
    /// The open party (party/party_open_rules.h): a host with no password also takes plain-address joins from its LAN
    /// and VPN subnets. ini party_open, env BB_PARTY_OPEN. Live. Off by default, so hosting with no password stays
    /// code only until the player asks for it.
    std::atomic<bool> party_open{false};
    /// 0.7.9: "Any level can join my open game" (party_invade_rules.h OpenLevelOk): the HOST skips the vanilla co-op level range
    /// for an open public join (no password, plain address), at the gate and in the later recheck. Invaders keep their range;
    /// a password or party code never had one. ini party_open_any_level, env BB_PARTY_OPEN_ANY_LEVEL. Live. Off by default.
    std::atomic<bool> party_open_any_level{false};
    /// 0.7.8 (CL-4cc63c): "Allow invaders" (party/party_invade_rules.h, docs/party/invasions.md): the HOST lets other players
    /// join its world as hostile phantoms from the public lists (VPN lobby / LAN discovery). Only the host's value counts. ini
    /// party_invaders, env BB_PARTY_INVADERS. Live. Off by default.
    std::atomic<bool> party_invaders{false};
    /// 0.7.9 (CL-7e748c): "Max invaders": how many invaders the host lets in at once, 1 or 2 (party_invade_rules.h). The HOST's
    /// value only; both count against Max players and the game's four slots. ini party_max_invaders, env
    /// BB_PARTY_MAX_INVADERS. Live. Default 1 (the 0.7.8 behaviour).
    std::atomic<int> party_max_invaders{1};
    /// 0.7.7 (CL-2eec68): splitscreen mode for several windows on one screen (party/party_splitscreen_rules.h): the party
    /// HUD shows only the compass, no name tags, party messages cut to errors. Each player's own. ini
    /// party_splitscreen, env BB_PARTY_SPLITSCREEN. Live. Off by default.
    std::atomic<bool> party_splitscreen{false};
    /// 0.7.7 (CL-adeb50): "Stay in my world" (party/party_stayhome_rules.h): a guest pauses the automatic joining (no bell,
    /// no gather, not asked to travel or vote) and stays in its own world, still in the party. Per player (guest).
    /// ini party_stay, env BB_PARTY_STAY. Live. Off by default.
    std::atomic<bool> party_stay{false};
    /// 0.7.7: which party messages show (0 All, 1 Errors only, 2 Off; coop::splitscreen::Messages). Splitscreen forces
    /// All to Errors only. Per player. ini party_messages = all|errors|off, env BB_PARTY_MESSAGES. Live.
    std::atomic<int> party_messages{0};
    /// 0.7.6: connection warnings (party/party_connwarn.h): an on-screen notice when the firewall, the router or a
    /// wrong address keeps the party from working. Per player. ini party_conn_warnings. Live.
    std::atomic<bool> party_conn_warnings{true};
    /// 0.7.7: the notice "<item>: shared with the party" on the screen when a shared item (key / unique item, weapon,
    /// rune) is given (party/party_item_notice_rules.h). Per player; shows only while the party notices
    /// (party_toasts) are on. ini party_item_notices. Live.
    std::atomic<bool> party_item_notices{true};
    /// 0.7.8: give items to a party member (party/party_give_rules.h): the Insert menu's Party tab lists the own
    /// consumables and sends one to a member through a two-phase transfer. The giver's switch shows the list, the
    /// receiver's is its consent. Per player. OFF by default. ini party_give_items; env BB_PARTY_GIVE_ITEMS=1 wins.
    std::atomic<bool> party_give_items{false};
    /// 0.7.9 (givegear079, CL-1b3b98): the same for weapons and armor (a weapon moves with its upgrade level and blood gems).
    /// Its own switch, separate from party_give_items: gear is worth more than a stack of bullets, so a hunter can take
    /// consumables and still refuse gear. The giver's switch adds Give to a weapon's / armor's menu, the receiver's is its
    /// consent. Per player. OFF by default. ini party_give_gear; env BB_PARTY_GIVE_GEAR=1 wins.
    std::atomic<bool> party_give_gear{false};
    /// Party gameplay switches (the launcher's Party Rules / My Party tabs, the game's party pages in
    /// System; PartySwitches below): ini key = the member's name, env BB_PARTY_* = 0/1 wins at start.
    /// On by default. Load puts each value into the process environment when its variable is unset,
    /// so the party modules that read their switch once at start see the ini's value (the rows of
    /// those say "next start"); the revive switches are read live from here (party_b2).
    /// The host's own (its death; shown read-only to guests through party_balance's rules):
    std::atomic<bool> party_host_revive{true};       ///< the host gets up at its lamp without a load
    std::atomic<bool> party_host_limbo_revive{true}; ///< the host gets up in the arena after a boss kill
    /// 0.7.2, what else cleanses rot (party_rot_rules.h; only while rot is on; read live):
    std::atomic<bool> party_rot_cleanse_boss{true};   ///< a boss's death: all of it, for the whole party
    std::atomic<bool> party_rot_cleanse_killer{true}; ///< your killer's death: that death's stack
    std::atomic<bool> party_rot_cleanse_echoes{true}; ///< your bloodstain picked up: its death's stack
    std::atomic<bool> party_rot_extra{false};         ///< 0.8.0 (C4): the rot may also take stamina recovery (host's rule)
    /// Each player's own (applies while it is a guest, or to its own world):
    std::atomic<bool> party_guest_revive_inplace{true}; ///< a guest gets up at the lamp without a load
    std::atomic<bool> party_guest_respawn{true};  ///< a dead guest comes back at the host's lamp
    std::atomic<bool> party_guest_refill{true};   ///< vials / bullets refilled with the host's
    std::atomic<bool> party_chalice_rejoin{true}; ///< a guest stays with / comes back to the host's Chalice Dungeon
    std::atomic<bool> party_guest_mark{true};     ///< guests can use the Hunter's Mark
    std::atomic<bool> party_full_rewards{true};   ///< full echoes and drops for guests
    std::atomic<bool> party_items{true};          ///< the host's item rewards given to guests
    std::atomic<bool> party_grant_bells{true};    ///< the bells after the prologue
    std::atomic<bool> party_open_world{true};     ///< no co-op area walls
    std::atomic<bool> party_progress{true};       ///< story progress copied into guests' saves
    std::atomic<bool> party_progress_npc{true};   ///< ... with NPC quest choices
    std::atomic<bool> party_story{true};          ///< cutscenes, time of day, endings shared
    std::atomic<bool> party_story_mirror{true};   ///< ... played live with the host
    /// 0.7.1 (user rule: every party gameplay switch in the menu, the launcher and the ini). Not put
    /// into the environment (PartySwitch::env_export false): read live from here; an explicit env
    /// variable still wins at start. The host's (party_balance Rules; a guest follows the host's):
    std::atomic<bool> party_kill_record{true};  ///< enemies stay dead for the party (party_kill_record.h)
    std::atomic<bool> party_npc_mirror{true};   ///< an NPC gone from the host's world is gone on guests
    std::atomic<bool> party_gather{true};       ///< I5: the host may gather the party (party_gather.h)
    /// 0.7.7 (CL-a3a9e6): guests may gather the party too (party_gather.h, a guest asks the host, which asks the others).
    /// The host's rule, sent to the guests; OFF by default.
    std::atomic<bool> party_guest_gather{false};
    /// 0.7.7 (CL-48ee07): a Chalice Dungeon lamp lists the dungeon's other lit lamps (layers) and the host's choice
    /// takes the whole party there (lamp_travel.h). The host's rule, sent to the guests; OFF by default.
    std::atomic<bool> party_layer_travel{false};
    /// 0.7.8 (CL-420d9f): the Ritual Altar's "Search by Chalice Glyph" shows the party's glyphs as a pick list before the
    /// code keyboard (party_chalice_glyphs.h). This player's own, read live; OFF by default.
    std::atomic<bool> party_glyph_list{false};
    /// 0.7.9 (CL-13f7d1): a summoned guest can use the Ritual Altar in the host's Dream (conduct a ritual with its own
    /// chalice and materials, enter its dungeons). This player's own, read live; OFF by default.
    std::atomic<bool> party_guest_altar{false};
    /// 0.7.9 (gdrf079): a host whose game would close its empty room (~5 s after the last member left) keeps it open
    /// while a title join (a restarted guest) is being served. The host's own, read live; ON by default (a bug fix).
    std::atomic<bool> party_title_room_hold{true};
    /// 0.7.3 (user decision 2026-10-22): covenant outcomes (Annalise, Alfred, Valtr) stay in each
    /// player's own save (party_progress Category::covenant_outcome). The host's; on by default.
    std::atomic<bool> party_covenant_per_player{true};
    /// 0.7.3: a party travel by lamp, headstone or Hunter's Mark runs only once every member voted
    /// YES (party_vote.h). The host's rule; OFF by default.
    std::atomic<bool> party_travel_vote{false};
    /// 0.7.5: Party starts = skip the prologue (party_start.h StartMode::SkipPrologue): a NEW character
    /// begins as after the prologue. The host's rule; OFF by default.
    std::atomic<bool> party_skip_prologue{false};
    /// 0.7.7 (bosstarget): bosses share their attention between the players in reach instead of keeping one
    /// target (party_enemyai_rules.h PickAttention). Acts on the AI owner's game (the host). On by default.
    std::atomic<bool> party_boss_share{true};
    std::atomic<bool> party_npc_fights{true};   ///< D11 (0.7.3): NPC hunters fight the whole party (party_npcfight_rules.h)
    std::atomic<bool> party_any_kidnap{true};   ///< 0.7.3: a Snatcher grabbing a guest takes the party to the Gaol (party_kidnap_rules.h)
    /// 0.7.5 (levelsync, party_levelsync_rules.h): the host's rules, both ON = Bloodborne's own co-op.
    /// Level sync: a hunter above the host's level is scaled down (max HP, stamina, defence, resistances)
    /// as in password co-op; phantom health: a hunter joining a party has 30 % less max HP.
    std::atomic<bool> party_level_sync{true};
    std::atomic<bool> party_phantom_hp{true};
    /// 0.7.6 (pvphp): the host's rule "Full health in PvP": no phantom health cut while friendly fire is on.
    std::atomic<bool> party_pvp_full_hp{true};
    /// 0.7.6 (bossdrops): the host's rule: a world boss's item drop also goes to the guests present at the kill.
    std::atomic<bool> party_boss_drops{true};
    /// Each player's own (while it is a guest; party_items):
    std::atomic<bool> party_key_view{true};     ///< the own key items while summoned (next start)
    std::atomic<bool> party_key_items_summoned{true}; ///< a host-world key item given at once (needs key_view)
    /// Insight a guest gets for a boss the host kills (party_phantom InsightMode): 0 same as the
    /// host (parity), 1 full bonus, 2 the game's own +1 (ini party_guest_insight parity|full|0; env
    /// BB_PARTY_GUEST_INSIGHT). Next start.
    std::atomic<int> party_guest_insight{0};

    /// Native HDR output (bbport_hdr.h, docs/party/ERSC_PARITY.md I4): an HDR10 swapchain with the
    /// SDR picture expanded by an inverse tone curve. Off by default (ini hdr, BB_HDR). On restart.
    std::atomic<bool> hdr{false};
    /// Peak brightness in cd/m2; 0 = the display's reported maximum, else 1000 (ini
    /// hdr_peak_nits, BB_HDR_PEAK). Live.
    std::atomic<int> hdr_peak_nits{0};
    /// Paper white: where the SDR picture's mid-tones and the port's overlay sit, cd/m2 (ini
    /// hdr_paper_white, BB_HDR_PAPER_WHITE). Live.
    std::atomic<int> hdr_paper_white{200};
    /// Highlight strength: 0 = the SDR picture at paper white, 1 = SDR white reaches the peak,
    /// up to 1.5 (more highlights at the peak); saturation 0.5 .. 1.5, 1 = the game's (ini
    /// hdr_strength / hdr_saturation, BB_HDR_STRENGTH / BB_HDR_SATURATION). Live.
    std::atomic<float> hdr_strength{1.0f};
    std::atomic<float> hdr_saturation{1.0f};
    /// The calibration pattern over the top of the picture (System > HDR; BB_HDR_PATTERN=1;
    /// never saved). Live.
    std::atomic<bool> hdr_calibration{false};
    /// Set by the renderer: the swapchain's encoding this session (BbHdr::Encoding: 0 SDR, 1
    /// HDR10 PQ, 2 scRGB), the peak the display reports (cd/m2, 0 unknown), and why HDR was asked
    /// for but does not run (null otherwise).
    std::atomic<int> hdr_active{0};
    std::atomic<float> hdr_display_peak{0.0f};
    std::atomic<const char*> hdr_problem{nullptr};

    /// Startup settings for the explicit BB_RENDER_RES compatibility patch only.
    int startup_preset = NativeAA;
    int startup_upscaler = UpscalerFsr3;
    bool startup_object_motion = true;
    bool startup_effects[EffectCount]{};
    int startup_model_lod = 0;
    int startup_texture_quality = TqAuto;
    int startup_output_res = OutputDefault;
    int startup_live_resolution = 0;
    bool startup_gpl = false;
    bool startup_nan_guard = false;
    bool startup_nan_guard_mark = false;
    int startup_frame_generation = 0;
    int startup_fg_backend = FgDlss;
    int startup_reflex = ReflexAuto;
    int startup_anti_lag = AntiLagAuto;
    bool startup_vrr = false;
    bool startup_hdr = false;
};

/// The party gameplay switches (Values::party_host_revive ... party_story_mirror): ini key, the
/// environment variable, the member.
struct PartySwitch {
    const char* key;
    const char* env;
    std::atomic<bool> Values::*member;
    /// Load puts the ini's value into `env` when it is unset (for modules reading it at start);
    /// false: the module reads the member, only an explicit variable is taken (never exported).
    bool env_export = true;
};
extern const PartySwitch PartySwitches[];
/// bbport.ini settings_rev (the launcher's INI_SETTINGS_REV is the same number):
///   1 (0.7.2): party_rot = 0 once. Rot became off by default, and 0.6.0 - 0.7.1 games wrote their
///              default party_rot=1 at every settings save, chosen or not (coordinator decision
///              2026-10-22: turn it off once; a later explicit 1 stays).
constexpr int kSettingsRev = 1;
extern const int PartySwitchCount;
/// party_guest_insight's ini / env values by index (parity, full, 0).
const char* GuestInsightName(int mode);

Values& Get();
/// Localized overlay text; the Language selector itself stays in English.
const char* MenuText(const char* english, const char* russian);

/// Reads the file, then the environment overrides. Called once at start.
void Load();
/// Checks the loaded choice before the first frame; unsupported FSR 4 uses FSR 3.1.
void ConfigureUpscalerSupport(bool fsr4, bool fsr411);
/// After device creation: DLSS availability; a DLSS setting falls back to FSR 3.1 without it.
void ConfigureDlssSupport(bool available, const char* problem);
/// Startup-patched scene dimensions cannot change until run.sh prepares a new image.
bool FixedRenderSession();
int RenderPreset();
bool ResolutionNeedsRestart();
/// Frame generation runs this session (chosen at start; the provider may still be unavailable).
inline bool FrameGenerationStarted() { return Get().startup_frame_generation >= 2; }
/// The menus' single frame generation choice: 0 off, 1-3 DLSS 2x-4x, 4 FSR 2x.
inline constexpr int FrameGenerationChoices = 5;
inline int FrameGenerationChoice(int factor, int backend) {
    if (factor < 2) return 0;
    return backend == FgFsr ? 4 : (factor > 4 ? 4 : factor) - 1;
}
inline void FrameGenerationFromChoice(int choice, int& factor, int& backend) {
    backend = choice == 4 ? FgFsr : FgDlss;
    factor = choice == 4 ? 2 : choice >= 1 && choice <= 3 ? choice + 1 : 0;
}
/// A setting applied only at start (game patches, model detail, motion vectors, live resolution,
/// GPL, the startup render size) differs from what this run started with.
bool RestartNeeded();
/// Writes the file (menu changes).
void Save();
/// The file's lines whose key starts with `prefix` (key, value), in file order.
std::vector<std::pair<std::string, std::string>> ReadLines(const std::string& prefix);
/// The in-game menu's key binding rows: sets (a value) or removes (none) key.* / mkkey.* lines,
/// keeping the rest of the file, and bumps BindingsGeneration so runtime_pad.c reads them again.
void WriteBindingLines(const std::vector<std::pair<std::string, std::optional<std::string>>>& lines);
int BindingsGeneration();
/// Window thread, about once a second: when the file changed on disk since it was last read or
/// written (the launcher's mouse & keyboard dialog while the game runs), re-reads the mouse
/// tuning keys (mk_sens_*, mk_invert_*, mk_deadzone, mk_smoothing, mk_wheel) and prompt_style only.
void ReloadMouseTuning();

/// The peak brightness the HDR curve uses now (setting, else the display's, else 1000).
float HdrPeakNits();

/// Render resolution divisor of a preset (1.0 native, 1.5 quality, ...).
float PresetScale(int preset);
const char* PresetName(int preset);
const char* UpscalerName(int upscaler);

} // namespace BbSettings
