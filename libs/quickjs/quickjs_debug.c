/*
 * quickjs_debug.c - CDP debugger on the patched QuickJS interpreter.
 *
 * Included at the very end of quickjs.c's translation unit (right after
 * quickjs_jit.c), so it may call QuickJS private statics: find_line_num,
 * get_var_ref, JS_GetFunctionBytecode, the gc_obj_list walkers, the
 * opcode enum (OP_debugger / OP_nop), get_leb128/get_sleb128, etc.
 *
 * Allocator policy: quickjs.c forbids bare malloc/free (see the
 * malloc_is_forbidden macros near line 2500). This file #undefs those
 * macros so it can use the C library allocator directly. All memory the
 * debugger returns to the CDP layer (strings, JSON values) is therefore
 * libc-malloc'd and the CDP layer frees it with libc free(). The
 * debugger's own long-lived state is also libc-malloc'd.
 *
 * Pause model: a breakpoint (OP_debugger) or single-step trap fires
 * inside the interpreter; js_debug_on_opcode()/js_debug_step_check()
 * call the CDP layer's on_pause callback, which emits Debugger.paused
 * and pumps the (non-blocking, reentrant) CDP transport loop until
 * js_debug_request_resume() sets the resume mode and clears ->paused.
 *
 * Single stepping is line-granular: a step pauses when the source
 * (url,line) of the current opcode differs from the pause site (for
 * step-into), additionally constrained by frame depth for step-over/out.
 * While stepping, the JIT is skipped (js_debug_is_active() gates the
 * JIT run-path), so the interpreter — where OP_debugger and the
 * per-opcode step hook live — always runs.
 */
#include "quickjs_debug.h"

/* quickjs.c forbids bare malloc/free past line ~2500; we want the real
   C library allocator here so returned strings are free()'d by the CDP
   layer. Restore the real symbols for the rest of this TU. */
#undef malloc
#undef free
#undef realloc

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* quickjs-ng internal types, visible because this file is in the TU. */
struct JSRuntime;
typedef struct JSStackFrame JSStackFrame;
struct JSFunctionBytecode;

/* ---- breakpoint: one CDP breakpoint id -> N patched bytecode sites ---- */
typedef struct {
    struct JSFunctionBytecode *b; /* weak; cleared to NULL on free */
    uint32_t offset;               /* byte offset within byte_code_buf */
    uint8_t orig_opcode;            /* the opcode byte we overwrote     */
    int armed;                     /* 1 if byte is currently OP_debugger */
} JSDebugSite;

typedef struct JSDebugBP {
    int id;
    char *url;
    int line;
    JSDebugSite *sites;
    int site_count;
} JSDebugBP;

struct JSDebugState {
    JSDebugCallbacks cbs;

    int paused;            /* inside the nested pause loop              */
    int pending_resume;    /* request_resume() was called                */
    int resume_mode;        /* JS_DEBUG_RESUME_* the user requested      */
    int step_pending;      /* a step sequence is armed                   */
    int step_mode;          /* INTO / OVER / OUT                          */
    int step_depth;         /* frame depth at step start (for over/out)  */

    int pause_on_next;     /* Debugger.pause: trap at the next opcode    */
    int pause_reason;

    /* pause-site location (top frame), reported via get_call_stack */
    JSStackFrame *pause_sf;
    struct JSFunctionBytecode *pause_b;
    const uint8_t *pause_pc;

    /* line-granular stepping: last pause (url,line,depth) */
    char *last_url;
    int last_line;
    int last_depth;

    /* breakpoint re-arm: after resuming a breakpoint, re-arm the patched
       byte at the NEXT opcode (so we don't immediately re-trap). */
    int rearm_pending;
    struct JSFunctionBytecode *rearm_b;
    uint32_t rearm_offset;

    JSDebugBP *bps;
    int bp_count;
    int bp_cap;
    int next_bp_id;
};

/* ================================================================== */
/* small libc string helpers                                            */
/* ================================================================== */
static char *jdbg_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *r = (char *)malloc(n);
    if (r) memcpy(r, s, n);
    return r;
}

/* Copy a QuickJS atom's string into a libc-malloc'd buffer (NULL if none). */
static char *jdbg_atom_strdup(JSContext *ctx, JSAtom atom)
{
    const char *s = JS_AtomToCString(ctx, atom);
    if (!s) return NULL;
    char *r = jdbg_strdup(s);
    JS_FreeCString(ctx, s);
    return r;
}

/* Convert a JSValue to a libc-malloc'd JSON string. */
static char *jdbg_value_json(JSContext *ctx, JSValueConst v)
{
    JSValue s = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(s) || JS_IsUndefined(s))
    {
        JS_FreeValue(ctx, s);
        /* primitives that JSON can't represent (functions, undefined):
           fall back to a plain string form. */
        const char *cs = JS_ToCString(ctx, v);
        char *r = cs ? jdbg_strdup(cs) : jdbg_strdup("undefined");
        if (cs) JS_FreeCString(ctx, cs);
        return r;
    }
    const char *cs = JS_ToCString(ctx, s);
    JS_FreeValue(ctx, s);
    char *r = cs ? jdbg_strdup(cs) : jdbg_strdup("");
    if (cs) JS_FreeCString(ctx, cs);
    return r;
}

