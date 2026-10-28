"""Iosefka's clinic: the Discord side. Walk a server, archive a message's files into the clinic store, give
the message a bug, and replace-then-delete it. discord.py objects go in, plain values come out; nothing
here logs in, so the cull runner and the live bot share it and tests drive it with fakes.

The order that matters: files stored and re-verified -> replacement posted -> original deleted -> recorded.
Every step is recorded in nw_messages.state, so a rerun after a crash neither double-posts nor skips the
delete.
"""
import asyncio
import hashlib
import importlib.util
import re
import shutil
import sys
import tempfile
import time
import weakref
from pathlib import Path


def _sib(name):
    try:
        return __import__(name)
    except ImportError:  # loaded by path without tools/discord on sys.path
        spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
        mod = importlib.util.module_from_spec(spec)
        sys.modules.setdefault(name, mod)
        spec.loader.exec_module(mod)
        return mod


logic = _sib("clinic_logic")
media = _sib("clinic_media")
voice = _sib("iosefka_voice")

try:
    import discord
    AllowedMentions = discord.AllowedMentions
except ImportError:  # the dev PC has no discord.py; the shape is all the tests need
    class AllowedMentions:
        def __init__(self, everyone=None, users=True, roles=True, replied_user=True):
            self.everyone, self.users, self.roles, self.replied_user = everyone, users, roles, replied_user

        @classmethod
        def none(cls):
            return cls(everyone=False, users=False, roles=False, replied_user=False)

DOWNLOAD_TRIES = 3
RETRY_DELAY = 1.0
DISCORD_LIMIT = 2000
DEFAULT_UPLOAD = 25 * 1024 * 1024
MEDIA_NOTE = "(pictures/video kept in the clinic)"
FILE_KINDS_WITH_STORE = ("log", "dump", "game", "archive", "other")
STREAM_BYTES = 200 * 1024 * 1024   # bigger non-video files are LZMA-compressed in a stream, not read whole
LENIENT_SIZE_KINDS = ("image", "video")  # the CDN may re-serve these with other bytes than attachment.size
TRUNCATED_BELOW = 0.5              # ...but not under half of the listed size


class DiskLow(Exception):
    """Not enough free disk for the next download: stop cleanly, resume later."""


class DownloadError(Exception):
    pass


def all_attachments(message):
    """The message's own files plus those of messages forwarded in it (Discord keeps a forwarded message's
    files in its snapshot, not in .attachments)."""
    out = list(getattr(message, "attachments", None) or [])
    for snap in getattr(message, "message_snapshots", None) or []:
        out += list(getattr(snap, "attachments", None) or [])
    return out


def is_thread(ch):
    return hasattr(ch, "archived") and hasattr(ch, "locked")


def place_skipped(ch):
    """Skip rule for a channel or thread (threads inherit their parent's). Fail-closed: a channel that
    cannot be named (unknown, PartialMessageable) or a thread whose parent cannot be resolved is skipped."""
    base = ch
    if is_thread(ch):
        base = getattr(ch, "parent", None)
        if base is None:
            guild, pid = getattr(ch, "guild", None), getattr(ch, "parent_id", None)
            base = guild.get_channel(pid) if guild is not None and pid and hasattr(guild, "get_channel") else None
        if base is None:
            return True
    name = getattr(base, "name", None)
    if name is None:
        return True
    return logic.skipped_channel(name)


async def aplace_skipped(ch):
    """place_skipped, but a thread's parent missing from the cache is fetched before giving up."""
    if is_thread(ch) and getattr(ch, "parent", None) is None:
        guild, pid = getattr(ch, "guild", None), getattr(ch, "parent_id", None)
        if guild is not None and pid and hasattr(guild, "fetch_channel") and \
                not (hasattr(guild, "get_channel") and guild.get_channel(pid) is not None):
            try:
                base = await guild.fetch_channel(pid)
            except Exception:
                return True
            name = getattr(base, "name", None)
            return True if name is None else logic.skipped_channel(name)
    return place_skipped(ch)


def clean_stale_tmp(root):
    """Remove download folders (att_*, info_*) a crashed run left behind under the temp root."""
    n = 0
    root = Path(root)
    if root.is_dir():
        for d in list(root.glob("att_*")) + list(root.glob("info_*")):
            if d.is_dir():
                shutil.rmtree(d, ignore_errors=True)
                n += 1
    return n


def _not_found(exc):
    return type(exc).__name__ == "NotFound"


# --- walking ---------------------------------------------------------------------------------------------

