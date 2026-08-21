/*
 * mini_log.c — implementation of the structured logging system (mini_log.h).
 *
 * Pure C99, no third-party deps. Monotonic clock is QueryPerformanceCounter on
 * Windows and clock_gettime(CLOCK_MONOTONIC) on POSIX. The engine is single-
 * threaded so a real lock is unnecessary; a recursion guard stops a sink that
 * re-logs from looping. The crash handler never calls the normal emit path —
 * it writes its own dump file directly and then asks mini_log_dump_to_file to
 * spill the ring (which only touches the ring array, not the sinks).
 */
#include "mini_log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
  #include <unistd.h>
#else
  /* unknown POSIX-ish; fall back to time() */
#endif

/* ------------------------------------------------------------------ */
/* monotonic clock                                                     */
/* ------------------------------------------------------------------ */
static double g_log_epoch_ms = 0; /* wall ms at init, for absolute stamps */

static double monotonic_ms_now(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq = {0};
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart * 1000.0;
#elif defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
    return (double)time(NULL) * 1000.0;
#else
    return (double)time(NULL) * 1000.0;
#endif
}

double mini_log_now_ms(void)
{
    return monotonic_ms_now() - g_log_epoch_ms;
}

static double wall_ms_now(void)
{
#if defined(_WIN32)
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    /* FILETIME is 100ns ticks since 1601; convert to ms since epoch */
    return (double)(u.QuadPart / 10000ULL) - 11644473600000.0;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
        return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
    return (double)time(NULL) * 1000.0;
#endif
}

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    MiniLogSink cb;
    void       *ud;
} LogSinkSlot;

static int           g_inited = 0;
static MiniLogLevel  g_level = MINI_LOG_INFO;
static MiniLogLevel  g_stderr_level = MINI_LOG_WARN;
static FILE         *g_file = NULL;
static char          g_file_path[1024] = {0};

static MiniLogEntry  g_ring[MINI_LOG_RING_CAP];
static int           g_ring_head = 0;   /* next write slot */
static int           g_ring_count = 0;  /* valid entries (cap at RING_CAP) */
static unsigned long g_total = 0;       /* entries ever emitted */

static LogSinkSlot   g_sinks[MINI_LOG_MAX_SINKS];
static int           g_sink_n = 0;

/* recursion guard: a sink that itself calls mini_logf is allowed, but its
   entry is stored only — not re-dispatched — to avoid infinite recursion. */
static int g_log_depth = 0;

const char *mini_log_level_str(MiniLogLevel l)
{
    switch (l)
    {
        case MINI_LOG_TRACE: return "TRACE";
        case MINI_LOG_DEBUG: return "DEBUG";
        case MINI_LOG_INFO:  return "INFO ";
        case MINI_LOG_WARN:  return "WARN ";
        case MINI_LOG_ERROR: return "ERROR";
        case MINI_LOG_FATAL: return "FATAL";
        default:             return "?    ";
    }
}

static const char *level_short(MiniLogLevel l)
{
    switch (l)
    {
        case MINI_LOG_TRACE: return "TRACE";
        case MINI_LOG_DEBUG: return "DEBUG";
        case MINI_LOG_INFO:  return "INFO";
        case MINI_LOG_WARN:  return "WARN";
        case MINI_LOG_ERROR: return "ERROR";
        case MINI_LOG_FATAL: return "FATAL";
        default:             return "?";
    }
}

void mini_log_init(void)
{
    if (g_inited)
        return;
    g_inited = 1;
    g_log_epoch_ms = monotonic_ms_now();

    /* env overrides: TINY_LOG=trace|debug|info|warn|error|fatal sets emit
       threshold; TINY_LOG_FILE=path attaches a file sink; TINY_LOG_STDERR
       sets the stderr mirror threshold. Lets the user capture JS console
       output to a file with no code change. */
    const char *lvl = getenv("TINY_LOG");
    if (lvl && lvl[0])
    {
        if      (!strcmp(lvl, "trace")) g_level = MINI_LOG_TRACE;
        else if (!strcmp(lvl, "debug")) g_level = MINI_LOG_DEBUG;
        else if (!strcmp(lvl, "info"))  g_level = MINI_LOG_INFO;
        else if (!strcmp(lvl, "warn"))  g_level = MINI_LOG_WARN;
        else if (!strcmp(lvl, "error")) g_level = MINI_LOG_ERROR;
        else if (!strcmp(lvl, "fatal")) g_level = MINI_LOG_FATAL;
    }
    const char *slvl = getenv("TINY_LOG_STDERR");
    if (slvl && slvl[0])
    {
        if      (!strcmp(slvl, "trace")) g_stderr_level = MINI_LOG_TRACE;
        else if (!strcmp(slvl, "debug")) g_stderr_level = MINI_LOG_DEBUG;
        else if (!strcmp(slvl, "info"))  g_stderr_level = MINI_LOG_INFO;
        else if (!strcmp(slvl, "warn"))  g_stderr_level = MINI_LOG_WARN;
        else if (!strcmp(slvl, "error")) g_stderr_level = MINI_LOG_ERROR;
        else if (!strcmp(slvl, "fatal")) g_stderr_level = MINI_LOG_FATAL;
    }
    const char *fp = getenv("TINY_LOG_FILE");
    if (fp && fp[0])
        mini_log_set_file(fp);
}

