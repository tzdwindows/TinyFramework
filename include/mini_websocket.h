/*
 * mini_websocket.h — RFC6455 WebSocket client (Phase 6).
 *
 * A real (compact) WebSocket client: blocking TCP(+TLS) connect + handshake
 * with the spec accept-key, a correct frame codec (text/binary/continuation,
 * close/ping/pong, 7/16/64-bit lengths, client-side masking), and a
 * non-blocking pump the host loop calls to deliver incoming frames to JS.
 *
 * The frame codec + accept-key computation are unit-tested against the RFC's
 * own test vectors (no live server needed). The wire connect is layered on
 * the existing mini_net socket/TLS helpers when MINI_TLS is on.
 */
#ifndef MINI_WEBSOCKET_H
#define MINI_WEBSOCKET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum MiniWsState { MINI_WS_CONNECTING = 0, MINI_WS_OPEN = 1, MINI_WS_CLOSING = 2, MINI_WS_CLOSED = 3 };
enum MiniWsOpcode {
    MINI_WSOP_CONT = 0x0, MINI_WSOP_TEXT = 0x1, MINI_WSOP_BIN = 0x2,
    MINI_WSOP_CLOSE = 0x8, MINI_WSOP_PING = 0x9, MINI_WSOP_PONG = 0xA
};

typedef struct MiniWS MiniWS;

typedef void (*MiniWsTextCb)(MiniWS *ws, const char *data, size_t len, void *ud);
typedef void (*MiniWsBinCb)(MiniWS *ws, const uint8_t *data, size_t len, void *ud);
typedef void (*MiniWsOpenCb)(MiniWS *ws, void *ud);
typedef void (*MiniWsCloseCb)(MiniWS *ws, int code, const char *reason, void *ud);
typedef void (*MiniWsErrCb)(MiniWS *ws, const char *msg, void *ud);

/* Connect (ws:// or wss://) + perform the RFC6455 handshake. origin may be
 * NULL. Returns the handle (state OPEN) or NULL on failure. */
MiniWS *mini_ws_connect(const char *url, const char *origin);

int   mini_ws_state(MiniWS *ws);
void  mini_ws_set_callbacks(MiniWS *ws, MiniWsOpenCb op, MiniWsTextCb ot,
                            MiniWsBinCb ob, MiniWsCloseCb oc, MiniWsErrCb oe, void *ud);

/* Send a text/binary frame (masked, as a client must). Returns 0 on success. */
int   mini_ws_send_text(MiniWS *ws, const char *data);
int   mini_ws_send_binary(MiniWS *ws, const uint8_t *data, size_t len);
/* Send a ping (application-level keepalive). */
int   mini_ws_send_ping(MiniWS *ws, const uint8_t *data, size_t len);
/* Initiate a close handshake with a status code (1000 default). */
int   mini_ws_close(MiniWS *ws, int code, const char *reason);

/* Non-blocking: read whatever frames are already on the socket and dispatch
 * the matching callback. Returns the # of frames delivered, or -1 on a hard
 * error (the socket is closed and on_close/on_error was invoked). */
int   mini_ws_pump(MiniWS *ws);

void  mini_ws_destroy(MiniWS *ws);

/* ---- codec helpers (exposed for unit testing) ---- */
/* base64-encode 16 random bytes -> 24-char null-terminated key. */
void  mini_ws_make_key(char out[25]);
/* Compute the Sec-WebSocket-Accept value for a given key. out is 29 bytes. */
void  mini_ws_accept_key(const char *key, char out[29]);
/* Encode a client->server frame (masked) into out. Returns total length, or
 * -1 if cap too small. fin=1 final fragment. */
int   mini_ws_encode_frame(uint8_t opcode, int fin, const uint8_t *payload,
                           size_t plen, uint8_t *out, size_t cap);
/* Decode one frame from buf (server->client, NOT masked). On success *opcode,
 * *fin, *consumed are set and the payload is malloc'd into *payload_out /
 * *plen_out (caller frees). Returns 0 on a whole frame, 1 if more bytes
 * needed (partial), -1 on a protocol error. */
int   mini_ws_decode_frame(const uint8_t *buf, size_t len, int *opcode, int *fin,
                           size_t *consumed, uint8_t **payload_out, size_t *plen_out);

#ifdef __cplusplus
}
#endif
#endif /* MINI_WEBSOCKET_H */
