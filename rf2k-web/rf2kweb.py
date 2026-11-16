#!/usr/bin/env python3
"""rf2k-web: a web interface for the RF-KIT RF2K-S amplifier.

The only program that talks to the amp. It polls the amp's REST API (one
request at a time, only while a browser is open) and pushes the state to every
browser on the LAN.

    python3 rf2kweb.py --amp 10.0.0.25:8080
    open http://<this machine>:8088/

Do not let anything else poll the amp at the same time: run FlexWeb without
--rfkit while this is running.
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import os
import signal
import sys

from ampclient import Amp
from webapp import WebApp
import rfkit

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
    ap.add_argument("--password-file", help="file holding the control password "
                    "(else $RF2K_WEB_PASSWORD); with one, browsers are view-only until signed in")
    ap.add_argument("--read-only", action="store_true", help="nobody can control the amp from the page")
    ap.add_argument("--keep-awake", type=int, default=60, metavar="SECS",
                    help="send the wake request this often while a browser is open (0 = only once)")
    ap.add_argument("--grace", type=int, default=30, metavar="SECS",
                    help="stop polling this long after the last browser closes")
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

    async def run():
        amp = Amp(amp_host, amp_port, keep_awake_s=max(0, args.keep_awake), grace_s=max(0, args.grace))
        app = WebApp(amp, password=password, read_only=args.read_only)
        server = await asyncio.start_server(app.handle, listen_host, listen_port)
        amp.set_browsers(0, 0)
        log.info("amp %s:%d - nothing is sent to it until a browser opens", amp_host, amp_port)
        log.info("listening on http://%s:%d/", listen_host, listen_port)
        if args.read_only:
            log.info("read-only: the page cannot control the amp")
        elif password:
            log.info("control needs the password; without it browsers are view-only")
        else:
            log.warning("no password: every browser that can reach this port can control the amp")
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
