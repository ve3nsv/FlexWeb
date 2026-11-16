"""The browser side: static page, Server-Sent Events, commands, sign-in.

Plain asyncio, no dependencies. Every browser gets the amp's state pushed over
SSE (GET /api/events); commands are POSTed to /api/command and answered with
the amp's own result, so a refusal (the amp's 400/409 reason) goes back to
whoever sent it. An open event stream is what "a browser is open" means to the
poller: no stream, no traffic to the amp.

Access: with a password configured, nothing about the amp is shown until the
browser signs in (the static page itself only holds the sign-in screen); a
signed-in browser can watch and control. Without one, every browser on the LAN
can watch and control. --read-only removes control in either case.
"""

from __future__ import annotations

import asyncio
import contextvars
import hmac
import json
import logging
import mimetypes
import secrets
import time
from pathlib import Path
from urllib.parse import urlsplit

log = logging.getLogger("rf2k.web")

STATIC_DIR = Path(__file__).resolve().parent / "static"
STATIC_FILES = {"/": "index.html", "/index.html": "index.html", "/app.js": "app.js",
                "/style.css": "style.css", "/favicon.svg": "favicon.svg"}
MAX_HEADER_BYTES = 16 * 1024
MAX_BODY_BYTES = 4 * 1024
SSE_KEEPALIVE_S = 15
SESSION_MAX_AGE_S = 30 * 24 * 3600
COOKIE = "rf2k_session"
KEEP_ALIVE_S = 60             # an idle browser connection is closed after this

# Whether the response being written ends its connection (per connection task).
_closing = contextvars.ContextVar("closing", default=True)

STATUS_TEXT = {200: "OK", 204: "No Content", 400: "Bad Request", 401: "Unauthorized",
               403: "Forbidden", 404: "Not Found", 405: "Method Not Allowed",
               413: "Payload Too Large", 415: "Unsupported Media Type", 500: "Internal Server Error"}


class HttpError(Exception):
    def __init__(self, status, message=""):
        super().__init__(message)
        self.status = status


class SseClient:
    def __init__(self, can_control, token):
        self.can_control = can_control
        self.token = token
        self.revoked = False          # signed out: the stream ends
        self.event = asyncio.Event()


