/*
 * mini_ipc.c — see mini_ipc.h.
 *
 * Single-threaded model: the host run loop + all GLFW callbacks + all JS run on
 * one thread, so the mailbox needs no lock. Each context (main + renderers) is
 * a separate QuickJS runtime; args are JSON-serialized across the boundary.
 * The run loop drains each context's mailbox during that context's phase and
 * calls its JS listeners/handlers there (no cross-context re-entrancy, except
 * the best-effort sendSync which re-enters the main context synchronously).
 */
#include "mini_ipc.h"
#include "mini_js_bridge.h"
#include "mini_native.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- message + mailbox ------------------------------------------------ */
enum { IPC_SEND = 0, IPC_INVOKE = 1, IPC_INVOKE_RESP = 2 };

typedef struct IpcMsg
{
    int src;     /* source ctx id (0=main, >0=renderer)            */
    int dst;     /* destination ctx id                              */
    int kind;    /* IPC_SEND / IPC_INVOKE / IPC_INVOKE_RESP         */
    int req_id;  /* for invoke / invoke_resp                        */
    int ok;      /* for invoke_resp (1 resolved, 0 rejected)       */
    char *channel;
    char *args;   /* JSON string (send/invoke); may be NULL         */
    char *result; /* JSON string (invoke_resp); may be NULL         */
} IpcMsg;

struct MiniIPC
{
    IpcMsg *q;
    int n, cap;
    int next_req_id;
    struct MiniBridge *main_bridge; /* for sendSync re-entry */
};

MiniIPC *mini_ipc_create(void)
{
    MiniIPC *ipc = (MiniIPC *)calloc(1, sizeof(*ipc));
    if (ipc)
        ipc->next_req_id = 1;
    return ipc;
}

void mini_ipc_destroy(MiniIPC *ipc)
{
    if (!ipc)
        return;
    for (int i = 0; i < ipc->n; i++)
    {
        free(ipc->q[i].channel);
        free(ipc->q[i].args);
        free(ipc->q[i].result);
    }
    free(ipc->q);
    free(ipc);
}

static void ipc_push(MiniIPC *ipc, IpcMsg *m)
{
    if (ipc->n >= ipc->cap)
    {
        int nc = ipc->cap ? ipc->cap * 2 : 16;
        IpcMsg *nq = (IpcMsg *)realloc(ipc->q, (size_t)nc * sizeof(*nq));
        if (!nq)
        {
            free(m->channel); free(m->args); free(m->result);
            return;
        }
        ipc->q = nq; ipc->cap = nc;
    }
    ipc->q[ipc->n++] = *m;
}

/* Post a 'send' message from src -> dst (public: used by webContents.send). */
void mini_ipc_post_send(MiniIPC *ipc, int src, int dst,
                        const char *channel, const char *args_json)
{
    if (!ipc || !channel)
        return;
    IpcMsg m;
    memset(&m, 0, sizeof(m));
    m.src = src; m.dst = dst; m.kind = IPC_SEND;
    m.channel = strdup(channel);
    m.args = args_json ? strdup(args_json) : NULL;
    ipc_push(ipc, &m);
}

/* ---- native functions exposed to each context's JS ------------------- */

static MiniIPC *ipc_of(JSContext *ctx)
{
    struct MiniBridge *b = (struct MiniBridge *)JS_GetContextOpaque(ctx);
    return b ? (MiniIPC *)mini_bridge_get_ipc(b) : NULL;
}
static int ctx_id_of(JSContext *ctx)
{
    struct MiniBridge *b = (struct MiniBridge *)JS_GetContextOpaque(ctx);
    return b ? mini_bridge_get_window_id(b) : 0;
}

/* __ipcSend(dst, channel, argsJson) */
static JSValue js_ipc_send(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    MiniIPC *ipc = ipc_of(ctx);
    int dst = 0;
    const char *ch = NULL, *args = NULL;
    if (argc > 0) JS_ToInt32(ctx, &dst, argv[0]);
    if (argc > 1 && JS_IsString(argv[1])) ch = JS_ToCString(ctx, argv[1]);
    if (argc > 2 && JS_IsString(argv[2])) args = JS_ToCString(ctx, argv[2]);
    mini_ipc_post_send(ipc, ctx_id_of(ctx), dst, ch ? ch : "", args);
    JS_FreeCString(ctx, ch);
    JS_FreeCString(ctx, args);
    return JS_UNDEFINED;
}

