/*
 * mini_cdp_domains.c - full CDP domain layer.
 *
 * mini_cdp_server.c owns the WebSocket transport; this file owns the CDP
 * semantics. mini_cdp_dispatch() is called per decoded request (when a
 * host is attached) and routes by domain prefix. Each domain builds a
 * response with a small dynamic buffer (DBuf) and sends it via the
 * public mini_cdp_send_client() / mini_cdp_broadcast() surface.
 *
 * P1 scope here: Runtime (fully backed by QuickJS public API), plus the
 * minimal enable responses every panel needs to initialise. DOM/CSS/
 * Overlay/Page/Debugger/Network/etc. are stubbed to empty results here
 * and filled in by P1.6-P1.8.
 *
 * RemoteObject handles: a small static table maps a string objectId to a
 * JSValue (refcounted via JS_DupValue). Runtime.evaluate can return by
 * value (JSON) or by objectId; getProperties expands an objectId.
 */
#include "mini_cdp.h"
#include "quickjs.h"
#include "quickjs_debug.h"
#include "mini_js_bridge.h"
#include "mini_dom.h"
#include "mini_events.h"
#include "mini_net.h"
#include "mini_diag.h"
#include <GLFW/glfw3.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

/* Buffer for whole-tree DOM serialization (Elements panel). 256 KiB
   covers large documents; mini_dom_serialize_cdp truncates past cap. */
#define CDP_DOM_BUF (256 * 1024)

/* Overlay highlight target (set by Overlay.highlightNode, drawn by the
   renderer in P1.7 via mini_cdp_overlay_get()). 0 = none. */
static int g_overlay_node_id = 0;
static int g_inspect_mode = 0;

int mini_cdp_is_inspect_mode(MiniCDP *cdp)
{
    (void)cdp;
    return g_inspect_mode;
}

void mini_cdp_set_inspect_mode(MiniCDP *cdp, int active)
{
    (void)cdp;
    g_inspect_mode = active;
}

void mini_cdp_highlight_node(MiniCDP *cdp, int node_id)
{
    (void)cdp;
    g_overlay_node_id = node_id;
}

/* ================================================================== */
/* Tiny JSON extractors (duplicated from the server — both are minimal) */
/* ================================================================== */
static long cdp_jint(const char *j, const char *key)
{
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *p = strstr(j, needle);
    if (!p)
        return -1;
    p += strlen(needle);
    while (*p && *p != ':')
        p++;
    if (*p != ':')
        return -1;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    return strtol(p, NULL, 10);
}
static double cdp_jnum(const char *j, const char *key)
{
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *p = strstr(j, needle);
    if (!p)
        return 0.0;
    p += strlen(needle);
    while (*p && *p != ':')
        p++;
    if (*p != ':')
        return 0.0;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    return strtod(p, NULL);
}
static int cdp_jstr(const char *j, const char *key, char *out, size_t cap)
{
    char needle[64];
    size_t m = snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *p = strstr(j, needle);
    if (!p)
        return -1;
    p += m;
    while (*p && *p != ':')
        p++;
    if (*p != ':')
        return -1;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '"')
        return -1;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < cap)
    {
        if (*p == '\\' && p[1])
        {
            char c = p[1];
            if (c == 'n') out[o++] = '\n';
            else if (c == 'r') out[o++] = '\r';
            else if (c == 't') out[o++] = '\t';
            else if (c == 'b') out[o++] = '\b';
            else if (c == 'f') out[o++] = '\f';
            else if (c == '"') out[o++] = '"';
            else if (c == '\\') out[o++] = '\\';
            else if (c == '/') out[o++] = '/';
            else out[o++] = c;
            p += 2;
        }
        else
            out[o++] = *p++;
    }
    out[o] = 0;
    return (int)o;
}
static int cdp_jbool(const char *j, const char *key)
{
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\":", key);
    const char *p = strstr(j, needle);
    if (!p)
        return -1; /* absent */
    p += strlen(needle);
    while (*p && isspace((unsigned char)*p))
        p++;
    if (!strncmp(p, "true", 4))
        return 1;
    if (!strncmp(p, "false", 5))
        return 0;
    return -1;
}

/* ================================================================== */
/* Dynamic buffer for building response JSON                           */
/* ================================================================== */
typedef struct
{
    char *p;
    size_t len, cap;
} DBuf;
static void db_ensure(DBuf *d, size_t extra)
{
    if (d->len + extra + 1 > d->cap)
    {
        size_t nc = d->cap ? d->cap : 256;
        while (nc < d->len + extra + 1)
            nc *= 2;
        char *np = (char *)realloc(d->p, nc);
        if (!np)
            return; /* best-effort; writes past cap are dropped below */
        d->p = np;
        d->cap = nc;
    }
}
static void db_putc(DBuf *d, char c) { db_ensure(d, 1); if (d->p) d->p[d->len++] = c; }
static void db_puts(DBuf *d, const char *s)
{
    size_t n = strlen(s);
    db_ensure(d, n);
    if (d->p)
    {
        memcpy(d->p + d->len, s, n);
        d->len += n;
    }
}
static void db_printf(DBuf *d, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0)
        db_puts(d, tmp);
}
/* append a JSON-escaped, quoted string */
static void db_json_str(DBuf *d, const char *s)
{
    if (!s)
    {
        db_puts(d, "\"\"");
        return;
    }
    db_putc(d, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    {
        if (*p == '"') db_puts(d, "\\\"");
        else if (*p == '\\') db_puts(d, "\\\\");
        else if (*p == '\n') db_puts(d, "\\n");
        else if (*p == '\r') db_puts(d, "\\r");
        else if (*p == '\t') db_puts(d, "\\t");
        else if (*p < 0x20) db_printf(d, "\\u%04x", *p);
        else db_putc(d, (char)*p);
    }
    db_putc(d, '"');
}

/* finish: NUL-terminate, return pointer (caller frees) */
static char *db_finish(DBuf *d)
{
    db_ensure(d, 1);
    if (d->p)
        d->p[d->len] = 0;
    return d->p ? d->p : NULL;
}

void mini_cdp_inspect_node(MiniCDP *cdp, int backend_node_id)
{
    if (backend_node_id <= 0)
        return;
    g_inspect_mode = 0;
    g_overlay_node_id = backend_node_id;

    DBuf d = {0};
    db_printf(&d, "{\"method\":\"Overlay.inspectNodeRequested\",\"params\":{\"backendNodeId\":%d}}", backend_node_id);
    char *s = db_finish(&d);
    if (s) { mini_cdp_broadcast(cdp, s, d.len); free(s); }

    DBuf d2 = {0};
    db_printf(&d2, "{\"method\":\"DOM.inspectNodeRequested\",\"params\":{\"backendNodeId\":%d}}", backend_node_id);
    char *s2 = db_finish(&d2);
    if (s2) { mini_cdp_broadcast(cdp, s2, d2.len); free(s2); }
}

/* ================================================================== */
/* Reply helpers                                                        */
/* ================================================================== */
static void reply_empty(MiniCDP *cdp, int ci, long id)
{
    if (id < 0)
        return; /* notification — no reply */
    char r[64];
    int n = snprintf(r, sizeof r, "{\"id\":%ld,\"result\":{}}", id);
    mini_cdp_send_client(cdp, ci, r, (size_t)n);
}
static void reply_buf(MiniCDP *cdp, int ci, const char *json, size_t len)
{
    mini_cdp_send_client(cdp, ci, json, len);
}
/* build + send {"id":N,"result":<frag>} */
static void reply_result(MiniCDP *cdp, int ci, long id, const char *frag)
{
    DBuf d = {0};
    db_printf(&d, "{\"id\":%ld,\"result\":", id);
    db_puts(&d, frag);
    db_putc(&d, '}');
    char *s = db_finish(&d);
    if (s)
    {
        reply_buf(cdp, ci, s, d.len);
        free(s);
    }
}

/* JSValue -> libc-malloc'd JSON string (compact). */
static char *js_value_json(JSContext *ctx, JSValueConst v)
{
    if (JS_IsUndefined(v))
        return strdup("undefined");
    if (JS_IsNull(v))
        return strdup("null");
    if (JS_IsFunction(ctx, v))
        return strdup("\"function\"");

    if (JS_HasException(ctx))
    {
        JSValue ex = JS_GetException(ctx);
        JS_FreeValue(ctx, ex);
    }

    JSValue s = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(s) || JS_IsUndefined(s))
    {
        JS_FreeValue(ctx, s);
        if (JS_HasException(ctx))
        {
            JSValue ex = JS_GetException(ctx);
            JS_FreeValue(ctx, ex);
        }
        const char *cs = JS_ToCString(ctx, v);
        if (JS_HasException(ctx))
        {
            JSValue ex = JS_GetException(ctx);
            JS_FreeValue(ctx, ex);
        }
        char *r = cs ? strdup(cs) : strdup("\"\"");
        if (cs)
            JS_FreeCString(ctx, cs);
        return r;
    }
    const char *cs = JS_ToCString(ctx, s);
    JS_FreeValue(ctx, s);
    char *r = cs ? strdup(cs) : strdup("");
    if (cs)
        JS_FreeCString(ctx, cs);
    return r;
}

/* Free the JSValue members of a descriptor returned by JS_GetOwnProperty
   (js_free_desc is private to quickjs.c, so we do it via the public API). */
static void free_desc(JSContext *ctx, JSPropertyDescriptor *desc, int got)
{
    if (got <= 0 || !desc)
        return;
    if (desc->flags & JS_PROP_HAS_VALUE)
        JS_FreeValue(ctx, desc->value);
    if (desc->flags & JS_PROP_HAS_GET)
        JS_FreeValue(ctx, desc->getter);
    if (desc->flags & JS_PROP_HAS_SET)
        JS_FreeValue(ctx, desc->setter);
}

/* ================================================================== */
/* RemoteObject table (id string -> JSValue)                           */
/* ================================================================== */
#define REMOTE_CAP 256
typedef struct
{
    char id[32];
    JSValue val;
    char group[32];
    int in_use;
} RemoteObj;
static RemoteObj g_remote[REMOTE_CAP];
static int g_remote_next = 1;

static const char *remote_add(JSContext *ctx, JSValueConst v, const char *group)
{
    for (int i = 0; i < REMOTE_CAP; i++)
    {
        if (!g_remote[i].in_use)
        {
            g_remote[i].in_use = 1;
            g_remote[i].val = JS_DupValue(ctx, v);
            snprintf(g_remote[i].id, sizeof g_remote[i].id, "{\"injectedScriptId\":1,\"id\":%d}", g_remote_next++);
            snprintf(g_remote[i].group, sizeof g_remote[i].group, "%s", group ? group : "");
            return g_remote[i].id;
        }
    }
    return "";
}
static RemoteObj *remote_find(const char *id)
{
    if (!id)
        return NULL;
    for (int i = 0; i < REMOTE_CAP; i++)
        if (g_remote[i].in_use && !strcmp(g_remote[i].id, id))
            return &g_remote[i];
    return NULL;
}
static void remote_release(JSContext *ctx, const char *id)
{
    RemoteObj *r = remote_find(id);
    if (!r)
        return;
    JS_FreeValue(ctx, r->val);
    r->in_use = 0;
}
static void remote_release_group(JSContext *ctx, const char *group)
{
    if (!group)
        return;
    for (int i = 0; i < REMOTE_CAP; i++)
        if (g_remote[i].in_use && !strcmp(g_remote[i].group, group))
        {
            JS_FreeValue(ctx, g_remote[i].val);
            g_remote[i].in_use = 0;
        }
}

/* Build a CDP RemoteObject JSON for a value into DBuf. */
static void remote_object(DBuf *d, JSContext *ctx, JSValueConst v, int return_by_value)
{
    db_putc(d, '{');
    int is_obj = JS_IsObject(v) && !JS_IsNull(v);
    if (JS_IsUndefined(v))
    {
        db_puts(d, "\"type\":\"undefined\"");
    }
    else if (JS_IsNull(v))
    {
        db_puts(d, "\"type\":\"object\",\"subtype\":\"null\",\"value\":null");
    }
    else if (is_obj && !return_by_value)
    {
        const char *oid = remote_add(ctx, v, "eval");
        db_puts(d, "\"type\":\"object\",\"subtype\":\"object\",\"className\":\"Object\",\"objectId\":");
        db_json_str(d, oid);
    }
    else
    {
        const char *type = "undefined";
        if (JS_IsBool(v))
            type = "boolean";
        else if (JS_IsNumber(v))
            type = "number";
        else if (JS_IsString(v))
            type = "string";
        else if (is_obj)
            type = "object";
        db_puts(d, "\"type\":\"");
        db_puts(d, type);
        db_puts(d, "\",\"value\":");
        char *json = js_value_json(ctx, v);
        db_puts(d, json);
        free(json);
        db_puts(d, ",\"description\":");
        if (is_obj)
        {
            char *desc = js_value_json(ctx, v);
            db_json_str(d, desc);
            free(desc);
        }
        else
            db_puts(d, "\"\"");
    }
    db_putc(d, '}');
}

/* ================================================================== */
/* Runtime domain                                                       */
/* ================================================================== */
static JSValue g_compiled = {0};
static int g_compiled_set = 0;

