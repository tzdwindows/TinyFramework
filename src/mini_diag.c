/*
 * mini_diag.c — dynamic diagnostics: per-frame profiler + memory monitor.
 *
 * Timing uses the same monotonic clock the main loop already has (glfwGetTime)
 * so profiler numbers are in the same timebase as rAF. FPS is an exponential
 * moving average (reactive but stable). Memory reads QuickJS via a bridge
 * accessor so this file stays free of QuickJS headers.
 */
#include "mini_diag.h"
#include "mini_dom.h"        /* MiniNode walk */
#include "mini_js_bridge.h"  /* mini_bridge_heap_usage */
#include "mini_cdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
  #include <time.h>
#elif defined(_WIN32)
  #include <windows.h>
#else
  #include <time.h>
#endif

struct MiniDiag {
    struct MiniBridge   *b;
    struct MiniRenderer *r;
    struct MiniDocument *doc;
    struct MiniCDP      *cdp;
    MiniDiagStats        s;
    double               ema_fps;
    double               frame_start_ms;
    int                   active;          /* HUD overlay on/off            */
    long                 frames;
};

/* monotonic ms (kept independent of GLFW so diag links without it) */
static double now_ms(void) {
#if defined(_WIN32)
    static LARGE_INTEGER f = {{0,0}}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart * 1000.0;
#elif defined(CLOCK_MONOTONIC)
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec*1000.0 + ts.tv_nsec/1.0e6;
#else
    return 0;
#endif
}

static size_t count_nodes(struct MiniNode *n) {
    if (!n) return 0;
    size_t c = 1;
    for (struct MiniNode *ch = n->first_child; ch; ch = ch->next_sibling)
        c += count_nodes(ch);
    return c;
}

MiniDiag *mini_diag_start(struct MiniBridge *b, struct MiniRenderer *r,
                          struct MiniDocument *doc, struct MiniCDP *cdp) {
    MiniDiag *d = (MiniDiag*)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->b = b; d->r = r; d->doc = doc; d->cdp = cdp;
    d->ema_fps = 60.0;
    d->active = 0;     /* HUD off by default; F12 toggles */
    return d;
}
void mini_diag_stop(MiniDiag *d) { free(d); }

void mini_diag_begin_frame(MiniDiag *d) {
    d->frame_start_ms = now_ms();
    d->s.raf_ms = d->s.layout_ms = d->s.draw_ms = 0.0;
}
double mini_diag_mark(MiniDiag *d) { return now_ms() - d->frame_start_ms; }
void mini_diag_section(MiniDiag *d, double *t0, int which) {
    double now = now_ms();
    double dt = now - *t0;
    switch (which) {
        case MINI_DIAG_RAF:    d->s.raf_ms    = dt; break;
        case MINI_DIAG_LAYOUT: d->s.layout_ms = dt; break;
        case MINI_DIAG_DRAW:   d->s.draw_ms   = dt; break;
    }
    *t0 = now;
}
void mini_diag_end_frame(MiniDiag *d) {
    double frame = now_ms() - d->frame_start_ms;
    d->s.frame_ms = frame;
    d->frames++;
    /* EMA FPS: alpha ~ 1/30 frames */
    double inst = frame > 0 ? 1000.0 / frame : 0.0;
    d->ema_fps = d->ema_fps + (inst - d->ema_fps) * 0.05;
    d->s.fps = d->ema_fps;
    /* memory */
    if (d->b) {
        size_t c = mini_bridge_heap_usage(d->b);
        d->s.js_heap_bytes = c;
    }
    if (d->doc && d->doc->body)
        d->s.dom_nodes = count_nodes(d->doc->body);
    if (d->r) {
        d->s.atlas_w = d->r->atlas_w;
        d->s.atlas_h = d->r->atlas_h;
        d->s.draw_calls = (int)d->r->vbuf.count;
    }
    /* HUD: every 30 frames print a one-liner when active */
    if (d->active && (d->frames % 30) == 0)
        mini_diag_dump(d);
}

const MiniDiagStats *mini_diag_stats(MiniDiag *d) { return &d->s; }

void mini_diag_set_cdp(MiniDiag *d, struct MiniCDP *cdp) { if (d) d->cdp = cdp; }

void mini_diag_key(MiniDiag *d, int key, int mods) {
    if (key == 293 /*GLFW_KEY_F12*/ ||
        (mods == 5 /* GLFW_MOD_CONTROL|SHIFT */ && (key == 73 /*'I'*/))) {
        d->active = !d->active;
        if (d->active) {
            fprintf(stderr, "[diag] HUD on — F12 to turn off\n");
            mini_diag_dump(d);
        } else fprintf(stderr, "[diag] HUD off\n");
    }
}

void mini_diag_dump(MiniDiag *d) {
    char line[512];
    int n = snprintf(line, sizeof line,
        "[diag] %5.1f fps | frame %5.2fms (raf %.2f layout %.2f draw %.2f) | "
        "JS heap %zu bytes (%zu allocs) | DOM %zu nodes | atlas %dx%d | drawCalls %d",
        d->s.fps, d->s.frame_ms, d->s.raf_ms, d->s.layout_ms, d->s.draw_ms,
        d->s.js_heap_bytes, d->s.js_malloc_count, d->s.dom_nodes,
        d->s.atlas_w, d->s.atlas_h, d->s.draw_calls);
    fprintf(stderr, "%s\n", line);
    if (d->cdp) mini_cdp_emit_log(d->cdp, "log", line);
    (void)n;
}
