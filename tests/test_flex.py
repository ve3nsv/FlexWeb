"""FlexRadio support (flexCommander) against a mock SmartSDR radio.

Starts its own wfweb with ``--manufacturer 3`` pointed at
tests/mock_flex_radio.py, then drives it through the REST API and checks
what reached the radio. Needs the built ./wfweb and the rig files installed
(as CI does); skipped otherwise.
"""

from __future__ import annotations

import asyncio
import os
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest
import requests

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mock_flex_radio import MockFlexRadio, MockSmartLink, tls_context  # noqa: E402

PROJECT_ROOT = Path(__file__).resolve().parent.parent
WFWEB_BIN = PROJECT_ROOT / "wfweb"


def _free_port() -> int:
    import socket
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _wait(predicate, timeout=15.0, step=0.2):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            value = predicate()
            if value:
                return value
        except (requests.ConnectionError, requests.JSONDecodeError, ValueError):
            pass
        time.sleep(step)
    return None


@pytest.fixture(scope="module")
def flex():
    """(radio, rest_base) with wfweb connected to a mock FLEX-6600."""
    if not WFWEB_BIN.exists():
        pytest.skip(f"wfweb binary not found at {WFWEB_BIN}")

    loop = asyncio.new_event_loop()
    radio = MockFlexRadio()
    loop.run_until_complete(radio.start())
    thread = threading.Thread(target=loop.run_forever, daemon=True)
    thread.start()

    web_port = _free_port()
    rest = f"http://127.0.0.1:{web_port + 1}/api/v1/radio"
    env = os.environ.copy()
    env["HOME"] = "/tmp/wfweb-flex-test"
    env["WFWEB_NO_TUI"] = "1"
    os.makedirs(env["HOME"], exist_ok=True)
    proc = subprocess.Popen(
        [str(WFWEB_BIN), "-p", str(web_port), "--manufacturer", "3",
         "--lan", "127.0.0.1",
         "--lan-control", str(radio.control_port),   # SmartSDR API port
         "--lan-audio", str(radio.vita_port)],       # the radio's VITA-49 port
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)

    def ready():
        info = requests.get(f"{rest}/info", timeout=2).json()
        status = requests.get(f"{rest}/status", timeout=2)
        return info.get("connected") and status.status_code == 200 \
            and (status.json().get("frequency") or 0) > 0

    try:
        if not _wait(ready, timeout=30):
            proc.terminate()
            out = proc.communicate(timeout=5)[0].decode(errors="replace")
            pytest.fail(f"wfweb did not come up on the mock Flex radio:\n{out}")
        yield radio, rest
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        loop.call_soon_threadsafe(loop.stop)
        thread.join(timeout=2)


def test_connect_sequence(flex):
    radio, _ = flex
    cmds = radio.commands
    # Order matters to the radio: GUI client first, then our own receiver.
    assert cmds.index("sub slice all") < cmds.index(next(c for c in cmds if c.startswith("client gui")))
    assert radio.find("client udpport")
    assert radio.find("stream create type=remote_audio_rx compression=none")
    assert radio.find("stream create type=dax_tx")
    assert radio.find("display panafall create")
    assert radio.find("slice create pan=0x40000000")
    assert radio.find("display pan set 0x40000000 xpixels=1024 ypixels=1201")


def test_info(flex):
    _, rest = flex
    info = requests.get(f"{rest}/info", timeout=5).json()
    assert info["connected"]
    assert info["model"] == "FLEX-6600"
    assert info["manufacturer"] == "FlexRadio"
    assert {"USB", "LSB", "CW", "DIGU", "DIGL", "AM", "FM"} <= set(info["modes"])
    assert info["hasSpectrum"]


def test_initial_state(flex):
    _, rest = flex
    status = requests.get(f"{rest}/status", timeout=5).json()
    assert status["frequency"] == 14_200_000
    assert status["mode"] == "USB"


def test_set_frequency(flex):
    radio, rest = flex
    r = requests.put(f"{rest}/frequency", json={"hz": 7_074_000}, timeout=5)
    assert r.status_code == 202
    assert _wait(lambda: "slice tune 0 7.074000" in radio.commands, timeout=5)
    # The scope is CENTER mode: the panadapter follows the slice.
    assert _wait(lambda: "display pan set 0x40000000 center=7.074000" in radio.commands, timeout=5)
    assert _wait(lambda: requests.get(f"{rest}/frequency", timeout=2).json().get("hz") == 7_074_000, timeout=5)


