// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: what the port's pages in the game's System menu hold (bbport_game_menu.cpp builds them
// with the game's own rows): the texts in English and Russian, the pages and their rows, the
// values the rows edit and how a changed value is applied (bbport_settings, party_status
// commands). No game code here: unit-tested by tests/test_game_menu_pages.cpp.
//
// The game's limits, kept by every page (the test checks them):
//  - at most RowsPerPage rows (the "ControllSetting" layout the pages use has six);
//  - a pop-up list only as the last row (it opens downwards over the rows below it);
//  - short row names (the name column is narrow), one-line help texts.
// Rows: Toggle (left/right between two values), Slider (0..10), Steps (a slider standing for a
// few ordered choices), List (pop-up). Read-only lines and buttons are toggles whose value texts
// are made at run time (Dynamic texts): a button acts when its value changes, a confirmed one
// (Leave, Kick, Restart) on the second press.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "party/party_status.h"

namespace BbGameMenu::Pages {

using u8 = std::uint8_t;
using u32 = std::uint32_t;

constexpr u32 IdBase = 0x7f0000;     ///< the port's text ids (the game's own are far below)
constexpr std::size_t RowsPerPage = 6;
/// Text limits in UTF-16 units (the game draws longer texts over the next column or clips them).
constexpr std::size_t MaxNameLength = 24, MaxValueLength = 26, MaxHelpLength = 110;
constexpr std::size_t DynamicCapacity = 160; ///< a run-time text's buffer (UTF-16 units)

enum class Kind { List, Toggle, Slider, Steps };

struct Row {
    int field = 0;
    u32 name = 0, help = 0;
    std::vector<u32> choices; ///< value texts, value = index (a toggle: two)
    Kind kind = Kind::List;
    std::function<bool()> visible; ///< null: always; checked when the page opens
};
struct Page {
    u32 name = 0, help = 0;
    std::vector<Row> rows;
};

/// Builds the texts, fields and pages (once; later calls do nothing).
void Define();
const std::vector<Page>& All();
int FieldCount();

/// The port's text `id` in English or Russian, or null when it is not one of ours.
const char16_t* Text(u32 id, bool russian);
std::size_t TextCount();
/// For the test: whether `id` is made at run time.
bool IsDynamic(u32 id);

/// What the game's rows bind (stable addresses for the run).
int* Value(int field);
u8* Byte(int field);
const int* DefaultValue(int field);
const u8* DefaultByte(int field);
bool ByteField(int field); ///< toggle and slider rows bind bytes, lists ints
/// A list row's choice `choice` can be offered (the upscalers this GPU and build run).
bool ChoiceAvailable(int field, int choice);

/// The page is opening (the game's menu thread): the rows' values from the settings, the
/// run-time texts refreshed.
void Load();
/// Each presented frame: applies what the player changed (saved to bbport.ini) and runs the
/// buttons; refreshes the run-time texts a few times a second. `now_ms`: a steady clock.
void Poll(std::int64_t now_ms);
/// Refreshes the run-time texts (party status, member rows, the restart line).
void RefreshDynamic();

// ---- The Party page's action table (extension point: party flow rows, e.g. Host / Join) ----
/// A text in both languages (UTF-8).
struct Texts {
    std::string en, ru;
};
/// One row of the Party page. Every text is made from the party board when the page opens and a
/// few times a second while it is open (Poll), so a row can change its name, value and help with
/// the party's state. The row is a game toggle row: its two value texts are `value` (idle) and
/// `pressed` (after the first press: "Copied", "Press again" ...).
struct PartyRow {
    using Board = party::status::Board;
    int order = 0; ///< position on the page; built-in rows: status 10, copy code 20, rejoin 30,
                   ///< leave 40, notices 50, last event 60
    std::function<Texts(const Board&)> name;    ///< at most MaxNameLength units (cut otherwise)
    std::function<Texts(const Board&)> help;    ///< the bottom line, at most MaxHelpLength
    std::function<Texts(const Board&)> value;   ///< the value column (null: "-")
    std::function<Texts(const Board&)> pressed; ///< the second value text (null: = value)
    std::function<bool(const Board&)> visible;  ///< null: always shown
    /// None: a read-only line. Once: `run` on the first press (0 -> 1; the press back only resets
    /// the text). Confirm: the first press arms (shows `pressed`), the second within 10 s runs.
    enum class Press { None, Once, Confirm } press = Press::None;
    std::function<void(const Board&)> run; ///< menu thread / present thread: queue, never block
    /// A settings toggle instead of a button (value = off text, pressed = on text): saved to
    /// bbport.ini through BbSettings::Save after toggle_set.
    bool toggle = false;
    std::function<bool()> toggle_get;
    std::function<void(bool)> toggle_set;
};
/// Adds a row to the Party page. Call before Define() (BbGameMenu::PatchImage, at start). The
/// game shows at most RowsPerPage rows: the first six visible ones in `order`.
void AddPartyRow(PartyRow row);
int PartyRowCount();

/// The "Restart the game" button (runtime_restart in the game; tests count calls).
void SetRestartHandler(void (*restart)());

// ---- helpers, exposed for the test ----
int ChoiceFromPosition(u8 position, int count);
u8 ChoicePosition(int choice, int count);
/// The 0..10 position of `value` on an 11-entry table (the nearest entry).
u8 NearestPosition(const float* table, float value);
std::u16string Utf16(const char* utf8);
} // namespace BbGameMenu::Pages
