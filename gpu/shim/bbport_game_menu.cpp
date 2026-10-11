// SPDX-License-Identifier: GPL-2.0-or-later
// The port's pages in the game's System menu: the glue to the game's menu code. What the pages
// hold is in bbport_game_menu_pages.{h,cpp}; this file only builds them with the game's rows.
//
// Row / page API (bbport_game_menu_pages.h):
//  - Pages::All(): the pages, in System menu order (after "Screen/Sound"); each Page has a
//    name, a help text and at most RowsPerPage Rows (more may be defined when some are hidden:
//    Row::visible is checked when the page opens; the first RowsPerPage visible rows are shown).
//  - Row: a field (index of the value it edits), name / help text ids, value text ids
//    (`choices`) and a Kind: Toggle (PairRow: two values, left/right), Slider (SliderRow 0..10),
//    Steps (SliderRow standing for a few choices; the help line says which positions), List
//    (ListRow pop-up; only as a page's last row, it opens over the rows below).
//  - Texts: ids from Pages::IdBase, answered by LookupHook in the game's language. Run-time
//    ("dynamic") texts are fixed buffers rewritten in place by Pages::RefreshDynamic (party
//    state, member rows), so a row can show live values.
//  - Values: Pages::Value / Byte (what the rows bind) and DefaultValue / DefaultByte (the
//    game's "Default"). Pages::Load() fills them from bbport_settings when a page opens (Rows<N>),
//    Pages::Poll() (each presented frame, BbGameMenu::Poll) applies changes, saves bbport.ini and
//    runs buttons (a toggle row whose value change is an action; Confirm buttons on the second
//    press).
//  - Party page: an action table. Pages::AddPartyRow(PartyRow) before PatchImage adds a row
//    (order, texts made from party::status::Board, Press None / Once / Confirm, run(board), or a
//    settings toggle); built-in rows use the same table (status 10, copy code 20, rejoin 30,
//    leave 40; the notices switch and the last event are on Party members).
//  - New pages: add them in Pages::DefineAll (MaxPages below caps the System items).
#include "bbport_game_menu.h"
#include "party/party_flow.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <malloc.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "bbport_game_menu_pages.h"
#include "bbport_settings.h"

extern "C" void* runtime_low_map(size_t size, int prot); // src/runtime_memory.c
extern "C" void runtime_restart(void);                  // src/probe.c

// The game's code is called, and calls the hooks below, with the System V ABI (the PS4's): the
// default on Linux, said explicitly for Windows.
#define BB_SYSV __attribute__((sysv_abi))