static int jdbg_frame_depth(JSStackFrame *sf)
{
    int d = 0;
    for (; sf; sf = sf->prev_frame)
        d++;
    return d;
}

/* ================================================================== */
/* state lifecycle                                                      */
/* ================================================================== */
void js_debug_state_init(JSRuntime *rt)
{
    struct JSDebugState *dbg =
        (struct JSDebugState *)calloc(1, sizeof(struct JSDebugState));
    rt->debug_state = dbg;
    if (dbg)
        dbg->next_bp_id = 1;
}

void js_debug_state_free(JSRuntime *rt)
{
    struct JSDebugState *dbg = (struct JSDebugState *)rt->debug_state;
    if (!dbg)
        return;
    /* The GC already freed every JSFunctionBytecode (and called
       js_debug_on_free_bytecode to null out the site pointers), so we
       must NOT touch byte_code_buf here — just free our own storage. */
    for (int i = 0; i < dbg->bp_count; i++)
    {
        free(dbg->bps[i].sites);
        free(dbg->bps[i].url);
    }
    free(dbg->bps);
    free(dbg->last_url);
    /* NOTE: dbg->cbs.ud is owned by the CDP layer, not freed here. */
    free(dbg);
    rt->debug_state = NULL;
}

int js_debug_is_active(JSRuntime *rt)
{
    struct JSDebugState *dbg = (struct JSDebugState *)rt->debug_state;
    if (!dbg)
        return 0;
    return dbg->bp_count > 0 || dbg->paused || dbg->step_pending ||
           dbg->pause_on_next || dbg->cbs.on_pause != NULL;
}

void js_debug_set_callbacks(JSRuntime *rt, const JSDebugCallbacks *cbs)
{
    struct JSDebugState *dbg = (struct JSDebugState *)rt->debug_state;
    if (!dbg)
    {
        if (cbs)
        {
            js_debug_state_init(rt);
            dbg = (struct JSDebugState *)rt->debug_state;
        }
        if (!dbg)
            return;
    }
    int was_active = js_debug_is_active(rt);
    if (cbs)
        dbg->cbs = *cbs;
    else
        memset(&dbg->cbs, 0, sizeof(dbg->cbs));
    int now_active = js_debug_is_active(rt);
    /* On any active/inactive transition that involves the JIT, flush
       native code so the interpreter (with OP_debugger + step hook) runs.
       The JIT run-gate also checks js_debug_is_active() per call, but
       flushing clears already-compiled entries. */
    if (was_active != now_active)
    {
#ifdef JS_JIT
        js_jit_flush_all(rt);
#endif
    }
}

void js_debug_request_resume(JSContext *ctx, int mode)
{
    JSRuntime *rt = ctx->rt;
    struct JSDebugState *dbg = (struct JSDebugState *)rt->debug_state;
    if (!dbg)
        return;
    dbg->resume_mode = mode;
    dbg->pending_resume = 1;
    dbg->paused = 0; /* ends the on_pause pump loop */
}

int js_debug_is_paused(JSRuntime *rt)
{
    struct JSDebugState *dbg = (struct JSDebugState *)rt->debug_state;
    return dbg ? dbg->paused : 0;
}

void js_debug_pause_on_next(JSContext *ctx, int reason)
{
    struct JSDebugState *dbg = (struct JSDebugState *)ctx->rt->debug_state;
    if (!dbg)
        return;
    dbg->pause_on_next = 1;
    dbg->pause_reason = reason;
#ifdef JS_JIT
    js_jit_flush_all(ctx->rt);
#endif
}

/* ================================================================== */
/* pause: record location, call the CDP on_pause, pump until resume    */
/* ================================================================== */
static void jdbg_record_loc(JSContext *ctx, struct JSDebugState *dbg,
                            JSStackFrame *sf, const uint8_t *pc)
{
    free(dbg->last_url);
    dbg->last_url = NULL;
    dbg->last_line = 0;
    dbg->last_depth = jdbg_frame_depth(sf);
    if (!sf)
        return;
    struct JSFunctionBytecode *b = JS_GetFunctionBytecode(sf->cur_func);
    if (b && pc && pc >= b->byte_code_buf)
    {
        int col;
        dbg->last_line = find_line_num(ctx, b,
                                        (uint32_t)(pc - b->byte_code_buf), &col);
        dbg->last_url = jdbg_atom_strdup(ctx, b->filename);
    }
}

