#!/usr/bin/env python3
"""
extract_engine.py - mechanically port Ken Silverman's Build ENGINE.C movement /
geometry functions into src/core/build_engine.c (portable C, SH-2 friendly).

"Build Engine & Tools" Copyright (c) 1993-1997 Ken Silverman.  Used under the
Build license (non-commercial use, credit given).  The renderer is NOT
extracted here - it is rewritten in src/core/render_impl.c.
"""
import re
import sys

SRC = sys.argv[1] if len(sys.argv) > 1 else "/tmp/engine.c"
OUT = sys.argv[2] if len(sys.argv) > 2 else "src/core/build_engine.c"

lines = open(SRC, encoding="latin1").read().split("\n")

# (first line of definition [1-based], return type)
FUNCS = [
    ("clipinsidebox(long x", "int32_t"),
    ("clipinsideboxline(long x", "int32_t"),
    ("inside (long x", "int32_t"),
    ("getangle(long xvect", "int32_t"),
    ("setsprite(short spritenum", "int32_t"),
    ("animateoffs(short tilenum", "int32_t"),
    ("initspritelists()", "void"),
    ("insertsprite(short sectnum", "int32_t"),
    ("insertspritesect(short sectnum", "int32_t"),
    ("insertspritestat(short statnum", "int32_t"),
    ("deletesprite(short spritenum", "int32_t"),
    ("deletespritesect(short deleteme", "int32_t"),
    ("deletespritestat (short deleteme", "int32_t"),
    ("changespritesect(short spritenum", "int32_t"),
    ("changespritestat(short spritenum", "int32_t"),
    ("nextsectorneighborz(short sectnum", "int32_t"),
    ("cansee(long x1", "int32_t"),
    ("hitscan(long xs", "int32_t"),
    ("neartag (long xs", "int32_t"),
    ("lintersect(long x1", "int32_t"),
    ("rintersect(long x1", "int32_t"),
    ("dragpoint(short pointhighlight", "void"),
    ("lastwall(short point", "int32_t"),
    ("clipmove (long *x", "int32_t"),
    ("keepaway (long *x", "void"),
    ("raytrace (long x3", "int32_t"),
    ("pushmove (long *x", "int32_t"),
    ("updatesector(long x", "void"),
    ("rotatepoint(long xpivot", "void"),
    ("krand()", "int32_t"),
    ("getzrange(long x", "void"),
    ("sectorofwall(short theline", "int32_t"),
    ("getceilzofslope(short sectnum", "int32_t"),
    ("getflorzofslope(short sectnum", "int32_t"),
    ("getzsofslope(short sectnum", "void"),
]


def find_func(sig):
    for i, l in enumerate(lines):
        if l.startswith(sig):
            return i
    raise SystemExit("not found: " + sig)


def func_body(start):
    """Return lines of the function starting at index start (until braces balance)."""
    depth = 0
    seen = False
    out = []
    i = start
    while True:
        l = lines[i]
        out.append(l)
        depth += l.count("{") - l.count("}")
        if "{" in l:
            seen = True
        if seen and depth == 0:
            break
        i += 1
    return out


def transform(text):
    t = text
    t = t.replace("unsigned long", "uint32_t")
    t = re.sub(r"\blong\b", "int32_t", t)
    t = re.sub(r"\bunsigned short\b", "uint16_t", t)
    t = re.sub(r"\bshort\b", "int16_t", t)
    t = re.sub(r"\bsigned char\b", "int8_t", t)
    # ROM-resident wall fields
    t = re.sub(r"wall\[([^\[\]]+(?:\[[^\[\]]*\][^\[\]]*)*)\]\.(lotag|hitag|nextwall|extra)",
               r"wallx[\1].\2", t)
    t = re.sub(r"\b(wal|wal2|wal3)->(lotag|hitag|nextwall|extra)\b", r"wallx[\1 - wall].\2", t)
    # tile tables
    t = re.sub(r"tilesizx\[([^\]]+)\]", r"TILESIZX(\1)", t)
    t = re.sub(r"tilesizy\[([^\]]+)\]", r"TILESIZY(\1)", t)
    t = re.sub(r"picanm\[([^\]]+)\]", r"PICANM(\1)", t)
    t = t.replace("nsqrtasm(", "ksqrt(").replace("krecipasm(", "krecip(")
    return t


out = []
out.append("""/*
 * build_engine.c - movement & geometry functions of Ken Silverman's Build
 * engine, mechanically ported from ENGINE.C by tools/extract_engine.py.
 *
 * "Build Engine & Tools" Copyright (c) 1993-1997 Ken Silverman
 * Ken Silverman's official web site: "http://www.advsys.net/ken"
 * Used under the Build license: non-commercial use, credit given.
 *
 * DO NOT EDIT - regenerate with: python3 tools/extract_engine.py
 */
#include "engine.h"

#pragma GCC diagnostic ignored "-Wparentheses"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
""")

for sig, rtype in FUNCS:
    i = find_func(sig)
    body = func_body(i)
    # multi-line K&R signatures: prepend the return type
    body[0] = rtype + " " + body[0].strip()
    # the clipmove helpers that precede clipmove
    text = transform("\n".join(body))
    out.append(text + "\n")

src = "\n".join(out)
open(OUT, "w").write(src)
print(f"wrote {OUT}: {len(src.splitlines())} lines, {len(FUNCS)} functions")
