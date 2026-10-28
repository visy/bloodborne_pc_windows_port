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
after <status>"). `triage_pending` is the size of the triage queue.

```json
{"since": "2026-10-28T08:00:00Z",
 "bugs": [ {"id": "NW-1a2b3c4", "title": "Lamp stays dark", "status": "new", "tags": {}, "...": "..."} ],
 "notes": [ {"bug_id": "NW-1b2c3d4", "at": "2026-10-28T09:10:00Z", "who": "visy", "kind": "share",
             "text": "please look at this"} ]}
```

### list

`ssh iosefka list [--status s]... [--open] [--tag k=v]... [--q text] [--since YYYY-MM-DD] [--assignee who]`

Filters combine with AND. `--status` and `--tag` can repeat. `--open` means every status except fixed,
closed and duplicate. A tag value matches case-insensitively, and a tag holding a list matches when any
element equals it (`feature=chalice` finds `["chalice","lamps"]`); `--tag area` (no value) means "has this
tag". `--q` searches title, message texts and notes. Newest first.

```json
[{"id": "NW-1a2b3c4", "title": "Chalice lamp stays dark", "status": "open",
  "tags": {"component": "coop-mod", "feature": ["chalice", "lamps"]}, "assignee": "vlad",
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

`ssh iosefka status NW-1a2b3c4 <status> [--dup NW-1b2c3d4] [--tell "'player-facing text'"]`

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

### tag

`ssh iosefka tag NW-1a2b3c4 component=coop-mod feature=chalice,lamps version=0.2.3`: a comma makes a list;
`key=` with no value removes the tag. Keys: lowercase letters, digits, `-`, `_`.

Suggested keys and values:

| key | values |
|---|---|
| area | map or boss name, e.g. `old-yharnam`, `cleric-beast`, `chalice` |
| feature | `lamps`, `joins`, `chalice`, `endings`, `summons`, `audio`, ... (a list is fine) |
| component | `coop-mod`, `nw-launcher`, `nw-port`, `game-data`, `network`, `unknown` |
| version | build the player ran, e.g. `0.2.3` |
| role | `host`, `guest` |
| plan | the open item name in `agents/PLAN_077.md` that covers it |

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
case with `case.json` `{title, tags, notes, files, status?}` and the files) and creates `local` bugs.

## Workflows

- Morning check: `ssh iosefka new`; open the interesting ones with `show`.
- Triage pass (Claude): `triage-queue`; per bug `show NW-x` + `files NW-x`, read the logs (party logs with
  `scripts/net_doctor.py`), set title (`title`), status and tags (component, area, feature, version, role,
  plan, severity), settle each `unsure` reason (wrong auto-link: `move`), look for duplicates in open bugs
  (`list --open --q ...`, then `merge`), check CHANGES.md and the Nightwatch repo for an already fixed
  cause, write a triage `note`, and finish with `tag NW-x triage=done`.
- Late details: a message with a bug id, a reply to Iosefka's note, a log of the same session or the
  reporter's follow-up within 24 h joins the bug automatically (see `updated_bugs` in `new`).
- Working a bug: `show NW-x` -> `files NW-x > x.tar`, extract, analyse the logs locally -> `note` with the
  finding -> `tag` (component, feature, version) -> `status open` / `waiting-logs`.
- Handing over: `share NW-x visy "'why'"`, or `assign NW-x visy`.
- Closing: `status NW-x fixed` (note the version first). Add `--tell` only when a player should hear
  something; get the wording approved first.
- Duplicates: `merge NW-keep NW-dup`.

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
- `--tell` text is PUBLIC: no names, no IPs, no log lines.
- Keep extracted logs out of repositories; delete the folder afterwards.
- Do not run `export`, `confirm-export` or `import` (visy only).

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
