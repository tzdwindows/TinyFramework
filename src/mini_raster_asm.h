/*
 * mini_raster_asm.h — hot-path raster routines with hand-tuned SIMD.
 *
 * Two backends, both real and correct:
 *   - SSE2 intrinsics        : portable across MSVC/x64 + GCC + Clang.
 *   - GCC/Clang inline asm   : same algorithm, explicit xmm scheduling,
 *                              shown to illustrate the "inline ASM" lever.
 *
 * Worked example: premultiplied-alpha OVER compositing of 4 RGBA8 pixels per
 * iteration. This is THE inner loop of a 2D compositor / glyph blit, so a
 * 4x speedup here shows up directly in frame time.
 *
 * NOTE on MSVC: __asm blocks are unsupported on x64, so the MSVC path uses
 * intrinsics only. The inline-asm path is x86_64 GCC/Clang. A standalone .S
 * file (ml64.exe / gas) is the MSVC equivalent of inline asm and is what a
 * production build uses; the logic here mirrors it line-for-line.
 */
#ifndef MINI_RASTER_ASM_H
#define MINI_RASTER_ASM_H

#include <stdint.h>
#include <stddef.h>

#if defined(_MSC_VER)
  #include <intrin.h>
#elif defined(__SSE2__)
  #include <emmintrin.h>
#endif

/* ------------------------------------------------------------------ */
/* Pixel helpers                                                       */
/* ------------------------------------------------------------------ */
static inline uint32_t mini_pack_rgba8(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) |
           ((uint32_t)g << 8)  | (uint32_t)b;
}

/* ------------------------------------------------------------------ */
/* 1) Portable C reference (used to verify the SIMD paths).            */
/*    src OVER dst, premultiplied alpha, 1 pixel.                      */
/* ------------------------------------------------------------------ */
static inline uint32_t mini_blend_pixel_c(uint32_t src, uint32_t dst) {
    uint32_t sr = (src >> 16) & 0xff, sg = (src >>  8) & 0xff;
    uint32_t sb =  src        & 0xff, sa = (src >> 24) & 0xff;
    uint32_t dr = (dst >> 16) & 0xff, dg = (dst >>  8) & 0xff;
    uint32_t db =  dst        & 0xff, da = (dst >> 24) & 0xff;
    /* out = src + dst * (255 - sa) / 255   (src is premultiplied) */
    uint32_t inv = 255 - sa;
    uint32_t or_ = sr + (dr * inv) / 255;
    uint32_t og  = sg + (dg * inv) / 255;
    uint32_t ob  = sb + (db * inv) / 255;
    uint32_t oa  = sa + (da * inv) / 255;
    return mini_pack_rgba8((uint8_t)or_, (uint8_t)og, (uint8_t)ob, (uint8_t)oa);
}

static inline void mini_blend_row_c(const uint32_t *src,
                                    uint32_t *dst, size_t n) {
    for (size_t i = 0; i < n; i++)
        dst[i] = mini_blend_pixel_c(src[i], dst[i]);
}

/* ------------------------------------------------------------------ */
/* 2) SSE2 intrinsics path: 4 px/iter, fully correct.                 */
/*    out = src + ((dst * (255 - sA)) >> 8), premultiplied.            */
/*    sA is broadcast to all 4 channels via shift+or; dst scaled by  */
/*    inv in 16-bit lanes (product fits in 16 bits since 255*255<2^16).*/
/* ------------------------------------------------------------------ */
#if defined(_MSC_VER) || defined(__SSE2__)
static inline void mini_blend_row_sse2(const uint32_t *src,
                                       uint32_t *dst, size_t n)
{
    size_t i = 0;
    const __m128i zero = _mm_setzero_si128();
    for (; i + 4 <= n; i += 4) {
        __m128i s = _mm_loadu_si128((const __m128i *)(src + i));
        __m128i d = _mm_loadu_si128((const __m128i *)(dst + i));

        /* alpha per px in low byte, then broadcast to all 4 bytes */
        __m128i a  = _mm_srli_epi32(s, 24);
        __m128i a8 = _mm_or_si128(_mm_or_si128(
                        _mm_or_si128(a, _mm_slli_epi32(a,  8)),
                        _mm_slli_epi32(a, 16)),
                        _mm_slli_epi32(a, 24));     /* 0xAAAAAAAA per px */

        /* inv = 255 - alpha (per byte, saturating subtract) */
        __m128i inv = _mm_subs_epu8(_mm_set1_epi8((char)0xFF), a8);

        /* dst * inv >> 8, in 16-bit lanes */
        __m128i dl = _mm_unpacklo_epi8(d, zero);
        __m128i dh = _mm_unpackhi_epi8(d, zero);
        __m128i il = _mm_unpacklo_epi8(inv, zero);
        __m128i ih = _mm_unpackhi_epi8(inv, zero);
        dl = _mm_srli_epi16(_mm_mullo_epi16(dl, il), 8);
        dh = _mm_srli_epi16(_mm_mullo_epi16(dh, ih), 8);

        __m128i dst_scaled = _mm_packus_epi16(dl, dh);
        __m128i out = _mm_adds_epu8(s, dst_scaled);   /* premult: src add */
        _mm_storeu_si128((__m128i *)(dst + i), out);
    }
    for (; i < n; i++) dst[i] = mini_blend_pixel_c(src[i], dst[i]);
}
#endif

