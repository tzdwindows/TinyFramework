/*
 * mini_css.c — CSS value engine + at-rule support (var/calc/min/max/clamp,
 *              @media, @keyframes, @font-face).
 *
 * Pure C99 (no DOM/render deps) so it is unit-testable standalone. Wired into
 * mini_dom.c's mini_css_apply (var substitution + calc routing + at-rule
 * parsing) and mini_calc_resolve (deferred expression evaluation).
 *
 * The math evaluator is a recursive-descent parser:
 *   expr   := term (('+'|'-') term)*
 *   term   := factor (('*'|'/') factor)*
 *   factor := number unit | '(' expr ')' | 'calc(' expr ')'
 *           | 'min(' expr (',' expr)* ')' | 'max(' expr (',')* ')'
 *           | 'clamp(' expr ',' expr ',' expr ')'
 * Length units are converted to px against the supplied MiniCssCtx at the
 * leaf, so every factor yields a px float and min/max/clamp compare px values.
 */
#include "mini_css.h"
#include "mini_dom.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

/* ================================================================== */
/* helpers                                                             */
/* ================================================================== */
static char *dup_n(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p)
        return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}
static int starts_with(const char *s, const char *pre)
{
    while (*pre)
    {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*pre))
            return 0;
        s++;
        pre++;
    }
    return 1;
}
static const char *skip_ws(const char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    return s;
}

/* ================================================================== */
/* CSS custom properties (var() registry)                              */
/* ================================================================== */
typedef struct MiniCssVar
{
    char *name;
    char *value;
} MiniCssVar;

static MiniCssVar *g_vars = NULL;
static int g_vars_n = 0, g_vars_cap = 0;

void mini_css_var_reset(void)
{
    for (int i = 0; i < g_vars_n; i++)
    {
        free(g_vars[i].name);
        free(g_vars[i].value);
    }
    g_vars_n = 0;
    /* invalidate the resolution cache: every var potentially changed */
    mini_css_var_cache_invalidate();
}

void mini_css_var_set(const char *name, const char *value)
{
    if (!name)
        return;
    for (int i = 0; i < g_vars_n; i++)
        if (!strcmp(g_vars[i].name, name))
        {
            /* only bump gen + replace if the value actually changed —
               avoids needless cache churn when the same token is re-declared
               on every cascade apply pass (the common case for :root). */
            if (value && g_vars[i].value && !strcmp(g_vars[i].value, value))
                return;
            free(g_vars[i].value);
            g_vars[i].value = strdup(value ? value : "");
            mini_css_var_cache_invalidate();
            return;
        }
    if (g_vars_n == g_vars_cap)
    {
        int nc = g_vars_cap ? g_vars_cap * 2 : 32;
        MiniCssVar *nv = (MiniCssVar *)realloc(g_vars, nc * sizeof(*nv));
        if (!nv)
            return;
        g_vars = nv;
        g_vars_cap = nc;
    }
    g_vars[g_vars_n].name = strdup(name);
    g_vars[g_vars_n].value = strdup(value ? value : "");
    g_vars_n++;
    mini_css_var_cache_invalidate();
}

const char *mini_css_var_get(const char *name)
{
    if (!name)
        return "";
    for (int i = 0; i < g_vars_n; i++)
        if (!strcmp(g_vars[i].name, name))
            return g_vars[i].value;
    return "";
}

/* Look up variable with DOM tree scope fallback */
static const char *get_var_scoped(const char *name, const void *node)
{
    if (!name)
        return "";
    if (node)
    {
        const MiniNode *n = (const MiniNode *)node;
        while (n)
        {
            for (const MiniNodeVar *v = n->vars; v; v = v->next)
            {
                if (v->name && !strcmp(v->name, name))
                    return v->value ? v->value : "";
            }
            n = n->parent;
        }
    }
    return mini_css_var_get(name);
}

/* Resolve a single var(...) occurrence beginning at `*pp` */
static void resolve_one_var_scoped(const char **pp, char **out, size_t *olen, size_t *ocap, const void *node)
{
    const char *p = skip_ws(*pp);
    if (*p != '(')
    {
        if (*olen + 4 > *ocap)
        {
            *ocap += 16;
            *out = realloc(*out, *ocap);
        }
        (*out)[(*olen)++] = 'v';
        (*out)[(*olen)++] = 'a';
        (*out)[(*olen)++] = 'r';
        (*out)[*olen] = 0;
        *pp = p;
        return;
    }
    p++; /* skip '(' */
    /* read the var name (custom property: --xxx) until ',' or ')' */
    const char *name_start = p;
    int depth = 1;
    while (*p && depth > 0)
    {
        if (*p == '(')
            depth++;
        else if (*p == ')')
        {
            depth--;
            if (depth == 0)
                break;
        }
        else if (*p == ',' && depth == 1)
            break;
        p++;
    }
    size_t name_len = (size_t)(p - name_start);
    while (name_len > 0 && isspace((unsigned char)name_start[name_len - 1]))
        name_len--;
    while (name_len > 0 && isspace((unsigned char)name_start[0]))
    {
        name_start++;
        name_len--;
    }
    char *name = dup_n(name_start, name_len);
    /* fallback? */
    const char *fallback = NULL;
    if (*p == ',')
    {
        p++;
        const char *fb_start = p;
        int d = 1;
        while (*p && d > 0)
        {
            if (*p == '(')
                d++;
            else if (*p == ')')
            {
                d--;
                if (d == 0)
                    break;
            }
            p++;
        }
        const char *fbe = p;
        while (fb_start < fbe && isspace((unsigned char)*fb_start))
            fb_start++;
        while (fbe > fb_start && isspace((unsigned char)fbe[-1]))
            fbe--;
        fallback = dup_n(fb_start, (size_t)(fbe - fb_start));
    }
    if (*p == ')')
        p++;
    *pp = p;
    const char *look = name;
    if (look[0] == '-' && look[1] == '-')
        look += 2;
    const char *val = get_var_scoped(look, node);
    const char *use = (val && val[0]) ? val : fallback;
    if (!use)
        use = "0";
    size_t ul = strlen(use);
    if (*olen + ul + 1 > *ocap)
    {
        *ocap = (*olen + ul + 1) * 2;
        *out = realloc(*out, *ocap);
    }
    memcpy(*out + *olen, use, ul);
    *olen += ul;
    (*out)[*olen] = 0;
    free(name);
    free((void *)fallback);
}

static char *resolve_vars_uncached(const char *value, const void *node)
{
    if (!value)
        return strdup("");
    char *cur = strdup(value);
    for (int pass = 0; pass < 8; pass++)
    {
        if (!strstr(cur, "var(") && !strstr(cur, "var "))
            break;
        size_t ocap = strlen(cur) + 32;
        char *out = (char *)malloc(ocap);
        if (!out)
            break;
        size_t olen = 0;
        const char *p = cur;
        while (*p)
        {
            if (p[0] == 'v' && p[1] == 'a' && p[2] == 'r' && (p[3] == '(' || isspace((unsigned char)p[3])))
            {
                const char *pp = p + 3;
                resolve_one_var_scoped(&pp, &out, &olen, &ocap, node);
                p = pp;
            }
            else
            {
                if (olen + 2 > ocap)
                {
                    ocap *= 2;
                    out = (char *)realloc(out, ocap);
                }
                out[olen++] = *p++;
                out[olen] = 0;
            }
        }
        out[olen] = 0;
        free(cur);
        cur = out;
    }
    return cur;
}

/* ================================================================== */
/* var() resolution cache                                              */
/* ================================================================== */
/* Resolving var() is expensive: up to 8 textual passes, each doing
   strstr + char-by-char realloc, plus a per-occurrence ancestor-chain
   walk for scoped lookup. On a design-token-heavy page (559 var() uses)
   the same (node, value) pair is resolved many times — once per
   matching decl during cascade apply, then AGAIN at layout time inside
   mini_calc_resolve for deferred calc()/clamp() length fields. This
   memo cache keys on (node ptr, value string) and is invalidated
   wholesale by bumping g_var_gen whenever any custom property changes
   (mini_css_var_set / mini_css_var_reset / mini_css_var_cache_invalidate
   called from node destruction to defeat stale node-ptr reuse).        */
typedef struct
{
    const void *node; /* MiniNode* or NULL */
    char *value;      /* strdup'd key: original (pre-resolution) value string */
    char *resolved;   /* strdup'd cached resolved string (caller frees copy) */
    uint64_t gen;     /* g_var_gen at compute time; 0 = empty slot */
} VarCacheEntry;

#define MINI_VAR_CACHE_CAP 8192 /* power of two; ~8K slots ≈ few MB worst case */
static VarCacheEntry g_var_cache[MINI_VAR_CACHE_CAP];
static uint64_t g_var_gen = 1; /* start at 1 so 0 = empty slot sentinel */

