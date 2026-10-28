# Iosefka's changelists: design (2026-10-28)

Extends Iosefka's clinic (design: 2026-10-28-iosefka-clinic-design.md) with versioned changelists for the
two projects, kept in the clinic database on quad.fi and used only by visy and vlad through their Claudes
over the existing SSH bug tool (`iosefka_cli.py --as <who>`, forced command).

## Intent

Today the coop mod's changelist lives in agents/PLAN_077.md (a hand-edited table) and the GitHub release
notes; Nightwatch has none (vlad prefixes commit messages with the version, e.g. "0.2.5 beta 3: ...").
The devs want one place both Claudes can query and update, linked to the clinic's bugs, so they can ask:

- "show me the current 0.7.7 changelist"
- "add the current local bug items to the next coop version changelist"
- "was this changelist item already fixed on the NW side?"
- "all open bugs with the chalice tag", "open bugs older than two weeks" (bug queries by metadata and age)

User decision: **Iosefka's database becomes the home** of the changelists ("offload these to Iosefka's
database"). PLAN_077 and later release notes are generated from / superseded by it.

## Goals

1. Two tracks, `coop` (bbport coop mod) and `nw` (Nightwatch). Each has versions; each version has items.
2. Query, add, edit, move, set status, link items to bugs/commits/local cases; filters like bug `list`.
3. NW track fills itself from the NightwatchCoop git log (commit subject prefix = version), idempotent.
4. Coop track: one-time import of PLAN_077 (0.7.6 + 0.7.7) and earlier GitHub release notes; the
   unmatched local player_logs cases come in as clinic bugs (source `local`, the pending import).
5. Bugs and items connected both ways; marking a bug fixed in a version adds or links an item.
6. Cross-track check: given an item or bug, list related items/bugs in the other track.
7. Release notes rendered from a version's done items: Added / Changed / Fixed markdown, public text only.
8. Bug `list` gains age filters and sorting.
9. CLINIC.md and the Claude skill document all of it with natural-language examples, for vlad's Claude.

## Non-goals

- No code runs on quad.fi for sync or checks: git reading and code digging happen on the dev's PC
  (their Claude / a small script there), which pushes data to Iosefka.
- No Discord posting of changelists; players never see them. No new Discord permissions.
- No web UI.
- No automatic GitHub release publishing (notes are rendered; a human/Claude pastes them).

## Anti-goals (failure even if it works)

- Player names, IPs, Discord ids or log excerpts in public item text or rendered notes.
- Duplicate items from re-running a sync or import.
- A dev losing edits because a sync overwrote their hand-tidied text.
- Server shell access or arbitrary file reads creeping into the forced command.
- Disk growth that matters on quad.fi (~5 GB free): changelists are small text rows only.

## Data model (clinic DB, same SQLite file)

`nw_versions`: `track` (coop|nw), `version` (free text, e.g. "0.7.7", "0.2.5 beta 3"), `status`
(planned | current | released), `released` (epoch or null), `notes` (private text), `created`, `updated`.
Primary key (track, version). At most one `current` per track (setting a new current demotes the old one
to planned unless it is released).

`nw_changes`: `id` CL-xxxxxx (6 hex, random, unique), `track`, `version`, `section`
(added | changed | fixed), `text` (public one-liner), `dev` (private notes, may hold paths), `status`
(planned | in-progress | done | dropped), `tags` (JSON like bug tags), `bugs` (JSON list of NW ids),
`cases` (JSON list of local case folder names), `commits` (JSON list of hashes), `source_key` (unique
nullable: `git:<hash>`, `plan077:<item>`, `release:<tag>:<n>`, `bug:<NW id>`), `author` (who), `created`,
`updated`, `locked` (JSON list of fields a human set; see "Settled" 1).

Item history: notes on items reuse the note pattern in a new `nw_change_notes` table (change_id, at, who,
kind, text), so "who changed what" is visible like bug notes.

## Commands (new, all through the forced command; JSON by default, `--text` for humans)

Changelists:

