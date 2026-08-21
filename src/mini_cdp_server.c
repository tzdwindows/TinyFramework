/*
 * mini_cdp_server.c — minimal CDP (Chrome DevTools Protocol) over WebSocket.
 *
 * Pure C99, no third-party deps. SHA-1 + Base64 + WebSocket frame codec +
 * a key-oriented JSON extractor sufficient for CDP request messages.
 *
 *   DOM.getDocument   -> host serializes the MiniNode tree
 *   Runtime.evaluate   -> host evaluates JS in QuickJS, returns JSON value
 *   console.*          -> host pushes Runtime.consoleAPICalled via emit_log
 *
 * Networking is non-blocking + single-threaded (polled from the main loop),
 * so all CDP work happens on the QuickJS thread — no locks needed.
 *
 * SHA-1 is SELF-VERIFIED (gcc -DCDP_SELFTEST) against the FIPS vectors.
 */
#include "mini_cdp.h"
#include "mini_dom.h"
#include "mini_js_bridge.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#define MINI_CLOSE closesocket
#define MINI_EWOULD WSAEWOULDBLOCK
static int mini_wsastarted = 0;
#define MINI_SOCK SOCKET
#define MINI_INVALID INVALID_SOCKET
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#define MINI_CLOSE close
#define MINI_EWOULD EWOULDBLOCK
typedef int MINI_SOCK;
#define MINI_INVALID (-1)
#endif

#if defined(_WIN32)
#define strncasecmp _strnicmp
#endif

static const char *strcasestr_custom(const char *haystack, const char *needle)
{
    if (!*needle)
        return haystack;
    for (; *haystack; haystack++)
    {
        if (tolower((unsigned char)*haystack) == tolower((unsigned char)*needle))
        {
            const char *h = haystack + 1, *n = needle + 1;
            while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n))
            {
                h++;
                n++;
            }
            if (!*n)
                return haystack;
        }
    }
    return NULL;
}

