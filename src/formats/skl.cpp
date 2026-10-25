#include <lolview/skl.hpp>

#include <cstring>
#include <stdexcept>
#include <string>

namespace lolview {
namespace {

std::uint16_t u16_at(std::span<const std::uint8_t> d, std::size_t o) {
    return static_cast<std::uint16_t>(d[o] | (d[o + 1] << 8));
}
std::int16_t i16_at(std::span<const std::uint8_t> d, std::size_t o) {
    return static_cast<std::int16_t>(u16_at(d, o));
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
std::string cstr(std::span<const std::uint8_t> d, std::size_t o) {
    if (o == 0 || o >= d.size()) return {};
    std::size_t end = o;
    while (end < d.size() && d[end] != 0) ++end;
    return std::string(reinterpret_cast<const char*>(d.data() + o), end - o);
}

}  // namespace

Skl Skl::parse(std::span<const std::uint8_t> d) {
    if (d.size() < 0x40) {
        throw std::runtime_error("skl: file too small");
    }
    if (u32_at(d, 4) != kToken) {
        throw std::runtime_error("skl: bad token (expected 0x22FD4FC3)");
    }
    const std::uint32_t version = u32_at(d, 8);
    if (version != 0) {
        throw std::runtime_error("skl: unsupported version " + std::to_string(version));
    }

    const std::size_t joint_count = u16_at(d, 0x0e);
    const std::size_t influence_count = u32_at(d, 0x10);
    const std::int32_t joints_off = i32_at(d, 0x14);
    const std::int32_t indices_off = i32_at(d, 0x18);
    const std::int32_t influences_off = i32_at(d, 0x1c);
    if (joints_off <= 0) {
        throw std::runtime_error("skl: missing joint array");
    }

    Skl result;
    result.joints_.reserve(joint_count);
    for (std::size_t i = 0; i < joint_count; ++i) {
        const std::size_t o = static_cast<std::size_t>(joints_off) + i * 100;
        if (o + 100 > d.size()) {
            throw std::runtime_error("skl: joint array truncated");
        }
        const std::int32_t name_rel = i32_at(d, o + 96);
        const std::int64_t name_pos = static_cast<std::int64_t>(o) + 96 + name_rel;

        SklJoint joint;
        joint.name = name_pos >= 0 ? cstr(d, static_cast<std::size_t>(name_pos)) : std::string();
        joint.parent = i16_at(d, o + 4);
        joint.hash = u32_at(d, o + 8);
        joint.local_translation = vec3_at(d, o + 16);
        joint.local_scale = vec3_at(d, o + 28);
        joint.local_rotation = normalize(quat_at(d, o + 40));
        joint.bind_translation = vec3_at(d, o + 56);
        joint.bind_scale = vec3_at(d, o + 68);
        joint.bind_rotation = normalize(quat_at(d, o + 80));
        result.joints_.push_back(std::move(joint));
    }

    result.hashes_.assign(result.joints_.size(), 0);
    if (indices_off > 0) {
        for (std::size_t i = 0; i < joint_count; ++i) {
            const std::size_t o = static_cast<std::size_t>(indices_off) + i * 8;
            if (o + 8 > d.size()) break;
            const std::int16_t id = i16_at(d, o);
            const std::uint32_t hash = u32_at(d, o + 4);
            if (id >= 0 && static_cast<std::size_t>(id) < result.hashes_.size()) {
                result.hashes_[static_cast<std::size_t>(id)] = hash;
            }
        }
    }
    if (result.hashes_.empty() || [&] {
            for (std::uint32_t h : result.hashes_) {
                if (h != 0) return false;
            }
            return true;
        }()) {
        for (std::size_t i = 0; i < result.joints_.size(); ++i) {
            result.hashes_[i] = result.joints_[i].hash;
        }
    }

    if (influences_off > 0 && influence_count > 0) {
        result.influences_.reserve(influence_count);
        for (std::size_t i = 0; i < influence_count; ++i) {
            const std::size_t o = static_cast<std::size_t>(influences_off) + i * 2;
            if (o + 2 > d.size()) break;
            result.influences_.push_back(i16_at(d, o));
        }
    }
    return result;
}

int Skl::find_joint(std::uint32_t hash) const noexcept {
    for (std::size_t i = 0; i < hashes_.size(); ++i) {
        if (hashes_[i] == hash) return static_cast<int>(i);
    }
    return -1;
}

std::vector<Mat4> Skl::bind_globals() const {
    const std::size_t n = joints_.size();
    std::vector<Mat4> out(n);
    std::vector<bool> done(n, false);

    // Iterative resolution so deep hierarchies cannot overflow the stack.
    std::vector<std::size_t> stack;
    for (std::size_t start = 0; start < n; ++start) {
        if (done[start]) continue;
        stack.clear();
        std::size_t current = start;
        while (true) {
            if (done[current]) break;
            stack.push_back(current);
            const std::int16_t parent = joints_[current].parent;
            if (parent < 0 || static_cast<std::size_t>(parent) >= n) break;
            if (done[static_cast<std::size_t>(parent)]) break;
            if (stack.size() > n) {
                throw std::runtime_error("skl: joint hierarchy contains a cycle");
            }
            current = static_cast<std::size_t>(parent);
        }

        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            const std::size_t index = *it;
            const auto& joint = joints_[index];
            const Mat4 local = trs_matrix(joint.local_translation, joint.local_scale,
                                          joint.local_rotation);
            const std::int16_t parent = joint.parent;
            if (parent >= 0 && static_cast<std::size_t>(parent) < n && done[parent]) {
                out[index] = out[static_cast<std::size_t>(parent)] * local;
            } else {
                out[index] = local;
            }
            done[index] = true;
        }
    }
    return out;
}

}  // namespace lolview
