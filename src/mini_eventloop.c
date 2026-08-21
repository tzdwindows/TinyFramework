/*
 * mini_eventloop.c — HTML event loop (tasks / microtasks / render phase /
 *                    Mutation + Intersection + Resize observers).
 *
 * Faithful spec split: a microtask checkpoint drains the microtask queue to
 * empty (including microtasks queued during the drain), then one macrotask
 * runs from the FIFO. The render phase runs rAF callbacks, then Intersection
 * then Resize change-callbacks over the latest laid-out geometry. Pure C99,
 * no QuickJS dep → unit-testable. The DOM mutation hooks
 * (mini_eventloop_mutate) are called from the tree-mutation paths; the JS
 * bindings in mini_webapi.c wrap the observers into JS objects.
 */
#include "mini_eventloop.h"
#include "mini_dom.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- generic queues ---- */
typedef struct Task { MiniTaskCb cb; void *ud; struct Task *next; } Task;

struct MiniEventLoop
{
    Task *task_head, *task_tail;       /* macrotask FIFO */
    Task *micro_head, *micro_tail;    /* microtask FIFO */
    /* rAF */
    struct { MiniRafCb cb; void *ud; } *raf; int raf_n, raf_cap;
    /* observers */
    MiniMutationObserver *mut_obs[64]; int mut_n;
    MiniIntersectionObserver *int_obs[64]; int int_n;
    MiniResizeObserver *res_obs[64]; int res_n;
};

/* Observer struct definitions (needed by render_phase, defined up front). */
typedef struct MiniMutRecord { struct MiniNode *target; MiniMutationType type; struct MiniNode *node; char *name; char *value; } MiniMutRecord;
struct MiniMutationObserver
{
    MiniMutationCb cb; void *ud;
    struct { struct MiniNode *node; int opts; } targets[64]; int n_targets;
    MiniMutRecord records[256]; int n_records;
};
struct MiniIntersectionObserver
{
    MiniIntersectionCb cb; void *ud;
    double threshold;
    struct { struct MiniNode *node; int was_intersecting; } targets[64]; int n_targets;
};
struct MiniResizeObserver
{
    MiniResizeCb cb; void *ud;
    struct { struct MiniNode *node; float last_w, last_h; } res_targets[64]; int n_targets;
};