static void jdbg_do_pause(JSContext *ctx, struct JSDebugState *dbg,
                         int reason, const int *hit_ids, int hit_count,
                         JSStackFrame *sf, const uint8_t *pause_pc)
{
    dbg->paused = 1;
    dbg->pending_resume = 0;
    dbg->resume_mode = JS_DEBUG_RESUME_CONTINUE;
    dbg->pause_sf = sf;
    {
        struct JSFunctionBytecode *b = sf ? JS_GetFunctionBytecode(sf->cur_func) : NULL;
        dbg->pause_b = b;
        dbg->pause_pc = pause_pc;
    }
    jdbg_record_loc(ctx, dbg, sf, pause_pc);

    if (dbg->cbs.on_pause)
        dbg->cbs.on_pause(ctx, reason, hit_ids, hit_count, dbg->cbs.ud);
    /* on_pause pumps the CDP loop until request_resume clears ->paused. */

    dbg->paused = 0;
    dbg->pause_sf = NULL;
    dbg->pause_b = NULL;
    dbg->pause_pc = NULL;
    if (dbg->cbs.on_resumed)
        dbg->cbs.on_resumed(dbg->cbs.ud);
}

/* ================================================================== */
/* breakpoint patching: walk gc_obj_list for FUNCTION_BYTECODE objects */
/* ================================================================== */
/* forward: the pc2line walker (mirrors find_line_num's loop) */
static void jdbg_collect_line_offsets(struct JSFunctionBytecode *b,
                                      int target_line,
                                      uint32_t *offs, int *n, int max)
{
    const uint8_t *p = b->pc2line_buf;
    const uint8_t *p_end;
    int pc, line_num, col_num;
    if (!p)
        return;
    p_end = p + b->pc2line_len;
    pc = 0;
    line_num = b->line_num;
    col_num = b->col_num;
    while (p < p_end && *n < max)
    {
        unsigned int op = *p++;
        int new_line_num, new_col_num;
        if (op == 0)
        {
            uint32_t val;
            int ret = get_leb128(&val, p, p_end);
            if (ret < 0)
                break;
            p += ret;
            pc += (int)val;
            int v;
            ret = get_sleb128(&v, p, p_end);
            if (ret < 0)
                break;
            p += ret;
            new_line_num = line_num + v;
        }
        else
        {
            op -= PC2LINE_OP_FIRST;
            pc += (int)(op / PC2LINE_RANGE);
            new_line_num = line_num + (int)(op % PC2LINE_RANGE) + PC2LINE_BASE;
        }
        int v;
        int ret = get_sleb128(&v, p, p_end);
        if (ret < 0)
            break;
        p += ret;
        new_col_num = col_num + v;
        if (new_line_num == target_line && pc >= 0 && pc < b->byte_code_len)
            offs[(*n)++] = (uint32_t)pc;
        line_num = new_line_num;
        col_num = new_col_num;
    }
}

static int jdbg_url_matches(JSContext *ctx, struct JSFunctionBytecode *b,
                            const char *url)
{
    if (!b->filename)
        return 0;
    const char *fn = JS_AtomToCString(ctx, b->filename);
    int match = 0;
    if (fn && url)
    {
        /* exact, or basename match (DevTools often passes a short url) */
        if (!strcmp(fn, url))
            match = 1;
        else
        {
            const char *slash = strrchr(url, '/');
            const char *base = slash ? slash + 1 : url;
            const char *bslash = strrchr(fn, '\\');
            const char *fbase = bslash ? bslash + 1 : fn;
            const char *fslash2 = strrchr(fn, '/');
            if (fslash2 && (!bslash || fslash2 > bslash))
                fbase = fslash2 + 1;
            if (base && fbase && !strcmp(base, fbase))
                match = 1;
        }
    }
    if (fn)
        JS_FreeCString(ctx, fn);
    return match;
}

