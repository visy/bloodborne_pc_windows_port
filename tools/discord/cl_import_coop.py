"""Import the coop port's GitHub release notes into Iosefka's changelists (track coop), and list
player_logs case folders no clinic text names. Runs on a dev PC. Standard library only.

    python cl_import_coop.py --releases > coop.jsonl          (then: ssh host cl import < coop.jsonl)
    python cl_import_coop.py --unmatched <player_logs dir> <names.json>
"""
import argparse
import json
import re
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from clinic_changes import clean_text, norm_version  # noqa: E402

API = "https://api.github.com/repos/visy/bloodborne_pc_windows_port/releases?per_page=100"
HEAD_RX = re.compile(r"^\s{0,3}#{2,3}\s+(.+?)\s*#*\s*$")
BULLET_RX = re.compile(r"^\s*[-*]\s+(.+?)\s*$")
DATE_RX = re.compile(r"^\d{4}-\d{2}-\d{2}$")
BS_W = r"\w"
SAFE_TEXT = "Change from a player report (details private)"


def _head_section(h):
    h = h.lower()
    if "fix" in h:
        return "fixed"
    if "add" in h or "new" in h:
        return "added"
    return "changed"


def _strip_md(s):
    s = re.sub(r"\*\*|__|`", "", s)
    return re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", s).strip()


def parse_release(tag, body):
    """Item lines from release markdown. Bullets before any heading, or under other headings, are changed."""
    version = norm_version(tag[1:] if tag[:1] in "vV" else tag)
    items, section = [], "changed"
    for line in (body or "").splitlines():
        h = HEAD_RX.match(line)
        if h:
            section = _head_section(h.group(1))
            continue
        b = BULLET_RX.match(line)
        if not b:
            continue
        raw = _strip_md(b.group(1))
        item = {"kind": "item", "source_key": f"release:{tag}:{len(items) + 1}", "track": "coop",
                "version": version, "section": section, "status": "done"}
        try:
            item["text"] = clean_text(raw[:300])
        except ValueError:
            item["text"] = SAFE_TEXT
            item["dev"] = raw[:2000]
        items.append(item)
    return items


def unmatched(player_logs, names):
    """Cases ("YYYY-MM-DD/case_folder", sorted) that no entry of names mentions. Layout is
    <player_logs>/<date>/<case>/; top-level dirs that are not dates are ignored. A case is named when a
    name equals "date/case" or the bare case folder, contains "date/case", or contains the folder as a
    whole word/path segment (case-insensitive, no loose substring)."""
    low = [n.lower() for n in names if isinstance(n, str) and n.strip()]
    out = []
    root = Path(player_logs)
    for day in sorted(root.iterdir()):
        if not day.is_dir() or not DATE_RX.match(day.name):
            continue
        for case in sorted(day.iterdir()):
            if not case.is_dir():
                continue
            key, c = f"{day.name}/{case.name}".lower(), case.name.lower()
            word = re.compile(r"(?<!"+BS_W+r")" + re.escape(c) + r"(?!"+BS_W+r")")
            if not any(n == key or n == c or key in n or word.search(n) for n in low):
                out.append(f"{day.name}/{case.name}")
    return out


def fetch_releases():
    req = urllib.request.Request(API, headers={"Accept": "application/vnd.github+json",
                                               "User-Agent": "iosefka-cl-import"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--releases", action="store_true")
    ap.add_argument("--unmatched", nargs=2, metavar=("PLAYER_LOGS_DIR", "NAMES_JSON"))
    a = ap.parse_args(argv)
    if a.unmatched:
        names = json.loads(Path(a.unmatched[1]).read_text(encoding="utf-8"))
        print(json.dumps(unmatched(a.unmatched[0], names), indent=1))
        return 0
    if not a.releases:
        ap.error("give --releases or --unmatched")
    for rel in reversed(fetch_releases()):
        tag = rel.get("tag_name") or ""
        if rel.get("draft"):
            continue
        try:
            items = parse_release(tag, rel.get("body") or "")
            version = norm_version(tag[1:] if tag[:1] in "vV" else tag)
        except ValueError as e:
            print(f"skipped {tag}: {e}", file=sys.stderr)
            continue
        vline = {"kind": "version", "track": "coop", "version": version, "status": "released",
                 "released": (rel.get("published_at") or "")[:10]}
        for r in [vline] + items:
            print(json.dumps(r, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
