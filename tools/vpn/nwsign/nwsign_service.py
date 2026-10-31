#!/usr/bin/env python3
"""nwsign HTTPS service. POST /v1/enroll, POST /v1/renew, GET /v1/health.

Privacy: logs nothing about clients. No client address is ever written to disk, to stdout or to a log
(http.server's request log and error handler, which print the address, are overridden).
"""
import json, os, socket, ssl, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nwsign_core import CAError, DAY, RateLimiter, State, load_config


class Server(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 32

    def __init__(self, addr, handler, state, ctx=None):
        super().__init__(addr, handler)
        self.state = state
        self.limiter = RateLimiter()
        self.slots = threading.BoundedSemaphore(state.cfg["max_threads"])
        if ctx:
            self.socket = ctx.wrap_socket(self.socket, server_side=True, do_handshake_on_connect=False)

    def process_request(self, request, client_address):
        # small thread cap: when full, drop the connection instead of queueing
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.slots.release()

    def handle_error(self, request, client_address):
        pass  # the default prints the client address


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    server_version = "nwsign"
    sys_version = ""

    def log_message(self, *a):
        pass

    def setup(self):
        t = self.server.state.cfg["request_timeout"]
        self.timeout = t
        super().setup()
        # hard wall-clock cap for the whole request (slowloris): drop the socket when it elapses
        self._wd = threading.Timer(t, self._kill)
        self._wd.daemon = True
        self._wd.start()

    def _kill(self):
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

    def finish(self):
        self._wd.cancel()
        try:
            super().finish()
        except OSError:
            pass

    def send_json(self, code, obj, extra=None):
        body = json.dumps(obj, separators=(",", ":")).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def limited(self, wait):
        self.send_json(429, {"error": "rate limited", "retry_after": wait}, {"Retry-After": str(wait)})

    def do_GET(self):
        if self.path == "/v1/health":
            self.send_json(200, {"ok": True})
        else:
            self.send_json(404, {"error": "not found"})

    def do_POST(self):
        s, lim, cfg = self.server.state, self.server.limiter, self.server.state.cfg
        ip = self.client_address[0]
        wait = lim.hit(lim.key("g", ip), cfg["general_per_ip_hour"], 3600)
        if wait:
            return self.limited(wait)
        if self.path not in ("/v1/enroll", "/v1/renew"):
            return self.send_json(404, {"error": "not found"})
        try:
            n = int(self.headers.get("Content-Length", ""))
        except ValueError:
            return self.send_json(411, {"error": "content-length required"})
        if n < 0 or n > cfg["max_body"]:
            return self.send_json(413, {"error": "body too large"})
        try:
            req = json.loads(self.rfile.read(n))
            if not isinstance(req, dict):
                raise ValueError
        except (ValueError, OSError):
            return self.send_json(400, {"error": "body must be a JSON object"})
        try:
            if self.path == "/v1/enroll":
                # only a successful enrollment uses up a slot: a failed one (bad CSR, server error) left a player
                # locked out for a day after three tries (2026-10-31, "Minutes left: 1436")
                ekey = lim.key("e", ip)
                wait = lim.peek(ekey, cfg["enroll_per_ip_day"], DAY) or lim.peek("global-enroll", cfg["enroll_global_day"], DAY)
                if wait:
                    return self.limited(wait)
                resp = s.enroll(req.get("csr"))
                lim.hit(ekey, cfg["enroll_per_ip_day"], DAY)
                lim.hit("global-enroll", cfg["enroll_global_day"], DAY)
            else:
                iid = req.get("install_id")
                if isinstance(iid, str):
                    wait = lim.hit(lim.key("r", ip) + iid, cfg["renew_per_install_day"], DAY)  # per caller too: an unsigned spam with a victim's id cannot use up its renewals
                    if wait:
                        return self.limited(wait)
                resp = s.renew(iid, req.get("csr"), req.get("timestamp"), req.get("signature"), req.get("cert"))
        except CAError as e:
            code = e.status if e.status in (400, 401, 403, 404) else 500
            return self.send_json(code, {"error": str(e) if code != 500 else "server error"})
        except Exception:
            return self.send_json(500, {"error": "server error"})
        self.send_json(200, resp)


def make_ssl_context(state):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(state.p("https.crt"), state.p("https.key"))
    return ctx


def crl_refresher(state, interval):
    while True:
        time.sleep(interval)
        try:
            state.regen_crl()
        except Exception as e:
            print("crl refresh failed:", e, file=sys.stderr, flush=True)


def make_server(cfg, use_tls=True):
    state = State(cfg)
    return Server((cfg["listen_host"], cfg["listen_port"]), Handler, state, make_ssl_context(state) if use_tls else None)


def main():
    cfg = load_config()
    if not cfg["vpn_host"]:
        sys.exit("vpn_host missing in config")
    state = State(cfg)
    state.regen_crl()  # fresh CRL + banned.txt at every start
    srv = make_server(cfg)
    threading.Thread(target=crl_refresher, args=(state, cfg["crl_refresh_hours"] * 3600), daemon=True).start()
    threading.Thread(target=lambda: [(time.sleep(3600), srv.limiter.prune()) for _ in iter(int, 1)], daemon=True).start()
    print("nwsign listening", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
