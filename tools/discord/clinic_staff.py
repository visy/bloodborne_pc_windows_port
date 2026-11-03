"""Staff window into Iosefka's clinic: the /clinic slash commands for moderators and developers.

Everything here is read-only except the optional `note` command (own role setting, off by default).
The logic (access, queries, formatting, file selection) is plain Python over the Clinic store, tested
without Discord; `register()` binds it to discord.py app_commands (imported lazily).

Privacy: replies are ephemeral in the guild (and a DM is private anyway); logs and message text are
passed through mask_ips before they leave; every use (who, which command, which bug id, which filter
NAMES, never the contents) lands in the nw_audit table (`iosefka_cli.py --as visy staff-audit`).

Environment (all optional):
  NETDOCTOR_STAFF_ROLES        role ids or names allowed to read (default: the roles named Mod and Dev,
                               exact name, case-insensitive). Setting it replaces the default.
  NETDOCTOR_STAFF_USERS        user ids allowed whatever their roles
  NETDOCTOR_STAFF_WRITE_ROLES  role ids/names that may also use /clinic note (default: none = no write)
  NETDOCTOR_STAFF_WRITE_USERS  user ids that may use /clinic note
  NETDOCTOR_STAFF_HIDE_PERM    a discord permission name (e.g. manage_messages): the commands are then hidden
                               from members without it (Server Settings > Integrations must allow the staff
                               roles by hand). Default unset: commands visible, the role check refuses.
  NETDOCTOR_STAFF_MAX_MB       largest attachment sent (default 8; also capped by the server's limit)
"""
import io
import json
import re
import sys
import time
import zipfile
import zlib
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from clinic_logic import mask_ips  # noqa: E402

DEFAULT_ROLES = ("mod", "dev")
PAGE_CHARS = 1800
MAX_FILES_PER_MESSAGE = 10
MAX_MESSAGES = 4          # attachment messages per command
MAX_FILES = 60            # files looked at per command
MEMBER_TTL = 120.0        # seconds a DM user's role lookup is remembered
SEND_KINDS = {"logs": ("log",), "images": ("image",), "stills": ("still",), "videos": ("video",),
              "all": ("log", "image", "still", "video")}
STATUSES = ("new", "open", "analysing", "waiting-logs", "fixed", "closed", "duplicate")
BUG_RX = re.compile(r"(?i)\bNW-([0-9a-f]{7})\b")
CL_RX = re.compile(r"(?i)\bCL-([0-9a-f]{6})\b")


# ---- config and access -------------------------------------------------------------------------

def _split(s):
    return [x for x in re.split(r"[,\s]+", s or "") if x]


def _ids(s):
    return {int(x) for x in _split(s) if x.isdigit()}


class Config:
    def __init__(self, roles=None, users=(), write_roles=(), write_users=(), guild_id=None, hide_perm="",
                 max_mb=8.0):
        self.roles = list(roles) if roles else list(DEFAULT_ROLES)
        self.users, self.write_users = set(users), set(write_users)
        self.write_roles = list(write_roles)
        self.guild_id, self.hide_perm, self.max_mb = guild_id, hide_perm, max_mb

    @classmethod
    def from_env(cls, env):
        return cls(roles=_split(env.get("NETDOCTOR_STAFF_ROLES")), users=_ids(env.get("NETDOCTOR_STAFF_USERS")),
                   write_roles=_split(env.get("NETDOCTOR_STAFF_WRITE_ROLES")),
                   write_users=_ids(env.get("NETDOCTOR_STAFF_WRITE_USERS")),
                   guild_id=int(env["NETDOCTOR_GUILD"]) if (env.get("NETDOCTOR_GUILD") or "").isdigit() else None,
                   hide_perm=(env.get("NETDOCTOR_STAFF_HIDE_PERM") or "").strip(),
                   max_mb=float(env.get("NETDOCTOR_STAFF_MAX_MB") or 8))

    @property
    def writes_enabled(self):
        return bool(self.write_roles or self.write_users)

    @staticmethod
    def _has(specs, member):
        for r in getattr(member, "roles", None) or []:
            name = (getattr(r, "name", "") or "").strip().lower()
            for s in specs:
                if (s.isdigit() and int(s) == getattr(r, "id", None)) or (not s.isdigit() and s.lower() == name):
                    return True
        return False

    def can_read(self, member):
        return getattr(member, "id", None) in self.users or self._has(self.roles, member) \
            or self.can_write(member)

    def can_write(self, member):
        return getattr(member, "id", None) in self.write_users or self._has(self.write_roles, member)


class Access:
    def __init__(self, read=False, write=False):
        self.read, self.write = read, write