async def walk_places(guild, archived=True):
    """Every text/news/voice/stage channel (and every thread of it, active or archived, public or private) and every
    forum post, except the skipped channels. Forums themselves have no messages and are not yielded."""
    forums = list(getattr(guild, "forums", None) or [])
    seen = set()
    places = list(getattr(guild, "text_channels", None) or []) + \
        list(getattr(guild, "voice_channels", None) or []) + list(getattr(guild, "stage_channels", None) or [])
    for ch in places + forums:
        if logic.skipped_channel(getattr(ch, "name", "")):
            continue
        is_forum = ch in forums
        if not is_forum:
            yield ch
        threads = list(getattr(ch, "threads", None) or [])
        can_archive = hasattr(ch, "archived_threads")
        for private in (() if not archived or not can_archive else (None,) if is_forum else (False, True)):
            try:
                it = ch.archived_threads(limit=None) if private is None else \
                    ch.archived_threads(private=private, limit=None)
                async for t in it:
                    threads.append(t)
            except Exception as e:  # no access to private archives, etc.
                print(f"cannot list archived threads of {ch.name}: {type(e).__name__}", file=sys.stderr, flush=True)
        for t in threads:
            if t.id not in seen:
                seen.add(t.id)
                yield t


# --- archiving -------------------------------------------------------------------------------------------

def _sha_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


async def _download(att, dest, strict=True):
    """Save an attachment. `strict`: the bytes must equal attachment.size. Otherwise (pictures and videos,
    which the CDN may re-serve with other metadata) anything but empty or truncated (< 50%) is accepted.
    Returns the size served."""
    last = None
    for i in range(DOWNLOAD_TRIES):
        try:
            await att.save(dest)
            got, want = Path(dest).stat().st_size, int(att.size or 0)
            if want and (got != want if strict else (got == 0 or got < want * TRUNCATED_BELOW)):
                raise OSError(f"size mismatch {got} != {want}")
            return got
        except Exception as e:
            last = e
            Path(dest).unlink(missing_ok=True)
            if i + 1 < DOWNLOAD_TRIES:
                await asyncio.sleep(RETRY_DELAY * (i + 1))
    raise DownloadError(f"{type(last).__name__}: {last}")


_LOCKS = weakref.WeakKeyDictionary()


def _dl_lock():
    """The one download+ffmpeg lock of this event loop (one file at a time: 1 CPU, 939 MB RAM)."""
    loop = asyncio.get_running_loop()
    if loop not in _LOCKS:
        _LOCKS[loop] = asyncio.Lock()
    return _LOCKS[loop]


def _prep_small(prepare, path, kind):
    """(sha, blob, codec, raw size) of a file read whole; the CPU work, for an executor."""
    data = Path(path).read_bytes()
    if kind == "image":
        stored, codec = media.shrink_image(data)
        sha, blob, codec = prepare(data, kind, stored=None if codec == "orig" else stored, codec=codec)
    else:
        sha, blob, codec = prepare(data, kind)
    return sha, blob, codec, len(data)


def _prep_archive(prepare, path):
    """Archive listing (None: unreadable) plus the prepared inner logs
    [(path inside the archive, kind, (sha, blob, codec), raw size)]. zip, rar and 7z, nested."""
    inner = []
    for n, b in media.archive_inner_logs(path):
        k = logic.file_kind(n)
        inner.append((n, k, prepare(b, k), len(b)))
    return media.archive_listing(path), inner


def store_inner(clinic, rec, listing, inner):
    """Put the prepared inner logs in the store and describe them in the attachment record: `inner` (member
    names, None = unreadable), `inner_shas` and `inner_files` [{sha256, name, kind}] (name = path inside)."""
    files = []
    for nm, k, p, raw_n in inner:
        sha = clinic.put_prepared(p[0], p[1], p[2], raw_n, Path(nm).name or "inner.log", k)
        files.append({"sha256": sha, "name": nm, "kind": k})
    rec["inner"] = listing
    rec["inner_shas"] = [f["sha256"] for f in files]
    rec["inner_files"] = files
    return rec


async def _store_attachment(clinic, att, tmp, stills):
    async with _dl_lock():
        return await _store_attachment_locked(clinic, att, tmp, stills)


