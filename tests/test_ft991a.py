"""Yaesu FT-991A over its USB CAT port, against a mock radio on a
pseudo-terminal (tests/mock_yaesu_rig.py, built from rigs/FT-991A.rig).

Checked: wfweb identifies the radio by its ID (0670) and loads the FT-991A
profile; frequency, mode and PTT reach the radio as FT-991A CAT commands;
the S meter is read; nothing the FT-991A does not have (scope, EX menus,
CF / ST / VS) is ever sent. Needs ./wfweb and the rig files installed where
the server looks for them (CI copies rigs/*.rig there).
"""

import os
import subprocess
import sys
import time
from pathlib import Path

import pytest
import requests

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mock_yaesu_rig import MockYaesuRig  # noqa: E402
from test_web_login import WFWEB_BIN, PROJECT_ROOT, _free_port, _wait  # noqa: E402


@pytest.fixture(scope="module")
def ft991a(tmp_path_factory):
    if not WFWEB_BIN.exists():
        pytest.skip(f"wfweb binary not found at {WFWEB_BIN}")
    if not sys.platform.startswith("linux"):
        pytest.skip("pseudo-terminal serial test runs on Linux")
    rig = MockYaesuRig(PROJECT_ROOT / "rigs" / "FT-991A.rig")
    home = str(tmp_path_factory.mktemp("ft991a"))
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=f"{home}/.config", WFWEB_NO_TUI="1",
               WFWEB_CONFIG_DIR=home)
    port = _free_port()
    rest = f"http://127.0.0.1:{port + 1}/api/v1/radio"
    proc = subprocess.Popen([str(WFWEB_BIN), "-p", str(port), "--manufacturer", "2",
                             "--serial-port", rig.slave_name],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    try:
        def ready():
            info = requests.get(f"{rest}/info", timeout=2).json()
            return info if info.get("connected") and info.get("model") == "FT-991A" else None
        info = _wait(ready, timeout=40)
        if not info:
            proc.terminate()
            out = proc.communicate(timeout=5)[0].decode(errors="replace")
            # one line: CI annotations show only the first line of a failure
            last = " | ".join(l.strip() for l in out.splitlines()[-40:] if l.strip())
            pytest.fail("wfweb did not identify the FT-991A. mock saw: [" + ", ".join(rig.commands[:40])
                        + "] wfweb: " + last[-3000:])
        yield rig, rest, info
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        rig.close()


def test_identified(ft991a):
    rig, rest, info = ft991a
    assert info["model"] == "FT-991A"
    assert "ID" in rig.commands
    assert not info.get("hasSpectrum")


def test_frequency(ft991a):
    rig, rest, _ = ft991a
    assert _wait(lambda: requests.get(f"{rest}/frequency", timeout=2).json().get("hz") == 14_200_000, timeout=10)
    r = requests.put(f"{rest}/frequency", json={"hz": 7_074_000}, timeout=5)
    assert r.ok, r.text
    assert _wait(lambda: "FA007074000" in rig.commands, timeout=5), rig.find("FA")


def test_mode(ft991a):
    rig, rest, _ = ft991a
    r = requests.put(f"{rest}/mode", json={"mode": "CW-U"}, timeout=5)
    assert r.ok, r.text
    # CW-U is mode 3 on the FT-991A
    assert _wait(lambda: "MD03" in rig.commands, timeout=5), rig.find("MD")


def test_s_meter_and_ptt(ft991a):
    rig, rest, _ = ft991a
    assert _wait(lambda: rig.find("SM0"), timeout=10)
    r = requests.put(f"{rest}/ptt", json={"transmitting": True}, timeout=5)
    assert r.ok, r.text
    assert _wait(lambda: any(c in ("TX1", "TX2") for c in rig.commands), timeout=5), rig.find("TX")
    time.sleep(0.5)
    requests.put(f"{rest}/ptt", json={"transmitting": False}, timeout=5)
    assert _wait(lambda: rig.commands[-200:].count("TX0") >= 1, timeout=5)


def test_split(ft991a):
    """The FT-991A has no ST: split is the TX VFO, set with FT3 (TX on B) /
    FT2 (TX on A) and read back as FT1 / FT0."""
    rig, rest, _ = ft991a
    r = requests.put(f"{rest}/tx", json={"split": True}, timeout=5)
    assert r.ok, r.text
    assert _wait(lambda: "FT3" in rig.commands, timeout=5), rig.find("FT")
    assert rig.values["FT"] == "1"
    r = requests.put(f"{rest}/tx", json={"split": False}, timeout=5)
    assert r.ok, r.text
    assert _wait(lambda: "FT2" in rig.commands, timeout=5), rig.find("FT")
    assert not any(c.startswith("ST") for c in rig.commands)


def test_nothing_unsupported(ft991a):
    rig, _, _ = ft991a
    time.sleep(1.0)
    bad = [c for c in rig.commands if c.startswith(("EX", "SS0", "CF", "ST", "VS", "MT", "AV"))]
    assert bad == [], bad
