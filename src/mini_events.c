/*
 * mini_events.c — DOM hit-test + W3C dispatch + host-input handlers.
 *
 * Layering: mini_events.c owns the C-side listener registry + the dispatch
 * algorithm (capture -> target -> bubble) + the host-input entry points that
 * main.c's GLFW callbacks call. The JS bridge (mini_js_bridge.c, Stage 3)
 * registers JS-backed trampolines into this registry via mini_events_add_*
 * and reads the activeElement/focus state. :hover/:active/:focus state bits
 * are set on MiniNode (driving match_pseudo in mini_dom.c) and interaction
 * CSS rules are re-applied via mini_dom_restyle_interaction on state change.
 *
 * Pure C99; no JS, no GL deps. Single-threaded (the engine is single-
 * threaded), so the registry needs no locking.
 */
#include "mini_events.h"
#include "mini_dom.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>

/* re-apply the page CSS so :hover/:active/:focus rules take effect (and revert
   when the state clears). Defined in mini_dom.c; stores the accumulated
   stylesheet as mini_css_apply runs. */
extern void mini_dom_restyle(struct MiniDocument *doc);

/* ------------------------------------------------------------------ */
/* Event state                                                         */
/* ------------------------------------------------------------------ */
#define MINI_LS_MAX 256 /* match the bridge's old g_ls cap */

struct MiniEventState
{
    struct MiniDocument *doc;
    MiniEventListener ls[MINI_LS_MAX];
    int ls_n;

    MiniInlineEventHandlerCb inline_handler_cb;
    void *inline_handler_ud;

    struct MiniNode *hover;        /* element under the cursor (or NULL) */
    struct MiniNode *focus;        /* activeElement (or NULL) */
    struct MiniNode *press_target; /* mousedown target (for click) */
    int press_button;

    struct MiniNode *last_click_target;
    double last_click_time; /* seconds, for dblclick */

    int vw, vh; /* last reported viewport (for resize) */
    int buttons_mask; /* currently held mouse buttons bitmask (1=left, 2=right, 4=middle) */

    /* Native form-control interaction state. The host input path used to only
       *dispatch* key/mouse events and rely on JS to mutate the DOM — which
       meant an <input>/<textarea> with no JS listener could never be edited
       and a <input type=range> could never be dragged. We now mirror the
       browser's built-in form-control behaviour here, on top of dispatch. */
    struct MiniNode *drag_range; /* <input type=range> whose thumb is held */
    char *edit_snapshot;         /* value at focus/press time -> change detect */
    int suppress_char;           /* keydown.preventDefault suppresses the
                                   following char/text-input insertion */

    /* Caret (text cursor) reporting. The renderer paints a blinking caret
       at n->caret_offset; this callback lets the host forward the caret's
       absolute pixel rect to the OS IME so the candidate window follows
       the insertion point instead of clinging to the window corner. */
    MiniCaretCb caret_cb;
    void *caret_ud;

    MiniCopyCb copy_cb; /* Ctrl+C -> host clipboard */
    void *copy_ud;

    /* Page text selection. anchor/focus are MN_TEXT_NODE leaves; the offsets
       are UTF-8 byte offsets into n->text. The renderer queries
       mini_events_node_selection_range() per text node to paint a blue
       highlight (mirrors caret_offset's node-stored model, but spans two
       endpoints so it's kept here rather than on each node). is_selecting is
       set between mousedown and mouseup while the user is dragging. */
    struct
    {
        struct MiniNode *anchor_node;
        int anchor_off;
        struct MiniNode *focus_node;
        int focus_off;
        int is_selecting;
    } sel;

    /* HTML5 drag-and-drop. source = the draggable element; the drag session
       runs from dragstart to dragend. drop_target = the element under the
       cursor that called preventDefault on dragover (so it accepts drops).
       data carries the DataTransfer "text/plain" payload the dragstart handler
       set; the drop handler reads it back. */
    struct
    {
        struct MiniNode *source;
        struct MiniNode *drop_target;
        int started;
        int drop_allowed;
        char *data;
    } dnd;

    /* Element-reposition drag (data-drag="move"): mousedown+move sets the
       element position:absolute and updates left/top every move. */
    struct
    {
        struct MiniNode *node;
        float dx, dy; /* pointer offset within the node's box */
        int active;
    } move;

    /* Mouse movement delta tracking for movementX / movementY / PointerLock */
    float last_mouse_x, last_mouse_y;
    int has_last_mouse;

    /* Mouse Gesture System (Middle Button Drag) */
    struct
    {
        int active;
        float start_x, start_y;
        float cur_x, cur_y;
        float last_dir_x, last_dir_y;
        float points[128][2];
        int num_points;
        char directions[8];
        int num_dirs;
        const char *action_name;
        const char *action_js;
    } gesture;

    /* Scrollbar thumb drag & state machine */
    int drag_scrollbar;
    MiniScrollbarState scrollbar_state;
    float scrollbar_drag_y0;
    float scrollbar_scroll_y0;

    MiniGestureCb gesture_cb;
    void *gesture_ud;
};

MiniScrollbarState mini_events_get_scrollbar_state(const MiniEventState *st)
{
    return st ? st->scrollbar_state : MINI_SCROLLBAR_IDLE;
}

void mini_events_release_capture(MiniEventState *st)
{
    if (!st) return;
    st->drag_scrollbar = 0;
    st->scrollbar_state = MINI_SCROLLBAR_IDLE;
    if (st->drag_range)
    {
        free(st->edit_snapshot);
        st->edit_snapshot = NULL;
        st->drag_range = NULL;
    }
    st->dnd.source = NULL;
    st->dnd.started = 0;
    st->move.node = NULL;
    st->move.active = 0;
    st->sel.is_selecting = 0;
    if (st->press_target)
    {
        mini_node_set_interaction_state(st->press_target, -1, 0, -1);
        st->press_target = NULL;
    }
    st->press_button = -1;
    if (st->doc)
        st->doc->dirty = 1;
}

void mini_events_set_gesture_cb(MiniEventState *st, MiniGestureCb cb, void *ud)
{
    if (!st) return;
    st->gesture_cb = cb;
    st->gesture_ud = ud;
}

int mini_events_get_gesture(const MiniEventState *st, MiniGestureState *out)
{
    if (!st || !out) return 0;
    out->active = st->gesture.active;
    out->num_points = st->gesture.num_points;
    out->action_name = st->gesture.action_name;
    if (out->num_points > 128) out->num_points = 128;
    for (int i = 0; i < out->num_points; i++)
    {
        out->points[i][0] = st->gesture.points[i][0];
        out->points[i][1] = st->gesture.points[i][1];
    }
    return st->gesture.active;
}

MiniEventState *mini_events_state_create(struct MiniDocument *doc)
{
    MiniEventState *st = (MiniEventState *)calloc(1, sizeof(*st));
    if (!st)
        return NULL;
    st->doc = doc;
    return st;
}

void mini_events_state_destroy(MiniEventState *st)
{
    /* listeners' ud (JSValues) are owned/freed by the bridge */
    free(st->edit_snapshot);
    free(st->dnd.data);
    free(st);
}

MiniEventListener *mini_events_add_listener(MiniEventState *st,
                                            struct MiniNode *target,
                                            const char *type,
                                            MiniEventListenerCb cb, void *ud,
                                            int useCapture)
{
    if (!st || !target || !type || !cb)
        return NULL;
    /* prefer reusing an inactive slot, else append */
    for (int i = 0; i < st->ls_n; i++)
    {
        if (!st->ls[i].active)
        {
            st->ls[i].target = target;
            snprintf(st->ls[i].type, sizeof st->ls[i].type, "%s", type);
            st->ls[i].cb = cb;
            st->ls[i].ud = ud;
            st->ls[i].useCapture = useCapture ? 1 : 0;
            st->ls[i].active = 1;
            return &st->ls[i];
        }
    }
    if (st->ls_n >= MINI_LS_MAX)
        return NULL;
    MiniEventListener *l = &st->ls[st->ls_n++];
    l->target = target;
    snprintf(l->type, sizeof l->type, "%s", type);
    l->cb = cb;
    l->ud = ud;
    l->useCapture = useCapture ? 1 : 0;
    l->active = 1;
    return l;
}

void mini_events_remove_listener(MiniEventState *st, MiniEventListener *l)
{
    if (!st || !l)
        return;
    l->active = 0;
    l->target = NULL;
    l->cb = NULL;
    /* ud freed by the caller (bridge owns the JSValue) */
    l->ud = NULL;
}

int mini_events_get_listener_count(const MiniEventState *st)
{
    return st ? st->ls_n : 0;
}

const MiniEventListener *mini_events_get_listener_at(const MiniEventState *st, int idx)
{
    if (!st || idx < 0 || idx >= st->ls_n)
        return NULL;
    return &st->ls[idx];
}

/* ------------------------------------------------------------------ */
/* Hit test — z-index / overflow / pointer-events aware.               */
/* ------------------------------------------------------------------ */
/* collect element children, sorted so the topmost-painted is first: highest
   z, and on ties the later tree sibling (matches render_children_z, where the
   last-painted = (max z, last tree sibling among equals) is topmost).        */
