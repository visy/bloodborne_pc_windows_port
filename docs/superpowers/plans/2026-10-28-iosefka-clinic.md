# Iosefka's Clinic Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Iosefka archives every log (with its bug context) from the Nightwatch Discord into a private
SQLite archive on quad.fi, replaces the public messages with a kind triage note, keeps doing it live, and
gives the two devs an SSH bug tool.

**Architecture:** New focused modules next to the existing bot in `tools/discord/`: a store
(`clinic_store.py`, new tables in the same `netdoctor.db`), pure logic (`clinic_logic.py`), media
handling (`clinic_media.py`), reply texts (`iosefka_voice.py`), Discord operations (`clinic_discord.py`),
a one-shot cull runner (`iosefka_cull.py`), the dev CLI (`iosefka_cli.py`). `netdoctor_bot.py` gains
live duty by calling the same Discord-operation functions the cull uses.

**Tech Stack:** Python 3.12 (bot venv on quad.fi; tests also run on the dev PC's Python), discord.py
2.5.2, sqlite3, lzma, Pillow (new), ffmpeg 2.8 CLI, pytest.

**Spec:** `docs/superpowers/specs/2026-10-28-iosefka-clinic-design.md` (read it first; every
"Builder's call" there is binding).

## Global Constraints

- Skip rule: a guild channel is skipped when its name starts with `staff` or equals `development`;
  threads inherit their parent's rule. `#moderator-only` is included. Nothing else is skipped.
- Bug id: `NW-` + 7 lowercase hex (`secrets.token_hex(4)[:7]`), unique in `nw_bugs`.
- Removable files: extensions `.log .txt .dmp .dcx`, names starting `userdata`, and zip/7z/rar whose
  listing holds any of those (unreadable archives count as removable). Images/videos never removed
  unless they share a message with a removable file.
- A Discord message is deleted only when every attachment of it has an `nw_files` row whose content was
  re-verified (decompress + sha256) AND (cull only) every removable file's sha256 is in a confirmed
  export. Order per message: post replacement -> delete original -> record. Never the reverse.
- All replies use `allowed_mentions=discord.AllowedMentions.none()` during the cull and catch-up; live
  replies may ping only the poster (`AllowedMentions(users=[author], everyone=False, roles=False)`).
- Deletion runs only with env `NW_DELETE=1`; default off (archive-only mode).
- IPs in any public text are masked with net_doctor's `hide()` rules (public IPv4 -> `x.x.x.x`-style
  mask as net_doctor does today); player names never appear in public text except the silent mention.
- Size: whole DB file ceiling 3 GiB (`NETDOCTOR_DB_MAX_MB` capped at 3072); ND cache rows pruned after 180
  days; `nw_*` tables never pruned automatically. At 2.4 GiB or free disk < 1 GB: stills off + one warning
  in the first channel whose name starts with `staff`.
- Video stills: one frame per 4 s, at most 48 evenly spread, 1280 px wide, WebP q60 (JPEG q70 without
  WebP). Images > 2 MB re-encoded WebP q85, max side 2560 px; key stays the original sha256.
- No player data in the repo: tests use invented names/IPs from documentation ranges
  (198.51.100.0/24, 203.0.113.0/24). Never commit anything from quad.fi or `player_logs/`.
- Files stay under `tools/discord/`; tests under `tests/` named `test_clinic_*.py`, loading modules by
  path like `tests/test_netdoctor_bot.py` does.
- Python stdlib + discord.py + Pillow only; the CLI must run without discord.py installed.

## Review Focus

1. Crash mid-cull (after replacement posted, before delete; after delete, before record): a rerun must
   neither double-post nor skip the delete. Test in Task 6 (`test_cull_resumes_each_step`).
2. Forwarded messages (`message_snapshots`) and edits that add a log to an old, uncached message must be
   caught. Tests in Task 5 (`test_snapshot_attachments_counted`) and Task 8 (`test_raw_edit_adds_log`).
3. Discord attachment URL expired or download fails/partial: the message stays (not deleted), state
   `archive_failed`, retried by the next sweep. Test in Task 5 (`test_failed_download_blocks_delete`).
4. Same log posted in two places / twice: one `nw_files` row, two messages, joined into one bug when all
   its logs match. Test in Task 2 (`test_cluster_joins_existing_bug_on_same_logs`).
