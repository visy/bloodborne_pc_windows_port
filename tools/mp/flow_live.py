#!/usr/bin/env python3
"""Live test of the in-game party flow through the REAL menus (docs/party/ingame_flow.md): two
instances start as the single-player game; the title's Party page is walked with the game's own pad
path (BB_TEST_MENU_FILE: MenuScript lines, test_newgame.h; text typed into the window's text box as
SDL key events, no OS input) and every restart goes through run.bat's exit-75 loop.

  python tools/mp/flow_live.py fresh    --root DIR --game DIR   # 1+2: New party game / Join party game
  python tools/mp/flow_live.py continue --root DIR --game DIR   # 3: Continue party game, both
      [--guest-world SCRIPT]     # 4: then in the world (MenuScript): e.g. Party > Leave, Join party

Screenshots go to ROOT/shots/<phase>/ (tools/mp/shot.py --watch on the run logs). The password is
PASSWORD below; the guest types the host's 127.0.0.1:<port> address.
"""
import argparse, json, os, shutil, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(HERE))
import instances  # noqa: E402

PASSWORD = "Moon-42"

# The title menu (empty single-player save): New Game, Party, System, Log In. The Party page opens
# on Status: Status, New party game, Join party game, [Continue party game,] Host address, Password,
# Defaults. Value rows act on Left / Right ("Confirm" rows: twice).
TITLE_TO_PARTY = "~down@New Game/cross/~mark=page@Host address"


def scripts(phase, port0):
    host_addr = f"127.0.0.1:{port0}"
    if phase == "fresh":
        host = [TITLE_TO_PARTY + "/down/down/down/down/right/type=" + PASSWORD + "/mark=h_pw_typed/enter/mark=h_pw_saved"
                "/up/up/up/mark=h_new/right/mark=h_new_again/right/mark=h_restart", "-"]
        guest = [TITLE_TO_PARTY + "/down/down/down/right/type=" + host_addr + "/mark=g_addr_typed/enter/mark=g_addr_saved"
                 "/down/right/type=" + PASSWORD + "/mark=g_pw_typed/enter/mark=g_pw_saved"
                 "/up/up/mark=g_join/right/mark=g_join_again/right/mark=g_restart", "-"]
        return host, guest
    if phase == "continue":
        # With a party character: ... Join party game, Continue party game, Host address ...
        line = TITLE_TO_PARTY + "/down/down/down/mark={w}_cont/right/mark={w}_cont_again/right/mark={w}_restart"
        # The guest first opens Host address twice (the saved address comes up; Enter keeps it, Esc
        # cancels): every press of a text row opens the box.
        guest = (TITLE_TO_PARTY + "/down/down/down/down/right/mark=g_addr_open/enter/mark=g_addr_kept/right"
                 "/mark=g_addr_open2/esc/up/mark=g_cont/right/mark=g_cont_again/right/mark=g_restart")
        return [line.format(w="h"), "-"], [guest, "-"]
    raise SystemExit("unknown phase " + phase)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("phase", choices=["fresh", "continue"])
    ap.add_argument("--root", required=True)
    ap.add_argument("--game", required=True)
    ap.add_argument("--seconds", type=int, default=420)
    ap.add_argument("--guest-world", default="", help="the guest's in-world walk after the restart")
    ap.add_argument("--host-world", default="", help="the host's in-world walk after the restart")
    ap.add_argument("--until", action="append", default=[])
    a = ap.parse_args()
    root = Path(a.root)
    port0 = instances.base_port(root)
    host, guest = scripts(a.phase, port0)
    host = [("title 5 " + l) if l != "-" else l for l in host]
    guest = [("title 5 " + l) if l != "-" else l for l in guest]
    # The party process after the restart: an in-world walk (Options > System > Party ...).
    if a.host_world:
        host[1] = "joined 8 " + a.host_world
    if a.guest_world:
        guest[1] = "joined 8 " + a.guest_world
    for i, lines in enumerate((host, guest)):
        user = root / f"inst{i}" / "user"
        user.mkdir(parents=True, exist_ok=True)
        if a.phase == "fresh":
            for name in ("party.json", "party_code.txt", "party_relaunch.bat", "party_state.json"):
                (user / name).unlink(missing_ok=True)
            for d in ("savedata", "savedata_party"):
                shutil.rmtree(user / d, ignore_errors=True)
        (root / f"menu{i}.txt").write_text("\n".join(lines) + "\n", encoding="ascii")
    shots = root / "shots" / a.phase
    shutil.rmtree(shots, ignore_errors=True)
    shots.mkdir(parents=True)
    watch = []
    for tag in ("page", "h_pw_typed", "h_pw_saved", "h_new", "h_new_again", "g_addr_typed", "g_addr_saved",
                "g_pw_typed", "g_pw_saved", "g_join", "g_join_again", "g_addr_open", "g_addr_kept", "g_addr_open2", "h_cont", "h_cont_again", "g_cont", "g_cont_again"):
        watch += ["--watch", rf"step \d+/\d+: ~?mark={tag}(@|\s)={tag}@1.2"]
    for n in range(1, 16):  # in-world walks: mark=w1 ... mark=w15
        watch += ["--watch", rf"step \d+/\d+: ~?mark=w{n}(@|\s)=w{n}@1.2"]
    watch += ["--watch", r"relaunching \(exit code 75=relaunch@0.3",
              "--watch", r"Party: starting the party game \(join\)=restarted@25,45",
              "--watch", r"newgame: .*done=newgame_done@2",
              "--watch", r"session trying-to-host .* -> host=hosting@3",
              "--watch", r"session trying-to-join .* -> client \(6=summoned@1,3",
              "--watch", r"test menu: step .*(leave|join)\w*=world_step@1.5",
              "--watch", r"Party flow: leave=left@4",
              "--watch", r"STUCK=stuck@0"]
    shot = subprocess.Popen([sys.executable, str(HERE / "shot.py"), "--root", str(root), "--out", str(shots),
                             "--seconds", "7200"] + watch)
    env = [f"BB_TEST_MENU_FILE={root}\\menu{{i}}.txt", "BB_PARTY_PORT={port}", "BB_PARTY_UPNP=0",
           "BB_PARTY_STUN=off", "BB_TEST_NEWGAME=party", "BB_TEST_PROLOGUE_SKIP=party",
           "BB_TEST_NAME=Hunter{i}"]
    cmd = [sys.executable, str(HERE / "instances.py"), "run", "--count", "2", "--root", str(root), "--game", a.game,
           "--saves", str(root / "nosaves"), "--build", str(REPO), "--seconds", str(a.seconds), "--idle", "0",
           "--label", f"flowlive {a.phase}"]
    for e in env:
        cmd += ["--env", e]
    # The summon, and a little of the log after it (the screenshots at +3 s).
    for u in a.until or [r"(?m)^Party: session trying-to-join .* -> client \(6[\s\S]{1500}"]:
        cmd += ["--until", u]
    subprocess.run([sys.executable, str(HERE / "instances.py"), "setup", "--count", "2", "--root", str(root),
                    "--build", str(REPO), "--game", a.game, "--saves", str(root / "nosaves")], check=True)
    r = subprocess.run(cmd)
    shot.terminate()
    for i in range(2):
        pj = root / f"inst{i}" / "user" / "party.json"
        print(f"inst{i} party.json:", pj.read_text(encoding="utf-8") if pj.exists() else "(none)")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
