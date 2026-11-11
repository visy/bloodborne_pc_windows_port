#!/usr/bin/env python3
"""The two-PC session runner (bbport 0.8.1). One copy runs on each PC:

  twopc\\runner.py --role pc1        the host (pc1)         START_PC1.bat
  twopc\\runner.py --role pc2        the guest / invader    START_PC2.bat

Flags (give them on pc1 only: pc2 takes pc1's plan):  --only 1a,1c   --skip 1d,2   --list   --no-wait
The kit holds the port build (bbport\\), the game files (CUSA03173\\, configured in twopc\\config.json as a relative path),
the test saves (twopc\\fixtures\\) and this runner. Nothing is asked: press go, wait. Every game session is scripted by the
party director's test aids (BB_PARTY_DIRECTOR_TEST, as tools/mp/smoke_party.py does); the two PCs find each other by a LAN
beacon (UDP, port 47931) that carries only the session name, a few log-event names and the host's party code, and move on by
log events, never by clock. On the Nightwatch VPN (block 3) pc2 asks the VPN lobby for the host instead.

At the end: twopc_logs_<role>_<time>.zip next to this kit folder's START bat (the game's addresses are masked a.b.x.x).
Only the kit's own bbport\\user\\ folder is ever written; the player's own save folder is not touched.
"""
import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import blocks  # noqa: E402

BEACON_PORT = 47931
TOKEN = "twopc081"
STATE_FILES = ("party_state.json", "party_story.json", "party_rot.txt", "party_prologue.txt", "party_stain.txt")
VPN_PASSWORD = "twopc081-vpn"
HOST_CHAR_NAME = "visy"   # the character name of the default test save: what the VPN lobby lists the host as


def say(*a):
    text = " ".join(str(x) for x in a)
    try:
        text = mask(text)
    except Exception:
        pass
    print("[twopc %s] %s" % (time.strftime("%H:%M:%S"), text), flush=True)


def _ipmask():
    for d in (HERE / "repo" / "scripts", HERE.parent / "scripts", HERE.parent.parent / "scripts"):
        if (d / "ipmask.py").exists():
            sys.path.insert(0, str(d))
            import ipmask
            return ipmask
    return None


_IPM = _ipmask()


def mask(text):
    return _IPM.mask_ips(text) if _IPM else text


def load_config(kit):
    cfg = {"role": None, "game_dir": "CUSA03173"}
    p = HERE / "config.json"
    if p.exists():
        cfg.update(json.loads(p.read_text(encoding="utf-8")))
    return cfg


# ---- sync between the two runners -------------------------------------------------------------------------

class Sync:
    """publish(state dict) of this runner; peer() the other runner's last state (None when never heard / older than `age`)."""

    def __init__(self, role):
        self.role = role
        self.other = "pc2" if role == "pc1" else "pc1"
        self.state = {}

    def publish(self, **kw):
        self.state.update(kw)
        self._send()

    def peer(self, age=15.0):
        raise NotImplementedError

    def _send(self):
        pass

    def close(self):
        pass


class NullSync(Sync):
    def peer(self, age=15.0):
        return None


