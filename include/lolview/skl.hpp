#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <lolview/math.hpp>

namespace lolview {

struct SklJoint {
    std::string name;
    std::int16_t parent = -1;
    std::uint32_t hash = 0;
    Vec3 local_translation{};
    Vec3 local_scale{1.0f, 1.0f, 1.0f};
    Quat local_rotation{};
    Vec3 bind_translation{};
    Vec3 bind_scale{1.0f, 1.0f, 1.0f};
    Quat bind_rotation{};
};

// `.skl` skeleton (Riot r3d2 rig):
//   u32 file_size, u32 token 0x22FD4FC3, u32 version 0,
//   u16 flags, u16 joint_count, u32 influences_count,
//   i32 joints_off, joint_indices_off, influences_off,
//       name_off, asset_name_off, bone_names_off, extension[5]
// Joints are 100 bytes each; the name offset is relative to that field.
class Skl {
public:
    static constexpr std::uint32_t kToken = 0x22FD4FC3;

    static Skl parse(std::span<const std::uint8_t> bytes);

    const std::vector<SklJoint>& joints() const noexcept { return joints_; }
    std::vector<Mat4> bind_globals() const;

    // Mesh vertices index this table (not the joint array): each entry is the
    // joint that influence belongs to.
    const std::vector<std::int16_t>& influences() const noexcept { return influences_; }

    // Joint index for an animation joint hash, or -1.
    int find_joint(std::uint32_t hash) const noexcept;

private:
    std::vector<SklJoint> joints_;
    std::vector<std::uint32_t> hashes_;
    std::vector<std::int16_t> influences_;
};

}  // namespace lolview
