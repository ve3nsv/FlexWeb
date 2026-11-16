"""The one client of the RF2K-S: a single-request HTTP queue and its poller.

A Python/asyncio port of FlexWeb's RfkitConnection (src/rfkitconnection.cpp)
plus the amp-side parts of its webServer (wake, keep-awake, idle grace).

The amp's REST server (Python wsgiref) serves one connection at a time and has
no idle timeout, so the rules here are strict:

* One request at a time, ever. Polls, wakes and commands share one queue; the
  next request goes out only after the previous one has finished.
* A request is never abandoned early. It times out only after 15 s with no
  bytes (the timer restarts whenever bytes arrive): a sleeping amp takes ~5 s
  to answer, and every connection closed before its answer makes the amp log a
  BrokenPipeError (enough of those and it stopped answering for minutes). Even
  when polling stops, the request in flight is allowed to finish.
* One GET per poll tick, only when nothing is queued or in flight: a slow amp
  lowers the poll rate instead of growing a backlog.
* Nothing is sent while no browser is open (after a grace period).
"""

from __future__ import annotations

import asyncio
import errno
import json
import logging
import socket
import time
from collections import deque

import rfkit

log = logging.getLogger("rf2k")

SILENCE_TIMEOUT_S = 15.0      # no bytes for this long: the request has failed
NO_ANSWER_SHOW_S = 2.0        # no bytes for this long: the page says NO ANSWER
RX_TICK_S = 0.250
TX_TICK_S = 0.025
TX_HOLD_S = 3.0               # fast polling continues this long after unkey
SLOW_PATH_S = 10.0            # /antennas, /operational-interface
TRIP_FROM_W = 5               # FWD falling from >= this to < 1 W mid-over: read /data
MAX_CONSECUTIVE_FAILURES = 3
RECONNECT_STEPS_S = (5, 15, 30, 60)
FIRST_WAKE_DELAY_S = 3.0
TUNE_WATCH_S = 30.0           # read /tuner during TX for this long after TUNE

# Receive schedule, one GET per 250 ms tick (a 3 s cycle): /power and /data
# every 0.75 s (/power is what notices a key-up; /data is the fault text),
# /operate-mode every 1.5 s, /tuner every 3 s plus the spare slot. The spare
# (None) reads /antennas or /operational-interface when 10 s old, else /tuner.
RX_SCHEDULE = (
    rfkit.PATH_POWER, rfkit.PATH_DATA, rfkit.PATH_TUNER,
    rfkit.PATH_POWER, rfkit.PATH_DATA, rfkit.PATH_OPERATE_MODE,
    rfkit.PATH_POWER, rfkit.PATH_DATA, None,
    rfkit.PATH_POWER, rfkit.PATH_DATA, rfkit.PATH_OPERATE_MODE,
)
# Transmit schedule, one GET per 25 ms tick: /power four times in five, /data
# once. While a tune is being watched the second /power reads /tuner instead,
# so TUNING shows from the amp's own readback while the carrier is up.
TX_SCHEDULE = (rfkit.PATH_POWER, rfkit.PATH_POWER, rfkit.PATH_DATA, rfkit.PATH_POWER, rfkit.PATH_POWER)
TX_TUNER_SLOT = 1

OFFLINE_TEXT = {
    "refused": "Amp program not running",
    "noanswer": "Amp not answering — busy, another program may be using it, or it is off",
    "noroute": "Amp unreachable — the network cannot reach it",
    "notrfkit": "Something other than an RF-KIT answers at that address",
}


def now() -> float:
    return time.monotonic()


# ── HTTP/1.1, one exchange per connection ────────────────────────────────

class TransportError(Exception):
    """The exchange did not complete. kind is one of OFFLINE_TEXT's keys."""

    def __init__(self, kind: str, message: str, silent_timeout: bool = False):
        super().__init__(message)
        self.kind = kind
        self.silent_timeout = silent_timeout


