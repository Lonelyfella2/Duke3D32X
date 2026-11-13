#!/usr/bin/env python3
"""
mkbigfont.py - convert a big bitmap font sheet into a C header.

    mkbigfont.py --prefix dfont assets/dukefont.png gen/dukefont.h
        Duke's big red menu font: title and pause screens
    mkbigfont.py --prefix tfont assets/tormentfont.png gen/tormentfont.h
        the Tormentstein 3D font: top messages, ENTERING / LEVEL COMPLETED,
        the skill and level menu

Sheet layout (both PNGs supplied for the port):
    a sample line ("DUKE NUKEM ATOMIC" / "Tormentstein 3D"), a gap, then
    the glyph rows (touching - the Duke sheet - or with gaps between them)
        !"#$%&'()*+,-./
        0123456789
        ABCDEFGHIJKLM
        NOPQRSTUVWXYZ
    glyphs are packed edge to edge (their shadows touch), so they are cut
    apart by colour rather than by empty columns:
      1. "cores" = 4-connected areas of the brighter shades (the dark outline
         shades are where neighbours touch);
      2. cores that overlap in x form one glyph ('"' and '%' have two);
         the cluster count is then fixed up against the expected characters
         (split the widest at its thinnest column / merge the closest pair);
      3. every opaque pixel goes to the nearest core (multi-source flood
         fill), which separates glyphs that interleave by a column;
      4. a glyph's advance is the distance to the next glyph on the sheet -
         Duke's menutext() draws its big font edge to edge the same way.
Pixels keep their own colours: up to 15 opaque colours, 4 bits per pixel.
Glyphs the sheet lacks but the game prints are built from others: ':' and
';' from the full stop and comma, '<' and '>' from the slash.
"""
import argparse
import heapq
import sys

from PIL import Image

ROWS = ['!"#$%&\'()*+,-./', "0123456789", "ABCDEFGHIJKLM", "NOPQRSTUVWXYZ"]
SPACE_ADV = 5          # MENUES.C menutext(): a space is 5 pixels


def load(path):
    im = Image.open(path)
    if im.mode == "P":
        trans = im.info.get("transparency", 0)
        pal = im.getpalette()
        w, h = im.size
        src = im.load()
        cols, idx = [], {}
        px = [[0] * w for _ in range(h)]
        for y in range(h):
            for x in range(w):
                p = src[x, y]
                if p == trans:
                    continue
                rgb = tuple(pal[3 * p:3 * p + 3])
                if rgb not in idx:
                    idx[rgb] = len(cols)
                    cols.append(rgb)
                px[y][x] = idx[rgb] + 1
        return px, cols, w, h
    im = im.convert("RGBA")
    w, h = im.size
    src = im.load()
    cols, idx = [], {}
    px = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            r, g, b, a = src[x, y]
            if a < 128:
                continue
            if (r, g, b) not in idx:
                idx[(r, g, b)] = len(cols)
                cols.append((r, g, b))
            px[y][x] = idx[(r, g, b)] + 1
    return px, cols, w, h


def bands(px, w, h):
    occ = [any(px[y]) for y in range(h)]
    out, y = [], 0
    while y < h:
        if occ[y]:
            y0 = y
            while y < h and occ[y]:
                y += 1
            out.append((y0, y - 1))
        else:
            y += 1
    return out


