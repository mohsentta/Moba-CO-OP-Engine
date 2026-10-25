#include <lolview/renderer.hpp>

#include <lolview/gl.hpp>
#include <lolview/skn.hpp>
#include <lolview/tex.hpp>
#include <lolview/vfx.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <stb_image_write.h>

namespace lolview {
namespace {

constexpr float kPi = 3.14159265358979323846f;

std::string last_sdl_error() { return std::string(SDL_GetError()); }

unsigned compile_shader(unsigned type, const char* source, const char* label) {
    const unsigned shader = gl::CreateShader(type);
    gl::ShaderSource(shader, 1, &source, nullptr);
    gl::CompileShader(shader);

    int ok = 0;
    gl::GetShaderiv(shader, gl::COMPILE_STATUS, &ok);
    if (!ok) {
        std::array<char, 2048> log{};
        gl::GetShaderInfoLog(shader, static_cast<int>(log.size()), nullptr, log.data());
        gl::DeleteShader(shader);
        throw std::runtime_error(std::string("shader compile failed (") + label + "): " +
                                 log.data());
    }
    return shader;
}

unsigned link_program(const char* vertex_source, const char* fragment_source) {
    const unsigned vertex = compile_shader(gl::VERTEX_SHADER, vertex_source, "vertex");
    const unsigned fragment = compile_shader(gl::FRAGMENT_SHADER, fragment_source, "fragment");

    const unsigned program = gl::CreateProgram();
    gl::AttachShader(program, vertex);
    gl::AttachShader(program, fragment);
    gl::LinkProgram(program);
    gl::DeleteShader(vertex);
    gl::DeleteShader(fragment);

    int ok = 0;
    gl::GetProgramiv(program, gl::LINK_STATUS, &ok);
    if (!ok) {
        std::array<char, 2048> log{};
        gl::GetProgramInfoLog(program, static_cast<int>(log.size()), nullptr, log.data());
        gl::DeleteProgram(program);
        throw std::runtime_error(std::string("program link failed: ") + log.data());
    }
    return program;
}

const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec4 a_bones;
layout(location = 4) in vec4 a_weights;

uniform mat4 u_mvp;
layout(std140) uniform BoneBlock {
    mat4 u_bones[256];
};
uniform int u_skinned;

out vec3 v_normal;
out vec2 v_uv;
out vec3 v_position;

void main() {
    mat4 skin = mat4(1.0);
    if (u_skinned != 0) {
        float weight_sum = a_weights.x + a_weights.y + a_weights.z + a_weights.w;
        if (weight_sum > 0.0001) {
            int i0 = int(a_bones.x + 0.5);
            int i1 = int(a_bones.y + 0.5);
            int i2 = int(a_bones.z + 0.5);
            int i3 = int(a_bones.w + 0.5);
            skin = u_bones[i0] * a_weights.x + u_bones[i1] * a_weights.y
                 + u_bones[i2] * a_weights.z + u_bones[i3] * a_weights.w;
        }
    }

    vec4 world = skin * vec4(a_position, 1.0);
    gl_Position = u_mvp * world;
    v_normal = mat3(skin) * a_normal;
    v_uv = a_uv;
    v_position = world.xyz;
}
)";

const char* kFragmentShader = R"(#version 330 core
in vec3 v_normal;
in vec2 v_uv;
in vec3 v_position;

uniform vec3 u_color;
uniform int u_use_texture;
uniform sampler2D u_texture;
uniform int u_alpha_mode;

out vec4 frag_color;

void main() {
    vec3 n = normalize(v_normal);
    vec3 key = normalize(vec3(0.45, 0.85, 0.30));
    vec3 fill = normalize(vec3(-0.55, 0.35, -0.60));
    float key_light = max(dot(n, key), 0.0);
    float fill_light = max(dot(n, fill), 0.0) * 0.35;

    vec3 base = u_color;
    float alpha = 1.0;
    if (u_use_texture != 0) {
        vec4 sampled = texture(u_texture, v_uv);
        base = sampled.rgb;
        alpha = sampled.a;
    }
    // Cutout foliage and decals mask with their alpha channel; blended surfaces
    // (water) fade instead.
    if (u_alpha_mode == 1 && alpha < 0.5) discard;
    if (u_alpha_mode == 2) alpha = max(alpha, 0.25);

    vec3 lit = base * (0.28 + 0.72 * key_light + fill_light);
    frag_color = vec4(lit, u_alpha_mode == 0 ? 1.0 : alpha);
}
)";

