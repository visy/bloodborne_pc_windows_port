#!/usr/bin/env python3
r"""Create the GitHub release and upload the zip (the old scratchpad make_release.py, with checks).

    python tools/release/publish.py --repo OWNER/NAME --tag vX.Y.Z --commit <FULL 40-char sha>
           --title "..." --notes notes.md --zip bloodborne_pc_windows_port_vX.Y.Z.zip [--yes-publish]

Without --yes-publish it is a DRY RUN: every local check runs, the requests are printed, nothing touches
the network or the stored credential. With it, the release is created and the zip uploaded.

REFUSES when:
  * --commit is not a full sha, or differs from the commit stage.py stamped (<zip>.stamp.json from
    make_zip.py: the commit the exe was built from), or a local tag of that name points elsewhere;
  * the tag is not v<stamped version>;
  * the zip's size / sha256 differ from the stamp, or out/bbport.exe inside the zip does not match the
    stamped exe sha256;
  * the notes file is missing or empty;
  * (with --yes-publish) the release or the tag already exists on GitHub.

The token comes from `git credential fill` and is never printed. SSL uses certifi's CA bundle when certifi
is installed (do not run with python -I: it would hide the user site where certifi may live).
"""
import argparse
import json
import ssl
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_common as rc  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
API = "https://api.github.com"


def die(msg):
    raise SystemExit(f"publish: REFUSED: {msg}")


def local_checks(repo, tag, commit, notes, zip_path, git_repo=REPO):
    """Every check that needs no network. Returns (stamp, notes text); dies on the first failure."""
    if not rc.is_full_sha(commit):
        die(f"--commit must be the full 40-character sha, got {commit!r}")
    zip_path = Path(zip_path)
    if not zip_path.is_file():
        die(f"no zip {zip_path}")
    stamp_path = Path(str(zip_path) + ".stamp.json")
    if not stamp_path.is_file():
        die(f"no {stamp_path} (make_zip.py writes it from stage.py's stamp): cannot prove which commit the exe is from")
    stamp = rc.read_stamp(stamp_path)
    if stamp.get("commit") != commit:
        die(f"the exe was built from {stamp.get('commit')}, the release would tag {commit}")
    if tag != "v" + str(stamp.get("version")):
        die(f"tag {tag} does not match the stamped version {stamp.get('version')}")
    if zip_path.stat().st_size != stamp.get("zip_size"):
        die("zip size differs from the stamp (zip changed after make_zip.py)")
    if rc.sha256(zip_path) != stamp.get("zip_sha256"):
        die("zip sha256 differs from the stamp (zip changed after make_zip.py)")
    with zipfile.ZipFile(zip_path) as z:
        try:
            info = z.getinfo("out/bbport.exe")
        except KeyError:
            die("zip has no out/bbport.exe")
        import hashlib
        h = hashlib.sha256()
        with z.open(info) as f:
            for b in iter(lambda: f.read(1 << 20), b""):
                h.update(b)
    if h.hexdigest() != stamp.get("exe_sha256"):
        die("out/bbport.exe in the zip is not the stamped exe")
    try:
        tagged = rc.git(git_repo, "rev-parse", "--verify", "-q", f"refs/tags/{tag}^{{commit}}")
    except RuntimeError:
        tagged = ""
    if tagged and tagged != commit:
        die(f"local tag {tag} points at {tagged}, not {commit}")
    notes_path = Path(notes)
    if not notes_path.is_file() or not notes_path.read_text(encoding="utf-8").strip():
        die(f"notes file missing or empty: {notes}")
    return stamp, notes_path.read_text(encoding="utf-8")


def ssl_context():
    try:
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        return ssl.create_default_context()


def stored_token():
    r = subprocess.run(["git", "credential", "fill"], input="protocol=https\nhost=github.com\n\n",
                       capture_output=True, text=True, check=True)
    token = dict(line.split("=", 1) for line in r.stdout.splitlines() if "=" in line).get("password")
    if not token:
        die("no stored GitHub credential")
    return token


