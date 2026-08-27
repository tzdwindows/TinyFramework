/*
 * mini_worker.c — background I/O worker pool (see mini_worker.h).
 *
 * Workers run pure-C I/O (file read/write, mini_net_fetch) and hand malloc'd
 * buffers back to the main thread, which wraps them into JSValues and fires
 * the queued callbacks from mini_worker_pump() each frame.
 *
 * Threading primitives are dual-platform, mirroring mini_net.c's prefetch
 * worker: Win = CRITICAL_SECTION + CONDITION_VARIABLE + _beginthreadex;
 * POSIX = pthread_mutex_t + pthread_cond_t + pthread_create.
 */
#include "mini_worker.h"
#include "mini_net.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- platform primitives ---- */
#if defined(_WIN32)
#  include <windows.h>
#  include <process.h>
typedef CRITICAL_SECTION   mw_mutex_t;
typedef CONDITION_VARIABLE mw_cond_t;
typedef HANDLE             mw_thread_t;
static void mw_mtx_init(mw_mutex_t *m)   { InitializeCriticalSection(m); }
static void mw_mtx_lock(mw_mutex_t *m)   { EnterCriticalSection(m); }
static void mw_mtx_unlock(mw_mutex_t *m) { LeaveCriticalSection(m); }
static void mw_cnd_init(mw_cond_t *c)    { InitializeConditionVariable(c); }
static void mw_cnd_wait(mw_cond_t *c, mw_mutex_t *m) { SleepConditionVariableCS(c, m, INFINITE); }
static void mw_cnd_signal(mw_cond_t *c)    { WakeConditionVariable(c); }
static void mw_cnd_broadcast(mw_cond_t *c) { WakeAllConditionVariable(c); }
static int mw_thread_create(mw_thread_t *t, unsigned(__stdcall *fn)(void *), void *arg)
{ *t = (HANDLE)_beginthreadex(NULL, 0, fn, arg, 0, NULL); return *t ? 0 : -1; }
static void mw_thread_join(mw_thread_t t) { if (t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); } }
#  define MW_THREAD_RET unsigned
#  define MW_THREAD_API unsigned __stdcall
#else
#  include <pthread.h>
#  include <unistd.h>
typedef pthread_mutex_t mw_mutex_t;
typedef pthread_cond_t  mw_cond_t;
typedef pthread_t       mw_thread_t;
static void mw_mtx_init(mw_mutex_t *m)   { pthread_mutex_init(m, NULL); }
static void mw_mtx_lock(mw_mutex_t *m)   { pthread_mutex_lock(m); }
static void mw_mtx_unlock(mw_mutex_t *m) { pthread_mutex_unlock(m); }
static void mw_cnd_init(mw_cond_t *c)    { pthread_cond_init(c, NULL); }
static void mw_cnd_wait(mw_cond_t *c, mw_mutex_t *m) { pthread_cond_wait(c, m); }
static void mw_cnd_signal(mw_cond_t *c)    { pthread_cond_signal(c); }
static void mw_cnd_broadcast(mw_cond_t *c) { pthread_cond_broadcast(c); }
static int mw_thread_create(mw_thread_t *t, void *(*fn)(void *), void *arg)
{ return pthread_create(t, NULL, fn, arg) == 0 ? 0 : -1; }
static void mw_thread_join(mw_thread_t t) { if (t) pthread_join(t, NULL); }
#  define MW_THREAD_RET void *
#  define MW_THREAD_API void *
#endif

/* ---- result produced by a worker, consumed on the main thread ---- */
typedef struct {
    MWKind  kind;
    JSValue cb;
    char   *buf;       /* FS_READ: file content; FETCH: resp body */
    size_t  len;
    int     err;
    int     status;    /* FETCH */
    char    mime[128]; /* FETCH */
} MiniWorkerResult;

struct MiniWorkerQueue {
    mw_mutex_t mutex;
    mw_cond_t  cond;
    MiniWorkerTask  **pending; int pend_n, pend_cap;
    MiniWorkerResult **ready;  int ready_n, ready_cap;
    mw_thread_t *threads;
    int n_threads;
    int shutdown;
};

