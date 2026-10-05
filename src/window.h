#pragma once
#ifndef WISP_WINDOW_H
#define WISP_WINDOW_H

#include <windows.h>
#include "wisp_bool.h"
#include "platform/config.h"


#ifdef __cplusplus
extern "C" {
#endif

/* Register the Wisp window class. Must be called once before CreateWindow. */
bool window_register_class(HINSTANCE hInst);

/* Create the main terminal window. Returns NULL on failure. */
HWND window_create(HINSTANCE hInst, const Config *cfg, int nShow);

/* Returns true if any background jobs are running (used by WM_CLOSE confirm). */
bool window_has_running_jobs(HWND hwnd);


#ifdef __cplusplus
}
#endif

#endif /* WISP_WINDOW_H */
