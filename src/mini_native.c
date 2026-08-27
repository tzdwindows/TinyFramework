/*
 * mini_native.c — OS capability + Electron-style module surface.
 *
 * Adds a Node/Electron-shaped surface (os / process / child_process / fs /
 * path / electron) on top of the sandboxed browser bridge, plus a CommonJS
 * `require` mechanism. The JS-side require lives in mini_js_shim; the C side
 * (this file) builds a table of built-in module objects (__miniBuiltinModules)
 * and registers the global `process`.
 *
 * Cross-platform: _WIN32 (Win32 API) vs __APPLE__ (macOS) vs __linux__/
 * __FreeBSD__ (POSIX). Phase 1 covers os + process + require skeleton;
 * fs/path (Phase 2), child_process (Phase 3) and the electron namespace
 * (Phase 4) are appended to install_native() in later steps.
 */
#include "mini_native.h"
#include "mini_js_bridge.h"
#include "mini_renderer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <quickjs.h>
#if defined(_WIN32)
#  define GLFW_EXPOSE_NATIVE_WIN32
#  include <GLFW/glfw3.h>
#  include <GLFW/glfw3native.h>
#else
#  include <GLFW/glfw3.h>
#endif

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0600
#  endif
#  include <windows.h>
#  include <shellapi.h>
#  include <shlobj.h>
#  include <commdlg.h>
#  include <direct.h>
#  include <io.h>
#  define GETCWD _getcwd
#  define CHDIR  _chdir
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/utsname.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <pwd.h>
#  include <dirent.h>
#  include <dlfcn.h>
#  define GETCWD getcwd
#  define CHDIR  chdir
#  if defined(__linux__)
#    include <sys/sysinfo.h>
#  endif
#  if defined(__APPLE__)
#    include <mach-o/dyld.h>
#  endif
#endif

/* ---- bridge access -------------------------------------------------------- */

static struct MiniBridge *nb_of(JSContext *ctx)
{
    return (struct MiniBridge *)JS_GetContextOpaque(ctx);
}

/* ---- UTF-8 helpers (Windows wide-string <-> UTF-8) ----------------------- */

#if defined(_WIN32)
/* Convert a UTF-16 wchar string to a freshly malloc'd UTF-8 string. */
static char *wide_to_utf8(const wchar_t *w)
{
    if (!w)
        return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0)
        return NULL;
    char *s = (char *)malloc((size_t)n);
    if (!s)
        return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}
/* Convert a UTF-8 string to a freshly malloc'd wchar string. */
static wchar_t *utf8_to_wide(const char *s)
{
    if (!s)
        return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w)
        return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
#endif

/* ---- platform / arch strings --------------------------------------------- */

static const char *plat_str(void)
{
#if defined(_WIN32)
    return "win32";
#elif defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#elif defined(__FreeBSD__)
    return "freebsd";
#else
    return "unknown";
#endif
}

static const char *arch_str(void)
{
#if defined(_WIN32)
#  if defined(_M_X64) || defined(__x86_64__)
    return "x64";
#  elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#  elif defined(_M_IX86) || defined(__i386__)
    return "ia32";
#  elif defined(_M_ARM) || defined(__arm__)
    return "arm";
#  else
    return "unknown";
#  endif
#else
#  if defined(__x86_64__)
    return "x64";
#  elif defined(__aarch64__)
    return "arm64";
#  elif defined(__i386__)
    return "ia32";
#  elif defined(__arm__)
    return "arm";
#  else
    return "unknown";
#  endif
#endif
}

/* Browser-style navigator.platform derived from the OS. */
const char *mini_navigator_platform(void)
{
#if defined(_WIN32)
    return "Win32";
#elif defined(__APPLE__)
    return "MacIntel";
#elif defined(__linux__) || defined(__FreeBSD__)
    /* e.g. "Linux x86_64" — matches what Chromium reports on Linux. */
    if (!strcmp(arch_str(), "x64"))
        return "Linux x86_64";
    if (!strcmp(arch_str(), "arm64"))
        return "Linux aarch64";
    if (!strcmp(arch_str(), "ia32"))
        return "Linux i686";
    return "Linux";
#else
    return "Unknown";
#endif
}

/* os.EOL */
static const char *eol_str(void)
{
#if defined(_WIN32)
    return "\r\n";
#else
    return "\n";
#endif
}

/* ================================================================== */
/* os module                                                          */
/* ================================================================== */

static JSValue js_os_platform(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
    return JS_NewString(ctx, plat_str());
}

static JSValue js_os_arch(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
    return JS_NewString(ctx, arch_str());
}

static JSValue js_os_type(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
#if defined(_WIN32)
    return JS_NewString(ctx, "Windows_NT");
#elif defined(__APPLE__)
    return JS_NewString(ctx, "Darwin");
#elif defined(__linux__)
    return JS_NewString(ctx, "Linux");
#elif defined(__FreeBSD__)
    return JS_NewString(ctx, "FreeBSD");
#else
    return JS_NewString(ctx, "Unknown");
#endif
}

static JSValue js_os_endianness(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
    /* Runtime endianness probe (covers ARM big-endian edge cases). */
    union { uint32_t u; uint8_t b[4]; } probe;
    probe.u = 1;
    return JS_NewString(ctx, probe.b[0] ? "LE" : "BE");
}

static JSValue js_os_hostname(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
#if defined(_WIN32)
    wchar_t buf[256] = {0};
    DWORD sz = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    if (GetComputerNameW(buf, &sz))
    {
        char *s = wide_to_utf8(buf);
        if (s)
        {
            JSValue v = JS_NewString(ctx, s);
            free(s);
            return v;
        }
    }
    return JS_NewString(ctx, "localhost");
#else
    char buf[256] = {0};
    if (gethostname(buf, sizeof(buf) - 1) == 0)
        return JS_NewString(ctx, buf);
    return JS_NewString(ctx, "localhost");
#endif
}

static JSValue js_os_homedir(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
#if defined(_WIN32)
    const char *h = getenv("USERPROFILE");
    if (h && h[0])
        return JS_NewString(ctx, h);
    {
        wchar_t buf[MAX_PATH] = {0};
        if (SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, buf) == S_OK)
        {
            char *s = wide_to_utf8(buf);
            if (s)
            {
                JSValue v = JS_NewString(ctx, s);
                free(s);
                return v;
            }
        }
    }
    return JS_NewString(ctx, getenv("SystemDrive") ? getenv("SystemDrive") : "C:\\");
#else
    const char *h = getenv("HOME");
    if (h && h[0])
        return JS_NewString(ctx, h);
    struct passwd *pw = getpwuid(getuid());
    return JS_NewString(ctx, (pw && pw->pw_dir) ? pw->pw_dir : "/");
#endif
}

static JSValue js_os_tmpdir(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
#if defined(_WIN32)
    const char *t = getenv("TEMP");
    if (!t || !t[0])
        t = getenv("TMP");
    if (!t || !t[0])
        t = getenv("USERPROFILE");
    if (!t || !t[0])
        t = "C:\\Windows\\Temp";
    return JS_NewString(ctx, t);
#else
    const char *t = getenv("TMPDIR");
    if (t && t[0])
        return JS_NewString(ctx, t);
    return JS_NewString(ctx, "/tmp");
#endif
}

static JSValue js_os_release(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
#if defined(_WIN32)
    /* "major.minor.build" — dwVersion layout via RtlGetVersion when available. */
    OSVERSIONINFOEXW oi = {0};
    oi.dwOSVersionInfoSize = sizeof(oi);
    /* GetVersionEx is deprecated/clamped post-Win8.1; still gives a usable
       base; a fuller answer would RtlGetVersion from ntdll. */
    if (GetVersionExW((OSVERSIONINFOW *)&oi))
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%lu.%lu.%lu",
                 (unsigned long)oi.dwMajorVersion,
                 (unsigned long)oi.dwMinorVersion,
                 (unsigned long)oi.dwBuildNumber);
        return JS_NewString(ctx, buf);
    }
    return JS_NewString(ctx, "0.0.0");
#else
    struct utsname u;
    if (uname(&u) == 0)
        return JS_NewString(ctx, u.release);
    return JS_NewString(ctx, "0.0.0");
#endif
}

static JSValue js_os_totalmem(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
#if defined(_WIN32)
    MEMORYSTATUSEX ms = {0};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
        return JS_NewFloat64(ctx, (double)ms.ullTotalPhys);
    return JS_NewFloat64(ctx, 0.0);
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0)
        return JS_NewFloat64(ctx, (double)pages * (double)page_size);
    return JS_NewFloat64(ctx, 0.0);
#endif
}

static JSValue js_os_freemem(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
#if defined(_WIN32)
    MEMORYSTATUSEX ms = {0};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
        return JS_NewFloat64(ctx, (double)ms.ullAvailPhys);
    return JS_NewFloat64(ctx, 0.0);
#else
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0)
        return JS_NewFloat64(ctx, (double)pages * (double)page_size);
#  if defined(__linux__)
    {
        struct sysinfo si;
        if (sysinfo(&si) == 0)
            return JS_NewFloat64(ctx, (double)si.freeram * (double)si.mem_unit);
    }
#  endif
    return JS_NewFloat64(ctx, 0.0);
#endif
}

static JSValue js_os_uptime(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
#if defined(_WIN32)
    return JS_NewFloat64(ctx, (double)GetTickCount64() / 1000.0);
#else
    FILE *f = fopen("/proc/uptime", "r");
    if (f)
    {
        double up = 0.0;
        if (fscanf(f, "%lf", &up) == 1)
        {
            fclose(f);
            return JS_NewFloat64(ctx, up);
        }
        fclose(f);
    }
    struct utsname u;
    if (uname(&u) == 0 && !strcmp(u.sysname, "Darwin"))
    {
        /* macOS: fall back to clock since boot (approx). */
        return JS_NewFloat64(ctx, (double)time(NULL) - (double)0);
    }
    return JS_NewFloat64(ctx, 0.0);
#endif
}

static JSValue js_os_loadavg(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
#if defined(_WIN32)
    /* Windows has no load-average concept. */
    JSValue a = JS_NewArray(ctx);
    double z = 0.0;
    JS_SetPropertyInt64(ctx, a, 0, JS_NewFloat64(ctx, z));
    JS_SetPropertyInt64(ctx, a, 1, JS_NewFloat64(ctx, z));
    JS_SetPropertyInt64(ctx, a, 2, JS_NewFloat64(ctx, z));
    return a;
#else
    double avg[3] = {0, 0, 0};
    int n = getloadavg(avg, 3);
    (void)n;
    JSValue a = JS_NewArray(ctx);
    JS_SetPropertyInt64(ctx, a, 0, JS_NewFloat64(ctx, avg[0]));
    JS_SetPropertyInt64(ctx, a, 1, JS_NewFloat64(ctx, avg[1]));
    JS_SetPropertyInt64(ctx, a, 2, JS_NewFloat64(ctx, avg[2]));
    return a;
#endif
}

/* os.cpus(): array of {model, speed, times:{user,nice,sys,idle,irq}} */
static JSValue js_os_cpus(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
    char model[256] = "unknown";
    int count = 1;
#if defined(_WIN32)
    {
        HKEY hKey;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                          "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                          0, KEY_READ, &hKey) == ERROR_SUCCESS)
        {
            DWORD sz = (DWORD)sizeof(model) - 1;
            RegQueryValueExA(hKey, "ProcessorNameString", NULL, NULL,
                             (LPBYTE)model, &sz);
            RegCloseKey(hKey);
        }
    }
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        count = (int)si.dwNumberOfProcessors;
    }
#else
    {
        FILE *f = fopen("/proc/cpuinfo", "r");
        if (f)
        {
            char line[512];
            count = 0;
            while (fgets(line, sizeof(line), f))
            {
                if (!strncmp(line, "model name", 10))
                {
                    char *p = strchr(line, ':');
                    if (p)
                    {
                        p++;
                        while (*p == ' ' || *p == '\t')
                            p++;
                        size_t l = strlen(p);
                        while (l && (p[l - 1] == '\n' || p[l - 1] == '\r'))
                            p[--l] = '\0';
                        snprintf(model, sizeof(model), "%s", p);
                    }
                }
                if (!strncmp(line, "processor", 9))
                    count++;
            }
            fclose(f);
            if (count == 0)
                count = 1;
        }
    }
#endif
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < count; i++)
    {
        JSValue cpu = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, cpu, "model", JS_NewString(ctx, model));
        JS_SetPropertyStr(ctx, cpu, "speed", JS_NewInt32(ctx, 0));
        JSValue times = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, times, "user", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, times, "nice", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, times, "sys", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, times, "idle", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, times, "irq", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, cpu, "times", times);
        JS_SetPropertyInt64(ctx, arr, i, cpu);
    }
    return arr;
}

/* os.networkInterfaces(): {name: [{address,netmask,family,mac,internal}]}
   Phase 1 returns an empty object (needs iphlpapi/lib linkage; filled in
   a later phase). */
static JSValue js_os_networkInterfaces(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
    /* TODO(phase5): enumerate via GetAdaptersAddresses (Win) / getifaddrs (POSIX). */
    return JS_NewObject(ctx);
}

static JSValue js_os_userInfo(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
    JSValue o = JS_NewObject(ctx);
#if defined(_WIN32)
    const char *u = getenv("USERNAME");
    if (!u)
        u = getenv("USER");
    JS_SetPropertyStr(ctx, o, "username", JS_NewString(ctx, u ? u : "user"));
    const char *h = getenv("USERPROFILE");
    JS_SetPropertyStr(ctx, o, "homedir", JS_NewString(ctx, h ? h : getenv("SystemDrive") ? getenv("SystemDrive") : "C:\\"));
    JS_SetPropertyStr(ctx, o, "shell", JS_NULL);
#else
    struct passwd *pw = getpwuid(getuid());
    JS_SetPropertyStr(ctx, o, "username", JS_NewString(ctx, (pw && pw->pw_name) ? pw->pw_name : "user"));
    const char *h = getenv("HOME");
    JS_SetPropertyStr(ctx, o, "homedir", JS_NewString(ctx, h ? h : (pw && pw->pw_dir) ? pw->pw_dir : "/"));
    JS_SetPropertyStr(ctx, o, "shell", JS_NewString(ctx, (pw && pw->pw_shell) ? pw->pw_shell : "/bin/sh"));
#endif
    return o;
}