const char* kParticleVertexShader = R"(#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in float a_size;

uniform mat4 u_viewproj;

out vec4 v_color;

void main() {
    gl_Position = u_viewproj * vec4(a_position, 1.0);
    gl_PointSize = a_size;
    v_color = a_color;
}
)";

const char* kParticleFragmentShader = R"(#version 330 core
in vec4 v_color;

uniform sampler2D u_sprite;

out vec4 frag_color;

void main() {
    vec4 sprite = texture(u_sprite, gl_PointCoord);
    frag_color = vec4(v_color.rgb * sprite.rgb, v_color.a * sprite.a);
}
)";

struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    std::uint8_t bones[4];
    float weights[4];
};

struct DrawRange {
    std::uint32_t start_index = 0;
    std::uint32_t count = 0;
    Vec3 color;
};

void vertex_input_layout() {
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 3, gl::FLOAT, 0, sizeof(Vertex),
                            reinterpret_cast<const void*>(0));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(1, 3, gl::FLOAT, 0, sizeof(Vertex),
                            reinterpret_cast<const void*>(3 * sizeof(float)));
    gl::EnableVertexAttribArray(2);
    gl::VertexAttribPointer(2, 2, gl::FLOAT, 0, sizeof(Vertex),
                            reinterpret_cast<const void*>(6 * sizeof(float)));
    gl::EnableVertexAttribArray(3);
    gl::VertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, 0, sizeof(Vertex),
                            reinterpret_cast<const void*>(offsetof(Vertex, bones)));
    gl::EnableVertexAttribArray(4);
    gl::VertexAttribPointer(4, 4, gl::FLOAT, 0, sizeof(Vertex),
                            reinterpret_cast<const void*>(offsetof(Vertex, weights)));
}

}  // namespace

struct Renderer::Impl {
    SDL_Window* window = nullptr;
    SDL_GLContext context = nullptr;
    unsigned program = 0;
    unsigned particle_program = 0;
    unsigned sprite_texture = 0;
    unsigned bone_ubo = 0;
    std::string version;

    // Static geometry uploaded once (map pieces, props).
    struct CachedMesh {
        unsigned vao = 0;
        unsigned vbo = 0;
        unsigned ebo = 0;
        std::uint32_t index_count = 0;
        std::vector<DrawRange> ranges;
    };
    std::vector<CachedMesh> meshes;
    std::unordered_map<const Skn*, int> mesh_ids;
    std::vector<unsigned> textures;
    std::unordered_map<const TexImage*, int> texture_ids;

    ~Impl() {
        if (bone_ubo != 0 && gl::DeleteBuffers != nullptr) gl::DeleteBuffers(1, &bone_ubo);
        if (sprite_texture != 0) glDeleteTextures(1, &sprite_texture);
        if (particle_program != 0 && gl::DeleteProgram != nullptr) {
            gl::DeleteProgram(particle_program);
        }
        if (program != 0 && gl::DeleteProgram != nullptr) gl::DeleteProgram(program);
        if (context != nullptr) SDL_GL_DestroyContext(context);
        if (window != nullptr) SDL_DestroyWindow(window);
        SDL_Quit();
    }

    void init(int width, int height, bool visible) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            throw std::runtime_error("SDL_Init failed: " + last_sdl_error());
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

