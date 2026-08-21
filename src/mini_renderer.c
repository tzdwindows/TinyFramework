/*
 * mini_renderer.c �?self-written GPU context + vector command flush.
 *
 * No third-party layout engine, no Chromium/WebKit. Desktop OpenGL 1.1
 * immediate-mode calls are used in this skeleton so the file compiles with
 * only <GL/gl.h> (no glad/glew). Production swaps the flush loop for
 * GL-ES / Vulkan recorded command buffers; the command buffer design above
 * makes that a localized change.
 *
 * The SIMD blend from mini_raster_asm.h is exercised by the glyph cache
 * blitter (CPU-side compositing into the atlas before a single GL upload),
 * which is where a software composite path earns its keep.
 */
#include "mini_renderer.h"
#include "mini_dom.h"
#include "mini_png.h"
#include "mini_raster_asm.h"

#include <GLFW/glfw3.h> /* platform window + GL context */
#include <GL/gl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

/* On Windows, declare the process-DPI-awareness API ourselves so we do not
   need windows.h (which would clash with the GL headers via its macros) and
   do not require a newer SDK at link time. Making the process DPI-aware
   before the window is created stops Windows DWM from bitmap-scaling the
   whole window on high-DPI displays — the single biggest cause of an
   "everything is soft/blurry" window on Win10/11 laptops. */
#ifdef _WIN32
#define WINAPI __stdcall
typedef int(WINAPI *PFN_SetProcessDpiAwarenessContext)(int);
typedef int(WINAPI *PFN_SetProcessDpiAwareness)(int);
typedef int(WINAPI *PFN_SetProcessDPIAware)(void);
__declspec(dllimport) void *__stdcall GetModuleHandleA(const char *);
__declspec(dllimport) void *__stdcall GetProcAddress(void *, const char *);
#endif

static void mini_set_dpi_aware(void)
{
#ifdef _WIN32
    void *user32 = GetModuleHandleA("user32.dll");
    if (user32)
    {
        /* Win10 1703+: per-monitor v2 awareness — crisp on every monitor in
           a mixed-DPI setup, and the GL framebuffer comes back at device
           resolution so the viewport/ortho (already framebuffer-sized) land
           pixel-exact. Context value -4 = PER_MONITOR_AWARE_V2.            */
        PFN_SetProcessDpiAwarenessContext set_ctx =
            (PFN_SetProcessDpiAwarenessContext)GetProcAddress(user32,
                                                              "SetProcessDpiAwarenessContext");
        if (set_ctx && set_ctx(-4))
            return;
        /* Win8.1+: per-monitor v1. value 2 = PROCESS_PER_MONITOR_DPI_AWARE. */
        PFN_SetProcessDpiAwareness set_aware =
            (PFN_SetProcessDpiAwareness)GetProcAddress(user32,
                                                       "SetProcessDpiAwareness");
        if (set_aware && set_aware(2))
            return;
        /* Vista+: system-DPI aware — the last-resort fallback that still
           prevents DWM bitmap scaling of the window. */
        PFN_SetProcessDPIAware sys =
            (PFN_SetProcessDPIAware)GetProcAddress(user32, "SetProcessDPIAware");
        if (sys)
            sys();
    }
#endif
}

/* mingw's <GL/gl.h> is the legacy 1.1 header �?define the GL 1.5+ types the
   Stage-4 shader/FBO foundation references (GLchar). */
#ifndef GLchar
typedef char GLchar;
#endif
#ifndef GLsizeiptr
typedef ptrdiff_t GLsizeiptr;
#endif
#ifndef GLintptr
typedef ptrdiff_t GLintptr;
#endif

#define KAWASE_MAX_LEVELS 6

/* stb_truetype: cross-platform single-header TrueType rasterizer. Used to
 * render the loaded TTF (e.g. AiDianFengYaHeiChangTi) into the shared atlas
 * so text is anti-aliased and supports emoji (�? + CJK, replacing the
 * built-in 5x7 bitmap font when a TTF is loaded. */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#ifndef PI
#define PI 3.14159265359f
#endif

/* GL 1.2 clamp constant; the mingw opengl32 headers do not always expose
   it even on a 2.1 context. Needed for NPOT gradient textures (GL_CLAMP
   border sampling is disallowed for NPOT in core GL 2.0+).               */
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

/* GL 1.3 multisample token; the mingw 1.1 header may omit it. Set up once
   at context creation (persistent GL state) rather than every frame. */
#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif

