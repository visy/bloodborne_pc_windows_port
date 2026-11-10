#!/usr/bin/env python3
"""Changelog fragments: changes.d/<slug>.md -> the CHANGES.md section of a release.

  changelog.py check                          every fragment parses (exit 1 with file:line otherwise)
  changelog.py preview                        print the assembled entries (Added / Changed / Fixed)
  changelog.py assemble X.Y.Z --date DATE     merge the fragments into CHANGES.md under `## X.Y.Z (DATE)`
                                              (after the entries already in that section) and delete them

Fragment format: see changes.d/README.md. Run from the repo root or pass --repo.
"""
import argparse
import re
import sys
from pathlib import Path

HEADINGS = ("Added", "Changed", "Fixed")
FRAG_DIR = "changes.d"
README = "README.md"


def fragment_files(repo):
    d = Path(repo) / FRAG_DIR
    if not d.is_dir():
        return []
    return sorted((p for p in d.glob("*.md") if p.name != README), key=lambda p: p.name)


def is_fragment_path(name):
    """True for a repo path `changes.d/<x>.md` that is a fragment (not the README)."""
    p = name.replace("\\", "/")
    return (p.startswith(FRAG_DIR + "/") and p.endswith(".md") and p.count("/") == 1
            and p[len(FRAG_DIR) + 1:] != README)


def parse_sections(lines, strict):
    """(preamble lines, {heading: [entry text, ...]}, errors[(line_no, msg)]) of a block of lines.
    An entry is a `- ` bullet with its continuation lines; blank lines between entries are dropped."""
    errors, pre, secs, cur, entry = [], [], {}, None, None

    def flush():
        nonlocal entry
        if entry is not None and cur is not None:
            while entry and not entry[-1].strip():
                entry.pop()
            secs[cur].append("\n".join(entry))
        entry = None

    for i, line in enumerate(lines, 1):
        if "<<<<<<<" in line or ">>>>>>>" in line or re.match(r"=======\s*$", line):
            errors.append((i, "merge conflict marker"))
        m = re.match(r"###\s+(.*?)\s*$", line)
        if m:
            flush()
            name = m.group(1)
            if name not in HEADINGS:
                errors.append((i, f"heading '### {name}' is not one of {', '.join(HEADINGS)}"))
                cur = None
                continue
            if name in secs:
                errors.append((i, f"heading '### {name}' appears twice"))
            cur = name
            secs.setdefault(cur, [])
        elif re.match(r"-\s+\S", line):
            flush()
            if cur is None:
                errors.append((i, "bullet before any ### heading"))
            entry = [line.rstrip()]
        elif line.strip():
            if entry is not None:
                entry.append(line.rstrip())
            elif cur is None:
                pre.append(line)
                if strict:
                    errors.append((i, "text before the first ### heading"))
            else:
                errors.append((i, "text under a heading that is not part of a '- ' bullet"))
        elif entry is not None:
            entry.append("")
    flush()
    return pre, secs, errors


def check_fragment(path):
    """Sorted list of (line_no, message) for one fragment file."""
    try:
        text = Path(path).read_bytes().decode("utf-8")
    except UnicodeDecodeError as e:
        return [(1, f"not valid UTF-8 ({e.reason})")]
    _, secs, errors = parse_sections(text.splitlines(), True)
    if not any(secs.values()):
        errors.append((1, "no '- ' bullet under Added / Changed / Fixed"))
    else:
        for h, ents in secs.items():
            if not ents:
                errors.append((1, f"heading '### {h}' has no bullet"))
    return sorted(errors)


def check(repo):
    """Prints file:line errors to stderr, returns their count."""
    bad = 0
    for p in fragment_files(repo):
        for ln, msg in check_fragment(p):
            print(f"{p}:{ln}: {msg}", file=sys.stderr)
            bad += 1
    return bad


def load_fragments(repo):
    """{heading: [entries]} of all fragments, files in name order; ValueError if one fails check."""
    if check(repo):
        raise ValueError("a changes.d fragment failed `changelog.py check`")
    out = {h: [] for h in HEADINGS}
    for p in fragment_files(repo):
        _, secs, _ = parse_sections(p.read_text(encoding="utf-8").splitlines(), True)
        for h in HEADINGS:
            out[h].extend(secs.get(h, []))
    return out


def render(secs):
    parts = []
    for h in HEADINGS:
        if secs.get(h):
            parts.append(f"### {h}\n\n" + "\n".join(secs[h]) + "\n")
    return "\n".join(parts)


def assemble_text(changes, ver, date, frags):
    """New CHANGES.md text with the fragments merged into `## ver (...)` (created when absent)."""
    lines = changes.split("\n")
    head = re.compile(r"##\s+" + re.escape(ver) + r"(\s|$)")
    start = next((i for i, l in enumerate(lines) if head.match(l)), None)
    if start is None:
        start = next((i for i, l in enumerate(lines) if l.startswith("## ")), len(lines))
        end, body = start, []
    else:
        end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("## ")), len(lines))
        body = lines[start + 1:end]
    pre, secs, errors = parse_sections(body, False)
    if errors:
        raise ValueError(f"CHANGES.md section {ver}: line {errors[0][0]} of the section: {errors[0][1]}")
    merged = {h: list(secs.get(h, [])) + list(frags.get(h, [])) for h in HEADINGS}
    section = f"## {ver} ({date})\n\n"
    if any(p.strip() for p in pre):
        section += "\n".join(pre).strip() + "\n\n"
    section += render(merged)
    new = lines[:start] + section.rstrip("\n").split("\n") + [""] + lines[end:]
    return "\n".join(new)


def assemble(repo, ver, date):
    """Merges the fragments into CHANGES.md and deletes them; returns how many it consumed."""
    repo = Path(repo)
    if not re.fullmatch(r"\d+(\.\d+){1,3}", ver):
        raise ValueError(f"bad version {ver!r}")
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", date):
        raise ValueError(f"bad date {date!r}, want YYYY-MM-DD")
    frags = load_fragments(repo)
    cp = repo / "CHANGES.md"
    old = cp.read_bytes().decode("utf-8")
    nl = "\r\n" if "\r\n" in old else "\n"
    old_lf = old.replace("\r\n", "\n")
    new = assemble_text(old_lf, ver, date, frags)
    if new != old_lf:
        cp.write_bytes(new.replace("\n", nl).encode("utf-8"))
    files = fragment_files(repo)
    for p in files:
        p.unlink()
    return len(files)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=str(Path(__file__).resolve().parents[2]))
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check")
    sub.add_parser("preview")
    s = sub.add_parser("assemble")
    s.add_argument("version")
    s.add_argument("--date", required=True)
    a = ap.parse_args(argv)
    try:
        if a.cmd == "check":
            bad = check(a.repo)
            print(f"{len(fragment_files(a.repo))} fragment(s), {bad} error(s)")
            return 1 if bad else 0
        if a.cmd == "preview":
            print(render(load_fragments(a.repo)))
            return 0
        n = assemble(a.repo, a.version, a.date)
        print(f"assembled {n} fragment(s) into CHANGES.md ## {a.version} ({a.date}); "
              f"commit CHANGES.md and the deleted changes.d files")
        return 0
    except ValueError as e:
        print(f"changelog: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
