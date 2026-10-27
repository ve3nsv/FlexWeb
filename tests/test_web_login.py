"""Web login (webAuth): named users, the login page, the session cookie on
the page / API / WebSocket, sign-out, the lockout, and the REST port on
127.0.0.1 only. Runs wfweb against the mock Flex radio; needs ./wfweb."""

from __future__ import annotations

import asyncio
import base64
import json
import os
import socket
import ssl
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest
import requests
import urllib3

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mock_flex_radio import MockFlexRadio  # noqa: E402

urllib3.disable_warnings()
PROJECT_ROOT = Path(__file__).resolve().parent.parent
WFWEB_BIN = PROJECT_ROOT / "wfweb"
PASSWORD = "hunter2-long"


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _wait(pred, timeout=20.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            v = pred()
            if v:
                return v
        except (requests.RequestException, ValueError, OSError):
            pass
        time.sleep(0.25)
    return None


def _ws(port: int, cookie: str | None):
    """WebSocket handshake on the HTTPS port; returns (status line, socket)."""
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    sock = ctx.wrap_socket(socket.create_connection(("127.0.0.1", port), timeout=5))
    key = base64.b64encode(os.urandom(16)).decode()
    extra = f"Cookie: wfweb_session={cookie}\r\n" if cookie else ""
    sock.sendall((f"GET / HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\n"
                  f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n{extra}"
                  "Sec-WebSocket-Version: 13\r\n\r\n").encode())
    head = b""
    while b"\r\n\r\n" not in head:
        chunk = sock.recv(4096)
        if not chunk:
            break
        head += chunk
    status = head.split(b"\r\n", 1)[0].decode(errors="replace")
    # Anything after the headers is the start of the frame stream.
    rest = head.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in head else b""
    return status, sock, rest


class _Reader:
    """Server text frames (unmasked) from one socket, keeping partial
    frames between calls."""

    def __init__(self, sock, buf: bytes = b""):
        self.sock, self.buf = sock, buf

    def frames(self, timeout=8.0):
        for op, payload in self.all(timeout):
            if op == 1:
                yield payload.decode(errors="replace")

    def all(self, timeout=8.0):
        """(opcode, payload) for every frame until timeout."""
        self.sock.settimeout(0.2)
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            msg = self._one()
            if msg is not None:
                yield msg
                continue
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                return
            self.buf += chunk

    def _one(self):
        buf = self.buf
        while len(buf) >= 2:
            b0, b1 = buf[0], buf[1]
            n, off = b1 & 0x7F, 2
            if n == 126:
                if len(buf) < 4:
                    return None
                n, off = struct.unpack(">H", buf[2:4])[0], 4
            elif n == 127:
                if len(buf) < 10:
                    return None
                n, off = struct.unpack(">Q", buf[2:10])[0], 10
            if len(buf) < off + n:
                return None
            payload, buf = buf[off:off + n], buf[off + n:]
            self.buf = buf
            return b0 & 0x0F, payload
        return None


def _ws_text_frames(sock, buf: bytes, timeout=8.0):
    """Yield server text frames (unmasked) until timeout."""
    sock.settimeout(0.5)
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        while len(buf) >= 2:
            b0, b1 = buf[0], buf[1]
            n, off = b1 & 0x7F, 2
            if n == 126:
                if len(buf) < 4:
                    break
                n, off = struct.unpack(">H", buf[2:4])[0], 4
            elif n == 127:
                if len(buf) < 10:
                    break
                n, off = struct.unpack(">Q", buf[2:10])[0], 10
            if len(buf) < off + n:
                break
            payload, buf = buf[off:off + n], buf[off + n:]
            if b0 & 0x0F == 1:
                yield payload.decode(errors="replace")
            elif b0 & 0x0F == 8:
                return
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            return
        buf += chunk


@pytest.fixture(scope="module")
def server():
    if not WFWEB_BIN.exists():
        pytest.skip(f"wfweb binary not found at {WFWEB_BIN}")
    home = "/tmp/wfweb-web-login-test"
    subprocess.run(["rm", "-rf", home])
    os.makedirs(home)
    # WFWEB_CONFIG_DIR: users in an ini file here on every platform (macOS
    # QSettings ignores $HOME and would leak the users into other tests).
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=f"{home}/.config", WFWEB_NO_TUI="1",
               WFWEB_CONFIG_DIR=home)

    # Users from the command line; a pipe gives the password on one line.
    add = subprocess.run([str(WFWEB_BIN), "--add-user", "tom"], input=(PASSWORD + "\n").encode(),
                         capture_output=True, env=env, timeout=20)
    assert add.returncode == 0, add.stdout + add.stderr
    short = subprocess.run([str(WFWEB_BIN), "--add-user", "amy"], input=b"short\n",
                           capture_output=True, env=env, timeout=20)
    assert short.returncode != 0
    amy = subprocess.run([str(WFWEB_BIN), "--add-user", "amy"], input=b"amy-password\n",
                         capture_output=True, env=env, timeout=20)
    assert amy.returncode == 0
    lst = subprocess.run([str(WFWEB_BIN), "--list-users"], capture_output=True, env=env, timeout=20)
    assert lst.stdout.decode().split() == ["amy", "tom"]
    stored = (Path(home) / "webusers.ini").read_text()
    assert PASSWORD not in stored and "pbkdf2-sha256$" in stored

    loop = asyncio.new_event_loop()
    radio = MockFlexRadio()
    loop.run_until_complete(radio.start())
    thread = threading.Thread(target=loop.run_forever, daemon=True)
    thread.start()
    port = _free_port()
    proc = subprocess.Popen([str(WFWEB_BIN), "-p", str(port), "--manufacturer", "3", "--lan", "127.0.0.1",
                             "--lan-control", str(radio.control_port), "--lan-audio", str(radio.vita_port)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    base = f"https://127.0.0.1:{port}"
    try:
        if not _wait(lambda: requests.get(f"http://127.0.0.1:{port + 1}/api/v1/radio/info", timeout=2).ok):
            proc.terminate()
            pytest.fail("wfweb did not start:\n" + proc.communicate(timeout=5)[0].decode(errors="replace"))
        yield base, port, env, radio
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        loop.call_soon_threadsafe(loop.stop)
        thread.join(timeout=2)


def _ws_send(sock, obj, opcode=1) -> None:
    data = obj if isinstance(obj, bytes) else json.dumps(obj).encode()
    mask = os.urandom(4)
    head = bytes([0x80 | opcode])
    n = len(data)
    head += bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n)
    sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))


