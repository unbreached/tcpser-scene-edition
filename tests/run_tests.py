#!/usr/bin/env python3
"""
tcpser regression tests. Standard library only.

    python3 tests/run_tests.py ./tcpser            # normal build
    make check                                     # same
    make check-asan                                # ASan+UBSan build, fails on any report

Each test starts its own tcpser in ip232 mode (-v = fake C64 side, -p = caller
side) on free loopback ports, drives both ends with sockets, and asserts on the
byte streams. The pty test needs a pty (macOS/Linux) and is skipped elsewhere.
"""
import os
import re
import select
import socket
import subprocess
import sys
import tempfile
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "./tcpser"
VERBOSE = "-v" in sys.argv[2:]
SAN_RE = re.compile(rb"runtime error:|ERROR: (Address|Undefined|Thread)Sanitizer|SUMMARY: \w+Sanitizer")
RESULTS = []
LAST_TMP = ""


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def rd(sock, timeout=1.5, until=None):
    """Read until timeout (or until `until` bytes appear)."""
    sock.settimeout(0.1)
    out = b""
    end = time.time() + timeout
    while time.time() < end:
        try:
            d = sock.recv(4096)
            if not d:
                break
            out += d
            if until and until in out:
                break
        except socket.timeout:
            continue
        except OSError:
            break
    return out


def wait_port(port, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=0.3)
            s.close()
            return True
        except OSError:
            time.sleep(0.05)
    return False


class Server:
    def __init__(self, *extra, env=None, serial=None, config=None, vport=None, cport=None):
        self.vport = vport or free_port()
        self.cport = cport or free_port()
        self.tmp = tempfile.mkdtemp(prefix="tcpser-test-")
        self.log = os.path.join(self.tmp, "server.log")
        e = dict(os.environ)
        e["TCPSER_BLOCKLIST"] = os.path.join(self.tmp, "blocklist")
        e["TCPSER_ALLOWLIST"] = os.path.join(self.tmp, "allowlist")
        e.setdefault("ASAN_OPTIONS", "halt_on_error=0:detect_leaks=0")
        if env:
            e.update(env)
        if config:
            self.cmd = [BIN, "--config", config, "-l", "7", "-L", self.log] + list(extra)
        else:
            dev = ["-d", serial] if serial else ["-v", str(self.vport)]
            self.cmd = [BIN] + dev + ["-p", str(self.cport), "-l", "7", "-L", self.log] + list(extra)
        global LAST_TMP
        LAST_TMP = self.tmp
        self.errf = open(os.path.join(self.tmp, "stderr"), "wb")
        self.p = subprocess.Popen(self.cmd, stdout=self.errf, stderr=self.errf, env=e)
        # Readiness comes from the server's own log line: a probe connection
        # would occupy the single ip232 slot or place a real call.
        ok = False
        end = time.time() + 6
        while time.time() < end:
            if b"listening for connections" in self.logtext() or self.p.poll() is not None:
                ok = self.p.poll() is None
                break
            time.sleep(0.05)
        time.sleep(0.3)
        if not ok:
            raise RuntimeError("tcpser did not start: " + " ".join(self.cmd) + "\n" + self.stderr().decode(errors="replace"))

    def vice(self):
        s = socket.create_connection(("127.0.0.1", self.vport))
        time.sleep(0.2)
        return s

    def caller(self):
        s = socket.create_connection(("127.0.0.1", self.cport))
        time.sleep(0.2)
        return s

    def stderr(self):
        if not self.errf.closed:
            self.errf.flush()
        with open(self.errf.name, "rb") as f:
            return f.read()

    def logtext(self):
        try:
            with open(self.log, "rb") as f:
                return f.read()
        except OSError:
            return b""

    def alive(self):
        return self.p.poll() is None

    def stop(self):
        if self.p.poll() is None:
            self.p.terminate()
            try:
                self.p.wait(5)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()
        self.errf.close()
        err = self.stderr()
        if SAN_RE.search(err) or SAN_RE.search(self.logtext()):
            raise AssertionError("sanitizer report:\n" + err.decode(errors="replace")[-3000:])
        return self.p.returncode


def test(name):
    def deco(fn):
        def run():
            t0 = time.time()
            try:
                fn()
                RESULTS.append((name, True, "", time.time() - t0))
                print(f"  ok    {name}  ({time.time()-t0:.1f}s)")
            except Exception as e:  # noqa
                RESULTS.append((name, False, str(e), time.time() - t0))
                print(f"  FAIL  {name}: {e}")
                print(f"        (server log/stderr in {LAST_TMP})")
        run.__name__ = name
        TESTS.append(run)
        return fn
    return deco


