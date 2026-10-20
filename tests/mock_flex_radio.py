"""Mock FlexRadio (SmartSDR API) for wfweb's FlexRadio tests.

Speaks enough of the real protocol for flexCommander to connect, own a
panadapter + slice, and stream:

  TCP  (control_port)  V/H greeting, C<seq>|cmd -> R<seq>|code|body, S status
  UDP  (vita_port)     receives DAX TX audio from the client
  UDP  -> client       FFT frames, meter packets, remote_audio_rx (float32
                       stereo 24 kHz, a 1 kHz tone) once the client has
                       registered "client udpport"

Everything the client sent is kept in ``commands`` (text) and ``dax_tx``
(VITA packets) for the tests to assert on.

Run standalone to point a real wfweb at it:
    python3 tests/mock_flex_radio.py --port 4992 --vita-port 4991
"""

from __future__ import annotations

import argparse
import asyncio
import math
import random
import struct
import time

OUI = 0x001C2D
ICC = 0x534C
PCC_IF_NARROW = 0x03E3
PCC_METER = 0x8002
PCC_FFT = 0x8003

PAN_ID = "0x40000000"
RX_STREAM = 0x04000008
TX_STREAM = 0x84000000

METERS = {  # id: (src, num, name, unit)
    1: ("SLC", 0, "LEVEL", "dBm"),
    2: ("TX-", 0, "FWDPWR", "dBm"),
    3: ("TX-", 0, "SWR", "SWR"),
    4: ("RAD", 0, "+13.8A", "Volts"),
}


def vita_header(ptype: int, stream: int, pcc: int, payload_words: int, count: int) -> bytes:
    words = 7 + payload_words
    w0 = (ptype << 28) | (1 << 27) | (3 << 22) | (1 << 20) | ((count & 0xF) << 16) | (words & 0xFFFF)
    return struct.pack(">IIIIIII", w0, stream, OUI, (ICC << 16) | pcc, 0, 0, 0)


