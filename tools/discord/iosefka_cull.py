#!/usr/bin/env python3
"""Iosefka's one-shot cull runner. Run by hand on the bot's server, one phase at a time; every phase can
be stopped and rerun (all state lives in the clinic database).

  iosefka_cull.py harvest                      store every file and message of the server (read-only on Discord)
  iosefka_cull.py verify                       re-check every stored file; exit 1 on any failure
  iosefka_cull.py unpack                       backfill: logs inside stored zip/rar/7z files (nested too) become own files
  iosefka_cull.py report --out PATH            dry run: what the cull would replace (no deletions)
  iosefka_cull.py starter-test --thread ID     delete the starter of a test post, check the post survives
  iosefka_cull.py cull [--limit N]             replace + delete the messages holding logs (needs NW_DELETE=1)
  iosefka_cull.py announce --channels a,b [--everyone a] [--dry-run] --yes   post and pin Iosefka's announcement

Exit codes: 0 ok, 1 failure, 2 refused (missing flag), 3 disk low (rerun later).
Environment as the bot (NETDOCTOR_TOKEN, NETDOCTOR_DB, NETDOCTOR_GUILD; ~/.config/netdoctor/env is read).
NW_DELETE=1 is required for `cull` and `starter-test`. NW_TMP: where downloads go (default
~/.cache/iosefka); one file at a time, deleted right after it is stored.
"""
import argparse
import asyncio
import importlib.util
import lzma
import os
import shutil
import tempfile
import sys
import time
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import clinic_discord as cd  # noqa: E402
import clinic_store  # noqa: E402

voice, logic = cd.voice, cd.logic

OK, FAIL, REFUSED, DISK_LOW = 0, 1, 2, 3


def load_env():
    env = Path.home() / ".config" / "netdoctor" / "env"
    if env.is_file():
        for line in env.read_text().splitlines():
            if "=" in line and not line.lstrip().startswith("#"):
                k, v = line.split("=", 1)
                os.environ.setdefault(k.strip(), v.strip())


def tmp_root():
    return Path(os.environ.get("NW_TMP") or Path.home() / ".cache" / "iosefka")


# --- harvest ---------------------------------------------------------------------------------------------

async def _harvest_place(clinic, place, log_info_fn, tmpdir, out):
    msgs = []
    try:
        async for m in place.history(limit=None, oldest_first=True):
            if not getattr(m.author, "bot", False):
                msgs.append(m)
    except Exception as e:
        if type(e).__name__ != "Forbidden":
            raise
        out(f"no access: {getattr(place, 'name', place.id)}")
        return 0, 0, 0, 1
    files = failed = 0
    infos_by_id, lights = {}, []
    stored = {}
    for m in msgs:
        if not cd.all_attachments(m):
            continue
        res = await cd.archive_message(clinic, m, log_info_fn=log_info_fn, tmpdir=tmpdir,
                                       stills=cd.stills_allowed(clinic, tmpdir))
        files += len(cd.all_attachments(m))
        failed += res["state"] == "archive_failed"
        infos_by_id[m.id] = res["infos"]
        stored[m.id] = res
    if cd.is_thread(place):
        forum = hasattr(getattr(place, "parent", None), "available_tags")
        if forum or any(r["removable"] for r in stored.values()):
            bug = cd.ensure_thread_bug(clinic, place, msgs[0].author.id if msgs else None)
            for m in msgs:
                if m.id in stored:
                    clinic.upsert_message({"message_id": m.id, "bug_id": bug})
                else:
                    cd.store_text(clinic, m, bug)
    else:
        for m in msgs:
            r = stored.get(m.id)
            if r and r["removable"] and cd.thread_of(m) is not None:
                await cd.assign_bug(clinic, m, r["infos"])  # a thread's starter lives in the parent channel
                continue
            lights.append(cd.light(m, bool(r and r["removable"])))
        if any(x["has_log"] for x in lights):
            cd.assign_clusters(clinic, lights, infos_by_id)
    if msgs:
        clinic.set_watermark(place.id, msgs[-1].id)
    out(f"place {getattr(place, 'name', place.id)}: {len(msgs)} msgs, {files} files"
        + (f", {failed} failed" if failed else ""))
    return len(msgs), files, failed, 0


