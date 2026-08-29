/*
 * mini_window.h — multi-window manager for TinyFramework (CUSTOM_MINI mode).
 *
 * One MiniWindow per OS window. Window #0 is the "primary" — it wraps the
 * MiniApp's existing renderer/document/bridge/event-state (created in
 * mini_app_create) and keeps the rich render path (CDP/DevTools/scrollbar/
 * gesture overlays). Windows #1..N are "secondary" windows opened from JS via
 * `new BrowserWindow()`: each owns an INDEPENDENT MiniBridge (own QuickJS
 * runtime), MiniDocument, MiniRenderer (own GLFW window + GL context) and
 * MiniEventState — the Electron renderer-process model, in-process. The only
 * cross-window channel is IPC (mini_ipc).
 *
 * The engine is single-threaded and processes one window at a time, so the
 * host run loop switches the process-wide "active" globals (active document /
 * active bridge / active renderer) to the window currently being rendered
 * before its layout/render/rAF/pump phase (and restores the primary after).
 */
#ifndef MINI_WINDOW_H
#define MINI_WINDOW_H

#include "mini_framework.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward decls (opaque outside their own headers). */
struct MiniRenderer;
struct MiniDocument;
struct MiniBridge;
struct MiniEventState;

/* Creation options parsed from the BrowserWindow constructor's options bag.
   -1 / 0 mean "default". Strings are copied by the manager. */
typedef struct MiniWindowOpts
{
    int width;            /* client width (default 800)   */
    int height;           /* client height (default 600)  */
    const char *title;    /* window title (default "TinyFramework") */
    int frameless;        /* 1 = frame:false (no decoration)      */
    int transparent;      /* 1 = transparent:true                 */
    int resizable;        /* 1 = user-resizable (default 1)       */
    int always_on_top;    /* 1 = always on top                    */
    int fullscreen;       /* 1 = open fullscreen                  */
    int maximized;        /* 1 = open maximized                   */
    /* webPreferences */
    const char *preload;        /* preload script path (renderer)        */
    int context_isolation;      /* 1 = run preload in isolated context  */
    int sandbox;                /* 1 = sandbox the renderer              */
    int node_integration;       /* 1 = expose Node APIs to renderer       */
} MiniWindowOpts;

/* One OS window. `win` is a GLFWwindow* kept opaque so this header does not
   pull in GLFW. Primary windows (id==0) do NOT own r/doc/bridge/events — they
   alias the MiniApp's fields (owns_resources==0); secondary windows own them
   (owns_resources==1) and free them on destroy. */
struct MiniWindow
{
    int id;                 /* 0 = primary, 1..N = secondary */
    int is_primary;
    int owns_resources;     /* 1 = this window owns r/doc/bridge/events */
    MiniApp *app;          /* back-pointer to the host */

    void *win;             /* GLFWwindow* (opaque) */
    struct MiniRenderer *r;
    struct MiniDocument *doc;
    struct MiniBridge *bridge;
    struct MiniEventState *events;

    /* live style/state (queried/set by the BrowserWindow JS API) */
    int resizable;
    int always_on_top;
    int fullscreen;
    int frameless;
    int transparent;
    int visible;

    /* navigation / renderer preferences */
    char *title;
    char *url;             /* current page URL */
    char *preload;         /* preload script path (renderer) */
    int context_isolation;
    int sandbox;
    int node_integration;

    /* per-frame flags */
    uint8_t input_dirty;   /* set by GLFW callbacks; read by the idle gate */
    uint8_t closing;       /* set when the window is being closed */

    /* Lifetime refcount, mirroring the ChildProcess pattern: the window list
       holds one ref, the JS BrowserWindow wrapper holds one ref. The JS
       finalizer and the run-loop sweep each drop one ref; when it hits 0 the
       window is fully destroyed (resources + shell). This avoids both leaks
       (prompt free once both sides drop) and use-after-free (never freed
       while either side still holds it). */
    int ref;

    /* JS handles (held to prevent GC). web_contents lives in the window's
       owning context (main context for primary, the window's own renderer
       context for secondary). bw_ref is the BrowserWindow JS wrapper, held in
       the MAIN context (where `new BrowserWindow()` was called). */
    /* JSValue web_contents; -- stored as opaque void* to avoid pulling quickjs.h */
    void *web_contents;   /* JSValue* (dup'd) or NULL */
    void *bw_ref;         /* JSValue* (dup'd) or NULL */

    /* BrowserWindow JS event listeners (BwListeners*; main context). Opaque so
       mini_window.h does not pull in quickjs.h. Managed by mini_native.c. */
    void *listeners_data;
};

