# TinyFramework

A tiny, from-scratch browser engine and desktop app runtime written in pure C99 — no Electron, no Chromium, no Node.

The whole stack — GL context, WHATWG HTML5 parser, CSS engine, DOM + layout, JavaScript, networking, TLS, DevTools — is hand-written in one C codebase. It ships in two modes behind a single API: a self-written renderer (full features, ~10 MB) or the OS WebView (tiny, ~2 MB).

[![Language](https://img.shields.io/badge/C99-blue.svg)](https://en.cppreference.com/w/c/99)
[![Platform](https://img.shields.io/badge/Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)](#platforms)
[![License](https://img.shields.io/badge/MIT-green.svg)](./LICENSE)

## Why

You can build a modern web-app-as-desktop-app in a few megabytes that cold-starts in ~150 ms and idles near 0% CPU. There is no bundled browser, no V8, no Node runtime — just a compact engine you can read end to end. It is also a clean base for experimenting with rendering, layout, and browser-protocol internals.

## Highlights

**Rendering** — vector command buffer flushed once per frame (state-sorted to minimise GL churn), stb_truetype glyph atlas with an 8-font fallback chain, scissor + rounded-corner clipping, LRU gradient cache, framebuffer→PNG screenshots.

**DOM & layout** — O(1) parent/child/sibling pointers mirroring the DOM, Block + Flexbox + Grid layout, Shadow DOM + slots, `::before/::after/::placeholder`, dirty-mark incremental relayout, full Canvas 2D path API.

**CSS** — `--var` with `var(...[, fallback])`, a recursive-descent `calc()` evaluator (units, `min/max/clamp`, `sin/cos/tan/sqrt/...`), `@media`, `@keyframes` with cubic-bezier easing, `@font-face`.

**HTML5** — a faithful WHATWG tokenizer + tree-construction (insertion modes, open-elements stack, Adoption Agency Algorithm, foster-parenting), streaming `mini_html5_feed()`.

**JavaScript (QuickJS, baseline JIT + SLJIT)** — DOM/BOM/WebGL polyfills, ES modules with `import.meta.url`, `localStorage`/`sessionStorage`, an `EventEmitter`-style surface, and a growing set of Web-API polyfills (`URL`/`URLSearchParams`, `Headers`/`Request`/`Response`/`fetch`, `FormData`, `AbortController`, `crypto`, `Intl`, `IntersectionObserver`, …).

**Electron-style main/renderer surface** — `app` lifecycle & paths, multi-instance `BrowserWindow` (each window is its own renderer process/context), `ipcMain`/`ipcRenderer`/`webContents`, `net.request`, `session` (cookies/cache/proxy), `protocol` custom-scheme handlers, `os`/`process`/`fs`/`path`/`child_process` via a CommonJS `require`.

**Networking & security** — HTTP/1.1 + HTTPS (vendored MbedTLS), `fetch` orchestration (HSTS, cookies, HTTP cache with conditional revalidation, CORS gate), RFC 6265 cookie jar, HSTS/SOP/CORS/CSP, RFC 6455 WebSocket, HTTP/2 frame codec.

**Tooling** — a Chrome DevTools Protocol server on :9222 (console, Elements, Runtime, Debugger, HeapProfiler, Page screenshot, overlay, Network, Emulation), an in-engine DevTools overlay (F12), per-frame profiler, structured ring-buffer logger, cross-platform crash handler.

**Packing** — ChaCha20-Poly1305 (RFC 8439) VFS; JS can be compiled to QuickJS bytecode, encrypted, and executed from RAM (never touches disk). Constant-time tag verification.

## Quick start

You need a C99 compiler (GCC/Clang/MSVC), CMake ≥ 3.16, and GLFW 3.4. Python 3 is optional for the packing script.

```bash
# CMake (recommended)
cmake -B build -S . -DGLFW_ROOT=/path/to/glfw-3.4 -DMINI_TLS=ON
cmake --build build --config Release
cmake --build build --target check     # self-tests

# Run an HTML entry
./build/tiny_app src/index.html

# DevTools: open chrome://inspect
TINY_CDP_PORT=9222 ./build/tiny_app src/index.html

# Headless run: render N frames, screenshot, then exit
TINY_FRAMES=300 ./build/tiny_app src/index.html   # -> build/shot.png
```

Windows (MSYS2/MinGW) also has a plain script:

```powershell
.\build.ps1          # -> build\tiny_app.exe + build\tiny_app.debug
```

The Python packer compiles JS to QuickJS bytecode and encrypts it:

```bash
python build.py --in src/ --out build/ --bytecode --key <32-byte-hex> --jit --jit-codegen --upx
python build.py --help
```

`main()` resolves the entry in this order: `argv[1]` → `entry` in `app.config.json` → `app.pak` (encrypted VFS) → `src/index.html` → `index.html` → `test_suite.js`.

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  Host app (main.c): mini_app_create → load → run → destroy    │
├──────────────────────────────────────────────────────────────┤
│  Unified API (mini_framework.h) — identical in both modes    │
├───────────────┬───────────────┬───────────────┬──────────────┤
│ platform/win  │ renderer      │ DOM + layout   │ JS (QuickJS) │
│ (GLFW/GL)     │ (vector buf)  │ (dom/css)      │ + bridge     │
├───────────────┼───────────────┼───────────────┼──────────────┤
│ html5 parser  │ event loop    │ input/W3C      │ native/worker│
├───────────────┴───────────────┴───────────────┴──────────────┤
│  net · cookies · httpcache · policy · websocket · h2/h3       │
├────────────────────────────────────────────────────────────────┤
│  cdp · devtools · diag · log · crash                           │
├────────────────────────────────────────────────────────────────┤
│  vfs: ChaCha20-Poly1305 packing + QuickJS bytecode            │
└────────────────────────────────────────────────────────────────┘
```

The engine is single-threaded: the main loop runs on the QuickJS thread (lock-free); only the background worker pool does blocking I/O, and worker threads never touch JSValues, JSContext, GL, or the DOM.

Per frame: `diag.begin → poll events → rAF → pump jobs → layout → render+flush → diag.end → cdp.poll`. Idle gating skips the whole relayout/render/swap pipeline on a static page and sleeps via `glfwWaitEventsTimeout`; dirty marks gate restyle/layout/paint independently.

## Modules

| Layer | Files |
| --- | --- |
| API / host | `mini_framework.h`, `main.c`, `mini_window.*` |
| Rendering | `mini_renderer.*`, `mini_gradient.c`, `mini_png.*` |
| DOM & layout | `mini_dom.*`, `mini_css.*`, `mini_bidi.*`, `mini_shaping.*` |
| HTML5 / CSS parse | `mini_html5.*` |
| JS bridge | `mini_js_bridge.*`, `mini_webgl_ext.*` |
| Main-process surface | `mini_native.*`, `mini_ipc.*`, `mini_protocol.*` |
| Networking | `mini_net.*`, `mini_cookies.*`, `mini_httpcache.*`, `mini_policy.*`, `mini_websocket.*`, `mini_h2.*`, `mini_h3.*` |
| Events / loop | `mini_events.*`, `mini_eventloop.*`, `mini_worker.*` |
| Observability | `mini_cdp_*`, `mini_devtools.*`, `mini_diag.*`, `mini_log.*`, `mini_crash.*` |
| Packing | `mini_vfs*` |

## Build options

| CMake option | Default | What it does |
| --- | --- | --- |
| `ENABLE_CDP` | `ON` | Embed the Chrome DevTools Protocol server |
| `ENABLE_DIAG` | `ON` | Embed the runtime profiler/diagnostics |
| `TINY_NATIVE` | `OFF` | Build against the OS WebView instead of the self-written engine |
| `MINI_JIT` | `ON` | Build QuickJS with the baseline JIT |
| `MINI_JIT_CODEGEN` | `ON` | Enable SLJIT native codegen (implies `MINI_JIT`) |
| `MINI_TLS` | `ON` | Enable HTTPS via vendored MbedTLS |
| `GUI_APP` | `ON` | Windows GUI subsystem (`-mwindows`) |
| `GLFW_ROOT` | `D:/glfw-3.4.bin.WIN64` | Path to a prebuilt GLFW SDK |

## Platforms

| Platform | Status | Notes |
| --- | --- | --- |
| Windows | Primary | Win32 IMM IME, SEH crash handler, GUI subsystem |
| Linux | Builds | POSIX `sigaction` crash handler + alt stack |
| macOS | Builds | Same as Linux |

## Bundled dependencies

| Library | Used for | License |
| --- | --- | --- |
| [QuickJS](https://bellard.org/quickjs/) | JS engine (+ JIT) | MIT |
| [MbedTLS](https://github.com/Mbed-TLS/mbedtls) | TLS / HTTPS | Apache-2.0 |
| [stb](https://github.com/nothings/stb) | image decode / truetype glyphs | Public Domain |
| [SLJIT](https://github.com/zherczeg/sljit) | QuickJS native codegen | BSD-2-Clause |
| [GLFW](https://www.glfw.org/) | window / input / GL context | Zlib |

Each retains its own license under `libs/`.

## License

MIT — see [LICENSE](./LICENSE). Bundled third-party libraries keep their original licenses.
