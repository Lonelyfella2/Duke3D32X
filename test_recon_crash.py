#!/usr/bin/env python3
"""Exercise lethal bullet and RPG damage through the RECON crash lifecycle."""
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
test_recon 1200 1200 -14000
crash_status
'''
for explosive in (0, 1):
    script = setup + f'''recon_damage {explosive}
crash_status
wait 8
crash_status
wait 16
crash_status
wait 100
crash_status
'''
    with tempfile.TemporaryDirectory() as d:
        p = Path(d)/'script.txt'; p.write_text(script)
        r = subprocess.run([str(root/'build/hostgame'), str(root/'gen/gamedata_le.bin'), str(p), d], capture_output=True, text=True, timeout=60, check=True)
    log = r.stdout + r.stderr
    rows = [list(map(int, x)) for x in re.findall(r'crash_status: crashes (\d+) z (-?\d+) pigs (\d+) explosions (\d+) scrap (\d+) kills (\d+) max (\d+)', log)]
    assert len(rows) == 5, log
    before, start, falling, impact, later = rows
    assert start[0] == 1 and start[5] == before[5], log
    assert falling[0] == 1 and falling[1] > start[1] and falling[3] > 0, log
    assert impact[0] == 0 and impact[2] == before[2] + 1 and impact[3] > 0 and impact[4] == 16, log
    assert impact[5] == before[5] + 1 and impact[6] == before[6] + 1, log
    assert later[5] == impact[5], log
print('RECON crash: bullets and RPG both spiral down, explode, eject a live Pig Cop, and count one kill.')
