#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "pty.h"
#include "../core/str_util.h"
#include "../core/log.h"

/* Reader thread context */
typedef struct {
    PtySession      *pty;
    PtyDataCallback  cb;
    void            *userdata;
} ReaderCtx;

/* ─── Reader Thread ──────────────────────────────────────────────────────── */

static DWORD WINAPI pty_reader_thread(LPVOID param) {
    ReaderCtx *ctx = (ReaderCtx *)param;
    PtySession *pty = ctx->pty;
    char buf[4096];
    DWORD n = 0;

    while (pty->alive && ReadFile(pty->hpipe_out, buf, sizeof(buf), &n, NULL) && n > 0) {
        if (ctx->cb) ctx->cb(buf, (int)n, ctx->userdata);
    }

    pty->alive = false;
    HeapFree(GetProcessHeap(), 0, ctx);
    return 0;
}

/* ─── pty_create ─────────────────────────────────────────────────────────── */

bool pty_create(PtySession *pty, int cols, int rows) {
    memset(pty, 0, sizeof(*pty));
    pty->hpipe_in = INVALID_HANDLE_VALUE;
    pty->hpipe_out = INVALID_HANDLE_VALUE;
    pty->cols = cols;
    pty->rows = rows;

    /* Pipe 1: PTY reads from this (our writes go to child stdin) */
    HANDLE pipe_read_from_us = INVALID_HANDLE_VALUE;
    HANDLE pipe_write_to_child = INVALID_HANDLE_VALUE;

    /* Pipe 2: PTY writes to this (child stdout, we read it) */
    HANDLE pipe_read_from_child = INVALID_HANDLE_VALUE;
    HANDLE pipe_write_to_us = INVALID_HANDLE_VALUE;

    if (!CreatePipe(&pipe_read_from_us, &pipe_write_to_child, NULL, 0)) {
        wisp_log_win32("CreatePipe (stdin)");
        return false;
    }
    if (!CreatePipe(&pipe_read_from_child, &pipe_write_to_us, NULL, 0)) {
        wisp_log_win32("CreatePipe (stdout)");
        CloseHandle(pipe_read_from_us);
        CloseHandle(pipe_write_to_child);
        return false;
    }

    COORD size = { (SHORT)cols, (SHORT)rows };
    HRESULT hr = CreatePseudoConsole(size, pipe_read_from_us, pipe_write_to_us, 0, &pty->hpcon);
    if (FAILED(hr)) {
        WISP_LOG_ERROR("CreatePseudoConsole failed: 0x%08X", hr);
        CloseHandle(pipe_read_from_us);
        CloseHandle(pipe_write_to_child);
        CloseHandle(pipe_read_from_child);
        CloseHandle(pipe_write_to_us);
        return false;
    }

    /* Close the ends used by the PTY (we hold the other ends) */
    CloseHandle(pipe_read_from_us);
    CloseHandle(pipe_write_to_us);

    pty->hpipe_in  = pipe_write_to_child; /* We write to child stdin via this */
    pty->hpipe_out = pipe_read_from_child; /* We read child output via this */
    pty->alive     = true;
    return true;
}

/* ─── pty_spawn ──────────────────────────────────────────────────────────── */

bool pty_spawn(PtySession *pty, const wchar_t *cmdline, const wchar_t *cwd,
               PtyDataCallback cb, void *userdata)
{
    if (!pty->hpcon) return false;

    /* Build STARTUPINFOEX with pseudo console attribute */
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attr_list =
        (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, attr_size);
    if (!attr_list) return false;

    if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size)) {
        wisp_log_win32("InitializeProcThreadAttributeList");
        HeapFree(GetProcessHeap(), 0, attr_list);
        return false;
    }

    if (!UpdateProcThreadAttribute(attr_list, 0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pty->hpcon,
            sizeof(pty->hpcon), NULL, NULL)) {
        wisp_log_win32("UpdateProcThreadAttribute");
        DeleteProcThreadAttributeList(attr_list);
        HeapFree(GetProcessHeap(), 0, attr_list);
        return false;
    }

    STARTUPINFOEXW si = {0};
    si.StartupInfo.cb  = sizeof(STARTUPINFOEXW);
    si.lpAttributeList = attr_list;

    /* Mutable copy of cmdline */
    wchar_t cmd_buf[32768];
    wcsncpy(cmd_buf, cmdline, 32767);
    cmd_buf[32767] = L'\0';

    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessW(
        NULL, cmd_buf, NULL, NULL, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        NULL, cwd, &si.StartupInfo, &pi);

    DeleteProcThreadAttributeList(attr_list);
    HeapFree(GetProcessHeap(), 0, attr_list);

    if (!ok) {
        wisp_log_win32("CreateProcessW");
        return false;
    }

    pty->hprocess      = pi.hProcess;
    pty->hthread_proc  = pi.hThread;
    pty->pid           = pi.dwProcessId;

    /* Start reader thread */
    ReaderCtx *ctx = (ReaderCtx *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ReaderCtx));
    if (!ctx) return false;
    ctx->pty      = pty;
    ctx->cb       = cb;
    ctx->userdata = userdata;

    pty->hthread_reader = CreateThread(NULL, 0, pty_reader_thread, ctx, 0, NULL);
    if (!pty->hthread_reader) {
        wisp_log_win32("CreateThread (reader)");
        HeapFree(GetProcessHeap(), 0, ctx);
        return false;
    }

    WISP_LOG_INFO("Spawned PID %lu: %ls", pty->pid, cmdline);
    return true;
}

