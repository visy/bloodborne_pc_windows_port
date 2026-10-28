"""Iosefka's changelists: versioned change items for the two projects (coop, nw), kept in the clinic
database (nw_versions, nw_changes, nw_change_notes). Human edits lock the fields they set; import
(sync scripts) never writes a locked field. Public `text` is masked and checked; `dev` is private.
Standard library only."""
import contextlib
import json
import re
import secrets
import sqlite3
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from clinic_logic import mask_ips  # noqa: E402
from clinic_store import _tag_match  # noqa: E402

TRACKS = ("coop", "nw")
SECTIONS = ("added", "changed", "fixed")
ITEM_STATUSES = ("planned", "in-progress", "done", "dropped")
VERSION_STATUSES = ("planned", "current", "released")
CL_RX = re.compile(r"^CL-[0-9a-f]{6}$")

SCHEMA = """
CREATE TABLE IF NOT EXISTS nw_versions (
    track TEXT NOT NULL, version TEXT NOT NULL, status TEXT, released REAL, notes TEXT, locked TEXT,
    created REAL, updated REAL, PRIMARY KEY (track, version));
CREATE TABLE IF NOT EXISTS nw_changes (
    id TEXT PRIMARY KEY, track TEXT, version TEXT, section TEXT, text TEXT, dev TEXT, status TEXT,
    tags TEXT, bugs TEXT, cases TEXT, commits TEXT, source_key TEXT UNIQUE, author TEXT, locked TEXT,
    created REAL, updated REAL);
CREATE INDEX IF NOT EXISTS nw_changes_ver ON nw_changes (track, version);
CREATE TABLE IF NOT EXISTS nw_change_notes (
    change_id TEXT NOT NULL, at REAL, who TEXT, kind TEXT, text TEXT);
CREATE INDEX IF NOT EXISTS nw_change_notes_id ON nw_change_notes (change_id);
"""

CHANGE_COLS = ("id", "track", "version", "section", "text", "dev", "status", "tags", "bugs", "cases",
               "commits", "source_key", "author", "locked", "created", "updated")
VERSION_COLS = ("track", "version", "status", "released", "notes", "locked", "created", "updated")
STATUS_ORDER = {"planned": 0, "in-progress": 1, "done": 2}
STAGE_RANK = {"alpha": 0, "beta": 1, "rc": 2}
MAX_TEXT = 300
MAX_DEV = 2000
MAX_NOTES = 2000

_VER_RX = re.compile(r"^(\d+(?:\.\d+)*)(?:\s*[- ]?\s*(alpha|beta|rc)\s*\.?\s*(\d+))?$", re.I)
_SNOWFLAKE_RX = re.compile(r"(?<!\d)\d{17,20}(?!\d)")
_TOKEN_RX = re.compile(r"[a-z0-9]{4,}")
_STOP = frozenset("""this that with from have when what will then than they them were been into over after
before while about there their which where would could should does done also only just more most some
such very still even each other both make made fixed fixes added adds change changed changes item bug
bugs game mode party""".split())


def norm_version(v):
    """Canonical version text ("0.2.5 beta 3"); ValueError when it is not number[+stage]."""
    m = _VER_RX.match((v or "").strip())
    if not m:
        raise ValueError(f"bad version {v!r} (expected like 0.7.7 or 0.2.5 beta 3)")
    return m.group(1) if not m.group(2) else f"{m.group(1)} {m.group(2).lower()} {int(m.group(3))}"


def version_key(v):
    """Sort key: numbers, then stage alpha < beta < rc < final (no stage), then stage number."""
    m = _VER_RX.match((v or "").strip())
    if not m:
        return ((-1,), 4, 0)
    nums = tuple(int(x) for x in m.group(1).split("."))
    while len(nums) > 1 and nums[-1] == 0:  # 0.7 == 0.7.0
        nums = nums[:-1]
    if not m.group(2):
        return (nums, 3, 0)
    return (nums, STAGE_RANK[m.group(2).lower()], int(m.group(3)))


def clean_text(t):
    """Public one-liner: IPs masked, whitespace collapsed. ValueError(why) when it must not be public."""
    t = " ".join((t or "").split())
    if not t:
        raise ValueError("empty text")
    if "player_logs" in t:
        raise ValueError("text mentions player_logs (put paths in dev)")
    if "<@" in t:
        raise ValueError("text contains a Discord mention")
    if _SNOWFLAKE_RX.search(t):
        raise ValueError("text contains an id-like number (17-20 digits)")
    t = mask_ips(t)
    if len(t) > MAX_TEXT:
        raise ValueError(f"text too long ({len(t)} > {MAX_TEXT})")
    return t


