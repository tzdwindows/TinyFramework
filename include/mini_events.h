/*
 * mini_events.h — host input -> DOM hit-test + W3C dispatch bridge.
 *
 * The host (GLFW/SDL/Win32) reports raw pointer/key events; we resolve them
 * to a target MiniNode by point/z/overflow/pointer-events hit-testing the
 * LAID-OUT tree, then run capture -> target -> bubble over a C-side listener
 * registry (the JS bridge registers JS-backed trampolines into it in Stage 3).
 * stopPropagation/preventDefault are honored. :hover/:active/:focus state is
 * driven here and re-applies interaction CSS rules via mini_dom.
 */
#ifndef MINI_EVENTS_H
#define MINI_EVENTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct MiniNode;
struct MiniDocument;

/* ------------------------------------------------------------------ */
/* Event object (a W3C-ish subset). Lives on the stack during dispatch. */
/* ------------------------------------------------------------------ */
typedef struct MiniEvent
{
    const char *type;            /* "click","mousedown","keydown",... */
    struct MiniNode *target;    /* hit-tested target (set before dispatch) */
    struct MiniNode *currentTarget; /* node whose listener is firing */
    int phase;                   /* 0 capture, 1 target, 2 bubble */
    /* mouse */
    float clientX, clientY, screenX, screenY;
    float pageX, pageY, offsetX, offsetY;
    float movementX, movementY;
    int button;                 /* 0=left 1=middle 2=right */
    int buttons;                /* bitmask: 1=left 2=right 4=middle */
    int altKey, ctrlKey, shiftKey, metaKey;
    /* keyboard */
    const char *key;            /* "Enter","ArrowUp","a",... */
    const char *code;           /* "KeyA","Enter",... */
    int keyCode;
    int which;
    int charCode;
    int repeat;
    int isComposing;
    /* wheel */
    float deltaX, deltaY;
    int deltaMode;              /* 0=pixel (the only mode we emit) */
    /* control */
    int stopPropagation;
    int preventDefault;
    int bubbles;                /* does this event type bubble? */
    void *ud;                   /* bridge private (e.g. JS event object) */
} MiniEvent;

/* listener callback. ud is the listener's ud (bridge wraps the JSValue). */
typedef struct MiniEventState MiniEventState;
typedef void (*MiniEventListenerCb)(MiniEvent *ev, void *ud);

/* inline HTML attribute handler callback (e.g. onclick="...") */
typedef void (*MiniInlineEventHandlerCb)(MiniEventState *st, struct MiniNode *node, MiniEvent *ev, const char *code, void *ud);

/* caret (text cursor) moved callback. Fired whenever the insertion point
   changes on a focused <input>/<textarea>. (x,y) is the caret's top-left
   in document absolute pixels; h is the caret line height. The host uses
   this to position the OS IME candidate window. */
typedef void (*MiniCaretCb)(struct MiniNode *n, float x, float y, float h, void *ud);

/* copy-to-clipboard callback: fired on Ctrl+C with the currently selected
 * text (page selection or in-field selection). The host forwards it to the
 * OS clipboard (e.g. glfwSetClipboardString). */
typedef void (*MiniCopyCb)(MiniEventState *st, const char *text, void *ud);

typedef struct MiniEventListener
{
    struct MiniNode *target;
    char type[24];
    MiniEventListenerCb cb;
    void *ud;
    int useCapture;             /* 1 = capture phase, 0 = bubble phase */
    int active;                 /* 0 = slot free / removed */
} MiniEventListener;

/* lifecycle. The state owns no nodes; it references the live document. */
MiniEventState *mini_events_state_create(struct MiniDocument *doc);
void mini_events_state_destroy(MiniEventState *st);

/* ---- listener registry ---- */
/* Register a listener. Returns a handle (NULL if the table is full). The
   handle is stable until removed (or the state is destroyed).             */
MiniEventListener *mini_events_add_listener(MiniEventState *st,
                                            struct MiniNode *target,
                                            const char *type,
                                            MiniEventListenerCb cb,
                                            void *ud, int useCapture);
void mini_events_remove_listener(MiniEventState *st, MiniEventListener *l);
int mini_events_get_listener_count(const MiniEventState *st);
const MiniEventListener *mini_events_get_listener_at(const MiniEventState *st, int idx);

/* ---- hit test (z-index / overflow / pointer-events aware) ---- */
/* Resolve the frontmost element under (x,y) in CSS pixels (top-left origin,
   +Y down, matching mini_layout_run's output). Walks children in reverse
   paint order (highest z, then last tree sibling), recursing before testing
   self. Honors overflow clip and pointer-events:none passthrough. Returns
   NULL if nothing is hit. (Legacy signature kept for the self-test.)       */
struct MiniNode *mini_dom_hit_test(struct MiniNode *root, float x, float y);
/* Resolve the frontmost element under (x,y) accounting for the document's
 * current scroll offset. This is the form elementFromPoint(x,y) uses. */
