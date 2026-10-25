#pragma once

// Minimal OpenGL 3.3+/4.6 core loader.
//
// Everything lives in namespace `gl` with plain C types in the signatures, so
// it never collides with the GL 1.1 declarations that platform headers provide.
// Function pointers are resolved through SDL_GL_GetProcAddress by the caller.

namespace lolview::gl {

using Proc = void (*)();
using Loader = Proc (*)(const char* name);

// Context / buffer targets
inline constexpr unsigned ARRAY_BUFFER = 0x8892;
inline constexpr unsigned ELEMENT_ARRAY_BUFFER = 0x8893;
inline constexpr unsigned STATIC_DRAW = 0x88E4;
inline constexpr unsigned FLOAT = 0x1406;
inline constexpr unsigned UNSIGNED_INT = 0x1405;
inline constexpr unsigned UNSIGNED_BYTE = 0x1401;
inline constexpr unsigned TRIANGLES = 0x0004;

// Shaders
inline constexpr unsigned VERTEX_SHADER = 0x8B31;
inline constexpr unsigned FRAGMENT_SHADER = 0x8B30;
inline constexpr unsigned COMPILE_STATUS = 0x8B81;
inline constexpr unsigned LINK_STATUS = 0x8B82;

// Textures
inline constexpr unsigned TEXTURE_2D = 0x0DE1;
inline constexpr unsigned TEXTURE0 = 0x84C0;
inline constexpr unsigned RGBA = 0x1908;
inline constexpr unsigned RGBA8 = 0x8058;
inline constexpr unsigned TEXTURE_MIN_FILTER = 0x2801;
inline constexpr unsigned TEXTURE_MAG_FILTER = 0x2800;
inline constexpr unsigned LINEAR = 0x2601;
inline constexpr unsigned PACK_ALIGNMENT = 0x0D05;
inline constexpr unsigned UNPACK_ALIGNMENT = 0x0CF5;

// Uniform buffers
inline constexpr unsigned UNIFORM_BUFFER = 0x8A11;
inline constexpr unsigned DYNAMIC_DRAW = 0x88E8;
inline constexpr unsigned PROGRAM_POINT_SIZE = 0x8642;

// Framebuffers
inline constexpr unsigned FRAMEBUFFER = 0x8D40;inline constexpr unsigned COLOR_ATTACHMENT0 = 0x8CE0;
inline constexpr unsigned DEPTH_ATTACHMENT = 0x8D00;
inline constexpr unsigned RENDERBUFFER = 0x8D41;
inline constexpr unsigned DEPTH_COMPONENT24 = 0x81A6;
inline constexpr unsigned FRAMEBUFFER_COMPLETE = 0x8CD5;

extern void (*GenVertexArrays)(int, unsigned*);
extern void (*BindVertexArray)(unsigned);
extern void (*DeleteVertexArrays)(int, const unsigned*);

extern void (*GenBuffers)(int, unsigned*);
extern void (*BindBuffer)(unsigned, unsigned);
extern void (*BufferData)(unsigned, long long, const void*, unsigned);
extern void (*BufferSubData)(unsigned, long long, long long, const void*);
extern void (*BindBufferBase)(unsigned, unsigned, unsigned);
extern void (*DeleteBuffers)(int, const unsigned*);

extern unsigned (*GetUniformBlockIndex)(unsigned, const char*);
extern void (*UniformBlockBinding)(unsigned, unsigned, unsigned);

extern void (*EnableVertexAttribArray)(unsigned);
extern void (*VertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void*);

extern unsigned (*CreateShader)(unsigned);
extern void (*ShaderSource)(unsigned, int, const char* const*, const int*);
extern void (*CompileShader)(unsigned);
extern void (*GetShaderiv)(unsigned, unsigned, int*);
extern void (*GetShaderInfoLog)(unsigned, int, int*, char*);
extern void (*DeleteShader)(unsigned);

extern unsigned (*CreateProgram)();
extern void (*AttachShader)(unsigned, unsigned);
extern void (*LinkProgram)(unsigned);
extern void (*GetProgramiv)(unsigned, unsigned, int*);
extern void (*GetProgramInfoLog)(unsigned, int, int*, char*);
extern void (*UseProgram)(unsigned);
extern void (*DeleteProgram)(unsigned);

extern int (*GetUniformLocation)(unsigned, const char*);
extern void (*UniformMatrix4fv)(int, int, unsigned char, const float*);
extern void (*Uniform1i)(int, int);
extern void (*Uniform1f)(int, float);
extern void (*Uniform3f)(int, float, float, float);

extern void (*GenFramebuffers)(int, unsigned*);
extern void (*BindFramebuffer)(unsigned, unsigned);
extern void (*FramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
extern unsigned (*CheckFramebufferStatus)(unsigned);
extern void (*DeleteFramebuffers)(int, const unsigned*);

extern void (*GenRenderbuffers)(int, unsigned*);
extern void (*BindRenderbuffer)(unsigned, unsigned);
extern void (*RenderbufferStorage)(unsigned, unsigned, int, int);
extern void (*FramebufferRenderbuffer)(unsigned, unsigned, unsigned, unsigned);
extern void (*DeleteRenderbuffers)(int, const unsigned*);

extern void (*ActiveTexture)(unsigned);
extern void (*GenerateMipmap)(unsigned);

// Resolves every pointer above. Returns false if any entry point is missing.
bool load(Loader loader);

}  // namespace lolview::gl
