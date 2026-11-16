"""rf2k-web against the mock amp: the hard rules, the commands, the web layer.

Timing constants are shortened so the suite runs in well under a minute.
"""

import asyncio
import json
import os
import socket
import sys
import time
import unittest
import urllib.error
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import ampclient  # noqa: E402
from ampclient import Amp  # noqa: E402
from mock_amp import MockAmp  # noqa: E402
from webapp import WebApp  # noqa: E402

FAST = dict(FIRST_WAKE_DELAY_S=0.3, RECONNECT_STEPS_S=(0.5, 1.0), SILENCE_TIMEOUT_S=3.0,
            NO_ANSWER_SHOW_S=1.0, TX_HOLD_S=1.0)


async def until(pred, timeout=5.0, step=0.05):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        await asyncio.sleep(step)
    return pred()


class Bench(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self._saved = {k: getattr(ampclient, k) for k in FAST}
        for k, v in FAST.items():
            setattr(ampclient, k, v)
        self.mock = MockAmp()
        self.amps = []

    async def asyncTearDown(self):
        for a in self.amps:
            a.set_browsers(0, 0)
            a._go_idle()
            if a._ticker:
                a._ticker.cancel()
        await asyncio.sleep(0.05)

    def tearDown(self):
        self.mock.close()
        for k, v in self._saved.items():
            setattr(ampclient, k, v)

    def amp(self, port=None, **kw):
        kw.setdefault("grace_s", 1)
        kw.setdefault("keep_awake_s", 60)
        a = Amp("127.0.0.1", port or self.mock.port, **kw)
        self.amps.append(a)
        return a

    async def connected(self, a, controllers=1):
        a.set_browsers(1, controllers)
        self.assertTrue(await until(lambda: a.connected and a.operate is not None and a.antennas is not None))


class Rules(Bench):
    async def test_nothing_sent_without_a_browser(self):
        a = self.amp()
        a.set_browsers(0, 0)
        await asyncio.sleep(1.0)
        self.assertEqual(self.mock.gets, [])
        self.assertEqual(a.snapshot(), {"target": a.target, "polling": False, "connected": False})

    async def test_one_request_at_a_time_and_idle_after_grace(self):
        a = self.amp()
        await self.connected(a)
        results = await asyncio.gather(a.command("operate", True), a.command("antenna", 2),
                                       a.command("reset"), a.command("operate", False))
        self.assertTrue(all(r["ok"] for r in results), results)
        self.mock.fwd = 600                         # transmit: 25 ms polling
        await asyncio.sleep(1.0)
        self.mock.fwd = 0
        await asyncio.sleep(0.5)
        self.assertEqual(self.mock.max_in_flight, 1)
        a.set_browsers(0, 0)
        await asyncio.sleep(1.0 + 0.5)              # the grace, then whatever was in flight
        n = len(self.mock.gets) + len(self.mock.puts)
        await asyncio.sleep(1.0)
        self.assertEqual(len(self.mock.gets) + len(self.mock.puts), n, "polled after the last browser left")
        self.assertFalse(a.snapshot()["polling"])

    async def test_commands_in_click_order(self):
        a = self.amp()
        await self.connected(a)
        await asyncio.gather(a.command("antenna", 2), a.command("operate", True), a.command("antenna", 1))
        cmds = [(p[2], p[3]) for p in self.mock.puts if p[2] != "/tuner"]
        self.assertEqual(cmds, [("/antennas/active", '{"type": "INTERNAL", "number": 2}'),
                                ("/operate-mode", '{"operate_mode": "OPERATE"}'),
                                ("/antennas/active", '{"type": "INTERNAL", "number": 1}')])

    async def test_wake_and_tune_api(self):
        a = self.amp()
        await self.connected(a)
        self.assertTrue(await until(lambda: a.tune_api == "supported"))
        wakes = [p for p in self.mock.puts if p[2] == "/tuner" and p[3] == "{}"]
        self.assertEqual(len(wakes), 1)
        self.assertEqual(a.snapshot()["tuneApi"], "supported")

    async def test_keep_awake_repeats(self):
        a = self.amp(keep_awake_s=0.5)
        await self.connected(a)
        await asyncio.sleep(2.0)
        wakes = [p for p in self.mock.puts if p[2] == "/tuner" and p[3] == "{}"]
        self.assertGreaterEqual(len(wakes), 3)

    async def test_no_wake_for_view_only_browsers(self):
        a = self.amp()
        await self.connected(a, controllers=0)
        await asyncio.sleep(1.0)
        self.assertEqual([p for p in self.mock.puts if p[2] == "/tuner"], [])

    async def test_stock_server_has_no_tune(self):
        self.mock.tune_api = False
        a = self.amp()
        await self.connected(a)
        self.assertTrue(await until(lambda: a.tune_api == "unsupported"))
        r = await a.command("tune")
        self.assertFalse(r["ok"])
        self.assertEqual([p for p in self.mock.puts if p[3] != "{}"], [])

    async def test_slow_wake_is_waited_for(self):
        ampclient.SILENCE_TIMEOUT_S = 6.0
        self.mock.wake_delay = 2.5                  # a sleeping amp
        a = self.amp()
        await self.connected(a)
        self.assertTrue(await until(lambda: a.snapshot().get("waking"), 2))
        self.assertTrue(await until(lambda: "silentMs" in a.snapshot(), 3))
        self.assertTrue(await until(lambda: not a.snapshot().get("waking"), 4))
        self.assertTrue(a.connected)
        self.assertNotIn("silentMs", a.snapshot())
        self.assertEqual(self.mock.broken_pipes, 0)
        self.assertEqual(a.tune_api, "supported")

    async def test_silence_then_offline_then_back(self):
        a = self.amp()
        await self.connected(a)
        self.mock.answering.clear()                 # the amp is switched off
        self.assertTrue(await until(lambda: "silentMs" in a.snapshot(), 2))
        self.assertTrue(a.connected, "one silent request must not drop the link at once")
        self.assertTrue(await until(lambda: not a.connected, 4))
        s = a.snapshot()
        self.assertEqual(s["offlineReason"], "noanswer")
        self.assertIn("not answering", s["offlineText"])
        self.mock.answering.set()
        self.assertTrue(await until(lambda: a.connected, 6))

    async def test_refused(self):
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        a = self.amp(port=port)
        a.set_browsers(1, 1)
        self.assertTrue(await until(lambda: a.snapshot().get("offlineReason") == "refused"))
        self.assertEqual(a.snapshot()["offlineText"], "Amp program not running")

    async def test_not_an_rfkit(self):
        self.mock.garbage = {"/info"}
        a = self.amp()
        a.set_browsers(1, 1)
        self.assertTrue(await until(lambda: a.snapshot().get("offlineReason") == "notrfkit"))

    async def test_backoff_then_recovery(self):
        self.mock.garbage = {"/info"}
        a = self.amp()
        a.set_browsers(1, 1)
        await asyncio.sleep(2.2)                    # tries at 0, +0.5, +1.0
        probes = [g for g in self.mock.gets if g[1] == "/info"]
        self.assertTrue(2 <= len(probes) <= 4, probes)
        self.mock.garbage = set()
        self.assertTrue(await until(lambda: a.connected, 3))

    async def test_malformed_body_is_ignored(self):
        a = self.amp()
        await self.connected(a)
        before = a.snapshot()
        self.assertFalse(a._apply("/power", b'{"forward":{"value":"lots"}}'))
        self.assertEqual(a.snapshot()["fwd"], before["fwd"])

    async def test_fields_unreported_are_absent(self):
        a = self.amp()
        a.active = a.connected = True               # connected, nothing read yet
        s = a.snapshot()
        for k in ("fwd", "ref", "swr", "temp", "operate", "band", "freq", "tuner", "iface", "antennas"):
            self.assertNotIn(k, s)                  # the page shows a dash, never 0


class Schedule(Bench):
    async def test_transmit_polls_power_fast(self):
        a = self.amp()
        await self.connected(a)
        await asyncio.sleep(0.5)
        self.mock.fwd = 800
        self.assertTrue(await until(lambda: a.transmitting))
        await asyncio.sleep(0.3)
        t0 = time.monotonic()
        await asyncio.sleep(1.0)
        gets = [g[1] for g in self.mock.gets if g[0] >= t0]
        self.assertGreater(len(gets), 15, gets)
        self.assertGreaterEqual(gets.count("/power") / len(gets), 0.7)
        self.assertGreaterEqual(gets.count("/data"), len(gets) // 5 - 1)
        self.mock.fwd = 0
        self.assertTrue(await until(lambda: not a.fast(), 3))
        t1 = time.monotonic()
        await asyncio.sleep(1.0)
        rx = [g for g in self.mock.gets if g[0] >= t1]
        self.assertLessEqual(len(rx), 6)            # 250 ms ticks

    async def test_receive_reads_data_every_three_ticks(self):
        a = self.amp()
        await self.connected(a)
        await asyncio.sleep(0.5)
        t0 = time.monotonic()
        await asyncio.sleep(3.0)
        gets = [g[1] for g in self.mock.gets if g[0] >= t0]
        self.assertGreaterEqual(gets.count("/data"), 3)

    async def test_unrequested_standby_reads_data_next(self):
        a = self.amp()
        a.operate = "OPERATE"
        a._apply("/operate-mode", b'{"operate_mode":"STANDBY"}')
        self.assertTrue(a._data_next)
        b = self.amp()
        b.operate = "OPERATE"
        b._standby_asked = True
        b._apply("/operate-mode", b'{"operate_mode":"STANDBY"}')
        self.assertFalse(b._data_next)

    async def test_power_collapse_mid_over_reads_data_next(self):
        a = self.amp()
        a._apply("/power", self._power(900))
        self.assertTrue(a.transmitting)
        a._apply("/power", self._power(0))
        self.assertTrue(a._data_next)

    @staticmethod
    def _power(w):
        return json.dumps({"temperature": {"value": 40, "unit": "C"}, "voltage": {"value": 50, "unit": "V"},
                           "current": {"value": 1, "unit": "A"}, "forward": {"value": w, "unit": "W"},
                           "reflected": {"value": 0, "unit": "W"}, "swr": {"value": 1.0, "unit": ""}}).encode()


class Commands(Bench):
    async def test_refusal_reason_comes_back(self):
        self.mock.refuse_operate = "currently disabled"
        a = self.amp()
        await self.connected(a)
        r = await a.command("operate", True)
        self.assertEqual(r, {"ok": False, "reason": "currently disabled", "what": "OPERATE"})

    async def test_state_comes_from_the_readback(self):
        a = self.amp()
        await self.connected(a)
        self.mock.operate = "STANDBY"
        r = await a.command("operate", True)
        self.assertTrue(r["ok"])
        self.assertTrue(await until(lambda: a.operate == "OPERATE", 2))
        put_t = [p[0] for p in self.mock.puts if p[2] == "/operate-mode"][-1]
        self.assertTrue([g for g in self.mock.gets if g[0] > put_t and g[1] == "/operate-mode"])

    async def test_antenna_rules(self):
        a = self.amp()
        await self.connected(a)
        self.assertFalse((await a.command("antenna", 7))["ok"])
        self.assertFalse((await a.command("antenna", "ext"))["ok"])
        r = await a.command("antenna", 3)             # DISABLED on the mock
        self.assertEqual(r["reason"], "antenna not available")
        self.assertTrue((await a.command("antenna", 2))["ok"])
        self.assertTrue(await until(lambda: any(x["num"] == 2 and x["state"] == "active" for x in a.antennas), 2))

    async def test_tune_once_and_tuning_readback(self):
        a = self.amp()
        await self.connected(a)
        self.assertTrue(await until(lambda: a.tune_api == "supported"))
        r1, r2 = await asyncio.gather(a.command("tune"), a.command("tune"))
        self.assertTrue(r1["ok"])
        self.assertFalse(r2["ok"])
        starts = [p for p in self.mock.puts if p[2] == "/tuner" and "START" in p[3]]
        self.assertEqual(len(starts), 1)
        self.assertTrue(await until(lambda: a.snapshot().get("tuner", {}).get("mode") == "TUNING", 2))
        self.assertTrue(await until(lambda: a.snapshot()["tuner"]["mode"] == "AUTO", 5))

    async def test_reset_clears_error(self):
        self.mock.status = "High SWR"
        a = self.amp()
        await self.connected(a)
        self.assertTrue(await until(lambda: a.snapshot().get("ampError") == "High SWR"))
        self.assertTrue((await a.command("reset"))["ok"])
        self.assertTrue(await until(lambda: a.snapshot().get("ampError") == "", 2))

    async def test_interface_error_is_a_fault(self):
        self.mock.iface_error = "link lost"
        a = self.amp()
        await self.connected(a)
        self.assertTrue(await until(lambda: a.snapshot().get("ampError") == "link lost"))


class Web(Bench):
    async def asyncSetUp(self):
        self.a = self.amp()
        self.app = WebApp(self.a, password="s3cret")
        self.server = await asyncio.start_server(self.app.handle, "127.0.0.1", 0)
        self.port = self.server.sockets[0].getsockname()[1]
        self.a.set_browsers(0, 0)

    async def asyncTearDown(self):
        self.server.close()
        await super().asyncTearDown()

    def http(self, method, path, body=None, ctype="application/json", cookie=None):
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}{path}", method=method,
                                     data=json.dumps(body).encode() if body is not None else None)
        if ctype:
            req.add_header("Content-Type", ctype)
        if cookie:
            req.add_header("Cookie", cookie)
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                return r.status, json.loads(r.read() or b"{}"), r.headers
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}"), e.headers

    async def call(self, *a, **kw):
        return await asyncio.to_thread(self.http, *a, **kw)

    async def sign_in(self):
        st, _, h = await self.call("POST", "/api/login", {"password": "s3cret"})
        self.assertEqual(st, 200)
        self.assertIn("HttpOnly", h["Set-Cookie"])
        return h["Set-Cookie"].split(";")[0]

    async def open_events(self, cookie=None):
        r, w = await asyncio.open_connection("127.0.0.1", self.port)
        w.write(b"GET /api/events HTTP/1.1\r\nHost: x\r\n" +
                (f"Cookie: {cookie}\r\n".encode() if cookie else b"") + b"\r\n")
        await w.drain()
        return r, w

    async def read_until(self, r, needle, timeout=5):
        got = b""
        end = time.monotonic() + timeout
        while needle not in got and time.monotonic() < end:
            chunk = await asyncio.wait_for(r.read(4096), timeout)
            if not chunk:
                break
            got += chunk
        return got

    async def test_page_loads_but_shows_nothing_without_sign_in(self):
        st, page = await asyncio.to_thread(lambda: (lambda r: (r.status, r.read()))(
            urllib.request.urlopen(f"http://127.0.0.1:{self.port}/", timeout=5)))
        self.assertEqual(st, 200)
        self.assertIn(b'id="login"', page)
        st, body, _ = await self.call("GET", "/api/state", ctype=None)
        self.assertEqual(st, 401)
        self.assertNotIn("target", body)
        r, w = await self.open_events()
        got = await self.read_until(r, b"\r\n\r\n")
        self.assertIn(b" 401 ", got)
        self.assertNotIn(b"event:", got)
        w.close()
        await asyncio.sleep(0.2)
        self.assertEqual(self.a.browsers, 0)
        self.assertEqual(self.mock.gets, [])      # the amp is not even contacted

    async def test_control_needs_sign_in(self):
        st, _, _ = await self.call("POST", "/api/command", {"cmd": "operate", "value": True})
        self.assertEqual(st, 401)
        st, body, _ = await self.call("POST", "/api/login", {"password": "nope"})
        self.assertEqual((st, body["error"]), (401, "Wrong password"))
        cookie = await self.sign_in()
        st, body, _ = await self.call("GET", "/api/state", ctype=None, cookie=cookie)
        self.assertEqual(st, 200)
        self.assertTrue(body["session"]["canControl"])
        st, body, _ = await self.call("POST", "/api/command", {"cmd": "operate", "value": True}, cookie=cookie)
        self.assertEqual((st, body["ok"]), (200, False))   # offline: no browser stream open
        self.assertEqual(self.mock.puts, [])

    async def test_forged_cookie_refused(self):
        st, _, _ = await self.call("GET", "/api/state", ctype=None, cookie="rf2k_session=guess")
        self.assertEqual(st, 401)

    async def test_cross_site_posts_refused(self):
        st, _, _ = await self.call("POST", "/api/login", {"password": "s3cret"}, ctype="text/plain")
        self.assertEqual(st, 415)

    async def test_signed_in_stream_then_sign_out_ends_it(self):
        cookie = await self.sign_in()
        r, w = await self.open_events(cookie)
        got = await self.read_until(r, b'"connected":true')
        self.assertIn(b"event: session", got)
        self.assertIn(b'"connected":true', got)
        self.assertEqual((self.a.browsers, self.a.controllers), (1, 1))
        st, _, _ = await self.call("POST", "/api/logout", {}, cookie=cookie)
        self.assertEqual(st, 200)
        self.assertTrue(await until(lambda: self.a.browsers == 0, 3), "stream kept running after sign-out")
        w.close()
        st, _, _ = await self.call("GET", "/api/state", ctype=None, cookie=cookie)
        self.assertEqual(st, 401)

    async def test_no_password_is_open(self):
        app = WebApp(self.a)
        server = await asyncio.start_server(app.handle, "127.0.0.1", 0)
        port = server.sockets[0].getsockname()[1]
        try:
            st, body = await asyncio.to_thread(lambda: (lambda r: (r.status, json.loads(r.read())))(
                urllib.request.urlopen(f"http://127.0.0.1:{port}/api/state", timeout=5)))
            self.assertEqual(st, 200)
            self.assertEqual(body["session"], {"canControl": True, "loginRequired": False,
                                               "signedIn": False, "readOnly": False})
        finally:
            server.close()

    async def test_read_only_with_password(self):
        self.app.read_only = True
        cookie = await self.sign_in()
        st, body, _ = await self.call("POST", "/api/command", {"cmd": "reset"}, cookie=cookie)
        self.assertEqual((st, body["error"]), (403, "View only"))

if __name__ == "__main__":
    unittest.main()
