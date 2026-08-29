/*
 * main.c �?TinyFramework entry point + main loop.
 *
 * The mini_app_* surface (mini_framework.h) is identical in both build
 * modes. CUSTOM_MINI wires renderer -> document -> bridge -> [cdp,diag];
 * NATIVE would wire a host WebView instead (stubbed).
 *
 * Main loop contract (the heart of the CUSTOM_MINI mode):
 *   diag.begin -> poll events -> fire rAF(JS) [timed] -> pump jobs ->
 *   layout(DOM) [timed] -> render flush [timed] -> diag.end -> cdp.poll
 */
#include "mini_framework.h"
#include "mini_renderer.h"
#include "mini_dom.h"
#include "mini_events.h"
#include "mini_js_bridge.h"
#include "mini_native.h"
#include "mini_window.h"
#include "mini_ipc.h"
#include "mini_protocol.h"
#include "mini_net.h"
#include "mini_vfs.h"
#include "mini_cdp.h"
#include "mini_diag.h"
#include "mini_log.h"
#include "mini_crash.h"
#include "mini_devtools.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if MINI_MODE_CUSTOM
#include <GLFW/glfw3.h>
#endif

/* Win32 IMM (Input Method Manager) so the IME candidate window tracks the
   text caret instead of clinging to the window corner. glfwGetWin32Window
   needs the native header behind GLFW_EXPOSE_NATIVE_WIN32. */
#if MINI_MODE_CUSTOM && defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GLFW/glfw3native.h>
#include <imm.h>
#endif

/* Packers prefix bytecode bundles with this 4-byte marker so the loader
   knows whether to feed the decrypted payload to JS_Eval (source) or to
   JS_ReadObject (bytecode). See build.py. */
#define MINI_BC_MARKER "QJC1"

/* ------------------------------------------------------------------ */
/* App handle                                                          */
/* ------------------------------------------------------------------ */
struct MiniApp
{
    MiniWindowConfig cfg;
#if MINI_MODE_CUSTOM
    MiniRenderer *r;
    MiniDocument *doc;
    MiniBridge *bridge;
    MiniEventState *events;
    MiniCDP *cdp;
    MiniDiag *diag;
    char *page_url;
    char *page_source;
    int loaded;

    /* Multi-window: the list of OS windows. windows[0] is the primary (a thin
       wrapper aliasing r/doc/bridge/events above); windows[1..N] are secondary
       renderer windows opened via `new BrowserWindow()`, each owning its own
       r/doc/bridge/events. next_window_id hands out ids >= 1. */
    MiniWindow **windows;
    int n_windows;
    int cap_windows;
    int next_window_id;
    int quitting; /* set when before-quit has fired (avoids double-emit) */
    MiniIPC *ipc; /* process-wide IPC registry (main + every renderer) */
    MiniProtocol *proto; /* process-wide custom-scheme registry (main ctx) */
#endif
};

int mini_app_mode(void) { return MINI_MODE_CUSTOM ? 1 : 0; }

/* ---- window-list accessors (struct MiniApp is private to this TU; these
 * let mini_window.c manage windows without seeing the struct) ----------- */
MiniWindow **mini_app_windows(MiniApp *app, int *n_out)
{
#if MINI_MODE_CUSTOM
    if (n_out)
        *n_out = app ? app->n_windows : 0;
    return app ? app->windows : NULL;
#else
    if (n_out) *n_out = 0;
    return NULL;
#endif
}

void mini_app_add_window(MiniApp *app, MiniWindow *mw)
{
#if MINI_MODE_CUSTOM
    if (!app || !mw)
        return;
    if (app->n_windows >= app->cap_windows)
    {
        int nc = app->cap_windows ? app->cap_windows * 2 : 8;
        MiniWindow **nw = (MiniWindow **)realloc(app->windows, (size_t)nc * sizeof(*nw));
        if (!nw)
            return;
        app->windows = nw;
        app->cap_windows = nc;
    }
    app->windows[app->n_windows++] = mw;
#else
    (void)app; (void)mw;
#endif
}

int mini_app_next_window_id(MiniApp *app)
{
#if MINI_MODE_CUSTOM
    if (!app)
        return 0;
    if (app->next_window_id == 0)
        app->next_window_id = 1; /* 0 is the primary */
    return app->next_window_id++;
#else
    (void)app;
    return 0;
#endif
}

void mini_app_remove_window(MiniApp *app, MiniWindow *mw)
{
#if MINI_MODE_CUSTOM
    if (!app || !mw)
        return;
    for (int i = 0; i < app->n_windows; i++)
    {
        if (app->windows[i] == mw)
        {
            /* compact: shift the tail down */
            for (int j = i; j < app->n_windows - 1; j++)
                app->windows[j] = app->windows[j + 1];
            app->n_windows--;
            app->windows[app->n_windows] = NULL;
            return;
        }
    }
#else
    (void)app; (void)mw;
#endif
}

/* The host's main (process) bridge — used by mini_native.c (BrowserWindow
 * event dispatch / finalizer) to reach the main context. struct MiniApp is
 * private to this TU. */
MiniBridge *mini_app_main_bridge(MiniApp *app)
{
#if MINI_MODE_CUSTOM
    return app ? app->bridge : NULL;
#else
    (void)app;
    return NULL;
#endif
}

/* The process-wide MiniIPC registry (opaque; mini_window.c installs it on
 * each secondary renderer bridge). Returns NULL in non-CUSTOM mode. */
void *mini_app_ipc(MiniApp *app)
{
#if MINI_MODE_CUSTOM
    return app ? app->ipc : NULL;
#else
    (void)app;
    return NULL;
#endif
}

/* The process-wide MiniProtocol registry (custom-scheme handlers). */
void *mini_app_proto(MiniApp *app)
{
#if MINI_MODE_CUSTOM
    return app ? app->proto : NULL;
#else
    (void)app;
    return NULL;
#endif
}

/* ------------------------------------------------------------------ */
/* File loader (tiny, no deps)                                         */
/* ------------------------------------------------------------------ */
static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0)
    {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len)
        *out_len = rd;
    return buf;
}

/* ================================================================== */
/* CUSTOM_MINI mode                                                    */
/* ================================================================== */
#if MINI_MODE_CUSTOM

/* optional frame cap for automated runs: set via env TINY_FRAMES */
static long g_frame_cap = 0;
static long g_frames = 0;

/* ---- CDP host callbacks (run on the QuickJS thread via cdp_poll) ---- */
static void cdp_on_eval(const char *expr, char *out, size_t cap, void *ud)
{
    MiniApp *app = (MiniApp *)ud;
    mini_bridge_eval_to_json(app->bridge, expr, out, cap);
}
static void cdp_on_dom(char *out, size_t cap, void *ud)
{
    MiniApp *app = (MiniApp *)ud;
    mini_dom_serialize_cdp(app->doc, out, cap);
}
/* console relay: bridge -> CDP Runtime.consoleAPICalled */
static void cdp_log_relay(const char *level, const char *msg, void *ud)
{
    MiniApp *app = (MiniApp *)ud;
    if (app->cdp)
        mini_cdp_emit_log(app->cdp, level, msg);
}

/* latest modifier state (GLFW cursor/scroll callbacks don't receive mods,
   so we track it from the key/button callbacks that do). */
static int g_mods = 0;

/* Input-activity flag for the idle gate: any GLFW input callback (mouse
   move / button / wheel / key / resize) sets this so the host loop renders
   the next frame even if JS/CSS didn't. Without it, hover/scroll/click on a
   page with no rAF and no transitions would be ignored by the idle gate.
   Volatile because the callbacks run on the GLFW event thread of the same
   single-threaded engine; a torn read only costs one extra frame at worst. */
static volatile int g_input_dirty = 1;

/* Map a GLFW physical key to the W3C key/code strings (e.g. 'a'/'KeyA').
   The returned pointers are valid until the next call (static buffers). */
