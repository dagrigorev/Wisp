/*
 * log.c — Structured process logger implementation.
 *
 * Thread-safety: every write is serialized with a CRITICAL_SECTION.
 * Rotation policy: the active log file is truncated and reused when the next
 * write would make it exceed the configured max size. Default limit: 10 MiB.
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include "log.h"

#define WISP_LOG_DEFAULT_MAX_BYTES (10ULL * 1024ULL * 1024ULL)

static struct {
    LogLevel          min_level;
    CRITICAL_SECTION  lock;
    HANDLE            h_file;
    BOOL              initialised;
    unsigned long long max_bytes;
    char              path[MAX_PATH];
    char              app_name[64];
} g_log;

static void log_ensure_init(void) {
    if (!g_log.initialised) {
        InitializeCriticalSection(&g_log.lock);
#ifdef NDEBUG
        g_log.min_level = LOG_INFO;
#else
        g_log.min_level = LOG_DEBUG;
#endif
        g_log.h_file = INVALID_HANDLE_VALUE;
        g_log.max_bytes = WISP_LOG_DEFAULT_MAX_BYTES;
        strcpy(g_log.app_name, "wisp");
        g_log.initialised = TRUE;
    }
}

static void utf8_to_wide(const char *s, wchar_t *out, int out_len) {
    if (!out || out_len <= 0) return;
    out[0] = L'\0';
    if (!s) return;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out, out_len);
}

static void get_exe_dir_w(wchar_t *out, DWORD out_len) {
    if (!out || out_len == 0) return;
    out[0] = L'\0';
    GetModuleFileNameW(NULL, out, out_len);
    wchar_t *p1 = wcsrchr(out, L'\\');
    wchar_t *p2 = wcsrchr(out, L'/');
    wchar_t *p = p1 ? p1 : p2;
    if (p1 && p2) p = (p1 > p2) ? p1 : p2;
    if (p) *p = L'\0';
}

static void make_dir_if_missing(const wchar_t *path) {
    if (!path || !*path) return;
    DWORD attr = GetFileAttributesW(path);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return;
    CreateDirectoryW(path, NULL);
}

static BOOL build_default_log_path(char *out, int out_len) {
    if (!out || out_len <= 0) return FALSE;
    out[0] = '\0';

    wchar_t root[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", root, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wchar_t app_dir[MAX_PATH];
        wchar_t log_dir[MAX_PATH];
        _snwprintf(app_dir, MAX_PATH - 1, L"%s\\Wisp", root);
        _snwprintf(log_dir, MAX_PATH - 1, L"%s\\logs", app_dir);
        app_dir[MAX_PATH - 1] = L'\0';
        log_dir[MAX_PATH - 1] = L'\0';
        make_dir_if_missing(app_dir);
        make_dir_if_missing(log_dir);
        wchar_t file[MAX_PATH];
        _snwprintf(file, MAX_PATH - 1, L"%s\\wisp.log", log_dir);
        file[MAX_PATH - 1] = L'\0';
        WideCharToMultiByte(CP_UTF8, 0, file, -1, out, out_len, NULL, NULL);
        return TRUE;
    }

    wchar_t exe_dir[MAX_PATH] = {0};
    get_exe_dir_w(exe_dir, MAX_PATH);
    if (exe_dir[0]) {
        wchar_t log_dir[MAX_PATH];
        _snwprintf(log_dir, MAX_PATH - 1, L"%s\\logs", exe_dir);
        log_dir[MAX_PATH - 1] = L'\0';
        make_dir_if_missing(log_dir);
        wchar_t file[MAX_PATH];
        _snwprintf(file, MAX_PATH - 1, L"%s\\wisp.log", log_dir);
        file[MAX_PATH - 1] = L'\0';
        WideCharToMultiByte(CP_UTF8, 0, file, -1, out, out_len, NULL, NULL);
        return TRUE;
    }
    return FALSE;
}

void wisp_log_set_level(LogLevel level) {
    log_ensure_init();
    g_log.min_level = level;
}

void wisp_log_set_max_file_size(unsigned long long max_bytes) {
    log_ensure_init();
    if (max_bytes > 0) g_log.max_bytes = max_bytes;
}

void wisp_log_set_file(const char *path) {
    log_ensure_init();
    if (!path || !*path) return;

    wchar_t wpath[MAX_PATH];
    utf8_to_wide(path, wpath, MAX_PATH);

    EnterCriticalSection(&g_log.lock);
    if (g_log.h_file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_log.h_file);
        CloseHandle(g_log.h_file);
    }
    strncpy(g_log.path, path, sizeof(g_log.path) - 1);
    g_log.path[sizeof(g_log.path) - 1] = '\0';
    g_log.h_file = CreateFileW(wpath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log.h_file != INVALID_HANDLE_VALUE) {
        SetFilePointer(g_log.h_file, 0, NULL, FILE_END);
    }
    LeaveCriticalSection(&g_log.lock);
}

void wisp_log_init_default(const char *app_name) {
    log_ensure_init();
    if (app_name && *app_name) {
        strncpy(g_log.app_name, app_name, sizeof(g_log.app_name) - 1);
        g_log.app_name[sizeof(g_log.app_name) - 1] = '\0';
    }
    char path[MAX_PATH];
    if (build_default_log_path(path, MAX_PATH)) {
        wisp_log_set_file(path);
    }
}

void wisp_log_close(void) {
    log_ensure_init();
    EnterCriticalSection(&g_log.lock);
    if (g_log.h_file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_log.h_file);
        CloseHandle(g_log.h_file);
        g_log.h_file = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&g_log.lock);
}

static const char *level_tag(LogLevel l) {
    switch (l) {
        case LOG_DEBUG: return "DBG";
        case LOG_INFO:  return "INF";
        case LOG_WARN:  return "WRN";
        case LOG_ERROR: return "ERR";
        default:        return "???";
    }
}

static void truncate_if_needed_locked(DWORD next_write_len) {
    if (g_log.h_file == INVALID_HANDLE_VALUE || g_log.max_bytes == 0) return;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(g_log.h_file, &size)) return;
    unsigned long long current = (unsigned long long)size.QuadPart;
    if (current + (unsigned long long)next_write_len <= g_log.max_bytes) return;
    SetFilePointer(g_log.h_file, 0, NULL, FILE_BEGIN);
    SetEndOfFile(g_log.h_file);
}

void wisp_log_write(LogLevel level, const char *file, int line, const char *fmt, ...) {
    log_ensure_init();
    if (level < g_log.min_level) return;

    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(msg, sizeof(msg) - 1, fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = '\0';

    const char *fname = file ? strrchr(file, '\\') : NULL;
    if (!fname && file) fname = strrchr(file, '/');
    fname = fname ? fname + 1 : (file ? file : "?");

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line_buf[2600];
    int n = _snprintf(line_buf, sizeof(line_buf) - 2,
        "%04u-%02u-%02u %02u:%02u:%02u.%03u [WISP/%s] [%s] [%s:%d] %s\r\n",
        (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
        (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond, (unsigned)st.wMilliseconds,
        g_log.app_name, level_tag(level), fname, line, msg);
    if (n < 0) n = (int)sizeof(line_buf) - 2;
    line_buf[n] = '\0';

    wchar_t wbuf[2600];
    MultiByteToWideChar(CP_UTF8, 0, line_buf, -1, wbuf, 2600);

    EnterCriticalSection(&g_log.lock);
    OutputDebugStringW(wbuf);
    if (g_log.h_file != INVALID_HANDLE_VALUE) {
        DWORD len = (DWORD)strlen(line_buf);
        truncate_if_needed_locked(len);
        SetFilePointer(g_log.h_file, 0, NULL, FILE_END);
        DWORD written = 0;
        WriteFile(g_log.h_file, line_buf, len, &written, NULL);
        FlushFileBuffers(g_log.h_file);
    }
    LeaveCriticalSection(&g_log.lock);
}

void wisp_log_win32(const char *context) {
    DWORD err = GetLastError();
    char msg[512] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err, 0, msg, sizeof(msg) - 1, NULL);
    char *p = msg + strlen(msg);
    while (p > msg && (p[-1] == '\r' || p[-1] == '\n')) p--;
    *p = '\0';
    WISP_LOG_ERROR("%s: Win32 error %lu - %s", context ? context : "Win32", (unsigned long)err, msg);
}

static LONG WINAPI wisp_unhandled_exception_filter(EXCEPTION_POINTERS *ep) {
    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    void *addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : NULL;
    WISP_LOG_ERROR("Unhandled structured exception: code=0x%08lX address=%p", (unsigned long)code, addr);
    wisp_log_close();
    return EXCEPTION_EXECUTE_HANDLER;
}

static void wisp_invalid_parameter_handler(const wchar_t *expression,
                                          const wchar_t *function,
                                          const wchar_t *file,
                                          unsigned int line,
                                          uintptr_t reserved) {
    (void)reserved;
    char expr[512] = {0}, func[256] = {0}, f[512] = {0};
    if (expression) WideCharToMultiByte(CP_UTF8, 0, expression, -1, expr, sizeof(expr), NULL, NULL);
    if (function)   WideCharToMultiByte(CP_UTF8, 0, function, -1, func, sizeof(func), NULL, NULL);
    if (file)       WideCharToMultiByte(CP_UTF8, 0, file, -1, f, sizeof(f), NULL, NULL);
    WISP_LOG_ERROR("Invalid CRT parameter: expression='%s' function='%s' file='%s' line=%u",
                  expr, func, f, line);
}

static void wisp_signal_handler(int sig) {
    WISP_LOG_ERROR("Process signal received: %d", sig);
    wisp_log_close();
    signal(sig, SIG_DFL);
    raise(sig);
}

void wisp_log_install_crash_handlers(void) {
    log_ensure_init();
    SetUnhandledExceptionFilter(wisp_unhandled_exception_filter);
    _set_invalid_parameter_handler(wisp_invalid_parameter_handler);
    signal(SIGABRT, wisp_signal_handler);
    signal(SIGFPE,  wisp_signal_handler);
    signal(SIGILL,  wisp_signal_handler);
    signal(SIGINT,  wisp_signal_handler);
    signal(SIGSEGV, wisp_signal_handler);
    signal(SIGTERM, wisp_signal_handler);
}