5. SSH forced-command input (`SSH_ORIGINAL_COMMAND` with `;`, backticks, `$()`, path traversal in
   `files --out`): parsed by shlex only, unknown commands rejected, no file writes on the server outside
   stdout. Test in Task 7 (`test_forced_command_rejects_shell`).

---

### Task 1: Clinic store

**Files:**
- Create: `tools/discord/clinic_store.py`
- Modify: `tools/discord/netdoctor_store.py` (prune: 30 -> 180 days default, ceiling 1 GiB -> 3 GiB, prune
  touches only ND tables), `tools/discord/netdoctor_bot.py` (docstring + default `NETDOCTOR_DB_MAX_MB`
  3072)
- Test: `tests/test_clinic_store.py`

**Interfaces:**
- Produces: `class Clinic(db_path)` opening the same file with `PRAGMA journal_mode=WAL`,
  `busy_timeout=10000`, creating tables `nw_files, nw_stills, nw_messages, nw_bugs, nw_notes,
  nw_watermark, nw_seen, nw_exports` (columns as in the spec; `nw_exports(sha256 PK, export_id,
  confirmed REAL)`). Methods:
  - `put_file(data: bytes, filename: str, kind: str, mime: str = "") -> str` (sha256; logs/text/dmp/dcx/
    archives LZMA; images per `clinic_media.shrink_image` result passed in as `stored: bytes, codec: str`
    keyword args; idempotent)
  - `get_file(sha: str) -> tuple[str, bytes, str] | None` (filename, original-or-stored bytes, codec)
  - `verify_file(sha: str) -> bool` (LZMA rows: decompress and re-hash; others: row exists, blob non-empty)
  - `add_stills(video_sha: str, frames: list[tuple[float, bytes]]) -> int`
  - `upsert_message(rec: dict) -> None` (keys = `nw_messages` columns; `attachments` list of dicts)
  - `message(mid: int) -> dict | None`, `messages_for_bug(bug_id: str) -> list[dict]`
  - `set_state(mid: int, state: str, replacement_id: int | None = None) -> None`
  - `new_bug(title: str, source: str, channel_id: int, thread_id: int | None) -> str`
  - `bug(bug_id) -> dict | None`, `bugs(status=None, tag=None, q=None) -> list[dict]`
  - `update_bug(bug_id, **fields) -> None` (tags merged), `merge_bugs(keep: str, dup: str) -> None`
  - `add_note(bug_id, who, kind, text) -> None`, `notes(bug_id) -> list[dict]`
  - `bug_for_thread(thread_id) -> str | None`, `bug_for_files(shas: list[str]) -> str | None` (a bug
    holding all of them)
  - `watermark(channel_id) -> int`, `set_watermark(channel_id, mid) -> None`
  - `mark_seen(who) -> float` (returns previous time), `changes_since(t: float) -> dict`
  - `record_export(export_id: str, shas: list[str]) -> None`, `confirm_export(export_id, shas) -> int`,
    `exported(sha) -> bool`
  - `size_bytes() -> int`, `backup(dest: Path) -> bool` (sqlite3 backup API; False when free disk <
    2x size)

- [ ] **Step 1: Write failing tests** in `tests/test_clinic_store.py`:
  `test_put_file_dedups_and_verifies` (same bytes twice -> one row, `verify_file` True; corrupt the blob
  via SQL -> False), `test_nd_prune_never_touches_nw_tables` (old `nw_*` rows with created 400 days ago
  survive `netdoctor_store.Store.prune()`; ND rows 200 days old go, 100 days old stay),
  `test_prune_ceiling_3gib` (`prune(max_bytes=10*2**30)` clamps to `3*2**30`), `test_merge_bugs`
  (messages and notes move; dup status `duplicate`, `dup_of` = keep), `test_bug_for_files`,
  `test_export_confirm` (`exported` only after confirm with matching sha list),
  `test_changes_since` (bug created after `mark_seen` appears; earlier does not).
- [ ] **Step 2: Run** `python -m pytest tests/test_clinic_store.py -q` - expect failures (module missing).
- [ ] **Step 3: Implement** `clinic_store.py` and the prune changes. Keep `netdoctor_store` prune
  deleting only from `logs, sessions, analyses, logdata`.
- [ ] **Step 4: Run** `python -m pytest tests/test_clinic_store.py tests/test_netdoctor_bot.py -q` - PASS.
- [ ] **Step 5: Commit** `git add tools/discord/clinic_store.py tools/discord/netdoctor_store.py tools/discord/netdoctor_bot.py tests/test_clinic_store.py && git commit -m "clinic: archive store (nw tables, WAL), ND prune 180 days / 3 GiB ceiling"`

