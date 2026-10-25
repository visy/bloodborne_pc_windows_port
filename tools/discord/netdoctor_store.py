"""netdoctor bot memory: which log files were posted where, who/when each one is, and which bot message
holds each session's analysis. SQLite (standard library). The log files themselves are never stored:
they stay on Discord and are downloaded again for each analysis.

Log files are kept LZMA-compressed (about 14x smaller), deduplicated by content hash, so an analysis can
be re-run after Discord forgets an attachment; they go with the 30-day prune and the size cap.

Grouping logs into sessions is pure logic here (same_session / group_sessions / session_key) so it is
tested without Discord.
"""
import hashlib
import json
import sqlite3
import time

SCHEMA = """
CREATE TABLE IF NOT EXISTS logs (
    channel_id INTEGER NOT NULL, message_id INTEGER NOT NULL, att_index INTEGER NOT NULL,
    filename TEXT, name TEXT, role TEXT, host TEXT, start INTEGER, end_ INTEGER, day TEXT,
    peers TEXT, posted REAL,
    PRIMARY KEY (channel_id, message_id, att_index));
CREATE TABLE IF NOT EXISTS logdata (
    sha1 TEXT PRIMARY KEY, filename TEXT, raw_size INTEGER, blob BLOB, created REAL);
CREATE TABLE IF NOT EXISTS analyses (
    ref TEXT PRIMARY KEY, channel_id INTEGER, guild_id INTEGER, bot_message_id INTEGER,
    files TEXT, text TEXT, created REAL);
CREATE TABLE IF NOT EXISTS sessions (
    channel_id INTEGER NOT NULL, session_key TEXT NOT NULL, bot_message_id INTEGER,
    log_count INTEGER, updated REAL, members TEXT,
    PRIMARY KEY (channel_id, session_key));
"""


