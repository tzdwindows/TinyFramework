/*
 * mini_cdp.h — minimal Chrome DevTools Protocol (CDP) server.
 *
 * Embeds an HTTP+WebSocket server on localhost so a developer can open
 * chrome://inspect (or connect ws://localhost:9222) and:
 *   - see console.log/warn/error streamed from QuickJS,
 *   - browse the self-written MiniNode DOM tree (Elements panel),
 *   - evaluate JS live in the QuickJS context (Runtime.evaluate).
 *
 * The server is single-threaded + non-blocking: mini_cdp_poll() is called
 * from the main loop, so CDP runs on the QuickJS thread (no locking).
 */
#ifndef MINI_CDP_H
#define MINI_CDP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MiniCDP MiniCDP;

/* Host callbacks. Each writes a JSON fragment into `out` (null-terminated).
   on_eval: evaluate `expr` in the JS engine, return a JSON-encoded result.
   on_dom:  serialize the MiniNode tree as a CDP DOM.getDocument response.
   on_log is inbound (host -> devtools), see mini_cdp_emit_log. */
typedef struct {
    void (*on_eval)(const char *expr, char *out, size_t out_cap, void *ud);
    void (*on_dom) (char *out, size_t out_cap, void *ud);
    void  *ud;
} MiniCDPCallbacks;

/* Start listening on `port` (9222 by default). Returns NULL on failure. */
MiniCDP *mini_cdp_start(uint16_t port, MiniCDPCallbacks *cb);
void     mini_cdp_stop(MiniCDP *cdp);

/* Non-blocking: accept clients, read frames, dispatch CDP commands,
   send responses. Call once per frame from the main loop. */
void mini_cdp_poll(MiniCDP *cdp);

/* Push a console entry to every connected devtools client as a
   Runtime.consoleAPICalled event. level: "log"|"warning"|"error". */
void mini_cdp_emit_log(MiniCDP *cdp, const char *level, const char *text);

/* ================================================================== */
/* Engine host attachment (P1: full-domain CDP).                       */
/*                                                                    */
/* The original API only had on_dom/on_eval callbacks. To back the    */
/* full CDP domain set (Runtime reflection, Debugger, DOM node-id      */
/* addressing, Page screenshot, Overlay, Performance) the CDP layer   */
/* needs direct engine pointers. attach_host() is additive: when a   */
/* host is attached, the domain layer (mini_cdp_domains.c) drives     */
/* responses from the live engine; the legacy on_eval/on_dom callbacks*/
/* remain as a fallback for hosts that never attach.                  */
/*                                                                    */
/* Fields are void* here so mini_cdp.h stays light (no transitive      */
/* engine includes); mini_cdp_domains.c casts them to the real types. */
/* ================================================================== */
typedef struct MiniCDPHost
{
    void *bridge;   /* MiniBridge*  — QuickJS (Runtime/Debugger/HeapProfiler) */
    void *doc;      /* MiniDocument* — DOM tree (Elements) */
    void *renderer; /* MiniRenderer* — screenshot + overlay */
    void *diag;     /* MiniDiag* — Performance metrics */
    void *events;   /* MiniEventState* — Input events */
    const char *url;         /* Current page document URL / file path */
    const char *page_source; /* Raw page HTML / source text */
} MiniCDPHost;

/* Attach the live engine so the full CDP domain set is backed by real
   data. NULL host detaches (back to legacy callback-only behaviour). */
void mini_cdp_attach_host(MiniCDP *cdp, MiniCDPHost *host);

/* Access the attached host (NULL if none). The domain layer uses this to
   reach the live engine (JSContext via mini_bridge_ctx, DOM, renderer). */
MiniCDPHost *mini_cdp_host(MiniCDP *cdp);

/* Send a CDP text frame to one client (by index) — for command replies. */
void mini_cdp_send_client(MiniCDP *cdp, int client_index,
                          const char *json, size_t len);
/* Send a CDP text frame to every upgraded client — for events. */
void mini_cdp_broadcast(MiniCDP *cdp, const char *json, size_t len);

/* Dispatch one decoded CDP request message to the full domain handler
   (mini_cdp_domains.c). Called from the transport poll loop with the
   originating client index so replies go to the right client. */
void mini_cdp_dispatch(MiniCDP *cdp, int client_index, const char *msg);

/* Overlay highlight: returns 1 and fills the box of the highlighted node
   (set by Overlay/DOM.highlightNode), or 0 if none. The renderer calls
   this each frame to draw the highlight on top of the scene. */
int mini_cdp_overlay_box(MiniCDP *cdp, float *x, float *y, float *w, float *h);
int mini_cdp_overlay_box_ex(MiniCDP *cdp,
                           float *x, float *y, float *w, float *h,
                           float pad[4], float marg[4],
                           char *tag_buf, size_t tag_cap,
                           char *id_buf, size_t id_cap,
                           char *class_buf, size_t class_cap);
int mini_cdp_has_overlay(MiniCDP *cdp);
int mini_cdp_is_inspect_mode(MiniCDP *cdp);
void mini_cdp_set_inspect_mode(MiniCDP *cdp, int active);
void mini_cdp_inspect_node(MiniCDP *cdp, int backend_node_id);
void mini_cdp_highlight_node(MiniCDP *cdp, int node_id);

/* Flush any unreported fetch records as Network events. Called each frame
   from the transport poll when Network is enabled. */
void mini_cdp_network_flush(MiniCDP *cdp);

/* Emulation.setDeviceMetricsOverride: returns 1 + fills w/h when an
   override is active, 0 otherwise. The main loop applies it (resizes
   the window) so the Device toolbar works. */
int mini_cdp_emulation_viewport(MiniCDP *cdp, int *w, int *h);

#ifdef __cplusplus
}
#endif
#endif /* MINI_CDP_H */
