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
| `BB_PARTY_MAX` | Max players | 3 |
| `BB_PARTY_AUTO` | Automatic summoning | 1 |
| `BB_PARTY_SEAMLESS` | Seamless party rules | 1 |
| `BB_PARTY_SAVE` | Separate party saves | `separate` (launcher); runtime default without it: shared |
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
| `BB_PARTY_GUEST_MARK` | Guests can use Hunter's Mark | 0 (opt-in) |
| `BB_PARTY_FULL_REWARDS` | Full rewards for guests | 1 |
| `BB_PARTY_ITEMS` | Share the host's item rewards | 1 |
| `BB_PARTY_GUEST_INSIGHT` | Insight for bosses | `parity` (`full`, `0`) |
| `BB_PARTY_PROGRESS` | Share story progress | 1 |
| `BB_PARTY_PROGRESS_NPC` | Also NPC quests | 0 |
| `BB_PARTY_STORY` | Share cutscenes and endings | 1 |
| `BB_PARTY_STORY_MIRROR` | Play them live with the host | 1 |
| `BB_SAVE_BACKUP_MINUTES` | Back up every (min) | 10 (0 = only at start) |
| `BB_SAVE_BACKUP_KEEP` | Backups to keep | 10 |
| `BB_PARTY_RESTART` | Restart the game after a crash | 1 (`run.bat`) |

## Debug, test and expert switches (not in the launcher)

Set them in the environment before `run.bat` (or with `tools/mp/instances.py run --env`).
"Default" is the behaviour when the variable is unset.

