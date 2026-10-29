# Iosefka's clinic: the Nightwatch bug archive

Iosefka (the Net Doctor bot in the Nightwatch Co-op Discord) takes every log players post, with the text,
screenshots and video stills around it, into a private archive on quad.fi. Each case becomes a bug
`NW-xxxxxxx` (7 hex digits, e.g. `NW-1a2b3c4`). The devs read and triage the archive through the
`iosefka` command over SSH. This file is the reference for people and for their Claude Code instances.
The ready-made Claude skill is in `tools/discord/iosefka-skill/SKILL.md`.

Everything the clinic holds is private: logs carry player names, Steam/PSN names and IP addresses.

## Quick queries

```
ssh iosefka new                                                  what is new since my last check (also bugs that got new material)
ssh iosefka triage-queue                                         bugs waiting for a triage pass + auto-linked messages
ssh iosefka list --open --tag component=nw-launcher              all open launcher bugs
ssh iosefka list --open --tag component=coop-mod --tag feature=chalice     open co-op chalice bugs
ssh iosefka files --open --tag component=coop-mod --tag feature=chalice > b.tar    pull them all
ssh iosefka show NW-1a2b3c4                                      one bug with every message and note
ssh iosefka vpn-profile > nightwatch.ovpn                       the Nightwatch VPN client profile (devs only; it is a key: keep it out of git)
```

## One-time setup

1. Make a key on your PC: `ssh-keygen -t ed25519 -f ~/.ssh/iosefka -C vlad` (an empty passphrase makes
   unattended Claude use easier; a passphrase plus ssh-agent works too).
2. Send visy the PUBLIC key (`~/.ssh/iosefka.pub`) only. Never send the private key.
3. visy adds one line to `~visy/.ssh/authorized_keys` on quad.fi:
   ```
   command="/home/visy/netdoctor-venv/bin/python /home/visy/bbport/tools/discord/iosefka_cli.py --as vlad",restrict ssh-ed25519 AAAA... vlad
   ```
   The key can run only this tool: no shell, no forwarding. `--as` is the name used in notes and in
   `new`; visy's own line says `--as visy`.
4. Add to `~/.ssh/config`:
   ```
   Host iosefka
       HostName quad.fi
       User visy
       IdentityFile ~/.ssh/iosefka
       IdentitiesOnly yes
       BatchMode yes
   ```
   Then `ssh iosefka list --open` works. Test with `ssh iosefka new`.

## Commands

Output is JSON; add `--text` after the command name for a short human summary. Times are UTC ISO strings.
Both devs can read everything and use every command except `export`, `confirm-export` and `import`,
which are for visy only (they answer `export is for visy only`, exit 1).

The command string goes through `shlex` on the server (never a shell), so quote text yourself: from a
local shell use double quotes around single quotes, e.g.
`ssh iosefka note NW-1a2b3c4 "'Missing d3d12 dll; see ND-0a1b2c3d'"`. A note containing `;`, backticks
or `$()` is stored as plain text and never run.

### new

