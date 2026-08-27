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

/* Per-frame: drain any finished child processes (non-blocking) and fire their
   JS callbacks/listeners. Called from mini_bridge_pump() after
   bridge_pump_websockets(). */
void bridge_pump_children(struct MiniBridge *b);

/* Release every tracked child process + free buffers. Called from
   mini_bridge_destroy(). */
void mini_native_destroy(struct MiniBridge *b);

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
