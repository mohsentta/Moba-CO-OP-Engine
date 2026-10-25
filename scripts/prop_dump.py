"""Prototype parser for Riot PROP v3 bin files.

Layout recovered from the reference implementation (CDTB cdtb/binfile.py):
  u32 'PROP'
  u32 version
  u32 linked_count, then linked_count strings (u16 length + bytes)
  u32 entry_count
  entry_count * u32 entry type hashes
  entries: u32 ?, u32 length, u32 path hash, u16 field_count, fields
  field:   u32 name hash, u8 type, value

Only the pieces needed to read VFX definitions are implemented.
"""
import struct
import sys

FIELDS_DICT = r"_local\hashes\hashes.binfields.txt"
TYPES_DICT = r"_local\hashes\hashes.bintypes.txt"

TYPE_NAMES = {
    0: "EMPTY", 1: "BOOL", 2: "S8", 3: "U8", 4: "S16", 5: "U16", 6: "S32", 7: "U32",
    8: "S64", 9: "U64", 10: "FLOAT", 11: "VEC2", 12: "VEC3", 13: "VEC4", 14: "MAT4",
    15: "RGBA", 16: "STRING", 17: "HASH", 18: "PATH", 0x80: "LIST", 0x81: "LIST2",
    0x82: "STRUCT", 0x83: "EMBEDDED", 0x84: "LINK", 0x85: "OPTION", 0x86: "MAP",
    0x87: "FLAG",
}


def fnv1a(text: str) -> int:
    h = 0x811C9DC5
    for byte in text.encode("utf-8"):
        h ^= byte
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def load_dictionary(path):
    table = {}
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                parts = line.split(maxsplit=1)
                if len(parts) != 2:
                    continue
                try:
                    table[int(parts[0], 16)] = parts[1].strip()
                except ValueError:
                    continue
    except OSError:
        pass
    return table


class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def take(self, n):
        chunk = self.data[self.pos:self.pos + n]
        if len(chunk) != n:
            raise EOFError(f"eof at {self.pos} wanting {n}")
        self.pos += n
        return chunk

    def u8(self): return struct.unpack("<B", self.take(1))[0]
    def u16(self): return struct.unpack("<H", self.take(2))[0]
    def u32(self): return struct.unpack("<I", self.take(4))[0]
    def u64(self): return struct.unpack("<Q", self.take(8))[0]
    def f32(self): return struct.unpack("<f", self.take(4))[0]
    def string(self):
        return self.take(self.u16()).decode("utf-8", "replace")


def read_value(reader, btype):
    if btype == 0: return None
    if btype == 1: return bool(reader.u8())
    if btype == 2: return struct.unpack("<b", reader.take(1))[0]
    if btype == 3: return reader.u8()
    if btype == 4: return struct.unpack("<h", reader.take(2))[0]
    if btype == 5: return reader.u16()
    if btype == 6: return struct.unpack("<i", reader.take(4))[0]
    if btype == 7: return reader.u32()
    if btype == 8: return struct.unpack("<q", reader.take(8))[0]
    if btype == 9: return reader.u64()
    if btype == 10: return reader.f32()
    if btype == 11: return struct.unpack("<2f", reader.take(8))
    if btype == 12: return struct.unpack("<3f", reader.take(12))
    if btype == 13: return struct.unpack("<4f", reader.take(16))
    if btype == 14: return struct.unpack("<16f", reader.take(64))
    if btype == 15: return tuple(reader.take(4))
    if btype == 16: return reader.string()
    if btype == 17: return reader.u32()
    if btype == 18:
        value = reader.u64()
        return ("path", f"{value:016x}")
    if btype in (0x80, 0x81):
        vtype = reader.u8()
        reader.u32()
        count = reader.u32()
        return ("list", TYPE_NAMES.get(vtype, hex(vtype)),
                [read_value(reader, vtype) for _ in range(count)])
    if btype in (0x82, 0x83):
        htype = reader.u32()
        count = 0
        if btype == 0x82:
            if htype != 0:
                reader.u32()
                count = reader.u16()
        else:
            reader.u32()
            count = reader.u16()
        return ("struct", htype, [read_field(reader) for _ in range(count)])
    if btype == 0x84: return ("link", reader.u32())
    if btype == 0x85:
        vtype = reader.u8()
        has_value = reader.u8()
        return ("option", TYPE_NAMES.get(vtype, hex(vtype)),
                read_value(reader, vtype) if has_value else None)
    if btype == 0x86:
        ktype = reader.u8()
        vtype = reader.u8()
        reader.u32()
        count = reader.u32()
        pairs = []
        for _ in range(count):
            pairs.append((read_value(reader, ktype), read_value(reader, vtype)))
        return ("map", pairs)
    if btype == 0x87: return reader.u8()
    raise ValueError(f"unknown bin type 0x{btype:02x} at 0x{reader.pos:x}")


