"""RADE end-of-over callsign, decoded from a real off-air recording.

`wfweb --rade-rx-test <wav>` runs a recording through the same receive path
as the radio's audio (resampler, Hilbert transform, rade_rx, the EOO text
decoder in src/rade_text.c). FDV_offair.wav, from the RADE C repo
(resources/radae_nopy), is 48 kHz off-air audio of freedv-gui stations, so
this checks that FlexWeb reads callsigns sent by freedv-gui itself - the
two-FlexWeb loopback (freedv-test skill) cannot, since both ends share our
encoder (punch list 34c).
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
WFWEB_BIN = ROOT / "wfweb"
OFFAIR = ROOT / "resources" / "radae_nopy" / "FDV_offair.wav"


@pytest.mark.skipif(not WFWEB_BIN.exists(), reason="wfweb binary not built")
@pytest.mark.skipif(not OFFAIR.exists(), reason="radae_nopy submodule (FDV_offair.wav) not checked out")
def test_offair_eoo_callsign():
    r = subprocess.run([str(WFWEB_BIN), "--rade-rx-test", str(OFFAIR)],
                       capture_output=True, text=True, timeout=600)
    out = r.stdout + r.stderr
    tail = " | ".join(l.strip() for l in out.splitlines()[-15:] if l.strip())
    assert r.returncode == 0, "no callsign decoded: " + tail[-2000:]
    assert "callsign VK5KVA" in r.stdout, "expected VK5KVA from the recording: " + tail[-2000:]
