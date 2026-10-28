# Iosefka's Changelists Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Versioned coop/nw changelists in Iosefka's clinic DB, queried and edited by visy and vlad through
the SSH bug tool, linked to bugs, synced from the NW git log, rendered into release notes.

**Architecture:** New store module `clinic_changes.py` (tables in the same SQLite file, opened on the
`Clinic` connection), new CLI module `iosefka_cl.py` plugged into `iosefka_cli.py` as the `cl` command,
PC-side scripts `cl_sync_nw.py` and `cl_import_coop.py` that emit JSONL for `cl import`. Standard library
only on the server side (the CLI must run without discord.py).

**Tech Stack:** Python 3.12 (quad.fi venv), sqlite3, argparse, pytest.

**Spec:** docs/superpowers/specs/2026-10-28-iosefka-changelists-design.md (read it, especially
"Settled after the builder check").

## Global Constraints

- Tracks: `coop`, `nw`. Sections: `added`, `changed`, `fixed`. Item statuses: `planned`, `in-progress`,
  `done`, `dropped`. Version statuses: `planned`, `current`, `released`.
- Item id `CL-` + 6 lowercase hex (`^CL-[0-9a-f]{6}$`), random, unique. Bug ids stay `^NW-[0-9a-f]{7}$`.
- Version order: numbers, then stage alpha < beta < rc < final (no stage = final), stage number ascending.
  "0.2.5 beta 3" < "0.2.5 rc 1" < "0.2.5" < "0.2.6".
- At most one `current` version per track; setting one demotes the previous current to `planned`.
- Human writes (`cl edit/status/move/link/version`) add the field names to the row's `locked` JSON list;
  `cl import` never writes a locked field; unlocked item status from import only moves forward
  (planned < in-progress < done; never to dropped, never backward).
- Public `text`: `mask_ips` applied; rejected if it contains `player_logs`, a Discord mention (`<@`) or a
  17-20 digit number. Max 300 chars. `cl notes` renders `text` only.
- `cl import`: per-line, each line its own transaction, bad lines reported as
  `{"line": k, "why": ...}`, result `{"applied": n, "skipped": [...]}`; stdin capped at 8 MB.
- No new visy-only commands; every write leaves a change note with `who`.
- Server side: standard library only. Never commit player names, IPs or log excerpts. Push only
  `git push fork master`.
- Existing clinic tests (161 pass, 2 skip) must stay green: `python -m pytest tests -q -k clinic`.

## Review Focus

- Re-running the NW sync after a dev moved/dropped/reworded an item: nothing the dev set changes
  (Task 1 `test_import_respects_locks`, Task 3 `test_sync_rerun_idempotent`).
- `status NW-x fixed --in nw:V` when a sync item already links NW-x: no second item
  (Task 2 `test_fixed_in_links_existing_item`).
- Text with an IP / player_logs path / Discord id through `cl add` and `cl import`: masked or rejected,
  never stored raw, never in `cl notes` (Task 1 `test_text_rules`, Task 2 `test_notes_public_only`).
- Garbage JSONL lines (bad JSON, unknown track, missing field) among good ones: good ones applied, bad
  ones listed (Task 1 `test_import_partial`).
- Shell metacharacters / quoting in `SSH_ORIGINAL_COMMAND` for `cl add` text: parsed by shlex, stored
  literally (Task 2 `test_cl_add_via_forced_command`).

---

### Task 1: Changelist store

**Files:**
- Create: `tools/discord/clinic_changes.py`
- Test: `tests/test_clinic_changes.py`

