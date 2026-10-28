"""Sync Nightwatch git history into Iosefka's changelists (track nw). Runs on a dev PC, never on the
server: reads `git log`, builds JSONL and pipes it to `ssh <host> cl import`. Standard library only.

    python cl_sync_nw.py <repo> [--dry-run] [--host visy@quad.fi]
    python cl_sync_nw.py <repo> --local ~/.config/netdoctor/netdoctor.db     (on quad.fi: straight into the DB)
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from clinic_changes import clean_text, norm_version, version_key  # noqa: E402

PREFIX_RX = re.compile(r"^(\d+\.\d+\.\d+(?:[ -](?:alpha|beta|rc)[ .]?\d+)?)\s*[:;(]\s*(.+)", re.I)
AREA_RX = re.compile(r"^[A-Za-z][\w /-]{0,30}:\s+")
NOISE_RX = re.compile(r'^(?:wip|merge|readme|docs)\b|^revert "', re.I)
BUG_RX = re.compile(r"NW-[0-9a-f]{7}")
LOG_FORMAT = "%H%x1f%cI%x1f%s%x1f%b%x1e"


def out_bytes(data):
    """Write bytes to stdout as is (UTF-8 whatever the console code page)."""
    buf = getattr(sys.stdout, "buffer", None)
    if buf is not None:
        sys.stdout.flush()
        buf.write(data)
        buf.flush()
    else:
        sys.stdout.write(data.decode("utf-8", "replace"))


def note(msg):
    """stderr line that cannot crash on characters the console code page lacks."""
    enc = getattr(sys.stderr, "encoding", None) or "utf-8"
    print(msg.encode(enc, "replace").decode(enc, "replace"), file=sys.stderr)


FIX_RX = re.compile(r"\b(?:fix|fixed|fixes|crash|crashes|broken)\b", re.I)
ADD_RX = re.compile(r"\b(?:add|added|adds|new)\b", re.I)


def _section(text):
    if FIX_RX.search(text):
        return "fixed"
    if ADD_RX.search(text):
        return "added"
    return "changed"


def parse_log(text, current=None):
    """(item lines, version lines) from git log --format=%H%x1f%cI%x1f%s%x1f%b%x1e output.

    A prefixed commit ("0.2.5 beta 3: ...") names its version; an unprefixed one belongs to the version of
    the chronologically next prefixed commit, or to `current` when none follows. `current` defaults to the
    version of the chronologically newest prefixed commit."""
    recs = []
    for rec in text.split("\x1e"):
        rec = rec.strip("\r\n")
        parts = rec.split("\x1f", 3)
        if len(parts) < 3:
            continue
        recs.append((parts[0].strip(), parts[1].strip(), parts[2].strip(), parts[3] if len(parts) > 3 else ""))
    recs.reverse()  # git prints newest first
    recs.sort(key=lambda r: r[1][:19])  # stable: ties keep log order
    if current:
        current = norm_version(current)
    pending, entries = [], []  # entries: (sha, commit datetime, version, rest, subj, body)

    def flush(version):
        for sha, day, rest, subj, body in pending:
            entries.append((sha, day, version, rest, subj, body))
        pending.clear()

    last_prefixed = None
    for sha, when, subj, body in recs:
        if NOISE_RX.match(subj):
            continue
        m = PREFIX_RX.match(subj)
        if m:
            try:
                version = norm_version(m.group(1))
            except ValueError as e:
                note(f"skipped {sha[:12]}: {e}")
                continue
            last_prefixed = version
            pending.append((sha, when, m.group(2).strip(), subj, body))
            flush(version)
        else:
            pending.append((sha, when, subj, subj, body))
    if current is None:
        current = last_prefixed
    if current:
        flush(current)
    else:
        pending.clear()
    items, dates = [], {}
    for sha, day, version, rest, subj, body in entries:
        try:
            msg = clean_text(rest[:300])
        except ValueError as e:
            note(f"skipped {sha[:12]}: {e}")
            continue
        item = {"kind": "item", "source_key": f"git:{sha}", "track": "nw", "version": version,
                "section": _section(rest), "text": msg, "status": "done", "commits": [sha[:12]],
                "dated": day}  # the commit's own ISO datetime
        bugs = list(dict.fromkeys(BUG_RX.findall(subj + "\n" + body)))
        if bugs:
            item["bugs"] = bugs
        items.append(item)
        if day[:10] > dates.get(version, ""):
            dates[version] = day[:10]
    vers = []
    for v in sorted(set(dates) | ({current} if current else set()), key=version_key):
        line = {"kind": "version", "track": "nw", "version": v}
        if v == current:
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
    ap.add_argument("--local", metavar="DB", help="import straight into this clinic database (as nw-sync), no ssh")
    a = ap.parse_args(argv)
    git = subprocess.run(["git", "-C", a.repo, "log", "--format=" + LOG_FORMAT], capture_output=True)
    if git.returncode:
        note(git.stderr.decode("utf-8", "replace"))
        return 1
    cur = None
    vf = Path(a.repo) / "VERSION"
    if vf.is_file():
        try:
            cur = norm_version(vf.read_text(encoding="utf-8").strip())
        except ValueError as e:
            note(f"VERSION ignored: {e}")
    items, vers = parse_log(git.stdout.decode("utf-8", "replace"), cur)
    data = "".join(json.dumps(r, ensure_ascii=False) + "\n" for r in vers + items)
    if a.dry_run:
        out_bytes(data.encode("utf-8"))
        return 0
    if a.local:
        import clinic_changes
        import clinic_store
        res = clinic_changes.Changes(clinic_store.Clinic(a.local)).import_lines(
            data.splitlines(), who="nw-sync")
        out_bytes((f"applied={res['applied']} skipped={len(res['skipped'])} "
                   f"locked={len(res.get('locked', []))}" + chr(10)).encode("utf-8"))
        for sk in res["skipped"]:
            note(f"skipped line {sk['line']}: {sk['why']}")
        return 0
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", a.host, "cl", "import"],
                       input=data.encode("utf-8"), capture_output=True)
    out_bytes(r.stdout)
    if r.returncode:
        note(r.stderr.decode("utf-8", "replace"))
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