def _drain_until(reader, pred, timeout=6.0):
    for text in reader.frames(timeout):
        try:
            msg = json.loads(text)
        except ValueError:
            continue
        if pred(msg):
            return msg
    return None


def _login(base, user, password, remember=True):
    data = {"user": user, "password": password}
    if remember:
        data["remember"] = "1"
    return requests.post(f"{base}/login", data=data, verify=False, allow_redirects=False, timeout=10)


def test_page_needs_login(server):
    base, port, _, _ = server
    r = requests.get(base + "/", verify=False, allow_redirects=False, timeout=5)
    assert r.status_code == 303 and r.headers["Location"] == "/login"
    r = requests.get(base + "/login", verify=False, timeout=5)
    assert r.status_code == 200 and 'name="password"' in r.text
    assert "View only" not in r.text   # guest view is off unless asked
    assert requests.get(base + "/api/v1/radio/info", verify=False, timeout=5).status_code == 401
    status, sock, _ = _ws(port, None)
    sock.close()
    assert " 101" not in status, status


def test_rest_port_is_local_only(server):
    _, port, _, _ = server
    # Reachable on 127.0.0.1 (scripts, the launcher) without a login...
    assert requests.get(f"http://127.0.0.1:{port + 1}/api/v1/radio/info", timeout=5).ok
    # ...and not on any other address of this machine.
    try:
        other = socket.gethostbyname(socket.gethostname())
    except OSError:
        other = ""
    if other and not other.startswith("127."):
        with pytest.raises(requests.ConnectionError):
            requests.get(f"http://{other}:{port + 1}/api/v1/radio/info", timeout=3)


def test_wrong_password(server):
    base, _, _, _ = server
    r = _login(base, "tom", "nope")
    assert r.status_code == 401 and "Wrong user name or password" in r.text
    assert "Set-Cookie" not in r.headers
    assert _login(base, "nobody", PASSWORD).status_code == 401


