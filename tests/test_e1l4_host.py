#!/usr/bin/env python3
"""E1L4 integration smoke test; run `make hostgame` first."""
import re
import subprocess
import tempfile
from pathlib import Path
root = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as out:
    run = subprocess.run([str(root/'build/hostgame'), str(root/'gen/gamedata_le.bin'),
                          str(root/'tests/host/host_e1l4.txt'), out],
                         capture_output=True, text=True, timeout=60, check=True)
log = run.stdout + run.stderr
assert re.search(r'stat start:.*lvl 3\b', log), log
scuba = {name: tuple(map(int, values)) for name, *values in re.findall(
    r'scuba_status (\w+): amount (\d+) active (\d+) icon (\d+) level (\d+) hp (\d+)', log)}
assert scuba['start'][:4] == (6400, 0, 5, 3), log
assert re.search(r'shotgun_status start: owned 1 ammo 10 equipped 2 level 3', log), log
assert re.search(r'shotgun_status underwater: owned 1 ammo 9 equipped 2 level 3', log), log
assert 0 < scuba['dived'][0] < 6400 and scuba['dived'][1] == 1, log
assert scuba['surfaced'][1] == 0 and scuba['surfaced'][0] <= scuba['dived'][0], log
floors = re.findall(r'sect 512: floorz (-?\d+)', log)
assert len(floors) == 2 and floors[0] != floors[1], log
assert re.search(r'pstat dived:.*sect 27\b', log), log
assert re.search(r'pstat surfaced:.*sect 83\b', log), log
subways = re.findall(r'subway sector 66 x (-?\d+) y (-?\d+) angle (\d+) velocity (\d+) target (-?\d+) state (\d+)', log)
assert len(subways) == 4 and int(subways[2][0]) < int(subways[1][0]) and int(subways[3][5]) == 0, log
assert re.search(r'stat next_level:.*lvl 4\b', log), log
print('E1L4 host smoke test: full scuba tank, underwater use, wave floor, dive/surface, subway movement and forced progression passed.')
