"""netdoctor bot memory: which log files were posted where, who/when each one is, and which bot message
holds each session's analysis. SQLite (standard library). The log files themselves are never stored:
they stay on Discord and are downloaded again for each analysis.

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
CREATE TABLE IF NOT EXISTS sessions (
    channel_id INTEGER NOT NULL, session_key TEXT NOT NULL, bot_message_id INTEGER,
    log_count INTEGER, updated REAL, members TEXT,
    PRIMARY KEY (channel_id, session_key));
"""


class Store:
    def __init__(self, path):
        self.db = sqlite3.connect(str(path))
        self.db.executescript(SCHEMA)
        self.db.commit()

    def has_log(self, channel_id, message_id, att_index):
        return self.db.execute("SELECT 1 FROM logs WHERE channel_id=? AND message_id=? AND att_index=?",
                               (channel_id, message_id, att_index)).fetchone() is not None

    def add_log(self, channel_id, message_id, att_index, filename, info):
        self.db.execute("INSERT OR REPLACE INTO logs VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                        (channel_id, message_id, att_index, filename, info.get("name"), info.get("role"),
                         info.get("host"), info.get("start"), info.get("end"), info.get("day", ""),
                         json.dumps(sorted(info.get("peers", ()))), time.time()))
        self.db.commit()

    def logs(self, channel_id):
        rows = self.db.execute("SELECT message_id, att_index, filename, name, role, host, start, end_, day, peers "
                               "FROM logs WHERE channel_id=? ORDER BY message_id, att_index", (channel_id,))
        out = []
        for mid, idx, fn, name, role, host, start, end, day, peers in rows:
            out.append({"message_id": mid, "att_index": idx, "filename": fn, "name": name, "role": role,
                        "host": host, "start": start, "end": end, "day": day or "",
                        "peers": set(json.loads(peers or "[]"))})
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
        self.db.commit()
        while self.size_bytes() > max_bytes * 0.8:
            n = self.db.execute("SELECT COUNT(*) FROM logs").fetchone()[0]
            m = self.db.execute("SELECT COUNT(*) FROM sessions").fetchone()[0]
            if n == 0 and m == 0:
                break
            self.db.execute("DELETE FROM logs WHERE rowid IN (SELECT rowid FROM logs ORDER BY posted LIMIT ?)",
                            (max(1, n // 4),))
            self.db.execute("DELETE FROM sessions WHERE rowid IN "
                            "(SELECT rowid FROM sessions ORDER BY updated LIMIT ?)", (max(1, m // 4),))
            self.db.commit()
            self.db.execute("VACUUM")
        return self.size_bytes()


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