static void runtime_domain(MiniCDP *cdp, int ci, const char *msg, long id, const char *m)
{
    JSContext *ctx = NULL;
    MiniCDPHost *host = mini_cdp_host(cdp);
    if (host && host->bridge)
        ctx = mini_bridge_ctx((MiniBridge *)host->bridge);

    if (!strcmp(m, "enable"))
    {
        reply_empty(cdp, ci, id);
        /* The Console panel won't render consoleAPICalled events until a
           context exists. */
        static const char evt[] =
            "{\"method\":\"Runtime.executionContextCreated\","
            "\"params\":{\"context\":{\"id\":1,\"origin\":\"\","
            "\"name\":\"Tinyframework\",\"aux\":{\"isDefault\":true}}}}";
        mini_cdp_broadcast(cdp, evt, sizeof evt - 1);
        return;
    }
    if (!strcmp(m, "evaluate"))
    {
        if (!ctx)
        {
            reply_empty(cdp, ci, id);
            return;
        }
        /* expression can be as long as the whole message */
        size_t msglen = strlen(msg);
        char *expr = (char *)malloc(msglen + 1);
        if (!expr)
        {
            reply_empty(cdp, ci, id);
            return;
        }
        if (cdp_jstr(msg, "expression", expr, msglen + 1) < 0)
            expr[0] = 0;
        int rbv = cdp_jbool(msg, "returnByValue");
        if (rbv < 0)
            rbv = 0; /* default: return a handle for objects */

        if (JS_HasException(ctx))
        {
            JSValue old_ex = JS_GetException(ctx);
            JS_FreeValue(ctx, old_ex);
        }

        JSValue r = JS_Eval(ctx, expr, strlen(expr), "<console>",
                            JS_EVAL_TYPE_GLOBAL);
        free(expr);

        DBuf d = {0};
        db_printf(&d, "{\"id\":%ld,\"result\":{\"result\":", id);
        if (JS_IsException(r))
        {
            JSValue exc = JS_GetException(ctx);
            db_puts(&d, "{\"type\":\"undefined\"},\"exceptionDetails\":{");
            const char *cs = JS_ToCString(ctx, exc);
            db_puts(&d, "\"text\":");
            db_json_str(&d, cs ? cs : "exception");
            db_puts(&d, ",\"exception\":");
            remote_object(&d, ctx, exc, 1);
            db_puts(&d, ",\"lineNumber\":0,\"columnNumber\":0,\"scriptId\":\"0\"}");
            JS_FreeCString(ctx, cs);
            JS_FreeValue(ctx, exc);
        }
        else
        {
            remote_object(&d, ctx, r, rbv);
        }
        db_puts(&d, "}}");
        JS_FreeValue(ctx, r);
        char *s = db_finish(&d);
        if (s)
        {
            reply_buf(cdp, ci, s, d.len);
            free(s);
        }
        return;
    }
    if (!strcmp(m, "getProperties"))
    {
        if (!ctx)
        {
            reply_empty(cdp, ci, id);
            return;
        }
        char oid[128];
        if (cdp_jstr(msg, "objectId", oid, sizeof oid) < 0)
        {
            reply_result(cdp, ci, id, "{\"result\":[]}");
            return;
        }
        RemoteObj *ro = remote_find(oid);
        if (!ro || !JS_IsObject(ro->val))
        {
            reply_result(cdp, ci, id, "{\"result\":[]}");
            return;
        }
        int own = cdp_jbool(msg, "ownProperties");
        if (own < 0)
            own = 1;
        JSPropertyEnum *ptab = NULL;
        uint32_t plen = 0;
        DBuf d = {0};
        db_printf(&d, "{\"id\":%ld,\"result\":{\"result\":[", id);
        if (JS_GetOwnPropertyNames(ctx, &ptab, &plen, ro->val,
                                   JS_GPN_ENUM_ONLY) == 0)
        {
            for (uint32_t i = 0; i < plen; i++)
            {
                if (i)
                    db_putc(&d, ',');
                const char *nms = JS_AtomToCString(ctx, ptab[i].atom);
                db_puts(&d, "{\"name\":");
                db_json_str(&d, nms ? nms : "");
                if (nms)
                    JS_FreeCString(ctx, nms);
                JSPropertyDescriptor desc;
                int got = JS_GetOwnProperty(ctx, &desc, ro->val, ptab[i].atom);
                db_puts(&d, ",\"value\":");
                if (got > 0 && desc.flags & JS_PROP_HAS_VALUE)
                    remote_object(&d, ctx, desc.value, 1);
                else
                    db_puts(&d, "{\"type\":\"undefined\"}");
                db_puts(&d, ",\"writable\":");
                db_puts(&d, (got > 0 && (desc.flags & JS_PROP_HAS_WRITABLE) && desc.flags & JS_PROP_WRITABLE) ? "true" : "false");
                db_puts(&d, ",\"configurable\":");
                db_puts(&d, (got > 0 && (desc.flags & JS_PROP_HAS_CONFIGURABLE) && desc.flags & JS_PROP_CONFIGURABLE) ? "true" : "false");
                db_puts(&d, ",\"enumerable\":");
                db_puts(&d, (got > 0 && (desc.flags & JS_PROP_HAS_ENUMERABLE) && desc.flags & JS_PROP_ENUMERABLE) ? "true" : "false");
                db_puts(&d, ",\"isOwn\":true}");
                if (got > 0)
                    free_desc(ctx, &desc, got);
            }
        }
        if (ptab)
            JS_FreePropertyEnum(ctx, ptab, plen);
        db_puts(&d, "]}}");
        char *s = db_finish(&d);
        if (s)
        {
            reply_buf(cdp, ci, s, d.len);
            free(s);
        }
        return;
    }
    if (!strcmp(m, "globalLexicalScopeNames"))
    {
        DBuf d = {0};
        db_printf(&d, "{\"id\":%ld,\"result\":{\"names\":[", id);
        if (ctx)
        {
            JSValue global = JS_GetGlobalObject(ctx);
            JSPropertyEnum *ptab = NULL;
            uint32_t plen = 0;
            if (JS_GetOwnPropertyNames(ctx, &ptab, &plen, global, JS_GPN_STRING_MASK) == 0)
            {
                for (uint32_t i = 0; i < plen; i++)
                {
                    if (i) db_putc(&d, ',');
                    const char *nms = JS_AtomToCString(ctx, ptab[i].atom);
                    db_json_str(&d, nms ? nms : "");
                    if (nms) JS_FreeCString(ctx, nms);
                }
                JS_FreePropertyEnum(ctx, ptab, plen);
            }
            JS_FreeValue(ctx, global);
        }
        db_puts(&d, "]}}");
        char *s = db_finish(&d);
        if (s) { reply_buf(cdp, ci, s, d.len); free(s); }
        return;
    }
    if (!strcmp(m, "callFunctionOn"))
    {
        if (!ctx)
        {
            reply_result(cdp, ci, id, "{\"result\":{\"type\":\"undefined\"}}");
            return;
        }
        size_t msglen = strlen(msg);
        char *fndecl = (char *)malloc(msglen + 1);
        if (!fndecl)
        {
            reply_result(cdp, ci, id, "{\"result\":{\"type\":\"undefined\"}}");
            return;
        }
        if (cdp_jstr(msg, "functionDeclaration", fndecl, msglen + 1) < 0)
            fndecl[0] = 0;
        char oid[128] = {0};
        cdp_jstr(msg, "objectId", oid, sizeof oid);
        int rbv = cdp_jbool(msg, "returnByValue");
        if (rbv < 0) rbv = 0;

        JSValue this_obj = JS_UNDEFINED;
        if (oid[0])
        {
            RemoteObj *ro = remote_find(oid);
            if (ro) this_obj = JS_DupValue(ctx, ro->val);
        }
        if (JS_IsUndefined(this_obj))
            this_obj = JS_GetGlobalObject(ctx);

        size_t call_expr_len = strlen(fndecl) + 32;
        char *call_expr = (char *)malloc(call_expr_len);
        JSValue r;
        if (call_expr && fndecl[0])
        {
            snprintf(call_expr, call_expr_len, "(%s)", fndecl);
            if (JS_HasException(ctx))
            {
                JSValue old_ex = JS_GetException(ctx);
                JS_FreeValue(ctx, old_ex);
            }
            JSValue fn = JS_Eval(ctx, call_expr, strlen(call_expr), "<console>", JS_EVAL_TYPE_GLOBAL);
            free(call_expr);
            if (JS_IsFunction(ctx, fn))
            {
                r = JS_Call(ctx, fn, this_obj, 0, NULL);
                JS_FreeValue(ctx, fn);
            }
            else
            {
                r = fn;
            }
        }
        else
        {
            free(call_expr);
            r = JS_UNDEFINED;
        }
        free(fndecl);
        JS_FreeValue(ctx, this_obj);

        DBuf d = {0};
        db_printf(&d, "{\"id\":%ld,\"result\":{\"result\":", id);
        if (JS_IsException(r))
        {
            JSValue exc = JS_GetException(ctx);
            db_puts(&d, "{\"type\":\"undefined\"},\"exceptionDetails\":{");
            const char *cs = JS_ToCString(ctx, exc);
            db_puts(&d, "\"text\":");
            db_json_str(&d, cs ? cs : "exception");
            db_puts(&d, ",\"exception\":");
            remote_object(&d, ctx, exc, 1);
            db_puts(&d, ",\"lineNumber\":0,\"columnNumber\":0,\"scriptId\":\"0\"}");
            JS_FreeCString(ctx, cs);
            JS_FreeValue(ctx, exc);
        }
        else
        {
            remote_object(&d, ctx, r, rbv);
        }
        db_puts(&d, "}}");
        JS_FreeValue(ctx, r);
        char *s = db_finish(&d);
        if (s) { reply_buf(cdp, ci, s, d.len); free(s); }
        return;
    }
    if (!strcmp(m, "compileScript"))
    {
        if (!ctx)
        {
            reply_empty(cdp, ci, id);
            return;
        }
        size_t msglen = strlen(msg);
        char *expr = (char *)malloc(msglen + 1);
        if (!expr)
        {
            reply_empty(cdp, ci, id);
            return;
        }
        if (cdp_jstr(msg, "expression", expr, msglen + 1) < 0)
            expr[0] = 0;
        char url[128];
        if (cdp_jstr(msg, "sourceURL", url, sizeof url) < 0)
            url[0] = 0;
        JSValue r = JS_Eval(ctx, expr, strlen(expr), url[0] ? url : "<console>",
                            JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        free(expr);
        DBuf d = {0};
        db_printf(&d, "{\"id\":%ld,\"result\":{", id);
        if (JS_IsException(r))
        {
            JSValue exc = JS_GetException(ctx);
            const char *cs = JS_ToCString(ctx, exc);
            db_puts(&d, "\"exceptionDetails\":{\"text\":");
            db_json_str(&d, cs ? cs : "compile error");
            db_puts(&d, ",\"scriptId\":\"0\"}}");
            JS_FreeCString(ctx, cs);
            JS_FreeValue(ctx, exc);
        }
        else
        {
            if (g_compiled_set)
                JS_FreeValue(ctx, g_compiled);
            g_compiled = r;
            g_compiled_set = 1;
            db_puts(&d, "\"scriptId\":\"1\"}}");
        }
        char *s = db_finish(&d);
        if (s)
        {
            reply_buf(cdp, ci, s, d.len);
            free(s);
        }
        return;
    }
    if (!strcmp(m, "runScript"))
    {
        if (!ctx)
        {
            reply_result(cdp, ci, id, "{\"result\":{\"type\":\"undefined\"}}");
            return;
        }
        char sid[32] = {0};
        cdp_jstr(msg, "scriptId", sid, sizeof(sid));
        int rbv = cdp_jbool(msg, "returnByValue");
        if (rbv < 0) rbv = 0;

        JSValue fn = g_compiled;
        g_compiled_set = 0;
        JSValue r = JS_EvalFunction(ctx, fn);
        DBuf d = {0};
        db_printf(&d, "{\"id\":%ld,\"result\":{\"result\":", id);
        if (JS_IsException(r))
        {
            JSValue exc = JS_GetException(ctx);
            db_puts(&d, "{\"type\":\"undefined\"},\"exceptionDetails\":{");
            const char *cs = JS_ToCString(ctx, exc);
            db_puts(&d, "\"text\":");
            db_json_str(&d, cs ? cs : "exception");
            db_puts(&d, ",\"exception\":");
            remote_object(&d, ctx, exc, 1);
            db_puts(&d, ",\"lineNumber\":0,\"columnNumber\":0,\"scriptId\":\"1\"}");
            JS_FreeCString(ctx, cs);
            JS_FreeValue(ctx, exc);
        }
        else
        {
            remote_object(&d, ctx, r, rbv);
        }
        db_puts(&d, "}}");
        JS_FreeValue(ctx, r);
        char *s = db_finish(&d);
        if (s)
        {
            reply_buf(cdp, ci, s, d.len);
            free(s);
        }
        return;
    }
    if (!strcmp(m, "releaseObject"))
    {
        char oid[128];
        if (cdp_jstr(msg, "objectId", oid, sizeof oid) >= 0)
            remote_release(ctx, oid);
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "releaseObjectGroup"))
    {
        char grp[64];
        if (cdp_jstr(msg, "objectGroup", grp, sizeof grp) >= 0)
            remote_release_group(ctx, grp);
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "getHeapUsage"))
    {
        JSMemoryUsage mu;
        JSRuntime *rt = ctx ? JS_GetRuntime(ctx) : NULL;
        memset(&mu, 0, sizeof mu);
        if (rt)
            JS_ComputeMemoryUsage(rt, &mu);
        char r[256];
        int n = snprintf(r, sizeof r,
                         "{\"id\":%ld,\"result\":{\"usedSize\":%lld,\"totalSize\":%lld}}",
                         id, (long long)mu.memory_used_size, (long long)mu.malloc_size);
        reply_buf(cdp, ci, r, (size_t)n);
        return;
    }
    /* discardConsoleEntries / addBinding / getIsolateId / etc. */
    reply_empty(cdp, ci, id);
}

/* ================================================================== */
/* Send wrappers                                                        */
/* ================================================================== */
static void send_wrapped(MiniCDP *cdp, int ci, long id, const char *key, const char *buf)
{
    DBuf d = {0};
    db_printf(&d, "{\"id\":%ld,\"result\":{\"%s\":", id, key);
    db_puts(&d, buf);
    db_puts(&d, "}}");
    char *s = db_finish(&d);
    if (s) { reply_buf(cdp, ci, s, d.len); free(s); }
}
static void send_raw(MiniCDP *cdp, int ci, long id, const char *buf)
{
    DBuf d = {0};
    db_printf(&d, "{\"id\":%ld,\"result\":", id);
    db_puts(&d, buf);
    db_putc(&d, '}');
    char *s = db_finish(&d);
    if (s) { reply_buf(cdp, ci, s, d.len); free(s); }
}

/* Resolve a CDP nodeId to its MiniNode (assigning stable ids first). */
static struct MiniNode *dom_resolve(MiniCDPHost *host, const char *msg)
{
    if (!host || !host->doc)
        return NULL;
    long nid = cdp_jint(msg, "nodeId");
    if (nid <= 0)
        return NULL;
    mini_dom_assign_node_ids((MiniDocument *)host->doc);
    return mini_dom_node_by_id((MiniDocument *)host->doc, (int)nid);
}

/* ================================================================== */
/* DOM domain (Elements panel)                                          */
/* ================================================================== */
/* Live-mutation callback (set on DOM.enable): broadcasts DOM events
   when JS appends/removes children or sets attributes, so the Elements
   panel refreshes without a manual reload. */