class Store:
    def __init__(self, path):
        self.db = sqlite3.connect(str(path))
        self.db.executescript(SCHEMA)
        cols = {r[1] for r in self.db.execute("PRAGMA table_info(logs)")}
        if "sha1" not in cols:  # files from before the compressed copies
            self.db.execute("ALTER TABLE logs ADD COLUMN sha1 TEXT")
        self.db.commit()

    def has_log(self, channel_id, message_id, att_index):
        return self.db.execute("SELECT 1 FROM logs WHERE channel_id=? AND message_id=? AND att_index=?",
                               (channel_id, message_id, att_index)).fetchone() is not None

    def add_log(self, channel_id, message_id, att_index, filename, info):
        self.db.execute("INSERT OR REPLACE INTO logs (channel_id, message_id, att_index, filename, name, role, "
                        "host, start, end_, day, peers, posted, sha1) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)",
                        (channel_id, message_id, att_index, filename, info.get("name"), info.get("role"),
                         info.get("host"), info.get("start"), info.get("end"), info.get("day", ""),
                         json.dumps(sorted(info.get("peers", ()))), time.time(), info.get("sha1", "")))
        self.db.commit()

    def logs(self, channel_id):
        rows = self.db.execute("SELECT message_id, att_index, filename, name, role, host, start, end_, day, peers, "
                               "sha1 FROM logs WHERE channel_id=? ORDER BY message_id, att_index", (channel_id,))
        out = []
        for mid, idx, fn, name, role, host, start, end, day, peers, sha in rows:
            out.append({"message_id": mid, "att_index": idx, "filename": fn, "name": name, "role": role,
                        "host": host, "start": start, "end": end, "day": day or "",
                        "peers": set(json.loads(peers or "[]")), "sha1": sha or ""})
        return out

    def find_session(self, channel_id, key, members):
        """The stored session for this group: same key, or any log in common (a later host log can move
        the key's time window). Returns (key, bot_message_id, log_count) or None."""
        want = {f"{m}:{i}" for m, i in members}
        for k, mid, count, mem in self.db.execute(
                "SELECT session_key, bot_message_id, log_count, members FROM sessions WHERE channel_id=?",
                (channel_id,)):
            if k == key or want & set(json.loads(mem or "[]")):
                return k, mid, count
        return None

    def set_session(self, channel_id, key, bot_message_id, members, old_key=None):
        if old_key and old_key != key:
            self.db.execute("DELETE FROM sessions WHERE channel_id=? AND session_key=?", (channel_id, old_key))
        self.db.execute("INSERT OR REPLACE INTO sessions VALUES (?,?,?,?,?,?)",
                        (channel_id, key, bot_message_id, len(members), time.time(),
                         json.dumps(sorted(f"{m}:{i}" for m, i in members))))
        self.db.commit()

    def put_logdata(self, data, filename, packed=None):
        """Store one log file compressed; returns its content hash (the key). `packed` = pack(data) made
        elsewhere (e.g. in a worker thread: this connection must only be used from its own thread)."""
        sha, blob = packed or pack(data)
        if self.db.execute("SELECT 1 FROM logdata WHERE sha1=?", (sha,)).fetchone():
            self.db.execute("UPDATE logdata SET created=? WHERE sha1=?", (time.time(), sha))
        else:
            self.db.execute("INSERT INTO logdata VALUES (?,?,?,?,?)", (sha, filename, len(data), blob, time.time()))
        self.db.commit()
        return sha

    def get_logdata(self, sha):
        """(filename, bytes) of a stored log, or None."""
        import lzma
        row = self.db.execute("SELECT filename, blob FROM logdata WHERE sha1=?", (sha,)).fetchone()
        return (row[0], lzma.decompress(row[1])) if row else None

    def add_analysis(self, ref, channel_id, guild_id, bot_message_id, files, text):
        """One analysis under its reference (re-running a session keeps its reference, newest text)."""
        self.db.execute("INSERT OR REPLACE INTO analyses VALUES (?,?,?,?,?,?,?)",
                        (ref, channel_id, guild_id, bot_message_id, json.dumps(files), text[:200000],
                         time.time()))
        self.db.commit()

    def get_analysis(self, ref):
        row = self.db.execute("SELECT ref, channel_id, guild_id, bot_message_id, files, text, created "
                              "FROM analyses WHERE ref=?", (ref,)).fetchone()
        if not row:
            return None
        keys = ("ref", "channel_id", "guild_id", "bot_message_id", "files", "text", "created")
        d = dict(zip(keys, row))
        d["files"] = json.loads(d["files"] or "[]")
        return d

    def recent_analyses(self, limit=20):
        return self.db.execute("SELECT ref, channel_id, created, substr(text, 1, 80) FROM analyses "
                               "ORDER BY created DESC LIMIT ?", (limit,)).fetchall()

    def size_bytes(self):
        pages = self.db.execute("PRAGMA page_count").fetchone()[0]
        return pages * self.db.execute("PRAGMA page_size").fetchone()[0]

    def prune(self, days=30, max_bytes=256 * 1024 * 1024):
        """Drop rows older than `days`; if the file is still over `max_bytes` (hard ceiling 1 GiB), drop
        the oldest rows until it is under 80% of it, then compact the file. A row is ~200 bytes, so the
        size cap is a safety net, not the normal path."""
        max_bytes = min(max_bytes, 1024 * 1024 * 1024)
        cut = time.time() - days * 86400
        self.db.execute("DELETE FROM logs WHERE posted < ?", (cut,))
        self.db.execute("DELETE FROM sessions WHERE updated < ?", (cut,))
        self.db.execute("DELETE FROM analyses WHERE created < ?", (cut,))
        self.db.execute("DELETE FROM logdata WHERE created < ?", (cut,))
        self.db.commit()
        if self.size_bytes() > max_bytes * 0.8:
            self.db.execute("VACUUM")
        while self.size_bytes() > max_bytes * 0.8:
            n = self.db.execute("SELECT COUNT(*) FROM logs").fetchone()[0]
            m = self.db.execute("SELECT COUNT(*) FROM sessions").fetchone()[0]
            if n == 0 and m == 0 and \
                    self.db.execute("SELECT COUNT(*) FROM analyses").fetchone()[0] == 0 and \
                    self.db.execute("SELECT COUNT(*) FROM logdata").fetchone()[0] == 0:
                break
            self.db.execute("DELETE FROM logs WHERE rowid IN (SELECT rowid FROM logs ORDER BY posted LIMIT ?)",
                            (max(1, n // 4),))
            self.db.execute("DELETE FROM sessions WHERE rowid IN "
                            "(SELECT rowid FROM sessions ORDER BY updated LIMIT ?)", (max(1, m // 4),))
            b = self.db.execute("SELECT COUNT(*) FROM logdata").fetchone()[0]
            self.db.execute("DELETE FROM logdata WHERE rowid IN "
                            "(SELECT rowid FROM logdata ORDER BY created LIMIT ?)", (max(1, b // 4),))
            k = self.db.execute("SELECT COUNT(*) FROM analyses").fetchone()[0]
            self.db.execute("DELETE FROM analyses WHERE rowid IN "
                            "(SELECT rowid FROM analyses ORDER BY created LIMIT ?)", (max(1, k // 4),))
            self.db.commit()
            self.db.execute("VACUUM")
        return self.size_bytes()


def pack(data):
    """(content hash, LZMA-compressed bytes) of a log file. Pure: safe in any thread."""
    import lzma
    return hashlib.sha1(data).hexdigest(), lzma.compress(data, preset=6)


def new_ref():
    import secrets
    return "ND-" + secrets.token_hex(4)


def same_session(a, b):
    """Two logs are one party session: linked by player names and compatible in time.

    start/end are minutes (net_doctor _minutes); end is known for Nightwatch logs (file name = close time).
    """
    if a["host"] and b["host"] and a["host"] != b["host"]:
        return False  # two different parties
    named = bool((a["name"] and a["name"] in b["peers"]) or (b["name"] and b["name"] in a["peers"])
                 or (a["host"] and a["host"] == b["host"]))
    if a["start"] is not None and b["start"] is not None:
        if abs(a["start"] - b["start"]) > 180:
            return False
        if a["end"] is not None and b["start"] > a["end"] + 1:
            return False
        if b["end"] is not None and a["start"] > b["end"] + 1:
            return False
        return named or abs(a["start"] - b["start"]) <= 20
    return named


def group_sessions(infos):
    """Lists of indexes into infos, one list per session, oldest session first."""
    parent = list(range(len(infos)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    for i in range(len(infos)):
        for j in range(i + 1, len(infos)):
            if same_session(infos[i], infos[j]):
                parent[find(i)] = find(j)
    groups = {}
    for i in range(len(infos)):
        groups.setdefault(find(i), []).append(i)
    return sorted(groups.values(), key=lambda g: min((infos[i]["start"] or 0) for i in g))


def session_key(infos, group):
    """Stable, name-free id of a session: host + day + 3-hour window, hashed. Adding another player's log
    of the same session keeps the key (the host is named in every log of it)."""
    hosts = sorted({infos[i]["host"] for i in group if infos[i]["host"]})
    who = hosts[0] if hosts else sorted((infos[i]["name"] or "") for i in group)[0]
    days = sorted(infos[i]["day"] for i in group if infos[i]["day"])
    starts = [infos[i]["start"] for i in group if infos[i]["start"] is not None]
    window = (min(starts) // 180) if starts else 0
    return hashlib.sha1(f"{who}|{days[0] if days else ''}|{window}".encode()).hexdigest()[:10]
