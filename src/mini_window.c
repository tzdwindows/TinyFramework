/*
 * mini_window.c — multi-window manager (see mini_window.h).
 *
 * Owns the per-window struct lifecycle for SECONDARY windows (each with its
 * own renderer/document/bridge/event-state — the Electron renderer-process
 * model, in-process) and a thin wrapper for the PRIMARY window (which aliases
 * the MiniApp's existing resources). The host run loop in main.c drives
 * per-window rendering via mini_window_render_frame().
 */
#include "mini_window.h"
#include "mini_renderer.h"
#include "mini_dom.h"
#include "mini_js_bridge.h"
#include "mini_native.h"
#include "mini_ipc.h"
#include "mini_events.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#  define GLFW_EXPOSE_NATIVE_WIN32
#  include <GLFW/glfw3.h>
#else
#  include <GLFW/glfw3.h>
#endif

/* ------------------------------------------------------------------ */
/* registration: primary + secondary                                   */
/* ------------------------------------------------------------------ */

struct MiniWindow *mini_window_register_primary(MiniApp *app,
                                               struct MiniRenderer *r,
                                               struct MiniDocument *doc,
                                               struct MiniBridge *bridge,
                                               struct MiniEventState *events)
{
    if (!app || !r)
        return NULL;
    struct MiniWindow *mw = (struct MiniWindow *)calloc(1, sizeof(*mw));
    if (!mw)
        return NULL;
    mw->id = 0;
    mw->is_primary = 1;
    mw->owns_resources = 0; /* the MiniApp owns r/doc/bridge/events */
    mw->app = app;
    mw->r = r;
    mw->doc = doc;
    mw->bridge = bridge;
    mw->events = events;
    mw->win = r->gpu.window_handle;
    mw->visible = 1;
    mw->resizable = 1;
    mw->frameless = 0;
    mw->transparent = 0;
    mw->always_on_top = 0;
    mw->fullscreen = 0;
    mw->title = strdup("TinyFramework");
    mini_bridge_set_window_id(bridge, 0);
    mini_app_add_window(app, mw);
    mw->ref = 1; /* list ref (the primary has no JS BrowserWindow wrapper) */
    mini_window_install_callbacks(mw);
    return mw;
}

struct MiniWindow *mini_window_create_secondary(MiniApp *app,
                                                const MiniWindowOpts *opts)
{
    if (!app)
        return NULL;
    MiniWindowOpts o;
    memset(&o, 0, sizeof(o));
    o.width = 800; o.height = 600;
    o.resizable = 1;
    if (opts)
    {
        if (opts->width > 0) o.width = opts->width;
        if (opts->height > 0) o.height = opts->height;
        o.title = opts->title;
        o.frameless = opts->frameless;
        o.transparent = opts->transparent;
        o.resizable = opts->resizable ? opts->resizable : (opts->resizable == 0 ? 0 : 1);
        o.always_on_top = opts->always_on_top;
        o.fullscreen = opts->fullscreen;
        o.maximized = opts->maximized;
        o.preload = opts->preload;
        o.context_isolation = opts->context_isolation;
        o.sandbox = opts->sandbox;
        o.node_integration = opts->node_integration;
    }

    /* 1) GPU context + renderer (own GLFW window + GL context; never
       glfwTerminate on failure so other windows survive). */
    MiniRendererHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.decorated = o.frameless ? 0 : 1;
    hints.transparent = o.transparent;
    hints.resizable = o.resizable;
    hints.always_on_top = o.always_on_top;
    hints.fullscreen = o.fullscreen;
    hints.maximized = o.maximized;
    struct MiniRenderer *r = mini_renderer_create_window(o.width, o.height, 8, 1, &hints);
    if (!r)
        return NULL;
    mini_window_load_default_fonts(r);

    /* 2) DOM document (own scene graph). */
    struct MiniDocument *doc = mini_doc_create();
    if (!doc) { mini_renderer_destroy(r); return NULL; }

    /* 3) Renderer bridge: own QuickJS runtime/context (isolated from the main
       process and from every other window). Browser globals + renderer
       electron (ipcRenderer/contextBridge). */
    struct MiniBridge *bridge = mini_bridge_create_child(r, doc);
    if (!bridge)
    {
        mini_doc_destroy(doc);
        mini_renderer_destroy(r);
        return NULL;
    }

    /* 4) W3C event state (per-document) + wire bridge -> events. */
    struct MiniEventState *events = mini_events_state_create(doc);
    if (!events)
    {
        mini_bridge_destroy(bridge);
        mini_doc_destroy(doc);
        mini_renderer_destroy(r);
        return NULL;
    }
    mini_bridge_set_events(bridge, events);

    /* 5) Assemble the MiniWindow. */
    struct MiniWindow *mw = (struct MiniWindow *)calloc(1, sizeof(*mw));
    if (!mw)
    {
        mini_events_state_destroy(events);
        mini_bridge_destroy(bridge);
        mini_doc_destroy(doc);
        mini_renderer_destroy(r);
        return NULL;
    }
    mw->id = mini_app_next_window_id(app);
    mw->is_primary = 0;
    mw->owns_resources = 1;
    mw->app = app;
    mw->r = r;
    mw->doc = doc;
    mw->bridge = bridge;
    mw->events = events;
    mw->win = r->gpu.window_handle;
    mw->visible = 1;
    mw->resizable = o.resizable;
    mw->frameless = o.frameless;
    mw->transparent = o.transparent;
    mw->always_on_top = o.always_on_top;
    mw->fullscreen = o.fullscreen;
    mw->title = strdup(o.title ? o.title : "TinyFramework");
    mw->preload = o.preload ? strdup(o.preload) : NULL;
    mw->context_isolation = o.context_isolation;
    mw->sandbox = o.sandbox;
    mw->node_integration = o.node_integration;
    mini_bridge_set_window_id(bridge, mw->id);

    if (mw->win)
        glfwSetWindowTitle((GLFWwindow *)mw->win, mw->title);

    /* Install ipcRenderer in this renderer's context (the process-wide
       MiniIPC registry was created on the host in mini_app_create). */
    MiniIPC *ipc = (MiniIPC *)mini_app_ipc(app);
    if (ipc)
        mini_ipc_install(ipc, bridge, 0, mw->id);

    mini_app_add_window(app, mw);
    mw->ref = 1; /* the window-list holds one ref; the JS wrapper adds another */
    mini_window_install_callbacks(mw);
    return mw;
}