static void glfw_to_keycode(int key, const char **ks, const char **cs)
{
    static char kbuf[16], cbuf[16];
    *ks = "";
    *cs = "";
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z)
    {
        kbuf[0] = (char)('a' + (key - GLFW_KEY_A));
        kbuf[1] = 0;
        cbuf[0] = 'K';
        cbuf[1] = 'e';
        cbuf[2] = 'y';
        cbuf[3] = (char)('A' + (key - GLFW_KEY_A));
        cbuf[4] = 0;
        *ks = kbuf;
        *cs = cbuf;
        return;
    }
    if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9)
    {
        kbuf[0] = (char)('0' + (key - GLFW_KEY_0));
        kbuf[1] = 0;
        cbuf[0] = 'D';
        cbuf[1] = 'i';
        cbuf[2] = 'g';
        cbuf[3] = 'i';
        cbuf[4] = 't';
        cbuf[5] = (char)('0' + (key - GLFW_KEY_0));
        cbuf[6] = 0;
        *ks = kbuf;
        *cs = cbuf;
        return;
    }
#define K(gk, s, c)           \
    if (key == GLFW_KEY_##gk) \
    {                         \
        *ks = s;              \
        *cs = c;              \
        return;               \
    }
    K(SPACE, " ", "Space")
    K(ENTER, "Enter", "Enter")
    K(TAB, "Tab", "Tab")
    K(BACKSPACE, "Backspace", "Backspace")
    K(DELETE, "Delete", "Delete")
    K(ESCAPE, "Escape", "Escape")
    K(LEFT, "ArrowLeft", "ArrowLeft")
    K(RIGHT, "ArrowRight", "ArrowRight")
    K(UP, "ArrowUp", "ArrowUp")
    K(DOWN, "ArrowDown", "ArrowDown")
    K(HOME, "Home", "Home")
    K(END, "End", "End")
    K(PAGE_UP, "PageUp", "PageUp")
    K(PAGE_DOWN, "PageDown", "PageDown")
    K(LEFT_SHIFT, "Shift", "ShiftLeft")
    K(RIGHT_SHIFT, "Shift", "ShiftRight")
    K(LEFT_CONTROL, "Control", "ControlLeft")
    K(RIGHT_CONTROL, "Control", "ControlRight")
        K(LEFT_ALT, "Alt", "AltLeft") K(RIGHT_ALT, "Alt", "AltRight")
            K(LEFT_SUPER, "Meta", "MetaLeft") K(RIGHT_SUPER, "Meta", "MetaRight")
                K(CAPS_LOCK, "CapsLock", "CapsLock")
#undef K
                    *ks = "Unidentified";
    *cs = "Unidentified";
}

/* Multi-window input routing: every GLFW window's user pointer is its
 * MiniWindow*. Each callback recovers it, then derives local aliases for that
 * window's events/doc/bridge/renderer. CDP/diag are primary-only (gated on
 * mw->is_primary) so a secondary window never touches the primary's debugger.
 * g_mods / g_input_dirty stay process-global: the engine is single-threaded
 * and the idle gate only needs to know "did ANY window get input this frame". */
static void glfw_key_cb(GLFWwindow *win, int key, int scancode, int action,
                        int mods)
{
    (void)scancode;
    MiniWindow *mw = (MiniWindow *)glfwGetWindowUserPointer(win);
    if (!mw) return;
    MiniApp *app = mw->app;
    MiniEventState *events = mw->events;
    MiniBridge *bridge = mw->bridge;
    MiniCDP *cdp = mw->is_primary ? app->cdp : NULL;
    MiniDiag *diag = mw->is_primary ? app->diag : NULL;
    g_mods = mods;
    g_input_dirty = 1; /* key events may change :focus / drive input */
    /* F12: toggle the in-engine DevTools overlay (primary only). */
    if (bridge && action == GLFW_PRESS && key == GLFW_KEY_F12 && mw->is_primary)
    {
        printf("open devtools\n");
        mini_devtools_toggle(bridge);
    }
    if (bridge && action == GLFW_PRESS && key == GLFW_KEY_ESCAPE)
    {
        if (mini_bridge_is_pointer_locked(bridge))
            mini_bridge_unlock_pointer(bridge);
    }
    if (diag && (action == GLFW_PRESS || action == GLFW_REPEAT))
        mini_diag_key(diag, key, mods);
    if (events &&
        (action == GLFW_PRESS || action == GLFW_REPEAT || action == GLFW_RELEASE))
    {
        const char *ks, *cs;
        glfw_to_keycode(key, &ks, &cs);
        const char *type = (action == GLFW_RELEASE) ? "keyup" : "keydown";
        mini_events_handle_key(events, type, ks, cs, key, mods,
                               action == GLFW_REPEAT);
        if (action == GLFW_PRESS && ks[0] && !ks[1] && ks[0] != ' ')
            mini_events_handle_key(events, "keypress", ks, cs, key, mods, 0);
        else if (action == GLFW_PRESS && key == GLFW_KEY_SPACE)
            mini_events_handle_key(events, "keypress", " ", "Space", key, mods, 0);
    }
}

static void glfw_char_cb(GLFWwindow *win, unsigned int codepoint)
{
    MiniWindow *mw = (MiniWindow *)glfwGetWindowUserPointer(win);
    if (!mw) return;
    g_input_dirty = 1; /* text input needs a fresh frame to show the char */
    if (mw->events)
        mini_events_handle_char(mw->events, codepoint);
}

#if MINI_MODE_CUSTOM && defined(_WIN32)
static void caret_ime_cb(struct MiniNode *n, float x, float y, float h, void *ud)
{
    (void)n;
    MiniWindow *mw = (MiniWindow *)ud;
    if (!mw || !mw->r)
        return;
    GLFWwindow *win = (GLFWwindow *)mw->r->gpu.window_handle;
    if (!win)
        return;
    HWND hwnd = glfwGetWin32Window(win);
    if (!hwnd)
        return;
    HIMC hImc = ImmGetContext(hwnd);
    if (!hImc)
        return;
    int fw, fh, ww, wh;
    glfwGetFramebufferSize(win, &fw, &fh);
    glfwGetWindowSize(win, &ww, &wh);
    float sx = ww > 0 ? (float)fw / (float)ww : 1.0f;
    float sy = wh > 0 ? (float)fh / (float)wh : 1.0f;
    int cx = (int)(x / sx);
    int cy = (int)(y / sy);
    int ch = (int)(h / sy) + 2;
    COMPOSITIONFORM cf;
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos.x = cx;
    cf.ptCurrentPos.y = cy;
    cf.rcArea.left = cx;
    cf.rcArea.top = cy;
    cf.rcArea.right = cx + 1;
    cf.rcArea.bottom = cy + ch;
    ImmSetCompositionWindow(hImc, &cf);
    CANDIDATEFORM cdf;
    cdf.dwIndex = 0;
    cdf.dwStyle = CFS_CANDIDATEPOS;
    cdf.ptCurrentPos.x = cx;
    cdf.ptCurrentPos.y = cy + ch;
    cdf.rcArea = cf.rcArea;
    ImmSetCandidateWindow(hImc, &cdf);
    ImmReleaseContext(hwnd, hImc);
}
#endif

#if MINI_MODE_CUSTOM
static void copy_clipboard_cb(MiniEventState *st, const char *text, void *ud)
{
    (void)st;
    MiniWindow *mw = (MiniWindow *)ud;
    if (!mw || !mw->r || !mw->r->gpu.window_handle || !text || !text[0])
        return;
    glfwSetClipboardString((GLFWwindow *)mw->r->gpu.window_handle, text);
}
#endif

static void glfw_win_to_fb(GLFWwindow *win, double wx, double wy,
                           float *fx, float *fy)
{
    int fw, fh, ww, wh;
    glfwGetFramebufferSize(win, &fw, &fh);
    glfwGetWindowSize(win, &ww, &wh);
    float sx = ww > 0 ? (float)fw / (float)ww : 1.0f;
    float sy = wh > 0 ? (float)fh / (float)wh : 1.0f;
    *fx = (float)wx * sx;
    *fy = (float)wy * sy;
}

static GLFWcursor *g_cursor_arrow = NULL;
static GLFWcursor *g_cursor_ibeam = NULL;
static GLFWcursor *g_cursor_hand = NULL;
static GLFWcursor *g_cursor_crosshair = NULL;

