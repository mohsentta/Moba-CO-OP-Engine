#include <lolview/anm_compressed.hpp>

#include <algorithm>
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

std::map<std::uint32_t, TrsKey> evaluate_compressed(const CompressedAnimation& anim,
                                                    float time) {
    std::map<std::uint32_t, TrsKey> out;
    if (anim.duration <= 0.0f || anim.jump_cache_count == 0 || anim.joint_count == 0 ||
        anim.frames.empty() || anim.jump_caches.empty()) {
        return out;
    }

    const float clamped = std::min(std::max(time, 0.0f), anim.duration);
    const float ratio = clamped / anim.duration;
    std::uint32_t cache_id = static_cast<std::uint32_t>(
        static_cast<float>(anim.jump_cache_count) * ratio);
    if (cache_id >= anim.jump_cache_count) cache_id = anim.jump_cache_count - 1;
    const auto compressed_time =
        static_cast<std::uint16_t>(std::min(65535.0f, ratio * 65535.0f));

    const bool keys16 = anim.frame_count < 0x10001u;
    const std::size_t record_size = keys16 ? 24u : 48u;

    const auto frame_time = [&](std::size_t index) {
        return anim.frames[index].time;
    };

    // Four control points -> interpolated sample, for one transform of a joint.
    struct Sample {
        bool rotation = false;
        bool valid = false;
        float value[3] = {0.0f, 0.0f, 0.0f};
        float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    };

    const auto sample = [&](const std::size_t indices[4], bool rotation, bool parametrized,
                            const float* minimum, const float* maximum) -> Sample {
        Sample result;
        result.rotation = rotation;
        for (int i = 0; i < 4; ++i) {
            if (indices[i] >= anim.frames.size()) return result;
        }

        float amount = 0.0f, ease_in = 0.5f, ease_out = 0.5f;
        if (parametrized) {
            const float t0 = frame_time(indices[0]);
            const float t1 = frame_time(indices[1]);
            const float t2 = frame_time(indices[2]);
            const float t3 = frame_time(indices[3]);
            const float span = std::max(1e-6f, t2 - t1);
            amount = (static_cast<float>(compressed_time) - t1) / span;
            ease_in = span / std::max(1e-6f, t2 - t0);
            ease_out = span / std::max(1e-6f, t3 - t1);
        } else {
            const int span = static_cast<int>(anim.frames[indices[2]].time) -
                             static_cast<int>(anim.frames[indices[1]].time);
            if (span <= 0) {
                const auto& frame = anim.frames[indices[1]];
                if (rotation) {
                    decompress_quat(frame.value, result.quat);
                } else {
                    for (int c = 0; c < 3; ++c) {
                        result.value[c] =
                            (static_cast<float>(frame.value[c]) / 65535.0f) *
                                (maximum[c] - minimum[c]) +
                            minimum[c];
                    }
                }
                result.valid = true;
                return result;
            }
            amount = (static_cast<float>(compressed_time) -
                      static_cast<float>(anim.frames[indices[1]].time)) /
                     static_cast<float>(span);
        }

        float w[4];
        w[0] = (((2.0f - amount) * amount) - 1.0f) * (amount * ease_in);
        w[1] = ((((2.0f - ease_out) * amount) + (ease_out - 3.0f)) * (amount * amount)) + 1.0f;
        w[2] = ((((3.0f - ease_in * 2.0f) + ((ease_in - 2.0f) * amount)) * amount) + ease_in) *
               amount;
        w[3] = ((amount - 1.0f) * amount) * (amount * ease_out);

        if (rotation) {
            float q[4][4];
            for (int i = 0; i < 4; ++i) decompress_quat(anim.frames[indices[i]].value, q[i]);
            float sum[4] = {0, 0, 0, 0};
            for (int c = 0; c < 4; ++c) {
                sum[c] = w[0] * q[0][c] + w[1] * q[1][c] + w[2] * q[2][c] + w[3] * q[3][c];
            }
            const float length = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] +
                                           sum[2] * sum[2] + sum[3] * sum[3]);
            const float inv = length > 1e-6f ? 1.0f / length : 0.0f;
            for (int c = 0; c < 4; ++c) result.quat[c] = sum[c] * inv;
        } else {
            float v[4][3];
            for (int i = 0; i < 4; ++i) {
                for (int c = 0; c < 3; ++c) {
                    v[i][c] = (static_cast<float>(anim.frames[indices[i]].value[c]) / 65535.0f) *
                                  (maximum[c] - minimum[c]) +
                              minimum[c];
                }
            }
            for (int c = 0; c < 3; ++c) {
                result.value[c] = w[0] * v[0][c] + w[1] * v[1][c] + w[2] * v[2][c] +
                                  w[3] * v[3][c];
            }
        }
        result.valid = true;
        return result;
    };

    const bool parametrized = anim.parametrized();
    for (std::uint32_t joint = 0; joint < anim.joint_count; ++joint) {
        const std::size_t record =
            (static_cast<std::size_t>(cache_id) * anim.joint_count + joint) * record_size;
        if (record + record_size > anim.jump_caches.size()) break;

        std::size_t indices[12];
        for (int i = 0; i < 12; ++i) {
            if (keys16) {
                const std::size_t at = record + static_cast<std::size_t>(i) * 2;
                indices[i] = static_cast<std::size_t>(anim.jump_caches[at]) |
                             (static_cast<std::size_t>(anim.jump_caches[at + 1]) << 8);
            } else {
                const std::size_t at = record + static_cast<std::size_t>(i) * 4;
                indices[i] = static_cast<std::size_t>(anim.jump_caches[at]) |
                             (static_cast<std::size_t>(anim.jump_caches[at + 1]) << 8) |
                             (static_cast<std::size_t>(anim.jump_caches[at + 2]) << 16) |
                             (static_cast<std::size_t>(anim.jump_caches[at + 3]) << 24);
            }
        }

        TrsKey key;
        const Sample rotation = sample(&indices[0], true, parametrized, nullptr, nullptr);
        if (rotation.valid) {
            key.rotation = Quat{rotation.quat[0], rotation.quat[1], rotation.quat[2],
                                rotation.quat[3]};
        }
        const Sample translation =
            sample(&indices[4], false, parametrized, anim.translation_min,
                   anim.translation_max);
        if (translation.valid) {
            key.translation = Vec3{translation.value[0], translation.value[1],
                                   translation.value[2]};
        } else {
            key.translation = Vec3{0.0f, 0.0f, 0.0f};
        }
        const Sample scale = sample(&indices[8], false, parametrized, anim.scale_min,
                                    anim.scale_max);
        if (scale.valid) {
            key.scale = Vec3{scale.value[0], scale.value[1], scale.value[2]};
        }

        out[anim.joint_hashes[joint]] = key;
    }
    return out;
}

}  // namespace lolview