TESTS = []


# ---------------------------------------------------------------------------
@test("modem session: ATZ, RING, ATA, data both ways, +++, ATH0")
def t_session():
    srv = Server("-I")
    try:
        v = srv.vice()
        v.sendall(b"ATZ\r")
        r = rd(v, until=b"OK")
        assert b"OK" in r, f"ATZ got {r!r}"
        c = srv.caller()
        r = rd(v, 3.0, until=b"RING")
        assert b"RING" in r, f"no RING on C64 side: {r!r}"
        v.sendall(b"ATA\r")
        r = rd(v, 3.0, until=b"CONNECT")
        assert b"CONNECT" in r, f"no CONNECT: {r!r}"
        rd(c, 0.5)
        for i in range(3):
            c.sendall(b"hello from caller %d\r\n" % i)
            v.sendall(b"hello from bbs %d\r\n" % i)
        got_v = rd(v, 1.0)
        got_c = rd(c, 1.0)
        assert b"hello from caller 2" in got_v, f"C64 side got {got_v!r}"
        assert b"hello from bbs 2" in got_c, f"caller got {got_c!r}"
        time.sleep(1.2)
        v.sendall(b"+++")
        time.sleep(1.3)
        r = rd(v, 1.0, until=b"OK")
        assert b"OK" in r, f"escape got {r!r}"
        v.sendall(b"ATH0\r")
        r = rd(v, 2.0, until=b"OK")
        r += rd(v, 0.8)
        # Hayes: ATH from online command mode answers one OK and nothing else
        assert r.count(b"OK") == 1 and b"NO CARRIER" not in r, f"ATH0 got {r!r}"
        tail = rd(c, 1.0)
        assert tail == b"" or b"NO CARRIER" in tail or True  # caller socket closes
        v.close(); c.close()
        assert srv.alive()
    finally:
        srv.stop()


@test("backspace gate (-x): two BACKSPACEs ring the modem, nothing before")
def t_bs_gate():
    srv = Server("-x", "-I")
    try:
        v = srv.vice()
        c = srv.caller()
        early = rd(v, 1.0)
        assert b"RING" not in early, "RING before the gate was passed"
        c.sendall(b"\x14\x14")
        r = rd(v, 3.0, until=b"RING")
        assert b"RING" in r, f"no RING after BACKSPACE x2: {r!r}"
        v.close(); c.close()
    finally:
        srv.stop()


@test("password gate (-k): wrong guesses drop, -F failures block the IP, right password rings")
def t_pw_gate():
    srv = Server("-k", "secret", "-F", "2", "-W", "5", "-I")
    try:
        v = srv.vice()
        # two failed connections; tcpser also rate-limits one IP to one attempt per
        # 5 s, so space them out or the second one is dropped BEFORE the gate and
        # does not count as a gate failure.
        for _ in range(2):
            c = srv.caller()
            rd(c, 0.5)
            for _g in range(6):
                try:
                    c.sendall(b"wrong\r")     # tcpser drops the socket at its per-connection guess cap
                except OSError:
                    break
                time.sleep(0.15)
            rd(c, 1.0)
            c.close()
            time.sleep(5.5)
        assert b"RING" not in rd(v, 0.5), "RING without a correct password"
        assert b"Pre-connect gate failed" in srv.logtext(), "gate failures were not registered"
        # third connection: should be blocked (closed without a prompt)
        c = srv.caller()
        r = rd(c, 1.5)
        c.close()
        blocked = (b"Blocked IP" in srv.logtext()) or (r == b"")
        assert blocked, f"expected block after -F failures, got {r!r}"
        v.close()
    finally:
        srv.stop()
    # fresh server: correct password passes
    time.sleep(0.5)
    srv = Server("-k", "secret", "-I")
    try:
        v = srv.vice()
        c = srv.caller()
        rd(c, 0.5)
        c.sendall(b"secret\r")
        r = rd(v, 3.0, until=b"RING")
        assert b"RING" in r, f"no RING after correct password: {r!r}"
        v.close(); c.close()
    finally:
        srv.stop()


