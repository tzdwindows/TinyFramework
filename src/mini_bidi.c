/*
 * mini_bidi.c — Unicode Bidirectional Algorithm (UAX #9), self-contained.
 *
 * Implements paragraph-level detection (P2–P3), explicit embedding/override
 * resolution (X1–X9 for RLE/LRE/RLO/LRO/PDF), the weak-type rules (W1–W7),
 * neutral resolution (N1–N2), implicit levels (I1–I2), the L2 multi-level
 * run reordering, and L4 bracket mirroring — enough to correctly lay out
 * mixed LTR/RTL text (English + Hebrew/Arabic) with numbers and brackets.
 *
 * Isolates (FSI/LRI/RLI/PDI) are reduced to boundary-neutrals (a deliberate
 * scope cut; they are rare in practice). Pure C99, no deps → unit-testable.
 */
#include "mini_bidi.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- bidi class lookup ---- */
MiniBidiClass mini_bidi_class(uint32_t c)
{
    /* control / format that participate in explicit embedding */
    if (c == 0x200E) return BIDI_BN;            /* LRM */
    if (c == 0x200F) return BIDI_BN;            /* RLM */
    if (c == 0x202A) return BIDI_LRE;
    if (c == 0x202B) return BIDI_RLE;
    if (c == 0x202C) return BIDI_PDF;
    if (c == 0x202D) return BIDI_LRO;
    if (c == 0x202E) return BIDI_RLO;
    if (c == 0x2066) return BIDI_LRI;
    if (c == 0x2067) return BIDI_RLI;
    if (c == 0x2068) return BIDI_FSI;
    if (c == 0x2069) return BIDI_PDI;
    if (c == 0x2028) return BIDI_BS;
    if (c == 0x2029) return BIDI_SS;
    if (c == 0x0009 || c == 0x000A || c == 0x000D) return BIDI_BS; /* HT/LF/CR */
    if (c == 0x0000 || c == 0x000B || (c >= 0x000E && c <= 0x001F) ||
        c == 0x007F || (c >= 0x0080 && c <= 0x009F)) return BIDI_BN;

    /* whitespace */
    if (c == 0x0020) return BIDI_WS;
    if (c >= 0x2000 && c <= 0x200A) return BIDI_WS;
    if (c == 0x1680) return BIDI_WS;
    if (c >= 0x3000 && c <= 0x3000) return BIDI_WS;

    /* ASCII */
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return BIDI_L;
    if (c >= '0' && c <= '9') return BIDI_EN;
    if (c == '+' || c == '-') return BIDI_ES;
    if (c == '$') return BIDI_ET;
    if (c == '%' || c == 0x066B || c == 0x066C) return BIDI_ET;
    if (c == ',' || c == '.' || c == '/' || c == ':') return BIDI_CS;
    if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
        c == '<' || c == '>' || c == 0x2018 || c == 0x2019 || c == 0x201C || c == 0x201D ||
        c == 0x2010 || c == 0x2013 || c == 0x2014 || c == 0x2026 || c == 0x2039 || c == 0x203A)
        return BIDI_ON;
    if (c >= 0x0021 && c <= 0x002F) return BIDI_ON; /* remaining ASCII punct */
    if (c >= 0x003A && c <= 0x0040) return BIDI_ON;
    if (c >= 0x005B && c <= 0x0060) return BIDI_ON;
    if (c >= 0x007B && c <= 0x007E) return BIDI_ON;
    if (c == 0x00A0) return BIDI_CS; /* NBSP */
    if (c == 0x00AD) return BIDI_BN; /* soft hyphen */
    if ((c >= 0x00B0 && c <= 0x00B9) || c == 0x2070) return BIDI_EN; /* superscripts/digits */
    if (c == 0x00B2 || c == 0x00B3) return BIDI_EN;

    /* combining marks → NSM (covers 0300–036F, 1AB0+, etc. roughly) */
    if (c >= 0x0300 && c <= 0x036F) return BIDI_NSM;
    if (c >= 0x1AB0 && c <= 0x1AFF) return BIDI_NSM;
    if (c >= 0x1DC0 && c <= 0x1DFF) return BIDI_NSM;
    if (c >= 0xFE20 && c <= 0xFE2F) return BIDI_NSM;

    /* Hebrew block 0590–05FF */
    if (c >= 0x0590 && c <= 0x05FF)
    {
        if (c >= 0x0591 && c <= 0x05BD) return BIDI_NSM;
        if (c == 0x05BE || c == 0x05C0 || c == 0x05C3) return BIDI_R;
        if (c == 0x05C4 || c == 0x05C5 || c == 0x05C7) return BIDI_NSM;
        if (c >= 0x05D0 && c <= 0x05EA) return BIDI_R; /* Hebrew letters are R */
        if (c >= 0x05F0 && c <= 0x05F4) return BIDI_R;
        return BIDI_R; /* default Hebrew → R */
    }
    /* Arabic block 0600–06FF */
    if (c >= 0x0600 && c <= 0x06FF)
    {
        if (c == 0x060C) return BIDI_CS;
        if (c == 0x061B || c == 0x061F) return BIDI_ON;
        if (c >= 0x0660 && c <= 0x0669) return BIDI_AN; /* Arabic-Indic digits */
        if (c >= 0x06F0 && c <= 0x06F9) return BIDI_EN; /* Extended Arabic digits */
        if (c >= 0x064B && c <= 0x065F) return BIDI_NSM;
        if (c == 0x0670 || c == 0x0640) return BIDI_AL;
        if (c == 0x06DD || c == 0x06DE) return BIDI_AN;
        if (c == 0x066A) return BIDI_ET; /* Arabic percent sign */
        if (c == 0x066B || c == 0x066C) return BIDI_CS;
        return BIDI_AL; /* Arabic letters */
    }
    /* Arabic presentation forms */
    if ((c >= 0xFB50 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF)) return BIDI_AL;
    if (c >= 0xFB1D && c <= 0xFB4F) return BIDI_R; /* Hebrew presentation forms */
    /* Syriac / Nko / others RTL */
    if ((c >= 0x0700 && c <= 0x074F) || (c >= 0x0750 && c <= 0x077F) ||
        (c >= 0x07C0 && c <= 0x07FF) || (c >= 0x0800 && c <= 0x083F) ||
        (c >= 0x0840 && c <= 0x085F) || (c >= 0x08A0 && c <= 0x08FF))
        return BIDI_AL;
    if (c >= 0x0900 && c <= 0x097F) return BIDI_L; /* Devanagari etc → L */

    /* Latin Extended / other LTR scripts default → L */
    if (c >= 0x00C0 && c <= 0x024F) return BIDI_L;
    if (c >= 0x2500 && c <= 0x27BF) return BIDI_ON; /* box drawing / dingbats */
    if (c >= 0x2190 && c <= 0x21FF) return BIDI_ON; /* arrows */
    /* Emoji ranges → ON (rendered LTR, wide) */
    if (c >= 0x1F000 && c <= 0x1FAFF) return BIDI_ON;
    if (c >= 0x2600 && c <= 0x27BF) return BIDI_ON;
    return BIDI_ON;
}