static int collect_children_zdesc(struct MiniNode *n,
                                  struct MiniNode **out, int max)
{
    int n_c = 0;
    for (struct MiniNode *c = n->first_child; c && n_c < max; c = c->next_sibling)
        out[n_c++] = c;
    /* shift out[j] right while it is NOT more topmost than k: out[j].z <= k.z
       (equal shifts too, reversing ties so later-tree comes first).         */
    for (int i = 1; i < n_c; i++)
    {
        struct MiniNode *k = out[i];
        int j = i - 1;
        while (j >= 0 && out[j]->style.z_index <= k->style.z_index)
        {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = k;
    }
    return n_c;
}

/* recursive hit test carrying an accumulated clip rect (for overflow). */
static struct MiniNode *hit_rec(struct MiniNode *n, float x, float y, float sx, float sy,
                                float cx0, float cy0, float cx1, float cy1)
{
    if (!n)
        return NULL;
    if (n->style.display == MINI_DISPLAY_NONE)
        return NULL; /* subtree not painted */

    float test_x = (n->style.position == 3 || n->style.position == 4) ? x : (x + sx);
    float test_y = (n->style.position == 3 || n->style.position == 4) ? y : (y + sy);

    /* child clip: an overflow:clip parent restricts its children to its box */
    float nx0 = cx0, ny0 = cy0, nx1 = cx1, ny1 = cy1;
    if (n->style.overflow == 1)
    {
        const MiniStyle *s = &n->style;
        if (s->abs_x > nx0)
            nx0 = s->abs_x;
        if (s->abs_y > ny0)
            ny0 = s->abs_y;
        if (s->abs_x + s->w < nx1)
            nx1 = s->abs_x + s->w;
        if (s->abs_y + s->h < ny1)
            ny1 = s->abs_y + s->h;
    }

    /* children first, reverse paint order (topmost first) */
    struct MiniNode *arr[256];
    int nc = 0;
    if (n->type == MN_ELEMENT_NODE || n->type == MN_DOCUMENT_FRAGMENT_NODE ||
        n->type == MN_DOCUMENT_NODE)
        nc = collect_children_zdesc(n, arr, 256);
    for (int i = 0; i < nc; i++)
    {
        struct MiniNode *c = arr[i];
        /* a child is only hittable where its ancestor clip permits */
        if (test_x < nx0 || test_x > nx1 || test_y < ny0 || test_y > ny1)
            break; /* the clip is uniform for all siblings here */
        struct MiniNode *h = hit_rec(c, x, y, sx, sy, nx0, ny0, nx1, ny1);
        if (h)
            return h;
    }

    /* self: painted elements with geometry, not pointer-events:none */
    if (n->type == MN_ELEMENT_NODE && n->style.display != MINI_DISPLAY_NONE &&
        n->style.pointer_events != 1)
    {
        const MiniStyle *s = &n->style;
        if (s->w > 0 && s->h > 0 &&
            test_x >= s->abs_x && test_x <= s->abs_x + s->w &&
            test_y >= s->abs_y && test_y <= s->abs_y + s->h &&
            test_x >= cx0 && test_x <= cx1 && test_y >= cy0 && test_y <= cy1)
            return (struct MiniNode *)n;
    }
    return NULL;
}

struct MiniNode *mini_dom_hit_test(struct MiniNode *root, float x, float y)
{
    if (!root)
        return NULL;
    return hit_rec(root, x, y, 0.0f, 0.0f, -1e9f, -1e9f, 1e9f, 1e9f);
}

struct MiniNode *mini_dom_hit_test_doc(struct MiniDocument *doc, float x, float y)
{
    if (!doc || !doc->body)
        return NULL;
    return hit_rec(doc->body, x, y, doc->scroll_x, doc->scroll_y, -1e9f, -1e9f, 1e9f, 1e9f);
}

void mini_events_set_inline_handler(MiniEventState *st, MiniInlineEventHandlerCb cb, void *ud)
{
    if (!st)
        return;
    st->inline_handler_cb = cb;
    st->inline_handler_ud = ud;
}

/* ------------------------------------------------------------------ */
/* Dispatch: capture -> target -> bubble.                              */
/* ------------------------------------------------------------------ */
/* fire all matching listeners at `node` for ev->type in ev->phase. */
static void fire_at(MiniEventState *st, struct MiniNode *node, MiniEvent *ev)
{
    if (!st || !node || !ev)
        return;

    /* 1. Fire registered C / JS listeners (snapshot count per W3C spec) */
    int count = st->ls_n;
    for (int i = 0; i < count && i < st->ls_n; i++)
    {
        MiniEventListener *l = &st->ls[i];
        if (!l->active || l->target != node)
            continue;
        if (strcmp(l->type, ev->type) != 0)
        {
            int match_ptr = 0;
            if (!strcmp(ev->type, "mousedown") && !strcmp(l->type, "pointerdown")) match_ptr = 1;
            else if (!strcmp(ev->type, "mousemove") && !strcmp(l->type, "pointermove")) match_ptr = 1;
            else if (!strcmp(ev->type, "mouseup") && !strcmp(l->type, "pointerup")) match_ptr = 1;
            else if (!strcmp(ev->type, "mouseleave") && !strcmp(l->type, "pointercancel")) match_ptr = 1;
            if (!match_ptr)
                continue;
        }
        int fire;
        if (ev->phase == 0)
            fire = l->useCapture; /* capture */
        else if (ev->phase == 1)
            fire = 1; /* target: both */
        else
            fire = !l->useCapture; /* bubble */
        if (!fire)
            continue;
        ev->currentTarget = node;
        l->cb(ev, l->ud);
        if (ev->stopPropagation)
            return;
    }

    /* 2. Fire inline on<type> attribute (e.g. onclick="...", onkeydown="...") during target / bubble phase */
    if (st->inline_handler_cb && (ev->phase == 1 || ev->phase == 2))
    {
        char on_attr[32];
        snprintf(on_attr, sizeof(on_attr), "on%s", ev->type);
        const char *code = mini_node_get_attribute(node, on_attr);
        if (code && code[0])
        {
            ev->currentTarget = node;
            st->inline_handler_cb(st, node, ev, code, st->inline_handler_ud);
            if (ev->stopPropagation)
                return;
        }
    }
}

void mini_event_dispatch(MiniEventState *st, MiniEvent *ev, struct MiniNode *target)
{
    if (!st || !ev || !target)
        return;
    ev->target = target;

    /* build path target -> ... -> root (document) */
    struct MiniNode *path[64];
    int np = 0;
    for (struct MiniNode *n = target; n && np < 64; n = n->parent)
        path[np++] = n;
    /* path[0] = target, path[np-1] = root */

    /* CAPTURE: root -> target (path[np-1] down to path[1]) */
    ev->phase = 0;
    for (int i = np - 1; i >= 1; i--)
    {
        ev->currentTarget = path[i];
        fire_at(st, path[i], ev);
        if (ev->stopPropagation)
            return;
    }

    /* TARGET: fire both capture+bubble listeners registered on target */
    ev->phase = 1;
    ev->currentTarget = path[0];
    fire_at(st, path[0], ev);
    if (ev->stopPropagation)
        return;

    /* BUBBLE: target -> root (path[1] up to path[np-1]) */
    if (ev->bubbles)
    {
        ev->phase = 2;
        for (int i = 1; i < np; i++)
        {
            ev->currentTarget = path[i];
            fire_at(st, path[i], ev);
            if (ev->stopPropagation)
                return;
        }
    }
}

void mini_events_restyle(MiniEventState *st)
{
    if (st && st->doc)
        mini_dom_restyle(st->doc);
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */
static double now_sec(void)
{
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

static void fill_mods(MiniEvent *ev, int mods)
{
    /* GLFW mods: SHIFT=1 CONTROL=2 ALT=4 SUPER=8 */
    ev->altKey = (mods & 4) ? 1 : 0;
    ev->ctrlKey = (mods & 2) ? 1 : 0;
    ev->shiftKey = (mods & 1) ? 1 : 0;
    ev->metaKey = (mods & 8) ? 1 : 0;
}

static MiniEvent make_mouse(const char *type, struct MiniNode *t,
                            float x, float y, int mods)
{
    MiniEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.target = t;
    ev.clientX = ev.screenX = ev.pageX = x;
    ev.clientY = ev.screenY = ev.pageY = y;
    if (t)
    {
        ev.offsetX = x - t->style.abs_x;
        ev.offsetY = y - t->style.abs_y;
    }
    ev.button = -1;
    fill_mods(&ev, mods);
    return ev;
}

static MiniEvent make_key(const char *type, struct MiniNode *t,
                          const char *key, const char *code, int kc,
                          int mods, int repeat)
{
    MiniEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.target = t;
    ev.key = key ? key : "";
    ev.code = code ? code : "";
    ev.keyCode = kc;
    ev.which = kc;
    if (key && key[0] && !key[1])
        ev.charCode = (int)(unsigned char)key[0];
    else
        ev.charCode = 0;
    ev.repeat = repeat;
    fill_mods(&ev, mods);
    return ev;
}

static int is_focusable(struct MiniNode *n)
{
    if (!n || n->type != MN_ELEMENT_NODE || !n->tag)
        return 0;
    if (!strcmp(n->tag, "input") || !strcmp(n->tag, "button") ||
        !strcmp(n->tag, "textarea") || !strcmp(n->tag, "select"))
        return 1;
    if (!strcmp(n->tag, "a") && mini_node_get_attribute(n, "href"))
        return 1;
    if (mini_node_get_attribute(n, "tabindex"))
        return 1;
    return 0;
}

/* collect focusable elements in document order */
static void collect_focusable(struct MiniNode *n, struct MiniNode **out, int *c, int max)
{
    if (!n)
        return;
    if (n->type == MN_ELEMENT_NODE && n->style.display != MINI_DISPLAY_NONE &&
        is_focusable(n) && *c < max)
        out[(*c)++] = n;
    for (struct MiniNode *c2 = n->first_child; c2; c2 = c2->next_sibling)
        collect_focusable(c2, out, c, max);
    if (n->shadow_root)
        collect_focusable(n->shadow_root, out, c, max);
}

static struct MiniNode *next_focusable(struct MiniDocument *doc,
                                       struct MiniNode *from, int backward)
{
    struct MiniNode *arr[256];
    int n = 0;
    collect_focusable(doc->root, arr, &n, 256);
    if (n == 0)
        return NULL;
    int idx = -1;
    for (int i = 0; i < n; i++)
        if (arr[i] == from)
        {
            idx = i;
            break;
        }
    if (idx < 0)
        return backward ? arr[n - 1] : arr[0];
    int ni = backward ? (idx - 1 + n) % n : (idx + 1) % n;
    return arr[ni];
}

/* ------------------------------------------------------------------ */
/* Native form-control behaviour                                       */
/* ------------------------------------------------------------------ */
/* Browsers edit <input>/<textarea> and drag <input type=range> natively
   — the value changes without any JS. This engine used to only dispatch
   the key/mouse events and trust JS to mutate the DOM, so a plain
   <input> or slider with no listener was dead. These helpers provide the
   native behaviour on top of dispatch. The renderer reads the value from
   the `value` attribute (or n->text for <textarea>), so we write there.   */

static const char *input_type(const struct MiniNode *n)
{
    const char *t = mini_node_get_attribute(n, "type");
    return t ? t : "text";
}

/* text-family <input> (the render_input default branch) or <textarea>.
   Excludes checkbox/range/color/file/submit/button/reset/image/hidden,
   which carry their own native UI and don't accept free text.            */
static int is_text_input(const struct MiniNode *n)
{
    if (!n || n->type != MN_ELEMENT_NODE || !n->tag)
        return 0;
    if (!strcmp(n->tag, "textarea"))
        return 1;
    if (!strcmp(n->tag, "input"))
    {
        const char *t = input_type(n);
        if (!strcmp(t, "checkbox") || !strcmp(t, "radio") ||
            !strcmp(t, "range") || !strcmp(t, "color") ||
            !strcmp(t, "file") || !strcmp(t, "submit") ||
            !strcmp(t, "button") || !strcmp(t, "reset") ||
            !strcmp(t, "image") || !strcmp(t, "hidden"))
            return 0;
        return 1;
    }
    return 0;
}

static int is_range_input(const struct MiniNode *n)
{
    if (!n || n->type != MN_ELEMENT_NODE || !n->tag)
        return 0;
    if (strcmp(n->tag, "input") != 0)
        return 0;
    return !strcmp(input_type(n), "range");
}

static const char *control_value_get(const struct MiniNode *n)
{
    if (!n->tag)
        return "";
    if (!strcmp(n->tag, "textarea"))
        return n->text ? n->text : "";
    return mini_node_get_attribute(n, "value");
}

/* write the value the renderer reads, and flag a repaint. */
static void control_value_set(MiniEventState *st, struct MiniNode *n,
                              const char *v)
{
    if (!n->tag)
        return;
    if (!strcmp(n->tag, "textarea"))
        mini_node_set_text(n, v ? v : "");
    else
        mini_node_set_attribute(n, "value", v ? v : "");
    n->dirty_paint = 1;
    if (st && st->doc)
        st->doc->dirty = 1;
}

static void dispatch_simple(MiniEventState *st, const char *type,
                            struct MiniNode *t, int bubbles)
{
    if (!st || !t)
        return;
    MiniEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.target = t;
    ev.bubbles = bubbles;
    mini_event_dispatch(st, &ev, t);
}

/* length in bytes of the UTF-8 code point starting at s[i] (i<len). */
static int utf8_cplen_at(const char *s, int i, int len)
{
    if (i >= len)
        return 0;
    if ((s[i] & 0x80) == 0)
        return 1;
    int n = 1;
    while (i + n < len && (s[i + n] & 0xC0) == 0x80)
        n++;
    return n;
}

/* clamp a caret offset to a valid codepoint boundary within [0,len]. */
static int caret_clamp(const char *s, int off)
{
    int len = s ? (int)strlen(s) : 0;
    if (off < 0)
        return 0;
    if (off > len)
        return len;
    /* back up if we landed mid-codepoint */
    while (off > 0 && (s[off] & 0xC0) == 0x80)
        off--;
    return off;
}

/* measure the pixel width of val[0..off) at font size fs/letter-spacing ls. */
static float measure_prefix(const char *s, int off, float fs, float ls)
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

/* the caret's absolute pixel (x,y) + line height for the focused control,
   so the host can forward it to the OS IME and the renderer can paint it. */
static void caret_pos(struct MiniNode *n, float *out_x, float *out_y,
                      float *out_h)
{
    const MiniStyle *s = &n->style;
    int is_ta = !strcmp(n->tag, "textarea");
    /* render_input draws text at fs=12 starting at x+5; textarea at x+4,y+4.
       Honour font-size when set so caret aligns with custom-sized text. */
    float fs = s->font_size > 0 ? s->font_size : 12.0f;
    float ls = s->letter_set ? s->len_letter.v : 0.0f;
    float line_h = fs + 4.0f; /* approximate line box for textarea */
    float x0 = s->abs_x + (is_ta ? 4.0f : 5.0f);
    float y0 = s->abs_y + (is_ta ? 4.0f : (s->h > fs ? (s->h - fs) * 0.5f : 0.0f));

    const char *val = control_value_get(n);
    int off = caret_clamp(val, n->caret_offset);

    if (!is_ta)
    {
        *out_x = x0 + measure_prefix(val, off, fs, ls);
        *out_y = y0;
        *out_h = fs;
        return;
    }
    /* textarea: find which line the caret is on (count '\n' before off). */
    int line = 0, line_start = 0;
    for (int i = 0; i < off; i++)
    {
        if (val[i] == '\n')
        {
            line++;
            line_start = i + 1;
        }
    }
    int col = off - line_start;
    *out_x = x0 + measure_prefix(val + line_start, col, fs, ls);
    *out_y = y0 + line * line_h;
    *out_h = fs;
}

static void fire_caret(MiniEventState *st, struct MiniNode *n)
{
    if (!st || !n || !st->caret_cb)
        return;
    float x, y, h;
    caret_pos(n, &x, &y, &h);
    st->caret_cb(n, x, y, h, st->caret_ud);
}

void mini_events_set_caret_cb(MiniEventState *st, MiniCaretCb cb, void *ud)
{
    if (!st)
        return;
    st->caret_cb = cb;
    st->caret_ud = ud;
}

void mini_events_set_copy_cb(MiniEventState *st, MiniCopyCb cb, void *ud)
{
    if (!st)
        return;
    st->copy_cb = cb;
    st->copy_ud = ud;
}

int mini_events_has_text_focus(MiniEventState *st)
{
    return (st && st->focus && is_text_input(st->focus)) ? 1 : 0;
}

/* find the codepoint boundary in s nearest to pixel x_rel (single line). */
static int offset_from_x(const char *s, float x_rel, float fs, float ls)
{
    int len = s ? (int)strlen(s) : 0;
    int best = 0;
    float best_diff = 1e9f;
    int i = 0;
    while (i <= len)
    {
        float w = measure_prefix(s, i, fs, ls);
        float diff = w - x_rel;
        if (diff < 0)
            diff = -diff;
        if (diff < best_diff)
        {
            best_diff = diff;
            best = i;
        }
        if (w >= x_rel && i > 0)
            break;
        if (i >= len)
            break;
        i += utf8_cplen_at(s, i, len);
    }
    return best;
}

/* ================================================================== */
/* Text selection (page text, not in-field)                          */
/* ================================================================== */
/* Find the MN_TEXT_NODE under (x,y) within `el`'s subtree whose laid-out
   vertical extent contains the click, choosing the inline run whose x-range
   is nearest. Text-node geometry (abs_x/abs_y/w/h) is written by layout at
   mini_dom.c:4587-4643.                                                          */
static struct MiniNode *text_node_under_rec(struct MiniNode *n, float x, float y,
                                            struct MiniNode *best,
                                            float *best_dist)
{
    if (!n)
        return best;
    if (n->type == MN_TEXT_NODE && n->text && n->text[0])
    {
        float y0 = n->style.abs_y;
        float h = n->style.h > 0 ? n->style.h : mini_text_line_height(n->style.font_size > 0 ? n->style.font_size : 16.0f);
        if (y >= y0 && y <= y0 + h)
        {
            float x0 = n->style.abs_x;
            float w = n->style.w > 0 ? n->style.w : 1e9f;
            if (x >= x0 && x <= x0 + w)
            {
                /* on this run and within x-range: best possible */
                return n;
            }
            /* right run on the line but click x is off to one side: track
               nearest so we still pick the run the user likely meant. */
            float d;
            if (x < x0)
                d = x0 - x;
            else
                d = x - (x0 + w);
            if (!best || d < *best_dist)
            {
                best = n;
                *best_dist = d;
            }
        }
    }
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        best = text_node_under_rec(c, x, y, best, best_dist);
    return best;
}

static struct MiniNode *text_node_under(struct MiniNode *el, float x, float y)
{
    if (!el)
        return NULL;
    float d = 1e9f;
    return text_node_under_rec(el, x, y, NULL, &d);
}

/* Map a click (x,y) to a byte offset within the text node's COLLAPSED src
   string (the same string the renderer paints, via mini_dom_text_layout),
   honoring wrap + text-align. Returns the offset; -1 if the node has no
   paintable text.                                                              */
static int text_offset_at(struct MiniNode *n, float x, float y)
{
    char buf[1024];
    float fs, ls, lh, dx, dy, ww;
    int align;
    int len = mini_dom_text_layout(n, buf, sizeof buf, &fs, &ls, &lh,
                                  &dx, &dy, &ww, &align);
    if (len <= 0)
        return -1;
    int starts[64], ends[64];
    float ws[64];
    int nl = mini_text_break_lines(buf, ww, fs, ls, starts, ends, ws, 64);
    if (nl <= 0)
        return 0;
    float y_rel = y - dy;
    int li = (int)(y_rel / lh);
    if (li < 0)
        li = 0;
    if (li >= nl)
        li = nl - 1;
    int ls2 = starts[li], le = ends[li];
    float lw = ws[li];
    float align_off = (align == 1) ? (ww - lw) * 0.5f
                  : (align == 2) ? (ww - lw)
                                 : 0.0f;
    char linebuf[1024];
    int L = le - ls2;
    if (L < 0)
        L = 0;
    if (L > (int)sizeof(linebuf) - 1)
        L = (int)sizeof(linebuf) - 1;
    memcpy(linebuf, buf + ls2, L);
    linebuf[L] = 0;
    int rel = offset_from_x(linebuf, x - (dx + align_off), fs, ls);
    int off = ls2 + rel;
    if (off < 0)
        off = 0;
    if (off > len)
        off = len;
    return off;
}

/* Clear the selection (also used when focus moves / a drag starts). */
static void selection_clear(MiniEventState *st)
{
    if (!st)
        return;
    if (st->sel.anchor_node)
        st->sel.anchor_node->dirty_paint = 1;
    if (st->sel.focus_node && st->sel.focus_node != st->sel.anchor_node)
        st->sel.focus_node->dirty_paint = 1;
    st->sel.anchor_node = NULL;
    st->sel.focus_node = NULL;
    st->sel.anchor_off = st->sel.focus_off = 0;
    st->sel.is_selecting = 0;
    if (st->doc)
        st->doc->dirty = 1;
}

/* Begin a selection drag at (x,y) on element `t`. */
static void selection_begin(MiniEventState *st, struct MiniNode *t,
                            float x, float y)
{
    selection_clear(st);
    struct MiniNode *tn = text_node_under(t, x, y);
    if (!tn)
        return;
    int off = text_offset_at(tn, x, y);
    if (off < 0)
        return;
    st->sel.anchor_node = st->sel.focus_node = tn;
    st->sel.anchor_off = st->sel.focus_off = off;
    st->sel.is_selecting = 1;
    tn->dirty_paint = 1;
    if (st->doc)
        st->doc->dirty = 1;
}

/* Extend the selection focus to (x,y) on the current hit element. */
static void selection_extend(MiniEventState *st, struct MiniNode *t,
                             float x, float y)
{
    if (!st->sel.is_selecting || !t)
        return;
    struct MiniNode *tn = text_node_under(t, x, y);
    if (!tn)
        return;
    int off = text_offset_at(tn, x, y);
    if (off < 0)
        return;
    if (st->sel.focus_node)
        st->sel.focus_node->dirty_paint = 1;
    st->sel.focus_node = tn;
    st->sel.focus_off = off;
    tn->dirty_paint = 1;
    if (st->doc)
        st->doc->dirty = 1;
}

static int is_all_ws_str(const char *s)
{
    if (!s) return 1;
    while (*s)
    {
        if (*s != ' ' && *s != '\t' && *s != '\r' && *s != '\n')
            return 0;
        s++;
    }
    return 1;
}

/* Does a text node `n` fall inside the [anchor,focus] range (document order)?
   If so, write the clamped [lo,hi) byte range within n's collapsed text and
   return 1. The renderer calls this per text node to paint the highlight.     */
int mini_events_node_selection_range(MiniEventState *st, const struct MiniNode *n,
                                     int *lo, int *hi)
{
    if (!st || !n || n->type != MN_TEXT_NODE || !n->text || is_all_ws_str(n->text))
        return 0;
    struct MiniNode *a = st->sel.anchor_node;
    struct MiniNode *f = st->sel.focus_node;
    if (!a || !f)
        return 0;
    /* determine document-order endpoints (start..end) */
    /* compare positions by walking from body: find which of a/f comes first.
       For a node that equals n, the range covers [min(anchor,focus),max] clamped
       per node. We compute per-node lo/hi by comparing n to a and f.           */
    /* For the boundary nodes the offset matters; for interior nodes the whole
       text is selected. We resolve start/end nodes via tree-order compare.     */
    struct MiniNode *start_n, *end_n;
    int start_off, end_off;
    if (mini_node_precedes(a, f))
    {
        start_n = a; start_off = st->sel.anchor_off;
        end_n = f; end_off = st->sel.focus_off;
    }
    else
    {
        start_n = f; start_off = st->sel.focus_off;
        end_n = a; end_off = st->sel.anchor_off;
    }
    /* is n before start or after end? */
    if (mini_node_precedes(n, start_n) || mini_node_precedes(end_n, n))
        return 0; /* outside the range */
    int nlen = (int)strlen(n->text);
    /* compute collapsed length to clamp */
    char buf[1024];
    int clen = mini_dom_text_layout(n, buf, sizeof buf, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    (void)nlen;
    int lo_v = 0, hi_v = clen;
    if (n == start_n)
        lo_v = start_off;
    if (n == end_n)
        hi_v = end_off;
    if (n == start_n && n == end_n)
    {
        lo_v = start_off;
        hi_v = end_off;
    }
    if (lo_v > hi_v)
    {
        int t2 = lo_v;
        lo_v = hi_v;
        hi_v = t2;
    }
    if (lo_v < 0)
        lo_v = 0;
    if (hi_v > clen)
        hi_v = clen;
    if (lo_v >= hi_v)
        return 0;
    if (lo)
        *lo = lo_v;
    if (hi)
        *hi = hi_v;
    return 1;
}

typedef struct
{
    struct MiniNode *start_n, *end_n;
    int start_off, end_off;
    char *out;
    size_t cap, len;
    int in_range, done;
} SelCtx;

/* pre-order walk: turn on at start_n, off after end_n; append collapsed slices */
static void sel_collect_rec(struct MiniNode *n, SelCtx *ctx)
{
    if (!n || ctx->done)
        return;
    if (n == ctx->start_n || n == ctx->end_n)
        ctx->in_range = 1;
    if (ctx->in_range && n->type == MN_TEXT_NODE && n->text)
    {
        char buf[1024];
        int clen = mini_dom_text_layout(n, buf, sizeof buf, NULL, NULL, NULL,
                                        NULL, NULL, NULL, NULL);
        int lo = 0, hi = clen;
        if (n == ctx->start_n)
            lo = ctx->start_off;
        if (n == ctx->end_n)
            hi = ctx->end_off;
        if (n == ctx->start_n && n == ctx->end_n)
        {
            lo = ctx->start_off;
            hi = ctx->end_off;
        }
        if (lo > hi)
        {
            int t = lo;
            lo = hi;
            hi = t;
        }
        if (lo < 0)
            lo = 0;
        if (hi > clen)
            hi = clen;
        if (hi > lo && ctx->len + (size_t)(hi - lo) + 1 < ctx->cap)
        {
            memcpy(ctx->out + ctx->len, buf + lo, (size_t)(hi - lo));
            ctx->len += (size_t)(hi - lo);
        }
    }
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        sel_collect_rec(c, ctx);
    if (n == ctx->end_n)
    {
        ctx->in_range = 0;
        ctx->done = 1;
    }
}

/* Concatenate the selected (collapsed) text for copy / getSelection().toString. */
int mini_events_selection_text(MiniEventState *st, char *out, size_t cap)
{
    if (!out || cap == 0)
        return 0;
    out[0] = 0;
    if (!st || !st->sel.anchor_node || !st->sel.focus_node || !st->doc ||
        !st->doc->body)
        return 0;
    struct MiniNode *a = st->sel.anchor_node, *f = st->sel.focus_node;
    SelCtx ctx;
    if (mini_node_precedes(a, f))
    {
        ctx.start_n = a;
        ctx.start_off = st->sel.anchor_off;
        ctx.end_n = f;
        ctx.end_off = st->sel.focus_off;
    }
    else
    {
        ctx.start_n = f;
        ctx.start_off = st->sel.focus_off;
        ctx.end_n = a;
        ctx.end_off = st->sel.anchor_off;
    }
    ctx.out = out;
    ctx.cap = cap;
    ctx.len = 0;
    ctx.in_range = 0;
    ctx.done = 0;
    sel_collect_rec(st->doc->body, &ctx);
    out[ctx.len] = 0;
    return (int)ctx.len;
}

/* walk body in pre-order, capturing first and last text nodes + a length */
static void find_first_last_text(struct MiniNode *n, struct MiniNode **first,
                                 struct MiniNode **last)
{
    if (!n)
        return;
    if (n->type == MN_TEXT_NODE && n->text && n->text[0])
    {
        if (!*first)
            *first = n;
        *last = n;
    }
    for (struct MiniNode *c = n->first_child; c; c = c->next_sibling)
        find_first_last_text(c, first, last);
}

void mini_events_select_all(MiniEventState *st)
{
    if (!st || !st->doc || !st->doc->body)
        return;
    selection_clear(st);
    struct MiniNode *first = NULL, *last = NULL;
    find_first_last_text(st->doc->body, &first, &last);
    if (!first)
        return;
    char buf[1024];
    int flen = mini_dom_text_layout(first, buf, sizeof buf, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL);
    int llen = mini_dom_text_layout(last, buf, sizeof buf, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL);
    st->sel.anchor_node = first;
    st->sel.anchor_off = 0;
    st->sel.focus_node = last;
    st->sel.focus_off = (last == first) ? flen : llen;
    first->dirty_paint = 1;
    if (last != first)
        last->dirty_paint = 1;
    st->doc->dirty = 1;
}

int mini_events_has_selection(MiniEventState *st)
{
    return (st && st->sel.anchor_node && st->sel.focus_node) ? 1 : 0;
}

/* ================================================================== */
/* Drag-and-drop (HTML5) + element-reposition drag                  */
/* ================================================================== */
/* the nearest ancestor (or self) of n with attribute `attr` set to a
   truthy value ("true" / non-empty). NULL if none. */
static struct MiniNode *find_attr_ancestor(struct MiniNode *n, const char *attr)
{
    for (struct MiniNode *p = n; p; p = p->parent)
    {
        if (p->type != MN_ELEMENT_NODE)
            continue;
        const char *v = mini_node_get_attribute(p, attr);
        if (v && v[0] && strcmp(v, "false") != 0)
            return p;
    }
    return NULL;
}

static struct MiniNode *draggable_ancestor(struct MiniNode *n)
{
    return find_attr_ancestor(n, "draggable");
}

/* element-reposition drag: data-drag="move" (or class "drag-move") */
static struct MiniNode *dragmove_ancestor(struct MiniNode *n)
{
    struct MiniNode *p = find_attr_ancestor(n, "data-drag");
    if (p)
    {
        const char *v = mini_node_get_attribute(p, "data-drag");
        if (v && !strcmp(v, "move"))
            return p;
    }
    for (struct MiniNode *q = n; q; q = q->parent)
    {
        if (q->type != MN_ELEMENT_NODE)
            continue;
        const char *cls = mini_node_get_attribute(q, "class");
        if (cls && strstr(cls, "drag-move"))
            return q;
    }
    return NULL;
}

/* DataTransfer payload (single "text/plain" slot). */
static char *dup_str(const char *v)
{
    if (!v)
        return NULL;
    size_t n = strlen(v) + 1;
    char *p = (char *)malloc(n);
    if (p)
        memcpy(p, v, n);
    return p;
}

void mini_events_dnd_set_data(MiniEventState *st, const char *v)
{
    if (!st)
        return;
    free(st->dnd.data);
    st->dnd.data = dup_str(v);
}

const char *mini_events_dnd_get_data(MiniEventState *st)
{
    return st ? st->dnd.data : NULL;
}

static void dnd_reset(MiniEventState *st)
{
    st->dnd.source = NULL;
    st->dnd.drop_target = NULL;
    st->dnd.started = 0;
    st->dnd.drop_allowed = 0;
    free(st->dnd.data);
    st->dnd.data = NULL;
}

/* Begin an HTML5 drag: fire dragstart on the source. If the listener
   preventDefaults, cancel the drag. The DataTransfer payload starts empty;
   the dragstart handler populates it via dataTransfer.setData(). */
static void dnd_begin(MiniEventState *st, struct MiniNode *src, float x, float y, int mods)
{
    st->dnd.source = src;
    st->dnd.started = 0;
    st->dnd.drop_target = NULL;
    st->dnd.drop_allowed = 0;
    free(st->dnd.data);
    st->dnd.data = NULL;
    MiniEvent ev = make_mouse("dragstart", src, x, y, mods);
    ev.bubbles = 1;
    mini_event_dispatch(st, &ev, src);
    if (ev.preventDefault)
    {
        dnd_reset(st); /* listener cancelled the drag */
        return;
    }
    st->dnd.started = 1;
}

/* Drive an active drag on mousemove: drag@source, dragenter/leave/dragover@hit. */
static void dnd_move(MiniEventState *st, float x, float y, int mods)
{
    if (!st->dnd.started || !st->dnd.source)
        return;
    MiniEvent ev = make_mouse("drag", st->dnd.source, x, y, mods);
    ev.bubbles = 1;
    mini_event_dispatch(st, &ev, st->dnd.source);

    struct MiniNode *hit = mini_dom_hit_test_doc(st->doc, x, y);
    if (hit && hit != st->dnd.drop_target)
    {
        if (st->dnd.drop_target)
        {
            MiniEvent le = make_mouse("dragleave", st->dnd.drop_target, x, y, mods);
            le.bubbles = 0;
            mini_event_dispatch(st, &le, st->dnd.drop_target);
        }
        MiniEvent en = make_mouse("dragenter", hit, x, y, mods);
        en.bubbles = 1;
        mini_event_dispatch(st, &en, hit);
        st->dnd.drop_target = hit;
        st->dnd.drop_allowed = 0;
    }
    if (hit)
    {
        MiniEvent ov = make_mouse("dragover", hit, x, y, mods);
        ov.bubbles = 1;
        mini_event_dispatch(st, &ov, hit);
        st->dnd.drop_allowed = ov.preventDefault ? 1 : 0;
    }
}

/* End the drag on mouseup: drop@target (if allowed) then dragend@source. */
static void dnd_end(MiniEventState *st, float x, float y, int mods)
{
    if (!st->dnd.started)
    {
        dnd_reset(st);
        return;
    }
    struct MiniNode *target = st->dnd.drop_target;
    if (st->dnd.drop_allowed && target)
    {
        MiniEvent dp = make_mouse("drop", target, x, y, mods);
        dp.bubbles = 1;
        mini_event_dispatch(st, &dp, target);
    }
    if (st->dnd.source)
    {
        MiniEvent de = make_mouse("dragend", st->dnd.source, x, y, mods);
        de.bubbles = 0;
        mini_event_dispatch(st, &de, st->dnd.source);
    }
    dnd_reset(st);
}

/* OS file drop (host drop callback): synthesize a drop on the element under
   the cursor (or body), carrying the file list as the data payload. */
void mini_events_handle_drop_files(MiniEventState *st, const char *const *paths,
                                   int count, float x, float y)
{
    if (!st || !st->doc)
        return;
    struct MiniNode *hit = mini_dom_hit_test_doc(st->doc, x, y);
    if (!hit)
        hit = st->doc->body;
    /* build a newline-joined payload of file paths; the JS side can split. */
    size_t total = 1;
    for (int i = 0; i < count; i++)
        total += (paths[i] ? strlen(paths[i]) : 0) + 1;
    char *buf = (char *)malloc(total);
    if (!buf)
        return;
    size_t o = 0;
    for (int i = 0; i < count; i++)
    {
        const char *p = paths[i] ? paths[i] : "";
        size_t l = strlen(p);
        memcpy(buf + o, p, l);
        o += l;
        buf[o++] = '\n';
    }
    buf[o] = 0;
    free(st->dnd.data);
    st->dnd.data = buf;
    st->dnd.source = NULL;
    st->dnd.drop_target = hit;
    st->dnd.drop_allowed = 1;
    MiniEvent enter = make_mouse("dragenter", hit, x, y, 0);
    enter.bubbles = 1;
    mini_event_dispatch(st, &enter, hit);
    MiniEvent ov = make_mouse("dragover", hit, x, y, 0);
    ov.bubbles = 1;
    mini_event_dispatch(st, &ov, hit);
    MiniEvent dp = make_mouse("drop", hit, x, y, 0);
    dp.bubbles = 1;
    mini_event_dispatch(st, &dp, hit);
    mini_events_restyle(st);
}

/* Set `prop:val` in n's inline style attribute, REPLACING any prior value for
   `prop` (so repeated drags don't grow the string). Also applies to the
   resolved style + marks dirty. Persisting to the inline `style` attribute is
   what survives mini_dom_restyle (which re-applies stylesheet rules each frame
   and would otherwise clobber a field-only write). */
static void set_inline_style(struct MiniNode *n, const char *prop, const char *val)
{
    char cur[1024];
    const char *c = mini_node_get_attribute(n, "style");
    snprintf(cur, sizeof cur, "%s", c ? c : "");
    char out[1024];
    size_t o = 0;
    int added = 0;
    char *tok = strtok(cur, ";");
    while (tok)
    {
        char *colon = strchr(tok, ':');
        if (colon)
        {
            *colon = 0;
            char *k = tok;
            while (*k == ' ')
                k++;
            char *v = colon + 1;
            while (*v == ' ')
                v++;
            if (strcmp(k, prop) == 0)
            {
                if (!added)
                {
                    int w = snprintf(out + o, sizeof(out) - o, "%s:%s;", prop, val);
                    if (w > 0)
                        o += (size_t)w;
                    added = 1;
                }
            }
            else
            {
                int w = snprintf(out + o, sizeof(out) - o, "%s:%s;", k, v);
                if (w > 0)
                    o += (size_t)w;
            }
        }
        tok = strtok(NULL, ";");
    }
    if (!added)
    {
        int w = snprintf(out + o, sizeof(out) - o, "%s:%s;", prop, val);
        if (w > 0)
            o += (size_t)w;
    }
    mini_node_set_attribute(n, "style", out);
    mini_style_set(n, prop, val);
    mini_node_mark_dirty(n, 1, 1);
}

/* Element-reposition drag (data-drag="move"): activate on first move, then set
   position:absolute + left/top so the node follows the pointer. Written to
   the inline style attribute so it survives restyle. */
static void move_drag_step(MiniEventState *st, float x, float y)
{
    struct MiniNode *mn = st->move.node;
    st->move.active = 1;
    if (mn->style.position == 0 || mn->style.position == 1)
        set_inline_style(mn, "position", "absolute");
    char b[32];
    snprintf(b, sizeof b, "%.0fpx", x - st->move.dx);
    set_inline_style(mn, "left", b);
    snprintf(b, sizeof b, "%.0fpx", y - st->move.dy);
    set_inline_style(mn, "top", b);
    if (st->doc)
        st->doc->dirty = 1;
}

/* keydown handling for a focused text input/textarea. Drives the caret:
   Backspace/Delete, Arrow Left/Right, Home/End, and Enter-on-textarea.
   Printable characters arrive via the char callback (apply_text_char). */
static void apply_text_edit_key(MiniEventState *st, struct MiniNode *n,
                                MiniEvent *ev)
{
    if (!st || !n || !ev || ev->preventDefault)
        return;
    const char *key = ev->key;
    if (!key || !key[0])
        return;

    int is_ta = !strcmp(n->tag, "textarea");
    const char *cur = control_value_get(n);
    int len = cur ? (int)strlen(cur) : 0;
    int caret = caret_clamp(cur, n->caret_offset);
    int moved_caret_only = 0; /* arrow/Home/End: no value change, no input */
    int shift = ev->shiftKey;

    /* Shift+arrow/Home/End extends the in-field selection (anchor fixed,
       caret moves); plain movement clears it. */
    #define SEL_ANCHOR_BEGIN()                                       \
        do                                                          \
        {                                                           \
            if (shift)                                              \
            {                                                       \
                if (n->sel_anchor_off < 0)                          \
                    n->sel_anchor_off = caret;                      \
            }                                                       \
            else                                                    \
                n->sel_anchor_off = -1;                             \
        } while (0)

    int has_sel = (n->sel_anchor_off >= 0 && n->sel_anchor_off != caret);
    int sel_a = has_sel ? (n->sel_anchor_off < caret ? n->sel_anchor_off : caret) : 0;
    int sel_b = has_sel ? (n->sel_anchor_off > caret ? n->sel_anchor_off : caret) : 0;
    if (sel_a < 0) sel_a = 0;
    if (sel_b > len) sel_b = len;

    if (!strcmp(key, "Backspace") || !strcmp(key, "Delete"))
    {
        if (has_sel && sel_b > sel_a)
        {
            char *buf = (char *)malloc(len - (sel_b - sel_a) + 1);
            if (!buf)
                return;
            memcpy(buf, cur, sel_a);
            memcpy(buf + sel_a, cur + sel_b, len - sel_b);
            buf[len - (sel_b - sel_a)] = 0;
            n->caret_offset = sel_a;
            n->sel_anchor_off = -1;
            control_value_set(st, n, buf);
            free(buf);
            dispatch_simple(st, "input", n, 1);
            fire_caret(st, n);
            mini_events_restyle(st);
            return;
        }
        if (!strcmp(key, "Backspace"))
        {
            if (caret > 0)
            {
                int cpl = utf8_cplen_at(cur, caret - 1, len);
                /* cpl is the length of the codepoint ending at `caret`:
                   back up over its continuation bytes first. */
                int start = caret - 1;
                while (start > 0 && (cur[start] & 0xC0) == 0x80)
                    start--;
                cpl = caret - start;
                char *buf = (char *)malloc(len - cpl + 1);
                if (!buf)
                    return;
                memcpy(buf, cur, caret - cpl);
                memcpy(buf + caret - cpl, cur + caret, len - caret);
                buf[len - cpl] = 0;
                caret -= cpl;
                n->sel_anchor_off = -1;
                control_value_set(st, n, buf);
                free(buf);
                n->caret_offset = caret;
                dispatch_simple(st, "input", n, 1);
            }
        }
        else if (!strcmp(key, "Delete"))
        {
            if (caret < len)
            {
                int cpl = utf8_cplen_at(cur, caret, len);
                char *buf = (char *)malloc(len - cpl + 1);
                if (!buf)
                    return;
                memcpy(buf, cur, caret);
                memcpy(buf + caret, cur + caret + cpl, len - caret - cpl);
                buf[len - cpl] = 0;
                n->sel_anchor_off = -1;
                control_value_set(st, n, buf);
                free(buf);
                n->caret_offset = caret; /* unchanged */
                dispatch_simple(st, "input", n, 1);
            }
        }
    }
    else if (!strcmp(key, "Enter") && is_ta)
    {
        char *buf = (char *)malloc(len + 2);
        if (!buf)
            return;
        memcpy(buf, cur, caret);
        buf[caret] = '\n';
        memcpy(buf + caret + 1, cur + caret, len - caret);
        buf[len + 1] = 0;
        control_value_set(st, n, buf);
        free(buf);
        n->caret_offset = caret + 1;
        dispatch_simple(st, "input", n, 1);
    }
    else if (!strcmp(key, "ArrowLeft"))
    {
        SEL_ANCHOR_BEGIN();
        if (caret > 0)
        {
            int start = caret - 1;
            while (start > 0 && (cur[start] & 0xC0) == 0x80)
                start--;
            n->caret_offset = start;
        }
        moved_caret_only = 1;
        ev->preventDefault = 1; /* don't scroll the page */
    }
    else if (!strcmp(key, "ArrowRight"))
    {
        SEL_ANCHOR_BEGIN();
        if (caret < len)
            n->caret_offset = caret + utf8_cplen_at(cur, caret, len);
        moved_caret_only = 1;
        ev->preventDefault = 1;
    }
    else if (!strcmp(key, "Home"))
    {
        SEL_ANCHOR_BEGIN();
        n->caret_offset = 0;
        moved_caret_only = 1;
        ev->preventDefault = 1;
    }
    else if (!strcmp(key, "End"))
    {
        SEL_ANCHOR_BEGIN();
        n->caret_offset = len;
        moved_caret_only = 1;
        ev->preventDefault = 1;
    }

    if (moved_caret_only || !strcmp(key, "Backspace") ||
        !strcmp(key, "Delete") || (!strcmp(key, "Enter") && is_ta))
    {
        fire_caret(st, n);
        mini_events_restyle(st);
    }
}

/* the char/text-input path: insert one Unicode code point at the caret of
   the focused text input/textarea and advance the caret. Suppressed when
   the preceding keydown was preventDefault'd. */
static void apply_text_char(MiniEventState *st, struct MiniNode *n,
                            unsigned int cp)
{
    if (!st || !n || cp == 0)
        return;

    char utf8[5];
    int kl = 0;
    if (cp < 0x80)
    {
        utf8[0] = (char)cp;
        kl = 1;
    }
    else if (cp < 0x800)
    {
        utf8[0] = (char)(0xC0 | (cp >> 6));
        utf8[1] = (char)(0x80 | (cp & 0x3F));
        kl = 2;
    }
    else if (cp < 0x10000)
    {
        utf8[0] = (char)(0xE0 | (cp >> 12));
        utf8[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        utf8[2] = (char)(0x80 | (cp & 0x3F));
        kl = 3;
    }
    else
    {
        utf8[0] = (char)(0xF0 | (cp >> 18));
        utf8[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        utf8[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        utf8[3] = (char)(0x80 | (cp & 0x3F));
        kl = 4;
    }
    utf8[kl] = 0;

    const char *type = !strcmp(n->tag, "textarea") ? NULL : input_type(n);
    int is_number = type && !strcmp(type, "number");
    const char *cur = control_value_get(n);
    int len = cur ? (int)strlen(cur) : 0;
    int caret = caret_clamp(cur, n->caret_offset);

    if (is_number)
    {
        char c = utf8[0];
        int has_dot = 0, has_sign = 0;
        for (int i = 0; i < len; i++)
        {
            if (cur[i] == '.')
                has_dot = 1;
            if ((cur[i] == '-' || cur[i] == '+') && i == 0)
                has_sign = 1;
        }
        if (c == '.')
        {
            if (has_dot || kl != 1)
                return;
        }
        else if (c == '-' || c == '+')
        {
            if (len != 0 || kl != 1)
                return;
        }
        else if (!(c >= '0' && c <= '9') || kl != 1)
            return;
    }

    int has_sel = (n->sel_anchor_off >= 0 && n->sel_anchor_off != caret);
    int sel_a = has_sel ? (n->sel_anchor_off < caret ? n->sel_anchor_off : caret) : caret;
    int sel_b = has_sel ? (n->sel_anchor_off > caret ? n->sel_anchor_off : caret) : caret;
    if (sel_a < 0) sel_a = 0;
    if (sel_b > len) sel_b = len;

    char *buf = (char *)malloc(len - (sel_b - sel_a) + kl + 1);
    if (!buf)
        return;
    memcpy(buf, cur, sel_a);
    memcpy(buf + sel_a, utf8, kl);
    memcpy(buf + sel_a + kl, cur + sel_b, len - sel_b);
    buf[len - (sel_b - sel_a) + kl] = 0;
    control_value_set(st, n, buf);
    free(buf);
    n->caret_offset = sel_a + kl;
    n->sel_anchor_off = -1;
    dispatch_simple(st, "input", n, 1);
    fire_caret(st, n);
    mini_events_restyle(st);
}

/* <input type=range>: map a cursor x to a snapped value in [min,max] and
   write it back. Returns 1 if the value actually changed. */
static int range_set_from_x(MiniEventState *st, struct MiniNode *n, float mx)
{
    const MiniStyle *s = &n->style;
    float x = s->abs_x, w = s->w;
    if (w <= 0)
        w = 140.0f;
    float lo = 0.0f, hi = 100.0f, step = 1.0f;
    const char *mn = mini_node_get_attribute(n, "min");
    const char *mx_a = mini_node_get_attribute(n, "max");
    const char *st_a = mini_node_get_attribute(n, "step");
    if (mn)
        lo = (float)atof(mn);
    if (mx_a)
        hi = (float)atof(mx_a);
    if (st_a)
        step = (float)atof(st_a);
    if (hi <= lo)
        hi = lo + 1.0f;
    if (step <= 0.0f)
        step = 1.0f;

    /* the renderer draws the thumb on a track from x+2 to x+w-2 */
    float frac = (mx - (x + 2.0f)) / (w - 4.0f);
    if (frac < 0.0f)
        frac = 0.0f;
    if (frac > 1.0f)
        frac = 1.0f;
    float val = lo + frac * (hi - lo);
    /* snap to step */
    float steps = (val - lo) / step;
    long snapped = (long)(steps + 0.5f);
    val = lo + (float)snapped * step;
    if (val < lo)
        val = lo;
    if (val > hi)
        val = hi;

    char buf[32];
    if (step == (float)(long)step && lo == (float)(long)lo &&
        hi == (float)(long)hi && val == (float)(long)val)
        snprintf(buf, sizeof buf, "%ld", (long)val);
    else
        snprintf(buf, sizeof buf, "%g", val);

    const char *cur = control_value_get(n);
    if (cur && strcmp(cur, buf) == 0)
        return 0; /* unchanged */
    control_value_set(st, n, buf);
    return 1;
}

/* snapshot/compare the value for change-event detection. */
static char *snapshot_value(struct MiniNode *n)
{
    const char *v = control_value_get(n);
    if (!v)
        v = "";
    size_t n2 = strlen(v) + 1;
    char *p = (char *)malloc(n2);
    if (p)
        memcpy(p, v, n2);
    return p;
}

static int value_changed(struct MiniNode *n, const char *snap)
{
    if (!snap)
        return 0;
    const char *v = control_value_get(n);
    return strcmp(v ? v : "", snap) != 0;
}

/* ------------------------------------------------------------------ */
/* Host-input entry points                                             */
/* ------------------------------------------------------------------ */
static int is_ancestor_node(struct MiniNode *anc, struct MiniNode *child)
{
    for (struct MiniNode *n = child; n; n = n->parent)
        if (n == anc)
            return 1;
    return 0;
}

void mini_events_handle_mouse_move(MiniEventState *st, float x, float y, int mods)
{
    if (!st || !st->doc)
        return;

    float dx = st->has_last_mouse ? (x - st->last_mouse_x) : 0.0f;
    float dy = st->has_last_mouse ? (y - st->last_mouse_y) : 0.0f;
    st->last_mouse_x = x;
    st->last_mouse_y = y;
    st->has_last_mouse = 1;

    /* While the thumb of a <input type=range> is held, the pointer can roam
       off the element and still drives the value (the browser keeps the
       "pointer capture" until release). Handle that before the normal
       hover/mousemove path. */
    if (st->drag_range)
    {
        if (range_set_from_x(st, st->drag_range, x))
            dispatch_simple(st, "input", st->drag_range, 1);
        mini_events_restyle(st);
        /* still fall through so :hover/mousemove update too */
    }

    /* HTML5 drag: start on the first move after pressing a draggable element,
       then drive drag/dragover every move. */
    if (st->dnd.source && !st->dnd.started)
        dnd_begin(st, st->dnd.source, x, y, mods);
    if (st->dnd.started)
        dnd_move(st, x, y, mods);

    if (st->move.node)
        move_drag_step(st, x, y);

    struct MiniNode *t = mini_dom_hit_test_doc(st->doc, x, y);
    struct MiniNode *old = st->hover;

    if (t != old)
    {
        if (old)
        {
            MiniEvent ev = make_mouse("mouseout", old, x, y, mods);
            ev.movementX = dx;
            ev.movementY = dy;
            ev.bubbles = 1;
            mini_event_dispatch(st, &ev, old);
            ev = make_mouse("mouseleave", old, x, y, mods);
            ev.movementX = dx;
            ev.movementY = dy;
            ev.bubbles = 0;
            mini_event_dispatch(st, &ev, old);

            for (struct MiniNode *n = old; n; n = n->parent)
            {
                if (!t || !is_ancestor_node(n, t))
                    mini_node_set_interaction_state(n, 0, -1, -1);
            }
        }
        if (t)
        {
            MiniEvent ev = make_mouse("mouseover", t, x, y, mods);
            ev.movementX = dx;
            ev.movementY = dy;
            ev.bubbles = 1;
            mini_event_dispatch(st, &ev, t);
            ev = make_mouse("mouseenter", t, x, y, mods);
            ev.movementX = dx;
            ev.movementY = dy;
            ev.bubbles = 0;
            mini_event_dispatch(st, &ev, t);

            for (struct MiniNode *n = t; n; n = n->parent)
                mini_node_set_interaction_state(n, 1, -1, -1);
        }
        st->hover = t;
        mini_events_restyle(st);
    }

    struct MiniNode *ev_t = t ? t : st->doc->body;

    /* Drag scrollbar & state machine */
    float vw_chk = (st->vw > 0) ? (float)st->vw : 1280.0f;
    if (st->drag_scrollbar && st->doc && st->doc->max_scroll_y > 0.0f)
    {
        st->scrollbar_state = MINI_SCROLLBAR_DRAGGING;
        float vh = (st->vh > 0) ? (float)st->vh : 800.0f;
        float total_h = st->doc->max_scroll_y + vh;
        float thumb_h = vh * (vh / total_h);
        if (thumb_h < 36.0f) thumb_h = 36.0f;
        if (thumb_h > vh - 10.0f) thumb_h = vh - 10.0f;
        float dy_drag = y - st->scrollbar_drag_y0;
        float scroll_delta = dy_drag / (vh - thumb_h) * st->doc->max_scroll_y;
        float new_sy = st->scrollbar_scroll_y0 + scroll_delta;
        if (new_sy < 0.0f) new_sy = 0.0f;
        if (new_sy > st->doc->max_scroll_y) new_sy = st->doc->max_scroll_y;
        st->doc->scroll_y = new_sy;
        st->doc->paint_dirty = 1;
    }
    else
    {
        if (x >= vw_chk - 14.0f && st->doc && st->doc->max_scroll_y > 0.0f)
            st->scrollbar_state = MINI_SCROLLBAR_HOVER;
        else
            st->scrollbar_state = MINI_SCROLLBAR_IDLE;
    }

    /* Mouse Gesture tracking */
    if (st->gesture.active)
    {
        st->gesture.cur_x = x;
        st->gesture.cur_y = y;
        if (st->gesture.num_points > 0)
        {
            float last_px = st->gesture.points[st->gesture.num_points - 1][0];
            float last_py = st->gesture.points[st->gesture.num_points - 1][1];
            float pdist = sqrtf((x - last_px) * (x - last_px) + (y - last_py) * (y - last_py));
            if (pdist >= 4.0f && st->gesture.num_points < 128)
            {
                st->gesture.points[st->gesture.num_points][0] = x;
                st->gesture.points[st->gesture.num_points][1] = y;
                st->gesture.num_points++;
            }
        }
        float ddx = x - st->gesture.last_dir_x;
        float ddy = y - st->gesture.last_dir_y;
        if (fabsf(ddx) > 18.0f || fabsf(ddy) > 18.0f)
        {
            char d = (fabsf(ddx) > fabsf(ddy)) ? (ddx > 0 ? 'R' : 'L') : (ddy > 0 ? 'D' : 'U');
            if (st->gesture.num_dirs == 0 || st->gesture.directions[st->gesture.num_dirs - 1] != d)
            {
                if (st->gesture.num_dirs < 7)
                {
                    st->gesture.directions[st->gesture.num_dirs++] = d;
                    st->gesture.directions[st->gesture.num_dirs] = 0;
                    st->gesture.last_dir_x = x;
                    st->gesture.last_dir_y = y;
                }
            }
        }
        const char *dirs = st->gesture.directions;
        if (!strcmp(dirs, "L"))
        {
            st->gesture.action_name = "⬅️ 后退 (Back)";
            st->gesture.action_js = "window.history.back()";
        }
        else if (!strcmp(dirs, "R"))
        {
            st->gesture.action_name = "➡️ 前进 (Forward)";
            st->gesture.action_js = "window.history.forward()";
        }
        else if (!strcmp(dirs, "U"))
        {
            st->gesture.action_name = "🔝 回到顶部 (Top)";
            st->gesture.action_js = "window.scrollTo(0, 0)";
        }
        else if (!strcmp(dirs, "D"))
        {
            st->gesture.action_name = "⬇️ 滚动到底部 (Bottom)";
            st->gesture.action_js = "window.scrollTo(0, document.body ? document.body.scrollHeight : 999999)";
        }
        else if (!strcmp(dirs, "DR") || !strcmp(dirs, "UD"))
        {
            st->gesture.action_name = "🔄 重新加载 (Reload)";
            st->gesture.action_js = "window.location.reload()";
        }
        if (st->doc) st->doc->dirty = 1;
    }

    /* Extend a page-text selection while the button is held (after hover
       tracking, using the freshly hit element). */
    if (st->sel.is_selecting && t)
        selection_extend(st, t, x, y);

    /* Extend text input selection when dragging within/across an input/textarea */
    if ((st->buttons_mask & 1) && st->press_target && is_text_input(st->press_target))
    {
        struct MiniNode *it = st->press_target;
        const MiniStyle *s = &it->style;
        int is_ta = !strcmp(it->tag, "textarea");
        float fs = s->font_size > 0 ? s->font_size : 12.0f;
        float ls = s->letter_set ? s->len_letter.v : 0.0f;
        float x0 = s->abs_x + (is_ta ? 4.0f : 5.0f);
        const char *val = control_value_get(it);
        int new_off = 0;
        if (is_ta)
        {
            float y0 = s->abs_y + 4.0f;
            float line_h = fs + 4.0f;
            int target_line = (int)((y - y0) / line_h);
            if (target_line < 0) target_line = 0;
            int L = (int)strlen(val);
            int i = 0, ln = 0, line_start = 0;
            while (ln < target_line && i < L)
            {
                if (val[i] == '\n')
                {
                    ln++;
                    line_start = i + 1;
                }
                i++;
            }
            int col = offset_from_x(val + line_start, x - x0, fs, ls);
            new_off = line_start + col;
        }
        else
        {
            new_off = offset_from_x(val, x - x0, fs, ls);
        }
        if (new_off != it->caret_offset)
        {
            it->caret_offset = new_off;
            fire_caret(st, it);
            if (st->doc)
            {
                st->doc->dirty = 1;
                st->doc->paint_dirty = 1;
            }
        }
    }

    if (ev_t)
    {
        MiniEvent ev = make_mouse("mousemove", ev_t, x, y, mods);
        ev.movementX = dx;
        ev.movementY = dy;
        ev.button = 0;
        ev.buttons = st->buttons_mask;
        ev.bubbles = 1;
        mini_event_dispatch(st, &ev, ev_t);
    }
}

void mini_events_handle_mouse_button(MiniEventState *st, int button, int action,
                                     float x, float y, int mods)
{
    if (!st || !st->doc)
        return;
    struct MiniNode *t = mini_dom_hit_test_doc(st->doc, x, y);
    if (!t && st->doc)
        t = st->doc->body ? st->doc->body : st->doc->root;
    /* GLFW 0=left 1=right 2=middle -> W3C 0=left 2=right 1=middle */
    int w3c = (button == 0) ? 0 : (button == 1) ? 2
                                                : 1;

    /* Middle button mouse gesture handling */
    if (button == 2 || w3c == 1)
    {
        if (action == 1) /* press */
        {
            st->gesture.active = 1;
            st->gesture.start_x = st->gesture.cur_x = st->gesture.last_dir_x = x;
            st->gesture.start_y = st->gesture.cur_y = st->gesture.last_dir_y = y;
            st->gesture.points[0][0] = x;
            st->gesture.points[0][1] = y;
            st->gesture.num_points = 1;
            st->gesture.num_dirs = 0;
            st->gesture.action_name = NULL;
            st->gesture.action_js = NULL;
            if (st->doc) st->doc->dirty = 1;
            return;
        }
        else /* release */
        {
            if (st->gesture.active)
            {
                if (st->gesture.action_js && st->gesture_cb)
                {
                    st->gesture_cb(st, st->gesture.action_js, st->gesture_ud);
                }
                st->gesture.active = 0;
                st->gesture.num_points = 0;
                if (st->doc) st->doc->dirty = 1;
            }
            return;
        }
    }

    if (action == 1) /* press */
    {
        /* Check scrollbar click */
        float vw = (st->vw > 0) ? (float)st->vw : 1280.0f;
        if (w3c == 0 && x >= vw - 14.0f && st->doc && st->doc->max_scroll_y > 0.0f)
        {
            st->drag_scrollbar = 1;
            st->scrollbar_state = MINI_SCROLLBAR_DRAGGING;
            st->scrollbar_drag_y0 = y;
            st->scrollbar_scroll_y0 = st->doc->scroll_y;
            if (st->doc) st->doc->dirty = 1;
            return;
        }

        st->press_target = t;
        st->press_button = button;
        int mask_bit = (w3c == 0) ? 1 : (w3c == 2) ? 2 : 4;
        st->buttons_mask |= mask_bit;
        if (t)
        {
            MiniEvent ev = make_mouse("mousedown", t, x, y, mods);
            ev.button = w3c;
            ev.buttons = st->buttons_mask;
            ev.bubbles = 1;
            mini_event_dispatch(st, &ev, t);
            mini_node_set_interaction_state(t, -1, 1, -1);
            /* click-to-focus: focusable targets grab focus; a click elsewhere
               clears it (mimics the browser's blur-on-click-void behavior). */
            mini_events_focus(st, is_focusable(t) ? t : NULL);

            /* Native <input type=range> drag: a mousedown on the slider
               captures the thumb and snaps the value to the click point.
               We snapshot the value now so the mouseup can fire `change`
               only if the drag actually moved it. */
            if (w3c == 0 && is_range_input(t) && !ev.preventDefault)
            {
                free(st->edit_snapshot);
                st->edit_snapshot = snapshot_value(t);
                st->drag_range = t;
                if (range_set_from_x(st, t, x))
                    dispatch_simple(st, "input", t, 1);
                mini_events_restyle(st);
            }

            /* Click-to-position the text caret: a left click inside a text
               <input>/<textarea> moves the caret to the character boundary
               nearest the click (so typing inserts where the user clicked,
               not always at the end) and repositions the OS IME. */
            if (w3c == 0 && is_text_input(t) && !ev.preventDefault)
            {
                /* focus moved to a field: drop any page-text selection and
                   any prior in-field selection (plain click repositions). */
                selection_clear(st);
                t->sel_anchor_off = -1;
                const MiniStyle *s = &t->style;
                int is_ta = !strcmp(t->tag, "textarea");
                float fs = s->font_size > 0 ? s->font_size : 12.0f;
                float ls = s->letter_set ? s->len_letter.v : 0.0f;
                float x0 = s->abs_x + (is_ta ? 4.0f : 5.0f);
                const char *val = control_value_get(t);
                if (is_ta)
                {
                    float y0 = s->abs_y + 4.0f;
                    float line_h = fs + 4.0f;
                    int target_line = (int)((y - y0) / line_h);
                    if (target_line < 0)
                        target_line = 0;
                    int L = (int)strlen(val);
                    int i = 0, ln = 0, line_start = 0;
                    while (ln < target_line && i < L)
                    {
                        if (val[i] == '\n')
                        {
                            ln++;
                            line_start = i + 1;
                        }
                        i++;
                    }
                    int col = offset_from_x(val + line_start, x - x0, fs, ls);
                    t->caret_offset = line_start + col;
                }
                else
                {
                    t->caret_offset = offset_from_x(val, x - x0, fs, ls);
                }
                t->sel_anchor_off = t->caret_offset;
                fire_caret(st, t);
                mini_events_restyle(st);
            }

            /* Left press on a non-field element: either start an HTML5 drag
               (draggable), or a reposition drag (data-drag="move"), or seed
               a page-text selection. A click on empty space clears the
               selection. */
            if (w3c == 0 && !ev.preventDefault && t &&
                !is_text_input(t) && !is_range_input(t))
            {
                struct MiniNode *dr = draggable_ancestor(t);
                if (dr)
                {
                    selection_clear(st);
                    st->dnd.source = dr;
                    st->dnd.started = 0; /* dragstart fires on first move */
                }
                else
                {
                    struct MiniNode *mv = dragmove_ancestor(t);
                    if (mv)
                    {
                        selection_clear(st);
                        st->move.node = mv;
                        st->move.dx = x - mv->style.abs_x;
                        st->move.dy = y - mv->style.abs_y;
                        st->move.active = 0; /* activates on first move */
                    }
                    else
                        selection_begin(st, t, x, y);
                }
            }
        }
        mini_events_restyle(st);
    }
    else /* release */
    {
        /* Always clear scrollbar drag */
        int was_drag_scrollbar = st->drag_scrollbar;
        st->drag_scrollbar = 0;
        float vw = (st->vw > 0) ? (float)st->vw : 1280.0f;
        st->scrollbar_state = (x >= vw - 14.0f && st->doc && st->doc->max_scroll_y > 0.0f)
                              ? MINI_SCROLLBAR_HOVER : MINI_SCROLLBAR_IDLE;
        if (was_drag_scrollbar && st->doc)
            st->doc->dirty = 1;

        /* a selection drag is over; the selection itself persists. */
        st->sel.is_selecting = 0;

        /* end an HTML5 drag (drop + dragend) or clear a pending one; and
           clear a reposition drag. */
        if (st->dnd.source)
            dnd_end(st, x, y, mods);
        st->move.node = NULL;
        st->move.active = 0;

        /* If a range drag was active, the value already tracked the pointer
           on the move path; on release fire `change` (if it moved) and drop
           the capture. */
        if (st->drag_range)
        {
            struct MiniNode *r = st->drag_range;
            st->drag_range = NULL;
            if (value_changed(r, st->edit_snapshot))
                dispatch_simple(st, "change", r, 1);
            free(st->edit_snapshot);
            st->edit_snapshot = NULL;
            mini_events_restyle(st);
        }

        struct MiniNode *rel_node = t ? t : (st->press_target ? st->press_target : (st->doc ? st->doc->body : NULL));
        if (w3c == 0 && st->press_target && is_text_input(st->press_target))
        {
            if (st->press_target->sel_anchor_off == st->press_target->caret_offset)
                st->press_target->sel_anchor_off = -1;
        }
        int mask_bit = (w3c == 0) ? 1 : (w3c == 2) ? 2 : 4;
        st->buttons_mask &= ~mask_bit;
        if (rel_node)
        {
            MiniEvent ev = make_mouse("mouseup", rel_node, x, y, mods);
            ev.button = w3c;
            ev.buttons = st->buttons_mask;
            ev.bubbles = 1;
            mini_event_dispatch(st, &ev, rel_node);
            mini_node_set_interaction_state(rel_node, -1, 0, -1);
            if (st->press_target && st->press_target != rel_node)
                mini_node_set_interaction_state(st->press_target, -1, 0, -1);

            /* click: press + release on the same target or ancestor/descendant */
            struct MiniNode *click_tgt = NULL;
            if (t && st->press_target)
            {
                if (t == st->press_target) click_tgt = t;
                else
                {
                    for (struct MiniNode *p = t; p; p = p->parent)
                        if (p == st->press_target) { click_tgt = st->press_target; break; }
                    if (!click_tgt)
                        for (struct MiniNode *p = st->press_target; p; p = p->parent)
                            if (p == t) { click_tgt = t; break; }
                }
            }
            else if (t) click_tgt = t;
            else if (st->press_target) click_tgt = st->press_target;

            if (click_tgt && w3c == 0)
            {
                ev = make_mouse("click", click_tgt, x, y, mods);
                ev.button = 0;
                ev.buttons = st->buttons_mask;
                ev.bubbles = 1;
                mini_event_dispatch(st, &ev, click_tgt);

                if (!ev.preventDefault)
                {
                    for (struct MiniNode *anc = click_tgt; anc; anc = anc->parent)
                    {
                        if (anc->tag && !strcmp(anc->tag, "a"))
                        {
                            const char *href = mini_node_get_attribute(anc, "href");
                            if (href && href[0] == '#')
                            {
                                if (href[1] == '\0')
                                {
                                    st->doc->scroll_y = 0.0f;
                                }
                                else
                                {
                                    struct MiniNode *tgt = mini_dom_query_selector(st->doc, href);
                                    if (tgt)
                                    {
                                        st->doc->scroll_y = tgt->style.abs_y;
                                        if (st->doc->scroll_y > st->doc->max_scroll_y)
                                            st->doc->scroll_y = st->doc->max_scroll_y;
                                    }
                                }
                                st->doc->dirty = 1;
                            }
                            break;
                        }
                    }

                    /* Checkbox & label handling */
                    struct MiniNode *chk_node = NULL;
                    if (t->tag && !strcmp(t->tag, "input"))
                    {
                        const char *type = mini_node_get_attribute(t, "type");
                        if (type && !strcmp(type, "checkbox"))
                            chk_node = t;
                    }
                    else if (t->tag && !strcmp(t->tag, "label"))
                    {
                        const char *for_id = mini_node_get_attribute(t, "for");
                        if (for_id && for_id[0] && st->doc)
                        {
                            char sel[128];
                            snprintf(sel, sizeof(sel), "#%s", for_id);
                            chk_node = mini_dom_query_selector(st->doc, sel);
                        }
                    }

                    if (chk_node)
                    {
                        if (mini_node_get_attribute(chk_node, "checked"))
                        {
                            mini_node_remove_attribute(chk_node, "checked");
                        }
                        else
                        {
                            mini_node_set_attribute(chk_node, "checked", "");
                        }
                        if (st->doc)
                        {
                            st->doc->dirty = 1;
                            st->doc->paint_dirty = 1;
                        }
                        MiniEvent chev = make_mouse("change", chk_node, x, y, mods);
                        chev.bubbles = 1;
                        mini_event_dispatch(st, &chev, chk_node);
                        mini_events_restyle(st);
                    }
                }

                double now = now_sec();
                if (t == st->last_click_target &&
                    (now - st->last_click_time) < 0.5)
                {
                    ev = make_mouse("dblclick", t, x, y, mods);
                    ev.button = 0;
                    ev.bubbles = 1;
                    mini_event_dispatch(st, &ev, t);
                    st->last_click_target = NULL;
                }
                else
                {
                    st->last_click_target = t;
                    st->last_click_time = now;
                }
            }
            else if (w3c == 2)
            {
                struct MiniNode *cmt = t ? t : (st->press_target ? st->press_target : (st->doc ? st->doc->body : NULL));
                if (cmt)
                {
                    MiniEvent cmev = make_mouse("contextmenu", cmt, x, y, mods);
                    cmev.button = 2;
                    cmev.bubbles = 1;
                    mini_event_dispatch(st, &cmev, cmt);
                }
            }
            mini_events_restyle(st);
        }
        st->press_target = NULL;
    }
}

void mini_events_handle_key(MiniEventState *st, const char *type,
                            const char *key, const char *code, int keyCode,
                            int mods, int repeat)
{
    if (!st || !st->doc)
        return;
    struct MiniNode *t = st->focus ? st->focus : st->doc->body;
    MiniEvent ev = make_key(type, t, key, code, keyCode, mods, repeat);
    ev.bubbles = 1;
    mini_event_dispatch(st, &ev, t);

    /* Tab moves focus (Shift+Tab backwards); we drive it here so keyboard
       navigation works without JS. */
    if (!strcmp(type, "keydown") && key && !strcmp(key, "Tab"))
    {
        struct MiniNode *nxt = next_focusable(st->doc, st->focus, mods & 1);
        if (nxt)
            mini_events_focus(st, nxt);
        ev.preventDefault = 1;
    }

    /* Native text editing on keydown: handle the non-printable edit keys
       (Backspace/Delete drop the last code point, Enter inserts a newline
       in a <textarea>). Printable characters arrive via the char callback
       so they go through apply_text_char, not here. A keydown listener
       that calls preventDefault on a printable key also suppresses the
       following char insertion (browser "before-input" semantics).
       NB: only set on keydown — keypress fires between keydown and the
       char callback and must not clobber the flag. */
    if (!strcmp(type, "keydown"))
    {
        /* Ctrl+A: select all text in the body (unless a text field owns focus
           and handled it — fields select their own content, handled below). */
        if (ev.ctrlKey && key && (key[0] == 'a' || key[0] == 'A') && !key[1] &&
            !ev.preventDefault)
        {
            if (st->focus && is_text_input(st->focus))
            {
                /* select the field's whole value */
                const char *v = control_value_get(st->focus);
                st->focus->sel_anchor_off = 0;
                st->focus->caret_offset = v ? (int)strlen(v) : 0;
                st->focus->dirty_paint = 1;
            }
            else
            {
                mini_events_select_all(st);
            }
            ev.preventDefault = 1;
        }

        /* Ctrl+C: copy the current selection (page text, or the focused
           field's in-field selection) to the host clipboard. */
        if (ev.ctrlKey && key && (key[0] == 'c' || key[0] == 'C') && !key[1] &&
            !ev.preventDefault && st->copy_cb)
        {
            char stack[4096];
            char *buf = stack;
            int heap = 0;
            if (st->focus && is_text_input(st->focus) &&
                st->focus->sel_anchor_off >= 0)
            {
                /* in-field selection: value[min(anch,caret)..max] */
                const char *v = control_value_get(st->focus);
                int vl = v ? (int)strlen(v) : 0;
                int a = st->focus->sel_anchor_off;
                int b = caret_clamp(v, st->focus->caret_offset);
                if (a > b) { int t = a; a = b; b = t; }
                if (a < 0) a = 0;
                if (b > vl) b = vl;
                int n = b - a;
                if (n > 0)
                {
                    if (n + 1 > (int)sizeof(stack))
                    {
                        buf = (char *)malloc(n + 1);
                        heap = 1;
                    }
                    if (buf)
                    {
                        memcpy(buf, v + a, n);
                        buf[n] = 0;
                        st->copy_cb(st, buf, st->copy_ud);
                    }
                }
            }
            else
            {
                int n = mini_events_selection_text(st, stack, sizeof stack);
                if (n == (int)sizeof(stack) - 1)
                {
                    /* may have been truncated; try a heap buffer for the full text */
                    buf = (char *)malloc(65536);
                    heap = 1;
                    if (buf)
                        mini_events_selection_text(st, buf, 65536);
                }
                if (buf && buf[0])
                    st->copy_cb(st, buf, st->copy_ud);
            }
            if (heap)
                free(buf);
            ev.preventDefault = 1;
        }

        if (st->focus && is_text_input(st->focus))
            apply_text_edit_key(st, st->focus, &ev);
        st->suppress_char = ev.preventDefault ? 1 : 0;
    }

    mini_events_restyle(st);
}

void mini_events_handle_char(MiniEventState *st, unsigned int codepoint)
{
    if (!st || !st->doc)
        return;
    if (st->suppress_char)
    {
        /* the preceding keydown was cancelable AND preventDefault was
           called — the listener is using the key as a hotkey, so swallow
           the text insertion. */
        st->suppress_char = 0;
        return;
    }
    if (st->focus && is_text_input(st->focus))
        apply_text_char(st, st->focus, codepoint);
}

void mini_events_handle_wheel(MiniEventState *st, float x, float y,
                              float dx, float dy, int mods)
{
    if (!st || !st->doc)
        return;
    struct MiniNode *t = mini_dom_hit_test_doc(st->doc, x, y);
    if (!t)
        t = st->doc->body;
    MiniEvent ev = make_mouse("wheel", t, x, y, mods);
    ev.bubbles = 1;
    ev.deltaX = dx * 100.0f;
    ev.deltaY = -dy * 100.0f;
    ev.deltaMode = 0; /* pixels */
    ev.buttons = st->buttons_mask;
    mini_event_dispatch(st, &ev, t);

    if (!ev.preventDefault)
    {
        float scroll_step = 60.0f;
        float new_sy = st->doc->scroll_y - dy * scroll_step;
        float new_sx = st->doc->scroll_x - dx * scroll_step;

        if (new_sy < 0.0f)
            new_sy = 0.0f;
        if (new_sy > st->doc->max_scroll_y)
            new_sy = st->doc->max_scroll_y;
        if (new_sx < 0.0f)
            new_sx = 0.0f;
        if (new_sx > st->doc->max_scroll_x)
            new_sx = st->doc->max_scroll_x;

        if (new_sy != st->doc->scroll_y || new_sx != st->doc->scroll_x)
        {
            st->doc->scroll_y = new_sy;
            st->doc->scroll_x = new_sx;
            st->doc->paint_dirty = 1;

            MiniEvent sev;
            memset(&sev, 0, sizeof sev);
            sev.type = "scroll";
            sev.target = st->doc->body;
            sev.bubbles = 1;
            fill_mods(&sev, mods);
            mini_event_dispatch(st, &sev, st->doc->body);
        }
    }
}

void mini_events_handle_resize(MiniEventState *st, int w, int h)
{
    if (!st || !st->doc)
        return;
    st->vw = w;
    st->vh = h;
    st->doc->viewport_w = w;
    st->doc->viewport_h = h;
    st->doc->dirty = 1;
    st->doc->layout_dirty = 1;
    mini_layout_run(st->doc, w, h);

    MiniEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = "resize";
    ev.target = st->doc->body;
    ev.bubbles = 1;
    fill_mods(&ev, 0);
    mini_event_dispatch(st, &ev, st->doc->body);
}

struct MiniNode *mini_events_active_element(MiniEventState *st)
{
    return st ? st->focus : NULL;
}

void mini_events_focus(MiniEventState *st, struct MiniNode *n)
{
    if (!st)
        return;
    if (st->focus == n)
        return;
    if (st->focus)
    {
        MiniEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.type = "blur";
        ev.target = st->focus;
        ev.bubbles = 0;
        mini_event_dispatch(st, &ev, st->focus);
        mini_node_set_interaction_state(st->focus, -1, -1, 0);

        /* Native change event: for text inputs/textareas, the browser
           fires `change` on blur if the value was edited since focus
           (we snapshotted it when focus was taken). <input type=range>
           fires change on thumb release, handled in the mouse path, so
           we don't double-fire it here. */
        if (is_text_input(st->focus) && !is_range_input(st->focus) &&
            value_changed(st->focus, st->edit_snapshot))
            dispatch_simple(st, "change", st->focus, 1);
        free(st->edit_snapshot);
        st->edit_snapshot = NULL;
    }
    st->focus = n;
    if (n)
    {
        MiniEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.type = "focus";
        ev.target = n;
        ev.bubbles = 0;
        mini_event_dispatch(st, &ev, n);
        mini_node_set_interaction_state(n, -1, -1, 1);

        /* snapshot the value now so a later blur/change can detect an edit.
           Range's change fires on release; we still snapshot so the release
           path can compare (it re-snapshots on press, so this is just the
           focus-time baseline). */
        if (is_text_input(n) || is_range_input(n))
        {
            free(st->edit_snapshot);
            st->edit_snapshot = snapshot_value(n);
        }
        /* place the text caret at the end of the value (click-to-position
           and arrow keys move it afterwards) and tell the host where it is
           so the OS IME candidate window follows. */
        if (is_text_input(n))
        {
            const char *v = control_value_get(n);
            n->caret_offset = v ? (int)strlen(v) : 0;
            fire_caret(st, n);
        }
    }
    mini_events_restyle(st);
}

void mini_events_on_node_destroyed(MiniEventState *st, struct MiniNode *n)
{
    if (!st || !n)
        return;
    if (st->hover == n)
        st->hover = NULL;
    if (st->focus == n)
        st->focus = NULL;
    if (st->press_target == n)
        st->press_target = NULL;
    if (st->last_click_target == n)
        st->last_click_target = NULL;
    if (st->drag_range == n)
        st->drag_range = NULL;
    if (st->sel.anchor_node == n)
    {
        st->sel.anchor_node = NULL;
        st->sel.anchor_off = 0;
    }
    if (st->sel.focus_node == n)
    {
        st->sel.focus_node = NULL;
        st->sel.focus_off = 0;
    }
    /* Remove all listeners targeting the destroyed node */
    for (int i = 0; i < st->ls_n; )
    {
        if (st->ls[i].target == n)
        {
            for (int j = i; j < st->ls_n - 1; j++)
                st->ls[j] = st->ls[j + 1];
            st->ls_n--;
        }
        else
        {
            i++;
        }
    }
}

/* ================================================================== */
/* HIT_TEST_SELFTEST — build a tiny flex tree, probe points.           */
/* ================================================================== */
#ifdef HIT_TEST_SELFTEST
#include <stdio.h>

static void setgeom(struct MiniNode *n, float x, float y, float w, float h)
{
    n->style.abs_x = x;
    n->style.abs_y = y;
    n->style.w = w;
    n->style.h = h;
    n->style.laid_out = 1;
    n->style.display = 0;
    n->style.bg_a = 1.0f;
}
static const char *tag(struct MiniNode *n) { return n && n->tag ? n->tag : "?"; }

int main(void)
{
    /* root (0,0,200,200)
          child A (0,0,100,200)        -- left half
            buttonA (10,10,30,30)
          child B (100,0,100,200)      -- right half, painted AFTER A (topmost) */
    MiniDocument *d = mini_doc_create();
    struct MiniNode *root = d->body;
    setgeom(root, 0, 0, 200, 200);
    struct MiniNode *A = mini_node_create_element("div");
    setgeom(A, 0, 0, 100, 200);
    struct MiniNode *B = mini_node_create_element("div");
    setgeom(B, 100, 0, 100, 200);
    mini_node_append_child(root, A);
    mini_node_append_child(root, B);
    struct MiniNode *btn = mini_node_create_element("button");
    setgeom(btn, 10, 10, 30, 30);
    mini_node_append_child(A, btn);

    int fails = 0;
    struct MiniNode *h;
    h = mini_dom_hit_test(root, 150, 100); /* right half -> B */
    if (h == B)
        printf("[PASS] hit B (right half)\n");
    else
    {
        printf("[FAIL] expected B got %s\n", tag(h));
        fails++;
    }
    h = mini_dom_hit_test(root, 20, 20); /* inside button */
    if (h == btn)
        printf("[PASS] hit button\n");
    else
    {
        printf("[FAIL] expected btn got %s\n", tag(h));
        fails++;
    }
    h = mini_dom_hit_test(root, 60, 100); /* left, not button -> A */
    if (h == A)
        printf("[PASS] hit A\n");
    else
    {
        printf("[FAIL] expected A got %s\n", tag(h));
        fails++;
    }
    h = mini_dom_hit_test(root, 999, 999); /* miss */
    if (h == NULL)
        printf("[PASS] miss returns null\n");
    else
    {
        printf("[FAIL] expected null\n");
        fails++;
    }

    /* pointer-events:none passthrough: B ignores hits; a point over B falls
       through to whatever is painted behind it. Here B is the right half and
       only the parent body is behind it at (150,100), so it hits body.       */
    B->style.pointer_events = 1;
    h = mini_dom_hit_test(root, 150, 100);
    if (h == root)
        printf("[PASS] pointer-events:none on B -> falls through to body\n");
    else
    {
        printf("[FAIL] pointer-events passthrough got %s\n", tag(h));
        fails++;
    }
    B->style.pointer_events = 0;

    /* overflow:clip on A clips its button child: a point in the button but
       outside A's box is NOT a hit (here button is inside A, so still hits) */
    A->style.overflow = 1;
    h = mini_dom_hit_test(root, 20, 20);
    mini_doc_destroy(d);
    printf(fails ? "HIT_TEST_SELFTEST: %d FAIL\n" : "HIT_TEST_SELFTEST: all PASS\n", fails);
    return fails ? 1 : 0;
}
#endif
