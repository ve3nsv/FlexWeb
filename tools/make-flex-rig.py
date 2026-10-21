#!/usr/bin/env python3
"""Generate rigs/FLEX-6000.rig, the wfweb rig definition for FlexRadio SDRs.

One file covers the FLEX-6000 / FLEX-8000 / Aurora family: flexCommander
reads the model name from the radio and everything else is pushed by the
radio itself. Mode registers here must match kModes in
src/radio/flexcommander.cpp. Run from the repo root.
"""
import re

out = []
def kv(k, v): out.append(f"{k}={v}")

out.append("[General]")
kv("Version", "2.11")
out.append("")
out.append("[Rig]")
kv("Manufacturer", 3)          # manufFlexRadio
kv("Model", "FLEX-6000")
kv("CIVAddress", 1)            # key in servermain's rig list, not a bus address
kv("RigCtlDModel", 0)
kv("NumberOfReceivers", 1)
kv("NumberOfVFOs", 1)
kv("SpectrumSeqMax", 0)
kv("SpectrumAmpMax", 160)      # FFT ypixels = 161: one amplitude step per pixel
kv("SpectrumLenMax", 1024)     # FFT xpixels
kv("HasSpectrum", "true")
kv("HasLAN", "true")
kv("HasEthernet", "true")
kv("HasWiFi", "false")
kv("HasTransmit", "true")
kv("HasFDComms", "true")
kv("HasCommand29", "false")
kv("MemGroups", 0)
kv("Memories", 0)
kv("MemStart", 1)
kv("MemFormat", "")
kv("SatMemories", 0)
kv("SatFormat", "")

def array(name, rows):
    for i, row in enumerate(rows, 1):
        for k, v in row.items():
            kv(f"{name}\\{i}\\{k}", v)
    kv(f"{name}\\size", len(rows))

commands = [
    "Selected Freq", "Selected Mode", "Transceiver ID", "Transceiver Status",
    "AF Gain", "RF Gain", "RF Power", "Squelch", "USB Mod Level",
    "S Meter", "Power Meter", "SWR Meter", "ALC Meter", "Comp Meter",
    "Scope Wave Data", "Scope On/Off", "Scope Data Output", "Scope Span", "Scope Speed",
    "Filter Width", "AGC Status", "Noise Blanker", "Noise Reduction", "Auto Notch",
    "Preamp Status", "Attenuator Status", "Antenna", "Tuner/ATU Status",
    "Key Speed", "Send CW", "CW Pitch", "Monitor Status", "Monitor Gain",
    "Compressor Status", "RIT Frequency", "RIT Status", "Data Off Mod Input",
    # VFO B = a second slice; SPLIT makes it the TX slice (flexCommander).
    "Unselected Freq", "VFO A Select", "VFO B Select", "VFO Swap A/B", "VFO Equal AB",
    "Split/Duplex",
]
array("Commands", [{
    "Type": c, "String": "", "Min": -9999 if c == "RIT Frequency" else 0,
    "Max": 9999 if c == "RIT Frequency" else 255, "Bytes": 0, "PadRight": "false",
    "Command29": "false", "GetCommand": "true",
    "SetCommand": "false" if c in ("Transceiver ID", "S Meter", "Power Meter", "SWR Meter",
                                   "ALC Meter", "Comp Meter", "Scope Wave Data") else "true",
    "Admin": "false"} for c in commands])

# Everything is pushed by the radio; these keep the meters fresh in the
# cache for clients that join later.
array("Periodic", [
    {"Priority": "Medium", "Command": "S Meter", "VFO": 0},
])

# Num = wfweb rigMode_t, Reg = flexCommander's register (kModes).
array("Modes", [
    {"Num": 0,  "Reg": 1,  "Name": "LSB",  "Min": 1000, "Max": 6000},
    {"Num": 1,  "Reg": 2,  "Name": "USB",  "Min": 1000, "Max": 6000},
    {"Num": 3,  "Reg": 3,  "Name": "CW",   "Min": 50,   "Max": 3000},
    {"Num": 2,  "Reg": 4,  "Name": "AM",   "Min": 3000, "Max": 20000},
    {"Num": 16, "Reg": 5,  "Name": "SAM",  "Min": 3000, "Max": 20000},
    {"Num": 5,  "Reg": 6,  "Name": "FM",   "Min": 0,    "Max": 0},
    {"Num": 5,  "Reg": 7,  "Name": "NFM",  "Min": 0,    "Max": 0},
    {"Num": 11, "Reg": 8,  "Name": "DIGU", "Min": 100,  "Max": 6000},
    {"Num": 10, "Reg": 9,  "Name": "DIGL", "Min": 100,  "Max": 6000},
    {"Num": 4,  "Reg": 10, "Name": "RTTY", "Min": 250,  "Max": 3000},
    {"Num": 25, "Reg": 11, "Name": "DFM",  "Min": 0,    "Max": 0},
])