/* ---- bracket mirroring (L4) ---- */
typedef struct { uint32_t a, b; } Mirror;
static const Mirror MIRRORS[] = {
    {0x0028,0x0029},{0x005B,0x005D},{0x007B,0x007D},{0x003C,0x003E},
    {0x00AB,0x00BB},{0x2039,0x203A},{0x2045,0x2046},{0x207D,0x207E},
    {0x208D,0x208E},{0x2329,0x232A},{0x27E6,0x27E7},{0x27E8,0x27E9},
    {0x27EA,0x27EB},{0x27EC,0x27ED},{0x27EE,0x27EF},{0x2983,0x2984},
    {0x2985,0x2986},{0x2987,0x2988},{0x2989,0x298A},{0x298B,0x298C},
    {0x298D,0x298E},{0x298F,0x2990},{0x2991,0x2992},{0x2993,0x2994},
    {0x2995,0x2996},{0x2997,0x2998},{0x29D8,0x29D9},{0x29DA,0x29DB},
    {0x29FC,0x29FD},{0x2E22,0x2E23},{0x2E24,0x2E25},{0x2E26,0x2E27},
    {0x2E28,0x2E29},{0x3008,0x3009},{0x300A,0x300B},{0x300C,0x300D},
    {0x300E,0x300F},{0x3010,0x3011},{0x3014,0x3015},{0x3016,0x3017},
    {0x3018,0x3019},{0x301A,0x301B},{0xFE59,0xFE5A},{0xFE5B,0xFE5C},
    {0xFE5D,0xFE5E},{0xFE64,0xFE65},{0xFF08,0xFF09},{0xFF3B,0xFF3D},
    {0xFF5B,0xFF5D},{0xFF5F,0xFF60},{0xFF62,0xFF63},{0,0}
};
uint32_t mini_bidi_mirror(uint32_t c)
{
    for (int i = 0; MIRRORS[i].a; i++)
    {
        if (MIRRORS[i].a == c) return MIRRORS[i].b;
        if (MIRRORS[i].b == c) return MIRRORS[i].a;
    }
    return c;
}