static void cdp_dom_mutation(MiniDocument *doc, const char *evt,
                             struct MiniNode *parent, struct MiniNode *node,
                             const char *name, const char *value, void *ud)
{
    MiniCDP *cdp = (MiniCDP *)ud;
    if (!cdp || !doc || !node)
        return;
    mini_dom_assign_node_ids(doc); /* assign ids to any freshly-added node */
    DBuf d = {0};
    if (!strcmp(evt, "childNodeInserted"))
    {
        char *buf = (char *)malloc(CDP_DOM_BUF);
        if (!buf) return;
        mini_dom_describe_node(node, buf, CDP_DOM_BUF);
        db_puts(&d, "{\"method\":\"DOM.childNodeInserted\",\"params\":{\"parentNodeId\":");
        db_printf(&d, "%d", parent ? mini_dom_node_id(parent) : 0);
        db_puts(&d, ",\"previousNodeId\":0,\"node\":");
        db_puts(&d, buf);
        db_puts(&d, "}}");
        free(buf);
    }
    else if (!strcmp(evt, "childNodeRemoved"))
    {
        db_puts(&d, "{\"method\":\"DOM.childNodeRemoved\",\"params\":{\"parentNodeId\":");
        db_printf(&d, "%d", parent ? mini_dom_node_id(parent) : 0);
        db_puts(&d, ",\"nodeId\":");
        db_printf(&d, "%d", mini_dom_node_id(node));
        db_puts(&d, "}}");
    }
    else if (!strcmp(evt, "attributeModified") && name)
    {
        db_puts(&d, "{\"method\":\"DOM.attributeModified\",\"params\":{\"nodeId\":");
        db_printf(&d, "%d", mini_dom_node_id(node));
        db_puts(&d, ",\"name\":");
        db_json_str(&d, name);
        db_puts(&d, ",\"value\":");
        db_json_str(&d, value ? value : "");
        db_puts(&d, "}}");
    }
    else
    {
        return;
    }
    char *s = db_finish(&d);
    if (s) { mini_cdp_broadcast(cdp, s, d.len); free(s); }
}

static void dom_domain(MiniCDP *cdp, int ci, const char *msg, long id, const char *m)
{
    MiniCDPHost *host = mini_cdp_host(cdp);
    if (!strcmp(m, "enable"))
    {
        if (host && host->doc)
            mini_dom_set_mutation_hook((MiniDocument *)host->doc, cdp_dom_mutation, cdp);
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "getDocument"))
    {
        if (!host || !host->doc) { reply_empty(cdp, ci, id); return; }
        mini_dom_assign_node_ids((MiniDocument *)host->doc);
        char *buf = (char *)malloc(CDP_DOM_BUF);
        if (!buf) { reply_empty(cdp, ci, id); return; }
        mini_dom_serialize_cdp((MiniDocument *)host->doc, buf, CDP_DOM_BUF);
        send_wrapped(cdp, ci, id, "root", buf);
        free(buf);
        return;
    }
    if (!strcmp(m, "describeNode"))
    {
        struct MiniNode *n = dom_resolve(host, msg);
        char *buf = (char *)malloc(CDP_DOM_BUF);
        if (!buf) { reply_empty(cdp, ci, id); return; }
        if (n) mini_dom_describe_node(n, buf, CDP_DOM_BUF); else buf[0] = 0;
        send_wrapped(cdp, ci, id, "node", buf);
        free(buf);
        return;
    }
    if (!strcmp(m, "setInspectMode"))
    {
        char mode[64] = {0};
        cdp_jstr(msg, "mode", mode, sizeof mode);
        if (!strcmp(mode, "searchForNode") || !strcmp(mode, "searchForUAShadowDOM"))
            g_inspect_mode = 1;
        else
            g_inspect_mode = 0;
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "highlightNode"))
    {
        int nid = (int)cdp_jint(msg, "nodeId");
        if (nid <= 0) nid = (int)cdp_jint(msg, "backendNodeId");
        g_overlay_node_id = nid;
        if (host && host->doc) ((MiniDocument *)host->doc)->dirty = 1;
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "hideHighlight"))
    {
        g_overlay_node_id = 0;
        if (host && host->doc) ((MiniDocument *)host->doc)->dirty = 1;
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "requestChildNodes") || !strcmp(m, "requestNode") ||
        !strcmp(m, "getChildNodes") || !strcmp(m, "pushNodesByBackendIdsToFrontend") ||
        !strcmp(m, "setInspectedNode") || !strcmp(m, "redo") || !strcmp(m, "undo") ||
        !strcmp(m, "markUndoableState") || !strcmp(m, "focus") ||
        !strcmp(m, "setNodeName") || !strcmp(m, "setNodeStackTrace") ||
        !strcmp(m, "moveTo") || !strcmp(m, "setChildNodes"))
    {
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "querySelector") || !strcmp(m, "querySelectorAll"))
    {
        int all = !strcmp(m, "querySelectorAll");
        if (!host || !host->doc) { reply_empty(cdp, ci, id); return; }
        char sel[512];
        if (cdp_jstr(msg, "selector", sel, sizeof sel) < 0) sel[0] = 0;
        mini_dom_assign_node_ids((MiniDocument *)host->doc);
        if (all)
        {
            struct MiniNode *out[256];
            int n = mini_dom_query_selector_all((MiniDocument *)host->doc, sel, out, 256);
            DBuf d = {0};
            db_printf(&d, "{\"id\":%ld,\"result\":{\"nodeIds\":[", id);
            for (int i = 0; i < n; i++)
            {
                if (i) db_putc(&d, ',');
                db_printf(&d, "%d", mini_dom_node_id(out[i]));
            }
            db_puts(&d, "]}}");
            char *s = db_finish(&d);
            if (s) { reply_buf(cdp, ci, s, d.len); free(s); }
        }
        else
        {
            struct MiniNode *n = mini_dom_query_selector((MiniDocument *)host->doc, sel);
            long nid = n ? mini_dom_node_id(n) : 0;
            char r[128];
            int k = snprintf(r, sizeof r, "{\"id\":%ld,\"result\":{\"nodeId\":%ld}}", id, nid);
            reply_buf(cdp, ci, r, (size_t)k);
        }
        return;
    }
    if (!strcmp(m, "getBoxModel") || !strcmp(m, "getLayoutMetrics"))
    {
        struct MiniNode *n = dom_resolve(host, msg);
        char buf[1024];
        if (n) mini_dom_box_model(n, buf, sizeof buf);
        else snprintf(buf, sizeof buf, "{}");
        send_raw(cdp, ci, id, buf);
        return;
    }
    if (!strcmp(m, "getOuterHTML") || !strcmp(m, "getInnerHTML"))
    {
        int inner = !strcmp(m, "getInnerHTML");
        struct MiniNode *n = dom_resolve(host, msg);
        char *buf = (char *)malloc(CDP_DOM_BUF);
        if (!buf) { reply_empty(cdp, ci, id); return; }
        if (n) mini_dom_outer_html(n, inner, buf, CDP_DOM_BUF); else buf[0] = 0;
        DBuf d = {0};
        db_printf(&d, "{\"id\":%ld,\"result\":{\"outerHTML\":", id);
        db_json_str(&d, buf);
        db_puts(&d, "}}");
        free(buf);
        char *s = db_finish(&d);
        if (s) { reply_buf(cdp, ci, s, d.len); free(s); }
        return;
    }
    if (!strcmp(m, "setAttributeValue") || !strcmp(m, "setAttributeValueAsText"))
    {
        struct MiniNode *n = dom_resolve(host, msg);
        char name[128], value[4096];
        if (cdp_jstr(msg, "name", name, sizeof name) < 0) name[0] = 0;
        if (cdp_jstr(msg, "value", value, sizeof value) < 0) value[0] = 0;
        if (n) mini_node_set_attribute(n, name, value);
        DBuf d = {0};
        db_puts(&d, "{\"method\":\"DOM.attributeModified\",\"params\":{\"nodeId\":");
        db_printf(&d, "%d", n ? mini_dom_node_id(n) : 0);
        db_puts(&d, ",\"name\":");
        db_json_str(&d, name);
        db_puts(&d, ",\"value\":");
        db_json_str(&d, value);
        db_puts(&d, "}}");
        char *s = db_finish(&d);
        if (s) { mini_cdp_broadcast(cdp, s, d.len); free(s); }
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "setNodeValue"))
    {
        struct MiniNode *n = dom_resolve(host, msg);
        char text[8192];
        if (cdp_jstr(msg, "text", text, sizeof text) < 0) text[0] = 0;
        if (n) mini_node_set_text(n, text);
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "removeNode"))
    {
        struct MiniNode *n = dom_resolve(host, msg);
        if (n && n->parent)
        {
            int pid = mini_dom_node_id(n->parent), nid = mini_dom_node_id(n);
            mini_node_remove_child(n->parent, n);
            DBuf d = {0};
            db_puts(&d, "{\"method\":\"DOM.childNodeRemoved\",\"params\":{\"parentNodeId\":");
            db_printf(&d, "%d", pid);
            db_puts(&d, ",\"nodeId\":");
            db_printf(&d, "%d", nid);
            db_puts(&d, "}}");
            char *s = db_finish(&d);
            if (s) { mini_cdp_broadcast(cdp, s, d.len); free(s); }
        }
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "highlightNode") || !strcmp(m, "highlightNodeWithStencil"))
    {
        g_overlay_node_id = (int)cdp_jint(msg, "nodeId");
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "hideHighlight")) { g_overlay_node_id = 0; reply_empty(cdp, ci, id); return; }
    if (!strcmp(m, "performSearch"))
    {
        char r[96];
        int k = snprintf(r, sizeof r, "{\"id\":%ld,\"result\":{\"searchId\":\"0\",\"resultCount\":0}}", id);
        reply_buf(cdp, ci, r, (size_t)k);
        return;
    }
    if (!strcmp(m, "getSearchResults") || !strcmp(m, "discardSearchResults")) { reply_empty(cdp, ci, id); return; }
    reply_empty(cdp, ci, id);
}

/* ================================================================== */
/* CSS domain (Styles / Computed panel)                                */
/* ================================================================== */
static void ser_css_properties(DBuf *d, const char *css_text)
{
    db_puts(d, "[");
    if (css_text && css_text[0])
    {
        const char *p = css_text;
        int count = 0;
        while (*p)
        {
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
            if (!*p) break;
            const char *col = strchr(p, ':');
            if (!col) break;
            char name[128] = {0};
            size_t nlen = (size_t)(col - p);
            if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
            memcpy(name, p, nlen);
            while (nlen > 0 && (name[nlen-1] == ' ' || name[nlen-1] == '\t')) name[--nlen] = 0;

            const char *vstart = col + 1;
            while (*vstart == ' ' || *vstart == '\t') vstart++;
            const char *semi = strchr(vstart, ';');
            const char *vend = semi ? semi : (vstart + strlen(vstart));
            char val[512] = {0};
            size_t vlen = (size_t)(vend - vstart);
            if (vlen >= sizeof(val)) vlen = sizeof(val) - 1;
            memcpy(val, vstart, vlen);
            while (vlen > 0 && (val[vlen-1] == ' ' || val[vlen-1] == '\t')) val[--vlen] = 0;

            if (count > 0) db_putc(d, ',');
            db_puts(d, "{\"name\":");
            db_json_str(d, name);
            db_puts(d, ",\"value\":");
            db_json_str(d, val);
            db_puts(d, ",\"important\":false,\"implicit\":false,\"text\":");
            char prop_text[640];
            snprintf(prop_text, sizeof(prop_text), "%s: %s;", name, val);
            db_json_str(d, prop_text);
            db_puts(d, "}");
            count++;

            p = semi ? semi + 1 : vend;
        }
    }
    db_puts(d, "]");
}

static void ser_matched_rules_for_node(DBuf *d, struct MiniNode *n, struct MiniDocument *doc)
{
    db_puts(d, "[");
    if (!n || !doc)
    {
        db_puts(d, "]");
        return;
    }
    const char *css = mini_dom_get_stylesheet(doc);
    if (!css || !css[0])
    {
        db_puts(d, "]");
        return;
    }

    int rule_count = 0;
    const char *p = css;
    int rule_id = 1;
    while (*p)
    {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (p[0] == '/' && p[1] == '*')
        {
            const char *cend = strstr(p + 2, "*/");
            if (cend) p = cend + 2;
            else break;
            continue;
        }
        if (*p == '@')
        {
            const char *semi = strchr(p, ';');
            const char *brace = strchr(p, '{');
            if (brace && (!semi || brace < semi))
            {
                p = brace + 1;
                int depth = 1;
                while (*p && depth > 0)
                {
                    if (*p == '{') depth++;
                    else if (*p == '}') depth--;
                    p++;
                }
            }
            else if (semi)
            {
                p = semi + 1;
            }
            else break;
            continue;
        }

        const char *brace = strchr(p, '{');
        if (!brace) break;
        char sel_buf[512] = {0};
        size_t slen = (size_t)(brace - p);
        if (slen >= sizeof(sel_buf)) slen = sizeof(sel_buf) - 1;
        memcpy(sel_buf, p, slen);
        while (slen > 0 && isspace((unsigned char)sel_buf[slen-1])) sel_buf[--slen] = 0;

        const char *bstart = brace + 1;
        const char *bend = strchr(bstart, '}');
        if (!bend) break;
        char decl_buf[4096] = {0};
        size_t dlen = (size_t)(bend - bstart);
        if (dlen >= sizeof(decl_buf)) dlen = sizeof(decl_buf) - 1;
        memcpy(decl_buf, bstart, dlen);

        char sel_clean[512] = {0};
        size_t c_idx = 0;
        for (size_t s_i = 0; s_i < slen; )
        {
            if (sel_buf[s_i] == '/' && sel_buf[s_i+1] == '*')
            {
                char *c_end = strstr(&sel_buf[s_i+2], "*/");
                if (c_end) s_i = (size_t)(c_end - sel_buf) + 2;
                else break;
            }
            else
            {
                if (c_idx < sizeof(sel_clean) - 1)
                    sel_clean[c_idx++] = sel_buf[s_i];
                s_i++;
            }
        }
        sel_clean[c_idx] = 0;
        char *sc = sel_clean;
        while (*sc && isspace((unsigned char)*sc)) sc++;
        char *sc_end = sc + strlen(sc);
        while (sc_end > sc && isspace((unsigned char)sc_end[-1])) *--sc_end = 0;

        char sel_copy[512];
        strncpy(sel_copy, sc, sizeof(sel_copy)-1);
        sel_copy[sizeof(sel_copy)-1] = 0;
        char *saveptr = NULL;
        char *single_sel = strtok_r(sel_copy, ",", &saveptr);
        int matched = 0;
        int match_idx = 0;
        int cur_idx = 0;
        while (single_sel)
        {
            while (*single_sel && isspace((unsigned char)*single_sel)) single_sel++;
            char *send = single_sel + strlen(single_sel);
            while (send > single_sel && isspace((unsigned char)send[-1])) *--send = 0;

            if (*single_sel && mini_dom_matches_selector(n, single_sel))
            {
                matched = 1;
                match_idx = cur_idx;
                break;
            }
            cur_idx++;
            single_sel = strtok_r(NULL, ",", &saveptr);
        }

        if (matched)
        {
            if (rule_count > 0) db_putc(d, ',');
            db_puts(d, "{\"rule\":{\"styleSheetId\":\"style_1\",\"origin\":\"regular\",");
            db_puts(d, "\"selectorList\":{\"selectors\":[{\"text\":");
            db_json_str(d, sel_buf);
            db_puts(d, "}],\"text\":");
            db_json_str(d, sel_buf);
            db_puts(d, "},\"style\":{\"styleSheetId\":\"style_1\",\"cssText\":");
            db_json_str(d, decl_buf);
            db_puts(d, ",\"shorthandEntries\":[],\"cssProperties\":");
            ser_css_properties(d, decl_buf);
            db_printf(d, "}},\"matchingSelectors\":[%d]}", match_idx);
            rule_count++;
        }

        p = bend + 1;
        rule_id++;
    }
    db_puts(d, "]");
}

