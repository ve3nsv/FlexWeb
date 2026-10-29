"""RF-KIT (RF2K-S) amplifier through wfweb, against a stub amp on loopback.

The stub speaks the amplifier's REST/JSON API (replies as in AetherSDR's
rfkit_connection_test) and, like a RF2K-S whose restServer.py has the tune
modification, answers PUT /tuner {} with 400 and {"tuner_mode":"START"} with
200. Checked here: the tune-API probe reaches the page as tuneApi, and the
amp controls are refused unless a signed-in named user asks while no other
browser holds the TX lock (read-only with no web users at all).
"""

import asyncio
import json
import os
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pytest
import requests

from mock_flex_radio import MockFlexRadio
from test_web_login import (WFWEB_BIN, _Reader, _drain_until, _free_port, _login, _wait,
                            _ws, _ws_send)

ROUTES = {
    "GET /info": (200, {"device": "RFKIT", "software_version": {"GUI": 1, "controller": 2},
                        "custom_device_name": "RF-POWER"}),
    "GET /power": (200, {"temperature": {"value": 40, "unit": "C"}, "voltage": {"value": 50, "unit": "V"},
                         "current": {"value": 10, "unit": "A"},
                         "forward": {"value": 0, "max_value": 1500, "unit": "W"},
                         "reflected": {"value": 0, "max_value": 300, "unit": "W"},
                         "swr": {"value": 1.0, "max_value": 10, "unit": ""}}),
    "GET /data": (200, {"band": {"value": 40, "unit": "m"}, "frequency": {"value": 7250, "unit": "kHz"},
                        "status": "OK"}),
    "GET /tuner": (200, {"mode": "AUTO", "setup": "CL"}),
    "GET /operate-mode": (200, {"operate_mode": "STANDBY"}),
    "GET /operational-interface": (200, {"operational_interface": "CAT"}),
    "GET /antennas": (200, {"antennas": [{"type": "INTERNAL", "number": 1, "state": "ACTIVE"},
                                         {"type": "INTERNAL", "number": 2, "state": "AVAILABLE"}]}),
    "PUT /operate-mode": (200, {"operate_mode": "OPERATE"}),
    "PUT /antennas/active": (200, {"type": "INTERNAL", "number": 2}),
}


class StubAmp:
    def __init__(self):
        self.puts = []        # (path, body) of every PUT
        self.gets = []        # (time, path) of every GET
        self.wake_delay = 0.0  # a sleeping RF2K-S answers the wake after ~5 s
        self.broken_pipes = 0
        self.refuse_operate = False   # answer PUT /operate-mode with 409, like a disabled amp
        stub = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def _reply(self, code, obj):
                body = json.dumps(obj).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                stub.gets.append((time.monotonic(), self.path))
                self._reply(*ROUTES.get("GET " + self.path, (404, {"status": "error"})))

            def do_PUT(self):
                n = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(n).decode()
                stub.puts.append((self.path, body))
                if self.path == "/tuner":
                    # the modified restServer.py: a body without tuner_mode is a 400
                    if json.loads(body or "{}").get("tuner_mode") == "START":
                        self._reply(200, {"mode": "AUTO_TUNING_FROM_AUTO", "setup": "CL"})
                    else:
                        if stub.wake_delay:
                            time.sleep(stub.wake_delay)
                        try:
                            self._reply(400, {"status": "400 Bad Request", "reason": "START required"})
                        except (BrokenPipeError, ConnectionResetError):
                            stub.broken_pipes += 1   # wfweb gave up before the answer
                    return
                if self.path == "/operate-mode" and stub.refuse_operate:
                    self._reply(409, {"status": "409 Conflict", "reason": "currently disabled"})
                    return
                self._reply(*ROUTES.get("PUT " + self.path, (404, {"status": "error"})))

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def wakes(self):
        """The empty PUT /tuner {}: wfweb's wake / tune-API request."""
        return [p for p in self.puts if p[0] == "/tuner" and p[1].strip() == "{}"]

    def gets_since(self, t0, path=None):
        return [g for g in self.gets if g[0] >= t0 and (path is None or g[1] == path)]

    def commands(self):
        """PUTs other than the tune-API probe."""
        return [p for p in self.puts if not (p[0] == "/tuner" and p[1].strip() == "{}")]


