/*
 * mini_html5.c — WHATWG HTML5 parsing pipeline (tokenizer + tree construction).
 *
 * Implements, from scratch and faithful to the WHATWG HTML Standard:
 *   • spec tokenizer state machine (data / tag-open / tag-name / attribute
 *     states / comment / doctype / RAWTEXT / RCDATA / character-reference)
 *   • tree construction with insertion modes + the open-elements stack and
 *     the active-formatting-elements list
 *   • the Adoption Agency Algorithm for mis-nested formatting elements
 *   • implicit end-tag generation (auto-close) for p / li / dd / dt / option /
 *     tr / td / th / tbody / tfoot / thead / caption / colgroup / html / head
 *   • table foster-parenting (mis-placed table-internal nodes are fostered
 *     to the location just before the <table>)
 *   • streaming: the parser is fed chunks; partial tokens at a chunk boundary
 *     are carried over so the DOM is built incrementally as bytes arrive.
 *
 * Builds directly into a MiniDocument via the mini_node_* tree API so it
 * composes with the existing layout/render/CDP pipeline (no separate IR).
 *
 * Reference: https://html.spec.whatwg.org/ — §13.2.5 (tokenizer), §13.2.6
 * (tree construction), §13.2.6.4 (adoption agency).
 */
#include "mini_html5.h"
#include "mini_dom.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ================================================================== */
/* Element classification (spec "special" / "formatting" / void / text) */
/* ================================================================== */
/* The spec's "special" set — elements that close <p> and serve as the
   "furthest block" / scope boundaries in the adoption-agency algorithm.  */
static int h5_is_special(const char *t)
{
    static const char *S[] = {
        "address", "applet", "area", "article", "aside", "base", "basefont", "bgsound",
        "blockquote", "body", "br", "button", "caption", "center", "col", "colgroup",
        "dd", "details", "dir", "div", "dl", "dt", "embed", "fieldset", "figcaption",
        "figure", "footer", "form", "frame", "frameset", "h1", "h2", "h3", "h4", "h5",
        "h6", "head", "header", "hgroup", "hr", "html", "iframe", "img", "input",
        "isindex", "li", "link", "listing", "main", "marquee", "menu", "meta", "nav",
        "noembed", "noframes", "noscript", "object", "ol", "p", "param", "plaintext",
        "pre", "script", "section", "select", "style", "summary", "table", "tbody",
        "td", "template", "textarea", "tfoot", "th", "thead", "title", "tr", "ul", "wbr",
        "xmp", NULL};
    if (!t)
        return 0;
    for (int i = 0; S[i]; i++)
        if (!strcmp(t, S[i]))
            return 1;
    return 0;
}
/* The adoption-agency formatting set (§13.2.6.4). */
static int h5_is_formatting(const char *t)
{
    static const char *F[] = {"a", "b", "big", "code", "em", "font", "i", "nobr", "s",
                              "small", "strike", "strong", "tt", "u", NULL};
    if (!t)
        return 0;
    for (int i = 0; F[i]; i++)
        if (!strcmp(t, F[i]))
            return 1;
    return 0;
}
/* RCDATA elements: text + entity decoding (title, textarea). */
static int h5_is_rcdata(const char *t)
{
    return t && (!strcmp(t, "title") || !strcmp(t, "textarea"));
}
/* RAWTEXT elements: verbatim text, no entity decode (script/style/xmp/...). */
static int h5_is_rawtext(const char *t)
{
    static const char *R[] = {"style", "script", "xmp", "iframe", "noembed",
                              "noframes", "noscript", "plaintext", NULL};
    if (!t)
        return 0;
    for (int i = 0; R[i]; i++)
        if (!strcmp(t, R[i]))
            return 1;
    return 0;
}

