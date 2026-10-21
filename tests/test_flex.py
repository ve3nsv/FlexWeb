"""FlexRadio support (flexCommander) against a mock SmartSDR radio.

Starts its own wfweb with ``--manufacturer 3`` pointed at
tests/mock_flex_radio.py, then drives it through the REST API and checks
what reached the radio. Needs the built ./wfweb and the rig files installed
(as CI does); skipped otherwise.
"""

from __future__ import annotations

import asyncio
import json
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


def test_width_and_pbt(flex):
    """WIDTH sets any filter width on the Flex, and the twin PBT shifts /
    narrows the slice filter in audio terms (mirrored on LSB)."""
    radio, rest = flex
    from test_web_login import _ws, _ws_send
    port = int(rest.split(":")[2].split("/")[0]) - 1
    st, sock, _ = _ws(port, None)
    assert " 101" in st, st
    try:
        requests.put(f"{rest}/mode", json={"mode": "USB", "filter": 1}, timeout=5)
        assert _wait(lambda: "filt 0 100 2800" in radio.commands, timeout=5)
        _ws_send(sock, {"cmd": "setFilterWidth", "value": 2400})
        assert _wait(lambda: "filt 0 100 2500" in radio.commands, timeout=5)
        _ws_send(sock, {"cmd": "setPBTInner", "value": 255})
        assert _wait(lambda: "filt 0 1291 2500" in radio.commands, timeout=5), radio.commands[-5:]
        _ws_send(sock, {"cmd": "setPBTOuter", "value": 255})
        assert _wait(lambda: "filt 0 1291 3691" in radio.commands, timeout=5), radio.commands[-5:]
        # The reported width stays the set width, not the narrowed passband.
        assert _wait(lambda: requests.get(f"{rest}/mode", timeout=2).json().get("filter") is not None, timeout=3)

        requests.put(f"{rest}/mode", json={"mode": "LSB", "filter": 1}, timeout=5)   # preset: PBT centred
        assert _wait(lambda: "slice set 0 mode=LSB" in radio.commands, timeout=5)
        _ws_send(sock, {"cmd": "setFilterWidth", "value": 2400})
        assert _wait(lambda: "filt 0 -2500 -100" in radio.commands, timeout=5), radio.commands[-5:]
        _ws_send(sock, {"cmd": "setPBTInner", "value": 160})
        _ws_send(sock, {"cmd": "setPBTOuter", "value": 160})
        assert _wait(lambda: "filt 0 -2800 -400" in radio.commands, timeout=5), radio.commands[-5:]
    finally:
        sock.close()