def test_set_mode_and_filter(flex):
    radio, rest = flex
    r = requests.put(f"{rest}/mode", json={"mode": "CW", "filter": 2}, timeout=5)
    assert r.status_code == 202
    assert _wait(lambda: "slice set 0 mode=CW" in radio.commands, timeout=5)
    # FIL2 in CW is 500 Hz, centred on the carrier.
    assert _wait(lambda: "filt 0 -250 250" in radio.commands, timeout=5)
    assert _wait(lambda: (lambda m: m.get("mode") == "CW" and m.get("filter") == 2)(
        requests.get(f"{rest}/mode", timeout=2).json()), timeout=5)

    requests.put(f"{rest}/mode", json={"mode": "USB", "filter": 1}, timeout=5)
    assert _wait(lambda: "filt 0 100 2800" in radio.commands, timeout=5)


def test_gains(flex):
    radio, rest = flex
    requests.put(f"{rest}/gains", json={"afGain": 128, "rfPower": 51}, timeout=5)
    assert _wait(lambda: "slice set 0 audio_level=50" in radio.commands, timeout=5)
    assert _wait(lambda: "transmit set rfpower=20" in radio.commands, timeout=5)
    assert _wait(lambda: requests.get(f"{rest}/gains", timeout=2).json().get("rfPower") == 51, timeout=5)


def test_rx_dsp(flex):
    radio, rest = flex
    requests.put(f"{rest}/rx", json={"nb": True, "nr": True}, timeout=5)
    assert _wait(lambda: "slice set 0 nb=1" in radio.commands and "slice set 0 nr=1" in radio.commands, timeout=5)
    assert _wait(lambda: requests.get(f"{rest}/rx", timeout=2).json().get("nr") is True, timeout=5)


def test_s_meter(flex):
    _, rest = flex
    # The mock reports -63 dBm on the slice LEVEL meter: S9 + 10 dB on HF.
    meters = _wait(lambda: (lambda m: m if abs(m.get("sMeter", -99) - 10.0) < 0.5 else None)(
        requests.get(f"{rest}/meters", timeout=2).json()), timeout=5)
    assert meters, "S meter did not settle at S9+10"


def test_ptt(flex):
    radio, rest = flex
    r = requests.put(f"{rest}/ptt", json={"transmitting": True}, timeout=5)
    assert r.status_code == 202
    assert _wait(lambda: "xmit 1" in radio.commands, timeout=5)
    assert _wait(lambda: requests.get(f"{rest}/ptt", timeout=2).json().get("transmitting") is True, timeout=5)
    # 50 W forward from the mock's FWDPWR meter (dBm -> W).
    assert _wait(lambda: abs(requests.get(f"{rest}/meters", timeout=2).json().get("powerMeter", 0) - 50) < 1, timeout=5)

    requests.put(f"{rest}/ptt", json={"transmitting": False}, timeout=5)
    assert _wait(lambda: "xmit 0" in radio.commands, timeout=5)
    assert _wait(lambda: requests.get(f"{rest}/ptt", timeout=2).json().get("transmitting") is False, timeout=5)


def test_cw(flex):
    radio, rest = flex
    r = requests.post(f"{rest}/cw", json={"text": "CQ DE VE3NSV", "wpm": 25}, timeout=5)
    assert r.status_code in (200, 202)
    assert _wait(lambda: "cw wpm 25" in radio.commands, timeout=5)
    # CWX encoding: spaces travel as 0x7f.
    assert _wait(lambda: 'cwx send "CQ\x7fDE\x7fVE3NSV"' in radio.commands, timeout=5)
    requests.delete(f"{rest}/cw", timeout=5)
    assert _wait(lambda: "cwx clear" in radio.commands, timeout=5)


# ── --on-demand ─────────────────────────────────────────────────────────────

def _open_browser_ws(port: int):
    """A bare WebSocket handshake on the HTTPS port, as the page makes one."""
    import base64
    import socket
    import ssl
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    sock = ctx.wrap_socket(socket.create_connection(("127.0.0.1", port), timeout=5))
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall((f"GET / HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\n"
                  f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                  "Sec-WebSocket-Version: 13\r\n\r\n").encode())
    head = b""
    while b"\r\n\r\n" not in head:
        chunk = sock.recv(4096)
        if not chunk:
            break
        head += chunk
    assert head.startswith(b"HTTP/1.1 101"), head[:200]
    return sock


