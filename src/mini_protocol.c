/*
 * mini_protocol.c — see mini_protocol.h.
 */
#include "mini_protocol.h"
#include "mini_js_bridge.h"
#include "mini_native.h"
#include "mini_window.h"
#include "mini_net.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { PROTO_STRING = 0, PROTO_BUFFER = 1, PROTO_FILE = 2, PROTO_HTTP = 3, PROTO_HANDLE = 4 };

typedef struct ProtoEntry { char *scheme; int type; JSValue handler; } ProtoEntry;
struct MiniProtocol
{
    ProtoEntry *a;
    int n, cap;
    JSContext *ctx; /* the main context (set at install) */
};

MiniProtocol *mini_protocol_create(void)
{
    return (MiniProtocol *)calloc(1, sizeof(MiniProtocol));
}

void mini_protocol_destroy(MiniProtocol *p)
{
    if (!p)
        return;
    /* Drop the JS ref we took on each handler (JS_DupValue in proto_register).
       Must run while the main context is still alive (mini_app_destroy calls
       this BEFORE mini_bridge_destroy), else the dup'd functions would remain
       on the GC list and trigger the list_empty(&rt->gc_obj_list) assertion
       at JS_FreeRuntime. */
    if (p->ctx)
    {
        for (int i = 0; i < p->n; i++)
        {
            JS_FreeValue(p->ctx, p->a[i].handler);
            p->a[i].handler = JS_NULL;
        }
    }
    for (int i = 0; i < p->n; i++)
        free(p->a[i].scheme);
    free(p->a);
    free(p);
}

static int proto_register(MiniProtocol *p, JSContext *ctx, const char *scheme, int type, JSValueConst handler)
{
    if (!p || !scheme || !JS_IsFunction(ctx, handler))
        return -1;
    /* replace an existing registration for the same scheme */
    for (int i = 0; i < p->n; i++)
    {
        if (p->a[i].scheme && !strcmp(p->a[i].scheme, scheme))
        {
            /* (old JSValue freed by GC) */
            p->a[i].type = type;
            p->a[i].handler = JS_DupValue(ctx, handler);
            p->ctx = ctx;
            return 0;
        }
    }
    if (p->n >= p->cap)
    {
        int nc = p->cap ? p->cap * 2 : 8;
        ProtoEntry *na = (ProtoEntry *)realloc(p->a, (size_t)nc * sizeof(*na));
        if (!na) return -1;
        p->a = na; p->cap = nc;
    }
    p->a[p->n].scheme = strdup(scheme);
    p->a[p->n].type = type;
    p->a[p->n].handler = JS_DupValue(ctx, handler);
    p->n++;
    p->ctx = ctx;
    return 0;
}

static MiniProtocol *proto_of(JSContext *ctx)
{
    struct MiniBridge *b = (struct MiniBridge *)JS_GetContextOpaque(ctx);
    return b ? (MiniProtocol *)mini_bridge_get_proto(b) : NULL;
}

/* ---- JS bindings (main context) ------------------------------------ */
static JSValue js_proto_reg(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv, int type)
{
    (void)tv;
    MiniProtocol *p = proto_of(ctx);
    if (!p || argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "register(scheme, handler) expects (string, function)");
    const char *scheme = JS_ToCString(ctx, argv[0]);
    int rc = proto_register(p, ctx, scheme, type, argv[1]);
    JS_FreeCString(ctx, scheme);
    return rc ? JS_ThrowTypeError(ctx, "protocol: registration failed") : JS_UNDEFINED;
}
static JSValue js_proto_reg_string(JSContext *ctx, JSValueConst tv, int a, JSValueConst *b) { return js_proto_reg(ctx, tv, a, b, PROTO_STRING); }
static JSValue js_proto_reg_buffer(JSContext *ctx, JSValueConst tv, int a, JSValueConst *b) { return js_proto_reg(ctx, tv, a, b, PROTO_BUFFER); }
static JSValue js_proto_reg_file(JSContext *ctx, JSValueConst tv, int a, JSValueConst *b) { return js_proto_reg(ctx, tv, a, b, PROTO_FILE); }
static JSValue js_proto_reg_http(JSContext *ctx, JSValueConst tv, int a, JSValueConst *b) { return js_proto_reg(ctx, tv, a, b, PROTO_HTTP); }
static JSValue js_proto_handle(JSContext *ctx, JSValueConst tv, int a, JSValueConst *b) { return js_proto_reg(ctx, tv, a, b, PROTO_HANDLE); }