def test_sign_in_and_out(server):
    base, port, _, _ = server
    r = _login(base, "tom", PASSWORD)
    assert r.status_code == 303 and r.headers["Location"] == "/"
    sc = r.headers["Set-Cookie"]
    assert "HttpOnly" in sc and "Secure" in sc and "SameSite=Strict" in sc and "Max-Age=" in sc
    token = r.cookies["wfweb_session"]
    jar = {"wfweb_session": token}
    page = requests.get(base + "/", cookies=jar, verify=False, timeout=5)
    assert page.status_code == 200 and "<html" in page.text.lower()
    assert requests.get(base + "/api/v1/radio/info", cookies=jar, verify=False, timeout=5).ok

    status, sock, buf = _ws(port, token)
    assert " 101" in status, status
    user = None
    for text in _ws_text_frames(sock, buf):
        msg = json.loads(text)
        if msg.get("type") == "rigInfo":
            user = msg.get("webUser")
            assert msg.get("webLogin") is True
            break
    sock.close()
    assert user == "tom"

    out = requests.get(base + "/logout", cookies=jar, verify=False, allow_redirects=False, timeout=5)
    assert out.status_code == 303 and "Max-Age=0" in out.headers["Set-Cookie"]
    assert requests.get(base + "/api/v1/radio/info", cookies=jar, verify=False, timeout=5).status_code == 401
    status, sock, _ = _ws(port, token)
    sock.close()
    assert " 101" not in status


def test_password_change_signs_out(server):
    base, _, env, _ = server
    token = _login(base, "tom", PASSWORD).cookies["wfweb_session"]
    jar = {"wfweb_session": token}
    assert requests.get(base + "/api/v1/radio/info", cookies=jar, verify=False, timeout=5).ok
    ch = subprocess.run([str(WFWEB_BIN), "--passwd", "tom"], input=(PASSWORD + "\n").encode(),
                        capture_output=True, env=env, timeout=20)
    assert ch.returncode == 0
    # The running server sees the change made by another process.
    assert _wait(lambda: requests.get(base + "/api/v1/radio/info", cookies=jar, verify=False,
                                      timeout=5).status_code == 401, timeout=5)


def test_tx_lock(server):
    """One browser transmits at a time; the others see who, cannot key over
    it, and their release does not unkey it."""
    base, port, _, radio = server
    tok_tom = _login(base, "tom", PASSWORD).cookies["wfweb_session"]
    tok_amy = _login(base, "amy", "amy-password").cookies["wfweb_session"]
    st, a, abuf = _ws(port, tok_tom)
    assert " 101" in st
    st, b, bbuf = _ws(port, tok_amy)
    assert " 101" in st
    ra, rb = _Reader(a, abuf), _Reader(b, bbuf)
    try:
        assert _drain_until(ra, lambda m: m.get("type") == "rigInfo")
        assert _drain_until(rb, lambda m: m.get("type") == "rigInfo")
        before = len(radio.find("xmit"))

        _ws_send(a, {"cmd": "setPTT", "value": True})
        assert _wait(lambda: radio.find("xmit")[before:] == ["xmit 1"], timeout=5)
        seen = _drain_until(rb, lambda m: m.get("type") == "txState" and m.get("on"))
        assert seen and seen["by"] == "tom" and seen["mine"] is False

        # amy presses PTT: refused, told who has the radio; her release
        # must not unkey tom.
        _ws_send(b, {"cmd": "setPTT", "value": True})
        busy = _drain_until(rb, lambda m: m.get("type") == "txBusy")
        assert busy and busy["by"] == "tom"
        _ws_send(b, {"cmd": "setPTT", "value": False})
        time.sleep(1.0)
        assert radio.find("xmit")[before:] == ["xmit 1"]

        _ws_send(a, {"cmd": "setPTT", "value": False})
        assert _wait(lambda: radio.find("xmit")[before:] == ["xmit 1", "xmit 0"], timeout=5)
        free = _drain_until(rb, lambda m: m.get("type") == "txState" and not m.get("on"))
        assert free

        # Now amy may key.
        _ws_send(b, {"cmd": "setPTT", "value": True})
        assert _wait(lambda: radio.find("xmit")[before:][-1:] == ["xmit 1"], timeout=5)
        _ws_send(b, {"cmd": "setPTT", "value": False})
        assert _wait(lambda: radio.find("xmit")[before:][-1:] == ["xmit 0"], timeout=5)
    finally:
        a.close()
        b.close()


