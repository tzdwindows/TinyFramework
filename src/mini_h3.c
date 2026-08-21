/*
 * mini_h3.c — HTTP/3 / WebTransport: honest stub + WebSocket fallback.
 */
#include "mini_h3.h"
#include "mini_log.h"
#include <string.h>

int mini_h3_available(void)
{
    /* QUIC is not implemented in this engine. Surfacing this explicitly beats
       silently returning a broken session. */
    return 0;
}

MiniWS *mini_webtransport_connect(const char *url, const char *origin)
{
    if (!url)
        return NULL;
    /* Pragmatic fallback: a WebTransport session over a ws(s) URL is mapped
       to a real WebSocket (mini_ws_connect). This gives apps a working,
       standards-shaped bidirectional session today. */
    if (!strncmp(url, "ws://", 5) || !strncmp(url, "wss://", 6))
    {
        MINI_LOGI("net.h3", "webtransport: falling back to WebSocket for %s (no QUIC)", url);
        return mini_ws_connect(url, origin);
    }
    /* A true http(s):// WebTransport URL needs HTTP/3 over QUIC, which is not
       built. Report clearly rather than producing a half-broken session. */
    MINI_LOGW("net.h3", "webtransport: %s requires HTTP/3 over QUIC, which is not "
                        "implemented (mini_h3_available()=%d); use ws/wss for a "
                        "fallback bidirectional session", url, mini_h3_available());
    return NULL;
}
