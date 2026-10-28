"""Iosefka's gentle asks: retest requests after a release post, and the nudge / close of bugs that wait for
logs. Targets are picked from the clinic database (pure, testable); posting reuses one path (post_to_bug).
Public texts carry no names, IPs or log lines; only the bug's own reporter is ever mentioned."""
import asyncio
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import clinic_changes as cc  # noqa: E402
import clinic_logic as logic  # noqa: E402
import iosefka_voice as voice  # noqa: E402

GUILD_ID = 1562099776869105674
RELEASES = "releases"
ASK_DELAY = 5.0            # seconds between two posts
NUDGE_DAYS = 14
CLOSE_DAYS = 30
RECENT_POST_DAYS = 7       # catch-up reads release posts no older than this
QUIET_KINDS = ("nudge", "retest-asked")  # Iosefka's own asks do not count as activity
CLOSE_TEXT = "closed: no logs for 30 days; reopens when logs arrive"

VERSION_RX = re.compile(r"(?<![\w.])v?(\d+\.\d+\.\d+(?:[ -]?(?:alpha|beta|rc)[ .-]?\d+)?)(?![\w.]*\d)", re.I)
NW_HINT = re.compile(r"nightwatch|\bnw\b", re.I)
COOP_HINT = re.compile(r"co-?op|bbport|\bport\b|bloodborne", re.I)


def versions_in(text):
    """Normalised version numbers named in a release post, in order, without repeats."""
    out = []
    for m in VERSION_RX.finditer(text or ""):
        try:
            v = cc.norm_version(m.group(1))
        except ValueError:
            continue
        if v not in out:
            out.append(v)
    return out


def tracks_for(ch, version, text):
    """Tracks whose changelist has `version`; when both do, the words of the post pick one (else both)."""
    have = [t for t in cc.TRACKS if any(v["version"] == version for v in ch.versions(t))]
    if len(have) > 1:
        nw, coop = bool(NW_HINT.search(text or "")), bool(COOP_HINT.search(text or ""))
        if nw and not coop:
            have = [t for t in have if t == "nw"]
        elif coop and not nw:
            have = [t for t in have if t == "coop"]
    return have


def reporter_of(clinic, bug):
    if bug.get("reporter_id"):
        return bug["reporter_id"]
    for m in clinic.messages_for_bug(bug["id"]):  # a bug made before reporter_id: whoever posted its first log
        if m.get("author_id") and m["author_id"] > 0 and m.get("attachments"):
            return m["author_id"]
    return None


def _asked(clinic, bug_id, version):
    return any(n["kind"] == "retest-asked" and n["text"].startswith(version + ":") for n in clinic.notes(bug_id))


def retest_targets(clinic, text):
    """[(bug dict, version)] to ask: bugs `fixed` and linked to a `done` item of a version the post names;
    never a bug already asked for that version, never a local bug or one without a reporter."""
    ch = cc.Changes(clinic)
    out, seen = [], set()
    for v in versions_in(text):
        for track in tracks_for(ch, v, text):
            for it in ch.items(track=track, version=v, status="done"):
                for bid in it["bugs"]:
                    b = clinic.bug(bid)
                    if (bid, v) in seen or not b or b["status"] != "fixed" or b["source"] == "local":
                        continue
                    if not reporter_of(clinic, b) or _asked(clinic, bid, v):
                        continue
                    seen.add((bid, v))
                    out.append((b, v))
    return out


def last_activity(clinic, bug):
    """Latest sign of life: stamped activity, creation, or any note that is not one of her own asks."""
    t = max(bug.get("activity") or 0, bug.get("created") or 0)
    for n in clinic.notes(bug["id"]):
        if n["kind"] not in QUIET_KINDS:
            t = max(t, n["at"] or 0)
    return t


def waiting_targets(clinic, now=None):
    """{'nudge': [bugs], 'close': [bugs]} among waiting-logs bugs (never local ones)."""
    now = now or time.time()
    nudge, close = [], []
    for b in clinic.bugs(status="waiting-logs"):
        if b["source"] == "local":
            continue
        last = last_activity(clinic, b)
        idle = (now - last) / 86400
        if idle >= CLOSE_DAYS:
            close.append(b)
        elif idle >= NUDGE_DAYS:
            if not any(n["kind"] == "nudge" and (n["at"] or 0) >= last for n in clinic.notes(b["id"])):
                nudge.append(b)
    return {"nudge": nudge, "close": close}


async def _place_for(ctx, bug):
    """(place, reply_to message id or None), or (None, reason). A DM bug's place is the reporter's DM."""
    import clinic_discord as cd
    client = ctx.client
    if bug["source"] == "dm":
        try:
            return await client.fetch_user(reporter_of(ctx.clinic, bug)), None
        except Exception as e:
            return None, type(e).__name__
    last = None
    for cid in (bug["thread_id"], bug["channel_id"]):
        if not cid:
            continue
        try:
            place = client.get_channel(cid) or await client.fetch_channel(cid)
        except Exception as e:
            if type(e).__name__ in ("NotFound", "Forbidden"):
                last = type(e).__name__
                continue
            raise
        guild = getattr(place, "guild", None)
        if ctx.guild_id and guild is not None and getattr(guild, "id", ctx.guild_id) != ctx.guild_id:
            return None, "other-guild"
        if await cd.aplace_skipped(place):
            return None, "skipped"
        reply = None
        for m in reversed(ctx.clinic.messages_for_bug(bug["id"])):
            if m.get("replacement_id") and (m.get("thread_id") or m.get("channel_id")) == place.id:
                reply = m["replacement_id"]
                break
        return place, reply
    return None, last or "no place"