**Interfaces:**
- Consumes: `clinic_store.Clinic` (use its `.db` connection), `clinic_logic.mask_ips`.
- Produces (all in `clinic_changes.py`):
  - constants `TRACKS, SECTIONS, ITEM_STATUSES, VERSION_STATUSES`, `CL_RX`
  - `version_key(v: str) -> tuple`
  - `clean_text(t: str) -> str` (raises `ValueError(why)`)
  - `class Changes(clinic)`:
    - `set_version(track, version, *, status=None, released=None, notes=None, who, human=True) -> dict`
    - `versions(track=None) -> list[dict]` (sorted by version_key, each with `counts` per item status)
    - `current(track) -> str|None`, `next(track) -> str|None`
    - `add(track, version, section, text, *, who, status="planned", tags=None, bugs=(), cases=(),
      commits=(), dev="", source_key=None) -> str` (CL id; creates the version as planned if missing)
    - `get(cid) -> dict|None` (fields + tags/bugs/cases/commits/locked as lists/dicts)
    - `items(track=None, version=None, status=None, section=None, tag=None, bug=None, q=None,
      since=None, older_than=None, idle=None) -> list[dict]` (status/tag accept lists like
      `Clinic.bugs`; epoch numbers; newest created first)
    - `edit(cid, *, who, text=None, section=None, dev=None, tags=None)`,
      `set_status(cid, status, *, who)`, `move(cid, track, version, *, who)`,
      `link(cid, *, who, bugs=(), cases=(), commits=(), unlink=False)`
    - `merge(keep, dup, *, who)`
    - `import_lines(lines: list[str], *, who) -> dict`
    - `item_for_bug(track, bug_id) -> dict|None`
    - `related(ref: str) -> list[dict]` (`{"kind": "item"|"bug", "id", "track"?, "title"/"text",
      "why": [..], "score"}`, top 10)
    - `render_notes(track, version) -> str`
    - `notes(cid) -> list[dict]`

Tables (`CREATE TABLE IF NOT EXISTS`, created in `Changes.__init__`): `nw_versions(track, version,
status, released, notes, locked, created, updated, PRIMARY KEY(track, version))`, `nw_changes(id PRIMARY
KEY, track, version, section, text, dev, status, tags, bugs, cases, commits, source_key UNIQUE, author,
locked, created, updated)`, `nw_change_notes(change_id, at, who, kind, text)`.

- [ ] **Step 1: Write failing tests** in `tests/test_clinic_changes.py` (fixture: `Changes(Clinic(tmp_path/"nd.db"))`):
  - `test_version_key_order`: sorted(["0.2.6","0.2.5","0.2.5 rc 1","0.2.5 beta 3","0.2.5 beta 10"], key=version_key) == ["0.2.5 beta 3","0.2.5 beta 10","0.2.5 rc 1","0.2.5","0.2.6"]
  - `test_one_current_per_track`: set 0.7.6 current, then 0.7.7 current → 0.7.6 is planned, `current("coop")=="0.7.7"`, nw track untouched; `next` returns lowest planned above current, None if none.
  - `test_add_and_filters`: add 3 items across tracks/sections/tags; `items(track="coop", tag="area=chalice")`, `items(status=["planned","done"])`, `items(bug="NW-0000001")`, `items(q="lamp")`, `older_than`/`idle` with patched `time.time` return the expected ids.
  - `test_text_rules`: `clean_text("host at 85.12.3.4")` masks to contain `<public IP>`; `"see player_logs/x"`, `"<@123>"`, `"id 156209977686910567"` raise ValueError; 301 chars raise.
  - `test_import_respects_locks`: import an item line (source_key git:abc, version 0.2.5, status in-progress); human `move` to 0.2.6 and `edit(text=...)`; re-import with version 0.2.5, status done, new text → version 0.2.6 and human text kept, status becomes done (unlocked, forward); importing status planned afterwards leaves done.
  - `test_import_partial`: lines `[good, "{bad", {"kind":"item","track":"x",...}, missing text, good version line]` → applied 2, skipped lines 2,3,4 with reasons.
  - `test_import_version_lines_respect_human`: human sets 0.2.5 current; import version line status released → stays current.
  - `test_merge`: links unioned into keep, dup status dropped, note on both.
  - `test_related`: nw item linking NW-0000001 and coop item linking it → related(coop id) lists the nw item with why containing "bug NW-0000001"; tag value overlap and word overlap also give reasons; same-track items excluded.
  - `test_render_notes`: done items only, `### Added / ### Changed / ### Fixed` headings in that order, empty sections omitted, `- text` bullets, dev text absent.
- [ ] **Step 2: Run** `python -m pytest tests/test_clinic_changes.py -q` → FAIL (module missing).
- [ ] **Step 3: Implement** `clinic_changes.py`. Word overlap for `related`: lowercase tokens ≥4 chars minus a small stopword set, score = shared bugs×10 + shared tag values×3 + shared tokens; items and bugs (via `Clinic.bugs()` titles/tags) from the other track only (bugs count as track `nw` when tag component starts with `nw-`, else `coop`); drop score < 2.
- [ ] **Step 4: Run** the test file → PASS; then `python -m pytest tests -q -k clinic` → all green.
- [ ] **Step 5: Commit** `git add tools/discord/clinic_changes.py tests/test_clinic_changes.py && git commit -m "clinic: changelist store (versions, items, locks, import, related, notes)"`

