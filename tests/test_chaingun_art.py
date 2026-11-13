#!/usr/bin/env python3
"""Verify that the ROM keeps the original chaingun art pixel-for-pixel.

Usage: python3 tests/test_chaingun_art.py /path/to/DUKE3D.GRP
"""
import os
import struct
import sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'tools'))
from grp import Grp
from art import load_tiles

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
original = load_tiles(Grp(sys.argv[1]))
for endian, name in [('<', 'gamedata_le.bin'), ('>', 'gamedata_be.bin')]:
    blob = open(os.path.join(root, 'gen', name), 'rb').read()
    count, directory = struct.unpack_from(endian + 'II', blob, 12)
    for number in range(2536, 2543):
        assert number < count
        w, h, ow, oh, shift, lw, lh, flags, xo, yo, anim, speed, offset = struct.unpack_from(
            endian + 'HHHHBBBBbbBBI', blob, directory + number * 20)
        tile = original[number]
        assert flags & 1 and shift == 0
        assert (w, h, ow, oh, xo, yo) == (tile.w, tile.h, tile.w, tile.h, tile.xoff, tile.yoff)
        assert blob[offset:offset+w*h] == tile.data, (name, number)
print('Chaingun body, barrel frames and flashes match the original GRP in both data blobs.')
