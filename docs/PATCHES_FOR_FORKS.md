# Patches other forks can reuse

For other Bloodborne PC port forks (shadPS4-based, the Linux bbport, other Windows ports) that want
our co-op and game-improvement work. Everything targets Bloodborne 1.09 (`CUSA03173`, `eboot.bin`).
Statements come from the repo's own notes; items marked **[unverified]** are not confirmed by them.
Back to the [README](../README.md).

Short version: the `Party: ...` XML patches and the plain game-improvement XML patches are the easy
copy. Most co-op behaviour is C++ runtime hooks that need our party layer (`gpu/shim/party`), but the
hook sites and the reverse-engineering notes are reusable on their own.

## 1. How the patches are organised

| Kind | Where | Notes |
|---|---|---|
| Game byte patches | `patches/Bloodborne.xml` (shadPS4 patch format) | `Line Type="bytes" Address=... Value=... Original=...`. `scripts/patches.py` compiles the enabled ones into `out/patches.bin`. It checks every `Original` against the 1.09 ELF and leaves a whole patch out on a mismatch. Only literal writes (bytes, floats, utf8/16); pattern ("mask") patches are rejected. |
| Co-op runtime hooks (C++) | `gpu/shim/party/*.cpp` | `party_b2.cpp` (seamless travel, death, spectate), `party_travel.cpp`, `party_guestsvc.cpp`, `party_progress.cpp`, `party_balance.cpp`, `seamless_rules.cpp`, `coop_hooks.cpp` (hook helpers) and others. |
| Stability fixes (C++) | `gpu/shim/bbport_stability.cpp`, `gpu/shim/bbport_ribbons.cpp` | Each site is compared byte for byte with the 1.09 code first; a site that differs is left alone with a log line. |
| Emulator-core changes | `gpu/shadps4/video_core/amdgpu/liverpool.cpp`, `src/runtime_savedata.c`, `scripts/boot_image.py` | Ordinary source changes, not game patches. |

### Address convention (from `docs/PARTY_COOP_PLAN.md`, `docs/party/seamless_rules.md`)

| Name | Value |
|---|---|
| Our offset | guest address minus image base 0x800000000 = raw ELF VA (eboot vaddr 0 is at file offset 0x4000) |
| `patches/Bloodborne.xml` / shadPS4 patch `Address` | our offset + 0x400000 |
| bbhost source address | our offset + 0x400000 (bbhost `Rva{}` constants in its headers are already our offsets) |
| shadp2p (Wozzardman) address | our offset |

The `Note` text in the XML and all docs quote **our offsets**. Example: "Bells anywhere" notes
`0x157f8c8`; its XML line is `Address="0x0197f8c8"`. If your fork's loader already applies shadPS4
patch XML, copy the `<Line>` elements as they are.

> **Log prefix (0.7.5):** the party code (`gpu/shim/party/*`, `gpu/shim/net/*`) starts every log line with `[coop] `
> (`Party ...:`, `Net: `, `Coop hooks:` ...), added in `coop::asyncio::Print` (`party_async_io.cpp`, constant
> `kLogPrefix`) and at the direct `printf` sites. If your fork carries our party code on another engine, keep it: a
> reader of your logs then sees which lines are ours. Tools (`tools/mp/*.py`) accept lines with and without it.

## 2. Co-op patches (game side)

"Copy as is": **yes** = self-contained byte patch that needs no party layer to apply (it only matters
once something creates a session); **layer** = depends on our party layer (`BB_PARTY` mode, our
transport, flow and event state); **hook-only** = our code, but the site list and notes are reusable.

All `Party:` XML entries ship `isEnabled="false"`; `patches.py` enables them when `BB_PARTY` is set.
Entries marked `Party="cosmetic"` are local pacing or banners and are left out of the party version check.

### 2.1 The `Party: ...` XML patches

The bell, boss-clear and map-reload ideas come from Wozzardman/shadp2p (GPL-2.0-or-later,
`documents/bloodborne-seamless-re.md`), validated there in live 2-client sessions on 1.09.