`ssh iosefka new`: bugs created and notes added by others since your last `new` (the first call lists
everything). It also marks "seen now". `share` notes addressed to you are included.
`updated_bugs` lists older bugs that got new messages or files since your last `new` (players add details
days later; a fixed or closed bug that gets new material is reopened and carries a note "new report
after <status>"). `triage_pending` is the size of the triage queue. `changelist` lists changelist items
someone else added, edited or moved (e.g. set `done` after a fix was merged), one entry per item with its
current status; a sync or import is folded into `imports` (who, track, version, item count).

```json
{"since": "2026-10-28T08:00:00Z",
 "bugs": [ {"id": "NW-1a2b3c4", "title": "Lamp stays dark", "status": "new", "tags": {}, "...": "..."} ],
 "notes": [ {"bug_id": "NW-1b2c3d4", "at": "2026-10-28T09:10:00Z", "who": "visy", "kind": "share",
             "text": "please look at this"} ]}
```

### list

`ssh iosefka list [--status s]... [--open] [--tag k=v]... [--q text] [--since YYYY-MM-DD] [--assignee who]
[--older-than DAYS] [--idle DAYS] [--sort newest|oldest|activity]`

Filters combine with AND. `--status` and `--tag` can repeat. `--open` means every status except fixed,
closed and duplicate. A tag value matches case-insensitively, and a tag holding a list matches when any
element equals it (`feature=chalice` finds `["chalice","lamps"]`); `--tag area` (no value) means "has this
tag". `--q` searches title, message texts and notes.

**Two times per bug.** `reported` is when the player wrote the earliest message of the bug (the Discord
message time, computed from the bug's messages, so merges and moves keep it right; a bug without messages
falls back to `filed`). `filed` is when Iosefka created the bug record (also in `created`, kept for
compatibility). `--since` and `--older-than DAYS` (reported more than DAYS days ago) use `reported`, as does
`--sort newest|oldest`; `--idle DAYS` keeps bugs with no activity (message, note or status change) for DAYS
days. `--sort` is `newest` (default, by reported), `oldest`, or `activity` (most recently active first).
`list --text` shows the reported date (YYYY-MM-DD) after the status.

```json
[{"id": "NW-1a2b3c4", "title": "Chalice lamp stays dark", "status": "open",
  "tags": {"component": "coop-mod", "feature": ["chalice", "lamps"]}, "assignee": "vlad",
  "reported": "2026-10-20T16:40:00Z", "filed": "2026-10-27T18:02:11Z",
  "created": "2026-10-27T18:02:11Z", "updated": "2026-10-28T09:00:00Z",
  "counts": {"messages": 4, "logs": 2, "images": 1, "stills": 12}, "nd_refs": [], "dup_of": null,
  "last_note": {"who": "vlad", "kind": "note", "at": "2026-10-28T09:00:00Z", "text": "reproduced..."}}]
```

### show

`ssh iosefka show NW-1a2b3c4 [--full | --brief]`: `--full` (default) has every message text;
`--brief` cuts message texts to 200 characters.

```json
{"bug": {"id": "NW-1a2b3c4", "title": "...", "status": "open", "tags": {}, "assignee": null,
         "counts": {"messages": 1, "logs": 1, "images": 0, "stills": 0}, "source": "thread",
         "channel_id": 123, "thread_id": 456, "...": "..."},
 "notes": [{"at": "2026-10-28T09:00:00Z", "who": "vlad", "kind": "note", "text": "...", "to": null}],
 "messages": [{"message_id": 100, "author": "PlayerA", "at": "2026-10-27T18:02:11Z", "state": "deleted",
               "content": "crashes when I join", "attachments": [{"sha256": "ab12...", "filename":
               "nightwatch-20261024_204859.log", "kind": "log", "size": 5120}]}],
 "files": [{"sha256": "ab12...", "filename": "nightwatch-20261024_204859.log", "kind": "log", "size": 5120}]}
```

Message authors are player names: private.

### files

`ssh iosefka files NW-1a2b3c4 [NW-... ] [--no-media] > x.tar`, or with the `list` filters:
`ssh iosefka files --open --tag component=nw-launcher > launcher.tar`. One tar streamed to stdout (nothing
is written on the server), one folder per bug:

```
NW-1a2b3c4/bug.json          the full `show` output
NW-1a2b3c4/logs/<sha8>_<name>
NW-1a2b3c4/images/<sha8>_<name>
NW-1a2b3c4/stills/<sha8>_still_00.webp    (video frames, one every 4 s)
```

`--no-media` leaves out images and stills. Ids or at least one filter are required.

### note

`ssh iosefka note NW-1a2b3c4 "'text'"`: add a timeline note by you.

### status

`ssh iosefka status NW-1a2b3c4 <status> [--dup NW-1b2c3d4] [--tell "'player-facing text'"] [--in TRACK:VERSION]`

```json
{"ok": true, "bug": "NW-1a2b3c4", "status": "fixed", "tell_queued": 7, "tell_text": "Fixed in 0.2.4."}
```

Statuses:

| status | meaning |
|---|---|
| new | filed by Iosefka, nobody has looked |
| open | looked at, confirmed or being worked |
| analysing | logs being analysed |
| waiting-logs | we need more logs (other peers of the session) or an answer from the player |
| fixed | fixed in a build; say which version in a note |
| closed | not a bug, cannot reproduce, or answered |
| duplicate | same as another bug; use `--dup NW-y` (or `merge`) |

`--tell`: Iosefka posts the text in the bug's thread or channel within a minute (public!). Public IPv4
addresses in it are masked, but write it with no names, IPs or log lines anyway. The text is also kept
as a `triage` note.

**No double pings.** Tells and asks to the same person with the same text (compared ignoring case and spacing)
never go out twice. When several bugs of one reporter get the same text (a bug with no `reporter_id` counts
as its first log poster), the bot posts ONE message in the newest bug's thread, mentions the person once and
says "About NW-a, NW-b and NW-c: ...". The other rows are marked sent with `merged_into` = the row that carried
them, and each bug gets a note ("told together with ..."). A text the person already got within the last 24 h
(`NW_TELL_WINDOW_HOURS`, also across separate CLI calls) is not sent again: the row is marked delivered by
merge and the bug gets a "not re-sent" note. `status --tell` answers `tell_merge_with` (bugs with a pending twin)
and `tell_warning` (already told recently); the `ask` dry run shows `merge_with` / `warning` per message. Every
bug still keeps its own `triage` / `ask` history note. Asks coalesce only with an equal issue and need.

`--in TRACK:VERSION` goes only with `fixed`: it also records the fix in the changelist (see Changelists).
If the bug already has a live (not dropped) item in that track, the item moves forward to done; an item
whose version you did not set by hand also moves to VERSION (created if missing), while one with a
hand-set version keeps it. Otherwise a new `fixed` item with the bug's title is added to that version:
done, unless the version is still `planned` (then the item is `planned` and `cl notes` leaves it out), so
make sure the version is `current` or `released` first. For NW, run the sync first: it sets the current
version from the VERSION file, whereas a version status you set by hand locks it, and the sync then no
longer moves it. Fixing the same bug on both tracks works: run the command once per track. Example:
`status NW-1a2b3c4 fixed --in nw:0.2.6`.

### ask

`ssh iosefka ask NW-1a2b3c4 --need "'host and guest logs of a Queen attempt'" [--issue "'which part'"]
[--for CL-1a2b3c] [--send] [--force]`: Iosefka asks the ORIGINAL reporter(s) of the bug for logs or details.
**Default is a dry run**: it prints the exact message(s) and where each would go (thread / channel / DM),
queues and records nothing. `--send` queues them (the bot posts within a minute, like `--tell`).

- Reporters: the bug's reporter plus the reporters of bugs merged into it, one message per person in that
  person's own thread (never a duplicate bug id: ask about the kept bug). Skipped (listed under `skipped`):
  local bugs, merged-in bugs a dev filed, a person already listed; a staff reporter is skipped at send time.
- Text: names the bug id and the issue (`--issue`, else the item's text for `--for CL-x`, else the bug
  title), quotes `--need` (max 900 chars), says to answer in the thread and attach the files, and where the
  game's logs are. IPs are masked, `@everyone` defused, at most 2000 chars; only the reporter is mentioned.
- With `--send`: a note of kind `ask` per person (who, issue, need, `to` = the person) is added to the bug
  history (`show` lists it), the status becomes `waiting-logs`, and the 14-day nudge / 30-day close count
  from the ask. At most one ask per bug per 3 days unless `--force` (a dry run shows the block).
- Replies: the person's next messages in the place she asked (14 days) join the bug even when short (under
  4 words) or past the 5-message follow-up cap, and with files of any session (rule `ask`).