### Task 2: `cl` CLI and bug query additions

**Files:**
- Create: `tools/discord/iosefka_cl.py`
- Modify: `tools/discord/iosefka_cli.py` (parser: `cl` subcommand; `list` new flags; `status --in`; dispatch)
- Test: `tests/test_clinic_cl_cli.py`

**Interfaces:**
- Consumes: Task 1 `Changes` and constants.
- Produces: `iosefka_cl.add_parser(sub, common)` registering `cl` with nested subcommands
  `versions, version, list, show, add, edit, status, move, link, merge, import, related, notes`
  (flags exactly as the spec "Commands" section); `iosefka_cl.run(a, clinic, who, emit, stdin) -> int`.
  `list` in iosefka_cli gains `--older-than DAYS`, `--idle DAYS`, `--sort newest|oldest|activity`
  (activity = `activity` or `updated`, newest first); `status ... --in TRACK:VERSION` only with `fixed`.

- [ ] **Step 1: Write failing tests** (pattern of `tests/test_clinic_cli.py`: `run(db, who, *argv)`):
  - `test_cl_add_list_show`: `cl add coop 0.7.7 fixed "Guest lamp after join" --tag area=lamps --bug NW-xxxxxxx` (seeded bug) → id matches CL_RX; `cl list --track coop --version 0.7.7` contains it; `cl show` includes the bug summary and an `add` note by the caller.
  - `test_cl_current_next_flags`: `cl version coop 0.7.6 --status current`, `cl version coop 0.7.7 --status planned`; `cl list --current` / `--next` filter accordingly; `--next` with none → empty list, exit 0.
  - `test_cl_import_stdin`: JSONL via stdin → JSON `applied`/`skipped`; >8 MB stdin → exit 2 with message.
  - `test_fixed_in_links_existing_item`: import an nw item linking NW-x; `status NW-x fixed --in nw:0.2.6` → no new item, existing item status done, version unchanged if locked; with no existing item → new item `source_key bug:NW-x`, section fixed, text = masked bug title.
  - `test_fixed_in_bad`: `--in` with status open → exit 2; `--in foo` → exit 2.
  - `test_notes_public_only`: `cl notes coop 0.7.7` output (raw markdown, not JSON, also under `--text`) has no `dev` text and no IP.
  - `test_cl_add_via_forced_command`: `entry()` with `SSH_ORIGINAL_COMMAND='cl add nw "0.2.5 beta 3" added "Free stamina; out of combat $(rm -rf /)"'` stores that literal text.
  - `test_list_age_and_sort`: seeded bugs with patched created/activity: `list --older-than 10` and `--idle 5` and `--sort oldest` give expected order; bad values (`--older-than x`, `--sort foo`) exit 2.
  - `test_vlad_can_write_both_tracks`: who="vlad" can `cl add coop ...` and `cl add nw ...`.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement** `iosefka_cl.py` + wire into `iosefka_cli.py` (`_run`: `if a.cmd == "cl": return iosefka_cl.run(...)`; update the "no command" help string; `_text_out` gets a compact line format for `cl list` items: `CL-xxxxxx  track version  status  section  text`). `ValueError` from the store → `CliError(str, 2)`; unknown id → exit 1.
- [ ] **Step 4: Run** new tests → PASS; `python -m pytest tests -q -k clinic` → green.
- [ ] **Step 5: Commit** `git commit -m "iosefka: cl command (changelists) + bug list age filters and sort"`

### Task 3: PC-side sync and import scripts

**Files:**
- Create: `tools/discord/cl_sync_nw.py`, `tools/discord/cl_import_coop.py`
- Test: `tests/test_clinic_cl_sync.py`

