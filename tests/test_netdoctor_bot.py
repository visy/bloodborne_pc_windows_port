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