`ssh iosefka cl ask CL-1a2b3c --need "'...'" [--issue ...] [--send] [--force]`: the same for every bug linked
to the item, one message per bug thread, each naming its own bug and the item's text as the issue.

### tag

`ssh iosefka tag NW-1a2b3c4 component=coop-mod feature=chalice,lamps coop_version=0.7.5`: a comma makes a list;
`key=` with no value removes the tag. Keys: lowercase letters, digits, `-`, `_`.

Tag vocabulary (normalised 2026-10-28; use these values so filters work; a list is fine where noted):

| key | values |
|---|---|
| component | `coop-mod`, `nw-port`, `nw-launcher`, `network`, `game-data`, `discord` (server/community), `external` (third-party tools) |
| area | region, list if several: `central-yharnam`, `cathedral-ward`, `upper-cathedral-ward`, `old-yharnam`, `hemwick`, `forbidden-woods`, `byrgenwerth`, `yahargul`, `cainhurst`, `nightmare-frontier`, `mensis`, `hunters-nightmare`, `research-hall`, `fishing-hamlet`, `hunters-dream`, `chalice` |
| place | free text detail: boss, NPC or spot (`Ludwig`, `Bath Messenger`, `Gascoigne lift`) |
| feature | list: `joins`, `bosses`, `performance`, `graphics`, `travel`, `items`, `crash`, `install`, `npc`, `lamps`, `chalice`, `saves`, `hud`, `enemies`, `summons`, `update`, `input`, `audio`, `echoes`, `pvp`, `lifts`, `party`, `launcher`, `chat`, `other` |
| nw_version | Nightwatch build the player ran, normalised: `0.2.4 beta 1`, `0.2.5` (list if several) |
| coop_version | co-op mod version: `0.7.5` (list if several) |
| version_note | the original free-text version (ranges, "older than", dates) when it said more |
| role | `host`, `guest`, `solo` (list if several) |
| severity | `major`, `minor` |
| plan | changelist/plan item name it belongs to (`enemyai`, `chalicefollow`, ...) |
| triage | `pending` (Iosefka filed it, waits for a Claude pass), `done` |
| local_cases, nd, source | links to local player_logs cases, ND analysis refs, `source=local` for imported cases |

Changelist items use the same `component`, `area` and `feature` values.
Example queries: `list --open --tag area=hunters-nightmare`, `list --tag nw_version="'0.2.4 beta 1'"`,
`list --open --tag feature=bosses --tag role=guest`.

