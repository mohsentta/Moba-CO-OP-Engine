"""Full linear walk of a .mapgeo v18 mesh table (with the v18 +4 tweak)."""
import struct
import sys


class R:
    def __init__(self, d):
        self.d = d
        self.o = 0

    def take(self, n):
        if self.o + n > len(self.d):
            raise EOFError(f"eof at {self.o} want {n}")
        b = self.d[self.o:self.o + n]
        self.o += n
        return b

    def u8(self): return self.take(1)[0]
    def u16(self): return struct.unpack("<H", self.take(2))[0]
    def u32(self): return struct.unpack("<I", self.take(4))[0]
    def i32(self): return struct.unpack("<i", self.take(4))[0]
    def f32(self): return struct.unpack("<f", self.take(4))[0]
    def s(self):
        n = self.u32()
        return self.take(n).decode("utf-8", "replace")


def main(path):
    d = open(path, "rb").read()
    r = R(d)
    r.take(4)
    version = r.u32()
    for _ in range(r.u32()):
        r.u32(); r.s()
    for _ in range(r.u32()):
        r.u32()
        elems = r.u32()
        for _ in range(elems):
            r.u32(); r.u32()
        r.take((15 - elems) * 8)
    vb = r.u32()
    for _ in range(vb):
        r.u8()
        r.take(r.u32())
    ib = r.u32()
    ib_sizes = []
    for _ in range(ib):
        r.u8()
        n = r.i32()
        ib_sizes.append(n)
        r.take(n)
    meshes = r.u32()
    print(f"version {version}: vb={vb} ib={ib} meshes={meshes} at 0x{r.o:x}")

    verts = tris = 0
    bad = 0
    mats = {}
    for m in range(meshes):
        vcount = r.i32()
        streams = r.u32()
        base = r.i32()
        ids = [r.i32() for _ in range(streams)]
        icount = r.u32()
        ib_id = r.i32()
        r.u8()                       # visibility
        if version >= 15:
            r.u32()                  # visibility controller hash
        if version >= 18:
            r.u32()                  # v18 extra field
        sub_count = r.u32()
        for _ in range(sub_count):
            r.u32()                  # material hash
            name = r.s()
            mats[name] = mats.get(name, 0) + 1
            r.take(16)               # start/count/min/max
        r.u8()                       # backface
        r.take(24)                   # aabb
        r.take(64)                   # transform
        r.u8()                       # quality
        r.u8()                       # transition
        r.take(2 if version >= 16 else 1)
        for _ in range(2):           # baked + stationary light channels
            r.s()
            r.take(16)
        if version >= 17:
            for _ in range(r.u32()):
                r.u32(); r.s()
            r.take(16)
        if m < 3 or m == meshes - 1:
            print(f"  mesh[{m}] verts={vcount} tris={icount//3} vb={ids} ib={ib_id} subs={sub_count}")
        verts += vcount
        tris += icount // 3

    print(f"walk ended at 0x{r.o:x} of 0x{len(d):x}; vertices={verts:,} triangles={tris:,}")
    print(f"distinct materials: {len(mats)}")
    for name, count in list(sorted(mats.items(), key=lambda kv: -kv[1]))[:6]:
        print(f"   {count:4d}  {name}")


if __name__ == "__main__":
    main(sys.argv[1])
