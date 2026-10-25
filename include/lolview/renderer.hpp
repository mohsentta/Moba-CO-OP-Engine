#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <lolview/math.hpp>

struct SDL_Window;

namespace lolview {

class Skn;
struct TexImage;
struct Particle;

struct RenderOptions {
    int width = 1024;
    int height = 1024;
    float yaw_degrees = 35.0f;
    float pitch_degrees = 12.0f;
    float distance_scale = 2.2f;
    Vec3 background{0.07f, 0.08f, 0.10f};
};

// Writes an RGBA8, top-down image as PNG. Throws on failure.
void save_png(const std::filesystem::path& path, int width, int height,
              const std::uint8_t* rgba);

// Owns the SDL3 window and the OpenGL 4.6 core context. The same instance backs
// both the offscreen capture path and the interactive sandbox: capture draws
// into an FBO, the sandbox draws into the default framebuffer.
class Renderer {
public:
    explicit Renderer(int width = 64, int height = 64, bool visible = false);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Offscreen: frame the mesh automatically and write a PNG.
    void render_skn(const Skn& mesh, const TexImage* texture,
                    const std::vector<Mat4>* skin_matrices,
                    const std::vector<Particle>* particles, const RenderOptions& options,
                    const std::filesystem::path& png_path);

    // Interactive primitives: caller owns the camera and the frame loop.
    void begin_frame(int width, int height);
    void draw_scene(const Skn& mesh, const TexImage* texture,
                    const std::vector<Mat4>* skin_matrices,
                    const std::vector<Particle>* particles, const Mat4& model, const Mat4& view,
                    const Mat4& proj, int width, int height);

    // Draws particles alone. `sprite` overrides the built-in soft sprite, so
    // real VFX textures from the game can be used.
    void draw_particles(const std::vector<Particle>& particles, const TexImage* sprite,
                        const Mat4& view, const Mat4& proj);
    void end_frame();

    // Reads the default framebuffer (call before end_frame, after ImGui).
    void capture_default(const std::filesystem::path& png_path, int width, int height);

    SDL_Window* window() const noexcept;
    void* gl_context() const noexcept;
    const std::string& gl_version() const noexcept;

    // Uploads an image and returns a GL texture handle (used by the HUD).
    unsigned upload_texture(const TexImage& image);

    // Alpha handling for draw_scene: 0 opaque, 1 cutout (alpha test), 2 blended.
    void set_alpha_mode(int mode) noexcept { alpha_mode_ = mode; }

    // Cached geometry for scenes with many static pieces: upload once, draw many.
    // Ids stay valid for the lifetime of the renderer; the mesh/texture must
    // outlive the cache entry (keyed by address).
    int cache_mesh(const Skn& mesh);
    int cache_texture(const TexImage& image);
    void draw_cached(int mesh_id, int texture_id, const Mat4& model, const Mat4& view,
                     const Mat4& proj, int width, int height, int alpha_mode);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    int alpha_mode_ = 0;
};

}  // namespace lolview