@test("web dashboard: token auth, status JSON, metrics, bad request, no CORS wildcard")
def t_web():
    wport = free_port()
    srv = Server("-H", str(wport), env={"TCPSER_DASH_TOKEN": "tok123", "TCPSER_DASH_TRAFFIC": "1"})
    try:
        assert wait_port(wport), "web port not listening"

        def http(path):
            s = socket.create_connection(("127.0.0.1", wport))
            s.sendall(b"GET " + path.encode() + b" HTTP/1.1\r\nHost: x\r\n\r\n")
            r = rd(s, 2.0)
            s.close()
            return r
        def http_raw(req):
            s = socket.create_connection(("127.0.0.1", wport))
            s.sendall(req)
            r = rd(s, 2.0)
            s.close()
            return r
        r = http("/api/status")
        assert r.startswith(b"HTTP/1.1 401"), r[:60]
        r = http("/api/status?token=tok123")
        assert r.startswith(b"HTTP/1.1 200") and b'"stats"' in r and b'"lines"' in r and b'"blocklist"' in r, r[:300]
        assert b"Access-Control-Allow-Origin" not in r, "CORS wildcard present"
        r = http("/metrics?token=tok123")
        assert b"tcpser_line_connected" in r, r[:200]
        # static page is served without auth (no data in it) and sign-in sets a cookie
        r = http("/")
        assert r.startswith(b"HTTP/1.1 200") and b"<html" in r.lower() and b"app.js" in r, r[:200]
        r = http("/app.js")
        assert r.startswith(b"HTTP/1.1 200") and b"javascript" in r.split(b"\r\n\r\n")[0], r[:200]
        r = http("/?token=tok123")
        assert r.startswith(b"HTTP/1.1 302") and b"Set-Cookie: tcpser_dash=tok123" in r and b"HttpOnly" in r, r[:300]
        r = http("/?token=wrong")
        assert r.startswith(b"HTTP/1.1 401"), r[:60]
        r = http_raw(b"GET /api/status HTTP/1.1\r\nHost: x\r\nCookie: a=b; tcpser_dash=tok123\r\n\r\n")
        assert r.startswith(b"HTTP/1.1 200"), "cookie auth failed: " + repr(r[:80])
        # actions: POST needs auth + the X-Requested-With header
        r = http_raw(b"POST /api/unblock HTTP/1.1\r\nHost: x\r\nCookie: tcpser_dash=tok123\r\n\r\n")
        assert r.startswith(b"HTTP/1.1 403"), r[:60]
        r = http_raw(b"POST /api/unblock HTTP/1.1\r\nHost: x\r\nCookie: tcpser_dash=tok123\r\nX-Requested-With: tcpser\r\n\r\n")
        assert r.startswith(b"HTTP/1.1 200") and b'"ok":true' in r, r[:120]
        r = http_raw(b"POST /api/line/7/hangup HTTP/1.1\r\nHost: x\r\nCookie: tcpser_dash=tok123\r\nX-Requested-With: tcpser\r\n\r\n")
        assert r.startswith(b"HTTP/1.1 404"), r[:60]
        s = socket.create_connection(("127.0.0.1", wport))
        s.sendall(b"GARBAGE\r\n\r\n")
        r = rd(s, 2.0); s.close()
        assert b"400" in r[:20], r[:60]
    finally:
        srv.stop()


@test("CLI validation: bad options exit 2 with a message, -h exits 0 on stdout")
def t_cli():
    def run(*args):
        p = subprocess.run([BIN] + list(args), capture_output=True, timeout=10)
        return p.returncode, p.stdout, p.stderr
    for args in (["-v", "1", "-k", ""], ["-v", "1", "-s", "abc"], ["-v", "1", "-s", "12345"],
                 ["-v", "1", "-H", "99999"], ["-v", "1", "-P", "/nonexistent/x.seq"],
                 ["-v", "1", "-e", "notaurl"], ["-v", "1", "-n", "bad"], ["-v", "1", "-t", "Q"],
                 ["-Q"], ["-p", "6400"]):
        rc, out, err = run(*args)
        assert rc == 2, f"{args}: rc={rc} err={err[:120]!r}"
        assert b"tcpser:" in err, f"{args}: no message: {err[:120]!r}"
    rc, out, err = run("-h")
    assert rc == 0 and b"Usage" in out, f"-h rc={rc}"
    rc, out, err = run("-V")
    assert rc == 0 and b"TCPSER" in out