namespace BbGameMenu {
namespace {

// ---- Platform (bbport: Windows port) ----
void* AlignedAlloc(std::size_t size) {
#ifdef _WIN32
    return _aligned_malloc(size, 16); // no C11 aligned_alloc in the Windows C runtime
#else
    return std::aligned_alloc(16, size);
#endif
}
void AlignedFree(void* p) {
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}
std::size_t PageSize() {
#ifdef _WIN32
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return std::size_t(info.dwPageSize);
#else
    return std::size_t(sysconf(_SC_PAGESIZE));
#endif
}

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// The game's menu code (eboot 1.09 vaddrs; found from the System menu builder 0x1bb3ad0).
constexpr u64 MsgLookup = 0xf6f340;      ///< (repository, table, category, id) -> UTF-16 text
constexpr u64 AddPageItem = 0x1b4e2a0;   ///< (menu, name+help texts, page factory, out)
constexpr u64 MakeText = 0x1ae8cc0;      ///< (out 0x40 bytes, category, id): a text entry
constexpr u64 FactoryToFunction = 0x1b6ed90; ///< ({vtable, fn}, out std::function) -> its __f_
constexpr u64 CreatePage = 0x1bb4c70;    ///< (out, arg, content std::function): an options page
constexpr u64 OpenPage = 0x1b20900;      ///< (a, b, layout name, rows fn, 0, 0): the page's content
constexpr u64 ListPush = 0x1b1bbd0;      ///< (value list, {u32 value, text}): up to 32 values
constexpr u64 ListRow = 0x1b78ab0;   ///< (page, texts, int* value, value list, int* default): pop-up list
constexpr u64 PairPush = 0x1b2c200;  ///< (two-value list, {u8 value, text})
constexpr u64 PairRow = 0x1b2a100;   ///< (page, texts, u8* value, two-value list, u8* default): left/right
constexpr u64 SliderRow = 0x1b2ac00; ///< (page, texts, u8* value 0..10, u8* default)
constexpr u64 FactoryVtable = 0x533d120; ///< a System item's {vtable, page factory fn}
constexpr u64 ContentVtable = 0x5343880; ///< an options page's {vtable, content fn}
constexpr u64 ControlsLayout = 0x4934065; ///< "ControllSetting": six plain rows, the layout used
constexpr u32 TextName = 0xc8, TextHelp = 0xc9; // SP_menu text / SP_one-line help
constexpr u32 ScreenSoundItem = 110001;         // "Screen/Sound" in the System menu: ours follow it
constexpr u32 ControlsTitle = 113020;           // the ControllSetting layout's title ("Controls")

const u8 MsgLookupPrologue[] = {0x89, 0xf0, 0x48, 0x8b, 0x77, 0x08, 0x48, 0x8b, 0x34, 0xc6};
const u8 AddPageItemPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
// The title screen's main menu (builder 0x1b4a3d0, docs/party/ingame_flow.md §3): its command items
// (Continue 0x61f95, Load Game 0x61f96, New Game 0x61f97, ...) go through AddCommandItem; the
// party flow adds the Party page as a title item right after New Game (BB_PARTY_TITLE_ENTRY=0: not).
constexpr u64 AddCommandItem = 0x1b4c9a0; ///< (command list, name+help texts, functor, 0) -> the list
constexpr u32 TitleNewGame = 0x61f97;
const u8 AddCommandItemPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};

u64 image_base = 0;
template <typename F>
F Game(u64 va) {
    return reinterpret_cast<F>(image_base + va);
}

// ---- Texts: the port's ids (Pages::IdBase up), looked up by the game like its own messages. ----
namespace P = BbGameMenu::Pages;
using P::Kind;
using P::Row;

/// The game's own language decides (its "Screen/Sound" text in Cyrillic: Russian, else English);
/// the port's menu language until the game has looked that text up.
enum class GameLanguage { Unknown, English, Russian };
std::atomic<GameLanguage> game_language{GameLanguage::Unknown};

const char16_t* TextOf(u32 id) {
    const GameLanguage game = game_language.load();
    const bool russian = game == GameLanguage::Unknown
                             ? BbSettings::Get().menu_language == BbSettings::MenuLanguage::Russian
                             : game == GameLanguage::Russian;
    return P::Text(id, russian);
}

// ---- The game's structures, as its own code builds them. ----
constexpr std::size_t TextSize = 0x40;     // text entry (MakeText): text pointer, string, flag
constexpr std::size_t ValueSize = 0x48;    // {u32 value; text}
constexpr std::size_t ListSize = 0x920;    // 32 values + count at +0x908 (+ alignment slack)
constexpr std::size_t PairListSize = 0xb0; // 2 values + count at +0x98 (+ alignment slack)
constexpr std::size_t ListCountOffset = 0x908, PairCountOffset = 0x98;
constexpr std::size_t FunctionSize = 0x28; // std::function: 32-byte buffer, __f_ at +0x20

/// Destroys a std::function the way the game's code does (its __f_ at +0x20).
void DestroyFunction(u8* f) {
    void* impl = *reinterpret_cast<void**>(f + 0x20);
    if (!impl) {
        return;
    }
    using Destroy = void(BB_SYSV*)(void*, int);
    Destroy destroy = (*reinterpret_cast<Destroy**>(impl))[4];
    destroy(impl, impl != f);
    *reinterpret_cast<void**>(f + 0x20) = nullptr;
}

/// Frees a text entry's string the way the game's code does (heap buffer once capacity >= 8).
void DestroyText(u8* text) {
    if (*reinterpret_cast<u64*>(text + 0x28) >= 8) {
        void* allocator = *reinterpret_cast<void**>(text + 0x30);
        using Free = void(BB_SYSV*)(void*, void*);
        (*reinterpret_cast<Free**>(allocator))[0x70 / 8](allocator, *reinterpret_cast<void**>(text + 0x10));
    }
    *reinterpret_cast<u64*>(text + 0x28) = 7;
    *reinterpret_cast<u64*>(text + 0x20) = 0;
    *reinterpret_cast<char16_t*>(text + 0x10) = 0;
}
void DestroyTexts(u8* pair) {
    DestroyText(pair);
    DestroyText(pair + TextSize);
}
/// A value list's entries (count at `count_offset`), after the row has copied them.
void DestroyValues(u8* list, std::size_t count_offset) {
    const u64 count = *reinterpret_cast<u64*>(list + count_offset);
    for (u64 i = 0; i < count; ++i) {
        DestroyText(list + i * ValueSize + 8);
    }
}

void MakeTexts(u8* pair, u32 name, u32 help) {
    using Make = void*(BB_SYSV*)(void*, u32, u32);
    Game<Make>(MakeText)(pair, TextName, name);
    Game<Make>(MakeText)(pair + TextSize, TextHelp, help);
}

void AddRows(void* page, const std::vector<Row>& all) {
    // Rows shown now (the party's member rows: one per member).
    std::vector<Row> rows;
    for (const Row& row : all) {
        if (!row.visible || row.visible()) rows.push_back(row);
    }
    if (rows.empty()) rows.push_back(all.front());
    if (rows.size() > P::RowsPerPage) rows.resize(P::RowsPerPage); // the layout's rows
    using Push = void(BB_SYSV*)(void*, void*);
    using AddRow = void(BB_SYSV*)(void*, void*, int*, void*, const int*);
    using Make = void*(BB_SYSV*)(void*, u32, u32);
    for (const Row& row : rows) {
        alignas(16) u8 pair[2 * TextSize] = {};
        MakeTexts(pair, row.name, row.help);
        if (row.kind == Kind::Slider || row.kind == Kind::Steps) {
            using Slider = void(BB_SYSV*)(void*, void*, u8*, const u8*);
            Game<Slider>(SliderRow)(page, pair, P::Byte(row.field), P::DefaultByte(row.field));
            DestroyTexts(pair);
            continue;
        }
        if (row.kind == Kind::Toggle) {
            using PairRowFn = void(BB_SYSV*)(void*, void*, u8*, void*, const u8*);
            u8* two = static_cast<u8*>(AlignedAlloc(PairListSize));
            std::memset(two, 0, PairListSize);
            for (std::size_t i = 0; i < 2; ++i) {
                alignas(16) u8 value[ValueSize] = {};
                value[0] = u8(i);
                Game<Make>(MakeText)(value + 8, TextName, row.choices[i]);
                Game<Push>(PairPush)(two, value);
                DestroyText(value + 8);
            }
            Game<PairRowFn>(PairRow)(page, pair, P::Byte(row.field), two, P::DefaultByte(row.field));
            DestroyValues(two, PairCountOffset);
            AlignedFree(two);
            DestroyTexts(pair);
            continue;
        }
        u8* list = static_cast<u8*>(AlignedAlloc(ListSize));
        std::memset(list, 0, ListSize);
        for (std::size_t i = 0; i < row.choices.size(); ++i) {
            if (!P::ChoiceAvailable(row.field, int(i))) {
                continue; // the list's values are the upscaler numbers, not positions
            }
            alignas(16) u8 value[ValueSize] = {};
            *reinterpret_cast<u32*>(value) = u32(i);
            Game<Make>(MakeText)(value + 8, TextName, row.choices[i]);
            Game<Push>(ListPush)(list, value);
            DestroyText(value + 8);
        }
        Game<AddRow>(ListRow)(page, pair, P::Value(row.field), list, P::DefaultValue(row.field));
        DestroyValues(list, ListCountOffset);
        AlignedFree(list);
        DestroyTexts(pair);
    }
}

std::atomic<const char16_t*> title_override{nullptr}; // while the game builds one of our pages (its menu thread)
// Called by the game's menu code (guest threads, System V ABI like the game), one set per page.
bool Trace();
template <int N>
BB_SYSV void Rows(void* page, void*) {
    if (Trace()) {
        const char16_t* name = P::Text(P::All()[N].name, false);
        std::string ascii;
        for (const char16_t* c = name; c && *c; ++c) ascii.push_back(*c < 128 ? char(*c) : '?');
        std::printf("Game menu: page %d %s open\n", N, ascii.c_str());
    }
    P::Load();
    AddRows(page, P::All()[N].rows);
}
template <int N>
BB_SYSV void* Content(void* a, void* b) {
    using Open = void*(BB_SYSV*)(void*, void*, const char*, void*, u64, u64);
    title_override = nullptr; // the title was looked up just before
    return Game<Open>(OpenPage)(a, b, Game<const char*>(ControlsLayout),
                                reinterpret_cast<void*>(&Rows<N>), 0, 0);
}
void* Factory(void* out, void* arg, void* content, const char16_t* title) {
    using Create = void*(BB_SYSV*)(void*, void*, void*);
    alignas(16) u8 function[FunctionSize] = {};
    *reinterpret_cast<u64*>(function) = image_base + ContentVtable;
    *reinterpret_cast<void**>(function + 8) = content;
    *reinterpret_cast<void**>(function + 0x20) = function;
    // The page is built later (the game's menu task): its layout title is looked up right before
    // the content function runs, which takes the override back.
    title_override = title;
    Game<Create>(CreatePage)(out, arg, function);
    DestroyFunction(function);
    return out;
}
template <int N>
BB_SYSV void* PageFactory(void* out, void* arg) {
    return Factory(out, arg, reinterpret_cast<void*>(&Content<N>), TextOf(P::All()[N].name));
}
constexpr std::size_t MaxPages = 12;
template <std::size_t... I>
std::array<void*, sizeof...(I)> MakeFactories(std::index_sequence<I...>) {
    return {reinterpret_cast<void*>(&PageFactory<int(I)>)...};
}
const std::array<void*, MaxPages> factories = MakeFactories(std::make_index_sequence<MaxPages>{});

// ---- Hooks ----
using LookupFn = const char16_t*(BB_SYSV*)(void*, u32, u32, u32);
using AddFn = void*(BB_SYSV*)(void*, void*, void*, void*);
LookupFn lookup_original = nullptr;
AddFn add_original = nullptr;
AddFn command_original = nullptr;
std::atomic<const void*> screen_sound_text{nullptr};
std::atomic<const void*> new_game_text{nullptr}; // the title's "New Game" text, as looked up

/// BB_GAME_MENU_TRACE=1: menu events in the log for scripted tests (tools driving the menu with
/// BB_PAD_FILE wait for them): "Game menu: new text", "Game menu: System menu built",
/// "Game menu: page N <name> open".
bool Trace() {
    static const bool trace = [] {
        const char* env = std::getenv("BB_GAME_MENU_TRACE");
        return env && env[0] == '1';
    }();
    return trace;
}

// Screen changes (TextActivity): a message id looked up that was not looked up in the last second.
std::atomic<u64> fresh_seq{0};
std::atomic<std::int64_t> fresh_ms{0};
struct SeenText {
    std::atomic<u64> key{0}; // category << 32 | id, + 1 (0 = empty)
    std::atomic<std::int64_t> ms{0};
};
SeenText seen_texts[4096];
struct SeenId {
    std::atomic<u32> id{0};
    std::atomic<std::int64_t> ms{0};
};
SeenId seen_ids[4096]; // by id only (LastLookupMs)
void NoteLookup(u32 category, u32 id) {
    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    {
        SeenId& e = seen_ids[(u64(id) * 0x9E3779B97F4A7C15ull) >> 52];
        e.id.store(id, std::memory_order_relaxed);
        e.ms.store(now, std::memory_order_relaxed);
    }
    const u64 key = (u64(category) << 32 | id) + 1;
    SeenText& s = seen_texts[(key * 0x9E3779B97F4A7C15ull) >> 52];
    if (s.key.load(std::memory_order_relaxed) == key && now - s.ms.load(std::memory_order_relaxed) < 1000) {
        s.ms.store(now, std::memory_order_relaxed);
        return;
    }
    s.key.store(key, std::memory_order_relaxed);
    s.ms.store(now, std::memory_order_relaxed);
    fresh_ms.store(now, std::memory_order_relaxed);
    fresh_seq.fetch_add(1, std::memory_order_release);
    if (Trace()) {
        std::printf("Game menu: new text %#x/%u\n", category, id);
    }
}

BB_SYSV const char16_t* LookupHook(void* repository, u32 table, u32 category, u32 id) {
    if (id >= P::IdBase && id < P::IdBase + P::TextCount()) {
        // BB_GAME_MENU_TRACE=1: how often the game asks for our texts (per 2 s).
        static const bool count = [] {
            const char* env = std::getenv("BB_GAME_MENU_TRACE");
            return env && env[0] == '1';
        }();
        if (count) {
            static std::atomic<u64> lookups{0};
            static std::atomic<long long> printed{0};
            ++lookups;
            const long long now = std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count();
            if (now - printed.load() >= 2) {
                printed = now;
                std::printf("Game menu: %llu lookups of the port's texts in 2 s (last id %u)\n",
                            (unsigned long long)lookups.exchange(0), id - P::IdBase);
            }
        }
        return TextOf(id);
    }
    if (title_override && category == TextName && id == ControlsTitle) {
        return title_override;
    }
    const char16_t* text = lookup_original(repository, table, category, id);
    NoteLookup(category, id);
    static const bool trace = [] {
        const char* env = std::getenv("BB_GAME_MENU_TRACE");
        return env && env[0] == '1';
    }();
    if (trace && id >= 110000 && id < 120000) {
        std::printf("Game menu: text %#x/%u%s from %#llx\n", category, id, title_override ? " (our page)" : "",
                    (unsigned long long)(reinterpret_cast<u64>(__builtin_return_address(0)) - image_base));
    }
    if (trace && id >= 401000 && id < 402000) {
        std::printf("Game menu: title text %#x/%#x from %#llx\n", category, id,
                    (unsigned long long)(reinterpret_cast<u64>(__builtin_return_address(0)) - image_base));
    }
    if (category == TextName && id == TitleNewGame && text) {
        new_game_text = text;
    }
    if (category == TextName && id == ScreenSoundItem && text) {
        screen_sound_text = text;
        bool cyrillic = false;
        for (const char16_t* c = text; *c; ++c) {
            cyrillic = cyrillic || (*c >= 0x400 && *c < 0x500);
        }
        game_language = cyrillic ? GameLanguage::Russian : GameLanguage::English;
    }
    return text;
}

void AddItem(void* menu, u32 name, u32 help, void* factory) {
    using ToFunction = void*(BB_SYSV*)(void*, void*);
    alignas(16) u8 pair[2 * TextSize] = {};
    MakeTexts(pair, name, help);
    alignas(16) u8 source[FunctionSize] = {};
    *reinterpret_cast<u64*>(source) = image_base + FactoryVtable;
    *reinterpret_cast<void**>(source + 8) = factory;
    *reinterpret_cast<void**>(source + 0x20) = source;
    alignas(16) u8 function[FunctionSize] = {};
    *reinterpret_cast<void**>(function + 0x20) = Game<ToFunction>(FactoryToFunction)(source, function);
    alignas(16) u8 out[16] = {};
    add_original(menu, pair, function, out);
    DestroyFunction(function);
    DestroyTexts(pair);
}

BB_SYSV void* AddHook(void* menu, void* pair, void* function, void* out) {
    void* result = add_original(menu, pair, function, out);
    const void* screen_sound = screen_sound_text.load();
    if (screen_sound && *reinterpret_cast<const void* const*>(pair) == screen_sound) {
        const auto& pages = P::All();
        for (std::size_t i = 0; i < pages.size() && i < MaxPages; ++i) {
            AddItem(menu, pages[i].name, pages[i].help, factories[i]);
        }
        if (Trace()) {
            std::printf("Game menu: System menu built\n");
        }
    }
    return result;
}

/// The Party page's index in P::All() (-1 when there is none).
int PartyPageIndex() {
    const auto& pages = P::All();
    for (std::size_t i = 0; i < pages.size() && i < MaxPages; ++i) {
        const char16_t* name = P::Text(pages[i].name, false);
        if (name && std::u16string(name) == u"Party") return int(i);
    }
    return -1;
}

/// The title's main menu: after its New Game item, the Party page as a title item.
BB_SYSV void* CommandHook(void* list, void* pair, void* functor, void* x) {
    void* result = command_original(list, pair, functor, x);
    const void* new_game = new_game_text.load();
    if (new_game && *reinterpret_cast<const void* const*>(pair) == new_game) {
        static const int party = PartyPageIndex();
        if (party >= 0) {
            const auto& page = P::All()[std::size_t(party)];
            AddItem(list, page.name, page.help, factories[std::size_t(party)]);
        }
    }
    return result;
}

/// `jmp hook` at the function (via a near thunk); its moved prologue + `jmp` back is the original.
bool Detour(unsigned char* image, u64 va, const u8* prologue, std::size_t length, void* hook,
            void** original, u8*& cursor) {
    u8* site = image + va;
    if (std::memcmp(site, prologue, length) != 0) {
        std::printf("Game menu: unexpected code at %#llx, not hooked\n", (unsigned long long)va);
        return false;
    }
    u8* trampoline = cursor;
    std::memcpy(trampoline, prologue, length);
    trampoline[length] = 0xe9;
    const std::int64_t back = std::int64_t(site + length) - std::int64_t(trampoline + length + 5);
    const std::int32_t back32 = std::int32_t(back);
    std::memcpy(trampoline + length + 1, &back32, 4);
    u8* thunk = trampoline + 32;
    thunk[0] = 0x48;
    thunk[1] = 0xb8; // movabs rax, hook
    std::memcpy(thunk + 2, &hook, 8);
    thunk[10] = 0xff;
    thunk[11] = 0xe0; // jmp rax
    const std::int64_t to = std::int64_t(thunk) - std::int64_t(site + 5);
    if (back != back32 || to != std::int32_t(to)) {
        std::printf("Game menu: stubs out of jump range, not hooked\n");
        return false;
    }
    const std::int32_t to32 = std::int32_t(to);
    site[0] = 0xe9;
    std::memcpy(site + 1, &to32, 4);
    for (std::size_t i = 5; i < length; ++i) {
        site[i] = 0xcc;
    }
    *original = trampoline;
    cursor += 64;
    return true;
}

/// A mapping within +-2 GiB of the image, for rel32 jumps both ways.
u8* MapNear(unsigned char* image, u64 image_size, std::size_t size) {
    const u64 base = reinterpret_cast<u64>(image);
#ifdef _WIN32
    // The image is the first block of the runtime's low guest area (0x800000000 up, all of it
    // reserved for the guest): the next low block is right after it, within rel32 reach.
    if (void* p = runtime_low_map(size, 3 /* read | write; VirtualProtect later */)) {
        const u64 at = reinterpret_cast<u64>(p);
        if (at + size <= base + (2ull << 30) - (1ull << 20) && at >= base) {
            return static_cast<u8*>(p);
        }
    }
    // Otherwise walk the actual free regions instead of guessing addresses: the guest address space
    // reservation and the loader leave few fixed hints free. Every byte of the stubs must be
    // within rel32 reach of every byte of the image.
    {
        constexpr u64 reach = (2ull << 30) - (1ull << 20);
        constexpr u64 granularity = 0x10000;
        const u64 lo = base + image_size > reach ? base + image_size - reach : granularity;
        const u64 hi = base + reach - size;
        MEMORY_BASIC_INFORMATION info;
        for (u64 at = lo; at < hi && VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info));) {
            const u64 region = reinterpret_cast<u64>(info.BaseAddress);
            const u64 region_end = region + info.RegionSize;
            if (info.State == MEM_FREE) {
                const u64 start = (std::max(region, at) + granularity - 1) & ~(granularity - 1);
                if (start + size <= region_end && start < hi) {
                    if (void* p = VirtualAlloc(reinterpret_cast<void*>(start), size,
                                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
                        return static_cast<u8*>(p);
                    }
                }
            }
            if (region_end <= at) {
                break;
            }
            at = region_end;
        }
    }
#endif
    for (u64 k = 1; k <= 64; ++k) {
        for (const u64 hint : {base - k * (24ull << 20), base + image_size + k * (24ull << 20)}) {
#ifdef _WIN32
            // Reservations start on the 64 KiB allocation granularity.
            void* p = VirtualAlloc(reinterpret_cast<void*>(hint & ~u64(0xffff)), size,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (p) {
                return static_cast<u8*>(p);
            }
#else
            void* p = mmap(reinterpret_cast<void*>(hint), size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p != MAP_FAILED) {
                return static_cast<u8*>(p);
            }
#endif
        }
    }
    return nullptr;
}

} // namespace

