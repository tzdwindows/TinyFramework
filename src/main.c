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
#endif
};

int mini_app_mode(void) { return MINI_MODE_CUSTOM ? 1 : 0; }

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

/* GLFW key callback -> diagnostics hotkey (F12 / Ctrl+Shift+I) + JS key events */
static void glfw_key_cb(GLFWwindow *win, int key, int scancode, int action,
                        int mods)
{
    (void)scancode;
    MiniApp *app = (MiniApp *)glfwGetWindowUserPointer(win);
    g_mods = mods;
    g_input_dirty = 1; /* key events may change :focus / drive input */
    /* F12: toggle the in-engine DevTools overlay (before dispatching the key
       to the page so the page can't swallow it). */
    if (app && app->bridge && action == GLFW_PRESS && key == GLFW_KEY_F12)
    {
        printf("open devtools\n");
        mini_devtools_toggle(app->bridge);
    }
    if (app && app->diag && (action == GLFW_PRESS || action == GLFW_REPEAT))
        mini_diag_key(app->diag, key, mods);
    if (app && app->events &&
        (action == GLFW_PRESS || action == GLFW_REPEAT || action == GLFW_RELEASE))
    {
        const char *ks, *cs;
        glfw_to_keycode(key, &ks, &cs);
        const char *type = (action == GLFW_RELEASE) ? "keyup" : "keydown";
        mini_events_handle_key(app->events, type, ks, cs, key, mods,
                               action == GLFW_REPEAT);
        /* keypress (deprecated but still common) fires on press for keys that
           produce a single character. */
        if (action == GLFW_PRESS && ks[0] && !ks[1] && ks[0] != ' ')
            mini_events_handle_key(app->events, "keypress", ks, cs, key, mods, 0);
        else if (action == GLFW_PRESS && key == GLFW_KEY_SPACE)
            mini_events_handle_key(app->events, "keypress", " ", "Space", key, mods, 0);
    }
}

/* GLFW char callback -> Unicode text input. The physical key callback only
   carries A-Z/0-9/named keys, so punctuation and shifted chars never reached
   a focused <input>/<textarea>. This delivers the OS-layout-resolved code
   point, which mini_events_handle_char appends to the focused control's
   value (and respects a preceding keydown's preventDefault). */
static void glfw_char_cb(GLFWwindow *win, unsigned int codepoint)
{
    MiniApp *app = (MiniApp *)glfwGetWindowUserPointer(win);
    g_input_dirty = 1; /* text input needs a fresh frame to show the char */
    if (app && app->events)
        mini_events_handle_char(app->events, codepoint);
}

/* Caret-moved -> reposition the OS IME composition + candidate window so
   Chinese/Japanese/Korean input commits at the insertion point. The events
   layer reports the caret in document framebuffer pixels; IMM wants client
   pixels of the window, so we divide by the fb/window scale. */