/* ------------------------------------------------------------------ */
/* Stage 4 GL foundation: GLSL programs + FBO entrypoints resolved at  */
/* runtime via glfwGetProcAddress (opengl32 only statically exports    */
/* GL 1.1). A flat-color program is compiled + link-verified at init   */
/* so the shader path is real; it is deliberately NOT wired into the    */
/* DOM render yet (the fixed-function immediate-mode path stays the     */
/* workhorse) to avoid clobbering the WebGL bridge's bound program.     */
/* The shader + FBO pointers are the substrate the trail-fade /        */
/* backdrop-blur / 3D-MVP consumers build on.                          */
/* ------------------------------------------------------------------ */
typedef GLuint (*PFNGL_m_CreateShader)(GLenum);
typedef void (*PFNGL_m_ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void (*PFNGL_m_CompileShader)(GLuint);
typedef GLuint (*PFNGL_m_CreateProgram)(void);
typedef void (*PFNGL_m_AttachShader)(GLuint, GLuint);
typedef void (*PFNGL_m_LinkProgram)(GLuint);
typedef void (*PFNGL_m_UseProgram)(GLuint);
typedef GLint (*PFNGL_m_GetUniformLocation)(GLuint, const GLchar *);
typedef void (*PFNGL_m_Uniform4f)(GLint, float, float, float, float);
typedef void (*PFNGL_m_GetShaderiv)(GLuint, GLenum, GLint *);
typedef void (*PFNGL_m_GetProgramiv)(GLuint, GLenum, GLint *);
typedef void (*PFNGL_m_GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
typedef void (*PFNGL_m_GenFramebuffers)(GLsizei, GLuint *);
typedef void (*PFNGL_m_BindFramebuffer)(GLenum, GLuint);
typedef void (*PFNGL_m_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (*PFNGL_m_CheckFramebufferStatus)(GLenum);
typedef void (*PFNGL_m_DeleteFramebuffers)(GLsizei, const GLuint *);
typedef void (*PFNGL_m_ActiveTexture)(GLenum);
typedef void (*PFNGL_m_DisableVertexAttribArray)(GLuint);
typedef void (*PFNGL_m_BindBuffer)(GLenum, GLuint);
typedef void (*PFNGL_m_BlendEquation)(GLenum);
typedef void (*PFNGL_m_BindVertexArray)(GLuint);

static unsigned int utf8_next(const char **p, int *len);
static void draw_rounded_corners_poly_tex(float x, float y, float w, float h, const float r[4], float du, float dv);
static void draw_rounded_corners_poly_tex_outset(float x, float y, float w, float h, const float r[4],
                                                 float vx, float vy, float vw, float vh, float vbuf_h, const GLfloat m[16]);
static GLuint run_fast_gaussian_blur(GLuint src_tex, int src_w, int src_h, float blur_radius);
static void blur_pool_ensure(int w, int h);

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif

static struct
{
    PFNGL_m_CreateShader CreateShader;
    PFNGL_m_ShaderSource ShaderSource;
    PFNGL_m_CompileShader CompileShader;
    PFNGL_m_CreateProgram CreateProgram;
    PFNGL_m_AttachShader AttachShader;
    PFNGL_m_LinkProgram LinkProgram;
    PFNGL_m_UseProgram UseProgram;
    PFNGL_m_GetUniformLocation GetUniformLocation;
    PFNGL_m_Uniform4f Uniform4f;
    PFNGL_m_GetShaderiv GetShaderiv;
    PFNGL_m_GetProgramiv GetProgramiv;
    PFNGL_m_GetShaderInfoLog GetShaderInfoLog;
    PFNGL_m_GenFramebuffers GenFramebuffers;
    PFNGL_m_BindFramebuffer BindFramebuffer;
    PFNGL_m_FramebufferTexture2D FramebufferTexture2D;
    PFNGL_m_CheckFramebufferStatus CheckFramebufferStatus;
    PFNGL_m_DeleteFramebuffers DeleteFramebuffers;
    PFNGL_m_ActiveTexture ActiveTexture;
    PFNGL_m_DisableVertexAttribArray DisableVertexAttribArray;
    PFNGL_m_BindBuffer BindBuffer;
    PFNGL_m_BlendEquation BlendEquation;
    PFNGL_m_BindVertexArray BindVertexArray;
    int have_shaders;
    int have_fbo;
    GLuint flat_prog; /* compiled+linked flat-color program (foundation) */
    GLint flat_color_loc;
} g_gl;

static struct
{
    GLuint src_tex;
    int src_w, src_h;
    GLuint fbo[3];
    GLuint tex[3];
    int w[3], h[3];
    GLuint noise_tex;
    int inited;
} g_blur_pool;

static GLuint compile_sh_stage4(GLenum type, const char *src)
{
    GLuint s = g_gl.CreateShader(type);
    if (!s)
        return 0;
    g_gl.ShaderSource(s, 1, &src, NULL);
    g_gl.CompileShader(s);
    GLint ok = 0;
    g_gl.GetShaderiv(s, 0x8B81 /*COMPILE_STATUS*/, &ok);
    if (!ok)
    {
        char log[1024] = {0};
        GLsizei lr = 0;
        g_gl.GetShaderInfoLog(s, sizeof log - 1, &lr, log);
        fprintf(stderr, "[gl] shader(%u) COMPILE FAILED: %s\n", (unsigned)s, log);
        return 0;
    }
    return s;
}

static void mini_gl_init_foundation(void)
{
#define G(field, name) g_gl.field = (PFNGL_m_##field)glfwGetProcAddress(name)
    G(CreateShader, "glCreateShader");
    G(ShaderSource, "glShaderSource");
    G(CompileShader, "glCompileShader");
    G(CreateProgram, "glCreateProgram");
    G(AttachShader, "glAttachShader");
    G(LinkProgram, "glLinkProgram");
    G(UseProgram, "glUseProgram");
    G(GetUniformLocation, "glGetUniformLocation");
    G(Uniform4f, "glUniform4f");
    G(GetShaderiv, "glGetShaderiv");
    G(GetProgramiv, "glGetProgramiv");
    G(GetShaderInfoLog, "glGetShaderInfoLog");
    G(GenFramebuffers, "glGenFramebuffers");
    G(BindFramebuffer, "glBindFramebuffer");
    G(FramebufferTexture2D, "glFramebufferTexture2D");
    G(CheckFramebufferStatus, "glCheckFramebufferStatus");
    G(DeleteFramebuffers, "glDeleteFramebuffers");
    G(ActiveTexture, "glActiveTexture");
    G(DisableVertexAttribArray, "glDisableVertexAttribArray");
    G(BindBuffer, "glBindBuffer");
    G(BlendEquation, "glBlendEquation");
    G(BindVertexArray, "glBindVertexArray");
#undef G
    g_gl.have_shaders = g_gl.CreateShader && g_gl.ShaderSource && g_gl.CompileShader &&
                        g_gl.CreateProgram && g_gl.AttachShader && g_gl.LinkProgram &&
                        g_gl.UseProgram && g_gl.GetUniformLocation && g_gl.Uniform4f &&
                        g_gl.GetShaderiv && g_gl.GetProgramiv;
    g_gl.have_fbo = g_gl.GenFramebuffers && g_gl.BindFramebuffer &&
                    g_gl.FramebufferTexture2D && g_gl.CheckFramebufferStatus;
    if (g_gl.have_shaders)
    {
        static const char *vsrc =
            "#version 120\n"
            "void main(){ gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; }\n";
        static const char *fsrc =
            "#version 120\n"
            "uniform vec4 u_color;\n"
            "void main(){ gl_FragColor = u_color; }\n";
        GLuint vs = compile_sh_stage4(0x8B31 /*VERTEX_SHADER*/, vsrc);
        GLuint fs = compile_sh_stage4(0x8B30 /*FRAGMENT_SHADER*/, fsrc);
        if (vs && fs)
        {
            g_gl.flat_prog = g_gl.CreateProgram();
            g_gl.AttachShader(g_gl.flat_prog, vs);
            g_gl.AttachShader(g_gl.flat_prog, fs);
            g_gl.LinkProgram(g_gl.flat_prog);
            GLint ok = 0;
            g_gl.GetProgramiv(g_gl.flat_prog, 0x8B82 /*LINK_STATUS*/, &ok);
            if (ok && g_gl.GetUniformLocation)
                g_gl.flat_color_loc = g_gl.GetUniformLocation(g_gl.flat_prog, "u_color");
            fprintf(stderr, ok ? "[gl] Stage-4 flat-color program linked (%u)\n" : "[gl] flat-color program LINK FAILED\n",
                    (unsigned)g_gl.flat_prog);
        }
    }
    fprintf(stderr, "[gl] foundation: shaders=%d fbo=%d\n",
            g_gl.have_shaders, g_gl.have_fbo);
}

#ifndef GL_SHADING_LANGUAGE_VERSION
#define GL_SHADING_LANGUAGE_VERSION 0x8B8B
#endif

/* ------------------------------------------------------------------ */
/* GL state cache (bridge.c owns the modern-GL pointers; the legacy   */
/* immediate-mode path here uses viewport/clear/scissor/texture 2D,  */
/* plus the bridge reset that restores program 0 each frame).         */
/* ------------------------------------------------------------------ */
void *mini_gl_bridge_new(void); /* defined in mini_js_bridge.c    */
void mini_gl_bridge_destroy(void *);
/* Drop the shader program + buffer/texture bindings the WebGL path
   leaks, restoring the fixed-function 2D pipeline. See mini_js_bridge.c. */
void mini_gl_bridge_reset_2d(void *state);
/* Re-bind the WebGL shader program the JS selected (saved by reset_2d).
   Must be called just before mini_bridge_fire_raf so that rAF drawArrays
   runs under the correct GLSL shader rather than program 0. */
void mini_gl_bridge_restore_webgl(void *state);

/* Glyph-run side buffer: a GLYPH_RUN command can't carry a variable
   number of quads in the fixed-size MiniCmd, so quads live here and the
   command indexes this store (reserved=start, glyph_count=n). One
   renderer owns it; reset each frame in begin_frame. Static is fine for
   the single-window app. */
/* Glyph-run and text side-buffers are now encapsulated in MiniRenderer struct. */

/* ================================================================== */
static MiniRenderer *g_active_renderer = NULL;
/* Built-in 5x7 bitmap font (ASCII 0x20..0x7E).                         */
/* The renderer has no font-file dependency: every glyph is authored    */
/* directly as seven 5-pixel rows. This is what lets the DOM render     */
/* actual text content AND readable placeholder labels (IMG, VIDEO,     */
/* INPUT, ...) without an external TTF/OTF + stb_truetype rasterizer.   */
/* Unknown glyphs (anything off the printable range, or a row left      */
/* blank for a rare symbol) fall back to a filled cell so text always    */
/* shows something �?the explicit "placeholder" policy.                  */
/* Each entry below is 7 adjacent 5-char rows; '#' = pixel on.          */
/* ================================================================== */
static const char *const MINI_FONT_SRC[95] = {
    /* 0x20 ' ' */ "....."
                   "....."
                   "....."
                   "....."
                   "....."
                   "....."
                   ".....",
    /* 0x21 '!' */ "..#.."
                   "..#.."
                   "..#.."
                   "..#.."
                   "....."
                   "..#.."
                   "..#..",
    /* 0x22 '"' */ ".#.#."
                   ".#.#."
                   "....."
                   "....."
                   "....."
                   "....."
                   ".....",
    /* 0x23 '#' */ ".#.#."
                   ".#.#."
                   "#####"
                   ".#.#."
                   "#####"
                   ".#.#."
                   ".#.#.",
    /* 0x24 '$' */ "..#.."
                   ".####"
                   "#...#"
                   ".###."
                   "#...#"
                   "####."
                   "..#..",
    /* 0x25 '%' */ "#...#"
                   "#..#."
                   ".#..."
                   "..#.."
                   "...#."
                   ".#..#"
                   "#...#",
    /* 0x26 '&' */ ".##.."
                   "#..#."
                   ".#..."
                   ".##.."
                   "#..#."
                   "#..#."
                   ".###.",
    /* 0x27 '\'*/ "..#.."
                  "..#.."
                  "....."
                  "....."
                  "....."
                  "....."
                  ".....",
    /* 0x28 '(' */ "..#.."
                   ".#..."
                   "#...."
                   "#...."
                   "#...."
                   ".#..."
                   "..#..",
    /* 0x29 ')' */ "..#.."
                   "...#."
                   "....#"
                   "....#"
                   "....#"
                   "...#."
                   "..#..",
    /* 0x2A '*' */ "....."
                   "..#.."
                   "#.#.#"
                   "#####"
                   "#.#.#"
                   "..#.."
                   ".....",
    /* 0x2B '+' */ "....."
                   "..#.."
                   "..#.."
                   "#####"
                   "..#.."
                   "..#.."
                   ".....",
    /* 0x2C ',' */ "....."
                   "....."
                   "....."
                   "....."
                   "..#.."
                   "..#.."
                   ".#...",
    /* 0x2D '-' */ "....."
                   "....."
                   "....."
                   "#####"
                   "....."
                   "....."
                   ".....",
    /* 0x2E '.' */ "....."
                   "....."
                   "....."
                   "....."
                   "....."
                   "..#.."
                   "..#..",
    /* 0x2F '/' */ "....#"
                   "...#."
                   "...#."
                   "..#.."
                   ".#..."
                   ".#..."
                   "#....",
    /* 0x30 '0' */ ".###."
                   "#...#"
                   "#..##"
                   "#.#.#"
                   "##..#"
                   "#...#"
                   ".###.",
    /* 0x31 '1' */ "..#.."
                   ".##.."
                   "..#.."
                   "..#.."
                   "..#.."
                   "..#.."
                   ".###.",
    /* 0x32 '2' */ ".###."
                   "#...#"
                   "....#"
                   "..#.."
                   ".#..."
                   "#...."
                   "#####",
    /* 0x33 '3' */ "#####"
                   "...#."
                   "..#.."
                   "...#."
                   "....#"
                   "#...#"
                   ".###.",
    /* 0x34 '4' */ "...#."
                   "..##."
                   ".#.#."
                   "#..#."
                   "#####"
                   "...#."
                   "...#.",
    /* 0x35 '5' */ "#####"
                   "#...."
                   "####."
                   "....#"
                   "....#"
                   "#...#"
                   ".###.",
    /* 0x36 '6' */ "..#.."
                   ".#..."
                   "#...."
                   "####."
                   "#...#"
                   "#...#"
                   ".###.",
    /* 0x37 '7' */ "#####"
                   "....#"
                   "...#."
                   "..#.."
                   ".#..."
                   ".#..."
                   ".#...",
    /* 0x38 '8' */ ".###."
                   "#...#"
                   "#...#"
                   ".###."
                   "#...#"
                   "#...#"
                   ".###.",
    /* 0x39 '9' */ ".###."
                   "#...#"
                   "#...#"
                   ".####"
                   "....#"
                   "...#."
                   ".##..",
    /* 0x3A ':' */ "....."
                   "..#.."
                   "....."
                   "....."
                   "....."
                   "..#.."
                   ".....",
    /* 0x3B ';' */ "....."
                   "..#.."
                   "....."
                   "....."
                   "..#.."
                   "..#.."
                   ".#...",
    /* 0x3C '<' */ "...#."
                   "..#.."
                   ".#..."
                   "#...."
                   ".#..."
                   "..#.."
                   "...#.",
    /* 0x3D '=' */ "....."
                   "....."
                   "#####"
                   "....."
                   "#####"
                   "....."
                   ".....",
    /* 0x3E '>' */ "#...."
                   ".#..."
                   "..#.."
                   "...#."
                   "..#.."
                   ".#..."
                   "#....",
    /* 0x3F '?' */ ".###."
                   "#...#"
                   "....#"
                   "...#."
                   "..#.."
                   "....."
                   "..#..",
    /* 0x40 '@' */ ".###."
                   "#...#"
                   "#.###"
                   "#.#.#"
                   "#.###"
                   "#...."
                   ".###.",
    /* 0x41 'A' */ "..#.."
                   ".#.#."
                   "#...#"
                   "#...#"
                   "#####"
                   "#...#"
                   "#...#",
    /* 0x42 'B' */ "####."
                   "#...#"
                   "#...#"
                   "####."
                   "#...#"
                   "#...#"
                   "####.",
    /* 0x43 'C' */ ".###."
                   "#...#"
                   "#...."
                   "#...."
                   "#...."
                   "#...#"
                   ".###.",
    /* 0x44 'D' */ "###.."
                   "#..#."
                   "#...#"
                   "#...#"
                   "#...#"
                   "#..#."
                   "###..",
    /* 0x45 'E' */ "#####"
                   "#...."
                   "#...."
                   "####."
                   "#...."
                   "#...."
                   "#####",
    /* 0x46 'F' */ "#####"
                   "#...."
                   "#...."
                   "####."
                   "#...."
                   "#...."
                   "#....",
    /* 0x47 'G' */ ".###."
                   "#...#"
                   "#...."
                   "#.###"
                   "#...#"
                   "#...#"
                   ".###.",
    /* 0x48 'H' */ "#...#"
                   "#...#"
                   "#...#"
                   "#####"
                   "#...#"
                   "#...#"
                   "#...#",
    /* 0x49 'I' */ ".###."
                   "..#.."
                   "..#.."
                   "..#.."
                   "..#.."
                   "..#.."
                   ".###.",
    /* 0x4A 'J' */ "..###"
                   "...#."
                   "...#."
                   "...#."
                   "#..#."
                   "#...#"
                   ".###.",
    /* 0x4B 'K' */ "#...#"
                   "#..#."
                   "#.#.."
                   "##..."
                   "#.#.."
                   "#..#."
                   "#...#",
    /* 0x4C 'L' */ "#...."
                   "#...."
                   "#...."
                   "#...."
                   "#...."
                   "#...."
                   "#####",
    /* 0x4D 'M' */ "#...#"
                   "##.##"
                   "#.#.#"
                   "#...#"
                   "#...#"
                   "#...#"
                   "#...#",
    /* 0x4E 'N' */ "#...#"
                   "##..#"
                   "#.#.#"
                   "#.#.#"
                   "#..##"
                   "#...#"
                   "#...#",
    /* 0x4F 'O' */ ".###."
                   "#...#"
                   "#...#"
                   "#...#"
                   "#...#"
                   "#...#"
                   ".###.",
    /* 0x50 'P' */ "####."
                   "#...#"
                   "#...#"
                   "####."
                   "#...."
                   "#...."
                   "#....",
    /* 0x51 'Q' */ ".###."
                   "#...#"
                   "#...#"
                   "#...#"
                   "#.#.#"
                   "#..#."
                   ".##.#",
    /* 0x52 'R' */ "####."
                   "#...#"
                   "#...#"
                   "####."
                   "#.#.."
                   "#..#."
                   "#...#",
    /* 0x53 'S' */ ".###."
                   "#...#"
                   "#...."
                   ".###."
                   "....#"
                   "#...#"
                   ".###.",
    /* 0x54 'T' */ "#####"
                   "..#.."
                   "..#.."
                   "..#.."
                   "..#.."
                   "..#.."
                   "..#..",
    /* 0x55 'U' */ "#...#"
                   "#...#"
                   "#...#"
                   "#...#"
                   "#...#"
                   "#...#"
                   ".###.",
    /* 0x56 'V' */ "#...#"
                   "#...#"
                   "#...#"
                   "#...#"
                   "#...#"
                   ".#.#."
                   "..#..",
    /* 0x57 'W' */ "#...#"
                   "#...#"
                   "#...#"
                   "#.#.#"
                   "#.#.#"
                   "##.##"
                   "#...#",
    /* 0x58 'X' */ "#...#"
                   "#...#"
                   ".#.#."
                   "..#.."
                   ".#.#."
                   "#...#"
                   "#...#",
    /* 0x59 'Y' */ "#...#"
                   "#...#"
                   ".#.#."
                   "..#.."
                   "..#.."
                   "..#.."
                   "..#..",
    /* 0x5A 'Z' */ "#####"
                   "....#"
                   "...#."
                   "..#.."
                   ".#..."
                   "#...."
                   "#####",
    /* 0x5B '[' */ ".###."
                   ".#..."
                   ".#..."
                   ".#..."
                   ".#..."
                   ".#..."
                   ".###.",
    /* 0x5C '\'*/ "#...."
                  ".#..."
                  ".#..."
                  ".#..."
                  ".#..."
                  ".#..."
                  "#....",
    /* 0x5D ']' */ ".###."
                   "...#."
                   "...#."
                   "...#."
                   "...#."
                   "...#."
                   ".###.",
    /* 0x5E '^' */ "..#.."
                   ".#.#."
                   "#...#"
                   "....."
                   "....."
                   "....."
                   ".....",
    /* 0x5F '_' */ "....."
                   "....."
                   "....."
                   "....."
                   "....."
                   "....."
                   "#####",
    /* 0x60 '`' */ ".#..."
                   "..#.."
                   "....."
                   "....."
                   "....."
                   "....."
                   ".....",
    /* 0x61 'a' */ "....."
                   "....."
                   ".###."
                   "....#"
                   ".####"
                   "#...#"
                   ".####",
    /* 0x62 'b' */ "#...."
                   "#...."
                   "####."
                   "#...#"
                   "#...#"
                   "#...#"
                   "####.",
    /* 0x63 'c' */ "....."
                   "....."
                   ".###."
                   "#...#"
                   "#...."
                   "#...#"
                   ".###.",
    /* 0x64 'd' */ "....#"
                   "....#"
                   ".####"
                   "#...#"
                   "#...#"
                   "#...#"
                   ".####",
    /* 0x65 'e' */ "....."
                   "....."
                   ".###."
                   "#...#"
                   "#####"
                   "#...."
                   ".###.",
    /* 0x66 'f' */ "..##."
                   ".#..#"
                   ".#..."
                   "####."
                   ".#..."
                   ".#..."
                   ".#...",
    /* 0x67 'g' */ "....."
                   ".####"
                   "#...#"
                   "#...#"
                   ".####"
                   "....#"
                   ".###.",
    /* 0x68 'h' */ "#...."
                   "#...."
                   "####."
                   "#...#"
                   "#...#"
                   "#...#"
                   "#...#",
    /* 0x69 'i' */ "..#.."
                   "....."
                   ".##.."
                   "..#.."
                   "..#.."
                   "..#.."
                   ".###.",
    /* 0x6A 'j' */ "...#."
                   "....."
                   "..##."
                   "...#."
                   "...#."
                   "#..#."
                   ".##..",
    /* 0x6B 'k' */ "#...."
                   "#...."
                   "#..#."
                   "#.#.."
                   "##..."
                   "#.#.."
                   "#..#.",
    /* 0x6C 'l' */ ".##.."
                   ".#..."
                   ".#..."
                   ".#..."
                   ".#..."
                   ".#..."
                   ".##..",
    /* 0x6D 'm' */ "....."
                   "....."
                   "##.#."
                   "#.#.#"
                   "#.#.#"
                   "#...#"
                   "#...#",
    /* 0x6E 'n' */ "....."
                   "....."
                   "####."
                   "#...#"
                   "#...#"
                   "#...#"
                   "#...#",
    /* 0x6F 'o' */ "....."
                   "....."
                   ".###."
                   "#...#"
                   "#...#"
                   "#...#"
                   ".###.",
    /* 0x70 'p' */ "....."
                   "####."
                   "#...#"
                   "#...#"
                   "####."
                   "#...."
                   "#....",
    /* 0x71 'q' */ "....."
                   ".####"
                   "#...#"
                   "#...#"
                   ".####"
                   "....#"
                   "....#",
    /* 0x72 'r' */ "....."
                   "....."
                   "#.##."
                   "##..#"
                   "#...."
                   "#...."
                   "#....",
    /* 0x73 's' */ "....."
                   ".###."
                   "#...#"
                   ".###."
                   "....#"
                   "#...#"
                   ".###.",
    /* 0x74 't' */ ".#..."
                   ".#..."
                   "####."
                   ".#..."
                   ".#..."
                   ".#..#"
                   "..##.",
    /* 0x75 'u' */ "....."
                   "....."
                   "#...#"
                   "#...#"
                   "#...#"
                   "#...#"
                   ".####",
    /* 0x76 'v' */ "....."
                   "....."
                   "#...#"
                   "#...#"
                   "#...#"
                   ".#.#."
                   "..#..",
    /* 0x77 'w' */ "....."
                   "....."
                   "#...#"
                   "#...#"
                   "#.#.#"
                   "#.#.#"
                   "#...#",
    /* 0x78 'x' */ "....."
                   "....."
                   "#...#"
                   ".#.#."
                   "..#.."
                   ".#.#."
                   "#...#",
    /* 0x79 'y' */ "....."
                   "#...#"
                   "#...#"
                   "#...#"
                   ".####"
                   "....#"
                   ".###.",
    /* 0x7A 'z' */ "....."
                   "....."
                   "#####"
                   "...#."
                   "..#.."
                   ".#..."
                   "#####",
    /* 0x7B '{' */ "..##."
                   ".#..."
                   ".#..."
                   "#...."
                   ".#..."
                   ".#..."
                   "..##.",
    /* 0x7C '|' */ "..#.."
                   "..#.."
                   "..#.."
                   "..#.."
                   "..#.."
                   "..#.."
                   "..#..",
    /* 0x7D '}' */ ".##.."
                   "...#."
                   "...#."
                   "....#"
                   "...#."
                   "...#."
                   ".##..",
    /* 0x7E '~' */ "....."
                   ".#.#."
                   "#.#.#"
                   ".#.#."
                   "....."
                   "....."
                   "....."};

/* Compiled bit tables: g_font[c][row] = 5-bit row pattern (bit0=left). */
static uint8_t g_font[128][7];
static int g_font_init = 0;

static void font_init(void)
{
    if (g_font_init)
        return;
    g_font_init = 1;
    for (int i = 0; i < 95; i++)
    {
        int c = 0x20 + i;
        const char *g = MINI_FONT_SRC[i];
        for (int r = 0; r < 7; r++)
        {
            uint8_t v = 0;
            for (int col = 0; col < 5; col++)
            {
                char ch = g[r * 5 + col];
                if (ch == '#')
                    v |= (uint8_t)(1u << col);
            }
            g_font[c][r] = v;
        }
    }
}

/* one glyph cell, in pixels, at a given font size. The font is 5 wide x
   7 tall; we add a 1px-equivalent gap so letters don't touch.          */
#define FONT_PX(fs) ((fs) / 7.5f)         /* size of one font pixel */
#define FONT_GW(fs) (5.0f * FONT_PX(fs))  /* glyph cell width       */
#define FONT_GH(fs) (7.0f * FONT_PX(fs))  /* glyph cell height = fs */
#define FONT_ADV(fs) (4.5f * FONT_PX(fs)) /* advance incl. 1px gap  */

/* ================================================================== */
/* TTF glyph cache (stb_truetype).                                     */
/* A glyph is rasterized once per (codepoint, pixel-size bucket) and   */
/* stored in the shared 1024x1024 atlas; its UV box + metrics are kept */
/* in a small hash-ish table so subsequent frames just emit quads.     */
/* The cache lives in the renderer (lifetime = renderer lifetime); the */
/* atlas cursor is a simple linear allocator (wraps to 0 on overflow).*/
/* ================================================================== */

/* TtGlyph defined in mini_renderer.h */

/* One renderer holds a single active TTF; the cache is renderer-global
   for simplicity (single-threaded engine). */
/* g_tt_cache is now inside MiniRenderer struct */

/* The renderer whose TTF is currently "active" for the cache helpers.
   Set by mini_renderer_load_font and reset on destroy. The text path
   reads it via the renderer passed in (it carries font_ctx + ttf_data). */

static int tt_size_bucket(float font_size)
{
    /* bucket to the nearest 0.5px so 16.0 and 16.1 share a raster. */
    int b = (int)(font_size * 2.0f + 0.5f);
    if (b < 1)
        b = 1;
    if (b > TT_SIZE_BUCKETS * 64)
        b = TT_SIZE_BUCKETS * 64;
    return b;
}

static float tt_bucket_to_size(int bucket)
{
    return (float)bucket / 2.0f;
}

/* find-or-insert a glyph cache entry for (cp, size). On miss rasterizes
   the glyph into the atlas and records its UV box. Returns NULL only on
   hard failure (no TTF loaded / rasterize error). */
static TtGlyph *tt_get_glyph(MiniRenderer *r, unsigned int cp, float font_size)
{
    if (!r || !r->ttf_loaded || r->num_fonts <= 0)
        return NULL;
    int bucket = tt_size_bucket(font_size);

    /* Open-addressing hash map lookup */
    uint32_t hash = ((cp * 2654435761u) ^ (bucket * 1013)) & (TT_GLYPH_CAP - 1);
    int found_idx = -1;
    int first_empty = -1;

    for (int step = 0; step < TT_GLYPH_CAP; step++)
    {
        int idx = (int)((hash + (uint32_t)step) & (TT_GLYPH_CAP - 1));
        TtGlyph *g = &r->tt_cache[idx];
        if (!g->in_use)
        {
            if (first_empty < 0)
                first_empty = idx;
            break;
        }
        if (g->cp == cp && g->size_bucket == (uint16_t)bucket)
        {
            found_idx = idx;
            break;
        }
    }

    if (found_idx >= 0)
        return &r->tt_cache[found_idx];

    float fs = tt_bucket_to_size(bucket);
    stbtt_fontinfo *chosen_info = NULL;
    float scale = 0.0f;
    int gw = 0, gh = 0, xoff = 0, yoff = 0;
    unsigned char *bmp = NULL;

    /* Search through all loaded fonts in fallback order */
    for (int fi = 0; fi < r->num_fonts; fi++)
    {
        if (!r->fonts[fi].loaded || !r->fonts[fi].font_ctx)
            continue;
        stbtt_fontinfo *info = (stbtt_fontinfo *)r->fonts[fi].font_ctx;
        int glyph_idx = stbtt_FindGlyphIndex(info, (int)cp);
        if (glyph_idx > 0)
        {
            float s = stbtt_ScaleForPixelHeight(info, fs);
            if (s > 0.0f)
            {
                int tg_w = 0, tg_h = 0, tg_xo = 0, tg_yo = 0;
                unsigned char *tb = stbtt_GetCodepointBitmap(info, 0.0f, s, (int)cp, &tg_w, &tg_h, &tg_xo, &tg_yo);
                if (tb)
                {
                    chosen_info = info;
                    scale = s;
                    bmp = tb;
                    gw = tg_w;
                    gh = tg_h;
                    xoff = tg_xo;
                    yoff = tg_yo;
                    break;
                }
            }
        }
    }

    if (!chosen_info && r->num_fonts > 0 && r->fonts[0].font_ctx)
    {
        chosen_info = (stbtt_fontinfo *)r->fonts[0].font_ctx;
        scale = stbtt_ScaleForPixelHeight(chosen_info, fs);
    }

    if (!bmp)
    {
        if (first_empty < 0)
            return NULL;
        TtGlyph *g = &r->tt_cache[first_empty];
        g->cp = cp;
        g->size_bucket = (uint16_t)bucket;
        g->in_use = 1;
        g->atlas_x = g->atlas_y = 0;
        g->gw = g->gh = 0;
        g->xoff = g->yoff = 0;
        int advance = 0, lsb = 0;
        if (chosen_info && scale > 0.0f)
            stbtt_GetCodepointHMetrics(chosen_info, (int)cp, &advance, &lsb);
        g->advance = (scale > 0.0f && advance > 0) ? scale * (float)advance : FONT_ADV(fs);
        return g;
    }

    int cell_w = gw + TT_ATLAS_PAD * 2;
    int cell_h = gh + TT_ATLAS_PAD * 2;

    if (r->tt_atlas_x + cell_w > r->atlas_w)
    {
        r->tt_atlas_x = 0;
        r->tt_atlas_y += r->tt_atlas_row_h + TT_ATLAS_PAD;
        r->tt_atlas_row_h = 0;
    }

    /* 如果图集需要被抹除，先执行 Flush 把已压入并绑定旧 UV 坐标的命令给绘制完毕！ */
    if (first_empty < 0 || r->tt_atlas_y + cell_h > r->atlas_h)
    {
        mini_renderer_flush(r);
        memset(r->tt_cache, 0, sizeof(r->tt_cache));
        r->tt_atlas_x = 0;
        r->tt_atlas_y = 0;
        r->tt_atlas_row_h = 0;
        if (r->atlas_cpu)
            memset(r->atlas_cpu, 0, (size_t)r->atlas_w * (size_t)r->atlas_h * sizeof(uint32_t));
        first_empty = (int)(hash & (TT_GLYPH_CAP - 1));
    }

    int ax = r->tt_atlas_x + TT_ATLAS_PAD;
    int ay = r->tt_atlas_y + TT_ATLAS_PAD;
    r->tt_atlas_x += cell_w;
    if (cell_h > r->tt_atlas_row_h)
        r->tt_atlas_row_h = cell_h;

    if (gw > 0 && gh > 0)
    {
        uint32_t *rgba = (uint32_t *)malloc((size_t)gw * gh * sizeof(uint32_t));
        if (rgba)
        {
            for (int i = 0; i < gw * gh; i++)
            {
                unsigned char a = bmp[i];
                rgba[i] = ((uint32_t)a << 24) | 0x00FFFFFFu;
            }
            mini_atlas_blit_glyph(r, ax, ay, gw, gh, rgba);
            free(rgba);
        }
    }
    stbtt_FreeBitmap(bmp, NULL);

    int advance = 0, lsb = 0;
    if (chosen_info)
        stbtt_GetCodepointHMetrics(chosen_info, (int)cp, &advance, &lsb);

    TtGlyph *g = &r->tt_cache[first_empty];
    g->cp = cp;
    g->size_bucket = (uint16_t)bucket;
    g->in_use = 1;
    g->atlas_x = (int16_t)ax;
    g->atlas_y = (int16_t)ay;
    g->gw = (int16_t)gw;
    g->gh = (int16_t)gh;
    g->xoff = (int16_t)xoff;
    g->yoff = (int16_t)yoff;
    g->advance = (scale > 0.0f && advance > 0) ? scale * (float)advance : FONT_ADV(fs);
    return g;
}

int mini_renderer_load_font(MiniRenderer *r, const char *path)
{
    if (!r || !path)
        return -1;

    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0)
    {
        fclose(f);
        return -1;
    }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf)
    {
        fclose(f);
        return -1;
    }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if ((long)rd != n)
    {
        free(buf);
        return -1;
    }

    stbtt_fontinfo *info = (stbtt_fontinfo *)malloc(sizeof(stbtt_fontinfo));
    if (!info)
    {
        free(buf);
        return -1;
    }
    if (!stbtt_InitFont(info, buf, stbtt_GetFontOffsetForIndex(buf, 0)))
    {
        free(info);
        free(buf);
        return -1;
    }

    if (r->fonts[0].loaded)
    {
        if (r->fonts[0].font_ctx)
            free(r->fonts[0].font_ctx);
        if (r->fonts[0].ttf_data)
            free(r->fonts[0].ttf_data);
    }

    stbtt_GetFontVMetrics(info, &r->fonts[0].ascent, &r->fonts[0].descent, &r->fonts[0].line_gap);
    r->fonts[0].ttf_data = buf;
    r->fonts[0].font_ctx = info;
    r->fonts[0].loaded = 1;
    if (r->num_fonts < 1)
        r->num_fonts = 1;

    r->font_ascent = r->fonts[0].ascent;
    r->font_descent = r->fonts[0].descent;
    r->font_line_gap = r->fonts[0].line_gap;
    r->ttf_data = buf;
    r->font_ctx = info;
    r->ttf_loaded = 1;
    g_active_renderer = r;
    r->tt_atlas_x = r->tt_atlas_y = r->tt_atlas_row_h = 0;
    memset(r->tt_cache, 0, sizeof(r->tt_cache));
    return 0;
}

float mini_text_measure_ex(const char *text, float font_size, float letter_spacing)
{
    if (!text || !*text || font_size <= 0)
        return 0.0f;
    font_init();
    if (g_active_renderer && g_active_renderer->ttf_loaded)
    {
        float w = 0.0f;
        const char *p = text;
        int len = 0;
        while (*p)
        {
            unsigned int cp = utf8_next(&p, &len);
            if (!len)
                break;
            TtGlyph *g = tt_get_glyph(g_active_renderer, cp, font_size);
            if (g)
                w += g->advance + letter_spacing;
            else
                w += FONT_ADV(font_size) + letter_spacing;
        }
        return w > 0.0f ? w : 0.0f;
    }
    float adv = FONT_ADV(font_size) + letter_spacing;
    float w = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        w += adv;
    return w - FONT_PX(font_size);
}

int mini_renderer_load_fallback_font(MiniRenderer *r, const char *path)
{
    if (!r || !path || r->num_fonts >= MINI_MAX_FONTS)
        return -1;

    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0)
    {
        fclose(f);
        return -1;
    }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf)
    {
        fclose(f);
        return -1;
    }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if ((long)rd != n)
    {
        free(buf);
        return -1;
    }

    stbtt_fontinfo *info = (stbtt_fontinfo *)malloc(sizeof(stbtt_fontinfo));
    if (!info)
    {
        free(buf);
        return -1;
    }
    if (!stbtt_InitFont(info, buf, stbtt_GetFontOffsetForIndex(buf, 0)))
    {
        free(info);
        free(buf);
        return -1;
    }

    int idx = r->num_fonts++;
    stbtt_GetFontVMetrics(info, &r->fonts[idx].ascent, &r->fonts[idx].descent, &r->fonts[idx].line_gap);
    r->fonts[idx].ttf_data = buf;
    r->fonts[idx].font_ctx = info;
    r->fonts[idx].loaded = 1;

    r->fallback_font_ascent = r->fonts[idx].ascent;
    r->fallback_font_descent = r->fonts[idx].descent;
    r->fallback_font_line_gap = r->fonts[idx].line_gap;
    r->fallback_ttf_data = buf;
    r->fallback_font_ctx = info;
    r->fallback_ttf_loaded = 1;
    return 0;
}