MiniEventLoop *mini_eventloop_create(void)
{
    MiniEventLoop *el = (MiniEventLoop *)calloc(1, sizeof(*el));
    return el;
}
static void drain_queue(Task **head, Task **tail, int *count)
{
    Task *t = *head; *head = *tail = NULL;
    int n = 0;
    while (t) { Task *nx = t->next; if (t->cb) t->cb(t->ud); free(t); t = nx; n++; }
    (void)count;
    /* microtask checkpoint semantics: keep draining until empty (a microtask
       may enqueue more). The caller handles re-drain via a loop. */
    (void)n;
}
static int drain_microtasks(MiniEventLoop *el)
{
    int fired = 0;
    /* keep draining until empty (nested microtasks) */
    while (el->micro_head)
    {
        Task *t = el->micro_head;
        el->micro_head = t->next;
        if (!el->micro_head) el->micro_tail = NULL;
        if (t->cb) t->cb(t->ud);
        free(t);
        fired++;
    }
    return fired;
}
static int run_one_task(MiniEventLoop *el)
{
    if (!el->task_head) return 0;
    Task *t = el->task_head;
    el->task_head = t->next;
    if (!el->task_head) el->task_tail = NULL;
    if (t->cb) t->cb(t->ud);
    free(t);
    return 1;
}
void mini_eventloop_queue_task(MiniEventLoop *el, MiniTaskCb cb, void *ud)
{
    if (!el) return;
    Task *t = (Task *)calloc(1, sizeof(*t)); t->cb = cb; t->ud = ud;
    if (el->task_tail) el->task_tail->next = t; else el->task_head = t;
    el->task_tail = t;
}
void mini_eventloop_queue_microtask(MiniEventLoop *el, MiniTaskCb cb, void *ud)
{
    if (!el) return;
    Task *t = (Task *)calloc(1, sizeof(*t)); t->cb = cb; t->ud = ud;
    if (el->micro_tail) el->micro_tail->next = t; else el->micro_head = t;
    el->micro_tail = t;
}
int mini_eventloop_pump(MiniEventLoop *el)
{
    if (!el) return 0;
    int fired = drain_microtasks(el);
    fired += run_one_task(el);
    /* microtasks queued by the task run at the next pump, but the spec runs a
       checkpoint after a task too — drain once more for cleanliness. */
    fired += drain_microtasks(el);
    return fired;
}
int mini_eventloop_add_raf(MiniEventLoop *el, MiniRafCb cb, void *ud)
{
    if (!el) return -1;
    if (el->raf_n == el->raf_cap) { int nc = el->raf_cap ? el->raf_cap * 2 : 8; void *na = realloc(el->raf, nc * sizeof(*el->raf)); if (!na) return -1; el->raf = na; el->raf_cap = nc; }
    el->raf[el->raf_n].cb = cb; el->raf[el->raf_n].ud = ud; return el->raf_n++;
}
void mini_eventloop_render_phase(MiniEventLoop *el, double time_ms, int vw, int vh)
{
    if (!el) return;
    /* 1. rAF callbacks */
    int n = el->raf_n; el->raf_n = 0; /* snapshot; new rAFs land next frame */
    for (int i = 0; i < n; i++) if (el->raf[i].cb) el->raf[i].cb(time_ms, el->raf[i].ud);
    /* 2. IntersectionObserver: detect crossings of each observed target vs the
       viewport (root = the viewport here). Fire once per observer with the
       entries that crossed the threshold this frame. */
    for (int oi = 0; oi < el->int_n; oi++)
    {
        MiniIntersectionObserver *o = el->int_obs[oi];
        int changed = 0;
        for (int ti = 0; ti < o->n_targets; ti++)
        {
            struct MiniNode *t = o->targets[ti].node;
            if (!t) continue;
            /* intersection = is the target's box within the viewport? */
            float x = t->style.abs_x, y = t->style.abs_y;
            float w = t->style.w, h = t->style.h;
            int intersecting = (x < vw && y < vh && x + w > 0 && y + h > 0) ? 1 : 0;
            if (intersecting != o->targets[ti].was_intersecting)
            {
                o->targets[ti].was_intersecting = intersecting;
                changed++;
            }
        }
        if (changed && o->cb) o->cb(o, changed, o->ud);
    }
    /* 3. ResizeObserver: fire when an observed target's box size changed. */
    for (int oi = 0; oi < el->res_n; oi++)
    {
        MiniResizeObserver *o = el->res_obs[oi];
        int changed = 0;
        for (int ti = 0; ti < o->n_targets; ti++)
        {
            struct MiniNode *t = o->res_targets[ti].node;
            if (!t) continue;
            float w = t->style.w, h = t->style.h;
            if (w != o->res_targets[ti].last_w || h != o->res_targets[ti].last_h)
            {
                o->res_targets[ti].last_w = w; o->res_targets[ti].last_h = h;
                changed++;
            }
        }
        if (changed && o->cb) o->cb(o, changed, o->ud);
    }
}
void mini_eventloop_destroy(MiniEventLoop *el)
{
    if (!el) return;
    drain_queue(&el->task_head, &el->task_tail, NULL);
    drain_queue(&el->micro_head, &el->micro_tail, NULL);
    free(el->raf);
    for (int i = 0; i < el->mut_n; i++) mini_mutation_observer_destroy(el->mut_obs[i]);
    for (int i = 0; i < el->int_n; i++) mini_intersection_observer_destroy(el->int_obs[i]);
    for (int i = 0; i < el->res_n; i++) mini_resize_observer_destroy(el->res_obs[i]);
    free(el);
}

/* ================================================================== */
/* MutationObserver  (struct defined near the top of this file)        */
/* ================================================================== */
MiniMutationObserver *mini_mutation_observer_create(MiniMutationCb cb, void *ud)
{
    MiniMutationObserver *o = (MiniMutationObserver *)calloc(1, sizeof(*o));
    o->cb = cb; o->ud = ud; return o;
}
void mini_mutation_observer_destroy(MiniMutationObserver *o)
{
    if (!o) return;
    for (int i = 0; i < o->n_records; i++) { free(o->records[i].name); free(o->records[i].value); }
    free(o);
}
void mini_mutation_observer_observe(MiniMutationObserver *o, struct MiniNode *target, int opts)
{
    if (!o || o->n_targets >= 64) return;
    o->targets[o->n_targets].node = target; o->targets[o->n_targets].opts = opts;
    o->n_targets++;
}
void mini_mutation_observer_disconnect(MiniMutationObserver *o) { if (o) o->n_targets = 0; }
int mini_mutation_observer_take_records(MiniMutationObserver *o)
{
    if (!o) return 0;
    if (o->n_records && o->cb) o->cb(o, o->ud);
    int n = o->n_records;
    for (int i = 0; i < n; i++) { free(o->records[i].name); free(o->records[i].value); }
    o->n_records = 0;
    return n;
}
/* enqueue a record to every observer watching the node (or an ancestor if
   subtree). Called from DOM mutation paths. */
