#!/usr/bin/env python3
"""Discord bot for the co-op connection doctor (scripts/net_doctor.py).

  /netdoctor log1 [log2 .. log5] [private]

Players attach the logs of ONE session (host and guests). The bot saves them in a temporary folder,
runs net_doctor, deletes the files and replies. In a channel the reply hides player names and public
IPs (--redactnames --redactips); with private:True only the caller sees the reply, unredacted.
Nothing is stored. Setup and running as a service: tools/discord/README.md.

Environment:
  NETDOCTOR_TOKEN      the bot token (required; never commit it)
  NETDOCTOR_SCRIPT     path to net_doctor.py (default: ../../scripts/net_doctor.py from this file)
  NETDOCTOR_GUILD      optional server id: slash commands appear there at once (global sync can
                       take up to an hour)
  NETDOCTOR_MAX_MB     largest attachment accepted, MiB (default 40)
"""
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
    client = discord.Client(intents=intents)
    tree = app_commands.CommandTree(client)
    guild = discord.Object(id=int(os.environ["NETDOCTOR_GUILD"])) if os.environ.get("NETDOCTOR_GUILD") else None

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
        with tempfile.TemporaryDirectory(prefix="netdoctor_") as tmp:
            paths = []
            for i, a in enumerate(atts):
                # keep the original name: Nightwatch names carry the session's close time
                name = Path(a.filename).name or f"log{i}.log"
                p = Path(tmp) / f"{i}_{name}"
                await a.save(p)
                paths.append(p)
            text = diagnose(paths, redact=not private)
        parts = split_message(text)
        if len(parts) > 4:
            await interaction.followup.send("The diagnosis is long, attached as a file.", ephemeral=private,
                                            file=discord.File(io.BytesIO(text.encode()), "netdoctor.md"))
            return
        for part in parts:
            await interaction.followup.send(part, ephemeral=private)

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
