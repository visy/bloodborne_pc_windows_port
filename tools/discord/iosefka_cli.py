#!/usr/bin/env python3
"""Iosefka's clinic bug tool for the two Nightwatch devs, run over SSH as a forced command:

  command="/home/visy/netdoctor-venv/bin/python /home/visy/bbport/tools/discord/iosefka_cli.py --as vlad",restrict ssh-ed25519 AAAA... vlad

The command line comes from SSH_ORIGINAL_COMMAND, split with shlex and never given to a shell; unknown
commands are rejected. Output is JSON (add --text for a human summary); `files` and `export` write a tar
to stdout. Nothing is written on the server except database rows. See tools/discord/CLINIC.md.

Standard library only: this must run without discord.py. `status --tell` queues the text in nw_outbox and
the bot posts it.
"""
import argparse
import hashlib
import json
import os
import re
import shlex
import sys
import tarfile
import time
import io
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import clinic_changes  # noqa: E402
import clinic_health  # noqa: E402
import clinic_vpn  # noqa: E402
import clinic_outbox  # noqa: E402
import clinic_store  # noqa: E402
import iosefka_cl  # noqa: E402
from clinic_logic import mask_ips  # noqa: E402

STATUSES = ("new", "open", "analysing", "waiting-logs", "fixed", "closed", "duplicate")
CLOSED = ("fixed", "closed", "duplicate")
VISY_ONLY = ("export", "confirm-export", "import")
DEVS_ONLY = ("vpn-profile",)  # how to enroll on the Nightwatch VPN (address + pin): only the two devs
DEVS = ("visy", "vlad")
BUG_RX = re.compile(r"^NW-[0-9a-f]{7}$")
ND_RX = re.compile(r"^ND-[0-9a-f]{8}$")
NAME_RX = re.compile(r"^[a-z0-9_-]{1,32}$")
KEY_RX = re.compile(r"^[a-z][a-z0-9_-]{0,31}$")
LOG_DIR_KINDS = ("log", "dump", "game", "save", "archive", "text", "other")
MEDIA_KINDS = ("image", "still")
SORTS = ("newest", "oldest", "activity")


class CliError(Exception):
    def __init__(self, msg, code=2):
        super().__init__(msg)
        self.code = code



class Parser(argparse.ArgumentParser):
    def error(self, message):
        raise CliError(message)

    def exit(self, status=0, message=None):
        raise CliError(message or "", status)


def _filters():
    p = Parser(add_help=False, allow_abbrev=False)
    p.add_argument("--status", action="append", default=[])
    p.add_argument("--open", action="store_true")
    p.add_argument("--tag", action="append", default=[])
    p.add_argument("--q")
    p.add_argument("--since")
    p.add_argument("--assignee")
    p.add_argument("--older-than", dest="older_than")
    p.add_argument("--idle")
    p.add_argument("--sort", default="newest")
    return p


def build_parser():
    common = Parser(add_help=False, allow_abbrev=False)
    common.add_argument("--text", action="store_true")
    top = Parser(prog="iosefka", allow_abbrev=False)
    sub = top.add_subparsers(dest="cmd", parser_class=lambda **kw: Parser(allow_abbrev=False, **kw))

    def add(name, *parents):
        return sub.add_parser(name, parents=[common, *parents], add_help=False)
    add("new")
    add("triage-queue")
    add("health")
    add("vpn-profile")
    add("vpn-status")
    s = add("title")
    s.add_argument("bug")
    s.add_argument("title_text")
    s = add("move")
    s.add_argument("message_id", type=int)
    s.add_argument("bug")
    add("list", _filters())
    s = add("show")
    s.add_argument("bug")
    g = s.add_mutually_exclusive_group()
    g.add_argument("--full", action="store_true")
    g.add_argument("--brief", action="store_true")
    s = add("files", _filters())
    s.add_argument("bugs", nargs="*")
    s.add_argument("--no-media", action="store_true")
    s = add("note")
    s.add_argument("bug")
    s.add_argument("note_text")
    s = add("status")
    s.add_argument("bug")
    s.add_argument("new_status", choices=STATUSES)
    s.add_argument("--dup")
    s.add_argument("--tell")
    s.add_argument("--in", dest="in_ver", metavar="TRACK:VERSION")
    s = add("ask")
    s.add_argument("bug")
    iosefka_cl.add_ask_args(s)
    s.add_argument("--for", dest="for_item", metavar="CL-ID")
    s = add("tag")
    s.add_argument("bug")
    s.add_argument("pairs", nargs="+")
    s = add("assign")
    s.add_argument("bug")
    s.add_argument("person")
    s = add("share")
    s.add_argument("bug")
    s.add_argument("person")
    s.add_argument("share_text")
    s = add("merge")
    s.add_argument("keep")
    s.add_argument("dup")
    s = add("nd")
    s.add_argument("ref")
    add("export")
    add("confirm-export")
    add("import")
    iosefka_cl.add_parser(sub, common)
    return top


