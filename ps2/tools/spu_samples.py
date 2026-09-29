#!/usr/bin/env python3
"""Convert the game's instrument/SFX samples to SPU2 PS-ADPCM.

The N64 synthesizer streams VADPCM (or raw 16-bit) samples out of the .tbl
files and decodes them on the RSP. On the PS2 the SPU2 plays samples from its
own RAM in PS-ADPCM, so every wavetable referenced by the two instrument banks
is decoded here and re-encoded once, offline.

PS-ADPCM works in 28-sample blocks and can only loop on block boundaries,
so looped samples are adjusted:
  * the loop body is unrolled until it is long enough (>= LOOP_MIN samples),
  * the whole sample is resampled by a factor r (~1) that makes the unrolled
    loop an exact number of blocks (the runtime multiplies the pitch by r),
  * silence is prepended so the loop starts on a block boundary (< 1 ms).

Output (virtual ROM region PS2_SPU_SAMPLES_VROM, see ps2/include/ps2/assetpack.h):
    header  : "SPUS", u32 version, u32 count, u32 entries_offset
    entries : sorted by (key, len):
              u32 key       ROM address of the wave data (wav->base at run time)
              u32 len       wav->len (bytes of source data)
              u32 loop_start, loop_end  (source samples; 0,0 if not looped)
              u32 data_off  offset of the PS-ADPCM data in this region
              u32 data_size bytes (multiple of 64)
              f32 pitch_scale
              u32 flags     bit0 looped
    data    : PS-ADPCM blocks, each sample 64-byte aligned
All fields little-endian.
"""
import struct
import sys
from concurrent.futures import ProcessPoolExecutor
from concurrent.futures.process import BrokenProcessPool

import numpy as np

MAX_WORKERS = 4
SPU_MAGIC = b"SPUS"
SPU_VERSION = 1
LOOP_MIN = 28 * 50      # unrolled loop length target: resample error <= 0.5 %
AL_ADPCM_WAVE = 0
AL_RAW16_WAVE = 1

# (ctl start, ctl end, tbl start) in the US ROM; the tbl follows its ctl.
US_BANKS = [
    (0xB4E5C0, 0xB54CE0, 0xB54CE0),   # B1_sounds1: music instruments
    (0xC6B650, 0xC7B1F0, 0xC7B1F0),   # B1_sounds2: sound effects / voices
]


# --------------------------------------------------------------------------
# Bank parsing (big-endian ROM data, offsets relative to the .ctl file)
# --------------------------------------------------------------------------

def _u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def _s16(b, o):
    return struct.unpack_from(">h", b, o)[0]


def collect_waves(rom, banks=US_BANKS):
    """Return a list of unique wave descriptors (dicts)."""
    waves = {}
    for ctl_lo, ctl_hi, tbl in banks:
        ctl = rom[ctl_lo:ctl_hi]
        seen_inst = set()

        def wave_at(woff):
            base, length = _u32(ctl, woff), _u32(ctl, woff + 4)
            wtype = ctl[woff + 8]
            loop_off, book_off = _u32(ctl, woff + 12), _u32(ctl, woff + 16)
            w = dict(key=tbl + base, len=length, type=wtype, loop=None, book=None)
            if loop_off:
                s, e, c = struct.unpack_from(">III", ctl, loop_off)
                if c != 0 and e > s:
                    w["loop"] = (s, e, c)
            if wtype == AL_ADPCM_WAVE:
                order, npred = struct.unpack_from(">ii", ctl, book_off)
                n = order * npred * 8
                w["book"] = (order, npred, struct.unpack_from(">%dh" % n, ctl, book_off + 8))
            return w

        def inst(ioff):
            if ioff == 0 or ioff in seen_inst:
                return
            seen_inst.add(ioff)
            count = _s16(ctl, ioff + 14)
            for i in range(count):
                soff = _u32(ctl, ioff + 16 + 4 * i)
                woff = _u32(ctl, soff + 8)
                w = wave_at(woff)
                lk = (w["key"], w["len"], w["loop"][:2] if w["loop"] else (0, 0))
                waves.setdefault(lk, w)

        bank_count = _s16(ctl, 2)
        for b in range(bank_count):
            boff = _u32(ctl, 4 + 4 * b)
            inst_count = _s16(ctl, boff)
            inst(_u32(ctl, boff + 8))  # percussion
            for i in range(inst_count):
                inst(_u32(ctl, boff + 12 + 4 * i))
    return list(waves.values())


# --------------------------------------------------------------------------
# Decoding
# --------------------------------------------------------------------------

