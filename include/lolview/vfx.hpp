#pragma once

#include <string_view>
#include <vector>

#include <lolview/math.hpp>

namespace lolview {

struct Particle {
    Vec3 position;
    float size = 8.0f;  // point size in pixels
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
    int slot = 0;  // emitter index, for per-emitter textures
};

// The practice sandbox ships its own effect definitions rather than reusing
// Riot's particle graphs: those are stored in graph descriptions whose location
// and encoding are not yet reversed. Everything here is procedural, so nothing
// is copied from the game.
enum class EffectKind { None, Spell1, Spell2, Spell3, Spell4 };

EffectKind effect_from_name(std::string_view name);

// Deterministic: the same (kind, time) always yields the same particles, which
// is what makes an offscreen PNG a stable regression check.
std::vector<Particle> simulate_effect(EffectKind kind, float time, const Vec3& origin,
                                      const Vec3& forward);

}  // namespace lolview
