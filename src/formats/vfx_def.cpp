#include <lolview/vfx_def.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lolview {
namespace {

constexpr std::uint32_t kVfxSystemType = fnv1a("VfxSystemDefinitionData");

const BinValue* field_of(const BinValue& value, std::string_view name) {
    return find_field(value, name);
}

// Many emitter parameters are wrapped: { constantValue: float } or
// { dynamics: { times, values } }.
float constant_float(const BinValue* value, float fallback) {
    if (value == nullptr) return fallback;
    if (const BinValue* constant = field_of(*value, "constantValue")) {
        return as_float(constant, fallback);
    }
    return fallback;
}

Vec3 constant_vec3(const BinValue* value, Vec3 fallback) {
    if (value == nullptr) return fallback;
    const BinValue* constant = field_of(*value, "constantValue");
    if (constant == nullptr) return fallback;
    return Vec3{constant->vec[0], constant->vec[1], constant->vec[2]};
}

std::array<float, 4> constant_vec4(const BinValue* value,
                                   std::array<float, 4> fallback) {
    if (value == nullptr) return fallback;
    const BinValue* constant = field_of(*value, "constantValue");
    if (constant == nullptr) return fallback;
    return {constant->vec[0], constant->vec[1], constant->vec[2], constant->vec[3]};
}

void read_color_curve(const BinValue& color_field, VfxEmitter& emitter) {
    const BinValue* dynamics = field_of(color_field, "dynamics");
    if (dynamics == nullptr) return;
    const BinValue* times = field_of(*dynamics, "times");
    const BinValue* values = field_of(*dynamics, "values");
    if (times == nullptr || values == nullptr) return;

    for (const auto& item : times->items) {
        emitter.color_times.push_back(static_cast<float>(item.number));
    }
    for (const auto& item : values->items) {
        emitter.color_values.push_back({item.vec[0], item.vec[1], item.vec[2], item.vec[3]});
    }
}

std::array<float, 4> sample_color(const VfxEmitter& emitter, float age01) {
    if (emitter.color_times.empty() || emitter.color_values.empty()) {
        return emitter.birth_color;
    }
    const std::size_t count =
        std::min(emitter.color_times.size(), emitter.color_values.size());
    if (count == 1) return emitter.color_values[0];

    if (age01 <= emitter.color_times[0]) return emitter.color_values[0];
    for (std::size_t i = 1; i < count; ++i) {
        if (age01 <= emitter.color_times[i]) {
            const float span = emitter.color_times[i] - emitter.color_times[i - 1];
            const float t = span > 1e-6f ? (age01 - emitter.color_times[i - 1]) / span : 0.0f;
            std::array<float, 4> result{};
            for (int k = 0; k < 4; ++k) {
                result[static_cast<std::size_t>(k)] =
                    emitter.color_values[i - 1][static_cast<std::size_t>(k)] * (1.0f - t) +
                    emitter.color_values[i][static_cast<std::size_t>(k)] * t;
            }
            return result;
        }
    }
    return emitter.color_values[count - 1];
}

VfxEmitter read_emitter(const BinValue& node) {
    VfxEmitter emitter;
    emitter.name = as_string(field_of(node, "emitterName"));
    emitter.rate = constant_float(field_of(node, "rate"), 0.0f);
    emitter.lifetime = constant_float(field_of(node, "particleLifetime"), 1.0f);
    emitter.texture = as_string(field_of(node, "texture"));
    emitter.blend_mode = as_int(field_of(node, "blendMode"), 1);
    emitter.birth_scale = constant_vec3(field_of(node, "birthScale0"), {1.0f, 1.0f, 1.0f});
    emitter.single_particle = as_bool(field_of(node, "isSingleParticle"), false);
    emitter.disabled = as_bool(field_of(node, "disabled"), false);

    emitter.birth_color = constant_vec4(field_of(node, "birthColor"),
                                        {1.0f, 1.0f, 1.0f, 1.0f});
    emitter.birth_velocity = constant_vec3(field_of(node, "birthVelocity"), {});

    if (const BinValue* shape = field_of(node, "SpawnShape")) {
        emitter.emit_offset = constant_vec3(field_of(*shape, "emitOffset"), {});
    }
    if (const BinValue* color = field_of(node, "Color")) {
        read_color_curve(*color, emitter);
    }
    return emitter;
}

}  // namespace

