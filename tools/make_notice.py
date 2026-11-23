#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Writes legal/NOTICE.txt: the notices this application's package carries.

Ours (MPL-2.0); LVGL (MIT) with the parts the package contains: Tiny TTF
(stb_truetype, MIT), Montserrat (SIL OFL 1.1, the TrueType font and LVGL's
built-in 14 px font) and Font Awesome (the built-in font's symbols); then the
NBT App SDK's third-party notices (NBT_SDK_DIR, default ../sdk). Every text is
read from the pinned sources. Run again when a dependency changes; `make
test` fails when the file is out of date (tests/test_package.py).
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get("NBT_SDK_DIR", ROOT.parent / "sdk"))
LVGL = ROOT / "third_party/lvgl"
FONTS = LVGL / "scripts/generators/built_in_font/font_license"
RULE = "\n" + "=" * 72 + "\n\n"

HEADER = """NBTFoundry

NBTFoundry nf-gauges
Copyright (c) 2026 Fabien Poussin
Original software: MPL-2.0. You may redistribute and modify it under that license.
This software comes WITHOUT ANY WARRANTY, to the extent permitted by law.
Original NBTFoundry artwork: MPL-2.0. Trademark rights are separate.
Source: https://github.com/NBTFoundry/nf-gauges
When redistributing binaries, include applicable notices and provide the source required by the component licenses.
"""


def lvgl_version():
    text = (LVGL / "include/lvgl/lv_version.h").read_text() if (LVGL / "include/lvgl/lv_version.h").exists() \
        else (LVGL / "lv_version.h").read_text()
    parts = {}
    for key in ("MAJOR", "MINOR", "PATCH"):
        for line in text.splitlines():
            if line.startswith(f"#define LVGL_VERSION_{key} "):
                parts[key] = line.split()[2]
    return "{MAJOR}.{MINOR}.{PATCH}".format(**parts) if len(parts) == 3 else "unknown"


def main():
    sdk = subprocess.run([sys.executable, str(SDK / "tools/third_party_notices.py")], check=True,
                         capture_output=True, text=True).stdout
    parts = [
        HEADER,
        "Head-unit risk and disclaimer\n\n" + (ROOT / "DISCLAIMER.md").read_text().split("\n", 1)[1],
        "MPL-2.0\n\n" + (ROOT / "LICENSES/MPL-2.0.txt").read_text(),
        f"LVGL {lvgl_version()} (third_party/lvgl)\nhttps://github.com/lvgl/lvgl\n\n"
        + (LVGL / "LICENCE.txt").read_text(),
        "Tiny TTF and stb_truetype, part of LVGL (third_party/lvgl/src/libs/tiny_ttf)\n\n"
        + (LVGL / "src/libs/tiny_ttf/LICENSE.txt").read_text(),
        "Montserrat Medium, the text font (LVGL's copy and its built-in 14 px font)\n\n"
        + (FONTS / "Montserrat/OFL.txt").read_text(),
        "Font Awesome, the symbols of LVGL's built-in 14 px font\n\n"
        + (FONTS / "FontAwesome5/LICENSE.txt").read_text(),
        sdk,
    ]
    text = "\n\n" + RULE.join(p.rstrip("\n") + "\n" for p in parts)
    out = ROOT / "legal/NOTICE.txt"
    if "--check" in sys.argv:
        if not out.exists() or out.read_text() != text:
            sys.exit("legal/NOTICE.txt is out of date: run make notice")
        return
    out.parent.mkdir(exist_ok=True)
    out.write_text(text)
    print("legal/NOTICE.txt: %d sections" % len(parts))


if __name__ == "__main__":
    main()
