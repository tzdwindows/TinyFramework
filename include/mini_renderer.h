/*
 * mini_renderer.h — self-written minimal renderer internal interface.
 *
 * Lives behind mini_framework.h in CUSTOM_MINI mode. NATIVE mode never
 * links this file. Everything here is plain C99 + GL/GLES, no third-party
 * layout engine, no WebKit/Chromium.
 *
 * The renderer is intentionally split into three cheap pieces:
 *   1. A vector/layer command buffer (rect / path / glyph / texture).
 *   2. A WebGL -> GL ES command bridge (driven by the JS bindings).
 *   3. A GPU context owning the window surface + default framebuffer.
 *
 * All draw commands are recorded first, then flushed once per frame so
 * we can sort by state (blend mode / texture / program) and minimize
 * glUseProgram / glBindTexture churn — the single biggest perf lever
 * on a thin GL ES backend.
 */
#ifndef MINI_RENDERER_H
#define MINI_RENDERER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* ------------------------------------------------------------------ */
    /* GPU context + window surface                                       */
    /* ------------------------------------------------------------------ */
    typedef struct MiniGPUContext
    {
        void *window_handle; /* GLFWwindow* (kept opaque on purpose) */
        int width, height;
        int sample_count;
        int gl_major, gl_minor;
        int vsync;
        /* current blend/depth state cache, avoids redundant gl* toggles */
        uint8_t blend_on;
        uint8_t depth_on;
    } MiniGPUContext;

    /* ------------------------------------------------------------------ */
    /* Vector command buffer (the 2D layer renderer)                      */
    /* ------------------------------------------------------------------ */
    typedef enum
    {
        MINI_CMD_CLEAR = 0,
        MINI_CMD_RECT,
        MINI_CMD_PATH_FILL,
        MINI_CMD_PATH_STROKE,
        MINI_CMD_GLYPH_RUN,
        MINI_CMD_TEXTURE_RECT,
        /* Phase 2: placeholder primitives so every DOM element category can
           draw a meaningful glyph/box (borders, lines, triangles, circles)
           and so text content can rasterize through the built-in font.     */
        MINI_CMD_RECT_STROKE, /* bordered box (form inputs, canvas, svg)  */
        MINI_CMD_LINE,        /* <hr>, table grid lines, borders          */
        MINI_CMD_TRIANGLE,    /* play button, disclosure caret            */
        MINI_CMD_CIRCLE,      /* radio/checkbox/dot markers               */
        MINI_CMD_TEXT,        /* rasterized text via built-in 5x7 font    */
        MINI_CMD_PUSH_XFORM,
        MINI_CMD_POP_XFORM,
        MINI_CMD_RECT_ROUNDED,
        MINI_CMD_SHADOW,
        MINI_CMD_GRADIENT,
        MINI_CMD_PUSH_CLIP,
        MINI_CMD_POP_CLIP,
        MINI_CMD_PUSH_ROUNDED_CLIP
    } MiniCmdType;

    typedef struct MiniCmd
    {
        MiniCmdType type;
        /* packed geometry; a real impl uses a tagged union in 64 bytes. */
        float x, y, w, h;
        float r, g, b, a;     /* fill / stroke color (premultiplied)   */
        uint32_t texture_id;  /* 0 = none                            */
        float u0, v0, u1, v1; /* UV box for texture_rect / radii / shadow params */
        uint16_t glyph_count; /* for GLYPH_RUN                       */
        uint16_t font_id;
        int32_t reserved;
        /* perspective distance for PUSH_XFORM (px); 0 = use the renderer
           default. Carries CSS `perspective()` / `perspective:` so a 3D
           rotateX/Y element (e.g. the grid-floor) gets a tunable, real
           perspective instead of the old hardcoded d=1000.            */
        float persp;
    } MiniCmd;

#define MINI_CMD_CAP 8192

    typedef struct MiniVectorBuffer
    {
        MiniCmd cmds[MINI_CMD_CAP];
        uint32_t count;
        int width, height; /* framebuffer dims for viewport     */
    } MiniVectorBuffer;