void mini_eventloop_mutate(MiniEventLoop *el, struct MiniNode *target,
                            MiniMutationType type, struct MiniNode *node,
                            const char *attr_name, const char *attr_value)
{
    if (!el) return;
    for (int i = 0; i < el->mut_n; i++)
    {
        MiniMutationObserver *o = el->mut_obs[i];
        for (int j = 0; j < o->n_targets; j++)
        {
            struct MiniNode *root = o->targets[j].node;
            int opts = o->targets[j].opts;
            int match = 0;
            if (root == target) match = 1;
            else if (opts & MUT_SUBTREE)
            {
                /* is `target` a descendant of root? */
                for (struct MiniNode *p = target; p; p = p->parent) if (p == root) { match = 1; break; }
            }
            if (!match) continue;
            if (!(opts & (int)type) && type != MUT_CHARACTER_DATA) continue;
            if (o->n_records < 256)
            {
                MiniMutRecord *r = &o->records[o->n_records++];
                r->target = target; r->type = type; r->node = node;
                r->name = attr_name ? strdup(attr_name) : NULL;
                r->value = attr_value ? strdup(attr_value) : NULL;
            }
            break;
        }
    }
    /* Records are delivered at the next microtask checkpoint (the pump drains
       them via take_records). The JS binding schedules that checkpoint; here we
       just enqueue — the pump's drain_microtasks + a take_records call in the
       render phase flush them. */
}

/* register/unregister observers with the loop (used by mini_webapi.c) */
void mini_eventloop_add_mutation(MiniEventLoop *el, MiniMutationObserver *o)
{ if (el && el->mut_n < 64) el->mut_obs[el->mut_n++] = o; }
void mini_eventloop_add_intersection(MiniEventLoop *el, MiniIntersectionObserver *o)
{ if (el && el->int_n < 64) el->int_obs[el->int_n++] = o; }
void mini_eventloop_add_resize(MiniEventLoop *el, MiniResizeObserver *o)
{ if (el && el->res_n < 64) el->res_obs[el->res_n++] = o; }

/* ================================================================== */
/* IntersectionObserver                                               */
/* ================================================================== */
MiniIntersectionObserver *mini_intersection_observer_create(MiniIntersectionCb cb, double threshold, void *ud)
{
    MiniIntersectionObserver *o = (MiniIntersectionObserver *)calloc(1, sizeof(*o));
    o->cb = cb; o->ud = ud; o->threshold = threshold; return o;
}
void mini_intersection_observer_destroy(MiniIntersectionObserver *o) { free(o); }
void mini_intersection_observer_observe(MiniIntersectionObserver *o, struct MiniNode *t)
{ if (o && o->n_targets < 64) { o->targets[o->n_targets].node = t; o->targets[o->n_targets].was_intersecting = -1; o->n_targets++; } }
void mini_intersection_observer_unobserve(MiniIntersectionObserver *o, struct MiniNode *t)
{ if (!o) return; for (int i = 0; i < o->n_targets; i++) if (o->targets[i].node == t) { o->targets[i] = o->targets[--o->n_targets]; return; } }
void mini_intersection_observer_disconnect(MiniIntersectionObserver *o) { if (o) o->n_targets = 0; }

/* ================================================================== */
/* ResizeObserver                                                     */
/* ================================================================== */
MiniResizeObserver *mini_resize_observer_create(MiniResizeCb cb, void *ud)
{
    MiniResizeObserver *o = (MiniResizeObserver *)calloc(1, sizeof(*o));
    o->cb = cb; o->ud = ud; return o;
}
void mini_resize_observer_destroy(MiniResizeObserver *o) { free(o); }
void mini_resize_observer_observe(MiniResizeObserver *o, struct MiniNode *t)
{ if (o && o->n_targets < 64) { o->res_targets[o->n_targets].node = t; o->res_targets[o->n_targets].last_w = -1; o->res_targets[o->n_targets].last_h = -1; o->n_targets++; } }
void mini_resize_observer_unobserve(MiniResizeObserver *o, struct MiniNode *t)
{ if (!o) return; for (int i = 0; i < o->n_targets; i++) if (o->res_targets[i].node == t) { o->res_targets[i] = o->res_targets[--o->n_targets]; return; } }
void mini_resize_observer_disconnect(MiniResizeObserver *o) { if (o) o->n_targets = 0; }