int mini_renderer_has_font(const MiniRenderer *r)
{
    return (r && (r->ttf_loaded || r->fallback_ttf_loaded || r->num_fonts > 0)) ? 1 : 0;
}

int mini_renderer_add_font_data(MiniRenderer *r, const char *family, const uint8_t *data, size_t size)
{
    if (!r)
        r = g_active_renderer;
    if (!r || !data || size == 0 || r->num_fonts >= MINI_MAX_FONTS)
        return -1;

    unsigned char *buf = (unsigned char *)malloc(size);
    if (!buf)
        return -1;
    memcpy(buf, data, size);

    stbtt_fontinfo *info = (stbtt_fontinfo *)malloc(sizeof(stbtt_fontinfo));
    if (!info)
    {
        free(buf);
        return -1;
    }
    if (!stbtt_InitFont(info, buf, stbtt_GetFontOffsetForIndex(buf, 0)))
    {
        free(info);
        free(buf);
        return -1;
    }

    int idx = r->num_fonts++;
    stbtt_GetFontVMetrics(info, &r->fonts[idx].ascent, &r->fonts[idx].descent, &r->fonts[idx].line_gap);
    r->fonts[idx].ttf_data = buf;
    r->fonts[idx].font_ctx = info;
    r->fonts[idx].loaded = 1;

    r->fallback_font_ascent = r->fonts[idx].ascent;
    r->fallback_font_descent = r->fonts[idx].descent;
    r->fallback_font_line_gap = r->fonts[idx].line_gap;
    r->fallback_ttf_data = buf;
    r->fallback_font_ctx = info;
    r->fallback_ttf_loaded = 1;
    return 0;
}

/* Decode the next UTF-8 codepoint from *p, advancing *p past it. Returns
   the codepoint and writes its byte length to *len (0 on end/invalid). */