static void install_os(JSContext *ctx, JSValue mods)
{
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "platform", JS_NewCFunction(ctx, js_os_platform, "platform", 0));
    JS_SetPropertyStr(ctx, o, "arch", JS_NewCFunction(ctx, js_os_arch, "arch", 0));
    JS_SetPropertyStr(ctx, o, "type", JS_NewCFunction(ctx, js_os_type, "type", 0));
    JS_SetPropertyStr(ctx, o, "endianness", JS_NewCFunction(ctx, js_os_endianness, "endianness", 0));
    JS_SetPropertyStr(ctx, o, "hostname", JS_NewCFunction(ctx, js_os_hostname, "hostname", 0));
    JS_SetPropertyStr(ctx, o, "homedir", JS_NewCFunction(ctx, js_os_homedir, "homedir", 0));
    JS_SetPropertyStr(ctx, o, "tmpdir", JS_NewCFunction(ctx, js_os_tmpdir, "tmpdir", 0));
    JS_SetPropertyStr(ctx, o, "release", JS_NewCFunction(ctx, js_os_release, "release", 0));
    JS_SetPropertyStr(ctx, o, "totalmem", JS_NewCFunction(ctx, js_os_totalmem, "totalmem", 0));
    JS_SetPropertyStr(ctx, o, "freemem", JS_NewCFunction(ctx, js_os_freemem, "freemem", 0));
    JS_SetPropertyStr(ctx, o, "uptime", JS_NewCFunction(ctx, js_os_uptime, "uptime", 0));
    JS_SetPropertyStr(ctx, o, "loadavg", JS_NewCFunction(ctx, js_os_loadavg, "loadavg", 0));
    JS_SetPropertyStr(ctx, o, "cpus", JS_NewCFunction(ctx, js_os_cpus, "cpus", 0));
    JS_SetPropertyStr(ctx, o, "networkInterfaces", JS_NewCFunction(ctx, js_os_networkInterfaces, "networkInterfaces", 0));
    JS_SetPropertyStr(ctx, o, "userInfo", JS_NewCFunction(ctx, js_os_userInfo, "userInfo", 0));
    JS_SetPropertyStr(ctx, o, "EOL", JS_NewString(ctx, eol_str()));
    JS_SetPropertyStr(ctx, mods, "os", o);
}

/* ================================================================== */
/* process global                                                     */
/* ================================================================== */

static double g_proc_start = -1.0; /* first-call lazy init (seconds since epoch) */

static JSValue js_process_cwd(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
    char buf[4096] = {0};
    if (GETCWD(buf, sizeof(buf) - 1))
        return JS_NewString(ctx, buf);
    return JS_NewString(ctx, ".");
}

static JSValue js_process_chdir(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "chdir(path) expected a string");
    const char *p = JS_ToCString(ctx, argv[0]);
    int rc = p ? CHDIR(p) : -1;
    JS_FreeCString(ctx, p);
    if (rc != 0)
        return JS_ThrowTypeError(ctx, "chdir failed: %s", strerror(errno));
    return JS_UNDEFINED;
}

static JSValue js_process_exit(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    int code = 0;
    if (argc > 0)
        JS_ToInt32(ctx, &code, argv[0]);
    struct MiniBridge *b = nb_of(ctx);
    struct MiniRenderer *r = mini_bridge_renderer(b);
    if (r && r->gpu.window_handle)
        glfwSetWindowShouldClose((GLFWwindow *)r->gpu.window_handle, GLFW_TRUE);
    return JS_UNDEFINED;
}

static JSValue js_process_uptime(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
    if (g_proc_start < 0)
        g_proc_start = glfwGetTime();
    return JS_NewFloat64(ctx, glfwGetTime() - g_proc_start);
}

static JSValue js_process_hrtime(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    (void)argc; (void)argv;
#if defined(_WIN32)
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER now;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    double secs = (double)now.QuadPart / (double)freq.QuadPart;
    JSValue a = JS_NewArray(ctx);
    JS_SetPropertyInt64(ctx, a, 0, JS_NewInt32(ctx, (int32_t)secs));
    JS_SetPropertyInt64(ctx, a, 1, JS_NewInt32(ctx, (int32_t)((secs - (int32_t)secs) * 1e9)));
    return a;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    JSValue a = JS_NewArray(ctx);
    JS_SetPropertyInt64(ctx, a, 0, JS_NewInt32(ctx, (int32_t)ts.tv_sec));
    JS_SetPropertyInt64(ctx, a, 1, JS_NewInt32(ctx, (int32_t)ts.tv_nsec));
    return a;
#endif
}

/* process.stdout/stderr.write — write a string to the host stdout/stderr. */
static JSValue js_procstream_write(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    /* The stream object is `this` (tv); we don't distinguish stdout/stderr
       via magic here — stdout/stderr share an impl but the JS shim sets each. */
    (void)tv;
    if (argc < 1)
        return JS_FALSE;
    const char *s = NULL;
    size_t len = 0;
    if (JS_IsString(argv[0]))
        s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (s)
    {
        fwrite(s, 1, len, stdout);
        fflush(stdout);
        JS_FreeCString(ctx, s);
    }
    return JS_TRUE;
}
static JSValue js_procstream_writeErr(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1)
        return JS_FALSE;
    const char *s = NULL;
    size_t len = 0;
    if (JS_IsString(argv[0]))
        s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (s)
    {
        fwrite(s, 1, len, stderr);
        fflush(stderr);
        JS_FreeCString(ctx, s);
    }
    return JS_TRUE;
}

/* Build the process.env object (snapshot at startup). */
static JSValue build_env(JSContext *ctx)
{
    JSValue env = JS_NewObject(ctx);
#if defined(_WIN32)
    /* GetEnvironmentStringsW returns a sequence of "NAME=VALUE\0" UTF-16
       blocks, terminated by a double NUL. */
    wchar_t *raw = GetEnvironmentStringsW();
    if (raw)
    {
        wchar_t *p = raw;
        while (*p)
        {
            wchar_t *eq = wcschr(p, L'=');
            if (eq && eq != p) /* skip leading '=' (Win drives) */
            {
                *eq = L'\0';
                char *name = wide_to_utf8(p);
                char *val = wide_to_utf8(eq + 1);
                if (name && val)
                {
                    /* Windows env names are case-insensitive; normalize to
                       upper so process.env.PATH works regardless of the
                       stored casing (Path vs PATH). */
                    for (char *q = name; *q; q++)
                        if (*q >= 'a' && *q <= 'z')
                            *q -= 32;
                    JS_SetPropertyStr(ctx, env, name, JS_NewString(ctx, val));
                }
                free(name);
                free(val);
                *eq = L'=';
            }
            p += wcslen(p) + 1;
        }
        FreeEnvironmentStringsW(raw);
    }
#else
    extern char **environ;
    for (char **e = environ; *e; e++)
    {
        char *eq = strchr(*e, '=');
        if (eq && eq != *e)
        {
            size_t nl = (size_t)(eq - *e);
            char *name = (char *)malloc(nl + 1);
            if (!name)
                continue;
            memcpy(name, *e, nl);
            name[nl] = '\0';
            JS_SetPropertyStr(ctx, env, name, JS_NewString(ctx, eq + 1));
            free(name);
        }
    }
#endif
    return env;
}

/* process.execPath — absolute path to the running executable. */
static JSValue proc_execpath(JSContext *ctx)
{
#if defined(_WIN32)
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD n = GetModuleFileNameW(NULL, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n > 0 && n < (DWORD)(sizeof(buf) / sizeof(buf[0])))
    {
        char *s = wide_to_utf8(buf);
        if (s)
        {
            JSValue v = JS_NewString(ctx, s);
            free(s);
            return v;
        }
    }
    return JS_NewString(ctx, "tiny_app");
#else
    char buf[4096] = {0};
    ssize_t n;
#  if defined(__linux__)
    n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
#  elif defined(__APPLE__)
    uint32_t sz = (uint32_t)sizeof(buf) - 1;
    if (_NSGetExecutablePath(buf, &sz) == 0)
        n = (ssize_t)strlen(buf);
    else
        n = -1;
    (void)sz;
#  else
    n = readlink("/proc/curproc/file", buf, sizeof(buf) - 1);
#  endif
    if (n > 0)
    {
        buf[n] = '\0';
        return JS_NewString(ctx, buf);
    }
    return JS_NewString(ctx, "tiny_app");
#endif
}

/* Build the global `process` object (no argv yet — set_argv fills it). */
static JSValue build_process(JSContext *ctx)
{
    JSValue p = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, p, "platform", JS_NewString(ctx, plat_str()));
    JS_SetPropertyStr(ctx, p, "arch", JS_NewString(ctx, arch_str()));
    /* pid/ppid are fixed integer values (Node exposes them as ints). */
    {
        int32_t pid_v = 0, ppid_v = 0;
#if defined(_WIN32)
        pid_v = (int32_t)GetCurrentProcessId();
#else
        pid_v = (int32_t)getpid();
        ppid_v = (int32_t)getppid();
#endif
        JS_SetPropertyStr(ctx, p, "pid", JS_NewInt32(ctx, pid_v));
        JS_SetPropertyStr(ctx, p, "ppid", JS_NewInt32(ctx, ppid_v));
    }
    JS_SetPropertyStr(ctx, p, "cwd", JS_NewCFunction(ctx, js_process_cwd, "cwd", 0));
    JS_SetPropertyStr(ctx, p, "chdir", JS_NewCFunction(ctx, js_process_chdir, "chdir", 1));
    JS_SetPropertyStr(ctx, p, "exit", JS_NewCFunction(ctx, js_process_exit, "exit", 0));
    JS_SetPropertyStr(ctx, p, "uptime", JS_NewCFunction(ctx, js_process_uptime, "uptime", 0));
    JS_SetPropertyStr(ctx, p, "hrtime", JS_NewCFunction(ctx, js_process_hrtime, "hrtime", 0));
    JS_SetPropertyStr(ctx, p, "execPath", proc_execpath(ctx));
    JS_SetPropertyStr(ctx, p, "argv", JS_NewArray(ctx)); /* filled by set_argv */
    JS_SetPropertyStr(ctx, p, "execArgv", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, p, "env", build_env(ctx));
    JS_SetPropertyStr(ctx, p, "version", JS_NewString(ctx, "v18.0.0"));
    {
        JSValue vers = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, vers, "tinyframework", JS_NewString(ctx, "1.0.0"));
        JS_SetPropertyStr(ctx, vers, "quickjs", JS_NewString(ctx, "2024-01-13"));
        JS_SetPropertyStr(ctx, vers, "node", JS_NewString(ctx, "18.0.0"));
        JS_SetPropertyStr(ctx, p, "versions", vers);
    }
    /* stdout / stderr */
    {
        JSValue out = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, out, "write", JS_NewCFunction(ctx, js_procstream_write, "write", 1));
        JS_SetPropertyStr(ctx, out, "isTTY", JS_FALSE);
        JS_SetPropertyStr(ctx, p, "stdout", out);
        JSValue err = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, err, "write", JS_NewCFunction(ctx, js_procstream_writeErr, "write", 1));
        JS_SetPropertyStr(ctx, err, "isTTY", JS_FALSE);
        JS_SetPropertyStr(ctx, p, "stderr", err);
    }
    return p;
}

static void install_process(JSContext *ctx, JSValue mods, JSValue global)
{
    JSValue p = build_process(ctx);
    JS_SetPropertyStr(ctx, mods, "process", JS_DupValue(ctx, p));
    JS_SetPropertyStr(ctx, global, "process", p); /* steals one ref */
}

/* ================================================================== */
/* fs module                                                          */
/* ================================================================== */

#if defined(_WIN32)
#  define MINI_MKDIR(p)   _mkdir(p)
#  define MINI_RMDIR(p)   _rmdir(p)
#  define MINI_UNLINK(p)  _unlink(p)
#  define MINI_CHMOD(p,m) _chmod((p),(m))
#  define MINI_STAT       _stat
#  define MINI_STATBUF    struct _stat
#else
#  define MINI_MKDIR(p)   mkdir((p),0755)
#  define MINI_RMDIR(p)   rmdir(p)
#  define MINI_UNLINK(p)  unlink(p)
#  define MINI_CHMOD(p,m) chmod((p),(m))
#  define MINI_STAT       stat
#  define MINI_STATBUF    struct stat
#endif

static JSValue js_fs_readFileSync(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "readFileSync(path[, opts]) expected a string path");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;

    int binary = 0;
    if (argc > 1)
    {
        JSValueConst o = argv[1];
        JSValue estr = JS_IsString(o) ? JS_DupValue(ctx, o)
                                      : (JS_IsObject(o) ? JS_GetPropertyStr(ctx, o, "encoding") : JS_UNDEFINED);
        if (JS_IsString(estr))
        {
            const char *enc = JS_ToCString(ctx, estr);
            if (enc)
            {
                binary = (strcmp(enc, "utf8") != 0 && strcmp(enc, "utf-8") != 0 &&
                         strcmp(enc, "ascii") != 0 && strcmp(enc, "latin1") != 0);
                JS_FreeCString(ctx, enc);
            }
        }
        JS_FreeValue(ctx, estr);
    }

    FILE *fp = fopen(path, "rb");
    if (!fp)
    {
        JSValue e = JS_ThrowTypeError(ctx, "ENOENT: no such file or directory, open '%s'", path);
        JS_FreeCString(ctx, path);
        return e;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0)
        sz = 0;
    unsigned char *buf = (unsigned char *)malloc((size_t)sz + 1);
    size_t rd = buf ? fread(buf, 1, (size_t)sz, fp) : 0;
    fclose(fp);
    JS_FreeCString(ctx, path);

    JSValue res;
    if (binary)
        res = JS_NewArrayBufferCopy(ctx, buf, rd);
    else
    {
        buf[rd] = '\0';
        res = JS_NewStringLen(ctx, (const char *)buf, rd);
    }
    free(buf);
    return res;
}

