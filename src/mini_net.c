/*
 * mini_net.c - minimal blocking HTTP/1.1 client + request recording.
 *
 * Plain http:// uses raw winsock/POSIX send/recv. When built with -DMINI_TLS
 * (and linked against vendored mbedtls under libs/mbedtls), https:// is
 * supported: after connect() succeeds, the socket fd is wrapped in a TLS
 * session and ssl_read/ssl_write replace raw send/recv. CA verification is
 * strict when a mozilla CA bundle (libs/mbedtls/cacert.pem) is loadable,
 * otherwise it degrades to VERIFY_NONE with a stderr warning so the engine
 * still runs. The bridge's fetch()/XHR and <script src> loader all funnel
 * through mini_net_http(), so TLS here gives JS fetch() HTTPS for free.
 *
 * No proxy / HTTP2. Chunked transfer-encoding is de-chunked in place (many
 * CDNs ship JS chunked; the raw frames would otherwise break JS eval). Plain HTTP sends
 * Connection: close and reads the full response (honoring Content-Length;
 * otherwise reads until the peer closes).
 *
 * Standalone: only winsock + libc (+ mbedtls when MINI_TLS) , syntax-checks
 * on its own.
 */
#include "mini_net.h"
#include "mini_cookies.h"
#include "mini_httpcache.h"
#include "mini_policy.h"
#include "mini_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#ifdef MINI_TLS
/* mbedtls TLS: HTTPS support for mini_net_http(). Static-linked; the build
   passes -DMINI_TLS and links mbedcrypto/mbedx509/mbedtls (see CMakeLists/
   build.ps1). Plain http:// still uses raw send/recv; https:// wraps the
   already-connected socket fd in a TLS session after connect(). */
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>
#include <mbedtls/version.h>
#include <mbedtls/net_sockets.h>   /* MBEDTLS_ERR_NET_SEND/RECV_FAILED (bio) */
#endif

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#define NET_CLOSE closesocket
#define NET_EWOULD WSAEWOULDBLOCK
#define NET_INVALID INVALID_SOCKET
#define NET_SOCKET SOCKET
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#define NET_CLOSE close
#define NET_EWOULD EWOULDBLOCK
#define NET_INVALID (-1)
#define NET_SOCKET int
#endif

/* ---- cross-platform mutex (used by the TLS RNG lock, the keep-alive pool,
   the HTTP cache-while-copy lock, and the prefetch pool). Defined up here so
   both the early TLS globals and the later Phase-7 helpers can use it. */
#if defined(_WIN32)
typedef CRITICAL_SECTION net_mutex_t;
static void nmtx_init(net_mutex_t *m) { InitializeCriticalSection(m); }
static void nmtx_lock(net_mutex_t *m) { EnterCriticalSection(m); }
static void nmtx_unlock(net_mutex_t *m) { LeaveCriticalSection(m); }
#else
#include <pthread.h>
typedef pthread_mutex_t net_mutex_t;
static void nmtx_init(net_mutex_t *m) { pthread_mutex_init(m, NULL); }
static void nmtx_lock(net_mutex_t *m) { pthread_mutex_lock(m); }
static void nmtx_unlock(net_mutex_t *m) { pthread_mutex_unlock(m); }
#endif

/* ---- winsock lifecycle ---- */
static int g_net_init = 0;
int mini_net_init(void)
{
    if (g_net_init)
        return 0;
#ifdef _WIN32
    WSADATA d;
    if (WSAStartup(MAKEWORD(2, 2), &d) != 0)
        return -1;
#endif
    g_net_init = 1;
    return 0;
}

/* ---- recording table ---- */
#define NET_REC_CAP 256
static MiniNetRecord g_recs[NET_REC_CAP];
static int g_rec_n = 0;
static int g_rec_next_id = 1;

void mini_net_record_free(MiniNetRecord *rec)
{
    if (!rec)
        return;
    free(rec->req_headers);
    free(rec->req_body);
    free(rec->resp_headers);
    free(rec->resp_body);
    rec->req_headers = rec->req_body = rec->resp_headers = rec->resp_body = NULL;
}

void mini_net_record_add(MiniNetRecord *rec)
{
    if (!rec)
        return;
    int idx;
    if (g_rec_n < NET_REC_CAP)
    {
        idx = g_rec_n++;
    }
    else
    {
        /* ring: evict the oldest */
        mini_net_record_free(&g_recs[0]);
        memmove(&g_recs[0], &g_recs[1], (NET_REC_CAP - 1) * sizeof(MiniNetRecord));
        idx = NET_REC_CAP - 1;
        g_rec_n = NET_REC_CAP;
    }
    g_recs[idx] = *rec;           /* steal the malloc'd pointers */
    memset(rec, 0, sizeof *rec);  /* donor no longer owns them */
    g_recs[idx].id = g_rec_next_id++;
    g_recs[idx].reported = 0;
}

int mini_net_record_count(void) { return g_rec_n; }
const MiniNetRecord *mini_net_record_get(int i)
{
    if (i < 0 || i >= g_rec_n)
        return NULL;
    return &g_recs[i];
}
void mini_net_record_mark_reported(int i)
{
    if (i >= 0 && i < g_rec_n)
        g_recs[i].reported = 1;
}

/* ---- URL parser: http(s)://host[:port]/path ----
   is_https: 0 = plain http, 1 = https (port 443). TLS is layered on the
   already-connected socket inside mini_net_http when MINI_TLS is defined. */
static int parse_url(const char *url, char *host, size_t hcap, int *port,
                     char *path, size_t pcap, int *is_https)
{
    *port = 80;
    *is_https = 0;
    path[0] = '/';
    path[1] = 0;
    const char *p = url;
    if (!strncmp(p, "http://", 7))
        p += 7;
    else if (!strncmp(p, "https://", 8))
    {
        p += 8;
        *is_https = 1;
        *port = 443;
    }
    /* host */
    size_t h = 0;
    while (*p && *p != ':' && *p != '/' && h + 1 < hcap)
        host[h++] = *p++;
    host[h] = 0;
    if (*p == ':')
    {
        p++;
        *port = 0;
        while (*p && *p >= '0' && *p <= '9')
            *port = *port * 10 + (*p++ - '0');
        if (*port == 0)
            *port = *is_https ? 443 : 80;
    }
    if (*p == '/')
    {
        size_t k = 0;
        while (*p && k + 1 < pcap)
            path[k++] = *p++;
        path[k] = 0;
    }
    return h > 0 ? 0 : -1;
}

/* dynamic byte buffer for the response */
typedef struct { char *p; size_t len, cap; } ByteBuf;
static void bb_ensure(ByteBuf *b, size_t extra)
{
    if (b->len + extra + 1 > b->cap)
    {
        size_t nc = b->cap ? b->cap : 4096;
        while (nc < b->len + extra + 1)
            nc *= 2;
        char *np = (char *)realloc(b->p, nc);
        if (np) { b->p = np; b->cap = nc; }
    }
}
static void bb_putc(ByteBuf *b, char c) { bb_ensure(b, 1); if (b->p) b->p[b->len++] = c; }
static void bb_put(ByteBuf *b, const void *src, size_t n)
{
    bb_ensure(b, n);
    if (b->p) { memcpy(b->p + b->len, src, n); b->len += n; }
}

#ifdef MINI_TLS
/* ---- TLS layer (mbedtls) ------------------------------------------- */
/* One global CA bundle + DRBG, lazily initialized on the first https
   request. The engine is single-threaded, so unsynchronized globals are
   safe here (matching the rest of this file's model). */
static mbedtls_x509_crt  g_cacert;
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_ctr_drbg;
static int g_tls_inited = 0;
static int g_ca_loaded = 0;