#define TT_GLYPH_CAP 2048
#define TT_SIZE_BUCKETS 64
#define TT_ATLAS_PAD 1
#define MINI_CLIP_DEPTH 32
#define MINI_MAX_FONTS 8

    typedef struct TtGlyph
    {
        uint32_t cp;          /* codepoint (0 = empty slot) */
        uint16_t size_bucket; /* size bucket */
        uint16_t in_use;      /* 1 if slot occupied */
        int16_t atlas_x, atlas_y;
        int16_t gw, gh;
        int16_t xoff, yoff;
        float advance;
    } TtGlyph;

    typedef struct MiniClipRect
    {
        float x, y, w, h;
        uint8_t stencil;
    } MiniClipRect;

    /* A single glyph quad for a glyph run */
    typedef struct MiniGlyphQuad
    {
        float dx, dy;         /* pen offset from run origin (px) */
        float gw, gh;         /* glyph cell size (px) */
        float u0, v0, u1, v1; /* atlas UV box */
    } MiniGlyphQuad;

    typedef struct MiniFontEntry
    {
        void *font_ctx;          /* stbtt_fontinfo* */
        unsigned char *ttf_data; /* malloc'd TTF file bytes */
        int loaded;              /* 1 once TTF is initialized */
        int ascent, descent, line_gap;
    } MiniFontEntry;

    /* ------------------------------------------------------------------ */
    /* Main renderer object                                               */
    /* ------------------------------------------------------------------ */
    typedef struct MiniRenderer
    {
        MiniGPUContext gpu;
        MiniVectorBuffer vbuf;

        /* texture atlas (single shared GL texture for UI glyphs/sprites) */
        uint32_t atlas_texture;
        int atlas_w, atlas_h;
        uint32_t *atlas_cpu;

        /* glyph cache: stb_truetype rasterized glyphs into atlas */
        void *font_ctx;          /* Primary font: stbtt_fontinfo* (alias to fonts[0].font_ctx) */
        unsigned char *ttf_data; /* Primary font bytes */
        int ttf_loaded;          /* 1 once any TTF is loaded */
        int font_ascent, font_descent, font_line_gap;

        /* Multi-font fallback chain */
        MiniFontEntry fonts[MINI_MAX_FONTS];
        int num_fonts;

        /* TTF atlas & glyph hash cache state (per-renderer instance) */
        TtGlyph tt_cache[TT_GLYPH_CAP];
        int tt_atlas_x, tt_atlas_y, tt_atlas_row_h;

        /* Legacy fallback pointers kept for compatibility */
        void *fallback_font_ctx;
        unsigned char *fallback_ttf_data;
        int fallback_ttf_loaded;
        int fallback_font_ascent, fallback_font_descent, fallback_font_line_gap;

        /* Scissor / clip stack (per-renderer instance) */
        MiniClipRect clip[MINI_CLIP_DEPTH];
        int clip_top;

        /* Side buffers for glyph quads & text store (per-renderer instance) */
        MiniGlyphQuad *glyph_quads;
        int glyph_cap, glyph_n;
        char *text_store;
        int text_cap, text_n;

        /* WebGL-bridge GPU resource bookkeeping (programs/buffers/etc.) */
        void *gl_state; /* MiniGLBridge* — see bridge.c */

#define MINI_GRAD_CACHE_CAP 16
        struct
        {
            uint32_t tex;    /* GL texture id, 0 = empty slot */
            int type;        /* 0 linear / 1 radial / 2 conic */
            int angle;       /* normalized degrees 0..359 */
            int bw, bh;      /* box size raster computed for */
            uint32_t c1, c2; /* packed RGBA8 of the two stops */
            uint32_t tick;   /* last-used tick (LRU eviction) */
        } grad_cache[MINI_GRAD_CACHE_CAP];
        uint32_t grad_tick;
    } MiniRenderer;

    /* Record a glyph run: append `quads` to the renderer side-buffer and emit
     * a GLYPH_RUN command that the flush path rasterizes as one textured
     * quad strip bound to the shared atlas. (x,y) is the run origin. */
    void mini_draw_glyph_run(MiniRenderer *r, float x, float y,
                             const MiniGlyphQuad *quads, int n,
                             float cr, float cg, float cb, float ca,
                             uint16_t font_id);

    /* Composite one rasterized glyph (RGBA, premultiplied) into the shared
     * atlas at (dst_x,dst_y) and upload the dirty sub-rect. The CPU mirror
     * is the compositing destination so no GL readback is needed. */
    void mini_atlas_blit_glyph(MiniRenderer *r,
                               int dst_x, int dst_y, int gw, int gh,
                               const uint32_t *src_rgba);

    /* ---- TTF font (stb_truetype) -------------------------------------- */
    /* Load a TrueType font file from disk into the renderer's font context.
     * Returns 0 on success; on failure the renderer keeps the built-in 5x7
     * bitmap font as a fallback. A renderer holds at most one TTF at a time
     * (the common case for an embedded UI). The buffer is owned by the
     * renderer and freed in mini_renderer_destroy.                     */
    int mini_renderer_load_font(MiniRenderer *r, const char *path);
    int mini_renderer_load_fallback_font(MiniRenderer *r, const char *path);
    /* Whether a TTF is loaded (0 = no, using 5x7 fallback). */
    int mini_renderer_has_font(const MiniRenderer *r);

    void mini_draw_push_xform(MiniRenderer *r, float cx, float cy, float rx, float ry, float rz, float tz);
    void mini_draw_push_xform_full(MiniRenderer *r, float cx, float cy,
                                   float tx, float ty, float tz,
                                   float rx, float ry, float rz,
                                   float sx, float sy,
                                   float skx, float sky,
                                   float perspective);
    void mini_draw_pop_xform(MiniRenderer *r);

    /* ------------------------------------------------------------------ */
    /* Lifecycle                                                          */
    /* ------------------------------------------------------------------ */
    MiniRenderer *mini_renderer_create(int width, int height, int samples, int vsync);
    void mini_renderer_destroy(MiniRenderer *r);

    /* Per-frame entry/exit (called by the main loop). */
    void mini_renderer_begin_frame(MiniRenderer *r);
    void mini_renderer_flush(MiniRenderer *r); /* execute vbuf + swap */
    void mini_renderer_end_frame(MiniRenderer *r);
    /* Re-bind the WebGL program/VAO/buffer/texture the JS layer set up, after
       the DOM 2D pass (which resets them to 0 for legacy glBegin/glEnd) and
       before rAF, so drawArrays/drawElements run with the app's own GL state. */
    void mini_renderer_restore_webgl(MiniRenderer *r);

    /* ------------------------------------------------------------------ */
    /* Vector command API (the "2D Canvas" backend)                       */
    /* ------------------------------------------------------------------ */
    void mini_draw_clear(MiniRenderer *r, float cr, float cg, float cb, float ca);
    void mini_draw_rect(MiniRenderer *r, float x, float y, float w, float h,
                        float cr, float cg, float cb, float ca);
    void mini_draw_texture(MiniRenderer *r, float x, float y, float w, float h,
                           uint32_t tex, float u0, float v0, float u1, float v1);

    /* ------------------------------------------------------------------ */
    /* Phase 2: placeholder + text primitives                              */
    /* Every native HTML element category (media, form, table, interactive)*/
    /* has a representative placeholder drawn from these. Text content is   */
    /* rasterized through a self-contained 5x7 bitmap font so the renderer  */
    /* has zero external font-file dependency.                              */
    /* ------------------------------------------------------------------ */
    void mini_draw_rect_stroke(MiniRenderer *r, float x, float y, float w, float h,
                               float line_w, float cr, float cg, float cb, float ca);
    void mini_draw_line(MiniRenderer *r, float x1, float y1, float x2, float y2,
                        float line_w, float cr, float cg, float cb, float ca);
    void mini_draw_triangle(MiniRenderer *r, float x1, float y1,
                            float x2, float y2, float x3, float y3,
                            float cr, float cg, float cb, float ca);
    void mini_draw_circle(MiniRenderer *r, float cx, float cy, float radius,
                          float cr, float cg, float cb, float ca);
    void mini_draw_polygon(MiniRenderer *r, const float *pts, int num_pts,
                           float cr, float cg, float cb, float ca);
    void mini_draw_polygon_stroke(MiniRenderer *r, const float *pts, int num_pts,
                                  float line_w, float cr, float cg, float cb, float ca);
    /* Draw `text` at (x,y) [top-left baseline of the first glyph] using the
       built-in 5x7 font scaled so each glyph cell is `font_size` px tall.
       Unknown glyphs render as a filled cell (placeholder bar) so any text
       still shows something. */
    void mini_draw_text(MiniRenderer *r, float x, float y, const char *text,
                        float font_size, float cr, float cg, float cb, float ca);
    /* Measure the rendered width of `text` at `font_size` (matches the
       advance used by mini_draw_text, so layout and render agree). */
    float mini_text_measure(const char *text, float font_size);
    /* Approximate height of a line of text at `font_size` (ascent+descent). */
    float mini_text_line_height(float font_size);

    /* overflow:hidden clipping: push a content-box scissor (intersected with
       any active clip, so nested clips nest) before drawing children; pop
       restores the prior clip. Coordinates are top-left origin (converted
       to GL bottom-left scissor internally).                          */
    void mini_renderer_push_clip(MiniRenderer *r, float x, float y, float w, float h);
    /* Stage 4: rounded clip — scissor rect refined by a stencil mask of the
       rounded shape, so overflow:hidden + border-radius children clip to the
       rounded corners (not the square scissor). GL 1.1 stencil; falls back to
       scissor-only if no stencil buffer is present.                       */
    void mini_renderer_push_rounded_clip(MiniRenderer *r, float x, float y, float w,
                                         float h, float radius);
    void mini_renderer_push_rounded_clip_corners(MiniRenderer *r, float x, float y, float w,
                                                 float h, const float radii[4]);
    void mini_renderer_pop_clip(MiniRenderer *r);

    /* ------------------------------------------------------------------ */
    /* Pipeline <-> layout wiring (defined in mini_dom.c)                */
    /* ------------------------------------------------------------------ */
    struct MiniNode;
    struct MiniDocument;
    void mini_dom_render_into(struct MiniNode *node, MiniRenderer *r);
    void mini_dom_render_page_backdrop(struct MiniDocument *doc, MiniRenderer *r, float fw, float fh);

    /* Add to API declarations */
    void mini_draw_rect_rounded(MiniRenderer *r, float x, float y, float w, float h, float radius, float cr, float cg, float cb, float ca);
    void mini_draw_rect_rounded_corners(MiniRenderer *r, float x, float y, float w, float h, const float radii[4], float cr, float cg, float cb, float ca);
    void mini_draw_shadow(MiniRenderer *r, float x, float y, float w, float h, float radius, float spread, float blur, float cr, float cg, float cb, float ca);
    void mini_draw_shadow_corners(MiniRenderer *r, float x, float y, float w, float h, const float radii[4], float spread, float blur, float cr, float cg, float cb, float ca);
    void mini_draw_gradient(MiniRenderer *r, float x, float y, float w, float h, float r1, float g1, float b1, float a1, float r2, float g2, float b2, float a2, int type, float angle);
    void mini_draw_backdrop_filter(MiniRenderer *r, float x, float y, float w, float h, float blur_radius, float invert, const float radii[4]);

    /* Capture the current framebuffer as a base64-ready PNG. Reads the front
       buffer via glReadPixels, Y-flips, and encodes with mini_png. On success
       returns 0 and sets *out to a malloc'd PNG buffer (caller frees) and
       *out_len to its size. For Page.captureScreenshot. */
    int mini_renderer_screenshot_png(MiniRenderer *r, uint8_t **out, size_t *out_len);

    void mini_draw_gradient_ex(MiniRenderer *r, float x, float y, float w, float h,
                               float r1, float g1, float b1, float a1,
                               float r2, float g2, float b2, float a2,
                               int type, float angle, float radius);
    void mini_draw_gradient_multi(MiniRenderer *r, float x, float y, float w, float h,
                                  const void *stops, int num_stops,
                                  int type, float angle, const float radii[4]);

    /* Background image painting */
    void mini_draw_background_image(MiniRenderer *r, float x, float y, float w, float h,
                                    const char *url, int size_mode,
                                    float bg_w, float bg_h,
                                    float pos_x, float pos_y, int repeat,
                                    const float radii[4]);

    /* ---- CPU gradient rasterizer (pure CPU, no GL; unit-testable) -------
       Fills `out` (rw*rh*4 bytes, straight RGBA8) with the gradient sampled
       at resolution rw x rh over a box of box_w x box_h. Deterministic per
       pixel, so it is verifiable without a display. type: 0=linear,
       1=radial (circle, farthest-corner), 2=conic (from angle, clockwise).
       angle is CSS degrees (0=to top). c1/c2 are [r,g,b,a] in 0..1.          */
    int mini_gradient_norm_angle_deg(float angle); /* -> [0,360) */
    void mini_gradient_raster(int type, int rw, int rh,
                              float box_w, float box_h, float angle_deg,
                              float c1r, float c1g, float c1b, float c1a,
                              float c2r, float c2g, float c2b, float c2a,
                              unsigned char *out);
    void mini_gradient_raster_multi(int type, int rw, int rh,
                                    float box_w, float box_h, float angle_deg,
                                    const void *stops, int num_stops,
                                    unsigned char *out);

    float mini_text_measure_ex(const char *text, float font_size, float letter_spacing);

    void mini_draw_text_ex(MiniRenderer *r, float x, float y, const char *text,
                           float font_size, float cr, float cg, float cb, float ca, float ls);
    void mini_draw_text_styled(MiniRenderer *r, float x, float y, const char *text,
                               float font_size, float cr, float cg, float cb, float ca, float ls, int font_style);
    void mini_draw_rect_difference(MiniRenderer *r, float x, float y, float w, float h, const float radii[4],
                                   float cr, float cg, float cb, float ca);
#ifdef __cplusplus
}
#endif
#endif /* MINI_RENDERER_H */