class FileSync(Sync):
    """Two runners on one PC (the dry run): a JSON file per role in a shared folder."""

    def __init__(self, role, folder):
        super().__init__(role)
        self.dir = Path(folder)
        self.dir.mkdir(parents=True, exist_ok=True)

    def _send(self):
        p = self.dir / ("%s.json" % self.role)
        tmp = p.with_suffix(".tmp")
        tmp.write_text(json.dumps(dict(self.state, k=TOKEN, role=self.role)), encoding="utf-8")
        for _ in range(5):
            try:
                os.replace(tmp, p)
                return
            except OSError:
                time.sleep(0.05)

    def peer(self, age=15.0):
        p = self.dir / ("%s.json" % self.other)
        try:
            return json.loads(p.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None


class UdpSync(Sync):
    """LAN beacon: every second this runner broadcasts its state; it keeps the other one's last message."""

    def __init__(self, role, port=BEACON_PORT):
        super().__init__(role)
        self.port, self.last, self.stop, self.lock = port, None, False, threading.Lock()
        self.rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.rx.bind(("", port))
        self.rx.settimeout(0.5)
        self.tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.tx.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.targets = ["255.255.255.255"]
        ip = lan_ipv4()
        if ip and ip.count(".") == 3:
            self.targets.append(ip.rsplit(".", 1)[0] + ".255")
        threading.Thread(target=self._listen, daemon=True).start()
        threading.Thread(target=self._beat, daemon=True).start()

    def _send(self):
        msg = json.dumps(dict(self.state, k=TOKEN, role=self.role)).encode()
        for t in self.targets:
            try:
                self.tx.sendto(msg, (t, self.port))
            except OSError:
                pass

    def _beat(self):
        while not self.stop:
            self._send()
            time.sleep(1.0)

    def _listen(self):
        while not self.stop:
            try:
                data, _addr = self.rx.recvfrom(4096)
                d = json.loads(data.decode("utf-8"))
            except (OSError, ValueError):
                continue
            if isinstance(d, dict) and d.get("k") == TOKEN and d.get("role") == self.other:
                d["_rx"] = time.time()
                with self.lock:
                    self.last = d

    def peer(self, age=15.0):
        with self.lock:
            d = self.last
        return d if d and time.time() - d["_rx"] <= age else None

    def close(self):
        self.stop = True


def lan_ipv4():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))
        return s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()


# ---- the VPN ----------------------------------------------------------------------------------------------

def own_ipv4s():
    ips = set()
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-Command",
                            "Get-NetIPAddress -AddressFamily IPv4 | ForEach-Object { $_.IPAddress }"],
                           capture_output=True, text=True, timeout=20)
        ips.update(x.strip() for x in r.stdout.split() if x.strip())
    except (OSError, subprocess.SubprocessError):
        pass
    try:
        ips.update(i[4][0] for i in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET))
    except OSError:
        pass
    return sorted(ips)


def tunnel_ip(kit):
    """This PC's Nightwatch tunnel address (10.8.0.0/22), or '' - launcher.py's own helper."""
    ln = _launcher(kit)
    if ln:
        return ln.lobby_tunnel_ip(own_ipv4s())
    import ipaddress
    net = ipaddress.ip_network("10.8.0.0/22")
    for ip in own_ipv4s():
        a = ipaddress.IPv4Address(ip)
        if a in net and a not in (net.network_address, net.network_address + 1, net.broadcast_address):
            return ip
    return ""


_LN = []


def _launcher(kit):
    if _LN:
        return _LN[0]
    try:
        import importlib.util
        spec = importlib.util.spec_from_file_location("bb_launcher", str(Path(kit) / "bbport" / "launcher.py"))
        m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(m)
        _LN.append(m)
        return m
    except Exception as e:  # tkinter missing or a launcher change: the helper is only a convenience
        say("launcher.py helper unavailable (%s); using the built-in tunnel check" % type(e).__name__)
        _LN.append(None)
        return None


def same24(a, b):
    return bool(a and b and a.rsplit(".", 1)[0] == b.rsplit(".", 1)[0])


# ---- the game process ---------------------------------------------------------------------------------------

class Game:
    """run.bat in the kit's bbport folder; its output goes, stamped with seconds since the session start, to `log`."""

    def __init__(self, cwd, env, log_path):
        self.t0 = time.time()
        self.text, self.lock = "", threading.Lock()
        self.log = open(log_path, "w", encoding="utf-8", errors="replace", newline="\n")
        self.p = subprocess.Popen(["cmd", "/c", str(Path(cwd) / "run.bat")], cwd=str(cwd), env=env, stdin=subprocess.DEVNULL,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for raw in iter(self.p.stdout.readline, b""):
            line = mask(raw.decode("utf-8", errors="replace").rstrip("\r\n"))
            out = "[+%.3f] %s\n" % (time.time() - self.t0, line)
            with self.lock:
                self.text += out
            try:
                self.log.write(out)
                self.log.flush()
            except ValueError:
                return

    def has(self, rx):
        with self.lock:
            return re.search(rx, self.text) is not None

    def alive(self):
        return self.p.poll() is None

    def stop(self, kit):
        subprocess.run(["taskkill", "/PID", str(self.p.pid), "/T", "/F"], capture_output=True)
        kill_kit_games(kit)
        try:
            self.p.wait(timeout=20)
        except subprocess.SubprocessError:
            pass
        time.sleep(0.5)
        try:
            self.log.close()
        except OSError:
            pass


def kill_kit_games(kit):
    """End every bbport.exe whose file lives under this kit (a restart loop's leftovers)."""
    pat = str((Path(kit) / "bbport").resolve()).replace("'", "''")
    ps = ("Get-CimInstance Win32_Process -Filter \"Name='bbport.exe'\" | Where-Object { $_.ExecutablePath -and "
          "$_.ExecutablePath.StartsWith('%s', [StringComparison]::OrdinalIgnoreCase) } | "
          "ForEach-Object { Stop-Process -Id $_.ProcessId -Force }" % pat)
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, timeout=60)


