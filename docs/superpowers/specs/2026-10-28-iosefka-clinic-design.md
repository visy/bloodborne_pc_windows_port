# Iosefka's Clinic: Nightwatch server log archive, PII cull and bug triage (design)

Date: 2026-10-28. Owner: visy. Builds on the existing Net Doctor Iosefka bot (`tools/discord/`).

## Why

The Nightwatch Co-op Discord holds hundreds of party/Nightwatch logs posted by players. Logs carry
player names, Steam/PSN names and IP addresses: personally identifying data, public on the server.
At the same time those logs, with the text, screenshots and videos around them, are the best bug
reports we have, and they are scattered over channels, forum posts and threads.

Goal: Iosefka takes every log (and its context) off the server into a private archive on quad.fi,
leaves a kind note where it was, and from then on does the same for every new log in real time. The
archive becomes the bug tracker that visy and vladtepes (Nightwatch dev) drive through their own Claude
instances over SSH. Reporting a bug stays painless for players and safe for them.

## Goals

1. Archive, never lose: every log file ever posted in a public channel/thread/forum post is stored on
   quad.fi, byte-exact (hash verified), deduplicated by content, before anything is deleted.
2. Context per bug: forum post / thread title, the reporter's text, follow-up posts, screenshots,
   video stills, peer logs of the same session, all under one id `NW-xxxxxxx` (7 hex).
3. Big cull: one pass over the whole server (every channel, every thread active or archived, public
   or private, every forum post) except `#staff*` and `#development`; each message holding a log is
   replaced by a triage note and removed.
4. Live duty: any new log anywhere (messages, edits that add files, forwarded messages, DMs) is
   archived and removed within seconds, with a friendly reply; nothing is missed while the bot is down
   (catch-up scan at start + hourly sweep).
5. Help first: when a post is a connection problem, Iosefka answers with net_doctor's advice and asks
   follow-up questions; when it is a bug, she files it, thanks the player and asks for missing peer
   logs of the same session. Any log she touches is removed from the server either way.
6. Dev workflow: an `iosefka` command over SSH (JSON output) to list new bugs, read a bug with all its
   context, fetch its files, add notes, tag, set status, close, assign/share between visy and vlad.
7. First triage: after the cull, all bugs are pulled to the dev PC, titled and tagged (game area,
   feature, component: co-op mod / NW launcher / NW port (Lantern renderer) / game data / unknown),
   linked with the open 0.7.7 list (`agents/PLAN_077.md`) and the local `player_logs/` cases.
8. Announcement: after the cull, a post in `#general` and `#help` explaining her triage duty and
   inviting players to talk to her directly.

## Non-goals

- No public bug tracker or web UI. No GitHub issues from player data.
- No moderation beyond logs (screenshots/videos alone stay on the server).
- No automatic code fixes; triage tags are best guesses for humans.

## Anti-goals (failure even if it "works")

- A log deleted from Discord that is not in the archive (or is corrupt there).
- Player names/IPs leaking into the repo, commits, release notes, public replies or the
  announcement.
- Pinging hundreds of old posters during the cull.
- Iosefka sounding like a moderation bot or like the Imposter. She is the real Iosefka: kind, careful
  about "infection" (here: personal data), "Now, go. And good hunting."
- The VPS filling its disk (5.2 GB free) and taking the bot down.

## Constraints (measured 2026-10-28)

- quad.fi: Ubuntu 16.04, 1 CPU, 939 MB RAM, 5.2 GB free disk, system python 3.5; bot venv Python 3.12
  with discord.py; ffmpeg 2.8 installed. User `visy` runs the bot (cron keepalive / systemd user).
- The bot is in one guild (Nightwatch Co-op) with Administrator (covers Manage Messages, private
  threads, unarchiving).
- Discord: a bot cannot remove one attachment of another user's message, only the whole message.
  Bot uploads are capped by the server's boost level (25 MB base).
- Server shape: text channels, news channels, forums `#bug-reports` (~80 posts), `#suggestions`,
  `#pre-release-bug-reports`, threads in `#help`. Skip: names starting with `staff`, and `development`
  (user rule), and `#moderator-only` (visy, later).
