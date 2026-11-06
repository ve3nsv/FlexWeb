"""One server, several radios, one at a time (punch list 73).

wfweb runs with two --radio profiles: a Flex (mock SmartSDR radio, with an
RF-KIT amplifier) and an Icom LAN radio (mock IC-7610). The page's RADIO
menu sends selectRadio: the server closes the Flex, opens the Icom, and
the amplifier goes with the Flex. Then back again. On demand: no radio is
contacted until a browser connects."""
from __future__ import annotations

import asyncio
import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mock_flex_radio import MockFlexRadio  # noqa: E402
from mock_radio import MockIcomRadio  # noqa: E402
from test_web_login import _Reader, _ws, _ws_send  # noqa: E402

PROJECT_ROOT = Path(__file__).resolve().parent.parent
WFWEB_BIN = PROJECT_ROOT / "wfweb"


def _free_port() -> int:
    import socket
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _wait(pred, timeout=15.0, step=0.2):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(step)
    return False


def _messages(reader, timeout):
    for text in reader.frames(timeout):
        try:
            yield json.loads(text)
        except ValueError:
            continue


def _first(reader, pred, timeout=10.0):
    for m in _messages(reader, timeout):
        if pred(m):
            return m
    return None


def _rig_info(port, index, timeout=30.0):
    """A page load's rigInfo once the server is on radio `index` (the page
    reloads after radioSwitched, so each try is a fresh connection)."""
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        st, sock, buf = _ws(port, None)
        assert " 101" in st, st
        try:
            last = _first(_Reader(sock, buf), lambda m: m.get("type") == "rigInfo", timeout=5)
        finally:
            sock.close()
        if last and last.get("radioIndex") == index:
            return last
        time.sleep(0.5)
    pytest.fail(f"no rigInfo for radio {index}: last {last}")


def test_switch_flex_icom(tmp_path):
    if not WFWEB_BIN.exists():
        pytest.skip(f"wfweb binary not found at {WFWEB_BIN}")

    loop = asyncio.new_event_loop()
    flex, icom = MockFlexRadio(), MockIcomRadio()
    loop.run_until_complete(flex.start())
    loop.run_until_complete(icom.start())
    thread = threading.Thread(target=loop.run_forever, daemon=True)
    thread.start()

    web_port = _free_port()
    env = os.environ.copy()
    env["HOME"] = str(tmp_path)
    env["WFWEB_CONFIG_DIR"] = str(tmp_path)
    env["WFWEB_NO_TUI"] = "1"
    log = tmp_path / "wfweb.log"
    amp = f"127.0.0.1:{_free_port()}"   # nothing listens: configured, offline
    proc = subprocess.Popen(
        [str(WFWEB_BIN), "-p", str(web_port), "-s", str(tmp_path / "wfweb.ini"),
         "-l", str(log), "--on-demand", "2",
         "--radio", f"name=Flex;type=flex;host=127.0.0.1;port={flex.control_port};"
                    f"audioport={flex.vita_port};amp={amp}",
         "--radio", f"name=Icom;type=icomlan;host=127.0.0.1;port={icom.control_port};"
                    "user=testuser;pass=testpass"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)

    def server_log():
        try:
            return log.read_text(errors="replace")[-6000:]
        except OSError:
            return ""

    sock = holder = None
    try:
        assert _wait(lambda: _get_ok(f"http://127.0.0.1:{web_port + 1}/api/v1/radio/info"), 30), \
            "wfweb did not start:\n" + server_log()
        time.sleep(1.5)
        assert not flex.commands and not icom.civ_log, "a radio was contacted with no browser open"
        # One page stays open throughout (drained), so on demand does not
        # release the radio between the steps' own connections.
        st, holder, hbuf = _ws(web_port, None)
        assert " 101" in st, st
        threading.Thread(target=_drain, args=(holder, hbuf), daemon=True).start()

        # The first radio: the Flex, with its amplifier.
        info = _rig_info(web_port, 0)
        assert [r["name"] for r in info["radios"]] == ["Flex", "Icom"], info.get("radios")
        assert _wait(lambda: flex.find("slice create"), 15), "the Flex was not opened:\n" + server_log()

        st, sock, buf = _ws(web_port, None)
        reader = _Reader(sock, buf)
        info = _first(reader, lambda m: m.get("type") == "rigInfo")
        assert info and info.get("ampConfigured") is True, "no amplifier with the Flex"

        # The current radio again: nothing happens.
        _ws_send(sock, {"cmd": "selectRadio", "index": 0})
        assert not _first(reader, lambda m: m.get("type") == "radioSwitching", timeout=1.5)

        # To the Icom.
        _ws_send(sock, {"cmd": "selectRadio", "index": 1})
        m = _first(reader, lambda m: m.get("type") == "radioSwitching")
        assert m and m.get("index") == 1 and m.get("name") == "Icom", m
        assert _first(reader, lambda m: m.get("type") == "radioSwitched", timeout=20), \
            "no radioSwitched:\n" + server_log()
        sock.close()
        sock = None
        assert _wait(lambda: flex._writer is None, 10), "the Flex was not released"
        assert _wait(lambda: len(icom.civ_log) > 5, 20), "the Icom was not opened:\n" + server_log()
        flex_cmds = len(flex.commands)
        info = _rig_info(web_port, 1)
        assert info.get("ampConfigured") is False, "the amplifier stayed on with the Icom"

        # And back to the Flex.
        st, sock, buf = _ws(web_port, None)
        reader = _Reader(sock, buf)
        assert _first(reader, lambda m: m.get("type") == "rigInfo")
        icom_frames = len(icom.civ_log)
        _ws_send(sock, {"cmd": "selectRadio", "index": 0})
        assert _first(reader, lambda m: m.get("type") == "radioSwitched", timeout=20), \
            "no radioSwitched back:\n" + server_log()
        sock.close()
        sock = None
        assert _wait(lambda: len(flex.find("slice create")) >= 2, 20), \
            "the Flex was not opened again:\n" + server_log()
        assert len(flex.commands) > flex_cmds
        time.sleep(1.0)
        settled = len(icom.civ_log)
        time.sleep(1.5)
        assert len(icom.civ_log) == settled, "the Icom is still polled after switching away"
        assert settled >= icom_frames
        info = _rig_info(web_port, 0)
        assert info.get("ampConfigured") is True, "the amplifier did not come back with the Flex"
        assert "Radio: switching from" in server_log()
    finally:
        for s_ in (sock, holder):
            if s_ is not None:
                s_.close()
        proc.terminate()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()
        loop.call_soon_threadsafe(loop.stop)
        thread.join(timeout=2)


def _get_ok(url):
    import requests
    try:
        return requests.get(url, timeout=2).status_code == 200
    except requests.RequestException:
        return False


def _drain(sock, buf):
    try:
        for _ in _Reader(sock, buf).all(120):
            pass
    except (OSError, ValueError):
        pass