static JSValue js_fs_writeFileSync(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "writeFileSync(path, data[, opts]) expected (string, string|ArrayBuffer)");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    const char *data = NULL;
    size_t dlen = 0;
    int isbuf = 0;
    if (JS_IsString(argv[1]))
        data = JS_ToCStringLen(ctx, &dlen, argv[1]);
    else
    {
        size_t sz = 0;
        void *ab = JS_GetArrayBuffer(ctx, &sz, argv[1]);
        if (ab)
        {
            data = (const char *)ab;
            dlen = sz;
            isbuf = 1;
        }
    }
    if (!data)
    {
        JS_FreeCString(ctx, path);
        return JS_ThrowTypeError(ctx, "data must be a string or ArrayBuffer");
    }
    FILE *fp = fopen(path, "wb");
    if (!fp)
    {
        JSValue e = JS_ThrowTypeError(ctx, "EACCES: cannot open '%s' for writing", path);
        JS_FreeCString(ctx, path);
        if (!isbuf)
            JS_FreeCString(ctx, data);
        return e;
    }
    if (dlen)
        fwrite(data, 1, dlen, fp);
    fclose(fp);
    JS_FreeCString(ctx, path);
    if (!isbuf)
        JS_FreeCString(ctx, data);
    return JS_UNDEFINED;
}

static JSValue js_fs_appendFileSync(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "appendFileSync(path, data) expected (string, string)");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    const char *data = NULL;
    size_t dlen = 0;
    int isbuf = 0;
    if (JS_IsString(argv[1]))
        data = JS_ToCStringLen(ctx, &dlen, argv[1]);
    else
    {
        size_t sz = 0;
        void *ab = JS_GetArrayBuffer(ctx, &sz, argv[1]);
        if (ab)
        {
            data = (const char *)ab;
            dlen = sz;
            isbuf = 1;
        }
    }
    if (!data)
    {
        JS_FreeCString(ctx, path);
        return JS_ThrowTypeError(ctx, "data must be a string or ArrayBuffer");
    }
    FILE *fp = fopen(path, "ab");
    if (!fp)
    {
        JSValue e = JS_ThrowTypeError(ctx, "EACCES: cannot open '%s' for appending", path);
        JS_FreeCString(ctx, path);
        if (!isbuf)
            JS_FreeCString(ctx, data);
        return e;
    }
    if (dlen)
        fwrite(data, 1, dlen, fp);
    fclose(fp);
    JS_FreeCString(ctx, path);
    if (!isbuf)
        JS_FreeCString(ctx, data);
    return JS_UNDEFINED;
}

static JSValue js_fs_existsSync(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_FALSE;
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_FALSE;
    MINI_STATBUF st;
    int r = MINI_STAT(path, &st);
    JS_FreeCString(ctx, path);
    return r == 0 ? JS_TRUE : JS_FALSE;
}

static JSValue js_stat_isFile(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return JS_GetPropertyStr(ctx, tv, "_isfile");
}
static JSValue js_stat_isDirectory(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return JS_GetPropertyStr(ctx, tv, "_isdir");
}
static JSValue js_stat_false(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)ctx; (void)tv; (void)argc; (void)argv;
    return JS_FALSE; /* symlinks aren't detected; report false */
}

static JSValue js_fs_statSync(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "statSync(path) expected a string");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    MINI_STATBUF st;
    if (MINI_STAT(path, &st) != 0)
    {
        JSValue e = JS_ThrowTypeError(ctx, "ENOENT: no such file or directory, stat '%s'", path);
        JS_FreeCString(ctx, path);
        return e;
    }
    JS_FreeCString(ctx, path);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "size", JS_NewInt64(ctx, (int64_t)st.st_size));
    JS_SetPropertyStr(ctx, o, "mtimeMs", JS_NewFloat64(ctx, (double)st.st_mtime * 1000.0));
    JS_SetPropertyStr(ctx, o, "atimeMs", JS_NewFloat64(ctx, (double)st.st_atime * 1000.0));
    JS_SetPropertyStr(ctx, o, "ctimeMs", JS_NewFloat64(ctx, (double)st.st_ctime * 1000.0));
    int isdir = S_ISDIR(st.st_mode);
    JS_SetPropertyStr(ctx, o, "_isfile", JS_NewBool(ctx, S_ISREG(st.st_mode)));
    JS_SetPropertyStr(ctx, o, "_isdir", JS_NewBool(ctx, isdir));
    JS_SetPropertyStr(ctx, o, "isFile", JS_NewCFunction(ctx, js_stat_isFile, "isFile", 0));
    JS_SetPropertyStr(ctx, o, "isDirectory", JS_NewCFunction(ctx, js_stat_isDirectory, "isDirectory", 0));
    JS_SetPropertyStr(ctx, o, "isSymbolicLink", JS_NewCFunction(ctx, js_stat_false, "isSymbolicLink", 0));
    return o;
}

/* recursive mkdir helper: creates each path segment. Returns 0 on success. */
static int mkdir_p(const char *p)
{
    if (!p || !*p)
        return -1;
    char *tmp = (char *)malloc(strlen(p) + 1);
    if (!tmp)
        return -1;
    strcpy(tmp, p);
    int rc = 0;
    for (char *q = tmp; *q; q++)
    {
        if (*q == '/' || *q == '\\')
        {
            char sv = *q;
            *q = '\0';
#if defined(_WIN32)
            if (q > tmp + 2 && !(q[-1] == ':')) /* don't mkdir "C:" */
#else
            if (q > tmp)
#endif
            {
                MINI_STATBUF st;
                if (MINI_STAT(tmp, &st) != 0)
                    MINI_MKDIR(tmp);
            }
            *q = sv;
        }
    }
    MINI_STATBUF st2;
    if (MINI_STAT(tmp, &st2) != 0)
        rc = MINI_MKDIR(tmp);
    free(tmp);
    return rc;
}

static JSValue js_fs_mkdirSync(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "mkdirSync(path[, opts]) expected a string");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    int recursive = 0;
    if (argc > 1 && JS_IsObject(argv[1]))
    {
        JSValue r = JS_GetPropertyStr(ctx, argv[1], "recursive");
        recursive = JS_ToBool(ctx, r);
        JS_FreeValue(ctx, r);
    }
    int rc = recursive ? mkdir_p(path) : MINI_MKDIR(path);
    JS_FreeCString(ctx, path);
    if (rc != 0)
        return JS_ThrowTypeError(ctx, "EEXIST or ENOENT: mkdir failed: %s", strerror(errno));
    return JS_UNDEFINED;
}

static JSValue js_fs_rmdirSync(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "rmdirSync(path) expected a string");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    int rc = MINI_RMDIR(path);
    JS_FreeCString(ctx, path);
    return rc == 0 ? JS_UNDEFINED : JS_ThrowTypeError(ctx, "rmdir failed: %s", strerror(errno));
}

static JSValue js_fs_unlinkSync(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "unlinkSync(path) expected a string");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    int rc = MINI_UNLINK(path);
    JS_FreeCString(ctx, path);
    return rc == 0 ? JS_UNDEFINED : JS_ThrowTypeError(ctx, "unlink failed: %s", strerror(errno));
}

static JSValue js_fs_renameSync(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx, "renameSync(oldPath, newPath) expected two strings");
    const char *oldp = JS_ToCString(ctx, argv[0]);
    const char *newp = JS_ToCString(ctx, argv[1]);
    if (!oldp || !newp)
    {
        JS_FreeCString(ctx, oldp);
        JS_FreeCString(ctx, newp);
        return JS_EXCEPTION;
    }
    int rc = rename(oldp, newp);
    JS_FreeCString(ctx, oldp);
    JS_FreeCString(ctx, newp);
    return rc == 0 ? JS_UNDEFINED : JS_ThrowTypeError(ctx, "rename failed: %s", strerror(errno));
}

static JSValue js_fs_readdirSync(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "readdirSync(path) expected a string");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    JSValue arr = JS_NewArray(ctx);
    int idx = 0;
#if defined(_WIN32)
    {
        size_t pl = strlen(path);
        char *pat = (char *)malloc(pl + 4);
        if (pat)
        {
            snprintf(pat, pl + 4, "%s\\*", path);
            wchar_t *wp = utf8_to_wide(pat);
            WIN32_FIND_DATAW fd;
            HANDLE h = wp ? FindFirstFileW(wp, &fd) : INVALID_HANDLE_VALUE;
            if (h != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                        continue;
                    char *n = wide_to_utf8(fd.cFileName);
                    if (n)
                    {
                        JS_SetPropertyInt64(ctx, arr, (uint32_t)idx, JS_NewString(ctx, n));
                        idx++;
                        free(n);
                    }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            free(wp);
            free(pat);
        }
    }
#else
    {
        DIR *d = opendir(path);
        if (d)
        {
            struct dirent *e;
            while ((e = readdir(d)))
            {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                    continue;
                JS_SetPropertyInt64(ctx, arr, (uint32_t)idx, JS_NewString(ctx, e->d_name));
                idx++;
            }
            closedir(d);
        }
    }
#endif
    JS_FreeCString(ctx, path);
    return arr;
}

static JSValue js_fs_realpathSync(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "realpathSync(path) expected a string");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    JSValue res;
#if defined(_WIN32)
    char abs[MAX_PATH * 2];
    if (_fullpath(abs, path, sizeof(abs)))
        res = JS_NewString(ctx, abs);
    else
        res = JS_ThrowTypeError(ctx, "realpath failed: %s", path);
#else
    char abs[4096];
    if (realpath(path, abs))
        res = JS_NewString(ctx, abs);
    else
        res = JS_ThrowTypeError(ctx, "realpath failed: %s", strerror(errno));
#endif
    JS_FreeCString(ctx, path);
    return res;
}

static JSValue js_fs_copyFileSync(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx, "copyFileSync(src, dest) expected two strings");
    const char *src = JS_ToCString(ctx, argv[0]);
    const char *dst = JS_ToCString(ctx, argv[1]);
    if (!src || !dst)
    {
        JS_FreeCString(ctx, src);
        JS_FreeCString(ctx, dst);
        return JS_EXCEPTION;
    }
    FILE *in = fopen(src, "rb");
    if (!in)
    {
        JSValue e = JS_ThrowTypeError(ctx, "ENOENT: no such file '%s'", src);
        JS_FreeCString(ctx, src);
        JS_FreeCString(ctx, dst);
        return e;
    }
    FILE *out = fopen(dst, "wb");
    if (!out)
    {
        fclose(in);
        JSValue e = JS_ThrowTypeError(ctx, "EACCES: cannot open '%s' for writing", dst);
        JS_FreeCString(ctx, src);
        JS_FreeCString(ctx, dst);
        return e;
    }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    JS_FreeCString(ctx, src);
    JS_FreeCString(ctx, dst);
    return JS_UNDEFINED;
}

static JSValue js_fs_chmodSync(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "chmodSync(path, mode) expected (string, int)");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    int mode = 0;
    JS_ToInt32(ctx, &mode, argv[1]);
    int rc = MINI_CHMOD(path, mode);
    JS_FreeCString(ctx, path);
    return rc == 0 ? JS_UNDEFINED : JS_ThrowTypeError(ctx, "chmod failed: %s", strerror(errno));
}

/* Schedule a callback on the next tick with bound args. Uses the shim's
   __miniDefer helper so args reach the callback (the built-in setTimeout
   does not forward extra arguments). */
static void defer_callback(JSContext *ctx, JSValue cb, int n, JSValueConst *args)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue defer = JS_GetPropertyStr(ctx, global, "__miniDefer");
    if (JS_IsFunction(ctx, defer))
    {
        JSValue arr = JS_NewArray(ctx);
        for (int i = 0; i < n; i++)
            JS_SetPropertyInt64(ctx, arr, (uint32_t)i, JS_DupValue(ctx, args[i]));
        JSValue fa[2];
        fa[0] = JS_DupValue(ctx, cb);
        fa[1] = arr;
        JSValue r = JS_Call(ctx, defer, JS_UNDEFINED, 2, fa);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, fa[0]);
        JS_FreeValue(ctx, arr);
    }
    JS_FreeValue(ctx, defer);
    JS_FreeValue(ctx, global);
}

static JSValue js_fs_readFile(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "readFile(path[, opts], cb)");
    JSValue cb = argc > 1 ? JS_DupValue(ctx, argv[argc - 1]) : JS_UNDEFINED;
    if (!JS_IsFunction(ctx, cb))
    {
        JS_FreeValue(ctx, cb);
        return JS_ThrowTypeError(ctx, "readFile(path[, opts], cb) expected a callback");
    }
    /* synchronous read, then defer callback(err, data) */
    JSValue data = js_fs_readFileSync(ctx, JS_NULL, 1, argv);
    JSValue args[2];
    if (JS_IsException(data))
    {
        JSValue ex = JS_GetException(ctx);
        args[0] = ex;
        args[1] = JS_UNDEFINED;
    }
    else
    {
        args[0] = JS_NULL;
        args[1] = data;
    }
    defer_callback(ctx, cb, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, cb);
    return JS_UNDEFINED;
}

