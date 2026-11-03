"""tools/discord/clinic_staff.py: /clinic staff commands (access, filters, pages, files, audit). Synthetic data only."""
import asyncio
import io
import os
import sys
import time
import zipfile
from pathlib import Path

import pytest

DISCORD = Path(__file__).resolve().parents[1] / "tools" / "discord"
sys.path.insert(0, str(DISCORD))
import clinic_changes  # noqa: E402
import clinic_staff as cs  # noqa: E402
import clinic_store  # noqa: E402

GUILD = 555000000000000001
T = [1000.0]


class Role:
    def __init__(self, id, name):
        self.id, self.name = id, name


class Member:
    def __init__(self, id, name="u", roles=()):
        self.id, self.name, self.roles = id, name, list(roles)


class FGuild:
    def __init__(self, id=GUILD, members=(), limit=8 * 1024 * 1024):
        self.id, self.members, self.filesize_limit = id, {m.id: m for m in members}, limit
        self.fetches = 0

    def get_member(self, uid):
        return None

    async def fetch_member(self, uid):
        self.fetches += 1
        if uid not in self.members:
            raise LookupError("unknown member")
        return self.members[uid]


class FClient:
    def __init__(self, guild):
        self.g = guild
        self.guilds = [guild]

    def get_guild(self, gid):
        return self.g if gid == self.g.id else None


class Inter:
    def __init__(self, user, guild=None):
        self.user, self.guild = user, guild


def run(coro):
    return asyncio.run(coro)


MOD, DEV, PLAYER = Role(1, "Mod"), Role(2, "dev"), Role(3, "Member")


def gate(**kw):
    return cs.Gate(cs.Config(guild_id=GUILD, **kw), clock=lambda: T[0])


# ---- permissions

def test_default_roles_are_exactly_mod_and_dev():
    g = gate()
    cl = FClient(FGuild())
    for role, ok in ((MOD, True), (DEV, True), (PLAYER, False), (Role(9, "Moderator-ish"), False),
                     (Role(10, "staff"), False)):
        assert run(g.access(Inter(Member(5, roles=[role]), FGuild()), cl)).read is ok, role.name
    assert not run(g.access(Inter(Member(5), FGuild()), cl)).read


def test_env_override_by_id_name_and_user():
    cfg = cs.Config.from_env({"NETDOCTOR_STAFF_ROLES": "77, Helper", "NETDOCTOR_STAFF_USERS": "42",
                              "NETDOCTOR_GUILD": str(GUILD)})
    g = cs.Gate(cfg)
    cl = FClient(FGuild())
    assert run(g.access(Inter(Member(5, roles=[Role(77, "x")]), FGuild()), cl)).read
    assert run(g.access(Inter(Member(5, roles=[Role(1, "HELPER")]), FGuild()), cl)).read
    assert not run(g.access(Inter(Member(5, roles=[MOD]), FGuild()), cl)).read   # override replaces defaults
    assert run(g.access(Inter(Member(42), FGuild()), cl)).read


def test_wrong_guild_refused():
    assert not run(gate().access(Inter(Member(5, roles=[MOD]), FGuild(id=1)), FClient(FGuild()))).read


def test_dm_path_fetches_member_and_caches():
    guild = FGuild(members=[Member(5, roles=[MOD]), Member(6, roles=[PLAYER])])
    cl, g = FClient(guild), gate()
    assert run(g.access(Inter(Member(5)), cl)).read
    assert run(g.access(Inter(Member(5)), cl)).read
    assert guild.fetches == 1                              # cached
    T[0] += cs.MEMBER_TTL + 1
    assert run(g.access(Inter(Member(5)), cl)).read
    assert guild.fetches == 2
    assert not run(g.access(Inter(Member(6)), cl)).read    # member without the role
    assert not run(g.access(Inter(Member(7)), cl)).read    # not in the guild at all


def test_dm_listed_user_without_fetch():
    guild = FGuild()
    assert run(gate(users={9}).access(Inter(Member(9)), FClient(guild))).read
    assert guild.fetches == 0


def test_write_is_separate_and_off_by_default():
    cl = FClient(FGuild())
    g = gate()
    assert not g.cfg.writes_enabled
    assert not run(g.access(Inter(Member(5, roles=[MOD]), FGuild()), cl)).write
    g2 = gate(write_roles=["Dev"])
    assert g2.cfg.writes_enabled
    assert run(g2.access(Inter(Member(5, roles=[DEV]), FGuild()), cl)).write
    assert not run(g2.access(Inter(Member(5, roles=[MOD]), FGuild()), cl)).write
    assert run(g2.access(Inter(Member(5, roles=[DEV]), FGuild()), cl)).read