- `cl versions [--track T]`: versions with status and item counts.
- `cl version <track> <version> [--status S] [--released DATE] [--note TEXT]`: create/update a version.
- `cl list [--track T] [--version V|--current|--next] [--status S]... [--section S] [--tag k=v]...
  [--bug NW-x] [--q TEXT] [--since DATE] [--older-than DAYS]`: items, filters AND together.
- `cl show <CL-id>`: item with notes and linked bugs' summaries.
- `cl add <track> <version> <section> <text> [--tag k=v]... [--bug NW-x]... [--case NAME]...
  [--commit HASH]... [--status S] [--dev TEXT]`.
- `cl edit <CL-id> [--text T] [--section S] [--dev T] [--tag k=v]...`, `cl status <CL-id> <status>`,
  `cl move <CL-id> <track> <version>`, `cl link <CL-id> [--bug]... [--case]... [--commit]... [--unlink]`.
- `cl import`: JSONL on stdin, one item per line with a `source_key`; upsert by source_key (new rows
  inserted; existing rows update only unlocked fields, see "Settled" 1). This is
  what the NW git sync, the PLAN_077 import and the release-notes import all use. Both devs may run it.
- `cl related <CL-id|NW-id>`: items and bugs in the other track sharing a linked bug, a tag value, or
  enough wording (simple token overlap score; top 10 with the reason for each match).
- `cl notes <track> <version>`: rendered release notes markdown (Added / Changed / Fixed, done items
  only, public `text` only, masked with `mask_ips`).

Bugs:

- `list` gains `--older-than DAYS` (created before now-N days), `--idle DAYS` (no activity for N days),
  and `--sort newest|oldest|activity` (default newest, today's behavior).
- `status <NW-id> fixed --in <track>:<version>` additionally upserts an item (`source_key bug:<NW id>`,
  section fixed, text = bug title, status done unless the version is planned → planned) and links it.

## Sync scripts (dev PC side, in the repo)

- `tools/discord/cl_sync_nw.py <nightwatch-src path>`: reads `git log` subjects, groups by a leading
  version prefix (`^(\d+\.\d+\.\d+(?: (?:alpha|beta|rc) ?\d+)?):`), maps the rest to a section by keyword
  (fix/fixed → fixed, add/new → added, else changed), emits JSONL with `source_key git:<hash>` and pipes
  it to `ssh visy@quad.fi cl import`. Commits without a prefix are skipped. Dry-run flag prints JSONL.
- `tools/discord/cl_import_coop.py`: one-time import: parses agents/PLAN_077.md tables/bullets into
  0.7.6 / 0.7.7 items (status from the plan/merged state where recorded, else planned; player_logs paths
  go to `dev`/`cases`, never `text`) and the fork's GitHub release notes (public API via HTTPS, no gh CLI
  on this PC) into released versions. Also builds the local-case tar for the existing `import` command.
- After the import, PLAN_077.md gets a header "moved to Iosefka's clinic (cl list --track coop)" and each
  row its CL id; the file is no longer edited.

## Privacy

`text` is public-safe by rule: `cl add/edit/import` run `mask_ips` on it and reject text containing a
Discord mention/snowflake-looking id or a `player_logs` path (point to `dev` instead). `cl notes` renders
`text` only. `dev` and `cases` never leave the CLI except in `cl show` / `cl list` JSON to the two devs.

## Access

visy and vlad can read and write both tracks; no new visy-only commands. Every write leaves a change note
with `who`.

## Docs

CLINIC.md: new "Changelists" section (concepts, commands, the sync script, examples phrased as what you'd
say to Claude). iosefka-skill/SKILL.md: new triggers and examples ("show me the current NW changelist",
"was X fixed on NW side", "make the 0.2.6 release notes"). Message to vlad (already sent by the user)
describes the expected result; docs must match it.

## Builder's calls (left to the builder, decided here)

- Section set fixed at added/changed/fixed (matches the coop release notes format).
- CL id is 6 hex (bugs use 7) so the two id kinds never collide visually.
- `--next` = lowest planned version above current by simple version-key compare; no `--next` if none.
- Cross-track matching is heuristic and explains itself; no ML, no embeddings.
- Tests in tests/test_clinic_changes.py (store + CLI), same fakes/patterns as the existing clinic tests.
- Deploy: pull on quad.fi, restart not needed for the CLI (bot untouched except nothing), schema is
  `CREATE TABLE IF NOT EXISTS`, no migration of existing tables.