def read_field(reader):
    name_hash = reader.u32()
    btype = reader.u8()
    return name_hash, btype, read_value(reader, btype)


def parse(data):
    reader = Reader(data)
    magic = reader.take(4)
    if magic != b"PROP":
        raise ValueError(f"not a PROP bin: {magic!r}")
    version = reader.u32()
    if version < 2:
        raise ValueError(f"unsupported version {version}")
    linked = [reader.string() for _ in range(reader.u32())]
    entry_count = reader.u32()
    entry_types = [reader.u32() for _ in range(entry_count)]

    entries = []
    for i in range(entry_count):
        length = reader.u32()
        path_hash = reader.u32()
        field_count = reader.u16()
        start = reader.pos
        fields = [read_field(reader) for _ in range(field_count)]
        consumed = reader.pos - start + 6  # path hash + field count are included
        if consumed != length:
            raise ValueError(f"entry {i}: consumed {consumed}, header says {length}")
        entries.append((path_hash, entry_types[i], fields))
    return version, linked, entries


def format_value(value, depth=0):
    if isinstance(value, tuple) and value and value[0] == "list":
        return f"list<{value[1]}>[{len(value[2])}]"
    if isinstance(value, tuple) and value and value[0] == "struct":
        return f"struct[{len(value[2])} fields]"
    if isinstance(value, str):
        return repr(value)
    if isinstance(value, float):
        return f"{value:.4g}"
    if isinstance(value, (bytes, bytearray)):
        return value.hex()
    return repr(value)


def dump_value(value, fields_dict, types_dict, indent, depth, max_items=6):
    pad = "  " * indent
    if isinstance(value, tuple) and value and value[0] == "list":
        items = value[2]
        print(f"list<{value[1]}> [{len(items)}]")
        for item in items[:max_items]:
            print(pad + "  - ", end="")
            dump_value(item, fields_dict, types_dict, indent + 1, depth + 1, max_items)
        if len(items) > max_items:
            print(pad + f"  ... {len(items) - max_items} more")
        return
    if isinstance(value, tuple) and value and value[0] == "struct":
        type_name = types_dict.get(value[1], f"0x{value[1]:08x}")
        print(f"struct {type_name} [{len(value[2])} fields]")
        if depth > 3:
            return
        for name_hash, btype, sub in value[2]:
            name = fields_dict.get(name_hash, f"0x{name_hash:08x}")
            label = f"{pad}  {name:<32} {TYPE_NAMES.get(btype, hex(btype)):<8}"
            if isinstance(sub, (tuple,)) and sub and sub[0] in ("list", "struct"):
                print(label)
                dump_value(sub, fields_dict, types_dict, indent + 1, depth + 1, max_items)
            else:
                print(label, format_value(sub))
        return
    print(format_value(value))


def list_names(path):
    """Compact listing: one line per entry with its type and particle name."""
    data = open(path, "rb").read()
    fields_dict = load_dictionary(FIELDS_DICT)
    types_dict = load_dictionary(TYPES_DICT)
    version, _linked, entries = parse(data)
    print(f"{path}: PROP v{version}, {len(entries)} entries")
    for path_hash, type_hash, fields in entries:
        names = {}
        for name_hash, btype, value in fields:
            name = fields_dict.get(name_hash, "")
            if name in ("particleName", "emitterName", "particlePath", "objectPath") and isinstance(value, str):
                names[name] = value
        type_name = types_dict.get(type_hash, f"0x{type_hash:08x}")
        print(f"  0x{path_hash:08x}  {type_name:<32} {names}")


def main(path, max_entries=3):
    if len(sys.argv) > 2 and sys.argv[2] == "names":
        list_names(path)
        return
    data = open(path, "rb").read()
    fields_dict = load_dictionary(FIELDS_DICT)
    types_dict = load_dictionary(TYPES_DICT)

    version, linked, entries = parse(data)
    print(f"{path}: PROP v{version}, {len(linked)} linked files, {len(entries)} entries")
    for path_hash, type_hash, fields in entries[:max_entries]:
        type_name = types_dict.get(type_hash, f"0x{type_hash:08x}")
        print(f"\nentry 0x{path_hash:08x}  type={type_name}  fields={len(fields)}")
        for name_hash, btype, value in fields:
            name = fields_dict.get(name_hash, f"0x{name_hash:08x}")
            label = f"  {name:<32} {TYPE_NAMES.get(btype, hex(btype)):<8}"
            if isinstance(value, tuple) and value and value[0] in ("list", "struct"):
                print(label)
                dump_value(value, fields_dict, types_dict, 1, 0)
            else:
                print(label, format_value(value))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else r"_local\vfx\def_small.bin")
