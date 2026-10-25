"""Probe Riot .tex headers to work out the layout empirically."""
import os
import struct
import sys


def mip_chain(width, height, bytes_per_block):
    total = 0
    while True:
        total += ((width + 3) // 4) * ((height + 3) // 4) * bytes_per_block
        if width == 1 and height == 1:
            break
        width = max(1, width // 2)
        height = max(1, height // 2)
    return total


def main(paths):
    for path in paths:
        if not os.path.exists(path):
            print("missing:", path)
            continue
        data = open(path, "rb").read()
        width, height = struct.unpack_from("<HH", data, 4)
        word = struct.unpack_from("<I", data, 8)[0]
        print(f"{os.path.basename(path)}: size={len(data)} w={width} h={height} "
              f"header[8:12]={data[8:12].hex(' ')} u32={word:#010x}")
        for name, bpb in (("BC1", 8), ("BC3/BC7", 16)):
            chain = mip_chain(width, height, bpb)
            print(f"   {name}: chain={chain} implied header={len(data) - chain}")
        # BC3 block sanity at the implied 12-byte offset: alpha endpoints first.
        if len(data) > 28:
            print(f"   first 16 payload bytes @12: {data[12:28].hex(' ')}")


if __name__ == "__main__":
    main(sys.argv[1:])
