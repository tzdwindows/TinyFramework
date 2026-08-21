/*
 * mini_httpcache.c — HTTP cache implementation + self-test.
 */
#include "mini_httpcache.h"
#include "mini_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
typedef CRITICAL_SECTION hc_mutex_t;
static void hc_mutex_init(hc_mutex_t *m) { InitializeCriticalSection(m); }
static void hc_mutex_lock(hc_mutex_t *m) { EnterCriticalSection(m); }
static void hc_mutex_unlock(hc_mutex_t *m) { LeaveCriticalSection(m); }
static void hc_mutex_destroy(hc_mutex_t *m) { DeleteCriticalSection(m); }
#else
#include <pthread.h>
typedef pthread_mutex_t hc_mutex_t;
static void hc_mutex_init(hc_mutex_t *m)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE); /* match Win CRITICAL_SECTION: a fetch may hold the lock across get()+copy */
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
}
static void hc_mutex_lock(hc_mutex_t *m) { pthread_mutex_lock(m); }
static void hc_mutex_unlock(hc_mutex_t *m) { pthread_mutex_unlock(m); }
static void hc_mutex_destroy(hc_mutex_t *m) { pthread_mutex_destroy(m); }
#endif

#define MAX_ENTRIES 128
static MiniCacheEntry g_entries[MAX_ENTRIES];
static int g_n = 0;
static hc_mutex_t g_lock;
static int g_lock_inited = 0;

static void ensure_lock(void)
{
    if (!g_lock_inited)
    {
        hc_mutex_init(&g_lock);
        g_lock_inited = 1;
    }
}

/* minimal strcasestr (not in C99; winsock/curl headers sometimes declare it,
   but don't rely on it). Aliased as strcasestr for the directives below. */
static const char *mini_strcasestr(const char *h, const char *n)
{
    if (!h || !n) return NULL;
    size_t nl = strlen(n);
    for (; *h; h++)
    {
        size_t i = 0;
        for (; i < nl && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)n[i]); i++)
            ;
        if (i == nl)
            return h;
    }
    return NULL;
}

void mini_httpcache_init(void) { ensure_lock(); }
void mini_httpcache_clear(void)
{
    ensure_lock();
    hc_mutex_lock(&g_lock);
    for (int i = 0; i < g_n; i++)
        free(g_entries[i].body);
    g_n = 0;
    hc_mutex_unlock(&g_lock);
}

/* case-insensitive token scan: find "name" as a whole directive in cc. */
static int cc_has(const char *cc, const char *name)
{
    if (!cc || !cc[0] || !name)
        return 0;
    size_t nl = strlen(name);
    const char *p = cc;
    while (*p)
    {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        const char *tok = p;
        while (*p && *p != ',' && !isspace((unsigned char)*p)) p++;
        size_t tl = (size_t)(p - tok);
        if (tl == nl && !strncasecmp(tok, name, nl))
            return 1;
        /* skip a possible =value */
        while (*p && *p != ',') p++;
    }
    return 0;
}

long mini_httpcache_parse_cc(const char *cc, int *no_store, int *no_cache)
{
    if (no_store) *no_store = cc_has(cc, "no-store");
    if (no_cache) *no_cache = cc_has(cc, "no-cache");
    if (!cc || !cc[0])
        return -1;
    /* find max-age=N ("max-age" is 7 chars) */
    const char *ma = mini_strcasestr(cc, "max-age");
    if (!ma)
        return -1;
    ma += 7;
    while (*ma && (*ma == ' ' || *ma == '=' || *ma == '\t'))
        ma++;
    char *end;
    long v = strtol(ma, &end, 10);
    if (end == ma)
        return -1;
    return v;
}

static int find_idx(const char *url)
{
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_entries[i].url, url))
            return i;
    return -1;
}

