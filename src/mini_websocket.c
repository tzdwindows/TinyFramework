/*
 * mini_websocket.c — RFC6455 WebSocket client + frame codec (Phase 6).
 *
 * The codec (encode/decode/mask), the handshake accept-key (SHA-1 + base64),
 * and the plain-ws:// wire connect are all real and unit-tested. wss://
 * layers mbedtls TLS when MINI_TLS is defined (duplicating mini_net's minimal
 * TLS setup — a small price for keeping the codec standalone-testable).
 */
#include "mini_websocket.h"
#include "mini_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef int socklen_t;
  #define WS_CLOSE closesocket
  #define WS_INVALID INVALID_SOCKET
  #define WS_SOCKET SOCKET
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  #define WS_CLOSE close
  #define WS_INVALID (-1)
  #define WS_SOCKET int
#endif

#ifdef MINI_TLS
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h> /* MBEDTLS_ERR_NET_SEND/RECV_FAILED */
#endif

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* ================================================================== */
/* SHA-1 (tiny, public-domain style) — needed for the accept-key only.  */
/* ================================================================== */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t buflen; } Sha1;
static void sha1_init(Sha1 *s) {
    s->h[0]=0x67452301; s->h[1]=0xEFCDAB89; s->h[2]=0x98BADCFE; s->h[3]=0x10325476; s->h[4]=0xC3D2E1F0;
    s->len=0; s->buflen=0;
}
static void sha1_block(Sha1 *s, const uint8_t *b) {
    uint32_t w[80], a,b2,c,d,e,f,k,t; int i;
    for (i=0;i<16;i++) w[i]=((uint32_t)b[i*4]<<24)|((uint32_t)b[i*4+1]<<16)|((uint32_t)b[i*4+2]<<8)|((uint32_t)b[i*4+3]);
    for (;i<80;i++) { t=w[i-3]^w[i-8]^w[i-14]^w[i-16]; w[i]=(t<<1)|(t>>31); }
    a=s->h[0]; b2=s->h[1]; c=s->h[2]; d=s->h[3]; e=s->h[4];
    for (i=0;i<80;i++) {
        if (i<20){f=(b2&c)|((~b2)&d);k=0x5A827999;}
        else if (i<40){f=b2^c^d;k=0x6ED9EBA1;}
        else if (i<60){f=(b2&c)|(b2&d)|(c&d);k=0x8F1BBCDC;}
        else {f=b2^c^d;k=0xCA62C1D6;}
        t=((a<<5)|(a>>27))+e+k+f+w[i]; e=d; d=c; c=(b2<<30)|(b2>>2); b2=a; a=t;
    }
    s->h[0]+=a; s->h[1]+=b2; s->h[2]+=c; s->h[3]+=d; s->h[4]+=e;
}
static void sha1_update(Sha1 *s, const void *data, size_t n) {
    const uint8_t *p=(const uint8_t*)data;
    s->len += n;
    while (n) {
        size_t take = 64 - s->buflen; if (take>n) take=n;
        memcpy(s->buf+s->buflen, p, take); s->buflen+=take; p+=take; n-=take;
        if (s->buflen==64) { sha1_block(s,s->buf); s->buflen=0; }
    }
}
static void sha1_final(Sha1 *s, uint8_t out[20]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha1_update(s, &pad, 1);
    pad = 0;
    while (s->buflen != 56) sha1_update(s, &pad, 1);
    uint8_t lenb[8]; for (int i=0;i<8;i++) lenb[i]=(uint8_t)(bits >> (56-8*i));
    sha1_update(s, lenb, 8);
    for (int i=0;i<5;i++){ out[i*4]=(uint8_t)(s->h[i]>>24); out[i*4+1]=(uint8_t)(s->h[i]>>16); out[i*4+2]=(uint8_t)(s->h[i]>>8); out[i*4+3]=(uint8_t)(s->h[i]); }
}