class GitHub:
    """Minimal REST client; tests replace it."""

    def __init__(self, token, ctx=None):
        self.headers = {"Authorization": f"Bearer {token}", "Accept": "application/vnd.github+json",
                        "X-GitHub-Api-Version": "2022-11-28", "User-Agent": "bbport-release"}
        self.ctx = ctx or ssl_context()

    def call(self, url, data=None, method="GET", ctype="application/json", length=None):
        headers = {**self.headers, "Content-Type": ctype}
        if length is not None:
            headers["Content-Length"] = str(length)
        req = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=600, context=self.ctx) as r:
                return r.status, json.loads(r.read() or b"{}")
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")


def release_request(repo, tag, commit, title, body):
    return {"url": f"{API}/repos/{repo}/releases", "method": "POST",
            "json": {"tag_name": tag, "target_commitish": commit, "name": title, "body": body,
                     "draft": False, "prerelease": False}}


def publish(gh, repo, tag, commit, title, body, zip_path):
    st, existing = gh.call(f"{API}/repos/{repo}/releases/tags/{tag}")
    if st == 200:
        die(f"release {tag} already exists: {existing.get('html_url')}")
    st, ref = gh.call(f"{API}/repos/{repo}/git/ref/tags/{tag}")
    if st == 200:
        die(f"tag {tag} already exists on {repo} (at {ref.get('object', {}).get('sha')})")
    req = release_request(repo, tag, commit, title, body)
    st, rel = gh.call(req["url"], json.dumps(req["json"]).encode(), "POST")
    if st != 201:
        raise SystemExit(f"publish: create failed {st}: {rel.get('message')} {rel.get('errors')}")
    print("release:", rel["html_url"])
    a = Path(zip_path)
    upload = rel["upload_url"].split("{")[0] + "?name=" + urllib.parse.quote(a.name)
    with open(a, "rb") as f:
        st, up = gh.call(upload, f, "POST", "application/zip", length=a.stat().st_size)
    print("asset:", st, up.get("browser_download_url"), up.get("size"))
    return 0 if st == 201 else 1


def main(argv=None, github=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", required=True, help="OWNER/NAME on GitHub")
    ap.add_argument("--tag", required=True)
    ap.add_argument("--commit", required=True, help="full sha the tag is created at")
    ap.add_argument("--title", required=True)
    ap.add_argument("--notes", required=True, help="release notes file (Added / Changed / Fixed)")
    ap.add_argument("--zip", required=True)
    ap.add_argument("--yes-publish", action="store_true", help="really create the release and upload")
    ap.add_argument("--git-repo", default=str(REPO), help="local repository for the tag check")
    a = ap.parse_args(argv)
    stamp, body = local_checks(a.repo, a.tag, a.commit, a.notes, a.zip, a.git_repo)
    req = release_request(a.repo, a.tag, a.commit, a.title, body)
    zp = Path(a.zip)
    print(f"checks ok: version {stamp['version']}, exe built from {stamp['commit'][:10]}, zip matches the stamp")
    if not a.yes_publish:
        shown = dict(req["json"], body=f"<{len(body)} chars from {a.notes}>")
        print("DRY RUN (add --yes-publish to send). Would send:")
        print(f"  GET  {API}/repos/{a.repo}/releases/tags/{a.tag}   (must be 404)")
        print(f"  GET  {API}/repos/{a.repo}/git/ref/tags/{a.tag}   (must be 404)")
        print(f"  {req['method']} {req['url']}")
        print("  " + json.dumps(shown, indent=2).replace("\n", "\n  "))
        print(f"  POST <upload_url>?name={urllib.parse.quote(zp.name)}  ({zp.stat().st_size:,} bytes, application/zip)")
        return 0
    gh = github or GitHub(stored_token())
    return publish(gh, a.repo, a.tag, a.commit, a.title, body, a.zip)


if __name__ == "__main__":
    sys.exit(main())
