#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "session.h"
#include "log.h"
#include "path_util.h"

void session_path(char *out, int out_size) {
    if (!out || out_size <= 0) return;
    char appdata[MAX_PATH] = {0};
    GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    _snprintf(out, out_size, "%s\\Wisp\\session.dat", appdata);
}

bool session_exists(void) {
    char path[MAX_PATH];
    session_path(path, sizeof(path));
    return path_exists(path);
}

bool session_save_file(const void *data, size_t size) {
    if (!data || size == 0) return false;

    char path[MAX_PATH];
    session_path(path, sizeof(path));

    char dir[MAX_PATH];
    strncpy(dir, path, MAX_PATH - 1);
    dir[MAX_PATH - 1] = '\0';
    char *bs = strrchr(dir, '\\');
    if (bs) {
        *bs = '\0';
        path_mkdirs(dir);
    }

    wchar_t *wpath = NULL;
    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
        if (wlen > 0) {
            wpath = HeapAlloc(GetProcessHeap(), 0, (size_t)wlen * sizeof(wchar_t));
            if (wpath) MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);
        }
    }
    if (!wpath) return false;

    HANDLE h = CreateFileW(wpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    HeapFree(GetProcessHeap(), 0, wpath);
    if (h == INVALID_HANDLE_VALUE) {
        WISP_LOG_ERROR("session_save: CreateFileW failed");
        wisp_log_win32("CreateFileW");
        return false;
    }

    DWORD written = 0;
    bool ok = WriteFile(h, data, (DWORD)size, &written, NULL) && written == (DWORD)size;
    if (!ok) {
        WISP_LOG_WARN("session_save: wrote %lu of %zu bytes", written, size);
    }
    CloseHandle(h);
    return ok;
}

bool session_read_file(void *data, size_t *size) {
    if (!data || !size) return false;

    char path[MAX_PATH];
    session_path(path, sizeof(path));

    wchar_t *wpath = NULL;
    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
        if (wlen > 0) {
            wpath = HeapAlloc(GetProcessHeap(), 0, (size_t)wlen * sizeof(wchar_t));
            if (wpath) MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);
        }
    }
    if (!wpath) return false;

    HANDLE h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    HeapFree(GetProcessHeap(), 0, wpath);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD file_size = GetFileSize(h, NULL);
    if (file_size == INVALID_FILE_SIZE || (size_t)file_size > *size) {
        CloseHandle(h);
        return false;
    }

    DWORD read = 0;
    bool ok = ReadFile(h, data, file_size, &read, NULL) && read == file_size;
    if (ok) *size = read;
    CloseHandle(h);
    return ok;
}

void session_delete(void) {
    char path[MAX_PATH];
    session_path(path, sizeof(path));
    if (path_exists(path)) {
        wchar_t *wpath = NULL;
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
        if (wlen > 0) {
            wpath = HeapAlloc(GetProcessHeap(), 0, (size_t)wlen * sizeof(wchar_t));
            if (wpath) MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);
        }
        if (wpath) {
            DeleteFileW(wpath);
            HeapFree(GetProcessHeap(), 0, wpath);
        }
    }
}
