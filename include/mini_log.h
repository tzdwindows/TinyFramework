/*
 * mini_log.h — cross-platform structured logging system.
 *
 * A single, dependency-free logger that every subsystem (app, JS bridge,
 * net, cdp, diag, crash) routes through. It provides:
 *
 *   • six levels (TRACE..FATAL) with a configurable minimum-emit threshold;
 *   • a persistent file sink (append) so logs survive a crash/exit;
 *   • an in-memory ring buffer (the "background" store) that the host, CDP,
 *     or a JS listener can poll at any time — capturing console.* output the
 *     engine produced even when nothing was watching live;
 *   • a subscriber list (live sinks) — the CDP Runtime.consoleAPICalled
 *     relay and a JS `onlog` hook plug in here;
 *   • crash-safe: the crash handler (mini_crash.c) can dump the whole ring
 *     to disk without re-entering the logger.
 *
 * The engine is single-threaded, so no real lock is required; a recursion
 * guard prevents a sink that re-logs from recursing forever. Pure C99.
 */
#ifndef MINI_LOG_H
#define MINI_LOG_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MINI_LOG_TRACE = 0,
    MINI_LOG_DEBUG,
    MINI_LOG_INFO,
    MINI_LOG_WARN,
    MINI_LOG_ERROR,
    MINI_LOG_FATAL,
    MINI_LOG_LEVEL_COUNT
} MiniLogLevel;

#define MINI_LOG_RING_CAP 1024  /* entries kept in the in-memory ring buffer */
#define MINI_LOG_MSG_LEN  1024  /* max message length (truncated, not split)  */
#define MINI_LOG_TAG_LEN  24
#define MINI_LOG_MAX_SINKS 8

typedef struct MiniLogEntry {
    double       ts_ms;   /* monotonic ms since mini_log_init */
    MiniLogLevel level;
    char         tag[MINI_LOG_TAG_LEN];  /* subsystem: "app","js","cdp","net",... */
    char         msg[MINI_LOG_MSG_LEN];
} MiniLogEntry;

/* A sink is called for every EMITTED entry (after the ring + file write).
 * It runs outside the logger's recursion guard, so it may call mini_logf
 * again (such re-entrant entries are stored but not re-dispatched). */
typedef void (*MiniLogSink)(const MiniLogEntry *e, void *ud);

/* Lifecycle. Idempotent. mini_logf is safe to call before init (degrades to
 * a stderr write) so order-of-init never causes lost logs at startup. */
void mini_log_init(void);
void mini_log_shutdown(void);

/* Minimum level to EMIT at all (default INFO). */
void          mini_log_set_level(MiniLogLevel lvl);
MiniLogLevel  mini_log_get_level(void);

/* Minimum level to also mirror to stderr (default WARN, so the terminal is
 * not flooded with console.log while still surfacing warnings/errors). */
void          mini_log_set_stderr_level(MiniLogLevel lvl);

/* Attach a persistent file sink (opened in append mode). NULL detaches and
 * closes the current file. Returns 0 on success. */
int   mini_log_set_file(const char *path);

/* Live subscriber sinks (the CDP console relay / a JS hook register here). */
int   mini_log_add_sink(MiniLogSink cb, void *ud);
int   mini_log_remove_sink(MiniLogSink cb, void *ud);

/* Core emit. `tag` may be NULL (becomes ""). Over-long messages truncate. */
void  mini_logf(MiniLogLevel lvl, const char *tag, const char *fmt, ...);
void  mini_logv(MiniLogLevel lvl, const char *tag, const char *fmt, va_list ap);

/* Convenience macros (variadic; needs >= C99). */
#define MINI_LOGT(tag, ...) mini_logf(MINI_LOG_TRACE, tag, __VA_ARGS__)
#define MINI_LOGD(tag, ...) mini_logf(MINI_LOG_DEBUG, tag, __VA_ARGS__)
#define MINI_LOGI(tag, ...) mini_logf(MINI_LOG_INFO,  tag, __VA_ARGS__)
#define MINI_LOGW(tag, ...) mini_logf(MINI_LOG_WARN,  tag, __VA_ARGS__)
#define MINI_LOGE(tag, ...) mini_logf(MINI_LOG_ERROR, tag, __VA_ARGS__)
#define MINI_LOGF(tag, ...) mini_logf(MINI_LOG_FATAL, tag, __VA_ARGS__)

/* ---- background ring buffer access (for polling JS/console output) ---- */
/* Copy up to `max` most-recent entries into `out` in chronological order.
 * Returns the number copied. `out`/`max` may be 0 to just query the count. */
int           mini_log_recent(MiniLogEntry *out, int max);
/* Copy entries emitted AFTER the `since` global counter (see mini_log_count)
 * into `out`, oldest-first, capped at `max`. Lets a poller fetch only what is
 * new since its last call. Returns the number copied. */
int           mini_log_since(MiniLogEntry *out, int max, unsigned long since);
/* Monotonic counter of every entry ever emitted (for change detection). */
unsigned long mini_log_count(void);
void          mini_log_clear(void);

/* Flush the file sink (best-effort fsync). */
void          mini_log_flush(void);

/* Dump the whole ring buffer to a file (used by the crash handler). */
void          mini_log_dump_to_file(const char *path);

const char   *mini_log_level_str(MiniLogLevel l);
double        mini_log_now_ms(void); /* monotonic ms since init */

#ifdef __cplusplus
}
#endif
#endif /* MINI_LOG_H */