static uint32_t var_cache_hash(const void *node, const char *value)
{
    uint32_t h = 2166136261u;
    /* mix node ptr in first so different nodes with the same value hash
       differently (scoped-var correctness — the same value string can
       resolve differently at nodes with different scoped vars). */
    uintptr_t np = (uintptr_t)node;
    np ^= (np >> 33);
    for (int i = 0; i < (int)sizeof(np); i++)
    {
        h ^= (uint8_t)(np >> (i * 8));
        h *= 16777619u;
    }
    for (const char *p = value; *p; p++)
    {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    return h & (MINI_VAR_CACHE_CAP - 1);
}

void mini_css_var_cache_invalidate(void)
{
    /* Wholesale invalidation via generation bump. Stale slots are lazily
       reclaimed on next store; memory is bounded by the fixed cap, so we
       do not walk to free here (cheap on the hot var-set path).           */
    g_var_gen++;
}

static char *var_cache_lookup(const void *node, const char *value)
{
    uint32_t h = var_cache_hash(node, value);
    for (int i = 0; i < 8; i++)
    {
        VarCacheEntry *e = &g_var_cache[(h + i) & (MINI_VAR_CACHE_CAP - 1)];
        if (!e->value)
            return NULL; /* empty slot ends the probe chain for this hash */
        if (e->gen == g_var_gen && e->node == node && !strcmp(e->value, value))
            return e->resolved;
    }
    return NULL;
}

static void var_cache_store(const void *node, const char *value, const char *resolved)
{
    uint32_t h = var_cache_hash(node, value);
    for (int i = 0; i < 8; i++)
    {
        VarCacheEntry *e = &g_var_cache[(h + i) & (MINI_VAR_CACHE_CAP - 1)];
        if (!e->value || e->gen != g_var_gen)
        {
            /* empty slot, or stale (from a previous gen) — claim it */
            free(e->value);
            free(e->resolved);
            e->node = node;
            e->value = strdup(value);
            e->resolved = strdup(resolved);
            e->gen = g_var_gen;
            return;
        }
    }
    /* 8-slot probe chain full of current-gen entries: skip caching this
       one (rare under the cap; correctness unaffected, just a miss). */
}

/* resolve vars but consult the memo cache first; store on miss. This is
   the public entry point — all callers (mini_style_set, mini_calc_resolve)
   go through here so the cache is consulted everywhere for free. */
char *mini_css_resolve_vars_node(const char *value, const void *node)
{
    if (!value)
        return strdup("");
    char *hit = var_cache_lookup(node, value);
    if (hit)
        return strdup(hit);
    char *res = resolve_vars_uncached(value, node);
    if (res)
        var_cache_store(node, value, res);
    return res;
}

char *mini_css_resolve_vars(const char *value)
{
    return mini_css_resolve_vars_node(value, NULL);
}

/* ================================================================== */
/* CSS math expression evaluator (calc / min / max / clamp)            */
/* ================================================================== */
typedef struct
{
    const char *p;
    const MiniCssCtx *ctx;
    int ok;
} Eval;

static float eval_expr(Eval *e);

/* parse a number (with optional sign, decimal, exponent) */
static float parse_number(const char **pp)
{
    const char *p = skip_ws(*pp);
    const char *s = p;
    if (*p == '+' || *p == '-')
        p++;
    while (*p && (isdigit((unsigned char)*p) || *p == '.'))
        p++;
    if (*p == 'e' || *p == 'E')
    {
        p++;
        if (*p == '+' || *p == '-')
            p++;
        while (isdigit((unsigned char)*p))
            p++;
    }
    char buf[32];
    size_t l = (size_t)(p - s);
    if (l >= sizeof(buf))
        l = sizeof(buf) - 1;
    memcpy(buf, s, l);
    buf[l] = 0;
    *pp = p;
    return (float)atof(buf);
}

/* parse a unit suffix and convert value→px against ctx */
static float conv_unit(float v, const char **pp, const MiniCssCtx *ctx)
{
    const char *p = skip_ws(*pp);
    /* recognize unit tokens */
    if (p[0] == 'p' && p[1] == 'x')
    {
        *pp = p + 2;
        return v;
    }
    if (p[0] == '%')
    {
        *pp = p + 1;
        return v * ctx->pct_base / 100.0f;
    }
    if (p[0] == 'e' && p[1] == 'm')
    {
        *pp = p + 2;
        return v * ctx->font_px;
    }
    if (p[0] == 'r' && p[1] == 'e' && p[2] == 'm')
    {
        *pp = p + 3;
        return v * ctx->root_font_px;
    }
    if (p[0] == 'v' && p[1] == 'w')
    {
        *pp = p + 2;
        return v * ctx->vw / 100.0f;
    }
    if (p[0] == 'v' && p[1] == 'h')
    {
        *pp = p + 2;
        return v * ctx->vh / 100.0f;
    }
    if (p[0] == 'v' && p[1] == 'm' && p[2] == 'i' && p[3] == 'n')
    {
        *pp = p + 4;
        float m = ctx->vw < ctx->vh ? ctx->vw : ctx->vh;
        return v * m / 100.0f;
    }
    if (p[0] == 'v' && p[1] == 'm' && p[2] == 'a' && p[3] == 'x')
    {
        *pp = p + 4;
        float m = ctx->vw > ctx->vh ? ctx->vw : ctx->vh;
        return v * m / 100.0f;
    }
    if (p[0] == 'c' && p[1] == 'h')
    {
        *pp = p + 2;
        return v * ctx->font_px * 0.5f;
    }
    if (p[0] == 'i' && p[1] == 'c' && p[2] == 'q')
    {
        *pp = p + 3;
        return v * ctx->font_px * 0.5f;
    }
    /* angle units (normalized to radians for trigonometric functions) */
    if (p[0] == 'd' && p[1] == 'e' && p[2] == 'g')
    {
        *pp = p + 3;
        return v * 0.017453292519943295f;
    }
    if (p[0] == 'r' && p[1] == 'a' && p[2] == 'd')
    {
        *pp = p + 3;
        return v;
    }
    if (p[0] == 'g' && p[1] == 'r' && p[2] == 'a' && p[3] == 'd')
    {
        *pp = p + 4;
        return v * 0.015707963267948967f;
    }
    if (p[0] == 't' && p[1] == 'u' && p[2] == 'r' && p[3] == 'n')
    {
        *pp = p + 4;
        return v * 6.283185307179586f;
    }
    return v;
}

static float eval_factor(Eval *e)
{
    const char *p = skip_ws(e->p);
    /* calc(...) */
    if (starts_with(p, "calc("))
    {
        e->p = p + 5;
        float v = eval_expr(e);
        p = skip_ws(e->p);
        if (*p == ')')
            e->p = p + 1;
        return v;
    }
    /* min(...) / max(...) */
    if (starts_with(p, "min(") || starts_with(p, "max("))
    {
        int is_min = (p[1] == 'i');
        e->p = p + 4;
        float best = 0;
        int first = 1;
        for (;;)
        {
            float v = eval_expr(e);
            p = skip_ws(e->p);
            if (first || (is_min ? v < best : v > best))
            {
                best = v;
                first = 0;
            }
            if (*p == ',')
            {
                e->p = p + 1;
                continue;
            }
            if (*p == ')')
            {
                e->p = p + 1;
                break;
            }
            break;
        }
        return best;
    }
    /* clamp(a, b, c) → clamp b to [a, c] */
    if (starts_with(p, "clamp("))
    {
        e->p = p + 6;
        float a = eval_expr(e);
        p = skip_ws(e->p);
        if (*p == ',')
            e->p = p + 1;
        float b = eval_expr(e);
        p = skip_ws(e->p);
        if (*p == ',')
            e->p = p + 1;
        float c = eval_expr(e);
        p = skip_ws(e->p);
        if (*p == ')')
            e->p = p + 1;
        if (b < a)
            return a;
        if (b > c)
            return c;
        return b;
    }
    /* sin / cos / tan */
    if (starts_with(p, "sin(") || starts_with(p, "cos(") || starts_with(p, "tan("))
    {
        int is_sin = starts_with(p, "sin(");
        int is_cos = starts_with(p, "cos(");
        e->p = p + 4;
        float val = eval_expr(e);
        p = skip_ws(e->p);
        if (*p == ')')
            e->p = p + 1;
        return is_sin ? sinf(val) : is_cos ? cosf(val)
                                           : tanf(val);
    }
    /* asin / acos / atan / atan2 */
    if (starts_with(p, "asin(") || starts_with(p, "acos(") || starts_with(p, "atan("))
    {
        int is_asin = starts_with(p, "asin(");
        int is_acos = starts_with(p, "acos(");
        e->p = p + (is_asin ? 5 : is_acos ? 5
                                          : 5);
        float val = eval_expr(e);
        p = skip_ws(e->p);
        if (*p == ')')
            e->p = p + 1;
        return is_asin ? asinf(val) : is_acos ? acosf(val)
                                              : atanf(val);
    }
    if (starts_with(p, "atan2("))
    {
        e->p = p + 6;
        float y = eval_expr(e);
        p = skip_ws(e->p);
        if (*p == ',')
            e->p = p + 1;
        float x = eval_expr(e);
        p = skip_ws(e->p);
        if (*p == ')')
            e->p = p + 1;
        return atan2f(y, x);
    }
    /* sqrt(...) */
    if (starts_with(p, "sqrt("))
    {
        e->p = p + 5;
        float val = eval_expr(e);
        p = skip_ws(e->p);
        if (*p == ')')
            e->p = p + 1;
        return val >= 0.0f ? sqrtf(val) : 0.0f;
    }
    /* pow(a, b) */
    if (starts_with(p, "pow("))
    {
        e->p = p + 4;
        float a = eval_expr(e);
        p = skip_ws(e->p);
        if (*p == ',')
            e->p = p + 1;
        float b = eval_expr(e);
        p = skip_ws(e->p);
        if (*p == ')')
            e->p = p + 1;
        return powf(a, b);
    }
    /* abs(...) */
    if (starts_with(p, "abs("))
    {
        e->p = p + 4;
        float val = eval_expr(e);
        p = skip_ws(e->p);
        if (*p == ')')
            e->p = p + 1;
        return fabsf(val);
    }
    /* mod(a, b) / rem(a, b) */
    if (starts_with(p, "mod(") || starts_with(p, "rem("))
    {
        e->p = p + 4;
        float a = eval_expr(e);
        p = skip_ws(e->p);
        if (*p == ',')
            e->p = p + 1;
        float b = eval_expr(e);
        p = skip_ws(e->p);
        if (*p == ')')
            e->p = p + 1;
        return (b != 0.0f) ? fmodf(a, b) : 0.0f;
    }
    /* round(...) */
    if (starts_with(p, "round("))
    {
        e->p = p + 6;
        const char *sub = skip_ws(e->p);
        int mode = 0; /* 0=nearest, 1=up, 2=down, 3=to-zero */
        if (starts_with(sub, "nearest,"))
        {
            mode = 0;
            e->p = sub + 8;
        }
        else if (starts_with(sub, "up,"))
        {
            mode = 1;
            e->p = sub + 3;
        }
        else if (starts_with(sub, "down,"))
        {
            mode = 2;
            e->p = sub + 5;
        }
        else if (starts_with(sub, "to-zero,"))
        {
            mode = 3;
            e->p = sub + 8;
        }
        float a = eval_expr(e);
        float step = 1.0f;
        p = skip_ws(e->p);
        if (*p == ',')
        {
            e->p = p + 1;
            step = eval_expr(e);
            p = skip_ws(e->p);
        }
        if (*p == ')')
            e->p = p + 1;
        if (step <= 0.0f)
            step = 1.0f;
        float d = a / step;
        float r = (mode == 1) ? ceilf(d) : (mode == 2) ? floorf(d)
                                       : (mode == 3)   ? truncf(d)
                                                       : roundf(d);
        return r * step;
    }
    /* hypot(a, b, ...) */
    if (starts_with(p, "hypot("))
    {
        e->p = p + 6;
        float sum_sq = 0.0f;
        for (;;)
        {
            float v = eval_expr(e);
            sum_sq += v * v;
            p = skip_ws(e->p);
            if (*p == ',')
            {
                e->p = p + 1;
                continue;
            }
            if (*p == ')')
            {
                e->p = p + 1;
                break;
            }
            break;
        }
        return sqrtf(sum_sq);
    }
    /* parenthesized sub-expression */
    if (*p == '(')
    {
        e->p = p + 1;
        float v = eval_expr(e);
        p = skip_ws(e->p);
        if (*p == ')')
            e->p = p + 1;
        return v;
    }
    /* unary minus */
    if (*p == '-')
    {
        e->p = p + 1;
        return -eval_factor(e);
    }
    if (*p == '+')
    {
        e->p = p + 1;
        return eval_factor(e);
    }
    /* number + unit */
    float v = parse_number(&e->p);
    return conv_unit(v, &e->p, e->ctx);
}

static float eval_term(Eval *e)
{
    float v = eval_factor(e);
    for (;;)
    {
        const char *p = skip_ws(e->p);
        if (*p == '*')
        {
            e->p = p + 1;
            v *= eval_factor(e);
        }
        else if (*p == '/')
        {
            e->p = p + 1;
            float r = eval_factor(e);
            v = (r != 0) ? v / r : 0;
        }
        else
            break;
    }
    return v;
}

static float eval_expr(Eval *e)
{
    float v = eval_term(e);
    for (;;)
    {
        const char *p = skip_ws(e->p);
        if (*p == '+')
        {
            e->p = p + 1;
            v += eval_term(e);
        }
        else if (*p == '-')
        {
            e->p = p + 1;
            v -= eval_term(e);
        }
        else
            break;
    }
    return v;
}

float mini_css_eval(const char *expr, const MiniCssCtx *ctx, int *ok)
{
    if (!expr)
    {
        if (ok)
            *ok = 0;
        return 0;
    }
    MiniCssCtx default_ctx;
    if (!ctx)
    {
        default_ctx.pct_base = 0;
        default_ctx.font_px = 16;
        default_ctx.root_font_px = 16;
        default_ctx.vw = 1280;
        default_ctx.vh = 720;
        ctx = &default_ctx;
    }
    Eval e;
    e.p = expr;
    e.ctx = ctx;
    e.ok = 1;
    float v = eval_expr(&e);
    if (ok)
        *ok = e.ok;
    return v;
}

/* ================================================================== */
/* @media query evaluation                                            */
/* ================================================================== */
/* parse "Npx"/"Nem"/"Nrem"/"N%"/"Nvw" to px given viewport */
static float media_length(const char *s, int vw, int vh)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    float v = (float)atof(s);
    if (strstr(s, "px"))
        return v;
    if (strstr(s, "em"))
        return v * 16;
    if (strstr(s, "rem"))
        return v * 16;
    if (strstr(s, "vw"))
        return v * vw / 100.0f;
    if (strstr(s, "vh"))
        return v * vh / 100.0f;
    if (strstr(s, "%"))
        return v * vw / 100.0f;
    return v; /* unitless → treat as px */
}