async def _store_attachment_locked(clinic, att, tmp, stills):
    name = Path(att.filename or "file").name or "file"
    kind = logic.file_kind(name)
    size = int(att.size or 0)
    if not media.enough_disk(tmp, size):
        raise DiskLow(f"free disk below {size} bytes + reserve")
    d = Path(tempfile.mkdtemp(prefix="att_", dir=str(tmp)))
    try:
        path = d / name
        served = await _download(att, path, strict=kind not in LENIENT_SIZE_KINDS)
        rec = {"id": att.id, "filename": name, "kind": kind, "size": served, "sha256": None}
        if kind in LENIENT_SIZE_KINDS and size and served != size:
            rec["served_size"] = served
        mime = getattr(att, "content_type", None) or ""
        loop = asyncio.get_running_loop()
        if kind == "video":
            rec["sha256"] = sha = await loop.run_in_executor(None, _sha_file, path)
            if stills and not clinic.stills(sha):
                frames = await loop.run_in_executor(None, media.video_stills, path)
                if frames:
                    clinic.add_stills(sha, frames)
        else:
            if served > STREAM_BYTES:
                sha, blob, raw = await loop.run_in_executor(None, clinic.prepare_stream, path)
                codec = "lzma"
            else:
                sha, blob, codec, raw = await loop.run_in_executor(None, _prep_small, clinic.prepare_file,
                                                                   path, kind)
            rec["sha256"] = clinic.put_prepared(sha, blob, codec, raw, name, kind, mime)
            del blob
            if kind == "archive":
                listing, inner = await loop.run_in_executor(None, _prep_archive, clinic.prepare_file, path)
                store_inner(clinic, rec, listing, inner)
        return rec
    finally:
        shutil.rmtree(d, ignore_errors=True)


def att_verified(clinic, a):
    sha = a.get("sha256")
    if not sha:
        return False
    if a.get("kind") == "video":
        return bool(clinic.stills(sha))
    return clinic.verify_file(sha) and all(clinic.verify_file(s) for s in a.get("inner_shas") or [])


def rec_removable(atts):
    return any(logic.removable(a["filename"], a.get("inner")) for a in atts)


def log_infos(clinic, atts, log_info_fn, tmp):
    """net_doctor log_info of every party log in these stored attachments (inner zip logs too)."""
    if log_info_fn is None:
        return []
    todo = [(a["sha256"], a["filename"]) for a in atts if a.get("kind") == "log" and a.get("sha256")]
    for a in atts:
        for s in a.get("inner_shas") or []:
            info = clinic.file_info(s)
            if info and info["kind"] == "log":
                todo.append((s, info["filename"]))
    out = []
    for sha, name in todo:
        got = clinic.get_file(sha)
        if not got:
            continue
        d = Path(tempfile.mkdtemp(prefix="info_", dir=str(tmp)))
        try:
            p = d / (Path(name).name or "log.log")
            p.write_bytes(got[1])
            try:
                info = log_info_fn(p)
            except Exception as e:
                print(f"cannot read {name}: {e}", file=sys.stderr, flush=True)
                continue
            if info and info.get("role") is not None:
                info = dict(info)
                info["sha256"] = sha
                clinic.put_loginfo(sha, info)
                out.append(info)
        finally:
            shutil.rmtree(d, ignore_errors=True)
    return out


def _place_ids(message):
    ch = message.channel
    if is_thread(ch):
        parent = getattr(ch, "parent", None)
        return (getattr(ch, "parent_id", None) or getattr(parent, "id", None) or ch.id), ch.id
    return ch.id, None


def _ts(dt):
    return dt.timestamp() if dt is not None else None


async def archive_message(clinic, message, *, log_info_fn=None, tmpdir=None, stills=True):
    """Download and store every attachment, upsert the message. Returns {"removable", "infos", "state"}.
    state: verified (all stored and re-verified), archive_failed (a download failed: retried by the next
    sweep), archived (stored but a video has no stills yet), kept (no files), or replaced/deleted when the
    message is already past that. Raises DiskLow before a download that would not fit."""
    tmp = Path(tmpdir or tempfile.gettempdir())
    tmp.mkdir(parents=True, exist_ok=True)
    old = clinic.message(message.id)
    if old and old["state"] == "deleted":
        return {"removable": rec_removable(old["attachments"]), "infos": [], "state": old["state"]}
    prev = {a["id"]: a for a in (old["attachments"] if old else []) if a.get("id") is not None}
    atts, failed = [], False
    for a in all_attachments(message):
        p = prev.get(a.id)
        if p and att_verified(clinic, p):
            atts.append(p)
            continue
        try:
            atts.append(await _store_attachment(clinic, a, tmp, stills))
        except DiskLow:
            raise
        except Exception as e:
            failed = True
            atts.append({"id": a.id, "filename": Path(a.filename or "file").name, "kind": logic.file_kind(a.filename),
                         "size": int(a.size or 0), "sha256": None, "error": str(e)[:200]})
    if failed:
        state = "archive_failed"
    elif not atts:
        state = "kept"
    else:
        state = "verified" if all(att_verified(clinic, a) for a in atts) else "archived"
    # a replaced message keeps that state in the record (its replacement is posted); the returned state says
    # whether all its files are stored now
    saved = "replaced" if old and old["state"] == "replaced" else state
    if saved == "replaced" and state == "verified":
        state = "replaced"
    channel_id, thread_id = _place_ids(message)
    ref = getattr(message, "reference", None)
    clinic.upsert_message({
        "message_id": message.id, "channel_id": channel_id, "thread_id": thread_id,
        "author_id": message.author.id, "author_name": str(getattr(message.author, "name", "")),
        "created": _ts(message.created_at), "edited": _ts(getattr(message, "edited_at", None)),
        "content": message.content or "", "reply_to": getattr(ref, "message_id", None),
        "attachments": atts, "state": saved, "seen": time.time(),
        "dm": 1 if message.guild is None else 0})
    return {"removable": rec_removable(atts), "infos": log_infos(clinic, atts, log_info_fn, tmp), "state": state}


