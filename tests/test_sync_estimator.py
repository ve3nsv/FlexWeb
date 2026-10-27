"""Scope / S-meter / audio lag estimator (resources/web/sync-estimator.js).

tests/sync_estimator_sim.mjs feeds it a speech-like loudness envelope as
audio chunks, scope frames and S-meter readings with known lags, and checks
that it finds them, keeps the last good value when the band goes quiet and
stays silent on noise alone.
"""

import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_sync_estimator_sim():
    if not shutil.which("node"):
        pytest.skip("node unavailable")
    result = subprocess.run(["node", str(ROOT / "tests" / "sync_estimator_sim.mjs")],
                            check=False, capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
