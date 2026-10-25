#include <lolview/anm.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace lolview {
namespace {

std::uint16_t u16_at(std::span<const std::uint8_t> d, std::size_t o) {
    return static_cast<std::uint16_t>(d[o] | (d[o + 1] << 8));
}
std::uint32_t u32_at(std::span<const std::uint8_t> d, std::size_t o) {
    return static_cast<std::uint32_t>(d[o]) | (static_cast<std::uint32_t>(d[o + 1]) << 8) |
           (static_cast<std::uint32_t>(d[o + 2]) << 16) |
           (static_cast<std::uint32_t>(d[o + 3]) << 24);
}
std::int32_t i32_at(std::span<const std::uint8_t> d, std::size_t o) {
    return static_cast<std::int32_t>(u32_at(d, o));
}
float f32_at(std::span<const std::uint8_t> d, std::size_t o) {
    const std::uint32_t bits = u32_at(d, o);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
Vec3 vec3_at(std::span<const std::uint8_t> d, std::size_t o) {
    return Vec3{f32_at(d, o), f32_at(d, o + 4), f32_at(d, o + 8)};
}
Quat quat_at(std::span<const std::uint8_t> d, std::size_t o) {
    return Quat{f32_at(d, o), f32_at(d, o + 4), f32_at(d, o + 8), f32_at(d, o + 12)};
}

// v5 stores rotations as three 15-bit components plus a 2-bit "largest
// component" selector packed into six bytes.
Quat decompress_quat6(const std::uint8_t* b) {
    const std::uint64_t first = static_cast<std::uint64_t>(b[0]) |
                                (static_cast<std::uint64_t>(b[1]) << 8);
    const std::uint64_t second = static_cast<std::uint64_t>(b[2]) |
                                 (static_cast<std::uint64_t>(b[3]) << 8);
    const std::uint64_t third = static_cast<std::uint64_t>(b[4]) |
                                (static_cast<std::uint64_t>(b[5]) << 8);
    const std::uint64_t bits = first | (second << 16) | (third << 32);

    const std::uint8_t max_index = static_cast<std::uint8_t>((bits >> 45) & 3);
    const float sqrt2 = std::sqrt(2.0f);
    const float one_over_sqrt2 = sqrt2 * 0.5f;
    const float scale = sqrt2 / 32767.0f;

    const float a = static_cast<float>((bits >> 30) & 32767) * scale - one_over_sqrt2;
    const float bb = static_cast<float>((bits >> 15) & 32767) * scale - one_over_sqrt2;
    const float c = static_cast<float>(bits & 32767) * scale - one_over_sqrt2;
    const float d = std::sqrt(std::max(0.0f, 1.0f - (a * a + bb * bb + c * c)));

    switch (max_index) {
        case 0: return Quat{d, a, bb, c};
        case 1: return Quat{a, d, bb, c};
        case 2: return Quat{a, bb, d, c};
        default: return Quat{a, bb, c, d};
    }
}

Quat slerp(const Quat& a, const Quat& b, float t) {
    float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    Quat target = b;
    if (dot < 0.0f) {
        dot = -dot;
        target = Quat{-b.x, -b.y, -b.z, -b.w};
    }
    if (dot > 0.9995f) {
        return normalize(Quat{a.x + t * (target.x - a.x), a.y + t * (target.y - a.y),
                               a.z + t * (target.z - a.z), a.w + t * (target.w - a.w)});
    }
    const float theta = std::acos(std::clamp(dot, -1.0f, 1.0f));
    const float sin_theta = std::sin(theta);
    if (std::fabs(sin_theta) < 1e-8f) return a;
    const float s0 = std::sin((1.0f - t) * theta) / sin_theta;
    const float s1 = std::sin(t * theta) / sin_theta;
    return Quat{s0 * a.x + s1 * target.x, s0 * a.y + s1 * target.y,
                s0 * a.z + s1 * target.z, s0 * a.w + s1 * target.w};
}

Vec3 lerp3(const Vec3& a, const Vec3& b, float t) {
    return Vec3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

}  // namespace

Anm Anm::parse(std::span<const std::uint8_t> d) {
    if (d.size() < 0x40 || std::memcmp(d.data(), "r3d2anmd", 8) != 0) {
        throw std::runtime_error("anm: not an r3d2anmd file");
    }

    const std::uint32_t version = u32_at(d, 8);
    Anm result;
    result.version_ = version;

    if (version == 5 || version == 4) {
        const std::size_t track_count = u32_at(d, 0x1c);
        const std::size_t frame_count = u32_at(d, 0x20);
        const float frame_duration = f32_at(d, 0x24);
        const std::int32_t vector_off = i32_at(d, 0x34);
        const std::int32_t quat_off = i32_at(d, 0x38);
        const std::int32_t frames_off = i32_at(d, 0x3c);
        if (vector_off <= 0 || quat_off <= 0 || frames_off <= 0) {
            throw std::runtime_error("anm: missing animation section");
        }

        result.fps_ = frame_duration > 0.0f ? 1.0f / frame_duration : 30.0f;
        result.frame_count_ = frame_count;
        result.duration_ = frame_count / result.fps_;

        if (version == 5) {
            const std::int32_t hashes_off = i32_at(d, 0x28);
            if (hashes_off <= 0) throw std::runtime_error("anm v5: missing joint hash section");

            const std::size_t vec_count = (quat_off - vector_off) / 12;
            const std::size_t quat_count = (hashes_off - quat_off) / 6;
            const std::size_t hash_count = (frames_off - hashes_off) / 4;

            result.vec_pal_.reserve(vec_count);
            for (std::size_t i = 0; i < vec_count; ++i) {
                result.vec_pal_.push_back(vec3_at(d, vector_off + 12 + i * 12));
            }
            result.quat_pal_.reserve(quat_count);
            for (std::size_t i = 0; i < quat_count; ++i) {
                const std::size_t o = quat_off + 12 + i * 6;
                result.quat_pal_.push_back(normalize(decompress_quat6(d.data() + o)));
            }

            std::vector<std::uint32_t> hashes;
            hashes.reserve(hash_count);
            for (std::size_t i = 0; i < hash_count; ++i) {
                hashes.push_back(u32_at(d, hashes_off + 12 + i * 4));
            }

            std::vector<std::vector<AnmFrame>> per_track(
                std::min(hashes.size(), track_count));
            const std::size_t base = frames_off + 12;
            for (std::size_t f = 0; f < frame_count; ++f) {
                for (std::size_t t = 0; t < track_count; ++t) {
                    const std::size_t o = base + (f * track_count + t) * 6;
                    if (o + 6 > d.size()) throw std::runtime_error("anm v5: frames truncated");
                    if (t < per_track.size()) {
                        per_track[t].push_back(AnmFrame{u16_at(d, o), u16_at(d, o + 2),
                                                        u16_at(d, o + 4)});
                    }
                }
            }
            for (std::size_t i = 0; i < per_track.size(); ++i) {
                result.tracks_[hashes[i]] = std::move(per_track[i]);
            }
        } else {
            const std::size_t vec_count = (quat_off - vector_off) / 12;
            const std::size_t quat_count = (frames_off - quat_off) / 16;

            result.vec_pal_.reserve(vec_count);
            for (std::size_t i = 0; i < vec_count; ++i) {
                result.vec_pal_.push_back(vec3_at(d, vector_off + 12 + i * 12));
            }
            result.quat_pal_.reserve(quat_count);
            for (std::size_t i = 0; i < quat_count; ++i) {
                result.quat_pal_.push_back(normalize(quat_at(d, quat_off + 12 + i * 16)));
            }

            std::unordered_map<std::uint32_t, std::size_t> index_of;
            const std::size_t base = frames_off + 12;
            for (std::size_t f = 0; f < frame_count; ++f) {
                for (std::size_t t = 0; t < track_count; ++t) {
                    const std::size_t o = base + (f * track_count + t) * 12;
                    if (o + 12 > d.size()) throw std::runtime_error("anm v4: frames truncated");
                    const std::uint32_t hash = u32_at(d, o);
                    const AnmFrame frame{u16_at(d, o + 4), u16_at(d, o + 6), u16_at(d, o + 8)};
                    auto& frames = result.tracks_[hash];
                    frames.resize(f);
                    frames.push_back(frame);
                    (void)index_of;
                }
            }
        }
        return result;
    }

    if (version == 3) {
        const std::size_t track_count = u32_at(d, 0x10);
        const std::size_t frame_count = u32_at(d, 0x14);
        const std::uint32_t fps_bits = u32_at(d, 0x18);
        float fps = 0.0f;
        std::memcpy(&fps, &fps_bits, sizeof(fps));
        result.fps_ = fps > 0.0f ? fps : 30.0f;
        result.frame_count_ = frame_count;
        result.duration_ = frame_count / result.fps_;
        result.vec_pal_.push_back(Vec3{1.0f, 1.0f, 1.0f});

        std::size_t o = 28;
        for (std::size_t t = 0; t < track_count; ++t) {
            if (o + 36 > d.size()) break;
            std::size_t name_end = o;
            while (name_end < o + 32 && d[name_end] != 0) ++name_end;
            const std::string name(reinterpret_cast<const char*>(d.data() + o), name_end - o);
            o += 36;  // 32-byte name + u32 flags

            // ELF hash of the joint name is the animation key in v3.
            std::uint32_t hash = 0;
            for (unsigned char ch : name) {
                hash = (hash << 4) + ch;
                const std::uint32_t g = hash & 0xF0000000u;
                if (g != 0) hash ^= g >> 24;
                hash &= ~g;
            }

            std::vector<AnmFrame> frames;
            frames.reserve(frame_count);
            for (std::size_t f = 0; f < frame_count; ++f) {
                if (o + 28 > d.size()) break;
                const Quat q = normalize(quat_at(d, o));
                const Vec3 tr = vec3_at(d, o + 16);
                o += 28;

                const auto rid = static_cast<std::uint16_t>(result.quat_pal_.size());
                result.quat_pal_.push_back(q);
                const auto tid = static_cast<std::uint16_t>(result.vec_pal_.size());
                result.vec_pal_.push_back(tr);
                frames.push_back(AnmFrame{tid, 0, rid});
            }
            result.tracks_[hash] = std::move(frames);
        }
        return result;
    }

    throw std::runtime_error("anm: unsupported version " + std::to_string(version));
}

std::unordered_map<std::uint32_t, AnmKey> Anm::evaluate(float time) const {
    std::unordered_map<std::uint32_t, AnmKey> out;
    if (frame_count_ == 0) return out;

    const float clamped = std::clamp(time, 0.0f, std::max(duration_, 0.0f));
    const float position = clamped * fps_;
    const std::size_t a = std::min(static_cast<std::size_t>(position), frame_count_ - 1);
    const std::size_t b = std::min(a + 1, frame_count_ - 1);
    const float fraction = position - static_cast<float>(a);

    for (const auto& [hash, frames] : tracks_) {
        if (a >= frames.size()) continue;
        const AnmFrame fa = frames[a];
        const AnmFrame fb = b < frames.size() ? frames[b] : fa;

        const Quat ra = fa.r < quat_pal_.size() ? quat_pal_[fa.r] : Quat{};
        const Quat rb = fb.r < quat_pal_.size() ? quat_pal_[fb.r] : ra;
        const Vec3 ta = fa.t < vec_pal_.size() ? vec_pal_[fa.t] : Vec3{};
        const Vec3 tb = fb.t < vec_pal_.size() ? vec_pal_[fb.t] : ta;
        const Vec3 sa = fa.s < vec_pal_.size() ? vec_pal_[fa.s] : Vec3{1.0f, 1.0f, 1.0f};
        const Vec3 sb = fb.s < vec_pal_.size() ? vec_pal_[fb.s] : sa;

        AnmKey key;
        key.rotation = (a == b || fraction < 1e-5f) ? ra : slerp(ra, rb, fraction);
        key.translation = (a == b || fraction < 1e-5f) ? ta : lerp3(ta, tb, fraction);
        key.scale = (a == b || fraction < 1e-5f) ? sa : lerp3(sa, sb, fraction);
        out[hash] = key;
    }
    return out;
}

}  // namespace lolview