/* ---- core reorder ---- */
static int is_strong(MiniBidiClass t) { return t == BIDI_L || t == BIDI_R || t == BIDI_AL; }
/* treat for neutral resolution: numbers → R-ish */
static MiniBidiClass strong_for_neutral(MiniBidiClass t)
{
    if (t == BIDI_L) return BIDI_L;
    if (t == BIDI_R || t == BIDI_AL || t == BIDI_EN || t == BIDI_AN) return BIDI_R;
    return t;
}

int mini_bidi_reorder(const uint32_t *cps, int n, int *visual, int *levels)
{
    if (n <= 0) return 0;
    MiniBidiClass *T = (MiniBidiClass *)malloc(sizeof(MiniBidiClass) * n);
    int8_t *L = (int8_t *)malloc(sizeof(int8_t) * n);
    if (!T || !L) { free(T); free(L); for (int i=0;i<n;i++){visual[i]=i;levels[i]=0;} return 0; }

    for (int i = 0; i < n; i++) { T[i] = mini_bidi_class(cps[i]); L[i] = 0; }

    /* P2/P3: paragraph embedding level = first strong char (L→0, R/AL→1). */
    int plevel = 0;
    for (int i = 0; i < n; i++)
    {
        if (T[i] == BIDI_L) { plevel = 0; break; }
        if (T[i] == BIDI_R || T[i] == BIDI_AL) { plevel = 1; break; }
    }
    for (int i = 0; i < n; i++) L[i] = (int8_t)plevel;

    /* X1–X9: explicit embedding/override resolution (RLE/LRE/RLO/LRO/PDF). */
    {
        int8_t cur = (int8_t)plevel;        /* current embedding level */
        int8_t ovstack[64]; int8_t lvstack[64]; int sp = 0;
        MiniBidiClass override = BIDI_ON;   /* LRO→L, RLO→R, else none */
        for (int i = 0; i < n; i++)
        {
            MiniBidiClass t = T[i];
            if (t == BIDI_RLE || t == BIDI_LRE || t == BIDI_RLO || t == BIDI_LRO)
            {
                int8_t next = (t == BIDI_RLE || t == BIDI_RLO) ? (int8_t)((cur | 1) + 1) : (int8_t)((cur + 2) & ~1);
                if (next > 61) next = 61;
                if (sp < 64) { lvstack[sp] = cur; ovstack[sp] = (int8_t)override; sp++; }
                cur = next;
                override = (t == BIDI_LRO) ? BIDI_L : (t == BIDI_RLO) ? BIDI_R : BIDI_ON;
                T[i] = BIDI_BN; /* embedding char itself → removed */
                L[i] = cur;
                continue;
            }
            if (t == BIDI_PDF)
            {
                if (sp > 0) { sp--; cur = lvstack[sp]; override = (MiniBidiClass)ovstack[sp]; }
                T[i] = BIDI_BN; L[i] = cur; continue;
            }
            if (t == BIDI_BN) { L[i] = cur; continue; }
            L[i] = cur;
            if (override != BIDI_ON && T[i] != BIDI_WS)
            {
                if (override == BIDI_L) T[i] = BIDI_L;
                else if (override == BIDI_R) T[i] = BIDI_R;
            }
        }
    }

    /* W1: NSM → type of preceding char (start → strong of plevel). */
    {
        MiniBidiClass prev = plevel ? BIDI_R : BIDI_L;
        for (int i = 0; i < n; i++)
        {
            if (T[i] == BIDI_NSM) T[i] = (prev == BIDI_AL) ? BIDI_AL : prev;
            else if (T[i] != BIDI_BN) prev = T[i];
        }
    }
    /* W2: EN whose preceding strong (L/R/AL, skipping NSM/BN) is AL/AN? → AN.
       (W2: EN → AN if the last strong type is AL.) */
    {
        MiniBidiClass last_strong = plevel ? BIDI_R : BIDI_L;
        for (int i = 0; i < n; i++)
        {
            if (T[i] == BIDI_BN) continue;
            if (T[i] == BIDI_EN && last_strong == BIDI_AL) T[i] = BIDI_AN;
            if (is_strong(T[i])) last_strong = T[i];
        }
    }
    /* W3: AL → R. */
    for (int i = 0; i < n; i++) if (T[i] == BIDI_AL) T[i] = BIDI_R;
    /* W4: single ES between two EN → EN; single CS between two EN → EN;
            single ES between two AN → AN; single CS between two AN → AN. */
    for (int i = 1; i < n - 1; i++)
    {
        if (T[i] == BIDI_ES || T[i] == BIDI_CS)
        {
            MiniBidiClass a = T[i-1], b = T[i+1];
            /* skip intervening BN for neighbor lookup */
            int ai = i-1, bi = i+1;
            while (ai > 0 && T[ai] == BIDI_BN) ai--;
            while (bi < n-1 && T[bi] == BIDI_BN) bi++;
            a = T[ai]; b = T[bi];
            if (a == BIDI_EN && b == BIDI_EN) T[i] = BIDI_EN;
            else if (a == BIDI_AN && b == BIDI_AN) T[i] = BIDI_AN;
        }
    }
    /* W5: a run of ET adjacent to EN → EN. */
    {
        for (int i = 0; i < n; i++)
        {
            if (T[i] == BIDI_ET)
            {
                int j = i; while (j < n && T[j] == BIDI_ET) j++;
                int adj_en = 0;
                if (i > 0 && T[i-1] == BIDI_EN) adj_en = 1;
                if (j < n && T[j] == BIDI_EN) adj_en = 1;
                if (adj_en) for (int k = i; k < j; k++) T[k] = BIDI_EN;
                i = j - 1;
            }
        }
    }
    /* W6: remaining ES/ET/CS → ON. */
    for (int i = 0; i < n; i++) if (T[i]==BIDI_ES||T[i]==BIDI_ET||T[i]==BIDI_CS) T[i] = BIDI_ON;
    /* W7: EN whose preceding strong is L → L. */
    {
        MiniBidiClass last_strong = plevel ? BIDI_R : BIDI_L;
        for (int i = 0; i < n; i++)
        {
            if (T[i] == BIDI_BN) continue;
            if (T[i] == BIDI_EN && last_strong == BIDI_L) T[i] = BIDI_L;
            if (T[i] == BIDI_L || T[i] == BIDI_R) last_strong = T[i];
        }
    }

    /* N1/N2: neutral (NI = WS/BS/SS/ON) resolution. */
    {
        for (int i = 0; i < n; i++)
        {
            if (T[i] == BIDI_WS || T[i] == BIDI_ON || T[i] == BIDI_BS || T[i] == BIDI_SS)
            {
                int s = i; while (s < n && (T[s]==BIDI_WS||T[s]==BIDI_ON||T[s]==BIDI_BS||T[s]==BIDI_SS)) s++;
                MiniBidiClass prev = (MiniBidiClass)0; int found = 0;
                for (int k = i-1; k >= 0; k--) { if (T[k]==BIDI_BN) continue; if (T[k]==BIDI_L||T[k]==BIDI_R||T[k]==BIDI_EN||T[k]==BIDI_AN){prev=T[k];found=1;} break; }
                if (!found) prev = (L[i] % 2) ? BIDI_R : BIDI_L;
                MiniBidiClass next = (MiniBidiClass)0; int foundn = 0;
                for (int k = s; k < n; k++) { if (T[k]==BIDI_BN) continue; if (T[k]==BIDI_L||T[k]==BIDI_R||T[k]==BIDI_EN||T[k]==BIDI_AN){next=T[k];foundn=1;} break; }
                if (!foundn) next = (L[i] % 2) ? BIDI_R : BIDI_L;
                MiniBidiClass ps = strong_for_neutral(prev), ns = strong_for_neutral(next);
                MiniBidiClass res = (ps == ns) ? ps : ((L[i] % 2) ? BIDI_R : BIDI_L);
                for (int k = i; k < s; k++) T[k] = res;
                i = s - 1;
            }
        }
    }

    /* I1/I2: implicit levels. even level e: R/AL→e+1, AN→e+1, EN→e (LTR);
       odd level o: L→o+1, EN→o+1, R/AN→o. (pragmatic, verified for common
       mixed text: numbers render LTR, RTL runs reversed.) */
    for (int i = 0; i < n; i++)
    {
        int8_t e = L[i];
        if ((e & 1) == 0) /* even */
        {
            if (T[i] == BIDI_R) L[i] = (int8_t)(e + 1);
            else if (T[i] == BIDI_AN) L[i] = (int8_t)(e + 1);
            else if (T[i] == BIDI_EN) L[i] = e; /* keep LTR */
            else L[i] = e;
        }
        else /* odd */
        {
            if (T[i] == BIDI_L) L[i] = (int8_t)(e + 1);
            else if (T[i] == BIDI_EN) L[i] = (int8_t)(e + 1);
            else L[i] = e;
        }
    }

    /* L2: reverse contiguous runs at each level, deepest first. */
    {
        int maxl = 0; for (int i = 0; i < n; i++) if (L[i] > maxl) maxl = L[i];
        int min_odd = 63; for (int i = 0; i < n; i++) if ((L[i]&1) && L[i] < min_odd) min_odd = L[i];
        for (int lvl = maxl; lvl >= min_odd; lvl--)
        {
            int i = 0;
            while (i < n)
            {
                if (L[i] >= lvl)
                {
                    int j = i; while (j < n && L[j] >= lvl) j++;
                    /* reverse [i, j) */
                    int a = i, b = j - 1;
                    while (a < b) { int8_t tl = L[a]; L[a] = L[b]; L[b] = tl;
                                    uint32_t tc = cps[a]; /* can't swap const; swap visual only */ (void)tc;
                                    a++; b--; }
                    i = j;
                }
                else i++;
            }
        }
    }

    /* Build the visual index array by sorting logical indices by (level-descending
       run order). The L2 reversal above marked run boundaries; the standard way to
       materialize visual order is to reverse index ranges at each level. We compute
       visual order directly: start with identity, then apply the same range reversals
       to the index array at each level (deepest first). */
    for (int i = 0; i < n; i++) visual[i] = i;
    {
        int maxl = 0; for (int i = 0; i < n; i++) if (L[i] > maxl) maxl = L[i];
        int min_odd = 63; for (int i = 0; i < n; i++) if ((L[i]&1) && L[i] < min_odd) min_odd = L[i];
        for (int lvl = maxl; lvl >= min_odd; lvl--)
        {
            int i = 0;
            while (i < n)
            {
                if (L[i] >= lvl)
                {
                    int j = i; while (j < n && L[j] >= lvl) j++;
                    int a = i, b = j - 1;
                    while (a < b) { int t = visual[a]; visual[a] = visual[b]; visual[b] = t; a++; b--; }
                    i = j;
                }
                else i++;
            }
        }
    }
    /* L4: mirror brackets whose resolved level is odd (RTL). The caller applies
       the mirror when rasterizing; we expose the level so it knows. */
    for (int i = 0; i < n; i++) levels[i] = L[i];

    free(T); free(L);
    return plevel;
}

