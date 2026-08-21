/*
 * mini_js_bridge.h — public surface of the QuickJS binding layer.
 */
#ifndef MINI_JS_BRIDGE_H
#define MINI_JS_BRIDGE_H

#include "quickjs.h"
#include "mini_dom.h"
#include "mini_renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MiniBridge MiniBridge;

/* Create engine + DOM/document globals + install the JS shim. */
MiniBridge *mini_bridge_create(MiniRenderer *r, MiniDocument *doc);
void        mini_bridge_destroy(MiniBridge *b);

/* Attach the W3C event system (mini_events.c). main.c calls this after
   creating both the bridge and the MiniEventState so JS addEventListener
   registers trampolines into the state the host loop drives.            */
struct MiniEventState;
void mini_bridge_set_events(MiniBridge *b, struct MiniEventState *ev);

/* Evaluate a script string (global scope). Returns 0 on success. */
int  mini_bridge_eval(MiniBridge *b, const char *src, size_t len, const char *fn);

/* Phase 1: parse raw HTML into the live DOM, then run inline <script>s. */
int  mini_bridge_load_html(MiniBridge *b, const char *html);

/* Set the page URL used as the base for resolving relative module specifiers
   and as import.meta.url for inline <script type=module>. Call before
   mini_bridge_load_html. Local paths are normalized to file:// URLs. */
void mini_bridge_set_doc_url(MiniBridge *b, const char *url);
const char *mini_bridge_get_doc_url(const MiniBridge *b);

/* Drain microtasks + fire due setTimeout timers. Called every frame. */
void mini_bridge_pump(MiniBridge *b);

/* Fire all queued requestAnimationFrame callbacks with `time_ms`.
   Callbacks registered during firing land in the next frame's queue. */
int  mini_bridge_fire_raf(MiniBridge *b, double time_ms);

/* Number of requestAnimationFrame callbacks currently queued for the next
   frame (i.e. re-registered by a JS animation loop during the previous fire).
   The host loop reads this to tell whether the page is actively animating via
   rAF, so it can skip the full render pipeline (and idle-sleep) when a static
   page has no pending animation work. */
int  mini_bridge_pending_raf(MiniBridge *b);

/* Current QuickJS heap usage in bytes (for the diagnostics monitor). */
size_t mini_bridge_heap_usage(MiniBridge *b);

/* Install a (level,msg) console relay — main.c wires it to CDP. */
void  mini_bridge_set_log_hook(MiniBridge *b,
                               void (*hook)(const char *, const char *, void *),
                               void *ud);

/* Evaluate `expr` and write JSON.stringify(result) into `out`. For CDP. */
int   mini_bridge_eval_to_json(MiniBridge *b, const char *expr, char *out, size_t cap);

/* Direct access to the QuickJS context for the CDP debugger/reflection
   layer (Runtime.getProperties, Debugger.*, HeapProfiler). */
JSContext *mini_bridge_ctx(MiniBridge *b);

/* In-memory localStorage/sessionStorage accessors for the CDP
   DOMStorage/Storage domain. `which`: 0=localStorage, 1=sessionStorage. */
const char *mini_bridge_storage_get(MiniBridge *b, int which, const char *key);
int         mini_bridge_storage_count(MiniBridge *b, int which);
const char *mini_bridge_storage_key(MiniBridge *b, int which, int i);
const char *mini_bridge_storage_val(MiniBridge *b, int which, int i);
void        mini_bridge_storage_set(MiniBridge *b, int which, const char *key, const char *val);
void        mini_bridge_storage_remove(MiniBridge *b, int which, const char *key);
void        mini_bridge_storage_clear(MiniBridge *b, int which);
struct MiniNode *mini_bridge_node_from_js(MiniBridge *b, JSValueConst val);

#ifdef __cplusplus
}
#endif
#endif /* MINI_JS_BRIDGE_H */
