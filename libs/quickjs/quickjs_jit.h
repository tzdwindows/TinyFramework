/*
 * quickjs_jit.h - Baseline JIT (SLJIT) for QuickJS - internal header.
 *
 * Copyright (c) 2026 TinyFramework. All rights reserved.
 *
 * Guarded by JS_JIT. When JS_JIT is not defined at compile time, this
 * entire subsystem is absent and the engine is byte-for-byte the stock
 * pure interpreter. The public runtime toggle is JS_EnableJIT().
 *
 * Design (honest scope): this is a *baseline* JIT with a guard + deopt
 * model, not a V8-tier optimizing JIT. Native code is emitted only for
 * the hot spine of integer bytecode; the instant an assumption fails
 * (non-int operand, overflow, unsupported opcode), control returns to
 * the interpreter at the exact opcode boundary. Because guards are
 * evaluated BEFORE sp/pc are mutated, sp and pc are always at a valid
 * opcode boundary on bail, so a single shared bailout path is correct.
 * Semantics are 100% the interpreter's: it owns every non-fast path.
 *
 * This header is an internal header: it is included from within
 * quickjs.c's translation unit (after JSFunctionBytecode / JSStackFrame /
 * JSRuntime are defined), so it may freely reference those types.
 */
#ifndef QUICKJS_JIT_H
#define QUICKJS_JIT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Result of js_jit_run(). */
enum {
    JS_JIT_DONE = 0,  /* function returned; *pret_val holds the result */
    JS_JIT_BAIL = 1,  /* deopt; resume interpreter at *psp / *ppc       */
};

/* Per-entry lifecycle. */
enum {
    JS_JIT_STATUS_NONE     = 0,
    JS_JIT_STATUS_STUB     = 1, /* compiled but always-bail (phase 1)    */
    JS_JIT_STATUS_COMPILED = 2, /* real native code (phase 2, SLJIT)     */
    JS_JIT_STATUS_FREED    = 3, /* entry released; do not touch          */
};

/* One compiled function. Heap-allocated (stable across registry growth)
   and registered by monotonic id for tracing. `bc` is a weak pointer
   cleared by js_jit_invalidate() when the bytecode is freed. */
struct JSJITEntry {
    uint64_t id;             /* monotonic, == index in registry          */
    JSFunctionBytecode *bc;  /* owning bytecode (weak)                   */
    void *code;              /* generated native code (SLJIT); NULL=stub */
    size_t code_size;        /* size of `code` in bytes                   */
    int status;
    JSAtom name;             /* function name atom, for tracing           */
};

/* Per-runtime JIT registry / state. Lives at rt->jit_state. */
struct JSJITState {
    struct JSJITEntry **entries; /* stable heap objects; id == index     */
    size_t count;
    size_t capacity;
    uint64_t next_id;
    int trace_level;   /* 0=off,1=basic,2=verbose (env JS_JIT_TRACE)     */
    int jit_disabled;  /* runtime on/off (JS_EnableJIT / JS_JIT_DISABLE) */
};

/* ---- API (implemented in quickjs_jit.c, inside quickjs.c's TU) ---- */

/* Init / free per-runtime state. */
void js_jit_state_init(JSRuntime *rt);
void js_jit_state_free(JSRuntime *rt);

/* Compile (pre-scan + codegen) a function. Idempotent: sets
   b->jit_compiled on first call. Creates b->jit_entry if the function
   is JIT-able, leaves it NULL otherwise. Returns 1 if an entry was
   created, 0 if not. Honors rt->jit_state->jit_disabled (no-ops). */
int js_jit_compile(JSContext *ctx, JSFunctionBytecode *b);

/* Execute compiled code for b. Updates *psp, *ppc, *pret_val.
   Pre: b->jit_entry != NULL. Returns JS_JIT_DONE or JS_JIT_BAIL. */
int js_jit_run(JSContext *ctx, JSFunctionBytecode *b, JSStackFrame *sf,
               JSValue **psp, uint8_t **ppc, JSValue *pret_val);

/* Release the native code + entry for b (no-op if not compiled). */
void js_jit_invalidate(JSRuntime *rt, JSFunctionBytecode *b);

/* Release all entries (shutdown / debug flush). */
void js_jit_flush_all(JSRuntime *rt);

/* Set trace level at runtime (also seeded from JS_JIT_TRACE env). */
void js_jit_set_trace(int level);

#ifdef __cplusplus
}
#endif

#endif /* QUICKJS_JIT_H */
