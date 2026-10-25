#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace lolview {

// One 32-byte table-of-contents record from a `.wad.client` archive.
struct WadEntry {
    std::uint64_t hash = 0;
    std::uint32_t offset = 0;
    std::uint32_t compressed_size = 0;
    std::uint32_t uncompressed_size = 0;
    // 0 = stored, 1 = gzip, 2 = zstd, 3 = zstd multi-frame.
    std::uint8_t compression = 0;
    std::uint8_t duplicate = 0;
    std::uint64_t checksum = 0;
};

std::string_view compression_name(std::uint8_t code) noexcept;

// Read-only reader for Riot's "WAD v3" archives.
//
// The TOC position is derived rather than assumed: the header/signature block
// has changed shape across patches, so candidate TOCs are scanned for and
// scored by structural invariants (see the reference parser in
// reference/legacy-rust/wad.rs, which this is a port of).
class Wad {
public:
    // Throws std::runtime_error when the file is unreadable or no consistent
    // TOC can be found.
    static Wad open(const std::filesystem::path& path);

    const std::filesystem::path& path() const noexcept { return path_; }
    std::size_t file_size() const noexcept { return bytes_.size(); }
    std::size_t toc_offset() const noexcept { return toc_offset_; }
    const std::vector<WadEntry>& entries() const noexcept { return entries_; }

    std::optional<std::size_t> find(std::uint64_t hash) const noexcept;
    const std::uint8_t* raw(const WadEntry& entry) const noexcept;

    // Payload decoding. Stored (uncompressed) chunks are supported today;
    // gzip/zstd decoding arrives with the format milestone and throws until
    // then rather than returning wrong bytes.
    std::vector<std::uint8_t> read_chunk(const WadEntry& entry) const;

private:
    Wad() = default;

    static std::optional<WadEntry> parse_entry(const std::uint8_t* bytes,
                                               std::size_t size,
                                               std::size_t offset) noexcept;
    static void walk_entries(const std::uint8_t* bytes,
                             std::size_t size,
                             std::size_t start,
                             std::vector<WadEntry>& out);

    std::filesystem::path path_;
    std::vector<std::uint8_t> bytes_;
    std::size_t toc_offset_ = 0;
    std::vector<WadEntry> entries_;
};

}  // namespace lolview
