/*
 * quickjs_debug.h - Chrome-DevTools-Protocol-style debugger interface
 *                   layered on top of the (patched) QuickJS interpreter.
 *
 * Copyright (c) 2026 TinyFramework. All rights reserved.
 *
 * This is the public surface the CDP layer (mini_cdp_domains.c) calls.
 * It is implemented in quickjs_debug.c, which is #include'd at the very
 * end of quickjs.c's translation unit so it can use QuickJS internals
 * (JSStackFrame, JSFunctionBytecode, find_line_num, get_var_ref,
 * rt->gc_obj_list, ...). Callers must only depend on this header.
 *
 * Stack frames are passed as opaque void* handles ("JSDebugFrameHandle")
 * because struct JSStackFrame is private to quickjs.c; the caller obtains
 * them from js_debug_get_call_stack() and hands them back to
 * js_debug_get_scopes() / js_debug_eval_on_frame() unchanged.
 *
 * Pause model: when a breakpoint (OP_debugger) or a single-step trap fires
 * inside the interpreter, js_debug_on_opcode()/js_debug_step_check() call
 * the registered on_pause callback. That callback (implemented by the CDP
 * layer) emits Debugger.paused and pumps the CDP server's transport loop
 * until js_debug_request_resume() sets the resume mode, then returns.
 * Because QuickJS is single-threaded and the transport poll is
 * non-blocking + reentrant, this nested-loop pause is safe.
 *
 * JIT: while debugging is "active" (any breakpoint set, or paused, or
 * stepping) the interpreter — not the JIT — must run, because OP_debugger
 * and the per-opcode step hook only live in the interpreter. quickjs.c's
 * JIT run-gate honours js_debug_is_active(); on the active/inactive
 * transition js_debug_set_callbacks() flushes existing native code.
 */
#ifndef QUICKJS_DEBUG_H
#define QUICKJS_DEBUG_H

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle to a paused JS stack frame. */
typedef void *JSDebugFrameHandle;

/* ------------------------------------------------------------------ */
/* One call frame (Debugger.paused -> callFrames / Debugger.StackTrace) */
/* ------------------------------------------------------------------ */
typedef struct JSDebugFrameInfo {
    JSDebugFrameHandle handle;   /* hand back to scopes/eval            */
    char *func_name;             /* malloc'd; caller frees              */
    char *script_id;             /* malloc'd "<id>"; caller frees       */
    char *url;                   /* malloc'd filename; caller frees      */
    int  line;                   /* 1-based source line                 */
    int  col;                    /* 1-based column                      */
    int  scope_count;            /* number of scope chains (filled by    */
                                  /* js_debug_get_scopes)                 */
} JSDebugFrameInfo;

/* ------------------------------------------------------------------ */
/* Scope chain (Debugger.paused -> callFrames[i].scopeChain)          */
/* ------------------------------------------------------------------ */
enum {
    JS_DEBUG_SCOPE_GLOBAL   = 0,
    JS_DEBUG_SCOPE_LOCAL    = 1,
    JS_DEBUG_SCOPE_CLOSURE  = 2,
    JS_DEBUG_SCOPE_WITH    = 3,
    JS_DEBUG_SCOPE_CATCH   = 4,
    JS_DEBUG_SCOPE_BLOCK   = 5,
    JS_DEBUG_SCOPE_SCRIPT  = 6,
    JS_DEBUG_SCOPE_EVAL    = 8,
    JS_DEBUG_SCOPE_MODULE  = 9,
};

typedef struct JSDebugVarInfo {
    char *name;        /* malloc'd; caller frees                    */
    char *value_json; /* malloc'd JSON-encoded value; caller frees */
    int  writable;
    int  configurable;
    int  enumerable;
    int  is_exception; /* value_json holds the exception instead    */
} JSDebugVarInfo;

typedef struct JSDebugScopeInfo {
    int type;            /* JS_DEBUG_SCOPE_*                       */
    char *name;         /* malloc'd, may be NULL; caller frees     */
    JSDebugVarInfo *vars;
    int var_count;
    /* objectId for this scope, if you want to expand it lazily; 0 = none */
    const char *object_id; /* NOT owned by caller (static-ish)     */
} JSDebugScopeInfo;

/* ------------------------------------------------------------------ */
/* Breakpoints                                                        */
/* ------------------------------------------------------------------ */
typedef struct JSDebugBreakLocation {
    int  line;
    int  col;
    int  script_id; /* 1-based index into the script list        */
} JSDebugBreakLocation;

/* ------------------------------------------------------------------ */
/* Heap enumeration (HeapProfiler.takeHeapSnapshot)                   */
/* ------------------------------------------------------------------ */
typedef struct {
    /* Called for every GC object, in allocation order. `name` is a
       human-readable class/ctor name (may be empty). `ptr` is a stable
       opaque id for the node (its address). */
    void (*on_node)(void *ud, void *ptr, int class_id, size_t size,
                    const char *name, size_t name_len);
    /* Called for every reference edge from `from_ptr` to `to_ptr`.
       `name` is the property/slot name (may be NULL/empty). */
    void (*on_edge)(void *ud, void *from_ptr, void *to_ptr,
                    const char *name, size_t name_len);
    void *ud;
} JSDebugHeapVisitor;

/* ------------------------------------------------------------------ */
/* Callbacks the CDP layer registers.                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    /* Fired when the interpreter hits a breakpoint/step trap. While
       inside, the CDP layer must emit Debugger.paused and pump the
       transport (mini_cdp_poll) until js_debug_request_resume() has been
       called. `ctx` is the live JSContext (use it to read call frames).
       `reason`: 0=breakpoint, 1=step, 2=exception, 3=other.
       `hit_bp_ids`/`hit_bp_count`: breakpoint ids that fired (reason 0). */
    void (*on_pause)(JSContext *ctx, int reason,
                     const int *hit_bp_ids, int hit_bp_count,
                     void *ud);
    /* Fired after resume is processed and execution is about to
       continue — useful to emit Debugger.resumed. */
    void (*on_resumed)(void *ud);
    void *ud;
} JSDebugCallbacks;

