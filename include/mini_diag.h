/*
 * mini_diag.h — dynamic runtime diagnostics (memory + profiler).
 *
 * Toggleable via hotkey (F12 / Ctrl+Shift+I) or queried by CDP. Captures
 * per-frame timing of rAF / layout / draw, an EMA FPS, QuickJS heap usage,
 * and the live MiniNode count. When active, prints a one-line HUD to stderr.
 */
#ifndef MINI_DIAG_H
#define MINI_DIAG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct MiniBridge;
struct MiniRenderer;
struct MiniDocument;
struct MiniCDP;

typedef struct {
    double raf_ms;
    double layout_ms;
    double draw_ms;
    double frame_ms;
    double fps;            /* EMA */
    size_t js_heap_bytes;  /* QuickJS malloc subsystem */
    size_t js_malloc_count;
    size_t dom_nodes;
    int    atlas_w, atlas_h;
    int    draw_calls;
} MiniDiagStats;

typedef struct MiniDiag MiniDiag;

/* b/r/doc/cdp may be NULL individually (diag degrades gracefully). */
MiniDiag *mini_diag_start(struct MiniBridge *b, struct MiniRenderer *r,
                          struct MiniDocument *doc, struct MiniCDP *cdp);
void      mini_diag_stop(MiniDiag *d);

/* Bracket the frame. _begin resets section timers; _section marks the end of
   a section (selected by `which`) whose start t0 was captured by _mark. */
enum { MINI_DIAG_RAF = 0, MINI_DIAG_LAYOUT = 1, MINI_DIAG_DRAW = 2 };
void  mini_diag_begin_frame(MiniDiag *d);
double mini_diag_mark(MiniDiag *d);                 /* returns current monotonic ms */
void  mini_diag_section(MiniDiag *d, double *t0, int which);
void  mini_diag_end_frame(MiniDiag *d);

const MiniDiagStats *mini_diag_stats(MiniDiag *d);

/* GLFW key callback hook: toggles the HUD overlay. key is a GLFW key code. */
void mini_diag_key(MiniDiag *d, int key, int mods);

/* Attach/detach the CDP server so the HUD dumps also reach devtools. */
void mini_diag_set_cdp(MiniDiag *d, struct MiniCDP *cdp);

/* Queryable snapshot (also pushed to CDP console if attached). */
void mini_diag_dump(MiniDiag *d);

#ifdef __cplusplus
}
#endif
#endif /* MINI_DIAG_H */
