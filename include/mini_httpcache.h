/*
 * mini_httpcache.h — HTTP cache (ETag / Last-Modified / Cache-Control).
 *
 * Phase 6: a small per-URL cache that honors Cache-Control max-age (freshness),
 * no-store/no-cache, and keeps ETag + Last-Modified so the fetch path can send
 * If-None-Match / If-Modified-Since and serve a cached body on 304. The cache
 * is shared with the fetch() / mini_net_fetch orchestration. Pure C99.
 */
#ifndef MINI_HTTPCACHE_H
#define MINI_HTTPCACHE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MiniCacheEntry
{
    char    url[1024];
    int     status;
    char    mime[128];
    char   *body;          /* malloc'd */
    size_t  body_len;
    char    etag[256];     /* "" if none */
    char    last_modified[64];
    time_t  stored_at;     /* when we cached it */
    long    max_age;       /* seconds of freshness; -1 = unspecified */
    int     no_store;      /* never cached (entry absent) */
    int     no_cache;      /* must revalidate before serving */
    int     used;
} MiniCacheEntry;

void mini_httpcache_init(void);
void mini_httpcache_clear(void);

/* Hold the cache lock across a get()->copy snapshot so a concurrent
   background prefetch (mini_net_prefetch) cannot store/evict the entry and
   leave a returned const MiniCacheEntry* dangling. No re-entrancy: do not call
   mini_httpcache_store/get/... (they take the lock themselves) while holding. */
void mini_httpcache_lock(void);
void mini_httpcache_unlock(void);

/* Parse a Cache-Control header. Returns max_age seconds, or -1 if none.
 * *no_store / *no_cache set to 1 if present (may be NULL). */
long mini_httpcache_parse_cc(const char *cc, int *no_store, int *no_cache);

/* Store (or replace) a response body under url. etag/lm may be "".
 * cc and expires drive freshness. A no-store response is not cached. */
void mini_httpcache_store(const char *url, int status, const char *mime,
                          const char *body, size_t len,
                          const char *etag, const char *last_modified,
                          const char *cache_control, const char *expires);

/* Lookup a fresh, cacheable entry. *stale (if !NULL) is set 1 if an entry
 * exists but needs revalidation. Returns the entry or NULL. */
const MiniCacheEntry *mini_httpcache_get(const char *url, int *stale);
/* The raw entry regardless of freshness (NULL if absent). For serving the
 * cached body on a 304 Not Modified after revalidation. */
const MiniCacheEntry *mini_httpcache_get_any(const char *url);

/* Conditional request headers for an existing (possibly stale) entry: writes
 * "If-None-Match: \"x\"\r\n" and/or "If-Modified-Since: ...\r\n" into out.
 * Returns the length, or -1 if the entry has no validators. */
int  mini_httpcache_conditional_headers(const char *url, char *out, size_t cap);

void mini_httpcache_invalidate(const char *url);

int  mini_httpcache_count(void);

#ifdef __cplusplus
}
#endif
#endif /* MINI_HTTPCACHE_H */
