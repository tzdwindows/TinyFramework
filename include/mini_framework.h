/*
 * mini_framework.h — TinyFramework unified application API
 *
 * A single header surface that is byte-for-byte identical in both build modes:
 *   1. NATIVE          : delegate to the OS WebView (binary < 2 MB)
 *   2. CUSTOM_MINI     : fully self-written renderer + JS engine (binary < 10 MB)
 *
 * Mode is selected at compile time via -DENABLE_CUSTOM_MINI_ENGINE.
 * The host application never sees the difference: same MiniApp* handle,
 * same four entry functions.
 */
#ifndef MINI_FRAMEWORK_H
#define MINI_FRAMEWORK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Public export control                                              */
/* ------------------------------------------------------------------ */
#if defined(_WIN32) && defined(MINI_SHARED)
  #ifdef MINI_BUILD
    #define MINI_API __declspec(dllexport)
  #else
    #define MINI_API __declspec(dllimport)
  #endif
#else
  #define MINI_API
#endif

/* ------------------------------------------------------------------ */
/* Build-time mode flags                                             */
/* ------------------------------------------------------------------ */
#if defined(ENABLE_CUSTOM_MINI_ENGINE)
  #define MINI_MODE_CUSTOM 1
  #define MINI_MODE_NATIVE 0
#else
  #define MINI_MODE_CUSTOM 0
  #define MINI_MODE_NATIVE 1
#endif

/* ------------------------------------------------------------------ */
/* Result codes                                                      */
/* ------------------------------------------------------------------ */
typedef enum {
    MINI_OK          =  0,
    MINI_ERR_INIT    = -1,   /* window/OS surface init failed  */
    MINI_ERR_GPU     = -2,   /* no usable GL/Vulkan context     */
    MINI_ERR_JS      = -3,   /* JS engine init / eval failed   */
    MINI_ERR_ASSET   = -4   /* entry bundle / asset load fail */
} MiniResult;

/* ------------------------------------------------------------------ */
/* Opaque handles                                                    */
/* ------------------------------------------------------------------ */
typedef struct MiniApp    MiniApp;     /* application + window + engine bundle */
typedef struct MiniWindow MiniWindow;  /* platform window abstraction          */

/* ------------------------------------------------------------------ */
/* Window configuration (identical across modes)                     */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *title;
    int   width;
    int   height;
    int   fullscreen;   /* 0 = windowed, 1 = fullscreen            */
    int   resizable;    /* 0 = fixed,    1 = user-resizable       */
    int   vsync;       /* 0 = disabled, 1 = enabled (default)     */
    int   sample_count;/* MSAA samples (0/1/2/4/8)                 */
} MiniWindowConfig;

static inline void MiniWindowConfig_Init(MiniWindowConfig *c) {
    c->title = "TinyFramework";
    c->width = 1280; c->height = 720;
    c->fullscreen = 0; c->resizable = 1;
    c->vsync = 1; c->sample_count = 0;
}

/* ------------------------------------------------------------------ */
/* Unified entry points (identical in NATIVE and CUSTOM_MINI)         */
/* ------------------------------------------------------------------ */

/* Create the window + (WebView OR custom renderer+JS engine). */
MINI_API MiniResult mini_app_create(const MiniWindowConfig *cfg, MiniApp **out);

/* Load the JS entry (URL for NATIVE, file path for CUSTOM_MINI). */
MINI_API MiniResult mini_app_load(MiniApp *app, const char *entry);

/* Load an encrypted in-memory VFS bundle (CUSTOM_MINI). `bundle` is the
   [nonce(12)|ciphertext|tag(16)] produced by the packer; decrypted in RAM
   (never to disk). If the bundle is QuickJS bytecode it is executed as such. */
MINI_API MiniResult mini_app_load_encrypted(MiniApp *app,
                                            const uint8_t *bundle, size_t size,
                                            const uint8_t key[32]);

/* Start the Chrome DevTools Protocol server on `port` (9222 default).
   Open chrome://inspect or connect ws://localhost:<port>. Returns 0 on ok. */
MINI_API MiniResult mini_app_enable_cdp(MiniApp *app, uint16_t port);

/* Blocking run loop. Returns when the window is closed or JS exits. */
MINI_API MiniResult mini_app_run(MiniApp *app);

/* Tear everything down. Safe to call even on partial init. */
MINI_API void      mini_app_destroy(MiniApp *app);

/* Mode introspection (handy for diagnostics). */
MINI_API int       mini_app_mode(void);   /* returns MINI_MODE_* */

#ifdef __cplusplus
}
#endif
#endif /* MINI_FRAMEWORK_H */
