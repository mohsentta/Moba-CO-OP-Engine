// lolview practice tool - League-style view.
//
// Loads a champion's mesh, textures, skeleton, animations and VFX out of the
// user's own League of Legends installation and presents a practice-range style
// scene: a locked isometric camera, right-click movement, cursor-aimed casts and
// a game HUD (ability icons, cooldowns, health/mana, target health bar).
//
// Nothing is shipped here; every asset is resolved by hash at runtime.

#include <lolview/anm.hpp>
#include <lolview/anm_compressed.hpp>
#include <lolview/asset_source.hpp>
#include <lolview/bin.hpp>
#include <lolview/mapgeo.hpp>
#include <lolview/net.hpp>
#include <lolview/pose.hpp>
#include <lolview/renderer.hpp>
#include <lolview/skl.hpp>
#include <lolview/skn.hpp>
#include <lolview/tex.hpp>
#include <lolview/vfx.hpp>
#include <lolview/vfx_def.hpp>
#include <lolview/wad.hpp>
#include <lolview/xxhash64.hpp>

#include <SDL3/SDL.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kFovDegrees = 45.0f;

// Camera handedness. Riot's engine is right-handed; our original basis was
// left-handed (right = cross(forward, up)), which mirrored every image.
bool g_right_handed = true;

// Same as lolview::look_at but with a right-handed basis (right = cross(up, f)).
lolview::Mat4 look_at_right_handed(const lolview::Vec3& eye, const lolview::Vec3& center,
                                   const lolview::Vec3& up) {
    const lolview::Vec3 f = lolview::normalize(center - eye);
    const lolview::Vec3 s = lolview::normalize(lolview::cross(up, f));
    const lolview::Vec3 u = lolview::cross(f, s);

    lolview::Mat4 out = lolview::Mat4::identity();
    out.m[0] = s.x;  out.m[4] = s.y;  out.m[8] = s.z;
    out.m[1] = u.x;  out.m[5] = u.y;  out.m[9] = u.z;
    out.m[2] = -f.x; out.m[6] = -f.y; out.m[10] = -f.z;
    out.m[12] = -(s.x * eye.x + s.y * eye.y + s.z * eye.z);
    out.m[13] = -(u.x * eye.x + u.y * eye.y + u.z * eye.z);
    out.m[14] = f.x * eye.x + f.y * eye.y + f.z * eye.z;
    return out;
}

lolview::Mat4 view_matrix(const lolview::Vec3& eye, const lolview::Vec3& center,
                          const lolview::Vec3& up) {
    return g_right_handed ? look_at_right_handed(eye, center, up)
                          : lolview::look_at(eye, center, up);
}

// ---------------------------------------------------------------------------
// League-style camera: fixed pitch, no roll, follows the champion when locked.
// ---------------------------------------------------------------------------
struct LeagueCamera {
    lolview::Vec3 focus{0.0f, 0.0f, 0.0f};  // ground point being looked at
    float distance = 1800.0f;
    float pitch = 55.0f * kPi / 180.0f;
    float yaw = 0.0f;  // 0 looks toward +Z; rotate to view from another side
    bool locked = true;

    lolview::Vec3 eye() const {
        const float cos_pitch = std::cos(pitch);
        return lolview::Vec3{focus.x - std::sin(yaw) * cos_pitch * distance,
                             focus.y + std::sin(pitch) * distance,
                             focus.z - std::cos(yaw) * cos_pitch * distance};
    }
    lolview::Mat4 view() const {
        return view_matrix(eye(), focus, lolview::Vec3{0.0f, 1.0f, 0.0f});
    }
};

struct MapPiece {
    lolview::Skn mesh;
    int mesh_id = -1;
    int texture_id = -1;
    lolview::Vec3 position{};
    lolview::Vec3 initial_position{};
    float yaw = 0.0f;
    float initial_yaw = 0.0f;
    float scale = 1.0f;
    bool visible = true;
    int alpha_mode = 0;
    std::string material;
    lolview::Vec3 center{};
    float radius = 0.0f;
    lolview::Vec3 bounds_min{};
    lolview::Vec3 bounds_max{};
};

std::vector<lolview::CompressedAnimation> g_compressed_clips;
std::map<std::string, std::vector<std::size_t>> g_compressed_by_key;

struct TerrainGroup {
    std::string material;
    std::string texture_path;
    std::vector<lolview::SknVertex> vertices;
    std::vector<std::uint16_t> indices;
    lolview::Skn mesh;
    lolview::TexImage texture;
    bool has_texture = false;
    int alpha_mode = 0;  // 0 opaque, 1 cutout, 2 blended
};

// Free-fly "superman" camera (F2): WASD + mouse, no ground constraint.
struct FreeCamera {
    lolview::Vec3 position{0.0f, 1200.0f, -1000.0f};
    float yaw = 0.0f;
    float pitch = -0.5f;

    lolview::Vec3 forward() const {
        const float cp = std::cos(pitch);
        return lolview::Vec3{std::sin(yaw) * cp, std::sin(pitch), std::cos(yaw) * cp};
    }
    lolview::Mat4 view() const {
        return view_matrix(position, position + forward(),
                           lolview::Vec3{0.0f, 1.0f, 0.0f});
    }
};

struct Unit {
    std::string model;
    const lolview::Skn* mesh = nullptr;  // not owned
    lolview::Vec3 position{};
    float yaw = 0.0f;
    float hp = 100.0f;
    float max_hp = 100.0f;
    int team = 0;
    bool alive = true;
};

struct ActiveEffect {    lolview::EffectKind kind = lolview::EffectKind::None;
    float age = 0.0f;
    lolview::Vec3 origin{};
    lolview::Vec3 direction{0.0f, 0.0f, 1.0f};
    float speed = 700.0f;
    bool hit_applied = false;
    const lolview::VfxSystem* vfx = nullptr;
    const std::vector<lolview::TexImage>* textures = nullptr;
};

struct Args {
    std::string game_dir;
    std::string archive = "Ahri.wad";
    std::string champion = "Ahri";
    std::string vfx_bin;
    std::string pet;
    std::string pet_archive = "Map11";
    bool load_map = false;
    bool save_scene_on_start = false;
    bool start_free_camera = false;
    // Our camera basis was left-handed (looking +Z with up +Y gave right = -X),
    // which mirrored every image. Right-handed is the default; --lh-view keeps
    // the old behaviour for comparison.
    bool right_handed_view = true;
    // Legacy data-level mirror, kept for comparison only.
    bool mirror_x = false;
    std::string scenario_path = "scenario.txt";
    std::string join;
    float auto_attack = 0.0f;
    float auto_spell = 0.0f;
    bool path_test = false;
    bool path_editor = false;
    std::string auto_move;
    std::string force_anim;
    float pick_x = -1.0f;
    float pick_y = -1.0f;
    int select_piece = -1;
    std::string scene_path;
    std::string capture;
    std::string auto_cast;
    float capture_after = -1.0f;
    float exit_after = -1.0f;
    float pitch_degrees = 0.0f;   // 0 = use the default
    float distance = 0.0f;
    float camera_yaw_degrees = 0.0f;
    int width = 1600;
    int height = 900;
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
// Archives are large (Map11 is 2.4 GB), so each one is opened at most once and
// shared by every lookup for the lifetime of the process.
std::map<std::string, std::shared_ptr<lolview::Wad>>& archive_cache() {
    static std::map<std::string, std::shared_ptr<lolview::Wad>> cache;
    return cache;
}

std::shared_ptr<lolview::Wad> open_archive(const std::string& path) {
    auto& cache = archive_cache();
    const auto existing = cache.find(path);
    if (existing != cache.end()) return existing->second;
    auto wad = std::make_shared<lolview::Wad>(lolview::Wad::open(path));
    cache[path] = wad;
    return wad;
}

std::optional<std::vector<std::uint8_t>> resolve_chunk(const std::string& game_dir,
                                                       const std::string& archive_filter,
                                                       const std::string& virtual_path) {
    lolview::AssetSource source(game_dir);
    const std::uint64_t key = lolview::path_hash(virtual_path);
    for (const auto& archive_path : source.archives()) {
        if (archive_path.string().find(archive_filter) == std::string::npos) continue;
        const auto wad = open_archive(archive_path.string());
        const auto index = wad->find(key);
        if (!index) continue;
        return wad->read_chunk(wad->entries()[*index]);
    }
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> resolve_chunk_hash(const std::string& game_dir,
                                                           const std::string& archive_filter,
                                                           std::uint64_t hash) {
    lolview::AssetSource source(game_dir);
    for (const auto& archive_path : source.archives()) {
        if (archive_path.string().find(archive_filter) == std::string::npos) continue;
        const auto wad = open_archive(archive_path.string());
        const auto index = wad->find(hash);
        if (!index) continue;
        return wad->read_chunk(wad->entries()[*index]);
    }
    return std::nullopt;
}

std::shared_ptr<lolview::Wad> find_archive(const std::string& game_dir,
                                           const std::string& filter) {
    lolview::AssetSource source(game_dir);
    for (const auto& path : source.archives()) {
        if (path.string().find(filter) != std::string::npos) return open_archive(path.string());
    }
    return nullptr;
}

// Map materials name a kitpiece (e.g. Ground_E2_ChaosBase_A_MAT); its texture is
// a sibling under <root>/textures/<name>.tex. Variants and kit prefixes make the
// mapping irregular, so several candidates are tried in order.
std::vector<std::string> terrain_texture_candidates(const std::string& material) {
    std::string path = material;
    std::string lowered = path;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const std::size_t marker = lowered.find("/materials/");
    if (marker == std::string::npos) return {};
    const std::string root = lowered.substr(0, marker);
    std::string tail = lowered.substr(marker + 11);  // after "/materials/"
    const std::size_t slash = tail.rfind('/');
    if (slash != std::string::npos) tail = tail.substr(slash + 1);
    if (tail.size() > 4 && tail.compare(tail.size() - 4, 4, "_mat") == 0) {
        tail.resize(tail.size() - 4);
    }

    std::string kit;
    const std::size_t kit_slash = root.rfind('/');
    if (kit_slash != std::string::npos) kit = root.substr(kit_slash + 1) + "_";

    std::vector<std::string> names;
    const auto add_variants = [&](const std::string& base) {
        if (base.empty()) return;
        names.push_back(base);
        names.push_back(base + "_1bitalpha");
        // Variant letter: dragonwing_d -> dragonwing_a (and its alpha cutout).
        if (base.size() > 2 && base[base.size() - 2] == '_') {
            const std::string stem = base.substr(0, base.size() - 2);
            names.push_back(stem);
            names.push_back(stem + "_a");
            names.push_back(stem + "_a_1bitalpha");
            names.push_back(stem + "_1bitalpha");
        }
    };
    add_variants(tail);
    // Kit prefix: chemtech_dragonwing_a -> dragonwing_a.
    if (!kit.empty() && tail.rfind(kit, 0) == 0) add_variants(tail.substr(kit.size()));

    std::vector<std::string> candidates;
    for (const auto& name : names) {
        const std::string candidate = "assets/" + root + "/textures/" + name + ".tex";
        if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end()) {
            candidates.push_back(candidate);
        }
    }
    return candidates;
}

std::string clip_path(const std::string& champion, const std::string& clip) {
    return "ASSETS/Characters/" + champion + "/Skins/Base/Animations/" + clip + ".anm";
}

std::string skin_path(const std::string& champion, const char* suffix) {
    return "ASSETS/Characters/" + champion + "/Skins/Base/" + champion + "_Base" + suffix;
}

// HUD ability icons. Champions disagree on naming: Icons_Ahri_Q.dds vs
// jinx_q1.dds (Jinx even has one Q icon per weapon).
std::vector<std::string> hud_icon_candidates(const std::string& champion, const char* ability,
                                             int variant) {
    std::string lower_champion = champion;
    std::string lower_ability = ability;
    std::transform(lower_champion.begin(), lower_champion.end(), lower_champion.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(lower_ability.begin(), lower_ability.end(), lower_ability.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const std::string base = "ASSETS/Characters/" + champion + "/HUD/Icons2D/";
    std::vector<std::string> candidates;
    candidates.push_back(base + "Icons_" + champion + "_" + ability + ".dds");
    std::string lower_variant = lower_ability + std::to_string(variant);
    candidates.push_back(base + lower_champion + "_" + lower_variant + ".dds");
    candidates.push_back(base + lower_champion + "_" + lower_ability + ".dds");
    candidates.push_back(base + lower_champion + "_" + lower_variant + "_1.dds");
    return candidates;
}

std::string icon_path(const std::string& champion, const char* ability) {    return "ASSETS/Characters/" + champion + "/HUD/Icons2D/Icons_" + champion + "_" +
           ability + ".dds";
}

lolview::Mat4 rotation_y(float angle) {
    lolview::Mat4 m = lolview::Mat4::identity();
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    m.m[0] = c;
    m.m[2] = -s;
    m.m[8] = s;
    m.m[10] = c;
    return m;
}

lolview::Mat4 model_matrix(const lolview::Vec3& position, float yaw) {
    return lolview::translation(position) * rotation_y(yaw);
}

lolview::Vec3 transform_point(const lolview::Mat4& m, const lolview::Vec3& p) {
    return lolview::Vec3{m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
                         m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
                         m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14]};
}

lolview::Mat4 piece_matrix(const lolview::Vec3& position, const lolview::Vec3& center,
                           float yaw, float scale);

// Moller-Trumbore ray/triangle test.
bool ray_triangle(const lolview::Vec3& origin, const lolview::Vec3& direction,
                  const lolview::Vec3& a, const lolview::Vec3& b, const lolview::Vec3& c,
                  float& out_t) {
    const lolview::Vec3 edge1{b.x - a.x, b.y - a.y, b.z - a.z};
    const lolview::Vec3 edge2{c.x - a.x, c.y - a.y, c.z - a.z};
    const lolview::Vec3 p = lolview::cross(direction, edge2);
    const float det = edge1.x * p.x + edge1.y * p.y + edge1.z * p.z;
    if (std::fabs(det) < 1e-8f) return false;
    const float inv = 1.0f / det;
    const lolview::Vec3 s{origin.x - a.x, origin.y - a.y, origin.z - a.z};
    const float u = (s.x * p.x + s.y * p.y + s.z * p.z) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const lolview::Vec3 q = lolview::cross(s, edge1);
    const float v = (direction.x * q.x + direction.y * q.y + direction.z * q.z) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    const float t = (edge2.x * q.x + edge2.y * q.y + edge2.z * q.z) * inv;
    if (t <= 0.0f) return false;
    out_t = t;
    return true;
}

// Every piece under the ray, nearest first. Bounding spheres reject cheaply,
// then real triangles decide, so thin and nested pieces pick correctly.
std::vector<std::pair<float, int>> pick_pieces(const std::vector<MapPiece>& pieces,
                                               const lolview::Vec3& origin,
                                               const lolview::Vec3& direction) {
    std::vector<std::pair<float, int>> hits;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        const auto& piece = pieces[i];
        if (!piece.visible) continue;

        const float cx = piece.position.x + piece.center.x * piece.scale;
        const float cy = piece.position.y + piece.center.y * piece.scale;
        const float cz = piece.position.z + piece.center.z * piece.scale;
        const float ox = cx - origin.x, oy = cy - origin.y, oz = cz - origin.z;
        const float tca = ox * direction.x + oy * direction.y + oz * direction.z;
        if (tca < 0.0f) continue;
        const float d2 = (ox * ox + oy * oy + oz * oz) - tca * tca;
        const float radius = piece.radius * piece.scale;
        if (d2 > radius * radius) continue;

        const lolview::Mat4 model = piece_matrix(piece.position, piece.center, piece.yaw,
                                                 piece.scale);
        const auto& vertices = piece.mesh.vertices();
        const auto& indices = piece.mesh.indices();
        if (vertices.empty() || indices.size() < 3) continue;

        std::vector<lolview::Vec3> world(vertices.size());
        for (std::size_t v = 0; v < vertices.size(); ++v) {
            world[v] = transform_point(
                model, lolview::Vec3{vertices[v].position[0], vertices[v].position[1],
                                     vertices[v].position[2]});
        }

        float best = 1e30f;
        for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
            float hit_t = 0.0f;
            if (ray_triangle(origin, direction, world[indices[t]], world[indices[t + 1]],
                             world[indices[t + 2]], hit_t)) {
                best = std::min(best, hit_t);
            }
        }
        if (best < 1e29f) hits.emplace_back(best, static_cast<int>(i));
    }
    std::sort(hits.begin(), hits.end());
    return hits;
}
lolview::Mat4 piece_matrix(const lolview::Vec3& position, const lolview::Vec3& center, float yaw,
                           float scale) {
    return lolview::translation(position) * lolview::translation(center) * rotation_y(yaw) *
           lolview::scaling(lolview::Vec3{scale, scale, scale}) *
           lolview::translation(lolview::Vec3{-center.x, -center.y, -center.z});
}

