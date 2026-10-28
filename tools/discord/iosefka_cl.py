"""`cl` subcommand of Iosefka's clinic CLI: versioned changelists (see clinic_changes.py).

iosefka_cli.py registers the parser (add_parser) and dispatches here (run). Errors are ClError; the
caller turns them into its own CliError (this module cannot import iosefka_cli: it is loaded by it).
Standard library only."""
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import clinic_changes as cc  # noqa: E402

MAX_IMPORT = 8 * 1024 * 1024
BUG_RX = re.compile(r"^NW-[0-9a-f]{7}$")
KEY_RX = re.compile(r"^[a-z][a-z0-9_-]{0,31}$")
HASH_RX = re.compile(r"^[0-9a-fA-F]{4,64}$")
CASE_RX = re.compile(r"^(?:\d{4}-\d{2}-\d{2}/)?[A-Za-z0-9._() -]{1,120}$")
SUBCOMMANDS = ("versions, version, list, show, add, edit, status, move, link, merge, import, related, notes")


class ClError(Exception):
    def __init__(self, msg, code=2):
        super().__init__(msg)
        self.code = code


def _iso(t):
    return datetime.fromtimestamp(t, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ") if t else None


def _item_out(it):
    return dict(it, created=_iso(it["created"]), updated=_iso(it["updated"]))


def _version_out(v):
    return dict(v, released=_iso(v["released"]), created=_iso(v["created"]), updated=_iso(v["updated"]))


def _date(s, what):
    try:
        return datetime.strptime(s, "%Y-%m-%d").replace(tzinfo=timezone.utc).timestamp()
    except ValueError:
        raise ClError(f"{what} wants YYYY-MM-DD: {s!r}")


def _days(s, what):
    try:
        n = float(s)
    except ValueError:
        n = -1
    if not n >= 0 or n != n or n == float("inf"):
        raise ClError(f"{what} wants a number of days: {s!r}")
    return time.time() - n * 86400


def _bug_ids(xs):
    for x in xs:
        if not BUG_RX.match(x):
            raise ClError(f"not a bug id (NW-xxxxxxx): {x!r}")
    return xs


def _cl_id(s):
    if not cc.CL_RX.match(s or ""):
        raise ClError(f"not a changelist item id (CL-xxxxxx): {s!r}")
    return s


def _tag_pairs(pairs):
    tags = {}
    for p in pairs:
        k, eq, v = p.partition("=")
        if not eq or not KEY_RX.match(k):
            raise ClError(f"tag wants key=value: {p!r}")
        tags[k] = None if v == "" else ([x for x in v.split(",") if x] if "," in v else v)
    return tags


def _filter_tags(pairs):
    for t in pairs:
        if not KEY_RX.match(t.partition("=")[0]):
            raise ClError(f"--tag wants key=value: {t!r}")
    return pairs


def parse_in(spec):
    """`TRACK:VERSION` for `status ... fixed --in`; ClError when malformed."""
    track, sep, version = (spec or "").partition(":")
    if not sep or track not in cc.TRACKS:
        raise ClError(f"--in wants TRACK:VERSION with TRACK one of {', '.join(cc.TRACKS)}: {spec!r}")
    try:
        return track, cc.norm_version(version)
    except ValueError as e:
        raise ClError(str(e))


def add_parser(sub, common):
    """Register `cl` and its nested subcommands on the top-level subparsers `sub`."""
    P = type(common)
    p = sub.add_parser("cl", parents=[common], add_help=False)
    cs = p.add_subparsers(dest="clcmd", parser_class=lambda **kw: P(allow_abbrev=False, **kw))

    def add(name, parents=True):
        return cs.add_parser(name, parents=[common] if parents else [], add_help=False)

    s = add("versions")
    s.add_argument("--track")
    s = add("version")
    s.add_argument("track")
    s.add_argument("ver")
    s.add_argument("--status")
    s.add_argument("--released")
    s.add_argument("--note")
    s = add("list")
    s.add_argument("--track")
    g = s.add_mutually_exclusive_group()
    g.add_argument("--version", dest="ver")
    g.add_argument("--current", action="store_true")
    g.add_argument("--next", action="store_true")
    s.add_argument("--status", action="append", default=[])
    s.add_argument("--section")
    s.add_argument("--tag", action="append", default=[])
    s.add_argument("--bug", dest="bug_filter")
    s.add_argument("--q")
    s.add_argument("--since")
    s.add_argument("--older-than", dest="older_than")
    s.add_argument("--idle")
    s = add("show")
    s.add_argument("cl_id")
    s = add("add")
    s.add_argument("track")
    s.add_argument("ver")
    s.add_argument("section")
    s.add_argument("item_text", metavar="text")
    s.add_argument("--tag", action="append", default=[])
    s.add_argument("--bug", dest="cl_bugs", action="append", default=[])
    s.add_argument("--case", dest="cl_cases", action="append", default=[])
    s.add_argument("--commit", dest="cl_commits", action="append", default=[])
    s.add_argument("--status", default="planned")
    s.add_argument("--dev")
    s = add("edit", parents=False)  # takes its own --text VALUE, so no human-output flag here
    s.add_argument("cl_id")
    s.add_argument("--text", dest="edit_text")
    s.add_argument("--section")
    s.add_argument("--dev")
    s.add_argument("--tag", action="append", default=[])
    s = add("status")
    s.add_argument("cl_id")
    s.add_argument("new_status")
    s = add("move")
    s.add_argument("cl_id")
    s.add_argument("track")
    s.add_argument("ver")
    s = add("link")
    s.add_argument("cl_id")
    s.add_argument("--bug", dest="cl_bugs", action="append", default=[])
    s.add_argument("--case", dest="cl_cases", action="append", default=[])
    s.add_argument("--commit", dest="cl_commits", action="append", default=[])
    s.add_argument("--unlink", action="store_true")
    s = add("merge")
    s.add_argument("cl_keep")
    s.add_argument("cl_dup")
    add("import")
    s = add("related")
    s.add_argument("cl_ref")
    s = add("notes")
    s.add_argument("track")
    s.add_argument("ver")
    return p


def _list(ch, a):
    if a.track and a.track not in cc.TRACKS:
        raise ClError(f"bad track {a.track!r} (one of {', '.join(cc.TRACKS)})")
    kw = dict(status=a.status or None, section=a.section, tag=_filter_tags(a.tag) or None,
              bug=a.bug_filter, q=a.q)
    if a.since:
        kw["since"] = _date(a.since, "--since")
    if a.older_than is not None:
        kw["older_than"] = _days(a.older_than, "--older-than")
    if a.idle is not None:
        kw["idle"] = _days(a.idle, "--idle")
    if a.current or a.next:
        out = []
        for t in ([a.track] if a.track else cc.TRACKS):
            v = ch.current(t) if a.current else ch.next(t)
            if v:
                out += ch.items(track=t, version=v, **kw)
        out.sort(key=lambda i: (i["created"], i["id"]), reverse=True)
        return out
    return ch.items(track=a.track, version=a.ver, **kw)


def _show(ch, clinic, cid):
    it = ch.get(cid)
    if it is None:
        raise ClError(f"no such item {cid}", 1)
    bugs = []
    for bid in it["bugs"]:
        b = clinic.bug(bid)
        bugs.append({"id": bid, "title": b["title"], "status": b["status"], "tags": b["tags"]} if b
                    else {"id": bid, "missing": True})
    notes = [dict(n, at=_iso(n["at"])) for n in ch.notes(cid)]
    return {"item": _item_out(it), "notes": notes, "bugs": bugs}


def fixed_in(clinic, who, bug_id, spec):
    """`status NW-x fixed --in TRACK:VERSION`: the bug's live (non-dropped) item in TRACK moves forward to
    done and, when its version is not locked, to VERSION (created if missing); a version-locked item keeps
    its version. With no such item, add `bug:<TRACK>:<id>`. Returns {"id", "created"}. ClError on bad input."""
    track, version = parse_in(spec)
    ch = cc.Changes(clinic)
    try:
        it = ch.item_for_bug(track, bug_id)
        if it is not None:
            changed = ch.set_status(it["id"], "done", who=who, human=False)
            if "version" not in it["locked"] and it["version"] != version:
                ch.move(it["id"], track, version, who=who)
                changed = True
            if changed:
                ch.add_note(it["id"], who, "link", f"bug {bug_id} marked fixed in {track} {version}")
            return {"id": it["id"], "created": False}
        bug = clinic.bug(bug_id)
        text = cc.clean_text(bug["title"])
        v = next((x for x in ch.versions(track) if x["version"] == version), None)
        status = "done" if v is not None and v["status"] != "planned" else "planned"
        cid = ch.add(track, version, "fixed", text, source_key=f"bug:{track}:{bug_id}", bugs=[bug_id],
                     status=status, who=who)
        return {"id": cid, "created": True}
    except ValueError as e:
        raise ClError(str(e), 1 if str(e).startswith("no such") else 2)


def run(a, clinic, who, emit, stdin):
    try:
        return _run(a, clinic, who, emit, stdin)
    except ValueError as e:
        raise ClError(str(e), 1 if str(e).startswith("no such") else 2)


def _run(a, clinic, who, emit, stdin):
    cmd = a.clcmd
    if not cmd:
        raise ClError(f"cl wants a subcommand ({SUBCOMMANDS})")
    ch = cc.Changes(clinic)
    if cmd == "versions":
        if a.track and a.track not in cc.TRACKS:
            raise ClError(f"bad track {a.track!r} (one of {', '.join(cc.TRACKS)})")
        return emit([_version_out(v) for v in ch.versions(a.track)])
    if cmd == "version":
        v = ch.set_version(a.track, a.ver, status=a.status, released=a.released, notes=a.note, who=who)
        return emit(_version_out(v))
    if cmd == "list":
        return emit([_item_out(i) for i in _list(ch, a)])
    if cmd == "show":
        return emit(_show(ch, clinic, _cl_id(a.cl_id)))
    if cmd == "add":
        cid = ch.add(a.track, a.ver, a.section, a.item_text, who=who, status=a.status,
                     tags=_tag_pairs(a.tag), bugs=_bug_ids(a.cl_bugs), cases=_cases(a.cl_cases),
                     commits=_commits(a.cl_commits), dev=a.dev or "")
        return emit(_item_out(ch.get(cid)))
    if cmd == "edit":
        it = ch.edit(_cl_id(a.cl_id), who=who, text=a.edit_text, section=a.section, dev=a.dev,
                     tags=_tag_pairs(a.tag) if a.tag else None)
        return emit(_item_out(it))
    if cmd == "status":
        _cl_id(a.cl_id)
        changed = ch.set_status(a.cl_id, a.new_status, who=who)
        return emit({"ok": True, "id": a.cl_id, "status": a.new_status, "changed": changed})
    if cmd == "move":
        return emit(_item_out(ch.move(_cl_id(a.cl_id), a.track, a.ver, who=who)))
    if cmd == "link":
        it = ch.link(_cl_id(a.cl_id), who=who, bugs=_bug_ids(a.cl_bugs), cases=_cases(a.cl_cases),
                     commits=_commits(a.cl_commits), unlink=a.unlink)
        return emit(_item_out(it))
    if cmd == "merge":
        return emit({"ok": True, "kept": _cl_id(a.cl_keep), "duplicate": _cl_id(a.cl_dup),
                     "item": _item_out(ch.merge(a.cl_keep, a.cl_dup, who=who))})
    if cmd == "import":
        data = stdin.read(MAX_IMPORT + 1)
        if len(data) > MAX_IMPORT:
            raise ClError(f"import is limited to {MAX_IMPORT // (1024 * 1024)} MB")
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            raise ClError("import wants UTF-8 JSONL on stdin")
        return emit(ch.import_lines(text.split("\n"), who=who))
    if cmd == "related":
        ref = a.cl_ref
        if not (cc.CL_RX.match(ref) or BUG_RX.match(ref)):
            raise ClError(f"related wants CL-xxxxxx or NW-xxxxxxx: {ref!r}")
        return emit({"ref": ref, "related": ch.related(ref)})
    if cmd == "notes":
        if a.track not in cc.TRACKS:
            raise ClError(f"bad track {a.track!r} (one of {', '.join(cc.TRACKS)})")
        return emit(ch.render_notes(a.track, cc.norm_version(a.ver)).encode("utf-8"))
    raise ClError(f"unknown cl command {cmd!r}")


def _cases(xs):
    for x in xs:
        if not CASE_RX.match(x) or ".." in x:
            raise ClError(f"not a case folder name: {x!r}")
    return xs


def _commits(xs):
    for x in xs:
        if not HASH_RX.match(x):
            raise ClError(f"not a commit hash: {x!r}")
    return xs
