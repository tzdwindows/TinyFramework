/*
 * mini_bidi.h — Unicode Bidirectional Algorithm (UAX #9) — self-contained.
 *
 * A from-scratch implementation of the core Unicode Bidi Algorithm so the
 * engine can lay out mixed left-to-right / right-to-left text (English +
 * Hebrew / Arabic) without a FriBidi/ICU dependency. Implemented stages:
 *
 *   • paragraph embedding level (P2–P3: first-strong-char heuristic)
 *   • explicit embedding / override resolution (X1–X9: RLE/LRE/RLO/LRO/PDF
 *     and BN/FSI/Boundary-Neutral handling)
 *   • weak-type resolution (W1–W7: EN/AN/ES/ET/CS/NSM/AL → resolved)
 *   • neutral-type resolution (N1–N2: NI resolved by surrounding strong)
 *   • implicit levels (I1–I2) + the L2 reordering of runs at each depth,
 *     with bracket-pair mirroring (L4) for parentheses in RTL context
 *
 * The public surface takes a logical-order UTF-8 string and produces a
 * visual-order index map (which logical char lands at each visual position)
 * plus a per-character resolved level, so the renderer can paint left-to-
 * right while each run keeps its direction. Pure C99 (no deps) → unit-
 * testable without the GL stack.
 */
#ifndef MINI_BIDI_H
#define MINI_BIDI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bidi character class (subset of UAX#9 §4.3 used by the resolver). */
typedef enum {
    BIDI_L = 0,   /* Left-to-Right            */
    BIDI_R,       /* Right-to-Left            */
    BIDI_AL,      /* Right-to-Left Arabic     */
    BIDI_EN,      /* European Number          */
    BIDI_ES,      /* European Separator       */
    BIDI_ET,      /* European Term. (±/− sign) */
    BIDI_AN,      /* Arabic Number            */
    BIDI_CS,      /* Common Separator         */
    BIDI_NSM,     /* Nonspacing Mark          */
    BIDI_BN,      /* Boundary Neutral         */
    BIDI_BS,      /* Block Separator          */
    BIDI_SS,      /* Segment Separator        */
    BIDI_WS,      /* Whitespace               */
    BIDI_ON,      /* Other Neutral            */
    BIDI_LRE, BIDI_RLE, BIDI_LRO, BIDI_RLO, BIDI_PDF, BIDI_FSI, BIDI_LRI, BIDI_RLI, BIDI_PDI
} MiniBidiClass;

/* Look up the bidi class of a codepoint (covers ASCII, Hebrew 0590–05FF,
   Arabic 0600–06FF, common brackets/punct, digit ranges). Unknown → ON. */
MiniBidiClass mini_bidi_class(uint32_t cp);

/* Mirror a codepoint that has a bidi mirror (e.g. '(' ↔ ')', '[' ↔ ']').
   Returns the mirror, or `cp` itself if none. Used in the L4 mirroring pass
   for RTL runs so brackets face the right way. */
uint32_t mini_bidi_mirror(uint32_t cp);

/* Reorder a UTF-8 string into visual order.
   `visual[i]` = index (in UTF-8 byte offset into `text`) of the logical char
   that should be painted at visual position i. `n` is the number of base
   codepoint positions (the indices are into a codepoint array, NOT bytes —
   the caller maps UTF-8 → codepoints first). `levels[i]` receives the
   resolved embedding level of the char at logical index i (odd = RTL).
   Returns the paragraph embedding level (0 LTR, 1 RTL).                          */
int mini_bidi_reorder(const uint32_t *cps, int n, int *visual, int *levels);

/* Convenience: reorder a UTF-8 string into a visual-order byte-index array.
   Fills `vis_byte[i]` = byte offset into `utf8` of the i-th visual char,
   for `*out_n` characters. Caller frees *vis_byte. Returns paragraph level. */
int mini_bidi_reorder_utf8(const char *utf8, size_t len, int **vis_byte, int *out_n);

/* Self-test: Hebrew RTL within LTR, Arabic numerals, bracket mirroring. */
int mini_bidi_selftest(void);

#ifdef __cplusplus
}
#endif
#endif /* MINI_BIDI_H */