_NOROUTE_ERRNOS = {errno.EHOSTUNREACH, errno.ENETUNREACH, errno.EADDRNOTAVAIL,
                   getattr(errno, "EHOSTDOWN", -1), getattr(errno, "ENETDOWN", -1)}


def _classify_oserror(e: OSError) -> TransportError:
    if isinstance(e, ConnectionRefusedError):
        return TransportError("refused", "connection refused")
    if isinstance(e, socket.gaierror):
        return TransportError("noroute", f"cannot resolve the address ({e})")
    if e.errno in _NOROUTE_ERRNOS:
        return TransportError("noroute", e.strerror or str(e))
    if e.errno == errno.ETIMEDOUT:
        return TransportError("noanswer", "connection timed out")
    if isinstance(e, (ConnectionResetError, BrokenPipeError, ConnectionAbortedError)):
        return TransportError("noanswer", "connection reset")
    return TransportError("noroute", e.strerror or str(e))


def _parse_response(buf: bytes, eof: bool):
    """(status, body) once buf holds a complete response, else None.
    Raises TransportError("notrfkit") for something that is not HTTP."""
    end = buf.find(b"\r\n\r\n")
    if end < 0:
        if len(buf) > 16 * 1024 or (eof and buf):
            raise TransportError("notrfkit", "not an HTTP answer")
        return None
    head = buf[:end].decode("iso-8859-1").split("\r\n")
    parts = head[0].split(" ", 2)
    if len(parts) < 2 or not parts[0].startswith("HTTP/") or not parts[1].isdigit():
        raise TransportError("notrfkit", "not an HTTP answer")
    status = int(parts[1])
    headers = {}
    for line in head[1:]:
        k, _, v = line.partition(":")
        headers[k.strip().lower()] = v.strip()
    body = buf[end + 4:]
    if "chunked" in headers.get("transfer-encoding", "").lower():
        out, pos = bytearray(), 0
        while True:
            nl = body.find(b"\r\n", pos)
            if nl < 0:
                break
            try:
                size = int(body[pos:nl].split(b";")[0], 16)
            except ValueError:
                raise TransportError("notrfkit", "bad chunked body")
            if size == 0:
                return status, bytes(out)
            if len(body) < nl + 2 + size + 2:
                break
            out += body[nl + 2:nl + 2 + size]
            pos = nl + 2 + size + 2
        if eof:
            raise TransportError("noanswer", "connection closed mid-answer")
        return None
    if "content-length" in headers:
        try:
            n = int(headers["content-length"])
        except ValueError:
            raise TransportError("notrfkit", "bad Content-Length")
        if n > rfkit.MAX_BODY_BYTES:
            raise TransportError("notrfkit", "response too large")
        if len(body) >= n:
            return status, bytes(body[:n])
        if eof:
            raise TransportError("noanswer", "connection closed mid-answer")
        return None
    return (status, bytes(body)) if eof else None


async def http_exchange(host, port, method, path, body=None, on_bytes=None,
                        silence_s=SILENCE_TIMEOUT_S):
    """One request on its own connection. The only timeout is silence_s with
    no bytes; it restarts on every byte received."""
    try:
        reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), silence_s)
    except asyncio.TimeoutError:
        raise TransportError("noanswer", f"no answer for {silence_s:.0f} s", silent_timeout=True)
    except OSError as e:
        raise _classify_oserror(e)
    try:
        lines = [f"{method} {path} HTTP/1.1", f"Host: {host}:{port}", "Accept: application/json",
                 "Connection: close", "User-Agent: rf2k-web"]
        if body is not None:
            lines += ["Content-Type: application/json", f"Content-Length: {len(body)}"]
        writer.write(("\r\n".join(lines) + "\r\n\r\n").encode() + (body or b""))
        try:
            await asyncio.wait_for(writer.drain(), silence_s)
        except asyncio.TimeoutError:
            raise TransportError("noanswer", f"no answer for {silence_s:.0f} s", silent_timeout=True)
        buf = bytearray()
        while True:
            try:
                chunk = await asyncio.wait_for(reader.read(8192), silence_s)
            except asyncio.TimeoutError:
                raise TransportError("noanswer", f"no answer for {silence_s:.0f} s", silent_timeout=True)
            if chunk:
                if on_bytes:
                    on_bytes()
                buf += chunk
                if len(buf) > rfkit.MAX_BODY_BYTES + 16 * 1024:
                    raise TransportError("notrfkit", "response too large")
            done = _parse_response(bytes(buf), eof=not chunk)
            if done is not None:
                return done
    except OSError as e:
        raise _classify_oserror(e)
    finally:
        writer.close()
        try:
            await asyncio.wait_for(writer.wait_closed(), 1.0)
        except (OSError, asyncio.TimeoutError):
            pass


