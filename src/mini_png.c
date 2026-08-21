/*
 * mini_png.c - minimal PNG encoder (no zlib dependency).
 *
 * Produces a valid PNG from raw RGBA pixels using zlib "stored" deflate
 * blocks (BTYPE=00, uncompressed), so Page.captureScreenshot works
 * without pulling in a real deflate implementation. Output is a complete
 * PNG: signature | IHDR | IDAT(zlib-stored over filtered scanlines) | IEND.
 *
 * Standalone: no GL, no engine headers. Used by the renderer's screenshot
 * path (Page domain in CDP).
 */
#include "mini_png.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- CRC32 (PNG/zlib polynomial 0xEDB88320) ---- */
static uint32_t png_crc_table[256];
static int png_crc_init = 0;
static void png_crc_build(void)
{
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        png_crc_table[i] = c;
    }
    png_crc_init = 1;
}
static uint32_t png_crc32(const uint8_t *p, size_t n)
{
    if (!png_crc_init)
        png_crc_build();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = png_crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---- Adler32 (zlib checksum over the uncompressed data) ---- */
static uint32_t png_adler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++)
    {
        a = (a + p[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

/* write a PNG chunk at cursor *po: len(4) + type(4) + data + crc(4) */
static void png_write_chunk(uint8_t *png, size_t *po, const char *type,
                            const uint8_t *data, size_t len)
{
    uint8_t *p = png + *po;
    put_be32(p, (uint32_t)len);
    p += 4;
    memcpy(p, type, 4);
    p += 4;
    if (len)
    {
        memcpy(p, data, len);
        p += len;
    }
    /* CRC covers type + data */
    put_be32(p, png_crc32(png + *po + 4, 4 + len));
    p += 4;
    *po += 4 + 4 + len + 4;
}

int mini_png_encode_rgba(const uint8_t *rgba, int w, int h,
                         uint8_t **out, size_t *out_len)
{
    if (!rgba || w <= 0 || h <= 0 || !out || !out_len)
        return -1;
    *out = NULL;
    *out_len = 0;

    size_t row = (size_t)w * 4;
    size_t raw_len = (row + 1) * (size_t)h; /* +1 filter byte per scanline */

    /* build the filtered scanlines: each row prefixed with a 0 (None) byte */
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw)
        return -1;
    for (int y = 0; y < h; y++)
    {
        uint8_t *r = raw + (size_t)y * (row + 1);
        r[0] = 0; /* filter: None */
        memcpy(r + 1, rgba + (size_t)y * row, row);
    }

    /* zlib stream with "stored" deflate blocks (<=65535 bytes each) */
    size_t blocks = (raw_len + 65534) / 65535;
    size_t zlib_cap = 2 + blocks * (5 + 65535) + 4; /* upper bound */
    uint8_t *zlib = (uint8_t *)malloc(zlib_cap);
    if (!zlib)
    {
        free(raw);
        return -1;
    }
    size_t zo = 0;
    zlib[zo++] = 0x78; /* CMF: deflate, window size 7 */
    zlib[zo++] = 0x01; /* FLG: (0x7801) % 31 == 0, no dict */
    size_t off = 0;
    while (off < raw_len)
    {
        size_t chunk = raw_len - off;
        int last;
        if (chunk > 65535)
        {
            chunk = 65535;
            last = 0;
        }
        else
            last = 1; /* final block */
        zlib[zo++] = (uint8_t)(last ? 0x01 : 0x00); /* BFINAL + BTYPE=00 */
        uint16_t len16 = (uint16_t)chunk;
        uint16_t nlen16 = (uint16_t)~len16;
        zlib[zo++] = (uint8_t)(len16 & 0xFF);
        zlib[zo++] = (uint8_t)((len16 >> 8) & 0xFF);
        zlib[zo++] = (uint8_t)(nlen16 & 0xFF);
        zlib[zo++] = (uint8_t)((nlen16 >> 8) & 0xFF);
        memcpy(zlib + zo, raw + off, chunk);
        zo += chunk;
        off += chunk;
    }
    uint32_t ad = png_adler32(raw, raw_len);
    zlib[zo++] = (uint8_t)(ad >> 24);
    zlib[zo++] = (uint8_t)(ad >> 16);
    zlib[zo++] = (uint8_t)(ad >> 8);
    zlib[zo++] = (uint8_t)(ad);
    free(raw);

    /* assemble the PNG */
    size_t total = 8 + (4 + 4 + 13 + 4) + (4 + 4 + zo + 4) + (4 + 4 + 0 + 4);
    uint8_t *png = (uint8_t *)malloc(total);
    if (!png)
    {
        free(zlib);
        return -1;
    }
    size_t po = 0;
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    memcpy(png + po, sig, 8);
    po += 8;

    uint8_t ihdr[13];
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16);
    ihdr[2] = (uint8_t)(w >> 8);  ihdr[3] = (uint8_t)(w);
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16);
    ihdr[6] = (uint8_t)(h >> 8);  ihdr[7] = (uint8_t)(h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 6;  /* color type 6 = truecolor + alpha */
    ihdr[10] = 0; /* compression: deflate */
    ihdr[11] = 0; /* filter: adaptive */
    ihdr[12] = 0; /* interlace: none */

    png_write_chunk(png, &po, "IHDR", ihdr, 13);
    png_write_chunk(png, &po, "IDAT", zlib, zo);
    png_write_chunk(png, &po, "IEND", NULL, 0);
    free(zlib);

    *out = png;
    *out_len = total;
    return 0;
}

int mini_png_decode_rgba(const uint8_t *data, size_t size,
                         uint8_t **out_rgba, int *out_w, int *out_h)
{
    if (!data || size < 18 || !out_rgba || !out_w || !out_h)
        return -1;
    *out_rgba = NULL;
    *out_w = 0;
    *out_h = 0;

    /* 1. BMP parser */
    if (data[0] == 'B' && data[1] == 'M' && size >= 54)
    {
        uint32_t data_offset = (uint32_t)data[10] | ((uint32_t)data[11] << 8) |
                               ((uint32_t)data[12] << 16) | ((uint32_t)data[13] << 24);
        int32_t w = (int32_t)((uint32_t)data[18] | ((uint32_t)data[19] << 8) |
                              ((uint32_t)data[20] << 16) | ((uint32_t)data[21] << 24));
        int32_t h = (int32_t)((uint32_t)data[22] | ((uint32_t)data[23] << 8) |
                              ((uint32_t)data[24] << 16) | ((uint32_t)data[25] << 24));
        uint16_t bpp = (uint16_t)data[28] | ((uint16_t)data[29] << 8);

        if (w <= 0 || h == 0 || (bpp != 24 && bpp != 32) || data_offset >= size)
            return -1;

        int flip_y = 1;
        if (h < 0) { h = -h; flip_y = 0; }

        uint8_t *rgba = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
        if (!rgba) return -1;

        size_t row_stride = (size_t)((w * (bpp / 8) + 3) & ~3);
        for (int y = 0; y < h; y++)
        {
            int src_y = flip_y ? (h - 1 - y) : y;
            size_t row_off = data_offset + (size_t)src_y * row_stride;
            if (row_off + (size_t)w * (bpp / 8) > size) break;

            const uint8_t *src_p = data + row_off;
            uint8_t *dst_p = rgba + (size_t)y * (size_t)w * 4;

            for (int x = 0; x < w; x++)
            {
                if (bpp == 32)
                {
                    dst_p[0] = src_p[2]; /* R */
                    dst_p[1] = src_p[1]; /* G */
                    dst_p[2] = src_p[0]; /* B */
                    dst_p[3] = src_p[3]; /* A */
                    src_p += 4;
                }
                else
                {
                    dst_p[0] = src_p[2];
                    dst_p[1] = src_p[1];
                    dst_p[2] = src_p[0];
                    dst_p[3] = 255;
                    src_p += 3;
                }
                dst_p += 4;
            }
        }
        *out_rgba = rgba;
        *out_w = w;
        *out_h = h;
        return 0;
    }

    /* 2. Uncompressed TGA parser */
    if (size >= 18 && data[2] == 2 /* uncompressed true-color */)
    {
        int w = (int)data[12] | ((int)data[13] << 8);
        int h = (int)data[14] | ((int)data[15] << 8);
        int bpp = (int)data[16];
        int id_len = (int)data[0];

        if (w > 0 && h > 0 && (bpp == 24 || bpp == 32) && 18 + id_len < (int)size)
        {
            uint8_t *rgba = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
            if (!rgba) return -1;

            const uint8_t *src = data + 18 + id_len;
            for (int y = 0; y < h; y++)
            {
                int dst_y = (data[17] & 0x20) ? y : (h - 1 - y);
                uint8_t *dst_p = rgba + (size_t)dst_y * (size_t)w * 4;
                for (int x = 0; x < w; x++)
                {
                    if (bpp == 32)
                    {
                        dst_p[0] = src[2];
                        dst_p[1] = src[1];
                        dst_p[2] = src[0];
                        dst_p[3] = src[3];
                        src += 4;
                    }
                    else
                    {
                        dst_p[0] = src[2];
                        dst_p[1] = src[1];
                        dst_p[2] = src[0];
                        dst_p[3] = 255;
                        src += 3;
                    }
                    dst_p += 4;
                }
            }
            *out_rgba = rgba;
            *out_w = w;
            *out_h = h;
            return 0;
        }
    }

    return -1;
}