/* __ipcInvoke(dst, channel, argsJson) -> reqId (int) */
static JSValue js_ipc_invoke(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    MiniIPC *ipc = ipc_of(ctx);
    if (!ipc)
        return JS_ThrowTypeError(ctx, "ipc: no registry");
    int dst = 0;
    const char *ch = NULL, *args = NULL;
    if (argc > 0) JS_ToInt32(ctx, &dst, argv[0]);
    if (argc > 1 && JS_IsString(argv[1])) ch = JS_ToCString(ctx, argv[1]);
    if (argc > 2 && JS_IsString(argv[2])) args = JS_ToCString(ctx, argv[2]);
    int req_id = ipc->next_req_id++;
    IpcMsg m;
    memset(&m, 0, sizeof(m));
    m.src = ctx_id_of(ctx); m.dst = dst; m.kind = IPC_INVOKE; m.req_id = req_id;
    m.channel = strdup(ch ? ch : "");
    m.args = args ? strdup(args) : NULL;
    ipc_push(ipc, &m);
    JS_FreeCString(ctx, ch);
    JS_FreeCString(ctx, args);
    return JS_NewInt32(ctx, req_id);
}

/* __ipcPostResponse(reqId, dst, ok, resultJson) — called by the main async helper */
static JSValue js_ipc_post_response(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    MiniIPC *ipc = ipc_of(ctx);
    int req_id = 0, dst = 0, ok = 0;
    const char *res = NULL;
    if (argc > 0) JS_ToInt32(ctx, &req_id, argv[0]);
    if (argc > 1) JS_ToInt32(ctx, &dst, argv[1]);
    if (argc > 2) ok = JS_ToBool(ctx, argv[2]);
    if (argc > 3 && JS_IsString(argv[3])) res = JS_ToCString(ctx, argv[3]);
    if (ipc)
    {
        IpcMsg m;
        memset(&m, 0, sizeof(m));
        m.src = 0; m.dst = dst; m.kind = IPC_INVOKE_RESP; m.req_id = req_id; m.ok = ok;
        m.result = res ? strdup(res) : NULL;
        ipc_push(ipc, &m);
    }
    JS_FreeCString(ctx, res);
    return JS_UNDEFINED;
}

