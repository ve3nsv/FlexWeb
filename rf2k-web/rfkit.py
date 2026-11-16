"""RF-KIT RF2K-S REST API codec: parse what the amp sends, build what we send.

A Python port of FlexWeb's include/rfkitprotocol.h / src/rfkitprotocol.cpp
(itself from VE3NSV's AetherSDR rfkit-tune-pr branch). Stateless: one parse
function per endpoint (bytes in, dict out or None) plus the presentation
helpers the page needs.

Protocol authority: the vendor's "RFKIT API and SDK Documentation" (OpenAPI
0.9.0). PUT /tuner is not in it; only a modified REST server has it.

Everything here arrives from a LAN peer: bodies are size-capped, a document
that is not the expected shape yields None (never a half-filled guess), and a
control value we cannot interpret (operate mode, interface, antenna type) is
refused rather than guessed. Descriptive fields (unit, max_value, L/C) default
quietly when absent.
"""

from __future__ import annotations

import json
import math

DEFAULT_PORT = 8080
MAX_BODY_BYTES = 64 * 1024

MIN_INTERNAL_ANTENNA = 1
MAX_INTERNAL_ANTENNA = 4
MIN_EXTERNAL_PORT = 1
MAX_EXTERNAL_PORT = 16

PATH_INFO = "/info"
PATH_POWER = "/power"
PATH_DATA = "/data"
PATH_TUNER = "/tuner"
PATH_ANTENNAS = "/antennas"
PATH_ANTENNA_ACTIVE = "/antennas/active"
PATH_OPERATE_MODE = "/operate-mode"
PATH_INTERFACE = "/operational-interface"
PATH_ERROR_RESET = "/error/reset"

# RF2K-S manual 7.2: tuning needs 4-39 W of drive, 10 W or more recommended.
TUNE_MIN_W = 4
TUNE_MAX_W = 39
TUNE_RECOMMENDED_W = 10


# ── low-level helpers ────────────────────────────────────────────────────

def _load(body: bytes):
    if not body or len(body) > MAX_BODY_BYTES:
        return None
    try:
        return json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        return None


def _object(body: bytes):
    doc = _load(body)
    return doc if isinstance(doc, dict) else None


def _number(v):
    # bool is an int in Python; JSON true is not a measurement.
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return None
    if not math.isfinite(v) or abs(v) > 1.0e9:
        return None
    return v


def _str(v, fallback=""):
    return v if isinstance(v, str) else fallback


def _value(v, integer=False):
    """{"value": n, "unit": "..."[, "max_value": m]} -> dict or None."""
    if not isinstance(v, dict):
        return None
    n = _number(v.get("value"))
    if n is None:
        return None
    out = {"value": int(round(n)) if integer else float(n), "unit": _str(v.get("unit"))}
    m = _number(v.get("max_value"))
    if m is not None and m > 0:
        out["max"] = int(round(m)) if integer else float(m)
    return out


# ── parsers ──────────────────────────────────────────────────────────────

def parse_info(body: bytes):
    o = _object(body)
    if o is None or not isinstance(o.get("device"), str):
        return None
    sw = o.get("software_version")
    sw = sw if isinstance(sw, dict) else {}
    gui, ctl = _number(sw.get("GUI")), _number(sw.get("controller"))
    return {
        "device": o["device"],
        "custom_name": _str(o.get("custom_device_name")),
        "gui_version": int(gui) if gui is not None else None,
        "controller_version": int(ctl) if ctl is not None else None,
    }


def parse_power(body: bytes):
    o = _object(body)
    if o is None:
        return None
    out = {
        "temperature": _value(o.get("temperature")),
        "voltage": _value(o.get("voltage")),
        "current": _value(o.get("current")),
        "forward": _value(o.get("forward"), integer=True),
        "reflected": _value(o.get("reflected"), integer=True),
        "swr": _value(o.get("swr")),
    }
    if any(v is None for v in out.values()):
        return None
    return out


def parse_data(body: bytes):
    o = _object(body)
    if o is None:
        return None
    band = _value(o.get("band"), integer=True)
    freq = _value(o.get("frequency"), integer=True)
    if band is None or freq is None:
        return None
    return {"band": band, "frequency": freq, "status": _str(o.get("status"))}


def _tuner_mode(v):
    # The API document lists the literal `false` (a YAML "off" artifact).
    if isinstance(v, bool):
        return "OFF" if not v else "UNKNOWN"
    s = _str(v)
    if s.startswith("AUTO_TUNING"):
        return "TUNING"
    if s in ("BYPASS", "MANUAL", "AUTO", "OFF"):
        return s
    if s == "false":
        return "OFF"
    return "UNKNOWN"


def _tuner_setup(s):
    s = _str(s)
    return s if s in ("NOT TUNED", "BYPASS", "CL", "LC") else "UNKNOWN"


def parse_tuner(body: bytes):
    o = _object(body)
    if o is None or "mode" not in o:
        return None
    return {
        "mode": _tuner_mode(o.get("mode")),
        "setup": _tuner_setup(o.get("setup")),
        "L": _value(o.get("L"), integer=True),
        "C": _value(o.get("C"), integer=True),
        "tuned_frequency": _value(o.get("tuned_frequency"), integer=True),
        "segment_size": _value(o.get("segment_size"), integer=True),
    }


def parse_operate_mode(body: bytes):
    o = _object(body)
    if o is None:
        return None
    m = o.get("operate_mode")
    return m if m in ("OPERATE", "STANDBY") else None


