#include <lolview/pose.hpp>

#include <lolview/anm.hpp>
#include <lolview/skl.hpp>

#include <cstddef>
#include <stdexcept>

namespace lolview {
namespace {

std::vector<Mat4> globals_from_locals(const Skl& skeleton,
                                      const std::vector<Mat4>& locals) {
    const std::size_t n = locals.size();
    std::vector<Mat4> globals(n);
    std::vector<bool> done(n, false);
    std::vector<std::size_t> stack;

    for (std::size_t start = 0; start < n; ++start) {
        if (done[start]) continue;
        stack.clear();
        std::size_t current = start;
        while (!done[current]) {
            stack.push_back(current);
            const std::int16_t parent = skeleton.joints()[current].parent;
            if (parent < 0 || static_cast<std::size_t>(parent) >= n) break;
            if (done[static_cast<std::size_t>(parent)]) break;
            if (stack.size() > n) throw std::runtime_error("hierarchy contains a cycle");
            current = static_cast<std::size_t>(parent);
        }

        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            const std::size_t index = *it;
            const std::int16_t parent = skeleton.joints()[index].parent;
            if (parent >= 0 && static_cast<std::size_t>(parent) < n && done[parent]) {
                globals[index] = globals[static_cast<std::size_t>(parent)] * locals[index];
            } else {
                globals[index] = locals[index];
            }
            done[index] = true;
        }
    }
    return globals;
}

}  // namespace

std::vector<Mat4> compute_skin_matrices(const Skl& skeleton, const Anm* animation,
                                        float time) {
    const auto& joints = skeleton.joints();
    const std::size_t n = joints.size();

    std::vector<Mat4> locals(n);
    if (animation == nullptr) {
        for (std::size_t i = 0; i < n; ++i) {
            locals[i] = trs_matrix(joints[i].local_translation, joints[i].local_scale,
                                   joints[i].local_rotation);
        }
    } else {
        const auto keys = animation->evaluate(time);
        for (std::size_t i = 0; i < n; ++i) {
            const auto it = keys.find(joints[i].hash);
            if (it != keys.end()) {
                locals[i] = trs_matrix(it->second.translation, it->second.scale,
                                       it->second.rotation);
            } else {
                locals[i] = trs_matrix(joints[i].local_translation, joints[i].local_scale,
                                       joints[i].local_rotation);
            }
        }
    }

    const std::vector<Mat4> animated = globals_from_locals(skeleton, locals);
    const std::vector<Mat4> bind = skeleton.bind_globals();

    std::vector<Mat4> skin(n);
    for (std::size_t i = 0; i < n; ++i) {
        skin[i] = animated[i] * inverse_affine(bind[i]);
    }

    // Vertices reference the influence table, not the joint array.
    const auto& influences = skeleton.influences();
    if (influences.empty()) return skin;

    std::vector<Mat4> by_influence(influences.size(), Mat4::identity());
    for (std::size_t i = 0; i < influences.size(); ++i) {
        const std::int16_t joint = influences[i];
        if (joint >= 0 && static_cast<std::size_t>(joint) < n) {
            by_influence[i] = skin[static_cast<std::size_t>(joint)];
        }
    }
    return by_influence;
}

}  // namespace lolview
