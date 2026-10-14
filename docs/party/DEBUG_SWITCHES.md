# Party co-op: environment switches

Every `BB_PARTY*` / `BB_SAVE_*` variable the party code reads (inventory of `getenv` in
`gpu/shim/party`, `gpu/shim/net`, `src/runtime_savepolicy.c`, `src/runtime_services.c`,
`run.bat`, `scripts/patches.py` and `launcher.py`, 2026-10-10). Paths are relative to the repo.

## User-facing settings (set by the launcher)

These are in the launcher's **Party** tab and its **Rules & sync...** window; `launcher.py`
`party_env()` writes them (also for `run.bat` started without the launcher, through
`launcher.py --write-env`) while party mode is on and unsets every one of them when it is off.
What they do for a player: [docs/PARTY.md](../PARTY.md).

| Variable | Launcher control | Default |
|---|---|---|
| `BB_PARTY` | Party tab: Mode | unset (off); `host` / `join` |
| `BB_PARTY_NAME` | Your name | Windows user name |
| `BB_PARTY_MAX` | Max players | 3 (2..4) |
| `BB_PARTY_AUTO` | Automatic summoning | 1 |
| `BB_PARTY_SEAMLESS` | Seamless party rules | 1 |
| `BB_PARTY_SAVE` | Separate party saves (always on) | `separate`; every party session uses `savedata_party`, `shared` is ignored |
| `BB_PARTY_PORT` | Port | 9307 |
| `BB_PARTY_UPNP` | UPnP | 1 |
| `BB_PARTY_PUBLIC_ADDR` | Public address (host) | unset (STUN / UPnP) |
| `BB_PARTY_STUN` | STUN server | `stun.l.google.com:19302`; `off` disables |
| `BB_PARTY_PASSWORD` | Password | unset |
| `BB_PARTY_CODE` | Party code (join) | unset |
| `BB_PARTY_START` | Rules & sync: Party starts | `prologue_solo` (`immediate`) |
| `BB_PARTY_GRANT_BELLS` | Give the bells | 1 |
| `BB_PARTY_OPEN_WORLD` | Open world (no co-op area walls) | 1 |
| `BB_PARTY_GUEST_RESPAWN` | Respawn at the host's lamp | 1 |
| `BB_PARTY_GUEST_REFILL` | Refill vials and bullets | 1 |
| `BB_PARTY_GUEST_MARK` | Guests can use Hunter's Mark (asks "Travel with the party?") | 1 |
| `BB_PARTY_FULL_REWARDS` | Full rewards for guests | 1 |
| `BB_PARTY_ITEMS` | Share the host's item rewards | 1 |
| `BB_PARTY_GUEST_INSIGHT` | Insight for bosses | `parity` (`full`, `0`) |
| `BB_PARTY_PROGRESS` | Share story progress | 1 |
| `BB_PARTY_PROGRESS_NPC` | Also NPC quests (shared NPC quest choices) | 1 |
| `BB_PARTY_STORY` | Share cutscenes and endings | 1 |
| `BB_PARTY_STORY_MIRROR` | Play them live with the host | 1 |
| `BB_SAVE_BACKUP_MINUTES` | Back up every (min) | 10 (0 = only at start) |
| `BB_SAVE_BACKUP_KEEP` | Backups to keep | 10 |
| `BB_PARTY_RESTART` | Restart the game after a crash | 1 (`run.bat`) |
| `BB_PARTY_LOG` | Advanced / Debug: Party logging level (set without a party too) | unset = normal (`verbose`, `trace`); [logging.md](logging.md) |
| `BB_PARTY_LOG_AREAS` | Advanced / Debug: the area checkboxes | unset = all (`net,summon,travel,death,items,flags,dialog`, `none`) |

## Debug, test and expert switches (not in the launcher)

Set them in the environment before `run.bat` (or with `tools/mp/instances.py run --env`).
"Default" is the behaviour when the variable is unset.

