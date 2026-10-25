#include <lolview/anm_compressed.hpp>

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace lolview {
namespace {

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

    void need(std::size_t n) const {
        if (offset_ + n > data_.size()) {
            throw std::runtime_error("compressed anim: unexpected end of file");
        }
    }
    std::uint16_t u16() {
        need(2);
        const std::uint16_t value =
            static_cast<std::uint16_t>(data_[offset_] | (data_[offset_ + 1] << 8));
        offset_ += 2;
        return value;
    }
    std::uint32_t u32() {
        need(4);
        std::uint32_t value = 0;
        value |= static_cast<std::uint32_t>(data_[offset_]);
        value |= static_cast<std::uint32_t>(data_[offset_ + 1]) << 8;
        value |= static_cast<std::uint32_t>(data_[offset_ + 2]) << 16;
        value |= static_cast<std::uint32_t>(data_[offset_ + 3]) << 24;
        offset_ += 4;
        return value;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    float f32() {
        const std::uint32_t bits = u32();
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    void seek(std::size_t position) { offset_ = position; }
    std::size_t position() const noexcept { return offset_; }

private:
    std::span<const std::uint8_t> data_;
    std::size_t offset_ = 0;
};

}  // namespace

bool is_compressed_animation(std::span<const std::uint8_t> bytes) {
    return bytes.size() >= 8 && std::memcmp(bytes.data(), "r3d2canm", 8) == 0;
}

void decompress_quat(const std::uint16_t value[3], float out_xyzw[4]) {
    const std::uint64_t bits = static_cast<std::uint64_t>(value[0]) |
                               (static_cast<std::uint64_t>(value[1]) << 16) |
                               (static_cast<std::uint64_t>(value[2]) << 32);
    const std::uint8_t missing = static_cast<std::uint8_t>((bits >> 45) & 3u);

    const float kSqrt2 = std::sqrt(2.0f);
    const float one_over_sqrt2 = kSqrt2 * 0.5f;
    const float step = kSqrt2 / 32767.0f;

    float a = static_cast<float>((bits >> 30) & 32767u) * step - one_over_sqrt2;
    float b = static_cast<float>((bits >> 15) & 32767u) * step - one_over_sqrt2;
    float c = static_cast<float>(bits & 32767u) * step - one_over_sqrt2;
    const float d = std::sqrt(std::max(0.0f, 1.0f - (a * a + b * b + c * c)));

    switch (missing) {
        case 0: out_xyzw[0] = d; out_xyzw[1] = a; out_xyzw[2] = b; out_xyzw[3] = c; break;
        case 1: out_xyzw[0] = a; out_xyzw[1] = d; out_xyzw[2] = b; out_xyzw[3] = c; break;
        case 2: out_xyzw[0] = a; out_xyzw[1] = b; out_xyzw[2] = d; out_xyzw[3] = c; break;
        default: out_xyzw[0] = a; out_xyzw[1] = b; out_xyzw[2] = c; out_xyzw[3] = d; break;
    }
}

CompressedAnimation parse_compressed_animation(std::span<const std::uint8_t> bytes) {
    if (!is_compressed_animation(bytes)) {
        throw std::runtime_error("compressed anim: bad magic");
    }
    Reader reader(bytes);
    reader.seek(8);

    CompressedAnimation anim;
    anim.version = reader.u32();
    if (anim.version < 1 || anim.version > 3) {
        throw std::runtime_error("compressed anim: unsupported version " +
                                 std::to_string(anim.version));
    }
    reader.u32();  // resource size
    reader.u32();  // format token
    anim.flags = reader.u32();
    anim.joint_count = reader.u32();
    anim.frame_count = reader.u32();
    const std::int32_t jump_count = reader.i32();
    if (jump_count < 0) throw std::runtime_error("compressed anim: negative jump cache count");
    anim.jump_cache_count = static_cast<std::uint32_t>(jump_count);
    anim.duration = reader.f32();
    anim.fps = reader.f32();

    for (int metric = 0; metric < 3; ++metric) {
        reader.f32();  // margin
        reader.f32();  // discontinuity threshold
    }
    for (float& value : anim.translation_min) value = reader.f32();
    for (float& value : anim.translation_max) value = reader.f32();
    for (float& value : anim.scale_min) value = reader.f32();
    for (float& value : anim.scale_max) value = reader.f32();

    const std::int32_t frames_off = reader.i32();
    const std::int32_t jump_off = reader.i32();
    const std::int32_t hashes_off = reader.i32();
    if (frames_off <= 0 || jump_off <= 0 || hashes_off <= 0) {
        throw std::runtime_error("compressed anim: missing section");
    }
    // Section offsets carry a 12-byte bias in this container.
    constexpr std::size_t kBias = 12;

    anim.joint_hashes.resize(anim.joint_count);
    reader.seek(static_cast<std::size_t>(hashes_off) + kBias);
    for (auto& hash : anim.joint_hashes) hash = reader.u32();

    anim.frames.resize(anim.frame_count);
    reader.seek(static_cast<std::size_t>(frames_off) + kBias);
    for (auto& frame : anim.frames) {
        frame.time = reader.u16();
        const std::uint16_t joint_and_type = reader.u16();
        frame.joint = static_cast<std::uint16_t>(joint_and_type & 0x3fffu);
        frame.transform = static_cast<std::uint8_t>(joint_and_type >> 14);
        for (std::uint16_t& value : frame.value) value = reader.u16();
    }

    anim.jump_frame_size = anim.frame_count < 0x10001u ? 24u : 48u;
    const std::size_t jump_bytes = static_cast<std::size_t>(anim.jump_cache_count) *
                                   anim.joint_count * anim.jump_frame_size;
    reader.seek(static_cast<std::size_t>(jump_off) + kBias);
    anim.jump_caches.resize(jump_bytes);
    for (std::uint8_t& byte : anim.jump_caches) {
        reader.need(1);
        // Reader has no byte accessor; reuse u16 by seeking backwards is overkill.
        byte = 0;
    }
    // Re-read the blob directly from the span (byte access).
    if (static_cast<std::size_t>(jump_off) + kBias + jump_bytes <= bytes.size()) {
        std::memcpy(anim.jump_caches.data(),
                    bytes.data() + static_cast<std::size_t>(jump_off) + kBias, jump_bytes);
    }
    return anim;
}

}  // namespace lolview
