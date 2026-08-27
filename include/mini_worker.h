/*
 * mini_worker.h — background I/O worker pool.
 *
 * The renderer/JS run on the main thread; blocking file/network I/O would
 * stall the frame loop. This pool runs pure-C I/O (fopen/fread, socket recv
 * via mini_net_fetch) on worker threads and hands malloc'd buffers back to
 * the main thread, which wraps them into JSValues and fires callbacks during
 * the per-frame pump (mini_worker_pump, called from mini_bridge_pump).
 *
 * Safety contract (mirrors mini_net.c's prefetch worker):
 *   - workers NEVER touch JSValue/JSContext/JSRuntime/GL;
 *   - `cb` is a main-thread JS_DupValue'd copy; workers only memcpy the value
 *     into the result; the main thread pumps and frees it;
 *   - pending/ready queues are guarded by a mutex + condvar.
 */
#ifndef MINI_WORKER_H
#define MINI_WORKER_H

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MiniWorkerQueue MiniWorkerQueue;

typedef enum {
    MW_FS_READ = 0,   /* read a file into a buffer */
    MW_FS_WRITE = 1,  /* write a buffer to a file */
    MW_FETCH = 2      /* HTTP(S) fetch via mini_net_fetch */
} MWKind;

/* One unit of background work. All string fields are owned by the submitter
   until mini_worker_submit() is called, after which the queue owns them (it
   strdup's/moves them and frees them when the task completes). `cb` is a
   JS_DupValue'd value the caller hands over; workers never read it. */
typedef struct {
    MWKind kind;
    char *path;        /* MW_FS_READ / MW_FS_WRITE */
    char *data;        /* MW_FS_WRITE: bytes to write (owned by task) */
    size_t data_len;
    char *method;      /* MW_FETCH: HTTP method (default GET) */
    char *url;        /* MW_FETCH */
    char *body;        /* MW_FETCH: request body (owned by task) */
    size_t body_len;
    char *page_origin; /* MW_FETCH: page origin for CORS context (may be NULL) */
    char *extra_headers; /* MW_FETCH: extra request headers (may be NULL) */
    JSValue cb;        /* callback fired on the main thread */
} MiniWorkerTask;

/* Create a pool with `n_threads` background workers (2-4 typical). */
MiniWorkerQueue *mini_worker_init(int n_threads);

/* Enqueue a task. The queue takes ownership of every string field (so the
   caller must not free them) and of `cb` (caller passes a DupValue'd ref;
   the queue frees it after the callback fires or on shutdown). */
void mini_worker_submit(MiniWorkerQueue *q, MiniWorkerTask *task);

/* Main-thread: drain all ready results and fire their JS callbacks. Must be
   called from the JS thread (e.g. from mini_bridge_pump each frame). */
void mini_worker_pump(MiniWorkerQueue *q, JSContext *ctx);

/* Shutdown: signal workers to stop, join them, free pending/ready items.
   `rt` lets us free stranded JSValue callbacks safely (RT-level). */
void mini_worker_destroy(MiniWorkerQueue *q, JSRuntime *rt);

#ifdef __cplusplus
}
#endif
#endif /* MINI_WORKER_H */