static JSValue js_fs_writeFile(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "writeFile(path, data[, opts], cb)");
    JSValue cb = argc > 2 ? JS_DupValue(ctx, argv[argc - 1]) : JS_UNDEFINED;
    if (!JS_IsFunction(ctx, cb))
    {
        JS_FreeValue(ctx, cb);
        return JS_ThrowTypeError(ctx, "writeFile(path, data[, opts], cb) expected a callback");
    }
    JSValue w = js_fs_writeFileSync(ctx, JS_NULL, 2, argv);
    JSValue args[2];
    if (JS_IsException(w))
    {
        args[0] = JS_GetException(ctx);
        args[1] = JS_UNDEFINED;
    }
    else
    {
        args[0] = JS_NULL;
        args[1] = JS_UNDEFINED;
    }
    JS_FreeValue(ctx, w);
    defer_callback(ctx, cb, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, cb);
    return JS_UNDEFINED;
}

static void install_fs(JSContext *ctx, JSValue mods)
{
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "readFileSync", JS_NewCFunction(ctx, js_fs_readFileSync, "readFileSync", 2));
    JS_SetPropertyStr(ctx, o, "writeFileSync", JS_NewCFunction(ctx, js_fs_writeFileSync, "writeFileSync", 3));
    JS_SetPropertyStr(ctx, o, "appendFileSync", JS_NewCFunction(ctx, js_fs_appendFileSync, "appendFileSync", 3));
    JS_SetPropertyStr(ctx, o, "existsSync", JS_NewCFunction(ctx, js_fs_existsSync, "existsSync", 1));
    JS_SetPropertyStr(ctx, o, "statSync", JS_NewCFunction(ctx, js_fs_statSync, "statSync", 1));
    JS_SetPropertyStr(ctx, o, "mkdirSync", JS_NewCFunction(ctx, js_fs_mkdirSync, "mkdirSync", 2));
    JS_SetPropertyStr(ctx, o, "rmdirSync", JS_NewCFunction(ctx, js_fs_rmdirSync, "rmdirSync", 1));
    JS_SetPropertyStr(ctx, o, "unlinkSync", JS_NewCFunction(ctx, js_fs_unlinkSync, "unlinkSync", 1));
    JS_SetPropertyStr(ctx, o, "renameSync", JS_NewCFunction(ctx, js_fs_renameSync, "renameSync", 2));
    JS_SetPropertyStr(ctx, o, "readdirSync", JS_NewCFunction(ctx, js_fs_readdirSync, "readdirSync", 1));
    JS_SetPropertyStr(ctx, o, "realpathSync", JS_NewCFunction(ctx, js_fs_realpathSync, "realpathSync", 1));
    JS_SetPropertyStr(ctx, o, "copyFileSync", JS_NewCFunction(ctx, js_fs_copyFileSync, "copyFileSync", 2));
    JS_SetPropertyStr(ctx, o, "chmodSync", JS_NewCFunction(ctx, js_fs_chmodSync, "chmodSync", 2));
    JS_SetPropertyStr(ctx, o, "readFile", JS_NewCFunction(ctx, js_fs_readFile, "readFile", 3));
    JS_SetPropertyStr(ctx, o, "writeFile", JS_NewCFunction(ctx, js_fs_writeFile, "writeFile", 4));
    JS_SetPropertyStr(ctx, mods, "fs", o);
}

/* ================================================================== */
/* path module                                                        */
/* ================================================================== */

#if defined(_WIN32)
#  define PATH_SEP    '\\'
#  define PATH_DELIM  ';'
#else
#  define PATH_SEP    '/'
#  define PATH_DELIM  ':'
#endif
#define PATH_IS_SEP(c) ((c) == '/' || (c) == '\\')

static int path_is_abs(const char *p)
{
    if (!p || !*p)
        return 0;
    if (p[0] == '/' || p[0] == '\\')
        return 1;
#if defined(_WIN32)
    {
        char c = (char)p[0];
        if (((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) && p[1] == ':')
            return 1;
    }
#endif
    return 0;
}

/* normalize: collapse . / .., dedupe separators, keep drive/root.
   Drive letters (C:) emerge naturally as a split component; rejoin adds a
   separator after them so "C:\\foo" stays "C:\\foo". */
static char *path_norm(const char *p)
{
    if (!p)
        return NULL;
    size_t pl = strlen(p);
    char *out = (char *)malloc(pl + 2);
    if (!out)
        return NULL;
    char *comps[256];
    int nc = 0;
    const char *tok = p;
    int rooted = (p[0] == '/' || p[0] == '\\');
    for (const char *s = p;; s++)
    {
        if (*s == '\0' || PATH_IS_SEP(*s))
        {
            size_t tl = (size_t)(s - tok);
            if (tl > 0 && nc < 256)
            {
                char *c = (char *)malloc(tl + 1);
                memcpy(c, tok, tl);
                c[tl] = '\0';
                comps[nc++] = c;
            }
            tok = (*s == '\0') ? s : s + 1;
            if (*s == '\0')
                break;
        }
    }
    /* process . and .. */
    char *stack[256];
    int sp = 0;
    for (int i = 0; i < nc; i++)
    {
        if (!strcmp(comps[i], "."))
        {
            free(comps[i]);
            continue;
        }
        if (!strcmp(comps[i], ".."))
        {
            free(comps[i]);
            if (sp > 0 && strcmp(stack[sp - 1], ".."))
            {
                sp--;
                free(stack[sp]);
            }
            else if (!rooted)
            {
                stack[sp++] = strdup("..");
            }
            continue;
        }
        stack[sp++] = comps[i];
    }
    size_t pos = 0;
    if (rooted)
        out[pos++] = PATH_SEP;
    for (int i = 0; i < sp; i++)
    {
        if (i > 0 && pos > 0 && !PATH_IS_SEP(out[pos - 1]))
            out[pos++] = PATH_SEP;
        size_t cl = strlen(stack[i]);
        memcpy(out + pos, stack[i], cl);
        pos += cl;
        free(stack[i]);
    }
    if (pos == 0)
        out[pos++] = '.';
    out[pos] = '\0';
    return out;
}

static char *path_join_str(int n, JSContext *ctx, JSValueConst *argv)
{
    char *buf = strdup("");
    for (int i = 0; i < n; i++)
    {
        if (!JS_IsString(argv[i]))
            continue;
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s)
            continue;
        size_t bl = strlen(buf), sl = strlen(s);
        int addsep = (bl > 0 && buf[bl - 1] != '/' && buf[bl - 1] != '\\' &&
                      s[0] != '/' && s[0] != '\\');
        char *nb = (char *)malloc(bl + sl + 2);
        memcpy(nb, buf, bl);
        size_t pos = bl;
        if (addsep)
            nb[pos++] = PATH_SEP;
        memcpy(nb + pos, s, sl);
        pos += sl;
        nb[pos] = '\0';
        free(buf);
        buf = nb;
        JS_FreeCString(ctx, s);
    }
    char *norm = path_norm(buf);
    free(buf);
    return norm;
}

static JSValue js_path_join(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    char *r = path_join_str(argc, ctx, argv);
    JSValue v = r ? JS_NewString(ctx, r) : JS_NewString(ctx, ".");
    free(r);
    return v;
}

static JSValue js_path_normalize(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_NewString(ctx, ".");
    const char *p = JS_ToCString(ctx, argv[0]);
    char *r = p ? path_norm(p) : NULL;
    JS_FreeCString(ctx, p);
    JSValue v = r ? JS_NewString(ctx, r) : JS_NewString(ctx, ".");
    free(r);
    return v;
}

static JSValue js_path_resolve(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    /* resolve to absolute: rightmost absolute arg wins, prepend cwd */
    char base[4096];
    GETCWD(base, sizeof(base) - 1);
    char *acc = strdup(base);
    for (int i = 0; i < argc; i++)
    {
        if (!JS_IsString(argv[i]))
            continue;
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s)
            continue;
        if (path_is_abs(s))
        {
            free(acc);
            acc = strdup(s);
        }
        else
        {
            size_t bl = strlen(acc), sl = strlen(s);
            char *nb = (char *)malloc(bl + sl + 2);
            memcpy(nb, acc, bl);
            nb[bl] = PATH_SEP;
            memcpy(nb + bl + 1, s, sl);
            nb[bl + 1 + sl] = '\0';
            free(acc);
            acc = nb;
        }
        JS_FreeCString(ctx, s);
    }
    char *norm = path_norm(acc);
    free(acc);
    if (!path_is_abs(norm ? norm : ""))
    {
        /* prefix cwd if still relative */
        char *full = (char *)malloc(strlen(base) + strlen(norm ? norm : ".") + 2);
        sprintf(full, "%s%c%s", base, PATH_SEP, norm ? norm : ".");
        free(norm);
        norm = path_norm(full);
        free(full);
    }
    JSValue v = norm ? JS_NewString(ctx, norm) : JS_NewString(ctx, base);
    free(norm);
    return v;
}

static JSValue js_path_basename(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_NewString(ctx, "");
    const char *p = JS_ToCString(ctx, argv[0]);
    if (!p)
        return JS_NewString(ctx, "");
    /* trim trailing separators */
    size_t l = strlen(p);
    while (l > 0 && PATH_IS_SEP(p[l - 1]))
        l--;
    size_t end = l;
    while (l > 0 && !PATH_IS_SEP(p[l - 1]))
        l--;
    size_t blen = end - l;
    char *base = (char *)malloc(blen + 1);
    memcpy(base, p + l, blen);
    base[blen] = '\0';
    JS_FreeCString(ctx, p);
    /* optional ext strip */
    if (argc > 1 && JS_IsString(argv[1]))
    {
        const char *ext = JS_ToCString(ctx, argv[1]);
        if (ext)
        {
            size_t el = strlen(ext), bl = strlen(base);
            if (el && el <= bl && !strcmp(base + bl - el, ext))
                base[bl - el] = '\0';
            JS_FreeCString(ctx, ext);
        }
    }
    JSValue v = JS_NewString(ctx, base);
    free(base);
    return v;
}

static JSValue js_path_dirname(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_NewString(ctx, ".");
    const char *p = JS_ToCString(ctx, argv[0]);
    if (!p)
        return JS_NewString(ctx, ".");
    size_t l = strlen(p);
    while (l > 0 && PATH_IS_SEP(p[l - 1]))
        l--;
    while (l > 0 && !PATH_IS_SEP(p[l - 1]))
        l--;
    while (l > 1 && PATH_IS_SEP(p[l - 1]))
        l--;
    JSValue v;
    if (l == 0)
        v = JS_NewString(ctx, ".");
    else
        v = JS_NewStringLen(ctx, p, l);
    JS_FreeCString(ctx, p);
    return v;
}

static JSValue js_path_extname(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_NewString(ctx, "");
    const char *p = JS_ToCString(ctx, argv[0]);
    if (!p)
        return JS_NewString(ctx, "");
    size_t l = strlen(p), base = l;
    while (base > 0 && !PATH_IS_SEP(p[base - 1]))
        base--;
    size_t dot = l;
    while (dot > base && p[dot - 1] != '.')
        dot--;
    JSValue v = (dot > base && dot < l) ? JS_NewStringLen(ctx, p + dot - 1, l - dot + 1)
                                        : JS_NewString(ctx, "");
    JS_FreeCString(ctx, p);
    return v;
}

static JSValue js_path_isAbsolute(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_FALSE;
    const char *p = JS_ToCString(ctx, argv[0]);
    int r = p ? path_is_abs(p) : 0;
    JS_FreeCString(ctx, p);
    return JS_NewBool(ctx, r);
}

static JSValue js_path_relative(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1]))
        return JS_NewString(ctx, "");
    const char *from = JS_ToCString(ctx, argv[0]);
    const char *to = JS_ToCString(ctx, argv[1]);
    if (!from || !to)
    {
        JS_FreeCString(ctx, from);
        JS_FreeCString(ctx, to);
        return JS_NewString(ctx, "");
    }
    char *fn = path_norm(from);
    char *tn = path_norm(to);
    /* shared prefix */
    size_t i = 0;
    while (fn[i] && tn[i] && fn[i] == tn[i])
        i++;
    /* back up to separator */
    size_t cp = i;
    while (cp > 0 && !PATH_IS_SEP(fn[cp - 1]))
        cp--;
    /* count remaining segments in `from` */
    int ups = 0;
    for (size_t k = cp; fn[k]; k++)
        if (PATH_IS_SEP(fn[k]) && fn[k + 1])
            ups++;
    if (fn[cp])
        ups++;
    char *out = (char *)malloc(strlen(tn) + ups * 3 + 2);
    size_t pos = 0;
    for (int u = 0; u < ups; u++)
    {
        out[pos++] = '.';
        out[pos++] = '.';
        out[pos++] = PATH_SEP;
    }
    strcpy(out + pos, tn + cp);
    if (pos == 0 && out[0] == '\0')
        out[0] = '.', out[1] = '\0';
    JSValue v = JS_NewString(ctx, out);
    free(out);
    free(fn);
    free(tn);
    JS_FreeCString(ctx, from);
    JS_FreeCString(ctx, to);
    return v;
}

