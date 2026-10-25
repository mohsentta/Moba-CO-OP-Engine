"""Print one VFX system from a PROP bin, by particleName."""
import sys

sys.path.insert(0, r"scripts")
import prop_dump as P  # noqa: E402


def main(path, wanted):
    data = open(path, "rb").read()
    fields_dict = P.load_dictionary(P.FIELDS_DICT)
    types_dict = P.load_dictionary(P.TYPES_DICT)
    _version, _linked, entries = P.parse(data)

    for path_hash, type_hash, fields in entries:
        values = {}
        for name_hash, _btype, value in fields:
            values[fields_dict.get(name_hash, f"0x{name_hash:08x}")] = value
        if values.get("particleName") != wanted:
            continue
        type_name = types_dict.get(type_hash, f"0x{type_hash:08x}")
        print(f"{wanted}: {type_name}  entry 0x{path_hash:08x}")
        for name_hash, btype, value in fields:
            name = fields_dict.get(name_hash, f"0x{name_hash:08x}")
            label = f"  {name:<32} {P.TYPE_NAMES.get(btype, hex(btype)):<8}"
            if isinstance(value, tuple) and value and value[0] in ("list", "struct"):
                print(label)
                P.dump_value(value, fields_dict, types_dict, 1, 0, max_items=3)
            else:
                print(label, P.format_value(value))
        return
    print(f"no entry named {wanted}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
