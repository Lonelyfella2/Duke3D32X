#!/usr/bin/env python3
"""Stage a real E1L2 RECON and verify auto-aim and damage on the host.

Run `make hostgame && python3 tests/test_flying_aim.py`.
"""
import re, subprocess, tempfile
from pathlib import Path
root = Path(__file__).resolve().parent.parent
setup = '''wait 3
press start 2
wait 3
press right 2
wait 3
press start 2
wait 8
god
goto -4000 16500 256
give
'''
def run(weapon, dx, dy, dz, away=False):
    script = setup + f'weapon {weapon}\ntest_recon {dx} {dy} {dz}\n'
    if away: script += 'setangle 1280\n'
    script += 'recon_status\npress b 2\nwait ' + ('15' if weapon == 4 else '1') + '\nrecon_status\n'
    with tempfile.TemporaryDirectory() as d:
        p = Path(d)/'script.txt'; p.write_text(script)
        r = subprocess.run([str(root/'build/hostgame'), str(root/'gen/gamedata_le.bin'), str(p), d],
                           capture_output=True, text=True, timeout=60, check=True)
    log = r.stdout + r.stderr
    m = re.findall(r'recon_status: id (\d+) hp (-?\d+) z (-?\d+) target (-?\d+) state (\d+)', log)
    assert len(m) == 2, log
    return [list(map(int, row)) for row in m], log

for weapon in (1, 2, 3, 4):
    for dx, dy, dz in ((1200, 1200, -14000), (1700, 0, -12000)):
        r, log = run(weapon, dx, dy, dz)
        assert r[0][3] == r[0][0], log
        assert r[1][1] < r[0][1], log
r, log = run(1, 1200, 1200, -14000, away=True)
assert r[0][3] != r[0][0] and r[1][1] == r[0][1], log
print('Flying Pig Cop aim: pistol, shotgun, chaingun and RPG hit elevated diagonal and straight targets; off-axis target not auto-aimed.')