### triage-queue, title, move

- `ssh iosefka triage-queue`: bugs with tag `triage=pending` (oldest first) and the `auto-link` notes added
  since your last queue check (messages Iosefka attached by a weaker rule: same session, 24 h follow-up).
  Clear a bug after the pass with `tag NW-x triage=done`. Tag `unsure` lists what she could not settle:
  network-or-bug, which-bug, no-log, unreadable-file, no-text.
- `ssh iosefka title NW-x "'new title'"`: set the title (IPs masked).
- `ssh iosefka move <message_id> NW-x`: re-point one message and its files to another bug (for a wrong
  auto-link); both bugs get a note.

### assign, share, merge

- `ssh iosefka assign NW-1a2b3c4 vlad` (`-` unassigns).
- `ssh iosefka share NW-1a2b3c4 visy "'look at the NAT part'"`: appears in visy's next `new`.
- `ssh iosefka merge NW-1a2b3c4 NW-1b2c3d4`: the second becomes a `duplicate` of the first; its messages,
  notes and missing tags move to the first.

### nd

`ssh iosefka nd ND-0a1b2c3d`: the stored Net Doctor analysis (the `ref ND-...` line under Iosefka's
posts): `{"ref", "channel_id", "guild_id", "bot_message_id", "files", "text", "created"}`.

### export, confirm-export, import (visy only)

`export` streams the whole archive as a tar (`manifest.json`, `archive.json`, `files/<sha256>`);
`confirm-export` takes the PC's re-hash JSON on stdin; `import` takes a tar of local cases (a folder per
case with `case.json` `{title, tags, notes, files, status?, reported?}` and the files) and creates `local` bugs.
`reported` (ISO date, ISO datetime with or without Z/offset, or epoch seconds) is the time of the case's
message, so the bug's `reported` is when it really happened; without it, now.

## Changelists

Besides bugs the clinic keeps versioned changelists: what is planned for, or went into, each release.
Use them to plan a release, see whether a bug is fixed on the other side, and produce release notes.

Concepts:

- Track: `coop` (the Bloodborne PC port with co-op, visy's side) or `nw` (Nightwatch, vlad's side).
  Both devs can read and write both tracks.
- Version: a release inside a track, e.g. `0.7.7` or `0.2.5 beta 3`. Version status is `planned`,
  `current` or `released`. `--current` and `--next` on `cl list` pick the track's current / next version.
- Item `CL-xxxxxx`: one change, with a section (`added`, `changed`, `fixed`), a `text`, an optional private
  `dev` text, a status (`planned`, `in-progress`, `done`, `dropped`), tags, and links to bugs (`NW-...`),
  player_logs case folders and commit hashes.
- Two times per item. `dated` is when the change itself happened: the NW commit date, the coop release's
  published date, the linked bug's `reported` for items made by `status ... fixed --in`, or whatever you set
  with `cl add --dated` / `cl edit --dated` (ISO date, ISO datetime or epoch; a hand-set `dated` is locked).
  `added` (also `created`) is when the item reached Iosefka. Imports carry `dated` too (JSONL field
  `dated`); re-importing the same lines fills `dated` on existing unlocked items, which is how old rows get
  backfilled. `cl list` shows both (`--text` prints dated as YYYY-MM-DD or `-`), sorts with
  `--sort added|dated` (default added, newest first; undated last for `dated`) and `--dated-since
  YYYY-MM-DD` filters on `dated`. `--since` / `--older-than` / `--idle` on items stay on `added`.
- Locks: any field you set by hand (`cl edit`, `cl move`, `cl status`, ...) is locked. Later imports and
  syncs never overwrite a locked field, so tidy up freely (a version status you set by hand is locked too).

Privacy rule: item `text` is PUBLIC (it ends up in release notes). Write it with no player names, IPs,
Discord ids or player_logs paths. Put anything private in `--dev`, which never appears in `cl notes`.

All commands are `ssh iosefka cl <command>`. Quoting is the same as for `note` and `--tell`: the string
is split by your local shell and again by the server, so any argument with spaces goes in single quotes
inside double quotes, e.g. `"'multi word text'"`. Output is JSON (except `cl notes`, which prints Markdown); `--text` gives a short summary, except on
`edit`, which takes its own `--text VALUE`.

