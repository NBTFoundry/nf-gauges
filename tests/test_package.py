# SPDX-License-Identifier: MPL-2.0
"""The signed package's declaration (with a throwaway key) and the notices
being current."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MGR_DIR = Path(os.environ.get("NBT_MGR_DIR", ROOT.parent / "manager"))


class NoticeTest(unittest.TestCase):
    def test_notice_is_current(self):
        if not (ROOT.parent / "sdk/tools/third_party_notices.py").exists() and "NBT_SDK_DIR" not in os.environ:
            self.skipTest("no SDK checkout for its notices")
        subprocess.run([sys.executable, str(ROOT / "tools/make_notice.py"), "--check"], check=True)


class PackageTest(unittest.TestCase):
    def test_the_package_declares_its_bridge_and_settings(self):
        verify = MGR_DIR / "build" / "nbt-verify"
        if not (MGR_DIR / "tools/package.py").exists() or not verify.exists():
            self.skipTest("the manager's tools/package.py or build/nbt-verify is missing")
        if not shutil.which("openssl"):
            self.skipTest("openssl is missing")
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            key, public = tmp / "app.key", tmp / "app.pub"
            subprocess.run(["openssl", "genrsa", "-out", key, "3072"], check=True, capture_output=True)
            subprocess.run(["openssl", "rsa", "-in", key, "-pubout", "-out", public], check=True,
                           capture_output=True)
            # A stand-in for the program: an ARM ELF executable header.
            binary = tmp / "stand-in"
            elf = bytearray(52)
            elf[:7] = b"\x7fELF\x01\x01\x01"
            elf[16:20] = b"\x02\x00\x28\x00"
            binary.write_bytes(bytes(elf))
            subprocess.run(["make", "-s", "-C", str(ROOT), "package", f"BUILD_DIR={tmp}",
                            f"PACKAGE_INPUT={binary}", f"APP_KEY={key}", f"NBT_MGR_DIR={MGR_DIR}",
                            "APP_VERSION=0.1.0", "APP_SEQ=1000"], check=True, capture_output=True)
            out = subprocess.run([str(verify), str(tmp / "gauges-0.1.0.nbt"), str(public), "gauges",
                                  "nbt_6.5_armv7"], check=True, capture_output=True, text=True).stdout
            self.assertIn("app=gauges\n", out)
            self.assertIn("bridge=1.6\n", out)
            self.assertIn("bridge-uses=show,controller\n", out)
            self.assertIn("bridge-files=csv|1|4\n", out)
            # The car-data names are those src/sources.cpp reads.
            sources = (ROOT / "src/sources.cpp").read_text()
            line = [l for l in out.splitlines() if l.startswith("bridge-cds=")][0]
            for name in line.split("=", 1)[1].split(","):
                self.assertIn(f'"{name}"', sources)


if __name__ == "__main__":
    unittest.main()