async def harvest(guilds, clinic, *, log_info_fn, tmpdir, out=print):
    total = [0, 0, 0, 0]
    try:
        for guild in guilds:
            async for place in cd.walk_places(guild):
                n = await _harvest_place(clinic, place, log_info_fn, tmpdir, out)
                total = [a + b for a, b in zip(total, n)]
    except cd.DiskLow:
        out(f"disk low, resume later (stopped after {total[0]} msgs, {total[1]} files)")
        return DISK_LOW
    out(f"harvest done: {total[0]} msgs, {total[1]} files, {total[2]} archive_failed (rerun retries them), "
        f"{total[3]} places without access; {len(clinic.bugs())} bugs")
    return OK


# --- verify, report --------------------------------------------------------------------------------------

def verify(clinic, out=print):
    bad, kinds = [], {}
    files = clinic.all_files()
    for f in files:
        kinds[f["kind"]] = kinds.get(f["kind"], 0) + 1
        if not clinic.verify_file(f["sha256"]):
            bad.append(f"file {f['sha256'][:12]} {f['filename']}")
    msgs = clinic.all_messages()
    for m in msgs:
        if m["state"] in ("verified", "replaced", "deleted") and not all(cd.att_verified(clinic, a)
                                                                         for a in m["attachments"]):
            bad.append(f"message {m['message_id']} state {m['state']} has an unverified attachment")
    out(f"messages {len(msgs)}, files {len(files)} ({', '.join(f'{k} {v}' for k, v in sorted(kinds.items()))}), "
        f"bugs {len(clinic.bugs())}, failed messages {sum(m['state'] == 'archive_failed' for m in msgs)}")
    for line in bad:
        out("FAIL " + line)
    out("verify: " + ("FAILED" if bad else "ok"))
    return FAIL if bad else OK


def _blob_to_file(clinic, sha, dest):
    """Write a stored file's bytes to `dest` (LZMA rows decompressed chunk by chunk). False when missing."""
    row = clinic.db.execute("SELECT blob, codec FROM nw_files WHERE sha256=?", (sha,)).fetchone()
    if not row or row[0] is None:
        return False
    blob = bytes(row[0])
    with open(dest, "wb") as f:
        if row[1] != "lzma":
            f.write(blob)
            return True
        dec = lzma.LZMADecompressor()
        for i in range(0, len(blob), 1 << 20):
            f.write(dec.decompress(blob[i:i + (1 << 20)]))
    return True


def unpack(clinic, log_info_fn=None, tmpdir=None, out=print):
    """Backfill: for every stored archive file take its inner logs out of the stored blob, store them as their
    own files, link them to every message attachment holding the archive and record their log_info. An archive
    nobody can read tags its bugs unsure=unreadable-file. Idempotent (done attachments carry `inner_files`).
    Returns OK, or DISK_LOW when the disk is too full to unpack the next archive."""
    tmp = Path(tmpdir or tempfile.gettempdir())
    tmp.mkdir(parents=True, exist_ok=True)
    holders = {}  # archive sha -> [(message, attachment index)]
    for m in clinic.all_messages():
        for i, a in enumerate(m["attachments"]):
            if a.get("kind") == "archive" and a.get("sha256") and "inner_files" not in a:
                holders.setdefault(a["sha256"], []).append((m, i))
    done = unreadable = made = 0
    for sha, held in sorted(holders.items()):
        info = clinic.file_info(sha)
        if not info:
            continue
        if not cd.media.enough_disk(tmp, int(info["raw_size"] or 0)):
            out(f"disk low: stopping before {sha[:12]}, rerun later")
            return DISK_LOW
        d = Path(tempfile.mkdtemp(prefix="unpack_", dir=str(tmp)))
        try:
            path = d / "archive.bin"
            if not _blob_to_file(clinic, sha, path):
                continue
            listing, inner = cd._prep_archive(clinic.prepare_file, path)
        except Exception as e:
            out(f"cannot unpack {sha[:12]}: {type(e).__name__}: {e}")
            continue
        finally:
            shutil.rmtree(d, ignore_errors=True)
        recs = {}
        for m, i in held:
            recs.setdefault(m["message_id"], (m, []))[1].append(i)
        for mid, (m, idxs) in recs.items():
            atts = m["attachments"]
            for i in idxs:
                cd.store_inner(clinic, atts[i], listing, inner)
                if log_info_fn is not None:
                    cd.log_infos(clinic, [atts[i]], log_info_fn, tmp)
            clinic.upsert_message({"message_id": mid, "attachments": atts})
            if listing is None and m.get("bug_id"):
                clinic.add_unsure(m["bug_id"], ["unreadable-file"])
        done += 1
        made += len(inner)
        unreadable += listing is None
    out(f"unpack: {done} archive(s), {made} inner file(s), {unreadable} unreadable")
    return OK