def store_text(clinic, message, bug_id):
    """Keep a message's text (no files) under a bug; never overwrites an archived record's state."""
    channel_id, thread_id = _place_ids(message)
    ref = getattr(message, "reference", None)
    if clinic.message(message.id):
        clinic.upsert_message({"message_id": message.id, "bug_id": bug_id})
        return
    clinic.upsert_message({
        "message_id": message.id, "channel_id": channel_id, "thread_id": thread_id,
        "author_id": message.author.id, "author_name": str(getattr(message.author, "name", "")),
        "created": _ts(message.created_at), "edited": _ts(getattr(message, "edited_at", None)),
        "content": message.content or "", "reply_to": getattr(ref, "message_id", None),
        "attachments": [], "bug_id": bug_id, "state": "kept", "seen": time.time(),
        "dm": 1 if message.guild is None else 0})


# --- bugs ------------------------------------------------------------------------------------------------

PENDING = {"triage": "pending"}  # every bug Iosefka files waits for a later triage pass
BUG_ID_RX = re.compile(r"NW-[0-9a-f]{7}", re.I)
REACT = "\U0001FA7A"


def resolve_dup(clinic, bug_id):
    """Follow dup_of: new material for a duplicate goes to the bug it duplicates."""
    for _ in range(8):
        b = clinic.bug(bug_id)
        if not b or b["status"] != "duplicate" or not b["dup_of"] or not clinic.bug(b["dup_of"]):
            break
        bug_id = b["dup_of"]
    return bug_id


def message_link(message):
    g = getattr(message, "guild", None)
    gid = getattr(g, "id", None) if g is not None else "@me"
    return f"https://discord.com/channels/{gid or 0}/{message.channel.id}/{message.id}"


def explicit_bugs(clinic, text):
    out = []
    for m in BUG_ID_RX.findall(text or ""):
        bid = "NW-" + m[3:].lower()
        if clinic.bug(bid):
            bid = resolve_dup(clinic, bid)
            if bid not in out:
                out.append(bid)
    return out


def reply_bug(clinic, message):
    ref = getattr(getattr(message, "reference", None), "message_id", None)
    if not ref:
        return None
    rec = clinic.message(ref)
    bug = (rec or {}).get("bug_id") or clinic.message_by_replacement(ref)
    return resolve_dup(clinic, bug) if bug else None


def _same_session(a, b):
    if a.get("day") and b.get("day") and a["day"] != b["day"]:
        return False  # a session is one day's; the post may come days later, the log cannot
    try:
        return _sib("netdoctor_store").same_session(a, b)
    except KeyError:
        return False


def session_bugs(clinic, infos, own_shas=()):
    """Bugs holding a stored log of the same party session as one of these infos (any age)."""
    out = []
    if not infos:
        return out
    for sha, old in clinic.all_loginfos().items():
        if sha in own_shas or not any(_same_session(old, i) for i in infos):
            continue
        bug = clinic.bug_for_files([sha])
        if bug:
            bug = resolve_dup(clinic, bug)
            if bug not in out:
                out.append(bug)
    return out


def find_link(clinic, message, infos=None):
    """The bug a message belongs to by the late-detail rules: (bug, rule, unsure reasons) or None.
    Priority: explicit id > reply > thread > same session > 24 h reporter follow-up."""
    explicit = explicit_bugs(clinic, message.content)
    reply = reply_bug(clinic, message)
    t = thread_of(message) if message.guild is not None else None
    tb = clinic.bug_for_thread(t.id) if t is not None else None
    thread = resolve_dup(clinic, tb) if tb else None
    plain = message.guild is not None and t is None
    own = {i.get("sha256") for i in infos or []}
    session = session_bugs(clinic, infos or [], own) if plain and infos else []
    follow = []
    if plain and not infos:
        follow = [resolve_dup(clinic, b) for b in
                  clinic.reporter_candidates(message.author.id, message.channel.id, _ts(message.created_at))]
    if explicit:
        bug, rule, others = explicit[0], "explicit", explicit[1:] + session + ([reply] if reply else [])
    elif reply:
        bug, rule, others = reply, "reply", session
    elif thread:
        bug, rule, others = thread, "thread", []
    elif session:
        bug, rule, others = session[0], "session", session[1:]
    elif follow:
        bug, rule, others = follow[0], "followup", follow[1:]
    else:
        return None
    return bug, rule, (["which-bug"] if any(o != bug for o in others) else [])


