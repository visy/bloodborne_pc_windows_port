# The in-game party flow

Starting, joining and continuing a party (co-op) game from the game's own menus, without the
launcher's Party tab. Code: `gpu/shim/party/party_flow.{h,cpp}` (party.json, the action planner,
the relaunch file; unit-tested by `tests/test_party_flow.cpp`, target `party-flow-test`),
`party_flow_game.cpp` (game glue), `party_flow_rows.cpp` (the Party page rows),
`gpu/shim/bbport_game_menu.cpp` (the title entry), `run.bat` (the relaunch loop),
`src/probe.c` `runtime_relaunch`, `src/runtime_savepolicy.c` (the party saves).

## 1. What the player does

**First time (everyone makes a fresh character):**

1. Host: title screen → **Party** (right after *New Game*) → **New party game** (press left/right
   twice). The game restarts by itself (a few seconds) into the party game: online title, party
   saves (`user\savedata_party`, empty the first time), hosting. Choose **New Game**, make the
   character. The party code is on *System → Party* (*Copy party code*) and in
   `user\party_code.txt`; it stays the same in later sessions (§4).
2. Friends: title → **Party** → **Host address** (type or paste the host's party code or
   `host:port`, Enter) → **Password** (if the host set one) → **Join party game** (twice). The game
   restarts by itself, joined to the host; choose **New Game** and make the character.
3. Everyone plays the prologue on their own (`BB_PARTY_START=prologue_solo`, `party_start.h`);
   once a hunter has the first Dream behind them and both starting weapons, the party gathers
   automatically (the host's Beckoning Bell, the guests' Small Resonant Bell, as before).

**Later sessions:** the host: title → **Party** → **Continue party game** (twice): restart, the
party character loads on its own, hosting starts. Friends: title → **Party** → **Continue party
game** (their last role was *join*: the saved address and password are used) – restart, the party
character loads, the game joins the host and the summon follows by itself. *Continue* /
*New Game* on the title stay the single-player game.

**In the world (party game):** *System → Party* has the party status, *Copy party code*,
*Summon again* / *Rejoin the host*, *Leave the party*, **Host party** / **Join party** (the other
role: a restart, the character continues; or hosting / joining again after *Leave*) and
**Single player** (restart into the single-player game and saves). With a single-player character
loaded the Party page only explains that party games start on the title screen: no party action
can touch a single-player save.

Text entry is the window's text box (the same as the character name): type on the keyboard,
**Ctrl+V** pastes, Enter confirms, Esc (or Circle) cancels. The pad cannot type; the address and
password are typed once and remembered.

## 2. Design decision: a quick restart instead of switching in place

The party layer is chosen when the game starts, and the single-player game stays strictly as it
was (no sockets, no party hooks, the offline title, `user\savedata`). Switching a running game
between the two was examined and rejected; every boot-time choice below would need a live switch
inside a running game:

| Boot-time choice | Where | Why it cannot change later |
|---|---|---|
| The party patches (*Bells anywhere*, *Keep session on map reload*, *SOS sign timeout*, *Bells without Insight*) and their hash in the party version check | `scripts/patches.py` → `out/patches.bin`, applied to the image before any game code runs | Byte patches in code the game is executing; the hash must be identical on every player (party_runtime HELLO). |
| The online title (*Skip Online/Offline Choice (Online)*) | the same patches; FrpgNetMan+0xa | The online/offline decision exists only in the title's dialog: the step chain 0x1C36A80 (online sign-in 0x1C37B30) has exactly one caller, the title functor 0x1B48D40 (Ghidra refs), reached from the title constructor 0x1B384F0 and the title re-entry 0x1B4BC30 only. There is no in-game online switch to call after *Continue*. |
| Import routing (sceNp / sceNet / sceHttp → the party library) | `bbnet_resolve`, at link time | The game's import slots are bound once at load; a dormant party library in single-player would replace the offline stubs single-player has always used. |
| The save folder (`savedata` / `savedata_party`) | `runtime_savepolicy_dir()`, read once; the game mounts its saves at the title | The game caches the mounted save data and the slot list; a remount under a running title is not something the game expects. |
| The party runtime's hooks (`PartyInit`) | `bbgpu_patch_image`, while the image is writable | Installed before the game runs, byte-verified; installing later means patching live code on other threads. |

