#include <lolview/gl.hpp>

namespace lolview::gl {
namespace {

Loader g_loader = nullptr;

template <typename Fn>
bool bind(Fn& slot, const char* name) {
    slot = reinterpret_cast<Fn>(g_loader(name));
    return slot != nullptr;
}

}  // namespace

void (*GenVertexArrays)(int, unsigned*) = nullptr;
void (*BindVertexArray)(unsigned) = nullptr;
void (*DeleteVertexArrays)(int, const unsigned*) = nullptr;

void (*GenBuffers)(int, unsigned*) = nullptr;
void (*BindBuffer)(unsigned, unsigned) = nullptr;
void (*BufferData)(unsigned, long long, const void*, unsigned) = nullptr;
void (*BufferSubData)(unsigned, long long, long long, const void*) = nullptr;
void (*BindBufferBase)(unsigned, unsigned, unsigned) = nullptr;
void (*DeleteBuffers)(int, const unsigned*) = nullptr;

unsigned (*GetUniformBlockIndex)(unsigned, const char*) = nullptr;
void (*UniformBlockBinding)(unsigned, unsigned, unsigned) = nullptr;

void (*EnableVertexAttribArray)(unsigned) = nullptr;
void (*VertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void*) = nullptr;

unsigned (*CreateShader)(unsigned) = nullptr;
void (*ShaderSource)(unsigned, int, const char* const*, const int*) = nullptr;
void (*CompileShader)(unsigned) = nullptr;
void (*GetShaderiv)(unsigned, unsigned, int*) = nullptr;
void (*GetShaderInfoLog)(unsigned, int, int*, char*) = nullptr;
void (*DeleteShader)(unsigned) = nullptr;

unsigned (*CreateProgram)() = nullptr;
void (*AttachShader)(unsigned, unsigned) = nullptr;
void (*LinkProgram)(unsigned) = nullptr;
void (*GetProgramiv)(unsigned, unsigned, int*) = nullptr;
void (*GetProgramInfoLog)(unsigned, int, int*, char*) = nullptr;
void (*UseProgram)(unsigned) = nullptr;
void (*DeleteProgram)(unsigned) = nullptr;

int (*GetUniformLocation)(unsigned, const char*) = nullptr;
void (*UniformMatrix4fv)(int, int, unsigned char, const float*) = nullptr;
void (*Uniform1i)(int, int) = nullptr;
void (*Uniform1f)(int, float) = nullptr;
void (*Uniform3f)(int, float, float, float) = nullptr;

void (*GenFramebuffers)(int, unsigned*) = nullptr;
void (*BindFramebuffer)(unsigned, unsigned) = nullptr;
void (*FramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int) = nullptr;
unsigned (*CheckFramebufferStatus)(unsigned) = nullptr;
void (*DeleteFramebuffers)(int, const unsigned*) = nullptr;

void (*GenRenderbuffers)(int, unsigned*) = nullptr;
void (*BindRenderbuffer)(unsigned, unsigned) = nullptr;
void (*RenderbufferStorage)(unsigned, unsigned, int, int) = nullptr;
void (*FramebufferRenderbuffer)(unsigned, unsigned, unsigned, unsigned) = nullptr;
void (*DeleteRenderbuffers)(int, const unsigned*) = nullptr;

void (*ActiveTexture)(unsigned) = nullptr;
void (*GenerateMipmap)(unsigned) = nullptr;

bool load(Loader loader) {
    if (loader == nullptr) return false;
    g_loader = loader;

    bool ok = true;
    ok &= bind(GenVertexArrays, "glGenVertexArrays");
    ok &= bind(BindVertexArray, "glBindVertexArray");
    ok &= bind(DeleteVertexArrays, "glDeleteVertexArrays");

    ok &= bind(GenBuffers, "glGenBuffers");
    ok &= bind(BindBuffer, "glBindBuffer");
    ok &= bind(BufferData, "glBufferData");
    ok &= bind(BufferSubData, "glBufferSubData");
    ok &= bind(BindBufferBase, "glBindBufferBase");
    ok &= bind(DeleteBuffers, "glDeleteBuffers");

    ok &= bind(GetUniformBlockIndex, "glGetUniformBlockIndex");
    ok &= bind(UniformBlockBinding, "glUniformBlockBinding");

    ok &= bind(EnableVertexAttribArray, "glEnableVertexAttribArray");
    ok &= bind(VertexAttribPointer, "glVertexAttribPointer");

    ok &= bind(CreateShader, "glCreateShader");
    ok &= bind(ShaderSource, "glShaderSource");
    ok &= bind(CompileShader, "glCompileShader");
    ok &= bind(GetShaderiv, "glGetShaderiv");
    ok &= bind(GetShaderInfoLog, "glGetShaderInfoLog");
    ok &= bind(DeleteShader, "glDeleteShader");

    ok &= bind(CreateProgram, "glCreateProgram");
    ok &= bind(AttachShader, "glAttachShader");
    ok &= bind(LinkProgram, "glLinkProgram");
    ok &= bind(GetProgramiv, "glGetProgramiv");
    ok &= bind(GetProgramInfoLog, "glGetProgramInfoLog");
    ok &= bind(UseProgram, "glUseProgram");
    ok &= bind(DeleteProgram, "glDeleteProgram");

    ok &= bind(GetUniformLocation, "glGetUniformLocation");
    ok &= bind(UniformMatrix4fv, "glUniformMatrix4fv");
    ok &= bind(Uniform1i, "glUniform1i");
    ok &= bind(Uniform1f, "glUniform1f");
    ok &= bind(Uniform3f, "glUniform3f");

    ok &= bind(GenFramebuffers, "glGenFramebuffers");
    ok &= bind(BindFramebuffer, "glBindFramebuffer");
    ok &= bind(FramebufferTexture2D, "glFramebufferTexture2D");
    ok &= bind(CheckFramebufferStatus, "glCheckFramebufferStatus");
    ok &= bind(DeleteFramebuffers, "glDeleteFramebuffers");

    ok &= bind(GenRenderbuffers, "glGenRenderbuffers");
    ok &= bind(BindRenderbuffer, "glBindRenderbuffer");
    ok &= bind(RenderbufferStorage, "glRenderbufferStorage");
    ok &= bind(FramebufferRenderbuffer, "glFramebufferRenderbuffer");
    ok &= bind(DeleteRenderbuffers, "glDeleteRenderbuffers");

    ok &= bind(ActiveTexture, "glActiveTexture");
    ok &= bind(GenerateMipmap, "glGenerateMipmap");

    return ok;
}

}  // namespace lolview::gl