// Ray through a screen pixel for an explicit camera basis.
void ray_from_basis(const lolview::Vec3& eye, const lolview::Vec3& forward_in,
                    float screen_x, float screen_y, int width, int height,
                    lolview::Vec3& origin, lolview::Vec3& direction) {
    const float aspect = static_cast<float>(width) / static_cast<float>(std::max(1, height));
    const float tan_half = std::tan(kFovDegrees * 0.5f * kPi / 180.0f);

    const lolview::Vec3 forward = lolview::normalize(forward_in);
    const lolview::Vec3 world_up{0.0f, 1.0f, 0.0f};
    const lolview::Vec3 right =
        g_right_handed ? lolview::normalize(lolview::cross(world_up, forward))
                       : lolview::normalize(lolview::cross(forward, world_up));
    const lolview::Vec3 up = g_right_handed ? lolview::cross(forward, right)
                                            : lolview::cross(right, forward);

    const float ndc_x = (2.0f * screen_x / static_cast<float>(width)) - 1.0f;
    const float ndc_y = 1.0f - (2.0f * screen_y / static_cast<float>(height));
    origin = eye;
    direction = lolview::normalize(forward + right * (ndc_x * tan_half * aspect) +
                                   up * (ndc_y * tan_half));
}

// Camera ray through a screen pixel.
bool camera_ray(const LeagueCamera& camera, float screen_x, float screen_y, int width,
                int height, lolview::Vec3& origin, lolview::Vec3& direction) {
    const float aspect = static_cast<float>(width) / static_cast<float>(std::max(1, height));
    const float tan_half = std::tan(kFovDegrees * 0.5f * kPi / 180.0f);

    origin = camera.eye();
    const lolview::Vec3 forward = lolview::normalize(camera.focus - origin);
    const lolview::Vec3 world_up{0.0f, 1.0f, 0.0f};
    const lolview::Vec3 right =
        g_right_handed ? lolview::normalize(lolview::cross(world_up, forward))
                       : lolview::normalize(lolview::cross(forward, world_up));
    const lolview::Vec3 up = g_right_handed ? lolview::cross(forward, right)
                                            : lolview::cross(right, forward);

    const float ndc_x = (2.0f * screen_x / static_cast<float>(width)) - 1.0f;
    const float ndc_y = 1.0f - (2.0f * screen_y / static_cast<float>(height));
    direction = lolview::normalize(forward + right * (ndc_x * tan_half * aspect) +
                                   up * (ndc_y * tan_half));
    return true;
}

// Camera ray intersected with the ground plane y = 0.
bool pick_ground(const LeagueCamera& camera, float screen_x, float screen_y, int width,
                 int height, lolview::Vec3& out) {
    lolview::Vec3 origin, direction;
    camera_ray(camera, screen_x, screen_y, width, height, origin, direction);
    if (std::fabs(direction.y) < 1e-5f) return false;
    const float t = -origin.y / direction.y;
    if (t < 0.0f) return false;
    out = origin + direction * t;
    return true;
}

// World -> screen pixel. Returns false when behind the camera.
bool project_to_screen(const lolview::Mat4& view, const lolview::Mat4& proj,
                       const lolview::Vec3& world, int width, int height, float& out_x,
                       float& out_y) {
    const lolview::Mat4 vp = proj * view;
    const float x = world.x, y = world.y, z = world.z;
    const float clip_x = vp.m[0] * x + vp.m[4] * y + vp.m[8] * z + vp.m[12];
    const float clip_y = vp.m[1] * x + vp.m[5] * y + vp.m[9] * z + vp.m[13];
    const float clip_w = vp.m[3] * x + vp.m[7] * y + vp.m[11] * z + vp.m[15];
    if (clip_w <= 0.0001f) return false;
    out_x = (clip_x / clip_w * 0.5f + 0.5f) * static_cast<float>(width);
    out_y = (1.0f - (clip_y / clip_w * 0.5f + 0.5f)) * static_cast<float>(height);
    return true;
}

lolview::TexImage make_ground_texture() {
    constexpr int size = 256;
    lolview::TexImage image;
    image.width = size;
    image.height = size;
    image.format = "RGBA";
    image.rgba.resize(static_cast<std::size_t>(size) * size * 4);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float fx = static_cast<float>(x) / size;
            const float fy = static_cast<float>(y) / size;
            const int cell = 32;
            const bool line = (x % cell == 0) || (y % cell == 0);
            float r = line ? 0.15f : 0.10f;
            float g = line ? 0.20f : 0.15f;
            float b = line ? 0.16f : 0.11f;

            // Soft vignette toward the middle keeps the eye on the champion.
            const float dx = fx - 0.5f;
            const float dy = fy - 0.5f;
            const float radius = std::sqrt(dx * dx + dy * dy) * 2.0f;
            const float shade = std::clamp(1.35f - radius * 0.7f, 0.45f, 1.3f);
            r *= shade;
            g *= shade;
            b *= shade;

            const std::size_t index = (static_cast<std::size_t>(y) * size + x) * 4;
            image.rgba[index + 0] = static_cast<std::uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f);
            image.rgba[index + 1] = static_cast<std::uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f);
            image.rgba[index + 2] = static_cast<std::uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f);
            image.rgba[index + 3] = 255;
        }
    }
    return image;
}

// Map textures are 2048^2; the practice view never needs that, and keeping all
// of them at full size would cost gigabytes. Box-filter down to a cap.
lolview::TexImage downscale_texture(const lolview::TexImage& source, int max_size) {
    if (source.width <= max_size && source.height <= max_size) return source;
    const int factor = std::max(1, std::max(source.width, source.height) / max_size);
    lolview::TexImage result;
    result.width = std::max(1, source.width / factor);
    result.height = std::max(1, source.height / factor);
    result.format = "RGBA";
    result.rgba.resize(static_cast<std::size_t>(result.width) * result.height * 4);
    for (int y = 0; y < result.height; ++y) {
        for (int x = 0; x < result.width; ++x) {
            int r = 0, g = 0, b = 0, a = 0;
            for (int dy = 0; dy < factor; ++dy) {
                for (int dx = 0; dx < factor; ++dx) {
                    const int sx = std::min(source.width - 1, x * factor + dx);
                    const int sy = std::min(source.height - 1, y * factor + dy);
                    const std::size_t index =
                        (static_cast<std::size_t>(sy) * source.width + sx) * 4;
                    r += source.rgba[index + 0];
                    g += source.rgba[index + 1];
                    b += source.rgba[index + 2];
                    a += source.rgba[index + 3];
                }
            }
            const int count = factor * factor;
            const std::size_t out = (static_cast<std::size_t>(y) * result.width + x) * 4;
            result.rgba[out + 0] = static_cast<std::uint8_t>(r / count);
            result.rgba[out + 1] = static_cast<std::uint8_t>(g / count);
            result.rgba[out + 2] = static_cast<std::uint8_t>(b / count);
            result.rgba[out + 3] = static_cast<std::uint8_t>(a / count);
        }
    }
    return result;
}

void apply_theme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.11f, 0.92f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.22f, 0.25f, 0.36f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.31f, 0.35f, 0.50f, 1.0f);
}

// Ground-aligned ellipse (the ground plane is foreshortened on screen).
void draw_ground_ellipse(ImDrawList* draw, ImVec2 center, float radius, ImU32 color,
                         float thickness, bool filled) {
    constexpr int segments = 40;
    const float squash = 0.45f;
    ImVec2 points[segments];
    for (int i = 0; i < segments; ++i) {
        const float angle = (2.0f * kPi * i) / segments;
        points[i] = ImVec2(center.x + std::cos(angle) * radius,
                           center.y + std::sin(angle) * radius * squash);
    }
    if (filled) {
        draw->AddConvexPolyFilled(points, segments, color);
    } else {
        draw->AddPolyline(points, segments, color, ImDrawFlags_Closed, thickness);
    }
}

