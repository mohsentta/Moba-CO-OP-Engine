"""Print printable string runs from a binary blob.

Used during format work to find asset paths inside decoded game data. It reads
only files you point it at; nothing is bundled or written.

    python scripts/asset_strings.py extracted/ahri.bin
    python scripts/asset_strings.py extracted/ahri.bin --min 8 --grep '\\.skn$'
"""
from __future__ import annotations

import argparse
import re
import sys

PRINTABLE = set(range(0x20, 0x7F))


def strings(data: bytes, min_len: int):
    start = None
    for i, byte in enumerate(data):
        if byte in PRINTABLE:
            if start is None:
                start = i
        else:
            if start is not None and i - start >= min_len:
                yield start, data[start:i].decode("ascii")
            start = None
    if start is not None and len(data) - start >= min_len:
        yield start, data[start:].decode("ascii")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("path")
    parser.add_argument("--min", type=int, default=6, dest="min_len")
    parser.add_argument("--grep", default=None, help="only show strings matching this regex")
    args = parser.parse_args()

    with open(args.path, "rb") as handle:
        data = handle.read()

    pattern = re.compile(args.grep) if args.grep else None
    shown = 0
    total = 0
    for offset, text in strings(data, args.min_len):
        total += 1
        if pattern and not pattern.search(text):
            continue
        shown += 1
        print(f"{offset:08x}  {text}")

    print(f"\n{shown} shown / {total} printable runs / {len(data)} bytes", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
