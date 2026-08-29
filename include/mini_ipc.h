/*
 * mini_ipc.h — inter-context IPC for multi-window TinyFramework.
 *
 * One MiniIPC per process (on the MiniApp). Each context (the main process +
 * every renderer/secondary window) is a separate QuickJS runtime, so JSValues
 * cannot cross contexts. IPC messages carry JSON-serialized args; the host
 * run loop drains each context's mailbox during that context's pump phase and
 * calls its JS listeners/handlers there (no cross-context re-entrancy).
 *
 * Surface (mirrors Electron):
 *   ipcMain.on(channel, listener) / .handle(channel, handler) / .removeListener / .removeAllListeners
 *   ipcRenderer.send(channel, ...args) / .invoke(channel, ...args)→Promise / .on / .removeListener / .sendSync (best-effort)
 *   webContents.send(channel, ...args)  (main → a specific renderer)
 *   event.sender  (the webContents that sent a message, for replies)
 */
#ifndef MINI_IPC_H
#define MINI_IPC_H

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

struct MiniBridge;

typedef struct MiniIPC MiniIPC;

/* Create/destroy the process-wide IPC registry (one per MiniApp). */
MiniIPC *mini_ipc_create(void);
void     mini_ipc_destroy(MiniIPC *ipc);

/* Install the IPC native functions + the JS glue shim into a bridge's context.
 * `is_main` selects the ipcMain surface (main) vs ipcRenderer surface
 * (renderer). `ctx_id` is the bridge's window id (0 = main, >0 = renderer) and
 * is also the source id stamped on every message this context sends. */
void mini_ipc_install(MiniIPC *ipc, struct MiniBridge *b, int is_main, int ctx_id);

/* Drain this context's mailbox (messages addressed to ctx_id) and dispatch
 * each to its JS listener/handler in this context's ctx. Called by the host
 * run loop during each context's pump phase (main + every secondary). */
void mini_ipc_dispatch(MiniIPC *ipc, struct MiniBridge *b, int ctx_id);

/* Post a 'send' message directly from C (used by webContents.send in the main
 * context to push a message to a specific renderer without going through JS). */
void mini_ipc_post_send(MiniIPC *ipc, int src, int dst,
                        const char *channel, const char *args_json);

#ifdef __cplusplus
}
#endif
#endif /* MINI_IPC_H */