static void update_cursor_icon(GLFWwindow *win, MiniWindow *mw, float fx, float fy)
{
    if (!win || !mw || !mw->doc) return;
    if (!g_cursor_arrow) g_cursor_arrow = glfwCreateStandardCursor(GLFW_ARROW_CURSOR);
    if (!g_cursor_ibeam) g_cursor_ibeam = glfwCreateStandardCursor(GLFW_IBEAM_CURSOR);
    if (!g_cursor_hand) g_cursor_hand = glfwCreateStandardCursor(GLFW_HAND_CURSOR);
    if (!g_cursor_crosshair) g_cursor_crosshair = glfwCreateStandardCursor(GLFW_CROSSHAIR_CURSOR);

    int fw = mw->r ? mw->r->gpu.width : 1280;
    if (fx >= (float)fw - 14.0f && mw->doc->max_scroll_y > 0.0f)
    {
        glfwSetCursor(win, g_cursor_arrow);
        return;
    }

    struct MiniNode *n = mini_dom_hit_test_doc(mw->doc, fx, fy);
    if (!n)
    {
        glfwSetCursor(win, g_cursor_arrow);
        return;
    }

    if (n->style.cursor == 1) { glfwSetCursor(win, g_cursor_hand); return; }
    if (n->style.cursor == 2) { glfwSetCursor(win, g_cursor_ibeam); return; }
    if (n->style.cursor == 6) { glfwSetCursor(win, g_cursor_crosshair); return; }
    if (n->style.cursor == 3 || n->style.cursor == 4) { glfwSetCursor(win, g_cursor_hand); return; }

    for (struct MiniNode *cur = n; cur; cur = cur->parent)
    {
        if (cur->tag)
        {
            if (!strcmp(cur->tag, "a") || !strcmp(cur->tag, "button") || !strcmp(cur->tag, "select") || !strcmp(cur->tag, "summary"))
            {
                glfwSetCursor(win, g_cursor_hand);
                return;
            }
            if (!strcmp(cur->tag, "input"))
            {
                const char *t = mini_node_get_attribute(cur, "type");
                if (t && (!strcmp(t, "button") || !strcmp(t, "submit") || !strcmp(t, "reset") || !strcmp(t, "checkbox") || !strcmp(t, "radio") || !strcmp(t, "range") || !strcmp(t, "color") || !strcmp(t, "file")))
                {
                    glfwSetCursor(win, g_cursor_hand);
                    return;
                }
                glfwSetCursor(win, g_cursor_ibeam);
                return;
            }
            if (!strcmp(cur->tag, "textarea"))
            {
                glfwSetCursor(win, g_cursor_ibeam);
                return;
            }
        }
        if (mini_node_get_attribute(cur, "onclick") || mini_node_get_attribute(cur, "role"))
        {
            const char *role = mini_node_get_attribute(cur, "role");
            if (mini_node_get_attribute(cur, "onclick") || (role && !strcmp(role, "button")))
            {
                glfwSetCursor(win, g_cursor_hand);
                return;
            }
        }
    }

    int is_text = (n->type == MN_TEXT_NODE) || (n->text && n->text[0]);
    if (!is_text && n->tag)
    {
        if (!strcmp(n->tag, "p") || !strcmp(n->tag, "h1") || !strcmp(n->tag, "h2") ||
            !strcmp(n->tag, "h3") || !strcmp(n->tag, "h4") || !strcmp(n->tag, "h5") ||
            !strcmp(n->tag, "h6") || !strcmp(n->tag, "span") || !strcmp(n->tag, "code") ||
            !strcmp(n->tag, "pre") || !strcmp(n->tag, "blockquote") || !strcmp(n->tag, "li") ||
            !strcmp(n->tag, "td") || !strcmp(n->tag, "th") || !strcmp(n->tag, "em") ||
            !strcmp(n->tag, "strong") || !strcmp(n->tag, "b") || !strcmp(n->tag, "i") ||
            !strcmp(n->tag, "label"))
        {
            is_text = 1;
        }
    }

    if (is_text)
    {
        int unselectable = 0;
        for (struct MiniNode *cur = n; cur; cur = cur->parent)
        {
            if (cur->style.user_select == 1) /* user-select: none */
            {
                unselectable = 1;
                break;
            }
            const char *ah = mini_node_get_attribute(cur, "aria-hidden");
            if (ah && !strcmp(ah, "true"))
            {
                unselectable = 1;
                break;
            }
            if (cur->tag && (!strcmp(cur->tag, "button") || !strcmp(cur->tag, "nav")))
            {
                unselectable = 1;
                break;
            }
        }
        if (!unselectable)
        {
            glfwSetCursor(win, g_cursor_ibeam);
            return;
        }
    }

    glfwSetCursor(win, g_cursor_arrow);
}

static void glfw_cursor_cb(GLFWwindow *win, double x, double y)
{
    MiniWindow *mw = (MiniWindow *)glfwGetWindowUserPointer(win);
    if (!mw) return;
    MiniApp *app = mw->app;
    MiniCDP *cdp = mw->is_primary ? app->cdp : NULL;
    float fx, fy;
    glfw_win_to_fb(win, x, y, &fx, &fy);

    update_cursor_icon(win, mw, fx, fy);

    if (cdp && mini_cdp_is_inspect_mode(cdp) && mw->doc)
    {
        mini_dom_assign_node_ids(mw->doc);
        struct MiniNode *n = mini_dom_hit_test_doc(mw->doc, fx, fy);
        if (n && n->cdp_node_id > 0)
        {
            mini_cdp_highlight_node(cdp, n->cdp_node_id);
            mw->doc->dirty = 1;
        }
    }

    if (mw->events)
    {
        g_input_dirty = 1; /* hover tracking needs a fresh frame */
        mini_events_handle_mouse_move(mw->events, fx, fy, g_mods);
    }
}

static void glfw_mouse_cb(GLFWwindow *win, int button, int action, int mods)
{
    MiniWindow *mw = (MiniWindow *)glfwGetWindowUserPointer(win);
    if (!mw) return;
    MiniApp *app = mw->app;
    MiniCDP *cdp = mw->is_primary ? app->cdp : NULL;
    double x, y;
    glfwGetCursorPos(win, &x, &y);
    g_mods = mods;
    float fx, fy;
    glfw_win_to_fb(win, x, y, &fx, &fy);

    if (cdp && mini_cdp_is_inspect_mode(cdp) && action == GLFW_PRESS && button == GLFW_MOUSE_BUTTON_LEFT)
    {
        if (mw->doc)
        {
            mini_dom_assign_node_ids(mw->doc);
            struct MiniNode *n = mini_dom_hit_test_doc(mw->doc, fx, fy);
            if (n && n->cdp_node_id > 0)
            {
                mini_cdp_inspect_node(cdp, n->cdp_node_id);
                mw->doc->dirty = 1;
                return; /* consume click for inspection */
            }
        }
    }

    if (mw->events)
    {
        g_input_dirty = 1; /* click/press may change :active/:focus */
        mini_events_handle_mouse_button(mw->events, button, action, fx, fy, mods);
    }
}

static void glfw_scroll_cb(GLFWwindow *win, double dx, double dy)
{
    MiniWindow *mw = (MiniWindow *)glfwGetWindowUserPointer(win);
    if (!mw || !mw->events)
        return;
    double x, y;
    glfwGetCursorPos(win, &x, &y);
    g_input_dirty = 1; /* wheel scrolls the page */
    float fx, fy;
    glfw_win_to_fb(win, x, y, &fx, &fy);
    mini_events_handle_wheel(mw->events, fx, fy, (float)dx, (float)dy, g_mods);
}

static void glfw_drop_cb(GLFWwindow *win, int count, const char **paths)
{
    MiniWindow *mw = (MiniWindow *)glfwGetWindowUserPointer(win);
    if (!mw || !mw->events || count <= 0)
        return;
    double x, y;
    glfwGetCursorPos(win, &x, &y);
    g_input_dirty = 1;
    float fx, fy;
    glfw_win_to_fb(win, x, y, &fx, &fy);
    mini_events_handle_drop_files(mw->events, paths, count, fx, fy);
}

static void glfw_fb_size_cb(GLFWwindow *win, int w, int h)
{
    MiniWindow *mw = (MiniWindow *)glfwGetWindowUserPointer(win);
    if (!mw || !mw->events)
        return;
    g_input_dirty = 1; /* resize relayouts the page */
    mini_events_handle_resize(mw->events, w, h);
    /* dispatch the BrowserWindow 'resize' JS event (safe: GLFW callbacks fire
       during glfwPollEvents, when no JS is executing) */
    if (!mw->is_primary)
        mini_window_emit_event(mw, "resize");
}