/* ─── pty_resize ─────────────────────────────────────────────────────────── */

void pty_resize(PtySession *pty, int cols, int rows) {
    if (!pty->hpcon) return;
    if (cols == pty->cols && rows == pty->rows) return;
    pty->cols = cols;
    pty->rows = rows;
    COORD size = { (SHORT)cols, (SHORT)rows };
    HRESULT hr = ResizePseudoConsole(pty->hpcon, size);
    if (FAILED(hr)) WISP_LOG_ERROR("ResizePseudoConsole failed: 0x%08X", hr);
}

/* ─── pty_write ──────────────────────────────────────────────────────────── */

int pty_write(PtySession *pty, const char *buf, int len) {
    if (!pty->alive || !buf || len <= 0) return -1;
    DWORD written = 0;
    if (!WriteFile(pty->hpipe_in, buf, (DWORD)len, &written, NULL)) {
        wisp_log_win32("WriteFile (pty_write)");
        return -1;
    }
    return (int)written;
}

/* ─── pty_ctrl_c ─────────────────────────────────────────────────────────── */

void pty_ctrl_c(PtySession *pty) {
    /* Send Ctrl+C byte directly to the PTY */
    char ctrl_c = '\x03';
    pty_write(pty, &ctrl_c, 1);
}

/* ─── pty_close ──────────────────────────────────────────────────────────── */

void pty_close(PtySession *pty) {
    pty->alive = false;

    if (pty->hpcon) {
        ClosePseudoConsole(pty->hpcon);
        pty->hpcon = NULL;
    }
    if (pty->hpipe_in  && pty->hpipe_in  != INVALID_HANDLE_VALUE) { CloseHandle(pty->hpipe_in);  pty->hpipe_in  = INVALID_HANDLE_VALUE; }
    if (pty->hpipe_out && pty->hpipe_out != INVALID_HANDLE_VALUE) { CloseHandle(pty->hpipe_out); pty->hpipe_out = INVALID_HANDLE_VALUE; }

    if (pty->hthread_reader) {
        WaitForSingleObject(pty->hthread_reader, 3000);
        CloseHandle(pty->hthread_reader);
        pty->hthread_reader = NULL;
    }
    if (pty->hprocess) {
        DWORD code = STILL_ACTIVE;
        if (!GetExitCodeProcess(pty->hprocess, &code)) code = STILL_ACTIVE;
        if (code == STILL_ACTIVE) {
            if (WaitForSingleObject(pty->hprocess, 250) == WAIT_TIMEOUT) {
                TerminateProcess(pty->hprocess, 0);
            }
        }
        CloseHandle(pty->hprocess);
        pty->hprocess = NULL;
    }
    if (pty->hthread_proc) {
        CloseHandle(pty->hthread_proc);
        pty->hthread_proc = NULL;
    }
}

/* ─── Status queries ─────────────────────────────────────────────────────── */

bool pty_is_alive(PtySession *pty) {
    if (!pty->alive || !pty->hprocess) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(pty->hprocess, &code)) return false;
    if (code != STILL_ACTIVE) { pty->alive = false; return false; }
    return true;
}

DWORD pty_exit_code(PtySession *pty) {
    DWORD code = 0;
    if (pty->hprocess) GetExitCodeProcess(pty->hprocess, &code);
    return code;
}