def segment_row(px, w, y0, gh, nexp, bright, rank):
    ys = range(y0, y0 + gh)
    # 1. cores
    seen, cores = set(), []
    for x in range(w):
        for y in ys:
            if (x, y) in seen or not bright(px[y][x]):
                continue
            st, pts = [(x, y)], []
            seen.add((x, y))
            while st:
                a, b = st.pop()
                pts.append((a, b))
                for c, d in ((a + 1, b), (a - 1, b), (a, b + 1), (a, b - 1)):
                    if 0 <= c < w and y0 <= d < y0 + gh and (c, d) not in seen and bright(px[d][c]):
                        seen.add((c, d))
                        st.append((c, d))
            cores.append(pts)
    # 2. cluster cores overlapping in x
    cores.sort(key=lambda p: min(q[0] for q in p))
    clusters = []
    for pts in cores:
        a, b = min(q[0] for q in pts), max(q[0] for q in pts)
        if clusters and a <= clusters[-1][1]:
            c = clusters[-1]
            clusters[-1] = (c[0], max(c[1], b), c[2] + pts)
        else:
            clusters.append((a, b, list(pts)))
    while len(clusters) > nexp:          # merge the closest neighbours
        gaps = [clusters[i + 1][0] - clusters[i][1] for i in range(len(clusters) - 1)]
        i = gaps.index(min(gaps))
        a, b = clusters[i], clusters[i + 1]
        clusters[i:i + 2] = [(a[0], max(a[1], b[1]), a[2] + b[2])]
    while len(clusters) < nexp:
        # Two glyphs whose bright pixels touch: cut where the fewest bright
        # pixels touch across the cut; on a tie, where a glyph's dark
        # right-hand shadow meets the next glyph's lit left edge (the
        # column brightness jumps up from left to right).
        i = max(range(len(clusters)), key=lambda k: clusters[k][1] - clusters[k][0])
        a, b, pts = clusters[i]
        def colmean(x):
            v = [rank[px[y][x]] for y in ys if px[y][x]]
            return sum(v) / len(v) if v else 0.0
        def lit(v):                  # clearly lit: above the three darkest shades
            return v != 0 and rank[v] > 3
        def crossings(x):            # lit pixels touching across the cut
            return sum(1 for y in ys if lit(px[y][x - 1]) and lit(px[y][x]))
        inner = range(a + 3, b - 2)
        cut = min(inner, key=lambda x: (crossings(x), colmean(x - 1) - colmean(x),
                                        abs(x - (a + b) / 2))) if len(inner) else (a + b) // 2
        left = [q for q in pts if q[0] < cut]
        right = [q for q in pts if q[0] >= cut]
        clusters[i:i + 1] = [(a, cut - 1, left), (cut, b, right)]
    # 3. nearest-core ownership of every opaque pixel in the row.  The
    # font's shadow falls to the right, so growing leftwards costs double:
    # a contested outline pixel goes to the glyph on its left.
    owner, best = {}, {}
    heap = []
    for k, (_, _, pts) in enumerate(clusters):
        for q in pts:
            best[q] = 0
            heapq.heappush(heap, (0, k, q))
    while heap:
        dist, k, (a, b) = heapq.heappop(heap)
        if (a, b) in owner:
            continue
        owner[(a, b)] = k
        for c in (a - 1, a, a + 1):
            for d in (b - 1, b, b + 1):
                if (c, d) == (a, b) or not (0 <= c < w and y0 <= d < y0 + gh) or not px[d][c]:
                    continue
                nd = dist + (2 if c < a else 1)
                if (c, d) not in owner and nd < best.get((c, d), 1 << 30):
                    best[(c, d)] = nd
                    heapq.heappush(heap, (nd, k, (c, d)))
    glyphs = []
    for k in range(len(clusters)):
        pts = [q for q, o in owner.items() if o == k]
        x0, x1 = min(q[0] for q in pts), max(q[0] for q in pts)
        bm = [[0] * (x1 - x0 + 1) for _ in range(gh)]
        for (a, b) in pts:
            bm[b - y0][a - x0] = px[b][a]
        glyphs.append((x0, bm))
    return glyphs


