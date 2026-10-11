// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_game_menu_pages.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

#include "bbport_settings.h"
#include "party/party_balance_rules.h"
#include "party/party_link.h"  // kMaxPartyPlayers
#include "party/party_status.h"

namespace BbGameMenu::Pages {
namespace {

// ---- Texts ----
struct Dynamic {
    std::size_t limit = MaxValueLength;
    std::array<char16_t, DynamicCapacity> buffer[2]{}; ///< English, Russian
};
struct TextEntry {
    std::u16string english, russian;
    int dynamic = -1; ///< index into `dynamics`, or -1
};
std::vector<TextEntry> texts;
std::vector<std::unique_ptr<Dynamic>> dynamics; // unique_ptr: buffers never move

u32 AddText(const char* english, const char* russian) {
    texts.push_back({Utf16(english), Utf16(russian), -1});
    return IdBase + u32(texts.size() - 1);
}

u32 AddDynamic(std::size_t limit) {
    auto d = std::make_unique<Dynamic>();
    d->limit = std::min(limit, DynamicCapacity - 1);
    dynamics.push_back(std::move(d));
    texts.push_back({{}, {}, int(dynamics.size() - 1)});
    return IdBase + u32(texts.size() - 1);
}

/// Writes a run-time text in place (the game may hold the pointer): cut to the text's limit
/// with an ellipsis, rewritten only when it changed.
std::atomic<std::uint64_t> dynamic_changes{0};

void SetDynamic(u32 id, const std::string& english, const std::string& russian) {
    Dynamic& d = *dynamics[std::size_t(texts[id - IdBase].dynamic)];
    for (int lang = 0; lang < 2; ++lang) {
        std::u16string text = Utf16(lang ? russian.c_str() : english.c_str());
        if (text.size() > d.limit) {
            text.resize(d.limit - 3);
            if (!text.empty() && text.back() >= 0xd800 && text.back() < 0xdc00) {
                text.pop_back(); // half a surrogate pair
            }
            text += u"..."; // the game's fonts: plain ASCII is safe
        }
        auto& buffer = d.buffer[lang];
        if (std::u16string_view(buffer.data()) == text) {
            continue;
        }
        buffer[text.size()] = 0; // shorter first: a reader never runs past the new end
        std::copy(text.begin(), text.end(), buffer.begin());
        buffer[text.size()] = 0;
        dynamic_changes.fetch_add(1, std::memory_order_release);
    }
}

// ---- Fields: what the rows edit ----
using Values = BbSettings::Values;
struct Field {
    Kind kind = Kind::List;
    int steps = 0; ///< Steps rows: the number of choices
    std::function<int(const Values&)> get;          ///< null: a button (0 when the page opens)
    std::function<void(int)> set;                    ///< applies a setting
    std::function<void(int from, int to, std::int64_t now)> act; ///< a button
    bool debounce = false; ///< applied once the value rests for 0.4 s (re-creates targets)
};
std::vector<Field> fields;
std::vector<int> values, defaults, applied, resting;
std::vector<u8> bytes, default_bytes;
std::vector<std::int64_t> rested_since, armed_at;
std::atomic<bool> loaded{false};
std::atomic<std::int64_t> last_refresh{-1000000};
void (*restart_handler)() = nullptr;
void (*quit_handler)() = nullptr;

Values& DefaultSettings() {
    static Values* v = [] {
        auto* d = new Values;
        for (int e = 0; e < BbSettings::EffectCount; ++e) {
            d->effects[e] = BbSettings::Effects[e].default_on;
        }
        return d;
    }();
    return *v;
}

int AddField(Field f) {
    fields.push_back(std::move(f));
    return int(fields.size() - 1);
}

int Setting(Kind kind, std::function<int(const Values&)> get, std::function<void(int)> set,
            int steps = 0) {
    Field f;
    f.kind = kind;
    f.get = std::move(get);
    f.set = std::move(set);
    f.steps = steps;
    return AddField(std::move(f));
}

int Flag(std::atomic<bool> Values::*member) {
    return Setting(
        Kind::Toggle, [member](const Values& v) { return int((v.*member).load()); },
        [member](int i) { (BbSettings::Get().*member) = i != 0; });
}

/// A 0..10 slider over an 11-entry table of a float setting.
int Scaled(std::atomic<float> Values::*member, const float* table) {
    return Setting(
        Kind::Slider, [member, table](const Values& v) { return int(NearestPosition(table, (v.*member).load())); },
        [member, table](int i) { (BbSettings::Get().*member) = table[std::clamp(i, 0, 10)]; });
}

/// A confirmed button: the first change arms it (its second value text says "press again"),
/// the change back within 10 s runs `fn`.
int Confirmed(std::function<void()> fn) {
    Field f;
    f.kind = Kind::Toggle;
    const int index = int(fields.size());
    f.act = [fn = std::move(fn), index](int from, int to, std::int64_t now) {
        if (from == 0 && to == 1) {
            armed_at[std::size_t(index)] = now;
        } else if (from == 1 && to == 0 && now - armed_at[std::size_t(index)] <= 10000) {
            armed_at[std::size_t(index)] = -1000000;
            fn();
        }
    };
    return AddField(std::move(f));
}

/// A read-only line: a toggle whose two value texts are the same.
int Info() {
    Field f;
    f.kind = Kind::Toggle;
    f.act = [](int, int, std::int64_t) {};
    return AddField(std::move(f));
}

// ---- Pages ----
std::vector<Page> pages;

void AddPage(const char* en, const char* ru, const char* help_en, const char* help_ru) {
    const u32 name = AddText(en, ru);
    pages.push_back({name, AddText(help_en, help_ru), {}});
}
void AddRow(int field, u32 name, u32 help, std::vector<u32> choices, Kind kind,
            std::function<bool()> visible = nullptr) {
    pages.back().rows.push_back({field, name, help, std::move(choices), kind, std::move(visible)});
}
void AddRow(int field, const char* en, const char* ru, const char* help_en, const char* help_ru,
            std::vector<u32> choices, Kind kind) {
    AddRow(field, AddText(en, ru), AddText(help_en, help_ru), std::move(choices), kind);
}

constexpr float SharpnessStep = 0.2f; // Display's slider: 0 off, 1..10 -> 0.2 .. 2.0
constexpr int LodValues[] = {-2, 0, 1, 2};
const float OpacityTable[11] = {0.10f, 0.20f, 0.30f, 0.40f, 0.50f, 0.60f, 0.70f, 0.75f, 0.80f, 0.90f, 1.00f};
const float ScaleTable[11] = {0.50f, 0.60f, 0.70f, 0.80f, 0.90f, 1.00f, 1.20f, 1.40f, 1.60f, 1.80f, 2.00f};
const float SensTable[11] = {0.25f, 0.40f, 0.50f, 0.60f, 0.80f, 1.00f, 1.25f, 1.50f, 2.00f, 3.00f, 5.00f};
const float SmoothTable[11] = {0.0f, 0.05f, 0.10f, 0.15f, 0.20f, 0.30f, 0.40f, 0.50f, 0.60f, 0.70f, 0.80f};
const float DeadTable[11] = {0.0f, 0.01f, 0.02f, 0.03f, 0.04f, 0.05f, 0.06f, 0.08f, 0.10f, 0.15f, 0.20f};
const float MaskScaleTable[11] = {0.0f, 0.25f, 0.50f, 0.75f, 1.00f, 1.25f, 1.50f, 2.00f, 2.50f, 3.00f, 4.00f};
const float TenthsTable[11] = {0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f};

bool UpscalerAvailable(int i) {
    const auto& s = BbSettings::Get();
    return i == BbSettings::UpscalerFsr4     ? s.fsr4_supported.load()
           : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
           : i == BbSettings::UpscalerDlss   ? s.dlss_supported.load()
                                             : true;
}

int upscaler_field = -1;

// ---- Party (party_status.h): the run-time texts and the members the rows stand for ----
constexpr int MaxMembers = party::kMaxPartyPlayers; // the host and the guests (party_link.h)
struct MemberTexts {
    u32 member_name[MaxMembers], member_value[MaxMembers], member_confirm[MaxMembers],
        member_help[MaxMembers];
    u32 restart_help;
    u32 event_value, event_help;
} party;
struct BalanceTexts {
    u32 rot_help = 0;
    u32 scale_help[6]{};
} balance;
std::string member_names[MaxMembers]; // the member row k stands for (kick)
bool member_kickable[MaxMembers]{};
int member_rows = 1;                  // rows the Members page shows (one when nobody is there)

/// The Party page: its action table (built-in rows and AddPartyRow's), each with its run-time
/// texts and the field its game row edits.
struct PartyEntry {
    PartyRow spec;
    u32 name = 0, help = 0, value = 0, pressed = 0;
    int field = -1;
};
std::vector<PartyRow> pending_party_rows; // AddPartyRow before Define
std::vector<PartyEntry> party_rows;       // sorted by order, fixed after Define
std::vector<char> party_row_visible;      // per entry, at the last refresh

const char* L(bool ru, const char* en, const char* ru_text) {
    return ru ? ru_text : en;
}

std::string StateShort(party::status::State st, bool ru) {
    using party::status::State;
    switch (st) {
    case State::Off: return L(ru, "Not running", "Не запущена");
    case State::Starting: return L(ru, "Starting", "Запуск");
    case State::Hosting: return L(ru, "Waiting for hunters", "Ждём охотников");
    case State::Connecting: return L(ru, "Connecting", "Подключение");
    case State::WaitingForWorld: return L(ru, "Loading the world", "Загрузка мира");
    case State::RingingBell: return L(ru, "Ringing the bell", "Звон колокола");
    case State::Joined: return L(ru, "Together", "Вместе");
    case State::Travelling: return L(ru, "Travelling", "Переход");
    case State::Reconnecting: return L(ru, "Reconnecting", "Переподключение");
    case State::Error: return L(ru, "Error", "Ошибка");
    }
    return "";
}

std::string StateLong(party::status::State st, party::status::Role role, bool ru) {
    using party::status::State;
    switch (st) {
    case State::Hosting: return L(ru, "Hosting: waiting for hunters", "Хост: ожидание охотников");
    case State::Connecting: return L(ru, "Connecting to the host", "Подключение к хосту");
    case State::WaitingForWorld: return L(ru, "Waiting for the world to load", "Ожидание загрузки мира");
    case State::RingingBell:
        return role == party::status::Role::Host
                   ? L(ru, "Ringing the Beckoning Bell", "Звон манящего колокола")
                   : L(ru, "Ringing the Small Resonant Bell", "Звон малого резонирующего колокола");
    case State::Travelling: return L(ru, "Travelling with the party", "Переход вместе с группой");
    case State::Reconnecting: return L(ru, "Connection lost: reconnecting", "Связь потеряна: переподключение");
    default: return StateShort(st, ru);
    }
}

void RefreshParty() {
    namespace ps = party::status;
    const ps::Board b = ps::Snapshot();
    const bool on = b.enabled;
    const bool host = b.role == ps::Role::Host;
    std::string en, ru;
    const auto both = [&](const auto& make) {
        en = make(false);
        ru = make(true);
    };
    for (std::size_t i = 0; i < party_rows.size(); ++i) {
        PartyEntry& e = party_rows[i];
        const PartyRow& r = e.spec;
        const Texts name = r.name ? r.name(b) : Texts{};
        const Texts help = r.help ? r.help(b) : Texts{};
        const Texts value = r.value ? r.value(b) : Texts{"-", "-"};
        const Texts pressed = r.pressed ? r.pressed(b) : value;
        SetDynamic(e.name, name.en, name.ru);
        SetDynamic(e.help, help.en, help.ru);
        SetDynamic(e.value, value.en, value.ru);
        SetDynamic(e.pressed, pressed.en, pressed.ru);
        party_row_visible[i] = !r.visible || r.visible(b);
    }

    // Last event (Party members page).
    both([&](bool r) { return b.last_event.empty() ? std::string(L(r, "None", "Нет")) : b.last_event; });
    SetDynamic(party.event_value, en, ru);
    both([&](bool r) {
        return b.last_event.empty() ? std::string(L(r, "No party events yet", "Событий группы ещё не было"))
                                    : std::string(L(r, "Last: ", "Последнее: ")) + b.last_event;
    });
    SetDynamic(party.event_help, en, ru);

    // Members.
    member_rows = std::clamp(int(b.members.size()), 1, MaxMembers);
    for (int k = 0; k < MaxMembers; ++k) {
        if (k >= int(b.members.size())) {
            member_names[k].clear();
            member_kickable[k] = false;
            both([&](bool r) { return std::string(k == 0 ? L(r, "No one yet", "Пока никого") : "-"); });
            SetDynamic(party.member_name[k], en, ru);
            SetDynamic(party.member_value[k], "-", "-");
            SetDynamic(party.member_confirm[k], "-", "-");
            both([&](bool r) {
                return std::string(on ? L(r, "Hunters appear here once they connect",
                                          "Охотники появятся здесь после подключения")
                                      : L(r, "Party mode is off", "Режим группы выключен"));
            });
            SetDynamic(party.member_help[k], en, ru);
            continue;
        }
        const ps::Member& m = b.members[std::size_t(k)];
        member_names[k] = m.name;
        member_kickable[k] = host && !m.local && m.slot != 0;
        both([&](bool r) {
            std::string text = m.name;
            if (m.slot == 0) text += L(r, " (host)", " (хост)");
            if (m.local) text += L(r, " (you)", " (вы)");
            return text;
        });
        SetDynamic(party.member_name[k], en, ru);
        const auto where = [&](bool r) {
            std::string text = !m.connected  ? L(r, "Connection lost", "Связь потеряна")
                               : m.slot == 0 ? L(r, "Host's world", "Мир хоста")
                               : m.in_world  ? L(r, "In world", "В мире") // short: the value column is narrow
                                             : L(r, "In own world", "В своём мире");
            if (m.connected && !m.local && m.ping_ms >= 0) {
                text += ", " + std::to_string(m.ping_ms) + L(r, " ms", " мс");
            }
            return text;
        };
        both(where);
        SetDynamic(party.member_value[k], en, ru);
        if (member_kickable[k]) {
            SetDynamic(party.member_confirm[k], "Press again: kick", "Ещё раз: исключить");
        } else {
            SetDynamic(party.member_confirm[k], en, ru);
        }
        both([&](bool r) {
            std::string text = where(r);
            if (!m.connected) text += L(r, ", slot kept", ", место сохранено");
            if (!m.area.empty()) text += ", " + m.area;
            if (member_kickable[k]) {
                text += L(r, ". Twice left/right: kick", ". Дважды влево/вправо: исключить");
            }
            return text;
        });
        SetDynamic(party.member_help[k], en, ru);
    }
}

void RefreshRestart() {
    if (BbSettings::RestartNeeded()) {
        SetDynamic(party.restart_help, "Changes wait for a restart: press twice to restart the game now",
                   "Изменения ждут перезапуска: нажмите дважды, чтобы перезапустить игру");
    } else {
        SetDynamic(party.restart_help, "Twice left/right: restart the game (title screen; progress is saved)",
                   "Дважды влево/вправо: перезапустить игру (титульный экран; прогресс сохранён)");
    }
}

Texts T2(const char* en, const char* ru) {
    return {en, ru};
}
bool Active(const party::status::Board& b) {
    return b.enabled && b.role != party::status::Role::Off;
}
/// The link runs (not after Leave): Copy code / Rejoin / Leave only then. After Leave the party
/// flow's Host party / Join party take over (live: the page then had seven rows, Join party showed
/// next to Rejoin the host and the sixth-row limit cut Single player).
bool Running(const party::status::Board& b) {
    return Active(b) && b.state != party::status::State::Off;
}

/// The Party page's own rows (orders 10..40): status, copy code, rejoin, leave. The join/leave
/// notices switch and the last event are on the Party members page (room for the party flow's
/// rows here).
void DefineBuiltinPartyRows() {
    namespace ps = party::status;
    using B = ps::Board;
    PartyRow status;
    status.order = 10;
    status.name = [](const B&) { return T2("Status", "Состояние"); };
    status.value = [](const B& b) {
        return b.enabled ? Texts{StateShort(b.state, false), StateShort(b.state, true)}
                         : T2("Single player", "Одиночная игра");
    };
    status.help = [](const B& b) {
        if (!b.enabled) {
            // The party flow's rows (party_flow_rows.cpp) start or join a party game on the title.
            return T2("Single-player saves. Party games start on the title screen (System > Party)",
                      "Одиночные сохранения. Игра группой начинается на титульном экране (Система > Группа)");
        }
        Texts t;
        for (bool r : {false, true}) {
            std::string text = b.role == ps::Role::Host    ? L(r, "Host (your world): ", "Хост (ваш мир): ")
                               : b.role == ps::Role::Guest ? L(r, "Guest: ", "Гость: ")
                                                           : "";
            text += StateLong(b.state, b.role, r);
            if (!b.detail.empty()) text += " (" + b.detail + ")";
            (r ? t.ru : t.en) = text;
        }
        return t;
    };
    party_rows.push_back({status});

    PartyRow copy;
    copy.order = 20;
    copy.name = [](const B&) { return T2("Copy party code", "Копировать код"); };
    copy.value = [](const B&) { return T2("Copy", "Копировать"); };
    copy.pressed = [](const B&) { return T2("Copied", "Скопирован"); };
    copy.help = [](const B& b) {
        if (!b.enabled) return T2("Party mode is off", "Режим группы выключен");
        if (b.code.empty()) return T2("The party code is not known yet", "Код группы ещё неизвестен");
        const bool host = b.role == ps::Role::Host;
        return Texts{std::string(host ? "Left/right: copy the code for friends: " : "Left/right: copy the code: ") +
                         b.code,
                     std::string(host ? "Влево/вправо: копировать код для друзей: " : "Влево/вправо: копировать код: ") +
                         b.code};
    };
    copy.visible = [](const B& b) { return Running(b); }; // party flow: the solo title's rows instead
    copy.press = PartyRow::Press::Once;
    copy.run = [](const B&) { ps::RequestCopyCode(); };
    party_rows.push_back({copy});

    PartyRow rejoin;
    rejoin.order = 30;
    rejoin.name = [](const B& b) {
        return b.role == ps::Role::Host ? T2("Summon again", "Призвать снова") : T2("Rejoin the host", "Вернуться к хосту");
    };
    rejoin.value = [](const B&) { return T2("Request", "Запросить"); };
    rejoin.pressed = [](const B&) { return T2("Requested", "Запрошено"); };
    rejoin.help = [](const B& b) {
        return b.role == ps::Role::Host
                   ? T2("Left/right: ring the Beckoning Bell again for waiting hunters",
                        "Влево/вправо: снова позвонить в манящий колокол для ожидающих")
               : b.role == ps::Role::Guest
                   ? T2("Left/right: reconnect to the host now and ring the bell",
                        "Влево/вправо: переподключиться к хосту и позвонить в колокол")
                   : T2("Party mode is off", "Режим группы выключен");
    };
    rejoin.visible = [](const B& b) { return Running(b); };
    rejoin.press = PartyRow::Press::Once;
    rejoin.run = [](const B& b) {
        if (Active(b)) ps::RequestRejoin();
    };
    party_rows.push_back({rejoin});

    PartyRow leave;
    leave.order = 40;
    leave.name = [](const B&) { return T2("Leave the party", "Покинуть группу"); };
    leave.value = [](const B&) { return T2("Leave", "Покинуть"); };
    leave.pressed = [](const B&) { return T2("Press again", "Нажмите ещё раз"); };
    leave.help = [](const B& b) {
        return b.role == ps::Role::Host
                   ? T2("Twice left/right: end the party, every guest returns home",
                        "Дважды влево/вправо: завершить группу, гости вернутся в свои миры")
               : b.role == ps::Role::Guest
                   ? T2("Twice left/right: return to your world, stop rejoining",
                        "Дважды влево/вправо: вернуться в свой мир и не возвращаться к хосту")
                   : T2("Party mode is off", "Режим группы выключен");
    };
    leave.visible = [](const B& b) { return Running(b); };
    leave.press = PartyRow::Press::Confirm;
    leave.run = [](const B& b) {
        if (Active(b)) ps::RequestLeave();
    };
    party_rows.push_back({leave});

}

void DefineAll() {
    const u32 off = AddText("Off", "Выкл"), on = AddText("On", "Вкл");
    const std::vector<u32> toggle = {off, on};
    auto& s = BbSettings::Get();
    (void)s;

    // ---- Display: the picture (sliders and toggles first, the pop-up list last). ----
    AddPage("Display", "Изображение", "Resolution, upscaler, sharpness and model detail (bbport)",
            "Разрешение, апскейлер, резкость и детализация (bbport)");
    {
        Field f;
        f.kind = Kind::Steps;
        f.steps = BbSettings::PresetCount;
        f.debounce = true;
        f.get = [](const Values& v) { return v.preset.load(); };
        f.set = [](int i) { BbSettings::Get().preset = std::clamp(i, 0, BbSettings::PresetCount - 1); };
        AddRow(AddField(std::move(f)), "Quality preset", "Пресет",
               "Scene resolution: 0-1 Native AA, 2-3 Quality, 4-6 Balanced, 7-8 Performance, 9-10 Ultra",
               "Разрешение сцены: 0-1 Native AA, 2-3 Quality, 4-6 Balanced, 7-8 Performance, 9-10 Ultra",
               {AddText("Native AA", "Native AA"), AddText("Quality", "Quality"), AddText("Balanced", "Balanced"),
                AddText("Performance", "Performance"), AddText("Ultra Performance", "Ultra Performance")},
               Kind::Steps);
    }
    AddRow(Setting(
               Kind::Slider,
               [](const Values& v) {
                   return v.sharpen ? std::clamp(int(std::lround(v.sharpness / SharpnessStep)), 1, 10) : 0;
               },
               [](int i) {
                   auto& st = BbSettings::Get();
                   st.sharpen = i != 0;
                   if (i != 0) st.sharpness = std::clamp(i, 0, 10) * SharpnessStep;
               }),
           "Sharpness", "Резкость", "RCAS after the upscaler: 0 off, 10 strongest",
           "RCAS после апскейлера: 0 выкл, 10 сильнее всего", {}, Kind::Slider);
    {
        Field f;
        f.kind = Kind::Steps;
        f.steps = BbSettings::OutputCount;
        f.debounce = true;
        f.get = [](const Values& v) { return v.output_res.load(); };
        f.set = [](int i) { BbSettings::Get().output_res = std::clamp(i, 0, BbSettings::OutputCount - 1); };
        AddRow(AddField(std::move(f)), "Output resolution", "Разрешение вывода",
               "Final frame: 0-1 1280x720, 2-4 1920x1080, 5-7 2560x1440, 8-10 3840x2160",
               "Готовый кадр: 0-1 1280x720, 2-4 1920x1080, 5-7 2560x1440, 8-10 3840x2160",
               {AddText("1280 x 720", "1280 x 720"), AddText("1920 x 1080", "1920 x 1080"),
                AddText("2560 x 1440", "2560 x 1440"), AddText("3840 x 2160", "3840 x 2160")},
               Kind::Steps);
    }
    AddRow(Flag(&Values::show_fps), "FPS counter", "Счётчик FPS", "Frame rate in the top right corner",
           "Частота кадров в правом верхнем углу", toggle, Kind::Toggle);
    AddRow(Setting(
               Kind::Steps,
               [](const Values& v) {
                   int index = 1;
                   for (int i = 0; i < 4; ++i) {
                       if (LodValues[i] == v.model_lod) index = i;
                   }
                   return index;
               },
               [](int i) { BbSettings::Get().model_lod = LodValues[std::clamp(i, 0, 3)]; }, 4),
           "Model detail", "Детализация",
           "After a restart: 0-1 highest, 2-4 game default, 5-7 lower, 8-10 lowest",
           "После перезапуска: 0-1 максимальная, 2-4 как в игре, 5-7 ниже, 8-10 минимальная",
           {AddText("Highest", "Максимальная"), AddText("Game default", "Как в игре"), AddText("Lower", "Ниже"),
            AddText("Lowest", "Минимальная")},
           Kind::Steps);
    upscaler_field = Setting(
        Kind::List, [](const Values& v) { return v.upscaler.load(); },
        [](int i) {
            const int u = std::clamp(i, 0, BbSettings::UpscalerCount - 1);
            BbSettings::Get().upscaler = UpscalerAvailable(u) ? u : int(BbSettings::UpscalerFsr3);
        });
    AddRow(upscaler_field, "Upscaler", "Апскейлер", "Temporal upscaler and anti-aliasing",
           "Временной апскейлер и сглаживание",
           {off, AddText("FSR 3.1", "FSR 3.1"), AddText("FSR 4", "FSR 4"), AddText("FSR 4.1.1", "FSR 4.1.1"),
            AddText("TAA", "TAA"), AddText("DLSS", "DLSS")},
           Kind::List);

    // ---- Effects: the game's graphics patches (after a restart). ----
    AddPage("Effects", "Эффекты", "The game's effects (bbport; after a restart)",
            "Эффекты игры (bbport; после перезапуска)");
    struct Short {
        const char *key, *en, *ru, *help_en, *help_ru;
    };
    static const Short graphics[] = {
        {"effect_chromatic_aberration", "Chromatic aberration", "Хром. аберрация",
         "Colour fringes at the frame edges", "Цветные каймы по краям кадра"},
        {"effect_dof", "Depth of field", "Глубина резкости", "Blur of distant and near objects (DoF)",
         "Размытие дальних и близких объектов (DoF)"},
        {"effect_motion_blur", "Motion blur", "Размытие в движении", "Blur when the camera turns",
         "Размытие при повороте камеры"},
        {"effect_ssao", "Ambient occlusion", "Затенение SSAO", "Shading in corners and contacts (SSAO)",
         "Затенение в углах и местах касания (SSAO)"},
        {"effect_dynamic_shadows", "Dynamic shadows", "Динамические тени", "Shadows from dynamic lights",
         "Тени от динамических источников света"},
        {"effect_ssr", "SSR reflections", "Отражения SSR", "Screen-space reflections (not in the original)",
         "Экранные отражения (не было в оригинале)"},
    };
    static const Short patches[] = {
        {"effect_game_aa", "Game's own AA", "Сглаживание игры", "The game's own anti-aliasing",
         "Собственное сглаживание игры"},
        {"skip_intro", "Skip intros", "Пропуск заставок", "Skip the startup logos and intro",
         "Пропуск логотипов и заставки при запуске"},
        {"debug_camera", "Free camera", "Свободная камера", "Hold Cross, press L3 (keys: Space + Z)",
         "Удерживайте Крест и нажмите L3 (клавиши: Пробел + Z)"},
        {"debug_menu", "Debug menu", "Отладочное меню", "Touchpad left side; needs Nexus mod #253 files",
         "Левая сторона тачпада; нужны файлы мода Nexus #253"},
    };
    const auto effect_row = [&](const Short& sh) {
        for (int e = 0; e < BbSettings::EffectCount; ++e) {
            if (std::strcmp(BbSettings::Effects[e].key, sh.key) != 0) continue;
            const std::string help_en = std::string(sh.help_en) + " (after a restart)";
            const std::string help_ru = std::string(sh.help_ru) + " (после перезапуска)";
            AddRow(Setting(
                       Kind::Toggle, [e](const Values& v) { return int(v.effects[e].load()); },
                       [e](int i) { BbSettings::Get().effects[e] = i != 0; }),
                   sh.en, sh.ru, help_en.c_str(), help_ru.c_str(), toggle, Kind::Toggle);
        }
    };
    for (const Short& sh : graphics) effect_row(sh);

    // ---- Upscaler: its details. ----
    AddPage("Upscaler", "Апскейлер", "Jitter, motion vectors, reactive mask, FSR 4 options (bbport)",
            "Сдвиг, векторы движения, маска реактивности, опции FSR 4 (bbport)");
    AddRow(Flag(&Values::jitter), "Subpixel jitter", "Субпиксельный сдвиг",
           "Shifts the scene each frame so the upscaler rebuilds more detail",
           "Сдвиг сцены каждый кадр: апскейлер собирает больше деталей", toggle, Kind::Toggle);
    AddRow(Flag(&Values::object_motion), "Character motion", "Движение персонажей",
           "Accurate motion vectors for animated objects (after a restart)",
           "Точные векторы движения анимированных объектов (после перезапуска)", toggle, Kind::Toggle);
    AddRow(Flag(&Values::reactive), "Reactive mask", "Маска реактивности",
           "Marks transparent effects: fewer trails behind haze, light and particles",
           "Помечает прозрачные эффекты: меньше шлейфов за дымкой, светом и частицами", toggle, Kind::Toggle);
    AddRow(Flag(&Values::fsr4_auto_exposure), "FSR 4 auto exposure", "FSR 4: автоэкспозиция",
           "FSR 4 only: the network normalises colour by exposure",
           "Только FSR 4: сеть нормирует цвет по экспозиции", toggle, Kind::Toggle);
    AddRow(Flag(&Values::fsr4_invert_jitter), "FSR 4 invert jitter", "FSR 4: обратный сдвиг",
           "FSR 4 only: inverts the jitter sign (for diagnosing ghosting)",
           "Только FSR 4: обратный знак сдвига (для проверки шлейфов)", toggle, Kind::Toggle);
    AddRow(Setting(
               Kind::List, [](const Values& v) { return std::clamp(v.live_resolution.load() + 1, 0, 2); },
               [](int i) { BbSettings::Get().live_resolution = std::clamp(i, 0, 2) - 1; }),
           "Live resolution", "Разрешение на лету",
           "Resolution and preset change without a restart (after a restart; Auto: by GPU)",
           "Разрешение и пресет меняются без перезапуска (после перезапуска; Авто: по видеокарте)",
           {AddText("Auto (by GPU)", "Авто (по видеокарте)"), AddText("Off (faster)", "Выкл (быстрее)"), on},
           Kind::List);

    // ---- Advanced: the reactive mask's tuning, shader compilation, debug views. ----
    AddPage("Advanced", "Дополнительно", "Reactive mask tuning, shader compilation, debug view (bbport)",
            "Настройка маски реактивности, компиляция шейдеров, отладка (bbport)");
    AddRow(Scaled(&Values::reactive_scale, MaskScaleTable), "Mask scale", "Масштаб маски",
           "Reactive mask scale: 0 = 0, 4 = 1.0 (default), 10 = 4.0",
           "Масштаб маски реактивности: 0 = 0, 4 = 1.0 (по умолчанию), 10 = 4.0", {}, Kind::Slider);
    AddRow(Scaled(&Values::reactive_threshold, TenthsTable), "Mask threshold", "Порог маски",
           "Reactive mask threshold: 0 = 0.0 .. 10 = 1.0 (default 2)",
           "Порог маски реактивности: 0 = 0.0 .. 10 = 1.0 (по умолчанию 2)", {}, Kind::Slider);
    AddRow(Scaled(&Values::reactive_max, TenthsTable), "Mask maximum", "Максимум маски",
           "Reactive mask maximum: 0 = 0.0 .. 10 = 1.0 (default 9)",
           "Максимум маски реактивности: 0 = 0.0 .. 10 = 1.0 (по умолчанию 9)", {}, Kind::Slider);
    AddRow(Flag(&Values::async_shaders), "Async shaders", "Асинхронные шейдеры",
           "New shaders compile in the background: objects may miss a frame or two",
           "Новые шейдеры компилируются в фоне: объекты могут пропасть на кадр-два", toggle, Kind::Toggle);
    AddRow(Flag(&Values::gpl), "Pipeline libraries", "Библиотеки конвейеров",
           "Pipelines from reusable parts (VK_EXT_graphics_pipeline_library; after a restart)",
           "Конвейеры из готовых частей (VK_EXT_graphics_pipeline_library; после перезапуска)", toggle,
           Kind::Toggle);
    AddRow(Setting(
               Kind::List, [](const Values& v) { return v.debug_view.load(); },
               [](int i) { BbSettings::Get().debug_view = std::clamp(i, 0, BbSettings::DebugViewCount - 1); }),
           "Debug view", "Отладочный вид",
           "Motion vectors: red/green = motion, blue = accurate object vector",
           "Векторы движения: красный/зелёный — движение, синий — точный вектор объекта",
           {off, AddText("Reactive mask", "Маска"), AddText("Motion vectors", "Векторы движения")}, Kind::List);

    // ---- Performance HUD. ----
    AddPage("Performance HUD", "Оверлей FPS", "Frame rate, frame time and load in a corner (F11) (bbport)",
            "Частота кадров, время кадра и загрузка в углу экрана (F11) (bbport)");
    AddRow(Flag(&Values::show_hud), "Performance HUD", "Оверлей FPS",
           "Frame rate, frame time, graph, VRAM, GPU, CPU, RAM. F11: on/off",
           "Частота и время кадра, график, видеопамять, нагрузка. F11: вкл/выкл", toggle, Kind::Toggle);
    AddRow(Scaled(&Values::hud_opacity, OpacityTable), "Background opacity", "Непрозрачность фона",
           "HUD background: 0 = 0.1 .. 7 = 0.75 (default) .. 10 = 1.0",
           "Фон оверлея: 0 = 0.1 .. 7 = 0.75 (по умолчанию) .. 10 = 1.0", {}, Kind::Slider);
    AddRow(Scaled(&Values::hud_scale, ScaleTable), "HUD size", "Размер оверлея",
           "HUD size: 0 = 0.5x, 5 = 1.0x (default), 10 = 2.0x",
           "Размер оверлея: 0 = 0.5x, 5 = 1.0x (по умолчанию), 10 = 2.0x", {}, Kind::Slider);
    AddRow(Flag(&Values::compile_indicator), "Shader indicator", "Индикатор шейдеров",
           "\"Compiling shaders: N%\" in the bottom right corner",
           "«Компиляция шейдеров: N%» в правом нижнем углу", toggle, Kind::Toggle);
    AddRow(Setting(
               Kind::Toggle,
               [](const Values& v) { return int(v.menu_language.load() == BbSettings::MenuLanguage::Russian); },
               [](int i) {
                   BbSettings::Get().menu_language =
                       i ? BbSettings::MenuLanguage::Russian : BbSettings::MenuLanguage::English;
               }),
           "Port's language", "Язык порта",
           "Language of the HUD, notices and the overlay menu (Insert)",
           "Язык оверлея FPS, уведомлений и меню оверлея (Insert)",
           {AddText("English", "English"), AddText("Русский", "Русский")}, Kind::Toggle);
    AddRow(Setting(
               Kind::List, [](const Values& v) { return v.hud_position.load(); },
               [](int i) { BbSettings::Get().hud_position = std::clamp(i, 0, BbSettings::HudPositionCount - 1); }),
           "HUD position", "Положение",
           "Corner of the screen (bottom right: the shader indicator moves left)",
           "Угол экрана (справа снизу: индикатор шейдеров уходит влево)",
           {AddText("Top left", "Сверху слева"), AddText("Top right", "Сверху справа"),
            AddText("Bottom left", "Снизу слева"), AddText("Bottom right", "Снизу справа")},
           Kind::List);

    // ---- HUD details: what the performance HUD shows. ----
    AddPage("HUD details", "Данные оверлея", "What the performance HUD shows (bbport)",
            "Что показывает оверлей FPS (bbport)");
    AddRow(Flag(&Values::hud_graph), "Frame-time graph", "График кадров",
           "The last 120 frame times; spikes are stutters", "Время последних 120 кадров; пики — подтормаживания",
           toggle, Kind::Toggle);
    AddRow(Flag(&Values::hud_vram), "Video memory", "Видеопамять", "VRAM used by the game and the driver's budget",
           "Видеопамять игры и бюджет драйвера", toggle, Kind::Toggle);
    AddRow(Flag(&Values::hud_gpu), "GPU load", "Загрузка видеокарты",
           "NVIDIA: from the driver; AMD / Intel: Windows' GPU counters",
           "NVIDIA: от драйвера; AMD / Intel: счётчики Windows", toggle, Kind::Toggle);
    AddRow(Flag(&Values::hud_cpu), "CPU load", "Загрузка процессора", "Load of all CPU cores together",
           "Общая загрузка всех ядер процессора", toggle, Kind::Toggle);
    AddRow(Flag(&Values::hud_ram), "RAM", "Оперативная память", "System memory in use / installed",
           "Занятая / установленная оперативная память", toggle, Kind::Toggle);

    // ---- Mouse & keyboard. ----
    AddPage("Mouse & keyboard", "Мышь и клавиатура", "PC controls: mouse look and attacks, Souls-style keys (bbport)",
            "Управление для ПК: обзор и атаки мышью, раскладка как в Souls (bbport)");
    AddRow(Flag(&Values::mk_enabled), "Mouse & keyboard", "Мышь и клавиатура",
           "Off: the classic keyboard layout (WASD / IJKL), the mouse unused",
           "Выкл: классическая раскладка (WASD / IJKL), мышь не используется", toggle, Kind::Toggle);
    AddRow(Scaled(&Values::mk_sens_x, SensTable), "Horizontal speed", "Скорость по горизонтали",
           "Mouse sensitivity: 0 = 0.25, 5 = 1.0 (default), 10 = 5.0",
           "Чувствительность мыши: 0 = 0.25, 5 = 1.0 (по умолчанию), 10 = 5.0", {}, Kind::Slider);
    AddRow(Scaled(&Values::mk_sens_y, SensTable), "Vertical speed", "Скорость по вертикали",
           "Mouse sensitivity: 0 = 0.25, 5 = 1.0 (default), 10 = 5.0",
           "Чувствительность мыши: 0 = 0.25, 5 = 1.0 (по умолчанию), 10 = 5.0", {}, Kind::Slider);
    AddRow(Setting(
               Kind::List,
               [](const Values& v) { return int(v.mk_invert_x.load()) | int(v.mk_invert_y.load()) << 1; },
               [](int i) {
                   BbSettings::Get().mk_invert_x = (i & 1) != 0;
                   BbSettings::Get().mk_invert_y = (i & 2) != 0;
               }),
           "Invert look", "Инверсия", "Inverts the mouse look horizontally and/or vertically",
           "Инверсия обзора мышью по горизонтали и/или вертикали",
           {AddText("None", "Нет"), AddText("Horizontal", "По горизонтали"), AddText("Vertical", "По вертикали"),
            AddText("Both", "Обе оси")},
           Kind::List);
    // A page holds six rows: the fine tuning and the wheel (upstream 0.51, #52) have their own.
    AddPage("Mouse tuning", "Настройка мыши", "Mouse look smoothing, dead zone and the wheel (bbport)",
            "Сглаживание и мёртвая зона обзора мышью, колесо (bbport)");
    AddRow(Scaled(&Values::mk_smoothing, SmoothTable), "Mouse smoothing", "Сглаживание мыши",
           "Averages the mouse over frames: 0 raw, 4 = 0.2 (default), 10 = 0.8",
           "Усреднение мыши: 0 без него, 4 = 0.2 (по умолчанию), 10 = 0.8", {}, Kind::Slider);
    AddRow(Scaled(&Values::mk_deadzone, DeadTable), "Mouse dead zone", "Мёртвая зона",
           "Ignores slow movement (tremor): 0 none, 5 = 0.05 (default), 10 = 0.2",
           "Не учитывает медленное движение: 0 нет, 5 = 0.05 (по умолчанию), 10 = 0.2", {}, Kind::Slider);
    AddRow(Flag(&Values::mk_wheel), "Mouse wheel", "Колесо мыши",
           "Quick items and tools (d-pad up / down), one press per notch",
           "Быстрые предметы и инструменты (крестовина вверх / вниз), по нажатию на щелчок", toggle,
           Kind::Toggle);

    // ---- Game patches (after a restart) and the restart itself. ----
    AddPage("Game patches", "Патчи игры", "The game's own AA, intros, free camera, debug menu, restart, quit (bbport)",
            "Сглаживание игры, заставки, свободная камера, отладочное меню, перезапуск, выход (bbport)");
    for (const Short& sh : patches) effect_row(sh);
    party.restart_help = AddDynamic(MaxHelpLength);
    AddRow(Confirmed([] {
               BbSettings::Save();
               if (restart_handler) restart_handler();
           }),
           AddText("Restart the game", "Перезапуск игры"), party.restart_help,
           {AddText("Restart", "Перезапуск"), AddText("Press again", "Нажмите ещё раз")}, Kind::Toggle);
    // Upstream 0.51 (#74, #116): quit from the menu, as the window's close button.
    AddRow(Confirmed([] {
               BbSettings::Save();
               if (quit_handler) quit_handler();
           }),
           "Quit the game", "Выход из игры",
           "Closes the game like the window's close button; in a party the others are told",
           "Закрывает игру, как кнопка закрытия окна; группа об этом узнает",
           {AddText("Quit", "Выйти"), AddText("Press again", "Нажмите ещё раз")}, Kind::Toggle);

    // ---- Party (party_status.h): the action table's rows. ----
    AddPage("Party", "Группа", "Party co-op: status, code, rejoin, leave (bbport)",
            "Совместная игра: состояние, код, возврат, выход (bbport)");
    DefineBuiltinPartyRows();
    for (PartyRow& r : pending_party_rows) party_rows.push_back({std::move(r)});
    pending_party_rows.clear();
    std::stable_sort(party_rows.begin(), party_rows.end(),
                     [](const PartyEntry& a, const PartyEntry& b) { return a.spec.order < b.spec.order; });
    party_row_visible.assign(party_rows.size(), 1);
    for (std::size_t i = 0; i < party_rows.size(); ++i) {
        PartyEntry& e = party_rows[i];
        e.name = AddDynamic(MaxNameLength);
        e.help = AddDynamic(MaxHelpLength);
        e.value = AddDynamic(MaxValueLength);
        e.pressed = AddDynamic(MaxValueLength);
        const PartyRow& r = e.spec;
        if (r.toggle) {
            e.field = Setting(
                Kind::Toggle, [get = r.toggle_get](const Values&) { return int(get()); },
                [set = r.toggle_set](int v) { set(v != 0); });
        } else if (r.press == PartyRow::Press::None || !r.run) {
            e.field = Info();
        } else {
            const auto run = r.run;
            const auto act = [run] { run(party::status::Snapshot()); };
            if (r.press == PartyRow::Press::Confirm) {
                e.field = Confirmed(act);
            } else {
                Field f;
                f.kind = Kind::Toggle;
                f.act = [act](int from, int to, std::int64_t) {
                    // Every press (live: the press back to 0 only reset the text, so a second edit of
                    // the address took two presses and showed 'Type, then Enter' after Enter).
                    if (from != to) act();
                };
                e.field = AddField(std::move(f));
            }
        }
        AddRow(e.field, e.name, e.help, {e.value, e.pressed}, Kind::Toggle,
               [i] { return party_row_visible[i] != 0; });
    }

    // ---- Party members: one row each (state, ping); the host kicks with two presses. ----
    AddPage("Party members", "Участники группы", "The hunters in the party, their state and ping (bbport)",
            "Охотники группы, их состояние и пинг (bbport)");
    for (int k = 0; k < MaxMembers; ++k) {
        party.member_name[k] = AddDynamic(MaxNameLength);
        party.member_value[k] = AddDynamic(MaxValueLength);
        party.member_confirm[k] = AddDynamic(MaxValueLength);
        party.member_help[k] = AddDynamic(MaxHelpLength);
        AddRow(Confirmed([k] {
                   if (member_kickable[k] && !member_names[k].empty()) party::status::KickMember(member_names[k]);
               }),
               party.member_name[k], party.member_help[k], {party.member_value[k], party.member_confirm[k]},
               Kind::Toggle, [k] { return k < member_rows; });
    }
    // One row per member (kMaxPartyPlayers = 4), then the switches: a full party of four still
    // shows the notices and party HUD switches (the page's six rows); guest lamps and the last
    // event show while fewer than four hunters are in the party (lamps also: overlay Party tab).
    // The System menu is at its 12 pages, so the switches cannot move to a page of their own.
    AddRow(Flag(&Values::party_toasts), "Join/leave notices", "Уведомления",
           "A short notice in a top corner when a hunter joins or leaves",
           "Короткое уведомление в углу, когда охотник входит или выходит", toggle, Kind::Toggle);
    // Party HUD (party_hud.h): party mode only. With four members the last event row gives way.
    AddRow(Flag(&Values::party_hud), AddText("Party HUD", "Панель группы"),
           AddText("Companions' names, health and where they are, in the bottom left corner",
                   "Имена, здоровье и местоположение спутников в левом нижнем углу"),
           toggle, Kind::Toggle, [] { return party::status::Enabled(); });
    // Guests use lamps and headstones (party_guestsvc; read at the next start). Shown while fewer
    // than four hunters are in the party (the page holds six rows).
    AddRow(Flag(&Values::party_guest_lamps), AddText("Guests use lamps", "Фонари гостям"),
           AddText("Guests may use lamps and headstones; the whole party travels (after a restart)",
                   "Гости могут пользоваться фонарями и надгробиями; группа идёт вместе (после перезапуска)"),
           toggle, Kind::Toggle, [] { return party::status::Enabled(); });
    party.event_value = AddDynamic(MaxValueLength);
    party.event_help = AddDynamic(MaxHelpLength);
    AddRow(Info(), AddText("Last event", "Последнее"), party.event_help, {party.event_value, party.event_value},
           Kind::Toggle);

    // ---- Party rules (party_balance.h): the death debuff and the overhead display. ----
    AddPage("Party rules", "Правила группы", "Death debuff, what shows above companions (bbport)",
            "Штраф за смерть, что видно над спутниками (bbport)");
    balance.rot_help = AddDynamic(MaxHelpLength);
    AddRow(Flag(&Values::party_rot), AddText("Rot on death", "Гниль при смерти"), balance.rot_help, toggle,
           Kind::Toggle);
    AddRow(Setting(
               Kind::List, [](const Values& v) { return std::clamp(v.party_overhead.load(), 0, 4); },
               [](int i) { BbSettings::Get().party_overhead = std::clamp(i, 0, 4); }),
           AddText("Above companions", "Над спутниками"),
           AddText("What shows above party members in view (only you see it)",
                   "Что показывать над спутниками в поле зрения (только у вас)"),
           {AddText("Nothing", "Ничего"), AddText("Name", "Имя"), AddText("Name and level", "Имя и уровень"),
            AddText("Name and health", "Имя и здоровье"), AddText("Name and deaths", "Имя и смерти")},
           Kind::List);

    // ---- Enemy scaling (party_balance.h): per extra hunter, the host's values count. ----
    AddPage("Enemy scaling", "Сила врагов", "Boss and enemy health, damage and poise per extra hunter (bbport)",
            "Здоровье, урон и стойкость боссов и врагов за каждого охотника (bbport)");
    const auto scale_row = [&](std::atomic<int> Values::*member, const int* table, const char* en, const char* ru,
                               int index) {
        balance.scale_help[index] = AddDynamic(MaxHelpLength);
        AddRow(Setting(
                   Kind::Slider,
                   [member, table](const Values& v) { return coop::balance::Position(table, (v.*member).load()); },
                   [member, table](int i) { (BbSettings::Get().*member) = coop::balance::AtPosition(table, i); }),
               AddText(en, ru), balance.scale_help[index], {}, Kind::Slider);
    };
    using namespace coop::balance;
    scale_row(&Values::party_boss_hp, kHpTable, "Boss health", "Здоровье боссов", 0);
    scale_row(&Values::party_boss_damage, kDamageTable, "Boss damage", "Урон боссов", 1);
    scale_row(&Values::party_boss_poise, kPoiseTable, "Boss poise", "Стойкость боссов", 2);
    scale_row(&Values::party_enemy_hp, kHpTable, "Enemy health", "Здоровье врагов", 3);
    scale_row(&Values::party_enemy_damage, kDamageTable, "Enemy damage", "Урон врагов", 4);
    scale_row(&Values::party_enemy_poise, kPoiseTable, "Enemy poise", "Стойкость врагов", 5);
}

/// The party rules' help lines: the current value, and on a guest that the host's rules count.
void RefreshBalance() {
    if (!balance.rot_help) return;
    const auto& v = BbSettings::Get();
    const bool guest = party::status::Snapshot().role == party::status::Role::Guest;
    const std::string g_en = guest ? " (host decides)" : "", g_ru = guest ? " (решает хост)" : "";
    SetDynamic(balance.rot_help, "Each death: max HP -10 % (up to 5), a lamp rest cleanses it" + g_en,
               "Каждая смерть: -10 % здоровья (до 5 раз), отдых у фонаря снимает" + g_ru);
    using namespace coop::balance;
    const int values[6] = {v.party_boss_hp, v.party_boss_damage, v.party_boss_poise,
                           v.party_enemy_hp, v.party_enemy_damage, v.party_enemy_poise};
    for (int k = 0; k < 6; ++k) {
        const int kind = k % 3;
        const std::string en = kind == 0 ? DescribeHp(values[k], false)
                               : kind == 1 ? DescribeDamage(values[k], false) : DescribePoise(values[k], false);
        const std::string ru = kind == 0 ? DescribeHp(values[k], true)
                               : kind == 1 ? DescribeDamage(values[k], true) : DescribePoise(values[k], true);
        const char* ref_en = k < 3 ? "; 100 % = Bloodborne" : "; vanilla 0";
        const char* ref_ru = k < 3 ? "; 100 % = как в игре" : "; в игре 0";
        if (!balance.scale_help[k]) continue;  // poise: overlay Party tab / bbport.ini only
        SetDynamic(balance.scale_help[k], en + (kind == 1 ? "" : ref_en) + g_en, ru + (kind == 1 ? "" : ref_ru) + g_ru);
    }
}

} // namespace

std::u16string Utf16(const char* utf8) {
    std::u16string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8); *p;) {
        u32 c = *p++;
        if (c >= 0xf0) {
            c = (c & 7) << 18 | (p[0] & 0x3f) << 12 | (p[1] & 0x3f) << 6 | (p[2] & 0x3f);
            p += 3;
        } else if (c >= 0xe0) {
            c = (c & 15) << 12 | (p[0] & 0x3f) << 6 | (p[1] & 0x3f);
            p += 2;
        } else if (c >= 0xc0) {
            c = (c & 31) << 6 | (p[0] & 0x3f);
            p += 1;
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(char16_t(0xd800 + (c >> 10)));
            out.push_back(char16_t(0xdc00 + (c & 0x3ff)));
        } else {
            out.push_back(char16_t(c));
        }
    }
    return out;
}

