# TinyFramework

> 一个用纯 C99 从零编写的极简、超高性能浏览器引擎 / 桌面应用运行时 —— 无需 Electron / Chromium / Node.js。

[![Language](https://img.shields.io/badge/language-C99-blue.svg)](https://en.cppreference.com/w/c/99)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)](#平台支持)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](#开源许可)

---

## 📖 项目简介

TinyFramework 是一个**完全自研**的轻量级浏览器内核与桌面应用框架。它不依赖任何庞大的浏览器运行时，而是从底层 GPU 上下文、HTML5 解析器、CSS 引擎、DOM 树、布局引擎到 JavaScript 引擎全部从零实现，旨在以极小的体积和极低的资源占用提供现代 Web 技术栈的完整体验。

### 核心目标

| 指标 | 自研模式 (CUSTOM_MINI) | 原生模式 (NATIVE) |
|------|----------------------|-------------------|
| 二进制体积 | < 10 MB | < 2 MB |
| 冷启动时间 | ~0.15 秒 | ~0.1 秒 |
| 内存占用 | 10 ~ 20 MB | 5 ~ 10 MB |
| JS 引擎 | QuickJS (含基线 JIT + SLJIT 代码生成) | OS WebView |

> **双模式设计**：同一套 `MiniApp*` API 在两种编译模式下逐字节相同 —— NATIVE 模式委托给系统 WebView（体积优先），CUSTOM_MINI 模式（`-DENABLE_CUSTOM_MINI_ENGINE`）使用全部自研渲染管线（功能优先）。宿主代码无需感知差异。

---

## ✨ 核心特性

### 🎨 自研渲染引擎
- **向量命令缓冲**：先录制全部绘制命令，每帧统一刷新，按状态排序以最小化 `glUseProgram` / `glBindTexture` 切换
- **TrueType 字形图集**：基于 `stb_truetype`，多字体回退链（最多 8 种），共享纹理，CPU 镜像合成（无 GL 回读）
- **剪裁栈**：scissor + 圆角剪裁（基于 stencil，回退到 scissor）
- **渐变缓存**：LRU 缓存（16 槽），支持 linear / radial / conic；纯 CPU 渐变光栅化器
- **截图**：帧缓冲转 PNG（Y 翻转），供 CDP `Page.captureScreenshot` 使用

### 📐 DOM + 布局引擎
- **场景图**：O(1) 父子兄弟指针，镜像 DOM NodeType，覆盖 Vue 3 / React 18 vDOM diff 所需的 Node/Element API
- **布局引擎**：Block 流 + Flexbox + Grid
- **Web 组件**：Shadow DOM + slots、`::before` / `::after` / `::placeholder` 伪元素
- **脏标记增量**：`doc->dirty`（重排）与 `doc->layout_dirty` 门控布局；`paint_dirty` 门控全量重绘
- **Canvas 2D**：完整的路径录制 API（begin_path / line_to / arc / fill / stroke / save / restore / transform / fill_text / measure_text）

### 🎭 CSS 值引擎
- **CSS 变量**：`--foo` + `var(--foo [, fallback])`，递归文本替换 + 作用域解析 + 缓存失效
- **`calc()` 数学求值器**：递归下降解析器，支持任意嵌套、`+ - * /`、长度单位（px / % / em / rem / vw / vh / vmin / vmax）、`min()` / `max()` / `clamp()` / `sin()` / `cos()` / `tan()` / `sqrt()` / `pow()` / `abs()` / `mod()` / `rem()` / `round()`
- **`@media` 查询**：min/max-width|height、orientation、`and` 组合，每次重排重新应用 → 动态响应式断点
- **`@keyframes`**：逐帧插值（立方贝塞尔缓动，颜色/单位/变换/阴影插值）
- **`@font-face`**：规则存储 + 匹配

### 📄 HTML5 解析器
- 忠实实现 WHATWG HTML 标准：分词器状态机 + 树构造（插入模式 + 开放元素栈 + 活动格式化元素列表）
- **Adoption Agency Algorithm**（AAA）处理错位嵌套的格式化元素
- 隐式/自动闭合规则；表格 foster-parenting
- **流式解析**：`mini_html5_feed()` 接受分块输入（边到边部分文档）

### 🧩 WebGL 桥接
- 补全 Three.js 所需的 WebGL 1.0/2.0 接口：索引绘制（`drawElements`）、完整 uniform 族、纹理参数 + mipmap
- `texImage2D` 由 `stb_image` 解码的 RGBA 缓冲区直接喂入 GL（零拷贝）
- 通过 `MiniWGL` 函数指针表解析，与桥接内部解耦

### ⚡ JavaScript 引擎（QuickJS）
- 内嵌完整 QuickJS 源码，**含基线 JIT**（`JS_JIT`）+ **SLJIT 原生代码生成**（`JS_JIT_CODEGEN`）
- DOM/BOM/WebGL polyfill 通过 `mini_js_bridge` 暴露
- ES6+ / ESM 模块支持，`import.meta.url` 基于文档 URL 解析
- 内存中 `localStorage` / `sessionStorage`

### 🔧 原生模块（Electron/Node 风格）
- 在沙箱浏览器桥接之上提供 `os` / `process` / `child_process` / `fs` / `path` / `electron` 模块
- 通过 CommonJS `require`（JS shim）+ C 端内置模块表实现
- 子进程管理：`JsChildProc` 追踪 stdout/stderr 管道、退出码、JS 回调，引用计数，非阻塞轮询

### 🔄 后台工作线程池
- 阻塞式文件/网络 I/O 在后台线程执行，永不阻塞帧循环
- 安全约定：工作线程**绝不**触碰 JSValue / JSContext / JSRuntime / GL
- `mini_worker_init(n_threads)` / `mini_worker_submit` / `mini_worker_pump(ctx)`

### 📡 HTML 事件循环
- 忠实实现 HTML Standard 事件循环模型（纯 C99，可单元测试）
- 宏任务 FIFO + 微任务队列（每个检查点排空）+ 渲染阶段（rAF + Intersection/ResizeObserver）
- MutationObserver / IntersectionObserver / ResizeObserver 注册表 + 变更检测

### 🌐 网络 & 安全
- **HTTP/1.1 客户端**：可选 HTTPS（内嵌 mbedTLS，`-DMINI_TLS`）
- **fetch 编排**：HSTS 升级 → Cookie 发送 → HTTP 缓存新鲜度服务 + 条件重验证 + 304 → Set-Cookie 捕获 → HSTS 记忆 → 缓存存储 → CORS 网关
- **Cookie 罐**（RFC6265）：domain/path/secure/SameSite/Max-Age/HttpOnly，LRU 淘汰
- **HTTP 缓存**：Cache-Control max-age / no-store / no-cache，ETag + Last-Modified 条件请求
- **安全策略**：HSTS / SOP / CORS（预检）/ CSP（`'self'` / `'none'` / `'*'` / origins）
- **WebSocket**（RFC6455）：完整帧编解码（7/16/64 位长度，客户端掩码），非阻塞 pump
- **HTTP/2**：帧编解码 + HPACK 静态表（61 条），基础奠基
- **HTTP/3**：诚实存根（QUIC 未实现，`mini_h3_available()` 返回 0，WebTransport 回退到 WebSocket）

### 🔒 加密虚拟文件系统（VFS）
- ChaCha20-Poly1305（RFC 8439）加密打包，纯 C99 实现，无 OpenSSL/mbedTLS 依赖
- 包格式：`[12 字节 nonce][N 字节密文][16 字节 Poly1305 tag]`
- 运行时内存解密（永不落盘），标签常数时间验证
- 支持 QuickJS 字节码（`QJC1` 标记 → `JS_ReadObject` 直接执行）

### 🛠️ 开发者工具
- **Chrome DevTools Protocol (CDP) 服务器**：localhost:9222，支持 `chrome://inspect` 连接 —— 实时 `console.log` 流、Elements 面板浏览自研 DOM、`Runtime.evaluate` 求值、Debugger、HeapProfiler、Page 截图、Overlay 高亮、Performance、Network、Emulation
- **引擎内 DevTools 覆盖层**：F12 切换，以 JS bundle 形式在引擎自身 QuickJS + DOM 中运行
- **运行时诊断**：每帧 rAF/layout/draw/frame 耗时、EMA FPS、QuickJS 堆、DOM 节点数、图集尺寸、绘制调用数
- **结构化日志**：六级日志 + 持久化文件 + 内存环形缓冲（1024 行）+ 实时订阅 sink
- **崩溃处理器**：跨平台（Windows SEH / POSIX sigaction + 备用栈），生成人类可读崩溃转储 + 日志刷盘

### 🌍 复杂文本排版
- **双向算法**（UAX #9）：完整实现 P2–P3、X1–X9、W1–W7、N1–N2、I1–I2、L2 重排 + L4 镜像
- **文本整形**：阿拉伯语连接（isolated/initial/medial/final 呈现形式）、天城文 pre-base matra 重排、emoji ZWJ/VS 折叠、OpenType 风格字距调整
- **音频**：最小 PCM 音频（`mini_audio_queue_pcm`）

### ⌨️ 输入系统
- W3C 事件分发：hit-test → capture → target → bubble
- `:hover` / `:active` / `:focus` 状态管理 + 交互 CSS 重新应用
- 文本选择、拖放（文件）、滚动条、鼠标手势
- Win32 IMM 输入法回调（候选窗口跟随文本插入符）

---

## 🏗️ 架构概览

```
┌─────────────────────────────────────────────────────────────┐
│                    宿主应用 (main.c)                         │
│         mini_app_create → load → run → destroy               │
├─────────────────────────────────────────────────────────────┤
│  统一 API 层 (mini_framework.h) —— 六个入口点，双模式相同      │
├──────────────┬──────────────┬──────────────┬────────────────┤
│  平台/窗口    │  渲染引擎     │  DOM + 布局   │  JS 引擎        │
│  (GLFW/GL)   │  (renderer)  │  (dom/css)   │  (QuickJS+JIT)  │
├──────────────┼──────────────┼──────────────┼────────────────┤
│  解析器       │  事件循环     │  输入系统     │  原生模块       │
│  (html5/css) │  (eventloop) │  (events)    │  (native/worker)│
├──────────────┴──────────────┴──────────────┴────────────────┤
│  网络 (net/cookies/httpcache/policy/websocket/h2/h3)          │
├──────────────────────────────────────────────────────────────┤
│  可观测性 (cdp/devtools/diag/log/crash)                        │
├──────────────────────────────────────────────────────────────┤
│  打包 (vfs: ChaCha20-Poly1305 加密, QuickJS 字节码)           │
└──────────────────────────────────────────────────────────────┘
```

**单线程引擎**：主循环运行在 QuickJS 线程上（无锁）；仅工作线程池在后台处理 I/O。

**主循环契约**：
```
diag.begin → 轮询事件 → 触发 rAF → 泵送任务 → 布局 → 渲染刷新 → diag.end → cdp.poll
```

- **空闲门控**：静态页面无输入/视口/动画需要时，跳过重排+布局+渲染+刷新+交换管线，通过 `glfwWaitEventsTimeout` 休眠（接近 0% CPU）
- **增量门控**：`doc->dirty` 门控重排，`doc->layout_dirty` 门控布局，`paint_dirty` 门控全量清除+渲染
- CDP 每次迭代轮询（廉价、非阻塞），静态页面仍可响应 `chrome://inspect`

---

## 📁 项目结构

```text
TinyFramework/
├── include/                # 公共头文件
│   ├── mini_framework.h    #   统一应用 API（六个入口点）
│   ├── mini_renderer.h     #   自研渲染引擎
│   ├── mini_dom.h          #   DOM 场景图 + 布局
│   ├── mini_css.h          #   CSS 值引擎
│   ├── mini_html5.h        #   WHATWG HTML5 解析器
│   ├── mini_webgl_ext.h    #   WebGL 1/2 桥接扩展
│   ├── mini_js_bridge.h   #   QuickJS 绑定层
│   ├── mini_native.h      #   Electron/Node 风格原生模块
│   ├── mini_worker.h       #   后台 I/O 工作线程池
│   ├── mini_eventloop.h   #   HTML Standard 事件循环
│   ├── mini_events.h       #   输入 → DOM hit-test + W3C 分发
│   ├── mini_devtools.h    #   引擎内 DevTools 覆盖层 (F12)
│   ├── mini_cdp.h          #   Chrome DevTools Protocol 服务器
│   ├── mini_cookies.h     #   RFC6265 Cookie 罐
│   ├── mini_httpcache.h   #   HTTP 缓存
│   ├── mini_policy.h       #   HSTS/SOP/CORS/CSP
│   ├── mini_websocket.h   #   RFC6455 WebSocket
│   ├── mini_h2.h          #   HTTP/2 帧编解码
│   ├── mini_h3.h          #   HTTP/3 诚实存根
│   ├── mini_vfs.h          #   加密虚拟文件系统
│   ├── mini_crash.h       #   跨平台崩溃处理器
│   ├── mini_log.h          #   结构化环形日志
│   ├── mini_diag.h         #   运行时诊断/分析器
│   ├── mini_audio.h       #   PCM 音频
│   ├── mini_bidi.h        #   Unicode 双向算法
│   ├── mini_shaping.h     #   复杂文本整形
│   └── ...
├── src/                    # 实现源码
│   ├── main.c              #   入口点 + MiniApp 结构 + 主循环
│   ├── mini_renderer.c     #   渲染管线实现
│   ├── mini_dom.c          #   DOM + 布局实现
│   ├── mini_css.c          #   CSS 引擎实现
│   ├── mini_html5.c        #   HTML5 解析器实现
│   └── ...                 #   其余模块实现
├── libs/                   # 内嵌第三方依赖
│   ├── quickjs/            #   QuickJS（含基线 JIT）
│   ├── mbedtls/            #   MbedTLS（HTTPS/TLS）
│   ├── stb/                #   stb_image / stb_truetype
│   └── sljit/              #   SLJIT（QuickJS 原生代码生成）
├── templates/
│   └── vscode-desktop-app/ #  VS Code 桌面应用开发模板
├── tests/                  # 测试套件（HTML 页面 + JS 脚本）
├── assets/                 # 应用图标等资源
├── font_extract/           # 内嵌 CJK 字体
├── CMakeLists.txt          # CMake 构建
├── build.py                # 跨平台构建 + 加密打包脚本
├── build.ps1               # Windows (MSYS2/MinGW) 构建脚本
└── .gitignore
```

---

## 🚀 快速开始

### 环境要求

- **C 编译器**：GCC / Clang / MSVC（支持 C99）
- **CMake** ≥ 3.16（使用 CMake 构建时）
- **GLFW 3.4**（窗口 + GL 上下文）
- **Python 3**（使用 `build.py` 打包时，需 `cryptography` 包）
- **可选**：MSYS2/MinGW-w64（Windows `build.ps1`）、UPX（体积压缩）

### 方式一：CMake 构建（推荐）

```bash
# 配置（默认开启 CDP、诊断、JIT、TLS、GUI 子系统）
cmake -B build -S . \
  -DGLFW_ROOT=/path/to/glfw-3.4 \
  -DENABLE_CDP=ON \
  -DENABLE_DIAG=ON \
  -DMINI_JIT=ON \
  -DMINI_JIT_CODEGEN=ON \
  -DMINI_TLS=ON

# 编译
cmake --build build --config Release

# 运行自测
cmake --build build --target check
```

### 方式二：Python 构建脚本（含加密打包）

```bash
# 开发构建（仅编译）
python build.py --in src/ --out build/ --no-compile

# 发布打包（编译 JS 为 QuickJS 字节码 + ChaCha20 加密）
python build.py \
  --in src/ \
  --out build/ \
  --bytecode \
  --key <32字节十六进制密钥> \
  --jit --jit-codegen \
  --upx

# 查看 build.py 全部选项
python build.py --help
```

### 方式三：Windows PowerShell 构建（MSYS2/MinGW）

```powershell
# 需先安装 MSYS2 + mingw-w64 工具链
.\build.ps1
# 产出: build\tiny_app.exe + build\tiny_app.debug
```

### 运行应用

```bash
# 加载 HTML 入口
./build/tiny_app.exe src/index.html

# 启用 CDP 调试（默认端口 9222）
TINY_CDP_PORT=9222 ./build/tiny_app.exe src/index.html
# 然后打开 chrome://inspect 连接

# 自定义窗口尺寸
TINY_WIDTH=1920 TINY_HEIGHT=1080 ./build/tiny_app.exe src/index.html

# 自动化运行（指定帧数后截图退出）
TINY_FRAMES=300 ./build/tiny_app.exe src/index.html
# 截图保存至 build/shot.png
```

### 入口解析顺序

`main()` 按以下顺序查找入口文件：

1. `argv[1]`（命令行参数）
2. `app.config.json` 中的 `entry` 字段
3. `app.pak`（加密 VFS 包，使用默认密钥在内存中解密执行）
4. `src/index.html`
5. `index.html`
6. `test_suite.js`

---

## 🧪 测试

```bash
# CMake 自测
cmake --build build --target check

# 单独运行
./build/vfs_selftest    # 加密 VFS 自测
./build/cdp_selftest    # CDP 服务器自测
```

测试套件（`tests/` 目录）：
- **HTML 测试页**：`test_1_scrollbar.html`（滚动条）、`test_2_placeholder.html`（占位符）、`test_3_threejs_webgl.html`（Three.js WebGL）、`test_4_layout.html`（布局）、`test_5_text.html`（文本）
- **JS 测试脚本**：`test_async.js`、`test_native_api.js`、`test_phase2.js`、`test_phase3.js`、`test_phase4.js`
- **原生模块测试**：`native_mod.js`、`native_mod2.js`

---

## ⚙️ 构建选项

| CMake 选项 | 默认 | 说明 |
|-----------|------|------|
| `ENABLE_CDP` | `ON` | 内嵌 Chrome DevTools Protocol 服务器 |
| `ENABLE_DIAG` | `ON` | 内嵌运行时诊断/分析器 |
| `TINY_NATIVE` | `OFF` | 构建为原生 WebView 模式（OFF = 自研引擎） |
| `MINI_JIT` | `ON` | 从源码构建 QuickJS 并启用基线 JIT |
| `MINI_JIT_CODEGEN` | `ON` | 启用 SLJIT 原生代码生成（隐含 `MINI_JIT`） |
| `MINI_TLS` | `ON` | 通过内嵌 mbedTLS 启用 HTTPS |
| `GUI_APP` | `ON` | 构建为 Windows GUI 子系统（`-mwindows`） |
| `GLFW_ROOT` | `D:/glfw-3.4.bin.WIN64` | 预编译 GLFW SDK 路径 |

---

## 📦 应用模板

项目提供 VS Code 桌面应用开发模板（`templates/vscode-desktop-app/`）：

```bash
cd templates/vscode-desktop-app
npm install
npm run dev       # 开发模式运行（CDP 调试 :9222）
npm run package   # 发布打包（QuickJS 字节码 + ChaCha20 加密）
```

模板特性：~3MB 发布包、~0.15s 冷启动、10~20MB 内存；完整 `app.config.json` 配置；`document.title` 动态窗口标题。详见 [模板文档](templates/vscode-desktop-app/README.md)。

---

## 🔐 安全打包流程

```text
  JS 源码 (.js/.mjs)
       │
       ▼  build.py --bytecode
  QuickJS 字节码 (QJC1 标记)
       │
       ▼  build.py --key <32字节>
  ChaCha20-Poly1305 加密包 (nonce|ct|tag)
       │
       ▼  运行时 mini_vfs_decrypt
  内存中解密（永不落盘）→ 直接执行
```

- 源码编译为 QuickJS 字节码，再经 RFC 8439 ChaCha20-Poly1305 加密
- 运行时在 RAM 中解密执行，明文在使用后清零，杜绝源码泄露

---

## 🌍 国际化文本

- **双向排版**：完整实现 Unicode UAX #9 双向算法（阿拉伯语、希伯来语等 RTL 文本）
- **文本整形**：阿拉伯语连接形态、天城文集群组合、emoji ZWJ 序列
- **CJK 字体**：内嵌「爱点风雅黑长体」TrueType 字体，多字体回退链

---

## 平台支持

| 平台 | 状态 | 说明 |
|------|------|------|
| Windows | ✅ 主力平台 | Win32 IMM 输入法、SEH 崩溃处理、GUI 子系统 |
| Linux | ✅ 可构建 | POSIX 信号崩溃处理、备用栈 |
| macOS | ✅ 可构建 | 同 Linux |

---

## 🔗 内嵌依赖

| 库 | 用途 | 许可 |
|----|------|------|
| [QuickJS](https://bellard.org/quickjs/) | JavaScript 引擎（含 JIT） | MIT |
| [MbedTLS](https://github.com/Mbed-TLS/mbedtls) | TLS / HTTPS | Apache-2.0 |
| [stb](https://github.com/nothings/stb) | 图像解码 / TrueType 字形 | Public Domain |
| [SLJIT](https://github.com/zherczeg/sljit) | QuickJS 原生代码生成 | BSD-2-Clause |
| [GLFW](https://www.glfw.org/) | 窗口 + 输入 + GL 上下文 | Zlib |

---

## 📜 开源许可

本项目采用 MIT 许可证。

内嵌第三方库保留各自原始许可（见 `libs/` 各子目录）。

---

## 🤝 贡献

欢迎提交 Issue 和 Pull Request。提交代码前请确保：

1. 新增功能附带测试（自测宏或 `tests/` 测试页）
2. 遵循现有 C99 代码风格（`_GNU_SOURCE`、UTF-8 编码、`mini_` 前缀命名）
3. 保持单线程引擎架构约定（工作线程不触碰 JS/GL）