#if MINI_MODE_CUSTOM && defined(_WIN32)
static void caret_ime_cb(struct MiniNode *n, float x, float y, float h, void *ud)
{
    (void)n;
    MiniApp *app = (MiniApp *)ud;
    if (!app || !app->r)
        return;
    GLFWwindow *win = (GLFWwindow *)app->r->gpu.window_handle;
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
/* Copy the currently selected text to the OS clipboard (Ctrl+C). */
static void copy_clipboard_cb(MiniEventState *st, const char *text, void *ud)
{
    (void)st;
    MiniApp *app = (MiniApp *)ud;
    if (!app || !app->r || !app->r->gpu.window_handle || !text || !text[0])
        return;
    glfwSetClipboardString((GLFWwindow *)app->r->gpu.window_handle, text);
}
#endif

/* window->framebuffer pixel scale (for high-DPI: cursor is in window px,
   layout/abs coords are in framebuffer px). */
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

static void update_cursor_icon(GLFWwindow *win, MiniApp *app, float fx, float fy)
{
    if (!win || !app || !app->doc) return;
    if (!g_cursor_arrow) g_cursor_arrow = glfwCreateStandardCursor(GLFW_ARROW_CURSOR);
    if (!g_cursor_ibeam) g_cursor_ibeam = glfwCreateStandardCursor(GLFW_IBEAM_CURSOR);
    if (!g_cursor_hand) g_cursor_hand = glfwCreateStandardCursor(GLFW_HAND_CURSOR);
    if (!g_cursor_crosshair) g_cursor_crosshair = glfwCreateStandardCursor(GLFW_CROSSHAIR_CURSOR);

    /* Check scrollbar hover */
    int fw = app->r ? app->r->gpu.width : 1280;
    if (fx >= (float)fw - 14.0f && app->doc->max_scroll_y > 0.0f)
    {
        glfwSetCursor(win, g_cursor_arrow);
        return;
    }

    struct MiniNode *n = mini_dom_hit_test_doc(app->doc, fx, fy);
    if (!n)
    {
        glfwSetCursor(win, g_cursor_arrow);
        return;
    }

    /* 1. Explicit CSS cursor property */
    if (n->style.cursor == 1) { glfwSetCursor(win, g_cursor_hand); return; }
    if (n->style.cursor == 2) { glfwSetCursor(win, g_cursor_ibeam); return; }
    if (n->style.cursor == 6) { glfwSetCursor(win, g_cursor_crosshair); return; }
    if (n->style.cursor == 3 || n->style.cursor == 4) { glfwSetCursor(win, g_cursor_hand); return; }

    /* 2. Check interactive / pointer elements */
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

    /* 3. Check selectable text vs unselectable elements */
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
        /* Check unselectable filters */
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
    MiniApp *app = (MiniApp *)glfwGetWindowUserPointer(win);
    if (!app)
        return;
    float fx, fy;
    glfw_win_to_fb(win, x, y, &fx, &fy);

    update_cursor_icon(win, app, fx, fy);

    if (app->cdp && mini_cdp_is_inspect_mode(app->cdp) && app->doc)
    {
        mini_dom_assign_node_ids(app->doc);
        struct MiniNode *n = mini_dom_hit_test_doc(app->doc, fx, fy);
        if (n && n->cdp_node_id > 0)
        {
            mini_cdp_highlight_node(app->cdp, n->cdp_node_id);
            app->doc->dirty = 1;
        }
    }

    if (app->events)
    {
        g_input_dirty = 1; /* hover tracking needs a fresh frame */
        mini_events_handle_mouse_move(app->events, fx, fy, g_mods);
    }
}

static void glfw_mouse_cb(GLFWwindow *win, int button, int action, int mods)
{
    MiniApp *app = (MiniApp *)glfwGetWindowUserPointer(win);
    if (!app)
        return;
    double x, y;
    glfwGetCursorPos(win, &x, &y);
    g_mods = mods;
    float fx, fy;
    glfw_win_to_fb(win, x, y, &fx, &fy);

    if (app->cdp && mini_cdp_is_inspect_mode(app->cdp) && action == GLFW_PRESS && button == GLFW_MOUSE_BUTTON_LEFT)
    {
        if (app->doc)
        {
            mini_dom_assign_node_ids(app->doc);
            struct MiniNode *n = mini_dom_hit_test_doc(app->doc, fx, fy);
            if (n && n->cdp_node_id > 0)
            {
                mini_cdp_inspect_node(app->cdp, n->cdp_node_id);
                app->doc->dirty = 1;
                return; /* consume click for inspection */
            }
        }
    }

    if (app->events)
    {
        g_input_dirty = 1; /* click/press may change :active/:focus */
        mini_events_handle_mouse_button(app->events, button, action, fx, fy, mods);
    }
}

static void glfw_scroll_cb(GLFWwindow *win, double dx, double dy)
{
    MiniApp *app = (MiniApp *)glfwGetWindowUserPointer(win);
    if (!app || !app->events)
        return;
    double x, y;
    glfwGetCursorPos(win, &x, &y);
    g_input_dirty = 1; /* wheel scrolls the page */
    float fx, fy;
    glfw_win_to_fb(win, x, y, &fx, &fy);
    mini_events_handle_wheel(app->events, fx, fy, (float)dx, (float)dy, g_mods);
}

/* OS file/text drop: forward the dropped paths as an HTML5 `drop` event on the
   element under the cursor (paths are newline-joined in the DataTransfer
   text/plain slot). */
static void glfw_drop_cb(GLFWwindow *win, int count, const char **paths)
{
    MiniApp *app = (MiniApp *)glfwGetWindowUserPointer(win);
    if (!app || !app->events || count <= 0)
        return;
    double x, y;
    glfwGetCursorPos(win, &x, &y);
    g_input_dirty = 1;
    float fx, fy;
    glfw_win_to_fb(win, x, y, &fx, &fy);
    mini_events_handle_drop_files(app->events, paths, count, fx, fy);
}

static void glfw_fb_size_cb(GLFWwindow *win, int w, int h)
{
    MiniApp *app = (MiniApp *)glfwGetWindowUserPointer(win);
    if (!app || !app->events)
        return;
    g_input_dirty = 1; /* resize relayouts the page */
    mini_events_handle_resize(app->events, w, h);
}

static void glfw_window_focus_cb(GLFWwindow *win, int focused)
{
    MiniApp *app = (MiniApp *)glfwGetWindowUserPointer(win);
    if (!app || !app->events)
        return;
    if (!focused)
    {
        mini_events_release_capture(app->events);
        g_input_dirty = 1;
    }
}

static void glfw_cursor_enter_cb(GLFWwindow *win, int entered)
{
    MiniApp *app = (MiniApp *)glfwGetWindowUserPointer(win);
    if (!app || !app->events)
        return;
    if (!entered)
    {
        mini_events_release_capture(app->events);
        g_input_dirty = 1;
    }
}

static void gesture_cb(MiniEventState *st, const char *action_js, void *ud)
{
    (void)st;
    MiniApp *app = (MiniApp *)ud;
    if (app && app->bridge && action_js && action_js[0])
    {
        mini_bridge_eval(app->bridge, action_js, strlen(action_js), "<gesture>");
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

    /* 1b) Load the bundled TrueType font (AiDianFengYaHeiChangTi) so text
       renders anti-aliased and supports emoji (�? + CJK, instead of the
       built-in 5x7 bitmap font. The TTF ships next to the exe / in the
    /* 1b) Dynamically load TrueType/OpenType font chain (System fonts + relative fallbacks) */
    {
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
                if (mini_renderer_load_font(app->r, pbuf) == 0)
                {
                    fprintf(stderr, "[app] Primary font loaded: %s\n", pbuf);
                    pri_loaded = 1;
                    break;
                }
            }
        }
        if (!pri_loaded)
        {
            for (size_t i = 0; i < sizeof(local_cands) / sizeof(local_cands[0]); i++)
            {
                if (mini_renderer_load_font(app->r, local_cands[i]) == 0)
                {
                    fprintf(stderr, "[app] Primary font loaded (local): %s\n", local_cands[i]);
                    pri_loaded = 1;
                    break;
                }
            }
        }
        if (!pri_loaded)
            fprintf(stderr, "[app] Primary font not found; using 5x7 bitmap fallback\n");

        const char *fb_names[] = {
            "seguiemj.ttf", "simhei.ttf", "segoeui.ttf", "simsun.ttc", "arialuni.ttf"
        };
        if (win_dir[0])
        {
            char pbuf[576];
            for (size_t i = 0; i < sizeof(fb_names) / sizeof(fb_names[0]); i++)
            {
                snprintf(pbuf, sizeof(pbuf), "%s/Fonts/%s", win_dir, fb_names[i]);
                if (mini_renderer_load_fallback_font(app->r, pbuf) == 0)
                {
                    fprintf(stderr, "[app] Fallback font loaded: %s\n", pbuf);
                }
            }
        }
        for (size_t i = 0; i < sizeof(local_cands) / sizeof(local_cands[0]); i++)
        {
            if (mini_renderer_load_fallback_font(app->r, local_cands[i]) == 0)
            {
                fprintf(stderr, "[app] Fallback font loaded (local): %s\n", local_cands[i]);
            }
        }
    }

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
    /* Forward caret moves to the OS IME so the candidate window follows the
       insertion point (no-op platform stub is used where the IMM API isn't
       available). */
#if MINI_MODE_CUSTOM && defined(_WIN32)
    mini_events_set_caret_cb(app->events, caret_ime_cb, app);
#endif

#if MINI_MODE_CUSTOM
    /* Ctrl+C -> GLFW clipboard. (Works on all GLFW backends.) */
    mini_events_set_copy_cb(app->events, copy_clipboard_cb, app);
#endif
    mini_events_set_gesture_cb(app->events, gesture_cb, app);
    if (app->r->gpu.window_handle)
    {
        GLFWwindow *win = (GLFWwindow *)app->r->gpu.window_handle;
        glfwSetWindowUserPointer(win, app);
        glfwSetKeyCallback(win, glfw_key_cb);
        glfwSetCharCallback(win, glfw_char_cb);
        glfwSetCursorPosCallback(win, glfw_cursor_cb);
        glfwSetMouseButtonCallback(win, glfw_mouse_cb);
        glfwSetScrollCallback(win, glfw_scroll_cb);
        glfwSetFramebufferSizeCallback(win, glfw_fb_size_cb);
        glfwSetWindowFocusCallback(win, glfw_window_focus_cb);
        glfwSetCursorEnterCallback(win, glfw_cursor_enter_cb);
        glfwSetDropCallback(win, glfw_drop_cb);
    }

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

        /* ---- idle gate ------------------------------------------------
           Skip the expensive restyle + layout + render + flush + swap
           pipeline when nothing needs a new frame. This is the fix for the
           old behaviour of doing a full-pipeline spin every frame even on
           a fully static page (the dominant CPU drain). We still tick
           effects + pump jobs above, so timers/anim stay responsive. */
        int need_frame = g_input_dirty || viewport_changed ||
                         mini_bridge_pending_raf(app->bridge) > 0 ||
                         app->doc->dirty || app->doc->active_effects ||
                         emu_active || /* an active CDP metrics override */
                         (g_frame_cap && g_frames < g_frame_cap) ||
                         mini_events_has_text_focus(app->events) ||
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

        /* DOM render: clear to the document's computed background color if specified,
           or default to dark backdrop (#000000 or #0b0f19). */
        float bg_r = 0.043f, bg_g = 0.059f, bg_b = 0.098f, bg_a = 1.0f;
        if (app->doc && app->doc->body && app->doc->body->style.bg_a > 0.0f)
        {
            bg_r = app->doc->body->style.bg_r;
            bg_g = app->doc->body->style.bg_g;
            bg_b = app->doc->body->style.bg_b;
            bg_a = app->doc->body->style.bg_a;
        }
        else if (app->doc && app->doc->root && app->doc->root->style.bg_a > 0.0f)
        {
            bg_r = app->doc->root->style.bg_r;
            bg_g = app->doc->root->style.bg_g;
            bg_b = app->doc->root->style.bg_b;
            bg_a = app->doc->root->style.bg_a;
        }
        /* 1. Clear background */
        mini_renderer_begin_frame(app->r);
        mini_draw_clear(app->r, bg_r, bg_g, bg_b, bg_a);
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
    return MINI_OK;
}

