#!/usr/bin/env python3
"""
run_tests.py - point-to-point PicoDrive tests for the Duke Nukem 3D 32X ROM.

Builds tests/harness.c, boots rom/duke3d32x.32x in the headless PicoDrive
libretro core with each tests/scripts/NN_*.txt input script and asserts:
  * every captured frame is lit and colourful (black-screen guard)
  * telemetry (COMM12 mirrored by the 68000) walks 3D Realms (0x0800) -> title (0x1000) ->
    menu(0x2000) -> gameplay(0x3xxx) as the script drives it
  * gameplay frames change while moving (the game is live)
  * the PWM mixer produces real audio when the pistol fires
  * in-game frame rate stays above a floor
"""
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ROM = os.path.join(ROOT, "rom", "duke3d32x.32x")
CORE = os.path.join(HERE, "emu", "picodrive_libretro.so")
OUT = os.path.join(ROOT, "out", "tests")
HARNESS = os.path.join(ROOT, "build", "harness")


def read_ppm(path):
    d = open(path, "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def frame_stats(path):
    w, h, px = read_ppm(path)
    total = w * h
    lit, colors = 0, set()
    for i in range(0, len(px), 3 * 7):
        r, g, b = px[i], px[i + 1], px[i + 2]
        if r > 8 or g > 8 or b > 8:
            lit += 1
        colors.add((r >> 3, g >> 3, b >> 3))
    return lit / (total / 7), len(colors), px


def run(script):
    os.makedirs(OUT, exist_ok=True)
    r = subprocess.run([HARNESS, CORE, ROM, os.path.join(HERE, "scripts", script), OUT],
                       capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        raise SystemExit(f"{script}: harness failed\n{r.stderr}")
    return r.stdout


def tele(out):
    return [(int(a, 16), int(b, 16), int(v)) for a, b, v in
            re.findall(r"comm12=([0-9a-f]+) comm14=([0-9a-f]+) vbl=(\d+)", out)]


def check_shots(names, min_lit=0.08, min_colors=8):
    pix = {}
    for n in names:
        lit, colors, px = frame_stats(os.path.join(OUT, n + ".ppm"))
        if lit < min_lit or colors < min_colors:
            raise SystemExit(f"FAIL {n}: black/blank frame (lit {lit:.2f}, {colors} colours)")
        pix[n] = px
    return pix


def main():
    if not os.path.exists(CORE):
        raise SystemExit("PicoDrive core missing - run ./setup.sh")
    subprocess.check_call(["cc", "-O2", "-std=c11", "-o", HARNESS, os.path.join(HERE, "harness.c"), "-ldl"])
    ok = 0
    # 01 boot -> 3D Realms screen -> title on its own, waiting for START
    t = tele(run("01_boot.txt"))
    check_shots(["realms", "title"])
    assert t and t[0][0] == 0x0800, f"boot: expected the 3D Realms screen (0x0800) first, got {t}"
    assert t[-1][0] == 0x1000, f"boot: expected title telemetry 0x1000, got {t}"
    print("PASS 01_boot: 3D Realms screen, then the title by itself (telemetry 0x0800 -> 0x1000)"); ok += 1
    # 02 menu
    t = tele(run("02_menu.txt"))
    check_shots(["menu", "menu_item2"])
    assert t[0][0] == 0x2000 and t[-1][1] == 2, f"menu: expected 0x2000 and cursor move, got {t}"
    print("PASS 02_menu: skill menu responds to the pad"); ok += 1
    # 03 gameplay
    out = run("03_gameplay.txt")
    t = tele(out)
    pix = check_shots(["play_a", "play_b", "play_c", "play_d"])
    assert all((x[0] & 0xF000) == 0x3000 for x in t), f"gameplay telemetry: {t}"
    diff = sum(1 for i in range(0, len(pix["play_a"]), 97) if pix["play_a"][i] != pix["play_b"][i])
    assert diff > 200, f"gameplay frames did not change while moving ({diff})"
    print(f"PASS 03_gameplay: E1L1 live, health {t[-1][0] & 0xff}, view changes ({diff} samples)"); ok += 1
    # 04 audio (PWM mixer on the slave SH-2)
    out = run("04_audio.txt")
    m = re.search(r"wav pistol: \d+ frames, peak (\d+), mean ([\d.]+)", out)
    assert m and int(m.group(1)) > 2000 and float(m.group(2)) > 200, f"no PWM audio: {out}"
    print(f"PASS 04_audio: PWM mixer output peak {m.group(1)}, mean {m.group(2)}"); ok += 1
    # 05 frame rate
    t = tele(run("05_fps.txt"))
    fps = [60 * (b[1] - a[1]) / (b[2] - a[2]) for a, b in zip(t, t[1:]) if b[2] > a[2]]
    assert fps and min(fps) >= 8, f"frame rate too low: {fps}"
    print("PASS 05_fps: in-game " + ", ".join(f"{f:.1f}" for f in fps) + " fps (PicoDrive)"); ok += 1
    # 06 every later level boots and plays on the 32X build (skill-menu start level)
    names = {1: "E1L2", 2: "E1L3", 3: "E1L4", 4: "E1L5", 5: "E3L2", 6: "E3L5", 7: "E3L9"}
    report = []
    for idx, name in names.items():
        script = os.path.join(OUT, f"lvl_{name}.txt")
        with open(script, "w") as f:
            f.write("run 240\npress start 8\nrun 60\npress start 8\nrun 60\n" + "press right 6\nrun 6\n" * idx +
                    "press start 8\nrun 420\nshot " + name + "_a\ntele\n"
                    "hold up\nrun 60\nrelease up\nhold left\nrun 40\nrelease left\nshot " + name + "_b\ntele\n")
        r = subprocess.run([HARNESS, CORE, ROM, script, OUT], capture_output=True, text=True, timeout=600)
        t = tele(r.stdout)
        pix = check_shots([name + "_a", name + "_b"])
        assert t and all((x[0] & 0xFF00) == (0x3000 | (idx << 8)) for x in t), f"{name}: telemetry {t}"
        assert all((x[0] & 0xff) > 0 for x in t), f"{name}: player died {t}"
        diff = sum(1 for i in range(0, len(pix[name + "_a"]), 97) if pix[name + "_a"][i] != pix[name + "_b"][i])
        assert diff > 200, f"{name}: view did not change ({diff})"
        report.append(f"{name} hp {t[-1][0] & 0xff}")
    print("PASS 06_levels: " + ", ".join(report) + " (32X build, start level from the menu)"); ok += 1
    print(f"all {ok} tests passed")


if __name__ == "__main__":
    main()
