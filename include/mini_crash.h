/*
 * mini_crash.h — system-level crash interception (cross-platform).
 *
 * Installs handlers that catch the fatal signals / structured exceptions the
 * host OS raises (NULL deref, divide-by-zero, abort, illegal instruction,
 * stack overflow, ...) and, instead of letting the process die silently:
 *
 *   1. writes a human-readable crash dump to
 *        <log_dir>/<app_name>-crash-<yyyymmdd-hhmmss>.txt
 *      (and mirrors it to stderr);
 *   2. flushes the structured log ring (mini_log) into that same dump so the
 *      last ~1000 JS console / app log lines survive the crash;
 *   3. terminates with _exit() so the handler is not re-entered.
 *
 * Cross-platform: Windows uses SetUnhandledExceptionFilter (SEH) + signal()
 * for the C signals and RtlCaptureStackBackTrace for the backtrace; POSIX
 * uses sigaction() on the fatal set with an alternate stack (so a stack
 * overflow can still be reported) and backtrace()/backtrace_symbols().
 *
 * Crash handlers are inherently best-effort (only async-signal-safe ops are
 * strictly guaranteed); this module favours "produce a useful report over
 * silence". Pure C99.
 */
#ifndef MINI_CRASH_H
#define MINI_CRASH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Install the handlers. app_name / log_dir may be NULL (defaults:
   "tiny_app" / "."). Idempotent; safe to call once at startup. */
void mini_crash_init(const char *app_name, const char *log_dir);

/* Set the application version string included in the dump header. */
void mini_crash_set_version(const char *version);

/* Manually report a fatal condition (assert-style). Writes the dump and
 * terminates with the given code. */
void mini_crash_report(const char *reason, int code);

/* Deliberately trigger a crash (NULL pointer write) so the handler can be
 * verified end-to-end. */
void mini_crash_self_test(void);

#ifdef __cplusplus
}
#endif
#endif /* MINI_CRASH_H */