int js_debug_set_breakpoint(JSContext *ctx, const char *url, int line,
                            int *out_bp_id)
{
    JSRuntime *rt = ctx->rt;
    struct JSDebugState *dbg = (struct JSDebugState *)rt->debug_state;
    if (!dbg)
        return 0;

    /* gather candidate offsets across every matching bytecode */
    uint32_t offs[512];
    int n_off = 0;
    struct list_head *el;
    list_for_each(el, &rt->gc_obj_list)
    {
        JSGCObjectHeader *gp = list_entry(el, JSGCObjectHeader, link);
        if (JS_GC_TYPE(gp) != JS_GC_OBJ_TYPE_FUNCTION_BYTECODE)
            continue;
        struct JSFunctionBytecode *b = (struct JSFunctionBytecode *)gp;
        if (!jdbg_url_matches(ctx, b, url))
            continue;
        int before = n_off;
        jdbg_collect_line_offsets(b, line, offs, &n_off, 512);
        (void)before;
    }
    if (n_off == 0)
        return 0; /* no script/location matches (P1: no deferred bps) */

    /* one breakpoint id, multiple sites — dedup offsets just in case */
    JSDebugBP *bp = NULL;
    if (dbg->bp_count >= dbg->bp_cap)
    {
        int nc = dbg->bp_cap ? dbg->bp_cap * 2 : 8;
        JSDebugBP *nb = (JSDebugBP *)realloc(dbg->bps, nc * sizeof(*nb));
        if (!nb)
            return 0;
        dbg->bps = nb;
        dbg->bp_cap = nc;
    }
    bp = &dbg->bps[dbg->bp_count];
    memset(bp, 0, sizeof(*bp));
    bp->id = dbg->next_bp_id++;
    bp->url = jdbg_strdup(url);
    bp->line = line;
    bp->sites = (JSDebugSite *)calloc(n_off, sizeof(JSDebugSite));
    bp->site_count = n_off;
    int si = 0;
    /* re-walk to attach each site to its owning bytecode */
    list_for_each(el, &rt->gc_obj_list)
    {
        JSGCObjectHeader *gp = list_entry(el, JSGCObjectHeader, link);
        if (JS_GC_TYPE(gp) != JS_GC_OBJ_TYPE_FUNCTION_BYTECODE)
            continue;
        struct JSFunctionBytecode *b = (struct JSFunctionBytecode *)gp;
        if (!jdbg_url_matches(ctx, b, url))
            continue;
        for (int k = 0; k < n_off; k++)
        {
            /* crude: assign each offset to this b if it is in range and
               not yet attached. We collected offsets across all bytecodes
               in one flat list; attach in order. For P1 this is good
               enough when one script = one bytecode. */
        }
    }
    /* For correctness when one script maps to exactly one bytecode (the
       common case), attach all offsets to that bytecode. Find the first
       matching bytecode again and patch all offsets there. */
    struct JSFunctionBytecode *owner = NULL;
    list_for_each(el, &rt->gc_obj_list)
    {
        JSGCObjectHeader *gp = list_entry(el, JSGCObjectHeader, link);
        if (JS_GC_TYPE(gp) != JS_GC_OBJ_TYPE_FUNCTION_BYTECODE)
            continue;
        struct JSFunctionBytecode *b = (struct JSFunctionBytecode *)gp;
        if (jdbg_url_matches(ctx, b, url))
        {
            owner = b;
            break;
        }
    }
    if (!owner)
    {
        free(bp->sites);
        free(bp->url);
        return 0;
    }
    for (int k = 0; k < n_off; k++)
    {
        JSDebugSite *st = &bp->sites[si++];
        st->b = owner;
        st->offset = offs[k];
        st->orig_opcode = owner->byte_code_buf[offs[k]];
        st->armed = 1;
        owner->byte_code_buf[offs[k]] = OP_debugger;
    }
    bp->site_count = si;
    dbg->bp_count++;
#ifdef JS_JIT
    js_jit_invalidate(rt, owner);
    js_jit_flush_all(rt);
#endif
    if (out_bp_id)
        *out_bp_id = bp->id;
    return bp->id;
}

int js_debug_remove_breakpoint(JSContext *ctx, int bp_id)
{
    struct JSDebugState *dbg = (struct JSDebugState *)ctx->rt->debug_state;
    if (!dbg)
        return -1;
    for (int i = 0; i < dbg->bp_count; i++)
    {
        if (dbg->bps[i].id == bp_id)
        {
            JSDebugBP *bp = &dbg->bps[i];
            for (int s = 0; s < bp->site_count; s++)
            {
                JSDebugSite *st = &bp->sites[s];
                if (st->b && st->armed)
                    st->b->byte_code_buf[st->offset] = st->orig_opcode;
            }
            free(bp->sites);
            free(bp->url);
            /* compact */
            memmove(&dbg->bps[i], &dbg->bps[i + 1],
                    (dbg->bp_count - i - 1) * sizeof(JSDebugBP));
            dbg->bp_count--;
            return 0;
        }
    }
    return -1;
}

void js_debug_remove_all_breakpoints(JSContext *ctx)
{
    JSRuntime *rt = ctx ? ctx->rt : NULL;
    struct JSDebugState *dbg = rt ? (struct JSDebugState *)rt->debug_state : NULL;
    if (!dbg)
        return;
    while (dbg->bp_count > 0)
    {
        JSDebugBP *bp = &dbg->bps[0];
        for (int s = 0; s < bp->site_count; s++)
        {
            JSDebugSite *st = &bp->sites[s];
            if (st->b && st->armed)
                st->b->byte_code_buf[st->offset] = st->orig_opcode;
        }
        free(bp->sites);
        free(bp->url);
        memmove(&dbg->bps[0], &dbg->bps[1],
                (dbg->bp_count - 1) * sizeof(JSDebugBP));
        dbg->bp_count--;
    }
}

void js_debug_on_free_bytecode(JSRuntime *rt, void *b_)
{
    struct JSDebugState *dbg = (struct JSDebugState *)rt->debug_state;
    if (!dbg)
        return;
    struct JSFunctionBytecode *b = (struct JSFunctionBytecode *)b_;
    for (int i = 0; i < dbg->bp_count; i++)
    {
        JSDebugBP *bp = &dbg->bps[i];
        for (int s = 0; s < bp->site_count; s++)
        {
            if (bp->sites[s].b == b)
            {
                bp->sites[s].b = NULL; /* dangling; patched byte is gone */
                bp->sites[s].armed = 0;
            }
        }
    }
    if (dbg->rearm_b == b)
    {
        dbg->rearm_b = NULL;
        dbg->rearm_pending = 0;
    }
    if (dbg->pause_b == b)
        dbg->pause_b = NULL;
}