/* ================================================================== */
/* SHA-1 (FIPS 180-4)                                                  */
/* ================================================================== */
#define ROL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
static void sha1(const uint8_t *msg, size_t len, uint8_t out[20])
{
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
             h3 = 0x10325476, h4 = 0xC3D2E1F0;
    uint64_t bits = (uint64_t)len * 8;
    size_t padded = ((len + 9 + 63) / 64) * 64;
    uint8_t *buf = (uint8_t *)calloc(padded, 1);
    if (!buf)
        return;
    memcpy(buf, msg, len);
    buf[len] = 0x80;
    buf[padded - 8] = (uint8_t)(bits >> 56);
    buf[padded - 7] = (uint8_t)(bits >> 48);
    buf[padded - 6] = (uint8_t)(bits >> 40);
    buf[padded - 5] = (uint8_t)(bits >> 32);
    buf[padded - 4] = (uint8_t)(bits >> 24);
    buf[padded - 3] = (uint8_t)(bits >> 16);
    buf[padded - 2] = (uint8_t)(bits >> 8);
    buf[padded - 1] = (uint8_t)(bits);
    for (size_t off = 0; off < padded; off += 64)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
        {
            const uint8_t *p = buf + off + i * 4;
            w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                   ((uint32_t)p[2] << 8) | (uint32_t)p[3];
        }
        for (int i = 16; i < 80; i++)
            w[i] = ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++)
        {
            uint32_t f, k;
            if (i < 20)
            {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            }
            else if (i < 40)
            {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            }
            else if (i < 60)
            {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            }
            else
            {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t t = ROL32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = ROL32(b, 30);
            b = a;
            a = t;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }
    uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; i++)
    {
        out[i * 4 + 0] = (uint8_t)(hs[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(hs[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(hs[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(hs[i]);
    }
    free(buf);
}

/* ================================================================== */
/* Base64 (RFC 4648)                                                   */
/* ================================================================== */
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static size_t b64encode(const uint8_t *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3)
    {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n)
            v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n)
            v |= in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? B64[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

/* ================================================================== */
/* Tiny JSON key extractor (enough for CDP request messages)           */
/* ================================================================== */
/* Find "key" then : then "value"; copy the unescaped value to out.   */
static int json_get_str(const char *j, const char *key, char *out, size_t cap)
{
    char needle[64];
    /* build "key" */
    size_t m = snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *p = strstr(j, needle);
    if (!p)
        return -1;
    p += m;
    while (*p && *p != ':')
        p++;
    if (*p != ':')
        return -1;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '"')
        return -1;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < cap)
    {
        if (*p == '\\' && p[1])
        {
            char c = p[1];
            if (c == 'n')
                out[o++] = '\n';
            else if (c == 't')
                out[o++] = '\t';
            else if (c == '"')
                out[o++] = '"';
            else if (c == '\\')
                out[o++] = '\\';
            else
                out[o++] = c;
            p += 2;
        }
        else
            out[o++] = *p++;
    }
    out[o] = 0;
    return (int)o;
}
static long json_get_int(const char *j, const char *key)
{
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *p = strstr(j, needle);
    if (!p)
        return -1;
    p += strlen(needle);
    while (*p && *p != ':')
        p++;
    if (*p != ':')
        return -1;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    return strtol(p, NULL, 10);
}

/* ================================================================== */
/* Per-client state                                                    */
/* ================================================================== */
#define CDP_MAX_CLIENTS 16
#define CDP_BUF 65536       /* per-client recv buffer + scratch size   */
#define CDP_MAX_FRAG (4 * 1024 * 1024) /* cap for a single reassembled
                                          CDP message (4 MiB)          */

typedef struct
{
    MINI_SOCK fd;
    int upgraded; /* 1 once WS handshake done           */
    uint8_t *rx;   /* recv accumulation buffer           */
    size_t rx_len;
    /* WebSocket fragmentation reassembly (RFC 6455 §5.4) */
    uint8_t *frag;     /* grown on demand while FIN=0   */
    size_t frag_len;
    size_t frag_cap;
    int frag_op; /* opcode that started the fragment (1/2) */
} CDPClient;

struct MiniCDP
{
    MINI_SOCK listen_fd;
    uint16_t port;
    MiniCDPCallbacks cb;
    MiniCDPHost host;            /* attached engine pointers (P1 full CDP)  */
    int host_attached;          /* 1 once mini_cdp_attach_host() was called */
    CDPClient clients[CDP_MAX_CLIENTS];
    char scratch[CDP_BUF]; /* shared scratch for JSON build */
};

/* ================================================================== */
/* Socket helpers                                                      */
/* ================================================================== */
static void set_nonblock(MINI_SOCK fd)
{
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}
static int would_block(void)
{
#ifdef _WIN32
    return WSAGetLastError() == MINI_EWOULD;
#else
    return errno == MINI_EWOULD;
#endif
}

/* ================================================================== */
/* Reliable send: loop until all bytes are written, tolerating         */
/* EWOULDBLOCK on non-blocking sockets. Returns 0 on success.         */
/* Capped at ~2 s of spinning so a stalled client can't wedge the     */
/* single-threaded main loop forever.                                 */
/* ================================================================== */
static int send_all(MINI_SOCK fd, const char *buf, size_t len)
{
    size_t off = 0;
    int spins = 0;
    while (off < len)
    {
        int n = send(fd, (const char *)buf + off, (int)(len - off), 0);
        if (n < 0)
        {
            if (would_block())
            {
                if (++spins > 2000)
                    return -1; /* peer isn't draining the socket */
#ifdef _WIN32
                Sleep(1);
#else
                usleep(1000);
#endif
                continue;
            }
            return -1; /* hard error / reset */
        }
        if (n == 0)
            return -1; /* peer closed the connection */
        off += (size_t)n;
        spins = 0;
    }
    return 0;
}

/* ================================================================== */
/* WebSocket frame write (server->client: unmasked, FIN=1)           */
/* ================================================================== */
static int ws_send_text(MINI_SOCK fd, const char *txt, size_t len)
{
    uint8_t hdr[10];
    size_t hlen;
    hdr[0] = 0x81; /* FIN + Text frame */
    if (len < 126)
    {
        hdr[1] = (uint8_t)len;
        hlen = 2;
    }
    else if (len < 65536)
    {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)len;
        hlen = 4;
    }
    else
    {
        hdr[1] = 127;
        for (int i = 0; i < 8; i++)
            hdr[2 + i] = (uint8_t)(len >> (8 * (7 - i)));
        hlen = 10;
    }

    size_t total = hlen + len;
    uint8_t *buf = (uint8_t *)malloc(total ? total : 1);
    if (!buf)
        return -1;
    memcpy(buf, hdr, hlen);
    if (len)
        memcpy(buf + hlen, txt, len);
    int rc = send_all(fd, (const char *)buf, total);
    free(buf);
    return rc;
}

/* Send a WebSocket control frame (server->client, unmasked, FIN=1). */
/* opcode: 0x8 close, 0xA pong. Control payloads must be <= 125 B.     */
static int ws_send_control(MINI_SOCK fd, uint8_t opcode,
                           const uint8_t *payload, size_t plen)
{
    if (plen > 125)
        plen = 125;
    uint8_t hdr[2] = {(uint8_t)(0x80 | opcode), (uint8_t)plen};
    size_t total = 2 + plen;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf)
        return -1;
    buf[0] = hdr[0];
    buf[1] = hdr[1];
    if (plen)
        memcpy(buf + 2, payload, plen);
    int rc = send_all(fd, (const char *)buf, total);
    free(buf);
    return rc;
}

/* ================================================================== */
/* WebSocket handshake (HTTP/1.1 Upgrade)                              */
/* ================================================================== */
static int do_handshake(CDPClient *c)
{
    c->rx[c->rx_len] = 0;

    const char *keyhdr = NULL;
    for (const char *p = (const char *)c->rx; *p; p++)
    {
        if ((p == (const char *)c->rx || p[-1] == '\n') &&
            !strncasecmp(p, "Sec-WebSocket-Key", 17))
        {
            keyhdr = p;
            break;
        }
    }
    if (!keyhdr)
        return -1;

    keyhdr = strchr(keyhdr, ':') + 1;
    while (*keyhdr && (*keyhdr == ' ' || *keyhdr == '\t'))
        keyhdr++;
    char wsk[256];
    size_t k = 0;
    while (*keyhdr && *keyhdr != '\r' && *keyhdr != '\n' && k + 1 < sizeof wsk)
        wsk[k++] = *keyhdr++;
    wsk[k] = 0;

    char concat[256];
    const char *GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    size_t cn = snprintf(concat, sizeof concat, "%s%s", wsk, GUID);
    if (cn >= sizeof concat)
        cn = sizeof concat - 1; /* clamp to what was actually written */
    uint8_t dig[20];
    sha1((const uint8_t *)concat, cn, dig);
    char acc[32];
    b64encode(dig, 20, acc);
    char resp[512];
    int rn = snprintf(resp, sizeof resp,
                      "HTTP/1.1 101 Switching Protocols\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: %s\r\n\r\n",
                      acc);
    if (send_all(c->fd, resp, (size_t)rn) < 0)
        return -1;

    c->upgraded = 1;
    const char *end_hdr = strstr((const char *)c->rx, "\r\n\r\n");
    if (end_hdr)
    {
        size_t hdr_len = (size_t)(end_hdr - (const char *)c->rx) + 4;
        if (c->rx_len > hdr_len)
        {
            size_t rem = c->rx_len - hdr_len;
            memmove(c->rx, c->rx + hdr_len, rem);
            c->rx_len = rem;
        }
        else
        {
            c->rx_len = 0;
        }
    }
    else
    {
        c->rx_len = 0;
    }
    return 0;
}

/* ================================================================== */
/* Decode one WebSocket frame from the rx buffer; return payload text  */
/* in place (unmasked).                                                */
/*                                                                    */
/* Returns:                                                           */
/*    >=0  payload length (written & NUL-terminated into `out`);      */
/*          *fin = FIN bit, *opcode = frame opcode                    */
/*    -1   incomplete frame (need more bytes)                         */
/*    -2   close frame received (a close echo has already been sent)  */
/*                                                                    */
/* PING (0x9) is answered with a PONG (0xA) here; the caller still   */
/* sees opcode 0x9 and must skip dispatching it as a CDP message.    */
/* PONG (0xA) is acknowledged by ignoring it.                        */
/* ================================================================== */
static int ws_decode_one(CDPClient *c, char *out, size_t out_cap,
                         int *fin, int *opcode)
{
    *fin = 0;
    *opcode = 0;
    if (c->rx_len < 2)
        return -1;
    uint8_t b0 = c->rx[0], b1 = c->rx[1];
    *fin = (b0 & 0x80) ? 1 : 0;
    *opcode = b0 & 0x0f;
    int masked = b1 & 0x80;
    uint64_t plen = b1 & 0x7f;
    size_t hlen = 2;
    if (plen == 126)
    {
        if (c->rx_len < 4)
            return -1;
        plen = ((uint64_t)c->rx[2] << 8) | c->rx[3];
        hlen = 4;
    }
    else if (plen == 127)
    {
        if (c->rx_len < 10)
            return -1;
        plen = 0;
        for (int i = 0; i < 8; i++)
            plen = (plen << 8) | c->rx[2 + i];
        hlen = 10;
    }
    uint8_t mask[4];
    if (masked)
    {
        if (c->rx_len < hlen + 4)
            return -1;
        memcpy(mask, c->rx + hlen, 4);
        hlen += 4;
    }
    if (c->rx_len < hlen + plen)
        return -1;
    const uint8_t *payload = c->rx + hlen;
    size_t n = plen < out_cap - 1 ? (size_t)plen : out_cap - 1;
    for (size_t i = 0; i < n; i++)
        out[i] = (char)(masked ? (payload[i] ^ mask[i % 4]) : payload[i]);
    out[n] = 0;

    /* consume this frame from the rx buffer */
    size_t total = hlen + (size_t)plen;
    memmove(c->rx, c->rx + total, c->rx_len - total);
    c->rx_len -= total;

    if (*opcode == 0x8)
    { /* close: RFC 6455 §5.5.1 — echo a close frame back, then close */
        ws_send_control(c->fd, 0x8, (const uint8_t *)out, n);
        return -2;
    }
    if (*opcode == 0x9)
    { /* ping: reply with a pong carrying the same payload */
        ws_send_control(c->fd, 0xA, (const uint8_t *)out, n);
        return (int)n;
    }
    return (int)n;
}

/* ================================================================== */
/* Tear down a client slot (socket + state)                           */
/* ================================================================== */
static void cdp_drop_client(CDPClient *c)
{
    if (c->fd != MINI_INVALID)
        MINI_CLOSE(c->fd);
    c->fd = MINI_INVALID;
    c->upgraded = 0;
    c->rx_len = 0;
    c->frag_len = 0;
    c->frag_op = 0;
}

/* Send a CDP response / event as a WS text frame. On hard send       */
/* failure the client is dropped (its socket is wedged).              */
static void cdp_send(MiniCDP *cdp, CDPClient *c, const char *json, size_t len)
{
    if (ws_send_text(c->fd, json, len) < 0)
        cdp_drop_client(c);
    (void)cdp;
}

/* ---- public send surface for the domain layer (mini_cdp_domains.c) ---- */
void mini_cdp_send_client(MiniCDP *cdp, int client_index,
                          const char *json, size_t len)
{
    if (!cdp || client_index < 0 || client_index >= CDP_MAX_CLIENTS)
        return;
    CDPClient *c = &cdp->clients[client_index];
    if (c->fd == MINI_INVALID || !c->upgraded)
        return;
#ifdef CDP_DEBUG_DUMP
    /* set CDP_DEBUG=1 to dump the first ~300 chars of every CDP response
       on stderr — lets us see the exact JSON DevTools receives. */
    {
        static int s_dump = -1;
        if (s_dump < 0)
        {
            const char *e = getenv("CDP_DEBUG");
            s_dump = (e && (e[0] == '1' || e[0] == 't' || e[0] == 'T')) ? 1 : 0;
        }
        if (s_dump)
        {
            size_t n = len < 300 ? len : 300;
            fprintf(stderr, "[cdp→] %.*s%s\n", (int)n, json, n < len ? "…" : "");
        }
    }
#endif
    if (ws_send_text(c->fd, json, len) < 0)
        cdp_drop_client(c);
}

void mini_cdp_broadcast(MiniCDP *cdp, const char *json, size_t len)
{
    if (!cdp)
        return;
    for (int i = 0; i < CDP_MAX_CLIENTS; i++)
    {
        CDPClient *c = &cdp->clients[i];
        if (c->fd != MINI_INVALID && c->upgraded)
            if (ws_send_text(c->fd, json, len) < 0)
                cdp_drop_client(c);
    }
}

MiniCDPHost *mini_cdp_host(MiniCDP *cdp)
{
    return cdp ? &cdp->host : NULL;
}

/* ================================================================== */
/* CDP dispatch                                                       */
/*                                                                    */
/* The host only really implements DOM.getDocument and Runtime.evaluate;*/
/* everything else is acknowledged with an empty result so DevTools    */
/* never stalls waiting on a request id. A few enable-* commands also */
/* emit the lifecycle events the corresponding panel needs to boot.   */
/* ================================================================== */
static void cdp_handle(MiniCDP *cdp, CDPClient *c, const char *msg)
{
    /* Full-domain CDP (mini_cdp_domains.c) drives responses from the live
       engine when a host is attached; otherwise fall through to the
       legacy on_dom/on_eval callbacks below. */
    if (cdp->host_attached)
    {
        mini_cdp_dispatch(cdp, (int)(c - cdp->clients), msg);
        return;
    }
    long id = json_get_int(msg, "id");
    char method[128];
    int got = json_get_str(msg, "method", method, sizeof method);

    if (got >= 0)
    {
        if (!strcmp(method, "Runtime.enable"))
        {
            char r[64];
            int n = snprintf(r, sizeof r, "{\"id\":%ld,\"result\":{}}", id);
            cdp_send(cdp, c, r, (size_t)n);
            /* Console panel requires a context before it will show any
               Runtime.consoleAPICalled events we push later. */
            static const char evt[] =
                "{\"method\":\"Runtime.executionContextCreated\","
                "\"params\":{\"context\":{\"id\":1,\"origin\":\"\","
                "\"name\":\"TinyFramework\",\"aux\":{\"isDefault\":true}}}}";
            cdp_send(cdp, c, evt, sizeof evt - 1);
            return;
        }
        if (!strcmp(method, "Page.enable"))
        {
            char r[64];
            int n = snprintf(r, sizeof r, "{\"id\":%ld,\"result\":{}}", id);
            cdp_send(cdp, c, r, (size_t)n);
            /* Give the Page panel a root frame so it initialises. */
            char evt[256];
            int en = snprintf(evt, sizeof evt,
                              "{\"method\":\"Page.frameNavigated\","
                              "\"params\":{\"frame\":{\"id\":\"1\","
                              "\"url\":\"about:blank\",\"name\":\"\","
                              "\"mimeType\":\"text/html\","
                              "\"securityOrigin\":\"\",\"loaderId\":\"1\"}}}");
            cdp_send(cdp, c, evt, (size_t)en);
            return;
        }
        if (!strcmp(method, "DOM.getDocument"))
        {
            cdp->cb.on_dom(cdp->scratch, CDP_BUF, cdp->cb.ud);
            cdp->scratch[CDP_BUF - 1] = 0;
            char head[128];
            int hn = snprintf(head, sizeof head,
                              "{\"id\":%ld,\"result\":{\"root\":", id);
            char tail[] = "}}";
            size_t sl = strlen(cdp->scratch);
            size_t total = (size_t)hn + sl + sizeof(tail) - 1;
            char *buf = (char *)malloc(total + 1);
            if (buf)
            {
                memcpy(buf, head, hn);
                memcpy(buf + hn, cdp->scratch, sl);
                memcpy(buf + hn + sl, tail, sizeof tail - 1);
                buf[total] = 0;
                cdp_send(cdp, c, buf, total);
                free(buf);
            }
            return;
        }
        if (!strcmp(method, "Runtime.evaluate"))
        {
            /* Heap-allocate the expression: it can be as long as the
               whole message (pasting a script into the console). */
            size_t msglen = strlen(msg);
            char *expr = (char *)malloc(msglen + 1);
            if (!expr)
                return;
            if (json_get_str(msg, "expression", expr, msglen + 1) < 0)
                expr[0] = 0;
            cdp->cb.on_eval(expr[0] ? expr : "undefined",
                            cdp->scratch, CDP_BUF, cdp->cb.ud);
            cdp->scratch[CDP_BUF - 1] = 0;
            free(expr);

            /* on_eval wrote a JSON-encoded value into scratch. Build a
               CDP RemoteObject with the right `type` inferred from the
               value's first character, returning it by value. */
            const char *v = cdp->scratch;
            size_t vlen = strlen(v);
            const char *type = "undefined";
            const char *subtype = "";
            int has_value = 0;
            if (vlen > 0)
            {
                char c0 = v[0];
                if (c0 == '"')
                    type = "string", has_value = 1;
                else if (c0 == '-' || (c0 >= '0' && c0 <= '9'))
                    type = "number", has_value = 1;
                else if (!strcmp(v, "true") || !strcmp(v, "false"))
                    type = "boolean", has_value = 1;
                else if (!strcmp(v, "null"))
                    type = "object", subtype = ",\"subtype\":\"null\"", has_value = 1;
                else if (c0 == '{' || c0 == '[')
                    type = "object", has_value = 1;
            }

            if (!has_value)
            {
                char r[96];
                int n = snprintf(r, sizeof r,
                                 "{\"id\":%ld,\"result\":{\"result\":{\"type\":\"%s\"}}}",
                                 id, type);
                cdp_send(cdp, c, r, (size_t)n);
            }
            else
            {
                char head[160];
                int hn = snprintf(head, sizeof head,
                                  "{\"id\":%ld,\"result\":{\"result\":{\"type\":\"%s\"%s,\"value\":",
                                  id, type, subtype);
                char tail[] = "}}}";
                size_t sl = vlen;
                size_t total = (size_t)hn + sl + (sizeof tail - 1);
                char *buf = (char *)malloc(total + 1);
                if (buf)
                {
                    memcpy(buf, head, hn);
                    memcpy(buf + hn, v, sl);
                    memcpy(buf + hn + sl, tail, sizeof tail - 1);
                    buf[total] = 0;
                    cdp_send(cdp, c, buf, total);
                    free(buf);
                }
            }
            return;
        }
    }

    /* Catch-all: acknowledge any request (even one whose method we
       couldn't parse because the frame was truncated) so DevTools never
       hangs waiting on an id. Notifications (id < 0) get nothing. */
    if (id >= 0)
    {
        char r[64];
        int n = snprintf(r, sizeof r, "{\"id\":%ld,\"result\":{}}", id);
        cdp_send(cdp, c, r, (size_t)n);
    }
}

/* ================================================================== */
/* HTTP discovery (GET /json) for chrome://inspect                    */
/*                                                                    */
/* chrome://inspect polls /json (and /json/version) every couple of  */
/* seconds. Each response must be complete + sent reliably, or the    */
/* target flickers in/out of the list. Hence: CORS header present,    */
/* no-cache, header+body sent in one send_all() call.                */
/* ================================================================== */
static void send_cors_preflight(MINI_SOCK fd)
{
    static const char r[] =
        "HTTP/1.1 204 No Content\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
        "Access-Control-Allow-Headers: *\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    send_all(fd, r, sizeof r - 1);
}

static void send_discovery(MiniCDP *cdp, MINI_SOCK fd, uint16_t port, const char *req)
{
    /* Path begins after the method token: "METHOD /path HTTP/1.1" */
    const char *path = req;
    while (*path && *path != ' ')
        path++;
    while (*path == ' ')
        path++; /* now points at "/..." */

    if (path && strstr(path, "/favicon.ico"))
    {
        /* Chrome asks for a favicon over and over; answer 204 so it
           stops, instead of dumping JSON at an image request. */
        static const char r[] =
            "HTTP/1.1 204 No Content\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        send_all(fd, r, sizeof r - 1);
        return;
    }

    const char *doc_title = "TinyFramework";
    const char *doc_url = "about:blank";
    if (cdp)
    {
        if (cdp->host.doc)
        {
            const char *t = mini_doc_get_title((const struct MiniDocument *)cdp->host.doc);
            if (t && t[0])
                doc_title = t;
        }
        if (cdp->host.bridge)
        {
            const char *u = mini_bridge_get_doc_url((const struct MiniBridge *)cdp->host.bridge);
            if (u && u[0])
                doc_url = u;
        }
    }

    char body[2048];
    int bn;
    if (path && strstr(path, "/json/version"))
    {
        bn = snprintf(body, sizeof body,
                      "{\"Browser\":\"Tinyframework/1.0\","
                      "\"Protocol-Version\":\"1.3\","
                      "\"User-Agent\":\"Tinyframework/1.0\","
                      "\"V8-Version\":\"12.0\","
                      "\"WebKit-Version\":\"537.36\","
                      "\"webSocketDebuggerUrl\":\"ws://localhost:%u/devtools\"}",
                      port);
    }
    else
    {
        /* /json and /json/list — the target list. devtoolsFrontendUrl
           is the absolute form so the inspect link works from both
           chrome://inspect and a pasted devtools:// URL. */
        bn = snprintf(body, sizeof body,
                      "[{"
                      "\"id\":\"1\","
                      "\"title\":\"%s\","
                      "\"type\":\"page\","
                      "\"url\":\"%s\","
                      "\"devtoolsFrontendUrl\":\"devtools://devtools/bundled/inspector.html?ws=localhost:%u/devtools\","
                      "\"devtoolsFrontendUrlCompat\":\"devtools://devtools/bundled/inspector.html?ws=localhost:%u/devtools\","
                      "\"webSocketDebuggerUrl\":\"ws://localhost:%u/devtools\""
                      "}]",
                      doc_title, doc_url, port, port, port);
    }

    char hdr[320];
    int hn = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=utf-8\r\n"
                      "Content-Length: %d\r\n"
                      "Cache-Control: no-cache\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Connection: close\r\n\r\n",
                      bn);
    /* send header + body together so Chrome never parses a half reply */
    size_t total = (size_t)hn + (size_t)bn;
    char *buf = (char *)malloc(total ? total : 1);
    if (!buf)
        return;
    memcpy(buf, hdr, hn);
    memcpy(buf + hn, body, bn);
    send_all(fd, buf, total);
    free(buf);
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */
MiniCDP *mini_cdp_start(uint16_t port, MiniCDPCallbacks *cb)
{
#ifdef _WIN32
    if (!mini_wsastarted)
    {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
        mini_wsastarted = 1;
    }
#endif
    MINI_SOCK s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == MINI_INVALID)
        return NULL;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7f000001); /* 127.0.0.1 only */
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 4))
    {
        MINI_CLOSE(s);
        return NULL;
    }
    set_nonblock(s);
    MiniCDP *cdp = (MiniCDP *)calloc(1, sizeof *cdp);
    cdp->listen_fd = s;
    cdp->port = port;
    cdp->cb = *cb;
    for (int i = 0; i < CDP_MAX_CLIENTS; i++)
    {
        cdp->clients[i].fd = MINI_INVALID;
        cdp->clients[i].rx = (uint8_t *)malloc(CDP_BUF);
    }
    fprintf(stderr, "[CDP] listening on http://localhost:%u (chrome://inspect)\n", port);
    return cdp;
}