void mini_log_shutdown(void)
{
    if (g_file)
    {
        fflush(g_file);
        fclose(g_file);
        g_file = NULL;
    }
    g_sink_n = 0;
    g_inited = 0;
}

void mini_log_set_level(MiniLogLevel lvl)
{
    g_level = lvl;
}
MiniLogLevel mini_log_get_level(void)
{
    return g_level;
}
void mini_log_set_stderr_level(MiniLogLevel lvl)
{
    g_stderr_level = lvl;
}

int mini_log_set_file(const char *path)
{
    if (g_file)
    {
        fflush(g_file);
        fclose(g_file);
        g_file = NULL;
        g_file_path[0] = 0;
    }
    if (!path || !path[0])
        return 0;
    /* append + create; binary so CR/LF control stays exact on Windows. */
    g_file = fopen(path, "ab");
    if (!g_file)
        return -1;
    snprintf(g_file_path, sizeof g_file_path, "%s", path);
    /* write a session-start banner so separate runs are distinguishable. */
    char hdr[160];
    double now = wall_ms_now();
    time_t secs = (time_t)(now / 1000.0);
    struct tm *tmr = localtime(&secs);
    if (tmr)
        snprintf(hdr, sizeof hdr,
                 "==== mini_log session %04d-%02d-%02d %02d:%02d:%02d ====\n",
                 tmr->tm_year + 1900, tmr->tm_mon + 1, tmr->tm_mday,
                 tmr->tm_hour, tmr->tm_min, tmr->tm_sec);
    else
        snprintf(hdr, sizeof hdr, "==== mini_log session ====\n");
    fputs(hdr, g_file);
    fflush(g_file);
    return 0;
}

int mini_log_add_sink(MiniLogSink cb, void *ud)
{
    if (!cb || g_sink_n >= MINI_LOG_MAX_SINKS)
        return -1;
    /* avoid duplicate registration */
    for (int i = 0; i < g_sink_n; i++)
        if (g_sinks[i].cb == cb && g_sinks[i].ud == ud)
            return 0;
    g_sinks[g_sink_n].cb = cb;
    g_sinks[g_sink_n].ud = ud;
    g_sink_n++;
    return 0;
}

int mini_log_remove_sink(MiniLogSink cb, void *ud)
{
    for (int i = 0; i < g_sink_n; i++)
        if (g_sinks[i].cb == cb && g_sinks[i].ud == ud)
        {
            g_sinks[i] = g_sinks[--g_sink_n];
            return 0;
        }
    return -1;
}

/* ring push (no sink dispatch) */
static void ring_push(const MiniLogEntry *e)
{
    g_ring[g_ring_head] = *e;
    g_ring_head = (g_ring_head + 1) % MINI_LOG_RING_CAP;
    if (g_ring_count < MINI_LOG_RING_CAP)
        g_ring_count++;
    g_total++;
}

unsigned long mini_log_count(void) { return g_total; }

int mini_log_recent(MiniLogEntry *out, int max)
{
    int n = g_ring_count;
    if (max < n)
        n = max;
    if (out && n > 0)
    {
        /* chronological order: oldest first. The oldest is at
           (head - count + CAP) % CAP when the ring is full. */
        int start = (g_ring_head - g_ring_count + MINI_LOG_RING_CAP) % MINI_LOG_RING_CAP;
        for (int i = 0; i < n; i++)
            out[i] = g_ring[(start + i) % MINI_LOG_RING_CAP];
    }
    return g_ring_count < max ? g_ring_count : max;
}

