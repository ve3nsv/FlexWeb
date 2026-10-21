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
import json
import asyncio
import math
import random
import struct
import time
from pathlib import Path

OUI = 0x001C2D
ICC = 0x534C
PCC_IF_NARROW = 0x03E3
PCC_METER = 0x8002
PCC_OPUS = 0x8005
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


class _OpusEncoder:
    """libopus through ctypes (CI has libopus0): 24 kHz stereo, 10 ms CBR
    frames of 80 bytes, so the VITA-49 payload needs no padding."""

    def __init__(self):
        import ctypes
        import ctypes.util
        name = ctypes.util.find_library("opus")
        if not name:
            raise OSError("libopus not found")
        self.ct = ctypes
        self.lib = ctypes.CDLL(name)
        self.lib.opus_encoder_create.restype = ctypes.c_void_p
        self.lib.opus_encode.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16), ctypes.c_int,
                                         ctypes.c_char_p, ctypes.c_int32]
        self.lib.opus_encoder_ctl.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int32]
        err = ctypes.c_int(0)
        self.st = self.lib.opus_encoder_create(24000, 2, 2048, ctypes.byref(err))
        if err.value != 0 or not self.st:
            raise OSError("opus_encoder_create failed")
        self.lib.opus_encoder_ctl(self.st, 4002, 64000)   # OPUS_SET_BITRATE
        self.lib.opus_encoder_ctl(self.st, 4006, 0)       # OPUS_SET_VBR off

    def encode(self, stereo_int16: list[int]) -> bytes:
        ct = self.ct
        pcm = (ct.c_int16 * len(stereo_int16))(*stereo_int16)
        out = ct.create_string_buffer(1500)
        n = self.lib.opus_encode(self.st, pcm, len(stereo_int16) // 2, out, 1500)
        if n < 0:
            raise OSError(f"opus_encode {n}")
        return out.raw[:n]


class MockFlexRadio:
    def __init__(self, model: str = "FLEX-6600", verbose: bool = False):
        self.model = model
        self.verbose = verbose
        self.handle = 0x5A3B1C2D
        self.control_port = 0
        self.vita_port = 0
        self.commands: list[str] = []
        self.dax_tx: list[bytes] = []
        self.dax_tx_times: list[float] = []
        self.key_delay = 0.0        # seconds from xmit 1 to TRANSMITTING
        self.on_air_at = 0.0
        self._xmit_requested = False
        self.client_addr: tuple[str, int] | None = None
        self.slice: dict[str, str] = {}
        self.slice_b: dict[str, str] = {}     # a second slice of ours (VFO B)
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
        # SmartLink: the radio is reached over TLS and waits for
        # "wan validate handle=<wan_handle>" before it says V/H.
        self.wan = False
        self.wan_handle = "WANHANDLE42"
        self.udp_registers: list[bytes] = []
        # remote_audio_rx compression=opus: Opus frames instead of float.
        self.rx_opus = False
        # Band antenna recall: like the radio, a slice that changes band gets
        # that band's antennas back a moment later, after the client's own
        # commands. recall_hf is what it recalls below 2 m (a stale value).
        self.recall_hf = {"rxant": "ANT1", "txant": "ANT1"}
        # Radio memories ("sub memories all", memory apply/create/set/remove).
        self.memories = {
            0: {"owner": "VE3NSV", "group": "Nets", "freq": "3.873000", "name": "Club\x7fnet",
                "mode": "LSB", "step": "100", "repeater": "SIMPLEX", "repeater_offset": "0.000000",
                "tone_mode": "OFF", "tone_value": "67.0", "power": "100",
                "rx_filter_low": "-2700", "rx_filter_high": "-100", "squelch": "0", "squelch_level": "20"},
            1: {"owner": "VE3NSV", "group": "", "freq": "14.074000", "name": "FT8",
                "mode": "DIGU", "step": "10", "repeater": "SIMPLEX", "repeater_offset": "0.000000",
                "tone_mode": "OFF", "tone_value": "67.0", "power": "50",
                "rx_filter_low": "0", "rx_filter_high": "3000", "squelch": "0", "squelch_level": "20"},
        }
        self.opus_sent = 0
        self._opus: _OpusEncoder | None = None
        self._opus_pending: list[int] = []

    # ── lifecycle ──────────────────────────────────────────────────────────

    async def start(self, port: int = 0, vita_port: int = 0, tls=None) -> None:
        self.wan = tls is not None
        self._server = await asyncio.start_server(self._client, "127.0.0.1", port, ssl=tls)
        self.control_port = self._server.sockets[0].getsockname()[1]
        loop = asyncio.get_running_loop()
        radio = self

        class Proto(asyncio.DatagramProtocol):
            def datagram_received(self, data, addr):
                if data.startswith(b"client udp_register"):
                    # SmartLink: VITA-49 goes back to where this came from.
                    radio.udp_registers.append(data)
                    radio.client_addr = addr
                    return
                radio.dax_tx.append(data)
                radio.dax_tx_times.append(time.monotonic())
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

    def _go_on_air(self) -> None:
        if not self._xmit_requested or self.transmitting:
            return
        self.transmitting = True
        self.on_air_at = time.monotonic()
        self.status("interlock state=TRANSMITTING source=SW")

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
        if not self.wan:
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
                if self.wan and cmd.startswith("wan validate"):
                    self.commands.append(cmd)
                    if cmd != f"wan validate handle={self.wan_handle}":
                        self._send(f"R{seq}|50000016|bad handle")
                        break
                    self._send(f"R{seq}|0|")
                    self._send("V1.4.0.0")
                    self._send(f"H{self.handle:08X}")
                    await writer.drain()
                    continue
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

    def _memory_status(self, i: int) -> None:
        body = " ".join(f"{k}={v}" for k, v in self.memories[i].items())
        self.status(f"memory {i} {body}")

    def _pan_status(self, **kv) -> None:
        self.pan.update({k: str(v) for k, v in kv.items()})
        body = " ".join(f"{k}={v}" for k, v in kv.items())
        self.status(f"display pan {PAN_ID} {body}")

    def _handle(self, cmd: str) -> tuple[int, str]:
        words = cmd.split()
        if cmd.startswith("client udpport"):
            if self.wan:
                return 0x50001000, "not supported over WAN"
            self.client_addr = (self._peer_ip, int(words[2]))
            return 0, ""
        if cmd == "sub memories all":
            for i in sorted(self.memories):
                self._memory_status(i)
            return 0, ""
        if cmd.startswith("memory apply"):
            m = self.memories.get(int(words[2]))
            if not m:
                return 0x50000016, "no such memory"
            self._slice_status(RF_frequency=m["freq"], mode=m["mode"])
            return 0, ""
        if cmd == "memory create":
            i = max(self.memories, default=-1) + 1
            self.memories[i] = {"owner": "VE3NSV", "group": "", "name": "",
                                "freq": f'{float(self.slice.get("RF_frequency", 14.2)):.6f}',
                                "mode": self.slice.get("mode", "USB"), "power": "100"}
            self._memory_status(i)
            return 0, str(i)
        if cmd.startswith("memory set"):
            i = int(words[2])
            if i not in self.memories:
                return 0x50000016, "no such memory"
            kv = dict(w.split("=", 1) for w in words[3:] if "=" in w)
            self.memories[i].update(kv)
            self._memory_status(i)
            return 0, ""
        if cmd.startswith("memory remove"):
            i = int(words[2])
            self.memories.pop(i, None)
            self.status(f"memory {i} removed")
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
        if cmd.startswith("display pan rfgain_info"):
            return 0, "-8,32,8"           # a FLEX-6400's range
        if cmd.startswith("display pan set") and "rfgain=" in cmd:
            g = int(cmd.split("rfgain=")[1].split()[0])
            if g < -8 or g > 32 or g % 8:
                return 0x50000016, ""     # the radio refuses an off-step value
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
        if cmd.startswith("slice create") and self.slice.get("in_use") == "1":
            # A second slice: VFO B.
            kv = dict(w.split("=", 1) for w in words[2:] if "=" in w)
            self.slice_b = {"in_use": "1", "client_handle": f"0x{self.handle:08X}",
                            "RF_frequency": kv.get("freq", "14.2"), "mode": kv.get("mode", "USB"), "tx": "0"}
            self.status("slice 1 " + " ".join(f"{k}={v}" for k, v in self.slice_b.items()))
            return 0, "1"
        if words[:2] in (["slice", "tune"], ["slice", "set"]) and len(words) > 2 and words[2] == "1":
            if words[1] == "tune":
                self.slice_b["RF_frequency"] = f"{float(words[3]):.6f}"
                self.status(f"slice 1 RF_frequency={self.slice_b['RF_frequency']}")
            else:
                self.slice_b.update(dict(w.split("=", 1) for w in words[3:] if "=" in w))
            return 0, ""
        if cmd.startswith("filt 1 "):
            return 0, ""
        if cmd.startswith("slice create"):
            kv = dict(w.split("=", 1) for w in words[2:] if "=" in w)
            self._slice_status(in_use=1, client_handle=f"0x{self.handle:08X}", pan=kv.get("pan", PAN_ID),
                               RF_frequency=float(kv.get("freq", "14.2")), mode=kv.get("mode", "USB"),
                               filter_lo=100, filter_hi=2800, audio_level=50, agc_mode="med",
                               agc_threshold=65, nb=0, nr=0, anf=0, rxant="ANT1", txant="ANT1",
                               ant_list="ANT1,ANT2,RX_A,XVTA", tx_ant_list="ANT1,ANT2,XVTA",
                               rit_on=0, rit_freq=0)
            return 0, "0"
        if cmd.startswith("slice tune"):
            mhz = float(words[3])
            was_2m = 144.0 <= float(self.slice.get("RF_frequency", 14.2)) <= 148.0
            if was_2m and not (144.0 <= mhz <= 148.0):
                recall = dict(self.recall_hf)
                asyncio.get_running_loop().call_later(
                    0.3, lambda: self._slice_status(**recall))
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
            self.rx_opus = "compression=opus" in cmd
            if self.rx_opus and self._opus is None:
                try:
                    self._opus = _OpusEncoder()
                except OSError as e:
                    print("mock: no Opus encoder:", e, flush=True)
            return 0, f"0x{RX_STREAM:08X}"
        if cmd.startswith("stream create type=dax_tx"):
            return 0, f"0x{TX_STREAM:08X}"
        if cmd.startswith("stream remove"):
            return 0, ""
        if cmd.startswith("xmit"):
            on = words[1] == "1"
            self._xmit_requested = on
            if on and self.key_delay > 0:
                # A real radio reports PTT_REQUESTED, then TRANSMITTING once
                # its TX delays have run; DAX audio before that is lost.
                self.status("interlock state=PTT_REQUESTED source=SW")
                asyncio.get_running_loop().call_later(self.key_delay, self._go_on_air)
            else:
                self.transmitting = on
                if on:
                    self.on_air_at = time.monotonic()
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
        if cmd.startswith("transmit tune "):
            self.transmit["tune"] = words[2]
            return 0, ""
        if cmd.startswith("radio set "):
            return 0, ""
        if cmd.startswith("tnf create"):
            self._tnf_id = getattr(self, "_tnf_id", 0) + 1
            freq = cmd.split("freq=")[1].split()[0]
            self.tnfs = getattr(self, "tnfs", {})
            self.tnfs[self._tnf_id] = freq
            self.status(f"tnf {self._tnf_id} freq={freq} width=0.000100 depth=1 permanent=0")
            return 0, str(self._tnf_id)
        if cmd.startswith("tnf remove "):
            getattr(self, "tnfs", {}).pop(int(words[2]), None)
            self.status(f"tnf {words[2]} removed")
            return 0, ""
        if cmd.startswith("tnf set "):
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
        if self.rx_opus:
            if self._opus is None:
                return
            for _ in range(frames):
                s = int(0.25 * 32767 * math.sin(self._phase))
                self._phase += 2 * math.pi * 1000 / 24000
                self._opus_pending += [s, s]
            while len(self._opus_pending) >= 480:      # 10 ms stereo
                chunk, self._opus_pending = self._opus_pending[:480], self._opus_pending[480:]
                data = self._opus.encode(chunk)
                data += b"\0" * (-len(data) % 4)
                self._vita(vita_header(1, RX_STREAM, PCC_OPUS, len(data) // 4, self._count) + data)
                self.opus_sent += 1
            return
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


class MockSmartLink:
    """The FlexRadio account service and the SmartLink broker, for one radio.

    HTTPS on ``auth_port`` answers the Auth0 token requests (password and
    refresh-token grants); TLS on ``server_port`` speaks the SmartLink line
    protocol: register -> radio list, application connect -> connect_ready.
    Point wfweb at it with WFWEB_SMARTLINK_AUTH / WFWEB_SMARTLINK_SERVER.
    """

    EMAIL = "op@example.com"
    PASSWORD = "secret-pw"
    ID_TOKEN = "id-token-1"
    REFRESH = "refresh-token-1"

    def __init__(self, radio: MockFlexRadio, tls):
        self.radio = radio
        self.tls = tls
        self.auth_port = 0
        self.server_port = 0
        self.token_requests: list[dict] = []
        self.lines: list[str] = []
        self._servers = []

    async def start(self) -> None:
        a = await asyncio.start_server(self._auth, "127.0.0.1", 0, ssl=self.tls)
        b = await asyncio.start_server(self._broker, "127.0.0.1", 0, ssl=self.tls)
        self._servers = [a, b]
        self.auth_port = a.sockets[0].getsockname()[1]
        self.server_port = b.sockets[0].getsockname()[1]

    async def stop(self) -> None:
        for srv in self._servers:
            srv.close()

    async def _auth(self, reader, writer) -> None:
        try:
            head = await reader.readuntil(b"\r\n\r\n")
            length = 0
            for h in head.decode(errors="replace").split("\r\n"):
                if h.lower().startswith("content-length:"):
                    length = int(h.split(":", 1)[1])
            req = json.loads(await reader.readexactly(length) or b"{}")
            self.token_requests.append(req)
            ok = (req.get("username") == self.EMAIL and req.get("password") == self.PASSWORD) \
                or req.get("refresh_token") == self.REFRESH
            if ok:
                status, body = "200 OK", {"id_token": self.ID_TOKEN, "refresh_token": self.REFRESH}
            else:
                status, body = "403 Forbidden", {"error": "invalid_grant",
                                                 "error_description": "Wrong email or password."}
            data = json.dumps(body).encode()
            writer.write(f"HTTP/1.1 {status}\r\nContent-Type: application/json\r\n"
                         f"Content-Length: {len(data)}\r\nConnection: close\r\n\r\n".encode() + data)
            await writer.drain()
        except (asyncio.IncompleteReadError, ConnectionResetError, ValueError):
            pass
        finally:
            writer.close()

    async def _broker(self, reader, writer) -> None:
        r = self.radio
        try:
            while True:
                raw = await reader.readline()
                if not raw:
                    break
                line = raw.decode(errors="replace").strip()
                self.lines.append(line)
                if line.startswith("application register"):
                    if f"token={self.ID_TOKEN}" not in line:
                        writer.write(b"application registration_invalid\n")
                        continue
                    writer.write(b"application info public_ip=127.0.0.1\n")
                    writer.write((f"radio list name=Mock radio_name=Mock callsign=VE3NSV serial=1234-5678 "
                                  f"model={r.model} status=Available public_ip=127.0.0.1 "
                                  f"public_tls_port={r.control_port} public_udp_port={r.vita_port} "
                                  f"upnp_supported=0 gui_client_programs=|\n").encode())
                elif line.startswith("application connect"):
                    serial = dict(w.split("=", 1) for w in line.split() if "=" in w).get("serial")
                    writer.write(f"radio connect_ready handle={r.wan_handle} serial={serial}\n".encode())
                await writer.drain()
        except (ConnectionResetError, asyncio.IncompleteReadError):
            pass


def tls_context():
    """Server-side TLS for the mocks (self-signed, localhost tests only)."""
    import ssl
    here = Path(__file__).resolve().parent
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(here / "smartlink_test_cert.pem", here / "smartlink_test_key.pem")
    return ctx


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