/* ---- worker body: pure-C I/O, no JS ---- */
static void worker_do(const MiniWorkerTask *t, MiniWorkerResult *r)
{
    r->kind = t->kind;
    r->cb = t->cb; /* POD copy of the JSValue; main thread pumps + frees */
    if (t->kind == MW_FS_READ)
    {
        FILE *fp = fopen(t->path ? t->path : "", "rb");
        if (!fp) { r->err = 1; return; }
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz < 0) sz = 0;
        char *buf = (char *)malloc((size_t)sz + 1);
        if (buf)
        {
            size_t rd = fread(buf, 1, (size_t)sz, fp);
            buf[rd] = '\0';
            r->buf = buf;
            r->len = rd;
        }
        else
            r->err = 1;
        fclose(fp);
    }
    else if (t->kind == MW_FS_WRITE)
    {
        FILE *fp = fopen(t->path ? t->path : "", "wb");
        if (!fp) { r->err = 1; return; }
        if (t->data && t->data_len)
            fwrite(t->data, 1, t->data_len, fp);
        fclose(fp);
    }
    else if (t->kind == MW_FETCH)
    {
        MiniNetRecord rec;
        memset(&rec, 0, sizeof rec);
        int rc = mini_net_fetch(t->method ? t->method : "GET",
                               t->url ? t->url : "",
                               t->extra_headers, t->body, t->body_len,
                               t->page_origin, &rec);
        r->status = rec.status;
        if (rc == 0 && rec.resp_body)
        {
            r->buf = rec.resp_body;
            r->len = rec.resp_body_len;
            rec.resp_body = NULL;       /* steal; record_free won't touch it */
            rec.resp_body_len = 0;
            strncpy(r->mime, rec.mime, sizeof(r->mime) - 1);
        }
        else
            r->err = 1;
        mini_net_record_free(&rec);
    }
}

/* free the owned string fields of a task (cb is moved into the result) */
static void task_free_owned(MiniWorkerTask *t)
{
    free(t->path);
    free(t->data);
    free(t->method);
    free(t->url);
    free(t->body);
    free(t->page_origin);
    free(t->extra_headers);
}

static MW_THREAD_API worker_main(void *arg)
{
    MiniWorkerQueue *q = (MiniWorkerQueue *)arg;
    for (;;)
    {
        mw_mtx_lock(&q->mutex);
        while (q->pend_n == 0 && !q->shutdown)
            mw_cnd_wait(&q->cond, &q->mutex);
        if (q->shutdown && q->pend_n == 0)
        {
            mw_mtx_unlock(&q->mutex);
            break;
        }
        MiniWorkerTask *task = q->pending[--q->pend_n];
        mw_mtx_unlock(&q->mutex);

        MiniWorkerResult *r = (MiniWorkerResult *)calloc(1, sizeof(*r));
        if (!r)
        {
            task_free_owned(task);
            free(task);
            continue;
        }
        worker_do(task, r);

        task_free_owned(task);
        free(task);

        mw_mtx_lock(&q->mutex);
        if (q->ready_n >= q->ready_cap)
        {
            int nc = q->ready_cap ? q->ready_cap * 2 : 16;
            MiniWorkerResult **nb = (MiniWorkerResult **)realloc(q->ready,
                                                                 (size_t)nc * sizeof(MiniWorkerResult *));
            if (nb) { q->ready = nb; q->ready_cap = nc; }
        }
        if (q->ready_n < q->ready_cap)
            q->ready[q->ready_n++] = r;
        else
        {
            /* queue full (very unlikely): drop the result + its callback */
            JS_FreeValueRT(NULL, r->cb);
            free(r->buf);
            free(r);
        }
        mw_mtx_unlock(&q->mutex);
    }
    return (MW_THREAD_RET)0;
}

/* ---- public API ---- */

MiniWorkerQueue *mini_worker_init(int n_threads)
{
    if (n_threads < 1) n_threads = 1;
    if (n_threads > 8) n_threads = 8;
    MiniWorkerQueue *q = (MiniWorkerQueue *)calloc(1, sizeof(*q));
    if (!q) return NULL;
    mw_mtx_init(&q->mutex);
    mw_cnd_init(&q->cond);
    q->threads = (mw_thread_t *)calloc((size_t)n_threads, sizeof(mw_thread_t));
    q->n_threads = n_threads;
    for (int i = 0; i < n_threads; i++)
    {
        if (mw_thread_create(&q->threads[i], worker_main, q) != 0)
            q->threads[i] = 0; /* thread didn't start; others still work */
    }
    return q;
}

