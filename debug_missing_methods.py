"""
debug_missing_methods.py — find which DOM/element/window methods the React SPA
calls that the engine does NOT define, so a "not a function" during render can
be pinpointed (the QuickJS error column lands inside i18n string data for this
source-map-less minified bundle, so it's useless).

It scrapes the engine's defined method names from mini_js_bridge.c (the C
element/document/window prototypes + the JS-shim prototype assignments) and
cross-references them against every `.name(` method call in the bundle.
"""
import os
import re

ROOT = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(ROOT, "src", "mini_js_bridge.c")
BUNDLE = os.path.join(ROOT, "build", "idx.js")


def engine_methods():
    src = open(BRIDGE, encoding="utf-8", errors="replace").read()
    names = set()
    # C-side element/document proto: SET(proto, "foo", ...)  and
    # JS_SetPropertyStr(ctx, dproto/eproto/proto, "foo", ...)
    for m in re.findall(r'(?:SET|JS_SetPropertyStr)\([^,]*,\s*"([a-zA-Z_$][\w$]*)"', src):
        names.add(m)
    # JS shim: MiniElement.prototype.foo = / MiniDocument.prototype.foo = /
    # window.foo = / globalThis.foo = / document.foo =
    for m in re.findall(r'(?:MiniElement|MiniDocument)\.prototype\.([a-zA-Z_$][\w$]*)\s*=', src):
        names.add(m)
    for m in re.findall(r'(?:window|globalThis|document)\.([a-zA-Z_$][\w$]*)\s*=', src):
        names.add(m)
    return names


def bundle_methods():
    src = open(BUNDLE, encoding="utf-8", errors="replace").read()
    return re.findall(r'\.([a-zA-Z_$][\w$]*)\s*\(', src)


def main():
    eng = engine_methods()
    builtins = set("""hasOwnProperty,toString,valueOf,constructor,call,apply,bind,then,catch,finally,resolve,reject,push,pop,shift,unshift,slice,splice,join,indexOf,lastIndexOf,substring,substr,toLowerCase,toUpperCase,charAt,charCodeAt,split,replace,search,match,trim,concat,includes,startsWith,endsWith,map,filter,reduce,forEach,find,findIndex,some,every,sort,reverse,keys,values,entries,get,set,has,delete,append,add,fetch,eval,parseInt,parseFloat,isNaN,isFinite,toFixed,toPrecision,toExponential,test,exec,next,done,log,warn,info,error,debug,isArray,from,of,is,fromEntries,defineProperty,defineProperties,assign,create,freeze,getOwnPropertyNames,getPrototypeOf,getOwnPropertyDescriptor,toJSON,encodeURIComponent,decodeURIComponent,setTimeout,clearTimeout,setInterval,clearInterval,requestAnimationFrame,cancelAnimationFrame,addEventListener,removeEventListener,dispatchEvent,createElement,createTextNode,createComment,getElementById,querySelector,querySelectorAll,createDocumentFragment,createElementNS,setAttribute,getAttribute,removeAttribute,hasAttribute,appendChild,removeChild,insertBefore,replaceChild,cloneNode,getContext,getBoundingClientRect,getClientRects""".split(","))
    eng |= builtins

    calls = bundle_methods()
    from collections import Counter
    c = Counter(calls)
    missing = sorted((n, k) for n, k in c.items() if n not in eng and not n.startswith("_"))
    print("engine defines ~%d method names; bundle makes %d distinct calls" % (len(eng), len(c)))
    print("---- methods called by the bundle but NOT defined by the engine ----")
    for n, k in missing:
        print("  %-28s x%d" % (n, k))


if __name__ == "__main__":
    main()
