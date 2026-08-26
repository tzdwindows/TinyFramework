/*
 * mini_js_bridge.c — QuickJS <-> self-written renderer/DOM binding.
 *
 * This is the polyfill layer. It exposes a standards-shaped surface so
 * Vue/React (DOM) and Three.js (WebGL) believe they run in a browser,
 * while every call is mapped to the C renderer or GL ES below.
 *
 * Technique: native C functions (JS_NewCFunction) provide the *primitive*
 * operations; a tiny JS shim installed on top wires the standard
 * *property descriptors* (textContent / style / querySelector) onto those
 * primitives using Object.defineProperty + Proxy. Splitting it this way
 * keeps the C side small and lets the browser-shaped semantics live in JS
 * where they're trivially expressible.
 *
 * WebGL is resolved through glfwGetProcAddress so the bridge links on
 * Windows (opengl32 only exports GL 1.1 statically) without glad/glew.
 */
#include "mini_dom.h"
#include "mini_renderer.h"
#include "mini_js_bridge.h"
#include "mini_events.h"
#include "mini_net.h"
#include "mini_log.h"
#include "mini_cookies.h"
#include "mini_httpcache.h"
#include "mini_policy.h"
#include "mini_websocket.h"
#include "mini_webgl_ext.h"
#include "mini_audio.h"
#include "stb_image.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <quickjs.h>
#include <GLFW/glfw3.h>
#include <GL/gl.h>

/* mingw's <GL/gl.h> is the legacy 1.1 header — define the GL 1.5+ types the
   WebGL bridge references (GLchar/GLsizeiptr/GLintptr). */
#ifndef GLchar
typedef char GLchar;
#endif
#ifndef GLsizeiptr
typedef ptrdiff_t GLsizeiptr;
#endif
#ifndef GLintptr
typedef ptrdiff_t GLintptr;
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <sys/stat.h>

static char *mc_strdup(const char *s);
static int mini_blob_lookup(const char *url, const char **out_path, const uint8_t **out_data, size_t *out_size, const char **out_type);

/* ================================================================== */
/* GL function-pointer types (self-declared; x64 has one calling      */
/* convention so plain pointers are correct there; x86 needs GLAPI).   */
/* ================================================================== */
typedef GLuint (*PFN_m_CreateShader)(GLenum);
typedef void (*PFN_m_ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void (*PFN_m_CompileShader)(GLuint);
typedef GLuint (*PFN_m_CreateProgram)(void);
typedef void (*PFN_m_AttachShader)(GLuint, GLuint);
typedef void (*PFN_m_LinkProgram)(GLuint);
typedef void (*PFN_m_UseProgram)(GLuint);
typedef void (*PFN_m_GenBuffers)(GLsizei, GLuint *);
typedef void (*PFN_m_BindBuffer)(GLenum, GLuint);
typedef void (*PFN_m_BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void (*PFN_m_EnableVA)(GLuint);
typedef void (*PFN_m_VertexAttribP)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void (*PFN_m_DrawArrays)(GLenum, GLint, GLsizei);
typedef void (*PFN_m_Viewport)(GLint, GLint, GLsizei, GLsizei);
typedef GLint (*PFN_m_GetUniformLocation)(GLuint, const GLchar *);
typedef void (*PFN_m_UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat *);
typedef void (*PFN_m_GenTextures)(GLsizei, GLuint *);
typedef void (*PFN_m_BindTexture)(GLenum, GLuint);
typedef void (*PFN_m_TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (*PFN_m_ActiveTexture)(GLenum);
typedef void (*PFN_m_GetShaderiv)(GLuint, GLenum, GLint *);
typedef void (*PFN_m_GetProgramiv)(GLuint, GLenum, GLint *);
typedef void (*PFN_m_GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
typedef void (*PFN_m_GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, char *);
typedef void (*PFN_m_Enable)(GLenum);
typedef void (*PFN_m_Scissor)(GLint, GLint, GLsizei, GLsizei);
typedef void (*PFN_m_BindVertexArray)(GLuint);

/* Additional WebGL functions for 3D/Effects/Postprocessing */
typedef void (*PFN_m_GenFramebuffers)(GLsizei, GLuint *);
typedef void (*PFN_m_BindFramebuffer)(GLenum, GLuint);
typedef void (*PFN_m_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (*PFN_m_CheckFramebufferStatus)(GLenum);
typedef void (*PFN_m_DeleteFramebuffers)(GLsizei, const GLuint *);
typedef void (*PFN_m_GenRenderbuffers)(GLsizei, GLuint *);
typedef void (*PFN_m_BindRenderbuffer)(GLenum, GLuint);
typedef void (*PFN_m_RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (*PFN_m_FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
typedef void (*PFN_m_DeleteRenderbuffers)(GLsizei, const GLuint *);
typedef void (*PFN_m_DeleteTextures)(GLsizei, const GLuint *);
typedef void (*PFN_m_DeleteBuffers)(GLsizei, const GLuint *);
typedef void (*PFN_m_DeleteProgram)(GLuint);
typedef void (*PFN_m_DeleteShader)(GLuint);
typedef void (*PFN_m_GenerateMipmap)(GLenum);
typedef void (*PFN_m_TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void (*PFN_m_BlendFunc)(GLenum, GLenum);
typedef void (*PFN_m_BlendEquation)(GLenum);
typedef void (*PFN_m_BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
typedef void (*PFN_m_BlendEquationSeparate)(GLenum, GLenum);
typedef void (*PFN_m_DepthFunc)(GLenum);
typedef void (*PFN_m_DepthMask)(GLboolean);
typedef void (*PFN_m_ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
typedef void (*PFN_m_CullFace)(GLenum);
typedef void (*PFN_m_FrontFace)(GLenum);
typedef void (*PFN_m_PolygonOffset)(GLfloat, GLfloat);
typedef void (*PFN_m_ClearDepth)(GLdouble);
typedef void (*PFN_m_ClearStencil)(GLint);
typedef void (*PFN_m_StencilMask)(GLuint);
typedef void (*PFN_m_StencilFunc)(GLenum, GLint, GLuint);
typedef void (*PFN_m_StencilOp)(GLenum, GLenum, GLenum);
typedef void (*PFN_m_DrawElements)(GLenum, GLsizei, GLenum, const void *);
typedef void (*PFN_m_Uniform1f)(GLint, GLfloat);
typedef void (*PFN_m_Uniform2f)(GLint, GLfloat, GLfloat);
typedef void (*PFN_m_Uniform3f)(GLint, GLfloat, GLfloat, GLfloat);
typedef void (*PFN_m_Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFN_m_Uniform1i)(GLint, GLint);
typedef void (*PFN_m_Uniform2i)(GLint, GLint, GLint);
typedef void (*PFN_m_Uniform3i)(GLint, GLint, GLint, GLint);
typedef void (*PFN_m_Uniform4i)(GLint, GLint, GLint, GLint, GLint);
typedef void (*PFN_m_Uniform1fv)(GLint, GLsizei, const GLfloat *);
typedef void (*PFN_m_Uniform2fv)(GLint, GLsizei, const GLfloat *);
typedef void (*PFN_m_Uniform3fv)(GLint, GLsizei, const GLfloat *);
typedef void (*PFN_m_Uniform4fv)(GLint, GLsizei, const GLfloat *);
typedef void (*PFN_m_Uniform1iv)(GLint, GLsizei, const GLint *);
typedef void (*PFN_m_Uniform2iv)(GLint, GLsizei, const GLint *);
typedef void (*PFN_m_Uniform3iv)(GLint, GLsizei, const GLint *);
typedef void (*PFN_m_Uniform4iv)(GLint, GLsizei, const GLint *);
typedef void (*PFN_m_UniformMatrix2fv)(GLint, GLsizei, GLboolean, const GLfloat *);
typedef void (*PFN_m_UniformMatrix3fv)(GLint, GLsizei, GLboolean, const GLfloat *);

typedef struct MiniGLBridge
{
    PFN_m_CreateShader CreateShader;
    PFN_m_ShaderSource ShaderSource;
    PFN_m_CompileShader CompileShader;
    PFN_m_CreateProgram CreateProgram;
    PFN_m_AttachShader AttachShader;
    PFN_m_LinkProgram LinkProgram;
    PFN_m_UseProgram UseProgram;
    PFN_m_GenBuffers GenBuffers;
    PFN_m_BindBuffer BindBuffer;
    PFN_m_BufferData BufferData;
    PFN_m_EnableVA EnableVA;
    PFN_m_VertexAttribP VertexAttribP;
    PFN_m_DrawArrays DrawArrays;
    PFN_m_Viewport Viewport;
    PFN_m_GetUniformLocation GetUniformLocation;
    PFN_m_UniformMatrix4fv UniformMatrix4fv;
    PFN_m_GenTextures GenTextures;
    PFN_m_BindTexture BindTexture;
    PFN_m_TexImage2D TexImage2D;
    PFN_m_ActiveTexture ActiveTexture;
    PFN_m_GetShaderiv GetShaderiv;
    PFN_m_GetProgramiv GetProgramiv;
    PFN_m_GetShaderInfoLog GetShaderInfoLog;
    PFN_m_GetProgramInfoLog GetProgramInfoLog;
    PFN_m_Enable Enable;
    PFN_m_Scissor Scissor;
    PFN_m_BindVertexArray BindVertexArray;

    PFN_m_GenFramebuffers GenFramebuffers;
    PFN_m_BindFramebuffer BindFramebuffer;
    PFN_m_FramebufferTexture2D FramebufferTexture2D;
    PFN_m_CheckFramebufferStatus CheckFramebufferStatus;
    PFN_m_DeleteFramebuffers DeleteFramebuffers;
    PFN_m_GenRenderbuffers GenRenderbuffers;
    PFN_m_BindRenderbuffer BindRenderbuffer;
    PFN_m_RenderbufferStorage RenderbufferStorage;
    PFN_m_FramebufferRenderbuffer FramebufferRenderbuffer;
    PFN_m_DeleteRenderbuffers DeleteRenderbuffers;
    PFN_m_DeleteTextures DeleteTextures;
    PFN_m_DeleteBuffers DeleteBuffers;
    PFN_m_DeleteProgram DeleteProgram;
    PFN_m_DeleteShader DeleteShader;
    PFN_m_GenerateMipmap GenerateMipmap;
    PFN_m_TexSubImage2D TexSubImage2D;
    PFN_m_BlendFunc BlendFunc;
    PFN_m_BlendEquation BlendEquation;
    PFN_m_BlendFuncSeparate BlendFuncSeparate;
    PFN_m_BlendEquationSeparate BlendEquationSeparate;
    PFN_m_DepthFunc DepthFunc;
    PFN_m_DepthMask DepthMask;
    PFN_m_ColorMask ColorMask;
    PFN_m_CullFace CullFace;
    PFN_m_FrontFace FrontFace;
    PFN_m_PolygonOffset PolygonOffset;
    PFN_m_ClearDepth ClearDepth;
    PFN_m_ClearStencil ClearStencil;
    PFN_m_StencilMask StencilMask;
    PFN_m_StencilFunc StencilFunc;
    PFN_m_StencilOp StencilOp;
    PFN_m_DrawElements DrawElements;
    PFN_m_Uniform1f Uniform1f;
    PFN_m_Uniform2f Uniform2f;
    PFN_m_Uniform3f Uniform3f;
    PFN_m_Uniform4f Uniform4f;
    PFN_m_Uniform1i Uniform1i;
    PFN_m_Uniform2i Uniform2i;
    PFN_m_Uniform3i Uniform3i;
    PFN_m_Uniform4i Uniform4i;
    PFN_m_Uniform1fv Uniform1fv;
    PFN_m_Uniform2fv Uniform2fv;
    PFN_m_Uniform3fv Uniform3fv;
    PFN_m_Uniform4fv Uniform4fv;
    PFN_m_Uniform1iv Uniform1iv;
    PFN_m_Uniform2iv Uniform2iv;
    PFN_m_Uniform3iv Uniform3iv;
    PFN_m_Uniform4iv Uniform4iv;
    PFN_m_UniformMatrix2fv UniformMatrix2fv;
    PFN_m_UniformMatrix3fv UniformMatrix3fv;

    /* Track the WebGL program the JS bound so we can restore it before rAF.
       The DOM 2D render runs between bridge_reset_2d and rAF; UseProgram(0)
       is needed for legacy glBegin/glEnd but must be undone before rAF so
       drawArrays uses the shader the JS already set up via useProgram(prog).
       Three.js (and any WebGL app) also CACHES VAO / buffer / texture
       bindings and only re-sets them when its cache thinks they changed.
       Because bridge_reset_2d binds 0 to all of them each frame, that cache
       goes stale: the app skips the re-bind and drawArrays runs with no VAO /
       no ARRAY_BUFFER -> no attributes -> no fragments (only the very first
       frame, before any reset, renders). So we record the last VAO / buffer /
       2D texture the JS bound and restore them too. */
    GLuint current_program;
    GLuint last_vao;
    GLuint last_array_buffer;
    GLuint last_element_array_buffer;
    GLuint last_texture_2d;
    GLuint current_fbo;
    int depth_test_enabled;
    int cull_face_enabled;
    int blend_enabled;
    int scissor_test_enabled;
    /* clearColor Three.js set via gl.clearColor(), saved so the DOM clear
       (which sets its own background each frame) does not contaminate the
       WebGL app's cached clear color. */
    GLfloat clear_r, clear_g, clear_b, clear_a;
    int has_clear;
    int unpack_flip_y;
} MiniGLBridge;

void *mini_gl_bridge_new(void)
{
    MiniGLBridge *b = (MiniGLBridge *)calloc(1, sizeof(*b));
    if (!b)
        return NULL;
#define R(field, name) b->field = (PFN_m_##field)glfwGetProcAddress(name)
    R(CreateShader, "glCreateShader");
    R(ShaderSource, "glShaderSource");
    R(CompileShader, "glCompileShader");
    R(CreateProgram, "glCreateProgram");
    R(AttachShader, "glAttachShader");
    R(LinkProgram, "glLinkProgram");
    R(UseProgram, "glUseProgram");
    R(GenBuffers, "glGenBuffers");
    R(BindBuffer, "glBindBuffer");
    R(BufferData, "glBufferData");
    R(EnableVA, "glEnableVertexAttribArray");
    R(VertexAttribP, "glVertexAttribPointer");
    R(DrawArrays, "glDrawArrays");
    R(Viewport, "glViewport");
    R(GetUniformLocation, "glGetUniformLocation");
    R(UniformMatrix4fv, "glUniformMatrix4fv");
    R(GenTextures, "glGenTextures");
    R(BindTexture, "glBindTexture");
    R(TexImage2D, "glTexImage2D");
    R(ActiveTexture, "glActiveTexture");
    R(GetShaderiv, "glGetShaderiv");
    R(GetProgramiv, "glGetProgramiv");
    R(GetShaderInfoLog, "glGetShaderInfoLog");
    R(GetProgramInfoLog, "glGetProgramInfoLog");
    R(Enable, "glEnable");
    R(Scissor, "glScissor");
    R(BindVertexArray, "glBindVertexArray");

    R(GenFramebuffers, "glGenFramebuffers");
    R(BindFramebuffer, "glBindFramebuffer");
    R(FramebufferTexture2D, "glFramebufferTexture2D");
    R(CheckFramebufferStatus, "glCheckFramebufferStatus");
    R(DeleteFramebuffers, "glDeleteFramebuffers");
    R(GenRenderbuffers, "glGenRenderbuffers");
    R(BindRenderbuffer, "glBindRenderbuffer");
    R(RenderbufferStorage, "glRenderbufferStorage");
    R(FramebufferRenderbuffer, "glFramebufferRenderbuffer");
    R(DeleteRenderbuffers, "glDeleteRenderbuffers");
    R(DeleteTextures, "glDeleteTextures");
    R(DeleteBuffers, "glDeleteBuffers");
    R(DeleteProgram, "glDeleteProgram");
    R(DeleteShader, "glDeleteShader");
    R(GenerateMipmap, "glGenerateMipmap");
    R(TexSubImage2D, "glTexSubImage2D");
    R(BlendFunc, "glBlendFunc");
    R(BlendEquation, "glBlendEquation");
    R(BlendFuncSeparate, "glBlendFuncSeparate");
    R(BlendEquationSeparate, "glBlendEquationSeparate");
    R(DepthFunc, "glDepthFunc");
    R(DepthMask, "glDepthMask");
    R(ColorMask, "glColorMask");
    R(CullFace, "glCullFace");
    R(FrontFace, "glFrontFace");
    R(PolygonOffset, "glPolygonOffset");
    R(ClearDepth, "glClearDepth");
    R(ClearStencil, "glClearStencil");
    R(StencilMask, "glStencilMask");
    R(StencilFunc, "glStencilFunc");
    R(StencilOp, "glStencilOp");
    R(DrawElements, "glDrawElements");
    R(Uniform1f, "glUniform1f");
    R(Uniform2f, "glUniform2f");
    R(Uniform3f, "glUniform3f");
    R(Uniform4f, "glUniform4f");
    R(Uniform1i, "glUniform1i");
    R(Uniform2i, "glUniform2i");
    R(Uniform3i, "glUniform3i");
    R(Uniform4i, "glUniform4i");
    R(Uniform1fv, "glUniform1fv");
    R(Uniform2fv, "glUniform2fv");
    R(Uniform3fv, "glUniform3fv");
    R(Uniform4fv, "glUniform4fv");
    R(Uniform1iv, "glUniform1iv");
    R(Uniform2iv, "glUniform2iv");
    R(Uniform3iv, "glUniform3iv");
    R(Uniform4iv, "glUniform4iv");
    R(UniformMatrix2fv, "glUniformMatrix2fv");
    R(UniformMatrix3fv, "glUniformMatrix3fv");
#undef R
    /* Diagnostic: report any critical GL 2.0+ pointers that failed to resolve. */
    int n_null = 0;
#define C(f)                                             \
    if (!b->f)                                           \
    {                                                    \
        n_null++;                                        \
        fprintf(stderr, "[gl-bridge] MISSING %s\n", #f); \
    }
    C(CreateShader);
    C(ShaderSource);
    C(CompileShader);
    C(CreateProgram);
    C(AttachShader);
    C(LinkProgram);
    C(UseProgram);
    C(GenBuffers);
    C(BindBuffer);
    C(BufferData);
    C(EnableVA);
    C(VertexAttribP);
    C(DrawArrays);
    C(Viewport);
    C(GetUniformLocation);
    C(UniformMatrix4fv);
#undef C
    if (n_null == 0)
        fprintf(stderr, "[gl-bridge] all shader/buffer GL pointers resolved\n");
    return b;
}

void mini_gl_bridge_destroy(void *p) { free(p); }

/* Reset the GL state the WebGL/3D path leaks back to the fixed-function
   2D default. The renderer's DOM draws use legacy glBegin/glEnd immediate
   mode; if a programmable shader program stays bound from the previous
   frame's rAF, those draws render under the stray program (which ignores
   glOrtho/glColor) and the DOM simply vanishes. Restores program 0 and
   drops the buffer/texture bindings the 3D path left set. Legacy 1.1
   toggles (scissor/texture_2d) are reset directly in mini_renderer_begin_frame.
   Called at the top of every frame, before the DOM is drawn. */
void mini_gl_bridge_reset_2d(void *state)
{
    MiniGLBridge *b = (MiniGLBridge *)state;
    if (!b)
        return;

    if (b->UseProgram)
        b->UseProgram(0);
    if (b->BindVertexArray)
        b->BindVertexArray(0);

    if (b->BindBuffer)
    {
        b->BindBuffer(0x8892 /*GL_ARRAY_BUFFER*/, 0);
        b->BindBuffer(0x8893 /*GL_ELEMENT_ARRAY_BUFFER*/, 0);
    }
    if (b->BindTexture)
        b->BindTexture(0x0DE1 /*GL_TEXTURE_2D*/, 0);

    glDisable(0x0B44 /*GL_CULL_FACE*/);
    glDisable(0x0B71 /*GL_DEPTH_TEST*/);
}

/* Restore the WebGL shader program that the JS bound via gl.useProgram().
   Called just BEFORE mini_bridge_fire_raf so that drawArrays in the rAF
   callback runs with the correct shader rather than program 0 (which
   bridge_reset_2d set to unblock the legacy 2D DOM render). */
/* Restore the WebGL GL state that the JS bound (program + VAO + ARRAY /
   ELEMENT buffer + 2D texture) and that bridge_reset_2d reverted to 0 for
   the legacy glBegin/glEnd DOM pass. Called just BEFORE mini_bridge_fire_raf
   so rAF drawArrays/drawElements runs against the app's own shader + VAO +
   buffers instead of program 0 / VAO 0 (which the app's state cache still
   believes are already bound, so it would skip re-binding and render with no
   attributes -> black). */
void mini_gl_bridge_restore_webgl(void *state)
{
    MiniGLBridge *b = (MiniGLBridge *)state;
    if (!b)
        return;
    if (b->UseProgram && b->current_program)
        b->UseProgram(b->current_program);
    if (b->BindVertexArray && b->last_vao)
        b->BindVertexArray(b->last_vao);
    if (b->BindBuffer)
    {
        if (b->last_array_buffer)
            b->BindBuffer(0x8892 /*GL_ARRAY_BUFFER*/, b->last_array_buffer);
        if (b->last_element_array_buffer)
            b->BindBuffer(0x8893 /*GL_ELEMENT_ARRAY_BUFFER*/, b->last_element_array_buffer);
    }
    if (b->BindTexture && b->last_texture_2d)
        b->BindTexture(0x0DE1 /*GL_TEXTURE_2D*/, b->last_texture_2d);
    if (b->depth_test_enabled)
        glEnable(0x0B71 /*GL_DEPTH_TEST*/);
    if (b->cull_face_enabled)
        glEnable(0x0B44 /*GL_CULL_FACE*/);
    if (b->blend_enabled)
        glEnable(0x0BE2 /*GL_BLEND*/);
    if (b->scissor_test_enabled)
        glEnable(0x0C11 /*GL_SCISSOR_TEST*/);
    if (b->has_clear)
        glClearColor(b->clear_r, b->clear_g, b->clear_b, b->clear_a);
}

/* ================================================================== */
/* JS shim — standard property descriptors built on the C primitives. */
/* This is what makes element.style.color = "red" route to mini_style_set */
/* and element.textContent read/write route to the node tree.          */
/* ================================================================== */
static const char *mini_js_shim =
    "Object.defineProperty(window, 'innerWidth', { configurable: true, get() { return typeof __mini_get_inner_width === 'function' ? __mini_get_inner_width() : 1280; } });\n"
    "Object.defineProperty(window, 'innerHeight', { configurable: true, get() { return typeof __mini_get_inner_height === 'function' ? __mini_get_inner_height() : 720; } });\n"
    "Object.defineProperty(window, 'outerWidth', { configurable: true, get() { return window.innerWidth; } });\n"
    "Object.defineProperty(window, 'outerHeight', { configurable: true, get() { return window.innerHeight; } });\n"
    "Object.defineProperty(window, 'devicePixelRatio', { configurable: true, get() { return typeof __mini_get_dpr === 'function' ? __mini_get_dpr() : 1.0; } });\n"
    "Object.defineProperty(window, 'scrollX', { configurable: true, get() { return typeof __mini_get_scroll_x === 'function' ? __mini_get_scroll_x() : 0.0; } });\n"
    "Object.defineProperty(window, 'scrollY', { configurable: true, get() { return typeof __mini_get_scroll_y === 'function' ? __mini_get_scroll_y() : 0.0; } });\n"
    "Object.defineProperty(window, 'pageXOffset', { configurable: true, get() { return window.scrollX; } });\n"
    "Object.defineProperty(window, 'pageYOffset', { configurable: true, get() { return window.scrollY; } });\n"
    "Object.defineProperty(globalThis, 'innerWidth', { configurable: true, get() { return window.innerWidth; } });\n"
    "Object.defineProperty(globalThis, 'innerHeight', { configurable: true, get() { return window.innerHeight; } });\n"
    "Object.defineProperty(globalThis, 'devicePixelRatio', { configurable: true, get() { return window.devicePixelRatio; } });\n"
    /* setInterval / clearInterval / setTimeout / clearTimeout are native now
       (stable ids + recurring re-arm), so the old setTimeout-loop shim that
       overrode them is gone — leaving it would re-break cancel. */
    "Object.defineProperty(HTMLCanvasElement.prototype, 'width', {\n"
    "  get() { return this._w || Number(this.getAttribute('width')) || window.innerWidth; },\n"
    "  set(v) { this._w = Number(v); this.setAttribute('width', String(v)); }\n"
    "});\n"
    "Object.defineProperty(HTMLCanvasElement.prototype, 'height', {\n"
    "  get() { return this._h || Number(this.getAttribute('height')) || window.innerHeight; },\n"
    "  set(v) { this._h = Number(v); this.setAttribute('height', String(v)); }\n"
    "});\n"
    "Object.defineProperty(MiniElement.prototype, 'offsetWidth', { configurable:true,\n"
    "  get(){ const r = this.getBoundingClientRect ? this.getBoundingClientRect() : null; return r ? r.width : 0; } });\n"
    "Object.defineProperty(MiniElement.prototype, 'offsetHeight', { configurable:true,\n"
    "  get(){ const r = this.getBoundingClientRect ? this.getBoundingClientRect() : null; return r ? r.height : 0; } });\n"
    "Object.defineProperty(MiniElement.prototype, 'offsetLeft', { configurable:true,\n"
    "  get(){ const r = this.getBoundingClientRect ? this.getBoundingClientRect() : null; return r ? r.left : 0; } });\n"
    "Object.defineProperty(MiniElement.prototype, 'offsetTop', { configurable:true,\n"
    "  get(){ const r = this.getBoundingClientRect ? this.getBoundingClientRect() : null; return r ? r.top : 0; } });\n"
    "Object.defineProperty(MiniElement.prototype, 'clientWidth', { configurable:true,\n"
    "  get(){ const r = this.getBoundingClientRect ? this.getBoundingClientRect() : null; return r ? r.width : 0; } });\n"
    "Object.defineProperty(MiniElement.prototype, 'clientHeight', { configurable:true,\n"
    "  get(){ const r = this.getBoundingClientRect ? this.getBoundingClientRect() : null; return r ? r.height : 0; } });\n"
    "Object.defineProperty(MiniElement.prototype, 'style', { configurable:true,\n"
    "  get() {\n"
    "    if (this._styleProxy) return this._styleProxy;\n"
    "    const el = this;\n"
    "    const target = {\n"
    "      setProperty(p, v) { el._setStyle(String(p), String(v)); this[p] = v; },\n"
    "      removeProperty(p) { el._setStyle(String(p), ''); delete this[p]; },\n"
    "      get cssText() { return el.getAttribute('style') || ''; },\n"
    "      set cssText(v) { el.setAttribute('style', String(v)); }\n"
    "    };\n"
    "    this._styleProxy = new Proxy(target, {\n"
    "      get(t, p) { return typeof t[p] === 'function' ? t[p].bind(t) : t[p]; },\n"
    "      set(t, p, v) {\n"
    "        if (p === 'cssText') { t.cssText = v; return true; }\n"
    "        el._setStyle(String(p), String(v));\n"
    "        t[p] = v;\n"
    "        return true;\n"
    "      }\n"
    "    });\n"
    "    return this._styleProxy;\n"
    "  }\n"
    "});\n"
    "Object.defineProperty(MiniElement.prototype, 'textContent', { configurable:true,\n"
    "  get(){ return this._getText(); }, set(v){ this._setText(v==null?'':String(v)); } });\n"
    "Object.defineProperty(MiniElement.prototype, 'innerHTML', { configurable:true,\n"
    "  get(){ return this._getInnerHTML(); }, set(v){ this._setInnerHTML(v==null?'':String(v)); } });\n"
    "Object.defineProperty(MiniDocument.prototype, 'activeElement', { configurable:true,\n"
    "  get(){ return this._getActiveElement(); } });\n"
    "Object.defineProperty(MiniDocument.prototype, 'title', { configurable:true,\n"
    "  get(){ return this._title||''; }, set(v){ this._title=String(v); if(typeof __mini_set_title==='function') __mini_set_title(this._title); } });\n"
    "Object.defineProperty(MiniElement.prototype, 'tagName', { configurable:true,\n"
    "  get(){ return this._tag(); } });\n"
    "Object.defineProperty(MiniElement.prototype, 'id', { configurable:true,\n"
    "  get(){ return this.getAttribute('id')||''; }, set(v){ this.setAttribute('id', String(v)); } });\n"
    /* value: the IDL property form controls are read/written through. For
       <textarea> the rendered value lives in textContent; for everything
       else (text <input>, <input type=range>, <option>...) it lives in the
       `value` attribute, which the renderer reads. Routing .value through
       setAttribute/setAttribute keeps JS, the native editor (mini_events.c)
       and the renderer all looking at the same store — without this, a
       typed or dragged value was invisible to JS that read el.value. */
    "Object.defineProperty(MiniElement.prototype, 'value', { configurable:true,\n"
    "  get(){ var t=this.tagName; if(t==='TEXTAREA'){ return this._getText()||''; } return this.getAttribute('value')||''; },\n"
    "  set(v){ v=v==null?'':String(v); var t=this.tagName; if(t==='TEXTAREA'){ this._setText(v); } else { this.setAttribute('value', v); } } });\n"
    /* draggable: the HTML5 DnD toggle (an attribute, like id). */
    "Object.defineProperty(MiniElement.prototype, 'draggable', { configurable:true,\n"
    "  get(){ return this.getAttribute('draggable')==='true'; },\n"
    "  set(v){ this.setAttribute('draggable', v?'true':'false'); } });\n"
    "Object.defineProperty(MiniElement.prototype, 'src', { configurable:true,\n"
    "  get(){ return this.getAttribute('src')||''; }, set(v){ this.setAttribute('src', v==null?'':String(v)); } });\n"
    "Object.defineProperty(MiniElement.prototype, 'href', { configurable:true,\n"
    "  get(){ return this.getAttribute('href')||''; }, set(v){ this.setAttribute('href', v==null?'':String(v)); } });\n"
    "Object.defineProperty(MiniElement.prototype, 'checked', { configurable:true,\n"
    "  get(){ return this.getAttribute('checked')!==null; }, set(v){ if(v){ this.setAttribute('checked', ''); } else { this.removeAttribute('checked'); } } });\n"
    "Object.defineProperty(MiniElement.prototype, 'disabled', { configurable:true,\n"
    "  get(){ return this.getAttribute('disabled')!==null; }, set(v){ if(v){ this.setAttribute('disabled', ''); } else { this.removeAttribute('disabled'); } } });\n"
    "Object.defineProperty(MiniElement.prototype, 'sheet', { configurable:true, get(){\n"
    "  var el=this; return { insertRule: function(rule, idx){ if(el._setText){ el._setText((el._getText()||'') + '\\n' + rule); } return 0; }, deleteRule: function(idx){}, get cssRules(){ return []; } };\n"
    "}});\n"
    "MiniDocument.prototype.querySelector = function(sel){ return this._cq(String(sel)); };\n"
    "MiniDocument.prototype.querySelectorAll = function(sel){ return this._cqa(String(sel)); };\n"
    "MiniDocument.prototype.getElementsByTagName = function(tag){ return this._cqa(String(tag)); };\n"
    "MiniDocument.prototype.getElementsByClassName = function(cls){ return this._cqa('.' + String(cls)); };\n"
    "MiniElement.prototype.querySelector = function(sel){ return this._cq ? this._cq(String(sel)) : document.querySelector(sel); };\n"
    "MiniElement.prototype.querySelectorAll = function(sel){ return this._cqa ? this._cqa(String(sel)) : document.querySelectorAll(sel); };\n"
    "MiniElement.prototype.getElementsByTagName = function(tag){ return this.querySelectorAll(String(tag)); };\n"
    "MiniElement.prototype.getElementsByClassName = function(cls){ return this.querySelectorAll('.' + String(cls)); };\n"
    "MiniElement.prototype.getClientRects = function(){ var r = this.getBoundingClientRect(); return [r]; };\n"
    "Object.defineProperty(MiniElement.prototype, 'ownerDocument', { configurable:true, get(){ return typeof document !== 'undefined' ? document : null; } });\n"
    "MiniElement.prototype.click = function(){ var ev = new MouseEvent('click', { bubbles:true, cancelable:true }); this.dispatchEvent(ev); };\n"
    "if (typeof ImageBitmap === 'undefined') {\n"
    "  globalThis.ImageBitmap = class ImageBitmap { constructor(w,h){ this.width=w||0; this.height=h||0; } close(){ this.width=0; this.height=0; } };\n"
    "  window.ImageBitmap = globalThis.ImageBitmap;\n"
    "}\n"
    "globalThis.createImageBitmap = function(src){ return (typeof __miniCreateImageBitmap === 'function') ? __miniCreateImageBitmap(src) : Promise.resolve(new ImageBitmap((src&&src.width)||300, (src&&src.height)||150)); };\n"
    "window.createImageBitmap = globalThis.createImageBitmap;\n"
    "if (typeof WebGL2RenderingContext === 'undefined' && typeof WebGLRenderingContext !== 'undefined') {\n"
    "  globalThis.WebGL2RenderingContext = WebGLRenderingContext;\n"
    "  window.WebGL2RenderingContext = WebGLRenderingContext;\n"
    "}\n"
    "function TextDecoder(encoding, options){\n"
    "  this.encoding = (encoding || 'utf-8').toLowerCase();\n"
    "  this.fatal = !!(options && options.fatal);\n"
    "  this.ignoreBOM = !!(options && options.ignoreBOM);\n"
    "}\n"
    "TextDecoder.prototype.decode = function(input, options){\n"
    "  if (!input) return '';\n"
    "  var bytes;\n"
    "  if (input instanceof ArrayBuffer) bytes = new Uint8Array(input);\n"
    "  else if (ArrayBuffer.isView(input)) bytes = new Uint8Array(input.buffer, input.byteOffset, input.byteLength);\n"
    "  else if (Array.isArray(input)) bytes = new Uint8Array(input);\n"
    "  else bytes = new Uint8Array(0);\n"
    "  var len = bytes.length, out = '', i = 0;\n"
    "  while (i < len){\n"
    "    var b1 = bytes[i++];\n"
    "    if (b1 < 0x80){ out += String.fromCharCode(b1); }\n"
    "    else if (b1 > 0xBF && b1 < 0xE0){ var b2 = bytes[i++] & 0x3F; out += String.fromCharCode(((b1 & 0x1F) << 6) | b2); }\n"
    "    else if (b1 > 0xDF && b1 < 0xF0){ var b2 = bytes[i++] & 0x3F, b3 = bytes[i++] & 0x3F; out += String.fromCharCode(((b1 & 0x0F) << 12) | (b2 << 6) | b3); }\n"
    "    else if (b1 > 0xEF && b1 < 0x100){\n"
    "      var b2 = bytes[i++] & 0x3F, b3 = bytes[i++] & 0x3F, b4 = bytes[i++] & 0x3F;\n"
    "      var code = (((b1 & 0x07) << 18) | (b2 << 12) | (b3 << 6) | b4) - 0x10000;\n"
    "      out += String.fromCharCode(0xD800 + (code >> 10), 0xDC00 + (code & 0x3FF));\n"
    "    }\n"
    "  }\n"
    "  return out;\n"
    "};\n"
    "window.TextDecoder = TextDecoder; globalThis.TextDecoder = TextDecoder;\n"
    "function TextEncoder(){\n"
    "  this.encoding = 'utf-8';\n"
    "}\n"
    "TextEncoder.prototype.encode = function(str){\n"
    "  str = String(str || ''); var utf8 = [];\n"
    "  for (var i = 0; i < str.length; i++){\n"
    "    var charcode = str.charCodeAt(i);\n"
    "    if (charcode < 0x80) utf8.push(charcode);\n"
    "    else if (charcode < 0x800) utf8.push(0xc0 | (charcode >> 6), 0x80 | (charcode & 0x3f));\n"
    "    else if (charcode < 0xd800 || charcode >= 0xe000) utf8.push(0xe0 | (charcode >> 12), 0x80 | ((charcode >> 6) & 0x3f), 0x80 | (charcode & 0x3f));\n"
    "    else { i++; charcode = 0x10000 + (((charcode & 0x3ff) << 10) | (str.charCodeAt(i) & 0x3ff)); utf8.push(0xf0 | (charcode >> 18), 0x80 | ((charcode >> 12) & 0x3f), 0x80 | ((charcode >> 6) & 0x3f), 0x80 | (charcode & 0x3f)); }\n"
    "  }\n"
    "  return new Uint8Array(utf8);\n"
    "};\n"
    "window.TextEncoder = TextEncoder; globalThis.TextEncoder = TextEncoder;\n"
    "function Headers(init){\n"
    "  this._map = {};\n"
    "  if (init){\n"
    "    if (init instanceof Headers){ Object.assign(this._map, init._map); }\n"
    "    else if (Array.isArray(init)){ for (var i=0;i<init.length;i++) this._map[String(init[i][0]).toLowerCase()] = String(init[i][1]); }\n"
    "    else if (typeof init==='object'){ for (var k in init) this._map[String(k).toLowerCase()] = String(init[k]); }\n"
    "  }\n"
    "}\n"
    "Headers.prototype.get = function(k){ return this._map[String(k).toLowerCase()] || null; };\n"
    "Headers.prototype.set = function(k,v){ this._map[String(k).toLowerCase()] = String(v); };\n"
    "Headers.prototype.has = function(k){ return String(k).toLowerCase() in this._map; };\n"
    "Headers.prototype.delete = function(k){ delete this._map[String(k).toLowerCase()]; };\n"
    "Headers.prototype.forEach = function(cb){ for (var k in this._map) cb(this._map[k], k, this); };\n"
    "window.Headers = Headers; globalThis.Headers = Headers;\n"
    "function Request(input, init){\n"
    "  init = init || {};\n"
    "  if (typeof input==='string'){ this.url = input; }\n"
    "  else if (input && input.url){ this.url = input.url; init = Object.assign({}, input, init); }\n"
    "  else { this.url = ''; }\n"
    "  this.method = (init.method || 'GET').toUpperCase();\n"
    "  this.headers = new Headers(init.headers);\n"
    "  this.body = init.body || null;\n"
    "  this.credentials = init.credentials || 'same-origin';\n"
    "}\n"
    "window.Request = Request; globalThis.Request = Request;\n"
    "function Response(body, init){\n"
    "  init = init || {};\n"
    "  this.status = init.status || 200;\n"
    "  this.statusText = init.statusText || 'OK';\n"
    "  this.ok = this.status >= 200 && this.status < 300;\n"
    "  this.headers = new Headers(init.headers);\n"
    "  this.url = init.url || '';\n"
    "  this._body = body || '';\n"
    "}\n"
    "Response.prototype.text = function(){ return __thenable(typeof this._body==='string' ? this._body : String(this._body)); };\n"
    "Response.prototype.json = function(){ return this.text().then(function(t){ return JSON.parse(t); }); };\n"
    "Response.prototype.arrayBuffer = function(){ return __thenable(this._body instanceof ArrayBuffer ? this._body : (this._body ? this._body.buffer || new ArrayBuffer(0) : new ArrayBuffer(0))); };\n"
    "Response.prototype.blob = function(){ return __thenable(new Blob([this._body])); };\n"
    "window.Response = Response; globalThis.Response = Response;\n"
    "if (typeof URL === 'undefined' || !URL.createObjectURL) {\n"
    "  function MiniURL(url, base) { if(base){ if(String(url).match(/^[a-zA-Z]+:\\/\\//) || String(url).startsWith('blob:')){ this.href=String(url); } else { this.href=String(base).replace(/\\/[^\\/]*$/, '/') + url; } } else { this.href=String(url); } }\n"
    "  MiniURL.prototype.toString=function(){ return this.href; };\n"
    "  MiniURL.createObjectURL=function(blob){ return (blob && typeof __miniCreateObjectURL==='function') ? __miniCreateObjectURL(blob) : ''; };\n"
    "  MiniURL.revokeObjectURL=function(url){ if(url && typeof __miniRevokeObjectURL==='function') __miniRevokeObjectURL(String(url)); };\n"
    "  window.URL = MiniURL; globalThis.URL = MiniURL;\n"
    "} else {\n"
    "  URL.createObjectURL=function(blob){ return (blob && typeof __miniCreateObjectURL==='function') ? __miniCreateObjectURL(blob) : ''; };\n"
    "  URL.revokeObjectURL=function(url){ if(url && typeof __miniRevokeObjectURL==='function') __miniRevokeObjectURL(String(url)); };\n"
    "}\n"
    "function Blob(parts, options){\n"
    "  options = options || {}; this.type = options.type || '';\n"
    "  var totalLen = 0, bufs = [];\n"
    "  if (parts && parts.length){\n"
    "    for (var i = 0; i < parts.length; i++){\n"
    "      var p = parts[i];\n"
    "      if (typeof p === 'string'){\n"
    "        var enc = (typeof TextEncoder !== 'undefined') ? new TextEncoder().encode(p) : new Uint8Array(0);\n"
    "        bufs.push(enc); totalLen += enc.byteLength;\n"
    "      } else if (p instanceof ArrayBuffer){\n"
    "        bufs.push(new Uint8Array(p)); totalLen += p.byteLength;\n"
    "      } else if (ArrayBuffer.isView(p)){\n"
    "        bufs.push(new Uint8Array(p.buffer, p.byteOffset, p.byteLength)); totalLen += p.byteLength;\n"
    "      } else if (p && p.__buffer){\n"
    "        bufs.push(new Uint8Array(p.__buffer)); totalLen += p.__buffer.byteLength;\n"
    "      } else if (p && p.__filePath){\n"
    "        var ab = (typeof __miniReadFileBinary === 'function') ? __miniReadFileBinary(p.__filePath) : new ArrayBuffer(0);\n"
    "        bufs.push(new Uint8Array(ab)); totalLen += ab.byteLength;\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  var merged = new Uint8Array(totalLen), off = 0;\n"
    "  for (var j = 0; j < bufs.length; j++){ merged.set(bufs[j], off); off += bufs[j].byteLength; }\n"
    "  this.__buffer = merged.buffer; this.size = totalLen;\n"
    "}\n"
    "Blob.prototype.arrayBuffer=function(){\n"
    "  if (this.__filePath){ var ab=(typeof __miniReadFileBinary==='function')?__miniReadFileBinary(this.__filePath):new ArrayBuffer(0); return Promise.resolve(ab); }\n"
    "  return Promise.resolve(this.__buffer ? this.__buffer.slice(0) : new ArrayBuffer(0));\n"
    "};\n"
    "Blob.prototype.text=function(){\n"
    "  if (this.__filePath){ var t=(typeof __miniReadFileText==='function')?__miniReadFileText(this.__filePath):''; return Promise.resolve(t); }\n"
    "  var str = (typeof TextDecoder !== 'undefined') ? new TextDecoder().decode(this.__buffer || new Uint8Array(0)) : '';\n"
    "  return Promise.resolve(str);\n"
    "};\n"
    "Blob.prototype.slice=function(start, end, contentType){\n"
    "  if (this.__filePath){\n"
    "    var ab = (typeof __miniReadFileBinary === 'function') ? __miniReadFileBinary(this.__filePath) : new ArrayBuffer(0);\n"
    "    var sl = ab.slice(start || 0, end !== undefined ? end : ab.byteLength);\n"
    "    return new Blob([sl], { type: contentType || this.type });\n"
    "  }\n"
    "  var buf = this.__buffer || new ArrayBuffer(0);\n"
    "  var sl2 = buf.slice(start || 0, end !== undefined ? end : buf.byteLength);\n"
    "  return new Blob([sl2], { type: contentType || this.type });\n"
    "};\n"
    "window.Blob = Blob; globalThis.Blob = Blob;\n"
    "function File(parts, name, options){\n"
    "  Blob.call(this, parts, options);\n"
    "  this.name = String(name || 'file'); options = options || {};\n"
    "  this.lastModified = options.lastModified || Date.now();\n"
    "  this.webkitRelativePath = options.webkitRelativePath || '';\n"
    "}\n"
    "File.prototype = Object.create(Blob.prototype); File.prototype.constructor = File;\n"
    "window.File = File; globalThis.File = File;\n"
    "function FileReader(){\n"
    "  this.result = null; this.error = null; this.readyState = 0;\n"
    "  this.onload = null; this.onerror = null; this.onloadend = null;\n"
    "}\n"
    "FileReader.prototype.readAsArrayBuffer=function(blob){\n"
    "  var self = this; self.readyState = 1;\n"
    "  blob.arrayBuffer().then(function(buf){ self.readyState = 2; self.result = buf; if(self.onload) self.onload({target:self}); if(self.onloadend) self.onloadend({target:self}); }).catch(function(err){ self.readyState = 2; self.error = err; if(self.onerror) self.onerror({target:self}); if(self.onloadend) self.onloadend({target:self}); });\n"
    "};\n"
    "FileReader.prototype.readAsText=function(blob){\n"
    "  var self = this; self.readyState = 1;\n"
    "  blob.text().then(function(str){ self.readyState = 2; self.result = str; if(self.onload) self.onload({target:self}); if(self.onloadend) self.onloadend({target:self}); }).catch(function(err){ self.readyState = 2; self.error = err; if(self.onerror) self.onerror({target:self}); if(self.onloadend) self.onloadend({target:self}); });\n"
    "};\n"
    "FileReader.prototype.readAsDataURL=function(blob){\n"
    "  var self = this; self.readyState = 1;\n"
    "  blob.arrayBuffer().then(function(buf){\n"
    "    var bytes = new Uint8Array(buf), binary = '';\n"
    "    for (var i = 0; i < bytes.byteLength; i++) binary += String.fromCharCode(bytes[i]);\n"
    "    var b64 = window.btoa(binary), type = blob.type || 'application/octet-stream';\n"
    "    self.readyState = 2; self.result = 'data:' + type + ';base64,' + b64;\n"
    "    if(self.onload) self.onload({target:self}); if(self.onloadend) self.onloadend({target:self});\n"
    "  }).catch(function(err){ self.readyState = 2; self.error = err; if(self.onerror) self.onerror({target:self}); if(self.onloadend) self.onloadend({target:self}); });\n"
    "};\n"
    "window.FileReader = FileReader; globalThis.FileReader = FileReader;\n"
    "window.Image = function(w, h){\n"
    "  var el = document.createElement('img'); if(w) el.width = w; if(h) el.height = h; return el;\n"
    "};\n"
    "globalThis.Image = window.Image;\n"
    "(function(){\n"
    "  Object.defineProperty(MiniElement.prototype, 'src', {\n"
    "    configurable: true,\n"
    "    get: function(){ return this.getAttribute('src') || ''; },\n"
    "    set: function(v){\n"
    "      v = (v == null) ? '' : String(v); this.setAttribute('src', v);\n"
    "      var tag = (this.tagName || this.nodeName || '').toUpperCase();\n"
    "      if (tag === 'IMG'){\n"
    "        var self = this;\n"
    "        var info = (typeof __miniGetImageInfo === 'function') ? __miniGetImageInfo(v) : null;\n"
    "        if (info){\n"
    "          self.width = info.width; self.height = info.height;\n"
    "          self.naturalWidth = info.width; self.naturalHeight = info.height;\n"
    "          self.complete = true;\n"
    "        } else {\n"
    "          self.width = 1; self.height = 1;\n"
    "          self.naturalWidth = 1; self.naturalHeight = 1;\n"
    "          self.complete = true;\n"
    "        }\n"
    "        setTimeout(function(){\n"
    "          var ev = { type: 'load', target: self, currentTarget: self };\n"
    "          if (typeof self.dispatchEvent === 'function') self.dispatchEvent(ev);\n"
    "          if (typeof self.onload === 'function') self.onload(ev);\n"
    "        }, 0);\n"
    "      }\n"
    "    }\n"
    "  });\n"
    "})();\n"
    /* ---- Stage 4+: the standard surface real apps reach for after addEventListener ---- */
    /* innerText: THE click fix. Previously undefined, so a handler that did
       `el.innerText = '...'` only set a plain JS prop and never touched the
       DOM tree -> the page looked unresponsive after a click. Route it through
    "Object.defineProperty(MiniElement.prototype, 'innerText', { configurable:true,\n"
    "  get(){ return this._getText(); }, set(v){ this._setText(v==null?'':String(v)); } });\n"
    "Object.defineProperty(MiniElement.prototype, 'outerHTML', { configurable:true,\n"
    "  get(){ return this._getOuterHTML(); }, set(v){ this._setOuterHTML(v==null?'':String(v)); } });\n"
    "Object.defineProperty(MiniElement.prototype, 'className', { configurable:true,\n"
    "  get(){ return this.getAttribute('class')||''; }, set(v){ this.setAttribute('class', String(v)); } });\n"
    "Object.defineProperty(MiniElement.prototype, 'nodeName', { configurable:true, get(){ return this._nodeName(); } });\n"
    "Object.defineProperty(MiniElement.prototype, 'nodeValue', { configurable:true,\n"
    "  get(){ return this._getNodeValue(); }, set(v){ this._setNodeValue(v==null?null:String(v)); } });\n"
    "Object.defineProperty(MiniElement.prototype, 'data', { configurable:true,\n"
    "  get(){ return this._getNodeValue(); }, set(v){ this._setNodeValue(v==null?null:String(v)); } });\n"
    /* Tree-traversal + nodeType are DOM *properties*; back them by the _-prefixed
       C methods so `el.parentNode` / `el.nodeType` read as values, not functions. */
    "(function(ps){ ps.forEach(function(p){ Object.defineProperty(MiniElement.prototype, p, { configurable:true, get:function(){ return this['_'+p](); } }); }); })\n"
    "(['firstChild','lastChild','firstElementChild','nextSibling','previousSibling','parentNode','parentElement','lastElementChild','nextElementSibling','previousElementSibling','children','childNodes','childElementCount','nodeType']);\n"
    /* classList: a DOMToken-like view over the `class` attribute. */
    "Object.defineProperty(MiniElement.prototype, 'classList', { configurable:true, get(){\n"
    "  var el=this;\n"
    "  var toks=function(){ return (el.getAttribute('class')||'').split(' ').filter(function(x){return x.length>0;}); };\n"
    "  var r={};\n"
    "  Object.defineProperty(r,'length',{get:function(){return toks().length;}});\n"
    "  r.contains=function(c){ return toks().indexOf(String(c))>=0; };\n"
    "  r.add=function(){ var cur=toks(); for(var i=0;i<arguments.length;i++){ var c=String(arguments[i]); if(cur.indexOf(c)<0) cur.push(c);} el.setAttribute('class',cur.join(' ')); };\n"
    "  r.remove=function(){ var cur=toks(); for(var i=0;i<arguments.length;i++){ var c=String(arguments[i]); var j=cur.indexOf(c); if(j>=0) cur.splice(j,1);} el.setAttribute('class',cur.join(' ')); };\n"
    "  r.toggle=function(c,force){ var cur=toks(); c=String(c); var has=cur.indexOf(c)>=0; if(has&&force!==true){cur.splice(cur.indexOf(c),1);el.setAttribute('class',cur.join(' '));return false;} if(!has&&force!==false){cur.push(c);el.setAttribute('class',cur.join(' '));return true;} return has; };\n"
    "  r.replace=function(o,n){ var cur=toks(); o=String(o);n=String(n); var i=cur.indexOf(o); if(i>=0){cur[i]=n;el.setAttribute('class',cur.join(' '));} };\n"
    "  r.item=function(i){ var t=toks(); return t[i]||null; };\n"
    "  r.toString=function(){ return el.getAttribute('class')||''; };\n"
    "  return r;\n"
    "}});\n"
    /* dataset: a Proxy over data-* attributes (camelCase <-> data-kebab-case). */
    "Object.defineProperty(MiniElement.prototype, 'dataset', { configurable:true, get(){\n"
    "  var el=this; var kebab=function(k){ return 'data-'+String(k).replace(/([A-Z])/g,'-$1').toLowerCase(); };\n"
    "  return new Proxy({}, {\n"
    "    get:function(t,k){ if(typeof k!=='string') return undefined; var v=el.getAttribute(kebab(k)); return v===null?undefined:v; },\n"
    "    set:function(t,k,v){ el.setAttribute(kebab(k), String(v)); t[k]=v; return true; },\n"
    "    has:function(t,k){ return el.getAttribute(kebab(k))!==null; },\n"
    "    deleteProperty:function(t,k){ el.removeAttribute(kebab(k)); delete t[k]; return true; },\n"
    "    ownKeys:function(t){ return Object.keys(t); }\n"
    "  });\n"
    "}});\n"
    /* matches / closest are native (pointer-compare) — see js_matches/js_closest.
       A JS `arr[i] === this` impl would be always-false here since wrappers
       are not interned. */
    /* HTMLElement alias so `instanceof HTMLElement` works. */
    "window.HTMLElement = MiniElement;\n"
    /* getComputedStyle: parse the C _computedStyleJSON into a CSSStyleDeclaration-ish object. */
    "window.getComputedStyle = function(el){\n"
    "  var s={}; var raw=(el && el._computedStyleJSON) ? el._computedStyleJSON() : '[]';\n"
    "  var arr; try{ arr=JSON.parse(raw); }catch(e){ arr=[]; }\n"
    "  for(var i=0;i<arr.length;i++){ var p=arr[i]; if(p && p.length>=2){ s[p[0]]=p[1]; } }\n"
    "  s.getPropertyValue=function(k){ k=String(k); return s[k]||''; };\n"
    "  s.getPropertyPriority=function(){ return ''; };\n"
    "  s.item=function(){ return ''; };\n"
    "  s.cssText='';\n"
    "  return s;\n"
    "};\n"
    "getComputedStyle = window.getComputedStyle;\n"
    /* window.getSelection(): minimal Selection backed by the C page-text
       selection. toString() returns the selected text (what apps read). */
    "window.getSelection = function(){ var t = (typeof __miniSelectionText==='function') ? __miniSelectionText() : ''; return { anchorNode:null, anchorOffset:0, focusNode:null, focusOffset:0, isCollapsed: !t.length, rangeCount: t.length?1:0, type: t.length?'Range':'None', toString:function(){ return t; }, removeAllRanges:function(){}, addRange:function(){} }; };\n"
    "getSelection = window.getSelection;\n"
    /* base64 (atob/btoa) — pure JS, no deps. */
    "var _b64ch='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';\n"
    "window.btoa=function(s){ s=String(s); var out=''; var i=0; var len=s.length;\n"
    "  for(i=0;i+2<len;i+=3){ var b=(s.charCodeAt(i)<<16)|(s.charCodeAt(i+1)<<8)|s.charCodeAt(i+2); out+=_b64ch[(b>>18)&63]+_b64ch[(b>>12)&63]+_b64ch[(b>>6)&63]+_b64ch[b&63]; }\n"
    "  var rem=len-i;\n"
    "  if(rem===1){ var b2=s.charCodeAt(i)<<16; out+=_b64ch[(b2>>18)&63]+_b64ch[(b2>>12)&63]+'=='; }\n"
    "  else if(rem===2){ var b3=(s.charCodeAt(i)<<16)|(s.charCodeAt(i+1)<<8); out+=_b64ch[(b3>>18)&63]+_b64ch[(b3>>12)&63]+_b64ch[(b3>>6)&63]+'='; }\n"
    "  return out; };\n"
    "window.atob=function(s){ s=String(s).replace(/[^A-Za-z0-9+/=]/g,''); var out=''; var lu={}; for(var k=0;k<_b64ch.length;k++) lu[_b64ch[k]]=k;\n"
    "  for(var i=0;i+3<s.length;i+=4){ var n=(lu[s[i]]<<18)|(lu[s[i+1]]<<12)|((s[i+2]==='='?0:lu[s[i+2]])<<6)|(s[i+3]==='='?0:lu[s[i+3]]); out+=String.fromCharCode((n>>16)&255); if(s[i+2]!=='=') out+=String.fromCharCode((n>>8)&255); if(s[i+3]!=='=') out+=String.fromCharCode(n&255); }\n"
    "  return out; };\n"
    "btoa=window.btoa; atob=window.atob;\n"
    /* Event / CustomEvent / MouseEvent / PointerEvent / WheelEvent constructors usable with dispatchEvent. */
    "function Event(type, opts){ this.type=String(type); opts=opts||{}; this.bubbles=!!opts.bubbles; this.cancelable=!!opts.cancelable; this.detail=null; this.target=null; this.currentTarget=null; this.timeStamp=(typeof performance!=='undefined'&&performance.now)?performance.now():0; this.defaultPrevented=false; this.propagationStopped=false; }\n"
    "Event.prototype.preventDefault=function(){ this.defaultPrevented=!!this.cancelable; };\n"
    "Event.prototype.stopPropagation=function(){ this.propagationStopped=true; this.__stop=true; };\n"
    "Event.prototype.stopImmediatePropagation=function(){ this.propagationStopped=true; this.__stop=true; };\n"
    "function CustomEvent(type, opts){ Event.call(this,type,opts); this.detail=(opts&&opts.detail!==undefined)?opts.detail:null; }\n"
    "CustomEvent.prototype=Object.create(Event.prototype); CustomEvent.prototype.constructor=CustomEvent;\n"
    "function UIEvent(type, opts){ Event.call(this,type,opts); if(opts) Object.assign(this, opts); }\n"
    "UIEvent.prototype=Object.create(Event.prototype); UIEvent.prototype.constructor=UIEvent;\n"
    "function MouseEvent(type, opts){ UIEvent.call(this,type,opts); this.clientX=(opts&&opts.clientX)||0; this.clientY=(opts&&opts.clientY)||0; this.button=(opts&&opts.button)||0; this.buttons=(opts&&opts.buttons)||0; this.movementX=(opts&&opts.movementX)||0; this.movementY=(opts&&opts.movementY)||0; if(opts) Object.assign(this, opts); }\n"
    "MouseEvent.prototype=Object.create(UIEvent.prototype); MouseEvent.prototype.constructor=MouseEvent;\n"
    "function PointerEvent(type, opts){ MouseEvent.call(this,type,opts); this.pointerId=(opts&&opts.pointerId)||1; this.pointerType=(opts&&opts.pointerType)||'mouse'; this.isPrimary=(opts&&opts.isPrimary)!==undefined?opts.isPrimary:true; this.width=(opts&&opts.width)||1; this.height=(opts&&opts.height)||1; this.pressure=(opts&&opts.pressure)||0; if(opts) Object.assign(this, opts); }\n"
    "PointerEvent.prototype=Object.create(MouseEvent.prototype); PointerEvent.prototype.constructor=PointerEvent;\n"
    "function WheelEvent(type, opts){ MouseEvent.call(this,type,opts); this.deltaX=(opts&&opts.deltaX)||0; this.deltaY=(opts&&opts.deltaY)||0; this.deltaZ=(opts&&opts.deltaZ)||0; this.deltaMode=(opts&&opts.deltaMode)||0; if(opts) Object.assign(this, opts); }\n"
    "WheelEvent.prototype=Object.create(MouseEvent.prototype); WheelEvent.prototype.constructor=WheelEvent;\n"
    "function KeyboardEvent(type, opts){ UIEvent.call(this,type,opts); this.key=(opts&&opts.key)||''; this.code=(opts&&opts.code)||''; this.keyCode=(opts&&opts.keyCode)||0; this.which=(opts&&opts.which)||0; if(opts) Object.assign(this, opts); }\n"
    "KeyboardEvent.prototype=Object.create(UIEvent.prototype); KeyboardEvent.prototype.constructor=KeyboardEvent;\n"
    "function FocusEvent(type, opts){ UIEvent.call(this,type,opts); if(opts) Object.assign(this, opts); }\n"
    "FocusEvent.prototype=Object.create(UIEvent.prototype); FocusEvent.prototype.constructor=FocusEvent;\n"
    "function InputEvent(type, opts){ UIEvent.call(this,type,opts); this.data=(opts&&opts.data)||''; this.inputType=(opts&&opts.inputType)||''; if(opts) Object.assign(this, opts); }\n"
    "InputEvent.prototype=Object.create(UIEvent.prototype); InputEvent.prototype.constructor=InputEvent;\n"
    "window.Event=Event; window.CustomEvent=CustomEvent; window.UIEvent=UIEvent; window.MouseEvent=MouseEvent; window.PointerEvent=PointerEvent; window.WheelEvent=WheelEvent; window.KeyboardEvent=KeyboardEvent; window.FocusEvent=FocusEvent; window.InputEvent=InputEvent;\n"
    "globalThis.Event=Event; globalThis.CustomEvent=CustomEvent; globalThis.UIEvent=UIEvent; globalThis.MouseEvent=MouseEvent; globalThis.PointerEvent=PointerEvent; globalThis.WheelEvent=WheelEvent; globalThis.KeyboardEvent=KeyboardEvent; globalThis.FocusEvent=FocusEvent; globalThis.InputEvent=InputEvent;\n"
    /* location / navigator (standard modern Chrome user-agent). */
    "window.location={href:'http://localhost/',protocol:'http:',host:'localhost',hostname:'localhost',port:'',pathname:'/',search:'',hash:'',origin:'http://localhost',toString:function(){return this.href;},assign:function(){},replace:function(){},reload:function(){}};\n"
    "window.navigator={userAgent:'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36',platform:'Win32',language:'en-US',languages:['en-US','en'],onLine:true,cookieEnabled:true,vendor:'Google Inc.',appVersion:'5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36',product:'Gecko',maxTouchPoints:0,hardwareConcurrency:4};\n"
    "globalThis.location = window.location; location = window.location;\n"
    "globalThis.navigator = window.navigator; navigator = window.navigator;\n"
    "globalThis.performance = window.performance; performance = window.performance;\n"
    /* matchMedia / ResizeObserver / MutationObserver / document.fonts */
    "window.matchMedia = function(q){ return { matches: false, media: String(q), addListener: function(){}, removeListener: function(){}, addEventListener: function(){}, removeEventListener: function(){}, dispatchEvent: function(){ return true; } }; };\n"
    "globalThis.matchMedia = window.matchMedia; matchMedia = window.matchMedia;\n"
    "if (typeof ResizeObserver === 'undefined') {\n"
    "  globalThis.ResizeObserver = class ResizeObserver { constructor(cb){ this.cb=cb; } observe(target){} unobserve(target){} disconnect(){} };\n"
    "  window.ResizeObserver = globalThis.ResizeObserver;\n"
    "}\n"
    "if (typeof MutationObserver === 'undefined') {\n"
    "  globalThis.MutationObserver = class MutationObserver { constructor(cb){ this.cb=cb; } observe(target, opts){} disconnect(){} takeRecords(){ return []; } };\n"
    "  window.MutationObserver = globalThis.MutationObserver;\n"
    "}\n"
    "if (typeof document.fonts === 'undefined') {\n"
    "  document.fonts = { ready: Promise.resolve(), check: function(){ return true; }, load: function(){ return Promise.resolve([]); }, addEventListener: function(){}, removeEventListener: function(){} };\n"
    "}\n"
    "document.queryCommandSupported = function(c){ return false; };\n"
    "document.queryCommandEnabled = function(c){ return false; };\n"
    "document.execCommand = function(c,u,v){ return false; };\n"
    "if(typeof MiniDocument!=='undefined'){\n"
    "  MiniDocument.prototype.queryCommandSupported = document.queryCommandSupported;\n"
    "  MiniDocument.prototype.queryCommandEnabled = document.queryCommandEnabled;\n"
    "  MiniDocument.prototype.execCommand = document.execCommand;\n"
    "  MiniDocument.prototype.hasFocus = function(){ return true; };\n"
    "  MiniDocument.prototype.elementFromPoint = function(x,y){ return document.body; };\n"
    "  MiniDocument.prototype.createRange = function(){\n"
    "    return {\n"
    "      setStart:function(){}, setEnd:function(){}, setStartBefore:function(){}, setStartAfter:function(){},\n"
    "      setEndBefore:function(){}, setEndAfter:function(){}, selectNode:function(){}, selectNodeContents:function(){},\n"
    "      collapse:function(){}, getBoundingClientRect:function(){ return {left:0,top:0,right:0,bottom:0,width:0,height:0,x:0,y:0}; },\n"
    "      getClientRects:function(){ return []; }, cloneContents:function(){ return document.createDocumentFragment(); },\n"
    "      deleteContents:function(){}, extractContents:function(){ return document.createDocumentFragment(); },\n"
    "      insertNode:function(){}, surroundContents:function(){}, commonAncestorContainer:document.body\n"
    "    };\n"
    "  };\n"
    "}\n"
    "document.createRange = (typeof MiniDocument!=='undefined'&&MiniDocument.prototype.createRange) ? MiniDocument.prototype.createRange : function(){ return { getBoundingClientRect:function(){ return {width:0,height:0}; } }; };\n"
    "document.hasFocus = function(){ return true; };\n"
    "document.elementFromPoint = function(x,y){ return document.body; };\n"
    "if (typeof requestIdleCallback === 'undefined') {\n"
    "  globalThis.requestIdleCallback = function(cb){ return setTimeout(function(){ cb({ didTimeout:false, timeRemaining:function(){ return 50; } }); }, 1); };\n"
    "  globalThis.cancelIdleCallback = function(id){ clearTimeout(id); };\n"
    "  window.requestIdleCallback = globalThis.requestIdleCallback;\n"
    "  window.cancelIdleCallback = globalThis.cancelIdleCallback;\n"
    "}\n"
    /* queueMicrotask (QuickJS ships Promise). */
    "window.queueMicrotask=function(fn){ Promise.resolve().then(typeof fn==='function'?fn:function(){}); };\n"
    "queueMicrotask=window.queueMicrotask;\n"
    /* ---- Phase 7 groundwork: History + MessageChannel (pure-JS shims) ---- */
    /* window.history: an in-process session history stack. pushState updates
       window.location.href (a real reload/re-navigation is out of scope for
       the mini engine, so this models the URL bar + back/forward state). */
    "(function(){\n"
    "  var stack=[{href:window.location.href, state:null}]; var cursor=0;\n"
    "  window.history={};\n"
    "  Object.defineProperty(window.history,'length',{get:function(){return stack.length;}});\n"
    "  Object.defineProperty(window.history,'state',{get:function(){return stack[cursor].state;}});\n"
    "  Object.defineProperty(window.history,'scrollRestoration',{get:function(){return 'auto';},set:function(){}});\n"
    "  window.history.pushState=function(state,title,url){\n"
    "    if(cursor<stack.length-1) stack=stack.slice(0,cursor+1);\n"
    "    var e={href: url||window.location.href, state:state}; stack.push(e); cursor=stack.length-1;\n"
    "    if(url){ try{ window.location.href=url; }catch(_){} }\n"
    "    var ev=new Event('popstate'); ev.state=state; (window.onpopstate||function(){}).call(window,ev);\n"
    "  };\n"
    "  window.history.replaceState=function(state,title,url){\n"
    "    stack[cursor]={href: url||window.location.href, state:state};\n"
    "    if(url){ try{ window.location.href=url; }catch(_){} }\n"
    "  };\n"
    "  window.history.back=function(){ window.history.go(-1); };\n"
    "  window.history.forward=function(){ window.history.go(1); };\n"
    "  window.history.go=function(n){\n"
    "    n=n||0; var t=cursor+n; if(t<0)t=0; if(t>stack.length-1)t=stack.length-1;\n"
    "    if(t===cursor) return; cursor=t; window.location.href=stack[cursor].href;\n"
    "    var ev=new Event('popstate'); ev.state=stack[cursor].state; (window.onpopstate||function(){}).call(window,ev);\n"
    "  };\n"
    "})();\n"
    /* MessageChannel: two ports; postMessage lands on the other port's
       onmessage via a macrotask (setTimeout 0) — same as browsers. */
    "function MessageChannel(){\n"
    "  var p1={onmessage:null,_peer:null}, p2={onmessage:null,_peer:null};\n"
    "  p1._peer=p2; p2._peer=p1;\n"
    "  p1.postMessage=function(m){ var peer=p1._peer; setTimeout(function(){ if(peer.onmessage){ var ev=new Event('message'); ev.data=m; peer.onmessage(ev); } },0); };\n"
    "  p2.postMessage=function(m){ var peer=p2._peer; setTimeout(function(){ if(peer.onmessage){ var ev=new Event('message'); ev.data=m; peer.onmessage(ev); } },0); };\n"
    "  p1.start=function(){}; p2.start=function(){}; p1.close=function(){}; p2.close=function(){};\n"
    "  this.port1=p1; this.port2=p2;\n"
    "}\n"
    "window.MessageChannel=MessageChannel;\n"
    /* document.head: live query (the DOM may parse after the shim runs). */
    "Object.defineProperty(document, 'head', { configurable:true, get:function(){ return document.querySelector('head'); } });\n"
    "Object.defineProperty(document, 'pointerLockElement', { configurable:true, get:function(){ return typeof this._pointerLockElement === 'function' ? this._pointerLockElement() : null; } });\n"
    "MiniElement.prototype.requestPointerLock = function(){ if (typeof this._requestPointerLock === 'function') this._requestPointerLock(); };\n"
    "MiniDocument.prototype.exitPointerLock = function(){ if (typeof this._exitPointerLock === 'function') this._exitPointerLock(); };\n"
    "document.exitPointerLock = function(){ if (typeof this._exitPointerLock === 'function') this._exitPointerLock(); };\n"
    /* on* IDL event properties (onclick/onkeydown/...). Without these, the
       "onclick" in element check is false, so Preact (htm) takes l.slice(2) =
       "Click" (wrong case) and registers addEventListener("Click", ...), which
       never matches the lowercase "click" the engine dispatches -> onClick
       never fires (diag: clickFired=false). Defining on<type> accessors on the
       prototype makes the `in` check true AND turns el.onclick=fn into a real
       listener (full IDL semantics). Single-quoted JS to avoid C escaping. */
    "(function(){\n"
    "  var T='click dblclick mousedown mouseup mousemove mouseover mouseout mouseenter mouseleave contextmenu wheel keydown keyup keypress input beforeinput change submit reset focus blur focusin focusout load unload beforeunload error abort scroll resize pointerdown pointerup pointermove pointercancel pointerover pointerout pointerenter pointerleave pointerlockchange pointerlockerror touchstart touchmove touchend touchcancel animationstart animationend animationiteration transitionend transitionrun transitionstart transitioncancel copy cut paste drag dragstart dragend dragenter dragover dragleave drop dragexit compositionstart compositionupdate compositionend DOMContentLoaded readystatechange visibilitychange fullscreenchange fullscreenerror hashchange popstate storage message messageerror selectionchange selectstart online offline gamepadconnected gamepaddisconnected'.split(' ');\n"
    "  function defOn(proto){ T.forEach(function(t){ Object.defineProperty(proto, 'on'+t, { configurable:true,\n"
    "    get:function(){ var h=this['__on_'+t]; return typeof h==='function'?h:null; },\n"
    "    set:function(v){ var k='__on_'+t, prev=this[k]; if(typeof prev==='function' && typeof this.removeEventListener==='function'){ try{ this.removeEventListener(t, prev); }catch(e){} } this[k]=null; if(typeof v==='function'){ this[k]=v; if(typeof this.addEventListener==='function') this.addEventListener(t, v); } } }); }); }\n"
    "  defOn(MiniElement.prototype);\n"
    "  if(typeof MiniDocument!=='undefined'){ defOn(MiniDocument.prototype); }\n"
    "  var WT='load error resize scroll message hashchange popstate beforeunload unload online offline storage focus blur'.split(' ');\n"
    "  WT.forEach(function(t){ Object.defineProperty(window, 'on'+t, { configurable:true,\n"
    "    get:function(){ var h=window['__onw_'+t]; return typeof h==='function'?h:null; },\n"
    "    set:function(v){ var k='__onw_'+t, p=window[k]; if(typeof p==='function'){ try{ window.removeEventListener(t,p); }catch(e){} } window[k]=null; if(typeof v==='function'){ window[k]=v; window.addEventListener(t,v); } } }); });\n"
    "})();\n"
    "var p=WebGLRenderingContext.prototype;\n"
    /* Full WebGL 1.0 constant surface. Previously only a handful were defined,
       so gl.TRIANGLE_STRIP resolved to `undefined` and coerced to mode=0
       (GL_INVALID_ENUM) in drawArrays -> the screen stayed black even though
       shaders compiled and pointers resolved. */
    "  p.VERTEX_SHADER=0x8B31; p.FRAGMENT_SHADER=0x8B30;\n"
    "  p.ARRAY_BUFFER=0x8892; p.ELEMENT_ARRAY_BUFFER=0x8893;\n"
    "  p.STREAM_DRAW=0x88E0; p.STATIC_DRAW=0x88E4; p.DYNAMIC_DRAW=0x88E8;\n"
    "  p.BYTE=0x1400; p.UNSIGNED_BYTE=0x1401; p.SHORT=0x1402; p.UNSIGNED_SHORT=0x1403; p.INT=0x1404; p.UNSIGNED_INT=0x1405; p.FLOAT=0x1406;\n"
    "  p.POINTS=0x0000; p.LINES=0x0001; p.LINE_LOOP=0x0002; p.LINE_STRIP=0x0003; p.TRIANGLES=0x0004; p.TRIANGLE_STRIP=0x0005; p.TRIANGLE_FAN=0x0006;\n"
    "  p.COLOR_BUFFER_BIT=0x4000; p.DEPTH_BUFFER_BIT=0x0100; p.STENCIL_BUFFER_BIT=0x0400;\n"
    "  p.ZERO=0; p.ONE=1; p.SRC_COLOR=0x0300; p.ONE_MINUS_SRC_COLOR=0x0301; p.SRC_ALPHA=0x0302; p.ONE_MINUS_SRC_ALPHA=0x0303; p.DST_ALPHA=0x0304; p.ONE_MINUS_DST_ALPHA=0x0305; p.DST_COLOR=0x0306; p.ONE_MINUS_DST_COLOR=0x0307; p.SRC_ALPHA_SATURATE=0x0308; p.CONSTANT_COLOR=0x8001; p.ONE_MINUS_CONSTANT_COLOR=0x8002; p.CONSTANT_ALPHA=0x8003; p.ONE_MINUS_CONSTANT_ALPHA=0x8004;\n"
    "  p.FUNC_ADD=0x8006; p.FUNC_SUBTRACT=0x800A; p.FUNC_REVERSE_SUBTRACT=0x800B;\n"
    "  p.BLEND=0x0BE2; p.DEPTH_TEST=0x0B71; p.CULL_FACE=0x0B44; p.SCISSOR_TEST=0x0C11; p.DITHER=0x0BD0; p.POLYGON_OFFSET_FILL=0x8037; p.SAMPLE_ALPHA_TO_COVERAGE=0x8098; p.SAMPLE_COVERAGE=0x80A0;\n"
    "  p.NEVER=0x0200; p.LESS=0x0201; p.EQUAL=0x0202; p.LEQUAL=0x0203; p.GREATER=0x0204; p.NOTEQUAL=0x0205; p.GEQUAL=0x0206; p.ALWAYS=0x0207;\n"
    "  p.FRONT=0x0404; p.BACK=0x0405; p.FRONT_AND_BACK=0x0408;\n"
    "  p.TEXTURE0=0x84C0; p.TEXTURE1=0x84C1; p.TEXTURE2=0x84C2; p.TEXTURE3=0x84C3; p.TEXTURE4=0x84C4; p.TEXTURE5=0x84C5; p.TEXTURE6=0x84C6; p.TEXTURE7=0x84C7; p.TEXTURE8=0x84C8; p.TEXTURE9=0x84C9;\n"
    "  p.TEXTURE_2D=0x0DE1; p.TEXTURE_CUBE_MAP=0x8513; p.TEXTURE_BINDING_2D=0x8069;\n"
    "  p.ALPHA=0x1906; p.RGB=0x1907; p.RGBA=0x1908; p.LUMINANCE=0x1909; p.LUMINANCE_ALPHA=0x190A;\n"
    "  p.TEXTURE_MIN_FILTER=0x2801; p.TEXTURE_MAG_FILTER=0x2800; p.TEXTURE_WRAP_S=0x2802; p.TEXTURE_WRAP_T=0x2803;\n"
    "  p.NEAREST=0x2600; p.LINEAR=0x2601;\n"
    "  p.NEAREST_MIPMAP_NEAREST=0x2700; p.LINEAR_MIPMAP_NEAREST=0x2701; p.NEAREST_MIPMAP_LINEAR=0x2702; p.LINEAR_MIPMAP_LINEAR=0x2703;\n"
    "  p.CLAMP_TO_EDGE=0x812F; p.REPEAT=0x2901; p.MIRRORED_REPEAT=0x8370;\n"
    "  p.UNPACK_FLIP_Y_WEBGL=0x9240; p.UNPACK_PREMULTIPLY_ALPHA_WEBGL=0x9241; p.UNPACK_COLORSPACE_CONVERSION_WEBGL=0x9243;\n"
    "  p.COMPILE_STATUS=0x8B81; p.LINK_STATUS=0x8B82; p.ACTIVE_UNIFORMS=0x8B86; p.ACTIVE_ATTRIBUTES=0x8B89;\n"
    "  p.MAX_VERTEX_ATTRIBS=0x8869; p.MAX_TEXTURE_SIZE=0x0D33; p.MAX_VIEWPORT_DIMS=0x0D3A;\n"
    "  p.NO_ERROR=0; p.INVALID_ENUM=0x0500; p.INVALID_VALUE=0x0501; p.INVALID_OPERATION=0x0502; p.OUT_OF_MEMORY=0x0505;\n"
    "  p.FRAMEBUFFER=0x8D40; p.RENDERBUFFER=0x8D41; p.COLOR_ATTACHMENT0=0x8CE0; p.DEPTH_ATTACHMENT=0x8D00; p.FRAMEBUFFER_COMPLETE=0x8CD5;\n"
    /* String- and limit-valued parameters Three.js queries via gl.getParameter
       during WebGLState / WebGLCapabilities setup. Previously these resolved
       to `undefined`, so gl.getParameter(gl.VERSION) coerced the missing enum
       to 0 and returned the *number* 0; Three.js then did
       glVersion.indexOf('WebGL') -> (0).indexOf -> "not a function", aborting
       WebGLRenderer construction. Defining the real GL enum values lets
       js_gl_getParameter route them to glGetString (VERSION/etc.) or
       glGetIntegerv (the MAX_* / box queries). */
    "  p.VENDOR=0x1F00; p.RENDERER=0x1F01; p.VERSION=0x1F02; p.SHADING_LANGUAGE_VERSION=0x8B8C;\n"
    "  p.MAX_TEXTURE_IMAGE_UNITS=0x8872; p.MAX_VERTEX_TEXTURE_IMAGE_UNITS=0x8B4C; p.MAX_COMBINED_TEXTURE_IMAGE_UNITS=0x8B4D;\n"
    "  p.MAX_VERTEX_UNIFORM_VECTORS=0x8DFB; p.MAX_VARYING_VECTORS=0x8DFC; p.MAX_FRAGMENT_UNIFORM_VECTORS=0x8DFD;\n"
    "  p.MAX_CUBE_MAP_TEXTURE_SIZE=0x851C; p.MAX_SAMPLES=0x9136;\n"
    "  p.SCISSOR_BOX=0x0C10; p.VIEWPORT=0x0BA2;\n"
    "  p.ALIASED_POINT_SIZE_RANGE=0x846D; p.ALIASED_LINE_WIDTH_RANGE=0x846E;\n"
    "  p.DEPTH_COMPONENT=0x1902; p.DEPTH_STENCIL=0x84F9; p.DEPTH_COMPONENT16=0x81A5; p.DEPTH_COMPONENT24=0x81A6; p.DEPTH_COMPONENT32F=0x8CAC; p.DEPTH24_STENCIL8=0x88F0; p.DEPTH_STENCIL_ATTACHMENT=0x821A; p.STENCIL_ATTACHMENT=0x8D20; p.STENCIL_INDEX8=0x8D48; p.RGBA4=0x8056; p.RGB5_A1=0x8057; p.RGB565=0x8D62; p.RGBA8=0x8058; p.RGB8=0x8051; p.NONE=0;\n"
    "  p.UNSIGNED_SHORT_5_6_5=0x8363; p.UNSIGNED_SHORT_4_4_4_4=0x8033; p.UNSIGNED_SHORT_5_5_5_1=0x8034;\n"
    "  p.CURRENT_PROGRAM=0x8B8D; p.GENERATE_MIPMAP_HINT=0x8192; p.COMPRESSED_TEXTURE_FORMATS=0x86A3;\n"
    /* Shader precision enums: Three.js queries gl.getShaderPrecisionFormat
       with these to decide whether highp/mediump are usable. Without them,
       gl.HIGH_FLOAT etc. are `undefined`, the C bridge receives precType=0,
       falls through to precision=0, and Three.js degrades to lowp -> its
       MeshStandardMaterial lighting underflows to a black cube. */
    "  p.LOW_FLOAT=0x8DF0; p.MEDIUM_FLOAT=0x8DF1; p.HIGH_FLOAT=0x8DF2;\n"
    "  p.LOW_INT=0x8DF4; p.MEDIUM_INT=0x8DF5; p.HIGH_INT=0x8DF6;\n"
    "  for(var k in p){ WebGLRenderingContext[k] = p[k]; }\n"
    "console = {\n"
    "  log(){   __emit.apply(null, ['log'].concat([].slice.call(arguments))); },\n"
    "  info(){  __emit.apply(null, ['info'].concat([].slice.call(arguments))); },\n"
    "  debug(){ __emit.apply(null, ['debug'].concat([].slice.call(arguments))); },\n"
    "  trace(){ __emit.apply(null, ['trace'].concat([].slice.call(arguments))); },\n"
    "  warn(){  __emit.apply(null, ['warning'].concat([].slice.call(arguments))); },\n"
    "  error(){ __emit.apply(null, ['error'].concat([].slice.call(arguments))); }\n"
    "};\n"
    "var __fmt = function(a){ try { return typeof a==='object' ? JSON.stringify(a) : String(a); } catch(e){ return String(a); } };\n"
    "var __emit = function(lvl){ var s=''; for (var i=1;i<arguments.length;i++) s += (i>1?' ':'') + __fmt(arguments[i]); __log(lvl, s); };\n"
    "function __thenable(v){ if(v && typeof v.then==='function') return v; return Promise.resolve(v); };\n"
    "/* Web Audio API standard polyfill */\n"
    "(function(){\n"
    "  if(typeof window==='undefined'||window.AudioContext||window.webkitAudioContext) return;\n"
    "  function AudioParam(defVal){ this.value = (defVal!==undefined)?defVal:0; this._events=[]; }\n"
    "  AudioParam.prototype.setValueAtTime = function(v, t){ this.value=v; this._events.push({type:'set', value:v, time:t}); return this; };\n"
    "  AudioParam.prototype.linearRampToValueAtTime = function(v, t){ this._events.push({type:'linear', value:v, time:t}); return this; };\n"
    "  AudioParam.prototype.exponentialRampToValueAtTime = function(v, t){ this._events.push({type:'exp', value:v, time:t}); return this; };\n"
    "  AudioParam.prototype._valAt = function(t){\n"
    "    var v = this.value;\n"
    "    for(var i=0; i<this._events.length; i++){\n"
    "      var ev = this._events[i];\n"
    "      if(t >= ev.time) v = ev.value;\n"
    "      else if(ev.type==='linear' && i>0){\n"
    "        var prev = this._events[i-1];\n"
    "        if(t >= prev.time && t < ev.time){\n"
    "          var frac = (t - prev.time)/(ev.time - prev.time);\n"
    "          v = prev.value + (ev.value - prev.value) * frac;\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "    return v;\n"
    "  };\n"
    "  function AudioBuffer(channels, length, sampleRate){\n"
    "    this.numberOfChannels = channels || 1; this.length = length || 0; this.sampleRate = sampleRate || 44100; this.duration = this.length / this.sampleRate; this._channels = [];\n"
    "    for(var i=0; i<this.numberOfChannels; i++) this._channels.push(new Float32Array(this.length));\n"
    "  }\n"
    "  AudioBuffer.prototype.getChannelData = function(ch){ return this._channels[ch||0]; };\n"
    "  function AudioNode(ctx){ this.context = ctx; this._connections = []; }\n"
    "  AudioNode.prototype.connect = function(dest){ if(this._connections.indexOf(dest)===-1) this._connections.push(dest); return dest; };\n"
    "  AudioNode.prototype.disconnect = function(){ this._connections = []; };\n"
    "  function GainNode(ctx){ AudioNode.call(this, ctx); this.gain = new AudioParam(1.0); }\n"
    "  GainNode.prototype = Object.create(AudioNode.prototype);\n"
    "  function BiquadFilterNode(ctx){ AudioNode.call(this, ctx); this.type = 'lowpass'; this.frequency = new AudioParam(350); this.Q = new AudioParam(1); this.gain = new AudioParam(0); }\n"
    "  BiquadFilterNode.prototype = Object.create(AudioNode.prototype);\n"
    "  function OscillatorNode(ctx){\n"
    "    AudioNode.call(this, ctx); this.type = 'sine'; this.frequency = new AudioParam(440); this.detune = new AudioParam(0);\n"
    "    this.startTime = -1; this.stopTime = -1; this._phase = 0; this.context._activeSources.push(this);\n"
    "  }\n"
    "  OscillatorNode.prototype = Object.create(AudioNode.prototype);\n"
    "  OscillatorNode.prototype.start = function(when){ this.startTime = (when!==undefined)?when:this.context.currentTime; };\n"
    "  OscillatorNode.prototype.stop = function(when){ this.stopTime = (when!==undefined)?when:this.context.currentTime; };\n"
    "  function AudioBufferSourceNode(ctx){\n"
    "    AudioNode.call(this, ctx); this.buffer = null; this.playbackRate = new AudioParam(1.0); this.loop = false; this.loopStart = 0; this.loopEnd = 0;\n"
    "    this.startTime = -1; this.stopTime = -1; this._offset = 0; this.context._activeSources.push(this);\n"
    "  }\n"
    "  AudioBufferSourceNode.prototype = Object.create(AudioNode.prototype);\n"
    "  AudioBufferSourceNode.prototype.start = function(when, offset){ this.startTime = (when!==undefined)?when:this.context.currentTime; this._offset = offset || 0; };\n"
    "  AudioBufferSourceNode.prototype.stop = function(when){ this.stopTime = (when!==undefined)?when:this.context.currentTime; };\n"
    "  function AudioDestinationNode(ctx){ AudioNode.call(this, ctx); }\n"
    "  AudioDestinationNode.prototype = Object.create(AudioNode.prototype);\n"
    "  function AudioContext(){\n"
    "    this.sampleRate = 44100; this.currentTime = 0; this.state = 'running'; this.destination = new AudioDestinationNode(this); this._activeSources = [];\n"
    "    var self = this;\n"
    "    if(typeof __mini_audio_init === 'function') __mini_audio_init(this.sampleRate, 1);\n"
    "    this._timer = setInterval(function(){ self._tick(); }, 30);\n"
    "  }\n"
    "  AudioContext.prototype.resume = function(){ this.state='running'; return Promise.resolve(); };\n"
    "  AudioContext.prototype.suspend = function(){ this.state='suspended'; return Promise.resolve(); };\n"
    "  AudioContext.prototype.close = function(){ this.state='closed'; if(this._timer) clearInterval(this._timer); return Promise.resolve(); };\n"
    "  AudioContext.prototype.createOscillator = function(){ return new OscillatorNode(this); };\n"
    "  AudioContext.prototype.createGain = function(){ return new GainNode(this); };\n"
    "  AudioContext.prototype.createBuffer = function(c,l,s){ return new AudioBuffer(c, l, s||this.sampleRate); };\n"
    "  AudioContext.prototype.createBufferSource = function(){ return new AudioBufferSourceNode(this); };\n"
    "  AudioContext.prototype.createBiquadFilter = function(){ return new BiquadFilterNode(this); };\n"
    "  AudioContext.prototype._tick = function(){\n"
    "    if(this.state !== 'running') return;\n"
    "    var chunkSize = Math.floor(this.sampleRate * 0.03);\n"
    "    var now = this.currentTime; var dt = 1.0 / this.sampleRate;\n"
    "    var out = new Float32Array(chunkSize); var active = false;\n"
    "    function getGain(node, t){\n"
    "      var g = 1.0;\n"
    "      for(var i=0; i<node._connections.length; i++){\n"
    "        var c = node._connections[i];\n"
    "        if(c instanceof GainNode) g *= c.gain._valAt(t) * getGain(c, t);\n"
    "      }\n"
    "      return g;\n"
    "    }\n"
    "    for(var sIdx = this._activeSources.length-1; sIdx >= 0; sIdx--){\n"
    "      var src = this._activeSources[sIdx];\n"
    "      if(src.startTime < 0 || now < src.startTime) continue;\n"
    "      if(src.stopTime >= 0 && now >= src.stopTime){ this._activeSources.splice(sIdx, 1); continue; }\n"
    "      active = true;\n"
    "      if(src instanceof OscillatorNode){\n"
    "        for(var i=0; i<chunkSize; i++){\n"
    "          var t = now + i * dt;\n"
    "          if(src.stopTime >= 0 && t >= src.stopTime) break;\n"
    "          var freq = src.frequency._valAt(t);\n"
    "          src._phase += 2.0 * Math.PI * freq * dt;\n"
    "          var val = Math.sin(src._phase);\n"
    "          if(src.type==='square') val = Math.sin(src._phase) >= 0 ? 1 : -1;\n"
    "          else if(src.type==='sawtooth') val = 2.0 * (src._phase/(2*Math.PI) - Math.floor(src._phase/(2*Math.PI) + 0.5));\n"
    "          else if(src.type==='triangle') val = 2.0 * Math.abs(2.0*(src._phase/(2*Math.PI) - Math.floor(src._phase/(2*Math.PI)+0.5))) - 1.0;\n"
    "          out[i] += val * getGain(src, t);\n"
    "        }\n"
    "      } else if(src instanceof AudioBufferSourceNode && src.buffer){\n"
    "        var data = src.buffer.getChannelData(0);\n"
    "        for(var i=0; i<chunkSize; i++){\n"
    "          var t = now + i * dt;\n"
    "          if(src.stopTime >= 0 && t >= src.stopTime) break;\n"
    "          var sPos = Math.floor(src._offset);\n"
    "          if(sPos < data.length){\n"
    "            out[i] += data[sPos] * getGain(src, t);\n"
    "            src._offset += src.playbackRate._valAt(t);\n"
    "          } else {\n"
    "            if(src.loop) src._offset = src.loopStart || 0;\n"
    "            else { src.stopTime = t; break; }\n"
    "          }\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "    this.currentTime += chunkSize * dt;\n"
    "    if(active && typeof __mini_audio_queue_pcm === 'function') __mini_audio_queue_pcm(out);\n"
    "  };\n"
    "  window.AudioContext = AudioContext;\n"
    "  window.webkitAudioContext = AudioContext;\n"
    "})();\n";

/* ================================================================== */
/* Bridge                                                              */
/* ================================================================== */
typedef struct MiniTimer
{
    JSValue cb;
    double due_ms;
    double interval_ms; /* >0 => recurring (setInterval) */
    int id;             /* stable handle returned to JS for clearTimeout/clearInterval */
    int recurring;
} MiniTimer;

/* rAF queue entry: stores the handle id alongside the callback so
   cancelAnimationFrame(id) can match and drop a pending frame. */
typedef struct MiniRafEntry
{
    JSValue cb;
    int id;
} MiniRafEntry;

/* A JS-backed event listener: mini_events.c dispatches a capture/target/
   bubble pass and calls the trampoline (which re-enters JS) per matching
   listener. `b` is an incomplete pointer here; the trampoline is defined
   after MiniBridge.                                                    */
typedef struct
{
    struct MiniBridge *b;
    JSValue cb;
    MiniEventListener *handle; /* back-pointer to the C listener slot */
    JSValue js_target;
} JsEvListener;

typedef struct MiniBridge
{
    JSRuntime *rt;
    JSContext *ctx;
    MiniRenderer *r;
    MiniDocument *doc;
    MiniGLBridge *gl;

    JSClassID el_cid;
    JSClassID doc_cid;

    /* requestAnimationFrame queue */
    MiniRafEntry *raf;
    int raf_n, raf_cap;
    int raf_next_id;

    /* setTimeout timers */
    MiniTimer *timers;
    int tm_n, tm_cap;
    int tm_next_id;

    /* console relay hook (e.g. to CDP). NULL = no relay. */
    void (*log_hook)(const char *level, const char *msg, void *ud);
    void *log_ud;

    /* W3C event system (Stage 2 mini_events.c). The host loop drives it via
       mini_events_handle_*; JS addEventListener registers a trampoline into
       it so JS callbacks fire on capture/target/bubble. ev_listeners owns
       the per-listener {cb} JSValues so they're freed on bridge destroy.   */
    MiniEventState *ev;
    JsEvListener **ev_listeners;
    int ev_listeners_n, ev_listeners_cap;

    /* WebSocket class + live sockets (pumped each frame; onclose sweeps). */
    JSClassID ws_cid;
    struct JsWebSocket **ws_list;
    int ws_n, ws_cap;

    /* ES module loader: bare-specifier import map (from <script type=importmap>)
       as parallel key/value arrays (key exact or trailing-slash prefix), and
       the page URL used as the inline module's base. */
    char **im_keys, **im_vals;
    int im_n, im_cap;
    char *doc_url; /* page URL (file:// for local); base for relative imports */
    struct MiniNode *locked_node;

    JSValue **all_wrappers;
    int all_wrappers_n, all_wrappers_cap;
} MiniBridge;

/* ES module loader callbacks (defined later; registered in mini_bridge_create so
   JS_SetModuleLoaderFunc can take them before their definitions appear). */
static char *mini_module_normalize(JSContext *ctx, const char *base,
                                   const char *name, void *opaque);
static JSModuleDef *mini_module_loader(JSContext *ctx, const char *module_name,
                                       void *opaque);

/* element finalizer: document owns the tree; wrapper is just a handle. */
static void js_el_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    (void)val;
}

/* ---- helpers ---- */
static MiniBridge *g_active_js_bridge = NULL;

void mini_bridge_on_node_destroyed(struct MiniNode *n)
{
    if (!n)
        return;
    if (g_active_js_bridge)
    {
        MiniBridge *b = g_active_js_bridge;
        for (int i = 0; i < b->ev_listeners_n; )
        {
            JsEvListener *L = b->ev_listeners[i];
            if (L && (L->handle == NULL || L->handle->target == n))
            {
                if (b->ev && L->handle)
                    mini_events_remove_listener(b->ev, L->handle);
                JS_FreeValue(b->ctx, L->cb);
                JS_FreeValue(b->ctx, L->js_target);
                free(L);
                for (int j = i; j < b->ev_listeners_n - 1; j++)
                    b->ev_listeners[j] = b->ev_listeners[j + 1];
                b->ev_listeners_n--;
            }
            else
            {
                i++;
            }
        }
    }
    if (n->js_wrapper)
    {
        JSValue *v = (JSValue *)n->js_wrapper;
        if (g_active_js_bridge && g_active_js_bridge->ctx)
        {
            JS_SetOpaque(*v, NULL);
            JS_FreeValue(g_active_js_bridge->ctx, *v);

            for (int i = 0; i < g_active_js_bridge->all_wrappers_n; i++) {
                if (g_active_js_bridge->all_wrappers[i] == v) {
                    g_active_js_bridge->all_wrappers[i] = NULL;
                }
            }
        }
        free(v);
        n->js_wrapper = NULL;
    }
}

static MiniBridge *bridge_of(JSContext *ctx)
{
    return (MiniBridge *)JS_GetContextOpaque(ctx);
}

static JSValue wrap_node(JSContext *ctx, struct MiniNode *n, JSClassID cid)
{
    MiniBridge *b = bridge_of(ctx);
    if (!n)
        return JS_NULL;
    if (n->js_wrapper)
    {
        JSValue *cached = (JSValue *)n->js_wrapper;
        return JS_DupValue(ctx, *cached);
    }
    JSValue obj = JS_NewObjectClass(ctx, cid);
    if (JS_IsException(obj))
        return obj;
    JS_SetOpaque(obj, n);
    JSValue *saved = (JSValue *)malloc(sizeof(JSValue));
    if (saved)
    {
        *saved = JS_DupValue(ctx, obj);
        n->js_wrapper = saved;

        if (b) {
            if (b->all_wrappers_n >= b->all_wrappers_cap) {
                int nc = b->all_wrappers_cap ? b->all_wrappers_cap * 2 : 1024;
                JSValue **nw = (JSValue **)realloc(b->all_wrappers, nc * sizeof(JSValue *));
                if (nw) { b->all_wrappers = nw; b->all_wrappers_cap = nc; }
            }
            if (b->all_wrappers_n < b->all_wrappers_cap) {
                b->all_wrappers[b->all_wrappers_n++] = saved;
            }
        }
    }
    return obj;
}

static struct MiniNode *el_this(JSContext *ctx, JSValueConst this_val, JSClassID cid)
{
    return (struct MiniNode *)JS_GetOpaque2(ctx, this_val, cid);
}

static void maybe_execute_script(MiniBridge *b, struct MiniNode *child, JSValueConst child_val)
{
    if (!b || !child || !child->tag)
        return;

    if (!strcmp(child->tag, "style"))
    {
        const char *css = child->text;
        if (!css && child->first_child && child->first_child->text)
            css = child->first_child->text;
        if (css && css[0] && b->doc)
        {
            mini_css_apply(b->doc, css);
            b->doc->dirty = 1;
            b->doc->paint_dirty = 1;
        }
        return;
    }

    if (strcmp(child->tag, "script"))
        return;

    const char *src = mini_node_get_attribute(child, "src");
    char *src_from_js = NULL;
    if (!src || !src[0])
    {
        JSValue jsrc = JS_GetPropertyStr(b->ctx, child_val, "src");
        if (JS_IsString(jsrc))
        {
            const char *cstr = JS_ToCString(b->ctx, jsrc);
            if (cstr && cstr[0])
            {
                src_from_js = strdup(cstr);
                src = src_from_js;
                mini_node_set_attribute(child, "src", src);
            }
            if (cstr) JS_FreeCString(b->ctx, cstr);
        }
        JS_FreeValue(b->ctx, jsrc);
    }
    char *code = NULL;
    size_t code_len = 0;
    int is_free_code = 0;

    if (src && src[0])
    {
        if (!strncmp(src, "http://", 7) || !strncmp(src, "https://", 8))
        {
            MiniNetRecord rec;
            memset(&rec, 0, sizeof rec);
            if (mini_net_fetch("GET", src, NULL, NULL, 0, NULL, &rec) == 0 && rec.resp_body)
            {
                code = rec.resp_body;
                code_len = rec.resp_body_len;
                is_free_code = 1;
                rec.resp_body = NULL;
                mini_net_record_add(&rec);
            }
            else
            {
                mini_net_record_free(&rec);
            }
        }
        else
        {
            const char *fpath = src;
            if (!strncmp(fpath, "file:///", 8)) fpath += 8;
            else if (!strncmp(fpath, "file://", 7)) fpath += 7;
            FILE *fp = fopen(fpath, "rb");
            if (fp)
            {
                fseek(fp, 0, SEEK_END);
                long sz = ftell(fp);
                fseek(fp, 0, SEEK_SET);
                if (sz > 0)
                {
                    code = (char *)malloc(sz + 1);
                    if (code)
                    {
                        size_t r = fread(code, 1, sz, fp);
                        code[r] = 0;
                        code_len = r;
                        is_free_code = 1;
                    }
                }
                fclose(fp);
            }
        }
    }
    else if (child->text && child->text[0])
    {
        code = child->text;
        code_len = strlen(child->text);
    }
    else if (child->first_child && child->first_child->text)
    {
        code = child->first_child->text;
        code_len = strlen(child->first_child->text);
    }

    if (code)
    {
        const char *script_name = (src && src[0]) ? src : "<dynamic_script>";
        JSValue res = JS_Eval(b->ctx, code, code_len, script_name, JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(res))
        {
            JSValue exc = JS_GetException(b->ctx);
            const char *err = JS_ToCString(b->ctx, exc);
            fprintf(stderr, "[Script Error] %s: %s\n", script_name, err ? err : "unknown");
            if (err) JS_FreeCString(b->ctx, err);
            JS_FreeValue(b->ctx, exc);

            JSValue onerror = JS_GetPropertyStr(b->ctx, child_val, "onerror");
            if (JS_IsFunction(b->ctx, onerror))
            {
                JSValue ev_obj = JS_NewObject(b->ctx);
                JS_SetPropertyStr(b->ctx, ev_obj, "type", JS_NewString(b->ctx, "error"));
                JS_SetPropertyStr(b->ctx, ev_obj, "target", JS_DupValue(b->ctx, child_val));
                JSValue r_call = JS_Call(b->ctx, onerror, child_val, 1, &ev_obj);
                JS_FreeValue(b->ctx, r_call);
                JS_FreeValue(b->ctx, ev_obj);
            }
            JS_FreeValue(b->ctx, onerror);
        }
        else
        {
            JSValue onload = JS_GetPropertyStr(b->ctx, child_val, "onload");
            if (JS_IsFunction(b->ctx, onload))
            {
                JSValue ev_obj = JS_NewObject(b->ctx);
                JS_SetPropertyStr(b->ctx, ev_obj, "type", JS_NewString(b->ctx, "load"));
                JS_SetPropertyStr(b->ctx, ev_obj, "target", JS_DupValue(b->ctx, child_val));
                JSValue r_call = JS_Call(b->ctx, onload, child_val, 1, &ev_obj);
                JS_FreeValue(b->ctx, r_call);
                JS_FreeValue(b->ctx, ev_obj);
            }
            JS_FreeValue(b->ctx, onload);

            JSValue disp = JS_GetPropertyStr(b->ctx, child_val, "dispatchEvent");
            if (JS_IsFunction(b->ctx, disp))
            {
                JSValue ev_obj = JS_NewObject(b->ctx);
                JS_SetPropertyStr(b->ctx, ev_obj, "type", JS_NewString(b->ctx, "load"));
                JS_SetPropertyStr(b->ctx, ev_obj, "target", JS_DupValue(b->ctx, child_val));
                JSValue r_call = JS_Call(b->ctx, disp, child_val, 1, &ev_obj);
                JS_FreeValue(b->ctx, r_call);
                JS_FreeValue(b->ctx, ev_obj);
            }
            JS_FreeValue(b->ctx, disp);
        }
        JS_FreeValue(b->ctx, res);
        if (is_free_code && code)
            free(code);
    }
    else if (src && src[0])
    {
        JSValue onerror = JS_GetPropertyStr(b->ctx, child_val, "onerror");
        if (JS_IsFunction(b->ctx, onerror))
        {
            JSValue ev_obj = JS_NewObject(b->ctx);
            JS_SetPropertyStr(b->ctx, ev_obj, "type", JS_NewString(b->ctx, "error"));
            JS_SetPropertyStr(b->ctx, ev_obj, "target", JS_DupValue(b->ctx, child_val));
            JSValue r_call = JS_Call(b->ctx, onerror, child_val, 1, &ev_obj);
            JS_FreeValue(b->ctx, r_call);
            JS_FreeValue(b->ctx, ev_obj);
        }
        JS_FreeValue(b->ctx, onerror);
    }
    if (src_from_js)
        free(src_from_js);
}

/* ----------------------------------------------------------------- */
/* Element methods                                                    */
/* ----------------------------------------------------------------- */
static JSValue js_appendChild(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *parent = el_this(ctx, tv, b->el_cid);
    struct MiniNode *child = el_this(ctx, argv[0], b->el_cid);
    if (!parent || !child)
        return JS_ThrowTypeError(ctx, "appendChild: bad args");
    mini_node_append_child(parent, child);
    maybe_execute_script(b, child, argv[0]);
    return JS_DupValue(ctx, argv[0]);
}
static JSValue js_removeChild(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *parent = el_this(ctx, tv, b->el_cid);
    struct MiniNode *child = el_this(ctx, argv[0], b->el_cid);
    if (!parent || !child)
        return JS_ThrowTypeError(ctx, "removeChild: bad args");
    mini_node_remove_child(parent, child);
    return JS_DupValue(ctx, argv[0]);
}
static JSValue js_setAttribute(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = el_this(ctx, tv, b->el_cid);
    if (!n)
        return JS_ThrowTypeError(ctx, "setAttribute: not an element");
    const char *k = JS_ToCString(ctx, argv[0]);
    const char *v = JS_ToCString(ctx, argv[1]);
    if (!k || !v)
    {
        JS_FreeCString(ctx, k);
        JS_FreeCString(ctx, v);
        return JS_EXCEPTION;
    }
    mini_node_set_attribute(n, k, v);
    if (!strcmp(k, "class") || !strcmp(k, "id"))
    {
        if (b->doc) mini_dom_restyle(b->doc);
    }
    if (!strcmp(k, "style"))
    {
        /* inline CSS: "color:red;background:#000;font-size:14px" */
        char buf[256];
        strncpy(buf, v, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        char *tok = strtok(buf, ";");
        while (tok)
        {
            char *colon = strchr(tok, ':');
            if (colon)
            {
                *colon++ = 0;
                while (*tok == ' ')
                    tok++;
                while (*colon == ' ')
                    colon++;
                mini_style_set(n, tok, colon);
            }
            tok = strtok(NULL, ";");
        }
    }
    JS_FreeCString(ctx, k);
    JS_FreeCString(ctx, v);
    return JS_UNDEFINED;
}
static JSValue js_getAttribute(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = el_this(ctx, tv, b->el_cid);
    if (!n)
        return JS_ThrowTypeError(ctx, "getAttribute: not an element");
    const char *k = JS_ToCString(ctx, argv[0]);
    const char *v = mini_node_get_attribute(n, k);
    JS_FreeCString(ctx, k);
    return v ? JS_NewString(ctx, v) : JS_NULL;
}
static JSValue js_setStyle(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = el_this(ctx, tv, b->el_cid);
    const char *p = JS_ToCString(ctx, argv[0]);
    const char *v = JS_ToCString(ctx, argv[1]);
    if (n && p && v)
    {
        /* Convert camelCase to kebab-case if needed */
        char prop_buf[128];
        int pi = 0;
        for (int i = 0; p[i] && pi < (int)sizeof(prop_buf) - 2; i++)
        {
            if (p[i] >= 'A' && p[i] <= 'Z')
            {
                prop_buf[pi++] = '-';
                prop_buf[pi++] = (char)(p[i] + ('a' - 'A'));
            }
            else
            {
                prop_buf[pi++] = p[i];
            }
        }
        prop_buf[pi] = '\0';
        mini_style_set_base(n, prop_buf, v);
        mini_style_set(n, prop_buf, v);
        n->dirty_layout = 1;
        n->dirty_paint = 1;
        if (b->doc)
        {
            b->doc->dirty = 1;
            b->doc->paint_dirty = 1;
        }
    }
    JS_FreeCString(ctx, p);
    JS_FreeCString(ctx, v);
    return JS_UNDEFINED;
}
static JSValue js_get_inner_width(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    (void)argc;
    (void)argv;
    MiniBridge *b = bridge_of(ctx);
    int w = 1280;
    if (b && b->doc && b->doc->viewport_w > 0)
        w = b->doc->viewport_w;
    else if (b && b->r && b->r->gpu.width > 0)
        w = b->r->gpu.width;
    return JS_NewInt32(ctx, w);
}
static JSValue js_get_inner_height(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    (void)argc;
    (void)argv;
    MiniBridge *b = bridge_of(ctx);
    int h = 720;
    if (b && b->doc && b->doc->viewport_h > 0)
        h = b->doc->viewport_h;
    else if (b && b->r && b->r->gpu.height > 0)
        h = b->r->gpu.height;
    return JS_NewInt32(ctx, h);
}
static JSValue js_get_device_pixel_ratio(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    (void)argc;
    (void)argv;
    MiniBridge *b = bridge_of(ctx);
    float dpr = 1.0f;
    if (b && b->r && b->r->gpu.window_handle)
    {
        GLFWwindow *win = (GLFWwindow *)b->r->gpu.window_handle;
        int fw = 0, fh = 0, ww = 0, wh = 0;
        glfwGetFramebufferSize(win, &fw, &fh);
        glfwGetWindowSize(win, &ww, &wh);
        if (ww > 0 && fw > 0)
            dpr = (float)fw / (float)ww;
    }
    return JS_NewFloat64(ctx, (double)dpr);
}
static JSValue js_get_scroll_x(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    (void)argc;
    (void)argv;
    MiniBridge *b = bridge_of(ctx);
    return JS_NewFloat64(ctx, (b && b->doc) ? (double)b->doc->scroll_x : 0.0);
}
static JSValue js_get_scroll_y(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    (void)argc;
    (void)argv;
    MiniBridge *b = bridge_of(ctx);
    return JS_NewFloat64(ctx, (b && b->doc) ? (double)b->doc->scroll_y : 0.0);
}

static uint8_t *js_bridge_base64_decode(const char *src, size_t len, size_t *out_len)
{
    if (!src || !out_len)
        return NULL;
    static const int8_t b64_table[256] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, 0, -1, -1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
        -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    size_t cap = (len / 4 + 1) * 3 + 4;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf)
        return NULL;
    size_t out_n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)src[i];
        if (c == '=')
            break;
        int8_t val = b64_table[c];
        if (val < 0)
            continue;
        acc = (acc << 6) | (uint32_t)val;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            buf[out_n++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    *out_len = out_n;
    return buf;
}

static JSValue js_getBoundingClientRect(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = el_this(ctx, tv, b->el_cid);
    JSValue r = JS_NewObject(ctx);
    float x = 0, y = 0, w = 0, h = 0;
    if (n)
    {
        x = n->style.abs_x;
        y = n->style.abs_y;
        w = n->style.w;
        h = n->style.h;
        if (b && b->doc && n->style.position != 3)
        {
            x -= b->doc->scroll_x;
            y -= b->doc->scroll_y;
        }
    }
    JS_SetPropertyStr(ctx, r, "x", JS_NewFloat64(ctx, x));
    JS_SetPropertyStr(ctx, r, "y", JS_NewFloat64(ctx, y));
    JS_SetPropertyStr(ctx, r, "left", JS_NewFloat64(ctx, x));
    JS_SetPropertyStr(ctx, r, "top", JS_NewFloat64(ctx, y));
    JS_SetPropertyStr(ctx, r, "right", JS_NewFloat64(ctx, x + w));
    JS_SetPropertyStr(ctx, r, "bottom", JS_NewFloat64(ctx, y + h));
    JS_SetPropertyStr(ctx, r, "width", JS_NewFloat64(ctx, w));
    JS_SetPropertyStr(ctx, r, "height", JS_NewFloat64(ctx, h));
    return r;
}

static JSValue js_getClientRects(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    JSValue rect = js_getBoundingClientRect(ctx, tv, argc, argv);
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, rect);
    return arr;
}
/* W3C textContent: concatenate all descendant text-node data */
static void concat_text(const struct MiniNode *n, char *buf, size_t cap, size_t *o)
{
    if (!n)
        return;
    if (n->type == MN_TEXT_NODE && n->text)
    {
        size_t l = strlen(n->text);
        for (size_t i = 0; i < l && *o + 1 < cap; i++)
            buf[(*o)++] = n->text[i];
    }
    else
    {
        for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
            concat_text(c, buf, cap, o);
    }
}
static JSValue js_getText(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = el_this(ctx, tv, b->el_cid);
    char buf[4096];
    size_t o = 0;
    if (n)
        concat_text(n, buf, sizeof buf, &o);
    buf[o] = 0;
    return JS_NewStringLen(ctx, buf, o);
}
static JSValue js_setText(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = el_this(ctx, tv, b->el_cid);
    const char *t = JS_ToCString(ctx, argv[0]);
    if (n)
    {
        /* W3C textContent SET: drop all children, append one text node */
        while (n->first_child)
            mini_node_remove_child(n, n->first_child);
        mini_node_append_child(n, mini_node_create_text(t ? t : ""));
        if (n->tag && !strcmp(n->tag, "style") && t && b && b->doc)
        {
            mini_css_apply(b->doc, t);
            b->doc->dirty = 1;
            b->doc->paint_dirty = 1;
        }
    }
    JS_FreeCString(ctx, t);
    return JS_UNDEFINED;
}

/* ================================================================== */
/* Stage 3: event bridge — register JS listeners into MiniEventState   */
/* so capture/target/bubble actually fires JS, plus DOM bridges.        */
/* ================================================================== */
/* stopPropagation/preventDefault on the JS event object: set sentinel
   bools the trampoline reads back to drive mini_event_dispatch.         */
static JSValue ev_stop_prop(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    JS_SetPropertyStr(ctx, tv, "__stop", JS_TRUE);
    return JS_UNDEFINED;
}
static JSValue ev_prevent_def(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    JS_SetPropertyStr(ctx, tv, "__prevent", JS_TRUE);
    JS_SetPropertyStr(ctx, tv, "defaultPrevented", JS_TRUE);
    return JS_UNDEFINED;
}

/* Build a fresh JS event object from a C MiniEvent (host-input path). */
static JSValue build_js_event(JSContext *ctx, const MiniEvent *ev, MiniBridge *b)
{
    JSValue eo = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, eo, "type", JS_NewString(ctx, ev->type ? ev->type : ""));
    JS_SetPropertyStr(ctx, eo, "target",
                      ev->target ? wrap_node(ctx, ev->target, b->el_cid) : JS_NULL);
    JS_SetPropertyStr(ctx, eo, "currentTarget",
                      ev->currentTarget ? wrap_node(ctx, ev->currentTarget, b->el_cid) : JS_NULL);
    JS_SetPropertyStr(ctx, eo, "eventPhase",
                      JS_NewInt32(ctx, ev->phase == 0 ? 1 : ev->phase == 1 ? 2
                                                                           : 3));
    JS_SetPropertyStr(ctx, eo, "bubbles", JS_NewBool(ctx, ev->bubbles));
    JS_SetPropertyStr(ctx, eo, "cancelable", JS_TRUE);
    JS_SetPropertyStr(ctx, eo, "clientX", JS_NewFloat64(ctx, ev->clientX));
    JS_SetPropertyStr(ctx, eo, "clientY", JS_NewFloat64(ctx, ev->clientY));
    JS_SetPropertyStr(ctx, eo, "screenX", JS_NewFloat64(ctx, ev->screenX));
    JS_SetPropertyStr(ctx, eo, "screenY", JS_NewFloat64(ctx, ev->screenY));
    float px = ev->clientX + (b->doc ? b->doc->scroll_x : 0.0f);
    float py = ev->clientY + (b->doc ? b->doc->scroll_y : 0.0f);
    JS_SetPropertyStr(ctx, eo, "pageX", JS_NewFloat64(ctx, px));
    JS_SetPropertyStr(ctx, eo, "pageY", JS_NewFloat64(ctx, py));
    float ox = ev->clientX, oy = ev->clientY;
    if (ev->target)
    {
        ox = ev->clientX - ev->target->style.abs_x;
        oy = ev->clientY - ev->target->style.abs_y;
    }
    JS_SetPropertyStr(ctx, eo, "offsetX", JS_NewFloat64(ctx, ox));
    JS_SetPropertyStr(ctx, eo, "offsetY", JS_NewFloat64(ctx, oy));
    JS_SetPropertyStr(ctx, eo, "layerX", JS_NewFloat64(ctx, ox));
    JS_SetPropertyStr(ctx, eo, "layerY", JS_NewFloat64(ctx, oy));
    JS_SetPropertyStr(ctx, eo, "x", JS_NewFloat64(ctx, ev->clientX));
    JS_SetPropertyStr(ctx, eo, "y", JS_NewFloat64(ctx, ev->clientY));
    JS_SetPropertyStr(ctx, eo, "movementX", JS_NewFloat64(ctx, ev->movementX));
    JS_SetPropertyStr(ctx, eo, "movementY", JS_NewFloat64(ctx, ev->movementY));
    JS_SetPropertyStr(ctx, eo, "button", JS_NewInt32(ctx, ev->button));
    JS_SetPropertyStr(ctx, eo, "buttons", JS_NewInt32(ctx, ev->buttons));
    JS_SetPropertyStr(ctx, eo, "pointerId", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, eo, "pointerType", JS_NewString(ctx, "mouse"));
    JS_SetPropertyStr(ctx, eo, "isPrimary", JS_TRUE);
    JS_SetPropertyStr(ctx, eo, "width", JS_NewFloat64(ctx, 1.0));
    JS_SetPropertyStr(ctx, eo, "height", JS_NewFloat64(ctx, 1.0));
    JS_SetPropertyStr(ctx, eo, "pressure", JS_NewFloat64(ctx, ev->buttons ? 0.5 : 0.0));
    JS_SetPropertyStr(ctx, eo, "altKey", JS_NewBool(ctx, ev->altKey));
    JS_SetPropertyStr(ctx, eo, "ctrlKey", JS_NewBool(ctx, ev->ctrlKey));
    JS_SetPropertyStr(ctx, eo, "shiftKey", JS_NewBool(ctx, ev->shiftKey));
    JS_SetPropertyStr(ctx, eo, "metaKey", JS_NewBool(ctx, ev->metaKey));
    if (ev->key)
        JS_SetPropertyStr(ctx, eo, "key", JS_NewString(ctx, ev->key));
    if (ev->code)
        JS_SetPropertyStr(ctx, eo, "code", JS_NewString(ctx, ev->code));
    JS_SetPropertyStr(ctx, eo, "keyCode", JS_NewInt32(ctx, ev->keyCode));
    JS_SetPropertyStr(ctx, eo, "which", JS_NewInt32(ctx, ev->keyCode));
    JS_SetPropertyStr(ctx, eo, "repeat", JS_NewBool(ctx, ev->repeat));
    JS_SetPropertyStr(ctx, eo, "deltaX", JS_NewFloat64(ctx, ev->deltaX));
    JS_SetPropertyStr(ctx, eo, "deltaY", JS_NewFloat64(ctx, ev->deltaY));
    JS_SetPropertyStr(ctx, eo, "deltaMode", JS_NewInt32(ctx, ev->deltaMode));
    JS_SetPropertyStr(ctx, eo, "stopPropagation",
                      JS_NewCFunction(ctx, (JSCFunction *)ev_stop_prop, "stopPropagation", 0));
    JS_SetPropertyStr(ctx, eo, "preventDefault",
                      JS_NewCFunction(ctx, (JSCFunction *)ev_prevent_def, "preventDefault", 0));
    JS_SetPropertyStr(ctx, eo, "__stop", JS_FALSE);
    JS_SetPropertyStr(ctx, eo, "__prevent", JS_FALSE);
    JS_SetPropertyStr(ctx, eo, "defaultPrevented", JS_FALSE);

    /* DragEvent.dataTransfer: a minimal DataTransfer backed by the C
       dnd.data slot (text/plain and files). Attached for every drag-family event so
       dragstart/dragover/drop all share the same payload. */
    if (ev->type && (!strncmp(ev->type, "drag", 4) || !strcmp(ev->type, "drop")))
    {
        static const char *dt_src =
            "({setData:function(t,v){__miniDndSet(String(v==null?'':v));},"
            "getData:function(t){return __miniDndGet();},"
            "clearData:function(){__miniDndSet('');},"
            "types:['text/plain', 'Files'],dropEffect:'move',effectAllowed:'all',files:[],items:[]})";
        JSValue dt = JS_Eval(ctx, dt_src, strlen(dt_src), "<dnd-dt>",
                             JS_EVAL_TYPE_GLOBAL);
        if (!JS_IsException(dt))
        {
            const char *dnd_data = (b && b->ev) ? mini_events_dnd_get_data(b->ev) : NULL;
            if (dnd_data && *dnd_data)
            {
                JSValue files_arr = JS_NewArray(ctx);
                JSValue items_arr = JS_NewArray(ctx);
                uint32_t file_idx = 0;
                char *dup = mc_strdup(dnd_data);
                if (dup)
                {
                    char *line = strtok(dup, "\r\n");
                    while (line)
                    {
                        while (*line == ' ' || *line == '\t') line++;
                        if (*line)
                        {
                            const char *slash = strrchr(line, '/');
                            if (!slash) slash = strrchr(line, '\\');
                            const char *fname = slash ? slash + 1 : line;

                            struct stat stbuf;
                            int sres = stat(line, &stbuf);
                            double fsize = (sres == 0) ? (double)stbuf.st_size : 0;
                            double mtime = (sres == 0) ? (double)stbuf.st_mtime * 1000.0 : 0;

                            const char *dot = strrchr(fname, '.');
                            const char *mime = "application/octet-stream";
                            if (dot)
                            {
                                if (!stricmp(dot, ".glb")) mime = "model/gltf-binary";
                                else if (!stricmp(dot, ".gltf")) mime = "model/gltf+json";
                                else if (!stricmp(dot, ".bin")) mime = "application/octet-stream";
                                else if (!stricmp(dot, ".png")) mime = "image/png";
                                else if (!stricmp(dot, ".jpg") || !stricmp(dot, ".jpeg")) mime = "image/jpeg";
                                else if (!stricmp(dot, ".webp")) mime = "image/webp";
                                else if (!stricmp(dot, ".hdr")) mime = "image/vnd.radiance";
                                else if (!stricmp(dot, ".json")) mime = "application/json";
                                else if (!stricmp(dot, ".txt")) mime = "text/plain";
                            }

                            JSValue file_obj = JS_NewObject(ctx);
                            JS_SetPropertyStr(ctx, file_obj, "name", JS_NewString(ctx, fname));
                            JS_SetPropertyStr(ctx, file_obj, "size", JS_NewFloat64(ctx, fsize));
                            JS_SetPropertyStr(ctx, file_obj, "type", JS_NewString(ctx, mime));
                            JS_SetPropertyStr(ctx, file_obj, "lastModified", JS_NewFloat64(ctx, mtime));
                            JS_SetPropertyStr(ctx, file_obj, "webkitRelativePath", JS_NewString(ctx, ""));
                            JS_SetPropertyStr(ctx, file_obj, "__filePath", JS_NewString(ctx, line));

                            JSValue global_obj = JS_GetGlobalObject(ctx);
                            JSValue file_ctor = JS_GetPropertyStr(ctx, global_obj, "File");
                            if (JS_IsFunction(ctx, file_ctor))
                            {
                                JSValue proto = JS_GetPropertyStr(ctx, file_ctor, "prototype");
                                JS_SetPrototype(ctx, file_obj, proto);
                                JS_FreeValue(ctx, proto);
                            }
                            JS_FreeValue(ctx, file_ctor);
                            JS_FreeValue(ctx, global_obj);

                            JS_SetPropertyUint32(ctx, files_arr, file_idx, JS_DupValue(ctx, file_obj));

                            JSValue item_obj = JS_NewObject(ctx);
                            JS_SetPropertyStr(ctx, item_obj, "kind", JS_NewString(ctx, "file"));
                            JS_SetPropertyStr(ctx, item_obj, "type", JS_NewString(ctx, mime));
                            JS_SetPropertyStr(ctx, item_obj, "_file", JS_DupValue(ctx, file_obj));
                            static const char *getasfile_src = "(function(){ return this._file; })";
                            JSValue fn_gas = JS_Eval(ctx, getasfile_src, strlen(getasfile_src), "<gas>", JS_EVAL_TYPE_GLOBAL);
                            JS_SetPropertyStr(ctx, item_obj, "getAsFile", fn_gas);

                            JS_SetPropertyUint32(ctx, items_arr, file_idx, item_obj);
                            JS_FreeValue(ctx, file_obj);
                            file_idx++;
                        }
                        line = strtok(NULL, "\r\n");
                    }
                    free(dup);
                }
                static const char *item_fn_src = "(function(i){ return this[i] || null; })";
                JSValue fn_item = JS_Eval(ctx, item_fn_src, strlen(item_fn_src), "<item>", JS_EVAL_TYPE_GLOBAL);
                JS_SetPropertyStr(ctx, files_arr, "item", fn_item);

                JS_SetPropertyStr(ctx, dt, "files", files_arr);
                JS_SetPropertyStr(ctx, dt, "items", items_arr);
            }
            JS_SetPropertyStr(ctx, eo, "dataTransfer", dt);
        }
        else
            JS_FreeValue(ctx, dt);
    }
    return eo;
}

/* MiniEventListenerCb: re-enters JS with the event object. For programmatic
   dispatchEvent, ev->ud points at the caller's JS event object (shared, so a
   prop set by one listener is visible to the next); for host-input events
   ev->ud is NULL and a fresh object is built per fire.                   */
static void ev_trampoline(MiniEvent *ev, void *ud)
{
    JsEvListener *L = (JsEvListener *)ud;
    if (!L || !L->b)
        return;
    JSContext *ctx = L->b->ctx;
    JSValue eo;
    if (ev->ud)
        eo = JS_DupValue(ctx, *(JSValue *)ev->ud);
    else
        eo = build_js_event(ctx, ev, L->b);

    /* 架构核心修复 1：将真实绑定的 JS 对象塞给 currentTarget，满足 React/Vue 的事件委托诉求 */
    JS_SetPropertyStr(ctx, eo, "currentTarget", JS_DupValue(ctx, L->js_target));

    /* 架构核心修复 2：将其作为 this 传给事件回调，彻底拯救 Preact 的 this._listeners！ */
    JSValue this_val = L->js_target;
    JSValue ret = JS_UNDEFINED;

    if (JS_IsFunction(ctx, L->cb))
    {
        ret = JS_Call(ctx, L->cb, this_val, 1, &eo);
    }
    else if (JS_IsObject(L->cb))
    {
        JSValue handleEvent = JS_GetPropertyStr(ctx, L->cb, "handleEvent");
        if (JS_IsFunction(ctx, handleEvent))
        {
            ret = JS_Call(ctx, handleEvent, L->cb, 1, &eo);
        }
        JS_FreeValue(ctx, handleEvent);
    }

    if (JS_IsException(ret))
    {
        /* 暴露框架内部因 API 不兼容导致的静默死锁错误 */
        JSValue e = JS_GetException(ctx);
        const char *str = JS_ToCString(ctx, e);
        JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
        const char *st = JS_ToCString(ctx, stack);
        fprintf(stderr, "\033[31m[Event Error] %s\n%s\033[0m\n", str ? str : "?", st ? st : "(no stack)");
        if (str)
            JS_FreeCString(ctx, str);
        if (st)
            JS_FreeCString(ctx, st);
        JS_FreeValue(ctx, stack);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, ret);

    /* 读取冒泡与默认行为阻断标志 */
    JSValue sv = JS_GetPropertyStr(ctx, eo, "__stop");
    if (JS_ToBool(ctx, sv))
        ev->stopPropagation = 1;
    JS_FreeValue(ctx, sv);
    JSValue pv = JS_GetPropertyStr(ctx, eo, "__prevent");
    if (JS_ToBool(ctx, pv))
        ev->preventDefault = 1;
    JS_FreeValue(ctx, pv);
    JS_FreeValue(ctx, eo);
}

static void js_inline_event_handler(MiniEventState *st, struct MiniNode *node, MiniEvent *ev, const char *code, void *ud)
{
    (void)st;
    MiniBridge *b = (MiniBridge *)ud;
    if (!b || !b->ctx || !node || !code || !*code)
        return;
    JSContext *ctx = b->ctx;
    JSValue eo = build_js_event(ctx, ev, b);
    JSValue this_val = wrap_node(ctx, node, b->el_cid);

    char fn_src[4096];
    snprintf(fn_src, sizeof(fn_src), "(function(event){ %s })", code);
    JSValue fn = JS_Eval(ctx, fn_src, strlen(fn_src), "<inline-event>", JS_EVAL_TYPE_GLOBAL);

    if (JS_IsException(fn))
    {
        JSValue exc = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, exc);
        fprintf(stderr, "\033[31m[内联事件语法错误] %s\n触发代码: %s\033[0m\n", s ? s : "?", code);
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, exc);
    }
    else if (JS_IsFunction(ctx, fn))
    {
        JSValue ret = JS_Call(ctx, fn, this_val, 1, &eo);
        if (JS_IsException(ret))
        {
            JSValue exc = JS_GetException(ctx);
            const char *s = JS_ToCString(ctx, exc);
            fprintf(stderr, "\033[31m[内联事件运行报错] %s\n触发代码: %s\033[0m\n", s ? s : "?", code);
            if (s) JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, fn);

    JSValue sv = JS_GetPropertyStr(ctx, eo, "__stop");
    if (JS_ToBool(ctx, sv))
        ev->stopPropagation = 1;
    JS_FreeValue(ctx, sv);

    JSValue pv = JS_GetPropertyStr(ctx, eo, "__prevent");
    if (JS_ToBool(ctx, pv))
        ev->preventDefault = 1;
    JS_FreeValue(ctx, pv);

    JS_FreeValue(ctx, this_val);
    JS_FreeValue(ctx, eo);
}

/* resolve a listener target from `this`: element, or document, else body
   (window-level events are dispatched on body by mini_events).          */
static struct MiniNode *listener_target(JSContext *ctx, JSValueConst tv, MiniBridge *b)
{
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    if (!n)
    {
        MiniDocument *d = (MiniDocument *)JS_GetOpaque(tv, b->doc_cid);
        if (d)
            n = d->root;
    }
    if (!n)
        n = b->doc->body;
    return n;
}

static JSValue js_addEventListener(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    if (!b || !b->ev || argc < 2)
        return JS_UNDEFINED;
    struct MiniNode *n = listener_target(ctx, tv, b);
    const char *type = JS_ToCString(ctx, argv[0]);
    int useCapture = (argc >= 3 && JS_ToBool(ctx, argv[2])) ? 1 : 0;
    JsEvListener *L = (JsEvListener *)calloc(1, sizeof(*L));
    if (!L)
    {
        JS_FreeCString(ctx, type);
        return JS_UNDEFINED;
    }
    L->b = b;
    L->cb = JS_DupValue(ctx, argv[1]);
    L->js_target = JS_DupValue(ctx, tv);
    MiniEventListener *h = mini_events_add_listener(b->ev, n, type ? type : "",
                                                    ev_trampoline, L, useCapture);
    JS_FreeCString(ctx, type);
    L->handle = h;
    if (!h)
    {
        JS_FreeValue(ctx, L->cb);
        free(L);
        return JS_UNDEFINED;
    }
    if (b->ev_listeners_n >= b->ev_listeners_cap)
    {
        int nc = b->ev_listeners_cap ? b->ev_listeners_cap * 2 : 16;
        JsEvListener **na = (JsEvListener **)realloc(b->ev_listeners,
                                                     sizeof(JsEvListener *) * nc);
        if (!na)
        {
            mini_events_remove_listener(b->ev, h);
            JS_FreeValue(ctx, L->cb);
            free(L);
            return JS_UNDEFINED;
        }
        b->ev_listeners = na;
        b->ev_listeners_cap = nc;
    }
    b->ev_listeners[b->ev_listeners_n++] = L;
    return JS_UNDEFINED;
}

static JSValue js_removeEventListener(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    if (!b || !b->ev || argc < 2)
        return JS_UNDEFINED;
    struct MiniNode *tn = listener_target(ctx, tv, b);
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type)
        return JS_UNDEFINED;
    for (int i = 0; i < b->ev_listeners_n; i++)
    {
        JsEvListener *L = b->ev_listeners[i];
        if (!L || !L->handle)
            continue;
        if (L->handle->target != tn)
            continue;
        if (strcmp(L->handle->type, type) != 0)
            continue;
        if (!JS_IsStrictEqual(ctx, L->cb, argv[1]))
            continue;
        mini_events_remove_listener(b->ev, L->handle);
        JS_FreeValue(ctx, L->cb);
        JS_FreeValue(ctx, L->js_target);
        free(L);
        b->ev_listeners[i] = NULL; /* leave a hole */
        break;
    }
    JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

static JSValue js_dispatchEvent(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    if (!b || !b->ev || argc < 1)
        return JS_TRUE;
    struct MiniNode *n = listener_target(ctx, tv, b);
    JSValue tval = JS_GetPropertyStr(ctx, argv[0], "type");
    const char *type = JS_ToCString(ctx, tval);
    JS_FreeValue(ctx, tval);
    if (!type)
        return JS_TRUE;
    MiniEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.target = n;
    ev.ud = NULL; /* let ev_trampoline safely build event object */
    ev.bubbles = 1;
    /* ensure the event object carries stop/prevent (a plain {type:...} the
       caller built doesn't), so listeners can drive propagation control.
       Resetting __stop/__prevent here means each dispatch starts clean.   */
    JS_SetPropertyStr(ctx, argv[0], "stopPropagation",
                      JS_NewCFunction(ctx, (JSCFunction *)ev_stop_prop, "stopPropagation", 0));
    JS_SetPropertyStr(ctx, argv[0], "preventDefault",
                      JS_NewCFunction(ctx, (JSCFunction *)ev_prevent_def, "preventDefault", 0));
    JS_SetPropertyStr(ctx, argv[0], "__stop", JS_FALSE);
    JS_SetPropertyStr(ctx, argv[0], "__prevent", JS_FALSE);
    JS_SetPropertyStr(ctx, argv[0], "defaultPrevented", JS_FALSE);
    /* copy a few standard fields if the caller set them */
    JSValue cxv = JS_GetPropertyStr(ctx, argv[0], "clientX");
    if (!JS_IsUndefined(cxv))
    {
        double v = 0;
        JS_ToFloat64(ctx, &v, cxv);
        ev.clientX = (float)v;
    }
    JS_FreeValue(ctx, cxv);
    JSValue cyv = JS_GetPropertyStr(ctx, argv[0], "clientY");
    if (!JS_IsUndefined(cyv))
    {
        double v = 0;
        JS_ToFloat64(ctx, &v, cyv);
        ev.clientY = (float)v;
    }
    JS_FreeValue(ctx, cyv);
    mini_event_dispatch(b->ev, &ev, n);
    JS_FreeCString(ctx, type);
    return ev.preventDefault ? JS_FALSE : JS_TRUE;
}

/* element.focus() / .blur() */
static JSValue js_focus(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    (void)argc;
    (void)argv;
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    if (b && b->ev && n)
        mini_events_focus(b->ev, n);
    return JS_UNDEFINED;
}
static JSValue js_blur(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    (void)argc;
    (void)argv;
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    if (b && b->ev && n && mini_events_active_element(b->ev) == n)
        mini_events_focus(b->ev, NULL);
    return JS_UNDEFINED;
}

/* innerHTML getter/setter (backed by mini_dom_outer_html / set_inner_html). */
static JSValue js_getInnerHTML(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    (void)argc;
    (void)argv;
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    if (!n)
        return JS_NewString(ctx, "");
    char buf[32768];
    mini_dom_outer_html(n, 1, buf, sizeof buf);
    return JS_NewString(ctx, buf);
}
static JSValue js_setInnerHTML(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    if (n && argc >= 1)
    {
        const char *html = JS_ToCString(ctx, argv[0]);
        if (html)
        {
            mini_node_set_inner_html(n, html);
            if (n->tag && !strcmp(n->tag, "style") && b && b->doc)
            {
                mini_css_apply(b->doc, html);
                b->doc->dirty = 1;
                b->doc->paint_dirty = 1;
            }
        }
        JS_FreeCString(ctx, html);
    }
    return JS_UNDEFINED;
}

/* removeAttribute / insertBefore / cloneNode (DOM bridges the proto lacked). */
static JSValue js_removeAttribute(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    if (n && argc >= 1)
    {
        const char *k = JS_ToCString(ctx, argv[0]);
        if (k)
        {
            mini_node_remove_attribute(n, k);
            JS_FreeCString(ctx, k);
        }
    }
    return JS_UNDEFINED;
}
static JSValue js_insertBefore(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *parent = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    struct MiniNode *newc = (argc >= 1) ? (struct MiniNode *)JS_GetOpaque(argv[0], b->el_cid) : NULL;
    struct MiniNode *ref = (argc >= 2 && !JS_IsNull(argv[1]))
                               ? (struct MiniNode *)JS_GetOpaque(argv[1], b->el_cid)
                               : NULL;
    if (parent && newc)
    {
        mini_node_insert_before(parent, newc, ref);
        maybe_execute_script(b, newc, argv[0]);
    }
    return newc ? JS_DupValue(ctx, argv[0]) : JS_NULL;
}
static JSValue js_cloneNode(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    int deep = (argc >= 1) ? JS_ToBool(ctx, argv[0]) : 0;
    struct MiniNode *c = n ? mini_node_clone(n, deep) : NULL;
    return c ? wrap_node(ctx, c, b->el_cid) : JS_NULL;
}

/* tagName / firstElementChild (tagName is uppercased per DOM). */
static JSValue js_getTag(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    (void)argc;
    (void)argv;
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    if (n && n->tag)
    {
        char up[32];
        size_t i = 0;
        for (; n->tag[i] && i < sizeof up - 1; i++)
            up[i] = (char)toupper((unsigned char)n->tag[i]);
        up[i] = 0;
        return JS_NewString(ctx, up);
    }
    return JS_NewString(ctx, "");
}
static JSValue js_firstElementChild(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    (void)argc;
    (void)argv;
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    struct MiniNode *c = n ? mini_node_first_element_child(n) : NULL;
    return c ? wrap_node(ctx, c, b->el_cid) : JS_NULL;
}

/* ------------------------------------------------------------------ */
/* Stage 4+: DOM traversal / properties / synthetic click / log poll    */
/* (the missing standard surface real apps reach for after addEventListener). */
/* ------------------------------------------------------------------ */
static JSValue wrap_or_null(JSContext *ctx, struct MiniNode *n, JSClassID cid)
{
    return n ? wrap_node(ctx, n, cid) : JS_NULL;
}
static JSValue js_firstChild(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    (void)argc;
    (void)argv;
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    return wrap_or_null(ctx, n ? n->first_child : NULL, b->el_cid);
}
static JSValue js_lastChild(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    (void)argc;
    (void)argv;
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    return wrap_or_null(ctx, n ? n->last_child : NULL, b->el_cid);
}
static JSValue js_nextSibling(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    (void)argc;
    (void)argv;
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    return wrap_or_null(ctx, n ? n->next_sibling : NULL, b->el_cid);
}
static JSValue js_prevSibling(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    (void)argc;
    (void)argv;
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    return wrap_or_null(ctx, n ? n->prev_sibling : NULL, b->el_cid);
}
static JSValue js_parentNode(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    (void)argc;
    (void)argv;
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    return wrap_or_null(ctx, n ? n->parent : NULL, b->el_cid);
}
static JSValue js_parentElement(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    (void)argc;
    (void)argv;
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    struct MiniNode *p = n ? n->parent : NULL;
    return (p && p->type == MN_ELEMENT_NODE) ? wrap_node(ctx, p, b->el_cid) : JS_NULL;
}
static JSValue js_lastElementChild(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    (void)argc;
    (void)argv;
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    return wrap_or_null(ctx, n ? mini_node_last_element_child(n) : NULL, b->el_cid);
}
static JSValue js_nextElementSibling(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    (void)argc;
    (void)argv;
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    for (struct MiniNode *s = n ? n->next_sibling : NULL; s; s = s->next_sibling)
        if (s->type == MN_ELEMENT_NODE)
            return wrap_node(ctx, s, b->el_cid);
    return JS_NULL;
}
static JSValue js_prevElementSibling(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    (void)argc;
    (void)argv;
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    for (struct MiniNode *s = n ? n->prev_sibling : NULL; s; s = s->prev_sibling)
        if (s->type == MN_ELEMENT_NODE)
            return wrap_node(ctx, s, b->el_cid);
    return JS_NULL;
}
/* children (HTMLCollection) + childNodes (NodeList) — snapshot arrays. */
static JSValue js_children(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    (void)argc;
    (void)argv;
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    JSValue arr = JS_NewArray(ctx);
    int i = 0;
    for (struct MiniNode *c = n ? n->first_child : NULL; c; c = c->next_sibling)
        if (c->type == MN_ELEMENT_NODE)
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i++, wrap_node(ctx, c, b->el_cid));
    return arr;
}
static JSValue js_childNodes(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    (void)argc;
    (void)argv;
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    JSValue arr = JS_NewArray(ctx);
    int i = 0;
    for (struct MiniNode *c = n ? n->first_child : NULL; c; c = c->next_sibling)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i++, wrap_node(ctx, c, b->el_cid));
    return arr;
}
static JSValue js_childElementCount(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    return JS_NewInt32(ctx, n ? mini_node_element_child_count(n) : 0);
}
static JSValue js_hasChildNodes(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    return JS_NewBool(ctx, n && n->first_child != NULL);
}

/* nodeType / nodeName / nodeValue */
static JSValue js_nodeType(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    return JS_NewInt32(ctx, n ? (int)n->type : 0);
}
static JSValue js_nodeName(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    if (!n)
        return JS_NewString(ctx, "");
    switch (n->type)
    {
    case MN_ELEMENT_NODE:
    {
        char up[32];
        size_t i = 0;
        for (; n->tag && n->tag[i] && i < sizeof up - 1; i++)
            up[i] = (char)toupper((unsigned char)n->tag[i]);
        up[i] = 0;
        return JS_NewString(ctx, up);
    }
    case MN_TEXT_NODE:
        return JS_NewString(ctx, "#text");
    case MN_COMMENT_NODE:
        return JS_NewString(ctx, "#comment");
    case MN_DOCUMENT_NODE:
        return JS_NewString(ctx, "#document");
    case MN_DOCUMENT_FRAGMENT_NODE:
        return JS_NewString(ctx, "#document-fragment");
    default:
        return JS_NewString(ctx, "");
    }
}
static JSValue js_getNodeValue(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    if (!n)
        return JS_NULL;
    if (n->type == MN_TEXT_NODE || n->type == MN_COMMENT_NODE)
        return JS_NewString(ctx, n->text ? n->text : "");
    return JS_NULL;
}
static JSValue js_setNodeValue(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    if (n && (n->type == MN_TEXT_NODE || n->type == MN_COMMENT_NODE) && argc >= 1)
    {
        const char *t = JS_ToCString(ctx, argv[0]);
        mini_node_set_text(n, t ? t : "");
        if (t)
            JS_FreeCString(ctx, t);
        if (b->doc)
            b->doc->dirty = 1;
    }
    return JS_UNDEFINED;
}
/* element.contains(node) */
static JSValue js_contains(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    struct MiniNode *other = (argc >= 1) ? (struct MiniNode *)JS_GetOpaque(argv[0], b->el_cid) : NULL;
    return JS_NewBool(ctx, n && other && mini_node_contains(n, other));
}
/* element.matches(sel) — true if this node is in document.querySelectorAll(sel).
   Must compare by underlying MiniNode pointer (wrappers are not interned, so a
   JS `arr[i] === this` test is always false here). */
static JSValue js_matches(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    if (!n || argc < 1)
        return JS_FALSE;
    const char *sel = JS_ToCString(ctx, argv[0]);
    JSValue r = JS_FALSE;
    if (sel)
    {
        if (mini_dom_matches_selector(n, sel))
            r = JS_TRUE;
        JS_FreeCString(ctx, sel);
    }
    return r;
}

/* element.closest(sel) — walk this node + ancestors, return the first that
   matches sel (by pointer comparison against querySelectorAll). */
static JSValue js_closest(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    if (!n || argc < 1)
        return JS_NULL;
    const char *sel = JS_ToCString(ctx, argv[0]);
    JSValue r = JS_NULL;
    if (sel)
    {
        /* 核心修复：直接使用底层 O(1) 选择器逐级向上匹配，支持文本节点精准回溯父级 */
        for (struct MiniNode *p = n; p; p = p->parent)
        {
            if (p->type != MN_ELEMENT_NODE && p->type != MN_DOCUMENT_NODE)
                continue;
            if (mini_dom_matches_selector(p, sel))
            {
                r = wrap_node(ctx, p, b->el_cid);
                break;
            }
        }
        JS_FreeCString(ctx, sel);
    }
    return r;
}

/* element.click() — dispatch a synthetic click (capture/target/bubble + inline onclick). */
static JSValue js_click(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    if (!b->ev || !n)
        return JS_UNDEFINED;
    MiniEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = "click";
    ev.target = n;
    ev.button = 0;
    ev.buttons = 1;
    ev.bubbles = 1;
    ev.clientX = n->style.abs_x + n->style.w * 0.5f;
    ev.clientY = n->style.abs_y + n->style.h * 0.5f;
    ev.screenX = ev.clientX;
    ev.screenY = ev.clientY;
    ev.pageX = ev.clientX;
    ev.pageY = ev.clientY;
    mini_event_dispatch(b->ev, &ev, n);
    return JS_UNDEFINED;
}
/* element.scrollIntoView() — best-effort: scroll the document so the box is on screen. */
static JSValue js_scrollIntoView(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    if (!b->doc || !n)
        return JS_UNDEFINED;
    float y = n->style.abs_y, h = n->style.h > 0 ? n->style.h : 0;
    float vh = (float)b->doc->viewport_h;
    if (vh <= 0)
        vh = (float)b->r->gpu.height;
    if (y < b->doc->scroll_y)
        b->doc->scroll_y = y;
    else if (y + h > b->doc->scroll_y + vh)
        b->doc->scroll_y = (y + h - vh > 0) ? (y + h - vh) : 0;
    if (b->doc->scroll_y < 0)
        b->doc->scroll_y = 0;
    if (b->doc->scroll_y > b->doc->max_scroll_y)
        b->doc->scroll_y = b->doc->max_scroll_y;
    b->doc->dirty = 1;
    return JS_UNDEFINED;
}
/* element._computedStyleJSON() — raw computed style as JSON for getComputedStyle. */
static JSValue js_computedStyleJSON(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    char buf[8192];
    buf[0] = 0;
    if (n)
        mini_dom_computed_style(n, buf, sizeof buf);
    return JS_NewString(ctx, buf[0] ? buf : "[]");
}
/* outerHTML getter: tag + children + close (mini_dom_outer_html inner=0). */
static JSValue js_getOuterHTML(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    if (!n)
        return JS_NewString(ctx, "");
    char buf[32768];
    mini_dom_outer_html(n, 0, buf, sizeof buf);
    return JS_NewString(ctx, buf);
}
/* outerHTML setter: best-effort replace this node in its parent with the
   parsed fragment. Falls back to innerHTML semantics when there's no parent. */
static JSValue js_setOuterHTML(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(tv, b->el_cid);
    if (!n || argc < 1)
        return JS_UNDEFINED;
    const char *html = JS_ToCString(ctx, argv[0]);
    if (!html)
        return JS_UNDEFINED;
    struct MiniNode *p = n->parent;
    if (!p || p->type == MN_DOCUMENT_NODE)
    {
        /* no editable parent: behave like innerHTML */
        mini_node_set_inner_html(n, html);
    }
    else
    {
        struct MiniNode *frag = mini_node_create_document_fragment();
        mini_node_set_inner_html(frag, html);
        /* Snapshot the fragment's top-level children BEFORE moving any:
           insert_before re-parents a node and invalidates frag's sibling
           chain, so iterating it live would walk the parent's list instead. */
        struct MiniNode *kids[256];
        int nk = 0;
        for (struct MiniNode *c = frag->first_child; c && nk < 256; c = c->next_sibling)
            kids[nk++] = c;
        /* Insert each in order before `n` — that preserves document order. */
        for (int i = 0; i < nk; i++)
            mini_node_insert_before(p, kids[i], n);
        if (nk > 0)
            mini_node_remove_child(p, n);
        mini_node_destroy(frag);
    }
    JS_FreeCString(ctx, html);
    if (b->doc)
        b->doc->dirty = 1;
    return JS_UNDEFINED;
}
/* document.elementFromPoint(x, y) — hit test the laid-out tree. */
static JSValue js_elementFromPoint(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    MiniBridge *b = bridge_of(ctx);
    if (!b->doc || argc < 2)
        return JS_NULL;
    double x = 0, y = 0;
    JS_ToFloat64(ctx, &x, argv[0]);
    JS_ToFloat64(ctx, &y, argv[1]);
    struct MiniNode *h = mini_dom_hit_test_doc(b->doc, (float)x, (float)y);
    return h ? wrap_node(ctx, h, b->el_cid) : JS_NULL;
}
/* document.createDocumentFragment / createComment / createElementNS alias. */
static JSValue js_createDocumentFragment(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    (void)argc;
    (void)argv;
    MiniBridge *b = bridge_of(ctx);
    return wrap_node(ctx, mini_node_create_document_fragment(), b->el_cid);
}
static JSValue js_createComment(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    MiniBridge *b = bridge_of(ctx);
    const char *d = (argc >= 1) ? JS_ToCString(ctx, argv[0]) : NULL;
    struct MiniNode *n = mini_node_create_comment(d ? d : "");
    if (d)
        JS_FreeCString(ctx, d);
    return wrap_node(ctx, n, b->el_cid);
}

/* canvas.getContext('2d' | 'webgl') returns a renderer context INSTANCE
   whose prototype carries the native methods; Three.js then calls
   gl.createBuffer / gl.drawArrays on it and they resolve via the proto. */
static JSValue js_getContext(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    const char *kind = JS_ToCString(ctx, argv[0]);
    JSValue ret = JS_NULL;
    if (kind)
    {
        /* Treat "webgl2" the same as "webgl": this engine implements a single
           WebGL 1.0 surface. Three.js (and other engines) request 'webgl2'
           FIRST via canvas.getContext('webgl2', ...) and use the first
           non-null context returned. Previously 'webgl2' fell through to the
           2D-context branch, yielding a truthy object with NO WebGL methods
           on its prototype -> gl.getExtension / gl.createShader were
           undefined and `new THREE.WebGLRenderer()` threw
           "TypeError: not a function" before the canvas was ever created.
           Returning the WebGLRenderingContext instance here makes Three.js
           pick the working context; isWebGL2 stays false (WebGL2RenderingContext
           is not defined) so the WebGL 1 code path is used. */
        const char *ctor_name =
            (!strcmp(kind, "webgl") || !strcmp(kind, "webgl2") ||
             !strcmp(kind, "experimental-webgl"))
                ? "WebGLRenderingContext"
                : "CanvasRenderingContext2D";
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue ctor = JS_GetPropertyStr(ctx, global, ctor_name);
        JSValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
        ret = JS_NewObject(ctx);
        JS_SetPrototype(ctx, ret, proto); /* does not steal proto */
        JS_FreeValue(ctx, proto);
        JS_FreeValue(ctx, ctor);
        JS_FreeValue(ctx, global);
        /* remember which canvas this context belongs to so gl.viewport can
           map canvas-local GL coords (bottom-left origin) to the canvas's
           window rect and scissor draws to it. */
        JS_SetPropertyStr(ctx, ret, "_cv", JS_DupValue(ctx, tv));
        /* 2D context defaults */
        JS_SetPropertyStr(ctx, ret, "fillStyle", JS_NewString(ctx, "#000000"));
        JS_SetPropertyStr(ctx, ret, "strokeStyle", JS_NewString(ctx, "#000000"));
        JS_SetPropertyStr(ctx, ret, "lineWidth", JS_NewFloat64(ctx, 1.0));
        JS_SetPropertyStr(ctx, ret, "font", JS_NewString(ctx, "10px sans-serif"));
        JS_SetPropertyStr(ctx, ret, "textAlign", JS_NewString(ctx, "start"));
        JS_SetPropertyStr(ctx, ret, "textBaseline", JS_NewString(ctx, "alphabetic"));
        JS_SetPropertyStr(ctx, ret, "globalAlpha", JS_NewFloat64(ctx, 1.0));
        JS_SetPropertyStr(ctx, ret, "globalCompositeOperation", JS_NewString(ctx, "source-over"));
        JS_SetPropertyStr(ctx, ret, "lineCap", JS_NewString(ctx, "butt"));
        JS_SetPropertyStr(ctx, ret, "lineJoin", JS_NewString(ctx, "miter"));
        JS_SetPropertyStr(ctx, ret, "miterLimit", JS_NewFloat64(ctx, 10.0));
        JS_SetPropertyStr(ctx, ret, "lineDashOffset", JS_NewFloat64(ctx, 0.0));
        JS_SetPropertyStr(ctx, ret, "shadowColor", JS_NewString(ctx, "rgba(0,0,0,0)"));
        JS_SetPropertyStr(ctx, ret, "shadowBlur", JS_NewFloat64(ctx, 0.0));
        JS_SetPropertyStr(ctx, ret, "shadowOffsetX", JS_NewFloat64(ctx, 0.0));
        JS_SetPropertyStr(ctx, ret, "shadowOffsetY", JS_NewFloat64(ctx, 0.0));
        JS_SetPropertyStr(ctx, ret, "imageSmoothingEnabled", JS_TRUE);
        JS_FreeCString(ctx, kind);
    }
    return ret;
}

/* ----------------------------------------------------------------- */
/* Document methods                                                   */
/* ----------------------------------------------------------------- */
static JSValue js_createElement(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    const char *tag = JS_ToCString(ctx, argv[0]);
    if (!tag)
        return JS_EXCEPTION;
    struct MiniNode *n = mini_node_create_element(tag);
    JS_FreeCString(ctx, tag);
    return wrap_node(ctx, n, b->el_cid);
}
/* createElementNS(namespace, qualifiedName) — this engine has one namespace,
   so we ignore the namespace and create an element from qualifiedName (argv[1]).
   (Aliasing js_createElement would use argv[0] = the namespace string.) */
static JSValue js_createElementNS(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    (void)tv;
    const char *tag = (argc >= 2) ? JS_ToCString(ctx, argv[1]) : NULL;
    if (!tag)
        return JS_NULL;
    struct MiniNode *n = mini_node_create_element(tag);
    JS_FreeCString(ctx, tag);
    return wrap_node(ctx, n, b->el_cid);
}
static JSValue js_createTextNode(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    const char *t = JS_ToCString(ctx, argv[0]);
    struct MiniNode *n = mini_node_create_text(t ? t : "");
    JS_FreeCString(ctx, t);
    return wrap_node(ctx, n, b->el_cid);
}
static JSValue js_getElementById(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    const char *id = JS_ToCString(ctx, argv[0]);
    JSValue ret = JS_NULL;
    if (id)
    {
        struct MiniNode *m = mini_dom_get_element_by_id(b->doc, id);
        if (m)
            ret = wrap_node(ctx, m, b->el_cid);

        JS_FreeCString(ctx, id);
    }
    return ret;
}
static JSValue js_byId(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    /* internal helper used by querySelector('#id') */
    JSValueConst args[1] = {argv[0]};
    return js_getElementById(ctx, tv, 1, args);
}
static JSValue js_byTag(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    const char *tag = JS_ToCString(ctx, argv[0]);
    JSValue ret = JS_NULL;
    if (tag)
    {
        struct MiniNode *stack[256];
        int sp = 0;
        stack[sp++] = b->doc->body;
        while (sp)
        {
            struct MiniNode *n = stack[--sp];
            if (n->tag && !strcmp(n->tag, tag))
            {
                ret = wrap_node(ctx, n, b->el_cid);
                break;
            }
            for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
                if (sp < 256)
                    stack[sp++] = c;
        }
        JS_FreeCString(ctx, tag);
    }
    return ret;
}
/* Phase 1.2: C-backed querySelector / querySelectorAll (CSS matcher) */
static JSValue js_doc_query(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    const char *sel = JS_ToCString(ctx, argv[0]);
    struct MiniNode *m = sel ? mini_dom_query_selector(b->doc, sel) : NULL;
    JS_FreeCString(ctx, sel);
    return m ? wrap_node(ctx, m, b->el_cid) : JS_NULL;
}
static JSValue js_doc_queryAll(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    const char *sel = JS_ToCString(ctx, argv[0]);
    struct MiniNode *out[256];
    int c = sel ? mini_dom_query_selector_all(b->doc, sel, out, 256) : 0;
    JS_FreeCString(ctx, sel);
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < c; i++)
        JS_SetPropertyUint32(ctx, arr, i, wrap_node(ctx, out[i], b->el_cid));
    return arr;
}
static JSValue js_document_body(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    return wrap_node(ctx, b->doc->body, b->el_cid);
}
/* document.activeElement — backed by the MiniEventState focus tracker. */
static JSValue js_doc_activeElement(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    (void)tv;
    (void)argc;
    (void)argv;
    if (!b || !b->ev)
        return JS_NULL;
    struct MiniNode *n = mini_events_active_element(b->ev);
    return n ? wrap_node(ctx, n, b->el_cid) : JS_NULL;
}

/* ----------------------------------------------------------------- */
/* 2D canvas context — records shape commands (replayed at render).    */
/* fillStyle/strokeStyle/lineWidth are plain JS props (set by script,  */
/* read here); everything else maps onto mini_2d_* in mini_dom.c.       */
/* ----------------------------------------------------------------- */
static void ctx2d_paint(JSContext *ctx, JSValueConst tv, const char *prop,
                        float *r, float *g, float *b, float *a)
{
    *r = *g = *b = 0.0f;
    *a = 1.0f;
    JSValue v = JS_GetPropertyStr(ctx, tv, prop);
    if (JS_IsObject(v) && !JS_IsNull(v))
    {
        /* gradient/pattern object: best-effort solid first-stop color.
           Full multi-stop/radial/conic rendering = Stage 4 shader. */
        JSValue isg = JS_GetPropertyStr(ctx, v, "_isGrad");
        int grad = JS_ToBool(ctx, isg);
        JS_FreeValue(ctx, isg);
        if (grad)
        {
            JSValue c0 = JS_GetPropertyStr(ctx, v, "_c0");
            const char *cs = JS_ToCString(ctx, c0);
            if (cs)
            {
                if (!mini_parse_color(cs, r, g, b, a))
                {
                    *r = *g = *b = 0.0f;
                    *a = 1.0f;
                }
                JS_FreeCString(ctx, cs);
            }
            JS_FreeValue(ctx, c0);
            JS_FreeValue(ctx, v);
            return;
        }
    }
    const char *s = JS_ToCString(ctx, v);
    if (s)
    {
        if (!mini_parse_color(s, r, g, b, a))
        {
            *r = *g = *b = 0.0f;
            *a = 1.0f;
        }
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
}
static int ctx2d_is_onscreen(JSContext *ctx, JSValueConst tv)
{
    JSValue cv = JS_GetPropertyStr(ctx, tv, "_cv");
    if (!JS_IsObject(cv))
    {
        JS_FreeValue(ctx, cv);
        return 0;
    }
    JSValue parent = JS_GetPropertyStr(ctx, cv, "parentNode");
    int onscreen = (!JS_IsUndefined(parent) && !JS_IsNull(parent));
    JS_FreeValue(ctx, parent);
    JS_FreeValue(ctx, cv);
    return onscreen;
}

static JSValue js_ctx2d_beginPath(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)ctx;
    (void)argc;
    (void)argv;
    if (ctx2d_is_onscreen(ctx, tv))
        mini_2d_begin_path();
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_closePath(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)ctx;
    (void)argc;
    (void)argv;
    if (ctx2d_is_onscreen(ctx, tv))
        mini_2d_close_path();
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_moveTo(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    double x, y;
    JS_ToFloat64(ctx, &x, argv[0]);
    JS_ToFloat64(ctx, &y, argv[1]);
    if (ctx2d_is_onscreen(ctx, tv))
        mini_2d_move_to((float)x, (float)y);
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_lineTo(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    double x, y;
    JS_ToFloat64(ctx, &x, argv[0]);
    JS_ToFloat64(ctx, &y, argv[1]);
    if (ctx2d_is_onscreen(ctx, tv))
        mini_2d_line_to((float)x, (float)y);
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_arc(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    double cx, cy, r, a0, a1;
    int ccw = 0;
    JS_ToFloat64(ctx, &cx, argv[0]);
    JS_ToFloat64(ctx, &cy, argv[1]);
    JS_ToFloat64(ctx, &r, argv[2]);
    JS_ToFloat64(ctx, &a0, argv[3]);
    JS_ToFloat64(ctx, &a1, argv[4]);
    if (argc > 5)
        JS_ToInt32(ctx, &ccw, argv[5]);
    if (ctx2d_is_onscreen(ctx, tv))
        mini_2d_arc((float)cx, (float)cy, (float)r, (float)a0, (float)a1, ccw);
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_fill(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    float r, g, b, a;
    ctx2d_paint(ctx, tv, "fillStyle", &r, &g, &b, &a);
    if (ctx2d_is_onscreen(ctx, tv))
        mini_2d_fill(r, g, b, a);
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_stroke(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    float r, g, b, a;
    ctx2d_paint(ctx, tv, "strokeStyle", &r, &g, &b, &a);
    JSValue lw = JS_GetPropertyStr(ctx, tv, "lineWidth");
    double lwv = 1.0;
    JS_ToFloat64(ctx, &lwv, lw);
    JS_FreeValue(ctx, lw);
    if (ctx2d_is_onscreen(ctx, tv))
        mini_2d_stroke(r, g, b, a, (float)lwv);
    return JS_UNDEFINED;
}
static const void *js_typed_data(JSContext *ctx, JSValueConst v, size_t *out_len);
#ifdef _WIN32
static void render_text_to_buffer_win32(uint8_t *buf, int cw, int ch, const char *text, int x, int y, int font_size, uint8_t r, uint8_t g, uint8_t b, uint8_t a, const char *align);
#endif

static void js_free_ab_raw(JSRuntime *rt, void *opaque, void *ptr)
{
    (void)rt;
    (void)opaque;
    free(ptr);
}

static uint8_t *ctx2d_get_canvas_buffer(JSContext *ctx, JSValueConst tv, int *out_w, int *out_h)
{
    JSValue cv = JS_GetPropertyStr(ctx, tv, "_cv");
    if (!JS_IsObject(cv))
    {
        JS_FreeValue(ctx, cv);
        return NULL;
    }
    int32_t w = 0, h = 0;
    JSValue wv = JS_GetPropertyStr(ctx, cv, "width");
    JSValue hv = JS_GetPropertyStr(ctx, cv, "height");
    JS_ToInt32(ctx, &w, wv);
    JS_ToInt32(ctx, &h, hv);
    JS_FreeValue(ctx, wv);
    JS_FreeValue(ctx, hv);
    if (w <= 0 || h <= 0)
    {
        JS_FreeValue(ctx, cv);
        return NULL;
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;

    JSValue buf_val = JS_GetPropertyStr(ctx, cv, "__pixels");
    size_t expected_sz = (size_t)w * (size_t)h * 4;
    size_t cur_sz = 0;
    uint8_t *ptr = NULL;
    if (!JS_IsUndefined(buf_val) && !JS_IsNull(buf_val))
    {
        ptr = (uint8_t *)JS_GetArrayBuffer(ctx, &cur_sz, buf_val);
    }
    if (!ptr || cur_sz != expected_sz)
    {
        JS_FreeValue(ctx, buf_val);
        uint8_t *raw = (uint8_t *)calloc(1, expected_sz ? expected_sz : 4);
        if (!raw)
        {
            JS_FreeValue(ctx, cv);
            return NULL;
        }
        buf_val = JS_NewArrayBufferCopy(ctx, raw, expected_sz);
        free(raw);
        JS_SetPropertyStr(ctx, cv, "__pixels", JS_DupValue(ctx, buf_val));
        JS_SetPropertyStr(ctx, cv, "__buffer", JS_DupValue(ctx, buf_val));
        JS_SetPropertyStr(ctx, cv, "_data", JS_DupValue(ctx, buf_val));
        ptr = (uint8_t *)JS_GetArrayBuffer(ctx, &cur_sz, buf_val);
    }
    JS_FreeValue(ctx, buf_val);
    JS_FreeValue(ctx, cv);
    return ptr;
}

static JSValue js_ctx2d_fillRect(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    double x, y, w, h;
    JS_ToFloat64(ctx, &x, argv[0]);
    JS_ToFloat64(ctx, &y, argv[1]);
    JS_ToFloat64(ctx, &w, argv[2]);
    JS_ToFloat64(ctx, &h, argv[3]);
    float r, g, b, a;
    ctx2d_paint(ctx, tv, "fillStyle", &r, &g, &b, &a);
    if (ctx2d_is_onscreen(ctx, tv))
        mini_2d_fill_rect((float)x, (float)y, (float)w, (float)h, r, g, b, a);

    int cw = 0, ch = 0;
    uint8_t *buf = ctx2d_get_canvas_buffer(ctx, tv, &cw, &ch);
    if (buf && cw > 0 && ch > 0)
    {
        int x0 = (int)x; if (x0 < 0) x0 = 0;
        int y0 = (int)y; if (y0 < 0) y0 = 0;
        int x1 = (int)(x + w); if (x1 > cw) x1 = cw;
        int y1 = (int)(y + h); if (y1 > ch) y1 = ch;
        uint8_t ur = (uint8_t)(r * 255.0f);
        uint8_t ug = (uint8_t)(g * 255.0f);
        uint8_t ub = (uint8_t)(b * 255.0f);
        uint8_t ua = (uint8_t)(a * 255.0f);
        for (int row = y0; row < y1; row++)
        {
            uint8_t *dst = buf + (row * cw + x0) * 4;
            for (int col = x0; col < x1; col++)
            {
                dst[0] = ur;
                dst[1] = ug;
                dst[2] = ub;
                dst[3] = ua;
                dst += 4;
            }
        }
    }
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_clearRect(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)ctx;
    (void)argc;
    double x, y, w, h;
    JS_ToFloat64(ctx, &x, argv[0]);
    JS_ToFloat64(ctx, &y, argv[1]);
    JS_ToFloat64(ctx, &w, argv[2]);
    JS_ToFloat64(ctx, &h, argv[3]);
    if (ctx2d_is_onscreen(ctx, tv))
        mini_2d_clear_rect((float)x, (float)y, (float)w, (float)h);

    int cw = 0, ch = 0;
    uint8_t *buf = ctx2d_get_canvas_buffer(ctx, tv, &cw, &ch);
    if (buf && cw > 0 && ch > 0)
    {
        int x0 = (int)x; if (x0 < 0) x0 = 0;
        int y0 = (int)y; if (y0 < 0) y0 = 0;
        int x1 = (int)(x + w); if (x1 > cw) x1 = cw;
        int y1 = (int)(y + h); if (y1 > ch) y1 = ch;
        for (int row = y0; row < y1; row++)
        {
            uint8_t *dst = buf + (row * cw + x0) * 4;
            memset(dst, 0, (size_t)(x1 - x0) * 4);
        }
    }
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_save(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)ctx;
    (void)argc;
    (void)argv;
    if (ctx2d_is_onscreen(ctx, tv))
        mini_2d_save();
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_restore(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)ctx;
    (void)argc;
    (void)argv;
    if (ctx2d_is_onscreen(ctx, tv))
        mini_2d_restore();
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_translate(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    double x, y;
    JS_ToFloat64(ctx, &x, argv[0]);
    JS_ToFloat64(ctx, &y, argv[1]);
    if (ctx2d_is_onscreen(ctx, tv))
        mini_2d_translate((float)x, (float)y);
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_scale(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    double sx, sy;
    JS_ToFloat64(ctx, &sx, argv[0]);
    JS_ToFloat64(ctx, &sy, argv[1]);
    if (ctx2d_is_onscreen(ctx, tv))
        mini_2d_scale((float)sx, (float)sy);
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_rotate(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    double r;
    JS_ToFloat64(ctx, &r, argv[0]);
    if (ctx2d_is_onscreen(ctx, tv))
        mini_2d_rotate((float)r);
    return JS_UNDEFINED;
}

/* ---- Stage 3: text / curves / transforms / measure / hit-test / state ---- */
/* sync the ctx.font JS prop into the C font size (called before text ops) */
static void ctx2d_sync_font(JSContext *ctx, JSValueConst tv)
{
    JSValue fv = JS_GetPropertyStr(ctx, tv, "font");
    const char *fs = JS_ToCString(ctx, fv);
    if (fs)
        mini_2d_set_font(fs);
    JS_FreeCString(ctx, fs);
    JS_FreeValue(ctx, fv);
}
static JSValue js_ctx2d_fillText(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    const char *text = (argc >= 1) ? JS_ToCString(ctx, argv[0]) : "";
    int alloc = (text != NULL && argc >= 1);
    if (!text)
        text = "";
    double x = 0, y = 0, maxw = 1e9;
    if (argc >= 2)
        JS_ToFloat64(ctx, &x, argv[1]);
    if (argc >= 3)
        JS_ToFloat64(ctx, &y, argv[2]);
    if (argc >= 4)
        JS_ToFloat64(ctx, &maxw, argv[3]);
    ctx2d_sync_font(ctx, tv);
    float r, g, b, a;
    ctx2d_paint(ctx, tv, "fillStyle", &r, &g, &b, &a);
    if (ctx2d_is_onscreen(ctx, tv))
        mini_2d_fill_text(text, (float)x, (float)y, (float)maxw, r, g, b, a);

    int cw = 0, ch = 0;
    uint8_t *buf = ctx2d_get_canvas_buffer(ctx, tv, &cw, &ch);
    if (buf && cw > 0 && ch > 0)
    {
        JSValue fv = JS_GetPropertyStr(ctx, tv, "font");
        const char *fs = JS_ToCString(ctx, fv);
        int font_size = 30;
        if (fs)
        {
            const char *px_pos = strstr(fs, "px");
            if (px_pos)
            {
                const char *p = px_pos - 1;
                while (p >= fs && *p >= '0' && *p <= '9') p--;
                font_size = atoi(p + 1);
                if (font_size <= 0) font_size = 30;
            }
            JS_FreeCString(ctx, fs);
        }
        JS_FreeValue(ctx, fv);
        
        JSValue align_val = JS_GetPropertyStr(ctx, tv, "textAlign");
        const char *align_str = JS_ToCString(ctx, align_val);
        
#ifdef _WIN32
        render_text_to_buffer_win32(buf, cw, ch, text, (int)x, (int)y, font_size,
                                    (uint8_t)(r * 255.0f), (uint8_t)(g * 255.0f), (uint8_t)(b * 255.0f), (uint8_t)(a * 255.0f),
                                    align_str);
#endif
        if (align_str) JS_FreeCString(ctx, align_str);
        JS_FreeValue(ctx, align_val);
    }

    if (alloc)
        JS_FreeCString(ctx, text);
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_strokeText(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    const char *text = (argc >= 1) ? JS_ToCString(ctx, argv[0]) : "";
    int alloc = (text != NULL && argc >= 1);
    if (!text)
        text = "";
    double x = 0, y = 0, maxw = 1e9;
    if (argc >= 2)
        JS_ToFloat64(ctx, &x, argv[1]);
    if (argc >= 3)
        JS_ToFloat64(ctx, &y, argv[2]);
    if (argc >= 4)
        JS_ToFloat64(ctx, &maxw, argv[3]);
    ctx2d_sync_font(ctx, tv);
    float r, g, b, a;
    ctx2d_paint(ctx, tv, "strokeStyle", &r, &g, &b, &a);
    if (ctx2d_is_onscreen(ctx, tv))
        mini_2d_stroke_text(text, (float)x, (float)y, (float)maxw, r, g, b, a);
    if (alloc)
        JS_FreeCString(ctx, text);
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_measureText(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    const char *text = (argc >= 1) ? JS_ToCString(ctx, argv[0]) : "";
    int alloc = (text != NULL && argc >= 1);
    if (!text)
        text = "";
    ctx2d_sync_font(ctx, tv);
    float w = 0;
    mini_2d_measure_text(text, &w);
    if (alloc)
        JS_FreeCString(ctx, text);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "width", JS_NewFloat64(ctx, (double)w));
    return o;
}

/* curves: tessellate from the current pen into the path via line_to. */
static JSValue js_ctx2d_quadraticCurveTo(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    (void)argc;
    double cpx, cpy, x, y;
    JS_ToFloat64(ctx, &cpx, argv[0]);
    JS_ToFloat64(ctx, &cpy, argv[1]);
    JS_ToFloat64(ctx, &x, argv[2]);
    JS_ToFloat64(ctx, &y, argv[3]);
    float sx, sy;
    mini_2d_get_pen(&sx, &sy);
    for (int i = 1; i <= 16; i++)
    {
        float t = i / 16.0f, mt = 1.0f - t;
        float bx = mt * mt * sx + 2 * mt * t * (float)cpx + t * t * (float)x;
        float by = mt * mt * sy + 2 * mt * t * (float)cpy + t * t * (float)y;
        if (ctx2d_is_onscreen(ctx, tv))
            mini_2d_line_to(bx, by);
    }
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_bezierCurveTo(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    (void)argc;
    double x1, y1, x2, y2, x, y;
    JS_ToFloat64(ctx, &x1, argv[0]);
    JS_ToFloat64(ctx, &y1, argv[1]);
    JS_ToFloat64(ctx, &x2, argv[2]);
    JS_ToFloat64(ctx, &y2, argv[3]);
    JS_ToFloat64(ctx, &x, argv[4]);
    JS_ToFloat64(ctx, &y, argv[5]);
    float sx, sy;
    mini_2d_get_pen(&sx, &sy);
    for (int i = 1; i <= 20; i++)
    {
        float t = i / 20.0f, mt = 1.0f - t;
        float bx = mt * mt * mt * sx + 3 * mt * mt * t * (float)x1 + 3 * mt * t * t * (float)x2 + t * t * t * (float)x;
        float by = mt * mt * mt * sy + 3 * mt * mt * t * (float)y1 + 3 * mt * t * t * (float)y2 + t * t * t * (float)y;
        if (ctx2d_is_onscreen(ctx, tv))
            mini_2d_line_to(bx, by);
    }
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_arcTo(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    (void)argc;
    /* best-effort: draw straight segments to the tangent points (the true
       tangent-arc needs corner-geometry; full version = Stage 4).         */
    double x1, y1, x2, y2, r;
    JS_ToFloat64(ctx, &x1, argv[0]);
    JS_ToFloat64(ctx, &y1, argv[1]);
    JS_ToFloat64(ctx, &x2, argv[2]);
    JS_ToFloat64(ctx, &y2, argv[3]);
    JS_ToFloat64(ctx, &r, argv[4]);
    (void)r;
    if (ctx2d_is_onscreen(ctx, tv))
    {
        mini_2d_line_to((float)x1, (float)y1);
        mini_2d_line_to((float)x2, (float)y2);
    }
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_ellipse(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    (void)argc;
    double cx, cy, rx, ry, rot, a0, a1;
    int ccw = 0;
    JS_ToFloat64(ctx, &cx, argv[0]);
    JS_ToFloat64(ctx, &cy, argv[1]);
    JS_ToFloat64(ctx, &rx, argv[2]);
    JS_ToFloat64(ctx, &ry, argv[3]);
    JS_ToFloat64(ctx, &rot, argv[4]);
    JS_ToFloat64(ctx, &a0, argv[5]);
    JS_ToFloat64(ctx, &a1, argv[6]);
    if (argc > 7)
        JS_ToInt32(ctx, &ccw, argv[7]);
    if (ctx2d_is_onscreen(ctx, tv))
    {
        int steps = 32;
        float da = (float)(a1 - a0) / steps;
        for (int i = 0; i <= steps; i++)
        {
            float a = (float)a0 + i * da;
            float px = (float)(cx + rx * cos(a) * cos(rot) - ry * sin(a) * sin(rot));
            float py = (float)(cy + rx * cos(a) * sin(rot) + ry * sin(a) * cos(rot));
            if (i == 0)
                mini_2d_move_to(px, py);
            else
                mini_2d_line_to(px, py);
        }
    }
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_rect(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    (void)argc;
    double x, y, w, h;
    JS_ToFloat64(ctx, &x, argv[0]);
    JS_ToFloat64(ctx, &y, argv[1]);
    JS_ToFloat64(ctx, &w, argv[2]);
    JS_ToFloat64(ctx, &h, argv[3]);
    if (ctx2d_is_onscreen(ctx, tv))
    {
        mini_2d_move_to((float)x, (float)y);
        mini_2d_line_to((float)(x + w), (float)y);
        mini_2d_line_to((float)(x + w), (float)(y + h));
        mini_2d_line_to((float)x, (float)(y + h));
        mini_2d_close_path();
    }
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_roundRect(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    /* best-effort: a plain rect (the rounded corners need arc tessellation
       at each corner; full version = Stage 4). argv[4] = radii.            */
    return js_ctx2d_rect(ctx, tv, 4, argv);
}
static JSValue js_ctx2d_strokeRect(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    double x, y, w, h;
    JS_ToFloat64(ctx, &x, argv[0]);
    JS_ToFloat64(ctx, &y, argv[1]);
    JS_ToFloat64(ctx, &w, argv[2]);
    JS_ToFloat64(ctx, &h, argv[3]);
    float r, g, b, a;
    ctx2d_paint(ctx, tv, "strokeStyle", &r, &g, &b, &a);
    JSValue lw = JS_GetPropertyStr(ctx, tv, "lineWidth");
    double lwv = 1.0;
    JS_ToFloat64(ctx, &lwv, lw);
    JS_FreeValue(ctx, lw);
    if (ctx2d_is_onscreen(ctx, tv))
        mini_2d_stroke_rect((float)x, (float)y, (float)w, (float)h, r, g, b, a, (float)lwv);
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_transform(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    double a, b, c, d, e, f;
    JS_ToFloat64(ctx, &a, argv[0]);
    JS_ToFloat64(ctx, &b, argv[1]);
    JS_ToFloat64(ctx, &c, argv[2]);
    JS_ToFloat64(ctx, &d, argv[3]);
    JS_ToFloat64(ctx, &e, argv[4]);
    JS_ToFloat64(ctx, &f, argv[5]);
    if (ctx2d_is_onscreen(ctx, tv))
        mini_2d_transform((float)a, (float)b, (float)c, (float)d, (float)e, (float)f);
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_setTransform(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    double a, b, c, d, e, f;
    JS_ToFloat64(ctx, &a, argv[0]);
    JS_ToFloat64(ctx, &b, argv[1]);
    JS_ToFloat64(ctx, &c, argv[2]);
    JS_ToFloat64(ctx, &d, argv[3]);
    JS_ToFloat64(ctx, &e, argv[4]);
    JS_ToFloat64(ctx, &f, argv[5]);
    if (ctx2d_is_onscreen(ctx, tv))
        mini_2d_set_transform((float)a, (float)b, (float)c, (float)d, (float)e, (float)f);
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_resetTransform(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)ctx;
    (void)argc;
    (void)argv;
    if (ctx2d_is_onscreen(ctx, tv))
        mini_2d_reset_transform();
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_setLineDash(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    /* dash rendering is Stage 4; store the array so the surface is complete. */
    (void)argc;
    JS_SetPropertyStr(ctx, tv, "_lineDash", JS_DupValue(ctx, argv[0]));
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_getLineDash(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    JSValue v = JS_GetPropertyStr(ctx, tv, "_lineDash");
    return JS_IsUndefined(v) ? JS_NewArray(ctx) : v;
}
static JSValue js_ctx2d_isPointInPath(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    double x, y;
    JS_ToFloat64(ctx, &x, argv[argc - 2]);
    JS_ToFloat64(ctx, &y, argv[argc - 1]);
    int r = 0;
    mini_2d_is_point_in_path((float)x, (float)y, &r);
    return JS_NewBool(ctx, r);
}
static JSValue js_ctx2d_isPointInStroke(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    /* best-effort: same point-in-path test (stroke-width test = Stage 4). */
    return js_ctx2d_isPointInPath(ctx, tv, argc, argv);
}
static JSValue js_ctx2d_clip(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    /* full path/stencil clipping = Stage 4 (needs the persistent FBO).      */
    (void)ctx;
    (void)tv;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

/* ---- gradients / pattern ---- */
static JSValue js_grad_addColorStop(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    const char *col = JS_ToCString(ctx, argv[1]);
    JSValue s0 = JS_GetPropertyStr(ctx, tv, "_set0");
    int isset = JS_ToBool(ctx, s0);
    JS_FreeValue(ctx, s0);
    if (!isset)
    {
        JS_SetPropertyStr(ctx, tv, "_c0", JS_NewString(ctx, col ? col : "#000000"));
        JS_SetPropertyStr(ctx, tv, "_set0", JS_TRUE);
    }
    JS_SetPropertyStr(ctx, tv, "_c1", JS_NewString(ctx, col ? col : "#000000"));
    JS_FreeCString(ctx, col);
    return JS_UNDEFINED;
}
static JSValue make_grad(JSContext *ctx, int type)
{
    JSValue g = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, g, "_isGrad", JS_TRUE);
    JS_SetPropertyStr(ctx, g, "_set0", JS_FALSE);
    JS_SetPropertyStr(ctx, g, "_c0", JS_NewString(ctx, "#000000"));
    JS_SetPropertyStr(ctx, g, "_c1", JS_NewString(ctx, "#000000"));
    JS_SetPropertyStr(ctx, g, "_type", JS_NewInt32(ctx, type)); /* 0 lin 1 rad 2 conic */
    JS_SetPropertyStr(ctx, g, "addColorStop",
                      JS_NewCFunction(ctx, (JSCFunction *)js_grad_addColorStop, "addColorStop", 2));
    return g;
}
static JSValue js_createLinearGradient(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    (void)argc;
    (void)argv;
    return make_grad(ctx, 0);
}
static JSValue js_createRadialGradient(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    (void)argc;
    (void)argv;
    return make_grad(ctx, 1);
}
static JSValue js_createConicGradient(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    (void)argc;
    (void)argv;
    return make_grad(ctx, 2);
}
static JSValue js_createPattern(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    /* pattern tiling needs a backing texture (Stage 4). Return null so a
       `if (pattern)` guard works; drawing falls back to the fill color.    */
    (void)tv;
    (void)argc;
    (void)argv;
    return JS_NULL;
}

/* ---- images / pixels (best-effort; full = Stage 4 FBO path) ---- */
static JSValue js_ctx2d_createImageData(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    int w = 1, h = 1;
    if (argc >= 1)
        JS_ToInt32(ctx, &w, argv[0]);
    if (argc >= 2)
        JS_ToInt32(ctx, &h, argv[1]);
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, o, "height", JS_NewInt32(ctx, h));
    /* construct a real Uint8ClampedArray via `new` (JS_Call would throw
       "must be called with new"). Falls back to a plain Array on failure. */
    char jbuf[64];
    snprintf(jbuf, sizeof jbuf, "new Uint8ClampedArray(%d)", w * h * 4);
    JSValue arr = JS_Eval(ctx, jbuf, strlen(jbuf), "<imagedata>",
                          JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(arr))
    {
        JS_FreeValue(ctx, arr);
        arr = JS_NewArray(ctx);
    }
    JS_SetPropertyStr(ctx, o, "data", arr);
    return o;
}
static JSValue js_ctx2d_getImageData(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    /* best-effort: zero-filled ImageData (real pixel readback = Stage 4 FBO). */
    int w = 1, h = 1;
    if (argc >= 3)
        JS_ToInt32(ctx, &w, argv[2]);
    if (argc >= 4)
        JS_ToInt32(ctx, &h, argv[3]);
    JSValueConst a[2] = {JS_NewInt32(ctx, w), JS_NewInt32(ctx, h)};
    JSValue o = js_ctx2d_createImageData(ctx, tv, 2, a);
    JS_FreeValue(ctx, a[0]);
    JS_FreeValue(ctx, a[1]);
    return o;
}
#ifdef _WIN32
#include <windows.h>
static void render_text_to_buffer_win32(uint8_t *buf, int cw, int ch, const char *text, int x, int y, int font_size, uint8_t r, uint8_t g, uint8_t b, uint8_t a, const char *align)
{
    if (!buf || cw <= 0 || ch <= 0 || !text || !text[0]) return;
    
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (wlen <= 0) return;
    wchar_t *wstr = (wchar_t *)malloc(wlen * sizeof(wchar_t));
    if (!wstr) return;
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wstr, wlen);
    
    HDC hdc = CreateCompatibleDC(NULL);
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = cw;
    bi.bmiHeader.biHeight = -ch; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    
    void *dib_pixels = NULL;
    HBITMAP hbm = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &dib_pixels, NULL, 0);
    HGDIOBJ old_bm = SelectObject(hdc, hbm);
    
    memset(dib_pixels, 0, (size_t)cw * ch * 4);
    
    HFONT hFont = CreateFontW(-font_size, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"SimSun");
    HGDIOBJ old_font = SelectObject(hdc, hFont);
    
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    
    SIZE sz = {0};
    GetTextExtentPoint32W(hdc, wstr, wlen - 1, &sz);
    int draw_x = x;
    int draw_y = y - sz.cy / 2;
    if (align && !strcmp(align, "center"))
        draw_x = x - sz.cx / 2;
    else if (align && !strcmp(align, "right"))
        draw_x = x - sz.cx;
        
    TextOutW(hdc, draw_x, draw_y, wstr, wlen - 1);
    
    const uint8_t *src = (const uint8_t *)dib_pixels;
    for (int i = 0; i < cw * ch; i++)
    {
        uint8_t alpha = (uint8_t)(((int)src[i * 4 + 0] + (int)src[i * 4 + 1] + (int)src[i * 4 + 2]) / 3);
        if (alpha > 0)
        {
            float f_alpha = (alpha / 255.0f) * (a / 255.0f);
            uint8_t *dst = buf + i * 4;
            dst[0] = (uint8_t)(dst[0] * (1.0f - f_alpha) + r * f_alpha);
            dst[1] = (uint8_t)(dst[1] * (1.0f - f_alpha) + g * f_alpha);
            dst[2] = (uint8_t)(dst[2] * (1.0f - f_alpha) + b * f_alpha);
            dst[3] = 255;
        }
    }
    
    SelectObject(hdc, old_font);
    DeleteObject(hFont);
    SelectObject(hdc, old_bm);
    DeleteObject(hbm);
    DeleteDC(hdc);
    free(wstr);
}
#endif

static JSValue js_ctx2d_putImageData(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_UNDEFINED;
    JSValueConst imgData = argv[0];
    int32_t dx = 0, dy = 0;
    if (argc >= 2) JS_ToInt32(ctx, &dx, argv[1]);
    if (argc >= 3) JS_ToInt32(ctx, &dy, argv[2]);
    
    int cw = 0, ch = 0;
    uint8_t *dst_buf = ctx2d_get_canvas_buffer(ctx, tv, &cw, &ch);
    if (!dst_buf || cw <= 0 || ch <= 0)
        return JS_UNDEFINED;
        
    int32_t sw = 0, sh = 0;
    JSValue wv = JS_GetPropertyStr(ctx, imgData, "width");
    JSValue hv = JS_GetPropertyStr(ctx, imgData, "height");
    JS_ToInt32(ctx, &sw, wv);
    JS_ToInt32(ctx, &sh, hv);
    JS_FreeValue(ctx, wv);
    JS_FreeValue(ctx, hv);
    if (sw <= 0 || sh <= 0)
        return JS_UNDEFINED;
        
    JSValue data_arr = JS_GetPropertyStr(ctx, imgData, "data");
    size_t src_len = 0;
    const uint8_t *src_px = (const uint8_t *)js_typed_data(ctx, data_arr, &src_len);
    if (src_px)
    {
        int x0 = dx < 0 ? 0 : dx;
        int y0 = dy < 0 ? 0 : dy;
        int x1 = (dx + sw) > cw ? cw : (dx + sw);
        int y1 = (dy + sh) > ch ? ch : (dy + sh);
        for (int row = y0; row < y1; row++)
        {
            int s_row = row - dy;
            const uint8_t *s_ptr = src_px + (s_row * sw + (x0 - dx)) * 4;
            uint8_t *d_ptr = dst_buf + (row * cw + x0) * 4;
            memcpy(d_ptr, s_ptr, (size_t)(x1 - x0) * 4);
        }
    }
    JS_FreeValue(ctx, data_arr);
    return JS_UNDEFINED;
}
static JSValue js_ctx2d_drawImage(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    /* drawImage needs image sources backed by textures (Stage 4). No-op. */
    (void)ctx;
    (void)tv;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

/* ----------------------------------------------------------------- */
/* WebGL methods — thin mapping onto the resolved GL pointers.        */
/* getTypedData: pulls a raw pointer + length out of a TypedArray.     */
/* ----------------------------------------------------------------- */
static const void *js_typed_data(JSContext *ctx, JSValueConst v, size_t *out_len)
{
    size_t off = 0, len = 0, elt = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &elt);
    if (!JS_IsException(ab))
    {
        size_t ab_size = 0;
        uint8_t *p = JS_GetArrayBuffer(ctx, &ab_size, ab);
        JS_FreeValue(ctx, ab);
        if (p)
        {
            if (out_len)
                *out_len = len;
            return p + off;
        }
    }
    else
    {
        JS_FreeValue(ctx, ab);
    }
    size_t ab_size = 0;
    uint8_t *p = JS_GetArrayBuffer(ctx, &ab_size, v);
    if (p)
    {
        if (out_len)
            *out_len = ab_size;
        return p;
    }
    return NULL;
}

static float g_temp_uniform_floats[1024];
static int32_t g_temp_uniform_ints[1024];

static const void *js_float_data(JSContext *ctx, JSValueConst v, size_t *out_len)
{
    const void *p = js_typed_data(ctx, v, out_len);
    if (p)
        return p;
    if (JS_IsArray(v))
    {
        JSValue len_val = JS_GetPropertyStr(ctx, v, "length");
        int64_t len = 0;
        JS_ToInt64(ctx, &len, len_val);
        JS_FreeValue(ctx, len_val);
        if (len > 0)
        {
            if (len > 1024) len = 1024;
            for (int64_t i = 0; i < len; i++)
            {
                JSValue el = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
                double d = 0;
                JS_ToFloat64(ctx, &d, el);
                g_temp_uniform_floats[i] = (float)d;
                JS_FreeValue(ctx, el);
            }
            if (out_len)
                *out_len = (size_t)len * sizeof(float);
            return g_temp_uniform_floats;
        }
    }
    return NULL;
}

static const void *js_int_data(JSContext *ctx, JSValueConst v, size_t *out_len)
{
    const void *p = js_typed_data(ctx, v, out_len);
    if (p)
        return p;
    if (JS_IsArray(v))
    {
        JSValue len_val = JS_GetPropertyStr(ctx, v, "length");
        int64_t len = 0;
        JS_ToInt64(ctx, &len, len_val);
        JS_FreeValue(ctx, len_val);
        if (len > 0)
        {
            if (len > 1024) len = 1024;
            for (int64_t i = 0; i < len; i++)
            {
                JSValue el = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
                int32_t iv = 0;
                JS_ToInt32(ctx, &iv, el);
                g_temp_uniform_ints[i] = iv;
                JS_FreeValue(ctx, el);
            }
            if (out_len)
                *out_len = (size_t)len * sizeof(int32_t);
            return g_temp_uniform_ints;
        }
    }
    return NULL;
}

#define WGL(name) \
    static JSValue js_gl_##name

static JSValue js_gl_createShader(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    GLenum t;
    JS_ToInt32(ctx, (int32_t *)&t, argv[0]);
    GLuint s = b->gl->CreateShader ? b->gl->CreateShader(t) : 0;
    return JS_NewInt32(ctx, (int32_t)s);
}
static JSValue js_gl_shaderSource(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    GLuint sh;
    int32_t t;
    JS_ToInt32(ctx, &t, argv[0]);
    sh = (GLuint)t;
    const char *src = JS_ToCString(ctx, argv[1]);
    if (src && b->gl->ShaderSource)
    {
        char fname[64];
        snprintf(fname, sizeof(fname), "build/shader_%u.glsl", (unsigned)sh);
        FILE *fp = fopen(fname, "w");
        if (fp) { fputs(src, fp); fclose(fp); }
        const GLchar *s = src;
        b->gl->ShaderSource(sh, 1, &s, NULL);
    }
    JS_FreeCString(ctx, src);
    return JS_UNDEFINED;
}
static JSValue js_gl_compileShader(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t sh;
    JS_ToInt32(ctx, &sh, argv[0]);
    if (b->gl->CompileShader)
        b->gl->CompileShader((GLuint)sh);
    /* surface compile failures: a silently-failed shader links to a no-op
       program and renders nothing — the classic black-screen cause. */
    if (b->gl->GetShaderiv && b->gl->GetShaderInfoLog)
    {
        GLint ok = 0;
        b->gl->GetShaderiv((GLuint)sh, 0x8B81 /*COMPILE_STATUS*/, &ok);
        if (!ok)
        {
            char log[2048] = {0};
            GLsizei lr = 0;
            b->gl->GetShaderInfoLog((GLuint)sh, sizeof(log) - 1, &lr, log);
            fprintf(stderr, "[gl] shader %u COMPILE FAILED: %s\n", (unsigned)sh, log);
        }
    }
    return JS_UNDEFINED;
}
static JSValue js_gl_createProgram(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    GLuint p = b->gl->CreateProgram ? b->gl->CreateProgram() : 0;
    return JS_NewInt32(ctx, (int32_t)p);
}
static JSValue js_gl_attachShader(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t p, s;
    JS_ToInt32(ctx, &p, argv[0]);
    JS_ToInt32(ctx, &s, argv[1]);
    if (b->gl->AttachShader)
        b->gl->AttachShader((GLuint)p, (GLuint)s);
    return JS_UNDEFINED;
}
static JSValue js_gl_getAttribLocation(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t prog;
    JS_ToInt32(ctx, &prog, argv[0]);
    const char *name = JS_ToCString(ctx, argv[1]);

    GLint loc = -1;
    typedef GLint (*PFN_glGetAttribLocation)(GLuint, const GLchar *);
    PFN_glGetAttribLocation fn = (PFN_glGetAttribLocation)glfwGetProcAddress("glGetAttribLocation");
    if (fn && name)
    {
        loc = fn((GLuint)prog, name);
    }
    JS_FreeCString(ctx, name);
    return JS_NewInt32(ctx, loc >= 0 ? loc : 0);
}

/* gl.getActiveUniform/getActiveAttrib(program, index): Three.js introspects
   every active uniform/attribute when a program is first used
   (WebGLUniforms constructor + fetchAttributeLocations). Without these the
   first render threw "TypeError: not a function" at WebGLUniforms. Forward to
   the real glGetActiveUniform/glGetActiveAttrib and return a WebGLActiveInfo
   { name, size, type } object, matching the JS shape Three.js expects. */
static JSValue js_gl_getActiveUniform(JSContext *ctx, JSValueConst tv,
                                      int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 2)
        return JS_NULL;
    int32_t program = 0, index = 0;
    JS_ToInt32(ctx, &program, argv[0]);
    JS_ToInt32(ctx, &index, argv[1]);
    GLint size = 0;
    GLenum type = 0;
    GLchar name[256] = {0};
    typedef void (*PFN)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *);
    PFN fn = (PFN)glfwGetProcAddress("glGetActiveUniform");
    if (fn)
        fn((GLuint)program, (GLuint)index, (GLsizei)(sizeof(name) - 1),
           NULL, &size, &type, name);
    name[sizeof(name) - 1] = 0;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, o, "size", JS_NewInt32(ctx, size));
    JS_SetPropertyStr(ctx, o, "type", JS_NewInt32(ctx, (int32_t)type));
    return o;
}
static JSValue js_gl_getActiveAttrib(JSContext *ctx, JSValueConst tv,
                                     int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 2)
        return JS_NULL;
    int32_t program = 0, index = 0;
    JS_ToInt32(ctx, &program, argv[0]);
    JS_ToInt32(ctx, &index, argv[1]);
    GLint size = 0;
    GLenum type = 0;
    GLchar name[256] = {0};
    typedef void (*PFN)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *);
    PFN fn = (PFN)glfwGetProcAddress("glGetActiveAttrib");
    if (fn)
        fn((GLuint)program, (GLuint)index, (GLsizei)(sizeof(name) - 1),
           NULL, &size, &type, name);
    name[sizeof(name) - 1] = 0;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, o, "size", JS_NewInt32(ctx, size));
    JS_SetPropertyStr(ctx, o, "type", JS_NewInt32(ctx, (int32_t)type));
    return o;
}
static JSValue js_gl_linkProgram(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t p;
    JS_ToInt32(ctx, &p, argv[0]);
    if (b->gl->LinkProgram)
        b->gl->LinkProgram((GLuint)p);
    if (b->gl->GetProgramiv && b->gl->GetProgramInfoLog)
    {
        GLint ok = 0;
        b->gl->GetProgramiv((GLuint)p, 0x8B82 /*LINK_STATUS*/, &ok);
        if (!ok)
        {
            char log[2048] = {0};
            GLsizei lr = 0;
            b->gl->GetProgramInfoLog((GLuint)p, sizeof(log) - 1, &lr, log);
            fprintf(stderr, "[gl] program %u LINK FAILED: %s\n", (unsigned)p, log);
        }
    }
    return JS_UNDEFINED;
}
static JSValue js_gl_useProgram(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t p;
    JS_ToInt32(ctx, &p, argv[0]);
    if (b->gl->UseProgram)
        b->gl->UseProgram((GLuint)p);
    /* Save the program JS selected so mini_gl_bridge_restore_webgl can
       re-bind it before rAF fires (bridge_reset_2d unsets it for DOM). */
    b->gl->current_program = (GLuint)p;
    return JS_UNDEFINED;
}
static JSValue js_gl_createBuffer(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    GLuint id = 0;
    if (b->gl->GenBuffers)
        b->gl->GenBuffers(1, &id);
    return JS_NewInt32(ctx, (int32_t)id);
}
static JSValue js_gl_bindBuffer(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t target, id;
    JS_ToInt32(ctx, &target, argv[0]);
    JS_ToInt32(ctx, &id, argv[1]);
    if (b->gl->BindBuffer)
        b->gl->BindBuffer((GLenum)target, (GLuint)id);
    /* record so mini_gl_bridge_restore_webgl can re-bind before rAF */
    if (b->gl)
    {
        if (target == 0x8892 /*GL_ARRAY_BUFFER*/)
            b->gl->last_array_buffer = (GLuint)id;
        else if (target == 0x8893 /*GL_ELEMENT_ARRAY_BUFFER*/)
            b->gl->last_element_array_buffer = (GLuint)id;
    }
    return JS_UNDEFINED;
}
static JSValue js_gl_bufferData(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t target, usage;
    JS_ToInt32(ctx, &target, argv[0]);
    JS_ToInt32(ctx, &usage, argv[2]);
    size_t len = 0;
    const void *p = js_typed_data(ctx, argv[1], &len);
    if (p && b->gl->BufferData)
        b->gl->BufferData((GLenum)target, (GLsizeiptr)len, p, (GLenum)usage);
    return JS_UNDEFINED;
}
static JSValue js_gl_enableVA(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t i;
    JS_ToInt32(ctx, &i, argv[0]);
    if (b->gl->EnableVA)
        b->gl->EnableVA((GLuint)i);
    return JS_UNDEFINED;
}
static JSValue js_gl_vertexAttribPointer(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t idx, size, type, norm, stride, offset;
    JS_ToInt32(ctx, &idx, argv[0]);
    JS_ToInt32(ctx, &size, argv[1]);
    JS_ToInt32(ctx, &type, argv[2]);
    JS_ToInt32(ctx, (int32_t *)&norm, argv[3]);
    JS_ToInt32(ctx, &stride, argv[4]);
    JS_ToInt32(ctx, &offset, argv[5]);
    if (b->gl->VertexAttribP)
        b->gl->VertexAttribP((GLuint)idx, (GLint)size, (GLenum)type, (GLboolean)norm, (GLsizei)stride, (const void *)(intptr_t)offset);
    return JS_UNDEFINED;
}
static JSValue js_gl_drawArrays(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t mode, first, count;
    JS_ToInt32(ctx, &mode, argv[0]);
    JS_ToInt32(ctx, &first, argv[1]);
    JS_ToInt32(ctx, &count, argv[2]);
    if (b->gl->DrawArrays)
        b->gl->DrawArrays((GLenum)mode, (GLint)first, (GLsizei)count);
    else
        glDrawArrays((GLenum)mode, (GLint)first, (GLsizei)count);
    GLenum err = glGetError();
    if (err)
        fprintf(stderr, "[gl-err] drawArrays error=0x%X (%d), fbo=%u\n", (unsigned)err, (int)err, (unsigned)(b->gl ? b->gl->current_fbo : 0));
    return JS_UNDEFINED;
}
static JSValue js_gl_scissor(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    int32_t x, y, w, h;
    JS_ToInt32(ctx, &x, argv[0]);
    JS_ToInt32(ctx, &y, argv[1]);
    JS_ToInt32(ctx, &w, argv[2]);
    JS_ToInt32(ctx, &h, argv[3]);
    glScissor(x, y, w, h);
    return JS_UNDEFINED;
}
static JSValue js_gl_viewport(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t x, y, w, h;
    JS_ToInt32(ctx, &x, argv[0]);
    JS_ToInt32(ctx, &y, argv[1]);
    JS_ToInt32(ctx, &w, argv[2]);
    JS_ToInt32(ctx, &h, argv[3]);
    /* If rendering into an offscreen framebuffer (FBO != 0), do not anchor to DOM canvas rect or force window scissor */
    if (b && b->gl && b->gl->current_fbo != 0)
    {
        glViewport(x, y, w, h);
        return JS_UNDEFINED;
    }
    /* canvas-anchored compositing: translate canvas-local GL coords (bottom-left
       origin) into the canvas's window rect, and scissor draws to that rect so
       WebGL renders INTO the canvas region, not the whole window. */
    JSValue cvj = JS_GetPropertyStr(ctx, tv, "_cv");
    struct MiniNode *cv = (!JS_IsNull(cvj) && !JS_IsUndefined(cvj))
                              ? (struct MiniNode *)JS_GetOpaque2(ctx, cvj, b->el_cid)
                              : NULL;
    JS_FreeValue(ctx, cvj);
    if (cv && cv->style.w > 0 && cv->style.h > 0)
    {
        int wh = b->r->gpu.height;
        int gl_x = (int)cv->style.abs_x + x;
        int gl_y = (int)(wh - (cv->style.abs_y + cv->style.h)) + y;
        glViewport(gl_x, gl_y, w, h);
        glEnable(GL_SCISSOR_TEST);
        glScissor((int)cv->style.abs_x,
                  (int)(wh - (cv->style.abs_y + cv->style.h)),
                  (int)cv->style.w, (int)cv->style.h);
    }
    else
    {
        glViewport(x, y, w, h);
        glDisable(GL_SCISSOR_TEST);
    }
    return JS_UNDEFINED;
}
static JSValue js_gl_clearColor(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    double r, g, bl, a;
    JS_ToFloat64(ctx, &r, argv[0]);
    JS_ToFloat64(ctx, &g, argv[1]);
    JS_ToFloat64(ctx, &bl, argv[2]);
    JS_ToFloat64(ctx, &a, argv[3]);
    glClearColor((float)r, (float)g, (float)bl, (float)a);
    /* record so mini_gl_bridge_restore_webgl can re-apply before rAF (the DOM
       2D pass sets its own background each frame, which would otherwise leave
       the WebGL app clearing to the DOM color). */
    MiniBridge *b = bridge_of(ctx);
    if (b && b->gl)
    {
        b->gl->clear_r = (float)r; b->gl->clear_g = (float)g;
        b->gl->clear_b = (float)bl; b->gl->clear_a = (float)a;
        b->gl->has_clear = 1;
    }
    return JS_UNDEFINED;
}
static JSValue js_gl_clear(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    int32_t mask;
    JS_ToInt32(ctx, &mask, argv[0]);
    glClear((GLbitfield)mask);
    GLenum err = glGetError();
    if (err)
    {
        MiniBridge *b = bridge_of(ctx);
        fprintf(stderr, "[gl-err] glClear error=0x%X (%d), fbo=%u\n", (unsigned)err, (int)err, (unsigned)(b && b->gl ? b->gl->current_fbo : 0));
    }
    return JS_UNDEFINED;
}
static JSValue js_gl_getUniformLocation(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t p;
    JS_ToInt32(ctx, &p, argv[0]);
    const char *name = JS_ToCString(ctx, argv[1]);
    GLint loc = -1;
    if (name && b->gl->GetUniformLocation)
        loc = b->gl->GetUniformLocation((GLuint)p, name);
    JS_FreeCString(ctx, name);
    if (loc < 0)
        return JS_NULL;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "_loc", JS_NewInt32(ctx, (int32_t)loc));
    JS_SetPropertyStr(ctx, obj, "_prog", JS_NewInt32(ctx, (int32_t)p));
    return obj;
}

static inline int32_t get_uniform_loc(JSContext *ctx, JSValueConst v)
{
    if (JS_IsNull(v) || JS_IsUndefined(v))
        return -1;
    if (JS_IsObject(v))
    {
        JSValue lv = JS_GetPropertyStr(ctx, v, "_loc");
        int32_t loc = -1;
        JS_ToInt32(ctx, &loc, lv);
        JS_FreeValue(ctx, lv);
        return loc;
    }
    int32_t loc = -1;
    JS_ToInt32(ctx, &loc, v);
    return loc;
}

static JSValue js_gl_uniformMatrix4fv(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 3) return JS_UNDEFINED;
    MiniBridge *b = bridge_of(ctx);
    int32_t loc = get_uniform_loc(ctx, argv[0]);
    if (loc < 0) return JS_UNDEFINED;
    int32_t transpose;
    JS_ToInt32(ctx, (int32_t *)&transpose, argv[1]);
    size_t len = 0;
    const void *p = js_float_data(ctx, argv[2], &len);
    if (p && b && b->gl && b->gl->UniformMatrix4fv)
    {
        GLsizei count = (GLsizei)(len / (16 * sizeof(GLfloat)));
        if (count < 1) count = 1;
        b->gl->UniformMatrix4fv((GLint)loc, count, (GLboolean)transpose, (const GLfloat *)p);
    }
    return JS_UNDEFINED;
}

/* Shader/program introspection: without these, any WebGL app that checks
   gl.getShaderParameter(...,COMPILE_STATUS) throws (undefined is not a
   function) and aborts before linking -> black screen. */
static JSValue js_gl_getShaderParameter(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t sh, pname;
    JS_ToInt32(ctx, &sh, argv[0]);
    JS_ToInt32(ctx, &pname, argv[1]);
    GLint v = 0;
    if (b->gl->GetShaderiv)
        b->gl->GetShaderiv((GLuint)sh, (GLenum)pname, &v);
    return JS_NewBool(ctx, v != 0); /* COMPILE_STATUS / DELETE_STATUS */
}
static JSValue js_gl_getProgramParameter(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t p, pname;
    JS_ToInt32(ctx, &p, argv[0]);
    JS_ToInt32(ctx, &pname, argv[1]);
    GLint v = 0;
    if (b->gl->GetProgramiv)
        b->gl->GetProgramiv((GLuint)p, (GLenum)pname, &v);
    /* LINK_STATUS (0x8B82) / VALIDATE_STATUS (0x8B83) are booleans. But
       ACTIVE_ATTRIBUTES (0x8B89), ACTIVE_UNIFORMS (0x8B86), and the
       *_MAX_LENGTH queries are INTEGER counts/lengths. Returning a boolean
       for ACTIVE_ATTRIBUTES coerces the real count (e.g. 3) to 1, so Three.js's
       fetchAttributeLocations loop runs once and binds only the FIRST active
       attribute — the 'position' attribute is never located/bound, gl_Position
       collapses to a point, and the mesh renders no fragments (black). */
    if (pname == 0x8B82 /*LINK_STATUS*/ || pname == 0x8B83 /*VALIDATE_STATUS*/)
        return JS_NewBool(ctx, v != 0);
    return JS_NewInt32(ctx, v);
}
static JSValue js_gl_getShaderInfoLog(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t sh;
    JS_ToInt32(ctx, &sh, argv[0]);
    char log[2048] = {0};
    GLsizei lr = 0;
    if (b->gl->GetShaderInfoLog)
        b->gl->GetShaderInfoLog((GLuint)sh, sizeof(log) - 1, &lr, log);
    return JS_NewString(ctx, log);
}
static JSValue js_gl_getProgramInfoLog(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t p;
    JS_ToInt32(ctx, &p, argv[0]);
    char log[2048] = {0};
    GLsizei lr = 0;
    if (b->gl->GetProgramInfoLog)
        b->gl->GetProgramInfoLog((GLuint)p, sizeof(log) - 1, &lr, log);
    return JS_NewString(ctx, log);
}
static JSValue js_gl_createTexture(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    GLuint id = 0;
    if (b->gl->GenTextures)
        b->gl->GenTextures(1, &id);
    return JS_NewInt32(ctx, (int32_t)id);
}
static JSValue js_gl_bindTexture(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t target, id;
    JS_ToInt32(ctx, &target, argv[0]);
    JS_ToInt32(ctx, &id, argv[1]);
    if (b->gl->BindTexture)
        b->gl->BindTexture((GLenum)target, (GLuint)id);
    if (b->gl && target == 0x0DE1 /*GL_TEXTURE_2D*/)
        b->gl->last_texture_2d = (GLuint)id;
    return JS_UNDEFINED;
}

/* ---- WebGL state / texture / draw / query methods (the slice Three.js ----
   exercises beyond create/bind/draw above). GL 1.1+ entry points are looked
   up through glfwGetProcAddress (opengl32 only exports 1.1 statically).  */
static JSValue js_gl_enable(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t cap;
    JS_ToInt32(ctx, &cap, argv[0]);
    if (b && b->gl)
    {
        if (cap == 0x0B71 /*GL_DEPTH_TEST*/)
            b->gl->depth_test_enabled = 1;
        else if (cap == 0x0B44 /*GL_CULL_FACE*/)
            b->gl->cull_face_enabled = 1;
        else if (cap == 0x0BE2 /*GL_BLEND*/)
            b->gl->blend_enabled = 1;
        else if (cap == 0x0C11 /*GL_SCISSOR_TEST*/)
            b->gl->scissor_test_enabled = 1;
    }
    if (cap == 0x0B71 || cap == 0x0B44 || cap == 0x0BE2 || cap == 0x0C11 ||
        cap == 0x8037 /*POLYGON_OFFSET_FILL*/ || cap == 0x0B90 /*STENCIL_TEST*/ ||
        cap == 0x0BD0 /*DITHER*/ || cap == 0x8642 /*PROGRAM_POINT_SIZE*/)
    {
        glEnable((GLenum)cap);
    }
    return JS_UNDEFINED;
}
static JSValue js_gl_disable(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t cap;
    JS_ToInt32(ctx, &cap, argv[0]);
    if (b && b->gl)
    {
        if (cap == 0x0B71 /*GL_DEPTH_TEST*/)
            b->gl->depth_test_enabled = 0;
        else if (cap == 0x0B44 /*GL_CULL_FACE*/)
            b->gl->cull_face_enabled = 0;
        else if (cap == 0x0BE2 /*GL_BLEND*/)
            b->gl->blend_enabled = 0;
        else if (cap == 0x0C11 /*GL_SCISSOR_TEST*/)
            b->gl->scissor_test_enabled = 0;
    }
    if (cap == 0x0B71 || cap == 0x0B44 || cap == 0x0BE2 || cap == 0x0C11 ||
        cap == 0x8037 /*POLYGON_OFFSET_FILL*/ || cap == 0x0B90 /*STENCIL_TEST*/ ||
        cap == 0x0BD0 /*DITHER*/ || cap == 0x8642 /*PROGRAM_POINT_SIZE*/)
    {
        glDisable((GLenum)cap);
    }
    return JS_UNDEFINED;
}
static JSValue js_gl_pixelStorei(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    int32_t pname, param;
    JS_ToInt32(ctx, &pname, argv[0]);
    JS_ToInt32(ctx, &param, argv[1]);
    if (pname == 0x9240 /*UNPACK_FLIP_Y_WEBGL*/)
    {
        MiniBridge *b = bridge_of(ctx);
        if (b && b->gl)
            b->gl->unpack_flip_y = param ? 1 : 0;
        return JS_UNDEFINED;
    }
    if (pname == 0x9241 /*UNPACK_PREMULTIPLY_ALPHA_WEBGL*/ ||
        pname == 0x9243 /*UNPACK_COLORSPACE_CONVERSION_WEBGL*/ ||
        pname == 0x9242 /*UNPACK_UNPREMULTIPLY_ALPHA_WEBGL*/ ||
        pname >= 0x8000)
    {
        return JS_UNDEFINED;
    }
    while (glGetError() != GL_NO_ERROR);
    glPixelStorei((GLenum)pname, (GLint)param);
    return JS_UNDEFINED;
}
static JSValue js_gl_activeTexture(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t u;
    JS_ToInt32(ctx, &u, argv[0]);
    if (b && b->gl && b->gl->ActiveTexture)
        b->gl->ActiveTexture((GLenum)u);
    return JS_UNDEFINED;
}
static JSValue js_gl_texParameteri(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    int32_t t, p, v;
    JS_ToInt32(ctx, &t, argv[0]);
    JS_ToInt32(ctx, &p, argv[1]);
    JS_ToInt32(ctx, &v, argv[2]);
    glTexParameteri((GLenum)t, (GLenum)p, (GLint)v);
    return JS_UNDEFINED;
}
static JSValue js_gl_texImage2D(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t target = 0x0DE1, level = 0, ifmt = 0x1908, w = 0, h = 0, border = 0, fmt = 0x1908, type = 0x1401;
    const void *px = NULL;
    size_t len = 0;
    uint8_t *temp_px = NULL;
    int stbi_allocated = 0;

    if (argc >= 8)
    {
        JS_ToInt32(ctx, &target, argv[0]);
        JS_ToInt32(ctx, &level, argv[1]);
        JS_ToInt32(ctx, &ifmt, argv[2]);
        JS_ToInt32(ctx, &w, argv[3]);
        JS_ToInt32(ctx, &h, argv[4]);
        JS_ToInt32(ctx, &border, argv[5]);
        JS_ToInt32(ctx, &fmt, argv[6]);
        JS_ToInt32(ctx, &type, argv[7]);
        if (argc > 8 && !JS_IsUndefined(argv[8]) && !JS_IsNull(argv[8]))
            px = js_typed_data(ctx, argv[8], &len);
    }
    else if (argc >= 5)
    {
        JS_ToInt32(ctx, &target, argv[0]);
        JS_ToInt32(ctx, &level, argv[1]);
        JS_ToInt32(ctx, &ifmt, argv[2]);
        JS_ToInt32(ctx, &fmt, argv[3]);
        JS_ToInt32(ctx, &type, argv[4]);
        JSValueConst src = (argc >= 6) ? argv[5] : JS_UNDEFINED;
        if (target == 0)
        {
            fprintf(stderr, "[tex-debug-0] argc=%d:", argc);
            for (int i = 0; i < argc; i++)
            {
                int32_t val = 0;
                JS_ToInt32(ctx, &val, argv[i]);
                fprintf(stderr, " arg[%d]=0x%X(%d)", i, (unsigned)val, (int)val);
            }
            fprintf(stderr, "\n");
        }

        if (JS_IsObject(src))
        {
            JSValue wv = JS_GetPropertyStr(ctx, src, "width");
            JSValue hv = JS_GetPropertyStr(ctx, src, "height");
            if (!JS_IsUndefined(wv))
                JS_ToInt32(ctx, &w, wv);
            if (!JS_IsUndefined(hv))
                JS_ToInt32(ctx, &h, hv);
            JS_FreeValue(ctx, wv);
            JS_FreeValue(ctx, hv);

            JSValue data = JS_GetPropertyStr(ctx, src, "data");
            if (JS_IsUndefined(data) || JS_IsNull(data))
            {
                JS_FreeValue(ctx, data);
                data = JS_GetPropertyStr(ctx, src, "_data");
            }
            if (JS_IsUndefined(data) || JS_IsNull(data))
            {
                JS_FreeValue(ctx, data);
                data = JS_GetPropertyStr(ctx, src, "__buffer");
            }
            if (!JS_IsUndefined(data) && !JS_IsNull(data))
            {
                px = js_typed_data(ctx, data, &len);
                JS_FreeValue(ctx, data);
            }
            else
            {
                JS_FreeValue(ctx, data);

                /* Check if it's an Image with a .src or data URL or filepath */
                JSValue src_prop = JS_GetPropertyStr(ctx, src, "src");
                const char *src_str = JS_IsString(src_prop) ? JS_ToCString(ctx, src_prop) : NULL;

                if (src_str && src_str[0])
                {
                    int iw = 0, ih = 0, ic = 0;
                    unsigned char *dec_px = NULL;
                    if (!strncmp(src_str, "data:image/", 11))
                    {
                        const char *comma = strchr(src_str, ',');
                        if (comma)
                        {
                            size_t raw_len = strlen(comma + 1);
                            size_t out_len = 0;
                            uint8_t *bin = js_bridge_base64_decode(comma + 1, raw_len, &out_len);
                            if (bin && out_len > 0)
                            {
                                dec_px = stbi_load_from_memory(bin, (int)out_len, &iw, &ih, &ic, 4);
                                free(bin);
                            }
                        }
                    }
                    else if (!strncmp(src_str, "blob:", 5))
                    {
                        const char *fpath = NULL;
                        const uint8_t *bdata = NULL;
                        size_t bsize = 0;
                        if (mini_blob_lookup(src_str, &fpath, &bdata, &bsize, NULL))
                        {
                            if (fpath)
                                dec_px = stbi_load(fpath, &iw, &ih, &ic, 4);
                            else if (bdata && bsize > 0)
                                dec_px = stbi_load_from_memory(bdata, (int)bsize, &iw, &ih, &ic, 4);
                        }
                    }
                    else if (!strncmp(src_str, "file://", 7))
                    {
                        const char *fpath = src_str + 7;
                        if (!strncmp(fpath, "/", 1) && fpath[2] == ':')
                            fpath++;
                        dec_px = stbi_load(fpath, &iw, &ih, &ic, 4);
                    }
                    else
                    {
                        dec_px = stbi_load(src_str, &iw, &ih, &ic, 4);
                    }
                    if (dec_px)
                    {
                        w = iw;
                        h = ih;
                        px = dec_px;
                        stbi_allocated = 1;
                    }
                    JS_FreeCString(ctx, src_str);
                }
                JS_FreeValue(ctx, src_prop);

                if (!px && w > 0 && h > 0)
                {
                    size_t buf_size = (size_t)w * (size_t)h * 4;
                    temp_px = (uint8_t *)calloc(1, buf_size ? buf_size : 4);
                    if (temp_px)
                    {
                        for (size_t i = 0; i < (size_t)(w * h); i++)
                        {
                            temp_px[i * 4 + 0] = 220;
                            temp_px[i * 4 + 1] = 220;
                            temp_px[i * 4 + 2] = 220;
                            temp_px[i * 4 + 3] = 255;
                        }
                        px = temp_px;
                    }
                }
            }
        }
    }

    if (fmt == 0x8C42 /*GL_SRGB_ALPHA_EXT*/ || fmt == 0x8C43 /*GL_SRGB8_ALPHA8_EXT*/)
    {
        fmt = 0x1908 /*GL_RGBA*/;
        if (ifmt == 0x1908 || ifmt == 0x8C42)
            ifmt = 0x8C43 /*GL_SRGB8_ALPHA8*/;
    }
    else if (fmt == 0x8C40 /*GL_SRGB_EXT*/ || fmt == 0x8C41 /*GL_SRGB8*/)
    {
        fmt = 0x1907 /*GL_RGB*/;
        if (ifmt == 0x1907 || ifmt == 0x8C40)
            ifmt = 0x8C41 /*GL_SRGB8*/;
    }
    else if (ifmt == 0x8C42 /*GL_SRGB_ALPHA_EXT*/)
    {
        ifmt = 0x8C43 /*GL_SRGB8_ALPHA8*/;
    }
    else if (ifmt == 0x8C40 /*GL_SRGB_EXT*/)
    {
        ifmt = 0x8C41 /*GL_SRGB8*/;
    }

    if (target == 0)
        target = 0x0DE1 /*GL_TEXTURE_2D*/;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    if (type == 0x8D61 /*HALF_FLOAT_OES*/ || type == 0x140B /*GL_HALF_FLOAT*/)
    {
        type = 0x140B /*GL_HALF_FLOAT*/;
        if (ifmt == 0x1908 /*GL_RGBA*/ || ifmt == 0x881A)
            ifmt = 0x881A /*GL_RGBA16F*/;
        else if (ifmt == 0x1907 /*GL_RGB*/ || ifmt == 0x881B)
            ifmt = 0x881B /*GL_RGB16F*/;
    }
    else if (type == 0x1406 /*GL_FLOAT*/)
    {
        if (ifmt == 0x1908 /*GL_RGBA*/ || ifmt == 0x8814)
            ifmt = 0x8814 /*GL_RGBA32F*/;
        else if (ifmt == 0x1907 /*GL_RGB*/ || ifmt == 0x8815)
            ifmt = 0x8815 /*GL_RGB32F*/;
    }

    if (fmt == 0x84F9 /*GL_DEPTH_STENCIL*/)
    {
        if (ifmt == 0x84F9)
            ifmt = 0x88F0 /*GL_DEPTH24_STENCIL8*/;
    }
    else if (fmt == 0x1902 /*GL_DEPTH_COMPONENT*/)
    {
        if (ifmt == 0x1902)
        {
            if (type == 0x1403 /*UNSIGNED_SHORT*/)
                ifmt = 0x81A5 /*GL_DEPTH_COMPONENT16*/;
            else if (type == 0x1405 /*UNSIGNED_INT*/)
                ifmt = 0x81A6 /*GL_DEPTH_COMPONENT24*/;
            else if (type == 0x1406 /*FLOAT*/)
                ifmt = 0x8CAC /*GL_DEPTH_COMPONENT32F*/;
            else
                ifmt = 0x81A6 /*GL_DEPTH_COMPONENT24*/;
        }
    }

    if (b && b->gl && b->gl->unpack_flip_y && px && w > 0 && h > 1)
    {
        size_t row_bytes = (size_t)w * 4;
        size_t total_bytes = row_bytes * (size_t)h;
        uint8_t *flipped = (uint8_t *)malloc(total_bytes);
        if (flipped)
        {
            const uint8_t *src_bytes = (const uint8_t *)px;
            for (int y = 0; y < h; y++)
            {
                memcpy(flipped + ((size_t)(h - 1 - y) * row_bytes), src_bytes + ((size_t)y * row_bytes), row_bytes);
            }
            if (stbi_allocated)
                stbi_image_free((void *)px);
            if (temp_px)
                free(temp_px);
            px = flipped;
            temp_px = flipped;
            stbi_allocated = 0;
        }
    }

    while (glGetError() != GL_NO_ERROR);
    if (b && b->gl && b->gl->TexImage2D)
        b->gl->TexImage2D((GLenum)target, (GLint)level, (GLint)ifmt, (GLsizei)w, (GLsizei)h, (GLint)border, (GLenum)fmt, (GLenum)type, px);
    else
        glTexImage2D((GLenum)target, (GLint)level, (GLint)ifmt, (GLsizei)w, (GLsizei)h, (GLint)border, (GLenum)fmt, (GLenum)type, px);

    GLenum tex_err = glGetError();
    if (tex_err)
        fprintf(stderr, "[tex-err] texImage2D error=0x%X, target=0x%X, ifmt=0x%X, fmt=0x%X, type=0x%X, size=%dx%d\n",
                (unsigned)tex_err, (unsigned)target, (unsigned)ifmt, (unsigned)fmt, (unsigned)type, (int)w, (int)h);

    if (level == 0 && px == NULL)
    {
        glTexParameteri((GLenum)target, 0x2801 /*GL_TEXTURE_MIN_FILTER*/, 0x2601 /*GL_LINEAR*/);
        glTexParameteri((GLenum)target, 0x2800 /*GL_TEXTURE_MAG_FILTER*/, 0x2601 /*GL_LINEAR*/);
        glTexParameteri((GLenum)target, 0x2802 /*GL_TEXTURE_WRAP_S*/, 0x812F /*GL_CLAMP_TO_EDGE*/);
        glTexParameteri((GLenum)target, 0x2803 /*GL_TEXTURE_WRAP_T*/, 0x812F /*GL_CLAMP_TO_EDGE*/);
        if (fmt == 0x1902 /*GL_DEPTH_COMPONENT*/ || fmt == 0x84F9 /*GL_DEPTH_STENCIL*/)
        {
            glTexParameteri((GLenum)target, 0x884C /*GL_TEXTURE_COMPARE_MODE*/, 0 /*GL_NONE*/);
        }
    }

    if (stbi_allocated && px)
        stbi_image_free((void *)px);
    if (temp_px)
        free(temp_px);

    return JS_UNDEFINED;
}
static JSValue js_gl_drawElements(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t mode, count, type, off;
    JS_ToInt32(ctx, &mode, argv[0]);
    JS_ToInt32(ctx, &count, argv[1]);
    JS_ToInt32(ctx, &type, argv[2]);
    JS_ToInt32(ctx, &off, argv[3]);
    while (glGetError() != GL_NO_ERROR);
    if (b && b->gl && b->gl->DrawElements)
        b->gl->DrawElements((GLenum)mode, (GLsizei)count, (GLenum)type, (const void *)(intptr_t)off);
    else
        glDrawElements((GLenum)mode, (GLsizei)count, (GLenum)type, (const void *)(intptr_t)off);
    GLenum err = glGetError();
    if (err)
        fprintf(stderr, "[gl-err] drawElements error=0x%X, mode=0x%X, count=%d, type=0x%X, off=%d, fbo=%u\n",
                (unsigned)err, (unsigned)mode, (int)count, (unsigned)type, (int)off, (unsigned)(b && b->gl ? b->gl->current_fbo : 0));
    return JS_UNDEFINED;
}
static JSValue js_gl_createVertexArray(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    GLuint id = 0;
    typedef void (*PFN)(GLsizei, GLuint *);
    PFN f = (PFN)glfwGetProcAddress("glGenVertexArrays");
    if (f)
        f(1, &id);
    return JS_NewInt32(ctx, (int32_t)id);
}
static JSValue js_gl_bindVertexArray(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    int32_t id;
    JS_ToInt32(ctx, &id, argv[0]);
    MiniBridge *b = bridge_of(ctx);
    if (b && b->gl && b->gl->BindVertexArray)
        b->gl->BindVertexArray((GLuint)id);
    else
    {
        typedef void (*PFN)(GLuint);
        PFN f = (PFN)glfwGetProcAddress("glBindVertexArray");
        if (f)
            f((GLuint)id);
    }
    if (b && b->gl)
        b->gl->last_vao = (GLuint)id;
    return JS_UNDEFINED;
}
static JSValue js_gl_deleteVertexArray(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    int32_t id = 0;
    if (argc > 0)
        JS_ToInt32(ctx, &id, argv[0]);
    GLuint a = (GLuint)id;
    typedef void (*PFN)(GLsizei, const GLuint *);
    PFN f = (PFN)glfwGetProcAddress("glDeleteVertexArrays");
    if (f && a)
        f(1, &a);
    return JS_UNDEFINED;
}
static JSValue js_gl_readPixels(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    int32_t x, y, w, h, fmt, type;
    JS_ToInt32(ctx, &x, argv[0]);
    JS_ToInt32(ctx, &y, argv[1]);
    JS_ToInt32(ctx, &w, argv[2]);
    JS_ToInt32(ctx, &h, argv[3]);
    JS_ToInt32(ctx, &fmt, argv[4]);
    JS_ToInt32(ctx, &type, argv[5]);
    size_t len = 0;
    void *px = (void *)js_typed_data(ctx, argv[6], &len);
    if (px)
        glReadPixels((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, (GLenum)fmt, (GLenum)type, px);
    return JS_UNDEFINED;
}

/* Framebuffers / Renderbuffers */
static JSValue js_gl_createFramebuffer(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    GLuint id = 0;
    if (b && b->gl && b->gl->GenFramebuffers)
        b->gl->GenFramebuffers(1, &id);
    return JS_NewInt32(ctx, (int32_t)id);
}
static JSValue js_gl_bindFramebuffer(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t target, id = 0;
    JS_ToInt32(ctx, &target, argv[0]);
    if (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1]))
        JS_ToInt32(ctx, &id, argv[1]);
    if (b && b->gl)
        b->gl->current_fbo = (GLuint)id;
    if (b && b->gl && b->gl->BindFramebuffer)
        b->gl->BindFramebuffer((GLenum)target, (GLuint)id);
    typedef void (*PFN_glDrawBuffer)(GLenum);
    typedef void (*PFN_glReadBuffer)(GLenum);
    typedef void (*PFN_glGetFramebufferAttachmentParameteriv)(GLenum, GLenum, GLenum, GLint *);
    PFN_glDrawBuffer db = (PFN_glDrawBuffer)glfwGetProcAddress("glDrawBuffer");
    PFN_glReadBuffer rb = (PFN_glReadBuffer)glfwGetProcAddress("glReadBuffer");
    PFN_glGetFramebufferAttachmentParameteriv get_att = (PFN_glGetFramebufferAttachmentParameteriv)glfwGetProcAddress("glGetFramebufferAttachmentParameteriv");
    if (id == 0)
    {
        if (db) db(GL_BACK); else glDrawBuffer(GL_BACK);
        if (rb) rb(GL_BACK); else glReadBuffer(GL_BACK);
    }
    else if (get_att)
    {
        GLint color_type = 0;
        get_att((GLenum)target, 0x8CE0 /*GL_COLOR_ATTACHMENT0*/, 0x8210 /*GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE*/, &color_type);
        if (color_type != 0 /*GL_NONE*/)
        {
            if (db) db(0x8CE0 /*GL_COLOR_ATTACHMENT0*/); else glDrawBuffer(0x8CE0);
            if (rb) rb(0x8CE0 /*GL_COLOR_ATTACHMENT0*/); else glReadBuffer(0x8CE0);
        }
        else
        {
            if (db) db(0 /*GL_NONE*/); else glDrawBuffer(0);
            if (rb) rb(0 /*GL_NONE*/); else glReadBuffer(0);
        }
        GLenum fbo_st = (b && b->gl && b->gl->CheckFramebufferStatus) ? b->gl->CheckFramebufferStatus((GLenum)target) : 0;
        if (fbo_st != 0x8CD5 /*COMPLETE*/)
            fprintf(stderr, "[fbo-diag] bind FBO=%d: status=0x%X, color_type=0x%X\n", (int)id, (unsigned)fbo_st, (unsigned)color_type);
    }
    return JS_UNDEFINED;
}
static JSValue js_gl_framebufferTexture2D(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t target, attach, textarget, tex, level = 0;
    JS_ToInt32(ctx, &target, argv[0]);
    JS_ToInt32(ctx, &attach, argv[1]);
    JS_ToInt32(ctx, &textarget, argv[2]);
    JS_ToInt32(ctx, &tex, argv[3]);
    if (argc > 4)
        JS_ToInt32(ctx, &level, argv[4]);
    if (b && b->gl && b->gl->FramebufferTexture2D)
    {
        if (attach == 0x821A /*DEPTH_STENCIL_ATTACHMENT*/)
        {
            b->gl->FramebufferTexture2D((GLenum)target, 0x8D00 /*GL_DEPTH_ATTACHMENT*/, (GLenum)textarget, (GLuint)tex, (GLint)level);
            b->gl->FramebufferTexture2D((GLenum)target, 0x8D20 /*GL_STENCIL_ATTACHMENT*/, (GLenum)textarget, (GLuint)tex, (GLint)level);
        }
        else
        {
            b->gl->FramebufferTexture2D((GLenum)target, (GLenum)attach, (GLenum)textarget, (GLuint)tex, (GLint)level);
        }
        fprintf(stderr, "[fbo-att] FBO=%u attach=0x%X to tex=%d (level=%d)\n", (unsigned)(b && b->gl ? b->gl->current_fbo : 0), (unsigned)attach, (int)tex, (int)level);
        if (attach == 0x8CE0 /*GL_COLOR_ATTACHMENT0*/)
        {
            typedef void (*PFN_glDrawBuffer)(GLenum);
            typedef void (*PFN_glReadBuffer)(GLenum);
            PFN_glDrawBuffer db = (PFN_glDrawBuffer)glfwGetProcAddress("glDrawBuffer");
            PFN_glReadBuffer rb = (PFN_glReadBuffer)glfwGetProcAddress("glReadBuffer");
            if (tex != 0)
            {
                if (db) db(0x8CE0 /*GL_COLOR_ATTACHMENT0*/); else glDrawBuffer(0x8CE0);
                if (rb) rb(0x8CE0 /*GL_COLOR_ATTACHMENT0*/); else glReadBuffer(0x8CE0);
            }
            else
            {
                if (db) db(0 /*GL_NONE*/); else glDrawBuffer(0);
                if (rb) rb(0 /*GL_NONE*/); else glReadBuffer(0);
            }
        }
    }
    return JS_UNDEFINED;
}
static JSValue js_gl_drawBuffers(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1)
        return JS_UNDEFINED;
    JSValueConst arg = argv[0];
    if (JS_IsArray(arg))
    {
        JSValue lenv = JS_GetPropertyStr(ctx, arg, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, lenv);
        JS_FreeValue(ctx, lenv);
        if (len > 0 && len <= 16)
        {
            GLenum bufs[16];
            for (int i = 0; i < len; i++)
            {
                JSValue v = JS_GetPropertyUint32(ctx, arg, (uint32_t)i);
                int32_t b = 0;
                JS_ToInt32(ctx, &b, v);
                bufs[i] = (GLenum)b;
                JS_FreeValue(ctx, v);
            }
            typedef void (*PFN_glDrawBuffers)(GLsizei, const GLenum *);
            PFN_glDrawBuffers fn = (PFN_glDrawBuffers)glfwGetProcAddress("glDrawBuffers");
            if (fn)
                fn((GLsizei)len, bufs);
        }
    }
    return JS_UNDEFINED;
}
static JSValue js_gl_checkFramebufferStatus(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t target;
    JS_ToInt32(ctx, &target, argv[0]);
    GLenum st = 0x8CD5; /* GL_FRAMEBUFFER_COMPLETE */
    if (b && b->gl && b->gl->CheckFramebufferStatus)
    {
        st = b->gl->CheckFramebufferStatus((GLenum)target);
        if (st == 0x8CDB /* GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER */ || st == 0x8CD7 /* INCOMPLETE_MISSING_ATTACHMENT */)
        {
            typedef void (*PFN_glDrawBuffer)(GLenum);
            typedef void (*PFN_glReadBuffer)(GLenum);
            PFN_glDrawBuffer db = (PFN_glDrawBuffer)glfwGetProcAddress("glDrawBuffer");
            PFN_glReadBuffer rb = (PFN_glReadBuffer)glfwGetProcAddress("glReadBuffer");
            if (db) db(GL_NONE); else glDrawBuffer(GL_NONE);
            if (rb) rb(GL_NONE); else glReadBuffer(GL_NONE);
            st = b->gl->CheckFramebufferStatus((GLenum)target);
        }
        fprintf(stderr, "[fbo-check] checkFramebufferStatus FBO=%u -> 0x%X\n", (unsigned)(b->gl ? b->gl->current_fbo : 0), (unsigned)st);
    }
    return JS_NewInt32(ctx, (int32_t)st);
}
static JSValue js_gl_deleteFramebuffer(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t id;
    JS_ToInt32(ctx, &id, argv[0]);
    GLuint gid = (GLuint)id;
    if (b && b->gl && b->gl->DeleteFramebuffers && gid)
        b->gl->DeleteFramebuffers(1, &gid);
    return JS_UNDEFINED;
}
static JSValue js_gl_createRenderbuffer(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    GLuint id = 0;
    if (b && b->gl && b->gl->GenRenderbuffers)
        b->gl->GenRenderbuffers(1, &id);
    return JS_NewInt32(ctx, (int32_t)id);
}
static JSValue js_gl_bindRenderbuffer(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t target, id = 0;
    JS_ToInt32(ctx, &target, argv[0]);
    if (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1]))
        JS_ToInt32(ctx, &id, argv[1]);
    if (b && b->gl && b->gl->BindRenderbuffer)
        b->gl->BindRenderbuffer((GLenum)target, (GLuint)id);
    return JS_UNDEFINED;
}
static JSValue js_gl_renderbufferStorage(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t target, ifmt, w, h;
    JS_ToInt32(ctx, &target, argv[0]);
    JS_ToInt32(ctx, &ifmt, argv[1]);
    JS_ToInt32(ctx, &w, argv[2]);
    JS_ToInt32(ctx, &h, argv[3]);
    if (ifmt == 0 || ifmt == 0x1902 /*GL_DEPTH_COMPONENT*/ || ifmt == 0x81A5 /*GL_DEPTH_COMPONENT16*/ || ifmt == 0x81A6 /*GL_DEPTH_COMPONENT24*/ || ifmt == 0x8CAC /*GL_DEPTH_COMPONENT32F*/)
        ifmt = 0x81A6 /*GL_DEPTH_COMPONENT24*/;
    else if (ifmt == 0x84F9 /*GL_DEPTH_STENCIL*/ || ifmt == 0x88F0)
        ifmt = 0x88F0 /*GL_DEPTH24_STENCIL8*/;
    else if (ifmt == 0x1908 /*GL_RGBA*/ || ifmt == 0x8058 /*GL_RGBA8*/)
        ifmt = 0x8058 /*GL_RGBA8*/;
    else if (ifmt == 0x1907 /*GL_RGB*/ || ifmt == 0x8051 /*GL_RGB8*/)
        ifmt = 0x8051 /*GL_RGB8*/;
    if (b && b->gl && b->gl->RenderbufferStorage)
    {
        b->gl->RenderbufferStorage((GLenum)target, (GLenum)ifmt, (GLsizei)w, (GLsizei)h);
        GLenum rb_err = glGetError();
        if (rb_err)
            fprintf(stderr, "[rb-err] renderbufferStorage error=0x%X, target=0x%X, ifmt=0x%X, size=%dx%d\n",
                    (unsigned)rb_err, (unsigned)target, (unsigned)ifmt, (int)w, (int)h);
    }
    return JS_UNDEFINED;
}
static JSValue js_gl_framebufferRenderbuffer(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t target, attach, rbtarget, rb;
    JS_ToInt32(ctx, &target, argv[0]);
    JS_ToInt32(ctx, &attach, argv[1]);
    JS_ToInt32(ctx, &rbtarget, argv[2]);
    JS_ToInt32(ctx, &rb, argv[3]);
    if (b && b->gl && b->gl->FramebufferRenderbuffer)
    {
        if (attach == 0x821A /*DEPTH_STENCIL_ATTACHMENT*/)
        {
            b->gl->FramebufferRenderbuffer((GLenum)target, 0x8D00 /*GL_DEPTH_ATTACHMENT*/, (GLenum)rbtarget, (GLuint)rb);
            b->gl->FramebufferRenderbuffer((GLenum)target, 0x8D20 /*GL_STENCIL_ATTACHMENT*/, (GLenum)rbtarget, (GLuint)rb);
        }
        else
        {
            b->gl->FramebufferRenderbuffer((GLenum)target, (GLenum)attach, (GLenum)rbtarget, (GLuint)rb);
        }
        if (attach == 0x8CE0 /*GL_COLOR_ATTACHMENT0*/)
        {
            typedef void (*PFN_glDrawBuffer)(GLenum);
            typedef void (*PFN_glReadBuffer)(GLenum);
            PFN_glDrawBuffer db = (PFN_glDrawBuffer)glfwGetProcAddress("glDrawBuffer");
            PFN_glReadBuffer rb = (PFN_glReadBuffer)glfwGetProcAddress("glReadBuffer");
            if (db) db(0x8CE0 /*GL_COLOR_ATTACHMENT0*/); else glDrawBuffer(0x8CE0);
            if (rb) rb(0x8CE0 /*GL_COLOR_ATTACHMENT0*/); else glReadBuffer(0x8CE0);
        }
    }
    return JS_UNDEFINED;
}
static JSValue js_gl_deleteRenderbuffer(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t id;
    JS_ToInt32(ctx, &id, argv[0]);
    GLuint gid = (GLuint)id;
    if (b && b->gl && b->gl->DeleteRenderbuffers && gid)
        b->gl->DeleteRenderbuffers(1, &gid);
    return JS_UNDEFINED;
}

/* Deletions & Mipmap */
static JSValue js_gl_deleteTexture(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t id;
    JS_ToInt32(ctx, &id, argv[0]);
    GLuint gid = (GLuint)id;
    if (b && b->gl && b->gl->DeleteTextures && gid)
        b->gl->DeleteTextures(1, &gid);
    else if (gid)
        glDeleteTextures(1, &gid);
    return JS_UNDEFINED;
}
static JSValue js_gl_deleteBuffer(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t id;
    JS_ToInt32(ctx, &id, argv[0]);
    GLuint gid = (GLuint)id;
    if (b && b->gl && b->gl->DeleteBuffers && gid)
        b->gl->DeleteBuffers(1, &gid);
    return JS_UNDEFINED;
}
static JSValue js_gl_deleteProgram(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t id;
    JS_ToInt32(ctx, &id, argv[0]);
    if (b && b->gl && b->gl->DeleteProgram && id)
        b->gl->DeleteProgram((GLuint)id);
    return JS_UNDEFINED;
}
static JSValue js_gl_deleteShader(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t id;
    JS_ToInt32(ctx, &id, argv[0]);
    if (b && b->gl && b->gl->DeleteShader && id)
        b->gl->DeleteShader((GLuint)id);
    return JS_UNDEFINED;
}
static JSValue js_gl_generateMipmap(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t target;
    JS_ToInt32(ctx, &target, argv[0]);
    if (b && b->gl && b->gl->GenerateMipmap)
        b->gl->GenerateMipmap((GLenum)target);
    return JS_UNDEFINED;
}
static JSValue js_gl_texSubImage2D(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t target, level, xoff, yoff, w, h, fmt, type;
    JS_ToInt32(ctx, &target, argv[0]);
    JS_ToInt32(ctx, &level, argv[1]);
    JS_ToInt32(ctx, &xoff, argv[2]);
    JS_ToInt32(ctx, &yoff, argv[3]);
    JS_ToInt32(ctx, &w, argv[4]);
    JS_ToInt32(ctx, &h, argv[5]);
    JS_ToInt32(ctx, &fmt, argv[6]);
    JS_ToInt32(ctx, &type, argv[7]);
    size_t len = 0;
    const void *px = (argc > 8 && !JS_IsUndefined(argv[8]) && !JS_IsNull(argv[8])) ? js_typed_data(ctx, argv[8], &len) : NULL;
    if (b && b->gl && b->gl->TexSubImage2D)
        b->gl->TexSubImage2D((GLenum)target, (GLint)level, (GLint)xoff, (GLint)yoff, (GLsizei)w, (GLsizei)h, (GLenum)fmt, (GLenum)type, px);
    else
        glTexSubImage2D((GLenum)target, (GLint)level, (GLint)xoff, (GLint)yoff, (GLsizei)w, (GLsizei)h, (GLenum)fmt, (GLenum)type, px);
    return JS_UNDEFINED;
}

/* Blend / Depth / Cull / Stencil / PolygonOffset State */
static JSValue js_gl_blendFunc(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t s, d;
    JS_ToInt32(ctx, &s, argv[0]);
    JS_ToInt32(ctx, &d, argv[1]);
    if (b && b->gl && b->gl->BlendFunc)
        b->gl->BlendFunc((GLenum)s, (GLenum)d);
    else
        glBlendFunc((GLenum)s, (GLenum)d);
    return JS_UNDEFINED;
}
static JSValue js_gl_blendEquation(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t eq;
    JS_ToInt32(ctx, &eq, argv[0]);
    if (b && b->gl && b->gl->BlendEquation)
        b->gl->BlendEquation((GLenum)eq);
    return JS_UNDEFINED;
}
static JSValue js_gl_blendFuncSeparate(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t srcRGB, dstRGB, srcAlpha, dstAlpha;
    JS_ToInt32(ctx, &srcRGB, argv[0]);
    JS_ToInt32(ctx, &dstRGB, argv[1]);
    JS_ToInt32(ctx, &srcAlpha, argv[2]);
    JS_ToInt32(ctx, &dstAlpha, argv[3]);
    if (b && b->gl && b->gl->BlendFuncSeparate)
        b->gl->BlendFuncSeparate((GLenum)srcRGB, (GLenum)dstRGB, (GLenum)srcAlpha, (GLenum)dstAlpha);
    return JS_UNDEFINED;
}
static JSValue js_gl_blendEquationSeparate(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t eqRGB, eqAlpha;
    JS_ToInt32(ctx, &eqRGB, argv[0]);
    JS_ToInt32(ctx, &eqAlpha, argv[1]);
    if (b && b->gl && b->gl->BlendEquationSeparate)
        b->gl->BlendEquationSeparate((GLenum)eqRGB, (GLenum)eqAlpha);
    return JS_UNDEFINED;
}
static JSValue js_gl_depthFunc(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t func;
    JS_ToInt32(ctx, &func, argv[0]);
    if (b && b->gl && b->gl->DepthFunc)
        b->gl->DepthFunc((GLenum)func);
    else
        glDepthFunc((GLenum)func);
    return JS_UNDEFINED;
}
static JSValue js_gl_depthMask(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int flag = JS_ToBool(ctx, argv[0]);
    if (b && b->gl && b->gl->DepthMask)
        b->gl->DepthMask((GLboolean)flag);
    else
        glDepthMask((GLboolean)flag);
    return JS_UNDEFINED;
}
static JSValue js_gl_colorMask(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int r = JS_ToBool(ctx, argv[0]), g = JS_ToBool(ctx, argv[1]), bl = JS_ToBool(ctx, argv[2]), a = JS_ToBool(ctx, argv[3]);
    if (b && b->gl && b->gl->ColorMask)
        b->gl->ColorMask((GLboolean)r, (GLboolean)g, (GLboolean)bl, (GLboolean)a);
    else
        glColorMask((GLboolean)r, (GLboolean)g, (GLboolean)bl, (GLboolean)a);
    return JS_UNDEFINED;
}
static JSValue js_gl_cullFace(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t mode;
    JS_ToInt32(ctx, &mode, argv[0]);
    if (b && b->gl && b->gl->CullFace)
        b->gl->CullFace((GLenum)mode);
    else
        glCullFace((GLenum)mode);
    return JS_UNDEFINED;
}
static JSValue js_gl_frontFace(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t mode;
    JS_ToInt32(ctx, &mode, argv[0]);
    if (b && b->gl && b->gl->FrontFace)
        b->gl->FrontFace((GLenum)mode);
    else
        glFrontFace((GLenum)mode);
    return JS_UNDEFINED;
}
static JSValue js_gl_polygonOffset(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    double factor, units;
    JS_ToFloat64(ctx, &factor, argv[0]);
    JS_ToFloat64(ctx, &units, argv[1]);
    if (b && b->gl && b->gl->PolygonOffset)
        b->gl->PolygonOffset((GLfloat)factor, (GLfloat)units);
    else
        glPolygonOffset((GLfloat)factor, (GLfloat)units);
    return JS_UNDEFINED;
}
static JSValue js_gl_clearDepth(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    double depth;
    JS_ToFloat64(ctx, &depth, argv[0]);
    if (b && b->gl && b->gl->ClearDepth)
        b->gl->ClearDepth((GLdouble)depth);
    else
        glClearDepth((GLdouble)depth);
    return JS_UNDEFINED;
}
static JSValue js_gl_clearStencil(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t s;
    JS_ToInt32(ctx, &s, argv[0]);
    if (b && b->gl && b->gl->ClearStencil)
        b->gl->ClearStencil((GLint)s);
    else
        glClearStencil((GLint)s);
    return JS_UNDEFINED;
}
static JSValue js_gl_stencilMask(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t mask;
    JS_ToInt32(ctx, &mask, argv[0]);
    if (b && b->gl && b->gl->StencilMask)
        b->gl->StencilMask((GLuint)mask);
    else
        glStencilMask((GLuint)mask);
    return JS_UNDEFINED;
}
static JSValue js_gl_stencilFunc(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t func, ref, mask;
    JS_ToInt32(ctx, &func, argv[0]);
    JS_ToInt32(ctx, &ref, argv[1]);
    JS_ToInt32(ctx, &mask, argv[2]);
    if (b && b->gl && b->gl->StencilFunc)
        b->gl->StencilFunc((GLenum)func, (GLint)ref, (GLuint)mask);
    else
        glStencilFunc((GLenum)func, (GLint)ref, (GLuint)mask);
    return JS_UNDEFINED;
}
static JSValue js_gl_stencilOp(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t fail, zfail, zpass;
    JS_ToInt32(ctx, &fail, argv[0]);
    JS_ToInt32(ctx, &zfail, argv[1]);
    JS_ToInt32(ctx, &zpass, argv[2]);
    if (b && b->gl && b->gl->StencilOp)
        b->gl->StencilOp((GLenum)fail, (GLenum)zfail, (GLenum)zpass);
    else
        glStencilOp((GLenum)fail, (GLenum)zfail, (GLenum)zpass);
    return JS_UNDEFINED;
}

/* WebGL Uniforms */
static JSValue js_gl_uniform1f(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    MiniBridge *b = bridge_of(ctx);
    int32_t loc = get_uniform_loc(ctx, argv[0]);
    if (loc < 0) return JS_UNDEFINED;
    double x;
    JS_ToFloat64(ctx, &x, argv[1]);
    if (b && b->gl && b->gl->Uniform1f)
        b->gl->Uniform1f((GLint)loc, (GLfloat)x);
    return JS_UNDEFINED;
}
static JSValue js_gl_uniform2f(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 3) return JS_UNDEFINED;
    MiniBridge *b = bridge_of(ctx);
    int32_t loc = get_uniform_loc(ctx, argv[0]);
    if (loc < 0) return JS_UNDEFINED;
    double x, y;
    JS_ToFloat64(ctx, &x, argv[1]);
    JS_ToFloat64(ctx, &y, argv[2]);
    if (b && b->gl && b->gl->Uniform2f)
        b->gl->Uniform2f((GLint)loc, (GLfloat)x, (GLfloat)y);
    return JS_UNDEFINED;
}
static JSValue js_gl_uniform3f(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 4) return JS_UNDEFINED;
    MiniBridge *b = bridge_of(ctx);
    int32_t loc = get_uniform_loc(ctx, argv[0]);
    if (loc < 0) return JS_UNDEFINED;
    double x, y, z;
    JS_ToFloat64(ctx, &x, argv[1]);
    JS_ToFloat64(ctx, &y, argv[2]);
    JS_ToFloat64(ctx, &z, argv[3]);
    if (b && b->gl && b->gl->Uniform3f)
        b->gl->Uniform3f((GLint)loc, (GLfloat)x, (GLfloat)y, (GLfloat)z);
    return JS_UNDEFINED;
}
static JSValue js_gl_uniform4f(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 5) return JS_UNDEFINED;
    MiniBridge *b = bridge_of(ctx);
    int32_t loc = get_uniform_loc(ctx, argv[0]);
    if (loc < 0) return JS_UNDEFINED;
    double x, y, z, w;
    JS_ToFloat64(ctx, &x, argv[1]);
    JS_ToFloat64(ctx, &y, argv[2]);
    JS_ToFloat64(ctx, &z, argv[3]);
    JS_ToFloat64(ctx, &w, argv[4]);
    if (b && b->gl && b->gl->Uniform4f)
        b->gl->Uniform4f((GLint)loc, (GLfloat)x, (GLfloat)y, (GLfloat)z, (GLfloat)w);
    return JS_UNDEFINED;
}
static JSValue js_gl_uniform1i(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    MiniBridge *b = bridge_of(ctx);
    int32_t loc = get_uniform_loc(ctx, argv[0]);
    if (loc < 0) return JS_UNDEFINED;
    int32_t x;
    JS_ToInt32(ctx, &x, argv[1]);
    if (b && b->gl && b->gl->Uniform1i)
        b->gl->Uniform1i((GLint)loc, (GLint)x);
    return JS_UNDEFINED;
}
static JSValue js_gl_uniform2i(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 3) return JS_UNDEFINED;
    MiniBridge *b = bridge_of(ctx);
    int32_t loc = get_uniform_loc(ctx, argv[0]);
    if (loc < 0) return JS_UNDEFINED;
    int32_t x, y;
    JS_ToInt32(ctx, &x, argv[1]);
    JS_ToInt32(ctx, &y, argv[2]);
    if (b && b->gl && b->gl->Uniform2i)
        b->gl->Uniform2i((GLint)loc, (GLint)x, (GLint)y);
    return JS_UNDEFINED;
}
static JSValue js_gl_uniform3i(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 4) return JS_UNDEFINED;
    MiniBridge *b = bridge_of(ctx);
    int32_t loc = get_uniform_loc(ctx, argv[0]);
    if (loc < 0) return JS_UNDEFINED;
    int32_t x, y, z;
    JS_ToInt32(ctx, &x, argv[1]);
    JS_ToInt32(ctx, &y, argv[2]);
    JS_ToInt32(ctx, &z, argv[3]);
    if (b && b->gl && b->gl->Uniform3i)
        b->gl->Uniform3i((GLint)loc, (GLint)x, (GLint)y, (GLint)z);
    return JS_UNDEFINED;
}
static JSValue js_gl_uniform4i(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 5) return JS_UNDEFINED;
    MiniBridge *b = bridge_of(ctx);
    int32_t loc = get_uniform_loc(ctx, argv[0]);
    if (loc < 0) return JS_UNDEFINED;
    int32_t x, y, z, w;
    JS_ToInt32(ctx, &x, argv[1]);
    JS_ToInt32(ctx, &y, argv[2]);
    JS_ToInt32(ctx, &z, argv[3]);
    JS_ToInt32(ctx, &w, argv[4]);
    if (b && b->gl && b->gl->Uniform4i)
        b->gl->Uniform4i((GLint)loc, (GLint)x, (GLint)y, (GLint)z, (GLint)w);
    return JS_UNDEFINED;
}
static JSValue js_gl_uniform1fv(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    MiniBridge *b = bridge_of(ctx);
    int32_t loc = get_uniform_loc(ctx, argv[0]);
    if (loc < 0) return JS_UNDEFINED;
    size_t len = 0;
    const void *p = js_float_data(ctx, argv[1], &len);
    if (p && b && b->gl && b->gl->Uniform1fv)
        b->gl->Uniform1fv((GLint)loc, (GLsizei)(len / sizeof(GLfloat)), (const GLfloat *)p);
    return JS_UNDEFINED;
}
static JSValue js_gl_uniform2fv(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    MiniBridge *b = bridge_of(ctx);
    int32_t loc = get_uniform_loc(ctx, argv[0]);
    if (loc < 0) return JS_UNDEFINED;
    size_t len = 0;
    const void *p = js_float_data(ctx, argv[1], &len);
    if (p && b && b->gl && b->gl->Uniform2fv)
        b->gl->Uniform2fv((GLint)loc, (GLsizei)(len / (2 * sizeof(GLfloat))), (const GLfloat *)p);
    return JS_UNDEFINED;
}
static JSValue js_gl_uniform3fv(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    MiniBridge *b = bridge_of(ctx);
    int32_t loc = get_uniform_loc(ctx, argv[0]);
    if (loc < 0) return JS_UNDEFINED;
    size_t len = 0;
    const void *p = js_float_data(ctx, argv[1], &len);
    if (p && b && b->gl && b->gl->Uniform3fv)
        b->gl->Uniform3fv((GLint)loc, (GLsizei)(len / (3 * sizeof(GLfloat))), (const GLfloat *)p);
    return JS_UNDEFINED;
}
static JSValue js_gl_uniform4fv(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    MiniBridge *b = bridge_of(ctx);
    int32_t loc = get_uniform_loc(ctx, argv[0]);
    if (loc < 0) return JS_UNDEFINED;
    size_t len = 0;
    const void *p = js_float_data(ctx, argv[1], &len);
    if (p && b && b->gl && b->gl->Uniform4fv)
        b->gl->Uniform4fv((GLint)loc, (GLsizei)(len / (4 * sizeof(GLfloat))), (const GLfloat *)p);
    return JS_UNDEFINED;
}
static JSValue js_gl_uniform1iv(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    MiniBridge *b = bridge_of(ctx);
    int32_t loc = get_uniform_loc(ctx, argv[0]);
    if (loc < 0) return JS_UNDEFINED;
    size_t len = 0;
    const void *p = js_int_data(ctx, argv[1], &len);
    if (p && b && b->gl && b->gl->Uniform1iv)
        b->gl->Uniform1iv((GLint)loc, (GLsizei)(len / sizeof(GLint)), (const GLint *)p);
    return JS_UNDEFINED;
}
static JSValue js_gl_uniform2iv(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    MiniBridge *b = bridge_of(ctx);
    int32_t loc = get_uniform_loc(ctx, argv[0]);
    if (loc < 0) return JS_UNDEFINED;
    size_t len = 0;
    const void *p = js_int_data(ctx, argv[1], &len);
    if (p && b && b->gl && b->gl->Uniform2iv)
        b->gl->Uniform2iv((GLint)loc, (GLsizei)(len / (2 * sizeof(GLint))), (const GLint *)p);
    return JS_UNDEFINED;
}
static JSValue js_gl_uniform3iv(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    MiniBridge *b = bridge_of(ctx);
    int32_t loc = get_uniform_loc(ctx, argv[0]);
    if (loc < 0) return JS_UNDEFINED;
    size_t len = 0;
    const void *p = js_int_data(ctx, argv[1], &len);
    if (p && b && b->gl && b->gl->Uniform3iv)
        b->gl->Uniform3iv((GLint)loc, (GLsizei)(len / (3 * sizeof(GLint))), (const GLint *)p);
    return JS_UNDEFINED;
}
static JSValue js_gl_uniform4iv(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    MiniBridge *b = bridge_of(ctx);
    int32_t loc = get_uniform_loc(ctx, argv[0]);
    if (loc < 0) return JS_UNDEFINED;
    size_t len = 0;
    const void *p = js_int_data(ctx, argv[1], &len);
    if (p && b && b->gl && b->gl->Uniform4iv)
        b->gl->Uniform4iv((GLint)loc, (GLsizei)(len / (4 * sizeof(GLint))), (const GLint *)p);
    return JS_UNDEFINED;
}
static JSValue js_gl_uniformMatrix2fv(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 3) return JS_UNDEFINED;
    MiniBridge *b = bridge_of(ctx);
    int32_t loc = get_uniform_loc(ctx, argv[0]);
    if (loc < 0) return JS_UNDEFINED;
    int32_t transpose;
    JS_ToInt32(ctx, &transpose, argv[1]);
    size_t len = 0;
    const void *p = js_float_data(ctx, argv[2], &len);
    if (p && b && b->gl && b->gl->UniformMatrix2fv)
    {
        GLsizei count = (GLsizei)(len / (4 * sizeof(GLfloat)));
        if (count < 1) count = 1;
        b->gl->UniformMatrix2fv((GLint)loc, count, (GLboolean)transpose, (const GLfloat *)p);
    }
    return JS_UNDEFINED;
}
static JSValue js_gl_uniformMatrix3fv(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 3) return JS_UNDEFINED;
    MiniBridge *b = bridge_of(ctx);
    int32_t loc = get_uniform_loc(ctx, argv[0]);
    if (loc < 0) return JS_UNDEFINED;
    int32_t transpose;
    JS_ToInt32(ctx, &transpose, argv[1]);
    size_t len = 0;
    const void *p = js_float_data(ctx, argv[2], &len);
    if (p && b && b->gl && b->gl->UniformMatrix3fv)
    {
        GLsizei count = (GLsizei)(len / (9 * sizeof(GLfloat)));
        if (count < 1) count = 1;
        b->gl->UniformMatrix3fv((GLint)loc, count, (GLboolean)transpose, (const GLfloat *)p);
    }
    return JS_UNDEFINED;
}

/* query helpers — Three.js probes these at WebGLRenderer construction */
static JSValue js_gl_getParameter(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    int32_t pname;
    JS_ToInt32(ctx, &pname, argv[0]);
    if (pname == 0x1F02 /*VERSION*/ || pname == 0x8B8C /*SHADING_LANGUAGE_VERSION*/ ||
        pname == 0x1F00 /*VENDOR*/ || pname == 0x1F01 /*RENDERER*/)
    {
        const GLubyte *s = glGetString((GLenum)pname);
        return JS_NewString(ctx, s ? (const char *)s : "");
    }
    GLint iv = 0;
    glGetIntegerv((GLenum)pname, &iv);
    return JS_NewInt32(ctx, iv);
}
static JSValue js_gl_getExtension(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    const char *name = JS_ToCString(ctx, argv[0]);
    JSValue r = JS_NULL;
    if (name)
    {
        static const char *have[] = {
            "OES_element_index_uint", "OES_texture_float", "OES_texture_float_linear",
            "OES_texture_half_float", "OES_texture_half_float_linear",
            "OES_standard_derivatives", "EXT_texture_filter_anisotropic",
            "EXT_shader_texture_lod", "WEBGL_depth_texture",
            "OES_vertex_array_object", "WEBGL_compressed_texture_s3tc",
            "WEBGL_lose_context", "ANGLE_instanced_arrays",
            "EXT_sRGB", "EXT_blend_minmax", "WEBGL_color_buffer_float",
            "EXT_color_buffer_half_float", NULL};
        for (int i = 0; have[i]; i++)
            if (!strcmp(name, have[i]))
            {
                r = JS_NewObject(ctx);
                if (!strcmp(name, "EXT_sRGB"))
                {
                    JS_SetPropertyStr(ctx, r, "SRGB_EXT", JS_NewInt32(ctx, 0x8C40));
                    JS_SetPropertyStr(ctx, r, "SRGB_ALPHA_EXT", JS_NewInt32(ctx, 0x8C42));
                    JS_SetPropertyStr(ctx, r, "SRGB8_ALPHA8_EXT", JS_NewInt32(ctx, 0x8C43));
                    JS_SetPropertyStr(ctx, r, "FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING_EXT", JS_NewInt32(ctx, 0x8210));
                }
                if (!strcmp(name, "EXT_blend_minmax"))
                {
                    JS_SetPropertyStr(ctx, r, "MIN_EXT", JS_NewInt32(ctx, 0x8007));
                    JS_SetPropertyStr(ctx, r, "MAX_EXT", JS_NewInt32(ctx, 0x8008));
                }
                if (!strcmp(name, "EXT_color_buffer_half_float"))
                {
                    JS_SetPropertyStr(ctx, r, "RGBA16F_EXT", JS_NewInt32(ctx, 0x881A));
                    JS_SetPropertyStr(ctx, r, "RGB16F_EXT", JS_NewInt32(ctx, 0x881B));
                    JS_SetPropertyStr(ctx, r, "FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE_EXT", JS_NewInt32(ctx, 0x8211));
                    JS_SetPropertyStr(ctx, r, "UNSIGNED_NORMALIZED_EXT", JS_NewInt32(ctx, 0x8C17));
                }
                if (!strcmp(name, "OES_texture_half_float"))
                    JS_SetPropertyStr(ctx, r, "HALF_FLOAT_OES", JS_NewInt32(ctx, 0x8D61));
                if (!strcmp(name, "WEBGL_depth_texture"))
                {
                    JS_SetPropertyStr(ctx, r, "UNSIGNED_INT_24_8_WEBGL", JS_NewInt32(ctx, 0x84FA));
                    JS_SetPropertyStr(ctx, r, "DEPTH_STENCIL", JS_NewInt32(ctx, 0x84F9));
                    JS_SetPropertyStr(ctx, r, "UNSIGNED_SHORT", JS_NewInt32(ctx, 0x1403));
                    JS_SetPropertyStr(ctx, r, "UNSIGNED_INT", JS_NewInt32(ctx, 0x1405));
                }
                /* OES_vertex_array_object: Three.js (WebGL1 path) calls
                   extension.createVertexArrayOES / bindVertexArrayOES /
                   deleteVertexArrayOES for every geometry's binding state.
                   The native gl.create/bind/deleteVertexArray already exist on
                   the WebGLRenderingContext prototype, so alias them under
                   the OES names. tv is the GL context; its prototype carries
                   the methods. */
                if (!strcmp(name, "OES_vertex_array_object"))
                {
                    JSValue proto = JS_GetPrototype(ctx, tv);
                    if (!JS_IsException(proto) && !JS_IsNull(proto))
                    {
                        JSValue fn;
                        fn = JS_GetPropertyStr(ctx, proto, "createVertexArray");
                        JS_SetPropertyStr(ctx, r, "createVertexArrayOES", fn); /* steals */
                        fn = JS_GetPropertyStr(ctx, proto, "bindVertexArray");
                        JS_SetPropertyStr(ctx, r, "bindVertexArrayOES", fn);
                        fn = JS_GetPropertyStr(ctx, proto, "deleteVertexArray");
                        JS_SetPropertyStr(ctx, r, "deleteVertexArrayOES", fn);
                    }
                    JS_FreeValue(ctx, proto);
                }
                break;
            }
    }
    JS_FreeCString(ctx, name);
    return r;
}

/* gl.getContextAttributes(): Three.js's WebXRManager queries this at
   WebGLRenderer construction (it does `gl.getContextAttributes()` and reads
   .alpha). It is not implemented on the GL object by default, so the call
   threw "not a function" and aborted renderer creation. Return the standard
   WebGLContextAttributes object; the host GL context is a real desktop
   context with depth/stencil/alpha, so these defaults match. */
static JSValue js_gl_getContextAttributes(JSContext *ctx, JSValueConst tv,
                                          int argc, JSValueConst *argv)
{
    (void)tv;
    (void)argc;
    (void)argv;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "alpha", JS_TRUE);
    JS_SetPropertyStr(ctx, o, "depth", JS_TRUE);
    JS_SetPropertyStr(ctx, o, "stencil", JS_TRUE);
    JS_SetPropertyStr(ctx, o, "antialias", JS_TRUE);
    JS_SetPropertyStr(ctx, o, "premultipliedAlpha", JS_TRUE);
    JS_SetPropertyStr(ctx, o, "preserveDrawingBuffer", JS_FALSE);
    JS_SetPropertyStr(ctx, o, "powerPreference", JS_NewString(ctx, "default"));
    JS_SetPropertyStr(ctx, o, "failIfMajorPerformanceCaveat", JS_FALSE);
    return o;
}

/* gl.getError(): Three.js reads it when reporting a shader/program failure
   (WebGLProgram error path). Cheap to forward to the real glGetError; on a
   clean render it stays GL_NO_ERROR (0). */
static JSValue js_gl_getError(JSContext *ctx, JSValueConst tv,
                              int argc, JSValueConst *argv)
{
    (void)tv;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, (int32_t)glGetError());
}
static JSValue js_gl_getShaderPrecisionFormat(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    int32_t shaderType = 0, precType = 0;
    if (argc >= 1) JS_ToInt32(ctx, &shaderType, argv[0]);
    if (argc >= 2) JS_ToInt32(ctx, &precType, argv[1]);
    GLint rangeMin = 127, rangeMax = 127, precision = 23;
    switch (precType)
    {
        case 0x8DF2 /*HIGH_FLOAT*/: rangeMin = 127; rangeMax = 127; precision = 23; break;
        case 0x8DF1 /*MEDIUM_FLOAT*/: rangeMin = 15; rangeMax = 15; precision = 10; break;
        case 0x8DF0 /*LOW_FLOAT*/: rangeMin = 1; rangeMax = 1; precision = 8; break;
        case 0x8DF6 /*HIGH_INT*/: case 0x8DF5 /*MEDIUM_INT*/: case 0x8DF4 /*LOW_INT*/:
            rangeMin = 1; rangeMax = 1; precision = 16; break;
        default: break;
    }
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "rangeMin", JS_NewInt32(ctx, rangeMin));
    JS_SetPropertyStr(ctx, o, "rangeMax", JS_NewInt32(ctx, rangeMax));
    JS_SetPropertyStr(ctx, o, "precision", JS_NewInt32(ctx, precision));
    return o;
}

/* PointerLock APIs */
static JSValue js_requestPointerLock(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    MiniBridge *b = bridge_of(ctx);
    if (!b)
        return JS_UNDEFINED;
    struct MiniNode *n = (struct MiniNode *)JS_GetOpaque2(ctx, tv, b->el_cid);
    b->locked_node = n;
    if (b->r && b->r->gpu.window_handle)
    {
        glfwSetInputMode((GLFWwindow *)b->r->gpu.window_handle, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    }
    if (b->doc && b->doc->root && b->ev)
    {
        MiniEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.type = "pointerlockchange";
        ev.target = b->doc->root;
        ev.bubbles = 1;
        mini_event_dispatch(b->ev, &ev, b->doc->root);
    }
    return JS_UNDEFINED;
}

static JSValue js_exitPointerLock(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    (void)argc;
    (void)argv;
    MiniBridge *b = bridge_of(ctx);
    if (!b)
        return JS_UNDEFINED;
    b->locked_node = NULL;
    if (b->r && b->r->gpu.window_handle)
    {
        glfwSetInputMode((GLFWwindow *)b->r->gpu.window_handle, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
    if (b->doc && b->doc->root && b->ev)
    {
        MiniEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.type = "pointerlockchange";
        ev.target = b->doc->root;
        ev.bubbles = 1;
        mini_event_dispatch(b->ev, &ev, b->doc->root);
    }
    return JS_UNDEFINED;
}

static JSValue js_doc_pointerLockElement(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    (void)argc;
    (void)argv;
    MiniBridge *b = bridge_of(ctx);
    if (!b || !b->locked_node)
        return JS_NULL;
    return wrap_node(ctx, b->locked_node, b->el_cid);
}

/* ----------------------------------------------------------------- */
/* window + timers + console                                         */
/* ----------------------------------------------------------------- */
static JSValue js_requestAnimationFrame(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    if (b->raf_n >= b->raf_cap)
    {
        int nc = b->raf_cap ? b->raf_cap * 2 : 8;
        MiniRafEntry *nb = (MiniRafEntry *)realloc(b->raf, sizeof(MiniRafEntry) * nc);
        if (!nb)
            return JS_ThrowTypeError(ctx, "rAF: OOM");
        b->raf = nb;
        b->raf_cap = nc;
    }
    int id = ++b->raf_next_id;
    b->raf[b->raf_n].cb = JS_DupValue(ctx, argv[0]);
    b->raf[b->raf_n].id = id;
    b->raf_n++;
    return JS_NewInt32(ctx, id);
}
static JSValue js_cancelAnimationFrame(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    int32_t id;
    JS_ToInt32(ctx, &id, argv[0]);
    /* find the pending callback by handle and splice it out (the previous
       body was `if(1)` — it matched nothing and never cancelled). */
    for (int i = 0; i < b->raf_n; i++)
    {
        if (b->raf[i].id == id)
        {
            JS_FreeValue(ctx, b->raf[i].cb);
            for (int j = i; j < b->raf_n - 1; j++)
                b->raf[j] = b->raf[j + 1];
            b->raf_n--;
            break;
        }
    }
    return JS_UNDEFINED;
}
/* Shared core for setTimeout (recurring=0) and setInterval (recurring=1).
   Returns the stable id JS uses for clearTimeout/clearInterval. */
static JSValue add_timer(JSContext *ctx, MiniBridge *b, JSValueConst cb,
                         double ms, int recurring)
{
    if (b->tm_n >= b->tm_cap)
    {
        int nc = b->tm_cap ? b->tm_cap * 2 : 8;
        MiniTimer *nt = (MiniTimer *)realloc(b->timers, sizeof(MiniTimer) * nc);
        if (!nt)
            return JS_ThrowTypeError(ctx, "timer: OOM");
        b->timers = nt;
        b->tm_cap = nc;
    }
    int id = ++b->tm_next_id;
    b->timers[b->tm_n].cb = JS_DupValue(ctx, cb);
    b->timers[b->tm_n].due_ms = glfwGetTime() * 1000.0 + (ms > 0 ? ms : 0);
    b->timers[b->tm_n].interval_ms = recurring ? (ms > 0 ? ms : 0) : 0;
    b->timers[b->tm_n].id = id;
    b->timers[b->tm_n].recurring = recurring ? 1 : 0;
    b->tm_n++;
    return JS_NewInt32(ctx, id);
}
static JSValue js_setTimeout(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    MiniBridge *b = bridge_of(ctx);
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "setTimeout: callback required");
    double ms = 0;
    if (argc >= 2)
        JS_ToFloat64(ctx, &ms, argv[1]);
    return add_timer(ctx, b, argv[0], ms, 0);
}
static JSValue js_setInterval(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    MiniBridge *b = bridge_of(ctx);
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "setInterval: callback required");
    double ms = 0;
    if (argc >= 2)
        JS_ToFloat64(ctx, &ms, argv[1]);
    return add_timer(ctx, b, argv[0], ms, 1);
}
/* clearTimeout(id) / clearInterval(id): cancel a pending timer by its id.
   Both share the same id space because setInterval is now native (not a
   setTimeout-loop re-armed under changing ids). */
static JSValue js_clearTimer(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    MiniBridge *b = bridge_of(ctx);
    if (argc < 1)
        return JS_UNDEFINED;
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    for (int i = 0; i < b->tm_n; i++)
    {
        if (b->timers[i].id == id)
        {
            JS_FreeValue(ctx, b->timers[i].cb);
            b->timers[i] = b->timers[--b->tm_n];
            return JS_UNDEFINED;
        }
    }
    return JS_UNDEFINED;
}
static JSValue js_console_log(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    MiniBridge *b = bridge_of(ctx);
    (void)tv;
    /* argv[0] = level string ("log"|"info"|"debug"|"trace"|"warning"|"error"),
       argv[1] = pre-joined message text. The JS shim already JSON-stringified
       object arguments before calling __log. */
    char level[16] = "log";
    const char *msg = "";
    int msg_alloc = 0;
    if (argc >= 1)
    {
        const char *lv = JS_ToCString(ctx, argv[0]);
        if (lv)
        {
            snprintf(level, sizeof level, "%s", lv);
            JS_FreeCString(ctx, lv);
        }
    }
    if (argc >= 2)
    {
        const char *m = JS_ToCString(ctx, argv[1]);
        if (m)
        {
            msg = m;
            msg_alloc = 1;
        }
    }
    /* Route every console.* line through the structured logger so it lands in
       the ring buffer + persistent file (the "background output" the host / a
       JS listener can poll) as well as the live CDP relay below. The terminal
       mirror is governed by mini_log's stderr threshold (default WARN). */
    MiniLogLevel ml = MINI_LOG_INFO;
    if (!strcmp(level, "warning") || !strcmp(level, "warn"))
        ml = MINI_LOG_WARN;
    else if (!strcmp(level, "error"))
        ml = MINI_LOG_ERROR;
    else if (!strcmp(level, "debug"))
        ml = MINI_LOG_DEBUG;
    else if (!strcmp(level, "trace"))
        ml = MINI_LOG_TRACE;
    mini_logf(ml, "js", "%s", msg);
    /* CDP Runtime.consoleAPICalled relay (kept direct so DevTools always
       sees every console call regardless of the log emit threshold). */
    if (b && b->log_hook)
        b->log_hook(level, msg, b->log_ud);
    if (msg_alloc)
        JS_FreeCString(ctx, msg);
    return JS_UNDEFINED;
}
/* JS log background-listener: returns the entries emitted since `cursor`
   (a mini_log_count() value the caller persisted) plus the current total. */
static JSValue js_log_poll(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    unsigned long cursor = 0;
    if (argc >= 1)
    {
        double d = 0;
        JS_ToFloat64(ctx, &d, argv[0]);
        if (d > 0)
            cursor = (unsigned long)d;
    }
    unsigned long total = mini_log_count();
    MiniLogEntry entries[64];
    int n = mini_log_since(entries, 64, cursor);
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < n; i++)
    {
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "ts", JS_NewFloat64(ctx, entries[i].ts_ms));
        JS_SetPropertyStr(ctx, o, "level", JS_NewString(ctx, mini_log_level_str(entries[i].level)));
        JS_SetPropertyStr(ctx, o, "tag", JS_NewString(ctx, entries[i].tag[0] ? entries[i].tag : ""));
        JS_SetPropertyStr(ctx, o, "msg", JS_NewString(ctx, entries[i].msg));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
    }
    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "entries", arr);
    JS_SetPropertyStr(ctx, r, "total", JS_NewInt64(ctx, (int64_t)total));
    return r;
}
static JSValue js_now(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    return JS_NewFloat64(ctx, glfwGetTime() * 1000.0);
}

/* ================================================================== */
/* fetch() + localStorage/sessionStorage (in-memory) — for the CDP   */
/* Network + Storage domains. fetch is synchronous (the engine has no  */
/* event loop beyond rAF/setTimeout); `await fetch()` still works      */
/* because awaiting a non-Promise returns the value.                   */
/* ================================================================== */
typedef struct
{
    char *key;
    char *val;
} MiniKV;
static MiniKV g_storage[2][256]; /* [0]=localStorage, [1]=sessionStorage */
static int g_storage_n[2];

typedef struct MiniBlobEntry
{
    char *url;       /* "blob:null/1001" */
    char *file_path; /* Disk path if File object */
    uint8_t *data;   /* In-memory buffer if memory Blob */
    size_t size;
    char *mime_type;
} MiniBlobEntry;

#define MAX_BLOBS 1024
static MiniBlobEntry g_blobs[MAX_BLOBS];
static int g_blob_count = 0;
static uint32_t g_blob_id_seq = 1000;

static char *mc_strdup(const char *s)
{
    if (!s)
        return NULL;
    size_t n = strlen(s) + 1;
    char *r = (char *)malloc(n);
    if (r)
        memcpy(r, s, n);
    return r;
}

static char *mini_blob_register_file(const char *file_path, const char *mime_type)
{
    if (!file_path || g_blob_count >= MAX_BLOBS)
        return NULL;
    char url_buf[128];
    snprintf(url_buf, sizeof(url_buf), "blob:null/%u", g_blob_id_seq++);
    MiniBlobEntry *e = &g_blobs[g_blob_count++];
    e->url = mc_strdup(url_buf);
    e->file_path = mc_strdup(file_path);
    e->data = NULL;
    e->size = 0;
    e->mime_type = mc_strdup(mime_type ? mime_type : "application/octet-stream");
    return mc_strdup(url_buf);
}

static char *mini_blob_register_mem(const uint8_t *data, size_t size, const char *mime_type)
{
    if (g_blob_count >= MAX_BLOBS)
        return NULL;
    char url_buf[128];
    snprintf(url_buf, sizeof(url_buf), "blob:null/%u", g_blob_id_seq++);
    MiniBlobEntry *e = &g_blobs[g_blob_count++];
    e->url = mc_strdup(url_buf);
    e->file_path = NULL;
    if (data && size > 0)
    {
        e->data = (uint8_t *)malloc(size);
        if (e->data)
            memcpy(e->data, data, size);
        e->size = size;
    }
    else
    {
        e->data = NULL;
        e->size = 0;
    }
    e->mime_type = mc_strdup(mime_type ? mime_type : "application/octet-stream");
    return mc_strdup(url_buf);
}

static int mini_blob_lookup(const char *url, const char **out_path, const uint8_t **out_data, size_t *out_size, const char **out_type)
{
    if (!url)
        return 0;
    for (int i = 0; i < g_blob_count; i++)
    {
        if (g_blobs[i].url && !strcmp(g_blobs[i].url, url))
        {
            if (out_path) *out_path = g_blobs[i].file_path;
            if (out_data) *out_data = g_blobs[i].data;
            if (out_size) *out_size = g_blobs[i].size;
            if (out_type) *out_type = g_blobs[i].mime_type;
            return 1;
        }
    }
    return 0;
}

static void mini_blob_revoke(const char *url)
{
    if (!url)
        return;
    for (int i = 0; i < g_blob_count; i++)
    {
        if (g_blobs[i].url && !strcmp(g_blobs[i].url, url))
        {
            free(g_blobs[i].url);
            free(g_blobs[i].file_path);
            free(g_blobs[i].data);
            free(g_blobs[i].mime_type);
            memmove(&g_blobs[i], &g_blobs[i + 1], sizeof(MiniBlobEntry) * (size_t)(g_blob_count - 1 - i));
            g_blob_count--;
            return;
        }
    }
}

static int kv_find(int s, const char *k)
{
    for (int i = 0; i < g_storage_n[s & 1]; i++)
        if (g_storage[s & 1][i].key && !strcmp(g_storage[s & 1][i].key, k))
            return i;
    return -1;
}
static const char *kv_get(int s, const char *k)
{
    int i = kv_find(s, k);
    return i >= 0 ? g_storage[s & 1][i].val : NULL;
}
static void kv_set(int s, const char *k, const char *v)
{
    int i = kv_find(s, k);
    if (i >= 0)
    {
        free(g_storage[s & 1][i].val);
        g_storage[s & 1][i].val = mc_strdup(v);
        return;
    }
    if (g_storage_n[s & 1] < 256)
    {
        g_storage[s & 1][g_storage_n[s & 1]].key = mc_strdup(k);
        g_storage[s & 1][g_storage_n[s & 1]].val = mc_strdup(v);
        g_storage_n[s & 1]++;
    }
}
static void kv_del(int s, const char *k)
{
    int i = kv_find(s, k);
    if (i < 0)
        return;
    free(g_storage[s & 1][i].key);
    free(g_storage[s & 1][i].val);
    memmove(&g_storage[s & 1][i], &g_storage[s & 1][i + 1],
            (g_storage_n[s & 1] - i - 1) * sizeof(MiniKV));
    g_storage_n[s & 1]--;
}
static void kv_clear(int s)
{
    for (int i = 0; i < g_storage_n[s & 1]; i++)
    {
        free(g_storage[s & 1][i].key);
        free(g_storage[s & 1][i].val);
    }
    g_storage_n[s & 1] = 0;
}

/* Public C accessors for the CDP DOMStorage/Storage domain. */
const char *mini_bridge_storage_get(MiniBridge *b, int s, const char *k)
{
    (void)b;
    return kv_get(s, k);
}
int mini_bridge_storage_count(MiniBridge *b, int s)
{
    (void)b;
    return g_storage_n[s & 1];
}
const char *mini_bridge_storage_key(MiniBridge *b, int s, int i)
{
    (void)b;
    s &= 1;
    if (i < 0 || i >= g_storage_n[s])
        return NULL;
    return g_storage[s][i].key;
}
const char *mini_bridge_storage_val(MiniBridge *b, int s, int i)
{
    (void)b;
    s &= 1;
    if (i < 0 || i >= g_storage_n[s])
        return NULL;
    return g_storage[s][i].val;
}
void mini_bridge_storage_set(MiniBridge *b, int s, const char *k, const char *v)
{
    (void)b;
    kv_set(s, k, v);
}
void mini_bridge_storage_remove(MiniBridge *b, int s, const char *k)
{
    (void)b;
    kv_del(s, k);
}
void mini_bridge_storage_clear(MiniBridge *b, int s)
{
    (void)b;
    kv_clear(s);
}

static JSValue js_storage_getItem(JSContext *ctx, JSValueConst tv, int argc,
                                  JSValueConst *argv, int magic)
{
    (void)tv;
    const char *k = (argc >= 1) ? JS_ToCString(ctx, argv[0]) : NULL;
    const char *v = k ? kv_get(magic, k) : NULL;
    JSValue r = v ? JS_NewString(ctx, v) : JS_NULL;
    if (k)
        JS_FreeCString(ctx, k);
    return r;
}
static JSValue js_storage_setItem(JSContext *ctx, JSValueConst tv, int argc,
                                  JSValueConst *argv, int magic)
{
    (void)tv;
    const char *k = (argc >= 1) ? JS_ToCString(ctx, argv[0]) : NULL;
    const char *v = (argc >= 2) ? JS_ToCString(ctx, argv[1]) : NULL;
    if (k && v)
        kv_set(magic, k, v);
    if (k)
        JS_FreeCString(ctx, k);
    if (v)
        JS_FreeCString(ctx, v);
    return JS_UNDEFINED;
}
static JSValue js_storage_removeItem(JSContext *ctx, JSValueConst tv, int argc,
                                     JSValueConst *argv, int magic)
{
    (void)tv;
    const char *k = (argc >= 1) ? JS_ToCString(ctx, argv[0]) : NULL;
    if (k)
        kv_del(magic, k);
    if (k)
        JS_FreeCString(ctx, k);
    return JS_UNDEFINED;
}
static JSValue js_storage_clear(JSContext *ctx, JSValueConst tv, int argc,
                                JSValueConst *argv, int magic)
{
    (void)tv;
    (void)argc;
    (void)argv;
    kv_clear(magic);
    return JS_UNDEFINED;
}
static JSValue js_storage_key(JSContext *ctx, JSValueConst tv, int argc,
                              JSValueConst *argv, int magic)
{
    (void)tv;
    int idx = 0;
    if (argc >= 1)
        JS_ToInt32(ctx, &idx, argv[0]);
    if (idx < 0 || idx >= g_storage_n[magic & 1])
        return JS_NULL;
    return JS_NewString(ctx, g_storage[magic & 1][idx].key);
}
static JSValue js_storage_length(JSContext *ctx, JSValueConst tv, int magic)
{
    (void)tv;
    return JS_NewInt32(ctx, g_storage_n[magic & 1]);
}

/* Response.text()/json()/arrayBuffer()/blob() */
static JSValue js_resp_text(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    JSValue b = JS_GetPropertyStr(ctx, tv, "_body");
    if (JS_IsUndefined(b) || JS_IsNull(b))
    {
        JS_FreeValue(ctx, b);
        JSValue ab = JS_GetPropertyStr(ctx, tv, "_ab");
        size_t sz = 0;
        uint8_t *ptr = JS_GetArrayBuffer(ctx, &sz, ab);
        if (ptr && sz > 0)
            b = JS_NewStringLen(ctx, (const char *)ptr, sz);
        else
            b = JS_NewString(ctx, "");
        JS_FreeValue(ctx, ab);
    }
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, "__thenable");
    JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, &b);
    JS_FreeValue(ctx, b);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, global);
    return ret;
}

static JSValue js_resp_json(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    JSValue b = JS_GetPropertyStr(ctx, tv, "_body");
    const char *s = JS_ToCString(ctx, b);
    JSValue parsed = s ? JS_ParseJSON(ctx, s, strlen(s), "<response>") : JS_NULL;
    if (s)
        JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, b);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, "__thenable");
    JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, &parsed);
    JS_FreeValue(ctx, parsed);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, global);
    return ret;
}

static JSValue js_resp_arrayBuffer(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    JSValue ab = JS_GetPropertyStr(ctx, tv, "_ab");
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, "__thenable");
    JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, &ab);
    JS_FreeValue(ctx, ab);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, global);
    return ret;
}

static JSValue js_resp_blob(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    JSValue ab = JS_GetPropertyStr(ctx, tv, "_ab");
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue blob_ctor = JS_GetPropertyStr(ctx, global, "Blob");
    JSValue parts = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, parts, 0, ab);
    JSValue blob_obj = JS_CallConstructor(ctx, blob_ctor, 1, &parts);
    JS_FreeValue(ctx, parts);
    JS_FreeValue(ctx, blob_ctor);

    JSValue fn = JS_GetPropertyStr(ctx, global, "__thenable");
    JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, &blob_obj);
    JS_FreeValue(ctx, blob_obj);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, global);
    return ret;
}

static JSValue js_fetch(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    const char *url = NULL;
    if (argc >= 1)
    {
        if (JS_IsString(argv[0]))
            url = JS_ToCString(ctx, argv[0]);
        else if (JS_IsObject(argv[0]))
        {
            JSValue uval = JS_GetPropertyStr(ctx, argv[0], "url");
            if (JS_IsString(uval))
                url = JS_ToCString(ctx, uval);
            JS_FreeValue(ctx, uval);
        }
    }
    if (!url)
        return JS_ThrowTypeError(ctx, "fetch: url required");

    uint8_t *raw_body = NULL;
    size_t raw_len = 0;
    int status = 200;
    const char *status_text = "OK";
    int free_raw = 0;

    /* 1. Handle blob: URL */
    if (!strncmp(url, "blob:", 5))
    {
        const char *fpath = NULL;
        const uint8_t *bdata = NULL;
        size_t bsize = 0;
        const char *btype = NULL;
        if (mini_blob_lookup(url, &fpath, &bdata, &bsize, &btype))
        {
            if (fpath)
            {
                FILE *fp = fopen(fpath, "rb");
                if (fp)
                {
                    fseek(fp, 0, SEEK_END);
                    long sz = ftell(fp);
                    fseek(fp, 0, SEEK_SET);
                    if (sz >= 0)
                    {
                        raw_body = (uint8_t *)malloc((size_t)sz);
                        if (raw_body)
                        {
                            raw_len = fread(raw_body, 1, (size_t)sz, fp);
                            free_raw = 1;
                        }
                    }
                    fclose(fp);
                }
            }
            else if (bdata && bsize > 0)
            {
                raw_body = (uint8_t *)bdata;
                raw_len = bsize;
            }
        }
        else
        {
            status = 404;
            status_text = "Not Found";
        }
    }
    /* 2. Handle file:// URL */
    else if (!strncmp(url, "file://", 7))
    {
        const char *fpath = url + 7;
        if (!strncmp(fpath, "/", 1) && fpath[2] == ':')
            fpath++;
        FILE *fp = fopen(fpath, "rb");
        if (fp)
        {
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (sz >= 0)
            {
                raw_body = (uint8_t *)malloc((size_t)sz);
                if (raw_body)
                {
                    raw_len = fread(raw_body, 1, (size_t)sz, fp);
                    free_raw = 1;
                }
            }
            fclose(fp);
        }
        else
        {
            status = 404;
            status_text = "Not Found";
        }
    }
    /* 3. Handle data: URL */
    else if (!strncmp(url, "data:", 5))
    {
        const char *comma = strchr(url, ',');
        if (comma)
        {
            size_t out_len = 0;
            raw_body = js_bridge_base64_decode(comma + 1, strlen(comma + 1), &out_len);
            raw_len = out_len;
            free_raw = 1;
        }
    }
    /* 4. Handle HTTP/HTTPS via mini_net_fetch */
    else
    {
        char method[8] = "GET";
        const char *body = NULL;
        size_t body_len = 0;
        char *hdrs = NULL;
        size_t hlen = 0, hcap = 0;

        if (argc >= 2 && JS_IsObject(argv[1]))
        {
            JSValue m = JS_GetPropertyStr(ctx, argv[1], "method");
            if (!JS_IsUndefined(m))
            {
                const char *ms = JS_ToCString(ctx, m);
                if (ms)
                {
                    snprintf(method, sizeof method, "%s", ms);
                    JS_FreeCString(ctx, ms);
                }
            }
            JS_FreeValue(ctx, m);
            JSValue b = JS_GetPropertyStr(ctx, argv[1], "body");
            if (JS_IsString(b) || (JS_IsObject(b) && !JS_IsNull(b)))
            {
                const char *bs = JS_ToCString(ctx, b);
                if (bs)
                {
                    body = bs;
                    body_len = strlen(bs);
                }
            }
            JS_FreeValue(ctx, b);
            JSValue h = JS_GetPropertyStr(ctx, argv[1], "headers");
            if (JS_IsObject(h) && !JS_IsNull(h))
            {
                JSPropertyEnum *ptab = NULL;
                uint32_t plen = 0;
                if (JS_GetOwnPropertyNames(ctx, &ptab, &plen, h, JS_GPN_ENUM_ONLY) == 0)
                {
                    for (uint32_t i = 0; i < plen; i++)
                    {
                        const char *kn = JS_AtomToCString(ctx, ptab[i].atom);
                        JSValue hv = JS_GetProperty(ctx, h, ptab[i].atom);
                        const char *vs = JS_ToCString(ctx, hv);
                        if (kn && vs)
                        {
                            size_t need = strlen(kn) + 2 + strlen(vs) + 3;
                            if (hlen + need + 1 > hcap)
                            {
                                hcap = hcap ? hcap : 256;
                                while (hcap < hlen + need + 1)
                                    hcap *= 2;
                                hdrs = (char *)realloc(hdrs, hcap);
                            }
                            if (hdrs)
                                hlen += (size_t)snprintf(hdrs + hlen, hcap - hlen, "%s: %s\r\n", kn, vs);
                        }
                        if (kn) JS_FreeCString(ctx, kn);
                        if (vs) JS_FreeCString(ctx, vs);
                        JS_FreeValue(ctx, hv);
                    }
                    if (ptab) JS_FreePropertyEnum(ctx, ptab, plen);
                }
            }
            JS_FreeValue(ctx, h);
        }

        MiniNetRecord rec;
        memset(&rec, 0, sizeof rec);
        mini_net_fetch(method, url, hdrs, body, body_len, NULL, &rec);
        status = rec.status;
        status_text = (rec.status >= 200 && rec.status < 300) ? "OK" : "Error";
        raw_body = (uint8_t *)rec.resp_body;
        raw_len = rec.resp_body_len;
        mini_net_record_add(&rec);
        if (body) JS_FreeCString(ctx, body);
        free(hdrs);
    }

    JSValue resp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, resp, "url", JS_NewString(ctx, url));
    JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, status));
    JS_SetPropertyStr(ctx, resp, "ok", JS_NewBool(ctx, status >= 200 && status < 300));
    JS_SetPropertyStr(ctx, resp, "statusText", JS_NewString(ctx, status_text));

    /* Attach _ab ArrayBuffer */
    JSValue ab = JS_NewArrayBufferCopy(ctx, raw_body ? raw_body : (const uint8_t *)"", raw_len);
    JS_SetPropertyStr(ctx, resp, "_ab", JS_DupValue(ctx, ab));
    if (raw_len < 1024 * 1024)
    {
        JSValue body_str = JS_NewStringLen(ctx, (const char *)(raw_body ? (const char *)raw_body : ""), raw_len);
        JS_SetPropertyStr(ctx, resp, "_body", body_str);
    }

    /* Functions */
    JS_SetPropertyStr(ctx, resp, "text", JS_NewCFunction(ctx, (JSCFunction *)js_resp_text, "text", 0));
    JS_SetPropertyStr(ctx, resp, "json", JS_NewCFunction(ctx, (JSCFunction *)js_resp_json, "json", 0));
    JS_SetPropertyStr(ctx, resp, "arrayBuffer", JS_NewCFunction(ctx, (JSCFunction *)js_resp_arrayBuffer, "arrayBuffer", 0));
    JS_SetPropertyStr(ctx, resp, "blob", JS_NewCFunction(ctx, (JSCFunction *)js_resp_blob, "blob", 0));

    /* Attach headers object */
    JSValue global_hdrs = JS_GetGlobalObject(ctx);
    JSValue headers_ctor = JS_GetPropertyStr(ctx, global_hdrs, "Headers");
    JSValue hobj = JS_CallConstructor(ctx, headers_ctor, 0, NULL);
    JS_FreeValue(ctx, headers_ctor);
    JS_FreeValue(ctx, global_hdrs);
    char len_buf[32];
    snprintf(len_buf, sizeof len_buf, "%zu", raw_len);
    JSValue len_val = JS_NewString(ctx, len_buf);
    JSValue key_val = JS_NewString(ctx, "content-length");
    JSValue set_fn = JS_GetPropertyStr(ctx, hobj, "set");
    if (JS_IsFunction(ctx, set_fn))
    {
        JSValue args[2] = { key_val, len_val };
        JSValue sret = JS_Call(ctx, set_fn, hobj, 2, args);
        JS_FreeValue(ctx, sret);
    }
    JS_FreeValue(ctx, set_fn);
    JS_FreeValue(ctx, key_val);
    JS_FreeValue(ctx, len_val);
    JS_SetPropertyStr(ctx, resp, "headers", hobj);

    if (free_raw && raw_body)
        free(raw_body);
    JS_FreeCString(ctx, url);
    JS_FreeValue(ctx, ab);

    /* wrap in a thenable so `await fetch()` and `fetch().then(...)` work */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, "__thenable");
    JSValue arg = JS_DupValue(ctx, resp);
    JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, &arg);
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, resp);
    if (JS_IsException(ret))
    {
        JS_FreeValue(ctx, ret);
        return JS_NULL;
    }
    return ret;
}

/* Return the current page-text selection as a string (for window.getSelection
   and CDP tests). Reads from the live event state. */
static JSValue js_mini_selection_text(JSContext *ctx, JSValueConst tv,
                                      int argc, JSValueConst *argv)
{
    (void)tv;
    (void)argc;
    (void)argv;
    MiniBridge *b = bridge_of(ctx);
    char buf[8192];
    int n = (b && b->ev) ? mini_events_selection_text(b->ev, buf, sizeof buf) : 0;
    return JS_NewStringLen(ctx, n > 0 ? buf : "", n > 0 ? (size_t)n : 0);
}

/* DataTransfer payload setters/getters for drag-and-drop. */
static JSValue js_mini_dnd_set(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    MiniBridge *b = bridge_of(ctx);
    if (b && b->ev && argc >= 1)
    {
        const char *s = JS_ToCString(ctx, argv[0]);
        if (s)
        {
            mini_events_dnd_set_data(b->ev, s);
            JS_FreeCString(ctx, s);
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_mini_dnd_get(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    (void)argc;
    (void)argv;
    MiniBridge *b = bridge_of(ctx);
    const char *s = (b && b->ev) ? mini_events_dnd_get_data(b->ev) : NULL;
    return JS_NewString(ctx, s ? s : "");
}

static JSValue js_mini_create_object_url(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "createObjectURL: Blob/File expected");
    JSValueConst obj = argv[0];
    JSValue fp = JS_GetPropertyStr(ctx, obj, "__filePath");
    JSValue type_val = JS_GetPropertyStr(ctx, obj, "type");
    const char *type_str = JS_IsString(type_val) ? JS_ToCString(ctx, type_val) : NULL;
    char *url = NULL;

    if (JS_IsString(fp))
    {
        const char *path = JS_ToCString(ctx, fp);
        if (path)
        {
            url = mini_blob_register_file(path, type_str);
            JS_FreeCString(ctx, path);
        }
    }
    else
    {
        JSValue buf_val = JS_GetPropertyStr(ctx, obj, "__buffer");
        size_t sz = 0;
        uint8_t *ptr = JS_GetArrayBuffer(ctx, &sz, buf_val);
        url = mini_blob_register_mem(ptr, sz, type_str);
        JS_FreeValue(ctx, buf_val);
    }
    if (type_str)
        JS_FreeCString(ctx, type_str);
    JS_FreeValue(ctx, fp);
    JS_FreeValue(ctx, type_val);

    if (!url)
        return JS_NULL;
    JSValue res = JS_NewString(ctx, url);
    free(url);
    return res;
}

static JSValue js_mini_revoke_object_url(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc >= 1 && JS_IsString(argv[0]))
    {
        const char *u = JS_ToCString(ctx, argv[0]);
        if (u)
        {
            mini_blob_revoke(u);
            JS_FreeCString(ctx, u);
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_mini_read_file_binary(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_NewArrayBufferCopy(ctx, NULL, 0);
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_NewArrayBufferCopy(ctx, NULL, 0);

    FILE *fp = fopen(path, "rb");
    if (!fp)
    {
        JS_FreeCString(ctx, path);
        return JS_NewArrayBufferCopy(ctx, NULL, 0);
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (buf)
    {
        size_t rd = fread(buf, 1, (size_t)sz, fp);
        fclose(fp);
        JS_FreeCString(ctx, path);
        JSValue res = JS_NewArrayBufferCopy(ctx, buf, rd);
        free(buf);
        return res;
    }
    fclose(fp);
    JS_FreeCString(ctx, path);
    return JS_NewArrayBufferCopy(ctx, NULL, 0);
}

static JSValue js_mini_read_file_text(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_NewString(ctx, "");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_NewString(ctx, "");

    FILE *fp = fopen(path, "rb");
    if (!fp)
    {
        JS_FreeCString(ctx, path);
        return JS_NewString(ctx, "");
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    char *buf = (char *)malloc((size_t)sz + 1);
    if (buf)
    {
        size_t rd = fread(buf, 1, (size_t)sz, fp);
        buf[rd] = '\0';
        fclose(fp);
        JS_FreeCString(ctx, path);
        JSValue res = JS_NewStringLen(ctx, buf, rd);
        free(buf);
        return res;
    }
    fclose(fp);
    JS_FreeCString(ctx, path);
    return JS_NewString(ctx, "");
}

static JSValue js_mini_get_image_info(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_NULL;
    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url)
        return JS_NULL;
    int w = 0, h = 0, comp = 0;
    int ok = 0;

    if (!strncmp(url, "blob:", 5))
    {
        const char *fpath = NULL;
        const uint8_t *bdata = NULL;
        size_t bsize = 0;
        if (mini_blob_lookup(url, &fpath, &bdata, &bsize, NULL))
        {
            if (fpath)
                ok = stbi_info(fpath, &w, &h, &comp);
            else if (bdata && bsize > 0)
                ok = stbi_info_from_memory(bdata, (int)bsize, &w, &h, &comp);
        }
    }
    else if (!strncmp(url, "data:image/", 11))
    {
        const char *comma = strchr(url, ',');
        if (comma)
        {
            size_t raw_len = strlen(comma + 1);
            size_t out_len = 0;
            uint8_t *bin = js_bridge_base64_decode(comma + 1, raw_len, &out_len);
            if (bin && out_len > 0)
            {
                ok = stbi_info_from_memory(bin, (int)out_len, &w, &h, &comp);
                free(bin);
            }
        }
    }
    else if (!strncmp(url, "file://", 7))
    {
        const char *fpath = url + 7;
        if (!strncmp(fpath, "/", 1) && fpath[2] == ':')
            fpath++;
        ok = stbi_info(fpath, &w, &h, &comp);
    }
    else
    {
        ok = stbi_info(url, &w, &h, &comp);
    }

    JS_FreeCString(ctx, url);
    if (!ok)
        return JS_NULL;

    JSValue res = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, res, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, res, "height", JS_NewInt32(ctx, h));
    return res;
}

static JSValue js_mini_create_image_bitmap(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "createImageBitmap: image source expected");

    JSValueConst src = argv[0];
    int iw = 0, ih = 0, ic = 0;
    unsigned char *dec_px = NULL;

    JSValue fp = JS_GetPropertyStr(ctx, src, "__filePath");
    if (JS_IsString(fp))
    {
        const char *path = JS_ToCString(ctx, fp);
        if (path)
        {
            dec_px = stbi_load(path, &iw, &ih, &ic, 4);
            JS_FreeCString(ctx, path);
        }
    }
    JS_FreeValue(ctx, fp);

    if (!dec_px)
    {
        JSValue buf_val = JS_GetPropertyStr(ctx, src, "__buffer");
        if (!JS_IsUndefined(buf_val) && !JS_IsNull(buf_val))
        {
            size_t sz = 0;
            uint8_t *ptr = JS_GetArrayBuffer(ctx, &sz, buf_val);
            if (ptr && sz > 0)
                dec_px = stbi_load_from_memory(ptr, (int)sz, &iw, &ih, &ic, 4);
        }
        JS_FreeValue(ctx, buf_val);
    }

    if (!dec_px)
    {
        JSValue src_prop = JS_GetPropertyStr(ctx, src, "src");
        if (JS_IsString(src_prop))
        {
            const char *src_str = JS_ToCString(ctx, src_prop);
            if (src_str)
            {
                if (!strncmp(src_str, "data:image/", 11))
                {
                    const char *comma = strchr(src_str, ',');
                    if (comma)
                    {
                        size_t out_len = 0;
                        uint8_t *bin = js_bridge_base64_decode(comma + 1, strlen(comma + 1), &out_len);
                        if (bin && out_len > 0)
                        {
                            dec_px = stbi_load_from_memory(bin, (int)out_len, &iw, &ih, &ic, 4);
                            free(bin);
                        }
                    }
                }
                else if (!strncmp(src_str, "blob:", 5))
                {
                    const char *fpath = NULL;
                    const uint8_t *bdata = NULL;
                    size_t bsize = 0;
                    if (mini_blob_lookup(src_str, &fpath, &bdata, &bsize, NULL))
                    {
                        if (fpath)
                            dec_px = stbi_load(fpath, &iw, &ih, &ic, 4);
                        else if (bdata && bsize > 0)
                            dec_px = stbi_load_from_memory(bdata, (int)bsize, &iw, &ih, &ic, 4);
                    }
                }
                else if (!strncmp(src_str, "file://", 7))
                {
                    const char *fpath = src_str + 7;
                    if (!strncmp(fpath, "/", 1) && fpath[2] == ':')
                        fpath++;
                    dec_px = stbi_load(fpath, &iw, &ih, &ic, 4);
                }
                else
                {
                    dec_px = stbi_load(src_str, &iw, &ih, &ic, 4);
                }
                JS_FreeCString(ctx, src_str);
            }
        }
        JS_FreeValue(ctx, src_prop);
    }

    JSValue ib = JS_NewObject(ctx);
    if (dec_px && iw > 0 && ih > 0)
    {
        JS_SetPropertyStr(ctx, ib, "width", JS_NewInt32(ctx, iw));
        JS_SetPropertyStr(ctx, ib, "height", JS_NewInt32(ctx, ih));
        JSValue ab = JS_NewArrayBufferCopy(ctx, dec_px, (size_t)(iw * ih * 4));
        JS_SetPropertyStr(ctx, ib, "_data", ab);
        stbi_image_free(dec_px);
    }
    else
    {
        JS_SetPropertyStr(ctx, ib, "width", JS_NewInt32(ctx, 1));
        JS_SetPropertyStr(ctx, ib, "height", JS_NewInt32(ctx, 1));
        uint8_t white[4] = { 255, 255, 255, 255 };
        JSValue ab = JS_NewArrayBufferCopy(ctx, white, 4);
        JS_SetPropertyStr(ctx, ib, "_data", ab);
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, "__thenable");
    JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, &ib);
    JS_FreeValue(ctx, ib);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, global);
    return ret;
}

static JSValue js_mini_set_title(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    MiniBridge *b = bridge_of(ctx);
    if (b && b->r && b->r->gpu.window_handle)
    {
        const char *t = JS_ToCString(ctx, argv[0]);
        if (t)
        {
            glfwSetWindowTitle((GLFWwindow *)b->r->gpu.window_handle, t);
            JS_FreeCString(ctx, t);
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_mini_audio_init(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    int32_t sample_rate = 44100;
    int32_t channels = 1;
    if (argc >= 1) JS_ToInt32(ctx, &sample_rate, argv[0]);
    if (argc >= 2) JS_ToInt32(ctx, &channels, argv[1]);
    int ret = mini_audio_init(sample_rate, channels);
    return JS_NewInt32(ctx, ret);
}

static JSValue js_mini_audio_queue_pcm(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    size_t len = 0;
    const void *p = js_float_data(ctx, argv[0], &len);
    if (p && len > 0)
    {
        size_t count = len / sizeof(float);
        mini_audio_queue_pcm((const float *)p, count);
    }
    return JS_UNDEFINED;
}

static void install_net_storage(JSContext *ctx, JSValue global)
{
    JS_SetPropertyStr(ctx, global, "__mini_set_title",
                      JS_NewCFunction(ctx, (JSCFunction *)js_mini_set_title, "__mini_set_title", 1));
    JS_SetPropertyStr(ctx, global, "__mini_audio_init",
                      JS_NewCFunction(ctx, (JSCFunction *)js_mini_audio_init, "__mini_audio_init", 2));
    JS_SetPropertyStr(ctx, global, "__mini_audio_queue_pcm",
                      JS_NewCFunction(ctx, (JSCFunction *)js_mini_audio_queue_pcm, "__mini_audio_queue_pcm", 1));

    JS_SetPropertyStr(ctx, global, "fetch",
                      JS_NewCFunction(ctx, (JSCFunction *)js_fetch, "fetch", 2));
    JS_SetPropertyStr(ctx, global, "createImageBitmap",
                      JS_NewCFunction(ctx, (JSCFunction *)js_mini_create_image_bitmap, "createImageBitmap", 1));
    JS_SetPropertyStr(ctx, global, "__miniCreateImageBitmap",
                      JS_NewCFunction(ctx, (JSCFunction *)js_mini_create_image_bitmap, "__miniCreateImageBitmap", 1));

    JS_SetPropertyStr(ctx, global, "__miniCreateObjectURL",
                      JS_NewCFunction(ctx, (JSCFunction *)js_mini_create_object_url, "__miniCreateObjectURL", 1));
    JS_SetPropertyStr(ctx, global, "__miniRevokeObjectURL",
                      JS_NewCFunction(ctx, (JSCFunction *)js_mini_revoke_object_url, "__miniRevokeObjectURL", 1));
    JS_SetPropertyStr(ctx, global, "__miniReadFileBinary",
                      JS_NewCFunction(ctx, (JSCFunction *)js_mini_read_file_binary, "__miniReadFileBinary", 1));
    JS_SetPropertyStr(ctx, global, "__miniReadFileText",
                      JS_NewCFunction(ctx, (JSCFunction *)js_mini_read_file_text, "__miniReadFileText", 1));
    JS_SetPropertyStr(ctx, global, "__miniGetImageInfo",
                      JS_NewCFunction(ctx, (JSCFunction *)js_mini_get_image_info, "__miniGetImageInfo", 1));

    /* debugging + getSelection(): expose the live page-text selection so the
       shim can build window.getSelection().toString() and tests can assert it. */
    JS_SetPropertyStr(ctx, global, "__miniSelectionText",
                      JS_NewCFunction(ctx, (JSCFunction *)js_mini_selection_text,
                                      "__miniSelectionText", 0));
    /* DataTransfer "text/plain" slot for drag-and-drop (set during dragstart,
       read during drop). */
    JS_SetPropertyStr(ctx, global, "__miniDndSet",
                      JS_NewCFunction(ctx, (JSCFunction *)js_mini_dnd_set,
                                      "__miniDndSet", 1));
    JS_SetPropertyStr(ctx, global, "__miniDndGet",
                      JS_NewCFunction(ctx, (JSCFunction *)js_mini_dnd_get,
                                      "__miniDndGet", 0));

    /* localStorage (magic 0) + sessionStorage (magic 1) */
    for (int s = 0; s < 2; s++)
    {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "getItem",
                          JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_storage_getItem, "getItem", 1, JS_CFUNC_generic_magic, s));
        JS_SetPropertyStr(ctx, obj, "setItem",
                          JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_storage_setItem, "setItem", 2, JS_CFUNC_generic_magic, s));
        JS_SetPropertyStr(ctx, obj, "removeItem",
                          JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_storage_removeItem, "removeItem", 1, JS_CFUNC_generic_magic, s));
        JS_SetPropertyStr(ctx, obj, "clear",
                          JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_storage_clear, "clear", 0, JS_CFUNC_generic_magic, s));
        JS_SetPropertyStr(ctx, obj, "key",
                          JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_storage_key, "key", 1, JS_CFUNC_generic_magic, s));
        /* `length` as a getter so `localStorage.length` reads the count */
        JSAtom latom = JS_NewAtom(ctx, "length");
        JS_DefinePropertyGetSet(ctx, obj, latom,
                                JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_storage_length, "get length", 0, JS_CFUNC_getter_magic, s),
                                JS_UNDEFINED, 0);
        JS_FreeAtom(ctx, latom);
        JS_SetPropertyStr(ctx, global, s == 0 ? "localStorage" : "sessionStorage", obj);
    }
}

/* ----------------------------------------------------------------- */
/* Install everything into the global object                          */
/* ----------------------------------------------------------------- */
/* The event methods (js_addEventListener / -remove / -dispatchEvent),
    focus/blur, innerHTML, insertBefore, cloneNode, removeAttribute are
    defined above (Stage 3 event bridge) — no forward decls needed.   */

#define SET(g, name, fn, n) JS_SetPropertyStr(ctx, g, name, JS_NewCFunction(ctx, (JSCFunction *)fn, name, n))

static JSValue js_setPointerCapture(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)ctx; (void)tv; (void)argc; (void)argv;
    return JS_UNDEFINED;
}
static JSValue js_releasePointerCapture(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)ctx; (void)tv; (void)argc; (void)argv;
    return JS_UNDEFINED;
}
static JSValue js_hasPointerCapture(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)ctx; (void)tv; (void)argc; (void)argv;
    return JS_FALSE;
}

static void install_element_proto(JSContext *ctx, JSClassID cid)
{
    /* Create the prototype, install methods on it, then JS_SetClassProto
       (which STEALS the value — QuickJS stores it as the class proto, so
       JS_NewObjectClass instances inherit it and JS_GetClassProto returns it).
       Previously we used JS_GetClassProto before installing anything, which
       returns JS_NULL -> MiniElement.prototype was null -> the shim threw
       "not an object". */
    JSValue proto = JS_NewObject(ctx);
    SET(proto, "appendChild", js_appendChild, 1);
    SET(proto, "removeChild", js_removeChild, 1);
    SET(proto, "setAttribute", js_setAttribute, 2);
    SET(proto, "getAttribute", js_getAttribute, 1);
    SET(proto, "_setStyle", js_setStyle, 2);
    SET(proto, "_getText", js_getText, 0);
    SET(proto, "_setText", js_setText, 1);
    SET(proto, "getContext", js_getContext, 1);
    SET(proto, "addEventListener", js_addEventListener, 3);
    SET(proto, "removeEventListener", js_removeEventListener, 2);
    SET(proto, "dispatchEvent", js_dispatchEvent, 1);
    SET(proto, "setPointerCapture", js_setPointerCapture, 1);
    SET(proto, "releasePointerCapture", js_releasePointerCapture, 1);
    SET(proto, "hasPointerCapture", js_hasPointerCapture, 1);
    SET(proto, "focus", js_focus, 0);
    SET(proto, "blur", js_blur, 0);
    SET(proto, "removeAttribute", js_removeAttribute, 1);
    SET(proto, "insertBefore", js_insertBefore, 2);
    SET(proto, "cloneNode", js_cloneNode, 1);
    SET(proto, "_tag", js_getTag, 0);
    SET(proto, "_getInnerHTML", js_getInnerHTML, 0);
    SET(proto, "_setInnerHTML", js_setInnerHTML, 1);
    SET(proto, "getBoundingClientRect", js_getBoundingClientRect, 0);
    SET(proto, "getClientRects", js_getClientRects, 0);
    SET(proto, "_cq", js_doc_query, 1);
    SET(proto, "_cqa", js_doc_queryAll, 1);
    /* Stage 4+: traversal / properties / synthetic click.
       DOM *properties* are registered as _-prefixed methods here and exposed
       as real getters in the JS shim (so el.parentNode / el.nodeType read as
       values, not function objects). Real DOM methods keep their plain names. */
    SET(proto, "_firstChild", js_firstChild, 0);
    SET(proto, "_lastChild", js_lastChild, 0);
    SET(proto, "_nextSibling", js_nextSibling, 0);
    SET(proto, "_previousSibling", js_prevSibling, 0);
    SET(proto, "_parentNode", js_parentNode, 0);
    SET(proto, "_parentElement", js_parentElement, 0);
    SET(proto, "_firstElementChild", js_firstElementChild, 0);
    SET(proto, "_lastElementChild", js_lastElementChild, 0);
    SET(proto, "_nextElementSibling", js_nextElementSibling, 0);
    SET(proto, "_previousElementSibling", js_prevElementSibling, 0);
    SET(proto, "_children", js_children, 0);
    SET(proto, "_childNodes", js_childNodes, 0);
    SET(proto, "_childElementCount", js_childElementCount, 0);
    SET(proto, "_nodeType", js_nodeType, 0);
    SET(proto, "_nodeName", js_nodeName, 0);
    SET(proto, "_getNodeValue", js_getNodeValue, 0);
    SET(proto, "_setNodeValue", js_setNodeValue, 1);
    SET(proto, "hasChildNodes", js_hasChildNodes, 0);
    SET(proto, "contains", js_contains, 1);
    SET(proto, "matches", js_matches, 1);
    SET(proto, "matchesSelector", js_matches, 1);
    SET(proto, "closest", js_closest, 1);
    SET(proto, "click", js_click, 0);
    SET(proto, "scrollIntoView", js_scrollIntoView, 0);
    SET(proto, "_computedStyleJSON", js_computedStyleJSON, 0);
    SET(proto, "_getOuterHTML", js_getOuterHTML, 0);
    SET(proto, "_setOuterHTML", js_setOuterHTML, 1);
    SET(proto, "_requestPointerLock", js_requestPointerLock, 0);
    JS_SetClassProto(ctx, cid, proto); /* steals proto */
}

static JSValue js_illegal_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* ---- event listener table was here in Phase 1; Stage 3 replaced the ----
   flat g_ls + dead mini_bridge_dispatch_mouse with the capture/target/
   bubble bridge above (js_addEventListener etc.). The host input path
   now drives mini_events_handle_* in main.c directly.                 */

static void install_webgl(JSContext *ctx, JSValue global)
{
    /* WebGLRenderingContext as a constructor whose instances share GL state */
    JSValue ctor = JS_NewCFunction2(ctx, (JSCFunction *)js_illegal_ctor, "WebGLRenderingContext", 0, JS_CFUNC_constructor, 0);
    JSValue proto = JS_NewObject(ctx);
    SET(proto, "createShader", js_gl_createShader, 1);
    SET(proto, "shaderSource", js_gl_shaderSource, 2);
    SET(proto, "compileShader", js_gl_compileShader, 1);
    SET(proto, "createProgram", js_gl_createProgram, 0);
    SET(proto, "attachShader", js_gl_attachShader, 2);
    SET(proto, "linkProgram", js_gl_linkProgram, 1);
    SET(proto, "useProgram", js_gl_useProgram, 1);
    SET(proto, "createBuffer", js_gl_createBuffer, 0);
    SET(proto, "bindBuffer", js_gl_bindBuffer, 2);
    SET(proto, "bufferData", js_gl_bufferData, 3);
    SET(proto, "enableVertexAttribArray", js_gl_enableVA, 1);
    SET(proto, "vertexAttribPointer", js_gl_vertexAttribPointer, 6);
    SET(proto, "drawArrays", js_gl_drawArrays, 3);
    SET(proto, "viewport", js_gl_viewport, 4);
    SET(proto, "scissor", js_gl_scissor, 4);
    SET(proto, "clearColor", js_gl_clearColor, 4);
    SET(proto, "clear", js_gl_clear, 1);
    SET(proto, "getUniformLocation", js_gl_getUniformLocation, 2);
    SET(proto, "getActiveUniform", js_gl_getActiveUniform, 2);
    SET(proto, "uniformMatrix4fv", js_gl_uniformMatrix4fv, 3);
    SET(proto, "getShaderParameter", js_gl_getShaderParameter, 2);
    SET(proto, "getProgramParameter", js_gl_getProgramParameter, 2);
    SET(proto, "getShaderInfoLog", js_gl_getShaderInfoLog, 1);
    SET(proto, "getProgramInfoLog", js_gl_getProgramInfoLog, 1);
    SET(proto, "createTexture", js_gl_createTexture, 0);
    SET(proto, "bindTexture", js_gl_bindTexture, 2);
    SET(proto, "getAttribLocation", js_gl_getAttribLocation, 2);
    SET(proto, "getActiveAttrib", js_gl_getActiveAttrib, 2);
    SET(proto, "enable", js_gl_enable, 1);
    SET(proto, "disable", js_gl_disable, 1);
    SET(proto, "pixelStorei", js_gl_pixelStorei, 2);
    SET(proto, "activeTexture", js_gl_activeTexture, 1);
    SET(proto, "texParameteri", js_gl_texParameteri, 3);
    SET(proto, "texImage2D", js_gl_texImage2D, 9);
    SET(proto, "drawElements", js_gl_drawElements, 4);
    SET(proto, "createVertexArray", js_gl_createVertexArray, 0);
    SET(proto, "bindVertexArray", js_gl_bindVertexArray, 1);
    SET(proto, "deleteVertexArray", js_gl_deleteVertexArray, 1);
    SET(proto, "readPixels", js_gl_readPixels, 7);
    SET(proto, "getParameter", js_gl_getParameter, 1);
    SET(proto, "getExtension", js_gl_getExtension, 1);
    SET(proto, "getContextAttributes", js_gl_getContextAttributes, 0);
    SET(proto, "getError", js_gl_getError, 0);
    SET(proto, "getShaderPrecisionFormat", js_gl_getShaderPrecisionFormat, 2);

    /* Framebuffers / Renderbuffers */
    SET(proto, "createFramebuffer", js_gl_createFramebuffer, 0);
    SET(proto, "bindFramebuffer", js_gl_bindFramebuffer, 2);
    SET(proto, "framebufferTexture2D", js_gl_framebufferTexture2D, 5);
    SET(proto, "checkFramebufferStatus", js_gl_checkFramebufferStatus, 1);
    SET(proto, "deleteFramebuffer", js_gl_deleteFramebuffer, 1);
    SET(proto, "createRenderbuffer", js_gl_createRenderbuffer, 0);
    SET(proto, "bindRenderbuffer", js_gl_bindRenderbuffer, 2);
    SET(proto, "renderbufferStorage", js_gl_renderbufferStorage, 4);
    SET(proto, "framebufferRenderbuffer", js_gl_framebufferRenderbuffer, 4);
    SET(proto, "deleteRenderbuffer", js_gl_deleteRenderbuffer, 1);

    /* Deletions & Mipmap */
    SET(proto, "deleteTexture", js_gl_deleteTexture, 1);
    SET(proto, "deleteBuffer", js_gl_deleteBuffer, 1);
    SET(proto, "deleteProgram", js_gl_deleteProgram, 1);
    SET(proto, "deleteShader", js_gl_deleteShader, 1);
    SET(proto, "generateMipmap", js_gl_generateMipmap, 1);
    SET(proto, "texSubImage2D", js_gl_texSubImage2D, 9);

    /* State setters */
    SET(proto, "blendFunc", js_gl_blendFunc, 2);
    SET(proto, "blendEquation", js_gl_blendEquation, 1);
    SET(proto, "blendFuncSeparate", js_gl_blendFuncSeparate, 4);
    SET(proto, "blendEquationSeparate", js_gl_blendEquationSeparate, 2);
    SET(proto, "depthFunc", js_gl_depthFunc, 1);
    SET(proto, "depthMask", js_gl_depthMask, 1);
    SET(proto, "colorMask", js_gl_colorMask, 4);
    SET(proto, "cullFace", js_gl_cullFace, 1);
    SET(proto, "frontFace", js_gl_frontFace, 1);
    SET(proto, "polygonOffset", js_gl_polygonOffset, 2);
    SET(proto, "clearDepth", js_gl_clearDepth, 1);
    SET(proto, "clearStencil", js_gl_clearStencil, 1);
    SET(proto, "stencilMask", js_gl_stencilMask, 1);
    SET(proto, "stencilFunc", js_gl_stencilFunc, 3);
    SET(proto, "stencilOp", js_gl_stencilOp, 3);

    /* Uniforms */
    SET(proto, "uniform1f", js_gl_uniform1f, 2);
    SET(proto, "uniform2f", js_gl_uniform2f, 3);
    SET(proto, "uniform3f", js_gl_uniform3f, 4);
    SET(proto, "uniform4f", js_gl_uniform4f, 5);
    SET(proto, "uniform1i", js_gl_uniform1i, 2);
    SET(proto, "uniform2i", js_gl_uniform2i, 3);
    SET(proto, "uniform3i", js_gl_uniform3i, 4);
    SET(proto, "uniform4i", js_gl_uniform4i, 5);
    SET(proto, "uniform1fv", js_gl_uniform1fv, 2);
    SET(proto, "uniform2fv", js_gl_uniform2fv, 2);
    SET(proto, "uniform3fv", js_gl_uniform3fv, 2);
    SET(proto, "uniform4fv", js_gl_uniform4fv, 2);
    SET(proto, "uniform1iv", js_gl_uniform1iv, 2);
    SET(proto, "uniform2iv", js_gl_uniform2iv, 2);
    SET(proto, "uniform3iv", js_gl_uniform3iv, 2);
    SET(proto, "uniform4iv", js_gl_uniform4iv, 2);
    SET(proto, "uniformMatrix2fv", js_gl_uniformMatrix2fv, 3);
    SET(proto, "uniformMatrix3fv", js_gl_uniformMatrix3fv, 3);

    JS_SetPropertyStr(ctx, ctor, "prototype", proto);
    JS_SetPropertyStr(ctx, global, "WebGLRenderingContext", ctor);

    JSValue ctor_gl2 = JS_NewCFunction2(ctx, (JSCFunction *)js_illegal_ctor, "WebGL2RenderingContext", 0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, ctor_gl2, "prototype", JS_DupValue(ctx, proto));
    JS_SetPropertyStr(ctx, global, "WebGL2RenderingContext", ctor_gl2);

    /* 2D context as a lightweight constructor */
    JSValue ctor2d = JS_NewCFunction2(ctx, (JSCFunction *)js_illegal_ctor, "CanvasRenderingContext2D", 0, JS_CFUNC_constructor, 0);
    JSValue proto2d = JS_NewObject(ctx);
    SET(proto2d, "fillRect", js_ctx2d_fillRect, 4);
    SET(proto2d, "clearRect", js_ctx2d_clearRect, 4);
    SET(proto2d, "beginPath", js_ctx2d_beginPath, 0);
    SET(proto2d, "closePath", js_ctx2d_closePath, 0);
    SET(proto2d, "moveTo", js_ctx2d_moveTo, 2);
    SET(proto2d, "lineTo", js_ctx2d_lineTo, 2);
    SET(proto2d, "arc", js_ctx2d_arc, 5);
    SET(proto2d, "fill", js_ctx2d_fill, 0);
    SET(proto2d, "stroke", js_ctx2d_stroke, 0);
    SET(proto2d, "save", js_ctx2d_save, 0);
    SET(proto2d, "restore", js_ctx2d_restore, 0);
    SET(proto2d, "translate", js_ctx2d_translate, 2);
    SET(proto2d, "scale", js_ctx2d_scale, 2);
    SET(proto2d, "rotate", js_ctx2d_rotate, 1);
    /* Stage 3: text / curves / transforms / state / gradients / images */
    SET(proto2d, "strokeRect", js_ctx2d_strokeRect, 4);
    SET(proto2d, "fillText", js_ctx2d_fillText, 4);
    SET(proto2d, "strokeText", js_ctx2d_strokeText, 4);
    SET(proto2d, "measureText", js_ctx2d_measureText, 1);
    SET(proto2d, "quadraticCurveTo", js_ctx2d_quadraticCurveTo, 4);
    SET(proto2d, "bezierCurveTo", js_ctx2d_bezierCurveTo, 6);
    SET(proto2d, "arcTo", js_ctx2d_arcTo, 5);
    SET(proto2d, "ellipse", js_ctx2d_ellipse, 7);
    SET(proto2d, "rect", js_ctx2d_rect, 4);
    SET(proto2d, "roundRect", js_ctx2d_roundRect, 5);
    SET(proto2d, "transform", js_ctx2d_transform, 6);
    SET(proto2d, "setTransform", js_ctx2d_setTransform, 6);
    SET(proto2d, "resetTransform", js_ctx2d_resetTransform, 0);
    SET(proto2d, "setLineDash", js_ctx2d_setLineDash, 1);
    SET(proto2d, "getLineDash", js_ctx2d_getLineDash, 0);
    SET(proto2d, "isPointInPath", js_ctx2d_isPointInPath, 2);
    SET(proto2d, "isPointInStroke", js_ctx2d_isPointInStroke, 2);
    SET(proto2d, "clip", js_ctx2d_clip, 0);
    SET(proto2d, "createLinearGradient", js_createLinearGradient, 4);
    SET(proto2d, "createRadialGradient", js_createRadialGradient, 6);
    SET(proto2d, "createConicGradient", js_createConicGradient, 1);
    SET(proto2d, "createPattern", js_createPattern, 2);
    SET(proto2d, "drawImage", js_ctx2d_drawImage, 9);
    SET(proto2d, "getImageData", js_ctx2d_getImageData, 4);
    SET(proto2d, "putImageData", js_ctx2d_putImageData, 3);
    SET(proto2d, "createImageData", js_ctx2d_createImageData, 2);
    JS_SetPropertyStr(ctx, ctor2d, "prototype", proto2d);
    JS_SetPropertyStr(ctx, global, "CanvasRenderingContext2D", ctor2d);
}
#undef SET

static JSValue js_win_scroll_to(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    MiniBridge *b = bridge_of(ctx);
    if (!b || !b->doc || argc < 2)
        return JS_UNDEFINED;
    double x = 0, y = 0;
    JS_ToFloat64(ctx, &x, argv[0]);
    JS_ToFloat64(ctx, &y, argv[1]);
    b->doc->scroll_x = (float)x;
    b->doc->scroll_y = (float)y;
    if (b->doc->scroll_x < 0.0f)
        b->doc->scroll_x = 0.0f;
    if (b->doc->scroll_y < 0.0f)
        b->doc->scroll_y = 0.0f;
    return JS_UNDEFINED;
}

static JSValue js_win_scroll_by(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    MiniBridge *b = bridge_of(ctx);
    if (!b || !b->doc || argc < 2)
        return JS_UNDEFINED;
    double dx = 0, dy = 0;
    JS_ToFloat64(ctx, &dx, argv[0]);
    JS_ToFloat64(ctx, &dy, argv[1]);
    b->doc->scroll_x += (float)dx;
    b->doc->scroll_y += (float)dy;
    if (b->doc->scroll_x < 0.0f)
        b->doc->scroll_x = 0.0f;
    if (b->doc->scroll_y < 0.0f)
        b->doc->scroll_y = 0.0f;
    return JS_UNDEFINED;
}

/* ================================================================== */
/* WebSocket (RFC6455) JS binding                                       */
/* ================================================================== */
/* A live JS WebSocket is rooted here (a JS_DupValue of the wrapper) so
   onopen/onmessage/onclose keep firing even if JS dropped its reference,
   matching the browser. On close the wrapper is un-rooted; the class
   finalizer frees the C handle once JS has released its last ref. */
typedef struct JsWebSocket
{
    MiniWS *ws;
    JSValue obj; /* the JS wrapper (Dup'd while open) */
    struct MiniBridge *b;
    int dead; /* closed; sweep after pump */
} JsWebSocket;

/* dispatch a JS callback property ("onopen".."onclose") with an event object */
static void ws_fire(JSContext *ctx, JSValueConst obj, const char *prop,
                    JSValue ev)
{
    JSValue cb = JS_GetPropertyStr(ctx, obj, prop);
    if (JS_IsFunction(ctx, cb))
    {
        JSValue ret = JS_Call(ctx, cb, obj, 1, &ev);
        if (JS_IsException(ret))
        {
            JSValue e = JS_GetException(ctx);
            JS_FreeValue(ctx, e);
        }
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, cb);
    JS_FreeValue(ctx, ev);
}

static void ws_cb_open(MiniWS *ws, void *ud)
{
    (void)ws;
    JsWebSocket *j = (JsWebSocket *)ud;
    JSValue ev = JS_NewObject(j->b->ctx);
    JS_SetPropertyStr(j->b->ctx, ev, "type", JS_NewString(j->b->ctx, "open"));
    ws_fire(j->b->ctx, j->obj, "onopen", ev);
}
static void ws_cb_text(MiniWS *ws, const char *data, size_t len, void *ud)
{
    (void)ws;
    JsWebSocket *j = (JsWebSocket *)ud;
    JSValue ev = JS_NewObject(j->b->ctx);
    JS_SetPropertyStr(j->b->ctx, ev, "type", JS_NewString(j->b->ctx, "message"));
    JS_SetPropertyStr(j->b->ctx, ev, "data", JS_NewStringLen(j->b->ctx, data, len));
    ws_fire(j->b->ctx, j->obj, "onmessage", ev);
}
static void ws_cb_bin(MiniWS *ws, const uint8_t *data, size_t len, void *ud)
{
    (void)ws;
    JsWebSocket *j = (JsWebSocket *)ud;
    JSValue ev = JS_NewObject(j->b->ctx);
    JS_SetPropertyStr(j->b->ctx, ev, "type", JS_NewString(j->b->ctx, "message"));
    JSValue ab = JS_NewArrayBufferCopy(j->b->ctx, data ? data : (const uint8_t *)"", len);
    JS_SetPropertyStr(j->b->ctx, ev, "data", ab);
    ws_fire(j->b->ctx, j->obj, "onmessage", ev);
}
static void ws_cb_close(MiniWS *ws, int code, const char *reason, void *ud)
{
    (void)ws;
    JsWebSocket *j = (JsWebSocket *)ud;
    j->dead = 1;
    JS_SetPropertyStr(j->b->ctx, j->obj, "readyState", JS_NewInt32(j->b->ctx, 3)); /* CLOSED */
    JSValue ev = JS_NewObject(j->b->ctx);
    JS_SetPropertyStr(j->b->ctx, ev, "type", JS_NewString(j->b->ctx, "close"));
    JS_SetPropertyStr(j->b->ctx, ev, "code", JS_NewInt32(j->b->ctx, code));
    JS_SetPropertyStr(j->b->ctx, ev, "reason", JS_NewString(j->b->ctx, reason ? reason : ""));
    JS_SetPropertyStr(j->b->ctx, ev, "wasClean", JS_TRUE);
    ws_fire(j->b->ctx, j->obj, "onclose", ev);
}
static void ws_cb_err(MiniWS *ws, const char *msg, void *ud)
{
    (void)ws;
    JsWebSocket *j = (JsWebSocket *)ud;
    JSValue ev = JS_NewObject(j->b->ctx);
    JS_SetPropertyStr(j->b->ctx, ev, "type", JS_NewString(j->b->ctx, "error"));
    JS_SetPropertyStr(j->b->ctx, ev, "message", JS_NewString(j->b->ctx, msg ? msg : "error"));
    ws_fire(j->b->ctx, j->obj, "onerror", ev);
}

static void js_ws_finalizer(JSRuntime *rt, JSValue val)
{
    /* The bridge rooted the wrapper via a Dup while it was open; this runs only
       after the sweep un-rooted it (or at shutdown). We never free j->obj here
       (it IS the object being finalized). */
    MiniBridge *b = (MiniBridge *)JS_GetRuntimeOpaque(rt);
    if (!b)
        return;
    JsWebSocket *j = (JsWebSocket *)JS_GetOpaque(val, b->ws_cid);
    if (!j)
        return;
    if (j->ws)
    {
        mini_ws_destroy(j->ws);
        j->ws = NULL;
    }
    free(j);
}

static JsWebSocket *ws_this(JSContext *ctx, JSValueConst tv)
{
    MiniBridge *b = bridge_of(ctx);
    return (JsWebSocket *)JS_GetOpaque2(ctx, tv, b->ws_cid);
}

static JSValue js_ws_send(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    JsWebSocket *j = ws_this(ctx, tv);
    if (!j || !j->ws || mini_ws_state(j->ws) != MINI_WS_OPEN)
        return JS_ThrowTypeError(ctx, "WebSocket is not open");
    if (argc < 1)
        return JS_UNDEFINED;
    if (JS_IsString(argv[0]))
    {
        const char *s = JS_ToCString(ctx, argv[0]);
        int rc = mini_ws_send_text(j->ws, s ? s : "");
        JS_FreeCString(ctx, s);
        return rc == 0 ? JS_UNDEFINED : JS_ThrowTypeError(ctx, "send failed");
    }
    size_t len = 0, off = 0, el = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &len, &el);
    size_t bs = 0;
    uint8_t *p = JS_GetArrayBuffer(ctx, &bs, ab);
    JS_FreeValue(ctx, ab);
    if (!p)
        return JS_ThrowTypeError(ctx, "send: need a string or ArrayBuffer");
    int rc = mini_ws_send_binary(j->ws, p + off, len);
    return rc == 0 ? JS_UNDEFINED : JS_ThrowTypeError(ctx, "send failed");
}
static JSValue js_ws_close(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    JsWebSocket *j = ws_this(ctx, tv);
    if (!j || !j->ws)
        return JS_UNDEFINED;
    int code = 1000;
    if (argc >= 1)
        JS_ToInt32(ctx, &code, argv[0]);
    const char *reason = NULL;
    if (argc >= 2)
        reason = JS_ToCString(ctx, argv[1]);
    mini_ws_close(j->ws, code, reason ? reason : "");
    if (reason)
        JS_FreeCString(ctx, reason);
    return JS_UNDEFINED;
}
static JSValue js_ws_ctor(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    MiniBridge *b = bridge_of(ctx);
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "WebSocket: url required");
    const char *url = JS_ToCString(ctx, argv[0]);
    /* origin = the current page origin (document.location) if available */
    JSValue loc = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "location");
    JSValue hrefv = JS_GetPropertyStr(ctx, loc, "href");
    const char *origin = NULL;
    if (JS_IsString(hrefv))
        origin = JS_ToCString(ctx, hrefv);
    MiniWS *ws = mini_ws_connect(url ? url : "", origin);
    if (origin)
        JS_FreeCString(ctx, origin);
    JS_FreeValue(ctx, hrefv);
    JS_FreeValue(ctx, loc);
    JS_FreeCString(ctx, url);
    if (!ws)
        return JS_ThrowTypeError(ctx, "WebSocket connect failed");

    JSValue obj = JS_NewObjectClass(ctx, b->ws_cid);
    if (JS_IsException(obj))
    {
        mini_ws_destroy(ws);
        return obj;
    }
    JsWebSocket *j = (JsWebSocket *)calloc(1, sizeof *j);
    j->ws = ws;
    j->b = b;
    j->obj = JS_DupValue(ctx, obj); /* root while open */
    JS_SetOpaque(obj, j);
    JS_SetPropertyStr(ctx, obj, "url", JS_NewString(ctx, url ? url : ""));
    JS_SetPropertyStr(ctx, obj, "binaryType", JS_NewString(ctx, "arraybuffer"));
    JS_SetPropertyStr(ctx, obj, "bufferedAmount", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, obj, "readyState", JS_NewInt32(ctx, 1)); /* OPEN */
    mini_ws_set_callbacks(ws, ws_cb_open, ws_cb_text, ws_cb_bin, ws_cb_close, ws_cb_err, j);

    if (b->ws_n >= b->ws_cap)
    {
        int nc = b->ws_cap ? b->ws_cap * 2 : 8;
        JsWebSocket **na = (JsWebSocket **)realloc(b->ws_list, sizeof(JsWebSocket *) * nc);
        if (!na)
        {
            mini_ws_destroy(ws);
            JS_FreeValue(ctx, obj);
            free(j);
            return JS_ThrowTypeError(ctx, "OOM");
        }
        b->ws_list = na;
        b->ws_cap = nc;
    }
    b->ws_list[b->ws_n++] = j;
    return obj;
}

/* pump all live WebSockets; sweep closed ones (after onclose was dispatched).
 * Ownership: the bridge rooted the wrapper via JS_DupValue; on close we
 * detach, destroy the C handle, then un-root last — the JS finalizer frees
 * the JsWebSocket once JS has released its refs (so a stray ws.send() after
 * close still finds a valid, j->ws==NULL handle). */
static void bridge_pump_websockets(MiniBridge *b)
{
    for (int i = 0; i < b->ws_n; i++)
    {
        JsWebSocket *j = b->ws_list[i];
        if (!j)
            continue;
        if (j->ws && mini_ws_state(j->ws) != MINI_WS_CLOSED)
            mini_ws_pump(j->ws);
        if (j->dead || (j->ws && mini_ws_state(j->ws) == MINI_WS_CLOSED))
        {
            b->ws_list[i] = NULL; /* detach */
            if (j->ws)
            {
                mini_ws_destroy(j->ws);
                j->ws = NULL;
            }
            JS_FreeValue(b->ctx, j->obj); /* un-root; may finalize+free j */
            /* do NOT touch j after this point */
        }
    }
    int w = 0;
    for (int i = 0; i < b->ws_n; i++)
        if (b->ws_list[i])
            b->ws_list[w++] = b->ws_list[i];
    b->ws_n = w;
}

MiniBridge *mini_bridge_create(MiniRenderer *r, MiniDocument *doc)
{
    MiniBridge *b = (MiniBridge *)calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    g_active_js_bridge = b;
    b->r = r;
    b->doc = doc;
    b->gl = (MiniGLBridge *)r->gl_state;
    b->rt = JS_NewRuntime();
    if (!b->rt)
    {
        free(b);
        return NULL;
    }
    JS_SetMemoryLimit(b->rt, 512 * 1024 * 1024); /* 512 MB heap cap */
    JS_SetMaxStackSize(b->rt, 16 * 1024 * 1024); /* 16 MB stack */
    b->ctx = JS_NewContext(b->rt);
    if (!b->ctx)
    {
        JS_FreeRuntime(b->rt);
        free(b);
        return NULL;
    }
    JS_SetContextOpaque(b->ctx, b);
    JS_SetRuntimeOpaque(b->rt, b); /* the WS finalizer needs to reach the bridge */

    /* ES module loader: bare specifiers resolve through the import map,
       module sources are fetched over the network (HTTPS via MINI_TLS when
       wired) — same blocking path as the classic external <script src>. */
    JS_SetModuleLoaderFunc(b->rt, mini_module_normalize, mini_module_loader, b);

    /* element class */
    JS_NewClassID(b->rt, &b->el_cid);
    JSClassDef edef = {.class_name = "MiniElement", .finalizer = js_el_finalizer};
    JS_NewClass(b->rt, b->el_cid, &edef);
    install_element_proto(b->ctx, b->el_cid);

    /* document class */
    JS_NewClassID(b->rt, &b->doc_cid);
    JSClassDef ddef = {.class_name = "MiniDocument"};
    JS_NewClass(b->rt, b->doc_cid, &ddef);
    JSValue dproto = JS_NewObject(b->ctx);
    JS_SetPropertyStr(b->ctx, dproto, "createElement",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_createElement, "createElement", 1));
    JS_SetPropertyStr(b->ctx, dproto, "createTextNode",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_createTextNode, "createTextNode", 1));
    JS_SetPropertyStr(b->ctx, dproto, "getElementById",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_getElementById, "getElementById", 1));
    JS_SetPropertyStr(b->ctx, dproto, "_byId",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_byId, "_byId", 1));
    JS_SetPropertyStr(b->ctx, dproto, "_byTag",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_byTag, "_byTag", 1));
    JS_SetPropertyStr(b->ctx, dproto, "_cq",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_doc_query, "_cq", 1));
    JS_SetPropertyStr(b->ctx, dproto, "_cqa",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_doc_queryAll, "_cqa", 1));
    JS_SetPropertyStr(b->ctx, dproto, "_getActiveElement",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_doc_activeElement, "_getActiveElement", 0));
    JS_SetPropertyStr(b->ctx, dproto, "elementFromPoint",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_elementFromPoint, "elementFromPoint", 2));
    JS_SetPropertyStr(b->ctx, dproto, "createDocumentFragment",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_createDocumentFragment, "createDocumentFragment", 0));
    JS_SetPropertyStr(b->ctx, dproto, "createComment",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_createComment, "createComment", 1));
    /* createElementNS(name, ns) — this engine has one namespace; ignore ns. */
    JS_SetPropertyStr(b->ctx, dproto, "createElementNS",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_createElementNS, "createElementNS", 2));
    JS_SetClassProto(b->ctx, b->doc_cid, dproto); /* steals dproto */

    /* WebSocket class (finalizer frees the C handle when JS releases the obj) */
    JS_NewClassID(b->rt, &b->ws_cid);
    JSClassDef wsdef = {.class_name = "WebSocket", .finalizer = js_ws_finalizer};
    JS_NewClass(b->rt, b->ws_cid, &wsdef);
    JSValue wsproto = JS_NewObject(b->ctx);
    JS_SetPropertyStr(b->ctx, wsproto, "send",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_ws_send, "send", 1));
    JS_SetPropertyStr(b->ctx, wsproto, "close",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_ws_close, "close", 2));
    JS_SetClassProto(b->ctx, b->ws_cid, wsproto); /* steals wsproto */

    /* globals */
    JSValue global = JS_GetGlobalObject(b->ctx);
    JS_SetPropertyStr(b->ctx, global, "__mini_get_inner_width",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_get_inner_width, "__mini_get_inner_width", 0));
    JS_SetPropertyStr(b->ctx, global, "__mini_get_inner_height",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_get_inner_height, "__mini_get_inner_height", 0));
    JS_SetPropertyStr(b->ctx, global, "__mini_get_dpr",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_get_device_pixel_ratio, "__mini_get_dpr", 0));
    JS_SetPropertyStr(b->ctx, global, "__mini_get_scroll_x",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_get_scroll_x, "__mini_get_scroll_x", 0));
    JS_SetPropertyStr(b->ctx, global, "__mini_get_scroll_y",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_get_scroll_y, "__mini_get_scroll_y", 0));

    /* document instance */
    JSValue docobj = JS_NewObjectClass(b->ctx, b->doc_cid);
    JS_SetOpaque(docobj, doc);
    /* expose document.body / document.documentElement / document.head as wrapped elements */
    struct MiniNode *head_node = NULL;
    if (doc && doc->root)
    {
        for (struct MiniNode *c = doc->root->first_child; c; c = c->next_sibling)
        {
            if (c->tag && !strcmp(c->tag, "head"))
            {
                head_node = c;
                break;
            }
        }
    }
    JS_SetPropertyStr(b->ctx, docobj, "head",
                      wrap_node(b->ctx, head_node ? head_node : doc->root, b->el_cid));
    JS_SetPropertyStr(b->ctx, docobj, "body",
                      wrap_node(b->ctx, doc->body, b->el_cid));
    JS_SetPropertyStr(b->ctx, docobj, "documentElement",
                      wrap_node(b->ctx, doc->root, b->el_cid));
    JS_SetPropertyStr(b->ctx, docobj, "addEventListener",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_addEventListener, "addEventListener", 3));
    JS_SetPropertyStr(b->ctx, docobj, "removeEventListener",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_removeEventListener, "removeEventListener", 3));
    JS_SetPropertyStr(b->ctx, docobj, "dispatchEvent",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_dispatchEvent, "dispatchEvent", 1));
    JS_SetPropertyStr(b->ctx, docobj, "_exitPointerLock",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_exitPointerLock, "_exitPointerLock", 0));
    JS_SetPropertyStr(b->ctx, docobj, "_pointerLockElement",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_doc_pointerLockElement, "_pointerLockElement", 0));
    JS_SetPropertyStr(b->ctx, global, "document", docobj);

    /* fetch() + localStorage/sessionStorage (Network + Storage domains) */
    install_net_storage(b->ctx, global);

    /* WebGL + 2D context constructors */
    install_webgl(b->ctx, global);

    /* window (plain object) */
    JSValue win = JS_DupValue(b->ctx, global);
    JS_SetPropertyStr(b->ctx, win, "requestAnimationFrame",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_requestAnimationFrame, "requestAnimationFrame", 1));
    JS_SetPropertyStr(b->ctx, win, "cancelAnimationFrame",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_cancelAnimationFrame, "cancelAnimationFrame", 1));
    JS_SetPropertyStr(b->ctx, win, "setTimeout",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_setTimeout, "setTimeout", 2));
    JS_SetPropertyStr(b->ctx, win, "setInterval",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_setInterval, "setInterval", 2));
    JS_SetPropertyStr(b->ctx, win, "clearTimeout",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_clearTimer, "clearTimeout", 1));
    JS_SetPropertyStr(b->ctx, win, "clearInterval",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_clearTimer, "clearInterval", 1));
    JS_SetPropertyStr(b->ctx, win, "__mini_log_poll",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_log_poll, "__mini_log_poll", 1));
    JS_SetPropertyStr(b->ctx, win, "scrollTo",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_win_scroll_to, "scrollTo", 2));
    JS_SetPropertyStr(b->ctx, win, "scrollBy",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_win_scroll_by, "scrollBy", 2));
    JS_SetPropertyStr(b->ctx, win, "scroll",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_win_scroll_to, "scroll", 2));
    JSValue perf = JS_NewObject(b->ctx);
    JS_SetPropertyStr(b->ctx, perf, "now",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_now, "now", 0));
    JS_SetPropertyStr(b->ctx, win, "performance", perf); /* steals perf */
    JS_SetPropertyStr(b->ctx, win, "addEventListener",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_addEventListener, "addEventListener", 3));
    JS_SetPropertyStr(b->ctx, win, "removeEventListener",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_removeEventListener, "removeEventListener", 3));
    JS_SetPropertyStr(b->ctx, win, "dispatchEvent",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_dispatchEvent, "dispatchEvent", 1));

    JSValue gl_ctor = JS_GetPropertyStr(b->ctx, global, "WebGLRenderingContext");
    JS_SetPropertyStr(b->ctx, win, "WebGLRenderingContext", JS_DupValue(b->ctx, gl_ctor));
    JSValue gl2_ctor = JS_GetPropertyStr(b->ctx, global, "WebGL2RenderingContext");
    JS_SetPropertyStr(b->ctx, win, "WebGL2RenderingContext", gl2_ctor);
    JS_FreeValue(b->ctx, gl_ctor);

    JSValue c2d_ctor = JS_GetPropertyStr(b->ctx, global, "CanvasRenderingContext2D");
    JS_SetPropertyStr(b->ctx, win, "CanvasRenderingContext2D", c2d_ctor);

    /* window.document / window.window / window.self */
    JS_SetPropertyStr(b->ctx, win, "document", JS_DupValue(b->ctx, docobj));
    JS_SetPropertyStr(b->ctx, win, "window", JS_DupValue(b->ctx, win));
    JS_SetPropertyStr(b->ctx, win, "self", JS_DupValue(b->ctx, win));

    /* navigator object */
    JSValue nav = JS_NewObject(b->ctx);
    JS_SetPropertyStr(b->ctx, nav, "userAgent", JS_NewString(b->ctx, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"));
    JS_SetPropertyStr(b->ctx, nav, "appVersion", JS_NewString(b->ctx, "5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"));
    JS_SetPropertyStr(b->ctx, nav, "appName", JS_NewString(b->ctx, "Netscape"));
    JS_SetPropertyStr(b->ctx, nav, "appCodeName", JS_NewString(b->ctx, "Mozilla"));
    JS_SetPropertyStr(b->ctx, nav, "vendor", JS_NewString(b->ctx, "Google Inc."));
    JS_SetPropertyStr(b->ctx, nav, "platform", JS_NewString(b->ctx, "Win32"));
    JS_SetPropertyStr(b->ctx, nav, "language", JS_NewString(b->ctx, "en-US"));
    JS_SetPropertyStr(b->ctx, nav, "maxTouchPoints", JS_NewInt32(b->ctx, 0));
    JS_SetPropertyStr(b->ctx, nav, "cookieEnabled", JS_TRUE);
    JS_SetPropertyStr(b->ctx, nav, "onLine", JS_TRUE);
    JS_SetPropertyStr(b->ctx, win, "navigator", JS_DupValue(b->ctx, nav));
    JS_SetPropertyStr(b->ctx, global, "navigator", nav);

    /* location object */
    JSValue loc = JS_NewObject(b->ctx);
    JS_SetPropertyStr(b->ctx, loc, "href", JS_NewString(b->ctx, b->doc_url ? b->doc_url : "http://localhost/"));
    JS_SetPropertyStr(b->ctx, loc, "origin", JS_NewString(b->ctx, "http://localhost"));
    JS_SetPropertyStr(b->ctx, loc, "protocol", JS_NewString(b->ctx, "http:"));
    JS_SetPropertyStr(b->ctx, loc, "host", JS_NewString(b->ctx, "localhost"));
    JS_SetPropertyStr(b->ctx, loc, "hostname", JS_NewString(b->ctx, "localhost"));
    JS_SetPropertyStr(b->ctx, loc, "port", JS_NewString(b->ctx, ""));
    JS_SetPropertyStr(b->ctx, loc, "pathname", JS_NewString(b->ctx, "/"));
    JS_SetPropertyStr(b->ctx, loc, "search", JS_NewString(b->ctx, ""));
    JS_SetPropertyStr(b->ctx, loc, "hash", JS_NewString(b->ctx, ""));
    JS_SetPropertyStr(b->ctx, win, "location", JS_DupValue(b->ctx, loc));
    JS_SetPropertyStr(b->ctx, docobj, "location", JS_DupValue(b->ctx, loc));
    JS_SetPropertyStr(b->ctx, global, "location", loc);

    JS_SetPropertyStr(b->ctx, global, "window", win);
    JS_SetPropertyStr(b->ctx, global, "self", JS_DupValue(b->ctx, win));
    JS_SetPropertyStr(b->ctx, global, "scrollTo",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_win_scroll_to, "scrollTo", 2));
    JS_SetPropertyStr(b->ctx, global, "scrollBy",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_win_scroll_by, "scrollBy", 2));
    JS_SetPropertyStr(b->ctx, global, "scroll",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_win_scroll_to, "scroll", 2));
    JS_SetPropertyStr(b->ctx, global, "requestAnimationFrame",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_requestAnimationFrame, "requestAnimationFrame", 1));
    JS_SetPropertyStr(b->ctx, global, "setTimeout",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_setTimeout, "setTimeout", 2));
    JS_SetPropertyStr(b->ctx, global, "setInterval",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_setInterval, "setInterval", 2));
    JS_SetPropertyStr(b->ctx, global, "clearTimeout",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_clearTimer, "clearTimeout", 1));
    JS_SetPropertyStr(b->ctx, global, "clearInterval",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_clearTimer, "clearInterval", 1));
    JS_SetPropertyStr(b->ctx, global, "__mini_log_poll",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_log_poll, "__mini_log_poll", 1));
    /* WebSocket constructor (RFC6455 client; instances inherit send/close) */
    {
        JSValue wsctor = JS_NewCFunction2(b->ctx, (JSCFunction *)js_ws_ctor, "WebSocket", 1,
                                          JS_CFUNC_constructor, 0);
        JSValue cproto = JS_GetClassProto(b->ctx, b->ws_cid);
        JS_SetPropertyStr(b->ctx, wsctor, "prototype", cproto); /* steal */
        JS_SetPropertyStr(b->ctx, global, "WebSocket", wsctor);
        /* CONNECTING..CLOSED constants on the constructor */
        JS_SetPropertyStr(b->ctx, wsctor, "CONNECTING", JS_NewInt32(b->ctx, MINI_WS_CONNECTING));
        JS_SetPropertyStr(b->ctx, wsctor, "OPEN", JS_NewInt32(b->ctx, MINI_WS_OPEN));
        JS_SetPropertyStr(b->ctx, wsctor, "CLOSING", JS_NewInt32(b->ctx, MINI_WS_CLOSING));
        JS_SetPropertyStr(b->ctx, wsctor, "CLOSED", JS_NewInt32(b->ctx, MINI_WS_CLOSED));
    }
    JS_SetPropertyStr(b->ctx, global, "__log",
                      JS_NewCFunction(b->ctx, (JSCFunction *)js_console_log, "__log", 2));

    /* Expose MiniElement / MiniDocument / HTMLCanvasElement so the JS shim
       can attach standard property descriptors onto their prototype.
       The .prototype of each ctor is the class proto (the very object
       instances inherit from), so shim mutations reach live instances. */
    JSValue el_ctor = JS_NewCFunction2(b->ctx, (JSCFunction *)js_illegal_ctor, "MiniElement", 0, JS_CFUNC_constructor, 0);
    JSValue el_proto = JS_GetClassProto(b->ctx, b->el_cid);
    JS_SetPropertyStr(b->ctx, el_ctor, "prototype", el_proto); /* steal */
    JS_SetPropertyStr(b->ctx, global, "MiniElement",
                      JS_DupValue(b->ctx, el_ctor));
    JS_SetPropertyStr(b->ctx, global, "HTMLCanvasElement",
                      JS_DupValue(b->ctx, el_ctor));
    JS_FreeValue(b->ctx, el_ctor);

    JSValue doc_ctor = JS_NewCFunction2(b->ctx, (JSCFunction *)js_illegal_ctor, "MiniDocument", 0, JS_CFUNC_constructor, 0);
    JSValue doc_proto = JS_GetClassProto(b->ctx, b->doc_cid);
    JS_SetPropertyStr(b->ctx, doc_ctor, "prototype", doc_proto); /* steal */
    JS_SetPropertyStr(b->ctx, global, "MiniDocument",
                      JS_DupValue(b->ctx, doc_ctor));
    JS_FreeValue(b->ctx, doc_ctor);

    JS_FreeValue(b->ctx, global);

    /* install the JS shim that wires property descriptors */
    JSValue shim = JS_Eval(b->ctx, mini_js_shim, strlen(mini_js_shim),
                           "<shim>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(shim))
    {
        JSValue ex = JS_GetException(b->ctx);
        const char *s = JS_ToCString(b->ctx, ex);
        JSValue stack = JS_GetPropertyStr(b->ctx, ex, "stack");
        const char *st = JS_ToCString(b->ctx, stack);
        fprintf(stderr, "[shim error] %s\n%s\n", s ? s : "?", st ? st : "(no stack)");
        JS_FreeCString(b->ctx, s);
        JS_FreeCString(b->ctx, st);
        JS_FreeValue(b->ctx, stack);
        JS_FreeValue(b->ctx, ex);
    }
    JS_FreeValue(b->ctx, shim);
    return b;
}

static void free_node_wrappers_rec(JSContext *ctx, struct MiniNode *n)
{
    if (!n)
        return;
    if (n->js_wrapper)
    {
        JSValue *v = (JSValue *)n->js_wrapper;
        JS_FreeValue(ctx, *v);
        free(v);
        n->js_wrapper = NULL;
    }
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        free_node_wrappers_rec(ctx, c);
    if (n->shadow_root)
        free_node_wrappers_rec(ctx, n->shadow_root);
}

void mini_bridge_destroy(MiniBridge *b)
{
    if (!b)
        return;
    if (g_active_js_bridge == b)
        g_active_js_bridge = NULL;

    /* 0. Join any in-flight parallel prefetch threads before tearing down —
       they only touch the (mutex-protected) HTTP cache, not the JS runtime,
       but must finish so their cache writes land before the process exits. */
    mini_net_prefetch_shutdown();

    /* 1. 释放 rAF 队列中的 JS 回调 */
    for (int i = 0; i < b->raf_n; i++)
        JS_FreeValue(b->ctx, b->raf[i].cb);
    free(b->raf);
    b->raf = NULL;
    b->raf_n = b->raf_cap = 0;

    /* 2. 释放定时器中的 JS 回调 */
    for (int i = 0; i < b->tm_n; i++)
        JS_FreeValue(b->ctx, b->timers[i].cb);
    free(b->timers);
    b->timers = NULL;
    b->tm_n = b->tm_cap = 0;

    /* 3. 释放 JS 事件监听器（核心修复：补全 L->js_target 的释放） */
    for (int i = 0; i < b->ev_listeners_n; i++)
    {
        JsEvListener *L = b->ev_listeners[i];
        if (!L)
            continue;
        if (b->ev && L->handle)
            mini_events_remove_listener(b->ev, L->handle);
        JS_FreeValue(b->ctx, L->cb);
        JS_FreeValue(b->ctx, L->js_target); /* 修复点：释放目标对象引用 */
        free(L);
    }
    free(b->ev_listeners);
    b->ev_listeners = NULL;
    b->ev_listeners_n = b->ev_listeners_cap = 0;

    /* 4. 核心修复：在释放 Context 之前，彻底解开所有 DOM 节点缓存的 JSValue */
    for (int i = 0; i < b->all_wrappers_n; i++)
    {
        JSValue *v = b->all_wrappers[i];
        if (v)
        {
            struct MiniNode *n = (struct MiniNode *)JS_GetOpaque(*v, b->el_cid);
            if (n && n->js_wrapper == v)
                n->js_wrapper = NULL;
            JS_SetOpaque(*v, NULL);
            JS_FreeValue(b->ctx, *v);
            free(v);
        }
    }
    free(b->all_wrappers);
    b->all_wrappers = NULL;
    b->all_wrappers_n = b->all_wrappers_cap = 0;

    /* 5. 关闭并释放所有活跃的 WebSockets */
    for (int i = 0; i < b->ws_n; i++)
    {
        JsWebSocket *j = b->ws_list[i];
        if (!j)
            continue;
        if (j->ws)
        {
            mini_ws_destroy(j->ws);
            j->ws = NULL;
        }
        JS_FreeValue(b->ctx, j->obj);
    }
    free(b->ws_list);
    b->ws_list = NULL;
    b->ws_n = b->ws_cap = 0;

    /* 6. 释放 Import Maps 与 URL 缓存 */
    for (int i = 0; i < b->im_n; i++)
    {
        free(b->im_keys[i]);
        free(b->im_vals[i]);
    }
    free(b->im_keys);
    free(b->im_vals);
    b->im_keys = b->im_vals = NULL;
    b->im_n = b->im_cap = 0;
    free(b->doc_url);
    b->doc_url = NULL;

    /* 7. 排空所有挂起的微任务与 Promise Job，确保 GC 干净 */
    if (b->rt)
    {
        for (;;)
        {
            JSContext *pctx = NULL;
            int r = JS_ExecutePendingJob(b->rt, &pctx);
            if (r <= 0)
                break;
        }
    }

    /* 8. 依次安全释放 Context 与 Runtime */
    if (b->ctx)
    {
        JS_FreeContext(b->ctx);
        b->ctx = NULL;
    }
    if (b->rt)
    {
        JS_RunGC(b->rt);
        JS_FreeRuntime(b->rt);
        b->rt = NULL;
    }
    free(b);
}

/* Attach the event system so JS addEventListener registers into the state
   the host loop (main.c) drives via mini_events_handle_*.                */
void mini_bridge_set_events(MiniBridge *b, struct MiniEventState *ev)
{
    if (b)
    {
        b->ev = ev;
        if (ev)
            mini_events_set_inline_handler(ev, js_inline_event_handler, b);
    }
}

/* Phase 1.1+1.2: parse a raw HTML string into the live DOM, apply <style>
   rules, then run every inline <script> in document order. */
static void apply_styles(struct MiniNode *n, MiniBridge *b)
{
    if (!n)
        return;
    if (n->type == MN_ELEMENT_NODE && n->tag && !strcmp(n->tag, "style"))
    {
        const char *css = (n->text && n->text[0]) ? n->text : (n->first_child && n->first_child->text ? n->first_child->text : NULL);
        if (css && css[0])
            mini_css_apply(b->doc, css);
    }
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        apply_styles(c, b);
}

/* ================================================================== */
/* ES module support (<script type=module> + <script type=importmap>).  */
/* ================================================================== */

/* A <script type=...> runs as a classic script when type is absent or one of
   the JS MIME types; "module"/"importmap" are handled separately elsewhere. */
static int is_classic_script(const char *type)
{
    if (!type)
        return 1;
    if (strstr(type, "javascript") || strstr(type, "ecmascript"))
        return 1;
    return 0;
}

/* Join a relative URL `ref` against base URL `base` (handles ./ ../ and
   root-relative). Returns a js_malloc'd string the caller owns, or NULL. */
static char *mini_url_join(JSContext *ctx, const char *base, const char *ref)
{
    const char *sch = strstr(base, "://");
    if (!sch)
        return js_strdup(ctx, ref);
    const char *host = sch + 3;
    const char *path = strchr(host, '/');
    size_t authlen = path ? (size_t)(path - base) : strlen(base);
    /* Directory portion of the path up to & incl the last '/'. A leading '/'
       in ref means root-relative, so the base directory is dropped entirely. */
    size_t dirlen = 0;
    if (ref[0] != '/')
    {
        const char *lastslash = path ? strrchr(path, '/') : NULL;
        dirlen = lastslash ? (size_t)(lastslash - path) + 1 : 0;
    }
    size_t rlen = strlen(ref);
    char *raw = js_malloc(ctx, dirlen + rlen + 1);
    if (!raw)
        return NULL;
    if (dirlen)
        memcpy(raw, path, dirlen);
    memcpy(raw + dirlen, ref, rlen + 1);

    /* Normalize into `out`: scheme://authority + '/'-joined segments,
       collapsing '.'/'..'. A small start-index stack drives pop on '..'. */
    size_t cap = authlen + dirlen + rlen + 2;
    char *out = js_malloc(ctx, cap);
    if (!out)
    {
        js_free(ctx, raw);
        return NULL;
    }
    int *stk = (int *)js_malloc(ctx, sizeof(int) * 256);
    if (!stk)
    {
        js_free(ctx, raw);
        js_free(ctx, out);
        return NULL;
    }
    memcpy(out, base, authlen);
    size_t opos = authlen;
    size_t sp = 0;
    char *p = raw;
    while (*p)
    {
        while (*p == '/')
            p++; /* skip separators; we add our own */
        if (!*p)
            break;
        char *e = p;
        while (*e && *e != '/')
            e++;
        size_t slen = (size_t)(e - p);
        if (slen == 1 && p[0] == '.')
        {
            /* current dir: skip */
        }
        else if (slen == 2 && p[0] == '.' && p[1] == '.')
        {
            if (sp > 0)
            {
                sp--;
                opos = (sp ? (size_t)stk[sp] - 1 : authlen);
            }
        }
        else if (sp < 255)
        {
            out[opos++] = '/';
            stk[sp++] = (int)opos; /* segment content starts here */
            memcpy(out + opos, p, slen);
            opos += slen;
        }
        p = e;
    }
    out[opos] = 0;
    js_free(ctx, stk);
    js_free(ctx, raw);
    if (opos == authlen)
    {
        out[opos++] = '/';
        out[opos] = 0;
    } /* keep '/' root */
    return out;
}

/* Set import.meta.url = url on a compiled module value. Mirrors quickjs-libc's
   js_module_set_import_meta but minimal (the module name is already a URL). */
static void mini_set_import_meta(JSContext *ctx, JSValueConst func_val,
                                 const char *url)
{
    if (JS_VALUE_GET_TAG(func_val) != JS_TAG_MODULE)
        return;
    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(func_val);
    JSValue meta = JS_GetImportMeta(ctx, m);
    if (JS_IsException(meta))
    {
        JS_FreeValue(ctx, meta);
        return;
    }
    JS_DefinePropertyValueStr(ctx, meta, "url",
                              JS_NewString(ctx, url), JS_PROP_C_W_E);
    JS_FreeValue(ctx, meta);
}

/* Module normalizer: absolute URL -> as-is; bare specifier -> import map
   (exact key, then longest trailing-slash prefix); relative -> join vs base.
   Returns a js_malloc'd string (QuickJS frees it) or NULL (throw). */
static char *mini_module_normalize(JSContext *ctx, const char *base,
                                   const char *name, void *opaque)
{
    (void)base;
    MiniBridge *b = (MiniBridge *)opaque;
    if (!name)
        return NULL;
    if (!strncmp(name, "http://", 7) || !strncmp(name, "https://", 8))
        return js_strdup(ctx, name);
    /* bare specifier: no scheme, not an absolute/relative path */
    if (!strchr(name, ':') && name[0] != '/' && name[0] != '.')
    {
        for (int i = 0; i < b->im_n; i++) /* exact match */
            if (!strcmp(b->im_keys[i], name))
                return js_strdup(ctx, b->im_vals[i]);
        int best = -1;
        size_t best_len = 0; /* longest trailing-slash prefix */
        for (int i = 0; i < b->im_n; i++)
        {
            size_t kl = strlen(b->im_keys[i]);
            if (kl > 0 && b->im_keys[i][kl - 1] == '/' &&
                !strncmp(b->im_keys[i], name, kl) && kl > best_len)
            {
                best = i;
                best_len = kl;
            }
        }
        if (best >= 0)
        {
            const char *rest = name + best_len;
            size_t vl = strlen(b->im_vals[best]), rl = strlen(rest);
            char *out = js_malloc(ctx, vl + rl + 1);
            if (!out)
                return NULL;
            memcpy(out, b->im_vals[best], vl);
            memcpy(out + vl, rest, rl + 1);
            return out;
        }
        JS_ThrowTypeError(ctx, "bare specifier '%s' not in import map", name);
        return NULL;
    }
    /* relative / root-relative: join against the importing module's URL */
    if (base && base[0])
    {
        char *out = mini_url_join(ctx, base, name);
        if (out)
            return out;
    }
    JS_ThrowTypeError(ctx, "could not resolve module specifier '%s'", name);
    return NULL;
}

static void get_qjc_path(const char *module_name, char *out, size_t cap)
{
#if defined(_WIN32)
    const char *appdata = getenv("LOCALAPPDATA");
    if (!appdata || !appdata[0]) appdata = getenv("APPDATA");
    if (appdata && appdata[0])
    {
        uint64_t h1 = 0xcbf29ce484222325ULL, h2 = 0x100000001b3ULL;
        for (const unsigned char *p = (const unsigned char *)module_name; *p; p++)
        {
            h1 = (h1 ^ *p) * 0x100000001b3ULL;
            h2 = (h2 + *p) * 0xcbf29ce484222325ULL;
        }
        snprintf(out, cap, "%s\\TinyFramework\\cache\\%016llx%016llx.qjc",
                 appdata, (unsigned long long)h1, (unsigned long long)h2);
        return;
    }
#endif
    uint64_t h1 = 0xcbf29ce484222325ULL, h2 = 0x100000001b3ULL;
    for (const unsigned char *p = (const unsigned char *)module_name; *p; p++)
    {
        h1 = (h1 ^ *p) * 0x100000001b3ULL;
        h2 = (h2 + *p) * 0xcbf29ce484222325ULL;
    }
    snprintf(out, cap, ".tiny_cache/%016llx%016llx.qjc",
             (unsigned long long)h1, (unsigned long long)h2);
}

/* Module loader: fetch module_name over the network and compile as a module
   (COMPILE_ONLY). The graph is instantiated by JS_EvalFunction of the entry
   module, which calls back here per import. Returns the JSModuleDef* (still
   referenced by the context) or NULL (throw) on failure. */
static JSModuleDef *mini_module_loader(JSContext *ctx, const char *module_name,
                                       void *opaque)
{
    (void)opaque;
    char *src_buf = NULL;
    size_t src_len = 0;
    int is_local = 0;

    /* 1. Fast path: check precompiled bytecode cache */
    char qjc_path[600];
    get_qjc_path(module_name, qjc_path, sizeof qjc_path);
    FILE *fq = fopen(qjc_path, "rb");
    if (fq)
    {
        fseek(fq, 0, SEEK_END);
        long qsz = ftell(fq);
        fseek(fq, 0, SEEK_SET);
        if (qsz > 0)
        {
            uint8_t *qbuf = (uint8_t *)malloc((size_t)qsz);
            if (qbuf && fread(qbuf, 1, (size_t)qsz, fq) == (size_t)qsz)
            {
                fclose(fq);
                JSValue val = JS_ReadObject(ctx, qbuf, (size_t)qsz, JS_READ_OBJ_BYTECODE);
                free(qbuf);
                if (!JS_IsException(val))
                {
                    mini_set_import_meta(ctx, val, module_name);
                    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(val);
                    JS_FreeValue(ctx, val);
                    return m;
                }
            }
            else
            {
                if (qbuf) free(qbuf);
                fclose(fq);
            }
        }
        else
        {
            fclose(fq);
        }
    }

    const char *path = module_name;
    if (!strncmp(path, "file:///", 8))
        path += 8;
    else if (!strncmp(path, "file://", 7))
        path += 7;

    if (!strstr(module_name, "://") || !strncmp(module_name, "file://", 7))
    {
        /* Local file */
        FILE *fp = fopen(path, "rb");
        if (fp)
        {
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (sz > 0)
            {
                src_buf = (char *)malloc((size_t)sz + 1);
                if (src_buf)
                {
                    size_t r = fread(src_buf, 1, (size_t)sz, fp);
                    src_buf[r] = 0;
                    src_len = r;
                    is_local = 1;
                }
            }
            fclose(fp);
        }
    }

    if (!is_local)
    {
        MiniNetRecord rec;
        memset(&rec, 0, sizeof rec);
        /* Join a parallel prefetch seeded by parse_importmap, then serve from
           the HTTP cache (or revalidate). mini_net_fetch already encompasses
           the network path, so the old mini_net_http retry is redundant. */
        mini_net_prefetch_await(module_name);
        if (mini_net_fetch("GET", module_name, NULL, NULL, 0, NULL, &rec) == 0 && rec.resp_body)
        {
            src_buf = rec.resp_body;
            src_len = rec.resp_body_len;
            rec.resp_body = NULL; /* take ownership */
            mini_net_record_add(&rec);
        }
        else
        {
            fprintf(stderr, "\033[31m[ESM Fatal] 网络加载失败，模块中止: %s\033[0m\n", module_name);
            mini_net_record_free(&rec);
            JS_ThrowReferenceError(ctx, "could not load module '%s'", module_name);
            return NULL;
        }
    }

    JSValue val = JS_Eval(ctx, src_buf, src_len, module_name,
                          JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(src_buf);
    if (JS_IsException(val))
    {
        JSValue exc = JS_GetException(ctx);
        const char *estr = JS_ToCString(ctx, exc);
        fprintf(stderr, "\033[31m[ESM Compile Error] %s: %s\033[0m\n", module_name, estr ? estr : "unknown");
        if (estr) JS_FreeCString(ctx, estr);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, val);
        return NULL;
    }

    /* Save precompiled bytecode to cache for instant future loads */
    size_t bc_len = 0;
    uint8_t *bc = JS_WriteObject(ctx, &bc_len, val, JS_WRITE_OBJ_BYTECODE);
    if (bc && bc_len > 0)
    {
        FILE *fqw = fopen(qjc_path, "wb");
        if (fqw)
        {
            fwrite(bc, 1, bc_len, fqw);
            fclose(fqw);
        }
        js_free(ctx, bc);
    }

    mini_set_import_meta(ctx, val, module_name);
    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(val);
    JS_FreeValue(ctx, val);
    return m;
}

/* Parse <script type=importmap> JSON into b->im_keys/im_vals. Leans on
   QuickJS's JSON parser (already linked) instead of a hand-rolled one. */
static void parse_importmap(MiniBridge *b, const char *text, size_t len)
{
    if (!b || !text || !len)
        return;
    JSContext *ctx = b->ctx;
    JSValue root = JS_ParseJSON(ctx, text, len, "<importmap>");
    if (JS_IsException(root))
    {
        JS_FreeValue(ctx, root);
        return;
    }
    JSValue imports = JS_GetPropertyStr(ctx, root, "imports");
    if (!JS_IsException(imports) && JS_IsObject(imports))
    {
        JSPropertyEnum *tab = NULL;
        uint32_t n = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, imports,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0)
        {
            for (uint32_t i = 0; i < n; i++)
            {
                const char *key = JS_AtomToCString(ctx, tab[i].atom);
                JSValue val = JS_GetPropertyStr(ctx, imports, key);
                const char *vs = JS_ToCString(ctx, val);
                if (key && vs && *vs)
                {
                    if (b->im_n == b->im_cap)
                    {
                        int nc = b->im_cap ? b->im_cap * 2 : 8;
                        char **k = (char **)realloc(b->im_keys, nc * sizeof(char *));
                        char **v = (char **)realloc(b->im_vals, nc * sizeof(char *));
                        if (k && v)
                        {
                            b->im_keys = k;
                            b->im_vals = v;
                            b->im_cap = nc;
                        }
                        else
                        {
                            free(k);
                            free(v); /* one may be NULL; freeing NULL is safe */
                            JS_FreeCString(ctx, key);
                            JS_FreeCString(ctx, vs);
                            JS_FreeValue(ctx, val);
                            continue;
                        }
                    }
                    b->im_keys[b->im_n] = strdup(key);
                    b->im_vals[b->im_n] = strdup(vs);
                    if (b->im_keys[b->im_n] && b->im_vals[b->im_n])
                        b->im_n++;
                    else
                    {
                        free(b->im_keys[b->im_n]);
                        free(b->im_vals[b->im_n]);
                    }
                }
                JS_FreeCString(ctx, key);
                JS_FreeCString(ctx, vs);
                JS_FreeValue(ctx, val);
            }
            JS_FreePropertyEnum(ctx, tab, n);
        }
    }
    JS_FreeValue(ctx, imports);
    JS_FreeValue(ctx, root);

    /* Parallel prefetch: seed a background fetch per importmap target so the
       module loader's mini_net_prefetch_await() + mini_net_fetch() find a warm
       cache hit instead of serially handshaking TLS per module (the ~10s
       preact+htm load becomes ~max(per-module)). Only http(s) targets. */
    for (int i = 0; i < b->im_n; i++)
    {
        const char *v = b->im_vals[i];
        if (v && (!strncmp(v, "http://", 7) || !strncmp(v, "https://", 8)))
            mini_net_prefetch(v);
    }
}

/* Evaluate a module script (inline or fetched): compile (COMPILE_ONLY), set
   import.meta, then JS_EvalFunction instantiates the graph (calling the loader
   for each import) and runs it. Reports errors like mini_bridge_eval. */
static int mini_bridge_eval_module(MiniBridge *b, const char *src, size_t len,
                                   const char *name)
{
    JSValue val = JS_Eval(b->ctx, src, len, name,
                          JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(val))
    {
        JSValue ex = JS_GetException(b->ctx);
        const char *s = JS_ToCString(b->ctx, ex);
        fprintf(stderr, "[eval error] %s\n", s ? s : "?");
        JS_FreeCString(b->ctx, s);
        JS_FreeValue(b->ctx, ex);
        JS_FreeValue(b->ctx, val);
        return -1;
    }
    mini_set_import_meta(b->ctx, val, name);
    JSValue r = JS_EvalFunction(b->ctx, val); /* instantiate + run the graph; may throw */
    if (JS_IsException(r))
    {
        JSValue ex = JS_GetException(b->ctx);
        const char *s = JS_ToCString(b->ctx, ex);
        fprintf(stderr, "[eval error] %s\n", s ? s : "?");
        JS_FreeCString(b->ctx, s);
        JS_FreeValue(b->ctx, ex);
    }
    JS_FreeValue(b->ctx, r);
    return 0;
}

static void eval_scripts(struct MiniNode *n, MiniBridge *b)
{
    if (!n)
        return;
    if (n->type == MN_ELEMENT_NODE && n->tag && !strcmp(n->tag, "script"))
    {
        const char *type = mini_node_get_attribute(n, "type");
        const char *src = mini_node_get_attribute(n, "src");
        int is_remote = src && (!strncmp(src, "http://", 7) || !strncmp(src, "https://", 8));
        int is_local_src = src && src[0] && !is_remote;

        const char *script_text = n->text;
        if (!script_text && n->first_child && n->first_child->text)
            script_text = n->first_child->text;

        if (type && !strcmp(type, "importmap"))
        {
            if (script_text)
                parse_importmap(b, script_text, strlen(script_text));
        }
        else if (type && !strcmp(type, "module"))
        {
            if (is_remote)
            {
                MiniNetRecord rec;
                memset(&rec, 0, sizeof rec);
                mini_net_prefetch_await(src);
                if (mini_net_fetch("GET", src, NULL, NULL, 0, NULL, &rec) == 0 && rec.resp_body)
                {
                    mini_bridge_eval_module(b, rec.resp_body, rec.resp_body_len, src);
                    mini_net_record_add(&rec);
                }
            }
            else if (is_local_src)
            {
                const char *fpath = src;
                if (!strncmp(fpath, "file:///", 8)) fpath += 8;
                else if (!strncmp(fpath, "file://", 7)) fpath += 7;
                FILE *fp = fopen(fpath, "rb");
                if (fp) {
                    fseek(fp, 0, SEEK_END);
                    long sz = ftell(fp);
                    fseek(fp, 0, SEEK_SET);
                    if (sz > 0) {
                        char *code = (char *)malloc(sz + 1);
                        if (code) {
                            size_t r = fread(code, 1, sz, fp);
                            code[r] = 0;
                            mini_bridge_eval_module(b, code, r, src);
                            free(code);
                        }
                    }
                    fclose(fp);
                }
            }
            else if (script_text)
            {
                mini_bridge_eval_module(b, script_text, strlen(script_text),
                                        b->doc_url ? b->doc_url : "<inline-module>");
            }
        }
        else if (is_classic_script(type))
        {
            if (is_remote)
            {
                MiniNetRecord rec;
                memset(&rec, 0, sizeof rec);
                mini_net_prefetch_await(src);
                if (mini_net_fetch("GET", src, NULL, NULL, 0, NULL, &rec) == 0 && rec.resp_body)
                {
                    mini_bridge_eval(b, rec.resp_body, rec.resp_body_len, src);
                    mini_net_record_add(&rec);
                }
            }
            else if (is_local_src)
            {
                const char *fpath = src;
                if (!strncmp(fpath, "file:///", 8)) fpath += 8;
                else if (!strncmp(fpath, "file://", 7)) fpath += 7;
                FILE *fp = fopen(fpath, "rb");
                if (fp) {
                    fseek(fp, 0, SEEK_END);
                    long sz = ftell(fp);
                    fseek(fp, 0, SEEK_SET);
                    if (sz > 0) {
                        char *code = (char *)malloc(sz + 1);
                        if (code) {
                            size_t r = fread(code, 1, sz, fp);
                            code[r] = 0;
                            mini_bridge_eval(b, code, r, src);
                            free(code);
                        }
                    }
                    fclose(fp);
                }
            }
            else if (script_text)
            {
                const char *code_src = script_text;
                while (*code_src && isspace((unsigned char)*code_src))
                    code_src++;
                if (!strncmp(code_src, "<!--", 4))
                    code_src += 4;
                size_t len = strlen(code_src);
                while (len > 0 && isspace((unsigned char)code_src[len - 1]))
                    len--;
                if (len >= 3 && !strncmp(code_src + len - 3, "-->", 3))
                    len -= 3;

                mini_bridge_eval(b, code_src, len, "<inline-script>");
            }
        }
    }
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        eval_scripts(c, b);
}

int mini_bridge_load_html(MiniBridge *b, const char *html)
{
    if (!b || !html)
        return -1;
    mini_dom_parse_html(b->doc, html);
    apply_styles(b->doc->root, b);
    eval_scripts(b->doc->root, b);

    /* 核心修复：通过注入 setTimeout 异步 JS 宏来延后派发 DOMContentLoaded！
       这能完美避开 C 宿主先 load_html 后再设置事件系统导致事件被吞的灾难级时序问题，
       确保事件一定在下一帧准确拉起 JS 业务生命周期。 */
    const char *dispatch_script =
        "setTimeout(function() {"
        "  var ev = new Event('DOMContentLoaded', {bubbles: true});"
        "  document.dispatchEvent(ev);"
        "}, 10);";
    mini_bridge_eval(b, dispatch_script, strlen(dispatch_script), "<dom-ready-dispatcher>");

    return 0;
}

int mini_bridge_eval(MiniBridge *b, const char *src, size_t len, const char *filename)
{
    JSValue v = JS_Eval(b->ctx, src, len, filename, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v))
    {
        JSValue ex = JS_GetException(b->ctx);
        const char *s = JS_ToCString(b->ctx, ex);
        JSValue stack = JS_GetPropertyStr(b->ctx, ex, "stack");
        const char *stk = JS_ToCString(b->ctx, stack);
        fprintf(stderr, "[eval error in %s] %s\n%s\n", filename ? filename : "?", s ? s : "?", stk ? stk : "");
        if (stk)
            JS_FreeCString(b->ctx, stk);
        JS_FreeValue(b->ctx, stack);
        JS_FreeCString(b->ctx, s);
        JS_FreeValue(b->ctx, ex);
        JS_FreeValue(b->ctx, v);
        return -1;
    }
    JS_FreeValue(b->ctx, v);
    return 0;
}

/* Set the page URL used as the base for resolving relative module specifiers
   and as import.meta.url for inline module scripts. Local paths are turned
   into file:// URLs (backslashes normalized to forward slashes). */
void mini_bridge_set_doc_url(MiniBridge *b, const char *url)
{
    if (!b)
        return;
    free(b->doc_url);
    b->doc_url = NULL;
    if (!url || !url[0])
        return;
    if (strstr(url, "://")) /* already an absolute URL */
    {
        b->doc_url = strdup(url);
        return;
    }
    size_t n = strlen(url);
    char *buf = (char *)malloc(n + 16);
    if (!buf)
        return;
    strcpy(buf, "file://");
    size_t o = strlen(buf);
    if (url[0] != '/') /* => file:/// for "C:\..." and "rel" */
        buf[o++] = '/';
    for (size_t i = 0; i < n; i++)
    {
        char c = url[i];
        if (c == '\\')
            c = '/';
        buf[o++] = c;
    }
    buf[o] = 0;
    b->doc_url = buf;
}

const char *mini_bridge_get_doc_url(const MiniBridge *b)
{
    return b ? b->doc_url : NULL;
}

/* fire queued rAF + due timers, then drain the JS job queue */
void mini_bridge_pump(MiniBridge *b)
{
    /* 1) microtask / Promise queue */
    for (;;)
    {
        JSContext *pctx = NULL;
        int r = JS_ExecutePendingJob(b->rt, &pctx);
        if (r <= 0)
            break; /* 0 none, -1 exception */
        if (r == -1 && pctx)
        {
            JSValue ex = JS_GetException(pctx);
            const char *msg = JS_ToCString(pctx, ex);
            fprintf(stderr, "[promise error] %s\n", msg ? msg : "exception");
            if (b->log_hook)
                b->log_hook("error", msg ? msg : "promise error", b->log_ud);
            JS_FreeCString(pctx, msg);
            JS_FreeValue(pctx, ex);
        }
    }
    /* 2) due timers (recurring ones re-arm instead of being dropped) */
    double now = glfwGetTime() * 1000.0;
    for (int i = 0; i < b->tm_n;)
    {
        if (b->timers[i].due_ms <= now)
        {
            int recurring = b->timers[i].recurring;
            double interval = b->timers[i].interval_ms;
            JSValue cb;
            if (recurring)
            {
                /* dup: a clearTimeout(id) inside the callback would otherwise
                   free the slot's cb while we still hold a raw pointer */
                cb = JS_DupValue(b->ctx, b->timers[i].cb);
                double iv = interval > 1.0 ? interval : 1.0; /* clamp 0-interval */
                b->timers[i].due_ms = now + iv;
            }
            else
            {
                cb = b->timers[i].cb;                /* take ownership (no dup); slot reused below */
                b->timers[i] = b->timers[--b->tm_n]; /* compact; don't advance i */
            }
            JSValue ret = JS_Call(b->ctx, cb, JS_UNDEFINED, 0, NULL);
            JS_FreeValue(b->ctx, cb);
            if (JS_IsException(ret))
            {
                JSValue ex = JS_GetException(b->ctx);
                const char *msg = JS_ToCString(b->ctx, ex);
                JSValue stack = JS_GetPropertyStr(b->ctx, ex, "stack");
                const char *st = JS_ToCString(b->ctx, stack);
                fprintf(stderr, "[timer error] %s\n%s\n", msg ? msg : "exception", st ? st : "");
                if (b->log_hook)
                {
                    char lbuf[1024];
                    snprintf(lbuf, sizeof lbuf, "%s\n%s", msg ? msg : "timer error", st ? st : "");
                    b->log_hook("error", lbuf, b->log_ud);
                }
                JS_FreeCString(b->ctx, msg);
                JS_FreeCString(b->ctx, st);
                JS_FreeValue(b->ctx, stack);
                JS_FreeValue(b->ctx, ex);
            }
            else
                JS_FreeValue(b->ctx, ret);
            if (recurring)
                i++; /* slot kept; move on so we don't re-fire this frame */
        }
        else
            i++;
    }
    /* 3) service live WebSockets (incoming frames -> onmessage/onclose) */
    bridge_pump_websockets(b);
}

int mini_bridge_fire_raf(MiniBridge *b, double time_ms)
{
    int n = b->raf_n;
    MiniRafEntry *cbs = b->raf;
    b->raf_n = 0;
    b->raf = NULL;
    b->raf_cap = 0; /* detach */
    /* 若本帧确有 rAF 回调，清空上一帧的 2D canvas 命令缓冲，让回调重新记录
       本帧画面（与原"回放即消费"语义等价）。仅在有 rAF 时重置，保证纯静态
       （非 rAF）的一次性绘制不被清掉、得以每帧回放而持久。 */
    if (n > 0)
        mini_2d_reset();
    /* a fresh frame can register new rAF during the callback */
    for (int i = 0; i < n; i++)
    {
        JSValue arg = JS_NewFloat64(b->ctx, time_ms);
        JSValue ret = JS_Call(b->ctx, cbs[i].cb, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(b->ctx, arg);
        JS_FreeValue(b->ctx, cbs[i].cb);
        if (JS_IsException(ret))
        {
            JSValue ex = JS_GetException(b->ctx);
            const char *msg = JS_ToCString(b->ctx, ex);
            JSValue stack = JS_GetPropertyStr(b->ctx, ex, "stack");
            const char *st = JS_ToCString(b->ctx, stack);
            fprintf(stderr, "[rAF error] %s\n%s\n", msg ? msg : "exception", st ? st : "");
            if (b->log_hook)
            {
                char lbuf[1024];
                snprintf(lbuf, sizeof lbuf, "%s\n%s", msg ? msg : "rAF error", st ? st : "");
                b->log_hook("error", lbuf, b->log_ud);
            }
            JS_FreeCString(b->ctx, msg);
            JS_FreeCString(b->ctx, st);
            JS_FreeValue(b->ctx, stack);
            JS_FreeValue(b->ctx, ex);
        }
        else
            JS_FreeValue(b->ctx, ret);
    }
    free(cbs);
    return n;
}

/* Pending rAF count for the host loop's idle gate: >0 means a JS animation
   loop re-registered for the next frame, so the page is actively animating
   and the full render pipeline must run. */
int mini_bridge_pending_raf(MiniBridge *b)
{
    return b ? b->raf_n : 0;
}

/* ----------------------------------------------------------------- */
/* Diagnostics + bytecode entrypoints                                 */
/* ----------------------------------------------------------------- */

/* current QuickJS heap usage (bytes) — drives the diagnostics monitor */
size_t mini_bridge_heap_usage(MiniBridge *b)
{
    JSMemoryUsage mu;
    JS_ComputeMemoryUsage(b->rt, &mu);
    return (size_t)(mu.malloc_size > 0 ? mu.malloc_size : 0);
}

/* Direct JSContext access for the CDP debugger/reflection layer. */
JSContext *mini_bridge_ctx(MiniBridge *b)
{
    return b ? b->ctx : NULL;
}

/* Install a console relay (level,msg) — main.c wires this to CDP. */
void mini_bridge_set_log_hook(MiniBridge *b,
                              void (*hook)(const char *, const char *, void *),
                              void *ud)
{
    b->log_hook = hook;
    b->log_ud = ud;
}

/* Evaluate QuickJS bytecode (.qjc produced by `qjsc`). The serialized
   object is consumed by JS_EvalFunction. Returns 0 on success. */
int mini_vfs_eval_bytecode(MiniBridge *b, const uint8_t *bc, size_t len)
{
    JSValue fun = JS_ReadObject(b->ctx, bc, len, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(fun))
    {
        JSValue ex = JS_GetException(b->ctx);
        const char *s = JS_ToCString(b->ctx, ex);
        fprintf(stderr, "[bytecode read error] %s\n", s ? s : "?");
        JS_FreeCString(b->ctx, s);
        JS_FreeValue(b->ctx, ex);
        return -1;
    }
    JSValue r = JS_EvalFunction(b->ctx, fun); /* consumes fun */
    int ok = JS_IsException(r) ? -1 : 0;
    if (ok != 0)
    {
        JSValue ex = JS_GetException(b->ctx);
        const char *s = JS_ToCString(b->ctx, ex);
        fprintf(stderr, "[bytecode run error] %s\n", s ? s : "?");
        JS_FreeCString(b->ctx, s);
        JS_FreeValue(b->ctx, ex);
    }
    JS_FreeValue(b->ctx, r);
    return ok;
}

/* Evaluate `expr` in the engine and JSON.stringify the result into `out`.
   Used by CDP Runtime.evaluate. The JSON value string is the payload. */
int mini_bridge_eval_to_json(MiniBridge *b, const char *expr, char *out, size_t cap)
{
    char js[16384];
    int n = snprintf(js, sizeof js,
                     "JSON.stringify((function(){return (%s);})())", expr);
    JSValue v = JS_Eval(b->ctx, js, (size_t)n, "<cdp-eval>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v))
    {
        JSValue ex = JS_GetException(b->ctx);
        const char *s = JS_ToCString(b->ctx, ex);
        snprintf(out, cap, "\"%s\"", s ? s : "error");
        JS_FreeCString(b->ctx, s);
        JS_FreeValue(b->ctx, ex);
        JS_FreeValue(b->ctx, v);
        return -1;
    }
    const char *s = JS_ToCString(b->ctx, v);
    snprintf(out, cap, "%s", s ? s : "null");
    JS_FreeCString(b->ctx, s);
    JS_FreeValue(b->ctx, v);
    return 0;
}

struct MiniNode *mini_bridge_node_from_js(MiniBridge *b, JSValueConst val)
{
    if (!b || !JS_IsObject(val))
        return NULL;
    return (struct MiniNode *)JS_GetOpaque(val, b->el_cid);
}

int mini_bridge_is_pointer_locked(MiniBridge *b)
{
    return (b && b->locked_node != NULL);
}

void mini_bridge_unlock_pointer(MiniBridge *b)
{
    if (!b || !b->locked_node)
        return;
    b->locked_node = NULL;
    if (b->r && b->r->gpu.window_handle)
    {
        glfwSetInputMode((GLFWwindow *)b->r->gpu.window_handle, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
    if (b->doc && b->doc->root && b->ev)
    {
        MiniEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.type = "pointerlockchange";
        ev.target = b->doc->root;
        ev.bubbles = 1;
        mini_event_dispatch(b->ev, &ev, b->doc->root);
    }
}