def _iso(t):
    return datetime.fromtimestamp(t, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ") if t else None


ID_TOKEN_RX = re.compile(r"(?i)(?<![0-9a-z])(nw|cl)-([0-9a-f]{6,7})(?![0-9a-z])")


def _norm_ids(arg):
    """NW-/CL- ids in any case ("cl-1CD510", "nw-ABC1234", "--bug=Nw-...") become the stored form."""
    return ID_TOKEN_RX.sub(lambda m: f"{m.group(1).upper()}-{m.group(2).lower()}", arg) if isinstance(arg, str) else arg


def _bug_id(s):
    if not BUG_RX.match(s or ""):
        raise CliError(f"not a bug id (NW-xxxxxxx): {s!r}")
    return s


def _need_bug(c, s):
    b = c.bug(_bug_id(s))
    if not b:
        raise CliError(f"no such bug: {s}", 1)
    return b


def _person(s):
    if not NAME_RX.match(s or ""):
        raise CliError(f"not a name: {s!r}")
    return s


def _excerpt(text, n=160):
    text = " ".join((text or "").split())
    return text if len(text) <= n else text[:n - 1] + "..."


def _summary(c, b):
    notes = c.notes(b["id"])
    last = notes[-1] if notes else None
    return {"id": b["id"], "title": b["title"], "status": b["status"], "tags": b["tags"],
            "assignee": b["assignee"], "reported": _iso(b["reported"]), "filed": _iso(b["created"]),
            "created": _iso(b["created"]), "updated": _iso(b["updated"]),
            "counts": c.counts(b["id"]), "nd_refs": json.loads(b["nd_refs"] or "[]") if isinstance(
                b["nd_refs"], str) else b["nd_refs"], "dup_of": b["dup_of"],
            "last_note": {"who": last["who"], "kind": last["kind"], "at": _iso(last["at"]),
                          "text": _excerpt(last["text"])} if last else None}


CL_NEWS_MAX = 30


def _changelist_news(c, since, who):
    """Changelist items someone else touched since `since`: one entry per item (its latest change), newest
    first; a bulk import (a sync) is folded into a count per track/version so it cannot flood `new`."""
    import clinic_changes
    clinic_changes.Changes(c)  # makes sure the tables exist
    rows = c.db.execute(
        "SELECT n.change_id, n.at, n.who, n.kind, n.text, ch.track, ch.version, ch.status, ch.text "
        "FROM nw_change_notes n JOIN nw_changes ch ON ch.id = n.change_id "
        "WHERE n.at > ? AND n.who != ? ORDER BY n.at DESC, n.rowid DESC", (since, who)).fetchall()
    bulk = {}
    for cid, at, by, kind, note, track, version, *_ in rows:
        if kind in ("import", "add"):
            bulk.setdefault((by, track, version), set()).add(cid)
    folded = {k for k, ids in bulk.items() if len(ids) > 5}  # a sync or an import, not a hand edit
    latest, imports = {}, {}
    for cid, at, by, kind, note, track, version, status, text in rows:
        if kind in ("import", "add") and (by, track, version) in folded:
            imports[(by, track, version)] = len(bulk[(by, track, version)])
            continue
        latest.setdefault(cid, {"id": cid, "track": track, "version": version, "status": status, "text": text,
                                "at": _iso(at), "who": by, "kind": kind, "note": _excerpt(note or "")})
    items = list(latest.values())
    return {"items": items[:CL_NEWS_MAX], "more": max(0, len(items) - CL_NEWS_MAX),
            "imports": [{"who": k[0], "track": k[1], "version": k[2], "items": n} for k, n in imports.items()]}


def _pending(c):
    """Bugs Iosefka filed that wait for a triage pass (tag triage=pending), oldest first."""
    return sorted(c.bugs(tag="triage=pending"), key=lambda b: b["created"])


def _query(c, a):
    since = None
    if a.since:
        try:
            since = datetime.strptime(a.since, "%Y-%m-%d").replace(tzinfo=timezone.utc).timestamp()
        except ValueError:
            raise CliError(f"--since wants YYYY-MM-DD: {a.since!r}")
    for s in a.status:
        if s not in STATUSES:
            raise CliError(f"unknown status {s!r}")
    for t in a.tag:
        if not KEY_RX.match(t.partition("=")[0]):
            raise CliError(f"--tag wants key=value: {t!r}")
    now = time.time()
    older = idle = None
    for attr, name in (("older_than", "--older-than"), ("idle", "--idle")):
        v = getattr(a, attr)
        if v is not None:
            try:
                n = float(v)
            except ValueError:
                n = -1
            if not (n >= 0 and n != float("inf")):
                raise CliError(f"{name} wants a number of days: {v!r}")
            if attr == "older_than":
                older = now - n * 86400
            else:
                idle = now - n * 86400
    if a.sort not in SORTS:
        raise CliError(f"--sort wants one of {', '.join(SORTS)}: {a.sort!r}")
    bugs = c.bugs(status=a.status or None, tag=a.tag or None, q=a.q, since=since,
                  assignee=a.assignee, exclude_status=CLOSED if a.open else None)
    if older is not None:
        bugs = [b for b in bugs if b["reported"] < older]
    if idle is not None:
        bugs = [b for b in bugs if _last_activity(b) < idle]
    if a.sort == "oldest":
        bugs.sort(key=lambda b: (b["reported"], b["id"]))
    elif a.sort == "newest":
        bugs.sort(key=lambda b: (b["reported"], b["id"]), reverse=True)
    elif a.sort == "activity":
        bugs.sort(key=_last_activity, reverse=True)
    return bugs


def _last_activity(b):
    return b["activity"] or b["updated"] or b["created"] or 0


def _has_filter(a):
    return bool(a.status or a.open or a.tag or a.q or a.since or a.assignee or a.older_than is not None
                or a.idle is not None)


def _show(c, b, brief=False):
    msgs = []
    for m in c.messages_for_bug(b["id"]):
        text = m["content"] or ""
        msgs.append({"message_id": m["message_id"], "author": m["author_name"], "at": _iso(m["created"]),
                     "state": m["state"], "content": _excerpt(text, 200) if brief else text,
                     "attachments": [{"sha256": x.get("sha256"), "filename": x.get("filename"),
                                      "kind": x.get("kind"), "size": x.get("size")}
                                     for x in m["attachments"]]})
    notes = [{"at": _iso(n["at"]), "who": n["who"], "kind": n["kind"], "text": n["text"],
              "to": n["to"] or None} for n in c.notes(b["id"])]
    files = [{"sha256": f["sha256"], "filename": f["filename"], "kind": f["kind"], "size": f["raw_size"]}
             | ({"inside": f["parent"], "path": f.get("path") or f["filename"]} if f.get("parent") else {})
             for f in c.files_for_bug(b["id"])]
    return {"bug": _summary(c, b) | {"source": b["source"], "channel_id": b["channel_id"],
                                     "thread_id": b["thread_id"]},
            "notes": notes, "messages": msgs, "files": files}


def _safe_name(name):
    name = re.sub(r"[^A-Za-z0-9._() -]", "_", os.path.basename((name or "file").replace("\\", "/")))
    return name.strip(". ") or "file"


def _add_bytes(tar, name, data, mtime=0):
    ti = tarfile.TarInfo(name)
    ti.size = len(data)
    ti.mtime = int(mtime or time.time())
    ti.mode = 0o600
    tar.addfile(ti, io.BytesIO(data))


def _write_bug_tar(c, bug_ids, out, media):
    with tarfile.open(fileobj=out, mode="w|") as tar:
        for bid in bug_ids:
            b = c.bug(bid)
            base = bid
            _add_bytes(tar, f"{base}/bug.json", json.dumps(_show(c, b), indent=2, ensure_ascii=False).encode(),
                       b["created"])
            for f in c.files_for_bug(bid):
                if f["kind"] in MEDIA_KINDS:
                    if not media:
                        continue
                    folder = "images" if f["kind"] == "image" else "stills"
                elif f.get("parent"):  # a log (or save) from inside an archive: logs|saves/<archive name>/<path>
                    inner = "/".join(_safe_name(p) for p in (f.get("path") or f["filename"]).split("/") if p.strip(". "))
                    name, data, _codec = c.get_file(f["sha256"])
                    top = "saves" if f["kind"] == "save" else "logs"
                    _add_bytes(tar, f"{base}/{top}/{_safe_name(f['parent'])}/{inner or _safe_name(name)}", data,
                               b["created"])
                    continue
                else:
                    folder = "saves" if f["kind"] == "save" else "logs"
                name, data, _codec = c.get_file(f["sha256"])
                _add_bytes(tar, f"{base}/{folder}/{f['sha256'][:8]}_{_safe_name(name)}", data, b["created"])


def _health(c):
    """What the bot recorded about itself plus this machine's state. Error lines are IP-masked."""
    snap = clinic_health.Health(c).snapshot(with_versions=True)
    snap["now"] = _iso(snap["now"])
    for k in ("last_start", "last_tick"):
        snap[k] = _iso(snap[k]) if snap[k] else None
    for st in snap["jobs"].values():
        st["at"] = _iso(st["at"]) if st["at"] else None
        st["age"] = None if st["age"] is None else round(st["age"])
    for e in snap["errors_24h"]["kinds"]:
        e["first_line"] = mask_ips(e["first_line"])
        e["last"] = _iso(e["last"])
    at, detail = snap.pop("upkeep")
    snap["upkeep"] = {"at": _iso(at) if at else None, "result": detail}
    return snap


def _health_text(d):
    lines = [f"last start {d['last_start']}, {d['restarts_24h']} start(s) in 24 h, last tick {d['last_tick']}"]
    for j, st in d["jobs"].items():
        lines.append(f"  job {j}: last {st['at']} ({st['age']} s ago){' OVERDUE' if st['overdue'] else ''}")
    lines.append(f"errors 24 h: {d['errors_24h']['count']}")
    for e in d["errors_24h"]["kinds"]:
        lines.append(f"  {e['count']}x [{e['place']}] {e['first_line']}")
    db, q = d["db"], d["queues"]
    lines.append(f"disk free {d['disk_free'] / 2**30:.1f} GB; db {db['size'] / 2**20:.0f} of {db['cap'] / 2**20:.0f} MiB "
                 f"({db['fraction'] * 100:.0f}%)")
    lines.append(f"queues: triage {q['triage']}, unsure {q['unsure']}, uploads waiting {q['uploads']}; "
                 f"24 h: {d['new_bugs_24h']} new bugs, {d['logs_24h']} logs")
    lines.append(clinic_vpn.line(d.get("vpn") or {"state": "off"}))
    lines.append("open alerts: " + (", ".join(d["alerts_active"]) or "none"))
    v = d["versions"]
    lines.append(f"git {v['git']}, discord.py {v['discord.py']}, python {v['python']}, tools "
                 + ", ".join(f"{k} {'yes' if ok else 'MISSING'}" for k, ok in v["tools"].items()))
    return lines


def _json_out(out, data):
    out.write((json.dumps(data, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))


def _text_out(out, cmd, data):
    lines = []
    if cmd == "cl" and isinstance(data, list) and all("section" in x for x in data):
        for i in data:
            lines.append(f"{i['id']}  {i['track']} {i['version']}  {i['status']}  {i['section']}  "
                         f"{(i.get('dated') or '-')[:10]}  {i['text']}")
    elif cmd in ("list", "new", "triage-queue") and "bugs" in data or cmd == "list":
        bugs = data if isinstance(data, list) else data["bugs"]
        for b in bugs:
            k = b["counts"]
            lines.append(f"{b['id']}  {b['status']:<12} {(b['reported'] or '-')[:10]}  {b['title']}  "
                         f"[{k['messages']} msg, {k['logs']} log, {k['images']} img, {k['stills']} still]")
        if cmd == "triage-queue":
            for n in data["auto_links"]:
                lines.append(f"  auto-link {n['bug_id']}: {_excerpt(n['text'])}")
        if cmd == "new":
            for b in data["updated_bugs"]:
                k = b["counts"]
                lines.append(f"  updated {b['id']}  {b['status']:<12} {b['title']}  "
                             f"[{k['messages']} msg, {k['logs']} log, {k['images']} img]")
            lines.append(f"  triage pending: {data['triage_pending']}")
            cl = data.get("changelist") or {}
            for i in cl.get("items", []):
                lines.append(f"  changelist {i['id']}  {i['track']} {i['version']}  {i['status']}  {i['text'][:80]}"
                             f"  ({i['who']} {i['kind']})")
            if cl.get("more"):
                lines.append(f"  changelist: {cl['more']} more items changed")
            for imp in cl.get("imports", []):
                lines.append(f"  changelist import by {imp['who']}: {imp['items']} items in {imp['track']} {imp['version']}")
            for n in data["notes"]:
                lines.append(f"  note {n['bug_id']} {n['who']} ({n['kind']}): {_excerpt(n['text'])}")
    elif cmd in ("ask", "cl") and isinstance(data, dict) and "asks" in data:
        lines.append("DRY RUN: nothing queued (add --send)" if data["dry_run"] else "QUEUED")
        for p in data["asks"]:
            lines.append(f"{p['bug']}: issue: {p.get('issue')!r}" + (f"  BLOCKED: {p['blocked']}" if p.get("blocked") else ""))
            for m in p["messages"]:
                lines.append(f"  -> {m['where']} (record {m['record']}, {m['chars']} chars):")
                lines += ["     " + x for x in m["text"].splitlines()]
            for sk in p.get("skipped", []):
                lines.append(f"  skipped {sk['record']}: {sk['why']}")
    elif cmd == "health":
        lines = _health_text(data)
    elif cmd == "show":
        b = data["bug"]
        lines.append(f"{b['id']}  {b['status']}  {b['title']}")
        lines.append(f"reported: {b['reported']}  filed: {b['filed']}")
        lines.append(f"tags: {json.dumps(b['tags'])}  assignee: {b['assignee']}")
        for n in data["notes"]:
            lines.append(f"  [{n['at']}] {n['who']} {n['kind']}: {n['text']}")
        for m in data["messages"]:
            lines.append(f"  <{m['author']} {m['at']}> {m['content']}")
        for f in data["files"]:
            where = f" (in {f['inside']}: {f['path']})" if f.get("inside") else ""
            lines.append(f"  file {f['kind']} {f['filename']} {f['sha256'][:8]}{where}")
    else:
        lines.append(json.dumps(data, indent=2, ensure_ascii=False))
    out.write(("\n".join(lines) + "\n").encode("utf-8"))


def _tag_pairs(pairs):
    tags = {}
    for p in pairs:
        k, eq, v = p.partition("=")
        if not eq or not KEY_RX.match(k):
            raise CliError(f"tag wants key=value: {p!r}")
        tags[k] = None if v == "" else ([x for x in v.split(",") if x] if "," in v else v)
    return tags


def _classify(name):
    n = name.lower()
    ext = n.rsplit(".", 1)[-1] if "." in n else ""
    if ext in ("png", "jpg", "jpeg", "webp", "gif", "bmp"):
        return "image"
    if ext in ("log", "txt"):
        return "log"
    if ext == "dmp":
        return "dump"
    if ext in ("zip", "7z", "rar"):
        return "archive"
    if n.startswith("userdata") or ext == "dcx":
        return "game"
    return "other"


def _safe_member(name):
    parts = name.replace("\\", "/").split("/")
    return bool(name) and not name.startswith("/") and ".." not in parts and ":" not in parts[0]


def _expected_hash(c, sha):
    got = c.get_file(sha)
    if not got:
        return None
    if got[2] in ("lzma", "orig"):
        return sha
    return hashlib.sha256(got[1]).hexdigest()


def _do_export(c, out):
    files = c.all_files()
    export_id = "EX-" + hashlib.sha256(str(time.time()).encode()).hexdigest()[:8]
    manifest = {"export_id": export_id, "files": [
        {"sha256": f["sha256"], "filename": f["filename"], "kind": f["kind"], "codec": f["codec"]}
        for f in files]}
    archive = {"bugs": c.bugs(), "notes": {b["id"]: c.notes(b["id"]) for b in c.bugs()},
               "messages": c.all_messages()}
    with tarfile.open(fileobj=out, mode="w|") as tar:
        _add_bytes(tar, "manifest.json", json.dumps(manifest, indent=2).encode())
        _add_bytes(tar, "archive.json", json.dumps(archive, indent=2, ensure_ascii=False).encode())
        for f in files:
            _add_bytes(tar, f"files/{f['sha256']}", c.get_file(f["sha256"])[1])
    c.record_export(export_id, [f["sha256"] for f in files])


def _do_confirm(c, stdin, out):
    try:
        data = json.loads(stdin.read().decode("utf-8"))
        export_id, entries = data["export_id"], data["files"]
    except (ValueError, KeyError, TypeError):
        raise CliError("confirm-export wants JSON {export_id, files: [{sha256, rehash}]} on stdin")
    good, bad = [], []
    for e in entries:
        sha = e.get("sha256")
        (good if sha and e.get("rehash") == _expected_hash(c, sha) else bad).append(sha)
    _json_out(out, {"export_id": export_id, "confirmed": c.confirm_export(export_id, good), "rejected": bad})


def _do_import(c, who, stdin, out):
    members = {}
    try:
        with tarfile.open(fileobj=stdin, mode="r|*") as tar:
            for m in tar:
                if m.isfile():
                    if not _safe_member(m.name):
                        raise CliError(f"unsafe name in tar: {m.name!r}")
                    members[m.name.replace("\\", "/")] = tar.extractfile(m).read()
    except tarfile.TarError as e:
        raise CliError(f"not a tar: {e}")
    done = []
    for name in sorted(n for n in members if n.endswith("case.json")):
        folder = name[:-len("case.json")]
        try:
            case = json.loads(members[name].decode("utf-8"))
            title = str(case["title"])
        except (ValueError, KeyError, TypeError):
            raise CliError(f"bad case.json in {folder or '.'}")
        try:
            reported = clinic_changes.parse_when(case.get("reported"), "reported")
        except ValueError as e:
            raise CliError(f"bad case.json in {folder or '.'}: {e}")
        bug_id = c.new_bug(title, "local", 0, None)
        tags = case.get("tags") or {}
        status = case.get("status")
        c.update_bug(bug_id, tags=tags, **({"status": status} if status in STATUSES else {}))
        names = case.get("files") or [n[len(folder):] for n in members if n.startswith(folder)
                                       and n != name and "/" not in n[len(folder):]]
        atts = []
        for fn in names:
            if f"{folder}{fn}" not in members:
                raise CliError(f"case.json lists {fn!r} but the tar lacks it")
            kind = _classify(fn)
            data = members[f"{folder}{fn}"]
            sha = c.put_file(data, _safe_name(fn), kind)
            atts.append({"sha256": sha, "filename": _safe_name(fn), "kind": kind, "size": len(data)})
        low = c.db.execute("SELECT MIN(message_id) FROM nw_messages").fetchone()[0] or 0
        c.upsert_message({"message_id": min(low, 0) - 1, "author_name": "local", "created": reported or time.time(),
                          "content": str(case.get("text", "")), "attachments": atts, "bug_id": bug_id,
                          "state": "kept"})
        for n in case.get("notes") or []:
            n = {"text": n} if isinstance(n, str) else n
            c.add_note(bug_id, n.get("who", who), n.get("kind", "note"), str(n["text"]))
        done.append({"id": bug_id, "folder": folder.rstrip("/"), "files": len(atts)})
    _json_out(out, {"imported": done})


def _vpn_profile():
    """The Nightwatch VPN enrollment note (server address, HTTPS pin, the enroll command) as bytes. Kept on the
    server only (mode 600), never in the repo. Since nwsign every install enrolls its own key: no shared profile."""
    p = Path(os.environ.get("NETDOCTOR_VPN_PROFILE") or Path.home() / ".config" / "netdoctor" / "nightwatch-vpn.txt")
    try:
        return p.read_bytes()
    except OSError:
        raise CliError("no VPN profile on the server", 1)


def main(argv, who, out=None, *, db=None, stdin=None):
    """Run one command; returns the exit status (0 ok, 1 not found / refused, 2 bad command line)."""
    out = out or sys.stdout.buffer
    stdin = stdin or sys.stdin.buffer
    try:
        return _run(argv, who, out, db, stdin)
    except CliError as e:
        if str(e):
            print(f"iosefka: {e}", file=sys.stderr)
        return e.code


def _run(argv, who, out, db, stdin):
    if not NAME_RX.match(who or ""):
        raise CliError("--as wants a short lowercase name")
    a = build_parser().parse_args([_norm_ids(x) for x in argv])
    if not a.cmd:
        raise CliError("no command (new, triage-queue, health, title, move, list, show, files, note, status, tag, assign, share, merge, nd, cl, vpn-profile, vpn-status)")
    if a.cmd in VISY_ONLY and who != "visy":
        raise CliError(f"{a.cmd} is for visy only", 1)
    if a.cmd in DEVS_ONLY and who not in DEVS:
        raise CliError(f"{a.cmd} is for the devs only", 1)
    if a.cmd == "nd" and not ND_RX.match(a.ref):
        raise CliError(f"not an analysis ref (ND-xxxxxxxx): {a.ref!r}")
    if a.cmd == "files" and not (a.bugs or _has_filter(a)):
        raise CliError("files wants bug ids or filters (--open, --tag, --status, --q, --since, --assignee)")
    for b in getattr(a, "bugs", None) or []:
        _bug_id(b)
    for attr in ("bug", "keep", "dup"):
        if isinstance(getattr(a, attr, None), str):
            _bug_id(getattr(a, attr))
    if a.cmd == "assign" and a.person != "-":
        _person(a.person)
    if a.cmd == "share":
        _person(a.person)
    if a.cmd == "status" and a.dup and a.new_status != "duplicate":
        raise CliError("--dup only goes with the status duplicate")
    if a.cmd == "tag":
        tags = _tag_pairs(a.pairs)
    if a.cmd == "status" and a.in_ver:
        if a.new_status != "fixed":
            raise CliError("--in only goes with the status fixed")
        try:
            iosefka_cl.parse_in(a.in_ver)
        except iosefka_cl.ClError as e:
            raise CliError(str(e), e.code)

    db_path = db or os.environ.get("NETDOCTOR_DB") or Path.home() / ".config" / "netdoctor" / "netdoctor.db"
    c = clinic_store.Clinic(db_path)
    text = getattr(a, "text", False)

    def emit(data):
        if isinstance(data, bytes):  # raw output (cl notes markdown), also with --text
            out.write(data)
            return 0
        _text_out(out, a.cmd, data) if text else _json_out(out, data)
        return 0

    if a.cmd == "cl":
        try:
            return iosefka_cl.run(a, c, who, emit, stdin)
        except iosefka_cl.ClError as e:
            raise CliError(str(e), e.code)
    if a.cmd == "new":
        since = c.mark_seen(who)
        got = c.changes_since(since, who)
        notes = [{"bug_id": n["bug_id"], "at": _iso(n["at"]), "who": n["who"], "kind": n["kind"],
                  "text": n["text"]} for n in got["notes"] if n["who"] != who]
        return emit({"since": _iso(since), "bugs": [_summary(c, b) for b in got["bugs"]],
                     "updated_bugs": [_summary(c, b) for b in got["updated_bugs"]], "notes": notes,
                     "changelist": _changelist_news(c, since, who), "triage_pending": len(_pending(c))})
    if a.cmd == "health":
        return emit(_health(c))
    if a.cmd == "triage-queue":
        since = c.mark_seen(who, "triage")
        links = [{"bug_id": n["bug_id"], "at": _iso(n["at"]), "text": n["text"]}
                 for n in c.notes_of_kind("auto-link", since)]
        return emit({"since": _iso(since), "bugs": [_summary(c, b) for b in _pending(c)], "auto_links": links})
    if a.cmd == "title":
        b = _need_bug(c, a.bug)
        new = mask_ips(a.title_text).strip()[:120]
        if not new:
            raise CliError("title wants some text")
        c.update_bug(a.bug, title=new)
        c.add_note(a.bug, who, "note", f"title: {b['title']!r} -> {new!r}")
        return emit({"ok": True, "bug": a.bug, "title": new})
    if a.cmd == "move":
        _need_bug(c, a.bug)
        old = c.move_message(a.message_id, a.bug)
        if old is False:
            raise CliError(f"no such message: {a.message_id}", 1)
        c.touch_bug(a.bug)
        note = f"moved message {a.message_id} from {old or 'no bug'}"
        c.add_note(a.bug, who, "note", note)
        if old and old != a.bug and c.bug(old):
            c.add_note(old, who, "note", f"moved message {a.message_id} to {a.bug}")
        return emit({"ok": True, "message": a.message_id, "from": old, "to": a.bug})
    if a.cmd == "list":
        return emit([_summary(c, b) for b in _query(c, a)])
    if a.cmd == "show":
        return emit(_show(c, _need_bug(c, a.bug), brief=a.brief))
    if a.cmd == "files":
        ids = a.bugs if a.bugs else [b["id"] for b in reversed(_query(c, a))]
        for i in ids:
            _need_bug(c, i)
        _write_bug_tar(c, ids, out, media=not a.no_media)
        return 0
    if a.cmd == "note":
        _need_bug(c, a.bug)
        c.add_note(a.bug, who, "note", a.note_text)
        return emit({"ok": True, "bug": a.bug})
    if a.cmd == "status":
        _need_bug(c, a.bug)
        fields = {"status": a.new_status}
        if a.dup:
            _need_bug(c, a.dup)
            fields["dup_of"] = a.dup
        change = None
        if a.in_ver:  # before the bug changes, so a refused item leaves everything as it was
            try:
                change = iosefka_cl.fixed_in(c, who, a.bug, a.in_ver)
            except iosefka_cl.ClError as e:
                raise CliError(str(e), e.code)
        c.update_bug(a.bug, **fields)
        c.add_note(a.bug, who, "status", a.new_status + (f" (duplicate of {a.dup})" if a.dup else "")
                   + (f" in {a.in_ver}" if a.in_ver else ""))
        res = {"ok": True, "bug": a.bug, "status": a.new_status}
        if change:
            res["change"] = change
        if a.tell:
            masked = mask_ips(a.tell)
            pv = clinic_outbox.preview(c, a.bug, "tell", masked)
            res["tell_queued"] = c.queue_tell(a.bug, who, masked)
            res["tell_text"] = masked
            if pv["merge_with"]:
                res["tell_merge_with"] = pv["merge_with"]
            if pv["already_told"]:
                res["tell_warning"] = clinic_outbox.describe({"merge_with": [], "already_told": pv["already_told"]})[0]
            c.add_note(a.bug, who, "triage", "told the player: " + masked)
        return emit(res)
    if a.cmd == "ask":
        b = _need_bug(c, a.bug)
        default = b["title"]
        if a.for_item:
            if not clinic_changes.CL_RX.match(a.for_item):
                raise CliError(f"not a changelist item id (CL-xxxxxx): {a.for_item!r}")
            it = clinic_changes.Changes(c).get(a.for_item)
            if it is None:
                raise CliError(f"no such item {a.for_item}", 1)
            default = it["text"]
        try:
            res = iosefka_cl.do_ask(c, who, [a.bug], a, default)
        except iosefka_cl.ClError as e:
            raise CliError(str(e), e.code)
        if a.send and res["asks"][0]["blocked"]:
            raise CliError(res["asks"][0]["blocked"], 1)
        return emit(res)
    if a.cmd == "tag":
        _need_bug(c, a.bug)
        c.update_bug(a.bug, tags=tags)
        c.add_note(a.bug, who, "tag", " ".join(a.pairs))
        return emit({"ok": True, "bug": a.bug, "tags": c.bug(a.bug)["tags"]})
    if a.cmd == "assign":
        _need_bug(c, a.bug)
        person = None if a.person == "-" else a.person
        c.update_bug(a.bug, assignee=person)
        c.add_note(a.bug, who, "note", f"assigned to {person}" if person else "unassigned")
        return emit({"ok": True, "bug": a.bug, "assignee": person})
    if a.cmd == "share":
        _need_bug(c, a.bug)
        c.add_note(a.bug, who, "share", a.share_text, to=a.person)
        return emit({"ok": True, "bug": a.bug, "to": a.person})
    if a.cmd == "merge":
        if a.keep == a.dup:
            raise CliError("merge wants two different bugs")
        _need_bug(c, a.keep)
        _need_bug(c, a.dup)
        c.merge_bugs(a.keep, a.dup)
        return emit({"ok": True, "kept": a.keep, "duplicate": a.dup})
    if a.cmd == "nd":
        import netdoctor_store
        row = netdoctor_store.Store(c.path).get_analysis(a.ref)
        if not row:
            raise CliError(f"no such analysis: {a.ref}", 1)
        row["created"] = _iso(row["created"])
        return emit(row)
    if a.cmd == "vpn-profile":
        out.write(_vpn_profile())
        return 0
    if a.cmd == "vpn-status":   # the same text /vpnstatus posts (a live sample, stored like the health loop's)
        target = clinic_vpn.ssh_target()
        sample = clinic_vpn.fetch(target) if target else {"ok": False, "error": "not set up"}
        if target and not sample.get("unconfigured"):
            clinic_vpn.store(c, sample)
        out.write(clinic_vpn.status_text(sample, clinic_vpn.summary(c, time.time(), target)) + "\n")
        return 0
    if a.cmd == "export":
        _do_export(c, out)
        return 0
    if a.cmd == "confirm-export":
        _do_confirm(c, stdin, out)
        return 0
    if a.cmd == "import":
        _do_import(c, who, stdin, out)
        return 0
    raise CliError(f"unknown command {a.cmd!r}")


def entry(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    who = None
    if "--as" in argv:
        i = argv.index("--as")
        who = argv[i + 1] if i + 1 < len(argv) else None
        del argv[i:i + 2]
    forced = os.environ.get("SSH_ORIGINAL_COMMAND")
    if forced is not None:
        try:
            argv = shlex.split(forced)
        except ValueError as e:
            print(f"iosefka: bad quoting: {e}", file=sys.stderr)
            return 2
    if who is None:
        print("iosefka: missing --as <name>", file=sys.stderr)
        return 2
    return main(argv, who)


if __name__ == "__main__":
    sys.exit(entry())
