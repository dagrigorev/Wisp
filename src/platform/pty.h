#pragma once
#ifndef WISP_PTY_H
#define WISP_PTY_H

#include <windows.h>
#include "wisp_bool.h"


#ifdef __cplusplus
extern "C" {
#endif

/* ─── PTY Session ────────────────────────────────────────────────────────── */

typedef struct {
    HPCON   hpcon;          /* Pseudo console handle */
    HANDLE  hpipe_in;       /* Write → child stdin */
    HANDLE  hpipe_out;      /* Read ← child stdout/stderr */
    HANDLE  hprocess;       /* Child process handle */
    HANDLE  hthread_proc;   /* Child thread handle */
    HANDLE  hthread_reader; /* Reader thread handle */
    DWORD   pid;
    int     cols, rows;
    bool    alive;
} PtySession;

/* Callback signature for data arriving from child */
typedef void (*PtyDataCallback)(const char *buf, int len, void *userdata);

/* ─── API ────────────────────────────────────────────────────────────────── */

/* Create the ConPTY pipe pair and pseudo-console. Must call before pty_spawn. */
bool pty_create(PtySession *pty, int cols, int rows);

/* Spawn a process attached to the pseudo-console.
   cmdline: UTF-16 command line (e.g. L"cmd.exe")
   cwd:     UTF-16 working directory (may be NULL)
   cb/ud:   callback invoked for each chunk of output data
*/
bool pty_spawn(PtySession *pty, const wchar_t *cmdline, const wchar_t *cwd,
               PtyDataCallback cb, void *userdata);

/* Resize the pseudo-console to new dimensions. */
void pty_resize(PtySession *pty, int cols, int rows);

/* Send data to child stdin. Returns bytes written, -1 on error. */
int  pty_write(PtySession *pty, const char *buf, int len);

/* Signal child with Ctrl+C. */
void pty_ctrl_c(PtySession *pty);

/* Close the PTY session and wait for reader thread. */
void pty_close(PtySession *pty);

/* Returns true if the child process is still alive. */
bool pty_is_alive(PtySession *pty);

/* Get child exit code (valid after !pty_is_alive). */
DWORD pty_exit_code(PtySession *pty);


#ifdef __cplusplus
}
#endif

#endif /* WISP_PTY_H */
