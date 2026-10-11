# Party balance: death debuff (C4), enemy scaling (D7), overhead display (E2)

Code: `gpu/shim/party/party_balance.{h,cpp}` (game side), `party_balance_rules.{h,cpp}` (pure,
`tests/test_party_balance.cpp`), settings in `bbport_settings.h` (`party_rot`, `party_boss_*`,
`party_enemy_*`, `party_overhead`), the System menu page "Party rules" in `bbport_game_menu_pages.cpp`, the
overlay's Party tab and tags in `bbport_overlay.cpp`, the VITALS trailer in `party_link.cpp`.
Addresses are our offsets (raw ELF VA of `out/eboot.elf`, 1.09).

## 1. The party's rules come from the host

The host's settings are the party's: `balance::Tick` (director tick, party mode) posts EVENT
`balance` `{"rot":true,"boss_hp":150,...}` to every guest on change and every 15 s (late joiners);
the guest's runtime hands it to `balance::OnHostRules` (JSON parsed with limits, every value clamped
to the menu's range). A guest uses its own settings until the first event arrives. The menu's help
lines say "(host decides)" on a guest. `BB_PARTY_BALANCE=0`: nothing (no hook, no param write, no
event).

## 2. D7: enemy scaling

### 2.1 What Bloodborne does (measured)

Read from the 1.09 params (`tools/party/flag_tool.py` layouts over `gameparam.parambnd.dcx`) and
the code (docs/party/four_players.md section 4):

- **Bosses**: the boss events (EMEVD `1003[109]` by cooperator count -> `2004[8]` SetSpEffect +
  `2004[53]` HP refresh) and native `Lua_MultiDoping` 0x138bc70 (chalice bosses) put SpEffect
  **7500** (1 cooperator) or **7501** (2) on the boss; party_fourp H4 uses 7502 for 3.

  | Row | maxHpRate | regist*ChangeRate | changeSuperArmorPoint | attack power rates |
  |---|---|---|---|---|
  | 7500 | 1.5 | 1.5 | +30 | 1.0 |
  | 7501 | 2.0 | 2.0 | +60 | 1.0 |
  | 7502 | 2.5 | 2.5 | +90 | 1.0 (unused in 1.09) |
  | 7503 | 3.0 | 3.0 | +120 | 1.0 (unused in 1.09) |

  So vanilla per extra hunter: boss HP +50 %, status resistance +50 %, poise +30, damage +0.
- **Normal enemies**: no co-op scaling at all. The max-HP rate 0x17e46e0 multiplies only the active
  SpEffects' `maxHpRate` (read live from the param row, node +0x48 -> row +0x10), the NG+ table
  0x4731ed0 by clear count and the 9200 level sync. BB's gameparam has no multiplayer correction
  param (no MultiHPCorrectionParam etc.).

### 2.2 Settings