def _enum(value, allowed, what):
    if value not in allowed:
        raise ValueError(f"bad {what} {value!r} (one of {', '.join(allowed)})")
    return value


def _jl(s, default):
    try:
        return json.loads(s) if s else default
    except ValueError:
        return default


def _dumps(x):
    return json.dumps(x, sort_keys=True)


def _strs(xs):
    """List of unique non-empty strings, order kept."""
    if isinstance(xs, str):
        xs = [xs]
    out = []
    for x in xs or ():
        x = str(x).strip()
        if x and x not in out:
            out.append(x)
    return out


def _tags_in(tags):
    if tags is None:
        return {}
    if isinstance(tags, dict):
        return dict(tags)
    out = {}
    for spec in _strs(tags):  # list of "k=v"
        k, _, v = spec.partition("=")
        if k:
            out[k] = v or True
    return out


def _merge_tags(have, new):
    out = dict(have)
    for k, v in new.items():
        if v is None:
            out.pop(k, None)
        else:
            out[k] = v
    return out


def _epoch(v, what="date"):
    if v is None or v == "":
        return None
    if isinstance(v, bool):
        raise ValueError(f"bad {what}")
    if isinstance(v, (int, float)):
        return float(v)
    try:
        return datetime.strptime(str(v)[:10], "%Y-%m-%d").replace(tzinfo=timezone.utc).timestamp()
    except ValueError:
        raise ValueError(f"bad {what} {v!r}")


def _tag_values(tags):
    out = set()
    for v in (tags or {}).values():
        for x in (v if isinstance(v, list) else [v]):
            if isinstance(x, str) and x:
                out.add(x.lower())
    return out


def _tokens(*texts):
    return {w for t in texts for w in _TOKEN_RX.findall((t or "").lower()) if w not in _STOP}


def bug_track(bug):
    """A bug counts as track nw when a tag name or value starts with `nw-`, else coop."""
    for k, v in (bug.get("tags") or {}).items():
        for x in [k] + (v if isinstance(v, list) else [v]):
            if isinstance(x, str) and x.lower().startswith("nw-"):
                return "nw"
    return "coop"


def _row(row):
    d = dict(zip(CHANGE_COLS, row))
    for k in ("tags",):
        d[k] = _jl(d[k], {})
    for k in ("bugs", "cases", "commits", "locked"):
        d[k] = _jl(d[k], [])
    return d


def _vrow(row):
    d = dict(zip(VERSION_COLS, row))
    d["locked"] = _jl(d["locked"], [])
    return d