static unsigned int utf8_next(const char **p, int *len)
{
    const unsigned char *s = (const unsigned char *)*p;
    if (!s || !*s)
    {
        *len = 0;
        return 0;
    }
    unsigned int c0 = s[0];
    if (c0 < 0x80)
    {
        *len = 1;
        *p = (const char *)(s + 1);
        return c0;
    }
    if ((c0 & 0xE0) == 0xC0 && s[1])
    {
        *len = 2;
        *p = (const char *)(s + 2);
        return ((c0 & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if ((c0 & 0xF0) == 0xE0 && s[1] && s[2])
    {
        *len = 3;
        *p = (const char *)(s + 3);
        return ((c0 & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    if ((c0 & 0xF8) == 0xF0 && s[1] && s[2] && s[3])
    {
        *len = 4;
        *p = (const char *)(s + 4);
        return ((c0 & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
               ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    }
    /* invalid lead byte �?advance one, return replacement */
    *len = 1;
    *p = (const char *)(s + 1);
    return 0xFFFD;
}

float mini_text_measure(const char *text, float font_size)
{
    return mini_text_measure_ex(text, font_size, 0.0f);
}

float mini_text_line_height(float font_size)
{
    /* 1.2x is the standard CSS line-height default for a font box. */
    return font_size > 0 ? font_size * 1.2f : 0.0f;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */
MiniRenderer *mini_renderer_create(int width, int height,
                                   int samples, int vsync)
{
    /* Become DPI-aware before the window is created so the GL framebuffer is
       at device resolution (no DWM bitmap upscale -> crisp on high-DPI). */
    mini_set_dpi_aware();
    if (!glfwInit())
        return NULL;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_SAMPLES, samples > 0 ? samples : 8);
    glfwWindowHint(GLFW_STENCIL_BITS, 8);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    GLFWwindow *win = glfwCreateWindow(width, height,
                                       "TinyFramework", NULL, NULL);
    if (!win)
    {
        glfwTerminate();
        return NULL;
    }
    glfwMakeContextCurrent(win);
    if (vsync)
        glfwSwapInterval(1);
    else
        glfwSwapInterval(0);
    fprintf(stderr, "[gl] GL_VERSION=%s  GLSL=%s\n",
            glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
    mini_gl_init_foundation(); /* Stage 4: resolve shader/FBO pointers + link a flat-color program */

    /* One-time quality/perf GL state. These are persistent; setting them
       every frame in begin_frame was pure churn, and GL_LINE_SMOOTH with
       GL_NICEST can push lines down a slow driver path on some GPUs — so we
       set them once at init instead. Line/polygon smoothing is left on for
       the AA it provides; GL_BLEND is (re)asserted per frame in
       begin_frame since the WebGL bridge can toggle it. */
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    glHint(GL_POLYGON_SMOOTH_HINT, GL_NICEST);

    MiniRenderer *r = (MiniRenderer *)calloc(1, sizeof(*r));
    if (!r)
    {
        glfwDestroyWindow(win);
        glfwTerminate();
        return NULL;
    }

    r->gpu.window_handle = win;
    r->gpu.width = width;
    r->gpu.height = height;
    r->gpu.sample_count = samples;
    r->gpu.vsync = vsync;
    r->gpu.gl_major = 2;
    r->gpu.gl_minor = 1;

    /* shared UI atlas: a single 1024x1024 GL texture for glyphs/sprites.
       Allocated once, updated by sub-rect; this is the dominant way thin
       2D engines keep texture-bind churn to zero on the GPU side.        */
    r->atlas_w = 2048;
    r->atlas_h = 2048;

    glGenTextures(1, &r->atlas_texture);
    glBindTexture(GL_TEXTURE_2D, r->atlas_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 r->atlas_w, r->atlas_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* CPU mirror of the atlas (zeroed). */
    r->atlas_cpu = (uint32_t *)calloc((size_t)r->atlas_w * r->atlas_h,
                                      sizeof(uint32_t));

    /* WebGL bridge (program/buffer/uniform bookkeeping) */
    r->gl_state = mini_gl_bridge_new();

    r->vbuf.width = width;
    r->vbuf.height = height;
    return r;
}

void mini_renderer_destroy(MiniRenderer *r)
{
    if (!r)
        return;
    free(r->glyph_quads);
    r->glyph_quads = NULL;
    r->glyph_cap = r->glyph_n = 0;
    free(r->text_store);
    r->text_store = NULL;
    r->text_cap = r->text_n = 0;
    for (int i = 0; i < r->num_fonts; i++)
    {
        if (r->fonts[i].font_ctx)
        {
            free(r->fonts[i].font_ctx);
            r->fonts[i].font_ctx = NULL;
        }
        if (r->fonts[i].ttf_data)
        {
            free(r->fonts[i].ttf_data);
            r->fonts[i].ttf_data = NULL;
        }
        r->fonts[i].loaded = 0;
    }
    r->num_fonts = 0;
    r->font_ctx = NULL;
    r->ttf_data = NULL;
    r->fallback_font_ctx = NULL;
    r->fallback_ttf_data = NULL;
    r->ttf_loaded = 0;
    r->fallback_ttf_loaded = 0;

    if (g_active_renderer == r)
        g_active_renderer = NULL;
    free(r->atlas_cpu);
    if (r->atlas_texture)
        glDeleteTextures(1, &r->atlas_texture);
    /* Free every cached gradient texture (slots with tex==0 are empty). */
    for (int i = 0; i < MINI_GRAD_CACHE_CAP; i++)
        if (r->grad_cache[i].tex)
            glDeleteTextures(1, &r->grad_cache[i].tex);
    mini_gl_bridge_destroy(r->gl_state);
    if (r->gpu.window_handle)
    {
        glfwDestroyWindow((GLFWwindow *)r->gpu.window_handle);
        glfwTerminate();
    }
    free(r);
}

/* ------------------------------------------------------------------ */
/* Per-frame begin / flush / end                                       */
/* ------------------------------------------------------------------ */
void mini_renderer_begin_frame(MiniRenderer *r)
{
    if (!r)
        return;
    g_active_renderer = r;
    r->vbuf.count = 0;
    r->glyph_n = 0;
    r->text_n = 0;
    r->clip_top = 0;

    int fw = r->gpu.width, fh = r->gpu.height;
    if (r->gpu.window_handle)
        glfwGetFramebufferSize((GLFWwindow *)r->gpu.window_handle, &fw, &fh);

    r->gpu.width = fw;
    r->gpu.height = fh;
    r->vbuf.width = fw;
    r->vbuf.height = fh;

    glViewport(0, 0, fw, fh);
    mini_gl_bridge_reset_2d(r->gl_state);

    /* 架构级修复 2.0：究极防暴走状态重置（Mother of All Resets）。
       全面斩断 WebGL/JS 层可能遗留的 VAO、FBO、混合方程、贴图单元、CULL_FACE 等感染，
       确保底层桌面级 GL 1.1 的固定管线绝对纯净，防止 DOM UI 绘制到虚空、被剔除或变黑。 */
    if (g_gl.UseProgram)
        g_gl.UseProgram(0);
    if (g_gl.BindFramebuffer)
        g_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);

    if (g_gl.BindVertexArray)
        g_gl.BindVertexArray(0);
    if (g_gl.BindBuffer)
    {
        g_gl.BindBuffer(0x8892 /*GL_ARRAY_BUFFER*/, 0);
        g_gl.BindBuffer(0x8893 /*GL_ELEMENT_ARRAY_BUFFER*/, 0);
    }
    if (g_gl.ActiveTexture)
        g_gl.ActiveTexture(0x84C0 /*GL_TEXTURE0*/);
    if (g_gl.BlendEquation)
        g_gl.BlendEquation(0x8006 /*GL_FUNC_ADD*/);
    /* NOTE: we deliberately do NOT DisableVertexAttribArray(0..7) here. The DOM
       2D pass uses legacy glBegin/glVertex immediate mode (it does not read
       generic vertex attributes), so disabling them is unnecessary here — but
       it would silently break WebGL apps (Three.js) that CACHE the enabled
       state: they would skip glEnableVertexAttribArray and rAF drawArrays would
       run with disabled attribute arrays -> no vertices -> a black canvas. The
       arrays Three.js enabled stay bound on the default VAO and are reused. */

    /* 致命漏洞修复：关闭 CULL_FACE 防止圆角矩形(POLYGON)被当作背面剔除 */
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);

    /* 关键：恢复 UI 渲染标准的 Alpha 混合与颜色蒙版写入 */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);

    /* 解除可能被 WebGL 模拟层遗留的固定管线客户端数组，防止 UI 渲染读越界全黑 */
    glDisableClientState(0x8074 /*GL_VERTEX_ARRAY*/);
    glDisableClientState(0x8076 /*GL_COLOR_ARRAY*/);
    glDisableClientState(0x8078 /*GL_TEXTURE_COORD_ARRAY*/);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, fw, fh, 0, -2000, 2000);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

/* Pack a 0..1 float color component into an RGBA8 uint32 (r<<24|g<<16|b<<8|a).
   Quantizing the stops into the cache key is what makes a hit exact: two
   gradients whose stops differ only below 8 bits get different entries. */
static uint32_t grad_pack_rgba(float fr, float fg, float fb, float fa)
{
    uint32_t r = (uint32_t)(fr <= 0.0f ? 0 : fr >= 1.0f ? 255
                                                        : (int)(fr * 255.0f + 0.5f));
    uint32_t g = (uint32_t)(fg <= 0.0f ? 0 : fg >= 1.0f ? 255
                                                        : (int)(fg * 255.0f + 0.5f));
    uint32_t b = (uint32_t)(fb <= 0.0f ? 0 : fb >= 1.0f ? 255
                                                        : (int)(fb * 255.0f + 0.5f));
    uint32_t a = (uint32_t)(fa <= 0.0f ? 0 : fa >= 1.0f ? 255
                                                        : (int)(fa * 255.0f + 0.5f));
    return (r << 24) | (g << 16) | (b << 8) | a;
}

/* Pick the CPU raster resolution for a gradient covering box (bw x bh).
   The old code capped each axis at 512 px, so any box larger than 512px was
   bilinearly upsampled by GL_LINEAR -> visibly blurry bands. We instead
   rasterize at the box's native resolution (pixel-exact), bounding only the
   total pixel count and the per-axis max so a pathological full-screen 4K
   gradient can't blow up GPU memory. GL_LINEAR then just smooths between
   exact gradient samples instead of magnifying a 512px image. */
#define MINI_GRAD_MAX_AXIS 4096
#define MINI_GRAD_MAX_PIXELS (4 * 1024 * 1024) /* ~16MB / texture ceiling */
static void grad_raster_dims(int bw, int bh, int *out_rw, int *out_rh)
{
    int rw = bw, rh = bh;
    if (rw < 1)
        rw = 1;
    if (rh < 1)
        rh = 1;
    if (rw > MINI_GRAD_MAX_AXIS)
        rw = MINI_GRAD_MAX_AXIS;
    if (rh > MINI_GRAD_MAX_AXIS)
        rh = MINI_GRAD_MAX_AXIS;
    long px = (long)rw * (long)rh;
    if (px > (long)MINI_GRAD_MAX_PIXELS)
    {
        /* proportional downscale to fit the pixel budget (keeps aspect) */
        float s = sqrtf((float)MINI_GRAD_MAX_PIXELS / (float)px);
        rw = (int)(rw * s);
        rh = (int)(rh * s);
        if (rw < 1)
            rw = 1;
        if (rh < 1)
            rh = 1;
    }
    *out_rw = rw;
    *out_rh = rh;
}

/* Look up (or create) a GL texture holding the CPU-rasterized gradient. The
   key is the full signature (type/angle/box-size/stops), so a hit is always
   exact. On miss: rasterize on the CPU, upload to a fresh GL 1.1 texture, and
   store it in the bounded cache, evicting the least-recently-used entry
   (deleting its GL texture) when the cache is full. Returns 0 only if the GL
   context cannot allocate (treated as "draw nothing" by the caller).         */
static uint32_t grad_cache_get(MiniRenderer *r, int type, int angle,
                               int bw, int bh, uint32_t c1p, uint32_t c2p,
                               int rw, int rh,
                               float c1r, float c1g, float c1b, float c1a,
                               float c2r, float c2g, float c2b, float c2a)
{
    /* 1. exact-match lookup (LRU touch) */
    int lru = 0;
    for (int i = 0; i < MINI_GRAD_CACHE_CAP; i++)
    {
        if (r->grad_cache[i].tex &&
            r->grad_cache[i].type == type &&
            r->grad_cache[i].angle == angle &&
            r->grad_cache[i].bw == bw &&
            r->grad_cache[i].bh == bh &&
            r->grad_cache[i].c1 == c1p &&
            r->grad_cache[i].c2 == c2p)
        {
            r->grad_cache[i].tick = ++r->grad_tick;
            return r->grad_cache[i].tex;
        }
        if (r->grad_cache[i].tick < r->grad_cache[lru].tick)
            lru = i; /* track the stalest slot for a potential miss */
    }

    /* 2. miss: rasterize on the CPU (no GL needed for the math) */
    unsigned char *buf = (unsigned char *)malloc((size_t)rw * rh * 4);
    if (!buf)
        return 0;
    mini_gradient_raster(type, rw, rh, (float)bw, (float)bh, (float)angle,
                         c1r, c1g, c1b, c1a, c2r, c2g, c2b, c2a, buf);

    /* 3. evict LRU if the chosen slot is occupied */
    if (r->grad_cache[lru].tex)
        glDeleteTextures(1, &r->grad_cache[lru].tex);

    /* 4. upload to a fresh GL 1.1 texture (no shaders/FBOs involved) */
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex)
    {
        free(buf);
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rw, rh, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, buf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    free(buf);

    r->grad_cache[lru].tex = tex;
    r->grad_cache[lru].type = type;
    r->grad_cache[lru].angle = angle;
    r->grad_cache[lru].bw = bw;
    r->grad_cache[lru].bh = bh;
    r->grad_cache[lru].c1 = c1p;
    r->grad_cache[lru].c2 = c2p;
    r->grad_cache[lru].tick = ++r->grad_tick;
    return tex;
}

static uint32_t grad_cache_multi_get(MiniRenderer *r, int type, float angle,
                                     int bw, int bh, const void *stops, int num_stops,
                                     int rw, int rh)
{
    uint32_t h = 2166136261u;
    h = (h ^ (uint32_t)type) * 16777619u;
    h = (h ^ (uint32_t)(angle * 10.0f)) * 16777619u;
    h = (h ^ (uint32_t)bw) * 16777619u;
    h = (h ^ (uint32_t)bh) * 16777619u;
    h = (h ^ (uint32_t)num_stops) * 16777619u;
    const uint8_t *sb = (const uint8_t *)stops;
    size_t sbsz = (size_t)num_stops * 20;
    for (size_t k = 0; k < sbsz; k++)
        h = (h ^ sb[k]) * 16777619u;

    int lru = 0;
    for (int i = 0; i < MINI_GRAD_CACHE_CAP; i++)
    {
        if (r->grad_cache[i].tex &&
            r->grad_cache[i].type == type &&
            r->grad_cache[i].bw == bw &&
            r->grad_cache[i].bh == bh &&
            r->grad_cache[i].c1 == h)
        {
            r->grad_cache[i].tick = ++r->grad_tick;
            return r->grad_cache[i].tex;
        }
        if (r->grad_cache[i].tick < r->grad_cache[lru].tick)
            lru = i;
    }

    unsigned char *buf = (unsigned char *)malloc((size_t)rw * rh * 4);
    if (!buf)
        return 0;
    mini_gradient_raster_multi(type, rw, rh, (float)bw, (float)bh, angle, stops, num_stops, buf);

    if (r->grad_cache[lru].tex)
        glDeleteTextures(1, &r->grad_cache[lru].tex);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex)
    {
        free(buf);
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rw, rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    free(buf);

    r->grad_cache[lru].tex = tex;
    r->grad_cache[lru].type = type;
    r->grad_cache[lru].angle = (int)angle;
    r->grad_cache[lru].bw = bw;
    r->grad_cache[lru].bh = bh;
    r->grad_cache[lru].c1 = h;
    r->grad_cache[lru].c2 = 0;
    r->grad_cache[lru].tick = ++r->grad_tick;
    return tex;
}

static void apply_clip(MiniRenderer *r);
static void draw_rounded_corners_poly(float x, float y, float w, float h, const float r[4]);

static void mini_exec_cmd(MiniRenderer *r, const MiniCmd *c)
{
    switch (c->type)
    {
    case MINI_CMD_CLEAR:
        glClearColor(c->r, c->g, c->b, c->a);
        glClear(GL_COLOR_BUFFER_BIT);
        break;
    case MINI_CMD_RECT:
        glColor4f(c->r, c->g, c->b, c->a);
        glBegin(GL_QUADS);
        glVertex2f(c->x, c->y);
        glVertex2f(c->x + c->w, c->y);
        glVertex2f(c->x + c->w, c->y + c->h);
        glVertex2f(c->x, c->y + c->h);
        glEnd();
        break;
    case MINI_CMD_TEXTURE_RECT:
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, c->texture_id);
        glColor4f(1, 1, 1, c->a);
        glBegin(GL_QUADS);
        glTexCoord2f(c->u0, c->v0);
        glVertex2f(c->x, c->y);
        glTexCoord2f(c->u1, c->v0);
        glVertex2f(c->x + c->w, c->y);
        glTexCoord2f(c->u1, c->v1);
        glVertex2f(c->x + c->w, c->y + c->h);
        glTexCoord2f(c->u0, c->v1);
        glVertex2f(c->x, c->y + c->h);
        glEnd();
        glDisable(GL_TEXTURE_2D);
        break;
    case MINI_CMD_GLYPH_RUN:
    {
        const MiniGlyphQuad *q = (const MiniGlyphQuad *)NULL;
        int n = c->glyph_count, start = c->reserved;
        if (start < 0 || start + n > r->glyph_n)
            break;
        q = r->glyph_quads + start;
        if (n <= 0)
            break;
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, r->atlas_texture);
        glColor4f(c->r, c->g, c->b, c->a);
        glBegin(GL_QUADS);
        for (int i = 0; i < n; i++)
        {
            /* Snap each glyph to the nearest whole pixel. Glyph quads carry
               fractional pen offsets (stbtt advances accumulate to a
               subpixel x, baseline math to a subpixel y); placing them at
               those fractional coords makes GL_LINEAR bilinearly smear every
               glyph by up to half a pixel -> soft/blurry text. Rounding to
               the nearest pixel lands each glyph 1:1 with the atlas so the
               filter only smooths the anti-aliased glyph edges, not its
               position. Spacing drift is <1px per glyph (invisible). */
            float gx = floorf(c->x + q[i].dx + 0.5f);
            float gy = floorf(c->y + q[i].dy + 0.5f);
            float gw = (float)q[i].gw;
            float gh = (float)q[i].gh;
            int is_italic = (c->font_id & 0x100) != 0;
            float slant = is_italic ? (gh * 0.22f) : 0.0f;

            glTexCoord2f(q[i].u0, q[i].v0);
            glVertex2f(gx + slant, gy);
            glTexCoord2f(q[i].u1, q[i].v0);
            glVertex2f(gx + gw + slant, gy);
            glTexCoord2f(q[i].u1, q[i].v1);
            glVertex2f(gx + gw, gy + gh);
            glTexCoord2f(q[i].u0, q[i].v1);
            glVertex2f(gx, gy + gh);
        }
        glEnd();
        glDisable(GL_TEXTURE_2D);
        break;
    }
    case MINI_CMD_RECT_STROKE:
    {
        float lw = c->reserved > 0 ? (float)c->reserved : 1.0f;
        if (lw < 1.0f)
            lw = 1.0f;

        // 解析传递进来的圆角半径 (复用 u0, v0 等浮点字段作为半径)
        float r_tl = c->u0, r_tr = c->v0, r_br = c->u1, r_bl = c->v1;
        if (r_tl > 0 || r_tr > 0 || r_br > 0 || r_bl > 0)
        {
            float max_r = (c->w < c->h ? c->w : c->h) * 0.5f;
            if (r_tl > max_r)
                r_tl = max_r;
            if (r_tr > max_r)
                r_tr = max_r;
            if (r_br > max_r)
                r_br = max_r;
            if (r_bl > max_r)
                r_bl = max_r;

            glColor4f(c->r, c->g, c->b, c->a);
            glLineWidth(lw);
            glBegin(GL_LINE_LOOP);
            if (r_tl > 0.5f)
            {
                int segs = (int)(r_tl * 1.5f);
                if (segs < 8)
                    segs = 8;
                if (segs > 32)
                    segs = 32;
                for (int i = 0; i <= segs; i++)
                {
                    float angle = PI + (i * PI / (2.0f * segs));
                    glVertex2f(c->x + r_tl + cosf(angle) * r_tl, c->y + r_tl + sinf(angle) * r_tl);
                }
            }
            else
                glVertex2f(c->x, c->y);

            if (r_tr > 0.5f)
            {
                int segs = (int)(r_tr * 1.5f);
                if (segs < 8)
                    segs = 8;
                if (segs > 32)
                    segs = 32;
                for (int i = 0; i <= segs; i++)
                {
                    float angle = 1.5f * PI + (i * PI / (2.0f * segs));
                    glVertex2f(c->x + c->w - r_tr + cosf(angle) * r_tr, c->y + r_tr + sinf(angle) * r_tr);
                }
            }
            else
                glVertex2f(c->x + c->w, c->y);

            if (r_br > 0.5f)
            {
                int segs = (int)(r_br * 1.5f);
                if (segs < 8)
                    segs = 8;
                if (segs > 32)
                    segs = 32;
                for (int i = 0; i <= segs; i++)
                {
                    float angle = 0.0f + (i * PI / (2.0f * segs));
                    glVertex2f(c->x + c->w - r_br + cosf(angle) * r_br, c->y + c->h - r_br + sinf(angle) * r_br);
                }
            }
            else
                glVertex2f(c->x + c->w, c->y + c->h);

            if (r_bl > 0.5f)
            {
                int segs = (int)(r_bl * 1.5f);
                if (segs < 8)
                    segs = 8;
                if (segs > 32)
                    segs = 32;
                for (int i = 0; i <= segs; i++)
                {
                    float angle = 0.5f * PI + (i * PI / (2.0f * segs));
                    glVertex2f(c->x + r_bl + cosf(angle) * r_bl, c->y + c->h - r_bl + sinf(angle) * r_bl);
                }
            }
            else
                glVertex2f(c->x, c->y + c->h);
            glEnd();
            glLineWidth(1.0f);
            break;
        }

        /* Original logic for straight rect stroke */
        float x = c->x, y = c->y, w = c->w, h = c->h;
        glColor4f(c->r, c->g, c->b, c->a);
        glBegin(GL_QUADS);
        glVertex2f(x, y);
        glVertex2f(x + w, y);
        glVertex2f(x + w, y + lw);
        glVertex2f(x, y + lw);
        glVertex2f(x, y + h - lw);
        glVertex2f(x + w, y + h - lw);
        glVertex2f(x + w, y + h);
        glVertex2f(x, y + h);
        glVertex2f(x, y);
        glVertex2f(x + lw, y);
        glVertex2f(x + lw, y + h);
        glVertex2f(x, y + h);
        glVertex2f(x + w - lw, y);
        glVertex2f(x + w, y);
        glVertex2f(x + w, y + h);
        glVertex2f(x + w - lw, y + h);
        glEnd();
        break;
    }
    case MINI_CMD_LINE:
    {
        /* Axis-aligned-ish line as a rotated quad. p1=(x,y), p2=(w,h).
           Width in `reserved`. */
        float lw = c->reserved > 0 ? (float)c->reserved : 1.0f;
        if (lw < 1.0f)
            lw = 1.0f;
        float dx = c->w - c->x, dy = c->h - c->y;
        float len = (float)sqrt(dx * dx + dy * dy);
        if (len <= 0)
            break;
        float nx = -dy / len * (lw * 0.5f), ny = dx / len * (lw * 0.5f);
        glColor4f(c->r, c->g, c->b, c->a);
        glBegin(GL_QUADS);
        glVertex2f(c->x + nx, c->y + ny);
        glVertex2f(c->x - nx, c->y - ny);
        glVertex2f(c->w - nx, c->h - ny);
        glVertex2f(c->w + nx, c->h + ny);
        glEnd();
        break;
    }
    case MINI_CMD_TRIANGLE:
    {
        /* Filled triangle: p1=(x,y), p2=(w,h), p3=(u0,v0). Used for the
           play button and the details/summary disclosure caret. */
        glColor4f(c->r, c->g, c->b, c->a);
        glBegin(GL_TRIANGLES);
        glVertex2f(c->x, c->y);
        glVertex2f(c->w, c->h);
        glVertex2f(c->u0, c->v0);
        glEnd();
        break;
    }
    case MINI_CMD_CIRCLE:
    {
        float rad = c->w;
        int segs = c->reserved > 0 ? c->reserved : (int)(rad * 2.5f);
        if (segs < 32)
            segs = 32;
        if (segs > 128)
            segs = 128;
        glColor4f(c->r, c->g, c->b, c->a);
        glBegin(GL_TRIANGLE_FAN);
        glVertex2f(c->x, c->y);
        for (int i = 0; i <= segs; i++)
        {
            float a = (float)i * (6.2831853f / (float)segs);
            glVertex2f(c->x + cosf(a) * rad, c->y + sinf(a) * rad);
        }
        glEnd();
        break;
    }
    case MINI_CMD_TEXT:
    {
        /* Rasterize `len` bytes from r->text_store+start through the 5x7
           font as one batched glBegin(GL_QUADS) of per-pixel rects.
           font_size lives in `w`. Unknown glyphs render as a filled cell
           so any text (incl. multibyte / unauthored) still shows. */
        font_init();
        int n = c->glyph_count, start = c->reserved;
        if (start < 0 || start + n > r->text_n)
            break;
        const char *s = r->text_store + start;
        float fs = c->w > 0 ? c->w : 16.0f;
        float px = FONT_PX(fs), gw = FONT_GW(fs), gh = FONT_GH(fs), adv = FONT_ADV(fs);
        float penx = c->x, peny = c->y;
        glColor4f(c->r, c->g, c->b, c->a);
        glBegin(GL_QUADS);
        for (int i = 0; i < n; i++)
        {
            unsigned char ch = (unsigned char)s[i];
            if (ch == '\n' || ch == '\r')
            {
                continue;
            }
            int has_glyph = (ch >= 0x20 && ch < 0x7F);
            if (has_glyph && ch != 0x20)
            {
                for (int r = 0; r < 7; r++)
                {
                    uint8_t row = g_font[ch][r];
                    if (!row)
                        continue;
                    for (int col = 0; col < 5; col++)
                    {
                        if (row & (1u << col))
                        {
                            float bx = penx + col * px;
                            float by = peny + r * px;
                            glVertex2f(bx, by);
                            glVertex2f(bx + px, by);
                            glVertex2f(bx + px, by + px);
                            glVertex2f(bx, by + px);
                        }
                    }
                }
            }
            else if (!has_glyph && ch != 0x20)
            {
                /* Non-ASCII / unmapped byte (e.g. the �?emoji is 3 UTF-8 bytes,
                   CJK glyphs are 3 bytes each). The 5x7 font has no glyph for
                   them. Drawing a filled bar per byte used to leave a solid
                   black rectangle inside circles like the avatar; instead skip
                   silently so multibyte text just renders as blank space �?                   the surrounding shape (a gradient circle, a label box)
                   stays clean. The pen still advances so layout widths are
                   roughly preserved.                                            */
                /* no quad emitted */
            }
            penx += adv;
        }
        glEnd();
        break;
    }
    case MINI_CMD_RECT_ROUNDED:
    {
        float r_tl = c->u0, r_tr = c->v0, r_br = c->u1, r_bl = c->v1;
        float max_r = (c->w < c->h ? c->w : c->h) * 0.5f;
        if (r_tl > max_r)
            r_tl = max_r;
        if (r_tr > max_r)
            r_tr = max_r;
        if (r_br > max_r)
            r_br = max_r;
        if (r_bl > max_r)
            r_bl = max_r;
        if (r_tl < 0)
            r_tl = 0;
        if (r_tr < 0)
            r_tr = 0;
        if (r_br < 0)
            r_br = 0;
        if (r_bl < 0)
            r_bl = 0;

        glColor4f(c->r, c->g, c->b, c->a);
        glBegin(GL_TRIANGLE_FAN);
        glVertex2f(c->x + c->w * 0.5f, c->y + c->h * 0.5f); /* Center Anchor */

        int segs = 16;
        if (r_tl > 0.5f)
        {
            for (int i = 0; i <= segs; i++)
            {
                float angle = PI + (i * PI / (2.0f * segs));
                glVertex2f(c->x + r_tl + cosf(angle) * r_tl, c->y + r_tl + sinf(angle) * r_tl);
            }
        }
        else
        {
            glVertex2f(c->x, c->y);
        }

        if (r_tr > 0.5f)
        {
            for (int i = 0; i <= segs; i++)
            {
                float angle = 1.5f * PI + (i * PI / (2.0f * segs));
                glVertex2f(c->x + c->w - r_tr + cosf(angle) * r_tr, c->y + r_tr + sinf(angle) * r_tr);
            }
        }
        else
        {
            glVertex2f(c->x + c->w, c->y);
        }

        if (r_br > 0.5f)
        {
            for (int i = 0; i <= segs; i++)
            {
                float angle = 0.0f + (i * PI / (2.0f * segs));
                glVertex2f(c->x + c->w - r_br + cosf(angle) * r_br, c->y + c->h - r_br + sinf(angle) * r_br);
            }
        }
        else
        {
            glVertex2f(c->x + c->w, c->y + c->h);
        }

        if (r_bl > 0.5f)
        {
            for (int i = 0; i <= segs; i++)
            {
                float angle = 0.5f * PI + (i * PI / (2.0f * segs));
                glVertex2f(c->x + r_bl + cosf(angle) * r_bl, c->y + c->h - r_bl + sinf(angle) * r_bl);
            }
        }
        else
        {
            glVertex2f(c->x, c->y + c->h);
        }

        /* Close loop */
        if (r_tl > 0.5f)
        {
            float a = PI + (0 * PI / (2.0f * segs));
            glVertex2f(c->x + r_tl + cosf(a) * r_tl, c->y + r_tl + sinf(a) * r_tl);
        }
        else
        {
            glVertex2f(c->x, c->y);
        }
        glEnd();
        break;
    }
    case MINI_CMD_PUSH_CLIP:
    {
        if (r->clip_top < MINI_CLIP_DEPTH)
        {
            if (r->clip_top > 0)
            {
                float ax = r->clip[r->clip_top - 1].x, ay = r->clip[r->clip_top - 1].y;
                float ar = ax + r->clip[r->clip_top - 1].w;
                float ab = ay + r->clip[r->clip_top - 1].h;
                float bx = c->x, by = c->y, br = c->x + c->w, bb = c->y + c->h;
                float nx = ax > bx ? ax : bx;
                float ny = ay > by ? ay : by;
                float nr = ar < br ? ar : br;
                float nb = ab < bb ? ab : bb;
                r->clip[r->clip_top].x = nx;
                r->clip[r->clip_top].y = ny;
                r->clip[r->clip_top].w = (nr > nx) ? nr - nx : 0.0f;
                r->clip[r->clip_top].h = (nb > ny) ? nb - ny : 0.0f;
            }
            else
            {
                r->clip[r->clip_top].x = c->x;
                r->clip[r->clip_top].y = c->y;
                r->clip[r->clip_top].w = c->w;
                r->clip[r->clip_top].h = c->h;
            }
            r->clip[r->clip_top].stencil = 0;
            r->clip_top++;
        }
        apply_clip(r);
        break;
    }
    case MINI_CMD_POP_CLIP:
    {
        if (r->clip_top > 0)
        {
            if (r->clip[r->clip_top - 1].stencil)
                glDisable(GL_STENCIL_TEST);
            r->clip_top--;
        }
        apply_clip(r);
        break;
    }
    case MINI_CMD_PUSH_ROUNDED_CLIP:
    {
        if (r->clip_top < MINI_CLIP_DEPTH)
        {
            if (r->clip_top > 0)
            {
                float ax = r->clip[r->clip_top - 1].x, ay = r->clip[r->clip_top - 1].y;
                float ar = ax + r->clip[r->clip_top - 1].w;
                float ab = ay + r->clip[r->clip_top - 1].h;
                float bx = c->x, by = c->y, br = c->x + c->w, bb = c->y + c->h;
                float nx = ax > bx ? ax : bx;
                float ny = ay > by ? ay : by;
                float nr = ar < br ? ar : br;
                float nb = ab < bb ? ab : bb;
                r->clip[r->clip_top].x = nx;
                r->clip[r->clip_top].y = ny;
                r->clip[r->clip_top].w = (nr > nx) ? nr - nx : 0.0f;
                r->clip[r->clip_top].h = (nb > ny) ? nb - ny : 0.0f;
            }
            else
            {
                r->clip[r->clip_top].x = c->x;
                r->clip[r->clip_top].y = c->y;
                r->clip[r->clip_top].w = c->w;
                r->clip[r->clip_top].h = c->h;
            }
            r->clip[r->clip_top].stencil = 1;
            r->clip_top++;
        }
        apply_clip(r);
        GLint stencil_bits = 0;
        glGetIntegerv(GL_STENCIL_BITS, &stencil_bits);
        if (stencil_bits > 0)
        {
            float rads[4] = {c->u0, c->v0, c->u1, c->v1};
            glEnable(GL_STENCIL_TEST);
            glClearStencil(0);
            glClear(GL_STENCIL_BUFFER_BIT);
            glStencilFunc(GL_ALWAYS, 1, 0xFF);
            glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
            glStencilMask(0xFF);
            glDisable(GL_BLEND);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            draw_rounded_corners_poly(c->x, c->y, c->w, c->h, rads);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glEnable(GL_BLEND);
            glStencilFunc(GL_EQUAL, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            glStencilMask(0x00);
        }
        break;
    }

    case MINI_CMD_GRADIENT:
    {
        int type = c->reserved;           /* 0=linear 1=radial 2=conic     */
        int angle = (int)c->font_id;      /* normalized degrees 0..359     */
        float rad = (float)c->texture_id; /* corner radius (0 = sharp box) */
        if (rad > c->w * 0.5f)
            rad = c->w * 0.5f;
        if (rad > c->h * 0.5f)
            rad = c->h * 0.5f;

        /* Axis-aligned linear (0/90/180/270deg) stays on the fast fixed-
           function Gouraud path: no texture bind, no upload, no GL state
           churn. Radial/conic/angled-linear need per-pixel math, so they
           take the CPU-raster texture path below.                          */
        if (type == 0 && (angle % 90) == 0)
        {
            int vertical = (angle == 0 || angle == 180);

            /* 核心修复：防止 GL_POLYGON 在直角时因多重退化顶点引发巨大的对角线缺口！ */
            if (rad <= 0.0f)
            {
                glBegin(GL_QUADS);
                float t;
                t = vertical ? 0.0f : 0.0f;
                glColor4f(c->r * (1.0f - t) + c->u0 * t, c->g * (1.0f - t) + c->v0 * t, c->b * (1.0f - t) + c->u1 * t, c->a * (1.0f - t) + c->v1 * t);
                glVertex2f(c->x, c->y);

                t = vertical ? 0.0f : 1.0f;
                glColor4f(c->r * (1.0f - t) + c->u0 * t, c->g * (1.0f - t) + c->v0 * t, c->b * (1.0f - t) + c->u1 * t, c->a * (1.0f - t) + c->v1 * t);
                glVertex2f(c->x + c->w, c->y);

                t = vertical ? 1.0f : 1.0f;
                glColor4f(c->r * (1.0f - t) + c->u0 * t, c->g * (1.0f - t) + c->v0 * t, c->b * (1.0f - t) + c->u1 * t, c->a * (1.0f - t) + c->v1 * t);
                glVertex2f(c->x + c->w, c->y + c->h);

                t = vertical ? 1.0f : 0.0f;
                glColor4f(c->r * (1.0f - t) + c->u0 * t, c->g * (1.0f - t) + c->v0 * t, c->b * (1.0f - t) + c->u1 * t, c->a * (1.0f - t) + c->v1 * t);
                glVertex2f(c->x, c->y + c->h);
                glEnd();
                break;
            }

            glBegin(GL_POLYGON);
            int segments = (int)(rad * 1.5f);
            if (segments < 16)
                segments = 16;
            if (segments > 32)
                segments = 32;

#define EMIT_GRAD_VERTEX(vx, vy)                                          \
    do                                                                    \
    {                                                                     \
        float t = vertical ? ((vy) - c->y) / c->h : ((vx) - c->x) / c->w; \
        if (t < 0.0f)                                                     \
            t = 0.0f;                                                     \
        if (t > 1.0f)                                                     \
            t = 1.0f;                                                     \
        glColor4f(c->r * (1.0f - t) + c->u0 * t,                          \
                  c->g * (1.0f - t) + c->v0 * t,                          \
                  c->b * (1.0f - t) + c->u1 * t,                          \
                  c->a * (1.0f - t) + c->v1 * t);                         \
        glVertex2f((vx), (vy));                                           \
    } while (0)

            /* Top-Left */
            for (int i = 0; i <= segments; i++)
            {
                float a = PI + (i * PI / (2 * segments));
                EMIT_GRAD_VERTEX(c->x + rad + cosf(a) * rad, c->y + rad + sinf(a) * rad);
            }
            /* Top-Right */
            for (int i = 0; i <= segments; i++)
            {
                float a = 1.5f * PI + (i * PI / (2 * segments));
                EMIT_GRAD_VERTEX(c->x + c->w - rad + cosf(a) * rad, c->y + rad + sinf(a) * rad);
            }
            /* Bottom-Right */
            for (int i = 0; i <= segments; i++)
            {
                float a = 0 + (i * PI / (2 * segments));
                EMIT_GRAD_VERTEX(c->x + c->w - rad + cosf(a) * rad, c->y + c->h - rad + sinf(a) * rad);
            }
            /* Bottom-Left */
            for (int i = 0; i <= segments; i++)
            {
                float a = 0.5f * PI + (i * PI / (2 * segments));
                EMIT_GRAD_VERTEX(c->x + rad + cosf(a) * rad, c->y + c->h - rad + sinf(a) * rad);
            }
            glEnd();
#undef EMIT_GRAD_VERTEX
            break;
        }

        /* ---- CPU-raster texture path (radial / conic / angled-linear) ----
           Rasterize on the CPU into a cached GL 1.1 texture, then draw a
           textured polygon. No shaders/FBOs, so the WebGL bridge's bound
           program cannot be clobbered; GL_TEXTURE_2D is disabled after the
           draw exactly like the existing TEXTURE_RECT / GLYPH_RUN paths.   */
        if (c->w <= 0.0f || c->h <= 0.0f)
            break;
        int bw = (int)(c->w + 0.5f), bh = (int)(c->h + 0.5f);
        int rw, rh;
        grad_raster_dims(bw, bh, &rw, &rh);
        uint32_t c1p = grad_pack_rgba(c->r, c->g, c->b, c->a);
        uint32_t c2p = grad_pack_rgba(c->u0, c->v0, c->u1, c->v1);
        uint32_t tex = grad_cache_get(r, type, angle, bw, bh, c1p, c2p, rw, rh,
                                      c->r, c->g, c->b, c->a,
                                      c->u0, c->v0, c->u1, c->v1);
        if (!tex)
            break;
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, tex);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f); /* texture already carries RGBA */
        if (rad > 0.0f)
        {
            /* Rounded polygon: same arc layout as the Gouraud path, but
               each vertex carries a UV mapped across the full box so the
               lower-res gradient texture is sampled at the right point.    */
            glBegin(GL_POLYGON);
            int segments = (int)(rad * 1.5f);
            if (segments < 16)
                segments = 16;
            if (segments > 32)
                segments = 32;
#define EMIT_GRAD_UV(vx, vy)                                      \
    do                                                            \
    {                                                             \
        glTexCoord2f(((vx) - c->x) / c->w, ((vy) - c->y) / c->h); \
        glVertex2f((vx), (vy));                                   \
    } while (0)
            for (int i = 0; i <= segments; i++)
            {
                float a = PI + (i * PI / (2 * segments));
                EMIT_GRAD_UV(c->x + rad + cosf(a) * rad, c->y + rad + sinf(a) * rad);
            }
            for (int i = 0; i <= segments; i++)
            {
                float a = 1.5f * PI + (i * PI / (2 * segments));
                EMIT_GRAD_UV(c->x + c->w - rad + cosf(a) * rad, c->y + rad + sinf(a) * rad);
            }
            for (int i = 0; i <= segments; i++)
            {
                float a = 0 + (i * PI / (2 * segments));
                EMIT_GRAD_UV(c->x + c->w - rad + cosf(a) * rad, c->y + c->h - rad + sinf(a) * rad);
            }
            for (int i = 0; i <= segments; i++)
            {
                float a = 0.5f * PI + (i * PI / (2 * segments));
                EMIT_GRAD_UV(c->x + rad + cosf(a) * rad, c->y + c->h - rad + sinf(a) * rad);
            }
            glEnd();
#undef EMIT_GRAD_UV
        }
        else
        {
            glBegin(GL_QUADS);
            glTexCoord2f(0.0f, 0.0f);
            glVertex2f(c->x, c->y);
            glTexCoord2f(1.0f, 0.0f);
            glVertex2f(c->x + c->w, c->y);
            glTexCoord2f(1.0f, 1.0f);
            glVertex2f(c->x + c->w, c->y + c->h);
            glTexCoord2f(0.0f, 1.0f);
            glVertex2f(c->x, c->y + c->h);
            glEnd();
        }
        glDisable(GL_TEXTURE_2D); /* restore: matches TEXTURE_RECT path */
        break;
    }

    case MINI_CMD_SHADOW:
    {
        glDisable(GL_TEXTURE_2D);
        float rad = c->u0;
        float spread = c->v0;
        float blur = c->u1;

        if (rad > c->w * 0.5f)
            rad = c->w * 0.5f;
        if (rad > c->h * 0.5f)
            rad = c->h * 0.5f;

        float ix = c->x - spread, iy = c->y - spread;
        float iw = c->w + spread * 2.0f, ih = c->h + spread * 2.0f;
        float ir = rad;

        if (iw <= 0.0f || ih <= 0.0f)
            break;

        float cx1 = ix + ir, cy1 = iy + ir;
        float cx2 = ix + iw - ir, cy2 = iy + ir;
        float cx3 = ix + iw - ir, cy3 = iy + ih - ir;
        float cx4 = ix + ir, cy4 = iy + ih - ir;

        int segments = 16;

        /* 1. Only draw solid inner core when spread is positive */
        if (spread > 0.0f)
        {
            glColor4f(c->r, c->g, c->b, c->a);
            glBegin(GL_TRIANGLE_FAN);
            glVertex2f(ix + iw * 0.5f, iy + ih * 0.5f);
            for (int i = 0; i <= segments; i++)
            {
                float a = PI + (i * PI / (2 * segments));
                glVertex2f(cx1 + cosf(a) * ir, cy1 + sinf(a) * ir);
            }
            for (int i = 0; i <= segments; i++)
            {
                float a = 1.5f * PI + (i * PI / (2 * segments));
                glVertex2f(cx2 + cosf(a) * ir, cy2 + sinf(a) * ir);
            }
            for (int i = 0; i <= segments; i++)
            {
                float a = 0.0f + (i * PI / (2 * segments));
                glVertex2f(cx3 + cosf(a) * ir, cy3 + sinf(a) * ir);
            }
            for (int i = 0; i <= segments; i++)
            {
                float a = 0.5f * PI + (i * PI / (2 * segments));
                glVertex2f(cx4 + cosf(a) * ir, cy4 + sinf(a) * ir);
            }
            glVertex2f(cx1 - ir, cy1);
            glEnd();
        }

        /* 2. Edge Quads & Corner Fans for smooth Gaussian blur falloff */
        if (blur > 0.0f)
        {
            const int num_rings = 3;
            const float ring_radii[4] = {0.0f, 0.40f, 0.75f, 1.0f};
            const float ring_alphas[4] = {0.45f, 0.18f, 0.04f, 0.0f};

            for (int ring = 0; ring < num_rings; ring++)
            {
                float r_in = ir + blur * ring_radii[ring];
                float r_out = ir + blur * ring_radii[ring + 1];
                float a_in = c->a * ring_alphas[ring];
                float a_out = c->a * ring_alphas[ring + 1];

                /* 4 Edge Quads */
                glBegin(GL_QUADS);
                /* Top */
                glColor4f(c->r, c->g, c->b, a_in);
                glVertex2f(cx1, cy1 - r_in);
                glVertex2f(cx2, cy2 - r_in);
                glColor4f(c->r, c->g, c->b, a_out);
                glVertex2f(cx2, cy2 - r_out);
                glVertex2f(cx1, cy1 - r_out);

                /* Right */
                glColor4f(c->r, c->g, c->b, a_in);
                glVertex2f(cx2 + r_in, cy2);
                glVertex2f(cx3 + r_in, cy3);
                glColor4f(c->r, c->g, c->b, a_out);
                glVertex2f(cx3 + r_out, cy3);
                glVertex2f(cx2 + r_out, cy2);

                /* Bottom */
                glColor4f(c->r, c->g, c->b, a_in);
                glVertex2f(cx3, cy3 + r_in);
                glVertex2f(cx4, cy4 + r_in);
                glColor4f(c->r, c->g, c->b, a_out);
                glVertex2f(cx4, cy4 + r_out);
                glVertex2f(cx3, cy3 + r_out);

                /* Left */
                glColor4f(c->r, c->g, c->b, a_in);
                glVertex2f(cx4 - r_in, cy4);
                glVertex2f(cx1 - r_in, cy1);
                glColor4f(c->r, c->g, c->b, a_out);
                glVertex2f(cx1 - r_out, cy1);
                glVertex2f(cx4 - r_out, cy4);
                glEnd();

                /* 4 Corner Strips */
#define DRAW_CORNER_STRIP(ccx, ccy, start_ang)              \
    glBegin(GL_TRIANGLE_STRIP);                             \
    for (int i = 0; i <= segments; i++)                     \
    {                                                       \
        float a = (start_ang) + (i * PI / (2 * segments));  \
        float ca = cosf(a), sa = sinf(a);                   \
        glColor4f(c->r, c->g, c->b, a_in);                  \
        glVertex2f((ccx) + ca * r_in, (ccy) + sa * r_in);   \
        glColor4f(c->r, c->g, c->b, a_out);                 \
        glVertex2f((ccx) + ca * r_out, (ccy) + sa * r_out); \
    }                                                       \
    glEnd();

                DRAW_CORNER_STRIP(cx1, cy1, PI);
                DRAW_CORNER_STRIP(cx2, cy2, 1.5f * PI);
                DRAW_CORNER_STRIP(cx3, cy3, 0.0f);
                DRAW_CORNER_STRIP(cx4, cy4, 0.5f * PI);
#undef DRAW_CORNER_STRIP
            }
        }
        break;
    }
    case MINI_CMD_PUSH_XFORM:
    {
        glPushMatrix();
        float tx = c->w;
        float ty = c->h;
        float tz = c->a;
        float sx = (c->u0 != 0.0f) ? c->u0 : 1.0f;
        float sy = (c->v0 != 0.0f) ? c->v0 : 1.0f;
        float skx = c->u1;
        float sky = c->v1;

        // 1. Move to transformed center
        glTranslatef(c->x + tx, c->y + ty, tz);

        // 2. 3D perspective & rotation. The perspective matrix is applied
        //    whenever a rotation OR an explicit perspective() is set, with
        //    d taken from c->persp (CSS perspective()/perspective:) when >0,
        //    else the renderer default. Previously d was hardcoded 1000 and
        //    only applied on rotation, so perspective: was a dead field.
        if (c->persp > 0.0f)
        {
            float d = c->persp;
            float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, -1.0f / d, 0, 0, 0, 1};
            glMultMatrixf(m);
        }
        if (c->r != 0.0f)
            glRotatef(c->r * 57.29578f, 1.0f, 0.0f, 0.0f);
        if (c->g != 0.0f)
            glRotatef(c->g * 57.29578f, 0.0f, 1.0f, 0.0f);
        if (c->b != 0.0f)
            glRotatef(c->b * 57.29578f, 0.0f, 0.0f, 1.0f);

        // 3. Skew
        if (skx != 0.0f || sky != 0.0f)
        {
            float skew_m[16] = {
                1.0f, tanf(sky), 0.0f, 0.0f,
                tanf(skx), 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f};
            glMultMatrixf(skew_m);
        }

        // 4. Scale
        if (sx != 1.0f || sy != 1.0f)
        {
            glScalef(sx, sy, 1.0f);
        }

        // 5. Move back from center
        glTranslatef(-c->x, -c->y, 0.0f);
        break;
    }
    case MINI_CMD_POP_XFORM:
    {
        glPopMatrix();
        break;
    }
    case 20: /* BACKDROP_FILTER (真·浏览器级高斯弥散 + 磨砂玻璃通透滤镜) */
    {
        float blur = c->u0, invert = c->v0;
        if (blur <= 0.0f && invert <= 0.0f)
            break;

        float r_tl = c->u1, r_tr = c->v1, r_br = c->r, r_bl = c->g;
        float rads[4] = {r_tl, r_tr, r_br, r_bl};
        int has_rounded = (r_tl > 0 || r_tr > 0 || r_br > 0 || r_bl > 0);

        if (blur > 0.0f)
        {
            GLfloat m[16];
            glGetFloatv(GL_MODELVIEW_MATRIX, m);

            float px[4] = {c->x, c->x + c->w, c->x + c->w, c->x};
            float py[4] = {c->y, c->y, c->y + c->h, c->y + c->h};
            float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
            for (int i = 0; i < 4; i++)
            {
                float sx = px[i] * m[0] + py[i] * m[4] + m[12];
                float sy = px[i] * m[1] + py[i] * m[5] + m[13];
                if (sx < min_x)
                    min_x = sx;
                if (sx > max_x)
                    max_x = sx;
                if (sy < min_y)
                    min_y = sy;
                if (sy > max_y)
                    max_y = sy;
            }

            if (max_x - min_x > 0.001f && max_y - min_y > 0.001f)
            {
                /* 外扩 1.8 倍模糊半径抓取屏幕，让光晕自然弥散扩散进整个盒子 */
                float pad = ceilf(blur * 1.8f);
                if (pad > 64.0f)
                    pad = 64.0f;

                int vx = (int)floorf(min_x - pad);
                int vy = (int)floorf((float)r->vbuf.height - (max_y + pad));
                int ceil_x = (int)ceilf(max_x + pad);
                int ceil_y = (int)ceilf((float)r->vbuf.height - (min_y - pad));

                int vw = ceil_x - vx;
                int vh = ceil_y - vy;

                if (vx < 0)
                {
                    vw += vx;
                    vx = 0;
                }
                if (vy < 0)
                {
                    vh += vy;
                    vy = 0;
                }
                if (vx + vw > r->vbuf.width)
                    vw = r->vbuf.width - vx;
                if (vy + vh > r->vbuf.height)
                    vh = r->vbuf.height - vy;

                if (vw > 0 && vh > 0)
                {
                    blur_pool_ensure(vw, vh);

                    /* 抓取屏幕图像 */
                    glBindTexture(GL_TEXTURE_2D, g_blur_pool.src_tex);
                    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vx, vy, vw, vh);
                    glBindTexture(GL_TEXTURE_2D, 0);

                    GLint saved_viewport[4];
                    glGetIntegerv(GL_VIEWPORT, saved_viewport);

                    /* 双轮多级高斯弥散 */
                    GLuint blurred_tex = run_fast_gaussian_blur(g_blur_pool.src_tex, vw, vh, blur);

                    glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);

                    glEnable(GL_TEXTURE_2D);
                    glBindTexture(GL_TEXTURE_2D, blurred_tex);
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

                    /* 基础色调乘法系数：invert(0.2) 对应乘以 0.6，hue-rotate 对应乘以 0.45 */
                    if (fabsf(invert - 0.5f) < 0.01f)
                        glColor4f(0.40f, 0.65f, 1.0f, 1.0f);
                    else if (invert > 0.0f)
                        glColor4f(0.60f, 0.60f, 0.60f, 1.0f);
                    else
                        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

                    /* 贴回完全化开的弥散模糊纹理 */
                    if (has_rounded)
                        draw_rounded_corners_poly_tex_outset(c->x, c->y, c->w, c->h, rads, (float)vx, (float)vy, (float)vw, (float)vh, (float)r->vbuf.height, m);
                    else
                    {
                        glBegin(GL_QUADS);
#define EMIT_BACKDROP_UV(px_val, py_val)                                  \
    do                                                                    \
    {                                                                     \
        float _sx = (px_val) * m[0] + (py_val) * m[4] + m[12];            \
        float _sy = (px_val) * m[1] + (py_val) * m[5] + m[13];            \
        float _u = (_sx - (float)vx) / (float)vw;                         \
        float _v = ((float)r->vbuf.height - _sy - (float)vy) / (float)vh; \
        glTexCoord2f(_u, _v);                                             \
        glVertex2f((px_val), (py_val));                                   \
    } while (0)

                        EMIT_BACKDROP_UV(c->x, c->y);
                        EMIT_BACKDROP_UV(c->x + c->w, c->y);
                        EMIT_BACKDROP_UV(c->x + c->w, c->y + c->h);
                        EMIT_BACKDROP_UV(c->x, c->y + c->h);
#undef EMIT_BACKDROP_UV
                        glEnd();
                    }

                    /* ========================================================== */
                    /* 2. 注入核心“物理磨砂/噪点”层 (Noise Grain)                     */
                    /* 浏览器及 macOS 的亚克力质感精髓在于必须有一层极微弱的高频噪点     */
                    /* ========================================================== */
                    glEnable(GL_TEXTURE_2D);
                    glBindTexture(GL_TEXTURE_2D, g_blur_pool.noise_tex);

                    /* 混合比例：4.5% 透明度的噪点即可完美模拟真实喷砂玻璃表面的微小颗粒 */
                    glColor4f(1.0f, 1.0f, 1.0f, 0.045f);

                    if (has_rounded)
                    {
                        glBegin(GL_TRIANGLE_FAN);
                        float cx = c->x + c->w * 0.5f;
                        float cy = c->y + c->h * 0.5f;
                        /* 使用绝对世界坐标 / 128 平铺，保证噪点密度恒定，拉伸框体时颗粒不发生形变放大 */
                        glTexCoord2f(cx / 128.0f, cy / 128.0f);
                        glVertex2f(cx, cy);

                        int segs = 16;
#define EMIT_NOISE_VERTEX(px, py)                   \
    do                                              \
    {                                               \
        glTexCoord2f((px) / 128.0f, (py) / 128.0f); \
        glVertex2f((px), (py));                     \
    } while (0)

                        if (r_tl > 0.5f)
                        {
                            for (int i = 0; i <= segs; i++)
                            {
                                float a = PI + (i * PI / (2.0f * segs));
                                EMIT_NOISE_VERTEX(c->x + r_tl + cosf(a) * r_tl, c->y + r_tl + sinf(a) * r_tl);
                            }
                        }
                        else
                        {
                            EMIT_NOISE_VERTEX(c->x, c->y);
                        }

                        if (r_tr > 0.5f)
                        {
                            for (int i = 0; i <= segs; i++)
                            {
                                float a = 1.5f * PI + (i * PI / (2.0f * segs));
                                EMIT_NOISE_VERTEX(c->x + c->w - r_tr + cosf(a) * r_tr, c->y + r_tr + sinf(a) * r_tr);
                            }
                        }
                        else
                        {
                            EMIT_NOISE_VERTEX(c->x + c->w, c->y);
                        }

                        if (r_br > 0.5f)
                        {
                            for (int i = 0; i <= segs; i++)
                            {
                                float a = 0.0f + (i * PI / (2.0f * segs));
                                EMIT_NOISE_VERTEX(c->x + c->w - r_br + cosf(a) * r_br, c->y + c->h - r_br + sinf(a) * r_br);
                            }
                        }
                        else
                        {
                            EMIT_NOISE_VERTEX(c->x + c->w, c->y + c->h);
                        }

                        if (r_bl > 0.5f)
                        {
                            for (int i = 0; i <= segs; i++)
                            {
                                float a = 0.5f * PI + (i * PI / (2.0f * segs));
                                EMIT_NOISE_VERTEX(c->x + r_bl + cosf(a) * r_bl, c->y + c->h - r_bl + sinf(a) * r_bl);
                            }
                        }
                        else
                        {
                            EMIT_NOISE_VERTEX(c->x, c->y + c->h);
                        }

                        if (r_tl > 0.5f)
                        {
                            float a = PI;
                            EMIT_NOISE_VERTEX(c->x + r_tl + cosf(a) * r_tl, c->y + r_tl + sinf(a) * r_tl);
                        }
                        else
                        {
                            EMIT_NOISE_VERTEX(c->x, c->y);
                        }
                        glEnd();
#undef EMIT_NOISE_VERTEX
                    }
                    else
                    {
                        glBegin(GL_QUADS);
                        glTexCoord2f(c->x / 128.0f, c->y / 128.0f);
                        glVertex2f(c->x, c->y);
                        glTexCoord2f((c->x + c->w) / 128.0f, c->y / 128.0f);
                        glVertex2f(c->x + c->w, c->y);
                        glTexCoord2f((c->x + c->w) / 128.0f, (c->y + c->h) / 128.0f);
                        glVertex2f(c->x + c->w, c->y + c->h);
                        glTexCoord2f(c->x / 128.0f, (c->y + c->h) / 128.0f);
                        glVertex2f(c->x, c->y + c->h);
                        glEnd();
                    }

                    glBindTexture(GL_TEXTURE_2D, 0);
                    glDisable(GL_TEXTURE_2D);
                }
            }
        }

        /* 复合滤镜：通过加法混合严格实现 CSS invert(0.2) 的 +0.2 磨砂底灰与淡蓝光晕 */
        if (invert > 0.0f)
        {
            glEnable(GL_BLEND);
            if (fabsf(invert - 0.5f) < 0.01f)
            {
                /* hue-rotate(180deg): 呈现图 2 中柔和的淡紫蓝弥散光 */
                glBlendFunc(GL_ONE, GL_ONE);
                glColor4f(0.18f, 0.28f, 0.58f, 1.0f);
            }
            else
            {
                /* invert(0.2): 将全黑背景提升为高档通透磨砂灰（图 2 质感） */
                glBlendFunc(GL_ONE, GL_ONE);
                glColor4f(0.20f, 0.20f, 0.20f, 1.0f);
            }

            if (has_rounded)
                draw_rounded_corners_poly(c->x, c->y, c->w, c->h, rads);
            else
            {
                glBegin(GL_QUADS);
                glVertex2f(c->x, c->y);
                glVertex2f(c->x + c->w, c->y);
                glVertex2f(c->x + c->w, c->y + c->h);
                glVertex2f(c->x, c->y + c->h);
                glEnd();
            }
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        break;
    }
    case 21: /* DIFFERENCE BLEND MODE (硬件模拟 Difference 混合) */
    {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE); /* 必须保护目的地 Alpha 不被 Difference 破坏 */
        glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_COLOR);
        float rads[4] = {c->u0, c->v0, c->u1, c->v1};
        int has_rounded = (c->u0 > 0 || c->v0 > 0 || c->u1 > 0 || c->v1 > 0);

        /* 核心修复：要使 Difference 在 OpenGL 遗留管线支持 Alpha 半透透明，必须预乘 Alpha */
        glColor4f(c->r * c->a, c->g * c->a, c->b * c->a, c->a);

        if (has_rounded)
            draw_rounded_corners_poly(c->x, c->y, c->w, c->h, rads);
        else
        {
            glBegin(GL_QUADS);
            glVertex2f(c->x, c->y);
            glVertex2f(c->x + c->w, c->y);
            glVertex2f(c->x + c->w, c->y + c->h);
            glVertex2f(c->x, c->y + c->h);
            glEnd();
        }
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); /* 恢复 Alpha 写入 */
        break;
    }
    default:
        break;
    }
}

