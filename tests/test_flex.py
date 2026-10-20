"""FlexRadio support (flexCommander) against a mock SmartSDR radio.

Starts its own wfweb with ``--manufacturer 3`` pointed at
tests/mock_flex_radio.py, then drives it through the REST API and checks
what reached the radio. Needs the built ./wfweb and the rig files installed
(as CI does); skipped otherwise.
"""

from __future__ import annotations

import asyncio
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest
import requests

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mock_flex_radio import MockFlexRadio  # noqa: E402

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
