"""Developer posts: visy's short messages to a dev channel (e.g. "PR opened for you"), posted by the bot.

Standard library only. visy queues one with the CLI (`iosefka dev-post --channel development --mention vladtepes22`,
the text on stdin); the bot's outbox loop (every 60 s) posts each pending row once in the named text channel. The
mention is a member's user name (looked up in the guild by the bot, never stored as an id here); only that member is
pinged (no @everyone / roles). A row whose channel or member is missing stays pending and is reported in the bot log.
Rows live in nw_devpost; `dev-post --list` shows the last ones, `dev-post --cancel ID` drops a pending one.
"""
import re
import time

SCHEMA = """CREATE TABLE IF NOT EXISTS nw_devpost (
    id INTEGER PRIMARY KEY, channel TEXT, mention TEXT, text TEXT, created REAL, by TEXT, sent REAL, cancelled REAL)"""
MAX_TEXT = 1800
NAME_RX = re.compile(r"^[a-z0-9_.-]{1,32}$")


def ensure(clinic):
    clinic.db.execute(SCHEMA)
    clinic.db.commit()


def queue(clinic, channel, text, mention="", by="", now=None):
    ensure(clinic)
    if not channel or not all(c.isalnum() or c in "-_" for c in channel):
        raise ValueError("channel: a channel name like development")
    mention = (mention or "").strip().lstrip("@").lower()
    if mention and not NAME_RX.match(mention):
        raise ValueError("mention: a Discord user name like vladtepes22")
    text = (text or "").strip()
    if not text:
        raise ValueError("no text (give it on stdin)")
    if len(text) > MAX_TEXT:
        raise ValueError(f"text too long ({len(text)} > {MAX_TEXT} characters)")
    if "@everyone" in text or "@here" in text:
        raise ValueError("no @everyone / @here")
    cur = clinic.db.execute("INSERT INTO nw_devpost (channel, mention, text, created, by) VALUES (?,?,?,?,?)",
                            (channel, mention, text, now or time.time(), by))
    clinic.db.commit()
    return cur.lastrowid


def _rows(cur):
    keys = ("id", "channel", "mention", "text", "created", "by", "sent", "cancelled")
    return [dict(zip(keys, r)) for r in cur]


def pending(clinic):
    ensure(clinic)
    return _rows(clinic.db.execute("SELECT id, channel, mention, text, created, by, sent, cancelled FROM nw_devpost "
                                   "WHERE sent IS NULL AND cancelled IS NULL ORDER BY id"))


def rows(clinic, limit=10):
    ensure(clinic)
    return _rows(clinic.db.execute("SELECT id, channel, mention, text, created, by, sent, cancelled FROM nw_devpost "
                                   "ORDER BY id DESC LIMIT ?", (limit,)))


def cancel(clinic, pid, now=None):
    ensure(clinic)
    cur = clinic.db.execute("UPDATE nw_devpost SET cancelled=? WHERE id=? AND sent IS NULL AND cancelled IS NULL",
                            (now or time.time(), pid))
    clinic.db.commit()
    if cur.rowcount != 1:
        raise ValueError(f"dev post {pid} is not pending")


def mark_sent(clinic, pid, now=None):
    clinic.db.execute("UPDATE nw_devpost SET sent=? WHERE id=?", (now or time.time(), pid))
    clinic.db.commit()


def resolve_id(clinic, name):
    """A member's Discord id from the clinic's own message archive (author_name), newest first; None if never seen.
    The bot runs without the members intent, so a guild lookup by name can miss people who have not spoken lately."""
    want = (name or "").strip().lstrip("@").lower().lstrip(".")
    if not want:
        return None
    for aid, an in clinic.db.execute("SELECT author_id, author_name FROM nw_messages WHERE author_name IS NOT NULL "
                                     "ORDER BY created DESC"):
        if (an or "").lower().lstrip(".") == want and aid and aid > 0:
            return aid
    return None
