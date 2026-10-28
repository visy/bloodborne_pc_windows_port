---
name: iosefka
description: Read and triage Nightwatch Co-op bug reports through Iosefka's clinic (the private archive of player logs from the Nightwatch Discord). Also keeps versioned changelists for the coop and NW tracks and makes release notes. Use for Nightwatch bug reports, NW-xxxxxxx bug ids, CL-xxxxxx changelist items, "check new bugs", "any open launcher bugs", "Iosefka", player logs from the Discord, bug triage, "the changelist", "what is planned for 0.7.7", "release notes", and "sync the NW changelist".
---

# Iosefka's clinic

The clinic is a private archive of Nightwatch Discord logs, with the text, screenshots and video stills
around them, grouped into bugs `NW-xxxxxxx` (e.g. `NW-1a2b3c4`). You reach it with `ssh iosefka <command>`
from the Bash tool. The full reference is `tools/discord/CLINIC.md` in the repository
visy/bloodborne_pc_windows_port on GitHub; read it when a command or option here is not enough.

Output is JSON: parse it (for example `ssh iosefka list --open | python -c "import sys,json; ..."`) rather
than reading by eye. `--text` after the command name gives a human summary.

## Hard rules

1. Logs, player names, Steam/PSN names and IP addresses are private. Never put them in GitHub (issues,
   commits, PRs, release notes), public Discord, a public paste, or code comments. Describe findings
   without them.
2. Extract logs only into a temporary folder outside any git repository, and delete it when the task ends.
3. Never use `--tell` without the user's explicit OK of the exact wording. It posts publicly. It must
   hold no names, IPs or log lines.
4. Never run `export`, `confirm-export` or `import`; they belong to visy.
5. Do not change statuses or tags the user did not ask for; note, tag and status writes are visible to
   the other dev.

## Queries

- All open launcher bugs: `ssh iosefka list --open --tag component=nw-launcher`
- Open co-op chalice bugs: `ssh iosefka list --open --tag component=coop-mod --tag feature=chalice`
- Pull them with logs: `ssh iosefka files --open --tag component=coop-mod --tag feature=chalice > b.tar`
- Filters (AND): `--status s` (repeatable), `--open`, `--tag k=v` (repeatable, case-insensitive, list
  tags match any element), `--q text`, `--since YYYY-MM-DD`, `--assignee who`, `--older-than DAYS`
  (created more than DAYS ago), `--idle DAYS` (no activity for DAYS), `--sort newest|oldest|activity`.
- Open bugs older than two weeks: `ssh iosefka list --open --older-than 14`
- Tag keys: area, feature, component (coop-mod, nw-launcher, nw-port, game-data, network, unknown),
  version, role, plan. Statuses: new, open, analysing, waiting-logs, fixed, closed, duplicate.

Quote text yourself: the command string is split on the server, so write
`ssh iosefka note NW-1a2b3c4 "'the finding'"` (single quotes inside double quotes).

## Changelists

Versioned change items `CL-xxxxxx` per track (`coop`, `nw`); both devs read and write both. Statuses:
planned, in-progress, done, dropped; sections: added, changed, fixed. Fields a dev sets by hand are
locked against later syncs. Item `text` is PUBLIC (release notes): no names, IPs, Discord ids or
player_logs paths; private detail goes in `--dev`. Quote multi-word arguments as `"'text'"` (split twice). Full reference: the Changelists section of CLINIC.md.