def join_bug(clinic, message, bug, rule, unsure=()):
    """New material joined `bug`: reopen a fixed/closed one, stamp activity, note weak links, flag doubts."""
    b = clinic.bug(bug)
    if b["status"] in ("fixed", "closed"):
        clinic.update_bug(bug, status="open")
        clinic.add_note(bug, "iosefka", "status", f"new report after {b['status']}: {message_link(message)}")
    clinic.touch_bug(bug)
    if rule in ("session", "followup"):
        clinic.add_note(bug, "iosefka", "auto-link", f"{rule} rule: message {message.id} {message_link(message)}")
    if unsure:
        clinic.add_unsure(bug, list(unsure))
    return bug


def link_text(clinic, message):
    """A text-only message: join the bug the rules name (store its words), or None. Idempotent."""
    got = find_link(clinic, message)
    if not got:
        return None
    bug, rule, unsure = got
    rec = clinic.message(message.id)
    if rec and rec.get("bug_id") == bug:
        return None
    store_text(clinic, message, bug)
    join_bug(clinic, message, bug, rule, unsure)
    return bug, rule


def thread_of(message):
    ch = message.channel
    return ch if is_thread(ch) else getattr(message, "thread", None)


def ensure_thread_bug(clinic, thread, reporter_id=None):
    """The thread's bug (made if missing). The reporter is the thread's owner, else `reporter_id`."""
    bug = clinic.bug_for_thread(thread.id)
    if bug:
        return bug
    parent = getattr(thread, "parent", None)
    forum = parent is not None and hasattr(parent, "available_tags")
    pid = getattr(thread, "parent_id", None) or getattr(parent, "id", None)
    return clinic.new_bug(logic.mask_ips(thread.name or "thread")[:80], "forum post" if forum else "thread",
                          pid, thread.id, getattr(thread, "owner_id", None) or reporter_id, tags=PENDING)


def _title(text):
    line = next((ln.strip() for ln in (text or "").splitlines() if ln.strip()), "")
    return logic.mask_ips(line)[:60] or "log report"


def _log_shas(rec):
    return [a["sha256"] for a in rec["attachments"]
            if a.get("sha256") and a.get("kind") in ("log", "dump", "game", "archive")]


def light(message, has_log=None):
    """The fields clinic_logic.cluster needs, plus the ones needed to store a text-only message."""
    if has_log is None:
        has_log = any(logic.removable(a.filename) for a in all_attachments(message))
    ref = getattr(message, "reference", None)
    return {"id": message.id, "author_id": message.author.id, "created": _ts(message.created_at),
            "reply_to": getattr(ref, "message_id", None), "has_log": has_log,
            "content": message.content or "", "author_name": str(getattr(message.author, "name", "")),
            "channel_id": message.channel.id}


def assign_clusters(clinic, lights, infos_by_id, focus=None):
    """Cluster plain-channel messages into bugs (new, or the one already holding them), store the text of
    members that are not stored yet. Returns {message id: bug id}. `focus`: only the cluster holding it."""
    by_id = {m["id"]: m for m in lights}
    out = {}
    for g in logic.cluster(lights, infos_by_id):
        if focus is not None and focus not in g:
            continue
        recs = {i: clinic.message(i) for i in g}
        existing = []
        for i in g:
            b = recs[i] and recs[i].get("bug_id")
            if b and b not in existing:
                existing.append(b)
        shas = sorted({s for r in recs.values() if r for s in _log_shas(r)})
        bug = existing[0] if existing else (clinic.bug_for_files(shas) if shas else None)
        if not bug:
            seed = next((by_id[i] for i in g if by_id[i]["has_log"]), by_id[g[0]])
            first = next((by_id[i] for i in g if by_id[i]["author_id"] == seed["author_id"]
                          and by_id[i]["content"].strip()), seed)
            bug = clinic.new_bug(_title(first["content"]), "channel cluster", seed["channel_id"], None, seed["author_id"],
                                tags=PENDING)
        for dup in existing[1:]:
            if dup != bug:
                clinic.merge_bugs(bug, dup)
        for i in g:
            m = by_id[i]
            if recs[i]:
                clinic.upsert_message({"message_id": i, "bug_id": bug})
            else:
                clinic.upsert_message({
                    "message_id": i, "channel_id": m["channel_id"], "thread_id": None, "author_id": m["author_id"],
                    "author_name": m["author_name"], "created": m["created"], "content": m["content"],
                    "reply_to": m["reply_to"], "attachments": [], "bug_id": bug, "state": "kept",
                    "seen": time.time()})
            out[i] = bug
    return out