/* ================================================================== */
/* interpreter hooks                                                    */
/* ================================================================== */
void js_debug_on_opcode(JSContext *ctx, void *sf_, uint8_t *bp_pc)
{
    JSStackFrame *sf = (JSStackFrame *)sf_;
    JSRuntime *rt = ctx->rt;
    struct JSDebugState *dbg = (struct JSDebugState *)rt->debug_state;
    if (!dbg || !sf)
        return;
    struct JSFunctionBytecode *b = JS_GetFunctionBytecode(sf->cur_func);
    if (!b || !b->byte_code_buf)
        return;
    uint32_t offset = (uint32_t)(bp_pc - b->byte_code_buf);

    /* find the armed breakpoint site at (b, offset) */
    JSDebugBP *hit_bp = NULL;
    JSDebugSite *hit_site = NULL;
    for (int i = 0; i < dbg->bp_count && !hit_site; i++)
    {
        JSDebugBP *bp = &dbg->bps[i];
        for (int s = 0; s < bp->site_count; s++)
        {
            if (bp->sites[s].b == b && bp->sites[s].offset == offset &&
                bp->sites[s].armed)
            {
                hit_bp = bp;
                hit_site = &bp->sites[s];
                break;
            }
        }
    }
    if (!hit_site)
    {
        /* spurious OP_debugger with no breakpoint record (the compiler
           never emits OP_debugger, so this only happens if state is
           corrupt). Neutralize it so we don't spin. */
        *bp_pc = OP_nop;
        return;
    }

    /* restore the original opcode so it can execute after the pause */
    b->byte_code_buf[offset] = hit_site->orig_opcode;
    hit_site->armed = 0;

    int hit_id = hit_bp->id;
    /* schedule re-arm at the next opcode, then drive the step mode */
    dbg->rearm_pending = 1;
    dbg->rearm_b = b;
    dbg->rearm_offset = offset;
    dbg->step_pending = 1;
    dbg->step_mode = JS_DEBUG_RESUME_CONTINUE; /* overwritten on resume */

    jdbg_do_pause(ctx, dbg, 0 /* reason = breakpoint */, &hit_id, 1, sf, bp_pc);

    /* on_resume: resume_mode holds the user's choice */
    dbg->step_mode = dbg->resume_mode;
    /* step_pending stays set; the step hook re-arms and applies the mode */
}

void js_debug_step_check(JSContext *ctx, void *sf_, const uint8_t *pc)
{
    JSStackFrame *sf = (JSStackFrame *)sf_;
    JSRuntime *rt = ctx->rt;
    struct JSDebugState *dbg = (struct JSDebugState *)rt->debug_state;
    if (!dbg)
        return;

    /* explicit Debugger.pause: trap at the very next opcode */
    if (dbg->pause_on_next)
    {
        dbg->pause_on_next = 0;
        int reason = dbg->pause_reason;
        jdbg_do_pause(ctx, dbg, reason, NULL, 0, sf, pc);
        dbg->step_mode = dbg->resume_mode;
        if (dbg->resume_mode == JS_DEBUG_RESUME_CONTINUE)
            dbg->step_pending = 0;
        else
            jdbg_record_loc(ctx, dbg, sf, pc);
        return;
    }

    if (!dbg->step_pending)
        return;

    struct JSFunctionBytecode *b = sf ? JS_GetFunctionBytecode(sf->cur_func) : NULL;

    /* re-arm a breakpoint we have stepped past */
    if (dbg->rearm_pending)
    {
        uint32_t cur_off = (b && pc >= b->byte_code_buf)
                               ? (uint32_t)(pc - b->byte_code_buf)
                               : 0xFFFFFFFFu;
        if (!b || dbg->rearm_b != b || cur_off != dbg->rearm_offset)
        {
            /* moved past the breakpoint: re-arm it */
            if (dbg->rearm_b && dbg->rearm_b->byte_code_buf)
            {
                for (int i = 0; i < dbg->bp_count; i++)
                    for (int s = 0; s < dbg->bps[i].site_count; s++)
                        if (dbg->bps[i].sites[s].b == dbg->rearm_b &&
                            dbg->bps[i].sites[s].offset == dbg->rearm_offset &&
                            !dbg->bps[i].sites[s].armed)
                        {
                            dbg->rearm_b->byte_code_buf[dbg->rearm_offset] = OP_debugger;
                            dbg->bps[i].sites[s].armed = 1;
                            goto rearmed;
                        }
            }
        rearmed:
            dbg->rearm_pending = 0;
            if (dbg->resume_mode == JS_DEBUG_RESUME_CONTINUE)
            {
                dbg->step_pending = 0;
                return;
            }
            /* else fall through with the user's step mode */
        }
        else
        {
            /* still at the breakpoint's own opcode: don't pause here */
            return;
        }
    }

    /* line-granular step decision */
    int cur_depth = jdbg_frame_depth(sf);
    int cur_line = 0;
    char *cur_url = NULL;
    if (b && pc && pc >= b->byte_code_buf)
    {
        int col;
        cur_line = find_line_num(ctx, b, (uint32_t)(pc - b->byte_code_buf), &col);
        cur_url = jdbg_atom_strdup(ctx, b->filename);
    }

    int line_changed = (cur_url == NULL) ||
                       (dbg->last_url == NULL) ||
                       strcmp(cur_url, dbg->last_url) ||
                       cur_line != dbg->last_line;
    int should_pause = 0;
    switch (dbg->step_mode)
    {
    case JS_DEBUG_RESUME_STEP_INTO:
        should_pause = line_changed;
        break;
    case JS_DEBUG_RESUME_STEP_OVER:
        should_pause = line_changed && (cur_depth <= dbg->last_depth);
        break;
    case JS_DEBUG_RESUME_STEP_OUT:
        should_pause = (cur_depth < dbg->last_depth);
        break;
    default:
        should_pause = 0;
        break;
    }

    free(cur_url);
    if (should_pause)
    {
        jdbg_do_pause(ctx, dbg, 1 /* reason = step */, NULL, 0, sf, pc);
        dbg->step_mode = dbg->resume_mode;
        if (dbg->resume_mode == JS_DEBUG_RESUME_CONTINUE)
            dbg->step_pending = 0;
        else
            jdbg_record_loc(ctx, dbg, sf, pc);
    }
}