class WebApp:
    def __init__(self, amp, password=None, read_only=False, secure=False):
        self.amp = amp
        self.secure = secure          # served over HTTPS: the cookie is Secure
        self.password = password or None
        self.read_only = read_only
        self.sessions = {}             # token -> expiry (wall clock)
        self.clients: set[SseClient] = set()
        self.state = amp.snapshot()
        self._login_lock = asyncio.Lock()
        amp.on_change = self._on_amp_change

    # ── who may see and control ──

    def _token_ok(self, token) -> bool:
        if not token:
            return False
        exp = self.sessions.get(token)
        if exp is None or exp < time.time():
            self.sessions.pop(token, None)
            return False
        return True

    def authorized(self, headers) -> bool:
        """May see the amp at all."""
        return self.password is None or self._token_ok(_cookie(headers.get("cookie", ""), COOKIE))

    def can_control(self, headers) -> bool:
        return not self.read_only and self.authorized(headers)

    def _require_auth(self, headers):
        if not self.authorized(headers):
            raise HttpError(401, "Sign in")

    def _session_info(self, headers) -> dict:
        return {"canControl": self.can_control(headers),
                "loginRequired": self.password is not None,
                "signedIn": self.password is not None and self.authorized(headers),
                "readOnly": self.read_only}

    # ── amp state fan-out ──

    def _on_amp_change(self, snap):
        self.state = snap
        for c in self.clients:
            c.event.set()

    def _browsers_changed(self):
        self.amp.set_browsers(len(self.clients), sum(1 for c in self.clients if c.can_control))

    # ── HTTP ──

    async def handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        """One connection, kept open between requests (HTTP/1.1 keep-alive) so
        a button press does not pay a new TLS handshake."""
        try:
            first = True
            while True:
                try:
                    method, target, version, headers, body = await asyncio.wait_for(
                        _read_request(reader), 30 if first else KEEP_ALIVE_S)
                except (asyncio.TimeoutError, asyncio.IncompleteReadError, asyncio.LimitOverrunError,
                        ConnectionError):
                    return
                except HttpError as e:
                    _closing.set(True)     # the rest of the request is unread
                    await _send(writer, e.status, {"error": str(e) or STATUS_TEXT.get(e.status, "")})
                    return
                first = False
                conn = headers.get("connection", "").lower()
                _closing.set(version != "HTTP/1.1" or "close" in conn)
                path = urlsplit(target).path
                try:
                    await self._route(method, path, headers, body, reader, writer)
                except HttpError as e:
                    await _send(writer, e.status, {"error": str(e) or STATUS_TEXT.get(e.status, "")})
                if _closing.get() or path == "/api/events" or writer.is_closing():
                    return
        except (ConnectionError, asyncio.CancelledError):
            pass
        except Exception:
            log.exception("request failed")
        finally:
            writer.close()

    async def _route(self, method, path, headers, body, reader, writer):
        if path == "/api/events":
            _require(method, "GET")
            self._require_auth(headers)
            await self._events(headers, reader, writer)
        elif path == "/api/state":
            _require(method, "GET")
            self._require_auth(headers)
            await _send(writer, 200, dict(self.state, session=self._session_info(headers)))
        elif path == "/api/command":
            _require(method, "POST")
            data = _json_body(headers, body)
            self._require_auth(headers)
            if not self.can_control(headers):
                raise HttpError(403, "View only")
            cmd = data.get("cmd")
            if not isinstance(cmd, str):
                raise HttpError(400, "cmd missing")
            result = await self.amp.command(cmd, data.get("value"))
            await _send(writer, 200, result)
        elif path == "/api/login":
            _require(method, "POST")
            data = _json_body(headers, body)
            await self._login(data, writer)
        elif path == "/api/logout":
            _require(method, "POST")
            _json_body(headers, body)
            token = _cookie(headers.get("cookie", ""), COOKIE)
            self.sessions.pop(token, None)
            for c in self.clients:          # that browser's open streams end now
                if token and c.token == token:
                    c.revoked = True
                    c.event.set()
            await _send(writer, 200, {"ok": True},
                        extra=[f"Set-Cookie: {COOKIE}=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict{self._secure_attr()}"])
        elif path in STATIC_FILES:
            _require(method, "GET", "HEAD")
            f = STATIC_DIR / STATIC_FILES[path]
            try:
                data = f.read_bytes()
            except OSError:
                raise HttpError(404)
            ctype = mimetypes.guess_type(f.name)[0] or "application/octet-stream"
            if ctype.startswith("text/") or ctype.endswith("javascript"):
                ctype += "; charset=utf-8"
            await _send_raw(writer, 200, ctype, b"" if method == "HEAD" else data,
                            extra=["Cache-Control: no-cache"])
        else:
            raise HttpError(404)

    async def _login(self, data, writer):
        if self.password is None:
            await _send(writer, 200, {"ok": True})
            return
        pw = data.get("password")
        async with self._login_lock:   # one guess at a time, slowly
            if not isinstance(pw, str) or not hmac.compare_digest(pw.encode(), self.password.encode()):
                await asyncio.sleep(1.0)
                log.warning("failed sign-in")
                raise HttpError(401, "Wrong password")
        token = secrets.token_urlsafe(32)
        now = time.time()
        self.sessions = {t: e for t, e in self.sessions.items() if e > now}
        self.sessions[token] = now + SESSION_MAX_AGE_S
        log.info("browser signed in")
        await _send(writer, 200, {"ok": True}, extra=[
            f"Set-Cookie: {COOKIE}={token}; Path=/; Max-Age={SESSION_MAX_AGE_S}; HttpOnly; "
            f"SameSite=Strict{self._secure_attr()}"])

    def _secure_attr(self):
        return "; Secure" if self.secure else ""

    async def _events(self, headers, reader, writer):
        client = SseClient(self.can_control(headers), _cookie(headers.get("cookie", ""), COOKIE))
        session = self._session_info(headers)
        writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
                     b"Connection: keep-alive\r\nX-Accel-Buffering: no\r\n\r\n")
        writer.write(_sse("session", session))
        self.clients.add(client)
        log.info("browser connected (%d open)", len(self.clients))
        self._browsers_changed()
        client.event.set()
        # The browser sends nothing more on this connection: EOF is it leaving.
        gone = asyncio.ensure_future(reader.read(1))
        try:
            while not gone.done() and not client.revoked:
                wait = asyncio.ensure_future(client.event.wait())
                done, _ = await asyncio.wait({wait, gone}, timeout=SSE_KEEPALIVE_S,
                                             return_when=asyncio.FIRST_COMPLETED)
                if gone in done:
                    wait.cancel()
                    break
                if wait in done:
                    client.event.clear()
                    if client.revoked or (self.password and not self._token_ok(client.token)):
                        break
                    writer.write(_sse("state", self.state))
                else:
                    wait.cancel()
                    writer.write(b": keepalive\n\n")
                await writer.drain()
        except (ConnectionError, asyncio.CancelledError):
            pass
        finally:
            gone.cancel()
            self.clients.discard(client)
            log.info("browser left (%d open)", len(self.clients))
            self._browsers_changed()


