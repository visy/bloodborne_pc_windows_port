# QA and testing

How this project tests its changes, how the automated party "bots" play the game, and the steps
that turn a set of merged fixes into a release. Everything here runs on one Windows PC with the
real game; nothing needs a second machine or an online service.

## 1. Layers of testing

| Layer | What it catches | Tool | When |
|---|---|---|---|
| Unit tests (C/C++) | logic errors in pure code: party codes, NAT, lift rules, menus, shop gates, frame pacing, decoders | `out/gpu/*test*.exe` (~80 targets, built with the GPU project) | every change, before any merge |
| Python tests | launcher, tools, save checks, test harness itself | `python -m pytest -q --deselect tests/test_probe.py` | every change, before any merge |
| Live scripted runs | a fix doing what it should inside the real game | `tools/mp/instances.py` + director test chains | while developing a fix |
| Party smoke | regressions across the whole co-op feature set, with an automatic PASS / FAIL | `tools/mp/smoke_party.py` | short (touched groups) per change; full once per release |
| Repeat runs | race-prone rows that pass once but fail 1 time in 10 | `tools/mp/repeat_party.py` | when a row is timing-sensitive |
| Soak | leaks, slow drift, crashes after an hour of play | `tools/mp/soak_party.py` + `soak_sample.py` | before larger releases, after memory work |
| Player playtests | everything the scripts never thought of | `docs/party/playtest*_checklist.md`, player logs | after a release, by real players |

A change is only as good as the layer that checked it. A unit test proves logic, not that the game
behaves; a smoke row proves one scripted path; a playtest finds the rest. Reports say which layers
ran and what stayed unproven.

## 2. Running several games on one PC: `instances.py`

All live testing starts real game instances side by side on the same PC:

```
python tools/mp/instances.py setup --count 2
python tools/mp/instances.py run --count 2 --party --env BB_PARTY_DIRECTOR_TEST=... --until "0:cooperators 0 -> 1"
```