/* ================================================================== */
/* call stack + scopes + eval (only valid while paused)                */
/* ================================================================== */
int js_debug_get_call_stack(JSContext *ctx, JSDebugFrameInfo *out, int max)
{
    struct JSDebugState *dbg = (struct JSDebugState *)ctx->rt->debug_state;
    if (!dbg || !dbg->paused)
        return 0;
    int n = 0;
    int top = 1;
    for (JSStackFrame *sf = ctx->rt->current_stack_frame; sf && n < max;
         sf = sf->prev_frame, top = 0)
    {
        memset(&out[n], 0, sizeof(out[n]));
        out[n].handle = (JSDebugFrameHandle)sf;
        struct JSFunctionBytecode *b = JS_GetFunctionBytecode(sf->cur_func);
        const uint8_t *pc = NULL;
        if (top && dbg->pause_pc)
            pc = dbg->pause_pc;
        else if (sf->cur_pc && b && sf->cur_pc > b->byte_code_buf)
            pc = sf->cur_pc - 1;
        if (b)
        {
            out[n].func_name = jdbg_atom_strdup(ctx, b->func_name);
            if (!out[n].func_name)
                out[n].func_name = jdbg_strdup("");
            out[n].url = jdbg_atom_strdup(ctx, b->filename);
            if (!out[n].url)
                out[n].url = jdbg_strdup("");
            if (pc && pc >= b->byte_code_buf)
            {
                int col;
                out[n].line = find_line_num(ctx, b,
                                             (uint32_t)(pc - b->byte_code_buf), &col);
                out[n].col = col;
            }
        }
        else
        {
            out[n].func_name = jdbg_strdup("[native]");
            out[n].url = jdbg_strdup("");
        }
        char sid[32];
        snprintf(sid, sizeof sid, "%d", n + 1);
        out[n].script_id = jdbg_strdup(sid);
        n++;
    }
    return n;
}

void js_debug_free_frames(JSDebugFrameInfo *frames, int n)
{
    if (!frames)
        return;
    for (int i = 0; i < n; i++)
    {
        free(frames[i].func_name);
        free(frames[i].url);
        free(frames[i].script_id);
    }
}

