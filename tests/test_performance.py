import shutil
import subprocess
from pathlib import Path
import pytest


def test_metrics_recorder():
    if not shutil.which('node'):
        pytest.skip('node unavailable')
    root = Path(__file__).resolve().parents[1]
    subprocess.run(['node', 'tests/performance_test.cjs'], cwd=root, check=True, timeout=30)


def test_waterfall_ring():
    if not shutil.which('node'):
        pytest.skip('node unavailable')
    root = Path(__file__).resolve().parents[1]
    subprocess.run(['node', 'tests/waterfall_ring_test.cjs'], cwd=root, check=True, timeout=30)
