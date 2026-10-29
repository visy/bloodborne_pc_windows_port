#!/usr/bin/env python3
"""Screenshot of the launcher's 'Nightwatch VPN games' list against a loopback lobby with synthetic hosts.

  python tools/mp/vpnlobby_launcher_shot.py --out DIR [--lobby-addr 127.0.0.1:PORT]

Starts tools/vpn/lobby/nwlobby.py on loopback (test override), announces three synthetic hosts from 127.0.0.2-4
(one of another version), opens the real launcher window (settings in a scratch folder, party_vpn_lobby=1 there),
waits until its list shows them and saves a PNG of the Party tab. Prints the rows it sees (never an address).
"""
import argparse
import importlib.util
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO / "tools" / "vpn" / "lobby"))
import nwlobby as nw  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--port", type=int, default=19311)
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix="vpnlobby_shot_"))
    (scratch / "bbport.ini").write_text("party_vpn_lobby=1\n")
    os.environ.update(BB_CONFIG=str(scratch / "bbport.ini"), BB_DATA_DIR=str(scratch),
                      BB_PARTY_VPNLOBBY_ADDR=f"127.0.0.1:{a.port}")
    lobby = subprocess.Popen([sys.executable, "-I", str(REPO / "tools" / "vpn" / "lobby" / "nwlobby.py")],
                             env=dict(os.environ, NWLOBBY_BIND=f"127.0.0.1:{a.port}", NWLOBBY_NET="127.0.0.0/8"))
    try:
        time.sleep(1)
        for ip, name, slots, build in (("127.0.0.2", "Eileen", (1, 4), "0.7.7 (a1b2c3d)"),
                                       ("127.0.0.3", "Father_Gascoigne", (2, 3), "0.7.7 (a1b2c3d)"),
                                       ("127.0.0.4", "Djura", (1, 4), "0.7.5 (9f8e7d6)")):
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.bind((ip, 0))
            s.sendto(nw.encode_announce(name, 9307, slots[0], slots[1], build, True), ("127.0.0.1", a.port))
            s.close()
        spec = importlib.util.spec_from_file_location("bb_launcher_shot", REPO / "launcher.py")
        L = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(L)
        L.PORT_VERSION = "0.7.7"
        app = L.BloodborneLauncher()
        app.geometry("1500x1040+20+10")
        app.notebook.select(app.party_tab)
        app.party_vpn_lobby.set(True)
        end = time.time() + 25
        while time.time() < end:
            app.update()
            if len(app.vpn_lobby_tree.get_children()) >= 3:
                break
            time.sleep(0.1)
        for _ in range(20):
            app.update()
            time.sleep(0.05)
        rows = [app.vpn_lobby_tree.item(i, "values") for i in app.vpn_lobby_tree.get_children()]
        print("rows seen:", rows, flush=True)
        shown = " ".join(str(v) for r in rows for v in r) + app.vpn_lobby_info_var.get()
        assert "127." not in shown and "10.8." not in shown, "an address is visible in the list"
        from PIL import ImageGrab
        app.lift()
        app.update()
        t = app.vpn_lobby_tree
        # the Party tab is taller than a screen: show only the two lists (the section widgets above them are hidden)
        sec = t.master.master
        keep = (sec,)
        for w in sec.master.grid_slaves():
            if w not in keep:
                w.grid_remove()
        app.geometry("900x980+20+5")
        for _ in range(10):
            app.update()
            time.sleep(0.05)
        print("tree at", t.winfo_rootx(), t.winfo_rooty(), "window", app.winfo_rootx(), app.winfo_rooty(),
              app.winfo_width(), app.winfo_height(), "screen", app.winfo_screenwidth(), app.winfo_screenheight(), flush=True)
        x, y, w, h = app.winfo_rootx(), app.winfo_rooty(), app.winfo_width(), app.winfo_height()
        img = ImageGrab.grab(bbox=(x, y, x + w, y + h))
        img.save(out / "launcher_vpn_lobby_list.png")
        print("saved", out / "launcher_vpn_lobby_list.png", img.size, flush=True)
        app.destroy()
        return 0 if len(rows) == 3 else 1
    finally:
        lobby.terminate()


if __name__ == "__main__":
    sys.exit(main())
