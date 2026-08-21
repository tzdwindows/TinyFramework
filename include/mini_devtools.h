/*
 * mini_devtools.h — in-engine "Chrome-like" DevTools overlay.
 *
 * The panel itself is a JS bundle (devtools.js) that runs inside the engine's
 * own QuickJS + DOM: it builds a position:fixed high-z subtree under <body>
 * (so it paints on top and is hit first), reads the live DOM via the standard
 * DOM API, polls console logs via __mini_log_poll, and evals input via the
 * page's global scope. F12 toggles it. This keeps the heavy UI in JS (where
 * the engine is strongest) and the C side to a one-line install + hotkey.
 */
#ifndef MINI_DEVTOOLS_H
#define MINI_DEVTOOLS_H

struct MiniBridge;

/* Eval the DevTools bundle into the bridge's JS context (idempotent). Call
 * once at startup. Returns 0 on success. */
int mini_devtools_install(struct MiniBridge *b);

/* Toggle the DevTools panel (calls window.__miniDT.toggle() in JS). */
int mini_devtools_toggle(struct MiniBridge *b);

#endif /* MINI_DEVTOOLS_H */
