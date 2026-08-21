/*
 * mini_css.h — CSS value engine + at-rule support (CSSOM).
 *
 * Extends the legacy flat-stylesheet parser (mini_css_apply in mini_dom.c)
 * with the modern CSS value + at-rule features:
 *
 *   • CSS custom properties (--foo: ...) and var(--foo [, fallback]) with
 *     recursive textual substitution (var-in-var, var-in-calc).
 *   • A real recursive-descent math evaluator for calc() supporting arbitrary
 *     nesting, parentheses, + - * /, length units (px/%/em/rem/vw/vh/vmin/
 *     vmax), and min()/max()/clamp() math functions — usable wherever a
 *     <length> is allowed, resolved at layout time against the real context.
 *   • @media query evaluation (min-/max-width|height, orientation) that
 *     re-applies on every restyle pass → dynamic responsive breakpoints.
 *   • @keyframes storage (named keyframe sets) + a per-frame animation step
 *     that interpolates animatable properties between keyframes.
 *   • @font-face rule storage (font-family + src url) for font loading.
 *
 * The value engine is pure C99 (no DOM/render deps) so it is unit-testable
 * without the GL stack. mini_dom.c wires it in: mini_css_apply stores custom
 * properties, var-substitutes declaration values, routes calc/min/max/clamp
 * to the deferred store, and parses @media/@keyframes/@font-face blocks.
 */
#ifndef MINI_CSS_H
#define MINI_CSS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* CSS custom properties (var() registry)                              */
/* ------------------------------------------------------------------ */
/* Reset the registry (call at the top of each stylesheet apply pass so
   :root custom properties are re-read against the current tree state). */
void mini_css_var_reset(void);
/* Set/replace a custom property by name (name excludes the leading "--"). */
void mini_css_var_set(const char *name, const char *value);
/* Look up a custom property; returns "" if unset, never NULL. */
const char *mini_css_var_get(const char *name);
/* Textually resolve every var(--name [, fallback]) in `value`, recursively,
   using the registry. Returns a malloc'd string (caller frees). Unknown vars
   with a fallback use the fallback; unknown vars with no fallback become "0". */
char *mini_css_resolve_vars(const char *value);
/* Scoped variable resolution walking up the DOM node tree */
char *mini_css_resolve_vars_node(const char *value, const void *node);
/* Invalidate the internal var() resolution cache wholesale (bumps a
   generation counter). Called automatically by mini_css_var_set /
   mini_css_var_reset, and must also be called when a node carrying
   scoped custom properties is destroyed, so a recycled node pointer
   cannot return a stale scoped resolution. Cheap (counter bump only). */
void mini_css_var_cache_invalidate(void);

/* ------------------------------------------------------------------ */
/* CSS math expression evaluator (calc / min / max / clamp / sin / sqrt...) */
/* ------------------------------------------------------------------ */
typedef struct MiniCssCtx
{
    float pct_base;      /* the 100% reference for this length (px)         */
    float font_px;       /* 1em in the current font-size context (px)       */
    float root_font_px;  /* 1rem in the root font-size context (px)         */
    float vw, vh;        /* viewport dimensions (px)                         */
} MiniCssCtx;

/* Evaluate a CSS math expression to a pixel value. Handles calc(), min(),
   max(), clamp(), sin(), cos(), tan(), sqrt(), pow(), abs(), mod(), rem(),
   round(), parentheses, + - * / (with CSS precedence), and length
 * units (px/%/em/rem/vw/vh/vmin/vmax). var() must already be substituted
 * (call mini_css_resolve_vars first). On a parse error returns 0 and sets
 * *ok to 0; on success *ok is 1. */
float mini_css_eval(const char *expr, const MiniCssCtx *ctx, int *ok);

/* ------------------------------------------------------------------ */
/* @media query evaluation                                             */
/* ------------------------------------------------------------------ */
/* Evaluate a CSS media-query condition string against the given viewport.
   Returns 1 if the media block should apply, 0 otherwise. Recognizes:
   (min-width: Npx), (max-width: ...), (min-/max-height: ...),
   (orientation: landscape|portrait), and `and`-joined lists. `all`/`screen`
   with no parens → 1. Unknown features → 1 (permissive, matches browsers). */
int mini_css_media_matches(const char *cond, int vw, int vh);

/* ------------------------------------------------------------------ */
/* @keyframes storage + animation step                                */
/* ------------------------------------------------------------------ */
/* Register a @keyframes rule. `name` is the animation name; `css` is the
   full keyframes body (from { ... } to { ... } 50% { ... }). Parsed into
   0..100% stops + property declarations. Duplicates of `name` are replaced. */
void mini_css_keyframes_set(const char *name, const char *css);
/* Look up a keyframes set by name; returns NULL if not registered. */
const void *mini_css_keyframes_get(const char *name);
/* Standard CSS cubic-bezier evaluator (x1, y1, x2, y2) at progression x (0..1) -> y (0..1) */
float mini_css_cubic_bezier(float x1, float y1, float x2, float y2, float x);

/* Standard timing function evaluator (0=linear, 1=ease, 2=ease-in, 3=ease-out, 4=ease-in-out, 5=cubic-bezier) */
float mini_css_eval_timing(int timing_mode, const float custom_bezier[4], float t);

/* Advanced CSS value interpolator supporting numbers, units, colors (#hex/rgb/hsl),
   transforms (translate/scale/rotate/skew), box-shadows, and multi-value lists */
void mini_css_interpolate_val(const char *prop, const char *v1, const char *v2, float t, char *out, size_t out_cap);

/* Return the interpolated declaration body ("prop:val;...") for the keyframes
   set `name` at time t (0..1) — malloc'd, NULL if the set doesn't exist.
   Performs smooth numerical, transform & color Lerp between keyframes. */
char *mini_css_keyframes_body_at(const char *name, double t);

/* ------------------------------------------------------------------ */
/* @font-face storage                                                  */
/* ------------------------------------------------------------------ */
typedef struct MiniFontFaceRule
{
    char family[64];
    char src_url[512];
    int  weight;
    char style[16];
} MiniFontFaceRule;
void mini_css_fontface_set(const char *css);             /* parse a @font-face body */
int  mini_css_fontface_count(void);
const MiniFontFaceRule *mini_css_fontface_get(int i);    /* NULL if out of range */
const MiniFontFaceRule *mini_css_fontface_match(const char *family, int weight, const char *style);

/* Self-test: exercises var substitution, calc/min/max/clamp eval, @media. */
int mini_css_selftest(void);

#ifdef __cplusplus
}
#endif
#endif /* MINI_CSS_H */