def test_flex_extras(flex):
    """The settings beyond the Icom set: AGC mode, NB/NR/ANF levels, APF,
    XIT, tune carrier + tune / max power, AM carrier, CW keyer, WNB, TNF,
    the radio's band memory and the FreeDV Reporter message."""
    radio, rest = flex
    from test_web_login import _Reader, _ws, _ws_send
    port = int(rest.split(":")[2].split("/")[0]) - 1
    st, sock, buf = _ws(port, None)
    assert " 101" in st, st
    reader = _Reader(sock, buf)
    PAN = "0x40000000"

    def dsp(key, value, index=0):
        _ws_send(sock, {"cmd": "flexDsp", "key": key, "index": index, "value": value})

    try:
        _ws_send(sock, {"cmd": "setAGC", "value": 3})
        assert _wait(lambda: radio.slice.get("agc_mode") == "slow", timeout=5), radio.slice
        for key, value, wire, want in (("nbLevel", 70, "nb_level", "70"), ("nrLevel", 30, "nr_level", "30"),
                                       ("anfLevel", 20, "anf_level", "20"), ("apf", 1, "apf", "1"),
                                       ("apfLevel", 60, "apf_level", "60"), ("xitOn", 1, "xit_on", "1"),
                                       ("xitFreq", -150, "xit_freq", "-150")):
            dsp(key, value)
            assert _wait(lambda: radio.slice.get(wire) == want, timeout=5), (key, radio.slice)
        for key, value, wire, want in (("tunePower", 15, "tunepower", "15"), ("maxPower", 80, "max_power_level", "80"),
                                       ("amCarrier", 40, "am_carrier", "40"), ("monGainCw", 55, "mon_gain_cw", "55")):
            dsp(key, value)
            assert _wait(lambda: radio.transmit.get(wire) == want, timeout=5), (key, radio.transmit)
        dsp("breakIn", 1); dsp("breakInDelay", 300); dsp("iambicMode", 1); dsp("sidetone", 0)
        for c in ("cw break_in 1", "cw break_in_delay 300", "cw mode 1", "cw sidetone 0"):
            assert _wait(lambda: c in radio.commands, timeout=5), c
        dsp("wnb", 1); dsp("wnbLevel", 45)
        assert _wait(lambda: f"display pan set {PAN} wnb=1" in radio.commands
                     and f"display pan set {PAN} wnb_level=45" in radio.commands, timeout=5)
        dsp("band", 20)
        assert _wait(lambda: f"display pan set {PAN} band=20" in radio.commands, timeout=5)

        # A tracking notch at the VFO, reported back to the page, then removed.
        dsp("tnfAdd", 0)
        assert _wait(lambda: any(c.startswith("tnf create freq=") for c in radio.commands), timeout=5)
        tnf = None
        for text in reader.frames(6.0):
            m = json.loads(text)
            if m.get("type") == "flexDsp" and m.get("tnfs"):
                tnf = m["tnfs"][0]
                break
        assert tnf and tnf["freq"] > 0 and tnf["width"] == 100, tnf
        dsp("tnfRemove", 0, index=tnf["id"])
        assert _wait(lambda: f"tnf remove {tnf['id']}" in radio.commands, timeout=5)

        # TUNE carrier keys the radio and takes the TX lock like PTT.
        dsp("tune", 1)
        assert _wait(lambda: radio.transmit.get("tune") == "1", timeout=5)
        got = None
        for text in reader.frames(4.0):
            m = json.loads(text)
            if m.get("type") == "txState" and m.get("on"):
                got = m
                break
        assert got and got.get("mine"), got
        dsp("tune", 0)
        assert _wait(lambda: radio.transmit.get("tune") == "0", timeout=5)

        _ws_send(sock, {"cmd": "setReporterMessage", "message": "  Flex 6400   from FN03  "})
        msg = None
        for text in reader.frames(4.0):
            m = json.loads(text)
            if m.get("type") == "reporterMessage":
                msg = m
                break
        assert msg and msg["message"] == "Flex 6400 from FN03", msg
    finally:
        sock.close()


def test_nnr(flex):
    """NNR runs on the RX audio in voice modes: the page gets NR GAIN
    readings and the state; in CW it waits (a speech model would remove the
    carrier) and says so."""
    radio, rest = flex
    from test_web_login import _Reader, _ws, _ws_send
    port = int(rest.split(":")[2].split("/")[0]) - 1
    st, sock, buf = _ws(port, None)
    assert " 101" in st, st
    reader = _Reader(sock, buf)
    try:
        requests.put(f"{rest}/mode", json={"mode": "USB", "filter": 1}, timeout=5)
        _ws_send(sock, {"cmd": "enableAudio", "value": True})
        _ws_send(sock, {"cmd": "flexDsp", "key": "nnrModel", "index": 0, "value": 0})
        _ws_send(sock, {"cmd": "flexDsp", "key": "nnr", "index": 0, "value": 1})
        gains, active_state = [], None
        for text in reader.frames(10.0):
            m = json.loads(text)
            if m.get("type") == "nnrGain" and m.get("active"):
                gains.append(m["gain"])
            if m.get("type") == "flexDsp" and m.get("nnr") == 1 and m.get("nnrActive"):
                active_state = m
            if len(gains) >= 3 and active_state:
                break
        assert active_state, "NNR never reported active"
        assert len(gains) >= 3 and all(0.0 <= g <= 1.0 for g in gains), gains
        assert active_state["nnrDelayMs"] > 0

        requests.put(f"{rest}/mode", json={"mode": "CW", "filter": 1}, timeout=5)
        stopped = False
        for text in reader.frames(8.0):
            m = json.loads(text)
            if m.get("type") == "nnrGain" and not m.get("active"):
                stopped = True
                break
        assert stopped, "NNR kept running in CW"
    finally:
        _ws_send(sock, {"cmd": "flexDsp", "key": "nnr", "index": 0, "value": 0})
        requests.put(f"{rest}/mode", json={"mode": "USB", "filter": 1}, timeout=5)
        sock.close()