/* match a single feature like (min-width: 768px) */
static int media_feature(const char *f, int vw, int vh)
{
    f = skip_ws(f);
    if (*f != '(')
        return 1; /* no feature → true */
    f++;
    /* read feature name */
    char name[32];
    int i = 0;
    while (*f && *f != ':' && *f != ')' && i < (int)sizeof(name) - 1)
        name[i++] = (char)tolower((unsigned char)*f++);
    name[i] = 0;
    f = skip_ws(f);
    if (*f == ':')
        f++;
    f = skip_ws(f);
    /* read value up to ')' */
    char val[64];
    int j = 0;
    while (*f && *f != ')' && j < (int)sizeof(val) - 1)
        val[j++] = *f++;
    val[j] = 0;
    if (*f == ')')
        f++;
    /* evaluate */
    if (strstr(name, "min-width"))
        return vw >= (int)media_length(val, vw, vh);
    if (strstr(name, "max-width"))
        return vw <= (int)media_length(val, vw, vh);
    if (strstr(name, "min-height"))
        return vh >= (int)media_length(val, vw, vh);
    if (strstr(name, "max-height"))
        return vh <= (int)media_length(val, vw, vh);
    if (strstr(name, "orientation"))
    {
        int land = vw >= vh;
        return strstr(val, "landscape") ? land : !land;
    }
    if (strstr(name, "color-scheme") || strstr(name, "prefers-"))
        return 0;
    /* unknown feature → permissive true (matches browser behavior for unknown) */
    return 1;
}

int mini_css_media_matches(const char *cond, int vw, int vh)
{
    if (!cond)
        return 1;
    const char *p = skip_ws(cond);
    /* leading media type: all / screen / print — accept all/screen, ignore print */
    if (starts_with(p, "all") || starts_with(p, "screen"))
    {
        p += (p[0] == 'a' ? 3 : 6);
        p = skip_ws(p);
    }
    else if (starts_with(p, "print"))
        return 0;
    int result = 1;
    if (*p == '\0')
        return 1;
    /* split on "and" — evaluate each (...) feature */
    for (;;)
    {
        p = skip_ws(p);
        if (*p != '(')
            break;
        /* find matching ')' */
        const char *start = p;
        int depth = 0;
        const char *q = p;
        while (*q)
        {
            if (*q == '(')
                depth++;
            else if (*q == ')')
            {
                depth--;
                if (depth == 0)
                {
                    q++;
                    break;
                }
            }
            q++;
        }
        char *feat = dup_n(start, (size_t)(q - start));
        if (!media_feature(feat, vw, vh))
        {
            result = 0;
            free(feat);
            break;
        }
        free(feat);
        p = skip_ws(q);
        if (starts_with(p, "and"))
        {
            p += 3;
            continue;
        }
        break;
    }
    return result;
}