/* ------------------------------------------------------------------ */
/* destroy / close / count                                            */
/* ------------------------------------------------------------------ */

void mini_window_destroy(struct MiniWindow *mw)
{
    if (!mw)
        return;
    /* detach from the app's window list first so the run loop won't touch it */
    if (mw->app)
        mini_app_remove_window(mw->app, mw);

    if (mw->owns_resources)
    {
        /* bridge before events (bridge destroy removes its listeners) */
        if (mw->bridge)
            mini_bridge_destroy(mw->bridge);
        if (mw->events)
            mini_events_state_destroy(mw->events);
        if (mw->doc)
            mini_doc_destroy(mw->doc);
        if (mw->r)
            mini_renderer_destroy(mw->r);
    }
    free(mw->title);
    free(mw->url);
    free(mw->preload);
    /* JS handles (web_contents / bw_ref) are owned by their own context and
       freed when that context is destroyed; here we only drop the C copy of
       the pointer. */
    free(mw);
}

void mini_window_close(struct MiniWindow *mw)
{
    if (!mw)
        return;
    mw->closing = 1;
    if (mw->win)
        glfwSetWindowShouldClose((GLFWwindow *)mw->win, GLFW_TRUE);
}

int mini_window_count_open(MiniApp *app)
{
    int n = 0, cnt = 0;
    struct MiniWindow **ws = mini_app_windows(app, &n);
    for (int i = 0; i < n; i++)
        if (ws[i] && !ws[i]->closing)
            cnt++;
    return cnt;
}

struct MiniWindow *mini_window_from_glfw(void *glfw_window)
{
    if (!glfw_window)
        return NULL;
    return (struct MiniWindow *)glfwGetWindowUserPointer((GLFWwindow *)glfw_window);
}

/* ------------------------------------------------------------------ */
/* per-frame: pump / needs_frame / render                              */
/* ------------------------------------------------------------------ */

void mini_window_pump(struct MiniWindow *mw)
{
    if (mw && mw->bridge)
        mini_bridge_pump(mw->bridge);
}

int mini_window_needs_frame(struct MiniWindow *mw)
{
    if (!mw || !mw->doc || mw->closing || !mw->win)
        return 0;
    if (mw->input_dirty)
        return 1;
    if (mini_bridge_pending_raf(mw->bridge) > 0)
        return 1;
    if (mw->doc->dirty || mw->doc->active_effects)
        return 1;
    return 0;
}