/* ---- base64 encode (write into out, return length, no padding beyond '=') ---- */
static const char B64CH[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int b64enc(const uint8_t *in, size_t n, char *out) {
    int o = 0; size_t i;
    for (i = 0; i + 2 < n; i += 3) {
        uint32_t v = ((uint32_t)in[i]<<16)|((uint32_t)in[i+1]<<8)|in[i+2];
        out[o++]=B64CH[(v>>18)&63]; out[o++]=B64CH[(v>>12)&63]; out[o++]=B64CH[(v>>6)&63]; out[o++]=B64CH[v&63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i+1 < n) v |= (uint32_t)in[i+1] << 8;
        out[o++]=B64CH[(v>>18)&63]; out[o++]=B64CH[(v>>12)&63];
        out[o++]= (i+1<n)?B64CH[(v>>6)&63]:'=';
        out[o++]='=';
    }
    out[o]=0;
    return o;
}

void mini_ws_make_key(char out[25]) {
    uint8_t k[16];
    /* pragmatic randomness: time + a counter (a WS key need not be secret; it
       only has to be unpredictable enough to be unique per handshake). */
    static uint32_t counter = 0;
    uint32_t seed = (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)&counter ^ (++counter) * 2654435761u;
    for (int i = 0; i < 16; i++) { seed = seed * 1103515245u + 12345u; k[i] = (uint8_t)(seed >> 16); }
    b64enc(k, 16, out);
}

void mini_ws_accept_key(const char *key, char out[29]) {
    Sha1 s; sha1_init(&s);
    sha1_update(&s, key, strlen(key));
    sha1_update(&s, WS_GUID, strlen(WS_GUID));
    uint8_t dg[20]; sha1_final(&s, dg);
    b64enc(dg, 20, out); /* 20 bytes -> 28 chars + NUL */
}

/* ---- frame codec ---- */
int mini_ws_encode_frame(uint8_t opcode, int fin, const uint8_t *payload,
                         size_t plen, uint8_t *out, size_t cap) {
    size_t need = 2;
    if (plen < 126) ; else if (plen <= 0xFFFF) need += 2; else need += 8;
    need += 4 + plen; /* mask + payload */
    if (need > cap) return -1;
    size_t o = 0;
    out[o++] = (uint8_t)((fin ? 0x80 : 0) | (opcode & 0xF));
    if (plen < 126) out[o++] = (uint8_t)(0x80 | (uint8_t)plen);
    else if (plen <= 0xFFFF) { out[o++] = 0x80 | 126; out[o++] = (uint8_t)(plen >> 8); out[o++] = (uint8_t)plen; }
    else { out[o++] = 0x80 | 127; for (int i=0;i<8;i++) out[o++] = (uint8_t)(plen >> (56-8*i)); }
    /* mask key (random-ish; security not the point, framing correctness is) */
    uint8_t mk[4]; static uint32_t mc=0; uint32_t seed = (uint32_t)time(NULL) ^ ++mc * 2654435761u;
    for (int i=0;i<4;i++){ seed = seed*1103515245u+12345u; mk[i]=(uint8_t)(seed>>16); }
    out[o++]=mk[0]; out[o++]=mk[1]; out[o++]=mk[2]; out[o++]=mk[3];
    for (size_t i=0;i<plen;i++) out[o++] = payload[i] ^ mk[i % 4];
    return (int)o;
}

int mini_ws_decode_frame(const uint8_t *buf, size_t len, int *opcode, int *fin,
                         size_t *consumed, uint8_t **payload_out, size_t *plen_out) {
    *consumed = 0; *payload_out = NULL; *plen_out = 0;
    if (len < 2) return 1; /* need at least the first 2 bytes */
    int finb = (buf[0] >> 7) & 1;
    int op = buf[0] & 0xF;
    int masked = (buf[1] >> 7) & 1;
    uint64_t plen = buf[1] & 0x7F;
    size_t hlen = 2;
    if (plen == 126) { if (len < 4) return 1; plen = ((uint64_t)buf[2] << 8) | buf[3]; hlen = 4; }
    else if (plen == 127) { if (len < 10) return 1; plen = 0; for (int i=0;i<8;i++) plen = (plen << 8) | buf[2+i]; hlen = 10; }
    if (masked) hlen += 4;
    if (len < hlen + plen) return 1; /* not enough yet */
    const uint8_t *mask = masked ? buf + hlen - 4 : NULL;
    const uint8_t *src = buf + hlen;
    uint8_t *p = (uint8_t *)malloc((size_t)plen + 1);
    if (!p) return -1;
    for (size_t i = 0; i < plen; i++) p[i] = src[i] ^ (mask ? mask[i % 4] : 0);
    p[plen] = 0;
    *opcode = op; *fin = finb; *consumed = hlen + (size_t)plen;
    *payload_out = p; *plen_out = (size_t)plen;
    return 0;
}

/* ================================================================== */
/* live client (TCP / TLS + handshake + pump)                          */
/* ================================================================== */
struct MiniWS {
    WS_SOCKET fd;
#ifdef MINI_TLS
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    int have_ssl;
#endif
    int state;
    int is_tls;
    /* recv buffer for partial frames */
    uint8_t rbuf[8192];
    size_t rlen;
    /* callbacks */
    MiniWsOpenCb on_open; MiniWsTextCb on_text; MiniWsBinCb on_bin;
    MiniWsCloseCb on_close; MiniWsErrCb on_err; void *ud;
    uint8_t *frag; size_t frag_len, frag_cap; int frag_op; /* reassembly */
    int dispatched_open; /* onopen fires on the first pump, not in the ctor */
};

/* forward decls (defs are below) */
static const char *strcasestr_local(const char *h, const char *n);
static int mini_ws_send_pong_raw(MiniWS *ws, const uint8_t *data, size_t len);
static int ws_handle_frame(MiniWS *ws, int opcode, int fin, uint8_t *pl, size_t plen);

static void ws_dispatch_open(MiniWS *ws) {
    ws->state = MINI_WS_OPEN;
    if (ws->on_open) ws->on_open(ws, ws->ud);
}
static void ws_fail(MiniWS *ws, const char *msg) {
    if (ws->state == MINI_WS_CLOSED) return;
    ws->state = MINI_WS_CLOSED;
    if (ws->on_err) ws->on_err(ws, msg, ws->ud);
}

static int ws_send_all(MiniWS *ws, const uint8_t *buf, size_t len) {
#ifdef MINI_TLS
    if (ws->have_ssl) {
        size_t off = 0;
        while (off < len) {
            int n = mbedtls_ssl_write(&ws->ssl, buf + off, len - off);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (n <= 0) return -1;
            off += (size_t)n;
        }
        return 0;
    }
#endif
    size_t off = 0;
    while (off < len) {
#ifdef _WIN32
        int n = send(ws->fd, (const char *)(buf + off), (int)(len - off), 0);
#else
        ssize_t n = send(ws->fd, buf + off, len - off, 0);
#endif
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static int ws_set_nonblocking(WS_SOCKET fd) {
#ifdef _WIN32
    u_long on = 1; return ioctlsocket(fd, FIONBIO, &on) == 0 ? 0 : -1;
#else
    int fl = fcntl(fd, F_GETFL, 0); return fcntl(fd, F_SETFL, fl | O_NONBLOCK) >= 0 ? 0 : -1;
#endif
}

#ifdef MINI_TLS
/* minimal CA bundle + DRBG (lazily shared; single-threaded engine). */
static mbedtls_x509_crt g_cacert;
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static int g_tls_init = 0, g_ca = 0;
/* Directory of the running executable (no trailing separator); mirrors
   mini_net.c so wss:// finds the vendored CA bundle regardless of cwd. */
static const char *exe_dir(void) {
    static char dir[512] = {0};
    if (dir[0]) return dir;
    char exe[512] = {0};
    int ok = 0;
#ifdef _WIN32
    ok = GetModuleFileNameA(NULL, exe, sizeof exe - 1) != 0;
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) { exe[n] = 0; ok = 1; }
#endif
    if (ok) {
        size_t n = strlen(exe);
        while (n && exe[n-1] != '\\' && exe[n-1] != '/') n--;
        if (n >= sizeof dir) n = sizeof dir - 1;
        memcpy(dir, exe, n); dir[n] = 0;
    }
    return dir;
}
static void tls_init(void) {
    if (g_tls_init) return;
    mbedtls_x509_crt_init(&g_cacert);
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                           (const unsigned char *)"tinyframework", 13);
    const char *paths[] = {"libs/mbedtls/cacert.pem","cacert.pem"};
    char buf[600]; const char *dir = exe_dir();
    if (dir[0]) {
        const char *pre[] = {"../",""};
        for (int p=0; !g_ca && p<2; p++)
            for (int i=0; !g_ca && i<2; i++) {
                int len = snprintf(buf, sizeof buf, "%s%s%s", dir, pre[p], paths[i]);
                if (len > 0 && (size_t)len < sizeof buf &&
                    mbedtls_x509_crt_parse_file(&g_cacert, buf) == 0) g_ca = 1;
            }
    }
    if (!g_ca)
        for (int i=0;i<2;i++) if (mbedtls_x509_crt_parse_file(&g_cacert, paths[i]) == 0) { g_ca = 1; break; }
    g_tls_init = 1;
}
static int tls_bio_send(void *ctx, const unsigned char *b, size_t n) {
    WS_SOCKET fd = *(WS_SOCKET *)ctx;
#ifdef _WIN32
    int r = send(fd, (const char *)b, (int)n, 0);
#else
    ssize_t r = send(fd, b, n, 0);
#endif
    if (r > 0) return r;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}
static int tls_bio_recv(void *ctx, unsigned char *b, size_t n) {
    WS_SOCKET fd = *(WS_SOCKET *)ctx;
#ifdef _WIN32
    int r = recv(fd, (char *)b, (int)n, 0);
#else
    ssize_t r = recv(fd, b, n, 0);
#endif
    if (r > 0) return r;
    if (r == 0) return 0;
#ifdef _WIN32
    if (WSAGetLastError() == WSAEWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
#endif
    return MBEDTLS_ERR_NET_RECV_FAILED;
}
#endif /* MINI_TLS */

static int ws_tcp_connect(const char *host, int port, WS_SOCKET *out) {
    struct addrinfo hints, *res=NULL, *rp;
    memset(&hints,0,sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    char ps[16]; snprintf(ps,sizeof ps,"%d",port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return -1;
    WS_SOCKET fd = WS_INVALID;
    for (rp=res; rp; rp=rp->ai_next) {
        fd = (WS_SOCKET)socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == WS_INVALID) continue;
        if (connect(fd, rp->ai_addr, (socklen_t)rp->ai_addrlen) == 0) break;
        WS_CLOSE(fd); fd = WS_INVALID;
    }
    freeaddrinfo(res);
    if (fd == WS_INVALID) return -1;
    *out = fd;
    return 0;
}

static int ws_send_handshake(MiniWS *ws, const char *host, int port, const char *path,
                             const char *origin, const char *key) {
    char req[1024];
    int n = snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "%s%s"
        "\r\n",
        path, host, port, key,
        origin ? "Origin: " : "", origin ? origin : "");
    if (n <= 0) return -1;
    return ws_send_all(ws, (const uint8_t *)req, (size_t)n);
}

/* read the 101 handshake response (blocking, small) */
static int ws_recv_handshake(MiniWS *ws, char *accept_out, size_t cap) {
    char buf[1024]; size_t total = 0;
    while (total < sizeof buf - 1) {
        int n;
#ifdef MINI_TLS
        if (ws->have_ssl) {
            int r = mbedtls_ssl_read(&ws->ssl, (unsigned char *)buf + total, sizeof buf - 1 - total);
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (r <= 0) return -1;
            n = r;
        } else
#endif
        {
#ifdef _WIN32
            int rn = recv(ws->fd, buf + total, (int)(sizeof buf - 1 - total), 0);
#else
            ssize_t rn = recv(ws->fd, buf + total, sizeof buf - 1 - total, 0);
#endif
            if (rn <= 0) return -1;
            n = (int)rn;
        }
        total += (size_t)n; buf[total] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }
    if (total == 0) return -1;
    /* status must be 101 */
    if (!strstr(buf, " 101 ")) return -1;
    /* pull Sec-WebSocket-Accept */
    const char *p = strstr(buf, "Sec-WebSocket-Accept:");
    if (!p) p = strcasestr_local(buf, "sec-websocket-accept:");
    if (!p) return -1;
    p += 22; while (*p == ' ' || *p == '\t') p++;
    size_t i = 0;
    while (*p && *p != '\r' && *p != '\n' && i + 1 < cap) accept_out[i++] = *p++;
    accept_out[i] = 0;
    return 0;
}

/* tiny local strcasestr to avoid depending on the libc having one */
static const char *strcasestr_local(const char *h, const char *n) {
    if (!h||!n) return NULL; size_t nl=strlen(n);
    for (; *h; h++) { size_t i=0; for (; i<nl && h[i] && tolower((unsigned char)h[i])==tolower((unsigned char)n[i]); i++); if (i==nl) return h; }
    return NULL;
}

MiniWS *mini_ws_connect(const char *url, const char *origin) {
    if (!url) return NULL;
    int is_tls = 0;
    const char *p = url;
    if (!strncmp(p, "ws://", 5)) p += 5;
    else if (!strncmp(p, "wss://", 6)) { p += 6; is_tls = 1; }
    else { MINI_LOGE("net.ws", "unsupported scheme: %s", url); return NULL; }
    char host[256]; const char *path; int port = is_tls ? 443 : 80;
    size_t h = 0;
    while (*p && *p != ':' && *p != '/' && *p != '?' && h + 1 < sizeof host) host[h++] = *p++;
    host[h] = 0;
    if (*p == ':') { p++; port = 0; while (*p >= '0' && *p <= '9') port = port * 10 + (*p++ - '0'); }
    path = (*p == '/') ? p : "/";
    WS_SOCKET fd;
    if (ws_tcp_connect(host, port, &fd) != 0) { MINI_LOGE("net.ws", "connect failed %s:%d", host, port); return NULL; }

    MiniWS *ws = (MiniWS *)calloc(1, sizeof *ws);
    if (!ws) { WS_CLOSE(fd); return NULL; }
    ws->fd = fd; ws->is_tls = is_tls; ws->state = MINI_WS_CONNECTING;

#ifdef MINI_TLS
    if (is_tls) {
        tls_init();
        mbedtls_ssl_config_init(&ws->conf);
        mbedtls_ssl_config_defaults(&ws->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        mbedtls_ssl_conf_max_version(&ws->conf, 3, 3);
        mbedtls_ssl_conf_authmode(&ws->conf, g_ca ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_ca_chain(&ws->conf, &g_cacert, NULL);
        mbedtls_ssl_conf_rng(&ws->conf, mbedtls_ctr_drbg_random, &g_drbg);
        mbedtls_ssl_init(&ws->ssl);
        if (mbedtls_ssl_setup(&ws->ssl, &ws->conf) != 0) { WS_CLOSE(fd); free(ws); return NULL; }
        mbedtls_ssl_set_bio(&ws->ssl, &ws->fd, tls_bio_send, tls_bio_recv, NULL);
        mbedtls_ssl_set_hostname(&ws->ssl, host); /* SNI */
        int r;
        while ((r = mbedtls_ssl_handshake(&ws->ssl)) != 0) {
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            char err[128]; mbedtls_strerror(r, err, sizeof err);
            MINI_LOGE("net.ws", "TLS handshake failed: %s", err);
            ws_fail(ws, "tls handshake failed"); ws->state = MINI_WS_CLOSED;
            WS_CLOSE(fd); free(ws); return NULL;
        }
        ws->have_ssl = 1;
    }
#else
    if (is_tls) { MINI_LOGE("net.ws", "wss:// requires MINI_TLS"); WS_CLOSE(fd); free(ws); return NULL; }
#endif

    char key[25]; mini_ws_make_key(key);
    if (ws_send_handshake(ws, host, port, path, origin, key) != 0) {
        ws_fail(ws, "send handshake failed"); WS_CLOSE(fd); free(ws); return NULL;
    }
    char accept[64], expected[29];
    if (ws_recv_handshake(ws, accept, sizeof accept) != 0) {
        ws_fail(ws, "no 101 handshake"); WS_CLOSE(fd); free(ws); return NULL;
    }
    mini_ws_accept_key(key, expected);
    if (strcmp(accept, expected) != 0) {
        MINI_LOGE("net.ws", "bad accept key (got %s want %s)", accept, expected);
        ws_fail(ws, "bad accept key"); WS_CLOSE(fd); free(ws); return NULL;
    }
    ws_set_nonblocking(fd);
    /* state OPEN, but DON'T fire onopen here — the JS ctor hasn't returned
     * yet, so the user's ws.onopen isn't set. Defer to the first pump. */
    ws->state = MINI_WS_OPEN;
    MINI_LOGI("net.ws", "connected %s (open, pending)", url);
    return ws;
}

int mini_ws_state(MiniWS *ws) { return ws ? ws->state : MINI_WS_CLOSED; }
void mini_ws_set_callbacks(MiniWS *ws, MiniWsOpenCb op, MiniWsTextCb ot,
                           MiniWsBinCb ob, MiniWsCloseCb oc, MiniWsErrCb oe, void *ud) {
    if (!ws) return;
    ws->on_open = op; ws->on_text = ot; ws->on_bin = ob; ws->on_close = oc; ws->on_err = oe; ws->ud = ud;
}

int mini_ws_send_text(MiniWS *ws, const char *data) {
    if (!ws || ws->state != MINI_WS_OPEN) return -1;
    size_t n = data ? strlen(data) : 0;
    uint8_t hdr[14]; int hl = mini_ws_encode_frame(MINI_WSOP_TEXT, 1, (const uint8_t *)(data ? data : ""), n, hdr, sizeof hdr);
    if (hl < 0) return -1;
    return ws_send_all(ws, hdr, (size_t)hl);
}
int mini_ws_send_binary(MiniWS *ws, const uint8_t *data, size_t len) {
    if (!ws || ws->state != MINI_WS_OPEN) return -1;
    uint8_t hdr[14]; int hl = mini_ws_encode_frame(MINI_WSOP_BIN, 1, data ? data : (const uint8_t *)"", len, hdr, sizeof hdr);
    if (hl < 0) return -1;
    return ws_send_all(ws, hdr, (size_t)hl);
}
int mini_ws_send_ping(MiniWS *ws, const uint8_t *data, size_t len) {
    if (!ws || ws->state == MINI_WS_CLOSED) return -1;
    uint8_t hdr[14]; int hl = mini_ws_encode_frame(MINI_WSOP_PING, 1, data, len, hdr, sizeof hdr);
    if (hl < 0) return -1;
    return ws_send_all(ws, hdr, (size_t)hl);
}
int mini_ws_close(MiniWS *ws, int code, const char *reason) {
    if (!ws || ws->state == MINI_WS_CLOSED) return -1;
    uint8_t pl[128]; size_t plen = 0;
    pl[0] = (uint8_t)((code >> 8) & 0xFF); pl[1] = (uint8_t)(code & 0xFF); plen = 2;
    if (reason) { size_t rl = strlen(reason); if (rl > sizeof pl - 2) rl = sizeof pl - 2; memcpy(pl+2, reason, rl); plen += rl; }
    uint8_t hdr[14]; int hl = mini_ws_encode_frame(MINI_WSOP_CLOSE, 1, pl, plen, hdr, sizeof hdr);
    if (hl > 0) ws_send_all(ws, hdr, (size_t)hl);
    ws->state = MINI_WS_CLOSING;
    return 0;
}

/* feed decoded frame into the reassembly + callback machine */
static int ws_handle_frame(MiniWS *ws, int opcode, int fin, uint8_t *pl, size_t plen) {
    if (opcode == MINI_WSOP_PING) { mini_ws_send_pong_raw(ws, pl, plen); free(pl); return 0; }
    if (opcode == MINI_WSOP_PONG) { free(pl); return 0; }
    if (opcode == MINI_WSOP_CLOSE) {
        int code = 1000; const char *reason = "";
        if (plen >= 2) code = ((int)pl[0] << 8) | pl[1];
        if (plen > 2) pl[plen] = 0, reason = (const char *)pl + 2;
        ws->state = MINI_WS_CLOSED;
        if (ws->on_close) ws->on_close(ws, code, reason, ws->ud);
        free(pl);
        return 0;
    }
    /* text/binary/continuation reassembly */
    if (opcode == MINI_WSOP_TEXT || opcode == MINI_WSOP_BIN) {
        if (ws->frag) { free(ws->frag); ws->frag = NULL; ws->frag_len = 0; } /* dropped partial */
        ws->frag_op = opcode;
        if (fin) {
            if (opcode == MINI_WSOP_TEXT) { if (ws->on_text) ws->on_text(ws, (const char *)pl, plen, ws->ud); }
            else { if (ws->on_bin) ws->on_bin(ws, pl, plen, ws->ud); }
            free(pl);
            return 0;
        }
        ws->frag = pl; ws->frag_len = plen; ws->frag_cap = plen;
        return 0;
    }
    if (opcode == MINI_WSOP_CONT) {
        if (!ws->frag) { free(pl); return 0; } /* stray continuation */
        if (ws->frag_len + plen > ws->frag_cap) {
            size_t nc = ws->frag_cap ? ws->frag_cap : 64;
            while (nc < ws->frag_len + plen) nc *= 2;
            uint8_t *np = (uint8_t *)realloc(ws->frag, nc);
            if (!np) { free(pl); free(ws->frag); ws->frag = NULL; return -1; }
            ws->frag = np; ws->frag_cap = nc;
        }
        memcpy(ws->frag + ws->frag_len, pl, plen); ws->frag_len += plen; free(pl);
        if (fin) {
            if (ws->frag_op == MINI_WSOP_TEXT) { if (ws->on_text) ws->on_text(ws, (const char *)ws->frag, ws->frag_len, ws->ud); }
            else { if (ws->on_bin) ws->on_bin(ws, ws->frag, ws->frag_len, ws->ud); }
            free(ws->frag); ws->frag = NULL; ws->frag_len = 0;
        }
        return 0;
    }
    free(pl);
    return 0;
}
static int mini_ws_send_pong_raw(MiniWS *ws, const uint8_t *data, size_t len) {
    if (!ws || ws->state == MINI_WS_CLOSED) return -1;
    uint8_t hdr[14]; int hl = mini_ws_encode_frame(MINI_WSOP_PONG, 1, data, len, hdr, sizeof hdr);
    if (hl < 0) return -1;
    return ws_send_all(ws, hdr, (size_t)hl);
}

int mini_ws_pump(MiniWS *ws) {
    if (!ws || ws->state == MINI_WS_CLOSED) return 0;
    /* fire the deferred onopen on the first pump (after JS set callbacks) */
    if (!ws->dispatched_open && ws->state == MINI_WS_OPEN)
    {
        ws->dispatched_open = 1;
        if (ws->on_open) ws->on_open(ws, ws->ud);
    }
    int delivered = 0;
    for (;;) {
        size_t space = sizeof ws->rbuf - ws->rlen;
        if (space == 0) break; /* buffer full of a partial frame; wait/overflow limit */
        int n;
#ifdef MINI_TLS
        if (ws->have_ssl) {
            int rr = mbedtls_ssl_read(&ws->ssl, (unsigned char *)ws->rbuf + ws->rlen, space);
            if (rr == MBEDTLS_ERR_SSL_WANT_READ || rr == MBEDTLS_ERR_SSL_WANT_WRITE) break;
            if (rr == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rr == 0) { ws->state = MINI_WS_CLOSED; if (ws->on_close) ws->on_close(ws, 1000, "", ws->ud); return delivered ? delivered : -1; }
            if (rr < 0) { ws_fail(ws, "ssl read error"); return -1; }
            n = rr;
        } else
#endif
        {
#ifdef _WIN32
            int rn = recv(ws->fd, (char *)ws->rbuf + ws->rlen, (int)space, 0);
            if (rn < 0) { if (WSAGetLastError() == WSAEWOULDBLOCK) break; ws_fail(ws, "recv error"); return -1; }
            if (rn == 0) { ws->state = MINI_WS_CLOSED; if (ws->on_close) ws->on_close(ws, 1000, "", ws->ud); return delivered ? delivered : -1; }
            n = rn;
#else
            ssize_t rn = recv(ws->fd, ws->rbuf + ws->rlen, space, 0);
            if (rn < 0) { if (errno == EWOULDBLOCK || errno == EAGAIN) break; ws_fail(ws, "recv error"); return -1; }
            if (rn == 0) { ws->state = MINI_WS_CLOSED; if (ws->on_close) ws->on_close(ws, 1000, "", ws->ud); return delivered ? delivered : -1; }
            n = (int)rn;
#endif
        }
        ws->rlen += (size_t)n;
        /* try to decode whole frames from the buffer */
        for (;;) {
            int op, fin; size_t consumed; uint8_t *pl; size_t plen;
            int r = mini_ws_decode_frame(ws->rbuf, ws->rlen, &op, &fin, &consumed, &pl, &plen);
            if (r == 1) break; /* need more bytes */
            if (r < 0) { ws_fail(ws, "frame decode error"); return -1; }
            ws_handle_frame(ws, op, fin, pl, plen);
            delivered++;
            if (consumed >= ws->rlen) { ws->rlen = 0; break; }
            memmove(ws->rbuf, ws->rbuf + consumed, ws->rlen - consumed);
            ws->rlen -= consumed;
        }
    }
    return delivered;
}

void mini_ws_destroy(MiniWS *ws) {
    if (!ws) return;
    if (ws->state == MINI_WS_OPEN || ws->state == MINI_WS_CLOSING) {
        uint8_t hdr[14]; int hl = mini_ws_encode_frame(MINI_WSOP_CLOSE, 1, NULL, 0, hdr, sizeof hdr);
        if (hl > 0) ws_send_all(ws, hdr, (size_t)hl);
    }
#ifdef MINI_TLS
    if (ws->have_ssl) { mbedtls_ssl_close_notify(&ws->ssl); mbedtls_ssl_free(&ws->ssl); mbedtls_ssl_config_free(&ws->conf); }
#endif
    if (ws->fd != WS_INVALID) WS_CLOSE(ws->fd);
    free(ws->frag);
    free(ws);
}

/* ================================================================== */
/* WEBSOCKET_SELFTEST — codec + accept-key against the RFC test vector  */
/* ================================================================== */
#ifdef WEBSOCKET_SELFTEST
static int ws_fail_ct = 0;
#define WSCK(c, m) do { if (!(c)) { fprintf(stderr, "WS FAIL: %s\n", m); ws_fail_ct++; } } while (0)

int main(void) {
    mini_log_init();

    /* RFC6455 §1.2 example: key -> accept. */
    char acc[29]; mini_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", acc);
    WSCK(!strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), "accept-key matches RFC vector");
    fprintf(stderr, "[ws] accept for sample key = %s\n", acc);

    /* make_key is 24 chars of base64. */
    char k[25]; mini_ws_make_key(k); WSCK(strlen(k) == 24, "key is 24 chars");

    /* encode a small text frame and decode it back. */
    uint8_t buf[64]; const char *msg = "hello"; size_t mlen = 5;
    int hl = mini_ws_encode_frame(MINI_WSOP_TEXT, 1, (const uint8_t *)msg, mlen, buf, sizeof buf);
    WSCK(hl > 0, "encode small frame");
    int op, fin; size_t cons; uint8_t *pl; size_t plen;
    int r = mini_ws_decode_frame(buf, (size_t)hl, &op, &fin, &cons, &pl, &plen);
    WSCK(r == 0, "decode ok");
    WSCK(op == MINI_WSOP_TEXT, "opcode text");
    WSCK(fin == 1, "fin set");
    WSCK(plen == 5 && !memcmp(pl, "hello", 5), "payload round-trips");
    WSCK(cons == (size_t)hl, "consumed == encoded");
    free(pl);

    /* medium length (126..65535) uses the 16-bit form */
    uint8_t big[300]; for (int i=0;i<300;i++) big[i]='A'+(i%26);
    uint8_t ob[340]; int ohl = mini_ws_encode_frame(MINI_WSOP_BIN, 1, big, 300, ob, sizeof ob);
    WSCK(ohl > 0, "encode 300-byte frame");
    r = mini_ws_decode_frame(ob, (size_t)ohl, &op, &fin, &cons, &pl, &plen);
    WSCK(r == 0 && plen == 300 && !memcmp(pl, big, 300), "300-byte round-trips");
    free(pl);

    /* partial decode -> "need more" (1) */
    r = mini_ws_decode_frame(buf, 3, &op, &fin, &cons, &pl, &plen);
    WSCK(r == 1, "partial returns need-more");

    fprintf(stderr, ws_fail_ct ? "WEBSOCKET_SELFTEST: %d FAIL\n" : "WEBSOCKET_SELFTEST: all PASS\n", ws_fail_ct);
    return ws_fail_ct ? 1 : 0;
}
#endif