/* ================================================================== */
/* @keyframes storage + animation step                                */
/* ================================================================== */
typedef struct MiniKfStop
{
    int pct;    /* 0..100 */
    char *body; /* raw declarations "prop:val;..." */
} MiniKfStop;

typedef struct MiniKeyframes
{
    char *name;
    MiniKfStop *stops;
    int n_stops, cap_stops;
} MiniKeyframes;

static MiniKeyframes *g_kf = NULL;
static int g_kf_n = 0, g_kf_cap = 0;

static MiniKeyframes *kf_find(const char *name)
{
    for (int i = 0; i < g_kf_n; i++)
        if (!strcmp(g_kf[i].name, name))
            return &g_kf[i];
    return NULL;
}

void mini_css_keyframes_set(const char *name, const char *css)
{
    if (!name || !css)
        return;
    MiniKeyframes *kf = kf_find(name);
    if (!kf)
    {
        if (g_kf_n == g_kf_cap)
        {
            int nc = g_kf_cap ? g_kf_cap * 2 : 8;
            MiniKeyframes *nk = realloc(g_kf, nc * sizeof(*nk));
            if (!nk)
                return;
            g_kf = nk;
            g_kf_cap = nc;
        }
        kf = &g_kf[g_kf_n++];
        memset(kf, 0, sizeof(*kf));
        kf->name = strdup(name);
    }
    else
    {
        for (int i = 0; i < kf->n_stops; i++)
            free(kf->stops[i].body);
        free(kf->stops);
        kf->stops = NULL;
        kf->n_stops = 0;
        kf->cap_stops = 0;
    }
    /* parse stops: selectors like "from","to","50%","0%,100%" followed by {body} */
    const char *p = css;
    while (*p)
    {
        p = skip_ws(p);
        if (*p == '}')
            break;
        /* read selector list up to '{' */
        const char *sel_start = p;
        while (*p && *p != '{')
            p++;
        if (*p != '{')
            break;
        char *sel = dup_n(sel_start, (size_t)(p - sel_start));
        p++; /* skip '{' */
        const char *body_start = p;
        int depth = 1;
        while (*p && depth > 0)
        {
            if (*p == '{')
                depth++;
            else if (*p == '}')
            {
                depth--;
                if (depth == 0)
                    break;
            }
            p++;
        }
        char *body = dup_n(body_start, (size_t)(p - body_start));
        if (*p == '}')
            p++;
        /* parse each comma-separated selector as a stop */
        char *save;
        char *tok = strtok_r(sel, ",", &save);
        while (tok)
        {
            while (*tok && isspace((unsigned char)*tok))
                tok++;
            int pct = -1;
            if (!strcmp(tok, "from"))
                pct = 0;
            else if (!strcmp(tok, "to"))
                pct = 100;
            else
            {
                pct = (int)strtol(tok, NULL, 10);
            }
            if (pct >= 0 && pct <= 100)
            {
                if (kf->n_stops == kf->cap_stops)
                {
                    int nc = kf->cap_stops ? kf->cap_stops * 2 : 4;
                    MiniKfStop *ns = realloc(kf->stops, nc * sizeof(*ns));
                    if (!ns)
                        break;
                    kf->stops = ns;
                    kf->cap_stops = nc;
                }
                kf->stops[kf->n_stops].pct = pct;
                kf->stops[kf->n_stops].body = strdup(body);
                kf->n_stops++;
            }
            tok = strtok_r(NULL, ",", &save);
        }
        free(sel);
        free(body);
    }
    /* sort stops by pct (insertion) */
    for (int i = 1; i < kf->n_stops; i++)
    {
        MiniKfStop tmp = kf->stops[i];
        int j = i - 1;
        while (j >= 0 && kf->stops[j].pct > tmp.pct)
        {
            kf->stops[j + 1] = kf->stops[j];
            j--;
        }
        kf->stops[j + 1] = tmp;
    }
    /* CSS spec: if 0% stop is missing and 100% exists (e.g. @keyframes rotate-border { 100% { transform: rotate(360deg); } }),
       synthesize a 0% stop with initial values (e.g. transform: rotate(0deg) / scale(1) / translate(0,0)) */
    if (kf->n_stops == 1 && kf->stops[0].pct == 100)
    {
        if (kf->n_stops + 1 > kf->cap_stops)
        {
            int nc = kf->cap_stops ? kf->cap_stops * 2 : 4;
            MiniKfStop *ns = realloc(kf->stops, nc * sizeof(*ns));
            if (ns)
            {
                kf->stops = ns;
                kf->cap_stops = nc;
            }
        }
        if (kf->n_stops < kf->cap_stops)
        {
            char zero_body[256] = {0};
            if (strstr(kf->stops[0].body, "rotate"))
                snprintf(zero_body, sizeof(zero_body), "transform: rotate(0deg);");
            else if (strstr(kf->stops[0].body, "scale"))
                snprintf(zero_body, sizeof(zero_body), "transform: scale(1);");
            else if (strstr(kf->stops[0].body, "opacity"))
                snprintf(zero_body, sizeof(zero_body), "opacity: 0;");
            else
                snprintf(zero_body, sizeof(zero_body), "%s", kf->stops[0].body);
            kf->stops[1] = kf->stops[0];
            kf->stops[0].pct = 0;
            kf->stops[0].body = strdup(zero_body);
            kf->n_stops = 2;
        }
    }
}

const void *mini_css_keyframes_get(const char *name)
{
    MiniKeyframes *kf = kf_find(name);
    return kf;
}

/* Helper to parse declarations from a CSS body into MiniDecl array */
typedef struct MiniDecl
{
    char prop[48];
    char val[128];
} MiniDecl;

static int parse_decls(const char *body, MiniDecl *decls, int max_decls)
{
    if (!body)
        return 0;
    int count = 0;
    char *copy = strdup(body);
    char *save;
    char *d = strtok_r(copy, ";", &save);
    while (d && count < max_decls)
    {
        char *colon = strchr(d, ':');
        if (colon)
        {
            *colon = '\0';
            char *p = d, *v = colon + 1;
            while (*p && isspace((unsigned char)*p))
                p++;
            while (*v && isspace((unsigned char)*v))
                v++;
            char *ep = p + strlen(p);
            while (ep > p && isspace((unsigned char)ep[-1]))
                *--ep = '\0';
            char *ev = v + strlen(v);
            while (ev > v && isspace((unsigned char)ev[-1]))
                *--ev = '\0';
            if (*p && *v)
            {
                strncpy(decls[count].prop, p, sizeof(decls[count].prop) - 1);
                decls[count].prop[sizeof(decls[count].prop) - 1] = '\0';
                strncpy(decls[count].val, v, sizeof(decls[count].val) - 1);
                decls[count].val[sizeof(decls[count].val) - 1] = '\0';
                count++;
            }
        }
        d = strtok_r(NULL, ";", &save);
    }
    free(copy);
    return count;
}

/* Standard CSS cubic-bezier evaluator (x1, y1, x2, y2) at progression x (0..1) -> y (0..1) */
float mini_css_cubic_bezier(float x1, float y1, float x2, float y2, float x)
{
    if (x <= 0.0f)
        return 0.0f;
    if (x >= 1.0f)
        return 1.0f;
    /* Solve for parameter u in [0, 1] using Newton-Raphson */
    float u = x;
    for (int i = 0; i < 8; i++)
    {
        float one_u = 1.0f - u;
        float cur_x = 3.0f * one_u * one_u * u * x1 + 3.0f * one_u * u * u * x2 + u * u * u;
        float dx = 3.0f * one_u * one_u * x1 + 6.0f * one_u * u * (x2 - x1) + 3.0f * u * u * (1.0f - x2);
        float diff = cur_x - x;
        if (fabsf(diff) < 1e-5f)
            break;
        if (fabsf(dx) < 1e-6f)
            break;
        u -= diff / dx;
        if (u < 0.0f)
            u = 0.0f;
        if (u > 1.0f)
            u = 1.0f;
    }
    float one_u = 1.0f - u;
    float cur_x = 3.0f * one_u * one_u * u * x1 + 3.0f * one_u * u * u * x2 + u * u * u;
    if (fabsf(cur_x - x) > 1e-3f)
    {
        float low = 0.0f, high = 1.0f;
        for (int i = 0; i < 12; i++)
        {
            u = (low + high) * 0.5f;
            one_u = 1.0f - u;
            cur_x = 3.0f * one_u * one_u * u * x1 + 3.0f * one_u * u * u * x2 + u * u * u;
            if (cur_x > x)
                high = u;
            else
                low = u;
        }
    }
    one_u = 1.0f - u;
    float y = 3.0f * one_u * one_u * u * y1 + 3.0f * one_u * u * u * y2 + u * u * u;
    return y;
}