class Gate:
    """Who may use the commands, in the guild and in DMs (a DM user is looked up as a member of the guild,
    remembered for MEMBER_TTL seconds). Rate limit per user."""

    def __init__(self, cfg, clock=time.time, per_minute=12, files_per_minute=4):
        self.cfg, self.clock = cfg, clock
        self.cache = {}
        self.hits, self.file_hits = {}, {}
        self.per_minute, self.files_per_minute = per_minute, files_per_minute

    async def access(self, interaction, client):
        user = interaction.user
        uid = getattr(user, "id", None)
        if uid is None:
            return Access()
        guild = getattr(interaction, "guild", None)
        if guild is not None:                                 # in a server: the member object has the roles
            if self.cfg.guild_id and getattr(guild, "id", None) != self.cfg.guild_id:
                return Access()
            return Access(self.cfg.can_read(user), self.cfg.can_write(user))
        if uid in self.cfg.users:
            return Access(True, uid in self.cfg.write_users or self.cfg.can_write(user))
        got = self.cache.get(uid)
        now = self.clock()
        if got and now - got[0] < MEMBER_TTL:
            return got[1]
        acc = Access()
        guilds = [client.get_guild(self.cfg.guild_id)] if self.cfg.guild_id else list(client.guilds)
        for g in guilds:
            if g is None:
                continue
            member = g.get_member(uid) if hasattr(g, "get_member") else None
            if member is None:
                try:
                    member = await g.fetch_member(uid)
                except Exception:
                    member = None
            if member is not None and self.cfg.can_read(member):
                acc = Access(True, self.cfg.can_write(member))
                break
        self.cache[uid] = (now, acc)
        return acc

    def allow_rate(self, uid, heavy=False):
        """False when this user is over the per-minute budget (files are costlier)."""
        now = self.clock()
        for table, limit in ((self.hits, self.per_minute), (self.file_hits, self.files_per_minute)):
            if table is self.file_hits and not heavy:
                continue
            q = [t for t in table.get(uid, []) if now - t < 60]
            if len(q) >= limit:
                table[uid] = q
                return False
            q.append(now)
            table[uid] = q
        return True


# ---- audit ---------------------------------------------------------------------------------------

AUDIT_SCHEMA = ("CREATE TABLE IF NOT EXISTS nw_audit (at REAL, user_id INTEGER, user_name TEXT, command TEXT, "
                "target TEXT, filters TEXT, allowed INTEGER, where_ TEXT)")


def audit(clinic, user, command, *, target="", filters=(), allowed=True, where="guild", now=None):
    """Who ran what. Never the contents: target is a bug/CL id, filters only the NAMES of the options used."""
    clinic.db.execute(AUDIT_SCHEMA)
    clinic.db.execute("INSERT INTO nw_audit VALUES (?,?,?,?,?,?,?,?)",
                      (now or time.time(), getattr(user, "id", 0), str(getattr(user, "name", ""))[:64], command,
                       str(target)[:40], ",".join(sorted(filters)), 1 if allowed else 0, where))
    clinic.db.commit()


def audit_rows(clinic, since=0.0, limit=100):
    clinic.db.execute(AUDIT_SCHEMA)
    rows = clinic.db.execute("SELECT at, user_id, user_name, command, target, filters, allowed, where_ FROM "
                             "nw_audit WHERE at>? ORDER BY at DESC LIMIT ?", (since, limit)).fetchall()
    return [dict(zip(("at", "user_id", "user", "command", "target", "filters", "allowed", "where"), r))
            for r in rows]


# ---- text helpers --------------------------------------------------------------------------------

def iso(t):
    return datetime.fromtimestamp(t, timezone.utc).strftime("%Y-%m-%d %H:%M") if t else "-"


def safe(text):
    """Text that leaves the bot: IPs masked."""
    return mask_ips(text or "")


def excerpt(text, n=300):
    text = " ".join((text or "").split())
    return text if len(text) <= n else text[:n - 1] + "..."


def paginate(lines, limit=PAGE_CHARS):
    """Lines grouped into pages of at most `limit` characters (a longer single line is cut)."""
    pages, cur, size = [], [], 0
    for line in lines:
        while len(line) > limit:
            if cur:
                pages.append("\n".join(cur))
                cur, size = [], 0
            pages.append(line[:limit])
            line = line[limit:]
        if cur and size + len(line) + 1 > limit:
            pages.append("\n".join(cur))
            cur, size = [], 0
        cur.append(line)
        size += len(line) + 1
    if cur:
        pages.append("\n".join(cur))
    return pages or ["(nothing)"]


def norm_bug(s):
    m = BUG_RX.search(s or "")
    return f"NW-{m.group(1).lower()}" if m else None


def norm_cl(s):
    m = CL_RX.search(s or "")
    return f"CL-{m.group(1).lower()}" if m else None


def parse_since(s, now=None):
    """YYYY-MM-DD, or 7d / 12h / 2w ago. None when empty; ValueError otherwise."""
    s = (s or "").strip().lower()
    if not s:
        return None
    m = re.fullmatch(r"(\d+)\s*([hdw])", s)
    if m:
        return (now or time.time()) - int(m.group(1)) * {"h": 3600, "d": 86400, "w": 604800}[m.group(2)]
    try:
        return datetime.strptime(s, "%Y-%m-%d").replace(tzinfo=timezone.utc).timestamp()
    except ValueError:
        raise ValueError("since wants YYYY-MM-DD or a span such as 7d, 12h, 2w")


def message_url(guild_id, m):
    """Link to a stored message (thread when it was in one); the original may be deleted once Iosefka
    replaced it, `replacement_id` then points at her note."""
    gid = guild_id or "@me"
    where = m.get("thread_id") or m.get("channel_id")
    if not where:
        return ""
    return f"https://discord.com/channels/{gid}/{where}/{m['message_id']}"


