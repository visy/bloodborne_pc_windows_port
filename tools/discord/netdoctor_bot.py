#!/usr/bin/env python3
"""Discord bot for the co-op connection doctor (scripts/net_doctor.py).

  /netdoctor log1 [log2 .. log5] [private]   logs attached to the command
  /netdoctor-recent [minutes] [private]      every log file posted in this channel lately, by anyone
  message menu "Diagnose logs"               that message's logs + logs posted 30 min around it
  DM: send log files to the bot in a direct message; it answers privately (with names and IPs)
  auto (NETDOCTOR_AUTO): react (default) puts a stethoscope on messages with log files; reply also
  posts a redacted diagnosis of the channel's last 30 min of logs, 90 s after the last log arrived

The bot saves logs in a temporary folder, runs net_doctor, deletes the files and replies. In a channel the reply hides player names and public
IPs (--redactnames --redactips); with private:True only the caller sees the reply, unredacted.
Nothing is stored. Setup and running as a service: tools/discord/README.md.

Environment:
  NETDOCTOR_TOKEN      the bot token (required; never commit it)
  NETDOCTOR_SCRIPT     path to net_doctor.py (default: ../../scripts/net_doctor.py from this file)
  NETDOCTOR_GUILD      optional server id: the server whose channels get the auto reaction (commands
                       are global so they also work in DMs; a new global command can take a while to
                       appear)
  NETDOCTOR_MAX_MB     largest attachment accepted, MiB (default 40)
  NETDOCTOR_AUTO       off | react | reply (default react)
  NETDOCTOR_CHANNELS   ids of ordinary channels where mentions/reactions work too (default: none - only
                       threads and DMs; slash commands work everywhere)
  NETDOCTOR_DB         memory file (default ~/.config/netdoctor/netdoctor.db; analysis rows older than 180 days
                       are dropped hourly)
  NETDOCTOR_DB_MAX_MB  size cap of that file, MiB (default 3072, never above 3072): oldest analysis rows go first;
                       the clinic's nw_* tables are never pruned
  NW_LIVE              1 = Iosefka's live duty: every log posted anywhere (except staff*, dev*,
                       moderator-only) is archived in the clinic and replaced by her note; also edits, forwards,
                       DMs (archived, never deleted; answered only with NW_DELETE=1), hourly catch-up, outbox, nightly backup.
                       Default off: the bot then behaves exactly as before (no archive, no new replies).
  NW_DELETE            1 = live duty may delete the original message (after replacement + verified archive).
                       Without it a live bot archives silently and posts nothing (DMs included). See CLINIC.md.
  NW_TMP               temp folder for downloads (default: <system temp>/iosefka)
Needs the privileged Message Content intent (Developer Portal -> Bot) to read ordinary messages.
"""
import asyncio
import fnmatch
import importlib.util
import io
import json
import os
import sys
import tempfile
import time
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))
import clinic_asks as asks  # noqa: E402
import clinic_discord as cd  # noqa: E402
import clinic_health as hl  # noqa: E402
import clinic_logic  # noqa: E402
import clinic_store  # noqa: E402
import iosefka_voice as voice  # noqa: E402
SCRIPT = Path(os.environ.get("NETDOCTOR_SCRIPT", HERE.parents[1] / "scripts" / "net_doctor.py"))
MAX_BYTES = int(float(os.environ.get("NETDOCTOR_MAX_MB", "40")) * 1024 * 1024)
DISCORD_LIMIT = 2000
MAX_LOGS = 8          # logs per diagnosis (one session: host + up to 3 guests, plus earlier halves)
AUTO_DEBOUNCE_S = 90  # reply mode: wait this long after the last posted log
DM_DEBOUNCE_S = 15    # a DM to the bot: wait for the rest of the files, then answer

# File names players attach: bbport logs\last_session.log / previous_session.log / session-*.log,
# Nightwatch nightwatch-*.log, and Discord's own name for pasted text (message.txt, message(3).txt).
LOG_PATTERNS = ("*.log", "message*.txt", "*session*.txt", "nightwatch*.txt")


def is_log_name(name):
    n = Path(name or "").name.lower()
    return any(fnmatch.fnmatch(n, p) for p in LOG_PATTERNS)


