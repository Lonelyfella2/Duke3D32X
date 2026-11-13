#!/usr/bin/env python3
"""
convert.py - turn DUKE3D.GRP into the Duke Nukem 3D 32X data blob.

  * Palette -> 32X CRAM (RGB555 + priority bit), 32-level shade tables and
    the LOOKUP.DAT palette swaps used by the selected maps.
  * ART tiles -> column-major 8bpp, downscaled 50% (everything except HUD /
    fonts / title art) with a transparency-aware 2x2 box filter.
  * MAP files -> compact runtime structs (sector 40B, wall 24B + 8B ROM-only,
    sprite 44B), LZ4-compressed; multiplayer-only sprites are pruned.
  * VOC/WAV -> IMA ADPCM at native rate (22 kHz sources halved), selected by
    priority tier until the sound budget is used.
  * USER.CON/DEFS.CON -> generated C header with the original game constants
    (weapon strengths, enemy health, ammo limits, sound ids, quotes).

Usage: convert.py --grp DUKE3D.GRP [--grp2 extra.grp] --out build/
"""
import argparse
import glob
import os
import re
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from grp import Grp                       # noqa: E402
from art import load_tiles, load_palette, load_lookups   # noqa: E402
from buildmap import load_map             # noqa: E402
from lz4enc import lz4_compress, lz4_decompress   # noqa: E402
from audio import load_sound, resample, ima_encode   # noqa: E402

# ---------------------------------------------------------------- config ---
KEEP_LEVELS = [  # (map, episode, level) - order = the single continuous campaign
    ("E1L1", 1, 1), ("E1L2", 1, 2), ("E1L3", 1, 3), ("E1L4", 1, 4), ("E1L5", 1, 5),
    ("E3L2", 3, 2), ("E3L5", 3, 5), ("E3L9", 3, 9),
]
MAXTILES = 6144
SOUND_BUDGET = 1100 * 1024          # bytes of ADPCM
TILE_BUDGET = 2300 * 1024           # bytes of tile pixels

# Tiles drawn 1:1 at native resolution (status bar, menu cursor, small
# status-bar fonts) keep full resolution.  Screens drawn into the 160x96 view
# (MENUSCREEN, DUKENUKEM) only ever show every other pixel, so they are
# stored at half resolution like the world art.
FULLRES_RANGES = [
    (2536, 2542),      # CHAINGUN: native-res body, barrels and muzzle flashes
    (2462, 2462),      # BOTTOMSTATUSBAR
    (2472, 2481),      # DIGITALNUM
    (2460, 2461), (2463, 2463), (2467, 2471),   # inventory icons, ACCESS_ICON
    (2813, 2821),      # SPINNINGNUKEICON (pause-menu cursor)
    (3010, 3021),      # THREEBYFIVE digits + ':' '/' (status-bar weapon list)
    (3072, 3135),      # MINIFONT (status-bar "%", ON/OFF/AUTO)
    (1, 10),           # editor markers (tiny)
]

# Gameplay tile families always worth carrying (when present in the GRP);
# 2D art only where the code draws it (fonts, help and menu screens the port
# never shows are left out - map sprites still bring in any tile they use).
ALWAYS_RANGES = [
    (20, 110),         # weapons/ammo/items/skies
    (1222, 1225),      # CRANE frames selected by its runtime state
    (1300, 1420),      # misc effects, holoduke, cool explosion, player
    (1405, 1480),      # APLAYER (holoduke + death)
    (1600, 1680),      # blood, firelaser, spit, freeze/shrink sparks, TRANSPORTERSTAR
    (1890, 1910),      # EXPLOSION2
    (2245, 2455),      # jibs, smoke, debris, scrap, bloodpools, water splash, GROWSPARK
    (2456, 2456),      # MENUSCREEN
    (2460, 2481),      # status bar, inventory icons, DIGITALNUM
    (2497, 2497),      # DUKENUKEM (campaign complete)
    (2510, 2629),      # first-person weapons, projectiles, sparks
    (2813, 2821),      # SPINNINGNUKEICON
    (3010, 3021),      # THREEBYFIVE
    (3072, 3135),      # MINIFONT
    (950, 960),        # bullet holes / decals
    (1031, 1034),      # glass pieces
    (3500, 3520),      # GROWSPARK etc (Atomic)
]
ACTOR_FAMILIES = {     # base tile -> (lo, hi) art range
    1680: (1680, 1819),   # LIZTROOP family
    1820: (1820, 1859),   # OCTABRAIN
    1860: (1860, 1879),   # INNERJAW
    1880: (1880, 1889),   # DRONE
    1920: (1920, 1959),   # COMMANDER
    1960: (1960, 1974),   # RECON
    1975: (1975, 1999),   # TANK
    2000: (2000, 2119),   # PIGCOP
    2120: (2120, 2244),   # LIZMAN
    2360: (2360, 2369),   # ROTATEGUN
    2370: (2370, 2399),   # GREENSLIME
    2630: (2630, 2709),   # BOSS1
    2710: (2710, 2759),   # BOSS2
    2760: (2760, 2812),   # BOSS3
    1550: (1550, 1559),   # SHARK
    4610: (4610, 4739),   # NEWBEAST
    4740: (4740, 4800),   # BOSS4
}
# spawned at runtime by other actors / events even when absent from a map
ACTOR_ALWAYS = [1680, 2000, 2120, 1820]
# 320x200 / logo art the port never draws: the title screen is the picture in
# assets/title.png (TITLE, DREALMS, BETASCREEN, THREEDEE, PLUTOPAK); LOADSCREEN
# (3281) is dropped too when assets/transition.png replaces it
EXCLUDE_TILES = {2486, 2492, 2493, 2498, 2499}
LOADSCREEN = 3281
# colours of the in-game text styles (src/game/hud_text.c style_rgb: shadow,
# body, highlight of yellow, blue, red, grey, green) - added to the palette of
# pictures that carry font.png text so the text keeps its exact colours
# tints the Tormentstein font is drawn with on pictures (src/game/hud_text.c
# tint_rgb: TF_WHITE, TF_YELLOW) - the transition picture's palette gets the
# font's colours in each, so ENTERING / LEVEL COMPLETED keep their shading
TFONT_TINTS = [(255, 255, 255), (255, 214, 64)]