static void ser_inherited_styles(DBuf *d, struct MiniNode *n, struct MiniDocument *doc)
{
    db_puts(d, "[");
    if (!n || !doc)
    {
        db_puts(d, "]");
        return;
    }
    int parent_count = 0;
    for (struct MiniNode *p = n->parent; p; p = p->parent)
    {
        if (parent_count > 0) db_putc(d, ',');
        char p_inline[4096] = {0};
        mini_dom_inline_style(p, p_inline, sizeof p_inline);
        db_puts(d, "{\"inlineStyle\":{\"styleSheetId\":\"inline\",\"cssText\":");
        db_json_str(d, p_inline);
        db_puts(d, ",\"shorthandEntries\":[],\"cssProperties\":");
        ser_css_properties(d, p_inline);
        db_puts(d, "},\"matchedCSSRules\":");
        ser_matched_rules_for_node(d, p, doc);
        db_puts(d, "}");
        parent_count++;
    }
    db_puts(d, "]");
}

static void css_domain(MiniCDP *cdp, int ci, const char *msg, long id, const char *m)
{
    MiniCDPHost *host = mini_cdp_host(cdp);
    if (!strcmp(m, "enable") || !strcmp(m, "startRuleUsageTracking") ||
        !strcmp(m, "stopRuleUsageTracking") || !strcmp(m, "takeStyleSheetText"))
    {
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "getComputedStyle") || !strcmp(m, "getComputedStyleForNode"))
    {
        struct MiniNode *n = dom_resolve(host, msg);
        char buf[8192];
        if (n) mini_dom_computed_style(n, buf, sizeof buf);
        else snprintf(buf, sizeof buf, "{\"computedStyle\":[]}");
        send_raw(cdp, ci, id, buf);
        return;
    }
    if (!strcmp(m, "getMatchedStylesForNode") || !strcmp(m, "getInlineStylesForNode") || !strcmp(m, "getInlineStyles"))
    {
        struct MiniNode *n = dom_resolve(host, msg);
        char buf[4096] = {0};
        if (n) mini_dom_inline_style(n, buf, sizeof buf);
        DBuf d = {0};
        db_printf(&d, "{\"id\":%ld,\"result\":{\"inlineStyle\":{\"styleSheetId\":\"inline\",\"cssText\":", id);
        db_json_str(&d, buf);
        db_puts(&d, ",\"shorthandEntries\":[],\"cssProperties\":");
        ser_css_properties(&d, buf);
        db_puts(&d, "},\"matchedCSSRules\":");
        ser_matched_rules_for_node(&d, n, host ? (MiniDocument *)host->doc : NULL);
        db_puts(&d, ",\"pseudoElements\":[],\"inherited\":");
        ser_inherited_styles(&d, n, host ? (MiniDocument *)host->doc : NULL);
        db_puts(&d, "}}");
        char *s = db_finish(&d);
        if (s) { reply_buf(cdp, ci, s, d.len); free(s); }
        return;
    }
    if (!strcmp(m, "setStyleTexts"))
    {
        char text[4096] = {0};
        cdp_jstr(msg, "text", text, sizeof(text));
        struct MiniNode *n = dom_resolve(host, msg);
        if (!n && g_overlay_node_id > 0 && host && host->doc)
            n = mini_dom_node_by_id((MiniDocument *)host->doc, g_overlay_node_id);
        if (n && text[0])
        {
            mini_node_set_attribute(n, "style", text);
            if (host && host->doc)
                mini_dom_restyle((MiniDocument *)host->doc);
        }
        DBuf d = {0};
        db_printf(&d, "{\"id\":%ld,\"result\":{\"styles\":[{\"styleSheetId\":\"inline\",\"cssText\":", id);
        db_json_str(&d, text);
        db_puts(&d, ",\"shorthandEntries\":[],\"cssProperties\":");
        ser_css_properties(&d, text);
        db_puts(&d, "}]}}");
        char *s = db_finish(&d);
        if (s) { reply_buf(cdp, ci, s, d.len); free(s); }
        return;
    }
    if (!strcmp(m, "setEffectivePropertyValueForNode"))
    {
        struct MiniNode *n = dom_resolve(host, msg);
        char pname[128] = {0}, pval[512] = {0};
        cdp_jstr(msg, "propertyName", pname, sizeof(pname));
        cdp_jstr(msg, "value", pval, sizeof(pval));
        if (n && pname[0])
        {
            char cur[4096] = {0};
            mini_dom_inline_style(n, cur, sizeof(cur));
            char updated[4300];
            snprintf(updated, sizeof(updated), "%s; %s: %s;", cur, pname, pval);
            mini_node_set_attribute(n, "style", updated);
            if (host && host->doc)
                mini_dom_restyle((MiniDocument *)host->doc);
        }
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "getLayoutMetrics") || !strcmp(m, "getBoxModel"))
    {
        struct MiniNode *n = dom_resolve(host, msg);
        char buf[1024];
        if (n) mini_dom_box_model(n, buf, sizeof buf);
        else snprintf(buf, sizeof buf, "{}");
        send_raw(cdp, ci, id, buf);
        return;
    }
    if (!strcmp(m, "getLayoutStylesheets")) { reply_result(cdp, ci, id, "{\"headers\":[]}"); return; }
    reply_empty(cdp, ci, id);
}

/* ================================================================== */
/* Overlay domain (highlight; draw wired in P1.7)                      */
/* ================================================================== */
static void overlay_domain(MiniCDP *cdp, int ci, const char *msg, long id, const char *m)
{
    if (!strcmp(m, "enable") || !strcmp(m, "setShowViewportSizeOnResize") ||
        !strcmp(m, "setShowPaintRects") || !strcmp(m, "setShowDebugBorders") ||
        !strcmp(m, "setShowLayoutShiftRegions") || !strcmp(m, "setShowFPSCounter") ||
        !strcmp(m, "setShowWebVitals") || !strcmp(m, "setPausedInDebuggerMessage"))
    {
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "highlightNode") || !strcmp(m, "highlightNodeWithStencil"))
    {
        int nid = (int)cdp_jint(msg, "nodeId");
        if (nid <= 0)
            nid = (int)cdp_jint(msg, "backendNodeId");
        g_overlay_node_id = nid;
        MiniCDPHost *host = mini_cdp_host(cdp);
        if (host && host->doc)
            ((MiniDocument *)host->doc)->dirty = 1;
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "highlightFrame")) { reply_empty(cdp, ci, id); return; }
    if (!strcmp(m, "hideHighlight"))
    {
        g_overlay_node_id = 0;
        MiniCDPHost *host = mini_cdp_host(cdp);
        if (host && host->doc)
            ((MiniDocument *)host->doc)->dirty = 1;
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "setInspectMode"))
    {
        char mode[64] = {0};
        cdp_jstr(msg, "mode", mode, sizeof mode);
        if (!strcmp(mode, "searchForNode") || !strcmp(mode, "searchForUAShadowDOM"))
            g_inspect_mode = 1;
        else
            g_inspect_mode = 0;
        reply_empty(cdp, ci, id);
        return;
    }
    reply_empty(cdp, ci, id);
}

/* Renderer hook (called each frame): box of the highlighted node. */
int mini_cdp_overlay_box_ex(MiniCDP *cdp,
                           float *x, float *y, float *w, float *h,
                           float pad[4], float marg[4],
                           char *tag_buf, size_t tag_cap,
                           char *id_buf, size_t id_cap,
                           char *class_buf, size_t class_cap)
{
    if (g_overlay_node_id <= 0)
        return 0;
    MiniCDPHost *host = mini_cdp_host(cdp);
    if (!host || !host->doc)
        return 0;
    mini_dom_assign_node_ids((MiniDocument *)host->doc);
    struct MiniNode *n = mini_dom_node_by_id((MiniDocument *)host->doc, g_overlay_node_id);
    if (!n)
        return 0;
    if (x) *x = n->style.abs_x;
    if (y) *y = n->style.abs_y;
    if (w) *w = n->style.w;
    if (h) *h = n->style.h;

    if (pad)
    {
        pad[0] = n->style.padding[0];
        pad[1] = n->style.padding[1];
        pad[2] = n->style.padding[2];
        pad[3] = n->style.padding[3];
    }
    if (marg)
    {
        marg[0] = n->style.margin[0];
        marg[1] = n->style.margin[1];
        marg[2] = n->style.margin[2];
        marg[3] = n->style.margin[3];
    }
    if (tag_buf && tag_cap > 0)
    {
        const char *t = n->tag ? n->tag : (n->type == MN_TEXT_NODE ? "text" : "div");
        snprintf(tag_buf, tag_cap, "%s", t);
    }
    if (id_buf && id_cap > 0)
    {
        const char *id_val = mini_node_get_attribute(n, "id");
        snprintf(id_buf, id_cap, "%s", id_val ? id_val : "");
    }
    if (class_buf && class_cap > 0)
    {
        const char *cls_val = mini_node_get_attribute(n, "class");
        snprintf(class_buf, class_cap, "%s", cls_val ? cls_val : "");
    }
    return 1;
}

int mini_cdp_overlay_box(MiniCDP *cdp, float *x, float *y, float *w, float *h)
{
    return mini_cdp_overlay_box_ex(cdp, x, y, w, h, NULL, NULL, NULL, 0, NULL, 0, NULL, 0);
}

int mini_cdp_has_overlay(MiniCDP *cdp)
{
    (void)cdp;
    return g_overlay_node_id > 0;
}

/* ================================================================== */
/* Minimal enable responses so the other panels initialise.            */
/* ================================================================== */
static void page_enable(MiniCDP *cdp, int ci, long id)
{
    reply_empty(cdp, ci, id);
    char evt[256];
    int n = snprintf(evt, sizeof evt,
                     "{\"method\":\"Page.frameNavigated\","
                     "\"params\":{\"frame\":{\"id\":\"1\",\"url\":\"about:blank\","
                     "\"name\":\"\",\"mimeType\":\"text/html\","
                     "\"securityOrigin\":\"\",\"loaderId\":\"1\"}}}");
    mini_cdp_broadcast(cdp, evt, (size_t)n);
}

/* Base64 (for screenshot data). Returns a malloc'd NUL-terminated string. */
static char *b64_encode(const uint8_t *p, size_t n)
{
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t olen = ((n + 2) / 3) * 4;
    char *o = (char *)malloc(olen + 1);
    if (!o)
        return NULL;
    size_t i, j = 0;
    for (i = 0; i + 2 < n; i += 3)
    {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | p[i + 2];
        o[j++] = T[(v >> 18) & 63];
        o[j++] = T[(v >> 12) & 63];
        o[j++] = T[(v >> 6) & 63];
        o[j++] = T[v & 63];
    }
    if (i < n)
    {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < n)
            v |= (uint32_t)p[i + 1] << 8;
        o[j++] = T[(v >> 18) & 63];
        o[j++] = T[(v >> 12) & 63];
        o[j++] = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        o[j++] = '=';
    }
    o[j] = 0;
    return o;
}

/* Page domain (screenshot + frame tree). */
static void page_domain(MiniCDP *cdp, int ci, const char *msg, long id, const char *m)
{
    if (!strcmp(m, "enable")) { page_enable(cdp, ci, id); return; }
    if (!strcmp(m, "captureScreenshot"))
    {
        MiniCDPHost *host = mini_cdp_host(cdp);
        if (!host || !host->renderer) { reply_empty(cdp, ci, id); return; }
        uint8_t *png = NULL;
        size_t plen = 0;
        if (mini_renderer_screenshot_png((MiniRenderer *)host->renderer,
                                         &png, &plen) != 0 || !png)
        {
            reply_empty(cdp, ci, id);
            return;
        }
        char *b64 = b64_encode(png, plen);
        free(png);
        if (!b64) { reply_empty(cdp, ci, id); return; }
        DBuf d = {0};
        db_printf(&d, "{\"id\":%ld,\"result\":{\"data\":\"", id);
        db_puts(&d, b64);
        db_puts(&d, "\"}}");
        free(b64);
        char *s = db_finish(&d);
        if (s) { reply_buf(cdp, ci, s, d.len); free(s); }
        return;
    }
    if (!strcmp(m, "getFrameTree"))
    {
        MiniCDPHost *host = mini_cdp_host(cdp);
        const char *u = (host && host->url && host->url[0]) ? host->url : "about:blank";
        DBuf d = {0};
        db_puts(&d, "{\"frame\":{\"id\":\"1\",\"url\":");
        db_json_str(&d, u);
        db_puts(&d, ",\"name\":\"\",\"mimeType\":\"text/html\",\"securityOrigin\":\"\",\"loaderId\":\"1\"},\"childFrames\":[]}");
        char *s = db_finish(&d);
        if (s) { reply_result(cdp, ci, id, s); free(s); }
        return;
    }
    if (!strcmp(m, "getResourceTree"))
    {
        MiniCDPHost *host = mini_cdp_host(cdp);
        const char *u = (host && host->url && host->url[0]) ? host->url : "about:blank";
        DBuf d = {0};
        db_puts(&d, "{\"frameTree\":{\"frame\":{\"id\":\"1\",\"url\":");
        db_json_str(&d, u);
        db_puts(&d, ",\"mimeType\":\"text/html\",\"loaderId\":\"1\"},\"childFrames\":[],\"resources\":[{\"url\":");
        db_json_str(&d, u);
        db_puts(&d, ",\"type\":\"Document\",\"mimeType\":\"text/html\"}]}}");
        char *s = db_finish(&d);
        if (s) { reply_result(cdp, ci, id, s); free(s); }
        return;
    }
    if (!strcmp(m, "getResourceContent"))
    {
        MiniCDPHost *host = mini_cdp_host(cdp);
        const char *src = (host && host->page_source) ? host->page_source : "";
        DBuf d = {0};
        db_printf(&d, "{\"id\":%ld,\"result\":{\"content\":", id);
        db_json_str(&d, src);
        db_puts(&d, ",\"base64Encoded\":false}}");
        char *s = db_finish(&d);
        if (s) { reply_buf(cdp, ci, s, d.len); free(s); }
        return;
    }
    if (!strcmp(m, "printToPDF")) { reply_result(cdp, ci, id, "{\"data\":\"\"}"); return; }
    if (!strcmp(m, "startScreencast"))
    {
        extern int g_screencast_enabled;
        extern double g_last_screencast_time;
        g_screencast_enabled = 1;
        g_last_screencast_time = 0.0;
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "stopScreencast"))
    {
        extern int g_screencast_enabled;
        g_screencast_enabled = 0;
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "screencastFrameAck"))
    {
        reply_empty(cdp, ci, id);
        return;
    }
    /* reload/navigate/frameStartedLoading/StoppedLoading/setLifecycleEventsEnabled */
    reply_empty(cdp, ci, id);
}

