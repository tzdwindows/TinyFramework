/*
 * mini_gradient.c - CPU rasterizer for CSS-style gradients.
 *
 * Pure CPU (no OpenGL, no GLFW): fills a straight-RGBA8 buffer with a
 * per-pixel gradient sample. This is the verifiable substrate for the
 * Stage-4 radial/conic/angled-linear consumers: because the math is
 * deterministic, it can be unit-tested without a display (see
 * tmp_grad_test.c), unlike shader/FBO output which would need a real GPU.
 *
 * The renderer (mini_renderer.c) rasterizes into a small buffer, uploads it
 * to a GL 1.1 texture, and draws a textured polygon -- no shaders, no FBOs,
 * so the WebGL bridge's bound program is never clobbered.
 *
 * Semantics (CSS-compliant where it matters for a micro-engine):
 *   type 0 linear : angle 0=to top, 90=to right, 180=to bottom (CSS deg).
 *                   t = (p . dir)/L + 0.5, L = |w*sin|+|h*cos|.
 *   type 1 radial : circle, farthest-corner. t = dist/D, D = half-diag.
 *   type 2 conic  : from `angle`, clockwise, 0deg at top.
 *                   t = ((point_angle - angle) mod 360)/360.
 * Colors are straight (non-premultiplied), matching the renderer's
 * GL_SRC_ALPHA/GL_ONE_MINUS_SRC_ALPHA blend. Output bytes are 0..255.
 */
#include "mini_renderer.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Normalize an arbitrary angle (degrees) to [0,360). */
int mini_gradient_norm_angle_deg(float angle)
{
    if (!isfinite(angle))
        return 0;
    float a = fmodf(angle, 360.0f);
    if (a < 0.0f)
        a += 360.0f;
    if (a >= 360.0f)
        a = 0.0f;
    return (int)(a + 0.5f) % 360; /* round to nearest int degree */
}

static unsigned char f2b(float v)
{
    if (v <= 0.0f)
        return 0;
    if (v >= 1.0f)
        return 255;
    return (unsigned char)(v * 255.0f + 0.5f);
}

void mini_gradient_raster(int type, int rw, int rh,
                          float box_w, float box_h, float angle_deg,
                          float c1r, float c1g, float c1b, float c1a,
                          float c2r, float c2g, float c2b, float c2a,
                          unsigned char *out)
{
    struct {
        float r, g, b, a;
        float pos;
    } stops[2] = {
        { c1r, c1g, c1b, c1a, 0.0f },
        { c2r, c2g, c2b, c2a, 1.0f }
    };
    mini_gradient_raster_multi(type, rw, rh, box_w, box_h, angle_deg, stops, 2, out);
}

typedef struct MiniGradStopResolved {
    float r, g, b, a;
    float pos;
} MiniGradStopResolved;

void mini_gradient_raster_multi(int type, int rw, int rh,
                                float box_w, float box_h, float angle_deg,
                                const void *raw_stops, int num_stops,
                                unsigned char *out)
{
    if (!out || rw <= 0 || rh <= 0 || box_w <= 0.0f || box_h <= 0.0f || !raw_stops || num_stops <= 0)
        return;

    const MiniGradStopResolved *in_stops = (const MiniGradStopResolved *)raw_stops;
    MiniGradStopResolved stops[16];
    int n = num_stops > 16 ? 16 : num_stops;
    for (int i = 0; i < n; i++) stops[i] = in_stops[i];

    if (n == 1) {
        stops[1] = stops[0];
        stops[0].pos = 0.0f;
        stops[1].pos = 1.0f;
        n = 2;
    }

    if (stops[0].pos < 0.0f) stops[0].pos = 0.0f;
    if (stops[n - 1].pos < 0.0f) stops[n - 1].pos = 1.0f;

    /* Distribute unspecified intermediate positions */
    int i = 0;
    while (i < n) {
        if (stops[i].pos >= 0.0f) { i++; continue; }
        int start = i - 1;
        int end = i;
        while (end < n && stops[end].pos < 0.0f) end++;
        if (end >= n) end = n - 1;
        float start_pos = stops[start].pos;
        float end_pos = stops[end].pos;
        int steps = end - start;
        for (int k = 1; k < steps; k++) {
            stops[start + k].pos = start_pos + (end_pos - start_pos) * ((float)k / (float)steps);
        }
        i = end;
    }

    const float cx = box_w * 0.5f;
    const float cy = box_h * 0.5f;
    const float D = sqrtf(cx * cx + cy * cy);
    const float a_rad = (float)mini_gradient_norm_angle_deg(angle_deg) * (float)(M_PI / 180.0);

    float dirx = sinf(a_rad);
    float diry = -cosf(a_rad);
    float L = fabsf(box_w * sinf(a_rad)) + fabsf(box_h * cosf(a_rad));
    if (L < 1e-6f) L = 1e-6f;

    for (int j = 0; j < rh; j++)
    {
        float py = ((float)j + 0.5f) / (float)rh * box_h;
        for (int c_idx = 0; c_idx < rw; c_idx++)
        {
            float px = ((float)c_idx + 0.5f) / (float)rw * box_w;
            float dx = px - cx;
            float dy = py - cy;

            float t;
            switch (type)
            {
            case 1: /* radial */
                t = sqrtf(dx * dx + dy * dy) / (D > 1e-6f ? D : 1.0f);
                break;
            case 2: /* conic */
            {
                float pa = atan2f(dx, -dy);
                if (pa < 0.0f) pa += (float)(2.0 * M_PI);
                float deg = pa * (float)(180.0 / M_PI);
                deg -= (float)mini_gradient_norm_angle_deg(angle_deg);
                deg = fmodf(deg, 360.0f);
                if (deg < 0.0f) deg += 360.0f;
                t = deg / 360.0f;
                break;
            }
            default: /* linear */
                t = (dx * dirx + dy * diry) / L + 0.5f;
                break;
            }
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;

            /* Sample along stops */
            float fr = stops[0].r, fg = stops[0].g, fb = stops[0].b, fa = stops[0].a;
            if (t <= stops[0].pos) {
                fr = stops[0].r; fg = stops[0].g; fb = stops[0].b; fa = stops[0].a;
            } else if (t >= stops[n - 1].pos) {
                fr = stops[n - 1].r; fg = stops[n - 1].g; fb = stops[n - 1].b; fa = stops[n - 1].a;
            } else {
                for (int k = 0; k < n - 1; k++) {
                    if (t >= stops[k].pos && t <= stops[k + 1].pos) {
                        float span = stops[k + 1].pos - stops[k].pos;
                        float frac = (span > 1e-6f) ? (t - stops[k].pos) / span : 0.0f;
                        float omt = 1.0f - frac;
                        fr = stops[k].r * omt + stops[k + 1].r * frac;
                        fg = stops[k].g * omt + stops[k + 1].g * frac;
                        fb = stops[k].b * omt + stops[k + 1].b * frac;
                        fa = stops[k].a * omt + stops[k + 1].a * frac;
                        break;
                    }
                }
            }

            unsigned char *p = out + ((size_t)j * rw + c_idx) * 4;
            p[0] = f2b(fr);
            p[1] = f2b(fg);
            p[2] = f2b(fb);
            p[3] = f2b(fa);
        }
    }
}