static JSValue js_proto_unprotocol(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    MiniProtocol *p = proto_of(ctx);
    if (!p || argc < 1 || !JS_IsString(argv[0]))
        return JS_UNDEFINED;
    const char *scheme = JS_ToCString(ctx, argv[0]);
    for (int i = 0; i < p->n; i++)
    {
        if (p->a[i].scheme && !strcmp(p->a[i].scheme, scheme))
        {
            free(p->a[i].scheme);
            /* shift down */
            for (int j = i; j < p->n - 1; j++) p->a[j] = p->a[j + 1];
            p->n--;
            break;
        }
    }
    JS_FreeCString(ctx, scheme);
    return JS_UNDEFINED;
}

static JSValue js_proto_register_schemes(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    /* best-effort: accept + ignore (privileged-scheme behaviour like secure,
       standard, supportFetchAPI, etc. is implied by our fetch/loadURL path). */
    (void)tv; (void)argv;
    if (argc < 1) return JS_UNDEFINED;
    return JS_UNDEFINED;
}

void mini_protocol_install(MiniProtocol *p, struct MiniApp *app)
{
    if (!p || !app)
        return;
    struct MiniBridge *b = mini_app_main_bridge(app);
    if (!b) return;
    mini_bridge_set_proto(b, p);
    p->ctx = mini_bridge_ctx(b);
    JSContext *ctx = p->ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue e = JS_GetPropertyStr(ctx, global, "electron");
    if (!JS_IsObject(e)) { e = JS_NewObject(ctx); JS_SetPropertyStr(ctx, global, "electron", e); }
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "registerStringProtocol", JS_NewCFunction(ctx, js_proto_reg_string, "registerStringProtocol", 2));
    JS_SetPropertyStr(ctx, proto, "registerBufferProtocol", JS_NewCFunction(ctx, js_proto_reg_buffer, "registerBufferProtocol", 2));
    JS_SetPropertyStr(ctx, proto, "registerFileProtocol", JS_NewCFunction(ctx, js_proto_reg_file, "registerFileProtocol", 2));
    JS_SetPropertyStr(ctx, proto, "registerHttpProtocol", JS_NewCFunction(ctx, js_proto_reg_http, "registerHttpProtocol", 2));
    JS_SetPropertyStr(ctx, proto, "handle", JS_NewCFunction(ctx, js_proto_handle, "handle", 2));
    JS_SetPropertyStr(ctx, proto, "unprotocol", JS_NewCFunction(ctx, js_proto_unprotocol, "unprotocol", 1));
    JS_SetPropertyStr(ctx, proto, "unregisterSchemesAsPrivileged", JS_NewCFunction(ctx, js_proto_register_schemes, "unregisterSchemesAsPrivileged", 1));
    JS_SetPropertyStr(ctx, proto, "registerSchemesAsPrivileged", JS_NewCFunction(ctx, js_proto_register_schemes, "registerSchemesAsPrivileged", 1));
    JS_SetPropertyStr(ctx, e, "protocol", proto);
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, global);
}

/* ---- resolve (consulted by loadURL) -------------------------------- */
static int starts_with(const char *s, const char *p)
{
    while (*p) { if (*s++ != *p++) return 0; }
    return 1;
}

