/*
 * mini_crash.c — implementation of system-level crash interception.
 *
 * Windows path: SetUnhandledExceptionFilter (catches SEH: access violation,
 *   stack overflow, illegal, divide-by-zero, ...) + signal() for the C-level
 *   signals (SIGABRT/SIGFPE/SIGILL/SIGSEGV/SIGINT/SIGTERM). Backtrace via
 *   RtlCaptureStackBackTrace + per-frame module name + offset (kernel32 only,
 *   no dbghelp dependency).
 *
 * POSIX path: sigaction() with SA_SIGINFO on the fatal set + an alternate
 *   stack (so a stack-overflow SIGSEGV can still be reported). Backtrace via
 *   backtrace()/backtrace_symbols() (<execinfo.h> on Linux/macOS).
 *
 * Both write a crash report + spill the mini_log ring to disk, then _exit().
 * Handlers are best-effort (they use fprintf which is not strictly async-
 * signal-safe) — the priority is "leave a useful trail", which is the whole
 * point of a crash interceptor.
 */
#include "mini_crash.h"
#include "mini_log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <dbghelp.h>
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
  #include <signal.h>
  #include <unistd.h>
  /* backtrace() lives in <execinfo.h> on glibc + Apple libc. */
  #if defined(__APPLE__) || defined(__linux__)
  #define MINI_HAVE_BACKTRACE 1
  #include <execinfo.h>
  #endif
#else
  #include <signal.h>
#endif

/* ------------------------------------------------------------------ */
/* immutable config (set once at init, read in a handler — safe)       */
/* ------------------------------------------------------------------ */
static char g_app[64]   = "tiny_app";
static char g_dir[512]  = ".";
static char g_ver[64]   = "unknown";
static int  g_installed = 0;

void mini_crash_set_version(const char *v)
{
    if (!v) return;
    snprintf(g_ver, sizeof g_ver, "%s", v);
}

static const char *platform_str(void)
{
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#elif defined(__unix__)
    return "POSIX";
#else
    return "Unknown";
#endif
}

/* a best-effort wall-clock stamp (signal handlers tolerate it). */
static void stamp_now(char *out, size_t cap)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (tm)
        snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec);
    else
        snprintf(out, cap, "?");
}

static void stamp_fname(char *out, size_t cap)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (tm)
        snprintf(out, cap, "%04d%02d%02d-%02d%02d%02d",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec);
    else
        snprintf(out, cap, "crash");
}

/* ------------------------------------------------------------------ */
/* the dump writer — shared by the SEH, signal, and manual paths       */
/* ------------------------------------------------------------------ */
/* `kind`  is a short string ("SEH"/"signal"/"manual").
 * `name`  is the human signal/exception name (e.g. "SIGSEGV","ACCESS_VIOLATION").
 * `code`  is the numeric code (signal # or exception code).
 * `addr`  is the faulting address (0 if unknown). */
#if defined(_WIN32)
static struct _EXCEPTION_POINTERS *g_cur_ep = NULL;
#endif