static void glfw_window_focus_cb(GLFWwindow *win, int focused)
{
    MiniWindow *mw = (MiniWindow *)glfwGetWindowUserPointer(win);
    if (!mw || !mw->events)
        return;
    if (!focused)
    {
        mini_events_release_capture(mw->events);
        g_input_dirty = 1;
    }
}

static void glfw_cursor_enter_cb(GLFWwindow *win, int entered)
{
    MiniWindow *mw = (MiniWindow *)glfwGetWindowUserPointer(win);
    if (!mw || !mw->events)
        return;
    if (!entered)
    {
        mini_events_release_capture(mw->events);
        g_input_dirty = 1;
    }
}

static void gesture_cb(MiniEventState *st, const char *action_js, void *ud)
{
    (void)st;
    MiniWindow *mw = (MiniWindow *)ud;
    if (mw && mw->bridge && action_js && action_js[0])
    {
        mini_bridge_eval(mw->bridge, action_js, strlen(action_js), "<gesture>");
    }
}

/* Install the GLFW input callbacks + per-window user pointer on a window so
 * input routes to its MiniWindow (and its events/doc/bridge). Also wires the
 * event-state side callbacks (caret IME / copy / gesture) with the MiniWindow
 * as userdata. Called for both the primary (from mini_app_create via
 * mini_window_register_primary) and every secondary window. */
void mini_window_install_callbacks(MiniWindow *mw)
{
    if (!mw || !mw->win)
        return;
    GLFWwindow *win = (GLFWwindow *)mw->win;
    glfwSetWindowUserPointer(win, mw);
    glfwSetKeyCallback(win, glfw_key_cb);
    glfwSetCharCallback(win, glfw_char_cb);
    glfwSetCursorPosCallback(win, glfw_cursor_cb);
    glfwSetMouseButtonCallback(win, glfw_mouse_cb);
    glfwSetScrollCallback(win, glfw_scroll_cb);
    glfwSetFramebufferSizeCallback(win, glfw_fb_size_cb);
    glfwSetWindowFocusCallback(win, glfw_window_focus_cb);
    glfwSetCursorEnterCallback(win, glfw_cursor_enter_cb);
    glfwSetDropCallback(win, glfw_drop_cb);
    if (mw->events)
    {
#if MINI_MODE_CUSTOM && defined(_WIN32)
        mini_events_set_caret_cb(mw->events, caret_ime_cb, mw);
#endif
#if MINI_MODE_CUSTOM
        mini_events_set_copy_cb(mw->events, copy_clipboard_cb, mw);
#endif
        mini_events_set_gesture_cb(mw->events, gesture_cb, mw);
    }
}

/* Load the default system + local TrueType font chain onto a renderer so any
 * window renders anti-aliased text + emoji. Extracted from the old primary
 * init so secondary windows reuse the exact same chain. */
void mini_window_load_default_fonts(MiniRenderer *r)
{
    if (!r)
        return;
    char win_dir[512] = {0};
#if defined(_WIN32)
    if (!GetWindowsDirectoryA(win_dir, sizeof(win_dir)))
    {
        const char *w = getenv("WINDIR");
        if (w)
            snprintf(win_dir, sizeof(win_dir), "%s", w);
        else
        {
            const char *drv = getenv("SystemDrive");
            snprintf(win_dir, sizeof(win_dir), "%s/Windows", drv ? drv : "");
        }
    }
#endif
    for (char *p = win_dir; *p; p++) if (*p == '\\') *p = '/';

    const char *pri_names[] = {
        "msyh.ttc", "msyh.ttf", "simhei.ttf", "simsun.ttc", "arial.ttf"
    };
    const char *local_cands[] = {
        "AiDianFengYaHeiChangTi.ttf",
        "build/AiDianFengYaHeiChangTi.ttf",
        "assets/font.ttf"
    };

    int pri_loaded = 0;
    if (win_dir[0])
    {
        char pbuf[576];
        for (size_t i = 0; i < sizeof(pri_names) / sizeof(pri_names[0]); i++)
        {
            snprintf(pbuf, sizeof(pbuf), "%s/Fonts/%s", win_dir, pri_names[i]);
            if (mini_renderer_load_font(r, pbuf) == 0)
            {
                pri_loaded = 1;
                break;
            }
        }
    }
    if (!pri_loaded)
    {
        for (size_t i = 0; i < sizeof(local_cands) / sizeof(local_cands[0]); i++)
        {
            if (mini_renderer_load_font(r, local_cands[i]) == 0)
            {
                pri_loaded = 1;
                break;
            }
        }
    }
    if (win_dir[0])
    {
        char pbuf[576];
        snprintf(pbuf, sizeof(pbuf), "%s/Fonts/seguiemj.ttf", win_dir);
        mini_renderer_load_fallback_font(r, pbuf);
    }
}

MiniResult mini_app_create(const MiniWindowConfig *cfg, MiniApp **out)
{
    if (!cfg || !out)
        return MINI_ERR_INIT;
    MiniApp *app = (MiniApp *)calloc(1, sizeof(*app));
    if (!app)
        return MINI_ERR_INIT;
    app->cfg = *cfg;

    /* 1) GPU context + self-written renderer */
    app->r = mini_renderer_create(cfg->width, cfg->height,
                                  cfg->sample_count, cfg->vsync);
    if (!app->r)
    {
        free(app);
        return MINI_ERR_GPU;
    }

    /* 1b) Load the default TrueType/OpenType font chain (system fonts + local
       fallbacks + emoji) so text renders anti-aliased with CJK + emoji
       support. Shared with secondary windows via mini_window_load_default_fonts. */
    mini_window_load_default_fonts(app->r);

    /* 2) DOM document (self-written scene graph) */
    app->doc = mini_doc_create();
    if (!app->doc)
    {
        mini_renderer_destroy(app->r);
        free(app);
        return MINI_ERR_INIT;
    }

    /* 3) QuickJS + DOM/BOM/WebGL polyfill bridge */
    app->bridge = mini_bridge_create(app->r, app->doc);
    if (!app->bridge)
    {
        mini_doc_destroy(app->doc);
        mini_renderer_destroy(app->r);
        free(app);
        return MINI_ERR_JS;
    }
    /* Wire the host (this MiniApp) onto the main bridge so the BrowserWindow
       constructor (which runs in this main context) can reach the host to
       create secondary windows via mini_window_create_secondary. */
    mini_bridge_set_host(app->bridge, app);

    /* Create the process-wide IPC registry and install ipcMain on this (main)
       context. Each secondary renderer gets ipcRenderer installed in
       mini_window_create_secondary. */
    app->ipc = mini_ipc_create();
    if (app->ipc)
        mini_ipc_install(app->ipc, app->bridge, 1, 0);

    /* Create the custom-scheme protocol registry and install the `protocol`
       JS surface on this main context. Consulted by BrowserWindow.loadURL. */
    app->proto = mini_protocol_create();
    if (app->proto)
        mini_protocol_install(app->proto, app);

    /* diagnostics (no CDP yet �?enable_cdp attaches it) */
    app->diag = mini_diag_start(app->bridge, app->r, app->doc, NULL);
    /* W3C event system: hit-test + capture/target/bubble dispatch, driven by
       the GLFW callbacks registered below. The bridge (Stage 3) registers JS
       listeners into this state; :hover/:active/:focus work even pre-JS. */
    app->events = mini_events_state_create(app->doc);
    /* attach so JS addEventListener registers into app->events (the same
       state the host loop drives). Must run after both exist. */
    mini_bridge_set_events(app->bridge, app->events);
    /* Install the in-engine DevTools bundle (toggled by F12) into the bridge's
       JS context. Defines window.__miniDT; the panel DOM is built on open(). */
    mini_devtools_install(app->bridge);

    /* Register the primary window (id 0) as a MiniWindow wrapping r/doc/bridge/
       events, and install all GLFW input callbacks + the per-window user
       pointer so input routes to this window (and, for future secondary
       windows, to each one's own MiniWindow). */
    mini_window_register_primary(app, app->r, app->doc, app->bridge, app->events);

    *out = app;
    return MINI_OK;
}

