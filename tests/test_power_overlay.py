"""Punch list 94: "Connecting to <radio>..." instead of "Radio is powered
off" while a LAN radio is being opened (tests/power_overlay_sim.mjs under
node)."""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_power_overlay():
    if not shutil.which("node"):
        pytest.skip("node unavailable")
    run = subprocess.run(["node", str(ROOT / "tests" / "power_overlay_sim.mjs")], capture_output=True, text=True,
                         timeout=60)
    assert run.returncode == 0, run.stdout + run.stderr
