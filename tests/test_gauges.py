# SPDX-License-Identifier: MPL-2.0
"""The desktop build as NBT Manager starts it: the health check without a
window, then user launches under Xvfb reading a fake gateway (fake_gateway.py):
the search, the identification, the standard values, batched reading,
definition files, the alive check, a refused tester address; NBT Manager's
diagnostics (bridge 1.8 `diag`) in place of the gateway; and the demo."""

import os
from pathlib import Path
import re
import select
import shutil
import signal
import socket
import struct
import subprocess
import threading
import tempfile
import time
import unittest

from fake_gateway import FakeGateway

ROOT = Path(__file__).resolve().parents[1]
APP = Path(os.environ.get("NBT_GAUGES_APP", ROOT / "build/nbt-gauges-host"))


def xvfb():
    r, w = os.pipe()
    proc = subprocess.Popen(
        ["Xvfb", "-displayfd", str(w), "-screen", "0", "1280x480x24", "-nolisten", "tcp"],
        pass_fds=(w,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.close(w)
    number = b""
    while not number.endswith(b"\n"):
        ready, _, _ = select.select([r], [], [], 10)
        chunk = os.read(r, 1) if ready else b""
        if not chunk:
            break
        number += chunk
    os.close(r)
    return proc, ":" + number.decode().strip()


class App:
    """The application, its log (standard error) read by line."""

    def __init__(self, env, args=(), pass_fds=(), diag_fd=None):
        full = {k: v for k, v in os.environ.items() if not k.startswith("NBT_")}
        full.update(env)
        command = [str(APP), *args]
        if diag_fd is not None:  # as descriptor 10, as the manager gives it
            command = ["bash", "-c", f'exec "$0" "$@" 10<&{diag_fd} {diag_fd}<&-', *command]
            pass_fds = (*pass_fds, diag_fd)
        self.proc = subprocess.Popen(command, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.PIPE, env=full, pass_fds=pass_fds)
        self.lines, self.seen, self.buf = [], [], b""

    def wait(self, pattern, timeout=20.0):
        regex, deadline = re.compile(pattern), time.monotonic() + timeout
        while True:
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                line = raw.decode(errors="replace")
                self.lines.append(line)
                self.seen.append(line)
            for i, line in enumerate(self.lines):
                if regex.search(line):
                    del self.lines[: i + 1]
                    return line
            fd = self.proc.stderr.fileno()
            left = deadline - time.monotonic()
            ready, _, _ = select.select([fd], [], [], max(0.0, left)) if left > 0 else ([], [], [])
            chunk = os.read(fd, 4096) if ready else b""
            if not chunk:
                raise AssertionError(f"no line matching {pattern!r}; saw {self.seen[-30:]!r}")
            self.buf += chunk

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
        _, err = self.proc.communicate(timeout=20)
        self.buf += err
        return self.proc.returncode


@unittest.skipUnless(os.access(APP, os.X_OK), f"{APP} not built")
class HealthCheckTest(unittest.TestCase):
    def test_reports_ready_without_a_window(self):
        """As NBT Manager runs it after an install: the screens are built
        off screen, ready on --ready-fd, then SIGTERM ends it with status 0."""
        with tempfile.TemporaryDirectory() as data:
            lease_fd = os.open(Path(data) / "lease", os.O_CREAT | os.O_RDWR, 0o600)
            rfd, wfd = os.pipe()
            env = {"NBT_LAUNCH": "check", "NBT_DATA_DIR": data}
            app = App(env, ["--ready-fd", str(wfd), "--lease-fd", str(lease_fd)], (wfd, lease_fd))
            os.close(wfd)
            try:
                readable, _, _ = select.select([rfd], [], [], 15.0)
                first = os.read(rfd, 16) if readable else b""
                self.assertTrue(first.startswith(b"R"), f"expected R (ready), got {first!r}")
                app.wait(r"screens ready, 1280x480")
                self.assertEqual(app.stop(), 0, app.seen[-20:])
            finally:
                if app.proc.poll() is None:
                    app.proc.kill()
                os.close(rfd)
                os.close(lease_fd)


@unittest.skipUnless(os.access(APP, os.X_OK), f"{APP} not built")
@unittest.skipUnless(shutil.which("Xvfb"), "Xvfb not installed")
class GatewayTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="nbt-gauges-"))
        self.files, self.data = self.tmp / "files", self.tmp / "data"
        self.files.mkdir()
        self.data.mkdir()
        self.x, self.display = xvfb()

    def tearDown(self):
        self.x.terminate()
        self.x.wait(timeout=5)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def start(self, gateway, discover=True, known=False, **settings):
        env = {"NBT_LAUNCH": "user", "DISPLAY": self.display, "LIBGL_ALWAYS_SOFTWARE": "1",
               "NBT_DATA_DIR": str(self.data), "NBT_FILES_DIR": str(self.files),
               "NBT_GAUGES_KNOWN_GATEWAY": ""}
        if known:
            env["NBT_GAUGES_KNOWN_GATEWAY"] = "127.0.0.1"
            env["NBT_GAUGES_DIAG_PORT"] = str(gateway.port)
        elif discover:
            env["NBT_GAUGES_DISCOVERY"] = f"127.0.0.1:{gateway.ident_port}"
            env["NBT_GAUGES_DIAG_PORT"] = str(gateway.port)
        else:
            env["NBT_GAUGES_GATEWAY"] = f"127.0.0.1:{gateway.port}"
        env.update({"NBT_SETTING_" + k.upper(): v for k, v in settings.items()})
        app = App(env)
        self.addCleanup(lambda: app.proc.poll() is None and app.proc.kill())
        app.wait(r"screens ready")
        return app

    def gateway(self, **kw):
        gw = FakeGateway(**kw)
        self.addCleanup(gw.close)
        return gw

    def test_finds_the_gateway_and_reads_the_standard_values_batched(self):
        gw = self.gateway()
        app = self.start(gw)
        app.wait(r"dme: gateway 127\.0\.0\.1 \(address 10, VIN WBA8E1C50GK000042\)")
        app.wait(r"dme: diagnostic index 00 00 2b 01")
        line = app.wait(r"dme: standard values: ")
        self.assertIn("05 0b 0c 0e 23 33 34 46 5c", line)
        app.wait(r"dme: 12 f40b answers: c8")
        app.wait(r"dme: 12 f405 answers: 82")
        app.wait(r"dme: 5 fast values in one read \(f300, 10 bytes\)")
        time.sleep(1.0)
        self.assertEqual(app.stop(), 0)
        # The alive check was answered with our tester address.
        self.assertIn(b"\x00\xf4", gw.alive_replies)
        # Only read services, the dynamic identifier cleared when done.
        services = {uds[0] for _, _, uds in gw.requests}
        self.assertLessEqual(services, {0x22, 0x2C})
        self.assertEqual(gw.requests[-1][2], b"\x2c\x03\xf3\x00")
        batched = sum(1 for _, _, uds in gw.requests if uds == b"\x22\xf3\x00")
        self.assertGreater(batched, 10)

    def test_the_known_gateway_address_before_the_broadcast(self):
        # The car blocks the UDP broadcast on the gateway link: the usual
        # address is tried on its TCP port first (here 127.0.0.1).
        gw = self.gateway()
        app = self.start(gw, discover=False, known=True)
        app.wait(r"dme: gateway 127\.0\.0\.1 answers on its diagnostic port")
        app.wait(r"dme: 12 f40b answers: c8")
        self.assertEqual(app.stop(), 0)

    def test_reads_one_value_per_request_when_batching_is_refused(self):
        gw = self.gateway(batch=False)
        app = self.start(gw, discover=False)
        app.wait(r"dme: 12 f40c answers")
        app.wait(r"dme: no batched reading \(2c 01 f300: request out of range\)")
        time.sleep(0.5)
        self.assertEqual(app.stop(), 0)
        singles = {uds for _, _, uds in gw.requests if uds[:2] == b"\x22\xf4"}
        self.assertIn(b"\x22\xf4\x0c", singles)

    def test_definition_file_values_and_refusals(self):
        (self.files / "dme.csv").write_text(
            "id,label,unit,ecu,did,pos,type,mul,div,add,min,max\n"
            "corr1,,,12,4A10,1,s8,0.375,1,0,,\n"
            "wastegate,Wastegate,%,12,5A20,3,u16,100,65535,0,0,100\n"
            "corr2,,,12,4A11,1,s8,0.375,1,0,,\n"
            "broken line\n")
        gw = self.gateway()
        app = self.start(gw, discover=False)
        app.wait(r"definitions: dme\.csv: 3 values")
        app.wait(r"definitions: dme\.csv line 5: expected")
        app.wait(r"dme: 12 4a11 refused \(request out of range\): left out")
        app.wait(r"dme: 7 fast values in one read \(f300, 13 bytes\)")
        time.sleep(0.5)
        self.assertEqual(app.stop(), 0)
        # The fast definitions that answer joined the batch at their positions.
        defined = [uds for _, _, uds in gw.requests if uds[:2] == b"\x2c\x01"][-1]
        self.assertIn(b"\x4a\x10\x01\x01", defined)
        self.assertIn(b"\x5a\x20\x03\x02", defined)
        self.assertNotIn(b"\x4a\x11", defined)

    def test_wheel_speeds_from_another_ecu_and_one_that_never_answers(self):
        (self.files / "chassis.csv").write_text(
            "id,label,unit,ecu,did,pos,type,mul,div,add,min,max\n"
            "wheel_fl,,,29,4D10,1,u16,1,100,0,,\n"
            "wheel_fr,,,29,4D10,3,u16,1,100,0,,\n"
            "wheel_rl,,,29,4D10,5,u16,1,100,0,,\n"
            "wheel_rr,,,29,4D10,7,u16,1,100,0,,\n"
            "front_share,,,19,4D20,1,u8,100,255,0,,\n")
        gw = self.gateway()
        app = self.start(gw, discover=False)
        app.wait(r"definitions: chassis\.csv: 5 values")
        app.wait(r"dme: 29 4d10 answers: 13 88 13 88 15 7c 15 7c")
        app.wait(r"dme: 5 fast values in one read")
        app.wait(r"dme: 19 4d20: no answer: left out")
        time.sleep(1.0)
        self.assertEqual(app.stop(), 0)
        self.assertEqual(sum(1 for l in app.seen if "29 4d10 answers" in l), 1)
        # The four speeds in one request each cycle; the absent ECU asked
        # three times, then left alone; the batch only for the DME.
        dsc = [uds for _, target, uds in gw.requests if target == 0x29]
        self.assertGreater(len(dsc), 5)
        self.assertEqual(set(dsc), {b"\x22\x4d\x10"})
        self.assertEqual(sum(1 for _, target, _ in gw.requests if target == 0x19), 3)
        self.assertTrue(all(target == 0x12 for _, target, uds in gw.requests if uds[0] == 0x2C))

    def test_through_the_managers_diagnostics_when_granted(self):
        # The manager's side of NBT_DIAG_FD=10: requests to the ECUs of the
        # package's policy answered (from the fake car), others refused.
        (self.files / "chassis.csv").write_text(
            "id,label,unit,ecu,did,pos,type,mul,div,add,min,max\n"
            "wheel_fl,,,29,4D10,1,u16,1,100,0,,\n"
            "front_share,,,19,4D20,1,u8,100,255,0,,\n")
        gw = self.gateway()
        ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.addCleanup(ours.close)
        asked = []

        def manager():
            while True:
                try:
                    data = ours.recv(2048)
                except OSError:
                    return
                if not data:
                    return
                ecu, uds = data[1], data[6:]
                asked.append((ecu, uds))
                answer = gw.answer(ecu, uds) if ecu in (0x12, 0x29) else None
                status = 0 if answer else 2 if ecu not in (0x12, 0x29) else 1
                ours.send(b"\x81" + bytes([ecu]) + data[2:4] + bytes([status, 1]) +
                          (answer or b""))

        threading.Thread(target=manager, daemon=True).start()
        env = {"NBT_LAUNCH": "user", "DISPLAY": self.display, "LIBGL_ALWAYS_SOFTWARE": "1",
               "NBT_DATA_DIR": str(self.data), "NBT_FILES_DIR": str(self.files),
               "NBT_GAUGES_KNOWN_GATEWAY": "", "NBT_DIAG_FD": "10"}
        app = App(env, diag_fd=theirs.fileno())
        theirs.close()
        self.addCleanup(lambda: app.proc.poll() is None and app.proc.kill())
        app.wait(r"dme: through NBT Manager's diagnostics")
        app.wait(r"dme: diagnostic index 00 00 2b 01")
        app.wait(r"dme: 12 f40b answers: c8")
        app.wait(r"dme: 29 4d10 answers: 13 88")
        app.wait(r"dme: 19 4d20: not in this package's diagnostics policy: left out")
        app.wait(r"dme: \d+ fast values in one read")
        self.assertEqual(app.stop(), 0)
        # Nothing of our own on the gateway; a refused ECU asked once.
        self.assertEqual(gw.requests, [])
        self.assertEqual(sum(1 for ecu, _ in asked if ecu == 0x19), 1)
        self.assertEqual(asked[-1], (0x12, b"\x2c\x03\xf3\x00"))

    def test_a_diesel_shows_dpf(self):
        gw = self.gateway(diesel=True)
        app = self.start(gw, discover=False)
        line = app.wait(r"dme: standard values: ")
        self.assertIn("51", line)
        self.assertIn("7a 7c", line)
        self.assertIn("8b", line)
        app.wait(r"ui: a diesel: DPF in place of IGNITION")
        app.wait(r"dme: 12 f48b answers: 73 00 e0 01 90 01 a4")
        self.assertEqual(app.stop(), 0)

    def test_another_tester_address_when_the_gateway_refuses_ours(self):
        gw = self.gateway(refuse_tester=0xF4)
        app = self.start(gw, discover=False)
        app.wait(r"gateway refused the request \(control 0x0040\)")
        app.wait(r"dme: connected to the gateway, tester f5")
        app.wait(r"dme: standard values: ")
        self.assertEqual(app.stop(), 0)
        self.assertEqual(gw.testers[:2], [0xF4, 0xF5])

    def test_demo_and_dme_off(self):
        gw = self.gateway()
        app = self.start(gw, source="demo", drivetrain="rwd")
        app.wait(r"dme: off")
        time.sleep(1.0)
        self.assertEqual(app.stop(), 0)
        self.assertEqual(gw.connections, 0)


if __name__ == "__main__":
    unittest.main()