/* Register the primary window (id 0) wrapping the MiniApp's already-created
 * r/doc/bridge/events. Called once from mini_app_create after the existing
 * init. Installs the GLFW window user pointer + all input callbacks. Returns
 * the MiniWindow (also stored as app->windows[0]). */
struct MiniWindow *mini_window_register_primary(MiniApp *app,
                                               struct MiniRenderer *r,
                                               struct MiniDocument *doc,
                                               struct MiniBridge *bridge,
                                               struct MiniEventState *events);

/* ---- MiniApp window-list accessors (struct MiniApp is private to main.c).
 * These let mini_window.c manage the window list without seeing the struct. */

struct MiniWindow **mini_app_windows(MiniApp *app, int *n_out); /* array + count */
void mini_app_add_window(MiniApp *app, struct MiniWindow *mw); /* append */
int  mini_app_next_window_id(MiniApp *app); /* returns next id and increments */
void mini_app_remove_window(MiniApp *app, struct MiniWindow *mw); /* compact */

/* The host's main (process) bridge — the context where the BrowserWindow JS
 * class and ipcMain live. struct MiniApp is opaque here, so this accessor
 * lets mini_native.c reach the main bridge without seeing the struct. */
struct MiniBridge *mini_app_main_bridge(MiniApp *app);

/* The process-wide MiniIPC registry (opaque; mini_window.c installs it on
 * each secondary renderer bridge). Returns NULL in non-CUSTOM mode. */
void *mini_app_ipc(MiniApp *app);

/* The process-wide MiniProtocol registry (custom-scheme handlers consulted
 * by BrowserWindow.loadURL). Opaque; mini_native.c casts to MiniProtocol*. */
void *mini_app_proto(MiniApp *app);

/* Create a secondary window (id >= 1) with its own renderer/document/bridge
 * (renderer process). `opts` is parsed from the BrowserWindow constructor.
 * Returns NULL on failure. The window is appended to app->windows. */
struct MiniWindow *mini_window_create_secondary(MiniApp *app,
                                                const MiniWindowOpts *opts);

/* Destroy one window (frees its owned resources + JS context for secondary;
 * a no-op resource-wise for the primary, which is owned by the app). Removes
 * it from app->windows. Safe to call on a half-created window. */
void mini_window_destroy(struct MiniWindow *mw);

/* Mark a window as closing (GLFW should-close + state flag). The run loop
 * sweeps closing secondary windows and destroys them. */
void mini_window_close(struct MiniWindow *mw);

/* Number of currently-open windows (primary if not closing + non-closing
 * secondaries). */
int mini_window_count_open(MiniApp *app);

/* Render one secondary window's frame: make its GL context current, switch
 * the active globals to it, run layout/restyle/render/rAF/pump, swap. Used by
 * the host run loop's secondary phase. */
void mini_window_render_frame(struct MiniWindow *mw, double now_ms, double dt);

/* Find the MiniWindow owning a given GLFWwindow (via the per-window user
 * pointer). Returns NULL if none. */
struct MiniWindow *mini_window_from_glfw(void *glfw_window);

/* Pump a secondary window's bridge (timers/microtasks/IPC). Cheap when empty.
 * Called every iteration by the host loop (even on idle frames). */
void mini_window_pump(struct MiniWindow *mw);

/* Does this secondary window need a new frame this iteration? (input dirty,
 * pending rAF, dirty doc, active effects). */
int mini_window_needs_frame(struct MiniWindow *mw);

/* Sweep all closing secondary windows: destroy them and compact app->windows.
 * Emits 'browser-window-blurred'/'closed' as appropriate (handled by caller
 * via the app lifecycle). */
void mini_window_sweep_closed(MiniApp *app);

/* Dispatch a named window event (e.g. "resize"/"close") to the BrowserWindow
 * JS listeners on this window. Implemented in mini_native.c (it needs the main
 * context to call the JS callbacks). */
void mini_window_emit_event(struct MiniWindow *mw, const char *name);

/* ---- implemented in main.c (the GLFW callbacks + font chain are file-static
 * there) ------------------------------------------------------------- */

/* Install GLFW input callbacks + the per-window user pointer on `mw`'s OS
 * window so input routes to this MiniWindow (and its events/doc/bridge). */
void mini_window_install_callbacks(struct MiniWindow *mw);

/* Load the default system + local TrueType font chain onto a renderer (so a
 * secondary window renders anti-aliased text + emoji, same as the primary). */
void mini_window_load_default_fonts(struct MiniRenderer *r);

#ifdef __cplusplus
}
#endif
#endif /* MINI_WINDOW_H */