def test_rate_limit():
    g = gate()
    T[0] = 5000.0
    assert all(g.allow_rate(1) for _ in range(12)) and not g.allow_rate(1)
    assert g.allow_rate(2)
    T[0] += 61
    assert g.allow_rate(1)
    assert [g.allow_rate(3, heavy=True) for _ in range(5)] == [True] * 4 + [False]


# ---- data

@pytest.fixture
def clinic(tmp_path):
    return clinic_store.Clinic(tmp_path / "c.db")


_mid = [1000]


def mk(clinic, title, status="open", reporter=111111111111111111, tags=None, name="Alice", created=None, logs=()):
    b = clinic.new_bug(title, "forum post", 10, 20, reporter, tags=tags)
    clinic.update_bug(b, status=status)
    atts = []
    for fname, data, kind in logs:
        sha = clinic.put_file(data, fname, kind)
        atts.append({"sha256": sha, "filename": fname, "kind": kind, "size": len(data)})
    _mid[0] += 1
    clinic.upsert_message({"message_id": _mid[0], "channel_id": 10, "thread_id": 20, "author_id": reporter,
                           "author_name": name, "created": created or time.time(),
                           "content": "crash at 8.8.8.8 help", "attachments": atts, "bug_id": b,
                           "state": "archived"})
    return b


def test_filters(clinic):
    ch = clinic_changes.Changes(clinic)
    a = mk(clinic, "Net drop", tags={"triage": "network", "component": "coop-mod"}, name="Alice",
           created=time.time() - 40 * 86400)
    b = mk(clinic, "Crash boss", status="fixed", reporter=222222222222222222, tags={"triage": "crash"}, name="Bob")
    c = mk(clinic, "Other", status="waiting-logs", name="Carol", reporter=333333333333333333)
    ch.add("coop", "0.7.8", "fixed", "fix boss crash", who="t", bugs=[b])

    def f(**k):
        return [x["id"] for x in cs.find_bugs(clinic, ch, **k)]
    assert set(f()) == {a, b, c}
    assert f(status=["fixed"]) == [b]
    assert f(triage="network") == [a]
    assert f(area="coop") == [a]
    assert f(version="0.7.8") == [b]
    assert f(reporter="bob") == [b]
    assert f(reporter="<@222222222222222222>") == [b]
    assert set(f(since="7d")) == {b, c}
    assert f(query="boss") == [b]
    assert f(status=["open"], reporter="alice") == [a]
    with pytest.raises(ValueError):
        f(since="yesterday")
    assert [x["id"] for x in cs.find_any(clinic, a.upper())] == [a]
    assert [x["id"] for x in cs.find_any(clinic, "carol")] == [c]
    assert [x["id"] for x in cs.find_any(clinic, "333333333333333333")] == [c]
    need = {x["id"] for x in cs.needs_logs(clinic)}
    assert c in need and b not in need


def test_find_by_player_name_in_log_info(clinic):
    b = mk(clinic, "Net", logs=[("a.log", b"hello", "log")])
    sha = clinic.files_for_bug(b)[0]["sha256"]
    clinic.put_loginfo(sha, {"name": "ZedPlayer", "peers": {"PeerQ"}, "role": "host"})
    assert [x["id"] for x in cs.find_any(clinic, "peerq")] == [b]
    assert cs.find_any(clinic, "nobodyhere") == []


def test_card_messages_result_masked(clinic):
    ch = clinic_changes.Changes(clinic)
    b = mk(clinic, "Net", tags={"triage": "network"})
    ch.add("coop", "0.7.8", "fixed", "fix net", who="t", bugs=[b])
    clinic.add_note(b, "iosefka", "triage", "peer 8.8.4.4 refused")
    clinic.db.execute("CREATE TABLE IF NOT EXISTS analyses (ref TEXT PRIMARY KEY, text TEXT)")
    clinic.db.execute("INSERT INTO analyses VALUES ('ND-0a0a0a0a', 'found 8.8.8.8 closed')")
    clinic.update_bug(b, nd_refs='["ND-0a0a0a0a"]')
    bug = clinic.bug(b)
    card = "\n".join(cs.bug_card(clinic, ch, bug, GUILD))
    assert "CL-" in card and "8.8.4.4" not in card and "<public IP>" in card
    msgs = "\n".join(cs.messages_pages(clinic, bug, GUILD))
    assert f"discord.com/channels/{GUILD}/20/" in msgs and "8.8.8.8" not in msgs
    res = "\n".join(cs.result_pages(clinic, bug, lambda r: cs._analysis(clinic, r)))
    assert "ND-0a0a0a0a" in res and "8.8.8.8" not in res