/* ---- the per-context JS glue shim ----------------------------------- */
static const char *ipc_shim =
"(function(){\n"
"  var isMain = !!globalThis.__ipcIsMain;\n"
"  var ctxId = globalThis.__ipcCtxId|0;\n"
"  var pendingInvokes = new Map();   /* renderer: reqId -> {resolve,reject} */\n"
"  var mainListeners = new Map();     /* main: channel -> [cb]               */\n"
"  var mainHandlers = new Map();      /* main: channel -> handler            */\n"
"  var renListeners = new Map();     /* renderer: channel -> [cb]           */\n"
"  function arr(m,ch){ var a=m.get(ch); if(!a){a=[]; m.set(ch,a);} return a; }\n"
"  function addOn(m,ch,cb,once){ cb.__once=!!once; arr(m,ch).push(cb); return cb; }\n"
"  function rmOn(m,ch,cb){ var a=m.get(ch); if(a){ m.set(ch, a.filter(function(x){return x!==cb;})); } }\n"
"  function makeSender(srcId){ return { id: srcId, frameId:null,\n"
"    send: function(ch){ var a=[]; for(var i=1;i<arguments.length;i++)a.push(arguments[i]); __ipcSend(srcId,ch,JSON.stringify(a)); },\n"
"    sendToFrame: function(){}, executeJavaScript: function(){} };\n"
"  }\n"
"  globalThis.__ipcDispatchSend = function(channel, argsJson, srcId){\n"
"    var args = argsJson ? JSON.parse(argsJson) : [];\n"
"    var ev = { sender: makeSender(srcId), frameId: null,\n"
"      preventDefault: function(){}, defaultPrevented:false,\n"
"      reply: function(ch){ var a=[]; for(var i=1;i<arguments.length;i++)a.push(arguments[i]); __ipcSend(srcId,ch,JSON.stringify(a)); } };\n"
"    var m = isMain ? mainListeners : renListeners;\n"
"    var a = m.get(channel); if(a){ a=a.slice(); for(var i=0;i<a.length;i++){ var cb=a[i];\n"
"      try{ cb.apply(null,[ev].concat(args)); }catch(e){ if(typeof console!=='undefined')console.error('ipc listener',e); }\n"
"      if(cb.__once) rmOn(m,channel,cb); } }\n"
"  };\n"
"  globalThis.__ipcDispatchInvokeMain = async function(channel, reqId, srcId, argsJson){\n"
"    var args = argsJson ? JSON.parse(argsJson) : [];\n"
"    var ev = { sender: makeSender(srcId), frameId: null };\n"
"    var h = mainHandlers.get(channel);\n"
"    try{ if(!h) throw new Error('No handler registered for '+channel);\n"
"      var r = await h.apply(null,[ev].concat(args));\n"
"      __ipcPostResponse(reqId, srcId, 1, r===undefined? 'undefined' : JSON.stringify(r)); }\n"
"    catch(e){ __ipcPostResponse(reqId, srcId, 0, JSON.stringify(String(e&&e.message||e))); }\n"
"  };\n"
"  globalThis.__ipcResolveInvoke = function(reqId, ok, resultJson){\n"
"    var p = pendingInvokes.get(reqId); if(!p) return;\n"
"    pendingInvokes.delete(reqId);\n"
"    try{ if(ok){ p.resolve(resultJson? JSON.parse(resultJson): undefined); }\n"
"      else { var msg = resultJson? JSON.parse(resultJson): 'ipc error'; p.reject(new Error(typeof msg==='string'?msg:JSON.stringify(msg))); } }\n"
"    catch(e){ p.reject(e); }\n"
"  };\n"
"  globalThis.__ipcSyncMain = function(channel, argsJson){ /* best-effort: sync handler only */\n"
"    var h = mainHandlers.get(channel); if(!h) return 'undefined';\n"
"    var args = argsJson? JSON.parse(argsJson): [];\n"
"    try{ var r = h.apply(null,[{sender:null,frameId:null}].concat(args)); return r===undefined? 'undefined': JSON.stringify(r); }\n"
"    catch(e){ return JSON.stringify(null); }\n"
"  };\n"
"  if(isMain){\n"
"    var ipcMain = { on:function(ch,cb){ addOn(mainListeners,ch,cb,false); return ipcMain; },\n"
"      once:function(ch,cb){ addOn(mainListeners,ch,cb,true); return ipcMain; },\n"
"      off:function(ch,cb){ rmOn(mainListeners,ch,cb); return ipcMain; },\n"
"      removeListener:function(ch,cb){ rmOn(mainListeners,ch,cb); return ipcMain; },\n"
"      removeAllListeners:function(ch){ if(ch) mainListeners.delete(ch); else mainListeners.clear(); return ipcMain; },\n"
"      handle:function(ch,h){ mainHandlers.set(ch,h); return ipcMain; },\n"
"      removeHandler:function(ch){ mainHandlers.delete(ch); return ipcMain; } };\n"
"    globalThis.__ipcMain = ipcMain;\n"
"    var e = globalThis.electron || (globalThis.electron = {}); e.ipcMain = ipcMain;\n"
"  } else {\n"
"    var ipcRenderer = { send:function(ch){ var a=[]; for(var i=1;i<arguments.length;i++)a.push(arguments[i]); __ipcSend(0,ch,JSON.stringify(a)); },\n"
"      invoke:function(ch){ var a=[]; for(var i=1;i<arguments.length;i++)a.push(arguments[i]); var rid=__ipcInvoke(0,ch,JSON.stringify(a)); return new Promise(function(res,rej){ pendingInvokes.set(rid,{resolve:res,reject:rej}); }); },\n"
"      sendSync:function(ch){ var a=[]; for(var i=1;i<arguments.length;i++)a.push(arguments[i]); var r=__ipcSyncMain(ch,JSON.stringify(a)); return r==='undefined'?undefined:JSON.parse(r); },\n"
"      on:function(ch,cb){ addOn(renListeners,ch,cb,false); return ipcRenderer; },\n"
"      once:function(ch,cb){ addOn(renListeners,ch,cb,true); return ipcRenderer; },\n"
"      off:function(ch,cb){ rmOn(renListeners,ch,cb); return ipcRenderer; },\n"
"      removeListener:function(ch,cb){ rmOn(renListeners,ch,cb); return ipcRenderer; },\n"
"      removeAllListeners:function(ch){ if(ch) renListeners.delete(ch); else renListeners.clear(); return ipcRenderer; } };\n"
"    globalThis.__ipcRenderer = ipcRenderer;\n"
"    var e = globalThis.electron || (globalThis.electron = {}); e.ipcRenderer = ipcRenderer;\n"
"  }\n"
"})();\n";

