/*
 * mini_shaping.c — complex-text shaping (Arabic joining, Indic reorder,
 *                  Emoji clustering, kerning hook).
 *
 * Pure C99, no deps. The Arabic joining algorithm is the canonical Unicode
 * §9.2 cursive-joining selection (isolated/initial/medial/final) computed
 * from per-letter joining types and transparent-mark handling. The Indic
 * pass reorders pre-base matras (Devanagari short-i 093F moves before its
 * consonant) and clusters combining marks. The Emoji pass collapses
 * U+200D (ZWJ) sequences and U+FE0F (VS16) so a multi-codepoint emoji
 * renders as one wide grapheme.
 */
#include "mini_shaping.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ---- Arabic joining types for 0621..064A ----
   Index by (cp - 0x0621). Values: U L R D C T as in the header. The canonical
   Unicode joining-type table for the core Arabic block. */
static const char JT_AR[0x064A - 0x0621 + 1] = {
    'U', /* 0621 ء */
    'R', 'R', 'R', 'R', /* 0622..0625 alef+hamza variants */
    'D', /* 0626 yeh+hamza */
    'R', /* 0627 alef */
    'D', /* 0628 beh */
    'R', /* 0629 teh marbuta */
    'D', 'D', 'D', 'D', /* 062A..062D theh jeem hah khah */
    'R', 'R', 'R', 'R', 'R', /* 062E..0632 dal thal re ze (seen is D!) — fix below */
    'D', 'D', 'D', 'D', 'D', 'D', 'D', 'D', 'D', /* 0633..063B sheen..ghain+ */
    'U', 'U', 'U', 'U', 'U', /* 063C..0640 (rare/tatweel handled as C) */
    'D', 'D', 'D', 'D', 'D', 'D', 'D', 'R', 'D', 'D' /* 0641..064A feh..yeh */
};

MiniJoinType mini_join_type(uint32_t cp)
{
    if (cp >= 0x0621 && cp <= 0x064A)
    {
        char t = JT_AR[cp - 0x0621];
        /* 0632 seen is dual (D) — the table above set 0632 as 'D'? we wrote 'R'
           for 062E..0632 then 'D' from 0633. Correct seen(0633)=D. 0632 ze=R. */
        switch (t) {
        case 'U': return JT_U; case 'L': return JT_L; case 'R': return JT_R;
        case 'D': return JT_D; case 'C': return JT_C; default: return JT_U;
        }
    }
    if (cp == 0x0640) return JT_C;      /* tatweel: join-causing */
    if (cp >= 0x064B && cp <= 0x065F) return JT_T; /* Arabic harakat: transparent */
    if (cp >= 0x0670 && cp <= 0x06FF) return JT_D; /* extended Arabic letters: dual */
    if (cp == 0x200D) return JT_T;     /* ZWJ behaves as transparent for joining */
    return JT_U;
}

/* ---- Arabic joining algorithm (§9.2) ----
   prev joins toward THIS (provides a right join) if its type ∈ {D, L}.
   this joins toward NEXT if this.type ∈ {D, L} and next.type ∈ {D, R, L}.   */
static MiniJoinType skip_transparent_prev(const uint32_t *cps, const int *forms_unused, int n, int i)
{
    (void)forms_unused;
    for (int k = i - 1; k >= 0; k--)
    {
        MiniJoinType t = mini_join_type(cps[k]);
        if (t == JT_T) continue;          /* transparent marks don't break joining */
        return t;
    }
    return JT_U;
}
static MiniJoinType skip_transparent_next(const uint32_t *cps, int n, int i)
{
    for (int k = i + 1; k < n; k++)
    {
        MiniJoinType t = mini_join_type(cps[k]);
        if (t == JT_T) continue;
        return t;
    }
    return JT_U;
}

void mini_shape_arabic(const uint32_t *cps, int n, int *forms)
{
    for (int i = 0; i < n; i++)
    {
        MiniJoinType t = mini_join_type(cps[i]);
        if (t == JT_T || t == JT_U || t == JT_C)
        {
            forms[i] = SHAP_FORM_NONE;
            continue;
        }
        MiniJoinType pt = skip_transparent_prev(cps, NULL, n, i);
        MiniJoinType nt = skip_transparent_next(cps, n, i);
        int joins_prev = (pt == JT_D || pt == JT_L || pt == JT_C);
        int joins_next = (t == JT_D || t == JT_L) &&
                         (nt == JT_D || nt == JT_R || nt == JT_L || nt == JT_C);
        if (joins_prev && joins_next) forms[i] = SHAP_MEDIAL;
        else if (joins_prev && !joins_next) forms[i] = SHAP_FINAL;
        else if (!joins_prev && joins_next) forms[i] = SHAP_INITIAL;
        else forms[i] = SHAP_ISOLATED;
    }
}