float mini_css_eval_timing(int timing_mode, const float custom_bezier[4], float t)
{
    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;
    switch (timing_mode)
    {
    case 0:
        return t; /* linear */
    case 1:
        return mini_css_cubic_bezier(0.25f, 0.1f, 0.25f, 1.0f, t); /* ease */
    case 2:
        return mini_css_cubic_bezier(0.42f, 0.0f, 1.0f, 1.0f, t); /* ease-in */
    case 3:
        return mini_css_cubic_bezier(0.0f, 0.0f, 0.58f, 1.0f, t); /* ease-out */
    case 4:
        return mini_css_cubic_bezier(0.42f, 0.0f, 0.58f, 1.0f, t); /* ease-in-out */
    case 5:
        if (custom_bezier)
            return mini_css_cubic_bezier(custom_bezier[0], custom_bezier[1], custom_bezier[2], custom_bezier[3], t);
        return mini_css_cubic_bezier(0.25f, 0.1f, 0.25f, 1.0f, t);
    default:
        return t;
    }
}

/* Helper: parse shadow components from a shadow layer string */
static int parse_shadow_components(const char *str, float *x, float *y, float *blur, float *spread,
                                   float *r, float *g, float *b, float *a)
{
    if (!str || !*str || !strcmp(str, "none"))
    {
        *x = *y = *blur = *spread = 0.0f;
        *r = *g = *b = *a = 0.0f;
        return 0;
    }
    *x = *y = *blur = *spread = 0.0f;
    *r = *g = *b = 0.0f;
    *a = 0.5f;
    int nums = 0;
    char col_str[64] = {0};
    const char *p = str;
    while (*p)
    {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        if (*p == '#' || *p == 'r' || *p == 'R' || *p == 'h' || *p == 'H')
        {
            int ci = 0, depth = 0;
            while (*p && ci < (int)sizeof(col_str) - 1)
            {
                if (*p == '(')
                    depth++;
                if (*p == ')' && depth > 0)
                {
                    col_str[ci++] = *p++;
                    depth--;
                    continue;
                }
                if (depth == 0 && isspace((unsigned char)*p))
                    break;
                col_str[ci++] = *p++;
            }
            col_str[ci] = 0;
            continue;
        }
        char *end = NULL;
        float val = strtof(p, &end);
        if (end == p)
        {
            p++;
            continue;
        }
        while (*end == 'p' || *end == 'x')
            end++;
        if (nums == 0)
            *x = val;
        else if (nums == 1)
            *y = val;
        else if (nums == 2)
            *blur = val;
        else if (nums == 3)
            *spread = val;
        nums++;
        p = end;
    }
    if (col_str[0])
    {
        mini_parse_color(col_str, r, g, b, a);
    }
    return (nums >= 2);
}

/* Helper: parse 2D + 3D transform components out of a transform() string.
 * Captures rotateX/rotateY/translateZ/perspective too, so keyframe
 * interpolation between two `transform` stops no longer silently drops
 * the 3D parts (the bug that broke a @keyframes grid-floor: rotateX(75deg)
 * would lose its tilt between stops). Values are kept in source units
 * (deg / px) since the result is re-emitted as a string that mini_style_set
 * re-parses. */
static void parse_transform_components(const char *str,
                                       float *tx, float *ty, float *tz,
                                       float *sx, float *sy,
                                       float *rx, float *ry, float *rz,
                                       float *skx, float *sky, float *persp)
{
    *tx = *ty = *tz = *rx = *ry = *rz = *skx = *sky = *persp = 0.0f;
    *sx = *sy = 1.0f;
    if (!str || !*str || !strcmp(str, "none"))
        return;
    const char *p = str;
    while (*p)
    {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        if (!strncmp(p, "translateY(", 11))
        {
            *ty = (float)atof(p + 11);
        }
        else if (!strncmp(p, "translateX(", 11))
        {
            *tx = (float)atof(p + 11);
        }
        else if (!strncmp(p, "translateZ(", 11))
        {
            *tz = (float)atof(p + 11);
        }
        else if (!strncmp(p, "translate3d(", 12))
        {
            char tmp[64];
            const char *e = strchr(p, ')');
            size_t l = e ? (size_t)(e - p - 12) : 0;
            if (l >= sizeof(tmp))
                l = sizeof(tmp) - 1;
            memcpy(tmp, p + 12, l);
            tmp[l] = 0;
            char *save;
            char *x_s = strtok_r(tmp, ",", &save);
            char *y_s = strtok_r(NULL, ",", &save);
            char *z_s = strtok_r(NULL, ",", &save);
            if (x_s)
                *tx = (float)atof(x_s);
            if (y_s)
                *ty = (float)atof(y_s);
            if (z_s)
                *tz = (float)atof(z_s);
        }
        else if (!strncmp(p, "translate(", 10))
        {
            char tmp[64];
            const char *e = strchr(p, ')');
            size_t l = e ? (size_t)(e - p - 10) : 0;
            if (l >= sizeof(tmp))
                l = sizeof(tmp) - 1;
            memcpy(tmp, p + 10, l);
            tmp[l] = 0;
            char *save;
            char *x_s = strtok_r(tmp, ",", &save);
            char *y_s = strtok_r(NULL, ",", &save);
            if (x_s)
                *tx = (float)atof(x_s);
            if (y_s)
                *ty = (float)atof(y_s);
        }
        else if (!strncmp(p, "perspective(", 12))
        {
            *persp = (float)atof(p + 12);
        }
        else if (!strncmp(p, "rotateX(", 8))
        {
            *rx = (float)atof(p + 8);
        }
        else if (!strncmp(p, "rotateY(", 8))
        {
            *ry = (float)atof(p + 8);
        }
        else if (!strncmp(p, "scale3d(", 8))
        {
            char tmp[64];
            const char *e = strchr(p, ')');
            size_t l = e ? (size_t)(e - p - 8) : 0;
            if (l >= sizeof(tmp))
                l = sizeof(tmp) - 1;
            memcpy(tmp, p + 8, l);
            tmp[l] = 0;
            char *save;
            char *x_s = strtok_r(tmp, ",", &save);
            char *y_s = strtok_r(NULL, ",", &save);
            if (x_s)
                *sx = (float)atof(x_s);
            if (y_s)
                *sy = (float)atof(y_s);
        }
        else if (!strncmp(p, "scaleY(", 7))
        {
            *sy = (float)atof(p + 7);
        }
        else if (!strncmp(p, "scaleX(", 7))
        {
            *sx = (float)atof(p + 7);
        }
        else if (!strncmp(p, "scale(", 6))
        {
            char tmp[64];
            const char *e = strchr(p, ')');
            size_t l = e ? (size_t)(e - p - 6) : 0;
            if (l >= sizeof(tmp))
                l = sizeof(tmp) - 1;
            memcpy(tmp, p + 6, l);
            tmp[l] = 0;
            char *save;
            char *x_s = strtok_r(tmp, ",", &save);
            char *y_s = strtok_r(NULL, ",", &save);
            if (x_s)
                *sx = (float)atof(x_s);
            if (y_s)
                *sy = (float)atof(y_s);
            else if (x_s)
                *sy = *sx;
        }
        else if (!strncmp(p, "rotateZ(", 8))
        {
            const char *st = strchr(p, '(');
            if (st)
                *rz = (float)atof(st + 1);
        }
        else if (!strncmp(p, "rotate(", 7))
        {
            const char *st = strchr(p, '(');
            if (st)
                *rz = (float)atof(st + 1);
        }
        else if (!strncmp(p, "skewX(", 6))
        {
            *skx = (float)atof(p + 6);
        }
        else if (!strncmp(p, "skewY(", 6))
        {
            *sky = (float)atof(p + 6);
        }
        else if (!strncmp(p, "skew(", 5))
        {
            /* two-arg skew(x, y) — single arg => both axes equal */
            char tmp[64];
            const char *e = strchr(p, ')');
            size_t l = e ? (size_t)(e - p - 5) : 0;
            if (l >= sizeof(tmp))
                l = sizeof(tmp) - 1;
            memcpy(tmp, p + 5, l);
            tmp[l] = 0;
            char *save;
            char *x_s = strtok_r(tmp, ",", &save);
            char *y_s = strtok_r(NULL, ",", &save);
            if (x_s)
                *skx = (float)atof(x_s);
            if (y_s)
                *sky = (float)atof(y_s);
            else if (x_s)
                *sky = (float)atof(x_s);
        }
        const char *cp = strchr(p, ')');
        p = cp ? cp + 1 : p + strlen(p);
    }
}