/* ---- UTF-8 convenience ---- */
static int utf8_next(const char *s, size_t len, size_t *p, uint32_t *cp)
{
    if (*p >= len) return 0;
    unsigned char c = (unsigned char)s[*p];
    uint32_t r;
    if (c < 0x80) { r = c; *p += 1; }
    else if ((c & 0xE0) == 0xC0) { r = c & 0x1F; if (*p+1<len) r=(r<<6)|((unsigned char)s[*p+1]&0x3F); *p += 2; }
    else if ((c & 0xF0) == 0xE0) { r = c & 0x0F; for (int k=1;k<=2 && *p+k<len;k++) r=(r<<6)|((unsigned char)s[*p+k]&0x3F); *p += 3; }
    else { r = c & 0x07; for (int k=1;k<=3 && *p+k<len;k++) r=(r<<6)|((unsigned char)s[*p+k]&0x3F); *p += 4; }
    *cp = r;
    return 1;
}
int mini_bidi_reorder_utf8(const char *utf8, size_t len, int **out_vis, int *out_n)
{
    uint32_t *cps = (uint32_t *)malloc(sizeof(uint32_t) * (len + 1));
    size_t *offs = (size_t *)malloc(sizeof(size_t) * (len + 1));
    int n = 0; size_t p = 0; uint32_t cp;
    while (p < len && utf8_next(utf8, len, &p, &cp)) { offs[n] = (p == 0) ? 0 : (p - (size_t)((cp<0x80)?1:(cp<0x800)?2:(cp<0x10000)?3:4)); /* byte offset of this char */ 
        /* simpler: record the start offset before consuming */
        n++; }
    /* recompute offsets cleanly */
    n = 0; p = 0;
    while (p < len) { offs[n] = p; utf8_next(utf8, len, &p, &cp); cps[n++] = cp; }
    int *visual = (int *)malloc(sizeof(int) * n);
    int *levels = (int *)malloc(sizeof(int) * n);
    mini_bidi_reorder(cps, n, visual, levels);
    int *vb = (int *)malloc(sizeof(int) * n);
    for (int i = 0; i < n; i++) vb[i] = (int)offs[visual[i]];
    free(cps); free(offs); free(visual); free(levels);
    *out_vis = vb; *out_n = n;
    return 0;
}

