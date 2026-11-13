"""Tiny LZ4 *block* format encoder (greedy, hash-chained) + reference decoder.

The SH-2 side decodes with src/core/lz4.c.  Format per sequence:
  token (hi nibble literal len, lo nibble match len-4), [lit len ext], literals,
  offset (u16 little-endian), [match len ext].  Last sequence = literals only.
"""

MINMATCH = 4
MAX_DIST = 65535


def _put_len(out, n):
    while n >= 255:
        out.append(255)
        n -= 255
    out.append(n)


def lz4_compress(src: bytes) -> bytes:
    n = len(src)
    out = bytearray()
    table = {}
    anchor = 0
    i = 0
    limit = n - 12          # LZ4 rule: last 5 bytes are literals, last match >= 12 from end
    while i < limit:
        key = src[i:i + 4]
        cand = table.get(key)
        table[key] = i
        if cand is None or i - cand > MAX_DIST:
            i += 1
            continue
        # extend match
        m = 4
        maxm = n - 5 - i
        while m < maxm and src[cand + m] == src[i + m]:
            m += 1
        # extend backwards
        while i > anchor and cand > 0 and src[i - 1] == src[cand - 1]:
            i -= 1
            cand -= 1
            m += 1
        lit = i - anchor
        tok_l = 15 if lit >= 15 else lit
        ml = m - MINMATCH
        tok_m = 15 if ml >= 15 else ml
        out.append((tok_l << 4) | tok_m)
        if lit >= 15:
            _put_len(out, lit - 15)
        out += src[anchor:i]
        off = i - cand
        out.append(off & 255)
        out.append(off >> 8)
        if ml >= 15:
            _put_len(out, ml - 15)
        # index a few positions inside the match for better ratio
        end = i + m
        j = i + 1
        while j < end and j < limit:
            table[src[j:j + 4]] = j
            j += 2
        i = end
        anchor = i
    lit = n - anchor
    out.append((15 if lit >= 15 else lit) << 4)
    if lit >= 15:
        _put_len(out, lit - 15)
    out += src[anchor:]
    return bytes(out)


def lz4_decompress(src: bytes, rawlen: int) -> bytes:
    out = bytearray()
    i = 0
    while i < len(src):
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1
                lit += b
                if b != 255:
                    break
        out += src[i:i + lit]; i += lit
        if i >= len(src):
            break
        off = src[i] | (src[i + 1] << 8); i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1
                ml += b
                if b != 255:
                    break
        ml += MINMATCH
        start = len(out) - off
        for k in range(ml):
            out.append(out[start + k])
    assert len(out) == rawlen, (len(out), rawlen)
    return bytes(out)


if __name__ == "__main__":
    import os
    data = os.urandom(1000) + b"abcd" * 5000 + bytes(range(256)) * 20
    c = lz4_compress(data)
    assert lz4_decompress(c, len(data)) == data
    print("lz4 ok", len(data), "->", len(c))