int mini_protocol_resolve(MiniProtocol *p, struct MiniApp *app,
                          const char *url,
                          char **body, size_t *len, char **mime, int *status)
{
    if (!p || !url || !body || !len || !mime || !status)
        return 0;
    *body = NULL; *len = 0; *mime = NULL; *status = 200;
    /* parse scheme = substring before "://" */
    const char *sep = strstr(url, "://");
    if (!sep)
        return 0;
    size_t slen = (size_t)(sep - url);
    char scheme[64];
    if (slen >= sizeof(scheme))
        return 0;
    memcpy(scheme, url, slen); scheme[slen] = 0;

    ProtoEntry *e = NULL;
    for (int i = 0; i < p->n; i++)
        if (p->a[i].scheme && !strcmp(p->a[i].scheme, scheme))
        { e = &p->a[i]; break; }
    if (!e)
        return 0;

    JSContext *ctx = p->ctx;
    if (!ctx)
        return 0;
    /* build a minimal request object */
    JSValue req = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, req, "url", JS_NewString(ctx, url));
    JS_SetPropertyStr(ctx, req, "method", JS_NewString(ctx, "GET"));
    JS_SetPropertyStr(ctx, req, "referrer", JS_NewString(ctx, ""));
    JSValue hdrs = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, req, "headers", hdrs);

    JSValue r = JS_Call(ctx, e->handler, JS_UNDEFINED, 1, &req);
    JS_FreeValue(ctx, req);
    if (JS_IsException(r))
    {
        JSValue ex = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, ex);
        fprintf(stderr, "[protocol] handler for %s:// threw: %s\n", scheme, s ? s : "?");
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, ex);
        JS_FreeValue(ctx, r);
        return 0;
    }

    int resolved = 0;
    if (e->type == PROTO_FILE)
    {
        /* result is { path } or a path string */
        JSValue pv = JS_IsString(r) ? JS_DupValue(ctx, r) : JS_GetPropertyStr(ctx, r, "path");
        if (JS_IsString(pv))
        {
            const char *path = JS_ToCString(ctx, pv);
            if (path)
            {
                FILE *f = fopen(path, "rb");
                if (f)
                {
                    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
                    char *buf = (char *)malloc((size_t)sz + 1);
                    size_t rd = buf ? fread(buf, 1, (size_t)sz, f) : 0;
                    fclose(f);
                    if (buf) { buf[rd] = 0; *body = buf; *len = rd; resolved = 1; }
                }
                JS_FreeCString(ctx, path);
            }
        }
        JS_FreeValue(ctx, pv);
    }
    else if (e->type == PROTO_HTTP)
    {
        /* result is { url } -> fetch that URL */
        JSValue uv = JS_GetPropertyStr(ctx, r, "url");
        if (JS_IsString(uv))
        {
            const char *u = JS_ToCString(ctx, uv);
            if (u)
            {
                MiniNetRecord rec; memset(&rec, 0, sizeof(rec));
                if (mini_net_fetch("GET", u, NULL, NULL, 0, NULL, &rec) == 0 && rec.resp_body)
                {
                    *body = (char *)malloc(rec.resp_body_len + 1);
                    if (*body) { memcpy(*body, rec.resp_body, rec.resp_body_len); (*body)[rec.resp_body_len] = 0; *len = rec.resp_body_len; resolved = 1; }
                    *status = rec.status;
                }
                mini_net_record_free(&rec);
                JS_FreeCString(ctx, u);
            }
        }
        JS_FreeValue(ctx, uv);
    }
    else
    {
        /* string/buffer/handle: { data, mimeType, statusCode } */
        JSValue dv = JS_GetPropertyStr(ctx, r, "data");
        if (JS_IsString(dv))
        {
            size_t plen = 0;
            const char *s = JS_ToCStringLen(ctx, &plen, dv);
            if (s)
            {
                *body = (char *)malloc(plen + 1);
                if (*body) { memcpy(*body, s, plen); (*body)[plen] = 0; *len = plen; resolved = 1; }
                JS_FreeCString(ctx, s);
            }
        }
        else if (JS_IsObject(dv))
        {
            /* ArrayBuffer? */
            size_t ablen = 0;
            const uint8_t *ab = JS_GetArrayBuffer(ctx, &ablen, dv);
            if (ab)
            {
                *body = (char *)malloc(ablen + 1);
                if (*body) { memcpy(*body, ab, ablen); (*body)[ablen] = 0; *len = ablen; resolved = 1; }
            }
        }
        JS_FreeValue(ctx, dv);
        JSValue mv = JS_GetPropertyStr(ctx, r, "mimeType");
        if (JS_IsString(mv))
        {
            const char *m = JS_ToCString(ctx, mv);
            if (m) *mime = strdup(m);
            JS_FreeCString(ctx, m);
        }
        JS_FreeValue(ctx, mv);
        JSValue sv = JS_GetPropertyStr(ctx, r, "statusCode");
        if (JS_IsNumber(sv)) JS_ToInt32(ctx, status, sv);
        JS_FreeValue(ctx, sv);
    }
    JS_FreeValue(ctx, r);
    if (!*mime) *mime = strdup("text/html");
    (void)starts_with; (void)app;
    return resolved;
}