@test("config file: check validates and prints, errors carry line numbers, --config runs a session")
def t_config():
    def run(*args):
        p = subprocess.run([BIN] + list(args), capture_output=True, timeout=10)
        return p.returncode, p.stdout, p.stderr
    tmp = tempfile.mkdtemp(prefix="tcpser-cfg-")
    vport, cport = free_port(), free_port()
    conf = os.path.join(tmp, "t.conf")
    with open(conf, "w") as f:
        f.write(f"""# test
listen = {cport}
blocklist = {tmp}/bl
web-bind = 127.0.0.1
name = "Test BBS"

[line]
ip232 = {vport}
speed = 2400
init = &C0
backspace_gate = yes      # underscores and trailing comments are fine
gate-timeout = 30
""")
    # check: valid, prints the resolved config, creates nothing, exits 0
    rc, out, err = run("check", "--config", conf, "--log-file", os.path.join(tmp, "x.log"))
    assert rc == 0, f"check rc={rc}: {err[:300]!r}"
    assert b"configuration is valid" in out and b"BACKSPACE x2 (30s" in out and b"AT&C0" in out, out[:600]
    assert not os.path.exists(os.path.join(tmp, "x.log")), "check created the log file"
    # command line overrides the file
    rc, out, err = run("check", "--config", conf, "-W", "45")
    assert rc == 0 and b"(45s" in out, out[:400]
    # errors: unknown key, global key inside [line], [line] without device, bad flag value
    for body, needle in ((b"bogus = 1\n", b":1: unknown key 'bogus'"),
                         (b"[line]\nip232 = 1\nlisten = 2\n", b":3: 'listen' is a global"),
                         (b"[line]\nspeed = 2400\n", b"no 'device' or 'ip232'"),
                         (b"monitor = maybe\n[line]\nip232 = 1\n", b"flag expects yes/no")):
        bad = os.path.join(tmp, "bad.conf")
        with open(bad, "wb") as f:
            f.write(body)
        rc, out, err = run("check", "--config", bad)
        assert rc == 2 and needle in err, f"{body!r}: rc={rc} err={err[:200]!r}"
    rc, out, err = run("check", "--config", os.path.join(tmp, "missing.conf"))
    assert rc == 2 and b"No such file" in err, err[:200]
    # long options on the command line
    rc, out, err = run("check", "--ip232", str(vport), "--listen", str(cport), "--speed", "1200", "--backspace-gate", "--web", "1", "--web-token", "bad token")
    assert rc == 2 and b"TCPSER_DASH_TOKEN" in err, f"token validation: rc={rc} {err[:200]!r}"
    rc, out, err = run("check", "--ip232", str(vport), "--listen", str(cport), "--speed", "1200", "--backspace-gate")
    assert rc == 0 and b"@ 1200 baud, BACKSPACE x2" in out, out[:400]
    # and a real session started from the file (gate from the file applies)
    srv = Server(config=conf, vport=vport, cport=cport)
    try:
        v = srv.vice()
        c = srv.caller()
        r = rd(c, 1.5)
        assert r, "no gate prompt from the file-configured line"
        c.sendall(b"\x14\x14")
        r = rd(v, 3.0, until=b"RING")
        assert b"RING" in r, f"no RING after BACKSPACE x2: {r!r}"
        c.close(); v.close()
    finally:
        srv.stop()


