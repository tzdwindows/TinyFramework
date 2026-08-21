/*
 * quickjs_jit_bench.c - correctness + performance harness for the JIT.
 *
 * Links against the JIT-enabled libqjs (built with -DJS_JIT; add
 * -DJS_JIT_CODEGEN for real SLJIT codegen). Runs a tight integer
 * workload twice in one process:
 *   (1) JS_EnableJIT(rt, 0)  -> pure interpreter baseline
 *   (2) JS_EnableJIT(rt, 1)  -> JIT (phase 1 = stub/interpreter-speed;
 *                                   phase 2 = real native codegen)
 * Asserts identical results and prints timings + speedup.
 *
 * Build (via the quickjs CMake, when JS_JIT is on):
 *   cmake -DJS_JIT=ON [-DJS_JIT_CODEGEN=ON] libs/quickjs
 *   cmake --build . --target jit_st
 * Run:
 *   ./jit_st                 # default workload size
 *   ./jit_st 20000000        # override size
 *   JS_JIT_TRACE=1 ./jit_st  # show compile/bail/free events
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "quickjs.h"

#ifndef JIT_BENCH_N
#define JIT_BENCH_N 5000000
#endif

/* Iterative fibonacci: a pure-int loop, ideal for the int fast path.
   The actual source is built at runtime from the workload size. */
#define JIT_STRINGIFY2(x) #x
#define STRINGIFY(x) JIT_STRINGIFY2(x)

static int run_workload(JSContext *ctx, const char *src, int *pres)
{
    JSValue r = JS_Eval(ctx, src, strlen(src), "<bench>", JS_EVAL_TYPE_GLOBAL);
    int rc = -1;
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, e);
        fprintf(stderr, "[bench] eval exception: %s\n", msg ? msg : "(null)");
        JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, e);
    } else {
        int32_t v;
        if (JS_ToInt32(ctx, &v, r) == 0) {
            *pres = (int)v;
            rc = 0;
        } else {
            fprintf(stderr, "[bench] result not int32\n");
        }
    }
    JS_FreeValue(ctx, r);
    return rc;
}

int main(int argc, char **argv)
{
    long n = JIT_BENCH_N;
    if (argc > 1)
        n = strtol(argv[1], NULL, 10);
    char srcbuf[256];
    snprintf(srcbuf, sizeof srcbuf,
             "function count(n){var i=0;while(i<n)i=i+1;return i;}\ncount(%ld);\n",
             n);

    JSRuntime *rt = JS_NewRuntime();
    if (!rt) { fprintf(stderr, "no runtime\n"); return 2; }
    JS_SetMemoryLimit(rt, 0);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { fprintf(stderr, "no context\n"); return 2; }

    int r_int = -1, r_jit = -1;
    clock_t t0, t1, t2, t3;
    double ti, tj;

    /* (1) interpreter baseline */
    JS_EnableJIT(rt, 0);
    t0 = clock();
    if (run_workload(ctx, srcbuf, &r_int) != 0) goto done;
    t1 = clock();
    ti = (double)(t1 - t0) / CLOCKS_PER_SEC;

    /* (2) JIT */
    JS_EnableJIT(rt, 1);
    t2 = clock();
    if (run_workload(ctx, srcbuf, &r_jit) != 0) goto done;
    t3 = clock();
    tj = (double)(t3 - t2) / CLOCKS_PER_SEC;

    printf("workload : fib(%ld)  (iterative, int loop)\n", n);
    printf("interp   : %d  in %6.3fs\n", r_int, ti);
    printf("jit      : %d  in %6.3fs\n", r_jit, tj);
    if (r_int != r_jit) {
        printf("RESULT   : MISMATCH (interp=%d jit=%d)  -- FAIL\n", r_int, r_jit);
        goto done;
    }
    printf("speedup  : %.2fx  %s\n",
           tj > 0.0 ? ti / tj : 0.0,
           tj < ti ? "(faster)" : "(no gain yet)");
    printf("RESULT   : PASS (results match)\n");

done:
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return (r_int == r_jit) ? 0 : 1;
}