void mini_worker_submit(MiniWorkerQueue *q, MiniWorkerTask *task)
{
    if (!q || !task) return;
    /* deep-copy the task struct (string pointers move; cb is POD-copied). */
    MiniWorkerTask *t = (MiniWorkerTask *)malloc(sizeof(*t));
    if (!t)
    {
        /* can't enqueue: fire the callback with an error on the main thread
           by dropping it (best-effort). */
        return;
    }
    *t = *task;
    /* the string fields' ownership transfers to `t`; the caller must NOT
       free them after this. (cb ref count: caller DupValue'd; we now own it.) */

    mw_mtx_lock(&q->mutex);
    if (q->pend_n >= q->pend_cap)
    {
        int nc = q->pend_cap ? q->pend_cap * 2 : 16;
        MiniWorkerTask **nb = (MiniWorkerTask **)realloc(q->pending,
                                                         (size_t)nc * sizeof(MiniWorkerTask *));
        if (nb) { q->pending = nb; q->pend_cap = nc; }
    }
    if (q->pend_n < q->pend_cap)
        q->pending[q->pend_n++] = t;
    else
    {
        /* pending full: drop the task (free strings + cb via RT) */
        task_free_owned(t);
        JS_FreeValueRT(NULL, t->cb);
        free(t);
    }
    mw_cnd_signal(&q->cond);
    mw_mtx_unlock(&q->mutex);
}

void mini_worker_pump(MiniWorkerQueue *q, JSContext *ctx)
{
    if (!q) return;
    /* drain the ready queue under the lock into a local batch */
    MiniWorkerResult **batch = NULL;
    int n = 0;
    mw_mtx_lock(&q->mutex);
    if (q->ready_n > 0)
    {
        n = q->ready_n;
        batch = (MiniWorkerResult **)malloc((size_t)n * sizeof(MiniWorkerResult *));
        if (batch) { memcpy(batch, q->ready, (size_t)n * sizeof(MiniWorkerResult *)); q->ready_n = 0; }
        else n = 0;
    }
    mw_mtx_unlock(&q->mutex);

    for (int i = 0; i < n; i++)
    {
        MiniWorkerResult *r = batch[i];
        JSValue args[2];
        if (r->err)
        {
            JSValue e = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, e, "message", JS_NewString(ctx, "I/O error"));
            JS_SetPropertyStr(ctx, e, "name", JS_NewString(ctx, "Error"));
            args[0] = e;
            args[1] = JS_UNDEFINED;
        }
        else
        {
            args[0] = JS_NULL;
            if (r->kind == MW_FS_READ)
                args[1] = r->buf ? JS_NewStringLen(ctx, r->buf, r->len) : JS_NewString(ctx, "");
            else if (r->kind == MW_FS_WRITE)
                args[1] = JS_UNDEFINED;
            else /* MW_FETCH */
            {
                JSValue o = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, o, "body", r->buf ? JS_NewStringLen(ctx, r->buf, r->len) : JS_NewString(ctx, ""));
                JS_SetPropertyStr(ctx, o, "status", JS_NewInt32(ctx, r->status));
                JS_SetPropertyStr(ctx, o, "mime", JS_NewString(ctx, r->mime));
                args[1] = o;
            }
        }
        if (JS_IsFunction(ctx, r->cb))
        {
            JSValue rv = JS_Call(ctx, r->cb, JS_UNDEFINED, 2, args);
            JS_FreeValue(ctx, rv);
        }
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        JS_FreeValue(ctx, r->cb);
        free(r->buf);
        free(r);
    }
    free(batch);
}

void mini_worker_destroy(MiniWorkerQueue *q, JSRuntime *rt)
{
    if (!q) return;
    mw_mtx_lock(&q->mutex);
    q->shutdown = 1;
    mw_cnd_broadcast(&q->cond);
    mw_mtx_unlock(&q->mutex);
    for (int i = 0; i < q->n_threads; i++)
        mw_thread_join(q->threads[i]);

    /* drop anything still pending/ready (stranded callbacks freed at RT level) */
    for (int i = 0; i < q->pend_n; i++)
    {
        MiniWorkerTask *t = q->pending[i];
        if (t)
        {
            task_free_owned(t);
            if (rt) JS_FreeValueRT(rt, t->cb);
            free(t);
        }
    }
    free(q->pending);
    for (int i = 0; i < q->ready_n; i++)
    {
        MiniWorkerResult *r = q->ready[i];
        if (r)
        {
            free(r->buf);
            if (rt) JS_FreeValueRT(rt, r->cb);
            free(r);
        }
    }
    free(q->ready);
    free(q->threads);
    free(q);
}