void mini_cdp_attach_host(MiniCDP *cdp, MiniCDPHost *host)
{
    if (!cdp)
        return;
    if (host)
    {
        cdp->host = *host;
        cdp->host_attached = 1;
    }
    else
    {
        memset(&cdp->host, 0, sizeof cdp->host);
        cdp->host_attached = 0;
    }
}

void mini_cdp_stop(MiniCDP *cdp)
{
    if (!cdp)
        return;
    for (int i = 0; i < CDP_MAX_CLIENTS; i++)
    {
        if (cdp->clients[i].fd != MINI_INVALID)
            MINI_CLOSE(cdp->clients[i].fd);
        free(cdp->clients[i].rx);
        free(cdp->clients[i].frag);
    }
    if (cdp->listen_fd != MINI_INVALID)
        MINI_CLOSE(cdp->listen_fd);
    free(cdp);
}

/* Append r bytes of a frame payload to the per-client reassembly
   buffer, growing it on demand. Returns 0 on success; on overflow or
   OOM it resets the in-flight fragment and returns -1. The buffer is
   retained across drops (freed in mini_cdp_stop) so a reused slot
   keeps its allocation. */
static int cdp_frag_append(CDPClient *c, const char *payload, int r)
{
    if (r < 0)
        r = 0;
    if ((size_t)r > CDP_MAX_FRAG ||
        c->frag_len + (size_t)r > CDP_MAX_FRAG)
    {
        c->frag_len = 0;
        c->frag_op = 0;
        return -1;
    }
    if (c->frag_len + (size_t)r > c->frag_cap)
    {
        size_t nc = c->frag_cap ? c->frag_cap : 65536;
        while (nc < c->frag_len + (size_t)r)
            nc *= 2;
        if (nc > CDP_MAX_FRAG)
            nc = CDP_MAX_FRAG;
        uint8_t *nb = (uint8_t *)realloc(c->frag, nc);
        if (!nb)
        {
            c->frag_len = 0;
            c->frag_op = 0;
            return -1;
        }
        c->frag = nb;
        c->frag_cap = nc;
    }
    if (r)
        memcpy(c->frag + c->frag_len, payload, (size_t)r);
    c->frag_len += (size_t)r;
    return 0;
}