static JSValue js_path_parse(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_NewObject(ctx);
    const char *p = JS_ToCString(ctx, argv[0]);
    if (!p)
        return JS_NewObject(ctx);
    JSValue o = JS_NewObject(ctx);
    /* root */
    size_t ri = 0;
#if defined(_WIN32)
    if (p[0] && p[1] == ':')
    {
        JS_SetPropertyStr(ctx, o, "root", JS_NewStringLen(ctx, p, 2));
        ri = 2;
        if (PATH_IS_SEP(p[2]))
            ri = 3;
    }
    else
#endif
    if (PATH_IS_SEP(p[0]))
    {
        JS_SetPropertyStr(ctx, o, "root", JS_NewStringLen(ctx, p, 1));
        ri = 1;
    }
    else
        JS_SetPropertyStr(ctx, o, "root", JS_NewString(ctx, ""));
    /* base + ext + name via basename/dirname reuse */
    size_t l = strlen(p), base = l;
    while (base > ri && !PATH_IS_SEP(p[base - 1]))
        base--;
    size_t bend = l;
    while (bend > base && PATH_IS_SEP(p[bend - 1]))
        bend--;
    JS_SetPropertyStr(ctx, o, "base", JS_NewStringLen(ctx, p + base, bend - base));
    /* ext */
    size_t dot = bend;
    while (dot > base + 1 && p[dot - 1] != '.')
        dot--;
    if (dot > base && p[dot - 1] == '.')
    {
        JS_SetPropertyStr(ctx, o, "ext", JS_NewStringLen(ctx, p + dot - 1, bend - dot + 1));
        JS_SetPropertyStr(ctx, o, "name", JS_NewStringLen(ctx, p + base, dot - 1 - base));
    }
    else
    {
        JS_SetPropertyStr(ctx, o, "ext", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, o, "name", JS_NewStringLen(ctx, p + base, bend - base));
    }
    JS_SetPropertyStr(ctx, o, "dir", JS_NewStringLen(ctx, p, base > 0 ? base - 1 : 0));
    JS_FreeCString(ctx, p);
    return o;
}

static void install_path(JSContext *ctx, JSValue mods)
{
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "join", JS_NewCFunction(ctx, js_path_join, "join", 1));
    JS_SetPropertyStr(ctx, o, "normalize", JS_NewCFunction(ctx, js_path_normalize, "normalize", 1));
    JS_SetPropertyStr(ctx, o, "resolve", JS_NewCFunction(ctx, js_path_resolve, "resolve", 1));
    JS_SetPropertyStr(ctx, o, "basename", JS_NewCFunction(ctx, js_path_basename, "basename", 2));
    JS_SetPropertyStr(ctx, o, "dirname", JS_NewCFunction(ctx, js_path_dirname, "dirname", 1));
    JS_SetPropertyStr(ctx, o, "extname", JS_NewCFunction(ctx, js_path_extname, "extname", 1));
    JS_SetPropertyStr(ctx, o, "relative", JS_NewCFunction(ctx, js_path_relative, "relative", 2));
    JS_SetPropertyStr(ctx, o, "isAbsolute", JS_NewCFunction(ctx, js_path_isAbsolute, "isAbsolute", 1));
    JS_SetPropertyStr(ctx, o, "parse", JS_NewCFunction(ctx, js_path_parse, "parse", 1));
    { char _s[2] = {PATH_SEP, 0}; char _d[2] = {PATH_DELIM, 0};
      JS_SetPropertyStr(ctx, o, "sep", JS_NewString(ctx, _s));
      JS_SetPropertyStr(ctx, o, "delimiter", JS_NewString(ctx, _d)); }
#if defined(_WIN32)
    JS_SetPropertyStr(ctx, o, "win32", JS_DupValue(ctx, o));
#else
    JS_SetPropertyStr(ctx, o, "posix", JS_DupValue(ctx, o));
#endif
    JS_SetPropertyStr(ctx, mods, "path", o);
}

/* ================================================================== */
/* child_process module                                              */
/* ================================================================== */

#if defined(_WIN32)
#  define MINI_POPEN  _popen
#  define MINI_PCLOSE _pclose
#else
#  define MINI_POPEN  popen
#  define MINI_PCLOSE pclose
#endif

static JSClassID g_cp_cid = 0;

static void cp_close_handles(JsChildProc *c)
{
    if (!c)
        return;
#if defined(_WIN32)
    if (c->stdout_pipe) { CloseHandle((HANDLE)c->stdout_pipe); c->stdout_pipe = NULL; }
    if (c->stderr_pipe) { CloseHandle((HANDLE)c->stderr_pipe); c->stderr_pipe = NULL; }
    if (c->proc) { CloseHandle((HANDLE)c->proc); c->proc = NULL; }
#else
    if (c->stdout_pipe) { close((int)(intptr_t)c->stdout_pipe); c->stdout_pipe = NULL; }
    if (c->stderr_pipe) { close((int)(intptr_t)c->stderr_pipe); c->stderr_pipe = NULL; }
    c->proc = NULL;
#endif
}

static void cp_free(JSRuntime *rt, JsChildProc *c)
{
    if (!c)
        return;
    cp_close_handles(c);
    JS_FreeValueRT(rt, c->cb);
    JS_FreeValueRT(rt, c->onclose);
    JS_FreeValueRT(rt, c->onexit);
    JS_FreeValueRT(rt, c->stdout_listeners);
    JS_FreeValueRT(rt, c->stderr_listeners);
    free(c->stdout_buf);
    free(c->stderr_buf);
    free(c);
}

static void js_cp_finalizer(JSRuntime *rt, JSValue val)
{
    JsChildProc *c = (JsChildProc *)JS_GetOpaque(val, g_cp_cid);
    if (!c)
        return;
    c->ref--;
    if (c->ref <= 0)
        cp_free(rt, c);
}

/* Spawn a child running `cmdline`. On Win, cmdline must already include
   "cmd.exe /c ..."; on POSIX, cmdline is run through /bin/sh -c. Returns
   the pid or -1 on failure. */
static int cp_spawn(const char *cmdline, JsChildProc *c)
{
    if (!cmdline || !c)
        return -1;
#if defined(_WIN32)
    HANDLE outR, outW, errR, errW;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&outR, &outW, &sa, 0))
        return -1;
    if (!CreatePipe(&errR, &errW, &sa, 0))
    {
        CloseHandle(outR);
        CloseHandle(outW);
        return -1;
    }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outW;
    si.hStdError = errW;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    wchar_t *wcmd = utf8_to_wide(cmdline);
    BOOL ok = wcmd ? CreateProcessW(NULL, wcmd, NULL, NULL, TRUE,
                                    CREATE_NO_WINDOW, NULL, NULL, &si, &pi) : FALSE;
    free(wcmd);
    CloseHandle(outW);
    CloseHandle(errW);
    if (!ok)
    {
        CloseHandle(outR);
        CloseHandle(errR);
        return -1;
    }
    CloseHandle(pi.hThread);
    c->proc = pi.hProcess;
    c->stdout_pipe = outR;
    c->stderr_pipe = errR;
    return (int)pi.dwProcessId;
#else
    int outp[2], errp[2];
    if (pipe(outp) != 0)
        return -1;
    if (pipe(errp) != 0)
    {
        close(outp[0]);
        close(outp[1]);
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0)
    {
        dup2(outp[1], STDOUT_FILENO);
        dup2(errp[1], STDERR_FILENO);
        close(outp[0]);
        close(outp[1]);
        close(errp[0]);
        close(errp[1]);
        execl("/bin/sh", "sh", "-c", cmdline, (char *)NULL);
        _exit(127);
    }
    close(outp[1]);
    close(errp[1]);
    if (pid < 0)
    {
        close(outp[0]);
        close(errp[0]);
        return -1;
    }
    c->proc = (void *)(intptr_t)pid;
    c->stdout_pipe = (void *)(intptr_t)outp[0];
    c->stderr_pipe = (void *)(intptr_t)errp[0];
    fcntl(outp[0], F_SETFL, O_NONBLOCK);
    fcntl(errp[0], F_SETFL, O_NONBLOCK);
    return (int)pid;
#endif
}

/* Non-blocking drain of one child pipe into its buffer. */
static void cp_drain(JsChildProc *c, int which)
{
    if (!c)
        return;
    char **pbuf = which == 0 ? &c->stdout_buf : &c->stderr_buf;
    size_t *plen = which == 0 ? &c->stdout_len : &c->stderr_len;
    size_t *pcap = which == 0 ? &c->stdout_cap : &c->stderr_cap;
#if defined(_WIN32)
    HANDLE pipe = (HANDLE)(which == 0 ? c->stdout_pipe : c->stderr_pipe);
    if (!pipe)
        return;
    for (;;)
    {
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL) || avail == 0)
            break;
        char tmp[4096];
        DWORD toread = avail < (DWORD)sizeof(tmp) ? avail : (DWORD)sizeof(tmp);
        DWORD rd = 0;
        if (!ReadFile(pipe, tmp, toread, &rd, NULL) || rd == 0)
            break;
        if (*plen + rd + 1 > *pcap)
        {
            *pcap = (*plen + rd + 1) * 2;
            *pbuf = (char *)realloc(*pbuf, *pcap);
            if (!*pbuf) { *plen = 0; *pcap = 0; return; }
        }
        memcpy(*pbuf + *plen, tmp, rd);
        *plen += rd;
    }
#else
    int fd = (int)(intptr_t)(which == 0 ? c->stdout_pipe : c->stderr_pipe);
    if (fd < 0)
        return;
    for (;;)
    {
        char tmp[4096];
        ssize_t rd = read(fd, tmp, sizeof(tmp));
        if (rd <= 0)
            break;
        if (*plen + (size_t)rd + 1 > *pcap)
        {
            *pcap = (*plen + (size_t)rd + 1) * 2;
            *pbuf = (char *)realloc(*pbuf, *pcap);
            if (!*pbuf) { *plen = 0; *pcap = 0; return; }
        }
        memcpy(*pbuf + *plen, tmp, (size_t)rd);
        *plen += (size_t)rd;
    }
#endif
}

static int cp_poll_exit(JsChildProc *c)
{
    if (!c || c->exited)
        return 1;
#if defined(_WIN32)
    DWORD code = 0;
    if (GetExitCodeProcess((HANDLE)c->proc, &code))
    {
        if (code != STILL_ACTIVE)
        {
            c->exited = 1;
            c->exit_code = (int)code;
            return 1;
        }
    }
    return 0;
#else
    int st = 0;
    pid_t r = waitpid((pid_t)(intptr_t)c->proc, &st, WNOHANG);
    if (r > 0)
    {
        c->exited = 1;
        c->exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        return 1;
    }
    return 0;
#endif
}

static int cp_kill(JsChildProc *c)
{
    if (!c || c->exited)
        return -1;
    c->killed = 1;
#if defined(_WIN32)
    return TerminateProcess((HANDLE)c->proc, 1) ? 0 : -1;
#else
    return kill((pid_t)(intptr_t)c->proc, SIGTERM);
#endif
}

/* Deliver newly-buffered bytes to 'data' listeners (spawn mode). */
static void cp_fire_data(JSContext *ctx, JsChildProc *c, int which)
{
    JSValue listeners = which == 0 ? c->stdout_listeners : c->stderr_listeners;
    char *buf = which == 0 ? c->stdout_buf : c->stderr_buf;
    size_t len = which == 0 ? c->stdout_len : c->stderr_len;
    size_t *fired = which == 0 ? &c->stdout_fired : &c->stderr_fired;
    if (!JS_IsArray(listeners) || len <= *fired)
        return;
    int ln = 0;
    JSValue larr = JS_DupValue(ctx, listeners);
    if (JS_ToInt32(ctx, &ln, JS_GetPropertyStr(ctx, larr, "length")))
        ln = 0;
    size_t chunklen = len - *fired;
    JSValue chunk = JS_NewStringLen(ctx, buf ? buf + *fired : "", chunklen);
    for (int i = 0; i < ln; i++)
    {
        JSValue cb = JS_GetPropertyInt64(ctx, larr, (uint32_t)i);
        if (JS_IsFunction(ctx, cb))
        {
            JSValue dup = JS_DupValue(ctx, chunk);
            JSValue r = JS_Call(ctx, cb, JS_UNDEFINED, 1, &dup);
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, dup);
        }
        JS_FreeValue(ctx, cb);
    }
    JS_FreeValue(ctx, chunk);
    JS_FreeValue(ctx, larr);
    *fired = len;
}

static JSValue js_cp_execSync(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "execSync(cmd[, opts]) expected a string command");
    const char *cmd = JS_ToCString(ctx, argv[0]);
    if (!cmd)
        return JS_EXCEPTION;
    FILE *fp = MINI_POPEN(cmd, "r");
    JS_FreeCString(ctx, cmd);
    if (!fp)
        return JS_ThrowTypeError(ctx, "execSync: popen failed");
    char *buf = NULL;
    size_t len = 0, cap = 0;
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), fp)) > 0)
    {
        if (len + n + 1 > cap)
        {
            cap = (len + n + 1) * 2;
            buf = (char *)realloc(buf, cap);
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    MINI_PCLOSE(fp);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        len--;
    JSValue res = buf ? JS_NewStringLen(ctx, buf, len) : JS_NewString(ctx, "");
    free(buf);
    return res;
}

static JSValue js_cp_exec(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "exec(cmd[, opts], cb) expected a string command");
    const char *cmd = JS_ToCString(ctx, argv[0]);
    if (!cmd)
        return JS_EXCEPTION;
    JSValue cb = (argc > 1 && JS_IsFunction(ctx, argv[argc - 1]))
                    ? JS_DupValue(ctx, argv[argc - 1])
                    : JS_UNDEFINED;
    if (!JS_IsFunction(ctx, cb))
    {
        JS_FreeCString(ctx, cmd);
        JS_FreeValue(ctx, cb);
        return JS_ThrowTypeError(ctx, "exec(cmd, cb) expected a callback");
    }
#if defined(_WIN32)
    char *cmdline = (char *)malloc(strlen(cmd) + 16);
    sprintf(cmdline, "cmd.exe /c %s", cmd);
#else
    char *cmdline = strdup(cmd);
#endif
    JS_FreeCString(ctx, cmd);
    JsChildProc *c = (JsChildProc *)calloc(1, sizeof(*c));
    if (!c)
    {
        free(cmdline);
        JS_FreeValue(ctx, cb);
        return JS_ThrowTypeError(ctx, "OOM");
    }
    c->cb = cb;
    c->onclose = JS_UNDEFINED;
    c->onexit = JS_UNDEFINED;
    c->stdout_listeners = JS_NewArray(ctx);
    c->stderr_listeners = JS_NewArray(ctx);
    c->ref = 1; /* children array only; no JS wrapper for exec */
    int pid = cp_spawn(cmdline, c);
    free(cmdline);
    if (pid < 0)
    {
        JSRuntime *rt = JS_GetRuntime(ctx);
        c->ref = 0;
        cp_free(rt, c);
        return JS_ThrowTypeError(ctx, "exec: spawn failed");
    }
    mini_bridge_add_child(nb_of(ctx), c);
    return JS_UNDEFINED;
}