void draw_bar(ImDrawList* draw, ImVec2 top_left, ImVec2 size, float fraction, ImU32 fill,
              ImU32 background, const char* label) {    draw->AddRectFilled(top_left, ImVec2(top_left.x + size.x, top_left.y + size.y), background, 2.0f);
    if (fraction > 0.0f) {
        draw->AddRectFilled(top_left,
                            ImVec2(top_left.x + size.x * std::clamp(fraction, 0.0f, 1.0f),
                                   top_left.y + size.y),
                            fill, 2.0f);
    }
    if (label != nullptr) {
        const ImVec2 text_size = ImGui::CalcTextSize(label);
        draw->AddText(ImVec2(top_left.x + (size.x - text_size.x) * 0.5f,
                             top_left.y + (size.y - text_size.y) * 0.5f),
                      IM_COL32(235, 240, 245, 235), label);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto next = [&](std::string& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        if (arg == "--game-dir") {
            if (!next(args.game_dir)) return 2;
        } else if (arg == "--archive") {
            if (!next(args.archive)) return 2;
        } else if (arg == "--champion") {
            if (!next(args.champion)) return 2;
        } else if (arg == "--vfx-bin") {
            if (!next(args.vfx_bin)) return 2;
        } else if (arg == "--pet") {
            if (!next(args.pet)) return 2;
        } else if (arg == "--pet-archive") {
            if (!next(args.pet_archive)) return 2;
        } else if (arg == "--map") {
            args.load_map = true;
        } else if (arg == "--scene") {
            if (!next(args.scene_path)) return 2;
        } else if (arg == "--save-scene") {
            args.save_scene_on_start = true;
        } else if (arg == "--free") {
            args.start_free_camera = true;
        } else if (arg == "--mirror-x") {
            args.mirror_x = true;
        } else if (arg == "--raw-projection") {
            args.right_handed_view = false;
        } else if (arg == "--no-mirror") {
            args.mirror_x = false;
        } else if (arg == "--lh-view") {
            args.right_handed_view = false;
        } else if (arg == "--anim") {
            if (!next(args.force_anim)) return 2;
        } else if (arg == "--join") {
            if (!next(args.join)) return 2;
        } else if (arg == "--auto-attack") {
            std::string value;
            if (!next(value)) return 2;
            args.auto_attack = static_cast<float>(std::atof(value.c_str()));
        } else if (arg == "--auto-spell") {
            std::string value;
            if (!next(value)) return 2;
            args.auto_spell = static_cast<float>(std::atof(value.c_str()));
        } else if (arg == "--path-test") {
            args.path_test = true;
        } else if (arg == "--path-editor") {
            args.path_editor = true;
        } else if (arg == "--auto-move") {
            if (!next(args.auto_move)) return 2;
        } else if (arg == "--scenario") {
            if (!next(args.scenario_path)) return 2;
        } else if (arg == "--pick-test") {
            std::string value;
            if (!next(value)) return 2;
            args.pick_x = static_cast<float>(std::atof(value.c_str()));
            if (!next(value)) return 2;
            args.pick_y = static_cast<float>(std::atof(value.c_str()));
        } else if (arg == "--select") {
            std::string value;
            if (!next(value)) return 2;
            args.select_piece = std::atoi(value.c_str());
        } else if (arg == "--capture") {
            if (!next(args.capture)) return 2;
        } else if (arg == "--auto-cast") {
            if (!next(args.auto_cast)) return 2;
        } else if (arg == "--capture-after") {
            std::string value;
            if (!next(value)) return 2;
            args.capture_after = static_cast<float>(std::atof(value.c_str()));
        } else if (arg == "--exit-after") {
            std::string value;
            if (!next(value)) return 2;
            args.exit_after = static_cast<float>(std::atof(value.c_str()));
        } else if (arg == "--pitch") {
            std::string value;
            if (!next(value)) return 2;
            args.pitch_degrees = static_cast<float>(std::atof(value.c_str()));
        } else if (arg == "--distance") {
            std::string value;
            if (!next(value)) return 2;
            args.distance = static_cast<float>(std::atof(value.c_str()));
        } else if (arg == "--cam-yaw") {
            std::string value;
            if (!next(value)) return 2;
            args.camera_yaw_degrees = static_cast<float>(std::atof(value.c_str()));
        } else if (arg == "--width") {
            std::string value;
            if (!next(value)) return 2;
            args.width = std::atoi(value.c_str());
        } else if (arg == "--height") {
            std::string value;
            if (!next(value)) return 2;
            args.height = std::atoi(value.c_str());
        } else {
            std::fprintf(stderr, "lolview_sandbox: unknown argument %s\n", argv[i]);
            return 2;
        }
    }

    if (args.game_dir.empty()) {
        std::fprintf(stderr,
                     "usage: lolview_sandbox --game-dir <install> [--champion Ahri]\n"
                     "                       [--archive Ahri.wad] [--vfx-bin <hash>]\n");
        return 2;
    }

    try {
        g_right_handed = args.right_handed_view;
        // ---- assets from the user's install ----
        // Champions disagree on naming (Ahri_Base.skn vs jinx.skn, Idle1.anm vs
        // jinx_minigun_idle1.anm), so several candidates are tried.
        const auto resolve_first = [&](const std::vector<std::string>& paths)
            -> std::optional<std::vector<std::uint8_t>> {
            for (const auto& path : paths) {
                if (auto bytes = resolve_chunk(args.game_dir, args.archive, path)) return bytes;
            }
            return std::nullopt;
        };
        const std::string base_dir =
            "ASSETS/Characters/" + args.champion + "/Skins/Base/";
        const auto mesh_bytes = resolve_first(
            {base_dir + args.champion + "_Base.skn", base_dir + args.champion + ".skn"});
        const auto skeleton_bytes = resolve_first(
            {base_dir + args.champion + "_Base.skl", base_dir + args.champion + ".skl"});
        const auto texture_bytes =
            resolve_first({base_dir + args.champion + "_Base_TX_CM.tex",
                           base_dir + args.champion + "_TX_CM.tex",
                           base_dir + args.champion + ".tex"});
        if (!mesh_bytes || !skeleton_bytes) {
            std::fprintf(stderr, "error: could not resolve %s's mesh/skeleton in %s\n",
                         args.champion.c_str(), args.archive.c_str());
            return 1;
        }

        lolview::Skn mesh = lolview::Skn::parse(*mesh_bytes);
        if (args.mirror_x) mesh.mirror_x();
        const lolview::Skl skeleton = lolview::Skl::parse(*skeleton_bytes);
        lolview::TexImage texture;
        const bool has_texture = texture_bytes.has_value();
        if (has_texture) texture = lolview::decode_tex(*texture_bytes);

        std::map<std::string, std::vector<lolview::Anm>> clips;
        {
            std::string champion_lower = args.champion;
            std::transform(champion_lower.begin(), champion_lower.end(),
                           champion_lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::vector<lolview::CompressedAnimation>& compressed_clips = g_compressed_clips;
            const auto add_clip = [&](const std::string& key, const std::string& stem) {
                const auto bytes = resolve_first({base_dir + "Animations/" + stem + ".anm",
                                                  base_dir + "animations/" + stem + ".anm"});
                if (!bytes) return;
                try {
                    clips[key].push_back(lolview::Anm::parse(*bytes));
                } catch (const std::exception& error) {
                    if (lolview::is_compressed_animation(*bytes)) {
                        try {
                            const auto anim = lolview::parse_compressed_animation(*bytes);
                            std::printf(
                                "  compressed: %-28s %3u joints %5u frames %5.2fs\n",
                                stem.c_str(), anim.joint_count, anim.frame_count,
                                anim.duration);
                            compressed_clips.push_back(anim);
                            g_compressed_by_key[key].push_back(compressed_clips.size() - 1);
                        } catch (const std::exception& nested) {
                            std::fprintf(stderr, "warning: %s.anm compressed parse failed: %s\n",
                                         stem.c_str(), nested.what());
                        }
                        return;
                    }
                    std::fprintf(stderr, "warning: %s.anm failed to parse: %s\n", stem.c_str(),
                                 error.what());
                }
            };
            // Both weapon families (Jinx: minigun / rlauncher) and the plain
            // naming used by most champions.
            for (const char* weapon : {"minigun", "rlauncher", ""}) {
                const std::string w = weapon;
                const std::string prefix =
                    w.empty() ? champion_lower + "_" : champion_lower + "_" + w + "_";
                const std::string key_prefix = w.empty() ? std::string() : w + "_";
                for (int i = 1; i <= 3; ++i) {
                    add_clip(key_prefix + "idle" + std::to_string(i), prefix + "idle" + std::to_string(i));
                }
                add_clip(key_prefix + "run", prefix + "run");
                add_clip(key_prefix + "run2", prefix + "run2");
                for (int i = 1; i <= 6; ++i) {
                    add_clip(key_prefix + "attack" + std::to_string(i), prefix + "attack" + std::to_string(i));
                }
                for (int i = 1; i <= 4; ++i) {
                    add_clip(key_prefix + "spell" + std::to_string(i), prefix + "spell" + std::to_string(i));
                }
            }
            // Champion-agnostic fallbacks (Ahri style).
            for (const char* name : {"Idle1", "Run", "Spell1", "Spell2", "Spell3", "Spell4",
                                     "attack1"}) {
                add_clip(name, std::string(name));
            }
        }
        std::size_t total_clips = 0;
        for (const auto& [key, list] : clips) total_clips += list.size();
        if (total_clips == 0) {
            std::fprintf(stderr,
                         "warning: no animation clips for %s; rendering the bind pose\n",
                         args.champion.c_str());
        }

        std::printf("lolview practice tool - %s\n", args.champion.c_str());
        std::printf("  mesh    : %zu verts, %zu tris\n", mesh.vertices().size(),
                    mesh.triangle_count());
        std::printf("  skeleton: %zu joints, %zu influences\n", skeleton.joints().size(),
                    skeleton.influences().size());
        std::printf("  clips   : ");
        for (const auto& [name, list] : clips) {
            std::printf("%s(%zu) ", name.c_str(), list.size());
        }
        std::printf("\n");

        // ---- VFX systems ----
        std::vector<lolview::VfxSystem> vfx_systems;
        std::map<lolview::EffectKind, const lolview::VfxSystem*> vfx_by_kind;
        std::map<const lolview::VfxSystem*, std::vector<lolview::TexImage>> vfx_textures;

        if (!args.vfx_bin.empty()) {
            const std::uint64_t bin_hash = std::strtoull(args.vfx_bin.c_str(), nullptr, 16);
            const auto bytes = resolve_chunk_hash(args.game_dir, args.archive, bin_hash);
            if (!bytes) {
                std::fprintf(stderr, "warning: VFX bin %s not found\n", args.vfx_bin.c_str());
            } else {
                const auto document = lolview::BinDocument::parse(*bytes);
                vfx_systems = lolview::extract_vfx_systems(document);

                const std::pair<const char*, lolview::EffectKind> mapping[] = {
                    {"Ahri_Base_Q_mis", lolview::EffectKind::Spell1},
                    {"Ahri_Base_W_mis", lolview::EffectKind::Spell2},
                    {"Ahri_Base_E_mis", lolview::EffectKind::Spell3},
                    {"Ahri_Base_R_mis", lolview::EffectKind::Spell4},
                };
                for (const auto& [name, kind] : mapping) {
                    const auto* system = lolview::find_vfx_system(vfx_systems, name);
                    if (system == nullptr) continue;
                    vfx_by_kind[kind] = system;

                    std::vector<lolview::TexImage> textures(system->emitters.size());
                    for (std::size_t i = 0; i < system->emitters.size(); ++i) {
                        const auto& emitter = system->emitters[i];
                        if (emitter.texture.empty()) continue;
                        const auto texture_bytes =
                            resolve_chunk(args.game_dir, args.archive, emitter.texture);
                        if (!texture_bytes) continue;
                        try {
                            textures[i] = lolview::decode_tex(*texture_bytes);
                        } catch (const std::exception&) {
                        }
                    }
                    vfx_textures[system] = std::move(textures);
                }
                std::printf("  vfx     : %zu systems, %zu mapped\n", vfx_systems.size(),
                            vfx_by_kind.size());
            }
        }

        // ---- optional jungle pet and Summoner's Rift ground ----
        lolview::Skn pet_mesh;
        lolview::TexImage pet_texture;
        bool has_pet = false;
        bool has_pet_texture = false;
        const lolview::Vec3 pet_position{170.0f, 0.0f, -70.0f};

        if (!args.pet.empty()) {
            const std::string base =
                "ASSETS/Characters/" + args.pet + "/Skins/Base/" + args.pet;
            const auto mesh_bytes =
                resolve_chunk(args.game_dir, args.pet_archive, base + ".skn");
            const auto texture_bytes =
                resolve_chunk(args.game_dir, args.pet_archive, base + ".tex");
            if (!mesh_bytes) {
                std::fprintf(stderr, "warning: pet '%s' not found in %s\n", args.pet.c_str(),
                             args.pet_archive.c_str());
            } else {
                try {
                    pet_mesh = lolview::Skn::parse(*mesh_bytes);
                    if (args.mirror_x) pet_mesh.mirror_x();
                    has_pet = true;
                    if (texture_bytes) {
                        pet_texture = lolview::decode_tex(*texture_bytes);
                        has_pet_texture = true;
                    }
                    std::printf("  pet     : %s, %zu verts\n", args.pet.c_str(),
                                pet_mesh.vertices().size());
                } catch (const std::exception& error) {
                    std::fprintf(stderr, "warning: pet parse failed: %s\n", error.what());
                }
            }
        }

        // ---- Summoner's Rift terrain, straight out of the map archive ----
        lolview::Skn terrain_mesh;
        bool has_terrain = false;
        std::vector<TerrainGroup> terrain_groups;
        std::vector<MapPiece> map_pieces;
        std::map<std::string, lolview::TexImage> terrain_textures;
        std::map<std::string, std::string> terrain_texture_paths;
        lolview::Vec3 spawn{0.0f, 0.0f, 0.0f};

        if (args.load_map) {
            const auto bytes = resolve_chunk(args.game_dir, "Map11",
                                             "data/maps/mapgeometry/map11/base_srx.mapgeo");
            if (!bytes) {
                std::fprintf(stderr, "warning: map terrain not found in Map11.wad\n");
            } else {
                try {
                    const auto asset = lolview::load_mapgeo(*bytes);
                    double cx = 0.0, cy = 0.0, cz = 0.0;
                    std::size_t count = 0;
                    std::set<std::pair<int, int>> water_cells;
                    double spawn_sum_x = 0.0, spawn_sum_y = 0.0, spawn_sum_z = 0.0;
                    std::size_t spawn_vertices = 0;
                    const auto cell_of = [](float x, float z) {
                        return std::pair<int, int>{static_cast<int>(std::floor(x / 900.0f)),
                                                   static_cast<int>(std::floor(z / 900.0f))};
                    };
                    for (const auto& mesh : asset.meshes) {
                        const std::string material =
                            mesh.submeshes.empty() ? std::string() : mesh.submeshes.front().material;
                        const bool is_water = material.find("Water") != std::string::npos ||
                                              material.find("Ocean") != std::string::npos;
                        const bool is_spawn = material.find("Spawn") != std::string::npos;
                        for (const auto& vertex : mesh.vertices) {
                            cx += vertex.px;
                            cy += vertex.py;
                            cz += vertex.pz;
                            ++count;
                            if (is_water) water_cells.insert(cell_of(vertex.px, vertex.pz));
                            if (is_spawn) {
                                spawn_sum_x += vertex.px;
                                spawn_sum_y += vertex.py;
                                spawn_sum_z += vertex.pz;
                                ++spawn_vertices;
                            }
                        }
                    }
                    if (count > 0) {
                        cx /= static_cast<double>(count);
                        cy /= static_cast<double>(count);
                        cz /= static_cast<double>(count);
                    }
                    const lolview::Vec3 center{static_cast<float>(cx), static_cast<float>(cy),
                                               static_cast<float>(cz)};

                    // Prefer the fountain as the practice spot: it is flat, dry
                    // and recognisable, unlike the middle of the map (river).
                    lolview::Vec3 focus = center;
                    if (spawn_vertices > 0) {
                        focus = lolview::Vec3{
                            static_cast<float>(spawn_sum_x / static_cast<double>(spawn_vertices)),
                            static_cast<float>(spawn_sum_y / static_cast<double>(spawn_vertices)),
                            static_cast<float>(spawn_sum_z / static_cast<double>(spawn_vertices))};
                    }

                    // Pick the meshes closest to the centre until the u16 index
                    // range is reached, so the champion stands on real ground
                    // rather than the middle of the whole map.
                    std::vector<std::size_t> order(asset.meshes.size());
                    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
                    std::vector<float> distance_sq(order.size(), 0.0f);
                    for (std::size_t i = 0; i < order.size(); ++i) {
                        const auto& mesh = asset.meshes[i];
                        float sx = 0.0f, sy = 0.0f, sz = 0.0f;
                        for (const auto& vertex : mesh.vertices) {
                            sx += vertex.px;
                            sy += vertex.py;
                            sz += vertex.pz;
                        }
                        const float n = static_cast<float>(std::max<std::size_t>(1, mesh.vertices.size()));
                        const float dx = sx / n - focus.x;
                        const float dy = sy / n - focus.y;
                        const float dz = sz / n - focus.z;
                        distance_sq[i] = dx * dx + dy * dy + dz * dz;
                    }
                    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                        return distance_sq[a] < distance_sq[b];
                    });

                    std::vector<lolview::SknMaterial> materials;
                    std::vector<lolview::SknVertex> vertices;
                    std::vector<std::uint16_t> indices;
                    float closest = 1e30f;
                    float spawn_y = center.y;
                    float closest_ground = 1e30f;
                    float ground_y = center.y;
                    bool have_ground = false;

                    for (std::size_t index : order) {
                        const auto& mesh = asset.meshes[index];
                        if (mesh.vertices.empty()) continue;
                        if (vertices.size() + mesh.vertices.size() > 64000) continue;

                        const std::uint32_t base_vertex =
                            static_cast<std::uint32_t>(vertices.size());
                        const std::uint32_t base_index =
                            static_cast<std::uint32_t>(indices.size());

                        const bool is_ground =
                            !mesh.submeshes.empty() &&
                            mesh.submeshes.front().material.find("Ground") != std::string::npos;

                        for (const auto& source : mesh.vertices) {
                            lolview::SknVertex vertex{};
                            vertex.position = {source.px, source.py, source.pz};
                            vertex.normal = {source.nx, source.ny, source.nz};
                            vertex.uv = {source.u, source.v};
                            vertex.bone_indices = {0, 0, 0, 0};
                            vertex.weights = {1.0f, 0.0f, 0.0f, 0.0f};
                            vertices.push_back(vertex);

                            const float dx = source.px - focus.x;
                            const float dz = source.pz - focus.z;
                            const float horizontal = dx * dx + dz * dz;
                            if (horizontal < closest) {
                                closest = horizontal;
                                spawn_y = source.py;
                            }
                            if (is_ground && horizontal < closest_ground) {
                                const auto cell = cell_of(source.px, source.pz);
                                bool dry = true;
                                for (int ox = -1; ox <= 1 && dry; ++ox) {
                                    for (int oz = -1; oz <= 1 && dry; ++oz) {
                                        if (water_cells.count({cell.first + ox, cell.second + oz})) {
                                            dry = false;
                                        }
                                    }
                                }
                                if (dry) {
                                    closest_ground = horizontal;
                                    ground_y = source.py;
                                    have_ground = true;
                                }
                            }
                        }
                        for (std::uint16_t index : mesh.indices) {
                            indices.push_back(static_cast<std::uint16_t>(base_vertex + index));
                        }

                        lolview::SknMaterial material;
                        material.name = mesh.submeshes.empty() ? "Terrain"
                                                               : mesh.submeshes.front().material;
                        material.start_vertex = base_vertex;
                        material.num_vertices = static_cast<std::uint32_t>(mesh.vertices.size());
                        material.start_index = base_index;
                        material.num_indices = static_cast<std::uint32_t>(mesh.indices.size());
                        if (material.num_indices > 0) materials.push_back(std::move(material));

                        if (vertices.size() > 60000) break;
                    }

                    terrain_mesh = lolview::Skn::from_parts(std::move(materials),
                                                           std::move(vertices),
                                                           std::move(indices));
                    has_terrain = terrain_mesh.indices().size() > 0;
                    spawn = lolview::Vec3{focus.x * (args.mirror_x ? -1.0f : 1.0f),
                                          have_ground ? ground_y : spawn_y, focus.z};
                    std::printf("  terrain : %zu verts, %zu tris around (%.0f, %.0f, %.0f)\n",
                                terrain_mesh.vertices().size(),
                                terrain_mesh.indices().size() / 3, spawn.x, spawn.y,
                                spawn.z);

                    // ---- textured pass: one batch per material, each with its
                    // own baked diffuse map ----
                    std::size_t total = 0;
                    std::vector<TerrainGroup> groups;
                    for (std::size_t index : order) {
                        const auto& mesh = asset.meshes[index];
                        if (mesh.vertices.empty()) continue;
                        if (total + mesh.vertices.size() > 62000) break;
                        const std::string material = mesh.submeshes.empty()
                                                         ? std::string("Terrain")
                                                         : mesh.submeshes.front().material;
                        TerrainGroup* group = nullptr;
                        for (auto& candidate : groups) {
                            if (candidate.material == material &&
                                candidate.vertices.size() + mesh.vertices.size() <= 64000) {
                                group = &candidate;
                                break;
                            }
                        }
                        if (group == nullptr) {
                            groups.emplace_back();
                            group = &groups.back();
                            group->material = material;
                        }
                        const auto base = static_cast<std::uint16_t>(group->vertices.size());
                        for (const auto& source : mesh.vertices) {
                            lolview::SknVertex vertex{};
                            vertex.position = {source.px, source.py, source.pz};
                            vertex.normal = {source.nx, source.ny, source.nz};
                            vertex.uv = {source.u, source.v};
                            vertex.weights = {1.0f, 0.0f, 0.0f, 0.0f};
                            group->vertices.push_back(vertex);
                        }
                        for (std::uint16_t i : mesh.indices) group->indices.push_back(base + i);
                        total += mesh.vertices.size();
                        if (total > 60000) break;
                    }

                    std::size_t textured = 0;
                    const auto map_archive = find_archive(args.game_dir, "Map11.wad");
                    for (auto& group : groups) {
                        std::vector<lolview::SknMaterial> materials(1);
                        materials[0].name = group.material;
                        materials[0].num_vertices =
                            static_cast<std::uint32_t>(group.vertices.size());
                        materials[0].num_indices =
                            static_cast<std::uint32_t>(group.indices.size());
                        auto vertices = std::move(group.vertices);
                        auto indices = std::move(group.indices);
                        group.mesh = lolview::Skn::from_parts(std::move(materials),
                                                             std::move(vertices),
                                                             std::move(indices));
                        if (map_archive == nullptr) continue;
                        for (const auto& path : terrain_texture_candidates(group.material)) {
                            const auto found = map_archive->find(lolview::path_hash(path));
                            if (!found) continue;
                            try {
                                auto decoded = lolview::decode_tex(
                                    map_archive->read_chunk(map_archive->entries()[*found]));
                                group.has_texture = true;
                                group.texture_path = path;
                                terrain_textures[group.material] = std::move(decoded);
                                terrain_texture_paths[group.material] = path;
                                ++textured;
                            } catch (const std::exception&) {
                            }
                            break;
                        }

                        // Foliage and decals mask with alpha; water fades.
                        std::string material_lower = group.material;
                        std::transform(material_lower.begin(), material_lower.end(),
                                       material_lower.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        const bool cutout =
                            group.texture_path.find("1bitalpha") != std::string::npos ||
                            material_lower.find("decal") != std::string::npos ||
                            material_lower.find("leaf") != std::string::npos ||
                            material_lower.find("foliage") != std::string::npos ||
                            material_lower.find("grass") != std::string::npos;
                        const bool blended = material_lower.find("water") != std::string::npos ||
                                             material_lower.find("ocean") != std::string::npos ||
                                             material_lower.find("shadow") != std::string::npos ||
                                             material_lower.find("mask") != std::string::npos;
                        group.alpha_mode = blended ? 2 : (cutout ? 1 : 0);
                    }
                    terrain_groups = std::move(groups);
                    std::printf("  textures: %zu / %zu materials mapped\n", textured,
                                terrain_groups.size());
                    for (const auto& group : terrain_groups) {
                        if (!group.has_texture) {
                            std::printf("    untextured: %s\n", group.material.c_str());
                        }
                    }

                    // ---- every mesh also becomes an individually editable piece
                    map_pieces.reserve(asset.meshes.size());
                    for (const auto& source : asset.meshes) {
                        if (source.vertices.empty() || source.indices.empty()) continue;
                        if (source.vertices.size() > 65535) continue;

                        MapPiece piece;
                        piece.material = source.submeshes.empty()
                                             ? std::string("Terrain")
                                             : source.submeshes.front().material;

                        std::vector<lolview::SknMaterial> piece_materials(1);
                        piece_materials[0].name = piece.material;
                        piece_materials[0].num_vertices =
                            static_cast<std::uint32_t>(source.vertices.size());
                        piece_materials[0].num_indices =
                            static_cast<std::uint32_t>(source.indices.size());

                        std::vector<lolview::SknVertex> piece_vertices(source.vertices.size());
                        const float mirror = args.mirror_x ? -1.0f : 1.0f;
                        for (std::size_t v = 0; v < source.vertices.size(); ++v) {
                            const auto& in = source.vertices[v];
                            lolview::SknVertex out{};
                            out.position = {in.px * mirror, in.py, in.pz};
                            out.normal = {in.nx * mirror, in.ny, in.nz};
                            out.uv = {in.u, in.v};
                            out.weights = {1.0f, 0.0f, 0.0f, 0.0f};
                            piece_vertices[v] = out;
                        }
                        std::vector<std::uint16_t> piece_indices(source.indices.begin(),
                                                                 source.indices.end());
                        if (args.mirror_x) {
                            // Mirroring reverses triangle winding; swap to keep
                            // the outward faces outward.
                            for (std::size_t i = 0; i + 2 < piece_indices.size(); i += 3) {
                                std::swap(piece_indices[i + 1], piece_indices[i + 2]);
                            }
                        }
                        piece.mesh = lolview::Skn::from_parts(std::move(piece_materials),
                                                             std::move(piece_vertices),
                                                             std::move(piece_indices));
                        piece.position = {source.transform.m[12] * (args.mirror_x ? -1.0f : 1.0f),
                                          source.transform.m[13], source.transform.m[14]};

                        const auto bounds = piece.mesh.compute_bounds();
                        piece.center = {(bounds.min[0] + bounds.max[0]) * 0.5f,
                                        (bounds.min[1] + bounds.max[1]) * 0.5f,
                                        (bounds.min[2] + bounds.max[2]) * 0.5f};
                        const float dx = bounds.max[0] - bounds.min[0];
                        const float dy = bounds.max[1] - bounds.min[1];
                        const float dz = bounds.max[2] - bounds.min[2];
                        piece.radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
                        piece.bounds_min = {bounds.min[0], bounds.min[1], bounds.min[2]};
                        piece.bounds_max = {bounds.max[0], bounds.max[1], bounds.max[2]};

                        std::string material_lower = piece.material;
                        std::transform(material_lower.begin(), material_lower.end(),
                                       material_lower.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        // Cutout is decided by the texture that was resolved: the
                        // material name carries no alpha hint, but the texture
                        // file does (`..._1bitalpha.tex`).
                        const auto path_it = terrain_texture_paths.find(piece.material);
                        const std::string& texture_path =
                            path_it == terrain_texture_paths.end() ? std::string() : path_it->second;
                        const bool cutout =
                            path_it != terrain_texture_paths.end() &&
                            (texture_path.find("1bitalpha") != std::string::npos ||
                             material_lower.find("decal") != std::string::npos ||
                             material_lower.find("leaf") != std::string::npos ||
                             material_lower.find("foliage") != std::string::npos ||
                             material_lower.find("grass") != std::string::npos);
                        const bool blended = material_lower.find("water") != std::string::npos ||
                                             material_lower.find("ocean") != std::string::npos;
                        piece.alpha_mode = blended ? 2 : (cutout ? 1 : 0);

                        map_pieces.push_back(std::move(piece));
                    }
                    for (auto& piece : map_pieces) {
                        piece.initial_position = piece.position;
                        piece.initial_yaw = piece.yaw;
                    }
                    std::printf("  pieces  : %zu editable meshes\n", map_pieces.size());

                    // Resolve a texture for every distinct material, not just the
                    // ones inside the old fountain-sized patch.
                    if (const auto full_archive = find_archive(args.game_dir, "Map11.wad")) {
                        std::set<std::string> materials;
                        for (const auto& piece : map_pieces) materials.insert(piece.material);
                        std::size_t resolved = 0;
                        for (const auto& material : materials) {
                            if (terrain_textures.count(material)) continue;
                            for (const auto& path : terrain_texture_candidates(material)) {
                                const auto found = full_archive->find(lolview::path_hash(path));
                                if (!found) continue;
                                try {
                                    terrain_textures[material] = downscale_texture(
                                        lolview::decode_tex(full_archive->read_chunk(
                                            full_archive->entries()[*found])),
                                        1024);
                                    terrain_texture_paths[material] = path;
                                    ++resolved;
                                } catch (const std::exception&) {
                                }
                                break;
                            }
                        }
                        std::printf("  textures: %zu additional materials resolved\n", resolved);
                    }

                    // Alpha mode depends on the resolved textures, so decide it
                    // after every material has been looked up.
                    for (auto& piece : map_pieces) {
                        std::string material_lower = piece.material;
                        std::transform(material_lower.begin(), material_lower.end(),
                                       material_lower.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        const auto path_it = terrain_texture_paths.find(piece.material);
                        const bool has_texture = path_it != terrain_texture_paths.end();
                        const std::string& texture_path =
                            has_texture ? path_it->second : std::string();
                        const bool cutout =
                            has_texture &&
                            (texture_path.find("1bitalpha") != std::string::npos ||
                             material_lower.find("decal") != std::string::npos ||
                             material_lower.find("leaf") != std::string::npos ||
                             material_lower.find("foliage") != std::string::npos ||
                             material_lower.find("grass") != std::string::npos);
                        const bool blended = material_lower.find("water") != std::string::npos ||
                                             material_lower.find("ocean") != std::string::npos;
                        piece.alpha_mode = blended ? 2 : (cutout ? 1 : 0);
                    }
                } catch (const std::exception& error) {
                    std::fprintf(stderr, "warning: terrain parse failed: %s\n", error.what());
                }
            }
        }

        const bool headless = !args.capture.empty();
        lolview::Renderer renderer(args.width, args.height, !headless);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        apply_theme();
        ImGui_ImplSDL3_InitForOpenGL(renderer.window(), renderer.gl_context());
        ImGui_ImplOpenGL3_Init("#version 330 core");

        // ---- HUD icons (real game art) ----
        unsigned icon_textures[4] = {0, 0, 0, 0};
        unsigned icon_textures_alt[4] = {0, 0, 0, 0};  // per-weapon Q art
        for (int i = 0; i < 4; ++i) {
            const char* keys[4] = {"Q", "W", "E", "R"};
            for (int variant = 1; variant <= 2; ++variant) {
                for (const auto& path : hud_icon_candidates(args.champion, keys[i], variant)) {
                    const auto bytes = resolve_chunk(args.game_dir, args.archive, path);
                    if (!bytes) continue;
                    try {
                        const unsigned texture =
                            renderer.upload_texture(lolview::decode_dds(*bytes));
                        if (variant == 1) {
                            icon_textures[i] = texture;
                        } else {
                            icon_textures_alt[i] = texture;
                        }
                    } catch (const std::exception&) {
                    }
                    break;
                }
            }
        }

        // ---- scene state ----
        std::size_t cached_textures = 0;
        for (auto& piece : map_pieces) {
            piece.mesh_id = renderer.cache_mesh(piece.mesh);
            const auto found = terrain_textures.find(piece.material);
            if (found != terrain_textures.end()) {
                piece.texture_id = renderer.cache_texture(found->second);
                ++cached_textures;
            }
        }
        if (!map_pieces.empty()) {
            std::printf("  cached  : %zu pieces, %zu textures\n", map_pieces.size(),
                        cached_textures);
            std::set<std::string> missing;
            for (const auto& piece : map_pieces) {
                if (piece.texture_id < 0) missing.insert(piece.material);
            }
            std::printf("  untextured materials: %zu\n", missing.size());
            int shown_missing = 0;
            for (const auto& name : missing) {
                if (shown_missing++ >= 30) break;
                std::printf("    %s\n", name.c_str());
            }
        }

        // ---- scene plumbing: save/load piece alignment + rotation ----
        std::string scene_path =
            args.scene_path.empty() ? std::string("scenes/map.scene") : args.scene_path;
        std::string editor_status = "no scene loaded";
        int selected_piece = -1;
        char piece_filter[64] = "";

        const auto save_scene = [&]() {
            std::error_code code;
            std::filesystem::create_directories(
                std::filesystem::path(scene_path).parent_path(), code);
            std::ofstream out(scene_path, std::ios::trunc);
            if (!out) {
                editor_status = "cannot write " + scene_path;
                return;
            }
            out << "# lolview map scene v1\n";
            out << "# index px py pz yaw_degrees scale visible\n";
            for (std::size_t i = 0; i < map_pieces.size(); ++i) {
                const auto& piece = map_pieces[i];
                out << i << ' ' << piece.position.x << ' ' << piece.position.y << ' '
                    << piece.position.z << ' ' << piece.yaw << ' ' << piece.scale << ' '
                    << (piece.visible ? 1 : 0) << '\n';
            }
            editor_status = "saved " + std::to_string(map_pieces.size()) + " pieces to " +
                            scene_path;
        };

        const auto load_scene = [&]() {
            std::ifstream in(scene_path);
            if (!in) {
                editor_status = "no scene at " + scene_path;
                return;
            }
            std::string line;
            std::size_t applied = 0;
            while (std::getline(in, line)) {
                if (line.empty() || line[0] == '#') continue;
                std::istringstream stream(line);
                std::size_t index = 0;
                float x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f, scale = 1.0f;
                int visible = 1;
                if (!(stream >> index >> x >> y >> z >> yaw >> scale >> visible)) continue;
                if (index >= map_pieces.size()) continue;
                auto& piece = map_pieces[index];
                piece.position = {x, y, z};
                piece.yaw = yaw;
                piece.scale = scale;
                piece.visible = visible != 0;
                ++applied;
            }
            editor_status = "loaded " + std::to_string(applied) + " pieces from " + scene_path;
        };

        // ---- undo history for piece transforms (Ctrl+Z) ----
        struct Snapshot {
            std::vector<lolview::Vec3> positions;
            std::vector<float> yaws;
            std::vector<float> scales;
            std::vector<std::uint8_t> visible;
        };
        const auto capture_snapshot = [&]() {
            Snapshot snapshot;
            snapshot.positions.reserve(map_pieces.size());
            snapshot.yaws.reserve(map_pieces.size());
            snapshot.scales.reserve(map_pieces.size());
            snapshot.visible.reserve(map_pieces.size());
            for (const auto& piece : map_pieces) {
                snapshot.positions.push_back(piece.position);
                snapshot.yaws.push_back(piece.yaw);
                snapshot.scales.push_back(piece.scale);
                snapshot.visible.push_back(piece.visible ? 1 : 0);
            }
            return snapshot;
        };
        const auto apply_snapshot = [&](const Snapshot& snapshot) {
            const std::size_t count = std::min(map_pieces.size(), snapshot.positions.size());
            for (std::size_t i = 0; i < count; ++i) {
                auto& piece = map_pieces[i];
                piece.position = snapshot.positions[i];
                piece.yaw = snapshot.yaws[i];
                piece.scale = snapshot.scales[i];
                piece.visible = snapshot.visible[i] != 0;
            }
        };
        std::vector<Snapshot> history;
        std::size_t history_index = 0;
        const auto commit_history = [&]() {
            if (history_index + 1 < history.size()) {
                history.resize(history_index + 1);
            }
            history.push_back(capture_snapshot());
            history_index = history.size() - 1;
            if (history.size() > 200) {
                history.erase(history.begin());
                history_index = history.size() - 1;
            }
        };
        const auto undo_last = [&]() {
            if (history.empty() || history_index == 0) {
                editor_status = "nothing to undo";
                return;
            }
            --history_index;
            apply_snapshot(history[history_index]);
            editor_status = "undo " + std::to_string(history_index) + " / " +
                            std::to_string(history.size() - 1);
        };
        if (!map_pieces.empty()) history.push_back(capture_snapshot());

        load_scene();        if (args.save_scene_on_start) {
            for (auto& piece : map_pieces) {
                piece.initial_position = piece.position;
                piece.initial_yaw = piece.yaw;
            }
            save_scene();
        }
        LeagueCamera camera;
        FreeCamera free_camera;
        float free_speed = 1200.0f;  // wheel adjusts this in editor mode
        bool free_mode = false;
        bool editor_mode = false;  // F2: free-fly camera + editor window
        // F3: walkable-polygon editor. The polygon is saved so the game host can
        // reject movement outside it.
        bool path_edit = false;
        std::vector<lolview::Vec3> walk_path;
        std::vector<lolview::Vec3> blocked_path;
        bool walk_path_closed = false;
        bool blocked_path_closed = false;
        bool obstacle_mode = false;  // Tab: draw blocked (red) polygons
        std::string walk_path_file = "scripts/paths.txt";

        if (args.start_free_camera) {
            free_mode = true;
            editor_mode = true;
        }
        if (args.select_piece >= 0 &&
            args.select_piece < static_cast<int>(map_pieces.size())) {
            selected_piece = args.select_piece;
        }
        if (args.pitch_degrees > 0.0f) camera.pitch = args.pitch_degrees * kPi / 180.0f;
        if (args.distance > 0.0f) camera.distance = args.distance;
        camera.yaw = args.camera_yaw_degrees * kPi / 180.0f;
        lolview::Vec3 champion_position = spawn;
        float champion_yaw = 0.0f;
        bool has_move_target = false;
        lolview::Vec3 move_target{};

        // Editor mode starts from the play camera so it opens on what you were
        // looking at, not at the world origin.
        camera.focus = champion_position + lolview::Vec3{0.0f, 0.0f, 220.0f};
        if (args.start_free_camera) {
            free_camera.position = camera.eye();
            const lolview::Vec3 direction =
                lolview::normalize(camera.focus - free_camera.position);
            free_camera.yaw = std::atan2(direction.x, direction.z);
            free_camera.pitch = std::asin(std::clamp(direction.y, -1.0f, 1.0f));
        }

        // Headless picking probe (used to verify selection without a mouse).
        if (args.pick_x >= 0.0f && args.pick_y >= 0.0f) {
            lolview::Vec3 origin, direction;
            if (free_mode) {
                ray_from_basis(free_camera.position, free_camera.forward(), args.pick_x,
                               args.pick_y, args.width, args.height, origin, direction);
            } else {
                camera_ray(camera, args.pick_x, args.pick_y, args.width, args.height, origin,
                           direction);
            }
            const auto hits = pick_pieces(map_pieces, origin, direction);
            std::printf("pick (%.0f, %.0f): %zu hit(s)\n", args.pick_x, args.pick_y,
                        hits.size());
            for (std::size_t i = 0; i < hits.size() && i < 5; ++i) {
                std::printf("  [%zu] t=%.1f piece=%d %s\n", i, hits[i].first, hits[i].second,
                            map_pieces[static_cast<std::size_t>(hits[i].second)]
                                .material.c_str());
            }
            return 0;
        }

        lolview::Vec3 dummy_position = spawn + lolview::Vec3{0.0f, 0.0f, 700.0f};
        float dummy_hp = 1000.0f;
        const float dummy_max_hp = 1000.0f;
        float dummy_flash = 0.0f;

        const lolview::Skn dummy_mesh = lolview::Skn::make_cube(40.0f);
        const lolview::Skn ground = lolview::Skn::make_plane(3000.0f);
        const lolview::TexImage ground_texture = make_ground_texture();
        const unsigned ground_texture_id = renderer.upload_texture(ground_texture);

        // ---- unit model registry ----
        // Scenario files name a model; the registry maps that name to geometry
        // owned here, so any provider model can be spawned by name.
        std::map<std::string, const lolview::Skn*> unit_models;
        std::map<std::string, const lolview::TexImage*> unit_textures;
        std::vector<std::unique_ptr<lolview::Skn>> owned_models;
        std::vector<std::unique_ptr<lolview::TexImage>> owned_textures;

        unit_models[args.champion] = &mesh;
        if (has_texture) unit_textures[args.champion] = &texture;
        if (has_pet) {
            unit_models["pet"] = &pet_mesh;
            if (has_pet_texture) unit_textures["pet"] = &pet_texture;
        }
        unit_models["cube"] = &dummy_mesh;

        // ---- networked client (optional) ----
        lolview::net::UdpSocket net_socket;
        bool connected = false;
        std::uint16_t local_player = 0;
        std::uint16_t own_unit = 0;
        std::vector<std::string> net_models;
        struct RemoteUnit {
            std::uint16_t id = 0;
            std::string model;
            float x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f, hp = 0.0f, max_hp = 0.0f;
            int team = 0;
        };
        std::vector<RemoteUnit> remote_units;
        std::uint16_t net_target = 0xffff;
        float last_auto_attack = -100.0f;
        float last_auto_spell = -100.0f;
        int auto_spell_slot = 0;
        std::string server_host;
        std::uint16_t server_port = 27015;

        if (args.path_editor) {
            // Start with the walkable-path editor open (edit.bat uses this).
            path_edit = true;
            editor_mode = true;
            free_mode = true;
            free_camera.position = camera.eye();
            const lolview::Vec3 direction = lolview::normalize(camera.focus - camera.eye());
            free_camera.yaw = std::atan2(direction.x, direction.z);
            free_camera.pitch = std::asin(direction.y);
            std::printf("  path    : editor open (click to add vertices)\n");
        }

        if (args.path_test) {
            // Seed a triangle so the overlay can be inspected without clicking.
            walk_path = {spawn + lolview::Vec3{-320.0f, 0.0f, -260.0f},
                         spawn + lolview::Vec3{320.0f, 0.0f, -260.0f},
                         spawn + lolview::Vec3{0.0f, 0.0f, 320.0f}};
            walk_path_closed = true;
            path_edit = true;
        }

        if (!args.join.empty()) {
            server_host = args.join;
            const auto colon = server_host.find(':');
            if (colon != std::string::npos) {
                server_port = static_cast<std::uint16_t>(
                    std::atoi(server_host.substr(colon + 1).c_str()));
                server_host = server_host.substr(0, colon);
            }
            if (net_socket.bind(0)) {
                net_socket.set_non_blocking(true);
                lolview::net::Hello hello;
                hello.name = "player";
                const bool sent =
                    net_socket.send_to(server_host, server_port, lolview::net::encode(hello));
                std::printf("  net     : hello %s -> %s:%u (local port %u)\n",
                            sent ? "sent" : "FAILED", server_host.c_str(), server_port,
                            net_socket.local_port());
                for (int attempt = 0; attempt < 3000 && !connected; ++attempt) {
                    if (const auto datagram = net_socket.recv()) {
                        if (const auto welcome = lolview::net::decode_welcome(datagram->data)) {
                            connected = true;
                            local_player = welcome->player_id;
                            own_unit = welcome->unit_id;
                            net_models = welcome->models;
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                std::printf("  net     : %s (%s:%u, player %u)\n",
                            connected ? "joined" : "no server", server_host.c_str(),
                            server_port, local_player);
                if (connected && !args.auto_move.empty()) {
                    const auto comma = args.auto_move.find(',');
                    if (comma != std::string::npos) {
                        lolview::net::Intent intent;
                        intent.player_id = local_player;
                        intent.kind = lolview::net::IntentKind::Move;
                        intent.x = static_cast<float>(std::atof(args.auto_move.substr(0, comma).c_str()));
                        intent.z = static_cast<float>(std::atof(args.auto_move.substr(comma + 1).c_str()));
                        net_socket.send_to(server_host, server_port,
                                           lolview::net::encode(intent));
                        std::printf("  net     : auto-move to (%.0f, %.0f)\n", intent.x,
                                    intent.z);
                    }
                }
            }
        }

        const auto load_extra_model = [&](const std::string& name) -> const lolview::Skn* {
            std::string archive_filter;
            std::string mesh_path;
            std::string texture_path;
            if (name == "minion" || name == "minion_melee") {
                archive_filter = "Map12";
                mesh_path =
                    "assets/characters/ha_chaosminionmelee/skins/base/chaos_minion_melee.skn";
                texture_path =
                    "assets/characters/ha_chaosminionmelee/skins/base/chaosminion_melee_tx_cm.tex";
            } else {
                return nullptr;
            }
            const auto mesh_bytes = resolve_chunk(args.game_dir, archive_filter, mesh_path);
            if (!mesh_bytes) return nullptr;
            auto model = std::make_unique<lolview::Skn>(lolview::Skn::parse(*mesh_bytes));
            if (args.mirror_x) model->mirror_x();
            const lolview::Skn* pointer = model.get();
            owned_models.push_back(std::move(model));
            if (const auto texture_bytes =
                    resolve_chunk(args.game_dir, archive_filter, texture_path)) {
                try {
                    auto image =
                        std::make_unique<lolview::TexImage>(lolview::decode_tex(*texture_bytes));
                    unit_textures[name] = image.get();
                    owned_textures.push_back(std::move(image));
                } catch (const std::exception&) {
                }
            }
            unit_models[name] = pointer;
            std::printf("  model   : loaded '%s' (%zu verts)\n", name.c_str(),
                        pointer->vertices().size());
            return pointer;
        };

        // ---- scenario: entities spawned from a plain-text script ----
        std::vector<Unit> units;
        {
            std::ifstream file(args.scenario_path);
            std::string line;
            while (std::getline(file, line)) {
                if (line.empty() || line[0] == '#') continue;
                std::istringstream stream(line);
                std::string kind;
                std::string model;
                float hp = 100.0f;
                float dx = 0.0f, dz = 0.0f;
                int team = 0;
                if (!(stream >> kind >> model >> hp >> dx >> dz)) continue;
                if (kind != "unit") continue;
                if (!(stream >> team)) team = 0;

                Unit unit;
                unit.model = model;
                unit.hp = unit.max_hp = hp;
                unit.team = team;
                if (unit_models.find(model) == unit_models.end()) {
                    load_extra_model(model);
                }
                const auto found = unit_models.find(model);
                unit.mesh = found != unit_models.end() ? found->second : &dummy_mesh;
                unit.position = spawn + lolview::Vec3{dx, 0.0f, dz};
                units.push_back(unit);
            }
        }
        if (!units.empty()) {
            std::printf("  scenario: %zu units from %s\n", units.size(),
                        args.scenario_path.c_str());
        }

        std::vector<ActiveEffect> effects;
        float cooldowns[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const float cooldown_max[4] = {7.0f, 9.0f, 14.0f, 120.0f};

        std::string current_clip;
        float clip_time = 0.0f;
        float cast_lock = 0.0f;
        std::string cast_action;
        bool rocket_mode = false;
        bool was_moving = false;
        int walk_variant = 0;
        int idle_variant = 0;
        int attack_variant = 0;
        float attack_lock = 0.0f;
        // Attack commit: brief root plus facing, so attacks read as a stop-and-swing.
        float attack_hold = 0.0f;
        float attack_facing_x = 0.0f;
        float attack_facing_z = 1.0f;

        camera.focus = champion_position;
        bool running = true;
        float elapsed = 0.0f;
        bool captured = false;
        float fps = 0.0f;
        std::uint64_t last_counter = SDL_GetPerformanceCounter();
        const double frequency = static_cast<double>(SDL_GetPerformanceFrequency());

        const auto start_cast = [&](lolview::EffectKind kind, int slot,
                                    const lolview::Vec3& aim) {
            if (cooldowns[slot] > 0.0f) return;
            cooldowns[slot] = cooldown_max[slot];
            if (connected) {
                lolview::net::Intent intent;
                intent.player_id = local_player;
                intent.kind = lolview::net::IntentKind::Cast;
                intent.slot = static_cast<std::uint8_t>(slot);
                // Server space is world-relative; the client offsets by its spawn.
                intent.x = aim.x - spawn.x;
                intent.z = aim.z - spawn.z;
                net_socket.send_to(server_host, server_port,
                                   lolview::net::encode(intent));
            }

            lolview::Vec3 direction = aim - champion_position;
            direction.y = 0.0f;
            if (lolview::length(direction) < 1.0f) direction = {0.0f, 0.0f, 1.0f};
            direction = lolview::normalize(direction);
            champion_yaw = std::atan2(direction.x, direction.z);

            cast_action = "spell" + std::to_string(slot + 1);
            cast_lock = 0.05f;  // the clip's own duration replaces this on entry
            clip_time = 0.0f;

            ActiveEffect effect;
            effect.kind = kind;
            effect.origin = champion_position + lolview::Vec3{0.0f, 130.0f, 0.0f} +
                            direction * 40.0f;
            effect.direction = direction;
            effect.speed = kind == lolview::EffectKind::Spell4   ? 900.0f
                           : kind == lolview::EffectKind::Spell1 ? 700.0f
                                                                 : 500.0f;
            const auto mapped = vfx_by_kind.find(kind);
            if (mapped != vfx_by_kind.end()) {
                effect.vfx = mapped->second;
                const auto textures = vfx_textures.find(mapped->second);
                if (textures != vfx_textures.end()) effect.textures = &textures->second;
            }
            effects.push_back(effect);
        };

        if (!args.auto_cast.empty()) {
            const auto kind = lolview::effect_from_name(args.auto_cast);
            const int slot = kind == lolview::EffectKind::Spell1   ? 0
                             : kind == lolview::EffectKind::Spell2 ? 1
                             : kind == lolview::EffectKind::Spell3 ? 2
                             : kind == lolview::EffectKind::Spell4 ? 3
                                                                   : -1;
            if (slot >= 0) {
                start_cast(kind, slot, champion_position + lolview::Vec3{0.0f, 0.0f, 600.0f});
            }
        }

        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT) running = false;
                if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;

                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                    event.button.button == SDL_BUTTON_RIGHT && !free_mode) {
                    float mx = 0.0f, my = 0.0f;
                    SDL_GetMouseState(&mx, &my);
                    int w = 0, h = 0;
                    SDL_GetWindowSizeInPixels(renderer.window(), &w, &h);
                    lolview::Vec3 ground_point;
                    if (w > 0 && h > 0 &&
                        pick_ground(camera, mx, my, w, h, ground_point)) {
                        move_target = ground_point;
                        has_move_target = true;
                        if (connected) {
                            lolview::net::Intent intent;
                            intent.player_id = local_player;
                            intent.kind = lolview::net::IntentKind::Move;
                            intent.x = ground_point.x;
                            intent.z = ground_point.z;
                            net_socket.send_to(server_host, server_port,
                                               lolview::net::encode(intent));
                        }
                    }
                }

                if (event.type == SDL_EVENT_MOUSE_MOTION && free_mode) {
                    // Right-drag looks around in free-fly mode.
                    const SDL_MouseButtonFlags buttons = SDL_GetMouseState(nullptr, nullptr);
                    if ((buttons & SDL_BUTTON_RMASK) != 0) {
                        // In a right-handed basis a positive mouse delta turns the
                        // view right, i.e. yaw grows.
                        const float sign = g_right_handed ? 1.0f : -1.0f;
                        free_camera.yaw +=
                            sign * static_cast<float>(event.motion.xrel) * 0.0025f;
                        free_camera.pitch = std::clamp(
                            free_camera.pitch -
                                static_cast<float>(event.motion.yrel) * 0.0025f,
                            -1.5f, 1.5f);
                    }
                }

                if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                    if (free_mode) {
                        // In editor mode the wheel is the fly-speed control.
                        free_speed = std::clamp(free_speed * (1.0f + event.wheel.y * 0.2f),
                                                80.0f, 60000.0f);
                    } else {
                        camera.distance = std::clamp(
                            camera.distance * (1.0f - event.wheel.y * 0.1f), 900.0f, 2400.0f);
                    }
                }

                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                    float mx = 0.0f, my = 0.0f;
                    SDL_GetMouseState(&mx, &my);
                    int w = 0, h = 0;
                    SDL_GetWindowSizeInPixels(renderer.window(), &w, &h);
                    lolview::Vec3 aim = champion_position + lolview::Vec3{0.0f, 0.0f, 600.0f};
                    if (w > 0 && h > 0) pick_ground(camera, mx, my, w, h, aim);

                    switch (event.key.scancode) {
                        case SDL_SCANCODE_Q:
                            // Jinx-style weapons: Q swaps the animation set
                            // between minigun and rocket launcher.
                            if (clips.count("rlauncher_run") || clips.count("rlauncher_idle1")) {
                                rocket_mode = !rocket_mode;
                            } else {
                                start_cast(lolview::EffectKind::Spell1, 0, aim);
                            }
                            break;
                        case SDL_SCANCODE_W:
                            start_cast(lolview::EffectKind::Spell2, 1, aim);
                            break;
                        case SDL_SCANCODE_E:
                            start_cast(lolview::EffectKind::Spell3, 2, aim);
                            break;
                        case SDL_SCANCODE_R:
                            start_cast(lolview::EffectKind::Spell4, 3, aim);
                            break;
                        case SDL_SCANCODE_A:
                            // Basic attack only when something is actually in range.
                            if (net_target == 0xffff) break;
                            ++attack_variant;
                            attack_lock = 0.05f;
                            if (connected && net_target != 0xffff) {
                                lolview::net::Intent intent;
                                intent.player_id = local_player;
                                intent.kind = lolview::net::IntentKind::Attack;
                                intent.target = net_target;
                                net_socket.send_to(server_host, server_port,
                                                   lolview::net::encode(intent));                            // Commit: stop for a moment and face the target.
                            attack_hold = 0.25f;
                            has_move_target = false;
                            for (const auto& remote : remote_units) {
                                if (remote.id != net_target) continue;
                                attack_facing_x = remote.x - champion_position.x;
                                attack_facing_z = remote.z - champion_position.z;
                                break;
                            }                            }
                            break;
                        case SDL_SCANCODE_S:
                            // League's stop command (free camera uses S to fly back).
                            if (!free_mode) has_move_target = false;
                            break;
                        case SDL_SCANCODE_MINUS:
                        case SDL_SCANCODE_KP_MINUS:
                            if (!free_mode) camera.yaw -= kPi / 4.0f;
                            break;
                        case SDL_SCANCODE_EQUALS:
                        case SDL_SCANCODE_KP_PLUS:
                            if (!free_mode) camera.yaw += kPi / 4.0f;
                            break;
                        case SDL_SCANCODE_Z:
                            if ((SDL_GetModState() & SDL_KMOD_CTRL) != 0) undo_last();
                            break;
                        case SDL_SCANCODE_TAB:
                            if (path_edit) {
                                obstacle_mode = !obstacle_mode;
                                std::printf("  path    : drawing %s\n",
                                            obstacle_mode ? "obstacles (red)"
                                                          : "walkable (cyan)");
                            }
                            break;
                        case SDL_SCANCODE_BACKSPACE:
                            if (path_edit) {
                                auto& target = obstacle_mode ? blocked_path : walk_path;
                                if (!target.empty()) {
                                    target.pop_back();
                                    (obstacle_mode ? blocked_path_closed : walk_path_closed) =
                                        false;
                                    std::printf("  path    : %zu points\n", target.size());
                                }
                            }
                            break;
                        case SDL_SCANCODE_RETURN:
                            if (path_edit) {
                                if (obstacle_mode && blocked_path.size() >= 3) {
                                    blocked_path_closed = true;
                                    std::printf("  path    : obstacle closed with %zu points\n",
                                                blocked_path.size());
                                } else if (!obstacle_mode && walk_path.size() >= 3) {
                                    walk_path_closed = true;
                                    std::printf("  path    : walkable closed with %zu points\n",
                                                walk_path.size());
                                }
                            }
                            break;
                        case SDL_SCANCODE_F5: {
                            if (!walk_path.empty() || !blocked_path.empty()) {
                                std::ofstream out(walk_path_file, std::ios::trunc);
                                if (out) {
                                    // Saved in spawn-relative space: that is the
                                    // space intents and the host share.
                                    const auto write_block = [&](const char* kind,
                                                                 const std::vector<lolview::Vec3>& points) {
                                        if (points.size() < 3) return;
                                        out << kind << " 1\n";
                                        for (const auto& point : points) {
                                            out << "point " << (point.x - spawn.x) << " "
                                                << (point.z - spawn.z) << "\n";
                                        }
                                        out << "end\n";
                                    };
                                    write_block("path", walk_path);
                                    write_block("obstacle", blocked_path);
                                    std::printf(
                                        "  path    : saved %zu walkable, %zu obstacle points to %s\n",
                                        walk_path.size(), blocked_path.size(),
                                        walk_path_file.c_str());
                                } else {
                                    std::printf("  path    : cannot write %s\n",
                                                walk_path_file.c_str());
                                }
                            }
                            break;
                        }
                        case SDL_SCANCODE_F3: {
                            path_edit = !path_edit;
                            if (path_edit) {
                                editor_mode = true;
                                free_mode = true;
                                free_camera.position = camera.eye();
                                const lolview::Vec3 direction =
                                    lolview::normalize(camera.focus - camera.eye());
                                free_camera.yaw = std::atan2(direction.x, direction.z);
                                free_camera.pitch = std::asin(direction.y);
                            }
                            std::printf("  path    : %s (%zu points)\n",
                                        path_edit ? "editing" : "closed", walk_path.size());
                            break;
                        }
                        case SDL_SCANCODE_F2: {
                            // F2 opens the editor: free-fly camera plus the
                            // editor window. Pressing it again returns to play.
                            editor_mode = !editor_mode;
                            free_mode = editor_mode;
                            if (free_mode) {
                                free_camera.position = camera.eye();
                                const lolview::Vec3 direction =
                                    lolview::normalize(camera.focus - camera.eye());
                                free_camera.yaw = std::atan2(direction.x, direction.z);
                                free_camera.pitch =
                                    std::asin(std::clamp(direction.y, -1.0f, 1.0f));
                            }
                            break;
                        }
                        case SDL_SCANCODE_Y:
                            camera.locked = !camera.locked;
                            break;
                        case SDL_SCANCODE_SPACE:
                            camera.focus = champion_position;
                            camera.locked = true;
                            break;
                        case SDL_SCANCODE_ESCAPE:
                            running = false;
                            break;
                        default:
                            break;
                    }
                }
            }

            const std::uint64_t now = SDL_GetPerformanceCounter();
            float dt = static_cast<float>(static_cast<double>(now - last_counter) / frequency);
            last_counter = now;
            dt = std::clamp(dt, 0.0f, 0.1f);
            elapsed += dt;
            if (args.exit_after >= 0.0f && elapsed >= args.exit_after) running = false;
            fps = fps == 0.0f ? 1.0f / std::max(dt, 1e-4f)
                              : fps * 0.92f + (1.0f / std::max(dt, 1e-4f)) * 0.08f;

            for (float& cooldown : cooldowns) cooldown = std::max(0.0f, cooldown - dt);
            dummy_flash = std::max(0.0f, dummy_flash - dt);

            // ---- free-fly camera input ----
            if (free_mode) {
                const bool* keys = SDL_GetKeyboardState(nullptr);
                const float speed = free_speed * (keys[SDL_SCANCODE_LSHIFT] ? 3.0f : 1.0f);
                const lolview::Vec3 forward = free_camera.forward();
                const lolview::Vec3 world_up{0.0f, 1.0f, 0.0f};
                const lolview::Vec3 right =
                    g_right_handed ? lolview::normalize(lolview::cross(world_up, forward))
                                   : lolview::normalize(lolview::cross(forward, world_up));
                lolview::Vec3 delta{0.0f, 0.0f, 0.0f};
                if (keys[SDL_SCANCODE_W]) delta = delta + forward;
                if (keys[SDL_SCANCODE_S]) delta = delta - forward;
                if (keys[SDL_SCANCODE_D]) delta = delta + right;
                if (keys[SDL_SCANCODE_A]) delta = delta - right;
                if (keys[SDL_SCANCODE_SPACE]) delta.y += 1.0f;
                if (keys[SDL_SCANCODE_LCTRL]) delta.y -= 1.0f;
                if (lolview::length(delta) > 1e-4f) {
                    free_camera.position =
                        free_camera.position + lolview::normalize(delta) * (speed * dt);
                }
            }

            // ---- camera yaw is stepped from key events (45 degrees) ----
            // ---- movement ----
            bool moving = false;
            if (has_move_target && attack_hold <= 0.0f && !connected) {
                lolview::Vec3 delta = move_target - champion_position;
                delta.y = 0.0f;
                const float distance = lolview::length(delta);
                if (distance > 12.0f) {
                    const lolview::Vec3 direction = delta * (1.0f / distance);
                    const float step = std::min(distance, 380.0f * dt);
                    champion_position = champion_position + direction * step;
                    champion_yaw = std::atan2(direction.x, direction.z);
                    moving = true;
                } else {
                    has_move_target = false;
                }
            }

            // ---- animation state ----
            // Clip keys are "<weapon>_<action>" for champions with two weapons
            // (Jinx: minigun / rlauncher); others fall back to plain names.
            cast_lock = std::max(0.0f, cast_lock - dt);
            attack_lock = std::max(0.0f, attack_lock - dt);
            if (attack_hold > 0.0f) {
                attack_hold = std::max(0.0f, attack_hold - dt);
                const float facing_length =
                    std::sqrt(attack_facing_x * attack_facing_x +
                              attack_facing_z * attack_facing_z);
                if (facing_length > 1e-3f) {
                    champion_yaw = std::atan2(attack_facing_x, attack_facing_z);
                }
            }
            if (moving && !was_moving) ++walk_variant;
            was_moving = moving;

            const std::string weapon_key = rocket_mode ? "rlauncher_" : "minigun_";
            const auto find_clip = [&](const std::string& base) -> const lolview::Anm* {
                for (const std::string& key : {weapon_key + base, base}) {
                    const auto it = clips.find(key);
                    if (it != clips.end() && !it->second.empty()) return &it->second.front();
                }
                return nullptr;
            };
            const auto clip_for = [&](const std::string& action) -> const lolview::Anm* {
                if (const auto* exact = find_clip(action)) return exact;
                // Fall back within the family so a missing variant never drops
                // the character to bind pose (which reads as T-pose flicker).
                if (action.rfind("idle", 0) == 0) {
                    for (int i = 1; i <= 3; ++i) {
                        if (const auto* c = find_clip("idle" + std::to_string(i))) return c;
                    }
                } else if (action.rfind("run", 0) == 0) {
                    if (const auto* c = find_clip("run")) return c;
                    if (const auto* c = find_clip("run2")) return c;
                } else if (action.rfind("attack", 0) == 0) {
                    for (int i = 1; i <= 6; ++i) {
                        if (const auto* c = find_clip("attack" + std::to_string(i))) return c;
                    }
                } else if (action.rfind("spell", 0) == 0) {
                    for (int i = 1; i <= 4; ++i) {
                        if (const auto* c = find_clip("spell" + std::to_string(i))) return c;
                    }
                }
                return nullptr;
            };

            std::string desired;
            if (!args.force_anim.empty()) {
                desired = args.force_anim;  // --anim, for inspection
            } else if (cast_lock > 0.0f && !cast_action.empty()) {
                desired = cast_action;
            } else if (attack_lock > 0.0f) {
                desired = "attack" + std::to_string(1 + (attack_variant % 6));
            } else if (moving) {
                // Two walk cycles per weapon, alternated each time one starts.
                desired = (walk_variant % 2 == 0) ? "run" : "run2";
            } else {
                desired = "idle" + std::to_string(1 + (idle_variant % 3));
            }
            if (desired != current_clip) {
                current_clip = desired;
                clip_time = 0.0f;
                if (desired.rfind("idle", 0) == 0) ++idle_variant;

                // Spell and attack actions must stay locked for the clip length,
                // which lives in the compressed table for most champions.
                if (desired.rfind("spell", 0) == 0 || desired.rfind("attack", 0) == 0) {
                    float duration = 0.5f;
                    for (const std::string& key : {weapon_key + desired, desired}) {
                        const auto it = g_compressed_by_key.find(key);
                        if (it != g_compressed_by_key.end() && !it->second.empty()) {
                            duration = g_compressed_clips[it->second.front()].duration;
                            break;
                        }
                    }
                    if (const auto* c = clip_for(desired)) duration = c->duration();
                    if (desired.rfind("spell", 0) == 0) {
                        cast_lock = duration;
                    } else {
                        attack_lock = duration;
                    }
                }
            }
            clip_time += dt;

            const lolview::Anm* clip = nullptr;
            if (!clips.empty()) {
                clip = clip_for(current_clip);
                if (clip == nullptr) {
                    // Last resort: any clip at all beats bind pose.
                    for (const auto& [key, list] : clips) {
                        if (!list.empty()) {
                            clip = &list.front();
                            break;
                        }
                    }
                }
            }
            const float sampled =
                clip != nullptr ? std::fmod(clip_time, std::max(clip->duration(), 0.01f))
                                : 0.0f;

            // Compressed clips (r3d2canm) carry the walk / attack / spell tracks
            // the uncompressed set lacks, so they take precedence.
            const auto compressed_for = [&](const std::string& action)
                -> const lolview::CompressedAnimation* {
                const auto lookup = [&](const std::string& base)
                    -> const lolview::CompressedAnimation* {
                    for (const std::string& key : {weapon_key + base, base}) {
                        const auto it = g_compressed_by_key.find(key);
                        if (it != g_compressed_by_key.end() && !it->second.empty()) {
                            return &g_compressed_clips[it->second.front()];
                        }
                    }
                    return nullptr;
                };
                if (const auto* exact = lookup(action)) return exact;
                if (action.rfind("idle", 0) == 0) {
                    for (int i = 1; i <= 3; ++i) {
                        if (const auto* c = lookup("idle" + std::to_string(i))) return c;
                    }
                } else if (action.rfind("run", 0) == 0) {
                    if (const auto* c = lookup("run")) return c;
                    if (const auto* c = lookup("run2")) return c;
                } else if (action.rfind("attack", 0) == 0) {
                    for (int i = 1; i <= 6; ++i) {
                        if (const auto* c = lookup("attack" + std::to_string(i))) return c;
                    }
                } else if (action.rfind("spell", 0) == 0) {
                    for (int i = 1; i <= 4; ++i) {
                        if (const auto* c = lookup("spell" + std::to_string(i))) return c;
                    }
                }
                return nullptr;
            };

            std::vector<lolview::Mat4> bones;
            if (const auto* compressed = compressed_for(current_clip)) {
                const float compressed_time =
                    std::fmod(clip_time, std::max(compressed->duration, 0.01f));
                bones = lolview::compute_skin_matrices_from_keys(
                    skeleton, lolview::evaluate_compressed(*compressed, compressed_time));
            } else {
                bones = lolview::compute_skin_matrices(skeleton, clip, sampled);
            }
            if (args.mirror_x) {
                // Conjugate by the X mirror so the mirrored mesh keeps its pose.
                lolview::Mat4 flip = lolview::Mat4::identity();
                flip.m[0] = -1.0f;
                for (auto& bone : bones) bone = flip * bone * flip;
            }

            // ---- camera follow ----
            if (camera.locked) {
                camera.focus = champion_position + lolview::Vec3{0.0f, 0.0f, 220.0f};
            }

            // ---- effects ----
            std::vector<std::pair<const lolview::TexImage*, std::vector<lolview::Particle>>>
                batches;
            const lolview::Vec3 view_reference = camera.eye();
            for (auto& effect : effects) {
                effect.age += dt;
                const float distance_to_champion =
                    lolview::length(effect.origin - view_reference);
                const float pixels_per_unit =
                    static_cast<float>(args.height) /
                    (2.0f * std::tan(kFovDegrees * 0.5f * kPi / 180.0f) *
                     std::max(1.0f, distance_to_champion));

                std::vector<lolview::Particle> frame;
                if (effect.vfx != nullptr) {
                    frame = lolview::simulate_vfx(*effect.vfx, effect.age, effect.origin,
                                                  effect.direction, pixels_per_unit,
                                                  effect.speed);
                } else {
                    frame = lolview::simulate_effect(effect.kind, effect.age, effect.origin,
                                                     effect.direction);
                }

                std::map<int, std::vector<lolview::Particle>> by_slot;
                for (auto& particle : frame) by_slot[particle.slot].push_back(particle);
                for (auto& [slot, group] : by_slot) {
                    const lolview::TexImage* sprite = nullptr;
                    if (effect.textures != nullptr && slot >= 0 &&
                        static_cast<std::size_t>(slot) < effect.textures->size()) {
                        const auto& candidate = (*effect.textures)[static_cast<std::size_t>(slot)];
                        if (candidate.width > 0) sprite = &candidate;
                    }
                    batches.emplace_back(sprite, std::move(group));
                }

                if (!effect.hit_applied &&
                    effect.age * effect.speed >
                        lolview::length(dummy_position + lolview::Vec3{0.0f, 60.0f, 0.0f} -
                                        effect.origin) -
                            80.0f) {
                    effect.hit_applied = true;
                    dummy_hp = std::max(0.0f, dummy_hp - 90.0f);
                    dummy_flash = 0.25f;

                    // Damage the nearest enemy unit to the impact point.
                    const lolview::Vec3 impact =
                        dummy_position + lolview::Vec3{0.0f, 60.0f, 0.0f};
                    Unit* best = nullptr;
                    float best_distance = 260.0f;
                    for (auto& unit : units) {
                        if (!unit.alive || unit.team == 0) continue;
                        const float distance =
                            lolview::length(unit.position - impact);
                        if (distance < best_distance) {
                            best_distance = distance;
                            best = &unit;
                        }
                    }
                    if (best != nullptr) {
                        best->hp = std::max(0.0f, best->hp - 90.0f);
                        if (best->hp <= 0.0f) best->alive = false;
                    }
                }
            }
            effects.erase(std::remove_if(effects.begin(), effects.end(),
                                         [](const ActiveEffect& e) { return e.age > 3.0f; }),
                          effects.end());

            // ---- network: pull the latest authoritative snapshot ----
            if (connected) {
                while (const auto datagram = net_socket.recv()) {
                    if (const auto snapshot = lolview::net::decode_snapshot(datagram->data)) {
                        remote_units.clear();
                        for (const auto& state : snapshot->units) {
                            RemoteUnit unit;
                            unit.id = state.id;
                            unit.model = state.model < net_models.size()
                                             ? net_models[state.model]
                                             : std::string("cube");
                            // Server coordinates are world-relative; the client
                            // places them around its own spawn, like the scenario.
                            unit.x = spawn.x + state.x;
                            unit.y = spawn.y + state.y;
                            unit.z = spawn.z + state.z;
                            unit.yaw = state.yaw;
                            unit.hp = state.hp;
                            unit.max_hp = state.max_hp;
                            unit.team = state.team;
                            if (unit_models.find(unit.model) == unit_models.end()) {
                                load_extra_model(unit.model);
                            }
                            remote_units.push_back(unit);
                        }
                    }
                }
                // Nearest enemy to the champion becomes the attack target.
                net_target = 0xffff;
                float best_distance = 600.0f;  // server enforces the same range
                for (const auto& remote : remote_units) {
                    if (remote.team == 0 || remote.hp <= 0.0f) continue;
                    if (remote.id == own_unit) continue;
                    const lolview::Vec3 remote_position{remote.x, remote.y, remote.z};
                    const float distance = lolview::length(remote_position - champion_position);
                    if (distance < best_distance) {
                        best_distance = distance;
                        net_target = remote.id;
                    }
                }
                // The host owns movement: follow the position it reports for us.
                for (const auto& remote : remote_units) {
                    if (remote.id != own_unit) continue;
                    champion_position = lolview::Vec3{remote.x, spawn.y, remote.z};
                    break;
                }

                // Headless verification: attack the nearest enemy on a timer.
                if (args.auto_attack > 0.0f && net_target != 0xffff &&
                    elapsed - last_auto_attack >= args.auto_attack) {
                    last_auto_attack = elapsed;
                    lolview::net::Intent intent;
                    intent.player_id = local_player;
                    intent.kind = lolview::net::IntentKind::Attack;
                    intent.target = net_target;
                    net_socket.send_to(server_host, server_port,
                                       lolview::net::encode(intent));
                    attack_hold = 0.25f;
                    has_move_target = false;
                    for (const auto& remote : remote_units) {
                        if (remote.id != net_target) continue;
                        attack_facing_x = remote.x - champion_position.x;
                        attack_facing_z = remote.z - champion_position.z;
                        break;
                    }
                }

                // Headless verification: cycle spell slots at the wave.
                if (args.auto_spell > 0.0f && elapsed - last_auto_spell >= args.auto_spell) {
                    last_auto_spell = elapsed;
                    lolview::net::Intent intent;
                    intent.player_id = local_player;
                    intent.kind = lolview::net::IntentKind::Cast;
                    intent.slot = static_cast<std::uint8_t>(auto_spell_slot++ % 4);
                    // Aim where the wave ends up (server space).
                    intent.x = 0.0f;
                    intent.z = 0.0f;
                    net_socket.send_to(server_host, server_port,
                                       lolview::net::encode(intent));
                }
            }

            // ---- draw ----
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(renderer.window(), &w, &h);
            if (w <= 0 || h <= 0) {
                w = args.width;
                h = args.height;
            }

            renderer.begin_frame(w, h);
            const lolview::Mat4 view = free_mode ? free_camera.view() : camera.view();
            lolview::Mat4 proj = lolview::perspective(
                kFovDegrees * kPi / 180.0f, static_cast<float>(w) / static_cast<float>(h),
                2.0f, 40000.0f);

            if (!map_pieces.empty()) {
                for (int mode = 0; mode <= 2; ++mode) {
                    for (const auto& piece : map_pieces) {
                        if (!piece.visible || piece.alpha_mode != mode) continue;
                        // Rotate and scale around the piece's own centre so the
                        // editor behaves predictably (vertices are baked in
                        // world space, so the pivot must be explicit).
                        const lolview::Mat4 model = piece_matrix(
                            piece.position, piece.center, piece.yaw, piece.scale);
                        renderer.draw_cached(piece.mesh_id, piece.texture_id, model, view, proj,
                                             w, h, mode);
                    }
                }
            } else if (!terrain_groups.empty()) {
                for (int mode = 0; mode <= 2; ++mode) {
                    for (const auto& group : terrain_groups) {
                        if (group.alpha_mode != mode) continue;
                        renderer.set_alpha_mode(mode);
                        renderer.draw_scene(group.mesh,
                                            group.has_texture ? &group.texture : nullptr, nullptr,
                                            nullptr, lolview::Mat4::identity(), view, proj, w, h);
                    }
                }
                renderer.set_alpha_mode(0);
            } else if (has_terrain) {
                renderer.draw_scene(terrain_mesh, nullptr, nullptr, nullptr,
                                    lolview::Mat4::identity(), view, proj, w, h);
            } else {
                renderer.draw_scene(ground, &ground_texture, nullptr, nullptr,
                                    lolview::Mat4::identity(), view, proj, w, h);
            }
            renderer.draw_scene(mesh, has_texture ? &texture : nullptr, &bones, nullptr,
                                model_matrix(champion_position, champion_yaw), view, proj, w, h);

            // Selection highlight: the picked piece as red wireframe.
            if (editor_mode && selected_piece >= 0 &&
                selected_piece < static_cast<int>(map_pieces.size())) {
                const auto& piece = map_pieces[selected_piece];
                const lolview::Mat4 model =
                    piece_matrix(piece.position, piece.center, piece.yaw, piece.scale);
                renderer.draw_wireframe(piece.mesh_id, model, view, proj, w, h,
                                        lolview::Vec3{1.0f, 0.2f, 0.2f});
            }
            if (has_pet) {
                renderer.draw_scene(pet_mesh, has_pet_texture ? &pet_texture : nullptr, nullptr,
                                    nullptr, lolview::translation(pet_position), view, proj, w, h);
            }
            for (const auto& unit : units) {
                // While connected the host owns the world; local scenario units
                // would duplicate the replicated ones and confuse targeting.
                if (connected) break;
                if (!unit.alive || unit.mesh == nullptr) continue;
                const auto texture_it = unit_textures.find(unit.model);
                const lolview::TexImage* unit_texture =
                    texture_it != unit_textures.end() ? texture_it->second : nullptr;
                renderer.draw_scene(*unit.mesh, unit_texture, nullptr, nullptr,
                                    model_matrix(unit.position, unit.yaw), view, proj, w, h);
            }
            renderer.draw_scene(dummy_mesh, nullptr, nullptr, nullptr,
                                lolview::translation(dummy_position + lolview::Vec3{0, 40, 0}),
                                view, proj, w, h);
            for (const auto& remote : remote_units) {
                if (remote.id == own_unit) continue;  // drawn as the local champion
                const lolview::Skn* remote_mesh = &dummy_mesh;
                const lolview::TexImage* remote_texture = nullptr;
                const std::vector<lolview::Mat4>* remote_bones = nullptr;
                if (remote.model == "champion") {
                    // Other players are champions too; without per-player model
                    // streaming they share this client's champion art (animated).
                    remote_mesh = &mesh;
                    remote_texture = has_texture ? &texture : nullptr;
                    remote_bones = &bones;
                } else {
                    const auto mesh_it = unit_models.find(remote.model);
                    if (mesh_it != unit_models.end()) remote_mesh = mesh_it->second;
                    const auto texture_it = unit_textures.find(remote.model);
                    if (texture_it != unit_textures.end()) remote_texture = texture_it->second;
                }
                renderer.draw_scene(
                    *remote_mesh, remote_texture, remote_bones, nullptr,
                    model_matrix(lolview::Vec3{remote.x, remote.y, remote.z}, remote.yaw), view,
                    proj, w, h);
            }

            for (const auto& [sprite, batch] : batches) {
                if (!batch.empty()) renderer.draw_particles(batch, sprite, view, proj);
            }

            // ---- HUD ----
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();

            // Walkable-path editing: left click places a vertex on the ground.
            if (path_edit && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                !ImGui::GetIO().WantCaptureMouse &&
                !(obstacle_mode ? blocked_path_closed : walk_path_closed)) {
                float mouse_x = 0.0f, mouse_y = 0.0f;
                SDL_GetMouseState(&mouse_x, &mouse_y);
                lolview::Vec3 origin, direction;
                if (free_mode) {
                    ray_from_basis(free_camera.position, free_camera.forward(), mouse_x, mouse_y,
                                   w, h, origin, direction);
                } else {
                    camera_ray(camera, mouse_x, mouse_y, w, h, origin, direction);
                }
                if (std::fabs(direction.y) > 1e-5f) {
                    const float t = -origin.y / direction.y;
                    if (t >= 0.0f) {
                        auto& target = obstacle_mode ? blocked_path : walk_path;
                        target.push_back(origin + direction * t);
                        std::printf("  path    : %s vertex %zu at (%.0f, %.0f)\n",
                                    obstacle_mode ? "obstacle" : "walkable", target.size(),
                                    target.back().x, target.back().z);
                    }
                }
            }

            // Click in the world selects a piece, but only while editing.
            if (editor_mode && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                !ImGui::GetIO().WantCaptureMouse && !map_pieces.empty()) {
                float mouse_x = 0.0f, mouse_y = 0.0f;
                SDL_GetMouseState(&mouse_x, &mouse_y);
                lolview::Vec3 ray_origin, ray_direction;
                if (free_mode) {
                    ray_from_basis(free_camera.position, free_camera.forward(), mouse_x,
                                   mouse_y, w, h, ray_origin, ray_direction);
                } else {
                    camera_ray(camera, mouse_x, mouse_y, w, h, ray_origin, ray_direction);
                }

                const auto hits = pick_pieces(map_pieces, ray_origin, ray_direction);
                if (!hits.empty()) {
                    // Clicking the already-selected piece steps to the next one
                    // under the cursor, so overlapping pieces can be cycled.
                    int next = hits.front().second;
                    for (std::size_t h = 0; h < hits.size(); ++h) {
                        if (hits[h].second == selected_piece) {
                            next = hits[(h + 1) % hits.size()].second;
                            break;
                        }
                    }
                    selected_piece = next;
                    editor_status = "selected " + std::to_string(next) + " (" +
                                    std::to_string(hits.size()) + " under cursor)";
                } else {
                    editor_status = "nothing under cursor";
                }
            }

            ImDrawList* draw = ImGui::GetBackgroundDrawList();
            // Obstacles are drawn in red on top of the walkable outline.
            if (path_edit && !blocked_path.empty()) {
                for (std::size_t i = 0; i + 1 < blocked_path.size(); ++i) {
                    float ax = 0, ay = 0, bx = 0, by = 0;
                    if (!project_to_screen(view, proj, blocked_path[i], w, h, ax, ay)) continue;
                    if (!project_to_screen(view, proj, blocked_path[i + 1], w, h, bx, by)) continue;
                    draw->AddLine(ImVec2(ax, ay), ImVec2(bx, by), IM_COL32(240, 80, 80, 255), 3.0f);
                }
                if (blocked_path_closed && blocked_path.size() >= 3) {
                    float ax = 0, ay = 0, bx = 0, by = 0;
                    if (project_to_screen(view, proj, blocked_path.back(), w, h, ax, ay) &&
                        project_to_screen(view, proj, blocked_path.front(), w, h, bx, by)) {
                        draw->AddLine(ImVec2(ax, ay), ImVec2(bx, by), IM_COL32(240, 80, 80, 255),
                                      3.0f);
                    }
                }
                for (const auto& point : blocked_path) {
                    float sx = 0, sy = 0;
                    if (!project_to_screen(view, proj, point, w, h, sx, sy)) continue;
                    draw->AddCircleFilled(ImVec2(sx, sy), 5.0f, IM_COL32(255, 170, 170, 240));
                }
            }
            // Walkable polygon overlay (F3 editor).
            if (path_edit && !walk_path.empty()) {
                for (std::size_t i = 0; i + 1 < walk_path.size(); ++i) {
                    float ax = 0, ay = 0, bx = 0, by = 0;
                    if (!project_to_screen(view, proj, walk_path[i], w, h, ax, ay)) continue;
                    if (!project_to_screen(view, proj, walk_path[i + 1], w, h, bx, by)) continue;
                    draw->AddLine(ImVec2(ax, ay), ImVec2(bx, by), IM_COL32(90, 220, 255, 255),
                                  3.0f);
                }
                if (walk_path_closed && walk_path.size() >= 3) {
                    float ax = 0, ay = 0, bx = 0, by = 0;
                    if (project_to_screen(view, proj, walk_path.back(), w, h, ax, ay) &&
                        project_to_screen(view, proj, walk_path.front(), w, h, bx, by)) {
                        draw->AddLine(ImVec2(ax, ay), ImVec2(bx, by), IM_COL32(90, 220, 255, 255),
                                      3.0f);
                    }
                }
                for (const auto& point : walk_path) {
                    float sx = 0, sy = 0;
                    if (!project_to_screen(view, proj, point, w, h, sx, sy)) continue;
                    draw->AddCircleFilled(ImVec2(sx, sy), 5.0f, IM_COL32(255, 255, 255, 235));
                }
            }


            // champion health / mana
            const float bar_width = 320.0f;
            const ImVec2 health_pos((w - bar_width) * 0.5f, static_cast<float>(h) - 118.0f);
            draw_bar(draw, health_pos, ImVec2(bar_width, 18.0f), 1.0f, IM_COL32(56, 176, 92, 255),
                     IM_COL32(12, 20, 14, 220), "1240 / 1240");
            draw_bar(draw, ImVec2(health_pos.x, health_pos.y + 20.0f), ImVec2(bar_width, 12.0f),
                     1.0f, IM_COL32(56, 108, 196, 255), IM_COL32(12, 16, 26, 220), nullptr);

            // ability bar
            constexpr float icon = 56.0f;
            constexpr float gap = 6.0f;
            const float total = 4 * icon + 3 * gap;
            const float start_x = (w - total) * 0.5f;
            const float start_y = static_cast<float>(h) - 74.0f;
            const char* keys[4] = {"Q", "W", "E", "R"};

            for (int i = 0; i < 4; ++i) {
                const ImVec2 top_left(start_x + i * (icon + gap), start_y);
                const ImVec2 bottom_right(top_left.x + icon, top_left.y + icon);

                // Q shows a different icon per weapon when the champion has one
                // (Jinx: jinx_q1 minigun / jinx_q2 rocket launcher).
                const unsigned icon = (i == 0 && rocket_mode && icon_textures_alt[0] != 0)
                                          ? icon_textures_alt[0]
                                          : icon_textures[i];
                if (icon != 0) {
                    draw->AddImage(static_cast<ImTextureID>(icon), top_left, bottom_right);
                } else {
                    draw->AddRectFilled(top_left, bottom_right, IM_COL32(40, 48, 66, 255), 4.0f);
                }
                draw->AddRect(top_left, bottom_right, IM_COL32(150, 165, 190, 220), 4.0f, 0, 2.0f);

                if (cooldowns[i] > 0.0f) {
                    const float fraction = cooldowns[i] / cooldown_max[i];
                    draw->AddRectFilled(top_left, bottom_right, IM_COL32(0, 0, 0, 150), 4.0f);
                    char text[16];
                    std::snprintf(text, sizeof(text), "%.1f", cooldowns[i]);
                    const ImVec2 size = ImGui::CalcTextSize(text);
                    draw->AddText(ImVec2(top_left.x + (icon - size.x) * 0.5f,
                                         top_left.y + (icon - size.y) * 0.5f),
                                  IM_COL32(255, 255, 255, 240), text);
                    (void)fraction;
                }

                draw->AddText(ImVec2(top_left.x + 4.0f, top_left.y + 2.0f),
                              IM_COL32(240, 235, 200, 230), keys[i]);
            }

            // dummy health bar in world space
            float dummy_screen_x = 0.0f, dummy_screen_y = 0.0f;
            if (project_to_screen(view, proj,
                                  dummy_position + lolview::Vec3{0.0f, 150.0f, 0.0f}, w, h,
                                  dummy_screen_x, dummy_screen_y)) {
                const ImVec2 top_left(dummy_screen_x - 55.0f, dummy_screen_y - 6.0f);
                const ImU32 fill = dummy_flash > 0.0f ? IM_COL32(240, 240, 240, 255)
                                                      : IM_COL32(196, 66, 72, 255);
                draw_bar(draw, top_left, ImVec2(110.0f, 10.0f), dummy_hp / dummy_max_hp, fill,
                         IM_COL32(12, 12, 14, 210), nullptr);
            }

            // health bars for scenario units
            for (const auto& unit : units) {
                if (!unit.alive || unit.max_hp <= 0.0f) continue;
                float unit_x = 0.0f, unit_y = 0.0f;
                if (!project_to_screen(view, proj,
                                       unit.position + lolview::Vec3{0.0f, 150.0f, 0.0f}, w, h,
                                       unit_x, unit_y)) {
                    continue;
                }
                const ImVec2 top_left(unit_x - 45.0f, unit_y - 5.0f);
                const ImU32 fill = unit.team == 0 ? IM_COL32(96, 200, 120, 255)
                                                  : IM_COL32(200, 78, 84, 255);
                draw_bar(draw, top_left, ImVec2(90.0f, 9.0f), unit.hp / unit.max_hp, fill,
                         IM_COL32(12, 12, 14, 210), nullptr);
            }

            // health bars for networked units
            for (const auto& remote : remote_units) {
                if (remote.id == own_unit) continue;
                if (remote.max_hp <= 0.0f) continue;
                float remote_x = 0.0f, remote_y = 0.0f;
                if (!project_to_screen(view, proj,
                                       lolview::Vec3{remote.x, remote.y + 150.0f, remote.z}, w,
                                       h, remote_x, remote_y)) {
                    continue;
                }
                const ImU32 fill = remote.team == 0 ? IM_COL32(96, 200, 120, 255)
                                                    : IM_COL32(200, 78, 84, 255);
                draw_bar(draw, ImVec2(remote_x - 45.0f, remote_y - 5.0f), ImVec2(90.0f, 9.0f),
                         remote.hp / remote.max_hp, fill, IM_COL32(12, 12, 14, 210), nullptr);
            }

            // champion selection ring / shadow on the ground
            float champion_x = 0.0f, champion_y = 0.0f;
            if (project_to_screen(view, proj, champion_position, w, h, champion_x,
                                  champion_y)) {
                draw_ground_ellipse(draw, ImVec2(champion_x, champion_y), 46.0f,
                                    IM_COL32(0, 0, 0, 80), 1.0f, true);
                draw_ground_ellipse(draw, ImVec2(champion_x, champion_y), 50.0f,
                                    IM_COL32(96, 220, 140, 200), 2.5f, false);
            }

            // dummy shadow
            float dummy_base_x = 0.0f, dummy_base_y = 0.0f;
            if (project_to_screen(view, proj, dummy_position, w, h, dummy_base_x,
                                  dummy_base_y)) {
                draw_ground_ellipse(draw, ImVec2(dummy_base_x, dummy_base_y), 40.0f,
                                    IM_COL32(0, 0, 0, 90), 1.0f, true);
            }

            // (selection highlight is drawn as a red wireframe in the 3D pass)

            // move-target marker
            if (has_move_target && attack_hold <= 0.0f && !connected) {
                float marker_x = 0.0f, marker_y = 0.0f;
                if (project_to_screen(view, proj, move_target, w, h, marker_x, marker_y)) {
                    draw->AddCircle(ImVec2(marker_x, marker_y), 12.0f,
                                    IM_COL32(120, 220, 140, 200), 24, 2.0f);
                }
            }

            // top-left status
            draw->AddText(ImVec2(14.0f, 12.0f), IM_COL32(220, 225, 235, 220),
                          "lolview practice tool");
            char status[160];
            std::snprintf(status, sizeof(status),
                          "%.0f fps   |   RMB move   S stop   QWER cast   -/+ rotate (%.0f deg)   "
                          "Y %s   F2 %s",
                          fps, camera.yaw * 180.0f / kPi, camera.locked ? "locked" : "free",
                          editor_mode ? "editor ON (WASD fly)" : "editor");            draw->AddText(ImVec2(14.0f, 30.0f), IM_COL32(150, 160, 175, 210), status);
            draw->AddText(ImVec2(14.0f, static_cast<float>(h) - 22.0f),
                          IM_COL32(120, 130, 145, 190),
                          "not affiliated with Riot Games - assets read from your own install");

            // ---- editor overlays: world/camera basis vectors ----
            if (editor_mode) {
                // Rows of the view matrix are the camera basis the image uses.
                const lolview::Vec3 cam_right{view.m[0], view.m[4], view.m[8]};
                const lolview::Vec3 cam_up{view.m[1], view.m[5], view.m[9]};
                const lolview::Vec3 cam_fwd{-view.m[2], -view.m[6], -view.m[10]};
                const lolview::Vec3 eye = free_mode ? free_camera.position : camera.eye();

                char lines[4][96];
                std::snprintf(lines[0], sizeof(lines[0]), "eye    %8.1f %8.1f %8.1f", eye.x,
                              eye.y, eye.z);
                std::snprintf(lines[1], sizeof(lines[1]), "right  %8.2f %8.2f %8.2f",
                              cam_right.x, cam_right.y, cam_right.z);
                std::snprintf(lines[2], sizeof(lines[2]), "up     %8.2f %8.2f %8.2f", cam_up.x,
                              cam_up.y, cam_up.z);
                std::snprintf(lines[3], sizeof(lines[3]), "fwd    %8.2f %8.2f %8.2f", cam_fwd.x,
                              cam_fwd.y, cam_fwd.z);

                float widest = 0.0f;
                for (const auto& line : lines) {
                    widest = std::max(widest, ImGui::CalcTextSize(line).x);
                }
                const float line_height = ImGui::GetTextLineHeightWithSpacing();
                const float box_width = widest + 20.0f;
                const ImVec2 top_left(static_cast<float>(w) - box_width - 16.0f, 8.0f);
                draw->AddRectFilled(
                    ImVec2(top_left.x - 6.0f, top_left.y - 4.0f),
                    ImVec2(top_left.x + box_width, top_left.y + line_height * 4.0f + 4.0f),
                    IM_COL32(12, 16, 22, 200), 4.0f);
                for (int i = 0; i < 4; ++i) {
                    draw->AddText(ImVec2(top_left.x, top_left.y + line_height * i),
                                  IM_COL32(190, 220, 200, 235), lines[i]);
                }
            }

            // ---- map editor (F2) ----
            if (editor_mode && !map_pieces.empty()) {
                ImGui::SetNextWindowPos(ImVec2(static_cast<float>(w) - 400.0f, 108.0f),
                                        ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(380.0f, 540.0f), ImGuiCond_FirstUseEver);
                ImGui::Begin("Map editor");
                ImGui::Text("%zu pieces   |   %s", map_pieces.size(), editor_status.c_str());
                ImGui::Text("fly speed %.0f  (wheel up/down)   |   click selects", free_speed);
                if (ImGui::Button("Save scene")) save_scene();
                ImGui::SameLine();
                if (ImGui::Button("Reload")) load_scene();
                ImGui::SameLine();
                if (ImGui::Button("Reset all")) {
                    for (auto& piece : map_pieces) {
                        piece.position = piece.initial_position;
                        piece.yaw = piece.initial_yaw;
                        piece.scale = 1.0f;
                        piece.visible = true;
                    }
                    commit_history();
                    editor_status = "reset all pieces";
                }
                ImGui::InputText("filter", piece_filter, sizeof(piece_filter));
                ImGui::BeginChild("piece list", ImVec2(0.0f, 250.0f), true);
                const std::string filter_text(piece_filter);
                int shown = 0;
                for (std::size_t i = 0; i < map_pieces.size() && shown < 600; ++i) {
                    const auto& piece = map_pieces[i];
                    if (!filter_text.empty() &&
                        piece.material.find(filter_text) == std::string::npos) {
                        continue;
                    }
                    ++shown;
                    const std::size_t slash = piece.material.rfind('/');
                    const std::string name = slash == std::string::npos
                                                 ? piece.material
                                                 : piece.material.substr(slash + 1);
                    const std::string label = std::to_string(i) + "  " + name;
                    if (ImGui::Selectable(label.c_str(),
                                          selected_piece == static_cast<int>(i))) {
                        selected_piece = static_cast<int>(i);
                    }
                }
                ImGui::EndChild();

                if (selected_piece >= 0 &&
                    selected_piece < static_cast<int>(map_pieces.size())) {
                    auto& piece = map_pieces[selected_piece];
                    ImGui::TextWrapped("%s", piece.material.c_str());
                    ImGui::DragFloat3("move", &piece.position.x, 5.0f);
                    if (ImGui::IsItemDeactivatedAfterEdit()) commit_history();
                    ImGui::DragFloat("yaw", &piece.yaw, 0.5f, -180.0f, 180.0f);
                    if (ImGui::IsItemDeactivatedAfterEdit()) commit_history();
                    ImGui::DragFloat("scale", &piece.scale, 0.02f, 0.05f, 20.0f);
                    if (ImGui::IsItemDeactivatedAfterEdit()) commit_history();
                    if (ImGui::Checkbox("visible", &piece.visible)) commit_history();
                    if (ImGui::Button("reset piece")) {
                        piece.position = piece.initial_position;
                        piece.yaw = piece.initial_yaw;
                        piece.scale = 1.0f;
                        commit_history();
                    }
                } else {
                    ImGui::TextUnformatted("click a piece in the world, or pick from the list");
                }
                ImGui::End();
            }

            ImGui::Render();            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            if (!headless) {
                renderer.end_frame();
            } else if (!captured && args.capture_after >= 0.0f &&
                       elapsed >= args.capture_after) {
                renderer.capture_default(args.capture, w, h);
                std::printf("captured %s at t=%.2fs\n", args.capture.c_str(), elapsed);
                captured = true;
                running = false;
            }
        }

        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