void mini_cdp_poll(MiniCDP *cdp)
{
    if (!cdp)
        return;

    /* Accept all pending connections up to available slots */
    for (;;)
    {
        struct sockaddr_in ca;
        socklen_t cl = sizeof ca;
        MINI_SOCK ns = accept(cdp->listen_fd, (struct sockaddr *)&ca, &cl);
        if (ns == MINI_INVALID)
            break;
        set_nonblock(ns);
        int slot = -1;
        for (int i = 0; i < CDP_MAX_CLIENTS; i++)
            if (cdp->clients[i].fd == MINI_INVALID)
            {
                slot = i;
                break;
            }
        if (slot < 0)
        {
            MINI_CLOSE(ns);
            break;
        }
        CDPClient *c = &cdp->clients[slot];
        c->fd = ns;
        c->upgraded = 0;
        c->rx_len = 0;
        c->frag_len = 0;
        c->frag_op = 0;
        if (c->rx)
            c->rx[0] = 0; /* so a would_block tick never parses garbage */
    }

    /* ---- service every connected client ---- */
    for (int i = 0; i < CDP_MAX_CLIENTS; i++)
    {
        CDPClient *c = &cdp->clients[i];
        if (c->fd == MINI_INVALID)
            continue;

        /* Drain the socket. Reserve one byte for the NUL terminator so
           we never write c->rx[CDP_BUF] (the original 1-byte heap OOB
           that corrupted state and made connections flake on/off). */
        int avail = (int)(CDP_BUF - 1 - c->rx_len);
        if (avail > 0)
        {
            int n = recv(c->fd, (char *)c->rx + c->rx_len, avail, 0);
            if (n == 0)
            {
                cdp_drop_client(c);
                continue;
            }
            if (n < 0)
            {
                if (!would_block())
                {
                    cdp_drop_client(c);
                    continue;
                }
                /* no new data this tick — fall through to process backlog */
            }
            else
            {
                c->rx_len += (size_t)n;
                c->rx[c->rx_len] = 0; /* rx_len <= CDP_BUF-1, so in-bounds */
            }
        }

        if (!c->upgraded)
        {
            /* Need the full HTTP request head before we can act. */
            if (c->rx_len == 0)
                continue;
            if (!strstr((const char *)c->rx, "\r\n\r\n"))
            {
                if (c->rx_len >= CDP_BUF - 1)
                    cdp_drop_client(c); /* head too large to fit */
                continue;
            }

            /* CORS preflight (chrome://inspect can probe first). */
            if (!strncmp((const char *)c->rx, "OPTIONS ", 8))
            {
                send_cors_preflight(c->fd);
                cdp_drop_client(c);
                continue;
            }

            if (!strncmp((const char *)c->rx, "GET ", 4))
            {
                if (strcasestr_custom((const char *)c->rx, "Upgrade: websocket"))
                {
                    if (do_handshake(c) < 0)
                        cdp_drop_client(c);
                    /* on success: upgraded; fall through to drain any
                       CDP frame that was piggybacked after the headers */
                }
                else
                {
                    send_discovery(cdp, c->fd, cdp->port, (const char *)c->rx);
                    cdp_drop_client(c);
                    continue;
                }
            }
            else
            {
                static const char r[] =
                    "HTTP/1.1 405 Method Not Allowed\r\n"
                    "Content-Length: 0\r\nConnection: close\r\n\r\n";
                send_all(c->fd, r, sizeof r - 1);
                cdp_drop_client(c);
                continue;
            }
        }

        /* ---- WebSocket frame loop ---- */
        if (c->upgraded && c->rx_len > 0)
        {
            char payload[CDP_BUF];
            int r, fin, opcode;
            while ((r = ws_decode_one(c, payload, sizeof payload,
                                      &fin, &opcode)) >= 0)
            {
                /* ping(0x9) was answered with a pong inside decode;
                   pong(0xA) needs no reply. Skip both as CDP messages. */
                if (opcode == 0x9 || opcode == 0xA)
                    continue;

                if (!fin)
                {
                    /* start or extend a fragmented message */
                    if (opcode == 0x1 || opcode == 0x2)
                    {
                        c->frag_op = opcode;
                        c->frag_len = 0;
                    }
                    cdp_frag_append(c, payload, r);
                    continue;
                }

                /* FIN=1: a complete (possibly final-continuation) message */
                const char *msg = payload;
                char *assembled = NULL;

                if (c->frag_len > 0 && opcode == 0x0)
                {
                    /* last continuation frame — append then deliver */
                    if (cdp_frag_append(c, payload, r) == 0)
                    {
                        assembled = (char *)malloc(c->frag_len + 1);
                        if (assembled)
                        {
                            memcpy(assembled, c->frag, c->frag_len);
                            assembled[c->frag_len] = 0;
                            msg = assembled;
                        }
                        c->frag_len = 0;
                        c->frag_op = 0;
                    }
                    else
                    {
                        continue; /* too big — fragment already reset */
                    }
                }
                else if (c->frag_len > 0)
                {
                    /* protocol error: new data frame mid-fragment.
                       Drop the orphaned fragment and use this frame. */
                    c->frag_len = 0;
                    c->frag_op = 0;
                }

                cdp_handle(cdp, c, msg);
                free(assembled);
                if (c->fd == MINI_INVALID)
                    break; /* cdp_handle dropped the client on send error */
            }

            if (r == -2)
            {
                /* close frame received (echo already sent) */
                cdp_drop_client(c);
                continue;
            }
            if (r == -1 && c->rx_len >= CDP_BUF - 1)
            {
                /* buffer full but no complete frame: single message is
                   larger than our buffer; can't recover, drop the client. */
                cdp_drop_client(c);
                continue;
            }
        }
    }

    /* Flush any recorded fetch requests as Network events and screencast frames */
    if (cdp->host_attached)
    {
        extern void mini_cdp_screencast_flush(MiniCDP *cdp);
        mini_cdp_network_flush(cdp);
        mini_cdp_screencast_flush(cdp);
    }
}

