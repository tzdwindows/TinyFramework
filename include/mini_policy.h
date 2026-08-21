/*
 * mini_policy.h — web security policy: HSTS, SOP, CORS, CSP (Phase 6).
 *
 * A compact, real implementation of the policy gates a browser applies around
 * the network stack. Each is small on its own; collected here so the fetch
 * orchestration (mini_net_fetch) has one policy surface to consult.
 *
 *   • HSTS   — remember hosts that sent Strict-Transport-Security; upgrade
 *              http→https for them.
 *   • SOP    — same-origin test for fetch mode "same-origin" / cookie scoping.
 *   • CORS  — is the response's Access-Control-Allow-Origin permitted for the
 *              requesting (credentialed?) origin; and is a preflight needed.
 *   • CSP   — parse Content-Security-Policy and test whether a directive
 *              allows a resource origin ('self' / 'none' / '*' / origins).
 *
 * Pure C99, no deps.
 */
#ifndef MINI_POLICY_H
#define MINI_POLICY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { char scheme[8]; char host[256]; int port; } MiniOrigin;

/* Parse "scheme://host[:port]" into an origin. Returns 0 on success. */
int mini_origin_parse(const char *url, MiniOrigin *o);
/* Two origins are "same" iff scheme+host+port all match. */
int mini_origin_same(const MiniOrigin *a, const MiniOrigin *b);
/* Render an origin as "scheme://host[:port]" into out. */
int mini_origin_str(const MiniOrigin *o, char *out, size_t cap);

/* ---- HSTS ---- */
void mini_hsts_clear(void);
/* Parse a Strict-Transport-Security header value from `host`, remember it.
 * max-age (incl. 0 = delete) and includeSubDomains are honored. */
void mini_hsts_remember(const char *host, const char *sts_header);
/* 1 if the host is known-HSTS and should be upgraded http→https. */
int  mini_hsts_should_upgrade(const char *host);

/* ---- SOP ---- */
/* 1 if `url` is same-origin with `page`. (mode "same-origin" gate.) */
int mini_sop_same_origin(const MiniOrigin *page, const char *url);

/* ---- CORS ---- */
/* 1 if a preflight (OPTIONS) is required for this cross-origin request:
 * non-simple method or non-simple headers (Content-Type beyond the simple
 * forms, or any header outside the safelist). */
int mini_cors_preflight_needed(const char *method, const char *header_names_csv);
/* 1 if the response may be exposed to `req_origin`, given the response's
 * Access-Control-Allow-Origin value and whether credentials were sent. */
int mini_cors_response_allowed(const MiniOrigin *req_origin,
                              const char *aca_origin, int aca_credentials,
                              int is_credentialed);

/* ---- CSP ---- */
/* Parse a CSP policy string (multiple directives, space-separated sources). */
void mini_csp_set(const char *csp);
/* 1 if `directive` (e.g. "script-src"/"img-src"/"default-src") allows a
 * resource from `url` given the page `page` origin. Honors 'self', 'none',
 * '*', and scheme/host origins. */
int mini_csp_allows(const char *directive, const MiniOrigin *page, const char *url);
void mini_csp_clear(void);

#ifdef __cplusplus
}
#endif
#endif /* MINI_POLICY_H */
