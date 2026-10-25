"""Instrumented TOC walk over Map11.wad to find why parsing stops early."""
import struct
import sys

TOC = 32


def parse_entry(b, o):
    if o + TOC > len(b):
        return None
    h, off, cs, us = struct.unpack_from("<QIII", b, o)
    return {
        "hash": h, "off": off, "cs": cs, "us": us,
        "comp": b[o + 20], "dup": b[o + 21],
    }


def walk(b, start, limit=1_000_000):
    out = []
    o = start
    while len(out) < limit:
        e = parse_entry(b, o)
        if e is None:
            return out, ("eof", o, None)
        if e["comp"] > 0x40:
            return out, ("comp", o, e)
        if e["off"] < o + TOC and e["off"] != 0:
            return out, ("offback", o, e)
        out.append(e)
        o += TOC
    return out, ("limit", o, None)


def main(path):
    size = __import__("os").path.getsize(path)
    with open(path, "rb") as handle:
        head = handle.read(2 * 1024 * 1024)
    print(f"file size {size:,} bytes; read prefix {len(head):,}")

    best = None
    for start in range(16, 4096, 16):
        entries, reason = walk(head, start)
        if not entries:
            continue
        min_off = min(e["off"] for e in entries)
        count = (min_off - start) // TOC if min_off > start else 0
        last = entries[-1]
        tail = last["off"] + last["cs"]
        score = min(len(entries), count) + (1_000_000 if tail == size else 0)
        if best is None or score > best[0]:
            best = (score, start, entries, reason, min_off, count, tail)

    score, start, entries, reason, min_off, count, tail = best
    print(f"best start=0x{start:x} walked={len(entries)} min_off={min_off:,} "
          f"implied_count={count} last_tail={tail:,} (file {size:,})")
    print(f"walk stopped: {reason[0]} at 0x{reason[1]:x}")
    if reason[2]:
        print("  offending record:", reason[2])
        o = reason[1]
        print("  raw:", head[o:o + 32].hex(" "))
    # Show the record that the implied count would end at
    end = start + count * TOC
    print(f"implied TOC end 0x{end:x} (index {count})")
    e = parse_entry(head, end)
    print("  entry at implied end:", e)
    print("  neighbour before:", parse_entry(head, end - TOC))


if __name__ == "__main__":
    main(sys.argv[1])