void mini_cdp_emit_log(MiniCDP *cdp, const char *level, const char *text)
{
    if (!cdp)
        return;
    if (!level)
        level = "log";

    /* Heap-allocate the escaped copy: with CDP_BUF at 64 KiB the old
       char esc[CDP_BUF] + char evt[CDP_BUF+256] pair would burn ~128 KB
       of stack on every console line. Each source char expands to at
       most 6 bytes (\uXXXX); cap the escape to a sane console size. */
    size_t tlen = text ? strlen(text) : 0;
    size_t esc_cap = tlen * 6 + 1;
    if (esc_cap > CDP_BUF)
        esc_cap = CDP_BUF;
    char *esc = (char *)malloc(esc_cap);
    if (!esc)
        return;

    size_t o = 0;
    if (text)
    {
        for (const char *p = text; *p && o + 6 < esc_cap; p++)
        {
            unsigned char ch = (unsigned char)*p;
            if (ch == '"')      { esc[o++] = '\\'; esc[o++] = '"'; }
            else if (ch == '\\') { esc[o++] = '\\'; esc[o++] = '\\'; }
            else if (ch == '\n') { esc[o++] = '\\'; esc[o++] = 'n'; }
            else if (ch == '\r') { esc[o++] = '\\'; esc[o++] = 'r'; }
            else if (ch == '\t') { esc[o++] = '\\'; esc[o++] = 't'; }
            else if (ch < 0x20)
            {
                /* other control chars -> \u00XX to keep JSON valid */
                o += (size_t)snprintf(esc + o, esc_cap - o,
                                     "\\u%04x", ch);
            }
            else
                esc[o++] = (char)ch;
        }
    }
    esc[o] = 0;

    size_t evt_cap = o + 320;
    char *evt = (char *)malloc(evt_cap);
    if (!evt)
    {
        free(esc);
        return;
    }
    int n = snprintf(evt, evt_cap,
                     "{\"method\":\"Runtime.consoleAPICalled\","
                     "\"params\":{\"type\":\"%s\","
                     "\"args\":[{\"type\":\"string\",\"value\":\"%s\"}],"
                     "\"executionContextId\":1,"
                     "\"timestamp\":%lld}}",
                     level, esc, (long long)time(NULL));
    free(esc);
    if (n < 0 || (size_t)n >= evt_cap)
    {
        free(evt);
        return; /* wouldn't have fit — drop this line rather than send
                   truncated JSON that would choke the DevTools parser */
    }

    for (int i = 0; i < CDP_MAX_CLIENTS; i++)
    {
        CDPClient *c = &cdp->clients[i];
        if (c->fd != MINI_INVALID && c->upgraded)
            ws_send_text(c->fd, evt, (size_t)n);
    }
    free(evt);
}