def load_doctor():
    """net_doctor as a module, loaded fresh each call so a git pull takes effect without a restart."""
    spec = importlib.util.spec_from_file_location("net_doctor", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def diagnose(paths, redact):
    """Run net_doctor on local files; returns its text."""
    nd = load_doctor()
    args = (["--redactnames", "--redactips"] if redact else []) + [str(p) for p in paths]
    buf = io.StringIO()
    with redirect_stdout(buf):
        try:
            nd.main(args)
        except SystemExit:
            pass
    return buf.getvalue().strip() or "No output from the doctor."


def split_message(text, limit=DISCORD_LIMIT):
    """Discord-sized chunks, split at line ends; a code block is never left open across a cut."""
    chunks, cur = [], ""
    for line in text.splitlines(keepends=True):
        while len(line) > limit:
            chunks.append(line[:limit])
            line = line[limit:]
        if len(cur) + len(line) > limit:
            chunks.append(cur)
            cur = ""
        cur += line
    if cur:
        chunks.append(cur)
    fixed, carry = [], False
    for c in chunks:
        if carry:
            c = "```\n" + c
        carry = c.count("```") % 2 == 1
        fixed.append(c + ("\n```" if carry else ""))
    return fixed


SMALL_TALK = ("thanks", "thank", "you", "thx", "ty", "tysm", "hi", "hello", "hey", "nice", "great", "good",
              "bot", "<3", ":)", "ok", "okay")
DB_PATH = Path(os.environ.get("NETDOCTOR_DB", Path.home() / ".config" / "netdoctor" / "netdoctor.db"))


all_attachments = cd.all_attachments  # lives in clinic_discord now (the cull uses it too)


def wants_analysis(content, bot_id):
    """Any message that mentions the bot asks for a (re)analysis, unless it is only thanks / a greeting."""
    text = content.replace(f"<@{bot_id}>", " ").replace(f"<@!{bot_id}>", " ").lower()
    words = [w.strip(".,!?:;()") for w in text.split()]
    words = [w for w in words if w]
    return not words or not all(w in SMALL_TALK for w in words)


FORCE_WORDS = ("rerun", "redo", "again", "update", "refresh", "force", "reanalyse", "reanalyze")


def wants_rerun(content):
    """'@bot rerun' (again, update, refresh, ...): analyse again even with no new logs (new doctor rules)."""
    words = [w.strip(".,!?:;()").lower() for w in content.split()]
    return any(w in FORCE_WORDS for w in words)


def log_info(nd, path):
    """Who and when of one log (for grouping logs into sessions)."""
    import re
    lg = nd.parse(path)
    host = lg.name if lg.role == "host" else None
    if host is None:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.search(r"roster: ([^(\s]+)\(slot 0,", line)
                if m:
                    host = m.group(1)
                    break
    peers = set(lg.guests) | set(lg.pings) | ({host} if host else set())
    peers.discard(lg.name)
    return {"name": lg.name, "role": lg.role, "host": host,
            "start": nd._minutes(*lg.started) if lg.started else None,
            "end": getattr(lg, "ended", None), "day": lg.started[0] if lg.started else "", "peers": peers}


# --- Iosefka's live duty (NW_LIVE) -------------------------------------------------------------------------

_VERDICTS = (  # headline phrase in net_doctor's "What's wrong" list -> key (net.* nat.* join.* = a network problem)
    ("Not network", "other.notnet"), ("different sessions", "other.sessions"),
    ("Different game files", "other.files"), ("Different versions", "other.versions"),
    ("refused by the host", "join.refused"), ("phone hotspot", "net.hotspot"),
    ("has no open port", "nat.noport"), ("party port does not answer", "net.udp"),
    ("cannot send UDP", "net.udp"), ("never reaches the host", "net.udp"),
    ("never reached the host", "join.noreach"), ("keeps dying", "net.drops"),
    ("Hole punching", "nat.punch"), ("keeps freezing", "net.freeze"), ("lost the link", "net.drops"),
    ("High ping", "net.ping"), ("keeps leaving", "net.flap"), ("instead of the VPN", "net.vpn"),
    ("own address", "net.selfaddr"), ("More players tried", "join.full"), ("LAN code pointed", "join.lan"),
    ("installed (", "other.vpn"))


def verdict_keys(diag_text):
    """Keys for the problems net_doctor lists under "What's wrong" (see _VERDICTS); empty when it found none.
    clinic_logic.classify treats net./nat./join./firewall. keys as a connection problem."""
    keys, inside = set(), False
    for line in (diag_text or "").splitlines():
        t = line.strip()
        if t.startswith("**What's wrong"):
            inside = True
            continue
        if not inside:
            continue
        if not t:
            break
        if t[:1].isdigit():
            hit = [k for phrase, k in _VERDICTS if phrase in t]
            keys.update(hit or ["other.unknown"])
    return keys


class Ctx:
    """What live duty needs: the archive, the flags, and hooks the tests replace."""

    def __init__(self, clinic, *, live=False, delete=False, tmp_root=None, log_info_fn=None, diagnose_fn=None,
                 record_logs=None, client=None, guild_id=None):
        self.clinic, self.live, self.delete = clinic, live, delete
        self.tmp_root = Path(tmp_root) if tmp_root else Path(tempfile.gettempdir()) / "iosefka"
        self.tmp_root.mkdir(parents=True, exist_ok=True)
        self.log_info_fn, self.diagnose_fn, self.record_logs = log_info_fn, diagnose_fn, record_logs
        self.client, self.guild_id = client, guild_id
        self.busy = set()
        self.ask_delay = asks.ASK_DELAY  # seconds between two retest asks / nudges
        self.warned_day = None
        self.flood = hl.FloodGuard(int(hl.env_float("NETDOCTOR_UPLOADS_PER_HOUR", 10)))
        self.health = hl.Health(clinic, expect=("sweep", "outbox", "backup") if live else ())
        self.health.is_skipped = self._place_id_skipped

    def _place_id_skipped(self, place_id):
        ch = self.client.get_channel(int(place_id)) if self.client is not None else None
        return ch is not None and cd.place_skipped(ch)


CTX = None
LATE_RULES = ("explicit", "reply", "session", "followup", "ask")  # links to a bug that is already on the table


def _stills_ok(ctx):
    return cd.stills_allowed(ctx.clinic, ctx.tmp_root)


def _bug_logs(ctx, bug_id):
    return [f for f in ctx.clinic.files_for_bug(bug_id) if f["kind"] == "log"]


async def handle_log_message(message, *, live, ctx=None):
    """Archive a message's files, file/join its bug, answer, replace and delete it. `live` = a fresh event
    (the poster may be pinged); catch-up passes False. Returns a short result word. Does nothing unless the
    clinic's live duty is on (NW_LIVE=1); deletes only with NW_DELETE=1 (without it: archive, no posts)."""
    ctx = ctx or CTX
    if ctx is None or not ctx.live:
        return "off"
    atts = all_attachments(message)
    if not atts or getattr(message.author, "bot", False):
        return "none"
    dm = message.guild is None
    if not dm and cd.place_skipped(message.channel):
        return "skipped"
    if message.id in ctx.busy:
        return "busy"
    ctx.busy.add(message.id)
    try:
        clinic = ctx.clinic
        try:
            res = await cd.archive_message(clinic, message, log_info_fn=ctx.log_info_fn, tmpdir=ctx.tmp_root,
                                           stills=_stills_ok(ctx))
        except cd.DiskLow:
            print("disk low: not archiving now, the next sweep retries", flush=True)
            return "disk-low"
        thread = None if dm else cd.thread_of(message)
        if not res["removable"]:  # saves / pictures / videos alone: copied, the message stays, no note
            atts = (clinic.message(message.id) or {}).get("attachments") or []
            if clinic_logic.has_saves(atts) and res["state"] != "archive_failed":
                bug, _rule = await cd.assign_bug_ex(clinic, message, res["infos"], log_info_fn=ctx.log_info_fn,
                                                    tmpdir=ctx.tmp_root)
                cd.tag_saves(clinic, bug, atts)
            elif thread is not None and clinic.bug_for_thread(thread.id):
                clinic.upsert_message({"message_id": message.id, "bug_id": clinic.bug_for_thread(thread.id)})
            return "kept"
        if res["state"] in ("replaced", "deleted"):
            bug = (clinic.message(message.id) or {}).get("bug_id")
            rule = "known"
        else:
            if ctx.record_logs:  # keep the older "@bot update" memory working after the message is gone
                try:
                    await ctx.record_logs(message)
                except Exception as e:
                    print(f"record_logs failed: {e}", flush=True)
            if res["state"] != "verified":
                return res["state"]
            bug, rule = await cd.assign_bug_ex(clinic, message, res["infos"], log_info_fn=ctx.log_info_fn,
                                               tmpdir=ctx.tmp_root)
        if bug is None:
            return "no-bug"
        if rule in (None, "thread"):
            _flag_unsure(clinic, bug, message)
        if dm:  # a DM is private and never deleted: answer only when live duty may act (archive-only is silent)
            if ctx.delete:
                await message.channel.send(voice.dm_ack(bug), allowed_mentions=cd.AllowedMentions.none())
            return "dm"
        if not ctx.delete:
            return await cd.replace_and_delete(clinic, message, bug, ping=live, delete=False, need_export=False)
        staff = is_staff(message.author)
        if staff:
            clinic.update_bug(bug, tags={"source": "dev"})
        short = staff or _full_reply_recent(clinic, bug, message.author.id)
        if short:
            extra = ""
        else:
            extra = voice.added_to(bug) if rule in LATE_RULES else await _reply_text(ctx, message, bug, res["infos"])
        out = await cd.replace_and_delete(clinic, message, bug, ping=live, delete=True, need_export=False,
                                          extra=extra, short=short)
        if not short and out in ("deleted", "delete_forbidden"):
            clinic.add_note(bug, "iosefka", FULL_REPLY, "full reply posted", to=str(message.author.id))
        if out == "delete_forbidden":
            ctx.health.forbidden(message.channel.id, "delete messages")
        return out
    finally:
        ctx.busy.discard(message.id)


FULL_REPLY = "reply_full"
REPLY_WINDOW = 30 * 60  # seconds: one full reply per author and bug in this time, then short ones
STAFF_WORDS = ("dev", "staff", "mod", "admin")


def is_staff(author):
    """A poster holding a role whose name has dev / staff / mod / admin in it."""
    return any(w in (getattr(r, "name", "") or "").lower() for r in (getattr(author, "roles", None) or [])
               for w in STAFF_WORDS)


def _full_reply_recent(clinic, bug, author_id):
    since = time.time() - REPLY_WINDOW
    return any(n["to"] == str(author_id) and n["bug_id"] == bug for n in clinic.notes_of_kind(FULL_REPLY, since))


def _flag_unsure(clinic, bug, message):
    """Doubts Iosefka cannot settle alone, left for the later triage pass (tag `unsure`)."""
    why = []
    msgs = clinic.messages_for_bug(bug)
    if not any((m["content"] or "").strip() for m in msgs):
        why.append("no-text")
    atts = [a for m in msgs for a in m["attachments"]]
    def _logish(a):
        if a.get("kind") in ("log", "dump", "game"):
            return True
        if a.get("inner_files") is not None:
            return any(f.get("kind") in ("log", "dump", "game") for f in a["inner_files"])
        return bool(a.get("inner_shas"))
    if not any(_logish(a) for a in atts):
        why.append("no-log")
    if any(a.get("kind") == "archive" and a.get("inner") is None for a in atts):  # listing None: unreadable
        why.append("unreadable-file")
    if why:
        clinic.add_unsure(bug, why)


async def _reply_text(ctx, message, bug, infos):
    """Iosefka's words for this report: net_doctor's redacted advice for a connection problem, else the
    bug-filed thanks with the missing roles of the session."""
    clinic = ctx.clinic
    logs = _bug_logs(ctx, bug)
    diag, keys = "", set()
    if infos and ctx.diagnose_fn:
        with tempfile.TemporaryDirectory(prefix="nwdiag_", dir=str(ctx.tmp_root)) as tmp:
            paths = []
            for n, f in enumerate(logs[-MAX_LOGS:]):
                got = clinic.get_file(f["sha256"])
                if got:
                    d = Path(tmp) / str(n)
                    d.mkdir()
                    p = d / (Path(got[0]).name or "log.log")
                    p.write_bytes(got[1])
                    paths.append(p)
            if paths:
                try:
                    diag = ctx.diagnose_fn(paths)
                except Exception as e:
                    print(f"diagnose failed: {e}", flush=True)
        keys = verdict_keys(diag)
    kind = clinic_logic.classify(message.content or "", keys) if infos else "bug"
    if infos and clinic_logic.classify_unsure(message.content or "", keys):
        clinic.add_unsure(bug, ["network-or-bug"])
    if kind == "network":
        clinic.update_bug(bug, tags={"triage": "network"})
        clinic.add_note(bug, "iosefka", "triage", "network question: advice given, logs archived")
        return voice.network_help(diag)
    bug_infos = cd.log_infos(clinic, [{"kind": "log", "sha256": f["sha256"], "filename": f["filename"]}
                                      for f in logs], ctx.log_info_fn, ctx.tmp_root) if ctx.log_info_fn else infos
    what = clinic_logic.holds([a for m in clinic.messages_for_bug(bug) for a in m["attachments"]])
    return voice.bug_filed(bug, clinic_logic.missing_roles(bug_infos), what=what)


async def dispatch_message(message, ctx):
    """Live duty for one new message. True when the clinic took the message off the server (the older
    reaction / auto-reply path must then stay quiet)."""
    if ctx is not None and ctx.live and message.guild is not None and asks.is_releases_channel(
            message.channel, ctx.guild_id or asks.GUILD_ID):
        try:
            await asks.on_release_post(ctx, message)
        except Exception as e:
            print(f"retest asks failed: {type(e).__name__}: {e}", flush=True)
    if ctx is None or not ctx.live or getattr(message.author, "bot", False):
        return False
    guild_msg = message.guild is not None
    if guild_msg and (cd.place_skipped(message.channel) or
                      (ctx.guild_id and message.guild.id != ctx.guild_id)):
        return False
    if all_attachments(message):
        if guild_msg and not ctx.flood.allow(message.author.id):
            hl.queue_upload(ctx.clinic, message)  # too many uploads from one author this hour: the sweep takes it
            return True
        return await handle_log_message(message, live=True, ctx=ctx) == "deleted"
    await handle_text_message(message, ctx, live=True)
    return False


async def handle_text_message(message, ctx, *, live):
    """A message with no files: by an NW id, a reply, its thread or the reporter's own follow-up it may
    belong to a bug; then its words are stored there. Silent: it stays on the server, nothing is posted and
    no reaction is added (a reaction on every "thanks" read as noise to the players)."""
    return cd.link_text(ctx.clinic, message)


async def handle_raw_edit(payload, ctx):
    """An edit (cached or not): fetch the message and handle it if it now holds a file we have not archived."""
    if ctx is None or not ctx.live or ctx.client is None:
        return "off"
    if ctx.guild_id and getattr(payload, "guild_id", None) not in (None, ctx.guild_id):
        return "other-guild"
    ch = ctx.client.get_channel(payload.channel_id) or await ctx.client.fetch_channel(payload.channel_id)
    try:
        message = await ch.fetch_message(payload.message_id)
    except Exception as e:
        if type(e).__name__ in ("NotFound", "Forbidden"):
            return "gone"
        raise
    atts = all_attachments(message)
    rec = ctx.clinic.message(message.id)
    known = {a.get("id") for a in (rec["attachments"] if rec else [])}
    if not atts or all(a.id in known for a in atts):
        return "known"
    return await handle_log_message(message, live=True, ctx=ctx)


def _after(mid):
    try:
        import discord
        return discord.Object(id=mid)
    except ImportError:
        return type("Obj", (), {"id": mid})()


# handling results after which a message needs no further visit; anything else is retried by the next sweep
FINAL = ("none", "off", "skipped", "kept", "deleted", "dm", "would-replace", "blocked:dm", "blocked:skipped",
         "delete_forbidden")
RETRY_STATES = ("archive_failed", "archived", "verified", "replaced")
RETRY_PER_SWEEP = 200


async def retry_unfinished(ctx, skip=(), out=print):
    """Re-handle stored guild messages that never reached their end (failed download, stored but not
    replaced, replaced but not deleted): fetch each from Discord; one that is gone is marked deleted.
    Stops at low disk. Returns how many were handled."""
    n = tried = 0
    for rec in ctx.clinic.messages_in_state(RETRY_STATES):
        if rec["message_id"] in skip or rec.get("dm"):
            continue
        if tried >= RETRY_PER_SWEEP:
            break
        tried += 1
        cid = rec["thread_id"] or rec["channel_id"]
        try:
            place = ctx.client.get_channel(cid) or await ctx.client.fetch_channel(cid)
            if ctx.guild_id and getattr(getattr(place, "guild", None), "id", ctx.guild_id) != ctx.guild_id:
                continue
            message = await place.fetch_message(rec["message_id"])
        except Exception as e:
            kind = type(e).__name__
            if kind == "NotFound":
                ctx.clinic.set_state(rec["message_id"], "deleted")
            elif kind != "Forbidden":
                raise
            else:
                ctx.health.forbidden(cid)
            continue
        r = await handle_log_message(message, live=False, ctx=ctx)
        if r == "disk-low":
            out("disk low during retries, the next sweep continues")
            break
        n += r not in ("none", "off", "skipped", "kept")
    return n


async def drain_queue(ctx, out=print):
    """The flood guard's queue, oldest first: each upload message is handled like a sweep message (no ping,
    and a log message is still replaced and deleted, only later). Rows leave the queue when the message is
    done or gone; a message that could not finish stays queued."""
    n = 0
    for row in hl.queued_uploads(ctx.clinic, RETRY_PER_SWEEP):
        try:
            place = ctx.client.get_channel(row["channel_id"]) or await ctx.client.fetch_channel(row["channel_id"])
            message = await place.fetch_message(row["message_id"])
        except Exception as e:
            kind = type(e).__name__
            if kind == "NotFound":
                hl.drop_queued(ctx.clinic, row["message_id"])
            elif kind == "Forbidden":
                ctx.health.forbidden(row["channel_id"])
            else:
                raise
            continue
        r = await handle_log_message(message, live=False, ctx=ctx)
        if r in ("disk-low", "busy"):
            out("disk low while draining the upload queue, the next sweep continues")
            break
        hl.drop_queued(ctx.clinic, row["message_id"])
        n += 1
    return n


async def catch_up(ctx, guilds, archived=True, out=print):
    """Scan every place from its watermark and handle what was posted while the bot was away (no pings).
    A place with no watermark yet is scanned over its last 200 messages. The watermark stops before the
    first message whose handling is not final (failed download, low disk, busy...), so the next sweep
    sees it again; afterwards stored messages that never finished are retried by id."""
    if not ctx.live:
        return 0
    n = 0
    touched = set()
    for guild in guilds:
        async for place in cd.walk_places(guild, archived=archived):
            wm = ctx.clinic.watermark(place.id)
            last = wm
            stuck = False
            stop = False
            try:
                kw = {"after": _after(wm), "limit": None, "oldest_first": True} if wm else \
                    {"limit": 200, "oldest_first": False}
                msgs = [m async for m in place.history(**kw)]
                msgs.sort(key=lambda m: m.id)
                for m in msgs:
                    if asks.is_releases_channel(place, ctx.guild_id or asks.GUILD_ID):
                        try:
                            await asks.on_release_post(ctx, m, recent_only=True)
                        except Exception as e:
                            out(f"retest asks failed: {type(e).__name__}: {e}")
                    if getattr(m.author, "bot", False):
                        if not stuck:
                            last = max(last, m.id)
                        continue
                    r = "none"
                    if all_attachments(m):
                        touched.add(m.id)
                        r = await handle_log_message(m, live=False, ctx=ctx)
                        n += r not in ("none", "off", "skipped", "kept")
                    elif not cd.place_skipped(place):
                        await handle_text_message(m, ctx, live=False)
                    if r not in FINAL:
                        stuck = True
                    if not stuck:
                        last = max(last, m.id)
                    if r == "disk-low":
                        stop = True
                        break
            except Exception as e:
                if type(e).__name__ != "Forbidden":
                    raise
                out(f"no access: {getattr(place, 'name', place.id)}")
                ctx.health.forbidden(place.id)
                continue
            if last > wm:
                ctx.clinic.set_watermark(place.id, last)
            if stop:
                out("disk low during catch-up, the next sweep continues")
                return n
    if ctx.client is not None:
        n += await retry_unfinished(ctx, skip=touched, out=out)
    return n


def _seed_author(clinic, bug_id):
    """Reporter of a bug made before reporter_id existed: whoever posted its first log."""
    for m in clinic.messages_for_bug(bug_id):
        if cd.rec_removable(m["attachments"]):
            return m["author_id"]
    return None


async def send_outbox(ctx, out=print):
    """Post the developers' --tell texts (queued by iosefka_cli) in the bug's thread or channel, once each."""
    sent = 0
    for row in ctx.clinic.outbox_pending():
        bug = ctx.clinic.bug(row["bug_id"])
        if row.get("kind") == "ask":
            await _send_ask(ctx, row, bug)
            sent += 1
            continue
        place = None
        for cid in ((bug["thread_id"], bug["channel_id"]) if bug else ()):
            if cid:
                try:
                    place = ctx.client.get_channel(cid) or await ctx.client.fetch_channel(cid)
                    break
                except Exception as e:
                    if type(e).__name__ not in ("NotFound", "Forbidden"):
                        raise
        if place is None:
            ctx.clinic.add_note(row["bug_id"], "iosefka", "status", "tell not delivered: no place to post it")
            ctx.clinic.outbox_sent(row["id"])
            continue
        text, am = voice.told(row["text"]), cd.AllowedMentions.none()
        reporter = bug.get("reporter_id") or _seed_author(ctx.clinic, bug["id"])
        if reporter:  # only the reporter is addressed, never another poster of the bug
            text = f"<@{reporter}> " + text
            am = cd.AllowedMentions(users=[type("U", (), {"id": reporter})()], everyone=False, roles=False)
        reopened = None
        try:
            if cd.is_thread(place) and (place.archived or place.locked):
                reopened = (place.archived, place.locked)
                await place.edit(archived=False, locked=False)
            await place.send(text[:DISCORD_LIMIT], allowed_mentions=am)
        except Exception as e:
            if type(e).__name__ in ("Forbidden", "NotFound"):
                ctx.clinic.add_note(row["bug_id"], "iosefka", "status", f"tell not delivered: {type(e).__name__}")
                if type(e).__name__ == "Forbidden":
                    ctx.health.forbidden(place.id, "post")
                ctx.clinic.outbox_sent(row["id"])
                continue
            raise
        finally:
            if reopened:
                try:
                    await place.edit(locked=reopened[1], archived=reopened[0])
                except Exception:
                    pass
        ctx.clinic.outbox_sent(row["id"])
        sent += 1
    return sent


async def _send_ask(ctx, row, bug):
    """A developer's ask (`ask` / `cl ask`): posted in the record's place to its reporter, once; a staff reporter
    is never asked."""
    clinic_asks = asks
    ask = json.loads(row["text"])
    reporter = clinic_asks.reporter_of(ctx.clinic, bug) if bug else None
    note_bug = ask["bug"]
    res = "skip:no bug"
    if bug and reporter:
        member = None
        guild = ctx.client.get_guild(ctx.guild_id) if ctx.guild_id else None
        if guild is not None:
            member = guild.get_member(reporter)
        if member is not None and is_staff(member):
            res = "skip:staff"
        else:
            res = await clinic_asks.post_to_bug(
                ctx, bug, lambda m: voice.reporter_ask(m, note_bug, ask["issue"], ask["need"]))
    if res != "sent":
        ctx.clinic.add_note(note_bug, "iosefka", "status", f"ask not delivered ({row['bug_id']}): {res}")
    ctx.clinic.outbox_sent(row["id"])


async def disk_guard(ctx, guilds, out=print):
    """Once a day, when the archive nears its ceiling or the disk is nearly full, say so in the first staff* channel."""
    if _stills_ok(ctx):
        return False
    today = time.strftime("%Y-%m-%d")
    if ctx.warned_day == today:
        return False
    ctx.warned_day = today
    free = cd.media.free_disk(ctx.tmp_root) // 2**20
    text = (f"Clinic warning: the archive is {ctx.clinic.size_bytes() // 2**20} MiB and the disk has {free} MiB "
            f"free. Video stills are off until there is room; logs and pictures are still stored.")
    for g in guilds:
        ch = next((c for c in sorted(g.text_channels, key=lambda c: c.name) if c.name.startswith("staff")), None)
        if ch:
            await ch.send(text, allowed_mentions=cd.AllowedMentions.none())
            return True
    out(text)
    return False


def safe_prune(store, max_bytes, out=print):
    """One hourly prune; a failure (disk full, locked file) is logged, never ends the loop. Returns the
    size in bytes, or None on failure."""
    try:
        size = store.prune(max_bytes=max_bytes)
        out(f"db pruned: {size // 1024} KiB")
        return size
    except Exception as e:
        out(f"db prune failed: {type(e).__name__}: {e}")
        return None


def seconds_until(hour, now=None):
    import datetime as dt
    now = now or dt.datetime.now()
    nxt = now.replace(hour=hour, minute=0, second=0, microsecond=0)
    if nxt <= now:
        nxt += dt.timedelta(days=1)
    return (nxt - now).total_seconds()


def main():
    import discord
    from discord import app_commands
    sys.path.insert(0, str(HERE))
    import netdoctor_store as ns

    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    store = ns.Store(DB_PATH)
    try:
        os.chmod(DB_PATH, 0o600)  # player names inside
    except OSError:
        pass
    db_max = int(float(os.environ.get("NETDOCTOR_DB_MAX_MB", "3072")) * 1024 * 1024)
    store.prune(max_bytes=db_max)
    pruner = []

    token = os.environ.get("NETDOCTOR_TOKEN")
    if not token:
        print("set NETDOCTOR_TOKEN", file=sys.stderr)
        return 1
    if not SCRIPT.is_file():
        print(f"net_doctor.py not found: {SCRIPT}", file=sys.stderr)
        return 1

    intents = discord.Intents.default()
    # Reading attachments of ordinary messages (auto mode, /netdoctor-recent, the message menu) needs the
    # privileged Message Content intent: Developer Portal -> Bot -> Message Content Intent.
    intents.message_content = True
    client = discord.Client(intents=intents)
    global CTX
    CTX = Ctx(clinic_store.Clinic(DB_PATH), live=os.environ.get("NW_LIVE") == "1",
              delete=os.environ.get("NW_DELETE") == "1",
              tmp_root=os.environ.get("NW_TMP") or None, client=client,
              guild_id=int(os.environ["NETDOCTOR_GUILD"]) if os.environ.get("NETDOCTOR_GUILD") else None,
              log_info_fn=lambda p: log_info(load_doctor(), p),
              diagnose_fn=lambda paths: diagnose(paths, redact=True))
    if CTX.live:
        cd.clean_stale_tmp(CTX.tmp_root)
    print(f"clinic live duty: {'ON' if CTX.live else 'off'}, delete: {'ON' if CTX.delete else 'off'}", flush=True)
    tree = app_commands.CommandTree(client)
    guild = discord.Object(id=int(os.environ["NETDOCTOR_GUILD"])) if os.environ.get("NETDOCTOR_GUILD") else None
    auto = os.environ.get("NETDOCTOR_AUTO", "react").lower()  # off | react | reply
    allowed_channels = {int(x) for x in os.environ.get("NETDOCTOR_CHANNELS", "").replace(",", " ").split()
                        if x.isdigit()}
    pending = {}  # channel id -> asyncio task (reply mode debounce)

    async def keep(path, filename):
        """Store the log compressed (off the event loop: LZMA takes a moment on big logs)."""
        data = path.read_bytes()
        packed = await asyncio.get_running_loop().run_in_executor(None, ns.pack, data)
        return store.put_logdata(data, filename, packed)  # the database only from this thread

    last_hashes = {}

    async def run_on(attachments, redact):
        """Download attachments (each into its own folder, keeping its name), keep them compressed, run
        the doctor. The content hashes land in last_hashes[id(attachments)]."""
        with tempfile.TemporaryDirectory(prefix="netdoctor_") as tmp:
            paths, hashes = [], []
            for i, a in enumerate(attachments):
                d = Path(tmp) / str(i)
                d.mkdir()
                # the original name matters: Nightwatch names carry the time the log was closed
                p = d / (Path(a.filename).name or "log.log")
                await a.save(p)
                paths.append(p)
                hashes.append(await keep(p, a.filename))
            last_hashes[id(attachments)] = hashes
            return diagnose(paths, redact=redact)

    def remember(channel, files, text, bot_message_id=None, ref=None):
        """Store an analysis under a short reference (shown in the post) for the server-side lookup
        tool (tools/discord/netdoctor_query.py). Returns the reference."""
        ref = ref or ns.new_ref()
        store.add_analysis(ref, getattr(channel, "id", 0), getattr(getattr(channel, "guild", None), "id", 0),
                           bot_message_id, files, text)
        return ref

    def file_recs(entries, hashes=()):
        """(message or None, index, attachment) -> records the lookup tool can load again (stored copy by
        hash first, Discord second)."""
        hashes = list(hashes) + [""] * len(entries)
        return [{"channel": getattr(getattr(m, "channel", None), "id", 0), "message": getattr(m, "id", 0),
                 "index": i, "url": a.url, "filename": a.filename, "sha1": h}
                for (m, i, a), h in zip(entries, hashes)]

    async def send_text(send, text, private, ref=None):
        if ref:
            text = text + f"\n-# ref {ref}"
        parts = split_message(text)
        if len(parts) > 4:
            await send("The diagnosis is long, attached as a file.", ephemeral=private,
                       file=discord.File(io.BytesIO(text.encode()), "netdoctor.md"))
            return
        for part in parts:
            await send(part, ephemeral=private)

    async def recent_logs(channel, minutes, around=None):
        """Party log attachments posted in the channel within `minutes` (of now, or of `around`)."""
        import datetime as dt
        center = around or dt.datetime.now(dt.timezone.utc)
        after = center - dt.timedelta(minutes=minutes)
        before = center + dt.timedelta(minutes=minutes) if around else None
        found = []
        async for msg in channel.history(limit=300, after=after, before=before, oldest_first=True):
            found += [(msg, i, a) for i, a in enumerate(all_attachments(msg))
                      if is_log_name(a.filename) and a.size <= MAX_BYTES]
        return found[-MAX_LOGS:]

    @tree.command(name="netdoctor", description="Explain co-op connection problems from a session's logs "
                                                "(host + guests)")
    @app_commands.allowed_contexts(guilds=True, dms=True, private_channels=True)
    @app_commands.describe(log1="host or guest log", log2="another player's log", log3="another player's log",
                           log4="another player's log", log5="another player's log",
                           private="only you see the reply, with names and IPs (default: public, hidden)")
    async def netdoctor(interaction: discord.Interaction, log1: discord.Attachment,
                        log2: discord.Attachment = None, log3: discord.Attachment = None,
                        log4: discord.Attachment = None, log5: discord.Attachment = None,
                        private: bool = False):
        await interaction.response.defer(ephemeral=private, thinking=True)
        atts = [a for a in (log1, log2, log3, log4, log5) if a is not None]
        too_big = [a.filename for a in atts if a.size > MAX_BYTES]
        if too_big:
            await interaction.followup.send(f"Too large (max {MAX_BYTES // (1024 * 1024)} MB): "
                                            + ", ".join(too_big), ephemeral=private)
            return
        text = await run_on(atts, redact=not private)
        ref = remember(interaction.channel, file_recs([(None, i, a) for i, a in enumerate(atts)],
                                                      last_hashes.pop(id(atts), ())), text)
        await send_text(interaction.followup.send, text, private, ref)

    @tree.command(name="netdoctor-recent", description="Diagnose every party log posted in this channel "
                                                       "recently (by anyone)")
    @app_commands.allowed_contexts(guilds=True, dms=True, private_channels=True)
    @app_commands.describe(minutes="how far back to look (default 60)",
                           private="only you see the reply, with names and IPs")
    async def netdoctor_recent(interaction: discord.Interaction, minutes: int = 60, private: bool = False):
        await interaction.response.defer(ephemeral=private, thinking=True)
        found = await recent_logs(interaction.channel, max(1, min(minutes, 24 * 60)))
        if not found:
            await interaction.followup.send(f"No log files posted here in the last {minutes} min.",
                                            ephemeral=private)
            return
        atts = [a for _, _, a in found]
        text = await run_on(atts, redact=not private)
        await send_text(interaction.followup.send, text, private,
                        remember(interaction.channel, file_recs(found, last_hashes.pop(id(atts), ())), text))

    @tree.context_menu(name="Diagnose logs")
    @app_commands.allowed_contexts(guilds=True, dms=True, private_channels=True)
    async def diagnose_menu(interaction: discord.Interaction, message: discord.Message):
        # this message's logs plus logs posted within 30 min around it (the other players' halves)
        await interaction.response.defer(ephemeral=True, thinking=True)
        found = await recent_logs(message.channel, 30, around=message.created_at)
        if not found:
            await interaction.followup.send("No log files in or around that message.", ephemeral=True)
            return
        atts = [a for _, _, a in found]
        text = await run_on(atts, redact=False)
        await send_text(interaction.followup.send, text, True,
                        remember(message.channel, file_recs(found, last_hashes.pop(id(atts), ())), text))

    async def auto_reply(channel, dm=False):
        await asyncio.sleep(DM_DEBOUNCE_S if dm else AUTO_DEBOUNCE_S)  # wait for the other logs
        pending.pop(channel.id, None)
        found = await recent_logs(channel, 30)
        if found:
            # a DM is private: names and addresses stay in
            atts = [a for _, _, a in found]
            text = await run_on(atts, redact=not dm)
            await send_text(lambda *a, ephemeral=False, **k: channel.send(*a, **k), text, False,
                            remember(channel, file_recs(found, last_hashes.pop(id(atts), ())), text))

    async def record_logs(message, channel_id=None):
        """Remember who/when of every party log file in a message (downloaded once, then deleted).
        channel_id: file it under this channel (a thread's starter post lives in the parent channel)."""
        channel_id = channel_id or message.channel.id
        new = 0
        for idx, a in enumerate(all_attachments(message)):
            if not is_log_name(a.filename) or a.size > MAX_BYTES or \
                    store.has_log(channel_id, message.id, idx):
                continue
            with tempfile.TemporaryDirectory(prefix="netdoctor_") as tmp:
                p = Path(tmp) / (Path(a.filename).name or "log.log")
                await a.save(p)
                sha = await keep(p, a.filename)
                try:
                    info = log_info(load_doctor(), p)
                except Exception as e:  # a broken file must not stop the others
                    print(f"cannot read {a.filename}: {e}", flush=True)
                    continue
            if info["role"] is None:
                continue  # not a party log
            info["sha1"] = sha
            store.add_log(channel_id, message.id, idx, a.filename, info)
            new += 1
        return new

    CTX.record_logs = record_logs

    async def analyse_channel(trigger, force=False):
        """(Re)analyse every party log posted in this channel / thread: one post per session; a session
        that already has a post gets it edited, a new session gets a new post."""
        channel = trigger.channel
        if isinstance(channel, discord.Thread) and channel.parent is not None:
            # a thread started from a post: the post (and its logs) lives in the parent channel and has
            # the thread's id; forum posts are inside the thread already (the fetch then just fails)
            try:
                starter = await channel.parent.fetch_message(channel.id)
                if all_attachments(starter) and not starter.author.bot:
                    await record_logs(starter, channel.id)
            except (discord.HTTPException, AttributeError):
                pass
        async for msg in channel.history(limit=1000, oldest_first=True):
            if all_attachments(msg) and not msg.author.bot:
                await record_logs(msg)
        infos = store.logs(channel.id)
        if not infos:
            await trigger.reply("No party logs here yet. Post the logs of one session (the host's and every "
                                "guest's), then mention me again.", mention_author=False)
            return
        changed = 0
        for group in ns.group_sessions(infos):
            key = ns.session_key(infos, group)
            members = [(infos[i]["message_id"], infos[i]["att_index"]) for i in group]
            found = store.find_session(channel.id, key, members)
            if found and found[2] == len(members) and not force:
                continue  # nothing new for this session
            with tempfile.TemporaryDirectory(prefix="netdoctor_") as tmp:
                paths = []
                for n, (mid, idx) in enumerate(members):
                    d = Path(tmp) / str(n)
                    d.mkdir()
                    sha = infos[group[n]].get("sha1", "")
                    kept = store.get_logdata(sha) if sha else None
                    if kept:  # the stored copy: no download, works after the message is gone
                        p = d / (Path(kept[0] or "log.log").name)
                        p.write_bytes(kept[1])
                        paths.append(p)
                        continue
                    att = None
                    for where in (channel, getattr(channel, "parent", None)):  # starter post: in the parent
                        if where is None:
                            continue
                        try:
                            att = all_attachments(await where.fetch_message(mid))[idx]
                            break
                        except (discord.HTTPException, IndexError, AttributeError):
                            pass
                    if att is None:
                        continue  # deleted meanwhile and never stored
                    p = d / (Path(att.filename).name or "log.log")
                    await att.save(p)
                    paths.append(p)
                if not paths:
                    continue
                text = diagnose(paths, redact=True)
            stamp = int(time.time())
            ref = "ND-" + ((found[0] if found else None) or key)[:8]
            footer = f"\n-# ref {ref} · {len(paths)} log(s) of this session · updated <t:{stamp}:R> · " \
                     f"mention me again after more logs are posted"
            files = []
            body = text + footer
            if len(body) > DISCORD_LIMIT:
                body = text[:DISCORD_LIMIT - len(footer) - 60].rsplit("\n", 1)[0] + \
                    "\n… (full analysis attached)" + footer
                files = [discord.File(io.BytesIO(text.encode()), "netdoctor.md")]
            old = None
            if found and found[1]:
                try:
                    old = await channel.fetch_message(found[1])
                except discord.HTTPException:
                    old = None
            if old:
                await old.edit(content=body, attachments=files)
                bot_msg = old
            else:
                bot_msg = await channel.send(body, files=files)
            store.set_session(channel.id, key, bot_msg.id, members, old_key=found[0] if found else None)
            remember(channel, [{"channel": channel.id, "message": mid, "index": idx, "url": "",
                                "filename": infos[i].get("filename", ""), "sha1": infos[i].get("sha1", "")}
                               for (mid, idx), i in zip(members, group)], text, bot_msg.id, ref)
            changed += 1
        try:
            await trigger.add_reaction("\N{WHITE HEAVY CHECK MARK}" if changed else "\N{SLEEPING SYMBOL}")
        except discord.HTTPException:
            pass

    def mentioned(message):
        """The bot itself, or its own managed role (Discord offers both when you type @Net Doctor...)."""
        if client.user in message.mentions:
            return True
        return any(getattr(r.tags, "bot_id", None) == client.user.id for r in message.role_mentions if r.tags)

    def small_place(channel):
        """Threads, DMs and channels listed in NETDOCTOR_CHANNELS: the bot answers mentions and reacts there.
        Big ordinary channels are left alone (slash commands still work everywhere)."""
        if isinstance(channel, (discord.Thread, discord.DMChannel, discord.GroupChannel)):
            return True
        return channel.id in allowed_channels

    async def legacy_on_message(message):
        if message.author.bot:
            return
        ours = message.guild is None or not guild or message.guild.id == guild.id
        if not ours or not small_place(message.channel):
            return
        if mentioned(message) and wants_analysis(message.content, client.user.id):
            print(f"mention in {type(message.channel).__name__} {message.channel.id}", flush=True)
            try:
                async with message.channel.typing():
                    await analyse_channel(message, force=wants_rerun(message.content))
            except discord.Forbidden as e:
                print(f"no access in {message.channel.id}: {e}", flush=True)
                try:
                    await message.reply("I can't read this place's history or post here. A server admin needs "
                                        "to give me View Channel, Read Message History, Send Messages and Send "
                                        "Messages in Threads here (channel/forum permissions).",
                                        mention_author=False)
                except discord.HTTPException:
                    pass
            return
        if not any(is_log_name(a.filename) for a in all_attachments(message)):
            return
        asyncio.create_task(record_logs(message))  # remembered for a later "@bot update"
        if message.guild is None:  # a DM with the bot: answer on its own
            old = pending.get(message.channel.id)
            if old:
                old.cancel()
            pending[message.channel.id] = asyncio.create_task(auto_reply(message.channel, dm=True))
            return
        if auto == "off":
            return
        try:
            await message.add_reaction("🩺")
        except discord.HTTPException:
            pass
        if auto == "reply":
            old = pending.get(message.channel.id)
            if old:
                old.cancel()
            pending[message.channel.id] = asyncio.create_task(auto_reply(message.channel))

    @client.event
    async def on_message(message):
        if message.author.bot:
            return
        if mentioned(message) and message.guild is not None:
            await legacy_on_message(message)  # the analysis reads the logs before the clinic takes them
            await dispatch_message(message, CTX)
            return
        if await dispatch_message(message, CTX):
            return  # the clinic archived it and replaced it: no stethoscope, no second reply
        await legacy_on_message(message)

    @client.event
    async def on_raw_message_edit(payload):
        try:
            await handle_raw_edit(payload, CTX)
        except Exception as e:
            print(f"edit handling failed: {type(e).__name__}: {e}", flush=True)
            hl.note_error(CTX.clinic, "edit handling", e)

    async def clinic_loops():
        """Live duty background work: catch-up (start, then hourly), outbox (60 s), backup (04:00), guards."""
        guilds = [g for g in client.guilds if not guild or g.id == guild.id]
        first = True
        tick = 0
        next_backup = time.time() + seconds_until(4)
        while True:
            try:
                if tick % 60 == 0:
                    await drain_queue(CTX)
                    await catch_up(CTX, guilds, archived=first)
                    await asks.sweep_waiting(CTX)
                    await disk_guard(CTX, guilds)
                    first = False
                    hl.record_job(CTX.clinic, "sweep")
                await send_outbox(CTX)
                hl.record_job(CTX.clinic, "outbox")
                if time.time() >= next_backup:
                    next_backup = time.time() + seconds_until(4)
                    ok = await asyncio.get_running_loop().run_in_executor(
                        None, CTX.clinic.backup, Path(str(DB_PATH) + ".bak"))
                    print("backup " + ("written" if ok else "skipped: not enough free disk"), flush=True)
                    hl.record_job(CTX.clinic, "backup", "written" if ok else "skipped")
            except Exception as e:
                print(f"clinic loop: {type(e).__name__}: {e}", flush=True)
                hl.note_error(CTX.clinic, "clinic loop", e)
            tick += 1
            await asyncio.sleep(60)

    async def prune_hourly():
        while True:
            await asyncio.sleep(3600)
            if safe_prune(store, db_max) is None:
                hl.note_error(CTX.clinic, "db prune", RuntimeError("prune failed"))

    async def health_loop():
        """Every 5 min, apart from the clinic loop: conditions, alerts, the morning report, weekly upkeep."""
        h = CTX.health
        h.send = hl.Deliver(client, allowed_mentions=cd.AllowedMentions.none())
        loop = asyncio.get_running_loop()
        while True:
            try:
                hl.record_job(CTX.clinic, "health_tick")
                h.check()
                if h.upkeep_due(time.localtime().tm_hour):
                    await loop.run_in_executor(None, h.run_upkeep)
                await h.flush()
                await hl.send_daily_report(h)
            except Exception as e:
                print(f"health loop: {type(e).__name__}: {e}", flush=True)
            await asyncio.sleep(300)

    @client.event
    async def on_error(event, *args, **kwargs):
        import traceback
        traceback.print_exc()
        exc = sys.exc_info()[1]
        if exc is not None and CTX is not None:
            hl.note_error(CTX.clinic, f"event {event}", exc)

    @client.event
    async def on_ready():
        if not pruner:
            pruner.append(asyncio.create_task(prune_hourly()))
            hl.record_start(CTX.clinic)
            hl.record_running_commit()
            try:
                problems = CTX.health.self_check([g for g in client.guilds if not guild or g.id == guild.id],
                                                 delete=CTX.delete, skip=cd.place_skipped)
                print("self-check: " + ("ok" if not problems else "; ".join(problems)), flush=True)
            except Exception as e:
                print(f"self-check failed: {type(e).__name__}: {e}", flush=True)
                hl.note_error(CTX.clinic, "self-check", e)
            pruner.append(asyncio.create_task(health_loop()))
            if CTX is not None and CTX.live:
                pruner.append(asyncio.create_task(clinic_loops()))
        print("in servers: " + (", ".join(f"{g.name} ({g.id})" for g in client.guilds) or "none - invite the "
              "bot with the OAuth2 URL (scopes bot + applications.commands)"), flush=True)
        try:
            if guild:
                # 0.7.6 first registered the commands on the server only: remove that copy (it would
                # show every command twice next to the global ones)
                tree.clear_commands(guild=guild)
                await tree.sync(guild=guild)
            await tree.sync()
        except discord.Forbidden:
            print(f"cannot register commands in server {guild.id if guild else '(global)'}: the bot is not in it "
                  f"or was invited without the applications.commands scope; check NETDOCTOR_GUILD and re-invite",
                  flush=True)
            return
        voice.SELF_ID = client.user.id
        print(f"netdoctor bot ready as {client.user} (script {SCRIPT})", flush=True)

    client.run(token)
    return 0


if __name__ == "__main__":
    sys.exit(main())