| XML name | What players get | Main addresses (our offsets) | Copy as is | Notes |
|---|---|---|---|---|
| Party: Bells anywhere | Beckoning, Small Resonant and Sinister bells work in every area, also negative SOS areas (defeated-boss regions, Hunter's Dream) | 0x157F8C8, 0x157F960, 0x157F6D1, 0x157F6D3, 0x15068BB, 0x15068FC, 0x191A8C3, 0x18700D3 | yes | Per-site bytes in `seamless_rules.md`. The area validator 0x131D7B0 itself is not patched. |
| Party: Bells after boss defeated | Summons still build after the area boss is dead and across maps | 0x18749E8, 0x18749F0, 0x14B714A, 0x1874B79 | yes | `seamless_rules.md` |
| Party: Keep session on map reload | The room is not stopped during a summoned map reload | 0x19471B1 (call to Stop 0x1ED07A0 becomes call to the matching-exists predicate 0x1ED00A0) | yes | Same site as hook K1. A second owner (OnMatchingCheck, return 0x13809BC) is deliberately left alone: it is also the real-disconnect path. |
| Party: SOS sign timeout 30s | An unanswered sign expires after 30 s instead of 180 s, so it is renewed sooner | 0x4927A5C (rodata float 180.0 to 30.0) | yes | Read only at 0x18729ED. The 55.0 float next to it is unchanged. |
| Party: Bells without Insight | A bell's Insight cost neither greys it out nor is spent | 0x157FA5C, 0x18C92BB | patch yes; param write is ours | Our layer also writes the Beckoning Bell row's cost to 0 at run time; not in the XML. |
| Party: Fast multiplayer messages | Multiplayer banners hold 1.5 s instead of 5.0 s | 0x1BFFACE, rodata 0x4928A98 (read only at 0x1BFFA7D) | yes | cosmetic. `join_timing.md` |
| Party: Hide bell searching messages | No "Searching for the chime of a ... Bell" banners | 0x1389CFC, 0x17C3CEA | layer | cosmetic. Only makes sense when our director rings the bells itself. |
| Party: Fast join | Shorter fixed summon and join pauses | Director timers 0x5574AD0, AD8, AE0, AE8 (static init 0x196D3A0); 0x14B80C4, 0x14B4975, 0x14BB067; NexusRevolution first retransmit 3000 to 300 ms at 0xCDB2E8 (defaults 0xCDB250) | yes | cosmetic. None of the pauses waits for data. The retransmit edit is specific to the game's own transport; check it if your fork replaces the transport. |
| Party: Fast rejoin | A crashed and restarted guest can be summoned again after 10 s instead of 30 s | peer pool 0x147B350 (0x147AF20, 0x1480D10) | yes | cosmetic. `join_timing.md` |
| Party: Fast bell sign | First sign goes out 2.0 s after a bell instead of 5.0 s | 0x18A9264, 0x18A9324, ten inlined starts at 0x189A095 + n*0x110 | yes | cosmetic. Own switch `BB_PARTY_FAST_SIGN=0`. |
| Party: Skip Online/Offline Choice (Online) | Boots to the main menu online without the dialog | dialog builder 0x1B39030 returns the PLAY ONLINE item's result (functor 0x1B49EB0) | yes | The plain `Skip Online/Offline Choice` entry (author bbport) does the same for PLAY OFFLINE. |

Version check: `patches.py` lists the gameplay (non-cosmetic) party patches and the party HELLO packet
compares that list between host and guest. A fork that mixes patch sets needs the same safeguard.

### 2.2 B2 seamless hooks K1-K11 (`docs/party/b2_seamless.md` section 4, `gpu/shim/party/party_b2.{h,cpp}`)

Make the session and remote players survive map loads. Call-site hooks and state rewinds, so not
copyable as bytes; the sites and the reasoning are what is reusable. Key finding: remote `PlayerIns`
can be re-created from data that survives a load (rewind task `+0x54` to 4, `+0xED |= 1`), so no new
join is needed. Dependency: **layer** (needs our party flow).

| Id | Site | What it does |
|---|---|---|
| K1 | 0x19471B1 | No session leave at stage unload while role 3 or 6 (or the XML retarget above) |
| K2 | 0x1947207 | With a session keep human member tasks, destroy NPC tasks as the reset does |
| K3 | 0x193C657 | No leave on a stage-record change with a session |
| K4 | 0x193C689 | Skip the presentation-member clear with a session |
| K5 | 0x13809B7 | No leave in OnMatchingCheck while a re-join runs |
| K5b | 0x1380A65 | No leave in OnMatchingError for 10 s after a guest changed its own player number |
| K6 | 0x178524F | Skip vanilla SummonReloadStart while a guest re-join runs |
| K7 | 0x13CDEAE | `75 6c` to `eb 6c`, written and restored around the guest's own warp |
| K8 | 0x18C4D7D | Host skips the "return" request to every human task when the host dies |
| K9 | 0x1381A93 | Veto of HostDead_1's send-home for a B2 guest |
| K10 | 0x13858F5, 0x1385A95 | No leave at boss clear (BlockClear2_1 / _3) for roles 3 and 6: the party stays after a boss kill |
| K11 | 0x17851CE | Call site of the packet 0x10 apply (target 0x13BECA0); on a B2 load it fills the host-world mirror (section 6.4 of the doc; not in the section 4 table) |

Also in that doc: S1 (session type restored after a load), E1 (one continuous loading screen), D1-D4
(network-flow state `+0x84` rewind, remote task re-create, host re-sends the join snapshot).

### 2.3 Other co-op fixes and features

| Feature | What players get | Where (our offsets) | Copy as is | Doc |
|---|---|---|---|---|
| Lamps for guests | World lamps and headstones work with guests present; whoever rests moves the party | Byte `+0x49` of the lamp object: `0x12F831C` `85 c9 0f 9f c1 08 c1` to `31 c9 90 90 90 90 90`. Talk-script answers: value filter at 0x203C564 (IsMultiplayerInProgress answers 0 for headstone and lamp scripts), IsClientPlayer filter for lamps; EMEVD rule `7200:1003:6@0` | byte patch: yes in principle (runtime-applied here, not an XML entry); answers: hook-only | `guest_services.md` section 3, `travel.md` |
| Open-world fog walls | Block-crossing allowed; lifts the vanilla "phantom area limit" (18 walls) | Common event 7600: skip only instruction 6 (`2005[3]`) and 7 (`2006[2]`), rule `BB_PARTY_EMEVD_SKIP="7600:2005:3@6,7600:2006:2@7"` | layer | `phantom_limits.md` 2.2. Enable only together with travel handling of block changes, otherwise crossing ends the session. |
| Guest services (Doll, shops, workshop) | A summoned guest can use the Doll, shops and workshop in the host's Dream | `IsClientPlayer` talk case 19: the `movzx` at 0x203BA78 is replaced by a jump to a filter (continues at 0x203EECF); answers 0 only for listed Dream talk scripts, only for a client in `m21_xx` | hook-only | `guest_services.md` section 2 (`party_guestsvc.cpp`) |
| Host-world flag mirror | Guest sees the host's lit lamps, picked-up items and opened gates | `party_progress`: after the packet 0x10 apply (0x13BECA0) 638 flag groups of the host's pool go into the guest's overlay (`+0x88`); boss-fight zones and session flags 2000-2099 excluded | layer | `event_flags.md` section 12 |
| Guest draw-bit fix | Re-created remote players are visible (was "health bar, no model") | Remote chr `+0x1E0` bit 20 (0x00100000) must be set; a 0.25 s tick re-sets it. InGameStart (0x193A3E0) sets it at 0x193A93C and calls chr vfunc `+0x610(1)`; the fix re-does those steps for a re-created chr | hook-only (small) | `split_travel.md` section 7 |
| Stale map-hit guard | Fixes a crash when the other player stands in a map block this game has streamed out | Entry hook on 0x18CB0E0 (chr physics pre-step): clears chr `+0x288` / `+0x350` of a remote player whose hit is no longer in the registry (`*0x553E850`). Faults were at 0x18CB16A / 0x18CB17D | hook-only | `travel.md` (stale map hit entry) |
| HP field | Where the game keeps HP | HP module (chr `+0x3B0` then `+0x20`): `+0xF8` HP, `+0xFC` max HP; `+0x128` is not HP | note | `b2_seamless.md` |
| Boss-clear keep | Party stays together after a boss dies | K10 above | hook-only | `b2_seamless.md` 6.7 |
| Death and revive | Host or guest death respawns in the host world; boss-fight deaths handled | `party_b2.cpp` (K8, K9, `DeadAfterClearTick`, host-death revive) | layer | `b2_seamless.md` 6.8-6.13 |
| Spectate | A dead player in a boss-fight limbo follows a living member; peers hide the dead copy | `party_b2.cpp`, `BB_PARTY_SPECTATE` (default on) | layer | `b2_seamless.md` (2026-10-12 section), `ERSC_PARITY.md` C5. Guest-side spectating not run live (per ERSC_PARITY). |
| Death debuff ("rot") | Optional per-death stacks: max HP -10 % and max stamina -10 % ship; three more kinds behind `BB_PARTY_ROT_ALL_KINDS=1` | Prologues replaced at 0x17E46E0 (max HP), 0x17E4A20 (stamina), 0x17E4C50, 0x17E5590, 0x17E8010 | hook-only | `balance.md` section 3 |
| Enemy and boss scaling | Scaling by cooperator count, from the host's settings | SpEffectParam rows 7500 / 7501 / 7502 (bosses with 1 / 2 / 3 cooperators) and 7503 (normal enemies; unused in 1.09) rewritten every second; EMEVD `1003[109]` count call 0x17BF187 and native MultiDoping 0x138BC70 | layer | `balance.md` section 2, `four_players.md` |

## 3. General game-improvement patches (not co-op)

### 3.1 XML patches in `patches/Bloodborne.xml`

Plain 1.09 byte patches; copy the `<Metadata>` blocks as they are and keep the `Author` credit. Most of
the file is Kyo's work (44 entries by Kyo alone); the author list as written in the XML:

| Entries | Author in the XML |
|---|---|
| Performance Patch, Disable Motion Blur / Dynamic Light Shadows / AA / DoF / SSAO, Screen Space Reflections, 50% Text scale, Model LOD patches (with auser1337), Increased camera distance, resolution and light grid patches, DS1-like and lower object/corpse physics, 30 / 60 / 60 (no deltatime) / 90 FPS++, rally patches, Player No Dead / Stealth / Silent, Sensitive Analog Input, light grid patches | Kyo (every entry not listed below has `Author="Kyo"`) |
| Uncap FPS++ | Lance McDonald (manfightdragon), Kyo |
| Unlock Game Region | Lance McDonald (manfightdragon), Kyo |
| Restore Debug Camera | Lance McDonald (manfightdragon) |
| Skip Intro, Disable Chromatic Aberration | illusion |
| Increased Graphics Heap Sizes | auser1337 |
| Restore Debug Menu | Whitehawkx, auser1337 |
| Disable Camera Auto Rotation via Movement | Imedved, Kyo |
| Sprint Fix (High FPS) | yogesh1239 |
| Intel 12th Gen+ SFX workaround | emoose |
| FMOD Crash Fix | Dasaav |
| Disable HTTP Requests | bloo |
| Bookmark and Capture outputs | Foxy Hooligans |
| Enemy Control | stagvant |
| Skip Online/Offline Choice | bbport |

`docs/CREDITS.md` credits "Kyo, Lance McDonald, illusion, emoose, auser1337, and contributors for the
60 FPS, camera, and engine patches". Several patches carry `READ NOTE` in the name: read the `Note`
attribute before enabling.

### 3.2 bbhost stability fixes (`gpu/shim/bbport_stability.cpp`, `bbport_ribbons.cpp`)

From droogie and the bbhost team (GPL-3.0-or-later). The file header says bbhost addresses are
image + 0x400000 and the file uses guest offsets (image + 0), e.g. bbhost 0x2aaa860 is +0x26aa860.

| Fix | Switch (default on) | Site (guest offset) |
|---|---|---|
| Heap size table growth (SprjMemory heaps for Scaleform/MENU; GFX above 1080p) | `BB_HEAP_TABLE=0`; GFX_GraphicsPrivate growth `BB_GFX_HEAP_MORE_MIB=N` | heap table, `runtime_memory.c` |
| GX resource-table reclaim wait (GPU catch-up wait) | `BB_GX_RECLAIM_WAIT=0`, bound `BB_GX_RECLAIM_WAIT_MS` (default 100) | GX table block reclaim |
| Frame-pool borrow fix | `BB_FRAME_POOL_FIX=0` | +0x1067F60 |
| Red-zone-free effect ribbon writers | `BB_RIBBON_FIX=0` | +0x28CE7B0, +0x28CEEC0 |
| NULL guards on the frame arena allocator (drop the render command) | `BB_ARENA_GUARD=0`, log `BB_ARENA_PROBE=0` | allocator +0xE45560, checks +0x21D5B35 and +0x2199303 |
| Frame command allocator 256 KiB -> 1 MiB (what the frame arenas draw from) | `BB_FRAME_ARENA_KIB=N` (0: the game's 256) | +0x2195C09, +0x2195C46 |
| Blend-state write mask not cleared | `BB_BLEND_MASK_FIX=0` | +0x2A51640 |

The credits also list a 6 GiB direct memory change from bbhost; its site is not in the stability header.
Exact before and after bytes are in the code; read it before copying. What each fix prevents is not
described in the header **[unverified]** beyond the names above.

### 3.3 Emulator-side items

| Item | Where | Description |
|---|---|---|
| DAS=1 DMA skip | `gpu/shadps4/video_core/amdgpu/liverpool.cpp` (near lines 525 and 2444) | A DMA_DATA packet with DAS=1 (register destination, not a memory copy) is skipped with a warning instead of tripping `ASSERT(das == 0)`. |
| PM4 padding guard | liverpool.cpp (near lines 1486 and 2407) | Trailing all-zero dwords of a DCB/ACB chunk end the chunk instead of hitting UNREACHABLE. From Mrsuss60 196ba93. |
| Boot-image cache | `scripts/boot_image.py`, `docs/STARTUP.md` | One process builds the boot image and reuses it (0.01 s instead of 10-15 s) while `eboot.bin`, `sce_module/*`, `param.sfo`, the shader folder and the scripts are unchanged. `BB_BOOT_CACHE=0` rebuilds. |
| Audio boost and limiter | `BB_AUDIO_BOOST` (default 1.4), `gpu/shim/bbgpu.cpp` | Multiplier on the original gains; the limiter is linear to 0.75 then a tanh knee below full scale (the earlier soft clip fell to 0 at x = 1.0). `docs/MRSUSS60_INTEGRATION.md` |
| Sound hack and whitelist | `src/runtime_savedata.c`, `BB_SOUND_HACK=0` off | Sets the flag at 0x204E of `userdata0010` in the game data save (rainvmaker, from the Diegolix29 shadPS4 fork); without it parts of the game audio (e.g. weapon sounds) never play. Only for title ids CUSA00207, 00208, 00299, 00900, 01363, 03014, 03023, 03173, 03179. |

## 4. Where to find things

| What | Path |
|---|---|
| All XML patches (search `Party:` for co-op) | `patches/Bloodborne.xml` |
| Patch compiler, party patch lists, version check | `scripts/patches.py` |
| Co-op runtime | `gpu/shim/party/*.cpp` |
| Stability fixes | `gpu/shim/bbport_stability.cpp`, `gpu/shim/bbport_ribbons.cpp` |
| Plan and address conventions | `docs/PARTY_COOP_PLAN.md`, `docs/PARTY.md` |
| Reverse-engineering notes | `docs/party/*.md`: `b2_seamless.md`, `seamless_rules.md`, `join_timing.md`, `guest_services.md`, `travel.md`, `split_travel.md`, `event_flags.md`, `phantom_limits.md`, `balance.md`, `four_players.md`, `items.md`, `world_actions.md`, `ERSC_PARITY.md` |
| Environment switches for every hook | `docs/party/DEBUG_SWITCHES.md` |
| Startup timings | `docs/STARTUP.md` |
| Ported from Mrsuss60 | `docs/MRSUSS60_INTEGRATION.md`, `docs/MRSUSS60_COMPARE.md` |

When porting a hook, copy the "1.09 bytes" column too and compare it with your ELF first, as our loader
does; a mismatch means a different game version.

## 5. License and credit

The project is **GPL-3.0-or-later**; parts derived from droogie/bbhost are GPL-3.0-or-later and
shadPS4-derived parts are GPL-2.0-or-later (details in [CREDITS.md](CREDITS.md)). Reuse under compatible
terms and keep the `SPDX-License-Identifier` and "derived from" headers in the source files.

Credit at least: Wozzardman / shadp2p (bell, boss-clear and map-reload patches), droogie and the bbhost
team (stability fixes), Kyo, Lance McDonald (manfightdragon), illusion, emoose, auser1337, yogesh1239,
Imedved, Whitehawkx, Foxy Hooligans, bloo, Dasaav, stagvant (the `Author` names in the XML), Mrsuss60
(Windows port, PM4 padding guard, audio boost origin), rainvmaker and Diegolix29 (sound hack),
deadinside28 (Linux bbport), the shadPS4 team.
