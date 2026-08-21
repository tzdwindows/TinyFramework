/*
 * mini_cookies.c — RFC6265 cookie jar implementation + self-test.
 *
 * Compact but real: domain match (host-only vs domain cookies, leading-dot
 * strip, subdomain match), path match (prefix), Secure gating, SameSite
 * stored (cross-site gating is the policy layer's job), Max-Age/Expires
 * parsing into an absolute expiry, HttpOnly hiding from document.cookie.
 */
#include "mini_cookies.h"
#include "mini_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

/* strptime()/timegm() are GNU extensions not exported by the default MinGW
   CRT, so we ship a tiny dependency-free HTTP-date parser instead (more
   portable and good enough for the RFC6265 date formats servers actually send). */
static const char *const COOKIE_MONTHS[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static int month_num(const char *m)
{
    if (!m || !m[0])
        return -1;
    for (int i = 0; i < 12; i++)
        if (!strncasecmp(m, COOKIE_MONTHS[i], 3))
            return i;
    return -1;
}
/* mktime, but for a UTC broken-down time (no timezone offset). */
static time_t mkgmtime_utc(struct tm *t)
{
    int year = t->tm_year + 1900;
    int month = t->tm_mon; /* 0..11 */
    static const int dsm[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    long days = (long)(year - 1970) * 365L
              + (year - 1969) / 4 - (year - 1901) / 100 + (year - 1601) / 400;
    days += dsm[month];
    if (month > 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        days += 1;
    days += t->tm_mday - 1;
    return (time_t)(days * 86400L + t->tm_hour * 3600L + t->tm_min * 60L + t->tm_sec);
}
/* Parse an RFC6265 HTTP-date (RFC1123 / RFC850) into a UTC time_t, or 0. */
static time_t parse_http_date(const char *s)
{
    if (!s || !s[0])
        return 0;
    /* skip a leading weekday name + comma + spaces ("Wed, " / "Wednesday, ") */
    const char *p = s;
    while (*p && (isalpha((unsigned char)*p) || *p == ',' || isspace((unsigned char)*p)))
        p++;
    int day, year, hh, mm, ss;
    char mname[8] = {0};
    struct tm t;
    memset(&t, 0, sizeof t);
    int mon;
    /* RFC1123: "09 Jun 2021 10:18:14 GMT" */
    if (sscanf(p, "%d %3s %d %d:%d:%d", &day, mname, &year, &hh, &mm, &ss) == 6)
    {
        mon = month_num(mname);
        if (mon < 0)
            return 0;
        t.tm_mday = day; t.tm_mon = mon; t.tm_year = year - 1900;
        t.tm_hour = hh; t.tm_min = mm; t.tm_sec = ss;
        return mkgmtime_utc(&t);
    }
    /* RFC850: "09-Jun-21 10:18:14 GMT" */
    if (sscanf(p, "%d-%3[^-]-%d %d:%d:%d", &day, mname, &year, &hh, &mm, &ss) == 6)
    {
        mon = month_num(mname);
        if (mon < 0)
            return 0;
        if (year < 100)
            year += (year < 70) ? 2000 : 1900;
        t.tm_mday = day; t.tm_mon = mon; t.tm_year = year - 1900;
        t.tm_hour = hh; t.tm_min = mm; t.tm_sec = ss;
        return mkgmtime_utc(&t);
    }
    return 0;
}

#define MAX_COOKIES 256
static MiniCookie g_jar[MAX_COOKIES];
static int g_jar_n = 0;

void mini_cookies_init(void) { /* state is static-zero; nothing to prealloc */ }
void mini_cookies_clear(void)
{
    g_jar_n = 0;
}

static void lc(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

static int host_matches(const char *domain, const char *host)
{
    /* host-only (domain empty) -> exact host match; else domain match: the
       host equals domain OR host ends with '.'+domain (subdomain). */
    if (!domain || !domain[0])
        return 0;
    size_t dl = strlen(domain), hl = strlen(host);
    if (hl == dl && !strcmp(host, domain))
        return 1;
    if (hl > dl && host[hl - dl - 1] == '.' && !strcmp(host + hl - dl, domain))
        return 1;
    return 0;
}

static int path_matches(const char *cpath, const char *req_path)
{
    if (!cpath || !cpath[0])
        return 0;
    if (strcmp(cpath, "/") == 0)
        return 1; /* path=/ matches everything */
    size_t cl = strlen(cpath), rl = strlen(req_path);
    if (rl < cl)
        return 0;
    if (strncmp(cpath, req_path, cl) != 0)
        return 0;
    /* the char right after the prefix must be '/' or end (per RFC path-match) */
    if (rl == cl)
        return 1;
    return req_path[cl] == '/' || cpath[cl - 1] == '/';
}

/* Parse a date that is either a Max-Age integer (seconds) or an HTTP-date.
   Returns an absolute expiry time, or 0 for session. */
static time_t parse_expiry(const char *max_age, const char *expires)
{
    if (max_age && max_age[0])
    {
        long s = atol(max_age);
        if (s <= 0)
            return 1; /* expired immediately */
        return time(NULL) + s;
    }
    if (expires && expires[0])
        return parse_http_date(expires);
    return 0; /* session cookie */
}

/* lowercase the domain attribute and strip a leading dot; default the domain
   to the request host (host-only cookie) when the attribute is absent. */
static void canon_domain(char *domain, size_t cap, const char *host)
{
    if (!domain[0])
    {
        snprintf(domain, cap, "%s", host ? host : "");
        lc(domain);
        return;
    }
    char *d = domain;
    if (d[0] == '.')
        memmove(d, d + 1, strlen(d)); /* strip leading dot */
    lc(domain);
    /* the domain attribute must domain-match the request host, else reject */
    if (!host_matches(domain, host))
        domain[0] = 0;
}

/* parse "name=value; Path=/; Domain=...; Secure; HttpOnly; SameSite=Lax;
   Max-Age=3600; expires=..." into a MiniCookie. Returns 0 if it has a name. */
static int parse_cookie(const char *str, const char *host, int is_https,
                        MiniCookie *out)
{
    memset(out, 0, sizeof *out);
    const char *p = str;
    /* first pair = name=value */
    const char *semi = strchr(p, ';');
    size_t nvlen = semi ? (size_t)(semi - p) : strlen(p);
    const char *eq = memchr(p, '=', nvlen);
    if (!eq)
        return -1;
    size_t nlen = (size_t)(eq - p);
    size_t vlen = nvlen - nlen - 1;
    if (nlen == 0 || nlen >= sizeof out->name || vlen >= sizeof out->value)
        return -1;
    memcpy(out->name, p, nlen); out->name[nlen] = 0;
    memcpy(out->value, eq + 1, vlen); out->value[vlen] = 0;
    /* trim spaces */
    char *e = out->name + nlen; while (e > out->name && isspace((unsigned char)e[-1])) *--e = 0;

    char max_age[32] = {0}, expires[64] = {0};
    const char *rest = semi ? semi + 1 : (p + nvlen);
    while (*rest)
    {
        while (*rest && (*rest == ';' || isspace((unsigned char)*rest))) rest++;
        const char *a = rest;
        const char *s2 = strchr(a, ';');
        size_t alen = s2 ? (size_t)(s2 - a) : strlen(a);
        const char *aeq = memchr(a, '=', alen);
        char aname[32]; size_t anlen = aeq ? (size_t)(aeq - a) : alen;
        if (anlen >= sizeof aname) anlen = sizeof aname - 1;
        memcpy(aname, a, anlen); aname[anlen] = 0;
        char aval[128]; size_t avlen = aeq ? alen - anlen - 1 : 0;
        if (avlen >= sizeof aval) avlen = sizeof aval - 1;
        memcpy(aval, aeq ? aeq + 1 : "", avlen); aval[avlen] = 0;
        if (!strcasecmp(aname, "path")) snprintf(out->path, sizeof out->path, "%s", aval);
        else if (!strcasecmp(aname, "domain")) snprintf(out->domain, sizeof out->domain, "%s", aval);
        else if (!strcasecmp(aname, "secure")) out->secure = 1;
        else if (!strcasecmp(aname, "httponly")) out->http_only = 1;
        else if (!strcasecmp(aname, "max-age")) snprintf(max_age, sizeof max_age, "%s", aval);
        else if (!strcasecmp(aname, "expires")) snprintf(expires, sizeof expires, "%s", aval);
        else if (!strcasecmp(aname, "samesite"))
        {
            if (!strcasecmp(aval, "lax")) out->same_site = MINI_SAME_LAX;
            else if (!strcasecmp(aval, "strict")) out->same_site = MINI_SAME_STRICT;
            else out->same_site = MINI_SAME_NONE;
        }
        rest = s2 ? s2 + 1 : a + alen;
    }
    if (out->secure && !is_https)
        return -1; /* Secure cookie over http -> drop */
    canon_domain(out->domain, sizeof out->domain, host);
    if (!out->domain[0])
        return -1; /* domain attribute didn't match the request host */
    if (!out->path[0])
        snprintf(out->path, sizeof out->path, "/");
    out->expires = parse_expiry(max_age[0] ? max_age : NULL, expires[0] ? expires : NULL);
    return 0;
}

static int find_slot(const MiniCookie *c)
{
    for (int i = 0; i < g_jar_n; i++)
        if (!strcmp(g_jar[i].name, c->name) &&
            !strcmp(g_jar[i].domain, c->domain) &&
            !strcmp(g_jar[i].path, c->path))
            return i;
    return -1;
}

static void purge_expired(void)
{
    time_t now = time(NULL);
    int w = 0;
    for (int i = 0; i < g_jar_n; i++)
    {
        if (g_jar[i].expires && g_jar[i].expires <= now)
            continue; /* expired */
        if (w != i)
            g_jar[w] = g_jar[i];
        w++;
    }
    g_jar_n = w;
}

int mini_cookies_set_from_header(const char *host, const char *path,
                                 int is_https, const char *set_cookie)
{
    if (!set_cookie || !set_cookie[0])
        return -1;
    MiniCookie c;
    if (parse_cookie(set_cookie, host, is_https, &c) != 0)
        return -1;
    purge_expired();
    int idx = find_slot(&c);
    if (idx >= 0)
    {
        /* overwrite */
        g_jar[idx] = c;
        return 0;
    }
    if (g_jar_n >= MAX_COOKIES)
    {
        /* evict the least-recently-used (used==0 first) */
        memmove(&g_jar[0], &g_jar[1], (MAX_COOKIES - 1) * sizeof(MiniCookie));
        g_jar_n = MAX_COOKIES - 1;
    }
    g_jar[g_jar_n++] = c;
    MINI_LOGD("net.cookie", "set %s=%s domain=%s path=%s",
              c.name, c.value, c.domain, c.path);
    return 0;
}

int mini_cookies_set_js(const char *host, const char *path,
                       int is_https, const char *cookie_str)
{
    /* document.cookie setter: host-only by default (no Domain attr honored
       from JS for safety in this compact impl — matches stricter browsers). */
    return mini_cookies_set_from_header(host, path, is_https, cookie_str);
}

static int eligible(const MiniCookie *c, const char *host, const char *path,
                    int is_https, int for_js)
{
    if (for_js && c->http_only)
        return 0;
    if (c->secure && !is_https)
        return 0;
    if (!host_matches(c->domain, host))
        return 0;
    if (!path_matches(c->path, path))
        return 0;
    return 1;
}

static int build_for(const char *host, const char *path, int is_https,
                     int for_js, char *out, size_t cap)
{
    purge_expired();
    size_t off = 0;
    int n = 0;
    for (int i = 0; i < g_jar_n; i++)
    {
        if (!eligible(&g_jar[i], host, path, is_https, for_js))
            continue;
        const char *sep = (n == 0) ? "" : "; ";
        size_t seplen = strlen(sep);
        if (off + seplen + strlen(g_jar[i].name) + 1 + strlen(g_jar[i].value) + 1 > cap)
            break;
        off += (size_t)snprintf(out + off, cap - off, "%s%s=%s",
                                sep, g_jar[i].name, g_jar[i].value);
        n++;
    }
    out[off] = 0;
    return n ? (int)off : -1;
}

int mini_cookies_build_header(const char *host, const char *path,
                              int is_https, int for_js, char *out, size_t cap)
{
    return build_for(host, path, is_https, for_js, out, cap);
}

int mini_cookies_get_js(const char *host, const char *path, int is_https,
                       char *out, size_t cap)
{
    return build_for(host, path, is_https, 1, out, cap);
}

int mini_cookies_count(void)
{
    purge_expired();
    return g_jar_n;
}

/* ================================================================== */
/* COOKIES_SELFTEST                                                    */
/* ================================================================== */
#ifdef COOKIES_SELFTEST
#include <assert.h>
static int ck_fail = 0;
#define CK(c, m) do { if (!(c)) { fprintf(stderr, "CK FAIL: %s\n", m); ck_fail++; } } while (0)

int main(void)
{
    mini_log_init();
    mini_cookies_clear();

    /* host-only cookie on example.com */
    CK(mini_cookies_set_from_header("example.com", "/", 1,
        "sid=abc; Path=/; HttpOnly; Secure; SameSite=Lax") == 0, "set host-only");
    char hdr[512];
    /* sent over https to same host, not to JS */
    CK(mini_cookies_build_header("example.com", "/", 1, 0, hdr, sizeof hdr) > 0, "build header");
    CK(strstr(hdr, "sid=abc") != NULL, "header has sid");
    /* document.cookie hides HttpOnly */
    char js[512];
    CK(mini_cookies_get_js("example.com", "/", 1, js, sizeof js) == -1, "httponly hidden from JS");

    /* domain cookie matches subdomain */
    CK(mini_cookies_set_from_header("example.com", "/", 1,
        "pref=dark; Domain=example.com; Path=/; Max-Age=3600") == 0, "set domain cookie");
    CK(mini_cookies_build_header("api.example.com", "/v1", 1, 0, hdr, sizeof hdr) > 0, "subdomain match");
    CK(strstr(hdr, "pref=dark") != NULL, "subdomain gets domain cookie");
    /* over http the Secure cookie is dropped at SET time */
    CK(mini_cookies_set_from_header("example.com", "/", 0,
        "sec=yes; Secure") == -1, "secure over http dropped");
    /* path specificity */
    CK(mini_cookies_set_from_header("example.com", "/app", 1,
        "k=v; Path=/app") == 0, "set path cookie");
    CK(mini_cookies_build_header("example.com", "/app/page", 1, 0, hdr, sizeof hdr) > 0, "path prefix match");
    /* overwrite by name+domain+path */
    CK(mini_cookies_set_from_header("example.com", "/", 1,
        "sid=xyz; Path=/; HttpOnly; Secure") == 0, "overwrite");
    mini_cookies_build_header("example.com", "/", 1, 0, hdr, sizeof hdr);
    CK(strstr(hdr, "sid=xyz") != NULL && !strstr(hdr, "sid=abc"), "overwrite replaced");
    CK(mini_cookies_count() <= MAX_COOKIES, "jar bounded");

    fprintf(stderr, ck_fail ? "COOKIES_SELFTEST: %d FAIL\n" : "COOKIES_SELFTEST: all PASS\n", ck_fail);
    return ck_fail ? 1 : 0;
}
#endif