std::vector<VfxSystem> extract_vfx_systems(const BinDocument& document) {
    std::vector<VfxSystem> systems;
    for (const auto& entry : document.entries()) {
        if (entry.type_hash != kVfxSystemType) continue;

        VfxSystem system;
        system.name = as_string(find_field(entry.fields, "particleName"));
        system.path = as_string(find_field(entry.fields, "particlePath"));
        if (system.name.empty()) continue;

        const BinValue* emitters = find_field(entry.fields, "complexEmitterDefinitionData");
        if (emitters != nullptr) {
            for (const auto& node : emitters->items) {
                if (node.type != BinType::Struct) continue;
                system.emitters.push_back(read_emitter(node));
            }
        }
        systems.push_back(std::move(system));
    }
    return systems;
}

const VfxSystem* find_vfx_system(const std::vector<VfxSystem>& systems,
                                 std::string_view name) {
    for (const auto& system : systems) {
        if (system.name == name) return &system;
    }
    return nullptr;
}

const VfxEmitter* pick_textured_emitter(const VfxSystem& system) {
    for (const auto& emitter : system.emitters) {
        if (!emitter.texture.empty() && !emitter.disabled) return &emitter;
    }
    return nullptr;
}

const VfxEmitter* dominant_emitter(const VfxSystem& system) {
    const VfxEmitter* best = nullptr;
    float best_score = -1.0f;
    for (const auto& emitter : system.emitters) {
        if (emitter.disabled || emitter.texture.empty()) continue;
        const float score = emitter.rate * std::max(0.05f, emitter.lifetime);
        if (score > best_score) {
            best_score = score;
            best = &emitter;
        }
    }
    return best != nullptr ? best : pick_textured_emitter(system);
}

std::vector<Particle> simulate_vfx(const VfxSystem& system, float time, const Vec3& origin,
                                   const Vec3& forward, float pixels_per_unit,
                                   float projectile_speed) {
    std::vector<Particle> particles;
    if (time < 0.0f) return particles;

    for (std::size_t emitter_index = 0; emitter_index < system.emitters.size();
         ++emitter_index) {
        const auto& emitter = system.emitters[emitter_index];
        if (emitter.disabled) continue;

        const float lifetime = std::max(0.05f, emitter.lifetime);
        const float rate = emitter.rate;
        const float size_px = std::clamp(
            std::max(emitter.birth_scale.x, emitter.birth_scale.y) * pixels_per_unit * 0.55f,
            6.0f, 120.0f);
        // Additive emitters stack quickly; tame them since blending is global.
        const float alpha_scale = emitter.blend_mode == 4 ? 0.5f : 0.9f;

        if (emitter.single_particle || rate <= 0.0f) {
            if (time > lifetime) continue;
            const float age01 = time / lifetime;
            const auto color = sample_color(emitter, age01);
            Particle particle;
            particle.position = origin + forward * (projectile_speed * time) +
                                emitter.emit_offset;
            particle.size = size_px;
            particle.r = color[0];
            particle.g = color[1];
            particle.b = color[2];
            particle.a = color[3] * alpha_scale;
            particle.slot = static_cast<int>(emitter_index);
            particles.push_back(particle);
            continue;
        }

        const int max_particles = 256;
        const int emitted = std::min(max_particles, static_cast<int>(time * rate) + 1);
        for (int i = 0; i < emitted; ++i) {
            const float spawn_time = static_cast<float>(i) / rate;
            if (spawn_time > time) break;
            const float age = time - spawn_time;
            if (age > lifetime) continue;

            const float age01 = age / lifetime;
            auto color = sample_color(emitter, age01);

            Particle particle;
            particle.position = origin + forward * (projectile_speed * spawn_time) +
                                emitter.emit_offset +
                                forward * (emitter.birth_velocity.z * age);
            particle.position.y += emitter.birth_velocity.y * age;
            particle.position.x += emitter.birth_velocity.x * age;
            particle.size = size_px;
            particle.r = color[0];
            particle.g = color[1];
            particle.b = color[2];
            particle.a = color[3] * alpha_scale;
            particle.slot = static_cast<int>(emitter_index);
            particles.push_back(particle);
        }
    }
    return particles;
}

}  // namespace lolview