int ChoiceFromPosition(u8 position, int count) {
    return std::clamp((int(position) * (count - 1) + 5) / 10, 0, count - 1);
}

u8 ChoicePosition(int choice, int count) {
    return u8((std::clamp(choice, 0, count - 1) * 10 * 2 + (count - 1)) / (2 * (count - 1)));
}

u8 NearestPosition(const float* table, float value) {
    int best = 0;
    for (int i = 1; i < 11; ++i) {
        if (std::fabs(table[i] - value) < std::fabs(table[best] - value)) best = i;
    }
    return u8(best);
}

void Define() {
    if (!pages.empty()) {
        return;
    }
    DefineAll();
    const std::size_t n = fields.size();
    values.assign(n, 0);
    defaults.assign(n, 0);
    applied.assign(n, 0);
    resting.assign(n, -1);
    bytes.assign(n, 0);
    default_bytes.assign(n, 0);
    rested_since.assign(n, 0);
    armed_at.assign(n, -1000000);
    for (const Page& p : pages) {
        for (const Row& r : p.rows) {
            Field& f = fields[std::size_t(r.field)];
            f.kind = r.kind;
            if (r.kind == Kind::Steps) f.steps = int(r.choices.size());
        }
    }
    const Values& d = DefaultSettings();
    for (std::size_t i = 0; i < n; ++i) {
        const Field& f = fields[i];
        defaults[i] = f.get ? f.get(d) : 0;
        default_bytes[i] = f.steps ? ChoicePosition(defaults[i], f.steps) : u8(defaults[i]);
    }
    RefreshDynamic();
}