/* Forward decl: mini_push lives with the vector command API below, but the
   convenience drawers above record commands through it.                  */
static inline int mini_push(MiniRenderer *r, MiniCmd c);

/* Draws a rectangle with rounded corners */
void mini_draw_rect_rounded(MiniRenderer *r, float x, float y, float w, float h,
                            float radius, float cr, float cg, float cb, float ca)
{
    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = MINI_CMD_RECT_ROUNDED;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    c.r = cr;
    c.g = cg;
    c.b = cb;
    c.a = ca;
    c.u0 = radius; /* Pass radius via u0 */
    mini_push(r, c);
}

/* Draws a soft drop shadow via geometry fading */
void mini_draw_shadow(MiniRenderer *r, float x, float y, float w, float h,
                      float radius, float spread, float blur,
                      float cr, float cg, float cb, float ca)
{
    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = MINI_CMD_SHADOW;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    c.r = cr;
    c.g = cg;
    c.b = cb;
    c.a = ca;
    c.u0 = radius;
    c.v0 = spread;
    c.u1 = blur; /* Packing vars into unused UV fields */
    mini_push(r, c);
}

/* Draws a smooth linear gradient rectangle */
void mini_draw_gradient(MiniRenderer *r, float x, float y, float w, float h,
                        float r1, float g1, float b1, float a1,
                        float r2, float g2, float b2, float a2, int type, float angle)
{
    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = MINI_CMD_GRADIENT;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    c.r = r1;
    c.g = g1;
    c.b = b1;
    c.a = a1;
    c.u0 = r2;
    c.v0 = g2;
    c.u1 = b2;
    c.v1 = a2;                                                 /* Second color packed in UV fields */
    c.reserved = type;                                         /* 0=linear 1=radial 2=conic            */
    c.font_id = (uint16_t)mini_gradient_norm_angle_deg(angle); /* 0..359 */
    c.texture_id = 0;                                          /* no corner radius (sharp box)         */
    mini_push(r, c);
}