def test_intercom(server):
    """While tom transmits: amy on OPERATOR hears tom's microphone (flagged
    as intercom) instead of the receiver; on ON-AIR she hears the radio,
    whose MON is switched on for the transmission and off again after."""
    if sys.platform == "darwin":
        # Raw-socket audio timing on the shared macOS runner is too erratic;
        # the routing is the same code everywhere and runs on Linux.
        pytest.skip("audio routing test is timing-sensitive on the macOS runner")
    base, port, _, radio = server
    tok_tom = _login(base, "tom", PASSWORD).cookies["wfweb_session"]
    tok_amy = _login(base, "amy", "amy-password").cookies["wfweb_session"]
    st, a, abuf = _ws(port, tok_tom)
    st2, b, bbuf = _ws(port, tok_amy)
    assert " 101" in st and " 101" in st2
    ra, rb = _Reader(a, abuf), _Reader(b, bbuf)
    try:
        assert _drain_until(ra, lambda m: m.get("type") == "rigInfo")
        assert _drain_until(rb, lambda m: m.get("type") == "rigInfo")
        for sock, r in ((a, ra), (b, rb)):
            _ws_send(sock, {"cmd": "enableAudio", "value": True})
            st_msg = _drain_until(r, lambda m: m.get("type") == "audioStatus")
            assert st_msg, "no audioStatus"
            if not st_msg.get("enabled"):
                pytest.skip(f"browser audio unavailable here: {st_msg.get('reason')}")
        _ws_send(b, {"cmd": "setTxListen", "mode": "op"})
        _ws_send(a, {"cmd": "enableMic", "value": True})
        time.sleep(0.5)

        # Before TX amy gets ordinary receiver audio.
        kinds = [p[1] for op, p in rb.all(1.0) if op == 2 and len(p) > 6 and p[0] == 0x02]
        assert 0 in kinds and 1 not in kinds

        _ws_send(a, {"cmd": "setPTT", "value": True})
        assert _wait(lambda: radio.find("xmit")[-1:] == ["xmit 1"], timeout=5)
        tone = struct.pack("<480h", *([1000, -1000] * 240))
        for i in range(40):
            _ws_send(a, bytes([0x03, 0x00]) + struct.pack("<HH", i, 0) + tone, opcode=2)
            time.sleep(0.01)
        # By order, not by time (CI machines are slow): once the operator's
        # audio has started, no receiver audio may follow it while keyed.
        intercom, rx_after = 0, 0
        for op, p in rb.all(2.0):
            if op == 2 and len(p) > 6 and p[0] == 0x02:
                if p[1] == 1:
                    intercom += 1
                elif intercom:
                    rx_after += 1
        assert intercom >= 1, "the operator's audio never reached the listener"
        assert rx_after == 0, rx_after
        _ws_send(a, {"cmd": "setPTT", "value": False})
        assert _wait(lambda: radio.find("xmit")[-1:] == ["xmit 0"], timeout=5)

        # ON-AIR: MON comes on for the transmission only.
        _ws_send(b, {"cmd": "setTxListen", "mode": "air"})
        time.sleep(0.3)
        mons = len(radio.find("transmit set mon="))
        _ws_send(a, {"cmd": "setPTT", "value": True})
        assert _wait(lambda: "transmit set mon=1" in radio.find("transmit set mon=")[mons:], timeout=5)
        _ws_send(a, {"cmd": "setPTT", "value": False})
        assert _wait(lambda: radio.find("transmit set mon=")[mons:][-1:] == ["transmit set mon=0"], timeout=5)
    finally:
        a.close()
        b.close()


