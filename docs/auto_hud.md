# Auto-hide HUD (0.7.8, CL-7d51fe)

Setting **Auto-hide HUD**, off by default: `System > Game HUD`, the launcher (Advanced game settings), `auto_hide_hud` in
`bbport.ini`, `BB_AUTO_HUD=0/1` over it. Live; local to this player (solo and party alike). Code:
`gpu/shim/party/auto_hud_rules.h` (the rules, pure), `auto_hud.cpp` (the game side), unit test `tests/test_auto_hud.cpp`
(`auto-hud-test`).

## What Elden Ring does (the model)

Sources: the Elden Ring wiki (HUD page: with the HUD display setting on *Auto* "the HUD will be automatically hidden,
except for the compass"; the Y / triangle button shows it for a moment), the Nightreign manual (Auto hides everything but
the compass and the player level), and player and mod descriptions of the default behaviour (the HP and FP bars come back
when damage is taken or spells are used, and in combat or while locked on; a stamina bar appears with use). No official
source gives the timers; the values below are ours, tuned by feel.

| Elden Ring element | Auto behaviour | Bloodborne counterpart | Here |
|---|---|---|---|
| HP bar | hidden idle; shown on damage / healing, in combat, locked on | HP bar | hidden idle; HP changed -> shown 5 s; HP below half -> kept |
| FP bar | shown on spell use | (none) | - |
| Stamina bar | shown while used / refilling | stamina bar | stamina changed -> shown 2 s after the last change |
| Equipment slots (quick items, weapons, flask counts) | shown when you switch | quick-item / weapon / bullet slots, vial counters | equipment or the selected quick item changed -> 4 s |
| Rune counter | shown when runes change | Blood Echoes and Insight | change -> 4 s |
| Compass | always shown | (none; our party panel's compass is our overlay and is untouched) | - |
| Boss bar | not affected | `BossTop` | not affected (another clip) |
| Lock-on cursor, prompts, item notices, subtitles | not affected | other clips of the in-game top menu | not affected |
| Combat / enemy nearby | HUD shown | lock-on, or a living enemy within 8 m | shown while it lasts, +6 s |
| Menus | HUD shown | any in-game menu / dialog | shown while open, +1 s |
| Y / triangle "peek" | temporary show | no free button | `BB_AUTO_HUD_TEST=S:peek` only (test aid) |

Fades are by the wall clock: in 0.2 s, out over 1.0 s (the same at any frame rate). Entering the world shows the HUD for
4 s. The tuning is `Tuning` in `auto_hud_rules.h`.

Differences: Bloodborne has no FP and no compass; "aggressive enemy nearby" is approximated by any living enemy within 8 m
(the game's aggro state is not read); the rune counter (Blood Echoes) and Insight are part of the same group as the bars in
Bloodborne's HUD, so they appear and leave with them.

## How it is done

The game's in-game HUD view (`0x1BF41C0`, the "IngameTop" movie) holds the Scaleform clip `PlayerHUD` (HP and stamina
gauges, the slots, vials, bullets, echoes and Insight) as a value wrapper at view +0x60 (vtable `0x5399F90`; +0x38 object
interface, +0x40 type, +0x48 display object). Its object interface's `GetDisplayInfo` / `SetDisplayInfo` (vtable +0xE0 /
+0xE8, as `0x1ACB760` uses them for visibility) set the clip's alpha only. The view is found by a prologue hook on its
constructor. Nothing else of the HUD is touched, the game's own show / hide logic (menus, cutscenes) stays as is. Switching
the setting off restores alpha 1.

Log lines: `Auto HUD: ...` (installed, the alpha read from the clip once, every shown / hidden change with the reason bits:
1 entered, 2 HP, 4 stamina, 8 echoes / Insight, 0x10 equipment, 0x20 lock-on, 0x40 enemy, 0x80 menu, 0x100 low HP, 0x200 peek).

## Test aids

`BB_AUTO_HUD_TEST="S:hp=40,S:hp=100,S:stam=20,S:echoes=+500,S:peek"`: S seconds after the world is up, set the own HP / stamina
to N % of the maximum, add echoes, or peek. Smoke phase `ah` (`--only auto_hud`).
