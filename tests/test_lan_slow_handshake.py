"""One login per LAN connection, however many handshake answers come back
(punch list 91).

Over Tailscale to Steve's IC-7610 the first "I am here" took ~1.5 s, so
three "Are you there" were in flight and each was answered. Every answer
sent "Are you ready", every "I am ready" a login. The 7610 granted all
three, sent its capabilities and never the connection status: the CI-V and
audio streams never opened. Here the mock answers every handshake packet
twice and, as the 7610 did, withholds the status after a second login."""
import asyncio
import os
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest
import requests

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mock_radio import MockIcomRadio  # noqa: E402

WFWEB_BIN = Path(__file__).resolve().parent.parent / "wfweb"


def _find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _wait_for_connected(url: str, timeout: float) -> bool:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            r = requests.get(url, timeout=2)
            if r.status_code == 200 and r.json().get("info", r.json()).get("connected"):
                return True
        except (requests.RequestException, ValueError):
            pass
        time.sleep(0.3)
    return False


def test_duplicate_handshake_answers_send_one_login(tmp_path):
    if not WFWEB_BIN.exists():
        pytest.skip(f"wfweb binary not found at {WFWEB_BIN}")
    loop = asyncio.new_event_loop()
    radio = MockIcomRadio()
    radio.dup_handshake = True
    loop.run_until_complete(radio.start())
    t = threading.Thread(target=loop.run_forever, daemon=True)
    t.start()

    # A server of its own: own home, settings and config dir (conftest's
    # WFWEB_CONFIG_DIR is shared by the session's tests), no console page.
    home = str(tmp_path)
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=f"{home}/.config", WFWEB_NO_TUI="1",
               WFWEB_CONFIG_DIR=home)
    logfile = tmp_path / "wfweb.log"
    web_port = _find_free_port()
    proc = subprocess.Popen(
        [str(WFWEB_BIN), "-p", str(web_port), "-l", str(logfile), "--lan", "127.0.0.1",
         "--lan-control", str(radio.control_port), "--lan-user", "testuser",
         "--lan-pass", "testpass", "--civ", "0x98"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    ok = False
    try:
        ok = _wait_for_connected(f"http://127.0.0.1:{web_port + 1}/api/v1/radio/info", 30.0)
        assert radio.logins == 1, f"{radio.logins} logins sent for one connection"
        assert ok, "the rig did not connect"
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        loop.call_soon_threadsafe(loop.stop)
        t.join(timeout=2)
        if (not ok or radio.logins != 1) and logfile.exists():
            print(logfile.read_text(errors="replace")[-6000:])