/* Resume modes passed to js_debug_request_resume(). */
enum {
    JS_DEBUG_RESUME_CONTINUE = 0,
    JS_DEBUG_RESUME_STEP_INTO = 1,
    JS_DEBUG_RESUME_STEP_OVER = 2,
    JS_DEBUG_RESUME_STEP_OUT = 3,
};

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

/* State lifecycle (called from JS_NewRuntime / JS_FreeRuntime). */
void js_debug_state_init(JSRuntime *rt);
void js_debug_state_free(JSRuntime *rt);

/* 1 if debugging is active (breakpoints exist, or paused, or stepping):
   the interpreter — not the JIT — must run. */
int js_debug_is_active(JSRuntime *rt);

/* Install/clear the CDP callbacks. Setting non-NULL callbacks marks
   debugging active and flushes any JIT native code so the interpreter
   runs. Passing NULL callbacks (and no breakpoints) deactivates. */
void js_debug_set_callbacks(JSRuntime *rt, const JSDebugCallbacks *cbs);

/* Pause control. request_resume ends a current pause with the given
   step mode; is_paused reflects the nested-loop state. */
void js_debug_request_resume(JSContext *ctx, int mode);
int  js_debug_is_paused(JSRuntime *rt);

/* Pause-on-next: arm a one-shot trap that pauses at the very next
   opcode the interpreter dispatches (Debugger.pause). */
void js_debug_pause_on_next(JSContext *ctx, int reason);

/* Breakpoints. url is the script filename; line is 1-based.
   set_breakpoint returns a bp id (>0) or 0 on failure. */
int  js_debug_set_breakpoint(JSContext *ctx, const char *url, int line,
                             int *out_bp_id);
int  js_debug_remove_breakpoint(JSContext *ctx, int bp_id);
void js_debug_remove_all_breakpoints(JSContext *ctx);

/* List scripts (functions) the debugger knows about, for
   Debugger.getScriptSource / possibleBreakpoints. Returns count and
   fills the out_urls/out_ids arrays (each malloc'd, caller frees the
   arrays and the strings). */
int  js_debug_get_scripts(JSContext *ctx, char ***out_urls,
                          int **out_ids, int max);

/* Get the source text of a function whose bytecode matches `url` (the
   first match). Writes a malloc'd NUL-terminated string to *out_src
   (NULL if unavailable). */
int  js_debug_get_script_source(JSContext *ctx, const char *url,
                                char **out_src);

/* While paused: read the call stack into `out` (capacity `max`).
   Returns the number of frames filled (0 if not paused). Strings in
   each JSDebugFrameInfo are malloc'd; free with js_debug_free_frames. */
int  js_debug_get_call_stack(JSContext *ctx, JSDebugFrameInfo *out,
                            int max);

/* Fill the scope chain for a frame handle (from get_call_stack) into
   `out` (capacity `max`). Returns count filled. */
int  js_debug_get_scopes(JSContext *ctx, JSDebugFrameHandle frame,
                         JSDebugScopeInfo *out, int max);

/* Evaluate `expr` in the scope of a paused frame. On success returns 0
   and writes a malloc'd JSON-encoded value to *out_val_json (caller
   frees). On exception returns -1 and writes a malloc'd message to
   *out_err (caller frees; may be NULL on OOM). */
int  js_debug_eval_on_frame(JSContext *ctx, JSDebugFrameHandle frame,
                            const char *expr, size_t len,
                            char **out_val_json, char **out_err);

/* Free helpers. */
void js_debug_free_frames(JSDebugFrameInfo *frames, int n);
void js_debug_free_scopes(JSDebugScopeInfo *scopes, int n);

/* Heap + CPU sampling (P3 domains; implemented but maybe minimal). */
void js_debug_enumerate_heap(JSRuntime *rt, JSDebugHeapVisitor *v);
int  js_debug_sample_stack(JSContext *ctx, JSDebugFrameInfo *out, int max);

/* ------------------------------------------------------------------ */
/* Interpreter hooks — called ONLY from quickjs.c, not by CDP callers. */
/* `sf` is a (struct JSStackFrame*) passed as void* because the type  */
/* is private to quickjs.c; the implementations (in quickjs_debug.c,   */
/* compiled inside the TU) cast it back.                               */
/* ------------------------------------------------------------------ */

/* Per-opcode step check, invoked at the top of the interpreter loop.
   `pc` points at the opcode byte about to be dispatched. No-op when
   debugging is inactive. May enter the pause nested-loop (step trap). */
void js_debug_step_check(JSContext *ctx, void *sf, const uint8_t *pc);

/* OP_debugger trap. `bp_pc` points at the OP_debugger byte (= pc-1 at
   the case entry, since SWITCH did opcode = *pc++). Restores the
   original opcode there, pauses (nested CDP pump) if this is a real
   breakpoint, and re-arms / arms stepping as needed. The caller then
   re-dispatches from bp_pc. */
void js_debug_on_opcode(JSContext *ctx, void *sf, uint8_t *bp_pc);

/* Called from free_function_bytecode() so the breakpoint table can drop
   any entries pointing at bytecode being freed (avoids dangling refs). */
void js_debug_on_free_bytecode(JSRuntime *rt, void *b);

#ifdef __cplusplus
}
#endif
#endif /* QUICKJS_DEBUG_H */