/* parse `rect(t r b l)` (comma- or space-separated) into 4 floats + a
 * per-value unit flag (0=px/number, 1=percent). Returns 1 only when it is a
 * rect() with exactly 4 numeric values; returns 0 for non-rect or any
 * `auto` (the caller then steps discretely — which is what glitch slicing
 * actually wants). Used by mini_css_interpolate_val so a `clip` keyframe
 * doesn't fall through to the generic token-stream lerp and mangle the
 * 4 rect numbers. */
static int parse_rect(const char *v, float out[4], int unit[4])
{
    if (!v)
        return 0;
    const char *p = strstr(v, "rect");
    if (!p)
        return 0;
    const char *lp = strchr(p, '(');
    const char *rp = strchr(p, ')');
    if (!lp || !rp || rp < lp)
        return 0;
    char buf[96];
    size_t l = (size_t)(rp - lp - 1);
    if (l >= sizeof(buf))
        l = sizeof(buf) - 1;
    memcpy(buf, lp + 1, l);
    buf[l] = 0;
    for (char *s = buf; *s; s++)
        if (*s == ',')
            *s = ' ';
    char *save;
    int n = 0;
    for (char *tok = strtok_r(buf, " \t", &save);
         tok && n < 4;
         tok = strtok_r(NULL, " \t", &save))
    {
        if (!strcmp(tok, "auto"))
            return 0; /* keep glitch hard-cut; don't guess auto */
        char *end;
        float f = (float)strtod(tok, &end);
        if (end == tok)
            return 0;
        out[n] = f;
        unit[n] = (strchr(tok, '%') != NULL) ? 1 : 0;
        n++;
    }
    return n == 4 ? 1 : 0;
}

void mini_css_interpolate_val(const char *prop, const char *v1, const char *v2, float t, char *out, size_t out_cap)
{
    if (!v1 || !v2 || out_cap == 0)
    {
        if (out && out_cap)
            out[0] = 0;
        return;
    }
    if (t <= 0.0f)
    {
        strncpy(out, v1, out_cap - 1);
        out[out_cap - 1] = 0;
        return;
    }
    if (t >= 1.0f)
    {
        strncpy(out, v2, out_cap - 1);
        out[out_cap - 1] = 0;
        return;
    }

    /* 1. Color interpolation (HEX, RGB, RGBA, HSL, Named Color) */
    float r1, g1, b1, a1, r2, g2, b2, a2;
    int is_c1 = mini_parse_color(v1, &r1, &g1, &b1, &a1);
    int is_c2 = mini_parse_color(v2, &r2, &g2, &b2, &a2);
    if (is_c1 && is_c2)
    {
        float r = r1 * (1.0f - t) + r2 * t;
        float g = g1 * (1.0f - t) + g2 * t;
        float b = b1 * (1.0f - t) + b2 * t;
        float a = a1 * (1.0f - t) + a2 * t;
        if (a >= 0.999f)
            snprintf(out, out_cap, "#%02x%02x%02x", (int)(r * 255.0f + 0.5f), (int)(g * 255.0f + 0.5f), (int)(b * 255.0f + 0.5f));
        else
            snprintf(out, out_cap, "rgba(%d,%d,%d,%.3g)", (int)(r * 255.0f + 0.5f), (int)(g * 255.0f + 0.5f), (int)(b * 255.0f + 0.5f), a);
        return;
    }

    /* 2. Box-shadow / Text-shadow interpolation */
    if (prop && strstr(prop, "shadow"))
    {
        float x1 = 0, y1 = 0, bl1 = 0, sp1 = 0, cr1 = 0, cg1 = 0, cb1 = 0, ca1 = 0;
        float x2 = 0, y2 = 0, bl2 = 0, sp2 = 0, cr2 = 0, cg2 = 0, cb2 = 0, ca2 = 0;
        int ok1 = parse_shadow_components(v1, &x1, &y1, &bl1, &sp1, &cr1, &cg1, &cb1, &ca1);
        int ok2 = parse_shadow_components(v2, &x2, &y2, &bl2, &sp2, &cr2, &cg2, &cb2, &ca2);
        if (!ok1 && ok2)
        {
            cr1 = cr2;
            cg1 = cg2;
            cb1 = cb2;
            ca1 = 0.0f;
        }
        if (!ok2 && ok1)
        {
            cr2 = cr1;
            cg2 = cg1;
            cb2 = cb1;
            ca2 = 0.0f;
        }
        float x = x1 * (1.0f - t) + x2 * t;
        float y = y1 * (1.0f - t) + y2 * t;
        float bl = bl1 * (1.0f - t) + bl2 * t;
        float sp = sp1 * (1.0f - t) + sp2 * t;
        float r = cr1 * (1.0f - t) + cr2 * t;
        float g = cg1 * (1.0f - t) + cg2 * t;
        float b = cb1 * (1.0f - t) + cb2 * t;
        float a = ca1 * (1.0f - t) + ca2 * t;
        if (sp1 != 0.0f || sp2 != 0.0f)
        {
            snprintf(out, out_cap, "%.4gpx %.4gpx %.4gpx %.4gpx rgba(%d,%d,%d,%.3g)",
                     x, y, bl, sp, (int)(r * 255.0f + 0.5f), (int)(g * 255.0f + 0.5f), (int)(b * 255.0f + 0.5f), a);
        }
        else
        {
            snprintf(out, out_cap, "%.4gpx %.4gpx %.4gpx rgba(%d,%d,%d,%.3g)",
                     x, y, bl, (int)(r * 255.0f + 0.5f), (int)(g * 255.0f + 0.5f), (int)(b * 255.0f + 0.5f), a);
        }
        return;
    }

    /* 3. Transform interpolation */
    if (prop && !strcmp(prop, "transform"))
    {
        float tx1, ty1, tz1, sx1, sy1, rx1, ry1, rz1, skx1, sky1, pp1;
        float tx2, ty2, tz2, sx2, sy2, rx2, ry2, rz2, skx2, sky2, pp2;
        parse_transform_components(v1, &tx1, &ty1, &tz1, &sx1, &sy1, &rx1, &ry1, &rz1, &skx1, &sky1, &pp1);
        parse_transform_components(v2, &tx2, &ty2, &tz2, &sx2, &sy2, &rx2, &ry2, &rz2, &skx2, &sky2, &pp2);
        float tx = tx1 * (1.0f - t) + tx2 * t;
        float ty = ty1 * (1.0f - t) + ty2 * t;
        float tz = tz1 * (1.0f - t) + tz2 * t;
        float sx = sx1 * (1.0f - t) + sx2 * t;
        float sy = sy1 * (1.0f - t) + sy2 * t;
        float rx = rx1 * (1.0f - t) + rx2 * t;
        float ry = ry1 * (1.0f - t) + ry2 * t;
        float rz = rz1 * (1.0f - t) + rz2 * t;
        float skx = skx1 * (1.0f - t) + skx2 * t;
        float sky = sky1 * (1.0f - t) + sky2 * t;
        float pp = pp1 * (1.0f - t) + pp2 * t;
        char buf[256];
        size_t bi = 0;
        buf[0] = 0;
        /* 3D parts first so the re-parsed transform carries rotateX/Y +
           perspective through to the renderer (otherwise a keyframe-animated
           3D element loses its tilt between stops). */
        if (pp > 0.0f)
        {
            bi += (size_t)snprintf(buf + bi, sizeof(buf) - bi, "perspective(%.4gpx) ", pp);
        }
        if (rx != 0.0f)
        {
            bi += (size_t)snprintf(buf + bi, sizeof(buf) - bi, "rotateX(%.4gdeg) ", rx);
        }
        if (ry != 0.0f)
        {
            bi += (size_t)snprintf(buf + bi, sizeof(buf) - bi, "rotateY(%.4gdeg) ", ry);
        }
        if (tz != 0.0f)
        {
            bi += (size_t)snprintf(buf + bi, sizeof(buf) - bi, "translateZ(%.4gpx) ", tz);
        }
        if (tx != 0.0f || ty != 0.0f)
        {
            bi += (size_t)snprintf(buf + bi, sizeof(buf) - bi, "translate(%.4gpx, %.4gpx) ", tx, ty);
        }
        if (sx != 1.0f || sy != 1.0f)
        {
            bi += (size_t)snprintf(buf + bi, sizeof(buf) - bi, "scale(%.4g, %.4g) ", sx, sy);
        }
        if (rz != 0.0f)
        {
            bi += (size_t)snprintf(buf + bi, sizeof(buf) - bi, "rotate(%.4gdeg) ", rz);
        }
        if (skx != 0.0f)
        {
            bi += (size_t)snprintf(buf + bi, sizeof(buf) - bi, "skewX(%.4gdeg) ", skx);
        }
        if (sky != 0.0f)
        {
            bi += (size_t)snprintf(buf + bi, sizeof(buf) - bi, "skewY(%.4gdeg) ", sky);
        }
        if (bi == 0)
            snprintf(buf, sizeof(buf), "none");
        else if (buf[bi - 1] == ' ')
            buf[bi - 1] = 0;
        strncpy(out, buf, out_cap - 1);
        out[out_cap - 1] = 0;
        return;
    }

    /* 3b. clip: rect(...) interpolation. Without this the 4 rect numbers
       would be mauled by the generic token-stream lerp below (it doesn't
       know rect structure), so a glitch `clip` keyframe would render
       garbage instead of a clean slice. */
    if (prop && (!strcmp(prop, "clip") || !strcmp(prop, "clip-path")))
    {
        float a1[4], a2[4];
        int u1[4], u2[4];
        int ok1 = parse_rect(v1, a1, u1);
        int ok2 = parse_rect(v2, a2, u2);
        if (ok1 && ok2)
        {
            char buf[128];
            size_t bi = 0;
            bi += (size_t)snprintf(buf + bi, sizeof(buf) - bi, "rect(");
            for (int i = 0; i < 4; i++)
            {
                float v = a1[i] * (1.0f - t) + a2[i] * t;
                /* preserve v1's unit per component (px or %) */
                bi += (size_t)snprintf(buf + bi, sizeof(buf) - bi,
                                       "%s%.4g%s", i ? " " : "", v,
                                       u1[i] ? "%" : "px");
            }
            bi += (size_t)snprintf(buf + bi, sizeof(buf) - bi, ")");
            strncpy(out, buf, out_cap - 1);
            out[out_cap - 1] = 0;
            return;
        }
        /* one side isn't a clean rect (e.g. `auto`) → step discretely to the
           nearest stop; that's the right look for a glitch hard-cut anyway. */
        strncpy(out, (t < 0.5f ? v1 : v2), out_cap - 1);
        out[out_cap - 1] = 0;
        return;
    }

    /* 4. Single number / float with unit (px, %, em, rem, deg, rad, or unitless) */
    char *end1 = NULL, *end2 = NULL;
    float n1 = strtof(v1, &end1);
    float n2 = strtof(v2, &end2);
    if (end1 != v1 && end2 != v2)
    {
        while (*end1 && isspace((unsigned char)*end1))
            end1++;
        while (*end2 && isspace((unsigned char)*end2))
            end2++;
        if (!strcmp(end1, end2) || !*end1 || !*end2)
        {
            const char *u = (*end1) ? end1 : end2;
            float res = n1 * (1.0f - t) + n2 * t;
            snprintf(out, out_cap, "%.4g%s", res, u ? u : "");
            return;
        }
    }

    /* 5. Tokenized numeric stream fallback */
    const char *p1 = v1, *p2 = v2;
    char buf[256];
    size_t bi = 0;
    int can_lerp_tokens = 1;
    while (*p1 && *p2 && bi < sizeof(buf) - 32)
    {
        if ((isdigit((unsigned char)*p1) || (*p1 == '-' && (isdigit((unsigned char)p1[1]) || p1[1] == '.')) || *p1 == '.') &&
            (isdigit((unsigned char)*p2) || (*p2 == '-' && (isdigit((unsigned char)p2[1]) || p2[1] == '.')) || *p2 == '.'))
        {
            char *e1 = NULL, *e2 = NULL;
            float num1 = strtof(p1, &e1);
            float num2 = strtof(p2, &e2);
            float lerped = num1 * (1.0f - t) + num2 * t;
            bi += (size_t)snprintf(buf + bi, sizeof(buf) - bi, "%.4g", lerped);
            p1 = e1;
            p2 = e2;
        }
        else if (*p1 == *p2)
        {
            buf[bi++] = *p1++;
            p2++;
        }
        else
        {
            can_lerp_tokens = 0;
            break;
        }
    }
    if (can_lerp_tokens && !*p1 && !*p2)
    {
        buf[bi] = '\0';
        strncpy(out, buf, out_cap - 1);
        out[out_cap - 1] = '\0';
        return;
    }

    /* Step at 0.5 for non-interpolatable properties */
    const char *src = (t < 0.5f) ? v1 : v2;
    strncpy(out, src, out_cap - 1);
    out[out_cap - 1] = '\0';
}