def cullable(clinic):
    """Messages the cull may replace and delete: stored, holding a log, and never a DM."""
    return [m for m in clinic.messages_in_state(("verified", "replaced"))
            if cd.rec_removable(m["attachments"]) and not m.get("dm")]


CHANNEL_RUN_GAP_S = 300


def cull_runs(clinic):
    """The cullable messages grouped into runs: consecutive (by id, one place) messages of one author for
    one bug, with no stored message of anyone else between them. Oldest first; a lone message is a run of 1."""
    todo = cullable(clinic)
    if not todo:
        return []
    stored, mine = {}, {}
    for m in clinic.all_messages():
        stored.setdefault(m["thread_id"] or m["channel_id"], []).append(m)
    for rec in todo:
        mine.setdefault(rec["thread_id"] or rec["channel_id"], []).append(rec)
    runs = []
    for place, recs in mine.items():
        cur = None
        for rec in recs:  # oldest id first
            key = (rec["author_id"], rec.get("bug_id"))
            # plain channels keep only messages with files, so other people's chat in between is unknown:
            # there a run also needs the messages to be close in time
            near = bool(rec["thread_id"]) or cur is None or \
                (rec.get("created") or 0) - (cur[1][-1].get("created") or 0) <= CHANNEL_RUN_GAP_S
            if cur is not None and key[1] and cur[0] == key and near and not any(
                    o["author_id"] != rec["author_id"]
                    and cur[1][-1]["message_id"] < o["message_id"] < rec["message_id"] for o in stored[place]):
                cur[1].append(rec)
                continue
            cur = (key, [rec])
            runs.append(cur[1])
    return sorted(runs, key=lambda r: r[0]["message_id"])


def report(clinic, path, resolve_name=None):
    lines = ["Iosefka cull report (dry run): messages that would be replaced and deleted", ""]
    runs = cull_runs(clinic)
    for run_ in runs:
        m = run_[0]
        bug = clinic.bug(m["bug_id"]) if m.get("bug_id") else None
        place = (resolve_name(m["thread_id"] or m["channel_id"]) if resolve_name else None) or             (bug["title"] if bug else "?")
        ids = ", ".join(str(r["message_id"]) for r in run_)
        if len(run_) > 1:
            lines.append(f"{m['bug_id'] or 'NO-BUG'}  place: {place}  run of {len(run_)} messages: {ids}  "
                         f"state {'/'.join(sorted({r['state'] for r in run_}))}")
        else:
            lines.append(f"{m['bug_id'] or 'NO-BUG'}  place: {place}  message {m['message_id']}  state {m['state']}")
        lines.append("  files: " + ", ".join(a["filename"] for r in run_ for a in r["attachments"]))
        lines.append("  would post:")
        lines += ["    " + ln for ln in voice.replacement_many(
            f"<@{m['author_id']}>", m["bug_id"] or "NW-???????", [r["content"] or "" for r in run_]).splitlines()]
        lines.append("")
    lines.append(f"{sum(len(r) for r in runs)} message(s)" + (f" in {len(runs)} run(s)" if any(len(r) > 1 for r in runs) else ""))
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")
    return OK


# --- starter test, cull, announce ------------------------------------------------------------------------

async def _place(client, cid):
    return client.get_channel(cid) or await client.fetch_channel(cid)


