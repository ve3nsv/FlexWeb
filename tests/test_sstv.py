"""SSTV codec (resources/web-shared/sstv.mjs).

tests/sstv_sim.mjs encodes a test card in every mode, passes it through a
channel with noise, a tuning offset and a sound-card clock error, decodes it
and checks the picture; it also checks joining a picture without its VIS
header and that noise alone gives no pictures.
"""

import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]


def test_sstv_sim():
    if not shutil.which("node"):
        pytest.skip("node unavailable")
    result = subprocess.run(
        ["node", str(ROOT / "tests" / "sstv_sim.mjs")],
        check=False,
        capture_output=True,
        text=True,
        timeout=900,
    )
    assert result.returncode == 0, result.stdout + result.stderr
