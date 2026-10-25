#include <lolview/bin.hpp>

#include <cstring>
#include <stdexcept>

namespace lolview {
namespace {

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

    std::span<const std::uint8_t> take(std::size_t count) {
        if (pos_ + count > data_.size()) {
            throw std::runtime_error("bin: unexpected end of file at " + std::to_string(pos_));
        }
        const auto view = data_.subspan(pos_, count);
        pos_ += count;
        return view;
    }

    std::uint8_t u8() { return take(1)[0]; }

    std::uint16_t u16() {
        const auto bytes = take(2);
        return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
    }

    std::uint32_t u32() {
        const auto bytes = take(4);
        return static_cast<std::uint32_t>(bytes[0]) |
               (static_cast<std::uint32_t>(bytes[1]) << 8) |
               (static_cast<std::uint32_t>(bytes[2]) << 16) |
               (static_cast<std::uint32_t>(bytes[3]) << 24);
    }

    std::uint64_t u64() {
        std::uint64_t value = 0;
        const auto bytes = take(8);
        for (int i = 7; i >= 0; --i) value = (value << 8) | bytes[static_cast<std::size_t>(i)];
        return value;
    }

    float f32() {
        const std::uint32_t bits = u32();
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    std::string string() {
        const std::uint16_t length = u16();
        const auto bytes = take(length);
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    std::size_t position() const noexcept { return pos_; }

private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
};

BinValue read_value(Reader& reader, BinType type);

std::vector<BinField> read_fields(Reader& reader, std::size_t count) {
    std::vector<BinField> fields;
    fields.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        BinField field;
        field.name_hash = reader.u32();
        field.value = read_value(reader, static_cast<BinType>(reader.u8()));
        fields.push_back(std::move(field));
    }
    return fields;
}

BinValue read_value(Reader& reader, BinType type) {
    BinValue value;
    value.type = type;

    switch (type) {
        case BinType::Empty:
            break;
        case BinType::Bool:
        case BinType::Flag:
            value.integer = reader.u8();
            break;
        case BinType::S8:
        case BinType::U8:
            value.integer = reader.u8();
            break;
        case BinType::S16:
        case BinType::U16:
            value.integer = reader.u16();
            break;
        case BinType::S32:
        case BinType::U32:
            value.integer = reader.u32();
            break;
        case BinType::S64:
        case BinType::U64:
        case BinType::Path:
            value.integer = reader.u64();
            break;
        case BinType::Hash:
        case BinType::Link:
            value.integer = reader.u32();
            break;
        case BinType::Float:
            value.number = reader.f32();
            break;
        case BinType::Vec2:
        case BinType::Vec3:
        case BinType::Vec4:
        case BinType::Rgba: {
            const int count = type == BinType::Vec2   ? 2
                              : type == BinType::Vec3 ? 3
                              : type == BinType::Vec4 ? 4
                                                      : 1;
            for (int i = 0; i < count; ++i) value.vec[static_cast<std::size_t>(i)] = reader.f32();
            if (type == BinType::Rgba) value.integer = 0;
            break;
        }
        case BinType::Mat4:
            reader.take(64);
            break;
        case BinType::String:
            value.text = reader.string();
            break;
        case BinType::List:
        case BinType::List2: {
            const auto element_type = static_cast<BinType>(reader.u8());
            (void)reader.u32();
            const std::uint32_t count = reader.u32();
            value.items.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                value.items.push_back(read_value(reader, element_type));
            }
            break;
        }
        case BinType::Struct:
        case BinType::Embedded: {
            value.type_hash = reader.u32();
            std::size_t count = 0;
            if (type == BinType::Struct) {
                if (value.type_hash != 0) {
                    (void)reader.u32();
                    count = reader.u16();
                }
            } else {
                (void)reader.u32();
                count = reader.u16();
            }
            value.fields = read_fields(reader, count);
            break;
        }
        case BinType::Option: {
            const auto inner_type = static_cast<BinType>(reader.u8());
            value.present = reader.u8() != 0;
            if (value.present) value.items.push_back(read_value(reader, inner_type));
            break;
        }
        case BinType::Map: {
            const auto key_type = static_cast<BinType>(reader.u8());
            const auto value_type = static_cast<BinType>(reader.u8());
            (void)reader.u32();
            const std::uint32_t count = reader.u32();
            value.items.reserve(static_cast<std::size_t>(count) * 2);
            for (std::uint32_t i = 0; i < count; ++i) {
                value.items.push_back(read_value(reader, key_type));
                value.items.push_back(read_value(reader, value_type));
            }
            break;
        }
        default:
            throw std::runtime_error("bin: unknown value type " +
                                     std::to_string(static_cast<int>(type)) + " at " +
                                     std::to_string(reader.position()));
    }
    return value;
}

}  // namespace