class Changes:
    def __init__(self, clinic):
        self.clinic = clinic
        self.db = clinic.db
        self.db.executescript(SCHEMA)
        self.db.commit()

    @contextlib.contextmanager
    def _txn(self):
        """One transaction: committed when the block ends, rolled back when it raises."""
        try:
            yield
            self.db.commit()
        except BaseException:
            self.db.rollback()
            raise

    def _note(self, key, who, kind, text):
        self.db.execute("INSERT INTO nw_change_notes (change_id, at, who, kind, text) VALUES (?,?,?,?,?)",
                        (key, time.time(), who, kind, text[:MAX_NOTES]))

    # versions

    def _version(self, track, version):
        row = self.db.execute(f"SELECT {', '.join(VERSION_COLS)} FROM nw_versions WHERE track=? AND version=?",
                              (track, version)).fetchone()
        return _vrow(row) if row else None

    def _set_version(self, track, version, status, released, notes, who, human):
        track = _enum(track, TRACKS, "track")
        version = norm_version(version)
        if status is not None:
            _enum(status, VERSION_STATUSES, "version status")
        released = _epoch(released, "released")
        if notes is not None:
            notes = notes.strip()[:MAX_NOTES]
        now = time.time()
        v = self._version(track, version)
        key = f"{track}:{version}"
        if v is None:
            self.db.execute("INSERT INTO nw_versions VALUES (?,?,?,?,?,?,?,?)",
                            (track, version, "planned", None, "", "[]", now, now))
            self._note(key, who, "version", "created")
            v = self._version(track, version)
        locked = list(v["locked"])
        given = {"status": status, "released": released, "notes": notes}
        changes = []
        for field, val in given.items():
            if val is None:
                continue
            if not human and field in locked:
                continue
            if not human and field == "status" and v["status"] == "released" and val != "released":
                continue  # import never reopens a released version
            if field == "status" and val == "current":
                cur = self.db.execute("SELECT version, locked FROM nw_versions WHERE track=? AND status='current' "
                                      "AND version<>?", (track, version)).fetchall()
                if not human and any("status" in _jl(c[1], []) for c in cur):
                    continue  # a human-set current stays; import cannot demote it
                for c in cur:
                    self.db.execute("UPDATE nw_versions SET status='planned', updated=? WHERE track=? AND version=?",
                                    (now, track, c[0]))
                    self._note(f"{track}:{c[0]}", who, "version", f"status current -> planned ({version} is current)")
            if val != v[field]:
                changes.append(f"{field} {v[field]!r} -> {val!r}")
                self.db.execute(f"UPDATE nw_versions SET {field}=?, updated=? WHERE track=? AND version=?",
                                (val, now, track, version))
            if field == "status" and val == "released" and given["released"] is None and not v["released"] \
                    and "released" not in locked:
                self.db.execute("UPDATE nw_versions SET released=? WHERE track=? AND version=?", (now, track, version))
            if human and field not in locked:
                locked.append(field)
        self.db.execute("UPDATE nw_versions SET locked=? WHERE track=? AND version=?",
                        (_dumps(sorted(locked)), track, version))
        if changes:
            self._note(key, who, "version", "; ".join(changes))
        return self._version(track, version)

    def set_version(self, track, version, *, status=None, released=None, notes=None, who, human=True):
        """Create or update a version. human=True locks the fields given (import will not touch them).
        Setting status=current demotes the track's previous current to planned."""
        with self._txn():
            return self._set_version(track, version, status, released, notes, who, human)

    def versions(self, track=None):
        """Versions sorted by version_key, each with `counts` (items per status)."""
        rows = self.db.execute(f"SELECT {', '.join(VERSION_COLS)} FROM nw_versions").fetchall()
        out = []
        for r in rows:
            v = _vrow(r)
            if track and v["track"] != track:
                continue
            counts = dict.fromkeys(ITEM_STATUSES, 0)
            for st, n in self.db.execute("SELECT status, COUNT(*) FROM nw_changes WHERE track=? AND version=? "
                                         "GROUP BY status", (v["track"], v["version"])):
                counts[st] = n
            v["counts"] = counts
            out.append(v)
        out.sort(key=lambda v: (v["track"], version_key(v["version"])))
        return out

    def current(self, track):
        vs = [v["version"] for v in self.versions(track) if v["status"] == "current"]
        return vs[0] if vs else None

    def next(self, track):
        """Lowest planned version above the current one (lowest planned when none is current)."""
        cur = self.current(track)
        for v in self.versions(track):
            if v["status"] == "planned" and (cur is None or version_key(v["version"]) > version_key(cur)):
                return v["version"]
        return None

    # items

    def _new_id(self):
        while True:
            cid = "CL-" + secrets.token_hex(3)
            if not self.db.execute("SELECT 1 FROM nw_changes WHERE id=?", (cid,)).fetchone():
                return cid

    def _add(self, track, version, section, text, who, status, tags, bugs, cases, commits, dev, source_key):
        track = _enum(track, TRACKS, "track")
        section = _enum(section, SECTIONS, "section")
        status = _enum(status, ITEM_STATUSES, "status")
        text = clean_text(text)
        version = norm_version(version)
        dev = (dev or "").strip()[:MAX_DEV]
        if source_key and self.db.execute("SELECT 1 FROM nw_changes WHERE source_key=?", (source_key,)).fetchone():
            raise ValueError(f"source_key {source_key} already exists")
        if self._version(track, version) is None:
            self._set_version(track, version, None, None, None, who, False)
        cid, now = self._new_id(), time.time()
        self.db.execute(f"INSERT INTO nw_changes ({', '.join(CHANGE_COLS)}) VALUES ({','.join('?' * 16)})",
                        (cid, track, version, section, text, dev, status, _dumps(_tags_in(tags)),
                         _dumps(_strs(bugs)), _dumps(_strs(cases)), _dumps(_strs(commits)),
                         source_key or None, who, "[]", now, now))
        self._note(cid, who, "add", f"added to {track} {version} as {section}/{status}")
        return cid

    def add(self, track, version, section, text, *, who, status="planned", tags=None, bugs=(), cases=(),
            commits=(), dev="", source_key=None):
        """New item; creates the version as planned when missing. Returns the CL id."""
        with self._txn():
            return self._add(track, version, section, text, who, status, tags, bugs, cases, commits, dev,
                             source_key)

    def get(self, cid):
        row = self.db.execute(f"SELECT {', '.join(CHANGE_COLS)} FROM nw_changes WHERE id=?", (cid,)).fetchone()
        return _row(row) if row else None

    def _need(self, cid):
        it = self.get(cid)
        if it is None:
            raise ValueError(f"no such item {cid}")
        return it

    def items(self, track=None, version=None, status=None, section=None, tag=None, bug=None, q=None,
              since=None, older_than=None, idle=None):
        """Items newest created first; every filter must match. `status`/`tag` take one value or a list
        (statuses: any; tags: all). since/older_than/idle are epoch times: created >= since,
        created < older_than, updated < idle."""
        statuses = [status] if isinstance(status, str) else list(status or [])
        tags = [tag] if isinstance(tag, str) else list(tag or [])
        if version:
            version = norm_version(version)
        out = []
        rows = self.db.execute(f"SELECT {', '.join(CHANGE_COLS)} FROM nw_changes ORDER BY created DESC, rowid DESC")
        for row in rows:
            it = _row(row)
            if track and it["track"] != track:
                continue
            if version and it["version"] != version:
                continue
            if statuses and it["status"] not in statuses:
                continue
            if section and it["section"] != section:
                continue
            if bug and bug not in it["bugs"]:
                continue
            if since is not None and it["created"] < since:
                continue
            if older_than is not None and not it["created"] < older_than:
                continue
            if idle is not None and not it["updated"] < idle:
                continue
            if not all(_tag_match(it["tags"], t) for t in tags):
                continue
            if q and q.lower() not in f"{it['text']}\n{it['dev']}\n{it['id']}".lower():
                continue
            out.append(it)
        return out

    def _update(self, it, who, kind, note, locks=(), **fields):
        locked = list(it["locked"])
        for f in locks:
            if f not in locked:
                locked.append(f)
        sets = dict(fields, locked=sorted(locked), updated=time.time())
        vals = [_dumps(v) if isinstance(v, (dict, list)) else v for v in sets.values()]
        self.db.execute(f"UPDATE nw_changes SET {', '.join(k + '=?' for k in sets)} WHERE id=?",
                        (*vals, it["id"]))
        self._note(it["id"], who, kind, note)

    def edit(self, cid, *, who, text=None, section=None, dev=None, tags=None):
        """Human edit: the fields given are locked. `tags` is a dict merged into the item's tags (None value
        removes a key)."""
        with self._txn():
            it = self._need(cid)
            f, locks, notes = {}, [], []
            if text is not None:
                f["text"] = clean_text(text)
                locks.append("text")
                notes.append("text")
            if section is not None:
                f["section"] = _enum(section, SECTIONS, "section")
                locks.append("section")
                notes.append(f"section {it['section']} -> {section}")
            if dev is not None:
                f["dev"] = dev.strip()[:MAX_DEV]
                locks.append("dev")
                notes.append("dev")
            if tags is not None:
                f["tags"] = _merge_tags(it["tags"], _tags_in(tags))
                locks.append("tags")
                notes.append("tags")
            if f:
                self._update(it, who, "edit", "edited " + ", ".join(notes), locks, **f)
            return self.get(cid)

    def set_status(self, cid, status, *, who, human=True):
        """human=True: set any status and lock it. human=False (automation, e.g. 'fixed --in'): only moves
        forward planned -> in-progress -> done, never from or to dropped, no lock. Returns True when it changed."""
        _enum(status, ITEM_STATUSES, "status")
        with self._txn():
            it = self._need(cid)
            if it["status"] == status:
                if human and "status" not in it["locked"]:
                    self._update(it, who, "status", f"status {status} (locked)", ["status"])
                return False
            if not human and (status not in STATUS_ORDER or it["status"] not in STATUS_ORDER
                              or STATUS_ORDER[status] < STATUS_ORDER[it["status"]]):
                return False
            self._update(it, who, "status", f"status {it['status']} -> {status}",
                         ["status"] if human else (), status=status)
            return True

    def move(self, cid, track, version, *, who):
        """Move to another version/track (locks `version`). Creates the target version as planned."""
        track = _enum(track, TRACKS, "track")
        version = norm_version(version)
        with self._txn():
            it = self._need(cid)
            if self._version(track, version) is None:
                self._set_version(track, version, None, None, None, who, False)
            self._update(it, who, "move", f"moved {it['track']} {it['version']} -> {track} {version}",
                         ["version"], track=track, version=version)
            return self.get(cid)

    def link(self, cid, *, who, bugs=(), cases=(), commits=(), unlink=False):
        with self._txn():
            it = self._need(cid)
            f, notes = {}, []
            for name, vals in (("bugs", bugs), ("cases", cases), ("commits", commits)):
                vals = _strs(vals)
                if not vals:
                    continue
                have = it[name]
                new = [x for x in have if x not in vals] if unlink else have + [x for x in vals if x not in have]
                if new != have:
                    f[name] = new
                    notes.append(f"{'unlinked' if unlink else 'linked'} {name} {', '.join(vals)}")
            if f:
                self._update(it, who, "link", "; ".join(notes), **f)
            return self.get(cid)

    def merge(self, keep, dup, *, who):
        """Fold `dup` into `keep`: links unioned, missing tags copied, dup becomes dropped (status locked)."""
        if keep == dup:
            raise ValueError("cannot merge an item into itself")
        with self._txn():
            k, d = self._need(keep), self._need(dup)
            f = {n: k[n] + [x for x in d[n] if x not in k[n]] for n in ("bugs", "cases", "commits")}
            f["tags"] = _merge_tags(d["tags"], k["tags"])
            self._update(k, who, "merge", f"merged {dup} into this item", **f)
            self._update(d, who, "merge", f"merged into {keep}; dropped", ["status"], status="dropped")
            return self.get(keep)

    def item_for_bug(self, track, bug_id):
        """The newest item in `track` that links the bug (any version), or None."""
        for it in self.items(track=track, bug=bug_id):
            return it
        return None

    # import

    def import_lines(self, lines, *, who):
        """Apply JSONL lines (items and versions), each in its own transaction. Bad lines are skipped and
        reported: {"applied": n, "skipped": [{"line": k, "why": ...}]} with 1-based line numbers."""
        applied, skipped = 0, []
        for n, line in enumerate(lines, 1):
            if isinstance(line, bytes):
                line = line.decode("utf-8", "replace")
            if not line.strip():
                continue
            try:
                rec = json.loads(line)
                if not isinstance(rec, dict):
                    raise ValueError("line is not a JSON object")
                with self._txn():
                    kind = rec.get("kind", "item")
                    if kind == "version":
                        self._import_version(rec, who)
                    elif kind == "item":
                        self._import_item(rec, who)
                    else:
                        raise ValueError(f"unknown kind {kind!r}")
                applied += 1
            except (ValueError, TypeError, sqlite3.Error) as e:
                skipped.append({"line": n, "why": str(e)})
        return {"applied": applied, "skipped": skipped}

    def _import_version(self, rec, who):
        for f in ("track", "version"):
            if not rec.get(f):
                raise ValueError(f"missing {f}")
        self._set_version(rec["track"], rec["version"], rec.get("status"), rec.get("released"),
                          rec.get("notes"), who, False)

    def _import_item(self, rec, who):
        for f in ("source_key", "track", "version", "section", "text"):
            if not rec.get(f):
                raise ValueError(f"missing {f}")
        key = str(rec["source_key"])
        row = self.db.execute("SELECT id FROM nw_changes WHERE source_key=?", (key,)).fetchone()
        if row is None:
            self._add(rec["track"], rec["version"], rec["section"], rec["text"], who,
                      rec.get("status") or "planned", rec.get("tags"), rec.get("bugs") or (),
                      rec.get("cases") or (), rec.get("commits") or (), rec.get("dev"), key)
            return
        it = self.get(row[0])
        locked = it["locked"]
        track = _enum(rec["track"], TRACKS, "track")
        version = norm_version(rec["version"])
        section = _enum(rec["section"], SECTIONS, "section")
        text = clean_text(rec["text"])
        status = _enum(rec["status"], ITEM_STATUSES, "status") if rec.get("status") else None
        f = {}
        if "version" not in locked and (track, version) != (it["track"], it["version"]):
            if self._version(track, version) is None:
                self._set_version(track, version, None, None, None, who, False)
            f["track"], f["version"] = track, version
        if "section" not in locked and section != it["section"]:
            f["section"] = section
        if "text" not in locked and text != it["text"]:
            f["text"] = text
        if "dev" not in locked and rec.get("dev") is not None and rec["dev"].strip()[:MAX_DEV] != it["dev"]:
            f["dev"] = rec["dev"].strip()[:MAX_DEV]
        if "tags" not in locked and rec.get("tags") is not None:
            merged = _merge_tags(it["tags"], _tags_in(rec["tags"]))
            if merged != it["tags"]:
                f["tags"] = merged
        if "status" not in locked and status in STATUS_ORDER and it["status"] in STATUS_ORDER \
                and STATUS_ORDER[status] > STATUS_ORDER[it["status"]]:
            f["status"] = status
        for name in ("bugs", "cases", "commits"):  # links only ever add
            new = it[name] + [x for x in _strs(rec.get(name) or ()) if x not in it[name]]
            if new != it[name]:
                f[name] = new
        if f:
            self._update(it, who, "import", "import updated " + ", ".join(sorted(f)), **f)

    # cross-track

    def related(self, ref):
        """Items and bugs in the other track that share a linked bug (x10), a tag value (x3) or wording
        (1 per shared word). Top 10 with score >= 2, each with the reasons."""
        bugs_by_id = {b["id"]: b for b in self.clinic.bugs()}
        if CL_RX.match(ref or ""):
            me = self._need(ref)
            track, my_bugs, my_tags = me["track"], set(me["bugs"]), _tag_values(me["tags"])
            my_words = _tokens(me["text"])
            skip_item, skip_bug = me["id"], None
        else:
            b = bugs_by_id.get(ref)
            if b is None:
                raise ValueError(f"no such item or bug {ref}")
            track, my_bugs, my_tags = bug_track(b), {b["id"]}, _tag_values(b["tags"])
            my_words = _tokens(b["title"])
            skip_item, skip_bug = None, b["id"]
        other = "nw" if track == "coop" else "coop"
        out = []

        def score(cand_bugs, cand_tags, cand_words):
            why, total = [], 0
            for bid in sorted(my_bugs & cand_bugs):
                why.append(f"bug {bid}")
                total += 10
            for t in sorted(my_tags & cand_tags):
                why.append(f"tag {t}")
                total += 3
            words = sorted(my_words & cand_words)
            if words:
                why.append("words " + ", ".join(words[:6]))
                total += len(words)
            return total, why

        for it in self.items(track=other):
            if it["id"] == skip_item:
                continue
            s, why = score(set(it["bugs"]), _tag_values(it["tags"]), _tokens(it["text"]))
            if s >= 2:
                out.append({"kind": "item", "id": it["id"], "track": it["track"], "version": it["version"],
                            "status": it["status"], "text": it["text"], "why": why, "score": s})
        for bid, b in bugs_by_id.items():
            if bid == skip_bug or bug_track(b) != other:
                continue
            s, why = score({bid}, _tag_values(b["tags"]), _tokens(b["title"]))
            if s >= 2:
                out.append({"kind": "bug", "id": bid, "track": other, "status": b["status"],
                            "title": b["title"], "why": why, "score": s})
        out.sort(key=lambda r: (-r["score"], r["id"]))
        return out[:10]

    # notes

    def render_notes(self, track, version):
        """Release notes markdown: done items only, public text only, Added / Changed / Fixed."""
        items = sorted(self.items(track=track, version=version, status="done"),
                       key=lambda i: (i["created"], i["id"]))
        parts = []
        for sec in SECTIONS:
            lines = [f"- {mask_ips(i['text'])}" for i in items if i["section"] == sec]
            if lines:
                parts.append(f"### {sec.capitalize()}\n" + "\n".join(lines))
        return "\n\n".join(parts) + ("\n" if parts else "")

    def notes(self, cid):
        """History of an item (or of a version, key "<track>:<version>"), oldest first."""
        rows = self.db.execute("SELECT at, who, kind, text FROM nw_change_notes WHERE change_id=? "
                               "ORDER BY at, rowid", (cid,)).fetchall()
        return [dict(zip(("at", "who", "kind", "text"), r)) for r in rows]