void mini_renderer_flush(MiniRenderer *r)
{
    /* Simple state sort: keep CLEAR first, then draw in z order as
       recorded. A production build keys the sort on (texture_id, blend)
       to minimize bind toggles. */
    for (uint32_t i = 0; i < r->vbuf.count; i++)
        mini_exec_cmd(r, &r->vbuf.cmds[i]);
    r->vbuf.count = 0;
}

void mini_renderer_end_frame(MiniRenderer *r)
{
    /* poll happens in the main loop; swap here. */
    glfwSwapBuffers((GLFWwindow *)r->gpu.window_handle);
}

void mini_renderer_restore_webgl(MiniRenderer *r)
{
    /* Undo the bridge_reset_2d state-zeroing (program/VAO/buffer/texture 0)
       done in begin_frame so the legacy glBegin/glEnd DOM pass could run. Must
       be called after mini_renderer_flush and before mini_bridge_fire_raf, or
       WebGL apps that cache GL state (Three.js) draw with no program/VAO ->
       no fragments. */
    if (r && r->gl_state)
        mini_gl_bridge_restore_webgl(r->gl_state);
}

/* ---- overflow:hidden clip stack (nested, intersected) --------------- */
/* Single-threaded renderer: a static stack of clip rects is fine. Each
   push intersects with the current top so nested overflow boxes nest;
   pop restores the previous rect (or disables scissor when empty).    */
static void apply_clip(MiniRenderer *r)
{
    if (!r || r->clip_top <= 0)
    {
        glDisable(GL_SCISSOR_TEST);
        return;
    }

    float x = r->clip[r->clip_top - 1].x;
    float y = r->clip[r->clip_top - 1].y;
    float w = r->clip[r->clip_top - 1].w;
    float h = r->clip[r->clip_top - 1].h;

    if (w < 0)
        w = 0;
    if (h < 0)
        h = 0;

    GLfloat m[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, m);

    float px[4] = {x, x + w, x + w, x};
    float py[4] = {y, y, y + h, y + h};
    float min_x = 1e9f, max_x = -1e9f;
    float min_y = 1e9f, max_y = -1e9f;

    for (int i = 0; i < 4; i++)
    {
        float sx = px[i] * m[0] + py[i] * m[4] + m[12];
        float sy = px[i] * m[1] + py[i] * m[5] + m[13];
        if (sx < min_x)
            min_x = sx;
        if (sx > max_x)
            max_x = sx;
        if (sy < min_y)
            min_y = sy;
        if (sy > max_y)
            max_y = sy;
    }
    int sc_y = (int)((float)r->vbuf.height - max_y);
    if (sc_y < 0)
        sc_y = 0;

    glEnable(GL_SCISSOR_TEST);
    glScissor((int)min_x, sc_y, (int)(max_x - min_x), (int)(max_y - min_y));
}

static void draw_rounded_corners_poly(float x, float y, float w, float h, const float r[4])
{
    float r_tl = r ? r[0] : 0, r_tr = r ? r[1] : 0, r_br = r ? r[2] : 0, r_bl = r ? r[3] : 0;
    float max_r = (w < h ? w : h) * 0.5f;
    if (r_tl > max_r)
        r_tl = max_r;
    if (r_tr > max_r)
        r_tr = max_r;
    if (r_br > max_r)
        r_br = max_r;
    if (r_bl > max_r)
        r_bl = max_r;
    if (r_tl < 0)
        r_tl = 0;
    if (r_tr < 0)
        r_tr = 0;
    if (r_br < 0)
        r_br = 0;
    if (r_bl < 0)
        r_bl = 0;

    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(x + w * 0.5f, y + h * 0.5f); /* Center Anchor Vertex */

    int segs = 16;
    /* Top-Left */
    if (r_tl > 0.5f)
    {
        for (int i = 0; i <= segs; i++)
        {
            float a = PI + (i * PI / (2.0f * segs));
            glVertex2f(x + r_tl + cosf(a) * r_tl, y + r_tl + sinf(a) * r_tl);
        }
    }
    else
    {
        glVertex2f(x, y);
    }
    /* Top-Right */
    if (r_tr > 0.5f)
    {
        for (int i = 0; i <= segs; i++)
        {
            float a = 1.5f * PI + (i * PI / (2.0f * segs));
            glVertex2f(x + w - r_tr + cosf(a) * r_tr, y + r_tr + sinf(a) * r_tr);
        }
    }
    else
    {
        glVertex2f(x + w, y);
    }
    /* Bottom-Right */
    if (r_br > 0.5f)
    {
        for (int i = 0; i <= segs; i++)
        {
            float a = 0.0f + (i * PI / (2.0f * segs));
            glVertex2f(x + w - r_br + cosf(a) * r_br, y + h - r_br + sinf(a) * r_br);
        }
    }
    else
    {
        glVertex2f(x + w, y + h);
    }
    /* Bottom-Left */
    if (r_bl > 0.5f)
    {
        for (int i = 0; i <= segs; i++)
        {
            float a = 0.5f * PI + (i * PI / (2.0f * segs));
            glVertex2f(x + r_bl + cosf(a) * r_bl, y + h - r_bl + sinf(a) * r_bl);
        }
    }
    else
    {
        glVertex2f(x, y + h);
    }
    /* Close the Fan loop strictly to the first perimeter vertex */
    if (r_tl > 0.5f)
    {
        float a = PI + (0 * PI / (2.0f * segs));
        glVertex2f(x + r_tl + cosf(a) * r_tl, y + r_tl + sinf(a) * r_tl);
    }
    else
    {
        glVertex2f(x, y);
    }
    glEnd();
}

static void draw_rounded_corners_poly_tex(float x, float y, float w, float h, const float r[4], float du, float dv)
{
    float r_tl = r ? r[0] : 0, r_tr = r ? r[1] : 0, r_br = r ? r[2] : 0, r_bl = r ? r[3] : 0;
    float max_r = (w < h ? w : h) * 0.5f;
    if (r_tl > max_r)
        r_tl = max_r;
    if (r_tr > max_r)
        r_tr = max_r;
    if (r_br > max_r)
        r_br = max_r;
    if (r_bl > max_r)
        r_bl = max_r;
    if (r_tl < 0)
        r_tl = 0;
    if (r_tr < 0)
        r_tr = 0;
    if (r_br < 0)
        r_br = 0;
    if (r_bl < 0)
        r_bl = 0;

    glBegin(GL_TRIANGLE_FAN);
    glTexCoord2f(0.5f + du, 0.5f + dv);
    glVertex2f(x + w * 0.5f, y + h * 0.5f);

    int segs = 16;
    if (r_tl > 0.5f)
    {
        for (int i = 0; i <= segs; i++)
        {
            float a = PI + (i * PI / (2.0f * segs));
            float px = x + r_tl + cosf(a) * r_tl;
            float py = y + r_tl + sinf(a) * r_tl;
            glTexCoord2f((px - x) / w + du, 1.0f - (py - y) / h + dv);
            glVertex2f(px, py);
        }
    }
    else
    {
        glTexCoord2f(0.0f + du, 1.0f + dv);
        glVertex2f(x, y);
    }
    if (r_tr > 0.5f)
    {
        for (int i = 0; i <= segs; i++)
        {
            float a = 1.5f * PI + (i * PI / (2.0f * segs));
            float px = x + w - r_tr + cosf(a) * r_tr;
            float py = y + r_tr + sinf(a) * r_tr;
            glTexCoord2f((px - x) / w + du, 1.0f - (py - y) / h + dv);
            glVertex2f(px, py);
        }
    }
    else
    {
        glTexCoord2f(1.0f + du, 1.0f + dv);
        glVertex2f(x + w, y);
    }
    if (r_br > 0.5f)
    {
        for (int i = 0; i <= segs; i++)
        {
            float a = 0.0f + (i * PI / (2.0f * segs));
            float px = x + w - r_br + cosf(a) * r_br;
            float py = y + h - r_br + sinf(a) * r_br;
            glTexCoord2f((px - x) / w + du, 1.0f - (py - y) / h + dv);
            glVertex2f(px, py);
        }
    }
    else
    {
        glTexCoord2f(1.0f + du, 0.0f + dv);
        glVertex2f(x + w, y + h);
    }
    if (r_bl > 0.5f)
    {
        for (int i = 0; i <= segs; i++)
        {
            float a = 0.5f * PI + (i * PI / (2.0f * segs));
            float px = x + r_bl + cosf(a) * r_bl;
            float py = y + h - r_bl + sinf(a) * r_bl;
            glTexCoord2f((px - x) / w + du, 1.0f - (py - y) / h + dv);
            glVertex2f(px, py);
        }
    }
    else
    {
        glTexCoord2f(0.0f + du, 0.0f + dv);
        glVertex2f(x, y + h);
    }
    if (r_tl > 0.5f)
    {
        float a = PI + (0 * PI / (2.0f * segs));
        float px = x + r_tl + cosf(a) * r_tl;
        float py = y + r_tl + sinf(a) * r_tl;
        glTexCoord2f((px - x) / w + du, 1.0f - (py - y) / h + dv);
        glVertex2f(px, py);
    }
    else
    {
        glTexCoord2f(0.0f + du, 1.0f + dv);
        glVertex2f(x, y);
    }
    glEnd();
}

