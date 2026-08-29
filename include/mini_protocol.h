/*
 * mini_protocol.h — custom-scheme protocol registry (Electron `protocol`).
 *
 * Registered in the MAIN context (protocols are main-process-only, matching
 * Electron). Consulted by BrowserWindow.loadURL: a URL whose scheme has a
 * registered handler is served by calling that handler (in the main context)
 * instead of going to the network/filesystem. Handlers may return:
 *   - a { data: string|ArrayBuffer, mimeType, statusCode, headers } object
 *     (registerStringProtocol / registerBufferProtocol / handle)
 *   - a file path string { path } (registerFileProtocol)
 *   - a redirect to an http(s) URL { url } (registerHttpProtocol)
 *
 * Renderer-side fetch of custom schemes is not supported in this build (only
 * loadURL, which runs in the main context, consults the registry).
 */
#ifndef MINI_PROTOCOL_H
#define MINI_PROTOCOL_H

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

struct MiniApp;

typedef struct MiniProtocol MiniProtocol;

MiniProtocol *mini_protocol_create(void);
void          mini_protocol_destroy(MiniProtocol *p);

/* Install the `protocol` JS surface (registerFileProtocol / registerHttpProtocol
 * / registerStringProtocol / registerBufferProtocol / handle / unprotocol /
 * registerSchemesAsPrivileged / unregisterSchemesAsPrivileged) on the MAIN
 * context. */
void mini_protocol_install(struct MiniProtocol *p, struct MiniApp *app);

/* Consult the registry for `url`. If its scheme is registered, call the JS
 * handler (in the main context) and write the resolved body + mime + status.
 * Returns 1 if handled (caller loads `*body`/`*len`), 0 otherwise. `*body`
 * is malloc'd and owned by the caller; `*mime` is malloc'd too. */
int mini_protocol_resolve(struct MiniProtocol *p, struct MiniApp *app,
                          const char *url,
                          char **body, size_t *len, char **mime, int *status);

#ifdef __cplusplus
}
#endif
#endif /* MINI_PROTOCOL_H */
