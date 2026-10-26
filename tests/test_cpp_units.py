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
    "serverload": [],
    "sdrspectrum": [],
    "scopesync": [],
    "kiwiwaterfall": [],
    "kiwisource": [],
    "rtltcpprotocol": [],
    "rtltcpsource": [],
    "rtllocalsource": [],
    "soapysource": [],
    "cputemperature": [],
    "sstvsettings": [],
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
    "rfkitconnection": ["src/rfkitconnection.cpp", "src/rfkitprotocol.cpp"],
    # Windows launcher helpers: wfweb command line composed from the form,
    # and the server log lines the launcher reacts to.
    "launcher": ["launcher/src/launchconfig.cpp", "launcher/src/logparser.cpp",
                 "launcher/src/portowner.cpp"],
    # Web login: PBKDF2 hashes, users, sessions, cookie parsing, lockout.
    "webauth": ["src/webauth.cpp"],
}

# Units that need more than Qt5Core.
MODULES = {"kiwisource": ["Qt5Core", "Qt5Network", "Qt5WebSockets"],
           "rtltcpsource": ["Qt5Core", "Qt5Network"],
           "webauth": ["Qt5Core", "Qt5Network"],
           "rfkitconnection": ["Qt5Core", "Qt5Network"]}


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
    extra = []
    if unit == "rfkitconnection":
        moc = shutil.which("moc") or shutil.which("moc-qt5")
        assert moc, "Qt moc is required to test the RF-KIT transport"
        generated = tmp_path / "moc_rfkitconnection.cpp"
        subprocess.run([moc, str(ROOT / "include/rfkitconnection.h"), "-o", str(generated)], check=True)
        extra.append(str(generated))
    command = [
        "g++", "-std=c++17", "-fPIC",
        "-I", str(ROOT / "include"),
        str(ROOT / "tests" / f"{unit}_test.cpp"),
        *(str(ROOT / src) for src in UNITS[unit]),
        *extra,
        *flags.stdout.split(),
        "-o", str(executable),
    ]
    subprocess.run(command, check=True, cwd=ROOT)
    env = None
    if unit == "webauth":
        # QSettings writes here: keep the real user's settings out of it.
        import os
        # WFWEB_CONFIG_DIR too: conftest.py points every test at one shared
        # empty directory, and the users this test adds must not land there.
        env = dict(os.environ, HOME=str(tmp_path), XDG_CONFIG_HOME=str(tmp_path / "cfg"),
                   WFWEB_CONFIG_DIR=str(tmp_path))
    args = [str(executable)]
    if unit in ("rtllocalsource", "soapysource"):
        import sys
        if sys.platform != "linux":
            pytest.skip("The fake RTL-SDR shared library is built on Linux")
        library = tmp_path / ("libfake_rtlsdr.so" if unit == "rtllocalsource" else "libfake_soapy.so")
        fake = "fake_rtlsdr.cpp" if unit == "rtllocalsource" else "fake_soapy.cpp"
        subprocess.run(["g++", "-std=c++17", "-fPIC", "-shared", "-pthread",
                        str(ROOT / "tests" / fake), "-o", str(library)], check=True)
        args.append(str(library))
    subprocess.run(args, check=True, cwd=ROOT, env=env)
    if unit == "rfkitconnection":
        for scenario in ("truncated", "deadline"):
            subprocess.run([str(executable), scenario], check=True, cwd=ROOT, env=env, timeout=20)