        SDL_WindowFlags flags = SDL_WINDOW_OPENGL;
        if (visible) {
            flags |= SDL_WINDOW_RESIZABLE;
        } else {
            flags |= SDL_WINDOW_HIDDEN;
        }
        window = SDL_CreateWindow("lolview", width, height, flags);
        if (window == nullptr) {
            throw std::runtime_error("SDL_CreateWindow failed: " + last_sdl_error());
        }
        context = SDL_GL_CreateContext(window);
        if (context == nullptr) {
            throw std::runtime_error("SDL_GL_CreateContext failed: " + last_sdl_error());
        }
        if (!SDL_GL_MakeCurrent(window, context)) {
            throw std::runtime_error("SDL_GL_MakeCurrent failed: " + last_sdl_error());
        }

        const auto loader = [](const char* name) -> gl::Proc {
            return reinterpret_cast<gl::Proc>(SDL_GL_GetProcAddress(name));
        };
        if (!gl::load(loader)) {
            throw std::runtime_error(
                "OpenGL 4.6 entry points missing; the driver did not provide a core context");
        }

        const auto* version_string = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        version = version_string != nullptr ? version_string : "unknown";

        program = link_program(kVertexShader, kFragmentShader);

        const unsigned block = gl::GetUniformBlockIndex(program, "BoneBlock");
        gl::UniformBlockBinding(program, block, 0);
        gl::GenBuffers(1, &bone_ubo);
        gl::BindBuffer(gl::UNIFORM_BUFFER, bone_ubo);
        gl::BufferData(gl::UNIFORM_BUFFER, static_cast<long long>(256 * 16 * sizeof(float)),
                       nullptr, gl::DYNAMIC_DRAW);
        gl::BindBufferBase(gl::UNIFORM_BUFFER, 0, bone_ubo);

        particle_program = link_program(kParticleVertexShader, kParticleFragmentShader);

        // Soft radial sprite, generated so VFX need no game textures.
        constexpr int kSpriteSize = 64;
        std::vector<std::uint8_t> sprite(static_cast<std::size_t>(kSpriteSize) * kSpriteSize * 4);
        for (int y = 0; y < kSpriteSize; ++y) {
            for (int x = 0; x < kSpriteSize; ++x) {
                const float dx = (x + 0.5f) / kSpriteSize * 2.0f - 1.0f;
                const float dy = (y + 0.5f) / kSpriteSize * 2.0f - 1.0f;
                const float r = std::sqrt(dx * dx + dy * dy);
                const float alpha = std::max(0.0f, 1.0f - r);
                const float glow = std::pow(alpha, 1.15f) * 1.35f;
                const std::size_t index = (static_cast<std::size_t>(y) * kSpriteSize + x) * 4;
                sprite[index + 0] = 255;
                sprite[index + 1] = 255;
                sprite[index + 2] = 255;
                sprite[index + 3] =
                    static_cast<std::uint8_t>(std::clamp(glow * 255.0f, 0.0f, 255.0f));
            }
        }
        glGenTextures(1, &sprite_texture);
        glBindTexture(GL_TEXTURE_2D, sprite_texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(gl::RGBA8), kSpriteSize, kSpriteSize, 0,
                     static_cast<GLenum>(gl::RGBA), static_cast<GLenum>(gl::UNSIGNED_BYTE),
                     sprite.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(gl::LINEAR));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(gl::LINEAR));
    }
};

Renderer::Renderer(int width, int height, bool visible) : impl_(new Impl()) {
    impl_->init(width, height, visible);
}

Renderer::~Renderer() { delete impl_; }

SDL_Window* Renderer::window() const noexcept { return impl_->window; }
void* Renderer::gl_context() const noexcept { return impl_->context; }
const std::string& Renderer::gl_version() const noexcept { return impl_->version; }

void save_png(const std::filesystem::path& path, int width, int height,
              const std::uint8_t* rgba) {
    if (stbi_write_png(path.string().c_str(), width, height, 4, rgba, width * 4) == 0) {
        throw std::runtime_error("cannot write PNG: " + path.string());
    }
}