/* Return the interpolated declaration body for the keyframes set `name` at
   time t (0..1) as a malloc'd "prop:val;prop:val;" string (NULL if the set
   doesn't exist). Performs smooth numerical, color & transform Lerp between keyframes. */
char *mini_css_keyframes_body_at(const char *name, double t)
{
    MiniKeyframes *kf = kf_find(name);
    if (!kf || kf->n_stops == 0)
        return NULL;
    int pct = (int)(t * 100.0 + 0.5);
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;

    int lo = 0, hi = kf->n_stops - 1;
    for (int i = 0; i < kf->n_stops; i++)
    {
        if (kf->stops[i].pct <= pct)
            lo = i;
        if (kf->stops[i].pct >= pct)
        {
            hi = i;
            break;
        }
    }

    if (lo == hi || kf->stops[lo].pct == kf->stops[hi].pct)
        return strdup(kf->stops[lo].body ? kf->stops[lo].body : "");

    float frac = (float)(pct - kf->stops[lo].pct) / (float)(kf->stops[hi].pct - kf->stops[lo].pct);
    if (frac < 0.0f)
        frac = 0.0f;
    if (frac > 1.0f)
        frac = 1.0f;

    MiniDecl decls_lo[32], decls_hi[32];
    int n_lo = parse_decls(kf->stops[lo].body, decls_lo, 32);
    int n_hi = parse_decls(kf->stops[hi].body, decls_hi, 32);

    char out_buf[1024] = {0};
    size_t out_len = 0;
    uint8_t hi_used[32] = {0};

    for (int i = 0; i < n_lo; i++)
    {
        int match = -1;
        for (int j = 0; j < n_hi; j++)
        {
            if (!strcmp(decls_lo[i].prop, decls_hi[j].prop))
            {
                match = j;
                hi_used[j] = 1;
                break;
            }
        }
        char interpolated[128];
        if (match >= 0)
        {
            mini_css_interpolate_val(decls_lo[i].prop, decls_lo[i].val, decls_hi[match].val, frac, interpolated, sizeof(interpolated));
        }
        else
        {
            strncpy(interpolated, decls_lo[i].val, sizeof(interpolated) - 1);
            interpolated[sizeof(interpolated) - 1] = '\0';
        }
        out_len += (size_t)snprintf(out_buf + out_len, sizeof(out_buf) - out_len, "%s:%s;", decls_lo[i].prop, interpolated);
    }
    for (int j = 0; j < n_hi; j++)
    {
        if (!hi_used[j])
        {
            out_len += (size_t)snprintf(out_buf + out_len, sizeof(out_buf) - out_len, "%s:%s;", decls_hi[j].prop, decls_hi[j].val);
        }
    }
    return strdup(out_buf);
}

/* ================================================================== */
/* @font-face storage                                                 */
/* ================================================================== */
static MiniFontFaceRule *g_ff = NULL;
static int g_ff_n = 0, g_ff_cap = 0;

void mini_css_fontface_set(const char *css)
{
    if (!css)
        return;
    if (g_ff_n == g_ff_cap)
    {
        int nc = g_ff_cap ? g_ff_cap * 2 : 4;
        MiniFontFaceRule *nf = realloc(g_ff, nc * sizeof(*nf));
        if (!nf)
            return;
        g_ff = nf;
        g_ff_cap = nc;
    }
    MiniFontFaceRule *r = &g_ff[g_ff_n++];
    memset(r, 0, sizeof(*r));
    r->weight = 400;
    /* parse declarations */
    char *body = strdup(css);
    char *save;
    char *decl = strtok_r(body, ";", &save);
    while (decl)
    {
        char *colon = strchr(decl, ':');
        if (colon)
        {
            *colon = 0;
            char *prop = decl;
            char *val = colon + 1;
            while (*prop && isspace((unsigned char)*prop))
                prop++;
            while (*val && isspace((unsigned char)*val))
                val++;
            char *e = prop + strlen(prop);
            while (e > prop && isspace((unsigned char)e[-1]))
                *--e = 0;
            e = val + strlen(val);
            while (e > val && isspace((unsigned char)e[-1]))
                *--e = 0;
            if (!strcmp(prop, "font-family"))
            {
                char *q = val;
                while (*q && (*q == '"' || *q == '\''))
                    q++;
                char *qe = q + strlen(q);
                while (qe > q && (qe[-1] == '"' || qe[-1] == '\''))
                    qe--;
                size_t l = (size_t)(qe - q);
                if (l >= sizeof(r->family))
                    l = sizeof(r->family) - 1;
                memcpy(r->family, q, l);
                r->family[l] = 0;
            }
            else if (!strcmp(prop, "src"))
            {
                char *u = strstr(val, "url(");
                if (u)
                {
                    u += 4;
                    while (*u && (*u == '"' || *u == '\''))
                        u++;
                    char *ue = strchr(u, ')');
                    if (ue)
                    {
                        while (ue > u && (ue[-1] == '"' || ue[-1] == '\''))
                            ue--;
                        size_t l = (size_t)(ue - u);
                        if (l >= sizeof(r->src_url))
                            l = sizeof(r->src_url) - 1;
                        memcpy(r->src_url, u, l);
                        r->src_url[l] = 0;
                    }
                }
            }
            else if (!strcmp(prop, "font-weight"))
            {
                r->weight = (int)strtol(val, NULL, 10);
            }
            else if (!strcmp(prop, "font-style"))
            {
                strncpy(r->style, val, sizeof(r->style) - 1);
            }
        }
        decl = strtok_r(NULL, ";", &save);
    }
    free(body);
}

