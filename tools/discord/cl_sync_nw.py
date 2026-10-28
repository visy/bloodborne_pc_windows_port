"""Sync Nightwatch git history into Iosefka's changelists (track nw). Runs on a dev PC, never on the
server: reads `git log`, builds JSONL and pipes it to `ssh <host> cl import`. Standard library only.

    python cl_sync_nw.py <repo> [--dry-run] [--host visy@quad.fi]
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from clinic_changes import clean_text, norm_version, version_key  # noqa: E402

PREFIX_RX = re.compile(r"^(\d+\.\d+\.\d+(?: (?:alpha|beta|rc) ?\d+)?):\s*(.+)", re.I)
BUG_RX = re.compile(r"NW-[0-9a-f]{7}")
LOG_FORMAT = "%H%x1f%cI%x1f%s%x1f%b%x1e"


def _section(rest):
    word = (rest.split() or [""])[0].lower().strip(":,.")
    if word in ("fix", "fixed", "fixes"):
        return "fixed"
    if word in ("add", "added", "new"):
        return "added"
    return "changed"


def parse_log(text):
    """(item lines, version lines) from git log --format=%H%x1f%cI%x1f%s%x1f%b%x1e output."""
    items, dates = [], {}
    for rec in text.split("\x1e"):
        rec = rec.strip("\r\n")
        if not rec.strip():
            continue
        parts = rec.split("\x1f", 3)
        if len(parts) < 3:
            continue
        sha, when, subj = parts[0].strip(), parts[1].strip(), parts[2].strip()
        body = parts[3] if len(parts) > 3 else ""
        m = PREFIX_RX.match(subj)
        if not m:
            continue
        try:
            version = norm_version(m.group(1))
            rest = m.group(2).strip()[:300]
            msg = clean_text(rest)
        except ValueError as e:
            print(f"skipped {sha[:12]}: {e}", file=sys.stderr)
            continue
        bugs = list(dict.fromkeys(BUG_RX.findall(subj + "\n" + body)))
        item = {"kind": "item", "source_key": f"git:{sha}", "track": "nw", "version": version,
                "section": _section(rest), "text": msg, "status": "done", "commits": [sha[:12]]}
        if bugs:
            item["bugs"] = bugs
        items.append(item)
        day = when[:10]
        if day > dates.get(version, ""):
            dates[version] = day
    vers = []
    newest = max(dates, key=version_key) if dates else None
    for v in sorted(dates, key=version_key):
        line = {"kind": "version", "track": "nw", "version": v}
        if v == newest:
            line["status"] = "current"
        else:
            line["status"] = "released"
            line["released"] = dates[v]
        vers.append(line)
    return items, vers


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("repo")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--host", default="visy@quad.fi")
    a = ap.parse_args(argv)
    git = subprocess.run(["git", "-C", a.repo, "log", "--format=" + LOG_FORMAT], capture_output=True)
    if git.returncode:
        print(git.stderr.decode("utf-8", "replace"), file=sys.stderr)
        return 1
    items, vers = parse_log(git.stdout.decode("utf-8", "replace"))
    data = "".join(json.dumps(r, ensure_ascii=False) + "\n" for r in vers + items)
    if a.dry_run:
        sys.stdout.write(data)
        return 0
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", a.host, "cl", "import"],
                       input=data.encode("utf-8"), capture_output=True)
    sys.stdout.write(r.stdout.decode("utf-8", "replace"))
    if r.returncode:
        sys.stderr.write(r.stderr.decode("utf-8", "replace"))
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
