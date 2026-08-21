/*
 * mini_policy.c — HSTS / SOP / CORS / CSP implementation + self-test.
 */
#include "mini_policy.h"
#include "mini_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

static const char *mini_strcasestr(const char *h, const char *n)
{
    if (!h || !n) return NULL;
    size_t nl = strlen(n);
    for (; *h; h++)
    {
        size_t i = 0;
        for (; i < nl && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)n[i]); i++)
            ;
        if (i == nl) return h;
    }
    return NULL;
}

/* ---- origin ---- */
int mini_origin_parse(const char *url, MiniOrigin *o)
{
    if (!o) return -1;
    memset(o, 0, sizeof *o);
    if (!url) return -1;
    const char *p = url;
    size_t si = 0;
    while (*p && *p != ':' && si + 1 < sizeof o->scheme) o->scheme[si++] = *p++;
    o->scheme[si] = 0;
    if (*p != ':') return -1;
    p++; /* ':' */
    if (*p == '/' && p[1] == '/') p += 2; else return -1;
    size_t hi = 0;
    while (*p && *p != ':' && *p != '/' && *p != '?' && *p != '#' && hi + 1 < sizeof o->host)
        o->host[hi++] = *p++;
    o->host[hi] = 0;
    if (*p == ':')
    {
        p++;
        o->port = 0;
        while (*p >= '0' && *p <= '9') o->port = o->port * 10 + (*p++ - '0');
    }
    else
        o->port = !strcmp(o->scheme, "https") ? 443 : 80;
    return hi > 0 ? 0 : -1;
}

int mini_origin_same(const MiniOrigin *a, const MiniOrigin *b)
{
    return a && b && !strcmp(a->scheme, b->scheme) &&
           !strcasecmp(a->host, b->host) && a->port == b->port;
}

int mini_origin_str(const MiniOrigin *o, char *out, size_t cap)
{
    if (!o) return -1;
    return snprintf(out, cap, "%s://%s", o->scheme, o->host);
}

/* ---- HSTS ---- */
typedef struct { char host[256]; time_t expires; int subs; int used; } HstsEntry;
#define HSTS_MAX 64
static HstsEntry g_hsts[HSTS_MAX];
static int g_hsts_n = 0;

void mini_hsts_clear(void) { g_hsts_n = 0; }

void mini_hsts_remember(const char *host, const char *sts)
{
    if (!host || !sts || !sts[0]) return;
    /* parse max-age=N and includeSubDomains */
    const char *ma = mini_strcasestr(sts, "max-age");
    long age = -1;
    if (ma)
    {
        ma += 7;
        while (*ma && (*ma == ' ' || *ma == '=' || *ma == '\t')) ma++;
        age = atol(ma);
    }
    int subs = mini_strcasestr(sts, "includeSubDomains") != NULL ? 1 : 0;
    int idx = -1;
    for (int i = 0; i < g_hsts_n; i++)
        if (!strcasecmp(g_hsts[i].host, host)) { idx = i; break; }
    if (age <= 0)
    {
        /* delete */
        if (idx >= 0)
        {
            memmove(&g_hsts[idx], &g_hsts[idx + 1], (g_hsts_n - idx - 1) * sizeof(HstsEntry));
            g_hsts_n--;
        }
        return;
    }
    if (idx < 0)
    {
        if (g_hsts_n >= HSTS_MAX)
        {
            memmove(&g_hsts[0], &g_hsts[1], (HSTS_MAX - 1) * sizeof(HstsEntry));
            g_hsts_n = HSTS_MAX - 1;
        }
        idx = g_hsts_n++;
        snprintf(g_hsts[idx].host, sizeof g_hsts[idx].host, "%s", host);
    }
    g_hsts[idx].expires = time(NULL) + age;
    g_hsts[idx].subs = subs;
}

int mini_hsts_should_upgrade(const char *host)
{
    if (!host) return 0;
    time_t now = time(NULL);
    for (int i = 0; i < g_hsts_n; i++)
    {
        if (g_hsts[i].expires && g_hsts[i].expires <= now) continue;
        if (!strcasecmp(g_hsts[i].host, host)) return 1;
        if (g_hsts[i].subs)
        {
            /* subdomain: host ends with '.'+hsts.host */
            size_t hl = strlen(host), bl = strlen(g_hsts[i].host);
            if (hl > bl && host[hl - bl - 1] == '.' &&
                !strcasecmp(host + hl - bl, g_hsts[i].host))
                return 1;
        }
    }
    return 0;
}

/* ---- SOP ---- */
int mini_sop_same_origin(const MiniOrigin *page, const char *url)
{
    if (!page || !url) return 0;
    MiniOrigin o;
    if (mini_origin_parse(url, &o) != 0) return 0;
    return mini_origin_same(page, &o);
}