def _start(env, radio, amp, extra=()):
    port = _free_port()
    proc = subprocess.Popen([str(WFWEB_BIN), "-p", str(port), "--manufacturer", "3", "--lan", "127.0.0.1",
                             "--lan-control", str(radio.control_port), "--lan-audio", str(radio.vita_port),
                             "--rfkit", f"127.0.0.1:{amp.port}", *extra],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    if not _wait(lambda: requests.get(f"http://127.0.0.1:{port + 1}/api/v1/radio/info", timeout=2).ok):
        proc.terminate()
        pytest.fail("wfweb did not start:\n" + proc.communicate(timeout=5)[0].decode(errors="replace"))
    return proc, port


@pytest.fixture()
def bench(tmp_path):
    if not WFWEB_BIN.exists():
        pytest.skip(f"wfweb binary not found at {WFWEB_BIN}")
    home = str(tmp_path)
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=f"{home}/.config", WFWEB_NO_TUI="1",
               WFWEB_CONFIG_DIR=home)
    loop = asyncio.new_event_loop()
    radio = MockFlexRadio()
    loop.run_until_complete(radio.start())
    thread = threading.Thread(target=loop.run_forever, daemon=True)
    thread.start()
    amp = StubAmp()
    procs = []
    try:
        yield env, radio, amp, procs
    finally:
        for p in procs:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
        amp.httpd.shutdown()
        loop.call_soon_threadsafe(loop.stop)
        thread.join(timeout=2)


def _amp_ready(reader):
    return _drain_until(reader, lambda m: m.get("type") == "amp" and m.get("connected")
                        and m.get("tuneApi") == "supported", timeout=15)


def test_amp_read_only_without_users(bench):
    env, radio, amp, procs = bench
    proc, port = _start(env, radio, amp)
    procs.append(proc)
    st, sock, buf = _ws(port, None)
    assert " 101" in st
    r = _Reader(sock, buf)
    try:
        assert _amp_ready(r), "amp never reported tune support"
        before = len(amp.commands())
        for cmd in ({"cmd": "ampOperate", "value": True}, {"cmd": "ampTune"},
                    {"cmd": "ampAntenna", "value": 2}, {"cmd": "ampErrorReset"}):
            _ws_send(sock, cmd)
            ref = _drain_until(r, lambda m: m.get("type") == "ampRefused")
            assert ref and ref["reason"] == "nousers", (cmd, ref)
        time.sleep(1.0)
        assert amp.commands()[before:] == [], amp.commands()[before:]
    finally:
        sock.close()


def test_amp_controls_need_user_and_tx_lock(bench):
    env, radio, amp, procs = bench
    for user, pw in (("tom", "tom-password-1"), ("amy", "amy-password")):
        add = subprocess.run([str(WFWEB_BIN), "--add-user", user], input=(pw + "\n").encode(),
                             capture_output=True, env=env, timeout=20)
        assert add.returncode == 0, add.stdout + add.stderr
    proc, port = _start(env, radio, amp)
    procs.append(proc)
    base = f"https://127.0.0.1:{port}"
    tom = _login(base, "tom", "tom-password-1").cookies["wfweb_session"]
    amy = _login(base, "amy", "amy-password").cookies["wfweb_session"]
    st, a, abuf = _ws(port, tom)
    assert " 101" in st
    st, b, bbuf = _ws(port, amy)
    assert " 101" in st
    ra, rb = _Reader(a, abuf), _Reader(b, bbuf)
    try:
        assert _amp_ready(ra)
        # signed in, nobody transmitting: allowed
        _ws_send(a, {"cmd": "ampOperate", "value": True})
        assert _wait(lambda: ("/operate-mode", '{"operate_mode":"OPERATE"}') in amp.puts, timeout=5), amp.puts
        _ws_send(a, {"cmd": "ampTune"})
        assert _wait(lambda: ("/tuner", '{"tuner_mode":"START"}') in amp.puts, timeout=5), amp.puts

        # tom keys: amy's amp commands are refused and say who has the radio
        _ws_send(a, {"cmd": "setPTT", "value": True})
        assert _drain_until(rb, lambda m: m.get("type") == "txState" and m.get("on"))
        n = len(amp.commands())
        _ws_send(b, {"cmd": "ampAntenna", "value": 2})
        ref = _drain_until(rb, lambda m: m.get("type") == "ampRefused")
        assert ref and ref["reason"] == "tx:tom", ref
        time.sleep(0.8)
        assert amp.commands()[n:] == []
        # the operator keying may still use them
        _ws_send(a, {"cmd": "ampAntenna", "value": 2})
        assert _wait(lambda: any(p[0] == "/antennas/active" for p in amp.puts), timeout=5)
        _ws_send(a, {"cmd": "setPTT", "value": False})
    finally:
        a.close()
        b.close()