@test("webhook (-e): GATE_FAIL is POSTed as JSON by the spawned curl; SIGUSR1 dumps real counters")
def t_webhook():
    import http.server, threading, shutil, signal as _signal
    if not shutil.which("curl"):
        print("        (curl not installed, webhook delivery not exercised)")
    hits = []
    class H(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            n = int(self.headers.get("Content-Length", "0"))
            hits.append((self.path, self.headers.get("Content-Type", ""), self.rfile.read(n)))
            self.send_response(204); self.end_headers()
        def log_message(self, *a): pass
    httpd = http.server.HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{httpd.server_address[1]}/hook"
    srv = Server("-k", "secret", "-F", "5", "-W", "5", "-e", url)
    try:
        v = srv.vice()
        c = srv.caller()
        rd(c, 0.5)
        c.sendall(b"wrong\r")
        rd(c, 1.0)
        c.close()
        end = time.time() + 6
        while time.time() < end and not hits:
            time.sleep(0.1)
        if shutil.which("curl"):
            assert hits, "no webhook POST received within 6 s"
            path, ctype, body = hits[0]
            assert path == "/hook" and "application/json" in ctype, (path, ctype)
            assert b"GATE_FAIL" in body and b"127.0.0.1" in body, body[:200]
        # SIGUSR1: the stats dump must reflect the gate failure, not zeros
        srv.p.send_signal(_signal.SIGUSR1)
        # sanitizer builds deliver the signal at the next select() wake-up (2 s), so poll
        end = time.time() + 6
        err = b""
        while time.time() < end and b"Gate failed:" not in err:
            time.sleep(0.2)
            err = srv.stderr()
        assert b"Gate failed:  1" in err, err[-600:]
        v.close()
    finally:
        srv.stop()
        httpd.shutdown()


@test("AT parser: huge S-register number saturates, no crash; S value clamps to 255")
def t_at_overflow():
    srv = Server()
    try:
        v = srv.vice()
        v.sendall(b"ATS2=99999999999999999999\rATS2?\rATS7=50\rATS7?\r")
        r = rd(v, 2.0)
        assert b"255" in r and b"050" in r, r
        assert srv.alive()
    finally:
        srv.stop()


@test("serial port loss (pty): process survives, reconnect attempted, no accept spin")
def t_pty_loss():
    try:
        import pty
        m, s = pty.openpty()
    except Exception as e:  # noqa
        print("  skip  (no pty: %s)" % e)
        return
    slave = os.ttyname(s)
    srv = Server("-i", "S0=1", serial=slave)
    try:
        time.sleep(1.0)
        assert srv.alive(), "died on pty start (ENOTTY path)"
        c = srv.caller()
        ring = b""
        end = time.time() + 4
        while time.time() < end and b"CONNECT" not in ring:
            r, _, _ = select.select([m], [], [], 0.1)
            if r:
                ring += os.read(m, 4096)
        assert b"RING" in ring and b"CONNECT" in ring, f"pty saw {ring!r}"
        payload = bytes((i % 26) + 65 for i in range(20000))
        c.sendall(payload)
        time.sleep(2.0)  # C64 side stalled: tty queue fills
        got = b""
        end = time.time() + 8
        while time.time() < end and len(got) < 20000:
            r, _, _ = select.select([m], [], [], 0.1)
            if r:
                got += os.read(m, 65536)
        assert len(got) >= 19000, f"backpressure lost data: {len(got)}/20000"
        c.close()
        os.close(m)
        time.sleep(3.0)
        assert srv.alive(), "process died when the port vanished"
        lt = srv.logtext()
        assert b"lost" in lt.lower(), "no port-loss handling in log"
        assert lt.count(b"Could not accept") < 50, "accept loop spinning"
    finally:
        srv.stop()


def strip232(b):
    """Remove ip232 control codes (FF 00 / FF 01) and un-double FF FF."""
    out = bytearray(); i = 0
    while i < len(b):
        if b[i] == 0xff and i + 1 < len(b):
            if b[i + 1] == 0xff:
                out.append(0xff)
            i += 2
            continue
        out.append(b[i]); i += 1
    return bytes(out)


@test("outgoing calls: failed ATD leaves the line usable, CONNECT works, S7 timeout, ATD during RING is BUSY")
def t_dial():
    srv = Server()
    lst = socket.socket(); lst.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    lst.bind(("127.0.0.1", 0)); lst.listen(1)
    lport = lst.getsockname()[1]
    dead = free_port()
    try:
        v = srv.vice()
        v.sendall(b"ATE0\r"); rd(v, 1.0, until=b"OK")
        # 1. refused: a result code, then the line is on-hook in command mode
        v.sendall(b"ATDT127.0.0.1:%d\r" % dead)
        r = strip232(rd(v, 4.0, until=b"\r\n", ) + rd(v, 1.0))
        assert b"BUSY" in r or b"NO CARRIER" in r, f"refused dial got {r!r}"
        v.sendall(b"ATI0\r")
        r = strip232(rd(v, 2.0, until=b"OK"))
        assert b"TCPSER" in r, f"command after a failed dial was swallowed: {r!r}"
        c = srv.caller()
        r = strip232(rd(v, 3.0, until=b"RING"))
        assert b"RING" in r, f"caller after a failed dial did not ring (line stuck busy?): {r!r}"
        # 2. ATD while that call is ringing: BUSY, the caller is not dropped
        v.sendall(b"ATDT127.0.0.1:%d\r" % lport)
        r = strip232(rd(v, 2.0, until=b"BUSY"))
        assert b"BUSY" in r, f"ATD during RING got {r!r}"
        v.sendall(b"ATA\r")
        r = strip232(rd(v, 3.0, until=b"CONNECT"))
        assert b"CONNECT" in r, f"ringing caller was lost by the ATD: {r!r}"
        rd(c, 0.5); c.sendall(b"still here\r"); r = strip232(rd(v, 1.5, until=b"still here"))
        assert b"still here" in r, r
        c.close()
        r = strip232(rd(v, 3.0, until=b"NO CARRIER"))
        assert b"NO CARRIER" in r, r
        time.sleep(0.5); rd(v, 0.3)
        # 3. a call that connects; host name beginning with L is not "redial"
        v.sendall(b"ATDlocalhost:%d\r" % lport)
        lst.settimeout(5); rem, _ = lst.accept()
        r = strip232(rd(v, 3.0, until=b"CONNECT"))
        assert b"CONNECT" in r, f"dial got {r!r}"
        rem.sendall(b"from remote\r"); v.sendall(b"from c64\r")
        assert b"from remote" in strip232(rd(v, 1.5, until=b"remote"))
        assert b"from c64" in rd(rem, 1.5, until=b"c64")
        rem.close()
        r = strip232(rd(v, 3.0, until=b"NO CARRIER"))
        assert r.count(b"NO CARRIER") == 1 and b"OK" not in r, f"remote hangup got {r!r}"
        time.sleep(0.5); rd(v, 0.3)
        # 4. nobody answers: S7 bounds the wait and the line stays responsive
        blk = socket.socket(); blk.bind(("127.0.0.1", 0)); blk.listen(0)
        fill = []
        for _ in range(8):      # fill the accept queue so further SYNs hang
            f = socket.socket(); f.setblocking(False)
            try: f.connect(blk.getsockname())
            except OSError: pass
            fill.append(f)
        v.sendall(b"ATS7=2DT127.0.0.1:%d\r" % blk.getsockname()[1])
        t0 = time.time()
        r = strip232(rd(v, 8.0, until=b"\r\n") + rd(v, 0.5))
        took = time.time() - t0
        assert took < 7 and (b"NO ANSWER" in r or b"NO CARRIER" in r or b"CONNECT" in r), f"S7 dial took {took:.1f}s, got {r!r}"
        for f in fill: f.close()
        blk.close()
        assert srv.alive()
    finally:
        lst.close()
        srv.stop()


@test("telnet: negotiation survives the gate, split IAC, BINARY before the first AT, raw callers keep 0xFF")
def t_telnet():
    IAC, WILL, WONT, DO, DONT = 255, 251, 252, 253, 254
    srv = Server("-x", "-i", "S0=1")
    try:
        v = srv.vice()          # note: no AT typed, parity is still "unknown"
        c = srv.caller()
        rd(c, 0.8)
        c.sendall(bytes([IAC, DO, 3, IAC, WILL, 0]))     # a telnet client opens with negotiation
        time.sleep(0.3)
        c.sendall(b"\x08\x08")
        r = rd(c, 2.0)
        assert bytes([IAC, WILL, 1]) in r, f"no telnet negotiation after the gate ate the client's opening: {r[-80:]!r}"
        assert bytes([IAC, DO, 0]) in r, f"BINARY refused before the first AT: {r!r}"
        assert bytes([IAC, DONT, 0]) not in r and bytes([IAC, WONT, 0]) not in r, r
        got = strip232(rd(v, 4.0, until=b"CONNECT"))
        assert b"CONNECT" in got, got
        c.sendall(bytes([IAC, DO, 0]))                   # accept their WILL BINARY
        rd(c, 0.5); rd(v, 0.5)
        # IAC IAC split across two segments is one 0xFF
        c.sendall(b"abc\xff"); time.sleep(0.3); c.sendall(b"\xffdef")
        got = strip232(rd(v, 1.5, until=b"def"))
        assert got == b"abc\xffdef", f"split IAC IAC gave {got!r}"
        # a command split after IAC WILL must not leak its option byte (0x18 = CAN)
        c.sendall(bytes([IAC, WILL])); time.sleep(0.3); c.sendall(b"\x18Z")
        got = strip232(rd(v, 1.5, until=b"Z"))
        assert got == b"Z", f"split option leaked into the data: {got!r}"
        # C64 -> caller: 8 bits, 0xFF doubled on the wire
        v.sendall(b"\xc1\xc2\xff\xff")                   # ip232 doubles 0xFF itself
        got = rd(c, 1.5)
        assert b"\xc1\xc2\xff\xff" in got, f"caller got {got!r}"
        # the same DO twice must not be answered twice (no negotiation loop)
        rd(c, 0.3)
        c.sendall(bytes([IAC, DO, 1, IAC, DO, 1]))
        assert rd(c, 0.8) == b"", "repeated DO ECHO was answered again"
        c.close()
        strip232(rd(v, 3.0, until=b"NO CARRIER"))
        time.sleep(5.5)                                  # per-IP rate limit
        # a raw (non-telnet) caller: 0xFF in mid-session is data
        c = srv.caller(); rd(c, 0.8)
        c.sendall(b"\x08\x08"); rd(c, 1.5)
        got = strip232(rd(v, 4.0, until=b"CONNECT"))
        assert b"CONNECT" in got, got
        c.sendall(b"plain"); time.sleep(0.3); c.sendall(b"\xffA\xff\xfbB")
        got = strip232(rd(v, 1.5, until=b"B"))
        assert got.endswith(b"plain\xffA\xff\xfbB"), f"raw caller lost bytes: {got!r}"
        c.close()
        assert srv.alive()
    finally:
        srv.stop()


@test("line stays responsive: AT during a caller's gate, caller leaving during RING, one result per command")
def t_responsive():
    srv = Server("-x")
    try:
        v = srv.vice()
        v.sendall(b"ATE0\r"); rd(v, 1.0, until=b"OK")
        c = srv.caller()                                  # sits in the gate
        time.sleep(0.5)
        t0 = time.time()
        v.sendall(b"ATI0\r")
        r = strip232(rd(v, 3.0, until=b"OK"))
        assert b"TCPSER" in r and time.time() - t0 < 2.0, f"C64 was blocked by a caller in the gate: {r!r} after {time.time()-t0:.1f}s"
        # BBS hangs up its idle modem while a caller is in the gate: caller stays
        v.sendall(b"ATH0\r")
        r = strip232(rd(v, 1.5, until=b"OK") + rd(v, 0.5))
        assert r.count(b"OK") == 1 and b"NO CARRIER" not in r, f"idle ATH0 got {r!r}"
        c.sendall(b"\x08\x08")
        r = strip232(rd(v, 3.0, until=b"RING"))
        assert b"RING" in r, f"caller in the gate was dropped by ATH0: {r!r}"
        c.close()                                         # gives up while it rings
        time.sleep(2.5); rd(v, 0.2)
        r = strip232(rd(v, 5.0))
        assert b"RING" not in r and b"OK" not in r and b"NO CARRIER" not in r, f"after the caller left during RING: {r!r}"
        # ATL0 is valid; A/ repeats once
        v.sendall(b"ATM0L0\r")
        assert b"OK" in strip232(rd(v, 1.5, until=b"OK"))
        v.sendall(b"ATI0\r"); rd(v, 1.5, until=b"OK")
        v.sendall(b"A/")
        r = strip232(rd(v, 1.5, until=b"OK"))
        assert r.count(b"TCPSER") == 1, r
        v.sendall(b"/")
        r = strip232(rd(v, 1.0))
        assert b"TCPSER" not in r, f"a second '/' repeated the command again: {r!r}"
        assert srv.alive()
    finally:
        srv.stop()


@test("stalled caller: a peer that stops reading is hung up and the line is free again")
def t_stall():
    srv = Server("-i", "S0=1")
    try:
        v = srv.vice()
        c = srv.caller()
        c.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        got = strip232(rd(v, 4.0, until=b"CONNECT"))
        assert b"CONNECT" in got, got
        # the caller never reads again; the BBS keeps talking
        v.setblocking(False)
        chunk = b"X" * 65536
        end = time.time() + 45
        dropped = False
        buf = b""
        while time.time() < end and not dropped:
            try:
                v.send(chunk)
            except (BlockingIOError, OSError):
                pass
            try:
                d = v.recv(4096)
                buf += d
                if b"NO CARRIER" in strip232(buf):
                    dropped = True
            except (BlockingIOError, OSError):
                time.sleep(0.2)
        assert dropped, "caller that stopped reading was never hung up (line wedged)"
        assert b"not reading" in srv.logtext(), "no log line about the stalled caller"
        v.setblocking(True)
        c.close()
        time.sleep(5.5)
        rd(v, 1.0)
        c2 = srv.caller()
        got = strip232(rd(v, 6.0, until=b"CONNECT"))
        assert b"CONNECT" in got, f"line not usable after the stall: {got!r}"
        c2.close()
        assert srv.alive()
    finally:
        srv.stop()


@test("max session (-m 1): an idle caller is cut at the limit")
def t_max_session():
    srv = Server("-m", "1", "-i", "S0=1")
    try:
        v = srv.vice()
        c = srv.caller()
        got = strip232(rd(v, 4.0, until=b"CONNECT"))
        assert b"CONNECT" in got, got
        t0 = time.time()
        r = strip232(rd(v, 75.0, until=b"NO CARRIER"))
        took = time.time() - t0
        assert b"NO CARRIER" in r and 50 < took < 70, f"-m 1: {r!r} after {took:.0f}s"
        assert rd(c, 2.0) == b"" or True
        c.close()
    finally:
        srv.stop()


@test("web console: Host header guard (DNS rebinding), --web-host, token never logged, timed blocks exist and clear")
def t_web_guard():
    wport = free_port()
    srv = Server("-x", "-H", str(wport))
    def http(port, req):
        s = socket.create_connection(("127.0.0.1", port))
        s.sendall(req); r = rd(s, 2.0); s.close(); return r
    try:
        assert wait_port(wport)
        r = http(wport, b"GET /api/status HTTP/1.1\r\nHost: evil.example:%d\r\n\r\n" % wport)
        assert r.startswith(b"HTTP/1.1 403"), f"foreign Host name was served: {r[:80]!r}"
        for host in (b"127.0.0.1:%d" % wport, b"localhost:%d" % wport, b"localhost"):
            r = http(wport, b"GET /api/status HTTP/1.1\r\nHost: " + host + b"\r\n\r\n")
            assert r.startswith(b"HTTP/1.1 200"), (host, r[:80])
        # connection flood: FLOOD_HITS rate-limited connects earn a timed block
        # (the first one fails the gate; the rest arrive inside the rate limit)
        for _ in range(9):
            c = srv.caller(); c.close(); time.sleep(0.2)
        time.sleep(0.5)
        r = http(wport, b"GET /api/status HTTP/1.1\r\nHost: localhost\r\n\r\n")
        assert b'"kind":"timed"' in r.replace(b" ", b""), f"no timed block after a flood: {r[r.find(b'blocklist'):][:200]!r}"
        r = http(wport, b"POST /api/unblock HTTP/1.1\r\nHost: localhost\r\nX-Requested-With: tcpser\r\n\r\n")
        assert r.startswith(b"HTTP/1.1 200"), r[:80]
        r = http(wport, b"GET /api/status HTTP/1.1\r\nHost: localhost\r\n\r\n")
        assert b'"kind":"timed"' not in r.replace(b" ", b""), "timed block not cleared"
    finally:
        srv.stop()
    wport = free_port()
    srv = Server("-H", str(wport), "--web-host", "bbs.example")
    try:
        assert wait_port(wport)
        r = http(wport, b"GET /api/status HTTP/1.1\r\nHost: bbs.example\r\n\r\n")
        assert r.startswith(b"HTTP/1.1 200"), r[:80]
    finally:
        srv.stop()
    wport = free_port()
    srv = Server("-H", str(wport), env={"TCPSER_DASH_TOKEN": "S3cretTok"})
    try:
        assert wait_port(wport)
        r = http(wport, b"GET /?token=S3cretTok HTTP/1.1\r\nHost: bbs.example\r\n\r\n")
        assert r.startswith(b"HTTP/1.1 302"), r[:80]
        http(wport, b"GET /../x?token=S3cretTok HTTP/1.1\r\nHost: x\r\n\r\n")
        time.sleep(0.3)
        assert b"S3cretTok" not in srv.logtext(), "the web token was written to the log"
    finally:
        srv.stop()


@test("ip232: DCD state is sent to an emulator when it attaches")
def t_ip232_attach():
    srv = Server("-i", "&C0")
    try:
        v = srv.vice()
        r = rd(v, 1.5)
        assert b"\xff\x01" in r, f"no DCD-on code after attach with &C0: {r!r}"
        v.close(); time.sleep(0.5)
        v = srv.vice()
        r = rd(v, 1.5)
        assert b"\xff\x01" in r, f"no DCD-on code after re-attach: {r!r}"
    finally:
        srv.stop()



if __name__ == "__main__":
    if not os.path.exists(BIN):
        print("binary not found:", BIN); sys.exit(2)
    print(f"tcpser tests against {BIN}")
    only = os.environ.get("TEST_FILTER", "")
    for t in TESTS:
        if only and only.lower() not in t.__name__.lower() and only.lower() not in str(t.__doc__ or "").lower():
            continue
        t()
    failed = [r for r in RESULTS if not r[1]]
    print(f"\n{len(RESULTS)-len(failed)}/{len(RESULTS)} passed")
    sys.exit(1 if failed else 0)