void Renderer::begin_frame(int width, int height) {
    gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glClearColor(0.07f, 0.08f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::end_frame() { SDL_GL_SwapWindow(impl_->window); }

void Renderer::capture_default(const std::filesystem::path& png_path, int width, int height) {
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    std::vector<unsigned char> flipped(pixels.size());
    for (int row = 0; row < height; ++row) {
        std::copy_n(pixels.data() + static_cast<std::size_t>(height - 1 - row) * width * 4,
                    static_cast<std::size_t>(width) * 4,
                    flipped.data() + static_cast<std::size_t>(row) * width * 4);
    }
    save_png(png_path, width, height, flipped.data());
}

void Renderer::draw_scene(const Skn& mesh, const TexImage* texture,
                          const std::vector<Mat4>* skin_matrices,
                          const std::vector<Particle>* particles, const Mat4& model,
                          const Mat4& view, const Mat4& proj, int width, int height) {
    // ---- geometry ----
    std::vector<Vertex> vertices;
    vertices.reserve(mesh.vertices().size());
    for (const auto& v : mesh.vertices()) {
        Vertex vertex{};
        vertex.px = v.position[0];
        vertex.py = v.position[1];
        vertex.pz = v.position[2];
        vertex.nx = v.normal[0];
        vertex.ny = v.normal[1];
        vertex.nz = v.normal[2];
        vertex.u = v.uv[0];
        vertex.v = v.uv[1];
        for (std::size_t i = 0; i < 4; ++i) {
            vertex.bones[i] = v.bone_indices[i];
            vertex.weights[i] = v.weights[i];
        }
        vertices.push_back(vertex);
    }

    std::vector<std::uint32_t> indices;
    indices.reserve(mesh.indices().size());
    for (std::uint16_t index : mesh.indices()) indices.push_back(index);

    static const Vec3 palette[] = {
        {0.78f, 0.62f, 0.86f}, {0.55f, 0.72f, 0.95f}, {0.95f, 0.72f, 0.55f},
        {0.60f, 0.88f, 0.70f}, {0.90f, 0.55f, 0.60f}, {0.70f, 0.70f, 0.70f},
    };
    std::vector<DrawRange> ranges;
    std::size_t palette_index = 0;
    for (const auto& material : mesh.materials()) {
        if (material.num_indices == 0) continue;
        const std::uint32_t start = material.start_index;
        const std::uint32_t available =
            static_cast<std::uint32_t>(indices.size() > start ? indices.size() - start : 0);
        if (available == 0) continue;
        DrawRange range;
        range.start_index = start;
        range.count = std::min(material.num_indices, available);
        range.color = palette[palette_index++ % (sizeof(palette) / sizeof(palette[0]))];
        ranges.push_back(range);
    }
    if (ranges.empty()) {
        ranges.push_back(DrawRange{0, static_cast<std::uint32_t>(indices.size()), palette[0]});
    }
    // A textured mesh keeps its own colours; the palette is only a stand-in.
    if (texture != nullptr && texture->width > 0 && !texture->rgba.empty()) {
        for (auto& range : ranges) range.color = Vec3{1.0f, 1.0f, 1.0f};
    }

    unsigned vao = 0;
    unsigned vbo = 0;
    unsigned ebo = 0;
    gl::GenVertexArrays(1, &vao);
    gl::BindVertexArray(vao);
    gl::GenBuffers(1, &vbo);
    gl::BindBuffer(gl::ARRAY_BUFFER, vbo);
    gl::BufferData(gl::ARRAY_BUFFER,
                   static_cast<long long>(vertices.size() * sizeof(Vertex)), vertices.data(),
                   gl::STATIC_DRAW);
    gl::GenBuffers(1, &ebo);
    gl::BindBuffer(gl::ELEMENT_ARRAY_BUFFER, ebo);
    gl::BufferData(gl::ELEMENT_ARRAY_BUFFER,
                   static_cast<long long>(indices.size() * sizeof(std::uint32_t)),
                   indices.data(), gl::STATIC_DRAW);
    vertex_input_layout();

    glViewport(0, 0, width, height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);

    const Mat4 mvp = proj * view * model;

    gl::UseProgram(impl_->program);
    const int loc_mvp = gl::GetUniformLocation(impl_->program, "u_mvp");
    const int loc_color = gl::GetUniformLocation(impl_->program, "u_color");
    const int loc_use_texture = gl::GetUniformLocation(impl_->program, "u_use_texture");
    const int loc_alpha_mode = gl::GetUniformLocation(impl_->program, "u_alpha_mode");
    gl::Uniform1i(loc_alpha_mode, alpha_mode_);
    if (alpha_mode_ == 2) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
    } else {
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
    }
    gl::UniformMatrix4fv(loc_mvp, 1, 0, mvp.data());
    gl::Uniform1i(loc_use_texture, 0);

    const int loc_skinned = gl::GetUniformLocation(impl_->program, "u_skinned");
    gl::Uniform1i(loc_skinned, 0);
    if (skin_matrices != nullptr && !skin_matrices->empty()) {
        const std::size_t bone_count = std::min<std::size_t>(skin_matrices->size(), 256);
        std::vector<float> flat(bone_count * 16);
        for (std::size_t i = 0; i < bone_count; ++i) {
            std::memcpy(flat.data() + i * 16, skin_matrices->at(i).data(), 16 * sizeof(float));
        }
        gl::BindBuffer(gl::UNIFORM_BUFFER, impl_->bone_ubo);
        gl::BufferSubData(gl::UNIFORM_BUFFER, 0,
                          static_cast<long long>(flat.size() * sizeof(float)), flat.data());
        gl::BindBufferBase(gl::UNIFORM_BUFFER, 0, impl_->bone_ubo);
        gl::Uniform1i(loc_skinned, 1);
    }

    unsigned texture_name = 0;
    if (texture != nullptr && texture->width > 0 && texture->height > 0 &&
        !texture->rgba.empty()) {
        glGenTextures(1, &texture_name);
        gl::ActiveTexture(gl::TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture_name);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(gl::RGBA8), texture->width,
                     texture->height, 0, static_cast<GLenum>(gl::RGBA),
                     static_cast<GLenum>(gl::UNSIGNED_BYTE), texture->rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(gl::LINEAR));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(gl::LINEAR));

        gl::Uniform1i(gl::GetUniformLocation(impl_->program, "u_texture"), 0);
        gl::Uniform1i(loc_use_texture, 1);
    }

    gl::BindVertexArray(vao);
    for (const auto& range : ranges) {
        gl::Uniform3f(loc_color, range.color.x, range.color.y, range.color.z);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(range.count), GL_UNSIGNED_INT,
                       reinterpret_cast<const void*>(
                           static_cast<std::uintptr_t>(range.start_index) * sizeof(std::uint32_t)));
    }

    if (particles != nullptr && !particles->empty()) {
        draw_particles(*particles, nullptr, view, proj);
    }

    if (texture_name != 0) glDeleteTextures(1, &texture_name);
    if (alpha_mode_ == 2) {
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
    }
    gl::DeleteBuffers(1, &ebo);
    gl::DeleteBuffers(1, &vbo);
    gl::DeleteVertexArrays(1, &vao);
}

