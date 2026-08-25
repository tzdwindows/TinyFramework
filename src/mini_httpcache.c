/*
 * mini_httpcache.c — HTTP persistent disk + RAM cache implementation.
 */
#include "mini_httpcache.h"
#include "mini_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
typedef CRITICAL_SECTION hc_mutex_t;
static void hc_mutex_init(hc_mutex_t *m) { InitializeCriticalSection(m); }
static void hc_mutex_lock(hc_mutex_t *m) { EnterCriticalSection(m); }
static void hc_mutex_unlock(hc_mutex_t *m) { LeaveCriticalSection(m); }
static void hc_mutex_destroy(hc_mutex_t *m) { DeleteCriticalSection(m); }
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_mutex_t hc_mutex_t;
static void hc_mutex_init(hc_mutex_t *m)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
}
static void hc_mutex_lock(hc_mutex_t *m) { pthread_mutex_lock(m); }
static void hc_mutex_unlock(hc_mutex_t *m) { pthread_mutex_unlock(m); }
static void hc_mutex_destroy(hc_mutex_t *m) { pthread_mutex_destroy(m); }
#endif

#define MAX_ENTRIES 512
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

/* 128-bit hash formatted as 32 hex characters for cross-platform disk storage */
static void url_to_filename(const char *url, char *out, size_t cap)
{
    uint64_t h1 = 0xcbf29ce484222325ULL;
    uint64_t h2 = 0x100000001b3ULL;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
    {
        h1 = (h1 ^ *p) * 0x100000001b3ULL;
        h2 = (h2 + *p) * 0xcbf29ce484222325ULL;
    }
    snprintf(out, cap, "%016llx%016llx", (unsigned long long)h1, (unsigned long long)h2);
}

static void mini_httpcache_get_dir(char *out, size_t cap)
{
#if defined(_WIN32)
    const char *appdata = getenv("LOCALAPPDATA");
    if (!appdata || !appdata[0])
        appdata = getenv("APPDATA");
    if (appdata && appdata[0])
    {
        char tf_dir[512];
        snprintf(tf_dir, sizeof(tf_dir), "%s\\TinyFramework", appdata);
        CreateDirectoryA(tf_dir, NULL);
        snprintf(out, cap, "%s\\TinyFramework\\cache", appdata);
        CreateDirectoryA(out, NULL);
        return;
    }
#endif
    const char *home = getenv("HOME");
    if (home && home[0])
    {
        snprintf(out, cap, "%s/.tiny_cache", home);
    }
    else
    {
        snprintf(out, cap, ".tiny_cache");
    }
#if defined(_WIN32)
    CreateDirectoryA(out, NULL);
#else
    mkdir(out, 0755);
#endif
}

static void mini_httpcache_save_to_disk(const MiniCacheEntry *e)
{
    if (!e || !e->url[0]) return;
    char dir[512];
    mini_httpcache_get_dir(dir, sizeof dir);
    char hash[64];
    url_to_filename(e->url, hash, sizeof hash);

    char meta_path[600], bin_path[600];
    snprintf(meta_path, sizeof meta_path, "%s\\%s.meta", dir, hash);
    snprintf(bin_path, sizeof bin_path, "%s\\%s.bin", dir, hash);

    FILE *fmeta = fopen(meta_path, "w");
    if (fmeta)
    {
        fprintf(fmeta, "URL: %s\n", e->url);
        fprintf(fmeta, "STATUS: %d\n", e->status);
        fprintf(fmeta, "MIME: %s\n", e->mime);
        fprintf(fmeta, "ETAG: %s\n", e->etag);
        fprintf(fmeta, "LAST_MODIFIED: %s\n", e->last_modified);
        fprintf(fmeta, "STORED_AT: %lld\n", (long long)e->stored_at);
        fprintf(fmeta, "MAX_AGE: %ld\n", e->max_age);
        fprintf(fmeta, "NO_CACHE: %d\n", e->no_cache);
        fprintf(fmeta, "NO_STORE: %d\n", e->no_store);
        fprintf(fmeta, "BODY_LEN: %zu\n", e->body_len);
        fclose(fmeta);
    }

    if (e->body && e->body_len > 0)
    {
        FILE *fbin = fopen(bin_path, "wb");
        if (fbin)
        {
            fwrite(e->body, 1, e->body_len, fbin);
            fclose(fbin);
        }
    }
}

