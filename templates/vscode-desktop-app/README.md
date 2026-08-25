# TinyFramework VS Code Desktop App Template

这是一个基于 **TinyFramework 自研极简超高性能浏览器内核** 的 VS Code 桌面应用程序开发模板。

---

## ✨ 核心特性

1. **极小体积 & 秒开体验**：
   - 完整发布包仅 ~3MB，不依赖庞大臃肿的 Electron / Chromium / Node.js 运行时；
   - 冷启动仅需 ~0.15 秒，内存占用仅 10~20MB。
2. **现代 Web 技术栈**：
   - 使用原生 HTML5 + CSS + JavaScript (ES6+ / ESM) 开发；
   - 支持 Canvas 2D、WebGL 3D (Three.js) 以及标准 Web Audio API。
3. **安全加密打包**：
   - 导出时支持将 JavaScript 源码编译为 QuickJS 原生字节码并采用 RFC 8439 (ChaCha20-Poly1305) 进行高强度加密；
   - 运行时内存解密执行，杜绝源码泄露。
4. **灵活自定义**：
   - 支持在 `app.config.json` 中配置应用名称、默认窗口宽高、窗口图标 (`icon.png` / `app.ico`)；
   - 支持在 JavaScript 中通过 `document.title = "..."` 动态修改窗口标题。

---

## 🚀 快速上手与开发

### 1. 开发运行（Dev 模式）
在 VS Code 中：
* 按 **F5** 或 **Ctrl + Shift + B** 选择 `Dev: Run Desktop App`；
* 或在终端运行：
  ```bash
  npm run dev
  ```
  即可启动实时开发窗口，支持 Chrome DevTools 协议（`:9222`）断点调试。

### 2. 导出与加密打包（Release 模式）
在 VS Code 中：
* 运行 Task `Build: Package & Encrypt App`；
* 或在终端运行：
  ```bash
  npm run package
  ```
  打包脚本将自动：
  1. 生成独立可执行文件（如 `dist/MyDesktopApp.exe`）；
  2. 将 JS 逻辑进行 ChaCha20 加密生成 `dist/app.pak`；
  3. 打包自定义图标与样式；
  4. 生成完整的 `dist/` 独立绿色发布包。

---

## 📁 目录结构

```text
├── .vscode/
│   ├── launch.json         # Chrome DevTools 调试配置
│   ├── tasks.json          # VS Code 运行与打包任务
│   └── settings.json       # 编辑器推荐配置
├── src/
│   ├── index.html          # 主窗口 UI 结构
│   ├── app.js              # 核心业务逻辑 / 事件 / 音频 / Canvas
│   └── styles.css          # 应用界面样式
├── assets/
│   └── icon.png            # 应用程序图标
├── scripts/
│   ├── dev.py              # 开发启动脚本
│   └── package.py          # 导出与源码加密工具
├── app.config.json         # 桌面应用全局配置
├── package.json            # npm 脚本声明
└── README.md               # 模板使用文档
```