# ---- queries ---------------------------------------------------------------------------------------

def reporter_matches(clinic, bug, who):
    """`who` is a user id / mention (reporter_id) or a name part (author of the bug's messages)."""
    ids = re.findall(r"\d{15,21}", who or "")
    if ids:
        return bug["reporter_id"] == int(ids[0])
    w = (who or "").strip().lower().lstrip("@")
    return bool(w) and any(w in (m["author_name"] or "").lower() for m in clinic.messages_for_bug(bug["id"])
                           if m["author_id"] == bug["reporter_id"] or not bug["reporter_id"])


def version_bug_ids(changes, version):
    out = set()
    for it in changes.items(version=version):
        out.update(it["bugs"])
    return out


def find_bugs(clinic, changes, *, status=None, triage=None, area=None, version=None, reporter=None,
              since=None, query=None, now=None):
    """Bugs newest first; every filter given must match."""
    tags = []
    if triage:
        tags.append(f"triage={triage}")
    bugs = clinic.bugs(status=status or None, tag=tags or None, q=query or None,
                       since=parse_since(since, now) if since else None)
    if area:
        a = area.lower()
        bugs = [b for b in bugs if any(a in str(x).lower() for k in ("component", "area")
                                       for x in (lambda v: v if isinstance(v, list) else [v])(b["tags"].get(k, "")))]
    if version:
        ids = version_bug_ids(changes, version)
        bugs = [b for b in bugs if b["id"] in ids]
    if reporter:
        bugs = [b for b in bugs if reporter_matches(clinic, b, reporter)]
    return bugs


def bug_line(clinic, b):
    k = clinic.counts(b["id"])
    tri = b["tags"].get("triage")
    return (f"`{b['id']}` {b['status']} {iso(b['reported'])[:10]}{' [' + str(tri) + ']' if tri else ''} "
            f"{excerpt(safe(b['title']), 70)} ({k['messages']}m {k['logs']}l {k['images'] + k['stills']}i)")


def list_pages(clinic, bugs, header="Bugs"):
    lines = [f"**{header}**: {len(bugs)}"] + [bug_line(clinic, b) for b in bugs]
    return paginate(lines)


def find_any(clinic, term):
    """Search by a bug id, a Discord user id, or a name/text: bugs whose reporter, message author, title,
    or the player names in their logs match."""
    bid = norm_bug(term)
    if bid:
        b = clinic.bug(bid)
        return [b] if b else []
    ids = re.findall(r"\d{15,21}", term or "")
    t = (term or "").strip().lower().lstrip("@")
    if not (ids or t):
        return []
    infos = clinic.all_loginfos()
    out = []
    for b in clinic.bugs():
        msgs = clinic.messages_for_bug(b["id"])
        hit = False
        if ids and (b["reporter_id"] == int(ids[0]) or any(m["author_id"] == int(ids[0]) for m in msgs)):
            hit = True
        elif t and not ids:
            hit = t in (b["title"] or "").lower() or any(t in (m["author_name"] or "").lower() for m in msgs)
            if not hit:
                for f in clinic.files_for_bug(b["id"]):
                    i = infos.get(f["sha256"])
                    if i and t in " ".join([str(i.get("name") or ""), str(i.get("host") or ""),
                                            *map(str, i.get("peers") or ())]).lower():
                        hit = True
                        break
        if hit:
            out.append(b)
    return out


def needs_logs(clinic):
    """Open bugs with no log: status waiting-logs, or open with zero logs."""
    out = []
    for b in clinic.bugs(exclude_status=("fixed", "closed", "duplicate")):
        if b["status"] == "waiting-logs" or (b["status"] in ("new", "open", "analysing")
                                             and clinic.counts(b["id"])["logs"] == 0):
            out.append(b)
    return out


def linked_items(changes, bug_id):
    return changes.items(bug=bug_id)


def cl_line(it):
    return (f"`{it['id']}` {it['track']} {it['version']} {it['status']} {it['section']} "
            f"{iso(it.get('dated'))[:10] if it.get('dated') else ''} {excerpt(safe(it['text']), 160)}").replace("  ", " ")


def bug_card(clinic, changes, b, guild_id=None):
    k = clinic.counts(b["id"])
    tags = ", ".join(f"{a}={','.join(v) if isinstance(v, list) else v}" for a, v in sorted(b["tags"].items())) or "-"
    lines = [f"**{b['id']}** {safe(b['title'])}",
             f"status {b['status']}; assignee {b['assignee'] or '-'}; source {b['source']}; tags {tags}",
             f"reported {iso(b['reported'])}; filed {iso(b['created'])}; updated {iso(b['updated'])}; "
             f"reporter id {b['reporter_id'] or '-'}",
             f"{k['messages']} messages, {k['logs']} logs, {k['images']} images, {k['stills']} stills"]
    if b["dup_of"]:
        lines.append(f"duplicate of {b['dup_of']}")
    dups = [x["id"] for x in clinic.bugs(status="duplicate") if x["dup_of"] == b["id"]]
    if dups:
        lines.append("merged duplicates: " + ", ".join(dups))
    items = linked_items(changes, b["id"])
    if items:
        lines.append("**Changelist**")
        lines += [cl_line(i) for i in items]
    notes = clinic.notes(b["id"])
    if notes:
        lines.append("**Notes**")
        for n in notes[-25:]:
            lines.append(f"{iso(n['at'])} {n['who']} [{n['kind']}] {excerpt(safe(n['text']), 220)}")
        if len(notes) > 25:
            lines.append(f"(+{len(notes) - 25} earlier notes)")
    return paginate(lines)