void mini_app_destroy(MiniApp *app)
{
    if (!app)
        return;
    /* bridge before events: bridge destroy calls mini_events_remove_listener
       on b->ev, so the event state must outlive the bridge.               */
    if (app->cdp)
        mini_cdp_stop(app->cdp);
    if (app->diag)
        mini_diag_stop(app->diag);
    if (app->bridge)
        mini_bridge_destroy(app->bridge);
    if (app->events)
        mini_events_state_destroy(app->events);
    if (app->doc)
        mini_doc_destroy(app->doc);
    if (app->r)
        mini_renderer_destroy(app->r);
    free(app->page_url);
    free(app->page_source);
    free(app);
}

/* ---- program entry: create window + engine, load a JS file, run ---- */
#include <stdlib.h>
int main(int argc, char **argv)
{
    /* Boot the structured logger + crash interceptor FIRST, before anything
       else, so every later subsystem (and every JS console.* line) is captured
       to the ring + file, and a hard crash leaves a dump + log trail. The
       TINY_LOG / TINY_LOG_FILE / TINY_LOG_STDERR env vars override defaults. */
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
    const char *w_env = getenv("TINY_WIDTH");
    const char *h_env = getenv("TINY_HEIGHT");
    if (w_env)
        cfg.width = atoi(w_env);
    if (h_env)
        cfg.height = atoi(h_env);
    cfg.title = "TinyFramework";
    cfg.vsync = 1;

    MiniApp *app = NULL;
    if (mini_app_create(&cfg, &app) != MINI_OK)
    {
        mini_logf(MINI_LOG_ERROR, "app", "create failed (mode=%s)",
                  mini_app_mode() ? "CUSTOM_MINI" : "NATIVE");
        return 1;
    }
    uint16_t cdp_port = 9222;
    const char *port_env = getenv("TINY_CDP_PORT");
    if (port_env && atoi(port_env) > 0)
        cdp_port = (uint16_t)atoi(port_env);
    mini_app_enable_cdp(app, cdp_port);

    const char *js = (argc > 1) ? argv[1] : "test_suite.js";
    if (mini_app_load(app, js) != MINI_OK)
    {
        mini_logf(MINI_LOG_ERROR, "app", "load failed: %s", js);
        mini_app_destroy(app);
        return 1;
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
