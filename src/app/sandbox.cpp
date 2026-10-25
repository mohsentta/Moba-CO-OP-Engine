// lolview practice tool - League-style view.
//
// Loads a champion's mesh, textures, skeleton, animations and VFX out of the
// user's own League of Legends installation and presents a practice-range style
// scene: a locked isometric camera, right-click movement, cursor-aimed casts and
// a game HUD (ability icons, cooldowns, health/mana, target health bar).
//
// Nothing is shipped here; every asset is resolved by hash at runtime.

#include <lolview/anm.hpp>
#include <lolview/asset_source.hpp>
#include <lolview/bin.hpp>
#include <lolview/mapgeo.hpp>
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
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kFovDegrees = 45.0f;

// ---------------------------------------------------------------------------
// League-style camera: fixed pitch, no roll, follows the champion when locked.
// ---------------------------------------------------------------------------
struct LeagueCamera {
    lolview::Vec3 focus{0.0f, 0.0f, 0.0f};  // ground point being looked at
    float distance = 1800.0f;
    float pitch = 55.0f * kPi / 180.0f;
    bool locked = true;

    lolview::Vec3 eye() const {
        return lolview::Vec3{focus.x, focus.y + distance * std::sin(pitch),
                             focus.z - distance * std::cos(pitch)};
    }
    lolview::Mat4 view() const {
        return lolview::look_at(eye(), focus, lolview::Vec3{0.0f, 1.0f, 0.0f});
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
        return lolview::look_at(position, position + forward(),
                                lolview::Vec3{0.0f, 1.0f, 0.0f});
    }
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
    int select_piece = -1;
    std::string scene_path;
    std::string capture;
    std::string auto_cast;
    float capture_after = -1.0f;
    float exit_after = -1.0f;
    float pitch_degrees = 0.0f;   // 0 = use the default
    float distance = 0.0f;
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

// Same pivot-based transform the renderer uses for a piece.
lolview::Mat4 piece_matrix(const lolview::Vec3& position, const lolview::Vec3& center, float yaw,
                           float scale) {
    return lolview::translation(position) * lolview::translation(center) * rotation_y(yaw) *
           lolview::scaling(lolview::Vec3{scale, scale, scale}) *
           lolview::translation(lolview::Vec3{-center.x, -center.y, -center.z});
}

// Camera ray through a screen pixel.
bool camera_ray(const LeagueCamera& camera, float screen_x, float screen_y, int width,
                int height, lolview::Vec3& origin, lolview::Vec3& direction) {
    const float aspect = static_cast<float>(width) / static_cast<float>(std::max(1, height));
    const float tan_half = std::tan(kFovDegrees * 0.5f * kPi / 180.0f);

    origin = camera.eye();
    const lolview::Vec3 forward = lolview::normalize(camera.focus - origin);
    const lolview::Vec3 right =
        lolview::normalize(lolview::cross(forward, lolview::Vec3{0.0f, 1.0f, 0.0f}));
    const lolview::Vec3 up = lolview::cross(right, forward);

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
        // ---- assets from the user's install ----
        const auto mesh_bytes =
            resolve_chunk(args.game_dir, args.archive, skin_path(args.champion, ".skn"));
        const auto skeleton_bytes =
            resolve_chunk(args.game_dir, args.archive, skin_path(args.champion, ".skl"));
        const auto texture_bytes =
            resolve_chunk(args.game_dir, args.archive, skin_path(args.champion, "_TX_CM.tex"));
        if (!mesh_bytes || !skeleton_bytes) {
            std::fprintf(stderr, "error: could not resolve %s's mesh/skeleton in %s\n",
                         args.champion.c_str(), args.archive.c_str());
            return 1;
        }

        const lolview::Skn mesh = lolview::Skn::parse(*mesh_bytes);
        const lolview::Skl skeleton = lolview::Skl::parse(*skeleton_bytes);
        lolview::TexImage texture;
        const bool has_texture = texture_bytes.has_value();
        if (has_texture) texture = lolview::decode_tex(*texture_bytes);

        std::map<std::string, lolview::Anm> clips;
        for (const char* name : {"Idle1", "Run", "Spell1", "Spell2", "Spell3", "Spell4",
                                 "attack1"}) {
            const auto bytes =
                resolve_chunk(args.game_dir, args.archive, clip_path(args.champion, name));
            if (!bytes) continue;
            try {
                clips.emplace(name, lolview::Anm::parse(*bytes));
            } catch (const std::exception&) {
            }
        }
        if (clips.empty()) {
            std::fprintf(stderr, "error: no animation clips could be loaded\n");
            return 1;
        }

        std::printf("lolview practice tool - %s\n", args.champion.c_str());
        std::printf("  mesh    : %zu verts, %zu tris\n", mesh.vertices().size(),
                    mesh.triangle_count());
        std::printf("  skeleton: %zu joints, %zu influences\n", skeleton.joints().size(),
                    skeleton.influences().size());
        std::printf("  clips   : ");
        for (const auto& [name, clip] : clips) std::printf("%s ", name.c_str());
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
                    spawn = lolview::Vec3{focus.x, have_ground ? ground_y : spawn_y, focus.z};
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
                        for (std::size_t v = 0; v < source.vertices.size(); ++v) {
                            const auto& in = source.vertices[v];
                            lolview::SknVertex out{};
                            out.position = {in.px, in.py, in.pz};
                            out.normal = {in.nx, in.ny, in.nz};
                            out.uv = {in.u, in.v};
                            out.weights = {1.0f, 0.0f, 0.0f, 0.0f};
                            piece_vertices[v] = out;
                        }
                        std::vector<std::uint16_t> piece_indices(source.indices.begin(),
                                                                 source.indices.end());
                        piece.mesh = lolview::Skn::from_parts(std::move(piece_materials),
                                                             std::move(piece_vertices),
                                                             std::move(piece_indices));
                        piece.position = {source.transform.m[12], source.transform.m[13],
                                          source.transform.m[14]};

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
                                        512);
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
        for (int i = 0; i < 4; ++i) {
            const char* keys[4] = {"Q", "W", "E", "R"};
            const auto bytes =
                resolve_chunk(args.game_dir, args.archive, icon_path(args.champion, keys[i]));
            if (!bytes) continue;
            try {
                icon_textures[i] = renderer.upload_texture(lolview::decode_dds(*bytes));
            } catch (const std::exception&) {
                icon_textures[i] = 0;
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

        load_scene();
        if (args.save_scene_on_start) {
            for (auto& piece : map_pieces) {
                piece.initial_position = piece.position;
                piece.initial_yaw = piece.yaw;
            }
            save_scene();
        }
        LeagueCamera camera;
        FreeCamera free_camera;
        bool free_mode = false;
        bool editor_mode = false;  // F2: free-fly camera + editor window

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
        lolview::Vec3 champion_position = spawn;
        float champion_yaw = 0.0f;
        bool has_move_target = false;
        lolview::Vec3 move_target{};

        lolview::Vec3 dummy_position = spawn + lolview::Vec3{0.0f, 0.0f, 700.0f};
        float dummy_hp = 1000.0f;
        const float dummy_max_hp = 1000.0f;
        float dummy_flash = 0.0f;

        const lolview::Skn dummy_mesh = lolview::Skn::make_cube(40.0f);
        const lolview::Skn ground = lolview::Skn::make_plane(3000.0f);
        const lolview::TexImage ground_texture = make_ground_texture();
        const unsigned ground_texture_id = renderer.upload_texture(ground_texture);

        std::vector<ActiveEffect> effects;
        float cooldowns[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const float cooldown_max[4] = {7.0f, 9.0f, 14.0f, 120.0f};

        std::string current_clip = "Idle1";
        float clip_time = 0.0f;
        float cast_lock = 0.0f;

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

            lolview::Vec3 direction = aim - champion_position;
            direction.y = 0.0f;
            if (lolview::length(direction) < 1.0f) direction = {0.0f, 0.0f, 1.0f};
            direction = lolview::normalize(direction);
            champion_yaw = std::atan2(direction.x, direction.z);

            const char* clip_names[4] = {"Spell1", "Spell2", "Spell3", "Spell4"};
            if (clips.count(clip_names[slot])) {
                current_clip = clip_names[slot];
                clip_time = 0.0f;
                cast_lock = clips.at(clip_names[slot]).duration();
            }

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
                    }
                }

                if (event.type == SDL_EVENT_MOUSE_MOTION && free_mode) {
                    // Right-drag looks around in free-fly mode.
                    const SDL_MouseButtonFlags buttons = SDL_GetMouseState(nullptr, nullptr);
                    if ((buttons & SDL_BUTTON_RMASK) != 0) {
                        free_camera.yaw -= static_cast<float>(event.motion.xrel) * 0.0025f;
                        free_camera.pitch = std::clamp(
                            free_camera.pitch -
                                static_cast<float>(event.motion.yrel) * 0.0025f,
                            -1.5f, 1.5f);
                    }
                }

                if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                    camera.distance = std::clamp(
                        camera.distance * (1.0f - event.wheel.y * 0.1f), 900.0f, 2400.0f);
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
                            start_cast(lolview::EffectKind::Spell1, 0, aim);
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
                        case SDL_SCANCODE_S:
                            // League's stop command (free camera uses S to fly back).
                            if (!free_mode) has_move_target = false;
                            break;
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
                const float speed = keys[SDL_SCANCODE_LSHIFT] ? 3600.0f : 1200.0f;
                const lolview::Vec3 forward = free_camera.forward();
                const lolview::Vec3 right = lolview::normalize(
                    lolview::cross(forward, lolview::Vec3{0.0f, 1.0f, 0.0f}));
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

            // ---- movement ----
            bool moving = false;
            if (has_move_target) {
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
            cast_lock = std::max(0.0f, cast_lock - dt);
            std::string desired = moving ? "Run" : "Idle1";
            if (cast_lock > 0.0f && !current_clip.empty() &&
                current_clip.rfind("Spell", 0) == 0) {
                desired = current_clip;
            }
            if (!clips.count(desired)) desired = clips.begin()->first;
            if (desired != current_clip) {
                current_clip = desired;
                clip_time = 0.0f;
            }
            clip_time += dt;
            const lolview::Anm& clip = clips.at(current_clip);
            const float sampled = std::fmod(clip_time, std::max(clip.duration(), 0.01f));
            const auto bones = lolview::compute_skin_matrices(skeleton, &clip, sampled);

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
                }
            }
            effects.erase(std::remove_if(effects.begin(), effects.end(),
                                         [](const ActiveEffect& e) { return e.age > 3.0f; }),
                          effects.end());