```
ssh iosefka cl versions --track coop                               versions with status and item counts (--track optional)
ssh iosefka cl version coop 0.7.8 --status planned                 create or update a version (also --released YYYY-MM-DD, --note "'..'")
ssh iosefka cl list --track nw --current                           items of the current NW version
ssh iosefka cl list --track coop --version 0.7.7                   items of one version
ssh iosefka cl list --track coop --next --section fixed            more filters: --status (repeat), --tag k=v, --bug NW-x, --q, --since, --older-than DAYS, --idle DAYS
ssh iosefka cl show CL-1a2b3c                                      one item with notes and its linked bugs
ssh iosefka cl add coop 0.7.8 fixed "'Chalice lamp state now syncs'" --bug NW-1a2b3c4 --dev "'private detail'"
ssh iosefka cl edit CL-1a2b3c --text "'Clearer wording'" --section changed
ssh iosefka cl status CL-1a2b3c done                               planned, in-progress, done, dropped
ssh iosefka cl move CL-1a2b3c coop 0.7.8                           move an item to another version
ssh iosefka cl link CL-1a2b3c --bug NW-1b2c3d4 --commit abc1234    add links (--case folder; --unlink removes them)
ssh iosefka cl merge CL-1a2b3c CL-2b3c4d                           second item folds into the first
ssh iosefka cl related NW-1a2b3c4                                  items and bugs that look related (also takes a CL id)
ssh iosefka cl notes nw 0.2.6                                      release notes as Markdown
ssh iosefka cl release coop 0.7.7 [--next 0.7.8] [--dry-run]        release a version (see below)
```

Notes on single commands:

- `cl add` wants track, version, section and text. `--status` defaults to `planned`. Repeat `--bug`,
  `--case`, `--commit`, `--tag k=v` as needed. Text is capped at 300 characters and masked for IPs.
- `cl list` without `--version/--current/--next` lists every version. `--bug NW-x` finds items linked to a bug.
- `cl related` only suggests candidates; nothing is linked until you run `cl link` or `cl merge`.
- `cl notes` gives public-safe release notes: only `done` items with their public text, grouped
  Added / Changed / Fixed. `dev` text and unfinished or dropped items are left out. Check the result holds
  no names or IPs before you paste it anywhere.
- `status NW-x fixed --in nw:0.2.6` marks a bug fixed and links it into the changelist in one step.
  For a fix that already shipped in a released version, the release-note item usually exists without the
  bug link: find it (`cl list --track coop --version 0.7.6 --q freeze`, or `cl related NW-x`), `cl link
  CL-x --bug NW-x`, then plain `status NW-x fixed`. `fixed --in` on it would add a second item (fold it back
  with `cl merge <keep> <dup>`).

### Releasing a version

`cl release <track> <version> [--next <version>] [--dry-run]` (both devs): the version becomes `released`
with released = now; its unfinished items (planned / in-progress) move to `--next` (created as planned when
missing; default: the track's next planned version, an error when there is none), and `--next` becomes
`current`. Every step leaves change notes (version notes, one `move` note per item). The JSON answer holds
`released`, `next`, `moved` (item ids) and `notes` (the rendered release notes markdown, same as `cl notes`).
`--dry-run` shows the same without changing anything.

Order for a release ("release 0.7.7"): 1. run `cl release coop 0.7.7` (preview with `--dry-run`),
2. post the `notes` markdown in #releases, 3. Iosefka reads the post and asks the reporters of the fixed
bugs to retest (next section). The NW track: run the sync first, so its VERSION file and items are current.

### Retest asks and waiting-logs nudges (the bot)

- Retest asks: a new message in the server's #releases channel (live; the hourly catch-up also handles posts
  of the last 7 days) is read for versions (`0.7.7`, `v0.7.7`, `0.2.6`, `0.2.5 beta 4`, `0.2.5-beta.4`). A
  version is matched to the track whose changelist has it (when both have it, the words Nightwatch / NW or
  co-op / port decide). Every bug with status `fixed` that is linked to a `done` item of that version gets ONE
  short ask in its thread (or channel, replying to her replacement note; a DM bug gets a DM): the fix is in
  <version>, please try and say if it still happens. Only the reporter is mentioned. A note of kind
  `retest-asked` ("<version>: asked to retest") makes it once per bug and version. Skipped: `source=local`
  bugs, bugs with no reporter, places that are gone or skipped (staff* / dev* such as development and "Dev Test" / moderator-only). One
  post every ~5 s. The reporter's reply reopens the bug through the usual follow-up rules.
- waiting-logs: in the hourly sweep a `waiting-logs` bug with no activity (message, status change, note by
  anyone but her own asks) for 14 days gets ONE nudge (note kind `nudge`); at 30 days it becomes `closed` with
  the note "closed: no logs for 30 days; reopens when logs arrive". New material reopens a closed bug (the
  existing rule). Local bugs are left alone.
- Preview or run by hand on quad.fi (needs `--yes` to post, NETDOCTOR_TOKEN for that):
  `python tools/discord/iosefka_cull.py retest-asks --version 0.7.7 --dry-run` (add `--track coop|nw`),
  `python tools/discord/iosefka_cull.py waiting-logs --dry-run`.