# ---- one session ---------------------------------------------------------------------------------------------

class Ctx:
    def __init__(self, a, kit, cfg, sync):
        self.a, self.kit, self.cfg, self.sync = a, Path(kit), cfg, sync
        self.role = a.role
        gd = Path(a.game or cfg["game_dir"])
        self.game = gd if gd.is_absolute() else (self.kit / gd)
        self.bb = self.kit / "bbport"
        self.fix = HERE / "fixtures"
        self.run_dir = self.kit / "logs" / time.strftime("run_%Y%m%d_%H%M%S")
        self.work = self.kit / "work"
        self.loopback = a.loopback
        self.slot = 0 if self.role == "pc1" else 1
        self.port = (a.port_base + self.slot) if a.loopback else None


def refuse_real_saves(ctx):
    bad = r"E:\games\bloodborne"
    for p in (ctx.bb, ctx.kit):
        if str(p.resolve()).lower().startswith(bad.lower()):
            sys.exit("the kit must not run inside the player's own game folder (%s)" % bad)


def prepare_saves(ctx, name):
    """The kit's own user\\savedata <- the fixture, a fresh savedata_party (party_saves_from in tools/mp/instances.py)."""
    user = ctx.bb / "user"
    src = ctx.fix / name / "savedata"
    if not src.is_dir():
        sys.exit("fixture %s is missing from the kit (twopc\\fixtures)" % name)
    user.mkdir(parents=True, exist_ok=True)
    for d in (user / "savedata", user / "savedata_party"):
        if d.exists():
            shutil.rmtree(d)
    shutil.copytree(src, user / "savedata")
    shutil.copytree(src, user / "savedata_party")
    for f in STATE_FILES:
        (user / f).unlink(missing_ok=True)


def game_env(ctx, s, role, code_file):
    e = {k: v for k, v in os.environ.items() if not k.startswith("BB_")}
    host = role == "pc1"
    title = "twopc081 %s %s" % (role, s["id"])
    e.update({
        "BB_GAME_DIR": str(ctx.game), "BB_FROM_LAUNCHER": "1", "BB_NO_LAUNCHER_SETTINGS": "1", "BB_FULLSCREEN": "0",
        "BB_TIMEOUT": str(s["seconds"] + 90), "BB_SHADER_PRECOMPILE": "1", "BB_MUTE_UNFOCUSED": "1",
        "BB_WINDOW_BACKGROUND": "1", "BB_CRASH_REGMEM": "1", "BB_AUDIO": "none", "BB_DLSS": "0",
        "BB_FPS": "60", "BB_FRAME_GENERATION": "off", "BB_FRAME_RATE_LOG": "10",
        "BB_PATCHES": "Skip Intro", "BB_SKIP_NETWORK_CHOICE": "online", "BB_TEST_REMO_SKIP_AFTER": "5",
        "BB_WINDOW_TITLE": title, "BB_PARTY_AUTOCONTINUE": "1", "BB_PARTY": "host" if host else "join",
        "BB_PARTY_CODE_FILE": str(code_file), "BB_PARTY_NAME": "Hunter%d" % ctx.slot, "BB_PARTY_UPNP": "0",
        "BB_PARTY_STUN": "off", "BB_PARTY_PREFER_LAN": "1",
    })
    for kv in s["env"] + (s["host_env"] if host else s["guest_env"]):
        k, _, v = kv.partition("=")
        e[k] = v
    chain = s["host"] if host else s["guest"]
    if chain:
        e["BB_PARTY_DIRECTOR_TEST"] = chain
    if s["net_sim"] and not host:
        e["BB_NET_SIM"] = s["net_sim"] + ",any=1"   # "any=1": the simulator also acts on a real (non-loopback) peer
    if host and s["pad_host"]:
        pad = ctx.work / "pad0.txt"
        pad.write_text("lx=128 ly=128\n")
        e["BB_PAD_FILE"] = str(pad)
    if s["vpn"]:
        e["BB_PARTY_PASSWORD"] = VPN_PASSWORD
        if ctx.a.vpn_lobby_test:  # dry run: a lobby on this PC (tools/vpn/lobby/nwlobby.py)
            e["BB_PARTY_VPNLOBBY_ADDR"] = ctx.a.vpn_lobby_test
    if ctx.loopback:
        e.update({"BB_MP_LOCAL_TEST": "1", "BB_MP_INSTANCE": str(ctx.slot), "BB_MP_PORT": str(ctx.port),
                  "BB_PARTY_LOOPBACK": "1", "BB_PARTY_LOCAL_IP": "127.0.0.1", "BB_PARTY_PORT": str(ctx.port),
                  "BB_WINDOW_X": str(20 + 650 * ctx.slot), "BB_WINDOW_Y": "40", "BB_WINDOW_W": "640", "BB_WINDOW_H": "380",
                  "BB_BOOT_CACHE_DIR": str(ctx.work / "bootcache"), "BB_SHADER_THREADS": "2",
                  "BB_ASYNC_PIPELINE_THREADS": "2", "BB_PREP_WORKERS": "1", "BB_VRAM_LIMIT_MB": "1792"})
        e["BB_MP_PARENT_PID"] = os.environ.get("BB_MP_PARENT_PID", "")
        if not e["BB_MP_PARENT_PID"]:
            del e["BB_MP_PARENT_PID"]
    return e


