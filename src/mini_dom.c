/*
 * mini_dom.c — self-written DOM tree + reduced layout (block + flex).
 *
 * This is NOT a browser DOM. It is a purpose-built scene graph exposing
 * the subset of Node/Element that Vue 3 / React 18 vDOM diffs exercise,
 * with a layout pass covering only block flow + flexbox. That scope
 * choice is what keeps this file in the hundreds-of-lines instead of the
 * millions a CSS2.1 engine would cost.
 */
#include "mini_dom.h"
#include "mini_html5.h"
#include "mini_css.h"
#include "mini_events.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <GL/gl.h>

/* ------------------------------------------------------------------ */
/* Linear Arena Allocator (Frame Arena & Document Arena)               */
/* ------------------------------------------------------------------ */
void mini_arena_init(MiniArena *arena, size_t default_block_size)
{
    if (!arena)
        return;
    if (default_block_size == 0)
        default_block_size = 65536;
    arena->default_block_size = default_block_size;
    arena->head = NULL;
    arena->current = NULL;
}

void *mini_arena_alloc(MiniArena *arena, size_t size)
{
    if (!arena || size == 0)
        return NULL;
    /* 8-byte alignment */
    size = (size + 7) & ~((size_t)7);
    if (arena->current && arena->current->used + size <= arena->current->capacity)
    {
        void *ptr = arena->current->data + arena->current->used;
        arena->current->used += size;
        return ptr;
    }
    size_t block_cap = arena->default_block_size;
    if (size > block_cap)
        block_cap = size;
    MiniArenaBlock *block = (MiniArenaBlock *)malloc(sizeof(MiniArenaBlock) + block_cap);
    if (!block)
        return NULL;
    block->capacity = block_cap;
    block->used = size;
    block->next = NULL;
    if (!arena->head)
    {
        arena->head = block;
        arena->current = block;
    }
    else
    {
        arena->current->next = block;
        arena->current = block;
    }
    return block->data;
}

char *mini_arena_strdup(MiniArena *arena, const char *s)
{
    if (!s)
        return NULL;
    size_t len = strlen(s);
    char *p = (char *)mini_arena_alloc(arena, len + 1);
    if (p)
    {
        memcpy(p, s, len);
        p[len] = '\0';
    }
    return p;
}

char *mini_arena_strndup(MiniArena *arena, const char *s, size_t n)
{
    if (!s)
        return NULL;
    size_t len = 0;
    while (len < n && s[len])
        len++;
    char *p = (char *)mini_arena_alloc(arena, len + 1);
    if (p)
    {
        memcpy(p, s, len);
        p[len] = '\0';
    }
    return p;
}

void mini_arena_reset(MiniArena *arena)
{
    if (!arena)
        return;
    for (MiniArenaBlock *b = arena->head; b; b = b->next)
    {
        b->used = 0;
    }
    arena->current = arena->head;
}

void mini_arena_destroy(MiniArena *arena)
{
    if (!arena)
        return;
    MiniArenaBlock *b = arena->head;
    while (b)
    {
        MiniArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    arena->head = NULL;
    arena->current = NULL;
}

/* ------------------------------------------------------------------ */
/* Subsystem Context Structures (Encapsulated in MiniDocument)        */
/* ------------------------------------------------------------------ */
typedef struct
{
    char key[96]; /* raw color string */
    float r, g, b, a;
    int ok;
    uint8_t used;
} ColorCacheEntry;

#define MINI_COLOR_CACHE_CAP 1024

typedef struct
{
    char sel[160];     /* key */
    char seg[16][128]; /* parsed compound segments */
    int comb[16];      /* combinators */
    int nseg;
    uint8_t used;
} CompiledSel;

#define MINI_CS_CAP 2048

typedef struct
{
    const void *node;
    uint32_t prop_hash;
    uint8_t pseudo;
} CascadeWinner;

#define MINI_CW_CAP 32768

struct G2dCmd
{
    int kind;
    float x, y, x2, y2, w, h, r;
    float cr, cg, cb, ca, lw;
    char *text;
    float fs, maxw;
};

#define G2D_CAP 131072

typedef struct
{
    float a, b, c, d, e, f;
} SvgXform;

typedef struct SvgGradientStop
{
    float offset;
    float r, g, b, a;
} SvgGradientStop;

typedef struct SvgGradient
{
    char id[64];
    int type; /* 0: linear, 1: radial */
    float x1, y1, x2, y2;
    float cx, cy, r, fx, fy;
    int is_user_space;
    SvgXform xform;
    int num_stops;
    SvgGradientStop stops[16];
    struct SvgGradient *next;
} SvgGradient;

typedef struct
{
    SvgGradient *gradients;
    struct MiniNode **id_nodes;
    int id_node_count;
    int id_node_cap;
} SvgDefsRegistry;

typedef struct MiniLayoutContext
{
    float vw, vh, root_font;
} MiniLayoutContext;

typedef struct MiniDocumentContext
{
    MiniLayoutContext layout_ctx;
    ColorCacheEntry color_cache_entries[MINI_COLOR_CACHE_CAP];
    int color_cache_count;
    CompiledSel compiled_sels[MINI_CS_CAP];
    int compiled_sels_count;
    CascadeWinner cascade_winners[MINI_CW_CAP];
    char *restyle_css_buf;
    size_t restyle_css_len;
    int is_restyling_active;
    struct G2dCmd *g2d_cmds;
    int g2d_count;
    float g2d_pts_x[8192], g2d_pts_y[8192];
    int g2d_pts_n, g2d_is_closed;
    int g2d_subpath_starts[512];
    int g2d_num_subpaths;
    double g2d_matrix[6];
    double g2d_stack[16][6];
    int g2d_stack_p;
    float g2d_f_size;
    float g2d_p_x, g2d_p_y;
    SvgDefsRegistry svg_defs;
    double anim_time_sec;
    MiniMutationHook mhook_fn;
    struct MiniDocument *mdoc_ptr;
    void *mud_ptr;
    MiniResourceLoaderCb res_loader_func;
    void *res_loader_user_data;
    struct MiniEventState *render_events_st;
} MiniDocumentContext;

static MiniDocumentContext g_fallback_ctx;
static MiniDocument *g_active_doc = NULL;

static MiniDocumentContext *mini_get_ctx(MiniDocument *doc)
{
    if (doc && doc->ctx)
        return (MiniDocumentContext *)doc->ctx;
    if (g_active_doc && g_active_doc->ctx)
        return (MiniDocumentContext *)g_active_doc->ctx;

    if (!g_fallback_ctx.g2d_cmds)
    {
        g_fallback_ctx.g2d_cmds = (struct G2dCmd *)calloc(G2D_CAP, sizeof(struct G2dCmd));
        g_fallback_ctx.g2d_matrix[0] = 1;
        g_fallback_ctx.g2d_matrix[3] = 1;
        g_fallback_ctx.g2d_f_size = 16.0f;
    }
    return &g_fallback_ctx;
}

#define g_anim_time (mini_get_ctx(g_active_doc)->anim_time_sec)
#define g_restyling (mini_get_ctx(g_active_doc)->is_restyling_active)
#define g_render_events (mini_get_ctx(g_active_doc)->render_events_st)
#define g_restyle_css (mini_get_ctx(g_active_doc)->restyle_css_buf)
#define g_restyle_len (mini_get_ctx(g_active_doc)->restyle_css_len)
#define g_color_cache (mini_get_ctx(g_active_doc)->color_cache_entries)
#define g_color_cache_n (mini_get_ctx(g_active_doc)->color_cache_count)
#define g_cs (mini_get_ctx(g_active_doc)->compiled_sels)
#define g_cs_n (mini_get_ctx(g_active_doc)->compiled_sels_count)
#define g_cw (mini_get_ctx(g_active_doc)->cascade_winners)
#define g_lctx (mini_get_ctx(g_active_doc)->layout_ctx)
#define g_res_loader_cb (mini_get_ctx(g_active_doc)->res_loader_func)
#define g_res_loader_ud (mini_get_ctx(g_active_doc)->res_loader_user_data)
#define g2d (mini_get_ctx(g_active_doc)->g2d_cmds)
#define g2d_n (mini_get_ctx(g_active_doc)->g2d_count)
#define g2d_px (mini_get_ctx(g_active_doc)->g2d_pts_x)
#define g2d_py (mini_get_ctx(g_active_doc)->g2d_pts_y)
#define g2d_pn (mini_get_ctx(g_active_doc)->g2d_pts_n)
#define g2d_closed (mini_get_ctx(g_active_doc)->g2d_is_closed)
#define g2d_subpaths (mini_get_ctx(g_active_doc)->g2d_subpath_starts)
#define g2d_num_subs (mini_get_ctx(g_active_doc)->g2d_num_subpaths)
#define g2d_m (mini_get_ctx(g_active_doc)->g2d_matrix)
#define g2d_stk (mini_get_ctx(g_active_doc)->g2d_stack)
#define g2d_sp (mini_get_ctx(g_active_doc)->g2d_stack_p)
#define g2d_font_size (mini_get_ctx(g_active_doc)->g2d_f_size)
#define g2d_pen_x (mini_get_ctx(g_active_doc)->g2d_p_x)
#define g2d_pen_y (mini_get_ctx(g_active_doc)->g2d_p_y)

static uint32_t cw_prop_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
    {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h ? h : 1;
}

static uint32_t cw_slot(const void *node, uint32_t ph, uint8_t pseudo)
{
    uintptr_t n = (uintptr_t)node;
    n ^= n >> 15;
    uint32_t h = (uint32_t)n ^ (ph * 2654435761u) ^ ((uint32_t)pseudo + 1) * 2246822519u;
    return h & (MINI_CW_CAP - 1);
}

static int cw_seen(const void *node, uint32_t ph, uint8_t pseudo)
{
    if (!g_active_doc) return 0;
    uint32_t h = cw_slot(node, ph, pseudo);
    for (int i = 0; i < 8; i++)
    {
        CascadeWinner *w = &g_cw[(h + i) & (MINI_CW_CAP - 1)];
        if (!w->node)
            return 0;
        if (w->node == node && w->prop_hash == ph && w->pseudo == pseudo)
            return 1;
    }
    return 0;
}

static void cw_mark(const void *node, uint32_t ph, uint8_t pseudo)
{
    if (!g_active_doc) return;
    uint32_t h = cw_slot(node, ph, pseudo);
    for (int i = 0; i < 8; i++)
    {
        CascadeWinner *w = &g_cw[(h + i) & (MINI_CW_CAP - 1)];
        if (!w->node)
        {
            w->node = node;
            w->prop_hash = ph;
            w->pseudo = pseudo;
            return;
        }
        if (w->node == node && w->prop_hash == ph && w->pseudo == pseudo)
            return;
    }
}

static void cw_clear(void)
{
    if (g_active_doc)
        memset(g_cw, 0, sizeof(CascadeWinner) * MINI_CW_CAP);
}


/* events state made visible to the render pass */
struct MiniEventState;
void mini_dom_set_render_events(struct MiniEventState *st)
{
    MiniDocumentContext *ctx = mini_get_ctx(g_active_doc);
    ctx->render_events_st = st;
}

/* ------------------------------------------------------------------ */
/* small string helpers (no libc bloat beyond what's needed)          */
/* ------------------------------------------------------------------ */
static char *mini_dup(const char *s)
{
    if (!s)
        return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

/* forward declarations */
static int ci_eq(const char *a, const char *b);
static int class_has(const char *classval, const char *tok);
static int parse_hex_color(const char *val, float *r, float *g, float *b, float *a);
static void parse_length(const char *v, MiniLength *out);
static void mini_calc_free(struct MiniNode *n);
extern void mini_draw_rect_rounded_corners_stroke(MiniRenderer *r, float x, float y, float w, float h,
                                                  const float radii[4], float lw, float cr, float cg, float cb, float ca);

/* ================================================================== */
/* Element classification registry — the single source of truth for   */
/* every native HTML tag. Drives: the parser's void/raw-text tables,   */
/* the layout's default display + heading font sizes, and the renderer's*/
/* per-category placeholder drawing. Adding a tag = adding a row here.  */
/* flags: bit0=is_void, bit1=is_raw_text.                               */
/* ================================================================== */
typedef struct
{
    const char *tag;
    uint8_t category; /* MiniElementCategory */
    uint8_t flags;    /* 1=void, 2=raw-text                  */
    uint8_t heading;  /* 0 or 1..6                            */
    uint8_t display;  /* MiniDisplayModel                     */
} TagDef;

#define F_VOID 1u
#define F_RAWTEXT 2u

static const TagDef TAG_TABLE[] = {
    /* --- 1. Root & metadata --- */
    {"base", MINI_CAT_METADATA, F_VOID, 0, MINI_DISPLAY_NONE},
    {"head", MINI_CAT_METADATA, 0, 0, MINI_DISPLAY_NONE},
    {"html", MINI_CAT_METADATA, 0, 0, MINI_DISPLAY_BLOCK},
    {"link", MINI_CAT_METADATA, F_VOID, 0, MINI_DISPLAY_NONE},
    {"meta", MINI_CAT_METADATA, F_VOID, 0, MINI_DISPLAY_NONE},
    {"script", MINI_CAT_METADATA, F_RAWTEXT, 0, MINI_DISPLAY_NONE},
    {"style", MINI_CAT_METADATA, F_RAWTEXT, 0, MINI_DISPLAY_NONE},
    {"title", MINI_CAT_METADATA, F_RAWTEXT, 0, MINI_DISPLAY_NONE},

    /* --- 2. Sectioning & layout --- */
    {"article", MINI_CAT_SECTION, 0, 0, MINI_DISPLAY_BLOCK},
    {"aside", MINI_CAT_SECTION, 0, 0, MINI_DISPLAY_BLOCK},
    {"body", MINI_CAT_SECTION, 0, 0, MINI_DISPLAY_BLOCK},
    {"footer", MINI_CAT_SECTION, 0, 0, MINI_DISPLAY_BLOCK},
    {"header", MINI_CAT_SECTION, 0, 0, MINI_DISPLAY_BLOCK},
    {"hgroup", MINI_CAT_SECTION, 0, 0, MINI_DISPLAY_BLOCK},
    {"main", MINI_CAT_SECTION, 0, 0, MINI_DISPLAY_BLOCK},
    {"nav", MINI_CAT_SECTION, 0, 0, MINI_DISPLAY_BLOCK},
    {"section", MINI_CAT_SECTION, 0, 0, MINI_DISPLAY_BLOCK},
    {"address", MINI_CAT_SECTION, 0, 0, MINI_DISPLAY_BLOCK},
    {"h1", MINI_CAT_SECTION, 0, 1, MINI_DISPLAY_BLOCK},
    {"h2", MINI_CAT_SECTION, 0, 2, MINI_DISPLAY_BLOCK},
    {"h3", MINI_CAT_SECTION, 0, 3, MINI_DISPLAY_BLOCK},
    {"h4", MINI_CAT_SECTION, 0, 4, MINI_DISPLAY_BLOCK},
    {"h5", MINI_CAT_SECTION, 0, 5, MINI_DISPLAY_BLOCK},
    {"h6", MINI_CAT_SECTION, 0, 6, MINI_DISPLAY_BLOCK},

    /* --- 3. Text & inline content --- */
    {"blockquote", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_BLOCK},
    {"div", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_BLOCK},
    {"figure", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_BLOCK},
    {"figcaption", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_BLOCK},
    {"hr", MINI_CAT_TEXT, F_VOID, 0, MINI_DISPLAY_BLOCK},
    {"p", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_BLOCK},
    {"pre", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_BLOCK},
    {"a", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"abbr", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"b", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"bdi", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"bdo", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"br", MINI_CAT_TEXT, F_VOID, 0, MINI_DISPLAY_INLINE},
    {"cite", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"code", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"data", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"del", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"dfn", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"em", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"i", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"ins", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"kbd", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"mark", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"q", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"ruby", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"rp", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"rt", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"s", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"samp", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"small", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"span", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"strong", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"sub", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"sup", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"time", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"u", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"var", MINI_CAT_TEXT, 0, 0, MINI_DISPLAY_INLINE},
    {"wbr", MINI_CAT_TEXT, F_VOID, 0, MINI_DISPLAY_INLINE},

    /* --- 4. Lists --- */
    {"dd", MINI_CAT_LIST, 0, 0, MINI_DISPLAY_BLOCK},
    {"dl", MINI_CAT_LIST, 0, 0, MINI_DISPLAY_BLOCK},
    {"dt", MINI_CAT_LIST, 0, 0, MINI_DISPLAY_BLOCK},
    {"li", MINI_CAT_LIST, 0, 0, MINI_DISPLAY_LIST_ITEM},
    {"menu", MINI_CAT_LIST, 0, 0, MINI_DISPLAY_BLOCK},
    {"ol", MINI_CAT_LIST, 0, 0, MINI_DISPLAY_BLOCK},
    {"ul", MINI_CAT_LIST, 0, 0, MINI_DISPLAY_BLOCK},

    /* --- 5. Forms & controls --- */
    {"button", MINI_CAT_FORM, 0, 0, MINI_DISPLAY_INLINE},
    {"datalist", MINI_CAT_FORM, 0, 0, MINI_DISPLAY_NONE},
    {"fieldset", MINI_CAT_FORM, 0, 0, MINI_DISPLAY_BLOCK},
    {"form", MINI_CAT_FORM, 0, 0, MINI_DISPLAY_BLOCK},
    {"input", MINI_CAT_FORM, F_VOID, 0, MINI_DISPLAY_INLINE},
    {"label", MINI_CAT_FORM, 0, 0, MINI_DISPLAY_INLINE},
    {"legend", MINI_CAT_FORM, 0, 0, MINI_DISPLAY_INLINE},
    {"meter", MINI_CAT_FORM, 0, 0, MINI_DISPLAY_INLINE},
    {"optgroup", MINI_CAT_FORM, 0, 0, MINI_DISPLAY_NONE},
    {"option", MINI_CAT_FORM, 0, 0, MINI_DISPLAY_NONE},
    {"output", MINI_CAT_FORM, 0, 0, MINI_DISPLAY_INLINE},
    {"progress", MINI_CAT_FORM, 0, 0, MINI_DISPLAY_INLINE},
    {"select", MINI_CAT_FORM, 0, 0, MINI_DISPLAY_INLINE},
    {"textarea", MINI_CAT_FORM, F_RAWTEXT, 0, MINI_DISPLAY_INLINE},

    /* --- 6. Media & embedded --- */
    {"area", MINI_CAT_MEDIA, F_VOID, 0, MINI_DISPLAY_NONE},
    {"audio", MINI_CAT_MEDIA, 0, 0, MINI_DISPLAY_INLINE},
    {"canvas", MINI_CAT_MEDIA, 0, 0, MINI_DISPLAY_INLINE},
    {"embed", MINI_CAT_MEDIA, F_VOID, 0, MINI_DISPLAY_INLINE},
    {"iframe", MINI_CAT_MEDIA, 0, 0, MINI_DISPLAY_INLINE},
    {"img", MINI_CAT_MEDIA, F_VOID, 0, MINI_DISPLAY_INLINE},
    {"map", MINI_CAT_MEDIA, 0, 0, MINI_DISPLAY_INLINE},
    {"math", MINI_CAT_MEDIA, 0, 0, MINI_DISPLAY_INLINE},
    {"object", MINI_CAT_MEDIA, 0, 0, MINI_DISPLAY_INLINE},
    {"param", MINI_CAT_MEDIA, F_VOID, 0, MINI_DISPLAY_NONE},
    {"picture", MINI_CAT_MEDIA, 0, 0, MINI_DISPLAY_INLINE},
    {"source", MINI_CAT_MEDIA, F_VOID, 0, MINI_DISPLAY_NONE},
    {"svg", MINI_CAT_MEDIA, 0, 0, MINI_DISPLAY_INLINE},
    {"track", MINI_CAT_MEDIA, F_VOID, 0, MINI_DISPLAY_NONE},
    {"video", MINI_CAT_MEDIA, 0, 0, MINI_DISPLAY_INLINE},

    /* --- 7. Tables --- */
    {"caption", MINI_CAT_TABLE, 0, 0, MINI_DISPLAY_BLOCK},
    {"col", MINI_CAT_TABLE, F_VOID, 0, MINI_DISPLAY_NONE},
    {"colgroup", MINI_CAT_TABLE, 0, 0, MINI_DISPLAY_NONE},
    {"table", MINI_CAT_TABLE, 0, 0, MINI_DISPLAY_BLOCK},
    {"tbody", MINI_CAT_TABLE, 0, 0, MINI_DISPLAY_BLOCK},
    {"td", MINI_CAT_TABLE, 0, 0, MINI_DISPLAY_BLOCK},
    {"tfoot", MINI_CAT_TABLE, 0, 0, MINI_DISPLAY_BLOCK},
    {"th", MINI_CAT_TABLE, 0, 0, MINI_DISPLAY_BLOCK},
    {"thead", MINI_CAT_TABLE, 0, 0, MINI_DISPLAY_BLOCK},
    {"tr", MINI_CAT_TABLE, 0, 0, MINI_DISPLAY_BLOCK},

    /* --- 8. Interactive & templates --- */
    {"details", MINI_CAT_INTERACTIVE, 0, 0, MINI_DISPLAY_BLOCK},
    {"dialog", MINI_CAT_INTERACTIVE, 0, 0, MINI_DISPLAY_BLOCK},
    {"portal", MINI_CAT_INTERACTIVE, 0, 0, MINI_DISPLAY_BLOCK},
    {"slot", MINI_CAT_INTERACTIVE, 0, 0, MINI_DISPLAY_INLINE},
    {"summary", MINI_CAT_INTERACTIVE, 0, 0, MINI_DISPLAY_BLOCK},
    {"template", MINI_CAT_INTERACTIVE, 0, 0, MINI_DISPLAY_NONE}};

const MiniElementInfo *mini_element_info(const char *tag)
{
    static MiniElementInfo info; /* single-threaded; cached in node anyway */
    info.category = MINI_CAT_UNKNOWN;
    info.is_void = 0;
    info.is_raw_text = 0;
    info.heading_level = 0;
    info.default_display = MINI_DISPLAY_BLOCK; /* custom elements render as a box */
    info.form_type = 0;
    if (!tag)
        return &info;
    for (size_t i = 0; i < sizeof(TAG_TABLE) / sizeof(TAG_TABLE[0]); i++)
    {
        if (strcmp(TAG_TABLE[i].tag, tag) == 0)
        {
            const TagDef *t = &TAG_TABLE[i];
            info.category = t->category;
            info.is_void = (t->flags & F_VOID) ? 1 : 0;
            info.is_raw_text = (t->flags & F_RAWTEXT) ? 1 : 0;
            info.heading_level = t->heading;
            info.default_display = t->display;
            return &info;
        }
    }
    return &info; /* unknown tag (incl. custom elements) → block */
}

/* default font size (px) per heading level, per CSS default UA stylesheet */
static float heading_font_size(int level)
{
    switch (level)
    {
    case 1:
        return 32.0f;
    case 2:
        return 24.0f;
    case 3:
        return 18.7f;
    case 4:
        return 16.0f;
    case 5:
        return 13.3f;
    case 6:
        return 10.7f;
    default:
        return 16.0f;
    }
}

/* ------------------------------------------------------------------ */
/* Node / document lifecycle                                          */
/* ------------------------------------------------------------------ */
struct MiniNode *mini_node_create_element(const char *tag)
{
    struct MiniNode *n = (struct MiniNode *)calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->type = MN_ELEMENT_NODE;

    char buf[48];
    const char *ttag = tag ? tag : "div";
    size_t i;
    for (i = 0; i < sizeof(buf) - 1 && ttag[i]; i++)
        buf[i] = (char)tolower((unsigned char)ttag[i]);
    buf[i] = 0;
    n->tag = mini_dup(buf);

    const MiniElementInfo *info = mini_element_info(buf);
    n->category = (uint8_t)info->category;
    n->default_display = info->default_display;
    n->heading_level = info->heading_level;
    n->style.display = info->default_display;

    n->style.flex_direction = 0;
    n->style.bg_a = 0.0f;
    n->style.color_a = 1.0f;
    n->style.scale_x = 1.0f;
    n->style.scale_y = 1.0f;

    /* 关键修复：CSS 标准规范中 transform-origin 默认必须为 50% 50%（中心点） */
    n->style.transform_origin_x = 0.5f;
    n->style.transform_origin_y = 0.5f;

    n->style.font_size = info->heading_level ? heading_font_size(info->heading_level) : 16.0f;
    n->style.align_self = -1;

    if (info->category == MINI_CAT_LIST && info->default_display == MINI_DISPLAY_LIST_ITEM)
        n->style.padding[3] = 24.0f;
    if (info->category == MINI_CAT_INTERACTIVE && buf[0] == 's' && buf[1] == 'u')
        n->style.padding[3] = 20.0f;

    if (!strcmp(buf, "button"))
        n->style.justify_content = 1;
    if (!strcmp(buf, "em") || !strcmp(buf, "i") || !strcmp(buf, "cite"))
        n->style.font_style = 1;
    if (!strcmp(buf, "strong") || !strcmp(buf, "b"))
        n->style.font_weight = 700;

    return n;
}

struct MiniNode *mini_node_create_text(const char *data)
{
    struct MiniNode *n = (struct MiniNode *)calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->type = MN_TEXT_NODE;
    n->text = mini_dup(data);
    n->style.align_self = -1; /* 修复点：默认继承父级 Flex 交叉轴对齐（实现完美居中） */
    return n;
}
/* text node from a (data,len) span — used by the HTML parser */
struct MiniNode *mini_node_create_text_n(const char *data, size_t len)
{
    struct MiniNode *n = (struct MiniNode *)calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->type = MN_TEXT_NODE;
    n->text = (char *)malloc(len + 1);
    if (!n->text)
    {
        free(n);
        return NULL;
    }
    memcpy(n->text, data, len);
    n->text[len] = 0;
    n->style.align_self = -1; /* 修复点：同上 */
    return n;
}

static unsigned int utf8_next(const char **p, int *len)
{
    const unsigned char *s = (const unsigned char *)*p;
    if (!s || !*s)
    {
        *len = 0;
        return 0;
    }
    unsigned int c0 = s[0];
    if (c0 < 0x80)
    {
        *len = 1;
        *p = (const char *)(s + 1);
        return c0;
    }
    if ((c0 & 0xE0) == 0xC0 && s[1])
    {
        *len = 2;
        *p = (const char *)(s + 2);
        return ((c0 & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if ((c0 & 0xF0) == 0xE0 && s[1] && s[2])
    {
        *len = 3;
        *p = (const char *)(s + 3);
        return ((c0 & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    if ((c0 & 0xF8) == 0xF0 && s[1] && s[2] && s[3])
    {
        *len = 4;
        *p = (const char *)(s + 4);
        return ((c0 & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    }
    *len = 1;
    *p = (const char *)(s + 1);
    return 0xFFFD;
}

/* ================================================================== */
/* Phase 2: the rest of the DOM Node hierarchy.                        */
/* CharacterData: Text (3) + Comment (8). Attr (2). DocumentType (10). */
/* DocumentFragment (11) — also the base of a ShadowRoot. These mirror */
/* the browser's Node subclasses so nodeType === checks and traversal   */
/* behave like a real DOM, not just the 3 types Phase 1 understood.    */
/* ================================================================== */
struct MiniNode *mini_node_create_comment(const char *data)
{
    struct MiniNode *n = (struct MiniNode *)calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->type = MN_COMMENT_NODE;
    n->text = mini_dup(data ? data : "");
    return n;
}
/* span variant used by the parser (avoids a temp copy of the slice) */
struct MiniNode *mini_node_create_comment_n(const char *data, size_t len)
{
    struct MiniNode *n = (struct MiniNode *)calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->type = MN_COMMENT_NODE;
    n->text = (char *)malloc(len + 1);
    if (!n->text)
    {
        free(n);
        return NULL;
    }
    memcpy(n->text, data, len);
    n->text[len] = 0;
    return n;
}
struct MiniNode *mini_node_create_document_type(const char *name)
{
    struct MiniNode *n = (struct MiniNode *)calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->type = MN_DOCUMENT_TYPE_NODE;
    n->tag = mini_dup(name ? name : "html");
    return n;
}
struct MiniNode *mini_node_create_document_fragment(void)
{
    struct MiniNode *n = (struct MiniNode *)calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->type = MN_DOCUMENT_FRAGMENT_NODE;
    n->style.display = MINI_DISPLAY_BLOCK; /* fragment lays out like a box */
    return n;
}
/* Attr node: name in tag, value in text (kept simple — a real Attr is a
   named property, not a child, so it never joins the child list).     */
struct MiniNode *mini_node_create_attribute(const char *name, const char *value)
{
    struct MiniNode *n = (struct MiniNode *)calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->type = MN_ATTRIBUTE_NODE;
    n->tag = mini_dup(name ? name : "");
    n->text = mini_dup(value ? value : "");
    return n;
}

/* ================================================================== */
/* Phase 1: micro HTML5 tokenizer/parser -> MiniNode tree              */
/* Compact state machine: start/end tags, attributes (quoted/bare),   */
/* void elements, raw-text <script>/<style>, comments, doctype.        */
/* Not a full WHATWG tokenizer, but parses real-world pages into a    */
/* tree Vue/React can mount on.                                       */
/* ================================================================== */
/* Void + raw-text semantics are now data-driven by the classification
   registry above, so the parser and the C-side everywhere agree on which
   tags are self-closing vs. which carry verbatim text content.        */
static int tag_is_void(const char *tag)
{
    return mini_element_info(tag)->is_void;
}
static int tag_is_rawtext(const char *tag)
{
    return mini_element_info(tag)->is_raw_text;
}

/* ---- HTML entity decoding ------------------------------------------ */
/* The 5 XML entities + numeric refs resolve to ASCII; named symbol refs
   resolve to UTF-8 (rendered as placeholders until a real font ships).
   &nbsp; maps to a regular space (0x20) so the ASCII font treats it as
   one — a pragmatic deviation from the U+00A0 spec. Unknown refs are
   left as their literal "&name;" text.                                */
static const struct
{
    const char *name;
    const char *utf8;
} HTML_ENT[] = {
    {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", " "}, {"ldquo", "\xE2\x80\x9C"}, {"rdquo", "\xE2\x80\x9D"}, {"lsquo", "\xE2\x80\x98"}, {"rsquo", "\xE2\x80\x99"}, {"copy", "\xC2\xA9"}, {"reg", "\xC2\xAE"}, {"trade", "\xE2\x84\xA2"}, {"mdash", "\xE2\x80\x94"}, {"ndash", "\xE2\x80\x93"}, {"hellip", "\xE2\x80\xA6"}, {"laquo", "\xC2\xAB"}, {"raquo", "\xC2\xBB"}, {"times", "\xC3\x97"}, {"divide", "\xC3\xB7"}, {"deg", "\xC2\xB0"}, {"plusmn", "\xC2\xB1"}, {"micro", "\xC2\xB5"}, {"para", "\xC2\xB6"}, {"sect", "\xC2\xA7"}, {"middot", "\xC2\xB7"}, {"bull", "\xE2\x80\xA2"}, {"prime", "\xE2\x80\xB2"}, {"Prime", "\xE2\x80\xB3"}, {"euro", "\xE2\x82\xAC"}, {"pound", "\xC2\xA3"}, {"cent", "\xC2\xA2"}, {"yen", "\xC2\xA5"}, {"dagger", "\xE2\x80\xA0"}, {"Dagger", "\xE2\x80\xA1"}, {"darr", "\xE2\x86\x93"}, {"uarr", "\xE2\x86\x91"}, {"larr", "\xE2\x86\x90"}, {"rarr", "\xE2\x86\x92"}, {"harr", "\xE2\x86\x94"}, {"ensp", "\xE2\x80\x82"}, {"emsp", "\xE2\x80\x83"}, {"thinsp", "\xE2\x80\x89"}, {"iexcl", "\xC2\xA1"}, {"iquest", "\xC2\xBF"}, {"frac12", "\xC2\xBD"}, {"frac14", "\xC2\xBC"}, {"frac34", "\xC2\xBE"}, {"sup2", "\xC2\xB2"}, {"sup3", "\xC2\xB3"}, {"szlig", "\xC3\x9F"}, {NULL, NULL}};

/* decode entities from src[0..len) into out[0..cap); returns decoded len.
   out must be at least len+1 bytes (output is never longer than input). */
static size_t decode_entities(const char *src, size_t len, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < len;)
    {
        if (src[i] != '&')
        {
            if (o < cap)
                out[o++] = src[i];
            i++;
            continue;
        }
        /* find a ';' within a sane window */
        size_t semi = 0;
        for (size_t k = i + 1; k < len && k < i + 16; k++)
            if (src[k] == ';')
            {
                semi = k;
                break;
            }
        if (!semi)
        {
            if (o < cap)
                out[o++] = '&';
            i++;
            continue;
        }
        const char *ent = src + i + 1;
        size_t nl = semi - (i + 1);
        int handled = 0;
        if (nl >= 2 && ent[0] == '#')
        {
            int hex = (ent[1] == 'x' || ent[1] == 'X');
            const char *ns = ent + (hex ? 2 : 1);
            const char *ne = ent + nl;
            int code = 0, ok = 1;
            for (const char *q = ns; q < ne; q++)
            {
                char ch = *q;
                int d = (ch >= '0' && ch <= '9')          ? ch - '0'
                        : (hex && ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
                        : (hex && ch >= 'A' && ch <= 'F') ? ch - 'A' + 10
                                                          : -1;
                if (d < 0)
                {
                    ok = 0;
                    break;
                }
                code = code * (hex ? 16 : 10) + d;
            }
            if (ok && code >= 0 && code <= 0x10FFFF)
            {
                if (code < 0x80)
                {
                    if (o < cap)
                        out[o++] = (char)code;
                }
                else if (code < 0x800)
                {
                    if (o + 1 < cap)
                    {
                        out[o++] = (char)(0xC0 | (code >> 6));
                        out[o++] = (char)(0x80 | (code & 0x3F));
                    }
                }
                else
                {
                    if (o + 2 < cap)
                    {
                        out[o++] = (char)(0xE0 | (code >> 12));
                        out[o++] = (char)(0x80 | ((code >> 6) & 0x3F));
                        out[o++] = (char)(0x80 | (code & 0x3F));
                    }
                }
                handled = 1;
            }
        }
        else
        {
            for (int t = 0; HTML_ENT[t].name; t++)
            {
                size_t el = strlen(HTML_ENT[t].name);
                if (el == nl && !strncmp(ent, HTML_ENT[t].name, el))
                {
                    const char *u = HTML_ENT[t].utf8;
                    while (*u)
                    {
                        if (o < cap)
                            out[o++] = *u;
                        u++;
                    }
                    handled = 1;
                    break;
                }
            }
        }
        if (handled)
            i = semi + 1;
        else
        {
            if (o < cap)
                out[o++] = '&';
            i++;
        }
    }
    return o;
}

/* WHATWG HTML5 parser delegation. The full tokenizer + tree-construction
   pipeline (Adoption Agency Algorithm, auto-close of <p>/<li>/<td>...,
   table foster-parenting, streaming incremental parse) lives in mini_html5.c;
   this entry keeps the legacy call sites (mini_bridge_load_html) intact so
   the integration is transparent to the bridge / the other agent's work. */
void mini_dom_parse_html(MiniDocument *doc, const char *html)
{
    mini_html5_parse(doc, html);
}

/* legacy single-pass tolerant parser — retained for the PARSE_SELFTEST
   regression suite and as a reference; the live path uses mini_html5_parse. */
static void mini_dom_parse_html_legacy(MiniDocument *doc, const char *html)
{
    if (!doc || !html)
        return;
    struct MiniNode *stack[64];
    int sp = 0;
    stack[sp++] = doc->body; /* append root */
    const char *p = html;

    while (*p)
    {
        /* comment / declaration — now materialized as real nodes so the
           tree carries the full Node hierarchy (Comment / DocumentType). */
        if (p[0] == '<' && p[1] == '!')
        {
            if (!strncmp(p, "<!--", 4))
            {
                const char *data = p + 4;
                const char *e = strstr(p, "-->");
                size_t len = e ? (size_t)(e - data) : strlen(data);
                struct MiniNode *c = mini_node_create_comment_n(data, len);
                if (c)
                    mini_node_append_child(stack[sp - 1], c);
                p = e ? e + 3 : p + strlen(p);
            }
            else
            {
                /* <!DOCTYPE ...> → a DocumentType node on the document. */
                const char *e = strchr(p, '>');
                size_t len = e ? (size_t)(e - p - 2) : strlen(p + 2);
                const char *inner = p + 2; /* skip "<!" */
                /* extract the first token as the doctype name */
                char nm[32];
                size_t ni = 0;
                while (ni < len && !isspace((unsigned char)inner[ni]) && ni < sizeof nm - 1)
                    nm[ni] = (char)tolower((unsigned char)inner[ni]), ni++;
                nm[ni] = 0;
                if (ni >= 7 && !strcmp(nm, "doctype"))
                {
                    /* skip the "doctype" keyword, take the next token */
                    size_t k = 7;
                    while (k < len && isspace((unsigned char)inner[k]))
                        k++;
                    size_t j = 0;
                    while (k < len && !isspace((unsigned char)inner[k]) && j < sizeof nm - 1)
                        nm[j++] = (char)tolower((unsigned char)inner[k++]);
                    nm[j] = 0;
                    struct MiniNode *dt = mini_node_create_document_type(nm[0] ? nm : "html");
                    if (dt)
                        mini_node_append_child(doc->root, dt);
                }
                p = e ? e + 1 : p + strlen(p);
            }
            continue;
        }
        /* end tag */
        if (p[0] == '<' && p[1] == '/')
        {
            p += 2;
            char tag[32];
            int ti = 0;
            while (*p && *p != '>' && !isspace((unsigned char)*p) && ti < 31)
                tag[ti++] = (char)tolower((unsigned char)*p++);
            tag[ti] = 0;
            /* structural tags are implicit (doc already owns body); skip pop */
            if (!strcmp(tag, "html") || !strcmp(tag, "head") || !strcmp(tag, "body"))
            {
                while (*p && *p != '>')
                    p++;
                if (*p == '>')
                    p++;
                continue;
            }
            /* raw-text elements weren't pushed at their start tag, so their
               end tag must NOT pop the stack (else it pops the parent). */
            if (tag_is_rawtext(tag))
            {
                while (*p && *p != '>')
                    p++;
                if (*p == '>')
                    p++;
                continue;
            }
            while (*p && *p != '>')
                p++; /* skip stray attrs */
            if (*p == '>')
                p++;
            /* pop until we find the matching open tag (tolerant) */
            while (sp > 1)
            {
                struct MiniNode *top = stack[--sp];
                if (top->tag && !strcmp(top->tag, tag))
                    break;
            }
            continue;
        }
        /* start tag */
        if (p[0] == '<')
        {
            ++p;
            char tag[32];
            int ti = 0;
            while (*p && !isspace((unsigned char)*p) && *p != '>' && *p != '/' && ti < 31)
                tag[ti++] = (char)tolower((unsigned char)*p++);
            tag[ti] = 0;
            if (ti == 0)
            {
                if (*p)
                    p++;
                continue;
            } /* stray '<' */
            /* structural tags are implicit (doc already owns body); skip */
            if (!strcmp(tag, "html") || !strcmp(tag, "head") || !strcmp(tag, "body"))
            {
                while (*p && *p != '>')
                    p++;
                if (*p == '>')
                    p++;
                continue;
            }
            struct MiniNode *n = mini_node_create_element(tag);
            if (!n)
            {
                while (*p && *p != '>')
                    p++;
                if (*p)
                    p++;
                continue;
            }

            /* attributes */
            int self_close = 0;
            while (*p && *p != '>')
            {
                while (*p && isspace((unsigned char)*p))
                    p++;
                if (*p == '>' || *p == '/')
                {
                    if (*p == '/')
                    {
                        self_close = 1;
                        p++;
                    }
                    continue;
                }
                char aname[64];
                int ai = 0;
                while (*p && !isspace((unsigned char)*p) && *p != '=' &&
                       *p != '>' && *p != '/' && ai < 63)
                    aname[ai++] = (char)tolower((unsigned char)*p++);
                aname[ai] = 0;
                char aval[512];
                aval[0] = 0;
                while (*p && isspace((unsigned char)*p))
                    p++;
                if (*p == '=')
                {
                    p++;
                    while (*p && isspace((unsigned char)*p))
                        p++;
                    if (*p == '"')
                    {
                        p++;
                        int j = 0;
                        while (*p && *p != '"' && j < 511)
                            aval[j++] = *p++;
                        aval[j] = 0;
                        if (*p == '"')
                            p++;
                    }
                    else if (*p == '\'')
                    {
                        p++;
                        int j = 0;
                        while (*p && *p != '\'' && j < 511)
                            aval[j++] = *p++;
                        aval[j] = 0;
                        if (*p == '\'')
                            p++;
                    }
                    else
                    {
                        int j = 0;
                        while (*p && !isspace((unsigned char)*p) &&
                               *p != '>' && *p != '/' && j < 511)
                            aval[j++] = *p++;
                        aval[j] = 0;
                    }
                }
                if (aname[0])
                    mini_node_set_attribute(n, aname, aval);
            }
            if (*p == '>')
                p++;

            mini_node_append_child(stack[sp - 1], n);

            /* raw-text elements: read verbatim until matching close tag */
            if (tag_is_rawtext(tag))
            {
                char close[40];
                snprintf(close, sizeof close, "</%s", tag);
                const char *e = strstr(p, close);
                size_t len = e ? (size_t)(e - p) : strlen(p);
                n->text = (char *)malloc(len + 1);
                if (n->text)
                {
                    memcpy(n->text, p, len);
                    n->text[len] = 0;
                }
                p = e ? e : p + len;
                continue; /* leaf, do not push */
            }
            if (self_close || tag_is_void(tag))
                continue; /* no children */
            if (sp < 64)
                stack[sp++] = n;
            continue;
        }
        /* text node: collect until next '<' */
        const char *start = p;
        while (*p && *p != '<')
            p++;
        size_t len = (size_t)(p - start);
        if (len == 0)
            continue;
        /* decode HTML entities (&amp; &lt; &#65; etc.) into a scratch buffer;
           output is never longer than the input. */
        char *dbuf = (char *)malloc(len + 1);
        size_t dl = 0;
        if (dbuf)
        {
            dl = decode_entities(start, len, dbuf, len);
            dbuf[dl] = 0;
        }
        else
        {
            dbuf = (char *)start; /* fallback: raw text (no decode, not freed) */
            dl = len;
        }
        /* skip whitespace-only text between block tags (keeps the tree clean) */
        int onlyws = 1;
        for (size_t i = 0; i < (size_t)dl; i++)
            if (!isspace((unsigned char)dbuf[i]))
            {
                onlyws = 0;
                break;
            }
        if (onlyws)
        {
            if (dbuf != start)
                free(dbuf);
            continue;
        }
        struct MiniNode *t = mini_node_create_text_n(dbuf, dl);
        if (dbuf != start)
            free(dbuf);
        if (t)
            mini_node_append_child(stack[sp - 1], t);
    }
}

void mini_node_destroy(struct MiniNode *n)
{
    if (!n)
        return;
    /* unlink children recursively */
    struct MiniNode *c = n->first_child;
    while (c)
    {
        struct MiniNode *next = c->next_sibling;
        mini_node_destroy(c);
        c = next;
    }
    /* a host element's shadow root is owned by the host: free it too. */
    if (n->shadow_root)
        mini_node_destroy(n->shadow_root);
    free(n->tag);
    free(n->text);
    /* free attrs */
    MiniAttr *a = n->attrs;
    while (a)
    {
        MiniAttr *nx = a->next;
        free(a->name);
        free(a->value);
        free(a);
        a = nx;
    }
    if (n->pseudo_before)
        mini_node_destroy(n->pseudo_before);
    if (n->pseudo_after)
        mini_node_destroy(n->pseudo_after);
    if (n->pseudo_placeholder)
        mini_node_destroy(n->pseudo_placeholder);
    MiniNodeVar *nv = n->vars;
    /* If this node carried scoped custom properties, its address may be
       recycled by a future malloc for a node with different (or no) scoped
       vars. Bump the var-resolution cache generation so a stale scoped
       result cannot be served for the recycled pointer. Cheap (counter
       bump only; cache slots are reclaimed lazily on next store). */
    if (nv)
        mini_css_var_cache_invalidate();
    while (nv)
    {
        MiniNodeVar *nx = nv->next;
        free(nv->name);
        free(nv->value);
        free(nv);
        nv = nx;
    }
    free(n->before_content);
    free(n->after_content);
    mini_calc_free(n);
    free(n);
}

void mini_node_set_var(struct MiniNode *n, const char *name, const char *value)
{
    if (!name || !value)
        return;
    if (name[0] == '-' && name[1] == '-')
        name += 2;
    if (!n || (n->tag && !strcmp(n->tag, "html")) || n->type == MN_DOCUMENT_NODE)
    {
        mini_css_var_set(name, value);
        return;
    }
    for (MiniNodeVar *v = n->vars; v; v = v->next)
    {
        if (!strcmp(v->name, name))
        {
            free(v->value);
            v->value = mini_dup(value);
            return;
        }
    }
    MiniNodeVar *v = (MiniNodeVar *)malloc(sizeof(*v));
    if (!v)
        return;
    v->name = mini_dup(name);
    v->value = mini_dup(value);
    v->next = n->vars;
    n->vars = v;
}

const char *mini_node_get_var(const struct MiniNode *n, const char *name)
{
    if (!name)
        return NULL;
    if (name[0] == '-' && name[1] == '-')
        name += 2;
    for (const struct MiniNode *cur = n; cur; cur = cur->parent)
    {
        for (const MiniNodeVar *v = cur->vars; v; v = v->next)
        {
            if (!strcmp(v->name, name))
                return v->value;
        }
    }
    return mini_css_var_get(name);
}

MiniDocument *mini_doc_create(void)
{
    MiniDocument *d = (MiniDocument *)calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    mini_arena_init(&d->doc_arena, 65536);
    mini_arena_init(&d->frame_arena, 65536);
    MiniDocumentContext *ctx = (MiniDocumentContext *)calloc(1, sizeof(MiniDocumentContext));
    if (ctx)
    {
        ctx->g2d_cmds = (struct G2dCmd *)calloc(G2D_CAP, sizeof(struct G2dCmd));
        ctx->g2d_matrix[0] = 1;
        ctx->g2d_matrix[3] = 1;
        ctx->g2d_f_size = 16.0f;
    }
    d->ctx = ctx;
    g_active_doc = d;
    d->root = mini_node_create_element("#document");
    d->root->type = MN_DOCUMENT_NODE;
    d->body = mini_node_create_element("body");
    mini_node_append_child(d->root, d->body);
    return d;
}

void mini_doc_destroy(MiniDocument *d)
{
    if (!d)
        return;
    if (g_active_doc == d)
        g_active_doc = NULL;
    mini_node_destroy(d->root);
    if (d->ctx)
    {
        MiniDocumentContext *ctx = (MiniDocumentContext *)d->ctx;
        if (ctx->restyle_css_buf)
            free(ctx->restyle_css_buf);
        SvgGradient *sg = ctx->svg_defs.gradients;
        while (sg)
        {
            SvgGradient *nxt = sg->next;
            free(sg);
            sg = nxt;
        }
        if (ctx->svg_defs.id_nodes)
            free(ctx->svg_defs.id_nodes);
        if (ctx->g2d_cmds)
        {
            for (int i = 0; i < ctx->g2d_count; i++)
            {
                if (ctx->g2d_cmds[i].text)
                    free(ctx->g2d_cmds[i].text);
            }
            free(ctx->g2d_cmds);
        }
        free(d->ctx);
    }
    mini_arena_destroy(&d->frame_arena);
    mini_arena_destroy(&d->doc_arena);
    free(d);
}

/* ------------------------------------------------------------------ */
/* Tree mutation                                                       */
/* ------------------------------------------------------------------ */
/* ---- live DOM mutation hook (CDP Elements panel sync) ---- */
void mini_dom_set_mutation_hook(struct MiniDocument *doc, MiniMutationHook hook, void *ud)
{
    MiniDocumentContext *ctx = mini_get_ctx(doc);
    ctx->mdoc_ptr = doc;
    ctx->mhook_fn = hook;
    ctx->mud_ptr = ud;
}

static void notify_mutation(const char *evt, struct MiniNode *parent, struct MiniNode *node, const char *name, const char *value)
{
    if (g_active_doc)
    {
        g_active_doc->dirty = 1;
        g_active_doc->paint_dirty = 1;
    }
    MiniDocumentContext *ctx = mini_get_ctx(g_active_doc);
    if (ctx->mhook_fn && ctx->mdoc_ptr)
        ctx->mhook_fn(ctx->mdoc_ptr, evt, parent, node, name, value, ctx->mud_ptr);
}

int mini_node_append_child(struct MiniNode *parent, struct MiniNode *child)
{
    if (!parent || !child)
        return -1;
    /* W3C: appending a DocumentFragment moves its children into the
       parent (the fragment is left empty). This is the fast batch-insert
       path that avoids one reflow per node.                              */
    if (child->type == MN_DOCUMENT_FRAGMENT_NODE && child != parent)
    {
        struct MiniNode *c = child->first_child;
        while (c)
        {
            struct MiniNode *nx = c->next_sibling;
            /* detach from fragment, append to parent */
            c->parent = parent;
            c->prev_sibling = parent->last_child;
            c->next_sibling = NULL;
            if (parent->last_child)
                parent->last_child->next_sibling = c;
            else
                parent->first_child = c;
            parent->last_child = c;
            c = nx;
        }
        child->first_child = child->last_child = NULL;
        return 0;
    }
    if (child->parent)
        mini_node_remove_child(child->parent, child);
    child->parent = parent;
    child->prev_sibling = parent->last_child;
    child->next_sibling = NULL;
    if (parent->last_child)
        parent->last_child->next_sibling = child;
    else
        parent->first_child = child;
    parent->last_child = child;
    notify_mutation("childNodeInserted", parent, child, NULL, NULL);
    return 0;
}

int mini_node_insert_before(struct MiniNode *parent, struct MiniNode *new_child,
                            struct MiniNode *ref)
{
    if (!parent || !new_child)
        return -1;
    if (!ref)
        return mini_node_append_child(parent, new_child);
    if (new_child->parent)
        mini_node_remove_child(new_child->parent, new_child);
    /* link new_child before ref */
    new_child->parent = parent;
    new_child->next_sibling = ref;
    new_child->prev_sibling = ref->prev_sibling;
    if (ref->prev_sibling)
        ref->prev_sibling->next_sibling = new_child;
    else
        parent->first_child = new_child;
    ref->prev_sibling = new_child;
    return 0;
}

int mini_node_remove_child(struct MiniNode *parent, struct MiniNode *child)
{
    if (!parent || !child || child->parent != parent)
        return -1;
    if (child->prev_sibling)
        child->prev_sibling->next_sibling = child->next_sibling;
    else
        parent->first_child = child->next_sibling;
    if (child->next_sibling)
        child->next_sibling->prev_sibling = child->prev_sibling;
    else
        parent->last_child = child->prev_sibling;
    child->parent = NULL;
    child->prev_sibling = child->next_sibling = NULL;
    notify_mutation("childNodeRemoved", parent, child, NULL, NULL);
    return 0;
}

/* ================================================================== */
/* Phase 2: complete Node/Element tree operations                      */
/* cloneNode, replaceChild, contains, element-only child traversal,    */
/* getElementsByTagName / -ByClassName, and Shadow DOM attach.       */
/* ================================================================== */
struct MiniNode *mini_node_clone(struct MiniNode *n, int deep)
{
    if (!n)
        return NULL;
    struct MiniNode *c = (struct MiniNode *)calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->type = n->type;
    c->tag = mini_dup(n->tag);
    c->text = mini_dup(n->text);
    c->style = n->style;
    c->category = n->category;
    c->default_display = n->default_display;
    c->heading_level = n->heading_level;
    c->state_hovered = n->state_hovered;
    c->state_active = n->state_active;
    c->state_focused = n->state_focused;
    c->before_content = mini_dup(n->before_content);
    c->after_content = mini_dup(n->after_content);
    /* calc() store is per-instance; a cloned node's calc lengths resolve to
       0 until CSS re-applies — rare path (cloneNode + calc). */
    c->calc = NULL;
    /* copy attributes (singly-linked list) */
    MiniAttr **tail = &c->attrs;
    for (MiniAttr *a = n->attrs; a; a = a->next)
    {
        MiniAttr *na = (MiniAttr *)calloc(1, sizeof(*na));
        if (!na)
            break;
        na->name = mini_dup(a->name);
        na->value = mini_dup(a->value);
        *tail = na;
        tail = &na->next;
    }
    if (deep)
    {
        for (struct MiniNode *ch = n->first_child; ch; ch = ch->next_sibling)
        {
            struct MiniNode *cc = mini_node_clone(ch, 1);
            if (cc)
                mini_node_append_child(c, cc);
        }
    }
    return c;
}

int mini_node_replace_child(struct MiniNode *parent, struct MiniNode *new_child,
                            struct MiniNode *old_child)
{
    if (!parent || !new_child || !old_child)
        return -1;
    if (old_child->parent != parent)
        return -1;
    /* insert new_child before old, then remove old (keeps sibling order
       even when new_child was elsewhere in the same parent).            */
    if (mini_node_insert_before(parent, new_child, old_child) != 0)
        return -1;
    return mini_node_remove_child(parent, old_child);
}

int mini_node_contains(const struct MiniNode *root, const struct MiniNode *target)
{
    if (!root || !target)
        return 0;
    for (const struct MiniNode *n = target; n; n = n->parent)
        if (n == root)
            return 1;
    return 0;
}

struct MiniNode *mini_node_first_element_child(const struct MiniNode *n)
{
    if (!n)
        return NULL;
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        if (c->type == MN_ELEMENT_NODE)
            return c;
    return NULL;
}
struct MiniNode *mini_node_last_element_child(const struct MiniNode *n)
{
    if (!n)
        return NULL;
    for (struct MiniNode *c = n->last_child; c; c = c->prev_sibling)
        if (c->type == MN_ELEMENT_NODE)
            return c;
    return NULL;
}
int mini_node_element_child_count(const struct MiniNode *n)
{
    int c = 0;
    if (!n)
        return 0;
    for (struct MiniNode *x = n->first_child; x; x = x->next_sibling)
        if (x->type == MN_ELEMENT_NODE)
            c++;
    return c;
}

/* live-collection-style queries (depth-first preorder). */
static void collect_by_tag(const struct MiniNode *n, const char *tag,
                           struct MiniNode **out, int *count, int max)
{
    if (!n)
        return;
    if (n->type == MN_ELEMENT_NODE && n->tag &&
        (tag[0] == '*' || ci_eq(n->tag, tag)))
        if (*count < max)
            out[(*count)++] = (struct MiniNode *)n;
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        collect_by_tag(c, tag, out, count, max);
}
int mini_dom_get_elements_by_tag_name(struct MiniDocument *doc, const char *tag,
                                      struct MiniNode **out, int max)
{
    if (!doc || !tag || !out || max <= 0)
        return 0;
    int count = 0;
    collect_by_tag(doc->root, tag, out, &count, max);
    return count;
}
static void collect_by_class(const struct MiniNode *n, const char *cls,
                             struct MiniNode **out, int *count, int max)
{
    if (!n)
        return;
    if (n->type == MN_ELEMENT_NODE)
    {
        const char *cv = mini_node_get_attribute(n, "class");
        if (cv && class_has(cv, cls))
            if (*count < max)
                out[(*count)++] = (struct MiniNode *)n;
    }
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        collect_by_class(c, cls, out, count, max);
}
int mini_dom_get_elements_by_class_name(struct MiniDocument *doc,
                                        const char *cls,
                                        struct MiniNode **out, int max)
{
    if (!doc || !cls || !out || max <= 0)
        return 0;
    int count = 0;
    collect_by_class(doc->root, cls, out, &count, max);
    return count;
}

/* ================================================================== */
/* Phase 3: Web Components — Shadow DOM                               */
/* attachShadow() on an element creates a DocumentFragment-mode root   */
/* that the renderer walks INSTEAD of the element's light-tree        */
/* children. A <slot> inside the shadow tree pulls in the host's      */
/* light children (assigned via host->first_child..).                 */
/* ================================================================== */
struct MiniNode *mini_node_attach_shadow(struct MiniNode *host)
{
    if (!host)
        return NULL;
    if (host->shadow_root)
        return host->shadow_root; /* already attached */
    struct MiniNode *root = mini_node_create_document_fragment();
    if (!root)
        return NULL;
    root->tag = mini_dup("#shadow-root");
    host->shadow_root = root;
    root->parent = host; /* host back-reference; not in the child list */
    return root;
}
struct MiniNode *mini_node_shadow_root(const struct MiniNode *host)
{
    return host ? host->shadow_root : NULL;
}

/* ------------------------------------------------------------------ */
/* Attributes / text                                                   */
/* ------------------------------------------------------------------ */
void mini_node_set_attribute(struct MiniNode *n, const char *k, const char *v)
{
    if (!n || !k)
        return;
    MiniAttr *a = NULL;
    for (MiniAttr *p = n->attrs; p; p = p->next)
        if (strcmp(p->name, k) == 0)
        {
            a = p;
            break;
        }
    if (a)
    {
        free(a->value);
        a->value = mini_dup(v);
    }
    else
    {
        a = (MiniAttr *)calloc(1, sizeof(*a));
        a->name = mini_dup(k);
        a->value = mini_dup(v);
        a->next = n->attrs;
        n->attrs = a;
    }
    notify_mutation("attributeModified", NULL, n, k, v);
}

const char *mini_node_get_attribute(const struct MiniNode *n, const char *k)
{
    if (!n || !k)
        return NULL;
    for (const MiniAttr *a = n->attrs; a; a = a->next)
        if (strcmp(a->name, k) == 0)
            return a->value;
    return NULL;
}

/* remove an attribute (JS removeAttribute). Marks the node dirty. */
void mini_node_remove_attribute(struct MiniNode *n, const char *k)
{
    if (!n || !k)
        return;
    MiniAttr *prev = NULL;
    for (MiniAttr *p = n->attrs; p; prev = p, p = p->next)
    {
        if (strcmp(p->name, k) == 0)
        {
            if (prev)
                prev->next = p->next;
            else
                n->attrs = p->next;
            free(p->name);
            free(p->value);
            free(p);
            n->dirty_layout = 1;
            n->dirty_paint = 1;
            notify_mutation("attributeRemoved", NULL, n, k, NULL);
            return;
        }
    }
}

void mini_node_set_text(struct MiniNode *n, const char *t)
{
    if (!n)
        return;
    free(n->text);
    n->text = mini_dup(t);
}

/* ------------------------------------------------------------------ */
/* Style resolver: only the ~20 props layout/render consume.          */
/* Unknown props are dropped — same bet Flutter/Yoga make.            */
/* ------------------------------------------------------------------ */
static float to_px(const char *v)
{
    if (!v)
        return 0.0f;
    /* trim trailing px (only unit we honor) */
    return (float)atof(v);
}

/* ================================================================== */
/* Phase 4: deferred calc() resolution.                                */
/* calc(100% - 20px) can't resolve at parse time — the % base is only  */
/* known at layout. We stash a 2-term expression on the node (field-   */
/* keyed) and resolve it in layout_node where pct_base/font/vw exist.   */
/* unit 9 (CALC) on a MiniLength is the "deferred calc" sentinel.      */
/* Supported shape: calc(<term> <op> <term>) with op in + - * /; each  */
/* term is <number>[unit] (px/%/em/rem/vw/vh/vmin/vmax). Deeper/nested */
/* calc falls back to parse_length (→0) — noted as unsupported.        */
/* ================================================================== */
typedef enum
{
    CF_NONE = 0,
    CF_W = 1,
    CF_H,
    CF_MTOP,
    CF_MRIGHT,
    CF_MBOT,
    CF_MLEFT,
    CF_PTOP,
    CF_PRIGHT,
    CF_PBOT,
    CF_PLEFT,
    CF_TOP,
    CF_LEFT,
    CF_RIGHT,
    CF_BOTTOM,
    CF_FONT,
    CF_GAP
} CalcField;

typedef struct MiniCalc
{
    uint8_t field; /* CalcField */
    char *expr;    /* full var-resolved expression: "calc(...)", "min(...)", ... */
    struct MiniCalc *next;
} MiniCalc;

/* forward: resolve_len is defined in the layout section below. */
static float resolve_len(MiniLength L, float pct_base, float font,
                         float root_font, float vw, float vh);

/* store/replace a deferred calc/min/max/clamp expression for (n, field).
   expr is the full var-resolved expression string (e.g. "calc(100% - 20px)",
   "min(10px, 50%)", "clamp(0, 5vw, 100px)"). Resolved at layout time by the
   recursive-descent evaluator in mini_css.c. */
static void mini_calc_set(struct MiniNode *n, uint8_t field, const char *expr)
{
    if (!n || !expr)
        return;
    MiniCalc *c = (MiniCalc *)n->calc;
    while (c)
    {
        if (c->field == field)
            break;
        c = c->next;
    }
    if (!c)
    {
        c = (MiniCalc *)calloc(1, sizeof(*c));
        if (!c)
            return;
        c->next = (MiniCalc *)n->calc;
        n->calc = c;
    }
    c->field = field;
    free(c->expr);
    c->expr = strdup(expr);
}

static void mini_calc_free(struct MiniNode *n)
{
    if (!n)
        return;
    MiniCalc *c = (MiniCalc *)n->calc;
    while (c)
    {
        MiniCalc *nx = c->next;
        free(c->expr);
        free(c);
        c = nx;
    }
    n->calc = NULL;
}

static float mini_calc_resolve(struct MiniNode *n, uint8_t field,
                               float pct_base, float font,
                               float root_font, float vw, float vh)
{
    MiniCalc *c = (MiniCalc *)n->calc;
    for (; c; c = c->next)
        if (c->field == field)
            break;
    if (!c || !c->expr)
        return 0.0f;
    MiniCssCtx ctx = {.pct_base = pct_base, .font_px = font, .root_font_px = root_font, .vw = vw, .vh = vh};
    int ok = 0;

    char *rv = mini_css_resolve_vars_node(c->expr, n);
    char *cur_val = rv;
    int depth = 0;
    while (cur_val && strstr(cur_val, "var(") && depth++ < 5)
    {
        char *next = mini_css_resolve_vars_node(cur_val, n);
        free(cur_val);
        cur_val = next;
    }

    float v = mini_css_eval(cur_val ? cur_val : c->expr, &ctx, &ok);
    free(cur_val);
    return ok ? v : 0.0f;
}

/* set a length field. Routes calc()/min()/max()/clamp() (and any value
   containing var()) to the deferred store + CALC sentinel; the full
   var-resolved expression is resolved at layout time by mini_css_eval.
   A plain <length> goes through parse_length. */
static int starts_ci(const char *s, const char *pre)
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
static void set_len_field(struct MiniNode *n, uint8_t field,
                          const char *val, MiniLength *out)
{
    if (!val || !*val)
    {
        parse_length(val, out);
        return;
    }
    /* Any value that needs the layout context OR the (per-document) custom-
       property registry to resolve is stored verbatim as a deferred calc and
       resolved at layout time by mini_calc_resolve, so the var() registry is
       guaranteed fully populated (the whole stylesheet has been applied). */
    if (strstr(val, "var(") || starts_ci(val, "calc(") ||
        starts_ci(val, "min(") || starts_ci(val, "max(") ||
        starts_ci(val, "clamp("))
    {
        mini_calc_set(n, field, val);
        out->v = 0.0f;
        out->unit = 9; /* CALC sentinel */
        return;
    }
    parse_length(val, out);
}

/* Parse a CSS length into {value, unit}. unit: 0=px/unitless, 1=%,
   2=em, 3=rem, 4=vw, 5=vh, 6=vmin, 7=vmax. "auto"/unknown → {0,0}. */
static void parse_length(const char *v, MiniLength *out)
{
    out->v = 0.0f;
    out->unit = 0;
    if (!v || !*v)
        return;

    char *end;
    out->v = (float)strtod(v, &end);
    while (end && *end && isspace((unsigned char)*end))
        end++;

    if (!strncmp(end, "auto", 4))
    {
        out->unit = 8;
        return;
    }

    if (!end || !*end)
        return;
    if (end[0] == '%')
        out->unit = 1;
    else if (end[0] == 'e' && end[1] == 'm')
        out->unit = 2;
    else if (end[0] == 'r' && end[1] == 'e' && end[2] == 'm')
        out->unit = 3;
    else if (end[0] == 'c' && end[1] == 'h')
        out->unit = 10; /* ch unit */
    else if (end[0] == 'v' && end[1] == 'w')
        out->unit = 4;
    else if (end[0] == 'v' && end[1] == 'h')
        out->unit = 5;
    else if (end[0] == 'v' && end[1] == 'm' && end[2] == 'i' && end[3] == 'n')
        out->unit = 6;
    else if (end[0] == 'v' && end[1] == 'm' && end[2] == 'a' && end[3] == 'x')
        out->unit = 7;
}

/* Like parse_box4 but for raw lengths (margin/padding): each token is a
   MiniLength resolved later by the layout pass. Same 1/2/3/4 expansion. */
static void parse_len_box4_node(struct MiniNode *node, const uint8_t f[4], const char *v, MiniLength out[4])
{
    out[0].v = out[1].v = out[2].v = out[3].v = 0.0f;
    out[0].unit = out[1].unit = out[2].unit = out[3].unit = 0;
    if (!v)
        return;
    char tmp[256];
    strncpy(tmp, v, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    const char *tok[4] = {0, 0, 0, 0};
    char *save = NULL;
    int got = 0;
    for (char *t = strtok_r(tmp, " \t\r\n\f", &save); t && got < 4; t = strtok_r(NULL, " \t\r\n\f", &save))
        tok[got++] = t;
    if (got == 0)
        return;

    if (got == 1)
    {
        /* 单值简写：为 4 个方向全部注册该表达式，确保 left/right/bottom 均能正确解析 */
        set_len_field(node, f[0], tok[0], &out[0]);
        set_len_field(node, f[1], tok[0], &out[1]);
        set_len_field(node, f[2], tok[0], &out[2]);
        set_len_field(node, f[3], tok[0], &out[3]);
    }
    else
    {
        set_len_field(node, f[0], tok[0], &out[0]);
        out[1] = out[2] = out[3] = out[0];
        if (got >= 2)
        {
            set_len_field(node, f[1], tok[1], &out[1]);
            set_len_field(node, f[3], tok[1], &out[3]);
        }
        if (got >= 3)
            set_len_field(node, f[2], tok[2], &out[2]);
        if (got >= 4)
            set_len_field(node, f[3], tok[3], &out[3]);
    }
}

/* Expand a 1/2/3/4-value box shorthand (margin/padding) into
   [top, right, bottom, left] per CSS: 1→all; 2→tb,lr; 3→t,lr,b;
   4→t,r,b,l. Each token runs through to_px (unit-aware later). */
static void parse_box4(const char *v, float out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (!v)
        return;
    char tmp[128];
    size_t n = 0;
    while (*v && n < sizeof tmp - 1)
        tmp[n++] = *v++;
    tmp[n] = 0;
    const char *tok[4] = {0, 0, 0, 0};
    char *save = NULL;
    int got = 0;
    for (char *t = strtok_r(tmp, " \t\r\n\f", &save);
         t && got < 4;
         t = strtok_r(NULL, " \t\r\n\f", &save))
        tok[got++] = t;
    if (got == 0)
        return;
    out[0] = out[1] = out[2] = out[3] = to_px(tok[0]);
    if (got >= 2)
        out[1] = out[3] = to_px(tok[1]); /* right, left */
    if (got >= 3)
        out[2] = to_px(tok[2]); /* bottom */
    if (got >= 4)
        out[3] = to_px(tok[3]); /* left (override) */
}

/* classify a border-style keyword; -1 if not a style keyword */
static int border_style_id(const char *s)
{
    if (!s)
        return -1;
    if (!strcmp(s, "none") || !strcmp(s, "hidden"))
        return 0;
    if (!strcmp(s, "solid") || !strcmp(s, "double"))
        return 1;
    if (!strcmp(s, "dashed"))
        return 2;
    if (!strcmp(s, "dotted"))
        return 3;
    return -1;
}

static int find_transition_config(const MiniStyle *s, const char *prop, float *out_dur, int *out_timing, float out_bezier[4])
{
    *out_dur = s->transition_duration > 0.0f ? s->transition_duration : 0.3f;
    *out_timing = s->transition_timing;
    if (out_bezier)
        memcpy(out_bezier, s->transition_bezier, sizeof(float) * 4);
    if (!s->has_transition && s->num_transitions == 0)
        return 0;
    for (int i = 0; i < s->num_transitions; i++)
    {
        if (!strcmp(s->transitions[i].prop, "all") || !strcmp(s->transitions[i].prop, prop))
        {
            *out_dur = s->transitions[i].duration > 0.0f ? s->transitions[i].duration : *out_dur;
            *out_timing = s->transitions[i].timing;
            if (out_bezier)
                memcpy(out_bezier, s->transitions[i].bezier, sizeof(float) * 4);
            return 1;
        }
    }
    if (s->has_transition)
        return 1;
    return 0;
}

static void mini_add_transition_val(struct MiniNode *n, const char *prop, const float *start_vals, const float *target_vals, int num_vals, float duration, int timing, const float bezier[4])
{
    if (!n || !prop || num_vals <= 0)
        return;
    float dur = duration > 0.0f ? duration : 0.3f;

    int all_same = 1;
    for (int i = 0; i < num_vals; i++)
    {
        if (fabsf(start_vals[i] - target_vals[i]) > 1e-4f)
        {
            all_same = 0;
            break;
        }
    }
    if (all_same)
        return;

    MiniActiveTransition *tr = n->active_transitions;
    while (tr)
    {
        if (!strcmp(tr->prop, prop))
        {
            int target_same = 1;
            for (int i = 0; i < num_vals && i < tr->num_vals; i++)
            {
                if (fabsf(tr->target_val[i] - target_vals[i]) > 1e-4f)
                {
                    target_same = 0;
                    break;
                }
            }
            if (target_same)
                return;

            int tgt_changed = 0;
            for (int i = 0; i < num_vals && i < 8; i++)
            {
                if (fabsf(tr->target_val[i] - target_vals[i]) > 1e-4f)
                {
                    tgt_changed = 1;
                    break;
                }
            }
            if (!tgt_changed)
                return;

            float prog = (tr->duration > 0.0) ? (float)(tr->start_time / tr->duration) : 1.0f;
            if (prog > 1.0f)
                prog = 1.0f;
            float ease = mini_css_eval_timing(tr->timing, tr->bezier, prog);
            for (int i = 0; i < num_vals && i < 8; i++)
            {
                float cur_live = (i < tr->num_vals) ? (tr->start_val[i] * (1.0f - ease) + tr->target_val[i] * ease) : start_vals[i];
                tr->start_val[i] = cur_live;
                tr->target_val[i] = target_vals[i];
            }
            tr->num_vals = num_vals;
            tr->start_time = 0.0;
            tr->duration = dur;
            tr->timing = timing;
            if (bezier)
                memcpy(tr->bezier, bezier, sizeof(tr->bezier));
            return;
        }
        tr = tr->next;
    }

    tr = (MiniActiveTransition *)calloc(1, sizeof(MiniActiveTransition));
    strncpy(tr->prop, prop, sizeof(tr->prop) - 1);
    for (int i = 0; i < num_vals && i < 8; i++)
    {
        tr->start_val[i] = start_vals[i];
        tr->target_val[i] = target_vals[i];
    }
    tr->num_vals = num_vals;
    tr->start_time = 0.0;
    tr->duration = dur;
    tr->timing = timing;
    if (bezier)
        memcpy(tr->bezier, bezier, sizeof(tr->bezier));
    tr->next = n->active_transitions;
    n->active_transitions = tr;
}

static void mini_add_transition(struct MiniNode *n, const char *prop, float start_val, float target_val, float duration)
{
    float sv = start_val, tv = target_val;
    mini_add_transition_val(n, prop, &sv, &tv, 1, duration, 0, NULL);
}

/* Forward declaration: g_tick_doc is defined further down (mini_dom_tick_frame
   scope). mini_style_set folds layout-affecting mutations into
   doc->layout_dirty at zero traversal cost during the animation tick walk so
   the host loop can skip mini_layout_run on pure paint-only frames. */
static MiniDocument *g_tick_doc;

/* Properties that change GEOMETRY (require a layout re-run) vs. those that
   only change pixels (require paint but not layout). Used to gate
   doc->layout_dirty so that paint-only animation frames (opacity blink,
   background-color pulse, transform translate) skip the full layout pass.
   Conservative: anything not explicitly listed here is treated as
   layout-affecting (safer to over-layout than to mis-render). */
static int prop_affects_layout(const char *p)
{
    if (!p || !p[0])
        return 1; /* unknown → conservative */
    /* fast path: single-letter / common prefixes */
    switch (p[0])
    {
    case 'o':
        return strcmp(p, "opacity") ? 1 : 0;
    case 'c':
        return (strcmp(p, "color") && strcmp(p, "caret-color") &&
                strcmp(p, "cursor"))
                   ? 1
                   : 0;
    case 'b':
        if (!strcmp(p, "background") || !strcmp(p, "background-color") ||
            !strcmp(p, "background-image") || !strcmp(p, "background-position") ||
            !strcmp(p, "background-size") || !strcmp(p, "background-repeat") ||
            !strcmp(p, "background-clip") || !strcmp(p, "background-origin") ||
            !strcmp(p, "background-attachment"))
            return 0;
        if (!strcmp(p, "box-shadow"))
            return 0;
        if (!strcmp(p, "border-color") || !strcmp(p, "border-top-color") ||
            !strcmp(p, "border-right-color") || !strcmp(p, "border-bottom-color") ||
            !strcmp(p, "border-left-color"))
            return 0;
        return 1;
    case 't':
        if (!strcmp(p, "transform") || !strcmp(p, "text-shadow") ||
            !strcmp(p, "text-decoration") || !strcmp(p, "text-decoration-color"))
            return 0;
        return 1;
    case 'f':
        if (!strcmp(p, "filter") || !strcmp(p, "fill") || !strcmp(p, "flood-color"))
            return 0;
        return 1;
    case 'v':
        return strcmp(p, "visibility") ? 1 : 0;
    case '-':
        /* vendor-prefixed paint-only props */
        if (!strcmp(p, "-webkit-backdrop-filter") ||
            !strcmp(p, "-webkit-text-fill-color") ||
            !strcmp(p, "-webkit-text-stroke-color") ||
            !strncmp(p, "--", 2)) /* CSS custom property: never affects layout */
            return 0;
        return 1;
    case 'd':
        if (!strcmp(p, "drop-shadow"))
            return 0;
        return 1;
    }
    return 1;
}

void mini_style_set(struct MiniNode *n, const char *prop, const char *val)
{
    if (!n || !prop || !val)
        return;
    /* CSS custom property (--foo: value) */
    if (prop[0] == '-' && prop[1] == '-')
    {
        mini_node_set_var(n, prop + 2, val);
        return;
    }

    char *res_val = mini_css_resolve_vars_node(val, n);
    char *cur_val = res_val;
    int depth = 0;
    while (cur_val && strstr(cur_val, "var(") && depth++ < 5)
    {
        char *next = mini_css_resolve_vars_node(cur_val, n);
        free(cur_val);
        cur_val = next;
    }
    res_val = cur_val;
    const char *v = (res_val && res_val[0]) ? res_val : val;

    MiniStyle *s = &n->style;
    /* dirty gates: layout only when the prop changes geometry; paint always.
       During the animation tick (g_tick_doc set), fold layout-affecting
       mutations into doc->layout_dirty so the host can skip the full layout
       pass on pure paint-only animation frames. */
    int affects_layout = prop_affects_layout(prop);
    if (affects_layout)
    {
        n->dirty_layout = 1;
        if (g_tick_doc)
            g_tick_doc->layout_dirty = 1;
    }
    n->dirty_paint = 1;

    if (!strcmp(prop, "width"))
        set_len_field(n, CF_W, v, &s->len_w);
    else if (!strcmp(prop, "height"))
        set_len_field(n, CF_H, v, &s->len_h);
    else if (!strcmp(prop, "margin"))
    {
        static const uint8_t mf[4] = {CF_MTOP, CF_MRIGHT, CF_MBOT, CF_MLEFT};
        MiniLength tmp_box[4];
        parse_len_box4_node(n, mf, v, tmp_box);
        if (!cw_seen(n, cw_prop_hash("margin-top"), 0)) s->len_margin[0] = tmp_box[0];
        if (!cw_seen(n, cw_prop_hash("margin-right"), 0)) s->len_margin[1] = tmp_box[1];
        if (!cw_seen(n, cw_prop_hash("margin-bottom"), 0)) s->len_margin[2] = tmp_box[2];
        if (!cw_seen(n, cw_prop_hash("margin-left"), 0)) s->len_margin[3] = tmp_box[3];
    }
    else if (!strcmp(prop, "margin-top"))
        set_len_field(n, CF_MTOP, v, &s->len_margin[0]);
    else if (!strcmp(prop, "margin-right"))
        set_len_field(n, CF_MRIGHT, v, &s->len_margin[1]);
    else if (!strcmp(prop, "margin-bottom"))
        set_len_field(n, CF_MBOT, v, &s->len_margin[2]);
    else if (!strcmp(prop, "margin-left"))
        set_len_field(n, CF_MLEFT, v, &s->len_margin[3]);
    else if (!strcmp(prop, "padding"))
    {
        static const uint8_t pf[4] = {CF_PTOP, CF_PRIGHT, CF_PBOT, CF_PLEFT};
        MiniLength tmp_box[4];
        parse_len_box4_node(n, pf, v, tmp_box);
        if (!cw_seen(n, cw_prop_hash("padding-top"), 0)) s->len_padding[0] = tmp_box[0];
        if (!cw_seen(n, cw_prop_hash("padding-right"), 0)) s->len_padding[1] = tmp_box[1];
        if (!cw_seen(n, cw_prop_hash("padding-bottom"), 0)) s->len_padding[2] = tmp_box[2];
        if (!cw_seen(n, cw_prop_hash("padding-left"), 0)) s->len_padding[3] = tmp_box[3];
    }
    else if (!strcmp(prop, "padding-top"))
        set_len_field(n, CF_PTOP, v, &s->len_padding[0]);
    else if (!strcmp(prop, "padding-right"))
        set_len_field(n, CF_PRIGHT, v, &s->len_padding[1]);
    else if (!strcmp(prop, "padding-bottom"))
        set_len_field(n, CF_PBOT, v, &s->len_padding[2]);
    else if (!strcmp(prop, "padding-left"))
        set_len_field(n, CF_PLEFT, v, &s->len_padding[3]);
    else if (!strcmp(prop, "padding-block") || !strcmp(prop, "padding-block-start") || !strcmp(prop, "padding-block-end"))
    {
        set_len_field(n, CF_PTOP, v, &s->len_padding[0]);
        set_len_field(n, CF_PBOT, v, &s->len_padding[2]);
    }
    else if (!strcmp(prop, "padding-inline") || !strcmp(prop, "padding-inline-start") || !strcmp(prop, "padding-inline-end"))
    {
        set_len_field(n, CF_PRIGHT, v, &s->len_padding[1]);
        set_len_field(n, CF_PLEFT, v, &s->len_padding[3]);
    }
    else if (!strcmp(prop, "margin-block") || !strcmp(prop, "margin-block-start") || !strcmp(prop, "margin-block-end"))
    {
        set_len_field(n, CF_MTOP, v, &s->len_margin[0]);
        set_len_field(n, CF_MBOT, v, &s->len_margin[2]);
    }
    else if (!strcmp(prop, "margin-inline") || !strcmp(prop, "margin-inline-start") || !strcmp(prop, "margin-inline-end"))
    {
        if (strstr(v, "auto"))
        {
            s->len_margin[1].unit = 8;
            s->len_margin[3].unit = 8;
        }
        else
        {
            set_len_field(n, CF_MRIGHT, v, &s->len_margin[1]);
            set_len_field(n, CF_MLEFT, v, &s->len_margin[3]);
        }
    }
    else if (!strcmp(prop, "display"))
    {
        if (!strcmp(v, "flex"))
            s->display = MINI_DISPLAY_FLEX;
        else if (!strcmp(v, "none"))
            s->display = MINI_DISPLAY_NONE;
        else if (!strcmp(v, "inline") || !strcmp(v, "inline-block"))
            s->display = MINI_DISPLAY_INLINE;
        else if (!strcmp(v, "inline-flex"))
            s->display = MINI_DISPLAY_INLINE_FLEX;
        else if (!strcmp(v, "table"))
            s->display = MINI_DISPLAY_TABLE;
        else if (!strcmp(v, "list-item"))
            s->display = MINI_DISPLAY_LIST_ITEM;
        else if (!strcmp(v, "grid") || !strcmp(v, "inline-grid"))
        {
            s->display = MINI_DISPLAY_FLEX;
            s->is_grid = 1;
        }
        else
            s->display = MINI_DISPLAY_BLOCK;
    }
    else if (!strcmp(prop, "flex-direction"))
    {
        if (!strcmp(v, "row"))
            s->flex_direction = 0;
        else if (!strcmp(v, "column") || !strcmp(v, "col"))
            s->flex_direction = 1;
        else if (!strcmp(v, "row-reverse"))
            s->flex_direction = 2;
        else if (!strcmp(v, "column-reverse") || !strcmp(v, "col-reverse"))
            s->flex_direction = 3;
        else
            s->flex_direction = 0;
    }
    else if (!strcmp(prop, "order"))
        s->order = atoi(v);
    else if (!strcmp(prop, "align-content"))
    {
        if (!strcmp(v, "flex-start") || !strcmp(v, "start"))
            s->align_content = 1;
        else if (!strcmp(v, "flex-end") || !strcmp(v, "end"))
            s->align_content = 2;
        else if (!strcmp(v, "center"))
            s->align_content = 3;
        else if (!strcmp(v, "space-between"))
            s->align_content = 4;
        else if (!strcmp(v, "space-around"))
            s->align_content = 5;
        else if (!strcmp(v, "space-evenly"))
            s->align_content = 6;
        else
            s->align_content = 0; /* stretch */
    }
    else if (!strcmp(prop, "vertical-align"))
    {
        if (!strcmp(v, "middle"))
            s->vertical_align = 1;
        else if (!strcmp(v, "top"))
            s->vertical_align = 2;
        else if (!strcmp(v, "bottom"))
            s->vertical_align = 3;
        else if (!strcmp(v, "sub"))
            s->vertical_align = 4;
        else if (!strcmp(v, "super"))
            s->vertical_align = 5;
        else
            s->vertical_align = 0; /* baseline */
    }
    else if (!strcmp(prop, "flex-wrap"))
    {
        if (!strcmp(v, "wrap"))
            s->flex_wrap = 1;
        else if (!strcmp(v, "wrap-reverse"))
            s->flex_wrap = 2;
        else
            s->flex_wrap = 0;
    }
    else if (!strcmp(prop, "flex-grow"))
        s->flex_grow = (float)atof(v);
    else if (!strcmp(prop, "flex-shrink"))
        s->flex_shrink = (float)atof(v);
    else if (!strcmp(prop, "flex-basis"))
    {
        parse_length(v, &s->len_flex_basis);
        s->flex_basis_set = 1;
    }
    else if (!strcmp(prop, "flex"))
    {
        /* flex shorthand: grow [shrink] [basis] */
        char tmp[128];
        strncpy(tmp, v, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = 0;
        char *save;
        char *t1 = strtok_r(tmp, " ", &save);
        if (t1)
        {
            if (!strcmp(t1, "none"))
            {
                s->flex_grow = 0.0f;
                s->flex_shrink = 0.0f;
                s->len_flex_basis.unit = 8;
                s->flex_basis_set = 1;
            }
            else if (!strcmp(t1, "auto"))
            {
                s->flex_grow = 1.0f;
                s->flex_shrink = 1.0f;
                s->len_flex_basis.unit = 8;
                s->flex_basis_set = 1;
            }
            else
            {
                s->flex_grow = (float)atof(t1);
                char *t2 = strtok_r(NULL, " ", &save);
                if (t2)
                {
                    s->flex_shrink = (float)atof(t2);
                    char *t3 = strtok_r(NULL, " ", &save);
                    if (t3)
                    {
                        parse_length(t3, &s->len_flex_basis);
                        s->flex_basis_set = 1;
                    }
                }
                else
                {
                    s->flex_shrink = 1.0f;
                    s->len_flex_basis.v = 0.0f;
                    s->len_flex_basis.unit = 0;
                    s->flex_basis_set = 1;
                }
            }
        }
    }
    else if (!strcmp(prop, "border-top-left-radius"))
    {
        s->border_radius_corners[0] = to_px(v);
        s->border_radius_pct_corners[0] = strchr(v, '%') ? 1 : 0;
    }
    else if (!strcmp(prop, "border-top-right-radius"))
    {
        s->border_radius_corners[1] = to_px(v);
        s->border_radius_pct_corners[1] = strchr(v, '%') ? 1 : 0;
    }
    else if (!strcmp(prop, "border-bottom-right-radius"))
    {
        s->border_radius_corners[2] = to_px(v);
        s->border_radius_pct_corners[2] = strchr(v, '%') ? 1 : 0;
    }
    else if (!strcmp(prop, "border-bottom-left-radius"))
    {
        s->border_radius_corners[3] = to_px(v);
        s->border_radius_pct_corners[3] = strchr(v, '%') ? 1 : 0;
    }
    else if (!strcmp(prop, "border-radius"))
    {
        char tmp[128];
        strncpy(tmp, v, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = 0;
        char *toks[4];
        int ntok = 0;
        char *save;
        char *t = strtok_r(tmp, " \t\r\n", &save);
        while (t && ntok < 4)
        {
            toks[ntok++] = t;
            t = strtok_r(NULL, " \t\r\n", &save);
        }
        if (ntok == 1)
        {
            float r_val = to_px(toks[0]);
            uint8_t pct = strchr(toks[0], '%') ? 1 : 0;
            s->border_radius = r_val;
            s->border_radius_pct = pct;
            for (int i = 0; i < 4; i++)
            {
                s->border_radius_corners[i] = r_val;
                s->border_radius_pct_corners[i] = pct;
            }
        }
        else if (ntok == 2)
        {
            float r1 = to_px(toks[0]), r2 = to_px(toks[1]);
            uint8_t p1 = strchr(toks[0], '%') ? 1 : 0, p2 = strchr(toks[1], '%') ? 1 : 0;
            s->border_radius_corners[0] = s->border_radius_corners[2] = r1;
            s->border_radius_pct_corners[0] = s->border_radius_pct_corners[2] = p1;
            s->border_radius_corners[1] = s->border_radius_corners[3] = r2;
            s->border_radius_pct_corners[1] = s->border_radius_pct_corners[3] = p2;
            s->border_radius = r1;
        }
        else if (ntok == 3)
        {
            s->border_radius_corners[0] = to_px(toks[0]);
            s->border_radius_pct_corners[0] = strchr(toks[0], '%') ? 1 : 0;
            s->border_radius_corners[1] = s->border_radius_corners[3] = to_px(toks[1]);
            s->border_radius_pct_corners[1] = s->border_radius_pct_corners[3] = strchr(toks[1], '%') ? 1 : 0;
            s->border_radius_corners[2] = to_px(toks[2]);
            s->border_radius_pct_corners[2] = strchr(toks[2], '%') ? 1 : 0;
            s->border_radius = s->border_radius_corners[0];
        }
        else if (ntok >= 4)
        {
            for (int i = 0; i < 4; i++)
            {
                s->border_radius_corners[i] = to_px(toks[i]);
                s->border_radius_pct_corners[i] = strchr(toks[i], '%') ? 1 : 0;
            }
            s->border_radius = s->border_radius_corners[0];
        }
    }
    else if (!strncmp(prop, "border", 6))
    {
        s->has_border = 1;
        int side[4] = {1, 1, 1, 1};
        const char *rest = prop + 6;
        int aspect = 0;
        if (*rest == '-')
        {
            rest++;
            if (!strncmp(rest, "top", 3))
            {
                side[1] = side[2] = side[3] = 0;
                rest += 3;
            }
            else if (!strncmp(rest, "right", 5))
            {
                side[0] = side[2] = side[3] = 0;
                rest += 5;
            }
            else if (!strncmp(rest, "bottom", 6))
            {
                side[0] = side[1] = side[3] = 0;
                rest += 6;
            }
            else if (!strncmp(rest, "left", 4))
            {
                side[0] = side[1] = side[2] = 0;
                rest += 4;
            }
            if (*rest == '-')
            {
                rest++;
                if (!strcmp(rest, "width"))
                    aspect = 1;
                else if (!strcmp(rest, "style"))
                    aspect = 2;
                else if (!strcmp(rest, "color"))
                    aspect = 3;
            }
            else if (!strncmp(rest, "width", 5))
                aspect = 1;
            else if (!strncmp(rest, "style", 5))
                aspect = 2;
            else if (!strncmp(rest, "color", 5))
                aspect = 3;
        }

        if (aspect == 1)
        {
            float bw[4];
            parse_box4(v, bw);
            for (int i = 0; i < 4; i++)
                if (side[i])
                    s->border_w[i] = bw[i];
        }
        else if (aspect == 2)
        {
            int sid = border_style_id(v);
            if (sid >= 0)
                for (int i = 0; i < 4; i++)
                    if (side[i])
                        s->border_style[i] = (uint8_t)sid;
        }
        else if (aspect == 3)
        {
            float old_r = s->border_r, old_g = s->border_g, old_b = s->border_b, old_a = s->border_a;
            mini_parse_color(v, &s->border_r, &s->border_g, &s->border_b, &s->border_a);
            float dur = 0.3f;
            int timing = 0;
            float bez[4] = {0};
            if (n->has_base_style && (find_transition_config(s, "border-color", &dur, &timing, bez) || find_transition_config(s, "border", &dur, &timing, bez)))
            {
                float cur_val[4] = {old_r, old_g, old_b, old_a}, tgt_val[4] = {s->border_r, s->border_g, s->border_b, s->border_a};
                mini_add_transition_val(n, "border-color", cur_val, tgt_val, 4, dur, timing, bez);
                s->border_r = old_r;
                s->border_g = old_g;
                s->border_b = old_b;
                s->border_a = old_a;
            }
        }
        else
        {
            char tmp[128];
            strncpy(tmp, v, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = 0;
            char *p = tmp;
            while (*p)
            {
                while (*p && isspace((unsigned char)*p))
                    p++;
                if (!*p)
                    break;
                char tok[64];
                int ti = 0, parens = 0;
                while (*p && ti < 63)
                {
                    if (*p == '(')
                        parens++;
                    else if (*p == ')')
                        parens--;
                    if (!parens && isspace((unsigned char)*p))
                        break;
                    tok[ti++] = *p++;
                }
                tok[ti] = 0;
                int sid = border_style_id(tok);
                char c0 = tok[0];
                int is_len = (c0 >= '0' && c0 <= '9') || c0 == '.' || c0 == '+' || c0 == '-';
                if (sid >= 0)
                {
                    for (int i = 0; i < 4; i++)
                        if (side[i])
                            s->border_style[i] = (uint8_t)sid;
                }
                else if (is_len)
                {
                    float wv = to_px(tok);
                    for (int i = 0; i < 4; i++)
                        if (side[i])
                            s->border_w[i] = wv;
                }
                else
                {
                    parse_hex_color(tok, &s->border_r, &s->border_g, &s->border_b, &s->border_a);
                }
            }
        }
    }
    else if (!strcmp(prop, "text-align"))
    {
        if (!strcmp(v, "center"))
            s->text_align = 1;
        else if (!strcmp(v, "right"))
            s->text_align = 2;
        else if (!strcmp(v, "justify"))
            s->text_align = 3;
        else if (!strcmp(v, "left"))
            s->text_align = 4;
        else
            s->text_align = 0;
    }
    else if (!strcmp(prop, "line-height"))
    {
        parse_length(v, &s->len_line_height);
        s->line_height_set = 1;
    }
    else if (!strcmp(prop, "letter-spacing"))
    {
        parse_length(v, &s->len_letter);
        s->letter_set = 1;
    }
    else if (!strcmp(prop, "text-decoration") || !strcmp(prop, "text-decoration-line"))
    {
        if (strstr(v, "underline"))
            s->text_decoration = 1;
        else if (strstr(v, "line-through"))
            s->text_decoration = 2;
        else if (strstr(v, "overline"))
            s->text_decoration = 3;
        else
            s->text_decoration = 0;
    }
    else if (!strcmp(prop, "white-space"))
    {
        if (!strcmp(v, "nowrap"))
            s->white_space = 1;
        else if (!strcmp(v, "pre"))
            s->white_space = 2;
        else if (!strcmp(v, "pre-wrap"))
            s->white_space = 3;
        else
            s->white_space = 0;
    }
    else if (!strcmp(prop, "word-break"))
    {
        if (!strcmp(v, "break-all"))
            s->word_break = 1;
        else if (!strcmp(v, "keep-all"))
            s->word_break = 2;
        else
            s->word_break = 0;
    }
    else if (!strcmp(prop, "text-overflow"))
    {
        if (!strcmp(v, "ellipsis"))
            s->text_overflow = 1;
        else
            s->text_overflow = 0;
    }
    else if (!strcmp(prop, "background-size"))
    {
        if (!strcmp(v, "cover"))
            s->bg_size_mode = 1;
        else if (!strcmp(v, "contain"))
            s->bg_size_mode = 2;
        else
        {
            s->bg_size_mode = 3;
            char tmp[64];
            strncpy(tmp, v, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = 0;
            char *save;
            char *w_str = strtok_r(tmp, " ", &save);
            char *h_str = strtok_r(NULL, " ", &save);
            if (w_str)
                parse_length(w_str, &s->bg_size_w);
            if (h_str)
                parse_length(h_str, &s->bg_size_h);
        }
    }
    else if (!strcmp(prop, "background-position"))
    {
        if (strstr(v, "center"))
        {
            memset(&s->bg_pos_x, 0, sizeof(s->bg_pos_x));
            memset(&s->bg_pos_y, 0, sizeof(s->bg_pos_y));
        }
        else
        {
            char tmp[64];
            strncpy(tmp, v, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = 0;
            char *save;
            char *x_str = strtok_r(tmp, " ", &save);
            char *y_str = strtok_r(NULL, " ", &save);
            if (x_str)
                parse_length(x_str, &s->bg_pos_x);
            if (y_str)
                parse_length(y_str, &s->bg_pos_y);
        }
    }
    else if (!strcmp(prop, "background-repeat"))
    {
        if (!strcmp(v, "no-repeat"))
            s->bg_repeat = 0;
        else if (!strcmp(v, "repeat-x"))
            s->bg_repeat = 2;
        else if (!strcmp(v, "repeat-y"))
            s->bg_repeat = 3;
        else
            s->bg_repeat = 1;
    }
    else if (!strcmp(prop, "box-shadow"))
    {
        float old_sx = s->shadow_x, old_sy = s->shadow_y, old_blur = s->shadow_blur, old_spread = s->shadow_spread;
        float old_r = s->shadow_r, old_g = s->shadow_g, old_b = s->shadow_b, old_a = s->shadow_a;

        if (!strcmp(v, "none") || !*v)
        {
            s->has_shadow = 0;
            s->num_shadows = 0;
            s->shadow_a = 0.0f;
            float dur = 0.3f;
            int timing = 0;
            float bez[4] = {0};
            if (n->has_base_style && (find_transition_config(s, "box-shadow", &dur, &timing, bez) || find_transition_config(s, "shadow", &dur, &timing, bez)))
            {
                float cur_vals[8] = {old_sx, old_sy, old_blur, old_spread, old_r, old_g, old_b, old_a};
                float tgt_vals[8] = {0, 0, 0, 0, old_r, old_g, old_b, 0.0f};
                mini_add_transition_val(n, "box-shadow", cur_vals, tgt_vals, 8, dur, timing, bez);
            }
        }
        else
        {
            char buf[512];
            strncpy(buf, v, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            char *layer = buf;
            s->num_shadows = 0;
            while (layer && s->num_shadows < MINI_MAX_SHADOWS)
            {
                char *next = NULL;
                int depth = 0;
                for (char *p = layer; *p; p++)
                {
                    if (*p == '(')
                        depth++;
                    else if (*p == ')')
                        depth--;
                    else if (*p == ',' && depth == 0)
                    {
                        next = p;
                        break;
                    }
                }
                if (next)
                    *next = 0;
                char *inset = strstr(layer, "inset");
                if (inset)
                    while (*inset && *inset != ',')
                        *inset++ = ' ';
                float x = 0, y = 0, blur = 0, spread = 0;
                int nums = 0, have_color = 0;
                char color[64] = "rgba(0,0,0,0.5)";
                char *p = layer;
                while (*p)
                {
                    while (*p && isspace((unsigned char)*p))
                        p++;
                    if (!*p)
                        break;
                    if (*p == '#' || *p == 'r' || *p == 'R' || *p == 'h' || *p == 'H' || *p == 'o' || *p == 'c')
                    {
                        char *c = color;
                        int ci = 0, dpth = 0;
                        while (*p && ci < (int)sizeof(color) - 1)
                        {
                            if (*p == '(')
                                dpth++;
                            if (*p == ')' && dpth > 0)
                            {
                                *c++ = *p++;
                                dpth--;
                                continue;
                            }
                            if (dpth == 0 && isspace((unsigned char)*p))
                                break;
                            *c++ = *p++;
                            ci++;
                        }
                        *c = 0;
                        if (color[0])
                            have_color = 1;
                        continue;
                    }
                    char *e;
                    float val_n = (float)strtod(p, &e);
                    if (e == p)
                    {
                        p++;
                        continue;
                    }
                    while (*e == 'p' || *e == 'x')
                        e++;
                    if (nums == 0)
                        x = val_n;
                    else if (nums == 1)
                        y = val_n;
                    else if (nums == 2)
                        blur = val_n;
                    else
                        spread = val_n;
                    nums++;
                    p = e;
                }
                if (nums >= 2)
                {
                    int si = s->num_shadows;
                    s->shadows[si].x = x;
                    s->shadows[si].y = y;
                    s->shadows[si].blur = (nums >= 3) ? blur : 0.0f;
                    s->shadows[si].spread = (nums >= 4) ? spread : 0.0f;
                    s->shadows[si].r = 0.0f;
                    s->shadows[si].g = 0.0f;
                    s->shadows[si].b = 0.0f;
                    s->shadows[si].a = 0.5f;
                    s->shadows[si].inset = (inset != NULL);
                    if (have_color)
                        mini_parse_color(color, &s->shadows[si].r, &s->shadows[si].g, &s->shadows[si].b, &s->shadows[si].a);
                    if (si == 0)
                    {
                        s->has_shadow = 1;
                        s->shadow_x = s->shadows[0].x;
                        s->shadow_y = s->shadows[0].y;
                        s->shadow_blur = s->shadows[0].blur;
                        s->shadow_spread = s->shadows[0].spread;
                        s->shadow_r = s->shadows[0].r;
                        s->shadow_g = s->shadows[0].g;
                        s->shadow_b = s->shadows[0].b;
                        s->shadow_a = s->shadows[0].a;
                    }
                    s->num_shadows++;
                }
                layer = next ? next + 1 : NULL;
            }

            float dur = 0.3f;
            int timing = 0;
            float bez[4] = {0};
            if (n->has_base_style && (find_transition_config(s, "box-shadow", &dur, &timing, bez) || find_transition_config(s, "shadow", &dur, &timing, bez)))
            {
                float cur_vals[8] = {old_sx, old_sy, old_blur, old_spread, old_r, old_g, old_b, old_a};
                float tgt_vals[8] = {s->shadow_x, s->shadow_y, s->shadow_blur, s->shadow_spread, s->shadow_r, s->shadow_g, s->shadow_b, s->shadow_a};
                mini_add_transition_val(n, "box-shadow", cur_vals, tgt_vals, 8, dur, timing, bez);
                s->shadow_x = old_sx;
                s->shadow_y = old_sy;
                s->shadow_blur = old_blur;
                s->shadow_spread = old_spread;
                s->shadow_r = old_r;
                s->shadow_g = old_g;
                s->shadow_b = old_b;
                s->shadow_a = old_a;
                if (s->num_shadows > 0)
                {
                    s->shadows[0].x = old_sx;
                    s->shadows[0].y = old_sy;
                    s->shadows[0].blur = old_blur;
                    s->shadows[0].spread = old_spread;
                    s->shadows[0].r = old_r;
                    s->shadows[0].g = old_g;
                    s->shadows[0].b = old_b;
                    s->shadows[0].a = old_a;
                }
            }
        }
    }
    else if (!strcmp(prop, "background") || !strcmp(prop, "background-color") || !strcmp(prop, "background-image"))
    {
        const char *u = strstr(v, "url(");
        if (u)
        {
            u += 4;
            while (*u && (*u == '"' || *u == '\''))
                u++;
            const char *ue = strchr(u, ')');
            if (ue)
            {
                while (ue > u && (ue[-1] == '"' || ue[-1] == '\''))
                    ue--;
                size_t l = (size_t)(ue - u);
                if (l >= sizeof(s->bg_image_url))
                    l = sizeof(s->bg_image_url) - 1;
                memcpy(s->bg_image_url, u, l);
                s->bg_image_url[l] = '\0';
            }
        }
        const char *gl = NULL;
        int gtype = 0;
        const char *glp = strstr(v, "linear-gradient");
        const char *grp = strstr(v, "radial-gradient");
        const char *gcp = strstr(v, "conic-gradient");
        int is_repeating = (strstr(v, "repeating-") != NULL);
        if (glp)
        {
            gl = glp;
            gtype = is_repeating ? 3 : 0;
        }
        else if (grp)
        {
            gl = grp;
            gtype = 1;
        }
        else if (gcp)
        {
            gl = gcp;
            gtype = 2;
        }

        if (gl)
        {
            const char *open = strchr(gl, '(');
            const char *close = open ? strrchr(open, ')') : NULL;
            if (open && close && close > open)
            {
                char args[512];
                size_t len = (size_t)(close - open - 1);
                if (len >= sizeof(args))
                    len = sizeof(args) - 1;
                memcpy(args, open + 1, len);
                args[len] = 0;

                int got = 0;
                float angle = -1.0f;
                const char *p = args;
                while (*p && got < 16)
                {
                    while (*p && isspace((unsigned char)*p))
                        p++;
                    if (!*p)
                        break;
                    const char *tok_start = p;
                    int depth = 0;
                    while (*p && !(depth == 0 && *p == ','))
                    {
                        if (*p == '(')
                            depth++;
                        else if (*p == ')')
                            depth--;
                        p++;
                    }
                    char tok[128];
                    size_t tl = (size_t)(p - tok_start);
                    if (tl >= sizeof(tok))
                        tl = sizeof(tok) - 1;
                    memcpy(tok, tok_start, tl);
                    tok[tl] = '\0';
                    if (*p == ',')
                        p++;

                    char *ts = tok;
                    while (*ts && isspace((unsigned char)*ts))
                        ts++;
                    char *te = ts + strlen(ts);
                    while (te > ts && isspace((unsigned char)te[-1]))
                        *--te = '\0';

                    if (*ts)
                    {
                        int is_dir = 0;
                        if (!strncmp(ts, "to ", 3))
                        {
                            is_dir = 1;
                            if (strstr(ts, "bottom"))
                                angle = 180;
                            else if (strstr(ts, "top"))
                                angle = 0;
                            else if (strstr(ts, "right"))
                                angle = 90;
                            else if (strstr(ts, "left"))
                                angle = 270;
                        }
                        else
                        {
                            char *degs = strstr(ts, "deg");
                            if (degs)
                            {
                                angle = (float)atof(ts);
                                is_dir = 1;
                            }
                        }
                        if (!is_dir)
                        {
                            float r, g, b, a;
                            char col_str[64] = {0};
                            float stop_pos = -1.0f;
                            char *pct_pos = strchr(ts, '%');
                            char *px_pos = strstr(ts, "px");
                            if (pct_pos || px_pos)
                            {
                                char *num_start = pct_pos ? pct_pos : px_pos;
                                while (num_start > ts && (isdigit((unsigned char)num_start[-1]) || num_start[-1] == '.'))
                                    num_start--;

                                float parsed_val = (float)atof(num_start);
                                stop_pos = pct_pos ? (parsed_val / 100.0f) : (is_repeating ? parsed_val : (parsed_val / 100.0f));

                                size_t c_len = (size_t)(num_start - ts);
                                while (c_len > 0 && isspace((unsigned char)ts[c_len - 1]))
                                    c_len--;
                                if (c_len >= sizeof(col_str))
                                    c_len = sizeof(col_str) - 1;
                                memcpy(col_str, ts, c_len);
                                col_str[c_len] = '\0';
                            }
                            else
                            {
                                strncpy(col_str, ts, sizeof(col_str) - 1);
                                char *ce = col_str + strlen(col_str);
                                while (ce > col_str && isspace((unsigned char)ce[-1]))
                                    *--ce = '\0';
                            }

                            if (mini_parse_color(col_str[0] ? col_str : ts, &r, &g, &b, &a))
                            {
                                s->grad_stops[got].r = r;
                                s->grad_stops[got].g = g;
                                s->grad_stops[got].b = b;
                                s->grad_stops[got].a = a;
                                s->grad_stops[got].pos = stop_pos;
                                got++;
                            }
                        }
                    }
                }
                if (got >= 2)
                {
                    int is_repeating = strstr(v, "repeating-") != NULL;
                    if (is_repeating)
                    {
                        float cycle = s->grad_stops[got - 1].pos - s->grad_stops[0].pos;
                        if (cycle > 0.001f)
                        {
                            int orig_got = got;
                            while (got < 16)
                            {
                                int src_idx = got % orig_got;
                                int cycle_num = got / orig_got;
                                s->grad_stops[got].r = s->grad_stops[src_idx].r;
                                s->grad_stops[got].g = s->grad_stops[src_idx].g;
                                s->grad_stops[got].b = s->grad_stops[src_idx].b;
                                s->grad_stops[got].a = s->grad_stops[src_idx].a;
                                s->grad_stops[got].pos = s->grad_stops[src_idx].pos + cycle * (float)cycle_num;
                                if (s->grad_stops[got].pos > 1.0f)
                                {
                                    got++;
                                    break;
                                }
                                got++;
                            }
                        }
                    }

                    s->has_gradient = 1;
                    s->grad_type = gtype;
                    s->grad_num_stops = got;
                    s->grad_r1 = s->grad_stops[0].r;
                    s->grad_g1 = s->grad_stops[0].g;
                    s->grad_b1 = s->grad_stops[0].b;
                    s->grad_a1 = s->grad_stops[0].a;
                    s->grad_r2 = s->grad_stops[got - 1].r;
                    s->grad_g2 = s->grad_stops[got - 1].g;
                    s->grad_b2 = s->grad_stops[got - 1].b;
                    s->grad_a2 = s->grad_stops[got - 1].a;
                    if (angle < 0)
                        angle = 180;
                    s->grad_angle = angle;
                    s->grad_vertical = (angle == 180 || angle == 0) ? 1 : 0;
                }
                else if (got == 1)
                {
                    s->has_gradient = 0;
                    s->bg_r = s->grad_stops[0].r;
                    s->bg_g = s->grad_stops[0].g;
                    s->bg_b = s->grad_stops[0].b;
                    s->bg_a = s->grad_stops[0].a;
                }
            }
        }
        else
        {
            float old_r = s->bg_r, old_g = s->bg_g, old_b = s->bg_b, old_a = s->bg_a;
            s->has_gradient = 0;
            mini_parse_color(v, &s->bg_r, &s->bg_g, &s->bg_b, &s->bg_a);
            float dur = 0.3f;
            int timing = 0;
            float bez[4] = {0};
            if (!g_restyling && (find_transition_config(s, "background-color", &dur, &timing, bez) || find_transition_config(s, "background", &dur, &timing, bez)))
            {
                float cur_val[4] = {old_r, old_g, old_b, old_a}, tgt_val[4] = {s->bg_r, s->bg_g, s->bg_b, s->bg_a};
                mini_add_transition_val(n, "background-color", cur_val, tgt_val, 4, dur, timing, bez);
                s->bg_r = old_r;
                s->bg_g = old_g;
                s->bg_b = old_b;
                s->bg_a = old_a;
            }
        }
    }
    else if (!strcmp(prop, "color"))
    {
        if (!strcmp(v, "inherit"))
        {
            s->color_set = 0;
        }
        else
        {
            float old_r = s->color_r, old_g = s->color_g, old_b = s->color_b, old_a = s->color_a;
            int ok = mini_parse_color(v, &s->color_r, &s->color_g, &s->color_b, &s->color_a);
            s->color_set = ok ? 1 : 0;
            float dur = 0.3f;
            int timing = 0;
            float bez[4] = {0};
            if (g_restyling && n->has_base_style && find_transition_config(s, "color", &dur, &timing, bez))
            {
                float cur_val[4] = {old_r, old_g, old_b, old_a}, tgt_val[4] = {s->color_r, s->color_g, s->color_b, s->color_a};
                mini_add_transition_val(n, "color", cur_val, tgt_val, 4, dur, timing, bez);
                if (old_r != 0.0f || old_g != 0.0f || old_b != 0.0f)
                {
                    s->color_r = old_r;
                    s->color_g = old_g;
                    s->color_b = old_b;
                    s->color_a = old_a;
                }
            }
        }
    }
    else if (!strcmp(prop, "font-size"))
    {
        set_len_field(n, CF_FONT, v, &s->len_font);
        s->font_set = 1;
    }
    else if (!strcmp(prop, "min-width"))
        parse_length(v, &s->len_min_w);
    else if (!strcmp(prop, "max-width"))
        parse_length(v, &s->len_max_w);
    else if (!strcmp(prop, "min-height"))
        parse_length(v, &s->len_min_h);
    else if (!strcmp(prop, "max-height"))
        parse_length(v, &s->len_max_h);
    else if (!strcmp(prop, "gap") || !strcmp(prop, "row-gap") || !strcmp(prop, "column-gap"))
        set_len_field(n, CF_GAP, v, &s->len_gap);
    else if (!strcmp(prop, "align-self"))
    {
        if (!strcmp(v, "auto"))
            s->align_self = -1;
        else if (!strcmp(v, "stretch"))
            s->align_self = 0;
        else if (!strcmp(v, "flex-start"))
            s->align_self = 1;
        else if (!strcmp(v, "center"))
            s->align_self = 2;
        else if (!strcmp(v, "flex-end"))
            s->align_self = 3;
        else if (!strcmp(v, "baseline"))
            s->align_self = 4;
    }
    else if (!strcmp(prop, "overflow") || !strcmp(prop, "overflow-x") || !strcmp(prop, "overflow-y"))
        s->overflow = (!strcmp(v, "visible")) ? 0 : 1;
    else if (!strcmp(prop, "position"))
    {
        if (!strcmp(v, "relative"))
            s->position = 1;
        else if (!strcmp(v, "absolute"))
            s->position = 2;
        else if (!strcmp(v, "fixed"))
            s->position = 3;
        else if (!strcmp(v, "sticky"))
            s->position = 4;
        else
            s->position = 0;
    }
    else if (!strcmp(prop, "top"))
    {
        float old_v = s->len_top.v;
        set_len_field(n, CF_TOP, v, &s->len_top);
        float dur = 0.3f;
        int timing = 0;
        float bez[4] = {0};
        if (n->has_base_style && find_transition_config(s, "top", &dur, &timing, bez))
        {
            float cur_val = old_v, tgt_val = s->len_top.v;
            mini_add_transition_val(n, "top", &cur_val, &tgt_val, 1, dur, timing, bez);
            s->len_top.v = old_v;
        }
    }
    else if (!strcmp(prop, "left"))
    {
        float old_v = s->len_left.v;
        set_len_field(n, CF_LEFT, v, &s->len_left);
        float dur = 0.3f;
        int timing = 0;
        float bez[4] = {0};
        if (n->has_base_style && find_transition_config(s, "left", &dur, &timing, bez))
        {
            float cur_val = old_v, tgt_val = s->len_left.v;
            mini_add_transition_val(n, "left", &cur_val, &tgt_val, 1, dur, timing, bez);
            s->len_left.v = old_v;
        }
    }
    else if (!strcmp(prop, "right"))
        set_len_field(n, CF_RIGHT, v, &s->len_right);
    else if (!strcmp(prop, "bottom"))
        set_len_field(n, CF_BOTTOM, v, &s->len_bottom);
    else if (!strcmp(prop, "inset"))
    {
        MiniLength box[4];
        static const uint8_t inf[4] = {CF_TOP, CF_RIGHT, CF_BOTTOM, CF_LEFT};
        parse_len_box4_node(n, inf, v, box);
        s->len_top = box[0];
        s->len_right = box[1];
        s->len_bottom = box[2];
        s->len_left = box[3];
    }
    else if (!strcmp(prop, "clip-path") || !strcmp(prop, "clip"))
    {
        if (strstr(v, "polygon"))
        {
            const char *p = strchr(v, '(');
            if (p)
            {
                p++;
                int count = 0;
                while (*p && count < 8)
                {
                    while (*p && (isspace((unsigned char)*p) || *p == ','))
                        p++;
                    if (!*p || *p == ')')
                        break;
                    char x_str[32] = {0}, y_str[32] = {0};
                    int xi = 0, yi = 0;
                    while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != ')')
                    {
                        if (xi < 31)
                            x_str[xi++] = *p;
                        p++;
                    }
                    while (*p && isspace((unsigned char)*p))
                        p++;
                    while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != ')')
                    {
                        if (yi < 31)
                            y_str[yi++] = *p;
                        p++;
                    }
                    float xf = (float)atof(x_str);
                    float yf = (float)atof(y_str);
                    if (strchr(x_str, '%'))
                        xf /= 100.0f;
                    if (strchr(y_str, '%'))
                        yf /= 100.0f;
                    s->clip_poly_pts[count][0] = xf;
                    s->clip_poly_pts[count][1] = yf;
                    count++;
                }
                s->num_clip_poly_pts = count;
                s->has_clip_polygon = (count >= 3);
            }
        }
        else if (strstr(v, "rect"))
        {
            const char *p = strchr(v, '(');
            if (p)
            {
                p++;
                float vals[4] = {0};
                int count = 0;
                while (*p && count < 4)
                {
                    while (*p && (isspace((unsigned char)*p) || *p == ','))
                        p++;
                    if (!*p || *p == ')')
                        break;
                    vals[count++] = (float)atof(p);
                    while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != ')')
                        p++;
                }
                s->clip_rect[0] = vals[0];
                s->clip_rect[1] = vals[1];
                s->clip_rect[2] = vals[2];
                s->clip_rect[3] = vals[3];
                s->has_clip_rect = 1;
            }
        }
    }
    else if (!strcmp(prop, "z-index"))
        s->z_index = (int)atoi(v);
    else if (!strcmp(prop, "box-sizing"))
        s->box_sizing = (!strcmp(v, "border-box")) ? 1 : 0;
    else if (!strcmp(prop, "justify-content"))
    {
        if (!strcmp(v, "center"))
            s->justify_content = 1;
        else if (!strcmp(v, "flex-end"))
            s->justify_content = 2;
        else if (!strcmp(v, "space-between"))
            s->justify_content = 3;
        else if (!strcmp(v, "space-around"))
            s->justify_content = 4;
        else if (!strcmp(v, "space-evenly"))
            s->justify_content = 5;
        else
            s->justify_content = 0;
    }
    else if (!strcmp(prop, "align-items"))
    {
        if (!strcmp(v, "flex-start"))
            s->align_items = 1;
        else if (!strcmp(v, "center"))
            s->align_items = 2;
        else if (!strcmp(v, "flex-end"))
            s->align_items = 3;
        else if (!strcmp(v, "baseline"))
            s->align_items = 4;
        else
            s->align_items = 0;
    }
    else if (!strcmp(prop, "text-transform"))
    {
        if (!strcmp(v, "uppercase"))
            s->text_transform = 1;
        else if (!strcmp(v, "lowercase"))
            s->text_transform = 2;
        else if (!strcmp(v, "capitalize"))
            s->text_transform = 3;
        else
            s->text_transform = 0;
    }
    else if (!strcmp(prop, "font-weight"))
    {
        if (!strcmp(v, "bold") || !strcmp(v, "bolder"))
            s->font_weight = 700;
        else if (!strcmp(v, "normal"))
            s->font_weight = 400;
        else
            s->font_weight = (int)atoi(v);
    }
    else if (!strcmp(prop, "font-style"))
    {
        if (!strcmp(v, "italic") || !strcmp(v, "oblique"))
            s->font_style = 1;
        else
            s->font_style = 0;
    }
    else if (!strcmp(prop, "grid-template-columns"))
    {
        int cnt = 0;
        const char *p = v;
        while (*p)
        {
            while (*p && (isspace((unsigned char)*p) || *p == ','))
                p++;
            if (!*p)
                break;
            if (!strncmp(p, "repeat(", 7))
            {
                int n_cnt = atoi(p + 7);
                if (n_cnt > 0)
                    cnt += n_cnt;
                int rdepth = 0;
                while (*p)
                {
                    if (*p == '(')
                        rdepth++;
                    else if (*p == ')')
                    {
                        rdepth--;
                        if (rdepth == 0)
                        {
                            p++;
                            break;
                        }
                    }
                    p++;
                }
            }
            else
            {
                cnt++;
                int depth = 0;
                while (*p && !(depth == 0 && (isspace((unsigned char)*p) || *p == ',')))
                {
                    if (*p == '(')
                        depth++;
                    else if (*p == ')')
                        depth--;
                    p++;
                }
            }
        }
        s->grid_cols = cnt;
        strncpy(s->grid_gtc, v, sizeof(s->grid_gtc) - 1);
        s->grid_gtc[sizeof(s->grid_gtc) - 1] = 0;
        s->is_grid = 1;
    }
    else if (!strcmp(prop, "grid-template-rows"))
    {
        strncpy(s->grid_gtr, v, sizeof(s->grid_gtr) - 1);
        s->grid_gtr[sizeof(s->grid_gtr) - 1] = 0;
        s->is_grid = 1;
    }
    else if (!strcmp(prop, "grid-auto-flow"))
    {
        if (strstr(v, "dense"))
            s->grid_auto_flow_dense = 1;
        else
            s->grid_auto_flow_dense = 0;
    }
    else if (!strcmp(prop, "grid-template-areas"))
    {
        strncpy(s->grid_areas, v, sizeof(s->grid_areas) - 1);
        s->grid_areas[sizeof(s->grid_areas) - 1] = 0;
        s->is_grid = 1;
    }
    else if (!strcmp(prop, "grid-area"))
    {
        strncpy(s->grid_area, v, sizeof(s->grid_area) - 1);
        s->grid_area[sizeof(s->grid_area) - 1] = 0;
    }
    else if (!strcmp(prop, "grid-column") || !strcmp(prop, "grid-column-start"))
    {
        if (strstr(v, "span"))
        {
            const char *sp = strstr(v, "span");
            s->grid_col_span = (uint8_t)atoi(sp + 4);
        }
        else
        {
            s->grid_col_start = (uint8_t)atoi(v);
        }
    }
    else if (!strcmp(prop, "grid-row") || !strcmp(prop, "grid-row-start"))
    {
        if (strstr(v, "span"))
        {
            const char *sp = strstr(v, "span");
            s->grid_row_span = (uint8_t)atoi(sp + 4);
        }
        else
        {
            s->grid_row_start = (uint8_t)atoi(v);
        }
    }
    else if (!strcmp(prop, "transform"))
    {
        float old_tx = s->translate_x, old_ty = s->translate_y, old_tz = s->translate_z;
        float old_sx = s->has_transform ? s->scale_x : 1.0f;
        float old_sy = s->has_transform ? s->scale_y : 1.0f;
        float old_rx = s->rotate_x, old_ry = s->rotate_y, old_rz = s->rotate_z;
        float old_skx = s->skew_x, old_sky = s->skew_y;

        s->has_transform = 1;
        s->translate_x = s->translate_y = s->translate_z = 0.0f;
        s->scale_x = s->scale_y = 1.0f;
        s->rotate_x = s->rotate_y = s->rotate_z = 0.0f;
        s->skew_x = s->skew_y = 0.0f;
        s->perspective = 0.0f;

        if (strcmp(v, "none") != 0 && *v)
        {
            const char *p = v;
            while (*p)
            {
                while (*p && isspace((unsigned char)*p))
                    p++;
                if (!*p)
                    break;
                if (!strncmp(p, "perspective(", 12))
                {
                    s->perspective = to_px(p + 12);
                }
                else if (!strncmp(p, "translate", 9))
                {
                    float dx = 0, dy = 0, dz = 0;
                    if (!strncmp(p, "translate3d(", 12))
                    {
                        char tmp[64];
                        const char *e = strchr(p, ')');
                        size_t l = e ? (size_t)(e - p - 12) : 0;
                        if (l >= sizeof(tmp))
                            l = sizeof(tmp) - 1;
                        memcpy(tmp, p + 12, l);
                        tmp[l] = 0;
                        char *save;
                        char *tx = strtok_r(tmp, ",", &save);
                        char *ty = strtok_r(NULL, ",", &save);
                        char *tz = strtok_r(NULL, ",", &save);
                        if (tx)
                            dx = to_px(tx);
                        if (ty)
                            dy = to_px(ty);
                        if (tz)
                            dz = to_px(tz);
                    }
                    else if (!strncmp(p, "translateZ(", 11))
                    {
                        dz = to_px(p + 11);
                    }
                    else if (!strncmp(p, "translateX(", 11))
                    {
                        dx = to_px(p + 11);
                    }
                    else if (!strncmp(p, "translateY(", 11))
                    {
                        dy = to_px(p + 11);
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
                        char *tx = strtok_r(tmp, ",", &save);
                        char *ty = strtok_r(NULL, ",", &save);
                        if (tx)
                            dx = to_px(tx);
                        if (ty)
                            dy = to_px(ty);
                    }

                    float rx = s->rotate_x, ry = s->rotate_y, rz = s->rotate_z;

                    float x1 = dx, y1 = dy, z1 = dz;

                    float x2 = x1 * cosf(ry) + z1 * sinf(ry);
                    float y2 = y1;
                    float z2 = -x1 * sinf(ry) + z1 * cosf(ry);

                    float x3 = x2;
                    float y3 = y2 * cosf(rx) - z2 * sinf(rx);
                    float z3 = y2 * sinf(rx) + z2 * cosf(rx);

                    float x4 = x3 * cosf(rz) - y3 * sinf(rz);
                    float y4 = x3 * sinf(rz) + y3 * cosf(rz);
                    float z4 = z3;

                    s->translate_x += x4;
                    s->translate_y += y4;
                    s->translate_z += z4;
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
                    char *sx = strtok_r(tmp, ",", &save);
                    char *sy = strtok_r(NULL, ",", &save);
                    if (sx)
                        s->scale_x = (float)atof(sx);
                    if (sy)
                        s->scale_y = (float)atof(sy);
                }
                else if (!strncmp(p, "scaleX(", 7))
                {
                    s->scale_x = (float)atof(p + 7);
                }
                else if (!strncmp(p, "scaleY(", 7))
                {
                    s->scale_y = (float)atof(p + 7);
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
                    char *sx = strtok_r(tmp, ",", &save);
                    char *sy = strtok_r(NULL, ",", &save);
                    if (sx)
                        s->scale_x = (float)atof(sx);
                    if (sy)
                        s->scale_y = (float)atof(sy);
                    else if (sx)
                        s->scale_y = s->scale_x;
                }
                else if (!strncmp(p, "rotateX(", 8))
                {
                    s->rotate_x = (float)atof(p + 8) * (3.14159265f / 180.0f);
                }
                else if (!strncmp(p, "rotateY(", 8))
                {
                    s->rotate_y = (float)atof(p + 8) * (3.14159265f / 180.0f);
                }
                else if (!strncmp(p, "rotateZ(", 8) || !strncmp(p, "rotate(", 7))
                {
                    const char *st = strchr(p, '(');
                    s->rotate_z = (float)atof(st ? st + 1 : p) * (3.14159265f / 180.0f);
                }
                else if (!strncmp(p, "skewX(", 6))
                {
                    s->skew_x = (float)atof(p + 6) * (3.14159265f / 180.0f);
                }
                else if (!strncmp(p, "skewY(", 6))
                {
                    s->skew_y = (float)atof(p + 6) * (3.14159265f / 180.0f);
                }
                else if (!strncmp(p, "skew(", 5))
                {
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
                        s->skew_x = (float)atof(x_s) * (3.14159265f / 180.0f);
                    if (y_s)
                        s->skew_y = (float)atof(y_s) * (3.14159265f / 180.0f);
                }
                const char *cp = strchr(p, ')');
                p = cp ? cp + 1 : p + strlen(p);
            }
        }

        float dur = 0.3f;
        int timing = 0;
        float bez[4] = {0};
        if (n->has_base_style && find_transition_config(s, "transform", &dur, &timing, bez))
        {
            float cur_vals[8] = {old_tx, old_ty, old_tz, old_sx, old_sy, old_rx, old_ry, old_rz};
            float tgt_vals[8] = {s->translate_x, s->translate_y, s->translate_z, s->scale_x, s->scale_y, s->rotate_x, s->rotate_y, s->rotate_z};
            mini_add_transition_val(n, "transform", cur_vals, tgt_vals, 8, dur, timing, bez);
            s->translate_x = old_tx;
            s->translate_y = old_ty;
            s->translate_z = old_tz;
            s->scale_x = old_sx;
            s->scale_y = old_sy;
            s->rotate_x = old_rx;
            s->rotate_y = old_ry;
            s->rotate_z = old_rz;
        }
    }
    else if (!strcmp(prop, "transform-origin"))
    {
        char tmp[64];
        strncpy(tmp, v, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = 0;
        char *save;
        char *ox = strtok_r(tmp, " ", &save);
        char *oy = strtok_r(NULL, " ", &save);
        if (ox)
        {
            if (strstr(ox, "%"))
                s->transform_origin_x = (float)atof(ox) / 100.0f;
            else if (!strcmp(ox, "center"))
                s->transform_origin_x = 0.5f;
            else if (!strcmp(ox, "left"))
                s->transform_origin_x = 0.0f;
            else if (!strcmp(ox, "right"))
                s->transform_origin_x = 1.0f;
        }
        if (oy)
        {
            if (strstr(oy, "%"))
                s->transform_origin_y = (float)atof(oy) / 100.0f;
            else if (!strcmp(oy, "center"))
                s->transform_origin_y = 0.5f;
            else if (!strcmp(oy, "top"))
                s->transform_origin_y = 0.0f;
            else if (!strcmp(oy, "bottom"))
                s->transform_origin_y = 1.0f;
        }
    }
    else if (!strcmp(prop, "perspective"))
    {
        if (v && strcmp(v, "none") != 0)
        {
            float pv = to_px(v);
            s->perspective = pv > 0.0f ? pv : 0.0f;
        }
        else
            s->perspective = 0.0f;
    }
    else if (!strcmp(prop, "transition") || !strcmp(prop, "transition-property") || !strcmp(prop, "transition-duration"))
    {
        s->has_transition = 1;
        char tmp[512];
        strncpy(tmp, v, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = 0;

        const char *lp = tmp;
        int ti = 0;
        while (*lp && ti < MINI_MAX_TRANSITIONS)
        {
            while (*lp && isspace((unsigned char)*lp))
                lp++;
            if (!*lp)
                break;
            const char *layer_start = lp;
            int depth = 0;
            while (*lp && (depth > 0 || *lp != ','))
            {
                if (*lp == '(')
                    depth++;
                else if (*lp == ')')
                    depth--;
                lp++;
            }
            char layer[256] = {0};
            size_t ll = (size_t)(lp - layer_start);
            if (ll >= sizeof(layer))
                ll = sizeof(layer) - 1;
            memcpy(layer, layer_start, ll);
            layer[ll] = 0;
            if (*lp == ',')
                lp++;

            char prop_name[32] = "all";
            float dur = 0.3f, delay = 0.0f;
            int timing = 1;
            float bez[4] = {0.25f, 0.1f, 0.25f, 1.0f};
            int has_dur = 0;

            const char *p = layer;
            while (*p)
            {
                while (*p && isspace((unsigned char)*p))
                    p++;
                if (!*p)
                    break;
                const char *tok_start = p;
                int pdepth = 0;
                while (*p && (pdepth > 0 || !isspace((unsigned char)*p)))
                {
                    if (*p == '(')
                        pdepth++;
                    else if (*p == ')')
                        pdepth--;
                    p++;
                }
                char tok[128] = {0};
                size_t tl = (size_t)(p - tok_start);
                if (tl >= sizeof(tok))
                    tl = sizeof(tok) - 1;
                memcpy(tok, tok_start, tl);
                tok[tl] = 0;

                if (strstr(tok, "cubic-bezier"))
                {
                    char *b_st = strchr(tok, '(');
                    if (b_st)
                    {
                        float x1 = 0, y1 = 0, x2 = 1, y2 = 1;
                        sscanf(b_st + 1, "%f,%f,%f,%f", &x1, &y1, &x2, &y2);
                        bez[0] = x1;
                        bez[1] = y1;
                        bez[2] = x2;
                        bez[3] = y2;
                        timing = 5;
                    }
                }
                else if (!strcmp(tok, "linear"))
                    timing = 0;
                else if (!strcmp(tok, "ease"))
                    timing = 1;
                else if (!strcmp(tok, "ease-in"))
                    timing = 2;
                else if (!strcmp(tok, "ease-out"))
                    timing = 3;
                else if (!strcmp(tok, "ease-in-out"))
                    timing = 4;
                else if (isdigit((unsigned char)tok[0]) || (tok[0] == '.' && isdigit((unsigned char)tok[1])))
                {
                    float n_val = (float)atof(tok);
                    if (strstr(tok, "ms"))
                        n_val /= 1000.0f;
                    if (!has_dur)
                    {
                        dur = n_val;
                        has_dur = 1;
                    }
                    else
                    {
                        delay = n_val;
                    }
                }
                else if (tok[0])
                {
                    strncpy(prop_name, tok, sizeof(prop_name) - 1);
                    prop_name[sizeof(prop_name) - 1] = 0;
                }
            }

            strncpy(s->transitions[ti].prop, prop_name, sizeof(s->transitions[ti].prop) - 1);
            s->transitions[ti].duration = dur;
            s->transitions[ti].delay = delay;
            s->transitions[ti].timing = timing;
            memcpy(s->transitions[ti].bezier, bez, sizeof(bez));
            if (ti == 0)
            {
                s->transition_duration = dur;
                s->transition_timing = timing;
                memcpy(s->transition_bezier, bez, sizeof(bez));
                strncpy(s->transition_prop, prop_name, sizeof(s->transition_prop) - 1);
            }
            ti++;
        }
        s->num_transitions = ti;
    }
    else if (!strcmp(prop, "transition-timing-function"))
    {
        s->has_transition = 1;
        int timing = 1;
        float bez[4] = {0.25f, 0.1f, 0.25f, 1.0f};
        if (strstr(v, "cubic-bezier"))
        {
            const char *bp = strchr(v, '(');
            if (bp)
            {
                float x1 = 0, y1 = 0, x2 = 1, y2 = 1;
                sscanf(bp + 1, "%f,%f,%f,%f", &x1, &y1, &x2, &y2);
                bez[0] = x1;
                bez[1] = y1;
                bez[2] = x2;
                bez[3] = y2;
                timing = 5;
            }
        }
        else if (!strcmp(v, "linear"))
            timing = 0;
        else if (!strcmp(v, "ease"))
            timing = 1;
        else if (!strcmp(v, "ease-in"))
            timing = 2;
        else if (!strcmp(v, "ease-out"))
            timing = 3;
        else if (!strcmp(v, "ease-in-out"))
            timing = 4;
        s->transition_timing = timing;
        memcpy(s->transition_bezier, bez, sizeof(bez));
        if (s->num_transitions > 0)
        {
            s->transitions[0].timing = timing;
            memcpy(s->transitions[0].bezier, bez, sizeof(bez));
        }
    }
    else if (!strcmp(prop, "transition-delay"))
    {
        s->has_transition = 1;
        float dl = (float)atof(v);
        if (strstr(v, "ms"))
            dl /= 1000.0f;
        if (s->num_transitions > 0)
            s->transitions[0].delay = dl;
    }
    else if (!strcmp(prop, "text-shadow"))
    {
        char buf[128];
        strncpy(buf, v, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        float x = 0, y = 0, bl = 0;
        int nums = 0, hc = 0;
        char col[64] = "rgba(0,0,0,0.5)";
        char *p = buf;
        while (*p)
        {
            while (*p && isspace((unsigned char)*p))
                p++;
            if (!*p)
                break;
            if (*p == '#' || *p == 'r' || *p == 'R' || *p == 'h' || *p == 'H' || *p == 'v' || *p == 'V')
            {
                char *c = col;
                int ci = 0, d = 0;
                while (*p && ci < (int)sizeof(col) - 1)
                {
                    if (*p == '(')
                        d++;
                    if (*p == ')' && d > 0)
                    {
                        *c++ = *p++;
                        d--;
                        continue;
                    }
                    if (d == 0 && isspace((unsigned char)*p))
                        break;
                    *c++ = *p++;
                    ci++;
                }
                *c = 0;
                if (col[0])
                    hc = 1;
                continue;
            }
            char *e;
            float val_n = (float)strtod(p, &e);
            if (e == p)
            {
                p++;
                continue;
            }
            while (*e == 'p' || *e == 'x')
                e++;
            if (nums == 0)
                x = val_n;
            else if (nums == 1)
                y = val_n;
            else
                bl = val_n;
            nums++;
            p = e;
        }
        if (nums >= 2)
        {
            s->has_text_shadow = 1;
            s->ts_x = x;
            s->ts_y = y;
            s->ts_blur = (nums >= 3) ? bl : 0.0f;
            s->ts_r = 0;
            s->ts_g = 0;
            s->ts_b = 0;
            s->ts_a = 1.0f;
            if (hc)
                mini_parse_color(col, &s->ts_r, &s->ts_g, &s->ts_b, &s->ts_a);
        }
    }
    else if (!strcmp(prop, "opacity"))
    {
        float old_op = s->has_opacity ? s->opacity : 1.0f;
        s->opacity = to_px(v);
        if (s->opacity < 0.0f)
            s->opacity = 0.0f;
        if (s->opacity > 1.0f)
            s->opacity = 1.0f;
        s->has_opacity = 1;

        float dur = 0.3f;
        int timing = 0;
        float bez[4] = {0};
        if (n->has_base_style && find_transition_config(s, "opacity", &dur, &timing, bez))
        {
            float cur_val = old_op, tgt_val = s->opacity;
            mini_add_transition_val(n, "opacity", &cur_val, &tgt_val, 1, dur, timing, bez);
            s->opacity = old_op;
        }
    }
    else if (!strcmp(prop, "cursor"))
    {
        if (!strcmp(v, "pointer"))
            s->cursor = 1;
        else if (!strcmp(v, "text"))
            s->cursor = 2;
        else if (!strcmp(v, "grab"))
            s->cursor = 3;
        else if (!strcmp(v, "grabbing"))
            s->cursor = 4;
        else if (!strcmp(v, "not-allowed"))
            s->cursor = 5;
        else if (!strcmp(v, "crosshair"))
            s->cursor = 6;
        else if (!strcmp(v, "ew-resize") || !strcmp(v, "col-resize"))
            s->cursor = 7;
        else if (!strcmp(v, "ns-resize") || !strcmp(v, "row-resize"))
            s->cursor = 8;
        else
            s->cursor = 0;
    }
    else if (!strcmp(prop, "user-select") || !strcmp(prop, "-webkit-user-select") || !strcmp(prop, "-moz-user-select"))
    {
        if (!strcmp(v, "none"))
            s->user_select = 1;
        else if (!strcmp(v, "text"))
            s->user_select = 2;
        else if (!strcmp(v, "all"))
            s->user_select = 3;
        else
            s->user_select = 0;
    }
    else if (!strcmp(prop, "pointer-events"))
    {
        s->pointer_events = !strcmp(v, "none") ? 1 : 0;
    }
    else if (!strcmp(prop, "backdrop-filter") || !strcmp(prop, "-webkit-backdrop-filter") || !strcmp(prop, "mix-blend-mode"))
    {
        s->has_backdrop_filter = 1;
        const char *b = strstr(v, "blur(");
        if (b)
            s->backdrop_blur = to_px(b + 5);

        const char *inv = strstr(v, "invert(");
        if (inv)
        {
            float val = (float)atof(inv + 7);
            s->filter_invert = strchr(inv, '%') ? val / 100.0f : val;
        }

        const char *hue = strstr(v, "hue-rotate(");
        if (hue)
        {
            s->filter_invert = 0.5f; /* 近似模拟 hue-rotate 为色彩翻转滤镜 */
        }
    }
    else if (!strcmp(prop, "filter") || !strcmp(prop, "-webkit-filter"))
    {
        s->has_filter = 1;
        const char *b = strstr(v, "blur(");
        if (b)
            s->filter_blur = to_px(b + 5);
        const char *inv = strstr(v, "invert(");
        if (inv)
        {
            float val = (float)atof(inv + 7);
            if (strchr(inv, '%'))
                val /= 100.0f;
            s->filter_invert = val;
        }
        const char *gs = strstr(v, "grayscale(");
        if (gs)
        {
            float val = (float)atof(gs + 10);
            if (strchr(gs, '%'))
                val /= 100.0f;
            s->filter_grayscale = val;
        }
        const char *br = strstr(v, "brightness(");
        if (br)
        {
            float val = (float)atof(br + 11);
            if (strchr(br, '%'))
                val /= 100.0f;
            s->filter_brightness = val;
        }
    }
    else if (!strcmp(prop, "background-clip") || !strcmp(prop, "-webkit-background-clip"))
    {
        if (strstr(v, "text"))
            s->text_gradient = 1;
    }
    else if (!strcmp(prop, "animation") || !strcmp(prop, "animation-name") || !strcmp(prop, "animation-duration") || !strcmp(prop, "animation-timing-function") || !strcmp(prop, "animation-iteration-count") || !strcmp(prop, "animation-direction") || !strcmp(prop, "animation-fill-mode") || !strcmp(prop, "animation-play-state"))
    {
        s->has_animation = 1;
        /* (re)setting an animation resets the tick memo so apply_animations
           re-evaluates from scratch (a renamed/re-timed animation should not
           inherit a stale "completed" / "last eased" state). */
        s->anim_last_eased = -1.0f;
        s->anim_completed = 0;
        if (!strcmp(prop, "animation"))
        {
            char name[64] = {0};
            float dur = 1.0f, delay = 0.0f;
            int iter = 1, dir = 0, timing = 0, fill = 0, play = 0;
            float bez[4] = {0.25f, 0.1f, 0.25f, 1.0f};

            char tmp[256];
            strncpy(tmp, v, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = 0;
            char *save;
            char *tok = strtok_r(tmp, " ", &save);
            int has_dur = 0;
            while (tok)
            {
                if (strstr(tok, "cubic-bezier"))
                {
                    char *b_st = strchr(tok, '(');
                    if (b_st)
                    {
                        float x1 = 0, y1 = 0, x2 = 1, y2 = 1;
                        sscanf(b_st + 1, "%f,%f,%f,%f", &x1, &y1, &x2, &y2);
                        bez[0] = x1;
                        bez[1] = y1;
                        bez[2] = x2;
                        bez[3] = y2;
                        timing = 5;
                    }
                }
                else if (!strcmp(tok, "linear"))
                {
                    timing = 0;
                }
                else if (!strcmp(tok, "ease"))
                {
                    timing = 1;
                }
                else if (!strcmp(tok, "ease-in"))
                {
                    timing = 2;
                }
                else if (!strcmp(tok, "ease-out"))
                {
                    timing = 3;
                }
                else if (!strcmp(tok, "ease-in-out"))
                {
                    timing = 4;
                }
                else if (!strcmp(tok, "infinite"))
                {
                    iter = -1;
                }
                else if (!strcmp(tok, "alternate"))
                {
                    dir = 2;
                }
                else if (!strcmp(tok, "alternate-reverse"))
                {
                    dir = 3;
                }
                else if (!strcmp(tok, "reverse"))
                {
                    dir = 1;
                }
                else if (!strcmp(tok, "normal"))
                {
                    dir = 0;
                }
                else if (!strcmp(tok, "forwards"))
                {
                    fill = 1;
                }
                else if (!strcmp(tok, "backwards"))
                {
                    fill = 2;
                }
                else if (!strcmp(tok, "both"))
                {
                    fill = 3;
                }
                else if (!strcmp(tok, "running"))
                {
                    play = 0;
                }
                else if (!strcmp(tok, "paused"))
                {
                    play = 1;
                }
                else if (isdigit((unsigned char)tok[0]) || (tok[0] == '.' && isdigit((unsigned char)tok[1])))
                {
                    float n_val = (float)atof(tok);
                    if (strstr(tok, "s") || strstr(tok, "ms"))
                    {
                        if (strstr(tok, "ms"))
                            n_val /= 1000.0f;
                        if (!has_dur)
                        {
                            dur = n_val;
                            has_dur = 1;
                        }
                        else
                        {
                            delay = n_val;
                        }
                    }
                    else
                    {
                        iter = (int)n_val;
                    }
                }
                else if (tok[0] && !name[0])
                {
                    strncpy(name, tok, sizeof(name) - 1);
                    name[sizeof(name) - 1] = 0;
                }
                tok = strtok_r(NULL, " ", &save);
            }

            strncpy(s->anim_name, name, sizeof(s->anim_name) - 1);
            s->anim_duration = dur;
            s->anim_delay = delay;
            s->anim_timing = timing;
            memcpy(s->anim_bezier, bez, sizeof(bez));
            s->anim_iteration_count = iter;
            s->anim_direction = dir;
            s->anim_fill_mode = fill;
            s->anim_play_state = play;

            if (name[0])
                mini_node_set_attribute(n, "data-anim-name", name);
            char db[32];
            snprintf(db, sizeof(db), "%g", dur);
            mini_node_set_attribute(n, "data-anim-dur", db);
            if (dir == 2)
                mini_node_set_attribute(n, "data-anim-dir", "alternate");
        }
        else if (!strcmp(prop, "animation-name"))
        {
            strncpy(s->anim_name, v, sizeof(s->anim_name) - 1);
            mini_node_set_attribute(n, "data-anim-name", v);
        }
        else if (!strcmp(prop, "animation-duration"))
        {
            float dur = (float)atof(v);
            if (strstr(v, "ms"))
                dur /= 1000.0f;
            s->anim_duration = dur;
            char db[32];
            snprintf(db, sizeof(db), "%g", dur);
            mini_node_set_attribute(n, "data-anim-dur", db);
        }
        else if (!strcmp(prop, "animation-direction"))
        {
            if (!strcmp(v, "alternate"))
            {
                s->anim_direction = 2;
                mini_node_set_attribute(n, "data-anim-dir", "alternate");
            }
            else if (!strcmp(v, "reverse"))
                s->anim_direction = 1;
            else
                s->anim_direction = 0;
        }
        else if (!strcmp(prop, "animation-iteration-count"))
        {
            if (!strcmp(v, "infinite"))
                s->anim_iteration_count = -1;
            else
                s->anim_iteration_count = atoi(v);
        }
        else if (!strcmp(prop, "animation-timing-function"))
        {
            if (strstr(v, "cubic-bezier"))
            {
                const char *bp = strchr(v, '(');
                if (bp)
                {
                    float x1 = 0, y1 = 0, x2 = 1, y2 = 1;
                    sscanf(bp + 1, "%f,%f,%f,%f", &x1, &y1, &x2, &y2);
                    s->anim_bezier[0] = x1;
                    s->anim_bezier[1] = y1;
                    s->anim_bezier[2] = x2;
                    s->anim_bezier[3] = y2;
                    s->anim_timing = 5;
                }
            }
            else if (!strcmp(v, "linear"))
                s->anim_timing = 0;
            else if (!strcmp(v, "ease"))
                s->anim_timing = 1;
            else if (!strcmp(v, "ease-in"))
                s->anim_timing = 2;
            else if (!strcmp(v, "ease-out"))
                s->anim_timing = 3;
            else if (!strcmp(v, "ease-in-out"))
                s->anim_timing = 4;
        }
        else if (!strcmp(prop, "animation-fill-mode"))
        {
            if (!strcmp(v, "forwards"))
                s->anim_fill_mode = 1;
            else if (!strcmp(v, "backwards"))
                s->anim_fill_mode = 2;
            else if (!strcmp(v, "both"))
                s->anim_fill_mode = 3;
            else
                s->anim_fill_mode = 0;
        }
        else if (!strcmp(prop, "animation-play-state"))
        {
            s->anim_play_state = !strcmp(v, "paused") ? 1 : 0;
        }
    }
    else if (!strcmp(prop, "float"))
    {
        if (!strcmp(v, "left"))
            mini_node_set_attribute(n, "data-float", "left");
        else if (!strcmp(v, "right"))
            mini_node_set_attribute(n, "data-float", "right");
        else
            mini_node_remove_attribute(n, "data-float");
    }
    else if (!strcmp(prop, "clear"))
    {
        mini_node_set_attribute(n, "data-clear", v);
    }
    else if (!strcmp(prop, "column-count") || !strcmp(prop, "columns"))
    {
        mini_node_set_attribute(n, "data-column-count", v);
    }
    else if (!strcmp(prop, "column-width"))
    {
        mini_node_set_attribute(n, "data-column-width", v);
    }

    free(res_val);
}
void mini_style_set_base(struct MiniNode *n, const char *prop, const char *val)
{
    if (!n)
        return;
    mini_style_set(n, prop, val);
    n->base_style = n->style;
    n->has_base_style = 1;
}

/* Concatenate all descendant text-node data into buf (textContent). */
static void collect_text_content(const struct MiniNode *n, char *buf,
                                 size_t cap, size_t *o)
{
    if (!n)
        return;
    if (n->type == MN_TEXT_NODE && n->text)
    {
        size_t l = strlen(n->text);
        for (size_t i = 0; i < l && *o + 1 < cap; i++)
            buf[(*o)++] = n->text[i];
    }
    else
    {
        for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
            collect_text_content(c, buf, cap, o);
    }
}

/* ------------------------------------------------------------------ */
/* Inline-flow helpers (used by the block-flow pass below).            */
/* text_wrap_lines: word-wrap a string to `max_w` using the renderer's */
/* text metric so a text node reserves the right number of line boxes. */
/* slot_host: walk up from a <slot> to the element whose shadow tree   */
/* contains it (so the slot can lay out / render the host's light kids).*/
/* ------------------------------------------------------------------ */
static int text_wrap_lines(const char *text, float font_size, float max_w, float ls)
{
    if (!text || !*text || max_w <= 0.0f)
        return 1;

    /* 架构级修复：测距仪注入 letter-spacing (ls) 参数 */
    float total_w = mini_text_measure_ex(text, font_size, ls);
    if (total_w <= max_w + 4.0f)
        return 1;

    int lines = 1;
    float line_w = 0.0f;
    const char *p = text;

    while (*p)
    {
        if (*p == '\n' || *p == '\r')
        {
            if (*p == '\r' && p[1] == '\n')
                p++;
            lines++;
            line_w = 0.0f;
            p++;
            continue;
        }

        if (isspace((unsigned char)*p))
        {
            /* 空格测距也必须带上字间距 */
            float sp_w = mini_text_measure_ex(" ", font_size, ls);
            if (line_w + sp_w > max_w + 2.0f && line_w > 0.0f)
            {
                lines++;
                line_w = 0.0f;
            }
            else
            {
                line_w += sp_w;
            }
            p++;
            continue;
        }

        const char *word_start = p;
        int word_bytes = 0;
        unsigned char c0 = (unsigned char)*p;

        if (c0 >= 0x80)
        {
            int clen = 1;
            if (c0 >= 0xF0)
                clen = 4;
            else if (c0 >= 0xE0)
                clen = 3;
            else if (c0 >= 0xC0)
                clen = 2;

            /* 致命修复：防止 UTF8 字节数越过 \0 导致内存越界死循环 */
            for (int i = 0; i < clen && *p; i++)
            {
                word_bytes++;
                p++;
            }
        }
        else
        {
            while (*p && !isspace((unsigned char)*p) && (unsigned char)*p < 0x80)
            {
                word_bytes++;
                p++;
            }
            if (word_bytes == 0)
            {
                p++;
                continue;
            }
        }

        char word_buf[128];
        if (word_bytes >= (int)sizeof(word_buf))
            word_bytes = sizeof(word_buf) - 1;
        memcpy(word_buf, word_start, word_bytes);
        word_buf[word_bytes] = '\0';

        /* 单词测距必须带上字间距 */
        float word_w = mini_text_measure_ex(word_buf, font_size, ls);
        if (word_w > max_w + 2.0f && word_bytes > 1)
        {
            const char *wp = word_start;
            const char *w_end = word_start + word_bytes;
            while (wp < w_end)
            {
                unsigned char uc = (unsigned char)*wp;
                int ulen = 1;
                if (uc >= 0xF0) ulen = 4;
                else if (uc >= 0xE0) ulen = 3;
                else if (uc >= 0xC0) ulen = 2;
                if (wp + ulen > w_end) ulen = (int)(w_end - wp);

                char char_buf[8] = {0};
                memcpy(char_buf, wp, ulen);
                char_buf[ulen] = '\0';
                float char_w = mini_text_measure_ex(char_buf, font_size, ls);

                if (line_w > 0.0f && line_w + char_w > max_w + 2.0f)
                {
                    lines++;
                    line_w = char_w;
                }
                else
                {
                    line_w += char_w;
                }
                wp += ulen;
            }
            continue;
        }
        if (line_w > 0.0f && line_w + word_w > max_w + 2.0f)
        {
            lines++;
            line_w = word_w;
        }
        else
        {
            line_w += word_w;
        }
    }
    return lines;
}

static struct MiniNode *slot_host(struct MiniNode *slot)
{
    if (!slot)
        return NULL;
    for (struct MiniNode *n = slot->parent; n; n = n->parent)
    {
        if (n->tag && !strcmp(n->tag, "#shadow-root"))
            return n->parent;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Layout: block + flexbox. Writes geometry into style.abs_x/abs_y/w/h.*/
/* ------------------------------------------------------------------ */
/* Resolve a raw CSS length to px against the layout context.
   pct_base = percentage basis (parent content width for w/margin/padding,
   or parent font-size for a font-size %). font = the element's own
   font-size (for em). vw/vh/vmin/vmax use the viewport.                */
static float resolve_len(MiniLength L, float pct_base, float font,
                         float root_font, float vw, float vh)
{
    switch (L.unit)
    {
    case 1:
        return L.v / 100.0f * pct_base; /* % */
    case 2:
        return L.v * font; /* em */
    case 3:
        return L.v * root_font; /* rem */
    case 4:
        return L.v / 100.0f * vw; /* vw */
    case 5:
        return L.v / 100.0f * vh; /* vh */
    case 6:
    {
        float m = vw < vh ? vw : vh;
        return L.v / 100.0f * m;
    } /* vmin */
    case 7:
    {
        float m = vw > vh ? vw : vh;
        return L.v / 100.0f * m;
    } /* vmax */
    case 10:
        return L.v * (font > 0.0f ? font : 16.0f) * 0.55f; /* ch unit (~0.55em) */
    default:
        return L.v; /* px / unitless */
    }
}

/* resolve a length that MAY be a deferred calc() (unit==9). Routes to the
   node's calc store so calc(100% - 20px) resolves with the layout context. */
static float resolve_field(struct MiniNode *n, uint8_t field, MiniLength L,
                           float pct_base, float font, float root_font,
                           float vw, float vh)
{
    if (L.unit == 9 && n)
        return mini_calc_resolve(n, field, pct_base, font, root_font, vw, vh);
    return resolve_len(L, pct_base, font, root_font, vw, vh);
}

static void layout_node(struct MiniNode *n, float x, float y,
                        float avail_w, float avail_h);

/* ================================================================== */
/* Phase 3: advanced layout — CSS Grid, Float + BFC clear, Multicol.    */
/* Each is a real algorithm (not an emulation), wired into layout_node's */
/* dispatch by display/property.                                       */
/* ================================================================== */

/* ---- CSS Grid: track sizing + auto-flow placement ----
   Parses grid-template-columns into a track list (fr / px / % / auto /
   minmax(a,b) / repeat(N|auto-fill|auto-fit, X)), resolves bases, distributes
   leftover space by fr factor, and places in-flow children row-major with
   optional column spans (grid-column: span N). Named lines are honored by
   numeric line indexing (grid-column: 2 / 4) in the common form.        */
typedef struct
{
    float base, fr, mn, mx;
    int has_minmax, is_auto;
} GridTrack;
static float grid_parse_len(const char *s, float avail, float font)
{
    /* a bare length token like "100px"/"50%"/"2em"/"1fr" → px (fr→0). */
    char *e;
    float v = (float)strtod(s, &e);
    if (e == s)
        return 0;
    if (*e == '%')
        return v * avail / 100.0f;
    if (e[0] == 'e' && e[1] == 'm')
        return v * font;
    if (e[0] == 'r' && e[1] == 'e' && e[2] == 'm')
        return v * g_lctx.root_font;
    if (e[0] == 'v' && e[1] == 'w')
        return v * g_lctx.vw / 100.0f;
    if (e[0] == 'v' && e[1] == 'h')
        return v * g_lctx.vh / 100.0f;
    return v; /* px / unitless */
}
/* parse one track token (up to next whitespace/comma at depth 0) into t. */
static const char *grid_parse_track(const char *p, GridTrack *t, float avail, float font)
{
    while (*p && (isspace((unsigned char)*p) || *p == ','))
        p++;
    if (!*p)
        return NULL;
    char tok[64];
    int i = 0, depth = 0;
    while (*p && !(depth == 0 && (isspace((unsigned char)*p) || *p == ',')) && i < (int)sizeof(tok) - 1)
    {
        if (*p == '(')
            depth++;
        else if (*p == ')' && depth > 0)
            depth--;
        tok[i++] = *p++;
    }
    tok[i] = 0;
    memset(t, 0, sizeof(*t));
    if (!strncmp(tok, "minmax(", 7))
    {
        t->has_minmax = 1;
        const char *q = tok + 7;
        char a[32], b[32];
        int ai = 0, bi = 0, inc = 0;
        while (*q && *q != ')')
        {
            if (*q == ',')
            {
                inc = 1;
                q++;
                while (*q && isspace((unsigned char)*q))
                    q++;
                continue;
            }
            if (!inc)
                a[ai++] = *q++;
            else
                b[bi++] = *q++;
        }
        a[ai] = b[bi] = 0;
        /* min → base, max → mx (fr in min/max is treated as 0 base, grows).
           The fr factor is taken from whichever of min/max is an fr value
           (minmax(100px, 1fr) → base 100, fr 1, grows to fill).               */
        int a_fr = a[0] && strstr(a, "fr");
        int b_fr = b[0] && strstr(b, "fr");
        t->mn = a_fr ? 0 : grid_parse_len(a, avail, font);
        t->mx = b_fr ? 1e9f : grid_parse_len(b, avail, font);
        if (a_fr)
            t->fr = (float)atof(a);
        else if (b_fr)
            t->fr = (float)atof(b);
        t->base = a_fr ? 0 : t->mn;
    }
    else if (strstr(tok, "fr"))
    {
        t->fr = (float)atof(tok);
        t->base = 0;
    }
    else if (!strcmp(tok, "auto"))
    {
        t->is_auto = 1;
        t->base = 0;
    }
    else if (!strncmp(tok, "min-content", 11) || !strncmp(tok, "max-content", 11))
    {
        t->is_auto = 1;
        t->base = 0;
    }
    else
        t->base = grid_parse_len(tok, avail, font);
    return p;
}

static void shift_subtree(struct MiniNode *n, float dx, float dy)
{
    if (!n || (dx == 0.0f && dy == 0.0f))
        return;
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
    {
        c->style.abs_x += dx;
        c->style.abs_y += dy;
        shift_subtree(c, dx, dy);
    }
}

static void flush_inline_line_align(struct MiniNode **kids, int count, float content_left, float pen_x, float content_w, int text_align)
{
    if (count <= 0 || text_align == 0)
        return;
    float line_w = pen_x - content_left;
    float off = 0.0f;
    if (text_align == 1) /* center */
        off = (content_w - line_w) * 0.5f;
    else if (text_align == 2) /* right */
        off = content_w - line_w;
    if (off > 0.0f)
    {
        for (int i = 0; i < count; i++)
        {
            kids[i]->style.abs_x += off;
            shift_subtree(kids[i], off, 0.0f);
        }
    }
}

static void layout_grid(struct MiniNode *n, float x, float y, float avail_w, float avail_h, float gap_px)
{
    MiniStyle *s = &n->style;
    s->abs_x = x + s->margin[3];
    s->abs_y = y + s->margin[0];

    float cw = (s->len_w.v > 0.0f || s->len_w.unit != 0) ? s->w : (avail_w - s->margin[1] - s->margin[3]);
    if (cw <= 0.0f)
        cw = (avail_w > 0.0f) ? avail_w : 900.0f;
    s->w = cw;

    float content_left = s->abs_x + s->padding[3];
    float content_top = s->abs_y + s->padding[0];
    float content_w = cw - s->padding[1] - s->padding[3];
    if (content_w <= 0.0f)
        content_w = cw;

    int nkids = 0;
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
    {
        if (c->type == MN_ELEMENT_NODE && c->style.display != MINI_DISPLAY_NONE &&
            c->style.position != 2 && c->style.position != 3)
            nkids++;
    }

    if (nkids == 0)
    {
        s->h = 0;
        s->laid_out = 1;
        return;
    }

    float actual_gap = (gap_px > 0.0f) ? gap_px : 0.0f;
    int ncols = 1;
    const char *gtc = s->grid_gtc[0] ? s->grid_gtc : mini_node_get_attribute(n, "data-gtc");
    if (gtc && (strstr(gtc, "auto-fit") || strstr(gtc, "auto-fill")))
    {
        float min_len = 250.0f;
        const char *mm = strstr(gtc, "minmax(");
        if (mm)
        {
            float parsed = grid_parse_len(mm + 7, content_w, s->font_size > 0 ? s->font_size : 16.0f);
            if (parsed > 0.0f)
                min_len = parsed;
        }
        ncols = (int)floorf((content_w + actual_gap) / (min_len + actual_gap));
        if (ncols < 1)
            ncols = 1;
    }
    else if (s->grid_cols > 0)
    {
        ncols = s->grid_cols;
    }
    else
    {
        ncols = (nkids >= 3) ? 3 : (nkids > 0 ? nkids : 1);
    }
    if (ncols < 1)
        ncols = 1;

    float total_gap = (ncols > 1) ? (ncols - 1) * actual_gap : 0.0f;
    float *col_widths = (float *)calloc(ncols, sizeof(float));
    float *col_positions = (float *)calloc(ncols, sizeof(float));
    float *fr_weights = (float *)calloc(ncols, sizeof(float));
    float total_fr = 0.0f;
    int parsed_cols = 0;

    if (gtc)
    {
        const char *gp = gtc;
        int in_repeat = 0;
        float repeat_val = 1.0f;

        while (*gp && parsed_cols < ncols)
        {
            while (*gp && (isspace((unsigned char)*gp) || *gp == ','))
                gp++;
            if (!*gp)
                break;

            if (!strncmp(gp, "repeat(", 7))
            {
                int rep_n = atoi(gp + 7);
                if (rep_n < 1)
                    rep_n = 1;
                const char *comma = strchr(gp, ',');
                const char *track_expr = comma ? comma + 1 : gp + 7;
                while (*track_expr && isspace((unsigned char)*track_expr))
                    track_expr++;

                GridTrack gt;
                grid_parse_track(track_expr, &gt, content_w, s->font_size > 0 ? s->font_size : 16.0f);
                float tr_fr = (gt.fr > 0.0f) ? gt.fr : 1.0f;

                for (int ri = 0; ri < rep_n && parsed_cols < ncols; ri++)
                {
                    fr_weights[parsed_cols] = tr_fr;
                    total_fr += tr_fr;
                    parsed_cols++;
                }

                int rdepth = 0;
                while (*gp)
                {
                    if (*gp == '(')
                        rdepth++;
                    else if (*gp == ')')
                    {
                        rdepth--;
                        if (rdepth == 0)
                        {
                            gp++;
                            break;
                        }
                    }
                    gp++;
                }
                continue;
            }

            const char *end_tok = gp;
            while (*end_tok && !isspace((unsigned char)*end_tok) && *end_tok != ',')
                end_tok++;
            char tok[32] = {0};
            size_t tlen = end_tok - gp;
            if (tlen >= sizeof(tok))
                tlen = sizeof(tok) - 1;
            strncpy(tok, gp, tlen);

            float v = (float)atof(tok);
            if (strstr(tok, "fr"))
            {
                fr_weights[parsed_cols] = (v > 0.0f) ? v : 1.0f;
                total_fr += fr_weights[parsed_cols];
            }
            else
            {
                fr_weights[parsed_cols] = 1.0f;
                total_fr += 1.0f;
            }
            gp = end_tok;
            parsed_cols++;
        }
    }

    if (total_fr <= 0.0f || parsed_cols < ncols)
    {
        for (int i = 0; i < ncols; i++)
            col_widths[i] = (content_w - total_gap) / (float)ncols;
    }
    else
    {
        float net_w = content_w - total_gap;
        if (net_w < 0.0f)
            net_w = 0.0f;
        for (int i = 0; i < ncols; i++)
            col_widths[i] = net_w * (fr_weights[i] / total_fr);
    }

    float cur_x_off = content_left;
    for (int i = 0; i < ncols; i++)
    {
        col_positions[i] = cur_x_off;
        cur_x_off += col_widths[i] + actual_gap;
    }

    int max_rows = 512;
    uint8_t *occupied = (uint8_t *)calloc(max_rows * ncols, sizeof(uint8_t));
    float *row_h_arr = (float *)calloc(max_rows, sizeof(float));

    /* Parse grid-template-rows if set */
    if (s->grid_gtr[0])
    {
        const char *rp = s->grid_gtr;
        int r_idx = 0;
        while (*rp && r_idx < max_rows)
        {
            while (*rp && isspace((unsigned char)*rp))
                rp++;
            if (!*rp)
                break;
            const char *rend = rp;
            while (*rend && !isspace((unsigned char)*rend))
                rend++;
            char rtok[32] = {0};
            size_t rtl = (size_t)(rend - rp);
            if (rtl >= sizeof(rtok))
                rtl = sizeof(rtok) - 1;
            strncpy(rtok, rp, rtl);
            float rv = (float)atof(rtok);
            if (rv > 0.0f && !strstr(rtok, "fr"))
                row_h_arr[r_idx] = rv;
            r_idx++;
            rp = rend;
        }
    }

    struct MiniNode **grid_items = (struct MiniNode **)malloc(nkids * sizeof(struct MiniNode *));
    int *item_rows = (int *)malloc(nkids * sizeof(int));
    int *item_cols = (int *)malloc(nkids * sizeof(int));
    int *item_row_spans = (int *)malloc(nkids * sizeof(int));
    int *item_col_spans = (int *)malloc(nkids * sizeof(int));
    int num_items = 0;

    int current_row = 0;
    int current_col = 0;

    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
    {
        if (c->type != MN_ELEMENT_NODE || c->style.display == MINI_DISPLAY_NONE ||
            c->style.position == 2 || c->style.position == 3)
            continue;

        int span_c = c->style.grid_col_span > 1 ? c->style.grid_col_span : 1;
        if (span_c > ncols)
            span_c = ncols;
        int span_r = c->style.grid_row_span > 1 ? c->style.grid_row_span : 1;
        if (span_r > max_rows)
            span_r = max_rows;

        /* If dense flow is requested, start searching from top-left */
        if (s->grid_auto_flow_dense)
        {
            current_row = 0;
            current_col = 0;
        }

        while (1)
        {
            if (current_col + span_c > ncols)
            {
                current_col = 0;
                current_row++;
                if (current_row >= max_rows)
                    break;
            }
            if (current_row >= max_rows)
                break;

            int fits = 1;
            for (int r_chk = 0; r_chk < span_r; r_chk++)
            {
                if (current_row + r_chk >= max_rows)
                {
                    fits = 0;
                    break;
                }
                for (int c_chk = 0; c_chk < span_c; c_chk++)
                {
                    if (occupied[(current_row + r_chk) * ncols + (current_col + c_chk)])
                    {
                        fits = 0;
                        break;
                    }
                }
                if (!fits)
                    break;
            }
            if (fits)
                break;
            current_col++;
        }

        if (current_row >= max_rows)
            break;

        for (int r = 0; r < span_r; r++)
        {
            if (current_row + r >= max_rows)
                continue;
            for (int c_idx = 0; c_idx < span_c; c_idx++)
            {
                occupied[(current_row + r) * ncols + (current_col + c_idx)] = 1;
            }
        }

        if (num_items < nkids)
        {
            grid_items[num_items] = c;
            item_rows[num_items] = current_row;
            item_cols[num_items] = current_col;
            item_row_spans[num_items] = span_r;
            item_col_spans[num_items] = span_c;
            num_items++;
        }
        if (!s->grid_auto_flow_dense)
            current_col += span_c;
    }

    /* 阶段一：测量所有块的天然高度以撑开轨道行高 */
    for (int i = 0; i < num_items; i++)
    {
        struct MiniNode *c = grid_items[i];
        int r = item_rows[i];
        int c_col = item_cols[i];
        int sr = item_row_spans[i];
        int sc = item_col_spans[i];

        float item_w = col_widths[c_col];
        for (int si = 1; si < sc; si++)
            item_w += actual_gap + col_widths[c_col + si];

        c->style.laid_out = 0;
        layout_node(c, 0, 0, item_w, 0);
        c->style.w = item_w;

        float chh = c->style.h + c->style.margin[0] + c->style.margin[2];
        float h_per_row = (chh - (sr - 1) * actual_gap) / sr;
        if (h_per_row < 0)
            h_per_row = 0;
        for (int k = 0; k < sr; k++)
        {
            if (r + k < max_rows && h_per_row > row_h_arr[r + k])
                row_h_arr[r + k] = h_per_row;
        }
    }

    /* 阶段二：根据确定的轨道高度进行网格填入拉伸（Bento盒子） */
    for (int i = 0; i < num_items; i++)
    {
        struct MiniNode *c = grid_items[i];
        int r = item_rows[i];
        int c_col = item_cols[i];
        int sr = item_row_spans[i];
        int sc = item_col_spans[i];

        float item_w = col_widths[c_col];
        for (int si = 1; si < sc; si++)
            item_w += actual_gap + col_widths[c_col + si];

        float place_y = content_top;
        for (int k = 0; k < r; k++)
        {
            place_y += row_h_arr[k] + actual_gap;
        }
        float cell_x = col_positions[c_col];

        float cell_h = 0;
        for (int k = 0; k < sr; k++)
        {
            if (r + k < max_rows)
                cell_h += row_h_arr[r + k];
        }
        cell_h += (sr - 1) * actual_gap;

        float target_h = cell_h - c->style.margin[0] - c->style.margin[2];
        MiniLength old_len = c->style.len_h;
        if (c->style.h < target_h)
        {
            c->style.len_h.v = target_h;
            c->style.len_h.unit = 0;
        }

        c->style.laid_out = 0;
        layout_node(c, cell_x, place_y, item_w, target_h);
        if (c->style.h < target_h)
            c->style.h = target_h;

        c->style.len_h = old_len;
    }

    /* 计算容器总高度 */
    float max_y = content_top;
    int any_row = 0;
    for (int r = 0; r < max_rows; r++)
    {
        int row_is_used = 0;
        for (int c = 0; c < ncols; c++)
        {
            if (occupied[r * ncols + c])
            {
                row_is_used = 1;
                break;
            }
        }
        if (!row_is_used && row_h_arr[r] == 0.0f)
            break;
        if (any_row)
            max_y += actual_gap;
        max_y += row_h_arr[r];
        any_row = 1;
    }

    float total_h = (max_y - s->abs_y) + s->padding[2];
    s->h = (s->len_h.v > 0.0f || s->len_h.unit != 0) ? s->h : total_h;
    s->laid_out = 1;

    free(col_widths);
    free(col_positions);
    free(fr_weights);
    free(occupied);
    free(row_h_arr);
    free(grid_items);
    free(item_rows);
    free(item_cols);
    free(item_row_spans);
    free(item_col_spans);
}
/* ---- Float + BFC clear ----
   A block-level float model: left/right floats intrude into the content box
   of subsequent in-flow block siblings (reducing their width at the float's
   vertical extent); `clear` drops the cursor below the relevant floats. A
   block formatting context (overflow:hidden, float, inline-block, flow-root,
   position:absolute/fixed) contains its floats so they don't escape.       */
typedef struct
{
    float x, w, top, bottom;
    int left;
} MiniFloat;
static int is_all_ws(const char *s)
{
    if (!s)
        return 1;
    while (*s)
    {
        if (!isspace((unsigned char)*s))
            return 0;
        s++;
    }
    return 1;
}
static float collapse_margins(float m1, float m2)
{
    if (m1 >= 0.0f && m2 >= 0.0f)
        return (m1 > m2) ? m1 : m2;
    if (m1 <= 0.0f && m2 <= 0.0f)
        return (m1 < m2) ? m1 : m2;
    return m1 + m2;
}

static void layout_block_children(struct MiniNode *n, float content_left,
                                  float content_top, float content_w,
                                  float content_h, float *out_bottom)
{
    MiniStyle *s = &n->style;
    MiniFloat fl[64];
    int nfl = 0;
    float y = content_top;
    float prev_m_bottom = 0.0f;
    int has_prev_sibling = 0;

    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
    {
        if (c->style.display == MINI_DISPLAY_NONE)
            continue;
        if (c->style.position == 2 || c->style.position == 3)
            continue; /* abs/fixed */
        /* text node: wrap into the (float-reduced) content width at y. */
        if (c->type == MN_TEXT_NODE)
        {
            if (!c->text || is_all_ws(c->text))
            {
                c->style.h = 0.0f;
                c->style.w = 0.0f;
                c->style.laid_out = 1;
                continue;
            }
            float lo = 0, ro = 0;
            for (int i = 0; i < nfl; i++)
                if (fl[i].bottom > y)
                {
                    if (fl[i].left)
                        lo = (fl[i].x + fl[i].w) > lo ? (fl[i].x + fl[i].w) : lo;
                    else
                        ro = (content_left + content_w - fl[i].x) > ro ? (content_left + content_w - fl[i].x) : ro;
                }
            c->style.font_size = s->font_size > 0 ? s->font_size : 16.0f;
            c->style.color_r = s->color_r;
            c->style.color_g = s->color_g;
            c->style.color_b = s->color_b;
            c->style.color_a = s->color_a;
            float lh = mini_text_line_height(c->style.font_size);
            if (s->line_height_set)
            {
                if (s->len_line_height.unit == 0 && s->len_line_height.v > 0.0f && s->len_line_height.v <= 5.0f)
                    lh = c->style.font_size * s->len_line_height.v;
                else if (s->len_line_height.v > 0.0f)
                    lh = s->len_line_height.v;
            }
            float ls = c->parent ? (c->parent->style.letter_set ? c->parent->style.len_letter.v : 0.0f) : (c->style.letter_set ? c->style.len_letter.v : 0.0f);
            int lines = text_wrap_lines(c->text, c->style.font_size, content_w - lo - ro, ls);
            c->style.abs_x = content_left + lo;
            c->style.abs_y = y;
            c->style.w = content_w - lo - ro;
            c->style.h = (float)lines * lh;
            c->style.laid_out = 1;
            y += c->style.h;
            continue;
        }
        const char *fl_attr = mini_node_get_attribute(c, "data-float");
        int is_float = fl_attr && (fl_attr[0] == 'l' || fl_attr[0] == 'r');
        int side = is_float ? (fl_attr[0] == 'r' ? 0 : 1) : 0; /* 1=left 0=right */
        const char *clr = mini_node_get_attribute(c, "data-clear");
        if (clr)
        {
            float clear_to = y;
            int want_l = strchr(clr, 'l') || strchr(clr, 'b');
            int want_r = strchr(clr, 'r') || strchr(clr, 'b');
            for (int i = 0; i < nfl; i++)
                if ((want_l && fl[i].left) || (want_r && !fl[i].left))
                    if (fl[i].bottom > clear_to)
                        clear_to = fl[i].bottom;
            if (y < clear_to)
                y = clear_to;
            /* drop floats whose bottom is now above the cursor */
            for (int i = 0; i < nfl;)
            {
                if (fl[i].bottom <= y)
                {
                    fl[i] = fl[--nfl];
                }
                else
                    i++;
            }
        }
        if (is_float && nfl < 64)
        {
            /* measure the float at full content width, then place to the side */
            layout_node(c, content_left, y, content_w, 0);
            float fw = c->style.w + c->style.margin[1] + c->style.margin[3];
            float fx = side ? content_left : (content_left + content_w - fw);
            /* if it doesn't fit beside an opposing float, drop below */
            float lo = 0, ro = 0;
            for (int i = 0; i < nfl; i++)
                if (fl[i].bottom > y)
                {
                    if (fl[i].left)
                        lo = fl[i].x + fl[i].w > lo ? fl[i].x + fl[i].w : lo;
                    else
                        ro = (content_left + content_w - fl[i].x) > ro ? (content_left + content_w - fl[i].x) : ro;
                }
            if (fx < content_left + lo || fx + fw > content_left + content_w - ro)
            {
                y = 0;
                for (int i = 0; i < nfl; i++)
                    if (fl[i].bottom > y)
                        y = fl[i].bottom;
                fx = side ? content_left : (content_left + content_w - fw);
            }
            c->style.abs_x = fx + c->style.margin[3];
            c->style.abs_y = y + c->style.margin[0];
            fl[nfl].x = fx;
            fl[nfl].w = fw;
            fl[nfl].top = y;
            fl[nfl].bottom = y + c->style.h + c->style.margin[0] + c->style.margin[2];
            fl[nfl].left = side;
            nfl++;
            continue;
        }
        /* in-flow block child: reduce width by float intrusions at its top */
        float lo = 0, ro = 0;
        for (int i = 0; i < nfl; i++)
            if (fl[i].bottom > y)
            {
                if (fl[i].left)
                    lo = (fl[i].x + fl[i].w) > lo ? (fl[i].x + fl[i].w) : lo;
                else
                    ro = (content_left + content_w - fl[i].x) > ro ? (content_left + content_w - fl[i].x) : ro;
            }
        float cx = content_left + lo;
        float cwid = content_w - lo - ro;
        if (cwid < 0)
            cwid = 0;
        /* if no width left at this y, advance below the offending floats */
        if (cwid <= 0 && nfl > 0)
        {
            float nb = y;
            for (int i = 0; i < nfl; i++)
                if (fl[i].bottom > nb)
                    nb = fl[i].bottom;
            y = nb;
            lo = ro = 0;
            for (int i = 0; i < nfl; i++)
                if (fl[i].bottom > y)
                {
                    if (fl[i].left)
                        lo = (fl[i].x + fl[i].w) > lo ? (fl[i].x + fl[i].w) : lo;
                    else
                        ro = (content_left + content_w - fl[i].x) > ro ? (content_left + content_w - fl[i].x) : ro;
                }
            cx = content_left + lo;
            cwid = content_w - lo - ro;
            if (cwid < 0)
                cwid = 0;
        }

        /* Apply vertical margin collapsing with preceding sibling */
        if (has_prev_sibling)
        {
            float collapsed = collapse_margins(prev_m_bottom, c->style.margin[0]);
            y += (collapsed - prev_m_bottom);
        }
        else
        {
            y += c->style.margin[0];
        }

        layout_node(c, cx, y, cwid, content_h);
        if (c->style.len_w.v == 0.0f && c->style.len_w.unit == 0 &&
            (c->style.display == MINI_DISPLAY_BLOCK || c->style.display == MINI_DISPLAY_FLEX) &&
            c->style.w == 0.0f)
            c->style.w = cwid - c->style.margin[1] - c->style.margin[3];
        y += c->style.h;
        prev_m_bottom = c->style.margin[2];
        has_prev_sibling = 1;
    }
    if (has_prev_sibling)
        y += prev_m_bottom;
    /* BFC: contain floats — the bottom encloses the lowest float. */
    for (int i = 0; i < nfl; i++)
        if (fl[i].bottom > y)
            y = fl[i].bottom;
    *out_bottom = y;
    (void)s;
}

/* ---- Multicol ----
   column-count N (or columns: N) splits in-flow children into N balanced
   columns of width content_w/N (minus column-gap ≈ gap_px). Children are
   stacked into column 0 until its height reaches the target (total/N), then
   the next column, etc.                                              */
static void layout_multicol(struct MiniNode *n, float content_left,
                            float content_top, float content_w, float gap_px,
                            float *out_bottom)
{
    MiniStyle *s = &n->style;
    const char *cc = mini_node_get_attribute(n, "data-column-count");
    if (!cc)
        cc = mini_node_get_attribute(n, "data-columns");
    int N = cc ? atoi(cc) : 1;
    if (N < 1)
        N = 1;
    const char *cwv = mini_node_get_attribute(n, "data-column-width");
    if (cwv && cwv[0])
    {
        int from_w = (int)(grid_parse_len(cwv, content_w, s->font_size > 0 ? s->font_size : 16));
        if (from_w > 0)
            N = (int)(content_w / from_w);
        if (N < 1)
            N = 1;
    }
    float col_w = (content_w - (N - 1) * gap_px) / (float)N;
    if (col_w < 0)
        col_w = 0;
    /* first pass: measure each child at col_w, sum heights */
    int nc = 0;
    float total_h = 0;
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
    {
        if (c->style.display == MINI_DISPLAY_NONE || c->style.position == 2 || c->style.position == 3)
            continue;
        layout_node(c, content_left, content_top, col_w, 0);
        total_h += c->style.h + c->style.margin[0] + c->style.margin[2];
        nc++;
    }
    if (nc == 0)
    {
        *out_bottom = content_top;
        return;
    }
    float target = total_h / (float)N;
    int col = 0;
    float cy = content_top;
    float max_bottom = content_top;
    float col_x = content_left;
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
    {
        if (c->style.display == MINI_DISPLAY_NONE || c->style.position == 2 || c->style.position == 3)
            continue;
        if (col < N - 1 && (cy - content_top) >= target)
        {
            col++;
            col_x += col_w + gap_px;
            cy = content_top;
        }
        layout_node(c, col_x, cy, col_w, 0);
        c->style.w = col_w - c->style.margin[1] - c->style.margin[3];
        c->style.abs_x = col_x + c->style.margin[3];
        c->style.abs_y = cy + c->style.margin[0];
        cy += c->style.h + c->style.margin[0] + c->style.margin[2];
        if (cy > max_bottom)
            max_bottom = cy;
    }
    *out_bottom = max_bottom;
}

static void layout_node(struct MiniNode *n, float x, float y,
                        float avail_w, float avail_h)
{
    if (!n)
        return;
    if (n->parent)
    {
        if ((n->style.font_size <= 0.0f || !n->style.font_set) && n->parent->style.font_size > 0.0f)
        {
            n->style.font_size = n->parent->style.font_size;
            if (n->has_base_style) n->base_style.font_size = n->style.font_size;
        }
        if (!n->style.letter_set && n->parent->style.letter_set)
        {
            n->style.len_letter = n->parent->style.len_letter;
            n->style.letter_set = 1;
            if (n->has_base_style) { n->base_style.len_letter = n->style.len_letter; n->base_style.letter_set = 1; }
        }
        if (!n->style.line_height_set && n->parent->style.line_height_set)
        {
            n->style.len_line_height = n->parent->style.len_line_height;
            n->style.line_height_set = 1;
            if (n->has_base_style) { n->base_style.len_line_height = n->style.len_line_height; n->base_style.line_height_set = 1; }
        }
        if (!n->style.color_set && n->parent && n->parent->style.color_set)
        {
            n->style.color_r = n->parent->style.color_r;
            n->style.color_g = n->parent->style.color_g;
            n->style.color_b = n->parent->style.color_b;
            n->style.color_a = n->parent->style.color_a;
            n->style.color_set = 1;
            if (n->has_base_style) {
                n->base_style.color_r = n->style.color_r;
                n->base_style.color_g = n->style.color_g;
                n->base_style.color_b = n->style.color_b;
                n->base_style.color_a = n->style.color_a;
                n->base_style.color_set = 1;
            }
        }
        if (n->style.text_align == 0 && n->parent->style.text_align != 0)
        {
            n->style.text_align = n->parent->style.text_align;
            if (n->has_base_style) n->base_style.text_align = n->style.text_align;
        }
        if (n->style.text_transform == 0 && n->parent->style.text_transform != 0)
        {
            n->style.text_transform = n->parent->style.text_transform;
            if (n->has_base_style) n->base_style.text_transform = n->style.text_transform;
        }
        if (n->parent->style.perspective > 0.0f && n->style.perspective == 0.0f)
        {
            n->style.perspective = n->parent->style.perspective;
            if (n->has_base_style) n->base_style.perspective = n->style.perspective;
        }
    }
    if (n->type == MN_TEXT_NODE)
    {
        n->style.abs_x = x;
        n->style.abs_y = y;
        float fs = (n->parent && n->parent->style.font_size > 0)
                       ? n->parent->style.font_size
                       : (n->style.font_size > 0 ? n->style.font_size : 16.0f);
        n->style.font_size = fs;
        float ls = (n->parent && n->parent->style.letter_set) ? n->parent->style.len_letter.v : 0.0f;
        float lh = mini_text_line_height(fs);
        if (n->parent && n->parent->style.line_height_set)
        {
            if (n->parent->style.len_line_height.unit == 0 && n->parent->style.len_line_height.v > 0.0f && n->parent->style.len_line_height.v <= 5.0f)
                lh = fs * n->parent->style.len_line_height.v;
            else if (n->parent->style.len_line_height.v > 0.0f)
                lh = n->parent->style.len_line_height.v;
        }

        if (n->text && !is_all_ws(n->text))
        {
            char collapsed[1024];
            char transformed[1024];
            const char *src = n->text;
            size_t ci = 0;
            int in_ws = 0;
            while (*src && ci < sizeof(collapsed) - 1)
            {
                if (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n')
                {
                    if (!in_ws)
                    {
                        collapsed[ci++] = ' ';
                        in_ws = 1;
                    }
                }
                else
                {
                    collapsed[ci++] = *src;
                    in_ws = 0;
                }
                src++;
            }
            collapsed[ci] = '\0';

            const char *eff_text = collapsed[0] ? collapsed : n->text;
            if (n->parent && n->parent->style.text_transform == 1) /* uppercase */
            {
                size_t tl = strlen(eff_text);
                if (tl >= sizeof(transformed))
                    tl = sizeof(transformed) - 1;
                for (size_t ti = 0; ti < tl; ti++)
                {
                    unsigned char c = (unsigned char)eff_text[ti];
                    transformed[ti] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : (char)c;
                }
                transformed[tl] = '\0';
                eff_text = transformed;
            }
            else if (n->parent && n->parent->style.text_transform == 2) /* lowercase */
            {
                size_t tl = strlen(eff_text);
                if (tl >= sizeof(transformed))
                    tl = sizeof(transformed) - 1;
                for (size_t ti = 0; ti < tl; ti++)
                {
                    unsigned char c = (unsigned char)eff_text[ti];
                    transformed[ti] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : (char)c;
                }
                transformed[tl] = '\0';
                eff_text = transformed;
            }

            float measure_w = (avail_w > 0.0f) ? avail_w : 10000.0f;
            int lines = text_wrap_lines(eff_text, fs, measure_w, ls);
            n->style.w = mini_text_measure_ex(eff_text, fs, ls);
            if (avail_w > 0.0f && n->style.w > avail_w)
                n->style.w = avail_w;
            n->style.h = (float)lines * lh;
        }
        else
        {
            /* 纯空白段间节点宽高归零，防止标签间的换行缩进撑大父容器高度 */
            n->style.w = 0.0f;
            n->style.h = 0.0f;
        }
        n->style.laid_out = 1;
        return;
    }

    if (n->type != MN_ELEMENT_NODE && n->type != MN_DOCUMENT_FRAGMENT_NODE)
    {
        n->style.abs_x = x;
        n->style.abs_y = y;
        n->style.laid_out = 1;
        return;
    }
    MiniStyle *s = &n->style;
    if (s->display == MINI_DISPLAY_NONE)
    {
        s->laid_out = 1;
        return;
    }

    float own_font, gap_px = 0, min_w = 0, max_w = 0, min_h = 0, max_h = 0;
    float top_v = 0, left_v = 0, right_v = 0, bottom_v = 0;
    {
        float parent_font = (n->parent && n->parent->style.font_size > 0.0f)
                                ? n->parent->style.font_size
                                : g_lctx.root_font;
        if (s->font_set)
            s->font_size = resolve_field(n, CF_FONT, s->len_font, parent_font,
                                         parent_font, g_lctx.root_font,
                                         g_lctx.vw, g_lctx.vh);
        own_font = s->font_size > 0.0f ? s->font_size : g_lctx.root_font;
        s->w = resolve_field(n, CF_W, s->len_w, avail_w, own_font,
                             g_lctx.root_font, g_lctx.vw, g_lctx.vh);

        s->h = (s->len_h.unit == 1)
                   ? resolve_len(s->len_h, avail_h, own_font, g_lctx.root_font, g_lctx.vw, g_lctx.vh)
                   : resolve_field(n, CF_H, s->len_h, avail_w, own_font,
                                   g_lctx.root_font, g_lctx.vw, g_lctx.vh);

        for (int i = 0; i < 4; i++)
        {
            uint8_t mf[4] = {CF_MTOP, CF_MRIGHT, CF_MBOT, CF_MLEFT};
            uint8_t pf[4] = {CF_PTOP, CF_PRIGHT, CF_PBOT, CF_PLEFT};
            s->margin[i] = resolve_field(n, mf[i], s->len_margin[i], avail_w,
                                         own_font, g_lctx.root_font,
                                         g_lctx.vw, g_lctx.vh);
            s->padding[i] = resolve_field(n, pf[i], s->len_padding[i], avail_w,
                                          own_font, g_lctx.root_font,
                                          g_lctx.vw, g_lctx.vh);
        }
        gap_px = resolve_field(n, CF_GAP, s->len_gap, avail_w, own_font,
                               g_lctx.root_font, g_lctx.vw, g_lctx.vh);
        min_w = resolve_len(s->len_min_w, avail_w, own_font, g_lctx.root_font,
                            g_lctx.vw, g_lctx.vh);
        max_w = resolve_len(s->len_max_w, avail_w, own_font, g_lctx.root_font,
                            g_lctx.vw, g_lctx.vh);
        min_h = resolve_len(s->len_min_h, avail_h, own_font, g_lctx.root_font,
                            g_lctx.vw, g_lctx.vh);
        max_h = resolve_len(s->len_max_h, avail_h, own_font, g_lctx.root_font,
                            g_lctx.vw, g_lctx.vh);
        if (max_w > 0 && min_w > max_w)
            min_w = max_w;
        if (max_h > 0 && min_h > max_h)
            min_h = max_h;
        left_v = resolve_field(n, CF_LEFT, s->len_left, avail_w, own_font,
                               g_lctx.root_font, g_lctx.vw, g_lctx.vh);
        right_v = resolve_field(n, CF_RIGHT, s->len_right, avail_w, own_font,
                                g_lctx.root_font, g_lctx.vw, g_lctx.vh);
        top_v = (s->len_top.unit == 1) ? resolve_len(s->len_top, avail_h, own_font, g_lctx.root_font, g_lctx.vw, g_lctx.vh)
                                       : resolve_field(n, CF_TOP, s->len_top, avail_w, own_font, g_lctx.root_font,
                                                       g_lctx.vw, g_lctx.vh);
        bottom_v = (s->len_bottom.unit == 1) ? resolve_len(s->len_bottom, avail_h, own_font, g_lctx.root_font, g_lctx.vw, g_lctx.vh)
                                             : resolve_field(n, CF_BOTTOM, s->len_bottom, avail_w, own_font,
                                                             g_lctx.root_font, g_lctx.vw, g_lctx.vh);
    }

    if (s->position == 2 || s->position == 3)
    {
        if (s->display == MINI_DISPLAY_INLINE)
            s->display = MINI_DISPLAY_BLOCK;
        else if (s->display == MINI_DISPLAY_INLINE_FLEX)
            s->display = MINI_DISPLAY_FLEX;
    }

    float mw = s->margin[1] + s->margin[3];
    if (s->w <= 0)
    {
        const char *a = mini_node_get_attribute(n, "width");
        if (a)
            s->w = to_px(a);
    }
    if (s->h <= 0)
    {
        const char *a = mini_node_get_attribute(n, "height");
        if (a)
            s->h = to_px(a);
    }
    if (n->category == MINI_CAT_MEDIA)
    {
        if (s->w <= 0)
        {
            if (n->tag && (!strcmp(n->tag, "canvas") || !strcmp(n->tag, "svg")))
                s->w = 300;
            else if (n->tag && !strcmp(n->tag, "video"))
                s->w = 320;
            else if (n->tag && !strcmp(n->tag, "iframe"))
                s->w = 300;
            else
                s->w = 128;
        }
        if (s->h <= 0)
        {
            if (n->tag && (!strcmp(n->tag, "canvas") || !strcmp(n->tag, "svg")))
                s->h = 150;
            else if (n->tag && !strcmp(n->tag, "video"))
                s->h = 180;
            else if (n->tag && !strcmp(n->tag, "iframe"))
                s->h = 200;
            else if (n->tag && !strcmp(n->tag, "audio"))
                s->h = 40;
            else
                s->h = 96;
        }
    }
    if (n->category == MINI_CAT_FORM && n->tag)
    {
        if (!strcmp(n->tag, "input"))
        {
            const char *itype = mini_node_get_attribute(n, "type");
            if (itype && (!strcmp(itype, "checkbox") || !strcmp(itype, "radio")))
            {
                if (s->w <= 0)
                    s->w = 16.0f; /* 复选框与单选框固有宽度 16px */
                if (s->h <= 0)
                    s->h = 16.0f;
            }
            else
            {
                if (s->w <= 0)
                    s->w = 80.0f;
                if (s->h <= 0)
                    s->h = 22.0f;
            }
        }
        else if (!strcmp(n->tag, "button"))
        {
            float fs = s->font_size > 0 ? s->font_size : 16.0f;
            float lh = mini_text_line_height(fs);
            if (s->len_h.v == 0.0f)
            {
                s->h = lh + s->padding[0] + s->padding[2];
            }
            if (s->w <= 0)
            {
                char tb[256];
                size_t o = 0;
                collect_text_content(n, tb, sizeof tb, &o);
                tb[o < sizeof tb ? o : sizeof tb - 1] = 0;
                const char *val = mini_node_get_attribute(n, "value");
                const char *lbl = (val && val[0]) ? val : (tb[0] ? tb : "Button");
                /* 精准补偿间距，避免折行 */
                s->w = mini_text_measure_ex(lbl, fs, s->len_letter.v) + s->padding[1] + s->padding[3];
                if (s->w < 48.0f)
                    s->w = 48.0f;
            }
        }
        else if (!strcmp(n->tag, "select"))
        {
            if (s->w <= 0)
                s->w = 160;
            if (s->h <= 0)
                s->h = 24;
        }
        else if (!strcmp(n->tag, "textarea"))
        {
            if (s->w <= 0)
                s->w = 300;
            if (s->h <= 0)
                s->h = 80;
        }
        else if (!strcmp(n->tag, "progress") || !strcmp(n->tag, "meter"))
        {
            if (s->w <= 0)
                s->w = 50.0f;
            if (s->h <= 0)
                s->h = 14.0f;
        }

        else if (!strcmp(n->tag, "canvas") || !strcmp(n->tag, "img") || !strcmp(n->tag, "video") || !strcmp(n->tag, "svg"))
        {
            if (s->w <= 0)
            {
                const char *aw = mini_node_get_attribute(n, "width");
                if (aw && aw[0])
                    s->w = (float)atof(aw);
                else if (!strcmp(n->tag, "canvas"))
                    s->w = 300;
            }
            if (s->h <= 0)
            {
                const char *ah = mini_node_get_attribute(n, "height");
                if (ah && ah[0])
                    s->h = (float)atof(ah);
                else if (!strcmp(n->tag, "canvas"))
                    s->h = 150;
            }
        }
    }

    float ph = s->padding[1] + s->padding[3];
    float pv = s->padding[0] + s->padding[2];

    float cw = 0.0f;
    /* 1. 显式指定宽度的元素：绝对以设置的 width 为准，绝不覆盖为容器全宽 */
    if (s->len_w.v > 0.0f || s->len_w.unit != 0)
    {
        if (s->box_sizing == 1 || s->len_w.unit == 1)
            cw = s->w;
        else
            cw = s->w + ph;
    }
    /* 2. inline-flex / 弹性收缩 flex 容器：按子项内容总宽 shrink-to-fit */
    else if (s->display == MINI_DISPLAY_INLINE_FLEX ||
             (s->display == MINI_DISPLAY_FLEX && n->parent &&
              s->position != 2 && s->position != 3 &&
              (n->parent->style.display == MINI_DISPLAY_FLEX || n->parent->style.display == MINI_DISPLAY_INLINE_FLEX) &&
              !n->parent->style.is_grid))
    {
        float kids_w = 0.0f;
        int kcount = 0;
        for (struct MiniNode *k = n->first_child; k; k = k->next_sibling)
        {
            if (k->style.display != MINI_DISPLAY_NONE && k->style.position != 2 && k->style.position != 3 &&
                !(k->type == MN_TEXT_NODE && (!k->text || is_all_ws(k->text))))
            {
                float kw = 0.0f;
                float k_fs = k->style.font_size > 0 ? k->style.font_size : (s->font_size > 0 ? s->font_size : 16.0f);
                if (k->style.len_w.v > 0.0f || k->style.len_w.unit != 0)
                    kw = resolve_field(k, CF_W, k->style.len_w, avail_w, k_fs, g_lctx.root_font, g_lctx.vw, g_lctx.vh);
                else if (k->type == MN_TEXT_NODE && k->text)
                    kw = mini_text_measure_ex(k->text, k_fs, k->style.len_letter.v);
                else
                {
                    char tb[512];
                    size_t o = 0;
                    collect_text_content(k, tb, sizeof tb, &o);
                    tb[o < sizeof tb ? o : sizeof tb - 1] = 0;
                    if (o > 0)
                        kw = mini_text_measure_ex(tb, k_fs, k->style.len_letter.v) + k->style.padding[1] + k->style.padding[3];
                }
                float item_w = kw + k->style.margin[1] + k->style.margin[3];
                if (s->flex_direction == 0)
                    kids_w += item_w;
                else if (item_w > kids_w)
                    kids_w = item_w;
                kcount++;
            }
        }
        if (s->flex_direction == 0 && kcount > 1)
            kids_w += (kcount - 1) * gap_px;
        cw = kids_w + ph;
    }
    /* 2b. 替换/表单控件元素（canvas/img/video/svg/input/select/textarea/progress/meter）
       无显式 CSS 宽但已有固有尺寸（来自 width 属性或默认 300/150/80/50 等）：用固有
       宽度，而非走 inline 空元素的文本宽≈0。否则 <textarea>/<progress>/<meter>/
       <input> 在无 CSS 宽时被分支#3 算成 cw≈0，getBoundingClientRect width=0，
       控件不渲染（实测 m7）。button 例外：其宽度按标签文本 shrink-to-fit（分支#3）。 */
    else if (n->tag &&
             (!strcmp(n->tag, "canvas") || !strcmp(n->tag, "img") ||
              !strcmp(n->tag, "video") || !strcmp(n->tag, "svg") ||
              !strcmp(n->tag, "input") || !strcmp(n->tag, "select") ||
              !strcmp(n->tag, "textarea") || !strcmp(n->tag, "progress") ||
              !strcmp(n->tag, "meter")) &&
             s->w > 0.0f)
    {
        cw = (s->box_sizing == 1) ? s->w : s->w + ph;
    }
    /* 3. inline 元素 / 按钮 / flex 中未定宽的子项（flex 行或非 stretch 的 flex 列）：根据文本内容自适应宽度 */
    else if (s->display == MINI_DISPLAY_INLINE || (n->tag && !strcmp(n->tag, "button")) ||
             (n->parent &&
              s->position != 2 && s->position != 3 &&
              (n->parent->style.display == MINI_DISPLAY_FLEX || n->parent->style.display == MINI_DISPLAY_INLINE_FLEX) &&
              !n->parent->style.is_grid &&
              ((n->parent->style.flex_direction == 0) ||
               (n->parent->style.flex_direction == 1 &&
                (n->parent->style.align_items == 1 || n->parent->style.align_items == 2 || n->parent->style.align_items == 3 ||
                 s->align_self == 1 || s->align_self == 2 || s->align_self == 3)))))
    {

        char tb[1024];
        size_t o = 0;
        collect_text_content(n, tb, sizeof tb, &o);
        tb[o < sizeof tb ? o : sizeof tb - 1] = 0;
        if (o > 0)
        {
            float fs = s->font_size > 0 ? s->font_size : 16.0f;
            /* 修复：移除 + 2.0f 幽灵宽度。防止子项期望宽度大于 flex 容器预留宽度导致计算溢出 */
            cw = mini_text_measure_ex(tb, fs, s->len_letter.v) + ph;
        }
        else
        {
            cw = ph; /* 无文字的 inline 空元素本征宽度仅为内边距，不可撑满容器 */
        }
    }
    /* 4. 普通块级 block 元素：默认填满可用父级空间 */
    else
    {
        cw = (avail_w > mw) ? (avail_w - mw) : 0.0f;
    }

    /* 5. 限制最大/最小尺寸 */
    if (max_w > 0.0f && cw > max_w)
        cw = max_w;
    if (min_w > 0.0f && cw < min_w)
        cw = min_w;

    /* 6. margin: auto 水平居中计算 */
    if (s->len_margin[1].unit == 8 && s->len_margin[3].unit == 8 && avail_w > 0.0f)
    {
        float space = avail_w - cw;
        if (space > 0.0f)
        {
            s->margin[1] = space / 2.0f;
            s->margin[3] = space / 2.0f;
        }
    }
    if (max_w > 0 && cw > max_w)
        cw = max_w;
    if (min_w > 0 && cw < min_w)
        cw = min_w;
    if (cw < 0)
        cw = 0;
    s->w = cw;
    float ch = (s->h > 0) ? (s->box_sizing == 1 ? s->h : s->h + pv) : 0.0f;
    if (max_h > 0 && ch > max_h)
        ch = max_h;
    if (min_h > 0 && ch < min_h)
        ch = min_h;

    s->abs_x = x + s->margin[3];
    s->abs_y = y + s->margin[0];

    if (s->position == 1)
    {
        float lo = (s->len_left.v != 0.0f || s->len_left.unit != 0)     ? left_v
                   : (s->len_right.v != 0.0f || s->len_right.unit != 0) ? -right_v
                                                                        : 0.0f;
        float to = (s->len_top.v != 0.0f || s->len_top.unit != 0)         ? top_v
                   : (s->len_bottom.v != 0.0f || s->len_bottom.unit != 0) ? -bottom_v
                                                                          : 0.0f;
        s->abs_x += lo;
        s->abs_y += to;
        shift_subtree(n, lo, to);
    }
    else if (s->position == 4) /* sticky */
    {
        float flow_x = s->abs_x;
        float flow_y = s->abs_y;
        float sc_x = g_active_doc ? g_active_doc->scroll_x : 0.0f;
        float sc_y = g_active_doc ? g_active_doc->scroll_y : 0.0f;
        float dx = 0.0f, dy = 0.0f;

        if (s->has_sticky_top || s->len_top.v != 0.0f || s->len_top.unit != 0)
        {
            float target_y = sc_y + top_v;
            if (target_y > flow_y)
            {
                dy = target_y - flow_y;
                if (n->parent)
                {
                    float parent_bottom = n->parent->style.abs_y + n->parent->style.h - n->parent->style.padding[2];
                    float max_y = parent_bottom - (s->h + s->margin[2]);
                    if (flow_y + dy > max_y)
                        dy = max_y - flow_y;
                }
                if (dy < 0.0f)
                    dy = 0.0f;
            }
        }
        if (s->has_sticky_left || s->len_left.v != 0.0f || s->len_left.unit != 0)
        {
            float target_x = sc_x + left_v;
            if (target_x > flow_x)
            {
                dx = target_x - flow_x;
                if (n->parent)
                {
                    float parent_right = n->parent->style.abs_x + n->parent->style.w - n->parent->style.padding[1];
                    float max_x = parent_right - (s->w + s->margin[1]);
                    if (flow_x + dx > max_x)
                        dx = max_x - flow_x;
                }
                if (dx < 0.0f)
                    dx = 0.0f;
            }
        }
        s->abs_x += dx;
        s->abs_y += dy;
        if (dx != 0.0f || dy != 0.0f)
            shift_subtree(n, dx, dy);
    }

    if (n->tag && !strcmp(n->tag, "slot"))
    {
        struct MiniNode *host = slot_host(n);
        if (host)
        {
            float cy = s->abs_y + s->padding[0];
            float sw = cw - s->padding[1] - s->padding[3];
            if (sw < 0)
                sw = 0;
            for (struct MiniNode *c = host->first_child; c; c = c->next_sibling)
            {
                if (c->style.display == MINI_DISPLAY_NONE)
                    continue;
                layout_node(c, s->abs_x + s->padding[3], cy, sw, 0);
                cy += c->style.h + c->style.margin[0] + c->style.margin[2];
            }
            if (s->h <= 0)
                ch = cy - s->abs_y + s->padding[2];
            s->w = cw;
            s->h = ch;
            s->laid_out = 1;
            return;
        }
    }

    if (s->display == MINI_DISPLAY_FLEX || s->display == MINI_DISPLAY_INLINE_FLEX)
    {
        if (s->is_grid)
        {
            layout_grid(n, x, y, avail_w, avail_h, gap_px);
            return;
        }
        int row = (s->flex_direction == 0 || s->flex_direction == 2);
        int is_reverse = (s->flex_direction == 2 || s->flex_direction == 3);
        float main_avail = row ? (cw - s->padding[1] - s->padding[3])
                               : (ch - s->padding[0] - s->padding[2]);
        float cross = row ? (ch - s->padding[0] - s->padding[2])
                          : (cw - s->padding[1] - s->padding[3]);

        if (main_avail < 0.0f)
            main_avail = 0.0f;
        if (cross < 0.0f)
            cross = 0.0f;

        int kcount = 0;
        for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        {
            if (c->style.display != MINI_DISPLAY_NONE &&
                c->style.position != 2 && c->style.position != 3 &&
                !(c->type == MN_TEXT_NODE && (!c->text || is_all_ws(c->text))))
                kcount++;
        }

        struct MiniNode **flex_kids = kcount > 0 ? (struct MiniNode **)malloc(kcount * sizeof(struct MiniNode *)) : NULL;
        int kid_idx = 0;
        for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        {
            if (c->style.display != MINI_DISPLAY_NONE &&
                c->style.position != 2 && c->style.position != 3 &&
                !(c->type == MN_TEXT_NODE && (!c->text || is_all_ws(c->text))))
            {
                if (kid_idx < kcount)
                    flex_kids[kid_idx++] = c;
            }
        }

        for (int i = 1; i < kcount; i++)
        {
            struct MiniNode *key = flex_kids[i];
            int j = i - 1;
            while (j >= 0 && flex_kids[j]->style.order > key->style.order)
            {
                flex_kids[j + 1] = flex_kids[j];
                j--;
            }
            flex_kids[j + 1] = key;
        }

        /* Check flex-wrap */
        if (s->flex_wrap == 1 && row)
        {
            /* Multi-line flex wrap layout */
            typedef struct
            {
                int start_idx;
                int count;
                float line_used;
                float line_max_cross;
                float line_y;
            } FlexLine;

            FlexLine lines[128];
            int num_lines = 0;

            int line_start = 0;
            int line_count = 0;
            float line_used = 0.0f;
            float line_max_cross = 0.0f;

            for (int i = 0; i < kcount; i++)
            {
                struct MiniNode *c = flex_kids[i];
                layout_node(c, s->abs_x + s->padding[3], s->abs_y + s->padding[0], main_avail, cross);
                float step = c->style.w + c->style.margin[1] + c->style.margin[3];
                float c_cross = c->style.h + c->style.margin[0] + c->style.margin[2];

                if (line_count > 0 && line_used + step + gap_px > main_avail)
                {
                    if (num_lines < 128)
                    {
                        lines[num_lines].start_idx = line_start;
                        lines[num_lines].count = line_count;
                        lines[num_lines].line_used = line_used;
                        lines[num_lines].line_max_cross = line_max_cross;
                        num_lines++;
                    }
                    line_start = i;
                    line_count = 0;
                    line_used = 0.0f;
                    line_max_cross = 0.0f;
                }
                line_used += step;
                if (c_cross > line_max_cross)
                    line_max_cross = c_cross;
                line_count++;
            }
            if (line_count > 0 && num_lines < 128)
            {
                lines[num_lines].start_idx = line_start;
                lines[num_lines].count = line_count;
                lines[num_lines].line_used = line_used;
                lines[num_lines].line_max_cross = line_max_cross;
                num_lines++;
            }

            float total_cross_h = 0.0f;
            for (int li = 0; li < num_lines; li++)
                total_cross_h += lines[li].line_max_cross;
            float total_line_gaps = (num_lines > 1) ? (num_lines - 1) * gap_px : 0.0f;
            float cross_leftover = cross - total_cross_h - total_line_gaps;

            float line_lead = 0.0f, extra_line_gap = 0.0f;
            if (cross_leftover > 0.0f)
            {
                switch (s->align_content)
                {
                case 1:
                    line_lead = 0.0f;
                    break;
                case 2:
                    line_lead = cross_leftover;
                    break;
                case 3:
                    line_lead = cross_leftover / 2.0f;
                    break;
                case 4:
                    extra_line_gap = (num_lines > 1) ? (cross_leftover / (float)(num_lines - 1)) : 0.0f;
                    break;
                case 5:
                    line_lead = (num_lines > 0) ? (cross_leftover / (float)(num_lines * 2)) : 0.0f;
                    extra_line_gap = (num_lines > 0) ? (cross_leftover / (float)num_lines) : 0.0f;
                    break;
                case 6:
                    line_lead = (num_lines > 0) ? (cross_leftover / (float)(num_lines + 1)) : 0.0f;
                    extra_line_gap = (num_lines > 0) ? (cross_leftover / (float)(num_lines + 1)) : 0.0f;
                    break;
                default:
                    break;
                }
            }

            float cur_line_y = s->abs_y + s->padding[0] + line_lead;
            for (int li = 0; li < num_lines; li++)
            {
                float l_used = lines[li].line_used;
                int l_count = lines[li].count;
                int s_idx = lines[li].start_idx;

                float lead = 0.0f, line_gap = gap_px;
                float line_leftover = main_avail - l_used - (l_count > 1 ? (l_count - 1) * gap_px : 0.0f);
                if (line_leftover > 0.0f)
                {
                    if (s->justify_content == 1)
                        lead = line_leftover / 2.0f;
                    else if (s->justify_content == 2)
                        lead = line_leftover;
                    else if (s->justify_content == 3 && l_count > 1)
                        line_gap += line_leftover / (float)(l_count - 1);
                    else if (s->justify_content == 4 && l_count > 0)
                    {
                        lead = line_leftover / (float)(l_count * 2);
                        line_gap += line_leftover / (float)l_count;
                    }
                    else if (s->justify_content == 5 && l_count > 0)
                    {
                        lead = line_leftover / (float)(l_count + 1);
                        line_gap += line_leftover / (float)(l_count + 1);
                    }
                }

                if (is_reverse)
                {
                    float pos_x = s->abs_x + s->padding[3] + main_avail - lead;
                    for (int ki = 0; ki < l_count; ki++)
                    {
                        struct MiniNode *lc = flex_kids[s_idx + ki];
                        float nx = pos_x - lc->style.w - lc->style.margin[1];
                        float ny = cur_line_y + lc->style.margin[0];
                        float dx = nx - lc->style.abs_x;
                        float dy = ny - lc->style.abs_y;
                        lc->style.abs_x = nx;
                        lc->style.abs_y = ny;
                        if (dx != 0.0f || dy != 0.0f)
                            shift_subtree(lc, dx, dy);
                        pos_x -= (lc->style.w + lc->style.margin[1] + lc->style.margin[3] + line_gap);
                    }
                }
                else
                {
                    float pos_x = s->abs_x + s->padding[3] + lead;
                    for (int ki = 0; ki < l_count; ki++)
                    {
                        struct MiniNode *lc = flex_kids[s_idx + ki];
                        float nx = pos_x + lc->style.margin[3];
                        float ny = cur_line_y + lc->style.margin[0];
                        float dx = nx - lc->style.abs_x;
                        float dy = ny - lc->style.abs_y;
                        lc->style.abs_x = nx;
                        lc->style.abs_y = ny;
                        if (dx != 0.0f || dy != 0.0f)
                            shift_subtree(lc, dx, dy);
                        pos_x += lc->style.w + lc->style.margin[1] + lc->style.margin[3] + line_gap;
                    }
                }
                cur_line_y += lines[li].line_max_cross + gap_px + extra_line_gap;
            }

            if (s->h <= 0.0f)
                ch = cur_line_y - s->abs_y + s->padding[2];
        }
        else
        {
            /* Single-line flex layout */
            float used = 0.0f;
            float total_grow = 0.0f;
            float total_shrink = 0.0f;
            float max_cross = 0.0f;
            float max_ascent = 0.0f;

            for (int ki = 0; ki < kcount; ki++)
            {
                struct MiniNode *c = flex_kids[ki];
                float child_w_constraint = row ? main_avail : cross;
                float child_h_constraint = row ? cross : main_avail;

                layout_node(c, s->abs_x + s->padding[3], s->abs_y + s->padding[0],
                            child_w_constraint, child_h_constraint);

                float step = row ? c->style.w + c->style.margin[1] + c->style.margin[3]
                                 : c->style.h + c->style.margin[0] + c->style.margin[2];
                used += step;
                if (c->style.flex_grow > 0.0f)
                    total_grow += c->style.flex_grow;
                float shrink_factor = (c->style.flex_shrink > 0.0f || c->style.flex_shrink == 0.0f) ? c->style.flex_shrink : 1.0f;
                total_shrink += shrink_factor * (row ? c->style.w : c->style.h);

                float child_cross = row ? (c->style.h + c->style.margin[0] + c->style.margin[2])
                                        : (c->style.w + c->style.margin[1] + c->style.margin[3]);
                if (child_cross > max_cross)
                    max_cross = child_cross;

                float c_ascent = 0.0f;
                int has_inflow_text = (c->type == MN_TEXT_NODE) || (c->first_child && (c->first_child->type == MN_TEXT_NODE && c->first_child->text && !is_all_ws(c->first_child->text)));
                if (has_inflow_text)
                {
                    float c_fs = c->style.font_size > 0.0f ? c->style.font_size : 16.0f;
                    c_ascent = c_fs * 0.82f;
                }
                else
                {
                    c_ascent = c->style.h;
                }
                if (c_ascent > max_ascent)
                    max_ascent = c_ascent;
            }

            if (cross <= 0.0f)
                cross = max_cross;

            float total_gap = (kcount > 1) ? (kcount - 1) * gap_px : 0.0f;
            float leftover = main_avail - used - total_gap;

            if (leftover > 0.0f && total_grow > 0.0f)
            {
                for (int ki = 0; ki < kcount; ki++)
                {
                    struct MiniNode *c = flex_kids[ki];
                    if (c->style.flex_grow > 0.0f)
                    {
                        float grow = leftover * (c->style.flex_grow / total_grow);
                        if (row)
                            c->style.w += grow;
                        else
                            c->style.h += grow;
                    }
                }
                used = main_avail;
                leftover = 0.0f;
            }
            else if (leftover < 0.0f && total_shrink > 0.0f)
            {
                float overflow = -leftover;
                for (int ki = 0; ki < kcount; ki++)
                {
                    struct MiniNode *c = flex_kids[ki];
                    float shrink_factor = (c->style.flex_shrink > 0.0f || c->style.flex_shrink == 0.0f) ? c->style.flex_shrink : 1.0f;
                    float child_basis = row ? c->style.w : c->style.h;
                    float reduction = overflow * (shrink_factor * child_basis) / total_shrink;
                    if (row)
                    {
                        c->style.w -= reduction;
                        if (c->style.w < 0)
                            c->style.w = 0;
                    }
                    else
                    {
                        c->style.h -= reduction;
                        if (c->style.h < 0)
                            c->style.h = 0;
                    }
                }
                leftover = 0.0f;
            }

            /* Auto margins */
            int auto_margins = 0;
            for (int ki = 0; ki < kcount; ki++)
            {
                struct MiniNode *c = flex_kids[ki];
                if (row)
                {
                    if (c->style.len_margin[3].unit == 8)
                        auto_margins++;
                    if (c->style.len_margin[1].unit == 8)
                        auto_margins++;
                }
                else
                {
                    if (c->style.len_margin[0].unit == 8)
                        auto_margins++;
                    if (c->style.len_margin[2].unit == 8)
                        auto_margins++;
                }
            }

            float lead = 0.0f, gap = 0.0f;
            if (leftover > 0.0f && auto_margins > 0)
            {
                float per_auto = leftover / (float)auto_margins;
                for (int ki = 0; ki < kcount; ki++)
                {
                    struct MiniNode *c = flex_kids[ki];
                    if (row)
                    {
                        if (c->style.len_margin[3].unit == 8)
                            c->style.margin[3] = per_auto;
                        if (c->style.len_margin[1].unit == 8)
                            c->style.margin[1] = per_auto;
                    }
                    else
                    {
                        if (c->style.len_margin[0].unit == 8)
                            c->style.margin[0] = per_auto;
                        if (c->style.len_margin[2].unit == 8)
                            c->style.margin[2] = per_auto;
                    }
                }
                leftover = 0.0f;
            }
            else if (leftover > 0.0f)
            {
                switch (s->justify_content)
                {
                case 1:
                    lead = leftover / 2.0f;
                    break;
                case 2:
                    lead = leftover;
                    break;
                case 3:
                    gap = (kcount > 1) ? (leftover / (float)(kcount - 1)) : 0.0f;
                    break;
                case 4:
                    lead = (kcount > 0) ? leftover / (float)(kcount * 2) : 0.0f;
                    gap = (kcount > 0) ? leftover / (float)kcount : 0.0f;
                    break;
                case 5:
                    lead = (kcount > 0) ? leftover / (float)(kcount + 1) : 0.0f;
                    gap = (kcount > 0) ? leftover / (float)(kcount + 1) : 0.0f;
                    break;
                default:
                    break;
                }
            }

            /* Final placement */
            if (is_reverse)
            {
                float pos = (row ? s->abs_x + s->padding[3] + main_avail - lead
                                 : s->abs_y + s->padding[0] + main_avail - lead);
                for (int ki = 0; ki < kcount; ki++)
                {
                    struct MiniNode *c = flex_kids[ki];
                    float child_cross = row ? c->style.h : c->style.w;
                    int has_cross_auto_margin = row ? (c->style.len_margin[0].unit == 8 || c->style.len_margin[2].unit == 8)
                                                    : (c->style.len_margin[1].unit == 8 || c->style.len_margin[3].unit == 8);
                    int ai = (c->style.align_self >= 0) ? c->style.align_self : s->align_items;
                    float off = 0.0f;
                    if (!has_cross_auto_margin)
                    {
                        if (ai == 2)
                            off = (cross - child_cross) / 2.0f;
                        else if (ai == 3)
                            off = cross - child_cross;
                        else if (ai == 4 && row)
                        {
                            float c_ascent = 0.0f;
                            int has_inflow_text = (c->type == MN_TEXT_NODE) || (c->first_child && (c->first_child->type == MN_TEXT_NODE && c->first_child->text && !is_all_ws(c->first_child->text)));
                            if (has_inflow_text)
                            {
                                float c_fs = c->style.font_size > 0.0f ? c->style.font_size : 16.0f;
                                c_ascent = c_fs * 0.82f;
                            }
                            else
                            {
                                c_ascent = c->style.h;
                            }
                            off = max_ascent - c_ascent;
                        }
                    }
                    if (off < 0.0f)
                        off = 0.0f;

                    float final_x = row ? (pos - c->style.w - c->style.margin[1]) : (s->abs_x + s->padding[3] + off);
                    float final_y = row ? (s->abs_y + s->padding[0] + off) : (pos - c->style.h - c->style.margin[2]);

                    float nx = final_x;
                    float ny = final_y;
                    float dx = nx - c->style.abs_x;
                    float dy = ny - c->style.abs_y;
                    c->style.abs_x = nx;
                    c->style.abs_y = ny;
                    if (dx != 0.0f || dy != 0.0f)
                        shift_subtree(c, dx, dy);

                    pos -= (row ? c->style.w + c->style.margin[1] + c->style.margin[3]
                                : c->style.h + c->style.margin[0] + c->style.margin[2]) +
                           gap + gap_px;
                }
            }
            else
            {
                float pos = (row ? s->abs_x + s->padding[3] : s->abs_y + s->padding[0]) + lead;
                for (int ki = 0; ki < kcount; ki++)
                {
                    struct MiniNode *c = flex_kids[ki];
                    float child_cross = row ? c->style.h : c->style.w;
                    int has_cross_auto_margin = row ? (c->style.len_margin[0].unit == 8 || c->style.len_margin[2].unit == 8)
                                                    : (c->style.len_margin[1].unit == 8 || c->style.len_margin[3].unit == 8);
                    int ai = (c->style.align_self >= 0) ? c->style.align_self : s->align_items;
                    float off = 0.0f;
                    if (!has_cross_auto_margin)
                    {
                        if (ai == 2)
                            off = (cross - child_cross) / 2.0f;
                        else if (ai == 3)
                            off = cross - child_cross;
                        else if (ai == 4 && row)
                        {
                            float c_ascent = 0.0f;
                            int has_inflow_text = (c->type == MN_TEXT_NODE) || (c->first_child && (c->first_child->type == MN_TEXT_NODE && c->first_child->text && !is_all_ws(c->first_child->text)));
                            if (has_inflow_text)
                            {
                                float c_fs = c->style.font_size > 0.0f ? c->style.font_size : 16.0f;
                                c_ascent = c_fs * 0.82f;
                            }
                            else
                            {
                                c_ascent = c->style.h;
                            }
                            off = max_ascent - c_ascent;
                        }
                    }
                    if (off < 0.0f)
                        off = 0.0f;

                    float final_x = row ? pos : (s->abs_x + s->padding[3] + off);
                    float final_y = row ? (s->abs_y + s->padding[0] + off) : pos;

                    float nx = final_x + c->style.margin[3];
                    float ny = final_y + c->style.margin[0];
                    float dx = nx - c->style.abs_x;
                    float dy = ny - c->style.abs_y;
                    c->style.abs_x = nx;
                    c->style.abs_y = ny;
                    if (dx != 0.0f || dy != 0.0f)
                        shift_subtree(c, dx, dy);

                    pos += (row ? c->style.w + c->style.margin[1] + c->style.margin[3]
                                : c->style.h + c->style.margin[0] + c->style.margin[2]) +
                           gap + gap_px;
                }
            }

            if (s->h <= 0.0f)
                ch = (!row) ? (used + total_gap + s->padding[0] + s->padding[2]) : (max_cross + s->padding[0] + s->padding[2]);
        }
        if (flex_kids)
            free(flex_kids);
    }
    else
    {
        float content_left = s->abs_x + s->padding[3];
        float content_w = cw - s->padding[1] - s->padding[3];
        float content_h = (s->h > 0.0f) ? (s->h - s->padding[0] - s->padding[2]) : (ch > 0.0f ? ch - s->padding[0] - s->padding[2] : 0.0f);
        if (content_w < 0.0f)
            content_w = 0.0f;
        if (content_h < 0.0f)
            content_h = 0.0f;
        float cy = s->abs_y + s->padding[0];

        /* CSS Multicol */
        if (mini_node_get_attribute(n, "data-column-count") ||
            mini_node_get_attribute(n, "data-columns") ||
            mini_node_get_attribute(n, "data-column-width"))
        {
            float bot = cy;
            layout_multicol(n, content_left, cy, content_w, gap_px, &bot);
            cy = bot;
        }
        else
        {
            int has_float = 0, bfc = (s->overflow != 0);
            for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
                if (c->style.display != MINI_DISPLAY_NONE &&
                    mini_node_get_attribute(c, "data-float"))
                {
                    has_float = 1;
                    break;
                }
            if (has_float || bfc)
            {
                float bot = cy;
                layout_block_children(n, content_left, cy, content_w, content_h, &bot);
                cy = bot;
            }
            else if (n->tag && !strcmp(n->tag, "tr"))
            {
                int ncells = 0, total_span = 0;
                for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
                    if (c->type == MN_ELEMENT_NODE && c->tag &&
                        (!strcmp(c->tag, "td") || !strcmp(c->tag, "th")) &&
                        c->style.display != MINI_DISPLAY_NONE)
                    {
                        ncells++;
                        const char *cs = mini_node_get_attribute(c, "colspan");
                        int sp = cs ? atoi(cs) : 1;
                        if (sp < 1)
                            sp = 1;
                        total_span += sp;
                    }
                if (ncells > 0)
                {
                    float unit = total_span > 0 ? content_w / (float)total_span : content_w;
                    float cxp = content_left;
                    float row_h = 0.0f;
                    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
                    {
                        if (c->type != MN_ELEMENT_NODE || !c->tag ||
                            (strcmp(c->tag, "td") && strcmp(c->tag, "th")) ||
                            c->style.display == MINI_DISPLAY_NONE)
                            continue;
                        const char *cs = mini_node_get_attribute(c, "colspan");
                        int sp = cs ? atoi(cs) : 1;
                        if (sp < 1)
                            sp = 1;
                        float cellw = unit * (float)sp;
                        layout_node(c, cxp, cy, cellw, 0);
                        cxp += cellw;
                        float chh = c->style.h + c->style.margin[0] + c->style.margin[2];
                        if (chh > row_h)
                            row_h = chh;
                    }
                    cy += row_h;
                }
            }
            else
            {
                float pen_x = content_left;
                float line_h = 0.0f;
                float prev_margin_bottom = 0.0f;
                int has_prev_block = 0;
                struct MiniNode *c = n->first_child;
                struct MiniNode *line_kids[64];
                int line_kid_count = 0;

                while (c)
                {
                    if (c->style.display != MINI_DISPLAY_NONE &&
                        c->style.position != 2 && c->style.position != 3)
                    {
                        int is_inline = (c->type == MN_TEXT_NODE || c->style.display == MINI_DISPLAY_INLINE);

                        if (is_inline)
                        {
                            has_prev_block = 0;
                            prev_margin_bottom = 0.0f;
                            if (c->tag && !strcmp(c->tag, "br"))
                            {
                                flush_inline_line_align(line_kids, line_kid_count, content_left, pen_x, content_w, s->text_align);
                                line_kid_count = 0;
                                float fs = s->font_size > 0.0f ? s->font_size : 16.0f;
                                float br_lh = mini_text_line_height(fs);
                                cy += line_h > 0.0f ? line_h : br_lh;
                                pen_x = content_left;
                                line_h = 0.0f;
                                c->style.w = 0;
                                c->style.h = 0;
                                c->style.laid_out = 1;
                                c = c->next_sibling;
                                continue;
                            }

                            /* 若为行内纯空格文本节点（如 span 之间的空格），仅在行内推进 pen_x，绝不自增高度 */
                            if (c->type == MN_TEXT_NODE && (!c->text || is_all_ws(c->text)))
                            {
                                if (pen_x > content_left)
                                {
                                    float sp_fs = s->font_size > 0.0f ? s->font_size : 16.0f;
                                    pen_x += mini_text_measure(" ", sp_fs);
                                }
                                c->style.w = 0.0f;
                                c->style.h = 0.0f;
                                c->style.laid_out = 1;
                                c = c->next_sibling;
                                continue;
                            }

                            float rem_w = content_w - (pen_x - content_left);
                            if (rem_w < 0.0f)
                                rem_w = 0.0f;

                            layout_node(c, pen_x, cy, rem_w, content_h);

                            float step_w = c->style.w + c->style.margin[1] + c->style.margin[3];
                            float step_h = c->style.h + c->style.margin[0] + c->style.margin[2];

                            /* 仅在整行确实放不下且不是行首时才换行 */
                            if (pen_x > content_left && (pen_x + step_w > content_left + content_w + 4.0f))
                            {
                                flush_inline_line_align(line_kids, line_kid_count, content_left, pen_x, content_w, s->text_align);
                                line_kid_count = 0;
                                cy += line_h > 0.0f ? line_h : 18.0f;
                                pen_x = content_left;
                                line_h = 0.0f;

                                rem_w = content_w;
                                layout_node(c, pen_x, cy, rem_w, content_h);
                                step_w = c->style.w + c->style.margin[1] + c->style.margin[3];
                                step_h = c->style.h + c->style.margin[0] + c->style.margin[2];
                            }

                            /* Baseline alignment for inline elements */
                            if (c->style.display == MINI_DISPLAY_INLINE && c->type != MN_TEXT_NODE)
                            {
                                float parent_fs = s->font_size > 0.0f ? s->font_size : 16.0f;
                                float child_fs = c->style.font_size > 0.0f ? c->style.font_size : parent_fs;
                                const char *cls = mini_node_get_attribute(c, "class");
                                if (cls && (strstr(cls, "accent-dot") || strstr(cls, "terminal-cursor")))
                                {
                                    float baseline = parent_fs * 0.82f;
                                    float target_ny = cy + baseline - c->style.h;
                                    if (strstr(cls, "accent-dot"))
                                        target_ny -= parent_fs * 0.05f;
                                    if (strstr(cls, "terminal-cursor"))
                                        target_ny += parent_fs * 0.15f;
                                    float dy = target_ny - c->style.abs_y;
                                    c->style.abs_y = target_ny;
                                    if (dy != 0.0f)
                                        shift_subtree(c, 0.0f, dy);
                                }
                                else if (child_fs < parent_fs)
                                {
                                    /* W3C inline baseline alignment: align child baseline to parent line baseline */
                                    float baseline_p = parent_fs * 0.82f;
                                    float baseline_c = child_fs * 0.82f;
                                    float shift_y = baseline_p - baseline_c;
                                    float target_ny = cy + shift_y;
                                    float dy = target_ny - c->style.abs_y;
                                    c->style.abs_y = target_ny;
                                    if (dy != 0.0f)
                                        shift_subtree(c, 0.0f, dy);
                                }
                            }

                            if (line_kid_count < (int)(sizeof(line_kids) / sizeof(line_kids[0])))
                                line_kids[line_kid_count++] = c;

                            pen_x += step_w;
                            if (step_h > line_h)
                                line_h = step_h;
                        }
                        else
                        {
                            flush_inline_line_align(line_kids, line_kid_count, content_left, pen_x, content_w, s->text_align);
                            line_kid_count = 0;
                            if (line_h > 0.0f || pen_x > content_left)
                            {
                                cy += line_h;
                                pen_x = content_left;
                                line_h = 0.0f;
                                has_prev_block = 0;
                                prev_margin_bottom = 0.0f;
                            }

                            float c_mtop = (c->style.len_margin[0].unit != 0 || c->style.len_margin[0].v != 0.0f)
                                               ? resolve_field(c, CF_MTOP, c->style.len_margin[0], content_w, own_font, g_lctx.root_font, g_lctx.vw, g_lctx.vh)
                                               : c->style.margin[0];
                            c->style.margin[0] = c_mtop;

                            if (has_prev_block)
                            {
                                float collapsed = collapse_margins(prev_margin_bottom, c_mtop);
                                cy = cy - prev_margin_bottom + collapsed - c_mtop;
                            }

                            /* 标准块级流排版：将当前父级的 content_left 与 cy 传给子块，子块内部会精确放置自身与所有子项 */
                            layout_node(c, content_left, cy, content_w, content_h);

                            if (c->style.len_w.v == 0.0f && c->style.len_w.unit == 0 &&
                                (c->style.display == MINI_DISPLAY_BLOCK || c->style.display == MINI_DISPLAY_FLEX) &&
                                c->style.w == 0.0f)
                            {
                                c->style.w = content_w - c->style.margin[1] - c->style.margin[3];
                            }

                            float actual_h = c->style.h;
                            if (actual_h <= 0.0f)
                                actual_h = mini_text_line_height(c->style.font_size > 0.0f ? c->style.font_size : 16.0f);

                            /* 游标推进到当前子块的底部加上其外边距，严禁外层调用 shift_subtree 造成负偏移重叠 */
                            cy = c->style.abs_y + actual_h + c->style.margin[2];
                            prev_margin_bottom = c->style.margin[2];
                            has_prev_block = 1;
                            pen_x = content_left;
                            line_h = 0.0f;
                        }
                    }
                    c = c->next_sibling;
                }

                flush_inline_line_align(line_kids, line_kid_count, content_left, pen_x, content_w, s->text_align);
                line_kid_count = 0;

                if (line_h > 0.0f)
                {
                    cy += line_h;
                }
            }
        }
        if (s->h <= 0)
            ch = cy - s->abs_y + s->padding[2];
    }

    if (n->shadow_root)
        layout_node(n->shadow_root, s->abs_x + s->padding[3],
                    s->abs_y + s->padding[0],
                    cw - s->padding[1] - s->padding[3], ch);
    if (max_w > 0 && cw > max_w)
        cw = max_w;
    if (min_w > 0 && cw < min_w)
        cw = min_w;
    if (max_h > 0 && ch > max_h)
        ch = max_h;
    if (min_h > 0 && ch < min_h)
        ch = min_h;
    if (n->tag && !strcmp(n->tag, "body"))
    {
        cw = avail_w;
        float doc_min_h = avail_h > 0 ? avail_h : g_lctx.vh;
        if (ch < doc_min_h)
            ch = doc_min_h;
    }
    s->w = cw;
    s->h = ch;
    s->laid_out = 1;
}

static int len_set(MiniLength L) { return (L.unit != 8) && (L.v != 0.0f || L.unit != 0); }

static struct MiniNode *abs_containing_block(struct MiniNode *n)
{
    for (struct MiniNode *a = n->parent; a; a = a->parent)
        if (a->type == MN_ELEMENT_NODE && a->style.position != 0)
            return a;
    return NULL;
}

static void layout_absolutes(struct MiniNode *n)
{
    if (!n)
        return;
    if (n->type == MN_ELEMENT_NODE &&
        (n->style.position == 2 || n->style.position == 3) &&
        !n->style.laid_out)
    {
        MiniStyle *s = &n->style;
        float cb_x, cb_y, cb_w, cb_h;
        if (s->position == 3)
        {
            cb_x = 0;
            cb_y = 0;
            cb_w = g_lctx.vw;
            cb_h = g_lctx.vh;
        }
        else
        {
            struct MiniNode *cb = abs_containing_block(n);
            if (cb)
            {
                MiniStyle *c = &cb->style;
                cb_x = c->abs_x + c->border_w[3];
                cb_y = c->abs_y + c->border_w[0];
                cb_w = c->w - c->border_w[1] - c->border_w[3];
                cb_h = (c->h > 0 ? c->h : g_lctx.vh) - c->border_w[0] - c->border_w[2];
            }
            else
            {
                cb_x = 0;
                cb_y = 0;
                cb_w = g_lctx.vw;
                cb_h = g_lctx.vh;
            }
        }
        if (cb_w < 0)
            cb_w = 0;
        if (cb_h < 0)
            cb_h = 0;

        float own_font = s->font_size > 0.0f ? s->font_size : g_lctx.root_font;
        float lv = resolve_field(n, CF_LEFT, s->len_left, cb_w, own_font, g_lctx.root_font, g_lctx.vw, g_lctx.vh);
        float rv = resolve_field(n, CF_RIGHT, s->len_right, cb_w, own_font, g_lctx.root_font, g_lctx.vw, g_lctx.vh);
        float tv = resolve_field(n, CF_TOP, s->len_top, cb_h, own_font, g_lctx.root_font, g_lctx.vw, g_lctx.vh);
        float bv = resolve_field(n, CF_BOTTOM, s->len_bottom, cb_h, own_font, g_lctx.root_font, g_lctx.vw, g_lctx.vh);

        float tw = len_set(s->len_w) ? resolve_field(n, CF_W, s->len_w, cb_w, own_font, g_lctx.root_font, g_lctx.vw, g_lctx.vh) : (s->w > 0 ? s->w : cb_w);
        float th = len_set(s->len_h) ? resolve_field(n, CF_H, s->len_h, cb_h, own_font, g_lctx.root_font, g_lctx.vw, g_lctx.vh) : (s->h > 0 ? s->h : cb_h);

        float target_w = cb_w;
        float pos_x = cb_x;
        int has_l = len_set(s->len_left);
        int has_r = len_set(s->len_right);
        if (has_l && has_r && !len_set(s->len_w))
        {
            target_w = cb_w - lv - rv - s->margin[1] - s->margin[3];
            if (target_w < 0)
                target_w = 0;
            pos_x = cb_x + lv + s->margin[3];
        }
        else if (has_r && !has_l)
        {
            pos_x = cb_x + cb_w - rv - tw - s->margin[1];
            target_w = tw;
        }
        else
        {
            pos_x = cb_x + (has_l ? lv : 0.0f) + s->margin[3];
            target_w = tw;
        }

        float target_h = cb_h;
        float pos_y = cb_y;
        int has_t = len_set(s->len_top);
        int has_b = len_set(s->len_bottom);
        if (has_t && has_b && !len_set(s->len_h))
        {
            target_h = cb_h - tv - bv - s->margin[0] - s->margin[2];
            if (target_h < 0)
                target_h = 0;
            pos_y = cb_y + tv + s->margin[0];
        }
        else if (has_b && !has_t)
        {
            pos_y = cb_y + cb_h - bv - th - s->margin[2];
            target_h = th;
        }
        else
        {
            pos_y = cb_y + (has_t ? tv : 0.0f) + s->margin[0];
            target_h = th;
        }

        s->w = target_w;
        s->h = target_h;
        layout_node(n, pos_x, pos_y, target_w, target_h);
    }

    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        layout_absolutes(c);
    if (n->shadow_root)
        layout_absolutes(n->shadow_root);
}

static void draw_text_wrapped_ex(MiniRenderer *r, const char *text,
                                 float x, float y, float max_w, float fs,
                                 float cr, float cg, float cb, float ca, int align, float ls, float lh, int font_weight, int font_style);

static void draw_text_wrapped(MiniRenderer *r, const char *text,
                              float x, float y, float max_w, float fs,
                              float cr, float cg, float cb, float ca, int align)
{
    if (!text || !*text || fs <= 0.0f || ca <= 0.0f)
        return;
    float lh = mini_text_line_height(fs);
    draw_text_wrapped_ex(r, text, x, y, max_w, fs, cr, cg, cb, ca, align, 0.0f, lh, 400, 0);
}

static void prune_empty_pseudos(struct MiniNode *n)
{
    if (!n)
        return;
    if (n->type == MN_ELEMENT_NODE && n->tag && !strncmp(n->tag, "::", 2))
    {
        /* W3C: Pseudo-elements without content are not generated.
           We set display to NONE so layout engines ignore them. */
        const char *cnt = mini_node_get_attribute(n, "content");
        if (!cnt)
            n->style.display = MINI_DISPLAY_NONE;
    }
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        prune_empty_pseudos(c);
    if (n->shadow_root)
        prune_empty_pseudos(n->shadow_root);
}

static void calc_content_bounds(struct MiniNode *n, float *max_x, float *max_y)
{
    if (!n || n->style.display == MINI_DISPLAY_NONE)
        return;
    if (n->style.position != 3 /* not fixed */)
    {
        float right = n->style.abs_x + n->style.w;
        float bottom = n->style.abs_y + n->style.h + n->style.margin[2];
        if (right > *max_x)
            *max_x = right;
        if (bottom > *max_y)
            *max_y = bottom;
    }
    if (n->style.overflow == 1)
        return;
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        calc_content_bounds(c, max_x, max_y);
    if (n->shadow_root)
        calc_content_bounds(n->shadow_root, max_x, max_y);
}

static void reset_laid_out_rec(struct MiniNode *n)
{
    if (!n)
        return;
    n->style.laid_out = 0;
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        reset_laid_out_rec(c);
    if (n->shadow_root)
        reset_laid_out_rec(n->shadow_root);
}

void mini_layout_run(MiniDocument *doc, int vw, int vh)
{
    if (!doc || !doc->body)
        return;
    /* Consume the layout-dirty gate: the host loop gated this call on
       (doc->dirty || doc->layout_dirty), so arriving here means geometry
       actually changed. Clear now so a subsequent pure-paint animation
       frame can skip us. */
    doc->layout_dirty = 0;
    /* 每帧开始前重置所有节点的布局完成标记，确保 hover/过渡时绝对定位节点正常重新计算 */
    prune_empty_pseudos(doc->root);
    reset_laid_out_rec(doc->root);

    /* publish the layout context for this frame: viewport + root font-size
       (rem base = the <html> element's font-size, else 16).               */
    g_lctx.vw = (float)vw;
    g_lctx.vh = (float)vh;
    g_lctx.root_font = 16.0f;
    for (struct MiniNode *e = doc->root->first_child; e; e = e->next_sibling)
    {
        if (e->type == MN_ELEMENT_NODE && e->tag && !strcmp(e->tag, "html"))
        {
            if (e->style.font_set)
            {
                float rf = resolve_len(e->style.len_font, 16.0f, 16.0f,
                                       16.0f, g_lctx.vw, g_lctx.vh);
                if (rf > 0.0f)
                    g_lctx.root_font = rf;
            }
            break;
        }
    }
    layout_node(doc->body, 0, 0, (float)vw, (float)vh);
    /* out-of-flow elements (absolute/fixed) were skipped above; position
       and lay them out now that their containing blocks have geometry.   */
    layout_absolutes(doc->body);
    if (doc->root != doc->body)
        layout_absolutes(doc->root);

    float max_x = (float)vw, max_y = (float)vh;
    calc_content_bounds(doc->body, &max_x, &max_y);
    doc->viewport_w = vw;
    doc->viewport_h = vh;
    doc->max_scroll_x = (max_x > (float)vw) ? (max_x - (float)vw) : 0.0f;
    doc->max_scroll_y = (max_y > (float)vh) ? (max_y - (float)vh) : 0.0f;
}

/* ------------------------------------------------------------------ */
/* Render: walk laid-out tree, emit vector commands.                   */
/* ------------------------------------------------------------------ */
/* ================================================================== */
/* Render: walk the laid-out tree, emit vector commands.               */
/* Tag-aware: every native HTML element category draws a representative */
/* placeholder (media/form/table/interactive) and real text content is  */
/* rasterized through the renderer's built-in 5x7 font. Shadow DOM hosts */
/* render their shadow tree (slots pull in light children).            */
/* ================================================================== */

/* first selected (or first) <option>'s text for a <select> */
static const char *select_selected_text(struct MiniNode *sel)
{
    struct MiniNode *first = NULL;
    for (struct MiniNode *c = sel->first_child; c; c = c->next_sibling)
    {
        if (c->type == MN_ELEMENT_NODE && c->tag && !strcmp(c->tag, "option"))
        {
            if (!first)
                first = c;
            if (mini_node_get_attribute(c, "selected") != NULL)
                return c->text ? c->text : "";
        }
    }
    return (first && first->text) ? first->text : "";
}

/* Paint the blinking text caret for a focused <input>/<textarea>. The caret
   byte offset is kept on the node by mini_events.c; here we measure the
   value prefix up to it for the caret's x and toggle it on/off using the
   same wall-clock the animation pass advances. Clamps to the field box so
   long text doesn't draw the caret past the border. */
static float caret_measure_prefix(const char *s, int off, float fs, float ls)
{
    int len = s ? (int)strlen(s) : 0;
    if (off <= 0 || len == 0)
        return 0.0f;
    if (off > len)
        off = len;
    char *tmp = (char *)malloc(off + 1);
    if (!tmp)
        return 0.0f;
    memcpy(tmp, s, off);
    tmp[off] = 0;
    float w = mini_text_measure_ex(tmp, fs, ls);
    free(tmp);
    return w;
}

static void render_text_caret(struct MiniNode *n, MiniRenderer *r,
                              const char *val, float x0, float y0,
                              float fs, float box_x, float box_w,
                              int multiline)
{
    if (!n->state_focused)
        return;
    const MiniStyle *s = &n->style;
    float ls = s->letter_set ? s->len_letter.v : 0.0f;
    int len = val ? (int)strlen(val) : 0;
    int off = n->caret_offset;
    if (off < 0)
        off = 0;
    if (off > len)
        off = len;
    while (off > 0 && (val[off] & 0xC0) == 0x80)
        off--; /* land on a codepoint boundary */

    float px = 0.0f;
    if (multiline)
    {
        /* caret is on the line containing `off`; x = width of that line's
           prefix up to the caret */
        int line_start = 0;
        for (int i = 0; i < off; i++)
            if (val[i] == '\n')
                line_start = i + 1;
        int col = off - line_start;
        px = caret_measure_prefix(val + line_start, col, fs, ls);
    }
    else
    {
        px = caret_measure_prefix(val, off, fs, ls);
    }

    float cx = x0 + px;
    if (cx < box_x + 1)
        cx = box_x + 1;
    if (cx > box_x + box_w - 1)
        cx = box_x + box_w - 1;

    /* In-field selection (Shift+arrows / Ctrl+A in a field): a blue rect
       between sel_anchor_off and the caret. Not blink-gated. Best-effort
       single-line rect (covers the common short selection). */
    if (n->sel_anchor_off >= 0 && n->sel_anchor_off != off)
    {
        int a = n->sel_anchor_off;
        if (a < 0)
            a = 0;
        if (a > len)
            a = len;
        int b = off;
        if (a > b)
        {
            int t = a;
            a = b;
            b = t;
        }
        float xa = x0 + caret_measure_prefix(val, a, fs, ls);
        float xb = x0 + caret_measure_prefix(val, b, fs, ls);
        if (xa > xb)
        {
            float t = xa;
            xa = xb;
            xb = t;
        }
        if (xa < box_x)
            xa = box_x;
        if (xb > box_x + box_w)
            xb = box_x + box_w;
        if (xb > xa)
            mini_draw_rect(r, xa, y0, xb - xa, fs,
                           0.10f, 0.45f, 0.96f, 0.5f);
    }

    /* ~0.53s on/off blink (1.9 cycles/sec -> half-cycle ~0.526s) */
    if (((int)(g_anim_time * 1.9) & 1))
        return; /* off phase */
    mini_draw_rect(r, cx, y0, 1.5f, fs, 0.05f, 0.05f, 0.05f, 1.0f);
}

/* Paint the page-text selection highlight for a text node: a blue rect per
   visual line over the selected byte range (mirrors draw_text_wrapped_ex's
   wrap + align). Drawn behind the glyphs (called before the main text draw). */
static void render_text_selection(struct MiniNode *n, MiniRenderer *r,
                                  const char *src, float draw_x, float draw_y,
                                  float wrap_w, float fs, float ls,
                                  float lh, int align)
{
    if (!g_render_events || !src || !*src)
        return;
    int lo, hi;
    if (!mini_events_node_selection_range(g_render_events, n, &lo, &hi))
        return;
    int starts[64], ends[64];
    float ws[64];
    int nl = mini_text_break_lines(src, wrap_w, fs, ls, starts, ends, ws, 64);
    float pen_y = 0.0f;
    for (int i = 0; i < nl; i++)
    {
        int ls2 = starts[i], le = ends[i];
        int a = lo, b = hi;
        if (a < ls2)
            a = ls2;
        if (b > le)
            b = le;
        if (a < b)
        {
            float lw = ws[i];
            float align_off = (align == 1)   ? (wrap_w - lw) * 0.5f
                              : (align == 2) ? (wrap_w - lw)
                                             : 0.0f;
            float xa = draw_x + align_off +
                       caret_measure_prefix(src + ls2, a - ls2, fs, ls);
            float xb = draw_x + align_off +
                       caret_measure_prefix(src + ls2, b - ls2, fs, ls);
            if (xb > xa)
                mini_draw_rect(r, xa, draw_y + pen_y, xb - xa, lh,
                               0.10f, 0.45f, 0.96f, 0.5f);
        }
        pen_y += lh;
    }
}

static void render_input(struct MiniNode *n, MiniRenderer *r)
{
    MiniStyle *s = &n->style;
    float x = s->abs_x, y = s->abs_y, w = s->w, h = s->h;
    if (h <= 0)
        h = 22;
    if (w <= 0)
        w = 140;
    const char *type = mini_node_get_attribute(n, "type");
    if (!type)
        type = "text";

    if (!strcmp(type, "checkbox"))
    {
        float sz = (w > 0 && w < 24) ? w : (h > 0 && h < 24 ? h : 16.0f);
        if (sz < 14.0f)
            sz = 16.0f;
        float oy = y + (h > sz ? (h - sz) * 0.5f : 0.0f);
        float radii[4] = {3.5f, 3.5f, 3.5f, 3.5f};
        int is_checked = (mini_node_get_attribute(n, "checked") != NULL);
        if (is_checked)
        {
            mini_draw_rect_rounded_corners(r, x, oy, sz, sz, radii, 0.13f, 0.77f, 0.37f, 1.0f);
            mini_draw_rect_rounded_corners_stroke(r, x, oy, sz, sz, radii, 1.0f, 0.20f, 0.83f, 0.60f, 1.0f);
            mini_draw_line(r, x + sz * 0.26f, oy + sz * 0.52f, x + sz * 0.44f, oy + sz * 0.72f, 1.8f, 1.0f, 1.0f, 1.0f, 1.0f);
            mini_draw_line(r, x + sz * 0.44f, oy + sz * 0.72f, x + sz * 0.76f, oy + sz * 0.28f, 1.8f, 1.0f, 1.0f, 1.0f, 1.0f);
        }
        else
        {
            mini_draw_rect_rounded_corners(r, x, oy, sz, sz, radii, 0.12f, 0.15f, 0.20f, 1.0f);
            mini_draw_rect_rounded_corners_stroke(r, x, oy, sz, sz, radii, 1.0f, 0.35f, 0.40f, 0.50f, 1.0f);
        }
        return;
    }
    if (!strcmp(type, "radio"))
    {
        float sz = h < 18 ? h : 18;
        float cxp = x + sz / 2, cyp = y + h / 2;
        mini_draw_circle(r, cxp, cyp, sz / 2, 0.4f, 0.4f, 0.4f, 1.0f);
        mini_draw_circle(r, cxp, cyp, sz / 2 - 2, 1.0f, 1.0f, 1.0f, 1.0f);
        if (mini_node_get_attribute(n, "checked") != NULL)
            mini_draw_circle(r, cxp, cyp, sz / 5, 0.2f, 0.5f, 0.85f, 1.0f);
        return;
    }
    if (!strcmp(type, "range"))
    {
        float ly = y + h / 2;
        mini_draw_line(r, x + 2, ly, x + w - 2, ly, 2, 0.5f, 0.5f, 0.5f, 1.0f);
        const char *v = mini_node_get_attribute(n, "value");
        const char *mn = mini_node_get_attribute(n, "min");
        const char *mx = mini_node_get_attribute(n, "max");
        float val = v ? (float)atof(v) : 50.0f;
        float lo = mn ? (float)atof(mn) : 0.0f, hi = mx ? (float)atof(mx) : 100.0f;
        if (hi <= lo)
            hi = lo + 1;
        float frac = (val - lo) / (hi - lo);
        if (frac < 0)
            frac = 0;
        if (frac > 1)
            frac = 1;
        mini_draw_circle(r, x + 2 + (w - 4) * frac, ly, 5, 0.2f, 0.5f, 0.85f, 1.0f);
        return;
    }
    if (!strcmp(type, "color"))
    {
        mini_draw_rect(r, x, y, w, h, 0.6f, 0.7f, 0.9f, 1.0f);
        mini_draw_rect_stroke(r, x, y, w, h, 1, 0.4f, 0.4f, 0.4f, 0.9f);
        return;
    }
    if (!strcmp(type, "file"))
    {
        mini_draw_rect(r, x, y, w, h, 0.88f, 0.88f, 0.9f, 1.0f);
        mini_draw_rect_stroke(r, x, y, w, h, 1, 0.45f, 0.45f, 0.45f, 0.9f);
        const char *lbl = "Choose File";
        float fs = 12.0f, tw = mini_text_measure(lbl, fs);
        mini_draw_text(r, x + 6, y + (h - fs) / 2, lbl, fs, 0.1f, 0.1f, 0.1f, 1.0f);
        (void)tw;
        return;
    }
    if (!strcmp(type, "submit") || !strcmp(type, "button") || !strcmp(type, "reset") ||
        !strcmp(type, "image"))
    {
        /* removed default opaque fill */
        mini_draw_rect_stroke(r, x, y, w, h, 1, 0.45f, 0.45f, 0.45f, 0.9f);
        const char *lbl = mini_node_get_attribute(n, "value");
        if (!lbl || !lbl[0])
            lbl = !strcmp(type, "submit")  ? "Submit"
                  : !strcmp(type, "reset") ? "Reset"
                                           : "Button";
        float fs = 12.0f, tw = mini_text_measure(lbl, fs);
        mini_draw_text(r, x + (w - tw) / 2, y + (h - fs) / 2, lbl, fs,
                       0.1f, 0.1f, 0.1f, 1.0f);
        return;
    }
    /* default: text / password / email / number / date / time / search / url / tel */
    int is_trans = (s->bg_a == 0.0f && mini_node_get_attribute(n, "style") && strstr(mini_node_get_attribute(n, "style"), "transparent"));
    if (!is_trans)
    {
        float br = (s->bg_a > 0) ? s->bg_r : 1.0f;
        float bg = (s->bg_a > 0) ? s->bg_g : 1.0f;
        float bb = (s->bg_a > 0) ? s->bg_b : 1.0f;
        float ba = (s->bg_a > 0) ? s->bg_a : 1.0f;
        mini_draw_rect(r, x, y, w, h, br, bg, bb, ba);
    }
    int no_border = (s->border_style[0] == 0 && s->has_border);
    if (!no_border && !is_trans)
    {
        float sr = s->has_border ? s->border_r : 0.4f;
        float sg = s->has_border ? s->border_g : 0.4f;
        float sb = s->has_border ? s->border_b : 0.4f;
        float sa = s->has_border ? s->border_a : 0.9f;
        mini_draw_rect_stroke(r, x, y, w, h, s->has_border ? s->border_w[0] : 1.0f, sr, sg, sb, sa);
    }
    const char *val = mini_node_get_attribute(n, "value");
    float fs = s->font_size > 0.0f ? s->font_size : 12.0f;
    if (val && val[0])
    {
        const char *disp = !strcmp(type, "password") ? "........" : val;
        draw_text_wrapped(r, disp, x + 5, y + (h - fs) / 2, w - 10, fs,
                          s->color_r, s->color_g, s->color_b, s->color_a, 0); /* 使用 CSS 真实颜色 */
    }
    else
    {
        const char *ph = mini_node_get_attribute(n, "placeholder");
        if (ph && ph[0])
        {
            float pr = 0.55f, pg = 0.55f, pb = 0.55f, pa = 0.85f;
            float pfs = fs;
            if (n->pseudo_placeholder)
            {
                const MiniStyle *ps = &n->pseudo_placeholder->style;
                if (ps->color_a > 0.0f)
                {
                    pr = ps->color_r; pg = ps->color_g; pb = ps->color_b; pa = ps->color_a;
                }
                if (ps->font_size > 0.0f) pfs = ps->font_size;
            }
            else if (s->color_a > 0.0f)
            {
                pr = s->color_r * 0.7f + 0.3f * 0.55f;
                pg = s->color_g * 0.7f + 0.3f * 0.55f;
                pb = s->color_b * 0.7f + 0.3f * 0.55f;
                pa = s->color_a * 0.60f;
            }
            draw_text_wrapped(r, ph, x + 5, y + (h - pfs) / 2, w - 10, pfs,
                              pr, pg, pb, pa, 0);
        }
    }
    /* caret: position over the actual value (password rendering is the
       fixed-dot stub above; its caret is best-effort on the value width) */
    render_text_caret(n, r, val ? val : "", x + 5, y + (h - fs) / 2,
                      fs, x, w, 0);
}

static void render_gauge(struct MiniNode *n, MiniRenderer *r, int is_progress)
{
    MiniStyle *s = &n->style;
    float x = s->abs_x, y = s->abs_y, w = s->w, h = s->h;
    if (h <= 0)
        h = 16;
    if (w <= 0)
        w = 120;
    mini_draw_rect(r, x, y, w, h, 0.9f, 0.9f, 0.9f, 1.0f);
    mini_draw_rect_stroke(r, x, y, w, h, 1, 0.4f, 0.4f, 0.4f, 0.9f);
    const char *v = mini_node_get_attribute(n, "value");
    const char *maxa = mini_node_get_attribute(n, "max");
    const char *mina = is_progress ? NULL : mini_node_get_attribute(n, "min");
    float val = v ? (float)atof(v) : 0.0f;
    float mx = maxa ? (float)atof(maxa) : (is_progress ? 100.0f : 1.0f);
    float mn = mina ? (float)atof(mina) : 0.0f;
    if (mx <= mn)
        mx = mn + 1;
    float frac = (val - mn) / (mx - mn);
    if (frac < 0)
        frac = 0;
    if (frac > 1)
        frac = 1;
    if (frac > 0)
        mini_draw_rect(r, x + 1, y + 1, (w - 2) * frac, h - 2,
                       0.2f, 0.5f, 0.85f, 1.0f);
}

static void render_form_control(struct MiniNode *n, MiniRenderer *r)
{
    const char *tag = n->tag;
    if (!tag)
        return;
    MiniStyle *s = &n->style;
    float x = s->abs_x, y = s->abs_y, w = s->w, h = s->h;
    if (!strcmp(tag, "form") || !strcmp(tag, "label") ||
        !strcmp(tag, "legend") || !strcmp(tag, "output"))
    {
        if (!strcmp(tag, "fieldset"))
            mini_draw_rect_stroke(r, x, y, w, h, 1, 0.4f, 0.4f, 0.4f, 0.9f);
        return; /* containers: bg already drawn; children render below */
    }
    if (!strcmp(tag, "option") || !strcmp(tag, "optgroup") ||
        !strcmp(tag, "datalist"))
        return; /* hidden — presented by their <select> host */
    if (!strcmp(tag, "input"))
    {
        render_input(n, r);
        return;
    }
    if (!strcmp(tag, "progress"))
    {
        render_gauge(n, r, 1);
        return;
    }
    if (!strcmp(tag, "meter"))
    {
        render_gauge(n, r, 0);
        return;
    }
    if (!strcmp(tag, "button"))
    {
        if (s->bg_a == 0.0f && !s->has_gradient)
        {
            /* removed default opaque fill */
            mini_draw_rect_stroke(r, x, y, w, h, 1, 0.45f, 0.45f, 0.45f, 0.9f);
        }
        return;
    }
    if (!strcmp(tag, "textarea"))
    {
        float br = (s->bg_a > 0) ? s->bg_r : 1.0f;
        float bg = (s->bg_a > 0) ? s->bg_g : 1.0f;
        float bb = (s->bg_a > 0) ? s->bg_b : 1.0f;
        float ba = (s->bg_a > 0) ? s->bg_a : 1.0f;
        mini_draw_rect(r, x, y, w, h, br, bg, bb, ba);
        float sr = s->has_border ? s->border_r : 0.4f;
        float sg = s->has_border ? s->border_g : 0.4f;
        float sb = s->has_border ? s->border_b : 0.4f;
        float sa = s->has_border ? s->border_a : 0.9f;
        mini_draw_rect_stroke(r, x, y, w, h, s->has_border ? s->border_w[0] : 1.0f, sr, sg, sb, sa);

        const char *tval = (n->text && n->text[0]) ? n->text : mini_node_get_attribute(n, "value");
        float fs = s->font_size > 0.0f ? s->font_size : 12.0f;
        if (tval && tval[0])
        {
            float cr = (s->color_a > 0) ? s->color_r : 0.1f;
            float cg = (s->color_a > 0) ? s->color_g : 0.1f;
            float cb = (s->color_a > 0) ? s->color_b : 0.1f;
            float ca = (s->color_a > 0) ? s->color_a : 1.0f;
            draw_text_wrapped(r, tval, x + 4, y + 4, w - 8, fs, cr, cg, cb, ca, 0);
            render_text_caret(n, r, tval, x + 4, y + 4, fs, x, w, 1);
        }
        else
        {
            const char *ph = mini_node_get_attribute(n, "placeholder");
            if (ph && ph[0])
            {
                float pr = 0.55f, pg = 0.55f, pb = 0.55f, pa = 0.85f;
                float pfs = fs;
                if (n->pseudo_placeholder)
                {
                    const MiniStyle *ps = &n->pseudo_placeholder->style;
                    if (ps->color_a > 0.0f)
                    {
                        pr = ps->color_r; pg = ps->color_g; pb = ps->color_b; pa = ps->color_a;
                    }
                    if (ps->font_size > 0.0f) pfs = ps->font_size;
                }
                else if (s->color_a > 0.0f)
                {
                    pr = s->color_r * 0.7f + 0.3f * 0.55f;
                    pg = s->color_g * 0.7f + 0.3f * 0.55f;
                    pb = s->color_b * 0.7f + 0.3f * 0.55f;
                    pa = s->color_a * 0.60f;
                }
                draw_text_wrapped(r, ph, x + 4, y + 4, w - 8, pfs, pr, pg, pb, pa, 0);
            }
            render_text_caret(n, r, "", x + 4, y + 4, fs, x, w, 1);
        }
        return;
    }
    if (!strcmp(tag, "select"))
    {
        mini_draw_rect(r, x, y, w, h, 1, 1, 1, 1);
        mini_draw_rect_stroke(r, x, y, w, h, 1, 0.4f, 0.4f, 0.4f, 0.9f);
        float ax = x + w - 10, ay = y + h / 2;
        mini_draw_triangle(r, ax - 4, ay - 3, ax + 4, ay - 3, ax, ay + 3,
                           0.3f, 0.3f, 0.3f, 1.0f);
        const char *txt = select_selected_text(n);
        if (txt && txt[0])
            draw_text_wrapped(r, txt, x + 6, y + (h - 12) / 2, w - 20, 12,
                              0.1f, 0.1f, 0.1f, 1.0f, 0);
        return;
    }
}

/* ===================== SVG vector rendering ======================== */
/* An <svg> container is rasterized as vector shapes (rect/circle/ellipse/
   line/polyline/polygon/path) using the existing GL primitives. Points are
   mapped through an affine transform (viewBox + per-shape transform), so
   translate/scale/rotate are honored. Fills use a triangle fan (convex
   approximation); strokes use line segments.                          */

static SvgXform svg_id(void)
{
    SvgXform m = {1, 0, 0, 1, 0, 0};
    return m;
}
static void svg_map(SvgXform m, float x, float y, float *ox, float *oy)
{
    *ox = m.a * x + m.c * y + m.e;
    *oy = m.b * x + m.d * y + m.f;
}
/* p ∘ q (apply q first, then p) */
static SvgXform svg_mul(SvgXform p, SvgXform q)
{
    SvgXform r;
    r.a = p.a * q.a + p.c * q.b;
    r.b = p.b * q.a + p.d * q.b;
    r.c = p.a * q.c + p.c * q.d;
    r.d = p.b * q.c + p.d * q.d;
    r.e = p.a * q.e + p.c * q.f + p.e;
    r.f = p.b * q.e + p.d * q.f + p.f;
    return r;
}

/* parse "translate(x,y) scale(s) rotate(a[,cx,cy]) matrix(...)" chains */
static SvgXform svg_parse_xform(const char *t)
{
    SvgXform m = svg_id();
    if (!t)
        return m;
    const char *p = t;
    while (*p)
    {
        while (*p && (*p == ' ' || *p == ','))
            p++;
        char fn[16];
        int fi = 0;
        while (*p && *p != '(' && fi < 15)
            fn[fi++] = (char)tolower((unsigned char)*p++);
        fn[fi] = 0;
        float args[6] = {0, 0, 0, 0, 0, 0};
        int na = 0;
        if (*p == '(')
        {
            p++;
            while (*p && *p != ')' && na < 6)
            {
                while (*p && (*p == ' ' || *p == ','))
                    p++;
                char *e;
                float v = (float)strtod(p, &e);
                if (e == p)
                {
                    p++;
                    continue;
                }
                args[na++] = v;
                p = e;
            }
            if (*p == ')')
                p++;
        }
        if (!strcmp(fn, "translate"))
        {
            SvgXform t2 = {1, 0, 0, 1, args[0], na > 1 ? args[1] : args[0]};
            m = svg_mul(m, t2);
        }
        else if (!strcmp(fn, "scale"))
        {
            float sx = args[0], sy = na > 1 ? args[1] : args[0];
            SvgXform t2 = {sx, 0, 0, sy, 0, 0};
            m = svg_mul(m, t2);
        }
        else if (!strcmp(fn, "rotate"))
        {
            float rad = args[0] * 0.0174532925f;
            float cs = (float)cos(rad), sn = (float)sin(rad);
            SvgXform t2 = {cs, sn, -sn, cs, 0, 0};
            if (na >= 3)
            {
                float cx = args[1], cy = args[2];
                SvgXform a1 = {1, 0, 0, 1, cx, cy}, a3 = {1, 0, 0, 1, -cx, -cy};
                m = svg_mul(m, svg_mul(a1, svg_mul(t2, a3)));
            }
            else
                m = svg_mul(m, t2);
        }
        else if (!strcmp(fn, "matrix") && na >= 6)
        {
            SvgXform t2 = {args[0], args[1], args[2], args[3], args[4], args[5]};
            m = svg_mul(m, t2);
        }
    }
    return m;
}

static float svg_attr_f(struct MiniNode *n, const char *name, float def)
{
    const char *v = mini_node_get_attribute(n, name);
    return v ? (float)atof(v) : def;
}

static const char *svg_inherited_attr(struct MiniNode *n, const char *name)
{
    while (n && n->type == MN_ELEMENT_NODE)
    {
        const char *v = mini_node_get_attribute(n, name);
        if (v)
            return v;
        if (n->tag && !strcmp(n->tag, "svg"))
            break;
        n = n->parent;
    }
    return NULL;
}

static struct MiniNode *find_node_by_id_rec(struct MiniNode *n, const char *id)
{
    if (!n || !id)
        return NULL;
    const char *nid = mini_node_get_attribute(n, "id");
    if (nid && !strcmp(nid, id))
        return n;
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
    {
        struct MiniNode *res = find_node_by_id_rec(c, id);
        if (res)
            return res;
    }
    if (n->shadow_root)
    {
        struct MiniNode *res = find_node_by_id_rec(n->shadow_root, id);
        if (res)
            return res;
    }
    return NULL;
}

struct MiniNode *mini_dom_get_element_by_id(struct MiniDocument *doc, const char *id)
{
    if (!doc || !doc->root || !id)
        return NULL;
    return find_node_by_id_rec(doc->root, id);
}

static void svg_parse_defs(struct MiniNode *root)
{
    if (!root)
        return;
    MiniDocumentContext *ctx = mini_get_ctx(g_active_doc);
    for (struct MiniNode *c = root->first_child; c; c = c->next_sibling)
    {
        if (c->type != MN_ELEMENT_NODE || !c->tag)
            continue;
        if (ci_eq(c->tag, "lineargradient") || ci_eq(c->tag, "radialgradient"))
        {
            const char *id = mini_node_get_attribute(c, "id");
            if (id && id[0])
            {
                SvgGradient *g = (SvgGradient *)calloc(1, sizeof(SvgGradient));
                if (g)
                {
                    strncpy(g->id, id, sizeof(g->id) - 1);
                    g->type = ci_eq(c->tag, "lineargradient") ? 0 : 1;
                    g->x1 = svg_attr_f(c, "x1", 0.0f);
                    g->y1 = svg_attr_f(c, "y1", 0.0f);
                    g->x2 = svg_attr_f(c, "x2", 1.0f);
                    g->y2 = svg_attr_f(c, "y2", 0.0f);
                    g->cx = svg_attr_f(c, "cx", 0.5f);
                    g->cy = svg_attr_f(c, "cy", 0.5f);
                    g->r = svg_attr_f(c, "r", 0.5f);

                    for (struct MiniNode *sc = c->first_child; sc && g->num_stops < 16; sc = sc->next_sibling)
                    {
                        if (sc->type == MN_ELEMENT_NODE && sc->tag && !strcmp(sc->tag, "stop"))
                        {
                            const char *off_str = mini_node_get_attribute(sc, "offset");
                            float off = off_str ? (float)atof(off_str) : 0.0f;
                            if (off_str && strchr(off_str, '%'))
                                off /= 100.0f;

                            const char *scol = mini_node_get_attribute(sc, "stop-color");
                            const char *sopa = mini_node_get_attribute(sc, "stop-opacity");
                            float sr = 0, sg = 0, sb = 0, sa = 1;
                            if (scol)
                                mini_parse_color(scol, &sr, &sg, &sb, &sa);
                            if (sopa)
                                sa *= (float)atof(sopa);

                            g->stops[g->num_stops].offset = off;
                            g->stops[g->num_stops].r = sr;
                            g->stops[g->num_stops].g = sg;
                            g->stops[g->num_stops].b = sb;
                            g->stops[g->num_stops].a = sa;
                            g->num_stops++;
                        }
                    }
                    g->next = ctx->svg_defs.gradients;
                    ctx->svg_defs.gradients = g;
                }
            }
        }
        svg_parse_defs(c);
    }
}

/* resolve a paint (fill/stroke). returns 1 if paintable, 0 if none. */
static int svg_paint(struct MiniNode *n, const char *v, float *r, float *g, float *b, float *a,
                     int def_black)
{
    if (!v || !v[0])
    {
        if (def_black)
        {
            *r = *g = *b = 0.0f;
            *a = 1.0f;
            return 1;
        }
        return 0;
    }
    if (!strcmp(v, "none") || !strcmp(v, "transparent"))
        return 0;

    if (ci_eq(v, "currentcolor"))
    {
        for (struct MiniNode *cur = n; cur; cur = cur->parent)
        {
            if (cur->style.color_a > 0.001f)
            {
                *r = cur->style.color_r;
                *g = cur->style.color_g;
                *b = cur->style.color_b;
                *a = cur->style.color_a;
                return 1;
            }
        }
        *r = 1.0f;
        *g = 1.0f;
        *b = 1.0f;
        *a = 1.0f;
        return 1;
    }

    if (!strncmp(v, "url(", 4))
    {
        const char *p = v + 4;
        while (*p && (isspace((unsigned char)*p) || *p == '\'' || *p == '\"' || *p == '#'))
            p++;
        char id[64] = {0};
        int idx = 0;
        while (*p && *p != ')' && *p != '\'' && *p != '\"' && idx < 63)
            id[idx++] = *p++;
        id[idx] = 0;
        MiniDocumentContext *ctx = mini_get_ctx(g_active_doc);
        for (SvgGradient *sg = ctx->svg_defs.gradients; sg; sg = sg->next)
        {
            if (!strcmp(sg->id, id))
            {
                if (sg->num_stops > 0)
                {
                    *r = sg->stops[0].r;
                    *g = sg->stops[0].g;
                    *b = sg->stops[0].b;
                    *a = sg->stops[0].a;
                }
                return 1;
            }
        }
        return 0;
    }

    if (parse_hex_color(v, r, g, b, a))
        return 1;

    if (mini_parse_color(v, r, g, b, a))
        return 1;

    if (def_black)
    {
        *r = *g = *b = 0.0f;
        *a = 1.0f;
        return 1;
    }
    return 0;
}

/* 判断点是否在三角形内部 */
static int point_in_tri(float px, float py, float ax, float ay, float bx, float by, float cx, float cy)
{
    float cp1 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
    float cp2 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
    float cp3 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
    int has_neg = (cp1 < -1e-4f) || (cp2 < -1e-4f) || (cp3 < -1e-4f);
    int has_pos = (cp1 > 1e-4f) || (cp2 > 1e-4f) || (cp3 > 1e-4f);
    return !(has_neg && has_pos);
}

/* 耳切法三角剖分（Ear Clipping）：支持任意凹多边形、星形及复杂多边形绘制 */
static void svg_draw_concave_polygon(MiniRenderer *r, const float *px, const float *py, int n,
                                     float fr, float fg, float fb, float fa)
{
    if (n < 3)
        return;
    if (n == 3)
    {
        mini_draw_triangle(r, px[0], py[0], px[1], py[1], px[2], py[2], fr, fg, fb, fa);
        return;
    }

    int idx[256];
    if (n > 256)
        n = 256;
    for (int i = 0; i < n; i++)
        idx[i] = i;

    /* 计算有向面积判断绕向 */
    float area = 0.0f;
    for (int i = 0; i < n; i++)
    {
        int next = (i + 1) % n;
        area += (px[i] * py[next] - px[next] * py[i]);
    }
    int ccw = (area > 0.0f);

    int nv = n;
    int guard = 2 * nv;
    for (int m = 0; nv > 2 && guard > 0;)
    {
        guard--;
        int u = (m > 0) ? m - 1 : nv - 1;
        int v = m;
        int w = (m + 1) % nv;

        float ax = px[idx[u]], ay = py[idx[u]];
        float bx = px[idx[v]], by = py[idx[v]];
        float cx = px[idx[w]], cy = py[idx[w]];

        float cp = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        int is_convex = ccw ? (cp > 1e-4f) : (cp < -1e-4f);

        int is_ear = 0;
        if (is_convex)
        {
            is_ear = 1;
            for (int p = 0; p < nv; p++)
            {
                if (p == u || p == v || p == w)
                    continue;
                if (point_in_tri(px[idx[p]], py[idx[p]], ax, ay, bx, by, cx, cy))
                {
                    is_ear = 0;
                    break;
                }
            }
        }

        if (is_ear)
        {
            mini_draw_triangle(r, ax, ay, bx, by, cx, cy, fr, fg, fb, fa);
            for (int s = v; s < nv - 1; s++)
                idx[s] = idx[s + 1];
            nv--;
            guard = 2 * nv;
            if (m >= nv)
                m = 0;
        }
        else
        {
            m = (m + 1) % nv;
        }
    }
    if (nv == 3)
    {
        mini_draw_triangle(r, px[idx[0]], py[idx[0]], px[idx[1]], py[idx[1]], px[idx[2]], py[idx[2]], fr, fg, fb, fa);
    }
}

/* draw a polygon/polyline given already-transformed screen points. */
static void svg_poly(MiniRenderer *r, const float *px, const float *py, int n,
                     int closed, int fill, float fr, float fg, float fb, float fa,
                     int stroke, float sr, float sg, float sb, float sa, float sw)
{
    if (n < 2)
        return;

    /* 1. 采用耳切法填充凹凸多边形 */
    if (fill && n >= 3)
        svg_draw_concave_polygon(r, px, py, n, fr, fg, fb, fa);

    /* 2. 线段三角面片光栅化描边 */
    if (stroke && sw > 0.0f)
    {
        float hw = sw * 0.5f;
        int count = closed ? n : (n - 1);
        for (int i = 0; i < count; i++)
        {
            int next_i = (i + 1) % n;
            float x1 = px[i], y1 = py[i];
            float x2 = px[next_i], y2 = py[next_i];
            float dx = x2 - x1, dy = y2 - y1;
            float len = sqrtf(dx * dx + dy * dy);
            if (len > 1e-4f)
            {
                float nx = -dy / len * hw;
                float ny = dx / len * hw;
                float ax = x1 + nx, ay = y1 + ny;
                float bx = x1 - nx, by = y1 - ny;
                float cx = x2 + nx, cy = y2 + ny;
                float dx_pt = x2 - nx, dy_pt = y2 - ny;
                mini_draw_triangle(r, ax, ay, bx, by, cx, cy, sr, sg, sb, sa);
                mini_draw_triangle(r, bx, by, dx_pt, dy_pt, cx, cy, sr, sg, sb, sa);
            }
            /* 架构级修复：在线段顶点处绘制小圆，自动生成完美的 Round Join 和 Round Cap */
            mini_draw_circle(r, x1, y1, hw, sr, sg, sb, sa);
        }
        /* 若为非闭合路径，需单独为终点补齐线帽 */
        if (!closed && n > 0)
        {
            mini_draw_circle(r, px[n - 1], py[n - 1], hw, sr, sg, sb, sa);
        }
    }
}

static const char *svg_num(const char *p, float *out)
{
    while (*p && (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    if (!*p)
        return NULL;
    char *e;
    float v = (float)strtod(p, &e);
    if (e == p)
        return NULL;
    *out = v;
    return e;
}

static const char *svg_flag(const char *p, float *out)
{
    while (*p && (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    if (*p == '0' || *p == '1')
    {
        *out = *p - '0';
        return p + 1;
    }
    return NULL;
}

/* parse a <path d="..."> into subpaths and stroke/fill each. */
static void svg_draw_path(struct MiniNode *n, MiniRenderer *r, SvgXform m,
                          int fill, float fr, float fg, float fb, float fa,
                          int stroke, float sr, float sg, float sb, float sa, float sw)
{
    const char *d = mini_node_get_attribute(n, "d");
    if (!d)
        return;
    float px[512], py[512];
    int np = 0;
    float curx = 0, cury = 0, sx = 0, sy = 0;
    float last_cx = 0, last_cy = 0; /* 用于 C 和 S 指令的平滑反射点 */
    char cmd = 0, prev_cmd = 0;
    const char *p = d;

    while (p && *p)
    {
        while (*p && (isspace((unsigned char)*p) || *p == ','))
            p++;
        if (!*p)
            break;
        if (isalpha((unsigned char)*p))
        {
            prev_cmd = cmd;
            cmd = (char)*p++;
        }
        if (!cmd)
            break;

        int rel = islower((unsigned char)cmd);
        char C = (char)toupper((unsigned char)cmd);

        if (C == 'M' || C == 'L')
        {
            float x, y;
            const char *np1, *np2;
            while ((np1 = svg_num(p, &x)) && (np2 = svg_num(np1, &y)))
            {
                p = np2;
                if (rel)
                {
                    x += curx;
                    y += cury;
                }
                if (C == 'M' && np > 0)
                {
                    svg_poly(r, px, py, np, 0, fill, fr, fg, fb, fa, stroke, sr, sg, sb, sa, sw);
                    np = 0;
                }
                if (np < 512)
                {
                    svg_map(m, x, y, &px[np], &py[np]);
                    np++;
                }
                curx = x;
                cury = y;

                /* 架构级修复 2：防止 M 指令多坐标时引发的点阵塌陷 */
                if (C == 'M')
                {
                    C = 'L';
                    sx = x;
                    sy = y;
                }
                if (cmd == 'M')
                    cmd = 'L';
                if (cmd == 'm')
                    cmd = 'l';
                prev_cmd = cmd;
            }
        }
        else if (C == 'H')
        {
            float x;
            const char *np1;
            while ((np1 = svg_num(p, &x)))
            {
                p = np1;
                if (rel)
                    x += curx;
                curx = x;
                if (np < 512)
                {
                    svg_map(m, x, cury, &px[np], &py[np]);
                    np++;
                }
                prev_cmd = cmd;
            }
        }
        else if (C == 'V')
        {
            float y;
            const char *np1;
            while ((np1 = svg_num(p, &y)))
            {
                p = np1;
                if (rel)
                    y += cury;
                cury = y;
                if (np < 512)
                {
                    svg_map(m, curx, y, &px[np], &py[np]);
                    np++;
                }
                prev_cmd = cmd;
            }
        }
        else if (C == 'Z')
        {
            svg_poly(r, px, py, np, 1, fill, fr, fg, fb, fa, stroke, sr, sg, sb, sa, sw);
            np = 0;
            curx = sx;
            cury = sy;
            prev_cmd = cmd;
        }
        else if (C == 'C')
        {
            float x1, y1, x2, y2, x, y;
            const char *np1, *np2, *np3, *np4, *np5, *np6;
            while ((np1 = svg_num(p, &x1)) && (np2 = svg_num(np1, &y1)) &&
                   (np3 = svg_num(np2, &x2)) && (np4 = svg_num(np3, &y2)) &&
                   (np5 = svg_num(np4, &x)) && (np6 = svg_num(np5, &y)))
            {
                p = np6;
                if (rel)
                {
                    x1 += curx;
                    y1 += cury;
                    x2 += curx;
                    y2 += cury;
                    x += curx;
                    y += cury;
                }
                for (int i = 1; i <= 8 && np < 512; i++)
                {
                    float t = i / 8.0f, mt = 1.0f - t;
                    float bx = mt * mt * mt * curx + 3 * mt * mt * t * x1 + 3 * mt * t * t * x2 + t * t * t * x;
                    float by = mt * mt * mt * cury + 3 * mt * mt * t * y1 + 3 * mt * t * t * y2 + t * t * t * y;
                    svg_map(m, bx, by, &px[np], &py[np]);
                    np++;
                }
                curx = x;
                cury = y;
                last_cx = x2;
                last_cy = y2;
                prev_cmd = cmd;
            }
        }
        /* 架构级修复 3：补全缺失的 S 指令 (Smooth Cubic Bezier)，盾牌曲线涅槃重生 */
        else if (C == 'S')
        {
            float x2, y2, x, y;
            const char *np1, *np2, *np3, *np4;
            while ((np1 = svg_num(p, &x2)) && (np2 = svg_num(np1, &y2)) &&
                   (np3 = svg_num(np2, &x)) && (np4 = svg_num(np3, &y)))
            {
                p = np4;
                if (rel)
                {
                    x2 += curx;
                    y2 += cury;
                    x += curx;
                    y += cury;
                }

                float x1 = curx, y1 = cury;
                char pC = (char)toupper((unsigned char)prev_cmd);
                if (pC == 'C' || pC == 'S')
                {
                    x1 = 2 * curx - last_cx;
                    y1 = 2 * cury - last_cy;
                }

                for (int i = 1; i <= 8 && np < 512; i++)
                {
                    float t = i / 8.0f, mt = 1.0f - t;
                    float bx = mt * mt * mt * curx + 3 * mt * mt * t * x1 + 3 * mt * t * t * x2 + t * t * t * x;
                    float by = mt * mt * mt * cury + 3 * mt * mt * t * y1 + 3 * mt * t * t * y2 + t * t * t * y;
                    svg_map(m, bx, by, &px[np], &py[np]);
                    np++;
                }
                curx = x;
                cury = y;
                last_cx = x2;
                last_cy = y2;
                prev_cmd = cmd;
            }
        }
        else if (C == 'Q')
        {
            float qx, qy, x, y;
            const char *np1, *np2, *np3, *np4;
            while ((np1 = svg_num(p, &qx)) && (np2 = svg_num(np1, &qy)) &&
                   (np3 = svg_num(np2, &x)) && (np4 = svg_num(np3, &y)))
            {
                p = np4;
                if (rel)
                {
                    qx += curx;
                    qy += cury;
                    x += curx;
                    y += cury;
                }
                for (int i = 1; i <= 8 && np < 512; i++)
                {
                    float t = i / 8.0f, mt = 1.0f - t;
                    float bx = mt * mt * curx + 2 * mt * t * qx + t * t * x;
                    float by = mt * mt * cury + 2 * mt * t * qy + t * t * y;
                    svg_map(m, bx, by, &px[np], &py[np]);
                    np++;
                }
                curx = x;
                cury = y;
                last_cx = qx;
                last_cy = qy;
                prev_cmd = cmd;
            }
        }
        else if (C == 'T')
        {
            float x, y;
            const char *np1, *np2;
            while ((np1 = svg_num(p, &x)) && (np2 = svg_num(np1, &y)))
            {
                p = np2;
                if (rel)
                {
                    x += curx;
                    y += cury;
                }
                float qx = curx, qy = cury;
                char pC = (char)toupper((unsigned char)prev_cmd);
                if (pC == 'Q' || pC == 'T')
                {
                    qx = 2 * curx - last_cx;
                    qy = 2 * cury - last_cy;
                }
                for (int i = 1; i <= 8 && np < 512; i++)
                {
                    float t = i / 8.0f, mt = 1.0f - t;
                    float bx = mt * mt * curx + 2 * mt * t * qx + t * t * x;
                    float by = mt * mt * cury + 2 * mt * t * qy + t * t * y;
                    svg_map(m, bx, by, &px[np], &py[np]);
                    np++;
                }
                curx = x;
                cury = y;
                last_cx = qx;
                last_cy = qy;
                prev_cmd = cmd;
            }
        }
        else if (C == 'A')
        {
            float rx, ry, rot, laf, sf, x, y;
            const char *n1, *n2, *n3, *n4, *n5, *n6, *n7;
            /* 严格使用 svg_flag 解析 laf 和 sf */
            while ((n1 = svg_num(p, &rx)) && (n2 = svg_num(n1, &ry)) &&
                   (n3 = svg_num(n2, &rot)) && (n4 = svg_flag(n3, &laf)) &&
                   (n5 = svg_flag(n4, &sf)) && (n6 = svg_num(n5, &x)) &&
                   (n7 = svg_num(n6, &y)))
            {
                p = n7;
                if (rel)
                {
                    x += curx;
                    y += cury;
                }

                if (rx < 0)
                    rx = -rx;
                if (ry < 0)
                    ry = -ry;
                if (rx == 0.0f || ry == 0.0f)
                {
                    if (np < 512)
                    {
                        svg_map(m, x, y, &px[np], &py[np]);
                        np++;
                    }
                    curx = x;
                    cury = y;
                    prev_cmd = cmd;
                    continue;
                }

                float rad = rot * 3.14159265f / 180.0f;
                float cos_r = cosf(rad), sin_r = sinf(rad);
                float dx2 = (curx - x) / 2.0f;
                float dy2 = (cury - y) / 2.0f;
                float x1 = cos_r * dx2 + sin_r * dy2;
                float y1 = -sin_r * dx2 + cos_r * dy2;

                float prx = rx * rx;
                float pry = ry * ry;
                float px1 = x1 * x1;
                float py1 = y1 * y1;

                float check = px1 / prx + py1 / pry;
                if (check > 1.0f)
                {
                    rx = sqrtf(check) * rx;
                    ry = sqrtf(check) * ry;
                    prx = rx * rx;
                    pry = ry * ry;
                }

                float sign = (laf == sf) ? -1.0f : 1.0f;
                float sq = ((prx * pry) - (prx * py1) - (pry * px1)) / ((prx * py1) + (pry * px1));
                sq = (sq < 0) ? 0 : sq;
                float coef = sign * sqrtf(sq);
                float cx1 = coef * ((rx * y1) / ry);
            float cy1 = coef * (-(ry * x1) / rx);

                float cx = cos_r * cx1 - sin_r * cy1 + (curx + x) / 2.0f;
                float cy = sin_r * cx1 + cos_r * cy1 + (cury + y) / 2.0f;

                float ux = (x1 - cx1) / rx;
                float uy = (y1 - cy1) / ry;
                float vx = (-x1 - cx1) / rx;
                float vy = (-y1 - cy1) / ry;

                float start_ang = atan2f(uy, ux);
                float dp = ux * vx + uy * vy;
                dp = (dp < -1.0f) ? -1.0f : ((dp > 1.0f) ? 1.0f : dp);
                float sweep_ang = acosf(dp);

                if (ux * vy - uy * vx < 0)
                    sweep_ang = -sweep_ang;
                if (sf == 0 && sweep_ang > 0)
                    sweep_ang -= 2.0f * 3.14159265f;
                else if (sf == 1 && sweep_ang < 0)
                    sweep_ang += 2.0f * 3.14159265f;

                int segs = (int)(fabsf(sweep_ang) * 12.0f);
                if (segs < 4)
                    segs = 4;
                if (segs > 32)
                    segs = 32;

                for (int i = 1; i <= segs && np < 512; i++)
                {
                    float a = start_ang + sweep_ang * ((float)i / (float)segs);
                    float e_x = cx + rx * cosf(a) * cos_r - ry * sinf(a) * sin_r;
                    float e_y = cy + rx * cosf(a) * sin_r + ry * sinf(a) * cos_r;
                    svg_map(m, e_x, e_y, &px[np], &py[np]);
                    np++;
                }
                curx = x;
                cury = y;
                prev_cmd = cmd;
            }
        }
        else
        {
            while (*p && !isalpha((unsigned char)*p))
                p++;
            cmd = 0;
        }
    }
    if (np >= 2)
        svg_poly(r, px, py, np, 0, fill, fr, fg, fb, fa, stroke, sr, sg, sb, sa, sw);
}
static void svg_draw_group(struct MiniNode *parent, MiniRenderer *r, SvgXform m);

static void svg_draw_shape(struct MiniNode *n, MiniRenderer *r, SvgXform m)
{
    const char *tag = n->tag;
    if (!tag)
        return;

    const char *fv = svg_inherited_attr(n, "fill");
    const char *sv = svg_inherited_attr(n, "stroke");
    float fr = 0, fg = 0, fb = 0, fa = 1, sr = 0, sg = 0, sb = 0, sa = 1;

    int fill = svg_paint(n, fv, &fr, &fg, &fb, &fa, 1);

    const char *sw_str = svg_inherited_attr(n, "stroke-width");
    float sw = sw_str ? (float)atof(sw_str) : 1.0f;
    if (sw < 1.0f)
        sw = 1.0f;

    int stroke = svg_paint(n, sv, &sr, &sg, &sb, &sa, 0);

    float px[512], py[512];
    int np = 0, closed = 0;

    if (!strcmp(tag, "rect"))
    {
        float x = svg_attr_f(n, "x", 0), y = svg_attr_f(n, "y", 0);
        float w = svg_attr_f(n, "width", 0), h = svg_attr_f(n, "height", 0);
        float X[4] = {x, x + w, x + w, x}, Y[4] = {y, y, y + h, y + h};
        for (int i = 0; i < 4 && np < 256; i++)
        {
            svg_map(m, X[i], Y[i], &px[np], &py[np]);
            np++;
        }
        closed = 1;
    }
    else if (!strcmp(tag, "circle") || !strcmp(tag, "ellipse"))
    {
        float cx = svg_attr_f(n, "cx", 0), cy = svg_attr_f(n, "cy", 0);
        float rx, ry;
        if (!strcmp(tag, "circle"))
        {
            float rr = svg_attr_f(n, "r", 0);
            rx = ry = rr;
        }
        else
        {
            rx = svg_attr_f(n, "rx", 0);
            ry = svg_attr_f(n, "ry", 0);
        }
        int seg = 32;
        for (int i = 0; i < seg && np < 256; i++)
        {
            float a = (float)(i * 2 * 3.14159265358979 / seg);
            svg_map(m, cx + rx * (float)cos(a), cy + ry * (float)sin(a),
                    &px[np], &py[np]);
            np++;
        }
        closed = 1;
    }
    else if (!strcmp(tag, "line"))
    {
        svg_map(m, svg_attr_f(n, "x1", 0), svg_attr_f(n, "y1", 0), &px[0], &py[0]);
        svg_map(m, svg_attr_f(n, "x2", 0), svg_attr_f(n, "y2", 0), &px[1], &py[1]);
        np = 2;
        fill = 0;
    }
    else if (!strcmp(tag, "polyline") || !strcmp(tag, "polygon"))
    {
        const char *pts = mini_node_get_attribute(n, "points");
        if (pts)
        {
            const char *pp = pts;
            while (*pp && np < 256)
            {
                float x, y;
                pp = svg_num(pp, &x);
                if (!pp)
                    break;
                const char *pp2 = svg_num(pp, &y);
                if (!pp2)
                    y = 0;
                else
                    pp = pp2;
                svg_map(m, x, y, &px[np], &py[np]);
                np++;
            }
        }
        closed = !strcmp(tag, "polygon");
        if (!closed)
            fill = 0;
    }
    else if (!strcmp(tag, "path"))
    {
        svg_draw_path(n, r, m, fill, fr, fg, fb, fa, stroke, sr, sg, sb, sa, sw);
        return;
    }
    else if (!strcmp(tag, "g"))
    {
        svg_draw_group(n, r, m);
        return;
    }
    else if (!strcmp(tag, "defs"))
    {
        return;
    }
    else if (!strcmp(tag, "use"))
    {
        const char *href = mini_node_get_attribute(n, "href");
        if (!href)
            href = mini_node_get_attribute(n, "xlink:href");
        if (href && href[0] == '#')
        {
            const char *id = href + 1;
            struct MiniNode *target = g_active_doc ? mini_dom_get_element_by_id(g_active_doc, id) : NULL;
            if (!target)
            {
                struct MiniNode *root_svg = n;
                while (root_svg->parent && root_svg->tag && strcmp(root_svg->tag, "svg"))
                    root_svg = root_svg->parent;
                target = find_node_by_id_rec(root_svg, id);
            }
            if (target && target != n)
            {
                float ux = svg_attr_f(n, "x", 0);
                float uy = svg_attr_f(n, "y", 0);
                SvgXform use_m = svg_id();
                use_m.e = ux;
                use_m.f = uy;
                SvgXform cm = svg_mul(m, use_m);
                cm = svg_mul(cm, svg_parse_xform(mini_node_get_attribute(n, "transform")));
                if (target->tag && !strcmp(target->tag, "symbol"))
                {
                    float sw = svg_attr_f(n, "width", 0);
                    float sh = svg_attr_f(n, "height", 0);
                    const char *svb = mini_node_get_attribute(target, "viewBox");
                    if (svb && sw > 0 && sh > 0)
                    {
                        float vx = 0, vy = 0, vw = sw, vh = sh;
                        sscanf(svb, "%f %f %f %f", &vx, &vy, &vw, &vh);
                        if (vw > 0 && vh > 0)
                        {
                            SvgXform sym_m = svg_id();
                            sym_m.a = sw / vw;
                            sym_m.d = sh / vh;
                            sym_m.e = -vx * sym_m.a;
                            sym_m.f = -vy * sym_m.d;
                            cm = svg_mul(cm, sym_m);
                        }
                    }
                    svg_draw_group(target, r, cm);
                }
                else if (target->tag && !strcmp(target->tag, "g"))
                {
                    svg_draw_group(target, r, cm);
                }
                else
                {
                    svg_draw_shape(target, r, cm);
                }
            }
        }
        return;
    }
    else if (!strcmp(tag, "text") || !strcmp(tag, "tspan"))
    {
        float tx = svg_attr_f(n, "x", 0);
        float ty = svg_attr_f(n, "y", 0);
        float tdx = svg_attr_f(n, "dx", 0);
        float tdy = svg_attr_f(n, "dy", 0);
        tx += tdx;
        ty += tdy;

        const char *anchor = svg_inherited_attr(n, "text-anchor");
        const char *fs_attr = svg_inherited_attr(n, "font-size");
        float fs = fs_attr ? (float)atof(fs_attr) : (n->style.font_size > 0 ? n->style.font_size : 16.0f);
        if (fs <= 0.0f)
            fs = 16.0f;

        char text_buf[512] = {0};
        size_t toff = 0;
        collect_text_content(n, text_buf, sizeof(text_buf), &toff);

        if (toff > 0)
        {
            float tw = mini_text_measure(text_buf, fs);
            if (anchor)
            {
                if (!strcmp(anchor, "middle"))
                    tx -= tw / 2.0f;
                else if (!strcmp(anchor, "end"))
                    tx -= tw;
            }
            float sx, sy;
            svg_map(m, tx, ty - fs * 0.8f, &sx, &sy);
            if (fill)
                mini_draw_text(r, sx, sy, text_buf, fs, fr, fg, fb, fa);
            else if (stroke)
                mini_draw_text(r, sx, sy, text_buf, fs, sr, sg, sb, sa);
        }
        return;
    }
    else
        return;

    svg_poly(r, px, py, np, closed, fill, fr, fg, fb, fa,
             stroke, sr, sg, sb, sa, sw);
}

static void svg_draw_group(struct MiniNode *parent, MiniRenderer *r, SvgXform m)
{
    for (struct MiniNode *c = parent->first_child; c; c = c->next_sibling)
    {
        if (c->type != MN_ELEMENT_NODE)
            continue;
        SvgXform cm = svg_mul(m, svg_parse_xform(mini_node_get_attribute(c, "transform")));
        svg_draw_shape(c, r, cm);
    }
}

/* render an <svg> element's vector content. */
static void render_svg(struct MiniNode *svg, MiniRenderer *r)
{
    MiniStyle *s = &svg->style;
    float ox = s->abs_x, oy = s->abs_y, bw = s->w, bh = s->h;
    if (bw <= 0 || bh <= 0)
        return;

    svg_parse_defs(svg);

    float vbx = 0, vby = 0, vbw = bw, vbh = bh;
    const char *vb = mini_node_get_attribute(svg, "viewBox");
    if (!vb)
        vb = mini_node_get_attribute(svg, "viewbox");
    if (vb)
    {
        char vbb[64];
        size_t k = 0;
        for (const char *q = vb; *q && k < sizeof vbb - 1; q++)
            vbb[k++] = (*q == ',') ? ' ' : *q;
        vbb[k] = 0;
        if (sscanf(vbb, "%f %f %f %f", &vbx, &vby, &vbw, &vbh) < 4)
            vbw = bw, vbh = bh;
    }
    if (vbw <= 0)
        vbw = bw;
    if (vbh <= 0)
        vbh = bh;

    const char *par = mini_node_get_attribute(svg, "preserveAspectRatio");
    float scale_x = bw / vbw;
    float scale_y = bh / vbh;
    float trans_x = -vbx * scale_x;
    float trans_y = -vby * scale_y;

    if (par && strstr(par, "none"))
    {
        /* Non-uniform scaling */
    }
    else
    {
        int is_slice = (par && strstr(par, "slice"));
        float uniform_scale = is_slice ? ((scale_x > scale_y) ? scale_x : scale_y)
                                       : ((scale_x < scale_y) ? scale_x : scale_y);
        scale_x = scale_y = uniform_scale;
        float fitted_w = vbw * uniform_scale;
        float fitted_h = vbh * uniform_scale;
        float align_x = 0.5f, align_y = 0.5f;
        if (par)
        {
            if (strstr(par, "xMin"))
                align_x = 0.0f;
            else if (strstr(par, "xMax"))
                align_x = 1.0f;
            if (strstr(par, "YMin"))
                align_y = 0.0f;
            else if (strstr(par, "YMax"))
                align_y = 1.0f;
        }
        trans_x = -vbx * uniform_scale + (bw - fitted_w) * align_x;
        trans_y = -vby * uniform_scale + (bh - fitted_h) * align_y;
    }

    SvgXform root = {scale_x, 0, 0, scale_y, ox + trans_x, oy + trans_y};
    root = svg_mul(root, svg_parse_xform(mini_node_get_attribute(svg, "transform")));
    svg_draw_group(svg, r, root);
}

/* ================== 2D canvas recording (replayed at render) ========= */
/* Single active 2D context (single-threaded). The JS bridge records shape
   commands during requestAnimationFrame; render_media replays them at the
   canvas element's z-position (so 2D content paints behind higher-z
   siblings) and clears. Points are baked through the 2D transform at
   record time; replay just adds the canvas origin. 1-frame latency.    */

enum
{
    G2D_CIRCLE,
    G2D_RECT,
    G2D_LINE,
    G2D_TRI,
    G2D_TEXT
};

static void g2d_push(int kind, float x, float y, float x2, float y2,
                     float w, float h, float rr, float cr, float cg, float cb,
                     float ca, float lw)
{
    if (!g2d)
        return; /* 确保指针非空 */
    if (g2d_n < G2D_CAP)
    {
        struct G2dCmd *c = &g2d[g2d_n++];
        c->kind = kind;
        c->x = x;
        c->y = y;
        c->x2 = x2;
        c->y2 = y2;
        c->w = w;
        c->h = h;
        c->r = rr;
        c->cr = cr;
        c->cg = cg;
        c->cb = cb;
        c->ca = ca;
        c->lw = lw;
        c->text = NULL;
        c->fs = 0.0f;
        c->maxw = 0.0f;
    }
}
static void g2d_pt(float x, float y) /* bake the current 2D transform */
{
    if (g2d_pn < 8192)
    {
        g2d_px[g2d_pn] = (float)(g2d_m[0] * x + g2d_m[2] * y + g2d_m[4]);
        g2d_py[g2d_pn] = (float)(g2d_m[1] * x + g2d_m[3] * y + g2d_m[5]);
        g2d_pn++;
    }
    g2d_pen_x = x;
    g2d_pen_y = y;
}

void mini_2d_get_pen(float *x, float *y)
{
    if (x)
        *x = g2d_pen_x;
    if (y)
        *y = g2d_pen_y;
}

void mini_2d_reset(void)
{
    /* free any text strings owned by pending commands (reset clears the
       buffer without replaying, so we own the free here). */
    for (int i = 0; i < g2d_n; i++)
        if (g2d[i].text)
        {
            free(g2d[i].text);
            g2d[i].text = NULL;
        }
    g2d_n = 0;
    g2d_pn = 0;
    g2d_num_subs = 0;
    g2d_closed = 0;
}
void mini_2d_begin_path(void)
{
    g2d_pn = 0;
    g2d_num_subs = 0;
    g2d_closed = 0;
}
void mini_2d_close_path(void) { g2d_closed = 1; }
void mini_2d_line_to(float x, float y)
{
    if (g2d_num_subs == 0 && g2d_num_subs < 512)
        g2d_subpaths[g2d_num_subs++] = g2d_pn;
    g2d_pt(x, y);
}
void mini_2d_arc(float cx, float cy, float r, float a0, float a1, int ccw)
{
    if (r < 0)
        r = 0;
    int seg = 24;
    float step = (a1 - a0) / seg;
    if (ccw)
        step = -step;
    if (g2d_num_subs == 0 && g2d_num_subs < 512)
        g2d_subpaths[g2d_num_subs++] = g2d_pn;
    g2d_pt(cx + r * cosf(a0), cy + r * sinf(a0));
    for (int i = 1; i <= seg; i++)
    {
        float a = a0 + step * i;
        g2d_pt(cx + r * cosf(a), cy + r * sinf(a));
    }
}
void mini_2d_fill(float r, float g, float b, float a)
{
    int num_sub = g2d_num_subs;
    if (num_sub == 0 && g2d_pn >= 3)
    {
        for (int i = 1; i < g2d_pn - 1; i++)
            g2d_push(G2D_TRI, g2d_px[0], g2d_py[0], g2d_px[i], g2d_py[i],
                     g2d_px[i + 1], g2d_py[i + 1], 0, r, g, b, a, 0);
        return;
    }
    for (int k = 0; k < num_sub; k++)
    {
        int start = g2d_subpaths[k];
        int end = (k + 1 < num_sub) ? g2d_subpaths[k + 1] : g2d_pn;
        int n = end - start;
        if (n < 3)
            continue;
        for (int i = start + 1; i < end - 1; i++)
            g2d_push(G2D_TRI, g2d_px[start], g2d_py[start], g2d_px[i], g2d_py[i],
                     g2d_px[i + 1], g2d_py[i + 1], 0, r, g, b, a, 0);
    }
}
void mini_2d_stroke(float r, float g, float b, float a, float w)
{
    if (w < 1.0f)
        w = 1.0f;
    int num_sub = g2d_num_subs;
    if (num_sub == 0 && g2d_pn > 0)
    {
        for (int i = 0; i < g2d_pn - 1; i++)
            g2d_push(G2D_LINE, g2d_px[i], g2d_py[i], g2d_px[i + 1], g2d_py[i + 1],
                     0, 0, 0, r, g, b, a, w);
        if (g2d_closed && g2d_pn >= 2)
            g2d_push(G2D_LINE, g2d_px[g2d_pn - 1], g2d_py[g2d_pn - 1], g2d_px[0], g2d_py[0],
                     0, 0, 0, r, g, b, a, w);
        return;
    }
    for (int k = 0; k < num_sub; k++)
    {
        int start = g2d_subpaths[k];
        int end = (k + 1 < num_sub) ? g2d_subpaths[k + 1] : g2d_pn;
        for (int i = start; i < end - 1; i++)
        {
            g2d_push(G2D_LINE, g2d_px[i], g2d_py[i], g2d_px[i + 1], g2d_py[i + 1],
                     0, 0, 0, r, g, b, a, w);
        }
        if (g2d_closed && (end - start) >= 2)
        {
            g2d_push(G2D_LINE, g2d_px[end - 1], g2d_py[end - 1], g2d_px[start], g2d_py[start],
                     0, 0, 0, r, g, b, a, w);
        }
    }
}
void mini_2d_fill_rect(float x, float y, float w, float h, float cr, float cg,
                       float cb, float ca)
{
    /* 4 transformed corners → 2 triangles (handles transforms) */
    float x0 = (float)(g2d_m[0] * x + g2d_m[2] * y + g2d_m[4]);
    float y0 = (float)(g2d_m[1] * x + g2d_m[3] * y + g2d_m[5]);
    float x1 = (float)(g2d_m[0] * (x + w) + g2d_m[2] * y + g2d_m[4]);
    float y1 = (float)(g2d_m[1] * (x + w) + g2d_m[3] * y + g2d_m[5]);
    float x2 = (float)(g2d_m[0] * (x + w) + g2d_m[2] * (y + h) + g2d_m[4]);
    float y2 = (float)(g2d_m[1] * (x + w) + g2d_m[3] * (y + h) + g2d_m[5]);
    float x3 = (float)(g2d_m[0] * x + g2d_m[2] * (y + h) + g2d_m[4]);
    float y3 = (float)(g2d_m[1] * x + g2d_m[3] * (y + h) + g2d_m[5]);
    g2d_push(G2D_TRI, x0, y0, x1, y1, x2, y2, 0, cr, cg, cb, ca, 0);
    g2d_push(G2D_TRI, x0, y0, x2, y2, x3, y3, 0, cr, cg, cb, ca, 0);
}

/* ---- text selection helpers (shared by render highlight + hit-test) ---- */
/* Collapse runs of ASCII whitespace into single spaces, mirroring the
   renderer's text-node pre-pass. is_pre (white-space:pre) copies verbatim.   */
void mini_text_collapse(const char *src, char *dst, size_t cap, int is_pre)
{
    if (!dst || cap == 0)
        return;
    if (!src)
    {
        dst[0] = 0;
        return;
    }
    if (is_pre)
    {
        size_t i = 0;
        for (; src[i] && i + 1 < cap; i++)
            dst[i] = src[i];
        dst[i] = 0;
        return;
    }
    size_t ci = 0;
    int in_ws = 0;
    for (const char *s = src; *s && ci + 1 < cap; s++)
    {
        if (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        {
            if (!in_ws)
            {
                dst[ci++] = ' ';
                in_ws = 1;
            }
        }
        else
        {
            dst[ci++] = *s;
            in_ws = 0;
        }
    }
    dst[ci] = 0;
}

/* Greedy word-wrap line breaks mirroring draw_text_wrapped_ex's segmentation:
   each "word" is a space, a multibyte cluster, or a run of non-space ASCII.
   \n forces a break. Fills per-line [start,end) byte offsets and pixel width
   up to max_lines. Returns the line count. safe margin = wrap_w + 4.        */
int mini_text_break_lines(const char *text, float wrap_w, float fs,
                          float ls, int *out_start, int *out_end,
                          float *out_w, int max_lines)
{
    if (!text || !*text || max_lines <= 0)
        return 0;
    if (wrap_w <= 0)
        wrap_w = 1e9f;
    float safe_max_w = wrap_w + 4.0f;
    int n_lines = 0;
    const char *p = text;
    int line_len = 0;
    float line_w = 0.0f;
    int line_start = 0;
    char word_buf[128];

    while (*p && n_lines < max_lines)
    {
        if (*p == '\n' || *p == '\r')
        {
            const char *np = p;
            if (*np == '\r' && np[1] == '\n')
                np++;
            np++;
            if (out_start)
                out_start[n_lines] = line_start;
            if (out_end)
                out_end[n_lines] = line_start + line_len;
            if (out_w)
                out_w[n_lines] = line_w;
            n_lines++;
            line_start = (int)(np - text);
            line_len = 0;
            line_w = 0.0f;
            p = np;
            continue;
        }

        const char *word_start = p;
        int word_bytes = 0;
        unsigned char c0 = (unsigned char)*p;
        if (isspace(c0))
        {
            word_bytes = 1;
            p++;
        }
        else if (c0 >= 0x80)
        {
            int clen = 1;
            if (c0 >= 0xF0)
                clen = 4;
            else if (c0 >= 0xE0)
                clen = 3;
            else if (c0 >= 0xC0)
                clen = 2;
            for (int i = 0; i < clen && *p; i++)
            {
                word_bytes++;
                p++;
            }
        }
        else
        {
            while (*p && !isspace((unsigned char)*p) && (unsigned char)*p < 0x80)
            {
                word_bytes++;
                p++;
            }
            if (word_bytes == 0)
            {
                p++;
                continue;
            }
        }

        if (word_bytes >= (int)sizeof(word_buf))
            word_bytes = sizeof(word_buf) - 1;
        memcpy(word_buf, word_start, word_bytes);
        word_buf[word_bytes] = 0;
        float word_w = mini_text_measure_ex(word_buf, fs, ls);

        if (line_w > 0.0f && (line_w + word_w > safe_max_w) &&
            !isspace((unsigned char)word_buf[0]))
        {
            if (out_start)
                out_start[n_lines] = line_start;
            if (out_end)
                out_end[n_lines] = line_start + line_len;
            if (out_w)
                out_w[n_lines] = line_w;
            n_lines++;
            if (n_lines >= max_lines)
                break;
            line_start = line_start + line_len;
            line_len = 0;
            line_w = 0.0f;
        }

        if (line_len + word_bytes < (int)sizeof(word_buf) - 1)
        {
            line_len += word_bytes;
            line_w += word_w;
        }
    }
    if (line_len > 0 && n_lines < max_lines)
    {
        if (out_start)
            out_start[n_lines] = line_start;
        if (out_end)
            out_end[n_lines] = line_start + line_len;
        if (out_w)
            out_w[n_lines] = line_w;
        n_lines++;
    }
    return n_lines;
}

/* Produce the exact string + geometry the renderer paints for a text node,
   so the hit-test and the selection highlight use the same src_text/box as
   the glyphs. Mirrors render_node's MN_TEXT_NODE setup (collapse + uppercase
   + font/wrap/align/line-height). Must stay in sync with that path.         */
int mini_dom_text_layout(const struct MiniNode *n, char *collapsed, size_t cap,
                         float *out_fs, float *out_ls, float *out_lh,
                         float *out_draw_x, float *out_draw_y,
                         float *out_wrap_w, int *out_align)
{
    if (!collapsed || cap == 0)
        return 0;
    collapsed[0] = 0;
    if (!n || n->type != MN_TEXT_NODE || !n->text || !n->text[0])
        return 0;
    struct MiniNode *p = n->parent;
    int is_pre = (p && p->style.white_space == 1);
    char tmp[1024];
    mini_text_collapse(n->text, tmp, sizeof tmp, is_pre);
    if (p && p->style.text_transform == 1) /* uppercase (offset-preserving) */
    {
        size_t tl = strlen(tmp);
        if (tl >= sizeof(tmp))
            tl = sizeof(tmp) - 1;
        for (size_t i = 0; i < tl; i++)
        {
            unsigned char c = (unsigned char)tmp[i];
            tmp[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : (char)c;
        }
        tmp[tl] = 0;
    }
    size_t cl = strlen(tmp);
    if (cl + 1 > cap)
        cl = cap - 1;
    memcpy(collapsed, tmp, cl);
    collapsed[cl] = 0;

    float fs = (p && p->style.font_size > 0) ? p->style.font_size
                                             : (n->style.font_size > 0 ? n->style.font_size : 16.0f);
    float ls = p ? (p->style.letter_set ? p->style.len_letter.v : 0.0f)
                 : (n->style.letter_set ? n->style.len_letter.v : 0.0f);
    float lh = mini_text_line_height(fs);
    if (p && p->style.line_height_set)
    {
        if (p->style.len_line_height.unit == 0 && p->style.len_line_height.v > 0 &&
            p->style.len_line_height.v <= 5.0f)
            lh = fs * p->style.len_line_height.v;
        else
            lh = p->style.len_line_height.v;
    }

    int align = p ? (p->style.text_align ? p->style.text_align : n->style.text_align)
                  : n->style.text_align;
    float draw_x = n->style.abs_x;
    float draw_y = n->style.abs_y;
    float wrap_w = (n->style.w > 0.0f) ? n->style.w : 10000.0f;
    if (align != 0 && p && p->style.w > 0.0f)
    {
        wrap_w = p->style.w - p->style.padding[1] - p->style.padding[3];
        if (wrap_w < 0.0f)
            wrap_w = 0.0f;
        draw_x = p->style.abs_x + p->style.padding[3];
    }
    if (draw_x == 0.0f && draw_y == 0.0f && p)
    {
        draw_x = p->style.abs_x + p->style.padding[3];
        draw_y = p->style.abs_y + p->style.padding[0];
    }

    if (out_fs)
        *out_fs = fs;
    if (out_ls)
        *out_ls = ls;
    if (out_lh)
        *out_lh = lh;
    if (out_draw_x)
        *out_draw_x = draw_x;
    if (out_draw_y)
        *out_draw_y = draw_y;
    if (out_wrap_w)
        *out_wrap_w = wrap_w;
    if (out_align)
        *out_align = align;
    return (int)cl;
}

static void draw_text_wrapped_ex(MiniRenderer *r, const char *text,
                                 float x, float y, float max_w, float fs,
                                 float cr, float cg, float cb, float ca, int align, float ls, float lh, int font_weight, int font_style)
{
    if (!r || !text || !*text || fs <= 0)
        return;
    if (ca <= 0.0f)
        ca = 1.0f;
    if (max_w <= 0)
        max_w = 1e9f;

    float safe_max_w = max_w + 4.0f;
    float pen_y = 0.0f;
    float v_off = (lh > fs) ? (lh - fs) * 0.5f : 0.0f;
    const char *p = text;
    char line_buf[1024] = {0};
    int line_len = 0;
    float line_w = 0.0f;

    while (*p)
    {
        if (*p == '\n' || *p == '\r')
        {
            if (*p == '\r' && p[1] == '\n') p++;
            p++;
            line_buf[line_len] = '\0';
            float draw_x = x;
            if (align == 1) draw_x = x + (max_w - line_w) * 0.5f;
            else if (align == 2) draw_x = x + (max_w - line_w);
            draw_x = floorf(draw_x + 0.5f);

            if (line_len > 0)
            {
                mini_draw_text_styled(r, draw_x, floorf(y + pen_y + v_off + 0.5f), line_buf, fs, cr, cg, cb, ca, ls, font_style);
                if (font_weight >= 600)
                {
                    /* 消除小数偏移干扰：加粗偏移量必须被强制限定为纯整数，防止亚像素计算把1像素塌陷吃回原点 */
                    float b_off = (font_weight >= 800) ? 2.0f : 1.0f;
                    mini_draw_text_styled(r, draw_x + b_off, floorf(y + pen_y + v_off + 0.5f), line_buf, fs, cr, cg, cb, ca, ls, font_style);
                }
            }
            pen_y += lh;
            line_len = 0;
            line_w = 0.0f;
            continue;
        }

        const char *word_start = p;
        int word_bytes = 0;
        unsigned char c0 = (unsigned char)*p;

        if (isspace(c0)) { word_bytes = 1; p++; }
        else if (c0 >= 0x80)
        {
            int clen = 1;
            if (c0 >= 0xF0) clen = 4;
            else if (c0 >= 0xE0) clen = 3;
            else if (c0 >= 0xC0) clen = 2;
            for (int i = 0; i < clen && *p; i++) { word_bytes++; p++; }
        }
        else
        {
            while (*p && !isspace((unsigned char)*p) && (unsigned char)*p < 0x80) { word_bytes++; p++; }
            if (word_bytes == 0) { p++; continue; }
        }

        char word_buf[128];
        if (word_bytes >= (int)sizeof(word_buf))
            word_bytes = sizeof(word_buf) - 1;
        memcpy(word_buf, word_start, word_bytes);
        word_buf[word_bytes] = '\0';

        float word_w = mini_text_measure_ex(word_buf, fs, ls);

        if (word_w > safe_max_w && word_bytes > 1)
        {
            const char *wp = word_start;
            const char *w_end = word_start + word_bytes;
            while (wp < w_end)
            {
                unsigned char uc = (unsigned char)*wp;
                int ulen = 1;
                if (uc >= 0xF0) ulen = 4;
                else if (uc >= 0xE0) ulen = 3;
                else if (uc >= 0xC0) ulen = 2;
                if (wp + ulen > w_end) ulen = (int)(w_end - wp);

                char char_buf[8] = {0};
                memcpy(char_buf, wp, ulen);
                char_buf[ulen] = '\0';
                float char_w = mini_text_measure_ex(char_buf, fs, ls);

                if (line_w > 0.0f && (line_w + char_w > safe_max_w))
                {
                    line_buf[line_len] = '\0';
                    float draw_x = x;
                    if (align == 1) draw_x = x + (max_w - line_w) * 0.5f;
                    else if (align == 2) draw_x = x + (max_w - line_w);
                    draw_x = floorf(draw_x + 0.5f);

                    mini_draw_text_styled(r, draw_x, floorf(y + pen_y + v_off + 0.5f), line_buf, fs, cr, cg, cb, ca, ls, font_style);
                    if (font_weight >= 600)
                    {
                        float b_off = (font_weight >= 800) ? 2.0f : 1.0f;
                        mini_draw_text_styled(r, draw_x + b_off, floorf(y + pen_y + v_off + 0.5f), line_buf, fs, cr, cg, cb, ca, ls, font_style);
                    }
                    pen_y += lh;
                    line_len = 0;
                    line_w = 0.0f;
                }
                if (line_len + ulen < (int)sizeof(line_buf) - 1)
                {
                    memcpy(line_buf + line_len, char_buf, ulen);
                    line_len += ulen;
                    line_w += char_w;
                }
                wp += ulen;
            }
            continue;
        }

        if (line_w > 0.0f && (line_w + word_w > safe_max_w) && !isspace((unsigned char)word_buf[0]))
        {
            line_buf[line_len] = '\0';
            float draw_x = x;
            if (align == 1) draw_x = x + (max_w - line_w) * 0.5f;
            else if (align == 2) draw_x = x + (max_w - line_w);
            draw_x = floorf(draw_x + 0.5f);

            mini_draw_text_styled(r, draw_x, floorf(y + pen_y + v_off + 0.5f), line_buf, fs, cr, cg, cb, ca, ls, font_style);
            if (font_weight >= 600)
            {
                float b_off = (font_weight >= 800) ? 2.0f : 1.0f;
                mini_draw_text_styled(r, draw_x + b_off, floorf(y + pen_y + v_off + 0.5f), line_buf, fs, cr, cg, cb, ca, ls, font_style);
            }
            pen_y += lh;
            line_len = 0;
            line_w = 0.0f;
        }

        if (line_len + word_bytes < (int)sizeof(line_buf) - 1)
        {
            memcpy(line_buf + line_len, word_buf, word_bytes);
            line_len += word_bytes;
            line_w += word_w;
        }
    }

    if (line_len > 0)
    {
        line_buf[line_len] = '\0';
        float draw_x = x;
        if (align == 1) draw_x = x + (max_w - line_w) * 0.5f;
        else if (align == 2) draw_x = x + (max_w - line_w);
        draw_x = floorf(draw_x + 0.5f);

        mini_draw_text_styled(r, draw_x, floorf(y + pen_y + v_off + 0.5f), line_buf, fs, cr, cg, cb, ca, ls, font_style);
        if (font_weight >= 600)
        {
            float b_off = (font_weight >= 800) ? 2.0f : 1.0f;
            mini_draw_text_styled(r, draw_x + b_off, floorf(y + pen_y + v_off + 0.5f), line_buf, fs, cr, cg, cb, ca, ls, font_style);
        }
    }
}

void mini_2d_clear_rect(float x, float y, float w, float h)
{ /* the per-frame full clear already wipes last frame; within-frame there's
     nothing to erase, so clearRect is a no-op for this immediate-record model. */
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}
/* strokeRect: 4 transformed edges → 4 line commands (does NOT touch the
   current path, matching W3C semantics). */
void mini_2d_stroke_rect(float x, float y, float w, float h,
                         float cr, float cg, float cb, float ca, float lw)
{
    float x0 = (float)(g2d_m[0] * x + g2d_m[2] * y + g2d_m[4]);
    float y0 = (float)(g2d_m[1] * x + g2d_m[3] * y + g2d_m[5]);
    float x1 = (float)(g2d_m[0] * (x + w) + g2d_m[2] * y + g2d_m[4]);
    float y1 = (float)(g2d_m[1] * (x + w) + g2d_m[3] * y + g2d_m[5]);
    float x2 = (float)(g2d_m[0] * (x + w) + g2d_m[2] * (y + h) + g2d_m[4]);
    float y2 = (float)(g2d_m[1] * (x + w) + g2d_m[3] * (y + h) + g2d_m[5]);
    float x3 = (float)(g2d_m[0] * x + g2d_m[2] * (y + h) + g2d_m[4]);
    float y3 = (float)(g2d_m[1] * x + g2d_m[3] * (y + h) + g2d_m[5]);
    g2d_push(G2D_LINE, x0, y0, x1, y1, 0, 0, 0, cr, cg, cb, ca, lw);
    g2d_push(G2D_LINE, x1, y1, x2, y2, 0, 0, 0, cr, cg, cb, ca, lw);
    g2d_push(G2D_LINE, x2, y2, x3, y3, 0, 0, 0, cr, cg, cb, ca, lw);
    g2d_push(G2D_LINE, x3, y3, x0, y0, 0, 0, 0, cr, cg, cb, ca, lw);
}
void mini_2d_save(void)
{
    if (g2d_sp < 16)
    {
        for (int i = 0; i < 6; i++)
            g2d_stk[g2d_sp][i] = g2d_m[i];
        g2d_sp++;
    }
}
void mini_2d_restore(void)
{
    if (g2d_sp > 0)
    {
        g2d_sp--;
        for (int i = 0; i < 6; i++)
            g2d_m[i] = g2d_stk[g2d_sp][i];
    }
}
void mini_2d_translate(float x, float y)
{
    g2d_m[4] += g2d_m[0] * x + g2d_m[2] * y;
    g2d_m[5] += g2d_m[1] * x + g2d_m[3] * y;
}
void mini_2d_scale(float sx, float sy)
{
    g2d_m[0] *= sx;
    g2d_m[1] *= sx;
    g2d_m[2] *= sy;
    g2d_m[3] *= sy;
}
void mini_2d_rotate(float rad)
{
    float c = cosf(rad), s = sinf(rad);
    double m0 = g2d_m[0], m1 = g2d_m[1], m2 = g2d_m[2], m3 = g2d_m[3];
    g2d_m[0] = m0 * c + m2 * s;
    g2d_m[1] = m1 * c + m3 * s;
    g2d_m[2] = -m0 * s + m2 * c;
    g2d_m[3] = -m1 * s + m3 * c;
}

/* ---- Stage 3: text / transforms / hit-test / measure ---- */

/* set font: parse "<size>px <family>" (also tolerates "italic bold 16px ...").
   Stores the size for fillText/measureText. */
void mini_2d_set_font(const char *font_str)
{
    if (!font_str)
        return;
    /* find the token ending in "px" */
    const char *p = font_str;
    while (*p)
    {
        char *e;
        float v = (float)strtod(p, &e);
        if (e != p)
        {
            const char *q = e;
            while (*q && isspace((unsigned char)*q))
                q++;
            if (q[0] == 'p' && q[1] == 'x')
            {
                g2d_font_size = v > 0 ? v : 16.0f;
                return;
            }
        }
        p = e + 1;
    }
}

/* record a text draw (bakes the transform; replay calls mini_draw_text). */
static void g2d_push_text(const char *text, float x, float y, float maxw,
                          float cr, float cg, float cb, float ca)
{
    if (g2d_n >= G2D_CAP)
        return;
    struct G2dCmd *c = &g2d[g2d_n++];
    c->kind = G2D_TEXT;
    c->x = (float)(g2d_m[0] * x + g2d_m[2] * y + g2d_m[4]);
    c->y = (float)(g2d_m[1] * x + g2d_m[3] * y + g2d_m[5]);
    c->fs = g2d_font_size;
    c->maxw = maxw;
    c->cr = cr;
    c->cg = cg;
    c->cb = cb;
    c->ca = ca;
    c->text = mini_dup(text ? text : "");
    c->x2 = c->y2 = c->w = c->h = c->r = 0;
    c->lw = 0;
}
void mini_2d_fill_text(const char *text, float x, float y, float maxw,
                       float r, float g, float b, float a)
{
    g2d_push_text(text, x, y, maxw, r, g, b, a);
}
void mini_2d_stroke_text(const char *text, float x, float y, float maxw,
                         float r, float g, float b, float a)
{
    /* no stroked-glyph path; render as a filled outline (best-effort). */
    g2d_push_text(text, x, y, maxw, r, g, b, a);
}
void mini_2d_measure_text(const char *text, float *out_w)
{
    if (out_w)
        *out_w = mini_text_measure(text ? text : "", g2d_font_size);
}

/* matrix ops: g2d_m = g2d_m ∘ [a b c d e f] (apply arg first). */
void mini_2d_transform(float a, float b, float c, float d, float e, float f)
{
    double m0 = g2d_m[0], m1 = g2d_m[1], m2 = g2d_m[2], m3 = g2d_m[3];
    double m4 = g2d_m[4], m5 = g2d_m[5];
    g2d_m[0] = a * m0 + c * m1;
    g2d_m[1] = b * m0 + d * m1;
    g2d_m[2] = a * m2 + c * m3;
    g2d_m[3] = b * m2 + d * m3;
    g2d_m[4] = a * m4 + c * m5 + e;
    g2d_m[5] = b * m4 + d * m5 + f;
}
void mini_2d_set_transform(float a, float b, float c, float d, float e, float f)
{
    g2d_m[0] = a;
    g2d_m[1] = b;
    g2d_m[2] = c;
    g2d_m[3] = d;
    g2d_m[4] = e;
    g2d_m[5] = f;
}
void mini_2d_reset_transform(void)
{
    g2d_m[0] = 1;
    g2d_m[1] = 0;
    g2d_m[2] = 0;
    g2d_m[3] = 1;
    g2d_m[4] = 0;
    g2d_m[5] = 0;
}

/* point-in-polygon (ray cast) over the current path (already baked through
   the transform). The test point is transformed the same way first. */
void mini_2d_is_point_in_path(float x, float y, int *out)
{
    if (out)
        *out = 0;
    float px = (float)(g2d_m[0] * x + g2d_m[2] * y + g2d_m[4]);
    float py = (float)(g2d_m[1] * x + g2d_m[3] * y + g2d_m[5]);
    int n = g2d_pn;
    if (n < 3)
        return;
    int inside = 0;
    for (int i = 0, j = n - 1; i < n; j = i++)
    {
        float xi = g2d_px[i], yi = g2d_py[i];
        float xj = g2d_px[j], yj = g2d_py[j];
        int c = ((yi > py) != (yj > py)) &&
                (px < (xj - xi) * (py - yi) / (yj - yi) + xi);
        if (c)
            inside = !inside;
    }
    if (out)
        *out = inside ? 1 : 0;
}
void mini_2d_move_to(float x, float y)
{
    g2d_pen_x = x;
    g2d_pen_y = y;
    if (g2d_num_subs < 512)
    {
        g2d_subpaths[g2d_num_subs++] = g2d_pn;
    }
    g2d_pt(x, y);
}

void mini_2d_replay(struct MiniRenderer *r, struct MiniNode *cv)
{
    if (!r || !cv)
        return;
    float ox = cv->style.abs_x, oy = cv->style.abs_y;
    float cw = cv->style.w > 0 ? cv->style.w : (g_lctx.vw > 0 ? g_lctx.vw : 300.0f);
    float ch = cv->style.h > 0 ? cv->style.h : (g_lctx.vh > 0 ? g_lctx.vh : 150.0f);

    float op = (cv->style.has_opacity) ? cv->style.opacity : 1.0f;

    mini_renderer_push_clip(r, ox, oy, cw, ch);

    for (int i = 0; i < g2d_n; i++)
    {
        struct G2dCmd *c = &g2d[i];
        float alpha = c->ca * op;

        switch (c->kind)
        {
        case G2D_CIRCLE:
            mini_draw_circle(r, ox + c->x, oy + c->y, c->r, c->cr, c->cg, c->cb, alpha);
            break;
        case G2D_RECT:
            mini_draw_rect(r, ox + c->x, oy + c->y, c->w, c->h, c->cr, c->cg, c->cb, alpha);
            break;
        case G2D_LINE:
            mini_draw_line(r, ox + c->x, oy + c->y, ox + c->x2, oy + c->y2,
                           c->lw, c->cr, c->cg, c->cb, alpha);
            break;
        case G2D_TRI:
            mini_draw_triangle(r, ox + c->x, oy + c->y, ox + c->x2, oy + c->y2,
                               ox + c->w, oy + c->h, c->cr, c->cg, c->cb, alpha);
            break;
        case G2D_TEXT:
            mini_draw_text(r, ox + c->x, oy + c->y, c->text ? c->text : "",
                           c->fs, c->cr, c->cg, c->cb, alpha);
            break;
        }
    }

    mini_renderer_pop_clip(r);

    /* 架构修复：不再在回放结束时消费/清空 2D 命令缓冲。
       原实现每帧回放后把 g2d_n 置 0 并 free 文本，导致同步绘制一次（非 rAF）
       的 canvas 内容在下一帧被 DOM 背景清除后永久消失——静态 canvas 画不出
       任何东西。浏览器中 canvas backing store 是持久的。现在保留命令，每帧
       DOM 渲染都会重新回放，静态内容得以持久。rAF 动画的清理由
       mini_bridge_fire_raf 在确有 rAF 回调时调用 mini_2d_reset() 完成，
       与原"每帧重新记录"语义一致。多 canvas 共享同一全局缓冲是既有局限。 */
}
static void render_media(struct MiniNode *n, MiniRenderer *r)
{
    const char *tag = n->tag;
    if (!tag)
        return;
    if (!strcmp(tag, "source") || !strcmp(tag, "track") ||
        !strcmp(tag, "param") || !strcmp(tag, "area"))
        return; /* metadata children of media hosts — no own box */
    if (!strcmp(tag, "svg"))
    {
        render_svg(n, r); /* real vector content, not a placeholder */
        return;
    }
    if (!strcmp(tag, "canvas"))
    {
        /* 2D canvas content recorded by the JS bridge (rAF) is replayed here
           at the canvas's z-position; WebGL canvases paint themselves via
           the GL bridge and leave the 2D buffer empty.                       */
        mini_2d_replay(r, n);
        return;
    }
    MiniStyle *s = &n->style;
    float x = s->abs_x, y = s->abs_y, w = s->w, h = s->h;
    if (w <= 0 || h <= 0)
        return;
    // if (strcmp(tag, "canvas") != 0)
    //{
    //     mini_draw_rect(r, x, y, w, h, 0.78f, 0.80f, 0.82f, 1.0f);
    //     mini_draw_rect_stroke(r, x, y, w, h, 1, 0.4f, 0.4f, 0.4f, 0.9f);
    // }
    mini_draw_rect(r, x, y, w, h, 0.78f, 0.80f, 0.82f, 1.0f);
    mini_draw_rect_stroke(r, x, y, w, h, 1, 0.4f, 0.4f, 0.4f, 0.9f);
    if (!strcmp(tag, "canvas") || !strcmp(tag, "svg") || !strcmp(tag, "iframe") ||
        !strcmp(tag, "object") || !strcmp(tag, "embed") || !strcmp(tag, "picture") ||
        !strcmp(tag, "map") || !strcmp(tag, "math"))
    {
        if (strcmp(tag, "canvas") != 0)
        {
            float fs = 11.0f;
            float tw = mini_text_measure(tag, fs);
            mini_draw_text(r, x + (w - tw) / 2, y + (h - fs) / 2, tag, fs,
                           0.3f, 0.3f, 0.3f, 0.8f);
        }
        /* canvas: the WebGL bridge paints on top of this placeholder via a
           scissored gl.viewport, so a real canvas shows WebGL content.   */
    }
    else if (!strcmp(tag, "img"))
    {
        /* broken-image glyph: an X + label */
        mini_draw_line(r, x + 4, y + 4, x + w - 4, y + h - 4, 1, 0.5f, 0.5f, 0.5f, 0.9f);
        mini_draw_line(r, x + w - 4, y + 4, x + 4, y + h - 4, 1, 0.5f, 0.5f, 0.5f, 0.9f);
        const char *alt = mini_node_get_attribute(n, "alt");
        const char *lbl = (alt && alt[0]) ? alt : "IMG";
        float fs = 11.0f, tw = mini_text_measure(lbl, fs);
        mini_draw_text(r, x + (w - tw) / 2, y + h + 2, lbl, fs,
                       0.3f, 0.3f, 0.3f, 0.8f);
    }
    else if (!strcmp(tag, "video") || !strcmp(tag, "audio"))
    {
        float cxp = x + w / 2, cyp = y + h / 2;
        float sz = (w < h ? w : h) * 0.18f;
        if (sz < 4)
            sz = 4;
        mini_draw_triangle(r, cxp - sz, cyp - sz, cxp - sz, cyp + sz,
                           cxp + sz, cyp, 0.2f, 0.5f, 0.85f, 1.0f);
        float fs = 10.0f, tw = mini_text_measure(tag, fs);
        mini_draw_text(r, x + (w - tw) / 2, y + h + 2, tag, fs,
                       0.3f, 0.3f, 0.3f, 0.8f);
    }
}

static void render_list_marker(struct MiniNode *n, MiniRenderer *r)
{
    struct MiniNode *parent = n->parent;
    if (!parent || !parent->tag)
        return;
    int ordered = !strcmp(parent->tag, "ol");
    int idx = 1;
    for (struct MiniNode *c = parent->first_child; c && c != n; c = c->next_sibling)
        if (c->type == MN_ELEMENT_NODE && c->tag && !strcmp(c->tag, "li"))
            idx++;
    MiniStyle *s = &n->style;
    float fs = s->font_size > 0 ? s->font_size : 16.0f;
    float mx = s->abs_x - 18.0f;
    if (mx < 0)
        mx = 0;
    float my = s->abs_y + 2.0f;
    if (ordered)
    {
        char buf[16];
        snprintf(buf, sizeof buf, "%d.", idx);
        mini_draw_text(r, mx, my, buf, fs, 0.15f, 0.15f, 0.15f, 1.0f);
    }
    else
    {
        mini_draw_circle(r, mx + 5, my + fs * 0.5f, fs * 0.18f,
                         0.15f, 0.15f, 0.15f, 1.0f);
    }
}

static void render_interactive(struct MiniNode *n, MiniRenderer *r)
{
    const char *tag = n->tag;
    if (!tag)
        return;
    MiniStyle *s = &n->style;
    float x = s->abs_x, y = s->abs_y, w = s->w, h = s->h;
    if (!strcmp(tag, "template"))
        return; /* never renders */
    if (!strcmp(tag, "dialog"))
    {
        if (mini_node_get_attribute(n, "open") != NULL)
        {
            mini_draw_rect(r, x, y, w, h, 1, 1, 1, 1);
            mini_draw_rect_stroke(r, x, y, w, h, 2, 0.2f, 0.2f, 0.2f, 1.0f);
        }
        return;
    }
    if (!strcmp(tag, "details"))
    {
        mini_draw_rect_stroke(r, x, y, w, h, 1, 0.6f, 0.6f, 0.6f, 0.4f);
        return;
    }
    if (!strcmp(tag, "summary"))
    {
        int open = n->parent &&
                   mini_node_get_attribute(n->parent, "open") != NULL;
        float ty = y + 2.0f;
        if (open)
            mini_draw_triangle(r, x + 2, ty, x + 14, ty, x + 8, ty + 8,
                               0.2f, 0.2f, 0.2f, 1.0f); /* ▾ open  */
        else
            mini_draw_triangle(r, x + 4, ty, x + 12, ty + 8, x + 4, ty + 8,
                               0.2f, 0.2f, 0.2f, 1.0f); /* ▶ closed */
        return;
    }
    /* slot: rendering of slotted light children is handled in render_node */
}

/* per-element placeholder decoration, drawn after the element's own bg */
static void render_element_decor(struct MiniNode *n, MiniRenderer *r)
{
    switch (n->category)
    {
    case MINI_CAT_TEXT:
    {
        const char *tag = n->tag;
        if (tag && !strcmp(tag, "hr"))
        {
            MiniStyle *s = &n->style;
            float ly = s->abs_y + s->h * 0.5f;
            if (s->h < 1)
                ly = s->abs_y;
            mini_draw_line(r, s->abs_x, ly, s->abs_x + s->w, ly, 1,
                           0.4f, 0.4f, 0.4f, 1.0f);
        }
        break;
    }
    case MINI_CAT_MEDIA:
        render_media(n, r);
        break;
    case MINI_CAT_FORM:
        render_form_control(n, r);
        break;
    case MINI_CAT_TABLE:
    {
        const char *tag = n->tag;
        MiniStyle *s = &n->style;
        if (tag && (!strcmp(tag, "td") || !strcmp(tag, "th")))
        {
            if (!strcmp(tag, "th"))
                mini_draw_rect(r, s->abs_x, s->abs_y, s->w, s->h,
                               0.9f, 0.9f, 0.9f, 0.7f);
            mini_draw_rect_stroke(r, s->abs_x, s->abs_y, s->w, s->h, 1,
                                  0.5f, 0.5f, 0.5f, 1.0f);
        }
        break;
    }
    case MINI_CAT_INTERACTIVE:
        render_interactive(n, r);
        break;
    case MINI_CAT_LIST:
        if (n->tag && !strcmp(n->tag, "li"))
            render_list_marker(n, r);
        break;
    default:
        break; /* SECTION / METADATA(empty) / UNKNOWN containers */
    }
}

/* Draw one side of an element's border. Painted as an overlay on the
   element box (content is not shrunk; true content-box accounting is
   deferred with box-sizing). solid/dashed/dotted are supported.       */
static void draw_border_side(MiniRenderer *r, const MiniStyle *s, int side,
                             float cr, float cg, float cb, float ca)
{
    float bw = s->border_w[side];
    if (bw <= 0.0f || s->border_style[side] == 0 /* none */)
        return;
    if (bw < 1.0f)
        bw = 1.0f; /* keep thin borders visible */
    float x = s->abs_x, y = s->abs_y, w = s->w, h = s->h;
    int horiz = (side == 0 || side == 2); /* top/bottom run along x */
    float len = horiz ? w : h;
    float bx = (side == 1) ? (x + w - bw) : x; /* right edge inset */
    float by = (side == 2) ? (y + h - bw) : y; /* bottom edge inset */
    if (s->border_style[side] == 1 /* solid */)
    {
        if (horiz)
            mini_draw_rect(r, bx, by, len, bw, cr, cg, cb, ca);
        else
            mini_draw_rect(r, bx, by, bw, len, cr, cg, cb, ca);
        return;
    }
    /* dashed (2) / dotted (3): segment along the major axis */
    float dash, gap;
    if (s->border_style[side] == 2)
    {
        dash = bw * 3.0f; /* dashed */
        gap = bw * 2.0f;
    }
    else
    {
        dash = bw; /* dotted */
        gap = bw * 2.0f;
    }
    if (dash < 1.0f)
        dash = 1.0f;
    if (gap < 1.0f)
        gap = 1.0f;
    float pos = 0.0f;
    while (pos < len)
    {
        float seg = (pos + dash <= len) ? dash : (len - pos);
        if (seg <= 0.0f)
            break;
        if (horiz)
            mini_draw_rect(r, bx + pos, by, seg, bw, cr, cg, cb, ca);
        else
            mini_draw_rect(r, bx, by + pos, bw, seg, cr, cg, cb, ca);
        pos += dash + gap;
    }
}

static void render_node(struct MiniNode *n, MiniRenderer *r);

/* render an element's light children sorted by z-index (stable: tree
   order preserved within equal z) so positioned overlays (z>0) paint
   above in-flow content and negative-z items paint behind.          */
static void render_children_z(struct MiniNode *parent, MiniRenderer *r)
{
    struct MiniNode *arr[256];
    int n = 0;
    /* 伪元素已包含在常规 child 链表中，直接按树结构遍历并排序 z-index */
    for (struct MiniNode *c = parent->first_child; c && n < 256; c = c->next_sibling)
        arr[n++] = c;
    for (int i = 1; i < n; i++)
    {
        struct MiniNode *k = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j]->style.z_index > k->style.z_index)
        {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = k;
    }
    for (int i = 0; i < n; i++)
        render_node(arr[i], r);
}

static MiniDocument *g_active_render_doc = NULL;

static void render_node(struct MiniNode *n, MiniRenderer *r)
{
    if (!n || !r)
        return;

    /* 1. 文本节点渲染 */
    if (n->type == MN_TEXT_NODE)
    {
        struct MiniNode *p = n->parent;
        int text_grad = (p && p->style.text_gradient == 1);

        float fs = (p && p->style.font_size > 0) ? p->style.font_size : (n->style.font_size > 0 ? n->style.font_size : 16.0f);
        float tr = 1.0f, tg = 1.0f, tb = 1.0f, ta = 1.0f;
        struct MiniNode *cur = n->parent ? n->parent : n;
        while (cur)
        {
            if (cur->style.color_set)
            {
                tr = cur->style.color_r;
                tg = cur->style.color_g;
                tb = cur->style.color_b;
                ta = cur->style.color_a;
                break;
            }
            cur = cur->parent;
        }
        if (ta <= 0.0f)
            ta = 1.0f;

        if (p && p->style.has_filter && p->style.filter_invert > 0.0f)
        {
            float inv = p->style.filter_invert;
            tr = tr * (1.0f - inv) + (1.0f - tr) * inv;
            tg = tg * (1.0f - inv) + (1.0f - tg) * inv;
            tb = tb * (1.0f - inv) + (1.0f - tb) * inv;
        }

        if (n->text && n->text[0] && fs > 0)
        {
            char collapsed[1024];
            char out_text[1024];
            const char *src_text = n->text;
            int is_pre = (p && p->style.white_space == 1);
            if (!is_pre)
            {
                const char *s = n->text;
                size_t ci = 0;
                int in_ws = 0;
                while (*s && ci < sizeof(collapsed) - 1)
                {
                    if (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
                    {
                        if (!in_ws)
                        {
                            collapsed[ci++] = ' ';
                            in_ws = 1;
                        }
                    }
                    else
                    {
                        collapsed[ci++] = *s;
                        in_ws = 0;
                    }
                    s++;
                }
                collapsed[ci] = '\0';
                src_text = collapsed;
            }

            if (p && p->style.text_transform == 1) /* uppercase */
            {
                size_t tl = strlen(src_text);
                if (tl >= sizeof(out_text))
                    tl = sizeof(out_text) - 1;
                for (size_t ti = 0; ti < tl; ti++)
                {
                    unsigned char c = (unsigned char)src_text[ti];
                    if (c >= 'a' && c <= 'z')
                        out_text[ti] = (char)(c - 'a' + 'A');
                    else
                        out_text[ti] = (char)c;
                }
                out_text[tl] = '\0';
                src_text = out_text;
            }

            if (!src_text[0])
                return;

            float draw_x = n->style.abs_x;
            float draw_y = n->style.abs_y;
            int align = p ? (p->style.text_align ? p->style.text_align : n->style.text_align) : n->style.text_align;
            float wrap_w = (n->style.w > 0.0f) ? n->style.w : 10000.0f;

            if (p && (p->style.display == MINI_DISPLAY_FLEX || p->style.display == MINI_DISPLAY_INLINE_FLEX || p->style.display == MINI_DISPLAY_INLINE))
            {
                align = 0; /* Flex / Inline 节点已有精准绝对坐标，严禁二次渲染偏移 */
            }
            else if (align != 0 && p && p->style.w > 0.0f)
            {
                wrap_w = p->style.w - p->style.padding[1] - p->style.padding[3];
                if (wrap_w < 0.0f)
                    wrap_w = 0.0f;
                draw_x = p->style.abs_x + p->style.padding[3];
            }

            if (draw_x == 0.0f && draw_y == 0.0f && p)
            {
                draw_x = p->style.abs_x + p->style.padding[3];
                draw_y = p->style.abs_y + p->style.padding[0];
            }

            if (src_text && !strcmp(src_text, "“"))
            {
                draw_y -= fs * 0.35f;
            }

            float ls = p ? (p->style.letter_set ? p->style.len_letter.v : 0.0f) : (n->style.letter_set ? n->style.len_letter.v : 0.0f);
            float lh = mini_text_line_height(fs);
            if (p && p->style.line_height_set)
            {
                if (p->style.len_line_height.unit == 0 && p->style.len_line_height.v > 0 && p->style.len_line_height.v <= 5.0f)
                {
                    lh = fs * p->style.len_line_height.v;
                }
                else
                {
                    lh = p->style.len_line_height.v;
                }
            }

            int fw = p ? (p->style.font_weight > 0 ? p->style.font_weight : 400) : (n->style.font_weight > 0 ? n->style.font_weight : 400);
            int fst = p ? (p->style.font_style > 0 ? p->style.font_style : 0) : (n->style.font_style > 0 ? n->style.font_style : 0);

            if (p && p->style.has_text_shadow)
            {
                float sx = p->style.ts_x;
                float sy = p->style.ts_y;
                float sr = p->style.ts_r, sg = p->style.ts_g, sb = p->style.ts_b, sa = p->style.ts_a;
                if (p->style.ts_blur > 0.0f)
                {
                    float b = p->style.ts_blur * 0.15f; /* 缩小硬绘制扩散半径 */
                    float b_alpha = sa * 0.20f;         /* 降低单次 Alpha 避免发黑 */
                    draw_text_wrapped_ex(r, src_text, draw_x + sx - b, draw_y + sy, wrap_w, fs, sr, sg, sb, b_alpha, align, ls, lh, fw, fst);
                    draw_text_wrapped_ex(r, src_text, draw_x + sx + b, draw_y + sy, wrap_w, fs, sr, sg, sb, b_alpha, align, ls, lh, fw, fst);
                    draw_text_wrapped_ex(r, src_text, draw_x + sx, draw_y + sy - b, wrap_w, fs, sr, sg, sb, b_alpha, align, ls, lh, fw, fst);
                    draw_text_wrapped_ex(r, src_text, draw_x + sx, draw_y + sy + b, wrap_w, fs, sr, sg, sb, b_alpha, align, ls, lh, fw, fst);
                }
                else
                {
                    draw_text_wrapped_ex(r, src_text, draw_x + sx, draw_y + sy, wrap_w, fs, sr, sg, sb, sa * 0.5f, align, ls, lh, fw, fst);
                }
            }

            /* page-text selection highlight, painted behind the glyphs */
            render_text_selection(n, r, src_text, draw_x, draw_y, wrap_w,
                                  fs, ls, lh, align);

            /* 渐变文字绘制并居中 */
            if (text_grad)
            {
                float r1 = (p && p->style.has_gradient) ? p->style.grad_r1 : 1.0f;
                float g1 = (p && p->style.has_gradient) ? p->style.grad_g1 : 1.0f;
                float b1 = (p && p->style.has_gradient) ? p->style.grad_b1 : 1.0f;
                float r2 = (p && p->style.has_gradient) ? p->style.grad_r2 : 0.0f;
                float g2 = (p && p->style.has_gradient) ? p->style.grad_g2 : 0.94f;
                float b2 = (p && p->style.has_gradient) ? p->style.grad_b2 : 1.0f;

                if (r1 == 0.0f && g1 == 0.0f && b1 == 0.0f && r2 == 0.0f && g2 == 0.0f && b2 == 0.0f)
                {
                    r1 = 1.0f;
                    g1 = 1.0f;
                    b1 = 1.0f;
                    r2 = 0.0f;
                    g2 = 0.94f;
                    b2 = 1.0f;
                }

                size_t len = strlen(src_text);
                float total_w = mini_text_measure_ex(src_text, fs, ls);
                float cur_x = draw_x;
                if (align == 1)
                {
                    cur_x = draw_x + (wrap_w - total_w) * 0.5f;
                }
                else if (align == 2)
                {
                    cur_x = draw_x + (wrap_w - total_w);
                }

                size_t i = 0;
                while (i < len)
                {
                    unsigned char c = (unsigned char)src_text[i];
                    int clen = 1;
                    if (c >= 0xF0)
                        clen = 4;
                    else if (c >= 0xE0)
                        clen = 3;
                    else if (c >= 0xC0)
                        clen = 2;
                    if (i + clen > len)
                        clen = (int)(len - i);

                    char utf8_char[8] = {0};
                    memcpy(utf8_char, src_text + i, clen);
                    utf8_char[clen] = '\0';

                    float t = (len > 1) ? ((float)i / (float)(len - 1)) : 0.0f;
                    float cr = r1 * (1.0f - t) + r2 * t;
                    float cg = g1 * (1.0f - t) + g2 * t;
                    float cb = b1 * (1.0f - t) + b2 * t;
                    float cw = mini_text_measure_ex(utf8_char, fs, ls);
                    mini_draw_text_styled(r, cur_x, draw_y, utf8_char, fs, cr, cg, cb, 1.0f, ls, fst);
                    if (fw >= 600)
                    {
                        float b_off = (fw >= 800) ? 1.0f : 0.6f;
                        mini_draw_text_styled(r, cur_x + b_off, draw_y, utf8_char, fs, cr, cg, cb, 1.0f, ls, fst);
                    }
                    cur_x += cw;
                    i += clen;
                }
            }
            else
            {
                draw_text_wrapped_ex(r, src_text, draw_x, draw_y, wrap_w, fs, tr, tg, tb, ta, align, ls, lh, fw, fst);
            }
        }
        return;
    }

    if (n->type != MN_ELEMENT_NODE && n->type != MN_DOCUMENT_FRAGMENT_NODE)
        return;
    if (n->style.display == MINI_DISPLAY_NONE)
        return;

    /* Box culling against overflow:hidden parent */
    if (n->parent && n->parent->style.overflow == 1 && n->style.position != 3 /* not fixed */)
    {
        float px0 = n->parent->style.abs_x;
        float px1 = px0 + n->parent->style.w;
        float py0 = n->parent->style.abs_y;
        float py1 = py0 + n->parent->style.h;
        /* 终极修复 3：放宽裁剪边界，<= 改为 <，防止 0 宽度或边缘贴合的 Flex 子项目被强制当成不可见抛弃 */
        if (n->style.abs_x > px1 || (n->style.abs_x + n->style.w) < px0 ||
            n->style.abs_y > py1 || (n->style.abs_y + n->style.h) < py0)
        {
            return; /* completely outside parent content bounds */
        }
    }

    MiniStyle *s = &n->style;

    int is_fixed = (s->position == 3);
    float g_sx = (g_active_render_doc) ? g_active_render_doc->scroll_x : 0.0f;
    float g_sy = (g_active_render_doc) ? g_active_render_doc->scroll_y : 0.0f;
    int cancel_scroll = (is_fixed && (g_sx > 0.0f || g_sy > 0.0f));
    if (cancel_scroll)
    {
        mini_draw_push_xform_full(r, 0, 0, g_sx, g_sy, 0, 0, 0, 0, 1, 1, 0, 0, 0.0f);
    }

    float radii[4];
    for (int i = 0; i < 4; i++)
    {
        float r_val = s->border_radius_corners[i] > 0.0f ? s->border_radius_corners[i] : s->border_radius;
        if (s->border_radius_pct_corners[i] || s->border_radius_pct)
        {
            float m = s->w < s->h ? s->w : s->h;
            r_val = m * 0.5f * (r_val / 50.0f);
        }
        radii[i] = r_val;
    }

    float op = s->has_opacity ? s->opacity : 1.0f;
    if (op < 0.0f)
        op = 0.0f;
    if (op > 1.0f)
        op = 1.0f;

    /* 仅当节点显式标记了 has_transform 或存在真实的位移/旋转/缩放/透视时，才推入变换矩阵 */
    int has_xform = s->has_transform || (s->translate_x != 0.0f || s->translate_y != 0.0f || s->translate_z != 0.0f ||
                                         (s->scale_x > 0.0f && s->scale_x != 1.0f) || (s->scale_y > 0.0f && s->scale_y != 1.0f) ||
                                         s->rotate_x != 0.0f || s->rotate_y != 0.0f || s->rotate_z != 0.0f ||
                                         s->skew_x != 0.0f || s->skew_y != 0.0f || s->perspective > 0.0f);
    if (has_xform)
    {
        float ox = s->transform_origin_x;
        float oy = s->transform_origin_y;
        if (ox == 0.0f && oy == 0.0f && !strstr(mini_node_get_attribute(n, "style") ? mini_node_get_attribute(n, "style") : "", "transform-origin"))
        {
            ox = 0.5f;
            oy = 0.5f;
        }

        float cx = s->abs_x + s->w * ox;
        float cy = s->abs_y + s->h * oy;
        float sx = (s->scale_x != 0.0f || s->has_transform) ? s->scale_x : 1.0f;
        float sy = (s->scale_y != 0.0f || s->has_transform) ? s->scale_y : 1.0f;

        /* 抑制 OpenGL 固化渲染管线中的默认 1000px 双重透视，使用 1e6f(无限远平角) 使其失效，还原纯净级 3D 正方体 */
        float eff_persp = s->perspective > 0.0f ? s->perspective : ((s->rotate_x != 0.0f || s->rotate_y != 0.0f || s->rotate_z != 0.0f) ? 1e6f : 0.0f);

        mini_draw_push_xform_full(r, cx, cy, s->translate_x, s->translate_y, s->translate_z,
                                  s->rotate_x, s->rotate_y, s->rotate_z,
                                  sx, sy, s->skew_x, s->skew_y, eff_persp);
    }

    if (s->has_clip_rect)
    {
        float cl = s->clip_rect[3];
        float ct = s->clip_rect[0];
        float cr_x = s->clip_rect[1] < 9000.0f ? s->clip_rect[1] : s->w;
        float cb_y = s->clip_rect[2] < 9000.0f ? s->clip_rect[2] : s->h;
        float cw = cr_x > cl ? (cr_x - cl) : 0.0f;
        float ch = cb_y > ct ? (cb_y - ct) : 0.0f;
        if (cw > s->w)
            cw = s->w;
        if (ch > s->h)
            ch = s->h;
        mini_renderer_push_clip(r, s->abs_x + cl, s->abs_y + ct, cw, ch);
    }

    int skip_bg = (s->text_gradient == 1);

    if (s->w > 0 && s->h > 0 && (s->has_shadow || s->num_shadows > 0) && !skip_bg && op > 0.001f)
    {
        int count = s->num_shadows > 0 ? s->num_shadows : 1;
        for (int i = 0; i < count; i++)
        {
            float sh_x = (s->num_shadows > 0) ? s->shadows[i].x : s->shadow_x;
            float sh_y = (s->num_shadows > 0) ? s->shadows[i].y : s->shadow_y;
            float sh_blur = (s->num_shadows > 0) ? s->shadows[i].blur : s->shadow_blur;
            float sh_spread = (s->num_shadows > 0) ? s->shadows[i].spread : s->shadow_spread;
            float sh_r = (s->num_shadows > 0) ? s->shadows[i].r : s->shadow_r;
            float sh_g = (s->num_shadows > 0) ? s->shadows[i].g : s->shadow_g;
            float sh_b = (s->num_shadows > 0) ? s->shadows[i].b : s->shadow_b;
            float sh_a = (s->num_shadows > 0) ? s->shadows[i].a : s->shadow_a;
            if (sh_a * op <= 0.001f)
                continue;

            if (s->has_clip_polygon && s->num_clip_poly_pts >= 3)
            {
                float pts[16];
                for (int pi = 0; pi < s->num_clip_poly_pts; pi++)
                {
                    pts[pi * 2] = s->abs_x + s->clip_poly_pts[pi][0] * s->w;
                    pts[pi * 2 + 1] = s->abs_y + s->clip_poly_pts[pi][1] * s->h;
                }
                for (int step = 1; step <= 8; step++)
                {
                    float sw = (float)step * 2.5f + sh_spread;
                    float sa = (sh_a * op) / (float)(step * step * 1.2f);
                    mini_draw_polygon_stroke(r, pts, s->num_clip_poly_pts, sw, sh_r, sh_g, sh_b, sa);
                }
            }
            else
            {
                float bx = s->abs_x + sh_x;
                float by = s->abs_y + sh_y;
                float bblur = sh_blur > 0.0f ? sh_blur : 20.0f;
                mini_draw_shadow_corners(r, bx, by, s->w, s->h, radii,
                                         sh_spread, bblur,
                                         sh_r, sh_g, sh_b, sh_a * op);
            }
        }
    }

    if (s->w > 0 && s->h > 0 && !skip_bg && op > 0.001f)
    {
        float own_fs = s->font_size > 0 ? s->font_size : 16.0f;
        if (s->has_clip_polygon && s->num_clip_poly_pts >= 3)
        {
            float pts[16];
            for (int i = 0; i < s->num_clip_poly_pts; i++)
            {
                pts[i * 2] = s->abs_x + s->clip_poly_pts[i][0] * s->w;
                pts[i * 2 + 1] = s->abs_y + s->clip_poly_pts[i][1] * s->h;
            }
            mini_draw_polygon(r, pts, s->num_clip_poly_pts, s->bg_r, s->bg_g, s->bg_b, s->bg_a * op);
            float in_r = s->border_r > 0 ? s->border_r : s->shadow_r;
            float in_g = s->border_g > 0 ? s->border_g : s->shadow_g;
            float in_b = s->border_b > 0 ? s->border_b : s->shadow_b;
            if (in_r > 0 || in_g > 0 || in_b > 0)
            {
                for (int step = 1; step <= 3; step++)
                {
                    float sw = (float)step * 1.5f;
                    float sa = (0.22f * op) / (float)step;
                    mini_draw_polygon_stroke(r, pts, s->num_clip_poly_pts, sw, in_r, in_g, in_b, sa);
                }
            }
        }
        else if (s->bg_image_url[0] != '\0')
        {
            float bw = resolve_len(s->bg_size_w, s->w, own_fs, 16.0f, 900.0f, 600.0f);
            float bh = resolve_len(s->bg_size_h, s->h, own_fs, 16.0f, 900.0f, 600.0f);
            float bx = resolve_len(s->bg_pos_x, s->w, own_fs, 16.0f, 900.0f, 600.0f);
            float by = resolve_len(s->bg_pos_y, s->h, own_fs, 16.0f, 900.0f, 600.0f);
            mini_draw_background_image(r, s->abs_x, s->abs_y, s->w, s->h,
                                       s->bg_image_url, s->bg_size_mode,
                                       bw, bh, bx, by, s->bg_repeat,
                                       radii);
        }
        else if (s->bg_size_h.v > 0.0f || s->bg_size_w.v > 0.0f)
        {
            float gh = resolve_len(s->bg_size_h, s->h, own_fs, 16.0f, 900.0f, 600.0f);
            float gw = resolve_len(s->bg_size_w, s->w, own_fs, 16.0f, 900.0f, 600.0f);
            if (gh >= 2.0f && gh <= 32.0f)
            {
                /* CRT Horizontal Scanlines */
                for (float gy = s->abs_y; gy <= s->abs_y + s->h; gy += gh)
                    mini_draw_rect(r, s->abs_x, gy + gh * 0.5f, s->w, gh * 0.5f, 0.0f, 0.0f, 0.0f, 0.60f * op);
            }
            if (gw >= 2.0f && gw <= 32.0f)
            {
                /* Vertical RGB Phosphor Subpixel Grid */
                for (float gx = s->abs_x; gx <= s->abs_x + s->w; gx += gw)
                {
                    mini_draw_rect(r, gx, s->abs_y, 2.0f, s->h, 1.0f, 0.0f, 0.0f, 0.06f * op);
                    mini_draw_rect(r, gx + 2.0f, s->abs_y, 2.0f, s->h, 0.0f, 1.0f, 0.0f, 0.04f * op);
                    mini_draw_rect(r, gx + 4.0f, s->abs_y, 2.0f, s->h, 0.0f, 0.5f, 1.0f, 0.06f * op);
                }
            }
            else if (gw >= 32.0f && gh >= 32.0f)
            {
                for (float gx = s->abs_x; gx <= s->abs_x + s->w; gx += gw)
                    mini_draw_line(r, gx, s->abs_y, gx, s->abs_y + s->h, 1.0f, 0.0f, 0.95f, 1.0f, 0.15f * op);
                for (float gy = s->abs_y; gy <= s->abs_y + s->h; gy += gh)
                    mini_draw_line(r, s->abs_x, gy, s->abs_x + s->w, gy, 1.0f, 1.0f, 0.0f, 0.33f, 0.15f * op);
            }
        }
        else if (s->has_gradient)
        {
            if (s->grad_type == 3)
            {
                /* Procedural Repeating Stripes (极速程序化条纹生成器) */
                if (radii[0] > 0 || radii[1] > 0 || radii[2] > 0 || radii[3] > 0)
                    mini_renderer_push_rounded_clip_corners(r, s->abs_x, s->abs_y, s->w, s->h, radii);
                else
                    mini_renderer_push_clip(r, s->abs_x, s->abs_y, s->w, s->h);

                mini_draw_rect(r, s->abs_x, s->abs_y, s->w, s->h, s->grad_r1, s->grad_g1, s->grad_b1, s->grad_a1 * op);

                float sr = s->grad_num_stops >= 3 ? s->grad_stops[2].r : s->grad_r2;
                float sg = s->grad_num_stops >= 3 ? s->grad_stops[2].g : s->grad_g2;
                float sb = s->grad_num_stops >= 3 ? s->grad_stops[2].b : s->grad_b2;
                float sa = s->grad_num_stops >= 3 ? s->grad_stops[2].a * op : s->grad_a2 * op;

                float cycle = s->grad_num_stops >= 4 ? s->grad_stops[3].pos : 20.0f;
                if (cycle < 1.0f)
                    cycle = 20.0f;
                float half = cycle * 0.5f;

                float diag = s->w + s->h;
                int num_lines = (int)(diag / cycle) + 5;

                float cx = s->abs_x + s->w * 0.5f;
                float cy = s->abs_y + s->h * 0.5f;
                float angle_rad = s->grad_angle * (3.14159265f / 180.0f);
                float nx = sinf(angle_rad), ny = -cosf(angle_rad);
                float ldx = -ny, ldy = nx;

                for (int i = -num_lines; i <= num_lines; i++)
                {
                    float offset = i * cycle;
                    float px = cx + nx * offset;
                    float py = cy + ny * offset;
                    mini_draw_line(r, px - ldx * diag, py - ldy * diag,
                                   px + ldx * diag, py + ldy * diag,
                                   half, sr, sg, sb, sa);
                }
                mini_renderer_pop_clip(r);
            }
            else if (s->grad_num_stops >= 2)
            {
                struct
                {
                    float r, g, b, a;
                    float pos;
                } stops[MINI_MAX_GRAD_STOPS];
                for (int si = 0; si < s->grad_num_stops; si++)
                {
                    stops[si].r = s->grad_stops[si].r;
                    stops[si].g = s->grad_stops[si].g;
                    stops[si].b = s->grad_stops[si].b;
                    stops[si].a = s->grad_stops[si].a * op;
                    stops[si].pos = s->grad_stops[si].pos;
                }
                mini_draw_gradient_multi(r, s->abs_x, s->abs_y, s->w, s->h,
                                         stops, s->grad_num_stops,
                                         s->grad_type, s->grad_angle, radii);
            }
            else if (radii[0] > 0 || radii[1] > 0 || radii[2] > 0 || radii[3] > 0)
                mini_draw_gradient_ex(r, s->abs_x, s->abs_y, s->w, s->h,
                                      s->grad_r1, s->grad_g1, s->grad_b1, s->grad_a1 * op,
                                      s->grad_r2, s->grad_g2, s->grad_b2, s->grad_a2 * op,
                                      s->grad_type, s->grad_angle, radii[0]);
            else
                mini_draw_gradient(r, s->abs_x, s->abs_y, s->w, s->h,
                                   s->grad_r1, s->grad_g1, s->grad_b1, s->grad_a1 * op,
                                   s->grad_r2, s->grad_g2, s->grad_b2, s->grad_a2 * op,
                                   s->grad_type, s->grad_angle);
        }
        else if (s->bg_a > 0.0f || s->has_backdrop_filter || (mini_node_get_attribute(n, "style") && strstr(mini_node_get_attribute(n, "style"), "difference")))
        {
            float draw_h = s->h;
            if (draw_h > 0.0f && draw_h <= 1.5f)
                draw_h = 2.0f;

            int is_difference = (mini_node_get_attribute(n, "style") && strstr(mini_node_get_attribute(n, "style"), "difference"));

            /* 1. 如果有毛玻璃，优先调用硬件捕获级高斯模糊 */
            if (s->has_backdrop_filter)
            {
                mini_draw_backdrop_filter(r, s->abs_x, s->abs_y, s->w, draw_h, s->backdrop_blur, s->filter_invert, radii);
            }

            /* 2. 差异混合模式（Difference） */
            if (is_difference)
            {
                mini_draw_rect_difference(r, s->abs_x, s->abs_y, s->w, draw_h, radii, s->bg_r, s->bg_g, s->bg_b, s->bg_a * op);
            }

            /* 3. 如果自身带有背景颜色，并且当前不在差异混合模式下，则覆盖背景颜色 */
            if (s->bg_a > 0.0f && !is_difference)
            {
                /* 根部 body/html 的纯色背景已由 mini_draw_clear 在帧开头清除，避免遮挡 WebGL */
                if (n->tag && (!strcmp(n->tag, "body") || !strcmp(n->tag, "html")))
                {
                    /* skip duplicate solid fill on root body/html */
                }
                else
                {
                    float cr = s->bg_r, cg = s->bg_g, cb = s->bg_b;
                if (s->has_filter)
                {
                    if (s->filter_invert > 0.0f)
                    {
                        float inv = s->filter_invert;
                        cr = cr * (1.0f - inv) + (1.0f - cr) * inv;
                        cg = cg * (1.0f - inv) + (1.0f - cg) * inv;
                        cb = cb * (1.0f - inv) + (1.0f - cb) * inv;
                    }
                    if (s->filter_grayscale > 0.0f)
                    {
                        float gs = s->filter_grayscale;
                        float lum = 0.299f * cr + 0.587f * cg + 0.114f * cb;
                        cr = cr * (1.0f - gs) + lum * gs;
                        cg = cg * (1.0f - gs) + lum * gs;
                        cb = cb * (1.0f - gs) + lum * gs;
                    }
                    if (s->filter_brightness != 1.0f && s->filter_brightness > 0.0f)
                    {
                        cr *= s->filter_brightness;
                        cg *= s->filter_brightness;
                        cb *= s->filter_brightness;
                    }
                }
                    if (radii[0] > 0 || radii[1] > 0 || radii[2] > 0 || radii[3] > 0)
                        mini_draw_rect_rounded_corners(r, s->abs_x, s->abs_y, s->w, draw_h,
                                                       radii, cr, cg, cb, s->bg_a * op);
                    else
                        mini_draw_rect(r, s->abs_x, s->abs_y, s->w, draw_h,
                                       cr, cg, cb, s->bg_a * op);
                }
            }
        }
    }

    if (s->w > 0 && s->h > 0 && s->has_border && s->border_a > 0.001f && op > 0.001f)
    {
        float br = s->border_r, bgc = s->border_g, bb = s->border_b, ba = s->border_a * op;

        if (s->has_clip_polygon && s->num_clip_poly_pts >= 3)
        {
            float pts[16];
            for (int i = 0; i < s->num_clip_poly_pts; i++)
            {
                pts[i * 2] = s->abs_x + s->clip_poly_pts[i][0] * s->w;
                pts[i * 2 + 1] = s->abs_y + s->clip_poly_pts[i][1] * s->h;
            }
            mini_draw_polygon_stroke(r, pts, s->num_clip_poly_pts, s->border_w[0] > 0 ? s->border_w[0] : 2.0f, br, bgc, bb, ba);
        }
        else if (radii[0] <= 0.0f && radii[1] <= 0.0f && radii[2] <= 0.0f && radii[3] <= 0.0f)
        {
            for (int i = 0; i < 4; i++)
                draw_border_side(r, s, i, br, bgc, bb, ba);
        }
        else
        {
            /* 使用正规封装函数绘制圆角边框 */
            mini_draw_rect_rounded_corners_stroke(r, s->abs_x, s->abs_y, s->w, s->h,
                                                  radii, s->border_w[0] > 0 ? s->border_w[0] : 1.0f,
                                                  br, bgc, bb, ba);
        }
    }

    if (n->type == MN_ELEMENT_NODE)
        render_element_decor(n, r);

    int is_svg = (n->tag && !strcmp(n->tag, "svg"));
    int clip = (s->overflow == 1 && s->w > 0 && s->h > 0);
    if (clip)
    {
        if (radii[0] > 0 || radii[1] > 0 || radii[2] > 0 || radii[3] > 0)
            mini_renderer_push_rounded_clip_corners(r, s->abs_x, s->abs_y, s->w, s->h, radii);
        else
            mini_renderer_push_clip(r, s->abs_x, s->abs_y, s->w, s->h);
    }

    if (!is_svg)
    {
        if (n->shadow_root)
        {
            render_node(n->shadow_root, r);
        }
        else if (n->tag && !strcmp(n->tag, "slot"))
        {
            struct MiniNode *host = slot_host(n);
            int any_element = 0;
            if (host)
                for (struct MiniNode *c = host->first_child; c; c = c->next_sibling)
                    if (c->type == MN_ELEMENT_NODE)
                    {
                        any_element = 1;
                        break;
                    }
            if (host && any_element)
            {
                for (struct MiniNode *c = host->first_child; c; c = c->next_sibling)
                    if (c->type == MN_ELEMENT_NODE)
                        render_node(c, r);
            }
            else
            {
                for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
                    render_node(c, r);
            }
        }
        else
        {
            render_children_z(n, r);
        }
    }

    if (clip)
        mini_renderer_pop_clip(r);

    if (s->has_clip_rect)
        mini_renderer_pop_clip(r);

    if (has_xform)
        mini_draw_pop_xform(r);

    if (cancel_scroll)
        mini_draw_pop_xform(r);
}
/* ------------------------------------------------------------------ */
/* Color parsing: #RGB / #RRGGBB / #RRGGBBAA, rgb()/rgba(), hsl()/     */
/* hsla(), the CSS named colors, and "transparent". Returns linear    */
/* 0..1 RGBA. "currentcolor" is left to the caller (returns 0 so the   */
/* existing color on the element is kept).                            */
/* ------------------------------------------------------------------ */
static float hue2rgb(float p, float q, float t)
{
    if (t < 0.0f)
        t += 1.0f;
    if (t > 1.0f)
        t -= 1.0f;
    if (t < 1.0f / 6.0f)
        return p + (q - p) * 6.0f * t;
    if (t < 0.5f)
        return q;
    if (t < 2.0f / 3.0f)
        return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

/* h,s,l in 0..1 → r,g,b in 0..1 (classic HSL→RGB) */
static void hsl_to_rgb(float h, float s, float l, float *r, float *g, float *b)
{
    if (s <= 0.0f)
    {
        *r = *g = *b = l;
        return;
    }
    float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    float p = 2.0f * l - q;
    *r = hue2rgb(p, q, h + 1.0f / 3.0f);
    *g = hue2rgb(p, q, h);
    *b = hue2rgb(p, q, h - 1.0f / 3.0f);
}

struct named_color
{
    const char *name;
    unsigned char r, g, b;
};
static const struct named_color NAMED_COLORS[] = {
    {"aliceblue", 240, 248, 255}, {"antiquewhite", 250, 235, 215}, {"aqua", 0, 255, 255}, {"aquamarine", 127, 255, 212}, {"azure", 240, 255, 255}, {"beige", 245, 245, 220}, {"bisque", 255, 228, 196}, {"black", 0, 0, 0}, {"blanchedalmond", 255, 235, 205}, {"blue", 0, 0, 255}, {"blueviolet", 138, 43, 226}, {"brown", 165, 42, 42}, {"burlywood", 222, 184, 135}, {"cadetblue", 95, 158, 160}, {"chartreuse", 127, 255, 0}, {"chocolate", 210, 105, 30}, {"coral", 255, 127, 80}, {"cornflowerblue", 100, 149, 237}, {"cornsilk", 255, 248, 220}, {"crimson", 220, 20, 60}, {"cyan", 0, 255, 255}, {"darkblue", 0, 0, 139}, {"darkcyan", 0, 139, 139}, {"darkgoldenrod", 184, 134, 11}, {"darkgray", 169, 169, 169}, {"darkgreen", 0, 100, 0}, {"darkgrey", 169, 169, 169}, {"darkkhaki", 189, 183, 107}, {"darkmagenta", 139, 0, 139}, {"darkolivegreen", 85, 107, 47}, {"darkorange", 255, 140, 0}, {"darkorchid", 153, 50, 204}, {"darkred", 139, 0, 0}, {"darksalmon", 233, 150, 122}, {"darkseagreen", 143, 188, 143}, {"darkslateblue", 72, 61, 139}, {"darkslategray", 47, 79, 79}, {"darkslategrey", 47, 79, 79}, {"darkturquoise", 0, 206, 209}, {"darkviolet", 148, 0, 211}, {"deeppink", 255, 20, 147}, {"deepskyblue", 0, 191, 255}, {"dimgray", 105, 105, 105}, {"dimgrey", 105, 105, 105}, {"dodgerblue", 30, 144, 255}, {"firebrick", 178, 34, 34}, {"floralwhite", 255, 250, 240}, {"forestgreen", 34, 139, 34}, {"fuchsia", 255, 0, 255}, {"gainsboro", 220, 220, 220}, {"ghostwhite", 248, 248, 255}, {"gold", 255, 215, 0}, {"goldenrod", 218, 165, 32}, {"gray", 128, 128, 128}, {"green", 0, 128, 0}, {"greenyellow", 173, 255, 47}, {"grey", 128, 128, 128}, {"honeydew", 240, 255, 240}, {"hotpink", 255, 105, 180}, {"indianred", 205, 92, 92}, {"indigo", 75, 0, 130}, {"ivory", 255, 255, 240}, {"khaki", 240, 230, 140}, {"lavender", 230, 230, 250}, {"lavenderblush", 255, 240, 245}, {"lawngreen", 124, 252, 0}, {"lemonchiffon", 255, 250, 205}, {"lightblue", 173, 216, 230}, {"lightcoral", 240, 128, 128}, {"lightcyan", 224, 255, 255}, {"lightgoldenrodyellow", 250, 250, 210}, {"lightgray", 211, 211, 211}, {"lightgreen", 144, 238, 144}, {"lightgrey", 211, 211, 211}, {"lightpink", 255, 182, 193}, {"lightsalmon", 255, 160, 122}, {"lightseagreen", 32, 178, 170}, {"lightskyblue", 135, 206, 250}, {"lightslategray", 119, 136, 153}, {"lightslategrey", 119, 136, 153}, {"lightsteelblue", 176, 196, 222}, {"lightyellow", 255, 255, 224}, {"lime", 0, 255, 0}, {"limegreen", 50, 205, 50}, {"linen", 250, 240, 230}, {"magenta", 255, 0, 255}, {"maroon", 128, 0, 0}, {"mediumaquamarine", 102, 205, 170}, {"mediumblue", 0, 0, 205}, {"mediumorchid", 186, 85, 211}, {"mediumpurple", 147, 112, 219}, {"mediumseagreen", 60, 179, 113}, {"mediumslateblue", 123, 104, 238}, {"mediumspringgreen", 0, 250, 154}, {"mediumturquoise", 72, 209, 204}, {"mediumvioletred", 199, 21, 133}, {"midnightblue", 25, 25, 112}, {"mintcream", 245, 255, 250}, {"mistyrose", 255, 228, 225}, {"moccasin", 255, 228, 181}, {"navajowhite", 255, 222, 173}, {"navy", 0, 0, 128}, {"oldlace", 253, 245, 230}, {"olive", 128, 128, 0}, {"olivedrab", 107, 142, 35}, {"orange", 255, 165, 0}, {"orangered", 255, 69, 0}, {"orchid", 218, 112, 214}, {"palegoldenrod", 238, 232, 170}, {"palegreen", 152, 251, 152}, {"paleturquoise", 175, 238, 238}, {"palevioletred", 219, 112, 147}, {"papayawhip", 255, 239, 213}, {"peachpuff", 255, 218, 185}, {"peru", 205, 133, 63}, {"pink", 255, 192, 203}, {"plum", 221, 160, 221}, {"powderblue", 176, 224, 230}, {"purple", 128, 0, 128}, {"rebeccapurple", 102, 51, 153}, {"red", 255, 0, 0}, {"rosybrown", 188, 143, 143}, {"royalblue", 65, 105, 225}, {"saddlebrown", 139, 69, 19}, {"salmon", 250, 128, 114}, {"sandybrown", 244, 164, 96}, {"seagreen", 46, 139, 87}, {"seashell", 255, 245, 238}, {"sienna", 160, 82, 45}, {"silver", 192, 192, 192}, {"skyblue", 135, 206, 235}, {"slateblue", 106, 90, 205}, {"slategray", 112, 128, 144}, {"slategrey", 112, 128, 144}, {"snow", 255, 250, 250}, {"springgreen", 0, 255, 127}, {"steelblue", 70, 130, 180}, {"tan", 210, 180, 140}, {"teal", 0, 128, 128}, {"thistle", 216, 191, 216}, {"tomato", 255, 99, 71}, {"turquoise", 64, 224, 208}, {"violet", 238, 130, 238}, {"wheat", 245, 222, 179}, {"white", 255, 255, 255}, {"whitesmoke", 245, 245, 245}, {"yellow", 255, 255, 0}, {"yellowgreen", 154, 205, 50}};

static int named_color_lookup(const char *name, float *r, float *g, float *b, float *a)
{
    size_t n = sizeof NAMED_COLORS / sizeof NAMED_COLORS[0];
    for (size_t i = 0; i < n; i++)
    {
        if (ci_eq(name, NAMED_COLORS[i].name))
        {
            *r = NAMED_COLORS[i].r / 255.0f;
            *g = NAMED_COLORS[i].g / 255.0f;
            *b = NAMED_COLORS[i].b / 255.0f;
            *a = 1.0f;
            return 1;
        }
    }
    return 0;
}

/* Split the inside of rgb()/hsl() into up to 4 numeric tokens, tracking
   whether each ended with '%'. Separators: comma, whitespace, '/'.       */
static int color_components(const char *s, float v[4], int pct[4])
{
    int n = 0;
    while (*s && n < 4)
    {
        while (*s && (*s == ',' || *s == '/' || isspace((unsigned char)*s)))
            s++;
        if (!*s)
            break;
        char *end;
        float f = (float)strtod(s, &end);
        if (end == s)
            break;
        v[n] = f;
        pct[n] = 0;
        while (*end && isspace((unsigned char)*end))
            end++;
        if (*end == '%')
        {
            pct[n] = 1;
            end++;
        }
        s = end;
        n++;
    }
    return n;
}

static float linear_to_srgb(float c)
{
    if (c <= 0.0f)
        return 0.0f;
    if (c >= 1.0f)
        return 1.0f;
    return c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static void oklab_to_rgb(float L, float a, float b, float *r, float *g, float *bout)
{
    float l_ = L + 0.3963377774f * a + 0.2158037573f * b;
    float m_ = L - 0.1055613458f * a - 0.0638541728f * b;
    float s_ = L - 0.0894841775f * a - 1.2914855480f * b;
    float l = l_ * l_ * l_;
    float m = m_ * m_ * m_;
    float s = s_ * s_ * s_;
    float cr = +4.0767434094f * l - 3.3077115913f * m + 0.2309699292f * s;
    float cg = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    float cb = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
    *r = linear_to_srgb(cr);
    *g = linear_to_srgb(cg);
    *bout = linear_to_srgb(cb);
}

static void hwb_to_rgb(float h, float w, float b, float *r, float *g, float *bout)
{
    while (h >= 360.0f)
        h -= 360.0f;
    while (h < 0.0f)
        h += 360.0f;
    if (w + b >= 1.0f)
    {
        float v = w / (w + b);
        *r = *g = *bout = v;
        return;
    }
    float cr, cg, cb;
    hsl_to_rgb(h / 360.0f, 1.0f, 0.5f, &cr, &cg, &cb);
    *r = cr * (1.0f - w - b) + w;
    *g = cg * (1.0f - w - b) + w;
    *bout = cb * (1.0f - w - b) + w;
}

/* ---- color-parse cache ----
   mini_parse_color is deterministic (a given color string always yields the
   same RGBA), but it is called from many style-set sites (border, bg,
   text-shadow, box-shadow, gradient stops) on every restyle and every paint
   of the same nodes — and oklch()/oklab_to_rgb do real trig + matrix work.
   Cache by the raw input string so the 100 oklch uses on a design page are
   converted at most once each per unique string, not per call site per node. */
static uint32_t color_cache_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
    {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h & (MINI_COLOR_CACHE_CAP - 1);
}

static int mini_parse_color_uncached(const char *val, float *r, float *g, float *b, float *a);

int mini_parse_color(const char *val, float *r, float *g, float *b, float *a)
{
    if (!val || !*val)
    {
        if (r)
            *r = 0;
        if (g)
            *g = 0;
        if (b)
            *b = 0;
        if (a)
            *a = 0;
        return 0;
    }
    /* short values (<=95 chars incl. NUL) go through the cache; longer ones
       (very rare) bypass to avoid truncation. */
    size_t vl = strlen(val);
    if (vl < sizeof(((ColorCacheEntry *)0)->key))
    {
        uint32_t h = color_cache_hash(val);
        for (int i = 0; i < 4; i++)
        {
            ColorCacheEntry *e = &g_color_cache[(h + i) & (MINI_COLOR_CACHE_CAP - 1)];
            if (!e->used)
                break; /* empty ends probe */
            if (!strcmp(e->key, val))
            {
                if (r)
                    *r = e->r;
                if (g)
                    *g = e->g;
                if (b)
                    *b = e->b;
                if (a)
                    *a = e->a;
                return e->ok;
            }
        }
        /* miss: parse, then store */
        float rr, gg, bb, aa;
        int ok = mini_parse_color_uncached(val, &rr, &gg, &bb, &aa);
        for (int i = 0; i < 4; i++)
        {
            ColorCacheEntry *e = &g_color_cache[(h + i) & (MINI_COLOR_CACHE_CAP - 1)];
            if (!e->used || g_color_cache_n > (MINI_COLOR_CACHE_CAP * 3) / 4)
            {
                if (g_color_cache_n > (MINI_COLOR_CACHE_CAP * 3) / 4)
                {
                    /* table getting full: clear wholesale then claim slot 0 of this chain */
                    memset(g_color_cache, 0, sizeof g_color_cache);
                    g_color_cache_n = 0;
                    e = &g_color_cache[h & (MINI_COLOR_CACHE_CAP - 1)];
                }
                snprintf(e->key, sizeof e->key, "%s", val);
                e->r = rr;
                e->g = gg;
                e->b = bb;
                e->a = aa;
                e->ok = ok;
                e->used = 1;
                g_color_cache_n++;
                break;
            }
        }
        if (r)
            *r = rr;
        if (g)
            *g = gg;
        if (b)
            *b = bb;
        if (a)
            *a = aa;
        return ok;
    }
    /* long value: bypass cache */
    return mini_parse_color_uncached(val, r, g, b, a);
}

static int mini_parse_color_uncached(const char *val, float *r, float *g, float *b, float *a)
{
    if (!val || !*val)
        return 0;
    while (*val && isspace((unsigned char)*val))
        val++;
    size_t L = strlen(val);
    while (L && isspace((unsigned char)val[L - 1]))
        L--;
    if (L == 0)
        return 0;
    char buf[128];
    if (L >= sizeof buf)
        L = sizeof buf - 1;
    memcpy(buf, val, L);
    buf[L] = 0;
    char low[128];
    for (size_t i = 0; i <= L; i++)
        low[i] = (char)tolower((unsigned char)buf[i]);

    if (!strcmp(low, "transparent"))
    {
        *r = *g = *b = 0.0f;
        *a = 0.0f;
        return 1;
    }
    if (!strcmp(low, "currentcolor"))
        return 0;

    /* hex: #RGB / #RRGGBB / #RRGGBBAA */
    if (low[0] == '#')
    {
        unsigned long c = strtoul(buf + 1, NULL, 16);
        unsigned int rv, gv, bv, av;
        if (L == 4)
        {
            rv = ((c >> 8) & 0xf) * 0x11;
            gv = ((c >> 4) & 0xf) * 0x11;
            bv = (c & 0xf) * 0x11;
            av = 0xff;
        }
        else if (L == 7)
        {
            rv = (c >> 16) & 0xff;
            gv = (c >> 8) & 0xff;
            bv = c & 0xff;
            av = 0xff;
        }
        else if (L == 9)
        {
            rv = (c >> 24) & 0xff;
            gv = (c >> 16) & 0xff;
            bv = (c >> 8) & 0xff;
            av = c & 0xff;
        }
        else
            return 0;
        *r = rv / 255.0f;
        *g = gv / 255.0f;
        *b = bv / 255.0f;
        *a = av / 255.0f;
        return 1;
    }

    /* color-mix(in srgb, color1 pct1, color2 pct2) */
    if (!strncmp(low, "color-mix(", 10))
    {
        char *open = strchr(low, '(');
        char *close = strrchr(low, ')');
        if (open && close && close > open)
        {
            char args[128];
            size_t n = (size_t)(close - open - 1);
            if (n >= sizeof(args))
                n = sizeof(args) - 1;
            memcpy(args, open + 1, n);
            args[n] = 0;
            char *comma1 = strchr(args, ',');
            if (comma1)
            {
                char *comma2 = strchr(comma1 + 1, ',');
                if (comma2)
                {
                    *comma2 = 0;
                    char *c1_str = comma1 + 1;
                    char *c2_str = comma2 + 1;
                    while (*c1_str && isspace((unsigned char)*c1_str))
                        c1_str++;
                    while (*c2_str && isspace((unsigned char)*c2_str))
                        c2_str++;

                    float r1 = 0, g1 = 0, b1 = 0, a1 = 1.0f;
                    float r2 = 0, g2 = 0, b2 = 0, a2 = 1.0f;
                    float p1 = 0.5f;

                    char *pct1 = strchr(c1_str, '%');
                    if (pct1)
                    {
                        char *num_start = pct1;
                        while (num_start > c1_str && (isdigit((unsigned char)num_start[-1]) || num_start[-1] == '.'))
                            num_start--;
                        p1 = (float)atof(num_start) / 100.0f;
                        *num_start = 0;
                    }
                    char *pct2 = strchr(c2_str, '%');
                    if (pct2)
                    {
                        char *num_start = pct2;
                        while (num_start > c2_str && (isdigit((unsigned char)num_start[-1]) || num_start[-1] == '.'))
                            num_start--;
                        *num_start = 0;
                    }

                    if (mini_parse_color(c1_str, &r1, &g1, &b1, &a1) &&
                        mini_parse_color(c2_str, &r2, &g2, &b2, &a2))
                    {
                        *r = r1 * p1 + r2 * (1.0f - p1);
                        *g = g1 * p1 + g2 * (1.0f - p1);
                        *b = b1 * p1 + b2 * (1.0f - p1);
                        *a = a1 * p1 + a2 * (1.0f - p1);
                        return 1;
                    }
                }
            }
        }
    }

    /* functional: rgb()/rgba(), hsl()/hsla(), oklab(), oklch(), hwb() */
    char *open = strchr(low, '(');
    char *close = strrchr(low, ')');
    if (open && close && close > open)
    {
        char args[128];
        size_t n = 0;
        for (char *p = open + 1; p < close && n < sizeof args - 1; p++)
            args[n++] = *p;
        args[n] = 0;
        float cv[4];
        int pct[4];
        int got = color_components(args, cv, pct);
        if (got >= 3)
        {
            if (!strncmp(low, "rgb", 3))
            {
                float ch[3];
                for (int i = 0; i < 3; i++)
                    ch[i] = pct[i] ? cv[i] / 100.0f : cv[i] / 255.0f;
                for (int i = 0; i < 3; i++)
                {
                    if (ch[i] < 0.0f)
                        ch[i] = 0.0f;
                    if (ch[i] > 1.0f)
                        ch[i] = 1.0f;
                }
                *r = ch[0];
                *g = ch[1];
                *b = ch[2];
                *a = (got >= 4) ? (pct[3] ? cv[3] / 100.0f : cv[3]) : 1.0f;
                if (*a < 0.0f)
                    *a = 0.0f;
                if (*a > 1.0f)
                    *a = 1.0f;
                return 1;
            }
            if (!strncmp(low, "hsl", 3))
            {
                float h = cv[0];
                while (h >= 360.0f)
                    h -= 360.0f;
                while (h < 0.0f)
                    h += 360.0f;
                h /= 360.0f;
                float sat = cv[1] / 100.0f;
                float lum = cv[2] / 100.0f;
                if (sat < 0.0f)
                    sat = 0.0f;
                if (sat > 1.0f)
                    sat = 1.0f;
                if (lum < 0.0f)
                    lum = 0.0f;
                if (lum > 1.0f)
                    lum = 1.0f;
                hsl_to_rgb(h, sat, lum, r, g, b);
                *a = (got >= 4) ? (pct[3] ? cv[3] / 100.0f : cv[3]) : 1.0f;
                if (*a < 0.0f)
                    *a = 0.0f;
                if (*a > 1.0f)
                    *a = 1.0f;
                return 1;
            }
            if (!strncmp(low, "oklab", 5))
            {
                float L_val = pct[0] ? cv[0] / 100.0f : cv[0];
                float a_val = cv[1];
                float b_val = cv[2];
                oklab_to_rgb(L_val, a_val, b_val, r, g, b);
                *a = (got >= 4) ? (pct[3] ? cv[3] / 100.0f : cv[3]) : 1.0f;
                return 1;
            }
            if (!strncmp(low, "oklch", 5))
            {
                /* 兼容 oklch(from var(...) ... / alpha) 相对颜色语法 */
                if (strstr(args, "from"))
                {
                    const char *slash = strchr(args, '/');
                    float alpha_rel = slash ? (float)atof(slash + 1) : 0.75f;
                    *r = 0.08f;
                    *g = 0.09f;
                    *b = 0.11f; /* 默认毛玻璃深色底色 */
                    *a = alpha_rel;
                    return 1;
                }

                float L_val = pct[0] ? cv[0] / 100.0f : cv[0];
                float C_val = cv[1];
                float H_deg = cv[2];
                float H_rad = H_deg * (3.14159265f / 180.0f);
                float a_val = C_val * cosf(H_rad);
                float b_val = C_val * sinf(H_rad);
                oklab_to_rgb(L_val, a_val, b_val, r, g, b);

                /* 提取斜杠后可能存在的 Alpha 值（例如 oklch(0.66 0.19 30 / 0.15)） */
                const char *slash = strchr(args, '/');
                if (slash)
                {
                    *a = (float)atof(slash + 1);
                }
                else
                {
                    *a = (got >= 4) ? (pct[3] ? cv[3] / 100.0f : cv[3]) : 1.0f;
                }
                return 1;
            }
            if (!strncmp(low, "hwb", 3))
            {
                float h_val = cv[0];
                float w_val = pct[1] ? cv[1] / 100.0f : cv[1];
                float b_val = pct[2] ? cv[2] / 100.0f : cv[2];
                hwb_to_rgb(h_val, w_val, b_val, r, g, b);
                *a = (got >= 4) ? (pct[3] ? cv[3] / 100.0f : cv[3]) : 1.0f;
                return 1;
            }
        }
    }

    /* named color */
    return named_color_lookup(low, r, g, b, a);
}

/* Legacy entry: every call site funnels through the full parser above. */
static int parse_hex_color(const char *val, float *r, float *g, float *b, float *a)
{
    return mini_parse_color(val, r, g, b, a);
}

void mini_dom_render_into(struct MiniNode *node, MiniRenderer *r)
{
    if (!node || !r)
        return;
    float sx = (g_active_render_doc) ? g_active_render_doc->scroll_x : 0.0f;
    float sy = (g_active_render_doc) ? g_active_render_doc->scroll_y : 0.0f;
    if (sx > 0.0f || sy > 0.0f)
    {
        mini_draw_push_xform_full(r, 0, 0, -sx, -sy, 0, 0, 0, 0, 1, 1, 0, 0, 0.0f);
        render_node(node, r);
        mini_draw_pop_xform(r);
    }
    else
    {
        render_node(node, r);
    }
}

/* ------------------------------------------------------------------ */
/* CDP DOM tree serialization (Elements panel).                        */
/* ------------------------------------------------------------------ */
typedef struct
{
    char *p;
    size_t cap;
    size_t len;
    int next_id;
    int use_stable; /* 1 = use n->cdp_node_id for ids; 0 = next_id++ */
    int max_depth;  /* -1 = unlimited; 0 = node only; N = children then N-1 */
} CdpSer;
static void ser_putc(CdpSer *s, char c)
{
    if (s->len + 1 < s->cap)
        s->p[s->len++] = c;
}
static void ser_puts(CdpSer *s, const char *str)
{
    while (*str && s->len + 1 < s->cap)
        s->p[s->len++] = *str++;
}
static void ser_json_str(CdpSer *s, const char *v)
{
    ser_putc(s, '"');
    for (const char *p = v; *p; p++)
    {
        if (*p == '"')
        {
            ser_puts(s, "\\\"");
        }
        else if (*p == '\\')
        {
            ser_puts(s, "\\\\");
        }
        else if (*p == '\n')
        {
            ser_puts(s, "\\n");
        }
        else if (*p == '\r')
        {
            ser_puts(s, "\\r");
        }
        else
            ser_putc(s, *p);
    }
    ser_putc(s, '"');
}
static int ser_count_children(struct MiniNode *n)
{
    int c = 0;
    for (struct MiniNode *ch = n->first_child; ch; ch = ch->next_sibling)
        c++;
    if (n->shadow_root)
        c++; /* shadow root counts as a child  */
    return c;
}
/* uppercase a tag for nodeName (HTML element names are canonicalized
   uppercase by the DOM; SVG/MathML keep case, but we only have HTML).   */
static void ser_puts_upper(CdpSer *s, const char *v)
{
    for (; *v; v++)
        ser_putc(s, (char)toupper((unsigned char)*v));
}
static void ser_node(CdpSer *s, struct MiniNode *n, int parent_id)
{
    if (!n)
    {
        return;
    }
    int id = s->use_stable ? n->cdp_node_id : s->next_id++;
    ser_putc(s, '{');
    char tmp[96];
    snprintf(tmp, sizeof tmp, "\"nodeId\":%d,\"backendNodeId\":%d,\"parentId\":%d,", id, id, parent_id);
    ser_puts(s, tmp);

    /* nodeType + nodeName + nodeValue by DOM Node subclass */
    const char *node_name = "#text";
    int nt = 1;
    switch (n->type)
    {
    case MN_DOCUMENT_NODE:
        nt = 9;
        node_name = "#document";
        break;
    case MN_ATTRIBUTE_NODE:
        nt = 2;
        node_name = n->tag ? n->tag : "";
        break;
    case MN_TEXT_NODE:
        nt = 3;
        node_name = "#text";
        break;
    case MN_CDATA_SECTION_NODE:
        nt = 4;
        node_name = "#cdata-section";
        break;
    case MN_PROCESSING_INSTRUCTION:
        nt = 7;
        node_name = n->tag ? n->tag : "";
        break;
    case MN_COMMENT_NODE:
        nt = 8;
        node_name = "#comment";
        break;
    case MN_DOCUMENT_TYPE_NODE:
        nt = 10;
        node_name = n->tag ? n->tag : "";
        break;
    case MN_DOCUMENT_FRAGMENT_NODE:
        nt = 11;
        node_name = "#document-fragment";
        break;
    default:
        nt = 1;
        node_name = n->tag ? n->tag : "";
        break;
    }
    snprintf(tmp, sizeof tmp, "\"nodeType\":%d,", nt);
    ser_puts(s, tmp);
    ser_puts(s, "\"nodeName\":\"");
    if (nt == 1)
    {
        if (n->tag)
            ser_puts_upper(s, n->tag);
    }
    else
        ser_puts(s, node_name);
    ser_puts(s, "\",\"nodeValue\":");
    if ((nt == 3 || nt == 8 || nt == 2 || nt == 4) && n->text)
        ser_json_str(s, n->text);
    else
        ser_puts(s, "\"\"");
    ser_putc(s, ',');

    /* attributes for element nodes (flat alternating [name, value, ...] array per CDP spec) */
    if (nt == 1)
    {
        ser_puts(s, "\"attributes\":[");
        int first = 1;
        for (MiniAttr *a = n->attrs; a; a = a->next)
        {
            if (!first)
                ser_putc(s, ',');
            first = 0;
            ser_json_str(s, a->name ? a->name : "");
            ser_putc(s, ',');
            ser_json_str(s, a->value ? a->value : "");
        }
        ser_puts(s, "],");
    }
    int cc = 0;
    for (struct MiniNode *ch = n->first_child; ch; ch = ch->next_sibling)
    {
        if (ch->tag && ch->tag[0] == ':' && ch->tag[1] == ':')
            continue;
        if (ch->type == MN_TEXT_NODE && (!ch->text || is_all_ws(ch->text)))
            continue;
        cc++;
    }
    if (n->shadow_root)
        cc++;

    snprintf(tmp, sizeof tmp, "\"childNodeCount\":%d", cc);
    ser_puts(s, tmp);
    if (cc > 0 && s->max_depth != 0)
    {
        int saved = s->max_depth;
        s->max_depth = (saved < 0) ? -1 : saved - 1;
        ser_puts(s, ",\"children\":[");
        int first = 1;
        for (struct MiniNode *ch = n->first_child; ch; ch = ch->next_sibling)
        {
            if (ch->tag && ch->tag[0] == ':' && ch->tag[1] == ':')
                continue;
            if (ch->type == MN_TEXT_NODE && (!ch->text || is_all_ws(ch->text)))
                continue;
            if (!first)
                ser_putc(s, ',');
            first = 0;
            ser_node(s, ch, id);
        }
        if (n->shadow_root)
        {
            if (!first)
                ser_putc(s, ',');
            ser_node(s, n->shadow_root, id);
        }
        ser_putc(s, ']');
        s->max_depth = saved;
    }
    ser_putc(s, '}');
}
/* ================================================================== */
/* Phase 1.2: micro CSS selector matcher                              */
/* Comma-separated compound selectors: tag, .class, #id, *, [attr],    */
/* [attr=val]. No combinators/pseudo — kept tiny.                     */
/* ================================================================== */
static int ci_eq_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    return 1;
}
static int ci_eq(const char *a, const char *b)
{ /* case-insensitive */
    if (!a || !b)
        return a == b;
    while (*a && *b)
    {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}
static int class_has(const char *classval, const char *tok)
{ /* whitespace-split */
    if (!classval || !tok)
        return 0;
    const char *p = classval;
    while (*p)
    {
        while (*p && isspace((unsigned char)*p))
            p++;
        const char *s = p;
        while (*p && !isspace((unsigned char)*p))
            p++;
        size_t n = (size_t)(p - s);
        if (strlen(tok) == n && ci_eq_n(s, tok, n))
            return 1;
    }
    return 0;
}
/* element-sibling navigation (text/comment siblings are skipped) */
static struct MiniNode *prev_elem_sibling(const struct MiniNode *n)
{
    for (struct MiniNode *s = n->prev_sibling; s; s = s->prev_sibling)
        if (s->type == MN_ELEMENT_NODE)
            return s;
    return NULL;
}
static struct MiniNode *next_elem_sibling(const struct MiniNode *n)
{
    for (struct MiniNode *s = n->next_sibling; s; s = s->next_sibling)
        if (s->type == MN_ELEMENT_NODE)
            return s;
    return NULL;
}
static int is_first_element_child(const struct MiniNode *n)
{
    if (!n->parent)
        return 0;
    return prev_elem_sibling(n) == NULL;
}
static int is_last_element_child(const struct MiniNode *n)
{
    if (!n->parent)
        return 0;
    return next_elem_sibling(n) == NULL;
}
static int is_first_of_type(const struct MiniNode *n)
{
    if (!n->parent || !n->tag)
        return 0;
    for (struct MiniNode *s = prev_elem_sibling(n); s; s = prev_elem_sibling(s))
        if (s->tag && ci_eq(s->tag, n->tag))
            return 0;
    return 1;
}
static int is_last_of_type(const struct MiniNode *n)
{
    if (!n->parent || !n->tag)
        return 0;
    for (struct MiniNode *s = next_elem_sibling(n); s; s = next_elem_sibling(s))
        if (s->tag && ci_eq(s->tag, n->tag))
            return 0;
    return 1;
}

/* parse an An+B argument (nth-child family). Returns 1 on success. */
static int parse_anb(const char *s0, int *a, int *b)
{
    *a = 0;
    *b = 0;
    const char *s = s0;
    while (*s && isspace((unsigned char)*s))
        s++;
    if (!*s)
        return 0;
    if (!strcmp(s, "odd"))
    {
        *a = 2;
        *b = 1;
        return 1;
    }
    if (!strcmp(s, "even"))
    {
        *a = 2;
        *b = 0;
        return 1;
    }
    char *end;
    if (*s == 'n' || (*s == '+' && s[1] == 'n') || (*s == '-' && s[1] == 'n'))
    {
        *a = (*s == '-') ? -1 : 1;
        if (*s == '+' || *s == '-')
            s++;
        s++; /* 'n' */
        if (*s == '+' || *s == '-')
            *b = (int)strtol(s, &end, 10);
        else if (*s)
            return 0;
        return 1;
    }
    long coef = strtol(s, &end, 10);
    if (end == s)
        return 0;
    if (*end == 0)
    {
        *a = 0;
        *b = (int)coef;
        return 1;
    }
    if (*end == 'n')
    {
        *a = (int)coef;
        s = end + 1;
        if (*s == '+' || *s == '-')
            *b = (int)strtol(s, &end, 10);
        else if (*s)
            return 0;
        return 1;
    }
    return 0;
}

/* 1-based index of n among element siblings (front or back) */
static int elem_index(const struct MiniNode *n, int from_end)
{
    int idx = 1;
    if (!from_end)
        for (struct MiniNode *s = prev_elem_sibling(n); s; s = prev_elem_sibling(s))
            idx++;
    else
        for (struct MiniNode *s = next_elem_sibling(n); s; s = next_elem_sibling(s))
            idx++;
    return idx;
}
static int elem_index_of_type(const struct MiniNode *n, int from_end)
{
    if (!n->tag)
        return 0;
    int idx = 1;
    if (!from_end)
    {
        for (struct MiniNode *s = prev_elem_sibling(n); s; s = prev_elem_sibling(s))
            if (s->tag && ci_eq(s->tag, n->tag))
                idx++;
    }
    else
    {
        for (struct MiniNode *s = next_elem_sibling(n); s; s = next_elem_sibling(s))
            if (s->tag && ci_eq(s->tag, n->tag))
                idx++;
    }
    return idx;
}
static int nth_test(int idx, int a, int b)
{
    if (a == 0)
        return idx == b;
    int diff = idx - b;
    if (diff % a != 0)
        return 0;
    return diff / a >= 0;
}

/* case-insensitive substring */
static int ci_contains(const char *hay, const char *needle)
{
    if (!hay || !needle)
        return 0;
    size_t hl = strlen(hay), nl = strlen(needle);
    if (nl == 0)
        return 1;
    if (hl < nl)
        return 0;
    for (size_t i = 0; i + nl <= hl; i++)
        if (ci_eq_n(hay + i, needle, nl))
            return 1;
    return 0;
}

/* attribute operator match. op: 1= 2~= 3^= 4$= 5*= 6|= */
static int attr_op_match(const char *got, int op, const char *av)
{
    if (!got)
        return 0;
    size_t al = strlen(av);
    switch (op)
    {
    case 1:
        return ci_eq(got, av);
    case 2:
        return class_has(got, av); /* ~= whitespace-token contains */
    case 3:
        return ci_eq_n(got, av, al); /* ^= */
    case 4:
    {
        size_t gl = strlen(got);
        return gl >= al && ci_eq_n(got + gl - al, av, al);
    }
    case 5:
        return ci_contains(got, av); /* *= */
    case 6:
        return ci_eq(got, av) || (ci_eq_n(got, av, al) && got[al] == '-');
    }
    return 0;
}

static int match_complex(const struct MiniNode *n, const char *sel);

/* match a pseudo-class/element. arg is the (...) text ("" if none). */
static int match_pseudo(const struct MiniNode *n, const char *name,
                        const char *arg, int is_element)
{
    if (!n || !name)
        return 0;
    if (!strcmp(name, "first-child"))
        return is_first_element_child(n);
    if (!strcmp(name, "last-child"))
        return is_last_element_child(n);
    if (!strcmp(name, "only-child"))
        return is_first_element_child(n) && is_last_element_child(n);
    if (!strcmp(name, "empty"))
        return !n->first_child;
    if (!strcmp(name, "root"))
        return (n->type == MN_DOCUMENT_NODE || !n->parent || (n->parent && n->parent->type == MN_DOCUMENT_NODE && n->tag && !strcmp(n->tag, "html")));
    if (!strcmp(name, "nth-child"))
    {
        int a, b;
        return parse_anb(arg, &a, &b) && nth_test(elem_index(n, 0), a, b);
    }
    if (!strcmp(name, "nth-last-child"))
    {
        int a, b;
        return parse_anb(arg, &a, &b) && nth_test(elem_index(n, 1), a, b);
    }
    if (!strcmp(name, "nth-of-type"))
    {
        int a, b;
        return parse_anb(arg, &a, &b) && nth_test(elem_index_of_type(n, 0), a, b);
    }
    if (!strcmp(name, "nth-last-of-type"))
    {
        int a, b;
        return parse_anb(arg, &a, &b) && nth_test(elem_index_of_type(n, 1), a, b);
    }
    if (!strcmp(name, "first-of-type"))
        return is_first_of_type(n);
    if (!strcmp(name, "last-of-type"))
        return is_last_of_type(n);
    if (!strcmp(name, "only-of-type"))
        return is_first_of_type(n) && is_last_of_type(n);
    if (!strcmp(name, "is") || !strcmp(name, "where"))
    {
        if (!arg[0])
            return 0;
        char ab[256];
        strncpy(ab, arg, sizeof(ab) - 1);
        ab[sizeof(ab) - 1] = 0;
        char *sv = NULL;
        for (char *t = strtok_r(ab, ",", &sv); t; t = strtok_r(NULL, ",", &sv))
        {
            while (*t && isspace((unsigned char)*t))
                t++;
            if (match_complex(n, t))
                return 1;
        }
        return 0;
    }
    if (!strcmp(name, "has"))
    {
        if (!arg[0])
            return 0;
        const char *a_cur = arg;
        while (*a_cur && isspace((unsigned char)*a_cur))
            a_cur++;
        if (*a_cur == '>')
        {
            const char *child_sel = a_cur + 1;
            while (*child_sel && isspace((unsigned char)*child_sel))
                child_sel++;
            for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
            {
                if (c->type == MN_ELEMENT_NODE && match_complex(c, child_sel))
                    return 1;
            }
            return 0;
        }
        else if (*a_cur == '+')
        {
            const struct MiniNode *nxt = next_elem_sibling(n);
            return nxt ? match_complex(nxt, a_cur + 1) : 0;
        }
        else if (*a_cur == '~')
        {
            for (const struct MiniNode *s = next_elem_sibling(n); s; s = next_elem_sibling(s))
            {
                if (match_complex(s, a_cur + 1))
                    return 1;
            }
            return 0;
        }
        /* Descendant search */
        struct MiniNode *stack[64];
        int sp = 0;
        for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
            if (sp < 64)
                stack[sp++] = c;
        while (sp > 0)
        {
            struct MiniNode *cur = stack[--sp];
            if (cur->type == MN_ELEMENT_NODE)
            {
                if (match_complex(cur, a_cur))
                    return 1;
                for (struct MiniNode *c = cur->first_child; c; c = c->next_sibling)
                    if (sp < 64)
                        stack[sp++] = c;
            }
        }
        return 0;
    }
    if (!strcmp(name, "not"))
    {
        if (!arg[0])
            return 0;
        char ab[128];
        strncpy(ab, arg, sizeof ab - 1);
        ab[sizeof ab - 1] = 0;
        char *sv = NULL;
        int any = 0;
        for (char *t = strtok_r(ab, ",", &sv); t; t = strtok_r(NULL, ",", &sv))
        {
            while (*t && isspace((unsigned char)*t))
                t++;
            if (match_complex(n, t))
            {
                any = 1;
                break;
            }
        }
        return !any;
    }
    /* UI-state pseudos: the interaction bits are set by mini_events.c (hit-
       test / focus) and read here so :hover/:active/:focus rules apply at the
       next restyle. focus-within also matches if any child is focused. */
    if (!strcmp(name, "hover"))
        return n->state_hovered ? 1 : 0;
    if (!strcmp(name, "active"))
        return n->state_active ? 1 : 0;
    if (!strcmp(name, "focus") || !strcmp(name, "focus-visible"))
        return n->state_focused ? 1 : 0;
    if (!strcmp(name, "focus-within"))
    {
        if (n->state_focused)
            return 1;
        for (struct MiniNode *a = n->first_child; a; a = a->next_sibling)
            if (a->state_focused)
                return 1;
        return 0;
    }
    if (!strcmp(name, "checked"))
    {
        const char *chk = mini_node_get_attribute(n, "checked");
        const char *sel = mini_node_get_attribute(n, "selected");
        return (chk != NULL) || (sel != NULL);
    }
    if (!strcmp(name, "disabled"))
    {
        if (mini_node_get_attribute(n, "disabled"))
            return 1;
        for (const struct MiniNode *p = n->parent; p; p = p->parent)
        {
            if (p->tag && !strcmp(p->tag, "fieldset") && mini_node_get_attribute(p, "disabled"))
                return 1;
        }
        return 0;
    }
    if (!strcmp(name, "enabled"))
    {
        if (n->tag && (!strcmp(n->tag, "input") || !strcmp(n->tag, "button") ||
                       !strcmp(n->tag, "select") || !strcmp(n->tag, "textarea") ||
                       !strcmp(n->tag, "option") || !strcmp(n->tag, "fieldset")))
        {
            return !match_pseudo(n, "disabled", "", 0);
        }
        return 0;
    }
    if (!strcmp(name, "required"))
    {
        return mini_node_get_attribute(n, "required") != NULL;
    }
    if (!strcmp(name, "placeholder-shown"))
    {
        const char *ph = mini_node_get_attribute(n, "placeholder");
        const char *val = mini_node_get_attribute(n, "value");
        if (!ph)
            return 0;
        return (!val || val[0] == '\0') && (!n->text || n->text[0] == '\0');
    }
    if (!strcmp(name, "placeholder"))
        return (n->tag && (!strcmp(n->tag, "input") || !strcmp(n->tag, "textarea")));
    if (!strcmp(name, "selection"))
        return 0;
    if (!strcmp(name, "visited") || !strcmp(name, "link"))
        return 0;
    if (is_element)
        return 0;
    return 0;
}

/* match one compound selector against an element node */
static int match_compound(const struct MiniNode *n, const char *sel)
{
    if (!n || !sel)
        return 0;
    if (n->type != MN_ELEMENT_NODE && n->type != MN_DOCUMENT_NODE)
        return 0;
    const char *p = sel;
    /* optional leading type / universal selector */
    if (*p && *p != '.' && *p != '#' && *p != '[' && *p != ':')
    {
        char tag[32];
        int ti = 0;
        while (*p && *p != '.' && *p != '#' && *p != '[' && *p != ':' && ti < 31)
            tag[ti++] = (char)tolower((unsigned char)*p++);
        tag[ti] = 0;
        if (ti)
        {
            if (tag[0] == '*' && ti == 1)
            { /* universal */
            }
            else if (!n->tag || !ci_eq(n->tag, tag))
                return 0;
        }
    }
    while (*p)
    {
        if (*p == '.')
        {
            p++;
            char tok[64];
            int ci = 0;
            while (*p && *p != '.' && *p != '#' && *p != '[' && *p != ':' && ci < 63)
                tok[ci++] = (char)tolower((unsigned char)*p++);
            tok[ci] = 0;
            if (!class_has(mini_node_get_attribute(n, "class"), tok))
                return 0;
        }
        else if (*p == '#')
        {
            p++;
            char id[64];
            int ii = 0;
            while (*p && *p != '.' && *p != '#' && *p != '[' && *p != ':' && ii < 63)
                id[ii++] = (char)tolower((unsigned char)*p++);
            id[ii] = 0;
            const char *v = mini_node_get_attribute(n, "id");
            if (!v || !ci_eq(v, id))
                return 0;
        }
        else if (*p == '[')
        {
            p++;
            char an[64];
            int ai = 0;
            while (*p && *p != ']' && *p != '=' && *p != '~' && *p != '^' &&
                   *p != '$' && *p != '*' && *p != '|' && ai < 63)
                an[ai++] = (char)tolower((unsigned char)*p++);
            an[ai] = 0;
            int op = 0;
            if (*p == '=')
            {
                op = 1;
                p++;
            }
            else if (p[0] && p[1] == '=' &&
                     (p[0] == '~' || p[0] == '^' || p[0] == '$' ||
                      p[0] == '*' || p[0] == '|'))
            {
                op = (p[0] == '~') ? 2 : (p[0] == '^') ? 3
                                     : (p[0] == '$')   ? 4
                                     : (p[0] == '*')   ? 5
                                                       : 6;
                p += 2;
            }
            if (op)
            {
                char av[128];
                int vi = 0;
                if (*p == '"' || *p == '\'')
                {
                    char q = *p++;
                    while (*p && *p != q && vi < 127)
                        av[vi++] = *p++;
                    if (*p == q)
                        p++;
                }
                else
                {
                    while (*p && *p != ']' && vi < 127)
                        av[vi++] = *p++;
                }
                av[vi] = 0;
                if (!attr_op_match(mini_node_get_attribute(n, an), op, av))
                    return 0;
            }
            else
            {
                if (!mini_node_get_attribute(n, an))
                    return 0;
            }
            if (*p == ']')
                p++;
        }
        else if (*p == ':')
        {
            p++;
            int is_el = 0;
            if (*p == ':')
            {
                is_el = 1;
                p++;
            }
            char pname[32];
            int pi = 0;
            while (*p && (isalnum((unsigned char)*p) || *p == '-') && pi < 31)
                pname[pi++] = (char)tolower((unsigned char)*p++);
            pname[pi] = 0;
            char parg[128];
            parg[0] = 0;
            if (*p == '(')
            {
                p++;
                int ai = 0;
                int depth = 1;
                while (*p && depth > 0)
                {
                    if (*p == '(')
                        depth++;
                    else if (*p == ')')
                    {
                        depth--;
                        if (depth == 0)
                        {
                            p++;
                            break;
                        }
                    }
                    if (ai < 127)
                        parg[ai++] = *p;
                    p++;
                }
                parg[ai] = 0;
            }
            if (!match_pseudo(n, pname, parg, is_el))
                return 0;
        }
        else
            break;
    }
    return 1;
}

/* split a complex selector into compound segments + leading combinators.
   comb[i] is the combinator before seg[i] (comb[0] unused). Returns nseg. */
static int parse_complex(const char *s0, char seg[][128], int *comb, int maxseg)
{
    int nseg = 0, i = 0;
    const char *s = s0;
    while (*s && isspace((unsigned char)*s))
        s++;
    if (!*s)
        return 0;
    nseg = 1;
    seg[0][0] = 0;
    i = 0;
    comb[0] = 0;
    int saw_ws = 0;
    while (*s)
    {
        char c = *s;
        if (c == '[')
        {
            if (i < 127)
                seg[nseg - 1][i++] = c;
            s++;
            while (*s && *s != ']')
            {
                if (i < 127)
                    seg[nseg - 1][i++] = *s;
                s++;
            }
            if (*s == ']')
            {
                if (i < 127)
                    seg[nseg - 1][i++] = ']';
                s++;
            }
            saw_ws = 0;
            continue;
        }
        if (c == ':')
        {
            if (i < 127)
                seg[nseg - 1][i++] = c;
            s++;
            if (*s == ':')
            {
                if (i < 127)
                    seg[nseg - 1][i++] = *s++;
            }
            while (*s && (isalnum((unsigned char)*s) || *s == '-'))
            {
                if (i < 127)
                    seg[nseg - 1][i++] = *s;
                s++;
            }
            if (*s == '(')
            {
                int depth = 0;
                do
                {
                    if (*s == '(')
                        depth++;
                    else if (*s == ')')
                        depth--;
                    if (i < 127)
                        seg[nseg - 1][i++] = *s;
                    s++;
                } while (*s && depth > 0);
            }
            saw_ws = 0;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f')
        {
            saw_ws = 1;
            s++;
            continue;
        }
        if (c == '>' || c == '+' || c == '~')
        {
            seg[nseg - 1][i] = 0;
            if (nseg < maxseg)
            {
                comb[nseg] = (int)c;
                nseg++;
                seg[nseg - 1][0] = 0;
                i = 0;
            }
            s++;
            while (*s && isspace((unsigned char)*s))
                s++;
            saw_ws = 0;
            continue;
        }
        /* normal compound character */
        if (saw_ws && i > 0)
        {
            seg[nseg - 1][i] = 0;
            if (nseg < maxseg)
            {
                comb[nseg] = ' ';
                nseg++;
                seg[nseg - 1][0] = 0;
                i = 0;
            }
            saw_ws = 0;
        }
        if (i < 127)
            seg[nseg - 1][i++] = c;
        s++;
    }
    seg[nseg - 1][i] = 0;
    return nseg;
}

/* match n against the full complex selector, right-to-left. */
static int match_at(const struct MiniNode *n, char seg[][128], int *comb,
                    int nseg, int from_right)
{
    int idx = nseg - 1 - from_right;
    if (idx < 0)
        return 1; /* all segments matched */
    if (!match_compound(n, seg[idx]))
        return 0;
    if (idx == 0)
        return 1;
    int cb = comb[idx]; /* combinator to the LEFT of seg[idx] */
    switch (cb)
    {
    case (int)'>':
        return n->parent && match_at(n->parent, seg, comb, nseg, from_right + 1);
    case (int)' ':
        for (struct MiniNode *a = n->parent; a; a = a->parent)
            if (match_at(a, seg, comb, nseg, from_right + 1))
                return 1;
        return 0;
    case (int)'+':
    {
        struct MiniNode *ps = prev_elem_sibling(n);
        return ps && match_at(ps, seg, comb, nseg, from_right + 1);
    }
    case (int)'~':
        for (struct MiniNode *ps = prev_elem_sibling(n); ps; ps = prev_elem_sibling(ps))
            if (match_at(ps, seg, comb, nseg, from_right + 1))
                return 1;
        return 0;
    }
    return 0;
}

/* ---- compiled-selector cache ----
   A selector string tokenizes deterministically (parse_complex is pure),
   so we can cache the (seg, comb, nseg) result keyed by the selector
   string and reuse it for every subsequent match against any node. The
   apply pass calls match_complex ~ndecls × nodes ≈ 1M+ times per
   restyle; without this cache every call re-tokenized the selector
   from scratch. Lookup is by strcmp so stale entries from an old page
   never match a new selector (just waste a slot); when the table fills
   we clear it wholesale. Memory: ~4 MB static for 2048 slots.            */

static uint32_t cs_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
    {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h & (MINI_CS_CAP - 1);
}

/* Return a pointer to the cached compiled form of `sel`, compiling it
   on first sight. The returned pointer is valid until the cache is
   cleared (never during a single match call). */
static CompiledSel *cs_get(const char *sel)
{
    uint32_t h = cs_hash(sel);
    for (int i = 0; i < 8; i++)
    {
        CompiledSel *c = &g_cs[(h + i) & (MINI_CS_CAP - 1)];
        if (!c->used)
            break; /* empty slot ends the probe chain */
        if (!strcmp(c->sel, sel))
            return c; /* cache hit */
    }
    /* miss: compile into the first empty slot in the probe chain */
    for (int i = 0; i < 8; i++)
    {
        CompiledSel *c = &g_cs[(h + i) & (MINI_CS_CAP - 1)];
        if (!c->used)
        {
            snprintf(c->sel, sizeof c->sel, "%s", sel);
            c->nseg = parse_complex(c->sel, c->seg, c->comb, 16);
            c->used = 1;
            g_cs_n++;
            return c;
        }
    }
    /* 8-slot chain full: if the table is mostly full, clear it and retry
       once so a flood of new selectors doesn't permanently miss. */
    if (g_cs_n > (MINI_CS_CAP * 3) / 4)
    {
        memset(g_cs, 0, sizeof g_cs);
        g_cs_n = 0;
        return cs_get(sel);
    }
    return NULL; /* rare chain overflow: caller falls back to stack parse */
}

static int match_complex(const struct MiniNode *n, const char *sel)
{
    if (!n || !sel || !*sel)
        return 0;
    CompiledSel *c = cs_get(sel);
    if (c)
    {
        if (c->nseg <= 0)
            return 0;
        return match_at(n, c->seg, c->comb, c->nseg, 0);
    }
    /* fallback: stack parse (rare, only on probe-chain overflow) */
    char seg[16][128];
    int comb[16];
    int nseg = parse_complex(sel, seg, comb, 16);
    if (nseg <= 0)
        return 0;
    return match_at(n, seg, comb, nseg, 0);
}

static void walk_collect(const struct MiniNode *n, const char *sel,
                         struct MiniNode **out, int *count, int max)
{
    if (!n)
        return;
    if (match_complex(n, sel))
        if (*count < max)
            out[(*count)++] = (struct MiniNode *)n;
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        walk_collect(c, sel, out, count, max);
}

int mini_dom_matches_selector(const struct MiniNode *n, const char *selector)
{
    if (!n || !selector || !*selector)
        return 0;
    return match_complex(n, selector);
}

int mini_dom_query_selector_all(struct MiniDocument *doc, const char *selector,
                                struct MiniNode **out, int max)
{
    if (!doc || !selector || !out || max <= 0)
        return 0;
    int count = 0;
    char buf[512];
    strncpy(buf, selector, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *p = buf;
    while (*p && count < max)
    {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        char *tok_start = p;
        int depth = 0;
        while (*p)
        {
            if (*p == '(' || *p == '[')
                depth++;
            else if (*p == ')' || *p == ']')
            {
                if (depth > 0)
                    depth--;
            }
            else if (*p == ',' && depth == 0)
                break;
            p++;
        }
        if (*p == ',')
        {
            *p = '\0';
            p++;
        }
        char *tok = tok_start;
        char *e = tok + strlen(tok);
        while (e > tok && isspace((unsigned char)e[-1]))
            e--;
        *e = 0;
        if (*tok)
            walk_collect(doc->root ? doc->root : doc->body, tok, out, &count, max);
    }
    return count;
}
struct MiniNode *mini_dom_query_selector(struct MiniDocument *doc, const char *selector)
{
    struct MiniNode *out[1];
    if (mini_dom_query_selector_all(doc, selector, out, 1) > 0)
        return out[0];
    return NULL;
}

/* ---- Phase 1.2b: CSS cascade applier (specificity + !important) --- */
/* Parse `selector { prop:val; ... }` rules, compute each selector's
   specificity, detect `!important`, then apply declarations in
   ascending (important, specificity, source-order) so the highest-
   priority declaration writes last (last-writer-wins on the field).
   Inline styles (setAttribute / element.style.x in the JS bridge)
   still run AFTER this and win over stylesheet normal declarations;
   the inline-normal-vs-important-stylesheet edge is a known deferral. */
static uint32_t spec_of(const char *sel)
{
    /* specificity a*10000 + b*100 + c: a=#id, b=#(class/attr/pseudo-class),
       c=#(type/pseudo-element). Sums across all compounds of a complex
       selector (combinators add nothing); comma lists take the max part. */
    uint32_t best = 0, a = 0, b = 0, c = 0;
    int at_start = 1;
    const char *p = sel;
    while (*p)
    {
        char ch = *p;
        if (ch == ',')
        {
            uint32_t s = a * 10000u + b * 100u + c;
            if (s > best)
                best = s;
            a = b = c = 0;
            at_start = 1;
            p++;
            continue;
        }
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' ||
            ch == '>' || ch == '+' || ch == '~')
        {
            at_start = 1; /* a combinator starts a new compound */
            p++;
            continue;
        }
        if (ch == '[')
        {
            b++;
            at_start = 0;
            p++;
            while (*p && *p != ']')
                p++;
            if (*p == ']')
                p++;
            continue;
        }
        if (ch == ':')
        {
            p++;
            int dbl = 0;
            if (*p == ':')
            {
                dbl = 1;
                p++;
            }
            const char *name_start = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '-'))
                p++;
            size_t nlen = (size_t)(p - name_start);
            int is_where = (!dbl && nlen == 5 && !strncmp(name_start, "where", 5));
            int is_is_not = (!dbl && ((nlen == 2 && !strncmp(name_start, "is", 2)) || (nlen == 3 && !strncmp(name_start, "not", 3))));

            if (*p == '(')
            {
                const char *arg_start = p + 1;
                int d = 0;
                do
                {
                    if (*p == '(')
                        d++;
                    else if (*p == ')')
                        d--;
                    p++;
                } while (*p && d > 0);
                const char *arg_end = p - 1;

                if (is_where)
                {
                    /* :where() strictly contributes (0,0,0) */
                }
                else if (is_is_not && arg_end > arg_start)
                {
                    char arg_buf[256];
                    size_t al = (size_t)(arg_end - arg_start);
                    if (al >= sizeof(arg_buf))
                        al = sizeof(arg_buf) - 1;
                    memcpy(arg_buf, arg_start, al);
                    arg_buf[al] = 0;
                    uint32_t inner_s = spec_of(arg_buf);
                    uint32_t in_a = inner_s / 10000u;
                    uint32_t in_b = (inner_s % 10000u) / 100u;
                    uint32_t in_c = inner_s % 100u;
                    a += in_a;
                    b += in_b;
                    c += in_c;
                }
                else
                {
                    if (dbl)
                        c++;
                    else
                        b++;
                }
            }
            else
            {
                if (dbl)
                    c++;
                else
                    b++;
            }
            at_start = 0;
            continue;
        }
        if (ch == '#')
        {
            a++;
            at_start = 0;
            p++;
            while (*p && (isalnum((unsigned char)*p) || *p == '-' || *p == '_'))
                p++;
            continue;
        }
        if (ch == '.')
        {
            b++;
            at_start = 0;
            p++;
            while (*p && (isalnum((unsigned char)*p) || *p == '-' || *p == '_'))
                p++;
            continue;
        }
        if (ch == '*')
        {
            at_start = 0;
            p++;
            continue;
        }
        if (at_start && isalpha((unsigned char)ch))
        {
            c++; /* type selector (once per compound) */
            at_start = 0;
        }
        p++;
    }
    {
        uint32_t s = a * 10000u + b * 100u + c;
        if (s > best)
            best = s;
    }
    return best;
}

/* Strip a trailing "!important" (case-insensitive) from val in place. */
static int strip_important(char *val)
{
    size_t n = strlen(val);
    while (n && isspace((unsigned char)val[n - 1]))
        val[--n] = 0;
    if (n < 10)
        return 0;
    const char *t = val + n - 10;
    static const char imp[] = "!important";
    for (int i = 0; i < 10; i++)
        if (tolower((unsigned char)t[i]) != imp[i])
            return 0;
    val[n - 10] = 0;
    n -= 10;
    while (n && isspace((unsigned char)val[n - 1]))
        val[--n] = 0;
    return 1;
}

static char *strip_css_comments(const char *css)
{
    if (!css)
        return NULL;
    size_t len = strlen(css);
    char *out = (char *)malloc(len + 1);
    if (!out)
        return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len;)
    {
        if (css[i] == '/' && css[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < len && !(css[i] == '*' && css[i + 1] == '/'))
                i++;
            if (i + 1 < len)
                i += 2;
            else
                i = len;
        }
        else
        {
            out[j++] = css[i++];
        }
    }
    out[j] = 0;
    return out;
}

const char *mini_dom_get_stylesheet(const struct MiniDocument *doc)
{
    MiniDocumentContext *ctx = mini_get_ctx((MiniDocument *)doc);
    return ctx->restyle_css_buf ? ctx->restyle_css_buf : "";
}

/* Skip a balanced { ... } block whose opening brace is at *bp; return a
   pointer just past the matching '}' (or to the end of string). */
static const char *css_skip_block(const char *bp)
{
    int depth = 0;
    for (const char *q = bp; *q; q++)
    {
        if (*q == '{')
            depth++;
        else if (*q == '}')
        {
            depth--;
            if (depth == 0)
                return q + 1;
        }
    }
    return bp + strlen(bp);
}
static int mini_style_is_supported_prop_val(const char *prop, const char *val)
{
    if (!prop || !*prop)
        return 0;
    while (*prop && isspace((unsigned char)*prop))
        prop++;
    static const char *known_props[] = {
        "display", "flex-direction", "flex-wrap", "flex-grow", "flex-shrink",
        "flex-basis", "flex", "order", "align-content", "justify-content",
        "align-items", "align-self", "gap", "grid-template-columns",
        "grid-template-rows", "grid-auto-flow", "grid-column", "grid-row",
        "grid-area", "position", "top", "left", "right", "bottom", "z-index",
        "width", "height", "min-width", "max-width", "min-height", "max-height",
        "margin", "padding", "border", "border-radius", "box-shadow",
        "box-sizing", "color", "background", "background-color", "background-image",
        "font-size", "font-weight", "font-family", "font-style", "text-align",
        "text-decoration", "text-transform", "vertical-align", "line-height",
        "letter-spacing", "opacity", "overflow", "transform", "filter",
        "backdrop-filter", "transition", "animation", "cursor", "pointer-events",
        "visibility", "float", "clear"};
    for (size_t i = 0; i < sizeof(known_props) / sizeof(known_props[0]); i++)
    {
        if (!strcmp(prop, known_props[i]))
            return 1;
    }
    return 0;
}

static int css_eval_supports_single(const char *expr)
{
    while (*expr && isspace((unsigned char)*expr))
        expr++;
    if (*expr == '(')
        expr++;
    const char *colon = strchr(expr, ':');
    if (!colon)
        return 0;
    char prop[64] = {0}, val[128] = {0};
    size_t pl = (size_t)(colon - expr);
    if (pl >= sizeof(prop))
        pl = sizeof(prop) - 1;
    strncpy(prop, expr, pl);
    prop[pl] = 0;
    char *pe = prop + strlen(prop) - 1;
    while (pe >= prop && isspace((unsigned char)*pe))
        *pe-- = 0;
    char *ps = prop;
    while (*ps && isspace((unsigned char)*ps))
        ps++;

    const char *vp = colon + 1;
    while (*vp && isspace((unsigned char)*vp))
        vp++;
    const char *ve = vp;
    while (*ve && *ve != ')' && *ve != ';')
        ve++;
    size_t vl = (size_t)(ve - vp);
    if (vl >= sizeof(val))
        vl = sizeof(val) - 1;
    strncpy(val, vp, vl);
    val[vl] = 0;
    char *vpe = val + strlen(val) - 1;
    while (vpe >= val && isspace((unsigned char)*vpe))
        *vpe-- = 0;

    return mini_style_is_supported_prop_val(ps, val);
}

static int css_eval_supports(const char *cond)
{
    if (!cond || !*cond)
        return 1;
    if (strstr(cond, "not ") || strstr(cond, "not("))
    {
        const char *inner = strstr(cond, "(");
        if (inner)
            return !css_eval_supports_single(inner);
    }
    if (strstr(cond, " and "))
    {
        char buf[256];
        strncpy(buf, cond, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        char *save;
        char *t = strtok_r(buf, " and ", &save);
        int res = 1;
        while (t)
        {
            while (*t && isspace((unsigned char)*t))
                t++;
            if (*t)
                res = res && css_eval_supports_single(t);
            t = strtok_r(NULL, " and ", &save);
        }
        return res;
    }
    if (strstr(cond, " or "))
    {
        char buf[256];
        strncpy(buf, cond, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        char *save;
        char *t = strtok_r(buf, " or ", &save);
        int res = 0;
        while (t)
        {
            while (*t && isspace((unsigned char)*t))
                t++;
            if (*t)
                res = res || css_eval_supports_single(t);
            t = strtok_r(NULL, " or ", &save);
        }
        return res;
    }
    return css_eval_supports_single(cond);
}

/* Pre-process a comment-stripped stylesheet: expand @media blocks that match
   the current viewport into their inner rules, extract+store @keyframes and
   @font-face rules, and drop @import/@supports/@page/@charset. Returns a
   malloc'd flat rule string the cascade loop can parse directly.           */
static char *css_expand_at_rules(const char *in, int vw, int vh)
{
    if (!in)
        return strdup("");
    size_t cap = strlen(in) + 64, olen = 0;
    char *out = (char *)malloc(cap);
    if (!out)
        return strdup("");
    const char *p = in;
    while (*p)
    {
        if (*p != '@')
        {
            if (olen + 2 > cap)
            {
                cap *= 2;
                out = realloc(out, cap);
            }
            out[olen++] = *p++;
            continue;
        }
        p++; /* past '@' */
        /* read at-keyword */
        const char *kw0 = p;
        while (*p && (isalpha((unsigned char)*p) || *p == '-'))
            p++;
        char kw[24];
        size_t kwl = (size_t)(p - kw0);
        if (kwl >= sizeof kw)
            kwl = sizeof kw - 1;
        memcpy(kw, kw0, kwl);
        kw[kwl] = 0;
        for (char *q = kw; *q; q++)
            *q = (char)tolower((unsigned char)*q);

        if (!strcmp(kw, "media"))
        {
            const char *cs = p;
            const char *brace = strchr(p, '{');
            if (!brace)
                break;
            char cond[256];
            size_t cl = (size_t)(brace - cs);
            if (cl >= sizeof cond)
                cl = sizeof cond - 1;
            memcpy(cond, cs, cl);
            cond[cl] = 0;
            const char *ib = brace + 1;
            const char *after = css_skip_block(brace);
            const char *close = after - 1; /* at matching '}' (or end) */
            if (mini_css_media_matches(cond, vw, vh))
            {
                size_t il = (size_t)(close - ib);
                if (olen + il + 2 > cap)
                {
                    cap = (olen + il + 2) * 2;
                    out = realloc(out, cap);
                }
                memcpy(out + olen, ib, il);
                olen += il;
                out[olen++] = '\n';
            }
            p = (*close == '}') ? close + 1 : close;
        }
        else if (!strcmp(kw, "keyframes"))
        {
            while (*p && isspace((unsigned char)*p))
                p++;
            const char *ns = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '-' || *p == '_'))
                p++;
            char name[64];
            size_t nl = (size_t)(p - ns);
            if (nl >= sizeof name)
                nl = sizeof name - 1;
            memcpy(name, ns, nl);
            name[nl] = 0;
            const char *brace = strchr(p, '{');
            if (!brace)
                break;
            const char *ib = brace + 1;
            const char *after = css_skip_block(brace);
            const char *close = after - 1;
            size_t il = (size_t)(close - ib);
            char *body = (char *)malloc(il + 1);
            memcpy(body, ib, il);
            body[il] = 0;
            mini_css_keyframes_set(name, body);
            free(body);
            p = (*close == '}') ? close + 1 : close;
        }
        else if (!strcmp(kw, "font-face"))
        {
            const char *brace = strchr(p, '{');
            if (!brace)
                break;
            const char *ib = brace + 1;
            const char *after = css_skip_block(brace);
            const char *close = after - 1;
            size_t il = (size_t)(close - ib);
            char *body = (char *)malloc(il + 1);
            memcpy(body, ib, il);
            body[il] = 0;
            mini_css_fontface_set(body);
            free(body);
            p = (*close == '}') ? close + 1 : close;
        }
        else if (!strcmp(kw, "supports"))
        {
            const char *cs = p;
            const char *brace = strchr(p, '{');
            if (!brace)
                break;
            char cond[256];
            size_t cl = (size_t)(brace - cs);
            if (cl >= sizeof cond)
                cl = sizeof cond - 1;
            memcpy(cond, cs, cl);
            cond[cl] = 0;
            const char *ib = brace + 1;
            const char *after = css_skip_block(brace);
            const char *close = after - 1;
            if (css_eval_supports(cond))
            {
                size_t il = (size_t)(close - ib);
                if (olen + il + 2 > cap)
                {
                    cap = (olen + il + 2) * 2;
                    out = (char *)realloc(out, cap);
                }
                memcpy(out + olen, ib, il);
                olen += il;
                out[olen++] = '\n';
            }
            p = (*close == '}') ? close + 1 : close;
        }
        else if (!strcmp(kw, "layer"))
        {
            const char *brace = strchr(p, '{');
            const char *semi = strchr(p, ';');
            if (semi && (!brace || semi < brace))
            {
                p = semi + 1;
            }
            else if (brace)
            {
                const char *ib = brace + 1;
                const char *after = css_skip_block(brace);
                const char *close = after - 1;
                size_t il = (size_t)(close - ib);
                if (olen + il + 2 > cap)
                {
                    cap = (olen + il + 2) * 2;
                    out = (char *)realloc(out, cap);
                }
                memcpy(out + olen, ib, il);
                olen += il;
                out[olen++] = '\n';
                p = (*close == '}') ? close + 1 : close;
            }
            else
                break;
        }
        else
        {
            /* @import / @charset / @page / @namespace ... */
            const char *brace = strchr(p, '{');
            const char *semi = strchr(p, ';');
            if (semi && (!brace || semi < brace))
                p = semi + 1;
            else if (brace)
                p = css_skip_block(brace);
            else
                break;
        }
    }
    out[olen] = 0;
    return out;
}

/* Helper to combine parent selector and child selector into output buffer */
static void css_combine_selectors(const char *parent, const char *child, char *out, size_t out_cap)
{
    out[0] = 0;
    if (!parent || !parent[0])
    {
        strncpy(out, child, out_cap - 1);
        out[out_cap - 1] = 0;
        return;
    }
    char pbuf[256], cbuf[256];
    strncpy(pbuf, parent, sizeof(pbuf) - 1);
    pbuf[sizeof(pbuf) - 1] = 0;
    strncpy(cbuf, child, sizeof(cbuf) - 1);
    cbuf[sizeof(cbuf) - 1] = 0;

    size_t cur_len = 0;
    char *p_save, *c_save;
    for (char *p_item = strtok_r(pbuf, ",", &p_save); p_item; p_item = strtok_r(NULL, ",", &p_save))
    {
        while (*p_item && isspace((unsigned char)*p_item))
            p_item++;
        char *pe = p_item + strlen(p_item) - 1;
        while (pe >= p_item && isspace((unsigned char)*pe))
            *pe-- = 0;

        char cbuf_copy[256];
        strncpy(cbuf_copy, child, sizeof(cbuf_copy) - 1);
        cbuf_copy[sizeof(cbuf_copy) - 1] = 0;
        for (char *c_item = strtok_r(cbuf_copy, ",", &c_save); c_item; c_item = strtok_r(NULL, ",", &c_save))
        {
            while (*c_item && isspace((unsigned char)*c_item))
                c_item++;
            char *ce = c_item + strlen(c_item) - 1;
            while (ce >= c_item && isspace((unsigned char)*ce))
                *ce-- = 0;

            if (cur_len > 0 && cur_len + 2 < out_cap)
            {
                out[cur_len++] = ',';
                out[cur_len++] = ' ';
                out[cur_len] = 0;
            }

            if (strchr(c_item, '&'))
            {
                const char *cp = c_item;
                while (*cp && cur_len + 1 < out_cap)
                {
                    if (*cp == '&')
                    {
                        size_t pl = strlen(p_item);
                        if (cur_len + pl < out_cap)
                        {
                            memcpy(out + cur_len, p_item, pl);
                            cur_len += pl;
                        }
                        cp++;
                    }
                    else
                    {
                        out[cur_len++] = *cp++;
                    }
                }
                out[cur_len] = 0;
            }
            else
            {
                int needs_space = (c_item[0] != '>' && c_item[0] != '+' && c_item[0] != '~');
                size_t pl = strlen(p_item), cl = strlen(c_item);
                if (cur_len + pl + (needs_space ? 1 : 0) + cl < out_cap)
                {
                    memcpy(out + cur_len, p_item, pl);
                    cur_len += pl;
                    if (needs_space)
                        out[cur_len++] = ' ';
                    memcpy(out + cur_len, c_item, cl);
                    cur_len += cl;
                    out[cur_len] = 0;
                }
            }
        }
    }
}

/* Recursively expand nested CSS rules */
static void css_expand_nesting_rec(const char *parent_sel, const char *css_body, char **out, size_t *olen, size_t *cap)
{
    const char *p = css_body;
    while (*p)
    {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;

        const char *brace = strchr(p, '{');
        if (!brace)
            break;

        const char *semi = strchr(p, ';');
        if (semi && semi < brace)
        {
            size_t decl_len = (size_t)(semi - p + 1);
            if (parent_sel && parent_sel[0])
            {
                size_t psl = strlen(parent_sel);
                if (*olen + psl + decl_len + 8 > *cap)
                {
                    *cap = (*olen + psl + decl_len + 8) * 2;
                    *out = (char *)realloc(*out, *cap);
                }
                memcpy(*out + *olen, parent_sel, psl);
                *olen += psl;
                (*out)[(*olen)++] = ' ';
                (*out)[(*olen)++] = '{';
                (*out)[(*olen)++] = ' ';
                memcpy(*out + *olen, p, decl_len);
                *olen += decl_len;
                (*out)[(*olen)++] = ' ';
                (*out)[(*olen)++] = '}';
                (*out)[(*olen)++] = '\n';
                (*out)[*olen] = 0;
            }
            p = semi + 1;
            continue;
        }

        char sel_tok[256] = {0};
        size_t sl = (size_t)(brace - p);
        if (sl >= sizeof(sel_tok))
            sl = sizeof(sel_tok) - 1;
        memcpy(sel_tok, p, sl);
        sel_tok[sl] = 0;

        char *st = sel_tok;
        while (*st && isspace((unsigned char)*st))
            st++;
        char *se = st + strlen(st) - 1;
        while (se >= st && isspace((unsigned char)*se))
            *se-- = 0;

        const char *ib = brace + 1;
        const char *after = css_skip_block(brace);
        const char *close = after - 1;
        size_t body_len = (size_t)(close - ib);

        char *body = (char *)malloc(body_len + 1);
        if (body)
        {
            memcpy(body, ib, body_len);
            body[body_len] = 0;

            char resolved_sel[512] = {0};
            css_combine_selectors(parent_sel, st, resolved_sel, sizeof(resolved_sel));

            char *inner_decls = (char *)malloc(body_len + 1);
            size_t id_len = 0;

            const char *bp = body;
            while (*bp)
            {
                while (*bp && isspace((unsigned char)*bp))
                    bp++;
                if (!*bp)
                    break;
                if (*bp == '{')
                {
                    bp = css_skip_block(bp);
                    continue;
                }
                const char *inner_brace = strchr(bp, '{');
                const char *inner_semi = strchr(bp, ';');
                if (inner_semi && (!inner_brace || inner_semi < inner_brace))
                {
                    size_t seg_l = (size_t)(inner_semi - bp + 1);
                    memcpy(inner_decls + id_len, bp, seg_l);
                    id_len += seg_l;
                    bp = inner_semi + 1;
                }
                else if (inner_brace)
                {
                    const char *rule_end = css_skip_block(inner_brace);
                    size_t r_len = (size_t)(rule_end - bp);
                    char *nested_rule_str = (char *)malloc(r_len + 1);
                    if (nested_rule_str)
                    {
                        memcpy(nested_rule_str, bp, r_len);
                        nested_rule_str[r_len] = 0;
                        css_expand_nesting_rec(resolved_sel, nested_rule_str, out, olen, cap);
                        free(nested_rule_str);
                    }
                    bp = rule_end;
                }
                else
                {
                    size_t rest_l = strlen(bp);
                    memcpy(inner_decls + id_len, bp, rest_l);
                    id_len += rest_l;
                    break;
                }
            }
            inner_decls[id_len] = 0;

            char *idt = inner_decls;
            while (*idt && isspace((unsigned char)*idt))
                idt++;
            if (*idt)
            {
                size_t rsl = strlen(resolved_sel);
                size_t idtl = strlen(idt);
                if (*olen + rsl + idtl + 8 > *cap)
                {
                    *cap = (*olen + rsl + idtl + 8) * 2;
                    *out = (char *)realloc(*out, *cap);
                }
                memcpy(*out + *olen, resolved_sel, rsl);
                *olen += rsl;
                (*out)[(*olen)++] = ' ';
                (*out)[(*olen)++] = '{';
                (*out)[(*olen)++] = ' ';
                memcpy(*out + *olen, idt, idtl);
                *olen += idtl;
                (*out)[(*olen)++] = ' ';
                (*out)[(*olen)++] = '}';
                (*out)[(*olen)++] = '\n';
                (*out)[*olen] = 0;
            }
            free(inner_decls);
            free(body);
        }

        p = (*close == '}') ? close + 1 : close;
    }
}

static char *css_expand_nesting(const char *in)
{
    if (!in)
        return strdup("");
    size_t cap = strlen(in) * 2 + 256, olen = 0;
    char *out = (char *)malloc(cap);
    if (!out)
        return strdup("");
    out[0] = 0;
    css_expand_nesting_rec("", in, &out, &olen, &cap);
    out[olen] = 0;
    return out;
}

/* ================================================================== */
/* Cascade winner tracker (replaces the O(ndecls² × matches)          */
/* "overshadow" inner loop in mini_css_apply)                          */
/* ================================================================== */
/* The old apply pass ran decls in ASCENDING priority order and, for
   every (decl, matched-node), scanned ALL later decls with the same prop
   to decide whether to skip ("overshadow") — turning the apply loop into
   O(ndecls² × matches × match_complex) per restyle. On a token-heavy page
   (1000+ decls) this was the dominant hover-latency cost.

   The replacement processes decls in DESCENDING priority order and tracks,
   per (host node, prop hash, pseudo-element kind), whether a higher-priority
   decl has already won that slot. First writer wins; subsequent decls of the
   same (node, prop, pseudo) are skipped with an O(1) hash lookup. The
   "Zeno" animation-clock reset the old loop guarded against stays fixed,
   because only the winning declaration ever reaches mini_style_set.        */

static void snapshot_base_styles(struct MiniNode *n);

void mini_css_apply(MiniDocument *doc, const char *css)
{
    if (!doc || !css)
        return;

    MiniDocument *prev_active = g_active_doc;
    g_active_doc = doc;

    if (!g_restyling)
    {
        size_t L = strlen(css);
        char *np = (char *)realloc(g_restyle_css, g_restyle_len + L + 1);
        if (np)
        {
            memcpy(np + g_restyle_len, css, L);
            g_restyle_len += L;
            np[g_restyle_len] = 0;
            g_restyle_css = np;
        }
    }

    char *clean_css = strip_css_comments(css);
    if (!clean_css)
    {
        g_active_doc = prev_active;
        return;
    }

    int cvw = g_lctx.vw > 0.0f ? (int)g_lctx.vw : 1280;
    int cvh = g_lctx.vh > 0.0f ? (int)g_lctx.vh : 720;
    char *processed = css_expand_at_rules(clean_css, cvw, cvh);
    char *nested_expanded = css_expand_nesting(processed);
    free(processed);
    processed = nested_expanded;

    typedef struct
    {
        char sel[128];
        uint32_t spec;
    } CssRule;
    typedef struct
    {
        int rule;
        int important;
        char prop[64];
        char val[256];
    } CssDecl;
    enum
    {
        MAX_RULES = 1024,
        MAX_DECLS = 4096
    };
    CssRule *rules = (CssRule *)malloc(sizeof(CssRule) * MAX_RULES);
    CssDecl *decls = (CssDecl *)malloc(sizeof(CssDecl) * MAX_DECLS);
    if (!rules || !decls)
    {
        free(rules);
        free(decls);
        free(clean_css);
        g_active_doc = prev_active;
        return;
    }
    int nrules = 0, ndecls = 0;

    const char *p = processed;
    while (*p)
    {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        /* At-rules (@media / @keyframes / @font-face / @supports / @page /
           @import): skip safely so a nested '{ ... { ... } ... }' block does
           not corrupt the flat cascade. We do NOT evaluate media queries or
           store keyframes here (animation wiring is Stage 4); the priority is
           that the rules AFTER the at-rule still parse. A blockless at-rule
           (@import "x";) is skipped to its ';'; a braced at-rule is skipped
           to its matching '}' at depth 0.                                       */
        if (*p == '@')
        {
            const char *brace = NULL, *semi = NULL;
            for (const char *q = p; *q; q++)
            {
                if (*q == '{')
                {
                    brace = q;
                    break;
                }
                if (*q == ';')
                {
                    semi = q;
                    break;
                }
                if (*q == '"' || *q == '\'')
                {
                    char quote = *q;
                    q++;
                    while (*q && *q != quote)
                        q++;
                    /* q now at the closing quote (or null) */
                }
            }
            if (semi && (!brace || semi < brace))
            {
                p = semi + 1; /* blockless @-rule (e.g. @import) */
                continue;
            }
            if (!brace)
            {
                p += strlen(p); /* unterminated @-rule */
                break;
            }
            /* balanced skip to the matching '}' at depth 0 */
            int depth = 0;
            const char *q = brace;
            for (; *q; q++)
            {
                if (*q == '{')
                    depth++;
                else if (*q == '}')
                {
                    depth--;
                    if (depth == 0)
                    {
                        q++;
                        break;
                    }
                }
            }
            p = q; /* past the closing '}' (or end of string) */
            continue;
        }
        const char *brace = strchr(p, '{');
        if (!brace)
            break;
        const char *ss = p;
        size_t sl = (size_t)(brace - p);
        while (sl > 0 && isspace((unsigned char)ss[0]))
        {
            ss++;
            sl--;
        }
        while (sl > 0 && isspace((unsigned char)ss[sl - 1]))
            sl--;
        int ri = nrules;
        if (sl > 0 && nrules < MAX_RULES)
        {
            CssRule *R = &rules[nrules++];
            if (sl >= sizeof R->sel)
                sl = sizeof R->sel - 1;
            memcpy(R->sel, ss, sl);
            R->sel[sl] = 0;
            R->spec = spec_of(R->sel);
        }
        p = brace + 1;
        const char *close = strchr(p, '}');
        if (!close)
            break;
        const char *d = p;
        while (d < close)
        {
            const char *semi = memchr(d, ';', (size_t)(close - d));
            const char *end = semi ? semi : close;
            const char *colon = memchr(d, ':', (size_t)(end - d));
            if (colon && ri < nrules)
            {
                char prop[64];
                const char *ps = d;
                size_t pn = (size_t)(colon - d);
                while (pn > 0 && isspace((unsigned char)ps[0]))
                {
                    ps++;
                    pn--;
                }
                while (pn > 0 && isspace((unsigned char)ps[pn - 1]))
                    pn--;
                if (pn >= sizeof prop)
                    pn = sizeof prop - 1;
                memcpy(prop, ps, pn);
                prop[pn] = 0;
                char val[256];
                const char *vs = colon + 1;
                size_t vn = (size_t)(end - vs);
                while (vn > 0 && isspace((unsigned char)vs[0]))
                {
                    vs++;
                    vn--;
                }
                while (vn > 0 && isspace((unsigned char)vs[vn - 1]))
                    vn--;
                if (vn >= sizeof val)
                    vn = sizeof val - 1;
                memcpy(val, vs, vn);
                val[vn] = 0;
                if (prop[0] && ndecls < MAX_DECLS)
                {
                    int imp = strip_important(val);
                    CssDecl *D = &decls[ndecls++];
                    D->rule = ri;
                    D->important = imp;
                    snprintf(D->prop, sizeof D->prop, "%s", prop);
                    snprintf(D->val, sizeof D->val, "%s", val);
                }
            }
            d = end + 1;
        }
        p = close + 1;
    }

    /* 升序排序 (important, specificity, source order) */
    for (int i = 1; i < ndecls; i++)
    {
        CssDecl key = decls[i];
        int j = i - 1;
        while (j >= 0)
        {
            CssDecl *dj = &decls[j];
            uint32_t sj = rules[dj->rule].spec;
            uint32_t sk = rules[key.rule].spec;
            int kj = dj->important, kk = key.important;
            int shift = (kj > kk) || (kj == kk && sj > sk);
            if (shift)
            {
                decls[j + 1] = decls[j];
                j--;
            }
            else
                break;
        }
        decls[j + 1] = key;
    }

    /* Clear the cascade-winner table for this pass. Cheap memset (~0.15 ms
       for 32K × 16-byte slots) — dwarfed by the cubic loop it replaces. */
    cw_clear();

    /* Pass 0: Register all CSS custom properties (--xxx) first so that
       all design tokens and CSS variables are available during cascade evaluation. */
    for (int i = 0; i < ndecls; i++)
    {
        CssDecl *D = &decls[i];
        if (D->prop[0] == '-' && D->prop[1] == '-')
        {
            const char *sel = rules[D->rule].sel;
            struct MiniNode *out[4096];
            int c = mini_dom_query_selector_all(doc, sel, out, 4096);
            for (int k = 0; k < c; k++)
            {
                mini_node_set_var(out[k], D->prop + 2, D->val);
            }
        }
    }

    /* Process declarations in DESCENDING priority order (highest spec /
       !important / source-order first). For each (host node, prop, pseudo)
       the FIRST decl to reach it wins; later (lower-priority) decls of the
       same triple are skipped via an O(1) hash lookup. This replaces the
       old ascending + O(ndecls²) "overshadow" re-match loop while keeping
       its Zeno-fix semantics intact (only the winning value reaches
       mini_style_set, so no double-apply resets the animation clock). */
    for (int i = ndecls - 1; i >= 0; i--)
    {
        CssDecl *D = &decls[i];
        if (D->prop[0] == '-' && D->prop[1] == '-')
            continue; /* already handled in Pass 0 */
        const char *sel = rules[D->rule].sel;
        /* ::before / ::after: strip the pseudo-element for matching (the host
           matches), and route the `content` declaration to the host's
           generated-content field. The rule's other declarations still apply
           to the host via mini_style_set — a best-effort approximation (a
           real pseudo generates an anonymous inline box; we draw the content
           text at the host's content origin instead — see render_node).       */
        int pseudo = 0; /* 0 none, 1 ::before, 2 ::after, 3 ::placeholder */
        char match_sel[128];
        const char *b = strstr(sel, "::before");
        if (!b)
            b = strstr(sel, ":before");
        const char *a = strstr(sel, "::after");
        if (!a)
            a = strstr(sel, ":after");
        const char *ph_sel = strstr(sel, "::placeholder");
        if (!ph_sel)
            ph_sel = strstr(sel, ":placeholder");
        if (!ph_sel)
            ph_sel = strstr(sel, "::-webkit-input-placeholder");
        if (!ph_sel)
            ph_sel = strstr(sel, "::-moz-placeholder");
        if (!ph_sel)
            ph_sel = strstr(sel, ":-ms-input-placeholder");
        if (b)
        {
            size_t L = (size_t)(b - sel);
            if (L >= sizeof match_sel)
                L = sizeof match_sel - 1;
            memcpy(match_sel, sel, L);
            match_sel[L] = 0;
            pseudo = 1;
        }
        else if (a)
        {
            size_t L = (size_t)(a - sel);
            if (L >= sizeof match_sel)
                L = sizeof match_sel - 1;
            memcpy(match_sel, sel, L);
            match_sel[L] = 0;
            pseudo = 2;
        }
        else if (ph_sel)
        {
            size_t L = (size_t)(ph_sel - sel);
            if (L >= sizeof match_sel)
                L = sizeof match_sel - 1;
            memcpy(match_sel, sel, L);
            match_sel[L] = 0;
            if (L == 0 || (L == 1 && match_sel[0] == '*'))
                strcpy(match_sel, "input,textarea");
            pseudo = 3;
        }
        else
        {
            snprintf(match_sel, sizeof match_sel, "%s", sel);
        }
        uint32_t ph = cw_prop_hash(D->prop);
        struct MiniNode *out[4096];
        int c = mini_dom_query_selector_all(doc, match_sel, out, 4096);
        for (int k = 0; k < c; k++)
        {
            /* Cascade winner: we process decls in DESCENDING priority order,
               so the first decl to reach this (host, prop, pseudo) is the
               winner. If a higher-priority decl already claimed this slot
               (processed earlier in this descending pass), skip — never let
               a weaker value overwrite the winner and reset the animation
               clock (the "Zeno" 1→0→1 flicker the old loop guarded). This
               is an O(1) hash lookup, replacing the old O(ndecls²) re-match
               inner loop. */
            if (cw_seen(out[k], ph, pseudo))
                continue;
            cw_mark(out[k], ph, pseudo);

            if (pseudo)
            {
                /* 关键修复：防止 *::before 匹配到伪元素自身，截断指数级 DOM 分形爆炸死循环 */
                if (out[k]->tag && !strncmp(out[k]->tag, "::", 2))
                    continue;

                struct MiniNode *ps_node = (pseudo == 1) ? out[k]->pseudo_before : (pseudo == 2 ? out[k]->pseudo_after : out[k]->pseudo_placeholder);
                if (!ps_node)
                {
                    ps_node = mini_node_create_element((pseudo == 1) ? "::before" : (pseudo == 2 ? "::after" : "::placeholder"));
                    ps_node->style.display = MINI_DISPLAY_INLINE; /* 伪元素默认流式显示模式为 inline */
                    if (pseudo == 1)
                    {
                        out[k]->pseudo_before = ps_node;
                        mini_node_insert_before(out[k], ps_node, out[k]->first_child);
                    }
                    else if (pseudo == 2)
                    {
                        out[k]->pseudo_after = ps_node;
                        mini_node_append_child(out[k], ps_node);
                    }
                    else if (pseudo == 3)
                    {
                        out[k]->pseudo_placeholder = ps_node;
                    }
                }
                if (!strcmp(D->prop, "content"))
                {
                    char tmp[128];
                    size_t vl = strlen(D->val);
                    if (vl >= sizeof tmp)
                        vl = sizeof tmp - 1;
                    memcpy(tmp, D->val, vl);
                    tmp[vl] = 0;
                    char *s = tmp;
                    while (*s && isspace((unsigned char)*s))
                        s++;
                    char *e = s + strlen(s);
                    while (e > s && isspace((unsigned char)e[-1]))
                        *--e = 0;
                    char *unq = s;
                    if ((*s == '"' || *s == '\'') && e > s && e[-1] == *s)
                    {
                        unq = s + 1;
                        e[-1] = 0;
                    }
                    else if (!strncmp(s, "attr(", 5))
                    {
                        char attr_name[64] = {0};
                        const char *ap = s + 5;
                        while (*ap && isspace((unsigned char)*ap))
                            ap++;
                        const char *ae = strchr(ap, ')');
                        if (ae)
                        {
                            size_t an_len = ae - ap;
                            if (an_len >= sizeof(attr_name))
                                an_len = sizeof(attr_name) - 1;
                            strncpy(attr_name, ap, an_len);
                            attr_name[an_len] = 0;
                            char *end_trim = attr_name + strlen(attr_name) - 1;
                            while (end_trim >= attr_name && isspace((unsigned char)*end_trim))
                                *end_trim-- = 0;
                            const char *attr_val = mini_node_get_attribute(out[k], attr_name);
                            if (attr_val)
                                unq = (char *)attr_val;
                        }
                    }
                    if (!strcmp(unq, "none"))
                    {
                        mini_node_remove_attribute(ps_node, "content");
                        free(ps_node->text);
                        ps_node->text = NULL;
                        if (pseudo == 1)
                        {
                            free(out[k]->before_content);
                            out[k]->before_content = NULL;
                        }
                        else
                        {
                            free(out[k]->after_content);
                            out[k]->after_content = NULL;
                        }
                    }
                    else
                    {
                        mini_node_set_attribute(ps_node, "content", unq);
                        char *dup = mini_dup(unq);
                        free(ps_node->text);
                        ps_node->text = dup;
                        struct MiniNode *fc = ps_node->first_child;
                        while (fc)
                        {
                            struct MiniNode *nx = fc->next_sibling;
                            mini_node_destroy(fc);
                            fc = nx;
                        }
                        ps_node->first_child = NULL;
                        ps_node->last_child = NULL;
                        if (*unq)
                        {
                            struct MiniNode *tchild = mini_node_create_text(unq);
                            mini_node_append_child(ps_node, tchild);
                        }
                        if (pseudo == 1)
                        {
                            free(out[k]->before_content);
                            out[k]->before_content = mini_dup(unq);
                        }
                        else
                        {
                            free(out[k]->after_content);
                            out[k]->after_content = mini_dup(unq);
                        }
                    }
                }
                else
                {
                    mini_style_set(ps_node, D->prop, D->val);
                }
                out[k]->dirty_layout = 1;
                out[k]->dirty_paint = 1;
            }
            else
            {
                mini_style_set(out[k], D->prop, D->val);
            }
        }
    }

    if (!g_restyling && doc && doc->root)
    {
        snapshot_base_styles(doc->root);
    }

    free(rules);
    free(decls);
    free(processed);
    free(clean_css);
    g_active_doc = prev_active;
}
const char *mini_doc_get_title(const struct MiniDocument *doc)
{
    if (!doc || !doc->root)
        return NULL;
    struct MiniNode *t = mini_dom_query_selector((MiniDocument *)doc, "title");
    if (t)
    {
        if (t->text && t->text[0])
            return t->text;
        if (t->first_child && t->first_child->text && t->first_child->text[0])
            return t->first_child->text;
    }
    return NULL;
}

void mini_dom_serialize_cdp(MiniDocument *doc, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    mini_dom_assign_node_ids(doc);      /* stable ids for nodeId addressing */
    CdpSer s = {out, cap, 0, 0, 1, -1}; /* use_stable=1, max_depth=-1 */
    ser_node(&s, doc->root, 0);
    out[s.len] = 0;
}

/* ================================================================== */
/* Serialize the DOM tree as indented HTML (innerHTML / debugging).    */
/* Walks the document root; emits doctype, tags + attributes, escapes */
/* text/comment content. Void elements self-close.                    */
/* ================================================================== */
static void html_escape_putc(char **p, char c, size_t *len, size_t cap)
{
    if (*len + 1 >= cap)
        return;
    (*p)[(*len)++] = c;
}
static void html_escape(char **p, const char *v, size_t *len, size_t cap)
{
    for (; *v; v++)
    {
        if (*v == '<')
        {
            if (*len + 4 < cap)
            {
                memcpy(*p + *len, "&lt;", 4);
                *len += 4;
            }
        }
        else if (*v == '>')
        {
            if (*len + 4 < cap)
            {
                memcpy(*p + *len, "&gt;", 4);
                *len += 4;
            }
        }
        else if (*v == '&')
        {
            if (*len + 5 < cap)
            {
                memcpy(*p + *len, "&amp;", 5);
                *len += 5;
            }
        }
        else if (*v == '"')
        {
            if (*len + 6 < cap)
            {
                memcpy(*p + *len, "&quot;", 6);
                *len += 6;
            }
        }
        else
            html_escape_putc(p, *v, len, cap);
    }
}
static void serialize_html_node(struct MiniNode *n, int depth,
                                char **p, size_t *len, size_t cap)
{
    if (!n)
        return;
    for (int i = 0; i < depth && *len + 1 < cap; i++)
        (*p)[(*len)++] = ' ';
    if (n->type == MN_TEXT_NODE)
    {
        if (n->text)
            html_escape(p, n->text, len, cap);
        if (*len + 1 < cap)
            (*p)[(*len)++] = '\n';
        return;
    }
    if (n->type == MN_COMMENT_NODE)
    {
        if (*len + 4 < cap)
        {
            memcpy(*p + *len, "<!--", 4);
            *len += 4;
        }
        if (n->text)
            html_escape(p, n->text, len, cap);
        if (*len + 3 < cap)
        {
            memcpy(*p + *len, "-->", 3);
            *len += 3;
        }
        if (*len + 1 < cap)
            (*p)[(*len)++] = '\n';
        return;
    }
    if (n->type == MN_DOCUMENT_TYPE_NODE)
    {
        if (*len + 10 < cap)
        {
            memcpy(*p + *len, "<!DOCTYPE ", 10);
            *len += 10;
        }
        if (n->tag)
            html_escape(p, n->tag, len, cap);
        if (*len + 2 < cap)
        {
            memcpy(*p + *len, ">", 1);
            *len += 1;
        }
        if (*len + 1 < cap)
            (*p)[(*len)++] = '\n';
        return;
    }
    if (n->type == MN_DOCUMENT_NODE || n->type == MN_DOCUMENT_FRAGMENT_NODE)
    {
        for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
            serialize_html_node(c, depth, p, len, cap);
        return;
    }
    /* element */
    if (*len + 1 < cap)
        (*p)[(*len)++] = '<';
    if (n->tag)
    {
        for (const char *t = n->tag; *t && *len + 1 < cap;)
            (*p)[(*len)++] = *t++;
    }
    for (MiniAttr *a = n->attrs; a; a = a->next)
    {
        if (*len + 1 < cap)
            (*p)[(*len)++] = ' ';
        if (a->name)
        {
            for (const char *t = a->name; *t && *len + 1 < cap;)
                (*p)[(*len)++] = *t++;
        }
        if (*len + 2 < cap)
        {
            (*p)[(*len)++] = '=';
            (*p)[(*len)++] = '"';
        }
        if (a->value)
            html_escape(p, a->value, len, cap);
        if (*len + 1 < cap)
            (*p)[(*len)++] = '"';
    }
    int is_void = mini_element_info(n->tag)->is_void;
    if (is_void)
    {
        if (*len + 3 < cap)
        {
            memcpy(*p + *len, " />", 3);
            *len += 3;
        }
        if (*len + 1 < cap)
            (*p)[(*len)++] = '\n';
        return;
    }
    if (*len + 1 < cap)
        (*p)[(*len)++] = '>';
    if (n->tag && tag_is_rawtext(n->tag) && n->text)
    {
        /* raw-text element (<script>/<style>): emit the verbatim content
           captured by the parser, no child recursion / no indent. */
        for (const char *t = n->text; *t && *len + 1 < cap;)
            (*p)[(*len)++] = *t++;
    }
    else
    {
        if (*len + 1 < cap)
            (*p)[(*len)++] = '\n';
        for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
            serialize_html_node(c, depth + 1, p, len, cap);
        for (int i = 0; i < depth && *len + 1 < cap; i++)
            (*p)[(*len)++] = ' ';
    }
    if (*len + 2 < cap)
    {
        (*p)[(*len)++] = '<';
        (*p)[(*len)++] = '/';
    }
    if (n->tag)
    {
        for (const char *t = n->tag; *t && *len + 1 < cap;)
            (*p)[(*len)++] = *t++;
    }
    if (*len + 1 < cap)
        (*p)[(*len)++] = '>';
    if (*len + 1 < cap)
        (*p)[(*len)++] = '\n';
}
void mini_dom_serialize_html(MiniDocument *doc, char *out, size_t cap)
{
    if (!doc || !out || cap == 0)
    {
        if (out && cap)
            out[0] = 0;
        return;
    }
    size_t len = 0;
    serialize_html_node(doc->root, 0, &out, &len, cap);
    out[len < cap ? len : cap - 1] = 0;
}

/* ================================================================== */
/* CDP per-node access — stable node ids + per-node serializers.       */
/* The Elements panel addresses nodes by id across requests, so ids    */
/* must be stable: existing nodes keep their id; newly-inserted nodes  */
/* get the next free id (>= current max).                              */
/* ================================================================== */
static int cdp_max_id_rec(struct MiniNode *n, int mx)
{
    if (!n)
        return mx;
    if (n->cdp_node_id > mx)
        mx = n->cdp_node_id;
    mx = cdp_max_id_rec(n->first_child, mx);
    mx = cdp_max_id_rec(n->next_sibling, mx);
    if (n->shadow_root)
        mx = cdp_max_id_rec(n->shadow_root, mx);
    return mx;
}
static void assign_ids_rec(struct MiniNode *n, int *next)
{
    if (!n)
        return;
    if (n->cdp_node_id == 0)
        n->cdp_node_id = (*next)++;
    assign_ids_rec(n->first_child, next);
    assign_ids_rec(n->next_sibling, next);
    if (n->shadow_root)
        assign_ids_rec(n->shadow_root, next);
}
int mini_dom_assign_node_ids(MiniDocument *doc)
{
    if (!doc || !doc->root)
        return 0;
    int next = cdp_max_id_rec(doc->root, 0) + 1;
    assign_ids_rec(doc->root, &next);
    return next - 1;
}
static struct MiniNode *find_id_rec(struct MiniNode *n, int id)
{
    if (!n)
        return NULL;
    if (n->cdp_node_id == id)
        return n;
    struct MiniNode *r = find_id_rec(n->first_child, id);
    if (r)
        return r;
    r = find_id_rec(n->next_sibling, id);
    if (r)
        return r;
    if (n->shadow_root)
        return find_id_rec(n->shadow_root, id);
    return NULL;
}
struct MiniNode *mini_dom_node_by_id(MiniDocument *doc, int id)
{
    if (!doc || !doc->root)
        return NULL;
    return find_id_rec(doc->root, id);
}
int mini_dom_node_id(const struct MiniNode *n)
{
    return n ? n->cdp_node_id : 0;
}

/* CDP Node JSON for one node + its immediate children (DOM.describeNode). */
void mini_dom_describe_node(const struct MiniNode *n, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = 0;
    if (!n)
        return;
    CdpSer s = {out, cap, 0, 0, 1, 1}; /* stable ids, depth 1 */
    int pid = n->parent ? n->parent->cdp_node_id : 0;
    ser_node(&s, (struct MiniNode *)n, pid);
    out[s.len < cap ? s.len : cap - 1] = 0;
}

/* CDP child array (DOM.requestChildNodes — we already send the full tree
   on getDocument; this is for completeness). */
void mini_dom_node_children_cdp(const struct MiniNode *n, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = 0;
    if (!n)
    {
        if (cap > 2)
        {
            out[0] = '[';
            out[1] = ']';
            out[2] = 0;
        }
        return;
    }
    CdpSer s = {out, cap, 0, 0, 1, 0}; /* stable ids, depth 0 (node-only) */
    int pid = n->cdp_node_id;
    ser_puts(&s, "[");
    int first = 1;
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
    {
        if (!first)
            ser_putc(&s, ',');
        first = 0;
        ser_node(&s, c, pid);
    }
    if (n->shadow_root)
    {
        if (!first)
            ser_putc(&s, ',');
        ser_node(&s, (struct MiniNode *)n->shadow_root, pid);
    }
    ser_puts(&s, "]");
    out[s.len < cap ? s.len : cap - 1] = 0;
}

/* CDP BoxModel (DOM.getBoxModel / CSS.getLayoutMetrics) from the node's
   computed layout geometry. Quads are 4 points (x1,y1..x4,y4) clockwise
   from top-left. margin/padding/border_w are [top,right,bottom,left].   */
void mini_dom_box_model(const struct MiniNode *n, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = 0;
    if (!n)
        return;
    const MiniStyle *st = &n->style;
    float x = st->abs_x, y = st->abs_y, w = st->w, h = st->h;
    float bt = st->border_w[0], br = st->border_w[1],
          bb = st->border_w[2], bl = st->border_w[3];
    float pt = st->padding[0], pr = st->padding[1],
          pb = st->padding[2], pl = st->padding[3];
    float mt = st->margin[0], mr = st->margin[1],
          mb = st->margin[2], ml = st->margin[3];
    float cx = x + bl + pl, cy = y + bt + pt,
          cw = w - bl - br - pl - pr, ch = h - bt - bb - pt - pb;
    float px = x + bl, py = y + bt, pw = w - bl - br, ph = h - bt - bb;
    float mx = x - ml, my = y - mt, mw = w + ml + mr, mh = h + mt + mb;
    snprintf(out, cap,
             "{\"content\":[%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f],"
             "\"padding\":[%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f],"
             "\"border\":[%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f],"
             "\"margin\":[%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f],"
             "\"width\":%.2f,\"height\":%.2f}",
             cx, cy, cx + cw, cy, cx + cw, cy + ch, cx, cy + ch,
             px, py, px + pw, py, px + pw, py + ph, px, py + ph,
             x, y, x + w, y, x + w, y + h, x, y + h,
             mx, my, mx + mw, my, mx + mw, my + mh, mx, my + mh,
             (double)cw, (double)ch);
}

/* Computed CSS pairs (CSS.getComputedStyle) from MiniStyle. */
void mini_dom_computed_style(const struct MiniNode *n, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = 0;
    if (!n)
        return;
    const MiniStyle *s = &n->style;
    const char *disp = s->display == 0 ? "block" : s->display == 1 ? "flex"
                                               : s->display == 2   ? "none"
                                               : s->display == 3   ? "inline"
                                                                   : "table";
    snprintf(out, cap,
             "{\"computedStyle\":["
             "{\"name\":\"display\",\"value\":\"%s\"},"
             "{\"name\":\"width\",\"value\":\"%.2fpx\"},"
             "{\"name\":\"height\",\"value\":\"%.2fpx\"},"
             "{\"name\":\"x\",\"value\":\"%.2fpx\"},"
             "{\"name\":\"y\",\"value\":\"%.2fpx\"},"
             "{\"name\":\"margin-top\",\"value\":\"%.2fpx\"},"
             "{\"name\":\"margin-right\",\"value\":\"%.2fpx\"},"
             "{\"name\":\"margin-bottom\",\"value\":\"%.2fpx\"},"
             "{\"name\":\"margin-left\",\"value\":\"%.2fpx\"},"
             "{\"name\":\"padding-top\",\"value\":\"%.2fpx\"},"
             "{\"name\":\"padding-right\",\"value\":\"%.2fpx\"},"
             "{\"name\":\"padding-bottom\",\"value\":\"%.2fpx\"},"
             "{\"name\":\"padding-left\",\"value\":\"%.2fpx\"},"
             "{\"name\":\"background-color\",\"value\":\"rgba(%.0f,%.0f,%.0f,%.2f)\"},"
             "{\"name\":\"color\",\"value\":\"rgba(%.0f,%.0f,%.0f,%.2f)\"},"
             "{\"name\":\"font-size\",\"value\":\"%.2fpx\"}"
             "]}",
             disp,
             (double)s->w, (double)s->h, (double)s->abs_x, (double)s->abs_y,
             (double)s->margin[0], (double)s->margin[1],
             (double)s->margin[2], (double)s->margin[3],
             (double)s->padding[0], (double)s->padding[1],
             (double)s->padding[2], (double)s->padding[3],
             (double)(s->bg_r * 255), (double)(s->bg_g * 255),
             (double)(s->bg_b * 255), (double)s->bg_a,
             (double)(s->color_r * 255), (double)(s->color_g * 255),
             (double)(s->color_b * 255), (double)s->color_a,
             (double)s->font_size);
}

/* Inline style attribute text (CSS.getInlineStyles). */
void mini_dom_inline_style(const struct MiniNode *n, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    const char *s = n ? mini_node_get_attribute(n, "style") : NULL;
    snprintf(out, cap, "%s", s ? s : "");
}

static void serialize_html_node_raw(const struct MiniNode *n, char **p, size_t *len, size_t cap)
{
    if (!n)
        return;
    if (n->type == MN_TEXT_NODE)
    {
        if (n->text)
            html_escape(p, n->text, len, cap);
        return;
    }
    if (n->type == MN_COMMENT_NODE)
    {
        if (*len + 4 < cap)
        {
            memcpy(*p + *len, "<!--", 4);
            *len += 4;
        }
        if (n->text)
            html_escape(p, n->text, len, cap);
        if (*len + 3 < cap)
        {
            memcpy(*p + *len, "-->", 3);
            *len += 3;
        }
        return;
    }
    if (n->type == MN_DOCUMENT_TYPE_NODE)
    {
        if (*len + 10 < cap)
        {
            memcpy(*p + *len, "<!DOCTYPE ", 10);
            *len += 10;
        }
        if (n->tag)
            html_escape(p, n->tag, len, cap);
        if (*len + 1 < cap)
        {
            (*p)[(*len)++] = '>';
        }
        return;
    }
    if (n->type == MN_DOCUMENT_NODE || n->type == MN_DOCUMENT_FRAGMENT_NODE)
    {
        for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
            serialize_html_node_raw(c, p, len, cap);
        return;
    }
    /* element */
    if (*len + 1 < cap)
        (*p)[(*len)++] = '<';
    if (n->tag)
    {
        for (const char *t = n->tag; *t && *len + 1 < cap;)
            (*p)[(*len)++] = *t++;
    }
    for (MiniAttr *a = n->attrs; a; a = a->next)
    {
        if (*len + 1 < cap)
            (*p)[(*len)++] = ' ';
        if (a->name)
        {
            for (const char *t = a->name; *t && *len + 1 < cap;)
                (*p)[(*len)++] = *t++;
        }
        if (*len + 2 < cap)
        {
            (*p)[(*len)++] = '=';
            (*p)[(*len)++] = '"';
        }
        if (a->value)
            html_escape(p, a->value, len, cap);
        if (*len + 1 < cap)
            (*p)[(*len)++] = '"';
    }
    int is_void = mini_element_info(n->tag)->is_void;
    if (is_void)
    {
        if (*len + 3 < cap)
        {
            memcpy(*p + *len, " />", 3);
            *len += 3;
        }
        return;
    }
    if (*len + 1 < cap)
        (*p)[(*len)++] = '>';
    if (n->tag && tag_is_rawtext(n->tag) && n->text)
    {
        for (const char *t = n->text; *t && *len + 1 < cap;)
            (*p)[(*len)++] = *t++;
    }
    else
    {
        for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
            serialize_html_node_raw(c, p, len, cap);
    }
    if (*len + 2 < cap)
    {
        (*p)[(*len)++] = '<';
        (*p)[(*len)++] = '/';
    }
    if (n->tag)
    {
        for (const char *t = n->tag; *t && *len + 1 < cap;)
            (*p)[(*len)++] = *t++;
    }
    if (*len + 1 < cap)
        (*p)[(*len)++] = '>';
}

/* outerHTML / innerHTML (DOM.getOuterHTML / innerHTML). */
void mini_dom_outer_html(const struct MiniNode *n, int inner,
                         char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = 0;
    if (!n)
        return;
    size_t len = 0;
    if (inner)
    {
        for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
            serialize_html_node_raw(c, &out, &len, cap);
    }
    else
    {
        serialize_html_node_raw(n, &out, &len, cap);
    }
    out[len < cap ? len : cap - 1] = 0;
}

/* ================================================================== */
/* Phase 4: dynamic DOM (innerHTML) + frame tick + interaction state.  */
/* ================================================================== */

/* Replace a node's children by parsing `html` into a fresh subtree. The
   old children are destroyed; the new ones are reparented in order. Marks
   the node dirty so the next tick_frame relays out the change. The parser
   appends to doc->body, so we parse into a transient document and move
   body's children into n (a fragment-style reparent).                    */
void mini_node_set_inner_html(struct MiniNode *n, const char *html)
{
    if (!n)
        return;
    /* tear down existing children */
    struct MiniNode *c = n->first_child;
    while (c)
    {
        struct MiniNode *nx = c->next_sibling;
        mini_node_destroy(c);
        c = nx;
    }
    n->first_child = n->last_child = NULL;
    if (html && *html)
    {
        if (strchr(html, '<') == NULL)
        {
            mini_node_append_child(n, mini_node_create_text(html));
        }
        else
        {
            /* 关键修复：保存当前活动文档指针，避免被临时文档销毁时置 NULL */
            MiniDocument *saved_doc = g_active_doc;
            MiniDocument *tmp = mini_doc_create();
            if (tmp)
            {
                mini_dom_parse_html(tmp, html);
                /* reparent body's children into n */
                struct MiniNode *k = tmp->body->first_child;
                while (k)
                {
                    struct MiniNode *nx = k->next_sibling;
                    mini_node_append_child(n, k);
                    k = nx;
                }
                /* detach so mini_doc_destroy frees only root/body shells */
                tmp->body->first_child = tmp->body->last_child = NULL;
                mini_doc_destroy(tmp);
            }
            g_active_doc = saved_doc; /* 恢复主文档指针 */
        }
    }
    n->dirty_layout = 1;
    n->dirty_paint = 1;
}

/* set/clear the interaction bits (-1 = leave unchanged); marks the node
   dirty so the next restyle picks up :hover / :active / :focus.         */
void mini_node_set_interaction_state(struct MiniNode *n, int hovered,
                                     int active, int focused)
{
    if (!n)
        return;
    int changed = 0;
    if (hovered >= 0 && n->state_hovered != (uint8_t)hovered)
    {
        n->state_hovered = (uint8_t)hovered;
        changed = 1;
    }
    if (active >= 0 && n->state_active != (uint8_t)active)
    {
        n->state_active = (uint8_t)active;
        changed = 1;
    }
    if (focused >= 0 && n->state_focused != (uint8_t)focused)
    {
        n->state_focused = (uint8_t)focused;
        changed = 1;
    }
    if (changed)
        n->dirty_paint = 1;
}

void mini_node_mark_dirty(struct MiniNode *n, int layout, int paint)
{
    if (!n)
        return;
    if (layout)
        n->dirty_layout = 1;
    if (paint)
        n->dirty_paint = 1;
}

/* is `anc` an ancestor of `x` (or equal)? */
static int mini_is_anc(const struct MiniNode *anc, const struct MiniNode *x)
{
    for (const struct MiniNode *p = x; p; p = p->parent)
        if (p == anc)
            return 1;
    return 0;
}

/* Document (pre-order) comparison. 1 iff a precedes b. */
int mini_node_precedes(const struct MiniNode *a, const struct MiniNode *b)
{
    if (a == b)
        return 0;
    if (!a)
        return 1; /* NULL sorts first */
    if (!b)
        return 0;
    if (mini_is_anc(a, b))
        return 1; /* a is an ancestor -> a precedes b */
    if (mini_is_anc(b, a))
        return 0; /* b is an ancestor -> b precedes a */
    /* find lowest common ancestor: walk a up to the first ancestor of b */
    const struct MiniNode *lca = NULL;
    for (const struct MiniNode *x = a->parent; x; x = x->parent)
    {
        if (mini_is_anc(x, b))
        {
            lca = x;
            break;
        }
    }
    if (!lca)
        return (uintptr_t)a < (uintptr_t)b; /* disjoint trees */
    /* branch children of lca on each path */
    const struct MiniNode *ba = a;
    while (ba->parent && ba->parent != lca)
        ba = ba->parent;
    const struct MiniNode *bb = b;
    while (bb->parent && bb->parent != lca)
        bb = bb->parent;
    for (const struct MiniNode *c = lca->first_child; c; c = c->next_sibling)
    {
        if (c == ba)
            return 1; /* a's branch first */
        if (c == bb)
            return 0;
    }
    return 0;
}

/* walk the tree clearing dirty marks (the host's mini_layout_run already
   re-laid the document this frame; the marks are advisory for a future
   incremental pass). */
static void clear_dirty_rec(struct MiniNode *n)
{
    if (!n)
        return;
    n->dirty_layout = 0;
    n->dirty_paint = 0;
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        clear_dirty_rec(c);
    if (n->shadow_root)
        clear_dirty_rec(n->shadow_root);
}

/* Document the current tick is advancing, so apply_animations/tick_transitions
   (which already walk the whole tree) can fold their dirty/active-effect
   findings into doc->dirty / doc->active_effects at zero extra traversal cost.
   Set once at the top of mini_dom_tick_frame; NULL-safe everywhere. */
static MiniDocument *g_tick_doc = NULL;
static void apply_animations(struct MiniNode *n, double t)
{
    if (!n)
        return;
    /* gather any dirty mark set since the last tick (hover / DOM / style
       mutations all stamp dirty_layout/dirty_paint) into the doc summary so
       the host gate can decide whether a frame is needed. */
    if (g_tick_doc && (n->dirty_layout || n->dirty_paint))
        g_tick_doc->dirty = 1;
    const char *name = mini_node_get_attribute(n, "data-anim-name");
    int is_data_anim = (name && name[0]);
    if (!is_data_anim)
    {
        if (n->style.has_animation && n->style.anim_name[0])
            name = n->style.anim_name;
    }
    /* NOTE: active_effects / paint_dirty are no longer set unconditionally
       here — they are set below ONLY when this tick actually produces a
       visible change (a finite animation completing, or an eased value
       that differs from last tick by more than a sub-pixel epsilon). This
       is what lets a steps(2) cursor blink skip 33/34 frames of work and
       lets a one-shot `forwards` reveal stop driving frames after it
       finishes, instead of looping forever (the old code wrapped prog with
       fmod and ignored iteration_count, so every animation was effectively
       infinite). */
    if (name && name[0])
    {
        const char *durs = mini_node_get_attribute(n, "data-anim-dur");
        double dur = (durs && durs[0]) ? atof(durs) : (double)n->style.anim_duration;
        if (dur <= 0.0)
            dur = 1.0;

        const char *dirs = mini_node_get_attribute(n, "data-anim-dir");
        int dir = dirs ? (!strcmp(dirs, "alternate") ? 2 : 0) : n->style.anim_direction;

        double cycle = t / dur;
        /* data-anim (attribute-driven) animations have no iteration count and
           are treated as infinite; CSS animations honour anim_iteration_count. */
        int iter = is_data_anim ? -1 : n->style.anim_iteration_count;
        int finite = (iter > 0);
        int completed = finite && (cycle >= (double)iter);

        double prog;
        int apply_this_tick = 1; /* whether to (re)apply the keyframe body */

        if (completed)
        {
            if (n->style.anim_completed)
            {
                /* finished on a prior tick and the end state is already held
                   in the node's style — nothing to do, do NOT drive a frame. */
                apply_this_tick = 0;
            }
            else
            {
                /* completion tick: apply the end state once so a `forwards` /
                   `both` fill holds the 100% keyframe, then mark completed. */
                n->style.anim_completed = 1;
                prog = (dir == 1) ? 0.0 : 1.0; /* reverse ends at 0% */
            }
        }
        else
        {
            n->style.anim_completed = 0; /* still running (or restarted) */
            prog = fmod(cycle, 1.0);
            if (prog < 0)
                prog += 1.0;
            if (dir == 2 || dir == 3)
            {
                int cycle_num = (int)floor(cycle);
                if (cycle_num % 2 != 0)
                    prog = 1.0 - prog;
            }
            else if (dir == 1)
            {
                prog = 1.0 - prog;
            }
        }

        if (apply_this_tick)
        {
            /* Ease the in-cycle progress with the animation's timing function so
               ease / ease-in-out keyframes behave (linear timing is identity). CSS
               applies the timing per keyframe segment; this applies it across the
               whole cycle — a close-enough approximation for this engine. */
            float eased = mini_css_eval_timing(n->style.anim_timing,
                                               n->style.anim_bezier,
                                               (float)prog);
            /* Skip-unchanged: if this is a normal (non-completion) tick and the
               eased value is within a sub-pixel epsilon of last tick's, the
               rendered output is identical — skip the keyframe-body parse +
               mini_style_set churn entirely. This is the big win for steps()
               animations (a blink holds the same value for many frames). */
            int unchanged = (!completed) &&
                            (n->style.anim_last_eased >= 0.0f) &&
                            (fabsf(eased - n->style.anim_last_eased) < 0.0005f);
            n->style.anim_last_eased = eased;
            if (!unchanged)
            {
                if (g_tick_doc)
                {
                    g_tick_doc->active_effects = 1; /* a visible change happened */
                    g_tick_doc->paint_dirty = 1;
                }
                char *body = mini_css_keyframes_body_at(name, (double)eased);
                if (body)
                {
                    char *save;
                    char *decl = strtok_r(body, ";", &save);
                    while (decl)
                    {
                        char *colon = strchr(decl, ':');
                        if (colon)
                        {
                            *colon = 0;
                            char *prop = decl, *val = colon + 1;
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
                            if (*prop)
                            {
                                mini_style_set(n, prop, val);
                                n->dirty_paint = 1;
                            }
                        }
                        decl = strtok_r(NULL, ";", &save);
                    }
                    free(body);
                }
            }
        }
    }
    else if (is_data_anim && g_tick_doc)
    {
        /* data-anim-name attribute present but no CSS animation resolved it:
           keep the legacy behaviour of driving frames so attribute-driven
           effects keep ticking. */
        g_tick_doc->active_effects = 1;
    }
    if (n->pseudo_before)
        apply_animations(n->pseudo_before, t);
    if (n->pseudo_after)
        apply_animations(n->pseudo_after, t);
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        apply_animations(c, t);
    if (n->shadow_root)
        apply_animations(n->shadow_root, t);
}

static void tick_transitions(struct MiniNode *n, double dt)
{
    if (!n)
        return;
    MiniActiveTransition **prev = &n->active_transitions;
    while (*prev)
    {
        MiniActiveTransition *tr = *prev;
        if (g_tick_doc)
        {
            g_tick_doc->dirty = 1;
            g_tick_doc->active_effects = 1; /* a transition is still running */
            g_tick_doc->paint_dirty = 1;    /* interpolated value changed this frame */
        }
        tr->start_time += dt;
        float progress = tr->duration > 0.0 ? (float)(tr->start_time / tr->duration) : 1.0f;
        if (progress > 1.0f)
            progress = 1.0f;
        float ease = mini_css_eval_timing(tr->timing, tr->bezier, progress);

        if (!strcmp(tr->prop, "opacity"))
        {
            n->style.opacity = tr->start_val[0] * (1.0f - ease) + tr->target_val[0] * ease;
            n->style.has_opacity = 1;
            n->dirty_paint = 1;
        }
        else if (!strcmp(tr->prop, "transform"))
        {
            n->style.translate_x = tr->start_val[0] * (1.0f - ease) + tr->target_val[0] * ease;
            n->style.translate_y = tr->start_val[1] * (1.0f - ease) + tr->target_val[1] * ease;
            n->style.translate_z = tr->start_val[2] * (1.0f - ease) + tr->target_val[2] * ease;
            n->style.scale_x = tr->start_val[3] * (1.0f - ease) + tr->target_val[3] * ease;
            n->style.scale_y = tr->start_val[4] * (1.0f - ease) + tr->target_val[4] * ease;
            n->style.rotate_x = tr->start_val[5] * (1.0f - ease) + tr->target_val[5] * ease;
            n->style.rotate_y = tr->start_val[6] * (1.0f - ease) + tr->target_val[6] * ease;
            n->style.rotate_z = tr->start_val[7] * (1.0f - ease) + tr->target_val[7] * ease;
            n->style.has_transform = 1;
            n->dirty_paint = 1;
            if (g_tick_doc)
            {
                g_tick_doc->dirty = 1;
                g_tick_doc->active_effects = 1;
            }
        }
        else if (!strcmp(tr->prop, "box-shadow") || !strcmp(tr->prop, "shadow"))
        {
            n->style.shadow_x = tr->start_val[0] * (1.0f - ease) + tr->target_val[0] * ease;
            n->style.shadow_y = tr->start_val[1] * (1.0f - ease) + tr->target_val[1] * ease;
            n->style.shadow_blur = tr->start_val[2] * (1.0f - ease) + tr->target_val[2] * ease;
            n->style.shadow_spread = tr->start_val[3] * (1.0f - ease) + tr->target_val[3] * ease;
            n->style.shadow_r = tr->start_val[4] * (1.0f - ease) + tr->target_val[4] * ease;
            n->style.shadow_g = tr->start_val[5] * (1.0f - ease) + tr->target_val[5] * ease;
            n->style.shadow_b = tr->start_val[6] * (1.0f - ease) + tr->target_val[6] * ease;
            n->style.shadow_a = tr->start_val[7] * (1.0f - ease) + tr->target_val[7] * ease;
            n->style.has_shadow = (n->style.shadow_a > 0.001f);
            if (n->style.num_shadows > 0)
            {
                n->style.shadows[0].x = n->style.shadow_x;
                n->style.shadows[0].y = n->style.shadow_y;
                n->style.shadows[0].blur = n->style.shadow_blur;
                n->style.shadows[0].spread = n->style.shadow_spread;
                n->style.shadows[0].r = n->style.shadow_r;
                n->style.shadows[0].g = n->style.shadow_g;
                n->style.shadows[0].b = n->style.shadow_b;
                n->style.shadows[0].a = n->style.shadow_a;
            }
            n->dirty_paint = 1;
        }
        else if (!strcmp(tr->prop, "border-color") || !strcmp(tr->prop, "border"))
        {
            n->style.border_r = tr->start_val[0] * (1.0f - ease) + tr->target_val[0] * ease;
            n->style.border_g = tr->start_val[1] * (1.0f - ease) + tr->target_val[1] * ease;
            n->style.border_b = tr->start_val[2] * (1.0f - ease) + tr->target_val[2] * ease;
            n->style.border_a = tr->start_val[3] * (1.0f - ease) + tr->target_val[3] * ease;
            n->dirty_paint = 1;
        }
        else if (!strcmp(tr->prop, "background-color") || !strcmp(tr->prop, "background"))
        {
            n->style.bg_r = tr->start_val[0] * (1.0f - ease) + tr->target_val[0] * ease;
            n->style.bg_g = tr->start_val[1] * (1.0f - ease) + tr->target_val[1] * ease;
            n->style.bg_b = tr->start_val[2] * (1.0f - ease) + tr->target_val[2] * ease;
            n->style.bg_a = tr->start_val[3] * (1.0f - ease) + tr->target_val[3] * ease;
            n->dirty_paint = 1;
        }
        else if (!strcmp(tr->prop, "color"))
        {
            n->style.color_r = tr->start_val[0] * (1.0f - ease) + tr->target_val[0] * ease;
            n->style.color_g = tr->start_val[1] * (1.0f - ease) + tr->target_val[1] * ease;
            n->style.color_b = tr->start_val[2] * (1.0f - ease) + tr->target_val[2] * ease;
            n->style.color_a = tr->start_val[3] * (1.0f - ease) + tr->target_val[3] * ease;
            n->dirty_paint = 1;
        }
        else if (!strcmp(tr->prop, "left"))
        {
            n->style.len_left.v = tr->start_val[0] * (1.0f - ease) + tr->target_val[0] * ease;
            n->dirty_layout = 1;
            n->dirty_paint = 1;
        }
        else if (!strcmp(tr->prop, "top"))
        {
            n->style.len_top.v = tr->start_val[0] * (1.0f - ease) + tr->target_val[0] * ease;
            n->dirty_layout = 1;
            n->dirty_paint = 1;
        }

        if (progress >= 1.0f)
        {
            *prev = tr->next;
            free(tr);
        }
        else
        {
            prev = &tr->next;
        }
    }
    if (n->pseudo_before)
        tick_transitions(n->pseudo_before, dt);
    if (n->pseudo_after)
        tick_transitions(n->pseudo_after, dt);
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        tick_transitions(c, dt);
    if (n->shadow_root)
        tick_transitions(n->shadow_root, dt);
}

void mini_dom_tick_frame(MiniDocument *doc, double delta_time)
{
    if (!doc)
        return;
    g_anim_time += delta_time;
    g_tick_doc = doc;
    g_active_render_doc = doc;
    doc->dirty = 0;
    doc->active_effects = 0;
    /* paint_dirty is re-derived this tick by apply_animations /
       tick_transitions (only when a value actually changed). Clearing here
       lets the host skip the full clear+render+flush on frames where the
       only animations are unchanged (e.g. a steps() blink holding its
       value, or a completed one-shot reveal). */
    doc->paint_dirty = 0;

    /* 关键修复：只要当前帧有 JS Canvas 2D 指令待绘制，必须标记脏区和活跃特效，驱动下一帧 requestAnimationFrame 持续循环 */
    if (g2d_n > 0)
    {
        doc->dirty = 1;
        doc->active_effects = 1;
        doc->paint_dirty = 1;
    }

    apply_animations(doc->root, g_anim_time);
    tick_transitions(doc->root, delta_time);
    clear_dirty_rec(doc->root);
    g_tick_doc = NULL;
}

static void snapshot_base_styles(struct MiniNode *n)
{
    if (!n)
        return;
    n->base_style = n->style;
    n->has_base_style = 1;
    if (n->pseudo_before)
    {
        n->pseudo_before->base_style = n->pseudo_before->style;
        n->pseudo_before->has_base_style = 1;
    }
    if (n->pseudo_after)
    {
        n->pseudo_after->base_style = n->pseudo_after->style;
        n->pseudo_after->has_base_style = 1;
    }
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        snapshot_base_styles(c);
    if (n->shadow_root)
        snapshot_base_styles(n->shadow_root);
}

/* Reverse (un-hover) transition for a pseudo-element's animatable
 * properties, mirroring what revert_unhovered_node does for the host.
 * Previously only `left` (and `opacity` for ::before) reverted on pseudos,
 * so a ::before with a hover transform / shadow / background / border /
 * opacity snapped back instantly instead of transitioning. This makes
 * hover effects on pseudo-elements (e.g. the conic-gradient border's
 * opacity-on-hover) animate off smoothly. */
static void revert_pseudo_node(struct MiniNode *ps)
{
    if (!ps || !ps->has_base_style)
        return;
    MiniStyle *s = &ps->style;
    MiniStyle *b = &ps->base_style;

    /* transform (2D 7-tuple; 3D rotate_x/y are driven by @keyframes, not
       transition, so they are intentionally not in the tween tuple). */
    if (s->has_transform || b->has_transform)
    {
        float cur[8] = {s->translate_x, s->translate_y, s->translate_z,
                        s->scale_x > 0 ? s->scale_x : 1.0f,
                        s->scale_y > 0 ? s->scale_y : 1.0f,
                        s->rotate_x, s->rotate_y, s->rotate_z};
        float base[8] = {b->translate_x, b->translate_y, b->translate_z,
                         b->scale_x > 0 ? b->scale_x : 1.0f,
                         b->scale_y > 0 ? b->scale_y : 1.0f,
                         b->rotate_x, b->rotate_y, b->rotate_z};
        float dur = 0.3f;
        int timing = 0;
        float bez[4] = {0};
        if (find_transition_config(s, "transform", &dur, &timing, bez))
            mini_add_transition_val(ps, "transform", cur, base, 8, dur, timing, bez);
        else
        {
            s->translate_x = base[0];
            s->translate_y = base[1];
            s->translate_z = base[2];
            s->scale_x = base[3];
            s->scale_y = base[4];
            s->rotate_x = base[5];
            s->rotate_y = base[6];
            s->rotate_z = base[7];
            s->skew_x = b->skew_x;
            s->skew_y = b->skew_y;
            s->has_transform = b->has_transform;
        }
    }
    /* box-shadow */
    if (s->has_shadow || b->has_shadow || s->num_shadows > 0 || b->num_shadows > 0)
    {
        float cur[8] = {s->shadow_x, s->shadow_y, s->shadow_blur, s->shadow_spread,
                        s->shadow_r, s->shadow_g, s->shadow_b, s->shadow_a};
        float base[8] = {b->shadow_x, b->shadow_y, b->shadow_blur, b->shadow_spread,
                         b->shadow_r, b->shadow_g, b->shadow_b, b->shadow_a};
        float dur = 0.3f;
        int timing = 0;
        float bez[4] = {0};
        if (find_transition_config(s, "box-shadow", &dur, &timing, bez))
            mini_add_transition_val(ps, "box-shadow", cur, base, 8, dur, timing, bez);
        else
        {
            s->shadow_x = base[0];
            s->shadow_y = base[1];
            s->shadow_blur = base[2];
            s->shadow_spread = base[3];
            s->shadow_r = base[4];
            s->shadow_g = base[5];
            s->shadow_b = base[6];
            s->shadow_a = base[7];
            s->num_shadows = b->num_shadows;
            memcpy(s->shadows, b->shadows, sizeof(s->shadows));
        }
    }
    /* border-color */
    if (s->border_r != b->border_r || s->border_g != b->border_g ||
        s->border_b != b->border_b || s->border_a != b->border_a)
    {
        float cur[4] = {s->border_r, s->border_g, s->border_b, s->border_a};
        float base[4] = {b->border_r, b->border_g, b->border_b, b->border_a};
        float dur = 0.3f;
        int timing = 0;
        float bez[4] = {0};
        if (find_transition_config(s, "border-color", &dur, &timing, bez) ||
            find_transition_config(s, "border", &dur, &timing, bez))
            mini_add_transition_val(ps, "border-color", cur, base, 4, dur, timing, bez);
        else
        {
            s->border_r = base[0];
            s->border_g = base[1];
            s->border_b = base[2];
            s->border_a = base[3];
        }
    }
    /* background-color */
    if (s->bg_r != b->bg_r || s->bg_g != b->bg_g ||
        s->bg_b != b->bg_b || s->bg_a != b->bg_a)
    {
        float cur[4] = {s->bg_r, s->bg_g, s->bg_b, s->bg_a};
        float base[4] = {b->bg_r, b->bg_g, b->bg_b, b->bg_a};
        float dur = 0.3f;
        int timing = 0;
        float bez[4] = {0};
        if (find_transition_config(s, "background-color", &dur, &timing, bez) ||
            find_transition_config(s, "background", &dur, &timing, bez))
            mini_add_transition_val(ps, "background-color", cur, base, 4, dur, timing, bez);
        else
        {
            s->bg_r = base[0];
            s->bg_g = base[1];
            s->bg_b = base[2];
            s->bg_a = base[3];
        }
    }
    /* opacity */
    if (s->has_opacity != b->has_opacity || fabsf(s->opacity - b->opacity) > 1e-4f)
    {
        float cur = s->has_opacity ? s->opacity : 1.0f;
        float base = b->has_opacity ? b->opacity : 1.0f;
        float dur = 0.3f;
        int timing = 0;
        float bez[4] = {0};
        if (find_transition_config(s, "opacity", &dur, &timing, bez))
            mini_add_transition_val(ps, "opacity", &cur, &base, 1, dur, timing, bez);
        else
        {
            s->opacity = base;
            s->has_opacity = b->has_opacity;
        }
    }
    /* left */
    if (fabsf(s->len_left.v - b->len_left.v) > 1e-4f)
    {
        float cur = s->len_left.v, base = b->len_left.v;
        float dur = 0.3f;
        int timing = 0;
        float bez[4] = {0};
        if (find_transition_config(s, "left", &dur, &timing, bez) ||
            find_transition_config(s, "all", &dur, &timing, bez))
            mini_add_transition_val(ps, "left", &cur, &base, 1, dur, timing, bez);
        else
            s->len_left = b->len_left;
    }
    /* top */
    if (fabsf(s->len_top.v - b->len_top.v) > 1e-4f)
    {
        float cur = s->len_top.v, base = b->len_top.v;
        float dur = 0.3f;
        int timing = 0;
        float bez[4] = {0};
        if (find_transition_config(s, "top", &dur, &timing, bez))
            mini_add_transition_val(ps, "top", &cur, &base, 1, dur, timing, bez);
        else
            s->len_top = b->len_top;
    }
    if (s->color_r != b->color_r || s->color_g != b->color_g ||
        s->color_b != b->color_b || s->color_a != b->color_a)
    {
        float cur[4] = {s->color_r, s->color_g, s->color_b, s->color_a};
        float base[4] = {b->color_r, b->color_g, b->color_b, b->color_a};
        float dur = 0.3f;
        int timing = 0;
        float bez[4] = {0};
        if (find_transition_config(s, "color", &dur, &timing, bez))
            mini_add_transition_val(ps, "color", cur, base, 4, dur, timing, bez);
        else
        {
            s->color_r = base[0];
            s->color_g = base[1];
            s->color_b = base[2];
            s->color_a = base[3];
        }
    }
}

static int is_node_or_ancestor_interacting(struct MiniNode *n)
{
    for (struct MiniNode *p = n; p; p = p->parent)
    {
        if (p->state_hovered || p->state_active || p->state_focused)
            return 1;
    }
    return 0;
}

static void revert_unhovered_node(struct MiniNode *n)
{
    if (!n || !n->has_base_style)
        return;
    int is_interacting = is_node_or_ancestor_interacting(n);
    if (!is_interacting && !mini_node_get_attribute(n, "style"))
    {
        /* 统一复用更强大的平滑动画恢复功能 */
        revert_pseudo_node(n);
    }

    /* 将伪元素也纳入 unhover 检查树，确保 ::after 底边线动画能平滑缩回 */
    if (n->pseudo_before)
        revert_unhovered_node(n->pseudo_before);
    if (n->pseudo_after)
        revert_unhovered_node(n->pseudo_after);

    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        revert_unhovered_node(c);
    if (n->shadow_root)
        revert_unhovered_node(n->shadow_root);
}

static void apply_inline_styles(struct MiniNode *n)
{
    if (!n)
        return;
    const char *style_attr = mini_node_get_attribute(n, "style");
    if (style_attr && style_attr[0])
    {
        char *buf = strdup(style_attr);
        if (buf)
        {
            char *save;
            char *decl = strtok_r(buf, ";", &save);
            while (decl)
            {
                char *colon = strchr(decl, ':');
                if (colon)
                {
                    *colon = 0;
                    char *p = decl, *v = colon + 1;
                    while (*p && isspace((unsigned char)*p))
                        p++;
                    while (*v && isspace((unsigned char)*v))
                        v++;
                    char *e = p + strlen(p);
                    while (e > p && isspace((unsigned char)e[-1]))
                        *--e = 0;
                    e = v + strlen(v);
                    while (e > v && isspace((unsigned char)e[-1]))
                        *--e = 0;
                    if (*p)
                        mini_style_set(n, p, v);
                }
                decl = strtok_r(NULL, ";", &save);
            }
            free(buf);
        }
    }
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        apply_inline_styles(c);
}

void mini_dom_restyle(struct MiniDocument *doc)
{
    if (!doc || !g_restyle_css)
        return;
    if (doc->root)
        revert_unhovered_node(doc->root);
    g_restyling = 1;
    mini_css_apply(doc, g_restyle_css);
    if (doc->root)
        apply_inline_styles(doc->root);
    g_restyling = 0;
    /* A restyle re-derives styles for the whole tree; geometry may have
       changed (display / width / grid-template / font-size …). Force the
       next mini_layout_run to actually run. */
    doc->layout_dirty = 1;
}

/* ================================================================== */
/* Phase 5: Resource Provider & Font Management System                */
/* ================================================================== */
void mini_dom_set_resource_loader(struct MiniDocument *doc, MiniResourceLoaderCb cb, void *user_data)
{
    MiniDocumentContext *ctx = mini_get_ctx(doc);
    ctx->res_loader_func = cb;
    ctx->res_loader_user_data = user_data;
}

static uint8_t *mini_base64_decode(const char *src, size_t len, size_t *out_len)
{
    if (!src || !out_len)
        return NULL;
    static const int8_t b64_table[256] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, 0, -1, -1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
        -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    size_t cap = (len / 4 + 1) * 3 + 4;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf)
        return NULL;
    size_t out_n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)src[i];
        if (c == '=')
            break;
        int8_t val = b64_table[c];
        if (val < 0)
            continue;
        acc = (acc << 6) | (uint32_t)val;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            buf[out_n++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    *out_len = out_n;
    return buf;
}

MiniResource mini_dom_load_resource(struct MiniDocument *doc, const char *url_or_path, const char *type)
{
    (void)doc;
    MiniResource res = {0};
    if (!url_or_path || !*url_or_path)
        return res;

    /* 1. External resource loader callback */
    if (g_res_loader_cb)
    {
        res = g_res_loader_cb(url_or_path, type, g_res_loader_ud);
        if (res.data && res.size > 0)
            return res;
    }

    /* 2. Data URI decoding (data:image/...;base64,...) */
    if (!strncmp(url_or_path, "data:", 5))
    {
        const char *comma = strchr(url_or_path, ',');
        if (comma)
        {
            const char *b64_start = comma + 1;
            size_t b64_len = strlen(b64_start);
            size_t decoded_len = 0;
            uint8_t *decoded = mini_base64_decode(b64_start, b64_len, &decoded_len);
            if (decoded && decoded_len > 0)
            {
                res.data = decoded;
                res.size = decoded_len;
                res.should_free = 1;
                return res;
            }
        }
    }

    /* 3. Local filesystem loading (supports file://, absolute, relative paths) */
    const char *file_path = url_or_path;
    if (!strncmp(file_path, "file:///", 8))
        file_path += 8;
    else if (!strncmp(file_path, "file://", 7))
        file_path += 7;

    FILE *f = fopen(file_path, "rb");
    if (f)
    {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0)
        {
            uint8_t *raw = (uint8_t *)malloc((size_t)sz);
            if (raw)
            {
                size_t rd = fread(raw, 1, (size_t)sz, f);
                res.data = raw;
                res.size = rd;
                res.should_free = 1;
                fclose(f);
                return res;
            }
        }
        fclose(f);
    }

    /* 4. Remote HTTP/HTTPS URL loading via mini_net */
    if (!strncmp(url_or_path, "http://", 7) || !strncmp(url_or_path, "https://", 8))
    {
        extern int mini_net_http(const char *method, const char *url, const char **req_headers, int req_h_n, const uint8_t *req_body, size_t req_body_sz, int *out_status, uint8_t **out_body, size_t *out_body_sz);
        uint8_t *body = NULL;
        size_t blen = 0;
        int status = 0;
        if (mini_net_http("GET", url_or_path, NULL, 0, NULL, 0, &status, &body, &blen) == 0 && body && blen > 0)
        {
            res.data = body;
            res.size = blen;
            res.should_free = 1;
            return res;
        }
    }

    return res;
}

void mini_dom_free_resource(MiniResource *res)
{
    if (res && res->should_free && res->data)
    {
        free((void *)res->data);
        res->data = NULL;
        res->size = 0;
        res->should_free = 0;
    }
}

void mini_dom_register_font(struct MiniDocument *doc, const char *family_name, const uint8_t *data, size_t size)
{
    (void)doc;
    if (!data || size == 0)
        return;
    extern int mini_renderer_add_font_data(void *r, const char *family, const uint8_t *data, size_t size);
    mini_renderer_add_font_data(NULL, family_name, data, size);
}

/* ================================================================== */
/* PARSE_SELFTEST — feed raw HTML, assert the tree.                    */
/* ================================================================== */
#ifdef PARSE_SELFTEST
#include <stdio.h>
static int g_fail = 0;
static void ck(int cond, const char *m)
{
    if (cond)
        fprintf(stderr, "[PASS] %s\n", m);
    else
    {
        fprintf(stderr, "[FAIL] %s\n", m);
        g_fail++;
    }
}
static const char *attrv(const struct MiniNode *n, const char *k) { return mini_node_get_attribute(n, k); }
static int childcount(const struct MiniNode *n)
{
    int c = 0;
    for (struct MiniNode *x = n->first_child; x; x = x->next_sibling)
        c++;
    return c;
}
static struct MiniNode *child_at(const struct MiniNode *n, int i)
{
    struct MiniNode *x = n->first_child;
    while (i-- > 0 && x)
        x = x->next_sibling;
    return x;
}
int main(void)
{
    const char *html =
        "<!DOCTYPE html><html><body>\n"
        "  <div id=\"main\" class=\"container\">\n"
        "    <p>Hello <b>world</b>!</p>\n"
        "    <img src=\"x.png\" alt=\"pic\">\n"
        "    <br>\n"
        "    <ul><li>one</li><li>two</li></ul>\n"
        "    <script>var a = 1 < 2;</script>\n"
        "    <style>body{color:red}</style>\n"
        "    <input type=\"text\" name=\"q\">\n"
        "  </div>\n"
        "</body></html>";
    MiniDocument *d = mini_doc_create();
    mini_dom_parse_html_legacy(d, html);

    struct MiniNode *body = d->body;
    ck(childcount(body) == 1, "body has 1 child (div) [whitespace-only text skipped]");
    struct MiniNode *div = child_at(body, 0);
    ck(div && div->tag && !strcmp(div->tag, "div"), "div is an element 'div'");
    ck(attrv(div, "id") && !strcmp(attrv(div, "id"), "main"), "div id=main");
    ck(attrv(div, "class") && !strcmp(attrv(div, "class"), "container"), "div class=container");

    /* div children: p, img, br, ul, script, style, input (7) */
    ck(childcount(div) == 7, "div has 7 children");

    struct MiniNode *p = child_at(div, 0);
    ck(p && !strcmp(p->tag, "p"), "child0 = p");
    /* p: text 'Hello ' + b('world') + text '!' */
    ck(childcount(p) == 3, "p has 3 children (text+b+text)");
    struct MiniNode *b = child_at(p, 1);
    ck(b && !strcmp(b->tag, "b"), "p[1] = b");
    ck(b->first_child && b->first_child->text &&
           !strcmp(b->first_child->text, "world"),
       "b contains 'world'");

    struct MiniNode *img = child_at(div, 1);
    ck(img && !strcmp(img->tag, "img"), "child1 = img");
    ck(attrv(img, "src") && !strcmp(attrv(img, "src"), "x.png"), "img src=x.png");
    ck(childcount(img) == 0, "img is void (no children)");

    struct MiniNode *ul = child_at(div, 3);
    ck(ul && !strcmp(ul->tag, "ul"), "child3 = ul");
    ck(childcount(ul) == 2, "ul has 2 li");

    struct MiniNode *sc = child_at(div, 4);
    ck(sc && !strcmp(sc->tag, "script"), "child4 = script");
    ck(sc->text && !strcmp(sc->text, "var a = 1 < 2;"), "script raw text preserved (with '<')");

    struct MiniNode *st = child_at(div, 5);
    ck(st && !strcmp(st->tag, "style"), "child5 = style");
    ck(st->text && !strcmp(st->text, "body{color:red}"), "style raw text preserved");

    struct MiniNode *inp = child_at(div, 6);
    ck(inp && !strcmp(inp->tag, "input"), "child6 = input");
    ck(attrv(inp, "type") && !strcmp(attrv(inp, "type"), "text"), "input type=text");
    ck(childcount(inp) == 0, "input is void");

    /* ---- Phase 1.2: CSS selector matcher against the parsed tree ---- */
    {
        struct MiniNode *m;
        m = mini_dom_query_selector(d, "#main");
        ck(m && m == div, "querySelector('#main')");
        m = mini_dom_query_selector(d, ".container");
        ck(m && m == div, "querySelector('.container')");
        m = mini_dom_query_selector(d, "img");
        ck(m && !strcmp(m->tag, "img"), "querySelector('img')");
        m = mini_dom_query_selector(d, "div.container");
        ck(m && m == div, "compound 'div.container'");
        m = mini_dom_query_selector(d, "[type]");
        ck(m && !strcmp(m->tag, "input"), "querySelector('[type]')");
        m = mini_dom_query_selector(d, "[type=text]");
        ck(m && !strcmp(m->tag, "input"), "querySelector('[type=text]')");
        struct MiniNode *o[16];
        int c;
        c = mini_dom_query_selector_all(d, "li", o, 16);
        ck(c == 2, "querySelectorAll('li') -> 2");
        c = mini_dom_query_selector_all(d, "input", o, 16);
        ck(c == 1, "querySelectorAll('input') -> 1");
        c = mini_dom_query_selector_all(d, "*", o, 16);
        ck(c >= 7, "querySelectorAll('*') >= 7");
        c = mini_dom_query_selector_all(d, "p, br", o, 16);
        ck(c == 2, "comma 'p, br' -> 2");
        m = mini_dom_query_selector(d, ".nope");
        ck(m == NULL, "querySelector('.nope') -> null");
    }

    mini_doc_destroy(d);
    fprintf(stderr, g_fail ? "PARSE_SELFTEST: %d FAIL\n" : "PARSE_SELFTEST: all PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
#endif

#ifdef COLOR_SELFTEST
#include <math.h>
static int color_close(float a, float b) { return fabsf(a - b) < 0.02f; }
static int color_eq(float r, float g, float b, float a,
                    float er, float eg, float eb, float ea)
{
    return color_close(r, er) && color_close(g, eg) &&
           color_close(b, eb) && color_close(a, ea);
}
int main(void)
{
    int fail = 0;
    float r, g, b, a;
#define CK(expr, er, eg, eb, ea)                                  \
    do                                                            \
    {                                                             \
        mini_parse_color(expr, &r, &g, &b, &a);                   \
        if (!color_eq(r, g, b, a, er, eg, eb, ea))                \
        {                                                         \
            fprintf(stderr, "[FAIL] %s -> %.3f %.3f %.3f %.3f\n", \
                    expr, r, g, b, a);                            \
            fail++;                                               \
        }                                                         \
        else                                                      \
            fprintf(stderr, "[PASS] %s\n", expr);                 \
    } while (0)
    CK("#000", 0, 0, 0, 1);
    CK("#fff", 1, 1, 1, 1);
    CK("#abc", 0xaa / 255.0f, 0xbb / 255.0f, 0xcc / 255.0f, 1);
    CK("#1a2b3c", 0x1a / 255.0f, 0x2b / 255.0f, 0x3c / 255.0f, 1);
    CK("#ff000080", 1, 0, 0, 0x80 / 255.0f);
    CK("rgb(255,0,0)", 1, 0, 0, 1);
    CK("rgba(0,255,0,0.5)", 0, 1, 0, 0.5f);
    CK("rgb(100%,0,0)", 1, 0, 0, 1);
    CK("hsl(0,100%,50%)", 1, 0, 0, 1);
    CK("hsl(120,100%,50%)", 0, 1, 0, 1);
    CK("hsl(240,100%,50%)", 0, 0, 1, 1);
    CK("red", 1, 0, 0, 1);
    CK("orange", 1, 0xa5 / 255.0f, 0, 1);
    CK("transparent", 0, 0, 0, 0);
    CK("white", 1, 1, 1, 1);
#undef CK
    fprintf(stderr, fail ? "COLOR_SELFTEST: %d FAIL\n" : "COLOR_SELFTEST: all PASS\n", fail);
    return fail ? 1 : 0;
}
#endif

#ifdef BOX_SELFTEST
#include <stdio.h>
static int bx_fail = 0;
static void bxck(int cond, const char *m)
{
    if (cond)
        fprintf(stderr, "[PASS] %s\n", m);
    else
    {
        fprintf(stderr, "[FAIL] %s\n", m);
        bx_fail++;
    }
}
static int fclose_(float x, float y)
{
    float d = x - y;
    if (d < 0.0f)
        d = -d;
    return d < 0.5f;
}
int main(void)
{
    MiniDocument *d = mini_doc_create();
    struct MiniNode *n = mini_node_create_element("div");
    mini_node_append_child(d->body, n);
    MiniStyle *s = &n->style;

    /* shorthand + longhand (resolved later by the layout pass) */
    mini_style_set(n, "margin", "10px 20px 30px 40px");
    mini_style_set(n, "padding", "5px 10px");
    mini_style_set(n, "padding-top", "7px");
    /* borders stay eager (px) */
    mini_style_set(n, "border", "2px dashed red");
    mini_style_set(n, "border-top-width", "5px");
    mini_style_set(n, "border-left", "3px solid blue");
    mini_style_set(n, "border-style", "dotted");
    mini_style_set(n, "border-radius", "8px");

    /* unit resolution: % / rem / vh on p; em (font) / em (box) / vw on q */
    struct MiniNode *p = mini_node_create_element("div");
    mini_node_append_child(d->body, p);
    mini_style_set(p, "width", "50%");        /* 50% of 1000 = 500 */
    mini_style_set(p, "font-size", "1.5rem"); /* 1.5 * 16 root = 24 */
    mini_style_set(p, "height", "10vh");      /* 10% of 800 = 80 */
    struct MiniNode *q = mini_node_create_element("div");
    mini_node_append_child(p, q);
    mini_style_set(q, "font-size", "2em"); /* 2 * parent 24 = 48 */
    mini_style_set(q, "width", "1em");     /* 1 * own 48 = 48 */
    mini_style_set(q, "margin", "5vw");    /* 5% of 1000 = 50 */

    /* flex gap + align-self + min/max-width (laid out below) */
    struct MiniNode *fx = mini_node_create_element("div");
    mini_node_append_child(d->body, fx);
    mini_style_set(fx, "display", "flex");
    mini_style_set(fx, "flex-direction", "row");
    mini_style_set(fx, "width", "200px");
    mini_style_set(fx, "gap", "10px");
    struct MiniNode *c1 = mini_node_create_element("div");
    struct MiniNode *c2 = mini_node_create_element("div");
    struct MiniNode *c3 = mini_node_create_element("div");
    mini_node_append_child(fx, c1);
    mini_node_append_child(fx, c2);
    mini_node_append_child(fx, c3);
    mini_style_set(c1, "width", "50px");
    mini_style_set(c2, "width", "50px");
    mini_style_set(c3, "width", "50px");
    mini_style_set(c3, "align-self", "center");
    struct MiniNode *mn = mini_node_create_element("div");
    mini_node_append_child(d->body, mn);
    mini_style_set(mn, "width", "500px");
    mini_style_set(mn, "max-width", "100px");
    struct MiniNode *mn2 = mini_node_create_element("div");
    mini_node_append_child(d->body, mn2);
    mini_style_set(mn2, "width", "10px");
    mini_style_set(mn2, "min-width", "50px");

    mini_layout_run(d, 1000, 800); /* vw=1000 vh=800 root_font=16 */

    /* margin/padding resolved from raw lengths (px → themselves) */
    bxck(fclose_(s->margin[0], 10) && fclose_(s->margin[1], 20) &&
             fclose_(s->margin[2], 30) && fclose_(s->margin[3], 40),
         "margin 4-value -> [10,20,30,40]");
    bxck(fclose_(s->padding[0], 7) && fclose_(s->padding[1], 10) &&
             fclose_(s->padding[2], 5) && fclose_(s->padding[3], 10),
         "padding 5/10 + padding-top=7 -> [7,10,5,10]");

    /* borders (eager) — final state after all border sets */
    bxck(fclose_(s->border_w[0], 5) && fclose_(s->border_w[1], 2) &&
             fclose_(s->border_w[2], 2) && fclose_(s->border_w[3], 3),
         "border widths top5 right2 bottom2 left3");
    bxck(s->border_style[0] == 3 && s->border_style[1] == 3 &&
             s->border_style[2] == 3 && s->border_style[3] == 3,
         "border styles all dotted(3)");
    bxck(fclose_(s->border_b, 1) && fclose_(s->border_r, 0),
         "border color = blue");
    bxck(fclose_(s->border_radius, 8), "border-radius = 8");
    bxck(s->has_border == 1, "has_border set");

    /* units */
    bxck(fclose_(p->style.w, 500), "width:50% of 1000 = 500");
    bxck(fclose_(p->style.font_size, 24), "font-size:1.5rem = 24");
    bxck(fclose_(p->style.h, 80), "height:10vh of 800 = 80");
    bxck(fclose_(q->style.font_size, 48), "font-size:2em (parent 24) = 48");
    bxck(fclose_(q->style.w, 48), "width:1em (own font 48) = 48");
    bxck(fclose_(q->style.margin[0], 50) && fclose_(q->style.margin[3], 50),
         "margin:5vw of 1000 = 50 (top & left)");

    /* flex gap: 3×50 + 2×10 = 170; children at 0, 60, 120 */
    bxck(fclose_(c2->style.abs_x - c1->style.abs_x, 60),
         "flex gap: child2 - child1 = 60 (50 + 10 gap)");
    bxck(fclose_(c3->style.abs_x - c2->style.abs_x, 60),
         "flex gap: child3 - child2 = 60");
    /* align-self center on c3 (cross axis = vertical; row → y centered) */
    bxck(fclose_(c3->style.abs_y, (fx->style.abs_y + (fx->style.h - c3->style.h) / 2)),
         "align-self: center on c3 (vertical)");
    /* min/max-width clamp */
    bxck(fclose_(mn->style.w, 100), "max-width clamps 500 -> 100");
    bxck(fclose_(mn2->style.w, 50), "min-width clamps 10 -> 50");

    mini_doc_destroy(d);
    fprintf(stderr, bx_fail ? "BOX_SELFTEST: %d FAIL\n" : "BOX_SELFTEST: all PASS\n", bx_fail);
    return bx_fail ? 1 : 0;
}
#endif

#if defined(DOM_CSS_SELFTEST)
#include <stdio.h>
static int css_fail = 0;
static void cssck(int c, const char *m)
{
    if (c)
        fprintf(stderr, "[PASS] %s\n", m);
    else
    {
        fprintf(stderr, "[FAIL] %s\n", m);
        css_fail++;
    }
}
static int ceq(float a, float b)
{
    float d = a - b;
    if (d < 0.0f)
        d = -d;
    return d < 0.02f;
}
int main(void)
{
    MiniDocument *d = mini_doc_create();
    const char *html = "<!DOCTYPE html><html><body><div id=\"x\" class=\"a\"></div></body></html>";
    mini_dom_parse_html(d, html);
    struct MiniNode *x = mini_dom_query_selector(d, "#x");
    cssck(x && x->tag && !strcmp(x->tag, "div"), "found #x div");

    /* case 1: specificity #id > .class > type (all target #x) */
    mini_style_set(x, "color", "#000000");
    mini_css_apply(d, "div{color:#ff0000}.a{color:#00ff00}#x{color:#0000ff}");
    cssck(x && ceq(x->style.color_b, 1.0f), "spec: #x beats .a beats div -> blue");

    /* case 2: !important beats higher specificity */
    mini_style_set(x, "color", "#000000");
    mini_css_apply(d, "#x{color:#0000ff}div{color:#ff0000 !important}");
    cssck(x && ceq(x->style.color_r, 1.0f), "!important div beats #x -> red");

    /* case 3: specificity beats source order (.a later but lower spec) */
    mini_style_set(x, "color", "#000000");
    mini_css_apply(d, "div.a{color:#ff0000}.a{color:#00ff00}");
    cssck(x && ceq(x->style.color_r, 1.0f), "div.a(101) beats later .a(100) -> red");

    mini_doc_destroy(d);
    fprintf(stderr, css_fail ? "CSS_SELFTEST: %d FAIL\n" : "CSS_SELFTEST: all PASS\n", css_fail);
    return css_fail ? 1 : 0;
}
#endif

#ifdef SELECTOR_SELFTEST
#include <stdio.h>
static int sel_fail = 0;
static void selck(int c, const char *m)
{
    if (c)
        fprintf(stderr, "[PASS] %s\n", m);
    else
    {
        fprintf(stderr, "[FAIL] %s\n", m);
        sel_fail++;
    }
}
static int q_count(MiniDocument *d, const char *s)
{
    struct MiniNode *o[64];
    return mini_dom_query_selector_all(d, s, o, 64);
}
int main(void)
{
    MiniDocument *d = mini_doc_create();
    const char *html = "<!DOCTYPE html><html><body>"
                       "<div id=\"r\"><section><p class=\"deep\">D</p></section>"
                       "<p class=\"a\">1</p><p class=\"b\">2</p>"
                       "<span data-x=\"hello world\" title=\"btn\" lang=\"en-US\">s</span>"
                       "<p class=\"a\">3</p></div></body></html>";
    mini_dom_parse_html(d, html);

    /* combinators */
    selck(q_count(d, "div p") == 4, "descendant div p -> 4");
    selck(q_count(d, "div > p") == 3, "child div > p -> 3 (deep excluded)");
    selck(q_count(d, "p + p") == 1, "adjacent p + p -> 1");
    selck(q_count(d, "p ~ p") == 2, "general sibling p ~ p -> 2");

    /* pseudo-classes */
    selck(q_count(d, "#r p:first-child") == 1, "#r p:first-child -> deep");
    selck(q_count(d, "#r p:last-child") == 2, "#r p:last-child -> 2");
    selck(q_count(d, "#r > :nth-child(2)") == 1, "#r > :nth-child(2) -> 1");
    selck(q_count(d, "#r > p:not(.a)") == 1, "#r > p:not(.a) -> 1");

    /* attribute operators */
    selck(q_count(d, "[data-x~=world]") == 1, "[data-x~=world] -> 1");
    selck(q_count(d, "[title^=btn]") == 1, "[title^=btn] -> 1");
    selck(q_count(d, "[title$=tn]") == 1, "[title$=tn] -> 1");
    selck(q_count(d, "[data-x*=lo]") == 1, "[data-x*=lo] -> 1");
    selck(q_count(d, "[lang|=en]") == 1, "[lang|=en] -> 1");

    /* specificity across combinators */
    selck(spec_of("#r p") == 10001u, "spec #r p = 10001 (id + type)");
    selck(spec_of(".a p") == 101u, "spec .a p = 101 (class + type)");

    mini_doc_destroy(d);
    fprintf(stderr, sel_fail ? "SELECTOR_SELFTEST: %d FAIL\n" : "SELECTOR_SELFTEST: all PASS\n", sel_fail);
    return sel_fail ? 1 : 0;
}
#endif

#ifdef ENTITY_SELFTEST
#include <stdio.h>
#include <string.h>
static int ent_fail = 0;
static void eck(int c, const char *m)
{
    if (c)
        fprintf(stderr, "[PASS] %s\n", m);
    else
    {
        fprintf(stderr, "[FAIL] %s\n", m);
        ent_fail++;
    }
}
int main(void)
{
    MiniDocument *d = mini_doc_create();
    const char *html = "<!DOCTYPE html><html><body>"
                       "<p>a&amp;b &lt;c&gt; &quot;d&quot; &apos;e&apos; &#65;&#x42; &nbsp; &copy; &foo;</p>"
                       "</body></html>";
    mini_dom_parse_html_legacy(d, html);
    struct MiniNode *p = mini_dom_query_selector(d, "p");
    struct MiniNode *tn = p ? p->first_child : NULL;
    const char *tx = (tn && tn->text) ? tn->text : "";
    eck(p && tn && tn->type == MN_TEXT_NODE, "p has a text child");
    eck(strstr(tx, "a&b <c> \"d\" 'e' AB") != NULL,
        "decode &amp; &lt; &gt; &quot; &apos; &#65; &#x42;");
    eck(strchr(tx, ' ') != NULL, "&nbsp; decoded to a space");
    eck(strstr(tx, "\xC2\xA9") != NULL, "&copy; -> UTF-8 copyright bytes");
    eck(strstr(tx, "&foo;") != NULL, "unknown &foo; left as literal");
    mini_doc_destroy(d);
    fprintf(stderr, ent_fail ? "ENTITY_SELFTEST: %d FAIL\n" : "ENTITY_SELFTEST: all PASS\n", ent_fail);
    return ent_fail ? 1 : 0;
}
#endif

#ifdef POSITION_SELFTEST
#include <stdio.h>
static int pos_fail = 0;
static void posck(int c, const char *m)
{
    if (c)
        fprintf(stderr, "[PASS] %s\n", m);
    else
    {
        fprintf(stderr, "[FAIL] %s\n", m);
        pos_fail++;
    }
}
static int pc(float a, float b)
{
    float d = a - b;
    if (d < 0.0f)
        d = -d;
    return d < 0.5f;
}
int main(void)
{
    MiniDocument *d = mini_doc_create();
    struct MiniNode *box = mini_node_create_element("div");
    mini_node_append_child(d->body, box);
    mini_style_set(box, "position", "relative");
    mini_style_set(box, "width", "200px");
    mini_style_set(box, "height", "200px");

    /* absolute child: positioned relative to the relative parent's padding box */
    struct MiniNode *ab = mini_node_create_element("div");
    mini_node_append_child(box, ab);
    mini_style_set(ab, "position", "absolute");
    mini_style_set(ab, "top", "20px");
    mini_style_set(ab, "left", "30px");
    mini_style_set(ab, "width", "50px");
    mini_style_set(ab, "height", "50px");

    /* relative child: kept in flow, visually shifted */
    struct MiniNode *rl = mini_node_create_element("b");
    mini_node_append_child(box, rl);
    mini_style_set(rl, "position", "relative");
    mini_style_set(rl, "left", "10px");
    mini_style_set(rl, "top", "5px");

    mini_layout_run(d, 1000, 800);

    posck(pc(box->style.abs_x, 0) && pc(box->style.abs_y, 0), "relative box at (0,0)");
    posck(pc(ab->style.abs_x, 30) && pc(ab->style.abs_y, 20),
          "absolute child at (30,20) from parent padding box");
    posck(pc(rl->style.abs_x, 10) && pc(rl->style.abs_y, 5),
          "relative child shifted to (10,5)");
    posck(ab->style.laid_out && rl->style.laid_out, "both children laid out");

    mini_doc_destroy(d);
    fprintf(stderr, pos_fail ? "POSITION_SELFTEST: %d FAIL\n" : "POSITION_SELFTEST: all PASS\n", pos_fail);
    return pos_fail ? 1 : 0;
}
#endif

/* ================================================================== */
/* LAYOUT_SELFTEST: CSS Grid track sizing, Float+BFC, Multicol.         */
/* ================================================================== */
#ifdef LAYOUT_SELFTEST
#include <math.h>
static int lay_fail = 0;
static void layck(int c, const char *m)
{
    if (!c)
    {
        fprintf(stderr, "LAYOUT FAIL: %s\n", m);
        lay_fail++;
    }
}
static int lclose(float a, float b) { return fabsf(a - b) < 1.5f; }

static struct MiniNode *mkdiv(struct MiniNode *parent, const char *tag)
{
    struct MiniNode *d = mini_node_create_element(tag ? tag : "div");
    mini_node_append_child(parent, d);
    return d;
}

int main(void)
{
    /* ---- CSS Grid: 1fr 2fr 1fr at 900px → 225 / 450 / 225 ---- */
    {
        MiniDocument *d = mini_doc_create();
        struct MiniNode *g = mkdiv(d->body, "div");
        mini_style_set(g, "display", "grid");
        mini_style_set(g, "grid-template-columns", "1fr 2fr 1fr");
        mini_style_set(g, "width", "900px");
        struct MiniNode *c0 = mkdiv(g, "div");
        struct MiniNode *c1 = mkdiv(g, "div");
        struct MiniNode *c2 = mkdiv(g, "div");
        (void)c0;
        (void)c1;
        (void)c2;
        mini_layout_run(d, 900, 600);
        /* each child laid out in its track; widths track the fr factors */
        struct MiniNode *k = g->first_child;
        float w0 = k ? k->style.w : -1;
        k = k ? k->next_sibling : NULL;
        float w1 = k ? k->style.w : -1;
        k = k ? k->next_sibling : NULL;
        float w2 = k ? k->style.w : -1;
        layck(lclose(w0, 225) && lclose(w1, 450) && lclose(w2, 225), "grid fr distribution 1fr/2fr/1fr");
        mini_doc_destroy(d);
    }

    /* ---- Grid minmax + repeat ---- */
    {
        MiniDocument *d = mini_doc_create();
        struct MiniNode *g = mkdiv(d->body, "div");
        mini_style_set(g, "display", "grid");
        mini_style_set(g, "grid-template-columns", "repeat(3, minmax(100px, 1fr))");
        mini_style_set(g, "width", "900px");
        for (int i = 0; i < 3; i++)
            mkdiv(g, "div");
        mini_layout_run(d, 900, 600);
        struct MiniNode *k = g->first_child;
        layck(k && lclose(k->style.w, 300), "grid repeat(3,minmax(100px,1fr)) → 300px");
        mini_doc_destroy(d);
    }

    /* ---- Float + BFC: two left floats + a block sibling ---- */
    {
        MiniDocument *d = mini_doc_create();
        struct MiniNode *box = mkdiv(d->body, "div");
        mini_style_set(box, "overflow", "hidden"); /* establishes BFC → contains floats */
        mini_style_set(box, "width", "400px");
        struct MiniNode *f1 = mkdiv(box, "div");
        mini_style_set(f1, "float", "left");
        mini_style_set(f1, "width", "100px");
        mini_style_set(f1, "height", "50px");
        struct MiniNode *f2 = mkdiv(box, "div");
        mini_style_set(f2, "float", "left");
        mini_style_set(f2, "width", "100px");
        mini_style_set(f2, "height", "50px");
        struct MiniNode *blk = mkdiv(box, "div");
        mini_style_set(blk, "width", "300px");
        mini_style_set(blk, "height", "20px");
        mini_layout_run(d, 800, 600);
        /* the block sibling must be pushed below the floats (no room beside
           two 100px floats + 300px block in a 400px container → wraps below) */
        layck(blk->style.abs_y >= f1->style.abs_y + 50.0f, "float pushes block sibling below");
        /* BFC contains floats: container height encloses the floats (>=50) */
        layck(box->style.h >= 50.0f, "BFC contains floats (height encloses)");
        mini_doc_destroy(d);
    }

    /* ---- Multicol: column-count: 2 ---- */
    {
        MiniDocument *d = mini_doc_create();
        struct MiniNode *mc = mkdiv(d->body, "div");
        mini_style_set(mc, "column-count", "2");
        mini_style_set(mc, "width", "400px");
        for (int i = 0; i < 4; i++)
        {
            struct MiniNode *c = mkdiv(mc, "div");
            mini_style_set(c, "height", "40px");
        }
        mini_layout_run(d, 800, 600);
        /* column width ≈ (400)/2 = 200 (no gap) */
        struct MiniNode *k = mc->first_child;
        layck(k && lclose(k->style.w, 200), "multicol column width = 200");
        /* 2nd-column child's x is offset to the right of the 1st column */
        struct MiniNode *c1 = k ? k->next_sibling : NULL;
        struct MiniNode *c2 = c1 ? c1->next_sibling : NULL;
        if (c2)
            layck(c2->style.abs_x > c1->style.abs_x, "multicol 2nd column offset right");
        mini_doc_destroy(d);
    }

    fprintf(stderr, lay_fail ? "LAYOUT_SELFTEST: %d FAIL\n" : "LAYOUT_SELFTEST: all PASS\n", lay_fail);
    return lay_fail ? 1 : 0;
}
#endif

/* ================================================================== */
/* Test Stubs for Standalone Self-Test Harness                        */
/* ================================================================== */
#if defined(FULL_SUITE_SELFTEST) || defined(ARENA_SELFTEST) || defined(NESTING_SELFTEST) || defined(AT_RULES_SELFTEST) || defined(WHERE_SPEC_SELFTEST) || defined(FORM_PSEUDO_SELFTEST) || defined(MARGIN_COLLAPSE_SELFTEST) || defined(FLEX_ORDER_SELFTEST) || defined(DYNAMIC_GRID_SELFTEST) || defined(STICKY_SELFTEST) || defined(SVG_FULL_SELFTEST) || defined(FILTER_SELFTEST) || defined(PARSE_SELFTEST) || defined(COLOR_SELFTEST) || defined(BOX_SELFTEST) || defined(CSS_SELFTEST) || defined(DOM_CSS_SELFTEST) || defined(SELECTOR_SELFTEST) || defined(ENTITY_SELFTEST) || defined(POSITION_SELFTEST) || defined(LAYOUT_SELFTEST)

void mini_renderer_push_clip(MiniRenderer *r, float x, float y, float w, float h)
{
    (void)r;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}
void mini_renderer_pop_clip(MiniRenderer *r) { (void)r; }
void mini_renderer_push_rounded_clip_corners(MiniRenderer *r, float x, float y, float w, float h, const float rad[4])
{
    (void)r;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)rad;
}
void mini_draw_rect(MiniRenderer *r, float x, float y, float w, float h, float cr, float cg, float cb, float ca)
{
    (void)r;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)cr;
    (void)cg;
    (void)cb;
    (void)ca;
}
void mini_draw_rect_stroke(MiniRenderer *r, float x, float y, float w, float h, float lw, float cr, float cg, float cb, float ca)
{
    (void)r;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)lw;
    (void)cr;
    (void)cg;
    (void)cb;
    (void)ca;
}
void mini_draw_rect_rounded_corners(MiniRenderer *r, float x, float y, float w, float h, const float rad[4], float cr, float cg, float cb, float ca)
{
    (void)r;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)rad;
    (void)cr;
    (void)cg;
    (void)cb;
    (void)ca;
}
void mini_draw_rect_rounded_corners_stroke(MiniRenderer *r, float x, float y, float w, float h, const float rad[4], float lw, float cr, float cg, float cb, float ca)
{
    (void)r;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)rad;
    (void)lw;
    (void)cr;
    (void)cg;
    (void)cb;
    (void)ca;
}
void mini_draw_line(MiniRenderer *r, float x1, float y1, float x2, float y2, float lw, float cr, float cg, float cb, float ca)
{
    (void)r;
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    (void)lw;
    (void)cr;
    (void)cg;
    (void)cb;
    (void)ca;
}
void mini_draw_triangle(MiniRenderer *r, float x1, float y1, float x2, float y2, float x3, float y3, float cr, float cg, float cb, float ca)
{
    (void)r;
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    (void)x3;
    (void)y3;
    (void)cr;
    (void)cg;
    (void)cb;
    (void)ca;
}
void mini_draw_circle(MiniRenderer *r, float cx, float cy, float radius, float cr, float cg, float cb, float ca)
{
    (void)r;
    (void)cx;
    (void)cy;
    (void)radius;
    (void)cr;
    (void)cg;
    (void)cb;
    (void)ca;
}
void mini_draw_polygon(MiniRenderer *r, const float *pts, int num_pts, float cr, float cg, float cb, float ca)
{
    (void)r;
    (void)pts;
    (void)num_pts;
    (void)cr;
    (void)cg;
    (void)cb;
    (void)ca;
}
void mini_draw_polygon_stroke(MiniRenderer *r, const float *pts, int num_pts, float lw, float cr, float cg, float cb, float ca)
{
    (void)r;
    (void)pts;
    (void)num_pts;
    (void)lw;
    (void)cr;
    (void)cg;
    (void)cb;
    (void)ca;
}
void mini_draw_shadow_corners(MiniRenderer *r, float x, float y, float w, float h, const float radii[4], float spread, float blur, float cr, float cg, float cb, float ca)
{
    (void)r;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)radii;
    (void)spread;
    (void)blur;
    (void)cr;
    (void)cg;
    (void)cb;
    (void)ca;
}
void mini_draw_background_image(MiniRenderer *r, float x, float y, float w, float h, const char *url, int size_mode, float bg_w, float bg_h, float pos_x, float pos_y, int repeat, const float radii[4])
{
    (void)r;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)url;
    (void)size_mode;
    (void)bg_w;
    (void)bg_h;
    (void)pos_x;
    (void)pos_y;
    (void)repeat;
    (void)radii;
}
void mini_draw_gradient(MiniRenderer *r, float x, float y, float w, float h, float r1, float g1, float b1, float a1, float r2, float g2, float b2, float a2, int type, float angle)
{
    (void)r;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)r1;
    (void)g1;
    (void)b1;
    (void)a1;
    (void)r2;
    (void)g2;
    (void)b2;
    (void)a2;
    (void)type;
    (void)angle;
}
void mini_draw_gradient_ex(MiniRenderer *r, float x, float y, float w, float h, float r1, float g1, float b1, float a1, float r2, float g2, float b2, float a2, int type, float angle, float radius)
{
    (void)r;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)r1;
    (void)g1;
    (void)b1;
    (void)a1;
    (void)r2;
    (void)g2;
    (void)b2;
    (void)a2;
    (void)type;
    (void)angle;
    (void)radius;
}
void mini_draw_gradient_multi(MiniRenderer *r, float x, float y, float w, float h, const void *stops, int num_stops, int type, float angle, const float radii[4])
{
    (void)r;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)stops;
    (void)num_stops;
    (void)type;
    (void)angle;
    (void)radii;
}
void mini_draw_push_xform_full(MiniRenderer *r, float ox, float oy, float tx, float ty, float tz, float sx, float sy, float rx, float ry, float rz, float kx, float ky, float p)
{
    (void)r;
    (void)ox;
    (void)oy;
    (void)tx;
    (void)ty;
    (void)tz;
    (void)sx;
    (void)sy;
    (void)rx;
    (void)ry;
    (void)rz;
    (void)kx;
    (void)ky;
    (void)p;
}
void mini_draw_pop_xform(MiniRenderer *r) { (void)r; }
void mini_draw_text(MiniRenderer *r, float x, float y, const char *text, float font_size, float cr, float cg, float cb, float ca)
{
    (void)r;
    (void)x;
    (void)y;
    (void)text;
    (void)font_size;
    (void)cr;
    (void)cg;
    (void)cb;
    (void)ca;
}
void mini_draw_text_styled(MiniRenderer *r, float x, float y, const char *text, float font_size, float cr, float cg, float cb, float ca, float letter_spacing, int font_flags)
{
    (void)r;
    (void)x;
    (void)y;
    (void)text;
    (void)font_size;
    (void)cr;
    (void)cg;
    (void)cb;
    (void)ca;
    (void)letter_spacing;
    (void)font_flags;
}
float mini_text_measure(const char *text, float font_size) { return text ? (float)strlen(text) * font_size * 0.6f : 0.0f; }
float mini_text_measure_ex(const char *text, float font_size, float letter_spacing)
{
    (void)letter_spacing;
    return text ? (float)strlen(text) * font_size * 0.6f : 0.0f;
}
float mini_text_line_height(float font_size) { return font_size * 1.2f; }
int mini_events_node_selection_range(MiniEventState *st, const struct MiniNode *n, int *start, int *end)
{
    (void)st;
    (void)n;
    (void)start;
    (void)end;
    return 0;
}
int mini_net_http(const char *method, const char *url, const char **req_headers, int req_h_n, const uint8_t *req_body, size_t req_body_sz, int *out_status, uint8_t **out_body, size_t *out_body_sz)
{
    (void)method;
    (void)url;
    (void)req_headers;
    (void)req_h_n;
    (void)req_body;
    (void)req_body_sz;
    (void)out_status;
    (void)out_body;
    (void)out_body_sz;
    return -1;
}
int mini_renderer_add_font_data(void *r, const char *family, const uint8_t *data, size_t size)
{
    (void)r;
    (void)family;
    (void)data;
    (void)size;
    return 0;
}

#endif

/* ================================================================== */
/* New Industry-Grade Self-Tests                                      */
/* ================================================================== */

static int run_arena_selftest(void)
{
    int fail = 0;
    MiniArena arena;
    mini_arena_init(&arena, 1024);
    void *p1 = mini_arena_alloc(&arena, 128);
    if (!p1)
    {
        fprintf(stderr, "ARENA FAIL: alloc 128 failed\n");
        fail++;
    }
    char *s = mini_arena_strdup(&arena, "Hello Industrial Micro-Engine");
    if (!s || strcmp(s, "Hello Industrial Micro-Engine") != 0)
    {
        fprintf(stderr, "ARENA FAIL: strdup failed\n");
        fail++;
    }
    mini_arena_reset(&arena);
    void *p2 = mini_arena_alloc(&arena, 256);
    if (!p2)
    {
        fprintf(stderr, "ARENA FAIL: alloc after reset failed\n");
        fail++;
    }
    mini_arena_destroy(&arena);
    fprintf(stderr, fail ? "ARENA_SELFTEST: %d FAIL\n" : "ARENA_SELFTEST: all PASS\n", fail);
    return fail;
}

static int run_nesting_selftest(void)
{
    int fail = 0;
    MiniDocument *d = mini_doc_create();
    struct MiniNode *p = mini_node_create_element("div");
    mini_node_set_attribute(p, "class", "parent");
    mini_node_append_child(d->body, p);
    struct MiniNode *c = mini_node_create_element("div");
    mini_node_set_attribute(c, "class", "child");
    mini_node_append_child(p, c);

    const char *nested_css = ".parent { color: rgb(255, 0, 0); & .child { color: rgb(0, 0, 255); } }";
    mini_css_apply(d, nested_css);

    if (fabsf(p->style.color_r - 1.0f) > 0.01f || fabsf(p->style.color_b - 0.0f) > 0.01f)
    {
        fprintf(stderr, "NESTING FAIL: parent color not red\n");
        fail++;
    }
    if (fabsf(c->style.color_b - 1.0f) > 0.01f || fabsf(c->style.color_r - 0.0f) > 0.01f)
    {
        fprintf(stderr, "NESTING FAIL: child color not blue\n");
        fail++;
    }
    mini_doc_destroy(d);
    fprintf(stderr, fail ? "NESTING_SELFTEST: %d FAIL\n" : "NESTING_SELFTEST: all PASS\n", fail);
    return fail;
}

static int run_at_rules_selftest(void)
{
    int fail = 0;
    MiniDocument *d = mini_doc_create();
    struct MiniNode *n1 = mini_node_create_element("div");
    mini_node_set_attribute(n1, "class", "supported");
    mini_node_append_child(d->body, n1);

    struct MiniNode *n2 = mini_node_create_element("div");
    mini_node_set_attribute(n2, "class", "unsupported");
    mini_node_append_child(d->body, n2);

    const char *css = "@supports (display: flex) { .supported { display: flex; } }\n"
                      "@supports (non-existent-prop-xyz: 123) { .unsupported { display: none; } }";
    mini_css_apply(d, css);

    if (n1->style.display != MINI_DISPLAY_FLEX)
    {
        fprintf(stderr, "AT_RULES FAIL: @supports (display: flex) did not apply\n");
        fail++;
    }
    if (n2->style.display == MINI_DISPLAY_NONE)
    {
        fprintf(stderr, "AT_RULES FAIL: @supports invalid prop incorrectly applied\n");
        fail++;
    }
    mini_doc_destroy(d);
    fprintf(stderr, fail ? "AT_RULES_SELFTEST: %d FAIL\n" : "AT_RULES_SELFTEST: all PASS\n", fail);
    return fail;
}

static int run_where_spec_selftest(void)
{
    int fail = 0;
    MiniDocument *d = mini_doc_create();
    struct MiniNode *btn = mini_node_create_element("button");
    mini_node_set_attribute(btn, "class", "btn");
    mini_node_append_child(d->body, btn);

    const char *css = "button { color: rgb(255, 0, 0); }\n:where(.btn) { color: rgb(0, 255, 0); }";
    mini_css_apply(d, css);

    if (fabsf(btn->style.color_r - 1.0f) > 0.01f || fabsf(btn->style.color_g - 0.0f) > 0.01f)
    {
        fprintf(stderr, "WHERE_SPEC FAIL: :where() specificity not zero, got r=%f g=%f\n", btn->style.color_r, btn->style.color_g);
        fail++;
    }
    mini_doc_destroy(d);
    fprintf(stderr, fail ? "WHERE_SPEC_SELFTEST: %d FAIL\n" : "WHERE_SPEC_SELFTEST: all PASS\n", fail);
    return fail;
}

static int run_form_pseudo_selftest(void)
{
    int fail = 0;
    MiniDocument *d = mini_doc_create();
    struct MiniNode *in1 = mini_node_create_element("input");
    mini_node_set_attribute(in1, "type", "checkbox");
    mini_node_set_attribute(in1, "checked", "checked");
    mini_node_append_child(d->body, in1);

    struct MiniNode *in2 = mini_node_create_element("input");
    mini_node_set_attribute(in2, "type", "text");
    mini_node_set_attribute(in2, "disabled", "disabled");
    mini_node_append_child(d->body, in2);

    struct MiniNode *in3 = mini_node_create_element("input");
    mini_node_set_attribute(in3, "type", "text");
    mini_node_set_attribute(in3, "required", "required");
    mini_node_append_child(d->body, in3);

    const char *css = ":checked { opacity: 0.5; }\n:disabled { opacity: 0.2; }\n:required { opacity: 0.8; }";
    mini_css_apply(d, css);

    if (fabsf(in1->style.opacity - 0.5f) > 0.01f)
    {
        fprintf(stderr, "FORM_PSEUDO FAIL: :checked did not match\n");
        fail++;
    }
    if (fabsf(in2->style.opacity - 0.2f) > 0.01f)
    {
        fprintf(stderr, "FORM_PSEUDO FAIL: :disabled did not match\n");
        fail++;
    }
    if (fabsf(in3->style.opacity - 0.8f) > 0.01f)
    {
        fprintf(stderr, "FORM_PSEUDO FAIL: :required did not match\n");
        fail++;
    }
    mini_doc_destroy(d);
    fprintf(stderr, fail ? "FORM_PSEUDO_SELFTEST: %d FAIL\n" : "FORM_PSEUDO_SELFTEST: all PASS\n", fail);
    return fail;
}

static int run_margin_collapse_selftest(void)
{
    int fail = 0;
    MiniDocument *d = mini_doc_create();
    struct MiniNode *b1 = mini_node_create_element("div");
    mini_style_set(b1, "height", "50px");
    mini_style_set(b1, "margin-bottom", "30px");
    mini_node_append_child(d->body, b1);

    struct MiniNode *b2 = mini_node_create_element("div");
    mini_style_set(b2, "height", "50px");
    mini_style_set(b2, "margin-top", "20px");
    mini_node_append_child(d->body, b2);

    mini_layout_run(d, 800, 600);

    float dist = b2->style.abs_y - b1->style.abs_y;
    if (fabsf(dist - 80.0f) > 1.5f)
    {
        fprintf(stderr, "MARGIN_COLLAPSE FAIL: expected distance 80px, got %f\n", dist);
        fail++;
    }
    mini_doc_destroy(d);
    fprintf(stderr, fail ? "MARGIN_COLLAPSE_SELFTEST: %d FAIL\n" : "MARGIN_COLLAPSE_SELFTEST: all PASS\n", fail);
    return fail;
}

static int run_flex_order_selftest(void)
{
    int fail = 0;
    MiniDocument *d = mini_doc_create();
    struct MiniNode *flex = mini_node_create_element("div");
    mini_style_set(flex, "display", "flex");
    mini_style_set(flex, "width", "600px");
    mini_node_append_child(d->body, flex);

    struct MiniNode *i1 = mini_node_create_element("div");
    mini_style_set(i1, "order", "3");
    mini_style_set(i1, "width", "100px");
    mini_style_set(i1, "height", "50px");
    mini_node_append_child(flex, i1);

    struct MiniNode *i2 = mini_node_create_element("div");
    mini_style_set(i2, "order", "1");
    mini_style_set(i2, "width", "100px");
    mini_style_set(i2, "height", "50px");
    mini_node_append_child(flex, i2);

    struct MiniNode *i3 = mini_node_create_element("div");
    mini_style_set(i3, "order", "2");
    mini_style_set(i3, "width", "100px");
    mini_style_set(i3, "height", "50px");
    mini_node_append_child(flex, i3);

    mini_layout_run(d, 600, 400);

    if (!(i2->style.abs_x < i3->style.abs_x && i3->style.abs_x < i1->style.abs_x))
    {
        fprintf(stderr, "FLEX_ORDER FAIL: items not sorted by order (i2=%f, i3=%f, i1=%f)\n",
                i2->style.abs_x, i3->style.abs_x, i1->style.abs_x);
        fail++;
    }
    mini_doc_destroy(d);
    fprintf(stderr, fail ? "FLEX_ORDER_SELFTEST: %d FAIL\n" : "FLEX_ORDER_SELFTEST: all PASS\n", fail);
    return fail;
}

static int run_dynamic_grid_selftest(void)
{
    int fail = 0;
    MiniDocument *d = mini_doc_create();
    struct MiniNode *grid = mini_node_create_element("div");
    mini_style_set(grid, "display", "grid");
    mini_style_set(grid, "grid-template-columns", "repeat(20, 1fr)");
    mini_style_set(grid, "width", "1000px");
    mini_node_append_child(d->body, grid);

    for (int i = 0; i < 20; i++)
    {
        struct MiniNode *item = mini_node_create_element("div");
        mini_style_set(item, "height", "40px");
        mini_node_append_child(grid, item);
    }
    mini_layout_run(d, 1000, 600);

    struct MiniNode *k = grid->first_child;
    if (!k || fabsf(k->style.w - 50.0f) > 1.5f)
    {
        fprintf(stderr, "DYNAMIC_GRID FAIL: 20-col track width expected 50, got %f\n", k ? k->style.w : 0.0f);
        fail++;
    }
    mini_doc_destroy(d);
    fprintf(stderr, fail ? "DYNAMIC_GRID_SELFTEST: %d FAIL\n" : "DYNAMIC_GRID_SELFTEST: all PASS\n", fail);
    return fail;
}

static int run_sticky_selftest(void)
{
    int fail = 0;
    MiniDocument *d = mini_doc_create();
    struct MiniNode *container = mini_node_create_element("div");
    mini_style_set(container, "width", "400px");
    mini_style_set(container, "height", "500px");
    mini_node_append_child(d->body, container);

    struct MiniNode *sticky = mini_node_create_element("div");
    mini_style_set(sticky, "position", "sticky");
    mini_style_set(sticky, "top", "10px");
    mini_style_set(sticky, "height", "40px");
    mini_node_append_child(container, sticky);

    d->scroll_y = 100.0f;
    mini_layout_run(d, 800, 600);

    if (fabsf(sticky->style.abs_y - 110.0f) > 1.5f)
    {
        fprintf(stderr, "STICKY FAIL: expected sticky abs_y 110px, got %f\n", sticky->style.abs_y);
        fail++;
    }
    mini_doc_destroy(d);
    fprintf(stderr, fail ? "STICKY_SELFTEST: %d FAIL\n" : "STICKY_SELFTEST: all PASS\n", fail);
    return fail;
}

static int run_svg_full_selftest(void)
{
    int fail = 0;
    MiniDocument *d = mini_doc_create();
    g_active_doc = d;
    struct MiniNode *svg = mini_node_create_element("svg");
    mini_node_set_attribute(svg, "viewBox", "0 0 200 200");
    mini_node_set_attribute(svg, "width", "200");
    mini_node_set_attribute(svg, "height", "200");
    mini_node_append_child(d->body, svg);

    struct MiniNode *defs = mini_node_create_element("defs");
    mini_node_append_child(svg, defs);

    struct MiniNode *grad = mini_node_create_element("linearGradient");
    mini_node_set_attribute(grad, "id", "grad1");
    mini_node_append_child(defs, grad);

    struct MiniNode *s1 = mini_node_create_element("stop");
    mini_node_set_attribute(s1, "offset", "0%");
    mini_node_set_attribute(s1, "stop-color", "#ff0000");
    mini_node_append_child(grad, s1);

    struct MiniNode *rect = mini_node_create_element("rect");
    mini_node_set_attribute(rect, "width", "100");
    mini_node_set_attribute(rect, "height", "100");
    mini_node_set_attribute(rect, "fill", "url(#grad1)");
    mini_node_append_child(svg, rect);

    struct MiniNode *txt = mini_node_create_element("text");
    mini_node_set_attribute(txt, "x", "50");
    mini_node_set_attribute(txt, "y", "50");
    mini_node_set_attribute(txt, "text-anchor", "middle");
    mini_node_set_text(txt, "Test SVG");
    mini_node_append_child(svg, txt);

    svg_parse_defs(svg);

    MiniDocumentContext *ctx = mini_get_ctx(d);
    if (!ctx->svg_defs.gradients || strcmp(ctx->svg_defs.gradients->id, "grad1") != 0)
    {
        fprintf(stderr, "SVG_FULL FAIL: gradient not registered in defs\n");
        fail++;
    }
    float fr = 0, fg = 0, fb = 0, fa = 1;
    if (!svg_paint(rect, "url(#grad1)", &fr, &fg, &fb, &fa, 1) || fabsf(fr - 1.0f) > 0.01f)
    {
        fprintf(stderr, "SVG_FULL FAIL: url(#grad1) paint resolve failed\n");
        fail++;
    }

    mini_doc_destroy(d);
    g_active_doc = NULL;
    fprintf(stderr, fail ? "SVG_FULL_SELFTEST: %d FAIL\n" : "SVG_FULL_SELFTEST: all PASS\n", fail);
    return fail;
}

static int run_filter_selftest(void)
{
    int fail = 0;
    MiniDocument *d = mini_doc_create();
    struct MiniNode *n = mini_node_create_element("div");
    mini_node_append_child(d->body, n);

    mini_style_set(n, "filter", "invert(75%) grayscale(40%) blur(5px) brightness(1.3)");

    if (!n->style.has_filter)
    {
        fprintf(stderr, "FILTER FAIL: has_filter flag not set\n");
        fail++;
    }
    if (fabsf(n->style.filter_invert - 0.75f) > 0.01f)
    {
        fprintf(stderr, "FILTER FAIL: invert expected 0.75, got %f\n", n->style.filter_invert);
        fail++;
    }
    if (fabsf(n->style.filter_grayscale - 0.40f) > 0.01f)
    {
        fprintf(stderr, "FILTER FAIL: grayscale expected 0.40, got %f\n", n->style.filter_grayscale);
        fail++;
    }
    if (fabsf(n->style.filter_blur - 5.0f) > 0.01f)
    {
        fprintf(stderr, "FILTER FAIL: blur expected 5.0, got %f\n", n->style.filter_blur);
        fail++;
    }
    if (fabsf(n->style.filter_brightness - 1.3f) > 0.01f)
    {
        fprintf(stderr, "FILTER FAIL: brightness expected 1.3, got %f\n", n->style.filter_brightness);
        fail++;
    }
    mini_doc_destroy(d);
    fprintf(stderr, fail ? "FILTER_SELFTEST: %d FAIL\n" : "FILTER_SELFTEST: all PASS\n", fail);
    return fail;
}

#ifdef ARENA_SELFTEST
int main(void) { return run_arena_selftest(); }
#endif

#ifdef NESTING_SELFTEST
int main(void) { return run_nesting_selftest(); }
#endif

#ifdef AT_RULES_SELFTEST
int main(void) { return run_at_rules_selftest(); }
#endif

#ifdef WHERE_SPEC_SELFTEST
int main(void) { return run_where_spec_selftest(); }
#endif

#ifdef FORM_PSEUDO_SELFTEST
int main(void) { return run_form_pseudo_selftest(); }
#endif

#ifdef MARGIN_COLLAPSE_SELFTEST
int main(void) { return run_margin_collapse_selftest(); }
#endif

#ifdef FLEX_ORDER_SELFTEST
int main(void) { return run_flex_order_selftest(); }
#endif

#ifdef DYNAMIC_GRID_SELFTEST
int main(void) { return run_dynamic_grid_selftest(); }
#endif

#ifdef STICKY_SELFTEST
int main(void) { return run_sticky_selftest(); }
#endif

#ifdef SVG_FULL_SELFTEST
int main(void) { return run_svg_full_selftest(); }
#endif

#ifdef FILTER_SELFTEST
int main(void) { return run_filter_selftest(); }
#endif

#ifdef FULL_SUITE_SELFTEST
int main(void)
{
    int total_fail = 0;
    fprintf(stderr, "\n=======================================================\n");
    fprintf(stderr, "    STARTING FULL INDUSTRY-GRADE DOM TEST SUITE\n");
    fprintf(stderr, "=======================================================\n\n");

    total_fail += run_arena_selftest();
    total_fail += run_nesting_selftest();
    total_fail += run_at_rules_selftest();
    total_fail += run_where_spec_selftest();
    total_fail += run_form_pseudo_selftest();
    total_fail += run_margin_collapse_selftest();
    total_fail += run_flex_order_selftest();
    total_fail += run_dynamic_grid_selftest();
    total_fail += run_sticky_selftest();
    total_fail += run_svg_full_selftest();
    total_fail += run_filter_selftest();

    fprintf(stderr, "\n=======================================================\n");
    if (total_fail == 0)
    {
        fprintf(stderr, "  >>> FULL SUITE RESULT: ALL TEST SUITES PASSED! (0 FAILURES) <<<\n");
    }
    else
    {
        fprintf(stderr, "  >>> FULL SUITE RESULT: %d TEST FAILURES DETECTED! <<<\n", total_fail);
    }
    fprintf(stderr, "=======================================================\n\n");
    return total_fail ? 1 : 0;
}
#endif
