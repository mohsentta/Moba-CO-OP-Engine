"""Step-through of the .mapgeo layout to find where parsing diverges."""
import struct
import sys


class R:
    def __init__(self, data):
        self.d = data
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
    print("magic:", r.take(4))
    version = r.u32()
    print("version:", version)
    n = r.u32()
    print("shader overrides:", n)
    for i in range(min(n, 4)):
        sampler = r.u32()
        print(f"   [{i}] sampler={sampler} tex={r.s()!r}")

    decl_count = r.u32()
    print("declarations:", decl_count, "at 0x%x" % r.o)
    for i in range(decl_count):
        usage = r.u32()
        elems = r.u32()
        print(f"   decl[{i}] usage={usage} elements={elems}")
        for e in range(elems):
            name = r.u32(); fmt = r.u32()
            print(f"        name={name} format={fmt}")
        r.take((15 - elems) * 8)

    vb_count = r.u32()
    print("vertex buffers:", vb_count, "at 0x%x" % r.o)
    for i in range(vb_count):
        flag = r.u8()
        size = r.u32()
        if i < 4:
            print(f"   vb[{i}] flag={flag} size={size}")
        r.take(size)
    print("   vb table ends at 0x%x" % r.o)

    ib_count = r.u32()
    print("index buffers:", ib_count, "at 0x%x" % r.o)
    for i in range(ib_count):
        flag = r.u8()
        size = r.i32()
        if i < 4:
            print(f"   ib[{i}] flag={flag} size={size}")
        r.take(size)
    print("   ib table ends at 0x%x" % r.o)

    mesh_count = r.u32()
    print("meshes:", mesh_count, "at 0x%x" % r.o)
    vcount = r.i32(); streams = r.u32(); base = r.i32()
    print(f"   mesh[0] vertex_count={vcount} streams={streams} base_decl={base}")
    ids = [r.i32() for _ in range(streams)]
    print(f"   vb ids={ids}")
    icount = r.u32(); ib_id = r.i32()
    print(f"   index_count={icount} ib_id={ib_id}")
    vis = r.u8()
    print(f"   visibility={vis} at 0x%x" % r.o)
    if version >= 15:
        print("   controller hash:", hex(r.u32()))
    sub_count = r.u32()
    print("   submeshes:", sub_count)
    for i in range(min(sub_count, 4)):
        h = r.u32(); mat = r.s(); start = r.i32(); cnt = r.i32(); mn = r.i32(); mx = r.i32()
        print(f"      [{i}] hash={h:08x} mat={mat!r} start={start} count={cnt} min={mn} max={mx}")
    print("   backface:", r.u8())
    aabb = [round(r.f32(), 1) for _ in range(6)]
    print("   aabb:", aabb)
    mat = [round(r.f32(), 2) for _ in range(16)]
    print("   transform:", mat)
    print("   quality:", r.u8())
    print("   transition:", r.u8())
    print("   render flags:", r.u16())
    tex1 = r.s(); sc1 = [round(r.f32(), 2) for _ in range(4)]
    print(f"   baked light tex={tex1!r} scale/bias={sc1}")
    tex2 = r.s(); sc2 = [round(r.f32(), 2) for _ in range(4)]
    print(f"   stationary tex={tex2!r} scale/bias={sc2}")
    n_over = r.u32()
    print("   mesh overrides:", n_over)
    for i in range(min(n_over, 4)):
        print("      sampler", r.u32(), "tex", r.s()[:60])
    print("   baked scale/bias:", [round(r.f32(), 2) for _ in range(4)])
    print("   next mesh starts at 0x%x" % r.o)


if __name__ == "__main__":
    main(sys.argv[1])
