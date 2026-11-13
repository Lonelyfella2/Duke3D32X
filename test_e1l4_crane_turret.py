#!/usr/bin/env python3
"""E1L4's two real crane pairs and turret hit/death behavior."""
import re, subprocess, tempfile
from pathlib import Path
root = Path(__file__).resolve().parent.parent
setup = '''wait 3
press start 2
wait 3
press right 2
wait 2
press right 2
wait 2
press right 2
wait 2
press start 2
wait 8
god
'''
def run(script):
    with tempfile.TemporaryDirectory() as d:
        p = Path(d)/'script'; p.write_text(setup + script)
        r = subprocess.run([str(root/'build/hostgame'), str(root/'gen/gamedata_le.bin'), str(p), d], capture_output=True, text=True, timeout=60, check=True)
        return r.stdout+r.stderr
for tag, x, y, sector in ((192,26112,32384,107),(193,24067,25363,275)):
    log=run(f'''crane_status
goto {x} {y} 0
wait 3
crane_status
wait 140
crane_status
wait 200
crane_status
''')
    rows = re.findall(fr'crane \d+ hitag {tag} pic (\d+) state (\d+) owner (-?\d+) sect (\d+) xy (-?\d+),(-?\d+) z (-?\d+) target (-?\d+),(-?\d+) sect (\d+)',log)
    assert len(rows)==4 and int(rows[0][0])==1224 and int(rows[0][-1])==sector,log
    assert int(rows[1][1])==1 and int(rows[2][2])==-2 and int(rows[3][2])==-1,log
for weapon in (0,1):
    log=run(f'''turret_damage 10 {weapon}
turret_damage 40 {weapon}
crash_status
''')
    rows=re.findall(r'turret_damage id (\d+) stat (\d+) hp (-?\d+) kills (\d+)', log)
    assert len(rows)==2 and rows[0][1:] == ('1','30','0') and rows[1][1]=='32' and rows[1][3]=='1',log
    assert re.search(r'explosions 1 scrap 19 kills 1',log),log
print('Both E1L4 cranes carry and release Duke; bullets and RPG destroy turrets with explosion and scrap.')
