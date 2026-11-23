#!/usr/bin/env python3
"""Regenerate EVDSTRCTRL_GZ.h from EVDSTRCTRL.h.

Run this after ANY edit to EVDSTRCTRL.h. If you forget, the build still succeeds
and the OLD dashboard is served with no warning; boot_serial() logs the gzip
size so the two can be compared.

    python gen_gz.py
"""
import gzip
import pathlib
import re

HERE = pathlib.Path(__file__).resolve().parent
SRC = HERE / "EVDSTRCTRL.h"
DST = HERE / "EVDSTRCTRL_GZ.h"

text = SRC.read_text(encoding="utf-8")
m = re.search(r'R"HTML\((.*)\)HTML"', text, re.S)
if not m:
    raise SystemExit("EVDSTRCTRL.h: R\"HTML( ... )HTML\" block not found")
html = m.group(1).encode("utf-8")
gz = gzip.compress(html, 9, mtime=0)

out = [
    "#pragma once",
    "// GENERATED FILE - do not hand-edit. Source: EVDSTRCTRL.h",
    "// Regenerate with: python gen_gz.py",
    f"// raw {len(html)} bytes -> gzip {len(gz)} bytes",
    "static const unsigned char EVDSTRCTRL_GZ[] PROGMEM = {",
]
for i in range(0, len(gz), 16):
    out.append("  " + ",".join(f"0x{b:02x}" for b in gz[i:i + 16]) + ",")
out.append("};")
out.append(f"static const unsigned int EVDSTRCTRL_GZ_LEN = {len(gz)};")
DST.write_text("\n".join(out) + "\n", encoding="utf-8")
print(f"EVDSTRCTRL.h {len(html)} B -> EVDSTRCTRL_GZ.h {len(gz)} B ({DST.name})")
