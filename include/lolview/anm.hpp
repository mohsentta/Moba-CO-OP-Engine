#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include <lolview/math.hpp>

namespace lolview {

struct AnmFrame {
    std::uint16_t t = 0;
    std::uint16_t s = 0;
    std::uint16_t r = 0;
};

struct AnmKey {
    Quat rotation{};
    Vec3 translation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};
};

// `.anm` animation (Riot r3d2anmd), versions 3/4/5.
//
// Palette-based: every frame stores u16 indices into shared translation/scale
// and rotation palettes. v5 uses 6-byte quantised quaternions and keeps joint
// hashes in their own section; v4 stores the hash per frame; v3 stores 32-byte
// joint names and raw quaternions.
class Anm {
public:
    static Anm parse(std::span<const std::uint8_t> bytes);

    std::uint32_t version() const noexcept { return version_; }
    float fps() const noexcept { return fps_; }
    float duration() const noexcept { return duration_; }
    std::size_t frame_count() const noexcept { return frame_count_; }
    std::size_t track_count() const noexcept { return tracks_.size(); }

    // Per-joint local transform at `time`, keyed by joint hash.
    std::unordered_map<std::uint32_t, AnmKey> evaluate(float time) const;

private:
    std::uint32_t version_ = 0;
    float fps_ = 30.0f;
    float duration_ = 0.0f;
    std::size_t frame_count_ = 0;
    std::vector<Vec3> vec_pal_;
    std::vector<Quat> quat_pal_;
    std::unordered_map<std::uint32_t, std::vector<AnmFrame>> tracks_;
};

}  // namespace lolview
