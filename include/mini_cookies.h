/*
 * mini_cookies.h — RFC6265 cookie jar (Phase 6 network foundation).
 *
 * A real (if compact) cookie store: parses Set-Cookie, applies the domain /
 * path / secure / SameSite rules, expires on time, and emits a matching
 * Cookie header for a request. JS document.cookie read/write and the fetch
 * path both go through here. Pure C99, no deps (libc + mini_log).
 */
#ifndef MINI_COOKIES_H
#define MINI_COOKIES_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { MINI_SAME_NONE = 0, MINI_SAME_LAX = 1, MINI_SAME_STRICT = 2 } MiniSameSite;

typedef struct MiniCookie
{
    char  name[128];
    char  value[1024];
    char  domain[256];   /* canonicalized lowercased, no leading dot */
    char  path[512];
    time_t expires;      /* 0 = session (cleared on shutdown) */
    int   max_age;       /* parsed Max-Age (drives expires when present) */
    int   secure;        /* send only over https */
    int   http_only;     /* hidden from document.cookie */
    MiniSameSite same_site;
    int   used;          /* LRU eviction bookkeeping */
} MiniCookie;

void mini_cookies_init(void);
void mini_cookies_clear(void);

/* Parse one Set-Cookie header in the context of a response from `host`/`path`
 * (is_https gates the Secure attribute). Stores (or replaces by name+domain+
 * path) a cookie. Returns 0 on a valid cookie, -1 if rejected. */
int  mini_cookies_set_from_header(const char *host, const char *path,
                                 int is_https, const char *set_cookie);

/* Build a "name=val; name=val" Cookie header value for cookies matching
 * (host,path,is_https). `for_js` excludes HttpOnly cookies (document.cookie).
 * Writes into out (null-terminated); returns the length, or -1 if none. */
int  mini_cookies_build_header(const char *host, const char *path,
                               int is_https, int for_js, char *out, size_t cap);

/* document.cookie setter: parse "name=val; path=/; ..." against the page
 * origin (host/path/is_https). */
int  mini_cookies_set_js(const char *host, const char *path,
                         int is_https, const char *cookie_str);

/* document.cookie getter: "name=val; name=val" (non-HttpOnly) into out. */
int  mini_cookies_get_js(const char *host, const char *path, int is_https,
                        char *out, size_t cap);

int  mini_cookies_count(void); /* live cookies (not expired) */

#ifdef __cplusplus
}
#endif
#endif /* MINI_COOKIES_H */
