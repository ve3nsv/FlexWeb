"""Check the radio definition against documented FT-991A CAT wire forms."""
import configparser
from pathlib import Path
from mock_yaesu991a import Mock991A


def test_ft991a_definition():
    p = configparser.ConfigParser(interpolation=None)
    p.optionxform = str
    p.read(Path(__file__).resolve().parents[1] / 'rigs' / 'FT-991A.rig')
    rig = p['Rig']
    assert rig['CIVAddress'] == '670'
    assert rig['HasSpectrum'] == 'false'
    cmds = {rig[f'Commands\\{i}\\Type']: rig[f'Commands\\{i}\\String']
            for i in range(1, int(rig['Commands\\size']) + 1)}
    assert {name: cmds[name] for name in ('Selected Freq', 'Selected Mode', 'Transceiver Status', 'S Meter', 'AF Gain')} == {
        'Selected Freq': 'FA', 'Selected Mode': 'MD0', 'Transceiver Status': 'TX', 'S Meter': 'SM0', 'AF Gain': 'AG0'}
    assert {rig[f'Bands\\{i}\\Name'] for i in range(1, int(rig['Bands\\size']) + 1)} >= {'2m', '70cm'}
    assert rig['Modes\\14\\Reg'] == '0E' and rig['Modes\\14\\Name'] == 'C4FM'


def test_simulator_commands():
    m = Mock991A()
    try:
        assert m.respond('ID') == 'ID0670;'
        assert m.respond('FA') == 'FA014230000;'
        assert m.respond('FA144200000') is None and m.respond('FA') == 'FA144200000;'
        assert m.respond('MD0E') is None and m.respond('MD0') == 'MD0E;'
        assert m.respond('TX1') is None and m.respond('TX') == 'TX1;'
        assert m.respond('SM0') == 'SM0120;'
        assert m.respond('XY') == '?;'
    finally:
        m.stop()


def test_ft991a_server_integration(tmp_path):
    """Exercise CAT identification, frequency, mode and PTT via a PTY radio."""
    import os
    import shutil
    import socket
    import subprocess
    import time
    import pytest
    import requests
    root = Path(__file__).resolve().parents[1]
    binary = root / 'wfweb'
    if not binary.exists():
        pytest.skip('server binary not built')
    bindir = tmp_path / 'bin'
    rigdir = tmp_path / 'share' / 'wfview' / 'rigs'
    bindir.mkdir()
    rigdir.mkdir(parents=True)
    executable = bindir / 'wfweb'
    shutil.copy2(binary, executable)
    shutil.copy2(root / 'rigs' / 'FT-991A.rig', rigdir)
    mock = Mock991A()
    mock.start()
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    env = dict(os.environ, WFWEB_CONFIG_DIR=str(tmp_path / 'config'),
               XDG_CONFIG_HOME=str(tmp_path / 'settings'), WFWEB_NO_TUI='1')
    proc = subprocess.Popen([str(executable), '-p', str(port), '--manufacturer', '2', '--serial-port', mock.path],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    base = f'http://127.0.0.1:{port + 1}/api/v1/radio'
    def wait(predicate, timeout=20):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                if predicate(): return True
            except (OSError, requests.RequestException, ValueError):
                pass
            time.sleep(.1)
        return False
    try:
        assert wait(lambda: requests.get(base + '/info', timeout=1).json().get('model') == 'FT-991A')
        info = requests.get(base + '/info', timeout=3).json()
        assert not info['hasSpectrum'] and 'C4FM' in info['modes']
        assert wait(lambda: requests.get(base + '/frequency', timeout=1).json().get('hz') == 14230000)
        assert requests.put(base + '/frequency', json={'hz': 144200000}, timeout=3).status_code == 202
        assert wait(lambda: mock.freq_a == 144200000)
        assert requests.put(base + '/mode', json={'mode': 'FM'}, timeout=3).status_code == 202
        assert wait(lambda: mock.mode == '4')
        assert requests.put(base + '/ptt', json={'transmitting': True}, timeout=3).status_code == 202
        assert wait(lambda: mock.ptt)
        assert requests.put(base + '/ptt', json={'transmitting': False}, timeout=3).status_code == 202
        assert wait(lambda: not mock.ptt)
    finally:
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
        mock.stop()