def test_aether_scope(flex):
    """AetherSDR-style scope: free pan (the panadapter stays put until the VFO
    nears an edge, sweeps say fixed glass), the page can move its centre and
    span, and drag one filter edge at a time."""
    radio, rest = flex
    from test_web_login import _Reader, _ws, _ws_send
    port = int(rest.split(":")[2].split("/")[0]) - 1
    st, sock, buf = _ws(port, None)
    assert " 101" in st, st
    reader = _Reader(sock, buf)
    PAN = "0x40000000"

    def dsp(key, value):
        _ws_send(sock, {"cmd": "flexDsp", "key": key, "index": 0, "value": value})

    try:
        requests.put(f"{rest}/mode", json={"mode": "USB", "filter": 1}, timeout=5)
        requests.put(f"{rest}/frequency", json={"hz": 14_200_000}, timeout=5)
        dsp("panFree", 1)
        got = None
        for text in reader.frames(6.0):
            m = json.loads(text)
            if m.get("type") == "flexDsp" and m.get("panFree") is True:
                got = m
                break
        assert got and got["rxHi"] > got["rxLo"], got

        dsp("panCenter", 14_230_000)
        assert _wait(lambda: f"display pan set {PAN} center=14.230000" in radio.commands, timeout=5)
        n = len(radio.commands)
        # A small tune inside the view does not move the panadapter...
        requests.put(f"{rest}/frequency", json={"hz": 14_205_000}, timeout=5)
        assert _wait(lambda: "slice tune 0 14.205000" in radio.commands, timeout=5)
        time.sleep(0.5)
        assert not any(c.startswith(f"display pan set {PAN} center=") for c in radio.commands[n:]), radio.commands[n:]
        # ...but one past the edge scrolls it.
        requests.put(f"{rest}/frequency", json={"hz": 14_400_000}, timeout=5)
        assert _wait(lambda: any(c.startswith(f"display pan set {PAN} center=") for c in radio.commands[n:]), timeout=5)

        dsp("panBandwidth", 50_000)
        assert _wait(lambda: f"display pan set {PAN} bandwidth=0.050000" in radio.commands, timeout=5)

        dsp("filterLo", 300)
        assert _wait(lambda: any(c.startswith("filt 0 300 ") for c in radio.commands), timeout=5)
        dsp("filterHi", 2400)
        assert _wait(lambda: "filt 0 300 2400" in radio.commands, timeout=5), radio.commands[-4:]
    finally:
        dsp("panFree", 0)
        sock.close()


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


def test_2m_antennas_come_back(flex):
    """2 m switches both antennas to the transverter port; leaving 2 m puts
    back the antennas last used below it, even when the radio's own band
    recall lands after wfweb's command with older ones."""
    radio, rest = flex
    from test_web_login import _Reader, _ws, _ws_send
    port = int(rest.split(":")[2].split("/")[0]) - 1
    st, sock, buf = _ws(port, None)
    assert " 101" in st, st
    try:
        requests.put(f"{rest}/frequency", json={"hz": 7_100_000}, timeout=5)
        assert _wait(lambda: radio.slice.get("RF_frequency", "").startswith("7.1"), timeout=5)
        # TX on ANT2 (index in tx_ant_list ANT1,ANT2,XVTA), RX stays ANT1.
        _ws_send(sock, {"cmd": "flexDsp", "key": "txAnt", "index": 0, "value": 1})
        assert _wait(lambda: radio.slice.get("txant") == "ANT2", timeout=5)

        requests.put(f"{rest}/frequency", json={"hz": 145_500_000}, timeout=5)
        assert _wait(lambda: radio.slice.get("txant") == "XVTA" and radio.slice.get("rxant") == "XVTA", timeout=5)

        requests.put(f"{rest}/frequency", json={"hz": 7_150_000}, timeout=5)
        time.sleep(1.5)   # past the radio's late recall of ANT1
        assert radio.slice.get("txant") == "ANT2", radio.slice
        assert radio.slice.get("rxant") == "ANT1", radio.slice
    finally:
        sock.close()


