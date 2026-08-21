/*
 * quickjs_jit.c - Baseline JIT for QuickJS (SLJIT).
 *
 * This file is #included at the very end of quickjs.c when JS_JIT is
 * defined, so it shares quickjs.c's translation unit and has access to
 * the interpreter's private internals (JSStackFrame, JSFunctionBytecode,
 * opcode_info, js_poll_interrupts, JS_AtomGetStrRT, the js_*_rt
 * allocators, ...). It is never compiled as a standalone translation
 * unit and must not be listed in the build sources.
 *
 * Scope (honest): this is a *baseline* JIT with guard + deopt, not a
 * V8-tier optimizing JIT. Native code is emitted only for the integer
 * fast-path spine of bytecode; any failed assumption bails back to the
 * interpreter at the exact opcode boundary (sp/pc are mutated only
 * after all guards pass), so semantics are entirely the interpreter's.
 *
 * Two build switches:
 *   JS_JIT            - master switch. Enables the plumbing (registry,
 *                       trace, release, entry gate, free hook). When
 *                       off, the engine is the stock interpreter.
 *   JS_JIT_CODEGEN    - additionally enables real SLJIT native codegen
 *                       (and links sljitLir.c). Without it, compile()
 *                       only records a tracked stub entry and js_jit_run
 *                       is dormant (status never reaches COMPILED), so a
 *                       JS_JIT-only build behaves like the interpreter.
 */
#ifdef JS_JIT

#include "quickjs_jit.h"
#include <stdlib.h>
#include <stddef.h>

#ifdef JS_JIT_CODEGEN
#include "sljitLir.h"

/* ------------------------------------------------------------------ */
/* JitCtx: the single pointer argument to every generated function.  */
/* ------------------------------------------------------------------ */
/* The generated entry function takes ONE argument (struct JitCtx *),
   placed by SLJIT into SLJIT_S0 (first saved arg, see SLJIT_ARGS1(W,P)).
   All interpreter state is funneled through it. The hot registers are
   cached in saved registers at entry and written back on exit. */

struct JitCtx {
    JSValue *sp;             /*  0  current operand stack pointer       */
    uint8_t *pc;            /*  8  current program counter (resume pc) */
    JSContext *ctx;         /* 16                                       */
    JSFunctionBytecode *b;  /* 24  (for cpool / atom lookups)          */
    JSStackFrame *sf;       /* 32                                       */
    JSValue *pret_val;      /* 40  function return value slot          */
    JSValue *var_buf;       /* 48  local variable base (= sf->var_buf) */
    JSValue *arg_buf;       /* 56  argument base (= sf->arg_buf)        */
};

#define JCTX_SP      0
#define JCTX_PC      8
#define JCTX_CTX     16
#define JCTX_B       24
#define JCTX_SF      32
#define JCTX_PRET    40
#define JCTX_VARBUF  48
#define JCTX_ARGBUF  56

/* Saved-register allocation (requires SLJIT_NUMBER_OF_SAVED_REGISTERS>=5). */
#define JREG_JCTX  SLJIT_S0   /* JitCtx * (the single arg)             */
#define JREG_SP    SLJIT_S1   /* JSValue *sp                            */
#define JREG_PC    SLJIT_S2   /* uint8_t *pc (resume pc, per-opcode)   */
#define JREG_VAR   SLJIT_S3   /* JSValue *var_buf                      */
#define JREG_ARG   SLJIT_S4   /* JSValue *arg_buf                      */

/* JSValue layout (non-NaN-boxed, 16 bytes): u (8B) at +0, tag (8B) at +8. */
#define JSV_U    0
#define JSV_TAG  8
#define JSV_SIZE 16
#endif /* JS_JIT_CODEGEN */

/* ------------------------------------------------------------------ */
/* Trace                                                              */
/* ------------------------------------------------------------------ */

static int jit_trace_level = 0;
static int jit_trace_inited = 0;

static void jit_trace_seed(void)
{
    if (jit_trace_inited)
        return;
    jit_trace_inited = 1;
    const char *s = getenv("JS_JIT_TRACE");
    if (s) {
        int v = atoi(s);
        jit_trace_level = (v < 0) ? 0 : v;
    }
}

void js_jit_set_trace(int level)
{
    jit_trace_level = (level < 0) ? 0 : level;
}

static const char *jit_atom_name(JSRuntime *rt, JSAtom a,
                                 char *buf, size_t n)
{
    if (!rt)
        return "?";
    return JS_AtomGetStrRT(rt, buf, (int)n, a);
}

/* ------------------------------------------------------------------ */
/* Registry                                                           */
/* ------------------------------------------------------------------ */

static struct JSJITEntry *jit_entry_new(JSRuntime *rt, JSFunctionBytecode *b)
{
    struct JSJITState *st = rt->jit_state;
    struct JSJITEntry *e;

    if (!st)
        return NULL;
    e = js_mallocz_rt(rt, sizeof(*e));
    if (!e)
        return NULL;
    e->id = st->next_id++;
    e->bc = b;
    e->code = NULL;
    e->code_size = 0;
    e->status = JS_JIT_STATUS_STUB;   /* Phase 1: stub (no native code) */
    e->name = b->func_name;

    if (st->count == st->capacity) {
        size_t ncap = st->capacity ? st->capacity * 2 : 64;
        struct JSJITEntry **na;
        na = js_realloc_rt(rt, st->entries, ncap * sizeof(*na));
        if (!na) {
            js_free_rt(rt, e);
            st->next_id--;
            return NULL;
        }
        st->entries = na;
        st->capacity = ncap;
    }
    st->entries[st->count++] = e;
    return e;
}