void Renderer::draw_particles(const std::vector<Particle>& particles, const TexImage* sprite,
                              const Mat4& view, const Mat4& proj) {
    if (particles.empty()) return;

    std::vector<float> data;
    data.reserve(particles.size() * 8);
    for (const auto& p : particles) {
        data.insert(data.end(), {p.position.x, p.position.y, p.position.z, p.r, p.g, p.b, p.a,
                                 p.size});
    }

    unsigned particle_vao = 0;
    unsigned particle_vbo = 0;
    gl::GenVertexArrays(1, &particle_vao);
    gl::BindVertexArray(particle_vao);
    gl::GenBuffers(1, &particle_vbo);
    gl::BindBuffer(gl::ARRAY_BUFFER, particle_vbo);
    gl::BufferData(gl::ARRAY_BUFFER, static_cast<long long>(data.size() * sizeof(float)),
                   data.data(), gl::DYNAMIC_DRAW);

    const auto stride = static_cast<int>(8 * sizeof(float));
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 3, gl::FLOAT, 0, stride, reinterpret_cast<const void*>(0));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(1, 4, gl::FLOAT, 0, stride,
                            reinterpret_cast<const void*>(3 * sizeof(float)));
    gl::EnableVertexAttribArray(2);
    gl::VertexAttribPointer(2, 1, gl::FLOAT, 0, stride,
                            reinterpret_cast<const void*>(7 * sizeof(float)));

    const Mat4 viewproj = proj * view;
    gl::UseProgram(impl_->particle_program);
    const int loc_viewproj = gl::GetUniformLocation(impl_->particle_program, "u_viewproj");
    gl::UniformMatrix4fv(loc_viewproj, 1, 0, viewproj.data());
    gl::ActiveTexture(gl::TEXTURE0);

    unsigned sprite_texture = 0;
    if (sprite != nullptr && sprite->width > 0 && sprite->height > 0 && !sprite->rgba.empty()) {
        glGenTextures(1, &sprite_texture);
        glBindTexture(GL_TEXTURE_2D, sprite_texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(gl::RGBA8), sprite->width,
                     sprite->height, 0, static_cast<GLenum>(gl::RGBA),
                     static_cast<GLenum>(gl::UNSIGNED_BYTE), sprite->rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(gl::LINEAR));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(gl::LINEAR));
    } else {
        glBindTexture(GL_TEXTURE_2D, impl_->sprite_texture);
    }
    gl::Uniform1i(gl::GetUniformLocation(impl_->particle_program, "u_sprite"), 0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDepthMask(GL_FALSE);
    glEnable(gl::PROGRAM_POINT_SIZE);
    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(particles.size()));
    glDisable(gl::PROGRAM_POINT_SIZE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    if (sprite_texture != 0) glDeleteTextures(1, &sprite_texture);
    gl::DeleteBuffers(1, &particle_vbo);
    gl::DeleteVertexArrays(1, &particle_vao);
}

