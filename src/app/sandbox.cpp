// lolview practice sandbox.
//
// A local, single-player sandbox that loads one champion's mesh, texture,
// skeleton and animations out of the user's own League of Legends install and
// lets you drive it: free camera, an animation clock, ability triggers with
// particle effects, and a dummy target with health.
//
// Nothing is shipped with this program; every asset is resolved by hash out of
// an installation the user points it at, at runtime.

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

struct Camera {
    lolview::Vec3 position{320.0f, 190.0f, -380.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    float speed = 260.0f;

    lolview::Vec3 forward() const {
        return lolview::Vec3{-std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                             -std::cos(yaw) * std::cos(pitch)};
    }
    lolview::Vec3 target() const { return position + forward() * 100.0f; }

    // Point the camera at a world position, keeping the current position.
    void aim_at(const lolview::Vec3& target) {
        const lolview::Vec3 dir = lolview::normalize(target - position);
        pitch = std::asin(std::clamp(dir.y, -1.0f, 1.0f));
        yaw = std::atan2(-dir.x, -dir.z);
    }
};

struct ActiveEffect {
    lolview::EffectKind kind = lolview::EffectKind::None;
    float age = 0.0f;
    bool hit_applied = false;
    const lolview::VfxSystem* vfx = nullptr;   // real game system, when loaded
    const std::vector<lolview::TexImage>* textures = nullptr;
};

struct Args {
    std::string game_dir;
    std::string archive = "Ahri.wad";
    std::string champion = "Ahri";
    std::string clip = "Idle1";
    std::string capture;
    std::string auto_cast;
    std::string vfx_bin;
    float capture_after = -1.0f;
    float exit_after = -1.0f;
    int width = 1280;
    int height = 720;
};

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

void apply_theme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.11f, 0.92f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.13f, 0.17f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.18f, 0.20f, 0.28f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.22f, 0.25f, 0.36f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.31f, 0.35f, 0.50f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.40f, 0.45f, 0.62f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.24f, 0.27f, 0.38f, 1.0f);
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
        } else if (arg == "--clip") {
            if (!next(args.clip)) return 2;
        } else if (arg == "--capture") {
            if (!next(args.capture)) return 2;
        } else if (arg == "--auto-cast") {
            if (!next(args.auto_cast)) return 2;
        } else if (arg == "--vfx-bin") {
            if (!next(args.vfx_bin)) return 2;
        } else if (arg == "--capture-after") {
            std::string value;
            if (!next(value)) return 2;
            args.capture_after = static_cast<float>(std::atof(value.c_str()));
        } else if (arg == "--exit-after") {
            std::string value;
            if (!next(value)) return 2;
            args.exit_after = static_cast<float>(std::atof(value.c_str()));
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
                     "                       [--archive Ahri.wad] [--clip Idle1]\n"
                     "                       [--capture out.png --capture-after 2.5]\n");
        return 2;
    }

    try {
        // ---- assets from the user's install ----
        const auto mesh_bytes = resolve_chunk(args.game_dir, args.archive,
                                              skin_path(args.champion, ".skn"));
        const auto skeleton_bytes = resolve_chunk(args.game_dir, args.archive,
                                                  skin_path(args.champion, ".skl"));
        const auto texture_bytes = resolve_chunk(
            args.game_dir, args.archive, skin_path(args.champion, "_TX_CM.tex"));
        if (!mesh_bytes || !skeleton_bytes) {
            std::fprintf(stderr, "error: could not resolve %s's mesh/skeleton in %s\n",
                         args.champion.c_str(), args.archive.c_str());
            return 1;
        }

        const lolview::Skn mesh = lolview::Skn::parse(*mesh_bytes);
        const lolview::Skl skeleton = lolview::Skl::parse(*skeleton_bytes);
        lolview::TexImage texture;
        bool has_texture = false;
        if (texture_bytes) {
            texture = lolview::decode_tex(*texture_bytes);
            has_texture = true;
        }

        std::map<std::string, lolview::Anm> clips;
        for (const char* name : {"Idle1", "Idle2", "Run", "Spell1", "Spell2", "Spell3",
                                 "Spell4", "attack1", "death"}) {
            const auto bytes = resolve_chunk(args.game_dir, args.archive,
                                             clip_path(args.champion, name));
            if (!bytes) continue;
            try {
                clips.emplace(name, lolview::Anm::parse(*bytes));
            } catch (const std::exception&) {
                // A clip that fails to parse is skipped, not fatal.
            }
        }
        if (clips.empty()) {
            std::fprintf(stderr, "error: no animation clips could be loaded\n");
            return 1;
        }

        std::printf("lolview sandbox - %s\n", args.champion.c_str());
        std::printf("  mesh    : %zu verts, %zu tris\n", mesh.vertices().size(),
                    mesh.triangle_count());
        std::printf("  skeleton: %zu joints, %zu influences\n", skeleton.joints().size(),
                    skeleton.influences().size());
        std::printf("  clips   : ");
        for (const auto& [name, clip] : clips) {
            std::printf("%s(%.2fs) ", name.c_str(), clip.duration());
        }
        std::printf("\n");

        // ---- real VFX systems, when a bin hash is supplied ----
        std::vector<lolview::VfxSystem> vfx_systems;
        std::map<lolview::EffectKind, const lolview::VfxSystem*> vfx_by_kind;
        std::map<const lolview::VfxSystem*, std::vector<lolview::TexImage>> vfx_textures;

        if (!args.vfx_bin.empty()) {
            const std::uint64_t bin_hash = std::strtoull(args.vfx_bin.c_str(), nullptr, 16);
            const auto bytes = resolve_chunk_hash(args.game_dir, args.archive, bin_hash);
            if (!bytes) {
                std::fprintf(stderr, "warning: VFX bin %s not found in %s\n",
                             args.vfx_bin.c_str(), args.archive.c_str());
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
                            // A texture that will not decode falls back to the sprite.
                        }
                    }
                    vfx_textures[system] = std::move(textures);
                }
                std::printf("vfx     : %zu systems, %zu mapped to abilities\n",
                            vfx_systems.size(), vfx_by_kind.size());
            }
        }

        const bool headless = !args.capture.empty();
        lolview::Renderer renderer(args.width, args.height, !headless);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;  // no imgui.ini litter
        apply_theme();
        ImGui_ImplSDL3_InitForOpenGL(renderer.window(), renderer.gl_context());
        ImGui_ImplOpenGL3_Init("#version 330 core");

        // ---- sandbox state ----
        Camera camera;
        camera.position = {330.0f, 210.0f, -390.0f};
        camera.aim_at(lolview::Vec3{0.0f, 105.0f, 60.0f});
        std::string current_clip = clips.count(args.clip) ? args.clip : clips.begin()->first;
        float clip_time = 0.0f;
        float back_to_idle = 0.0f;

        lolview::Skn dummy = lolview::Skn::make_cube(28.0f);
        float dummy_hp = 1000.0f;
        float dummy_max_hp = 1000.0f;
        const lolview::Vec3 dummy_position{0.0f, 40.0f, 210.0f};

        std::vector<ActiveEffect> effects;
        bool running = true;
        float elapsed = 0.0f;
        bool captured = false;

        std::uint64_t last_counter = SDL_GetPerformanceCounter();
        const double frequency = static_cast<double>(SDL_GetPerformanceFrequency());
        float fps = 0.0f;

        const auto cast = [&](lolview::EffectKind kind, const std::string& clip) {
            if (clips.count(clip)) {
                current_clip = clip;
                clip_time = 0.0f;
                back_to_idle = clips.at(clip).duration();
            }
            ActiveEffect effect;
            effect.kind = kind;
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
            const char* clip_name = nullptr;
            switch (kind) {
                case lolview::EffectKind::Spell1: clip_name = "Spell1"; break;
                case lolview::EffectKind::Spell2: clip_name = "Spell2"; break;
                case lolview::EffectKind::Spell3: clip_name = "Spell3"; break;
                case lolview::EffectKind::Spell4: clip_name = "Spell4"; break;
                default: break;
            }
            if (clip_name != nullptr) cast(kind, clip_name);
        }

        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT) running = false;
                if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;
                if (event.type == SDL_EVENT_MOUSE_MOTION &&
                    (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT))) {
                    camera.yaw -= event.motion.xrel * 0.004f;
                    camera.pitch = std::clamp(camera.pitch - event.motion.yrel * 0.004f,
                                              -1.4f, 1.4f);
                }
                if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                    camera.speed = std::clamp(camera.speed * (1.0f + event.wheel.y * 0.1f),
                                              30.0f, 2000.0f);
                }
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                    switch (event.key.scancode) {
                        case SDL_SCANCODE_Q: cast(lolview::EffectKind::Spell1, "Spell1"); break;
                        case SDL_SCANCODE_W: cast(lolview::EffectKind::Spell2, "Spell2"); break;
                        case SDL_SCANCODE_E: cast(lolview::EffectKind::Spell3, "Spell3"); break;
                        case SDL_SCANCODE_R: cast(lolview::EffectKind::Spell4, "Spell4"); break;
                        case SDL_SCANCODE_ESCAPE: running = false; break;
                        default: break;
                    }
                }
            }

            const std::uint64_t now = SDL_GetPerformanceCounter();
            float dt = static_cast<float>(static_cast<double>(now - last_counter) / frequency);
            last_counter = now;
            dt = std::clamp(dt, 0.0f, 0.1f);
            elapsed += dt;
            if (args.exit_after >= 0.0f && elapsed >= args.exit_after) running = false;
            fps = fps == 0.0f ? 1.0f / std::max(dt, 1e-4f) : fps * 0.92f + (1.0f / std::max(dt, 1e-4f)) * 0.08f;

            // ---- input: free camera ----
            const bool* keys = SDL_GetKeyboardState(nullptr);
            lolview::Vec3 move{};
            const lolview::Vec3 flat_forward{std::sin(camera.yaw), 0.0f, std::cos(camera.yaw)};
            const lolview::Vec3 right{std::cos(camera.yaw), 0.0f, -std::sin(camera.yaw)};
            if (keys[SDL_SCANCODE_W]) move = move + flat_forward;
            if (keys[SDL_SCANCODE_S]) move = move - flat_forward;
            if (keys[SDL_SCANCODE_D]) move = move + right;
            if (keys[SDL_SCANCODE_A]) move = move - right;
            if (keys[SDL_SCANCODE_SPACE]) move.y += 1.0f;
            if (keys[SDL_SCANCODE_LCTRL]) move.y -= 1.0f;
            if (lolview::length(move) > 0.001f) {
                camera.position = camera.position + normalize(move) * (camera.speed * dt);
            }

            // ---- animation clock ----
            const lolview::Anm& clip = clips.at(current_clip);
            clip_time += dt;
            if (back_to_idle > 0.0f) {
                back_to_idle -= dt;
                if (back_to_idle <= 0.0f && clips.count("Idle1")) {
                    current_clip = "Idle1";
                    clip_time = 0.0f;
                }
            }
            const float sampled = std::fmod(clip_time, std::max(clip.duration(), 0.01f));
            const auto bones = lolview::compute_skin_matrices(skeleton, &clip, sampled);

            // ---- effects ----
            std::vector<std::pair<const lolview::TexImage*, std::vector<lolview::Particle>>>
                batches;
            const lolview::Vec3 effect_origin{0.0f, 130.0f, 40.0f};
            const lolview::Vec3 effect_forward{0.0f, 0.0f, 1.0f};
            const float view_distance =
                std::max(1.0f, lolview::length(effect_origin - camera.position));
            const float pixels_per_unit =
                static_cast<float>(args.height) /
                (2.0f * std::tan(50.0f * kPi / 180.0f * 0.5f) * view_distance);

            for (auto& effect : effects) {
                effect.age += dt;

                std::vector<lolview::Particle> frame;
                if (effect.vfx != nullptr) {
                    const float speed = effect.kind == lolview::EffectKind::Spell4 ? 900.0f
                                        : effect.kind == lolview::EffectKind::Spell1 ? 700.0f
                                                                                     : 500.0f;
                    frame = lolview::simulate_vfx(*effect.vfx, effect.age, effect_origin,
                                                  effect_forward, pixels_per_unit, speed);
                } else {
                    frame = lolview::simulate_effect(effect.kind, effect.age, effect_origin,
                                                     effect_forward);
                }

                // Group by emitter so each group can use its own texture.
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

                if (!effect.hit_applied && effect.age > 1.4f) {
                    effect.hit_applied = true;
                    dummy_hp = std::max(0.0f, dummy_hp - 120.0f);
                }
            }
            effects.erase(std::remove_if(effects.begin(), effects.end(),
                                         [](const ActiveEffect& e) { return e.age > 3.0f; }),
                          effects.end());

            // ---- draw ----
            int w = 0;
            int h = 0;
            SDL_GetWindowSizeInPixels(renderer.window(), &w, &h);
            if (w <= 0 || h <= 0) {
                w = args.width;
                h = args.height;
            }

            renderer.begin_frame(w, h);
            const lolview::Mat4 view =
                lolview::look_at(camera.position, camera.target(),
                                 lolview::Vec3{0.0f, 1.0f, 0.0f});
            const lolview::Mat4 proj = lolview::perspective(
                50.0f * kPi / 180.0f, static_cast<float>(w) / static_cast<float>(h), 1.0f,
                6000.0f);

            renderer.draw_scene(mesh, has_texture ? &texture : nullptr, &bones, nullptr,
                                lolview::Mat4::identity(), view, proj, w, h);
            renderer.draw_scene(dummy, nullptr, nullptr, nullptr,
                                lolview::translation(dummy_position), view, proj, w, h);
            for (const auto& [texture, batch] : batches) {
                if (!batch.empty()) renderer.draw_particles(batch, texture, view, proj);
            }

            // ---- UI ----
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();

            ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(310, 430), ImGuiCond_FirstUseEver);
            ImGui::Begin("lolview practice sandbox");
            ImGui::Text("%s", args.champion.c_str());
            ImGui::TextDisabled("not affiliated with Riot Games");
            ImGui::Separator();

            ImGui::Text("pose  %.2f s / %.2f s", sampled, clip.duration());
            ImGui::Text("clip  %s", current_clip.c_str());
            ImGui::Text("fps   %.0f", fps);
            ImGui::Separator();

            if (ImGui::BeginCombo("animation", current_clip.c_str())) {
                for (const auto& [name, animation] : clips) {
                    const bool selected = name == current_clip;
                    if (ImGui::Selectable(name.c_str(), selected)) {
                        current_clip = name;
                        clip_time = 0.0f;
                        back_to_idle = 0.0f;
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::Separator();
            ImGui::Text("cast");
            if (ImGui::Button("Q##cast")) cast(lolview::EffectKind::Spell1, "Spell1");
            ImGui::SameLine();
            if (ImGui::Button("W##cast")) cast(lolview::EffectKind::Spell2, "Spell2");
            ImGui::SameLine();
            if (ImGui::Button("E##cast")) cast(lolview::EffectKind::Spell3, "Spell3");
            ImGui::SameLine();
            if (ImGui::Button("R##cast")) cast(lolview::EffectKind::Spell4, "Spell4");

            ImGui::Separator();
            ImGui::Text("target dummy");
            ImGui::ProgressBar(dummy_hp / dummy_max_hp, ImVec2(-1, 0),
                               (std::to_string(static_cast<int>(dummy_hp)) + " hp").c_str());
            if (ImGui::Button("reset dummy")) dummy_hp = dummy_max_hp;

            ImGui::Separator();
            ImGui::TextWrapped(
                "WASD move  SPACE/CTRL up-down  RMB look  wheel speed  QWER cast  ESC quit");
            ImGui::End();

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