def write_ini(ctx, s, role):
    extra = s["ini_host"] if role == "pc1" else s["ini_guest"]
    (ctx.bb / "bbport.ini").write_text(blocks.INI + "".join(x + "\n" for x in extra), encoding="utf-8")


def wait_for(cond, limit, tick=0.5):
    end = time.time() + limit
    while time.time() < end:
        v = cond()
        if v:
            return v
        time.sleep(tick)
    return None


def guest_start_wait(ctx, s, idx):
    """pc2: wait for the host to be ready for this session; returns (code text or '', start reason) or None when the
    host went on without us."""
    cap = s["seconds"]
    if s["vpn"]:
        return vpn_guest_target(ctx, s, idx)

    def ready():
        p = ctx.sync.peer(40)
        if not p:
            return None
        if p.get("idx", -1) > idx or p.get("finished"):
            return "missed"
        if p.get("idx") != idx or p.get("session") != s["id"]:
            return None
        if s["start"] == "code" and p.get("code"):
            return ("code", p["code"])
        if s["start"].startswith("event:") and s["start"][6:] in (p.get("events") or []):
            return ("event", p.get("code") or "")
        return None
    r = wait_for(ready, cap)
    if r is None or r == "missed":
        return None
    return r[1], r[0]


def vpn_guest_target(ctx, s, idx=0):
    """pc2 on the VPN: the tunnel address, then the lobby's row of the host (its address:port, never shown)."""
    ln = _launcher(ctx.kit)
    own = wait_tunnel(ctx, 900)
    if not own:
        return None
    if ctx.a.vpn_lobby_test:  # dry run: a lobby on this PC
        target = (ctx.a.vpn_lobby_test.rpartition(":")[0], int(ctx.a.vpn_lobby_test.rpartition(":")[2]))
        bind = "127.0.0.5"  # the lobby hides the asker's own game: ask from another loopback address
    else:
        target, bind = ("10.8.0.1", 9310), own
    say("VPN: this PC %s; asking the lobby for the host (%s)" % (mask(own), HOST_CHAR_NAME))

    def find():
        pp = ctx.sync.peer(1e9)
        if pp and pp.get("idx") == idx and pp.get("state") == "skipped":
            return {"skipped": True}
        ent = ln.lobby_fetch(target, bind) if ln else None
        for e in ent or []:
            if e.get("name") == HOST_CHAR_NAME and e.get("password"):
                return e
        return None
    e = wait_for(find, 900, tick=4)
    if e and e.get("skipped"):
        say("VPN: pc1 has no tunnel; block 3 skipped")
        return None
    if not e:
        say("VPN: the host never showed in the lobby")
        return None
    rep = {"own": mask(own), "host": mask(e["ip"]), "same_24": same24(own, e["ip"])}
    (ctx.run_dir / "vpn_report.json").write_text(json.dumps(rep), encoding="utf-8")
    say("VPN: host at %s, this PC %s, same /24: %s" % (mask(e["ip"]), mask(own), "yes" if rep["same_24"] else "no"))
    return "%s:%d" % (e["ip"], e["port"]), "lobby"


