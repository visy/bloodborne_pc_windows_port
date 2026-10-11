// SPDX-License-Identifier: GPL-3.0-or-later
// The party flow's rows on the game menu's Party page (System > Party, also on the title screen):
// registered through the page's action table (BbGameMenu::Pages::AddPartyRow); the page layout,
// the row kinds and the press handling are the game menu's (bbport_game_menu_pages.h). What the
// rows do is party_flow.h.
//
//   single-player process, title:  New party game, Join party game, Continue party game (once a
//                                  party character exists), Host address, Password
//   single-player process, world:  the status line explains how to start a party game
//   party process:                 Host party / Join party (the other role, or again after Leave),
//                                  Single player (back to the single-player game)
#include "party_flow.h"

#include "../bbport_game_menu_pages.h"

#include <string>

namespace party::flow {

namespace {

namespace P = BbGameMenu::Pages;
using Texts = P::Texts;
using Board = party::status::Board;

Texts T(const char* en, const char* ru) { return {en, ru}; }

/// A value column text of at most P::MaxValueLength units (ASCII here).
std::string Short(const std::string& s) {
    if (s.size() <= P::MaxValueLength) return s;
    return s.substr(0, P::MaxValueLength - 3) + "...";
}

/// The row's help, followed by the last action's outcome when there is one.
Texts WithMessage(Texts t) {
    const std::string m = LastMessage();
    if (!m.empty()) {
        t.en = m;
        t.ru = m;  // the outcome lines are English only (they also go to the log)
    }
    return t;
}

bool SoloTitle() {
    const Context c = CurrentContext();
    return !c.party_process && !c.in_world;
}
bool Party() { return CurrentContext().party_process; }

}  // namespace

void RegisterMenuRows() {
    using Row = P::PartyRow;
    if (!Enabled()) return;

    Row fresh;
    fresh.order = 12;
    fresh.name = [](const Board&) { return T("New party game", "Новая игра группой"); };
    fresh.value = [](const Board&) { return T("Host", "Хост"); };
    fresh.pressed = [](const Board&) { return T("Press again", "Нажмите ещё раз"); };
    fresh.help = [](const Board&) {
        return WithMessage(T("Twice: restart as the party's host with the party saves, then New Game",
                             "Дважды: перезапуск хостом группы на сохранениях группы, затем Новая игра"));
    };
    fresh.visible = [](const Board&) { return SoloTitle(); };
    fresh.press = Row::Press::Confirm;
    fresh.run = [](const Board&) { Run(Action::NewParty); };
    P::AddPartyRow(fresh);

    Row join;
    join.order = 14;
    join.name = [](const Board&) { return T("Join party game", "Присоединиться"); };
    join.value = [](const Board&) {
        const Saved s = Current();
        return s.host_address.empty() ? T("Set address first", "Сначала адрес") : T("Join", "Войти");
    };
    join.pressed = [](const Board&) { return T("Press again", "Нажмите ещё раз"); };
    join.help = [](const Board&) {
        const Saved s = Current();
        return WithMessage(s.has_character
                               ? T("Twice: restart and join the saved host with your party character",
                                   "Дважды: перезапуск и вход к хосту с персонажем группы")
                               : T("Twice: restart and join the saved host; then New Game for your hunter",
                                   "Дважды: перезапуск и вход к хосту; затем Новая игра"));
    };
    join.visible = [](const Board&) { return SoloTitle(); };
    join.press = Row::Press::Confirm;
    join.run = [](const Board&) { Run(Action::JoinParty); };
    P::AddPartyRow(join);

    Row cont;
    cont.order = 16;
    cont.name = [](const Board&) { return T("Continue party game", "Продолжить группой"); };
    cont.value = [](const Board&) {
        return Current().role == "join" ? T("As guest", "Гостем") : T("As host", "Хостом");
    };
    cont.pressed = [](const Board&) { return T("Press again", "Нажмите ещё раз"); };
    cont.help = [](const Board&) {
        return WithMessage(T("Twice: restart with the party saves and continue your party character",
                             "Дважды: перезапуск на сохранениях группы и продолжение персонажа группы"));
    };
    cont.visible = [](const Board&) { return SoloTitle() && Current().has_character; };
    cont.press = Row::Press::Confirm;
    cont.run = [](const Board&) { Run(Action::ContinueParty); };
    P::AddPartyRow(cont);

    Row addr;
    addr.order = 17;
    addr.name = [](const Board&) { return T("Host address", "Адрес хоста"); };
    addr.value = [](const Board&) {
        const Saved s = Current();
        return s.host_address.empty() ? T("Not set", "Не задан") : Texts{Short(s.host_address), Short(s.host_address)};
    };
    addr.pressed = addr.value;  // every press opens the text box: the value in both states
    addr.help = [](const Board&) {
        return WithMessage(T("Party code or host:port from the host; type it on the keyboard (Ctrl+V pastes)",
                             "Код группы или хост:порт; введите с клавиатуры (Ctrl+V вставляет)"));
    };
    addr.visible = [](const Board&) { return SoloTitle(); };
    addr.press = Row::Press::Once;
    addr.run = [](const Board&) { BeginEdit(Field::Address); };
    P::AddPartyRow(addr);

    Row pass;
    pass.order = 18;
    pass.name = [](const Board&) { return T("Password", "Пароль"); };
    pass.value = [](const Board&) { return Current().password.empty() ? T("None", "Нет") : T("Set", "Задан"); };
    pass.pressed = pass.value;  // every press opens the text box: the value in both states
    pass.help = [](const Board&) {
        return WithMessage(T("The party's password (host and guests the same); saved protected for this Windows user",
                             "Пароль группы (у всех одинаковый); хранится защищённым для этого пользователя"));
    };
    pass.visible = [](const Board&) { return SoloTitle(); };
    pass.press = Row::Press::Once;
    pass.run = [](const Board&) { BeginEdit(Field::Password); };
    P::AddPartyRow(pass);

    Row name;
    name.order = 19;
    name.name = [](const Board&) { return T("Party name", "Имя в группе"); };
    name.value = [](const Board&) {
        const Saved s = Current();
        return s.name.empty() ? T("Automatic", "Автоматически") : Texts{Short(s.name), Short(s.name)};
    };
    name.pressed = name.value;  // every press opens the text box: the value in both states
    name.help = [](const Board&) {
        return WithMessage(T("Your name in the party (1-16 of A-Z a-z 0-9 _ -); each player needs their own",
                             "Ваше имя в группе (1-16 из A-Z a-z 0-9 _ -); у каждого своё"));
    };
    name.visible = [](const Board&) { return SoloTitle(); };
    name.press = Row::Press::Once;
    name.run = [](const Board&) { BeginEdit(Field::Name); };
    P::AddPartyRow(name);

    Row host;
    host.order = 25;
    host.name = [](const Board&) { return T("Host party", "Стать хостом"); };
    host.value = [](const Board&) { return CurrentContext().host ? T("Host again", "Снова хостом") : T("Switch", "Сменить"); };
    host.pressed = [](const Board&) { return T("Press again", "Нажмите ещё раз"); };
    host.help = [](const Board&) {
        return WithMessage(CurrentContext().host
                               ? T("Twice: host the party again", "Дважды: снова открыть группу")
                               : T("Twice: restart as the host (your party character continues)",
                                   "Дважды: перезапуск хостом (персонаж группы продолжает)"));
    };
    host.visible = [](const Board&) {
        const Context c = CurrentContext();
        return c.party_process && (!c.host || !c.link_active);
    };
    host.press = Row::Press::Confirm;
    host.run = [](const Board&) { Run(Action::HostParty); };
    P::AddPartyRow(host);

    Row guest;
    guest.order = 26;
    guest.name = [](const Board&) { return T("Join party", "Войти в группу"); };
    guest.value = [](const Board&) { return CurrentContext().host ? T("Switch", "Сменить") : T("Join again", "Снова войти"); };
    guest.pressed = [](const Board&) { return T("Press again", "Нажмите ещё раз"); };
    guest.help = [](const Board&) {
        const Saved s = Current();
        if (s.host_address.empty())
            return WithMessage(T("No host address saved: set it on the title screen (System > Party)",
                                 "Адрес хоста не задан: укажите его на титульном экране"));
        return WithMessage(Texts{"Twice: join " + s.host_address, "Дважды: войти к " + s.host_address});
    };
    guest.visible = [](const Board&) {
        const Context c = CurrentContext();
        return c.party_process && (c.host || !c.link_active);
    };
    guest.press = Row::Press::Confirm;
    guest.run = [](const Board&) { Run(Action::JoinParty); };
    P::AddPartyRow(guest);

    Row solo;
    solo.order = 45;
    solo.name = [](const Board&) { return T("Single player", "Одиночная игра"); };
    solo.value = [](const Board&) { return T("Switch", "Перейти"); };
    solo.pressed = [](const Board&) { return T("Press again", "Нажмите ещё раз"); };
    solo.help = [](const Board&) {
        return WithMessage(T("Twice: leave and restart into the single-player game and saves",
                             "Дважды: выйти и перезапустить одиночную игру и её сохранения"));
    };
    solo.visible = [](const Board&) { return Party(); };
    solo.press = Row::Press::Confirm;
    solo.run = [](const Board&) { Run(Action::SinglePlayer); };
    P::AddPartyRow(solo);
}

}  // namespace party::flow