### Nightwatch sync (vlad's PC)

The NW track is filled from the Nightwatch git history by a script that runs on the dev's PC, not on the
server (it needs the git clone):

```
python tools/discord/cl_sync_nw.py <path to NightwatchCoop clone> --host iosefka --dry-run     show what it would send
python tools/discord/cl_sync_nw.py <path to NightwatchCoop clone> --host iosefka               send it
```

`--host` is the ssh target the script pipes `cl import` to. The default is `visy@quad.fi` (the forced-command
account); vlad's key is installed in that account too, but his key is picked by his ssh alias, so use
`--host iosefka` (the Host alias from the setup section).

- Versions come from commit subject prefixes such as `0.2.5 beta 3: ...` (`;` or `(` after the version
  also works). A commit without a prefix joins the next prefixed version after it.
- The current NW version is the one in the clone's `VERSION` file.
- Each commit becomes one item, keyed by its hash, so re-running is safe and creates no duplicates.
- Items a dev edited (text, section, status, version, tags) are locked and never overwritten by a later
  sync. The import result lists what a lock blocked as `"locked": [{"line": k, "fields": [...]}]` (version
  lines too); look at it, e.g. a hand-set version status keeps NW from moving to a new current version.
- Sections are guessed from keywords in the subject (fix, fixed, fixes, crash, crashes, broken -> `fixed`; add, added, adds, new -> `added`; else
  `changed`), so tidy them with `cl edit`. Commit subjects become public item text: keep them free of
  names and IPs.

On quad.fi the same sync runs hourly from cron straight into the database (no ssh): `cl_sync_nw.py <clone>
--local ~/.config/netdoctor/netdoctor.db` imports as `nw-sync`. `tools/discord/nw_sync_cron.sh` clones
https://github.com/vladtepes22/NightwatchCoop to `~/nightwatch-src` when missing (a full clone, about 45 MB:
the sync needs the history), runs `git pull --ff-only`, syncs, and appends one line per run
(`applied=N skipped=N locked=N`) to `~/.config/netdoctor/nw_sync.log`. Crontab line (visy):

```
17 * * * * sh $HOME/bbport/tools/discord/nw_sync_cron.sh
```

