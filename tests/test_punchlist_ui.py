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

@pytest.mark.parametrize("script", ["sstv_fsk_sim.mjs", "sstv_templates_sim.mjs"])
def test_sstv_additions(script):
    if not shutil.which("node"):
        pytest.skip("node unavailable")
    root = Path(__file__).resolve().parents[1]
    subprocess.run(["node", str(root / "tests" / script)], cwd=root, check=True, timeout=30)