static JSValue js_cp_stream_on(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv, int magic)
{
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "on(event, listener) expects 2 args");
    const char *ev = JS_ToCString(ctx, argv[0]);
    int is_data = (ev && !strcmp(ev, "data"));
    JS_FreeCString(ctx, ev);
    JSValue owner = JS_GetPropertyStr(ctx, tv, "_owner");
    JsChildProc *c = (JsChildProc *)JS_GetOpaque2(ctx, owner, g_cp_cid);
    JS_FreeValue(ctx, owner);
    if (!c || !is_data)
        return JS_DupValue(ctx, tv);
    JSValue arr = magic == 0 ? c->stdout_listeners : c->stderr_listeners;
    int ln = 0;
    JSValue a = JS_DupValue(ctx, arr);
    JS_ToInt32(ctx, &ln, JS_GetPropertyStr(ctx, a, "length"));
    JS_SetPropertyInt64(ctx, a, (uint32_t)ln, JS_DupValue(ctx, argv[1]));
    JS_FreeValue(ctx, a);
    return JS_DupValue(ctx, tv);
}
static JSValue js_cp_stdout_on(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    return js_cp_stream_on(ctx, tv, argc, argv, 0);
}
static JSValue js_cp_stderr_on(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    return js_cp_stream_on(ctx, tv, argc, argv, 1);
}

static JSValue js_cp_obj_on(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "on(event, listener) expects 2 args");
    JsChildProc *c = (JsChildProc *)JS_GetOpaque2(ctx, tv, g_cp_cid);
    if (!c)
        return JS_DupValue(ctx, tv);
    const char *ev = JS_ToCString(ctx, argv[0]);
    if (ev)
    {
        if (!strcmp(ev, "close"))
        {
            JS_FreeValue(ctx, c->onclose);
            c->onclose = JS_DupValue(ctx, argv[1]);
        }
        else if (!strcmp(ev, "exit"))
        {
            JS_FreeValue(ctx, c->onexit);
            c->onexit = JS_DupValue(ctx, argv[1]);
        }
        JS_FreeCString(ctx, ev);
    }
    return JS_DupValue(ctx, tv);
}

static JSValue js_cp_kill(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JsChildProc *c = (JsChildProc *)JS_GetOpaque2(ctx, tv, g_cp_cid);
    if (!c)
        return JS_FALSE;
    return cp_kill(c) == 0 ? JS_TRUE : JS_FALSE;
}

static JSValue js_cp_spawn(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "spawn(cmd[, args][, opts]) expected a string command");
    const char *cmd = JS_ToCString(ctx, argv[0]);
    if (!cmd)
        return JS_EXCEPTION;
    char *cmdline = strdup(cmd);
    if (argc > 1 && JS_IsArray(argv[1]))
    {
        int ln = 0;
        JS_ToInt32(ctx, &ln, JS_GetPropertyStr(ctx, argv[1], "length"));
        for (int i = 0; i < ln; i++)
        {
            JSValue a = JS_GetPropertyInt64(ctx, argv[1], (uint32_t)i);
            if (JS_IsString(a))
            {
                const char *as = JS_ToCString(ctx, a);
                if (as)
                {
                    char *nb = (char *)malloc(strlen(cmdline) + strlen(as) + 2);
                    sprintf(nb, "%s %s", cmdline, as);
                    free(cmdline);
                    cmdline = nb;
                    JS_FreeCString(ctx, as);
                }
            }
            JS_FreeValue(ctx, a);
        }
    }
    JS_FreeCString(ctx, cmd);
#if defined(_WIN32)
    {
        char *full = (char *)malloc(strlen(cmdline) + 16);
        sprintf(full, "cmd.exe /c %s", cmdline);
        free(cmdline);
        cmdline = full;
    }
#endif
    JsChildProc *c = (JsChildProc *)calloc(1, sizeof(*c));
    if (!c)
    {
        free(cmdline);
        return JS_ThrowTypeError(ctx, "OOM");
    }
    c->cb = JS_UNDEFINED;
    c->onclose = JS_UNDEFINED;
    c->onexit = JS_UNDEFINED;
    c->stdout_listeners = JS_NewArray(ctx);
    c->stderr_listeners = JS_NewArray(ctx);
    int pid = cp_spawn(cmdline, c);
    free(cmdline);
    if (pid < 0)
    {
        cp_free(JS_GetRuntime(ctx), c);
        return JS_ThrowTypeError(ctx, "spawn: failed to launch");
    }
    JSValue obj = JS_NewObjectClass(ctx, g_cp_cid);
    JS_SetOpaque(obj, c);
    c->ref = 2; /* children array + JS wrapper */
    JS_SetPropertyStr(ctx, obj, "pid", JS_NewInt32(ctx, pid));
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "_owner", JS_DupValue(ctx, obj));
    JS_SetPropertyStr(ctx, out, "on", JS_NewCFunction(ctx, js_cp_stdout_on, "on", 2));
    JS_SetPropertyStr(ctx, obj, "stdout", out);
    JSValue err = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, err, "_owner", JS_DupValue(ctx, obj));
    JS_SetPropertyStr(ctx, err, "on", JS_NewCFunction(ctx, js_cp_stderr_on, "on", 2));
    JS_SetPropertyStr(ctx, obj, "stderr", err);
    JS_SetPropertyStr(ctx, obj, "on", JS_NewCFunction(ctx, js_cp_obj_on, "on", 2));
    JS_SetPropertyStr(ctx, obj, "kill", JS_NewCFunction(ctx, js_cp_kill, "kill", 0));
    mini_bridge_add_child(nb_of(ctx), c);
    return obj;
}

static JSValue js_cp_spawnSync(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "spawnSync(cmd[, args][, opts]) expected a string command");
    const char *cmd = JS_ToCString(ctx, argv[0]);
    if (!cmd)
        return JS_EXCEPTION;
    char *cmdline = strdup(cmd);
    if (argc > 1 && JS_IsArray(argv[1]))
    {
        int ln = 0;
        JS_ToInt32(ctx, &ln, JS_GetPropertyStr(ctx, argv[1], "length"));
        for (int i = 0; i < ln; i++)
        {
            JSValue a = JS_GetPropertyInt64(ctx, argv[1], (uint32_t)i);
            if (JS_IsString(a))
            {
                const char *as = JS_ToCString(ctx, a);
                if (as)
                {
                    char *nb = (char *)malloc(strlen(cmdline) + strlen(as) + 2);
                    sprintf(nb, "%s %s", cmdline, as);
                    free(cmdline);
                    cmdline = nb;
                    JS_FreeCString(ctx, as);
                }
            }
            JS_FreeValue(ctx, a);
        }
    }
    JS_FreeCString(ctx, cmd);
    FILE *fp = MINI_POPEN(cmdline, "r");
    free(cmdline);
    JSValue res = JS_NewObject(ctx);
    if (!fp)
    {
        JS_SetPropertyStr(ctx, res, "status", JS_NULL);
        JS_SetPropertyStr(ctx, res, "error", JS_NewString(ctx, "spawn failed"));
        return res;
    }
    char *buf = NULL;
    size_t len = 0, cap = 0;
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), fp)) > 0)
    {
        if (len + n + 1 > cap)
        {
            cap = (len + n + 1) * 2;
            buf = (char *)realloc(buf, cap);
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    MINI_PCLOSE(fp);
    JS_SetPropertyStr(ctx, res, "stdout", buf ? JS_NewStringLen(ctx, buf, len) : JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, res, "stderr", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, res, "status", JS_NewInt32(ctx, 0));
    free(buf);
    return res;
}

static void install_child_process(JSContext *ctx, JSValue mods)
{
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "execSync", JS_NewCFunction(ctx, js_cp_execSync, "execSync", 2));
    JS_SetPropertyStr(ctx, o, "exec", JS_NewCFunction(ctx, js_cp_exec, "exec", 3));
    JS_SetPropertyStr(ctx, o, "spawn", JS_NewCFunction(ctx, js_cp_spawn, "spawn", 3));
    JS_SetPropertyStr(ctx, o, "spawnSync", JS_NewCFunction(ctx, js_cp_spawnSync, "spawnSync", 3));
    JS_SetPropertyStr(ctx, mods, "child_process", o);
}

/* ================================================================== */
/* electron namespace                                                */
/* ================================================================== */

static char *g_electron_name = NULL;

static JSValue js_app_getName(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
    return JS_NewString(ctx, g_electron_name ? g_electron_name : "TinyFramework");
}
static JSValue js_app_setName(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "setName(s) expected a string");
    free(g_electron_name);
    const char *s = JS_ToCString(ctx, argv[0]);
    g_electron_name = s ? strdup(s) : NULL;
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}
static JSValue js_app_getVersion(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
    return JS_NewString(ctx, "1.0.0");
}

static const char *app_path_home(void)
{
    const char *h = getenv("USERPROFILE");
#if !defined(_WIN32)
    if (!h) h = getenv("HOME");
    if (!h) { struct passwd *pw = getpwuid(getuid()); if (pw) h = pw->pw_dir; }
#endif
    return h ? h : ".";
}

static JSValue js_app_getPath(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "getPath(name) expected a string");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    static char buf[1100];
    const char *r = NULL;
    const char *appd = getenv("APPDATA");
    if (!appd)
        appd = app_path_home();
    const char *nm = g_electron_name ? g_electron_name : "TinyFramework";
    if (!strcmp(name, "temp"))
    {
#if defined(_WIN32)
        r = getenv("TEMP");
        if (!r) r = getenv("TMP");
#else
        r = getenv("TMPDIR");
#endif
        if (!r) r = "/tmp";
    }
    else if (!strcmp(name, "home"))
        r = app_path_home();
    else if (!strcmp(name, "appData"))
        r = appd;
    else if (!strcmp(name, "userData"))
    {
        snprintf(buf, sizeof(buf), "%s/%s", appd, nm);
        r = buf;
    }
    else if (!strcmp(name, "logs"))
    {
        snprintf(buf, sizeof(buf), "%s/%s/logs", appd, nm);
        r = buf;
    }
    else if (!strcmp(name, "desktop"))
    {
        snprintf(buf, sizeof(buf), "%s/Desktop", app_path_home());
        r = buf;
    }
    else
    {
        JSValue e = JS_ThrowTypeError(ctx, "getPath: unknown name '%s'", name);
        JS_FreeCString(ctx, name);
        return e;
    }
    JS_FreeCString(ctx, name);
    return JS_NewString(ctx, r);
}

static JSValue js_app_request_quit(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
    struct MiniBridge *b = nb_of(ctx);
    struct MiniRenderer *r = mini_bridge_renderer(b);
    if (r && r->gpu.window_handle)
        glfwSetWindowShouldClose((GLFWwindow *)r->gpu.window_handle, GLFW_TRUE);
    return JS_UNDEFINED;
}

static JSValue js_app_whenReady(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue Promise = JS_GetPropertyStr(ctx, global, "Promise");
    JSValue resolve = JS_GetPropertyStr(ctx, Promise, "resolve");
    JSValue p = JS_Call(ctx, resolve, Promise, 0, NULL);
    JS_FreeValue(ctx, resolve);
    JS_FreeValue(ctx, Promise);
    JS_FreeValue(ctx, global);
    return p;
}

static JSValue js_app_on(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "on(event, cb) expects 2 args");
    const char *ev = JS_ToCString(ctx, argv[0]);
    if (ev && !strcmp(ev, "ready"))
        defer_callback(ctx, argv[1], 0, NULL);
    JS_FreeCString(ctx, ev);
    return JS_UNDEFINED;
}

/* shell: open a URL/path with the system default handler. */
static void shell_open(const char *url)
{
#if defined(_WIN32)
    wchar_t *w = utf8_to_wide(url);
    if (w)
    {
        ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
        free(w);
    }
#else
    pid_t pid = fork();
    if (pid == 0)
    {
        execlp("xdg-open", "xdg-open", url, (char *)NULL);
#  if defined(__APPLE__)
        execlp("open", "open", url, (char *)NULL);
#  endif
        _exit(127);
    }
#endif
}

static JSValue js_shell_openExternal(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "openExternal(url) expected a string");
    const char *url = JS_ToCString(ctx, argv[0]);
    if (url)
    {
        shell_open(url);
        JS_FreeCString(ctx, url);
    }
    return JS_UNDEFINED;
}

static JSValue js_shell_openPath(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    return js_shell_openExternal(ctx, tv, argc, argv); /* same impl */
}

static JSValue js_shell_beep(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)ctx; (void)tv; (void)argc; (void)argv;
#if defined(_WIN32)
    MessageBeep(MB_OK);
#else
    fwrite("\a", 1, 1, stderr);
    fflush(stderr);
#endif
    return JS_UNDEFINED;
}

