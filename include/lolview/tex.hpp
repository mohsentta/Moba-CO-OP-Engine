#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lolview {

struct TexImage {
    int width = 0;
    int height = 0;
    std::string format;              // "BC1", "BC3", "BC7"
    std::vector<std::uint8_t> rgba;  // width * height * 4, top-down
};

// Riot `.tex` container:
//   u32  magic "TEX\0"
//   u16  width
//   u16  height
//   u8   version/flags (1)
//   u8   format code (0x0a = BC1, 0x0c = BC3)
//   u16  flags
//   ...  BCn mip chain, largest mip first (mip 0 decoded here)
//
// The format is cross-checked against the payload size: a 1024x1024 BC3 chain
// is 1,398,128 bytes, while the same texture as BC1 is exactly half that.
TexImage decode_tex(std::span<const std::uint8_t> bytes);

// Plain DDS files (ability icons ship as DDS rather than TEX). Handles the
// BC1/BC2/BC3 variants; mip 0 only.
TexImage decode_dds(std::span<const std::uint8_t> bytes);

}  // namespace lolview