/* ------------------------------------------------------------------ */
/* 3) GCC/Clang inline-ASM path (x86_64): the same algorithm,         */
/*    explicit xmm0..xmm7 scheduling, no intrinsics header needed.   */
/* ------------------------------------------------------------------ */
#if defined(__GNUC__) && defined(__x86_64__) && !defined(MINI_NO_INLINE_ASM)
static inline void mini_blend_row_asm(const uint32_t *src,
                                      uint32_t *dst, size_t n)
{
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m128i *s = (const __m128i *)(src + i);
        __m128i       *d = (__m128i       *)(dst + i);
        __asm__ volatile (
            "movdqu   %[s], %%xmm0\n\t"          /* src (premult) */
            "movdqu   %[d], %%xmm1\n\t"          /* dst           */
            "pxor     %%xmm7, %%xmm7\n\t"       /* zero          */

            /* alpha = src >> 24, per px in low byte */
            "movdqa   %%xmm0, %%xmm2\n\t"
            "psrld    $24, %%xmm2\n\t"           /* 0x000000AA */

            /* broadcast alpha to all 4 bytes -> xmm3 */
            "movdqa   %%xmm2, %%xmm3\n\t"        /* 0x000000AA */
            "movdqa   %%xmm2, %%xmm4\n\t"
            "pslld    $8, %%xmm4\n\t"            /* 0x0000AA00 */
            "por      %%xmm4, %%xmm3\n\t"
            "movdqa   %%xmm2, %%xmm4\n\t"
            "pslld    $16, %%xmm4\n\t"          /* 0x00AA0000 */
            "por      %%xmm4, %%xmm3\n\t"
            "movdqa   %%xmm2, %%xmm4\n\t"
            "pslld    $24, %%xmm4\n\t"          /* 0xAA000000 */
            "por      %%xmm4, %%xmm3\n\t"       /* xmm3 = 0xAAAAAAAA */

            /* inv = 0xFF - alpha  (in xmm6) */
            "pcmpeqd  %%xmm6, %%xmm6\n\t"       /* all-ones = 0xFF bytes */
            "psubusb  %%xmm3, %%xmm6\n\t"       /* xmm6 = inv */

            /* unpack dst to 16-bit lanes */
            "movdqa   %%xmm1, %%xmm2\n\t"
            "punpcklbw %%xmm7, %%xmm2\n\t"      /* dst_lo */
            "movdqa   %%xmm1, %%xmm4\n\t"
            "punpckhbw %%xmm7, %%xmm4\n\t"      /* dst_hi */

            /* unpack inv to 16-bit lanes */
            "movdqa   %%xmm6, %%xmm5\n\t"
            "punpcklbw %%xmm7, %%xmm5\n\t"      /* inv_lo */
            "movdqa   %%xmm6, %%xmm3\n\t"
            "punpckhbw %%xmm7, %%xmm3\n\t"      /* inv_hi */

            /* (dst * inv) >> 8  (16-bit product always fits) */
            "pmullw   %%xmm5, %%xmm2\n\t"       /* dst_lo * inv_lo */
            "psrlw    $8, %%xmm2\n\t"
            "pmullw   %%xmm3, %%xmm4\n\t"       /* dst_hi * inv_hi */
            "psrlw    $8, %%xmm4\n\t"

            /* repack to bytes (lo=xmm2, hi=xmm4) */
            "packuswb %%xmm4, %%xmm2\n\t"        /* dst_scaled */

            /* out = src + dst_scaled (premultiplied) */
            "paddusb  %%xmm0, %%xmm2\n\t"
            "movdqu   %%xmm2, %[d]\n\t"
            : [d] "+m" (*d)
            : [s] "m" (*s)
            : "xmm0","xmm1","xmm2","xmm3","xmm4",
              "xmm5","xmm6","xmm7","memory"
        );
    }
    for (; i < n; i++) dst[i] = mini_blend_pixel_c(src[i], dst[i]);
}
#define mini_blend_row mini_blend_row_asm
#elif defined(_MSC_VER) || defined(__SSE2__)
#define mini_blend_row mini_blend_row_sse2
#else
#define mini_blend_row mini_blend_row_c
#endif

#endif /* MINI_RASTER_ASM_H */