MiniResult mini_app_load(MiniApp *app, const char *entry)
{
    if (!app || !entry)
        return MINI_ERR_ASSET;
    size_t len = 0;
    char *src = read_file(entry, &len);
    if (!src)
        return MINI_ERR_ASSET;
    int rc;
    mini_bridge_set_doc_url(app->bridge, entry); /* base URL for module specifiers */
    free(app->page_url);
    free(app->page_source);
    app->page_url = _strdup(entry);
    app->page_source = _strdup(src);

    size_t nlen = strlen(entry);
    if (nlen > 5 && !strcmp(entry + nlen - 5, ".html"))
        rc = mini_bridge_load_html(app->bridge, src); /* Phase 1: raw HTML */
    else
        rc = mini_bridge_eval(app->bridge, src, len, entry);
    free(src);
    app->loaded = (rc == 0);

    if (app->cdp)
    {
        MiniCDPHost host = {
            .bridge = app->bridge,
            .doc = app->doc,
            .renderer = app->r,
            .diag = app->diag,
            .events = app->events,
            .url = app->page_url,
            .page_source = app->page_source
        };
        mini_cdp_attach_host(app->cdp, &host);
    }
    return rc == 0 ? MINI_OK : MINI_ERR_JS;
}

MiniResult mini_app_load_encrypted(MiniApp *app,
                                   const uint8_t *bundle, size_t size,
                                   const uint8_t key[32])
{
    if (!app || !bundle)
        return MINI_ERR_ASSET;
    /* Loading from an encrypted VFS bundle => the app is "packaged"
       (app.isPackaged returns true). */
    mini_native_set_packaged(1);
    uint8_t *pt = NULL;
    size_t pt_len = 0;
    /* AAD = the 4-byte magic tag so a bundle swapped between source/bytecode
       modes fails authentication. (Empty here for a single-mode bundle.) */
    if (mini_vfs_decrypt(bundle, size, key, NULL, 0, &pt, &pt_len) != 0)
        return MINI_ERR_ASSET; /* tag mismatch / tampered */
    int rc;
    if (pt_len >= 4 && memcmp(pt, MINI_BC_MARKER, 4) == 0)
    {
        /* QuickJS bytecode payload */
        rc = mini_vfs_eval_bytecode(app->bridge, pt + 4, pt_len - 4);
    }
    else
    {
        /* plain JS source */
        rc = mini_bridge_eval(app->bridge, (const char *)pt, pt_len, "<vfs>");
    }
    /* zero the decrypted payload before freeing (defense in depth) */
    memset(pt, 0, pt_len);
    free(pt);
    app->loaded = (rc == 0);
    return rc == 0 ? MINI_OK : MINI_ERR_JS;
}

MiniResult mini_app_enable_cdp(MiniApp *app, uint16_t port)
{
    if (!app)
        return MINI_ERR_INIT;
    MiniCDPCallbacks cb = {.on_eval = cdp_on_eval, .on_dom = cdp_on_dom, .ud = app};
    app->cdp = mini_cdp_start(port, &cb);
    if (!app->cdp)
        return MINI_ERR_INIT;
    /* attach the live engine so the full CDP domain set (Runtime reflection,
       Debugger, DOM node-id addressing, Page screenshot, Overlay highlight)
       is backed by real data — not just the legacy on_eval/on_dom callbacks. */
    MiniCDPHost host = {
        .bridge = app->bridge,
        .doc = app->doc,
        .renderer = app->r,
        .diag = app->diag,
        .events = app->events,
        .url = app->page_url,
        .page_source = app->page_source
    };
    mini_cdp_attach_host(app->cdp, &host);
    /* attach CDP to diagnostics + relay console.* from the bridge */
    if (app->diag)
        mini_diag_set_cdp(app->diag, app->cdp);
    mini_bridge_set_log_hook(app->bridge, cdp_log_relay, app);
    return MINI_OK;
}

