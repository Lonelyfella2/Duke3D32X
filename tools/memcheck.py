#!/usr/bin/env python3
"""Fail the build if SDRAM usage (.data+.bss) collides with the stacks."""
import re
import sys

text = open(sys.argv[1]).read()
def sym(name):
    m = re.search(r"0x([0-9a-fA-F]+)\s+" + re.escape(name) + r"\b", text)
    return int(m.group(1), 16) if m else None

bss_end = sym("__bss_end")
data_size = sym("__data_size")
text_end = sym("__text_end")
LIMIT = 0x0603C000            # leave >= 12 KiB of master stack below 0x0603F000
used = bss_end - 0x06000000
print(f"[memcheck] ROM text+rodata {(text_end - 0x02000000) / 1024:.0f} KiB, "
      f"SDRAM used {used / 1024:.1f} KiB of 240 KiB (data {data_size / 1024:.1f} KiB), "
      f"master stack headroom {(0x0603F000 - bss_end) / 1024:.1f} KiB")
if bss_end >= LIMIT:
    print(f"[memcheck] ERROR: __bss_end 0x{bss_end:08x} >= 0x{LIMIT:08x}")
    sys.exit(1)
