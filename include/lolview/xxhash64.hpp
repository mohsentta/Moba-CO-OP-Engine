#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lolview {

// xxHash64, seed 0. This is the hash the game uses to address WAD chunks.
std::uint64_t xxh64(const void* data, std::size_t len, std::uint64_t seed = 0);

inline std::uint64_t xxh64(std::string_view s, std::uint64_t seed = 0) {
    return xxh64(s.data(), s.size(), seed);
}

// Virtual chunk paths are addressed case-insensitively with forward slashes.
std::string normalize_virtual_path(std::string_view path);

// Chunk key for a virtual path, e.g. "assets/characters/ahri/skins/base/ahri.skn".
std::uint64_t path_hash(std::string_view path);

}  // namespace lolview