const std::vector<Page>& All() {
    return pages;
}

int FieldCount() {
    return int(fields.size());
}

const char16_t* Text(u32 id, bool russian) {
    if (id < IdBase || id >= IdBase + texts.size()) {
        return nullptr;
    }
    const TextEntry& t = texts[id - IdBase];
    if (t.dynamic >= 0) {
        return dynamics[std::size_t(t.dynamic)]->buffer[russian ? 1 : 0].data();
    }
    return russian ? t.russian.c_str() : t.english.c_str();
}

std::size_t TextCount() {
    return texts.size();
}

std::uint64_t DynamicChanges() {
    return dynamic_changes.load(std::memory_order_acquire);
}

bool IsDynamic(u32 id) {
    return id >= IdBase && id < IdBase + texts.size() && texts[id - IdBase].dynamic >= 0;
}

int* Value(int field) {
    return &values[std::size_t(field)];
}
u8* Byte(int field) {
    return &bytes[std::size_t(field)];
}
const int* DefaultValue(int field) {
    return &defaults[std::size_t(field)];
}
const u8* DefaultByte(int field) {
    return &default_bytes[std::size_t(field)];
}
bool ByteField(int field) {
    return fields[std::size_t(field)].kind != Kind::List;
}

bool ChoiceAvailable(int field, int choice) {
    return field != upscaler_field || UpscalerAvailable(choice);
}

