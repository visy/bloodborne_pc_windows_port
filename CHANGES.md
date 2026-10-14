# Bloodborne PC - Windows Native Port Improvements & Changes

This document details the optimizations, fixes, and refactoring applied to ensure stability, smooth frame pacing, low CPU utilization, and clear diagnostics on 4-core / 4-thread systems (such as the Intel Core i5-4590, 14 GB DDR3, GTX 1650 4 GB) while dynamically adapting to other hardware configurations (AMD GPUs, hybrid P/E CPUs, variable RAM/VRAM, and audio configurations). It also documents features and stability improvements ported from Linux upstream 0.3.


## 0.6.1 (Windows port 0.6.1, party co-op)

- **Party logging for bug reports**: the launcher's *Advanced / Debug* tab has a *Party logging* level (Normal / Verbose / Trace, `BB_PARTY_LOG`) with area checkboxes (`BB_PARTY_LOG_AREAS`). Verbose tags every party line with the run id, slot, time since start, the shared party clock and local time, so the host's and the guests' logs line up; it adds a state snapshot every 10 s and events for loads (with the reason), deaths, the item box, prompts, the guest service menu and every message between the games (Trace: with contents). **F9** (or *Party tab > Mark an issue*) writes `=== USER MARK n ===` into every player's log. *Save support bundle* zips the recent logs, crash logs, `summary.log`, `bbport.ini` and an `info.txt` with passwords, party codes, tokens, IP addresses and the user name removed (also written automatically after a crash); the launcher keeps 20 session logs instead of 5. Details: [docs/party/logging.md](docs/party/logging.md).
- **CPUs without FSGSBASE start**: `wrfsbase`/`rdfsbase` are used only where CPUID reports them (checked once, cached); elsewhere the guest thread pointer stays in the Windows TLS slot and the crash handler answers the game's `fs:` reads from it. Nothing changes on CPUs with FSGSBASE (from Mrsuss60/bloodborne_pc_windows_port 336bacc by Mrsuss60).
- **Less VRAM growth from old depth buffers**: depth/stencil targets and shadow maps unused for a while are now freed by the texture collector (they are never read back), where before they were kept as GPU-written render targets and VRAM grew with every area visited (from Mrsuss60/bloodborne_pc_windows_port PR #89 by ALuaNegra).
- **One loading screen when a guest rests its way back to the host, checked live**: a guest resting at a lamp (travelling alone) back into the host's area used to see two loading screens (its own travel, then the host's reset load). Now the host's reset load runs first and the guest comes in after it with one loading screen, into the reset world.
- **Plain loading-screen texts in a party, checked live**: the party notice on a loading screen reads "Respawning with the party" / "Travelling with the party" instead of an internal name ("Travelling: host_death"); a guest's death screen no longer says "Returning to your world" while it stays with the party, and the host no longer sees "The bell's resonance has beckoned ..." again after a wipe's respawn (`BB_PARTY_BANNERS=0` shows every banner as the game does).
- **No empty bloodstain, checked live**: dying with 0 Blood Echoes no longer leaves an "Acquire Blood Echoes" stain with nothing in it, also after the respawn loading screen (which used to put the empty stain back) (`BB_PARTY_EMPTY_STAIN=1`: as the game does).
- **No "beckoned" banner after a wipe, checked live**: the host's "The bell's resonance has beckoned ..." after the party's respawn is hidden for real now (the join banner did not go through the filter); the first summon still shows it.
- **Joining a host that is still in its prologue, checked live**: the guest stays in its own world and shows "Waiting: the host is in its prologue" until the host is done.
- **Party name tags follow companions smoothly, checked live**: a tag now moves every frame with the companion it names (its in-game position read each frame, projected with that frame's camera); before it stepped about 10 times a second behind a moving companion (`BB_PARTY_HUD_TAGLOG=frame` logs the tag and position per frame).
- Test aids: scripted test steps that move the player wait while that player is dead, and every step waits while the party's respawn load and re-join run.
- **A guest's death after a wipe or a rejoin stays a party death, checked live**: after the party's respawn loading screen (host death, wipe) or a rejoin from the title, a guest's own character came back as an "own world" body instead of a co-op phantom, so its next death took the single-player path: it left the session, loaded its own world and had to be summoned again (in a wipe it missed the party's respawn and came back 50 s later). It is a phantom again a moment after such a load, so the next death or wipe follows the party as designed.
- **No title join into the host's prologue**: a guest starting (or restarting) its game while the host was still in its prologue was let in from the title (only the in-world summon checked it); it now waits in its own world ("Waiting: the host is in its prologue").
- **No more waiting after a guest rests alone, checked live**: when a guest rests at a lamp and travels on alone, the host's short reset load no longer waits a minute for that guest to arrive; a guest that rests its way back into the host's area loads along with the host and both carry on together.
- **A host travelling while you join no longer leaves you in an empty map, checked live**: the host's lamp travel waits (up to 30 s) until a guest it just let in has arrived, then everyone travels together; and a guest who asks to join just as the host's own warp starts is joined in the host's new map instead of the one the host is leaving.
- **No "summon stuck" restart because of the host's travel**: the host's session watchdog now waits out the host's loading screen and the re-join after it, instead of ending a join that was only waiting for the load (playtest 1).
- **No false "no arrival within 180 s"**: a guest restarting its game no longer makes the host report an arrival timeout at once (checked live).
- **The host's bell stays quiet while a guest's join is answered**: it rang for a moment just as a guest asked to join; now it rings only when the guest's own summon fails.
- **Enemy drops: one roll for the whole party** (playtest: "roll should be the same for all always"): every player still gets their own copy of an enemy's drop, but it is now the same item for everyone, from one shared roll per kill (each game seeds its roll from the host's world and the enemy, and uses the host's item discovery). Enemies roll anew after they respawn (a rest, a death or travel load of the host). `BB_PARTY_SHARED_DROP_ROLL=0` brings back an own roll per player.
- **No more frozen item box on a guest** (playtest #15, #12): a boss reward or key item for a guest now waits until the guest is alive, no cutscene plays and the boss-defeat banner is over before its "item acquired" box opens. A box left over a load or a death (or one that cannot be confirmed for 20 s) is closed; the items stay in the inventory. `BB_PARTY_ITEMGET_WATCH=0` turns the watchdog off.
- **Key items from bosses reach the guest even after a bad skip** (playtest #16): when a boss reward (Oedon Tomb Key, badges) was marked as already given but the guest holds none of it, the guest now gets it; every skip and give is logged.
- **The Old Hunter Bell is no longer lost for a guest**: a guest offered the bell in the host's Dream gets it in its own inventory and save (the game refused this per-player gift on a summoned guest, and nobody replayed it). Checked live. Test aid: `BB_PARTY_GIFT_TEST=10050` with `BB_PARTY_GIFT_TEST_HOSTONLY=1`.
- **A rejoining guest lands next to the host, not in the Hunter's Dream** (playtest 1): a guest that rang before its own map was known joined the host's room from the Dream without loading the host's area and sat there. The guest now waits for its own map, travels to the host's lamp with a normal loading screen first when it is in another area, and never takes a room from another map (the host refuses one too, and the guest's bell goes down while it travels). Live, over a simulated DSL line: guest save in the Dream, host in Central Yharnam: one load to the host's lamp, joined 3.4 s after the bell; guest game killed and restarted: back beside the host (1 m) 2.9 s after its bell.
- **No summons into a prologue**: the host refuses a direct summon while it (or the guest) has not finished its prologue; the guest's Party status says "Waiting: the host is in its prologue".
- **The bell rings only on the player who rang it**: the host's Beckoning Bell no longer rings along with every guest's bell; it rings only as the fallback when the guest's direct summon fails.
- **Join problems say why**: every refused or failed direct summon is logged with its reason on both sides (the host used to refuse some silently, e.g. with direct summon off), and a restarted guest is no longer refused for "a crashed member's connection is still held" once the host has let go of its old connection.
- **The host's death uses the loading screen again, checked live**: when the host dies with guests in its world, everyone gets one respawn loading screen at the host's lamp, as in the original game, and the world resets. This fixes what the first Internet playtest hit after a host death: broken geometry and walls after the death warp, and guests who could not pick up items, take their bloodstain or close an item box until their next load. The instant revive in place is still there as an option (`BB_PARTY_B2_HOST_REVIVE=1`). Dying in a boss fight still lets you watch until the fight ends.
- **Dying together is a party wipe**: when a guest goes down a moment before or after the host, the party now respawns together (a guest's death reached the host too late before, so the host got up alone).
- **A guest's rest resets the world for the party, checked live**: a guest who rests at a lamp and travels alone (No) now gives the host a short loading screen where it stands, and the enemies come back, as when the host rests. The travel question says so. Yes still moves the whole party.
- **A guest's lamp in the same map no longer moves the host**: the guest gets its own loading screen to that lamp and nobody is asked anything; before, the host was pulled across the map with it (playtest: the Hunter's Dream lamp after a level-up).
- **The travel question no longer hangs**: "Travel with the party?" closes by itself on a loading screen, when you die, or after 30 s without an answer (it stayed up for 2 minutes across the host's travel and death).
- **Bloodstains are logged**: the party log records when your bloodstain is dropped, taken back, or lost to a second death, to sort out missing echoes reports.
- **Fixed the host crash from playtest 1 (after ~70 min in a party)**: each frame's render commands come from a 256 KiB per-frame buffer; most likely a busy frame (two players and their effects in Old Yharnam) used it up and the game wrote through the failed allocation (guest +0x2199310). The buffer is now 1 MiB (`BB_FRAME_ARENA_KIB`), a second guard drops that one command for the frame instead of crashing (`BB_ARENA_GUARD`; test aid `BB_ARENA_FAIL_TEST=N`, checked live), and the log notes the fullest frame. Debug tracing no longer writes false `WATCHDOG: NO PROGRESS` dumps or `[BLOCKED >3s]` lines while frames still flip, and the launcher warns when tracing is on in a party.
- **Object motion off on AMD GPUs before RDNA3**: the RX 6600, 6650 XT, 6700 XT and 6900 XT lost the device at the first frame of the world with object motion on. It is now off on AMD RX 6000 and older (and AMD integrated GPUs); the upscalers use camera motion only there. `BB_OBJECT_MOTION=1` turns it on to test (from Supermedo/bloodborne_pc 4f30202).
- **No crash when video memory runs full**: on 6 GB cards at high resolutions an image or buffer the driver's VRAM budget refused stopped the game. It is now made past the budget (the driver may keep it in system memory: slower, but the game goes on; logged once), and past the critical mark old GPU-written render targets unused for a minute are freed (from Supermedo/bloodborne_pc 44e7644).
- **Game and launcher folders with parentheses work**: a path such as `C:\Games (x86)\Bloodborne` broke `run.bat` (cmd ended a block at the `)`) when the game folder was missing its `eboot.bin`, the Old Hunters folder was created, or a party restart message was shown. The launcher also hands `run.bat` the Python it runs on (from Mrsuss60/bloodborne_pc_windows_port 1af887a).
- **No more pops on loud hits**: when the sound queue ran low but was not empty, silence was put between the queued sound and the next block, a short gap that popped on strong hits and boss effects. Silence now goes in only when the queue has run dry (from Mrsuss60/bloodborne_pc_windows_port 367480b; its smoother limiter was already in).
- **NPCs are solid for guests**: Gehrman, Iosefka, Alfred, Eileen, the Oedon Chapel Dweller, Arianna and the other talk NPCs were see-through "wandering ghosts" in a guest's game (only the Doll had been fixed), which also could stop a guest from talking to them. Every talk NPC now looks and talks the same for guests as for the host; enemies stay as they were. `BB_PARTY_GUEST_SERVICES=0` brings back the old look. (Playtest 1 bugs 11, 17.)
- **Levelling up at the Doll no longer plays on the host**: when a guest levelled up at the Doll in the host's Dream, the host's hunter knelt and the host's Doll gestured too. Each player's level-up now stays in their own game (checked live). (Playtest 1 bug 10, the animation part.)
- **A cutscene never replays in the middle of a boss fight**: the host no longer plays a guest's copy of a scene it played itself in the last 10 minutes, or any guest's scene while a boss fight is on (Gascoigne's intro played a second time during the fight). A guest also no longer gets a catch-up replay of a scene it already watched in its own world (its first Hunter's Dream). (Playtest 1 bug 14 and its follow-ups.)
- **Clearer NPC talk logs for guests**: the logs now say when a guest's talk to an NPC in the host's world was let through, also outside test runs, to settle reports like "guests cannot knock on doors". Checked live: a guest knocks on the Central Yharnam residents' doors and gets their dialogue. (Playtest 1 bug 5.)
- **Blood Echoes for every kill in a party**: while a guest was in the host's world, killing an enemy paid nobody its Blood Echoes (the host got them only once the guest left, the guest never). Now each player gets the enemy's full echoes a few seconds after it dies, the host and every guest. Live: two host kills, the guest +48 echoes each, the host +48. `BB_PARTY_KILL_ECHOES=0` turns it off.
- **Madman's Knowledge for guests, checked live**: its Insight goes to a guest's own hunter in the host's world and stays in its save (guest game killed and Continued: Insight kept). Test aid: `guest_use=1500` / `host_use=ID`.
- **A guest's death no longer brings enemies back, checked live**: when a guest dies outside a boss fight and gets up at its lamp, dead enemies stay dead and nobody else reloads. A host death, a party wipe or a rest resets the enemies, as in the original game. Test aid: `BB_PARTY_ENEMY_CENSUS=1` logs the loaded map's enemies each second and any that come back alive without a load.
- **Spectating after a death in a boss fight, checked for guests, and you can switch whom you watch**: a guest who dies while the host fights on now watches the host (as a dead host already watched the guests), and gets up at the host's lamp with no loading screen when the boss dies; the host sees it come back there. Press Cross while spectating to watch the next living party member. Live (Cleric Beast): guest died, watched the host, boss killed, guest up at the lamp beside the arena, no load on either game.
- **The "host goes in first" notice at a first-fight boss fog now shows**: a guest at a boss fog that has no prompt yet because the host has not had that first fight (Father Gascoigne) is told why. The notice never showed because the guest's position was read from a field that stays where the guest arrived; it now uses the guest's real position. (Playtest 1 bug 8.)
- **Anyone can go through a boss fog first**: a guest at a boss fog no longer has to wait for the host; walking through starts the fight for the whole party (the host's game opens it), and the host and the others follow through the fog. First fights with an intro cutscene still start with the host. Live (Cleric Beast rematch): the guest went in first, the boss bar came up on both games 0.1 s later. `BB_PARTY_GUEST_FOG_FIRST=0` turns it off.
- **NPC gifts reach the whole party**: when an NPC hands a guest an item in the host's world, the host gets it too and then every other guest (the NPC's quest has moved on for everyone, so the gift was lost for them before). Only items an NPC or event gives once count (no map treasure, no per-player Messenger items), and nobody gets one twice. Live: guest gift -> host given the same lot -> sent to the party; the guest skipped its copy. Test aid: `BB_PARTY_GIFT_TEST=<lot>` on a guest.
- **Roam freely, checked live**: a guest walking away from the host with the stick (140 m apart, through the old multiplayer barrier walls, 5 minutes) stays in the party: nothing sends it home and the host keeps it as a cooperator. Test aid: `guest_stick=X:Z[:R[:STEPS]]` (or `obj:ENTITY:DIST:ANGLE`) walks the guest by the left stick, stopping early when blocked.
- **The Hunter's Dream mirror changes your appearance** (ported from [bbhost](https://github.com/droogie/bbhost)): the mirror in the workshop offers "Put on Disguise" and opens the game's appearance editor, a feature the developers left unfinished. Works in single player and for every party member on their own hunter (a guest in the host's Dream too). Launcher Game tab > *Hunter's Dream mirror: change appearance* (on; `BB_DREAM_MIRROR=0` turns it off); every player in a party needs the same setting (it changes the Dream's layout, so the party mod check compares it). The editor opens over the world, without character creation's book frame.
- **Memory Altar for guests, checked live**: a guest in the host's Dream gets the "Memory Altar" prompt and its menu (runes, covenants).
- **Mods no longer write into the game folder on Python 3.10 / 3.11**: without the symlink right, the mod overlay's folder links are junctions, which those Pythons did not recognize, so a mod file replaced the game's own file through the junction. Fixed; the overlay is also reused between launches while the mods stay the same (no new `mod-game-*` folder and no boot-image rebuild each start).
- **Progress check for guests (test tooling)**: a guest's own save was checked live to keep the lamps, shortcuts, boss defeats and story flags the host's world gave it, also after the guest's game was killed and Continued. Test aids: `host_kill=0` (nearest enemy), an Echoes change log line, and the save test fixture is no longer re-applied after a crash restart.
- **A cutscene no longer plays twice for a guest**: when the host triggers a story scene, the guest watches it with the host and its own copy of the event no longer starts the same scene again right after (was opt-in; now on, `BB_PARTY_STORY_DEDUPE=0` turns it off).
- **Every guest sees the guests who join after it**: a guest who joined from the title used to miss the players who came later (no character for them; the host also took ~48 s to finish a late guest's first sync). Every player in the world now asks a newcomer for its data, so all see all. Live (host + 2 guests, one after the other): presence 3 on every player, the host counts the second guest as it finishes loading.
- **No joining into a boss fight or cutscene by any path**: joining from the title and the direct summon now wait too while the host fights a boss or watches a cutscene (a guest used to load straight into the Cleric Beast fight). The guest plays on in its own world and is summoned right after the fight; the Party status shows "Waiting: the host is in a boss fight" (host: "Joiners wait: you are in a boss fight"). Live: refused at 48.7 s, kill 75.7 s, guest beside the host after.
- **Guests keep all the Insight the host's world gives**: a boss's first-encounter Insight and the kill's Insight a few seconds later both reach every guest present (the second one used to be dropped by a one-event-per-30-s limit). The anti-flood cap is now 20 Insight per minute, 10 per event. Live: story +1, then a boss kill -> guest Insight 6 -> 9.
- **NPC quest steps reach every guest's own save in full**: when an NPC moves on in the host's world (Iosefka 1141 -> 1143), the old state is now cleared in each guest's save too, and a guest catching up takes over the host's state of every NPC the host has met, so no save ends up with two states of one NPC. Live: guest `1141=1 1143=0` -> NPC state cleared 1 at the join catch-up.

## 0.6.0 (Windows port 0.6.0, party co-op)

- **Direct summon, on by default**: a ringing guest takes the host's room straight over the party link and hands it to its game as a summon message (ported from [bbhost](https://github.com/droogie/bbhost)), instead of waiting for the host's game to poll and summon a bell sign. Live: bell -> in the host's room in 3.0 s, in the host's world in 11.8 s (was ~20 s locally, 63 s in a real 2-PC session); the bell signs stay as the fallback. Launcher: Party > *Direct summon* (`BB_PARTY_DIRECT_SUMMON=0` turns it off).

## 0.5.9 (Windows port 0.5.9, party co-op)

- **Menu buttons act on Cross**: every action on the port's System pages (*New / Join / Continue party game*, *Host party*, *Join party*, *Single player*, *Rejoin*, *Leave*, *Kick*, *Restart the game*, *Quit the game*) and the text rows (*Host address*, *Password*, *Party name*) are now buttons with one value: Left/Right do nothing (they used to flip a two-value row showing *Press again* / *Copied*). Actions that restart or end something ask for a second press on the help line (*Press again to confirm*). Read-only lines (*Status*, *Last event*) do nothing. The value column on the focused row still shows the game's dimmed arrows.
- **No more vanishing rows**: the game's settings pages have six row slots; the solo title's *Party* page no longer lost *Party name* (no *Status* line there now), the role switches moved to a new *System → Party mode* page (*Host party*, *Join party*, *Single player*; also on the title in a party game), and *Party members* hides *Guests use lamps* / *Last event* as the party fills up. A unit test walks 3840 party states.
- **Copy Internet code / Copy LAN code**: a host gets two copy buttons instead of one that alternated between the codes; the help line shows the code.
- **Continue party game for launcher players**: shown whenever the party saves hold a character (it needed a party game started from the title menu before). In a party game started from the launcher, the title's *Party* page shows the role and "not open yet" / "not joined yet" instead of *Not running*.

- **Party name tags in the game's style**: in a party the game's own phantom plate (which vanished behind any tree or corner) is off; ours replaces it at every distance at the same spot, matched pixel for pixel to the game's plate live: the name in the plate's blue with a dark outline and shadow, where the game's name line sits (just above the bar, at its left end), over the same thin red bar with thin light end caps, one fixed size at every distance like the game's. New setting **Party name tags** (launcher Party tab, in-game System > Party rules, settings overlay; bbport.ini `party_name_tags`, on by default): off brings back the game's own plate as in the original game. The party HUD, notices and the settings menu's titles and labels now use Libre Baskerville (SIL OFL 1.1) like the game's UI.
- **Shorter party codes**: the Internet code is now `BBP2-XXXX-XXXX-XXXX-XXXX-XXXX` (was 29 characters), the LAN code `BBP2-XXXX-XXXX-XXXX-XXX` (it holds only the last number of the host's address; the guest adds the rest from its own). Old `BBP1-` codes still work; a host's code changes once with this update.
- **Faster re-join after a crash**: when a guest's game crashes, the host's game now drops its connection to the dead game at once (through the game's own connection release) instead of waiting ~25 s for it to time out; the restarted guest is back in the host's world ~12 s after its restart (was ~28 s).
- Checked every scripted warp of the game for needless loading screens inside the same map: the only ones are cutscene warps (boss intros, the Cathedral Ward evening, the ending) and Chalice layers, which keep the game's own behavior (no change in play).
- **Faster bell summons on a real network**: when a summon attempt fails, the guest's sign is offered again at once (no 30 s wait for the next sign), and a stuck attempt gives up after 8 s instead of 15 s. The first 2-PC LAN session took 63 s to summon because of two such failures.
- **Clear message when the host refuses you**: a wrong or missing password, an old code, a full party or a different version now shows one message box saying what to do; your game stays online alone in your own world (no more *Failed to acquire Bloodborne server information*), and *System → Party → Rejoin the host* retries with the password set there.
- **Per-player treasure**: guests can pick up items lying in the world (corpse glows) too, each player gets their own copy, and the host's pickup no longer takes it away from the others.
- **No crash when hit on Intel 12th gen and later** (from Supermedo/bloodborne_pc, DarkSpartan): stores of 607 small game functions now run below the area Windows overwrites when it handles a fault on those CPUs.
- **Louder game audio on stereo speakers and headphones** (from Supermedo/bloodborne_pc, andrecabralgit): the 7.1 mix is folded to stereo by the port with the front channels at full level (SDL's fold played it ~13 dB quieter); the volume boost setting is unchanged.
- **Mods with Developer Mode on**: replaced files are no longer seen by the game as 0 bytes (from Supermedo/bloodborne_pc, gStone).
- Overlay checkboxes and sliders keep their changes (from Supermedo/bloodborne_pc, Jgamrot).
- A pipeline the graphics driver cannot create (some AMD/older drivers) now skips its draws instead of stopping the game; fewer texture-cache stalls (unneeded hashing of streamed textures, a garbage-collector walk that never stopped early); SRT walker faults from speculative draw preparation no longer patch the shader walker (from Supermedo/bloodborne_pc).
- **Correct physics step with the FPS presets** (from Supermedo/bloodborne_pc, found by FIocker): the 60/90/unlocked presets wrote the physics step into unused padding with wrong values, so it never changed; it is now set at its real address (1/30 at 60 FPS as the game's own, 1/90 at 90, 1/120 unlocked, 1/60 for 30 FPS++). This changes the gameplay patch set: party players need the same version, as always.
- **Even frame pacing with a frame limit** (from Supermedo/bloodborne_pc): two flips are never closer than the limit's period (a frame after a late one used to be flipped early, and Windows' millisecond sleeps let 6-7 ms intervals through at a 120 cap, which slowed running down at high frame rates); slots are waited for precisely. The frame stats line also shows the shortest flip interval and the 1st percentile.
- Fewer GPU crashes (from Supermedo/bloodborne_pc): the texture cache never moves its images in memory, and pipelines with motion vectors are never preloaded from the cache (the driver refused some at start, others drew without motion output).
- **Keyboard movement** (after Supermedo/bloodborne_pc): two opposite keys held together give the one pressed last instead of stopping (rolling from A to D keeps a sprint), and a diagonal (W+D) is a full stick push on the circle instead of the square's corner.
- **Direct mouse camera** (imedved's Bloodborne mouse camera, ported by mcrib884, via Supermedo/bloodborne_pc): new option *Direct mouse camera* (launcher mouse & keyboard dialog, Insert menu, game menu *Mouse tuning*; bbport.ini `mk_direct_camera`, off by default): in mouse & keyboard mode the mouse turns the camera 1:1 like a PC game, with no stick dead zone or turn curve; lock-on keeps the stick look so a flick switches targets.

## 0.5.8 (Windows port 0.5.8, party co-op)

- **No more warps before the party forms**: a host's lamp travel or death no longer moves a player who is still in the prologue or playing in their own world without being summoned (a guest in the Hunter's Dream was warped into the clinic, or sent back to the Dream by the host's death).
- **Both party codes shown**: the launcher, *System → Party* and the overlay's Party tab show the host's **Internet code** (friends elsewhere) and **LAN code** (same network), each with its own copy button. The launcher used to show the raw `internet=...` line.
- **Same-network fallback**: a guest on the host's network using the Internet code (routers without hairpin NAT gave "Failed to acquire Bloodborne server information") now finds the host on the LAN by itself, in about 2 s. Probes carry only a hash of the code's secret; the host answers only its own party, rate-limited.
- **Saved password ignored for codes without one**: a password left in the guest's launcher no longer causes "wrong password or party code" when the host has none.
- **LAN bell join fixed**: a guest that reached the host on its LAN address sent the game's traffic to the host's public address (the router dropped it; the bell join timed out after ~48 s). It now uses the host's LAN address, and a peer that answers on its LAN address is reached there.
- **Password checks before joining**: the launcher (and the title screen's *Party* page) says "This party needs a password" when the code needs one and the password is empty.
- **Clearer join refusal**: the guest's notice and *System → Party* say "Old party code: ask the host for the current one", "This party needs a password" or "Wrong party password" instead of only the game's "Failed to acquire Bloodborne server information".
- **Launcher Stop closes the game normally**: Stop and quitting the launcher ask the game to close (party goodbye, UPnP mapping removed, GPU released) and kill it only after 8 s. A force-kill could leave bbport.exe stuck with port 9307 held.
- **Gesture and Personal Effects menus** open again from the emulated touchpad (Mrsuss60).
- **Shader cache kept between starts**: motion vector pipelines are no longer saved; they could never be reloaded, so every start removed them again ("N stale pipelines") (Vorshim92).

## 0.5.7 (Windows port 0.5.7, hotfix)

- **No automatic button presses in manual play**: starting a party from the launcher pressed Confirm repeatedly through character creation and name entry. The automatic *Continue* now runs only when asked for (automated tests, *Continue party game* / *Join party game* with an existing character, a crash restart) and stops after 40 s.

## 0.5.6 (Windows port 0.5.6, party co-op)

- **Per-player enemy drops** (on by default): every enemy drops loot for each player separately, and guests keep their own drops.
- **Players who join later are visible to everyone** (with *Continue party game* joins, earlier guests didn't see later ones).
- **The 4th player joins reliably**; 5-player parties work in tests (`BB_PARTY_MAX=5`, experimental, not in the launcher).
- **Fixes**: a guest's Continue after travelling alone starts at its own spot; a title join no longer records an empty spot (a later Continue could fall through Central Yharnam); crash re-join back to about 28 s (0.5.5 had made it 41 s).
- **Known issues**: guest treasure pickups are not per-player yet; existing players flash briefly when a new one arrives; crash re-join is still ~28 s.

## 0.5.5 (Windows port 0.5.5, party co-op)

- **Continue party game straight into the host's world**: one loading screen, the guest lands beside the host, even when its own save is in another map; falls back to the normal join if the host isn't up yet.
- **Spectating**: a player who dies in a boss fight watches a living party member until the fight ends.
- **Resting during a boss fight** is a real rest (the fight and enemies reset).
- **Joining a host in another map**: the guest gathers to the host's map first and is placed beside the host.
- **README**: step-by-step "Start playing" for host and guest.

## 0.5.4 (Windows port 0.5.4, fixes)

- **Party wipes work again**: in 0.5.3 a guest's "down" message was ignored (a name clash with split travel), so a host death after a guest's became a revive instead of a wipe. A guest left dead by a same-map wipe is now revived beside the host.
- **Guest starts at its own spot after Continue**: party travel, wipes and lone re-joins no longer overwrite the guest's own save position with the spot beside the host.
- **Guest bloodstain**: recoverable in the guest's own world (documented).
- *Continue party game straight into the host's world* (one loading screen) is in, but off by default until verified (`BB_PARTY_TITLE_JOIN=1`).

## 0.5.3 (Windows port 0.5.3, stability release)

- **Party co-op fixes**: the invisible guest on the host is fixed at its root (rebuilt characters are shown the way the game shows a summoned phantom); the guest grey screen after a party travel is fixed; the party HUD and name tags hide during cutscenes.
- **Louder sound by default** (1.4x master volume with a soft limiter; the old limiter dropped to silence at full scale). *Sound* page in the game's System menu; `BB_AUDIO_BOOST=1.0` restores the original level.
- **Window mode**: windowed / borderless / fullscreen (`BB_WINDOW_MODE`, launcher).
- **From Mrsuss60's Windows port**: Microsoft Store Python found by the launcher and run.bat; F / V also heal; a GPU command padding guard. A rare DMA packet (DAS=1) is skipped instead of asserting.
- **Test tooling**: grey-screen detector; the reaper handles time-stamped logs.
- **Known issues** (unchanged from 0.5.2): a guest who died while summoned can start in the wrong place after Continue; a guest's bloodstain is not visible in the host's world; no spectator camera in boss limbo; 4-player joins unreliable; 2-PC play untested.

## 0.5.2 (Windows port 0.5.2, party co-op milestone)

- **Party co-op** (still in development; README *Party co-op*, docs/party/ERSC_PARITY.md ~81%):
  - Travel: "Travel with the party?" at lamps, headstones and the Hunter's Mark; travelling alone loads only the traveller; no loading screen for warps inside the loaded map.
  - Deaths: the host and guests respawn at their own lamp without a load when it is in the loaded map; dying in a boss fight waits until the fight ends; bloodstains for guests; rot (death debuff, on by default).
  - Guests: open doors and pull levers in the host's world, talk to NPCs (quest choices shared), trigger cutscenes for the whole party, get Insight.
  - Joining: waits outside boss fights, faster connect, no "Session lost" after Leave, the host's End party sends guests home cleanly.
  - Chalice dungeons: any player's ritual, guest summons, boss loot per player, lifts.
  - Balance: enemy scaling settings (harder than vanilla by default), names over party members.
  - Fixes: invisible guest on the host (missing draw bit), guest grey screen going to the Dream alone, crash when players stand in different map areas, guests stuck dead after a boss, wrong HP field.
  - Internet: tested under simulated lag, loss and outages; guest codes for hosts without port forwarding.
  - Version check: different builds or party options are refused with a clear message.
- **Faster startup**: the prepared game image is cached (run.bat steps ~14 s -> ~1 s); quicker automatic Continue.
- **Known issues**: guest position can drift on Continue after dying while summoned; no spectator camera while dead in a boss fight; 4-player joins unreliable; the 4th player and 2-PC play are untested.

## 0.5.1 (Windows port 0.5.1, with upstream bbport 0.51)

- **Upstream 0.51** ([docs/UPSTREAM_0.51_INTEGRATION.md](docs/UPSTREAM_0.51_INTEGRATION.md)): game profile for every 1.09 retail region, motion blur smear fix, 4 GB VRAM fallback, title flicker fix above 1080p, swapchain recreation fix, DLSS library choice in the launcher, mouse wheel and mouse bindings in mouse & keyboard mode, *Quit the game* in the overlay and System menu. The shadNet online module is not included (needs a server; conflicts with party co-op).
- **Party co-op** (in development; see the README's *Party co-op* section): in-game New / Join / Continue / Leave party flow, lamps and headstones for everyone, host-world state shared live, guest lone-death respawn, re-join fixes, crash fix when players split across map areas, internet robustness (lag / loss / outage tested), guests placed 0.5 m beside the host.
- **README** rewritten as a short page; details moved to docs/ (BUILDING, SETTINGS, SHADERS, SAVES, WINDOWS_PORT, CREDITS).

## 0.5 (Windows port 0.5, merged with upstream bbport 0.5)

- **Version**: 0.5 everywhere: `VERSION` at the repository root feeds CMake (`project(... VERSION)`, the `BBPORT_VERSION` define and the `bbport.exe` version resource), `bbport.exe --version` and its first log line, the launcher's title bar and the overlay menu's corner.
- **Upstream 0.5** ([docs/CHANGES_0.5.md](docs/CHANGES_0.5.md)): native game settings pages (*Display*, *Effects*) with slider choices, the command-processor decoder (`gpu/cp`, `cp-decoder-test`) and game profiles (`gpu/games`), GPU timestamps, occlusion queries behind `BB_OCCLUSION_QUERIES=1`, the memory layer's shared write traps (`runtime_memory_trap`), VRAM churn fixes and the texture collector's emergency pass.
- **Windows adaptations**: the write trap table (runtime) and the image watcher table (page manager) are reserved with `VirtualAlloc` and committed in 64 KiB chunks instead of `mmap(MAP_NORESERVE)`; traps are applied with `VirtualProtect` and kept on view pieces the Windows runtime remaps; fault addresses come from the Windows exception context; render-pass-break diagnostics use `GetModuleHandleEx` instead of `dladdr`.
- **Kept from this port**: the async pipeline compiler and shader precompile (upstream's rule that full-screen passes and indirect draws never go without their pipeline is folded in), the second-based texture collector ages (upstream's `BB_GC_PRESSURE_IDLE_SECONDS` overrides them), the tabbed overlay menu, `launcher.py`, `run.bat`, the bbhost stability fixes and party co-op (still in development).
- **Memory model**: upstream's *Auto* (the new model on AMD) is Linux-only; `run.bat` keeps the 0.3 model unless `BB_PC_MODEL=1` is set by hand.

---

## 1. Crash Analysis & Resolutions (Wolf Enemy Combat & Stability)

### A. Dynamic VRAM GC Thresholds (`gpu/shadps4/video_core/texture_cache/texture_cache.cpp`)
- **Root Cause**: On 4 GB GPUs like the GTX 1650, texture cache garbage collection was set to trigger critical eviction only at 97% of VRAM. When the player entered combat with the first wolf enemy, dynamic particle effects, blood decals, and new texture allocations pushed VRAM beyond 100%, triggering `vk::Result=ErrorOutOfDeviceMemory STOP: GPU library assertion failed`.
- **Change**: Configured safe, progressive dynamic GC thresholds (70% trigger, 80% pressure, 88% critical eviction), fully configurable via environment variables (`BB_GC_TRIGGER`, `BB_GC_PRESSURE`, `BB_GC_CRITICAL`). This ensures stale images are evicted smoothly before GPU memory is exhausted.
- **Presenter Resilience** (`gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp`): Added fallback allocation without `WITHIN_BUDGET` flag and host memory fallback in `RecreateFrame` during transient memory pressure.

### B. Remnant Slice Remapping in Win32 Memory Views (`src/win32_memory.c`, `src/runtime_memory.c`)
- **Root Cause**: Windows memory virtualization using `VirtualAlloc2` placeholders and `MapViewOfFile3` was dropping entire 64 KiB section views when guest code partially unmapped an allocation, failing to preserve and remap remnant prefix and suffix slices.
- **Change**: Implemented view slice tracking and automatic remnant remapping with `restore_remnant()`. Added a 64-entry lock-free ring buffer tracking recent memory operations (`g_mem_op_ring`), dumped during crash diagnosis.

### C. Removal of Silent Null Skips in VEH (`src/probe.c`)
- **Root Cause**: The Vectored Exception Handler had silent instruction skips (`8b 04 07`, `8b 3c 19`, `c5 fa 5c`) that forced destination registers to zero. This masked upstream null pointer bugs and cascaded into all general-purpose registers (RAX, RBX, RCX, RDX, RDI, RBP) becoming 0, culminating in the crash at guest offset `0x28ce9b5`.
- **Change**: Removed silent skips. If an unhandled memory fault occurs, a full crash dump (`out/crash_<timestamp>.log`) is generated with registers, stack frames, RBP chains, and faulting thread import history before clean termination.

### D. Pipeline Cache SRT Walker Fault Handler (`gpu/shadps4/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp`)
- **Root Cause**: Restored pipeline cache entries invoked SRT walkers without ensuring the signal handler was registered, leading to access violation crashes.
- **Change**: Added `EnsureSrtFaultHandler()` on walker registration and wrapped concurrent instruction patching in a mutex (`patch_mutex`) to support parallel draw-preparation workers.

### E. AvPlayer Thread Safety (`gpu/shim/core/libraries/kernel/threads.h`)
- **Root Cause**: AvPlayer demuxer and decoder threads attempted to join themselves or collided with owner destructors during playback teardown.
- **Change**: Implemented thread-safe `Take()`/`Finish()` pattern under `std::mutex` ensuring clean detachment and non-reentrant joining.

### F. High-Entropy ASLR & Address Space Collision (Error 487) (`gpu/CMakeLists.txt`, `src/win32_memory.c`, `src/probe.c`, `src/runtime_memory.c`)
- **Root Cause**: Windows 64-bit High-Entropy ASLR and DynamicBase were enabled by default on `bbport.exe`. Windows randomized the main thread stack and CRT heap into `[0x800000000, 0xfc00000000)` (~1 TB guest address space). When `VirtualAlloc2` attempted to reserve the full 1 TB placeholder, it collided with the preexisting thread stack and returned `Windows error 487 (ERROR_INVALID_ADDRESS)`. As a result, `space()` failed, `runtime_low_map` could not allocate, image loading fell back to host allocations exceeding 40 bits (`1<<40`), and guest thread stack allocations failed with `"Cannot allocate guest thread"`.
- **Change**:
  1. Linked `bbport.exe` with `-Wl,--disable-dynamicbase` and `-Wl,--disable-high-entropy-va` in `gpu/CMakeLists.txt`, ensuring bottom-up memory layout below 32 GB.
  2. Called `runtime_memory_reserve_space()` at the very start of `main()` in `src/probe.c` before Vulkan, driver DLLs, or background worker threads are initialized.
  3. Added a chunked placeholder fallback in `win_mem_space()`: if a single 1 TB placeholder fails due to any preexisting system or hook allocation, it queries memory and reserves all free regions in `[start, end)` as placeholders, setting `space_start` and `space_end` successfully.

---

## 2. Audio Subsystem Refactoring (Distortion & Stuttering Fixes)

### A. Eliminated High-Priority Spinloop (`src/runtime_audio.c`)
- **Root Cause**: `output_port` ran a tight busy-wait loop (`__builtin_ia32_pause()`) while the thread was set to `THREAD_PRIORITY_TIME_CRITICAL`. On a 4C/4T CPU, this locked one core at 100% and preempted FMOD mixer and audio worker threads, causing severe underruns, robotic voice, and distorted sound.
- **Change**: Replaced spinning `sleep_until` with `host_sleep_until_ns` waitable timers and demoted audio thread priority to `THREAD_PRIORITY_HIGHEST`.

### B. Queue Steering & Cadence Alignment (`src/runtime_audio.c`)
- **Change**: Restored queue steering against the hardware WASAPI clock. The minimum queue level over a 32-buffer window is tracked and steered towards a 2-period target buffer via `adjust_ns` (±3% cadence corrections), absorbing clock drift between CPU QPC and audio DAC clocks.
- **Prefill Headroom**: Added `BB_AUDIO_PREFILL` environment variable support (default 2 periods) for configurable buffer headroom.

### C. Soft-Knee Saturation Limiter (`src/runtime_audio.c`)
- **Root Cause**: 8-channel audio was hard-clipped at `[-1.0, 1.0]`, causing square-wave clipping and harsh buzzing during multi-channel audio peaks.
- **Change**: Replaced hard clipping with a smooth soft-knee saturation curve (`soft_clip`), preventing square-wave distortion while preserving audio dynamic range.

### D. Scratch Buffer in AJM Decoding (`src/runtime_ajm.c`)
- **Change**: Replaced per-job heap `malloc`/`free` calls during multi-chunk input decoding with a 4096-byte stack scratch buffer, reducing heap allocation overhead during ATRAC9 audio playback.

---

## 3. CPU Performance & Frame Time Smoothing (4C/4T Haswell & Dynamic Hardware Adaptation)

### A. Native Win32 Synchronization Primitives (`src/host_sync.h`)
- **Change**: Replaced winpthreads overhead with native Windows `SRWLOCK` and `CONDITION_VARIABLE` across `runtime_mutex.c`, `runtime_rwlock.c`, `runtime_sema.c`, `runtime_thread.c`, `runtime_kernel.c`, `runtime_savedata.c`, and `runtime_pad.c`.

### B. High-Resolution Waitable Timers (`src/runtime_host.c`)
- **Change**: Implemented waitable timers using `CreateWaitableTimerExW` with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` on Windows 10/11, enabling sub-millisecond sleep precision without burning CPU cycles in spinloops.

### C. Process Scheduling & Power Throttling (`src/runtime_thread.c`)
- **Change**: Configured `ABOVE_NORMAL_PRIORITY_CLASS` and explicitly disabled Windows EcoQoS / power throttling via `SetProcessInformation(ProcessPowerThrottling)`, ensuring CPU cores maintain full execution clock frequency.

### D. Thread Priority Mapping (`src/runtime_thread.c`)
- **Change**: Refined PS4 priority translation (`ps4_prio_to_win32`) to cap normal guest worker threads at `THREAD_PRIORITY_HIGHEST`, preventing 60+ guest threads from monopolizing 4 physical CPU cores.

### E. Dynamic Core Count Detection & `BB_NCPU` (`src/runtime_kernel.c`)
- **Root Cause**: `guest_sysctl` hardcoded `hw.ncpu = 7`, misleading the guest engine into scheduling across 7 cores on 4-core systems.
- **Change**: Implemented dynamic core detection via `GetSystemInfo` / `sysconf`, with an explicit `BB_NCPU` override environment variable.

### F. Removed Redundant Syscall Checks (`gpu/shadps4/video_core/renderer_vulkan/vk_bind_helper.h`, `vk_scheduler.h`)
- **Change**: Replaced per-draw `std::jthread::joinable()` syscall checks with atomic boolean flags (`running`, `recorder_running`), eliminating repeated OS kernel transitions during render command recording.

### G. Non-SEH Register Context Restoration (`src/runtime_host.c`, `src/runtime.h`)
- **Change**: Implemented `runtime_setjmp`/`runtime_longjmp` restoring all Win64 callee-saved registers without triggering Windows SEH stack unwinding across guest frames lacking xdata unwind metadata.

---

## 4. Launcher & Diagnostics (`launcher.py`, `src/probe.c`)

### A. Pre-Launch Verification Check & Setup Audit (`launcher.py`)
- **Feature**: Added comprehensive pre-launch verification (`verify_prelaunch`) and dedicated `✓ Verify Setup` UI button:
  - Verifies game directory exists and contains a valid, non-empty `eboot.bin` (reporting exact file size in MiB).
  - Checks for game asset directories (such as `dvdroot_ps4`).
  - Verifies compiled native binaries (`out/bbport.exe` and `out/bbgpu.dll`), ensuring they have non-zero size and logging their build compilation timestamps.
  - Verifies runtime launch scripts (`run.bat`, `scripts/patches.py`).
  - Checks Vulkan runtime driver availability via `vulkan-1.dll`.
  - Automatically runs before launch and displays detailed diagnostic checklist in the launcher console and GUI dialogs.

### B. Feature Toggles & Vanilla Mode (`launcher.py`)
- **Toggles**: Independent checkboxes for Upscaler (FSR), Object Motion, Overlay Menu, FPS Patch, Mods Folder, Resolution Scaling, Debug Tracing, Watchdog, and Draw Prep Workers.
- **Presets**: "Vanilla mode" sets all optional extras off with 30 FPS native lock; "Everything on" restores defaults.

### C. Enhanced Crash Diagnostics (`src/probe.c`)
- **Crash Log**: Automatically writes `out/crash_<timestamp>.log` containing exception code, fault address, guest eboot offset, full x86-64 register state, 16 stack frames with symbol mapping, 16 guest RBP frames, last 32 imports of the faulting thread, and recent memory operations dump.
- **Summary Log**: Periodically writes and finalizes `out/summary.log` tracking runtime, flips, frametime percentiles (p95/p99), freeze durations, and audio underrun counters.

---

## 5. Save Data Mount & Performance Regression Fixes

### A. Title Screen "Failed to create save data" Fix (`src/runtime_savedata.c`)
- **Root Cause**: `exports[]` in `src/runtime_savedata.c` only matched old `#p#J` hashes. Bloodborne's EBOOT imports `#O#P` NIDs (e.g., `ZkZhskCPXFw#O#P` for `sceSaveDataInitialize`, `32HQAQdwM2o#O#P` for `sceSaveDataMount`). Because of this mismatch, all save data calls resolved to NULL and executed unbound stubs, leaving `/savedata0` unmounted and halting the game at the title screen with *"Failed to create save data. Press OK to try again."*
- **Change**: Added all `#O#P` NIDs and plaintext symbol names to `exports[]`. Enhanced `make_dirs()` to handle both Windows backslashes (`\`) and POSIX forward slashes (`/`).

### B. Framerate Drop (10 FPS Regression) & CPU Spun Loops Fix (`src/runtime_kernel.c`, `src/probe.c`)
- **Root Cause**:
  1. `exports[]` in `src/runtime_kernel.c` lacked Bloodborne's actual NIDs for `sceKernelClockGettime` (`QBi7HCK03hw#p#J`), `sceKernelUsleep` (`1jfXLRVzisc#p#J`), and `sceKernelReadTsc` (`-2IRUCO--PM#p#J`). `usleep` returned 0 without sleeping, spinning CPU cores at 100%.
  2. In `src/probe.c`, unbound import execution called `printf` and `fflush(stdout)` unconditionally on every single call without checking `g_bb_trace`. With unresolved/unbound imports being invoked in fast loops, this caused over 400,000 unbuffered console I/O writes and flushes, bottlenecking the process and dropping framerate from 30 FPS down to ~8-10 FPS.
- **Change**: Bound correct Bloodborne timer/sleep NIDs in `src/runtime_kernel.c`, and strictly gated unbound import logging in `src/probe.c` behind `if (g_bb_trace && count <= 3)` with low call caps.

---

## 6. Low-VRAM (4 GB) & Low-Core (4C/4T) Hardware Adaptation

### A. VRAM Budget Scaling & Thrashing Elimination (`gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp`, `vk_instance.h`, `texture_cache.cpp`)
- **Root Cause**:
  1. On discrete GPUs, `CollectPhysicalMemoryInfo` subtracted a 1 GB system memory reserve from the dedicated VRAM budget regardless of total VRAM size. On a 4 GB card (GTX 1650), this reduced the usable budget from ~3800 MiB to ~2825 MiB.
  2. In `TextureCache::GarbageCollectImages`, the default trigger threshold was hardcoded to 70% of budget (`2825 * 0.70 = 1977 MiB`), pressure at 80% (2260 MiB), and critical eviction at 88% (2486 MiB).
  3. Bloodborne's base working set in Iosefka's Clinic is ~2500–2600 MiB. On 8 GB GPUs (like the RX 6600), the 70% threshold is ~4900 MiB, so GC never triggers (0 evictions). On 4 GB GPUs, 2500 MiB immediately exceeded the 88% critical mark, forcing the texture cache into a permanent critical eviction panic.
  4. Every 5 seconds, 1389 images were evicted and downloaded back across PCIe using `scheduler.Finish()`, stalling the CPU/GPU pipelines and tanking frame rate from 30 FPS down to 10 FPS.
  5. During combat with the Scourge Beast, particle stream buffers and blood decal textures were evicted while being accessed by guest draw routines, leading to page-watcher invalidation and guest register corruption (`address 0` fault at `0x28ce9b5`).
- **Change**:
  1. Exposed `GetDeviceLocalMemory()` in `Instance` to query physical dedicated VRAM size.
  2. In `CollectPhysicalMemoryInfo()`, restricted the 1 GB system reserve subtraction to GPUs with > 4 GB VRAM, preserving the full physical VRAM pool for <= 4 GB cards.
  3. Dynamically scale GC budget on <= 4 GB GPUs up to 88% of physical VRAM (~3600 MiB on 4 GB cards), with dynamic low-VRAM thresholds: trigger at 88% (3168 MiB), pressure at 93% (3348 MiB), and critical at 97% (3492 MiB). This allows Bloodborne's 2500–2600 MiB working set to remain resident without thrashing.
  4. Protected recently touched images from aggressive eviction by raising `ticks_to_destroy` to 900 ticks (30 seconds) in aggressive mode and 1800 ticks (60 seconds) in pressured mode, preventing active combat encounter assets from being destroyed.
  5. Throttled synchronous downloads (`gc_downloads`) to at most 1 per frame to eliminate multiple `scheduler.Finish()` pipeline flushes.

### B. 4C/4T CPU Thread Scheduling & Starvation Resolution (`src/runtime_thread.c`, `src/host_sync.h`)
- **Root Cause**:
  1. `ps4_prio_to_win32` assigned `THREAD_PRIORITY_HIGHEST` to any thread with priority `<= 260`. Bloodborne spawns over 20 background worker threads (5 `GXWorker`, `HavokWorkerThread`, 6 `CSChrThread`, 6 `CSClothThread`, 6 `SpClothVertexUpdate`) with priority `256`.
  2. On a 12-thread CPU (Ryzen 5600), hardware threads handle these workers easily. On a 4-core / 4-thread CPU (i5-4590), 20 workers running at `THREAD_PRIORITY_HIGHEST` completely preempted and starved the main game loop thread and Vulkan presenter (running at default `THREAD_PRIORITY_NORMAL`).
  3. `host_yield()` called Win32 `SwitchToThread()`, which yields only to threads on the same physical core; when no other thread is scheduled on that core, it returns immediately without yielding, causing high-CPU spin contention.
- **Change**:
  1. Added dynamic CPU core count detection (`get_host_cores()`). On systems with 4 cores or fewer, worker pool threads (`prio >= 250`) are assigned `THREAD_PRIORITY_NORMAL` (or `BELOW_NORMAL`), while audio threads maintain elevated priority.
  2. In `runtime_thread_attach_main()`, elevated the main game loop thread to `THREAD_PRIORITY_ABOVE_NORMAL`, preventing background workers from starving the main loop on low-core CPUs.
  3. In `host_sync.h`, updated `host_yield()` to fall back to `Sleep(0)` when `SwitchToThread()` fails, giving up the time slice to any ready thread across all cores.

---

## 7. Combat / Hit Blood Particle Generator Stack Unwind (`src/probe.c`)

### A. Clean Frame Restoration for Impact Particle Generation (`0x28ce7b0 .. 0x28ceeb8`)
- **Root Cause**:
  1. Function `0x28ce7b0` is the blood splatter/impact vertex stream generator invoked when any enemy lands a hit on the character.
  2. The function relies on the System V AMD64 128-byte Red Zone (`[rsp - 0x60]`, `[rsp - 0x38]`, etc.). On Windows, memory pressure / write-tracking faults invoke kernel exception dispatch (`KiUserExceptionDispatch`), which allocates 1.3 KB below `RSP`, clobbering these Red Zone local pointers.
  3. A prior recovery attempted to branch mid-function to `0x28cea00`. Jumping into the middle of the loop skipped stream initialization and cascaded into reading `[rcx + rdx] = [0 + 4] = 0x4` at `0x28cea45`.
- **Change**:
  1. Updated `win_veh_handler` in `src/probe.c`: if any fault occurs within the entire function body (`0x28ce7ba .. 0x28ceeb8`), it safely reads the preserved callee-saved registers from the stack frame (`RBX`, `R12`, `R13`, `R14`, `R15`, `RBP`), pops the return address, restores `RSP = RSP + 0x68`, and returns directly to the caller (`0x2ab485c` / `0x2ac458e`).
  2. The caller's code immediately completes the hit reaction and frame without crashing, allowing gameplay to proceed smoothly on all enemy hits.
---

## 8. Linux Upstream 0.3 Port to Windows

### A. Stability: Atomic Label Writes (Guest Fault `0x263b8e7` Fix)
- **Root Cause**: `memcpy` copies 4-7 bytes using two overlapping 4-byte stores. On preemption, the second store could land after guest code freed the label block, corrupting the heap free list with a pointer ending in `...00000004` (guest fault `0x263b8e7`).
- **Files Modified**: `src/runtime_memory.c`, `gpu/shadps4/video_core/amdgpu/liverpool.cpp`, `gpu/shadps4/video_core/amdgpu/pm4_cmds.h`.
- **Change**: Replaced `memcpy` in `runtime_memory_write_backing` with `store_once()` using aligned 64/32/16/8-bit release atomic stores. In `liverpool.cpp`, implemented `StoreLabel()` using `__atomic_store_n(..., __ATOMIC_RELEASE)` in `SignalEop` and `RunEventWriteEos`. In `pm4_cmds.h`, used atomic release stores for `ReleaseMem::SignalFence`.

### B. Stability: GPU Breadcrumbs & Diagnostics
- **Files**: `gpu/shadps4/video_core/renderer_vulkan/vk_breadcrumbs.h`, `vk_breadcrumbs.cpp`, `gpu/bbgpu.h`, `gpu/shim/bbgpu.cpp`, `src/probe.c`.
- **Change**: Ported `vk_breadcrumbs` using `VK_AMD_buffer_marker`. Integrated `bbgpu_dump_breadcrumbs(f)` into `dump_hang_snapshot()` and `write_crash_dump()` in `src/probe.c`, recording active render pass and draw information directly to crash dumps.

### C. Pre-flight Game Check (CUSA03173 v1.09)
- **Files**: `scripts/game_check.py`, `launcher.py`.
- **Change**: Ported `scripts/game_check.py` and connected `verify_prelaunch()` in `launcher.py`. Validates Game ID (`CUSA03173`) and version (`1.09`), providing explanations for missing or mismatched game files, and supporting bypass via `BB_SKIP_GAME_CHECK=1`.

### D. Memory / VRAM Collector Base
- **Files**: `gpu/shadps4/video_core/texture_cache/image.cpp`, `gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp`, `gpu/shadps4/video_core/texture_cache/texture_cache.h`, `texture_cache.cpp`.
- **Change**:
  1. Configured 64 MB VMA blocks via `BB_VMA_BLOCK_MB` (default 64) to prevent fragmented blocks from being held by empty allocations.
  2. Added dedicated memory allocation (`VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT`) for large images $\ge$ `BB_VMA_DEDICATED_MB` (default 4 MiB).
  3. Ported upstream 20-second idle texture collector (`BB_GC_IDLE_SECONDS=20`) in `TextureCache::GarbageCollectImages` while preserving the Vulkan budget query (`VK_EXT_memory_budget`) and dynamic thresholds for low-VRAM GPUs.

### E. Camera Motion Vector Vertical-Axis Fix
- **File**: `gpu/shadps4/video_core/host_shaders/camera_motion.comp`.
- **Change**: Replaced inverted NDC/UV vertical sign convention (`1.0 - uv.y * 2.0`) with screen-space row mapping (`uv.y * 2.0 - 1.0`), eliminating stair smearing in FSR 4 and stair blur in FSR 3.1 within 0.05 px of GPU per-vertex motion.

### F. Anisotropic Filtering (`BB_ANISO`)
- **Files**: `gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h`, `vk_rasterizer.cpp`, `gpu/shim/bbport_toggles.h`, `launcher.py`.
- **Change**: Added `Rasterizer::ForceAnisotropy` clamped to `instance.MaxSamplerAnisotropy()`. Integrated an Anisotropic Filtering dropdown into `launcher.py` with dynamic descriptions, respecting Vanilla Mode (`BB_ANISO=0`).

### G. Patches & High FPS Enhancements
- **Files**: `patches/Bloodborne.xml`, `scripts/patches.py`.
- **Change**:
  1. Ported `Sprint Fix (High FPS)` for 90 FPS and uncap modes (switches wall distance detector from per-frame to speed).
  2. Ported Intel CPU detection (`intel_cpu` / `intel_tonemap_fix`) and automated activation of `Intel Black Tonemap Fix` on Intel CPUs with `BB_INTEL_TONEMAP_FIX` override.

---

## 9. 0.1% and 1.0% Frametime Stutter Elimination & Performance Enhancements

### A. Critical Path Thread Priority Scheduling (`src/runtime_thread.c`)
- **Root Cause**: On 4-core / 4-thread CPUs, `ps4_prio_to_win32()` assigned `THREAD_PRIORITY_NORMAL` to all threads with `prio <= 260` (the 12 background worker threads: `EzWorkPool:0..3`, `GXWorker:0..5`, `ClothWorker`, `HavokWorker`), but downgraded threads with `prio <= 700` to `THREAD_PRIORITY_BELOW_NORMAL`. Bloodborne's primary rendering thread (`GXRenderThread`) and main game loop (`MOMainThread`) are created with PS4 default `prio = 700`. As a result, the critical GPU submission thread ran at lower priority than 12 background workers. During enemy encounters and physics simulations, the workers preempted `GXRenderThread`, causing rendering stalls (`== Stall during rendering at flush 1046`), pipeline bubbles, and severe 1% / 0.1% low frametime drops.
- **Change**:
  1. Explicitly elevated critical path rendering and main threads (`GXRenderThread`, `RenderThread`, `Present`, `Flip`, `Display`, `MOMainThread`, `MainThread`) to `THREAD_PRIORITY_ABOVE_NORMAL`.
  2. Demoted background worker pools (`Worker`, `Pool`, `Work`, `Cloth`, `Havok`, `Job`) to `THREAD_PRIORITY_BELOW_NORMAL` on systems with $\le$ 4 cores, ensuring the GPU command submission pipeline is never starved or preempted.
  3. Updated `thread_rename()` to dynamically update Win32 thread priority if a thread was created anonymously and renamed after startup.

### B. Texture Cache Anti-Thrashing & Calibrated VRAM Thresholds (`gpu/shadps4/video_core/texture_cache/texture_cache.cpp`)
- **Root Cause**: On 4 GB discrete GPUs (e.g. GTX 1650), Bloodborne's active scene memory hovers around 3.1–3.2 GB. Under the previous calculation, `pressure_gc_memory` was dynamically clamped to 3,163 MiB. Because memory usage hovered around 3,216 MiB, the collector remained permanently in `pressured = true`. In that state, `ticks_to_destroy` was set to 80 ticks (~0.2s / 3 frames), causing any texture temporarily out of camera view to be evicted. In 5 seconds, over 3,600 images were evicted and downloaded synchronously via `DownloadImageMemory(image_id, true)` (calling `scheduler.Finish()` multiple times per frame). On subsequent frames, these same textures had to be re-allocated, staged, and re-uploaded over PCIe, creating recurring 150–300 ms pipeline freezes.
- **Change**:
  1. Recalibrated thresholds on low-VRAM GPUs: `trigger_gc_memory` (~70% / 2,860 MiB) allows the 20-second idle collector (`BB_GC_IDLE_SECONDS=20`) to reclaim stale area textures smoothly; `pressure_gc_memory` (~90% / 3,680 MiB) prevents false emergency triggers during standard rendering; `critical_gc_memory` (~95% / 3,890 MiB) acts as a safety backstop.
  2. Replaced tick-based age under pressure with real-time second boundaries: pressured eviction only targets textures idle for $\ge$ 3–5 seconds (`pressure_tick`), while critical eviction targets textures idle for $\ge$ 1 second (`critical_tick`). Active view frustum textures are never evicted.
  3. Rate-limited synchronous GPU downloads (`scheduler.Finish()`) to at most 1 per submission pass and limited deletions per pass (4 normal, 6 pressure, 16 critical), eliminating pipeline stall cascades.
