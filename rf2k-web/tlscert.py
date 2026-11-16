"""HTTPS for the browser side: a self-signed certificate made on first run.

Python's standard library can serve TLS but cannot make a certificate, so the
`openssl` command does that once (it is on every Linux and macOS, and on a
Raspberry Pi). Bring your own instead with --cert / --key, e.g. one from a
local CA so browsers do not warn at all.

The certificate is self-signed, so each browser warns once; check the SHA-256
fingerprint the server prints at startup against the one the browser shows,
then accept it. Only the browser <-> rf2k-web link is encrypted; the amp's own
REST API is plain HTTP and stays so.
"""

from __future__ import annotations

import hashlib
import ipaddress
import logging
import os
import shutil
import socket
import ssl
import subprocess
from pathlib import Path

log = logging.getLogger("rf2k.tls")

DAYS = 3650


def default_dir() -> Path:
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return Path(base) / "rf2k-web"


def _local_names():
    """Names and addresses browsers may use to reach this machine."""
    dns, ips = {"localhost"}, {"127.0.0.1", "::1"}
    host = socket.gethostname()
    if host:
        dns.add(host)
        if "." not in host:
            dns.add(host + ".local")      # mDNS, e.g. raspberrypi.local
        try:
            for ip in socket.gethostbyname_ex(host)[2]:
                ips.add(ip)
        except OSError:
            pass
    try:
        # The address of the interface that routes to the LAN (no packet is sent).
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("10.255.255.255", 1))
            ips.add(s.getsockname()[0])
    except OSError:
        pass
    valid = set()
    for ip in ips:
        try:
            valid.add(str(ipaddress.ip_address(ip)))
        except ValueError:
            pass
    return sorted(dns), sorted(valid)


def generate(cert: Path, key: Path):
    openssl = shutil.which("openssl")
    if not openssl:
        raise RuntimeError("HTTPS needs a certificate and the 'openssl' command was not found to make one. "
                           "Install OpenSSL, pass --cert and --key, or run with --http.")
    cert.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.chmod(cert.parent, 0o700)
    except OSError:
        pass
    dns, ips = _local_names()
    san = ",".join([f"DNS:{d}" for d in dns] + [f"IP:{i}" for i in ips])
    tmp_key = key.with_suffix(".tmp")
    old = os.umask(0o077)                 # the private key is readable by its owner only
    try:
        r = subprocess.run([openssl, "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
                            "-nodes", "-days", str(DAYS), "-subj", "/CN=rf2k-web",
                            "-addext", f"subjectAltName={san}",
                            "-addext", "basicConstraints=critical,CA:FALSE",
                            "-addext", "extendedKeyUsage=serverAuth",
                            "-keyout", str(tmp_key), "-out", str(cert)],
                           capture_output=True, text=True, timeout=60)
    finally:
        os.umask(old)
    if r.returncode != 0:
        tmp_key.unlink(missing_ok=True)
        raise RuntimeError("openssl could not make the certificate: " + (r.stderr.strip() or r.stdout.strip()))
    os.replace(tmp_key, key)
    log.info("made a self-signed certificate %s for %s", cert, ", ".join(dns + ips))


def fingerprint(cert: Path) -> str:
    der = ssl.PEM_cert_to_DER_cert(cert.read_text())
    h = hashlib.sha256(der).hexdigest().upper()
    return ":".join(h[i:i + 2] for i in range(0, len(h), 2))


def server_context(cert: Path, key: Path) -> ssl.SSLContext:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(str(cert), str(key))
    return ctx


def ensure(cert: str | None, key: str | None, cert_dir: str | None):
    """(context, cert path): the given pair, else the one in cert_dir, made
    there on first run."""
    if bool(cert) != bool(key):
        raise RuntimeError("give both --cert and --key, or neither")
    if cert:
        c, k = Path(cert), Path(key)
    else:
        d = Path(cert_dir) if cert_dir else default_dir()
        c, k = d / "cert.pem", d / "key.pem"
        if not (c.exists() and k.exists()):
            generate(c, k)
    return server_context(c, k), c


def quiet_handshake_errors(loop):
    """A failed TLS handshake (most often someone typing http:// instead of
    https://, or a browser refusing the certificate) is one log line, not an
    asyncio traceback."""
    default = loop.get_exception_handler()
    last = [0.0]

    def handler(loop, context):
        exc = context.get("exception")
        msg = context.get("message", "").lower()
        # "SSL handshake failed" (older Pythons) / "Error on transport creation
        # for incoming connection" (3.11+)
        if ("handshake" in msg or "transport creation" in msg) and isinstance(exc, (ssl.SSLError, OSError)):
            now = loop.time()
            if now - last[0] > 60:
                last[0] = now
                if isinstance(exc, ssl.SSLError) and exc.reason == "HTTP_REQUEST":
                    log.info("a browser asked with plain http:// - this server only answers https://")
                else:
                    log.info("a TLS handshake failed: %s", exc)
            return
        if default:
            default(loop, context)
        else:
            loop.default_exception_handler(context)

    loop.set_exception_handler(handler)
