/*
 * mini_webgl_ext.h — WebGL 1.0/2.0 gaps that full Three.js needs.
 *
 * Adds: indexed draw (drawElements), the full uniform family, texture
 * parameters + mipmaps, and texImage2D fed by an stb_image-decoded RGBA
 * buffer (zero-copy: the decoded pixels are handed straight to GL, no
 * intermediate JS ArrayBuffer).
 *
 * The bridge resolves modern GL pointers once via glfwGetProcAddress; here we
 * take a small typed table (MiniWGL) so this file is independent of the
 * bridge's internal struct.
 */
#ifndef MINI_WEBGL_EXT_H
#define MINI_WEBGL_EXT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GL fixed types (mirrors GLenums) */
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;

typedef struct MiniWGL {
    void (*DrawElements)(GLenum mode, GLsizei count, GLenum type, const void *indices);
    void (*Uniform1f)(GLint, float);
    void (*Uniform2f)(GLint, float, float);
    void (*Uniform3f)(GLint, float, float, float);
    void (*Uniform4f)(GLint, float, float, float, float);
    void (*Uniform1i)(GLint, GLint);
    void (*Uniform2i)(GLint, GLint, GLint);
    void (*Uniform3i)(GLint, GLint, GLint, GLint);
    void (*Uniform4i)(GLint, GLint, GLint, GLint, GLint);
    void (*Uniform1fv)(GLint, GLsizei, const float *);
    void (*Uniform2fv)(GLint, GLsizei, const float *);
    void (*Uniform3fv)(GLint, GLsizei, const float *);
    void (*Uniform4fv)(GLint, GLsizei, const float *);
    void (*UniformMatrix2fv)(GLint, GLsizei, GLboolean, const float *);
    void (*UniformMatrix3fv)(GLint, GLsizei, GLboolean, const float *);
    void (*UniformMatrix4fv)(GLint, GLsizei, GLboolean, const float *);
    void (*TexParameteri)(GLenum target, GLenum pname, GLint param);
    void (*GenerateMipmap)(GLenum target);
    void (*ActiveTexture)(GLenum texture);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*GetUniformLocation)(GLuint, const char *);
    /* element-array buffer for indexed draw */
    void (*GenBuffers)(GLsizei, GLuint *);
    void (*BindBuffer)(GLenum, GLuint);
    void (*BufferData)(GLenum, GLsizei, const void *, GLenum);
} MiniWGL;

/* ---- indexed draw: gl.drawElements(mode, count, type, offset) ---- */
void mini_wgl_draw_elements(MiniWGL *gl, GLenum mode, GLsizei count,
                            GLenum type, GLint offset);

/* ---- uniform setters (typed entrypoints bound from JS) ---- */
void mini_wgl_uniform_f(MiniWGL *gl, GLint loc, int n, const float *v);
void mini_wgl_uniform_i(MiniWGL *gl, GLint loc, int n, const GLint *v);
void mini_wgl_uniform_matrix(MiniWGL *gl, GLint loc, int dim,
                             GLboolean transpose, const float *v);

/*
 * Zero-copy texture upload from a decoded image file.
 * Decodes `path` with stb_image to RGBA (forcing 4 channels), uploads the
 * pixel pointer directly to GL via TexImage2D, then frees the decode buffer.
 * Sets *out_w/*out_h. Returns 0 on success.
 * (path may be a local file or a http(s) URL already downloaded by mini_net.)
 */
int mini_wgl_tex_image_from_file(MiniWGL *gl, const char *path,
                                 int *out_w, int *out_h);
int mini_wgl_tex_image_from_memory(MiniWGL *gl, const unsigned char *buf, int len,
                                   int *out_w, int *out_h);

#ifdef __cplusplus
}
#endif
#endif /* MINI_WEBGL_EXT_H */
