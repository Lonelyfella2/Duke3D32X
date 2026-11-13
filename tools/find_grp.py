#!/usr/bin/env python3
"""Locate the best DUKE3D.GRP: prefer a registered/Atomic GRP (has E3L2.MAP)
anywhere under /home/user (any capitalisation), else the shareware one."""
import os, struct, sys

def grp_info(path):
    try:
        with open(path, "rb") as f:
            if f.read(12) != b"KenSilverman":
                return None
            n = struct.unpack("<i", f.read(4))[0]
            names = [f.read(16)[:12].split(b"\0")[0].decode("ascii", "replace").upper() for _ in range(n)]
        return names
    except OSError:
        return None

best, best_score = None, -1
for root, dirs, files in os.walk("/home/user"):
    dirs[:] = [d for d in dirs if d not in ("node_modules", ".git", "build")]
    for fn in files:
        if fn.lower().endswith(".grp"):
            p = os.path.join(root, fn)
            names = grp_info(p)
            if not names:
                continue
            score = 2 if "E3L2.MAP" in names else 1
            if score > best_score:
                best, best_score = p, score
print(best or "")