async def _missing_permissions(client, runs):
    """Names of the places of `runs` where the bot lacks Send / Manage Messages (checked before any post)."""
    bad = set()
    for pid in {r[0]["thread_id"] or r[0]["channel_id"] for r in runs}:
        try:
            place = await _place(client, pid)
        except Exception:
            continue  # gone or unreadable: the run reports it
        guild = getattr(place, "guild", None)
        perms_for = getattr(place, "permissions_for", None)
        if guild is None or perms_for is None or getattr(guild, "me", None) is None:
            continue
        p = perms_for(guild.me)
        is_thread = getattr(place, "parent", None) is not None and hasattr(place, "archived")
        can_send = p.send_messages_in_threads if is_thread else p.send_messages
        if not (p.manage_messages and can_send):
            parent = getattr(place, "parent", None)
            bad.add(f"#{getattr(parent, 'name', None) or getattr(place, 'name', pid)}")
    return bad


async def starter_test(client, thread_id, out=print, env=os.environ):
    if env.get("NW_DELETE") != "1":
        out("starter-test deletes a message: set NW_DELETE=1")
        return REFUSED
    thread = await _place(client, thread_id)
    parent = getattr(thread, "parent", None)
    before = [m.id async for m in thread.history(limit=None, oldest_first=True)]
    try:
        starter = await thread.fetch_message(thread.id)  # forum post: the starter is inside the thread
    except Exception:
        starter = await parent.fetch_message(thread.id)  # thread from a message: the starter is in the parent
    await starter.delete()
    still = None
    try:
        still = await client.fetch_channel(thread_id)
        after = [m.id async for m in still.history(limit=None, oldest_first=True)]
    except Exception:
        after = []
    ok = still is not None and set(before) - {thread.id} <= set(after)
    out(f"replies before {len(before) - 1}, after {len(after)}")
    out("starter-delete keeps thread: " + ("yes" if ok else "no"))
    return OK if ok else FAIL


async def _fetch(client, rec):
    place = await _place(client, rec["thread_id"] or rec["channel_id"])
    return await place.fetch_message(rec["message_id"])


async def cull(client, clinic, *, limit=None, delay=1.0, out=print, env=os.environ):
    if env.get("NW_DELETE") != "1":
        out("cull deletes messages: set NW_DELETE=1 (and run `report` first)")
        return REFUSED
    # a delete Discord refused earlier (missing permission, fixed since): its note is already posted, so the
    # run only deletes now
    for m in clinic.messages_in_state(("delete_forbidden",)):
        clinic.set_state(m["message_id"], "replaced")
    runs = cull_runs(clinic)
    missing = await _missing_permissions(client, runs)
    if missing:
        out("cull refused: Iosefka cannot send/delete in: " + ", ".join(sorted(missing)))
        return FAIL
    done = tried = 0
    for run_ in runs:
        if limit is not None and tried >= limit:
            break
        tried += 1
        if not run_[0].get("bug_id"):
            out(f"{run_[0]['message_id']}: blocked:no-bug")
            continue
        messages = []
        for rec in run_:
            try:
                messages.append(await _fetch(client, rec))
            except Exception as e:
                if type(e).__name__ == "Forbidden":
                    out(f"{rec['message_id']}: no access to its place")
                    continue
                if type(e).__name__ != "NotFound":
                    raise
                clinic.set_state(rec["message_id"], "deleted")  # gone already: after the delete, before the record
                out(f"{rec['message_id']}: deleted (already gone)")
        if not messages:
            continue
        bug_id = run_[0]["bug_id"]
        try:
            results = await cd.replace_run(clinic, messages, bug_id, ping=False, need_export=True)
        except Exception as e:  # one place the bot may not post in must not stop the whole cull
            if type(e).__name__ != "Forbidden":
                raise
            results = {m.id: "send_forbidden" for m in messages}
        for mid, res in results.items():
            out(f"{mid} {bug_id}: {res}")
        n = sum(r == "deleted" for r in results.values())
        done += n
        if any(r in ("delete_forbidden", "send_forbidden") for r in results.values()):
            out(f"cull stopped: Discord refused in place {run_[0]['thread_id'] or run_[0]['channel_id']}")
            out(f"cull: {done} message(s) replaced and deleted")
            return FAIL
        if n:
            await asyncio.sleep(delay)
    out(f"cull: {done} message(s) replaced and deleted")
    return OK