/// Load (the game's menu thread, when a page opens) and Poll / RefreshDynamic (the present
/// thread) share the values and the run-time texts' sources (member names, the visible rows):
/// one of them at a time. Recursive: a button's action may refresh the texts.
std::recursive_mutex& StateMutex() {
    static std::recursive_mutex m;
    return m;
}

void Load() {
    std::scoped_lock lock{StateMutex()};
    const Values& s = BbSettings::Get();
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const Field& f = fields[i];
        values[i] = f.get ? f.get(s) : 0;
        bytes[i] = f.steps ? ChoicePosition(values[i], f.steps) : u8(values[i]);
        applied[i] = values[i];
        resting[i] = values[i];
        if (!f.get) {
            default_bytes[i] = 0;
        }
    }
    RefreshDynamic();
    loaded = true;
}

void RefreshDynamic() {
    std::scoped_lock lock{StateMutex()};
    RefreshParty();
    RefreshRestart();
    RefreshBalance();
}

void Poll(std::int64_t now_ms) {
    std::scoped_lock lock{StateMutex()};
    if (!loaded) {
        return;
    }
    if (now_ms - last_refresh.load() >= 250) {
        last_refresh = now_ms;
        RefreshDynamic();
    }
    bool save = false;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const Field& f = fields[i];
        if (f.kind != Kind::List) {
            // A slider with choices: its position, not the choice's number.
            values[i] = f.steps ? ChoiceFromPosition(bytes[i], f.steps) : bytes[i];
        }
        const int value = values[i];
        if (value == applied[i]) {
            resting[i] = value;
            continue;
        }
        if (f.debounce) {
            // The output resolution and the preset re-create the scene targets and the upscaler
            // (a few grey frames): applied once the slider rests for 0.4 s, not at each step.
            if (resting[i] != value) {
                resting[i] = value;
                rested_since[i] = now_ms;
                continue;
            }
            if (now_ms - rested_since[i] < 400) {
                continue;
            }
        }
        const int from = applied[i];
        applied[i] = value;
        if (f.act) {
            f.act(from, value, now_ms);
            // The game's "Default" sets this: a button's default follows its value, so Default
            // does not press it.
            default_bytes[i] = bytes[i];
            defaults[i] = value;
        } else if (f.set) {
            f.set(value);
            save = true;
        }
    }
    if (save) {
        BbSettings::Save();
    }
}

void AddPartyRow(PartyRow row) {
    pending_party_rows.push_back(std::move(row));
}

int PartyRowCount() {
    return int(party_rows.size());
}

void SetRestartHandler(void (*restart)()) {
    restart_handler = restart;
}

void SetQuitHandler(void (*quit)()) {
    quit_handler = quit;
}

} // namespace BbGameMenu::Pages
