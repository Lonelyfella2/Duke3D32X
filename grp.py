"""Minimal reader for Ken Silverman's GRP archive format (Build engine)."""
import struct

class Grp:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            data = f.read()
        if data[:12] != b"KenSilverman":
            raise ValueError(f"{path}: not a GRP file")
        n = struct.unpack_from("<i", data, 12)[0]
        self.files = {}
        self.order = []
        off = 16 + 16 * n
        for i in range(n):
            name = data[16 + 16 * i:16 + 16 * i + 12].split(b"\0")[0].decode("ascii").upper()
            size = struct.unpack_from("<i", data, 16 + 16 * i + 12)[0]
            self.files[name] = (off, size)
            self.order.append(name)
            off += size
        self.data = data

    def has(self, name):
        return name.upper() in self.files

    def read(self, name):
        off, size = self.files[name.upper()]
        return self.data[off:off + size]

    def names(self, ext=None):
        if ext is None:
            return list(self.order)
        ext = ext.upper()
        return [n for n in self.order if n.endswith(ext)]
