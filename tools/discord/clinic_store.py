"""Iosefka's clinic archive: log files, screenshots, video stills, messages and bug records, in the same
SQLite file as the netdoctor bot memory (netdoctor_store). The nw_* tables here are never pruned.

Files are keyed by the sha256 of the original bytes and deduplicated. Logs and other text are LZMA
compressed; images may be stored re-encoded (the key stays the original hash). Standard library only.
"""
import hashlib
import json
import lzma
import os
import secrets
import shutil
import sqlite3
import time
from pathlib import Path

SCHEMA = """
CREATE TABLE IF NOT EXISTS nw_files (
    sha256 TEXT PRIMARY KEY, kind TEXT, filename TEXT, mime TEXT, raw_size INTEGER, codec TEXT,
    blob BLOB, created REAL);
CREATE TABLE IF NOT EXISTS nw_stills (
    video_sha256 TEXT NOT NULL, n INTEGER NOT NULL, t_s REAL, file_sha256 TEXT,
    PRIMARY KEY (video_sha256, n));
CREATE TABLE IF NOT EXISTS nw_messages (
    message_id INTEGER PRIMARY KEY, channel_id INTEGER, thread_id INTEGER, author_id INTEGER,
    author_name TEXT, created REAL, edited REAL, content TEXT, reply_to INTEGER,
    attachments TEXT, bug_id TEXT, state TEXT, replacement_id INTEGER, seen REAL);
CREATE INDEX IF NOT EXISTS nw_messages_bug ON nw_messages (bug_id);
CREATE TABLE IF NOT EXISTS nw_bugs (
    id TEXT PRIMARY KEY, title TEXT, source TEXT, channel_id INTEGER, thread_id INTEGER,
    status TEXT, dup_of TEXT, tags TEXT, assignee TEXT, nd_refs TEXT, created REAL, updated REAL);
CREATE TABLE IF NOT EXISTS nw_notes (
    bug_id TEXT NOT NULL, at REAL, who TEXT, kind TEXT, text TEXT, to_who TEXT);
CREATE INDEX IF NOT EXISTS nw_notes_bug ON nw_notes (bug_id);
CREATE TABLE IF NOT EXISTS nw_watermark (channel_id INTEGER PRIMARY KEY, last_message_id INTEGER);
CREATE TABLE IF NOT EXISTS nw_seen (who TEXT NOT NULL, bug_id TEXT NOT NULL, at REAL, PRIMARY KEY (who, bug_id));
CREATE TABLE IF NOT EXISTS nw_exports (sha256 TEXT PRIMARY KEY, export_id TEXT, confirmed REAL);
CREATE TABLE IF NOT EXISTS nw_outbox (
    id INTEGER PRIMARY KEY AUTOINCREMENT, bug_id TEXT, text TEXT, who TEXT, created REAL, sent REAL);
"""

LZMA_KINDS = ("log", "dump", "game", "archive", "text", "other")
MESSAGE_COLS = ("channel_id", "thread_id", "author_id", "author_name", "created", "edited", "content",
                "reply_to", "attachments", "bug_id", "state", "replacement_id", "seen")
BUG_FIELDS = ("title", "source", "channel_id", "thread_id", "status", "dup_of", "assignee", "nd_refs")
BUG_COLS = ("id", "title", "source", "channel_id", "thread_id", "status", "dup_of", "tags", "assignee",
            "nd_refs", "created", "updated")


def _bug_dict(row):
    d = dict(zip(BUG_COLS, row))
    d["tags"] = json.loads(d["tags"] or "{}")
    return d


def _tag_match(tags, spec):
    k, _, v = spec.partition("=")
    have = tags.get(k)
    if have is None:
        return False
    if not v:
        return True
    vals = have if isinstance(have, list) else [have]
    return v.lower() in (str(x).lower() for x in vals)


