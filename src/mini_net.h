/*
 * mini_net.h - minimal blocking HTTP/1.1 client + request recording, for
 *              the CDP Network domain.
 *
 * HTTP/1.1 client + request recording, for the CDP Network domain.
 * Optional TLS (https) when built with -DMINI_TLS (vendored mbedtls) —
 * requests. The bridge's fetch()/XMLHttpRequest call mini_net_http() and
 * record the result; the Network domain reads the records to emit
 * requestWillBeSent/responseReceived/loadingFinished + getResponseBody.
 */
#ifndef MINI_NET_H
#define MINI_NET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MiniNetRecord
{
    char method[8];
    char url[1024];
    char *req_headers;   /* malloc'd (raw request head) */
    char *req_body;       /* malloc'd, may be NULL */
    size_t req_body_len;
    int  status;          /* HTTP status code; 0 on transport error */
    char *resp_headers;   /* malloc'd (raw response head) */
    char *resp_body;       /* malloc'd (decoded body) */
    size_t resp_body_len;
    char mime[128];       /* Content-Type value */
    double timing_ms;
    int  reported;        /* 1 once the CDP layer has emitted events */
    int  id;              /* CDP requestId == table index + 1 */
    int  failed;          /* 1 if the transport failed (no response) */
} MiniNetRecord;

/* Idempotent winsock init. */
int mini_net_init(void);

/* Blocking HTTP/1.1 request. Fills `out` (stealing malloc'd buffers into
   out->req_headers/req_body/resp_headers/resp_body — caller frees them via
   mini_net_record_free or by moving into the table). Returns 0 on success
   (got a response), -1 on transport failure (out->failed set). */
int mini_net_http(const char *method, const char *url,
                  const char *extra_headers, const char *body, size_t body_len,
                  MiniNetRecord *out);

/* Phase 6 fetch orchestration layered on mini_net_http: HSTS upgrade,
   cookie send, HTTP cache (fresh-serve + conditional revalidation +
   304-from-cache), Set-Cookie capture, HSTS remember, cache store, and a
   CORS gate. `page_origin` is the origin string of the page making the
   request (may be NULL = no SOP/CORS context). */
int mini_net_fetch(const char *method, const char *url,
                   const char *extra_headers, const char *body, size_t body_len,
                   const char *page_origin, MiniNetRecord *out);

/* Move a filled record into the global recording table (steals its malloc'd
   pointers). Assigns an id. */
void mini_net_record_add(MiniNetRecord *rec);

int  mini_net_record_count(void);
const MiniNetRecord *mini_net_record_get(int i); /* NULL if out of range */
void mini_net_record_mark_reported(int i);

/* Free the malloc'd members of a record (does not free the record itself). */
void mini_net_record_free(MiniNetRecord *rec);

/* ---- Phase 7: parallel prefetch + keep-alive ---- */
/* Spawn a detached background thread that GETs `url` and stores the response
   into the HTTP cache, so a later mini_net_fetch() for the same URL is a cache
   hit instead of a fresh TLS handshake. parse_importmap seeds one per mapped
   CDN module; the module loader calls mini_net_prefetch_await() before its own
   mini_net_fetch(). Only http(s) URLs are prefetched; file:// is synchronous. */
void mini_net_prefetch(const char *url);
/* If a prefetch for `url` is in flight, block until it finishes (so the cache
   is warm), then consume the slot. No-op if none. */
void mini_net_prefetch_await(const char *url);
/* Join all live prefetch threads + close the keep-alive connection pool. Call
   at bridge/process teardown so background cache writes land before exit. */
void mini_net_prefetch_shutdown(void);

/* Parse Content-Length / Transfer-Encoding: chunked / Connection: close out of
   a response header block (pure, no I/O -> unit-testable). */
void mini_net_parse_response_meta(const char *hdrs, size_t hlen,
                                 long *out_cl, int *out_chunked, int *out_close);

/* Process-wide proxy / user-agent overrides (set by session.setProxy /
   setUserAgent). The proxy is best-effort HTTP forward-proxy only in this
   build (HTTPS CONNECT is a documented TODO). NULL = no override. */
void        mini_net_set_proxy(const char *url);
const char *mini_net_get_proxy(void);
void        mini_net_set_user_agent(const char *ua);
const char *mini_net_get_user_agent(void);

#ifdef __cplusplus
}
#endif
#endif /* MINI_NET_H */
