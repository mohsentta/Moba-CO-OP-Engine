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

private:
    std::span<const std::uint8_t> data_;
    std::size_t offset_ = 0;
};

struct QuatL {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
};

QuatL decode_quat(const std::uint16_t value[3]) {
    float xyzw[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    decompress_quat(value, xyzw);
    return QuatL{xyzw[0], xyzw[1], xyzw[2], xyzw[3]};
}

float dot(const QuatL& a, const QuatL& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

// Catmull-Rom weights, matching the reference implementation.
void catmull_weights(float amount, float ease_in, float ease_out, float out[4]) {
    out[0] = (((2.0f - amount) * amount) - 1.0f) * (amount * ease_in);
    out[1] = ((((2.0f - ease_out) * amount) + (ease_out - 3.0f)) * (amount * amount)) + 1.0f;
    out[2] = ((((3.0f - ease_in * 2.0f) + ((ease_in - 2.0f) * amount)) * amount) + ease_in) *
             amount;
    out[3] = ((amount - 1.0f) * amount) * (amount * ease_out);
}

// Index of the last key at or before `time`; zero when the time precedes them.
std::size_t last_key_at(const std::vector<CompressedTrackKey>& track, std::uint16_t time) {
    std::size_t low = 0;
    std::size_t high = track.size();
    while (low < high) {
        const std::size_t mid = low + (high - low) / 2;
        if (track[mid].time <= time) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low == 0 ? 0 : low - 1;
}

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

    const float a = static_cast<float>((bits >> 30) & 32767u) * step - one_over_sqrt2;
    const float b = static_cast<float>((bits >> 15) & 32767u) * step - one_over_sqrt2;
    const float c = static_cast<float>(bits & 32767u) * step - one_over_sqrt2;
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

    // Bucket keys per joint and transform. Playback binary-searches these, which
    // is the stateless equivalent of the reference's cursor walk.
    anim.tracks.assign(static_cast<std::size_t>(anim.joint_count) * 3, {});
    for (const auto& frame : anim.frames) {
        if (frame.joint >= anim.joint_count || frame.transform > 2) continue;
        const std::size_t slot = static_cast<std::size_t>(frame.joint) * 3 + frame.transform;
        anim.tracks[slot].push_back(
            CompressedTrackKey{frame.time, {frame.value[0], frame.value[1], frame.value[2]}});
    }
    for (auto& track : anim.tracks) {
        std::stable_sort(track.begin(), track.end(),
                         [](const CompressedTrackKey& a, const CompressedTrackKey& b) {
                             return a.time < b.time;
                         });
    }

    anim.jump_frame_size = anim.frame_count < 0x10001u ? 24u : 48u;
    const std::size_t jump_bytes = static_cast<std::size_t>(anim.jump_cache_count) *
                                   anim.joint_count * anim.jump_frame_size;
    anim.jump_caches.resize(jump_bytes);
    const std::size_t jump_start = static_cast<std::size_t>(jump_off) + kBias;
    if (jump_start + jump_bytes <= bytes.size()) {
        std::memcpy(anim.jump_caches.data(), bytes.data() + jump_start, jump_bytes);
    }
    return anim;
}

std::map<std::uint32_t, TrsKey> evaluate_compressed(const CompressedAnimation& anim,
                                                    float time) {
    std::map<std::uint32_t, TrsKey> out;
    if (anim.duration <= 0.0f || anim.joint_count == 0 || anim.tracks.empty()) return out;

    const float clamped = std::min(std::max(time, 0.0f), anim.duration);
    const auto compressed_time = static_cast<std::uint16_t>(
        std::min(65535.0f, (clamped / anim.duration) * 65535.0f));
    const bool parametrized = anim.parametrized();

    for (std::uint32_t joint = 0; joint < anim.joint_count; ++joint) {
        TrsKey key;

        // ---- rotation ----
        const auto& rotation = anim.tracks[static_cast<std::size_t>(joint) * 3 + 0];
        if (!rotation.empty()) {
            const std::size_t index = last_key_at(rotation, compressed_time);
            const std::size_t first = index == 0 ? 0 : index - 1;
            const std::size_t window[4] = {
                first,
                std::min(first + 1, rotation.size() - 1),
                std::min(first + 2, rotation.size() - 1),
                std::min(first + 3, rotation.size() - 1),
            };

            QuatL q[4];
            for (int i = 0; i < 4; ++i) q[i] = decode_quat(rotation[window[i]].value);
            // Shortest-path consistency, as the reference does when seeding.
            for (int i = 1; i < 4; ++i) {
                if (dot(q[i], q[0]) < 0.0f) {
                    q[i].x = -q[i].x;
                    q[i].y = -q[i].y;
                    q[i].z = -q[i].z;
                    q[i].w = -q[i].w;
                }
            }
            const float qv[4][4] = {{q[0].x, q[0].y, q[0].z, q[0].w},
                                    {q[1].x, q[1].y, q[1].z, q[1].w},
                                    {q[2].x, q[2].y, q[2].z, q[2].w},
                                    {q[3].x, q[3].y, q[3].z, q[3].w}};

            float amount = 0.0f, ease_in = 0.5f, ease_out = 0.5f;
            const float t1 = rotation[window[1]].time;
            const float t2 = rotation[window[2]].time;
            if (parametrized) {
                const float t0 = rotation[window[0]].time;
                const float t3 = rotation[window[3]].time;
                const float span = std::max(1e-6f, t2 - t1);
                amount = (static_cast<float>(compressed_time) - t1) / span;
                ease_in = span / std::max(1e-6f, t2 - t0);
                ease_out = span / std::max(1e-6f, t3 - t1);
            } else if (t2 > t1) {
                amount = (static_cast<float>(compressed_time) - t1) / (t2 - t1);
            }

            float w[4];
            catmull_weights(amount, ease_in, ease_out, w);
            float sum[4];
            for (int c = 0; c < 4; ++c) {
                sum[c] = w[0] * qv[0][c] + w[1] * qv[1][c] + w[2] * qv[2][c] +
                         w[3] * qv[3][c];
            }
            const float length = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] +
                                           sum[2] * sum[2] + sum[3] * sum[3]);
            const float inv = length > 1e-6f ? 1.0f / length : 0.0f;
            key.rotation = Quat{sum[0] * inv, sum[1] * inv, sum[2] * inv, sum[3] * inv};
        }

        // ---- translation and scale ----
        for (int transform = 1; transform <= 2; ++transform) {
            const auto& track = anim.tracks[static_cast<std::size_t>(joint) * 3 + transform];
            if (track.empty()) continue;
            const std::size_t index = last_key_at(track, compressed_time);
            const std::size_t first = index == 0 ? 0 : index - 1;
            const std::size_t window[4] = {
                first,
                std::min(first + 1, track.size() - 1),
                std::min(first + 2, track.size() - 1),
                std::min(first + 3, track.size() - 1),
            };
            const float* minimum = transform == 1 ? anim.translation_min : anim.scale_min;
            const float* maximum = transform == 1 ? anim.translation_max : anim.scale_max;

            float value[4][3];
            for (int i = 0; i < 4; ++i) {
                for (int c = 0; c < 3; ++c) {
                    value[i][c] = (static_cast<float>(track[window[i]].value[c]) / 65535.0f) *
                                      (maximum[c] - minimum[c]) +
                                  minimum[c];
                }
            }

            float amount = 0.0f, ease_in = 0.5f, ease_out = 0.5f;
            const float t1 = track[window[1]].time;
            const float t2 = track[window[2]].time;
            if (parametrized) {
                const float t0 = track[window[0]].time;
                const float t3 = track[window[3]].time;
                const float span = std::max(1e-6f, t2 - t1);
                amount = (static_cast<float>(compressed_time) - t1) / span;
                ease_in = span / std::max(1e-6f, t2 - t0);
                ease_out = span / std::max(1e-6f, t3 - t1);
            } else if (t2 > t1) {
                amount = (static_cast<float>(compressed_time) - t1) / (t2 - t1);
            }

            float w[4];
            catmull_weights(amount, ease_in, ease_out, w);
            float result[3];
            for (int c = 0; c < 3; ++c) {
                result[c] = w[0] * value[0][c] + w[1] * value[1][c] + w[2] * value[2][c] +
                            w[3] * value[3][c];
            }
            if (transform == 1) {
                key.translation = Vec3{result[0], result[1], result[2]};
            } else {
                key.scale = Vec3{result[0], result[1], result[2]};
            }
        }

        out[anim.joint_hashes[joint]] = key;
    }
    return out;
}

}  // namespace lolview
