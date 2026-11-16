#!/usr/bin/env python3
"""A fake RF2K-S REST server, for developing rf2k-web without the real amp.

Like the real one it is a single-threaded wsgiref-style server (one connection
at a time) and, like VE3NSV's modified server, it answers PUT /tuner {} with
400 and {"tuner_mode":"START"} with a tune. Run it on its own to drive the page:

    python3 mock_amp.py --port 8090 --simulate
    python3 rf2kweb.py --amp 127.0.0.1:8090

--simulate keys a voice-like over every 20 s, with a fault now and then.
The tests use the MockAmp class directly and set its knobs.
"""

from __future__ import annotations

import argparse
import json
import math
import random
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer


class MockAmp:
    def __init__(self, host="127.0.0.1", port=0):
        self.lock = threading.Lock()
        self.gets = []              # (monotonic, path)
        self.puts = []              # (monotonic, method, path, body)
        self.in_flight = 0
        self.max_in_flight = 0      # >1 would mean the client broke rule 1
        self.broken_pipes = 0       # answers the client hung up on
        self.answering = threading.Event()
        self.answering.set()        # cleared: requests get no answer (amp off)
        self.delay = 0.0            # every answer is this late
        self.wake_delay = 0.0       # the wake answers this late (a sleeping amp: ~5 s)
        self.tune_api = True        # False: a stock server, PUT /tuner is 405
        self.refuse_operate = None  # a reason: PUT /operate-mode answers 409
        self.garbage = set()        # paths that answer a non-RF-KIT body

        self.name = "Shack amp"
        self.operate = "STANDBY"
        self.status = ""
        self.iface_error = ""
        self.fwd = 0
        self.ref = 0
        self.temp = 38.0
        self.band, self.freq = 20, 14200
        self.tuner_mode, self.setup = "AUTO", "CL"
        self.tune_until = 0.0
        self.active_ant = 1
        self.antennas = {1: "AVAILABLE", 2: "AVAILABLE", 3: "DISABLED"}
        mock = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.0"     # like wsgiref: one request per connection

            def log_message(self, *a):
                pass

            def _reply(self, code, obj):
                body = (obj if isinstance(obj, bytes) else json.dumps(obj).encode())
                try:
                    self.send_response(code)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    with mock.lock:
                        mock.broken_pipes += 1

            def _enter(self):
                with mock.lock:
                    mock.in_flight += 1
                    mock.max_in_flight = max(mock.max_in_flight, mock.in_flight)
                mock.answering.wait()
                if mock.delay:
                    time.sleep(mock.delay)

            def _leave(self):
                with mock.lock:
                    mock.in_flight -= 1

            def do_GET(self):
                with mock.lock:
                    mock.gets.append((time.monotonic(), self.path))
                self._enter()
                try:
                    if self.path in mock.garbage:
                        self._reply(200, b"<html>router login</html>")
                        return
                    code, obj = mock.get(self.path)
                    self._reply(code, obj)
                finally:
                    self._leave()

            def _write(self, method):
                n = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(n).decode() if n else ""
                with mock.lock:
                    mock.puts.append((time.monotonic(), method, self.path, body))
                self._enter()
                try:
                    code, obj = mock.write(method, self.path, body)
                    self._reply(code, obj)
                finally:
                    self._leave()

            def do_PUT(self):
                self._write("PUT")

            def do_POST(self):
                self._write("POST")

        self.httpd = HTTPServer((host, port), Handler)   # one connection at a time
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.answering.set()
        self.httpd.shutdown()
        self.httpd.server_close()

    # ── the API ──

    def get(self, path):
        with self.lock:
            if self.tuner_mode == "AUTO_TUNING_FROM_AUTO" and time.monotonic() > self.tune_until:
                self.tuner_mode, self.setup = "AUTO", "CL"
            swr = 1.0
            if self.fwd > 0 and self.ref < self.fwd:
                rho = math.sqrt(self.ref / self.fwd)
                swr = (1 + rho) / (1 - rho)
            if path == "/info":
                return 200, {"device": "RFKIT", "software_version": {"GUI": 12, "controller": 34},
                             "custom_device_name": self.name}
            if path == "/power":
                return 200, {"temperature": {"value": round(self.temp, 1), "unit": "°C"},
                             "voltage": {"value": 50.2 if self.fwd < 1 else 48.9, "unit": "V"},
                             "current": {"value": round(0.4 + self.fwd / 45, 1), "unit": "A"},
                             "forward": {"value": int(self.fwd), "max_value": 1500, "unit": "W"},
                             "reflected": {"value": int(self.ref), "max_value": 300, "unit": "W"},
                             "swr": {"value": round(swr, 2), "max_value": 10.0, "unit": ""}}
            if path == "/data":
                return 200, {"band": {"value": self.band, "unit": "m"},
                             "frequency": {"value": self.freq, "unit": "kHz"}, "status": self.status}
            if path == "/tuner":
                return 200, {"mode": self.tuner_mode, "setup": self.setup,
                             "L": {"value": 755, "unit": "nH"}, "C": {"value": 439, "unit": "pF"},
                             "tuned_frequency": {"value": self.freq, "unit": "kHz"},
                             "segment_size": {"value": 50, "unit": "kHz"}}
            if path == "/operate-mode":
                return 200, {"operate_mode": self.operate}
            if path == "/operational-interface":
                return 200, {"operational_interface": "TCI", "error": self.iface_error}
            if path == "/antennas":
                ants = [{"type": "INTERNAL", "number": n,
                         "state": "ACTIVE" if n == self.active_ant else s}
                        for n, s in sorted(self.antennas.items())]
                ants.append({"type": "EXTERNAL", "state": "AVAILABLE"})
                return 200, {"antennas": ants}
            if path == "/antennas/active":
                return 200, {"type": "INTERNAL", "number": self.active_ant}
        return 404, {"status": "404 Not Found", "reason": "no such endpoint"}

    def write(self, method, path, body):
        try:
            data = json.loads(body) if body else {}
        except ValueError:
            return 400, {"status": "400 Bad Request", "reason": "invalid JSON"}
        if method == "PUT" and path == "/tuner":
            if not self.tune_api:
                return 405, {"status": "405 Method Not Allowed", "reason": "Method not allowed"}
            if data.get("tuner_mode") != "START":
                if self.wake_delay:
                    time.sleep(self.wake_delay)
                return 400, {"status": "400 Bad Request", "reason": "START required"}
            with self.lock:
                if self.tuner_mode not in ("AUTO", "MANUAL"):
                    return 409, {"status": "409 Conflict", "reason": "tuner not in AUTO or MANUAL"}
                self.tuner_mode = "AUTO_TUNING_FROM_AUTO"
                self.tune_until = time.monotonic() + 2.0
            return 200, {"mode": "AUTO_TUNING_FROM_AUTO", "setup": self.setup}
        if method == "PUT" and path == "/operate-mode":
            if self.refuse_operate:
                return 409, {"status": "409 Conflict", "reason": self.refuse_operate}
            m = data.get("operate_mode")
            if m not in ("OPERATE", "STANDBY"):
                return 400, {"status": "400 Bad Request", "reason": "invalid operate_mode"}
            with self.lock:
                if m == "OPERATE" and self.status:
                    return 409, {"status": "409 Conflict", "reason": "amplifier is in error state"}
                self.operate = m
            return 200, {"operate_mode": m}
        if method == "PUT" and path == "/antennas/active":
            n = data.get("number")
            with self.lock:
                if data.get("type") != "INTERNAL" or self.antennas.get(n) not in ("AVAILABLE",):
                    return 409, {"status": "409 Conflict", "reason": "antenna not available"}
                self.active_ant = n
            return 200, {"type": "INTERNAL", "number": n}
        if method == "POST" and path == "/error/reset":
            with self.lock:
                self.status = ""
                self.iface_error = ""
                self.operate = "STANDBY"
            return 200, {}
        return 405, {"status": "405 Method Not Allowed", "reason": "Method not allowed"}

    # ── stand-alone simulation ──

    def simulate(self):
        """Every 20 s: a 6 s over of syllable-shaped power; now and then a
        High SWR trip that drops the amp to STANDBY."""
        t0 = time.monotonic()
        while True:
            t = (time.monotonic() - t0) % 20.0
            with self.lock:
                keyed = 10.0 < t < 16.0 and self.operate == "OPERATE" and not self.status
                if keyed:
                    syllable = max(0.0, math.sin(t * 9.0)) * (0.6 + 0.4 * random.random())
                    self.fwd = int(1250 * syllable)
                    self.ref = int(self.fwd * 0.012)
                    self.temp = min(65.0, self.temp + 0.02)
                    if random.random() < 0.0001:
                        self.status, self.operate, self.fwd, self.ref = "High SWR", "STANDBY", 0, 0
                else:
                    self.fwd = self.ref = 0
                    self.temp = max(36.0, self.temp - 0.01)
            time.sleep(0.02)


def main():
    ap = argparse.ArgumentParser(description="Fake RF2K-S REST server")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8090)
    ap.add_argument("--simulate", action="store_true", help="key a voice-like over every 20 s (in OPERATE)")
    args = ap.parse_args()
    amp = MockAmp(args.host, args.port)
    print(f"mock RF2K-S on http://{args.host}:{amp.port}/")
    if args.simulate:
        threading.Thread(target=amp.simulate, daemon=True).start()
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        amp.close()


if __name__ == "__main__":
    main()
