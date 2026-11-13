#!/usr/bin/env python3
"""The E1L2 opening elevator must reach the exit in one automatic trip."""
import re, subprocess, tempfile
from pathlib import Path
root = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as d:
    r = subprocess.run([str(root/'build/hostgame'), str(root/'gen/gamedata_le.bin'), str(root/'tests/host/host_e1l2_elevator.txt'), d], capture_output=True, text=True, timeout=60, check=True)
log = r.stdout + r.stderr
floors = list(map(int, re.findall(r'sect 201: floorz (-?\d+)', log)))
assert floors == [-57344, 8192, 8192], log
states = {name: int(sect) for name, sect in re.findall(r'stat (\w+): pos \([^)]*\) ang \d+ sect (\d+)', log)}
assert states['start'] == states['arrived'] == states['stable'] == 201, log
assert states['exit'] != 201, log
print('E1L2 opening elevator reaches landing and Duke can walk out without pressing use.')
