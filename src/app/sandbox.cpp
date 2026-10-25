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
#include <map>
#include <optional>
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
    float distance = 880.0f;
    float pitch = 40.0f * kPi / 180.0f;
    bool locked = true;

    lolview::Vec3 eye() const {
        return lolview::Vec3{focus.x, focus.y + distance * std::sin(pitch),
                             focus.z - distance * std::cos(pitch)};
    }
    lolview::Mat4 view() const {
        return lolview::look_at(eye(), focus, lolview::Vec3{0.0f, 1.0f, 0.0f});
    }
};

struct ActiveEffect {
    lolview::EffectKind kind = lolview::EffectKind::None;
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
std::optional<std::vector<std::uint8_t>> resolve_chunk(const std::string& game_dir,
                                                       const std::string& archive_filter,
                                                       const std::string& virtual_path) {
    lolview::AssetSource source(game_dir);
    const std::uint64_t key = lolview::path_hash(virtual_path);
    for (const auto& archive_path : source.archives()) {
        if (archive_path.string().find(archive_filter) == std::string::npos) continue;
        const auto wad = lolview::Wad::open(archive_path);
        const auto index = wad.find(key);
        if (!index) continue;
        return wad.read_chunk(wad.entries()[*index]);
    }
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> resolve_chunk_hash(const std::string& game_dir,
                                                           const std::string& archive_filter,
                                                           std::uint64_t hash) {
    lolview::AssetSource source(game_dir);
    for (const auto& archive_path : source.archives()) {
        if (archive_path.string().find(archive_filter) == std::string::npos) continue;
        const auto wad = lolview::Wad::open(archive_path);
        const auto index = wad.find(hash);
        if (!index) continue;
        return wad.read_chunk(wad.entries()[*index]);
    }
    return std::nullopt;
}

std::string clip_path(const std::string& champion, const std::string& clip) {
    return "ASSETS/Characters/" + champion + "/Skins/Base/Animations/" + clip + ".anm";
}

std::string skin_path(const std::string& champion, const char* suffix) {
    return "ASSETS/Characters/" + champion + "/Skins/Base/" + champion + "_Base" + suffix;
}

std::string icon_path(const std::string& champion, const char* ability) {
    return "ASSETS/Characters/" + champion + "/HUD/Icons2D/Icons_" + champion + "_" +
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

// Camera ray through a screen pixel, intersected with the ground plane y = 0.
bool pick_ground(const LeagueCamera& camera, float screen_x, float screen_y, int width,
                 int height, lolview::Vec3& out) {
    const float aspect = static_cast<float>(width) / static_cast<float>(std::max(1, height));
    const float tan_half = std::tan(kFovDegrees * 0.5f * kPi / 180.0f);

    const lolview::Vec3 eye = camera.eye();
    const lolview::Vec3 forward = lolview::normalize(camera.focus - eye);
    const lolview::Vec3 right =
        lolview::normalize(lolview::cross(forward, lolview::Vec3{0.0f, 1.0f, 0.0f}));
    const lolview::Vec3 up = lolview::cross(right, forward);

    const float ndc_x = (2.0f * screen_x / static_cast<float>(width)) - 1.0f;
    const float ndc_y = 1.0f - (2.0f * screen_y / static_cast<float>(height));
    const lolview::Vec3 direction = lolview::normalize(
        forward + right * (ndc_x * tan_half * aspect) + up * (ndc_y * tan_half));

    if (std::fabs(direction.y) < 1e-5f) return false;
    const float t = -eye.y / direction.y;
    if (t < 0.0f) return false;
    out = eye + direction * t;
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
        LeagueCamera camera;
        if (args.pitch_degrees > 0.0f) camera.pitch = args.pitch_degrees * kPi / 180.0f;
        if (args.distance > 0.0f) camera.distance = args.distance;
        lolview::Vec3 champion_position{0.0f, 0.0f, 0.0f};
        float champion_yaw = 0.0f;
        bool has_move_target = false;
        lolview::Vec3 move_target{};

        const lolview::Vec3 dummy_position{0.0f, 0.0f, 460.0f};
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
                    event.button.button == SDL_BUTTON_RIGHT) {
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
                        case SDL_SCANCODE_Y:
                            camera.locked = !camera.locked;
                            break;
                        case SDL_SCANCODE_SPACE:
                            camera.focus = champion_position;
                            camera.locked = true;
                            break;
                        case SDL_SCANCODE_A:
                        case SDL_SCANCODE_D:
                            camera.locked = false;
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
                camera.focus = champion_position + lolview::Vec3{0.0f, 0.0f, 120.0f};
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
            const lolview::Mat4 view = camera.view();
            const lolview::Mat4 proj = lolview::perspective(
                kFovDegrees * kPi / 180.0f, static_cast<float>(w) / static_cast<float>(h),
                1.0f, 8000.0f);

            renderer.draw_scene(ground, &ground_texture, nullptr, nullptr,
                                lolview::Mat4::identity(), view, proj, w, h);
            renderer.draw_scene(mesh, has_texture ? &texture : nullptr, &bones, nullptr,
                                model_matrix(champion_position, champion_yaw), view, proj, w, h);
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
                          "%.0f fps   |   Y: %s camera   |   RMB move   QWER cast aim with mouse",
                          fps, camera.locked ? "locked" : "free");
            draw->AddText(ImVec2(14.0f, 30.0f), IM_COL32(150, 160, 175, 210), status);
            draw->AddText(ImVec2(14.0f, static_cast<float>(h) - 22.0f),
                          IM_COL32(120, 130, 145, 190),
                          "not affiliated with Riot Games - assets read from your own install");

            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

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
