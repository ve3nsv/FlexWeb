#!/usr/bin/env python3
"""Generate rigs/FT-991A.rig from rigs/FT-710.rig.

The FT-991A speaks the same "new" Yaesu CAT as the FT-710 for almost
everything wfweb uses; the differences come from the FT-991A CAT reference
and Hamlib (rigs/yaesu/newcat.c, ft991.c):
- radio ID 0670; no scope over USB; no 70 MHz; 2 m and 70 cm (BS15 / BS16)
- RIT on / off is RT (no CF); no RIT offset readback, no memory tags (MT),
  no ST / VS (split is FT2 / FT3 there), no AV, no RM meter select
- meters: RMn + 3 digits (no right padding); filter width SH0 + 2 digits; SSB widths 200-3200 Hz (21), CW / DATA
  50-3000 Hz (17); attenuator OFF / 12 dB
- menu (EX) numbers differ from the FT-710's, so none are sent: the mod
  source and USB levels are set on the radio (menus 070-073, 110 ...)
- modes 01-0D as the FT-710; 0E is C4FM (not supported), no 0F
Run it again after changing the FT-710 file: python3 tools/make-ft991a-rig.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
src = (ROOT / "rigs" / "FT-710.rig").read_text(encoding="utf-8").split("\n")

ITEM = re.compile(r'^([^\\=]+)\\(\d+)\\([^=]+)=(.*)$')
SIZE = re.compile(r'^([^\\=]+)\\size=')

arrays = {}
for line in src:
    m = ITEM.match(line)
    if m:
        a, i, k, v = m.group(1), int(m.group(2)), m.group(3), m.group(4)
        e = arrays.setdefault(a, {}).setdefault(i, {"_keys": []})
        e[k] = v
        e["_keys"].append(k)


def entries(a):
    return [arrays[a][i] for i in sorted(arrays.get(a, {}))]


def mk(**kv):
    d = {k: str(v) for k, v in kv.items()}
    d["_keys"] = list(kv.keys())
    return d


DROP = {"Data Off Mod Input", "USB Mod Level", "ACC1 Mod Level", "Anti-Vox Gain", "RIT Frequency",
        "System Date", "System Time", "Memory Tag", "Scope Speed", "Scope Span", "Scope Mode",
        "VFO A Select", "VFO B Select", "Meter Type"}
cmds = []
for c in entries("Commands"):
    if c["Type"] in DROP or c["String"].startswith("EX"):
        continue
    if c["Type"] == "Split/Duplex":
        # The FT-991A has no ST: split is the TX VFO, FT. It reads FT0 (TX on
        # VFO A) / FT1 (TX on VFO B) and is set with FT2 / FT3;
        # yaesuCommander maps the set (punch list: FT-991A split).
        c.update(String="FT", Min="0", Max="1", Bytes="1", PadRight="false")
    if c["Type"] == "RIT Status":
        c.update(String="RT", Bytes="1", PadRight="false")
    if c["Type"] == "Band Stacking Reg":
        c["Max"] = "16"          # BS15 = 2 m, BS16 = 70 cm
    if c["Type"] == "Attenuator Status":
        c["Max"] = "1"
    if c["Type"] == "Filter Width":
        c.update(Bytes="2", Max="21")
    if c["String"] in ("RM3", "RM4", "RM5", "RM6", "RM7", "RM8"):
        # the FT-991A answers RMn + 3 digits (the FT-710 adds 3 more)
        c.update(Bytes="3", PadRight="false")
    cmds.append(c)

periodic = [p for p in entries("Periodic")
            if p["Command"] not in {"Data Off Mod Input", "RIT Frequency", "Scope Mode", "Scope Span", "Scope Speed"}]
modes = [m for m in entries("Modes") if m["Reg"] not in ("0E", "0F")]

bands = []
for b in entries("Bands"):
    if b["Name"] in ("4m", "630m", "2200m"):
        continue
    if b["Name"] == "Gen":
        b["End"] = "470000000"
    bands.append(b)
gen = [b for b in bands if b["Name"] == "Gen"]
bands = [b for b in bands if b["Name"] != "Gen"]
band = lambda name, num, bsr, start, end, rng, color: mk(Region="", Num=num, BSR=bsr, Start=start, End=end, Range=rng,
                                                      MemoryGroup=-1, Name=name, Bytes=9, Power=50, Antennas="true",
                                                      Offset=0, Color=color)
bands = [band("70cm", 5, 16, 420000000, 450000000, 450, "#ff0000ff"),
         band("2m", 6, 15, 144000000, 148000000, 148, "#ff00ffff")] + bands + gen

SSB = [200, 400, 600, 850, 1100, 1350, 1500, 1650, 1800, 1950, 2100, 2200, 2300, 2400, 2500, 2600, 2700,
       2800, 2900, 3000, 3200]
CW = [50, 100, 150, 200, 250, 300, 350, 400, 450, 500, 800, 1200, 1400, 1700, 2000, 2400, 3000]
widths = [mk(Bands="0006", Num=i, Hz=hz) for i, hz in enumerate(SSB, 1)] + \
         [mk(Bands="1188", Num=i, Hz=hz) for i, hz in enumerate(CW, 1)]   # CW-U/L, DATA-L/U

attenuators = [mk(Num=0, Name="OFF"), mk(Num=1, Name="12 dB")]

NEW = {"Commands": cmds, "Periodic": periodic, "Spans": [], "Modes": modes, "Bands": bands,
       "Widths": widths, "Attenuators": attenuators}

out, done = [], set()
for line in src:
    m = ITEM.match(line) or SIZE.match(line)
    a = m.group(1) if m else None
    if a in NEW:
        if a not in done:
            done.add(a)
            for i, e in enumerate(NEW[a], 1):
                out += [f"{a}\\{i}\\{k}={e[k]}" for k in e["_keys"]]
            out.append(f"{a}\\size={len(NEW[a])}")
        continue
    line = {"Model=FT-710": "Model=FT-991A", "CIVAddress=800": "CIVAddress=670",
            "HasSpectrum=true": "HasSpectrum=false", "SpectrumLenMax=850": "SpectrumLenMax=0"}.get(line, line)
    out.append(line)
(ROOT / "rigs" / "FT-991A.rig").write_text("\n".join(out), encoding="utf-8")
print("rigs/FT-991A.rig:", len(cmds), "commands,", len(bands), "bands,", len(modes), "modes,", len(widths), "widths")