class MockFlexRadio:
    def __init__(self, model: str = "FLEX-6600", verbose: bool = False):
        self.model = model
        self.verbose = verbose
        self.handle = 0x5A3B1C2D
        self.control_port = 0
        self.vita_port = 0
        self.commands: list[str] = []
        self.dax_tx: list[bytes] = []
        self.client_addr: tuple[str, int] | None = None
        self.slice: dict[str, str] = {}
        self.pan: dict[str, str] = {}
        self.pan_scale: dict[str, str] = {}   # size/range in effect, not echoed
        self.transmit = {"rfpower": "100", "dax": "0", "mon": "0", "speech_processor_enable": "0"}
        self.transmitting = False
        self.s_dbm = -63.0          # S9 + 10 dB
        self._writer: asyncio.StreamWriter | None = None
        self._server: asyncio.AbstractServer | None = None
        self._udp: asyncio.DatagramTransport | None = None
        self._stream_task: asyncio.Task | None = None
        self._count = 0
        self._fft_frame = 0
        self._phase = 0.0

    # ── lifecycle ──────────────────────────────────────────────────────────

    async def start(self, port: int = 0, vita_port: int = 0) -> None:
        self._server = await asyncio.start_server(self._client, "127.0.0.1", port)
        self.control_port = self._server.sockets[0].getsockname()[1]
        loop = asyncio.get_running_loop()
        radio = self

        class Proto(asyncio.DatagramProtocol):
            def datagram_received(self, data, addr):
                radio.dax_tx.append(data)
                if radio.verbose and len(radio.dax_tx) % 25 == 1:
                    print(f"  DAX TX packets received: {len(radio.dax_tx)}", flush=True)

        self._udp, _ = await loop.create_datagram_endpoint(Proto, local_addr=("127.0.0.1", vita_port))
        self.vita_port = self._udp.get_extra_info("sockname")[1]
        self._stream_task = asyncio.ensure_future(self._stream())

    async def stop(self) -> None:
        if self._stream_task:
            self._stream_task.cancel()
        if self._writer:
            self._writer.close()
        if self._server:
            self._server.close()
            await self._server.wait_closed()
        if self._udp:
            self._udp.close()

    def kick_client(self) -> None:
        """Another client disconnects us, as SmartSDR's multiFLEX does: the
        radio says so but leaves the socket open."""
        self.status(f"client 0x{self.handle:08X} disconnected forced=1 wan_validation_failed=0 duplicate_client_id=0")

    def drop_client(self) -> None:
        """Close the TCP connection, as a radio reboot would."""
        if self._writer:
            self._writer.close()
            self._writer = None

    # ── TCP API ───────────────────────────────────────────────────────────

    def _send(self, line: str) -> None:
        if self._writer:
            self._writer.write((line + "\n").encode())

    def status(self, body: str, handle: int | None = None) -> None:
        self._send(f"S{(self.handle if handle is None else handle):08X}|{body}")

    async def _client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self._writer = writer
        peer = writer.get_extra_info("peername")
        self._peer_ip = peer[0]
        self.slice = {}
        self.pan = {}
        self.client_addr = None
        self._send("V1.4.0.0")
        self._send(f"H{self.handle:08X}")
        try:
            while True:
                raw = await reader.readline()
                if not raw:
                    break
                line = raw.decode(errors="replace").strip()
                if not line.startswith("C") or "|" not in line:
                    continue
                seq, cmd = line[1:].split("|", 1)
                if cmd != "ping":
                    self.commands.append(cmd)
                    if self.verbose:
                        print(">", cmd.replace("\x7f", "<7f>").replace("\x1e", "<1e>"), flush=True)
                code, body = self._handle(cmd)
                self._send(f"R{seq}|{code:X}|{body}")
                await writer.drain()
        except (ConnectionResetError, asyncio.IncompleteReadError):
            pass
        finally:
            if self._writer is writer:
                self._writer = None

    def _slice_status(self, **kv) -> None:
        self.slice.update({k: str(v) for k, v in kv.items()})
        body = " ".join(f"{k}={v}" for k, v in kv.items())
        self.status(f"slice 0 {body}")

    def _pan_status(self, **kv) -> None:
        self.pan.update({k: str(v) for k, v in kv.items()})
        body = " ".join(f"{k}={v}" for k, v in kv.items())
        self.status(f"display pan {PAN_ID} {body}")

    def _handle(self, cmd: str) -> tuple[int, str]:
        words = cmd.split()
        if cmd.startswith("client udpport"):
            self.client_addr = (self._peer_ip, int(words[2]))
            return 0, ""
        if cmd.startswith("sub meter"):
            defs = "#".join(f"{i}.src={s}#{i}.num={n}#{i}.nam={nm}#{i}.unit={u}"
                            for i, (s, n, nm, u) in METERS.items())
            self.status(f"meter {defs}#", handle=0)
            return 0, ""
        if cmd.startswith("sub") or cmd.startswith("client") or cmd in ("keepalive enable", "ping"):
            return 0, ""
        if cmd == "info":
            return 0, f'model="{self.model}",chassis_serial="1234-5678",name="Mock",callsign="VE3NSV",region="USA"'
        if cmd == "ant list":
            return 0, "ANT1,ANT2,RX_A,XVTA"
        if cmd == "slice list":
            return 0, ""
        if cmd.startswith("display panafall create"):
            self.pan_scale = {}
            self._pan_status(center="14.200000", bandwidth="0.200000", x_pixels=50, y_pixels=20,
                             min_dbm="-125.00", max_dbm="-40.00",
                             rfgain=0, client_handle=f"0x{self.handle:08X}")
            return 0, PAN_ID
        if cmd.startswith("display pan set"):
            kv = dict(w.split("=", 1) for w in words[4:] if "=" in w)
            mapped = {("x_pixels" if k == "xpixels" else "y_pixels" if k == "ypixels" else k): v
                      for k, v in kv.items()}
            # Like a FLEX-6400: the new size, dBm range and bandwidth take
            # effect, but the status keeps reporting the pan's old ones.
            own = {k: v for k, v in mapped.items() if k in ("x_pixels", "y_pixels", "min_dbm", "max_dbm", "bandwidth")}
            self.pan_scale.update(own)
            echoed = {k: v for k, v in mapped.items() if k not in own}
            if echoed:
                self._pan_status(**echoed)
            return 0, ""
        if cmd.startswith("slice create"):
            kv = dict(w.split("=", 1) for w in words[2:] if "=" in w)
            self._slice_status(in_use=1, client_handle=f"0x{self.handle:08X}", pan=kv.get("pan", PAN_ID),
                               RF_frequency=float(kv.get("freq", "14.2")), mode=kv.get("mode", "USB"),
                               filter_lo=100, filter_hi=2800, audio_level=50, agc_mode="med",
                               agc_threshold=65, nb=0, nr=0, anf=0, rxant="ANT1", rit_on=0, rit_freq=0)
            return 0, "0"
        if cmd.startswith("slice tune"):
            mhz = float(words[3])
            # Like the radio: the slice moves, the panadapter stays put
            # unless the slice leaves the screen.
            self._slice_status(RF_frequency=f"{mhz:.6f}")
            return 0, ""
        if cmd.startswith("slice set"):
            # Like the radio: the change takes effect, but the client that
            # made it gets no status echo of it.
            kv = dict(w.split("=", 1) for w in words[3:] if "=" in w)
            self.slice.update(kv)
            return 0, ""
        if cmd.startswith("filt"):
            self.slice.update(filter_lo=words[2], filter_hi=words[3])   # not echoed
            return 0, ""
        if cmd.startswith("stream create type=remote_audio_rx"):
            return 0, f"0x{RX_STREAM:08X}"
        if cmd.startswith("stream create type=dax_tx"):
            return 0, f"0x{TX_STREAM:08X}"
        if cmd.startswith("stream remove"):
            return 0, ""
        if cmd.startswith("xmit"):
            self.transmitting = words[1] == "1"
            self.status(f"interlock state={'TRANSMITTING' if self.transmitting else 'READY'} source=SW")
            return 0, ""
        if cmd.startswith("transmit set"):
            kv = dict(w.split("=", 1) for w in words[2:] if "=" in w)
            if "miclevel" in kv:
                kv["mic_level"] = kv.pop("miclevel")
            self.transmit.update(kv)   # not echoed to the setter either
            return 0, ""
        if cmd.startswith("cw ") or cmd.startswith("cwx ") or cmd.startswith("atu "):
            return 0, ""
        return 0x50000015, "Unknown command"

    # ── VITA-49 streams ───────────────────────────────────────────────────

    def _vita(self, pkt: bytes) -> None:
        if self.client_addr and self._udp:
            self._udp.sendto(pkt, self.client_addr)

    async def _stream(self) -> None:
        last = time.monotonic()
        while True:
            await asyncio.sleep(0.05)
            now = time.monotonic()
            dt, last = now - last, now
            if not self.client_addr:
                continue
            self._count += 1
            self._send_fft()
            self._send_meters()
            self._send_audio(int(24000 * dt))

    def _send_fft(self) -> None:
        if not self.pan:
            return
        total = int(self.pan_scale.get("x_pixels", self.pan.get("x_pixels", 100)))
        ypix = int(self.pan_scale.get("y_pixels", self.pan.get("y_pixels", 20)))
        # Noise with some jitter near the bottom, a medium signal off to one
        # side and a strong carrier in the middle — all well inside the
        # vertical range, as on a real band.
        bins = [int(ypix * (0.78 + 0.04 * random.random())) for _ in range(total)]
        for i in range(total // 4 - 3, total // 4 + 3):
            bins[i] = int(ypix * 0.55)
        bins[total // 2] = int(ypix * 0.35)
        self._fft_frame += 1
        half = total // 2
        for start, chunk in ((0, bins[:half]), (half, bins[half:])):
            sub = struct.pack(">HHHHI", start, len(chunk), 2, total, self._fft_frame)
            payload = sub + b"".join(struct.pack(">H", b) for b in chunk)
            words = (len(payload) + 3) // 4
            payload += b"\0" * (words * 4 - len(payload))
            self._vita(vita_header(3, int(PAN_ID, 16), PCC_FFT, words, self._count) + payload)

    def _send_meters(self) -> None:
        fwd_dbm = 10 * math.log10(50.0 * 1000) if self.transmitting else -50.0   # 50 W
        # A real radio spreads its meters over several packets. The TX/RAD
        # ones go first and the slice LEVEL right behind them, so a client
        # that drops whatever lands inside its rate limit never sees the S
        # meter (flexCommander once did).
        for vals in ({2: fwd_dbm * 128, 3: 1.5 * 128, 4: 13.8 * 256}, {1: self.s_dbm * 128}):
            payload = b"".join(struct.pack(">Hh", i, int(round(v))) for i, v in vals.items())
            self._vita(vita_header(3, 0x700, PCC_METER, len(payload) // 4, self._count) + payload)

    def _send_audio(self, frames: int) -> None:
        while frames > 0:
            n = min(128, frames)
            frames -= n
            samples = []
            for _ in range(n):
                s = 0.25 * math.sin(self._phase)
                self._phase += 2 * math.pi * 1000 / 24000
                samples += [s, s]
            payload = struct.pack(f">{len(samples)}f", *samples)
            self._vita(vita_header(1, RX_STREAM, PCC_IF_NARROW, len(payload) // 4, self._count) + payload)

    # ── helpers for tests ─────────────────────────────────────────────────

    def find(self, prefix: str) -> list[str]:
        return [c for c in self.commands if c.startswith(prefix)]


async def _main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=4992)
    ap.add_argument("--vita-port", type=int, default=4991)
    args = ap.parse_args()
    radio = MockFlexRadio(verbose=True)
    await radio.start(args.port, args.vita_port)
    print(f"mock Flex on TCP {radio.control_port}, VITA UDP {radio.vita_port}", flush=True)
    while True:
        await asyncio.sleep(3600)


if __name__ == "__main__":
    asyncio.run(_main())