- Volume (read-only count 2026-10-28, skip rule applied): 5418 messages; 188 log/txt files (357 MB raw,
  ~25 MB LZMA), 243 images (282 MB), 40 videos (857 MB, stills only), 4 zips, 1 crash dump (.dmp),
  1 game file (.dcx). Logs by place: #bug-reports 135 (79 posts), #help 42, #general 9, #co-op 2.
  Expected archive size 300-400 MB.
- Files removed from the server (they can carry names/IPs/memory): .log/.txt, crash dumps (.dmp), zips
  holding any of those, save/game files (.dcx, userdata). Everything else (images, videos) stays unless
  it shares a message with a removed file (then it is reposted by Iosefka).

## Decisions made by visy

- Removal: delete the whole message holding a log; Iosefka reposts the author's text (IP addresses
  masked) and the message's screenshots/videos in her replacement note, so threads still read. Logs live
  only in the archive.
- Server owner approved the deletions (bot already admin).
- SSH: per-person access, locked to the bug tool (no shell).
- Database cap raised to 3 GB; prune limit raised (the archive is not pruned by age).
- Videos: sampled into compressed stills, the video file deleted from disk afterwards.
- Personality: the real Iosefka (clinic doctor) from the Bloodborne wiki text.
- Triage uses the open 0.7.7 list and the stored player logs on the dev PC, moving cases into the clinic
  where it makes sense.

## Design

### Storage (`netdoctor.db`, one SQLite file, WAL mode)

Existing tables stay (ND analyses cache). Prune: ND cache rows by age (30 -> 180 days); size ceiling
raised from 1 GiB to 3 GiB for the whole file. NW tables are never pruned automatically; at 2.4 GiB
Iosefka logs a warning and posts once in `#staff` that the archive needs attention, and stops storing
video stills (logs and images are always stored).

New tables:

- `nw_files(sha256 PK, kind, filename, mime, raw_size, codec, blob, created)`: kind = log | image |
  still | archive | other. Logs/text: LZMA, verified by decompress + hash before the row counts. Images:
  stored as-is up to 2 MB, larger ones re-encoded to WebP q85 max 2560 px (original hash kept as the
  key, `codec` records the transform). Zip/7z with logs inside: stored as-is plus each inner log as its
  own row (dedup across zips and loose files).
- `nw_stills(video_sha256, n, t_s, file_sha256)`: video stills. Sampling: one frame every 4 s,
  evenly spread to at most 48 frames, 1280 px wide, WebP q60 (JPEG q70 when WebP is unavailable).
  The video is downloaded to a temp folder (one at a time, size checked against free disk first),
  sampled with ffmpeg, then deleted.
- `nw_messages(message_id PK, channel_id, thread_id, author_id, author_name, created, edited, content,
  reply_to, attachments JSON[{sha256, filename, kind, size}], bug_id, state, replacement_id, seen)`:
  every message of every thread/post that has a bug, plus every message holding a file anywhere.
  state = archived | verified | replaced | deleted | kept.
- `nw_bugs(id PK 'NW-xxxxxxx', title, source (forum post / thread / channel cluster / dm / local),
  channel_id, thread_id, status (new, open, analysing, waiting-logs, fixed, closed, duplicate),
  dup_of, tags JSON {area, feature, component, version, role}, assignee, nd_refs, created, updated)`.
- `nw_notes(bug_id, at, who, kind (note, status, tag, share, triage), text)`: the bug's timeline.
- `nw_watermark(channel_id PK, last_message_id)`: catch-up after downtime.
- `nw_seen(who, bug_id, at)`: "new since my last check" per dev.

### Grouping messages into bugs

- Forum post or thread: one bug; title = post/thread name; every message in it belongs to the bug.
- Plain channel (`#help`, `#general`, `#co-op`, ...): a cluster around each log message: the poster's
  messages 30 min before/after, replies to them, and other authors' logs that net_doctor's
  `same_session` matches (peer logs). Overlapping clusters merge.
- Dedup: a cluster whose logs all already belong to bug X joins X (same log reposted elsewhere).
- DMs: one bug per DM conversation per day.

### Cull (`iosefka_cull.py`, run by hand on quad.fi, resumable, all state in the DB)

1. Harvest (read-only): walk every non-skipped channel; per channel its active threads and archived
   public + private threads (paged); every message (with forwarded snapshots). Store messages, download
   and store every attachment (logs, images, archives; videos -> stills). Idempotent; re-runs skip done
   work.