def synth(glyph, gh):
    """build ':' ';' '<' '>' if the sheet doesn't have them"""
    def blank(w):
        return [[0] * w for _ in range(gh)]
    def rows_used(bm):
        return [y for y, row in enumerate(bm) if any(row)]
    if ":" not in glyph and "." in glyph:
        bm, adv = glyph["."]
        ys = rows_used(bm)
        if ys:
            dh, gap = ys[-1] - ys[0] + 1, 2
            new = [row[:] for row in bm]
            for y in ys:
                ty = y - dh - gap
                if ty >= 0:
                    new[ty] = bm[y][:]
            glyph[":"] = (new, adv)
    if ";" not in glyph and ":" in glyph and "," in glyph:
        cb, _ = glyph[":"]
        mb, madv = glyph[","]
        w = max(len(cb[0]), len(mb[0]))
        new = blank(w)
        dot = rows_used(glyph["."][0]) if "." in glyph else []
        for y in range(gh):
            src = mb if (dot and y >= dot[0]) else cb
            for x in range(len(src[0])):
                new[y][x] = src[y][x]
        glyph[";"] = (new, max(madv, w + 1))
    if "<" not in glyph and "/" in glyph:
        bm, adv = glyph["/"]
        w, half = len(bm[0]), gh // 2
        up = bm[:half]                          # the slash's upper half: centre -> top right
        lt = [row[:] for row in up] + [row[:] for row in reversed(up)]
        lt += [[0] * w] * (gh - len(lt))
        xs = [x for row in lt for x, v in enumerate(row) if v]
        x0 = min(xs) if xs else 0
        lt = [row[x0:] for row in lt]
        nw = len(lt[0])
        glyph["<"] = (lt, nw + 1)
        glyph[">"] = ([list(reversed(row)) for row in lt], nw + 1)
    return glyph


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--prefix", default="dfont", help="C symbol prefix (dfont, tfont, ...)")
    ap.add_argument("src")
    ap.add_argument("dst")
    args = ap.parse_args()
    src, dst, pre = args.src, args.dst, args.prefix
    PRE = pre.upper()
    px, cols, w, h = load(src)
    if len(cols) > 15:
        raise SystemExit(f"{src}: {len(cols)} colours, at most 15 supported")
    lum = [0.3 * r + 0.59 * g + 0.11 * b for r, g, b in cols]
    order = sorted(range(len(cols)), key=lambda i: lum[i])
    dark = set(i + 1 for i in order[:2])                 # the two outline shades
    bright = lambda v: v != 0 and v not in dark
    rank = [0] * (len(cols) + 1)                         # pixel value -> brightness rank
    for r, i in enumerate(order):
        rank[i + 1] = r + 1

    bs = bands(px, w, h)
    sample, rest = bs[0], bs[1:]
    if len(rest) == len(ROWS):                           # rows separated by gaps
        gh = max(b - a + 1 for a, b in rest)
        rows_y = [a for a, b in rest]
    else:                                                # touching rows: sample height
        gh = sample[1] - sample[0] + 1
        rows_y = []
        for (a, b) in rest:
            n = max(1, round((b - a + 1) / gh))
            rows_y += [a + i * gh for i in range(n)]
    if len(rows_y) != len(ROWS):
        raise SystemExit(f"{src}: found {len(rows_y)} glyph rows, expected {len(ROWS)}")

    glyph = {}
    for y0, chars in zip(rows_y, ROWS):
        gl = segment_row(px, w, y0, min(gh, h - y0), len(chars), bright, rank)
        for i, ch in enumerate(chars):
            x0, bm = gl[i]
            bm += [[0] * len(bm[0])] * (gh - len(bm))
            adv = (gl[i + 1][0] - x0) if i + 1 < len(gl) else len(bm[0])
            glyph[ch] = (bm, max(adv, 1))
    glyph = synth(glyph, gh)

    # pack: glyphs for ASCII 32..95, 4 bits per pixel, rows of ceil(w/2) bytes
    data, table = bytearray(), []
    for c in range(32, 96):
        ch = chr(c)
        if ch not in glyph:
            table.append((0, 0, SPACE_ADV if ch == " " else 0))
            continue
        bm, adv = glyph[ch]
        gw = len(bm[0])
        off = len(data)
        for row in bm:
            for x in range(0, gw, 2):
                lo = row[x]
                hi = row[x + 1] if x + 1 < gw else 0
                data.append(lo | (hi << 4))
        table.append((off, gw, adv))
    with open(dst, "w") as f:
        f.write(f"/* generated by tools/mkbigfont.py from {src} - do not edit */\n")
        f.write(f"#ifndef {PRE}_H_\n#define {PRE}_H_\n#include <stdint.h>\n\n")
        f.write(f"#define {PRE}_H {gh}\n#define {PRE}_NCOL {len(cols)}\n\n")
        f.write(f"/* the font's own colours; pixel value v (1..15) = {pre}_rgb[v-1] */\n")
        f.write(f"static const uint8_t {pre}_rgb[{len(cols)}][3] = {{\n")
        for r, g, b in cols:
            f.write(f"    {{ {r}, {g}, {b} }},\n")
        f.write(f"}};\n\n/* ASCII 32..95: offset into {pre}_px, width, advance (0 = missing) */\n")
        f.write("#ifndef BIGFONT_GLYPH_T\n#define BIGFONT_GLYPH_T\n"
                "typedef struct { uint16_t off; uint8_t w, adv; } bigfont_glyph_t;\n#endif\n")
        f.write(f"static const bigfont_glyph_t {pre}_glyph[64] = {{\n")
        for c, (off, gw, adv) in zip(range(32, 96), table):
            label = chr(c) if chr(c) not in "\\*/" else f"0x{c:02x}"
            f.write(f"    {{ {off}, {gw}, {adv} }},  /* {label} */\n")
        f.write(f"}};\n\n/* rows of ceil(w/2) bytes, low nibble = left pixel */\n")
        f.write(f"static const uint8_t {pre}_px[{len(data)}] = {{\n")
        for i in range(0, len(data), 24):
            f.write("    " + ", ".join(str(v) for v in data[i:i + 24]) + ",\n")
        f.write("};\n\n#endif\n")
    print(f"mkbigfont: {pre}: {len(glyph)} glyphs, {len(cols)} colours, {gh} px tall, {len(data)} bytes -> {dst}")


if __name__ == "__main__":
    main()
