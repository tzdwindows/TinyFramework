/*
 * mini_h2.c — HTTP/2 frame codec + HPACK static table (pragmatic) + self-test.
 */
#include "mini_h2.h"
#include "mini_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void mini_h2_encode_header(uint8_t out[9], uint32_t length, uint8_t type,
                           uint8_t flags, uint32_t stream_id)
{
    out[0] = (uint8_t)((length >> 16) & 0xFF);
    out[1] = (uint8_t)((length >> 8) & 0xFF);
    out[2] = (uint8_t)(length & 0xFF);
    out[3] = type;
    out[4] = flags;
    out[5] = (uint8_t)((stream_id >> 24) & 0x7F); /* top bit reserved */
    out[6] = (uint8_t)((stream_id >> 16) & 0xFF);
    out[7] = (uint8_t)((stream_id >> 8) & 0xFF);
    out[8] = (uint8_t)(stream_id & 0xFF);
}

int mini_h2_decode_header(const uint8_t *buf, size_t len, H2FrameHeader *h)
{
    if (len < 9 || !buf || !h)
        return -1;
    h->length = ((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | buf[2];
    h->type = buf[3];
    h->flags = buf[4];
    h->stream_id = ((uint32_t)(buf[5] & 0x7F) << 24) | ((uint32_t)buf[6] << 16) |
                   ((uint32_t)buf[7] << 8) | buf[8];
    return 0;
}

/* ---- HPACK integer codec ---- */
int mini_hpack_enc_int(uint8_t *out, size_t cap, uint64_t value, uint8_t prefix_bits)
{
    if (!out || cap == 0) return -1;
    uint8_t max = (uint8_t)((1u << prefix_bits) - 1);
    size_t o = 0;
    if (value < max)
    {
        out[0] = (uint8_t)((out[0] & ~max) | (uint8_t)value); /* preserve prefix bits in [0] */
        return 1;
    }
    out[0] = (uint8_t)((out[0] & ~max) | max);
    o = 1;
    value -= max;
    while (value >= 128)
    {
        if (o >= cap) return -1;
        out[o++] = (uint8_t)((value & 0x7F) | 0x80);
        value >>= 7;
    }
    if (o >= cap) return -1;
    out[o++] = (uint8_t)value;
    return (int)o;
}

int mini_hpack_dec_int(const uint8_t *buf, size_t len, uint8_t prefix_bits, uint64_t *out)
{
    if (!buf || len == 0 || !out) return -1;
    uint8_t mask = (uint8_t)((1u << prefix_bits) - 1);
    uint64_t v = buf[0] & mask;
    size_t i = 1;
    if (v < mask) { *out = v; return 1; }
    uint64_t m = 0;
    while (i < len)
    {
        uint8_t b = buf[i];
        v += (uint64_t)(b & 0x7F) << m;
        m += 7;
        i++;
        if ((b & 0x80) == 0) { *out = v; return (int)i; }
        if (m > 63) return -1; /* overflow */
    }
    return -1; /* truncated */
}

/* ---- HPACK static table (RFC 7541 §A) — indices 1..61 ---- */
static const char *const STATIC_NAMES[62] = {
    NULL, /* index 0 unused */
    /* 1..7 */  ":authority", ":method", ":method", ":path", ":path", ":scheme", ":scheme",
    /* 8..14 */ ":status", ":status", ":status", ":status", ":status", ":status", ":status",
    /* 15..21 */ "accept-charset", "accept-encoding", "accept-language", "accept-ranges", "accept", "access-control-allow-origin", "age",
    /* 22..28 */ "allow", "authorization", "cache-control", "content-disposition", "content-encoding", "content-language", "content-length",
    /* 29..35 */ "content-location", "content-range", "content-type", "cookie", "date", "etag", "expect",
    /* 36..42 */ "expires", "from", "host", "if-match", "if-modified-since", "if-none-match", "if-range",
    /* 43..49 */ "if-unmodified-since", "last-modified", "link", "location", "max-forwards", "proxy-authenticate", "proxy-authorization",
    /* 50..56 */ "range", "referer", "refresh", "retry-after", "server", "set-cookie", "strict-transport-security",
    /* 57..61 */ "transfer-encoding", "user-agent", "vary", "via", "www-authenticate"
};
static const char *const STATIC_VALUES[62] = {
    NULL,
    /* 1..7 */  "", "GET", "POST", "/", "/index.html", "http", "https",
    /* 8..14 */ "200", "204", "206", "304", "400", "404", "500",
    /* 15..21 */ "", "gzip, deflate", "", "", "", "", "",
    /* 22..61 */ "", "", "", "", "", "", "",  /* 22..28 */
    "", "", "", "", "", "", "",                 /* 29..35 */
    "", "", "", "", "", "", "",                 /* 36..42 */
    "", "", "", "", "", "", "",                 /* 43..49 */
    "", "", "", "", "", "", "",                 /* 50..56 */
    "", "", "", "", ""                          /* 57..61 */
};

const char *mini_hpack_static_name(int idx)
{
    return (idx >= 1 && idx <= 61) ? STATIC_NAMES[idx] : NULL;
}
const char *mini_hpack_static_value(int idx)
{
    return (idx >= 1 && idx <= 61) ? STATIC_VALUES[idx] : NULL;
}
int mini_hpack_static_find_name(const char *name)
{
    if (!name) return -1;
    for (int i = 1; i <= 61; i++)
        if (!strcmp(STATIC_NAMES[i], name))
            return i;
    return -1;
}

/* decode one HPACK string (length-prefixed, no Huffman here) into a malloc'd
 * null-terminated string. *consumed set to bytes used. 0 ok, -1 error. */
static int hpack_dec_string(const uint8_t *buf, size_t len, char **out, size_t *consumed)
{
    if (len == 0) return -1;
    /* the Huffman flag is the top bit of the length byte; we don't decode
       Huffman (kept pragmatic), so reject Huffman-encoded strings honestly. */
    uint8_t huff = buf[0] & 0x80;
    if (huff) return -1; /* Huffman not supported — signal rather than mis-decode */
    uint64_t slen = 0;
    int n = mini_hpack_dec_int(buf, len, 7, &slen);
    if (n < 0) return -1;
    if ((size_t)n + slen > len) return -1; /* truncated */
    char *s = (char *)malloc((size_t)slen + 1);
    if (!s) return -1;
    memcpy(s, buf + n, (size_t)slen);
    s[slen] = 0;
    *out = s; *consumed = (size_t)n + (size_t)slen;
    return 0;
}

int mini_hpack_decode_block(const uint8_t *buf, size_t len, H2HeaderPair *out, int max_pairs)
{
    int n = 0;
    size_t i = 0;
    while (i < len)
    {
        uint8_t b = buf[i];
        if ((b & 0x80) != 0)
        {
            /* indexed header field (static only here) */
            uint64_t idx = 0;
            int c = mini_hpack_dec_int(buf + i, len - i, 7, &idx);
            if (c < 0) return -1;
            const char *name = mini_hpack_static_name((int)idx);
            const char *val = mini_hpack_static_value((int)idx);
            if (!name) { i += (size_t)c; continue; } /* unknown index -> skip */
            if (n >= max_pairs) return -1;
            out[n].name = strdup(name);
            out[n].value = strdup(val ? val : "");
            n++;
            i += (size_t)c;
        }
        else
        {
            /* literal header (with or without indexing) — static table lookup
               for the name, then a literal value. */
            int name_idx_type = (b >> 6) & 1; /* 0=incremental, 1=without indexing (6) */
            (void)name_idx_type;
            uint64_t idx = 0;
            int c = mini_hpack_dec_int(buf + i, len - i, 6, &idx);
            if (c < 0) return -1;
            i += (size_t)c;
            char *name = NULL; char *val = NULL; size_t cc = 0;
            if (idx > 0)
            {
                const char *sn = mini_hpack_static_name((int)idx);
                if (!sn) return -1;
                name = strdup(sn);
            }
            else
            {
                if (hpack_dec_string(buf + i, len - i, &name, &cc) != 0) return -1;
                i += cc;
            }
            if (hpack_dec_string(buf + i, len - i, &val, &cc) != 0) { free(name); return -1; }
            i += cc;
            if (n >= max_pairs) { free(name); free(val); return -1; }
            out[n].name = name; out[n].value = val; n++;
        }
    }
    return n;
}

/* ================================================================== */
/* H2_SELFTEST                                                          */
/* ================================================================== */
#ifdef H2_SELFTEST
static int h2f = 0;
#define H2CK(c, m) do { if (!(c)) { fprintf(stderr, "H2 FAIL: %s\n", m); h2f++; } } while (0)

int main(void)
{
    mini_log_init();

    /* frame header round-trip */
    uint8_t fh[9];
    mini_h2_encode_header(fh, 0x1234, H2_SETTINGS, 0, 1);
    H2FrameHeader h;
    H2CK(mini_h2_decode_header(fh, 9, &h) == 0, "decode header");
    H2CK(h.length == 0x1234 && h.type == H2_SETTINGS && h.stream_id == 1, "header fields");

    /* HPACK integer: value 10 with prefix 5 -> single byte (10 fits) */
    uint8_t ib[2] = {0}; int n = mini_hpack_enc_int(ib, 2, 10, 5);
    H2CK(n == 1, "enc int small");
    uint64_t v; int dn = mini_hpack_dec_int(ib, 2, 5, &v);
    H2CK(dn == 1 && v == 10, "dec int small");
    /* value 1337 with prefix 5 -> multi-byte (RFC 7541 example) */
    uint8_t ib2[8] = {0}; int n2 = mini_hpack_enc_int(ib2, 8, 1337, 5);
    H2CK(n2 > 1, "enc int multi");
    /* ib2[0] should have the 5-bit max (31) in the low bits + prefix preserved */
    H2CK((ib2[0] & 0x1F) == 0x1F, "enc int multi first byte");
    uint64_t v2; mini_hpack_dec_int(ib2, (size_t)n2, 5, &v2);
    H2CK(v2 == 1337, "dec int multi");

    /* static table */
    H2CK(!strcmp(mini_hpack_static_name(2), ":method"), "static :method");
    H2CK(!strcmp(mini_hpack_static_value(2), "GET"), "static GET");
    H2CK(mini_hpack_static_find_name("content-type") == 31, "find content-type");

    /* decode a header block: indexed(2)=:method:GET + literal name "x-foo" "bar"
       0x82 = indexed field #2; 0x00 = literal w/o indexing, name-idx 0;
       then len-prefixed strings (no Huffman). */
    uint8_t blk[16];
    blk[0] = 0x82;
    blk[1] = 0x00;
    blk[2] = 5; memcpy(blk + 3, "x-foo", 5);  /* name: len 5, "x-foo" */
    blk[8] = 3; memcpy(blk + 9, "bar", 3);     /* value: len 3, "bar" */
    size_t blen = 12;
    H2HeaderPair pairs[4];
    int pn = mini_hpack_decode_block(blk, blen, pairs, 4);
    H2CK(pn == 2, "decode block 2 pairs");
    H2CK(pn >= 1 && pairs[0].name && !strcmp(pairs[0].name, ":method") && !strcmp(pairs[0].value, "GET"), "pair0 static");
    H2CK(pn >= 2 && pairs[1].name && !strcmp(pairs[1].name, "x-foo") && !strcmp(pairs[1].value, "bar"), "pair1 literal");
    for (int i = 0; i < pn; i++) { free(pairs[i].name); free(pairs[i].value); }

    fprintf(stderr, h2f ? "H2_SELFTEST: %d FAIL\n" : "H2_SELFTEST: all PASS\n", h2f);
    return h2f ? 1 : 0;
}
#endif
