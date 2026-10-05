#include <windows.h>
#include <string.h>
#include "window.h"
#include "core/str_util.h"
#include "core/log.h"

extern LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

bool window_register_class(HINSTANCE hInst) {
    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL; /* We paint our own background */
    wc.lpszClassName = L"Wisp";
    wc.hIcon         = LoadIcon(hInst, MAKEINTRESOURCE(1));
    wc.hIconSm       = LoadIcon(hInst, MAKEINTRESOURCE(1));

    if (!RegisterClassExW(&wc)) {
        wisp_log_win32("RegisterClassExW");
        return false;
    }
    return true;
}

HWND window_create(HINSTANCE hInst, const Config *cfg, int nShow) {
    (void)cfg;
    /* Initial size: 80x24 cells approx — will be resized by renderer */
    int w = 800, h = 600;

    DWORD style    = WS_OVERLAPPEDWINDOW;
    DWORD ex_style = WS_EX_APPWINDOW;

    /* Adjust for client area */
    RECT r = { 0, 0, w, h };
    AdjustWindowRectEx(&r, style, FALSE, ex_style);
    int win_w = r.right  - r.left;
    int win_h = r.bottom - r.top;

    HWND hwnd = CreateWindowExW(
        ex_style,
        L"Wisp",
        L"Wisp",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        win_w, win_h,
        NULL, NULL,
        hInst, (LPVOID)cfg   /* Pass config as creation param */
    );

    if (!hwnd) {
        wisp_log_win32("CreateWindowExW");
        return NULL;
    }

    /* The app shows the window after Direct2D and the first terminal pane are
       initialized. Showing here can dispatch WM_PAINT while renderer globals
       are still empty. */
    return hwnd;
}

bool window_has_running_jobs(HWND hwnd) {
    /* Retrieved from global job table via message or global pointer */
    (void)hwnd;
    return false; /* Simplified — main.c checks directly */
}