def test_opus_tx_voice(server):
    """AUDIO > QUALITY OPUS: the page sends its voice as 0x09 Opus frames; the
    server decodes them and they take the same path as 0x03 PCM (here: on to
    the OPERATOR listener while keyed)."""
    if sys.platform == "darwin":
        pytest.skip("audio routing test is timing-sensitive on the macOS runner")
    base, port, _, radio = server
    tok_tom = _login(base, "tom", PASSWORD).cookies["wfweb_session"]
    tok_amy = _login(base, "amy", "amy-password").cookies["wfweb_session"]
    st, a, abuf = _ws(port, tok_tom)
    st2, b, bbuf = _ws(port, tok_amy)
    assert " 101" in st and " 101" in st2
    ra, rb = _Reader(a, abuf), _Reader(b, bbuf)
    try:
        info = _drain_until(ra, lambda m: m.get("type") == "rigInfo")
        assert info and info.get("txOpus") is True
        assert _drain_until(rb, lambda m: m.get("type") == "rigInfo")
        for sock, r in ((a, ra), (b, rb)):
            _ws_send(sock, {"cmd": "enableAudio", "value": True})
            st_msg = _drain_until(r, lambda m: m.get("type") == "audioStatus")
            assert st_msg, "no audioStatus"
            if not st_msg.get("enabled"):
                pytest.skip(f"browser audio unavailable here: {st_msg.get('reason')}")
        _ws_send(b, {"cmd": "setTxListen", "mode": "op"})
        _ws_send(a, {"cmd": "enableMic", "value": True})
        time.sleep(0.5)
        _ws_send(a, {"cmd": "setPTT", "value": True})
        assert _wait(lambda: radio.find("xmit")[-1:] == ["xmit 1"], timeout=5)
        # A one-byte Opus packet: TOC 0xF8 (CELT fullband 20 ms, mono, one
        # frame) with an empty frame - decodes to 960 samples of silence.
        for i in range(25):
            _ws_send(a, bytes([0x09, 0x00]) + struct.pack("<HH", i, 0) + bytes([0xF8]), opcode=2)
            time.sleep(0.02)
        intercom = sum(1 for op, p in rb.all(2.0) if op == 2 and len(p) > 6 and p[0] == 0x02 and p[1] == 1)
        assert intercom >= 1, "decoded Opus voice never reached the listener"
        _ws_send(a, {"cmd": "setPTT", "value": False})
        assert _wait(lambda: radio.find("xmit")[-1:] == ["xmit 0"], timeout=5)
    finally:
        a.close()
        b.close()


def test_intercom_talk(server):
    """TALK: amy's microphone (0x06) reaches tom as 0x07 with a talker id,
    never comes back to amy, never keys the radio, and both browsers are
    told who is talking (and when she stops)."""
    base, port, _, radio = server
    tok_tom = _login(base, "tom", PASSWORD).cookies["wfweb_session"]
    tok_amy = _login(base, "amy", "amy-password").cookies["wfweb_session"]
    st, a, abuf = _ws(port, tok_tom)
    st2, b, bbuf = _ws(port, tok_amy)
    assert " 101" in st and " 101" in st2
    ra, rb = _Reader(a, abuf), _Reader(b, bbuf)
    try:
        assert _drain_until(ra, lambda m: m.get("type") == "rigInfo")
        assert _drain_until(rb, lambda m: m.get("type") == "rigInfo")
        for sock, r in ((a, ra), (b, rb)):
            _ws_send(sock, {"cmd": "enableAudio", "value": True})
            st_msg = _drain_until(r, lambda m: m.get("type") == "audioStatus")
            assert st_msg, "no audioStatus"
            if not st_msg.get("enabled"):
                pytest.skip(f"browser audio unavailable here: {st_msg.get('reason')}")
        xmits = len(radio.find("xmit 1"))
        tone = struct.pack("<480h", *([1000, -1000] * 240))
        for i in range(20):
            _ws_send(b, bytes([0x06, 0x00]) + struct.pack("<HH", i, 0) + tone, opcode=2)
            time.sleep(0.01)

        talk, talkers = [], []
        for op, p in ra.all(2.0):
            if op == 2 and p[:1] == b"\x07":
                talk.append(p)
            elif op == 1:
                m = json.loads(p)
                if m.get("type") == "talkers":
                    talkers.append(m)
        assert talk, "amy's talk audio never reached tom"
        assert talk[0][1] != 0 and struct.unpack("<H", talk[0][4:6])[0] == 48
        assert talk[0][6:] == tone
        assert any(m["who"] == ["amy"] and not m["me"] for m in talkers), talkers
        # She stopped sending: the talker list empties again.
        assert any(m["who"] == [] for m in talkers), talkers

        back = [p for op, p in rb.all(0.5) if op == 2 and p[:1] == b"\x07"]
        assert not back, "the talker heard herself"
        assert len(radio.find("xmit 1")) == xmits, "talk keyed the radio"
    finally:
        a.close()
        b.close()


