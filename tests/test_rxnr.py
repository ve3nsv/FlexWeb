"""Receive noise reduction (punch list 57 / 59): build and run
tests/rxnr_test.cpp - NNR and NR2 lower a noise-only signal and keep a
speech-like one at 8-48 kHz, switching NNR -> NR2 -> off through rxNrChain
has no gap and no click, a reducer switched off comes back on the same
kind. It compiles the C parts (WDSP NNR) with the C compiler, which
tests/test_cpp_units.py cannot (it builds everything as C++)."""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
C_SOURCES = ["resources/wdsp-nnr/" + f for f in
             ("nnr.c", "nnet.c", "nnio.c", "fftw_shim.c", "nnr_model_0.c", "nnr_model_1.c")]
CPP_SOURCES = ["tests/rxnr_test.cpp", "src/rxnr.cpp", "src/nnrfilter.cpp", "src/dsp/spectralnr.cpp",
               "src/logcategories.cpp"]


def test_rxnr(tmp_path):
    if not (shutil.which("gcc") and shutil.which("g++") and shutil.which("pkg-config")):
        pytest.skip("C / C++ compiler or pkg-config unavailable")
    flags = subprocess.run(["pkg-config", "--cflags", "--libs", "Qt5Core"], capture_output=True, text=True)
    if flags.returncode:
        pytest.skip("Qt5 development package unavailable")
    objs = []
    for src in C_SOURCES:
        obj = tmp_path / (Path(src).stem + ".o")
        subprocess.run(["gcc", "-c", "-O2", "-w", "-I", str(ROOT / "resources/wdsp-nnr"), str(ROOT / src),
                        "-o", str(obj)], check=True)
        objs.append(str(obj))
    exe = tmp_path / "rxnr_test"
    subprocess.run(["g++", "-std=c++17", "-O2", "-fPIC", "-I", str(ROOT / "include"), "-I", str(ROOT / "src"),
                    *(str(ROOT / s) for s in CPP_SOURCES), *objs, *flags.stdout.split(), "-o", str(exe)],
                   check=True)
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300, cwd=ROOT)
    print(run.stdout)
    assert run.returncode == 0, run.stdout + run.stderr