2. Verify: every log row decompresses to its hash; report counts (messages, logs, images, videos,
   bugs) and any failures. A failure blocks the message from deletion.
3. Pull: the dev PC downloads the full archive (`iosefka export` -> tar stream over SSH) into
   `E:\games\bbport\player_logs\nw_clinic\` and checks the manifest. This is the second copy; the cull
   deletes only messages whose files are verified on quad.fi AND listed in the PC manifest.
4. Dry run: a report of every message to be removed (channel, thread, file names, replacement text
   preview, no deletions). visy reviews it.
5. Cull: per message, oldest thread first: post the replacement note (text with IPs masked, re-upload
   its images/videos when they fit the upload cap, else a link-free note that media is in the clinic),
   then delete the original, then record. Mentions render the author but never ping
   (`allowed_mentions=none`). Archived/locked threads are opened for the note and archived/locked again.
   ~1 action/s, Discord rate limits respected; crash-safe (each step recorded).

### Live duty (in `netdoctor_bot.py`)

- Triggers: `on_message` and `on_message_edit` in every non-skipped guild channel/thread, DMs, and
  forwarded messages. Not only "small places": the auto rule now covers the whole server for logs.
- Any log: archive + verify, then classify:
  - connection question (net_doctor finds a connection-type problem, or the text is about joining /
    connecting): reply with the redacted diagnosis and ask what they tried; files a bug only if it turns
    out to be one (mention or follow-up) - the logs are still archived under a bug in state `help`.
  - bug: create/join the bug, reply with the NW id, thanks, and a request for missing peer logs when
    the log names a host/guests whose logs are not in the bug yet (no names in the reply: "your host's
    log of that session").
  - in both cases the message is replaced and removed (same steps as the cull, single message; the PC
    copy rule does not apply live, quad.fi verify does).
- Non-log follow-ups (text, pictures, video) inside a bug thread are added to the bug (videos as
  stills); the messages stay on the server.
- Mention Iosefka anywhere: she answers in character (existing analyse command, help, status of the
  reporter's bugs by NW id).
- Catch-up: at start and hourly, scan each channel/thread from its watermark; anything missed is handled
  as live (without ping-heavy replies: one note per bug).
- Disk guard: free disk < 1 GB -> stills off, warning in `#staff`.

### Dev access (`iosefka_cli.py` over SSH)