            // ---- draw ----
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(renderer.window(), &w, &h);
            if (w <= 0 || h <= 0) {
                w = args.width;
                h = args.height;
            }

            renderer.begin_frame(w, h);
            const lolview::Mat4 view = free_mode ? free_camera.view() : camera.view();
            const lolview::Mat4 proj = lolview::perspective(
                kFovDegrees * kPi / 180.0f, static_cast<float>(w) / static_cast<float>(h),
                1.0f, 8000.0f);

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
            if (has_pet) {
                renderer.draw_scene(pet_mesh, has_pet_texture ? &pet_texture : nullptr, nullptr,
                                    nullptr, lolview::translation(pet_position), view, proj, w, h);
            }
            renderer.draw_scene(dummy_mesh, nullptr, nullptr, nullptr,
                                lolview::translation(dummy_position + lolview::Vec3{0, 40, 0}),
                                view, proj, w, h);
            for (const auto& [sprite, batch] : batches) {
                if (!batch.empty()) renderer.draw_particles(batch, sprite, view, proj);
            }

            // ---- HUD ----
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();

            // Click in the world selects a piece (unless the editor has the mouse).
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                !ImGui::GetIO().WantCaptureMouse && !map_pieces.empty()) {
                float mouse_x = 0.0f, mouse_y = 0.0f;
                SDL_GetMouseState(&mouse_x, &mouse_y);
                lolview::Vec3 ray_origin, ray_direction;
                camera_ray(camera, mouse_x, mouse_y, w, h, ray_origin, ray_direction);
                float best_t = 1e30f;
                int best_index = -1;
                for (std::size_t i = 0; i < map_pieces.size(); ++i) {
                    const auto& piece = map_pieces[i];
                    if (!piece.visible) continue;
                    const lolview::Vec3 center{piece.position.x + piece.center.x * piece.scale,
                                               piece.position.y + piece.center.y * piece.scale,
                                               piece.position.z + piece.center.z * piece.scale};
                    const lolview::Vec3 to_center = center - ray_origin;
                    const float t = to_center.x * ray_direction.x +
                                    to_center.y * ray_direction.y +
                                    to_center.z * ray_direction.z;
                    if (t <= 0.0f) continue;
                    const lolview::Vec3 closest{to_center.x - ray_direction.x * t,
                                                to_center.y - ray_direction.y * t,
                                                to_center.z - ray_direction.z * t};
                    const float distance = lolview::length(closest);
                    const float radius = std::max(piece.radius * piece.scale, 15.0f);
                    if (distance <= radius && t < best_t) {
                        best_t = t;
                        best_index = static_cast<int>(i);
                    }
                }
                if (best_index >= 0) {
                    selected_piece = best_index;
                    editor_status = "selected piece " + std::to_string(best_index);
                }
            }

            ImDrawList* draw = ImGui::GetBackgroundDrawList();

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

                if (icon_textures[i] != 0) {
                    draw->AddImage(static_cast<ImTextureID>(icon_textures[i]), top_left,
                                   bottom_right);
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

            // outline of the selected piece
            if (selected_piece >= 0 && selected_piece < static_cast<int>(map_pieces.size())) {
                const auto& piece = map_pieces[selected_piece];
                const lolview::Mat4 model =
                    piece_matrix(piece.position, piece.center, piece.yaw, piece.scale);
                lolview::Vec3 corners[8];
                for (int i = 0; i < 8; ++i) {
                    const lolview::Vec3 local{(i & 1) ? piece.bounds_max.x : piece.bounds_min.x,
                                              (i & 2) ? piece.bounds_max.y : piece.bounds_min.y,
                                              (i & 4) ? piece.bounds_max.z : piece.bounds_min.z};
                    corners[i] = transform_point(model, local);
                }
                float sx[8] = {0.0f};
                float sy[8] = {0.0f};
                bool valid[8] = {false};
                for (int i = 0; i < 8; ++i) {
                    valid[i] = project_to_screen(view, proj, corners[i], w, h, sx[i], sy[i]);
                }
                static const int edges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
                                                 {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                (void)edges;

                // Screen-space brackets: readable even when a piece spans the map.
                float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
                int visible_corners = 0;
                for (int i = 0; i < 8; ++i) {
                    if (!valid[i]) continue;
                    min_x = std::min(min_x, sx[i]);
                    min_y = std::min(min_y, sy[i]);
                    max_x = std::max(max_x, sx[i]);
                    max_y = std::max(max_y, sy[i]);
                    ++visible_corners;
                }
                if (visible_corners > 0) {
                    const float pad = 5.0f;
                    min_x = std::clamp(min_x - pad, 0.0f, static_cast<float>(w) - 1.0f);
                    min_y = std::clamp(min_y - pad, 0.0f, static_cast<float>(h) - 1.0f);
                    max_x = std::clamp(max_x + pad, 0.0f, static_cast<float>(w) - 1.0f);
                    max_y = std::clamp(max_y + pad, 0.0f, static_cast<float>(h) - 1.0f);
                    const ImU32 color = IM_COL32(120, 240, 160, 240);
                    draw->AddRect(ImVec2(min_x, min_y), ImVec2(max_x, max_y),
                                  IM_COL32(120, 240, 160, 70), 0.0f, 0, 1.0f);
                    const float len = std::min(20.0f, (max_x - min_x) * 0.25f);
                    const float len_y = std::min(20.0f, (max_y - min_y) * 0.25f);
                    draw->AddLine(ImVec2(min_x, min_y), ImVec2(min_x + len, min_y), color, 2.0f);
                    draw->AddLine(ImVec2(min_x, min_y), ImVec2(min_x, min_y + len_y), color, 2.0f);
                    draw->AddLine(ImVec2(max_x, min_y), ImVec2(max_x - len, min_y), color, 2.0f);
                    draw->AddLine(ImVec2(max_x, min_y), ImVec2(max_x, min_y + len_y), color, 2.0f);
                    draw->AddLine(ImVec2(min_x, max_y), ImVec2(min_x + len, max_y), color, 2.0f);
                    draw->AddLine(ImVec2(min_x, max_y), ImVec2(min_x, max_y - len_y), color, 2.0f);
                    draw->AddLine(ImVec2(max_x, max_y), ImVec2(max_x - len, max_y), color, 2.0f);
                    draw->AddLine(ImVec2(max_x, max_y), ImVec2(max_x, max_y - len_y), color, 2.0f);
                }
            }

            // move-target marker
            if (has_move_target) {
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
                          "%.0f fps   |   RMB move   S stop   QWER cast   Y %s camera   "
                          "F2 %s",
                          fps, camera.locked ? "locked" : "free",
                          editor_mode ? "editor ON (WASD fly)" : "editor");
            draw->AddText(ImVec2(14.0f, 30.0f), IM_COL32(150, 160, 175, 210), status);
            draw->AddText(ImVec2(14.0f, static_cast<float>(h) - 22.0f),
                          IM_COL32(120, 130, 145, 190),
                          "not affiliated with Riot Games - assets read from your own install");

            // ---- map editor (F2) ----
            if (editor_mode && !map_pieces.empty()) {
                ImGui::SetNextWindowPos(ImVec2(static_cast<float>(w) - 400.0f, 56.0f),
                                        ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(380.0f, 540.0f), ImGuiCond_FirstUseEver);
                ImGui::Begin("Map editor");
                ImGui::Text("%zu pieces   |   %s", map_pieces.size(), editor_status.c_str());
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
                    ImGui::DragFloat("yaw", &piece.yaw, 0.5f, -180.0f, 180.0f);
                    ImGui::DragFloat("scale", &piece.scale, 0.02f, 0.05f, 20.0f);
                    ImGui::Checkbox("visible", &piece.visible);
                    if (ImGui::Button("reset piece")) {
                        piece.position = piece.initial_position;
                        piece.yaw = piece.initial_yaw;
                        piece.scale = 1.0f;
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
