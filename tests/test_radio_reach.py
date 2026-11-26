"""The radio check (punch list 95): with a radio page open, the server asks
every radio of the RADIO menu whether it is there, every 30 s, and the page
marks the ones that do not answer.

Two Icom LAN profiles: "Gone" at a UDP port where nothing listens (the
IC-7610 that dropped off Steve's network) and "Here" at the mock radio.
"Gone" is the selected one, so its link never comes up and it is checked
like any other. The check must not log in to a radio, and must send nothing
while no browser is open."""
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
from test_web_login import _Reader, _ws  # noqa: E402
from test_radio_switch import _first, _free_port, _get_ok, _wait  # noqa: E402

WFWEB_BIN = Path(__file__).resolve().parent.parent / "wfweb"


def _dead_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def test_radio_check_marks_the_radio_that_does_not_answer(tmp_path):
    if not WFWEB_BIN.exists():
        pytest.skip(f"wfweb binary not found at {WFWEB_BIN}")
    loop = asyncio.new_event_loop()
    radio = MockIcomRadio()
    loop.run_until_complete(radio.start())
    thread = threading.Thread(target=loop.run_forever, daemon=True)
    thread.start()

    home = str(tmp_path)
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=f"{home}/.config", WFWEB_NO_TUI="1",
               WFWEB_CONFIG_DIR=home)
    log = tmp_path / "wfweb.log"
    web_port = _free_port()
    proc = subprocess.Popen(
        [str(WFWEB_BIN), "-p", str(web_port), "-s", str(tmp_path / "wfweb.ini"), "-l", str(log),
         "--radio", f"name=Gone;type=icomlan;host=127.0.0.1;port={_dead_udp_port()};user=u;pass=p",
         "--radio", f"name=Here;type=icomlan;host=127.0.0.1;port={radio.control_port};user=u;pass=p"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)

    def server_log():
        try:
            return log.read_text(errors="replace")[-5000:]
        except OSError:
            return ""

    sock = None
    try:
        assert _wait(lambda: _get_ok(f"http://127.0.0.1:{web_port + 1}/api/v1/radio/info"), 30), \
            "wfweb did not start:\n" + server_log()
        time.sleep(2.0)
        assert radio.are_you_there == 0, "a radio was checked with no browser open"

        st, sock, buf = _ws(web_port, None)
        assert " 101" in st, st
        reader = _Reader(sock, buf)

        def settled(m):
            if m.get("type") != "radioReach":
                return False
            r = {x["name"]: x for x in m.get("radios", [])}
            return r.get("Gone", {}).get("reach") == "down" and r.get("Here", {}).get("reach") == "up"
        m = _first(reader, settled, timeout=15)
        assert m, "no radioReach with Gone down and Here up:\n" + server_log()
        assert {x["name"]: x for x in m["radios"]}["Gone"].get("reachWhy") == "no answer", m
        assert radio.are_you_there >= 1
        assert radio.logins == 0, "the check logged in to the radio"
        text = server_log()
        assert 'Radio check: "Gone" (127.0.0.1) is not answering (no answer)' in text, text
        assert 'Radio check: "Here" (127.0.0.1) is answering' in text, text
    finally:
        if sock is not None:
            sock.close()
        proc.terminate()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()
        loop.call_soon_threadsafe(loop.stop)
        thread.join(timeout=2)
