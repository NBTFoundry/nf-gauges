# SPDX-License-Identifier: MPL-2.0
"""A fake BMW vehicle gateway for the tests: answers vehicle identification on
UDP (as on 6811) and diagnostic requests on TCP (as on 6801) the way the car's
gateway and a DME do: an acknowledgement echoing each request, then the ECU's
answer; an alive check after connecting; dynamically defined identifiers
(2C 01/03 F300); negative answers for what it does not know.

Run alone (`python3 tests/fake_gateway.py`) it serves on 127.0.0.1:16801 and
16811 with a slowly changing engine, for trying the desktop build:
  NBT_GAUGES_DISCOVERY=127.0.0.1:16811 NBT_GAUGES_DIAG_PORT=16801 build/nbt-gauges-host
"""

import math
import socket
import struct
import threading
import time

TESTER = 0xF4
DME = 0x12
DSC = 0x29  # answers one identifier with the four wheel speeds (made up for the tests)
VIN = b"WBA8E1C50GK000042"


def frame(control: int, payload: bytes) -> bytes:
    return struct.pack(">IH", len(payload), control) + payload


def support_bits(pids, base):
    """The 4 bytes of a support identifier (F400, F420...) for `pids`."""
    value = 0
    for pid in pids:
        if base < pid <= base + 32:
            value |= 1 << (32 - (pid - base))
    if any(pid > base + 32 for pid in pids):
        value |= 1  # the next block exists
    return struct.pack(">I", value)


class Engine:
    """Standard values (PID -> data bytes) and DME-specific identifiers; a
    diesel adds its fuel type and particulate filter."""

    def __init__(self, diesel=False):
        self.t0 = time.monotonic()
        self.diesel = diesel

    def standard(self):
        t = time.monotonic() - self.t0
        rpm = 2500 + 500 * math.sin(t)
        return {
            0x05: bytes([90 + 40]),                    # coolant 90 C
            0x0B: bytes([200]),                        # manifold 200 kPa
            0x0C: struct.pack(">H", int(rpm * 4)),     # rpm
            0x0E: bytes([int((20 + 64) * 2)]),         # timing 20 deg
            0x23: struct.pack(">H", 1500),             # rail 15000 kPa
            0x33: bytes([100]),                        # baro 100 kPa
            0x34: struct.pack(">HH", 26214, 0x8000),   # lambda 0.80
            0x46: bytes([20 + 40]),                    # ambient 20 C
            0x5C: bytes([105 + 40]),                   # oil 105 C
        } | ({
            0x51: bytes([4]),                                         # diesel
            0x7A: bytes([0x01, 0x04, 0xE2, 0, 0, 0, 0]),              # drop 12.5 kPa
            0x7C: bytes([0x03, 0x19, 0x00, 0x19, 0xC8, 0, 0, 0, 0]),  # 600 / 620 C
            0x8B: bytes([0x73, 0x00, 0xE0, 0x01, 0x90, 0x01, 0xA4]),  # 88%, 400 min, 420 km
        } if self.diesel else {})

    def defined(self):
        return {0x4A10: bytes([0xFC]), 0x5A20: bytes([0, 0, 0x80, 0x00])}