def _coef_table(order, npred, book):
    tables = []
    pos = 0
    for _ in range(npred):
        t = [[0] * (order + 8) for _ in range(8)]
        for j in range(order):
            for k in range(8):
                t[k][j] = book[pos]
                pos += 1
        for k in range(1, 8):
            t[k][order] = t[k - 1][order - 1]
        t[0][order] = 1 << 11
        for k in range(1, 8):
            for j in range(k):
                t[j][k + order] = 0
            for j in range(k, 8):
                t[j][k + order] = t[j - k][order]
        tables.append(t)
    return tables


def decode_vadpcm(data, order, npred, book):
    table = _coef_table(order, npred, book)
    nframes = len(data) // 9
    out = np.zeros(nframes * 16, dtype=np.int32)
    prev = [0] * 16
    for f in range(nframes):
        fr = data[f * 9:(f + 1) * 9]
        scale = 1 << (fr[0] >> 4)
        pred = fr[0] & 0xF
        ix = []
        for b in fr[1:]:
            for nib in (b >> 4, b & 0xF):
                ix.append((nib - 16 if nib >= 8 else nib) * scale)
        coefs = table[pred] if pred < len(table) else table[0]
        cur = [0] * 16
        for j in range(2):
            if j == 0:
                hist = prev[16 - order:16]
            else:
                hist = cur[8 - order:8]
            vec = hist + ix[j * 8:j * 8 + 8]
            for i in range(8):
                row = coefs[i]
                acc = 0
                for k in range(order + 8):
                    acc += row[k] * vec[k]
                v = acc >> 11          # floor division, like the SDK decoder
                if v > 32767:
                    v = 32767
                elif v < -32768:
                    v = -32768
                cur[j * 8 + i] = v
        out[f * 16:(f + 1) * 16] = cur
        prev = cur
    return out


def decode_wave(w, raw):
    if w["type"] == AL_ADPCM_WAVE:
        order, npred, book = w["book"]
        return decode_vadpcm(raw, order, npred, book)
    return np.frombuffer(bytes(raw[:len(raw) & ~1]), dtype=">i2").astype(np.int32)


# --------------------------------------------------------------------------
# Resampling (Catmull-Rom)
# --------------------------------------------------------------------------

def _resample(x, n_out, periodic=False):
    n_in = len(x)
    if n_out == n_in or n_in == 0:
        return x.astype(np.float64)
    pos = np.arange(n_out, dtype=np.float64) * (n_in / n_out)
    i = np.floor(pos).astype(np.int64)
    t = pos - i
    if periodic:
        idx = lambda k: np.mod(i + k, n_in)
    else:
        idx = lambda k: np.clip(i + k, 0, n_in - 1)
    xf = x.astype(np.float64)
    p0, p1, p2, p3 = xf[idx(-1)], xf[idx(0)], xf[idx(1)], xf[idx(2)]
    return p1 + 0.5 * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)))


# --------------------------------------------------------------------------
# PS-ADPCM encoding
# --------------------------------------------------------------------------

FILTERS = ((0, 0), (60, 0), (115, -52), (98, -55), (122, -60))


def _encode_block(block, h1, h2):
    """Return (header_byte, nibbles bytes, new h1, new h2)."""
    best = None
    for fi, (f0, f1) in enumerate(FILTERS):
        # estimate the residual range with the ideal history
        p1, p2 = h1, h2
        peak = 0
        for s in block:
            r = s - ((p1 * f0 + p2 * f1 + 32) >> 6)
            if r > peak:
                peak = r
            elif -r > peak:
                peak = -r
            p2, p1 = p1, s
        unit_bits = 0
        while (7 << unit_bits) < peak and unit_bits < 12:
            unit_bits += 1
        for ub in (unit_bits, unit_bits + 1) if unit_bits < 12 else (unit_bits,):
            shift = 12 - ub
            p1, p2 = h1, h2
            err = 0
            nibs = []
            half = (1 << ub) >> 1
            for s in block:
                pred = (p1 * f0 + p2 * f1 + 32) >> 6
                r = s - pred
                q = (r + half) >> ub if r >= 0 else -((-r + half) >> ub)
                if q > 7:
                    q = 7
                elif q < -8:
                    q = -8
                d = ((q << 12) >> shift) + pred
                if d > 32767:
                    d = 32767
                elif d < -32768:
                    d = -32768
                err += (s - d) * (s - d)
                nibs.append(q & 0xF)
                p2, p1 = p1, d
            if best is None or err < best[0]:
                best = (err, (fi << 4) | shift, nibs, p1, p2)
    _, hdr, nibs, n1, n2 = best
    packed = bytes(nibs[i] | (nibs[i + 1] << 4) for i in range(0, 28, 2))
    return hdr, packed, n1, n2