async def post_to_bug(ctx, bug, make_text):
    """Post an ask for `bug` in its thread / channel (a DM for DM bugs), mentioning only its reporter.
    make_text(mention) -> str. Returns "sent", "skip:<why>", or "skip:gone" when Discord refused."""
    import clinic_discord as cd
    reporter = reporter_of(ctx.clinic, bug)
    place, extra = await _place_for(ctx, bug)
    if place is None:
        return "skip:" + str(extra)
    dm = bug["source"] == "dm"
    text = logic.mask_ips(make_text("" if dm else f"<@{reporter}>"))[:2000]
    am = cd.AllowedMentions.none() if dm else cd.AllowedMentions(
        users=[type("U", (), {"id": reporter})()], everyone=False, roles=False)
    reopened = None
    try:
        if cd.is_thread(place) and (place.archived or place.locked):
            reopened = (place.archived, place.locked)
            await place.edit(archived=False, locked=False)
        kw = {"allowed_mentions": am}
        if extra:
            try:
                kw["reference"] = await place.fetch_message(extra)
            except Exception:
                pass
        await place.send(text, **kw)
    except Exception as e:
        if type(e).__name__ in ("Forbidden", "NotFound"):
            return "skip:gone"
        raise
    finally:
        if reopened:
            try:
                await place.edit(locked=reopened[1], archived=reopened[0])
            except Exception:
                pass
    return "sent"


_LOCK = {}


def _lock():
    loop = asyncio.get_running_loop()
    if _LOCK.get("loop") is not loop:
        _LOCK["loop"], _LOCK["lock"] = loop, asyncio.Lock()
    return _LOCK["lock"]


async def send_retest_asks(ctx, text, *, dry_run=False, out=print, delay=None):
    """Ask the reporters of the fixed bugs named by a release post `text` to retest. Returns
    [(bug id, version, result)]; dry_run posts and records nothing (result "would-ask").
    One run at a time (live duty and catch-up may see the same post)."""
    async with _lock():
        return await _send_retest_asks(ctx, text, dry_run, out, delay)


async def _send_retest_asks(ctx, text, dry_run, out, delay):
    delay = getattr(ctx, "ask_delay", ASK_DELAY) if delay is None else delay
    results = []
    for bug, v in retest_targets(ctx.clinic, text):
        if dry_run:
            results.append((bug["id"], v, "would-ask"))
            out(f"would ask {bug['id']} to retest {v}")
            continue
        r = await post_to_bug(ctx, bug, lambda m, b=bug, v=v: voice.retest_ask(m, b["id"], v))
        if r == "sent":
            ctx.clinic.add_note(bug["id"], "iosefka", "retest-asked", f"{v}: asked to retest")
            await asyncio.sleep(delay)
        elif r == "skip:gone":
            ctx.clinic.add_note(bug["id"], "iosefka", "retest-asked", f"{v}: not delivered: no place to post it")
        results.append((bug["id"], v, r))
        out(f"{bug['id']} {v}: {r}")
    return results


async def sweep_waiting(ctx, *, dry_run=False, out=print, now=None, delay=None):
    """The hourly pass: one nudge at 14 idle days, close at 30. Returns (nudged ids, closed ids)."""
    delay = getattr(ctx, "ask_delay", ASK_DELAY) if delay is None else delay
    t = waiting_targets(ctx.clinic, now)
    nudged, closed = [], []
    for b in t["close"]:
        closed.append(b["id"])
        out(f"{'would close' if dry_run else 'close'} {b['id']}")
        if not dry_run:
            ctx.clinic.update_bug(b["id"], status="closed")
            ctx.clinic.add_note(b["id"], "iosefka", "status", CLOSE_TEXT)
    for b in t["nudge"]:
        if dry_run:
            nudged.append(b["id"])
            out(f"would nudge {b['id']}")
            continue
        if not reporter_of(ctx.clinic, b):
            continue
        r = await post_to_bug(ctx, b, lambda m, b=b: voice.logs_nudge(m, b["id"]))
        if r in ("sent", "skip:gone"):
            ctx.clinic.add_note(b["id"], "iosefka", "nudge",
                                "asked for logs" if r == "sent" else "not delivered: no place to post it")
            nudged.append(b["id"])
            if r == "sent":
                await asyncio.sleep(delay)
        out(f"{b['id']} nudge: {r}")
    return nudged, closed


def is_releases_channel(ch, guild_id=GUILD_ID):
    """The #releases text channel of the server (a thread inside it does not count)."""
    g = getattr(ch, "guild", None)
    return (getattr(ch, "name", None) == RELEASES and g is not None and getattr(g, "id", guild_id) == guild_id
            and not (hasattr(ch, "archived") and hasattr(ch, "locked")))


def message_text(message):
    parts = [getattr(message, "content", "") or ""]
    for e in getattr(message, "embeds", None) or []:
        parts += [getattr(e, "title", "") or "", getattr(e, "description", "") or ""]
    return "\n".join(parts)


async def on_release_post(ctx, message, *, recent_only=False):
    """A message in #releases: ask the reporters of the bugs it fixed to retest. Returns results or None."""
    import clinic_discord as cd
    if ctx is None or not ctx.live or ctx.client is None or             not is_releases_channel(message.channel, ctx.guild_id or GUILD_ID):
        return None
    me = getattr(ctx.client, "user", None)
    if me is not None and getattr(message.author, "id", None) == getattr(me, "id", -1):
        return None
    if recent_only and time.time() - cd._ts(message.created_at) > RECENT_POST_DAYS * 86400:
        return None
    return await send_retest_asks(ctx, message_text(message))
