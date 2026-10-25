#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lolview {

enum class BinType : std::uint8_t {
    Empty = 0,
    Bool = 1,
    S8 = 2,
    U8 = 3,
    S16 = 4,
    U16 = 5,
    S32 = 6,
    U32 = 7,
    S64 = 8,
    U64 = 9,
    Float = 10,
    Vec2 = 11,
    Vec3 = 12,
    Vec4 = 13,
    Mat4 = 14,
    Rgba = 15,
    String = 16,
    Hash = 17,
    Path = 18,
    List = 0x80,
    List2 = 0x81,
    Struct = 0x82,
    Embedded = 0x83,
    Link = 0x84,
    Option = 0x85,
    Map = 0x86,
    Flag = 0x87,
};

struct BinField;

struct BinValue {
    BinType type = BinType::Empty;
    std::uint64_t integer = 0;      // integers, hash, path, link
    double number = 0.0;            // float
    std::array<float, 4> vec{};     // vec2/3/4 and rgba
    std::string text;               // string
    std::vector<BinValue> items;    // list elements, map pairs, option payload
    std::vector<BinField> fields;   // struct / embedded contents
    std::uint32_t type_hash = 0;    // struct / embedded class hash
    bool present = false;           // option
};

struct BinField {
    std::uint32_t name_hash = 0;
    BinValue value;
};

struct BinEntry {
    std::uint32_t path_hash = 0;
    std::uint32_t type_hash = 0;
    std::vector<BinField> fields;
};

// FNV-1a 32 over the ASCII-lowercased name. Riot hashes bin field and class
// names this way, so "particleName" and "particlename" address the same field.
constexpr std::uint32_t fnv1a(std::string_view text) {
    std::uint32_t hash = 0x811C9DC5u;
    for (char character : text) {
        const auto byte = static_cast<std::uint8_t>(
            (character >= 'A' && character <= 'Z') ? character - 'A' + 'a' : character);
        hash ^= byte;
        hash *= 0x01000193u;
    }
    return hash;
}

// Reader for Riot PROP v3 ("property bin") files.
class BinDocument {
public:
    static BinDocument parse(std::span<const std::uint8_t> bytes);

    std::uint32_t version() const noexcept { return version_; }
    const std::vector<BinEntry>& entries() const noexcept { return entries_; }

private:
    std::uint32_t version_ = 0;
    std::vector<BinEntry> entries_;
};

// Field lookup by name (hashed with FNV-1a).
const BinValue* find_field(const std::vector<BinField>& fields, std::string_view name);
const BinValue* find_field(const BinValue& value, std::string_view name);

// Helpers that tolerate missing or wrongly typed values.
float as_float(const BinValue* value, float fallback = 0.0f);
int as_int(const BinValue* value, int fallback = 0);
bool as_bool(const BinValue* value, bool fallback = false);
std::string as_string(const BinValue* value);

}  // namespace lolview