async def announce(guilds, names, *, dry_run, yes, everyone=(), out=print):
    text = voice.announcement()
    if dry_run:
        out(text)
        return OK
    if not yes:
        out("announce posts to the server: add --yes (preview with --dry-run)")
        return REFUSED
    code = OK
    for name in names:
        ch = next((c for g in guilds for c in g.text_channels if c.name == name), None)
        if ch is None:
            out(f"{name}: no such channel")
            code = FAIL
            continue
        ping = name in everyone
        am = cd.AllowedMentions(everyone=True, users=False, roles=False) if ping else cd.AllowedMentions.none()
        if ping:
            try:
                me = next((g.me for g in guilds if ch in g.text_channels), None)
                if me is not None and not ch.permissions_for(me).mention_everyone:
                    out(f"warning: {name}: I lack Mention Everyone, the @everyone will not ping")
            except Exception:
                pass
        msg = await ch.send(("@everyone " if ping else "") + text, allowed_mentions=am)
        await msg.pin()
        if getattr(ch, "is_news", lambda: False)():
            try:
                await msg.publish()
            except Exception:
                pass
        out(f"{name}: {msg.jump_url}")
    return code


# --- entry -----------------------------------------------------------------------------------------------

def _guilds(client):
    gid = os.environ.get("NETDOCTOR_GUILD")
    return [g for g in client.guilds if not gid or str(g.id) == gid]


def _run_client(token, work):
    import discord
    intents = discord.Intents.default()
    intents.message_content = True
    client = discord.Client(intents=intents)
    box = {}

    @client.event
    async def on_ready():
        if "code" in box or box.get("started"):
            return
        box["started"] = True
        try:
            box["code"] = await work(client)
        except Exception:
            traceback.print_exc()
            box["code"] = FAIL
        finally:
            await client.close()
    client.run(token, log_handler=None)
    return box.get("code", FAIL)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="phase", required=True)
    sub.add_parser("harvest")
    sub.add_parser("verify")
    sub.add_parser("unpack")
    r = sub.add_parser("report")
    r.add_argument("--out", required=True)
    s = sub.add_parser("starter-test")
    s.add_argument("--thread", type=int, required=True)
    c = sub.add_parser("cull")
    c.add_argument("--limit", type=int)
    a = sub.add_parser("announce")
    a.add_argument("--channels", required=True)
    a.add_argument("--everyone", default="", help="comma list of those channels whose post starts with @everyone")
    a.add_argument("--dry-run", action="store_true")
    a.add_argument("--yes", action="store_true")
    args = ap.parse_args(argv)

    load_env()
    if args.phase == "announce" and args.dry_run:
        return asyncio.run(announce([], [], dry_run=True, yes=False))
    if args.phase == "cull" and os.environ.get("NW_DELETE") != "1":
        print("cull deletes messages: set NW_DELETE=1 (and run `report` first)")
        return REFUSED
    if args.phase == "announce" and not args.yes:
        print("announce posts to the server: add --yes (preview with --dry-run)")
        return REFUSED
    bot = _load_bot()
    clinic = clinic_store.Clinic(bot.DB_PATH)
    if args.phase == "verify":
        return verify(clinic)
    if args.phase == "unpack":
        nd = bot.load_doctor()
        return unpack(clinic, lambda p: bot.log_info(nd, p), tmp_root())
    token = os.environ.get("NETDOCTOR_TOKEN")
    if args.phase != "report" and not token:
        print("set NETDOCTOR_TOKEN", file=sys.stderr)
        return FAIL
    nd = bot.load_doctor()
    cd.clean_stale_tmp(tmp_root())

    def log_info_fn(p):
        return bot.log_info(nd, p)

    async def work(client):
        guilds = _guilds(client)
        if args.phase == "harvest":
            return await harvest(guilds, clinic, log_info_fn=log_info_fn, tmpdir=tmp_root())
        if args.phase == "starter-test":
            return await starter_test(client, args.thread)
        if args.phase == "cull":
            return await cull(client, clinic, limit=args.limit)
        if args.phase == "announce":
            return await announce(guilds, [x.strip() for x in args.channels.split(",") if x.strip()],
                                  dry_run=False, yes=True,
                                  everyone={x.strip() for x in args.everyone.split(",") if x.strip()})
        return FAIL

    if args.phase == "report":
        return report(clinic, args.out)
    return _run_client(token, work)


def _load_bot():
    spec = importlib.util.spec_from_file_location("netdoctor_bot", HERE / "netdoctor_bot.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


if __name__ == "__main__":
    sys.exit(main())
