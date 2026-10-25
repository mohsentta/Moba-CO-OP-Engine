#pragma once

#include <vector>

#include <lolview/math.hpp>

namespace lolview {

class Skl;
class Anm;

// Skinning matrices for the pose at `time` (nullptr animation = bind pose).
// The result is indexed by *influence*, matching the bone indices stored in the
// mesh; skeleton.influences() maps each influence to its joint. Falls back to
// joint indexing when the skeleton has no influence table.
std::vector<Mat4> compute_skin_matrices(const Skl& skeleton, const Anm* animation, float time);

}  // namespace lolview