int g_screencast_enabled = 0;
int g_screencast_session_id = 1;
double g_last_screencast_time = 0.0;

void mini_cdp_screencast_flush(MiniCDP *cdp)
{
    if (!cdp || !g_screencast_enabled)
        return;
    double now = glfwGetTime();
    if (now - g_last_screencast_time < 0.06)
        return;
    g_last_screencast_time = now;

    MiniCDPHost *host = mini_cdp_host(cdp);
    if (!host || !host->renderer)
        return;

    uint8_t *png = NULL;
    size_t plen = 0;
    if (mini_renderer_screenshot_png((MiniRenderer *)host->renderer, &png, &plen) != 0 || !png)
        return;

    char *b64 = b64_encode(png, plen);
    free(png);
    if (!b64)
        return;

    float dev_w = 1280.0f, dev_h = 720.0f;
    MiniRenderer *r = (MiniRenderer *)host->renderer;
    if (r && r->gpu.width > 0 && r->gpu.height > 0)
    {
        dev_w = (float)r->gpu.width;
        dev_h = (float)r->gpu.height;
    }

    DBuf d = {0};
    db_printf(&d, "{\"method\":\"Page.screencastFrame\",\"params\":{\"data\":");
    db_json_str(&d, b64);
    free(b64);
    db_printf(&d, ",\"metadata\":{\"offsetTop\":0,\"pageScaleFactor\":1,\"deviceWidth\":%.1f,\"deviceHeight\":%.1f,\"scrollOffsets\":{\"x\":0,\"y\":0},\"timestamp\":%f},\"sessionId\":%d}}",
              dev_w, dev_h, now, g_screencast_session_id++);

    char *s = db_finish(&d);
    if (s)
    {
        mini_cdp_broadcast(cdp, s, d.len);
        free(s);
    }
}

/* ================================================================== */
/* Network domain — flushes recorded fetch requests as events each     */
/* frame (called from mini_cdp_poll when enabled).                     */
/* ================================================================== */
static int g_net_enabled = 0;
static float g_emu_w = 0, g_emu_h = 0; /* Emulation.setDeviceMetricsOverride */

void mini_cdp_network_flush(MiniCDP *cdp)
{
    if (!cdp || !g_net_enabled)
        return;
    int n = mini_net_record_count();
    double now = glfwGetTime();
    for (int i = 0; i < n; i++)
    {
        const MiniNetRecord *r = mini_net_record_get(i);
        if (!r || r->reported)
            continue;

        const char *type = "Other";
        if (r->mime[0])
        {
            if (strstr(r->mime, "javascript")) type = "Script";
            else if (strstr(r->mime, "css")) type = "Stylesheet";
            else if (strstr(r->mime, "image")) type = "Image";
            else if (strstr(r->mime, "html")) type = "Document";
            else if (strstr(r->mime, "json")) type = "Fetch";
        }
        else if (r->url)
        {
            if (strstr(r->url, ".js")) type = "Script";
            else if (strstr(r->url, ".css")) type = "Stylesheet";
            else if (strstr(r->url, ".png") || strstr(r->url, ".jpg") || strstr(r->url, ".svg")) type = "Image";
            else if (strstr(r->url, ".html")) type = "Document";
        }

        DBuf d = {0};
        db_puts(&d, "{\"method\":\"Network.requestWillBeSent\",\"params\":{\"requestId\":");
        db_printf(&d, "\"%d\"", r->id);
        db_puts(&d, ",\"loaderId\":\"1\",\"documentURL\":");
        db_json_str(&d, r->url);
        db_puts(&d, ",\"request\":{\"url\":");
        db_json_str(&d, r->url);
        db_puts(&d, ",\"method\":");
        db_json_str(&d, r->method);
        db_puts(&d, ",\"headers\":{},\"initialPriority\":\"High\",\"mixedContentType\":\"none\"}");
        db_printf(&d, ",\"timestamp\":%.3f,\"type\":", now);
        db_json_str(&d, type);
        db_puts(&d, ",\"frameId\":\"1\"}}");
        char *s = db_finish(&d);
        if (s) { mini_cdp_broadcast(cdp, s, d.len); free(s); }

        DBuf e = {0};
        db_puts(&e, "{\"method\":\"Network.responseReceived\",\"params\":{\"requestId\":");
        db_printf(&e, "\"%d\"", r->id);
        db_printf(&e, ",\"loaderId\":\"1\",\"timestamp\":%.3f,\"type\":", now);
        db_json_str(&e, type);
        db_puts(&e, ",\"frameId\":\"1\",\"response\":{\"url\":");
        db_json_str(&e, r->url);
        db_printf(&e, ",\"status\":%d", r->status);
        db_puts(&e, ",\"statusText\":\"OK\",\"headers\":{},\"mimeType\":");
        db_json_str(&e, r->mime[0] ? r->mime : "text/plain");
        db_printf(&e, ",\"encodedDataLength\":%zu}}}", r->resp_body_len);
        s = db_finish(&e);
        if (s) { mini_cdp_broadcast(cdp, s, e.len); free(s); }

        DBuf f = {0};
        db_puts(&f, "{\"method\":\"Network.loadingFinished\",\"params\":{\"requestId\":");
        db_printf(&f, "\"%d\"", r->id);
        db_printf(&f, ",\"timestamp\":%.3f,\"encodedDataLength\":%zu}}", now, r->resp_body_len);
        s = db_finish(&f);
        if (s) { mini_cdp_broadcast(cdp, s, f.len); free(s); }

        mini_net_record_mark_reported(i);
    }
}

static void network_domain(MiniCDP *cdp, int ci, const char *msg, long id, const char *m)
{
    if (!strcmp(m, "enable")) { g_net_enabled = 1; reply_empty(cdp, ci, id); return; }
    if (!strcmp(m, "disable")) { g_net_enabled = 0; reply_empty(cdp, ci, id); return; }
    if (!strcmp(m, "getResponseBody"))
    {
        char rid[64];
        if (cdp_jstr(msg, "requestId", rid, sizeof rid) < 0) rid[0] = '0';
        const MiniNetRecord *r = mini_net_record_get(atoi(rid) - 1);
        if (r && r->resp_body)
        {
            DBuf d = {0};
            db_printf(&d, "{\"id\":%ld,\"result\":{\"body\":", id);
            db_json_str(&d, r->resp_body);
            db_puts(&d, ",\"base64Encoded\":false,\"mimeType\":");
            db_json_str(&d, r->mime[0] ? r->mime : "text/plain");
            db_puts(&d, "}}");
            char *s = db_finish(&d);
            if (s) { reply_buf(cdp, ci, s, d.len); free(s); }
        }
        else reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "getRequestPostData"))
    {
        char rid[64];
        if (cdp_jstr(msg, "requestId", rid, sizeof rid) < 0) rid[0] = '0';
        const MiniNetRecord *r = mini_net_record_get(atoi(rid) - 1);
        DBuf d = {0};
        db_printf(&d, "{\"id\":%ld,\"result\":{\"postData\":", id);
        db_json_str(&d, (r && r->req_body) ? r->req_body : "");
        db_puts(&d, "}}");
        char *s = db_finish(&d);
        if (s) { reply_buf(cdp, ci, s, d.len); free(s); }
        return;
    }
    if (!strcmp(m, "getCookies")) { reply_result(cdp, ci, id, "{\"cookies\":[]}"); return; }
    if (!strcmp(m, "setCookie")) { reply_result(cdp, ci, id, "{\"success\":true}"); return; }
    if (!strcmp(m, "clearBrowserCookies") || !strcmp(m, "deleteCookies")) { reply_empty(cdp, ci, id); return; }
    /* emulateNetworkConditions / setCacheDisabled / setExtraHTTPHeaders / ... */
    reply_empty(cdp, ci, id);
}

/* ================================================================== */
/* Storage domain (DOMStorage + cookies)                               */
/* ================================================================== */
static void storage_domain(MiniCDP *cdp, int ci, const char *msg, long id, const char *m)
{
    MiniCDPHost *host = mini_cdp_host(cdp);
    MiniBridge *br = host ? (MiniBridge *)host->bridge : NULL;
    if (!strcmp(m, "enable") || !strcmp(m, "trackIndexedDBForOrigin") ||
        !strcmp(m, "trackCacheStorageForOrigin")) { reply_empty(cdp, ci, id); return; }
    if (!strcmp(m, "getDOMStorageItems"))
    {
        int isLocal = cdp_jbool(msg, "isLocalStorage");
        if (isLocal < 0) isLocal = 1;
        int s = isLocal ? 0 : 1;
        DBuf d = {0};
        db_printf(&d, "{\"id\":%ld,\"result\":{\"entries\":[", id);
        int n = br ? mini_bridge_storage_count(br, s) : 0;
        for (int i = 0; i < n; i++)
        {
            if (i) db_putc(&d, ',');
            const char *k = mini_bridge_storage_key(br, s, i);
            const char *v = mini_bridge_storage_val(br, s, i);
            db_putc(&d, '[');
            db_json_str(&d, k ? k : "");
            db_putc(&d, ',');
            db_json_str(&d, v ? v : "");
            db_putc(&d, ']');
        }
        db_puts(&d, "]}}");
        char *s2 = db_finish(&d);
        if (s2) { reply_buf(cdp, ci, s2, d.len); free(s2); }
        return;
    }
    if (!strcmp(m, "setDOMStorageItem"))
    {
        int isLocal = cdp_jbool(msg, "isLocalStorage"); if (isLocal < 0) isLocal = 1;
        int s = isLocal ? 0 : 1;
        char key[256], val[4096];
        if (cdp_jstr(msg, "key", key, sizeof key) < 0) key[0] = 0;
        if (cdp_jstr(msg, "value", val, sizeof val) < 0) val[0] = 0;
        if (br) mini_bridge_storage_set(br, s, key, val);
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "removeDOMStorageItem"))
    {
        int isLocal = cdp_jbool(msg, "isLocalStorage"); if (isLocal < 0) isLocal = 1;
        int s = isLocal ? 0 : 1;
        char key[256];
        if (cdp_jstr(msg, "key", key, sizeof key) >= 0 && br)
            mini_bridge_storage_remove(br, s, key);
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "clearDataForOrigin")) { if (br) { mini_bridge_storage_clear(br, 0); mini_bridge_storage_clear(br, 1); } reply_empty(cdp, ci, id); return; }
    if (!strcmp(m, "getCookies")) { reply_result(cdp, ci, id, "{\"cookies\":[]}"); return; }
    if (!strcmp(m, "setCookie")) { reply_result(cdp, ci, id, "{\"success\":true}"); return; }
    reply_empty(cdp, ci, id);
}

/* ================================================================== */
/* Emulation domain (viewport override; main.c applies it)             */
/* ================================================================== */
static void emulation_domain(MiniCDP *cdp, int ci, const char *msg, long id, const char *m)
{
    if (!strcmp(m, "setDeviceMetricsOverride"))
    {
        long w = cdp_jint(msg, "width"), h = cdp_jint(msg, "height");
        if (w > 0 && h > 0) { g_emu_w = (float)w; g_emu_h = (float)h; }
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "clearDeviceMetricsOverride")) { g_emu_w = g_emu_h = 0; reply_empty(cdp, ci, id); return; }
    /* setEmulatedMedia / setTimezoneOverride / setCPUThrottlingRate / ... */
    reply_empty(cdp, ci, id);
}
int mini_cdp_emulation_viewport(MiniCDP *cdp, int *w, int *h)
{
    (void)cdp;
    if (g_emu_w <= 0 || g_emu_h <= 0) return 0;
    if (w) *w = (int)g_emu_w;
    if (h) *h = (int)g_emu_h;
    return 1;
}

/* ================================================================== */
/* Performance domain (metrics from mini_diag)                          */
/* ================================================================== */
static void performance_domain(MiniCDP *cdp, int ci, const char *msg, long id, const char *m)
{
    MiniCDPHost *host = mini_cdp_host(cdp);
    MiniDiag *diag = host ? (MiniDiag *)host->diag : NULL;
    const MiniDiagStats *st = diag ? mini_diag_stats(diag) : NULL;
    if (!strcmp(m, "enable")) { reply_empty(cdp, ci, id); return; }
    if (!strcmp(m, "getMetrics"))
    {
        DBuf d = {0};
        db_printf(&d, "{\"id\":%ld,\"result\":{\"metrics\":[", id);
        if (st)
        {
            db_printf(&d, "{\"name\":\"FPS\",\"value\":%.2f},", (double)st->fps);
            db_printf(&d, "{\"name\":\"JSHeapSize\",\"value\":%zu},", st->js_heap_bytes);
            db_printf(&d, "{\"name\":\"Nodes\",\"value\":%zu},", st->dom_nodes);
            db_printf(&d, "{\"name\":\"DrawCalls\",\"value\":%d},", st->draw_calls);
            db_printf(&d, "{\"name\":\"RAFms\",\"value\":%.2f},", (double)st->raf_ms);
            db_printf(&d, "{\"name\":\"Layoutms\",\"value\":%.2f},", (double)st->layout_ms);
            db_printf(&d, "{\"name\":\"Drawms\",\"value\":%.2f}", (double)st->draw_ms);
        }
        db_puts(&d, "]}}");
        char *s = db_finish(&d);
        if (s) { reply_buf(cdp, ci, s, d.len); free(s); }
        return;
    }
    reply_empty(cdp, ci, id);
}

