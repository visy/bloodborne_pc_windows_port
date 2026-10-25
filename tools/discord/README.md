# netdoctor Discord bot

`/netdoctor` runs the co-op connection doctor (`scripts/net_doctor.py`) on logs attached in Discord
and replies with a plain-words diagnosis.

- Attach the logs of **one session**: the host's and every guest's (up to 5).
- In a channel the reply hides player names and public IP addresses.
- `private: True` shows the reply only to you, with names and addresses.
- The bot keeps nothing: files go to a temporary folder and are deleted after the run.

## 1. Create the bot (Discord Developer Portal)

1. https://discord.com/developers/applications -> New Application -> name it (e.g. "Net Doctor").
2. Bot -> Reset Token -> copy the token (keep it secret; never commit it).
3. OAuth2 -> URL Generator: scopes `bot` and `applications.commands`; bot permissions `Send Messages`,
   `Attach Files`, `Use Slash Commands`. Open the generated URL and add the bot to the server (a server
   admin has to do this).
4. Optional: Discord settings -> Advanced -> Developer Mode, right-click the server -> Copy Server ID
   (makes the command appear at once instead of after up to an hour).

No privileged intents are needed.

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

Update the doctor's rules: `git -C ~/bbport pull` (the bot loads `net_doctor.py` fresh on every
command, no restart needed; restart after a change to the bot itself:
`systemctl --user restart netdoctor`).
