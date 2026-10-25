"""Compare SKL bind-local translations with ANM translations for the same joints."""
import struct

skl = open(r"_local/extracted/ahri_base.skl", "rb").read()
anm = open(r"_local/extracted/ahri_idle1.anm", "rb").read()

u16 = lambda b, o: struct.unpack_from("<H", b, o)[0]
i16 = lambda b, o: struct.unpack_from("<h", b, o)[0]
u32 = lambda b, o: struct.unpack_from("<I", b, o)[0]
i32 = lambda b, o: struct.unpack_from("<i", b, o)[0]
f32 = lambda b, o: struct.unpack_from("<f", b, o)[0]
vec3 = lambda b, o: (f32(b, o), f32(b, o + 4), f32(b, o + 8))


def cstr(b, o):
    end = o
    while end < len(b) and b[end] != 0:
        end += 1
    return b[o:end].decode("ascii", "replace")


joint_count = u16(skl, 0x0E)
joints_off = i32(skl, 0x14)
joints = []
for i in range(joint_count):
    o = joints_off + i * 100
    name_off = o + 96 + i32(skl, o + 96)
    joints.append({
        "name": cstr(skl, name_off),
        "parent": i16(skl, o + 4),
        "hash": u32(skl, o + 8),
        "t": vec3(skl, o + 16),
        "ib_t": vec3(skl, o + 56),
    })

# bind global translations (column-vector: child = parent * local)
glob = [(0.0, 0.0, 0.0)] * len(joints)
done = [False] * len(joints)


def build(i):
    if done[i]:
        return
    j = joints[i]
    if 0 <= j["parent"] < len(joints):
        build(j["parent"] if not done[j["parent"]] else j["parent"])
        p = glob[j["parent"]]
    else:
        p = (0.0, 0.0, 0.0)
    glob[i] = (p[0] + j["t"][0], p[1] + j["t"][1], p[2] + j["t"][2])
    done[i] = True


for i in range(len(joints)):
    build(i)

tracks = u32(anm, 0x1C)
frames = u32(anm, 0x20)
hashes_off, vector_off, quat_off, frames_off = i32(anm, 0x28), i32(anm, 0x34), i32(anm, 0x38), i32(anm, 0x3C)
vec_count = (quat_off - vector_off) // 12
hashes = [u32(anm, hashes_off + 12 + i * 4) for i in range((frames_off - hashes_off) // 4)]

frames_data_off = frames_off + 12
first_frame = {}
for t in range(tracks):
    o = frames_data_off + t * 6
    if t < len(hashes):
        ti = struct.unpack_from("<H", anm, o)[0]
        first_frame[hashes[t]] = vector_off + 12 + ti * 12

binding = {j["hash"]: (i, j) for i, j in enumerate(joints)}

print(f"{'joint':<22}{'parent':>7}{'bind local t':>34}{'bind global t':>34}{'anim t':>34}")
for i in range(0, min(12, len(joints))):
    j = joints[i]
    h = j["hash"]
    at = None
    if h in first_frame:
        at = vec3(anm, first_frame[h])
    fmt = lambda v: "(" + ", ".join(f"{x:8.2f}" for x in v) + ")" if v else "—"
    print(f"{j['name'][:21]:<22}{j['parent']:>7}{fmt(j['t']):>34}{fmt(glob[i]):>34}{fmt(at):>34}")

# How many animation translations are closer to bind-global than to bind-local?
close_local = close_global = 0
for i, j in enumerate(joints):
    h = j["hash"]
    if h not in first_frame:
        continue
    at = vec3(anm, first_frame[h])
    dl = sum((a - b) ** 2 for a, b in zip(at, j["t"])) ** 0.5
    dg = sum((a - b) ** 2 for a, b in zip(at, glob[i])) ** 0.5
    if dl < 1.0:
        close_local += 1
    if dg < 1.0:
        close_global += 1
print(f"\nmatching bind-local: {close_local}   matching bind-global: {close_global}   joints with track: {len(first_frame)}/{len(joints)}")