static int mini_httpcache_load_from_disk(const char *url, MiniCacheEntry *out_entry)
{
    if (!url || !url[0] || !out_entry) return 0;
    char dir[512];
    mini_httpcache_get_dir(dir, sizeof dir);
    char hash[64];
    url_to_filename(url, hash, sizeof hash);

    char meta_path[600], bin_path[600];
    snprintf(meta_path, sizeof meta_path, "%s\\%s.meta", dir, hash);
    snprintf(bin_path, sizeof bin_path, "%s\\%s.bin", dir, hash);

    FILE *fmeta = fopen(meta_path, "r");
    if (!fmeta) return 0;

    memset(out_entry, 0, sizeof(MiniCacheEntry));
    char line[1024];
    size_t body_len = 0;
    while (fgets(line, sizeof line, fmeta))
    {
        char *nl = strchr(line, '\r'); if (nl) *nl = 0;
        nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!strncmp(line, "URL: ", 5))
            snprintf(out_entry->url, sizeof out_entry->url, "%s", line + 5);
        else if (!strncmp(line, "STATUS: ", 8))
            out_entry->status = atoi(line + 8);
        else if (!strncmp(line, "MIME: ", 6))
            snprintf(out_entry->mime, sizeof out_entry->mime, "%s", line + 6);
        else if (!strncmp(line, "ETAG: ", 6))
            snprintf(out_entry->etag, sizeof out_entry->etag, "%s", line + 6);
        else if (!strncmp(line, "LAST_MODIFIED: ", 15))
            snprintf(out_entry->last_modified, sizeof out_entry->last_modified, "%s", line + 15);
        else if (!strncmp(line, "STORED_AT: ", 11))
            out_entry->stored_at = (time_t)atoll(line + 11);
        else if (!strncmp(line, "MAX_AGE: ", 9))
            out_entry->max_age = atol(line + 9);
        else if (!strncmp(line, "NO_CACHE: ", 10))
            out_entry->no_cache = atoi(line + 10);
        else if (!strncmp(line, "NO_STORE: ", 10))
            out_entry->no_store = atoi(line + 10);
        else if (!strncmp(line, "BODY_LEN: ", 10))
            body_len = (size_t)atoll(line + 10);
    }
    fclose(fmeta);

    if (strcmp(out_entry->url, url) != 0)
    {
        return 0;
    }

    if (body_len > 0)
    {
        FILE *fbin = fopen(bin_path, "rb");
        if (!fbin) return 0;
        out_entry->body = (char *)malloc(body_len + 1);
        if (!out_entry->body)
        {
            fclose(fbin);
            return 0;
        }
        size_t read_bytes = fread(out_entry->body, 1, body_len, fbin);
        out_entry->body[read_bytes] = 0;
        out_entry->body_len = read_bytes;
        fclose(fbin);
    }
    else
    {
        out_entry->body = (char *)calloc(1, 1);
        out_entry->body_len = 0;
    }

    return 1;
}

static void mini_httpcache_delete_from_disk(const char *url)
{
    if (!url || !url[0]) return;
    char dir[512];
    mini_httpcache_get_dir(dir, sizeof dir);
    char hash[64];
    url_to_filename(url, hash, sizeof hash);

    char meta_path[600], bin_path[600];
    snprintf(meta_path, sizeof meta_path, "%s\\%s.meta", dir, hash);
    snprintf(bin_path, sizeof bin_path, "%s\\%s.bin", dir, hash);

    remove(meta_path);
    remove(bin_path);
}