async def _neighbors(message, window=logic.WINDOW_S):
    """Messages around this one in its channel, from Discord when the channel can say (fakes may not)."""
    hist = getattr(message.channel, "history", None)
    if hist is None:
        return []
    out = []
    try:
        async for m in hist(limit=100, around=message):
            if m.id != message.id and abs(_ts(m.created_at) - _ts(message.created_at)) <= window:
                out.append(m)
    except Exception as e:
        print(f"no history around {message.id}: {type(e).__name__}", file=sys.stderr, flush=True)
    return out


async def assign_bug(clinic, message, infos, *, log_info_fn=None, tmpdir=None):
    """The bug a log message belongs to (see assign_bug_ex)."""
    return (await assign_bug_ex(clinic, message, infos, log_info_fn=log_info_fn, tmpdir=tmpdir))[0]


async def assign_bug_ex(clinic, message, infos, *, log_info_fn=None, tmpdir=None):
    """(bug, rule): rule names the late-detail link used (explicit, reply, thread, session, followup) or is
    None. Order: an NW id in the text > a reply to the bug's message or note > the thread's bug > a log of
    the same session already in a bug > the 24 h follow-up (text only) > thread/forum post -> its bug; DM
    -> one per conversation per UTC day; plain channel -> the cluster around the message."""
    rec = clinic.message(message.id)
    if rec and rec.get("bug_id"):
        return rec["bug_id"], None
    got = find_link(clinic, message, infos)
    if got:
        bug, rule, unsure = got
        clinic.upsert_message({"message_id": message.id, "bug_id": bug})
        join_bug(clinic, message, bug, rule, unsure)
        return bug, rule
    return await _assign_new(clinic, message, infos, log_info_fn, tmpdir), None


async def _assign_new(clinic, message, infos, log_info_fn, tmpdir):
    t = thread_of(message)
    if message.guild is None:
        day = time.strftime("%Y-%m-%d", time.gmtime(_ts(message.created_at)))
        title = f"DM log report {day}"
        bug = next((b["id"] for b in clinic.bugs() if b["source"] == "dm" and b["channel_id"] == message.channel.id
                    and b["title"] == title), None) or clinic.new_bug(title, "dm", message.channel.id, None, message.author.id,
                                                                tags=PENDING)
        clinic.upsert_message({"message_id": message.id, "bug_id": bug})
        return bug
    if t is not None:
        bug = ensure_thread_bug(clinic, t, message.author.id)
        clinic.upsert_message({"message_id": message.id, "bug_id": bug})
        return bug
    tmp = Path(tmpdir or tempfile.gettempdir())
    lights = [light(message, True)]
    infos_by_id = {message.id: infos}
    for m in await _neighbors(message):
        lights.append(light(m))
        r = clinic.message(m.id)
        if r and log_info_fn:
            infos_by_id[m.id] = log_infos(clinic, r["attachments"], log_info_fn, tmp)
    # stored neighbours the channel history did not return
    known = {m["id"] for m in lights}
    created = _ts(message.created_at)
    for r in clinic.messages_near(message.channel.id, created, logic.WINDOW_S):
        if r["message_id"] not in known:
            lights.append({"id": r["message_id"], "author_id": r["author_id"], "created": r["created"],
                           "reply_to": r["reply_to"], "has_log": rec_removable(r["attachments"]),
                           "content": r["content"] or "", "author_name": r["author_name"] or "",
                           "channel_id": r["channel_id"]})
            if log_info_fn:
                infos_by_id[r["message_id"]] = log_infos(clinic, r["attachments"], log_info_fn, tmp)
    return assign_clusters(clinic, lights, infos_by_id, focus=message.id)[message.id]


# --- replace and delete ----------------------------------------------------------------------------------

def _exported(clinic, atts):
    for a in atts:
        if logic.removable(a["filename"], a.get("inner")):
            if not clinic.exported(a["sha256"]) or not all(clinic.exported(s) for s in a.get("inner_shas") or []):
                return False
    return True


def _chunks(text, limit=DISCORD_LIMIT):
    out, cur = [], ""
    for line in text.splitlines(keepends=True):
        while len(line) > limit:
            if cur:
                out.append(cur)
                cur = ""
            out.append(line[:limit])
            line = line[limit:]
        if len(cur) + len(line) > limit:
            out.append(cur)
            cur = ""
        cur += line
    if cur:
        out.append(cur)
    return out