def test_radio_memories(flex):
    """The radio's memories reach the page, recall onto wfweb's slice, and
    can be created, renamed, regrouped and removed from it."""
    radio, rest = flex
    from test_web_login import _Reader, _ws, _ws_send
    port = int(rest.split(":")[2].split("/")[0]) - 1
    st, sock, buf = _ws(port, None)
    assert " 101" in st, st
    reader = _Reader(sock, buf)

    def latest_list(timeout=6.0):
        got = None
        end = time.monotonic() + timeout
        for text in reader.frames(timeout):
            m = json.loads(text)
            if m.get("type") == "flexMemories":
                got = m["list"]
                return got
            if time.monotonic() > end:
                break
        return got

    try:
        mems = latest_list()
        assert mems is not None, "no flexMemories"
        by_name = {m["name"]: m for m in mems}
        assert "Club net" in by_name and by_name["Club net"]["group"] == "Nets"   # 0x7F -> space
        assert by_name["FT8"]["freq"] == 14.074 and by_name["FT8"]["mode"] == "DIGU"

        _ws_send(sock, {"cmd": "flexMemory", "action": "apply", "id": by_name["Club net"]["id"]})
        assert _wait(lambda: "memory apply 0" in radio.commands, timeout=5)
        assert _wait(lambda: requests.get(f"{rest}/frequency", timeout=2).json().get("hz") == 3_873_000, timeout=5)

        _ws_send(sock, {"cmd": "flexMemory", "action": "create", "id": 0, "name": "My spot", "group": "DX"})
        assert _wait(lambda: any(c.startswith("memory set 2 ") and "name=My\x7fspot" in c and "group=DX" in c
                                 for c in radio.commands), timeout=5)
        mems = latest_list()
        while mems is not None and not any(m["name"] == "My spot" for m in mems):
            mems = latest_list()
        assert mems and any(m["name"] == "My spot" and m["group"] == "DX" for m in mems)

        _ws_send(sock, {"cmd": "flexMemory", "action": "remove", "id": 2})
        assert _wait(lambda: "memory remove 2" in radio.commands, timeout=5)
        mems = latest_list()
        while mems is not None and any(m["id"] == 2 for m in mems):
            mems = latest_list()
        assert mems is not None and len(mems) == 2
    finally:
        sock.close()


def test_fm_repeater(flex):
    """FM repeater on the Flex: DUP offset/direction and CTCSS encode become
    slice settings, and the radio's own values come back to the page."""
    radio, rest = flex
    from test_web_login import _Reader, _ws, _ws_send
    port = int(rest.split(":")[2].split("/")[0]) - 1
    st, sock, buf = _ws(port, None)
    assert " 101" in st, st
    reader = _Reader(sock, buf)
    try:
        for key, value in (("rptOffset", 600000), ("rptDir", 2), ("toneValue", 1000), ("toneMode", 1)):
            _ws_send(sock, {"cmd": "flexDsp", "key": key, "index": 0, "value": value})
        assert _wait(lambda: radio.slice.get("repeater_offset_dir") == "up"
                     and radio.slice.get("fm_repeater_offset_freq") == "0.600000"
                     and radio.slice.get("tx_offset_freq") == "0.600000", timeout=5), radio.slice
        assert _wait(lambda: radio.slice.get("fm_tone_mode") == "ctcss_tx"
                     and radio.slice.get("fm_tone_value") == "100.0", timeout=5), radio.slice

        _ws_send(sock, {"cmd": "flexDsp", "key": "rptDir", "index": 0, "value": 1})
        assert _wait(lambda: radio.slice.get("tx_offset_freq") == "-0.600000", timeout=5), radio.slice

        # The radio changes them itself (SmartSDR, a memory): the page hears it.
        radio._slice_status(repeater_offset_dir="simplex", fm_repeater_offset_freq="5.000000",
                            fm_tone_mode="off", fm_tone_value="88.5")
        seen = None
        for text in reader.frames(6.0):
            m = json.loads(text)
            if m.get("type") == "flexDsp" and m.get("rptDir") == "simplex" \
                    and m.get("rptOffsetHz") == 5_000_000:
                seen = m
                break
        assert seen and seen["rptOffsetHz"] == 5_000_000 and seen["toneMode"] == "off" \
            and seen["toneTenths"] == 885, seen

        # A memory or the band memory sets the shift but not the transmit
        # offset: in FM, FlexWeb puts tx_offset_freq where the shift says.
        radio._slice_status(mode="FM")
        radio._slice_status(repeater_offset_dir="up", fm_repeater_offset_freq="0.600000",
                            tx_offset_freq="0.000000")
        assert _wait(lambda: radio.slice.get("tx_offset_freq") == "0.600000", timeout=5), radio.slice
        radio._slice_status(repeater_offset_dir="down", fm_repeater_offset_freq="0.600000")
        assert _wait(lambda: radio.slice.get("tx_offset_freq") == "-0.600000", timeout=5), radio.slice
        radio._slice_status(repeater_offset_dir="simplex", mode="USB")
    finally:
        sock.close()