/* Case-insensitive ASCII equal. */
static int h5_ci(const char *a, const char *b)
{
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

/* ---- compact named + numeric character-reference decoder ---------------
   Resolves &amp; &lt; &gt; &quot; &apos; &nbsp; + common symbols + numeric
   (&#65; &#x41;). Writes UTF-8 into `out`, returns #bytes written. `*in` is
   advanced past the consumed reference. On an unknown/incomplete ref, leaves
   the '&' literally and advances one byte. */
static const struct
{
    const char *n;
    const char *u;
} H5_ENT[] = {
    {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", " "},
    {"ldquo", "\xE2\x80\x9C"}, {"rdquo", "\xE2\x80\x9D"}, {"lsquo", "\xE2\x80\x98"}, {"rsquo", "\xE2\x80\x99"},
    {"sbquo", "\xE2\x80\x9A"}, {"bdquo", "\xE2\x80\x9E"}, {"prime", "\xE2\x80\xB2"}, {"Prime", "\xE2\x80\xB3"},
    {"mdash", "\xE2\x80\x94"}, {"ndash", "\xE2\x80\x93"}, {"hellip", "\xE2\x80\xA6"},
    {"copy", "\xC2\xA9"}, {"reg", "\xC2\xAE"}, {"trade", "\xE2\x84\xA2"},
    {"deg", "\xC2\xB0"}, {"plusmn", "\xC2\xB1"}, {"times", "\xC3\x97"}, {"divide", "\xC3\xB7"},
    {"micro", "\xC2\xB5"}, {"para", "\xC2\xB6"}, {"sect", "\xC2\xA7"}, {"middot", "\xC2\xB7"},
    {"bull", "\xE2\x80\xA2"}, {"euro", "\xE2\x82\xAC"}, {"pound", "\xC2\xA3"}, {"cent", "\xC2\xA2"},
    {"yen", "\xC2\xA5"}, {"darr", "\xE2\x86\x93"}, {"uarr", "\xE2\x86\x91"}, {"larr", "\xE2\x86\x90"},
    {"rarr", "\xE2\x86\x92"}, {"harr", "\xE2\x86\x94"}, {"lArr", "\xE2\x87\x90"}, {"uArr", "\xE2\x87\x91"},
    {"rArr", "\xE2\x87\x92"}, {"dArr", "\xE2\x87\x93"}, {"hArr", "\xE2\x87\x94"},
    {"dagger", "\xE2\x80\xA0"}, {"Dagger", "\xE2\x80\xA1"}, {"laquo", "\xC2\xAB"}, {"raquo", "\xC2\xBB"},
    {"ensp", "\xE2\x80\x82"}, {"emsp", "\xE2\x80\x83"}, {"thinsp", "\xE2\x80\x89"},
    {"iexcl", "\xC2\xA1"}, {"iquest", "\xC2\xBF"}, {"frac12", "\xC2\xBD"}, {"frac14", "\xC2\xBC"},
    {"frac34", "\xC2\xBE"}, {"sup2", "\xC2\xB2"}, {"sup3", "\xC2\xB3"}, {"szlig", "\xC3\x9F"},
    {"minus", "\xE2\x88\x92"}, {"infin", "\xE2\x88\x9E"}, {"radic", "\xE2\x88\x9A"},
    {"ne", "\xE2\x89\xA0"}, {"equiv", "\xE2\x89\xA1"}, {"le", "\xE2\x89\xA4"}, {"ge", "\xE2\x89\xA5"},
    {"sum", "\xE2\x88\x91"}, {"prod", "\xE2\x88\x8F"}, {"int", "\xE2\x88\xAB"},
    {"spades", "\xE2\x99\xA0"}, {"clubs", "\xE2\x99\xA3"}, {"hearts", "\xE2\x99\xA5"}, {"diams", "\xE2\x99\xA6"},
    {"check", "\xE2\x9C\x93"},
    {"agrave", "\xC3\xA0"}, {"eacute", "\xC3\xA9"}, {"egrave", "\xC3\xA8"}, {"ccedil", "\xC3\xA7"},
    {"uuml", "\xC3\xBC"}, {"ouml", "\xC3\xB6"}, {"auml", "\xC3\xA4"},
    {"aacute", "\xC3\xA1"}, {"iacute", "\xC3\xAD"}, {"oacute", "\xC3\xB3"}, {"uacute", "\xC3\xBA"},
    {"ntilde", "\xC3\xB1"}, {NULL, NULL}};

/* Encode a single Unicode codepoint as UTF-8 into out (<=4 bytes). */
static int h5_utf8(unsigned cp, char *out)
{
    if (cp < 0x80)
    {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800)
    {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000)
    {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Try to decode a character reference beginning at `s[0]` ('&'). On success
   writes UTF-8 to out and returns bytes consumed (incl. trailing ';'); on
   failure returns 0 (caller emits the literal '&'). */
static size_t h5_decode_ref(const char *s, size_t len, char *out, size_t cap)
{
    if (!len || s[0] != '&')
        return 0;
    size_t i = 1;
    if (i < len && s[i] == '#')
    { /* numeric reference */
        i++;
        int hex = 0, val = 0;
        if (i < len && (s[i] == 'x' || s[i] == 'X'))
        {
            hex = 1;
            i++;
        }
        size_t start = i;
        int digits = 0;
        while (i < len)
        {
            char c = s[i];
            int d = (c >= '0' && c <= '9') ? c - '0' : (hex && c >= 'a' && c <= 'f') ? c - 'a' + 10
                                                   : (hex && c >= 'A' && c <= 'F')   ? c - 'A' + 10
                                                                                     : -1;
            if (d < 0)
                break;
            val = val * 16 + d;
            digits++;
            i++;
        }
        if (!digits)
            return 0;
        if (i < len && s[i] == ';')
            i++;
        else if (i == len)
            return 0; /* incomplete */
        if (val == 0)
            val = 0xFFFD;
        else if (val > 0x10FFFF)
            val = 0xFFFD;
        {
            int w = h5_utf8((unsigned)val, out);
            out[w] = 0;
        }
        return i;
    }
    /* named reference: letters/digits up to ';' */
    size_t start = i;
    while (i < len)
    {
        char c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            i++;
        else
            break;
    }
    size_t nl = i - start;
    int semi = (i < len && s[i] == ';');
    if (nl == 0)
        return 0;
    if (!semi && i == len)
        return 0; /* possibly incomplete: defer */
    char name[24];
    if (nl >= sizeof(name))
        nl = sizeof(name) - 1;
    memcpy(name, s + start, nl);
    name[nl] = 0;
    for (int k = 0; H5_ENT[k].n; k++)
        if (!strcmp(name, H5_ENT[k].n))
        {
            const char *u = H5_ENT[k].u;
            size_t w = 0;
            while (u[w] && w + 1 < cap)
            {
                out[w] = u[w];
                w++;
            }
            out[w] = 0;
            return semi ? i + 1 : i;
        }
    (void)start;
    return 0;
}

/* ================================================================== */
/* Tokenizer                                                          */
/* ================================================================== */
typedef enum
{
    TS_DATA,
    TS_TAG_OPEN,
    TS_END_TAG_OPEN,
    TS_TAG_NAME,
    TS_BEFORE_ATTR_NAME,
    TS_ATTR_NAME,
    TS_AFTER_ATTR_NAME,
    TS_BEFORE_ATTR_VALUE,
    TS_ATTR_VALUE_DQ,
    TS_ATTR_VALUE_SQ,
    TS_ATTR_VALUE_UQ,
    TS_AFTER_ATTR_VALUE,
    TS_SELF_CLOSING,
    TS_BOGUS_COMMENT,
    TS_MARKUP_DECL_OPEN,
    TS_COMMENT,
    TS_DOCTYPE,
    TS_RAWTEXT,
    TS_RCDATA,
    TS_EOF
} H5TokState;

typedef enum
{
    TT_START,
    TT_END,
    TT_CHAR,
    TT_COMMENT,
    TT_DOCTYPE,
    TT_EOF
} H5TokType;

typedef struct
{
    H5TokType type;
    char *name; /* tag/comment/doctype name (malloc) */
    struct
    {
        char *n;
        char *v;
    } *attrs;
    int n_attr, cap_attr;
    int is_self_closing;
} H5Token;

/* ================================================================== */
/* Tree-construction state                                            */
/* ================================================================== */
typedef enum
{
    M_INITIAL,
    M_BEFORE_HTML,
    M_BEFORE_HEAD,
    M_IN_HEAD,
    M_AFTER_HEAD,
    M_IN_BODY,
    M_IN_TABLE,
    M_IN_TABLE_BODY,
    M_IN_ROW,
    M_IN_CELL,
    M_IN_SELECT,
    M_AFTER_BODY,
    M_AFTER_AFTER_BODY
} H5Mode;

typedef struct
{
    int marker;
    struct MiniNode *node;
} AFEEntry;

struct MiniHtml5Parser
{
    struct MiniDocument *doc;
    struct MiniNode **open;
    int open_n, open_cap; /* open-elements stack */
    AFEEntry *afe;
    int afe_n, afe_cap; /* active formatting elts */
    H5Mode mode;
    struct MiniNode *head_element;      /* the <head> */
    struct MiniNode *form_element;      /* the <form> (AAA ctx) */
    struct MiniNode *table_foster;      /* current table for foster-parenting */
    struct MiniNode *pending_text_tail; /* last text node, for coalescing */
    int fragment;                       /* setInnerHTML context */

    H5TokState ts;
    char *acc;
    int acc_n, acc_cap; /* token accumulator */
    char *carry;
    int carry_n;           /* partial-token carryover across feeds */
    H5Token cur;           /* current tag token being built */
    int reconsume;         /* 1 = re-feed carry next */
    int rawtext_is_rcdata; /* distinguishes RCDATA vs RAWTEXT */
    int eof;
};

static void set_last_attr_value(H5Token *t, const char *v);
static void pop_until_one(struct MiniHtml5Parser *p, const char *a,
                          const char *b, const char *c);
static void maybe_enter_rawtext(struct MiniHtml5Parser *p);
static void process_token(struct MiniHtml5Parser *p, H5Token *t);
static void emit_current(struct MiniHtml5Parser *p);
static void emit_char(struct MiniHtml5Parser *p, const char *s);

/* ---- dynamic arrays ---- */
static int ptr_push(struct MiniNode ***arr, int *n, int *cap, struct MiniNode *v)
{
    if (*n == *cap)
    {
        int nc = *cap ? *cap * 2 : 16;
        struct MiniNode **na = realloc(*arr, nc * sizeof(void *));
        if (!na)
            return -1;
        *arr = na;
        *cap = nc;
    }
    (*arr)[(*n)++] = v;
    return 0;
}
static struct MiniNode *ptr_pop(struct MiniNode **arr, int *n)
{
    if (*n == 0)
        return NULL;
    return arr[--(*n)];
}

/* ---- string accumulator ---- */
static void acc_putc(struct MiniHtml5Parser *p, char c)
{
    if (p->acc_n + 2 > p->acc_cap)
    {
        int nc = p->acc_cap ? p->acc_cap * 2 : 64;
        char *na = realloc(p->acc, nc);
        if (!na)
            return;
        p->acc = na;
        p->acc_cap = nc;
    }
    p->acc[p->acc_n++] = c;
    p->acc[p->acc_n] = 0;
}
static void acc_puts(struct MiniHtml5Parser *p, const char *s)
{
    if (s)
        while (*s)
            acc_putc(p, *s++);
}
static void acc_reset(struct MiniHtml5Parser *p)
{
    p->acc_n = 0;
    if (p->acc)
        p->acc[0] = 0;
}
static char *acc_dup(struct MiniHtml5Parser *p)
{
    if (p->acc_n == 0)
        return strdup("");
    char *s = malloc(p->acc_n + 1);
    if (!s)
        return NULL;
    memcpy(s, p->acc, p->acc_n);
    s[p->acc_n] = 0;
    return s;
}

/* ---- token attr helpers ---- */
static void tok_set_name(H5Token *t, const char *s)
{
    free(t->name);
    t->name = strdup(s ? s : "");
}
static void tok_add_attr(H5Token *t, const char *name, const char *val)
{
    if (t->n_attr == t->cap_attr)
    {
        int nc = t->cap_attr ? t->cap_attr * 2 : 8;
        void *na = realloc(t->attrs, nc * sizeof(t->attrs[0]));
        if (!na)
            return;
        t->attrs = na;
        t->cap_attr = nc;
    }
    t->attrs[t->n_attr].n = strdup(name ? name : "");
    t->attrs[t->n_attr].v = strdup(val ? val : "");
    t->n_attr++;
}
static void tok_reset(H5Token *t)
{
    free(t->name);
    t->name = NULL;
    for (int i = 0; i < t->n_attr; i++)
    {
        free(t->attrs[i].n);
        free(t->attrs[i].v);
    }
    free(t->attrs);
    t->attrs = NULL;
    t->n_attr = t->cap_attr = 0;
    t->is_self_closing = 0;
    t->type = TT_START;
}

/* ---- open-elements stack helpers ---- */
static struct MiniNode *current_node(struct MiniHtml5Parser *p) { return p->open_n ? p->open[p->open_n - 1] : p->doc->root; }
static int open_index(struct MiniHtml5Parser *p, struct MiniNode *n)
{
    for (int i = p->open_n - 1; i >= 0; i--)
        if (p->open[i] == n)
            return i;
    return -1;
}
static void open_push(struct MiniHtml5Parser *p, struct MiniNode *n) { ptr_push(&p->open, &p->open_n, &p->open_cap, n); }
static struct MiniNode *open_pop(struct MiniHtml5Parser *p) { return ptr_pop(p->open, &p->open_n); }

static int pop_until(struct MiniHtml5Parser *p, const char *tag)
{
    int found = 0;
    for (int i = p->open_n - 1; i >= 0; i--)
        if (p->open[i]->tag && h5_ci(p->open[i]->tag, tag))
        {
            found = 1;
            break;
        }
    if (!found)
        return 0;
    while (p->open_n)
    {
        struct MiniNode *n = open_pop(p);
        if (n->tag && h5_ci(n->tag, tag))
            break;
    }
    return 1;
}

static int has_in_scope(struct MiniHtml5Parser *p, const char *tag, int table_scope)
{
    static const char *DEFAULT_SCOPE[] = {"applet", "caption", "html", "table", "td", "th", "marquee", "object", "template", NULL};
    static const char *TABLE_SCOPE[] = {"html", "table", NULL};
    const char **b = table_scope ? TABLE_SCOPE : DEFAULT_SCOPE;
    for (int i = p->open_n - 1; i >= 0; i--)
    {
        struct MiniNode *n = p->open[i];
        if (n->tag && h5_ci(n->tag, tag))
            return 1;
        if (n->tag)
        {
            for (int j = 0; b[j]; j++)
                if (h5_ci(n->tag, b[j]))
                    return 0;
        }
    }
    return 0;
}

static void open_remove(struct MiniHtml5Parser *p, struct MiniNode *n)
{
    int idx = open_index(p, n);
    if (idx < 0)
        return;
    for (int i = idx; i < p->open_n - 1; i++)
        p->open[i] = p->open[i + 1];
    p->open_n--;
}

/* ---- active-formatting-elements list ---- */
static void afe_push_typed(struct MiniHtml5Parser *p, int marker, struct MiniNode *node)
{
    if (p->afe_n == p->afe_cap)
    {
        int nc = p->afe_cap ? p->afe_cap * 2 : 16;
        AFEEntry *na = realloc(p->afe, nc * sizeof(AFEEntry));
        if (!na)
            return;
        p->afe = na;
        p->afe_cap = nc;
    }
    p->afe[p->afe_n].marker = marker;
    p->afe[p->afe_n].node = node;
    p->afe_n++;
}
static void afe_marker(struct MiniHtml5Parser *p) { afe_push_typed(p, 1, NULL); }
static int afe_index(struct MiniHtml5Parser *p, struct MiniNode *n)
{
    for (int i = p->afe_n - 1; i >= 0; i--)
        if (!p->afe[i].marker && p->afe[i].node == n)
            return i;
    return -1;
}
static void afe_remove(struct MiniHtml5Parser *p, struct MiniNode *n)
{
    int idx = afe_index(p, n);
    if (idx < 0)
        return;
    for (int i = idx; i < p->afe_n - 1; i++)
        p->afe[i] = p->afe[i + 1];
    p->afe_n--;
}

/* ================================================================== */
/* Insertion: create element, set attrs, attach to current node       */
/* ================================================================== */
static struct MiniNode *create_element_for_token(struct MiniHtml5Parser *p, H5Token *t)
{
    struct MiniNode *n = mini_node_create_element(t->name ? t->name : "div");
    if (!n)
        return NULL;
    for (int i = 0; i < t->n_attr; i++)
        mini_node_set_attribute(n, t->attrs[i].n, t->attrs[i].v);
    return n;
}

static struct MiniNode *insert_element(struct MiniHtml5Parser *p, struct MiniNode *parent, struct MiniNode *n)
{
    mini_node_append_child(parent, n);
    open_push(p, n);
    p->pending_text_tail = NULL;
    return n;
}

static void foster_insert(struct MiniHtml5Parser *p, struct MiniNode *n)
{
    struct MiniNode *table = p->table_foster;
    struct MiniNode *parent = table ? table->parent : current_node(p);
    if (table && parent)
    {
        if (mini_node_insert_before(parent, n, table) == 0)
        {
            open_push(p, n);
            p->pending_text_tail = NULL;
            return;
        }
    }
    insert_element(p, current_node(p), n);
}

/* Insert a character (text).
   关键修复：对于 RAWTEXT / RCDATA 节点（style, script, title, textarea），
   直接将其文本写入 cur->text，确保 CSS/JS 引擎能够读取 node->text 执行！ */
static void insert_char(struct MiniHtml5Parser *p, const char *utf8)
{
    if (!utf8 || !*utf8)
        return;
    struct MiniNode *cur = current_node(p);
    struct MiniNode *parent = cur;

    /* Rawtext / RCDATA 节点（style, script, title, textarea）直接追加到 cur->text */
    if (cur && cur->type == MN_ELEMENT_NODE && cur->tag &&
        (h5_is_rawtext(cur->tag) || h5_is_rcdata(cur->tag)))
    {
        size_t ol = cur->text ? strlen(cur->text) : 0;
        size_t nl = strlen(utf8);
        char *nb = (char *)realloc(cur->text, ol + nl + 1);
        if (nb)
        {
            cur->text = nb;
            memcpy(cur->text + ol, utf8, nl);
            cur->text[ol + nl] = 0;
        }
        return;
    }

    if ((p->mode == M_IN_TABLE || p->mode == M_IN_TABLE_BODY || p->mode == M_IN_ROW ||
         p->mode == M_IN_CELL) &&
        p->table_foster)
    {
        if (!*utf8 || (*utf8 == ' ' && !utf8[1]) || (*utf8 == '\t' && !utf8[1]) ||
            (*utf8 == '\n' && !utf8[1]) || (*utf8 == '\r' && !utf8[1]) || (*utf8 == '\f' && !utf8[1]))
            return;
        parent = p->table_foster->parent ? p->table_foster->parent : cur;
        struct MiniNode *tn = NULL;
        if (p->table_foster->prev_sibling && p->table_foster->prev_sibling->type == MN_TEXT_NODE)
            tn = p->table_foster->prev_sibling;
        if (tn)
        {
            size_t ol = strlen(tn->text), nl = strlen(utf8);
            char *nb = realloc(tn->text, ol + nl + 1);
            if (nb)
            {
                tn->text = nb;
                memcpy(tn->text + ol, utf8, nl);
                tn->text[ol + nl] = 0;
            }
        }
        else
        {
            tn = mini_node_create_text(utf8);
            mini_node_insert_before(parent, tn, p->table_foster);
            p->pending_text_tail = tn;
        }
        return;
    }

    struct MiniNode *lc = parent ? parent->last_child : NULL;
    if (p->pending_text_tail && lc == p->pending_text_tail && lc && lc->type == MN_TEXT_NODE)
    {
        size_t ol = strlen(lc->text), nl = strlen(utf8);
        char *nb = realloc(lc->text, ol + nl + 1);
        if (nb)
        {
            lc->text = nb;
            memcpy(lc->text + ol, utf8, nl);
            lc->text[ol + nl] = 0;
        }
    }
    else
    {
        struct MiniNode *tn = mini_node_create_text(utf8);
        mini_node_append_child(parent, tn);
        p->pending_text_tail = tn;
    }
}

static void insert_comment(struct MiniHtml5Parser *p, struct MiniNode *parent, const char *data)
{
    struct MiniNode *c = mini_node_create_comment(data ? data : "");
    if (c)
        mini_node_append_child(parent, c);
    p->pending_text_tail = NULL;
}

static int is_implied_end(const char *t)
{
    static const char *I[] = {"dd", "dt", "li", "option", "optgroup", "p", "rb", "rp", "rt", "rtc", "thead", "tbody", "tfoot", "tr", "td", "th", NULL};
    if (!t)
        return 0;
    for (int i = 0; I[i]; i++)
        if (h5_ci(t, I[i]))
            return 1;
    return 0;
}

static void gen_implied_end(struct MiniHtml5Parser *p, const char *except)
{
    while (p->open_n)
    {
        struct MiniNode *n = current_node(p);
        const char *t = n->tag;
        if (!t || !is_implied_end(t))
            break;
        if (except && h5_ci(t, except))
            break;
        open_pop(p);
    }
}

static int h5_closes_p(const char *t)
{
    static const char *C[] = {
        "address", "article", "aside", "blockquote", "center", "details", "dialog",
        "dir", "div", "dl", "fieldset", "figcaption", "figure", "footer", "form",
        "h1", "h2", "h3", "h4", "h5", "h6", "header", "hgroup", "hr", "main", "nav", "ol",
        "p", "pre", "section", "table", "ul", "listing", "summary", NULL};
    if (!t)
        return 0;
    for (int i = 0; C[i]; i++)
        if (h5_ci(t, C[i]))
            return 1;
    return 0;
}

static void close_p_if_open(struct MiniHtml5Parser *p)
{
    gen_implied_end(p, NULL);
    if (has_in_scope(p, "p", 0))
        pop_until(p, "p");
}

static void reconstruct_active_formatting(struct MiniHtml5Parser *p)
{
    if (p->afe_n == 0)
        return;
    int entry = p->afe_n - 1;
    AFEEntry *e = &p->afe[entry];
    if (e->marker)
        return;
    if (e->node && open_index(p, e->node) >= 0)
        return;
    int idx = entry;
    while (idx >= 0)
    {
        if (p->afe[idx].marker)
            break;
        if (p->afe[idx].node && open_index(p, p->afe[idx].node) >= 0)
            break;
        idx--;
    }
    for (int i = idx + 1; i < p->afe_n; i++)
    {
        struct MiniNode *orig = p->afe[i].node;
        if (!orig || p->afe[i].marker)
            continue;
        struct MiniNode *clone = mini_node_clone(orig, 0);
        if (!clone)
            continue;
        mini_node_append_child(current_node(p), clone);
        open_push(p, clone);
        p->afe[i].node = clone;
    }
}

static void adoption_agency(struct MiniHtml5Parser *p, const char *subject)
{
    int fmt_idx = -1;
    for (int i = p->afe_n - 1; i >= 0; i--)
    {
        if (p->afe[i].marker)
            break;
        struct MiniNode *fn = p->afe[i].node;
        if (fn && fn->tag && h5_ci(fn->tag, subject) && open_index(p, fn) >= 0)
        {
            fmt_idx = i;
            break;
        }
    }
    if (fmt_idx < 0)
    {
        if (has_in_scope(p, subject, 0))
        {
            gen_implied_end(p, NULL);
            while (p->open_n)
            {
                struct MiniNode *n = open_pop(p);
                if (n->tag && h5_ci(n->tag, subject))
                    break;
            }
        }
        return;
    }
    struct MiniNode *fmt = p->afe[fmt_idx].node;
    int fi = open_index(p, fmt);
    if (fi < 0)
    {
        afe_remove(p, fmt);
        return;
    }
    struct MiniNode *furthest = NULL;
    int furthest_i = -1;
    for (int i = p->open_n - 1; i > fi; i--)
    {
        struct MiniNode *n = p->open[i];
        if (n->tag && h5_is_special(n->tag) && !h5_is_formatting(n->tag))
        {
            furthest = n;
            furthest_i = i;
            break;
        }
    }
    if (!furthest)
    {
        while (p->open_n)
        {
            struct MiniNode *n = open_pop(p);
            if (n == fmt)
                break;
        }
        afe_remove(p, fmt);
        return;
    }
    struct MiniNode *common_ancestor = (fi > 0) ? p->open[fi - 1] : p->doc->root;
    struct MiniNode *node = furthest;
    struct MiniNode *last_node = furthest;
    int node_i = furthest_i;
    int guard = 0;
    while (guard++ < 256)
    {
        if (node_i <= 0)
            break;
        node_i--;
        node = p->open[node_i];
        if (node == fmt)
            break;
        int ai = afe_index(p, node);
        if (ai >= 0)
        {
            struct MiniNode *clone = mini_node_clone(node, 0);
            if (!clone)
                break;
            p->afe[ai].node = clone;
            p->open[node_i] = clone;
            mini_node_append_child(last_node, clone);
            struct MiniNode *c = node->first_child;
            while (c)
            {
                struct MiniNode *nx = c->next_sibling;
                mini_node_remove_child(node, c);
                mini_node_append_child(clone, c);
                c = nx;
            }
            last_node = clone;
        }
        else
        {
            open_remove(p, node);
        }
    }
    if (last_node)
    {
        if (last_node->parent && last_node->parent != common_ancestor)
            mini_node_remove_child(last_node->parent, last_node);
        if (last_node->parent != common_ancestor)
            mini_node_append_child(common_ancestor, last_node);
    }
    open_remove(p, fmt);
    afe_remove(p, fmt);
    {
        struct MiniNode *newfmt = mini_node_clone(fmt, 0);
        if (newfmt)
        {
            struct MiniNode *c = last_node ? last_node->first_child : NULL;
            while (c)
            {
                struct MiniNode *nx = c->next_sibling;
                mini_node_remove_child(last_node, c);
                mini_node_append_child(newfmt, c);
                c = nx;
            }
            if (last_node)
                mini_node_append_child(last_node, newfmt);
            open_push(p, newfmt);
            afe_push_typed(p, 0, newfmt);
        }
    }
}

/* ================================================================== */
/* Insertion-mode dispatch                                            */
/* ================================================================== */
static void ensure_html(struct MiniHtml5Parser *p)
{
    struct MiniNode *html = NULL;
    for (struct MiniNode *c = p->doc->root->first_child; c; c = c->next_sibling)
        if (c->tag && h5_ci(c->tag, "html"))
        {
            html = c;
            break;
        }
    if (html)
        return;
    html = mini_node_create_element("html");
    struct MiniNode *body = p->doc->body;
    if (body && body->parent == p->doc->root)
        mini_node_remove_child(p->doc->root, body);
    mini_node_append_child(p->doc->root, html);
    p->head_element = mini_node_create_element("head");
    mini_node_append_child(html, p->head_element);
    if (body)
        mini_node_append_child(html, body);
    else
    {
        p->doc->body = mini_node_create_element("body");
        mini_node_append_child(html, p->doc->body);
    }
    open_push(p, html);
}

static void in_body_start(struct MiniHtml5Parser *p, H5Token *t)
{
    const char *tag = t->name;
    if (h5_ci(tag, "html"))
        return;
    if (h5_ci(tag, "head"))
        return;
    if (h5_ci(tag, "base") || h5_ci(tag, "basefont") || h5_ci(tag, "bgsound") ||
        h5_ci(tag, "link") || h5_ci(tag, "meta") || h5_ci(tag, "title"))
    {
        struct MiniNode *n = create_element_for_token(p, t);
        if (n)
        {
            mini_node_append_child(p->head_element ? p->head_element : current_node(p), n);
            if (!mini_element_info(tag)->is_void)
                open_push(p, n);
        }
        return;
    }
    if (h5_ci(tag, "style") || h5_ci(tag, "script"))
    {
        struct MiniNode *n = create_element_for_token(p, t);
        if (n)
        {
            mini_node_append_child(current_node(p), n);
            open_push(p, n);
        }
        return;
    }
    if (h5_ci(tag, "body"))
        return;
    if (h5_closes_p(tag))
        close_p_if_open(p);
    if (h5_ci(tag, "li"))
        gen_implied_end(p, "li");
    if (h5_ci(tag, "dd") || h5_ci(tag, "dt"))
        gen_implied_end(p, NULL);
    if (h5_ci(tag, "table"))
    {
        struct MiniNode *n = create_element_for_token(p, t);
        if (n)
        {
            mini_node_append_child(current_node(p), n);
            open_push(p, n);
            p->table_foster = n;
            p->mode = M_IN_TABLE;
        }
        return;
    }
    if (h5_is_formatting(tag))
    {
        reconstruct_active_formatting(p);
        struct MiniNode *n = create_element_for_token(p, t);
        if (n)
        {
            insert_element(p, current_node(p), n);
            afe_push_typed(p, 0, n);
        }
        return;
    }
    reconstruct_active_formatting(p);
    struct MiniNode *n = create_element_for_token(p, t);
    if (n)
    {
        if (t->is_self_closing || mini_element_info(tag)->is_void)
        {
            mini_node_append_child(current_node(p), n);
        }
        else
        {
            insert_element(p, current_node(p), n);
        }
    }
}

static void in_body_end(struct MiniHtml5Parser *p, H5Token *t)
{
    const char *tag = t->name;
    if (h5_is_formatting(tag))
    {
        adoption_agency(p, tag);
        return;
    }
    if (h5_ci(tag, "p"))
    {
        gen_implied_end(p, "p");
        if (has_in_scope(p, "p", 0))
            pop_until(p, "p");
        return;
    }
    if (h5_ci(tag, "li") || h5_ci(tag, "dd") || h5_ci(tag, "dt"))
    {
        gen_implied_end(p, tag);
        if (has_in_scope(p, tag, 0))
            pop_until(p, tag);
        return;
    }
    if (h5_ci(tag, "body"))
    {
        p->mode = M_AFTER_BODY;
        return;
    }
    if (h5_ci(tag, "html"))
    {
        p->mode = M_AFTER_BODY;
        return;
    }
    if (has_in_scope(p, tag, 0))
    {
        gen_implied_end(p, NULL);
        while (p->open_n)
        {
            struct MiniNode *n = open_pop(p);
            if (n->tag && h5_ci(n->tag, tag))
                break;
        }
    }
}

static void in_table_start(struct MiniHtml5Parser *p, H5Token *t)
{
    const char *tag = t->name;
    if (h5_ci(tag, "caption"))
    {
        pop_until(p, "table");
        struct MiniNode *n = create_element_for_token(p, t);
        if (n)
        {
            mini_node_append_child(current_node(p), n);
            open_push(p, n);
            afe_marker(p);
            p->mode = M_IN_BODY;
        }
        return;
    }
    if (h5_ci(tag, "colgroup"))
    {
        struct MiniNode *n = create_element_for_token(p, t);
        if (n)
        {
            mini_node_append_child(current_node(p), n);
            open_push(p, n);
        }
        return;
    }
    if (h5_ci(tag, "col"))
    {
        struct MiniNode *n = create_element_for_token(p, t);
        if (n)
        {
            mini_node_append_child(current_node(p), n);
        }
        return;
    }
    if (h5_ci(tag, "thead") || h5_ci(tag, "tfoot") || h5_ci(tag, "tbody"))
    {
        struct MiniNode *n = create_element_for_token(p, t);
        if (n)
        {
            mini_node_append_child(current_node(p), n);
            open_push(p, n);
            p->mode = M_IN_TABLE_BODY;
        }
        return;
    }
    if (h5_ci(tag, "tr"))
    {
        struct MiniNode *n = create_element_for_token(p, t);
        if (n)
        {
            mini_node_append_child(current_node(p), n);
            open_push(p, n);
            p->mode = M_IN_ROW;
        }
        return;
    }
    if (h5_ci(tag, "th") || h5_ci(tag, "td"))
    {
        struct MiniNode *tr = mini_node_create_element("tr");
        mini_node_append_child(current_node(p), tr);
        open_push(p, tr);
        p->mode = M_IN_ROW;
        struct MiniNode *c = create_element_for_token(p, t);
        if (c)
        {
            mini_node_append_child(current_node(p), c);
            open_push(p, c);
            p->mode = M_IN_CELL;
        }
        return;
    }
    reconstruct_active_formatting(p);
    struct MiniNode *n = create_element_for_token(p, t);
    if (n)
    {
        if (t->is_self_closing || mini_element_info(tag)->is_void)
        {
            struct MiniNode *table = p->table_foster;
            struct MiniNode *parent = table ? table->parent : current_node(p);
            if (table && parent)
                mini_node_insert_before(parent, n, table);
            else
                mini_node_append_child(parent, n);
        }
        else
        {
            foster_insert(p, n);
        }
    }
}

static void in_table_end(struct MiniHtml5Parser *p, H5Token *t)
{
    const char *tag = t->name;
    if (h5_ci(tag, "table"))
    {
        if (has_in_scope(p, "table", 1))
        {
            pop_until(p, "table");
            p->table_foster = NULL;
            p->mode = M_IN_BODY;
        }
        return;
    }
    if (h5_ci(tag, "tbody") || h5_ci(tag, "tfoot") || h5_ci(tag, "thead"))
    {
        if (has_in_scope(p, tag, 1))
            pop_until(p, tag), p->mode = M_IN_TABLE;
        return;
    }
    if (h5_ci(tag, "tr"))
    {
        if (has_in_scope(p, tag, 1))
            pop_until(p, tag), p->mode = M_IN_TABLE_BODY;
        return;
    }
    if (h5_ci(tag, "th") || h5_ci(tag, "td"))
    {
        if (has_in_scope(p, tag, 1))
            pop_until(p, tag), p->mode = M_IN_ROW;
        return;
    }
    in_body_end(p, t);
}

static void in_row_start(struct MiniHtml5Parser *p, H5Token *t)
{
    const char *tag = t->name;
    if (h5_ci(tag, "th") || h5_ci(tag, "td"))
    {
        struct MiniNode *n = create_element_for_token(p, t);
        if (n)
        {
            mini_node_append_child(current_node(p), n);
            open_push(p, n);
            p->mode = M_IN_CELL;
        }
        return;
    }
    if (h5_ci(tag, "tr"))
    {
        if (has_in_scope(p, "tr", 1))
            pop_until(p, "tr");
        struct MiniNode *n = create_element_for_token(p, t);
        if (n)
        {
            mini_node_append_child(current_node(p), n);
            open_push(p, n);
        }
        return;
    }
    if (has_in_scope(p, "tr", 1))
        pop_until(p, "tr");
    p->mode = M_IN_TABLE_BODY;
    in_table_start(p, t);
}

static void in_cell_start(struct MiniHtml5Parser *p, H5Token *t)
{
    in_body_start(p, t);
}

static void in_cell_end(struct MiniHtml5Parser *p, H5Token *t)
{
    const char *tag = t->name;
    if (h5_ci(tag, "td") || h5_ci(tag, "th"))
    {
        if (has_in_scope(p, tag, 1))
        {
            gen_implied_end(p, NULL);
            pop_until(p, tag);
            p->mode = M_IN_ROW;
        }
        return;
    }
    if (h5_ci(tag, "tr") || h5_ci(tag, "tbody") || h5_ci(tag, "thead") || h5_ci(tag, "tfoot"))
    {
        if (has_in_scope(p, "td", 1) || has_in_scope(p, "th", 1))
        {
            gen_implied_end(p, NULL);
            pop_until(p, "td");
            pop_until(p, "th");
            p->mode = M_IN_ROW;
            in_table_end(p, t);
        }
        return;
    }
    in_body_end(p, t);
}

static void process_token(struct MiniHtml5Parser *p, H5Token *t)
{
    switch (p->mode)
    {
    case M_INITIAL:
    case M_BEFORE_HTML:
        ensure_html(p);
        p->mode = M_BEFORE_HEAD;
        if (t->type == TT_CHAR)
        {
            insert_char(p, t->name ? t->name : "");
            return;
        }
        p->mode = M_IN_BODY;
        if (t->type == TT_START && (h5_ci(t->name, "head")))
        {
            struct MiniNode *h = create_element_for_token(p, t);
            if (h)
            {
                mini_node_append_child(current_node(p), h);
                open_push(p, h);
                p->head_element = h;
                p->mode = M_IN_HEAD;
            }
            return;
        }
        break;
    case M_BEFORE_HEAD:
        if (t->type == TT_START && h5_ci(t->name, "head"))
        {
            struct MiniNode *h = create_element_for_token(p, t);
            if (h)
            {
                mini_node_append_child(current_node(p), h);
                open_push(p, h);
                p->head_element = h;
                p->mode = M_IN_HEAD;
            }
            return;
        }
        {
            struct MiniNode *h = mini_node_create_element("head");
            mini_node_append_child(current_node(p), h);
            open_push(p, h);
            p->head_element = h;
            p->mode = M_IN_HEAD;
            process_token(p, t);
            return;
        }
    case M_IN_HEAD:
        if (t->type == TT_START)
        {
            const char *tag = t->name;
            if (h5_ci(tag, "title") || h5_ci(tag, "style") || h5_ci(tag, "script"))
            {
                struct MiniNode *n = create_element_for_token(p, t);
                if (n)
                {
                    mini_node_append_child(current_node(p), n);
                    open_push(p, n);
                }
                return;
            }
            if (h5_ci(tag, "meta") || h5_ci(tag, "link") || h5_ci(tag, "base"))
            {
                struct MiniNode *n = create_element_for_token(p, t);
                if (n)
                    mini_node_append_child(current_node(p), n);
                return;
            }
            open_pop(p);
            p->mode = M_AFTER_HEAD;
            process_token(p, t);
            return;
        }
        if (t->type == TT_END)
        {
            const char *tag = t->name;
            if (h5_ci(tag, "head"))
            {
                open_pop(p);
                p->mode = M_AFTER_HEAD;
                return;
            }
            if (h5_ci(tag, "title") || h5_ci(tag, "style") || h5_ci(tag, "script"))
            {
                if (current_node(p)->tag && h5_ci(current_node(p)->tag, tag))
                {
                    open_pop(p);
                }
                return;
            }
            if (h5_ci(tag, "body") || h5_ci(tag, "html"))
            {
                open_pop(p);
                p->mode = M_AFTER_HEAD;
                process_token(p, t);
                return;
            }
            return;
        }
        if (t->type == TT_CHAR)
        {
            insert_char(p, t->name ? t->name : "");
            return;
        }
        if (t->type == TT_COMMENT)
        {
            insert_comment(p, current_node(p), t->name ? t->name : "");
            return;
        }
        open_pop(p);
        p->mode = M_AFTER_HEAD;
        process_token(p, t);
        return;
    case M_AFTER_HEAD:
        if (t->type == TT_START && h5_ci(t->name, "body"))
        {
            struct MiniNode *b = create_element_for_token(p, t);
            if (b)
            {
                mini_node_append_child(current_node(p), b);
                open_push(p, b);
                p->doc->body = b;
                p->mode = M_IN_BODY;
            }
            return;
        }
        if (t->type == TT_START && h5_ci(t->name, "head"))
        {
            return;
        }
        {
            struct MiniNode *b = mini_node_create_element("body");
            mini_node_append_child(current_node(p), b);
            open_push(p, b);
            p->doc->body = b;
            p->mode = M_IN_BODY;
            process_token(p, t);
            return;
        }
    case M_IN_BODY:
        if (t->type == TT_CHAR)
        {
            insert_char(p, t->name ? t->name : "");
            return;
        }
        if (t->type == TT_COMMENT)
        {
            insert_comment(p, current_node(p), t->name ? t->name : "");
            return;
        }
        if (t->type == TT_START)
        {
            in_body_start(p, t);
            return;
        }
        if (t->type == TT_END)
        {
            in_body_end(p, t);
            return;
        }
        return;
    case M_IN_TABLE:
        if (t->type == TT_START)
        {
            in_table_start(p, t);
            return;
        }
        if (t->type == TT_END)
        {
            in_table_end(p, t);
            return;
        }
        if (t->type == TT_CHAR)
        {
            insert_char(p, t->name ? t->name : "");
            return;
        }
        if (t->type == TT_COMMENT)
        {
            insert_comment(p, current_node(p), t->name ? t->name : "");
            return;
        }
        return;
    case M_IN_TABLE_BODY:
        if (t->type == TT_START && (h5_ci(t->name, "tr")))
        {
            struct MiniNode *n = create_element_for_token(p, t);
            if (n)
            {
                mini_node_append_child(current_node(p), n);
                open_push(p, n);
                p->mode = M_IN_ROW;
            }
            return;
        }
        if (t->type == TT_END && (h5_ci(t->name, "tbody") || h5_ci(t->name, "thead") || h5_ci(t->name, "tfoot")))
        {
            if (has_in_scope(p, t->name, 1))
                pop_until(p, t->name), p->mode = M_IN_TABLE;
            return;
        }
        if (t->type == TT_START)
        {
            if (has_in_scope(p, "tbody", 1) || has_in_scope(p, "thead", 1) || has_in_scope(p, "tfoot", 1))
                pop_until_one(p, "tbody", "thead", "tfoot");
            p->mode = M_IN_TABLE;
            in_table_start(p, t);
            return;
        }
        return;
    case M_IN_ROW:
        if (t->type == TT_START)
        {
            in_row_start(p, t);
            return;
        }
        if (t->type == TT_END)
        {
            in_table_end(p, t);
            return;
        }
        if (t->type == TT_CHAR)
        {
            insert_char(p, t->name ? t->name : "");
            return;
        }
        return;
    case M_IN_CELL:
        if (t->type == TT_START)
        {
            in_cell_start(p, t);
            return;
        }
        if (t->type == TT_END)
        {
            in_cell_end(p, t);
            return;
        }
        if (t->type == TT_CHAR)
        {
            insert_char(p, t->name ? t->name : "");
            return;
        }
        if (t->type == TT_COMMENT)
        {
            insert_comment(p, current_node(p), t->name ? t->name : "");
            return;
        }
        return;
    case M_IN_SELECT:
        if (t->type == TT_CHAR)
        {
            insert_char(p, t->name ? t->name : "");
            return;
        }
        if (t->type == TT_START)
        {
            in_body_start(p, t);
            return;
        }
        if (t->type == TT_END)
        {
            in_body_end(p, t);
            return;
        }
        return;
    case M_AFTER_BODY:
        if (t->type == TT_END && h5_ci(t->name, "html"))
        {
            p->mode = M_AFTER_AFTER_BODY;
            return;
        }
        p->mode = M_IN_BODY;
        process_token(p, t);
        return;
    case M_AFTER_AFTER_BODY:
        if (t->type == TT_CHAR)
        {
            insert_char(p, t->name ? t->name : "");
            return;
        }
        p->mode = M_IN_BODY;
        process_token(p, t);
        return;
    }
}

static void pop_until_one(struct MiniHtml5Parser *p, const char *a, const char *b, const char *c)
{
    const char *tags[4] = {a, b, c, NULL};
    for (int i = p->open_n - 1; i >= 0; i--)
    {
        struct MiniNode *n = p->open[i];
        if (n->tag)
        {
            for (int j = 0; j < 3 && tags[j]; j++)
                if (h5_ci(n->tag, tags[j]))
                {
                    while (p->open_n)
                    {
                        struct MiniNode *x = open_pop(p);
                        if (x == n)
                            break;
                    }
                    return;
                }
        }
    }
}

/* ================================================================== */
/* Tokenizer                                                          */
/* ================================================================== */
static void emit_char(struct MiniHtml5Parser *p, const char *s)
{
    H5Token t;
    memset(&t, 0, sizeof(t));
    t.type = TT_CHAR;
    t.name = strdup(s ? s : "");
    process_token(p, &t);
    free(t.name);
}

static void emit_ref(struct MiniHtml5Parser *p, const char *utf8)
{
    H5Token t;
    memset(&t, 0, sizeof(t));
    t.type = TT_CHAR;
    t.name = strdup(utf8 ? utf8 : "");
    process_token(p, &t);
    free(t.name);
}

static void emit_current(struct MiniHtml5Parser *p)
{
    if (p->cur.type == TT_COMMENT || p->cur.type == TT_DOCTYPE)
    {
        if (!p->cur.name)
            p->cur.name = strdup("");
        process_token(p, &p->cur);
        tok_reset(&p->cur);
        return;
    }
    if (!p->cur.name || !p->cur.name[0])
    {
        tok_reset(&p->cur);
        return;
    }
    int was_start = (p->cur.type == TT_START);
    for (char *q = p->cur.name; *q; q++)
        *q = (char)tolower((unsigned char)*q);
    process_token(p, &p->cur);
    tok_reset(&p->cur);
    if (was_start)
        maybe_enter_rawtext(p);
}

static void attr_finalize(struct MiniHtml5Parser *p)
{
    if (p->cur.n_attr > 0)
    {
        if (!p->cur.attrs[p->cur.n_attr - 1].v[0] && p->acc_n > 0)
        {
            free(p->cur.attrs[p->cur.n_attr - 1].v);
            p->cur.attrs[p->cur.n_attr - 1].v = acc_dup(p);
        }
    }
    acc_reset(p);
}

static void tokenize_run(struct MiniHtml5Parser *p, const char *buf, size_t len)
{
    size_t i = 0;
    while (i < len)
    {
        char c = buf[i];
        switch (p->ts)
        {
        case TS_DATA:
            if (c == '&')
            {
                char out[8] = {0};
                size_t used = h5_decode_ref(buf + i, len - i, out, sizeof(out));
                if (used > 0)
                {
                    emit_ref(p, out[0] ? out : "");
                    i += used;
                    continue;
                }
                emit_char(p, "&");
                i++;
                continue;
            }
            if (c == '<')
            {
                p->ts = TS_TAG_OPEN;
                i++;
                continue;
            }
            {
                size_t start = i;
                while (i < len && buf[i] != '<' && buf[i] != '&')
                    i++;
                size_t text_len = i - start;
                if (text_len > 0)
                {
                    char *chunk = malloc(text_len + 1);
                    if (chunk)
                    {
                        memcpy(chunk, buf + start, text_len);
                        chunk[text_len] = '\0';
                        emit_char(p, chunk);
                        free(chunk);
                    }
                }
                continue;
            }
        case TS_TAG_OPEN:
            if (c == '!')
            {
                p->ts = TS_MARKUP_DECL_OPEN;
                i++;
                continue;
            }
            if (c == '/')
            {
                p->ts = TS_END_TAG_OPEN;
                i++;
                continue;
            }
            if (isalpha((unsigned char)c))
            {
                p->cur.type = TT_START;
                acc_reset(p);
                acc_putc(p, c);
                p->ts = TS_TAG_NAME;
                i++;
                continue;
            }
            if (c == '?')
            {
                p->ts = TS_BOGUS_COMMENT;
                acc_reset(p);
                i++;
                continue;
            }
            emit_char(p, "<");
            p->ts = TS_DATA;
            continue;
        case TS_END_TAG_OPEN:
            if (isalpha((unsigned char)c))
            {
                p->cur.type = TT_END;
                acc_reset(p);
                acc_putc(p, c);
                p->ts = TS_TAG_NAME;
                i++;
                continue;
            }
            if (c == '>')
            {
                p->ts = TS_DATA;
                i++;
                continue;
            }
            p->ts = TS_BOGUS_COMMENT;
            acc_reset(p);
            continue;
        case TS_TAG_NAME:
        {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f')
            {
                tok_set_name(&p->cur, p->acc_n ? p->acc : "");
                acc_reset(p);
                p->ts = TS_BEFORE_ATTR_NAME;
                i++;
                continue;
            }
            if (c == '/')
            {
                tok_set_name(&p->cur, p->acc_n ? p->acc : "");
                acc_reset(p);
                p->ts = TS_SELF_CLOSING;
                i++;
                continue;
            }
            if (c == '>')
            {
                tok_set_name(&p->cur, p->acc_n ? p->acc : "");
                acc_reset(p);
                p->ts = TS_DATA;
                emit_current(p);
                i++;
                continue;
            }
            acc_putc(p, c);
            i++;
            continue;
        }
        case TS_BEFORE_ATTR_NAME:
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f')
            {
                i++;
                continue;
            }
            if (c == '/' || c == '>')
            {
                p->ts = TS_AFTER_ATTR_NAME;
                continue;
            }
            acc_reset(p);
            acc_putc(p, c);
            p->ts = TS_ATTR_NAME;
            i++;
            continue;
        case TS_ATTR_NAME:
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f')
            {
                tok_add_attr(&p->cur, p->acc_n ? p->acc : "", "");
                acc_reset(p);
                p->ts = TS_AFTER_ATTR_NAME;
                i++;
                continue;
            }
            if (c == '/' || c == '>')
            {
                tok_add_attr(&p->cur, p->acc_n ? p->acc : "", "");
                acc_reset(p);
                p->ts = TS_AFTER_ATTR_NAME;
                continue;
            }
            if (c == '=')
            {
                tok_add_attr(&p->cur, p->acc_n ? p->acc : "", "");
                acc_reset(p);
                p->ts = TS_BEFORE_ATTR_VALUE;
                i++;
                continue;
            }
            acc_putc(p, c);
            i++;
            continue;
        case TS_AFTER_ATTR_NAME:
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f')
            {
                i++;
                continue;
            }
            if (c == '/')
            {
                p->ts = TS_SELF_CLOSING;
                i++;
                continue;
            }
            if (c == '=')
            {
                p->ts = TS_BEFORE_ATTR_VALUE;
                i++;
                continue;
            }
            if (c == '>')
            {
                p->ts = TS_DATA;
                emit_current(p);
                i++;
                continue;
            }
            acc_reset(p);
            acc_putc(p, c);
            p->ts = TS_ATTR_NAME;
            i++;
            continue;
        case TS_BEFORE_ATTR_VALUE:
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f')
            {
                i++;
                continue;
            }
            if (c == '"')
            {
                p->ts = TS_ATTR_VALUE_DQ;
                i++;
                continue;
            }
            if (c == '\'')
            {
                p->ts = TS_ATTR_VALUE_SQ;
                i++;
                continue;
            }
            if (c == '>')
            {
                p->ts = TS_DATA;
                emit_current(p);
                i++;
                continue;
            }
            p->ts = TS_ATTR_VALUE_UQ;
            continue;
        case TS_ATTR_VALUE_DQ:
            if (c == '"')
            {
                set_last_attr_value(&p->cur, p->acc_n ? p->acc : "");
                acc_reset(p);
                p->ts = TS_AFTER_ATTR_VALUE;
                i++;
                continue;
            }
            if (c == '&')
            {
                char o[8] = {0};
                size_t u = h5_decode_ref(buf + i, len - i, o, sizeof(o));
                if (u > 0)
                {
                    acc_puts(p, o[0] ? o : "");
                    i += u;
                    continue;
                }
                acc_putc(p, '&');
                i++;
                continue;
            }
            acc_putc(p, c);
            i++;
            continue;
        case TS_ATTR_VALUE_SQ:
            if (c == '\'')
            {
                set_last_attr_value(&p->cur, p->acc_n ? p->acc : "");
                acc_reset(p);
                p->ts = TS_AFTER_ATTR_VALUE;
                i++;
                continue;
            }
            if (c == '&')
            {
                char o[8] = {0};
                size_t u = h5_decode_ref(buf + i, len - i, o, sizeof(o));
                if (u > 0)
                {
                    acc_puts(p, o[0] ? o : "");
                    i += u;
                    continue;
                }
                acc_putc(p, '&');
                i++;
                continue;
            }
            acc_putc(p, c);
            i++;
            continue;
        case TS_ATTR_VALUE_UQ:
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f')
            {
                set_last_attr_value(&p->cur, p->acc_n ? p->acc : "");
                acc_reset(p);
                p->ts = TS_AFTER_ATTR_VALUE;
                i++;
                continue;
            }
            if (c == '>')
            {
                set_last_attr_value(&p->cur, p->acc_n ? p->acc : "");
                acc_reset(p);
                p->ts = TS_DATA;
                emit_current(p);
                i++;
                continue;
            }
            if (c == '&')
            {
                char o[8] = {0};
                size_t u = h5_decode_ref(buf + i, len - i, o, sizeof(o));
                if (u > 0)
                {
                    acc_puts(p, o[0] ? o : "");
                    i += u;
                    continue;
                }
                acc_putc(p, '&');
                i++;
                continue;
            }
            acc_putc(p, c);
            i++;
            continue;
        case TS_AFTER_ATTR_VALUE:
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f')
            {
                p->ts = TS_BEFORE_ATTR_NAME;
                i++;
                continue;
            }
            if (c == '/')
            {
                p->ts = TS_SELF_CLOSING;
                i++;
                continue;
            }
            if (c == '>')
            {
                p->ts = TS_DATA;
                emit_current(p);
                i++;
                continue;
            }
            p->ts = TS_BEFORE_ATTR_NAME;
            continue;
        case TS_SELF_CLOSING:
            if (c == '>')
            {
                p->cur.is_self_closing = 1;
                p->ts = TS_DATA;
                emit_current(p);
                i++;
                continue;
            }
            p->ts = TS_BEFORE_ATTR_NAME;
            continue;
        case TS_BOGUS_COMMENT:
            if (c == '>')
            {
                tok_set_name(&p->cur, p->acc_n ? p->acc : "");
                p->cur.type = TT_COMMENT;
                p->ts = TS_DATA;
                emit_current(p);
                i++;
                continue;
            }
            acc_putc(p, c);
            i++;
            continue;
        case TS_MARKUP_DECL_OPEN:
            if (c == '-' && i + 1 < len && buf[i + 1] == '-')
            {
                p->ts = TS_COMMENT;
                acc_reset(p);
                i += 2;
                continue;
            }
            if (tolower((unsigned char)c) == 'd')
            {
                p->ts = TS_DOCTYPE;
                acc_reset(p);
                acc_putc(p, c);
                i++;
                continue;
            }
            p->ts = TS_BOGUS_COMMENT;
            acc_reset(p);
            continue;
        case TS_COMMENT:
            if (c == '-' && i + 2 < len && buf[i + 1] == '-' && buf[i + 2] == '>')
            {
                tok_set_name(&p->cur, p->acc_n ? p->acc : "");
                p->cur.type = TT_COMMENT;
                p->ts = TS_DATA;
                emit_current(p);
                i += 3;
                continue;
            }
            if (c == '-' && i + 1 == len)
            {
                acc_putc(p, c);
                i++;
                continue;
            }
            acc_putc(p, c);
            i++;
            continue;
        case TS_DOCTYPE:
            if (c == '>')
            {
                tok_set_name(&p->cur, p->acc_n ? p->acc : "");
                p->cur.type = TT_DOCTYPE;
                p->ts = TS_DATA;
                emit_current(p);
                i++;
                continue;
            }
            acc_putc(p, c);
            i++;
            continue;
        case TS_RAWTEXT:
        case TS_RCDATA:
        {
            const char *end = NULL;
            for (size_t k = i; k + 1 < len; k++)
            {
                if (buf[k] == '<' && buf[k + 1] == '/')
                {
                    size_t m = k + 2;
                    size_t j = 0;
                    const char *tn = p->cur.name ? p->cur.name : "";
                    while (tn[j] && m < len && tolower((unsigned char)buf[m]) == tolower((unsigned char)tn[j]))
                    {
                        m++;
                        j++;
                    }
                    if (tn[j] == 0)
                    {
                        end = buf + k;
                        break;
                    }
                }
            }
            if (end)
            {
                size_t k = (size_t)(end - buf);
                if (k > i)
                {
                    if (p->ts == TS_RCDATA)
                    {
                        char outbuf[1024];
                        size_t oi = 0;
                        size_t si = i;
                        while (si < k)
                        {
                            if (buf[si] == '&')
                            {
                                char o[8] = {0};
                                size_t u = h5_decode_ref(buf + si, k - si, o, sizeof(o));
                                if (u > 0)
                                {
                                    for (size_t z = 0; z < u && o[z] && oi + 1 < sizeof(outbuf);)
                                        outbuf[oi++] = o[z++];
                                    si += u;
                                    continue;
                                }
                                outbuf[oi++] = '&';
                                si++;
                            }
                            else
                            {
                                if (oi + 1 < sizeof(outbuf))
                                    outbuf[oi++] = buf[si];
                                si++;
                            }
                            if (oi >= sizeof(outbuf) - 4)
                            {
                                outbuf[oi] = 0;
                                H5Token t;
                                memset(&t, 0, sizeof(t));
                                t.type = TT_CHAR;
                                t.name = strdup(outbuf);
                                process_token(p, &t);
                                free(t.name);
                                oi = 0;
                            }
                        }
                        outbuf[oi] = 0;
                        if (oi)
                        {
                            H5Token t;
                            memset(&t, 0, sizeof(t));
                            t.type = TT_CHAR;
                            t.name = strdup(outbuf);
                            process_token(p, &t);
                            free(t.name);
                        }
                    }
                    else
                    {
                        char *s = malloc(k - i + 1);
                        memcpy(s, buf + i, k - i);
                        s[k - i] = 0;
                        H5Token t;
                        memset(&t, 0, sizeof(t));
                        t.type = TT_CHAR;
                        t.name = s;
                        process_token(p, &t);
                        free(s);
                    }
                }
                size_t m = k + 2;
                const char *tn = p->cur.name ? p->cur.name : "";
                while (*tn && m < len && tolower((unsigned char)buf[m]) == tolower((unsigned char)*tn))
                {
                    m++;
                    tn++;
                }
                while (m < len && buf[m] != '>')
                    m++;
                if (m < len)
                    m++;
                i = m;
                {
                    H5Token t;
                    memset(&t, 0, sizeof(t));
                    t.type = TT_END;
                    t.name = strdup(p->cur.name ? p->cur.name : "");
                    process_token(p, &t);
                    free(t.name);
                    tok_reset(&p->cur);
                }
                p->ts = TS_DATA;
                continue;
            }
            {
                size_t k = len;
                if (p->ts == TS_RCDATA)
                {
                    char outbuf[1024];
                    size_t oi = 0;
                    size_t si = i;
                    while (si < k)
                    {
                        if (buf[si] == '&')
                        {
                            char o[8] = {0};
                            size_t u = h5_decode_ref(buf + si, k - si, o, sizeof(o));
                            if (u > 0)
                            {
                                for (size_t z = 0; z < u && o[z] && oi + 1 < sizeof(outbuf);)
                                    outbuf[oi++] = o[z++];
                                si += u;
                                continue;
                            }
                            outbuf[oi++] = '&';
                            si++;
                        }
                        else
                        {
                            if (oi + 1 < sizeof(outbuf))
                                outbuf[oi++] = buf[si];
                            si++;
                        }
                        if (oi >= sizeof(outbuf) - 4)
                        {
                            outbuf[oi] = 0;
                            H5Token t;
                            memset(&t, 0, sizeof(t));
                            t.type = TT_CHAR;
                            t.name = strdup(outbuf);
                            process_token(p, &t);
                            free(t.name);
                            oi = 0;
                        }
                    }
                }
                else
                {
                    char *s = malloc(k - i + 1);
                    memcpy(s, buf + i, k - i);
                    s[k - i] = 0;
                    H5Token t;
                    memset(&t, 0, sizeof(t));
                    t.type = TT_CHAR;
                    t.name = s;
                    process_token(p, &t);
                    free(s);
                }
                i = len;
            }
            continue;
        }
        case TS_EOF:
            i = len;
            continue;
        }
    }
}

static void set_last_attr_value(H5Token *t, const char *v)
{
    if (t->n_attr == 0)
        return;
    free(t->attrs[t->n_attr - 1].v);
    t->attrs[t->n_attr - 1].v = strdup(v ? v : "");
}

static void maybe_enter_rawtext(struct MiniHtml5Parser *p)
{
    struct MiniNode *n = current_node(p);
    if (!n || !n->tag)
        return;
    if (h5_is_rawtext(n->tag))
    {
        p->ts = TS_RAWTEXT;
        tok_reset(&p->cur);
        p->cur.name = strdup(n->tag);
        p->cur.type = TT_START;
    }
    else if (h5_is_rcdata(n->tag))
    {
        p->ts = TS_RCDATA;
        tok_reset(&p->cur);
        p->cur.name = strdup(n->tag);
        p->cur.type = TT_START;
    }
}

/* ================================================================== */
/* Public API                                                         */
/* ================================================================== */
MiniHtml5Parser *mini_html5_init(struct MiniDocument *doc)
{
    if (!doc)
        return NULL;
    MiniHtml5Parser *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->doc = doc;
    p->ts = TS_DATA;
    p->mode = M_INITIAL;
    ensure_html(p);
    struct MiniNode *html = NULL, *body = NULL;
    for (struct MiniNode *c = doc->root->first_child; c; c = c->next_sibling)
    {
        if (c->tag && h5_ci(c->tag, "html"))
            html = c;
    }
    if (html)
        for (struct MiniNode *c = html->first_child; c; c = c->next_sibling)
            if (c->tag && h5_ci(c->tag, "body"))
                body = c;
    if (body)
        doc->body = body;
    p->mode = M_IN_BODY;
    if (html)
        open_push(p, html);
    if (body)
        open_push(p, body);
    else
    {
        body = mini_node_create_element("body");
        mini_node_append_child(html ? html : doc->root, body);
        doc->body = body;
        open_push(p, body);
    }
    return p;
}

static size_t carryover_tail(struct MiniHtml5Parser *p, const char *buf, size_t len)
{
    switch (p->ts)
    {
    case TS_DATA:
    case TS_RCDATA:
    {
        for (size_t k = len; k > 0; k--)
        {
            if (buf[k - 1] == '&')
            {
                return len - (k - 1);
            }
            if (buf[k - 1] == ';')
                break;
        }
        return 0;
    }
    case TS_TAG_OPEN:
    case TS_END_TAG_OPEN:
    case TS_TAG_NAME:
    case TS_BEFORE_ATTR_NAME:
    case TS_ATTR_NAME:
    case TS_AFTER_ATTR_NAME:
    case TS_BEFORE_ATTR_VALUE:
    case TS_ATTR_VALUE_DQ:
    case TS_ATTR_VALUE_SQ:
    case TS_ATTR_VALUE_UQ:
    case TS_AFTER_ATTR_VALUE:
    case TS_SELF_CLOSING:
        for (size_t k = len; k > 0; k--)
            if (buf[k - 1] == '<')
                return len - (k - 1);
        return 0;
    case TS_COMMENT:
    case TS_DOCTYPE:
    case TS_BOGUS_COMMENT:
    case TS_MARKUP_DECL_OPEN:
        for (size_t k = len; k > 0; k--)
            if (buf[k - 1] == '<')
                return len - (k - 1);
        return 0;
    case TS_RAWTEXT:
        for (size_t k = len; k + 1 > 1; k--)
        {
            if (k >= 2 && buf[k - 2] == '<' && buf[k - 1] == '/')
                return len - (k - 2);
        }
        return 0;
    default:
        return 0;
    }
}

int mini_html5_feed(struct MiniHtml5Parser *p, const char *data, size_t len)
{
    if (!p || (!data && len))
        return -1;
    if (!len)
        return 0;
    size_t total = p->carry_n + len;
    char *work = malloc(total + 1);
    if (!work)
        return -1;
    if (p->carry_n)
        memcpy(work, p->carry, p->carry_n);
    if (len)
        memcpy(work + p->carry_n, data, len);
    work[total] = 0;
    tokenize_run(p, work, total);
    size_t co = carryover_tail(p, work, total);
    free(p->carry);
    p->carry = NULL;
    p->carry_n = 0;
    if (co > 0 && co <= total)
    {
        p->carry = malloc(co + 1);
        if (p->carry)
        {
            memcpy(p->carry, work + (total - co), co);
            p->carry[co] = 0;
            p->carry_n = co;
        }
    }
    free(work);
    return 0;
}

int mini_html5_finish(struct MiniHtml5Parser *p)
{
    if (!p)
        return -1;
    if (p->carry_n)
    {
        char *work = malloc(p->carry_n + 1);
        if (work)
        {
            memcpy(work, p->carry, p->carry_n);
            work[p->carry_n] = 0;
            p->carry_n = 0;
            free(p->carry);
            p->carry = NULL;
            tokenize_run(p, work, strlen(work));
            free(work);
        }
    }
    p->eof = 1;
    return 0;
}

void mini_html5_destroy(struct MiniHtml5Parser *p)
{
    if (!p)
        return;
    free(p->open);
    free(p->afe);
    tok_reset(&p->cur);
    free(p->acc);
    free(p->carry);
    free(p);
}

void mini_html5_parse(struct MiniDocument *doc, const char *html)
{
    if (!doc)
        return;
    MiniHtml5Parser *p = mini_html5_init(doc);
    if (!p)
        return;
    if (html)
        mini_html5_feed(p, html, strlen(html));
    mini_html5_finish(p);
    mini_html5_destroy(p);
}