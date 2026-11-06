"""The S meter gets the fast poll slot in receive (IC-7300 over RS-BA1,
2026-11-06). Po / SWR / ALC were polled all the time at the S meter's
priority, so in receive (where they read zero) they took two thirds of its
slot: one S meter reading every ~420 ms instead of ~140 ms. They are now
polled only while transmitting."""
import time

import requests


def _reads(mock, sub, since):
    # CI-V read frames: FE FE <to> <from> [29 <receiver>] 15 <sub> FD - the
    # mock is an IC-7610, whose two receivers take the 29 prefix.
    tail = bytes([0x15, sub, 0xFD])
    return [f for f in mock.civ_log[since:] if len(f) in (7, 9) and f.endswith(tail)]


def test_tx_meters_only_while_transmitting(rest_url, mock_radio):
    requests.put(f"{rest_url}/ptt", json={"transmitting": False}, timeout=5)
    time.sleep(1.5)                       # settle in receive

    start = len(mock_radio.civ_log)
    time.sleep(3.0)
    s_rx, po_rx, swr_rx = (len(_reads(mock_radio, s, start)) for s in (0x02, 0x11, 0x12))
    assert po_rx == 0 and swr_rx == 0, f"Po / SWR polled in receive: {po_rx} / {swr_rx} reads in 3 s"
    # The mock is an IC-7610: Overflow and both receivers' S meters share the
    # slot (~420 ms each); an IC-7300 has the S meter alone (~140 ms).
    assert s_rx >= 5, f"S meter polled too slowly in receive: {s_rx} reads in 3 s"

    requests.put(f"{rest_url}/ptt", json={"transmitting": True}, timeout=5)
    try:
        start = len(mock_radio.civ_log)
        deadline = time.time() + 5
        while time.time() < deadline and not _reads(mock_radio, 0x11, start):
            time.sleep(0.1)
        assert _reads(mock_radio, 0x11, start), "Po not polled while transmitting"
    finally:
        requests.put(f"{rest_url}/ptt", json={"transmitting": False}, timeout=5)

    time.sleep(2.0)                       # unkey reaches the server, the polls leave the queue
    start = len(mock_radio.civ_log)
    time.sleep(2.0)
    assert not _reads(mock_radio, 0x11, start), "Po still polled after unkey"