/* ---- CORS ---- */
static int is_simple_method(const char *m)
{
    return m && (!strcasecmp(m, "GET") || !strcasecmp(m, "HEAD") || !strcasecmp(m, "POST"));
}
static int is_simple_header(const char *name)
{
    if (!name) return 1;
    while (*name == ' ') name++;
    return !strcasecmp(name, "Accept") || !strcasecmp(name, "Accept-Language") ||
           !strcasecmp(name, "Content-Language") ||
           !strcasecmp(name, "Content-Type") ||
           !strcasecmp(name, "Range");
}
static int is_simple_content_type(const char *val)
{
    return val && (!strncasecmp(val, "application/x-www-form-urlencoded", 33) ||
                   !strncasecmp(val, "multipart/form-data", 19) ||
                   !strncasecmp(val, "text/plain", 10));
}

int mini_cors_preflight_needed(const char *method, const char *headers_csv)
{
    if (!is_simple_method(method))
        return 1;
    if (!headers_csv || !headers_csv[0])
        return 0;
    /* split header names on the CRLF that mini_net joins with; we accept ", "
       or "\r\n" separators. A Content-Type that is a non-simple value forces
       a preflight. */
    const char *p = headers_csv;
    while (*p)
    {
        char name[64] = {0};
        size_t ni = 0;
        while (*p && *p != ':' && *p != ',' && *p != '\r' && *p != '\n' && ni + 1 < sizeof name)
            name[ni++] = *p++;
        name[ni] = 0;
        /* trim trailing spaces */
        while (ni > 0 && (name[ni - 1] == ' ')) name[--ni] = 0;
        const char *val = NULL;
        if (*p == ':')
        {
            p++;
            while (*p == ' ') p++;
            val = p;
            while (*p && *p != '\r' && *p != '\n' && *p != ',') p++;
        }
        if (ni > 0 && !is_simple_header(name))
            return 1;
        if (ni > 0 && val && !strcasecmp(name, "Content-Type") && !is_simple_content_type(val))
            return 1;
        /* advance past separator */
        while (*p == ',' || *p == '\r' || *p == '\n' || *p == ' ') p++;
    }
    return 0;
}

int mini_cors_response_allowed(const MiniOrigin *req_origin,
                              const char *aca_origin, int aca_credentials,
                              int is_credentialed)
{
    if (!aca_origin || !aca_origin[0])
        return 0;
    if (!strcmp(aca_origin, "*"))
    {
        /* credentialed requests cannot use the wildcard */
        if (is_credentialed) return 0;
        return 1;
    }
    if (!req_origin) return 0;
    char req_str[280];
    mini_origin_str(req_origin, req_str, sizeof req_str);
    if (!strcasecmp(aca_origin, req_str))
        return 1;
    /* "null" origin matches literally */
    if (!strcmp(aca_origin, "null") && !strcmp(req_str, "null"))
        return 1;
    (void)aca_credentials;
    return 0;
}

/* ---- CSP ---- */
/* Store up to 8 directives, each with up to 16 sources. */
#define CSP_MAX_DIRS 8
#define CSP_MAX_SRC 16
typedef struct { char name[24]; char src[CSP_MAX_SRC][128]; int n; } CspDir;
static CspDir g_csp[CSP_MAX_DIRS];
static int g_csp_n = 0;

void mini_csp_clear(void) { g_csp_n = 0; }

void mini_csp_set(const char *csp)
{
    g_csp_n = 0;
    if (!csp || !csp[0]) return;
    const char *p = csp;
    while (*p && g_csp_n < CSP_MAX_DIRS)
    {
        while (*p == ' ' || *p == ';') p++;
        CspDir *d = &g_csp[g_csp_n];
        d->n = 0;
        /* directive name */
        size_t ni = 0;
        while (*p && *p != ' ' && *p != ';' && ni + 1 < sizeof d->name)
            d->name[ni++] = *p++;
        d->name[ni] = 0;
        if (!ni) break;
        /* sources until ; */
        while (*p == ' ') p++;
        while (*p && *p != ';')
        {
            while (*p == ' ') p++;
            char tok[128]; size_t ti = 0;
            while (*p && *p != ' ' && *p != ';' && ti + 1 < sizeof tok)
                tok[ti++] = *p++;
            tok[ti] = 0;
            if (ti && d->n < CSP_MAX_SRC)
                snprintf(d->src[d->n++], sizeof d->src[0], "%s", tok);
            while (*p == ' ') p++;
        }
        g_csp_n++;
        if (*p == ';') p++;
    }
}

static const CspDir *csp_find(const char *name)
{
    for (int i = 0; i < g_csp_n; i++)
        if (!strcasecmp(g_csp[i].name, name)) return &g_csp[i];
    return NULL;
}

