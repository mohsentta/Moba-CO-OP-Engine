#include <lolview/decompress.hpp>
#include <lolview/wad.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace lolview {
namespace {

constexpr std::size_t kTocEntrySize = 32;
constexpr std::size_t kMaxEntries = 4'000'000;

std::uint32_t u32_at(const std::uint8_t* b, std::size_t o) noexcept {
    return static_cast<std::uint32_t>(b[o]) |
           (static_cast<std::uint32_t>(b[o + 1]) << 8) |
           (static_cast<std::uint32_t>(b[o + 2]) << 16) |
           (static_cast<std::uint32_t>(b[o + 3]) << 24);
}

std::uint64_t u64_at(const std::uint8_t* b, std::size_t o) noexcept {
    return static_cast<std::uint64_t>(u32_at(b, o)) |
           (static_cast<std::uint64_t>(u32_at(b, o + 4)) << 32);
}

bool compression_known(std::uint8_t code) noexcept {
    return code == 0 || code == 1 || code == 2 || code == 3 || code == 4 ||
           code == 0x24 || code == 0x34;
}

}  // namespace

std::string_view compression_name(std::uint8_t code) noexcept {
    switch (code) {
        case 0: return "stored";
        case 1: return "gzip";
        case 2: return "zstd";
        case 3: return "zstd-multi";
        case 0x24: return "zstd-f";
        case 0x34: return "zstd-f";
        default: return "?";
    }
}

std::optional<WadEntry> Wad::parse_entry(const std::uint8_t* bytes,
                                         std::size_t size,
                                         std::size_t offset) noexcept {
    if (offset + kTocEntrySize > size) {
        return std::nullopt;
    }
    WadEntry e;
    e.hash = u64_at(bytes, offset);
    e.offset = u32_at(bytes, offset + 8);
    e.compressed_size = u32_at(bytes, offset + 12);
    e.uncompressed_size = u32_at(bytes, offset + 16);
    e.compression = bytes[offset + 20];
    e.duplicate = bytes[offset + 21];
    e.checksum = u64_at(bytes, offset + 24);
    return e;
}

// An entry's payload must start after the entry itself, which makes the walk
// self-terminating: the first structurally invalid record ends it.
void Wad::walk_entries(const std::uint8_t* bytes,
                       std::size_t size,
                       std::size_t start,
                       std::vector<WadEntry>& out) {
    std::size_t o = start;
    while (true) {
        const auto entry = parse_entry(bytes, size, o);
        if (!entry) break;
        if (!compression_known(entry->compression)) break;
        if (entry->compressed_size == 0 || entry->uncompressed_size == 0) break;
        if (static_cast<std::uint64_t>(entry->offset) + entry->compressed_size > size) break;
        if (entry->offset < o + kTocEntrySize) break;
        out.push_back(*entry);
        o += kTocEntrySize;
        if (out.size() > kMaxEntries) break;
    }
}

Wad Wad::open(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::runtime_error("cannot open archive: " + path.string());
    }
    const std::streamoff size = in.tellg();
    if (size < 0) {
        throw std::runtime_error("cannot size archive: " + path.string());
    }
    Wad wad;
    wad.path_ = path;
    wad.bytes_.resize(static_cast<std::size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(wad.bytes_.data()), size)) {
        throw std::runtime_error("cannot read archive: " + path.string());
    }

    const std::uint8_t* bytes = wad.bytes_.data();
    const std::size_t len = wad.bytes_.size();
    if (len < 16 || bytes[0] != 'R' || bytes[1] != 'W') {
        throw std::runtime_error(path.string() + ": not an RW WAD");
    }

    bool have_best = false;
    std::size_t best_start = 0;
    std::vector<WadEntry> best_entries;
    std::uint64_t best_score = 0;

    for (std::size_t start = 16; start < 4096 && start < len; start += 16) {
        std::vector<WadEntry> entries;
        walk_entries(bytes, len, start, entries);
        if (entries.empty()) continue;

        // The structural walk can run past the real TOC into chunk data; shrink
        // until every payload starts after the TOC that describes it.
        std::size_t n = entries.size();
        while (n > 0) {
            std::size_t min_off = std::numeric_limits<std::size_t>::max();
            for (std::size_t i = 0; i < n; ++i) {
                min_off = std::min(min_off, static_cast<std::size_t>(entries[i].offset));
            }
            const std::size_t allowed = (min_off > start ? min_off - start : 0) / kTocEntrySize;
            if (allowed >= n) break;
            n = allowed;
        }
        entries.resize(n);
        if (entries.empty()) continue;

        const std::size_t toc_end = start + entries.size() * kTocEntrySize;
        bool ok = true;
        for (const auto& e : entries) {
            if (static_cast<std::size_t>(e.offset) < toc_end) { ok = false; break; }
        }
        if (!ok) continue;

        const WadEntry& last = entries.back();
        const std::uint64_t tail =
            static_cast<std::uint64_t>(last.offset) + last.compressed_size;
        const std::uint64_t score =
            static_cast<std::uint64_t>(entries.size()) + (tail == len ? 1'000'000ULL : 0ULL);

        if (!have_best || score > best_score) {
            have_best = true;
            best_score = score;
            best_start = start;
            best_entries = std::move(entries);
        }
    }

    if (!have_best) {
        throw std::runtime_error(path.string() + ": no consistent TOC found (size " +
                                 std::to_string(len) + ")");
    }

    wad.toc_offset_ = best_start;
    wad.entries_ = std::move(best_entries);
    return wad;
}

std::optional<std::size_t> Wad::find(std::uint64_t hash) const noexcept {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].hash == hash) return i;
    }
    return std::nullopt;
}

const std::uint8_t* Wad::raw(const WadEntry& entry) const noexcept {
    return bytes_.data() + entry.offset;
}

std::vector<std::uint8_t> Wad::read_chunk(const WadEntry& entry) const {
    const std::uint8_t* data = raw(entry);
    const std::size_t size = entry.compressed_size;
    const std::size_t want = entry.uncompressed_size;

    const bool zstd_magic = size >= 4 && data[0] == 0x28 && data[1] == 0xB5 &&
                            data[2] == 0x2F && data[3] == 0xFD;
    const bool gzip_magic = size >= 2 && data[0] == 0x1F && data[1] == 0x8B;

    // Dispatch on the stored magic first: shipped archives use several
    // compression codes for the same zstd payload (2, 3, 4, 0x24, 0x34).
    if (zstd_magic) {
        return zstd_decompress(std::span<const std::uint8_t>(data, size), want);
    }
    if (gzip_magic) {
        return gzip_decompress(std::span<const std::uint8_t>(data, size), want);
    }

    switch (entry.compression) {
        case 0:
            break;
        case 1:
            return gzip_decompress(std::span<const std::uint8_t>(data, size), want);
        case 2:
        case 3:
        case 4:
        case 0x24:
        case 0x34:
            return zstd_decompress(std::span<const std::uint8_t>(data, size), want);
        default:
            throw std::runtime_error("unsupported compression code " +
                                     std::to_string(entry.compression) + " (" +
                                     std::string(compression_name(entry.compression)) + ")");
    }

    if (size != want) {
        throw std::runtime_error("stored chunk is " + std::to_string(size) +
                                 " bytes, TOC says " + std::to_string(want));
    }
    return std::vector<std::uint8_t>(data, data + size);
}

}  // namespace lolview