MiniResult mini_app_run(MiniApp *app)
{
    if (!app || !app->r)
        return MINI_ERR_INIT;
    GLFWwindow *win = (GLFWwindow *)app->r->gpu.window_handle;

    /* target frame interval for the 60 fps ceiling + the idle wait. When
       vsync is on, glfwSwapBuffers already throttles to the refresh rate;
       when it is off (or the driver ignores it), this keeps the loop from
       spinning at 100% CPU. The idle path uses it as the glfwWaitEventsTimeout
       so timers/rAF/CSS effects still wake us at ~60Hz while a static page
       sleeps instead of busy-spinning. */
    const double target_frame = 1.0 / 60.0;
    double last_time = glfwGetTime();
    double last_swap = last_time;
    int last_emu_w = -1, last_emu_h = -1;

    mini_layout_run(app->doc, app->r->gpu.width, app->r->gpu.height);

    while (!glfwWindowShouldClose(win))
    {
        glfwPollEvents();

        /* The primary is the active window at the top of each iteration: switch
           the process-wide active doc/bridge to the primary so the primary
           viewport/tick/pump/render phases below operate on the primary's
           document. (Secondary windows switch these per-window during their
           own render phase, then restore the primary here next iteration.) */
        mini_dom_set_active_doc(app->doc);
        mini_bridge_set_active(app->bridge);

        /* Service CDP every iteration (cheap, non-blocking) so a static page
           still answers chrome://inspect / Runtime.evaluate — the idle gate
           below would otherwise skip the post-render poll. */
        if (app->cdp)
            mini_cdp_poll(app->cdp);

        /* DevTools Device toolbar (Emulation.setDeviceMetricsOverride): if
           an override is active, resize the window to match. */
        int emu_w = -1, emu_h = -1;
        int emu_active = 0;
        if (app->cdp)
            emu_active = mini_cdp_emulation_viewport(app->cdp, &emu_w, &emu_h);
        if (emu_active &&
            (emu_w != last_emu_w || emu_h != last_emu_h) &&
            (emu_w > 0 && emu_h > 0) &&
            (emu_w != app->r->gpu.width || emu_h != app->r->gpu.height))
            glfwSetWindowSize(win, emu_w, emu_h);
        last_emu_w = emu_w;
        last_emu_h = emu_h;

        int fw, fh;
        glfwGetFramebufferSize(win, &fw, &fh);
        int viewport_changed = (fw != app->r->gpu.width || fh != app->r->gpu.height);
        if (viewport_changed)
        {
            app->r->gpu.width = fw;
            app->r->gpu.height = fh;
            app->r->vbuf.width = fw;
            app->r->vbuf.height = fh;
            if (app->events)
                mini_events_handle_resize(app->events, fw, fh);
            else if (app->doc)
            {
                app->doc->viewport_w = fw;
                app->doc->viewport_h = fh;
                app->doc->dirty = 1;
                app->doc->layout_dirty = 1;
            }
        }

        double now = glfwGetTime();
        double dt = now - last_time;
        if (dt < 0.0)
            dt = 0.0;
        else if (dt > 0.25)
            dt = 0.25; /* clamp after a stall (debugger / suspend) */
        last_time = now;

        /* Advance CSS transitions / @keyframes for the elapsed wall-clock dt */
        mini_dom_tick_frame(app->doc, dt);

        /* Drain JS microtasks/macrotasks even on an "idle" frame so that
           setTimeout / Promise.then keep firing (a callback may mutate the
           DOM or call requestAnimationFrame, flipping the gate to render
           on the next iteration). Cheap when the queue is empty. */
        mini_bridge_pump(app->bridge);
        if (app->ipc)
            mini_ipc_dispatch(app->ipc, app->bridge, 0); /* drain renderer→main */
        /* pump any second-instance relays (emit 'second-instance' to app.on) */
        mini_app_pump_second_instance(app);
        /* pump completed net.request fetches (emit response/data/end) */
        mini_net_api_pump(app);

        /* Multi-window: pump every secondary window's bridge too, so a static
           primary still services secondary setTimeout / Promise / IPC. */
        {
            int nsec = 0;
            MiniWindow **secs = mini_app_windows(app, &nsec);
            for (int i = 0; i < nsec; i++)
                if (secs[i] && !secs[i]->is_primary)
                {
                    mini_window_pump(secs[i]);
                    if (app->ipc)
                        mini_ipc_dispatch(app->ipc, secs[i]->bridge, secs[i]->id);
                }
        }

        /* ---- idle gate ------------------------------------------------
           Skip the expensive restyle + layout + render + flush + swap
           pipeline when nothing needs a new frame. This is the fix for the
           old behaviour of doing a full-pipeline spin every frame even on
           a fully static page (the dominant CPU drain). We still tick
           effects + pump jobs above, so timers/anim stay responsive. */
        int sec_need = 0; /* any secondary window needs a frame this iter? */
        {
            int nsec = 0;
            MiniWindow **secs = mini_app_windows(app, &nsec);
            for (int i = 0; i < nsec; i++)
                if (mini_window_needs_frame(secs[i])) { sec_need = 1; break; }
        }
        int need_frame = g_input_dirty || viewport_changed ||
                         mini_bridge_pending_raf(app->bridge) > 0 ||
                         app->doc->dirty || app->doc->active_effects ||
                         emu_active || /* an active CDP metrics override */
                         (g_frame_cap && g_frames < g_frame_cap) ||
                         mini_events_has_text_focus(app->events) ||
                         sec_need ||
                         (app->cdp && (mini_cdp_has_overlay(app->cdp) || mini_cdp_is_inspect_mode(app->cdp))); /* caret blink or CDP overlay/inspect */
        if (!need_frame)
        {
            /* Block until input or ~one frame elapses. On a static page the
               loop sleeps here (near-0% CPU) instead of re-rendering
               identical frames at full tilt. */
            if (app->cdp)
                mini_cdp_poll(app->cdp);
            glfwWaitEventsTimeout(target_frame);
            if (app->cdp)
                mini_cdp_poll(app->cdp);
            continue;
        }

        if (app->diag)
            mini_diag_begin_frame(app->diag);
        double t = app->diag ? mini_diag_mark(app->diag) : 0;

        if (app->doc->dirty || g_input_dirty || viewport_changed)
        {
            mini_dom_restyle(app->doc);
        }
        g_input_dirty = 0;

        /* layout: read (possibly mutated) DOM, write geometry.
           Gated on doc->dirty (restyle/mutation) OR doc->layout_dirty
           (a layout-affecting style change). Pure paint-only animation
           frames (opacity blink, transform, background pulse) skip this
           entirely — previously layout ran every frame unconditionally,
           re-parsing grid templates and double-laying children at 60 Hz. */
        if (app->doc->dirty || app->doc->layout_dirty)
            mini_layout_run(app->doc, fw, fh);
        if (app->diag)
            mini_diag_section(app->diag, &t, MINI_DIAG_LAYOUT);

        /* 1. Clear background & render page backdrop (solid, gradient, image) */
        mini_renderer_begin_frame(app->r);
        mini_dom_render_page_backdrop(app->doc, app->r, (float)fw, (float)fh);
        mini_renderer_flush(app->r);

        /* 2. JS frame (rAF): WebGL/Three.js draws into canvas region */
        mini_renderer_restore_webgl(app->r);
        mini_bridge_fire_raf(app->bridge, now * 1000.0);
        if (app->diag)
            mini_diag_section(app->diag, &t, MINI_DIAG_RAF);

        /* 3. DOM render pass: Renders HTML/CSS elements & overlays ON TOP of WebGL */
        mini_renderer_begin_frame(app->r);
        mini_dom_set_render_events(app->events);
        mini_dom_render_into(app->doc->body, app->r);
        mini_renderer_flush(app->r);
        if (app->diag)
            mini_diag_section(app->diag, &t, MINI_DIAG_DRAW);

        /* DevTools Elements Box Model Highlight Overlay (rendered on the very TOP layer) */
        if (app->cdp && mini_cdp_has_overlay(app->cdp))
        {
            float hx = 0, hy = 0, hw = 0, hh = 0;
            float pad[4] = {0}, marg[4] = {0};
            char tag[64] = {0}, id_str[64] = {0}, cls[128] = {0};
            if (mini_cdp_overlay_box_ex(app->cdp, &hx, &hy, &hw, &hh, pad, marg, tag, sizeof tag, id_str, sizeof id_str, cls, sizeof cls))
            {
                /* 1. Margin box (amber fill) */
                if (marg[0] > 0 || marg[1] > 0 || marg[2] > 0 || marg[3] > 0)
                {
                    mini_draw_rect(app->r,
                                   hx - marg[3], hy - marg[0],
                                   hw + marg[1] + marg[3], hh + marg[0] + marg[2],
                                   0.965f, 0.698f, 0.420f, 0.35f);
                }
                /* 2. Padding box (green fill) */
                if (pad[0] > 0 || pad[1] > 0 || pad[2] > 0 || pad[3] > 0)
                {
                    mini_draw_rect(app->r, hx, hy, hw, hh, 0.576f, 0.769f, 0.490f, 0.40f);
                }
                /* 3. Content box (glowing DevTools blue fill) */
                float cx = hx + pad[3];
                float cy = hy + pad[0];
                float cw = hw - pad[1] - pad[3];
                float ch = hh - pad[0] - pad[2];
                if (cw > 0 && ch > 0)
                {
                    mini_draw_rect(app->r, cx, cy, cw, ch, 0.435f, 0.659f, 0.863f, 0.66f);
                }
                else
                {
                    mini_draw_rect(app->r, hx, hy, hw, hh, 0.435f, 0.659f, 0.863f, 0.66f);
                }
                /* 4. Glowing solid border outline */
                mini_draw_rect_stroke(app->r, hx, hy, hw, hh, 2.0f, 0.2f, 0.5f, 0.98f, 1.0f);

                /* 5. Tooltip badge with tag name, #id, .class and dimensions */
                char badge[256];
                if (id_str[0])
                    snprintf(badge, sizeof badge, "%s#%s | %.0f x %.0f", tag, id_str, hw, hh);
                else if (cls[0])
                    snprintf(badge, sizeof badge, "%s.%s | %.0f x %.0f", tag, cls, hw, hh);
                else
                    snprintf(badge, sizeof badge, "%s | %.0f x %.0f", tag, hw, hh);

                float badge_w = mini_text_measure(badge, 12.0f) + 12.0f;
                float badge_h = 20.0f;
                float badge_x = hx;
                float badge_y = (hy - badge_h - 4.0f >= 0) ? (hy - badge_h - 4.0f) : (hy + hh + 4.0f);
                mini_draw_rect_rounded(app->r, badge_x, badge_y, badge_w, badge_h, 3.0f, 0.15f, 0.15f, 0.18f, 0.92f);
                mini_draw_text(app->r, badge_x + 6.0f, badge_y + 4.0f, badge, 12.0f, 1.0f, 1.0f, 1.0f, 1.0f);

                mini_renderer_flush(app->r);
            }
        }

        /* 6. Vertical Scrollbar Slider (滑块) */
        if (app->doc && app->doc->max_scroll_y > 0.0f)
        {
            int fw = app->r ? app->r->gpu.width : 1280;
            int fh = app->r ? app->r->gpu.height : 800;
            float vw = (float)fw;
            float vh = (float)fh;
            float track_x = vw - 8.0f;
            float total_h = app->doc->max_scroll_y + vh;
            float thumb_h = vh * (vh / total_h);
            if (thumb_h < 36.0f) thumb_h = 36.0f;
            if (thumb_h > vh - 10.0f) thumb_h = vh - 10.0f;
            float thumb_y = (app->doc->scroll_y / app->doc->max_scroll_y) * (vh - thumb_h);
            float radii[4] = {3.0f, 3.0f, 3.0f, 3.0f};

            MiniScrollbarState sst = mini_events_get_scrollbar_state(app->events);
            float alpha = 0.40f;
            float thumb_w = 6.0f;
            if (sst == MINI_SCROLLBAR_DRAGGING)
            {
                alpha = 0.85f;
                thumb_w = 8.0f;
                track_x = vw - 9.0f;
            }
            else if (sst == MINI_SCROLLBAR_HOVER)
            {
                alpha = 0.65f;
                thumb_w = 7.0f;
                track_x = vw - 8.5f;
            }
            mini_draw_rect_rounded_corners(app->r, track_x, thumb_y, thumb_w, thumb_h, radii, 1.0f, 1.0f, 1.0f, alpha);
            mini_renderer_flush(app->r);
        }

        /* 7. Mouse Gesture Visual Trail & Action Badge */
        MiniGestureState gest;
        if (app->events && mini_events_get_gesture(app->events, &gest) && gest.active && gest.num_points >= 2)
        {
            for (int i = 0; i < gest.num_points - 1; i++)
            {
                mini_draw_line(app->r, gest.points[i][0], gest.points[i][1],
                               gest.points[i+1][0], gest.points[i+1][1],
                               3.5f, 0.0f, 0.85f, 1.0f, 0.90f);
            }
            if (gest.action_name)
            {
                float bx = gest.points[gest.num_points - 1][0] + 16.0f;
                float by = gest.points[gest.num_points - 1][1] - 12.0f;
                float tw = mini_text_measure(gest.action_name, 14.0f);
                float radii[4] = {6.0f, 6.0f, 6.0f, 6.0f};
                mini_draw_rect_rounded_corners(app->r, bx - 8.0f, by - 6.0f, tw + 16.0f, 28.0f, radii, 0.08f, 0.10f, 0.15f, 0.92f);
                mini_draw_rect_stroke(app->r, bx - 8.0f, by - 6.0f, tw + 16.0f, 28.0f, 1.0f, 0.0f, 0.85f, 1.0f, 0.70f);
                mini_draw_text(app->r, bx, by + 2.0f, gest.action_name, 14.0f, 1.0f, 1.0f, 1.0f, 1.0f);
            }
            mini_renderer_flush(app->r);
        }

        mini_renderer_end_frame(app->r); /* swap (incl. WebGL) */
        mini_bridge_pump(app->bridge);   /* drain rAF-queued microtasks */

        /* Multi-window: render each open secondary window in its own GL context
           (the secondary path switches the active doc/bridge/renderer to that
           window), then restore the primary context + active globals so the
           next iteration's primary phase is correct. */
        {
            int nsec = 0;
            MiniWindow **secs = mini_app_windows(app, &nsec);
            for (int i = 0; i < nsec; i++)
            {
                MiniWindow *mw = secs[i];
                if (!mw || mw->is_primary || mw->closing)
                    continue;
                mini_window_render_frame(mw, now * 1000.0, dt);
            }
            glfwMakeContextCurrent(win);
            mini_dom_set_active_doc(app->doc);
            mini_bridge_set_active(app->bridge);
        }
        /* destroy secondary windows that closed this frame (compact the list) */
        mini_window_sweep_closed(app);

        /* this frame's accumulated dirty has been consumed by the render */
        app->doc->dirty = 0;

        if (app->diag)
            mini_diag_end_frame(app->diag);
        if (app->cdp)
            mini_cdp_poll(app->cdp);

        if (g_frame_cap && ++g_frames >= g_frame_cap)
        {
            /* debug: dump a screenshot before exiting so we can see what
               actually rendered (the diag counters are unreliable).       */
            uint8_t *png = NULL;
            size_t plen = 0;
            if (mini_renderer_screenshot_png(app->r, &png, &plen) == 0 && png)
            {
                FILE *sf = fopen("build/shot.png", "wb");
                if (sf)
                {
                    fwrite(png, 1, plen, sf);
                    fclose(sf);
                }
                free(png);
            }
            glfwSetWindowShouldClose(win, GLFW_TRUE);
            break;
        }

        /* 60 fps ceiling: if vsync is off or the driver isn't limiting,
           sleep the remainder so we never spin faster than 60 fps. When vsync
           is ON, glfwSwapBuffers in mini_renderer_end_frame already hardware-
           throttles to the display refresh rate. */
        if (!app->cfg.vsync)
        {
            double after = glfwGetTime();
            double elapsed = after - last_swap;
            last_swap = after;
            if (elapsed < target_frame)
            {
                double rem = target_frame - elapsed;
                if (rem > 0.0005)
                    glfwWaitEventsTimeout(rem);
            }
        }
    }

    /* App lifecycle: the run loop is exiting (primary closed / app.quit()).
       Emit 'before-quit' then 'window-all-closed' (no open windows remain).
       before-quit is advisory here (not preventable in this build). */
    if (!app->quitting)
    {
        app->quitting = 1;
        mini_app_emit_event(app, "before-quit");
        mini_app_emit_event(app, "window-all-closed");
    }
    return MINI_OK;
}