struct MiniNode *mini_dom_hit_test_doc(struct MiniDocument *doc, float x, float y);

/* ---- dispatch ---- */
/* Set inline attribute handler (e.g. onclick="...") */
void mini_events_set_inline_handler(MiniEventState *st, MiniInlineEventHandlerCb cb, void *ud);

/* Run capture -> target -> bubble over the registry for ev->type. ev->target
 * must be set. If ev->bubbles is 0, only the target phase fires. Honors
 * stopPropagation (stops further nodes) and preventDefault (flag only).    */
void mini_event_dispatch(MiniEventState *st, MiniEvent *ev,
                         struct MiniNode *target);

/* Re-apply :hover / :active / :focus / :focus-within CSS rules based on the
 * current interaction state. Called internally on state change; also public
 * so the bridge can restyle after a programmatic focus()/blur().           */
void mini_events_restyle(MiniEventState *st);

/* ---- host-input entry points (main.c calls these) ---- */
/* mouse move: derives mousemove + the mouseover/out + mouseenter/leave pair
 * on hover-target change, and drives :hover state.                       */
void mini_events_handle_mouse_move(MiniEventState *st, float x, float y, int mods);
/* mouse button: action 1=press, 0=release. Derives mousedown/mouseup/click/
 * dblclick and drives :active. (x,y) is the cursor at the event.          */
void mini_events_handle_mouse_button(MiniEventState *st, int button, int action,
                                     float x, float y, int mods);
/* keyboard: type is "keydown"/"keyup"/"keypress". Drives focus + :focus.   */
void mini_events_handle_key(MiniEventState *st, const char *type,
                           const char *key, const char *code, int keyCode,
                           int mods, int repeat);
/* Unicode text input (host char callback): appends one code point to the
 * focused <input>/<textarea> and dispatches `input`. Suppressed when the
 * preceding keydown called preventDefault (hotkeys). */
void mini_events_handle_char(MiniEventState *st, unsigned int codepoint);
/* wheel: dispatches wheel on the hovered node (bubbles).                   */
void mini_events_handle_wheel(MiniEventState *st, float x, float y,
                              float dx, float dy, int mods);
/* window resize: dispatches resize (target = document).                   */
void mini_events_handle_resize(MiniEventState *st, int w, int h);

/* ---- focus management ---- */
struct MiniNode *mini_events_active_element(MiniEventState *st);
void mini_events_focus(MiniEventState *st, struct MiniNode *n);

/* Register the caret-moved callback (for OS IME positioning). */
void mini_events_set_caret_cb(MiniEventState *st, MiniCaretCb cb, void *ud);

/* Register the copy-to-clipboard callback (fired on Ctrl+C). */
void mini_events_set_copy_cb(MiniEventState *st, MiniCopyCb cb, void *ud);

/* 1 if the focused element is a text <input>/<textarea> (i.e. a blinking
 * caret is active and the host should keep rendering for blink). */
int mini_events_has_text_focus(MiniEventState *st);

/* ---- text selection (page text) ---- */
/* Per-text-node selected byte range [lo,hi) in the collapsed string, for the
 * renderer to paint the highlight. Returns 1 if `n` is inside the selection. */
int mini_events_node_selection_range(MiniEventState *st, const struct MiniNode *n,
                                    int *lo, int *hi);
/* Selected text (collapsed) for copy / getSelection().toString. Writes a
 * NUL-terminated string into out[cap]; returns the length (0 if none). */
int mini_events_selection_text(MiniEventState *st, char *out, size_t cap);
/* Select all text in the body (Ctrl+A). */
void mini_events_select_all(MiniEventState *st);
/* 1 if there is an active page-text selection. */
int mini_events_has_selection(MiniEventState *st);

/* ---- drag-and-drop ---- */
/* DataTransfer "text/plain" slot (set by the dragstart handler, read by drop). */
void mini_events_dnd_set_data(MiniEventState *st, const char *v);
const char *mini_events_dnd_get_data(MiniEventState *st);
/* OS file drop (host glfw drop callback): synthesize dragenter/dragover/drop
 * on the element under (x,y) with the file paths as the payload. */
void mini_events_handle_drop_files(MiniEventState *st, const char *const *paths,
                                   int count, float x, float y);

/* ---- mouse gestures ---- */
typedef struct MiniGestureState
{
    int active;
    float points[128][2];
    int num_points;
    const char *action_name;
} MiniGestureState;

typedef void (*MiniGestureCb)(MiniEventState *st, const char *action_js, void *ud);
void mini_events_set_gesture_cb(MiniEventState *st, MiniGestureCb cb, void *ud);
int mini_events_get_gesture(const MiniEventState *st, MiniGestureState *out);

#ifdef __cplusplus
}
#endif
#endif /* MINI_EVENTS_H */
