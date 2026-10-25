#include <lolview/tex.hpp>

#include <bcdec.h>

#include <cstring>
#include <stdexcept>
#include <string>

namespace lolview {
namespace {

std::uint32_t u32_at(const std::uint8_t* bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

}  // namespace

TexImage decode_dds(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 128 || std::memcmp(bytes.data(), "DDS ", 4) != 0) {
        throw std::runtime_error("dds: not a DDS file");
    }

    const int height = static_cast<int>(u32_at(bytes.data(), 12));
    const int width = static_cast<int>(u32_at(bytes.data(), 16));
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        throw std::runtime_error("dds: implausible dimensions");
    }

    char fourcc[5] = {};
    std::memcpy(fourcc, bytes.data() + 84, 4);

    void (*decode_block)(const void*, void*, int) = nullptr;
    int block_bytes = 16;
    if (std::memcmp(fourcc, "DXT1", 4) == 0) {
        decode_block = &bcdec_bc1;
        block_bytes = 8;
    } else if (std::memcmp(fourcc, "DXT3", 4) == 0) {
        decode_block = &bcdec_bc2;
    } else if (std::memcmp(fourcc, "DXT5", 4) == 0) {
        decode_block = &bcdec_bc3;
    } else {
        throw std::runtime_error(std::string("dds: unsupported fourCC '") + fourcc + "'");
    }

    TexImage image;
    image.width = width;
    image.height = height;
    image.format = block_bytes == 8 ? "BC1" : "BC3";
    image.rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);

    const int blocks_x = (width + 3) / 4;
    const int blocks_y = (height + 3) / 4;
    const std::size_t needed =
        static_cast<std::size_t>(blocks_x) * blocks_y * static_cast<std::size_t>(block_bytes);
    if (bytes.size() < 128 + needed) {
        throw std::runtime_error("dds: truncated payload");
    }

    std::uint8_t block[64];
    for (int by = 0; by < blocks_y; ++by) {
        for (int bx = 0; bx < blocks_x; ++bx) {
            const std::size_t source =
                128 + (static_cast<std::size_t>(by) * blocks_x + bx) * block_bytes;
            std::memset(block, 0, sizeof(block));
            decode_block(bytes.data() + source, block, 16);

            const int copy_w = std::min(4, width - bx * 4);
            const int copy_h = std::min(4, height - by * 4);
            for (int row = 0; row < copy_h; ++row) {
                std::uint8_t* destination =
                    image.rgba.data() +
                    (static_cast<std::size_t>(by * 4 + row) * width + bx * 4) * 4;
                std::memcpy(destination, block + row * 16,
                            static_cast<std::size_t>(copy_w) * 4);
            }
        }
    }
    return image;
}

}  // namespace lolview