extern void mini_audio_shutdown(void);

void mini_app_destroy(MiniApp *app)
{
    if (!app)
        return;
    mini_audio_shutdown();
    /* Join any in-flight parallel prefetch threads + close the keep-alive pool
       ONCE (process-global). Must run before any bridge is destroyed so cache
       writes land; was previously per-bridge, which deadlocked under multi-window. */
    mini_net_prefetch_shutdown();
    /* Drain any completed-but-not-yet-pumped net.request results so their
       ClientRequest JS refs are freed before the runtime is torn down (avoids
       leaving objects on the GC list → list_empty assertion). */
    mini_net_api_pump(app);
#if MINI_MODE_CUSTOM
    if (app->ipc)
    {
        mini_ipc_destroy(app->ipc);
        app->ipc = NULL;
    }
    if (app->proto)
    {
        mini_protocol_destroy(app->proto);
        app->proto = NULL;
    }
#endif
    /* Order matters for the refcounted window model (mirrors ChildProcess):
       destroy the MAIN bridge FIRST so its GC fires BrowserWindow finalizers,
       dropping each secondary's JS-wrapper ref. Then drop the window-list ref
       for every window; when both refs are gone the window is fully freed
       (its own renderer/document/bridge/event-state). The primary (windows[0])
       is a thin wrapper (owns_resources==0): its resources are the app's and
       are freed explicitly below. */
    if (app->cdp)
        mini_cdp_stop(app->cdp);
    if (app->diag)
        mini_diag_stop(app->diag);
    /* bridge before events: bridge destroy calls mini_events_remove_listener
       on b->ev, so the event state must outlive the bridge.               */
    if (app->bridge)
        mini_bridge_destroy(app->bridge); /* GC: drops secondary JS refs */
    if (app->events)
        mini_events_state_destroy(app->events);
    if (app->doc)
        mini_doc_destroy(app->doc);
    if (app->r)
        mini_renderer_destroy(app->r);
#if MINI_MODE_CUSTOM
    /* Drop the window-list ref for every remaining window (primary + any
       secondaries whose JS handle was already GC'd above). ref==0 ⇒ free. */
    if (app->windows)
    {
        int n = app->n_windows;
        MiniWindow **kill = (MiniWindow **)calloc((size_t)(n > 0 ? n : 1), sizeof(*kill));
        if (kill)
        {
            int nk = 0;
            for (int i = 0; i < n; i++)
                if (app->windows[i])
                    kill[nk++] = app->windows[i];
            for (int i = 0; i < nk; i++)
            {
                MiniWindow *mw = kill[i];
                mw->ref--; /* drop the list ref */
                if (mw->ref <= 0)
                    mini_window_destroy(mw);
            }
            free(kill);
        }
        app->n_windows = 0;
        free(app->windows);
        app->windows = NULL;
    }
#endif
    /* GLFW owns the whole window/context pool; terminate it once, after every
       renderer (primary + secondaries) is gone. mini_renderer_destroy no longer
       calls glfwTerminate so that destroying one window never tears down the
       others in a multi-window app. */
    mini_renderer_terminate_glfw();
    free(app->page_url);
    free(app->page_source);
    free(app);
}

/* ---- program entry: create window + engine, load a JS file, run ---- */
#include <stdlib.h>
#include "stb_image.h"

static void load_app_config(MiniWindowConfig *cfg, char *entry_out, size_t entry_cap, char *icon_out, size_t icon_cap)
{
    const char *config_paths[] = { "app.config.json", "config.json", "package.json" };
    for (size_t i = 0; i < sizeof(config_paths)/sizeof(config_paths[0]); i++)
    {
        size_t sz = 0;
        char *content = read_file(config_paths[i], &sz);
        if (content)
        {
            char *p = strstr(content, "\"title\":");
            if (p)
            {
                p += 8;
                while (*p && (*p == ' ' || *p == '\"' || *p == ':')) p++;
                char *end = strchr(p, '\"');
                if (end)
                {
                    static char title_buf[256];
                    size_t len = (size_t)(end - p);
                    if (len >= sizeof(title_buf)) len = sizeof(title_buf) - 1;
                    memcpy(title_buf, p, len);
                    title_buf[len] = 0;
                    cfg->title = title_buf;
                }
            }
            p = strstr(content, "\"width\":");
            if (p) { p += 8; while(*p && !isdigit((unsigned char)*p)) p++; if(isdigit((unsigned char)*p)) cfg->width = atoi(p); }
            p = strstr(content, "\"height\":");
            if (p) { p += 9; while(*p && !isdigit((unsigned char)*p)) p++; if(isdigit((unsigned char)*p)) cfg->height = atoi(p); }
            p = strstr(content, "\"entry\":");
            if (p && entry_out)
            {
                p += 8;
                while (*p && (*p == ' ' || *p == '\"' || *p == ':')) p++;
                char *end = strchr(p, '\"');
                if (end)
                {
                    size_t len = (size_t)(end - p);
                    if (len >= entry_cap) len = entry_cap - 1;
                    memcpy(entry_out, p, len);
                    entry_out[len] = 0;
                }
            }
            p = strstr(content, "\"icon\":");
            if (p && icon_out)
            {
                p += 7;
                while (*p && (*p == ' ' || *p == '\"' || *p == ':')) p++;
                char *end = strchr(p, '\"');
                if (end)
                {
                    size_t len = (size_t)(end - p);
                    if (len >= icon_cap) len = icon_cap - 1;
                    memcpy(icon_out, p, len);
                    icon_out[len] = 0;
                }
            }
            free(content);
            break;
        }
    }
}

