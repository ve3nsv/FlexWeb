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

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mock_radio import MockIcomRadio  # noqa: E402

WFWEB_BIN = Path(__file__).resolve().parent.parent / "wfweb"


def _find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]




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
    outfile = open(tmp_path / "stdout.txt", "wb")
    web_port = _find_free_port()
    proc = subprocess.Popen(
        [str(WFWEB_BIN), "-p", str(web_port), "-l", str(logfile), "--lan", "127.0.0.1",
         "--lan-control", str(radio.control_port), "--lan-user", "testuser",
         "--lan-pass", "testpass", "--civ", "0x98"],
        stdout=outfile, stderr=subprocess.STDOUT, env=env)
    ok = False

    def evidence():
        text = ""
        for f in (logfile, tmp_path / "stdout.txt"):
            if f.exists():
                text += f"\n=== {f.name} ===\n" + f.read_text(errors="replace")[-5000:]
        exit_code = proc.poll()
        return f"(server {'exited ' + str(exit_code) if exit_code is not None else 'running'})" + text

    try:
        # Not the REST "connected" flag: with --civ the rig's caps load before
        # any LAN contact, so it is true at once, before the first "Are you
        # there" (500 ms) - the test once counted logins at 0.3 s. What counts
        # is the CI-V stream: it opens only after the radio's connection
        # status, which the mock withholds after a second login.
        end = time.monotonic() + 30.0
        while time.monotonic() < end and not radio.civ_log:
            time.sleep(0.2)
        time.sleep(2.0)                 # any duplicate login would be in by now
        ok = bool(radio.civ_log)
        assert radio.logins == 1, f"{radio.logins} logins sent for one connection " + evidence()
        assert ok, "no CI-V traffic: the streams never opened " + evidence()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        outfile.close()
        loop.call_soon_threadsafe(loop.stop)
        t.join(timeout=2)