static void write_dump(const char *kind, const char *name, long code,
                       void *addr, const char *reason)
{
    char ts[64], fname[32];
    stamp_now(ts, sizeof ts);
    stamp_fname(fname, sizeof fname);

    char dir_resolved[1024] = {0};
#if defined(_WIN32)
    if (g_dir[0] && (g_dir[0] == '/' || g_dir[0] == '\\' || (g_dir[1] == ':' && (g_dir[2] == '\\' || g_dir[2] == '/'))))
    {
        snprintf(dir_resolved, sizeof dir_resolved, "%s", g_dir);
    }
    else
    {
        char exe_dir[1024] = {0};
        if (GetModuleFileNameA(NULL, exe_dir, sizeof exe_dir - 1))
        {
            char *last_slash = strrchr(exe_dir, '\\');
            if (!last_slash) last_slash = strrchr(exe_dir, '/');
            if (last_slash) *last_slash = 0;
            snprintf(dir_resolved, sizeof dir_resolved, "%s", exe_dir);
        }
        else
        {
            snprintf(dir_resolved, sizeof dir_resolved, "%s", g_dir[0] ? g_dir : ".");
        }
    }
    CreateDirectoryA(dir_resolved, NULL);
#else
    snprintf(dir_resolved, sizeof dir_resolved, "%s", g_dir[0] ? g_dir : ".");
#endif

    char crash_path[1024];
    snprintf(crash_path, sizeof crash_path, "%s\\%s-crash-%s.txt",
             dir_resolved, g_app, fname);
    char lastlog[1024];
    snprintf(lastlog, sizeof lastlog, "%s\\%s-lastlog.txt", dir_resolved, g_app);

#if defined(_WIN32)
    char dmp_path[1024];
    char last_dmp_path[1024];
    snprintf(dmp_path, sizeof dmp_path, "%s\\%s-crash-%s.dmp", dir_resolved, g_app, fname);
    snprintf(last_dmp_path, sizeof last_dmp_path, "%s\\%s-lastcrash.dmp", dir_resolved, g_app);
    int dmp_created = 0;
    HANDLE hDmp = CreateFileA(dmp_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hDmp != INVALID_HANDLE_VALUE)
    {
        CONTEXT ctx_rec;
        memset(&ctx_rec, 0, sizeof ctx_rec);
        ctx_rec.ContextFlags = CONTEXT_FULL;
        RtlCaptureContext(&ctx_rec);
        EXCEPTION_RECORD exc_rec;
        memset(&exc_rec, 0, sizeof exc_rec);
        exc_rec.ExceptionCode = (DWORD)code;
        exc_rec.ExceptionAddress = addr ? addr : (void *)ctx_rec.Rip;
        EXCEPTION_POINTERS ep_synth;
        ep_synth.ContextRecord = &ctx_rec;
        ep_synth.ExceptionRecord = &exc_rec;

        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = g_cur_ep ? g_cur_ep : &ep_synth;
        mei.ClientPointers = FALSE;

        MINIDUMP_TYPE dmp_type = (MINIDUMP_TYPE)(
            MiniDumpNormal |
            MiniDumpWithDataSegs |
            MiniDumpWithHandleData |
            MiniDumpWithUnloadedModules |
            MiniDumpWithThreadInfo
        );
        if (MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                              hDmp, dmp_type, &mei, NULL, NULL))
        {
            dmp_created = 1;
            CopyFileA(dmp_path, last_dmp_path, FALSE);
        }
        CloseHandle(hDmp);
    }
#endif

    FILE *f = fopen(crash_path, "wb");
    if (!f)
        f = stderr; /* fall back to stderr so we never lose the report */

    fprintf(f, "================ %s CRASH REPORT ================\n", g_app);
    fprintf(f, " app      : %s\n", g_app);
    fprintf(f, " version  : %s\n", g_ver);
    fprintf(f, " host     : %s (%s)\n", platform_str(),
#if defined(_WIN32)
            "x86-64"
#elif defined(__SIZEOF_POINTER__)
            (__SIZEOF_POINTER__ == 8 ? "64-bit" : "32-bit")
#else
            "?"
#endif
            );
    fprintf(f, " time     : %s\n", ts);
    fprintf(f, " kind     : %s\n", kind);
    fprintf(f, " reason   : %s%s%s\n", name,
            reason ? " — " : "", reason ? reason : "");
    fprintf(f, " code     : 0x%lX (%ld)\n", (unsigned long)code, code);
    fprintf(f, " fault    : 0x%p\n", addr);
#if defined(_WIN32)
    if (dmp_created)
        fprintf(f, " minidump : %s\n", dmp_path);
#endif

    /* ---- backtrace (platform) ---- */
    fprintf(f, " backtrace:\n");