## Settled after the builder check (builder's calls, made by the designer)

1. **Human edits win, per field.** Items keep a `locked` JSON list of fields a human set through
   `cl edit/status/move/link` (text, section, status, version, tags). `cl import` never writes a locked
   field. For unlocked status, import only moves forward (planned -> in-progress -> done); it never
   reopens or drops. Replaces the single `edited` flag.
2. **One item per fix.** `status NW-x fixed --in T:V` first looks for an item in track T that already
   links that bug (any version); if found it links/notes it (status forward, version untouched if locked),
   else creates `bug:<NW id>`. The NW sync links bugs whose id appears in a commit subject/body. PLAN077
   and release-note items link the bugs/cases they name. Remaining near-duplicates show up in
   `cl related`; `cl merge <keep> <dup>` folds one item into another (links unioned, dup gets status
   dropped with a note). Rewritten NW history (new hashes) is rare; dupes are handled by `cl merge`.
3. **Versions from sync.** `cl import` lines may be version lines `{"kind":"version", track, version,
   status, released}` as well as item lines `{"kind":"item", ...}`. The NW sync sends: newest prefix =
   current, older prefixes = released (released = date of the newest commit with that prefix); its items
   are done. Import never demotes a version a human set (versions get the same lock rule).
4. **Import behavior.** Per-line: good lines are applied, bad lines skipped and reported
   (`{"applied": n, "skipped": [{"line": k, "why": ...}]}`); each line is its own transaction. `who` is
   the SSH user; stdin capped at 8 MB. Item line fields: required `source_key, track, version, section,
   text`; optional `status, tags, bugs, cases, commits, dev`.
5. **Coop import text.** The coop import JSONL is prepared on the PC by Claude from PLAN_077 and the
   release notes, with clean public one-liners (row codes like D8 and paths go to `dev`), reviewed in the
   JSONL before sending. PLAN_077 prose bullets become 0.7.7 items (features -> added, reports -> fixed).
   PLAN statuses: running -> in-progress, queued/blank -> planned. 0.7.6 table items whose fix is in the
   0.7.6 release notes are done in 0.7.6; the rest move to 0.7.7 as planned. All GitHub releases on the
   fork are imported as released versions with done items (section from the notes' heading).
6. **Unmatched local cases.** A player_logs case folder is "unmatched" when no clinic bug note, item
   `cases` entry or PLAN text names it. Each becomes a clinic bug through the existing `import` tar
   format (folder + case.json: title = cleaned folder name or the case's own notes headline, tags from
   any matching PLAN item, status open). visy-only as today.
7. **Version order.** Split numbers then a stage: alpha < beta < rc < final (no stage = final), stage
   number ascending. "0.2.5 beta 3" < "0.2.5 rc 1" < "0.2.5".
8. **Item dates.** `--since` / `--older-than` on items use `created`; `--idle` uses `updated`.

## Addendum (user, 2026-10-28): original timestamps

User: "add timestamps from the original messages so that they correlate with bug report times and
changelist add times can be separate from that / triage time".

- **Bugs: `reported`** = the earliest original message time of the bug (Discord message `created`,
  computed from nw_messages, so merges/moves stay right); falls back to the bug's `created` when it has no
  messages. Bug `created` stays the filing/triage time and is shown as `filed`. `list`/`show`/`new` output
  carries `reported` and `filed`; `list --since`, `--older-than` and `--sort newest|oldest` use `reported`;
  `--idle` keeps using activity. Local `import` cases take `reported` from case.json (ISO date/datetime or
  epoch) for their message time instead of now.
- **Changelist items: `dated`** = when the change itself happened (NW commit date, release published date,
  linked bug's `reported` for `fixed --in` items, optional `dated` in import lines, `cl add/edit --dated`),
  separate from `created` (added to Iosefka, shown as `added`). Import fills `dated` unless locked; a human
  `cl edit --dated` locks it. `cl list` output shows both; `cl list --sort added|dated` (default added,
  newest first). Existing `--since/--older-than` on items stay on `added`; new `--dated-since DATE`.
