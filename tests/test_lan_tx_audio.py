"""Browser mic audio reaches a LAN Icom (punch list 72).

IC-7300 over RS-BA1: TUNE keyed the rig but voice carried nothing. The
LAN audio stream also opened a local capture device (wfview's desktop
path) and sent it - the PC's default input, a silent virtual cable - on
the same TX stream as the browser's audio. The server build now takes TX
audio from the web server only. Here the mock radio records the TX audio
packets: the browser's tone must arrive, at its own rate (no second
stream interleaved), with no silence inside the over. Between overs the
stream keeps running with silence at real-time rate, as wfview's always-on
mic stream does."""
import struct
import time

import pytest

from test_web_login import _Reader, _ws, _ws_send


def _wait(pred, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.05)
    return False


def _drain_until(reader, pred, timeout=8.0):
    import json
    for text in reader.frames(timeout):
        try:
            m = json.loads(text)
        except ValueError:
            continue
        if pred(m):
            return m
    return None


def test_browser_mic_reaches_lan_rig(wfweb_instance, mock_radio):
    _, web_port = wfweb_instance
    st, sock, buf = _ws(web_port, None)
    assert " 101" in st, st
    reader = _Reader(sock, buf)
    try:
        assert _drain_until(reader, lambda m: m.get("type") == "rigInfo"), "no rigInfo"
        _ws_send(sock, {"cmd": "enableAudio", "value": True})
        st_msg = _drain_until(reader, lambda m: m.get("type") == "audioStatus")
        assert st_msg, "no audioStatus"
        if not st_msg.get("enabled"):
            pytest.skip(f"browser audio unavailable here: {st_msg.get('reason')}")
        _ws_send(sock, {"cmd": "enableMic", "value": True})
        time.sleep(0.5)
        # Between overs: silence at real time (~96 kB/s at 48 kHz 16-bit).
        mock_radio.tx_audio.clear()
        time.sleep(2.0)
        idle = list(mock_radio.tx_audio)
        idle_bytes = sum(len(p) for _, p in idle)
        assert idle and all(not any(p) for _, p in idle), "no silence keepalive between overs"
        assert 0.7 * 2 * 96000 <= idle_bytes <= 1.3 * 2 * 96000, f"keepalive not at real time: {idle_bytes} bytes in 2 s"

        _ws_send(sock, {"cmd": "setPTT", "value": True})
        time.sleep(0.3)
        mock_radio.tx_audio.clear()

        # 1 s of a 1 kHz tone at 48 kHz, in 10 ms browser frames at real time
        frame = struct.pack("<480h", *[int(8000 * (1 if (i // 24) % 2 == 0 else -1)) for i in range(480)])
        start = time.monotonic()
        for i in range(100):
            _ws_send(sock, bytes([0x03, 0x00]) + struct.pack("<HH", i & 0xFFFF, 0) + frame, opcode=2)
            delay = start + (i + 1) * 0.01 - time.monotonic()
            if delay > 0:
                time.sleep(delay)
        sent = 100 * len(frame)
        voiced = lambda: sum(len(p) for _, p in mock_radio.tx_audio if any(p))
        assert _wait(lambda: voiced() >= 0.8 * sent, timeout=5), \
            f"TX audio did not reach the rig: {voiced()} of {sent} bytes"
        time.sleep(0.5)
        _ws_send(sock, {"cmd": "setPTT", "value": False})

        pkts = list(mock_radio.tx_audio)
        loud = [i for i, (_, p) in enumerate(pkts) if any(p)]
        got = sum(len(pkts[i][1]) for i in loud)
        # Our tone once, not interleaved with a second stream...
        assert got <= 1.3 * sent, f"{got} bytes of TX audio for {sent} sent: a second stream on the same TX"
        # ... and no silence inside the over (the keepalive only fills gaps).
        inside = [i for i in range(loud[0], loud[-1] + 1) if not any(pkts[i][1])]
        assert not inside, f"{len(inside)} silent packets inside the over"
        # The keepalive comes back after it.
        assert _wait(lambda: any(not any(p) for _, p in mock_radio.tx_audio[len(pkts):]), timeout=2), \
            "no silence keepalive after the over"
    finally:
        _ws_send(sock, {"cmd": "setPTT", "value": False})
        _ws_send(sock, {"cmd": "enableMic", "value": False})
        sock.close()