int mini_css_fontface_count(void) { return g_ff_n; }
const MiniFontFaceRule *mini_css_fontface_get(int i) { return (i < 0 || i >= g_ff_n) ? NULL : &g_ff[i]; }

const MiniFontFaceRule *mini_css_fontface_match(const char *family, int weight, const char *style)
{
    if (!family || g_ff_n == 0)
        return NULL;
    const MiniFontFaceRule *best = NULL;
    int best_diff = 99999;
    for (int i = 0; i < g_ff_n; i++)
    {
        const MiniFontFaceRule *r = &g_ff[i];
        if (!strcasecmp(r->family, family))
        {
            int diff = abs(r->weight - (weight ? weight : 400));
            if (style && r->style[0] && strcasecmp(r->style, style))
                diff += 1000;
            if (diff < best_diff)
            {
                best_diff = diff;
                best = r;
            }
        }
    }
    return best;
}

/* ================================================================== */
/* Self-test                                                          */
/* ================================================================== */
#ifdef CSS_SELFTEST
#include <assert.h>

static int eqf(float a, float b) { return fabsf(a - b) < 0.5f; }

int mini_css_selftest(void)
{
    int fails = 0;
    /* var substitution */
    mini_css_var_reset();
    mini_css_var_set("gap", "20px");
    mini_css_var_set("base", "100%");
    {
        char *r = mini_css_resolve_vars("var(--gap)");
        if (strcmp(r, "20px"))
        {
            printf("CSS1: var → '%s'\n", r);
            fails++;
        }
        free(r);
    }
    {
        char *r = mini_css_resolve_vars("calc(var(--gap) * 2 + 10px)");
        if (strcmp(r, "calc(20px * 2 + 10px)"))
        {
            printf("CSS2: var-in-calc → '%s'\n", r);
            fails++;
        }
        free(r);
    }
    {
        char *r = mini_css_resolve_vars("var(--undef, 5px)");
        if (strcmp(r, "5px"))
        {
            printf("CSS3: fallback → '%s'\n", r);
            fails++;
        }
        free(r);
    }

    /* calc eval */
    MiniCssCtx ctx = {.pct_base = 800, .font_px = 16, .root_font_px = 16, .vw = 1280, .vh = 720};
    int ok;
    {
        float v = mini_css_eval("calc(100% - 20px)", &ctx, &ok);
        if (!ok || !eqf(v, 780))
        {
            printf("CSS4: calc(100%%-20px)=%.1f\n", v);
            fails++;
        }
    }
    {
        float v = mini_css_eval("calc(2 * 50px + 10px)", &ctx, &ok);
        if (!ok || !eqf(v, 110))
        {
            printf("CSS5: nested calc=%.1f\n", v);
            fails++;
        }
    }
    {
        float v = mini_css_eval("calc((10px + 5px) * 2)", &ctx, &ok);
        if (!ok || !eqf(v, 30))
        {
            printf("CSS6: parens=%.1f\n", v);
            fails++;
        }
    }
    /* min/max/clamp */
    {
        float v = mini_css_eval("min(100px, 50%)", &ctx, &ok);
        if (!ok || !eqf(v, 100))
        {
            printf("CSS7: min=%.1f\n", v);
            fails++;
        }
    }
    {
        float v = mini_css_eval("max(10px, 5%)", &ctx, &ok);
        if (!ok || !eqf(v, 40))
        {
            printf("CSS8: max=%.1f\n", v);
            fails++;
        }
    }
    {
        float v = mini_css_eval("clamp(10px, 50%, 1000px)", &ctx, &ok);
        if (!ok || !eqf(v, 400))
        {
            printf("CSS9: clamp=%.1f\n", v);
            fails++;
        }
    }
    {
        float v = mini_css_eval("clamp(10px, 5px, 1000px)", &ctx, &ok);
        if (!ok || !eqf(v, 10))
        {
            printf("CSS10: clamp lo=%.1f\n", v);
            fails++;
        }
    }
    /* viewport units */
    {
        float v = mini_css_eval("calc(50vw + 25vh)", &ctx, &ok);
        if (!ok || !eqf(v, 640 + 180))
        {
            printf("CSS11: vw/vh=%.1f\n", v);
            fails++;
        }
    }

    /* @media */
    if (!mini_css_media_matches("(min-width: 768px)", 1024, 768))
    {
        printf("CSS12: media min-width\n");
        fails++;
    }
    if (mini_css_media_matches("(min-width: 2000px)", 1024, 768))
    {
        printf("CSS13: media min-width no\n");
        fails++;
    }
    if (!mini_css_media_matches("screen and (max-width: 1024px)", 800, 600))
    {
        printf("CSS14: media screen+max\n");
        fails++;
    }
    if (mini_css_media_matches("(orientation: portrait)", 1024, 768))
    {
        printf("CSS15: orientation\n");
        fails++;
    }

    /* Math functions: sin, sqrt, abs, mod, pow, round, hypot */
    {
        float v = mini_css_eval("sin(90deg)", &ctx, &ok);
        if (!ok || !eqf(v, 1.0f))
        {
            printf("CSS19: sin(90deg)=%.2f\n", v);
            fails++;
        }
    }
    {
        float v = mini_css_eval("cos(0deg)", &ctx, &ok);
        if (!ok || !eqf(v, 1.0f))
        {
            printf("CSS20: cos(0deg)=%.2f\n", v);
            fails++;
        }
    }
    {
        float v = mini_css_eval("sqrt(144px)", &ctx, &ok);
        if (!ok || !eqf(v, 12.0f))
        {
            printf("CSS21: sqrt(144)=%.1f\n", v);
            fails++;
        }
    }
    {
        float v = mini_css_eval("pow(2, 3)", &ctx, &ok);
        if (!ok || !eqf(v, 8.0f))
        {
            printf("CSS22: pow(2,3)=%.1f\n", v);
            fails++;
        }
    }
    {
        float v = mini_css_eval("abs(-42px)", &ctx, &ok);
        if (!ok || !eqf(v, 42.0f))
        {
            printf("CSS23: abs(-42)=%.1f\n", v);
            fails++;
        }
    }
    {
        float v = mini_css_eval("mod(17, 5)", &ctx, &ok);
        if (!ok || !eqf(v, 2.0f))
        {
            printf("CSS24: mod(17,5)=%.1f\n", v);
            fails++;
        }
    }
    {
        float v = mini_css_eval("round(up, 10.2px)", &ctx, &ok);
        if (!ok || !eqf(v, 11.0f))
        {
            printf("CSS25: round(up,10.2)=%.1f\n", v);
            fails++;
        }
    }
    {
        float v = mini_css_eval("hypot(3px, 4px)", &ctx, &ok);
        if (!ok || !eqf(v, 5.0f))
        {
            printf("CSS26: hypot(3,4)=%.1f\n", v);
            fails++;
        }
    }

    /* @keyframes smooth Lerp */
    mini_css_keyframes_set("slide", "0% { opacity: 0; transform: translateX(0px); } 100% { opacity: 1; transform: translateX(100px); }");
    char *kf_half = mini_css_keyframes_body_at("slide", 0.5);
    if (!kf_half || !strstr(kf_half, "0.5") || !strstr(kf_half, "50px"))
    {
        printf("CSS27: keyframes lerp half -> '%s'\n", kf_half ? kf_half : "NULL");
        fails++;
    }
    free(kf_half);

    /* @font-face match */
    mini_css_fontface_set("font-family: 'Roboto'; src: url(roboto-bold.ttf); font-weight: 700; font-style: normal");
    mini_css_fontface_set("font-family: 'Roboto'; src: url(roboto-regular.ttf); font-weight: 400; font-style: normal");
    const MiniFontFaceRule *matched = mini_css_fontface_match("Roboto", 600, "normal");
    if (!matched || matched->weight != 700)
    {
        printf("CSS28: fontface match weight (expected 700, got %d)\n", matched ? matched->weight : -1);
        fails++;
    }

    if (fails == 0)
        printf("[css] selftest OK\n");
    else
        printf("[css] selftest FAILED (%d)\n", fails);
    return fails ? 1 : 0;
}
int main(void) { return mini_css_selftest(); }
#endif /* CSS_SELFTEST */