def messages_pages(clinic, b, guild_id=None):
    lines = [f"**{b['id']} messages**"]
    for m in clinic.messages_for_bug(b["id"]):
        att = ", ".join(a.get("filename") or "file" for a in m["attachments"])
        link = message_url(guild_id, m)
        rep = f" (replaced by {message_url(guild_id, {'message_id': m['replacement_id'], 'thread_id': m['thread_id'], 'channel_id': m['channel_id']})})" \
            if m.get("replacement_id") else ""
        lines.append(f"{iso(m['created'])} {m['author_name'] or '?'} [{m['state']}] <{link}>{rep}")
        if m["content"]:
            lines.append("> " + excerpt(safe(m["content"]), 500))
        if att:
            lines.append("files: " + safe(att))
    return paginate(lines)


def result_pages(clinic, b, nd_get=None):
    """Triage notes and net_doctor results (the stored ND-xxxx analysis text, IPs masked)."""
    lines = [f"**{b['id']} triage / net_doctor**", f"triage tag: {b['tags'].get('triage', '-')}"]
    for n in clinic.notes(b["id"]):
        if n["kind"] in ("triage", "tag"):
            lines.append(f"{iso(n['at'])} {n['who']} [{n['kind']}] {excerpt(safe(n['text']), 300)}")
    refs = b["nd_refs"]
    refs = json.loads(refs or "[]") if isinstance(refs, str) else (refs or [])
    for ref in refs:
        a = nd_get(ref) if nd_get else None
        lines.append(f"**{ref}**" + ("" if a else " (analysis not stored)"))
        if a:
            lines += safe(a["text"]).splitlines()
    return paginate(lines)


def health_text(clinic):
    import clinic_health
    snap = clinic_health.Health(clinic).snapshot(with_versions=False)
    q = snap["queues"]
    lines = [f"last start {iso(snap['last_start'])}, {snap['restarts_24h']} start(s) in 24 h, last tick {iso(snap['last_tick'])}"]
    for j, st in snap["jobs"].items():
        lines.append(f"job {j}: {iso(st['at'])}{' OVERDUE' if st['overdue'] else ''}")
    lines.append(f"errors 24 h: {snap['errors_24h']['count']}")
    lines.append(f"queues: triage {q['triage']}, unsure {q['unsure']}, uploads {q['uploads']}; 24 h: "
                 f"{snap['new_bugs_24h']} new bugs, {snap['logs_24h']} logs")
    lines.append("open alerts: " + (", ".join(snap["alerts_active"]) or "none"))
    return paginate(lines)


# ---- files -----------------------------------------------------------------------------------------

def _safe_name(name):
    name = re.sub(r"[^A-Za-z0-9._() -]", "_", (name or "file").replace("\\", "/").split("/")[-1])
    return name[:80] or "file"


def gather_files(clinic, bug_id, what="logs"):
    """[{name, data, kind, sha}] of a bug's stored files of the wanted kinds. Logs are masked
    (mask_ips) every time: the stored copy may be raw."""
    kinds = SEND_KINDS[what]
    out = []
    for f in clinic.files_for_bug(bug_id)[:MAX_FILES * 3]:
        if f["kind"] not in kinds or len(out) >= MAX_FILES:
            continue
        got = clinic.get_file(f["sha256"])
        if not got or got[1] is None:
            continue
        name, data, _ = got
        if f["kind"] == "log":
            data = safe(data.decode("utf-8", "replace")).encode("utf-8")
        out.append({"name": f"{f['sha256'][:8]}_{_safe_name(f.get('path') or name)}", "data": data,
                    "kind": f["kind"], "sha": f["sha256"]})
    return out


def _zip(items):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for it in items:
            z.writestr(it["name"], it["data"])
    return buf.getvalue()