def test_paginate():
    pages = cs.paginate([f"line {i} " + "x" * 90 for i in range(100)])
    assert len(pages) > 3 and all(len(p) <= cs.PAGE_CHARS for p in pages)
    assert sum(p.count("line ") for p in pages) == 100
    assert all(len(p) <= cs.PAGE_CHARS for p in cs.paginate(["y" * 5000]))
    assert cs.paginate([]) == ["(nothing)"]


# ---- files

def test_logs_are_masked_and_kind_filtered(clinic):
    b = mk(clinic, "L", logs=[("s.log", b"peer 8.8.8.8:9307 ok\nlocal 192.168.1.5\n", "log"),
                              ("p.png", b"\x89PNG....", "image")])
    logs = cs.gather_files(clinic, b, "logs")
    assert len(logs) == 1 and b"8.8.8.8" not in logs[0]["data"] and b"192.x.x.x" in logs[0]["data"]
    assert [f["kind"] for f in cs.gather_files(clinic, b, "images")] == ["image"]
    assert len(cs.gather_files(clinic, b, "all")) == 2


def imgs(n, size):
    return [{"name": f"f{i}.png", "data": os.urandom(size), "kind": "image", "sha": str(i)} for i in range(n)]


def test_plan_small_goes_raw():
    groups, skipped = cs.plan_attachments(imgs(3, 100), 1000)
    assert len(groups) == 1 and len(groups[0]) == 3 and not skipped


def test_plan_many_files_zipped_under_limit():
    groups, skipped = cs.plan_attachments(imgs(25, 100), 100000)
    assert not skipped and all(len(g) == 1 and g[0][0].endswith(".zip") for g in groups)
    n = 0
    for g in groups:
        assert len(g[0][1]) <= 100000
        n += len(zipfile.ZipFile(io.BytesIO(g[0][1])).namelist())
    assert n == 25


def test_plan_big_log_compresses_big_image_skipped():
    big_log = {"name": "big.log", "data": b"line of log\n" * 200000, "kind": "log", "sha": "a"}
    big_img = {"name": "big.png", "data": os.urandom(300000), "kind": "image", "sha": "b"}
    groups, skipped = cs.plan_attachments([big_log, big_img], 100000)
    assert skipped == ["big.png"]
    assert len(groups) == 1 and len(groups[0][0][1]) <= 100000


def test_plan_splits_when_over_size():
    groups, skipped = cs.plan_attachments(imgs(4, 60000), 150000)
    assert not skipped and all(len(g[0][1]) <= 150000 for g in groups) and len(groups) >= 2


def test_file_limit_uses_server_cap():
    cfg = cs.Config(max_mb=8)
    assert cs.file_limit(cfg, FGuild(limit=2 * 1024 * 1024)) == 2 * 1024 * 1024
    assert cs.file_limit(cfg, None) == 8 * 1024 * 1024


# ---- audit and write

def test_audit_records_no_contents(clinic):
    cs.audit(clinic, Member(5, "modA"), "list", filters=["query", "status"], allowed=True, where="dm")
    cs.audit(clinic, Member(6, "rando"), "show", target="NW-1234567", allowed=False)
    rows = cs.audit_rows(clinic)
    assert {r["user"] for r in rows} == {"modA", "rando"}
    r = next(r for r in rows if r["user"] == "modA")
    assert r["filters"] == "query,status" and r["where"] == "dm"
    assert next(r for r in rows if r["user"] == "rando")["allowed"] == 0


def test_staff_note_masked(clinic):
    b = mk(clinic, "N")
    cs.add_staff_note(clinic, b, "modA", "player at 8.8.8.8 retested")
    n = clinic.notes(b)[-1]
    assert n["who"] == "staff:modA" and "8.8.8.8" not in n["text"]


# ---- discord binding (needs discord.py)

