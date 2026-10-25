#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include <lolview/pose.hpp>

namespace lolview {

// One key of one transform of one joint (10 bytes in the file).
struct CompressedFrame {
    std::uint16_t time = 0;
    std::uint16_t joint = 0;
    std::uint8_t transform = 0;  // 0 rotation, 1 translation, 2 scale
    std::uint16_t value[3] = {0, 0, 0};
};

// League's "r3d2canm" container: keyframe-compressed animations. Frames are
// shared per joint/transform and sampled through per-joint jump caches of four
// control points, interpolated with Catmull-Rom.
struct CompressedAnimation {
    std::uint32_t version = 0;
    std::uint32_t flags = 0;
    std::uint32_t joint_count = 0;
    std::uint32_t frame_count = 0;
    std::uint32_t jump_cache_count = 0;
    float duration = 0.0f;
    float fps = 0.0f;

    float translation_min[3] = {0, 0, 0};
    float translation_max[3] = {0, 0, 0};
    float scale_min[3] = {1, 1, 1};
    float scale_max[3] = {1, 1, 1};

    std::vector<std::uint32_t> joint_hashes;
    std::vector<CompressedFrame> frames;
    // Raw jump caches: jump_cache_count * joint_count records, 24 bytes each
    // (4 x u16 per transform) or 48 bytes when frame_count >= 0x10001.
    std::vector<std::uint8_t> jump_caches;
    std::uint32_t jump_frame_size = 24;

    bool parametrized() const noexcept { return (flags & 4u) != 0; }
};

bool is_compressed_animation(std::span<const std::uint8_t> bytes);
CompressedAnimation parse_compressed_animation(std::span<const std::uint8_t> bytes);

// Decompresses the 48-bit quantised quaternion stored in three u16 values.
void decompress_quat(const std::uint16_t value[3], float out_xyzw[4]);

// Samples every joint at `time` (seconds), returning local transforms keyed by
// joint name hash. Jump caches give four control points per joint and transform;
// they are interpolated with Catmull-Rom (parametrised when the clip says so).
std::map<std::uint32_t, TrsKey> evaluate_compressed(const CompressedAnimation& anim,
                                                    float time);

}  // namespace lolview