void mini_ipc_install(MiniIPC *ipc, struct MiniBridge *b, int is_main, int ctx_id)
{
    if (!ipc || !b)
        return;
    mini_bridge_set_ipc(b, ipc);
    if (is_main)
        ipc->main_bridge = b;
    JSContext *ctx = mini_bridge_ctx(b);
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__ipcIsMain", JS_NewBool(ctx, is_main));
    JS_SetPropertyStr(ctx, global, "__ipcCtxId", JS_NewInt32(ctx, ctx_id));
    JS_SetPropertyStr(ctx, global, "__ipcSend", JS_NewCFunction(ctx, js_ipc_send, "__ipcSend", 3));
    JS_SetPropertyStr(ctx, global, "__ipcInvoke", JS_NewCFunction(ctx, js_ipc_invoke, "__ipcInvoke", 3));
    JS_SetPropertyStr(ctx, global, "__ipcPostResponse", JS_NewCFunction(ctx, js_ipc_post_response, "__ipcPostResponse", 4));
    JS_FreeValue(ctx, global);
    /* The shim only defines functions/Maps (no DOM access), so we don't need
       to switch the active document. Just eval it in this context. */
    JSValue r = JS_Eval(ctx, ipc_shim, strlen(ipc_shim), "<ipc-shim>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r))
    {
        JSValue ex = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, ex);
        fprintf(stderr, "[ipc] shim eval failed: %s\n", s ? s : "?");
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, ex);
    }
    JS_FreeValue(ctx, r);
}

/* ---- drain + dispatch (called by the run loop per context) ---------- */

static JSValue js_global_call_str3(JSContext *ctx, const char *fn,
                                   const char *a, const char *b, int c)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue f = JS_GetPropertyStr(ctx, global, fn);
    JSValue args[3];
    args[0] = a ? JS_NewString(ctx, a) : JS_NULL;
    args[1] = b ? JS_NewString(ctx, b) : JS_NULL;
    args[2] = JS_NewInt32(ctx, c);
    JSValue r = JS_IsFunction(ctx, f) ? JS_Call(ctx, f, JS_UNDEFINED, 3, args) : JS_UNDEFINED;
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, global);
    return r;
}
static JSValue js_global_call_si_s(JSContext *ctx, const char *fn,
                                  const char *a, int reqId, int srcId, const char *b)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue f = JS_GetPropertyStr(ctx, global, fn);
    JSValue args[4];
    args[0] = a ? JS_NewString(ctx, a) : JS_NULL;
    args[1] = JS_NewInt32(ctx, reqId);
    args[2] = JS_NewInt32(ctx, srcId);
    args[3] = b ? JS_NewString(ctx, b) : JS_NULL;
    JSValue r = JS_IsFunction(ctx, f) ? JS_Call(ctx, f, JS_UNDEFINED, 4, args) : JS_UNDEFINED;
    for (int i = 0; i < 4; i++) JS_FreeValue(ctx, args[i]);
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, global);
    return r;
}
static JSValue js_global_call_ii_s(JSContext *ctx, const char *fn, int a, int b, const char *c)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue f = JS_GetPropertyStr(ctx, global, fn);
    JSValue args[3];
    args[0] = JS_NewInt32(ctx, a);
    args[1] = JS_NewInt32(ctx, b);
    args[2] = c ? JS_NewString(ctx, c) : JS_NULL;
    JSValue r = JS_IsFunction(ctx, f) ? JS_Call(ctx, f, JS_UNDEFINED, 3, args) : JS_UNDEFINED;
    for (int i = 0; i < 3; i++) JS_FreeValue(ctx, args[i]);
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, global);
    return r;
}

void mini_ipc_dispatch(MiniIPC *ipc, struct MiniBridge *b, int ctx_id)
{
    if (!ipc || !b)
        return;
    JSContext *ctx = mini_bridge_ctx(b);
    if (!ctx)
        return;
    /* switch active globals to this context so any DOM ops in listeners use it */
    mini_bridge_set_active(b);

    /* drain every message addressed to this context */
    for (int i = 0; i < ipc->n; )
    {
        IpcMsg *m = &ipc->q[i];
        if (m->dst != ctx_id)
        { i++; continue; }
        /* consume: copy out, then compact the queue */
        IpcMsg msg = *m;
        for (int j = i; j < ipc->n - 1; j++)
            ipc->q[j] = ipc->q[j + 1];
        ipc->n--;
        /* re-fetch pointer (compaction moved things) — use the copy */
        (void)m;

        JSValue r;
        if (msg.kind == IPC_SEND)
        {
            r = js_global_call_str3(ctx, "__ipcDispatchSend",
                                    msg.channel, msg.args, msg.src);
        }
        else if (msg.kind == IPC_INVOKE && ctx_id == 0)
        {
            /* main handles an invoke (async helper posts the response) */
            r = js_global_call_si_s(ctx, "__ipcDispatchInvokeMain",
                                    msg.channel, msg.req_id, msg.src, msg.args);
        }
        else if (msg.kind == IPC_INVOKE_RESP)
        {
            r = js_global_call_ii_s(ctx, "__ipcResolveInvoke",
                                    msg.req_id, msg.ok, msg.result);
        }
        else
            r = JS_UNDEFINED;
        JS_FreeValue(ctx, r);

        free(msg.channel);
        free(msg.args);
        free(msg.result);
        /* i stays — the compacted slot at i is new */
    }
}