void mini_window_render_frame(struct MiniWindow *mw, double now_ms, double dt)
{
    if (!mw || !mw->r || !mw->win || mw->closing)
        return;

    glfwMakeContextCurrent((GLFWwindow *)mw->win);
    /* switch the process-wide "active" globals to this window so layout/
       restyle/render/rAF operate on THIS window's document/renderer/bridge */
    mini_dom_set_active_doc(mw->doc);
    mini_bridge_set_active(mw->bridge);

    int fw = 0, fh = 0;
    glfwGetFramebufferSize((GLFWwindow *)mw->win, &fw, &fh);
    if (fw <= 0) fw = mw->r->gpu.width;
    if (fh <= 0) fh = mw->r->gpu.height;
    int viewport_changed = (fw != mw->r->gpu.width || fh != mw->r->gpu.height);
    if (viewport_changed)
    {
        mw->r->gpu.width = fw;
        mw->r->gpu.height = fh;
        mw->r->vbuf.width = fw;
        mw->r->vbuf.height = fh;
        if (mw->events)
            mini_events_handle_resize(mw->events, fw, fh);
        else if (mw->doc)
        {
            mw->doc->viewport_w = fw;
            mw->doc->viewport_h = fh;
            mw->doc->dirty = 1;
            mw->doc->layout_dirty = 1;
        }
    }

    /* advance CSS transitions / @keyframes for the elapsed wall-clock dt */
    mini_dom_tick_frame(mw->doc, dt);
    /* drain JS microtasks/macrotasks (timers/Promise.then) */
    mini_bridge_pump(mw->bridge);

    if (mw->doc->dirty || mw->input_dirty || viewport_changed)
        mini_dom_restyle(mw->doc);
    mw->input_dirty = 0;

    if (mw->doc->dirty || mw->doc->layout_dirty)
        mini_layout_run(mw->doc, fw, fh);

    /* 1. clear + page backdrop */
    mini_renderer_begin_frame(mw->r);
    mini_dom_render_page_backdrop(mw->doc, mw->r, (float)fw, (float)fh);
    mini_renderer_flush(mw->r);

    /* 2. JS rAF (WebGL/Canvas draws into the window's region) */
    mini_renderer_restore_webgl(mw->r);
    mini_bridge_fire_raf(mw->bridge, now_ms);

    /* 3. DOM render pass on top */
    mini_renderer_begin_frame(mw->r);
    mini_dom_set_render_events(mw->events);
    mini_dom_render_into(mw->doc->body, mw->r);
    mini_renderer_flush(mw->r);

    /* 4. scrollbar (simplified copy of the primary path) */
    if (mw->doc && mw->doc->max_scroll_y > 0.0f)
    {
        float vw = (float)fw, vh = (float)fh;
        float track_x = vw - 8.0f;
        float total_h = mw->doc->max_scroll_y + vh;
        float thumb_h = vh * (vh / total_h);
        if (thumb_h < 36.0f) thumb_h = 36.0f;
        if (thumb_h > vh - 10.0f) thumb_h = vh - 10.0f;
        float thumb_y = (mw->doc->scroll_y / mw->doc->max_scroll_y) * (vh - thumb_h);
        float radii[4] = {3, 3, 3, 3};
        mini_draw_rect_rounded_corners(mw->r, track_x, thumb_y, 6.0f, thumb_h, radii, 1.0f, 1.0f, 1.0f, 0.40f);
        mini_renderer_flush(mw->r);
    }

    mini_renderer_end_frame(mw->r); /* swap */
    mini_bridge_pump(mw->bridge);
    mw->doc->dirty = 0;
}

/* ------------------------------------------------------------------ */
/* sweep: destroy closing secondary windows                           */
/* ------------------------------------------------------------------ */

void mini_window_sweep_closed(MiniApp *app)
{
    if (!app)
        return;
    int n = 0;
    struct MiniWindow **ws = mini_app_windows(app, &n);
    /* snapshot the closing secondaries (the array mutates as we remove) */
    struct MiniWindow **to_kill = NULL;
    int nk = 0;
    to_kill = (struct MiniWindow **)calloc((size_t)(n > 0 ? n : 1), sizeof(*to_kill));
    if (!to_kill)
        return;
    for (int i = 0; i < n; i++)
    {
        struct MiniWindow *mw = ws[i];
        if (mw && !mw->is_primary && mw->closing)
            to_kill[nk++] = mw;
    }
    /* Drop the window-list ref for each; fully destroy only when both the
       list ref and the JS-wrapper ref are gone (ref==0). If JS still holds
       the BrowserWindow handle, the window's OS surface is already closed
       (glfwSetWindowShouldClose) and it is removed from the render list, but
       its resources live until the JS handle is GC'd (finalizer drops the
       last ref). */
    for (int i = 0; i < nk; i++)
    {
        struct MiniWindow *mw = to_kill[i];
        mini_app_remove_window(app, mw);
        mw->ref--; /* drop the list ref */
        if (mw->ref <= 0)
            mini_window_destroy(mw);
    }
    free(to_kill);
}
