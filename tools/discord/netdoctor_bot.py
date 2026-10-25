#!/usr/bin/env python3
"""Discord bot for the co-op connection doctor (scripts/net_doctor.py).

  /netdoctor log1 [log2 .. log5] [private]   logs attached to the command
  /netdoctor-recent [minutes] [private]      every log file posted in this channel lately, by anyone
  message menu "Diagnose logs"               that message's logs + logs posted 30 min around it
  DM: send log files to the bot in a direct message; it answers privately (with names and IPs)
  auto (NETDOCTOR_AUTO): react (default) puts a stethoscope on messages with log files; reply also
  posts a redacted diagnosis of the channel's last 30 min of logs, 90 s after the last log arrived

The bot saves logs in a temporary folder, runs net_doctor, deletes the files and replies. In a channel the reply hides player names and public
IPs (--redactnames --redactips); with private:True only the caller sees the reply, unredacted.
Nothing is stored. Setup and running as a service: tools/discord/README.md.

Environment:
  NETDOCTOR_TOKEN      the bot token (required; never commit it)
  NETDOCTOR_SCRIPT     path to net_doctor.py (default: ../../scripts/net_doctor.py from this file)
  NETDOCTOR_GUILD      optional server id: the server whose channels get the auto reaction (commands
                       are global so they also work in DMs; a new global command can take a while to
                       appear)
  NETDOCTOR_MAX_MB     largest attachment accepted, MiB (default 40)
  NETDOCTOR_AUTO       off | react | reply (default react)
  NETDOCTOR_CHANNELS   ids of ordinary channels where mentions/reactions work too (default: none - only
                       threads and DMs; slash commands work everywhere)
  NETDOCTOR_DB         memory file (default ~/.config/netdoctor/netdoctor.db; rows older than 30 days
                       are dropped hourly)
  NETDOCTOR_DB_MAX_MB  size cap of that file, MiB (default 256, never above 1024): oldest rows go first
Needs the privileged Message Content intent (Developer Portal -> Bot) to read ordinary messages.
"""
import asyncio
import fnmatch
import importlib.util
import io
import os
import sys
import tempfile
import time
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = Path(os.environ.get("NETDOCTOR_SCRIPT", HERE.parents[1] / "scripts" / "net_doctor.py"))
MAX_BYTES = int(float(os.environ.get("NETDOCTOR_MAX_MB", "40")) * 1024 * 1024)
DISCORD_LIMIT = 2000
MAX_LOGS = 8          # logs per diagnosis (one session: host + up to 3 guests, plus earlier halves)
AUTO_DEBOUNCE_S = 90  # reply mode: wait this long after the last posted log
DM_DEBOUNCE_S = 15    # a DM to the bot: wait for the rest of the files, then answer

# File names players attach: bbport logs\last_session.log / previous_session.log / session-*.log,
# Nightwatch nightwatch-*.log, and Discord's own name for pasted text (message.txt, message(3).txt).
LOG_PATTERNS = ("*.log", "message*.txt", "*session*.txt", "nightwatch*.txt")


def is_log_name(name):
    n = Path(name or "").name.lower()
    return any(fnmatch.fnmatch(n, p) for p in LOG_PATTERNS)


