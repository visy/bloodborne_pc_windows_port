#!/bin/sh
# Turn off SSH password logins on the Nightwatch VPN server (keys only). Run as root:  sudo sh ssh-no-password.sh
# Undo:  sudo sh ssh-no-password.sh --undo
# The image ships /etc/ssh/sshd_config.d/00-permit-root.conf (PermitRootLogin yes) and 50-cloud-init.conf (likely
# PasswordAuthentication yes); sshd uses the FIRST value it reads, so this drop-in is named to sort before both.
# Root may still log in with a key (prohibit-password); the provider's web console stays the way back in.
# Open sessions are kept (reload, not restart). Check from a NEW terminal that your key login works before closing this one.
set -eu
F=/etc/ssh/sshd_config.d/00-nightwatch-keys-only.conf
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
if [ "${1:-}" = "--undo" ]; then
  rm -f "$F"; sshd -t && systemctl reload ssh; echo "password logins back to the image default"; exit 0
fi
[ -n "$(find /home /root -maxdepth 3 -name authorized_keys -size +0 2>/dev/null)" ] || { echo "no authorized_keys anywhere: refusing (you would lock yourself out)" >&2; exit 1; }
cat > "$F" <<'C'
# Nightwatch: keys only (tools/vpn/server/ssh-no-password.sh). Sorts before 00-permit-root / 50-cloud-init: first value wins.
PasswordAuthentication no
KbdInteractiveAuthentication no
PermitRootLogin prohibit-password
C
chmod 644 "$F"
sshd -t || { rm -f "$F"; echo "sshd config test failed; removed $F" >&2; exit 1; }
systemctl reload ssh
sshd -T 2>/dev/null | grep -E "^(passwordauthentication|kbdinteractiveauthentication|permitrootlogin) "
echo "Done. Test a key login from a new terminal now."