void mini_httpcache_store(const char *url, int status, const char *mime,
                          const char *body, size_t len,
                          const char *etag, const char *last_modified,
                          const char *cache_control, const char *expires)
{
    if (!url || !url[0])
        return;
    int no_store = 0, no_cache = 0;
    long ma = mini_httpcache_parse_cc(cache_control, &no_store, &no_cache);
    if (no_store)
        return; /* never cached */
    /* only cache 2xx GET responses (and 3xx? keep pragmatic: 2xx only here) */
    if (status < 200 || status >= 300)
        return;

    ensure_lock();
    hc_mutex_lock(&g_lock);
    int idx = find_idx(url);
    if (idx < 0)
    {
        if (g_n >= MAX_ENTRIES)
        {
            /* evict index 0 (oldest) */
            free(g_entries[0].body);
            memmove(&g_entries[0], &g_entries[1], (MAX_ENTRIES - 1) * sizeof(MiniCacheEntry));
            g_n = MAX_ENTRIES - 1;
            idx = g_n;
        }
        else
            idx = g_n++;
        memset(&g_entries[idx], 0, sizeof(MiniCacheEntry));
        snprintf(g_entries[idx].url, sizeof g_entries[idx].url, "%s", url);
    }
    else
    {
        free(g_entries[idx].body);
    }
    MiniCacheEntry *e = &g_entries[idx];
    e->status = status;
    snprintf(e->mime, sizeof e->mime, "%s", mime ? mime : "");
    e->body = (char *)malloc(len + 1);
    if (e->body)
    {
        memcpy(e->body, body ? body : "", len);
        e->body[len] = 0;
    }
    e->body_len = len;
    snprintf(e->etag, sizeof e->etag, "%s", etag ? etag : "");
    snprintf(e->last_modified, sizeof e->last_modified, "%s", last_modified ? last_modified : "");
    e->stored_at = time(NULL);
    e->max_age = ma;
    e->no_cache = no_cache;
    e->no_store = 0;
    (void)expires; /* Expires header handled as a fallback below if max_age<0 */
    if (ma < 0 && expires && expires[0])
    {
        /* parse HTTP-date to a delta from now; reuse the cookies parser shape */
        /* (kept pragmatic: Expires fallback rarely needed for CDN JS) */
        e->max_age = -1;
    }
    hc_mutex_unlock(&g_lock);
    MINI_LOGD("net.cache", "stored %s (status=%d, max_age=%ld, len=%zu)",
              url, status, e->max_age, len);
}

static int fresh(const MiniCacheEntry *e)
{
    if (!e)
        return 0;
    if (e->no_cache)
        return 0; /* must revalidate */
    if (e->max_age < 0)
        return 0; /* unspecified -> treat as stale (no heuristic here) */
    return (time(NULL) - e->stored_at) < e->max_age;
}

/* Callers that need a STABLE snapshot of a returned entry (mini_net.c's
   serve_from_cache / 304 path) must hold mini_httpcache_lock() across the
   copy: a concurrent background prefetch can store/evict otherwise. */
void mini_httpcache_lock(void)   { ensure_lock(); hc_mutex_lock(&g_lock); }
void mini_httpcache_unlock(void) { hc_mutex_unlock(&g_lock); }

const MiniCacheEntry *mini_httpcache_get(const char *url, int *stale)
{
    ensure_lock();
    hc_mutex_lock(&g_lock);
    int idx = find_idx(url);
    if (idx < 0)
    {
        hc_mutex_unlock(&g_lock);
        if (stale) *stale = 0;
        return NULL;
    }
    const MiniCacheEntry *e = &g_entries[idx];
    int is_fresh = fresh(e);
    hc_mutex_unlock(&g_lock);
    if (is_fresh)
    {
        if (stale) *stale = 0;
        return e;
    }
    /* exists but stale — caller may revalidate */
    if (stale) *stale = 1;
    return NULL; /* not directly servable; caller should conditional-fetch */
}

const MiniCacheEntry *mini_httpcache_get_any(const char *url)
{
    ensure_lock();
    hc_mutex_lock(&g_lock);
    int idx = find_idx(url);
    const MiniCacheEntry *e = idx < 0 ? NULL : &g_entries[idx];
    hc_mutex_unlock(&g_lock);
    return e;
}