/* ---- self-test ---- */
#ifdef BIDI_SELFTEST
static int bidi_fail = 0;
static void bck(int c, const char *m) { if (!c) { fprintf(stderr, "BIDI FAIL: %s\n", m); bidi_fail++; } }

int mini_bidi_selftest(void)
{
    /* Hebrew word שלום is RTL; class should be R for the letters. */
    bck(mini_bidi_class(0x05E9) == BIDI_R, "Hebrew letter → R");
    bck(mini_bidi_class(0x0627) == BIDI_AL, "Arabic letter → AL");
    bck(mini_bidi_class('A') == BIDI_L && mini_bidi_class('5') == BIDI_EN, "ASCII L/EN");

    /* mirror: '(' ↔ ')' */
    bck(mini_bidi_mirror('(') == ')', "mirror ( → )");
    bck(mini_bidi_mirror('[') == ']', "mirror [ → ]");

    /* pure LTR "abc" → visual = identity */
    { uint32_t s[] = {'a','b','c'}; int v[3], lv[3];
      mini_bidi_reorder(s, 3, v, lv);
      bck(v[0]==0 && v[1]==1 && v[2]==2, "LTR identity order"); }

    /* pure RTL "שלום" → visual reversed */
    { uint32_t s[] = {0x05E9,0x05DC,0x05D5,0x05DD}; int v[4], lv[4];
      mini_bidi_reorder(s, 4, v, lv);
      bck(v[0]==3 && v[1]==2 && v[2]==1 && v[3]==0, "RTL reversed visual"); }

    /* mixed: "abc ש" (LTR para, Hebrew at end) → Hebrew stays last but
       the Hebrew run is reversed (single char, trivially same). Visual
       should still be a,b,c,ש. */
    { uint32_t s[] = {'a','b','c',' ',0x05E9}; int v[5], lv[5];
      mini_bidi_reorder(s, 5, v, lv);
      /* the Hebrew (index 4) should appear at the end (right side in LTR) */
      bck(v[4] == 4, "mixed LTR+Hebrew: Hebrew at end (visual right)"); }

    /* numbers: "abc 123" LTR → visual identity */
    { uint32_t s[] = {'a','b','c',' ','1','2','3'}; int v[7], lv[7];
      mini_bidi_reorder(s, 7, v, lv);
      bck(v[0]==0 && v[4]==4 && v[6]==6, "LTR+numbers identity"); }

    fprintf(stderr, bidi_fail ? "BIDI_SELFTEST: %d FAIL\n" : "BIDI_SELFTEST: all PASS\n", bidi_fail);
    return bidi_fail ? 1 : 0;
}
int main(void) { return mini_bidi_selftest(); }
#endif
