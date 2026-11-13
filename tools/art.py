"""Parser for Build engine ART tile archives and PALETTE.DAT / LOOKUP.DAT."""
import struct


class Tile:
    __slots__ = ("num", "w", "h", "picanm", "data")

    def __init__(self, num, w, h, picanm, data):
        self.num, self.w, self.h, self.picanm, self.data = num, w, h, picanm, data

    # picanm bit fields (Build): frames 0-5, type 6-7, xoff 8-15, yoff 16-23, speed 24-27
    @property
    def anim_frames(self):
        return self.picanm & 63

    @property
    def anim_type(self):
        return (self.picanm >> 6) & 3

    @property
    def xoff(self):
        v = (self.picanm >> 8) & 255
        return v - 256 if v > 127 else v

    @property
    def yoff(self):
        v = (self.picanm >> 16) & 255
        return v - 256 if v > 127 else v

    @property
    def anim_speed(self):
        return (self.picanm >> 24) & 15

    def pixel(self, x, y):           # Build tiles are column-major
        return self.data[x * self.h + y]


def load_tiles(grp):
    tiles = {}
    for name in sorted(n for n in grp.names(".ART")):
        d = grp.read(name)
        ver, numtiles, start, end = struct.unpack_from("<iiii", d, 0)
        n = end - start + 1
        off = 16
        sx = struct.unpack_from(f"<{n}h", d, off); off += 2 * n
        sy = struct.unpack_from(f"<{n}h", d, off); off += 2 * n
        pa = struct.unpack_from(f"<{n}I", d, off); off += 4 * n
        for i in range(n):
            w, h = sx[i], sy[i]
            size = w * h if (w > 0 and h > 0) else 0
            tiles[start + i] = Tile(start + i, max(w, 0), max(h, 0), pa[i], d[off:off + size])
            off += size
    return tiles


def load_palette(grp):
    d = grp.read("PALETTE.DAT")
    pal = [(d[i * 3] , d[i * 3 + 1], d[i * 3 + 2]) for i in range(256)]   # 6-bit VGA
    numshades = struct.unpack_from("<h", d, 768)[0]
    shades = d[770:770 + numshades * 256]
    trans = d[770 + numshades * 256: 770 + numshades * 256 + 65536]
    return pal, numshades, shades, trans


def load_lookups(grp):
    d = grp.read("LOOKUP.DAT")
    n = d[0]
    off = 1
    remaps = {}
    for i in range(n):
        idx = d[off]; off += 1
        remaps[idx] = d[off:off + 256]; off += 256
    alt = []
    while off + 768 <= len(d):
        alt.append([(d[off + i * 3], d[off + i * 3 + 1], d[off + i * 3 + 2]) for i in range(256)])
        off += 768
    return remaps, alt