static JSValue js_shell_showItemInFolder(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "showItemInFolder(path) expected a string");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (path)
    {
#if defined(_WIN32)
        /* "explorer.exe /select,"<path>" */
        size_t l = strlen(path) + 64;
        char *cmd = (char *)malloc(l);
        snprintf(cmd, l, "explorer.exe /select,\"%s\"", path);
        wchar_t *w = utf8_to_wide(cmd);
        if (w) { ShellExecuteW(NULL, L"open", L"explorer.exe", w, NULL, SW_SHOWNORMAL); free(w); }
        free(cmd);
#else
        shell_open(path); /* best-effort on POSIX */
#endif
        JS_FreeCString(ctx, path);
    }
    return JS_UNDEFINED;
}

static JSValue js_shell_moveItemToTrash(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "moveItemToTrash(path) expected a string");
    const char *path = JS_ToCString(ctx, argv[0]);
    int ok = 0;
    if (path)
    {
#if defined(_WIN32)
        wchar_t *w = utf8_to_wide(path);
        if (w)
        {
            /* double-NUL terminated wide list for SHFileOperation */
            size_t l = wcslen(w) + 2;
            wchar_t *buf = (wchar_t *)calloc(l, sizeof(wchar_t));
            memcpy(buf, w, wcslen(w) * sizeof(wchar_t));
            SHFILEOPSTRUCTW op = {0};
            op.wFunc = FO_DELETE;
            op.pFrom = buf;
            op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
            ok = SHFileOperationW(&op) == 0;
            free(buf);
            free(w);
        }
#else
        /* best-effort: remove the file (no trash on headless POSIX). */
        ok = unlink(path) == 0;
#endif
        JS_FreeCString(ctx, path);
    }
    return JS_NewBool(ctx, ok);
}

/* clipboard (GLFW clipboard, plain text only). */
static JSValue js_clipboard_readText(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
    struct MiniBridge *b = nb_of(ctx);
    struct MiniRenderer *r = mini_bridge_renderer(b);
    if (!r || !r->gpu.window_handle)
        return JS_NewString(ctx, "");
    const char *s = glfwGetClipboardString((GLFWwindow *)r->gpu.window_handle);
    return JS_NewString(ctx, s ? s : "");
}
static JSValue js_clipboard_writeText(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1)
        return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    struct MiniBridge *b = nb_of(ctx);
    struct MiniRenderer *r = mini_bridge_renderer(b);
    if (r && r->gpu.window_handle)
        glfwSetClipboardString((GLFWwindow *)r->gpu.window_handle, s);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}
static JSValue js_clipboard_clear(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
    JSValueConst e = JS_NewString(ctx, "");
    JSValue r = js_clipboard_writeText(ctx, JS_NULL, 1, &e);
    JS_FreeValue(ctx, e);
    return r;
}

/* dialog: message box + open/save (Win uses native commdlg). */
static JSValue js_dialog_showMessageBoxSync(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_NewInt32(ctx, 0);
    JSValue mv = JS_GetPropertyStr(ctx, argv[0], "message");
    JSValue tv2 = JS_GetPropertyStr(ctx, argv[0], "title");
    JSValue buttons = JS_GetPropertyStr(ctx, argv[0], "buttons");
    const char *msg = JS_IsString(mv) ? JS_ToCString(ctx, mv) : NULL;
    const char *title = JS_IsString(tv2) ? JS_ToCString(ctx, tv2) : NULL;
    int nbtn = 0;
    if (JS_IsArray(buttons))
        JS_ToInt32(ctx, &nbtn, JS_GetPropertyStr(ctx, buttons, "length"));
    int chosen = 0;
#if defined(_WIN32)
    wchar_t *wm = utf8_to_wide(msg ? msg : "");
    wchar_t *wt = utf8_to_wide(title ? title : "");
    UINT flags = (nbtn >= 2) ? MB_YESNO : MB_OK;
    int r = MessageBoxW(NULL, wm ? wm : L"", wt ? wt : L"", flags);
    chosen = (flags == MB_YESNO) ? (r == IDYES ? 0 : 1) : 0;
    free(wm);
    free(wt);
#else
    (void)nbtn;
    fprintf(stderr, "[dialog] %s: %s\n", title ? title : "", msg ? msg : "");
#endif
    JS_FreeCString(ctx, msg);
    JS_FreeCString(ctx, title);
    JS_FreeValue(ctx, mv);
    JS_FreeValue(ctx, tv2);
    JS_FreeValue(ctx, buttons);
    return JS_NewInt32(ctx, chosen);
}

static JSValue js_dialog_showMessageBox(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    JSValue cb = (argc > 1 && JS_IsFunction(ctx, argv[argc - 1]))
                    ? JS_DupValue(ctx, argv[argc - 1])
                    : (argc > 0 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED);
    JSValue result = js_dialog_showMessageBoxSync(ctx, JS_NULL, argc > 1 ? 1 : 0, argv);
    int r = 0;
    JS_ToInt32(ctx, &r, result);
    JS_FreeValue(ctx, result);
    if (JS_IsFunction(ctx, cb))
    {
        JSValue a = JS_NewInt32(ctx, r);
        defer_callback(ctx, cb, 1, &a);
        JS_FreeValue(ctx, a);
    }
    JS_FreeValue(ctx, cb);
    return JS_UNDEFINED;
}

static JSValue js_dialog_showErrorBox(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    const char *title = (argc > 0 && JS_IsString(argv[0])) ? JS_ToCString(ctx, argv[0]) : NULL;
    const char *content = (argc > 1 && JS_IsString(argv[1])) ? JS_ToCString(ctx, argv[1]) : NULL;
#if defined(_WIN32)
    wchar_t *wt = utf8_to_wide(title ? title : "");
    wchar_t *wc = utf8_to_wide(content ? content : "");
    MessageBoxW(NULL, wc ? wc : L"", wt ? wt : L"Error", MB_ICONERROR | MB_OK);
    free(wt);
    free(wc);
#else
    fprintf(stderr, "[error] %s: %s\n", title ? title : "", content ? content : "");
#endif
    JS_FreeCString(ctx, title);
    JS_FreeCString(ctx, content);
    return JS_UNDEFINED;
}

#if defined(_WIN32)
/* Win open/save dialog helper: returns a malloc'd UTF-8 path or NULL. */
static char *win_file_dialog(JSContext *ctx, int save)
{
    wchar_t wpath[MAX_PATH] = {0};
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = wpath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : 0);
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok)
        return NULL;
    return wide_to_utf8(wpath);
}
#endif

static JSValue js_dialog_showOpenDialogSync(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
#if defined(_WIN32)
    char *p = win_file_dialog(ctx, 0);
    if (!p)
        return JS_NewArray(ctx); /* empty array = cancelled */
    JSValue a = JS_NewArray(ctx);
    JS_SetPropertyInt64(ctx, a, 0, JS_NewString(ctx, p));
    free(p);
    return a;
#else
    return JS_NewArray(ctx);
#endif
}
static JSValue js_dialog_showSaveDialogSync(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
#if defined(_WIN32)
    char *p = win_file_dialog(ctx, 1);
    if (!p)
        return JS_NewString(ctx, "");
    JSValue v = JS_NewString(ctx, p);
    free(p);
    return v;
#else
    return JS_NewString(ctx, "");
#endif
}

/* BrowserWindow: wraps the single GLFW window. */
static JSValue js_bw_setTitle(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_UNDEFINED;
    const char *t = JS_ToCString(ctx, argv[0]);
    if (t)
    {
        struct MiniBridge *b = nb_of(ctx);
        struct MiniRenderer *r = mini_bridge_renderer(b);
        if (r && r->gpu.window_handle)
            glfwSetWindowTitle((GLFWwindow *)r->gpu.window_handle, t);
        JS_FreeCString(ctx, t);
    }
    return JS_UNDEFINED;
}
static JSValue js_bw_setSize(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    int w = 800, h = 600;
    if (argc > 0) JS_ToInt32(ctx, &w, argv[0]);
    if (argc > 1) JS_ToInt32(ctx, &h, argv[1]);
    struct MiniBridge *b = nb_of(ctx);
    struct MiniRenderer *r = mini_bridge_renderer(b);
    if (r && r->gpu.window_handle)
        glfwSetWindowSize((GLFWwindow *)r->gpu.window_handle, w, h);
    return JS_UNDEFINED;
}
static JSValue js_bw_simple(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv, int magic)
{
    (void)tv; (void)argc; (void)argv;
    struct MiniBridge *b = nb_of(ctx);
    struct MiniRenderer *r = mini_bridge_renderer(b);
    GLFWwindow *win = (r && r->gpu.window_handle) ? (GLFWwindow *)r->gpu.window_handle : NULL;
    if (!win) return JS_UNDEFINED;
    switch (magic)
    {
    case 0: glfwIconifyWindow(win); break;
    case 1: glfwMaximizeWindow(win); break;
    case 2: glfwRestoreWindow(win); break;
    case 3: glfwShowWindow(win); break;
    case 4: glfwHideWindow(win); break;
    case 5: glfwSetWindowShouldClose(win, GLFW_TRUE); break;
    }
    return JS_UNDEFINED;
}
static JSValue js_bw_minimize(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv) { return js_bw_simple(ctx, tv, argc, argv, 0); }
static JSValue js_bw_maximize(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv) { return js_bw_simple(ctx, tv, argc, argv, 1); }
static JSValue js_bw_restore(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv) { return js_bw_simple(ctx, tv, argc, argv, 2); }
static JSValue js_bw_show(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv) { return js_bw_simple(ctx, tv, argc, argv, 3); }
static JSValue js_bw_hide(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv) { return js_bw_simple(ctx, tv, argc, argv, 4); }
static JSValue js_bw_close(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv) { return js_bw_simple(ctx, tv, argc, argv, 5); }

static JSValue js_bw_loadFile(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "loadFile(path) expected a string");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    FILE *fp = fopen(path, "rb");
    if (!fp)
    {
        JSValue e = JS_ThrowTypeError(ctx, "loadFile: cannot open '%s'", path);
        JS_FreeCString(ctx, path);
        return e;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    size_t rd = buf ? fread(buf, 1, (size_t)sz, fp) : 0;
    buf[rd] = '\0';
    fclose(fp);
    JS_FreeCString(ctx, path);
    struct MiniBridge *b = nb_of(ctx);
    mini_bridge_load_html(b, buf);
    free(buf);
    return JS_UNDEFINED;
}

/* BrowserWindow constructor: returns a wrapper around the live window. */
static JSValue js_BrowserWindow_ctor(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    JSValue w = JS_NewObject(ctx);
    if (argc > 0 && JS_IsObject(argv[0]))
    {
        JSValue title = JS_GetPropertyStr(ctx, argv[0], "title");
        if (JS_IsString(title))
            js_bw_setTitle(ctx, JS_NULL, 1, &title);
        JS_FreeValue(ctx, title);
        JSValue wv = JS_GetPropertyStr(ctx, argv[0], "width");
        JSValue hv = JS_GetPropertyStr(ctx, argv[0], "height");
        if (JS_IsNumber(wv) && JS_IsNumber(hv))
        {
            JSValueConst tmp[2] = { wv, hv };
            js_bw_setSize(ctx, JS_NULL, 2, tmp);
        }
        JS_FreeValue(ctx, wv);
        JS_FreeValue(ctx, hv);
    }
    JS_SetPropertyStr(ctx, w, "loadFile", JS_NewCFunction(ctx, js_bw_loadFile, "loadFile", 1));
    JS_SetPropertyStr(ctx, w, "loadURL", JS_NewCFunction(ctx, js_bw_loadFile, "loadURL", 1));
    JS_SetPropertyStr(ctx, w, "setTitle", JS_NewCFunction(ctx, js_bw_setTitle, "setTitle", 1));
    JS_SetPropertyStr(ctx, w, "setSize", JS_NewCFunction(ctx, js_bw_setSize, "setSize", 2));
    JS_SetPropertyStr(ctx, w, "minimize", JS_NewCFunction(ctx, js_bw_minimize, "minimize", 0));
    JS_SetPropertyStr(ctx, w, "maximize", JS_NewCFunction(ctx, js_bw_maximize, "maximize", 0));
    JS_SetPropertyStr(ctx, w, "restore", JS_NewCFunction(ctx, js_bw_restore, "restore", 0));
    JS_SetPropertyStr(ctx, w, "show", JS_NewCFunction(ctx, js_bw_show, "show", 0));
    JS_SetPropertyStr(ctx, w, "hide", JS_NewCFunction(ctx, js_bw_hide, "hide", 0));
    JS_SetPropertyStr(ctx, w, "close", JS_NewCFunction(ctx, js_bw_close, "close", 0));
    return w;
}

/* Menu: buildFromTemplate returns a plain holder; setApplicationMenu installs
   a Win top-level menu (other platforms: best-effort, no native menu). */
static JSValue js_Menu_buildFromTemplate(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    JSValue m = JS_NewObject(ctx);
    if (argc > 0)
        JS_SetPropertyStr(ctx, m, "items", JS_DupValue(ctx, argv[0]));
    return m;
}
static JSValue js_Menu_setApplicationMenu(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
#if defined(_WIN32)
    struct MiniBridge *b = nb_of(ctx);
    struct MiniRenderer *r = mini_bridge_renderer(b);
    GLFWwindow *win = (r && r->gpu.window_handle) ? (GLFWwindow *)r->gpu.window_handle : NULL;
    if (win)
    {
        HWND hwnd = glfwGetWin32Window(win);
        if (hwnd)
        {
            HMENU menu = CreateMenu();
            if (argc > 0 && JS_IsObject(argv[0]))
            {
                JSValue items = JS_GetPropertyStr(ctx, argv[0], "items");
                int n = 0;
                if (JS_IsArray(items))
                    JS_ToInt32(ctx, &n, JS_GetPropertyStr(ctx, items, "length"));
                for (int i = 0; i < n; i++)
                {
                    JSValue it = JS_GetPropertyInt64(ctx, items, (uint32_t)i);
                    JSValue lbl = JS_GetPropertyStr(ctx, it, "label");
                    const char *s = JS_IsString(lbl) ? JS_ToCString(ctx, lbl) : NULL;
                    if (s)
                    {
                        wchar_t *ws = utf8_to_wide(s);
                        if (ws) { AppendMenuW(menu, MF_STRING, (UINT_PTR)(i + 1), ws); free(ws); }
                        JS_FreeCString(ctx, s);
                    }
                    JS_FreeValue(ctx, lbl);
                    JS_FreeValue(ctx, it);
                }
                JS_FreeValue(ctx, items);
            }
            SetMenu(hwnd, menu);
            DrawMenuBar(hwnd);
        }
    }
#endif
    return JS_UNDEFINED;
}

