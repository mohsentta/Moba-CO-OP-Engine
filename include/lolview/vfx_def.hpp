#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include <lolview/bin.hpp>
#include <lolview/math.hpp>
#include <lolview/vfx.hpp>

namespace lolview {

// One emitter of a Riot VFX system, decoded from a PROP bin.
struct VfxEmitter {
    std::string name;
    float rate = 0.0f;
    float lifetime = 1.0f;
    Vec3 emit_offset{};
    Vec3 birth_velocity{};
    std::array<float, 4> birth_color{1.0f, 1.0f, 1.0f, 1.0f};
    std::vector<float> color_times;
    std::vector<std::array<float, 4>> color_values;
    std::string texture;
    int blend_mode = 1;
    Vec3 birth_scale{1.0f, 1.0f, 1.0f};  // world-space width/height/depth
    bool single_particle = false;
    bool disabled = false;
};

struct VfxSystem {
    std::string name;
    std::string path;
    std::vector<VfxEmitter> emitters;
};

// Every entry of type VfxSystemDefinitionData in the bin.
std::vector<VfxSystem> extract_vfx_systems(const BinDocument& document);
const VfxSystem* find_vfx_system(const std::vector<VfxSystem>& systems, std::string_view name);
const VfxEmitter* pick_textured_emitter(const VfxSystem& system);

// The emitter that contributes most particles (used to pick one sprite texture
// for the whole system when only a single texture can be bound).
const VfxEmitter* dominant_emitter(const VfxSystem& system);

// Deterministic simulation of one system at `time` seconds after cast.
// `pixels_per_unit` converts emitter scales (world units) to point sizes and
// `projectile_speed` moves the emit origin along `forward` over time, which is
// what turns a trail emitter into a trail.
std::vector<Particle> simulate_vfx(const VfxSystem& system, float time, const Vec3& origin,
                                   const Vec3& forward, float pixels_per_unit,
                                   float projectile_speed = 0.0f);

}  // namespace lolview
