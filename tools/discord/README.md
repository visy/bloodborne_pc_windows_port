# netdoctor Discord bot

The bot runs the co-op connection doctor (`scripts/net_doctor.py`) on logs posted in Discord and
replies with a plain-words diagnosis:

- `/netdoctor` with up to 5 log files attached to the command.
- `/netdoctor-recent [minutes]`: every log file posted in the channel in the last N minutes (default
  60), by anyone - players post their halves of a session in separate messages.
- Right-click a message -> Apps -> **Diagnose logs**: that message's logs plus the logs posted 30
  minutes around it (the reply is private).
- DM: send log files to the bot in a direct message; it answers privately, with names and
  addresses (the commands work in DMs too).
- Mentions and the automatic reaction work only in **threads and DMs** (big channels are left alone;
  `NETDOCTOR_CHANNELS=<id,id>` in the env file adds ordinary channels). Slash commands work everywhere.
- In a bug thread: mention the bot, e.g. `@Net Doctor Iosefka analyse` (any message mentioning her
  counts, except a plain thanks/hello). It reads every log posted in the thread/channel,
  groups them into sessions (same host, overlapping times) and posts one analysis per session. Mention
  it again after more logs arrive, even hours later: a session that already has a post gets that post
  edited, a new session gets a new post. The bot remembers who/when of each posted log and which post
  belongs to which session in a SQLite file (`~/.config/netdoctor/netdoctor.db`).
- Auto: the bot puts a stethoscope reaction on messages with log files (`NETDOCTOR_AUTO=react`,
  default); with `NETDOCTOR_AUTO=reply` it also posts a redacted diagnosis 90 s after the last log.

- Attach the logs of **one session**: the host's and every guest's (up to 5).
- In a channel the reply hides player names and public IP addresses.
- `private: True` shows the reply only to you, with names and addresses.
- The bot keeps nothing: files go to a temporary folder and are deleted after the run.

## Memory and lookup by reference

Every analysis ends with `ref ND-xxxxxxxx`. The bot keeps, for 30 days and within a size cap (default
256 MiB, never above 1 GiB, oldest first): each analysis text, which logs it used, and the log files
themselves LZMA-compressed (about 14x smaller, deduplicated by content). On the server:

```sh
~/netdoctor-venv/bin/python ~/bbport/tools/discord/netdoctor_query.py --list
~/netdoctor-venv/bin/python ~/bbport/tools/discord/netdoctor_query.py ND-1a2b3c4d            # analysis + logs
~/netdoctor-venv/bin/python ~/bbport/tools/discord/netdoctor_query.py ND-1a2b3c4d --rerun    # unredacted rerun
~/netdoctor-venv/bin/python ~/bbport/tools/discord/netdoctor_query.py ND-1a2b3c4d --logs DIR # extract the logs
```

The file holds player names and logs: it is readable only by the bot's user (0600).

## 1. Create the bot (Discord Developer Portal)

1. https://discord.com/developers/applications -> New Application -> name it (e.g. "Net Doctor").
2. Bot -> Reset Token -> copy the token (keep it secret; never commit it).
3. Bot -> Privileged Gateway Intents: turn on **Message Content Intent** (needed to read log files
   posted in ordinary messages). The other two privileged intents stay off.
4. OAuth2 -> URL Generator: scopes `bot` and `applications.commands`; bot permissions `View Channels`,
   `Read Message History`, `Send Messages`, `Attach Files`, `Add Reactions`, `Use Slash Commands`. Open the generated URL and add the bot to the server (a server
   admin has to do this).
5. Optional: Discord settings -> Advanced -> Developer Mode, right-click the server -> Copy Server ID
   (makes the command appear at once instead of after up to an hour).

If the bot was already invited, re-invite it with the new URL to add the extra permissions.

## 2. Run it on a Linux server

```sh
git clone https://github.com/visy/bloodborne_pc_windows_port.git ~/bbport
python3 -m venv ~/netdoctor-venv
~/netdoctor-venv/bin/pip install -r ~/bbport/tools/discord/requirements.txt
mkdir -p ~/.config/netdoctor && chmod 700 ~/.config/netdoctor
printf 'NETDOCTOR_TOKEN=%s\nNETDOCTOR_GUILD=%s\n' '<token>' '<server id>' > ~/.config/netdoctor/env
chmod 600 ~/.config/netdoctor/env
```

As a user service (starts on boot, restarts on failure):

```sh
mkdir -p ~/.config/systemd/user
cp ~/bbport/tools/discord/netdoctor.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now netdoctor
loginctl enable-linger "$USER"      # keep it running when you are logged out
journalctl --user -u netdoctor -f   # log
```

No systemd lingering (e.g. no polkit, `loginctl enable-linger` fails)? Use cron instead of the
service: `crontab -e` and add the two lines from the top of `tools/discord/keepalive.sh`.

Housekeeping (hourly): `17 * * * * sh $HOME/bbport/tools/discord/cleanup.sh` in `crontab -e` removes
upload folders a crash left in /tmp and trims the bot log past 10 MB.

Update the doctor's rules: `git -C ~/bbport pull` (the bot loads `net_doctor.py` fresh on every
command, no restart needed; restart after a change to the bot itself:
`systemctl --user restart netdoctor`).