/* ================================================================== */
/* SHA-1 self-test (gcc -DCDP_SELFTEST mini_cdp_server.c -lws2_32)     */
/* ================================================================== */
#ifdef CDP_SELFTEST
static void hex20(const uint8_t *d)
{
    for (int i = 0; i < 20; i++)
        printf("%02x", d[i]);
    printf("\n");
}
int main(void)
{
    uint8_t d[20];
    int fails = 0;
    sha1((const uint8_t *)"abc", 3, d);
    printf("sha1(abc)   = ");
    hex20(d);
    if (memcmp(d, (uint8_t[]){0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e, 0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d}, 20))
    {
        printf("[FAIL] sha1(abc)\n");
        fails++;
    }
    else
        printf("[PASS] sha1(abc)\n");
    sha1((const uint8_t *)"", 0, d);
    printf("sha1(\"\")    = ");
    hex20(d);
    if (memcmp(d, (uint8_t[]){0xda, 0x39, 0xa3, 0xee, 0x5e, 0x6b, 0x4b, 0x0d, 0x32, 0x55, 0xbf, 0xef, 0x95, 0x60, 0x18, 0x90, 0xaf, 0xd8, 0x07, 0x09}, 20))
    {
        printf("[FAIL] sha1(\"\")\n");
        fails++;
    }
    else
        printf("[PASS] sha1(\"\")\n");
    /* WebSocket magic: base64(sha1("dGhlIHNhbXBsZSBub25jZQ==" + GUID)) */
    char cat[256];
    const char *GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    snprintf(cat, sizeof cat, "%s%s", "dGhlIHNhbXBsZSBub25jZQ==", GUID);
    sha1((const uint8_t *)cat, strlen(cat), d);
    char acc[32];
    b64encode(d, 20, acc);
    printf("ws accept  = %s\n", acc);
    if (strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="))
    {
        printf("[FAIL] ws accept\n");
        fails++;
    }
    else
        printf("[PASS] ws accept\n");
    printf(fails ? "CDP_SELFTEST: %d FAIL\n" : "CDP_SELFTEST: all PASS\n", fails);
    return fails ? 1 : 0;
}
#endif