| user says | run |
|---|---|
| show the current NW changelist | `ssh iosefka cl list --track nw --current` |
| what's planned for coop 0.7.7 | `ssh iosefka cl list --track coop --version 0.7.7` |
| add these bugs to the next coop version | `ssh iosefka cl versions --track coop`, then `ssh iosefka cl add coop 0.7.8 fixed "'public text'" --bug NW-1a2b3c4` per item |
| was NW-1a2b3c4 fixed on the NW side | `ssh iosefka cl related NW-1a2b3c4`, `ssh iosefka cl list --track nw --bug NW-1a2b3c4` |
| mark NW-1a2b3c4 fixed in NW 0.2.6 | `ssh iosefka status NW-1a2b3c4 fixed --in nw:0.2.6` |
| make the 0.2.6 release notes | `ssh iosefka cl notes nw 0.2.6` (public-safe; re-read before pasting) |
| move this item to 0.7.8 | `ssh iosefka cl move CL-1a2b3c coop 0.7.8` |
| drop this item | `ssh iosefka cl status CL-1a2b3c dropped` |
| fix an item's wording or section | `ssh iosefka cl edit CL-1a2b3c --text "'new wording'" --section fixed` |
| sync the NW changelist | on the PC: `python tools/discord/cl_sync_nw.py <NightwatchCoop clone> --host iosefka` (`--dry-run` to preview; default host is visy@quad.fi) |

The NW sync reads git log: `0.2.5 beta 3:` style subject prefixes name the version (unprefixed commits
join the next prefixed one), the VERSION file is the current version, each commit is one item keyed by
hash (safe to re-run), and sections are guessed from keywords, so tidy them with `cl edit`. After the
user pushes NW commits, run the sync and report the new items. `status NW-x fixed --in T:V` moves the bug's live item to done (and to V unless its version was set by hand) or adds a done item (planned if that version is still planned); do it per track. Run the NW sync first: it sets NW current from the VERSION file, and a hand-set version status locks it. Check the `locked` list in the import result.

## Procedures

Check new bugs:
1. `ssh iosefka new`. Summarise the bugs (id, title, counts) and notes shared with the user.
   `updated_bugs` are older bugs that got new messages or files (players add details later; fixed or
   closed ones are reopened with a note). `triage_pending` counts the triage queue.
2. Offer to triage; do not change anything yet.

Triage pass (bugs Iosefka filed and was unsure about):
1. `ssh iosefka triage-queue`: bugs with `triage=pending` (oldest first) and `auto-link` notes (messages she
   attached by a weaker rule). Tag `unsure` says what she could not settle (network-or-bug, which-bug,
   no-log, unreadable-file, no-text).
2. Per bug: `show NW-x`, `files NW-x` (see Triage a bug); read the logs, party logs with `scripts/net_doctor.py`.
3. Set `title NW-x "'text'"`, `status`, and tags: component, area, feature, version, role, plan, severity.
4. Resolve each unsure item; a wrongly linked message goes back with `move <message_id> NW-y`.
5. Check duplicates (`list --open --q ...`, then `merge`) and whether CHANGES.md or the Nightwatch repo
   already has the fix.
6. Write a triage `note`, then `tag NW-x triage=done`.

Triage a bug:
1. `ssh iosefka show NW-x` (use `--brief` for long threads). Read the messages, notes and file counts.
2. Make a temp dir (`mktemp -d`), then `ssh iosefka files NW-x > x.tar` and `tar -xf x.tar -C <tempdir>`.
   `NW-x/bug.json` is the same as `show`; logs are in `logs/`, screenshots in `images/`, video frames
   in `stills/`. Look at images and stills when the report is visual.
3. Analyse the logs: for connection problems run the repository's `scripts/net_doctor.py` on them.
4. Write the finding as a note, without names or IPs: `ssh iosefka note NW-x "'...'"`.
5. Set structure only as asked or clearly implied: `tag NW-x component=... feature=... version=...`,
   `status NW-x open|waiting-logs|fixed|closed`, `status NW-x duplicate --dup NW-y` or `merge NW-keep
   NW-dup`.
6. Delete the temp dir.

Hand over: `ssh iosefka share NW-x visy "'why it matters'"` or `assign NW-x visy`.

Close a bug: write a note with the fixing version, then `ssh iosefka status NW-x fixed`. If a player
should hear about it, draft the text, show it to the user, and only after approval add
`--tell "'text'"`.

Duplicates: `ssh iosefka merge NW-keep NW-dup` (the second becomes a duplicate of the first).

## Errors

- `Permission denied (publickey)`: the key is not set up; point the user at the setup section of
  CLINIC.md.
- `iosefka: ...` messages come from the tool; exit 2 is a bad command line (check quoting), exit 1 is a
  missing bug or a visy-only command.
