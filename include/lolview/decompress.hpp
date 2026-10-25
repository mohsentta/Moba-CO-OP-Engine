#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lolview {

// Decode a zstd payload, including archives that concatenate several
// independent frames for one chunk ("zstd-multi"). `expected_size` comes from
// the WAD table of contents and is enforced: a payload that decodes to a
// different length is treated as corrupt, not silently truncated.
std::vector<std::uint8_t> zstd_decompress(std::span<const std::uint8_t> input,
                                          std::size_t expected_size);

// Decode a gzip member (RFC 1952), including multi-member streams.
std::vector<std::uint8_t> gzip_decompress(std::span<const std::uint8_t> input,
                                          std::size_t expected_size);

}  // namespace lolview