| Name | What | Default | Owner file |
|---|---|---|---|
| `BB_PARTY_HOST` | Overrides the role from `BB_PARTY`: `1` host, anything else guest | role from `BB_PARTY` | `gpu/shim/party/party_director.cpp`, `gpu/shim/net/bbnet_glue.cpp` |
| `BB_PARTY_CODE_FILE` | Host: also writes the party code to this file. Guest without a code: waits up to 60 s for the file and joins with it | unset | `gpu/shim/party/party_runtime.cpp` (`tools/mp/instances.py`) |
| `BB_PARTY_PREFER_LAN` | Guest reading `BB_PARTY_CODE_FILE`: try the LAN code before the Internet code | off | `gpu/shim/party/party_runtime.cpp` |
| `BB_PARTY_LOOPBACK` | Local tests: every party and game socket binds 127.0.0.1 only (no firewall prompt) | off | `gpu/shim/party/party_runtime.cpp`, `gpu/shim/net/net_socket.cpp`, `gpu/shim/party/party_sock.h` |
| `BB_MP_LOCAL_TEST` | Same as `BB_PARTY_LOOPBACK` for the party layer; set by the instance harness (`tools/mp/instances.py`) | off | `gpu/shim/party/party_runtime.cpp` |
| `BB_PARTY_LOCAL_IP` | Forces the local IPv4 address the network layer reports and binds | default-route adapter | `gpu/shim/net/net_socket.cpp` |
| `BB_PARTY_FORCE_RELAY` | Sends all guest-to-guest game traffic through the host relay | off | `gpu/shim/net/party_udp.cpp` |
| `BB_PARTY_HASH_PATCHES` | `0`: gameplay patches are not compared in the version check | compared | `gpu/shim/party/party_runtime.cpp` |
| `BB_PARTY_HOST_LOST_MS` | Guest: ms after the host's link drops before the game leaves the host's world | 2000 | `gpu/shim/party/party_runtime.cpp` |
| `BB_PARTY_RESTARTED` | Set to `1` by `run.bat` on a crash restart: rejoin path, `restart` backup | unset | `run.bat`, `gpu/shim/party/party_runtime.cpp`, `src/runtime_savepolicy.c` |
| `BB_PARTY_RESTART_MAX` | Most crash restarts in 10 minutes | 5 | `run.bat` |
| `BB_PARTY_AUTOCONTINUE` | `0`: the director does not press Continue on the title (it still does after a crash restart); `1`: always | on in party mode | `gpu/shim/party/party_director.cpp` |
| `BB_PARTY_RING_EVERY` | Seconds between the director's automatic bells (min 1) | 30 | `gpu/shim/party/party_director.cpp` |
| `BB_PARTY_TRAVEL` | `0`: no travel-together hooks or replay (B1); also drops the open-world rules | on | `gpu/shim/party/party_travel.cpp`, `gpu/shim/party/party_phantom.cpp` |
| `BB_PARTY_DREAM_GATE` | `0`: the Hunter's Dream stays closed to co-op (no area-table patch) | patched | `gpu/shim/party/party_travel.cpp` |
| `BB_PARTY_FULL_HP` | `0`: cooperators keep the game's 70 % max HP | 100 % | `gpu/shim/party/seamless_rules.cpp` |
| `BB_PARTY_BELL_NO_INSIGHT` | `0`: the Beckoning Bell costs Insight again (param rule and XML patch) | no Insight | `gpu/shim/party/seamless_rules.cpp`, `scripts/patches.py` |
| `BB_PARTY_EMEVD_TRACE` | `1`: logs each new (event, bank, instruction) the game runs | off | `gpu/shim/party/seamless_rules.cpp` |
| `BB_PARTY_EMEVD_SKIP` | `event:bank:id[@index][,...]` (`*` = any event): skips those EMEVD instructions | none | `gpu/shim/party/seamless_rules.cpp` |
| `BB_PARTY_FOURP` | `0`: no 4-player patches (rule tag `4p:off`; must match on every player) | on | `gpu/shim/party/party_fourp.cpp` |
| `BB_PARTY_FOURP_SCALING` | `0`: no boss HP scaling for a 3rd cooperator (H2-H4) | on | `gpu/shim/party/party_fourp.cpp` |
| `BB_PARTY_FOURP_NPC_SIGNS` | `0`: no NPC-sign count rewrite (E6) | on | `gpu/shim/party/party_fourp.cpp` |
| `BB_PARTY_FOURP_RECRUIT` | `0`: no cosmetic server-side cap patch (P5) | on | `gpu/shim/party/party_fourp.cpp` |
| `BB_PARTY_PROGRESS_SCAN_MS` | Host flag diff period in ms (min 100) | 1000 | `gpu/shim/party/party_progress.cpp` |
| `BB_PARTY_PROGRESS_MIRROR` | `1`: also writes set-only flags into a summoned guest's host-world overlay | off | `gpu/shim/party/party_progress.cpp` |
| `BB_PARTY_FLAG_DUMP` | `1`: BBPF event-flag snapshots to `<user>/party/flags_*.bbpf` (`tools/party/flag_tool.py`) | off | `gpu/shim/party/party_progress.cpp` |
| `BB_PARTY_STORY_PLAYER` | Player argument of the remo call (10000 = local player, -1 = no appearance snapshot) | 10000 | `gpu/shim/party/party_story.cpp` |
| `BB_PARTY_STORY_TEST` | `cutscene:<remo id>[,mirror]`: single-instance replay test 10 s after the world is up | off | `gpu/shim/party/party_story.cpp` |
| `BB_PARTY_STORY_TEST_DELAY` | Delay of `BB_PARTY_STORY_TEST` in s | 10 | `gpu/shim/party/party_story.cpp` |
| `BB_PARTY_ITEMS_TEST` | `<lot>[,<lot>...]`: applies these item lots once in the own world (any role) | off | `gpu/shim/party/party_items.cpp` |
| `BB_PARTY_DIRECTOR_TEST` | `log_state,start,grant_bells,drop_bells,award_lot=N,insight=N,ring_host,ring_guest`: director test actions without a party | off | `gpu/shim/party/party_director.cpp` |
| `BB_PARTY_DIRECTOR_TEST_DELAY` | Delay of the director test actions in s (after the world is up) | 10 | `gpu/shim/party/party_director.cpp` |
| `BB_PARTY_PHANTOM_PROBE` | `insight`, `refill`: one offline probe 5 s after the world is up | off | `gpu/shim/party/party_phantom.cpp` |
| `BB_PARTY_TRACE` | `1`: logs every FROM API / HTTP request and answer (default: the first few) | off | `gpu/shim/net/bbnet_glue.cpp` |
| `BB_PARTY_TRACE_SSINFO` | `1`: logs the game's ss.info parse result | off | `gpu/shim/party/party_director.cpp` |
| `BB_PARTY_ONLINE_WATCH` | `0`: no `Net: online:` lines (the game's FROM-client / FrpgNetMan / network-flow online state, logged on every change) | on | `gpu/shim/net/online_watch.cpp` |
| `BB_NET_TRACE` | `1`: logs every datagram (default: the first 400) | capped | `gpu/shim/net/net_socket.cpp` |
| `BB_NET_SIM` | Network simulator: `lat=80,jitter=20,loss=2,dup=0.5,reorder=1,seed=7` or a preset `lan`, `wifi`, `dsl`, `mobile`, `bad` | off | `gpu/shim/net/netsim.h`, `gpu/shim/net/bbnet_glue.cpp` |
| `BB_PARTY_STATUS_LOG` | `1`: a `Party: status ...` line every 10 s and on each change | off | `gpu/shim/party/party_status_bridge.cpp` |
| `BB_PARTY_STATUS_TEST` | `host:kick:Hunter1@40,guest:leave@60,rejoin@80`: presses the overlay Party tab's buttons on a timer | off | `gpu/shim/party/party_status_bridge.cpp` |
| `BB_PARTY_TEST_NPC` | `<entity>[:<session type>][,...]` or `auto`: single-instance NPC-summon fixture | off | `gpu/shim/party/party_npc_test.cpp` |
| `BB_PARTY_TEST_NPC_MODE` | `summon`, `cap`, `filter`, `log` | `summon` | `gpu/shim/party/party_npc_test.cpp` |
| `BB_PARTY_TEST_NPC_PATH` | `a`: the faithful request path | default path | `gpu/shim/party/party_npc_test.cpp` |
| `BB_PARTY_TEST_NPC_DELAY` | Seconds in a steady world before the first summon | 5 | `gpu/shim/party/party_npc_test.cpp` |
| `BB_PARTY_TEST_NPC_RETURN` | Sends every summoned NPC home after N s | never | `gpu/shim/party/party_npc_test.cpp` |
| `BB_PARTY_TEST_NPC_LOG` | `0`: no per-second state line | on | `gpu/shim/party/party_npc_test.cpp` |
| `BB_PARTY_TEST_NPC_PATCH` | `1`: both patch groups of filter mode C on for the run | off | `gpu/shim/party/party_npc_test.cpp` |
| `BB_PARTY_TEST_NPC_WARP` | `<WarpParam id>`: one lamp warp before anything else | off | `gpu/shim/party/party_npc_test.cpp` |
| `BB_SAVE_BACKUP` | `1`: save backups also outside a party session; `0`: none in a party session | on in a party session | `src/runtime_savepolicy.c` |
| `BB_SAVE_TRACE` | `1`: logs every operation on save files (/savedataN) | off | `src/runtime_file.c` |

Not a party switch despite the name: `BB_SAVE_LOG` (frame / readback statistics, launcher
Advanced tab; `run.bat`). Related non-party switches the party code reads: `BB_MODS_ENABLED`
(mods off = no gameplay mods in the version check), `BB_SKIP_NETWORK_CHOICE` (the launcher sets
`online` in party mode; `scripts/patches.py`).