def _sse(event, data) -> bytes:
    return f"event: {event}\ndata: {json.dumps(data, separators=(',', ':'))}\n\n".encode()


def _require(method, *allowed):
    if method not in allowed:
        raise HttpError(405)


def _json_body(headers, body) -> dict:
    # A JSON content type cannot be sent cross-site without a CORS preflight,
    # which this server never answers: together with the Origin check and the
    # SameSite cookie, other web pages cannot drive the amp through a browser.
    if not headers.get("content-type", "").split(";")[0].strip().lower() == "application/json":
        raise HttpError(415, "Content-Type must be application/json")
    origin = headers.get("origin")
    if origin and urlsplit(origin).netloc.lower() != headers.get("host", "").lower():
        raise HttpError(403, "cross-origin request refused")
    try:
        data = json.loads(body or b"{}")
    except ValueError:
        raise HttpError(400, "bad JSON")
    if not isinstance(data, dict):
        raise HttpError(400, "bad JSON")
    return data


def _cookie(header, name):
    for part in header.split(";"):
        k, _, v = part.strip().partition("=")
        if k == name:
            return v
    return ""


async def _read_request(reader):
    head = await reader.readuntil(b"\r\n\r\n")
    if len(head) > MAX_HEADER_BYTES:
        raise HttpError(413)
    lines = head.decode("iso-8859-1").split("\r\n")
    parts = lines[0].split(" ")
    if len(parts) != 3:
        raise HttpError(400)
    method, target, version = parts
    headers = {}
    for line in lines[1:]:
        if line:
            k, _, v = line.partition(":")
            headers[k.strip().lower()] = v.strip()
    body = b""
    n = headers.get("content-length")
    if n:
        if not n.isdigit() or int(n) > MAX_BODY_BYTES:
            raise HttpError(413)
        body = await reader.readexactly(int(n))
    return method, target, version, headers, body


async def _send(writer, status, obj, extra=()):
    await _send_raw(writer, status, "application/json",
                    json.dumps(obj).encode(), extra=["Cache-Control: no-store", *extra])


async def _send_raw(writer, status, ctype, data, extra=()):
    head = [f"HTTP/1.1 {status} {STATUS_TEXT.get(status, '')}", f"Content-Type: {ctype}",
            f"Content-Length: {len(data)}",
            "Connection: close" if _closing.get() else f"Keep-Alive: timeout={KEEP_ALIVE_S}",
            "X-Content-Type-Options: nosniff",
            "Referrer-Policy: no-referrer", *extra]
    writer.write(("\r\n".join(head) + "\r\n\r\n").encode() + data)
    await writer.drain()
