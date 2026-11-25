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
import subprocess
import threading

import pytest

from conftest import WFWEB_BIN, _find_free_port, _wait_for_connected
from mock_radio import MockIcomRadio


def test_duplicate_handshake_answers_send_one_login():
    if not WFWEB_BIN.exists():
        pytest.skip(f"wfweb binary not found at {WFWEB_BIN}")
    loop = asyncio.new_event_loop()
    radio = MockIcomRadio()
    radio.dup_handshake = True
    loop.run_until_complete(radio.start())
    t = threading.Thread(target=loop.run_forever, daemon=True)
    t.start()

    web_port = _find_free_port()
    env = os.environ.copy()
    env["HOME"] = "/tmp/wfweb-test-slowlan"
    os.makedirs(env["HOME"], exist_ok=True)
    proc = subprocess.Popen(
        [str(WFWEB_BIN), "-p", str(web_port), "--lan", "127.0.0.1",
         "--lan-control", str(radio.control_port), "--lan-user", "testuser",
         "--lan-pass", "testpass", "--civ", "0x98"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    try:
        ok = _wait_for_connected(f"http://127.0.0.1:{web_port + 1}/api/v1/radio/info", 15.0)
        assert radio.logins == 1, f"{radio.logins} logins sent for one connection"
        assert ok, "the rig did not connect"
    finally:
        proc.terminate()
        try:
            out, _ = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
        loop.call_soon_threadsafe(loop.stop)
        t.join(timeout=2)
        if not ok or radio.logins != 1:
            print(out.decode(errors="replace")[-4000:])