def plan_attachments(files, limit):
    """Messages worth of attachments: ([[(name, bytes), ...], ...], skipped_names). One message's files
    fit in `limit` bytes and MAX_FILES_PER_MESSAGE; when there are too many or too big, they are zipped
    (a file that does not fit even compressed is skipped and named)."""
    if not files:
        return [], []
    if len(files) <= MAX_FILES_PER_MESSAGE and sum(len(f["data"]) for f in files) <= limit:
        return [[(f["name"], f["data"]) for f in files]], []
    skipped, sized = [], []
    for f in files:
        c = len(zlib.compress(f["data"], 6)) if f["kind"] in ("log", "text") else len(f["data"])
        if c + 512 > limit:
            skipped.append(f["name"])
        else:
            sized.append((c + 512, f))
    bins = []  # first fit, biggest first
    for c, f in sorted(sized, key=lambda x: -x[0]):
        for b in bins:
            if b[0] + c <= limit:
                b[0] += c
                b[1].append(f)
                break
        else:
            bins.append([c, [f]])
    out, n = [], 0
    for _, items in bins:
        stack = [items]
        while stack:
            part = stack.pop(0)
            z = _zip(part)
            if len(z) <= limit:
                n += 1
                out.append([(f"clinic_files_{n}.zip", z)])
            elif len(part) > 1:
                stack += [part[:len(part) // 2], part[len(part) // 2:]]
            else:
                skipped.append(part[0]["name"])
    return out, skipped


def file_limit(cfg, guild=None):
    lim = int(cfg.max_mb * 1024 * 1024)
    gl = getattr(guild, "filesize_limit", None)
    return min(lim, gl) if gl else lim


def file_summary(clinic, bug_id):
    """What the archive holds for a bug, by kind (so the staff know what to ask for)."""
    kinds = {}
    for f in clinic.files_for_bug(bug_id):
        kinds[f["kind"]] = kinds.get(f["kind"], 0) + 1
    return ", ".join(f"{n} {k}" for k, n in sorted(kinds.items())) or "no stored files"


# ---- changelist views --------------------------------------------------------------------------------

RANK_KEYS = ("rank", "priority", "prio")
AREA_KEYS = ("component", "area")


def _tagvals(tags, keys=None):
    out = []
    for k, v in (tags or {}).items():
        if keys is None or k in keys:
            out += [str(x) for x in (v if isinstance(v, list) else [v])]
    return out


def tags_text(tags):
    return ", ".join(f"{k}={','.join(map(str, v)) if isinstance(v, list) else v}"
                     for k, v in sorted(tags.items())) or "-"


def cl_item_pages(changes, cid, clinic=None):
    """Everything on one item: text, dev notes, status, tags, rank, linked bugs, commits, history."""
    it = changes.get(cid)
    if not it:
        return None
    lines = [f"**{it['id']}** {it['track']} {it['version']} {it['section']} **{it['status']}**",
             safe(it["text"]),
             f"dev: {safe(it['dev']) or '-'}",
             f"tags: {tags_text(it['tags'])}",
             f"rank: {', '.join(_tagvals(it['tags'], RANK_KEYS)) or '-'}; author {it['author'] or '-'}; "
             f"added {iso(it['created'])}; updated {iso(it['updated'])}; dated {iso(it.get('dated'))}",
             f"cases: {', '.join(map(str, it['cases'])) or '-'}",
             f"commits: {', '.join(map(str, it['commits'])) or '-'}"]
    for bid in it["bugs"]:
        b = clinic.bug(bid) if clinic else None
        lines.append(f"bug `{bid}`" + (f" {b['status']} {excerpt(safe(b['title']), 90)}" if b else ""))
    hist = changes.notes(cid)
    if hist:
        lines.append("**History**")
        lines += [f"{iso(n['at'])} {n['who']} [{n['kind']}] {excerpt(safe(n['text']), 300)}" for n in hist[-30:]]
    return paginate(lines)


def cl_bug_pages(changes, bug_id):
    items = linked_items(changes, bug_id)
    return paginate([f"**Changelist items for {bug_id}**: {len(items)}"] + [cl_line(i) for i in items])


def cl_list_pages(changes, *, track=None, version=None, status=None, priority=None, area=None, text=None):
    """Items of any version (released, current, planned, future); every filter given must match.
    `priority` matches a rank / priority tag value, `area` a component / area tag value (else any tag value)."""
    import clinic_changes
    items = changes.items(track=track or None, version=version or None, status=status or None, q=text or None)
    if priority:
        p = priority.lower()
        items = [i for i in items if any(p in v.lower() for v in _tagvals(i["tags"], RANK_KEYS))]
    if area:
        a = area.lower()
        items = [i for i in items if any(a in v.lower() for v in (_tagvals(i["tags"], AREA_KEYS)
                                                                  or _tagvals(i["tags"])))]
    items.sort(key=lambda i: (i["track"], clinic_changes.version_key(i["version"]), i["section"], i["id"]))
    return paginate([f"**Changelist**: {len(items)} items"] + [cl_line(i) for i in items])


def cl_versions_pages(changes, track=None):
    lines = ["**Versions**"]
    for v in changes.versions(track or None):
        c = v["counts"]
        lines.append(f"{v['track']} {v['version']} {v['status']}"
                     + (f" released {iso(v['released'])[:10]}" if v["released"] else "")
                     + f" - planned {c['planned']}, in-progress {c['in-progress']}, done {c['done']}, "
                     f"dropped {c['dropped']}" + (f" - {excerpt(safe(v['notes']), 100)}" if v["notes"] else ""))
    return paginate(lines)


def cl_notes_markdown(changes, track, version):
    import clinic_changes
    if track not in clinic_changes.TRACKS:
        raise ValueError("track is coop or nw")
    return changes.render_notes(track, clinic_changes.norm_version(version))


def cl_related_pages(changes, ref):
    rel = changes.related(ref)
    lines = [f"**Related to {ref}**: {len(rel)}"]
    for r in rel:
        if r["kind"] == "item":
            lines.append(f"item `{r['id']}` {r['track']} {r['version']} {r['status']} "
                         f"{excerpt(safe(r['text']), 100)} ({'; '.join(r['why'])})")
        else:
            lines.append(f"bug `{r['id']}` {r['track']} {r['status']} {excerpt(safe(r['title']), 100)} "
                         f"({'; '.join(r['why'])})")
    return paginate(lines)


def cl_for_ref_pages(changes, clinic, ref):
    """bug -> its items; CL -> its bugs; both plus the cross-track related lookup. None when unknown."""
    cid = norm_cl(ref)
    if cid:
        it = changes.get(cid)
        if not it:
            return None
        lines = [f"**Bugs of {cid}**: {len(it['bugs'])}"]
        for bid in it["bugs"]:
            b = clinic.bug(bid)
            lines.append(bug_line(clinic, b) if b else f"`{bid}` (not in the archive)")
        return paginate(lines + cl_related_pages(changes, cid))
    bid = norm_bug(ref)
    if not bid or not clinic.bug(bid):
        return None
    return paginate(cl_bug_pages(changes, bid) + cl_related_pages(changes, bid))


def add_staff_note(clinic, bug_id, who, text):
    text = excerpt(safe(text), 500)
    clinic.add_note(bug_id, f"staff:{who}", "note", text)
    return text


# ---- discord binding ---------------------------------------------------------------------------------

REFUSAL = "Sorry, /clinic is for moderators and developers only."


def register(tree, client, ctx, gate, env_fn=None):
    """Add the /clinic group to `tree`. ctx.clinic is the store; ctx.guild_id the server (links)."""
    import discord
    from discord import app_commands
    import clinic_changes
    cfg = gate.cfg
    perms = None
    if cfg.hide_perm:
        perms = discord.Permissions(**{cfg.hide_perm: True})
    group = app_commands.Group(name="clinic", description="Iosefka's clinic bug archive (moderators and developers)",
                               default_permissions=perms, allowed_contexts=app_commands.AppCommandContext(
                                   guild=True, dm_channel=True, private_channel=True))
    none = discord.AllowedMentions.none()

    def changes():
        return clinic_changes.Changes(ctx.clinic)

    def gid(interaction):
        return getattr(interaction, "guild_id", None) or cfg.guild_id or ctx.guild_id

    class Pager(discord.ui.View):
        def __init__(self, pages, user_id, ephemeral):
            super().__init__(timeout=300)
            self.pages, self.i, self.user_id, self.eph = pages, 0, user_id, ephemeral
            self._sync()

        def _sync(self):
            self.prev.disabled = self.i <= 0
            self.next.disabled = self.i >= len(self.pages) - 1

        def text(self):
            return f"{self.pages[self.i]}\n-# page {self.i + 1}/{len(self.pages)}"

        async def interaction_check(self, interaction):
            return interaction.user.id == self.user_id

        @discord.ui.button(label="Prev", style=discord.ButtonStyle.secondary)
        async def prev(self, interaction, button):
            self.i -= 1
            self._sync()
            await interaction.response.edit_message(content=self.text(), view=self)

        @discord.ui.button(label="Next", style=discord.ButtonStyle.secondary)
        async def next(self, interaction, button):
            self.i += 1
            self._sync()
            await interaction.response.edit_message(content=self.text(), view=self)

    async def guard(interaction, command, *, target="", filters=(), heavy=False, write=False):
        """True when allowed (and audited); else the refusal was sent."""
        where = "guild" if interaction.guild is not None else "dm"
        acc = await gate.access(interaction, client)
        ok = acc.write if write else acc.read
        audit(ctx.clinic, interaction.user, command, target=target, filters=filters, allowed=ok, where=where)
        if not ok:
            await interaction.response.send_message(REFUSAL, ephemeral=True)
            return False
        if not gate.allow_rate(interaction.user.id, heavy):
            await interaction.response.send_message("Slow down a little: try again in a minute.", ephemeral=True)
            return False
        return True

    async def reply_pages(interaction, pages):
        if len(pages) == 1:
            await interaction.response.send_message(pages[0], ephemeral=True, allowed_mentions=none)
            return
        view = Pager(pages, interaction.user.id, True)
        await interaction.response.send_message(view.text(), view=view, ephemeral=True, allowed_mentions=none)

    def need_bug(s):
        bid = norm_bug(s)
        return bid, (ctx.clinic.bug(bid) if bid else None)

    async def bug_cmd(interaction, name, bug, build):
        bid, b = need_bug(bug)
        if not await guard(interaction, name, target=bid or "?"):
            return
        if not b:
            await interaction.response.send_message("No such bug (use NW-xxxxxxx).", ephemeral=True)
            return
        await reply_pages(interaction, build(b))

    @group.command(name="list", description="List bugs; every filter given must match")
    @app_commands.describe(status="bug status", triage="triage tag value (pending, done, network...)",
                           area="component / area tag", version="changelist version that links the bug",
                           reporter="Discord id, mention or name part", since="YYYY-MM-DD or 7d",
                           query="text in title, notes or messages")
    @app_commands.choices(status=[app_commands.Choice(name=s, value=s) for s in STATUSES])
    async def list_(interaction: discord.Interaction, status: str = None, triage: str = None, area: str = None,
                    version: str = None, reporter: str = None, since: str = None, query: str = None):
        used = [k for k, v in dict(status=status, triage=triage, area=area, version=version, reporter=reporter,
                                   since=since, query=query).items() if v]
        if not await guard(interaction, "list", filters=used):
            return
        try:
            bugs = find_bugs(ctx.clinic, changes(), status=status, triage=triage, area=area, version=version,
                             reporter=reporter, since=since, query=query)
        except ValueError as e:
            await interaction.response.send_message(str(e), ephemeral=True)
            return
        await reply_pages(interaction, list_pages(ctx.clinic, bugs))

    @group.command(name="show", description="One bug: status, tags, reporter, dates, notes, changelist links")
    async def show(interaction: discord.Interaction, bug: str):
        await bug_cmd(interaction, "show", bug, lambda b: bug_card(ctx.clinic, changes(), b, gid(interaction)))

    @group.command(name="messages", description="A bug's messages with links to the Discord messages")
    async def messages(interaction: discord.Interaction, bug: str):
        await bug_cmd(interaction, "messages", bug, lambda b: messages_pages(ctx.clinic, b, gid(interaction)))

    @group.command(name="result", description="Triage notes and the stored net_doctor result of a bug")
    async def result(interaction: discord.Interaction, bug: str):
        await bug_cmd(interaction, "result", bug,
                      lambda b: result_pages(ctx.clinic, b, lambda ref: _analysis(ctx.clinic, ref)))

    @group.command(name="files", description="Send a bug's stored files (logs IP-masked); zipped when many")
    @app_commands.describe(what="logs, images, stills, videos or all")
    @app_commands.choices(what=[app_commands.Choice(name=k, value=k) for k in SEND_KINDS])
    async def files(interaction: discord.Interaction, bug: str, what: str = "logs"):
        bid, b = need_bug(bug)
        if not await guard(interaction, "files", target=bid or "?", filters=(what,), heavy=True):
            return
        if not b:
            await interaction.response.send_message("No such bug (use NW-xxxxxxx).", ephemeral=True)
            return
        await interaction.response.defer(ephemeral=True, thinking=True)
        found = gather_files(ctx.clinic, bid, what)
        limit = file_limit(cfg, interaction.guild)
        groups, skipped = plan_attachments(found, limit)
        head = f"{bid}: {file_summary(ctx.clinic, bid)}; sending {len(found)} {what}"
        if not groups:
            await interaction.followup.send(head + " - nothing to send.", ephemeral=True)
            return
        for n, g in enumerate(groups[:MAX_MESSAGES]):
            await interaction.followup.send(head if n == 0 else f"{bid} (part {n + 1}/{len(groups)})",
                                            files=[discord.File(io.BytesIO(d), filename=nm) for nm, d in g],
                                            ephemeral=True, allowed_mentions=none)
        rest = []
        if len(groups) > MAX_MESSAGES:
            rest.append(f"{len(groups) - MAX_MESSAGES} more part(s) not sent (message cap); ask visy for `files {bid}`.")
        if skipped:
            rest.append("Too big to send (limit " + f"{limit // 1048576} MB): " + ", ".join(skipped[:10]))
        if rest:
            await interaction.followup.send("\n".join(rest), ephemeral=True)

    @group.command(name="queue", description="Bugs waiting for a triage pass, oldest first")
    async def queue(interaction: discord.Interaction):
        if not await guard(interaction, "queue"):
            return
        bugs = sorted(ctx.clinic.triage_queue(), key=lambda b: b["created"])
        await reply_pages(interaction, list_pages(ctx.clinic, bugs, "Triage queue"))

    @group.command(name="find", description="Search by player name, Discord id, or NW bug id")
    async def find(interaction: discord.Interaction, who: str):
        if not await guard(interaction, "find"):
            return
        await reply_pages(interaction, list_pages(ctx.clinic, find_any(ctx.clinic, who), "Matches"))

    @group.command(name="mine", description="Bugs you reported yourself")
    async def mine(interaction: discord.Interaction):
        if not await guard(interaction, "mine"):
            return
        bugs = [b for b in ctx.clinic.bugs() if b["reporter_id"] == interaction.user.id]
        await reply_pages(interaction, list_pages(ctx.clinic, bugs, "Your reports"))

    @group.command(name="needlogs", description="Open bugs that still need logs")
    async def needlogs_(interaction: discord.Interaction):
        if not await guard(interaction, "needlogs"):
            return
        await reply_pages(interaction, list_pages(ctx.clinic, needs_logs(ctx.clinic), "Bugs needing logs"))

    @group.command(name="changed", description="What changed for a bug: its changelist items and fix status")
    async def changed(interaction: discord.Interaction, bug: str):
        bid, b = need_bug(bug)
        if not await guard(interaction, "changed", target=bid or "?"):
            return
        if not b:
            await interaction.response.send_message("No such bug (use NW-xxxxxxx).", ephemeral=True)
            return
        lines = [f"**{bid}** status {b['status']}, updated {iso(b['updated'])}"]
        lines += cl_bug_pages(changes(), bid)
        lines += [f"{iso(n['at'])} {n['who']} [{n['kind']}] {excerpt(safe(n['text']), 200)}"
                  for n in ctx.clinic.notes(bid) if n["kind"] == "status"]
        await reply_pages(interaction, paginate(lines))

    @group.command(name="health", description="Latest health summary of Iosefka")
    async def health(interaction: discord.Interaction):
        if not await guard(interaction, "health"):
            return
        await reply_pages(interaction, health_text(ctx.clinic))

    cl = app_commands.Group(name="cl", description="Changelist", parent=group)

    @cl.command(name="item", description="One changelist item in full (CL-xxxxxx)")
    async def cl_item(interaction: discord.Interaction, item: str):
        cid = norm_cl(item)
        if not await guard(interaction, "cl item", target=cid or "?"):
            return
        pages = cl_item_pages(changes(), cid, ctx.clinic) if cid else None
        if not pages:
            await interaction.response.send_message("No such item (use CL-xxxxxx).", ephemeral=True)
            return
        await reply_pages(interaction, pages)

    @cl.command(name="bug", description="Changelist items linked to a bug")
    async def cl_bug(interaction: discord.Interaction, bug: str):
        await bug_cmd(interaction, "cl bug", bug, lambda b: cl_bug_pages(changes(), b["id"]))

    TRACKS = [app_commands.Choice(name=t, value=t) for t in ("coop", "nw")]
    ISTAT = [app_commands.Choice(name=t, value=t) for t in ("planned", "in-progress", "done", "dropped")]

    @cl.command(name="list", description="Items of any version incl. future ones; every filter must match")
    @app_commands.describe(version="e.g. 0.7.8 (released, current or planned)", priority="rank / priority tag",
                           area="component / area tag", text="text, dev notes or id")
    @app_commands.choices(track=TRACKS, status=ISTAT)
    async def cl_list(interaction: discord.Interaction, track: str = None, version: str = None, status: str = None,
                      priority: str = None, area: str = None, text: str = None):
        used = [k for k, v in dict(track=track, version=version, status=status, priority=priority, area=area,
                                   text=text).items() if v]
        if not await guard(interaction, "cl list", filters=used):
            return
        try:
            pages = cl_list_pages(changes(), track=track, version=version, status=status, priority=priority,
                                  area=area, text=text)
        except ValueError as e:
            await interaction.response.send_message(str(e), ephemeral=True)
            return
        await reply_pages(interaction, pages)

    @cl.command(name="version", description="One version's items (list with a version)")
    @app_commands.choices(track=TRACKS)
    async def cl_version(interaction: discord.Interaction, version: str, track: str = None):
        if not await guard(interaction, "cl version", target=version[:20]):
            return
        try:
            pages = cl_list_pages(changes(), track=track, version=version)
        except ValueError as e:
            await interaction.response.send_message(str(e), ephemeral=True)
            return
        await reply_pages(interaction, pages)

    @cl.command(name="versions", description="All versions with status and item counts")
    @app_commands.choices(track=TRACKS)
    async def cl_versions(interaction: discord.Interaction, track: str = None):
        if not await guard(interaction, "cl versions"):
            return
        await reply_pages(interaction, cl_versions_pages(changes(), track))

    @cl.command(name="notes", description="Release notes markdown of a version (done items)")
    @app_commands.choices(track=TRACKS)
    async def cl_notes(interaction: discord.Interaction, track: str, version: str):
        if not await guard(interaction, "cl notes", target=version[:20]):
            return
        try:
            md = cl_notes_markdown(changes(), track, version) or "(no done items)\n"
        except ValueError as e:
            await interaction.response.send_message(str(e), ephemeral=True)
            return
        if len(md) < 1800:
            await interaction.response.send_message(f"```md\n{md}```", ephemeral=True, allowed_mentions=none)
        else:
            await interaction.response.send_message(
                f"Release notes {track} {version}", ephemeral=True,
                file=discord.File(io.BytesIO(md.encode()), filename=f"notes_{track}_{version}.md"))

    @cl.command(name="related", description="Bug -> its CL items; CL item -> its bugs and related items")
    async def cl_related(interaction: discord.Interaction, ref: str):
        if not await guard(interaction, "cl related", target=(norm_cl(ref) or norm_bug(ref) or "?")):
            return
        pages = cl_for_ref_pages(changes(), ctx.clinic, ref)
        if not pages:
            await interaction.response.send_message("Unknown reference (CL-xxxxxx or NW-xxxxxxx).", ephemeral=True)
            return
        await reply_pages(interaction, pages)

    if cfg.writes_enabled:
        @group.command(name="note", description="Add a note to a bug (separate write role)")
        async def note(interaction: discord.Interaction, bug: str, text: str):
            bid, b = need_bug(bug)
            if not await guard(interaction, "note", target=bid or "?", write=True):
                return
            if not b:
                await interaction.response.send_message("No such bug (use NW-xxxxxxx).", ephemeral=True)
                return
            add_staff_note(ctx.clinic, bid, str(interaction.user.name)[:32], text)
            await interaction.response.send_message(f"Note added to {bid}.", ephemeral=True)

    tree.add_command(group)
    return group


def _analysis(clinic, ref):
    try:
        row = clinic.db.execute("SELECT text FROM analyses WHERE ref=?", (ref,)).fetchone()
    except Exception:
        return None
    return {"text": row[0]} if row else None