/* Serializes mbedtls_ctr_drbg_random (the shared DRBG) so concurrent TLS
   handshakes on background prefetch threads don't race -> corrupt RNG ->
   SSL_INVALID_RECORD. Declared here; the net_mutex_t type is defined later
   (near the keep-alive pool) but is just a CRITICAL_SECTION/pthread_mutex. */
static net_mutex_t g_rng_lock;
static int g_rng_lock_inited = 0;
static int tls_rng_locked(void *p_rng, unsigned char *buf, size_t len)
{
    (void)p_rng;
    if (!g_rng_lock_inited) { nmtx_init(&g_rng_lock); g_rng_lock_inited = 1; }
    nmtx_lock(&g_rng_lock);
    int r = mbedtls_ctr_drbg_random(&g_ctr_drbg, buf, len);
    nmtx_unlock(&g_rng_lock);
    return r;
}

/* Directory of the running executable (no trailing separator). Locates the
   vendored CA bundle regardless of the launcher's cwd — e.g. run as
   build\tiny_app.exe, the bundle is at <dir>\..\libs\mbedtls\cacert.pem.
   Returns "" if the path can't be determined. */
static const char *exe_dir(void)
{
    static char dir[512] = {0};
    if (dir[0]) return dir;
    char exe[512] = {0};
    int ok = 0;
#if defined(_WIN32)
    ok = GetModuleFileNameA(NULL, exe, sizeof exe - 1) != 0;
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) { exe[n] = 0; ok = 1; }
#endif
    if (ok)
    {
        size_t n = strlen(exe);
        while (n && exe[n-1] != '\\' && exe[n-1] != '/') n--;
        if (n >= sizeof dir) n = sizeof dir - 1;
        memcpy(dir, exe, n);
        dir[n] = 0;
    }
    return dir;
}

static void tls_globals_init(void)
{
    if (g_tls_inited)
        return;
    mbedtls_x509_crt_init(&g_cacert);
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_ctr_drbg);
    const char *pers = "tinyframework";
    if (mbedtls_ctr_drbg_seed(&g_ctr_drbg, mbedtls_entropy_func,
                              &g_entropy, (const unsigned char *)pers,
                              strlen(pers)) != 0)
    {
        fprintf(stderr, "[net] WARNING: ctr_drbg_seed failed; TLS will fail\n");
    }
    /* Try a few plausible CA bundle locations. Exe-relative first so the
       bundle resolves regardless of the launcher's cwd (e.g. launched from
       build/ where cwd-relative libs/... would miss it); cwd-relative is the
       fallback for project-root launches. */
    const char *ca_paths[] = {
        "libs/mbedtls/cacert.pem",
        "cacert.pem",
        "assets/cacert.pem",
    };
    char cabuf[600];
    const char *dir = exe_dir();
    if (dir[0])
    {
        /* exe in build/ -> <dir>\..\libs\... ; exe at root -> <dir>\libs\... */
        const char *pre[] = { "../", "" };
        for (size_t p = 0; !g_ca_loaded && p < sizeof pre / sizeof pre[0]; p++)
            for (size_t i = 0; !g_ca_loaded && i < sizeof ca_paths / sizeof ca_paths[0]; i++)
            {
                int len = snprintf(cabuf, sizeof cabuf, "%s%s%s",
                                   dir, pre[p], ca_paths[i]);
                if (len > 0 && (size_t)len < sizeof cabuf &&
                    mbedtls_x509_crt_parse_file(&g_cacert, cabuf) == 0)
                    g_ca_loaded = 1;
            }
    }
    if (!g_ca_loaded)
        for (size_t i = 0; i < sizeof ca_paths / sizeof ca_paths[0]; i++)
            if (mbedtls_x509_crt_parse_file(&g_cacert, ca_paths[i]) == 0)
            {
                g_ca_loaded = 1;
                break;
            }
    if (!g_ca_loaded)
        fprintf(stderr, "[net] WARNING: CA bundle not found; "
                        "TLS will be insecure (VERIFY_NONE)\n");
    g_tls_inited = 1;
}

/* Custom BIO that reads/writes our already-connected socket fd, so we never
   go through mbedtls's own connect (we already did the TCP connect). */
typedef struct { NET_SOCKET fd; } tls_bio_ctx;

static int tls_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    tls_bio_ctx *c = (tls_bio_ctx *)ctx;
#ifdef _WIN32
    int n = send(c->fd, (const char *)buf, (int)len, 0);
#else
    ssize_t n = send(c->fd, buf, len, 0);
#endif
    if (n > 0)
        return (int)n;
    if (n == 0)
        return MBEDTLS_ERR_NET_SEND_FAILED;
#ifdef _WIN32
    if (WSAGetLastError() == NET_EWOULD)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
#endif
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int tls_bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    tls_bio_ctx *c = (tls_bio_ctx *)ctx;
#ifdef _WIN32
    int n = recv(c->fd, (char *)buf, (int)len, 0);
#else
    ssize_t n = recv(c->fd, buf, len, 0);
#endif
    if (n > 0)
        return (int)n;
    if (n == 0)
        return 0; /* peer closed = EOF */
#ifdef _WIN32
    if (WSAGetLastError() == NET_EWOULD)
        return MBEDTLS_ERR_SSL_WANT_READ;
#endif
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

/* Set up an SSL client session on `fd` (already TCP-connected) and run the
   handshake. On success *ssl is ready for ssl_read/ssl_write; *conf stays
   bound to *ssl and must be freed together by the caller. Returns 0 on
   success, -1 on failure (caller closes fd). bctx must outlive the session
   (it is stored inside the ssl via set_bio). */
/* forward decl: defined with the keep-alive helpers below; used here so the
   lazy CA/DRBG init is race-free across background prefetch threads. */
static void tls_globals_init_locked(void);