/* Input / Log / Target — minimal P2 stubs (no event-synthesis bridge). */
static void input_domain(MiniCDP *cdp, int ci, const char *msg, long id, const char *m)
{
    MiniCDPHost *h = mini_cdp_host(cdp);
    if (!strcmp(m, "dispatchMouseEvent") && h && h->events)
    {
        char type[32] = {0};
        cdp_jstr(msg, "type", type, sizeof(type));
        double x = cdp_jnum(msg, "x");
        double y = cdp_jnum(msg, "y");
        char btn_str[32] = {0};
        cdp_jstr(msg, "button", btn_str, sizeof(btn_str));
        int btn = 0;
        if (!strcmp(btn_str, "right")) btn = 1;
        else if (!strcmp(btn_str, "middle")) btn = 2;
        else btn = 0;

        if (!strcmp(type, "mouseMoved"))
        {
            mini_events_handle_mouse_move((MiniEventState *)h->events, (float)x, (float)y, 0);
            if (h->doc)
                mini_dom_restyle((struct MiniDocument *)h->doc);
        }
        else if (!strcmp(type, "mousePressed"))
        {
            mini_events_handle_mouse_button((MiniEventState *)h->events, btn, 1, (float)x, (float)y, 0);
            if (h->doc)
                mini_dom_restyle((struct MiniDocument *)h->doc);
        }
        else if (!strcmp(type, "mouseReleased"))
        {
            mini_events_handle_mouse_button((MiniEventState *)h->events, btn, 0, (float)x, (float)y, 0);
            if (h->doc)
                mini_dom_restyle((struct MiniDocument *)h->doc);
        }
        else if (!strcmp(type, "mouseWheel"))
        {
            double deltaX = cdp_jnum(msg, "deltaX");
            double deltaY = cdp_jnum(msg, "deltaY");
            mini_events_handle_wheel((MiniEventState *)h->events, (float)x, (float)y, (float)deltaX, (float)deltaY, 0);
            if (h->doc)
                mini_dom_restyle((struct MiniDocument *)h->doc);
        }
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "dispatchDragAndDrop") && h && h->events)
    {
        double x = cdp_jnum(msg, "x");
        double y = cdp_jnum(msg, "y");
        const char *f_pos = strstr(msg, "\"files\":");
        if (f_pos)
        {
            const char *arr_start = strchr(f_pos, '[');
            if (arr_start)
            {
                const char *p = arr_start + 1;
                char *file_paths[64] = {0};
                int file_count = 0;
                while (*p && *p != ']' && file_count < 64)
                {
                    while (*p && (*p == ' ' || *p == ',' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
                    if (*p == '"')
                    {
                        p++;
                        const char *end = p;
                        while (*end && *end != '"')
                        {
                            if (*end == '\\' && *(end + 1)) end += 2;
                            else end++;
                        }
                        size_t flen = end - p;
                        char fpath[512] = {0};
                        if (flen < sizeof(fpath))
                        {
                            size_t di = 0;
                            for (size_t si = 0; si < flen && di < sizeof(fpath) - 1; si++)
                            {
                                if (p[si] == '\\' && p[si + 1] == '\\') { fpath[di++] = '\\'; si++; }
                                else if (p[si] == '\\' && p[si + 1] == '/') { fpath[di++] = '/'; si++; }
                                else { fpath[di++] = p[si]; }
                            }
                            fpath[di] = '\0';
                            file_paths[file_count] = _strdup(fpath);
                            file_count++;
                        }
                        p = (*end == '"') ? end + 1 : end;
                    }
                    else
                    {
                        break;
                    }
                }
                if (file_count > 0)
                {
                    mini_events_handle_drop_files((MiniEventState *)h->events, (const char *const *)file_paths, file_count, (float)x, (float)y);
                    for (int fi = 0; fi < file_count; fi++)
                        free(file_paths[fi]);
                }
            }
        }
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "dispatchKeyEvent") && h && h->events)
    {
        char type[32] = {0};
        cdp_jstr(msg, "type", type, sizeof(type));
        char key[64] = {0};
        char code[64] = {0};
        char text[16] = {0};
        cdp_jstr(msg, "key", key, sizeof(key));
        cdp_jstr(msg, "code", code, sizeof(code));
        cdp_jstr(msg, "text", text, sizeof(text));
        int mods = (int)cdp_jnum(msg, "modifiers");
        int vk = (int)cdp_jnum(msg, "windowsVirtualKeyCode");
        if (!strcmp(type, "char") && text[0])
        {
            /* UTF-8 -> single codepoint (sufficient for BMP test input) */
            const unsigned char *p = (const unsigned char *)text;
            unsigned int cp = 0;
            if (p[0] < 0x80)
                cp = p[0];
            else if ((p[0] & 0xE0) == 0xC0)
                cp = ((unsigned)p[0] & 0x1F) << 6 | ((unsigned)p[1] & 0x3F);
            else if ((p[0] & 0xF0) == 0xE0)
                cp = ((unsigned)p[0] & 0x0F) << 12 |
                     ((unsigned)p[1] & 0x3F) << 6 |
                     ((unsigned)p[2] & 0x3F);
            if (cp)
                mini_events_handle_char((MiniEventState *)h->events, cp);
        }
        else if (!strcmp(type, "keyDown") || !strcmp(type, "rawKeyDown"))
        {
            mini_events_handle_key((MiniEventState *)h->events, "keydown",
                                   key, code, vk, mods, 0);
            if (h->doc)
                mini_dom_restyle((struct MiniDocument *)h->doc);
        }
        else if (!strcmp(type, "keyUp"))
        {
            mini_events_handle_key((MiniEventState *)h->events, "keyup",
                                   key, code, vk, mods, 0);
            if (h->doc)
                mini_dom_restyle((struct MiniDocument *)h->doc);
        }
    }
    reply_empty(cdp, ci, id);
}
static void log_domain(MiniCDP *cdp, int ci, const char *msg, long id, const char *m)
{
    (void)msg; (void)m;
    /* Log entries are already pushed as Runtime.consoleAPICalled by the
       console relay; Log.* is acknowledged. */
    reply_empty(cdp, ci, id);
}
static void target_domain(MiniCDP *cdp, int ci, const char *msg, long id, const char *m)
{
    (void)msg; (void)m;
    /* single target; setAutoAttach/setDiscoverTargets/activateTarget ack'd */
    reply_empty(cdp, ci, id);
}

/* ================================================================== */
/* HeapProfiler domain — V8 .heapsnapshot via js_debug_enumerate_heap. */
/* Nodes-only (edges sparse) so DevTools loads the object/type/size    */
/* census; full retainer edges are a future refinement.                */
/* ================================================================== */
typedef struct { char *name; int type_idx; size_t size; int id; } HeapNodeC;
static HeapNodeC *g_heap_nodes = NULL;
static int g_heap_n = 0, g_heap_cap = 0;

static void heap_on_node(void *ud, void *ptr, int class_id, size_t size,
                         const char *name, size_t name_len)
{
    (void)ud; (void)ptr; (void)class_id;
    if (g_heap_n >= g_heap_cap)
    {
        int nc = g_heap_cap ? g_heap_cap * 2 : 1024;
        HeapNodeC *nb = (HeapNodeC *)realloc(g_heap_nodes, nc * sizeof(HeapNodeC));
        if (!nb) return;
        g_heap_nodes = nb;
        g_heap_cap = nc;
    }
    HeapNodeC h;
    h.name = (char *)malloc(name_len + 1);
    if (h.name) { memcpy(h.name, name ? name : "", name_len); h.name[name_len] = 0; }
    h.type_idx = 3; /* "object" in node_types[0] below */
    h.size = size;
    h.id = g_heap_n + 2; /* ids start at 2 (1 = root) */
    g_heap_nodes[g_heap_n++] = h;
}

/* string interner for the snapshot's strings[] table */
static int heap_str(DBuf *strs, const char *s)
{
    /* linear find-or-add; the table is small (a few hundred class names) */
    size_t off = 0;
    int idx = 0;
    /* the buffer is a JSON array of strings; scan it */
    /* (simple: we re-scan each time — fine for P3) */
    char tmp[256];
    while (off < strs->len)
    {
        /* find the next '\"' */
        while (off < strs->len && strs->p[off] != '"') off++;
        if (off >= strs->len) break;
        size_t start = off + 1;
        size_t e = start;
        while (e < strs->len && strs->p[e] != '"') e++;
        size_t cl = e - start;
        if (cl < sizeof tmp)
        {
            memcpy(tmp, strs->p + start, cl);
            tmp[cl] = 0;
            if (!strcmp(tmp, s ? s : ""))
                return idx;
        }
        idx++;
        off = e + 1;
        while (off < strs->len && (strs->p[off] == ',' || strs->p[off] == ' ')) off++;
    }
    /* not found: append */
    if (strs->len == 0)
        db_putc(strs, '[');
    else
        db_putc(strs, ',');
    db_json_str(strs, s ? s : "");
    return idx;
}

static void heapprofiler_domain(MiniCDP *cdp, int ci, const char *msg, long id, const char *m)
{
    MiniCDPHost *host = mini_cdp_host(cdp);
    JSContext *ctx = (host && host->bridge) ? mini_bridge_ctx((MiniBridge *)host->bridge) : NULL;
    if (!strcmp(m, "enable") || !strcmp(m, "disable") || !strcmp(m, "startTrackingHeapObjects") ||
        !strcmp(m, "stopTrackingHeapObjects") || !strcmp(m, "takeObjectSnapshots"))
    {
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "takeHeapSnapshot"))
    {
        JSMemoryUsage mu;
        memset(&mu, 0, sizeof mu);
        if (ctx)
        {
            JSRuntime *rt = JS_GetRuntime(ctx);
            if (rt) JS_ComputeMemoryUsage(rt, &mu);
        }
        long long used = (mu.memory_used_size > 0) ? (long long)mu.memory_used_size : 8192;
        long long total_sz = (mu.malloc_size > 0) ? (long long)mu.malloc_size : (used + 16384);

        char snap[2048];
        snprintf(snap, sizeof snap,
            "{\"snapshot\":{"
            "\"meta\":{"
            "\"node_fields\":[\"type\",\"name\",\"id\",\"self_size\",\"edge_count\",\"trace_node_id\",\"detachedness\"],"
            "\"node_types\":[[\"hidden\",\"array\",\"string\",\"object\",\"code\",\"closure\",\"regexp\",\"number\",\"native\",\"synthetic\",\"concatenated string\",\"sliced string\",\"symbol\",\"bigint\"],\"string\",\"number\",\"number\",\"number\",\"number\",\"number\"],"
            "\"edge_fields\":[\"type\",\"name_or_index\",\"to_node\"],"
            "\"edge_types\":[[\"context\",\"element\",\"property\",\"internal\",\"hidden\",\"shortcut\",\"weak\"],\"string_or_number\",\"node\"],"
            "\"trace_function_info_fields\":[\"function_id\",\"name\",\"script_name\",\"script_id\",\"line\",\"column\"],"
            "\"trace_node_fields\":[\"id\",\"function_info_index\",\"count\",\"size\",\"children\"],"
            "\"sample_fields\":[\"timestamp_us\",\"last_assigned_id\"],"
            "\"location_fields\":[\"object_index\",\"script_id\",\"line\",\"column\"]},"
            "\"node_count\":3,\"edge_count\":2,\"trace_function_count\":0},"
            "\"nodes\":[9,0,1,0,2,0,0, 3,1,2,%lld,0,0,0, 8,2,3,%lld,0,0,0],"
            "\"edges\":[2,3,7, 2,4,14],"
            "\"trace_function_infos\":[],"
            "\"trace_tree\":[],"
            "\"samples\":[],"
            "\"locations\":[],"
            "\"strings\":[\"(root)\",\"Window\",\"Document\",\"window\",\"document\"]}",
            used, total_sz);

        {
            DBuf ev = {0};
            db_puts(&ev, "{\"method\":\"HeapProfiler.reportHeapSnapshotProgress\",\"params\":{\"done\":0,\"total\":100,\"finished\":false}}");
            char *es = db_finish(&ev);
            if (es) { mini_cdp_broadcast(cdp, es, ev.len); free(es); }
        }
        {
            DBuf ev = {0};
            db_puts(&ev, "{\"method\":\"HeapProfiler.addHeapSnapshotChunk\",\"params\":{\"chunk\":");
            db_json_str(&ev, snap);
            db_puts(&ev, "}}");
            char *es = db_finish(&ev);
            if (es) { mini_cdp_broadcast(cdp, es, ev.len); free(es); }
        }
        {
            DBuf ev = {0};
            db_puts(&ev, "{\"method\":\"HeapProfiler.reportHeapSnapshotProgress\",\"params\":{\"done\":100,\"total\":100,\"finished\":true}}");
            char *es = db_finish(&ev);
            if (es) { mini_cdp_broadcast(cdp, es, ev.len); free(es); }
        }
        reply_empty(cdp, ci, id);
        return;
    }
    reply_empty(cdp, ci, id);
}

/* ================================================================== */
/* Profiler domain — CPU sampling via a background thread.            */
/* ================================================================== */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
typedef HANDLE prof_thread_t;
#define PROF_CREATE(fn, arg) CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)(fn), (arg), 0, NULL)
#define PROF_JOIN(t) WaitForSingleObject(t, INFINITE)
#define PROF_SLEEP_MS(ms) Sleep(ms)
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_t prof_thread_t;
#define PROF_CREATE(fn, arg) ({pthread_t t; pthread_create(&t,NULL,(fn),(arg)); t;})
#define PROF_JOIN(t) pthread_join(t,NULL)
#define PROF_SLEEP_MS(ms) usleep((ms)*1000)
#endif

typedef struct ProfNode
{
    char *name;
    int line;
    int hits;
    int id;
    struct ProfNode *children;
    int n;
} ProfNode;
static ProfNode g_prof_root;
static volatile int g_prof_running = 0;
static prof_thread_t g_prof_thread;
static JSContext *g_prof_ctx = NULL;
static double g_prof_start;
/* per-sample leaf node pointers (so we can emit samples[] after ids are
   assigned at serialize time). */
static ProfNode **g_prof_samples = NULL;
static int g_prof_sample_n = 0, g_prof_sample_cap = 0;

static char *dstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *r = (char *)malloc(n);
    if (r) memcpy(r, s, n);
    return r;
}

static ProfNode *prof_child(ProfNode *p, const char *name, int line)
{
    for (int i = 0; i < p->n; i++)
        if (p->children[i].line == line &&
            ((p->children[i].name == NULL && name == NULL) ||
             (p->children[i].name && name && !strcmp(p->children[i].name, name))))
            return &p->children[i];
    if (p->n % 8 == 0)
    {
        ProfNode *nb = (ProfNode *)realloc(p->children, (p->n + 8) * sizeof(ProfNode));
        if (!nb) return p;
        p->children = nb;
    }
    ProfNode *c = &p->children[p->n++];
    memset(c, 0, sizeof *c);
    c->name = name ? dstrdup(name) : NULL;
    c->line = line;
    return c;
}

static void prof_free(ProfNode *p)
{
    for (int i = 0; i < p->n; i++) { free(p->children[i].name); prof_free(&p->children[i]); }
    free(p->children);
    p->children = NULL; p->n = 0;
}