/* screen: primary display + all displays via GLFW monitors. */
static JSValue screen_obj_for(JSContext *ctx, GLFWmonitor *mon)
{
    if (!mon)
        return JS_NewObject(ctx);
    const GLFWvidmode *vm = glfwGetVideoMode(mon);
    int x, y, w, h;
    glfwGetMonitorWorkarea(mon, &x, &y, &w, &h);
    JSValue o = JS_NewObject(ctx);
    JSValue bounds = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, bounds, "x", JS_NewInt32(ctx, x));
    JS_SetPropertyStr(ctx, bounds, "y", JS_NewInt32(ctx, y));
    JS_SetPropertyStr(ctx, bounds, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, bounds, "height", JS_NewInt32(ctx, h));
    JS_SetPropertyStr(ctx, o, "bounds", bounds);
    JSValue size = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, size, "width", JS_NewInt32(ctx, vm ? vm->width : w));
    JS_SetPropertyStr(ctx, size, "height", JS_NewInt32(ctx, vm ? vm->height : h));
    JS_SetPropertyStr(ctx, o, "size", size);
    struct MiniBridge *b = nb_of(ctx);
    struct MiniRenderer *r = mini_bridge_renderer(b);
    float sx = 1.0f, sy = 1.0f;
    if (r && r->gpu.window_handle)
        glfwGetWindowContentScale((GLFWwindow *)r->gpu.window_handle, &sx, &sy);
    JS_SetPropertyStr(ctx, o, "scaleFactor", JS_NewFloat64(ctx, (double)sx));
    return o;
}
static JSValue js_screen_getPrimaryDisplay(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
    return screen_obj_for(ctx, glfwGetPrimaryMonitor());
}
static JSValue js_screen_getAllDisplays(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
    int count = 0;
    GLFWmonitor **mons = glfwGetMonitors(&count);
    JSValue a = JS_NewArray(ctx);
    for (int i = 0; i < count; i++)
        JS_SetPropertyInt64(ctx, a, (uint32_t)i, screen_obj_for(ctx, mons[i]));
    return a;
}

static JSValue js_nativeImage_createEmpty(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv; (void)argc; (void)argv;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "isEmpty", JS_TRUE);
    return o;
}
static JSValue js_nativeImage_readFromPath(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    (void)tv;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "isEmpty", JS_FALSE);
    if (argc > 0 && JS_IsString(argv[0]))
    {
        const char *p = JS_ToCString(ctx, argv[0]);
        JS_SetPropertyStr(ctx, o, "path", JS_NewString(ctx, p ? p : ""));
        JS_FreeCString(ctx, p);
    }
    return o;
}

static void install_electron(JSContext *ctx, JSValue mods, JSValue global)
{
    JSValue e = JS_NewObject(ctx);

    /* app */
    JSValue app = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, app, "getName", JS_NewCFunction(ctx, js_app_getName, "getName", 0));
    JS_SetPropertyStr(ctx, app, "setName", JS_NewCFunction(ctx, js_app_setName, "setName", 1));
    JS_SetPropertyStr(ctx, app, "getVersion", JS_NewCFunction(ctx, js_app_getVersion, "getVersion", 0));
    JS_SetPropertyStr(ctx, app, "getPath", JS_NewCFunction(ctx, js_app_getPath, "getPath", 1));
    JS_SetPropertyStr(ctx, app, "quit", JS_NewCFunction(ctx, js_app_request_quit, "quit", 0));
    JS_SetPropertyStr(ctx, app, "exit", JS_NewCFunction(ctx, js_app_request_quit, "exit", 1));
    JS_SetPropertyStr(ctx, app, "whenReady", JS_NewCFunction(ctx, js_app_whenReady, "whenReady", 0));
    JS_SetPropertyStr(ctx, app, "isReady", JS_NewCFunction(ctx, js_app_whenReady, "isReady", 0)); /* truthy */
    JS_SetPropertyStr(ctx, app, "on", JS_NewCFunction(ctx, js_app_on, "on", 2));
    JS_SetPropertyStr(ctx, app, "requestSingleInstanceLock", JS_NewCFunction(ctx, js_app_whenReady, "requestSingleInstanceLock", 0));
    JS_SetPropertyStr(ctx, e, "app", app);

    /* shell */
    JSValue shell = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, shell, "openExternal", JS_NewCFunction(ctx, js_shell_openExternal, "openExternal", 1));
    JS_SetPropertyStr(ctx, shell, "openPath", JS_NewCFunction(ctx, js_shell_openPath, "openPath", 1));
    JS_SetPropertyStr(ctx, shell, "showItemInFolder", JS_NewCFunction(ctx, js_shell_showItemInFolder, "showItemInFolder", 1));
    JS_SetPropertyStr(ctx, shell, "moveItemToTrash", JS_NewCFunction(ctx, js_shell_moveItemToTrash, "moveItemToTrash", 1));
    JS_SetPropertyStr(ctx, shell, "beep", JS_NewCFunction(ctx, js_shell_beep, "beep", 0));
    JS_SetPropertyStr(ctx, e, "shell", shell);

    /* clipboard */
    JSValue clip = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, clip, "readText", JS_NewCFunction(ctx, js_clipboard_readText, "readText", 0));
    JS_SetPropertyStr(ctx, clip, "writeText", JS_NewCFunction(ctx, js_clipboard_writeText, "writeText", 1));
    JS_SetPropertyStr(ctx, clip, "clear", JS_NewCFunction(ctx, js_clipboard_clear, "clear", 0));
    JS_SetPropertyStr(ctx, e, "clipboard", clip);

    /* dialog */
    JSValue dlg = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, dlg, "showMessageBoxSync", JS_NewCFunction(ctx, js_dialog_showMessageBoxSync, "showMessageBoxSync", 1));
    JS_SetPropertyStr(ctx, dlg, "showMessageBox", JS_NewCFunction(ctx, js_dialog_showMessageBox, "showMessageBox", 2));
    JS_SetPropertyStr(ctx, dlg, "showErrorBox", JS_NewCFunction(ctx, js_dialog_showErrorBox, "showErrorBox", 2));
    JS_SetPropertyStr(ctx, dlg, "showOpenDialogSync", JS_NewCFunction(ctx, js_dialog_showOpenDialogSync, "showOpenDialogSync", 1));
    JS_SetPropertyStr(ctx, dlg, "showSaveDialogSync", JS_NewCFunction(ctx, js_dialog_showSaveDialogSync, "showSaveDialogSync", 1));
    JS_SetPropertyStr(ctx, e, "dialog", dlg);

    /* BrowserWindow constructor */
    JSValue bw = JS_NewCFunction2(ctx, (JSCFunction *)js_BrowserWindow_ctor, "BrowserWindow", 1, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, e, "BrowserWindow", bw);

    /* Menu */
    JSValue menu = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, menu, "buildFromTemplate", JS_NewCFunction(ctx, js_Menu_buildFromTemplate, "buildFromTemplate", 1));
    JS_SetPropertyStr(ctx, menu, "setApplicationMenu", JS_NewCFunction(ctx, js_Menu_setApplicationMenu, "setApplicationMenu", 1));
    JS_SetPropertyStr(ctx, e, "Menu", menu);

    /* screen */
    JSValue scr = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, scr, "getPrimaryDisplay", JS_NewCFunction(ctx, js_screen_getPrimaryDisplay, "getPrimaryDisplay", 0));
    JS_SetPropertyStr(ctx, scr, "getAllDisplays", JS_NewCFunction(ctx, js_screen_getAllDisplays, "getAllDisplays", 0));
    JS_SetPropertyStr(ctx, e, "screen", scr);

    /* nativeImage */
    JSValue ni = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ni, "createEmpty", JS_NewCFunction(ctx, js_nativeImage_createEmpty, "createEmpty", 0));
    JS_SetPropertyStr(ctx, ni, "readFromPath", JS_NewCFunction(ctx, js_nativeImage_readFromPath, "readFromPath", 1));
    JS_SetPropertyStr(ctx, e, "nativeImage", ni);

    JS_SetPropertyStr(ctx, mods, "electron", e);
    /* electron is also available as a global for parity with Electron's
       renderer process where `require('electron')` is prebound. */
    JS_SetPropertyStr(ctx, global, "electron", JS_DupValue(ctx, e));
}

/* ================================================================== */
/* install_native — built-in module table + globals                   */
/* ================================================================== */

void install_native(struct MiniBridge *b)
{
    JSContext *ctx = mini_bridge_ctx(b);
    JSValue global = JS_GetGlobalObject(ctx);

    /* Built-in module table: __miniBuiltinModules. Each property is the module
       object; the JS shim's require() reads it for require('os'), etc. */
    JSValue mods = JS_NewObject(ctx);
    install_os(ctx, mods);
    install_process(ctx, mods, global);
    install_fs(ctx, mods);
    install_path(ctx, mods);
    /* ChildProcess class for spawn() wrappers (opaque = JsChildProc*) */
    {
        JSRuntime *rt = JS_GetRuntime(ctx);
        if (!g_cp_cid)
        {
            JS_NewClassID(rt, &g_cp_cid);
            JSClassDef cpdef = {.class_name = "ChildProcess", .finalizer = js_cp_finalizer};
            JS_NewClass(rt, g_cp_cid, &cpdef);
        }
    }
    install_child_process(ctx, mods);
    install_electron(ctx, mods, global);

    JS_SetPropertyStr(ctx, global, "__miniBuiltinModules", mods);
    mini_bridge_set_builtin_mods(b, JS_DupValue(ctx, mods));

    JS_FreeValue(ctx, global);
}

/* ================================================================== */
/* pump + destroy (Phase 3 fills children; Phase 1 no-ops)            */
/* ================================================================== */

void bridge_pump_children(struct MiniBridge *b)
{
    if (!b)
        return;
    int n = 0;
    JsChildProc **arr = mini_bridge_children(b, &n);
    if (!arr || n == 0)
        return;
    JSContext *ctx = mini_bridge_ctx(b);
    JSRuntime *rt = JS_GetRuntime(ctx);
    for (int i = 0; i < n; i++)
    {
        JsChildProc *c = arr[i];
        if (!c || c->exited)
            continue;
        cp_drain(c, 0);
        cp_drain(c, 1);
        cp_fire_data(ctx, c, 0);
        cp_fire_data(ctx, c, 1);
        if (cp_poll_exit(c))
        {
            /* final drain + final data delivery before firing completion */
            cp_drain(c, 0);
            cp_drain(c, 1);
            cp_fire_data(ctx, c, 0);
            cp_fire_data(ctx, c, 1);
            /* exec-mode: cb(err, stdout, stderr) */
            if (JS_IsFunction(ctx, c->cb))
            {
                JSValue args[3];
                args[0] = JS_NULL;
                args[1] = (c->stdout_buf && c->stdout_len)
                              ? JS_NewStringLen(ctx, c->stdout_buf, c->stdout_len)
                              : JS_NewString(ctx, "");
                args[2] = (c->stderr_buf && c->stderr_len)
                              ? JS_NewStringLen(ctx, c->stderr_buf, c->stderr_len)
                              : JS_NewString(ctx, "");
                JSValue r = JS_Call(ctx, c->cb, JS_UNDEFINED, 3, args);
                JS_FreeValue(ctx, r);
                JS_FreeValue(ctx, args[1]);
                JS_FreeValue(ctx, args[2]);
            }
            /* spawn-mode: 'exit' then 'close' listeners */
            if (JS_IsFunction(ctx, c->onexit))
            {
                JSValue a = JS_NewInt32(ctx, c->exit_code);
                JSValue r = JS_Call(ctx, c->onexit, JS_UNDEFINED, 1, &a);
                JS_FreeValue(ctx, r);
                JS_FreeValue(ctx, a);
            }
            if (JS_IsFunction(ctx, c->onclose))
            {
                JSValue a = JS_NewInt32(ctx, c->exit_code);
                JSValue r = JS_Call(ctx, c->onclose, JS_UNDEFINED, 1, &a);
                JS_FreeValue(ctx, r);
                JS_FreeValue(ctx, a);
            }
            /* sweep: drop the array's ref; free if the JS wrapper is gone too */
            mini_bridge_remove_child(b, i);
            i--; /* compacted slot re-examined next iteration */
            c->ref--;
            if (c->ref <= 0)
                cp_free(rt, c);
        }
    }
}

void mini_native_destroy(struct MiniBridge *b)
{
    if (!b)
        return;
    int n = 0;
    JsChildProc **arr = mini_bridge_children(b, &n);
    if (!arr || n == 0)
        return;
    JSContext *ctx = mini_bridge_ctx(b);
    JSRuntime *rt = ctx ? JS_GetRuntime(ctx) : NULL;
    for (int i = 0; i < n; i++)
    {
        JsChildProc *c = arr[i];
        if (!c)
            continue;
        c->ref--; /* drop the array's ref */
        if (c->ref <= 0 && rt)
            cp_free(rt, c);
    }
}
