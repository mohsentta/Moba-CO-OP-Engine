#include <lolview/tex.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

#include <bcdec.h>

namespace lolview {
namespace {

std::size_t mip_chain_size(int width, int height, int bytes_per_block) {
    std::size_t total = 0;
    while (true) {
        total += static_cast<std::size_t>((width + 3) / 4) * ((height + 3) / 4) *
                 static_cast<std::size_t>(bytes_per_block);
        if (width == 1 && height == 1) break;
        width = std::max(1, width / 2);
        height = std::max(1, height / 2);
    }
    return total;
}

}  // namespace

TexImage decode_tex(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12 + 8) {
        throw std::runtime_error("tex: file too small");
    }
    if (!(bytes[0] == 'T' && bytes[1] == 'E' && bytes[2] == 'X' && bytes[3] == 0)) {
        throw std::runtime_error("tex: bad magic (expected 'TEX\\0')");
    }

    const int width = static_cast<int>(bytes[4] | (bytes[5] << 8));
    const int height = static_cast<int>(bytes[6] | (bytes[7] << 8));
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        throw std::runtime_error("tex: implausible dimensions");
    }

    const std::uint8_t format_code = bytes[9];
    const std::span<const std::uint8_t> payload = bytes.subspan(12);

    // Which byte-per-block does the payload size imply?
    const std::size_t eight = mip_chain_size(width, height, 8);
    const std::size_t sixteen = mip_chain_size(width, height, 16);

    bool is_bc1 = false;
    if (payload.size() >= sixteen) {
        is_bc1 = false;
    } else if (payload.size() >= eight) {
        is_bc1 = true;
    } else {
        throw std::runtime_error("tex: payload smaller than one mip chain");
    }

    // Cross-check with the header code when it is one we have seen.
    if (format_code == 0x0a && payload.size() >= eight && payload.size() < sixteen) {
        is_bc1 = true;
    }

    const char* format = nullptr;
    void (*decode_block)(const void*, void*, int) = nullptr;

    if (is_bc1) {
        format = "BC1";
        decode_block = &bcdec_bc1;
    } else if (format_code == 0x1c || format_code == 0x1d) {
        format = "BC7";
        decode_block = &bcdec_bc7;
    } else {
        format = "BC3";
        decode_block = &bcdec_bc3;
    }

    TexImage image;
    image.width = width;
    image.height = height;
    image.format = format;
    image.rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);

    const int block_bytes = is_bc1 ? 8 : 16;
    const int blocks_x = (width + 3) / 4;
    const int blocks_y = (height + 3) / 4;
    const std::size_t needed =
        static_cast<std::size_t>(blocks_x) * blocks_y * static_cast<std::size_t>(block_bytes);
    if (payload.size() < needed) {
        throw std::runtime_error("tex: truncated mip 0 (need " + std::to_string(needed) +
                                 " bytes, have " + std::to_string(payload.size()) + ")");
    }

    // Riot stores the chain with mip 0 LAST: reading from the front yields a
    // tile of the smallest mips, while the big image sits at the end.
    const std::size_t mip0_offset = payload.size() - needed;
    const std::span<const std::uint8_t> mip0 = payload.subspan(mip0_offset, needed);

    std::uint8_t block[64];
    for (int by = 0; by < blocks_y; ++by) {
        for (int bx = 0; bx < blocks_x; ++bx) {
            const std::size_t src_offset =
                (static_cast<std::size_t>(by) * blocks_x + bx) * block_bytes;
            std::memset(block, 0, sizeof(block));
            decode_block(mip0.data() + src_offset, block, 16);

            const int copy_w = std::min(4, width - bx * 4);
            const int copy_h = std::min(4, height - by * 4);
            for (int row = 0; row < copy_h; ++row) {
                std::uint8_t* dst = image.rgba.data() +
                                    (static_cast<std::size_t>(by * 4 + row) * width + bx * 4) * 4;
                std::memcpy(dst, block + row * 16, static_cast<std::size_t>(copy_w) * 4);
            }
        }
    }
    return image;
}

}  // namespace lolview
