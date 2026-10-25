#include <lolview/vfx.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lolview {
namespace {

// Deterministic hash -> [0,1). Keeps every frame reproducible without RNG state.
float hash01(std::uint32_t n) {
    n ^= n >> 16;
    n *= 0x7FEB352Du;
    n ^= n >> 15;
    n *= 0x846CA68Bu;
    n ^= n >> 16;
    return static_cast<float>(n & 0xFFFFFFu) / static_cast<float>(0x1000000);
}

Vec3 jitter(std::uint32_t seed, float scale) {
    return Vec3{(hash01(seed) * 2.0f - 1.0f) * scale,
                (hash01(seed * 7919u + 13u) * 2.0f - 1.0f) * scale,
                (hash01(seed * 104729u + 7u) * 2.0f - 1.0f) * scale};
}

Vec3 sphere_direction(std::uint32_t seed) {
    const float z = hash01(seed) * 2.0f - 1.0f;
    const float phi = hash01(seed * 31337u + 3u) * 6.2831853f;
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return Vec3{r * std::cos(phi), z, r * std::sin(phi)};
}

// A moving projectile plus a trail of sparks behind it.
void add_projectile(std::vector<Particle>& out, float time, const Vec3& origin,
                    const Vec3& forward, float speed, float life, float size,
                    const Vec3& color, std::uint32_t seed) {
    const float travel = speed * std::min(time, life);
    if (time <= life) {
        Particle core;
        core.position = origin + forward * travel;
        core.size = size;
        core.r = color.x;
        core.g = color.y;
        core.b = color.z;
        core.a = 0.95f;
        out.push_back(core);
    }

    constexpr float kTrailInterval = 0.018f;
    const int steps = static_cast<int>(time / kTrailInterval);
    for (int i = 0; i < steps; ++i) {
        const float age = time - static_cast<float>(i) * kTrailInterval;
        if (age > 0.55f) continue;
        const float spawn_time = time - age;
        if (spawn_time > life) continue;

        Particle spark;
        spark.position = origin + forward * (speed * spawn_time) + jitter(seed + static_cast<std::uint32_t>(i), 7.0f);
        spark.size = size * (0.35f + age * 0.6f);
        spark.a = std::max(0.0f, 1.0f - age / 0.55f) * 0.8f;
        spark.r = color.x;
        spark.g = color.y;
        spark.b = color.z;
        out.push_back(spark);
    }
}

// A radial burst of sparks from a point.
void add_burst(std::vector<Particle>& out, float time, const Vec3& origin, float speed,
               float life, float size, const Vec3& color, int count, std::uint32_t seed) {
    if (time > life) return;
    for (int i = 0; i < count; ++i) {
        const Vec3 dir = sphere_direction(seed + static_cast<std::uint32_t>(i) * 17u);
        Particle spark;
        spark.position = origin + dir * (speed * time);
        spark.size = size * (1.0f - time / life * 0.5f);
        spark.a = std::max(0.0f, 1.0f - time / life);
        spark.r = color.x;
        spark.g = color.y;
        spark.b = color.z;
        out.push_back(spark);
    }
}

}  // namespace

EffectKind effect_from_name(std::string_view name) {
    if (name == "q" || name == "spell1" || name == "Q") return EffectKind::Spell1;
    if (name == "w" || name == "spell2" || name == "W") return EffectKind::Spell2;
    if (name == "e" || name == "spell3" || name == "E") return EffectKind::Spell3;
    if (name == "r" || name == "spell4" || name == "R") return EffectKind::Spell4;
    return EffectKind::None;
}

std::vector<Particle> simulate_effect(EffectKind kind, float time, const Vec3& origin,
                                      const Vec3& forward) {
    std::vector<Particle> particles;
    if (kind == EffectKind::None || time < 0.0f) return particles;

    switch (kind) {
        case EffectKind::Spell1:
            add_projectile(particles, time, origin, forward, 150.0f, 2.2f, 55.0f,
                           Vec3{0.55f, 0.72f, 1.0f}, 1234u);
            add_burst(particles, time, origin + forward * 18.0f, 26.0f, 0.7f, 22.0f,
                      Vec3{0.45f, 0.62f, 1.0f}, 22, 99u);
            break;
        case EffectKind::Spell2:
            add_burst(particles, time, origin + Vec3{0.0f, -40.0f, 0.0f}, 85.0f, 1.1f, 28.0f,
                      Vec3{0.85f, 0.45f, 1.0f}, 60, 555u);
            break;
        case EffectKind::Spell3:
            add_projectile(particles, time, origin, forward, 95.0f, 2.6f, 44.0f,
                           Vec3{1.0f, 0.42f, 0.72f}, 777u);
            break;
        case EffectKind::Spell4:
            add_projectile(particles, time, origin, forward, 320.0f, 1.6f, 40.0f,
                           Vec3{0.9f, 0.7f, 1.0f}, 4242u);
            add_burst(particles, time, origin + forward * 40.0f, 45.0f, 0.8f, 26.0f,
                      Vec3{0.75f, 0.55f, 1.0f}, 30, 31u);
            break;
        case EffectKind::None:
            break;
    }
    return particles;
}

}  // namespace lolview
