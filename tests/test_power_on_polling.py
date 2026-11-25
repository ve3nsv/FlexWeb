"""Power On pressed while the rig is already on keeps the polls (punch
list 92). servermain::powerRigOn started the power probe - queue cleared,
one frequency read every 2 s - and only an off -> on change restored the
polls, so after a Power On on a rig that was on the S meter (and every
other polled value) ran at that rate for the rest of the session."""
import time

from test_meter_polling import _reads
from test_web_login import _Reader, _ws, _ws_send


def test_power_on_while_on_keeps_polling(wfweb_instance, mock_radio):
    _, web_port = wfweb_instance
    st, sock, buf = _ws(web_port, None)
    assert " 101" in st, st
    try:
        _ws_send(sock, {"cmd": "setPower", "value": True})
        time.sleep(6.0)                    # past powerRigOn's 5 s timer
        start = len(mock_radio.civ_log)
        time.sleep(3.0)
        s_reads = len(_reads(mock_radio, 0x02, start))
        assert s_reads >= 5, f"S meter polled {s_reads} times in 3 s after Power On"
    finally:
        sock.close()