def test_amp_wake_keep_awake_and_poll_schedule(bench, tmp_path):
    """Punch list 42: no tune-API probe on connect (it wakes a sleeping
    RF2K-S); the empty PUT /tuner goes out when someone who may transmit
    connects (not a guest), repeats every KeepAwakeSecs while they are on and
    stops when they leave. Punch list 43: one GET at a time, ~3.7/s in
    receive (was 12), /power fast while the radio transmits. Punch list 48:
    nothing at all is sent to the amp while no browser is connected."""
    env, radio, amp, procs = bench
    add = subprocess.run([str(WFWEB_BIN), "--add-user", "tom"], input=b"tom-password-1\n",
                         capture_output=True, env=env, timeout=20)
    assert add.returncode == 0
    ini = tmp_path / "flexweb.ini"
    ini.write_text("[Amplifier]\nKeepAwakeSecs=2\nIdleGraceSecs=2\n")
    proc, port = _start(env, radio, amp, ("--guest-view", "-s", str(ini)))
    procs.append(proc)
    base = f"https://127.0.0.1:{port}"
    # no browser: no request at all, not even /info
    time.sleep(3.0)
    assert amp.gets == [] and amp.puts == [], (amp.gets, amp.puts)

    # a guest connects: polled (it sees the amp panel), never woken - it cannot transmit
    g = requests.post(base + "/login", data={"guest": "1"}, verify=False, allow_redirects=False, timeout=10)
    st, gs, gbuf = _ws(port, g.cookies["wfweb_session"])
    assert " 101" in st
    assert _wait(lambda: amp.gets_since(0, "/power"), timeout=15)
    time.sleep(2.0)
    assert amp.wakes() == [], amp.puts

    # receive schedule: about 3.7 GETs a second, not 12
    t0 = time.monotonic()
    time.sleep(4.0)
    rate = len(amp.gets_since(t0)) / 4.0
    assert 2.0 <= rate <= 6.0, rate

    tom = _login(base, "tom", "tom-password-1").cookies["wfweb_session"]
    st, a, abuf = _ws(port, tom)
    assert " 101" in st
    ra = _Reader(a, abuf)
    try:
        assert _wait(lambda: len(amp.wakes()) >= 1, timeout=5), "no wake when tom connected"
        assert _amp_ready(ra), "the wake reply did not report tune support"
        # kept awake every 2 s while tom is on
        assert _wait(lambda: len(amp.wakes()) >= 3, timeout=8), amp.wakes()

        # transmit: /power as fast as the amp answers
        _ws_send(a, {"cmd": "setPTT", "value": True})
        # punch list 44: the page's over starts with the radio keyed
        assert _drain_until(ra, lambda m: m.get("type") == "amp" and m.get("keyed") is True, timeout=5)
        time.sleep(0.5)
        t1 = time.monotonic()
        time.sleep(2.0)
        fast = len(amp.gets_since(t1, "/power"))
        assert fast >= 12, fast            # ~26/s nominal at 25 ms; was 4/s
        assert not amp.gets_since(t1, "/data"), "receive-only reads continued during TX"
        _ws_send(a, {"cmd": "setPTT", "value": False})
        assert _drain_until(ra, lambda m: m.get("type") == "amp" and m.get("keyed") is False, timeout=5)
        # back in receive after the 3 s hold
        time.sleep(4.0)
        t2 = time.monotonic()
        time.sleep(3.0)
        slow = len(amp.gets_since(t2, "/power"))
        assert slow <= 4, slow
    finally:
        a.close()
    # tom gone (the guest stays): the repeats stop, polling goes on
    time.sleep(1.0)
    n = len(amp.wakes())
    time.sleep(4.5)
    assert len(amp.wakes()) == n, amp.wakes()
    assert amp.gets_since(time.monotonic() - 2.0), "polling stopped while the guest is still on"

    # the last browser gone: after the 2 s grace, nothing more reaches the amp
    gs.close()
    time.sleep(4.0)
    t3 = time.monotonic()
    time.sleep(3.0)
    assert not amp.gets_since(t3) and len(amp.wakes()) == n, amp.gets_since(t3)