### Task 2: Clinic logic (pure)

**Files:**
- Create: `tools/discord/clinic_logic.py`
- Test: `tests/test_clinic_logic.py`

**Interfaces:**
- Consumes: `netdoctor_store.same_session`, `group_sessions` (peer matching), net_doctor `hide`
  semantics (reimplement the mask with the same regex; do not import net_doctor here).
- Produces:
  - `skipped_channel(name: str) -> bool`
  - `file_kind(filename: str) -> str` -> `log | dump | game | archive | image | video | other`
  - `removable(filename: str, inner_names: list[str] | None = None) -> bool`
  - `mask_ips(text: str) -> str` (public IPv4 and IPv4:port masked; private/VPN ranges kept like
    net_doctor; IPv6 global masked)
  - `cluster(msgs: list[dict], infos: dict[int, list[dict]]) -> list[list[int]]`: msgs = dicts with
    `id, author_id, created (epoch s), reply_to, has_log`; infos = message id -> log_info dicts.
    Rule: seed each log message; add same-author messages within +-1800 s, replies to cluster messages,
    and other messages whose logs `same_session` with a cluster log; merge overlaps. Returns message id
    lists, oldest first.
  - `missing_peers(infos: list[dict]) -> list[str]`: peers/host named in the logs with no log of their
    own in the list (names; callers never print them publicly, only count/roles).
  - `missing_roles(infos) -> list[str]`: `"host"` and/or `"guest"` descriptions for the reply.
  - `classify(text: str, verdict_keys: set[str]) -> str` -> `"network" | "bug"`: network when
    `verdict_keys` has any key starting `net.`/`nat.`/`join.`/`firewall.` (Task 8 maps net_doctor
    findings to such keys) or the text matches `\b(join|joining|connect|connection|summon|can't see|
    cant see|timeout|timed out|firewall|vpn|port|nat|hamachi|radmin|zerotier)\b` and not
    `\b(bug|crash|broken|glitch|stuck|softlock)\b`.

- [ ] **Step 1: Failing tests**: `test_skip_rule` (`staff`, `staff-archived`, `development` True;
  `moderator-only`, `general`, `dev-chat` False), `test_removable_kinds` (`nightwatch-x.log`,
  `notes.txt`, `crash.dmp`, `userdata0010`, `x.dcx` True; `a.png`, `b.mp4` False; `saves.zip` with
  inner `["last_session.log"]` True, with `["a.png"]` False, with `None` (unreadable) True),
  `test_mask_ips` (`198.51.100.7:9307` masked, `192.168.1.5` kept, `26.1.2.3` Radmin kept per
  net_doctor), `test_cluster_time_window_and_replies`, `test_cluster_joins_peer_logs_same_session`,
  `test_cluster_joins_existing_bug_on_same_logs` (this test lives here as a cluster + `bug_for_files`
  scenario with a fake store dict), `test_missing_peers`, `test_classify`.
- [ ] **Step 2: Run** `python -m pytest tests/test_clinic_logic.py -q` - FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** - PASS.
- [ ] **Step 5: Commit** `clinic: grouping, file rules, IP mask, network-vs-bug`.

### Task 3: Media handling

**Files:**
- Create: `tools/discord/clinic_media.py`
- Modify: `tools/discord/requirements.txt` (add `Pillow==11.0.0`)
- Test: `tests/test_clinic_media.py`

**Interfaces:**
- Produces:
  - `shrink_image(data: bytes) -> tuple[bytes, str]` (stored bytes, codec `"orig"|"webp85"|"jpeg85"`);
    unchanged when <= 2 MB or Pillow fails to open.
  - `video_stills(path: Path, every_s=4.0, max_frames=48, width=1280) -> list[tuple[float, bytes]]`
    via `ffprobe` for duration then one `ffmpeg -ss t -i path -frames:v 1 -vf scale=1280:-2` per frame
    to PNG in a temp dir, re-encoded with Pillow (WebP q60 / JPEG q70). Runs `nice -n 10` on POSIX.
  - `archive_listing(path: Path) -> list[str] | None` (zip via zipfile; 7z/rar -> None)
  - `archive_inner_logs(path: Path) -> list[tuple[str, bytes]]` (zip members that are removable kinds)
  - `free_disk(path: Path) -> int`