System > Enemy scaling: six sliders (boss / enemy health, damage, poise; the help line shows the
value), also in the overlay's Party tab and bbport.ini. The page layout the port uses
("ControllSetting") has six rows; the System list itself scrolls, and the port's page cap
(MaxPages, one factory per page: our own constant, not the game's) is now 16. Live 2026-10-11
(single instance, the title's System menu walked with BB_TEST_MENU_FILE, BB_GAME_MENU_TRACE):
`Game menu: page 11 Party rules open`, `Game menu: page 12 Enemy scaling open` (the 13th page).

Per extra hunter (cooperator), Nightreign semantics where vanilla has a step (100 = vanilla):

| Setting (ini) | Unit | Range | Default | Vanilla |
|---|---|---|---|---|
| Boss health (`party_boss_hp`) | % of the boss HP step (+50 %) | 0..400 | **150** (+75 % HP per hunter) | 100 |
| Boss damage (`party_boss_damage`) | + % damage per hunter | 0..100 | **10** | 0 |
| Boss poise (`party_boss_poise`) | % of the poise step (+30) | -100..300 | 100 | 100 |
| Enemy health (`party_enemy_hp`) | % of the boss HP step | 0..400 | **50** (+25 % HP per hunter) | 0 |
| Enemy damage (`party_enemy_damage`) | + % damage per hunter | 0..100 | 0 | 0 |
| Enemy poise (`party_enemy_poise`) | % of the poise step | -100..300 | 0 | 0 |

Status resistance follows the HP setting (as in vanilla). Why these defaults (user decision
2026-10-11: harder than vanilla, ERSC-like boss boosts, adjustable): a party of three with free
builds, no tether and quick respawns trivialises vanilla bosses (2.0x HP for two cooperators);
ERSC's own defaults add per extra player +100 % boss HP and +35 % enemy HP with no damage
scaling. Ours: two cooperators give a boss 2.5x HP (vanilla 2.0x) and +20 % damage, normal enemies
1.5x HP; one cooperator 1.75x / 1.25x. Damage stays modest because Bloodborne's rally lets the
party regain HP. ERSC's note that in 3-4 player parties big enemies' poise drops (more
staggerable) is offered as negative poise values (e.g. enemy poise -50 %: -15 poise per hunter);
the default keeps vanilla poise.

### 2.3 How

- Rows: every second the host's values go into SpEffectParam 7500 / 7501 / 7502 (bosses with 1 / 2
  / 3 cooperators) and 7503 (normal enemies, for the current cooperator count from the game's own
  count 0x15bdc20, identical on every peer): maxHpRate +0x10, attack power rates +0x48..+0x54,
  status resist rates +0x108..+0x114, changeSuperArmorPoint +0x13a (s16). A row is written only
  while it holds the 1.09 value or what we wrote last (else logged and left alone); a param reload
  is re-written.
- Normal enemies: every 2 s, with cooperators present and any enemy setting not zero, every chr of
  this world's chr sets (WorldChrMan +0x850 tables) that is not a player and whose NpcParam row
  (`[[chr+0x3c8]+8]`) has teamType 23 (5792 of 6010 rows: the hostile enemies) and no
  isSoulGetByBoss (0x149 bit 4) gets SpEffect 7503 (chr vfunc +0x3f0), unless it is a boss: the 34
  entities the boss events dope (EMEVD 2004[8] with SpEffect 7500 / 7501, scanned in every 1.09
  map; `kBossEntities`), resolved each pass with GetChrByEntityId 0x13c97a0, never get it, so a
  boss is scaled once, by the boss rows. (Removing 7503 from a boss at its doping, 0x17e1d90 =
  EMEVD 2004[21]'s remove, was tried first: the Cleric Beast died on the spot, HP 6597 -> 0.)
  Chalice bosses (m29: event parameters / Lua_MultiDoping) are kept out by isSoulGetByBoss only. The game's per-frame HP update (0x1a0d7d0) recomputes max HP from the
  rows and raises current HP with the "independent" rate, so a full-health enemy stays full.
- Same HP on every screen: every peer writes the host's values and applies them to its copy of the
  enemies; the cooperator count is the game's, which counts the same on host and guests.

### 2.4 Live (2026-10-11, Cleric Beast, host + 1 guest)

`BB_PARTY_PROGRESS_TEST_CLEAR` un-killed the Cleric Beast, `host_move` into region 2412805 started
the fight. Both peers: `boss chr 1001409f: SpEffect 7500 (flags 4): HP 5277 / 5277 base 2700, rate
1.955` (2700 x 1.117 area row 7002 x 1.75; vanilla 1.5 would give 4523); the boss and Father
Gascoigne's two entities stayed out of 7503 (`3 bosses`). Normal enemies: sample max HP 520 ->
650. Damage, boss_damage 0 vs 100 (x1.0 vs x2.0 attack power at 1 cooperator): the players' hits
were 61-126 (largest 239) vs 216-534 (the defense formula is not linear, so not exactly x2). Poise:
the row's changeSuperArmorPoint is written; stagger was not measured.

## 3. C4: death debuff ("rot")

ERSC: optional rot, 5 kinds, one random per death, stacking, removed only at a grace. Ours
(Bloodborne-themed, two kinds):

- Each death of the local player (HP > 0 -> 0 in the world, any role) adds a stack, at most 5, of
  a kind picked at random: max HP -10 % or max stamina -10 % per stack. A toast after the respawn
  says "Rot 2/5: max HP -10 %, stamina -10 % until you rest at a lamp".
- Max HP through the max-HP rate 0x17e46e0 (prologue `55 48 89 e5 53 50`), max stamina through the
  stamina rate 0x17e4a20 (`55 48 89 e5 48 83 ec 10`; the SpEffects' maxStaminaRate +0x18, read by
  the per-frame update 0x1a572e0 into stamina max +0x138); both replaced, the result multiplied
  only for the local player's SpEffect list (`PlayerIns+0x1c8`). ERSC's other kinds (damage dealt,
  item discovery, vials) would need SpEffect rows of our own and are not done.
  No param row or SpEffect is used, so nothing reaches the other peers' copies.
- A lamp / headstone warp the player starts (0x13CDF30, party_travel `SetLampUseObserver`,
  before the guest lamp interceptor) cleanses it; the respawn warp of a death (within 20 s of it)
  does not. Travelling along with the party (a replay) does not either: only who rests is cleansed.
- Kept in `<user>/party_rot.txt` across restarts. Party mode only; host and guests alike; the
  host's on/off is the party's. Setting: System > Party rules > "Rot on death" (and the overlay's Party tab) (on by default,
  user decision 2026-10-11).
- Echoes / bloodstain: unchanged here. The user's decision is single-player behaviour for everyone
  (echoes drop into a recoverable bloodstain); that is the hostdeath work. Rot only touches max HP,
  so the two stack independently: a death drops echoes (bloodstain) and adds a rot stack; picking
  up the bloodstain does not cleanse rot, a lamp rest does.

## 4. E2: overhead display

ERSC `overhead_player_display`: normal / none / ping / level / death count. Ours: the party HUD's
name tags (projected with the game camera above each companion in view, 1-80 m), now a setting of
their own, independent of the HUD panel: System > Party rules > "Above companions": Nothing / Name
/ Name and level ("Hunter1  Lv 37") / Name and health (default, the HP bar as before) / Name and
deaths ("Hunter0  deaths 1, rot 1"). Local only. Bloodborne itself draws a phantom label (the
blue "Co-operator" / name text and a thin HP bar, about 2.1 m up); our tag sits above it (2.45 m,
live screenshot 2026-10-11) in the member's colour and adds what the game's does not show (level,
deaths, rot; names for members the game does not label). Ping is not offered (the Party members page shows it).

The HP behind the bars: data module (`chr+0x3b0 -> +0x20`) +0xf8 HP / +0xfc max, the HP struct the
game's kill (0x18bf9a0) and per-frame update (0x1a0d7d0) use. The party HUD read +0x128 / +0x12c
before, which stayed at 106 / 106 on a 719 HP hunter in the live runs (so the bars never moved);
party_hud_game now reads +0xf8 / +0xfc. (party_b2's guest HP keep still uses +0x128: worth a
look by its owner.)

**Reusable for I2 (pings, through-wall markers).** `party::hud::WorldToScreen(view, proj, world
point, display w, h, margin)` (party_hud.h, pure, tested in party-hud-test) projects a world point to
display pixels with the game camera (`BbCamera::Latest()`: view 3x4 + projection scales, captured
from the frame's constants), reports in-front / on-screen, and keeps x / y for off-screen points so
a caller can clamp an edge arrow. `party::hud::MemberColour(slot)` gives each member a fixed colour
(host gold, then pale blue, green, violet); the tags draw names in it. Approach found viable: the
**overlay** (ImGui background draw list over the presented frame) - live-verified for the tags,
drawn after the game's frame, so it naturally shows through walls (no depth test; I2 wants that).
The game's own Scaleform HUD (lock-on / enemy HP bar markers) was not investigated here; its
phantom label exists (blue "Cooperator" + name above phantoms) but its draw path is native.
Caveats for I2: the camera matrix is trusted only within 15 m of the player (cutscene cameras),
and the 1-80 m range filter is the tags' choice, not the projection's.

Level, deaths and rot stacks travel in a VITALS trailer: after the entries, `u8 n`, then per entry
`u16 level, u16 deaths, u8 rot`. Older peers stop reading after the entries; a frame without the
trailer reads as zeros (unknown: the tag shows the name only). The guest's single-entry VITALS
must say count 1 now.

## 5. Tests

- `party-balance-test`: vanilla rules reproduce rows 7500-7502, defaults, poise below zero, JSON
  round trip / clamping / malformed input, rot factors, slider tables.
- `party-link-test` (vitals): level / deaths / rot relayed guest -> host -> other guest.
- `game-menu-pages-test`: the new page keeps the menu's limits (12 pages = MaxPages).
- Live: see the ERSC_PARITY rows C4 / D7 / E2 for the runs.
