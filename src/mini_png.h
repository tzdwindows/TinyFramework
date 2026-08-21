/*
 * mini_png.h - minimal PNG encoder (no zlib dependency).
 */
#ifndef MINI_PNG_H
#define MINI_PNG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encode raw RGBA pixels (top-to-bottom, 4 bytes/pixel) into a complete
   PNG (stored/uncompressed deflate). On success returns 0 and sets *out
   to a malloc'd buffer (caller frees) and *out_len to its byte size. */
int mini_png_encode_rgba(const uint8_t *rgba, int w, int h,
                         uint8_t **out, size_t *out_len);

/* Decode BMP, TGA, or raw RGBA image into malloc'd RGBA8 buffer. */
int mini_png_decode_rgba(const uint8_t *data, size_t size,
                         uint8_t **out_rgba, int *out_w, int *out_h);

#ifdef __cplusplus
}
#endif
#endif /* MINI_PNG_H */