The coop track is seeded once from the GitHub releases with `python tools/discord/cl_import_coop.py
--releases` (visy's job).

How vlad keeps NW in sync: after pushing commits, tell Claude "sync the NW changelist"; it runs
`cl_sync_nw.py` against the clone (a `--dry-run` first if asked for a look), then reports the new items.

### What you'd say to Claude, and what it runs

| you say | Claude runs |
|---|---|
| show the current NW changelist | `ssh iosefka cl list --track nw --current` |
| what's planned for coop 0.7.7 | `ssh iosefka cl list --track coop --version 0.7.7` |
| add the local bug items to the next coop version | `ssh iosefka cl versions --track coop` to find the next version, then per item `ssh iosefka cl add coop <version> fixed "'<public text>'" --case "'<date>/<folder>'"` |
| was NW-1a2b3c4 fixed on the NW side | `ssh iosefka cl related NW-1a2b3c4` and `ssh iosefka cl list --track nw --bug NW-1a2b3c4` |
| mark NW-1a2b3c4 fixed in NW 0.2.6 | `ssh iosefka status NW-1a2b3c4 fixed --in nw:0.2.6` |
| make the 0.2.6 release notes | `ssh iosefka cl notes nw 0.2.6` |
| move this item to 0.7.8 | `ssh iosefka cl move CL-1a2b3c coop 0.7.8` |
| drop this item | `ssh iosefka cl status CL-1a2b3c dropped` |
| open bugs older than two weeks | `ssh iosefka list --open --older-than 14` |
| bugs nobody touched for a week, oldest first | `ssh iosefka list --open --idle 7 --sort oldest` |
| all open chalice bugs | `ssh iosefka list --open --tag feature=chalice` |
| sync the NW changelist | on the PC: `python tools/discord/cl_sync_nw.py <clone> --host iosefka` |

## Workflows

- Morning check: `ssh iosefka new`; open the interesting ones with `show`.
- Triage pass (Claude): `triage-queue`; per bug `show NW-x` + `files NW-x`, read the logs (party logs with
  `scripts/net_doctor.py`), set title (`title`), status and tags (component, area, feature, version, role,
  plan, severity), settle each `unsure` reason (wrong auto-link: `move`), look for duplicates in open bugs
  (`list --open --q ...`, then `merge`), check CHANGES.md and the Nightwatch repo for an already fixed
  cause, write a triage `note`, and finish with `tag NW-x triage=done`.
- Late details: a message with a bug id, a reply to Iosefka's note, a log of the same session or the
  reporter's follow-up within 24 h joins the bug automatically (see `updated_bugs` in `new`). Outside a
  thread the follow-up rule takes only messages of 4+ words (no "thanks", emoji) and at most 5 per bug;
  Iosefka adds no reaction to follow-ups.
- Working a bug: `show NW-x` -> `files NW-x > x.tar`, extract, analyse the logs locally -> `note` with the
  finding -> `tag` (component, feature, version) -> `status open` / `waiting-logs`.
- Handing over: `share NW-x visy "'why'"`, or `assign NW-x visy`.
- Closing: `status NW-x fixed` (note the version first). Add `--tell` only when a player should hear
  something; get the wording approved first.
- Duplicates: `merge NW-keep NW-dup`.
- Release: `cl list --track nw --current`, tidy sections and texts with `cl edit`, `status NW-x fixed --in
  nw:<version>` for bugs fixed in it, then `cl notes nw <version>` for the notes.

## Getting files

```
ssh iosefka files NW-1a2b3c4 > x.tar
mkdir -p ~/clinic/NW-1a2b3c4 && tar -xf x.tar -C ~/clinic/NW-1a2b3c4
```

Extract into a private folder OUTSIDE any git repository, and delete it when done. Logs from the
Nightwatch launcher (`nightwatch-*.log`) and the game logs can be read with `scripts/net_doctor.py` in
the bbport repo (pass the log files; it prints a connection diagnosis).

## Rules

- Logs, names, Steam/PSN names and IPs are private. Never paste them into GitHub (issues, commits, PRs,
  release notes), public Discord, or any chat outside this work. Quote a finding, not the log lines.
- `--tell` text is PUBLIC: no names, no IPs, no log lines. So is changelist item `text`; private detail
  goes in `--dev`.
- Keep extracted logs out of repositories; delete the folder afterwards.
- Do not run `export`, `confirm-export` or `import` (visy only).
- Upload policy (2026-10-28): only files that carry names / IPs (logs, text, crash dumps, game files) make
  Iosefka delete a message and post her replacement note. Saves, pictures and videos hold no personal data:
  they are copied into the clinic (videos sampled to stills) and the message stays on Discord untouched,
  with no note and no reaction. A message with a log AND saves is still deleted and replaced.
- Archives: bigger than 100 MiB (`ARCHIVE_CAP_BYTES` in `clinic_logic.py`) they are never downloaded or
  touched; a readable archive with no log, save, image or video inside (a mod pack, a tool) is left alone,
  not copied. Archives with a log inside are deleted and replaced; with only saves / pictures / videos
  they are copied and the message stays. An unreadable archive still counts as holding a log. The cull
  uses the same rule, so it never deletes save / picture / video-only messages.

## Troubleshooting

- `Permission denied (publickey)`: your key is not added yet, or the Host alias points at the wrong key.
- `iosefka: ... invalid choice` / `unknown command`: only the commands above exist; check spelling.
- `iosefka: bad quoting`: unbalanced quotes; the string you sent through ssh is split again on the server.
  Put the text in single quotes inside double quotes (see the note example above).
- `export is for visy only`: visy-only command.
- `no such bug`: the id is typed wrong, or it was never filed; try `list --q`.
- Empty output from `files`: the filters matched nothing (it prints an empty tar).

## Server side: live duty and the cull (visy)

Flags in `~/.config/netdoctor/env`: `NW_LIVE=1` turns live duty on (archive + replace), `NW_DELETE=1` lets
it delete (and is required for `cull` and `starter-test`). Both default off; the bot then behaves as
before. `iosefka_cull.py` phases: `harvest` (resumable; reruns skip verified messages and retry
`archive_failed` ones; stops with exit code 3 and "disk low, resume later" when a download would leave
less than 1.5 GB free), `verify`, `report --out PATH`, `starter-test --thread ID`, `cull [--limit N]`
(deletes only messages whose every removable file is in a confirmed export; posts without pings), and
`announce --channels general,help [--dry-run] --yes` (posts and pins the announcement and prints the
links). Places the bot cannot read are logged as `no access: <place>` and counted, never fatal.
A message holding a video is not deleted until the video has stills in the archive (stills are off while
the archive is above 2.4 GiB or the disk has under 1 GB free; a warning goes to the first `staff*` channel).

## Health, alerts and deploy

Iosefka watches herself so nobody has to. Everything is stored in the clinic database (`nw_health`,
`nw_alerts`, `nw_errors`, `nw_queue`).

**Where reports go.** The daily report and every alert are posted in the thread `NETDOCTOR_HEALTH_THREAD`
(default 1563914486996279387, "Iosefka's Triage" in #staff). She only SENDS there: the staff skip rule is
unchanged, she never reads, archives or culls anything in `staff*` channels or threads. When Discord answers
403/404 for the thread she logs that once and DMs the owner instead (`NETDOCTOR_ALERT_USER`, the Discord
user id of visy; set it in `~/.config/netdoctor/env`). With neither, alerts are only logged.

**Daily report** at `NETDOCTOR_HEALTH_AT` (default `08:30`) in `NETDOCTOR_HEALTH_TZ` (default
`Europe/Helsinki`, DST-aware): uptime and restarts, jobs on time or overdue, errors in 24 h, disk, DB size,
queue sizes, new bugs and logs. No player names, IPs or log lines. Send one now:
`iosefka_cull.py health-report [--dry-run]` (the dry run prints it).

**Nightwatch VPN monitor.** Every 5 minutes (the health loop) she runs `ssh -o BatchMode=yes -o ConnectTimeout=10
$NETDOCTOR_VPN_SSH` (default alias `nwvpn`; `off` disables). That alias is an ssh Host in quad.fi's
`~/.ssh/config` and its key is restricted on the VPN server to the forced command
`python3 ~/nwvpn/vpn_count.py` (`tools/discord/vpn/vpn_count.py`), which asks the OpenVPN management interface
(127.0.0.1 only) for `load-stats` and prints one JSON line `{"ok":true,"clients":N,"max_clients":M,"pool":253}`.
Only the NUMBER of clients and the limit are read: no addresses or names are collected, stored or alerted, and the
server's address lives only in the ssh config outside the repo. Samples (`nw_vpn`: time, clients, max) are kept
90 days. The effective limit is `min(max-clients, 253)` (pool 10.8.0.0/24). Alerts (6 h rate limit, resolved when
fixed): `VPN near its limit` at 80% of the effective limit, `VPN full` at 100% (both say to raise `max-clients`;
beyond 253 the subnet must widen to /23 plus a firewall update), `VPN monitor down` when samples fail for over
30 min after at least one good one. The daily report and `health` show current players, 24 h and 7 d peak and the
limit. Without a working alias (nothing stored) `health` says `VPN monitor: not set up` and nothing is alerted.

**Alerts** (each kind at most once per 6 h, unless it was resolved and came back; none contains names, IPs
or log lines): a new traceback in her own handling (deduplicated by first line, count per hour), an overdue
job (hourly sweep > 2.5 h, outbox sender > 10 min, nightly backup > 30 h, Nightwatch sync via the last
timestamp in `nw_sync.log` > 3 h or a failed last run), Forbidden on a place she needs (once per place),
disk free < 2 GB, DB above 80% of its cap, `triage=pending` > 15, `unsure` > 15, failed integrity check,
missing tools. A watchdog task checks every 5 minutes.

**`health` command** (both devs): `ssh iosefka health [--text]` shows the last start, restarts, each job's
last run, errors of the last 24 h (counts and first lines, IPs masked), disk free, DB size against the cap,
queue sizes, open alerts, and versions (git commit, discord.py, python, 7z/unrar/ffmpeg present).

**Start-up self-check** (on_ready, logged as `self-check: ...`): permissions per non-skipped place (view,
read history, send, and manage messages when NW_DELETE=1), `PRAGMA quick_check`, and the 7z, unrar and ffmpeg
programs. Problems are alerted as ONE message listing channel names. Places whose name is skipped (staff*, dev*,
moderator-only), and private places neither @everyone nor she can view, are never checked; Send Messages is only
required where @everyone can post or attach files (forums: Send Messages in Threads + Manage Threads/Messages).
Open `forbidden:<channel>` alerts of places that are no longer a problem are closed.

**Weekly DB upkeep** (about 05:00 local, every 7 days): `PRAGMA integrity_check`, WAL checkpoint(TRUNCATE),
`VACUUM` only when free disk >= 2x the DB size.

**Flood guard**: at most `NETDOCTOR_UPLOADS_PER_HOUR` (default 10, 0 = off) upload messages per author and
hour are handled live in guild channels. The rest are stored in `nw_queue` and handled by the hourly sweep in
order (before the catch-up scan): they are still archived and deleted, only later, and never dropped.

**Deploy** on quad.fi: `sh ~/bbport/tools/discord/deploy.sh [--dry-run]`. It records the current commit,
`git pull --ff-only`, runs `pytest tests -q -k clinic` with `~/netdoctor-venv` (pytest is installed there
with `uv pip install --python ~/netdoctor-venv/bin/python pytest` if missing), kills her process by the exact
pattern `[t]ools/discord/netdoctor_bot.py` and starts it with `keepalive.sh`, waits for the `netdoctor bot
ready as` log line, then runs `iosefka_cli.py --as visy health`. Any failure resets to the recorded commit,
restarts, and sends an alert (`clinic_health.py alert TEXT`, plain REST, same destination rules).
