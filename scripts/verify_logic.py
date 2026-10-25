"""Replica of the C++ core logic, used to validate the port before a compiler exists.

Mirrors src/core/xxhash64.cpp and src/formats/wad.cpp line for line so the
algorithm (not the syntax) can be checked against the Rust reference behaviour.
No game data is read; the archive under test is synthetic.
"""
import struct

P1 = 0x9E3779B185EBCA87
P2 = 0xC2B2AE3D27D4EB4F
P3 = 0x165667B19E3779F9
P4 = 0x85EBCA77C2B2AE63
P5 = 0x27D4EB2F165667C5
M = (1 << 64) - 1


def rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & M


def rnd(acc, inp):
    acc = (acc + inp * P2) & M
    acc = rotl(acc, 31)
    return (acc * P1) & M


def merge_round(acc, val):
    val = rnd(0, val)
    acc ^= val
    return (acc * P1 + P4) & M


def xxh64(data: bytes, seed=0):
    n = len(data)
    p = 0
    if n >= 32:
        limit = n - 32
        v1 = (seed + P1 + P2) & M
        v2 = (seed + P2) & M
        v3 = seed & M
        v4 = (seed - P1) & M
        while p <= limit:
            v1 = rnd(v1, struct.unpack_from('<Q', data, p)[0]); p += 8
            v2 = rnd(v2, struct.unpack_from('<Q', data, p)[0]); p += 8
            v3 = rnd(v3, struct.unpack_from('<Q', data, p)[0]); p += 8
            v4 = rnd(v4, struct.unpack_from('<Q', data, p)[0]); p += 8
        h = (rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18)) & M
        h = merge_round(h, v1)
        h = merge_round(h, v2)
        h = merge_round(h, v3)
        h = merge_round(h, v4)
    else:
        h = (seed + P5) & M

    h = (h + n) & M
    while p + 8 <= n:
        h ^= rnd(0, struct.unpack_from('<Q', data, p)[0])
        h = (rotl(h, 27) * P1 + P4) & M
        p += 8
    if p + 4 <= n:
        h ^= (struct.unpack_from('<I', data, p)[0] * P1) & M
        h = (rotl(h, 23) * P2 + P3) & M
        p += 4
    while p < n:
        h ^= (data[p] * P5) & M
        h = (rotl(h, 11) * P1) & M
        p += 1
    h ^= h >> 33
    h = (h * P2) & M
    h ^= h >> 29
    h = (h * P3) & M
    h ^= h >> 32
    return h


def norm(p: str) -> str:
    return p.replace('\\', '/').lower()


def path_hash(p: str) -> int:
    return xxh64(norm(p).encode())


# ---------------- WAD scanner (port of src/formats/wad.cpp) ----------------
TOC = 32


def parse_entry(b, o):
    if o + TOC > len(b):
        return None
    h, off, cs, us = struct.unpack_from('<QIII', b, o)
    return dict(hash=h, offset=off, csize=cs, usize=us,
                comp=b[o + 20], dup=b[o + 21], checksum=struct.unpack_from('<Q', b, o + 24)[0])


def known(c):
    return c in (0, 1, 2, 3, 4, 0x24, 0x34)


def walk(b, start):
    out, o = [], start
    while True:
        e = parse_entry(b, o)
        if e is None: break
        if not known(e['comp']): break
        if e['csize'] == 0 or e['usize'] == 0: break
        if e['offset'] + e['csize'] > len(b): break
        if e['offset'] < o + TOC: break
        out.append(e)
        o += TOC
    return out


def open_wad(b):
    assert len(b) >= 16 and b[0:2] == b'RW'
    best = None
    for start in range(16, min(4096, len(b)), 16):
        entries = walk(b, start)
        if not entries: continue
        n = len(entries)
        while n > 0:
            min_off = min(e['offset'] for e in entries[:n])
            allowed = (min_off - start if min_off > start else 0) // TOC
            if allowed >= n: break
            n = allowed
        entries = entries[:n]
        if not entries: continue
        toc_end = start + len(entries) * TOC
        if any(e['offset'] < toc_end for e in entries): continue
        last = entries[-1]
        tail = last['offset'] + last['csize']
        score = len(entries) + (1_000_000 if tail == len(b) else 0)
        if best is None or score > best[2]:
            best = (start, entries, score)
    return best


def make_archive():
    b = bytearray(100)
    b[0:2] = b'RW'
    struct.pack_into('<QIII', b, 16, 0x1111111111111111, 80, 10, 10)
    b[36] = 0
    struct.pack_into('<QIII', b, 48, 0x2222222222222222, 90, 10, 10)
    b[68] = 0
    for i in range(80, 100):
        b[i] = i
    return bytes(b)


def main():
    fails = 0
    def check(label, cond):
        nonlocal fails
        print(('PASS ' if cond else 'FAIL ') + label)
        if not cond: fails += 1

    check('xxh64("") == EF46DB3751D8E999', xxh64(b'') == 0xEF46DB3751D8E999)
    check('xxh64("hello") == 26C7827D889F6DA3', xxh64(b'hello') == 0x26C7827D889F6DA3)
    check('xxh64 32-byte block path executes', xxh64(b'a' * 64) != 0)
    check('path hash is case/separator insensitive',
          path_hash('DATA\\A.bin') == path_hash('data/a.bin'))
    check('different paths differ', path_hash('x.skn') != path_hash('y.skn'))

    best = open_wad(make_archive())
    check('synthetic archive opens', best is not None)
    if best:
        start, entries, score = best
        check('TOC offset 16', start == 16)
        check('two entries', len(entries) == 2)
        check('tail touches EOF (score bonus)', score >= 1_000_002)
        check('second entry offset 90', entries[1]['offset'] == 90)

    bad = b'not-a-wad-at-all'
    try:
        open_wad(bad); ok = False
    except AssertionError:
        ok = True
    check('non-RW input rejected', ok)

    print(f'\n{fails} failure(s)')
    return 1 if fails else 0


if __name__ == '__main__':
    raise SystemExit(main())
