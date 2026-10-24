"""Deterministic S-meter and recorder audio timing regressions."""
from pathlib import Path
import shutil
import subprocess
import pytest

def test_punchlist_ui():
    if not shutil.which("node"):
        pytest.skip("node unavailable")
    root = Path(__file__).resolve().parents[1]
    subprocess.run(["node", "tests/punchlist_ui_sim.mjs"], cwd=root, check=True, timeout=30)