def test_lockout(server):
    base, _, _, _ = server
    # The earlier sign-ins reset the count: five wrong passwords lock.
    codes = [_login(base, "tom", f"bad{i}").status_code for i in range(5)]
    assert codes[:4] == [401] * 4 and codes[4] == 429, codes
    # Even the right password is refused while locked out.
    r = _login(base, "tom", PASSWORD)
    assert r.status_code == 429 and "Try again" in r.text


def test_guest_view(tmp_path):
    """--guest-view: the login page's "View only" button gives a session that
    sees everything but is refused every command that changes the radio, keys
    it or reaches other browsers (punch list 2)."""
    if not WFWEB_BIN.exists():
        pytest.skip(f"wfweb binary not found at {WFWEB_BIN}")
    home = str(tmp_path)
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=f"{home}/.config", WFWEB_NO_TUI="1",
               WFWEB_CONFIG_DIR=home)
    add = subprocess.run([str(WFWEB_BIN), "--add-user", "tom"], input=(PASSWORD + "\n").encode(),
                         capture_output=True, env=env, timeout=20)
    assert add.returncode == 0
    loop = asyncio.new_event_loop()
    radio = MockFlexRadio()
    loop.run_until_complete(radio.start())
    thread = threading.Thread(target=loop.run_forever, daemon=True)
    thread.start()
    port = _free_port()
    proc = subprocess.Popen([str(WFWEB_BIN), "-p", str(port), "--manufacturer", "3", "--lan", "127.0.0.1",
                             "--lan-control", str(radio.control_port), "--lan-audio", str(radio.vita_port),
                             "--guest-view"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    base = f"https://127.0.0.1:{port}"
    try:
        assert _wait(lambda: requests.get(f"http://127.0.0.1:{port + 1}/api/v1/radio/info", timeout=2).ok)
        page = requests.get(base + "/login", verify=False, timeout=5)
        assert "View only (no password)" in page.text and 'name="guest"' in page.text
        r = requests.post(base + "/login", data={"guest": "1"}, verify=False, allow_redirects=False, timeout=10)
        assert r.status_code == 303 and r.headers["Location"] == "/"
        token = r.cookies["wfweb_session"]
        jar = {"wfweb_session": token}
        assert requests.get(base + "/", cookies=jar, verify=False, timeout=5).status_code == 200
        assert requests.get(base + "/api/v1/radio/info", cookies=jar, verify=False, timeout=5).ok
        # nothing changes through the API on the web port
        assert requests.put(base + "/api/v1/radio/frequency", json={"value": 7100000}, cookies=jar,
                            verify=False, timeout=5).status_code == 403

        st, sock, buf = _ws(port, token)
        assert " 101" in st, st
        rd = _Reader(sock, buf)
        try:
            info = _drain_until(rd, lambda m: m.get("type") == "rigInfo")
            assert info and info.get("guest") is True and info.get("webUser") == "guest"
            before = list(radio.commands)
            for cmd in ({"cmd": "setFrequency", "value": 7100000}, {"cmd": "setPTT", "value": True},
                        {"cmd": "setMode", "value": "CW"}, {"cmd": "ampOperate", "value": True},
                        {"cmd": "flexDsp", "key": "tune", "index": 0, "value": 1}):
                _ws_send(sock, cmd)
                ref = _drain_until(rd, lambda m: m.get("type") == "guestRefused")
                assert ref and ref["cmd"] == cmd["cmd"], (cmd, ref)
            # binary (mic / intercom) is dropped
            _ws_send(sock, bytes([0x03, 0, 0, 0, 0, 0]) + bytes(960), opcode=2)
            # read-only requests still answer
            _ws_send(sock, {"cmd": "timePing", "t0": 1})
            assert _drain_until(rd, lambda m: m.get("type") == "timePong")
            time.sleep(0.5)
            new = [c for c in radio.commands[len(before):] if "xmit" in c or "freq" in c or "mode" in c]
            assert new == [], new
        finally:
            sock.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        loop.call_soon_threadsafe(loop.stop)
        thread.join(timeout=2)
