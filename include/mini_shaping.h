/*
 * mini_shaping.h — complex-text shaping (Arabic / Devanagari / Emoji / kerning).
 *
 * A self-contained shaping layer that, given a logical-order codepoint run
 * (after BiDi reordering), produces a shaped glyph run ready for rasterization:
 *
 *   • Arabic: the joining algorithm selects isolated / initial / medial /
 *     final presentation forms per letter so connected (cursive) Arabic renders
 *     with the correct ligatures (lam-alef, connected glyphs).
 *   • Devanagari (Indic): pre-base matras (e.g. 093F short-i) are reordered
 *     before their consonant, and combining marks (nukta, vowel signs, virama)
 *     are clustered so the font positions vowel signs above/below/after.
 *   • Emoji: ZWJ (U+200D) sequences and variation-selector (U+FE0F) clusters
 *     are collapsed into a single wide glyph; emoji codepoints get a 2× cell.
 *   • Kerning: an OpenType-style pair-kerning hook (the renderer supplies the
 *     font's kern advance) adjusts inter-glyph spacing.
 *
 * Pure C99 (no deps). The renderer applies the shaped run (output codepoints
 * + x-offsets + advances) — wired in Phase 8 to avoid touching the GL path
 * concurrently with the other agent's WebGL work.
 */
#ifndef MINI_SHAPING_H
#define MINI_SHAPING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Form selected by the Arabic joining algorithm for a letter. */
enum { SHAP_FORM_NONE = -1, SHAP_ISOLATED=0, SHAP_INITIAL=1, SHAP_MEDIAL=2, SHAP_FINAL=3 };

/* Arabic joining type (UAX #9 / Unicode §9.2). */
typedef enum {
    JT_U = 0,   /* non-joining   */
    JT_L,        /* left-joining  */
    JT_R,        /* right-joining */
    JT_D,        /* dual-joining  */
    JT_C,        /* join-causing (tatweel) */
    JT_T         /* transparent (combining marks) */
} MiniJoinType;

MiniJoinType mini_join_type(uint32_t cp);

/* Run the Arabic joining algorithm over `cps[0..n)`, writing the selected
   form index (SHAP_ISOLATED..SHAP_FINAL, or SHAP_FORM_NONE for non-Arabic)
   into `forms[i]`. Respects transparent marks (they don't break joining).    */
void mini_shape_arabic(const uint32_t *cps, int n, int *forms);

/* Map an Arabic base letter + form to its Arabic Presentation Form codepoint
   (Forms-A/B). Returns the base codepoint unchanged if no presentation form
   is known (the renderer falls back to the base glyph). */
uint32_t mini_arabic_presentation(uint32_t base, int form);

/* Cluster + reorder a codepoint run for Indic (Devanagari) and Emoji.
   Writes, for each input position i: `out_cp[i]` the codepoint to rasterize
   (after pre-base matra reorder + ZWJ/VS collapse), `out_advance_mul[i]` the
   advance multiplier (1.0 normal, 2.0 for a wide emoji cell, 0.0 for a
   collapsed ZWJ/VS that merges into the previous glyph), and `out_cluster[i]`
   the cluster id (chars sharing an id are one grapheme). Returns the count
   (= n). Caller frees the three out arrays (each malloc'd to n). */
int mini_shape_cluster(const uint32_t *cps, int n,
                       uint32_t **out_cp, float **out_advance_mul, int **out_cluster);

/* Pair-kerning hook. `kern_fn(gid_a, gid_b, ud)` returns the kern advance in
   font units (≤0 = bring closer). Applied to consecutive glyphs of the same
   run by the renderer. Provided here so the renderer wires a single callback. */
typedef float (*MiniKernFn)(uint32_t cp_a, uint32_t cp_b, void *ud);

/* Self-test: Arabic joining form selection, emoji ZWJ collapse, Devanagari
   pre-base matra reorder. */
int mini_shaping_selftest(void);

#ifdef __cplusplus
}
#endif
#endif /* MINI_SHAPING_H */