void Renderer::render_skn(const Skn& mesh, const TexImage* texture,
                          const std::vector<Mat4>* skin_matrices,
                          const std::vector<Particle>* particles, const RenderOptions& options,
                          const std::filesystem::path& png_path) {
    const int width = std::max(16, options.width);
    const int height = std::max(16, options.height);

    const SknBounds bounds = mesh.compute_bounds();
    const Vec3 center{(bounds.min[0] + bounds.max[0]) * 0.5f,
                      (bounds.min[1] + bounds.max[1]) * 0.5f,
                      (bounds.min[2] + bounds.max[2]) * 0.5f};
    const Vec3 extent{bounds.max[0] - bounds.min[0], bounds.max[1] - bounds.min[1],
                      bounds.max[2] - bounds.min[2]};
    const float radius = std::max(1.0f, 0.5f * std::sqrt(dot(extent, extent)));
    const float distance = radius * std::max(1.2f, options.distance_scale);

    const float yaw = options.yaw_degrees * kPi / 180.0f;
    const float pitch = options.pitch_degrees * kPi / 180.0f;
    const Vec3 eye{center.x + distance * std::cos(pitch) * std::sin(yaw),
                   center.y + distance * std::sin(pitch),
                   center.z + distance * std::cos(pitch) * std::cos(yaw)};

    const Mat4 view = look_at(eye, center, Vec3{0.0f, 1.0f, 0.0f});
    const Mat4 projection =
        perspective(45.0f * kPi / 180.0f, static_cast<float>(width) / static_cast<float>(height),
                    std::max(0.05f, distance * 0.01f), distance * 6.0f + radius * 8.0f);

    unsigned fbo = 0;
    unsigned color = 0;
    unsigned depth = 0;
    gl::GenFramebuffers(1, &fbo);
    gl::BindFramebuffer(gl::FRAMEBUFFER, fbo);

    glGenTextures(1, &color);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(gl::RGBA8), width, height, 0,
                 static_cast<GLenum>(gl::RGBA), static_cast<GLenum>(gl::UNSIGNED_BYTE), nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(gl::LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(gl::LINEAR));
    gl::FramebufferTexture2D(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::TEXTURE_2D, color, 0);

    gl::GenRenderbuffers(1, &depth);
    gl::BindRenderbuffer(gl::RENDERBUFFER, depth);
    gl::RenderbufferStorage(gl::RENDERBUFFER, gl::DEPTH_COMPONENT24, width, height);
    gl::FramebufferRenderbuffer(gl::FRAMEBUFFER, gl::DEPTH_ATTACHMENT, gl::RENDERBUFFER, depth);

    if (gl::CheckFramebufferStatus(gl::FRAMEBUFFER) != gl::FRAMEBUFFER_COMPLETE) {
        gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
        throw std::runtime_error("framebuffer is not complete");
    }

    glClearColor(options.background.x, options.background.y, options.background.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    draw_scene(mesh, texture, skin_matrices, particles, Mat4::identity(), view, projection, width,
               height);

    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    std::vector<unsigned char> flipped(pixels.size());
    for (int row = 0; row < height; ++row) {
        std::copy_n(pixels.data() + static_cast<std::size_t>(height - 1 - row) * width * 4,
                    static_cast<std::size_t>(width) * 4,
                    flipped.data() + static_cast<std::size_t>(row) * width * 4);
    }
    save_png(png_path, width, height, flipped.data());

    gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
    gl::DeleteFramebuffers(1, &fbo);
    gl::DeleteRenderbuffers(1, &depth);
    glDeleteTextures(1, &color);
}

int Renderer::cache_mesh(const Skn& mesh) {
    const auto existing = impl_->mesh_ids.find(&mesh);
    if (existing != impl_->mesh_ids.end()) return existing->second;

    std::vector<Vertex> vertices;
    vertices.reserve(mesh.vertices().size());
    for (const auto& v : mesh.vertices()) {
        Vertex vertex{};
        vertex.px = v.position[0];
        vertex.py = v.position[1];
        vertex.pz = v.position[2];
        vertex.nx = v.normal[0];
        vertex.ny = v.normal[1];
        vertex.nz = v.normal[2];
        vertex.u = v.uv[0];
        vertex.v = v.uv[1];
        for (std::size_t i = 0; i < 4; ++i) {
            vertex.bones[i] = v.bone_indices[i];
            vertex.weights[i] = v.weights[i];
        }
        vertices.push_back(vertex);
    }

    std::vector<std::uint32_t> indices;
    indices.reserve(mesh.indices().size());
    for (std::uint16_t index : mesh.indices()) indices.push_back(index);

    std::vector<DrawRange> ranges;
    static const Vec3 kPalette[] = {
        {0.62f, 0.62f, 0.58f}, {0.55f, 0.58f, 0.52f}, {0.66f, 0.60f, 0.52f},
        {0.52f, 0.56f, 0.60f}, {0.60f, 0.55f, 0.62f}, {0.58f, 0.62f, 0.55f},
    };
    std::size_t material_index = 0;
    for (const auto& material : mesh.materials()) {
        if (material.num_indices == 0) continue;
        // Stable per-material tint for meshes that resolve no texture.
        const std::size_t hash = std::hash<std::string>{}(material.name);
        const Vec3 color = kPalette[(hash + material_index) % (sizeof(kPalette) / sizeof(kPalette[0]))];
        ++material_index;
        ranges.push_back(DrawRange{material.start_index, material.num_indices, color});
    }
    if (ranges.empty() && !indices.empty()) {
        ranges.push_back(DrawRange{0, static_cast<std::uint32_t>(indices.size()),
                                   Vec3{0.6f, 0.6f, 0.58f}});
    }

    Impl::CachedMesh cached;
    gl::GenVertexArrays(1, &cached.vao);
    gl::BindVertexArray(cached.vao);
    gl::GenBuffers(1, &cached.vbo);
    gl::BindBuffer(gl::ARRAY_BUFFER, cached.vbo);
    gl::BufferData(gl::ARRAY_BUFFER,
                   static_cast<long long>(vertices.size() * sizeof(Vertex)), vertices.data(),
                   gl::STATIC_DRAW);
    gl::GenBuffers(1, &cached.ebo);
    gl::BindBuffer(gl::ELEMENT_ARRAY_BUFFER, cached.ebo);
    gl::BufferData(gl::ELEMENT_ARRAY_BUFFER,
                   static_cast<long long>(indices.size() * sizeof(std::uint32_t)),
                   indices.data(), gl::STATIC_DRAW);
    vertex_input_layout();
    cached.index_count = static_cast<std::uint32_t>(indices.size());
    cached.ranges = std::move(ranges);

    impl_->meshes.push_back(std::move(cached));
    const int id = static_cast<int>(impl_->meshes.size()) - 1;
    impl_->mesh_ids[&mesh] = id;
    return id;
}

int Renderer::cache_texture(const TexImage& image) {
    const auto existing = impl_->texture_ids.find(&image);
    if (existing != impl_->texture_ids.end()) return existing->second;
    if (image.width <= 0 || image.height <= 0 || image.rgba.empty()) return -1;

    unsigned name = 0;
    glGenTextures(1, &name);
    glBindTexture(GL_TEXTURE_2D, name);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(gl::RGBA8), image.width, image.height, 0,
                 static_cast<GLenum>(gl::RGBA), static_cast<GLenum>(gl::UNSIGNED_BYTE),
                 image.rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(gl::LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(gl::LINEAR));

    impl_->textures.push_back(name);
    const int id = static_cast<int>(impl_->textures.size()) - 1;
    impl_->texture_ids[&image] = id;
    return id;
}

void Renderer::draw_cached(int mesh_id, int texture_id, const Mat4& model, const Mat4& view,
                           const Mat4& proj, int width, int height, int alpha_mode) {
    if (mesh_id < 0 || mesh_id >= static_cast<int>(impl_->meshes.size())) return;
    const auto& cached = impl_->meshes[static_cast<std::size_t>(mesh_id)];

    glViewport(0, 0, width, height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);

    const Mat4 mvp = proj * view * model;
    gl::UseProgram(impl_->program);
    gl::UniformMatrix4fv(gl::GetUniformLocation(impl_->program, "u_mvp"), 1, 0, mvp.data());
    gl::Uniform1i(gl::GetUniformLocation(impl_->program, "u_skinned"), 0);
    gl::Uniform1i(gl::GetUniformLocation(impl_->program, "u_alpha_mode"), alpha_mode);

    const bool textured = texture_id >= 0 &&
                          texture_id < static_cast<int>(impl_->textures.size());
    gl::Uniform1i(gl::GetUniformLocation(impl_->program, "u_use_texture"), textured ? 1 : 0);
    if (textured) {
        gl::ActiveTexture(gl::TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, impl_->textures[static_cast<std::size_t>(texture_id)]);
        gl::Uniform1i(gl::GetUniformLocation(impl_->program, "u_texture"), 0);
    }

    if (alpha_mode == 2) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
    } else {
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
    }

    gl::BindVertexArray(cached.vao);
    const int loc_color = gl::GetUniformLocation(impl_->program, "u_color");
    for (const auto& range : cached.ranges) {
        const Vec3 color = textured ? Vec3{1.0f, 1.0f, 1.0f} : range.color;
        gl::Uniform3f(loc_color, color.x, color.y, color.z);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(range.count), GL_UNSIGNED_INT,
                       reinterpret_cast<const void*>(static_cast<std::uintptr_t>(
                           range.start_index) * sizeof(std::uint32_t)));
    }

    if (alpha_mode == 2) {
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
    }
}

}  // namespace lolview