/* minimal strcasestr */
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

    /* Check disk cache */
    MiniCacheEntry disk_e;
    if (mini_httpcache_load_from_disk(url, &disk_e))
    {
        int idx = -1;
        if (g_n >= MAX_ENTRIES)
        {
            free(g_entries[0].body);
            memmove(&g_entries[0], &g_entries[1], (MAX_ENTRIES - 1) * sizeof(MiniCacheEntry));
            g_n = MAX_ENTRIES - 1;
            idx = g_n;
        }
        else
        {
            idx = g_n++;
        }
        g_entries[idx] = disk_e;
        return idx;
    }

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
    {
        mini_httpcache_delete_from_disk(url);
        return;
    }
    if (status < 200 || status >= 300)
        return;

    /* Heuristic freshness for static web resources if no max-age was provided */
    if (ma < 0 && !no_cache)
    {
        if (strstr(url, "unpkg.com") || strstr(url, "cdnjs.cloudflare.com") ||
            strstr(url, "cdn.jsdelivr.net") || strstr(url, "@") ||
            strstr(url, ".min.js") || strstr(url, ".js") ||
            strstr(url, ".css") || strstr(url, ".ttf") ||
            strstr(url, ".woff") || strstr(url, ".woff2") ||
            strstr(url, ".png") || strstr(url, ".jpg"))
        {
            ma = 86400 * 30; /* 30 days */
        }
        else
        {
            ma = 86400; /* 24 hours */
        }
    }

    ensure_lock();
    hc_mutex_lock(&g_lock);
    int idx = -1;
    for (int i = 0; i < g_n; i++)
    {
        if (!strcmp(g_entries[i].url, url))
        {
            idx = i;
            break;
        }
    }

    if (idx < 0)
    {
        if (g_n >= MAX_ENTRIES)
        {
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
    (void)expires;

    /* Persist to disk */
    mini_httpcache_save_to_disk(e);

    hc_mutex_unlock(&g_lock);
    MINI_LOGD("net.cache", "stored %s (status=%d, max_age=%ld, len=%zu) to disk",
              url, status, e->max_age, len);
    fprintf(stderr, "[HTTP Cache STORE] %s (len=%zu, max_age=%ld s)\n", url, len, e->max_age);
}

static int fresh(const MiniCacheEntry *e)
{
    if (!e)
        return 0;
    if (e->no_cache)
        return 0;
    if (e->max_age < 0)
        return 0;
    return (time(NULL) - e->stored_at) < e->max_age;
}

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
        fprintf(stderr, "[HTTP Cache HIT] %s (len=%zu)\n", url, e->body_len);
        return e;
    }
    if (stale) *stale = 1;
    return NULL;
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
    mini_httpcache_delete_from_disk(url);
    for (int i = 0; i < g_n; i++)
    {
        if (!strcmp(g_entries[i].url, url))
        {
            free(g_entries[i].body);
            memmove(&g_entries[i], &g_entries[i + 1], (g_n - i - 1) * sizeof(MiniCacheEntry));
            g_n--;
            break;
        }
    }
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

    mini_httpcache_store("https://x/ns", 200, "text/plain", "x", 1, "", "",
                         "no-store", "");
    HC(mini_httpcache_get("https://x/ns", &stale) == NULL, "no-store not served");

    mini_httpcache_store("https://x/z", 200, "text/plain", "z", 1, "", "",
                         "max-age=0", "");
    HC(mini_httpcache_get("https://x/z", &stale) == NULL, "max-age=0 not fresh");
    HC(stale == 1, "stale entry exists for revalidation");

    fprintf(stderr, hc_fail ? "HTTPCACHE_SELFTEST: %d FAIL\n" : "HTTPCACHE_SELFTEST: all PASS\n", hc_fail);
    return hc_fail ? 1 : 0;
}
#endif