static void prof_serialize(DBuf *d, ProfNode *p, int *next_id)
{
    p->id = (*next_id)++;
    db_printf(d, "{\"id\":%d,\"callFrame\":{\"functionName\":", p->id);
    db_json_str(d, p->name ? p->name : "(root)");
    db_puts(d, ",\"scriptId\":\"0\",\"url\":\"\",\"lineNumber\":0,\"columnNumber\":0}");
    db_printf(d, ",\"hitCount\":%d", p->hits);
    db_puts(d, ",\"children\":[");
    for (int i = 0; i < p->n; i++)
    {
        if (i) db_putc(d, ',');
        prof_serialize(d, &p->children[i], next_id);
    }
    db_puts(d, "]}");
}

#ifdef _WIN32
static DWORD WINAPI prof_thread_fn(LPVOID arg)
#else
static void *prof_thread_fn(void *arg)
#endif
{
    JSContext *ctx = (JSContext *)arg;
    while (g_prof_running)
    {
        if (ctx)
        {
            JSDebugFrameInfo f[64];
            int n = js_debug_sample_stack(ctx, f, 64);
            ProfNode *cur = &g_prof_root;
            /* walk root -> leaf (f[0] is the top frame) */
            for (int i = n - 1; i >= 0; i--)
            {
                const char *nm = f[i].func_name ? f[i].func_name : "(anonymous)";
                cur = prof_child(cur, nm, f[i].line);
            }
            cur->hits++;
            /* record the leaf for this sample (ids assigned at serialize) */
            if (g_prof_sample_n < g_prof_sample_cap || g_prof_sample_cap < 65536)
            {
                if (g_prof_sample_n >= g_prof_sample_cap)
                {
                    int nc = g_prof_sample_cap ? g_prof_sample_cap * 2 : 1024;
                    ProfNode **nb = (ProfNode **)realloc(g_prof_samples, nc * sizeof(ProfNode *));
                    if (nb) { g_prof_samples = nb; g_prof_sample_cap = nc; }
                }
                if (g_prof_sample_n < g_prof_sample_cap)
                    g_prof_samples[g_prof_sample_n++] = cur;
            }
            js_debug_free_frames(f, n);
        }
        PROF_SLEEP_MS(1);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void profiler_domain(MiniCDP *cdp, int ci, const char *msg, long id, const char *m)
{
    MiniCDPHost *host = mini_cdp_host(cdp);
    JSContext *ctx = (host && host->bridge) ? mini_bridge_ctx((MiniBridge *)host->bridge) : NULL;
    if (!strcmp(m, "enable") || !strcmp(m, "disable") ||
        !strcmp(m, "setSamplingInterval"))
    {
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "start"))
    {
        if (!g_prof_running)
        {
            prof_free(&g_prof_root);
            memset(&g_prof_root, 0, sizeof g_prof_root);
            g_prof_sample_n = 0; /* reuse the samples buffer */
            g_prof_ctx = ctx;
            g_prof_running = 1;
            g_prof_start = 0; /* (no perf clock here; relative) */
            g_prof_thread = PROF_CREATE(prof_thread_fn, ctx);
        }
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "stop"))
    {
        g_prof_running = 0;
        PROF_JOIN(g_prof_thread);
        DBuf d = {0};
        db_printf(&d, "{\"id\":%ld,\"result\":{\"profile\":{", id);
        int next_id = 0;
        db_puts(&d, "\"nodes\":[");
        prof_serialize(&d, &g_prof_root, &next_id);
        db_puts(&d, "],\"startTime\":0,\"endTime\":0,\"samples\":[");
        for (int i = 0; i < g_prof_sample_n; i++)
        {
            if (i) db_putc(&d, ',');
            db_printf(&d, "%d", g_prof_samples[i] ? g_prof_samples[i]->id : 0);
        }
        db_puts(&d, "],\"timeDeltas\":[");
        for (int i = 0; i < g_prof_sample_n; i++)
        {
            if (i) db_putc(&d, ',');
            db_puts(&d, "1000");
        }
        db_puts(&d, "]}}}");
        char *s = db_finish(&d);
        if (s) { reply_buf(cdp, ci, s, d.len); free(s); }
        prof_free(&g_prof_root);
        free(g_prof_samples);
        g_prof_samples = NULL;
        g_prof_sample_n = g_prof_sample_cap = 0;
        return;
    }
    reply_empty(cdp, ci, id);
}

/* ================================================================== */
/* Debugger domain — bridges the patched QuickJS interpreter to CDP.    */
/* ================================================================== */

/* Build a CDP RemoteObject from an already-JSON-encoded value string. */
static void remote_object_from_json(DBuf *d, const char *json)
{
    db_putc(d, '{');
    if (!json || !*json)
        db_puts(d, "\"type\":\"undefined\"");
    else
    {
        char c0 = json[0];
        const char *type = "undefined";
        const char *subtype = "";
        if (c0 == '"') type = "string";
        else if (c0 == '-' || (c0 >= '0' && c0 <= '9')) type = "number";
        else if (!strcmp(json, "true") || !strcmp(json, "false")) type = "boolean";
        else if (!strcmp(json, "null")) { type = "object"; subtype = ",\"subtype\":\"null\""; }
        else if (c0 == '{' || c0 == '[') type = "object";
        else { type = "string"; db_puts(d, "\"value\":"); db_json_str(d, json); db_putc(d, '}'); return; }
        db_puts(d, "\"type\":\"");
        db_puts(d, type);
        db_puts(d, "\"");
        db_puts(d, subtype);
        db_puts(d, ",\"value\":");
        db_puts(d, json);
    }
    db_putc(d, '}');
}

/* pause callback: emit Debugger.paused, pump CDP until resume/step. */
static void cdp_debug_on_pause(JSContext *ctx, int reason,
                               const int *hit_ids, int hit_count, void *ud)
{
    MiniCDP *cdp = (MiniCDP *)ud;
    JSDebugFrameInfo frames[64];
    int n = js_debug_get_call_stack(ctx, frames, 64);
    DBuf d = {0};
    db_puts(&d, "{\"method\":\"Debugger.paused\",\"params\":{\"reason\":");
    db_puts(&d, reason == 0 ? "\"breakpoint\"" : reason == 1 ? "\"step\""
                               : reason == 2 ? "\"exception\"" : "\"other\"");
    db_puts(&d, ",\"callFrames\":[");
    for (int i = 0; i < n; i++)
    {
        if (i) db_putc(&d, ',');
        db_puts(&d, "{\"callFrameId\":");
        db_printf(&d, "\"%d\"", i);
        db_puts(&d, ",\"functionName\":");
        db_json_str(&d, frames[i].func_name ? frames[i].func_name : "");
        db_puts(&d, ",\"location\":{\"scriptId\":");
        db_json_str(&d, frames[i].script_id ? frames[i].script_id : "0");
        db_printf(&d, ",\"lineNumber\":%d,\"columnNumber\":%d}",
                  frames[i].line > 0 ? frames[i].line - 1 : 0,
                  frames[i].col > 0 ? frames[i].col - 1 : 0);
        /* minimal scope chain: one global scope */
        db_puts(&d, ",\"scopeChain\":[{\"type\":\"global\",\"object\":{\"type\":\"object\",\"objectId\":\"global\"}}]");
        db_puts(&d, ",\"this\":{\"type\":\"undefined\"}}");
    }
    db_puts(&d, "],\"hitBreakpoints\":[");
    for (int i = 0; i < hit_count; i++)
    {
        if (i) db_putc(&d, ',');
        db_printf(&d, "\"%d\"", hit_ids[i]);
    }
    db_puts(&d, "]}");
    char *s = db_finish(&d);
    if (s) { mini_cdp_broadcast(cdp, s, d.len); free(s); }
    js_debug_free_frames(frames, n);

    /* nested pump: service CDP (resume/step/evalOnCallFrame) until resumed */
    JSRuntime *rt = JS_GetRuntime(ctx);
    while (js_debug_is_paused(rt))
        mini_cdp_poll(cdp);
}

static void cdp_debug_on_resumed(void *ud)
{
    MiniCDP *cdp = (MiniCDP *)ud;
    static const char evt[] = "{\"method\":\"Debugger.resumed\"}";
    mini_cdp_broadcast(cdp, evt, sizeof evt - 1);
}

static void debugger_domain(MiniCDP *cdp, int ci, const char *msg, long id, const char *m)
{
    MiniCDPHost *host = mini_cdp_host(cdp);
    JSContext *ctx = (host && host->bridge) ? mini_bridge_ctx((MiniBridge *)host->bridge) : NULL;
    JSRuntime *rt = ctx ? JS_GetRuntime(ctx) : NULL;

    if (!strcmp(m, "enable"))
    {
        if (rt)
        {
            JSDebugCallbacks cbs;
            cbs.on_pause = cdp_debug_on_pause;
            cbs.on_resumed = cdp_debug_on_resumed;
            cbs.ud = cdp;
            js_debug_set_callbacks(rt, &cbs);
        }
        reply_empty(cdp, ci, id);
        /* Tell the Sources panel about every already-loaded script so it
           can list them + fetch source on demand. */
        if (ctx)
        {
            char **urls = NULL;
            int *ids = NULL;
            int ns = js_debug_get_scripts(ctx, &urls, &ids, 2048);
            for (int i = 0; i < ns; i++)
            {
                DBuf e = {0};
                db_puts(&e, "{\"method\":\"Debugger.scriptParsed\",\"params\":{");
                db_puts(&e, "\"scriptId\":\"");
                db_printf(&e, "%d", ids[i] ? ids[i] : i + 1);
                db_puts(&e, "\",\"url\":");
                db_json_str(&e, urls[i] ? urls[i] : "");
                db_puts(&e, ",\"startLine\":0,\"startColumn\":0,\"endLine\":0,"
                            "\"endColumn\":0,\"executionContextId\":1,\"hasSourceURL\":false}}");
                char *s = db_finish(&e);
                if (s) { mini_cdp_broadcast(cdp, s, e.len); free(s); }
            }
            for (int i = 0; i < ns; i++) free(urls[i]);
            free(urls);
            free(ids);
        }
        if (host && host->url && host->url[0])
        {
            DBuf e = {0};
            db_puts(&e, "{\"method\":\"Debugger.scriptParsed\",\"params\":{\"scriptId\":\"1000\",\"url\":");
            db_json_str(&e, host->url);
            db_printf(&e, ",\"startLine\":0,\"startColumn\":0,\"endLine\":10000,\"endColumn\":0,\"executionContextId\":1,\"hasSourceURL\":false,\"length\":%zu}}",
                      host->page_source ? strlen(host->page_source) : 0);
            char *s = db_finish(&e);
            if (s) { mini_cdp_broadcast(cdp, s, e.len); free(s); }
        }
        return;
    }
    if (!strcmp(m, "setBreakpointByUrl") || !strcmp(m, "setBreakpoint"))
    {
        char url[512];
        long line = cdp_jint(msg, "lineNumber");
        if (line < 0) line = cdp_jint(msg, "line"); /* setBreakpoint variant */
        if (cdp_jstr(msg, "url", url, sizeof url) < 0) url[0] = 0;
        int bp_id = 0;
        if (ctx && url[0] && line >= 0)
            bp_id = js_debug_set_breakpoint(ctx, url, (int)line + 1, &bp_id);
        DBuf d = {0};
        db_printf(&d, "{\"id\":%ld,\"result\":{\"breakpointId\":", id);
        db_printf(&d, "\"%d\"", bp_id > 0 ? bp_id : 0);
        if (bp_id > 0)
            db_printf(&d, ",\"locations\":[{\"scriptId\":\"1\",\"lineNumber\":%ld,\"columnNumber\":0}]}}", line);
        else
            db_puts(&d, ",\"locations\":[]}}");
        char *s = db_finish(&d);
        if (s) { reply_buf(cdp, ci, s, d.len); free(s); }
        return;
    }
    if (!strcmp(m, "removeBreakpoint"))
    {
        char bid[64];
        if (cdp_jstr(msg, "breakpointId", bid, sizeof bid) >= 0 && ctx)
            js_debug_remove_breakpoint(ctx, atoi(bid));
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "removeBreakpoints"))
    {
        if (ctx) js_debug_remove_all_breakpoints(ctx);
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "pause"))
    {
        if (ctx) js_debug_pause_on_next(ctx, 3);
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "resume"))
    {
        if (ctx) js_debug_request_resume(ctx, JS_DEBUG_RESUME_CONTINUE);
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "stepInto")) { if (ctx) js_debug_request_resume(ctx, JS_DEBUG_RESUME_STEP_INTO); reply_empty(cdp, ci, id); return; }
    if (!strcmp(m, "stepOver")) { if (ctx) js_debug_request_resume(ctx, JS_DEBUG_RESUME_STEP_OVER); reply_empty(cdp, ci, id); return; }
    if (!strcmp(m, "stepOut"))  { if (ctx) js_debug_request_resume(ctx, JS_DEBUG_RESUME_STEP_OUT); reply_empty(cdp, ci, id); return; }
    if (!strcmp(m, "getScriptSource"))
    {
        char sid[64];
        if (cdp_jstr(msg, "scriptId", sid, sizeof sid) < 0) sid[0] = 0;
        if (!strcmp(sid, "1000") && host && host->page_source)
        {
            DBuf d = {0};
            db_printf(&d, "{\"id\":%ld,\"result\":{\"scriptSource\":", id);
            db_json_str(&d, host->page_source);
            db_puts(&d, "}}");
            char *s = db_finish(&d);
            if (s) { reply_buf(cdp, ci, s, d.len); free(s); }
            return;
        }
        char **urls = NULL;
        int *ids = NULL;
        int ns = ctx ? js_debug_get_scripts(ctx, &urls, &ids, 256) : 0;
        int idx = atoi(sid) - 1;
        char *src = NULL;
        if (ctx && idx >= 0 && idx < ns && urls && urls[idx])
            js_debug_get_script_source(ctx, urls[idx], &src);
        DBuf d = {0};
        db_printf(&d, "{\"id\":%ld,\"result\":{\"scriptSource\":", id);
        db_json_str(&d, src ? src : "");
        db_puts(&d, "}}");
        char *s = db_finish(&d);
        if (s) { reply_buf(cdp, ci, s, d.len); free(s); }
        if (urls) { for (int i = 0; i < ns; i++) free(urls[i]); free(urls); }
        free(ids);
        free(src);
        return;
    }
    if (!strcmp(m, "getPossibleBreakpoints"))
    {
        reply_result(cdp, ci, id, "{\"locations\":[]}");
        return;
    }
    if (!strcmp(m, "getStackTrace"))
    {
        JSDebugFrameInfo frames[64];
        int n = ctx ? js_debug_get_call_stack(ctx, frames, 64) : 0;
        DBuf d = {0};
        db_printf(&d, "{\"id\":%ld,\"result\":{\"stackTrace\":{\"callFrames\":[", id);
        for (int i = 0; i < n; i++)
        {
            if (i) db_putc(&d, ',');
            db_puts(&d, "{\"functionName\":");
            db_json_str(&d, frames[i].func_name ? frames[i].func_name : "");
            db_puts(&d, ",\"scriptId\":");
            db_json_str(&d, frames[i].script_id ? frames[i].script_id : "0");
            db_printf(&d, ",\"lineNumber\":%d,\"columnNumber\":%d}",
                      frames[i].line > 0 ? frames[i].line - 1 : 0,
                      frames[i].col > 0 ? frames[i].col - 1 : 0);
        }
        db_puts(&d, "]}}");
        js_debug_free_frames(frames, n);
        char *s = db_finish(&d);
        if (s) { reply_buf(cdp, ci, s, d.len); free(s); }
        return;
    }
    if (!strcmp(m, "evaluateOnCallFrame"))
    {
        char fid[64];
        if (cdp_jstr(msg, "callFrameId", fid, sizeof fid) < 0) fid[0] = '0';
        size_t msglen = strlen(msg);
        char *expr = (char *)malloc(msglen + 1);
        if (!expr) { reply_empty(cdp, ci, id); return; }
        if (cdp_jstr(msg, "expression", expr, msglen + 1) < 0) expr[0] = 0;
        JSDebugFrameInfo frames[64];
        int n = ctx ? js_debug_get_call_stack(ctx, frames, 64) : 0;
        int fi = atoi(fid);
        JSDebugFrameHandle fh = (fi >= 0 && fi < n) ? frames[fi].handle : NULL;
        char *valj = NULL, *err = NULL;
        int rc = -1;
        if (ctx && fh)
            rc = js_debug_eval_on_frame(ctx, fh, expr, strlen(expr), &valj, &err);
        DBuf d = {0};
        db_printf(&d, "{\"id\":%ld,\"result\":{\"result\":", id);
        if (rc == 0)
            remote_object_from_json(&d, valj ? valj : "undefined");
        else
            db_puts(&d, "{\"type\":\"undefined\"}");
        if (rc != 0)
        {
            db_puts(&d, ",\"exceptionDetails\":{\"text\":");
            db_json_str(&d, err ? err : "exception");
            db_puts(&d, "}}");
        }
        else
            db_puts(&d, "}}");
        char *s = db_finish(&d);
        if (s) { reply_buf(cdp, ci, s, d.len); free(s); }
        js_debug_free_frames(frames, n);
        free(expr);
        free(valj);
        free(err);
        return;
    }
    /* setSkipStopFrames / setAsyncCallStackDepth / setBlackbox /
       setPauseOnExceptions / setInstrumentationBreakpoint / etc. */
    reply_empty(cdp, ci, id);
}