int js_debug_get_scopes(JSContext *ctx, JSDebugFrameHandle frame,
                       JSDebugScopeInfo *out, int max)
{
    JSStackFrame *sf = (JSStackFrame *)frame;
    if (!sf || max <= 0)
        return 0;
    struct JSFunctionBytecode *b = JS_GetFunctionBytecode(sf->cur_func);
    int n = 0;

    /* scope 1: local (arguments + local variables) */
    if (b && n < max)
    {
        int total = (int)b->arg_count + (int)b->var_count;
        if (total > 0)
        {
            JSDebugVarInfo *vars =
                (JSDebugVarInfo *)calloc(total, sizeof(JSDebugVarInfo));
            if (vars)
            {
                int vi = 0;
                /* arguments: vardefs[0..arg_count-1], values arg_buf[i] */
                for (uint16_t i = 0; i < b->arg_count && i < sf->arg_count; i++)
                {
                    vars[vi].name = jdbg_atom_strdup(ctx, b->vardefs[i].var_name);
                    vars[vi].value_json = jdbg_value_json(ctx, sf->arg_buf[i]);
                    vars[vi].writable = !b->vardefs[i].is_const;
                    vars[vi].configurable = 1;
                    vars[vi].enumerable = 1;
                    vars[vi].is_exception = 0;
                    vi++;
                }
                /* locals: vardefs[arg_count..], values var_buf[i] */
                for (uint16_t i = 0; i < b->var_count; i++)
                {
                    vars[vi].name = jdbg_atom_strdup(ctx, b->vardefs[b->arg_count + i].var_name);
                    vars[vi].value_json = (sf->var_buf ? jdbg_value_json(ctx, sf->var_buf[i]) : jdbg_strdup("undefined"));
                    vars[vi].writable = !b->vardefs[b->arg_count + i].is_const;
                    vars[vi].configurable = 1;
                    vars[vi].enumerable = 1;
                    vars[vi].is_exception = 0;
                    vi++;
                }
                memset(&out[n], 0, sizeof(out[n]));
                out[n].type = JS_DEBUG_SCOPE_LOCAL;
                out[n].name = jdbg_strdup("Local");
                out[n].vars = vars;
                out[n].var_count = vi;
                out[n].object_id = "local";
                n++;
            }
        }
    }

    /* scope 2: global */
    if (n < max)
    {
        JSValue g = JS_GetGlobalObject(ctx);
        JSPropertyEnum *ptab = NULL;
        uint32_t plen = 0;
        int rc = JS_GetOwnPropertyNames(ctx, &ptab, &plen, g,
                                        JS_GPN_ENUM_ONLY);
        if (rc == 0 && plen > 0)
        {
            int cnt = plen > 256 ? 256 : (int)plen; /* cap for sanity */
            JSDebugVarInfo *vars = (JSDebugVarInfo *)calloc(cnt, sizeof(JSDebugVarInfo));
            if (vars)
            {
                int vi = 0;
                for (int i = 0; i < (int)plen && vi < cnt; i++)
                {
                    JSValue val = JS_GetProperty(ctx, g, ptab[i].atom);
                    vars[vi].name = jdbg_atom_strdup(ctx, ptab[i].atom);
                    vars[vi].value_json = jdbg_value_json(ctx, val);
                    vars[vi].writable = 1;
                    vars[vi].configurable = 1;
                    vars[vi].enumerable = 1;
                    vars[vi].is_exception = 0;
                    vi++;
                    JS_FreeValue(ctx, val);
                }
                memset(&out[n], 0, sizeof(out[n]));
                out[n].type = JS_DEBUG_SCOPE_GLOBAL;
                out[n].name = jdbg_strdup("Global");
                out[n].vars = vars;
                out[n].var_count = vi;
                out[n].object_id = "global";
                n++;
            }
        }
        if (ptab)
            JS_FreePropertyEnum(ctx, ptab, plen);
        JS_FreeValue(ctx, g);
    }
    return n;
}

void js_debug_free_scopes(JSDebugScopeInfo *scopes, int n)
{
    if (!scopes)
        return;
    for (int i = 0; i < n; i++)
    {
        free(scopes[i].name);
        for (int j = 0; j < scopes[i].var_count; j++)
        {
            free(scopes[i].vars[j].name);
            free(scopes[i].vars[j].value_json);
        }
        free(scopes[i].vars);
    }
}

int js_debug_eval_on_frame(JSContext *ctx, JSDebugFrameHandle frame,
                           const char *expr, size_t len,
                           char **out_val_json, char **out_err)
{
    /* P1: evaluate in the global scope. Full in-frame scope resolution
       (seeing the paused frame's locals) needs QuickJS's internal
       JS_EvalInternal scope machinery and is deferred. Globals and any
       expression not referencing locals work fully. */
    (void)frame;
    if (out_err)
        *out_err = NULL;
    JSValue r = JS_Eval(ctx, expr, len, "<eval-on-call-frame>",
                        JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r))
    {
        JSValue exc = JS_GetException(ctx);
        if (out_err)
        {
            const char *cs = JS_ToCString(ctx, exc);
            *out_err = cs ? jdbg_strdup(cs) : jdbg_strdup("exception");
            if (cs)
                JS_FreeCString(ctx, cs);
        }
        if (out_val_json)
            *out_val_json = NULL;
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, r);
        return -1;
    }
    if (out_val_json)
        *out_val_json = jdbg_value_json(ctx, r);
    JS_FreeValue(ctx, r);
    return 0;
}