All of these are already right at boot when the process starts with `BB_PARTY` set, so the flow
**restarts the game automatically** with the new environment. The restart takes a few seconds
(the shader cache is warm) and needs no input: the new process continues the party character on
its own (the director's auto-Continue, as after a crash) or waits at the title for *New Game*. A
return-to-title plus an online *Continue* would still have needed the patches, the routing and the
save folder switched, so it gains nothing.

### How the restart works

1. The Party page's action (`party::flow::Run`) plans it (`PlanAction`): `BB_PARTY=host|join`,
   `BB_PARTY_SAVE=separate`, `BB_PARTY_MENU=1` (this run came from the menu: party.json supplies the
   address, password and host secret), `BB_PARTY_AUTOCONTINUE=0|1`, `BB_SKIP_NETWORK_CHOICE=online`,
   `BB_PARTY_NAME` / `BB_PARTY_PORT` when saved, and unsets `BB_PARTY_CODE`, `BB_PARTY_PASSWORD`,
   `BB_PARTY_RESTARTED`. *Single player* unsets the party variables.
2. It stores `user\party.json`, writes `user\party_relaunch.bat` (`set "K=V"` lines; values limited
   to `[A-Za-z0-9_.:@-]`), shuts the party link down cleanly (`bbnet_shutdown`: BYE, UPnP unmapped,
   the crash marker removed – so the next run is not taken for a crash restart) and ends the game
   with exit code **75** (`runtime_relaunch`, the same TerminateProcess path as the overlay's
   restart).
3. `run.bat` starts the game with `BB_RELAUNCH_LOOP=1`; on 75 it calls and deletes
   `party_relaunch.bat`, runs `scripts\patches.py` again (party patches, online title) and starts
   the game again in the same console – the launcher's log and the test harness keep following
   it. Without the loop (an older `run.bat`, `run.sh` on Linux) `runtime_relaunch` falls back to
   `runtime_restart`, which starts `run.bat` / `run.sh` with the environment already set in the
   process.

The launcher's Party tab keeps working unchanged: it sets `BB_PARTY` before the first start (the
party game from the beginning; the in-game rows then switch role or back to single player). Both
paths use the party saves; the menu path keeps its settings in party.json, the launcher path in
`launcher_settings.json` (a launcher run never reads party.json's password or code).

## 3. The title entry

The title's main menu is built by 0x1B4A3D0 (Ghidra): command items through
**AddCommandItem 0x1B4C9A0** (`(list, name+help texts, functor, 0)`, returns the list) – Continue
0x61F95 (only when the last slot is valid), Load Game 0x61F96, New Game 0x61F97, 0x61F98, and
online-only items – and page items through AddPageItem 0x1B4E2A0 (0x61F99, 0x61F9B). The game menu
already hooks AddPageItem (System pages); the flow adds a byte-verified detour on AddCommandItem
(prologue `55 48 89 E5 41 57`, the same 6 bytes the System hook moves): right after the item whose
name text is New Game's (the pointer the message lookup returned for 200/0x61F97), it adds the
port's **Party** page as a title page item, with the same page factory as *System → Party*. So the
title shows *Continue / Load Game / New Game / Party / ...*; *Party* opens the same page as
*System → Party*. `BB_PARTY_TITLE_ENTRY=0` leaves the title menu alone (*System → Party* still has
everything).

## 4. user\party.json

```json
{ "version": 1, "name": "Hunter", "role": "join", "host_address": "BBP1-....",
  "password_dpapi": "<base64>", "port": 0, "has_character": true, "slot": 2,
  "host_secret": "0123456789abcdef" }
```

* **password**: Windows DPAPI (`CryptProtectData`, this Windows user, fixed extra entropy) as
  `password_dpapi`; only this Windows user on this PC can read it back. A hand-written plain
  `"password"` is accepted and re-stored protected. Never in the environment, `launcher.log` or
  `party_relaunch.bat`. (On Linux it is stored in clear text; the page says "stored protected"
  only where it is.)
* **host_address**: a BBP1 party code or `host[:port]` (checked with `decode_party_code` when typed).
* **host_secret**: the host's party-code secret, reused by menu runs (`BB_PARTY_MENU=1`) so the
  code friends saved stays valid next session (as long as the public address and port stay the
  same; a `host:port` address with the password works whatever the secret).
* **has_character / slot**: set when a party character reaches the world in a party session
  (the game's last-used slot, `[[0x553B130]+0x50]+2000`); *Continue party game* is offered once it
  is true. The game's own *Continue* in the party process loads the party save's last-used slot,
  which is the party character (the party saves hold no other).
* **role**: the last party role; *Continue party game* uses it.

## 5. Saves: the party save is the only co-op path

* `runtime_savepolicy_dir()`: every party session (any non-empty `BB_PARTY`: host, join or a
  code) plays from `user\savedata_party`; `BB_PARTY_SAVE=shared` is ignored (logged). The
  single-player *Continue* / *New Game* always use `user\savedata`.
* The party save is mounted from the start of the party process (the save root is chosen before
  the title mounts the saves), so a party action never sees a solo character, and the
  progress / item sync (C2/C3) can only write into party characters.
* Party actions in the world are offered only with the party save loaded (`PartyActionsAllowed`).
* Backups of the party save at every party start and every N minutes stay on (save policy C5).
* The launcher's *Separate party saves* is always on (the checkbox is kept, disabled); the
  "copy single-player save" button and first-run offer remain for players who want their solo
  hunter in the party.
* The test harness copies its test saves into `savedata_party` too (`instances.py`), so
  `--party` runs keep their characters.

## 6. Tests

* `party-flow-test`: party.json round trip (DPAPI on Windows, plain fallback, bad values dropped),
  every action's plan in each context (solo title / solo world / party host / party guest),
  the relaunch file's escaping.
* `BB_PARTY_FLOW_TEST=<action>[@seconds]` (`new`, `join`, `continue`, `host`, `leave`, `solo`;
  `world:` prefix = in the world): runs that action once, as the row would – the in-game
  verification below uses it besides driving the menu with the pad.

Evidence of the in-game runs: §7.

## 7. Verification

(filled in by the runs below)