async def _media_files(message, rec, files=None, shared=None):
    """Files to repost in the replacement: pictures and videos that are not removable, within the upload
    cap. Anything else that stays out of the message (text, odd files) is left in the clinic (media note)."""
    removable_ids = {a["id"] for a in rec["attachments"] if logic.removable(a["filename"], a.get("inner"))}
    limit = getattr(getattr(message, "guild", None), "filesize_limit", None) or DEFAULT_UPLOAD
    files = [] if files is None else files  # a run shares one list: at most 10 files in one post
    budget = limit if shared is None or shared[0] is None else shared[0]
    left_out = False
    for a in all_attachments(message):
        if a.id in removable_ids:
            continue
        if logic.file_kind(a.filename) not in ("image", "video"):
            left_out = True
            continue
        if len(files) >= 10 or a.size > budget:
            left_out = True
            continue
        try:
            files.append(await a.to_file())
            budget -= a.size
            if shared is not None:
                shared[0] = budget
        except Exception:
            left_out = True
    return files, left_out


def _status(exc):
    return getattr(exc, "status", None)


async def _gate(clinic, message, bug_id, delete, need_export):
    """Every safety check before a replacement. Returns (result, message, rec): result is None when the
    message may go on, else the final answer (blocked:<why>, deleted, would-replace, replaced)."""
    mid = message.id
    rec = clinic.message(mid)
    if not rec:
        return "blocked:not-archived", message, rec
    if rec["state"] == "deleted":
        return "deleted", message, rec
    if message.guild is None:  # a DM is private: never touched
        return "blocked:dm", message, rec
    if await aplace_skipped(message.channel):
        return "blocked:skipped", message, rec
    if delete:
        try:
            message = await message.channel.fetch_message(mid)
        except Exception as e:
            if _not_found(e):
                clinic.set_state(mid, "deleted")
                return "deleted", message, rec
            raise
    ids = {a.id for a in all_attachments(message)}
    stored = {a["id"] for a in rec["attachments"]}
    if rec["state"] not in ("verified", "replaced") or not rec["attachments"] or not ids <= stored \
            or not all(att_verified(clinic, a) for a in rec["attachments"]):
        return "blocked:not-verified", message, rec
    if need_export and not _exported(clinic, rec["attachments"]):
        return "blocked:not-exported", message, rec
    if not delete:
        return ("replaced" if rec["state"] == "replaced" else "would-replace"), message, rec
    return None, message, rec


async def replace_and_delete(clinic, message, bug_id, *, ping, delete, need_export, extra=""):
    """Post the replacement note, delete the original, record. Returns the final state, "would-replace"
    when delete is False, or "blocked:<why>" (not-archived, dm, skipped, not-verified, not-exported). A
    message already `replaced` skips the post. A delete the bot may not do is recorded as
    "delete_forbidden". `extra`: more of Iosefka's words (bug_filed / network_help) added to the same
    message when they fit. The message is re-fetched first: what is deleted is what Discord holds now."""
    res, message, rec = await _gate(clinic, message, bug_id, delete, need_export)
    if res:
        return res
    mid = message.id
    if rec.get("bug_id") != bug_id:
        clinic.upsert_message({"message_id": mid, "bug_id": bug_id})
    ch = message.channel
    reopened = None
    try:
        if is_thread(ch) and (ch.archived or ch.locked):
            reopened = (ch.archived, ch.locked)
            await ch.edit(archived=False, locked=False)
        if rec["state"] != "replaced":
            files, left_out = await _media_files(message, rec)
            note = MEDIA_NOTE if left_out else ""
            text = voice.replacement(f"<@{message.author.id}>", bug_id, message.content or "", note)
            more = []
            if extra:
                if len(text) + 2 + len(extra) <= DISCORD_LIMIT:
                    text += "\n\n" + extra
                else:
                    more = _chunks(extra)
            am = AllowedMentions(users=[message.author], everyone=False, roles=False) if ping \
                else AllowedMentions.none()
            # the nonce makes Discord drop a second post of the same replacement (a retry after a lost reply)
            kw = {"allowed_mentions": am, "nonce": str(mid)[-25:]}
            try:
                sent = await ch.send(text, files=files, **kw) if files else await ch.send(text, **kw)
            except Exception as e:
                if not files or _status(e) not in (400, 413):  # only "payload too large" drops the files
                    raise
                text = voice.replacement(f"<@{message.author.id}>", bug_id, message.content or "", MEDIA_NOTE)
                sent = await ch.send(text, **kw)
            clinic.set_state(mid, "replaced", sent.id)
            for part in more:
                await ch.send(part, allowed_mentions=am)
        try:
            await message.delete()
        except Exception as e:
            if type(e).__name__ == "Forbidden":
                clinic.set_state(mid, "delete_forbidden")
                print(f"cannot delete {mid}: Forbidden (the replacement is posted)", file=sys.stderr, flush=True)
                return "delete_forbidden"
            if not _not_found(e):
                raise
        clinic.set_state(mid, "deleted")
        return "deleted"
    finally:
        if reopened:
            try:
                await ch.edit(locked=reopened[1], archived=reopened[0])
            except Exception as e:
                print(f"could not restore thread {ch.id}: {e}", file=sys.stderr, flush=True)