- [ ] **Step 1: Failing tests**: `test_shrink_small_image_untouched`, `test_shrink_big_image_webp`
  (3 MB noise PNG made with Pillow -> smaller, codec webp85 or jpeg85), `test_video_stills_count`
  (skip unless `ffmpeg` on PATH: generate a 10 s `testsrc` video -> 3 frames at 0,4,8 s; a 400 s video ->
  48 frames), `test_zip_listing_and_inner_logs`.
- [ ] **Step 2: Run** `python -m pytest tests/test_clinic_media.py -q` - FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** - PASS (ffmpeg test may skip on the PC; it must pass on quad.fi in Task 9).
- [ ] **Step 5: Commit** `clinic: image shrink, video stills, zip scan`.

### Task 4: Iosefka's voice

**Files:**
- Create: `tools/discord/iosefka_voice.py`
- Test: `tests/test_clinic_voice.py`

**Interfaces:**
- Produces (all return `str` <= 2000 chars, never containing an IP or a player name other than the
  passed mention string):
  - `replacement(mention: str, bug_id: str, text: str, media_note: str = "") -> str`: header line + the
    author's masked text quoted (`> ` lines, truncated to fit) + `media_note`.
  - `bug_filed(bug_id: str, missing: list[str]) -> str` (thanks; when `missing` has roles, asks for
    "your host's log" / "the other guests' logs" of that session)
  - `network_help(diagnosis: str) -> str` (diagnosis + "Tell me what you tried, and I'll do what I can.")
  - `dm_ack(bug_id: str) -> str`, `told(text: str) -> str` (dev `--tell` wrapper),
  - `announcement() -> str` (the `#general`/`#help` post; shown to visy before posting).
- Tone: the real Iosefka (spec "Iosefka's voice"); lines may borrow "Now, go. And good hunting.", "This
  night is long, but morning always comes.", "I pray for your safety." Exact copy for the replacement
  header:
  `"{mention} brought a log to my clinic. I've taken it inside, where patients aren't exposed to infection: names and addresses stay off the open streets. It's being triaged as {bug_id}. Thank you, truly. Now, go. And good hunting."`
- [ ] **Step 1: Failing tests**: `test_replacement_masks_and_fits` (text with `203.0.113.9` -> masked;
  10 000-char text -> result <= 2000 and ends with the media note), `test_bug_filed_asks_for_missing`
  (`["host"]` -> contains "host's log"; `[]` -> no request), `test_announcement_mentions_duties`
  (contains "triage", "logs", and an invitation to mention her).
- [ ] **Step 2-4:** run FAIL, implement, run PASS.
- [ ] **Step 5: Commit** `clinic: Iosefka's reply texts`.

### Task 5: Discord operations

**Files:**
- Create: `tools/discord/clinic_discord.py`
- Test: `tests/test_clinic_discord.py` (fake channel/thread/message classes; no network)

**Interfaces:**
- Consumes: Tasks 1-4.
- Produces (async, discord.py objects in, plain results out):
  - `async def walk_places(guild) -> AsyncIterator[channel_or_thread]`: every text/news channel and
    forum not skipped; for each, `channel.threads` (active) + `archived_threads(private=False)` +
    `archived_threads(private=True)` (paged by discord.py, `limit=None`); forum posts are threads.
  - `all_attachments(message) -> list` (attachments + `message_snapshots[*].attachments`; move the
    existing helper here and import it from the bot).
  - `async def archive_message(clinic, message, *, log_info_fn, tmpdir) -> dict`: downloads every
    attachment (retry 3x; size mismatch = failure), stores files (logs via `put_file`, images via
    `shrink_image`, videos -> `video_stills` then the temp file deleted, zips + inner logs), upserts the
    message with state `verified` when every file verifies, else `archive_failed`; returns
    `{"removable": bool, "infos": [log_info...], "state": str}`.
  - `async def assign_bug(clinic, message, infos) -> str`: thread/forum post -> `bug_for_thread` or a new
    bug titled by the thread name; channel -> `cluster` over the channel's recent stored messages ->
    `bug_for_files` or new bug titled by the first line of the poster's text (60 chars, IPs masked);
    DM -> per conversation per UTC day.
  - `async def replace_and_delete(clinic, message, bug_id, *, ping: bool, delete: bool,
    need_export: bool) -> str`: preconditions per Global Constraints (else returns `"blocked:<why>"`);
    unarchive/unlock the thread when needed (remember and restore); step A post replacement
    (`iosefka_voice.replacement`, re-upload the message's images/videos when each <= the guild's
    `filesize_limit`, else `media_note="(pictures/video kept in the clinic)"`) and
    `set_state(mid, "replaced", replacement_id)`; step B delete original (404 = already gone = ok) and
    `set_state(mid, "deleted")`. A message already `replaced` skips step A. `delete=False` stops after
    nothing (returns `"would-replace"`). Returns final state.
