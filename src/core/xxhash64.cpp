#include <lolview/xxhash64.hpp>

#include <bit>
#include <cstring>

namespace lolview {
namespace {

constexpr std::uint64_t kP1 = 0x9E3779B185EBCA87ULL;
constexpr std::uint64_t kP2 = 0xC2B2AE3D27D4EB4FULL;
constexpr std::uint64_t kP3 = 0x165667B19E3779F9ULL;
constexpr std::uint64_t kP4 = 0x85EBCA77C2B2AE63ULL;
constexpr std::uint64_t kP5 = 0x27D4EB2F165667C5ULL;

static_assert(std::endian::native == std::endian::little,
              "WAD and xxHash reads assume a little-endian host");

inline std::uint64_t rotl(std::uint64_t x, int r) noexcept {
    return (x << r) | (x >> (64 - r));
}

inline std::uint64_t read64(const std::uint8_t* p) noexcept {
    std::uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline std::uint32_t read32(const std::uint8_t* p) noexcept {
    std::uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline std::uint64_t round(std::uint64_t acc, std::uint64_t input) noexcept {
    acc += input * kP2;
    acc = rotl(acc, 31);
    acc *= kP1;
    return acc;
}

inline std::uint64_t merge_round(std::uint64_t acc, std::uint64_t val) noexcept {
    val = round(0, val);
    acc ^= val;
    acc = acc * kP1 + kP4;
    return acc;
}

}  // namespace

std::uint64_t xxh64(const void* data, std::size_t len, std::uint64_t seed) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    const std::uint8_t* const end = p + len;
    std::uint64_t h = 0;

    if (len >= 32) {
        const std::uint8_t* const limit = end - 32;
        std::uint64_t v1 = seed + kP1 + kP2;
        std::uint64_t v2 = seed + kP2;
        std::uint64_t v3 = seed;
        std::uint64_t v4 = seed - kP1;
        do {
            v1 = round(v1, read64(p)); p += 8;
            v2 = round(v2, read64(p)); p += 8;
            v3 = round(v3, read64(p)); p += 8;
            v4 = round(v4, read64(p)); p += 8;
        } while (p <= limit);

        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = merge_round(h, v1);
        h = merge_round(h, v2);
        h = merge_round(h, v3);
        h = merge_round(h, v4);
    } else {
        h = seed + kP5;
    }

    h += static_cast<std::uint64_t>(len);

    while (p + 8 <= end) {
        h ^= round(0, read64(p));
        h = rotl(h, 27) * kP1 + kP4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= static_cast<std::uint64_t>(read32(p)) * kP1;
        h = rotl(h, 23) * kP2 + kP3;
        p += 4;
    }
    while (p < end) {
        h ^= static_cast<std::uint64_t>(*p) * kP5;
        h = rotl(h, 11) * kP1;
        ++p;
    }

    h ^= h >> 33;
    h *= kP2;
    h ^= h >> 29;
    h *= kP3;
    h ^= h >> 32;
    return h;
}

std::string normalize_virtual_path(std::string_view path) {
    std::string out;
    out.reserve(path.size());
    for (char c : path) {
        if (c == '\\') {
            out.push_back('/');
        } else {
            out.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
        }
    }
    return out;
}

std::uint64_t path_hash(std::string_view path) {
    const std::string normalized = normalize_virtual_path(path);
    return xxh64(normalized.data(), normalized.size(), 0);
}

}  // namespace lolview