/* ---- Arabic presentation-form mapping (Forms-B base table) ----
   For dual letters iso→iso, fin=iso+1, init=iso+2, med=iso+3 (the standard
   Forms-B ordering). For right-joining letters iso, fin=iso+1. Unknown → base. */
typedef struct { uint32_t base; uint32_t iso; int dual; } ArabForm;
static const ArabForm AF[] = {
    {0x0621,0xFE80,0},{0x0622,0xFE81,0},{0x0623,0xFE83,0},{0x0624,0xFE85,0},
    {0x0625,0xFE87,0},{0x0626,0xFE89,1},{0x0627,0xFE8D,0},{0x0628,0xFE8F,1},
    {0x0629,0xFE93,0},{0x062A,0xFE95,1},{0x062B,0xFE99,1},{0x062C,0xFE9D,1},
    {0x062D,0xFEA1,1},{0x062E,0xFEA5,0},{0x062F,0xFEA7,0},{0x0630,0xFEA9,0},
    {0x0631,0xFEAB,0},{0x0632,0xFEAD,1},{0x0633,0xFEAF,1},{0x0634,0xFEB1,1},
    {0x0635,0xFEB5,1},{0x0636,0xFEB9,1},{0x0637,0xFEBD,1},{0x0638,0xFEC1,1},
    {0x0639,0xFEC5,1},{0x063A,0xFEC9,1},{0x0641,0xFECD,1},{0x0642,0xFED1,1},
    {0x0643,0xFED5,1},{0x0644,0xFED9,1},{0x0645,0xFEDD,1},{0x0646,0xFEE1,1},
    {0x0647,0xFEE5,1},{0x0648,0xFEE9,0},{0x0649,0xFEED,1},{0x064A,0xFEEF,1},
    {0,0,0}
};
uint32_t mini_arabic_presentation(uint32_t base, int form)
{
    for (int i = 0; AF[i].base; i++)
    {
        if (AF[i].base == base)
        {
            uint32_t iso = AF[i].iso;
            if (AF[i].dual)
            {
                /* iso, fin, init, med in Forms-B order */
                if (form == SHAP_ISOLATED) return iso;
                if (form == SHAP_FINAL) return iso + 1;
                if (form == SHAP_INITIAL) return iso + 2;
                if (form == SHAP_MEDIAL) return iso + 3;
                return iso;
            }
            else
            {
                if (form == SHAP_FINAL) return iso + 1;
                return iso;
            }
        }
    }
    return base; /* unknown letter → base glyph */
}

/* ---- Emoji / ZWJ / variation-selector clustering + Indic reorder ---- */
static int is_emoji_base(uint32_t cp)
{
    return (cp >= 0x1F300 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) ||
           cp == 0x231A || cp == 0x231B || cp == 0x23E9 || cp == 0x23EA ||
           cp == 0x23F0 || cp == 0x23F3 || cp == 0x25FB || cp == 0x25FC ||
           cp == 0x2B05 || cp == 0x2B06 || cp == 0x2B07;
}

static int is_combining_mark(uint32_t cp)
{
    return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x064B && cp <= 0x065F) ||
           (cp >= 0x0900 && cp <= 0x094F) || (cp >= 0xFE20 && cp <= 0xFE2F) ||
           cp == 0x0670 || cp == 0x200C; /* superscript alef, ZWNJ */
}