def test_register_commands(clinic):
    discord = pytest.importorskip("discord")
    from discord import app_commands
    client = discord.Client(intents=discord.Intents.default())
    tree = app_commands.CommandTree(client)

    class Ctx:
        pass
    ctx = Ctx()
    ctx.clinic, ctx.guild_id = clinic, GUILD
    group = cs.register(tree, client, ctx, gate())
    names = {c.qualified_name for c in tree.walk_commands()}
    assert {"clinic list", "clinic show", "clinic messages", "clinic files", "clinic result", "clinic queue",
            "clinic find", "clinic mine", "clinic needlogs", "clinic changed", "clinic health",
            "clinic cl item", "clinic cl bug", "clinic cl version", "clinic cl list",
            "clinic cl versions", "clinic cl notes", "clinic cl related"} <= names
    assert "clinic note" not in names                      # write is off by default
    assert group.default_permissions is None               # visible to Mod without manage_messages
    tree2 = app_commands.CommandTree(discord.Client(intents=discord.Intents.default()))
    g2 = cs.register(tree2, client, ctx, gate(write_roles=["Dev"], hide_perm="manage_messages"))
    assert "clinic note" in {c.qualified_name for c in tree2.walk_commands()}
    assert g2.default_permissions.manage_messages


class FResp:
    def __init__(self):
        self.sent = []

    async def send_message(self, content=None, **kw):
        self.sent.append((content, kw))


class FInter(Inter):
    def __init__(self, user, guild):
        super().__init__(user, guild)
        self.response, self.guild_id = FResp(), getattr(guild, "id", None)


def test_handlers_refuse_and_answer(clinic):
    discord = pytest.importorskip("discord")
    from discord import app_commands
    client = discord.Client(intents=discord.Intents.default())
    tree = app_commands.CommandTree(client)

    class Ctx:
        pass
    ctx = Ctx()
    ctx.clinic, ctx.guild_id = clinic, GUILD
    b = mk(clinic, "Handler bug")
    cs.register(tree, client, ctx, gate())
    show = tree.get_command("clinic").get_command("show")

    guild = FGuild()
    bad = FInter(Member(8, "rando", [PLAYER]), guild)
    run(show.callback(bad, b))
    text, kw = bad.response.sent[0]
    assert text == cs.REFUSAL and kw["ephemeral"] and "Handler" not in text
    good = FInter(Member(5, "modA", [MOD]), guild)
    run(show.callback(good, b))
    text, kw = good.response.sent[0]
    assert "Handler bug" in text and kw["ephemeral"]
    rows = cs.audit_rows(clinic)
    assert [r["allowed"] for r in rows if r["user"] == "rando"] == [0]
    assert [r["allowed"] for r in rows if r["user"] == "modA"] == [1]


def test_changelist_views(clinic):
    ch = clinic_changes.Changes(clinic)
    b = mk(clinic, "Boss crash", tags={"component": "coop-mod"})
    ch.set_version("coop", "0.9.0", status="planned", who="t")
    i1 = ch.add("coop", "0.9.0", "added", "future thing 8.8.8.8", who="t", tags={"rank": "01", "component": "ui"},
                bugs=[b], dev="internal detail", commits=["abc1234"])
    i2 = ch.add("coop", "0.7.8", "fixed", "done thing", who="t", status="done", tags={"rank": "02"})
    i3 = ch.add("nw", "0.7.8", "fixed", "dropped thing", who="t", status="dropped")
    ch.add_note(i1, "t", "note", "history line")
    item = "\n".join(cs.cl_item_pages(ch, i1, clinic))
    assert "internal detail" in item and "abc1234" in item and "history line" in item and b in item
    assert "rank: 01" in item and "8.8.8.8" not in item
    assert cs.cl_item_pages(ch, "CL-ffffff") is None

    def ids(**k):
        return "\n".join(cs.cl_list_pages(ch, **k))
    assert i1 in ids(version="0.9.0") and i2 not in ids(version="0.9.0")        # future version visible
    assert i3 in ids(status="dropped") and i2 not in ids(status="dropped")
    assert i2 in ids(priority="02") and i1 not in ids(priority="02")
    assert i1 in ids(area="ui") and i2 not in ids(area="ui")
    assert i2 in ids(track="coop", text="done") and i3 not in ids(track="coop")
    vs = "\n".join(cs.cl_versions_pages(ch))
    assert "0.9.0 planned" in vs and "0.7.8" in vs
    assert "done thing" in cs.cl_notes_markdown(ch, "coop", "0.7.8")
    with pytest.raises(ValueError):
        cs.cl_notes_markdown(ch, "xx", "1")
    byb = "\n".join(cs.cl_for_ref_pages(ch, clinic, b))
    assert i1 in byb
    bycl = "\n".join(cs.cl_for_ref_pages(ch, clinic, i1))
    assert b in bycl
    assert cs.cl_for_ref_pages(ch, clinic, "NW-0000000") is None