- Instance 0 hosts, the others join through `ROOT/party_code.txt`. Each instance has a hardlinked
  build, its own `user\` folder and its own `bbport.ini` (720p, no upscaler).
- Networking is loopback only (`BB_MP_LOCAL_TEST=1`, `127.0.0.1`, a port range per instance root,
  never port 9307): no firewall prompts, no UPnP, no STUN. `--real-net` uses a real adapter when a
  test needs one.
- Games run muted (`BB_AUDIO=none`), in background windows (`BB_WINDOW_BACKGROUND=1`, no focus
  stealing, no desktop keystrokes), with DLSS off and light settings (30 FPS, few worker threads) so
  several fit on one machine.
- Saves: every run starts from a fresh copy of the test saves (`test_saves\savedata`, fixtures
  described in `docs/party/test_saves.md`). The player's own saves are never touched.
- `--until "I:REGEX"` ends the run as soon as instance I logs the line: runs stop on a condition,
  not after a fixed time. `--env` / `--ienv I:K=V` pass switches to all or one instance;
  `--crash-at S` kills a game on purpose.

### Machine-wide limits

Several agents share the PC, so the tools enforce limits:

- **Game queue** (`tools/mp/game_queue.py`, `agents/game_queue.json`): at most 2 game windows at
  once. `instances.py run` waits for a slot. `game_queue.py status` shows who holds one.
- **Build queue**: `python tools/agent/build.py <build dir>` is the only way to build (at most 2
  builds at once, below-normal priority, shared ccache, LTO off for agent builds).
- **Reaper** (`tools/mp/reaper.py`): ends games that sit idle (in a menu, or no party log for a
  while), games outside the queue, and tiles windows by slot. A test that waits must wait on a
  condition with a timeout.

## 3. The bots: how the game plays itself

There is no external input injection. The party code has a built-in **director** that drives the
game from inside the process, plus a few in-process input paths:

- **Director test chains** (`BB_PARTY_DIRECTOR_TEST`, reference in `docs/party/DEBUG_SWITCHES.md`):
  a comma-separated list of steps that run in order, each a delay or a condition after the
  previous one. Examples: `host_lamp=ID@S` (use a lamp as a player would, with the travel
  question), `guest_die@S`, `host_kill=ENTITY@S` (kill a loaded enemy or boss by its real death
  path), `host_move=X:Y:Z@S%MAP` (warp the character in a given map), `host_press=BUTTONS@S`
  (tap pad buttons), `guest_stick=X:Z@S` (walk there with the left stick in a closed loop, no warp),
  `host_route=...` (walk along waypoints from `tools/party/nav_route.py`), `host_leave@S` /
  `guest_rejoin@S` (the Party page buttons), `host_outage=MS@S` (a simulated network outage).
- **Conditions instead of sleeps**: a step can wait for an event flag (`?FLAG=V`), another player
  near a spot (`?near=X:Y:Z:R`), a map object's animation (`?obj=ENTITY:ANIM`), the Nth cooperator
  (`^N`) or the end of a cutscene (`~`). `BB_PARTY_TEST_COND_TIMEOUT` makes a stuck condition jump to
  the last step, so a run always reaches its end marker instead of idling.
- **Pad file** (`BB_PAD_FILE`): scripted controller input read in-process; used for walks and menu
  presses. `BB_TEST_MENU_FILE` walks the real System menu through the game's own pad path (the
  `rules` smoke phase checks every party settings page this way).
- **Fixtures and helpers**: `BB_PARTY_TEST_START_LAMP` (start at a lamp), `BB_PARTY_TEST_INVINCIBLE`,
  `BB_PARTY_TEST_PROLOGUE`, `BB_PARTY_WORLDACT_TEST` (levers, doors, pickups), `BB_NET_SIM`
  (latency, loss, outages: `lan`, `wifi`, `mobile`, `bad` presets), `BB_WORLD_WARPER` (tours every
  lamp, boss arena and Chalice spot; used for shader-cache and crash sweeps, `docs/SETTINGS.md`).
- **Logging as the oracle**: every party subsystem writes a plain-text log line for each state
  change (`Party: cooperators 0 -> 1`, `Party travel: ...`, `Party B2: ...`). Checks read these
  lines; nothing is judged from memory reads in the test script itself.

Test switches are for tests only. Player-facing behaviour is never controlled by them.

## 4. The party smoke: `smoke_party.py`

A fixed set of co-op scenarios with an automatic verdict per step, built from the director chains
above. It runs through `instances.py` (so it uses the shared queue), takes screenshots of the
rendered games at key moments (`tools/mp/shot.py`) and prints PASS / FAIL for every row from the
logs.

```
python tools/mp/smoke_party.py --only join,travel,death        # a few groups
python tools/mp/smoke_party.py --phase s1,s2                   # whole phases
python tools/mp/smoke_party.py --check-only --out DIR          # re-judge saved logs
python tools/mp/smoke_party.py --real-net --only join          # same steps on a real adapter
```

- **Phases** are scenarios of 2-3 players, ~3-8 minutes each: `s1` (join, Leave + Rejoin, a guest's
  death and bloodstain, party lamp travel, a guest-started Gascoigne fight, the host's death,
  a wipe), `s2` (Cleric Beast kill, arena revive, item box, guest crash and rejoin, host crash
  and recovery, host Leave), `rules` (every party settings page), `summonfail`, lift phases
  (`lf`, `el`, `lt`, `ch`, `rh`, `fw`, `fwg`, `fwb`, `fh`), falls (`fl`, `fd`, `fdj`), Chalice
  (`cl`, `clg`), lamps and the Dream (`lp`, `gd`, `gh`, `gm`), NPC fights, PvP, items and shop
  gates (`ie`, `il`), echo holding (`eh`) and more. The docstring at the top of the script
  describes each one.
- **Groups** (`GROUPS` in the script) name the rows a change can touch: `join`, `leave`, `travel`,
  `boss`, `wipe`, `gather`, `lifts`, `fall`, `chalice_guest`, `items_leak`, `echo_hold`, ... `--only`
  runs just the phases those groups need and stops each phase once its rows are judged
  (`GROUP_UNTIL` markers), so a short smoke takes minutes, not the full ~15-20.
- **Every fix adds or extends a phase** so the bug it fixed stays covered (for example the
  Forbidden Woods lift got `fw`, `fwg`, `fwb`, `fwe` when it was fixed).

### Short smoke vs full smoke

- **Per change**: the developer runs `--only <the groups the change touches>`. A full smoke per
  change would block the 2-slot game queue for everyone.
- **Per release**: one full smoke on the merged `master` (all phases), by the integrator.
  Failures are triaged one by one: a real regression blocks the release; a known flake or a stale
  check is fixed or documented in the release notes.

## 5. Repeats and soak

- `tools/mp/repeat_party.py SCENARIO --cycles N` repeats a race-prone row many times **in one game
  session** (Leave/Rejoin, crash and rejoin, ...) and reports a success count and timings per
  cycle. One pass of a timing-sensitive row is not evidence.
- `tools/mp/soak_party.py --minutes 75` lets two rendered games play for over an hour with pad
  input only (no warps, no invincibility) while `soak_sample.py` records memory, VRAM, handles and
  threads every minute and `BB_PARTY_PERF=1` the frame times. `--report` summarises growth.
- **Single-run measurement**: sweeps and comparisons happen inside one game session (for example
  `host_gfx=lod:N` steps for a graphics A/B), never one restart per data point.

## 6. Visual checks

Logs miss what a player sees: a guest inside a wall, a grey screen, a health bar floating in the
air. Live runs therefore take screenshots of every rendered instance at the key moments (join,
arrival, death, respawn, travel), or as on/off pairs at the same spot, and the developer looks at
them. Not a shot every few seconds: only the moments that matter.

## 7. How a change gets to `master`

Work happens in parallel, each task in its own worktree clone (`tools/agent/agent_env.py create
NAME --game`, branch `agent/NAME`) with its own build directory and game copy, so tasks never share
a build or a save.

The gate before a merge, all of it green:

1. Build every target, including all test targets (`build.py`, exit code 0).
2. Run every `out\gpu\*test*.exe` (working directory `out\`) and the Python tests.
3. A live run of the fix when the behaviour is in the game (with screenshots where the player would
   see it), or a clear note that it is unproven live and why.
4. The short smoke for the touched groups; add a phase or group if none covers the fix.
5. `CHANGES.md` entry under the top `## X.Y.Z (unreleased)` section, in plain player language
   (`Fixed:` / `Added:` / `Changed:`), and the README's co-op section if a player-visible behaviour
   changed.