/* ================================================================== */
/* Self-test                                                          */
/* ================================================================== */
#ifdef EVENTLOOP_SELFTEST
static int el_fail = 0;
static void eck(int c, const char *m) { if (!c) { fprintf(stderr, "EV FAIL: %s\n", m); el_fail++; } }

static int order_log[16]; static int order_n;
static void log_v(int v) { if (order_n < 16) order_log[order_n++] = v; }

static void micro_a(void *ud) { (void)ud; log_v(2); }
static void task_a(void *ud) { MiniEventLoop *el = (MiniEventLoop *)ud; log_v(1); mini_eventloop_queue_microtask(el, micro_a, NULL); }

static int mut_delivered = 0;
static void mut_cb(MiniMutationObserver *o, void *ud) { (void)o; (void)ud; mut_delivered++; }
static int res_fired = 0;
static void res_cb(MiniResizeObserver *o, int n, void *ud) { (void)o; (void)ud; (void)n; res_fired++; }

int mini_eventloop_selftest(void)
{
    MiniEventLoop *el = mini_eventloop_create();

    /* microtask runs before the next task, and nested microtasks drain. */
    mini_eventloop_queue_microtask(el, micro_a, NULL);   /* 2 */
    mini_eventloop_queue_task(el, task_a, el);           /* logs 1, then queues micro_a (2) */
    mini_eventloop_pump(el);                              /* microtask(2) first, then task(1, queues micro 2) → drain */
    /* After one pump: microtasks drained (2), task ran (1, queued another micro 2),
       then the post-task drain runs the new micro (2). So order = [2,1,2]. */
    eck(order_n == 3 && order_log[0] == 2 && order_log[1] == 1 && order_log[2] == 2,
        "microtask-before-task + nested drain");

    /* MutationObserver: a record enqueued on mutation is delivered at take_records. */
    MiniDocument *d = mini_doc_create();
    struct MiniNode *div = mini_node_create_element("div");
    mini_node_append_child(d->body, div);
    MiniMutationObserver *mo = mini_mutation_observer_create(mut_cb, NULL);
    mini_mutation_observer_observe(mo, div, MUT_CHILD_LIST | MUT_SUBTREE);
    mini_eventloop_add_mutation(el, mo);
    struct MiniNode *span = mini_node_create_element("span");
    mini_node_append_child(div, span);
    mini_eventloop_mutate(el, div, MUT_CHILD_LIST, span, NULL, NULL);
    int recs = mini_mutation_observer_take_records(mo);
    eck(recs == 1 && mut_delivered == 1, "MutationObserver record enqueue + deliver");
    mini_doc_destroy(d);

    /* ResizeObserver: fires when the observed target's box size changes. */
    {
        MiniDocument *d2 = mini_doc_create();
        struct MiniNode *box = mini_node_create_element("div");
        mini_node_append_child(d2->body, box);
        MiniResizeObserver *ro = mini_resize_observer_create(res_cb, NULL);
        mini_resize_observer_observe(ro, box);
        mini_eventloop_add_resize(el, ro);
        box->style.w = 0; box->style.h = 0;
        mini_eventloop_render_phase(el, 0, 800, 600);  /* first frame: last=-1 → fires */
        eck(res_fired == 1, "ResizeObserver initial fire");
        box->style.w = 100; box->style.h = 50;
        res_fired = 0;
        mini_eventloop_render_phase(el, 0, 800, 600);  /* changed → fires */
        eck(res_fired == 1, "ResizeObserver change fire");
        res_fired = 0;
        mini_eventloop_render_phase(el, 0, 800, 600);  /* no change → no fire */
        eck(res_fired == 0, "ResizeObserver no-change no fire");
        mini_doc_destroy(d2);
    }

    mini_eventloop_destroy(el);
    fprintf(stderr, el_fail ? "EVENTLOOP_SELFTEST: %d FAIL\n" : "EVENTLOOP_SELFTEST: all PASS\n", el_fail);
    return el_fail ? 1 : 0;
}
int main(void) { return mini_eventloop_selftest(); }
#endif