def encode_psadpcm(pcm, loop_block=None):
    """pcm: int sequence, length multiple of 28. loop_block: index of the
    block the loop starts at (None = one-shot). Returns bytes padded to 64."""
    nblocks = len(pcm) // 28
    out = bytearray()
    h1 = h2 = 0
    for b in range(nblocks):
        hdr, packed, h1, h2 = _encode_block(pcm[b * 28:(b + 1) * 28], h1, h2)
        flags = 0
        if loop_block is None:
            if b == 0:
                flags |= 0x04           # loop start = sample start (sane LSA)
            if b == nblocks - 1:
                flags |= 0x01           # end: voice goes silent
        else:
            if b == loop_block:
                flags |= 0x04
            if b == nblocks - 1:
                flags |= 0x03           # end + repeat from loop start
        out += bytes((hdr, flags)) + packed
    while len(out) % 64:
        out += bytes((0, 0x00)) + bytes(14)  # never played: after the end block
    return bytes(out)


def convert_wave(args):
    w, raw = args
    pcm = decode_wave(w, raw)
    if w["loop"]:
        ls, le, _count = w["loop"]
        le = min(le, len(pcm))
        ls = min(ls, le - 1)
        body = pcm[ls:le]
        k = 1 if len(body) >= LOOP_MIN else -(-LOOP_MIN // len(body))
        body = np.tile(body, k)
        n_loop = max(28, int(round(len(body) / 28.0)) * 28)
        r = n_loop / len(body)
        head = _resample(pcm[:ls], int(round(ls * r)))
        body_r = _resample(body, n_loop, periodic=True)
        pad = (-len(head)) % 28
        out = np.concatenate([np.zeros(pad), head, body_r])
        loop_block = (pad + len(head)) // 28
        pitch_scale = r
    else:
        out = pcm.astype(np.float64)
        pad = (-len(out)) % 28 if len(out) else 28
        out = np.concatenate([out, np.zeros(pad)])
        loop_block = None
        pitch_scale = 1.0
    ints = [int(v) for v in np.clip(np.round(out), -32768, 32767)]
    data = encode_psadpcm(ints, loop_block)
    return w, data, pitch_scale


def build(rom, jobs=8, log=print):
    waves = collect_waves(rom)
    waves.sort(key=lambda w: (w["key"], w["len"], w["loop"][:2] if w["loop"] else (0, 0)))
    # Each worker process imports numpy (~50 MB committed); keep the count
    # modest and fall back to converting in-process if workers cannot start.
    jobs = max(1, min(jobs, MAX_WORKERS))
    log("spu: converting %d samples to PS-ADPCM (%d workers)..." % (len(waves), jobs))
    work = [(w, bytes(rom[w["key"]:w["key"] + w["len"]])) for w in waves]
    try:
        with ProcessPoolExecutor(max_workers=jobs) as ex:
            results = list(ex.map(convert_wave, work, chunksize=4))
    except BrokenProcessPool:
        log("spu: worker processes failed to start (low memory?); converting serially")
        results = [convert_wave(item) for item in work]
    header_size = 16
    entry_size = 32
    data_start = (header_size + entry_size * len(results) + 63) & ~63
    entries = bytearray()
    data = bytearray()
    looped = 0
    for w, blob, pitch_scale in results:
        off = data_start + len(data)
        ls, le = (w["loop"][0], w["loop"][1]) if w["loop"] else (0, 0)
        looped += 1 if w["loop"] else 0
        entries += struct.pack("<IIIIIIfI", w["key"], w["len"], ls, le, off, len(blob), pitch_scale,
                               1 if w["loop"] else 0)
        data += blob
    head = SPU_MAGIC + struct.pack("<III", SPU_VERSION, len(results), header_size)
    region = bytearray(head + entries)
    region += bytes(data_start - len(region))
    region += data
    log("spu: %d samples (%d looped), %d KiB PS-ADPCM" % (len(results), looped, len(data) // 1024))
    return bytes(region)


if __name__ == "__main__":
    rom = open(sys.argv[1], "rb").read()
    blob = build(rom, jobs=int(sys.argv[3]) if len(sys.argv) > 3 else 8)
    open(sys.argv[2], "wb").write(blob)
