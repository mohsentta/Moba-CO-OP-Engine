#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include <lolview/math.hpp>

namespace lolview {

class Skl;
class Anm;

// One joint's local transform at a sampled time.
struct TrsKey {
    Vec3 translation{0.0f, 0.0f, 0.0f};
    Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};
};

// Skinning matrices for the pose at `time` (nullptr animation = bind pose).
// The result is indexed by *influence*, matching the bone indices stored in the
// mesh; skeleton.influences() maps each influence to its joint. Falls back to
// joint indexing when the skeleton has no influence table.
std::vector<Mat4> compute_skin_matrices(const Skl& skeleton, const Anm* animation, float time);

// Same output, for callers that already sampled local transforms (compressed
// animations live in their own container and decode straight into this form).
std::vector<Mat4> compute_skin_matrices_from_keys(
    const Skl& skeleton, const std::map<std::uint32_t, TrsKey>& keys);

}  // namespace lolview