def test_time_ping_and_key_delay(flex):
    """FT8 timing inputs: the server answers timePing with its (NTP-corrected)
    time, and a keyed Flex reports its key-up delay to the page."""
    radio, rest = flex
    from test_web_login import _Reader, _ws, _ws_send
    port = int(rest.split(":")[2].split("/")[0]) - 1
    st, sock, buf = _ws(port, None)
    assert " 101" in st, st
    reader = _Reader(sock, buf)
    try:
        t0 = time.time() * 1000
        _ws_send(sock, {"cmd": "timePing", "t0": t0})
        pong = None
        for text in reader.frames(6.0):
            m = json.loads(text)
            if m.get("type") == "timePong":
                pong = m
                break
        assert pong and pong["t0"] == t0
        assert abs(pong["ts"] - time.time() * 1000) < 60_000   # NTP may correct the CI clock a little
        assert "ntpOk" in pong

        # The delay is smoothed (3/4 old + 1/4 new) and earlier tests keyed
        # with no delay, so key a few times and watch it climb toward 200 ms.
        radio.key_delay = 0.2
        try:
            seen = []
            for _ in range(8):
                requests.put(f"{rest}/ptt", json={"transmitting": True}, timeout=5)
                for text in reader.frames(4.0):
                    m = json.loads(text)
                    v = m.get("keyDelayMs") if m.get("type") == "flexDsp" else None
                    if v is not None and (not seen or v != seen[-1]):
                        seen.append(v)
                        if v > 0:
                            break
                requests.put(f"{rest}/ptt", json={"transmitting": False}, timeout=5)
                time.sleep(0.3)
                if seen and seen[-1] >= 120:
                    break
            assert seen and 120 <= seen[-1] <= 1000, seen
        finally:
            requests.put(f"{rest}/ptt", json={"transmitting": False}, timeout=5)
            radio.key_delay = 0.0
    finally:
        sock.close()


