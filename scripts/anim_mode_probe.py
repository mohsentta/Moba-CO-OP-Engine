"""Check whether an ANM clip stores absolute local transforms (like Idle1) or
offsets relative to the bind pose (additive)."""
import struct
import sys


def load_skl(path):
    d = open(path, "rb").read()
    u16 = lambda o: struct.unpack_from("<H", d, o)[0]
    i16 = lambda o: struct.unpack_from("<h", d, o)[0]
    u32 = lambda o: struct.unpack_from("<I", d, o)[0]
    i32 = lambda o: struct.unpack_from("<i", d, o)[0]
    f32 = lambda o: struct.unpack_from("<f", d, o)[0]
    vec3 = lambda o: (f32(o), f32(o + 4), f32(o + 8))

    def cstr(o):
        end = o
        while end < len(d) and d[end] != 0:
            end += 1
        return d[o:end].decode("ascii", "replace")

    joints = []
    for i in range(u16(0x0E)):
        o = i32(0x14) + i * 100
        joints.append({"name": cstr(o + 96 + i32(o + 96)), "parent": i16(o + 4),
                       "hash": u32(o + 8), "t": vec3(o + 16)})
    return joints


def load_anm(path):
    d = open(path, "rb").read()
    u32 = lambda o: struct.unpack_from("<I", d, o)[0]
    i32 = lambda o: struct.unpack_from("<i", d, o)[0]
    f32 = lambda o: struct.unpack_from("<f", d, o)[0]
    tracks, frames = u32(0x1C), u32(0x20)
    hashes_off, vector_off, quat_off, frames_off = i32(0x28), i32(0x34), i32(0x38), i32(0x3C)
    hashes = [u32(hashes_off + 12 + i * 4) for i in range((frames_off - hashes_off) // 4)]
    first = {}
    for t in range(min(tracks, len(hashes))):
        o = frames_off + 12 + t * 6
        ti = struct.unpack_from("<H", d, o)[0]
        first[hashes[t]] = (vector_off + 12 + ti * 12, d)
    return first, d


def main(skl_path, anm_path):
    joints = load_skl(skl_path)
    first, d = load_anm(anm_path)
    f32 = lambda src, o: struct.unpack_from("<f", src, o)[0]
    vec3 = lambda src, o: (f32(src, o), f32(src, o + 4), f32(src, o + 8))

    matched = 0
    total = 0
    worst = []
    for j in joints:
        if j["hash"] not in first:
            continue
        off, src = first[j["hash"]]
        t = vec3(src, off)
        total += 1
        dist = sum((a - b) ** 2 for a, b in zip(t, j["t"])) ** 0.5
        if dist < 1.0:
            matched += 1
        else:
            worst.append((dist, j["name"], t, j["t"]))

    print(f"{anm_path}: {matched}/{total} joints match bind-local at frame 0")
    worst.sort(reverse=True)
    for dist, name, t, b in worst[:6]:
        print(f"   {name[:26]:<27} dist={dist:8.1f} anim=({t[0]:8.2f},{t[1]:8.2f},{t[2]:8.2f}) "
              f"bind=({b[0]:8.2f},{b[1]:8.2f},{b[2]:8.2f})")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