def tinted_colours(font_path, tints):
    cols = font_colours(font_path)
    return [(r * t[0] // 255, g * t[1] // 255, b * t[2] // 255) for t in tints for r, g, b in cols]


TEXT_STYLE_RGB = [
    (176, 72, 0), (252, 212, 32), (255, 255, 255),
    (32, 32, 66), (66, 82, 138), (95, 205, 228),
    (96, 0, 0), (216, 32, 16), (255, 200, 96),
    (56, 56, 56), (168, 168, 168),
    (0, 64, 0), (40, 180, 40), (200, 255, 160),
]
ASSET_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets")

SOUND_EXCLUDE = {"BONUSMUSIC", "BAR_MUSIC", "WIND_REPEAT", "DUKETALKTOBOSS", "BOSSTALKTODUKE", "RIPHEADNECK"}
# enemy sound families: only carried when that enemy appears in a kept level
SOUND_ACTOR_PREFIX = {
    "PRED_": 1680, "PIG_": 2000, "CAPT_": 2120, "OCTA_": 1820, "COMM_": 1920, "RECO_": 1960,
    "DRON_": 1880, "TURR_": 2360, "SLIM_": 2370, "BOS1_": 2630, "BOS2_": 2710, "BOS3_": 2760,
    "BOS4_": 4740, "NEWBEAST_": 4610,
}
SRC_GAME_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "game")
SOUND_MAX_BUDGET = 1400 * 1024      # never spend more than this on samples
SOUND_TIER2_PREFIX = ("GENERIC_AMBIENCE", "WAR_AMBIENCE", "ENDSEQ", "SPACE_AMBIENCE",
                      "COMPUTER_AMBIENCE", "BUBBLE_AMBIENCE", "MACHINE_AMBIENCE",
                      "SEWER_AMBIENCE", "WIND_AMBIENCE", "FIRE_CRACKLE")


# ------------------------------------------------------------- helpers ---
def align(buf, n=4):
    while len(buf) % n:
        buf.append(0)


def log2ceil(v):
    n = 0
    while (1 << n) < v:
        n += 1
    return n


def in_ranges(t, ranges):
    return any(a <= t <= b for a, b in ranges)


class Blob:
    """Endian-aware growing byte buffer."""

    def __init__(self, endian):
        self.e = endian
        self.buf = bytearray()

    def here(self):
        return len(self.buf)

    def align(self, n=4):
        align(self.buf, n)

    def pack(self, fmt, *vals):
        self.buf += struct.pack(self.e + fmt, *vals)

    def raw(self, b):
        self.buf += b

    def patch(self, off, fmt, *vals):
        struct.pack_into(self.e + fmt, self.buf, off, *vals)


# ------------------------------------------------------------ CON parse ---
def parse_con(grp):
    text = {}
    for name in ("DEFS.CON", "USER.CON", "GAME.CON"):
        if grp.has(name):
            text[name] = grp.read(name).decode("latin1")
    defines = {}
    for name in ("DEFS.CON", "USER.CON", "GAME.CON"):
        for line in text.get(name, "").splitlines():
            m = re.match(r"\s*define\s+([A-Za-z_]\w*)\s+(-?\d+)", line)
            if m and m.group(1) not in defines:
                defines[m.group(1)] = int(m.group(2))
    sounds = []
    quotes = {}
    levelnames = {}
    music = {}
    for name in ("USER.CON", "GAME.CON"):
        for line in text.get(name, "").splitlines():
            raw = line
            line = line.split("//")[0]
            p = line.split()
            if not p:
                continue
            kw = p[0].lower()
            if kw == "definesound" and len(p) >= 8:
                sid = defines.get(p[1])
                if sid is None:
                    try:
                        sid = int(p[1])
                    except ValueError:
                        continue
                sounds.append(dict(id=sid, name=p[1], file=p[2].upper(), plo=int(p[3]),
                                   phi=int(p[4]), pri=int(p[5]), type=int(p[6]), vol=int(p[7])))
            elif kw == "definequote" and len(p) >= 2:
                q = int(p[1])
                body = raw.split(None, 2)[2] if len(raw.split(None, 2)) > 2 else ""
                quotes[q] = body.strip()[:60]
            elif kw == "definelevelname" and len(p) >= 6:
                ep, lv = int(p[1]), int(p[2])
                title = " ".join(raw.split()[6:]).strip()
                levelnames[(ep + 1, lv + 1)] = (p[3].upper(), p[4], title)
            elif kw == "music" and len(p) >= 2:
                vol = int(p[1])
                music[vol] = [x.upper() for x in p[2:]]
    return defines, sounds, quotes, levelnames, music


# ------------------------------------------------------------ palette ---
def c6to5(c):
    return (c * 31 + 31) // 63


def build_palette_cram(pal):
    out = []
    for r, g, b in pal:
        out.append(0x8000 | (c6to5(b) << 10) | (c6to5(g) << 5) | c6to5(r))
    return out


def nearest_lut(pal):
    """6-bit RGB -> nearest palette index (excluding 255 = transparent)."""
    p = np.array(pal[:255], dtype=np.int32)
    lut = np.zeros(64 * 64 * 64, dtype=np.uint8)
    grid = np.arange(64 * 64 * 64, dtype=np.int32)
    r, g, b = grid >> 12, (grid >> 6) & 63, grid & 63
    step = 8192
    for i in range(0, len(grid), step):
        rr, gg, bb = r[i:i + step, None], g[i:i + step, None], b[i:i + step, None]
        d = (rr - p[None, :, 0]) ** 2 * 3 + (gg - p[None, :, 1]) ** 2 * 4 + (bb - p[None, :, 2]) ** 2 * 2
        lut[i:i + step] = d.argmin(axis=1)
    return lut


# -------------------------------------------------------------- tiles ---
def downscale_tile(t, pal_np, lut):
    w, h = t.w, t.h
    a = np.frombuffer(t.data, dtype=np.uint8).reshape(w, h)
    nw, nh = (w + 1) // 2, (h + 1) // 2
    pa = np.empty((nw * 2, nh * 2), dtype=np.uint8)
    pa[:w, :h] = a
    if w & 1:
        pa[w, :h] = a[w - 1]
    if h & 1:
        pa[:, h] = pa[:, h - 1]
    blocks = pa.reshape(nw, 2, nh, 2).transpose(0, 2, 1, 3).reshape(nw, nh, 4)
    trans = blocks == 255
    ntrans = trans.sum(axis=2)
    rgb = pal_np[blocks].astype(np.int32)                 # nw,nh,4,3
    op = (~trans)[..., None]
    cnt = np.maximum(op.sum(axis=2), 1)
    avg = (rgb * op).sum(axis=2) // cnt                   # nw,nh,3
    idx = lut[(avg[..., 0] << 12) | (avg[..., 1] << 6) | avg[..., 2]]
    out = np.where(ntrans >= 2, 255, idx).astype(np.uint8)
    return nw, nh, out.tobytes()                           # column-major (x outer)


def select_tiles(tiles, maps):
    want = set()
    actors_present = set(ACTOR_ALWAYS)
    for mp in maps:
        for w in mp.walls:
            want.add(w.picnum)
            want.add(w.overpicnum)
        for s in mp.sectors:
            want.add(s.ceilingpicnum)
            want.add(s.floorpicnum)
        for s in mp.sprites:
            want.add(s.picnum)
            for base, (lo, hi) in ACTOR_FAMILIES.items():
                if lo <= s.picnum <= hi:
                    actors_present.add(base)
    for lo, hi in ALWAYS_RANGES + FULLRES_RANGES:
        want.update(range(lo, hi + 1))
    for base in actors_present:
        lo, hi = ACTOR_FAMILIES[base]
        want.update(range(lo, hi + 1))
    # parallax sky neighbours (Duke uses 4-8 consecutive tiles per sky)
    for p in list(want):
        if p in (78, 79, 80, 84, 89):
            want.update(range(p, p + 8))
    # animation frames
    for p in list(want):
        t = tiles.get(p)
        if t and t.w:
            for k in range(1, t.anim_frames + 1):
                want.add(p + k if t.anim_type != 3 else p - k)
    want -= EXCLUDE_TILES
    return sorted(p for p in want if p in tiles and tiles[p].w > 0 and tiles[p].h > 0 and p < MAXTILES), actors_present


class _T:            # light tile wrapper for a second downscale pass
    def __init__(self, w, h, data):
        self.w, self.h, self.data = w, h, data


def emit_tiles(blob, tiles, chosen, pal, lut, quarter_big=0):
    """Write pixel data; returns the directory entries."""
    pal_np = np.array(pal, dtype=np.int32)
    entries = {}
    total = 0
    stored = {}                  # identical pixel data is stored once
    for p in chosen:
        t = tiles[p]
        shift = 0 if in_ranges(p, FULLRES_RANGES) else 1
        if t.w <= 2 or t.h <= 2:
            shift = 0
        if shift:
            w, h, px = downscale_tile(t, pal_np, lut)
            if quarter_big and t.w * t.h >= quarter_big and w > 8 and h > 8 and not (2510 <= p <= 2629):
                w, h, px = downscale_tile(_T(w, h, px), pal_np, lut)
                shift = 2
        else:
            w, h, px = t.w, t.h, t.data
        flags = 1
        if 255 in px:
            flags |= 2
        if (1 << log2ceil(h)) == h:
            flags |= 8
        key = (w, h, bytes(px))
        if key in stored:
            off, flags = stored[key]
        else:
            blob.align(4)
            off = blob.here()
            blob.raw(px)
            if (flags & 2) and h <= 255:
                a = np.frombuffer(px, dtype=np.uint8).reshape(w, h)
                op = a != 255
                anyc = op.any(axis=1)
                top = np.where(anyc, op.argmax(axis=1), 255).astype(np.uint8)
                bot = np.where(anyc, h - op[:, ::-1].argmax(axis=1), 0).astype(np.uint8)
                blob.raw(top.tobytes())
                blob.raw(bot.tobytes())
                flags |= 4
            stored[key] = (off, flags)
            total += len(px)
        entries[p] = (w, h, t.w, t.h, shift, log2ceil(w), log2ceil(h), flags,
                      t.xoff, t.yoff, (t.anim_frames | (t.anim_type << 6)), t.anim_speed, off)
    return entries, total


# --------------------------------------------------------------- maps ---
SPECIAL_KEEP_INVISIBLE = set(range(1, 11))   # SE, activator, touchplate, locators, ...
PRUNE_PICS = {1405, 1267}                      # APLAYER (MP starts), RAT critters
# (RESPAWN, picnum 9, stays: activators spawn enemies through it - E1L5's
#  Battlelord among them)


MAP_LIMITS = dict(sectors=600, walls=3440, sprites=912)   # engine.h minus runtime headroom


def _game_names():
    names = {}
    for line in open(os.path.join(SRC_GAME_DIR, "names.h")):
        m = re.match(r"#define (\w+) (\d+)", line)
        if m:
            names[m.group(1)] = int(m.group(2))
    return names


# sprites the game code acts on: never thinned out
_KEEP_NAMES = """AMMO AMMOLOTS SHOTGUNAMMO BATTERYAMMO RPGAMMO HBOMBAMMO CRYSTALAMMO GROWAMMO
    DEVISTATORAMMO FREEZEAMMO FIRSTGUNSPRITE SHOTGUNSPRITE CHAINGUNSPRITE RPGSPRITE FREEZESPRITE
    SHRINKERSPRITE DEVISTATORSPRITE HEAVYHBOMB TRIPBOMBSPRITE COLA SIXPAK ATOMICHEALTH SHIELD
    FIRSTAID STEROIDS HOLODUKE JETPACK HEATSENSOR AIRTANK BOOTS ACCESSCARD CAMERA1 VIEWSCREEN
    VIEWSCREEN2 DUKECAR SEENINE OOZFILTER EXPLODINGBARREL EXPLODINGBARREL2 FIREBARREL FIREEXT
    CRACK1 CRACK2 CRACK3 CRACK4 TOILET STALL HYDRENT FANSPRITE WATERFOUNTAIN QUEBALL STRIPEBALL
    POCKET""".split()
CYCLER, MUSICANDSFX_PIC, SEENINE_PIC, OOZFILTER_PIC = 7, 5, 1247, 1079


def prune_sprites(mp, loop_sounds=frozenset()):
    """Drop multiplayer-only sprites and, while a map has more sprites than the
    32X budget, only what the port doesn't act on - in this order: light
    CYCLERs (not implemented), excess C-9 charges of one trigger group (by
    hitag; visible charges first), ambient MUSICANDSFX the port never plays
    (non-looping), then untagged non-blocking decoration.  Enemies, items,
    switches and anything tagged are never thinned."""
    names = _game_names()
    keep_pics = set(names[n] for n in _KEEP_NAMES if n in names)
    def is_actor(p):
        return any(lo <= p <= hi for lo, hi in ACTOR_FAMILIES.values())
    sprites = []
    for s in mp.sprites:
        if s.picnum in PRUNE_PICS or s.picnum == CYCLER:
            continue
        if (s.cstat & 32768) and s.picnum not in SPECIAL_KEEP_INVISIBLE and s.picnum >= 11:
            continue
        if s.sectnum < 0 or s.sectnum >= len(mp.sectors):
            continue
        sprites.append(s)
    limit = MAP_LIMITS["sprites"]
    if len(sprites) > limit:                     # C-9 clusters: keep 6 per trigger group
        groups, out = {}, []
        for s in sprites:
            if s.picnum in (SEENINE_PIC, OOZFILTER_PIC):
                groups.setdefault(s.hitag, []).append(s)
            else:
                out.append(s)
        for tag, lst in groups.items():
            lst.sort(key=lambda s: -s.xrepeat)   # visible charges first
            out += lst[:6]
        sprites = out
    if len(sprites) > limit:                     # ambient sounds the port can't play
        mute = [s for s in sprites if s.picnum == MUSICANDSFX_PIC and s.lotag not in loop_sounds]
        drop = set(id(s) for s in mute[:len(sprites) - limit])
        sprites = [s for s in sprites if id(s) not in drop]
    if len(sprites) > limit:                     # plain decoration
        decor = [s for s in sprites if s.picnum >= 11 and not (s.cstat & 1) and not s.lotag and not s.hitag
                 and s.picnum not in keep_pics and not is_actor(s.picnum)]
        drop = set(id(s) for s in decor[:len(sprites) - limit])
        sprites = [s for s in sprites if id(s) not in drop]
    if len(sprites) > limit:
        raise SystemExit(f"map has {len(sprites)} sprites that can't be thinned (max {limit})")
    return sprites


LOOP_SOUNDS = frozenset()       # set in main(): sound ids whose CON type loops


def pack_map(mp, e):
    sprites = prune_sprites(mp, LOOP_SOUNDS)
    if len(mp.sectors) > MAP_LIMITS["sectors"] or len(mp.walls) > MAP_LIMITS["walls"]:
        raise SystemExit(f"map exceeds 32X limits: {len(mp.sectors)} sectors / {len(mp.walls)} walls "
                         f"(max {MAP_LIMITS['sectors']}/{MAP_LIMITS['walls']})")
    # single-player start = the map header position (APLAYER sprites are MP spawns)
    px, py, pz, pa, ps = mp.posx, mp.posy, mp.posz, mp.ang, mp.cursectnum
    hdr = struct.pack(e + "iiihhHHHH", px, py, pz, pa, ps,
                      len(mp.sectors), len(mp.walls), len(sprites), 0)
    out = bytearray()
    for s in mp.sectors:
        out += struct.pack(e + "iihhHHhhhhbbBBBBBBhhBBh",
                           s.ceilingz, s.floorz, s.wallptr, s.wallnum, s.ceilingstat & 0xffff,
                           s.floorstat & 0xffff, s.ceilingpicnum, s.floorpicnum, s.ceilingheinum,
                           s.floorheinum, s.ceilingshade, s.floorshade, s.ceilingpal, s.floorpal,
                           s.ceilingxpanning, s.ceilingypanning, s.floorxpanning, s.floorypanning,
                           s.lotag, s.hitag, s.visibility, 0, s.extra)
    secb = bytes(out)
    out = bytearray()
    for w in mp.walls:
        out += struct.pack(e + "iihhHhhbBBBBB", w.x, w.y, w.point2, w.nextsector, w.cstat & 0xffff,
                           w.picnum, w.overpicnum, w.shade, w.pal, w.xrepeat, w.yrepeat,
                           w.xpanning, w.ypanning)
    wallb = bytes(out)
    out = bytearray()
    for s in sprites:
        out += struct.pack(e + "iiiHhbBBBBBbbhhhhhhhhhh", s.x, s.y, s.z, s.cstat & 0xffff, s.picnum,
                           s.shade, s.pal, s.clipdist, 0, s.xrepeat, s.yrepeat, s.xoffset,
                           s.yoffset, s.sectnum, s.statnum, s.ang, -1, s.xvel, s.yvel, s.zvel,
                           s.lotag, s.hitag, s.extra)
    sprb = bytes(out)
    wallx = bytearray()
    for w in mp.walls:
        wallx += struct.pack(e + "hhhh", w.nextwall, w.lotag, w.hitag, w.extra)
    return hdr, (secb, wallb, sprb), bytes(wallx), len(sprites)


# ------------------------------------------------------------- sounds ---
def code_sound_refs(sounds):
    """Sound ids the game code can play: every CON sound name used in
    src/game/*.c, plus the families it indexes as NAME + (krand() % n)."""
    byname = {s["name"]: s["id"] for s in sounds}
    src = ""
    for fn in sorted(glob.glob(os.path.join(SRC_GAME_DIR, "*.c"))):
        with open(fn, encoding="latin1") as f:
            src += f.read()
    ids = {byname[t] for t in set(re.findall(r"\b[A-Z][A-Z0-9_]+\b", src)) if t in byname}
    for name, op, n in re.findall(r"\b([A-Z][A-Z0-9_]+)\s*\+\s*\(\s*krand\(\)\s*([%&])\s*(\d+)\s*\)", src):
        if name in byname:
            ids.update(byname[name] + k for k in range(int(n) if op == "%" else int(n) + 1))
    return ids


def map_sound_refs(maps):
    """Sound ids the kept levels can trigger: MUSICANDSFX sprites (lotag start /
    hitag stop sound) and one-shot sound sectors (lotag 10000 + id)."""
    ids = set()
    for mp in maps:
        for s in mp.sprites:
            if s.picnum == 5:
                ids.update((s.lotag, s.hitag))
        for sc in mp.sectors:
            if 10000 <= sc.lotag < 16383:
                ids.add(sc.lotag - 10000)
    return ids


def select_sounds(grp, sounds, maps, actors_present):
    """Only sounds something can actually play (there is no CON VM: enemy and
    player sounds come from the C code), in priority order: code-triggered
    (weapons, Duke's one-liners, enemies present in the levels) first, then
    level sounds (doors, lifts, switches, one-shots), ambience last."""
    code, mapr = code_sound_refs(sounds), map_sound_refs(maps)
    chosen, seen = [], set()
    for s in sounds:
        sid, name = s["id"], s["name"]
        if sid in seen or name in SOUND_EXCLUDE or not grp.has(s["file"]):
            continue
        fam = next((b for pfx, b in SOUND_ACTOR_PREFIX.items() if name.startswith(pfx)), None)
        if fam is not None and fam not in actors_present:
            continue
        if sid in code:
            tier = 0
        elif sid in mapr:
            tier = 1
        else:
            continue
        if name.startswith(SOUND_TIER2_PREFIX):
            tier = 2
        seen.add(sid)
        chosen.append((tier, s))
    chosen.sort(key=lambda x: (x[0], x[1]["id"]))
    return chosen


def encode_sounds(grp, chosen):
    out = {}
    used = 0
    cache = {}
    # Keep all referenced effects if possible: first lower the sample rate of
    # low-priority ambience rather than discarding entire sounds. Estimate the
    # encoded sizes once per unique file; code effects retain their old rate.
    seen, core_bytes, ambient_bytes = set(), 0, 0
    for tier, s in chosen:
        fn = s["file"]
        if fn in seen:
            continue
        seen.add(fn)
        snd = load_sound(grp.read(fn))
        if snd is None:
            continue
        rate, pcm = snd
        n = len(pcm) // 2 if rate > 11025 else len(pcm)
        if tier == 2:
            ambient_bytes += 4 + (n + 1) // 2
        else:
            core_bytes += 4 + (n + 1) // 2
    ambient_scale = 1.0
    if ambient_bytes and core_bytes + ambient_bytes > SOUND_BUDGET:
        ambient_scale = max(0.35, min(1.0, (SOUND_BUDGET - core_bytes - 1024) / ambient_bytes))
        print(f"[convert] ambient sounds resampled to {ambient_scale:.0%} of previous rate to fit")
    for tier, s in chosen:
        fn = s["file"]
        if fn in cache:
            out[s["id"]] = (s, cache[fn])
            continue
        snd = load_sound(grp.read(fn))
        if snd is None:
            continue
        rate, pcm = snd
        if rate > 11025:
            pcm = resample(pcm, rate, rate // 2)
            rate //= 2
        if tier == 2 and ambient_scale < 1.0:
            target = max(1000, int(rate * ambient_scale))
            pcm = resample(pcm, rate, target)
            rate = target
        enc, _, _ = ima_encode(pcm)
        if used + len(enc) > SOUND_BUDGET:
            continue
        used += len(enc)
        cache[fn] = (rate, len(pcm), enc)
        out[s["id"]] = (s, cache[fn])
    return out, used


# ---------------------------------------------------------- title art ---
def font_colours(font_path):
    """the opaque colours of a font sheet (the big Duke font)"""
    from PIL import Image
    if not font_path or not os.path.exists(font_path):
        return []
    f = np.asarray(Image.open(font_path).convert("RGBA")).reshape(-1, 4)
    return sorted({(int(r), int(g), int(b)) for r, g, b, a in f if a >= 128})


TRANSPARENT = 255      # palette index left free in picture palettes: see-through sprite pixels
LZ4_PICTURES = {0, 1, 2}  # title background, transition, 3D Realms: full screens, stored LZ4
LOGO_POS = (66, 104)   # top-left of the DUKE NUKEM 3D logo on the 320x224 title


def to_cram(colours):
    cram = [0x8000 | ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3) for r, g, b in colours]
    return cram + [0x8000] * (256 - len(cram))


def open_rgba(path, size=None):
    from PIL import Image
    im = Image.open(path).convert("RGBA")
    if size and im.size != size:
        im = im.resize(size, Image.LANCZOS)
    return np.asarray(im)


def index_images(images, extra):
    """Give several RGBA images one shared palette.  Opaque colours are kept
    exactly if they fit in 255 entries next to the `extra` colours (text drawn
    on the pictures); otherwise all images are quantised together.  Pixels
    with alpha < 128 become TRANSPARENT (index 255, never a colour)."""
    from PIL import Image
    masks = [im[..., 3] >= 128 for im in images]
    keys = [(im[..., 0].astype(np.int64) << 16) | (im[..., 1].astype(np.int64) << 8) | im[..., 2]
            for im in images]
    used = np.unique(np.concatenate([k[m] for k, m in zip(keys, masks)]))
    extra = list(dict.fromkeys(tuple(int(v) for v in c) for c in extra))
    used_set = set(int(k) for k in used)
    ext = [c for c in extra if ((c[0] << 16) | (c[1] << 8) | c[2]) not in used_set]
    budget = 255 - len(ext)
    if len(used) > budget:                           # quantise everything together
        h = sum(im.shape[0] for im in images)
        w = max(im.shape[1] for im in images)
        canvas = np.zeros((h, w, 3), np.uint8)
        y = 0
        first = images[0][masks[0]][0][:3]
        for im, m in zip(images, masks):
            part = im[..., :3].copy()
            part[~m] = first
            canvas[y:y + im.shape[0], :im.shape[1]] = part
            y += im.shape[0]
        q = Image.fromarray(canvas).quantize(colors=budget, method=Image.MEDIANCUT, dither=Image.NONE)
        qp, qa = q.getpalette(), np.asarray(q)
        qkeys = np.array([(qp[3 * v] << 16) | (qp[3 * v + 1] << 8) | qp[3 * v + 2] for v in range(len(qp) // 3)],
                         np.int64)
        keys, y = [], 0
        for im in images:
            keys.append(qkeys[qa[y:y + im.shape[0], :im.shape[1]]])
            y += im.shape[0]
        used = np.unique(np.concatenate([k[m] for k, m in zip(keys, masks)]))
    palette = [(int(k) >> 16, (int(k) >> 8) & 255, int(k) & 255) for k in used] + ext
    out = []
    for k, m in zip(keys, masks):
        idx = np.full(m.shape, TRANSPARENT, np.uint8)
        idx[m] = np.searchsorted(used, k[m]).astype(np.uint8)
        out.append(idx)
    return out, palette


def split_logo(rgba):
    """DUKE NUKEM 3D logo -> (top, bottom) parts with their offsets: the cut is
    the first row where the opaque width collapses (the "3D" under the name)."""
    m = rgba[..., 3] >= 128
    cnt = m.sum(axis=1)
    cut = None
    for y in range(8, len(cnt)):
        if cnt[y] < 0.4 * cnt[:y].max():
            cut = y
            break
    parts = []
    for y0, y1 in ((0, cut), (cut, len(cnt))):
        sub = m[y0:y1]
        ys, xs = np.nonzero(sub)
        by0, by1, bx0, bx1 = ys.min() + y0, ys.max() + y0, xs.min(), xs.max()
        parts.append((rgba[by0:by1 + 1, bx0:bx1 + 1], int(bx0), int(by0)))
    return parts


def load_pictures(args):
    """Full-screen pictures and the title logo, in DK_PIC_* order
    (src/core/dukedata.h).  Returns (pictures, palettes): each picture is
    (w, h, pixels, palette number, x, y)."""
    pics, pals = [None] * 6, []
    def ok(path):
        return path and os.path.exists(path)
    if ok(args.title):                       # title background + logo (one palette)
        imgs = [open_rgba(args.title, (320, 224))]
        logo = split_logo(open_rgba(args.title_logo)) if ok(args.title_logo) else []
        imgs += [part for part, _, _ in logo]
        logo32 = open_rgba(args.title_32x) if ok(args.title_32x) else None
        if logo32 is not None:
            # Trim transparent padding so positioning refers to visible artwork.
            ys, xs = np.nonzero(logo32[..., 3] >= 128)
            if len(xs):
                logo32 = logo32[ys.min():ys.max()+1, xs.min():xs.max()+1]
                imgs.append(logo32)
            else:
                logo32 = None
        idx, pal = index_images(imgs, font_colours(args.title_font) + TEXT_STYLE_RGB)
        pals.append(pal)
        pics[0] = (320, 224, idx[0].tobytes(), len(pals) - 1, 0, 0)
        for k, (part, bx, by) in enumerate(logo):
            h, w = part.shape[:2]
            x = 85 if k == 1 and logo32 is not None else LOGO_POS[0] + bx
            pics[3 + k] = (w, h, idx[1 + k].tobytes(), len(pals) - 1, x, LOGO_POS[1] + by)
        if logo32 is not None:
            h, w = logo32.shape[:2]
            pics[5] = (w, h, idx[1 + len(logo)].tobytes(), len(pals) - 1, 150, 146)
    for slot, path, extra in ((1, args.transition, tinted_colours(args.transition_font, TFONT_TINTS)),
                              (2, args.realms, [])):
        if ok(path):
            idx, pal = index_images([open_rgba(path, (320, 224))], extra)
            pals.append(pal)
            pics[slot] = (320, 224, idx[0].tobytes(), len(pals) - 1, 0, 0)
    return pics, pals


# --------------------------------------------------------------- main ---
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--grp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--maps", default=",".join(m for m, _, _ in KEEP_LEVELS))
    ap.add_argument("--code-reserve", type=int, default=176,
                    help="KB kept free for the SH-2/68000 code (~125 KB today + growth margin)")
    ap.add_argument("--title", default=os.path.join(ASSET_DIR, "title.png"),
                    help="320x224 title screen picture")
    ap.add_argument("--title-font", default=os.path.join(ASSET_DIR, "dukefont.png"),
                    help="big font drawn on the title (its colours join the title palette)")
    ap.add_argument("--transition", default=os.path.join(ASSET_DIR, "transition.png"),
                    help="320x224 picture for the ENTERING and LEVEL COMPLETED screens")
    ap.add_argument("--transition-font", default=os.path.join(ASSET_DIR, "tormentfont.png"),
                    help="font drawn on the transition picture (its colours join that palette)")
    ap.add_argument("--title-logo", default=os.path.join(ASSET_DIR, "title_logo.png"),
                    help="DUKE NUKEM / 3D logo (transparent) that slams onto the title")
    ap.add_argument("--title-32x", default=os.path.join(ASSET_DIR, "title_32x.png"),
                    help="transparent 32X title logo, shown after the 3D logo")
    ap.add_argument("--realms", default=os.path.join(ASSET_DIR, "realms.png"),
                    help="320x224 3D Realms screen shown at power-on")
    args = ap.parse_args()
    args.pictures, args.palettes = load_pictures(args)
    for name, pic in zip(("title background", "transition screen", "3D Realms screen", "logo DUKE NUKEM", "logo 3D", "logo 32X"),
                         args.pictures):
        if pic:
            print(f"[convert] {name}: {pic[0]}x{pic[1]} at ({pic[4]},{pic[5]}), "
                  f"palette {pic[3]} ({len(args.palettes[pic[3]])} colours)")
    if args.pictures[1]:
        EXCLUDE_TILES.add(LOADSCREEN)

    grp = Grp(args.grp)
    os.makedirs(args.out, exist_ok=True)
    full = grp.has("E3L2.MAP")
    print(f"[convert] {args.grp}: {len(grp.order)} files ({'registered' if full else 'shareware'})")

    defines, con_sounds, quotes, levelnames, music_lists = parse_con(grp)
    global LOOP_SOUNDS
    LOOP_SOUNDS = frozenset(snd["id"] for snd in con_sounds if snd["type"] & 1)
    tiles = load_tiles(grp)
    pal, numshades, shades, trans = load_palette(grp)
    remaps, altpals = load_lookups(grp)
    tables = grp.read("TABLES.DAT")
    sintable = struct.unpack_from("<2048h", tables, 0)
    radar = list(struct.unpack_from("<640h", tables, 4096))
    radarang = radar + [-radar[639 - i] for i in range(640)]
    textfont = tables[4096 + 1280:4096 + 1280 + 1024]
    smallfont = tables[4096 + 1280 + 1024:4096 + 1280 + 2048]

    levels = []
    for m, ep, lv in KEEP_LEVELS:
        if m in args.maps.split(",") and grp.has(m + ".MAP"):
            levels.append((m, ep, lv, load_map(grp.read(m + ".MAP"))))
        else:
            print(f"[convert] {m}: not in this GRP - skipped")
    maps = [x[3] for x in levels]

    chosen, actors = select_tiles(tiles, maps)
    pals_used = {0, 1, 2, 6, 7, 8, 21, 23}
    for mp in maps:
        for w in mp.walls:
            pals_used.add(w.pal)
        for s in mp.sectors:
            pals_used.add(s.floorpal)
            pals_used.add(s.ceilingpal)
        for s in mp.sprites:
            pals_used.add(s.pal)
    pals_used = sorted(p for p in pals_used if p == 0 or p in remaps)
    lut = nearest_lut(pal)

    snd_sel = select_sounds(grp, con_sounds, maps, actors)
    global SOUND_BUDGET
    ROM_LIMIT = 4 * 1024 * 1024 - args.code_reserve * 1024
    MIN_SOUND = 480 * 1024
    quarter_big = 0
    # everything but the samples first; the sound budget is whatever room is left
    for attempt in range(8):
        base = build_blobs(args, grp, full, defines, quotes, levelnames, tiles, pal, shades, remaps,
                           altpals, sintable, radarang, textfont, smallfont, levels, chosen, pals_used,
                           lut, {}, quarter_big, dry=True)
        room = ROM_LIMIT - base - 16 * 1024            # sound directory + alignment slack
        if room >= MIN_SOUND or quarter_big == 2048:
            break
        quarter_big = 12000 if not quarter_big else max(2048, quarter_big // 2)
        print(f"[convert] {base // 1024} KB without sound: quarter-res sprites >= {quarter_big} px")
    SOUND_BUDGET = max(0, min(SOUND_MAX_BUDGET, room))
    snd_enc, snd_bytes = encode_sounds(grp, snd_sel)
    size_est = build_blobs(args, grp, full, defines, quotes, levelnames, tiles, pal, shades, remaps,
                           altpals, sintable, radarang, textfont, smallfont, levels, chosen, pals_used,
                           lut, snd_enc, quarter_big, dry=True)
    if size_est > ROM_LIMIT:
        raise SystemExit(f"cannot fit the data into the 4 MB ROM ({size_est // 1024} KB > {ROM_LIMIT // 1024} KB)")
    want = sum(1 for _ in snd_sel)
    print(f"[convert] sound budget {SOUND_BUDGET // 1024} KB: {len(snd_enc)} of {want} playable sounds")
    size = build_blobs(args, grp, full, defines, quotes, levelnames, tiles, pal, shades, remaps, altpals,
                       sintable, radarang, textfont, smallfont, levels, chosen, pals_used, lut, snd_enc,
                       quarter_big, dry=False)
    write_report(args, levels, chosen, snd_enc, snd_bytes, size, quarter_big, full)


def build_blobs(args, grp, full, defines, quotes, levelnames, tiles, pal, shades, remaps, altpals,
                sintable, radarang, textfont, smallfont, levels, chosen, pals_used, lut, snd_enc,
                quarter_big, dry):
    global last_mapinfo, last_tile_stats
    for endian, suffix in (((">", "be"),) if dry else ((">", "be"), ("<", "le"))):
        b = Blob(endian)
        b.raw(bytes(256))                           # header placeholder
        # palette
        b.align(4)
        off_palette = b.here()
        for c in build_palette_cram(pal):
            b.pack("H", c)
        off_altpals = b.here()
        for ap_ in altpals[:5]:
            for c in build_palette_cram(ap_):
                b.pack("H", c)
        # sine table
        b.align(4)
        off_sin = b.here()
        b.pack("2048h", *sintable)
        off_radar = b.here()
        b.pack("1280h", *radarang)
        off_font = b.here()
        b.raw(textfont)
        b.raw(smallfont)
        # full-screen pictures / logo sprites, their palettes, and a directory
        b.align(4)
        cram_off = []
        for pal_ in args.palettes:
            cram_off.append(b.here())
            b.pack("256H", *to_cram(pal_))
        picdir = []
        for slot, pic in enumerate(args.pictures):
            if not pic:
                picdir.append((0, 0, 0, 0, 0, 0, 0, 0))
                continue
            pw, ph, ppx, pn, px_, py_ = pic
            b.align(4)
            off = b.here()
            lz = (0, 0)
            if slot in LZ4_PICTURES and pw == 320 and ph > 192:
                # shown whole: two LZ4 blocks, one per run of screen lines
                za, zb = lz4_compress(ppx[:192 * 320]), lz4_compress(ppx[192 * 320:])
                b.raw(za)
                b.raw(zb)
                lz = (len(za), len(zb))
            else:
                b.raw(ppx)                   # logo sprites: drawn straight from ROM
            picdir.append((off, pw, ph, cram_off[pn], px_, py_) + lz)
        b.align(4)
        off_pics = b.here()
        for e in picdir:
            b.pack("IHHIhhII", *e)
        # palookups
        b.align(16)
        palook = {}
        for p in pals_used:
            palook[p] = b.here()
            if p == 0:
                b.raw(shades[:32 * 256])
            else:
                rm = remaps[p]
                tab = bytearray(32 * 256)
                for s in range(32):
                    row = shades[s * 256:(s + 1) * 256]
                    tab[s * 256:(s + 1) * 256] = bytes(row[rm[c]] for c in range(256))
                b.raw(bytes(tab))
        b.align(4)
        off_palookup = b.here()
        for p in range(32):
            b.pack("I", palook.get(p, palook[0]))
        # tiles
        entries, tile_bytes = emit_tiles(b, tiles, chosen, pal, lut, quarter_big)
        b.align(4)
        off_tiledir = b.here()
        ntiles = max(entries) + 1 if entries else 0
        for p in range(ntiles):
            if p in entries:
                w, h, ow, oh, sh, lw, lh, fl, xo, yo, an, sp, off = entries[p]
                b.pack("HHHHBBBBbbBBI", w, h, ow, oh, sh, lw, lh, fl, xo, yo, an, sp, off)
            else:
                t = tiles.get(p)
                ow, oh = (t.w, t.h) if t else (0, 0)
                b.pack("HHHHBBBBbbBBI", 0, 0, ow, oh, 0, 0, 0, 0, 0, 0, 0, 0, 0)
        # maps
        mapinfo = []
        for m, ep, lv, mp in levels:
            hdr, parts, wallx, nspr = pack_map(mp, endian)
            b.align(4)
            off_hdr = b.here()
            b.raw(hdr)
            offs, sizes = [], []
            for raw in parts:
                lz = lz4_compress(raw)
                if suffix == "be":
                    assert lz4_decompress(lz, len(raw)) == raw
                b.align(4)
                offs.append(b.here())
                sizes.append(len(lz))
                b.raw(lz)
            b.align(4)
            off_wx = b.here()
            b.raw(wallx)
            title = levelnames.get((ep, lv), ("", "", m))[2] or m
            mapinfo.append((m, title, off_hdr, offs, sizes, off_wx, ep, lv,
                            len(mp.sectors), len(mp.walls), nspr))
        b.align(4)
        off_mapdir = b.here()
        for (m, title, off_hdr, offs, sizes, off_wx, ep, lv, *_rest) in mapinfo:
            b.raw(m.encode()[:8].ljust(8, b"\0"))
            b.raw(title.upper().encode("latin1", "replace")[:31].ljust(32, b"\0"))
            b.pack("IIIIIIIIHHHH", off_hdr, offs[0], offs[1], offs[2], sizes[0], sizes[1], sizes[2],
                   off_wx, 0xFFFF, 0, ep, lv)
        # sounds
        maxsid = max(snd_enc) + 1 if snd_enc else 0
        snd_offs = {}
        for sid in sorted(snd_enc):
            s, (rate, n, enc) = snd_enc[sid]
            key = id(enc)
            if key not in snd_offs:
                b.align(2)
                snd_offs[key] = b.here()
                hdr = struct.pack(endian + "hBB", *struct.unpack(">hBB", enc[:4]))
                b.raw(hdr + enc[4:])
        b.align(4)
        off_snddir = b.here()
        for sid in range(maxsid):
            if sid in snd_enc:
                s, (rate, n, enc) = snd_enc[sid]
                b.pack("IIHBBbbBB", snd_offs[id(enc)], n, rate, 1, max(0, min(255, s["pri"])),
                       max(-128, min(127, s["plo"] // 32)), max(-128, min(127, s["phi"] // 32)),
                       s["type"] & 255, max(0, min(255, s["vol"] + 128)))
            else:
                b.pack("IIHBBbbBB", 0, 0, 0, 0, 0, 0, 0, 0, 0)
        # strings (quotes)
        b.align(4)
        off_strings = b.here()
        nq = max(quotes) + 1 if quotes else 0
        b.pack("I", nq)
        tbl = b.here()
        b.raw(bytes(4 * nq))
        for q in range(nq):
            if q in quotes:
                b.patch(tbl + 4 * q, "I", b.here())
                b.raw(quotes[q].encode("latin1", "replace") + b"\0")
        b.align(4)
        size = b.here()
        hdr = struct.pack(endian + "22I", 0x324B4B44, 3, size, ntiles, off_tiledir, off_palette,
                          off_palookup, len(mapinfo), off_mapdir, maxsid, off_snddir,
                          off_altpals, off_sin, 0, 0, off_strings, 0, 1 if full else 0,
                          off_radar, off_font, off_pics, len(picdir))
        b.buf[0:len(hdr)] = hdr
        last_mapinfo = mapinfo
        last_tile_stats = (len(entries), tile_bytes)
        if dry:
            return size
        path = os.path.join(args.out, f"gamedata_{suffix}.bin")
        with open(path, "wb") as f:
            f.write(b.buf)
    gen_headers(args, grp, defines)
    return size


def gen_headers(args, grp, defines):
    mapinfo = last_mapinfo

    # ---- CON actions/moves (enemy animation & speed tables) ----
    acts, moves = {}, {}
    con_txt = grp.read("GAME.CON").decode("latin1") if grp.has("GAME.CON") else ""
    for line in con_txt.splitlines():
        p = line.split("//")[0].split()
        if len(p) >= 2 and p[0] == "action" and p[1][0].isalpha() and p[1] not in acts:
            vals = [int(x) for x in p[2:7] if re.match(r"^-?\d+$", x)]
            if len(p) == 2 or vals:
                vals += [0] * (5 - len(vals))
                if len(vals) >= 3 and vals[1] == 0:
                    vals[1] = 1
                if vals[2] == 0:
                    vals[2] = 1
                acts[p[1]] = vals[:5]
        elif len(p) >= 2 and p[0] == "move" and p[1][0].isalpha() and p[1] not in moves:
            vals = [int(x) for x in p[2:4] if re.match(r"^-?\d+$", x)]
            if len(p) == 2 or vals:
                vals += [0] * (2 - len(vals))
                moves[p[1]] = vals[:2]
    with open(os.path.join(args.out, "con_actions.h"), "w") as f:
        f.write("/* generated from GAME.CON by tools/convert.py: {start, frames, views, inc, delay} */\n")
        f.write("#ifndef CON_ACTIONS_H\n#define CON_ACTIONS_H\n")
        for k in sorted(acts):
            f.write(f"#define CA_{k} {{{', '.join(str(v) for v in acts[k])}}}\n")
        for k in sorted(moves):
            f.write(f"#define CM_{k} {{{', '.join(str(v) for v in moves[k])}}}\n")
        f.write("#endif\n")

    # ---- generated header with original CON constants ----
    hpath = os.path.join(args.out, "con_defs.h")
    with open(hpath, "w") as f:
        f.write("/* generated by tools/convert.py from DEFS.CON / USER.CON - do not edit */\n")
        f.write("#ifndef CON_DEFS_H\n#define CON_DEFS_H\n")
        for k in sorted(defines):
            if re.match(r"^[A-Z_][A-Z0-9_]*$", k):
                f.write(f"#define CON_{k} {defines[k]}\n")
        f.write(f"#define DK_NUM_LEVELS {len(mapinfo)}\n")
        f.write("#endif\n")



def write_report(args, levels, chosen, snd_enc, snd_bytes, size, quarter_big, full):
    mapinfo = last_mapinfo
    ntiles, tile_bytes = last_tile_stats
    lines = []
    lines.append(f"Duke Nukem 3D 32X data report ({'registered GRP' if full else 'SHAREWARE GRP - E1 only'})")
    lines.append("maps: " + ", ".join(f"{m[0]} {m[1]} ({m[8]} sectors/{m[9]} walls/{m[10]} sprites, "
                                     f"{sum(m[4]) // 1024} KB LZ4)" for m in mapinfo))
    lines.append(f"tiles: {ntiles} kept, {tile_bytes // 1024} KB pixels; world textures & sprites at 50% "
                 f"linear resolution (25% of the pixels){', big sprites at 25%' if quarter_big else ''}; "
                 f"HUD/fonts/title at 100%")
    lines.append(f"sounds: {len(snd_enc)} kept, {snd_bytes // 1024} KB 4-bit IMA ADPCM (50% of 8-bit PCM), "
                 f"22 kHz sources halved to 11 kHz")
    lines.append(f"blob: {size // 1024} KB")
    txt = "\n".join(lines)
    open(os.path.join(args.out, "report.txt"), "w").write(txt + "\n")
    print("[convert] " + txt.replace("\n", "\n[convert] "))


if __name__ == "__main__":
    main()