| Name | What | Default | Owner file |
|---|---|---|---|
| `BB_PARTY_MENU` | `1`: this party run was started by the in-game party flow (docs/party/ingame_flow.md): `user\party.json` supplies the host address, password and host code secret when `BB_PARTY_CODE` / `BB_PARTY_PASSWORD` are unset | unset (launcher runs) | `gpu/shim/party/party_flow_game.cpp` |
| `BB_PARTY_FLOW` | `0`: no party flow rows on the Party page | on | `gpu/shim/party/party_flow_game.cpp` |
| `BB_PARTY_TITLE_ENTRY` | `0`: no *Party* item on the title's main menu (System → Party still has the rows) | on | `gpu/shim/bbport_game_menu.cpp` |
| `BB_PARTY_FLOW_TEST` | Tests: `<action>[@s]` (`new`, `join`, `continue`, `host`, `leave`, `solo`, `auto` = `join` when party.json says role join, else `new`; `world:` prefix = in the world) runs that party flow action once, s seconds (default 15) after reaching the title / world; the restart it causes gets `BB_PARTY_FLOW_TEST_NEXT` as its `BB_PARTY_FLOW_TEST` | unset | `gpu/shim/party/party_flow_game.cpp` |
| `BB_RELAUNCH_LOOP` | Set by `run.bat`: the game may end with exit code 75 to be started again with `user\party_relaunch.bat` applied; unset: the party flow restarts through `runtime_restart` | `1` under `run.bat` | `run.bat`, `src/probe.c` |
| `BB_PARTY_HOST` | Overrides the role from `BB_PARTY`: `1` host, anything else guest | role from `BB_PARTY` | `gpu/shim/party/party_director.cpp`, `gpu/shim/net/bbnet_glue.cpp` |
| `BB_PARTY_CODE_FILE` | Host: also writes the party code to this file. Guest without a code: waits up to 60 s for the file and joins with it | unset | `gpu/shim/party/party_runtime.cpp` (`tools/mp/instances.py`) |
| `BB_PARTY_PREFER_LAN` | Guest reading `BB_PARTY_CODE_FILE`: try the LAN code before the Internet code | off | `gpu/shim/party/party_runtime.cpp` |
| `BB_PARTY_LOOPBACK` | Local tests: every party and game socket binds 127.0.0.1 only (no firewall prompt) | off | `gpu/shim/party/party_runtime.cpp`, `gpu/shim/net/net_socket.cpp`, `gpu/shim/party/party_sock.h` |
| `BB_MP_LOCAL_TEST` | Same as `BB_PARTY_LOOPBACK` for the party layer; set by the instance harness (`tools/mp/instances.py`) | off | `gpu/shim/party/party_runtime.cpp` |
| `BB_PARTY_LOCAL_IP` | Forces the local IPv4 address the network layer reports and binds | default-route adapter | `gpu/shim/net/net_socket.cpp` |
| `BB_PARTY_FORCE_RELAY` | Sends all guest-to-guest game traffic through the host relay | off | `gpu/shim/net/party_udp.cpp` |
| `BB_PARTY_HASH_PATCHES` | `0`: gameplay patches are not compared in the version check | compared | `gpu/shim/party/party_runtime.cpp` |
| `BB_HEADLESS` | `1`: headless-lite test game: the window starts minimised, nothing is presented and no draw call is recorded (compute, copies, the PM4 stream, fences and labels run as usual: game logic and party code unchanged); `tools/mp/instances.py --headless N` sets it (and the upscaler off, `BB_DLSS=0`) for the last N instances. Measured (2026-10-12, 2 instances in the Hunter's Dream): rendering 2.55 cores CPU / 23 % GPU, headless 2.07 cores / 8.5 % GPU | off | `gpu/shim/bbport_headless.h` |
| `BB_PARTY_CHECK_BUILD` | `0` on the host: the bbport build id is not compared in the version check (A9, docs/party/version_check.md) | compared | `gpu/shim/party/party_runtime.cpp` |
| `BB_PARTY_HOST_LOST_MS` | Guest: ms after the host's link drops before the game leaves the host's world | 2000 | `gpu/shim/party/party_runtime.cpp` |
| `BB_PARTY_RESTARTED` | Set to `1` by `run.bat` on a crash restart: rejoin path, `restart` backup | unset | `run.bat`, `gpu/shim/party/party_runtime.cpp`, `src/runtime_savepolicy.c` |
| `BB_PARTY_RESTART_MAX` | Most crash restarts in 10 minutes | 5 | `run.bat` |
| `BB_PARTY_AUTOCONTINUE` | `0`: the director does not press Continue on the title (it still does after a crash restart); `1`: always | on in party mode | `gpu/shim/party/party_director.cpp` |
| `BB_PARTY_FAST_JOIN` | `0`: the game's summon / join pauses stay (no `Party: Fast join` patch), ss.info sign polling every 5 s, director pauses 5 s / 4 s / 2.5 s, SDK-like room delays ([join_timing.md](join_timing.md)) | on in party mode | `scripts/patches.py`, `gpu/shim/net/from_api.cpp`, `gpu/shim/net/np_matching2.cpp`, `gpu/shim/party/party_director.cpp` |
| `BB_PARTY_FAST_MESSAGES` | `0`: the multiplayer banners hold 5 s again and the bell "Searching..." banners show | on in party mode | `scripts/patches.py` |
| `BB_PARTY_FAST_SIGN` | `0`: the first sign goes out 5 s after a bell (no `Party: Fast bell sign`) | 2 s with fast join | `scripts/patches.py` |
| `BB_PARTY_RING_DELAY` | Seconds in a steady world before the director's first bell (also the bell grant) | 0.3 (5 with `BB_PARTY_FAST_JOIN=0`) | `gpu/shim/party/party_director.cpp` |
| `BB_PARTY_RING_EVERY` | Seconds between the director's automatic bells (min 1) | 30 | `gpu/shim/party/party_director.cpp` |
| `BB_PARTY_TRAVEL` | `0`: no travel-together hooks or replay (B1); also drops the open-world rules | on | `gpu/shim/party/party_travel.cpp`, `gpu/shim/party/party_phantom.cpp` |
| `BB_PARTY_DREAM_GATE` | `0`: the Hunter's Dream stays closed to co-op (no area-table patch) | patched | `gpu/shim/party/party_travel.cpp` |
| `BB_PARTY_FULL_HP` | `0`: cooperators keep the game's 70 % max HP | 100 % | `gpu/shim/party/seamless_rules.cpp` |
| `BB_PARTY_BELL_NO_INSIGHT` | `0`: the Beckoning Bell costs Insight again (param rule and XML patch) | no Insight | `gpu/shim/party/seamless_rules.cpp`, `scripts/patches.py` |
| `BB_PARTY_PERF` | `1`: per-frame and per-call cost (p50 / p99 / max) of every coop-tick section and the hot hooks (SetEventFlag, EMEVD filter, award, travel funnel, ...) and of the party log lines, logged as `Party perf:` lines from a background thread; summarise with `tools/party/perf_summary.py run.log` | off | `gpu/shim/party/party_perf.cpp` |
| `BB_PARTY_PERF_SECONDS` | Report period of `BB_PARTY_PERF` | 10 | `gpu/shim/party/party_perf.cpp` |
| `BB_PARTY_EMEVD_TRACE` | `1`: logs each new (event, bank, instruction) the game runs | off | `gpu/shim/party/seamless_rules.cpp` |
| `BB_PARTY_EMEVD_SKIP` | `event:bank:id[@index][,...]` (`*` = any event): skips those EMEVD instructions | none | `gpu/shim/party/seamless_rules.cpp` |
| `BB_PARTY_FOURP` | `0`: no 4-player patches (rule tag `4p:off`; must match on every player) | on | `gpu/shim/party/party_fourp.cpp` |
| `BB_PARTY_FOURP_SCALING` | `0`: no boss HP scaling for a 3rd cooperator (H2-H4) | on | `gpu/shim/party/party_fourp.cpp` |
| `BB_PARTY_FOURP_NPC_SIGNS` | `0`: no NPC-sign count rewrite (E6) | on | `gpu/shim/party/party_fourp.cpp` |
| `BB_PARTY_FOURP_RECRUIT` | `0`: no cosmetic server-side cap patch (P5) | on | `gpu/shim/party/party_fourp.cpp` |
| `BB_PARTY_PROGRESS_SCAN_MS` | Host flag diff period in ms (min 100) | 1000 | `gpu/shim/party/party_progress.cpp` |
| `BB_PARTY_PROGRESS_MIRROR` | `1`: also writes set-only flags into a summoned guest's host-world overlay | off | `gpu/shim/party/party_progress.cpp` |
| `BB_PARTY_WORLD_MIRROR` | `0`: the guest no longer copies the host's world (EVENT `world_groups`) into its host-world flag overlay (vanilla: only the 12 groups of packet 0x10; A/B tests) | on | `gpu/shim/party/party_progress.cpp` |
| `BB_PARTY_FLAG_DUMP` | `1`: BBPF event-flag snapshots to `<user>/party/flags_*.bbpf` (`tools/party/flag_tool.py`): a `start` dump at the first steady world (what the save held), then session boundaries and syncs | off | `gpu/shim/party/party_progress.cpp` |
| `BB_PARTY_B2_DEATH_GRACE` | s a guest's own death in the host's world is held for a host travel (a wipe: the guest follows the host's respawn, docs/party/b2_seamless.md 6.8); 0 = no hold | 8 | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_B2_SOLO_RESPAWN` | `0`: a guest that dies alone (host alive) takes the old redirect (leave, bell) instead of respawning at the host's last lamp in the host's world (docs/party/b2_seamless.md 6.9) | on | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_TRAVEL_CHOICE` | Split travel (docs/party/split_travel.md): `ask` = a lamp / headstone / Hunter's Mark in a party asks "Travel with the party?" (game dialog); `party` = always the party move, no question; `alone` = always alone. An open question closes on a load, the player's death or after 30 s; a guest's lamp of the loaded map asks nothing (its own load); a guest's NO (alone from a lamp / headstone) asks the host for a rest load where it stands (the world resets for the party, `rest` lamp request) | `ask` | `gpu/shim/party/party_split.cpp` |
| `BB_PARTY_SPLIT_TEST` | Tests: `yes,no,...` answers the questions in order with the in-process pad (YES = Cross; NO = Right, Cross) | off | `gpu/shim/party/party_split.cpp` |
| `BB_PARTY_SPLIT_DIALOG` | Probing: `kind,button1,button2` of the question's generic dialog | `8,3,4` | `gpu/shim/party/party_split.cpp` |
| `BB_PARTY_INWORLD` | `1` (opt-in since playtest 1): a guest whose map the host's travel lands in (a host death at a lamp of this map) moves beside the host in-world with no load. Default: it loads too (B2 follow; the vanilla loading screen, its HostDead state ends, the world resets) | off | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_GUEST_LAMP_INWORLD` | `1` (opt-in since playtest 1): a guest's lamp / Mark to a lamp of the loaded map is a chr warp with no load. Default: its own loading screen (a re-join into the host's area), no question, the host never moves for it (`party_death_rules.h` PlanGuestLamp) | off | `gpu/shim/party/party_split.cpp` |
| `BB_PARTY_SPLIT_ENEMIES` | `1`: enemies killed while the party is apart are reconciled at a meet without a load (the other side's copy killed; docs/party/split_travel.md 5) - not verified live | off | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_STAGE_INWORLD` | `1`: a scripted stage warp (EMEVD 2003[14] / Lua WarpNextStage) to a warp point of the loaded map is an in-world move to the point's MSB region (no load) when the player is alone; `2`: also a host with members (the host moves alone). No real warp of the game reaches it (all same-map points are cutscene warps outside the travel funnel, split_travel.md §9): a test aid | off | `gpu/shim/party/party_b2.cpp`, `party_travel.cpp` |
| `BB_PARTY_TITLE_OWN_LAMP_SPOT` | `0`: a title-joined guest whose own save spot is unknown keeps the forced placement the join left (else: its own last lamp, so its own save keeps a valid spawn) | on | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_SPLIT_ALONE_FORCED` | `1`: a guest's alone load into another area uses the old forced transform (it became the own save's spawn) instead of the lamp warp with a chr warp after the arrival | off | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_SPLIT_SELFHEAL` | `1`: a remote chr > 4 m from every member's reported position for 3 s is removed and built again | off | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_RENDER_PROBE` | `1`: raw bytes of every remote chr and its modules in the B2 status line (the invisible-chr diff, split_travel.md 7) | off | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_TITLE_JOIN` | `0`: off. Default: the guest joins the host's session from the title so Continue loads the host's world (one load); falls back to the old Continue when the host does not ack within 6 s or anything fails (join_timing.md) | on | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_TITLE_JOIN_PROBE` | `1`: the host logs its serialized session descriptor at each bell join (compare with the guest's summon message head) | off | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_SPLIT_KEEP_1598` | Set: an alone load keeps the host's packet-0x19 value (GSM +0x1598 / +0x1593) | unset (cleared) | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_B2_KEEP_BUFFS` | `0`: a guest's timed SpEffects lost in a B2 load are not re-applied | on | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_B2_HOST_REVIVE` | `1` (opt-in since playtest 1): a host that dies with a member in its world is revived alone at its lamp in the loaded map, with no load (docs/party/b2_seamless.md 6.13). Default: the vanilla respawn load for the whole party at the host's lamp (one loading screen for everyone, the world resets); a death in a boss fight still waits in the limbo, then that load. Either way the dead are recounted every frame while the host is down (a member at HP 0 tells the host at once), so near-simultaneous deaths are a wipe (`party_death_rules.h`) | off | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_B2_HOST_REVIVE_DELAY` | s between the host's taken death funnel (after YOU DIED) and its revive | 1 | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_B2_REVIVE_RECIPE` | Diagnostics: comma list of the revive steps (`recover`, `flags`, `ctrl`, `restore`, `fade`, `reset`, `idle`, `anim`, `warp`, `gsm`, `module`, `v158`); `|` separates the recipes of successive revivals | `recover,flags,ctrl,fade,reset,idle,warp,gsm` | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_B2_REVIVE_ANIM` | Diagnostics: the animation the `anim` step forces on the revived host (Lua ForcePlayAnimation); `|` per revival | 0 | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_B2_REVIVE_DIFF` | `1`: at the revive, logs every dword of the host's chr, ChrCtrl and modules that differs from the last alive copy (`revive diff` lines) | off | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_B2_HOST_REVIVE_RECREATE` | `0`: a guest keeps its (dead) copy of the revived host's chr instead of re-creating it | on | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_BANNERS` | `0`: every SummonMessage banner as the game shows it; default: a B2 guest never sees "You died. Returning to your world." / "Your beckoner has died..." (it stays), and join banners ("beckoned", "Entered the world") are hidden for 15 s after a B2 re-join (logged `banner N hidden`) | on | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_EMPTY_STAIN` | `1`: a death with 0 Blood Echoes leaves its (empty) bloodstain as the game does; default: removed right after the record (logged `death with 0 Blood Echoes: its bloodstain removed`) | off | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_GUEST_CHR_TYPE` | `0`: a guest's own chr type (PlayerIns +0x78) stays as a B2 load / title join leaves it (0, an own-world body: its death then leaves the session); default: put back to 1 (white phantom) 2 s after such a load, logged `guest: own chr type 0 -> 1` | on | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_GUEST_BLOODSTAIN` | `0`: a guest that dies in the host's world keeps its Blood Echoes (vanilla phantom) instead of dropping them into a bloodstain | on | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_HOST_DEATH_ALONE` | `0`: a host whose last lamp is in another map moves the whole party there on death (old behaviour) instead of loading it alone while the guests stay | on | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_GUEST_OWN_LAMP` | `0`: a guest that dies outside a boss fight respawns at the host's last lamp instead of its own last lamp (split travel `RespawnAloneAtOwnLamp`) | on | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_GUEST_REVIVE` | `0`: a guest whose respawn lamp is in the loaded map respawns with a load as before instead of being revived in place | on | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_B2_ARRIVAL_WARP` | `0`: a guest's B2 load uses the exact transform beside the host again (that transform then became its own save's spawn spot: the Continue drift) instead of the host's own travel plus a chr warp after arrival | on | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_SPECTATE` | `0`: no spectating in a boss-fight limbo (the dead player's camera stays on its body); while spectating, Cross switches to the next living member | on | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_JOIN_BESIDE` | `0`: a guest that arrives by a bell join more than `BB_PARTY_JOIN_BESIDE_MIN` m (4) from the host stays where the summon put it instead of being put beside the host | on | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_POS_SCAN` | `1`: diagnostics: logs where the local player's position is kept in GSM / GameDataMan / PlayerGameData (`pos scan:` lines, every 10 s) | off | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_GUEST_PIN_SAVE_POS` | `1`: keeps a summoned guest's own save spot records (GSM +0x14A0 / +0x15B0 / +0x15F0, forced placement) at its pre-summon spot (not a fix yet, b2_seamless.md 6.13) | off | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_GUEST_DEATH_ALONE` | `1`: a guest whose own last lamp is in another map is revived in place, then travels alone to it (not verified live) | off | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_B2_HOST_REVIVE_SOLO` | `1`: test fixture: the host revive also without a session or a member | off | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_PROGRESS_TEST_CLEAR` | `<id>[,<id>...]`: clears these flags once in the own save at the first steady own world, before any host flag is applied (a guest save that lacks host progress; test saves only) | off | `gpu/shim/party/party_progress.cpp` |
| `BB_PARTY_PROGRESS_TEST_SET` | `<id>[,<id>...]`: sets these flags once in the own save at the same point, after the clears (e.g. a stale NPC quest state on a guest save, D8; test saves only) | off | `gpu/shim/party/party_progress.cpp` |
| `BB_PARTY_STORY_PLAYER` | Player argument of the remo call (10000 = local player, -1 = no appearance snapshot) | 10000 | `gpu/shim/party/party_story.cpp` |
| `BB_PARTY_STORY_TEST` | `cutscene:<remo id>[,mirror]`: single-instance replay test 10 s after the world is up; `host:<remo id>`: the party host plays it and sends it like a captured 2002 play once a cooperator has been in the world for the delay; `leave_after_mirror`: a guest leaves the party once a live mirror ended; `ending:<1|2|3>[,summoned]`: queue that ending as if the host had sent it after the delay (`summoned`: only once a phantom) - ending, staff roll and NG+ on that save | off | `gpu/shim/party/party_story.cpp` |
| `BB_PARTY_STORY_TEST_DELAY` | Delay of `BB_PARTY_STORY_TEST` in s | 10 | `gpu/shim/party/party_story.cpp` |
| `BB_PARTY_ITEMS_TEST` | `<lot>[,<lot>...]`: applies these item lots once in the own world (any role) | off | `gpu/shim/party/party_items.cpp` |
| `BB_PARTY_BALANCE` | `0`: no party balance rules (rot hook, SpEffect 7500-7503 writes, enemy SpEffect, EVENT `balance`); settings: System > Party rules, overlay Party tab (ini `party_rot`, `party_boss_*`, `party_enemy_*`, `party_overhead`; docs/party/balance.md) | on in party mode | `gpu/shim/party/party_balance.cpp` |
| `BB_PARTY_ROT_ALL_KINDS` | `1`: rot kinds 3-5 too (stamina cost +10 %, rally -15 %, item discovery -10 per stack; hooks 0x17e4c50 / 0x17e5590 / 0x17e8010), not yet verified live | off (health and stamina only) | `gpu/shim/party/party_balance.cpp` |
| `BB_PARTY_ROT_KIND` | `N[,N...]`: force the kind of this player's deaths in turn (tests; 0 health, 1 stamina, 2 stamina cost, 3 rally, 4 discovery) | random | `gpu/shim/party/party_balance.cpp` |
| `BB_PARTY_HUD_POSDIAG` | `1` (with `BB_PARTY_HUD_LOG=1`): dump position-like values in each remote PlayerIns (the floating name plate hunt) | off | `gpu/shim/party/party_hud_game.cpp` |
| `BB_PARTY_KILL_ECHOES` | `0`: no party kill-echo payment (default: in a party session each game calls the game's kill-echo payer 0x18C5070 for an enemy seen alive, then dying and unpaid 3 s later, logged `kill echoes: entity N paid by the party`; the enemy census also logs each death's payer gates) | on | `gpu/shim/party/party_balance.cpp` |
| `BB_PARTY_ENEMY_CENSUS` | `1`: H4 enemy census: every second in a steady world, the loaded map's enemies (team 23) by entity id, alive / dead; logs kills, `RESPAWN without a load` (a dead enemy alive again with no load in between) and a steady line every 15 s; a load starts a new baseline (`party_enemy_census.h`, unit party-enemy-census-test) | off | `gpu/shim/party/party_balance.cpp` |
| `BB_PARTY_BALANCE_LOG` | `1`: log every enemy pass (enemies seen, SpEffect 7503 applied / carried, a sample enemy's HP and effects) | off | `gpu/shim/party/party_balance.cpp` |
| `BB_PARTY_DIRECTOR_TEST` | `log_state,start,grant_bells,drop_bells,award_lot=N,set_flag=N[:N...],insight=N,ring_host,ring_guest`: director test actions without a party (on a party host `award_lot` / `set_flag` wait until a cooperator has been in the world for the delay); live travel steps `host_warp=ID@S` (the party move, no split question), `host_lamp=ID@S` / `guest_lamp=ID@S` (the lamp as the player uses it: with the split question), `host_alone=ID@S` (the host travels alone), `host_die@S`, `guest_die@S` (optional `#N`: not before this player's Nth join) run in order, S s after each (re)join (docs/party/travel.md 6; without `%MAP` a die step after another step waits for the next re-join, so use `host_die@S%MAP` in a chain); `host_kill=ENTITY@S` (host) kills that entity S s after the world is steady with a cooperator again since the previous step (docs/party/b2_seamless.md 6.7); `host_kill=0` the nearest living regular (non-boss) enemy within 500 m; the target comes from the enemy census's loaded-enemy list (`PickEnemyKillTarget`, party_enemy_census.h), so it is loaded and alive, logged as `host_kill: entity N (census key K; loaded, HP H) at (x, y, z), D m away` (N the map entity id chr +0x1F0, K the census key +0x3B8, which differs between games); the kill is ForceDead by id (a boss: its real defeat path), then the death path on the chr, then HP 0 if still alive (`killed by ...: HP a -> b`); not loaded / dead / none in range: `no target`; `host_kill=ENTITY:hp@S` only sets its HP to 0 (no ForceDead / death call); each kill logs the own Echoes and the kill-echo payer's gates before and after (`+0x1e1 ... (paid N), remote, soul`); `guest_kill=` the same on the guest without ForceDead; `host_use=GOODS@S` / `guest_use=GOODS@S` apply what using goods GOODS does (its EquipParamGoods refId SpEffect, or BehaviorParam_PC's; one given if none owned, not consumed, no animation; Madman's Knowledge 1500 = SpEffect 2160, +1 Insight) and request a game save; `host_move=X:Y:Z[:YAW]@S[%MAP]` / `guest_move=...` warp that role's chr to a spot (only in a map whose name contains MAP); `host_press=BUTTONS@S[%MAP][!]` / `guest_press=...` tap pad buttons in-process (0x4000 Cross; `!`: only once the own chr is alive and the world up); `host_lastlamp=ID@S` sets the host's last-lamp record (+0x1528) to another lamp (steady-world clock); `host_outage=MS@S` / `guest_outage=MS@S` a simulated total network outage (BB_NET_SIM on); `host_lamp=ID@S` warps the host to a lamp (the lamp-warp function, a B2 party travel; steady-world clock); `bell_host@AA` / `bell_guest@AA` ring only while the host's map (the roster's for a guest) is in area AA (e.g. 29: ring once the host is inside its chalice dungeon); `host_flag=ID[:0]@S` sets that event flag on the host (`:0` clears it, e.g. `12101021:0` puts a test save back into its prologue: the host is no longer start-ready); `log_pos` logs the local chr's physics position every second; `host_obj=ENTITY[:DIST[:ANGLE]]@S[%MAP]` / `guest_obj=...` stand that role's chr DIST m (1.2) from map object ENTITY at ANGLE degrees from its front, facing it; `host_chr=` / `guest_chr=` the same at a character; `guest_stick=X:Z[:R[:STEPS]]@S[%MAP]` the guest walks by the left stick (in-process pad file, closed loop on its position, no warp) to (X, Z) within R m (1), at most STEPS stick steps (40); any value also stamps the party logs with the wall clock | off | `gpu/shim/party/party_director.cpp` |
| `BB_PARTY_ALTAR_AUTO` | `1`: after the host's altar item *Enter the party's dungeon* took the glyph, the director answers the remaining prompts (OK, the result, YES, the altar, the first awaken point; text-gated, strict). Not verified live yet | off | `gpu/shim/party/party_director.cpp` |
| `BB_PARTY_EMEVD_TRACE_ALL` | `ID[,ID...]`: with `BB_PARTY_EMEVD_TRACE`, log every call of these events (not only the 1st/1000th) | off | `gpu/shim/party/seamless_rules.cpp` |
| `BB_PARTY_LOG_SNAPSHOT_S` | With `BB_PARTY_LOG=verbose`: seconds between state snapshots (`0`: none) | 10 | `gpu/shim/party/party_log_game.cpp` |
| `BB_PARTY_LOG_MARK_AT` | Test aid: one user mark (as F9) S s after the world first came up with the party link up (verbose only) | off | `gpu/shim/party/party_log_game.cpp` |
| `BB_PARTY_LOG_TIME` | `1`: wall-clock stamps on the Party / Party travel / Party phantom log lines | off | `gpu/shim/party/party_director.cpp` |
| `BB_PARTY_DIRECTOR_TEST_DELAY` | Delay of the director test actions in s (after the world is up) | 10 | `gpu/shim/party/party_director.cpp` |
| `BB_PARTY_DESYNC_S` | Host: seconds between desync digests (party_desync.h: the world mirror and the host's candidate flags, checked by every guest, which asks a resync of the buckets that differ); `0` off | 15 | `gpu/shim/party/party_desync.cpp` |
| `BB_PARTY_DESYNC_TEST` | `N`, guest: after the N-th matching digest, corrupt one mirror group and forget one host flag of the guest's model, so the next digest must find, resync and resolve both (live check) | off | `gpu/shim/party/party_desync.cpp` |
| `BB_PARTY_GUEST_SERVICES` | `0`: guests cannot use the Doll, shops or workshop in the host's Dream (vanilla client gates) | on in party mode | `gpu/shim/party/party_guestsvc.cpp` |
| `BB_PARTY_GUEST_SERVICES_TALK` | `all` or a comma list of more talk script ids the guest may use (diagnostics) | built-in list | `gpu/shim/party/party_guestsvc.cpp` |
| `BB_PARTY_GUEST_LAMPS` | `0`: guests cannot use world lamps / headstones (vanilla client gates); `1` forces them on. Otherwise the ini / menu switch `party_guest_lamps` (Party members page, "Guests use lamps"). On: a guest's lamp / headstone use becomes a request and the whole party moves (host-led); a lamp a guest lights is lit in the host's world (EVENT `lamp_lit`) | on in party mode | `gpu/shim/party/party_guestsvc.cpp` |
| `BB_PARTY_HEADSTONES` | `0`: the Hunter's Dream headstones and the world lamps stay vanilla (no headstone / lamp use while a session is up) | on in party mode (the host's headstone or lamp travel moves the party; lamp object gate +0x49 patched, lamp talk scripts' IsMultiplayerInProgress answered 0) | `gpu/shim/party/party_guestsvc.cpp` |
| `BB_PARTY_GUESTSVC_TEST` | `echoes=N;warp=x,y,z,yaw[/...]`: live-test fixture on the guest, 15 s after the guest-services gate opened; also `lamppress`, `lampflag=ID`, `lampprobe` (lamp objects and their FrpgNetMan +0xC70 entries; host side too), `hostheadstone` / `guestheadstone` with `keys=cross,...` (that role uses headstone t210000), `lampwalk` (stand 1 m in front of the nearest lamp), `hostlamppress` (host: Cross, Left, Cross at the lamp), `hurt` (with `lampwalk` on the guest: half HP before the rest), `buttonprobe`, `talkprobe` | off | `gpu/shim/party/party_guestsvc.cpp` |
| `BB_PARTY_WORLD_ACTIONS` | `0`: a guest's levers / doors / shortcuts / NPC talk stay in its copy of the host's world (no EVENT `world_action`, no own-save write); also drops the treasure pickup patches (docs/party/world_actions.md) | on in party mode | `gpu/shim/party/party_worldact.cpp` |
| `BB_PARTY_GUEST_PICKUP` | `0`: no per-player world treasure for guests (default on: treasure spawned on a guest, handles of its own, kept at the host's item sync, its pickup flag into the guest's own save; patches 0x17DC3D6 / 0x17DC3E3 / 0x17D6838 / 0x17D6AF6 / 0x17D52D9 / 0x17D52E6 / 0x17D34A0 / 0x17D40AB) | on | `gpu/shim/party/party_worldact.cpp` |
| `BB_PARTY_GUEST_DROPS` | `0`: a guest keeps only Blood Vials / Quicksilver Bullets of its own roll of an enemy's drop (vanilla client filter 0x17DD764); default: every player keeps its whole own roll | on in party mode | `gpu/shim/party/party_worldact.cpp` |
| `BB_PARTY_SHARED_DROP_ROLL` | `0`: every player's game rolls each enemy's drop with its own RNG (the old behaviour); default: one shared roll per kill (the game RNG seeded per enemy from the host's salt and world generation, EVENT `drop_seed`, the host's item discovery), every player gets its own copy of the same items; log `Party: drops: shared drop roll ...` | on | `gpu/shim/party/party_droproll.cpp` |
| `BB_PARTY_CATCHUP` | `1`: the late-joiner catch-up for a guest whose save is behind the host's (opening skip, campaign-start flags, starting weapons, warp to the host's lamp). Off since playtest 1 #19: every player plays its own prologue; summons start once everybody is ready | off | `gpu/shim/party/party_catchup.cpp` |
| `BB_PARTY_TEST_PROLOGUE` | `FLAG:0[+FLAG:0...]`: live test of #19 - writes these start flags at the first in-world tick, before readiness is read (`12100105:0` in the Hunter's Dream = the first Dream visit with the weapons taken; `12101021:0` = a weapon not taken). `BB_PARTY_TEST_PROLOGUE_END=S[:LAMP]`: after S s restore them, or lamp-warp to LAMP (2412950 = the sickroom lamp, the headstone's target) | off | `gpu/shim/party/party_director.cpp` |
| `BB_PARTY_GATE_CATCHUP` | `1`: a guest derives the area gate flags 2AAx / 3AAx of its own killed bosses into its own save (docs/party/world_actions.md 7) - opt-in for 0.5.2 | off | `gpu/shim/party/party_worldact.cpp` |
| `BB_PARTY_TOD_CATCHUP` | `1`: the host re-sends its time of day (Blood Moon) to a member once it is in the host's world | off | `gpu/shim/party/party_story.cpp` |
| `BB_PARTY_GUEST_TALK` | `0`: world NPCs refuse a guest again (their talk scripts' IsClientPlayer stays 1) | on in party mode | `gpu/shim/party/party_guestsvc.cpp` |
| `BB_PARTY_WORLD_WATCH` | `id,...`: logs the visible / own-save / host-image value of these flags on every change (both roles) | off | `gpu/shim/party/party_worldact.cpp` |
| `BB_PARTY_WORLDACT_TEST` | Guest live fixture `obj=E[/E..];chr=E[/E..];pickup=N[;flag=F];kill=N;after=S`: S s after a steady world in the host's world, stand at each map object, then each character (8 directions x 1.0 / 1.6 m) until an action prompt shows, press Cross (talk: 3 more); then the N nearest treasures this player has not taken (logged spawns; `flag=F` only that pickup flag), Cross on each; then `kill=N`: the nearest live enemy (AAB0000-AAB0699, 25 m) ForceDead, stand on its spot, Cross x3 (its drop) | off | `gpu/shim/party/party_worldact.cpp` |
| `BB_PARTY_PHANTOM_PROBE` | `insight`, `refill`: one offline probe 5 s after the world is up; `host9350` (party host): synthetic story then boss-kill Insight starts 6 s / 9 s after a cooperator is in (G2 live check) | off | `gpu/shim/party/party_phantom.cpp` |
| `BB_PARTY_TRACE` | `1`: logs every FROM API / HTTP request and answer (default: the first few) | off | `gpu/shim/net/bbnet_glue.cpp` |
| `BB_PARTY_TRACE_SSINFO` | `1`: logs the game's ss.info parse result | off | `gpu/shim/party/party_director.cpp` |
| `BB_PARTY_ONLINE_WATCH` | `0`: no `Net: online:` lines (the game's FROM-client / FrpgNetMan / network-flow online state, logged on every change) | on | `gpu/shim/net/online_watch.cpp` |
| `BB_NET_TRACE` | `1`: logs every datagram (default: the first 400) | capped | `gpu/shim/net/net_socket.cpp` |
| `BB_NET_SIM` | Network simulator on the game's UDP and the party link's TCP: `lat=80,jit=20,loss=2,dup=0.5,reorder=1,seed=7` (one-way ms / %), `outage=MS,every=MS` or `outage=MS,at=MS` (one-shot after start), `any=1` also for non-loopback peers (default: loopback only), or a preset `lan`, `wifi`, `dsl`, `mobile`, `bad`; director steps `host_outage=MS@S` / `guest_outage=MS@S`; docs/party/network_robustness.md | off | `gpu/shim/net/netsim.h`, `gpu/shim/net/bbnet_glue.cpp` |
| `BB_PARTY_STATUS_LOG` | `1`: a `Party: status ...` line every 10 s and on each change | off | `gpu/shim/party/party_status_bridge.cpp` |
| `BB_PARTY_HUD_LOG` | `1`: a `Party HUD: ...` line every 5 s (own / remote PlayerIns reads, camera, the board) | off | `gpu/shim/party/party_hud_game.cpp` |
| `BB_PARTY_HUD_TAGS` | `0`: no name tags above companions (the panel stays) | on | `gpu/shim/bbport_overlay.cpp` |
| `BB_PARTY_HUD_TUNE_FILE` | Test aid: path of a `key=value` file the overlay re-reads every 0.5 s for our name plate's parameters (`font_h`, `bar_w`, `bar_h`, `cap_w`, `cap_h`, `cap_rise`, `cap_dx`, `name_inset`, `baseline_gap`, `outline`: fractions of the display height, `bar_w` of the width; `rim` px, `glow` alpha; `name_rgb`, `bar_top`, `bar_bottom`, `cap_rgb` as r,g,b 0..255); with `BB_PARTY_HUD_GAME_PLATE=1` and `BB_PARTY_HUD_TAG_SHIFT` to tune ours live beside the game's plate | off (the `kPlate*` constants) | `gpu/shim/party/party_hud.cpp` |
| `BB_PARTY_STATUS_TEST` | `host:kick:Hunter1@40,guest:leave@60,rejoin@80`: presses the overlay Party tab's buttons on a timer | off | `gpu/shim/party/party_status_bridge.cpp` |
| `BB_PARTY_TEST_NPC` | `<entity>[:<session type>][,...]` or `auto`: single-instance NPC-summon fixture | off | `gpu/shim/party/party_npc_test.cpp` |
| `BB_PARTY_TEST_NPC_MODE` | `summon`, `cap`, `filter`, `log` | `summon` | `gpu/shim/party/party_npc_test.cpp` |
| `BB_PARTY_TEST_NPC_PATH` | `a`: the faithful request path | default path | `gpu/shim/party/party_npc_test.cpp` |
| `BB_PARTY_TEST_NPC_DELAY` | Seconds in a steady world before the first summon | 5 | `gpu/shim/party/party_npc_test.cpp` |
| `BB_PARTY_TEST_NPC_RETURN` | Sends every summoned NPC home after N s | never | `gpu/shim/party/party_npc_test.cpp` |
| `BB_PARTY_TEST_NPC_LOG` | `0`: no per-second state line | on | `gpu/shim/party/party_npc_test.cpp` |
| `BB_PARTY_TEST_NPC_PATCH` | `1`: both patch groups of filter mode C on for the run | off | `gpu/shim/party/party_npc_test.cpp` |
| `BB_PARTY_GATHER_DSUM` | `1`: experiment (A5): a guest in another map than the host rings where it stands (no gather to the host's lamp) and the direct summon is meant to carry it; live it joins the room but its game does not load the host's map (join_timing.md, 2026-10-13 a5) | off | `gpu/shim/party/party_director.cpp` |
| `BB_PARTY_TEST_TRAVEL_ON_SERVE` | `<ReturnPointParam id>`: test aid (host): one lamp warp 1.5 s after the host first served its room to a direct summon (a host travel while a member loads in; the travel is held until the joiner is in, playtest 1 batch 6) | off | `gpu/shim/party/party_b2.cpp` |
| `BB_PARTY_TEST_DIE_ON_DOWN` | `S`: test aid (host, with `BB_PARTY_DIRECTOR_TEST`): the host dies (the `host_die` death path) S s after a guest's down notice: a wipe without `host_kill` (whose ForceDead on an enemy raised the guest's boss-clear state) | off | `gpu/shim/party/party_director.cpp` |
| `BB_PARTY_TEST_START_LAMP` | `<ReturnPointParam id>`: test aid: one lamp warp 3 s after the first steady own world, before any join (e.g. the host in Central Yharnam while a late joiner's save is in the Dream) | off | `gpu/shim/party/party_director.cpp` |
| `BB_PARTY_TEST_NPC_WARP` | `<WarpParam id>`: one lamp warp before anything else | off | `gpu/shim/party/party_npc_test.cpp` |
| `BB_PARTY_ITEMS_HOST_TEST` | `<lot>[,...]`: the host awards these lots through the game's `0x17DDC50` (hostOnly 1) 20 s after a cooperator appeared, 10 s apart (summoned-give check) | off | `gpu/shim/party/party_items.cpp` |
| `BB_PARTY_GUEST_FOG_FIRST` | `0`: a guest must wait for the host at boss fogs again (vanilla); default on: the guest's fog skips the host-entered condition and the host opens the fight on its `world_action` `fog` report (B2) | on | `gpu/shim/party/seamless_rules.cpp`, `party_worldact.cpp` |
| `BB_PARTY_GIFT_TEST` | `<lot>`: a summoned guest awards this lot itself through the game's `0x17DDC50` (hostOnly 0, as an NPC talk gift) once its world is steady for 20 s, to check the D5 gift path (guest `gift: ... reported`, host `gift from slot N ... awarded`) | off | `gpu/shim/party/party_items.cpp` |
| `BB_PARTY_GIFT_TEST_HOSTONLY` | `1`: the `BB_PARTY_GIFT_TEST` award uses hostOnly 1, as the Doll / Messenger talk awards its per-player gifts (e.g. `BB_PARTY_GIFT_TEST=10050`, the Old Hunter Bell: guest `own gift lot 10050 ... given to the guest itself`) | off | `gpu/shim/party/party_items.cpp` |
| `BB_PARTY_ITEMS_HOST_TEST_FLAGS` | `<id>[,...]`: flags the host sets with the first `BB_PARTY_ITEMS_HOST_TEST` award | off | `gpu/shim/party/party_items.cpp` |
| `BB_PARTY_PROGRESS_TEST_READ` | `<id>[,...]`: logs own-save / visible values at each steady load | off | `gpu/shim/party/party_progress.cpp` |
| `BB_PARTY_STORY_SUMMONED` | `0`: the C4 replay queue waits for the own world (no replays while summoned) | on | `gpu/shim/party/party_story.cpp` |
| `BB_PARTY_STORY_ENDING_SUMMONED` | `0`: a summoned guest's queued ending waits for the own world (default: it warps to its own Hunter's Dream and runs the ending there) | on | `gpu/shim/party/party_story.cpp` |
| `BB_PARTY_ITEMGET_PROBE` | `1`: logs the item-get dialog's state on every change (list entries at `*0x553B1C0`, dialog-up word `*0x5562878+0x130`, the HUD model rows, the open-edge gate) | off | `gpu/shim/party/party_items.cpp` |
| `BB_PARTY_ITEMGET_WATCH` | `0`: no item-get dialog watchdog; default: a box left over a load, up while the player is dead for 3 s, or with its confirm check held for 20 s is cleared the way its confirm clears it (items stay in the inventory; log `item-get dialog cleared`) | on | `gpu/shim/party/party_items.cpp` |
| `BB_SAVE_BACKUP` | `1`: save backups also outside a party session; `0`: none in a party session | on in a party session | `src/runtime_savepolicy.c` |
| `BB_SAVE_TRACE` | `1`: logs every operation on save files (/savedataN) | off | `src/runtime_file.c` |

Not a party switch despite the name: `BB_SAVE_LOG` (frame / readback statistics, launcher
Advanced tab; `run.bat`). Related non-party switches the party code reads: `BB_MODS_ENABLED`
(mods off = no gameplay mods in the version check), `BB_SKIP_NETWORK_CHOICE` (the launcher sets
`online` in party mode; `scripts/patches.py`).

Not party switches either, for any bug report: `BB_TRACE=1` (launcher Advanced / Debug: Debug
tracing) prints a once-a-second heartbeat (flips/s, GPU submits/s), `[BLOCKED >3s]` lines while
frames stall and hang dumps (`out/hang_dump*.log`, with thread states and the main thread's
once-a-second RIP samples) at no measurable fps cost, since imports keep their direct calls.
`BB_TRACE_IMPORTS=1` (or `bbport.exe --trace`) adds the full per-call import trace
(`out/imports_<time>.log`, `[IMPORT CALL]` lines, the main thread's recent imports in hang
dumps): every import then goes through a logging thunk, about 70% slower (102 -> 31 fps), so use
it only to find which import a hang or crash is in. `src/probe.c`.