def wait_tunnel(ctx, limit):
    t = tunnel_ip(ctx.kit) if not ctx.a.vpn_lobby_test else "127.0.0.1"
    if t:
        return t
    say("Connect the Nightwatch VPN now (block 3); waiting up to %d min for the tunnel address ..." % (limit // 60))
    return wait_for(lambda: tunnel_ip(ctx.kit), limit, tick=5)


def run_session(ctx, s, idx, total):
    role, host = ctx.role, ctx.role == "pc1"
    sd = ctx.run_dir / s["id"]
    sd.mkdir(parents=True, exist_ok=True)
    say("session %d/%d  %s  %s  (about %d min)" % (idx + 1, total, s["id"], s["title"], s["minutes"]))
    res = {"id": s["id"], "role": role, "started": time.strftime("%H:%M:%S"), "ok": False, "note": ""}
    code_file = ctx.work / "party_code.txt"
    ctx.work.mkdir(parents=True, exist_ok=True)
    code_file.unlink(missing_ok=True)
    if s["vpn"] and host:
        own = wait_tunnel(ctx, 900)
        if not own:
            res["note"] = "no VPN tunnel address (block 3 skipped)"
            ctx.sync.publish(idx=idx, session=s["id"], state="skipped", events=[], code="")
            return res
        (sd / "vpn_report_pc1.json").write_text(json.dumps({"own": mask(own)}), encoding="utf-8")
        say("VPN: this PC %s" % mask(own))
    start_reason = ""
    if not host:
        ctx.sync.publish(idx=idx, session=s["id"], state="waiting", events=[])
        got = guest_start_wait(ctx, s, idx)
        if got is None:
            res["note"] = "the host never got ready for this session (or moved on)"
            return res
        target, start_reason = got
        if s["vpn"]:
            code_file.write_text("%s\n" % target, encoding="utf-8")
        else:
            code_file.write_text(target, encoding="utf-8")
        say("host is ready (%s): starting the game" % start_reason)
    prepare_saves(ctx, s["save_host"] if host else s["save_guest"])
    write_ini(ctx, s, role)
    env = game_env(ctx, s, role, code_file)
    ctx.sync.publish(idx=idx, session=s["id"], state="boot", events=[], code="")
    game = Game(ctx.bb, env, sd / ("%s.log" % role))
    t0 = time.time()
    cap = s["seconds"] + (300 if idx == 0 else 0)   # the first game start of a kit builds its boot image
    events, code_sent, own_done_at, failed = [], "", None, ""
    until = s["host_until"] if host else s["guest_until"]
    hold = s["hold_host"] if host else s["hold_guest"]
    try:
        while time.time() - t0 < cap:
            time.sleep(0.5)
            if host:
                if not code_sent and code_file.exists():
                    txt = code_file.read_text(encoding="utf-8", errors="replace")
                    if txt.strip():
                        code_sent = txt
                        ctx.sync.publish(code=txt)
                        say("host: party code ready (sent to pc2 over the beacon)")
                for name, rx in s["signals"].items():
                    if name not in events and game.has(rx):
                        events.append(name)
                        ctx.sync.publish(events=list(events))
                        say("host event: %s" % name)
            if not game.alive():
                failed = "the game process ended (exit %s)" % game.p.returncode
                break
            pp = ctx.sync.peer(1e9)
            if pp and (pp.get("idx", -1) > idx or pp.get("finished")) and not s["vpn"]:
                failed = "the other PC had already moved on"
                ctx.sync.publish(state="done", failed=failed)
                break
            if own_done_at is None and (not until or all(game.has(u) for u in until)):
                own_done_at = time.time()
                say("%s: own markers seen; holding %d s" % (s["id"], hold))
            if own_done_at is not None and time.time() - own_done_at >= hold:
                ctx.sync.publish(state="done")
                break
            if game.has(r"the game exited with -?[1-9]\d* \(a crash\)"):
                failed = "the game crashed"
                break
        else:
            failed = "timed out after %d s" % cap
        if failed:
            say("%s: %s" % (s["id"], failed))
            ctx.sync.publish(state="done", failed=failed)
        # leave when the other PC is done too (or after the grace)
        t_done = time.time()
        while time.time() - t_done < s["grace"]:
            p = ctx.sync.peer(60)
            if p and p.get("idx") == idx and p.get("state") in ("done", "skipped"):
                break
            if p and (p.get("idx", -1) > idx or p.get("finished")):
                break
            if ctx.sync.peer(1e9) is None and time.time() - t_done > 20:
                break  # no beacon at all (VPN): the grace is not worth waiting
            time.sleep(1)
    finally:
        game.stop(ctx.kit)
    res.update(ok=not failed, note=failed or "done", seconds=int(time.time() - t0))
    last = ctx.bb / "logs" / "last_session.log"
    if last.exists():
        try:
            (sd / ("%s_last_session.log" % role)).write_text(mask(last.read_text(encoding="utf-8", errors="replace")), encoding="utf-8")
        except OSError:
            pass
    return res


# ---- the end: logs, net_doctor, session_score, one zip --------------------------------------------------------

def tool_py(rel):
    for base in (HERE / "repo", HERE.parent, HERE.parent.parent):
        p = base / rel
        if p.exists():
            return p
    return None


def run_tools(ctx, plan_ids):
    rd = ctx.run_dir
    nd, sc = tool_py("scripts/net_doctor.py"), tool_py("tools/party/session_score.py")
    for sid in plan_ids:
        d = rd / sid
        logs = [str(p) for p in sorted(d.glob("%s.log" % ctx.role))]
        if not logs:
            continue
        if nd:
            r = subprocess.run([sys.executable, "-I", str(nd), "--redactips"] + logs, capture_output=True, text=True, timeout=120)
            (d / ("netdoctor_%s.txt" % ctx.role)).write_text(mask(r.stdout + r.stderr), encoding="utf-8")
        if sc:
            sdir = rd / "_score" / sid
            sdir.mkdir(parents=True, exist_ok=True)
            for lg in logs:
                shutil.copy2(lg, sdir / Path(lg).name)
            r = subprocess.run([sys.executable, "-I", str(sc), str(sdir), "--md", str(d / ("session_score_%s.md" % ctx.role))],
                               capture_output=True, text=True, timeout=180)
            (d / ("session_score_%s.txt" % ctx.role)).write_text(mask(r.stdout + r.stderr), encoding="utf-8")
    shutil.rmtree(rd / "_score", ignore_errors=True)


def make_zip(ctx, results, plan_ids):
    stamp = time.strftime("%Y%m%d_%H%M%S")
    meta = {"role": ctx.role, "plan": plan_ids, "results": results, "version": "twopc081",
            "stamp": stamp, "loopback": bool(ctx.loopback)}
    (ctx.run_dir / "meta.json").write_text(json.dumps(meta, indent=1), encoding="utf-8")
    out = ctx.kit / ("twopc_logs_%s_%s.zip" % (ctx.role, stamp))
    if ctx.a.out_dir:
        out = Path(ctx.a.out_dir) / out.name
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for p in sorted(ctx.run_dir.rglob("*")):
            if p.is_file():
                if p.suffix.lower() in (".log", ".txt", ".md", ".json"):  # one more mask pass over every text
                    z.writestr("twopc_logs_%s/%s" % (ctx.role, p.relative_to(ctx.run_dir).as_posix()),
                               mask(p.read_text(encoding="utf-8", errors="replace")))
                else:
                    z.write(p, "twopc_logs_%s/%s" % (ctx.role, p.relative_to(ctx.run_dir).as_posix()))
    return out


# ---- main ------------------------------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--role", choices=["pc1", "pc2"])
    ap.add_argument("--only", default="")
    ap.add_argument("--skip", default="")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--no-wait", action="store_true", help="pc1: do not wait for pc2's hello")
    ap.add_argument("--kit", default=str(HERE.parent))
    ap.add_argument("--game", default="", help="game folder (default: config.json, relative to the kit)")
    ap.add_argument("--out-dir", default="")
    ap.add_argument("--loopback", action="store_true", help="dry run: two runners on one PC, files for the beacon, loopback party")
    ap.add_argument("--sync-dir", default="")
    ap.add_argument("--port-base", type=int, default=47680)
    ap.add_argument("--vpn-lobby-test", default="", help="dry run: ip:port of a local lobby instead of the VPN's")
    a = ap.parse_args()
    cfg = load_config(a.kit)
    a.role = a.role or cfg.get("role")
    allp = blocks.sessions()
    only = [x for x in a.only.split(",") if x]
    skip = [x for x in a.skip.split(",") if x]
    plan = blocks.select(allp, only, skip)
    if a.list:
        for s in allp:
            print("%-8s %2d min  %s%s" % (s["id"], s["minutes"], s["title"], "" if s in plan else "   (skipped)"))
        print("1b       (no game: the load times are read from the 1a.travel logs)")
        return
    if not a.role:
        sys.exit("--role pc1|pc2 needed")
    kit = Path(a.kit).resolve()
    if a.loopback:
        sync = FileSync(a.role, a.sync_dir) if a.sync_dir else NullSync(a.role)
    else:
        sync = UdpSync(a.role)
    ctx = Ctx(a, kit, cfg, sync)
    refuse_real_saves(ctx)
    if not (ctx.game / "eboot.bin").exists():
        sys.exit("no eboot.bin in the kit's game folder %s (re-extract the zip)" % ctx.game)
    if not (ctx.bb / "run.bat").exists():
        sys.exit("bbport\\run.bat missing from the kit")
    ctx.run_dir.mkdir(parents=True, exist_ok=True)
    (ctx.work).mkdir(exist_ok=True)
    if sys.platform == "win32":
        import ctypes
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
    say("%s: %d session(s), about %d min" % (a.role, len(plan), sum(s["minutes"] for s in plan)))
    kill_kit_games(kit)
    # hello: pc1 publishes the plan, pc2 takes it
    if a.role == "pc1":
        sync.publish(hello=True, plan=[s["id"] for s in plan], idx=-1, session="", state="hello")
        if not a.no_wait:
            say("waiting for pc2 (start START_PC2.bat there) ...")
            if not wait_for(lambda: sync.peer(30), 1800, tick=1):
                sync.close()
                sys.exit("pc2 never answered (same network? Windows Firewall: allow the kit's python on Private networks)")
            say("pc2 is there")
    else:
        sync.publish(hello=True, idx=-1, session="", state="hello")
        say("waiting for pc1 ...")
        p = wait_for(lambda: (sync.peer(30) or {}).get("plan"), 1800, tick=1)
        if not p:
            sync.close()
            sys.exit("pc1 never answered (same network? Windows Firewall: allow the kit's python on Private networks)")
        if only or skip:
            say("flags are taken from pc1; ignoring --only/--skip here")
        by_id = {s["id"]: s for s in allp}
        plan = [by_id[i] for i in p if i in by_id]
        say("plan from pc1: %s" % " ".join(s["id"] for s in plan))
    results = []
    for idx, s in enumerate(plan):
        try:
            results.append(run_session(ctx, s, idx, len(plan)))
        except KeyboardInterrupt:
            results.append({"id": s["id"], "role": a.role, "ok": False, "note": "interrupted"})
            break
        except Exception as e:  # a session's own failure must not lose the rest or the logs
            kill_kit_games(kit)
            results.append({"id": s["id"], "role": a.role, "ok": False, "note": "runner error: %s" % e})
            say("session %s: runner error: %s" % (s["id"], e))
    sync.publish(finished=True, state="finished", idx=len(plan))
    run_tools(ctx, [r["id"] for r in results])
    out = make_zip(ctx, results, [s["id"] for s in plan])
    time.sleep(2)
    sync.close()
    say("done: %s" % out)
    say("send this zip back (pc2 sends its own); verdict: python twopc\\verdict.py <pc1 zip> <pc2 zip>")


if __name__ == "__main__":
    main()
