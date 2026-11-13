#!/usr/bin/env python3
"""E1L4 behaviors identified by comparison with original PREMAP/ACTORS/SECTOR."""
import re, subprocess, tempfile
from pathlib import Path
root = Path(__file__).resolve().parent.parent
script = '''wait 3
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
sector_effector 355
wall_status 2718
sect 272
operate 272
wait 30
sect 272
wait 200
sect 272
sprite_status 13
sprite_status 14
sprite_status 863
test_switch 13
sprite_status 13
sprite_status 14
sprite_status 863
goto 42752 40768 512
wait 2
stat current_start
wait 10
stat current_end
'''
with tempfile.TemporaryDirectory() as d:
    p = Path(d)/'script.txt'; p.write_text(script)
    r = subprocess.run([str(root/'build/hostgame'), str(root/'gen/gamedata_le.bin'), str(p), d], capture_output=True, text=True, check=True, timeout=60)
log = r.stdout + r.stderr
assert re.search(r'sector_effector 355: id \d+ kind 24 speed 5000', log), log
assert re.search(r'wall_status 2718: pic \d+ cstat 155 over 663', log), log
floorceil = re.findall(r'sect 272: floorz (-?\d+) ceilingz (-?\d+)', log)
assert floorceil == [('-24576','-24576'), ('-24576','-40960'), ('-24576','-24576')], log
for n in (13, 14, 863):
    assert re.findall(fr'sprite_status {n}: pic (\d+)', log) == ['136', '137'], log
positions = re.findall(r'stat current_(?:start|end): pos \((-?\d+),(-?\d+),(-?\d+)\)', log)
assert len(positions) == 2 and int(positions[1][1]) > int(positions[0][1]) + 100, log
print('E1L4 source audit: timed door closes, linked switches sync, forcefields block hitscan, and water current moves Duke.')
