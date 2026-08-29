/*
 * mini_native.h — OS capability + Electron-style module surface.
 *
 * The browser-style bridge (mini_js_bridge.c) is intentionally sandboxed:
 * it cannot spawn processes, read arbitrary files, or query the OS. This
 * layer adds a Node/Electron-shaped surface on top — os / process /
 * child_process / fs / path / electron — wired through a CommonJS `require`
 * mechanism (JS shim) backed by a C-side table of built-in module objects.
 *
 * install_native() is the single entry called from mini_bridge_create();
 * bridge_pump_children() is hooked into the per-frame pump for async
 * child-process completion (non-blocking, so the UI thread never stalls).
 */
#ifndef MINI_NATIVE_H
#define MINI_NATIVE_H

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward decls — MiniBridge is opaque (defined privately in mini_js_bridge.c). */
struct MiniBridge;
struct MiniRenderer;
struct MiniApp;

/* ---- child_process support ------------------------------------------------- */

/* One live child process tracked by the per-frame pump so exec/spawn can
   complete asynchronously without blocking the UI thread. Defined here so
   mini_js_bridge.c can hold an array of them. */
typedef struct JsChildProc
{
    void *proc;        /* OS handle: Win=PROCESS_INFORMATION*, POSIX=pid_t storage */
    void *stdout_pipe; /* OS pipe handle (read end) */
    void *stderr_pipe; /* OS pipe handle (read end) */
    int   exited;
    int   exit_code;
    char *stdout_buf;  /* accumulated stdout */
    size_t stdout_len, stdout_cap;
    char *stderr_buf;  /* accumulated stderr */
    size_t stderr_len, stderr_cap;
    JSValue cb;        /* exec callback (err,stdout,stderr); JS_NULL if none */
    JSValue onclose;   /* spawn 'close' listener */
    JSValue onexit;    /* spawn 'exit' listener */
    JSValue stdout_listeners; /* array of 'data' listeners for spawn */
    JSValue stderr_listeners;
    int killed;
    int ref;                 /* refcount: children array + JS wrapper obj */
    size_t stdout_fired;     /* bytes already dispatched to 'data' listeners */
    size_t stderr_fired;
} JsChildProc;

/* Browser-style navigator.platform string derived from the OS, for the
   navigator object installed by mini_js_bridge.c (replaces the hardcoded
   "Win32"). */
const char *mini_navigator_platform(void);

/* ---- entry points (called by mini_js_bridge.c) ---------------------------- */

/* Build the built-in module table (__miniBuiltinModules global), install the
   global `process` object, and register os/process (Phase 1), fs/path
   (Phase 2), child_process (Phase 3), electron namespace (Phase 4). Called
   from mini_bridge_create() after install_webgl(). */
void install_native(struct MiniBridge *b);

/* Install the renderer-scoped `electron` object on a secondary-window
   (renderer) context: ipcRenderer, contextBridge, webFrame, crashReporter.
   No app/BrowserWindow/ipcMain/os/fs/child_process (Electron renderer split).
   Called from mini_bridge_create_ex() when is_renderer=1. The ipcRenderer
   bindings are wired to the process-wide MiniIPC registry (mini_ipc.c). */
void install_renderer_electron(struct MiniBridge *b);

/* Per-frame: drain any finished child processes (non-blocking) and fire their
   JS callbacks/listeners. Called from mini_bridge_pump() after
   bridge_pump_websockets(). */
void bridge_pump_children(struct MiniBridge *b);

/* Release every tracked child process + free buffers. Called from
   mini_bridge_destroy(). */
void mini_native_destroy(struct MiniBridge *b);

/* ---- app lifecycle (multi-window / single-instance) --------------------- */
/* Mark the app as packaged (loaded from an encrypted bundle) so app.isPackaged
 * returns true. Called from mini_app_load_encrypted in main.c. */
void mini_native_set_packaged(int packaged);

/* Emit an app lifecycle event (e.g. "window-all-closed", "before-quit",
 * "browser-window-created", "activate") to its JS listeners in the MAIN
 * context. argv (argc of them) are dup'd in the main ctx and consumed.
 * "browser-window-created" is emitted by the BrowserWindow constructor. */
void mini_app_emit(struct MiniApp *app, const char *event, int argc, JSValueConst *argv);
void mini_app_emit_event(struct MiniApp *app, const char *event); /* 0-arg convenience */

/* Drain any second-instance relays (argv/cwd handed off by a 2nd process over
 * the named pipe) and emit 'second-instance' on the main context. Called by
 * the host run loop each frame (main phase). */
void mini_app_pump_second_instance(struct MiniApp *app);

/* Drain any completed `net.request` fetches (run on background threads) and
 * emit response/data/end/error on each ClientRequest in its main context.
 * Called by the host run loop each frame (main phase). */
void mini_net_api_pump(struct MiniApp *app);

/* ---- bridge accessors (implemented in mini_js_bridge.c so MiniBridge stays
   opaque to mini_native.c; each just exposes a private field) --------------- */

/* Store argc/argv (process.argv source). Also updates process.argv on the
   already-installed global process object. main.c calls this right after
   mini_app_create(). argv lifetime is the process (main's argv), so pointers
   are stored as-is. */
void mini_bridge_set_argv(struct MiniBridge *b, int argc, char **argv);

/* Read back the stored argv (NULL-able). */
char **mini_bridge_argv(struct MiniBridge *b, int *argc_out);

/* Renderer handle (for window operations: app.quit, BrowserWindow, dialog). */
struct MiniRenderer *mini_bridge_renderer(struct MiniBridge *b);

/* Multi-window: this bridge's owning window id (0 = main process / primary,
   >0 = a renderer / secondary window). The IPC registry routes messages
   between the main context and each renderer by this id. */
void mini_bridge_set_window_id(struct MiniBridge *b, int id);
int  mini_bridge_get_window_id(struct MiniBridge *b);

/* The MiniApp host pointer (set on the main/primary bridge only) so the
   BrowserWindow constructor — which runs in the main context — can reach the
   host to create secondary windows. NULL on renderer bridges. */
void mini_bridge_set_host(struct MiniBridge *b, void *host);
void *mini_bridge_get_host(struct MiniBridge *b);

/* The process-wide MiniIPC registry pointer (set by mini_ipc_install). Lets
   IPC native functions reach the shared mailbox from any bridge. */
void  mini_bridge_set_ipc(struct MiniBridge *b, void *ipc);
void *mini_bridge_get_ipc(struct MiniBridge *b);

/* The process-wide MiniProtocol registry pointer (main bridge only) for
   custom-scheme handlers consulted by BrowserWindow.loadURL. */
void  mini_bridge_set_proto(struct MiniBridge *b, void *proto);
void *mini_bridge_get_proto(struct MiniBridge *b);

/* Background worker pool (mini_worker.c) for async fs/fetch. */
struct MiniWorkerQueue;
struct MiniWorkerQueue *mini_bridge_workers(struct MiniBridge *b);

/* Built-in module table object (__miniBuiltinModules). install_native sets it
   so mini_bridge_set_argv can reach the process object later. */
void     mini_bridge_set_builtin_mods(struct MiniBridge *b, JSValue mods);
JSValue  mini_bridge_builtin_mods(struct MiniBridge *b);

/* child-process array management (used by mini_native.c). */
void           mini_bridge_add_child(struct MiniBridge *b, JsChildProc *c);
JsChildProc  **mini_bridge_children(struct MiniBridge *b, int *n_out);
void           mini_bridge_remove_child(struct MiniBridge *b, int idx);

#ifdef __cplusplus
}
#endif
#endif /* MINI_NATIVE_H */
