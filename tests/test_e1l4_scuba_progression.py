#!/usr/bin/env python3
"""The E1L4 tank is granted during campaign progression too."""
import re, subprocess, tempfile
from pathlib import Path
root = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as d:
    run = subprocess.run([str(root/'build/hostgame'), str(root/'gen/gamedata_le.bin'), str(root/'tests/host/host_e1l4_scuba_progression.txt'), d], capture_output=True, text=True, check=True, timeout=60)
log = run.stdout + run.stderr
rows = {n: tuple(map(int, v)) for n, *v in re.findall(r'scuba_status (\w+): amount (\d+) active (\d+) icon (\d+) level (\d+) hp (\d+)', log)}
assert rows['before'][0] == 0 and rows['before'][3] == 2, log
assert rows['after'][:4] == (6400, 0, 5, 3), log
assert re.search(r'shotgun_status before: owned 0 ammo 0 equipped 1 level 2', log), log
assert re.search(r'shotgun_status after: owned 1 ammo 10 equipped 2 level 3', log), log
print('Campaign progression from E1L3 grants a full scuba tank on E1L4 entry.')