- [ ] **Step 1: Failing tests**: `test_walk_skips_staff_and_development` (incl. archived private
  threads under a skipped parent), `test_snapshot_attachments_counted`,
  `test_failed_download_blocks_delete` (download raises -> state `archive_failed`;
  `replace_and_delete` returns `blocked:not-verified`, fake message not deleted),
  `test_replace_posts_before_delete` (fake records call order: send then delete),
  `test_replace_reopens_archived_thread` (archived+locked restored after),
  `test_no_ping_in_cull` (sent `allowed_mentions` has `users=False`), `test_need_export_blocks`
  (`exported` False -> `blocked:not-exported`).
- [ ] **Step 2-4:** run FAIL, implement, run PASS.
- [ ] **Step 5: Commit** `clinic: Discord walk, archive, replace-then-delete`.

### Task 6: Cull runner

**Files:**
- Create: `tools/discord/iosefka_cull.py`
- Test: `tests/test_clinic_cull.py`

**Interfaces:**
- Consumes: Task 5; reads the env file like `netdoctor_query.load_env`.
- Produces CLI `iosefka_cull.py <phase>`:
  - `harvest`: logs in a discord.Client (no gateway handlers besides on_ready), walks every place, every
    message (`history(limit=None, oldest_first=True)`), `archive_message` for messages with files, stores
    text-only messages of threads/posts that have a bug and of channel clusters, assigns bugs, sets
    watermarks; prints progress lines `place <name>: n msgs, n files` and a final summary. Idempotent.
  - `verify`: re-verifies every file; prints counts; exit 1 on any failure.
  - `report --out PATH`: dry run: per removable message: bug id, channel/thread name, file names,
    replacement preview (no names except a `<@id>` placeholder). Plain text file.
  - `starter-test --thread ID`: deletes the starter message of a test post (visy's), then checks the
    thread still exists and replies are intact; prints `starter-delete keeps thread: yes|no`.
  - `cull [--limit N]`: requires `NW_DELETE=1`; `replace_and_delete(ping=False, delete=True,
    need_export=True)` for every removable `verified` message, oldest first, ~1/s; prints each result;
    stops on the first unexpected exception.
- [ ] **Step 1: Failing tests** (fake client/store): `test_cull_requires_flag`,
  `test_cull_resumes_each_step` (simulate crash after replacement, rerun -> one replacement, one delete;
  crash after delete before record -> 404 treated as done), `test_report_has_no_names`.
- [ ] **Step 2-4:** run FAIL, implement, run PASS.
- [ ] **Step 5: Commit** `clinic: cull runner (harvest, verify, report, starter-test, cull)`.

### Task 7: Dev CLI

**Files:**
- Create: `tools/discord/iosefka_cli.py`, `tools/discord/CLINIC.md`
- Test: `tests/test_clinic_cli.py`

**Interfaces:**
- Consumes: Task 1 (no discord.py import at module level; `--tell` queues instead, see below).
- Produces: `main(argv: list[str], who: str, out=sys.stdout.buffer) -> int`; entry reads `--as <who>`
  from argv and the command from `SSH_ORIGINAL_COMMAND` (shlex) when set, else from argv. Commands per
  spec: `new, list, show, files, note, status, tag, assign, share, merge, nd, export, confirm-export,
  import`. JSON output (`--text` for humans). `files NW-x` and `export` write a tar to stdout
  (`export` adds `manifest.json` = `{export_id, files: [{sha256, filename, kind}]}`); `confirm-export`
  reads that manifest JSON (with the PC's re-hashes) from stdin. `status --tell "text"` stores a row in a
  new table `nw_outbox(id, bug_id, text, who, created, sent)`; the bot posts outbox rows (Task 8).
  `export`, `confirm-export`, `import` require `who == "visy"`. `import` reads a tar on stdin with
  `case.json` (`title, tags, notes, files`) per folder and creates `source="local"` bugs.
- `CLINIC.md`: the SSH line format for `authorized_keys`
  (`command="/home/visy/netdoctor-venv/bin/python /home/visy/bbport/tools/discord/iosefka_cli.py --as vlad",restrict ssh-ed25519 AAAA... vlad`),
  every command with an example, the workflow (new -> show -> files -> note/tag/status -> share), and the
  PII rules (no names/IPs/log excerpts in public places, GitHub included).
- [ ] **Step 1: Failing tests**: `test_forced_command_rejects_shell` (`SSH_ORIGINAL_COMMAND="list; rm -rf ~"`
  -> exit 2, no side effect; `files ../../etc` -> error), `test_show_json_roundtrip`,
  `test_export_visy_only`, `test_tell_goes_to_outbox_masked` (IP masked in stored text),
  `test_new_since_last_check`.
- [ ] **Step 2-4:** run FAIL, implement, run PASS.
- [ ] **Step 5: Commit** `clinic: iosefka SSH bug tool + CLINIC.md`.

### Task 8: Live duty in the bot

**Files:**
- Modify: `tools/discord/netdoctor_bot.py`, `tools/discord/README.md`
- Test: `tests/test_netdoctor_bot.py` (new tests), `tests/test_clinic_live.py`

**Interfaces:**
- Consumes: Tasks 1-5, 7 (`nw_outbox`).
- Produces in the bot:
  - `verdict_keys(diag_text: str) -> set[str]` (pure; maps net_doctor's connection findings to `net.*`
    keys by their headline phrases; unit-tested on sample diagnoses).
  - `async def handle_log_message(message, *, live: bool)`: archive -> bug -> classify -> reply
    (`network_help` with the redacted diagnosis, or `bug_filed` with missing roles) -> 
    `replace_and_delete(ping=live, delete=NW_DELETE, need_export=False)`. DMs: archive + `dm_ack`, never
    delete. Non-log messages in a thread that has a bug: stored (videos -> stills), not deleted.
  - Events: `on_message` for every guild place not skipped (replaces the "small place" rule for logs;
    mentions keep working as today), `on_raw_message_edit` (fetch the message, handle if it now has an
    unarchived removable file), DMs.
  - Tasks: `catch_up()` at ready and hourly (watermarks + threads archived since last sweep),
    `send_outbox()` every 60 s, nightly `backup()` at 04:00 local, disk/size guard every hour (warning
    once per day in the first `staff*` channel).
  - Live is gated: `NW_LIVE=1` env enables handle_log_message; default off until the cull is done.
- [ ] **Step 1: Failing tests**: `test_verdict_keys_on_samples`, `test_raw_edit_adds_log` (fake raw
  event -> handled once), `test_dm_never_deleted`, `test_live_off_by_default`,
  `test_outbox_sent_once`.
- [ ] **Step 2-4:** run FAIL, implement, run PASS; full `python -m pytest tests/test_clinic_*.py tests/test_netdoctor_bot.py tests/test_net_doctor.py -q` PASS.
- [ ] **Step 5: Commit** `clinic: live duty (logs anywhere, edits, catch-up, outbox, backup, guards)`.

### Task 9: Deploy and verify on quad.fi (orchestrator, not a subagent)

- [ ] Push to the fork (`git push fork master`); on quad.fi `git -C ~/bbport pull`, `pip install -r
  requirements.txt` in the venv, `python -m pytest` the clinic tests there (ffmpeg test must PASS).
- [ ] Restart the bot with `NW_LIVE=0 NW_DELETE=0`; confirm it starts and existing ND features work.
- [ ] `iosefka_cull.py harvest`, then `verify` (exit 0). Counts compared with the recon count (188 logs,
  243 images, 40 videos, 4 zips).
- [ ] `iosefka_cli.py export` streamed to `E:\games\bbport\player_logs\nw_clinic\`; re-hash on the PC;
  `confirm-export`.
- [ ] Triage (spec "First triage") with PLAN_077 + player_logs; write back; import local cases.
- [ ] `report` -> visy reviews. visy makes a test post -> `starter-test`.
- [ ] After visy's go: `NW_DELETE=1 iosefka_cull.py cull`; then `verify` + a recount of logs left on the
  server (recon script) = 0.
- [ ] `NW_LIVE=1 NW_DELETE=1` in the env file, restart; a test log by visy in `#help` -> archived,
  replaced, visible in `iosefka new`.
- [ ] Announcement text to visy; post in `#general` and `#help` after approval.
- [ ] vlad's key line added when visy sends his public key.
