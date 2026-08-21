/*
 * mini_eventloop.h — HTML event loop: tasks, microtasks, render phase, observers.
 *
 * A faithful split of the HTML Standard's event-loop processing model:
 *
 *   • a macrotask FIFO (Task queue) — MessageChannel / postMessage / setTimeout
 *     land here;
 *   • a microtask queue drained to empty at every microtask checkpoint
 *     (Promise.then / queueMicrotask / MutationObserver callbacks);
 *   • a render phase that runs requestAnimationFrame callbacks, then the
 *     IntersectionObserver / ResizeObserver callbacks, before paint;
 *   • the three Observer registries (MutationObserver, IntersectionObserver,
 *     ResizeObserver) with observe/unobserve/disconnect/takeRecords and the
 *     record enqueue + change-detection that drives their callbacks.
 *
 * Pure C99 (no QuickJS dependency) so the queueing/observer logic is unit-
 * testable. The JS bindings (queueMicrotask, MessageChannel, the *Observer
 * constructors) live in mini_webapi.c and call into this; main.c drives the
 * pump + render phase from the host loop.
 */
#ifndef MINI_EVENTLOOP_H
#define MINI_EVENTLOOP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct MiniNode;

/* generic task/microtask callback */
typedef void (*MiniTaskCb)(void *ud);

typedef struct MiniEventLoop MiniEventLoop;

MiniEventLoop *mini_eventloop_create(void);
void mini_eventloop_destroy(MiniEventLoop *el);

/* queue a macrotask (Task). FIFO; one task runs per pump step. */
void mini_eventloop_queue_task(MiniEventLoop *el, MiniTaskCb cb, void *ud);
/* queue a microtask (drained to empty at the next checkpoint). */
void mini_eventloop_queue_microtask(MiniEventLoop *el, MiniTaskCb cb, void *ud);

/* Run one iteration of the event loop: drain the microtask queue to empty,
   then run ONE task (if any). Matches the spec's "perform a microtask
   checkpoint, then pull a task" step. Returns the # of callbacks fired. */
int mini_eventloop_pump(MiniEventLoop *el);

/* Render phase: fire requestAnimationFrame callbacks (registered via the
   bridge), then IntersectionObserver then ResizeObserver change callbacks,
   all using the latest laid-out geometry. `time_ms` is the frame time. */
typedef int (*MiniRafCb)(double time_ms, void *ud);
int mini_eventloop_add_raf(MiniEventLoop *el, MiniRafCb cb, void *ud);
void mini_eventloop_render_phase(MiniEventLoop *el, double time_ms, int viewport_w, int viewport_h);

/* ------------------------------------------------------------------ */
/* MutationObserver                                                   */
/* ------------------------------------------------------------------ */
typedef enum {
    MUT_CHILD_LIST = 1,
    MUT_ATTRIBUTES = 2,
    MUT_CHARACTER_DATA = 4,
    MUT_SUBTREE = 8
} MiniMutationType;
typedef struct MiniMutationObserver MiniMutationObserver;
typedef void (*MiniMutationCb)(MiniMutationObserver *obs, void *ud);

MiniMutationObserver *mini_mutation_observer_create(MiniMutationCb cb, void *ud);
void mini_mutation_observer_destroy(MiniMutationObserver *obs);
/* observe a target subtree (opts is a MiniMutationType bitmask). */
void mini_mutation_observer_observe(MiniMutationObserver *obs, struct MiniNode *target, int opts);
void mini_mutation_observer_disconnect(MiniMutationObserver *obs);
/* drain queued records into obs->cb; returns # records delivered. */
int mini_mutation_observer_take_records(MiniMutationObserver *obs);

/* Called by the DOM (mini_node_append/remove/set_attribute etc.) when a
   mutation happens — enqueues a record to every observer watching the
   mutated node. `el` may be NULL (no loop attached → no-op). */
void mini_eventloop_mutate(MiniEventLoop *el, struct MiniNode *target,
                            MiniMutationType type, struct MiniNode *node,
                            const char *attr_name, const char *attr_value);

/* ------------------------------------------------------------------ */
/* IntersectionObserver                                               */
/* ------------------------------------------------------------------ */
typedef struct MiniIntersectionObserver MiniIntersectionObserver;
typedef void (*MiniIntersectionCb)(MiniIntersectionObserver *obs, int n_entries, void *ud);

MiniIntersectionObserver *mini_intersection_observer_create(MiniIntersectionCb cb, double threshold, void *ud);
void mini_intersection_observer_destroy(MiniIntersectionObserver *obs);
void mini_intersection_observer_observe(MiniIntersectionObserver *obs, struct MiniNode *target);
void mini_intersection_observer_unobserve(MiniIntersectionObserver *obs, struct MiniNode *target);
void mini_intersection_observer_disconnect(MiniIntersectionObserver *obs);

/* ------------------------------------------------------------------ */
/* ResizeObserver                                                     */
/* ------------------------------------------------------------------ */
typedef struct MiniResizeObserver MiniResizeObserver;
typedef void (*MiniResizeCb)(MiniResizeObserver *obs, int n_entries, void *ud);

MiniResizeObserver *mini_resize_observer_create(MiniResizeCb cb, void *ud);
void mini_resize_observer_destroy(MiniResizeObserver *obs);
void mini_resize_observer_observe(MiniResizeObserver *obs, struct MiniNode *target);
void mini_resize_observer_unobserve(MiniResizeObserver *obs, struct MiniNode *target);
void mini_resize_observer_disconnect(MiniResizeObserver *obs);

/* register/unregister observers with the loop (used by mini_webapi.c). */
void mini_eventloop_add_mutation(MiniEventLoop *el, MiniMutationObserver *o);
void mini_eventloop_add_intersection(MiniEventLoop *el, MiniIntersectionObserver *o);
void mini_eventloop_add_resize(MiniEventLoop *el, MiniResizeObserver *o);

/* Self-test: microtask-before-task ordering, nested microtask drain,
   MutationObserver record enqueue + checkpoint delivery, ResizeObserver
   change detection. */
int mini_eventloop_selftest(void);

#ifdef __cplusplus
}
#endif
#endif /* MINI_EVENTLOOP_H */