#if defined(_WIN32)
    {
        HANDLE process = GetCurrentProcess();
        SymInitialize(process, NULL, TRUE);
        void *frames[64];
        ULONG n = RtlCaptureStackBackTrace(0, 64, frames, NULL);
        char sym_buf[sizeof(SYMBOL_INFO) + 256];
        PSYMBOL_INFO symbol = (PSYMBOL_INFO)sym_buf;
        symbol->MaxNameLen = 255;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        IMAGEHLP_LINE64 line;
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

        for (ULONG i = 0; i < n; i++)
        {
            DWORD64 displacement = 0;
            DWORD disp_line = 0;
            HMODULE hmod = NULL;
            const char *mod = "?";
            char modpath[512] = {0};
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    (LPCSTR)frames[i], &hmod) && hmod)
            {
                if (GetModuleFileNameA(hmod, modpath, 511))
                    mod = modpath;
            }
            const char *base = mod;
            for (const char *p = mod; *p; p++)
                if (*p == '\\' || *p == '/')
                    base = p + 1;

            if (SymFromAddr(process, (DWORD64)frames[i], &displacement, symbol))
            {
                if (SymGetLineFromAddr64(process, (DWORD64)frames[i], &disp_line, &line))
                {
                    fprintf(f, "   #%-2u 0x%p  %s!%s (%s:%lu)\n", (unsigned)i, frames[i],
                            base, symbol->Name, line.FileName, (unsigned long)line.LineNumber);
                }
                else
                {
                    fprintf(f, "   #%-2u 0x%p  %s!%s +0x%llX\n", (unsigned)i, frames[i],
                            base, symbol->Name, (unsigned long long)displacement);
                }
            }
            else
            {
                fprintf(f, "   #%-2u 0x%p  %s +0x%lX\n", (unsigned)i, frames[i],
                        base, (unsigned long)((char *)frames[i] - (char *)hmod));
            }
        }
        if (n == 0)
            fprintf(f, "   (no frames captured)\n");
    }
#elif (defined(__APPLE__) || defined(__linux__)) && defined(MINI_HAVE_BACKTRACE)
    {
        void *frames[64];
        int n = backtrace(frames, 64);
        char **syms = backtrace_symbols(frames, n);
        for (int i = 0; i < n; i++)
            fprintf(f, "   #%-2d %s\n", i, syms ? syms[i] : "?");
        free(syms);
        if (n == 0)
            fprintf(f, "   (no frames captured)\n");
    }
#else
    fprintf(f, "   (backtrace not available on this platform)\n");
#endif

    fprintf(f, "\n-- structured log ring dumped to: %s --\n", lastlog);
    fprintf(f, "==================================================\n");

    if (f != stderr)
    {
        /* also mirror to stderr so it is visible in the terminal */
        fflush(f);
        fclose(f);
    }

    /* spill the mini_log ring to a stable-name file for easy retrieval */
    mini_log_dump_to_file(lastlog);

    /* flush the persistent log file (if any) so nothing is stuck in stdio */
    mini_log_flush();
}

/* ------------------------------------------------------------------ */
/* Windows: SEH unhandled-exception filter                             */
/* ------------------------------------------------------------------ */
#if defined(_WIN32)
static LONG WINAPI seh_filter(struct _EXCEPTION_POINTERS *ep)
{
    g_cur_ep = ep;
    const char *name = "EXCEPTION";
    DWORD code = ep ? ep->ExceptionRecord->ExceptionCode : 0;
    void *addr = ep && ep->ExceptionRecord ?
                 (void *)ep->ExceptionRecord->ExceptionAddress : NULL;
    switch (code)
    {
        case 0xC0000005: name = "ACCESS_VIOLATION"; break;
        case 0xC000001D: name = "ILLEGAL_INSTRUCTION"; break;
        case 0xC0000094: name = "INT_DIVIDE_BY_ZERO"; break;
        case 0xC0000095: name = "INT_OVERFLOW"; break;
        case 0xC0000096: name = "PRIV_INSTRUCTION"; break;
        case 0xC00000FD: name = "STACK_OVERFLOW"; break;
        case 0xC0000409: name = "STATUS_STACK_BUFFER_OVERRUN"; break;
        case 0xE06D7363: name = "CPP_EXCEPTION"; break;
        default: break;
    }
    /* access-violation record carries the faulting address in ExceptionInformation[1] */
    if (code == 0xC0000005 && ep && ep->ExceptionRecord->NumberParameters >= 2)
        addr = (void *)ep->ExceptionRecord->ExceptionInformation[1];
    write_dump("SEH", name, (long)code, addr, NULL);
    _exit(1); /* never returns; avoids re-entering the broken state */
    return EXCEPTION_EXECUTE_HANDLER; /* unreachable */
}
#endif