def test_amp_slow_wake_is_waited_for(bench):
    """Punch list 46: a sleeping RF2K-S answers the wake after ~5 s. wfweb
    waits for it (it used to give up at 4 s; the amp's server then wrote into
    a closed connection, and flooded with those it stopped answering), stays
    connected, and learns TUNE support from the late answer."""
    env, radio, amp, procs = bench
    amp.wake_delay = 5.5
    proc, port = _start(env, radio, amp)
    procs.append(proc)
    st, sock, buf = _ws(port, None)   # login off: this browser may transmit
    assert " 101" in st
    r = _Reader(sock, buf)
    try:
        assert _amp_ready(r), "the slow wake's answer never reached the page"
        assert amp.broken_pipes == 0, "wfweb abandoned the wake before the amp answered"
        # polling carried on after it, and the amp is still connected
        t0 = time.monotonic()
        time.sleep(3.0)
        assert amp.gets_since(t0), "polling stopped after the slow wake"
        assert not _drain_until(r, lambda m: m.get("type") == "amp" and m.get("connected") is False, timeout=1.0)
    finally:
        sock.close()


def test_amp_refusal_reaches_the_page(bench):
    """Punch list 50: a command the amp turns down (409, "currently
    disabled") is told to the browser that sent it, with the amp's reason."""
    env, radio, amp, procs = bench
    amp.refuse_operate = True
    add = subprocess.run([str(WFWEB_BIN), "--add-user", "tom"], input=b"tom-password-1\n",
                         capture_output=True, env=env, timeout=20)
    assert add.returncode == 0
    proc, port = _start(env, radio, amp)
    procs.append(proc)
    tom = _login(f"https://127.0.0.1:{port}", "tom", "tom-password-1").cookies["wfweb_session"]
    st, a, abuf = _ws(port, tom)
    assert " 101" in st
    ra = _Reader(a, abuf)
    try:
        assert _amp_ready(ra)
        _ws_send(a, {"cmd": "ampOperate", "value": True})
        ref = _drain_until(ra, lambda m: m.get("type") == "ampRefused" and m.get("reason") == "amp", timeout=5)
        assert ref and ref.get("what") == "OPERATE" and ref.get("text") == "currently disabled", ref
    finally:
        a.close()


def test_amp_offline_reason(bench):
    """Punch list 49: nothing listening at the amp's address - the page is
    told the amp's program is not running, not just "offline"."""
    env, radio, amp, procs = bench
    closed = _free_port()          # nothing listens here
    port = _free_port()
    proc = subprocess.Popen([str(WFWEB_BIN), "-p", str(port), "--manufacturer", "3", "--lan", "127.0.0.1",
                             "--lan-control", str(radio.control_port), "--lan-audio", str(radio.vita_port),
                             "--rfkit", f"127.0.0.1:{closed}"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    procs.append(proc)
    assert _wait(lambda: requests.get(f"http://127.0.0.1:{port + 1}/api/v1/radio/info", timeout=2).ok)
    st, sock, buf = _ws(port, None)
    assert " 101" in st
    r = _Reader(sock, buf)
    try:
        m = _drain_until(r, lambda m: m.get("type") == "amp" and m.get("connected") is False
                         and m.get("offlineReason"), timeout=15)
        assert m and m["offlineReason"] == "refused", m
    finally:
        sock.close()
