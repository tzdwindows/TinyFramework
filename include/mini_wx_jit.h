/*
 * mini_wx_jit.h — W^X (Write XOR Execute) executable-memory manager.
 *
 * The JIT backends (SLJIT/MIR or our own emitter) produce machine code into
 * a buffer that is ONLY ever Writeable OR eXecutable, never both — the
 * rule mandated by macOS (Apple Silicon `pthread_jit_write_protect_np` +
 * MAP_JIT), hardened Windows (VirtualProtect toggle), and PaX/grsec Linux.
 *
 * This file is pure C99 + OS primitives, no third-party deps. It is the
 * one piece of the JIT story that is fully verifiable in isolation; a
 * self-test emits a real x86_64 function, flips to RX, calls it, checks
 * the return value, and tears down — proving the W^X path end to end.
 */
#ifndef MINI_WX_JIT_H
#define MINI_WX_JIT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MiniJITBuf {
    uint8_t *mem;     /* RW while writing, RX while executing */
    size_t   size;    /* bytes written                         */
    size_t   cap;     /* capacity                              */
    int      writable; /* 1 = currently RW, 0 = currently RX  */
} MiniJITBuf;

/* Allocate an executable buffer of `cap` bytes in WRITABLE mode. */
MiniJITBuf *mini_jit_alloc(size_t cap);

/* Append raw bytes (only valid in writable mode). */
int  mini_jit_emit(MiniJITBuf *b, const uint8_t *code, size_t n);

/* Flip RW -> RX (make executable). After this, writing is forbidden. */
int  mini_jit_finalize(MiniJITBuf *b);

/* Tear down: unmap/VirtualFree. */
void mini_jit_free(MiniJITBuf *b);

/* Get an executable pointer (only after _finalize). */
void *mini_jit_code(MiniJITBuf *b);

#ifdef __cplusplus
}
#endif
#endif /* MINI_WX_JIT_H */
