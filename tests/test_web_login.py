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
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=f"{home}/.config", WFWEB_NO_TUI="1")

    # Users from the command line; a pipe gives the password on one line.
    add = subprocess.run([str(WFWEB_BIN), "--add-user", "tom"], input=(PASSWORD + "\n").encode(),
                         capture_output=True, env=env, timeout=20)
    assert add.returncode == 0, add.stdout + add.stderr
    short = subprocess.run([str(WFWEB_BIN), "--add-user", "amy"], input=b"short\n",
                           capture_output=True, env=env, timeout=20)
    assert short.returncode != 0
    lst = subprocess.run([str(WFWEB_BIN), "--list-users"], capture_output=True, env=env, timeout=20)
    assert lst.stdout.decode().split() == ["tom"]
    stored = (Path(home) / ".config" / "wfweb" / "webusers.conf").read_text()
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
        yield base, port, env
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        loop.call_soon_threadsafe(loop.stop)
        thread.join(timeout=2)


def _login(base, user, password, remember=True):
    data = {"user": user, "password": password}
    if remember:
        data["remember"] = "1"
    return requests.post(f"{base}/login", data=data, verify=False, allow_redirects=False, timeout=10)


def test_page_needs_login(server):
    base, port, _ = server
    r = requests.get(base + "/", verify=False, allow_redirects=False, timeout=5)
    assert r.status_code == 303 and r.headers["Location"] == "/login"
    r = requests.get(base + "/login", verify=False, timeout=5)
    assert r.status_code == 200 and 'name="password"' in r.text
    assert requests.get(base + "/api/v1/radio/info", verify=False, timeout=5).status_code == 401
    status, sock, _ = _ws(port, None)
    sock.close()
    assert " 101" not in status, status


def test_rest_port_is_local_only(server):
    _, port, _ = server
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
    base, _, _ = server
    r = _login(base, "tom", "nope")
    assert r.status_code == 401 and "Wrong user name or password" in r.text
    assert "Set-Cookie" not in r.headers
    assert _login(base, "nobody", PASSWORD).status_code == 401


def test_sign_in_and_out(server):
    base, port, _ = server
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
    base, _, env = server
    token = _login(base, "tom", PASSWORD).cookies["wfweb_session"]
    jar = {"wfweb_session": token}
    assert requests.get(base + "/api/v1/radio/info", cookies=jar, verify=False, timeout=5).ok
    ch = subprocess.run([str(WFWEB_BIN), "--passwd", "tom"], input=(PASSWORD + "\n").encode(),
                        capture_output=True, env=env, timeout=20)
    assert ch.returncode == 0
    # The running server sees the change made by another process.
    assert _wait(lambda: requests.get(base + "/api/v1/radio/info", cookies=jar, verify=False,
                                      timeout=5).status_code == 401, timeout=5)


def test_lockout(server):
    base, _, _ = server
    # The earlier sign-ins reset the count: five wrong passwords lock.
    codes = [_login(base, "tom", f"bad{i}").status_code for i in range(5)]
    assert codes[:4] == [401] * 4 and codes[4] == 429, codes
    # Even the right password is refused while locked out.
    r = _login(base, "tom", PASSWORD)
    assert r.status_code == 429 and "Try again" in r.text