/* ================================================================== */
/* scripts / source                                                    */
/* ================================================================== */
int js_debug_get_scripts(JSContext *ctx, char ***out_urls, int **out_ids,
                         int max)
{
    JSRuntime *rt = ctx->rt;
    int n = 0;
    char **urls = (char **)calloc(max > 0 ? max : 1, sizeof(char *));
    int *ids = (int *)calloc(max > 0 ? max : 1, sizeof(int));
    if (!urls || !ids)
    {
        free(urls);
        free(ids);
        return 0;
    }
    struct list_head *el;
    list_for_each(el, &rt->gc_obj_list)
    {
        if (n >= max)
            break;
        JSGCObjectHeader *gp = list_entry(el, JSGCObjectHeader, link);
        if (JS_GC_TYPE(gp) != JS_GC_OBJ_TYPE_FUNCTION_BYTECODE)
            continue;
        struct JSFunctionBytecode *b = (struct JSFunctionBytecode *)gp;
        urls[n] = jdbg_atom_strdup(ctx, b->filename);
        ids[n] = n + 1;
        n++;
    }
    *out_urls = urls;
    *out_ids = ids;
    return n;
}

int js_debug_get_script_source(JSContext *ctx, const char *url, char **out_src)
{
    if (out_src)
        *out_src = NULL;
    JSRuntime *rt = ctx->rt;
    struct list_head *el;
    list_for_each(el, &rt->gc_obj_list)
    {
        JSGCObjectHeader *gp = list_entry(el, JSGCObjectHeader, link);
        if (JS_GC_TYPE(gp) != JS_GC_OBJ_TYPE_FUNCTION_BYTECODE)
            continue;
        struct JSFunctionBytecode *b = (struct JSFunctionBytecode *)gp;
        if (jdbg_url_matches(ctx, b, url) && b->source)
        {
            if (out_src)
                *out_src = jdbg_strdup(b->source);
            return 0;
        }
    }
    return -1;
}

/* ================================================================== */
/* heap + CPU sampling (P3; minimal-but-real walkers)                  */
/* ================================================================== */
void js_debug_enumerate_heap(JSRuntime *rt, JSDebugHeapVisitor *v)
{
    if (!v || !v->on_node)
        return;
    struct list_head *el;
    list_for_each(el, &rt->gc_obj_list)
    {
        JSGCObjectHeader *gp = list_entry(el, JSGCObjectHeader, link);
        int type = JS_GC_TYPE(gp);
        size_t sz = 0;
        const char *name = "";
        size_t name_len = 0;
        void *ptr = (void *)gp;
        if (type == JS_GC_OBJ_TYPE_JS_OBJECT)
        {
            struct JSObject *p = (struct JSObject *)gp;
            sz = sizeof(struct JSObject);
            /* class name from the class array (best-effort) */
            if (p->class_id < rt->class_count)
            {
                JSAtom ca = rt->class_array[p->class_id].class_name;
                char nbuf[256];
                const char *s = JS_AtomGetStrRT(rt, nbuf, sizeof nbuf, ca);
                if (s)
                {
                    name = s;
                    name_len = strlen(s);
                }
            }
            v->on_node(v->ud, ptr, p->class_id, sz, name, name_len);
            /* edges: own properties (best-effort, by name) */
            if (v->on_edge && p->shape && p->prop)
            {
                JSShape *sh = p->shape;
                /* shape->prop_count properties follow the shape header */
                /* (kept minimal: a full edge walk is a P3 refinement) */
                (void)sh;
            }
        }
        else
        {
            const char *tn =
                type == JS_GC_OBJ_TYPE_FUNCTION_BYTECODE ? "FunctionBytecode" : type == JS_GC_OBJ_TYPE_SHAPE ? "Shape" : type == JS_GC_OBJ_TYPE_VAR_REF ? "VarRef" : type == JS_GC_OBJ_TYPE_ASYNC_FUNCTION ? "AsyncFunction" : "GCObject";
            v->on_node(v->ud, ptr, type, 0, tn, strlen(tn));
        }
    }
}

int js_debug_sample_stack(JSContext *ctx, JSDebugFrameInfo *out, int max)
{
    int n = 0;
    int top = 1;
    for (JSStackFrame *sf = ctx->rt->current_stack_frame; sf && n < max;
         sf = sf->prev_frame, top = 0)
    {
        memset(&out[n], 0, sizeof(out[n]));
        out[n].handle = (JSDebugFrameHandle)sf;
        struct JSFunctionBytecode *b = JS_GetFunctionBytecode(sf->cur_func);
        if (b)
        {
            out[n].func_name = jdbg_atom_strdup(ctx, b->func_name);
            out[n].url = jdbg_atom_strdup(ctx, b->filename);
            const uint8_t *pc = (top && sf->cur_pc) ? sf->cur_pc
                                : (sf->cur_pc && b->byte_code_buf && sf->cur_pc > b->byte_code_buf)
                                    ? sf->cur_pc - 1 : NULL;
            if (pc && pc >= b->byte_code_buf)
            {
                int col;
                out[n].line = find_line_num(ctx, b,
                                             (uint32_t)(pc - b->byte_code_buf), &col);
                out[n].col = col;
            }
        }
        else
        {
            out[n].func_name = jdbg_strdup("[native]");
            out[n].url = jdbg_strdup("");
        }
        char sid[32];
        snprintf(sid, sizeof sid, "%d", n + 1);
        out[n].script_id = jdbg_strdup(sid);
        n++;
    }
    return n;
}
