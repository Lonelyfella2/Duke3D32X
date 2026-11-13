"""Creative VOC / RIFF WAV loader and IMA ADPCM encoder (mono)."""
import struct
import numpy as np


def load_sound(data: bytes):
    """Return (rate, int16 numpy samples) or None."""
    if data[:19] == b"Creative Voice File":
        return _load_voc(data)
    if data[:4] == b"RIFF" and data[8:12] == b"WAVE":
        return _load_wav(data)
    return None


def _load_voc(d):
    off = struct.unpack_from("<H", d, 20)[0]
    rate = 11025
    chunks = []
    while off < len(d):
        typ = d[off]
        if typ == 0:
            break
        blen = d[off + 1] | (d[off + 2] << 8) | (d[off + 3] << 16)
        body = d[off + 4: off + 4 + blen]
        off += 4 + blen
        if typ == 1:
            div, codec = body[0], body[1]
            rate = int(round(1000000 / (256 - div)))
            if codec == 0:
                chunks.append(np.frombuffer(body[2:], dtype=np.uint8).astype(np.int16) - 128)
        elif typ == 2:
            chunks.append(np.frombuffer(body, dtype=np.uint8).astype(np.int16) - 128)
        elif typ == 3:
            n = struct.unpack_from("<H", body, 0)[0] + 1
            chunks.append(np.zeros(n, dtype=np.int16))
        elif typ == 9:
            r, bits, ch, codec = struct.unpack_from("<IBBH", body, 0)
            rate = r
            raw = body[12:]
            if bits == 8:
                s = np.frombuffer(raw, dtype=np.uint8).astype(np.int16) - 128
            else:
                s = (np.frombuffer(raw[:len(raw) // 2 * 2], dtype="<i2") >> 8).astype(np.int16)
            if ch == 2:
                s = s[0::2]
            chunks.append(s)
    if not chunks:
        return None
    s = np.concatenate(chunks)
    return rate, (s.astype(np.int32) << 8).clip(-32768, 32767).astype(np.int16)


def _load_wav(d):
    off = 12
    fmt = None
    data = None
    while off + 8 <= len(d):
        cid = d[off:off + 4]
        clen = struct.unpack_from("<I", d, off + 4)[0]
        body = d[off + 8: off + 8 + clen]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", body, 0)
        elif cid == b"data":
            data = body
        off += 8 + clen + (clen & 1)
    if not fmt or data is None or fmt[0] != 1:
        return None
    _, ch, rate, _, _, bits = fmt
    if bits == 8:
        s = (np.frombuffer(data, dtype=np.uint8).astype(np.int32) - 128) << 8
    else:
        s = np.frombuffer(data[:len(data) // 2 * 2], dtype="<i2").astype(np.int32)
    if ch == 2:
        s = (s[0::2] + s[1::2]) >> 1
    return rate, s.clip(-32768, 32767).astype(np.int16)


def resample(samples, rate, target):
    if rate == target or len(samples) == 0:
        return samples
    n = int(len(samples) * target / rate)
    x = np.linspace(0, len(samples) - 1, n)
    return np.interp(x, np.arange(len(samples)), samples.astype(np.float64)).astype(np.int16)


STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
        253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
        3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
        12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


def ima_encode(samples):
    """Encode int16 mono -> bytes: [s16 BE predictor][u8 index][u8 0] + nibbles.

    Nibble order: high nibble first (sample 2k), low nibble second (2k+1).
    The predictor header is big-endian; the converter rewrites it per target.
    """
    pred = int(samples[0]) if len(samples) else 0
    idx = 0
    # pick a starting index that matches the initial slope
    if len(samples) > 8:
        d = int(np.abs(np.diff(samples[:16].astype(np.int32))).mean())
        while idx < 88 and STEP[idx] < d:
            idx += 1
    start_pred, start_idx = pred, idx
    nibbles = []
    for s in samples:
        s = int(s)
        step = STEP[idx]
        diff = s - pred
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        delta = step >> 3
        if diff >= step:
            code |= 4; diff -= step; delta += step
        step >>= 1
        if diff >= step:
            code |= 2; diff -= step; delta += step
        step >>= 1
        if diff >= step:
            code |= 1; delta += step
        if code & 8:
            pred -= delta
        else:
            pred += delta
        pred = max(-32768, min(32767, pred))
        idx = max(0, min(88, idx + INDEX[code & 7]))
        nibbles.append(code)
    if len(nibbles) & 1:
        nibbles.append(0)
    out = bytearray(struct.pack(">hBB", start_pred, start_idx, 0))
    for i in range(0, len(nibbles), 2):
        out.append((nibbles[i] << 4) | nibbles[i + 1])
    return bytes(out), start_pred, start_idx


def ima_decode(blob, n):
    pred, idx = struct.unpack_from(">hB", blob, 0)
    out = np.zeros(n, dtype=np.int16)
    for i in range(n):
        b = blob[4 + (i >> 1)]
        code = (b >> 4) if (i & 1) == 0 else (b & 15)
        step = STEP[idx]
        delta = step >> 3
        if code & 4: delta += step
        if code & 2: delta += step >> 1
        if code & 1: delta += step >> 2
        pred = pred - delta if code & 8 else pred + delta
        pred = max(-32768, min(32767, pred))
        idx = max(0, min(88, idx + INDEX[code & 7]))
        out[i] = pred
    return out