void PatchImage(unsigned char* image, std::uint64_t size) {
    if (const char* env = std::getenv("BB_GAME_MENU"); env && env[0] == '0') {
        return;
    }
    image_base = reinterpret_cast<u64>(image);
    const std::size_t page = PageSize();
    u8* stubs = MapNear(image, size, page);
    if (!stubs) {
        std::printf("Game menu: no memory near the image (%p, %llu MiB), the port's pages are off\n",
                    static_cast<void*>(image), static_cast<unsigned long long>(size >> 20));
        return;
    }
    party::flow::RegisterMenuRows(); // the party flow's rows on the Party page (party_flow_rows.cpp)
    P::Define();
    P::SetRestartHandler(&runtime_restart);
    if (P::All().size() > MaxPages) {
        std::printf("Game menu: %zu pages, only %zu shown\n", P::All().size(), MaxPages);
    }
    u8* cursor = stubs;
    // The lookup first: the menu hook only acts once it has seen the Screen/Sound text.
    const bool ok =
        Detour(image, MsgLookup, MsgLookupPrologue, sizeof(MsgLookupPrologue),
               reinterpret_cast<void*>(&LookupHook), reinterpret_cast<void**>(&lookup_original), cursor) &&
        Detour(image, AddPageItem, AddPageItemPrologue, sizeof(AddPageItemPrologue),
               reinterpret_cast<void*>(&AddHook), reinterpret_cast<void**>(&add_original), cursor);
    // The title entry (party flow): needs the Party page and AddPageItem's original.
    const char* title_entry = std::getenv("BB_PARTY_TITLE_ENTRY");
    if (ok && (!title_entry || title_entry[0] != '0') && PartyPageIndex() >= 0 &&
        Detour(image, AddCommandItem, AddCommandItemPrologue, sizeof(AddCommandItemPrologue),
               reinterpret_cast<void*>(&CommandHook), reinterpret_cast<void**>(&command_original), cursor)) {
        std::printf("Game menu: the Party page is also on the title screen (after New Game)\n");
    }
#ifdef _WIN32
    DWORD old_protect = 0;
    VirtualProtect(stubs, page, PAGE_EXECUTE_READ, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), stubs, page);
