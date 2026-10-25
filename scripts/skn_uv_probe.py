"""Check the UV (and normal) ranges in a decoded .skn."""
import struct
import sys


def main(path):
    data = open(path, "rb").read()
    magic, major, minor, mats = struct.unpack_from("<IHHI", data, 0)
    assert magic == 0x00112233, hex(magic)
    offset = 12 + mats * 80
    unk0, index_count, vertex_count, stride, unk1 = struct.unpack_from("<5I", data, offset)
    offset += 20 + 40
    offset += index_count * 2

    uvs, normals = [], []
    pos_min = [1e9] * 3
    pos_max = [-1e9] * 3
    for _ in range(vertex_count):
        px, py, pz = struct.unpack_from("<3f", data, offset)
        bones = data[offset + 12:offset + 16]
        weights = struct.unpack_from("<4f", data, offset + 16)
        nx, ny, nz = struct.unpack_from("<3f", data, offset + 32)
        u, v = struct.unpack_from("<2f", data, offset + 44)
        uvs.append((u, v))
        normals.append((nx, ny, nz))
        for k, val in enumerate((px, py, pz)):
            pos_min[k] = min(pos_min[k], val)
            pos_max[k] = max(pos_max[k], val)
        offset += stride

    us = [u for u, _ in uvs]
    vs = [v for _, v in uvs]
    print(f"{path}: verts={vertex_count} stride={stride} version={major}.{minor}")
    print(f"  uv u: [{min(us):.4f}, {max(us):.4f}]   uv v: [{min(vs):.4f}, {max(vs):.4f}]")
    print(f"  pos : [{pos_min[0]:.2f},{pos_min[1]:.2f},{pos_min[2]:.2f}] .. "
          f"[{pos_max[0]:.2f},{pos_max[1]:.2f},{pos_max[2]:.2f}]")
    lens = [(n[0] ** 2 + n[1] ** 2 + n[2] ** 2) ** 0.5 for n in normals[:2000]]
    print(f"  normal length: min={min(lens):.3f} max={max(lens):.3f} "
          f"avg={sum(lens)/len(lens):.3f}")
    print(f"  weights[0] sample: {[round(weights[0],3) for weights in [struct.unpack_from('<4f', data, 12 + mats*80 + 20 + 40 + index_count*2 + i*stride + 16) for i in range(3)]]}")


if __name__ == "__main__":
    main(sys.argv[1])
