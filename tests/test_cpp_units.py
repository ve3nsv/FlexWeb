"""Build and run the self-contained C++ unit tests (no rig, no server).

Each entry is a test program under tests/ plus the sources it needs.  They
link only against Qt5Core, so a plain g++ + pkg-config is enough.
"""

import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]

UNITS = {
    # WSJT-X NetworkMessage serialization, checked by deserializing with Qt's
    # own schema-3 representation.
    "wsjtx_protocol": ["src/wsjtxmessage.cpp"],
    # ADIF parse/append/rewrite, chronological index, paging, merge.
    "logbook": ["src/logbook.cpp"],
    # Terminal status page layout at every terminal size.
    "consolestatus": ["src/consolestatus.cpp"],
    # FlexRadio SmartSDR line parser, VITA-49 decoders, DAX TX packets.
    "flexprotocol": ["src/radio/flexprotocol.cpp"],
    # RF-KIT (RF2K-S) amplifier REST/JSON codec.
    "rfkitprotocol": ["src/rfkitprotocol.cpp"],
    # Windows launcher helpers: wfweb command line composed from the form,
    # and the server log lines the launcher reacts to.
    "launcher": ["launcher/src/launchconfig.cpp", "launcher/src/logparser.cpp",
                 "launcher/src/portowner.cpp"],
    # Web login: PBKDF2 hashes, users, sessions, cookie parsing, lockout.
    "webauth": ["src/webauth.cpp"],
}

# Units that need more than Qt5Core.
MODULES = {"webauth": ["Qt5Core", "Qt5Network"]}


@pytest.mark.parametrize("unit", sorted(UNITS))
def test_cpp_unit(unit, tmp_path):
    if not shutil.which("g++") or not shutil.which("pkg-config"):
        pytest.skip("C++ compiler/pkg-config unavailable")
    flags = subprocess.run(
        ["pkg-config", "--cflags", "--libs", *MODULES.get(unit, ["Qt5Core"])],
        check=False,
        capture_output=True,
        text=True,
    )
    if flags.returncode:
        pytest.skip("Qt5 development package unavailable")

    executable = tmp_path / f"{unit}_test"
    command = [
        "g++", "-std=c++17", "-fPIC",
        "-I", str(ROOT / "include"),
        str(ROOT / "tests" / f"{unit}_test.cpp"),
        *(str(ROOT / src) for src in UNITS[unit]),
        *flags.stdout.split(),
        "-o", str(executable),
    ]
    subprocess.run(command, check=True, cwd=ROOT)
    env = None
    if unit == "webauth":
        # QSettings writes here: keep the real user's settings out of it.
        import os
        env = dict(os.environ, HOME=str(tmp_path), XDG_CONFIG_HOME=str(tmp_path / "cfg"))
    subprocess.run([str(executable)], check=True, cwd=ROOT, env=env)
