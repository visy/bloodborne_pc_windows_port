#!/usr/bin/env python3
"""Discord bot for the co-op connection doctor (scripts/net_doctor.py).

  /netdoctor log1 [log2 .. log5] [private]   logs attached to the command
  /netdoctor-recent [minutes] [private]      every log file posted in this channel lately, by anyone
  message menu "Diagnose logs"               that message's logs + logs posted 30 min around it
  auto (NETDOCTOR_AUTO): react (default) puts a stethoscope on messages with log files; reply also
  posts a redacted diagnosis of the channel's last 30 min of logs, 90 s after the last log arrived

The bot saves logs in a temporary folder, runs net_doctor, deletes the files and replies. In a channel the reply hides player names and public
IPs (--redactnames --redactips); with private:True only the caller sees the reply, unredacted.
Nothing is stored. Setup and running as a service: tools/discord/README.md.

Environment:
  NETDOCTOR_TOKEN      the bot token (required; never commit it)
  NETDOCTOR_SCRIPT     path to net_doctor.py (default: ../../scripts/net_doctor.py from this file)
  NETDOCTOR_GUILD      optional server id: slash commands appear there at once (global sync can
                       take up to an hour)
  NETDOCTOR_MAX_MB     largest attachment accepted, MiB (default 40)
  NETDOCTOR_AUTO       off | react | reply (default react)
Needs the privileged Message Content intent (Developer Portal -> Bot) to read ordinary messages.
"""
import asyncio
import fnmatch
import importlib.util
import io
import os
import sys
import tempfile
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = Path(os.environ.get("NETDOCTOR_SCRIPT", HERE.parents[1] / "scripts" / "net_doctor.py"))
MAX_BYTES = int(float(os.environ.get("NETDOCTOR_MAX_MB", "40")) * 1024 * 1024)
DISCORD_LIMIT = 2000
MAX_LOGS = 8          # logs per diagnosis (one session: host + up to 3 guests, plus earlier halves)
AUTO_DEBOUNCE_S = 90  # reply mode: wait this long after the last posted log

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


def main():
    import discord
    from discord import app_commands

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
    pending = {}  # channel id -> asyncio task (reply mode debounce)

    async def run_on(attachments, redact):
        """Download attachments (each into its own folder, keeping its name), run the doctor."""
        with tempfile.TemporaryDirectory(prefix="netdoctor_") as tmp:
            paths = []
            for i, a in enumerate(attachments):
                d = Path(tmp) / str(i)
                d.mkdir()
                # the original name matters: Nightwatch names carry the time the log was closed
                p = d / (Path(a.filename).name or "log.log")
                await a.save(p)
                paths.append(p)
            return diagnose(paths, redact=redact)

    async def send_text(send, text, private):
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
            found += [a for a in msg.attachments if is_log_name(a.filename) and a.size <= MAX_BYTES]
        return found[-MAX_LOGS:]

    @tree.command(name="netdoctor", description="Explain co-op connection problems from a session's logs "
                                                "(host + guests)", guild=guild)
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
        await send_text(interaction.followup.send, await run_on(atts, redact=not private), private)

    @tree.command(name="netdoctor-recent", description="Diagnose every party log posted in this channel "
                                                       "recently (by anyone)", guild=guild)
    @app_commands.describe(minutes="how far back to look (default 60)",
                           private="only you see the reply, with names and IPs")
    async def netdoctor_recent(interaction: discord.Interaction, minutes: int = 60, private: bool = False):
        await interaction.response.defer(ephemeral=private, thinking=True)
        atts = await recent_logs(interaction.channel, max(1, min(minutes, 24 * 60)))
        if not atts:
            await interaction.followup.send(f"No log files posted here in the last {minutes} min.",
                                            ephemeral=private)
            return
        await send_text(interaction.followup.send, await run_on(atts, redact=not private), private)

    @tree.context_menu(name="Diagnose logs", guild=guild)
    async def diagnose_menu(interaction: discord.Interaction, message: discord.Message):
        # this message's logs plus logs posted within 30 min around it (the other players' halves)
        await interaction.response.defer(ephemeral=True, thinking=True)
        atts = await recent_logs(message.channel, 30, around=message.created_at)
        if not atts:
            await interaction.followup.send("No log files in or around that message.", ephemeral=True)
            return
        await send_text(interaction.followup.send, await run_on(atts, redact=False), True)

    async def auto_reply(channel):
        await asyncio.sleep(AUTO_DEBOUNCE_S)  # wait for the other players' logs
        pending.pop(channel.id, None)
        atts = await recent_logs(channel, 30)
        if atts:
            await send_text(lambda *a, ephemeral=False, **k: channel.send(*a, **k),
                            await run_on(atts, redact=True), False)

    @client.event
    async def on_message(message):
        if message.author.bot or auto == "off" or message.guild is None:
            return
        if guild and message.guild.id != guild.id:
            return
        if not any(is_log_name(a.filename) for a in message.attachments):
            return
        try:
            await message.add_reaction("\N{STETHOSCOPE}")
        except discord.HTTPException:
            pass
        if auto == "reply":
            old = pending.get(message.channel.id)
            if old:
                old.cancel()
            pending[message.channel.id] = asyncio.create_task(auto_reply(message.channel))

    @client.event
    async def on_ready():
        if guild:
            await tree.sync(guild=guild)
        else:
            await tree.sync()
        print(f"netdoctor bot ready as {client.user} (script {SCRIPT})", flush=True)

    client.run(token)
    return 0


if __name__ == "__main__":
    sys.exit(main())