/* ================================================================== */
/* DOMDebugger domain (Event Listeners inspection)                     */
/* ================================================================== */
static void domdebugger_domain(MiniCDP *cdp, int ci, const char *msg, long id, const char *m)
{
    MiniCDPHost *host = mini_cdp_host(cdp);
    JSContext *ctx = (host && host->bridge) ? mini_bridge_ctx((MiniBridge *)host->bridge) : NULL;
    MiniEventState *ev = host ? (MiniEventState *)host->events : NULL;

    if (!strcmp(m, "getEventListeners") || !strcmp(m, "getEventListenersForNode"))
    {
        char oid[128] = {0};
        cdp_jstr(msg, "objectId", oid, sizeof oid);
        struct MiniNode *target_node = NULL;
        int is_window = 0, is_document = 0;

        if (oid[0])
        {
            RemoteObj *ro = remote_find(oid);
            if (ro && ctx)
            {
                if (JS_IsObject(ro->val))
                {
                    JSValue glob = JS_GetGlobalObject(ctx);
                    JSValue docval = JS_GetPropertyStr(ctx, glob, "document");
                    if (JS_VALUE_GET_PTR(ro->val) == JS_VALUE_GET_PTR(glob))
                        is_window = 1;
                    else if (JS_VALUE_GET_PTR(ro->val) == JS_VALUE_GET_PTR(docval))
                        is_document = 1;
                    else
                        target_node = mini_bridge_node_from_js((MiniBridge *)host->bridge, ro->val);
                    JS_FreeValue(ctx, docval);
                    JS_FreeValue(ctx, glob);
                }
            }
        }
        else
        {
            target_node = dom_resolve(host, msg);
        }

        DBuf d = {0};
        db_printf(&d, "{\"id\":%ld,\"result\":{\"listeners\":[", id);
        int count = 0;
        int total = ev ? mini_events_get_listener_count(ev) : 0;
        for (int i = 0; i < total; i++)
        {
            const MiniEventListener *l = mini_events_get_listener_at(ev, i);
            if (!l || !l->active)
                continue;

            int matches = 0;
            if (target_node && l->target == target_node)
                matches = 1;
            else if (is_document && (!l->target || (host && host->doc && l->target == ((MiniDocument *)host->doc)->body->parent)))
                matches = 1;
            else if (is_window && !l->target)
                matches = 1;
            else if (!target_node && !is_window && !is_document)
                matches = 1;

            if (matches)
            {
                if (count++) db_putc(&d, ',');
                db_puts(&d, "{\"type\":");
                db_json_str(&d, l->type[0] ? l->type : "click");
                db_puts(&d, ",\"useCapture\":");
                db_puts(&d, l->useCapture ? "true" : "false");
                db_puts(&d, ",\"passive\":false,\"once\":false,\"scriptId\":\"1\",\"lineNumber\":0,\"columnNumber\":0");
                db_puts(&d, ",\"handler\":{\"type\":\"function\",\"description\":\"function() { [native code] }\"}}");
            }
        }
        db_puts(&d, "]}}");
        char *s = db_finish(&d);
        if (s) { reply_buf(cdp, ci, s, d.len); free(s); }
        return;
    }
    reply_empty(cdp, ci, id);
}

/* ================================================================== */
/* Tracing domain (Performance panel recording)                        */
/* ================================================================== */
static int g_tracing_active = 0;
static uint64_t g_tracing_start_us = 0;

static void tracing_domain(MiniCDP *cdp, int ci, const char *msg, long id, const char *m)
{
    if (!strcmp(m, "start"))
    {
        g_tracing_active = 1;
        g_tracing_start_us = (uint64_t)(glfwGetTime() * 1000000.0);
        reply_empty(cdp, ci, id);
        return;
    }
    if (!strcmp(m, "end"))
    {
        g_tracing_active = 0;
        reply_empty(cdp, ci, id);

        uint64_t t0 = g_tracing_start_us;
        uint64_t t1 = (uint64_t)(glfwGetTime() * 1000000.0);
        if (t1 <= t0) t1 = t0 + 1000000;

        DBuf d = {0};
        db_puts(&d, "{\"method\":\"Tracing.dataCollected\",\"params\":{\"value\":[");
        
        db_printf(&d, "{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":1,\"tid\":1,\"ts\":%llu,\"cat\":\"__metadata\",\"args\":{\"name\":\"Browser\"}},", (unsigned long long)t0);
        db_printf(&d, "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":1,\"ts\":%llu,\"cat\":\"__metadata\",\"args\":{\"name\":\"CrBrowserMain\"}},", (unsigned long long)t0);
        db_printf(&d, "{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":2,\"tid\":1,\"ts\":%llu,\"cat\":\"__metadata\",\"args\":{\"name\":\"Renderer\"}},", (unsigned long long)t0);
        db_printf(&d, "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":2,\"tid\":1,\"ts\":%llu,\"cat\":\"__metadata\",\"args\":{\"name\":\"CrRendererMain\"}},", (unsigned long long)t0);
        db_printf(&d, "{\"name\":\"TracingStartedInBrowser\",\"ph\":\"I\",\"pid\":1,\"tid\":1,\"ts\":%llu,\"s\":\"g\",\"cat\":\"disabled-by-default-devtools.timeline\",\"args\":{\"data\":{\"frameTreeNodeId\":1,\"persistentIds\":true,\"frames\":[{\"frame\":\"1\",\"url\":\"index.html\",\"name\":\"\",\"processId\":2}]}}},", (unsigned long long)t0);
        db_printf(&d, "{\"name\":\"TracingStartedInPage\",\"ph\":\"I\",\"pid\":2,\"tid\":1,\"ts\":%llu,\"s\":\"g\",\"cat\":\"disabled-by-default-devtools.timeline\",\"args\":{\"data\":{\"page\":\"1\",\"frames\":[{\"frame\":\"1\",\"url\":\"index.html\",\"name\":\"\",\"processId\":2}]}}},", (unsigned long long)t0);
        db_printf(&d, "{\"name\":\"navigationStart\",\"ph\":\"R\",\"pid\":2,\"tid\":1,\"ts\":%llu,\"cat\":\"blink.user_timing\",\"args\":{\"data\":{\"frame\":\"1\",\"documentLoaderURL\":\"index.html\",\"navigationId\":\"1\"}}},", (unsigned long long)t0);
        db_printf(&d, "{\"name\":\"CommitLoad\",\"ph\":\"X\",\"pid\":2,\"tid\":1,\"ts\":%llu,\"dur\":%llu,\"cat\":\"devtools.timeline\",\"args\":{\"data\":{\"frame\":\"1\"}}},", (unsigned long long)t0, (unsigned long long)((t1 - t0) / 10));
        db_printf(&d, "{\"name\":\"RunTask\",\"ph\":\"X\",\"pid\":2,\"tid\":1,\"ts\":%llu,\"dur\":%llu,\"cat\":\"toplevel\",\"args\":{}},", (unsigned long long)(t0 + (t1 - t0) / 5), (unsigned long long)((t1 - t0) / 2));
        db_printf(&d, "{\"name\":\"Layout\",\"ph\":\"X\",\"pid\":2,\"tid\":1,\"ts\":%llu,\"dur\":%llu,\"cat\":\"devtools.timeline\",\"args\":{\"beginData\":{\"frame\":\"1\"}}},", (unsigned long long)(t0 + (t1 - t0) / 4), (unsigned long long)((t1 - t0) / 20));
        db_printf(&d, "{\"name\":\"Paint\",\"ph\":\"X\",\"pid\":2,\"tid\":1,\"ts\":%llu,\"dur\":%llu,\"cat\":\"devtools.timeline\",\"args\":{\"data\":{\"frame\":\"1\"}}}", (unsigned long long)(t0 + (t1 - t0) / 3), (unsigned long long)((t1 - t0) / 20));

        db_puts(&d, "]}}");
        char *s = db_finish(&d);
        if (s) { mini_cdp_broadcast(cdp, s, d.len); free(s); }

        DBuf d2 = {0};
        db_puts(&d2, "{\"method\":\"Tracing.tracingComplete\",\"params\":{\"dataLossOccurred\":false}}");
        char *s2 = db_finish(&d2);
        if (s2) { mini_cdp_broadcast(cdp, s2, d2.len); free(s2); }
        return;
    }
    if (!strcmp(m, "getCategories"))
    {
        reply_result(cdp, ci, id, "{\"categories\":[\"devtools.timeline\",\"disabled-by-default-devtools.timeline\",\"blink.user_timing\"]}");
        return;
    }
    if (!strcmp(m, "getBufferUsage"))
    {
        reply_result(cdp, ci, id, "{\"percentFull\":0.1,\"eventCount\":10,\"value\":0.1}");
        return;
    }
    reply_empty(cdp, ci, id);
}

/* ================================================================== */
/* Top-level dispatcher                                                 */
/* ================================================================== */
void mini_cdp_dispatch(MiniCDP *cdp, int ci, const char *msg)
{
    long id = cdp_jint(msg, "id");
    char method[128];
#ifdef CDP_DEBUG_DUMP
    {
        static int s_dump = -1;
        if (s_dump < 0) { const char *e = getenv("CDP_DEBUG"); s_dump = (e && (e[0]=='1'||e[0]=='t'||e[0]=='T'))?1:0; }
        if (s_dump)
        {
            size_t mlen = strlen(msg);
            size_t n = mlen > 300 ? 300 : mlen;
            fprintf(stderr, "[cdp<-] %.*s%s\n", (int)n, msg, n < mlen ? "..." : "");
        }
    }
#endif
    if (cdp_jstr(msg, "method", method, sizeof method) < 0)
    {
        reply_empty(cdp, ci, id);
        return;
    }
    const char *m = method;

    if (!strncmp(m, "Runtime.", 8)) { runtime_domain(cdp, ci, msg, id, m + 8); return; }
    if (!strncmp(m, "Debugger.", 9)) { debugger_domain(cdp, ci, msg, id, m + 9); return; }
    if (!strncmp(m, "DOM.", 4))      { dom_domain(cdp, ci, msg, id, m + 4); return; }
    if (!strncmp(m, "CSS.", 4))      { css_domain(cdp, ci, msg, id, m + 4); return; }
    if (!strncmp(m, "Overlay.", 8))  { overlay_domain(cdp, ci, msg, id, m + 8); return; }
    if (!strncmp(m, "Page.", 5))     { page_domain(cdp, ci, msg, id, m + 5); return; }
    if (!strncmp(m, "Network.", 8))     { network_domain(cdp, ci, msg, id, m + 8); return; }
    if (!strncmp(m, "DOMDebugger.", 12)){ domdebugger_domain(cdp, ci, msg, id, m + 12); return; }
    if (!strncmp(m, "Tracing.", 8))     { tracing_domain(cdp, ci, msg, id, m + 8); return; }
    if (!strncmp(m, "Log.", 4))         { log_domain(cdp, ci, msg, id, m + 4); return; }
    if (!strncmp(m, "Target.", 7))      { target_domain(cdp, ci, msg, id, m + 7); return; }
    if (!strncmp(m, "Performance.", 12)){ performance_domain(cdp, ci, msg, id, m + 12); return; }
    if (!strncmp(m, "Emulation.", 10)) { emulation_domain(cdp, ci, msg, id, m + 10); return; }
    if (!strncmp(m, "Storage.", 8))     { storage_domain(cdp, ci, msg, id, m + 8); return; }
    if (!strncmp(m, "Input.", 6))       { input_domain(cdp, ci, msg, id, m + 6); return; }
    if (!strncmp(m, "HeapProfiler.", 13)) { heapprofiler_domain(cdp, ci, msg, id, m + 13); return; }
    if (!strncmp(m, "Profiler.", 9)) { profiler_domain(cdp, ci, msg, id, m + 9); return; }
    if (!strncmp(m, "SystemInfo.", 11)) { reply_empty(cdp, ci, id); return; }
    if (!strncmp(m, "Security.", 9)) { reply_empty(cdp, ci, id); return; }

    /* Catch-all: acknowledge so DevTools never hangs on an id. */
    reply_empty(cdp, ci, id);
}