def parse_interface(body: bytes):
    o = _object(body)
    if o is None:
        return None
    name = o.get("operational_interface")
    if name not in ("UNIV", "CAT", "UDP", "TCI"):
        return None
    return {"interface": name, "error": _str(o.get("error"))}


def _antenna(o):
    """{"type":..., "number":...} -> ("INTERNAL", n) / ("EXTERNAL", 0) or None."""
    t = o.get("type")
    if t == "EXTERNAL":
        return ("EXTERNAL", 0)
    n = _number(o.get("number"))
    if t != "INTERNAL" or n is None or n != int(n):
        return None
    n = int(n)
    if not MIN_INTERNAL_ANTENNA <= n <= MAX_INTERNAL_ANTENNA:
        return None
    return ("INTERNAL", n)


def parse_antenna(body: bytes):
    o = _object(body)
    return _antenna(o) if o is not None else None


def parse_antennas(body: bytes):
    """The /antennas list: a bare array, or the {"antennas":[...]} a real
    RF2K-S sends. Malformed entries are skipped; None only if it is neither."""
    doc = _load(body)
    if isinstance(doc, list):
        entries = doc
    elif isinstance(doc, dict) and isinstance(doc.get("antennas"), list):
        entries = doc["antennas"]
    else:
        return None
    out = []
    for e in entries:
        if not isinstance(e, dict):
            continue
        a = _antenna(e)
        state = e.get("state")
        if a is None or state not in ("ACTIVE", "AVAILABLE", "DISABLED"):
            continue
        item = {"external": a[0] == "EXTERNAL", "num": a[1], "state": state.lower()}
        if item["external"]:
            port = _number(e.get("number"))
            if port is not None and port == int(port) and MIN_EXTERNAL_PORT <= port <= MAX_EXTERNAL_PORT:
                item["port"] = int(port)
        out.append(item)
    return out


def parse_api_error(body: bytes):
    o = _object(body)
    if o is None:
        return None
    return {"status": _str(o.get("status")), "reason": _str(o.get("reason"))}


# ── request bodies ───────────────────────────────────────────────────────

def operate_body(operate: bool) -> bytes:
    return json.dumps({"operate_mode": "OPERATE" if operate else "STANDBY"}).encode()


def antenna_body(num: int):
    """Only internal antennas 1-4 can be selected; EXTERNAL is an indicator."""
    if isinstance(num, bool) or not isinstance(num, int):
        return None
    if not MIN_INTERNAL_ANTENNA <= num <= MAX_INTERNAL_ANTENNA:
        return None
    return json.dumps({"type": "INTERNAL", "number": num}).encode()


TUNER_START_BODY = b'{"tuner_mode":"START"}'
TUNER_WAKE_BODY = b"{}"


def classify_tune_probe(http_status: int) -> str:
    """What the wake reply says about PUT /tuner."""
    if 200 <= http_status < 300 or http_status == 400:
        return "supported"
    if http_status in (404, 405, 501):
        return "unsupported"
    return "unknown"


# ── presentation ─────────────────────────────────────────────────────────

def tuner_pill(tuner) -> str:
    """The ATU tile text (KPA1500 wording, as in AetherSDR / FlexWeb)."""
    mode, setup = tuner["mode"], tuner["setup"]
    if mode == "AUTO":
        return "ATU NO TUNE" if setup == "NOT TUNED" else "ATU IN"
    if mode == "TUNING":
        return "ATU IN"
    if mode == "MANUAL":
        if setup == "BYPASS":
            return "ATU MAN BYP"
        if setup == "NOT TUNED":
            return "ATU NO TUNE"
        return "ATU MAN"
    if mode == "BYPASS":
        return "ATU BYP"
    if mode == "OFF":
        return "ATU OFF"
    return "ATU —"


def tuner_pill_in_use(pill: str) -> bool:
    return pill in ("ATU IN", "ATU MAN")


def tuner_pill_alert(pill: str) -> bool:
    return pill == "ATU NO TUNE"


def tune_allowed(tuner) -> bool:
    return tuner is not None and tuner["mode"] in ("AUTO", "MANUAL")


def tune_drive(power) -> str:
    unit = power["forward"]["unit"].strip()
    if unit and unit.lower() != "w":
        return "unknown"
    w = power["forward"]["value"]
    if w < TUNE_MIN_W or w > TUNE_MAX_W:
        return "outside"
    return "low" if w < TUNE_RECOMMENDED_W else "good"


def watts(v):
    """A power reading in W, or None when the amp uses another unit."""
    unit = v["unit"].strip().lower()
    if unit in ("", "w"):
        return v["value"]
    if unit == "kw":
        return v["value"] * 1000
    return None


def temperature_celsius(t) -> float:
    unit = t["unit"].upper()
    if "F" in unit and "C" not in unit:
        return (t["value"] - 32.0) * 5.0 / 9.0
    return t["value"]


def format_frequency(f) -> str:
    unit = f["unit"].strip().lower()
    scale = {"hz": 1e-6, "khz": 1e-3, "mhz": 1.0}.get(unit)
    if scale is None:
        return f"{f['value']} {f['unit']}".strip()
    return f"{f['value'] * scale:.3f} MHz"


def format_band(b) -> str:
    return f"{b['value']} {b['unit']}".strip()


def format_value(v) -> str:
    return f"{v['value']} {v['unit']}".strip()
