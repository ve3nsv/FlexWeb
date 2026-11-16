#!/usr/bin/env python3
"""rf2k-web: a web interface for the RF-KIT RF2K-S amplifier.

The only program that talks to the amp. It polls the amp's REST API (one
request at a time, only while a browser is open) and pushes the state to every
browser on the LAN.

    python3 rf2kweb.py --amp 10.0.0.25:8080
    open https://<this machine>:8088/

Do not let anything else poll the amp at the same time: run FlexWeb without
--rfkit while this is running.
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import os
import signal
import socket
import sys

from ampclient import Amp
from webapp import WebApp
import rfkit
import tlscert

log = logging.getLogger("rf2k")


def host_port(text, default_port):
    text = text.strip()
    if text.startswith("["):              # [v6]:port
        host, _, rest = text[1:].partition("]")
        port = rest.lstrip(":") or default_port
    elif text.count(":") == 1:
        host, port = text.split(":")
    else:
        host, port = text, default_port
    port = int(port)
    if not host or not 0 < port < 65536:
        raise argparse.ArgumentTypeError(f"bad address {text!r}")
    return host, port


def main(argv=None):
    ap = argparse.ArgumentParser(description="Web interface for the RF-KIT RF2K-S amplifier")
    ap.add_argument("--amp", default=os.environ.get("RF2K_AMP", "10.0.0.25:8080"),
                    help="the amplifier's REST server, host[:port] (default 10.0.0.25:8080)")
    ap.add_argument("--listen", default=os.environ.get("RF2K_LISTEN", "0.0.0.0:8088"),
                    help="where browsers connect, host[:port] (default 0.0.0.0:8088)")
    ap.add_argument("--password-file", help="file holding the password "
                    "(else $RF2K_WEB_PASSWORD); with one, the page shows nothing until signed in")
    ap.add_argument("--read-only", action="store_true", help="nobody can control the amp from the page")
    ap.add_argument("--keep-awake", type=int, default=60, metavar="SECS",
                    help="send the wake request this often while a browser is open (0 = only once)")
    ap.add_argument("--grace", type=int, default=30, metavar="SECS",
                    help="stop polling this long after the last browser closes")
    ap.add_argument("--http", action="store_true",
                    help="serve plain HTTP instead of HTTPS (the password then crosses the network unencrypted)")
    ap.add_argument("--cert", help="HTTPS certificate (PEM); default: a self-signed one made on first run")
    ap.add_argument("--key", help="its private key (PEM)")
    ap.add_argument("--cert-dir", help="where the self-signed certificate is kept (default ~/.config/rf2k-web)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname).1s %(name)s: %(message)s")

    amp_host, amp_port = host_port(args.amp, rfkit.DEFAULT_PORT)
    listen_host, listen_port = host_port(args.listen, 8088)
    password = os.environ.get("RF2K_WEB_PASSWORD") or None
    if args.password_file:
        with open(args.password_file, encoding="utf-8") as f:
            password = f.read().strip() or None

    ssl_ctx = cert_path = None
    if not args.http:
        try:
            ssl_ctx, cert_path = tlscert.ensure(args.cert, args.key, args.cert_dir)
        except (RuntimeError, OSError) as e:
            log.error("%s", e)
            return 2

    async def run():
        amp = Amp(amp_host, amp_port, keep_awake_s=max(0, args.keep_awake), grace_s=max(0, args.grace))
        app = WebApp(amp, password=password, read_only=args.read_only, secure=ssl_ctx is not None)
        if ssl_ctx:
            tlscert.quiet_handshake_errors(asyncio.get_running_loop())
        server = await asyncio.start_server(app.handle, listen_host, listen_port, ssl=ssl_ctx)
        amp.set_browsers(0, 0)
        log.info("amp %s:%d - nothing is sent to it until a browser opens", amp_host, amp_port)
        scheme = "https" if ssl_ctx else "http"
        shown = listen_host if listen_host not in ("0.0.0.0", "::", "") else socket.gethostname()
        log.info("listening on %s://%s:%d/", scheme, shown, listen_port)
        if ssl_ctx:
            log.info("certificate %s, SHA-256 fingerprint %s", cert_path, tlscert.fingerprint(cert_path))
            if not args.cert:
                log.info("self-signed: each browser warns once - check the fingerprint, then accept")
        elif password:
            log.warning("plain HTTP: the password crosses the network unencrypted")
        if password:
            log.info("password set: the page shows nothing until the browser signs in")
        else:
            log.warning("no password: every browser that can reach this port can see%s the amp",
                        "" if args.read_only else " and control")
        if args.read_only:
            log.info("read-only: the page cannot control the amp")
        stop = asyncio.Event()
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, stop.set)
            except NotImplementedError:   # Windows
                pass
        async with server:
            await stop.wait()
        log.info("stopping")

    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