class FakeGateway:
    def __init__(self, port=0, ident_port=0, refuse_tester=None, batch=True, diesel=False):
        self.engine = Engine(diesel)
        self.refuse_tester = refuse_tester
        self.batch = batch
        self.lock = threading.Lock()
        self.requests = []          # (tester, target, uds bytes)
        self.alive_replies = []     # payloads of 0x13 frames
        self.testers = []           # tester addresses seen per connection
        self.dynamic = []           # [(did, pos, size)]
        self.connections = 0
        self.tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.tcp.bind(("127.0.0.1", port))
        self.tcp.listen(4)
        self.port = self.tcp.getsockname()[1]
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(("127.0.0.1", ident_port))
        self.ident_port = self.udp.getsockname()[1]
        self.closing = False
        for target in (self._accept, self._ident):
            threading.Thread(target=target, daemon=True).start()

    def close(self):
        self.closing = True
        for s in (self.tcp, self.udp):
            try:
                s.close()
            except OSError:
                pass

    def _ident(self):
        while not self.closing:
            try:
                data, peer = self.udp.recvfrom(64)
            except OSError:
                return
            if data == b"\0\0\0\0\0\x11":
                body = b"DIAGADR10BMWMAC001A37AABBCCBMWVIN" + VIN
                self.udp.sendto(frame(0x11, body), peer)

    def _accept(self):
        while not self.closing:
            try:
                conn, _ = self.tcp.accept()
            except OSError:
                return
            with self.lock:
                self.connections += 1
            threading.Thread(target=self._serve, args=(conn,), daemon=True).start()

    def _serve(self, conn):
        conn.settimeout(0.2)
        buf = b""
        sent_alive = False
        seen_tester = None
        while not self.closing:
            try:
                chunk = conn.recv(4096)
            except socket.timeout:
                chunk = None
            except OSError:
                break
            if chunk == b"":
                break
            if chunk:
                buf += chunk
            while len(buf) >= 6:
                length, control = struct.unpack(">IH", buf[:6])
                if len(buf) < 6 + length:
                    break
                payload, buf = buf[6 : 6 + length], buf[6 + length :]
                if control == 0x13:
                    with self.lock:
                        self.alive_replies.append(payload)
                    continue
                if control != 0x01 or len(payload) < 3:
                    continue
                tester, target, uds = payload[0], payload[1], payload[2:]
                if seen_tester is None:
                    seen_tester = tester
                    with self.lock:
                        self.testers.append(tester)
                if self.refuse_tester is not None and tester == self.refuse_tester:
                    conn.sendall(frame(0x40, bytes([tester, target])))
                    continue
                with self.lock:
                    self.requests.append((tester, target, uds))
                conn.sendall(frame(0x02, payload))  # the gateway's acknowledgement
                answer = self.answer(target, uds)
                if answer is not None:
                    conn.sendall(frame(0x01, bytes([target, tester]) + answer))
                if not sent_alive:
                    sent_alive = True
                    conn.sendall(frame(0x12, b""))
        conn.close()

    def answer(self, target, uds):
        if target == DSC:
            if uds == b"\x22\x4d\x10":  # FL, FR, RL, RR in 1/100 km/h
                return b"\x62\x4d\x10" + struct.pack(">HHHH", 5000, 5000, 5500, 5500)
            return bytes([0x7F, uds[0], 0x31])
        if target != DME:
            return None  # no such ECU in this car: nobody answers
        sid = uds[0]
        if sid == 0x2C and len(uds) >= 4 and uds[2:4] == b"\xf3\x00":
            if not self.batch:
                return bytes([0x7F, 0x2C, 0x31])
            if uds[1] == 0x03:
                self.dynamic = []
                return b"\x6c\x03\xf3\x00"
            if uds[1] == 0x01:
                entries = uds[4:]
                dynamic = [struct.unpack(">HBB", entries[i : i + 4]) for i in range(0, len(entries), 4)]
                if any(self.record(src) is None for src, _, _ in dynamic):
                    return bytes([0x7F, 0x2C, 0x31])
                self.dynamic = dynamic
                return b"\x6c\x01\xf3\x00"
        if sid != 0x22 or len(uds) != 3:
            return bytes([0x7F, sid, 0x11])
        did = uds[1] << 8 | uds[2]
        record = self.record(did)
        if record is None:
            return bytes([0x7F, 0x22, 0x31])
        return b"\x62" + uds[1:3] + record

    def record(self, did):
        standard = self.engine.standard()
        if did == 0xF150:
            return b"\x00\x00\x2b\x01"
        if did & 0xFF1F == 0xF400:  # support: F400, F420, ...
            return support_bits(sorted(standard), did & 0xFF)
        if did >> 8 == 0xF4 and (did & 0xFF) in standard:
            return standard[did & 0xFF]
        if did in self.engine.defined():
            return self.engine.defined()[did]
        if did == 0xF300 and self.dynamic:
            out = b""
            for src, pos, size in self.dynamic:
                rec = self.record(src) or b""
                out += rec[pos - 1 : pos - 1 + size].ljust(size, b"\0")
            return out
        return None


if __name__ == "__main__":
    gw = FakeGateway(16801, 16811)
    print(f"fake gateway on 127.0.0.1:{gw.port}, identification on UDP {gw.ident_port}")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        gw.close()
