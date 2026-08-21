/*
 * mini_vfs.h — encrypted Virtual File System + QuickJS bytecode eval.
 *
 * Bundle layout (self-contained, no files on disk):
 *   [ 12-byte nonce ][ N-byte ciphertext ][ 16-byte Poly1305 tag ]
 * Cipher: ChaCha20-Poly1305 (RFC 8439), implemented in pure C99 in
 * mini_vfs_decrypt.c — no OpenSSL/mbedTLS dependency. Decryption happens
 * into a malloc'd RAM buffer; nothing is ever written to disk.
 */
#ifndef MINI_VFS_H
#define MINI_VFS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct MiniBridge;  /* forward; real type in mini_js_bridge.h */

/* Decrypt a bundle into a freshly malloc'd buffer (caller frees *out).
   Returns 0 on success (tag verified in constant time), non-zero on failure. */
int mini_vfs_decrypt(const uint8_t *bundle, size_t bundle_size,
                     const uint8_t key[32],
                     const uint8_t *aad, size_t aad_len,
                     uint8_t **out, size_t *out_len);

/* Encrypt `in` into a malloc'd bundle [nonce|ct|tag]. nonce MUST be 12 bytes. */
int mini_vfs_encrypt(const uint8_t *in, size_t in_len,
                     const uint8_t key[32],
                     const uint8_t nonce[12],
                     const uint8_t *aad, size_t aad_len,
                     uint8_t **out, size_t *out_len);

/* Evaluate QuickJS bytecode (.qjc, produced by `qjsc`) inside the bridge.
   Returns 0 on success. Implementation lives in mini_js_bridge.c so it can
   reach the QuickJS C API without exporting engine internals here. */
int mini_vfs_eval_bytecode(struct MiniBridge *b, const uint8_t *bc, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* MINI_VFS_H */