static void apply_app_icon(MiniApp *app, const char *custom_icon)
{
    if (!app || !app->r || !app->r->gpu.window_handle) return;
    const char *candidates[6];
    int n_cands = 0;
    if (custom_icon && custom_icon[0]) candidates[n_cands++] = custom_icon;
    candidates[n_cands++] = "assets/app.ico";
    candidates[n_cands++] = "assets/icon.png";
    candidates[n_cands++] = "app.ico";
    candidates[n_cands++] = "icon.png";

    for (int i = 0; i < n_cands; i++)
    {
        int iw = 0, ih = 0, ic = 0;
        unsigned char *pix = stbi_load(candidates[i], &iw, &ih, &ic, 4);
        if (pix && iw > 0 && ih > 0)
        {
            GLFWimage gimg;
            gimg.width = iw;
            gimg.height = ih;
            gimg.pixels = pix;
            glfwSetWindowIcon((GLFWwindow *)app->r->gpu.window_handle, 1, &gimg);
            stbi_image_free(pix);
            break;
        }
    }
}

int main(int argc, char **argv)
{
    /* Send CRT assert()/abort() output to stderr and abort (no blocking GUI
       popup) so mini_crash's SIGABRT handler can capture it and a test harness
       can read the QuickJS leak dump off stdout. (Default GUI behaviour shows
       a modal "Assertion failed!" dialog that hangs the process.) */
#if defined(_WIN32)
    _set_error_mode(_OUT_TO_STDERR);
#endif
    mini_log_init();
    mini_crash_set_version("1.0.0");
    mini_crash_init("tiny_app", "build");
    if (!getenv("TINY_LOG_FILE"))
        mini_log_set_file("build/tiny_app.log");
    mini_logf(MINI_LOG_INFO, "app", "TinyFramework starting (mode=%s)",
              mini_app_mode() ? "CUSTOM_MINI" : "NATIVE");

    const char *fc = getenv("TINY_FRAMES");
    if (fc)
        g_frame_cap = atol(fc);

    MiniWindowConfig cfg;
    MiniWindowConfig_Init(&cfg);
    cfg.width = 1280;
    cfg.height = 800;
    cfg.title = "TinyFramework";
    cfg.vsync = 1;

    char config_entry[512] = {0};
    char config_icon[512] = {0};
    load_app_config(&cfg, config_entry, sizeof(config_entry), config_icon, sizeof(config_icon));

    const char *w_env = getenv("TINY_WIDTH");
    const char *h_env = getenv("TINY_HEIGHT");
    if (w_env)
        cfg.width = atoi(w_env);
    if (h_env)
        cfg.height = atoi(h_env);

    MiniApp *app = NULL;
    if (mini_app_create(&cfg, &app) != MINI_OK)
    {
        mini_logf(MINI_LOG_ERROR, "app", "create failed (mode=%s)",
                  mini_app_mode() ? "CUSTOM_MINI" : "NATIVE");
        return 1;
    }
    /* Wire process.argv: pass main()'s argc/argv to the bridge so the global
       process object exposes them (Electron/Node parity). */
#if MINI_MODE_CUSTOM
    if (app->bridge)
        mini_bridge_set_argv(app->bridge, argc, argv);
#endif
    apply_app_icon(app, config_icon);

    uint16_t cdp_port = 9222;
    const char *port_env = getenv("TINY_CDP_PORT");
    if (port_env && atoi(port_env) > 0)
        cdp_port = (uint16_t)atoi(port_env);
    mini_app_enable_cdp(app, cdp_port);

    const char *js = (argc > 1) ? argv[1] : NULL;
    if (!js)
    {
        if (config_entry[0]) js = config_entry;
        else
        {
            FILE *fp;
            if ((fp = fopen("app.pak", "rb")) != NULL) { fclose(fp); js = "app.pak"; }
            else if ((fp = fopen("src/index.html", "rb")) != NULL) { fclose(fp); js = "src/index.html"; }
            else if ((fp = fopen("index.html", "rb")) != NULL) { fclose(fp); js = "index.html"; }
            else js = "test_suite.js";
        }
    }
    if (mini_app_load(app, js) != MINI_OK)
    {
        mini_logf(MINI_LOG_ERROR, "app", "load failed: %s", js);
        mini_app_destroy(app);
        return 1;
    }

    /* If encrypted bundle app.pak exists, decrypt and evaluate it in RAM */
    FILE *fpk = fopen("app.pak", "rb");
    if (fpk)
    {
        fseek(fpk, 0, SEEK_END);
        long psz = ftell(fpk);
        fseek(fpk, 0, SEEK_SET);
        if (psz > 0)
        {
            uint8_t *pbuf = (uint8_t *)malloc((size_t)psz);
            if (pbuf && fread(pbuf, 1, (size_t)psz, fpk) == (size_t)psz)
            {
                static const uint8_t default_vfs_key[32] = {
                    0x54,0x69,0x6e,0x79,0x46,0x72,0x61,0x6d,
                    0x65,0x77,0x6f,0x72,0x6b,0x53,0x65,0x63,
                    0x75,0x72,0x65,0x4b,0x65,0x79,0x32,0x30,
                    0x32,0x36,0x21,0x40,0x23,0x24,0x25,0x5e
                };
                mini_app_load_encrypted(app, pbuf, (size_t)psz, default_vfs_key);
            }
            if (pbuf) free(pbuf);
        }
        fclose(fpk);
    }
    fprintf(stderr, "[app] running %s (frame cap=%ld, CDP :%u)\n", js, g_frame_cap, cdp_port);
    mini_logf(MINI_LOG_INFO, "app", "running %s (frame cap=%ld, CDP :%u)",
              js, g_frame_cap, cdp_port);
    MiniResult r = mini_app_run(app);
    mini_app_destroy(app);
    mini_logf(MINI_LOG_INFO, "app", "exiting (rc=%d)", (r == MINI_OK) ? 0 : 1);
    mini_log_flush();
    return (r == MINI_OK) ? 0 : 1;
}

/* ================================================================== */
/* NATIVE mode (host WebView) �?stub                                   */
/* ================================================================== */
#else

MiniResult mini_app_create(const MiniWindowConfig *cfg, MiniApp **out)
{
    (void)cfg;
    (void)out;
    fprintf(stderr, "[TinyFramework] NATIVE mode not linked. Rebuild with "
                    "-DENABLE_CUSTOM_MINI_ENGINE or link a WebView backend.\n");
    return MINI_ERR_INIT;
}
MiniResult mini_app_load(MiniApp *app, const char *entry)
{
    (void)app;
    (void)entry;
    return MINI_ERR_INIT;
}
MiniResult mini_app_load_encrypted(MiniApp *app, const uint8_t *b, size_t s, const uint8_t k[32])
{
    (void)app;
    (void)b;
    (void)s;
    (void)k;
    return MINI_ERR_INIT;
}
MiniResult mini_app_enable_cdp(MiniApp *app, uint16_t port)
{
    (void)app;
    (void)port;
    return MINI_ERR_INIT;
}
MiniResult mini_app_run(MiniApp *app)
{
    (void)app;
    return MINI_ERR_INIT;
}
void mini_app_destroy(MiniApp *app) { (void)app; }

#endif
