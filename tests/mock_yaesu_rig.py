"""Mock Yaesu CAT radio on a pseudo-terminal (Linux / macOS), for
tests/test_ft991a.py.

It reads the rig file it impersonates (rigs/FT-991A.rig) and answers the
way the "new" Yaesu CAT does: a command that is exactly one of the rig
file's strings is a read ("FA;" -> "FA014200000;"), a longer one is a set
("FA007100000;", no reply), anything else gets "?;". The radio ID ("ID;")
comes from the rig file's CIVAddress. Every command received is recorded.
"""

from __future__ import annotations

import os
import re
import threading
import time
import tty
from pathlib import Path


def _commands(rig_file: Path) -> dict[str, int]:
    """CAT string -> value width, from the rig file's Commands array."""
    cmds: dict[int, dict[str, str]] = {}
    civ = 0
    for line in rig_file.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^Commands\\(\d+)\\(\w+)=(.*)$", line)
        if m:
            cmds.setdefault(int(m.group(1)), {})[m.group(2)] = m.group(3)
        elif line.startswith("CIVAddress="):
            civ = int(line.split("=", 1)[1])
    out = {c["String"]: int(c.get("Bytes", "0") or 0) for c in cmds.values()
           if c.get("String") and c["String"] != "?"}
    out["ID"] = 4
    return out, civ


class MockYaesuRig:
    def __init__(self, rig_file: Path):
        self.widths, civ = _commands(rig_file)
        self.values: dict[str, str] = {k: "0" * w for k, w in self.widths.items()}
        self.values.update({
            "ID": f"{civ:04d}", "FA": "014200000", "FB": "007100000", "MD0": "2", "MD1": "2",
            "SM0": "120", "AG": "0128", "RG0": "255", "PC": "100", "SH0": "17", "PS": "1",
            "TX": "0", "RM5": "000", "RM6": "000",
        })
        self.commands: list[str] = []
        self.master, slave = os.openpty()
        tty.setraw(slave)
        self.slave_name = os.ttyname(slave)
        self._slave = slave          # keep it open so the device stays
        self._stop = False
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _reply(self, c: str) -> str | None:
        if c in self.widths:                          # a read
            return c + self.values.get(c, "")
        for k in sorted(self.widths, key=len, reverse=True):
            if c.startswith(k) and len(c) > len(k):   # a set
                self.values[k] = c[len(k):]
                if k == "TX":
                    # keyed: some power and a good match on the meters
                    keyed = self.values["TX"] != "0"
                    self.values["RM5"] = "180" if keyed else "000"
                    self.values["RM6"] = "020" if keyed else "000"
                return None
        return "?"

    def _run(self) -> None:
        buf = b""
        while not self._stop:
            try:
                data = os.read(self.master, 4096)
            except OSError:
                time.sleep(0.05)
                continue
            if not data:
                time.sleep(0.01)
                continue
            buf += data
            while b";" in buf:
                raw, buf = buf.split(b";", 1)
                c = raw.decode(errors="replace").strip()
                if not c:
                    continue
                self.commands.append(c)
                r = self._reply(c)
                if r is not None:
                    os.write(self.master, (r + ";").encode())

    def find(self, prefix: str) -> list[str]:
        return [c for c in self.commands if c.startswith(prefix)]

    def close(self) -> None:
        self._stop = True
        for fd in (self.master, self._slave):
            try:
                os.close(fd)
            except OSError:
                pass