/* ------------------------------------------------------------------ */
/* Compile / pre-scan                                                 */
/* ------------------------------------------------------------------ */

#ifdef JS_JIT_CODEGEN
/* Phase 2: real SLJIT codegen for an integer fast-path opcode set.
   Declared here, defined further below. Returns 1 on success and
   fills e->code / e->code_size; returns 0 if the function is not
   JIT-able (unsupported opcode present, ...). */
static int jit_codegen(JSContext *ctx, JSFunctionBytecode *b,
                       struct JSJITEntry *e);
#endif

int js_jit_compile(JSContext *ctx, JSFunctionBytecode *b)
{
    JSRuntime *rt = ctx->rt;
    struct JSJITState *st = rt->jit_state;
    struct JSJITEntry *e;
    char nbuf[64];

    if (!st)
        return 0;
    if (b->jit_compiled)
        return (b->jit_entry != NULL);
    b->jit_compiled = 1;

    if (st->jit_disabled)
        return 0;

    /* Phase 1 gate: only ordinary (non-generator, non-async, non-ctor)
       functions. Phase 2 additionally pre-scans the opcode stream and
       refuses functions containing any opcode outside the supported set
       (which also excludes try/catch, calls, property access, ...). */
    if (b->func_kind != JS_FUNC_NORMAL)
        return 0;

    e = jit_entry_new(rt, b);
    if (!e)
        return 0;
    b->jit_entry = e;

#ifdef JS_JIT_CODEGEN
    if (jit_codegen(ctx, b, e)) {
        e->status = JS_JIT_STATUS_COMPILED;
    } else {
        /* Not JIT-able: keep the stub entry (tracked, but never run). */
        e->status = JS_JIT_STATUS_STUB;
    }
#endif

    if (jit_trace_level >= 1) {
        fprintf(stderr, "[jit #%04llu] compile fn=%s len=%d status=%s\n",
                (unsigned long long)e->id,
                jit_atom_name(rt, b->func_name, nbuf, sizeof nbuf),
                b->byte_code_len,
                e->status == JS_JIT_STATUS_COMPILED ? "compiled" : "stub");
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Run                                                                */
/* ------------------------------------------------------------------ */

int js_jit_run(JSContext *ctx, JSFunctionBytecode *b, JSStackFrame *sf,
               JSValue **psp, uint8_t **ppc, JSValue *pret_val)
{
    struct JSJITEntry *e = b->jit_entry;

    if (!e || e->status != JS_JIT_STATUS_COMPILED) {
        /* No native code: bail to the interpreter at the current pc. */
        return JS_JIT_BAIL;
    }

#ifdef JS_JIT_CODEGEN
    {
        /* Phase 2: invoke the generated native code, which takes a single
           JitCtx pointer (SLJIT_S0). It mutates jctx.sp/pc and writes the
           return value (if JS_JIT_DONE) to *jctx.pret_val. On any failed
           guard it leaves sp/pc at a valid opcode boundary and returns
           JS_JIT_BAIL; the interpreter then resumes from there. */
        struct JitCtx jctx;
        int (*fn)(struct JitCtx *) = (int (*)(struct JitCtx *))(void *)e->code;
        int r;
        jctx.sp = *psp;
        jctx.pc = *ppc;
        jctx.ctx = ctx;
        jctx.b = b;
        jctx.sf = sf;
        jctx.pret_val = pret_val;
        jctx.var_buf = sf->var_buf;
        jctx.arg_buf = sf->arg_buf;
        r = fn(&jctx);
        *psp = jctx.sp;
        *ppc = jctx.pc;
        if (r != JS_JIT_DONE)
            r = JS_JIT_BAIL;
        if (jit_trace_level >= 2) {
            fprintf(stderr, "[jit #%04llu] %s pc=%ld\n",
                    (unsigned long long)e->id,
                    r == JS_JIT_DONE ? "done" : "bail",
                    (long)(*ppc - b->byte_code_buf));
        }
        return r;
    }
#else
    /* Phase 1 (JS_JIT only): no native code is ever produced, so this
       path is unreachable. Kept for completeness/safety. */
    (void)ctx;
    (void)sf;
    (void)psp;
    (void)ppc;
    (void)pret_val;
    return JS_JIT_BAIL;
#endif
}

/* ------------------------------------------------------------------ */
/* Release                                                            */
/* ------------------------------------------------------------------ */

static void jit_entry_release_code(struct JSJITEntry *e)
{
    if (e->code) {
#ifdef JS_JIT_CODEGEN
        sljit_free_code(e->code, NULL);
#endif
        e->code = NULL;
        e->code_size = 0;
    }
}

void js_jit_invalidate(JSRuntime *rt, JSFunctionBytecode *b)
{
    struct JSJITEntry *e = b->jit_entry;

    (void)rt;
    if (!e)
        return;
    b->jit_entry = NULL;
    if (e->status == JS_JIT_STATUS_FREED)
        return;
    jit_entry_release_code(e);
    e->status = JS_JIT_STATUS_FREED;
    e->bc = NULL;
    if (jit_trace_level >= 1) {
        fprintf(stderr, "[jit #%04llu] free\n",
                (unsigned long long)e->id);
    }
}

void js_jit_flush_all(JSRuntime *rt)
{
    struct JSJITState *st = rt->jit_state;
    size_t i;

    if (!st)
        return;
    for (i = 0; i < st->count; i++) {
        struct JSJITEntry *e = st->entries[i];
        if (!e || e->status == JS_JIT_STATUS_FREED)
            continue;
        if (e->bc) {
            e->bc->jit_entry = NULL;
            e->bc->jit_compiled = 0;  /* allow recompile on next call */
        }
        jit_entry_release_code(e);
        e->status = JS_JIT_STATUS_FREED;
        e->bc = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Per-runtime state                                                  */
/* ------------------------------------------------------------------ */

void js_jit_state_init(JSRuntime *rt)
{
    struct JSJITState *st;

    jit_trace_seed();
    st = js_mallocz_rt(rt, sizeof(*st));
    rt->jit_state = st;
    if (!st)
        return;  /* OOM: JIT disabled silently */
    /* Seed runtime disable from env (also settable via JS_EnableJIT). */
    {
        const char *d = getenv("JS_JIT_DISABLE");
        if (d && (d[0] == '1' || d[0] == 't' || d[0] == 'T'))
            st->jit_disabled = 1;
    }
}

void js_jit_state_free(JSRuntime *rt)
{
    struct JSJITState *st = rt->jit_state;
    size_t i;

    if (!st)
        return;
    for (i = 0; i < st->count; i++) {
        struct JSJITEntry *e = st->entries[i];
        if (!e)
            continue;
        if (e->status != JS_JIT_STATUS_FREED) {
            jit_entry_release_code(e);
            if (e->bc)
                e->bc->jit_entry = NULL;
        }
        js_free_rt(rt, e);
    }
    js_free_rt(rt, st->entries);
    js_free_rt(rt, st);
    rt->jit_state = NULL;
}

/* Public runtime toggle (declared in quickjs.h when JS_JIT is on). */
void JS_EnableJIT(JSRuntime *rt, int enable)
{
    if (!rt->jit_state)
        return;
    rt->jit_state->jit_disabled = enable ? 0 : 1;
    if (!enable) {
        /* Dropping to interpreter: release compiled code and reset the
           compiled flag so the gate no longer calls js_jit_run. */
        js_jit_flush_all(rt);
    }
}

#ifdef JS_JIT_CODEGEN
/* ================================================================== */
/* Phase 2: SLJIT native codegen                                      */
/* ================================================================== */

/*
 * Calling convention of the generated entry function:
 *
 *   int jit_entry_fn(JSContext *ctx, JSFunctionBytecode *b,
 *                    JSStackFrame *sf, JSValue **psp, uint8_t **ppc,
 *                    JSValue *pret_val);
 *
 * SLJIT register allocation:
 *   SLJIT_S0  = sp   (JSValue *stack pointer)
 *   SLJIT_S1  = pc   (uint8_t *program counter)
 *   SLJIT_S2  = ctx  (JSContext *)
 *   SLJIT_S3  = sf   (JSStackFrame *)
 *   SLJIT_S4  = b    (JSFunctionBytecode *) — for cpool/var offsets
 *   (S5.. free for scratch carry)
 *
 * Invariants mirrored from the interpreter (quickjs.c CASE bodies):
 *   JSValue = { union u (8 bytes), int64_t tag (8 bytes) } = 16 bytes.
 *   JS_TAG_INT=0, JS_TAG_BOOL=1, JS_TAG_NULL=2, JS_TAG_UNDEFINED=3.
 *   stack values grow upward; sp points at the next free slot.
 *   var_buf = sf->var_buf; arg_buf = sf->arg_buf; cpool = b->cpool.
 *
 * Deopt invariant: every opcode evaluates ALL guards BEFORE mutating
 * sp or pc, so on bail sp/pc are already at a valid opcode boundary.
 * A single shared "bail" epilogue writes S0->*psp, S1->*ppc, returns
 * JS_JIT_BAIL. A "done" epilogue pops the return value into *pret_val
 * and returns JS_JIT_DONE.
 */

/* ---- emit helpers (small, local to this TU) -------------------- */

static sljit_s32 jit_poll_helper(JSContext *ctx)
{
    /* nonzero => interrupt/exception pending => the JIT must bail so the
       interpreter re-runs the goto and throws. */
    return js_poll_interrupts(ctx) != 0 ? 1 : 0;
}

/* Slow path for the inlined interrupt poll: only hit when the counter
   reaches zero. Resets the counter and runs the handler. Returns 1 if the
   runtime wants an interrupt (=> JIT must bail). */
static sljit_s32 jit_poll_slow(JSContext *ctx)
{
    return __js_poll_interrupts(ctx) != 0 ? 1 : 0;
}

/* Record a deferred jump that must land on `target_off` (a byte offset into
   b->byte_code_buf). Resolved against per-opcode labels after the emit pass. */
struct jit_fixup {
    struct sljit_jump *jmp;
    int target_off;   /* branch target byte offset (only for kind==BRANCH) */
    int kind;         /* 0=branch, 1=bail, 2=done */
};
#define JIT_FX_BRANCH 0
#define JIT_FX_BAIL   1
#define JIT_FX_DONE   2

/* ---- pre-scan: is every opcode in the supported int fast-path set? ---- */
static int jit_prescan(JSFunctionBytecode *b)
{
    const uint8_t *p = b->byte_code_buf;
    const uint8_t *end = p + b->byte_code_len;
    while (p < end) {
        int op = *p;
        switch (op) {
        case OP_push_i8: case OP_push_i16: case OP_push_i32:
        case OP_push_0: case OP_push_1: case OP_push_2: case OP_push_3:
        case OP_push_4: case OP_push_5: case OP_push_6: case OP_push_7:
        case OP_push_minus1:
        case OP_undefined: case OP_null: case OP_push_false: case OP_push_true:
        case OP_get_loc: case OP_get_loc0: case OP_get_loc1:
        case OP_get_loc2: case OP_get_loc3: case OP_get_loc8:
        case OP_put_loc: case OP_put_loc0: case OP_put_loc1:
        case OP_put_loc2: case OP_put_loc3: case OP_put_loc8:
        case OP_set_loc: case OP_set_loc0: case OP_set_loc1:
        case OP_set_loc2: case OP_set_loc3: case OP_set_loc8:
        case OP_get_arg: case OP_put_arg: case OP_set_arg:
        case OP_get_arg0: case OP_get_arg1: case OP_get_arg2: case OP_get_arg3:
        case OP_put_arg0: case OP_put_arg1: case OP_put_arg2: case OP_put_arg3:
        case OP_set_arg0: case OP_set_arg1: case OP_set_arg2: case OP_set_arg3:
        case OP_add: case OP_sub: case OP_mul:
        case OP_add_loc:
        case OP_lt: case OP_lte: case OP_gt: case OP_gte:
        case OP_eq: case OP_neq: case OP_strict_eq: case OP_strict_neq:
        case OP_goto: case OP_goto8: case OP_goto16:
        case OP_if_true: case OP_if_false:
        case OP_if_true8: case OP_if_false8:
        case OP_return: case OP_return_undef:
            break;
        case OP_push_const8: {
            int idx = p[1];
            if (JS_VALUE_GET_TAG(b->cpool[idx]) != JS_TAG_INT)
                return 0;
            break;
        }
        case OP_push_const: {
            int idx = (int)get_u32(p + 1);
            if (JS_VALUE_GET_TAG(b->cpool[idx]) != JS_TAG_INT)
                return 0;
            break;
        }
        default:
            if (jit_trace_level >= 1)
                fprintf(stderr, "[jit] prescan refuse op=%d (idx %d)\n",
                        op, (int)(p - b->byte_code_buf));
            return 0;  /* unsupported opcode (try/catch, calls, ...) */
        }
        p += short_opcode_info(op).size;
    }
    return 1;
}

/* linear lookup of the opcode index whose byte offset == off */
static int jit_find_label_idx(const int *offs, int nops, int off)
{
    int i;
    for (i = 0; i < nops; i++)
        if (offs[i] == off)
            return i;
    return -1;
}

static int jit_codegen(JSContext *ctx, JSFunctionBytecode *b,
                       struct JSJITEntry *e)
{
    struct sljit_compiler *c;
    struct sljit_label **labels;
    struct sljit_label *bail_label, *done_label;
    struct jit_fixup *fx;
    struct sljit_jump *j;
    int nfx = 0, nops = 0, i;
    int *offs;
    const uint8_t *p, *end;
    uint8_t *bc = b->byte_code_buf;
    void *code;

    if (!jit_prescan(b))
        return 0;
    if (b->byte_code_len <= 0)
        return 0;

    /* count opcodes + record offsets */
    offs = js_mallocz(ctx, sizeof(int) * (b->byte_code_len + 1));
    if (!offs)
        return 0;
    p = bc; end = bc + b->byte_code_len;
    while (p < end) {
        int op = *p;
        offs[nops++] = (int)(p - bc);
        p += short_opcode_info(op).size;
    }

    labels = js_mallocz(ctx, sizeof(struct sljit_label *) * (nops + 1));
    fx = js_mallocz(ctx, sizeof(struct jit_fixup) * (nops * 2 + 8));
    if (!labels || !fx) {
        js_free(ctx, offs);
        js_free(ctx, labels);
        js_free(ctx, fx);
        return 0;
    }

    c = sljit_create_compiler(NULL);
    if (!c) {
        js_free(ctx, offs); js_free(ctx, labels); js_free(ctx, fx);
        return 0;
    }

    /* entry: 1 pointer arg (JitCtx*) -> S0; sp/pc/var/arg cached in S1..S4 */
    sljit_emit_enter(c, 0, SLJIT_ARGS1(W, P), 4, 5, 0);
    sljit_emit_op1(c, SLJIT_MOV, JREG_SP, 0, SLJIT_MEM1(JREG_JCTX), JCTX_SP);
    sljit_emit_op1(c, SLJIT_MOV, JREG_VAR, 0, SLJIT_MEM1(JREG_JCTX), JCTX_VARBUF);
    sljit_emit_op1(c, SLJIT_MOV, JREG_ARG, 0, SLJIT_MEM1(JREG_JCTX), JCTX_ARGBUF);

    /* emit pass */
    p = bc; i = 0;
    while (p < end) {
        int op = *p;
        int ooff = (int)(p - bc);
        sljit_sw slot;        /* var/arg base reg */
        int idx = 0;

        labels[i] = sljit_emit_label(c);
        /* resume pc = this opcode's address (for any bail within) */
        sljit_emit_const(c, SLJIT_MOV, JREG_PC, 0, (sljit_sw)(bc + ooff));

        switch (op) {
        /* ---- push immediate ints ---- */
        case OP_push_0: case OP_push_1: case OP_push_2: case OP_push_3:
        case OP_push_4: case OP_push_5: case OP_push_6: case OP_push_7:
            slot = (sljit_sw)(op - OP_push_0);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_U, SLJIT_IMM, slot);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_TAG, SLJIT_IMM, 0);
            sljit_emit_op2(c, SLJIT_ADD, JREG_SP, 0, JREG_SP, 0, SLJIT_IMM, JSV_SIZE);
            break;
        case OP_push_minus1:
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_U, SLJIT_IMM, (sljit_sw)-1);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_TAG, SLJIT_IMM, 0);
            sljit_emit_op2(c, SLJIT_ADD, JREG_SP, 0, JREG_SP, 0, SLJIT_IMM, JSV_SIZE);
            break;
        case OP_push_i8:
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_U, SLJIT_IMM, (sljit_sw)get_i8(p + 1));
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_TAG, SLJIT_IMM, 0);
            sljit_emit_op2(c, SLJIT_ADD, JREG_SP, 0, JREG_SP, 0, SLJIT_IMM, JSV_SIZE);
            break;
        case OP_push_i16:
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_U, SLJIT_IMM, (sljit_sw)get_i16(p + 1));
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_TAG, SLJIT_IMM, 0);
            sljit_emit_op2(c, SLJIT_ADD, JREG_SP, 0, JREG_SP, 0, SLJIT_IMM, JSV_SIZE);
            break;
        case OP_push_i32:
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_U, SLJIT_IMM, (sljit_sw)get_i32(p + 1));
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_TAG, SLJIT_IMM, 0);
            sljit_emit_op2(c, SLJIT_ADD, JREG_SP, 0, JREG_SP, 0, SLJIT_IMM, JSV_SIZE);
            break;
        case OP_push_const8:
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_U, SLJIT_IMM, (sljit_sw)JS_VALUE_GET_INT(b->cpool[p[1]]));
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_TAG, SLJIT_IMM, 0);
            sljit_emit_op2(c, SLJIT_ADD, JREG_SP, 0, JREG_SP, 0, SLJIT_IMM, JSV_SIZE);
            break;
        case OP_push_const:
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_U, SLJIT_IMM, (sljit_sw)JS_VALUE_GET_INT(b->cpool[get_u32(p + 1)]));
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_TAG, SLJIT_IMM, 0);
            sljit_emit_op2(c, SLJIT_ADD, JREG_SP, 0, JREG_SP, 0, SLJIT_IMM, JSV_SIZE);
            break;
        /* ---- push trivial immediates ---- */
        case OP_undefined:
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_U, SLJIT_IMM, 0);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_TAG, SLJIT_IMM, JS_TAG_UNDEFINED);
            sljit_emit_op2(c, SLJIT_ADD, JREG_SP, 0, JREG_SP, 0, SLJIT_IMM, JSV_SIZE);
            break;
        case OP_null:
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_U, SLJIT_IMM, 0);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_TAG, SLJIT_IMM, JS_TAG_NULL);
            sljit_emit_op2(c, SLJIT_ADD, JREG_SP, 0, JREG_SP, 0, SLJIT_IMM, JSV_SIZE);
            break;
        case OP_push_false:
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_U, SLJIT_IMM, 0);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_TAG, SLJIT_IMM, JS_TAG_BOOL);
            sljit_emit_op2(c, SLJIT_ADD, JREG_SP, 0, JREG_SP, 0, SLJIT_IMM, JSV_SIZE);
            break;
        case OP_push_true:
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_U, SLJIT_IMM, 1);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_TAG, SLJIT_IMM, JS_TAG_BOOL);
            sljit_emit_op2(c, SLJIT_ADD, JREG_SP, 0, JREG_SP, 0, SLJIT_IMM, JSV_SIZE);
            break;
        /* ---- get_loc / get_arg : int fast path, else bail ---- */
        case OP_get_loc0: idx=0; slot=JREG_VAR; goto get_loc_common;
        case OP_get_loc1: idx=1; slot=JREG_VAR; goto get_loc_common;
        case OP_get_loc2: idx=2; slot=JREG_VAR; goto get_loc_common;
        case OP_get_loc3: idx=3; slot=JREG_VAR; goto get_loc_common;
        case OP_get_loc8: idx=p[1]; slot=JREG_VAR; goto get_loc_common;
        case OP_get_loc: idx=(int)get_u16(p+1); slot=JREG_VAR; goto get_loc_common;
        case OP_get_arg: idx=(int)get_u16(p+1); slot=JREG_ARG; goto get_loc_common;
        case OP_get_arg0: idx=0; slot=JREG_ARG; goto get_loc_common;
        case OP_get_arg1: idx=1; slot=JREG_ARG; goto get_loc_common;
        case OP_get_arg2: idx=2; slot=JREG_ARG; goto get_loc_common;
        case OP_get_arg3: idx=3; slot=JREG_ARG; goto get_loc_common;
        get_loc_common:
            /* guard: tag==INT(0) */
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1((sljit_s32)slot), idx*16+JSV_TAG);
            j = sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
            fx[nfx].jmp=j; fx[nfx].kind=JIT_FX_BAIL; nfx++;
            sljit_emit_op1(c, SLJIT_MOV_S32, SLJIT_R0, 0, SLJIT_MEM1((sljit_s32)slot), idx*16+JSV_U);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_U, SLJIT_R0, 0);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), JSV_TAG, SLJIT_IMM, 0);
            sljit_emit_op2(c, SLJIT_ADD, JREG_SP, 0, JREG_SP, 0, SLJIT_IMM, JSV_SIZE);
            break;
        /* ---- put_loc / put_arg / set_loc : int fast path, else bail ---- */
        case OP_put_loc0: idx=0; slot=JREG_VAR; goto put_loc_common;
        case OP_put_loc1: idx=1; slot=JREG_VAR; goto put_loc_common;
        case OP_put_loc2: idx=2; slot=JREG_VAR; goto put_loc_common;
        case OP_put_loc3: idx=3; slot=JREG_VAR; goto put_loc_common;
        case OP_put_loc8: idx=p[1]; slot=JREG_VAR; goto put_loc_common;
        case OP_put_loc: idx=(int)get_u16(p+1); slot=JREG_VAR; goto put_loc_common;
        case OP_put_arg: idx=(int)get_u16(p+1); slot=JREG_ARG; goto put_loc_common;
        case OP_put_arg0: idx=0; slot=JREG_ARG; goto put_loc_common;
        case OP_put_arg1: idx=1; slot=JREG_ARG; goto put_loc_common;
        case OP_put_arg2: idx=2; slot=JREG_ARG; goto put_loc_common;
        case OP_put_arg3: idx=3; slot=JREG_ARG; goto put_loc_common;
        put_loc_common: {
            /* new = sp[-1]; guard new.tag==0; guard old.tag>=0 (no refcount) */
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(JREG_SP), -JSV_SIZE+JSV_TAG);
            j = sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
            fx[nfx].jmp=j; fx[nfx].kind=JIT_FX_BAIL; nfx++;
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1((sljit_s32)slot), idx*16+JSV_TAG);
            j = sljit_emit_cmp(c, SLJIT_SIG_LESS, SLJIT_R1, 0, SLJIT_IMM, 0);
            fx[nfx].jmp=j; fx[nfx].kind=JIT_FX_BAIL; nfx++;
            /* copy sp[-1] (16B) into slot */
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(JREG_SP), -JSV_SIZE+JSV_U);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1((sljit_s32)slot), idx*16+JSV_U, SLJIT_R0, 0);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(JREG_SP), -JSV_SIZE+JSV_TAG);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1((sljit_s32)slot), idx*16+JSV_TAG, SLJIT_R0, 0);
            sljit_emit_op2(c, SLJIT_SUB, JREG_SP, 0, JREG_SP, 0, SLJIT_IMM, JSV_SIZE);
            break;
        }
        case OP_set_loc0: idx=0; slot=JREG_VAR; goto set_loc_common;
        case OP_set_loc1: idx=1; slot=JREG_VAR; goto set_loc_common;
        case OP_set_loc2: idx=2; slot=JREG_VAR; goto set_loc_common;
        case OP_set_loc3: idx=3; slot=JREG_VAR; goto set_loc_common;
        case OP_set_loc8: idx=p[1]; slot=JREG_VAR; goto set_loc_common;
        case OP_set_loc: idx=(int)get_u16(p+1); slot=JREG_VAR; goto set_loc_common;
        case OP_set_arg: idx=(int)get_u16(p+1); slot=JREG_ARG; goto set_loc_common;
        case OP_set_arg0: idx=0; slot=JREG_ARG; goto set_loc_common;
        case OP_set_arg1: idx=1; slot=JREG_ARG; goto set_loc_common;
        case OP_set_arg2: idx=2; slot=JREG_ARG; goto set_loc_common;
        case OP_set_arg3: idx=3; slot=JREG_ARG; goto set_loc_common;
        set_loc_common: {
            /* dup semantics: keep sp[-1]; store copy into slot (int fast path) */
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(JREG_SP), -JSV_SIZE+JSV_TAG);
            j = sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
            fx[nfx].jmp=j; fx[nfx].kind=JIT_FX_BAIL; nfx++;
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1((sljit_s32)slot), idx*16+JSV_TAG);
            j = sljit_emit_cmp(c, SLJIT_SIG_LESS, SLJIT_R1, 0, SLJIT_IMM, 0);
            fx[nfx].jmp=j; fx[nfx].kind=JIT_FX_BAIL; nfx++;
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(JREG_SP), -JSV_SIZE+JSV_U);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1((sljit_s32)slot), idx*16+JSV_U, SLJIT_R0, 0);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(JREG_SP), -JSV_SIZE+JSV_TAG);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1((sljit_s32)slot), idx*16+JSV_TAG, SLJIT_R0, 0);
            /* sp unchanged */
            break;
        }
        /* ---- add / sub / mul : int+int, no overflow, else bail ---- */
        case OP_add: case OP_sub: case OP_mul: {
            sljit_s32 aop = (op==OP_add)?SLJIT_ADD:(op==OP_sub)?SLJIT_SUB:SLJIT_MUL;
            /* guard both int */
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(JREG_SP), -2*JSV_SIZE+JSV_TAG);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(JREG_SP), -JSV_SIZE+JSV_TAG);
            sljit_emit_op2(c, SLJIT_OR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
            j = sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
            fx[nfx].jmp=j; fx[nfx].kind=JIT_FX_BAIL; nfx++;
            /* 64-bit on sign-extended int32s (never overflows 64-bit) */
            sljit_emit_op1(c, SLJIT_MOV_S32, SLJIT_R0, 0, SLJIT_MEM1(JREG_SP), -2*JSV_SIZE+JSV_U);
            sljit_emit_op1(c, SLJIT_MOV_S32, SLJIT_R1, 0, SLJIT_MEM1(JREG_SP), -JSV_SIZE+JSV_U);
            sljit_emit_op2(c, aop, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
            /* overflow iff result != sign_ext_32(result) */
            sljit_emit_op1(c, SLJIT_MOV_S32, SLJIT_R1, 0, SLJIT_R0, 0);
            j = sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_R1, 0);
            fx[nfx].jmp=j; fx[nfx].kind=JIT_FX_BAIL; nfx++;
            if (op == OP_mul) {
                /* conservative -0 guard: bail on any zero product so the
                   interpreter re-runs mul (it distinguishes 0 from -0.0
                   via the sign of the operands). Correct, just cautious. */
                j = sljit_emit_cmp(c, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
                fx[nfx].jmp=j; fx[nfx].kind=JIT_FX_BAIL; nfx++;
            }
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), -2*JSV_SIZE+JSV_U, SLJIT_R0, 0);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), -2*JSV_SIZE+JSV_TAG, SLJIT_IMM, 0);
            sljit_emit_op2(c, SLJIT_SUB, JREG_SP, 0, JREG_SP, 0, SLJIT_IMM, JSV_SIZE);
            break;
        }
        /* ---- add_loc : var[idx] += sp[-1] (int fast path, else bail) ---- */
        case OP_add_loc: {
            int ai = p[1];
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(JREG_VAR), ai*16+JSV_TAG);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(JREG_SP), -JSV_SIZE+JSV_TAG);
            sljit_emit_op2(c, SLJIT_OR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
            j = sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
            fx[nfx].jmp=j; fx[nfx].kind=JIT_FX_BAIL; nfx++;
            sljit_emit_op1(c, SLJIT_MOV_S32, SLJIT_R0, 0, SLJIT_MEM1(JREG_VAR), ai*16+JSV_U);
            sljit_emit_op1(c, SLJIT_MOV_S32, SLJIT_R1, 0, SLJIT_MEM1(JREG_SP), -JSV_SIZE+JSV_U);
            sljit_emit_op2(c, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
            sljit_emit_op1(c, SLJIT_MOV_S32, SLJIT_R1, 0, SLJIT_R0, 0);
            j = sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_R1, 0);
            fx[nfx].jmp=j; fx[nfx].kind=JIT_FX_BAIL; nfx++;
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_VAR), ai*16+JSV_U, SLJIT_R0, 0);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_VAR), ai*16+JSV_TAG, SLJIT_IMM, 0);
            sljit_emit_op2(c, SLJIT_SUB, JREG_SP, 0, JREG_SP, 0, SLJIT_IMM, JSV_SIZE);
            break;
        }
        /* ---- comparisons : int<int -> push bool, else bail ---- */
        case OP_lt: case OP_lte: case OP_gt: case OP_gte:
        case OP_eq: case OP_neq: case OP_strict_eq: case OP_strict_neq: {
            sljit_s32 ct; /* condition to JUMP to "set true" */
            switch (op) {
            case OP_lt: ct=SLJIT_SIG_LESS; break;
            case OP_lte: ct=SLJIT_SIG_LESS_EQUAL; break;
            case OP_gt: ct=SLJIT_SIG_GREATER; break;
            case OP_gte: ct=SLJIT_SIG_GREATER_EQUAL; break;
            case OP_eq: case OP_strict_eq: ct=SLJIT_EQUAL; break;
            default: ct=SLJIT_NOT_EQUAL; break; /* neq, strict_neq */
            }
            /* guard both int */
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(JREG_SP), -2*JSV_SIZE+JSV_TAG);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(JREG_SP), -JSV_SIZE+JSV_TAG);
            sljit_emit_op2(c, SLJIT_OR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
            j = sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
            fx[nfx].jmp=j; fx[nfx].kind=JIT_FX_BAIL; nfx++;
            sljit_emit_op1(c, SLJIT_MOV_S32, SLJIT_R0, 0, SLJIT_MEM1(JREG_SP), -2*JSV_SIZE+JSV_U);
            sljit_emit_op1(c, SLJIT_MOV_S32, SLJIT_R1, 0, SLJIT_MEM1(JREG_SP), -JSV_SIZE+JSV_U);
            /* assume true: store bool(1) at sp[-2] */
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), -2*JSV_SIZE+JSV_U, SLJIT_IMM, 1);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), -2*JSV_SIZE+JSV_TAG, SLJIT_IMM, JS_TAG_BOOL);
            /* if condition true, skip the set-false */
            j = sljit_emit_cmp(c, ct, SLJIT_R0, 0, SLJIT_R1, 0);
            /* condition false -> store bool(0); condition true jumps over it */
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_SP), -2*JSV_SIZE+JSV_U, SLJIT_IMM, 0);
            {
                struct sljit_label *skip = sljit_emit_label(c);
                sljit_set_label(j, skip);
            }
            sljit_emit_op2(c, SLJIT_SUB, JREG_SP, 0, JREG_SP, 0, SLJIT_IMM, JSV_SIZE);
            break;
        }
        /* ---- goto* : poll interrupts, then jump to target ---- */
        case OP_goto: case OP_goto8: case OP_goto16: {
            int rel, target;
            if (op==OP_goto) rel=(int)(int32_t)get_u32(p+1);
            else if (op==OP_goto16) rel=(int)(int16_t)get_u16(p+1);
            else rel=(int)(int8_t)p[1];
            target = ooff + 1 + rel;
            {
                struct sljit_jump *jslow, *jcont;
                struct sljit_label *lslow;
                sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(JREG_JCTX), JCTX_CTX);
                /* inline the interrupt-counter fast path: --counter; the
                   common case (counter>0) falls straight through to the
                   loop-back jump, so there is no branch misprediction.
                   Only when counter<=0 (rare) do we branch to the slow
                   path that calls the handler. */
                sljit_emit_op1(c, SLJIT_MOV_U32, SLJIT_R1, 0,
                               SLJIT_MEM1(SLJIT_R0),
                               (sljit_sw)offsetof(JSContext, interrupt_counter));
                sljit_emit_op2(c, SLJIT_SUB, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 1);
                sljit_emit_op1(c, SLJIT_MOV32, SLJIT_MEM1(SLJIT_R0),
                               (sljit_sw)offsetof(JSContext, interrupt_counter),
                               SLJIT_R1, 0);
                /* rare path (counter<=0) -> slow; forward branch, predicted
                   not-taken, which is correct for the common case. */
                jslow = sljit_emit_cmp(c, SLJIT_SIG_LESS_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
                /* common path: jump to the loop target. */
                j = sljit_emit_jump(c, SLJIT_JUMP);
                fx[nfx].jmp=j; fx[nfx].kind=JIT_FX_BRANCH; fx[nfx].target_off=target; nfx++;
                /* slow path (out of line): call handler, bail on interrupt. */
                lslow = sljit_emit_label(c);
                sljit_set_label(jslow, lslow);
                sljit_emit_icall(c, SLJIT_CALL, SLJIT_ARGS1(W, P),
                                 SLJIT_IMM, (sljit_sw)&jit_poll_slow);
                j = sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_RETURN_REG, 0, SLJIT_IMM, 0);
                fx[nfx].jmp=j; fx[nfx].kind=JIT_FX_BAIL; nfx++;
                jcont = sljit_emit_jump(c, SLJIT_JUMP);
                fx[nfx].jmp=jcont; fx[nfx].kind=JIT_FX_BRANCH; fx[nfx].target_off=target; nfx++;
            }
            break;
        }
        /* ---- if_* : truthy fast path (tag<=UNDEF), else bail ---- */
        case OP_if_true8: case OP_if_false8: case OP_if_true: case OP_if_false: {
            int rel, target; sljit_s32 ct;
            if (op==OP_if_true||op==OP_if_false) rel=(int)(int32_t)get_u32(p+1);
            else rel=(int)(int8_t)p[1];
            target = ooff + 1 + rel;
            /* op1 = sp[-1] */
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(JREG_SP), -JSV_SIZE+JSV_TAG);
            j = sljit_emit_cmp(c, SLJIT_GREATER, SLJIT_R0, 0, SLJIT_IMM, JS_TAG_UNDEFINED);
            fx[nfx].jmp=j; fx[nfx].kind=JIT_FX_BAIL; nfx++;
            sljit_emit_op1(c, SLJIT_MOV_S32, SLJIT_R1, 0, SLJIT_MEM1(JREG_SP), -JSV_SIZE+JSV_U);
            sljit_emit_op2(c, SLJIT_SUB, JREG_SP, 0, JREG_SP, 0, SLJIT_IMM, JSV_SIZE);
            ct = (op==OP_if_true||op==OP_if_true8) ? SLJIT_NOT_EQUAL : SLJIT_EQUAL;
            j = sljit_emit_cmp(c, ct, SLJIT_R1, 0, SLJIT_IMM, 0);
            fx[nfx].jmp=j; fx[nfx].kind=JIT_FX_BRANCH; fx[nfx].target_off=target; nfx++;
            break;
        }
        /* ---- return ---- */
        case OP_return: {
            /* *pret_val = sp[-1]; sp -= 16; -> done */
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(JREG_JCTX), JCTX_PRET);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(JREG_SP), -JSV_SIZE+JSV_U);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0), JSV_U, SLJIT_R1, 0);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(JREG_SP), -JSV_SIZE+JSV_TAG);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0), JSV_TAG, SLJIT_R1, 0);
            sljit_emit_op2(c, SLJIT_SUB, JREG_SP, 0, JREG_SP, 0, SLJIT_IMM, JSV_SIZE);
            j = sljit_emit_jump(c, SLJIT_JUMP);
            fx[nfx].jmp=j; fx[nfx].kind=JIT_FX_DONE; nfx++;
            break;
        }
        case OP_return_undef: {
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(JREG_JCTX), JCTX_PRET);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0), JSV_U, SLJIT_IMM, 0);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0), JSV_TAG, SLJIT_IMM, JS_TAG_UNDEFINED);
            j = sljit_emit_jump(c, SLJIT_JUMP);
            fx[nfx].jmp=j; fx[nfx].kind=JIT_FX_DONE; nfx++;
            break;
        }
        default:
            /* prescan guarantees we never get here */
            j = sljit_emit_jump(c, SLJIT_JUMP);
            fx[nfx].jmp=j; fx[nfx].kind=JIT_FX_BAIL; nfx++;
            break;
        }

        p += short_opcode_info(op).size;
        i++;
    }

    /* unreachable fall-through safety */
    j = sljit_emit_jump(c, SLJIT_JUMP);
    fx[nfx].jmp=j; fx[nfx].kind=JIT_FX_BAIL; nfx++;

    /* bail epilogue: write sp/pc back, return JS_JIT_BAIL */
    bail_label = sljit_emit_label(c);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_JCTX), JCTX_SP, JREG_SP, 0);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_JCTX), JCTX_PC, JREG_PC, 0);
    sljit_emit_return(c, SLJIT_MOV, SLJIT_IMM, JS_JIT_BAIL);

    /* done epilogue: write sp back, return JS_JIT_DONE */
    done_label = sljit_emit_label(c);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(JREG_JCTX), JCTX_SP, JREG_SP, 0);
    sljit_emit_return(c, SLJIT_MOV, SLJIT_IMM, JS_JIT_DONE);

    /* resolve all deferred jumps */
    for (i = 0; i < nfx; i++) {
        if (fx[i].kind == JIT_FX_BAIL)
            sljit_set_label(fx[i].jmp, bail_label);
        else if (fx[i].kind == JIT_FX_DONE)
            sljit_set_label(fx[i].jmp, done_label);
        else {
            int li = jit_find_label_idx(offs, nops, fx[i].target_off);
            if (li >= 0)
                sljit_set_label(fx[i].jmp, labels[li]);
            else
                sljit_set_label(fx[i].jmp, bail_label);
        }
    }

    if (sljit_get_compiler_error(c)) {
        sljit_free_compiler(c);
        js_free(ctx, offs); js_free(ctx, labels); js_free(ctx, fx);
        return 0;
    }
    code = sljit_generate_code(c, 0, NULL);
    sljit_free_compiler(c);
    js_free(ctx, offs); js_free(ctx, labels); js_free(ctx, fx);
    if (!code)
        return 0;
    e->code = code;
    e->status = JS_JIT_STATUS_COMPILED;
    return 1;
}

#endif /* JS_JIT_CODEGEN */

#endif /* JS_JIT */