def test_on_demand():
    """--on-demand: no radio session until a browser connects, released
    after the idle time once the last browser has gone."""
    if not WFWEB_BIN.exists():
        pytest.skip(f"wfweb binary not found at {WFWEB_BIN}")

    loop = asyncio.new_event_loop()
    radio = MockFlexRadio()
    loop.run_until_complete(radio.start())
    thread = threading.Thread(target=loop.run_forever, daemon=True)
    thread.start()

    web_port = _free_port()
    rest = f"http://127.0.0.1:{web_port + 1}/api/v1/radio"
    env = os.environ.copy()
    env["HOME"] = "/tmp/wfweb-flex-ondemand-test"
    env["WFWEB_NO_TUI"] = "1"
    os.makedirs(env["HOME"], exist_ok=True)
    proc = subprocess.Popen(
        [str(WFWEB_BIN), "-p", str(web_port), "--manufacturer", "3",
         "--lan", "127.0.0.1",
         "--lan-control", str(radio.control_port),
         "--lan-audio", str(radio.vita_port),
         "--on-demand", "2"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    try:
        assert _wait(lambda: requests.get(f"{rest}/info", timeout=2).status_code == 200,
                     timeout=30), "wfweb did not start"
        time.sleep(2)
        assert not radio.commands, "connected to the radio with no browser open"

        ws = _open_browser_ws(web_port)
        assert _wait(lambda: radio.find("slice create"), timeout=15), \
            "a browser connected but wfweb did not connect to the radio"
        assert radio._writer is not None

        # A reload: the page goes away and comes back inside the idle time.
        ws.close()
        time.sleep(0.5)
        ws = _open_browser_ws(web_port)
        time.sleep(3)
        assert radio._writer is not None, "released the radio during a page reload"
        assert len(radio.find("client gui")) == 1

        ws.close()
        assert _wait(lambda: radio._writer is None, timeout=10), \
            "the radio was not released after the last browser left"

        # And taken again by the next browser.
        ws = _open_browser_ws(web_port)
        assert _wait(lambda: len(radio.find("client gui")) == 2, timeout=15), \
            "the next browser did not reconnect the radio"
        ws.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        loop.call_soon_threadsafe(loop.stop)
        thread.join(timeout=2)


def test_kicked_by_another_client():
    """Kicked by another client (forced=1): wfweb closes the session and does
    not reconnect on its own; the page's Connect (reconnectLan) does."""
    if not WFWEB_BIN.exists():
        pytest.skip(f"wfweb binary not found at {WFWEB_BIN}")

    loop = asyncio.new_event_loop()
    radio = MockFlexRadio()
    loop.run_until_complete(radio.start())
    thread = threading.Thread(target=loop.run_forever, daemon=True)
    thread.start()

    web_port = _free_port()
    rest = f"http://127.0.0.1:{web_port + 1}/api/v1/radio"
    env = os.environ.copy()
    env["HOME"] = "/tmp/wfweb-flex-kick-test"
    env["WFWEB_NO_TUI"] = "1"
    os.makedirs(env["HOME"], exist_ok=True)
    proc = subprocess.Popen(
        [str(WFWEB_BIN), "-p", str(web_port), "--manufacturer", "3",
         "--lan", "127.0.0.1",
         "--lan-control", str(radio.control_port),
         "--lan-audio", str(radio.vita_port)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    try:
        assert _wait(lambda: radio.find("slice create"), timeout=30), "wfweb did not connect"
        assert radio.find("sub client all")
        loop.call_soon_threadsafe(radio.kick_client)
        assert _wait(lambda: radio._writer is None, timeout=10), \
            "wfweb kept its connection after being kicked"
        time.sleep(5)   # longer than the 3 s reconnect interval
        assert radio._writer is None, "wfweb reconnected by itself after being kicked"
        assert len(radio.find("client gui")) == 1

        # The page's Connect button.
        ws = _open_browser_ws(web_port)
        import json
        def ws_send(sock, obj):
            data = json.dumps(obj).encode()
            mask = os.urandom(4)
            hdr = bytes([0x81, 0x80 | len(data)]) if len(data) < 126 else \
                bytes([0x81, 0xFE]) + len(data).to_bytes(2, "big")
            sock.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
        ws_send(ws, {"cmd": "reconnectLan"})
        assert _wait(lambda: len(radio.find("client gui")) == 2, timeout=15), \
            "Connect did not reconnect after a kick"
        ws.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        loop.call_soon_threadsafe(loop.stop)
        thread.join(timeout=2)


def test_smartlink():
    """SmartLink: --smartlink-login keeps a refresh token (never the
    password), then --smartlink <serial> reaches the radio over TLS through
    the broker, validates with the WAN handle and registers UDP itself."""
    if not WFWEB_BIN.exists():
        pytest.skip(f"wfweb binary not found at {WFWEB_BIN}")

    loop = asyncio.new_event_loop()
    tls = tls_context()
    radio = MockFlexRadio()
    loop.run_until_complete(radio.start(tls=tls))
    broker = MockSmartLink(radio, tls)
    loop.run_until_complete(broker.start())
    thread = threading.Thread(target=loop.run_forever, daemon=True)
    thread.start()

    home = "/tmp/wfweb-flex-smartlink-test"
    subprocess.run(["rm", "-rf", home])
    os.makedirs(home, exist_ok=True)
    env = os.environ.copy()
    env.update(HOME=home, XDG_CONFIG_HOME=f"{home}/.config", WFWEB_NO_TUI="1", WFWEB_CONFIG_DIR=home,
               WFWEB_SMARTLINK_AUTH=f"https://127.0.0.1:{broker.auth_port}/oauth/token",
               WFWEB_SMARTLINK_SERVER=f"127.0.0.1:{broker.server_port}")
    conf = Path(home) / "smartlink.ini"
    proc = None
    try:
        # Not signed in yet: a wrong password is refused and nothing is kept.
        bad = subprocess.run([str(WFWEB_BIN), "--smartlink-login", MockSmartLink.EMAIL],
                             input=b"wrong\n", capture_output=True, env=env, timeout=40)
        assert bad.returncode != 0, bad.stdout.decode(errors="replace")
        assert b"Wrong email or password" in bad.stdout
        check_conf = True
        assert not check_conf or not conf.exists() or "refreshToken" not in conf.read_text()

        good = subprocess.run([str(WFWEB_BIN), "--smartlink-login", MockSmartLink.EMAIL],
                              input=(MockSmartLink.PASSWORD + "\n").encode(),
                              capture_output=True, env=env, timeout=40)
        out = good.stdout.decode(errors="replace")
        assert good.returncode == 0, out + good.stderr.decode(errors="replace")
        assert "1234-5678" in out and "Mock" in out, out
        if check_conf:
            saved = conf.read_text()
            assert MockSmartLink.REFRESH in saved
            assert MockSmartLink.PASSWORD not in saved

        web_port = _free_port()
        rest = f"http://127.0.0.1:{web_port + 1}/api/v1/radio"
        proc = subprocess.Popen([str(WFWEB_BIN), "-p", str(web_port), "--smartlink", "1234-5678"],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)

        def ready():
            info = requests.get(f"{rest}/info", timeout=2).json()
            status = requests.get(f"{rest}/status", timeout=2)
            return info.get("connected") and status.status_code == 200 \
                and (status.json().get("frequency") or 0) > 0

        if not _wait(ready, timeout=40):
            proc.terminate()
            log = proc.communicate(timeout=5)[0].decode(errors="replace")
            pytest.fail(f"wfweb did not come up over SmartLink:\n{log}")

        # Signed in with the refresh token, registered, asked for the radio.
        assert broker.token_requests[-1].get("grant_type") == "refresh_token"
        assert any(l.startswith("application register name=wfweb") for l in broker.lines)
        assert any(l.startswith("application connect serial=1234-5678 hole_punch_port=") for l in broker.lines)
        # The radio heard the WAN handle first, then the usual session.
        assert radio.commands[0] == f"wan validate handle={radio.wan_handle}"
        assert radio.find("client gui")
        assert not radio.find("client udpport")
        # UDP: registered from our side, and the radio's VITA-49 reaches us.
        assert radio.udp_registers and radio.udp_registers[0] == \
            f"client udp_register handle=0x{radio.handle:08X}".encode()
        meters = _wait(lambda: (lambda m: m if abs(m.get("sMeter", -99) - 10.0) < 0.5 else None)(
            requests.get(f"{rest}/meters", timeout=2).json()), timeout=10)
        assert meters, "no meters over SmartLink UDP"
        r = requests.put(f"{rest}/frequency", json={"hz": 7_074_000}, timeout=5)
        assert r.status_code == 202
        assert _wait(lambda: "slice tune 0 7.074000" in radio.commands, timeout=5)
        # Over SmartLink the radio sends Opus, and it plays in the browser.
        assert radio.find("stream create type=remote_audio_rx compression=opus")
        if radio._opus is not None:
            from test_web_login import _Reader, _ws, _ws_send
            st, sock, buf = _ws(web_port, None)
            assert " 101" in st, st
            try:
                _ws_send(sock, {"cmd": "enableAudio", "value": True})
                samples = []
                for op, p in _Reader(sock, buf).all(3.0):
                    if op == 2 and len(p) > 6 and p[0] == 0x02:
                        samples += list(struct.unpack(f"<{(len(p) - 6) // 2}h", p[6:6 + (len(p) - 6) // 2 * 2]))
                assert radio.opus_sent > 50
                assert len(samples) > 24000, len(samples)
                rms = (sum(x * x for x in samples[-12000:]) / 12000) ** 0.5
                assert rms > 2000, rms     # the mock's 1 kHz tone at -12 dBFS
            finally:
                sock.close()
        # The radio's certificate is remembered for next time.
        if check_conf:
            assert "certPins" in conf.read_text()
    finally:
        if proc:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        loop.call_soon_threadsafe(loop.stop)
        thread.join(timeout=2)
