/*
 * mini_webgl_ext.c — WebGL gaps: drawElements, full uniforms, texImage2D.
 *
 * These close the holes that stop full Three.js from rendering Meshes with
 * indexed geometry and textured materials. Everything routes through the
 * MiniWGL pointer table resolved once at startup (glfwGetProcAddress), so
 * there is no per-frame symbol lookup.
 *
 * stb_image is the ONLY added dependency (~50 KB, public-domain, single
 * header). It decodes PNG/JPEG/BMP/PSD/TGA to RGBA; we hand that pointer
 * straight to glTexImage2D (zero-copy) then free it.
 */
#include "mini_webgl_ext.h"

/* stb_image is provided as a single header by the build (see CMake:
   add the stb include dir). We only pull the declarations here; the
   implementation is compiled once in mini_renderer.c (or a dedicated TU). */
#include "stb_image.h"

#include <string.h>

/* GL constants the JS layer passes through; we forward them verbatim. The
   numeric values are the WebGL/GLenum values, so no <GL/gl.h> needed. */
#define WGL_TRIANGLES 0x0004
#define WGL_UNSIGNED_SHORT 0x1403
#define WGL_UNSIGNED_INT   0x1405
#define WGL_TEXTURE_2D   0x0DE1
#define WGL_RGBA          0x1908
#define WGL_UNSIGNED_BYTE 0x1401

void mini_wgl_draw_elements(MiniWGL *gl, GLenum mode, GLsizei count,
                            GLenum type, GLint offset) {
    /* The element-array buffer (ELEMENT_ARRAY_BUFFER) was bound + filled via
       bindBuffer/bufferData; `offset` is the byte offset into that buffer. */
    if (!gl || !gl->DrawElements) return;
    /* ELEMENT_ARRAY_BUFFER is still bound from the last bindBuffer call, so
       passing (const void*)(intptr_t)offset is correct per the GL spec. */
    gl->DrawElements(mode, count, type, (const void *)(intptr_t)offset);
}

void mini_wgl_uniform_f(MiniWGL *gl, GLint loc, int n, const float *v) {
    if (!gl) return;
    switch (n) {
        case 1: if (gl->Uniform1f) gl->Uniform1f(loc, v[0]); break;
        case 2: if (gl->Uniform2f) gl->Uniform2f(loc, v[0], v[1]); break;
        case 3: if (gl->Uniform3f) gl->Uniform3f(loc, v[0], v[1], v[2]); break;
        case 4: if (gl->Uniform4f) gl->Uniform4f(loc, v[0], v[1], v[2], v[3]); break;
    }
}
void mini_wgl_uniform_i(MiniWGL *gl, GLint loc, int n, const GLint *v) {
    if (!gl) return;
    switch (n) {
        case 1: if (gl->Uniform1i) gl->Uniform1i(loc, v[0]); break;
        case 2: if (gl->Uniform2i) gl->Uniform2i(loc, v[0], v[1]); break;
        case 3: if (gl->Uniform3i) gl->Uniform3i(loc, v[0], v[1], v[2]); break;
        case 4: if (gl->Uniform4i) gl->Uniform4i(loc, v[0], v[1], v[2], v[3]); break;
    }
}
void mini_wgl_uniform_matrix(MiniWGL *gl, GLint loc, int dim,
                             GLboolean transpose, const float *v) {
    if (!gl) return;
    switch (dim) {
        case 2: if (gl->UniformMatrix2fv) gl->UniformMatrix2fv(loc,1,transpose,v); break;
        case 3: if (gl->UniformMatrix3fv) gl->UniformMatrix3fv(loc,1,transpose,v); break;
        case 4: if (gl->UniformMatrix4fv) gl->UniformMatrix4fv(loc,1,transpose,v); break;
    }
}

int mini_wgl_tex_image_from_file(MiniWGL *gl, const char *path,
                                 int *out_w, int *out_h) {
    if (!gl || !gl->TexImage2D || !path) return -1;
    int w=0, h=0, ch=0;
    /* force 4 channels (RGBA); stb_image pads if source has fewer */
    unsigned char *px = stbi_load(path, &w, &h, &ch, 4);
    if (!px) return -2;                         /* decode failed */
    /* zero-copy: hand the decoded buffer pointer straight to the GPU */
    gl->TexImage2D(WGL_TEXTURE_2D, 0, WGL_RGBA, w, h, 0,
                   WGL_RGBA, WGL_UNSIGNED_BYTE, px);
    if (gl->GenerateMipmap) gl->GenerateMipmap(WGL_TEXTURE_2D);
    if (out_w) *out_w = w; if (out_h) *out_h = h;
    stbi_image_free(px);                        /* GPU has copied; free decode buf */
    return 0;
}