6. Merge `master` into the branch, resolve conflicts, rerun the gate; then merge into `master`
   under a lock file (one merge at a time) and push.

Rules every change follows:

- **New behaviour is a toggle**: a row on the matching System menu page, a launcher checkbox and a
  `bbport.ini` key; host-decided rules show read-only on guests. Fixes get no player toggle (turning
  a fix off would only bring the bug back), at most a `BB_*` switch for testing.
- **New diagnostics are opt-in**: behind a launcher Diagnostics checkbox or a `BB_*_LOG` switch, off
  by default and rate-limited.
- **Correct over seamless**: if a seamless path glitches, use the game's own loading screen.
- **Privacy**: player logs contain names and IP addresses. Nothing from them goes into commits,
  docs or release notes; examples use `203.0.113.x`.
- **Report honestly**: the change report lists the root cause with evidence, what was tested, and
  what stayed unproven. Parity and progress numbers are given as "done per plan" and "proven live"
  separately.

## 8. Player reports

Bug reports from players come with logs (`nightwatch-*.log`, `last_session.log`), screenshots or
videos. Intake:

1. Store the files under `player_logs/<date>/<short-name>/` with a `README.txt`: what was reported,
   build and role (host/guest), and the first findings with log line numbers. These folders stay
   private.
2. Skim, don't deep-dive at intake: find the role, build, area, and the lines around the event.
   Common first checks: `party networking on (host|guest)`, `Party: roster`, `link:`, `direct
   summon`, `STOP:`, `Stall during rendering`, `Freeze:`, `NG cycle`.
3. Add a one-line entry to the next round's queue with a severity: S1 blocks play or loses
   progress, S2 is wrong but playable, then features and polish.
4. Reply to the player with a workaround if one exists, and what extra evidence would help (their
   own log from the failing attempt, a screenshot, the guest's log as well as the host's).
5. The fix round picks entries by severity; the fix adds a smoke phase that reproduces the report.

## 9. Making a release

1. **Freeze the round**: all planned branches merged on `master`, or explicitly moved to the next
   round. Nothing half-merged.
2. **Full smoke on `master`** (all phases, at 60 FPS where frame hooks matter: `BB_MP_FPS=60`).
   Triage every failure: regression (fix it, rerun that group), flake (rerun once, document),
   stale check (fix the check).
3. **Release build**: LTO (and PGO when the profile is current) via `build.py` in the release build
   directory. Agents build without LTO, so run one LTO build before release day: LTO removes
   symbols only referenced from assembly unless they are marked used. Clear stale ccache entries
   (`CCACHE_RECACHE=1`) if a build behaves differently from the source.
4. **Version bump**: `VERSION`, `PORT_VERSION` in `launcher.py`, the README version line, and the
   `CHANGES.md` heading from "unreleased" to the version and date. `tests/test_version.py` checks
   they agree.
5. **Stage the package** from the previous release folder plus changed files only: the new
   executables, changed docs, scripts and patches, `CHANGES.md`, `README.md`, `VERSION`,
   `launcher.py`, the bundled Python (`tools/release/make_python_bundle.py`). Never shader caches,
   saves, settings or logs. Compare every staged text file with `git show <commit>:<path>`.
6. **Release check on the staged build**: `bbport.exe --version`, and a short smoke (`s1`) run
   against the staged folder, not the build directory.
7. **Install** over a local game folder the same way a player would, leaving `user\`,
   `bbport.ini`, `launcher_settings.json` and `mods\` alone, and start it once.
8. **Push and tag**: push `master` to the project remote, tag the release commit.
9. **Release notes**: plain `Added` / `Changed` / `Fixed` / `Known issues` sections in player
   language, generic thanks to testers (no names or details from private logs). Known issues list
   what the smoke or the round left open.
10. **Publish** the zip with the notes on GitHub, then open the next round with the items carried
    over.

After a release, player reports start the next round at section 8.