**Interfaces:**
- Produces:
  - `cl_sync_nw.parse_log(text: str) -> tuple[list[dict], list[dict]]` (item lines, version lines) from
    `git log --format=%H%x1f%cI%x1f%s%x1f%b%x1e` output; `main(argv)`: `cl_sync_nw.py <repo> [--dry-run]
    [--host visy@quad.fi]` runs git, builds JSONL, pipes it to `ssh -o BatchMode=yes <host> cl import`
    (or prints with `--dry-run`), prints the import result.
  - `cl_import_coop.parse_release(tag: str, body: str) -> list[dict]` (item lines from GitHub release
    markdown: `## Added/Changed/Fixed` or `### ...` headings, `-`/`*` bullets, `source_key
    release:<tag>:<n>`, status done, version = tag without leading `v`); `main(argv)`: `--releases`
    fetches `https://api.github.com/repos/visy/bloodborne_pc_windows_port/releases?per_page=100` with
    urllib and writes JSONL (+ released version lines) to stdout; `--unmatched <player_logs dir>
    <names.json>` lists case folders not named in names.json (a JSON list of strings collected from
    clinic notes/items/PLAN text) as JSON.

- [ ] **Step 1: Write failing tests:**
  - `test_parse_log_versions`: three commits "0.2.5 beta 3: VRAM cleanup", "0.2.5 beta 3: Fix firewall prompt", "Cheats: free stamina" + one "0.2.4: Add logs menu" → 3 items (no-prefix skipped), sections changed/fixed/added, source_key `git:<hash>`, status done; version lines: "0.2.5 beta 3" current, "0.2.4" released with that commit's date; a body mentioning NW-1234567 → `bugs: ["NW-1234567"]`.
  - `test_sync_rerun_idempotent`: feed parse_log output to `Changes.import_lines` twice (with a human `move` in between) → item count unchanged, moved version kept.
  - `test_parse_release`: sample body with `### Added`/`### Fixed` bullets → items with right sections; a body with no headings → bullets go to `changed`; bullets with player_logs paths are dropped from text (go to `dev`).
  - `test_unmatched`: tmp player_logs tree with dated case folders; names list covers one → the others returned.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement** both scripts (standard library; no shell=True; scripts never run on quad.fi).
- [ ] **Step 4: Run** → PASS; `python -m pytest tests -q -k clinic` → green.
- [ ] **Step 5: Commit** `git commit -m "clinic: NW git sync and coop release/case import scripts for changelists"`

### Task 4: Docs for vlad's Claude

**Files:**
- Modify: `tools/discord/CLINIC.md` (new "Changelists" section + bug list flags), `tools/discord/iosefka-skill/SKILL.md` (triggers + examples)

- [ ] **Step 1:** Write the CLINIC.md section: concepts (tracks, versions, items, statuses, locks), every
  `cl` command with one example each, the NW sync (`python tools/discord/cl_sync_nw.py <NightwatchCoop
  clone>` from the PC, safe to re-run), "fixed --in", `cl related`, `cl notes`, privacy rule for `text`,
  and a "what you'd say to Claude → what it runs" table covering: show current NW changelist, what's
  planned for coop 0.7.7, add local bug items to next coop version, was X fixed on NW side, make 0.2.6
  release notes, open bugs older than two weeks, all open chalice bugs.
- [ ] **Step 2:** Update SKILL.md description/triggers to include changelists and release notes, add the
  same examples compactly. Check every command in the docs against `build_parser()` by running each
  example's argv through `iosefka_cli.build_parser().parse_args` in a throwaway snippet (all parse).
- [ ] **Step 3: Commit** `git commit -m "clinic docs: changelists for vlad's Claude"`

### Task 5: Deploy and fill (orchestrator, not a subagent)

- [ ] Full test run green; `git push fork master`; on quad.fi `git -C ~/bbport pull --ff-only`; smoke
  `SSH_ORIGINAL_COMMAND="cl versions"` as vlad (no bot restart needed; bot doesn't use the new tables).
- [ ] `git -C E:\games\bbport\nightwatch-src pull --ff-only`; `cl_sync_nw.py --dry-run` review; real run.
- [ ] Coop: `cl_import_coop.py --releases` → review JSONL → `cl import`. Write the PLAN_077 JSONL by hand
  (clean public text, row codes/paths in dev, statuses per spec Settled 5) → review → `cl import`. Set
  coop 0.7.7 current (0.7.6 released).
- [ ] Unmatched local cases → case.json tar → existing `import` (visy). Spot-check the new ids from the
  import result with `show`.
- [ ] PLAN_077.md header "moved to Iosefka's clinic" + CL ids per row; update memory (iosefka-clinic,
  release memories pointing at PLAN_077) and graft.