class Clinic:
    def __init__(self, db_path):
        self.path = Path(db_path)
        self.db = sqlite3.connect(str(self.path), timeout=10)
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA busy_timeout=10000")
        self.db.executescript(SCHEMA)
        self.db.commit()

    # files

    def put_file(self, data, filename, kind, mime="", *, stored=None, codec=None):
        """Store one file under the sha256 of `data`; returns the hash. Idempotent. `stored`/`codec`: an
        already re-encoded copy (clinic_media.shrink_image) kept instead of the original."""
        sha = hashlib.sha256(data).hexdigest()
        if self.db.execute("SELECT 1 FROM nw_files WHERE sha256=?", (sha,)).fetchone():
            return sha
        if stored is not None:
            blob, codec = stored, codec or "orig"
        elif kind in LZMA_KINDS:
            blob, codec = lzma.compress(data, preset=6), "lzma"
        else:
            blob, codec = data, "orig"
        self.db.execute("INSERT INTO nw_files VALUES (?,?,?,?,?,?,?,?)",
                        (sha, kind, filename, mime, len(data), codec, blob, time.time()))
        self.db.commit()
        return sha

    def get_file(self, sha):
        """(filename, bytes, codec) with LZMA rows decompressed, or None."""
        row = self.db.execute("SELECT filename, blob, codec FROM nw_files WHERE sha256=?", (sha,)).fetchone()
        if not row:
            return None
        name, blob, codec = row
        return name, (lzma.decompress(blob) if codec == "lzma" else bytes(blob)), codec

    def file_info(self, sha):
        row = self.db.execute("SELECT sha256, kind, filename, mime, raw_size, codec FROM nw_files "
                              "WHERE sha256=?", (sha,)).fetchone()
        return dict(zip(("sha256", "kind", "filename", "mime", "raw_size", "codec"), row)) if row else None

    def all_files(self):
        """Every file as dicts without the blob, oldest first."""
        rows = self.db.execute("SELECT sha256, kind, filename, mime, raw_size, codec FROM nw_files "
                               "ORDER BY created, sha256")
        return [dict(zip(("sha256", "kind", "filename", "mime", "raw_size", "codec"), r)) for r in rows]

    def verify_file(self, sha):
        row = self.db.execute("SELECT blob, codec FROM nw_files WHERE sha256=?", (sha,)).fetchone()
        if not row or not row[0]:
            return False
        if row[1] != "lzma":
            return True
        try:
            return hashlib.sha256(lzma.decompress(row[0])).hexdigest() == sha
        except (lzma.LZMAError, EOFError):
            return False

    def add_stills(self, video_sha, frames):
        """Store (t_s, encoded image) frames of a video, replacing earlier stills of it; returns the count."""
        self.db.execute("DELETE FROM nw_stills WHERE video_sha256=?", (video_sha,))
        for n, (t, data) in enumerate(frames):
            ext = "webp" if data[:4] == b"RIFF" and data[8:12] == b"WEBP" else "jpg"
            sha = self.put_file(data, f"{video_sha[:8]}_still_{n:02d}.{ext}", "still", "image/" + ext)
            self.db.execute("INSERT INTO nw_stills VALUES (?,?,?,?)", (video_sha, n, t, sha))
        self.db.commit()
        return len(frames)

    def stills(self, video_sha):
        rows = self.db.execute("SELECT n, t_s, file_sha256 FROM nw_stills WHERE video_sha256=? ORDER BY n",
                               (video_sha,))
        return [{"n": n, "t_s": t, "sha256": s} for n, t, s in rows]

    # messages

    def upsert_message(self, rec):
        """Insert a message, or update only the columns given (state and replacement_id survive)."""
        rec = dict(rec)
        mid = rec.pop("message_id", None) or rec.pop("id")
        if isinstance(rec.get("attachments"), (list, tuple)):
            rec["attachments"] = json.dumps(rec["attachments"])
        bad = set(rec) - set(MESSAGE_COLS)
        if bad:
            raise KeyError(f"unknown message column(s): {sorted(bad)}")
        if self.db.execute("SELECT 1 FROM nw_messages WHERE message_id=?", (mid,)).fetchone():
            if rec:
                sets = ", ".join(f"{k}=?" for k in rec)
                self.db.execute(f"UPDATE nw_messages SET {sets} WHERE message_id=?", (*rec.values(), mid))
        else:
            rec.setdefault("state", "archived")
            rec.setdefault("attachments", "[]")
            cols = ["message_id", *rec]
            self.db.execute(f"INSERT INTO nw_messages ({', '.join(cols)}) VALUES ({', '.join('?' * len(cols))})",
                            (mid, *rec.values()))
        self.db.commit()

    def _message(self, row):
        d = dict(zip(("message_id", *MESSAGE_COLS), row))
        d["attachments"] = json.loads(d["attachments"] or "[]")
        return d

    def message(self, mid):
        row = self.db.execute(f"SELECT message_id, {', '.join(MESSAGE_COLS)} FROM nw_messages "
                              "WHERE message_id=?", (mid,)).fetchone()
        return self._message(row) if row else None

    def messages_for_bug(self, bug_id):
        rows = self.db.execute(f"SELECT message_id, {', '.join(MESSAGE_COLS)} FROM nw_messages "
                               "WHERE bug_id=? ORDER BY created, message_id", (bug_id,))
        return [self._message(r) for r in rows]

    def all_messages(self):
        rows = self.db.execute(f"SELECT message_id, {', '.join(MESSAGE_COLS)} FROM nw_messages "
                               "ORDER BY message_id")
        return [self._message(r) for r in rows]

    def set_state(self, mid, state, replacement_id=None):
        if replacement_id is None:
            self.db.execute("UPDATE nw_messages SET state=? WHERE message_id=?", (state, mid))
        else:
            self.db.execute("UPDATE nw_messages SET state=?, replacement_id=? WHERE message_id=?",
                            (state, replacement_id, mid))
        self.db.commit()

    # bugs

    def new_bug(self, title, source, channel_id, thread_id):
        while True:
            bug_id = "NW-" + secrets.token_hex(4)[:7]
            if not self.db.execute("SELECT 1 FROM nw_bugs WHERE id=?", (bug_id,)).fetchone():
                break
        now = time.time()
        self.db.execute("INSERT INTO nw_bugs VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                        (bug_id, title, source, channel_id, thread_id, "new", None, "{}", None, "[]", now, now))
        self.db.commit()
        return bug_id

    def bug(self, bug_id):
        row = self.db.execute(f"SELECT {', '.join(BUG_COLS)} FROM nw_bugs WHERE id=?", (bug_id,)).fetchone()
        return _bug_dict(row) if row else None

    def bugs(self, status=None, tag=None, q=None, since=None, assignee=None, exclude_status=None):
        """Bugs newest first; every given filter must match. `status`/`tag` take one value or a list
        (statuses: any of them; tags: all of them). A tag is "key=value", compared case-insensitively, and
        a stored value may be a list (matches when any element equals). `q` matches title, notes and
        message text. `since` is an epoch time (created at or after). `exclude_status` drops those."""
        statuses = [status] if isinstance(status, str) else list(status or [])
        tags = [tag] if isinstance(tag, str) else list(tag or [])
        rows = self.db.execute(f"SELECT {', '.join(BUG_COLS)} FROM nw_bugs ORDER BY created DESC, id")
        out = []
        for row in rows:
            b = _bug_dict(row)
            if statuses and b["status"] not in statuses:
                continue
            if exclude_status and b["status"] in exclude_status:
                continue
            if since is not None and b["created"] < since:
                continue
            if assignee and (b["assignee"] or "").lower() != assignee.lower():
                continue
            if not all(_tag_match(b["tags"], t) for t in tags):
                continue
            if q and not self._matches(b, q):
                continue
            out.append(b)
        return out

    def counts(self, bug_id):
        """How many messages, logs, images and video stills a bug holds."""
        msgs = self.messages_for_bug(bug_id)
        kinds = {"log": 0, "image": 0, "still": 0}
        for f in self.files_for_bug(bug_id):
            if f["kind"] in kinds:
                kinds[f["kind"]] += 1
        return {"messages": len(msgs), "logs": kinds["log"], "images": kinds["image"], "stills": kinds["still"]}

    def _matches(self, b, q):
        like = f"%{q.lower()}%"
        if q.lower() in (b["title"] or "").lower():
            return True
        return bool(self.db.execute(
            "SELECT 1 FROM nw_notes WHERE bug_id=? AND lower(text) LIKE ? UNION ALL "
            "SELECT 1 FROM nw_messages WHERE bug_id=? AND lower(content) LIKE ? LIMIT 1",
            (b["id"], like, b["id"], like)).fetchone())

    def update_bug(self, bug_id, **fields):
        """Set bug fields; `tags` is a dict merged into the existing tags (a None value removes a key)."""
        tags = fields.pop("tags", None)
        bad = set(fields) - set(BUG_FIELDS)
        if bad:
            raise KeyError(f"unknown bug field(s): {sorted(bad)}")
        if tags:
            merged = (self.bug(bug_id) or {}).get("tags", {})
            for k, v in tags.items():
                if v is None:
                    merged.pop(k, None)
                else:
                    merged[k] = v
            fields["tags"] = json.dumps(merged, sort_keys=True)
        fields["updated"] = time.time()
        sets = ", ".join(f"{k}=?" for k in fields)
        self.db.execute(f"UPDATE nw_bugs SET {sets} WHERE id=?", (*fields.values(), bug_id))
        self.db.commit()

    def merge_bugs(self, keep, dup):
        """Move dup's messages and notes to keep; dup becomes a duplicate of keep."""
        self.db.execute("UPDATE nw_messages SET bug_id=? WHERE bug_id=?", (keep, dup))
        self.db.execute("UPDATE nw_notes SET bug_id=? WHERE bug_id=?", (keep, dup))
        extra = (self.bug(dup) or {}).get("tags", {})
        have = (self.bug(keep) or {}).get("tags", {})
        self.update_bug(keep, tags={k: v for k, v in extra.items() if k not in have})
        self.update_bug(dup, status="duplicate", dup_of=keep)
        self.add_note(dup, "iosefka", "status", f"merged into {keep}")

    def add_note(self, bug_id, who, kind, text, to=""):
        self.db.execute("INSERT INTO nw_notes VALUES (?,?,?,?,?,?)", (bug_id, time.time(), who, kind, text, to))
        self.db.execute("UPDATE nw_bugs SET updated=? WHERE id=?", (time.time(), bug_id))
        self.db.commit()

    def notes(self, bug_id):
        rows = self.db.execute("SELECT bug_id, at, who, kind, text, to_who FROM nw_notes WHERE bug_id=? "
                               "ORDER BY at, rowid", (bug_id,))
        return [dict(zip(("bug_id", "at", "who", "kind", "text", "to"), r)) for r in rows]

    def bug_for_thread(self, thread_id):
        row = self.db.execute("SELECT id FROM nw_bugs WHERE thread_id=? AND status!='duplicate' "
                              "ORDER BY created LIMIT 1", (thread_id,)).fetchone()
        return row[0] if row else None

    def bug_for_files(self, shas):
        """A bug whose messages hold all of these files, or None."""
        want = set(shas)
        if not want:
            return None
        held = {}
        like = " OR ".join("m.attachments LIKE ?" for _ in want)
        for bug_id, att in self.db.execute(
                "SELECT m.bug_id, m.attachments FROM nw_messages m JOIN nw_bugs b ON b.id=m.bug_id "
                f"WHERE b.status!='duplicate' AND ({like})", [f"%{x}%" for x in sorted(want)]):
            held.setdefault(bug_id, set()).update(a.get("sha256") for a in json.loads(att or "[]"))
        for bug_id in sorted(held):
            if want <= held[bug_id]:
                return bug_id
        return None

    def files_for_bug(self, bug_id):
        """Files of a bug (attachments and video stills), deduplicated, as dicts without the blob."""
        out, seen = [], set()

        def add(sha):
            info = sha not in seen and self.file_info(sha)
            if info:
                seen.add(sha)
                out.append(info)
        for m in self.messages_for_bug(bug_id):
            for a in m["attachments"]:
                sha = a.get("sha256")
                if sha:
                    add(sha)
                    for s in self.stills(sha):
                        add(s["sha256"])
        return out

    # catch-up and "new since my last check"

    def watermark(self, channel_id):
        row = self.db.execute("SELECT last_message_id FROM nw_watermark WHERE channel_id=?",
                              (channel_id,)).fetchone()
        return row[0] if row else 0

    def set_watermark(self, channel_id, mid):
        self.db.execute("INSERT OR REPLACE INTO nw_watermark VALUES (?,?)", (channel_id, mid))
        self.db.commit()

    def mark_seen(self, who):
        """Record that `who` looked now; returns the previous time (0.0 the first time)."""
        row = self.db.execute("SELECT at FROM nw_seen WHERE who=? AND bug_id=''", (who,)).fetchone()
        self.db.execute("INSERT OR REPLACE INTO nw_seen VALUES (?,?,?)", (who, "", time.time()))
        self.db.commit()
        return row[0] if row else 0.0

    def changes_since(self, t, who=None):
        """Bugs created after t and notes added after t. A `share` note addressed to someone else is left
        out when `who` is given."""
        bugs = [_bug_dict(r) for r in self.db.execute(
            f"SELECT {', '.join(BUG_COLS)} FROM nw_bugs WHERE created>? ORDER BY created", (t,))]
        notes = [dict(zip(("bug_id", "at", "who", "kind", "text", "to"), r)) for r in self.db.execute(
            "SELECT bug_id, at, who, kind, text, to_who FROM nw_notes WHERE at>? ORDER BY at, rowid", (t,))]
        if who:
            notes = [n for n in notes if n["kind"] != "share" or n["to"] == who]
        return {"bugs": bugs, "notes": notes}

    # exports (the PC copy before the cull)

    def record_export(self, export_id, shas):
        for sha in shas:
            self.db.execute("INSERT INTO nw_exports VALUES (?,?,NULL) ON CONFLICT(sha256) DO UPDATE SET "
                            "export_id=excluded.export_id WHERE confirmed IS NULL", (sha, export_id))
        self.db.commit()

    def confirm_export(self, export_id, shas):
        n = 0
        for sha in shas:
            n += self.db.execute("UPDATE nw_exports SET confirmed=? WHERE sha256=? AND export_id=? "
                                 "AND confirmed IS NULL", (time.time(), sha, export_id)).rowcount
        self.db.commit()
        return n

    def exported(self, sha):
        row = self.db.execute("SELECT confirmed FROM nw_exports WHERE sha256=?", (sha,)).fetchone()
        return bool(row and row[0])

    # outbox: player-facing texts from the CLI that the bot posts

    def queue_tell(self, bug_id, who, text):
        cur = self.db.execute("INSERT INTO nw_outbox (bug_id, text, who, created, sent) VALUES (?,?,?,?,NULL)",
                              (bug_id, text, who, time.time()))
        self.db.commit()
        return cur.lastrowid

    def outbox_pending(self):
        rows = self.db.execute("SELECT id, bug_id, text, who, created FROM nw_outbox WHERE sent IS NULL "
                               "ORDER BY id")
        return [dict(zip(("id", "bug_id", "text", "who", "created"), r)) for r in rows]

    def outbox_sent(self, outbox_id):
        self.db.execute("UPDATE nw_outbox SET sent=? WHERE id=?", (time.time(), outbox_id))
        self.db.commit()

    # size and backup

    def size_bytes(self):
        pages = self.db.execute("PRAGMA page_count").fetchone()[0]
        return pages * self.db.execute("PRAGMA page_size").fetchone()[0]

    def backup(self, dest):
        """Copy the database with the sqlite3 backup API; False (nothing written) when free disk < 2x size."""
        dest = Path(dest)
        if shutil.disk_usage(dest.parent).free < 2 * self.size_bytes():
            return False
        tmp = dest.with_name(dest.name + ".tmp")
        out = sqlite3.connect(str(tmp))
        try:
            self.db.backup(out)
        finally:
            out.close()
        os.replace(tmp, dest)
        return True