static void draw_rounded_corners_poly_tex_outset(float x, float y, float w, float h, const float r[4],
                                                 float vx, float vy, float vw, float vh, float vbuf_h, const GLfloat m[16])
{
    float r_tl = r ? r[0] : 0, r_tr = r ? r[1] : 0, r_br = r ? r[2] : 0, r_bl = r ? r[3] : 0;
    float max_r = (w < h ? w : h) * 0.5f;
    if (r_tl > max_r)
        r_tl = max_r;
    if (r_tr > max_r)
        r_tr = max_r;
    if (r_br > max_r)
        r_br = max_r;
    if (r_bl > max_r)
        r_bl = max_r;
    if (r_tl < 0)
        r_tl = 0;
    if (r_tr < 0)
        r_tr = 0;
    if (r_br < 0)
        r_br = 0;
    if (r_bl < 0)
        r_bl = 0;

#define EMIT_OUTSET_VERTEX(px, py)                     \
    do                                                 \
    {                                                  \
        float _sx = (px) * m[0] + (py) * m[4] + m[12]; \
        float _sy = (px) * m[1] + (py) * m[5] + m[13]; \
        float _u = (_sx - vx) / vw;                    \
        float _v = (vbuf_h - _sy - vy) / vh;           \
        glTexCoord2f(_u, _v);                          \
        glVertex2f((px), (py));                        \
    } while (0)

    glBegin(GL_TRIANGLE_FAN);
    EMIT_OUTSET_VERTEX(x + w * 0.5f, y + h * 0.5f);

    int segs = 16;
    if (r_tl > 0.5f)
    {
        for (int i = 0; i <= segs; i++)
        {
            float a = PI + (i * PI / (2.0f * segs));
            EMIT_OUTSET_VERTEX(x + r_tl + cosf(a) * r_tl, y + r_tl + sinf(a) * r_tl);
        }
    }
    else
    {
        EMIT_OUTSET_VERTEX(x, y);
    }

    if (r_tr > 0.5f)
    {
        for (int i = 0; i <= segs; i++)
        {
            float a = 1.5f * PI + (i * PI / (2.0f * segs));
            EMIT_OUTSET_VERTEX(x + w - r_tr + cosf(a) * r_tr, y + r_tr + sinf(a) * r_tr);
        }
    }
    else
    {
        EMIT_OUTSET_VERTEX(x + w, y);
    }

    if (r_br > 0.5f)
    {
        for (int i = 0; i <= segs; i++)
        {
            float a = 0.0f + (i * PI / (2.0f * segs));
            EMIT_OUTSET_VERTEX(x + w - r_br + cosf(a) * r_br, y + h - r_br + sinf(a) * r_br);
        }
    }
    else
    {
        EMIT_OUTSET_VERTEX(x + w, y + h);
    }

    if (r_bl > 0.5f)
    {
        for (int i = 0; i <= segs; i++)
        {
            float a = 0.5f * PI + (i * PI / (2.0f * segs));
            EMIT_OUTSET_VERTEX(x + r_bl + cosf(a) * r_bl, y + h - r_bl + sinf(a) * r_bl);
        }
    }
    else
    {
        EMIT_OUTSET_VERTEX(x, y + h);
    }

    if (r_tl > 0.5f)
    {
        float a = PI;
        EMIT_OUTSET_VERTEX(x + r_tl + cosf(a) * r_tl, y + r_tl + sinf(a) * r_tl);
    }
    else
    {
        EMIT_OUTSET_VERTEX(x, y);
    }

    glEnd();
#undef EMIT_OUTSET_VERTEX
}

static void stencil_rounded(MiniRenderer *r, float x, float y, float w, float h, float radius)
{
    (void)r;
    float r4[4] = {radius, radius, radius, radius};
    draw_rounded_corners_poly(x, y, w, h, r4);
}

static void stencil_rounded_corners(MiniRenderer *r, float x, float y, float w, float h, const float radii[4])
{
    (void)r;
    draw_rounded_corners_poly(x, y, w, h, radii);
}

void mini_draw_backdrop_filter(MiniRenderer *r, float x, float y, float w, float h, float blur_radius, float invert, const float radii[4])
{
    if (!r || w <= 0 || h <= 0)
        return;
    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = 20;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    c.u0 = blur_radius;
    c.v0 = invert;
    if (radii)
    {
        c.u1 = radii[0];
        c.v1 = radii[1];
        c.r = radii[2];
        c.g = radii[3];
    }
    mini_push(r, c);
}

void mini_draw_rect_difference(MiniRenderer *r, float x, float y, float w, float h, const float radii[4], float cr, float cg, float cb, float ca)
{
    if (!r || w <= 0 || h <= 0)
        return;
    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = 21;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    c.r = cr;
    c.g = cg;
    c.b = cb;
    c.a = ca;
    if (radii)
    {
        c.u0 = radii[0];
        c.v0 = radii[1];
        c.u1 = radii[2];
        c.v1 = radii[3];
    }
    mini_push(r, c);
}

void mini_draw_rect_rounded_corners(MiniRenderer *r, float x, float y, float w, float h,
                                    const float radii[4], float cr, float cg, float cb, float ca)
{
    if (!r || w <= 0 || h <= 0)
        return;
    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = MINI_CMD_RECT_ROUNDED;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    c.r = cr;
    c.g = cg;
    c.b = cb;
    c.a = ca;
    if (radii)
    {
        c.u0 = radii[0];
        c.v0 = radii[1];
        c.u1 = radii[2];
        c.v1 = radii[3];
    }
    mini_push(r, c);
}

void mini_draw_polygon(MiniRenderer *r, const float *pts, int num_pts,
                       float cr, float cg, float cb, float ca)
{
    if (!r || !pts || num_pts < 3)
        return;
    float x0 = pts[0], y0 = pts[1];
    for (int i = 1; i < num_pts - 1; i++)
    {
        MiniCmd c;
        memset(&c, 0, sizeof c);
        c.type = MINI_CMD_TRIANGLE;
        c.x = x0;
        c.y = y0;
        c.w = pts[i * 2];
        c.h = pts[i * 2 + 1];
        c.u0 = pts[(i + 1) * 2];
        c.v0 = pts[(i + 1) * 2 + 1];
        c.r = cr;
        c.g = cg;
        c.b = cb;
        c.a = ca;
        mini_push(r, c);
    }
}

void mini_draw_polygon_stroke(MiniRenderer *r, const float *pts, int num_pts,
                              float line_w, float cr, float cg, float cb, float ca)
{
    if (!r || !pts || num_pts < 2)
        return;
    for (int i = 0; i < num_pts; i++)
    {
        int next = (i + 1) % num_pts;
        mini_draw_line(r, pts[i * 2], pts[i * 2 + 1], pts[next * 2], pts[next * 2 + 1],
                       line_w > 0.0f ? line_w : 1.0f, cr, cg, cb, ca);
    }
}

void mini_draw_shadow_corners(MiniRenderer *r, float x, float y, float w, float h,
                              const float radii[4], float spread, float blur,
                              float cr, float cg, float cb, float ca)
{
    if (!r)
        return;
    float avg_r = radii ? (radii[0] + radii[1] + radii[2] + radii[3]) * 0.25f : 0.0f;
    mini_draw_shadow(r, x, y, w, h, avg_r, spread, blur, cr, cg, cb, ca);
}

/* Simple background image texture cache */
typedef struct MiniImgCacheEntry
{
    char url[256];
    GLuint tex;
    int w, h;
} MiniImgCacheEntry;

#define MINI_IMG_CACHE_CAP 32

static MiniImgCacheEntry g_img_cache[MINI_IMG_CACHE_CAP];
static int g_img_cache_n = 0;

static void blur_pool_ensure(int w, int h)
{
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    if (!g_blur_pool.inited)
    {
        memset(&g_blur_pool, 0, sizeof(g_blur_pool));
        g_blur_pool.inited = 1;

        /* --- 核心修复 1：生成符合正态分布的高级“高斯噪点”贴图 --- */
        glGenTextures(1, &g_blur_pool.noise_tex);
        glBindTexture(GL_TEXTURE_2D, g_blur_pool.noise_tex);
        uint8_t noise[128 * 128];
        for (int i = 0; i < 128 * 128; i++)
        {
            /* 3次随机取平均，产生高斯(正态)分布，颗粒感如物理毛玻璃般细腻 */
            noise[i] = (uint8_t)(((rand() % 256) + (rand() % 256) + (rand() % 256)) / 3);
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 128, 128, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, noise);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    /* 1. 抓屏原图尺寸 */
    if (!g_blur_pool.src_tex)
        glGenTextures(1, &g_blur_pool.src_tex);
    if (g_blur_pool.src_w != w || g_blur_pool.src_h != h)
    {
        g_blur_pool.src_w = w;
        g_blur_pool.src_h = h;
        glBindTexture(GL_TEXTURE_2D, g_blur_pool.src_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    /* 2. 降采样 1/3 卷积缓冲 (fbo 0 & 1)，大范围扩散光晕 */
    int dw = w / 3;
    int dh = h / 3;
    if (dw < 32)
        dw = 32;
    if (dh < 32)
        dh = 32;

    for (int i = 0; i < 2; i++)
    {
        if (!g_blur_pool.fbo[i] && g_gl.GenFramebuffers)
            g_gl.GenFramebuffers(1, &g_blur_pool.fbo[i]);
        if (!g_blur_pool.tex[i])
            glGenTextures(1, &g_blur_pool.tex[i]);

        if (g_blur_pool.w[i] != dw || g_blur_pool.h[i] != dh)
        {
            g_blur_pool.w[i] = dw;
            g_blur_pool.h[i] = dh;
            glBindTexture(GL_TEXTURE_2D, g_blur_pool.tex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, dw, dh, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);

            if (g_gl.BindFramebuffer)
                g_gl.BindFramebuffer(GL_FRAMEBUFFER, g_blur_pool.fbo[i]);
            if (g_gl.FramebufferTexture2D)
                g_gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_blur_pool.tex[i], 0);
            if (g_gl.BindFramebuffer)
                g_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        }
    }

    /* 3. 升采样还原缓冲 (fbo 2)，保证全分辨率贴回 */
    if (!g_blur_pool.fbo[2] && g_gl.GenFramebuffers)
        g_gl.GenFramebuffers(1, &g_blur_pool.fbo[2]);
    if (!g_blur_pool.tex[2])
        glGenTextures(1, &g_blur_pool.tex[2]);
    if (g_blur_pool.w[2] != w || g_blur_pool.h[2] != h)
    {
        g_blur_pool.w[2] = w;
        g_blur_pool.h[2] = h;
        glBindTexture(GL_TEXTURE_2D, g_blur_pool.tex[2]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        if (g_gl.BindFramebuffer)
            g_gl.BindFramebuffer(GL_FRAMEBUFFER, g_blur_pool.fbo[2]);
        if (g_gl.FramebufferTexture2D)
            g_gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_blur_pool.tex[2], 0);
        if (g_gl.BindFramebuffer)
            g_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

static void gaussian_blur_pass_1d(GLuint src_tex, GLuint dst_fbo, int dst_w, int dst_h, float dir_x, float dir_y, float radius)
{
    if (g_gl.BindFramebuffer)
        g_gl.BindFramebuffer(GL_FRAMEBUFFER, dst_fbo);

    glViewport(0, 0, dst_w, dst_h);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, (double)dst_w, 0.0, (double)dst_h, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);

    /* 9-Tap 标准高斯钟形曲线权重 (总和严格为 1.00) */
    const float weights[9] = {0.06f, 0.09f, 0.12f, 0.15f, 0.16f, 0.15f, 0.12f, 0.09f, 0.06f};
    const float offsets[9] = {-4.0f, -3.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f, 4.0f};

    /* 核心修复：单步跨度为 radius / 4.0，确保 9 个抽头完美覆盖整个高斯弥散范围而不越界 */
    float step_val = radius * 0.28f;
    if (step_val > 5.5f)
        step_val = 3.5f; /* 强行封顶，超大模糊依赖外层 1/3 降采样已足够 */

    float step_x = (dir_x * step_val) / (float)dst_w;
    float step_y = (dir_y * step_val) / (float)dst_h;

    for (int t = 0; t < 9; t++)
    {
        float du = offsets[t] * step_x;
        float dv = offsets[t] * step_y;
        float w = weights[t];
        glColor4f(w, w, w, 1.0f);
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f + du, 0.0f + dv);
        glVertex2f(0.0f, 0.0f);
        glTexCoord2f(1.0f + du, 0.0f + dv);
        glVertex2f((float)dst_w, 0.0f);
        glTexCoord2f(1.0f + du, 1.0f + dv);
        glVertex2f((float)dst_w, (float)dst_h);
        glTexCoord2f(0.0f + du, 1.0f + dv);
        glVertex2f(0.0f, (float)dst_h);
        glEnd();
    }

    glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
}
static GLuint run_fast_gaussian_blur(GLuint src_tex, int src_w, int src_h, float blur_radius)
{
    int dw = g_blur_pool.w[0];
    int dh = g_blur_pool.h[0];
    float r = blur_radius > 0.0f ? blur_radius : 12.0f;

    /* 第 1 轮：大范围扩散 (src_tex -> FBO 0 -> FBO 1) */
    gaussian_blur_pass_1d(src_tex, g_blur_pool.fbo[0], dw, dh, 1.0f, 0.0f, r * 1.5f);
    gaussian_blur_pass_1d(g_blur_pool.tex[0], g_blur_pool.fbo[1], dw, dh, 0.0f, 1.0f, r * 1.5f);

    /* 第 2 轮：二次平滑消除阶梯感 (FBO 1 -> FBO 0 -> FBO 1) */
    gaussian_blur_pass_1d(g_blur_pool.tex[1], g_blur_pool.fbo[0], dw, dh, 1.0f, 0.0f, r * 0.9f);
    gaussian_blur_pass_1d(g_blur_pool.tex[0], g_blur_pool.fbo[1], dw, dh, 0.0f, 1.0f, r * 0.9f);

    /* 第 3 阶段：升采样还原至全分辨率缓冲 FBO 2 */
    if (g_gl.BindFramebuffer)
        g_gl.BindFramebuffer(GL_FRAMEBUFFER, g_blur_pool.fbo[2]);

    glViewport(0, 0, src_w, src_h);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, (double)src_w, 0.0, (double)src_h, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_blur_pool.tex[1]);
    glDisable(GL_BLEND);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f);
    glVertex2f(0.0f, 0.0f);
    glTexCoord2f(1.0f, 0.0f);
    glVertex2f((float)src_w, 0.0f);
    glTexCoord2f(1.0f, 1.0f);
    glVertex2f((float)src_w, (float)src_h);
    glTexCoord2f(0.0f, 1.0f);
    glVertex2f(0.0f, (float)src_h);
    glEnd();

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();

    if (g_gl.BindFramebuffer)
        g_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);

    glMatrixMode(GL_MODELVIEW);
    return g_blur_pool.tex[2];
}

static GLuint get_or_load_image_tex(const char *url, int *out_w, int *out_h)
{
    if (!url || !*url)
        return 0;
    for (int i = 0; i < g_img_cache_n; i++)
    {
        if (!strcmp(g_img_cache[i].url, url))
        {
            if (out_w)
                *out_w = g_img_cache[i].w;
            if (out_h)
                *out_h = g_img_cache[i].h;
            return g_img_cache[i].tex;
        }
    }

    extern MiniResource mini_dom_load_resource(struct MiniDocument * doc, const char *url_or_path, const char *type);
    extern void mini_dom_free_resource(MiniResource * res);
    MiniResource res = mini_dom_load_resource(NULL, url, "image");
    if (!res.data || res.size == 0)
        return 0;

    uint8_t *rgba = NULL;
    int iw = 0, ih = 0;
    if (mini_png_decode_rgba(res.data, res.size, &rgba, &iw, &ih) != 0 || !rgba)
    {
        mini_dom_free_resource(&res);
        return 0;
    }
    mini_dom_free_resource(&res);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, iw, ih, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    free(rgba);

    if (g_img_cache_n < MINI_IMG_CACHE_CAP)
    {
        snprintf(g_img_cache[g_img_cache_n].url, sizeof(g_img_cache[g_img_cache_n].url), "%s", url);
        g_img_cache[g_img_cache_n].tex = tex;
        g_img_cache[g_img_cache_n].w = iw;
        g_img_cache[g_img_cache_n].h = ih;
        g_img_cache_n++;
    }
    if (out_w)
        *out_w = iw;
    if (out_h)
        *out_h = ih;
    return tex;
}

void mini_draw_background_image(MiniRenderer *r, float x, float y, float w, float h,
                                const char *url, int size_mode,
                                float bg_w, float bg_h,
                                float pos_x, float pos_y, int repeat,
                                const float radii[4])
{
    (void)radii;
    (void)repeat;
    if (!r || !url || !*url || w <= 0 || h <= 0)
        return;
    int img_w = 0, img_h = 0;
    GLuint tex = get_or_load_image_tex(url, &img_w, &img_h);
    if (!tex || img_w <= 0 || img_h <= 0)
        return;

    float dw = (bg_w > 0) ? bg_w : (float)img_w;
    float dh = (bg_h > 0) ? bg_h : (float)img_h;
    if (size_mode == 1)
    { /* cover */
        float scale = (w / (float)img_w > h / (float)img_h) ? (w / (float)img_w) : (h / (float)img_h);
        dw = (float)img_w * scale;
        dh = (float)img_h * scale;
    }
    else if (size_mode == 2)
    { /* contain */
        float scale = (w / (float)img_w < h / (float)img_h) ? (w / (float)img_w) : (h / (float)img_h);
        dw = (float)img_w * scale;
        dh = (float)img_h * scale;
    }

    float u_scale = w / (dw > 0.0f ? dw : 1.0f);
    float v_scale = h / (dh > 0.0f ? dh : 1.0f);
    float u_off = -pos_x / (dw > 0.0f ? dw : 1.0f);
    float v_off = -pos_y / (dh > 0.0f ? dh : 1.0f);

    mini_draw_texture(r, x, y, w, h, tex, u_off, v_off, u_off + u_scale, v_off + v_scale);
}

void mini_draw_gradient_multi(MiniRenderer *r, float x, float y, float w, float h,
                              const void *stops, int num_stops,
                              int type, float angle, const float radii[4])
{
    (void)radii;
    if (!r || !stops || num_stops <= 0 || w <= 0 || h <= 0)
        return;
    int bw = (int)(w + 0.5f), bh = (int)(h + 0.5f);
    int rw, rh;
    grad_raster_dims(bw, bh, &rw, &rh);

    uint32_t tex = grad_cache_multi_get(r, type, angle, bw, bh, stops, num_stops, rw, rh);
    if (!tex)
        return;

    mini_draw_texture(r, x, y, w, h, tex, 0.0f, 0.0f, 1.0f, 1.0f);
}

void mini_renderer_push_clip(MiniRenderer *r, float x, float y, float w, float h)
{
    if (!r)
        return;
    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = MINI_CMD_PUSH_CLIP;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    mini_push(r, c);
}

void mini_renderer_push_rounded_clip(MiniRenderer *r, float x, float y, float w,
                                     float h, float radius)
{
    float r4[4] = {radius, radius, radius, radius};
    mini_renderer_push_rounded_clip_corners(r, x, y, w, h, r4);
}

void mini_renderer_push_rounded_clip_corners(MiniRenderer *r, float x, float y, float w,
                                             float h, const float radii[4])
{
    if (!r)
        return;
    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = MINI_CMD_PUSH_ROUNDED_CLIP;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    if (radii)
    {
        c.u0 = radii[0];
        c.v0 = radii[1];
        c.u1 = radii[2];
        c.v1 = radii[3];
    }
    mini_push(r, c);
}

void mini_renderer_pop_clip(MiniRenderer *r)
{
    if (!r)
        return;
    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = MINI_CMD_POP_CLIP;
    mini_push(r, c);
}

/* Capture the framebuffer as a PNG (Page.captureScreenshot). Reads the
   front buffer (visible surface) so the last swapped frame is captured
   even when called between frames. */