async def replace_run(clinic, messages, bug_id, *, need_export=True, ping=False):
    """Replace a run (consecutive messages of one author about one bug in one place) with ONE note, then
    delete each. Every message passes the same gates as `replace_and_delete`; a blocked message is left out
    and the run splits around it. Returns {message_id: result}. A segment with messages already `replaced`
    reuses their replacement (no second note) and only deletes the rest. The nonce is the first message id."""
    results, segment = {}, []

    async def flush():
        if len(segment) == 1:
            results[segment[0][0].id] = await replace_and_delete(clinic, segment[0][0], bug_id, ping=ping,
                                                                 delete=True, need_export=need_export)
        elif segment:
            results.update(await _run_segment(clinic, list(segment), bug_id, ping))
        segment.clear()

    for message in messages:
        res, message, rec = await _gate(clinic, message, bug_id, True, need_export)
        if res == "deleted":
            results[message.id] = res
        elif res:
            results[message.id] = res
            await flush()
        else:
            segment.append((message, rec))
    await flush()
    return results


async def _run_segment(clinic, seg, bug_id, ping):
    first = seg[0][0]
    ch = first.channel
    rid = next((r["replacement_id"] for _, r in seg if r["state"] == "replaced" and r.get("replacement_id")), None)
    results, reopened = {}, None
    try:
        if is_thread(ch) and (ch.archived or ch.locked):
            reopened = (ch.archived, ch.locked)
            await ch.edit(archived=False, locked=False)
        for m, r in seg:
            if r.get("bug_id") != bug_id:
                clinic.upsert_message({"message_id": m.id, "bug_id": bug_id})
        if rid is None:
            files, shared, left_out = [], [None], False
            for m, r in seg:
                _, lo = await _media_files(m, r, files, shared)
                left_out = left_out or lo
            note = MEDIA_NOTE if left_out else ""
            texts = [m.content or "" for m, _ in seg]
            mention = f"<@{first.author.id}>"
            text = voice.replacement_many(mention, bug_id, texts, note)
            am = AllowedMentions(users=[first.author], everyone=False, roles=False) if ping                 else AllowedMentions.none()
            kw = {"allowed_mentions": am, "nonce": str(first.id)[-25:]}
            try:
                sent = await ch.send(text, files=files, **kw) if files else await ch.send(text, **kw)
            except Exception as e:
                if not files or _status(e) not in (400, 413):
                    raise
                sent = await ch.send(voice.replacement_many(mention, bug_id, texts, MEDIA_NOTE), **kw)
            rid = sent.id
        for m, r in seg:
            if r["state"] != "replaced" or r.get("replacement_id") != rid:
                clinic.set_state(m.id, "replaced", rid)
        for m, _ in seg:
            try:
                await m.delete()
            except Exception as e:
                if type(e).__name__ == "Forbidden":
                    clinic.set_state(m.id, "delete_forbidden")
                    print(f"cannot delete {m.id}: Forbidden (the replacement is posted)", file=sys.stderr, flush=True)
                    results[m.id] = "delete_forbidden"
                    continue
                if not _not_found(e):
                    raise
            clinic.set_state(m.id, "deleted")
            results[m.id] = "deleted"
        return results
    finally:
        if reopened:
            try:
                await ch.edit(locked=reopened[1], archived=reopened[0])
            except Exception as e:
                print(f"could not restore thread {ch.id}: {e}", file=sys.stderr, flush=True)


# --- guards ----------------------------------------------------------------------------------------------

STILLS_OFF_BYTES = int(2.4 * 2**30)
STILLS_OFF_FREE = 1_000_000_000


def stills_allowed(clinic, path):
    """False when the archive is near its 3 GiB ceiling or the disk is nearly full: videos are then kept
    unstilled (their messages stay on the server) until there is room."""
    return clinic.size_bytes() < STILLS_OFF_BYTES and media.free_disk(path) >= STILLS_OFF_FREE