# ── the amp ──────────────────────────────────────────────────────────────

class Request:
    __slots__ = ("method", "path", "body", "kind", "label", "future")

    def __init__(self, method, path, body=None, kind="poll", label="", future=None):
        self.method, self.path, self.body = method, path, body
        self.kind = kind          # poll | probe | wake | command
        self.label = label        # what a command was, for a refusal
        self.future = future      # a command's outcome, for whoever sent it


class Amp:
    """State of the one amp, the request queue, and the polling schedule.
    Every method runs on the event loop thread."""

    def __init__(self, host, port, keep_awake_s=60, grace_s=30, on_change=None):
        self.host, self.port = host, port
        self.keep_awake_s = keep_awake_s
        self.grace_s = grace_s
        self.on_change = on_change
        self.target = f"{host}:{port}"

        self._queue: deque[Request] = deque()
        self._busy = False             # a request is on the wire (any epoch)
        self._epoch = 0
        self._sent_at = 0.0
        self._last_byte = 0.0
        self._silence_handle = None
        self.silent_since = None       # monotonic; None while answering

        self.active = False            # a browser is open (or the grace runs)
        self.connected = False
        self.offline_reason = ""       # "" while still trying the first time
        self.error = ""
        self._failures = 0
        self._reconnect_step = 0
        self._reconnect_handle = None
        self._idle_handle = None

        self.browsers = 0
        self.controllers = 0           # browsers that may control (and so transmit)
        self._awake_wanted = False
        self._keep_awake_handle = None
        self._first_wake_handle = None
        self._wake_pending = False
        self.waking = False

        self._ticker = None
        self._publish_pending = False
        self._last_published = None
        self._clear_status()

    # ── status ──

    def _clear_status(self):
        self.info = self.power = self.data = self.tuner = None
        self.operate = None
        self.iface = None
        self.antennas = None
        self.tune_api = "unknown"
        self.tune_pending = False
        self.transmitting = False
        self._fast_until = 0.0
        self._tune_watch_until = 0.0
        self._data_next = False
        self._standby_asked = False
        self._last_antennas = 0.0
        self._last_iface = 0.0
        self._slot = 0
        self._paused_until = 0.0
        self._warned = set()

    def snapshot(self) -> dict:
        s = {"target": self.target, "polling": self.active, "connected": self.connected}
        if not self.active:
            return s
        if not self.connected:
            s["offlineReason"] = self.offline_reason
            s["offlineText"] = OFFLINE_TEXT.get(self.offline_reason, "")
            s["error"] = self.error
            return s
        if self.silent_since is not None:
            s["silentMs"] = int((now() - self.silent_since) * 1000)
        s["waking"] = self.waking
        s["tuneApi"] = self.tune_api
        s["tunePending"] = self.tune_pending
        s["keyed"] = self.transmitting
        if self.info:
            s["device"] = self.info["custom_name"] or self.info["device"]
            s["model"] = self.info["device"]
            s["firmware"] = {"gui": self.info["gui_version"], "controller": self.info["controller_version"]}
        if self.operate is not None:
            s["operate"] = self.operate == "OPERATE"
        if self.power:
            p = self.power
            fwd, ref = rfkit.watts(p["forward"]), rfkit.watts(p["reflected"])
            if fwd is not None:
                s["fwd"] = fwd
            if ref is not None:
                s["ref"] = ref
            if p["forward"].get("max"):
                s["fwdLimit"] = p["forward"]["max"]
            s["swr"] = round(p["swr"]["value"], 2)
            s["temp"] = round(rfkit.temperature_celsius(p["temperature"]), 1)
            s["volts"] = round(p["voltage"]["value"], 1)
            s["amps"] = round(p["current"]["value"], 1)
            s["tuneDrive"] = rfkit.tune_drive(p)
        fault = ""
        if self.data:
            s["band"] = rfkit.format_band(self.data["band"])
            s["freq"] = rfkit.format_frequency(self.data["frequency"])
            fault = self.data["status"].strip()
        if self.iface:
            s["iface"] = self.iface["interface"]
            if not fault:
                fault = self.iface["error"].strip()
        s["ampError"] = fault
        if self.tuner:
            t = self.tuner
            pill = rfkit.tuner_pill(t)
            s["tuner"] = {
                "mode": t["mode"], "setup": t["setup"], "pill": pill,
                "inUse": rfkit.tuner_pill_in_use(pill), "alert": rfkit.tuner_pill_alert(pill),
                "allowed": rfkit.tune_allowed(t),
            }
            for k, name in (("L", "L"), ("C", "C"), ("tuned_frequency", "freq"), ("segment_size", "segment")):
                if t[k]:
                    s["tuner"][name] = (rfkit.format_frequency(t[k]) if k == "tuned_frequency"
                                        else rfkit.format_value(t[k]))
        if self.antennas is not None:
            s["antennas"] = self.antennas
        return s

    def _publish(self):
        """Coalesced: at most one snapshot per event-loop turn, none if unchanged."""
        if self._publish_pending:
            return
        self._publish_pending = True
        asyncio.get_running_loop().call_soon(self._do_publish)

    def _do_publish(self):
        self._publish_pending = False
        snap = self.snapshot()
        text = json.dumps(snap, sort_keys=True)
        if text == self._last_published:
            return
        self._last_published = text
        if self.on_change:
            self.on_change(snap)

    # ── browsers ──

    def set_browsers(self, total: int, controllers: int):
        """Called when a browser connects or leaves. Polling runs only while
        one is open, plus a grace so a reload does not reconnect."""
        self.browsers, self.controllers = total, controllers
        loop = asyncio.get_running_loop()
        if self._ticker is None:
            self._ticker = loop.create_task(self._tick_loop())
        if total > 0:
            if self._idle_handle:
                self._idle_handle.cancel()
                self._idle_handle = None
            if not self.active:
                self.active = True
                log.info("a browser is open: polling the amp at %s", self.target)
                self._connect()
            self._update_awake()
        elif self.active and self._idle_handle is None:
            self._idle_handle = loop.call_later(self.grace_s, self._go_idle)
            self._update_awake()

    def _go_idle(self):
        self._idle_handle = None
        if self.browsers > 0:
            return
        log.info("no browser for %d s: polling stopped (nothing is sent to the amp until one opens)",
                 self.grace_s)
        self.active = False
        self.connected = False
        self._cancel(("_reconnect_handle",))
        self._reset_session()
        self._publish()

    # ── session ──

    def _cancel(self, names):
        for n in names:
            h = getattr(self, n)
            if h:
                h.cancel()
                setattr(self, n, None)

    def _reset_session(self):
        """Forget the session. The request in flight (if any) is NOT
        cancelled: it runs to its end and its result is discarded."""
        self._epoch += 1
        while self._queue:
            self._resolve(self._queue.popleft(), False, "Amplifier offline")
        self._cancel(("_keep_awake_handle", "_first_wake_handle", "_silence_handle"))
        self._awake_wanted = False
        self._wake_pending = False
        self.waking = False
        self.silent_since = None
        self._failures = 0
        self._clear_status()

    def _connect(self):
        self._reconnect_handle = None
        if not self.active:
            return
        self._reset_session()
        log.debug("connecting to %s", self.target)
        self._enqueue(Request("GET", rfkit.PATH_INFO, kind="probe"))

    def _go_offline(self, err: TransportError):
        was = self.connected
        self.connected = False
        self._reset_session()
        if self.offline_reason != err.kind or was:
            log.info("amp not reachable at %s: %s (%s)", self.target,
                     OFFLINE_TEXT.get(err.kind, err.kind), err)
        self.offline_reason, self.error = err.kind, str(err)
        self._publish()
        self._arm_reconnect()

    def _arm_reconnect(self):
        if not self.active or self._reconnect_handle:
            return
        delay = RECONNECT_STEPS_S[min(self._reconnect_step, len(RECONNECT_STEPS_S) - 1)]
        if self._reconnect_step:
            log.info("amp still not answering, next try in %d s", delay)
        self._reconnect_step += 1
        self._reconnect_handle = asyncio.get_running_loop().call_later(delay, self._connect)

    def _on_connected(self):
        self.connected = True
        self._failures = 0
        self._reconnect_step = 0      # back to 5 s once it answers as an RF-KIT
        self.offline_reason = self.error = ""
        log.info("connected to %s (%s)", self.target,
                 (self.info["custom_name"] or self.info["device"]) if self.info else "?")
        # Read everything once so the page fills in at once.
        for p in (rfkit.PATH_POWER, rfkit.PATH_DATA, rfkit.PATH_OPERATE_MODE,
                  rfkit.PATH_ANTENNAS, rfkit.PATH_TUNER, rfkit.PATH_INTERFACE):
            self._enqueue(Request("GET", p))
        # Let a just-woken amp answer a few ordinary polls before the slow wake.
        epoch = self._epoch
        self._first_wake_handle = asyncio.get_running_loop().call_later(
            FIRST_WAKE_DELAY_S, lambda: epoch == self._epoch and self._update_awake(just_connected=True))
        self._publish()

    # ── wake / keep-awake ──

    def _update_awake(self, just_connected=False):
        """The empty PUT /tuner {} wakes the amp and restarts its sleep timer.
        Sent while someone who may transmit has a browser open."""
        want = self.connected and self.controllers > 0
        if want and (not self._awake_wanted or just_connected):
            if just_connected or not self._awake_wanted:
                log.info("waking the amp%s", f" (kept awake every {self.keep_awake_s} s while a browser is open)"
                         if self.keep_awake_s else "")
            self._awake_wanted = True
            self._wake(first=True)
            self._schedule_keep_awake()
        elif not want and self._awake_wanted:
            log.info("no controlling browser: keep-awake stopped (the amp's sleep timer runs)")
            self._cancel(("_keep_awake_handle",))
            self._awake_wanted = False

    def _schedule_keep_awake(self):
        self._cancel(("_keep_awake_handle",))
        if self.keep_awake_s > 0:
            self._keep_awake_handle = asyncio.get_running_loop().call_later(self.keep_awake_s, self._keep_awake)

    def _keep_awake(self):
        self._keep_awake_handle = None
        if self._awake_wanted and self.connected:
            self._wake(first=False)
            self._schedule_keep_awake()

    def _wake(self, first):
        if not self.connected or self._wake_pending:
            return
        self._wake_pending = True
        if first:
            self.waking = True
            self._publish()
        self._enqueue(Request("PUT", rfkit.PATH_TUNER, rfkit.TUNER_WAKE_BODY, kind="wake"), front=True)

    # ── queue ──

    def _enqueue(self, req: Request, front=False):
        if front:
            # Ahead of queued polls, behind earlier commands: two clicks reach
            # the amp in the order they were made.
            i = 0
            while i < len(self._queue) and self._queue[i].kind in ("command", "wake"):
                i += 1
            self._queue.insert(i, req)
        else:
            self._queue.append(req)
        self._pump()

    def _pump(self):
        if self._busy or not self._queue:
            return
        req = self._queue.popleft()
        self._busy = True
        asyncio.get_running_loop().create_task(self._run(req, self._epoch))

    def _on_bytes(self):
        self._last_byte = now()
        if self._silence_handle:
            self._silence_handle.cancel()
        self._silence_handle = asyncio.get_running_loop().call_later(NO_ANSWER_SHOW_S, self._went_silent)
        if self.silent_since is not None:
            log.info("silence ended after %.1f s", now() - self.silent_since)
            self.silent_since = None
            self._publish()

    def _went_silent(self):
        self._silence_handle = None
        if self._busy and self.connected:
            self.silent_since = self._last_byte
            log.info("no answer for %.0f s - shown on the page", now() - self._last_byte)
            self._publish()

    def _end_silence(self):
        self._cancel(("_silence_handle",))
        if self.silent_since is not None:
            self.silent_since = None
            self._publish()

    async def _run(self, req: Request, epoch: int):
        self._sent_at = self._last_byte = now()
        self._silence_handle = asyncio.get_running_loop().call_later(NO_ANSWER_SHOW_S, self._went_silent)
        err = status = body = None

        def on_bytes():
            if epoch == self._epoch:
                self._on_bytes()
        try:
            status, body = await http_exchange(self.host, self.port, req.method, req.path, req.body,
                                               on_bytes=on_bytes, silence_s=SILENCE_TIMEOUT_S)
        except TransportError as e:
            err = e
        except Exception as e:  # a bug must not wedge the queue
            log.exception("request %s %s failed", req.method, req.path)
            err = TransportError("noanswer", str(e))
        finally:
            self._busy = False
        if epoch != self._epoch:
            self._resolve(req, False, "Amplifier offline")
        else:
            self._end_silence()
            if err:
                self._on_failure(req, err)
            else:
                self._on_reply(req, status, body)
        self._pump()

    def _resolve(self, req, ok, reason=""):
        if req.future and not req.future.done():
            req.future.set_result({"ok": ok, "reason": reason, "what": req.label})

    # ── replies ──

    def _on_failure(self, req: Request, err: TransportError):
        log.warning("%s %s: %s", req.method, req.path, err)
        if req.kind == "wake":
            self._wake_pending = False
            self.waking = False
            if self.connected:
                # A wake that got no answer does not count toward dropping the
                # link; the polls decide that.
                self._paused_until = now() + 5
                self._publish()
                return
        if req.kind == "command":
            self._resolve(req, False, "No answer from the amplifier" if err.kind == "noanswer" else str(err))
            if req.path == rfkit.PATH_TUNER:
                self.tune_pending = False
        if req.kind == "probe" or not self.connected:
            self._go_offline(err)
            return
        # A request with no bytes at all for the full 15 s: the amp is off or
        # unplugged, not a hiccup. The page has said NO ANSWER since 2 s in.
        if err.silent_timeout:
            self._go_offline(err)
            return
        self._failures += 1
        if self._failures >= MAX_CONSECUTIVE_FAILURES:
            self._go_offline(err)
            return
        self._paused_until = now() + 5   # give a struggling amp room

    def _on_reply(self, req: Request, status: int, body: bytes):
        self._failures = 0
        took = now() - self._sent_at
        if req.kind != "wake" and took > 1.0:
            # The amp answered, but slowly: give it as long again.
            self._paused_until = now() + min(took, SILENCE_TIMEOUT_S)
            log.info("slow answer on %s (%.0f ms) - polling paused as long", req.path, took * 1000)

        if req.kind == "wake":
            self._wake_pending = False
            self.waking = False
            api = rfkit.classify_tune_probe(status)
            if api != self.tune_api:
                log.info("tune via REST: %s (HTTP %d)", api, status)
                self.tune_api = api
            self._publish()
            return

        if not 200 <= status < 300:
            err = rfkit.parse_api_error(body)
            reason = err["reason"] if err and err["reason"] else f"HTTP {status}"
            log.warning("%s %s refused: %d %s", req.method, req.path, status, reason)
            if req.kind == "command":
                self._resolve(req, False, reason)
                if req.path == rfkit.PATH_TUNER:
                    self.tune_pending = False
                    self._publish()
            if req.kind == "probe":
                self._go_offline(TransportError("notrfkit", f"not an RF-KIT amplifier (HTTP {status})"))
            return

        if req.kind == "probe":
            info = rfkit.parse_info(body)
            if info is None:
                self._go_offline(TransportError("notrfkit", "not an RF-KIT amplifier (unexpected /info reply)"))
                return
            self.info = info
            self._on_connected()
            return

        if req.kind == "command":
            self._resolve(req, True)
            # Never assume the new state: read it back from the amp.
            follow = {
                rfkit.PATH_OPERATE_MODE: (rfkit.PATH_OPERATE_MODE,),
                rfkit.PATH_ANTENNA_ACTIVE: (rfkit.PATH_ANTENNAS,),
                rfkit.PATH_ERROR_RESET: (rfkit.PATH_DATA, rfkit.PATH_INTERFACE, rfkit.PATH_OPERATE_MODE),
                rfkit.PATH_TUNER: (rfkit.PATH_TUNER, rfkit.PATH_DATA),
            }.get(req.path, ())
            if req.path == rfkit.PATH_TUNER:
                self.tune_pending = False
                self._tune_watch_until = now() + TUNE_WATCH_S
                self._publish()
            for p in follow:
                self._enqueue(Request("GET", p))
            return

        if self._apply(req.path, body):
            self._publish()

    def _apply(self, path: str, body: bytes) -> bool:
        parser = {
            rfkit.PATH_POWER: rfkit.parse_power, rfkit.PATH_DATA: rfkit.parse_data,
            rfkit.PATH_TUNER: rfkit.parse_tuner, rfkit.PATH_OPERATE_MODE: rfkit.parse_operate_mode,
            rfkit.PATH_INTERFACE: rfkit.parse_interface, rfkit.PATH_ANTENNAS: rfkit.parse_antennas,
        }.get(path)
        if parser is None:
            return False
        if path == rfkit.PATH_ANTENNAS:
            self._last_antennas = now()
        elif path == rfkit.PATH_INTERFACE:
            self._last_iface = now()
        v = parser(body)
        if v is None:
            # Not the expected shape: ignore it, keep what we had.
            if path not in self._warned:
                self._warned.add(path)
                log.warning("unparseable reply from %s: %r", path, body[:200])
            return False

        if path == rfkit.PATH_POWER:
            fwd = rfkit.watts(v["forward"]) or 0
            prev = rfkit.watts(self.power["forward"]) if self.power else None
            # FWD collapsing mid-over is how a trip looks from /power.
            if self.transmitting and prev is not None and prev >= TRIP_FROM_W and fwd < 1:
                self._data_next = True
            keyed = fwd >= 1
            if keyed != self.transmitting:
                self.transmitting = keyed
                if not keyed:
                    self._fast_until = now() + TX_HOLD_S
                self._slot = 0
            self.power = v
        elif path == rfkit.PATH_DATA:
            self.data = v
        elif path == rfkit.PATH_TUNER:
            self.tuner = v
        elif path == rfkit.PATH_OPERATE_MODE:
            # OPERATE dropping to STANDBY that nobody asked for (a fault, the
            # sleep timer): read the fault text next.
            if self.operate == "OPERATE" and v == "STANDBY" and not self._standby_asked:
                self._data_next = True
            if v == "STANDBY":
                self._standby_asked = False
            self.operate = v
        elif path == rfkit.PATH_INTERFACE:
            self.iface = v
        elif path == rfkit.PATH_ANTENNAS:
            self.antennas = v
        return True

    # ── polling ──

    def fast(self) -> bool:
        return self.transmitting or now() < self._fast_until

    async def _tick_loop(self):
        while True:
            fast = self.fast()
            await asyncio.sleep(TX_TICK_S if fast else RX_TICK_S)
            if self.fast() != fast:
                self._slot = 0
            try:
                self._poll()
            except Exception:
                log.exception("poll")

    def _poll(self):
        if not self.connected or self._busy or self._queue:
            return
        t = now()
        if t < self._paused_until:
            return
        if self._data_next:
            self._data_next = False
            self._enqueue(Request("GET", rfkit.PATH_DATA))
            return
        if self.fast():
            i = self._slot % len(TX_SCHEDULE)
            path = TX_SCHEDULE[i]
            tuning = self.tuner is not None and self.tuner["mode"] == "TUNING"
            if i == TX_TUNER_SLOT and (tuning or t < self._tune_watch_until):
                path = rfkit.PATH_TUNER
            self._slot = (i + 1) % len(TX_SCHEDULE)
        else:
            i = self._slot % len(RX_SCHEDULE)
            path = RX_SCHEDULE[i]
            self._slot = (i + 1) % len(RX_SCHEDULE)
            if path is None:
                if t - self._last_antennas >= SLOW_PATH_S:
                    path = rfkit.PATH_ANTENNAS
                elif t - self._last_iface >= SLOW_PATH_S:
                    path = rfkit.PATH_INTERFACE
                else:
                    path = rfkit.PATH_TUNER
        self._enqueue(Request("GET", path))

    # ── commands ──

    async def command(self, cmd: str, value=None) -> dict:
        """OPERATE / STANDBY, antenna, error reset, tune. The answer is the
        amp's own (ok, or its refusal reason); the page repaints only from the
        readback that follows."""
        if not self.connected:
            return {"ok": False, "reason": "Amplifier offline", "what": cmd}
        if cmd == "operate":
            if not isinstance(value, bool):
                return {"ok": False, "reason": "bad value", "what": "OPERATE"}
            req = Request("PUT", rfkit.PATH_OPERATE_MODE, rfkit.operate_body(value),
                          kind="command", label="OPERATE" if value else "STANDBY")
            self._standby_asked = not value
        elif cmd == "antenna":
            body = rfkit.antenna_body(value)
            if body is None:
                return {"ok": False, "reason": f"{value} is not a selectable antenna", "what": "ANTENNA"}
            req = Request("PUT", rfkit.PATH_ANTENNA_ACTIVE, body, kind="command", label="ANTENNA")
        elif cmd == "reset":
            req = Request("POST", rfkit.PATH_ERROR_RESET, b"", kind="command", label="RESET")
        elif cmd == "tune":
            if self.tune_api != "supported":
                return {"ok": False, "reason": "this amplifier's server has no tune command", "what": "TUNE"}
            if not rfkit.tune_allowed(self.tuner):
                return {"ok": False, "reason": "the tuner is not idle in AUTO or MANUAL", "what": "TUNE"}
            if self.tune_pending:
                return {"ok": False, "reason": "a tune is already being started", "what": "TUNE"}
            self.tune_pending = True
            self._publish()
            req = Request("PUT", rfkit.PATH_TUNER, rfkit.TUNER_START_BODY, kind="command", label="TUNE")
        else:
            return {"ok": False, "reason": f"unknown command {cmd!r}", "what": cmd}
        req.future = asyncio.get_running_loop().create_future()
        log.info("command %s", req.label)
        self._enqueue(req, front=True)
        result = await req.future
        if not result["ok"]:
            log.info("amp refused %s: %s", req.label, result["reason"])
        return result
