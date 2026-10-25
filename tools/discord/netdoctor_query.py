#!/usr/bin/env python3
"""Look up a netdoctor bot analysis by its reference (the 'ref ND-xxxxxxxx' line under each post).
Run on the bot's server (it reads the bot's memory file and its token).

  python netdoctor_query.py ND-1a2b3c4d                 the stored analysis, its post link and its logs
  python netdoctor_query.py ND-1a2b3c4d --logs DIR      also download the logs into DIR
  python netdoctor_query.py ND-1a2b3c4d --rerun         download into a temp folder and run net_doctor
                                                        again, unredacted (names, IPs) - for the dev
  python netdoctor_query.py --list                      the 20 newest analyses

Environment as the bot (NETDOCTOR_TOKEN, NETDOCTOR_DB); without them it reads
~/.config/netdoctor/env. Standard library only.
"""
import argparse
import json
import os
import sys
import tempfile
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import netdoctor_store as ns  # noqa: E402


def load_env():
    env = Path.home() / ".config" / "netdoctor" / "env"
    if env.is_file():
        for line in env.read_text().splitlines():
            if "=" in line and not line.lstrip().startswith("#"):
                k, v = line.split("=", 1)
                os.environ.setdefault(k.strip(), v.strip())


def api(path, token):
    req = urllib.request.Request("https://discord.com/api/v10" + path,
                                 headers={"Authorization": f"Bot {token}",
                                          "User-Agent": "netdoctor-query (bbport, 1)"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def download(files, dest, token, store=None):
    """Each log: the bot's compressed copy when it has one, else through its Discord message (fresh
    URL), else the stored URL."""
    dest.mkdir(parents=True, exist_ok=True)
    out = []
    for n, f in enumerate(files):
        kept = store.get_logdata(f["sha1"]) if store and f.get("sha1") else None
        if kept:
            d = dest / str(n)
            d.mkdir(exist_ok=True)
            p = d / Path(kept[0] or f.get("filename") or "log.log").name
            p.write_bytes(kept[1])
            out.append(p)
            continue
        url, name = f.get("url"), f.get("filename")
        if f.get("message") and f.get("channel"):
            try:
                msg = api(f"/channels/{f['channel']}/messages/{f['message']}", token)
                att = msg["attachments"][f.get("index", 0)]
                url, name = att["url"], att["filename"]
            except Exception as e:
                print(f"  message {f['message']}: {e}", file=sys.stderr)
        if not url:
            continue
        d = dest / str(n)
        d.mkdir(exist_ok=True)
        p = d / Path(name or "log.log").name
        req = urllib.request.Request(url, headers={"User-Agent": "netdoctor-query (bbport, 1)"})
        with urllib.request.urlopen(req, timeout=60) as r, open(p, "wb") as w:
            w.write(r.read())
        out.append(p)
    return out


def main(argv=None):
    load_env()
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("ref", nargs="?")
    ap.add_argument("--logs", help="download the logs into this folder")
    ap.add_argument("--rerun", action="store_true", help="run net_doctor again on the logs, unredacted")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args(argv)
    db = Path(os.environ.get("NETDOCTOR_DB", Path.home() / ".config" / "netdoctor" / "netdoctor.db"))
    store = ns.Store(db)
    if a.list or not a.ref:
        for ref, ch, created, head in store.recent_analyses():
            import time
            print(f"{ref}  {time.strftime('%Y-%m-%d %H:%M', time.localtime(created))}  channel {ch}  "
                  f"{head.splitlines()[0] if head else ''}")
        return 0
    ref = a.ref if a.ref.upper().startswith("ND-") else "ND-" + a.ref
    ref = "ND-" + ref[3:].lower()
    rec = store.get_analysis(ref)
    if not rec:
        print(f"no analysis {ref} (kept 30 days)", file=sys.stderr)
        return 1
    link = f"https://discord.com/channels/{rec['guild_id'] or '@me'}/{rec['channel_id']}/{rec['bot_message_id']}" \
        if rec["bot_message_id"] else "(reply without a stored post id)"
    print(f"{ref}: {len(rec['files'])} log(s), post {link}")
    for f in rec["files"]:
        stored = "stored" if f.get("sha1") and store.get_logdata(f["sha1"]) else "on Discord only"
        print(f"  log: {f.get('filename') or '?'}  message {f.get('message')} #{f.get('index')}  ({stored})")
    print("\n" + rec["text"])
    token = os.environ.get("NETDOCTOR_TOKEN")
    if (a.logs or a.rerun) and not token and not all(f.get("sha1") for f in rec["files"]):
        print("NETDOCTOR_TOKEN missing", file=sys.stderr)
        return 1
    if a.logs:
        got = download(rec["files"], Path(a.logs), token, store)
        print(f"\ndownloaded {len(got)} log(s) to {a.logs}")
    if a.rerun:
        sys.path.insert(0, str(HERE))
        import netdoctor_bot as bot
        with tempfile.TemporaryDirectory(prefix="netdoctor_") as tmp:
            paths = download(rec["files"], Path(tmp), token, store)
            print("\n=== rerun (unredacted) ===\n" + bot.diagnose(paths, redact=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