#else
    mprotect(stubs, page, PROT_READ | PROT_EXEC);
#endif
    std::printf("Game menu: %s (%zu pages in System)\n", ok ? "the port's pages are in" : "hooks failed, off",
                P::All().size());
}


std::int64_t LastLookupMs(std::uint32_t id) {
    const SeenId& e = seen_ids[(u64(id) * 0x9E3779B97F4A7C15ull) >> 52];
    return e.id.load(std::memory_order_relaxed) == id ? e.ms.load(std::memory_order_relaxed) : 0;
}

int RecentLookups(std::int64_t within_ms, std::uint32_t* out, int max) {
    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    int n = 0;
    for (const SeenId& e : seen_ids) {
        const std::int64_t ms = e.ms.load(std::memory_order_relaxed);
        if (ms && now - ms < within_ms && n < max) {
            out[n++] = e.id.load(std::memory_order_relaxed);
        }
    }
    return n;
}

void TextActivity(std::uint64_t* seq, std::int64_t* last_ms) {
    *seq = fresh_seq.load(std::memory_order_acquire);
    *last_ms = fresh_ms.load(std::memory_order_relaxed);
}

void Poll() {
    party::flow::Tick(); // the party flow: text box, party character note (party_flow.h)
    P::Poll(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
}

} // namespace BbGameMenu