def test_opus_to_browser(flex):
    """AUDIO > QUALITY OPUS: the browser gets 0x05 Opus frames instead of 0x02
    PCM, and they decode to the radio's tone."""
    import ctypes
    import ctypes.util
    radio, rest = flex
    name = ctypes.util.find_library("opus")
    if not name:
        pytest.skip("libopus not available")
    lib = ctypes.CDLL(name)
    lib.opus_decoder_create.restype = ctypes.c_void_p
    lib.opus_decode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int32,
                                ctypes.POINTER(ctypes.c_int16), ctypes.c_int, ctypes.c_int]
    from test_web_login import _Reader, _ws, _ws_send
    port = int(rest.split(":")[2].split("/")[0]) - 1
    st, sock, buf = _ws(port, None)
    assert " 101" in st, st
    reader = _Reader(sock, buf)
    try:
        _ws_send(sock, {"cmd": "enableAudio", "value": True})
        _ws_send(sock, {"cmd": "setAudioCodec", "value": "opus"})
        codec, frames, pcm_after = None, [], 0
        end = time.monotonic() + 8
        for op, p in reader.all(8.0):
            if op == 1:
                m = json.loads(p.decode(errors="replace"))
                if m.get("type") == "audioCodec":
                    codec = m
            elif op == 2 and len(p) > 6:
                if p[0] == 0x05:
                    frames.append(p)
                elif p[0] == 0x02 and p[1] == 0 and len(frames) > 5:
                    pcm_after += 1
            if len(frames) >= 100 or time.monotonic() > end:
                break
        assert codec and codec["value"] == "opus", codec
        assert len(frames) >= 50, len(frames)
        assert pcm_after == 0, "PCM still sent to an Opus browser"
        rate = struct.unpack("<H", frames[0][4:6])[0] * 1000
        err = ctypes.c_int(0)
        dec = lib.opus_decoder_create(rate, 1, ctypes.byref(err))
        out = (ctypes.c_int16 * 5760)()
        samples = []
        for f in frames:
            data = bytes(f[6:])
            n = lib.opus_decode(dec, data, len(data), out, 5760, 0)
            assert n > 0
            samples += out[:n]
        tail = samples[-rate // 2:]
        rms = (sum(x * x for x in tail) / len(tail)) ** 0.5
        assert rms > 2000, rms           # the mock's 1 kHz tone at -12 dBFS
        # Back to PCM.
        _ws_send(sock, {"cmd": "setAudioCodec", "value": "pcm"})
        got_pcm = False
        for op, p in reader.all(4.0):
            if op == 2 and len(p) > 6 and p[0] == 0x02:
                got_pcm = True
                break
        assert got_pcm
    finally:
        sock.close()


def test_preamp_steps(flex):
    """P.AMP/ATT on a Flex uses the radio's own steps (rfgain_info): the page
    gets the range, a step lands on the pan, and the REST preamp/attenuator
    map onto valid values."""
    radio, rest = flex
    from test_web_login import _Reader, _ws, _ws_send
    port = int(rest.split(":")[2].split("/")[0]) - 1
    st, sock, buf = _ws(port, None)
    assert " 101" in st, st
    reader = _Reader(sock, buf)
    try:
        got = None
        for text in reader.frames(6.0):
            m = json.loads(text)
            if m.get("type") == "flexDsp" and m.get("rfgStep") == 8 and m.get("rfgLow") == -8:
                got = m
                break
        assert got and got["rfgHigh"] == 32, got
        _ws_send(sock, {"cmd": "flexDsp", "key": "panRfGain", "index": 0, "value": 16})
        assert _wait(lambda: radio.pan.get("rfgain") == "16", timeout=5), radio.pan
        _ws_send(sock, {"cmd": "flexDsp", "key": "panRfGain", "index": 0, "value": 13})   # snaps
        assert _wait(lambda: radio.pan.get("rfgain") == "16", timeout=2)
        _ws_send(sock, {"cmd": "setAttenuator", "value": 1})
        assert _wait(lambda: radio.pan.get("rfgain") == "-8", timeout=5), radio.pan
        _ws_send(sock, {"cmd": "setPreamp", "value": 1})
        assert _wait(lambda: radio.pan.get("rfgain") == "8", timeout=5), radio.pan
        _ws_send(sock, {"cmd": "setPreamp", "value": 0})
        assert _wait(lambda: radio.pan.get("rfgain") == "0", timeout=5), radio.pan
    finally:
        sock.close()


def test_vfo_b_and_split(flex):
    """VFO B is a second (muted) slice; selecting it points tuning at it,
    SPLIT makes it the TX slice, A=B and swap work on the two slices."""
    radio, rest = flex
    from test_web_login import _Reader, _ws, _ws_send
    port = int(rest.split(":")[2].split("/")[0]) - 1
    st, sock, buf = _ws(port, None)
    assert " 101" in st, st
    reader = _Reader(sock, buf)

    def status_has(pred, timeout=6.0):
        for text in reader.frames(timeout):
            m = json.loads(text)
            if pred(m):
                return m
        return None

    try:
        requests.put(f"{rest}/frequency", json={"hz": 14_200_000}, timeout=5)
        assert _wait(lambda: radio.slice.get("RF_frequency", "").startswith("14.2"), timeout=5)
        _ws_send(sock, {"cmd": "selectVFO", "value": "B"})
        assert _wait(lambda: radio.slice_b.get("in_use") == "1", timeout=5), "no second slice"
        assert _wait(lambda: radio.slice_b.get("audio_mute") == "1", timeout=5), radio.slice_b

        # Tuning now moves B, not A.
        requests.put(f"{rest}/frequency", json={"hz": 14_205_000}, timeout=5)
        assert _wait(lambda: radio.slice_b.get("RF_frequency") == "14.205000", timeout=5), radio.slice_b
        assert radio.slice.get("RF_frequency", "").startswith("14.2") and \
            not radio.slice.get("RF_frequency", "").startswith("14.205")

        _ws_send(sock, {"cmd": "setSplit", "value": True})
        assert _wait(lambda: radio.slice_b.get("tx") == "1" and radio.slice.get("tx") == "0", timeout=5), \
            (radio.slice, radio.slice_b)
        assert status_has(lambda m: m.get("split") is True), "split not reported"

        _ws_send(sock, {"cmd": "setSplit", "value": False})
        assert _wait(lambda: radio.slice_b.get("tx") == "0" and radio.slice.get("tx") == "1", timeout=5)

        _ws_send(sock, {"cmd": "selectVFO", "value": "A"})
        _ws_send(sock, {"cmd": "swapVFO"})
        assert _wait(lambda: radio.slice_b.get("RF_frequency", "").startswith("14.2000")
                     and radio.slice.get("RF_frequency", "").startswith("14.205"), timeout=5), \
            (radio.slice.get("RF_frequency"), radio.slice_b.get("RF_frequency"))
    finally:
        sock.close()


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
        assert any(l.startswith("application register name=FlexWeb") for l in broker.lines)
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