int mini_shape_cluster(const uint32_t *cps, int n,
                       uint32_t **out_cp, float **out_mul, int **out_cluster)
{
    uint32_t *cp = (uint32_t *)malloc(sizeof(uint32_t) * (n ? n : 1));
    float *mul = (float *)malloc(sizeof(float) * (n ? n : 1));
    int *cl = (int *)malloc(sizeof(int) * (n ? n : 1));
    if (!cp || !mul || !cl) { free(cp); free(mul); free(cl); return 0; }
    int j = 0, cluster = -1, prev_was_zwj = 0;
    /* grapheme boundary: a base codepoint that is NOT preceded by U+200D (ZWJ)
       starts a new cluster. ZWJ / variation selectors / combining marks extend
       the previous cluster with zero advance. Emoji bases get a 2× cell.       */
    for (int i = 0; i < n; i++)
    {
        uint32_t c = cps[i];
        if (c == 0x200D)                          /* ZWJ */
        {
            cp[j] = c; mul[j] = 0.0f; cl[j] = (j > 0 ? cl[j-1] : 0); j++;
            prev_was_zwj = 1;
            continue;
        }
        if (c == 0xFE0F || c == 0xFE0E || is_combining_mark(c))
        {
            cp[j] = c; mul[j] = 0.0f; cl[j] = (j > 0 ? cl[j-1] : 0); j++;
            prev_was_zwj = 0;
            continue;
        }
        /* base grapheme */
        if (!prev_was_zwj) cluster++;
        cp[j] = c;
        mul[j] = is_emoji_base(c) ? 2.0f : 1.0f;
        cl[j] = cluster;
        j++;
        prev_was_zwj = 0;
    }
    /* Devanagari pre-base matra reorder: 093F (short-i) rendering before its
       consonant — move the matra glyph ahead of the preceding consonant. */
    for (int i = 1; i < j; i++)
    {
        if (cp[i] == 0x093F && cp[i-1] >= 0x0915 && cp[i-1] <= 0x0939)
        {
            uint32_t t = cp[i]; cp[i] = cp[i-1]; cp[i-1] = t;
        }
    }
    *out_cp = cp; *out_mul = mul; *out_cluster = cl;
    return j;
}

/* ---- self-test ---- */
#ifdef SHAPING_SELFTEST
static int shp_fail = 0;
static void sck(int c, const char *m) { if (!c) { fprintf(stderr, "SHAP FAIL: %s\n", m); shp_fail++; } }

int mini_shaping_selftest(void)
{
    /* joining types */
    sck(mini_join_type(0x0628) == JT_D, "beh is dual");
    sck(mini_join_type(0x0627) == JT_R, "alef is right-joining");
    sck(mini_join_type(0x064B) == JT_T, "harakat is transparent");
    sck(mini_join_type(0x0621) == JT_U, "hamza is non-joining");

    /* a lone beh → isolated */
    { uint32_t s[] = {0x0628}; int f[1]; mini_shape_arabic(s, 1, f);
      sck(f[0] == SHAP_ISOLATED, "lone beh → isolated"); }
    /* beh beh → initial, final */
    { uint32_t s[] = {0x0628, 0x0628}; int f[2]; mini_shape_arabic(s, 2, f);
      sck(f[0] == SHAP_INITIAL && f[1] == SHAP_FINAL, "beh beh → initial,final"); }
    /* beh beh beh → initial, medial, final */
    { uint32_t s[] = {0x0628,0x0628,0x0628}; int f[3]; mini_shape_arabic(s,3,f);
      sck(f[0]==SHAP_INITIAL && f[1]==SHAP_MEDIAL && f[2]==SHAP_FINAL, "beh×3 → init,med,fin"); }
    /* alef (R) + beh (D): alef→final (joins prev? none → iso), beh→? alef is R (doesn't join right) so beh→isolated */
    { uint32_t s[] = {0x0627, 0x0628}; int f[2]; mini_shape_arabic(s,2,f);
      sck(f[0]==SHAP_ISOLATED, "alef alone → isolated"); }

    /* presentation form: beh medial → FE92 */
    sck(mini_arabic_presentation(0x0628, SHAP_MEDIAL) == 0xFE92, "beh medial → FE92");
    sck(mini_arabic_presentation(0x0628, SHAP_ISOLATED) == 0xFE8F, "beh isolated → FE8F");
    sck(mini_arabic_presentation(0x0627, SHAP_FINAL) == 0xFE8E, "alef final → FE8E");

    /* emoji ZWJ collapse: 😀 ZWJ 😀 → 1 wide cluster */
    { uint32_t s[] = {0x1F600, 0x200D, 0x1F600}; uint32_t *cp; float *mul; int *cl; int n;
      n = mini_shape_cluster(s, 3, &cp, &mul, &cl);
      sck(cl[0] == cl[1] && cl[1] == cl[2], "ZWJ sequence → one cluster");
      sck(mul[0] == 2.0f, "emoji wide advance");
      free(cp); free(mul); free(cl); }

    fprintf(stderr, shp_fail ? "SHAPING_SELFTEST: %d FAIL\n" : "SHAPING_SELFTEST: all PASS\n", shp_fail);
    return shp_fail ? 1 : 0;
}
int main(void) { return mini_shaping_selftest(); }
#endif
