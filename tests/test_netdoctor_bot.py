"""tools/discord/netdoctor_bot.py: the parts that run without Discord (diagnose, message splitting)."""
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("netdoctor_bot", ROOT / "tools" / "discord" / "netdoctor_bot.py")
bot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bot)

LOG = """Nightwatch Co-op v0.2.3 beta 3, engine x, 2026-10-24 20:39:01
[coop] Net: party networking on (host, as HostA, port 9307)
[coop] Party: NAT: cone (no open port); no open port (cone NAT): guests join by UDP hole punching
[coop] Party: public address 198.51.100.7:9307 (stun x)
"""


def test_diagnose_redacts_by_default(tmp_path):
    p = tmp_path / "nightwatch-20261024_204859.log"
    p.write_text(LOG, encoding="utf-8")
    pub = bot.diagnose([p], redact=True)
    assert "HostA" not in pub and "Host (host)" in pub
    priv = bot.diagnose([p], redact=False)
    assert "HostA (host)" in priv


def test_split_keeps_code_blocks_closed():
    text = "a\n```\n" + ("x" * 50 + "\n") * 100 + "```\nend\n"
    parts = bot.split_message(text, limit=500)
    assert len(parts) > 1
    assert all(len(p) <= 500 + 4 + 4 for p in parts)
    assert all(p.count("```") % 2 == 0 for p in parts)
    assert "".join(parts).count("x") == 5000


def test_log_names():
    for n in ("nightwatch-20261024_204859.log", "last_session.log", "previous_session(6).log",
              "session-20261025-155742.log", "message(21).txt", "host_2048.log"):
        assert bot.is_log_name(n), n
    for n in ("screenshot.png", "saves.zip", "userdata0010", "notes.txt"):
        assert not bot.is_log_name(n), n


def _store_mod():
    s = importlib.util.spec_from_file_location("netdoctor_store", ROOT / "tools" / "discord" / "netdoctor_store.py")
    m = importlib.util.module_from_spec(s)
    s.loader.exec_module(m)
    return m


def _info(name, role, host, start, end=None, peers=()):
    return {"name": name, "role": role, "host": host, "start": start, "end": end, "day": "2026-10-25",
            "peers": set(peers)}


def test_sessions_grouping_and_key():
    ns = _store_mod()
    host = _info("H", "host", "H", 600, 660, peers=("G1", "G2"))
    g1 = _info("G1", "guest", "H", 610, 650, peers=("H",))
    later = _info("G2", "guest", "H", 900, peers=("H",))           # same host, 5 h later: another session
    other = _info("X", "host", "X", 605, peers=())                   # different party at the same time
    groups = ns.group_sessions([host, g1, later, other])
    assert sorted(map(sorted, groups)) == [[0, 1], [2], [3]]
    infos = [host, g1]
    assert ns.session_key(infos, [1]) == ns.session_key(infos, [0, 1])  # adding the host log keeps the key


def test_store_session_by_members(tmp_path):
    ns = _store_mod()
    st = ns.Store(tmp_path / "db.sqlite")
    st.add_log(1, 10, 0, "a.log", _info("G1", "guest", "H", 610))
    assert st.has_log(1, 10, 0) and not st.has_log(1, 10, 1)
    st.set_session(1, "k1", 555, [(10, 0)])
    assert st.find_session(1, "k2", [(10, 0), (11, 0)]) == ("k1", 555, 1)   # shared log, new key
    assert st.find_session(1, "k3", [(12, 0)]) is None


def test_wants_analysis():
    assert bot.wants_analysis("<@42> analyse", 42)
    assert bot.wants_analysis("<@42>", 42)
    assert bot.wants_analysis("hey <@!42> can you check these logs?", 42)
    assert bot.wants_analysis("<@42> testing iosefka unique hash", 42)
    assert not bot.wants_analysis("<@42> thanks!", 42)
    assert not bot.wants_analysis("thank you <@42>", 42)


def test_store_size_cap(tmp_path):
    ns = _store_mod()
    st = ns.Store(tmp_path / "db.sqlite")
    for i in range(3000):
        st.add_log(1, i, 0, "x" * 200 + ".log", _info("G%d" % i, "guest", "H", 600, peers=("P" * 100,)))
    before = st.size_bytes()
    after = st.prune(days=30, max_bytes=before // 2)
    assert after <= before // 2 * 0.8 + 65536
    assert 0 < len(st.logs(1)) < 3000          # oldest dropped, newest kept


def test_analysis_ref_roundtrip(tmp_path):
    ns = _store_mod()
    st = ns.Store(tmp_path / "db.sqlite")
    ref = ns.new_ref()
    assert ref.startswith("ND-") and len(ref) == 11
    st.add_analysis(ref, 5, 7, 99, [{"channel": 5, "message": 10, "index": 0, "url": "", "filename": "a.log"}],
                    "**Co-op connection check**")
    rec = st.get_analysis(ref)
    assert rec["bot_message_id"] == 99 and rec["files"][0]["filename"] == "a.log"
    assert st.get_analysis("ND-00000000") is None
    assert st.recent_analyses()[0][0] == ref


def test_logdata_compressed(tmp_path):
    ns = _store_mod()
    st = ns.Store(tmp_path / "db.sqlite")
    data = ("[coop] Party: roster: HostA(slot 0, home, map 1, 0 ms)\n" * 20000).encode()
    sha = st.put_logdata(data, "nightwatch-20261025_120000.log")
    assert st.put_logdata(data, "copy.log") == sha                 # deduplicated by content
    name, back = st.get_logdata(sha)
    assert back == data and name == "nightwatch-20261025_120000.log"
    assert st.size_bytes() < len(data) // 10                       # stored compressed


def test_event_handlers_are_coroutines():
    import ast
    tree = ast.parse((ROOT / "tools" / "discord" / "netdoctor_bot.py").read_text(encoding="utf-8"))
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            for d in node.decorator_list:
                if ast.unparse(d) == "client.event":
                    assert isinstance(node, ast.AsyncFunctionDef), node.name
