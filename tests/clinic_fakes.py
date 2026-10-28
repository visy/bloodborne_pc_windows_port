"""Fake Discord objects for the clinic tests (no network, no discord.py). Invented names only."""
import datetime as dt

T0 = dt.datetime(2026, 10, 25, 20, 0, 0, tzinfo=dt.timezone.utc)


class NotFound(Exception):
    pass


class Att:
    def __init__(self, id, filename, data=b"x", fail=0, size=None, content_type=None):
        self.id, self.filename, self.data, self.fail = id, filename, data, fail
        self.size = len(data) if size is None else size
        self.content_type = content_type
        self.url = f"https://cdn.invalid/{id}"
        self.saves = 0

    async def save(self, path):
        self.saves += 1
        if self.fail > 0:
            self.fail -= 1
            raise OSError("connection reset")
        with open(path, "wb") as f:
            f.write(self.data)

    async def to_file(self):
        return ("file", self.id, self.filename)


class User:
    def __init__(self, id, name="Poster"):
        self.id, self.name = id, name


class Guild:
    def __init__(self, text_channels=(), forums=(), filesize_limit=25 * 1024 * 1024):
        self.text_channels, self.forums, self.filesize_limit = list(text_channels), list(forums), filesize_limit


class Channel:
    """A text channel; `calls` is a shared list recording ("send"/"delete"/"edit", ...) in order."""
    _next = [9000]

    def __init__(self, id, name, calls=None, guild="auto"):
        self.id, self.name = id, name
        self.calls = calls if calls is not None else []
        self.guild = Guild() if guild == "auto" else guild
        self.sent = []
        self.threads = []
        self.private_archived = []
        self.public_archived = []
        self.messages = []

    async def send(self, content=None, **kw):
        Channel._next[0] += 1
        m = Msg(Channel._next[0], self, User(1, "Iosefka"), content or "")
        m.kw = kw
        self.sent.append(m)
        self.calls.append(("send", self.id, m.id))
        return m

    def archived_threads(self, private=False, limit=None):
        items = self.private_archived if private else self.public_archived

        async def gen():
            for t in items:
                yield t
        return gen()

    async def fetch_message(self, mid):
        for m in self.messages:
            if m.id == mid and not m.deleted:
                return m
        raise NotFound(f"unknown message {mid}")

    def history(self, limit=100, around=None, **kw):
        async def gen():
            for m in self.messages:
                if not m.deleted:
                    yield m
        return gen()


class Forum(Channel):
    available_tags = []

    def archived_threads(self, limit=None):  # forums have no private= argument
        async def gen():
            for t in self.public_archived:
                yield t
        return gen()


class Thread(Channel):
    def __init__(self, id, name, parent, calls=None, archived=False, locked=False):
        super().__init__(id, name, calls, parent.guild)
        self.parent, self.parent_id = parent, parent.id
        self.archived, self.locked = archived, locked

    async def edit(self, **kw):
        self.calls.append(("edit", self.id, dict(kw)))
        for k, v in kw.items():
            setattr(self, k, v)


class Msg:
    def __init__(self, id, channel, author, content="", attachments=(), minutes=0, reply_to=None, guild="auto"):
        self.id, self.channel, self.author, self.content = id, channel, author, content
        self.attachments = list(attachments)
        self.message_snapshots = []
        self.created_at = T0 + dt.timedelta(minutes=minutes)
        self.edited_at = None
        self.reference = type("Ref", (), {"message_id": reply_to})() if reply_to else None
        self.guild = getattr(channel, "guild", None) if guild == "auto" else guild
        self.thread = None
        self.deleted = False
        self.delete_error = None
        self.kw = {}

    async def delete(self):
        if self.delete_error:
            raise self.delete_error
        self.deleted = True
        self.channel.calls.append(("delete", self.channel.id, self.id))


class Client:
    def __init__(self, guilds=(), channels=()):
        self.guilds = list(guilds)
        self._ch = {c.id: c for c in channels}

    def get_channel(self, cid):
        return self._ch.get(cid)

    async def fetch_channel(self, cid):
        if cid not in self._ch:
            raise NotFound("unknown channel")
        return self._ch[cid]