/* ------------------------------------------------------------------ */
/* C-level signal handler (Windows + POSIX)                            */
/* ------------------------------------------------------------------ */
static const char *sig_name(int sig)
{
#if defined(SIGSEGV)
    if (sig == SIGSEGV) return "SIGSEGV";
#endif
#if defined(SIGABRT)
    if (sig == SIGABRT) return "SIGABRT";
#endif
#if defined(SIGFPE)
    if (sig == SIGFPE) return "SIGFPE";
#endif
#if defined(SIGILL)
    if (sig == SIGILL) return "SIGILL";
#endif
#if defined(SIGBUS)
    if (sig == SIGBUS) return "SIGBUS";
#endif
#if defined(SIGTRAP)
    if (sig == SIGTRAP) return "SIGTRAP";
#endif
#if defined(SIGSYS)
    if (sig == SIGSYS) return "SIGSYS";
#endif
#if defined(SIGINT)
    if (sig == SIGINT) return "SIGINT";
#endif
#if defined(SIGTERM)
    if (sig == SIGTERM) return "SIGTERM";
#endif
    return "SIG?";
}

#if defined(_WIN32)
static void sig_handler_win(int sig)
{
    write_dump("signal", sig_name(sig), (long)sig, NULL, NULL);
    _exit(1);
}
#else
static void sig_handler_posix(int sig, siginfo_t *info, void *uctx)
{
    (void)uctx;
    void *addr = info ? info->si_addr : NULL;
    write_dump("signal", sig_name(sig), (long)sig, addr, NULL);
    _exit(1);
}
#endif

#if defined(_WIN32)
static LONG WINAPI veh_filter(struct _EXCEPTION_POINTERS *ep)
{
    if (!ep || !ep->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0xC0000005 || /* ACCESS_VIOLATION */
        code == 0xC000001D || /* ILLEGAL_INSTRUCTION */
        code == 0xC0000094 || /* INT_DIVIDE_BY_ZERO */
        code == 0xC00000FD || /* STACK_OVERFLOW */
        code == 0xC0000409)   /* STATUS_STACK_BUFFER_OVERRUN */
    {
        return seh_filter(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

/* ------------------------------------------------------------------ */
/* installation                                                       */
/* ------------------------------------------------------------------ */
void mini_crash_init(const char *app_name, const char *log_dir)
{
    if (app_name && app_name[0])
        snprintf(g_app, sizeof g_app, "%s", app_name);
    if (log_dir && log_dir[0])
        snprintf(g_dir, sizeof g_dir, "%s", log_dir);
    if (g_installed)
        return;
    g_installed = 1;

#if defined(_WIN32)
    AddVectoredExceptionHandler(1, veh_filter);
    SetUnhandledExceptionFilter(seh_filter);
    signal(SIGABRT, sig_handler_win);
    signal(SIGFPE,  sig_handler_win);
    signal(SIGILL,  sig_handler_win);
    signal(SIGSEGV, sig_handler_win);
    signal(SIGINT,  sig_handler_win);
    signal(SIGTERM, sig_handler_win);
#else
    /* alternate stack so a stack-overflow SIGSEGV is still reportable */
    static char altstack[SIGSTKSZ * 2];
    stack_t ss;
    ss.ss_sp = altstack;
    ss.ss_size = sizeof altstack;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = sig_handler_posix;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
#ifdef SIGBUS
    sigaction(SIGBUS,  &sa, NULL);
#endif
#ifdef SIGTRAP
    sigaction(SIGTRAP, &sa, NULL);
#endif
#ifdef SIGSYS
    sigaction(SIGSYS,  &sa, NULL);
#endif
#endif
}

void mini_crash_report(const char *reason, int code)
{
    write_dump("manual", "FATAL_ASSERT", (long)code, NULL, reason);
    _exit(code ? code : 1);
}

__attribute__((optimize("O0"))) void mini_crash_self_test(void)
{
    volatile int *ptr = (volatile int *)0;
    *ptr = 0xDEADBEEF;
}