int mini_renderer_screenshot_png(MiniRenderer *r, uint8_t **out, size_t *out_len)
{
    if (!r || !out || !out_len || !r->gpu.window_handle)
        return -1;
    *out = NULL;
    *out_len = 0;
    int w = r->gpu.width, h = r->gpu.height;
    if (w <= 0 || h <= 0)
        return -1;

    glReadBuffer(GL_FRONT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    uint8_t *buf = (uint8_t *)malloc((size_t)w * h * 4);
    if (!buf)
        return -1;
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);

    /* GL origin is bottom-left; PNG rows are top-to-bottom -> flip Y. */
    uint8_t *flip = (uint8_t *)malloc((size_t)w * h * 4);
    if (!flip)
    {
        free(buf);
        return -1;
    }
    size_t row = (size_t)w * 4;
    for (int y = 0; y < h; y++)
        memcpy(flip + (size_t)y * row, buf + (size_t)(h - 1 - y) * row, row);
    free(buf);

    int rc = mini_png_encode_rgba(flip, w, h, out, out_len);
    free(flip);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Vector command API (the "2D Canvas" backend)                       */
/* ------------------------------------------------------------------ */
static inline int mini_push(MiniRenderer *r, MiniCmd c)
{
    if (r->vbuf.count >= MINI_CMD_CAP)
        mini_renderer_flush(r);
    r->vbuf.cmds[r->vbuf.count++] = c;
    return 0;
}

void mini_draw_clear(MiniRenderer *r, float cr, float cg, float cb, float ca)
{
    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = MINI_CMD_CLEAR;
    c.r = cr;
    c.g = cg;
    c.b = cb;
    c.a = ca;
    mini_push(r, c);
}

void mini_draw_rect(MiniRenderer *r, float x, float y, float w, float h,
                    float cr, float cg, float cb, float ca)
{
    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = MINI_CMD_RECT;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    c.r = cr;
    c.g = cg;
    c.b = cb;
    c.a = ca;
    mini_push(r, c);
}

void mini_draw_texture(MiniRenderer *r, float x, float y, float w, float h,
                       uint32_t tex, float u0, float v0, float u1, float v1)
{
    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = MINI_CMD_TEXTURE_RECT;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    c.texture_id = tex;
    c.u0 = u0;
    c.v0 = v0;
    c.u1 = u1;
    c.v1 = v1;
    c.a = 1.0f;
    mini_push(r, c);
}

/* Record a glyph run: append the quads to the side-buffer and emit a
   GLYPH_RUN command that the flush path rasterizes as one textured quad
   strip bound to the shared atlas. (x,y) is the run origin; each quad's
   position is the origin + its pen offset. */
void mini_draw_glyph_run(MiniRenderer *r, float x, float y,
                         const MiniGlyphQuad *quads, int n,
                         float cr, float cg, float cb, float ca,
                         uint16_t font_id)
{
    if (!r || !quads || n <= 0)
        return;
    if (r->glyph_n + n > r->glyph_cap)
    {
        int nc = r->glyph_cap ? r->glyph_cap : 64;
        while (nc < r->glyph_n + n)
            nc *= 2;
        MiniGlyphQuad *nq = (MiniGlyphQuad *)realloc(r->glyph_quads, (size_t)nc * sizeof(MiniGlyphQuad));
        if (!nq)
            return;
        r->glyph_quads = nq;
        r->glyph_cap = nc;
    }
    int start = r->glyph_n;
    for (int i = 0; i < n; i++)
        r->glyph_quads[r->glyph_n++] = quads[i];

    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = MINI_CMD_GLYPH_RUN;
    c.x = x;
    c.y = y;
    c.r = cr;
    c.g = cg;
    c.b = cb;
    c.a = ca;
    c.font_id = font_id;
    c.reserved = start;
    c.glyph_count = (uint16_t)n;
    mini_push(r, c);
}

/* ------------------------------------------------------------------ */
/* Glyph cache blitter �?the place the SIMD blend actually runs.       */
/* Composites one sub-rect (a glyph bitmap) into the CPU-side atlas    */
/* buffer in premultiplied form, then uploads that one dirty region.  */
/* This is CPU compositing; the GL flush only sees the final texture.  */
/* ------------------------------------------------------------------ */
void mini_atlas_blit_glyph(MiniRenderer *r,
                           int dst_x, int dst_y, int gw, int gh,
                           const uint32_t *src_rgba)
{
    if (!r || !src_rgba || gw <= 0 || gh <= 0 || !r->atlas_cpu)
        return;
    /* Source rows are `gw` pixels apart (the original glyph width); the
       clip only narrows how many pixels per row / how many rows we
       actually composite, not the source stride. */
    int stride = gw;
    int xoff = 0, yoff = 0;
    if (dst_x < 0)
    {
        xoff = -dst_x;
        dst_x = 0;
    }
    if (dst_y < 0)
    {
        yoff = -dst_y;
        dst_y = 0;
    }
    int cw = gw - xoff;
    if (dst_x + cw > r->atlas_w)
        cw = r->atlas_w - dst_x;
    int ch = gh - yoff;
    if (dst_y + ch > r->atlas_h)
        ch = r->atlas_h - dst_y;
    if (cw <= 0 || ch <= 0)
        return;

    /* composite src OVER the CPU mirror's existing pixels, premultiplied.
       The mirror IS the authoritative atlas content (kept in sync on
       every blit), so there is no glGetTexImage readback to pay for. */
    const uint32_t *sbase = src_rgba + (size_t)yoff * stride + (size_t)xoff;
    for (int y = 0; y < ch; y++)
    {
        const uint32_t *s = sbase + (size_t)y * stride;
        uint32_t *d = r->atlas_cpu + (size_t)(dst_y + y) * r->atlas_w + dst_x;
        mini_blend_row(s, d, (size_t)cw);
    }

    /* upload the dirty sub-rect to the GL texture */
    glBindTexture(GL_TEXTURE_2D, r->atlas_texture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, r->atlas_w);
    glTexSubImage2D(GL_TEXTURE_2D, 0, dst_x, dst_y, cw, ch,
                    GL_RGBA, GL_UNSIGNED_BYTE,
                    r->atlas_cpu + (size_t)dst_y * r->atlas_w + dst_x);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

/* ================================================================== */
/* Phase 2: placeholder + text primitives                               */
/* Each maps to one MiniCmd. The DOM render dispatch (mini_dom.c) calls */
/* these to draw a representative glyph/box per element category �?    */
/* media (img/video/audio), form controls (input/button/select/progress),*/
/* tables, interactive (details/dialog), and real text content through  */
/* the built-in font.                                                  */
/* ================================================================== */
void mini_draw_rect_stroke(MiniRenderer *r, float x, float y, float w, float h,
                           float line_w, float cr, float cg, float cb, float ca)
{
    if (!r)
        return;
    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = MINI_CMD_RECT_STROKE;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    c.r = cr;
    c.g = cg;
    c.b = cb;
    c.a = ca;
    c.reserved = (line_w > 0 && line_w < 0x7fffffff) ? (int)line_w : 1;
    mini_push(r, c);
}

void mini_draw_line(MiniRenderer *r, float x1, float y1, float x2, float y2,
                    float line_w, float cr, float cg, float cb, float ca)
{
    if (!r)
        return;
    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = MINI_CMD_LINE;
    c.x = x1;
    c.y = y1;
    c.w = x2;
    c.h = y2; /* p1=(x,y), p2=(w,h) */
    c.r = cr;
    c.g = cg;
    c.b = cb;
    c.a = ca;
    c.reserved = (line_w > 0 && line_w < 0x7fffffff) ? (int)line_w : 1;
    mini_push(r, c);
}

void mini_draw_triangle(MiniRenderer *r, float x1, float y1,
                        float x2, float y2, float x3, float y3,
                        float cr, float cg, float cb, float ca)
{
    if (!r)
        return;
    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = MINI_CMD_TRIANGLE;
    c.x = x1;
    c.y = y1; /* p1 */
    c.w = x2;
    c.h = y2; /* p2 */
    c.u0 = x3;
    c.v0 = y3; /* p3 */
    c.r = cr;
    c.g = cg;
    c.b = cb;
    c.a = ca;
    mini_push(r, c);
}

void mini_draw_circle(MiniRenderer *r, float cx, float cy, float radius,
                      float cr, float cg, float cb, float ca)
{
    if (!r || radius <= 0)
        return;
    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = MINI_CMD_CIRCLE;
    c.x = cx;
    c.y = cy;
    c.w = radius; /* center=(x,y), r=w */
    c.r = cr;
    c.g = cg;
    c.b = cb;
    c.a = ca;
    c.reserved = 0; /* 0 �?default 16 segs */
    mini_push(r, c);
}

void mini_draw_text_styled(MiniRenderer *r, float x, float y, const char *text,
                           float font_size, float cr, float cg, float cb, float ca, float ls, int font_style)
{
    if (!r || !text || !*text || font_size <= 0)
        return;
    font_init();
    if (r->ttf_loaded && r->font_ctx)
    {
        float penx = 0.0f;
        stbtt_fontinfo *info = (stbtt_fontinfo *)r->font_ctx;
        int bucket = tt_size_bucket(font_size);
        float fs = tt_bucket_to_size(bucket);
        float scale = stbtt_ScaleForPixelHeight(info, fs);
        float scaled_ascent = (float)r->font_ascent * scale;
        float baseline = floorf(y + (scaled_ascent > 0.0f ? scaled_ascent : fs * 0.75f) + 0.5f);
        const char *p = text;
        int blen = 0;
        uint16_t fid = (uint16_t)(1 | (font_style ? 0x100 : 0));
        while (*p)
        {
            unsigned int cp = utf8_next(&p, &blen);
            if (!blen)
                break;
            if (cp == '\n' || cp == '\r')
                continue;

            if (cp == 0xFE0F)
                continue; // 变体选择器剔除

            TtGlyph *g = tt_get_glyph(r, cp, fs);
            if (!g)
            {
                penx += FONT_ADV(fs) + ls;
                continue;
            }
            if (g->gw > 0 && g->gh > 0)
            {
                MiniGlyphQuad q;
                q.dx = penx + (float)g->xoff;
                q.dy = baseline + (float)g->yoff - y;
                q.gw = (float)g->gw;
                q.gh = (float)g->gh;
                float aw = (float)r->atlas_w, ah = (float)r->atlas_h;
                q.u0 = (float)g->atlas_x / aw;
                q.v0 = (float)g->atlas_y / ah;
                q.u1 = (float)(g->atlas_x + g->gw) / aw;
                q.v1 = (float)(g->atlas_y + g->gh) / ah;
                mini_draw_glyph_run(r, floorf(x + 0.5f), floorf(y + 0.5f), &q, 1, cr, cg, cb, ca, fid);
            }
            penx += g->advance + ls;
        }
        return;
    }

    /* Bitmap font fallback */
    int len = (int)strlen(text);
    if (r->text_n + len + 1 > r->text_cap)
    {
        int nc = r->text_cap ? r->text_cap : 256;
        while (nc < r->text_n + len + 1)
            nc *= 2;
        char *nb = (char *)realloc(r->text_store, (size_t)nc);
        if (!nb)
            return;
        r->text_store = nb;
        r->text_cap = nc;
    }
    int start = r->text_n;
    memcpy(r->text_store + start, text, (size_t)len);
    r->text_store[start + len] = 0;
    r->text_n += len + 1;

    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = MINI_CMD_TEXT;
    c.x = floorf(x + 0.5f);
    c.y = floorf(y + 0.5f);
    c.w = font_size;
    c.r = cr;
    c.g = cg;
    c.b = cb;
    c.a = ca;
    c.reserved = start;
    c.glyph_count = (uint16_t)((len > 0xFFFF) ? 0xFFFF : len);
    mini_push(r, c);
}

void mini_draw_text_ex(MiniRenderer *r, float x, float y, const char *text,
                       float font_size, float cr, float cg, float cb, float ca, float ls)
{
    mini_draw_text_styled(r, x, y, text, font_size, cr, cg, cb, ca, ls, 0);
}

void mini_draw_text(MiniRenderer *r, float x, float y, const char *text, float font_size, float cr, float cg, float cb, float ca)
{
    mini_draw_text_ex(r, x, y, text, font_size, cr, cg, cb, ca, 0.0f);
}

static void draw_text_wrapped(MiniRenderer *r, const char *text,
                              float x, float y, float max_w, float fs,
                              float cr, float cg, float cb, float ca, int align)
{
    if (!r || !text || !*text || fs <= 0 || ca <= 0)
        return;
    if (max_w <= 0)
        max_w = 1e9f;

    float lh = mini_text_line_height(fs);
    float pen_y = 0.0f;

    const char *p = text;
    char line_buf[1024];
    int line_len = 0;
    float line_w = 0.0f;

    while (*p)
    {
        if (*p == '\n' || *p == '\r')
        {
            if (*p == '\r' && p[1] == '\n')
                p++;
            p++;
            line_buf[line_len] = '\0';
            float draw_x = x;
            if (align == 1)
                draw_x = floorf(x + (max_w - line_w) * 0.5f + 0.5f);
            else if (align == 2)
                draw_x = floorf(x + (max_w - line_w) + 0.5f);
            if (line_len > 0)
                mini_draw_text(r, draw_x, floorf(y + pen_y + 0.5f), line_buf, fs, cr, cg, cb, ca);
            pen_y += lh;
            line_len = 0;
            line_w = 0.0f;
            continue;
        }

        const char *token_start = p;
        int token_bytes = 0;

        const char *peek_p = p;
        int cp_len = 0;
        unsigned int cp = utf8_next(&peek_p, &cp_len);

        if (cp > 0x7F)
        {
            token_bytes = cp_len;
            p = peek_p;
        }
        else if (isspace((unsigned char)*p))
        {
            while (*p && isspace((unsigned char)*p) && *p != '\n' && *p != '\r')
                p++;
            token_bytes = (int)(p - token_start);
        }
        else
        {
            while (*p && (unsigned char)*p <= 0x7F && !isspace((unsigned char)*p) && *p != '\n' && *p != '\r')
                p++;
            token_bytes = (int)(p - token_start);
        }

        if (token_bytes <= 0)
            break;

        char token_str[512];
        int t_len = token_bytes < (int)sizeof(token_str) - 1 ? token_bytes : (int)sizeof(token_str) - 1;
        memcpy(token_str, token_start, t_len);
        token_str[t_len] = '\0';

        float tok_w = mini_text_measure(token_str, fs);

        if (line_w > 0.0f && (line_w + tok_w > max_w))
        {
            line_buf[line_len] = '\0';
            float draw_x = x;
            if (align == 1)
                draw_x = x + (max_w - line_w) * 0.5f;
            else if (align == 2)
                draw_x = x + (max_w - line_w);
            mini_draw_text(r, draw_x, y + pen_y, line_buf, fs, cr, cg, cb, ca);

            pen_y += lh;
            line_len = 0;
            line_w = 0.0f;

            if (token_str[0] == ' ')
                continue;
        }

        if (tok_w > max_w && line_len == 0)
        {
            const char *tp = token_str;
            int clen = 0;
            while (*tp)
            {
                const char *cp_start = tp;
                unsigned int c = utf8_next(&tp, &clen);
                if (!clen)
                    break;
                char char_buf[8];
                int cbytes = (int)(tp - cp_start);
                memcpy(char_buf, cp_start, cbytes);
                char_buf[cbytes] = '\0';
                float cw = mini_text_measure(char_buf, fs);

                if (line_w > 0.0f && (line_w + cw > max_w))
                {
                    line_buf[line_len] = '\0';
                    float draw_x = x;
                    if (align == 1)
                        draw_x = x + (max_w - line_w) * 0.5f;
                    else if (align == 2)
                        draw_x = x + (max_w - line_w);
                    mini_draw_text(r, draw_x, y + pen_y, line_buf, fs, cr, cg, cb, ca);
                    pen_y += lh;
                    line_len = 0;
                    line_w = 0.0f;
                }

                if (line_len + cbytes < (int)sizeof(line_buf) - 1)
                {
                    memcpy(line_buf + line_len, char_buf, cbytes);
                    line_len += cbytes;
                    line_w += cw;
                }
            }
            continue;
        }

        if (line_len + t_len < (int)sizeof(line_buf) - 1)
        {
            memcpy(line_buf + line_len, token_str, t_len);
            line_len += t_len;
            line_w += tok_w;
        }
    }

    if (line_len > 0)
    {
        line_buf[line_len] = '\0';
        float draw_x = x;
        if (align == 1)
            draw_x = x + (max_w - line_w) * 0.5f;
        else if (align == 2)
            draw_x = x + (max_w - line_w);
        mini_draw_text(r, draw_x, y + pen_y, line_buf, fs, cr, cg, cb, ca);
    }
}

void mini_draw_gradient_ex(MiniRenderer *r, float x, float y, float w, float h,
                           float r1, float g1, float b1, float a1,
                           float r2, float g2, float b2, float a2,
                           int type, float angle, float radius)
{
    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = MINI_CMD_GRADIENT;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    c.r = r1;
    c.g = g1;
    c.b = b1;
    c.a = a1;
    c.u0 = r2;
    c.v0 = g2;
    c.u1 = b2;
    c.v1 = a2;
    c.reserved = type;                                         /* 0=linear 1=radial 2=conic            */
    c.font_id = (uint16_t)mini_gradient_norm_angle_deg(angle); /* 0..359 */
    c.texture_id = (uint32_t)radius;                           /* corner radius for the rounded shape   */
    mini_push(r, c);
}

void mini_draw_push_xform_full(MiniRenderer *r, float cx, float cy,
                               float tx, float ty, float tz,
                               float rx, float ry, float rz,
                               float sx, float sy,
                               float skx, float sky,
                               float perspective)
{
    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = MINI_CMD_PUSH_XFORM;
    c.x = cx;
    c.y = cy;
    c.w = tx;
    c.h = ty;
    c.a = tz;
    c.r = rx;
    c.g = ry;
    c.b = rz;
    c.u0 = (sx != 0.0f) ? sx : 1.0f;
    c.v0 = (sy != 0.0f) ? sy : 1.0f;
    c.u1 = skx;
    c.v1 = sky;
    c.persp = perspective; /* 0 = renderer default */
    mini_push(r, c);
}

void mini_draw_push_xform(MiniRenderer *r, float cx, float cy, float rx, float ry, float rz, float tz)
{
    mini_draw_push_xform_full(r, cx, cy, 0.0f, 0.0f, tz, rx, ry, rz, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f);
}

void mini_draw_pop_xform(MiniRenderer *r)
{
    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = MINI_CMD_POP_XFORM;
    mini_push(r, c);
}

/* 导出给 mini_dom.c 使用的圆角边框绘制函数（rect + stroke with per-corner radii） */
void mini_draw_rect_rounded_corners_stroke(MiniRenderer *r, float x, float y, float w, float h,
                                           const float radii[4], float lw, float cr, float cg, float cb, float ca)
{
    if (!r)
        return;
    MiniCmd c;
    memset(&c, 0, sizeof c);
    c.type = MINI_CMD_RECT_STROKE;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    c.r = cr;
    c.g = cg;
    c.b = cb;
    c.a = ca;
    c.reserved = (lw > 0 && lw < 0x7fffffff) ? (int)lw : 1;
    if (radii)
    {
        c.u0 = radii[0];
        c.v0 = radii[1];
        c.u1 = radii[2];
        c.v1 = radii[3];
    }
    mini_push(r, c);
}