#include <lolview/renderer.hpp>

#include <lolview/gl.hpp>
#include <lolview/tex.hpp>

#include <SDL3/SDL_opengl.h>

namespace lolview {

unsigned Renderer::upload_texture(const TexImage& image) {
    if (image.width <= 0 || image.height <= 0 || image.rgba.empty()) return 0;

    unsigned texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(gl::RGBA8), image.width, image.height, 0,
                 static_cast<GLenum>(gl::RGBA), static_cast<GLenum>(gl::UNSIGNED_BYTE),
                 image.rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(gl::LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(gl::LINEAR));
    return texture;
}

}  // namespace lolview