def load_doctor():
    """net_doctor as a module, loaded fresh each call so a git pull takes effect without a restart."""
    spec = importlib.util.spec_from_file_location("net_doctor", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def diagnose(paths, redact):
    """Run net_doctor on local files; returns its text."""
    nd = load_doctor()
    args = (["--redactnames", "--redactips"] if redact else []) + [str(p) for p in paths]
    buf = io.StringIO()
    with redirect_stdout(buf):
        try:
            nd.main(args)
        except SystemExit:
            pass
    return buf.getvalue().strip() or "No output from the doctor."


def split_message(text, limit=DISCORD_LIMIT):
    """Discord-sized chunks, split at line ends; a code block is never left open across a cut."""
    chunks, cur = [], ""
    for line in text.splitlines(keepends=True):
        while len(line) > limit:
            chunks.append(line[:limit])
            line = line[limit:]
        if len(cur) + len(line) > limit:
            chunks.append(cur)
            cur = ""
        cur += line
    if cur:
        chunks.append(cur)
    fixed, carry = [], False
    for c in chunks:
        if carry:
            c = "```\n" + c
        carry = c.count("```") % 2 == 1
        fixed.append(c + ("\n```" if carry else ""))
    return fixed


SMALL_TALK = ("thanks", "thank", "you", "thx", "ty", "tysm", "hi", "hello", "hey", "nice", "great", "good",
              "bot", "<3", ":)", "ok", "okay")
DB_PATH = Path(os.environ.get("NETDOCTOR_DB", Path.home() / ".config" / "netdoctor" / "netdoctor.db"))


def wants_analysis(content, bot_id):
    """Any message that mentions the bot asks for a (re)analysis, unless it is only thanks / a greeting."""
    text = content.replace(f"<@{bot_id}>", " ").replace(f"<@!{bot_id}>", " ").lower()
    words = [w.strip(".,!?:;()") for w in text.split()]
    words = [w for w in words if w]
    return not words or not all(w in SMALL_TALK for w in words)


def log_info(nd, path):
    """Who and when of one log (for grouping logs into sessions)."""
    import re
    lg = nd.parse(path)
    host = lg.name if lg.role == "host" else None
    if host is None:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.search(r"roster: ([^(\s]+)\(slot 0,", line)
                if m:
                    host = m.group(1)
                    break
    peers = set(lg.guests) | set(lg.pings) | ({host} if host else set())
    peers.discard(lg.name)
    return {"name": lg.name, "role": lg.role, "host": host,
            "start": nd._minutes(*lg.started) if lg.started else None,
            "end": getattr(lg, "ended", None), "day": lg.started[0] if lg.started else "", "peers": peers}


def main():
    import discord
    from discord import app_commands
    sys.path.insert(0, str(HERE))
    import netdoctor_store as ns

    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    store = ns.Store(DB_PATH)
    try:
        os.chmod(DB_PATH, 0o600)  # player names inside
    except OSError:
        pass
    db_max = int(float(os.environ.get("NETDOCTOR_DB_MAX_MB", "256")) * 1024 * 1024)
    store.prune(max_bytes=db_max)
    pruner = []

    token = os.environ.get("NETDOCTOR_TOKEN")
    if not token:
        print("set NETDOCTOR_TOKEN", file=sys.stderr)
        return 1
    if not SCRIPT.is_file():
        print(f"net_doctor.py not found: {SCRIPT}", file=sys.stderr)
        return 1

    intents = discord.Intents.default()
    # Reading attachments of ordinary messages (auto mode, /netdoctor-recent, the message menu) needs the
    # privileged Message Content intent: Developer Portal -> Bot -> Message Content Intent.
    intents.message_content = True
    client = discord.Client(intents=intents)
    tree = app_commands.CommandTree(client)
    guild = discord.Object(id=int(os.environ["NETDOCTOR_GUILD"])) if os.environ.get("NETDOCTOR_GUILD") else None
    auto = os.environ.get("NETDOCTOR_AUTO", "react").lower()  # off | react | reply
    allowed_channels = {int(x) for x in os.environ.get("NETDOCTOR_CHANNELS", "").replace(",", " ").split()
                        if x.isdigit()}
    pending = {}  # channel id -> asyncio task (reply mode debounce)

    async def keep(path, filename):
        """Store the log compressed (off the event loop: LZMA takes a moment on big logs)."""
        data = path.read_bytes()
        packed = await asyncio.get_running_loop().run_in_executor(None, ns.pack, data)
        return store.put_logdata(data, filename, packed)  # the database only from this thread

    last_hashes = {}

    async def run_on(attachments, redact):
        """Download attachments (each into its own folder, keeping its name), keep them compressed, run
        the doctor. The content hashes land in last_hashes[id(attachments)]."""
        with tempfile.TemporaryDirectory(prefix="netdoctor_") as tmp:
            paths, hashes = [], []
            for i, a in enumerate(attachments):
                d = Path(tmp) / str(i)
                d.mkdir()
                # the original name matters: Nightwatch names carry the time the log was closed
                p = d / (Path(a.filename).name or "log.log")
                await a.save(p)
                paths.append(p)
                hashes.append(await keep(p, a.filename))
            last_hashes[id(attachments)] = hashes
            return diagnose(paths, redact=redact)

    def remember(channel, files, text, bot_message_id=None, ref=None):
        """Store an analysis under a short reference (shown in the post) for the server-side lookup
        tool (tools/discord/netdoctor_query.py). Returns the reference."""
        ref = ref or ns.new_ref()
        store.add_analysis(ref, getattr(channel, "id", 0), getattr(getattr(channel, "guild", None), "id", 0),
                           bot_message_id, files, text)
        return ref

    def file_recs(entries, hashes=()):
        """(message or None, index, attachment) -> records the lookup tool can load again (stored copy by
        hash first, Discord second)."""
        hashes = list(hashes) + [""] * len(entries)
        return [{"channel": getattr(getattr(m, "channel", None), "id", 0), "message": getattr(m, "id", 0),
                 "index": i, "url": a.url, "filename": a.filename, "sha1": h}
                for (m, i, a), h in zip(entries, hashes)]

    async def send_text(send, text, private, ref=None):
        if ref:
            text = text + f"\n-# ref {ref}"
        parts = split_message(text)
        if len(parts) > 4:
            await send("The diagnosis is long, attached as a file.", ephemeral=private,
                       file=discord.File(io.BytesIO(text.encode()), "netdoctor.md"))
            return
        for part in parts:
            await send(part, ephemeral=private)

    async def recent_logs(channel, minutes, around=None):
        """Party log attachments posted in the channel within `minutes` (of now, or of `around`)."""
        import datetime as dt
        center = around or dt.datetime.now(dt.timezone.utc)
        after = center - dt.timedelta(minutes=minutes)
        before = center + dt.timedelta(minutes=minutes) if around else None
        found = []
        async for msg in channel.history(limit=300, after=after, before=before, oldest_first=True):
            found += [(msg, i, a) for i, a in enumerate(msg.attachments)
                      if is_log_name(a.filename) and a.size <= MAX_BYTES]
        return found[-MAX_LOGS:]

    @tree.command(name="netdoctor", description="Explain co-op connection problems from a session's logs "
                                                "(host + guests)")
    @app_commands.allowed_contexts(guilds=True, dms=True, private_channels=True)
    @app_commands.describe(log1="host or guest log", log2="another player's log", log3="another player's log",
                           log4="another player's log", log5="another player's log",
                           private="only you see the reply, with names and IPs (default: public, hidden)")
    async def netdoctor(interaction: discord.Interaction, log1: discord.Attachment,
                        log2: discord.Attachment = None, log3: discord.Attachment = None,
                        log4: discord.Attachment = None, log5: discord.Attachment = None,
                        private: bool = False):
        await interaction.response.defer(ephemeral=private, thinking=True)
        atts = [a for a in (log1, log2, log3, log4, log5) if a is not None]
        too_big = [a.filename for a in atts if a.size > MAX_BYTES]
        if too_big:
            await interaction.followup.send(f"Too large (max {MAX_BYTES // (1024 * 1024)} MB): "
                                            + ", ".join(too_big), ephemeral=private)
            return
        text = await run_on(atts, redact=not private)
        ref = remember(interaction.channel, file_recs([(None, i, a) for i, a in enumerate(atts)],
                                                      last_hashes.pop(id(atts), ())), text)
        await send_text(interaction.followup.send, text, private, ref)

    @tree.command(name="netdoctor-recent", description="Diagnose every party log posted in this channel "
                                                       "recently (by anyone)")
    @app_commands.allowed_contexts(guilds=True, dms=True, private_channels=True)
    @app_commands.describe(minutes="how far back to look (default 60)",
                           private="only you see the reply, with names and IPs")
    async def netdoctor_recent(interaction: discord.Interaction, minutes: int = 60, private: bool = False):
        await interaction.response.defer(ephemeral=private, thinking=True)
        found = await recent_logs(interaction.channel, max(1, min(minutes, 24 * 60)))
        if not found:
            await interaction.followup.send(f"No log files posted here in the last {minutes} min.",
                                            ephemeral=private)
            return
        atts = [a for _, _, a in found]
        text = await run_on(atts, redact=not private)
        await send_text(interaction.followup.send, text, private,
                        remember(interaction.channel, file_recs(found, last_hashes.pop(id(atts), ())), text))

    @tree.context_menu(name="Diagnose logs")
    @app_commands.allowed_contexts(guilds=True, dms=True, private_channels=True)
    async def diagnose_menu(interaction: discord.Interaction, message: discord.Message):
        # this message's logs plus logs posted within 30 min around it (the other players' halves)
        await interaction.response.defer(ephemeral=True, thinking=True)
        found = await recent_logs(message.channel, 30, around=message.created_at)
        if not found:
            await interaction.followup.send("No log files in or around that message.", ephemeral=True)
            return
        atts = [a for _, _, a in found]
        text = await run_on(atts, redact=False)
        await send_text(interaction.followup.send, text, True,
                        remember(message.channel, file_recs(found, last_hashes.pop(id(atts), ())), text))

    async def auto_reply(channel, dm=False):
        await asyncio.sleep(DM_DEBOUNCE_S if dm else AUTO_DEBOUNCE_S)  # wait for the other logs
        pending.pop(channel.id, None)
        found = await recent_logs(channel, 30)
        if found:
            # a DM is private: names and addresses stay in
            atts = [a for _, _, a in found]
            text = await run_on(atts, redact=not dm)
            await send_text(lambda *a, ephemeral=False, **k: channel.send(*a, **k), text, False,
                            remember(channel, file_recs(found, last_hashes.pop(id(atts), ())), text))

    async def record_logs(message):
        """Remember who/when of every party log file in a message (downloaded once, then deleted)."""
        new = 0
        for idx, a in enumerate(message.attachments):
            if not is_log_name(a.filename) or a.size > MAX_BYTES or \
                    store.has_log(message.channel.id, message.id, idx):
                continue
            with tempfile.TemporaryDirectory(prefix="netdoctor_") as tmp:
                p = Path(tmp) / (Path(a.filename).name or "log.log")
                await a.save(p)
                sha = await keep(p, a.filename)
                try:
                    info = log_info(load_doctor(), p)
                except Exception as e:  # a broken file must not stop the others
                    print(f"cannot read {a.filename}: {e}", flush=True)
                    continue
            if info["role"] is None:
                continue  # not a party log
            info["sha1"] = sha
            store.add_log(message.channel.id, message.id, idx, a.filename, info)
            new += 1
        return new

    async def analyse_channel(trigger):
        """(Re)analyse every party log posted in this channel / thread: one post per session; a session
        that already has a post gets it edited, a new session gets a new post."""
        channel = trigger.channel
        async for msg in channel.history(limit=1000, oldest_first=True):
            if msg.attachments and not msg.author.bot:
                await record_logs(msg)
        infos = store.logs(channel.id)
        if not infos:
            await trigger.reply("No party logs here yet. Post the logs of one session (the host's and every "
                                "guest's), then mention me again.", mention_author=False)
            return
        changed = 0
        for group in ns.group_sessions(infos):
            key = ns.session_key(infos, group)
            members = [(infos[i]["message_id"], infos[i]["att_index"]) for i in group]
            found = store.find_session(channel.id, key, members)
            if found and found[2] == len(members):
                continue  # nothing new for this session
            with tempfile.TemporaryDirectory(prefix="netdoctor_") as tmp:
                paths = []
                for n, (mid, idx) in enumerate(members):
                    d = Path(tmp) / str(n)
                    d.mkdir()
                    sha = infos[group[n]].get("sha1", "")
                    kept = store.get_logdata(sha) if sha else None
                    if kept:  # the stored copy: no download, works after the message is gone
                        p = d / (Path(kept[0] or "log.log").name)
                        p.write_bytes(kept[1])
                        paths.append(p)
                        continue
                    try:
                        att = (await channel.fetch_message(mid)).attachments[idx]
                    except (discord.HTTPException, IndexError):
                        continue  # deleted meanwhile and never stored
                    p = d / (Path(att.filename).name or "log.log")
                    await att.save(p)
                    paths.append(p)
                if not paths:
                    continue
                text = diagnose(paths, redact=True)
            stamp = int(time.time())
            ref = "ND-" + ((found[0] if found else None) or key)[:8]
            footer = f"\n-# ref {ref} · {len(paths)} log(s) of this session · updated <t:{stamp}:R> · " \
                     f"mention me again after more logs are posted"
            files = []
            body = text + footer
            if len(body) > DISCORD_LIMIT:
                body = text[:DISCORD_LIMIT - len(footer) - 60].rsplit("\n", 1)[0] + \
                    "\n… (full analysis attached)" + footer
                files = [discord.File(io.BytesIO(text.encode()), "netdoctor.md")]
            old = None
            if found and found[1]:
                try:
                    old = await channel.fetch_message(found[1])
                except discord.HTTPException:
                    old = None
            if old:
                await old.edit(content=body, attachments=files)
                bot_msg = old
            else:
                bot_msg = await channel.send(body, files=files)
            store.set_session(channel.id, key, bot_msg.id, members, old_key=found[0] if found else None)
            remember(channel, [{"channel": channel.id, "message": mid, "index": idx, "url": "",
                                "filename": infos[i].get("filename", ""), "sha1": infos[i].get("sha1", "")}
                               for (mid, idx), i in zip(members, group)], text, bot_msg.id, ref)
            changed += 1
        try:
            await trigger.add_reaction("\N{WHITE HEAVY CHECK MARK}" if changed else "\N{SLEEPING SYMBOL}")
        except discord.HTTPException:
            pass

    def mentioned(message):
        """The bot itself, or its own managed role (Discord offers both when you type @Net Doctor...)."""
        if client.user in message.mentions:
            return True
        return any(getattr(r.tags, "bot_id", None) == client.user.id for r in message.role_mentions if r.tags)

    def small_place(channel):
        """Threads, DMs and channels listed in NETDOCTOR_CHANNELS: the bot answers mentions and reacts there.
        Big ordinary channels are left alone (slash commands still work everywhere)."""
        if isinstance(channel, (discord.Thread, discord.DMChannel, discord.GroupChannel)):
            return True
        return channel.id in allowed_channels

    @client.event
    async def on_message(message):
        if message.author.bot:
            return
        ours = message.guild is None or not guild or message.guild.id == guild.id
        if not ours or not small_place(message.channel):
            return
        if mentioned(message) and wants_analysis(message.content, client.user.id):
            print(f"mention in {type(message.channel).__name__} {message.channel.id}", flush=True)
            try:
                async with message.channel.typing():
                    await analyse_channel(message)
            except discord.Forbidden as e:
                print(f"no access in {message.channel.id}: {e}", flush=True)
                try:
                    await message.reply("I can't read this place's history or post here. A server admin needs "
                                        "to give me View Channel, Read Message History, Send Messages and Send "
                                        "Messages in Threads here (channel/forum permissions).",
                                        mention_author=False)
                except discord.HTTPException:
                    pass
            return
        if not any(is_log_name(a.filename) for a in message.attachments):
            return
        asyncio.create_task(record_logs(message))  # remembered for a later "@bot update"
        if message.guild is None:  # a DM with the bot: answer on its own
            old = pending.get(message.channel.id)
            if old:
                old.cancel()
            pending[message.channel.id] = asyncio.create_task(auto_reply(message.channel, dm=True))
            return
        if auto == "off":
            return
        try:
            await message.add_reaction("🩺")
        except discord.HTTPException:
            pass
        if auto == "reply":
            old = pending.get(message.channel.id)
            if old:
                old.cancel()
            pending[message.channel.id] = asyncio.create_task(auto_reply(message.channel))

    async def prune_hourly():
        while True:
            await asyncio.sleep(3600)
            size = store.prune(max_bytes=db_max)
            print(f"db pruned: {size // 1024} KiB", flush=True)

    @client.event
    async def on_ready():
        if not pruner:
            pruner.append(asyncio.create_task(prune_hourly()))
        print("in servers: " + (", ".join(f"{g.name} ({g.id})" for g in client.guilds) or "none - invite the "
              "bot with the OAuth2 URL (scopes bot + applications.commands)"), flush=True)
        try:
            if guild:
                # 0.7.6 first registered the commands on the server only: remove that copy (it would
                # show every command twice next to the global ones)
                tree.clear_commands(guild=guild)
                await tree.sync(guild=guild)
            await tree.sync()
        except discord.Forbidden:
            print(f"cannot register commands in server {guild.id if guild else '(global)'}: the bot is not in it "
                  f"or was invited without the applications.commands scope; check NETDOCTOR_GUILD and re-invite",
                  flush=True)
            return
        print(f"netdoctor bot ready as {client.user} (script {SCRIPT})", flush=True)

    client.run(token)
    return 0


if __name__ == "__main__":
    sys.exit(main())