- One SSH key per person in `~visy/.ssh/authorized_keys` with
  `command="<venv python> <bbport>/tools/discord/iosefka_cli.py --as <who>",restrict` (no shell, no
  forwarding). The CLI parses `SSH_ORIGINAL_COMMAND` with shlex, never through a shell. Password logins
  stay off (keys only). (Builder's call: no new Unix users needed, no root; identity comes from `--as`.)
- Commands (JSON by default, `--text` for humans):
  `new` (bugs/notes since my last check), `list [--status s] [--tag k=v] [--q text]`,
  `show NW-x` (title, status, tags, timeline, every message text with author, file list),
  `files NW-x` (tar of logs + images + stills to stdout), `note NW-x "text"`,
  `status NW-x <status> [--dup NW-y] [--tell "player-facing text"]` (`--tell`: Iosefka posts it in the
  bug's thread), `tag NW-x k=v ...`, `assign NW-x <who>`, `share NW-x <who> "text"` (appears in that
  person's `new`), `nd ND-x` (old analyses), `export` (whole archive tar + manifest, for visy only),
  `import` (local cases from the dev PC: JSON + files on stdin, see triage).
- A short guide `tools/discord/CLINIC.md` for both devs' Claude instances: the commands, the workflow,
  the PII rules.

### First triage (dev PC, Claude + subagents)

1. `export` the archive to `player_logs/nw_clinic/`.
2. For each bug: read text + logs (net_doctor, log signatures) -> title, tags (area: map/boss; feature:
   lamps, joins, chalice, endings, ...; component: co-op mod / NW launcher / NW port / game data /
   network / unknown), duplicate links, status.
3. Match against `agents/PLAN_077.md` open items and `player_logs/<date>/<case>` folders: a match adds
   the PLAN item name as a tag and links; a local case with no server bug is imported into the clinic
   (source `local`), so the clinic is the one place for player reports. PLAN_077 rows get the NW id.
4. Write back via `tag`/`note`/`status` in one batch. Summary in chat.

### Iosefka's voice (reply templates, one file `iosefka_voice.py`)

Short, warm, clinic metaphors, never threatening. Examples:
- replacement: "@player brought a log to my clinic. I've taken it inside, where patients aren't exposed
  to infection: names and addresses stay off the open streets. It's being triaged as NW-1a2b3c4. Thank
  you, truly. Now, go. And good hunting."
- missing peer logs: "To treat this properly I need the other half of that night: could your host
  (and anyone else in that session) bring me their log too? I'll keep it safe."
- network help: diagnosis + "Tell me what you tried, and I'll do what I can."
- announcement (`#general`, `#help`): draft shown to visy before posting.

## Testing

- Unit (no Discord): grouping/clustering, dedup, IP masking, archive verify, CLI parsing/forced command
  safety, prune rules (NW tables never pruned), stills sampler on a sample video.
- Fake-Discord harness for cull steps (post-then-delete order, resume after crash at each step, archived
  thread handling, mention suppression).
- Live check on the server: a test log posted by visy in `#help` -> archived, replaced, NW id, CLI shows
  it; edit-adds-file and forwarded-message cases; bot restart catch-up.

## Rollout

1. Code + tests, deployed with deletion disabled (`NW_DELETE=0`): harvest + verify on the live server.
2. Export to PC, triage, dry-run report -> visy approves.
3. `NW_DELETE=1`, cull. Then live duty on, announcement posted (after visy reads the text).
4. vlad's SSH key added; CLINIC.md shared.

## Resolved after the builder check

Answered by visy:
- Images/videos alone stay on the server even if they show names (visy picked "repost rest", not
  "also cull images").

Builder's calls (visy can override):
- Forum/thread starter holding a log: deleting a starter message leaves the post and its replies
  ("original message was deleted"); the thread itself is never deleted. The first real step of the cull
  verifies this on a test post in `#pre-release-bug-reports` made by visy; if Discord deletes the post,
  the cull stops and starter posts go back to visy.
- Replacement notes name the author with a silent mention (the name was already public there; no ping).
- PC copy: `export` writes a sha256 manifest; the PC side (Claude here) re-hashes every file and returns
  the manifest via `iosefka confirm-export`; the cull deletes only messages whose every file is in a
  confirmed export. Live duty after the cull: quad.fi verify + the nightly backup below.
- Backup: nightly `sqlite3 .backup` to `netdoctor.db.bak` (one copy, ~0.4 GB; skipped if free disk
  < 2x DB size), plus an `export` pull to the PC after each triage session.
- Connection vs bug: no LLM. net_doctor's verdict (it already reports connection problems) plus
  keywords in the text (join, connect, summon, can't see, timeout, firewall, VPN, port). Wrong guess
  only changes the reply; the log is archived and removed either way. A player can always reply
  "it's a bug" to turn it into one.
- Peer logs: net_doctor `log_info` already gives name, role, host, peers and times per log; a peer
  named in the log without a log in the bug = missing.
- DMs: bots cannot delete a user's DM messages, and DMs are not public. DMs are archived (bug per
  conversation per day) and answered, never deleted.
- `--tell`: both devs may use it; the text goes through the IP mask and is logged in `nw_notes`.
  `export` / `confirm-export` / `import` only for `--as visy` (per-key label, enforced in the CLI).
- Live events: gateway `on_message` + `on_raw_message_edit` (uncached edits too). Hourly sweep: every
  channel and active thread from its watermark, plus threads archived since the last sweep; full
  re-walks only on demand (`iosefka_cull.py harvest`).
- Concurrency: the harvest runs as its own process while the bot keeps running (bot in archive-only
  mode until the cull ends); SQLite WAL + busy timeout; one ffmpeg at a time, nice 10. Pillow is added to
  the venv; WebP when Pillow has it, else JPEG.
- Bug merges: `merge NW-a NW-b` re-points messages and notes, NW-b becomes `duplicate` of NW-a.

- `#moderator-only` is skipped too (visy, after the permission change): skipped = `#staff*`, `#development`, `#moderator-only`.

## Open questions

None.