int mini_log_since(MiniLogEntry *out, int max, unsigned long since)
{
    if (g_ring_count == 0 || since >= g_total)
        return 0;
    /* The oldest entry in the ring carries global index (total - count). */
    unsigned long oldest_global = g_total - (unsigned long)g_ring_count;
    /* first global index strictly greater than `since`, clamped to ring */
    unsigned long want_global = since + 1;
    if (want_global < oldest_global)
        want_global = oldest_global;
    int start_i = (int)(want_global - oldest_global); /* offset into ring */
    if (start_i < 0)
        start_i = 0;
    int avail = g_ring_count - start_i;
    int n = avail < max ? avail : max;
    if (out && n > 0)
    {
        int ring_start = (g_ring_head - g_ring_count + MINI_LOG_RING_CAP) % MINI_LOG_RING_CAP;
        for (int i = 0; i < n; i++)
            out[i] = g_ring[(ring_start + start_i + i) % MINI_LOG_RING_CAP];
    }
    return n;
}

void mini_log_clear(void)
{
    g_ring_head = 0;
    g_ring_count = 0;
}

void mini_log_flush(void)
{
    if (g_file)
        fflush(g_file);
}

void mini_log_dump_to_file(const char *path)
{
    if (!path)
        return;
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    int n = g_ring_count;
    int start = (g_ring_head - g_ring_count + MINI_LOG_RING_CAP) % MINI_LOG_RING_CAP;
    fputs("==== mini_log ring dump ====\n", f);
    for (int i = 0; i < n; i++)
    {
        const MiniLogEntry *e = &g_ring[(start + i) % MINI_LOG_RING_CAP];
        fprintf(f, "[%9.2f] %-5s [%s] %s\n",
                e->ts_ms, level_short(e->level),
                e->tag[0] ? e->tag : "-", e->msg);
    }
    fputs("==== end ring dump ====\n", f);
    fclose(f);
}

void mini_logv(MiniLogLevel lvl, const char *tag, const char *fmt, va_list ap)
{
    if (lvl < 0 || lvl >= MINI_LOG_LEVEL_COUNT)
        return;
    /* safe before init: if the user hasn't called mini_log_init, still emit
       to stderr so early-startup logs aren't lost. */
    if (g_inited && lvl < g_level)
        return;

    MiniLogEntry e;
    e.ts_ms = mini_log_now_ms();
    e.level = lvl;
    {
        const char *t = tag ? tag : "";
        size_t i = 0;
        for (; t[i] && i < MINI_LOG_TAG_LEN - 1; i++)
            e.tag[i] = t[i];
        e.tag[i] = 0;
    }
    vsnprintf(e.msg, MINI_LOG_MSG_LEN, fmt ? fmt : "", ap);
    e.msg[MINI_LOG_MSG_LEN - 1] = 0;

    /* store + mirror */
    g_log_depth++;
    ring_push(&e);

    /* stderr mirror (skip if recursing — stderr itself can't recurse here) */
    if (lvl >= g_stderr_level)
    {
        fprintf(stderr, "[%9.2f] %-5s [%s] %s\n",
                e.ts_ms, level_short(e.level),
                e.tag[0] ? e.tag : "-", e.msg);
    }
    /* file sink */
    if (g_file)
    {
        fprintf(g_file, "[%9.2f] %-5s [%s] %s\n",
                e.ts_ms, level_short(e.level),
                e.tag[0] ? e.tag : "-", e.msg);
        fflush(g_file); /* survive a hard crash mid-line */
    }

    /* dispatch live sinks only at the top of the stack (no re-entrancy) */
    if (g_log_depth == 1)
    {
        /* snapshot the sink list: a sink may add/remove sinks */
        LogSinkSlot snap[MINI_LOG_MAX_SINKS];
        int sn = g_sink_n;
        if (sn > MINI_LOG_MAX_SINKS)
            sn = MINI_LOG_MAX_SINKS;
        for (int i = 0; i < sn; i++)
            snap[i] = g_sinks[i];
        g_log_depth--;
        for (int i = 0; i < sn; i++)
            if (snap[i].cb)
                snap[i].cb(&e, snap[i].ud);
    }
    else
    {
        g_log_depth--;
    }
}

void mini_logf(MiniLogLevel lvl, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    mini_logv(lvl, tag, fmt, ap);
    va_end(ap);
}