static int tls_setup_and_handshake(mbedtls_ssl_context *ssl,
                                   mbedtls_ssl_config *conf,
                                   tls_bio_ctx *bctx,
                                   NET_SOCKET fd, const char *hostname)
{
    /* Locked lazy init so background prefetch threads can't race the main
       thread on first https use. Idempotent once g_tls_inited is set. */
    tls_globals_init_locked();
    int ret;
    mbedtls_ssl_config_init(conf);
    if ((ret = mbedtls_ssl_config_defaults(conf, MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
        return -1;
    /* Cap at TLS 1.2 (major=3, minor=3): mbedtls 3.6's TLS 1.3 path can fail
       when PSA crypto isn't fully wired in a minimal static build; CDNs like
       cdnjs still negotiate TLS 1.2, so this keeps HTTPS working. */
    mbedtls_ssl_conf_max_version(conf, 3, 3);
    mbedtls_ssl_conf_authmode(conf, g_ca_loaded ? MBEDTLS_SSL_VERIFY_REQUIRED
                                                 : MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_ca_chain(conf, &g_cacert, NULL);
    /* The DRBG (g_ctr_drbg) is a shared singleton; mbedtls_ctr_drbg_random is
       NOT thread-safe, so concurrent handshakes (prefetch threads + main)
       race on it -> "SSL - An invalid SSL record was received" (-0x7200).
       Wrap the RNG call in a mutex so concurrent handshakes are safe. */
    mbedtls_ssl_conf_rng(conf, tls_rng_locked, NULL);

    mbedtls_ssl_init(ssl);
    if ((ret = mbedtls_ssl_setup(ssl, conf)) != 0)
    {
        mbedtls_ssl_config_free(conf);
        return -1;
    }
    bctx->fd = fd;
    mbedtls_ssl_set_bio(ssl, bctx, tls_bio_send, tls_bio_recv, NULL);
    if (hostname && *hostname)
        mbedtls_ssl_set_hostname(ssl, hostname); /* SNI (required by CDNs) */

    while ((ret = mbedtls_ssl_handshake(ssl)) != 0)
    {
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        char err[128];
        mbedtls_strerror(ret, err, sizeof err);
        fprintf(stderr, "[net] TLS handshake failed (-0x%04x): %s\n",
                (unsigned)-ret, err);
        return -1;
    }
    return 0;
}

/* Send exactly len bytes, looping over partial sends / TLS want flags. */
static int send_all_tls(mbedtls_ssl_context *ssl, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        int n = mbedtls_ssl_write(ssl, (const unsigned char *)(buf + off), len - off);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (n <= 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}
#endif /* MINI_TLS */

/* send all bytes on a plain socket; returns 0 ok, -1 failure */
static int send_all_plain(NET_SOCKET fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
#ifdef _WIN32
        int n = send(fd, buf + off, (int)(len - off), 0);
#else
        ssize_t n = send(fd, buf + off, len - off, 0);
#endif
        if (n <= 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

/* case-insensitive substring search (HTTP headers are case-insensitive) */
static const char *stristr(const char *hay, const char *needle)
{
    if (!hay || !needle)
        return NULL;
    for (; *hay; hay++)
    {
        const char *h = hay, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { h++; n++; }
        if (!*n)
            return hay;
    }
    return NULL;
}

/* De-chunk a Transfer-Encoding: chunked body in place. Returns the new body
   length (0 on parse error). Format: <hex-size>\r\n<data>\r\n...0\r\n\r\n.
   Many CDNs (cdnjs) ship JS chunked; without this the raw chunk frames
   (e.g. the leading "1d41\r\n") sit in the body and break JS eval. */
static size_t dechunk(char *body, size_t len)
{
    char *src = body, *dst = body, *end = body + len;
    while (src < end)
    {
        size_t csz = 0;
        int h = 0;
        while (src < end && *src != '\r' && *src != '\n' && *src != ';')
        {
            char c = *src;
            int v = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (v < 0)
                break;
            csz = csz * 16 + (size_t)v;
            if (++h > 16)
                return 0;
            src++;
        }
        while (src < end && *src != '\n') src++; /* skip rest of size line */
        if (src < end) src++;                     /* skip \n */
        if (csz == 0)
            break; /* last-chunk marker */
        if (src + csz > end)
            return 0; /* truncated */
        memmove(dst, src, csz);
        dst += csz;
        src += csz;
        while (src < end && (*src == '\r' || *src == '\n')) src++; /* CRLF */
    }
    *dst = 0;
    return (size_t)(dst - body);
}

/* ================================================================== */
/* Keep-alive connection pool + Content-Length/chunked-aware reader   */
/* (Phase 7: avoid re-handshaking TLS per ES module — the single big  */
/*  win for slow CDN module loading). Defensive: if a body boundary    */
/*  can't be determined we fall back to "read until close" (exactly the */
/*  old behaviour), so body integrity is never put at risk; the only   */
/*  downside of a miss is that the connection isn't pooled.            */
/* ================================================================== */
/* (net_mutex_t / nmtx_* are defined up top, before the TLS globals.) */

/* The TLS globals (CA bundle + DRBG) are read-only after first init, but
   the lazy init itself races when background prefetch threads start. */
static net_mutex_t g_tls_lock;
static int g_tls_lock_inited = 0;
static void tls_lock_ensure(void)
{
    if (!g_tls_lock_inited) { nmtx_init(&g_tls_lock); g_tls_lock_inited = 1; }
}

#ifdef MINI_TLS
/* Wrap the lazy init so concurrent threads don't double-init the globals. */
static void tls_globals_init_locked(void)
{
    tls_lock_ensure();
    nmtx_lock(&g_tls_lock);
    tls_globals_init();   /* idempotent (guards on g_tls_inited) */
    nmtx_unlock(&g_tls_lock);
}
#endif

/* ---- keep-alive pool: fixed slots, never memmoved (mbedtls owns heap) -- */
typedef struct {
    char host[256];
    int  port;
    int  is_https;
    NET_SOCKET fd;
    int  valid;     /* 1 = fd/ssl usable */
    int  in_use;   /* 1 = currently handed to a request */
    time_t last_used;
#ifdef MINI_TLS
    mbedtls_ssl_context  ssl;
    mbedtls_ssl_config  conf;
    tls_bio_ctx bio;   /* persists for mbedtls_ssl_set_bio across reuses */
    int  has_ssl;
#endif
} KaConn;

#define KA_CAP 16
static KaConn g_ka[KA_CAP];
static net_mutex_t g_ka_lock;
static int g_ka_lock_inited = 0;
static void ka_lock_ensure(void)
{
    if (!g_ka_lock_inited) { nmtx_init(&g_ka_lock); g_ka_lock_inited = 1; }
}

static void ka_close_slot(KaConn *c)
{
    if (!c) return;
#ifdef MINI_TLS
    if (c->has_ssl)
    {
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        c->has_ssl = 0;
    }
#endif
    if (c->fd != NET_INVALID)
    {
        NET_CLOSE(c->fd);
        c->fd = NET_INVALID;
    }
    c->valid = 0;
}

/* Acquire a pooled connection for (host,port,is_https). If *out_reused=1 the
   fd+ssl are ready (no reconnect/handshake). If *out_reused=0 the caller must
   connect+handshake into the returned slot. Returns NULL only when the pool is
   entirely busy (all slots in_use): caller then does a non-pooled request. */
/* keep-alive is OFF by default: empirical testing showed esm.sh/Cloudflare
   return HTTP 400 to our keep-alive HTTP/1.1 requests (they accept
   Connection: close), so for the CDN-driven module loading that dominates real
   use, keep-alive yields ZERO benefit and only adds a wasted 400 round-trip.
   The pool + 4xx-retry code below stays compiled (and correct) so it can be
   re-enabled per-host later; with this flag 0, ka_acquire returns NULL and
   mini_net_http falls through to the original one-off close path. */
static int g_keepalive_enabled = 0;
static KaConn *ka_acquire(const char *host, int port, int is_https, int *out_reused)
{
    *out_reused = 0;
    if (!g_keepalive_enabled)
        return NULL;
    ka_lock_ensure();
    nmtx_lock(&g_ka_lock);
    /* 1. exact-match idle entry */
    for (int i = 0; i < KA_CAP; i++)
    {
        if (g_ka[i].valid && !g_ka[i].in_use &&
            g_ka[i].port == port && g_ka[i].is_https == is_https &&
            !strcmp(g_ka[i].host, host))
        {
            g_ka[i].in_use = 1;
            *out_reused = 1;
            nmtx_unlock(&g_ka_lock);
            return &g_ka[i];
        }
    }
    /* 2. a free slot. Condition must include !in_use, else two threads can
       both grab the same fresh slot (valid=0, in_use just set to 1 by the
       first) -> both handshake into the same ka->ssl -> TLS corruption. */
    for (int i = 0; i < KA_CAP; i++)
    {
        if (!g_ka[i].valid && !g_ka[i].in_use)
        {
            g_ka[i].in_use = 1;
            nmtx_unlock(&g_ka_lock);
            return &g_ka[i];
        }
    }
    /* 3. evict the oldest idle entry (any host) */
    int oldest = -1; time_t ot = 0;
    for (int i = 0; i < KA_CAP; i++)
    {
        if (g_ka[i].valid && !g_ka[i].in_use && (oldest < 0 || g_ka[i].last_used < ot))
        { oldest = i; ot = g_ka[i].last_used; }
    }
    if (oldest >= 0)
    {
        ka_close_slot(&g_ka[oldest]);
        g_ka[oldest].in_use = 1;
        nmtx_unlock(&g_ka_lock);
        return &g_ka[oldest];
    }
    nmtx_unlock(&g_ka_lock);
    return NULL; /* pool entirely busy -> caller does a one-off request */
}

/* Release a slot. reusable=1 keeps the fd+ssl for the next request; 0 closes. */
static void ka_release(KaConn *c, int reusable)
{
    if (!c) return;
    ka_lock_ensure();
    nmtx_lock(&g_ka_lock);
    if (reusable && c->valid)
    {
        c->in_use = 0;
        c->last_used = time(NULL);
    }
    else
    {
        ka_close_slot(c);
        c->in_use = 0;
    }
    nmtx_unlock(&g_ka_lock);
}

static void ka_shutdown_all(void)
{
    ka_lock_ensure();
    nmtx_lock(&g_ka_lock);
    for (int i = 0; i < KA_CAP; i++)
        ka_close_slot(&g_ka[i]);
    nmtx_unlock(&g_ka_lock);
}

/* ---- body reader: understands Content-Length / chunked; falls back to
   read-until-close. RecvCb contract: returns n>0 (data), 0 (peer EOF),
   -1 (retry: TLS WANT / EWOULDBLOCK — internally bounded), -2 (fatal). */
typedef int (*NetRecvCb)(void *io, char *buf, int cap);

/* read until the header-block terminator. Returns 0 if \r\n\r\n found
   (*hdr_end_off set), 1 if peer closed with a (partial) header but no
   terminator, -2 fatal. */
static int read_until_headers(NetRecvCb cb, void *io, ByteBuf *buf, size_t *hdr_end_off)
{
    for (;;)
    {
        if (buf->len >= 8192) return 1; /* header too large -> treat as no boundary */
        char tmp[1024];
        int n = cb(io, tmp, (int)sizeof tmp);
        if (n > 0) { bb_put(buf, tmp, (size_t)n); }
        else if (n == 0) break;          /* peer closed */
        else if (n == -1) continue;      /* retry */
        else return -2;                  /* fatal */
        char *he = buf->p ? strstr(buf->p, "\r\n\r\n") : NULL;
        if (he) { *hdr_end_off = (size_t)((he + 4) - buf->p); return 0; }
    }
    char *he = buf->p ? strstr(buf->p, "\r\n\r\n") : NULL;
    if (he) { *hdr_end_off = (size_t)((he + 4) - buf->p); return 0; }
    return 1;
}

/* Read exactly need bytes into buf (for Content-Length bodies). Returns 0 ok,
   -1 on premature EOF / fatal. `have` is bytes already in buf past the header. */
static int read_exact(NetRecvCb cb, void *io, ByteBuf *buf, size_t have, size_t need)
{
    while (have < need)
    {
        char tmp[4096];
        int want = (int)((need - have) > sizeof tmp ? sizeof tmp : (need - have));
        int n = cb(io, tmp, want);
        if (n > 0) { bb_put(buf, tmp, (size_t)n); have += (size_t)n; }
        else if (n == 0) return -1;        /* premature close */
        else if (n == -1) continue;       /* retry */
        else return -1;                   /* fatal */
    }
    return 0;
}

/* Read the rest of a chunked body until the terminating "0\r\n\r\n" (or EOF).
   The caller dechunks the accumulated raw buffer afterwards. */
static void read_chunked(NetRecvCb cb, void *io, ByteBuf *raw)
{
    for (;;)
    {
        char tmp[4096];
        int n = cb(io, tmp, (int)sizeof tmp);
        if (n > 0) bb_put(raw, tmp, (size_t)n);
        else if (n == 0) break;
        else if (n == -1) continue;
        else break;                       /* fatal */
        if (raw->p && strstr(raw->p, "\r\n0\r\n\r\n")) break;
        if (raw->len > (8 * 1024 * 1024)) break; /* sanity cap */
    }
}

/* read whatever remains until peer close (legacy fallback). */
static void read_until_close(NetRecvCb cb, void *io, ByteBuf *buf)
{
    for (;;)
    {
        char tmp[4096];
        int n = cb(io, tmp, (int)sizeof tmp);
        if (n > 0) bb_put(buf, tmp, (size_t)n);
        else if (n == 0) break;
        else if (n == -1) continue;
        else break;
        if (buf->len > (64 * 1024 * 1024)) break; /* sanity cap */
    }
}

/* Parse Content-Length / Transfer-Encoding: chunked / Connection: close out
   of a response header block. Pure (no I/O) so it is unit-testable. */
void mini_net_parse_response_meta(const char *hdrs, size_t hlen,
                                  long *out_cl, int *out_chunked, int *out_close)
{
    if (out_cl) *out_cl = -1;
    if (out_chunked) *out_chunked = 0;
    if (out_close) *out_close = 0;
    if (!hdrs) return;
    const char *end = hdrs + hlen;
    const char *p = hdrs;
    while (p < end)
    {
        const char *eol = memchr(p, '\r', (size_t)(end - p));
        if (!eol || eol >= end) break;
        size_t llen = (size_t)(eol - p);
        /* match "Name:" case-insensitively */
#define HNAME(n) (llen > (sizeof(n)-1) && !strncasecmp(p, n, sizeof(n)-1) && p[sizeof(n)-1] == ':')
        if (HNAME("Content-Length"))
        {
            const char *v = p + sizeof("Content-Length"); /* includes ':' */
            char *tail; long v2 = strtol(v, &tail, 10);
            if (tail != v && out_cl) *out_cl = v2;
        }
        else if (HNAME("Transfer-Encoding"))
        {
            const char *v = p + sizeof("Transfer-Encoding");
            if (stristr(v, "chunked") && out_chunked) *out_chunked = 1;
        }
        else if (HNAME("Connection"))
        {
            const char *v = p + sizeof("Connection");
            if (stristr(v, "close") && out_close) *out_close = 1;
        }
#undef HNAME
        /* advance past "\r\n" */
        p = eol;
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;
    }
}

/* I/O handle for the reader: a pooled KaConn (persistent ssl) or a one-off
   stack fd (+ optional stack ssl). net_recv collapses TLS WANT / EWOULDBLOCK
   into bounded retries so the reader only sees >0 / 0 / -2. */
typedef struct {
    NET_SOCKET fd;
#ifdef MINI_TLS
    mbedtls_ssl_context *ssl;   /* NULL for plain http */
#endif
} NetIo;

static int net_recv(void *io_, char *buf, int cap)
{
    NetIo *io = (NetIo *)io_;
    for (int i = 0; i < 100000; i++) /* bound WANT retries so we never spin */
    {
#ifdef MINI_TLS
        if (io->ssl)
        {
            int r = mbedtls_ssl_read(io->ssl, (unsigned char *)buf, (size_t)cap);
            if (r > 0) return r;
            if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == 0) return 0;
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            return -2; /* fatal */
        }
#endif
#ifdef _WIN32
        int rn = recv(io->fd, buf, cap, 0);
        if (rn > 0) return rn;
        if (rn == 0) return 0;
        if (WSAGetLastError() == NET_EWOULD) continue;
        return -2;
#else
        ssize_t rn = recv(io->fd, buf, (size_t)cap, 0);
        if (rn > 0) return (int)rn;
        if (rn == 0) return 0;
        if (errno == NET_EWOULD) continue;
        return -2;
#endif
    }
    return -2; /* stall */
}

/* send_all over the same I/O handle (TLS or plain). */
static int net_send(NetIo *io, const char *buf, size_t len)
{
#ifdef MINI_TLS
    if (io->ssl)
    {
        size_t off = 0;
        while (off < len)
        {
            int n = mbedtls_ssl_write(io->ssl, (const unsigned char *)(buf + off), len - off);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (n <= 0) return -1;
            off += (size_t)n;
        }
        return 0;
    }
#endif
    return send_all_plain(io->fd, buf, len);
}

int mini_net_http(const char *method, const char *url,
                  const char *extra_headers, const char *body, size_t body_len,
                  MiniNetRecord *out)
{
    if (!out)
        return -1;
    memset(out, 0, sizeof *out);
    snprintf(out->method, sizeof out->method, "%s", method ? method : "GET");
    snprintf(out->url, sizeof out->url, "%s", url ? url : "");
    out->failed = 0;
    out->status = 0;

    mini_net_init();

    int is_https = 0;
    char host[256], path[1024];
    int port;
    if (parse_url(url, host, sizeof host, &port, path, sizeof path, &is_https) != 0)
    {
        out->failed = 1;
        return -1;
    }

    /* ---- Phase 7: keep-alive connection pool. Try to reuse a pooled TLS
       session for (host,port,is_https) so per-module TLS handshakes are
       amortised. If the pool is entirely busy we fall back to a one-off
       Connection: close request (the original behaviour): a boundary miss on
       the pooled path also degrades to read-until-close, so body integrity is
       never put at risk. Some origins (e.g. esm.sh/Cloudflare) 400 a
       keep-alive HTTP/1.1 request from a non-browser client; a 4xx/5xx on the
       pooled path is retried ONCE as a fresh Connection: close request so
       those origins still load. force_close gates that one retry. */
    int force_close = 0;
    int ka_reused = 0;
    KaConn *ka = NULL;
acquire_ka:
    if (!force_close)
        ka = ka_acquire(host, port, is_https, &ka_reused);
    int pooled = (ka != NULL);
    NetIo io; memset(&io, 0, sizeof io);
#ifdef MINI_TLS
    mbedtls_ssl_context ossl; mbedtls_ssl_config oconf; tls_bio_ctx obio;
    int ossl_init = 0;             /* one-off (non-pooled) TLS, freed at end */
#endif
    ByteBuf resp = {0};
    int reusable = 0;              /* may return a pooled conn to the pool */

    /* resolve + connect (+ TLS), into the pool slot or a one-off fd */
    {
        struct addrinfo hints, *res = NULL, *rp;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        char portstr[16];
        snprintf(portstr, sizeof portstr, "%d", port);
        if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        { out->failed = 1; goto conn_fail; }
        NET_SOCKET fd = NET_INVALID;
        for (rp = res; rp; rp = rp->ai_next)
        {
            fd = (NET_SOCKET)socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (fd == NET_INVALID) continue;
            if (connect(fd, rp->ai_addr, (socklen_t)rp->ai_addrlen) == 0) break;
            NET_CLOSE(fd); fd = NET_INVALID;
        }
        freeaddrinfo(res);
        if (fd == NET_INVALID) { out->failed = 1; goto conn_fail; }

        if (pooled)
        {
            ka->fd = fd;
            snprintf(ka->host, sizeof ka->host, "%s", host);
            ka->port = port; ka->is_https = is_https;
#ifdef MINI_TLS
            if (is_https)
            {
                ka->bio.fd = fd;
                if (tls_setup_and_handshake(&ka->ssl, &ka->conf, &ka->bio, fd, host) != 0)
                { out->failed = 1; goto conn_fail; }
                ka->has_ssl = 1;
            }
#else
            if (is_https)
            {
                fprintf(stderr, "[net] https://%s requested but MINI_TLS not built\n", host);
                out->failed = 1; goto conn_fail;
            }
#endif
            ka->valid = 1;
            io.fd = ka->fd;
#ifdef MINI_TLS
            if (is_https) io.ssl = &ka->ssl;
#endif
        }
        else
        {
            io.fd = fd;
#ifdef MINI_TLS
            if (is_https)
            {
                if (tls_setup_and_handshake(&ossl, &oconf, &obio, fd, host) != 0)
                { out->failed = 1; goto conn_fail; }
                ossl_init = 1; io.ssl = &ossl;
            }
#else
            if (is_https)
            {
                fprintf(stderr, "[net] https://%s requested but MINI_TLS not built\n", host);
                out->failed = 1; goto conn_fail;
            }
#endif
        }
    }

    /* build the request (keep-alive when pooled, close otherwise). */
    ByteBuf req = {0};
    bb_put(&req, method, strlen(method));
    bb_put(&req, " ", 1);
    bb_put(&req, path, strlen(path));
    bb_put(&req, " HTTP/1.1\r\nHost: ", 17);
    bb_put(&req, host, strlen(host));
    bb_put(&req, pooled ? "\r\nConnection: keep-alive\r\n"
                         : "\r\nConnection: close\r\n",
            pooled ? 27 : 21);
    /* A browser-like User-Agent: esm.sh/Cloudflare 400s keep-alive requests
       that carry no UA (bot heuristics). Also makes us a well-behaved client. */
    {
        static const char ua[] =
            "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
            "AppleWebKit/537.36 (KHTML, like Gecko) "
            "TinyFramework/1.0 Chrome/120 Safari/537.36\r\n";
        bb_put(&req, ua, strlen(ua));
        bb_put(&req, "Accept: */*\r\n", strlen("Accept: */*\r\n"));
        bb_put(&req, "Accept-Language: en-US,en;q=0.9\r\n",
                strlen("Accept-Language: en-US,en;q=0.9\r\n"));
    }
    if (body && body_len)
    {
        char cl[40];
        int n = snprintf(cl, sizeof cl, "Content-Length: %zu\r\n", body_len);
        bb_put(&req, cl, (size_t)n);
        if (extra_headers && strstr(extra_headers, "Content-Type") == NULL)
            bb_put(&req, "Content-Type: application/x-www-form-urlencoded\r\n", 49);
    }
    if (extra_headers && *extra_headers)
    {
        bb_put(&req, extra_headers, strlen(extra_headers));
        if (extra_headers[strlen(extra_headers) - 1] != '\n')
            bb_put(&req, "\r\n", 2);
    }
    bb_put(&req, "\r\n", 2);
    size_t head_len = req.len; /* request head ends at the blank line */
    if (body && body_len)
        bb_put(&req, body, body_len);

    /* send */
    if (net_send(&io, req.p, req.len) != 0)
    { out->failed = 1; free(req.p); goto conn_fail; }
    /* keep a copy of just the request head (without the body) */
    out->req_headers = (char *)malloc(head_len + 1);
    if (out->req_headers)
    {
        memcpy(out->req_headers, req.p, head_len);
        out->req_headers[head_len] = 0;
    }
    free(req.p); /* request buffer no longer needed */
    out->req_body = (body && body_len) ? (char *)malloc(body_len) : NULL;
    if (out->req_body)
    {
        memcpy(out->req_body, body, body_len);
        out->req_body_len = body_len;
    }

    /* read response. Pooled: header-aware (Content-Length / chunked) so the
       socket can go back to the pool; one-off: read until close (original). */
    if (pooled)
    {
        size_t hdr_off = 0;
        int hr = read_until_headers(net_recv, &io, &resp, &hdr_off);
        if (hr == 0)
        {
            long cl = -1; int chunked = 0, close_it = 0;
            mini_net_parse_response_meta(resp.p, hdr_off, &cl, &chunked, &close_it);
            size_t body_have = resp.len - hdr_off;
            /* status code (for the no-body rule) — "HTTP/1.1 NNN " */
            int rstatus = 0;
            { const char *sp = strchr(resp.p, ' '); if (sp) rstatus = atoi(sp + 1); }
            /* 1xx/204/304 and HEAD responses have NO body: stop at the header
               boundary and keep the connection reusable. Without this a 304
               (the conditional-revalidation path!) over a keep-alive socket
               would block forever in read_until_close waiting for a body that
               never comes. */
            int no_body = (rstatus >= 100 && rstatus < 200) || rstatus == 204 ||
                          rstatus == 304 || cl == 0 ||
                          (method && !strcasecmp(method, "HEAD"));
            if (no_body)
            {
                /* nothing to read; whatever bytes are already past hdr_off are
                   the start of the next pipelined response — leave them. */
                /* (we don't support pipelining; truncate resp at hdr_off) */
                if (resp.len > hdr_off) resp.len = hdr_off;
            }
            else if (chunked)
            {
                /* keep reading until the terminating 0-chunk; dechunked later
                   by the shared post-processing (which already handles chunked). */
                read_chunked(net_recv, &io, &resp);
            }
            else if (cl >= 0)
            {
                if (body_have < (size_t)cl &&
                    read_exact(net_recv, &io, &resp, body_have, (size_t)cl) != 0)
                    close_it = 1; /* premature EOF: keep what we have, no reuse */
            }
            else
            {
                /* no boundary (HTTP/1.0 / non-conformant server): can't reuse
                   — read the rest until the peer closes. */
                close_it = 1;
                read_until_close(net_recv, &io, &resp);
            }
            reusable = !close_it;
        }
        else
        {
            /* headers incomplete / peer closed early: salvage what we have. */
            if (hr == 1) read_until_close(net_recv, &io, &resp);
            reusable = 0;
        }
    }
    else
    {
        /* one-off Connection: close — read until the peer closes (original). */
        read_until_close(net_recv, &io, &resp);
    }

    /* release / close the connection now that the response is fully in `resp`. */
    if (pooled)
        ka_release(ka, reusable);
    else
    {
#ifdef MINI_TLS
        if (ossl_init)
        {
            mbedtls_ssl_close_notify(&ossl);
            mbedtls_ssl_free(&ossl);
            mbedtls_ssl_config_free(&oconf);
        }
#endif
        if (io.fd != NET_INVALID)
            NET_CLOSE(io.fd);
    }

    if (!resp.p || resp.len == 0)
    {
        free(resp.p);
        out->failed = 1;
        return -1;
    }

    /* split head/body at \r\n\r\n */
    char *body_start = strstr(resp.p, "\r\n\r\n");
    size_t resp_head_len;
    if (body_start)
    {
        resp_head_len = (size_t)(body_start - resp.p);
        body_start += 4;
    }
    else
    {
        resp_head_len = resp.len;
        body_start = resp.p + resp.len;
    }
    out->resp_headers = (char *)malloc(resp_head_len + 1);
    if (out->resp_headers)
    {
        memcpy(out->resp_headers, resp.p, resp_head_len);
        out->resp_headers[resp_head_len] = 0;
    }
    size_t body_len_resp = (size_t)((resp.p + resp.len) - body_start);
    /* if Content-Length present, trust it for the body length (harmless on
       the pooled path where the reader already bounded the body exactly). */
    long cl = -1;
    if (out->resp_headers)
    {
        const char *clh = strstr(out->resp_headers, "Content-Length:");
        if (!clh)
            clh = strstr(out->resp_headers, "content-length:");
        if (clh)
            cl = atol(clh + 15);
    }
    if (cl >= 0 && (size_t)cl <= body_len_resp)
        body_len_resp = (size_t)cl;
    out->resp_body = (char *)malloc(body_len_resp + 1);
    if (out->resp_body)
    {
        memcpy(out->resp_body, body_start, body_len_resp);
        out->resp_body[body_len_resp] = 0;
    }
    out->resp_body_len = body_len_resp;

    /* de-chunk if Transfer-Encoding: chunked (HTTP headers are
       case-insensitive; cdnjs & most CDNs ship JS chunked — without this the
       raw chunk frames e.g. "1d41\r\n" sit in the body and break JS eval). */
    if (out->resp_headers && out->resp_body &&
        stristr(out->resp_headers, "chunked"))
    {
        out->resp_body_len = dechunk(out->resp_body, out->resp_body_len);
    }

    /* status code + mime */
    if (out->resp_headers)
    {
        /* "HTTP/1.1 200 OK" */
        const char *sp = strchr(out->resp_headers, ' ');
        if (sp)
            out->status = atoi(sp + 1);
        const char *ct = strstr(out->resp_headers, "Content-Type:");
        if (!ct)
            ct = strstr(out->resp_headers, "content-type:");
        if (ct)
        {
            ct += 13;
            while (*ct == ' ' || *ct == '\t')
                ct++;
            size_t m = 0;
            while (*ct && *ct != '\r' && *ct != '\n' && *ct != ';' && m + 1 < sizeof out->mime)
                out->mime[m++] = *ct++;
            out->mime[m] = 0;
        }
    }
    if (out->mime[0] == 0)
        snprintf(out->mime, sizeof out->mime, "text/plain");

    /* Graceful degradation: some origins (esm.sh/Cloudflare) reject keep-alive
       HTTP/1.1 requests from non-browser clients with a 4xx. Retry ONCE on a
       fresh Connection: close socket, which those origins accept. Only for
       idempotent bodyless GETs; force_close prevents a second retry. */
    if (pooled && !force_close && out->status >= 400 && out->status < 600 &&
        (body == NULL || body_len == 0))
    {
        free(out->resp_headers); out->resp_headers = NULL;
        free(out->resp_body);    out->resp_body = NULL; out->resp_body_len = 0;
        free(out->req_headers);  out->req_headers = NULL;
        free(out->req_body);     out->req_body = NULL;
        free(resp.p);
        memset(&resp, 0, sizeof resp);
        out->status = 0; out->mime[0] = 0;
        force_close = 1; ka = NULL;
        goto acquire_ka;
    }

    free(resp.p);
    out->failed = 0;
    return 0;

conn_fail:
    /* connection / send failed: release/close the connection, free buffers. */
    if (pooled)
        ka_release(ka, 0);
    else
    {
#ifdef MINI_TLS
        if (ossl_init)
        {
            mbedtls_ssl_free(&ossl);
            mbedtls_ssl_config_free(&oconf);
        }
#endif
        if (io.fd != NET_INVALID)
            NET_CLOSE(io.fd);
    }
    free(resp.p);
    out->failed = 1;
    return -1;
}

/* ================================================================== */
/* Phase 6: fetch orchestration (HSTS + cookies + cache + CORS)        */
/* ================================================================== */
static int header_value(const char *hdrs, const char *name, char *out, size_t cap)
{
    if (!hdrs || !name) return 0;
    size_t nl = strlen(name);
    const char *p = hdrs;
    while (*p)
    {
        const char *line = p;
        const char *eol = strstr(p, "\r\n");
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        if (llen > nl && !strncasecmp(line, name, nl) && line[nl] == ':')
        {
            const char *v = line + nl + 1;
            while (v < line + llen && (*v == ' ' || *v == '\t')) v++;
            size_t vl = (size_t)((line + llen) - v);
            if (vl >= cap) vl = cap - 1;
            memcpy(out, v, vl); out[vl] = 0;
            return 1;
        }
        if (!eol) break;
        p = eol + 2;
    }
    return 0;
}

static void capture_set_cookies(const char *host, const char *path, int is_https, const char *hdrs)
{
    if (!hdrs) return;
    const char *p = hdrs;
    while ((p = stristr(p, "Set-Cookie:")) != NULL)
    {
        p += 11;
        while (*p == ' ' || *p == '\t') p++;
        char line[1024]; size_t i = 0;
        while (*p && *p != '\r' && *p != '\n' && i + 1 < sizeof line) line[i++] = *p++;
        line[i] = 0;
        if (i) mini_cookies_set_from_header(host, path, is_https, line);
        if (*p == '\r') p++;
        if (*p == '\n') p++;
    }
}

static void serve_from_cache(MiniNetRecord *out, const char *method, const char *url, const MiniCacheEntry *ce)
{
    memset(out, 0, sizeof *out);
    snprintf(out->method, sizeof out->method, "%s", method ? method : "GET");
    snprintf(out->url, sizeof out->url, "%s", url ? url : "");
    out->status = ce->status;
    snprintf(out->mime, sizeof out->mime, "%s", ce->mime);
    out->resp_body = (char *)malloc(ce->body_len + 1);
    if (out->resp_body)
    {
        memcpy(out->resp_body, ce->body, ce->body_len);
        out->resp_body[ce->body_len] = 0;
        out->resp_body_len = ce->body_len;
    }
    out->resp_headers = (char *)malloc(256);
    if (out->resp_headers)
        snprintf(out->resp_headers, 256, "HTTP/1.1 %d OK (from cache)\r\nContent-Type: %s\r\n\r\n",
                 ce->status, ce->mime);
    out->failed = 0;
    MINI_LOGD("net.fetch", "cache hit %s", url);
}

int mini_net_fetch(const char *method, const char *url,
                   const char *extra_headers, const char *body, size_t body_len,
                   const char *page_origin, MiniNetRecord *out)
{
    if (!out) return -1;
    mini_net_init();
    mini_cookies_init();
    mini_httpcache_init();

    MiniOrigin o;
    int parsed = (mini_origin_parse(url, &o) == 0);
    char eff_url[1024];
    snprintf(eff_url, sizeof eff_url, "%s", url ? url : "");
    if (parsed && !strcmp(o.scheme, "http") && mini_hsts_should_upgrade(o.host))
    {
        snprintf(eff_url, sizeof eff_url, "https://%s", url + 7);
        mini_origin_parse(eff_url, &o);
        parsed = 1;
    }
    int is_https = parsed && !strcmp(o.scheme, "https");

    /* extract the request path for cookie matching */
    char reqpath[1024] = "/";
    {
        const char *pp = strstr(eff_url, "://");
        if (pp)
        {
            pp += 3;
            const char *sl = strchr(pp, '/');
            if (sl) snprintf(reqpath, sizeof reqpath, "%s", sl);
        }
    }

    /* fresh cache hit -> serve without the network (GET only). Hold the cache
       lock across get()+serve_from_cache's copy so a background prefetch can't
       store/evict the entry and dangle the pointer (the mutex is recursive on
       both platforms, so get()'s internal lock re-enters safely). */
    int stale = 0;
    mini_httpcache_lock();
    const MiniCacheEntry *ce = mini_httpcache_get(eff_url, &stale);
    if (ce && !strcasecmp(method ? method : "GET", "GET"))
    {
        serve_from_cache(out, method, eff_url, ce);
        mini_httpcache_unlock();
        return 0;
    }
    mini_httpcache_unlock();

    /* merged headers: Cookie + conditional revalidators + caller headers */
    char cookie[2048];
    int has_cookie = mini_cookies_build_header(o.host, reqpath, is_https, 0, cookie, sizeof cookie);
    char cond[512]; cond[0] = 0;
    if (stale) mini_httpcache_conditional_headers(eff_url, cond, sizeof cond);

    char *hdrs = NULL; size_t hlen = 0, hcap = 0;
#define HAP(s) do { size_t l = strlen(s); if (hlen + l + 1 > hcap) { while (hcap < hlen + l + 1) hcap = hcap ? hcap * 2 : 256; hdrs = (char *)realloc(hdrs, hcap); } if (hdrs) { memcpy(hdrs + hlen, s, l); hlen += l; hdrs[hlen] = 0; } } while (0)
    if (has_cookie > 0) { HAP("Cookie: "); HAP(cookie); HAP("\r\n"); }
    if (cond[0]) HAP(cond);
    if (extra_headers && *extra_headers) { HAP(extra_headers); if (extra_headers[strlen(extra_headers) - 1] != '\n') HAP("\r\n"); }
#undef HAP

    int rc = mini_net_http(method, eff_url, hdrs, body, body_len, out);
    free(hdrs);
    if (rc != 0) return rc;

    /* 304 Not Modified -> serve the stale cached body. Lock across the copy so
       a concurrent prefetch store/evict can't free the entry's body mid-copy. */
    if (out->status == 304)
    {
        mini_httpcache_lock();
        const MiniCacheEntry *se = mini_httpcache_get_any(eff_url);
        if (se)
        {
            free(out->resp_body);
            out->resp_body = (char *)malloc(se->body_len + 1);
            if (out->resp_body)
            {
                memcpy(out->resp_body, se->body, se->body_len);
                out->resp_body[se->body_len] = 0;
                out->resp_body_len = se->body_len;
            }
            out->status = se->status;
            snprintf(out->mime, sizeof out->mime, "%s", se->mime);
            MINI_LOGD("net.fetch", "304 -> cached body %s", eff_url);
        }
        mini_httpcache_unlock();
    }

    /* Set-Cookie capture, HSTS remember, CORS gate, cache store */
    if (out->resp_headers)
    {
        capture_set_cookies(o.host, reqpath, is_https, out->resp_headers);
        char sts[256];
        if (header_value(out->resp_headers, "Strict-Transport-Security", sts, sizeof sts))
            mini_hsts_remember(o.host, sts);

        MiniOrigin po;
        int poparsed = (page_origin && mini_origin_parse(page_origin, &po) == 0);
        if (poparsed && !mini_origin_same(&po, &o))
        {
            char acao[256];
            int got = header_value(out->resp_headers, "Access-Control-Allow-Origin", acao, sizeof acao);
            if (!got || !mini_cors_response_allowed(&po, acao, 0, has_cookie > 0))
            {
                MINI_LOGW("net.cors", "blocked cross-origin %s for %s", eff_url, page_origin);
                out->failed = 1;
                out->status = 0;
            }
        }

        if (!strcasecmp(method ? method : "GET", "GET") && out->status >= 200 && out->status < 300)
        {
            char etag[256] = {0}, lm[128] = {0}, cc[256] = {0}, exp[128] = {0};
            header_value(out->resp_headers, "ETag", etag, sizeof etag);
            header_value(out->resp_headers, "Last-Modified", lm, sizeof lm);
            header_value(out->resp_headers, "Cache-Control", cc, sizeof cc);
            mini_httpcache_store(eff_url, out->status, out->mime,
                                 out->resp_body ? out->resp_body : "", out->resp_body_len,
                                 etag, lm, cc, exp);
        }
    }
    return rc;
}

/* ================================================================== */
/* Phase 7: parallel prefetch pool. Background threads warm the HTTP cache */
/* for URLs discovered by parse_importmap. The module loader calls         */
/* mini_net_prefetch_await(url) before mini_net_fetch() so the (already-  */
/* finished) prefetch is a cache hit instead of a live TLS handshake.      */
/* Thread fn only touches the (mutex-protected) HTTP cache + the net     */
/* layer (keep-alive pool + TLS, both guarded); it never touches the JS   */
/* runtime or the recording table, so it is safe to run concurrently.     */
/* ================================================================== */

/* portable thread handle + create/join (defined per-platform below). */
#if defined(_WIN32)
#include <process.h>
typedef HANDLE pf_thread_t;
#else
#include <pthread.h>
typedef pthread_t pf_thread_t;
#endif

typedef struct {
    char url[1024];
    int  valid;   /* slot in use (thread live or awaiting join) */
} PrefetchSlot;

#define PF_CAP 64
static PrefetchSlot g_pf[PF_CAP];
static pf_thread_t  g_pf_th[PF_CAP];
static net_mutex_t  g_pf_lock;
static int g_pf_lock_inited = 0;
static void pf_lock_ensure(void)
{
    if (!g_pf_lock_inited) { nmtx_init(&g_pf_lock); g_pf_lock_inited = 1; }
}

/* Worker body: fetch the URL via the keep-alive path and store the result
   into the HTTP cache (mirroring mini_net_fetch's store step). header_value
   is file-scope above; mini_net_record_free frees the temp record (we never
   call mini_net_record_add, so no race on the CDP ring). */
static void pf_do(PrefetchSlot *s)
{
    MiniNetRecord rec;
    memset(&rec, 0, sizeof rec);
    if (mini_net_http("GET", s->url, NULL, NULL, 0, &rec) == 0 &&
        rec.resp_headers && rec.resp_body && rec.status >= 200 && rec.status < 300)
    {
        char etag[256] = {0}, lm[128] = {0}, cc[256] = {0}, exp[128] = {0};
        header_value(rec.resp_headers, "ETag", etag, sizeof etag);
        header_value(rec.resp_headers, "Last-Modified", lm, sizeof lm);
        header_value(rec.resp_headers, "Cache-Control", cc, sizeof cc);
            mini_httpcache_store(s->url, rec.status, rec.mime,
                                 rec.resp_body, rec.resp_body_len,
                                 etag, lm, cc, exp);
    }
    mini_net_record_free(&rec);
}

#if defined(_WIN32)
static unsigned __stdcall pf_thread_main(void *arg) { pf_do((PrefetchSlot *)arg); return 0; }
static int pf_create(pf_thread_t *out, PrefetchSlot *s)
{
    *out = (pf_thread_t)_beginthreadex(NULL, 0, pf_thread_main, s, 0, NULL);
    return *out ? 0 : -1;
}
static void pf_join(pf_thread_t t)
{
    if (t) { WaitForSingleObject((HANDLE)t, INFINITE); CloseHandle((HANDLE)t); }
}
#else
static void *pf_thread_main(void *arg) { pf_do((PrefetchSlot *)arg); return NULL; }
static int pf_create(pf_thread_t *out, PrefetchSlot *s)
{
    return pthread_create(out, NULL, pf_thread_main, s) == 0 ? 0 : -1;
}
static void pf_join(pf_thread_t t)
{
    if (t) pthread_join(t, NULL);
}
#endif

/* Parallel prefetch is OFF by default: empirical testing against esm.sh
   (the CDN both target pages import from) showed Cloudflare THROTTLES
   concurrent connections from one IP, so firing N prefetch threads makes
   module loading ~3x SLOWER, not faster (gl-matrix: 9s with prefetch vs 3s
   without). For CDNs that don't throttle concurrency this is a real win,
   so the pool + worker stay compiled and correct; flip this to 1 to enable
   per-host. mini_net_prefetch_await() is a cheap no-op when nothing is in
   flight, so leaving the await calls in the loader is free. */
static int g_prefetch_enabled = 0;

void mini_net_prefetch(const char *url)
{
    if (!g_prefetch_enabled)
        return;
    if (!url || !*url)
        return;
    /* only prefetch http(s); file:// is handled synchronously by the loader */
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
        return;
    pf_lock_ensure();
    nmtx_lock(&g_pf_lock);
    /* dedup: an entry for this URL already in flight -> nothing to do */
    for (int i = 0; i < PF_CAP; i++)
        if (g_pf[i].valid && !strcmp(g_pf[i].url, url))
        { nmtx_unlock(&g_pf_lock); return; }
    /* take the first free slot */
    int slot = -1;
    for (int i = 0; i < PF_CAP; i++)
        if (!g_pf[i].valid) { slot = i; break; }
    if (slot < 0) { nmtx_unlock(&g_pf_lock); return; } /* pool busy: skip */
    PrefetchSlot *s = &g_pf[slot];
    snprintf(s->url, sizeof s->url, "%s", url);
    s->valid = 1;
    pf_thread_t th;
    if (pf_create(&th, s) != 0)
    {
        s->valid = 0;
        nmtx_unlock(&g_pf_lock);
        return;
    }
    g_pf_th[slot] = th;
    nmtx_unlock(&g_pf_lock);
}

void mini_net_prefetch_await(const char *url)
{
    if (!url) return;
    pf_lock_ensure();
    nmtx_lock(&g_pf_lock);
    pf_thread_t th;
    int found = 0;
    for (int i = 0; i < PF_CAP; i++)
    {
        if (g_pf[i].valid && !strcmp(g_pf[i].url, url))
        {
            th = g_pf_th[i];
            found = 1;
            break;
        }
    }
    nmtx_unlock(&g_pf_lock);
    if (!found) return;
    /* Join outside the lock (the worker never takes g_pf_lock, so no deadlock).
       Once joined the cache is warm -> the caller's mini_net_fetch() hits. */
    pf_join(th);
    nmtx_lock(&g_pf_lock);
    for (int i = 0; i < PF_CAP; i++)
        if (g_pf[i].valid && !strcmp(g_pf[i].url, url))
        { g_pf[i].valid = 0; g_pf_th[i] = (pf_thread_t)0; break; }
    nmtx_unlock(&g_pf_lock);
}

void mini_net_prefetch_shutdown(void)
{
    pf_lock_ensure();
    /* join every live prefetch so their cache writes land before teardown */
    nmtx_lock(&g_pf_lock);
    pf_thread_t tojoin[PF_CAP];
    int n = 0;
    for (int i = 0; i < PF_CAP; i++)
    {
        if (g_pf[i].valid)
        {
            tojoin[n++] = g_pf_th[i];
            g_pf[i].valid = 0;
            g_pf_th[i] = (pf_thread_t)0;
        }
    }
    nmtx_unlock(&g_pf_lock);
    for (int i = 0; i < n; i++)
        pf_join(tojoin[i]);
    /* tear down the keep-alive pool too: free mbedtls sessions + close fds */
    ka_shutdown_all();
}


/* ================================================================== */
#ifdef NETFETCH_SELFTEST
#include "mini_log.h"
static int nf = 0;
#define NCK(c, m) do { if (!(c)) { fprintf(stderr, "NF FAIL: %s\n", m); nf++; } } while (0)

int main(void)
{
    mini_log_init();
    mini_cookies_init();
    mini_httpcache_init();
    mini_httpcache_clear();

    /* seed a fresh cache entry, then fetch -> must be served from cache with
       NO network round-trip (mini_net_http is never called). */
    mini_httpcache_store("http://x.test/data", 200, "application/json",
                         "{\"ok\":1}", 8, "\"v1\"", "",
                         "public, max-age=3600", "");
    MiniNetRecord rec;
    int rc = mini_net_fetch("GET", "http://x.test/data", NULL, NULL, 0, NULL, &rec);
    NCK(rc == 0, "fetch cache hit rc");
    NCK(rec.failed == 0, "not failed");
    NCK(rec.status == 200, "status 200");
    NCK(rec.resp_body && !strcmp(rec.resp_body, "{\"ok\":1}"), "cached body served");
    NCK(rec.resp_body_len == 8, "body length");
    NCK(!strcmp(rec.mime, "application/json"), "mime");
    mini_net_record_free(&rec);

    /* keep-alive response-meta parser: the risky part of Phase 7 (deciding a
       body boundary so a socket can go back to the pool). Pure, no I/O. */
    {
        const char *h = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                         "Content-Length: 5\r\nConnection: keep-alive\r\n\r\n";
        long cl; int ch = 0, cl2 = 0;
        mini_net_parse_response_meta(h, strlen(h), &cl, &ch, &cl2);
        NCK(cl == 5, "parse Content-Length=5");
        NCK(ch == 0, "parse not chunked");
        NCK(cl2 == 0, "parse keep-alive (not close)");
    }
    {
        const char *h = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                         "Connection: close\r\n\r\n";
        long cl; int ch = 0, cl2 = 0;
        mini_net_parse_response_meta(h, strlen(h), &cl, &ch, &cl2);
        NCK(ch == 1, "parse chunked");
        NCK(cl2 == 1, "parse Connection: close");
        NCK(cl == -1, "parse no Content-Length");
    }

    fprintf(stderr, nf ? "NETFETCH_SELFTEST: %d FAIL\n" : "NETFETCH_SELFTEST: all PASS\n", nf);
    return nf ? 1 : 0;
}
#endif