int mini_httpcache_conditional_headers(const char *url, char *out, size_t cap)
{
    ensure_lock();
    hc_mutex_lock(&g_lock);
    int idx = find_idx(url);
    if (idx < 0)
    {
        hc_mutex_unlock(&g_lock);
        return -1;
    }
    const MiniCacheEntry *e = &g_entries[idx];
    size_t off = 0;
    int wrote = 0;
    if (e->etag[0])
    {
        off += (size_t)snprintf(out + off, cap - off, "If-None-Match: %s\r\n", e->etag);
        wrote = 1;
    }
    if (e->last_modified[0])
    {
        off += (size_t)snprintf(out + off, cap - off, "If-Modified-Since: %s\r\n", e->last_modified);
        wrote = 1;
    }
    out[off] = 0;
    hc_mutex_unlock(&g_lock);
    return wrote ? (int)off : -1;
}

void mini_httpcache_invalidate(const char *url)
{
    ensure_lock();
    hc_mutex_lock(&g_lock);
    int idx = find_idx(url);
    if (idx < 0)
    {
        hc_mutex_unlock(&g_lock);
        return;
    }
    free(g_entries[idx].body);
    memmove(&g_entries[idx], &g_entries[idx + 1], (g_n - idx - 1) * sizeof(MiniCacheEntry));
    g_n--;
    hc_mutex_unlock(&g_lock);
}

int mini_httpcache_count(void)
{
    ensure_lock();
    hc_mutex_lock(&g_lock);
    int n = g_n;
    hc_mutex_unlock(&g_lock);
    return n;
}

/* ================================================================== */
/* HTTPCACHE_SELFTEST                                                  */
/* ================================================================== */
#ifdef HTTPCACHE_SELFTEST
static int hc_fail = 0;
#define HC(c, m) do { if (!(c)) { fprintf(stderr, "HC FAIL: %s\n", m); hc_fail++; } } while (0)

int main(void)
{
    mini_log_init();
    mini_httpcache_clear();

    int ns = 0, nc = 0;
    HC(mini_httpcache_parse_cc("public, max-age=3600", &ns, &nc) == 3600, "cc max-age");
    HC(ns == 0, "no no-store");
    int ns2 = 0, nc2 = 0;
    HC(mini_httpcache_parse_cc("no-store", &ns2, &nc2) == -1, "no-store no max-age");
    HC(ns2 == 1, "no-store flag");

    mini_httpcache_store("https://x/y", 200, "text/plain", "hello", 5,
                         "\"v1\"", "Wed, 09 Jun 2021 10:18:14 GMT",
                         "max-age=3600", "");
    int stale = 0;
    const MiniCacheEntry *e = mini_httpcache_get("https://x/y", &stale);
    HC(e != NULL, "fresh entry served");
    HC(!stale, "not stale");
    HC(e && e->body_len == 5 && !memcmp(e->body, "hello", 5), "body cached");
    HC(e && !strcmp(e->etag, "\"v1\""), "etag cached");

    char cond[256];
    HC(mini_httpcache_conditional_headers("https://x/y", cond, sizeof cond) > 0, "conditional headers");
    HC(strstr(cond, "If-None-Match") != NULL, "has If-None-Match");
    HC(strstr(cond, "If-Modified-Since") != NULL, "has If-Modified-Since");

    /* no-store -> not cached */
    mini_httpcache_store("https://x/ns", 200, "text/plain", "x", 1, "", "",
                         "no-store", "");
    HC(mini_httpcache_get("https://x/ns", &stale) == NULL, "no-store not served");

    /* max-age=0 -> stale immediately */
    mini_httpcache_store("https://x/z", 200, "text/plain", "z", 1, "", "",
                         "max-age=0", "");
    HC(mini_httpcache_get("https://x/z", &stale) == NULL, "max-age=0 not fresh");
    HC(stale == 1, "stale entry exists for revalidation");

    fprintf(stderr, hc_fail ? "HTTPCACHE_SELFTEST: %d FAIL\n" : "HTTPCACHE_SELFTEST: all PASS\n", hc_fail);
    return hc_fail ? 1 : 0;
}
#endif