BinDocument BinDocument::parse(std::span<const std::uint8_t> bytes) {
    Reader reader(bytes);
    const auto magic = reader.take(4);
    if (std::memcmp(magic.data(), "PROP", 4) != 0) {
        throw std::runtime_error("bin: not a PROP file");
    }

    BinDocument document;
    document.version_ = reader.u32();
    if (document.version_ < 2) {
        throw std::runtime_error("bin: unsupported version " +
                                 std::to_string(document.version_));
    }

    const std::uint32_t linked = reader.u32();
    for (std::uint32_t i = 0; i < linked; ++i) (void)reader.string();

    const std::uint32_t entry_count = reader.u32();
    std::vector<std::uint32_t> entry_types(entry_count);
    for (std::size_t i = 0; i < entry_types.size(); ++i) entry_types[i] = reader.u32();

    document.entries_.reserve(entry_count);
    for (std::size_t i = 0; i < entry_types.size(); ++i) {
        BinEntry entry;
        const std::uint32_t length = reader.u32();
        entry.path_hash = reader.u32();
        const std::uint16_t field_count = reader.u16();
        const std::size_t start = reader.position();
        entry.fields = read_fields(reader, field_count);
        const std::size_t consumed = reader.position() - start + 6;
        if (consumed != length) {
            throw std::runtime_error("bin: entry " + std::to_string(i) + " consumed " +
                                     std::to_string(consumed) + " of " +
                                     std::to_string(length));
        }
        entry.type_hash = entry_types[i];
        document.entries_.push_back(std::move(entry));
    }
    return document;
}

const BinValue* find_field(const std::vector<BinField>& fields, std::string_view name) {
    const std::uint32_t wanted = fnv1a(name);
    for (const auto& field : fields) {
        if (field.name_hash == wanted) return &field.value;
    }
    return nullptr;
}

const BinValue* find_field(const BinValue& value, std::string_view name) {
    return find_field(value.fields, name);
}

float as_float(const BinValue* value, float fallback) {
    if (value == nullptr) return fallback;
    if (value->type == BinType::Float) return static_cast<float>(value->number);
    if (value->type == BinType::Option && value->present && !value->items.empty()) {
        return as_float(&value->items.front(), fallback);
    }
    return fallback;
}

int as_int(const BinValue* value, int fallback) {
    if (value == nullptr) return fallback;
    if (value->type == BinType::U8 || value->type == BinType::S8 ||
        value->type == BinType::U16 || value->type == BinType::S16 ||
        value->type == BinType::U32 || value->type == BinType::S32 ||
        value->type == BinType::Bool || value->type == BinType::Flag) {
        return static_cast<int>(value->integer);
    }
    return fallback;
}

bool as_bool(const BinValue* value, bool fallback) {
    if (value == nullptr) return fallback;
    if (value->type == BinType::Bool || value->type == BinType::Flag) {
        return value->integer != 0;
    }
    return fallback;
}

std::string as_string(const BinValue* value) {
    if (value == nullptr) return {};
    if (value->type == BinType::String) return value->text;
    return {};
}

}  // namespace lolview
