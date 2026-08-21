/*
 * mini_h2.h — HTTP/2 frame codec + HPACK static table (Phase 6, pragmatic).
 *
 * This is the *framing* foundation of an HTTP/2 client: a correct 9-byte
 * frame-header codec (length/type/flags/stream-id) for all frame types, plus
 * HPACK integer + literal-string (no Huffman) encode/decode and the RFC 7541
 * §A static header table (61 entries). It is genuinely testable on its own.
 *
 * What it is NOT (kept honest): a full h2 *client* — that also needs ALPN
 * negotiation over TLS, the dynamic HPACK table, and Huffman coding. Those are
 * intentionally out of scope here; the existing HTTP/1.1+TLS fetch path
 * remains the live transport. This module gives a place for h2 framing to
 * land and be unit-tested when the transport layer is extended.
 */
#ifndef MINI_H2_H
#define MINI_H2_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HTTP/2 frame types (RFC 9113 §6). */
enum {
    H2_DATA = 0x0, H2_HEADERS = 0x1, H2_PRIORITY = 0x2, H2_RST_STREAM = 0x3,
    H2_SETTINGS = 0x4, H2_PUSH_PROMISE = 0x5, H2_PING = 0x6,
    H2_GOAWAY = 0x7, H2_WINDOW_UPDATE = 0x8, H2_CONTINUATION = 0x9
};
#define H2_FLAG_ACK 0x01
#define H2_FLAG_END_STREAM 0x01
#define H2_FLAG_END_HEADERS 0x04
#define H2_FLAG_PADDED 0x08
#define H2_FLAG_PRIORITY 0x20

/* 9-byte frame header. */
typedef struct {
    uint32_t length;   /* 24-bit payload length */
    uint8_t  type;
    uint8_t  flags;
    uint32_t stream_id; /* 31-bit (top bit reserved, must be 0 on send) */
} H2FrameHeader;

/* Encode a frame header into out[9]. stream_id must have the top bit clear. */
void mini_h2_encode_header(uint8_t out[9], uint32_t length, uint8_t type,
                           uint8_t flags, uint32_t stream_id);
/* Decode 9 bytes into h. Returns 0 on success, -1 if buf is short. */
int  mini_h2_decode_header(const uint8_t *buf, size_t len, H2FrameHeader *h);

/* ---- HPACK (RFC 7541) ---- */
/* HPACK integer encode (prefix bits). Returns bytes written into out. */
int  mini_hpack_enc_int(uint8_t *out, size_t cap, uint64_t value, uint8_t prefix_bits);
/* HPACK integer decode. Returns bytes consumed (>0) or -1 on error/trunc. */
int  mini_hpack_dec_int(const uint8_t *buf, size_t len, uint8_t prefix_bits, uint64_t *out);

/* Static table lookup. idx 1..61. Returns NULL if out of range. */
const char *mini_hpack_static_name(int idx);
const char *mini_hpack_static_value(int idx);
/* Find a static index whose name matches (value ignored). -1 if none. */
int  mini_hpack_static_find_name(const char *name);

/* Decode a HEADERS payload block against the STATIC table only (no dynamic
 * table, no Huffman). Writes name/value pairs into *out_pairs (malloc'd,
 * each string malloc'd) up to *max_pairs; returns the count written, or -1 on
 * a decode error. Caller frees each string and the arrays. */
typedef struct { char *name; char *value; } H2HeaderPair;
int  mini_hpack_decode_block(const uint8_t *buf, size_t len,
                             H2HeaderPair *out, int max_pairs);

#ifdef __cplusplus
}
#endif
#endif /* MINI_H2_H */
