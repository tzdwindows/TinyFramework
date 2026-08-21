/*
 * mini_wx_jit.c — W^X executable-memory manager + a tiny x86_64 emitter demo.
 *
 * Why this exists: a JIT that maps memory RWX simultaneously is rejected by
 * every hardened OS. The portable contract is a toggle: write machine code
 * while the page is RW, then flip to RX before calling it. This file
 * implements that toggle portably and proves it by emitting and EXECUTING
 * a real native function (see JIT_SELFTEST).
 *
 *   - Apple Silicon: mmap(MAP_JIT) + pthread_jit_write_protect_np(0/1)
 *   - Windows:       VirtualAlloc(PAGE_READWRITE) -> VirtualProtect(PAGE_EXECUTE_READ)
 *   - Linux/BSD:     mmap(PROT_READ|PROT_WRITE) -> mprotect(PROT_READ|PROT_EXEC)
 */
#include "mini_wx_jit.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #include <windows.h>
#elif defined(__APPLE__)
  #include <mach/mach.h>
  #include <pthread.h>
  #include <sys/mman.h>
  #include <libkern/OSCacheControl.h>
  #ifndef MAP_JIT
    #define MAP_JIT 0x800
  #endif
  extern void pthread_jit_write_protect_np(int enabled);
#else
  #include <sys/mman.h>
#endif

/* page rounding */
static size_t page_size(void) {
#if defined(_WIN32)
    SYSTEM_INFO si; GetSystemInfo(&si); return si.dwPageSize;
#else
    return 4096;
#endif
}

MiniJITBuf *mini_jit_alloc(size_t cap) {
    size_t ps = page_size();
    size_t sz = ((cap + ps - 1) / ps) * ps;
    if (sz == 0) sz = ps;

    MiniJITBuf *b = (MiniJITBuf *)calloc(1, sizeof *b);
    if (!b) return NULL;

#if defined(_WIN32)
    b->mem = (uint8_t *)VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_READWRITE);
#elif defined(__APPLE__)
    b->mem = (uint8_t *)mmap(NULL, sz, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
#else
    b->mem = (uint8_t *)mmap(NULL, sz, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
    if (!b->mem) { free(b); return NULL; }
    b->cap = sz; b->size = 0; b->writable = 1;
    return b;
}

int mini_jit_emit(MiniJITBuf *b, const uint8_t *code, size_t n) {
    if (!b || !b->writable || b->size + n > b->cap) return -1;
#if defined(__APPLE__)
    pthread_jit_write_protect_np(0);   /* W mode */
#endif
    memcpy(b->mem + b->size, code, n);
    b->size += n;
    return 0;
}

int mini_jit_finalize(MiniJITBuf *b) {
    if (!b || !b->writable) return -1;
#if defined(_WIN32)
    DWORD old;
    if (!VirtualProtect(b->mem, b->cap, PAGE_EXECUTE_READ, &old)) return -1;
#elif defined(__APPLE__)
    pthread_jit_write_protect_np(1);                       /* X mode */
    sys_icache_invalidate(b->mem, b->size);               /* flush I$ */
#else
    if (mprotect(b->mem, b->cap, PROT_READ | PROT_EXEC) != 0) return -1;
    __builtin___clear_cache((char *)b->mem, (char *)b->mem + b->size);
#endif
    b->writable = 0;
    return 0;
}

void *mini_jit_code(MiniJITBuf *b) { return b && !b->writable ? (void *)b->mem : NULL; }

void mini_jit_free(MiniJITBuf *b) {
    if (!b) return;
#if defined(_WIN32)
    VirtualFree(b->mem, 0, MEM_RELEASE);
#else
    munmap(b->mem, b->cap);
#endif
    free(b);
}

/* ================================================================== */
/* JIT_SELFTEST: emit `int add(int,int)` in x86_64, W^X-execute it.    */
/*   Win64 calling convention: args in ECX,EDX; return in EAX.         */
/*   System V:          args in EDI,ESI; return in EAX.                */
/* ================================================================== */
#ifdef JIT_SELFTEST
#include <stdio.h>

#if defined(_WIN32)
static const uint8_t K_ADD[] = {
    0x89,0xC8,   /* mov eax, ecx   (arg0 -> eax)  */
    0x01,0xD0,   /* add eax, edx  (arg1 -> eax)  */
    0xC3         /* ret                            */
};
#else
static const uint8_t K_ADD[] = {
    0x89,0xF8,   /* mov eax, edi  (arg0 -> eax)  */
    0x01,0xF0,   /* add eax, esi  (arg1 -> eax)  */
    0xC3         /* ret                           */
};
#endif

typedef int (*add_fn)(int, int);

int main(void) {
    int fails = 0;
    MiniJITBuf *b = mini_jit_alloc(sizeof K_ADD);
    if (!b) { printf("[FAIL] jit alloc\n"); return 1; }
    if (mini_jit_emit(b, K_ADD, sizeof K_ADD) != 0) { printf("[FAIL] emit\n"); fails++; }
    if (mini_jit_finalize(b) != 0) { printf("[FAIL] finalize (W^X flip)\n"); fails++; }

    add_fn fn = (add_fn)mini_jit_code(b);
    if (!fn) { printf("[FAIL] code ptr\n"); fails++; }
    else {
        int r = fn(7, 35);                 /* must be 42 */
        printf("jit add(7,35) = %d\n", r);
        if (r == 42) printf("[PASS] W^X native execution\n");
        else { printf("[FAIL] result %d != 42\n", r); fails++; }

        int r2 = fn(100, 23);              /* 123 */
        if (r2 == 123) printf("[PASS] second call\n");
        else { printf("[FAIL] r2 %d\n", r2); fails++; }
    }
    mini_jit_free(b);
    printf(fails ? "JIT_SELFTEST: %d FAIL\n" : "JIT_SELFTEST: all PASS\n", fails);
    return fails ? 1 : 0;
}
#endif
