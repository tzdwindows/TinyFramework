/*
 * mini_h3.h — HTTP/3 / WebTransport (Phase 6, honest stub).
 *
 * HTTP/3 runs over QUIC, which is a full UDP transport with its own crypto,
 * streams, ACK/loss-recovery and congestion control. Implementing QUIC from
 * scratch is far beyond the scope of this engine and would not be honest to
 * pretend. So this module is deliberately a small, well-marked surface:
 *
 *   • mini_h3_available()              — always 0 (QUIC not implemented).
 *   • mini_webtransport_connect(url)   — falls back to a WebSocket (ws/wss)
 *                                        via mini_ws_connect when the URL uses
 *                                        the ws(s) scheme, since WebTransport's
 *                                        JS surface overlaps WebSocket's; for
 *                                        a true http(s):// WebTransport URL it
 *                                        returns NULL with a logged reason.
 *
 * A real QUIC stack can later be dropped behind these functions without
 * touching call sites.
 */
#ifndef MINI_H3_H
#define MINI_H3_H

#include "mini_websocket.h" /* MiniWS used as the WebTransport fallback handle */

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if a real HTTP/3 (QUIC) transport is available. Currently always 0. */
int mini_h3_available(void);

/* Open a WebTransport session. Returns a MiniWS* (via WebSocket fallback) on
 * ws/wss URLs, or NULL (with a logged reason) for true http/https WebTransport. */
MiniWS *mini_webtransport_connect(const char *url, const char *origin);

#ifdef __cplusplus
}
#endif
#endif /* MINI_H3_H */
