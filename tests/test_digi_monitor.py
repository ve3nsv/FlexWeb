"""Punch list 68: the digital TX monitor follows VOL, MUTE and AF
(tests/digi_monitor_sim.mjs under node)."""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_digi_monitor():
    if not shutil.which("node"):
        pytest.skip("node unavailable")
    run = subprocess.run(["node", str(ROOT / "tests" / "digi_monitor_sim.mjs")], capture_output=True, text=True,
                         timeout=60)
    assert run.returncode == 0, run.stdout + run.stderr
