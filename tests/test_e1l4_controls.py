#!/usr/bin/env python3
"""Exercise E1L4's submerged entrance button and nearby monitor artwork."""
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
        p = Path(d)/'script.txt'; p.write_text(setup + script)
        r = subprocess.run([str(root/'build/hostgame'), str(root/'gen/gamedata_le.bin'), str(p), d], capture_output=True, text=True, timeout=60, check=True)
    return r.stdout + r.stderr

for angle in (384, 448, 512, 576, 640):
    log = run(f'''goto -19648 22900 512
wait 2
sprite_status 740
sect 240
setangle {angle}
press c 2
wait 10
sprite_status 740
sect 240
# Stage Duke at the upper switch's swimming height, then press use again.
test_player_z -20000
setangle 512
wait 1
press c 2
wait 10
sprite_status 742
sect 240
''')
    pics = re.findall(r'sprite_status 740: pic (\d+)', log)
    ceilings = [int(x) for x in re.findall(r'sect 240: floorz -?\d+ ceilingz (-?\d+)', log)]
    assert pics == ['162', '163'], log
    assert len(ceilings) == 3 and ceilings[1] == ceilings[0] and ceilings[2] < ceilings[1], log
    assert re.search(r'sprite_status 742: pic 163\b', log), log
for x, y, angle in ((-19648, 22900, 1024), (-19648, 22000, 512)):
    log = run(f'''goto {x} {y} {angle}
wait 2
sprite_status 740
press c 2
wait 2
sprite_status 740
''')
    assert re.findall(r'sprite_status 740: pic (\d+)', log) == ['162', '162'], log
log = run('''wall_status 395
wall_status 401
wait 400
wall_status 395
wall_status 401
''')
for wall in (395, 401):
    assert re.findall(fr'wall_status {wall}: pic (\d+)', log) == ['359', '359'], log
print('E1L4 switch responds at five angles and its mechanism waits for the correct combination; monitor tiles stay stable.')
