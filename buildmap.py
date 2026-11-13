"""Parser for Build engine v7 .MAP files (Duke Nukem 3D)."""
import struct
from dataclasses import dataclass

SECTOR_FMT = "<hhiihhhhbBBBhhbBBBBBhhh"   # 40 bytes
WALL_FMT = "<iihhhhhhbBBBBBhhh"          # 32 bytes
SPRITE_FMT = "<iiihhbBBBBBbbhhhhhhhhhh"   # 44 bytes
assert struct.calcsize(SECTOR_FMT) == 40
assert struct.calcsize(WALL_FMT) == 32
assert struct.calcsize(SPRITE_FMT) == 44

SECTOR_FIELDS = ("wallptr wallnum ceilingz floorz ceilingstat floorstat ceilingpicnum "
                 "ceilingheinum ceilingshade ceilingpal ceilingxpanning ceilingypanning "
                 "floorpicnum floorheinum floorshade floorpal floorxpanning floorypanning "
                 "visibility filler lotag hitag extra").split()
WALL_FIELDS = ("x y point2 nextwall nextsector cstat picnum overpicnum shade pal "
               "xrepeat yrepeat xpanning ypanning lotag hitag extra").split()
SPRITE_FIELDS = ("x y z cstat picnum shade pal clipdist filler xrepeat yrepeat xoffset "
                 "yoffset sectnum statnum ang owner xvel yvel zvel lotag hitag extra").split()


class Rec:
    __slots__ = ("__dict__",)

    def __init__(self, fields, values):
        self.__dict__.update(zip(fields, values))

    def __repr__(self):
        return repr(self.__dict__)


@dataclass
class BuildMap:
    version: int
    posx: int
    posy: int
    posz: int
    ang: int
    cursectnum: int
    sectors: list
    walls: list
    sprites: list


def load_map(data: bytes) -> BuildMap:
    version, posx, posy, posz, ang, cursect = struct.unpack_from("<iiiihh", data, 0)
    if version != 7:
        raise ValueError(f"unsupported map version {version}")
    off = 20
    nsec = struct.unpack_from("<H", data, off)[0]; off += 2
    sectors = []
    for i in range(nsec):
        sectors.append(Rec(SECTOR_FIELDS, struct.unpack_from(SECTOR_FMT, data, off))); off += 40
    nwall = struct.unpack_from("<H", data, off)[0]; off += 2
    walls = []
    for i in range(nwall):
        walls.append(Rec(WALL_FIELDS, struct.unpack_from(WALL_FMT, data, off))); off += 32
    nspr = struct.unpack_from("<H", data, off)[0]; off += 2
    sprites = []
    for i in range(nspr):
        sprites.append(Rec(SPRITE_FIELDS, struct.unpack_from(SPRITE_FMT, data, off))); off += 44
    return BuildMap(version, posx, posy, posz, ang, cursect, sectors, walls, sprites)