int mini_csp_allows(const char *directive, const MiniOrigin *page, const char *url)
{
    const CspDir *d = csp_find(directive);
    if (!d)
        d = csp_find("default-src");
    if (!d)
        return 1; /* no policy -> allow (open) */
    int has_self = 0, has_none = 0, has_star = 0;
    MiniOrigin o;
    int parsed = (mini_origin_parse(url ? url : "", &o) == 0);
    for (int i = 0; i < d->n; i++)
    {
        const char *s = d->src[i];
        if (!strcmp(s, "'none'")) has_none = 1;
        else if (!strcmp(s, "'self'")) has_self = 1;
        else if (!strcmp(s, "*")) has_star = 1;
        else if (parsed)
        {
            /* a source origin like https://cdn.x or scheme:data */
            if (!strncasecmp(s, "http", 4) || !strncmp(s, "data:", 5) ||
                !strncmp(s, "blob:", 5))
            {
                char src_str[128];
                /* match scheme://host only */
                MiniOrigin so;
                if (mini_origin_parse(s, &so) == 0)
                {
                    snprintf(src_str, sizeof src_str, "%s://%s", so.scheme, so.host);
                    char url_str[280];
                    snprintf(url_str, sizeof url_str, "%s://%s", o.scheme, o.host);
                    if (!strcasecmp(src_str, url_str)) return 1;
                }
            }
        }
    }
    if (has_none) return 0;
    if (has_star) return 1;
    if (has_self && page && parsed)
        return mini_origin_same(page, &o);
    if (has_self) return 0; /* 'self' present but no page context */
    return 0; /* directive with sources but none matched */
}

/* ================================================================== */
/* POLICY_SELFTEST                                                     */
/* ================================================================== */
#ifdef POLICY_SELFTEST
static int pl_fail = 0;
#define PL(c, m) do { if (!(c)) { fprintf(stderr, "PL FAIL: %s\n", m); pl_fail++; } } while (0)

int main(void)
{
    mini_log_init();

    /* origin */
    MiniOrigin a, b;
    PL(mini_origin_parse("https://example.com:8443", &a) == 0, "parse origin");
    PL(!strcmp(a.scheme, "https") && !strcmp(a.host, "example.com") && a.port == 8443, "origin fields");
    mini_origin_parse("https://example.com", &b);
    PL(mini_origin_same(&a, &b) == 0, "diff port not same");
    mini_origin_parse("https://example.com:8443", &b);
    PL(mini_origin_same(&a, &b) == 1, "same origin");

    /* HSTS */
    mini_hsts_clear();
    mini_hsts_remember("example.com", "max-age=31536000; includeSubDomains");
    PL(mini_hsts_should_upgrade("example.com") == 1, "hsts host");
    PL(mini_hsts_should_upgrade("api.example.com") == 1, "hsts subdomain");
    PL(mini_hsts_should_upgrade("other.com") == 0, "no hsts");
    mini_hsts_remember("example.com", "max-age=0"); /* delete */
    PL(mini_hsts_should_upgrade("example.com") == 0, "hsts deleted");

    /* SOP */
    mini_origin_parse("https://app.example.com", &a);
    PL(mini_sop_same_origin(&a, "https://app.example.com/path") == 1, "sop same");
    PL(mini_sop_same_origin(&a, "https://other.com/path") == 0, "sop diff");

    /* CORS preflight */
    PL(mini_cors_preflight_needed("GET", "Accept: */*") == 0, "simple GET no preflight");
    PL(mini_cors_preflight_needed("PUT", NULL) == 1, "PUT needs preflight");
    PL(mini_cors_preflight_needed("POST", "Content-Type: application/json") == 1, "json POST preflight");
    PL(mini_cors_preflight_needed("POST", "Content-Type: text/plain") == 0, "plain POST no preflight");
    PL(mini_cors_preflight_needed("GET", "X-Custom: 1") == 1, "custom header preflight");

    /* CORS response */
    mini_origin_parse("https://app.example.com", &a);
    PL(mini_cors_response_allowed(&a, "*", 0, 0) == 1, "wildcard non-cred");
    PL(mini_cors_response_allowed(&a, "*", 0, 1) == 0, "wildcard cred denied");
    PL(mini_cors_response_allowed(&a, "https://app.example.com", 1, 1) == 1, "origin match cred");
    PL(mini_cors_response_allowed(&a, "https://other.com", 0, 0) == 0, "origin mismatch");

    /* CSP */
    mini_csp_clear();
    mini_csp_set("default-src 'self'; script-src 'self' https://cdn.example");
    PL(mini_csp_allows("script-src", &a, "https://cdn.example/lib.js") == 1, "csp script cdn");
    PL(mini_csp_allows("script-src", &a, "https://evil.example/x.js") == 0, "csp script blocked");
    PL(mini_csp_allows("script-src", &a, "https://app.example.com/a.js") == 1, "csp self script");
    mini_origin_parse("https://app.example.com", &a);
    PL(mini_csp_allows("img-src", &a, "https://app.example.com/img.png") == 1, "csp default self img");
    PL(mini_csp_allows("img-src", &a, "https://evil.example/x.png") == 0, "csp default blocked img");
    mini_csp_set("img-src 'none'");
    PL(mini_csp_allows("img-src", &a, "https://app.example.com/img.png") == 0, "csp none blocks");

    fprintf(stderr, pl_fail ? "POLICY_SELFTEST: %d FAIL\n" : "POLICY_SELFTEST: all PASS\n", pl_fail);
    return pl_fail ? 1 : 0;
}
#endif