# Half the panadapter width in Hz, the Icom convention the page expects
# (flexCommander asks the radio for twice this as the pan bandwidth).
spans = [(5000, "±5 kHz"), (12500, "±12.5 kHz"), (25000, "±25 kHz"), (50000, "±50 kHz"),
         (100000, "±100 kHz"), (250000, "±250 kHz"), (500000, "±500 kHz"), (1250000, "±1.25 MHz")]
array("Spans", [{"Num": i, "Name": n, "Freq": f} for i, (f, n) in enumerate(spans)])

array("Inputs", [
    {"Num": 0, "Reg": 0, "Name": "MIC"},          # inputMic: the radio's own mic input
    {"Num": 4, "Reg": 1, "Name": "LAN (DAX)"},    # inputLAN: browser audio via DAX
])

array("Tuning Steps", [
    {"Num": 1, "Name": "1 Hz", "Hz": 1}, {"Num": 2, "Name": "10 Hz", "Hz": 10},
    {"Num": 3, "Name": "100 Hz", "Hz": 100}, {"Num": 4, "Name": "1 kHz", "Hz": 1000},
    {"Num": 5, "Name": "5 kHz", "Hz": 5000}, {"Num": 6, "Name": "10 kHz", "Hz": 10000},
])

array("Preamps", [{"Num": 0, "Name": "OFF"}, {"Num": 1, "Name": "+10 dB"},
                  {"Num": 2, "Name": "+20 dB"}, {"Num": 3, "Name": "+30 dB"}])
array("Antennas", [{"Num": 0, "Name": "ANT1"}, {"Num": 1, "Name": "ANT2"}])
array("Attenuators", [{"Num": 0, "Name": "0 dB"}, {"Num": 1, "Name": "-10 dB"}])
array("Filters", [{"Num": n, "Modes": 0, "Name": f"FIL{n}"} for n in (1, 2, 3)])

bands = [  # (Num, name, start, end, range, color) -- Num from wfweb's availableBands
    # 2 m only through a transverter defined in SmartSDR (XVTA): flexCommander
    # moves RX and TX to the XVTR port inside 144-148 MHz.
    (6,  "2m",   144000000, 148000000, 0, "#ff55aaff"),
    (10, "6m",   50000000, 54000000, 60,  "#ffff5500"),
    (11, "10m",  28000000, 29700000, 45,  "#ff00aaff"),
    (12, "12m",  24890000, 24990000, 30,  "#ff00aa00"),
    (13, "15m",  21000000, 21450000, 26,  "#ffff55ff"),
    (14, "17m",  18068000, 18168000, 22,  "#ffaaaa00"),
    (15, "20m",  14000000, 14350000, 20,  "#ffff0000"),
    (16, "30m",  10100000, 10150000, 15,  "#ffffaa00"),
    (17, "40m",  7000000,  7300000,  11,  "#ffaa5500"),
    (18, "60m",  5250000,  5450000,  0,   "#ffaaaaff"),
    (19, "80m",  3500000,  4000000,  6,   "#ff55ffff"),
    (20, "160m", 1800000,  2000000,  2,   "#ffffff00"),
    (21, "630m", 472000,   479000,   0,   "#ffaaff00"),
    (22, "2200m", 135700,  137800,   0,   "#ff5555ff"),
    (23, "Gen",  30000,    54000000, 1.6, "#00000000"),
]
array("Bands", [{"Region": "", "Num": n, "BSR": 0, "Start": s, "End": e, "Range": r,
                 "MemoryGroup": -1, "Name": name, "Bytes": 5,
                 "Power": 0 if name in ("630m", "2200m", "Gen") else 100,
                 "Antennas": "true", "Offset": 0, "Color": c}
                for (n, name, s, e, r, c) in bands])

# flexCommander reports meters already in engineering units; these tables
# only give the page its scale (rated power for Po %, red lines).
meters = []
for rig, act, red in [(0, 0, False), (50, 50, False), (100, 100, True), (120, 120, False)]:
    meters.append({"Meter": "Power", "RigVal": rig, "ActualVal": act, "RedLine": str(red).lower()})
for rig, act, red in [(0, -54, False), (54, 0, True), (114, 60, False)]:
    meters.append({"Meter": "S-Meter", "RigVal": rig, "ActualVal": act, "RedLine": str(red).lower()})
for rig, act, red in [(1, 1, False), (3, 3, True), (6, 6, False)]:
    meters.append({"Meter": "SWR", "RigVal": rig, "ActualVal": act, "RedLine": str(red).lower()})
for rig, act, red in [(0, 0, False), (1, 1, True), (2, 2, False)]:
    meters.append({"Meter": "ALC", "RigVal": rig, "ActualVal": act, "RedLine": str(red).lower()})
for rig, act, red in [(0, 0, False), (10, 10, True), (30, 30, False)]:
    meters.append({"Meter": "Comp", "RigVal": rig, "ActualVal": act, "RedLine": str(red).lower()})
array("Meters", meters)

array("ScopeModes", [{"Num": 0, "Name": "Center Mode"}])

open("rigs/FLEX-6000.rig", "w", encoding="utf-8").write("\n".join(out) + "\n")
print("wrote rigs/FLEX-6000.rig")
