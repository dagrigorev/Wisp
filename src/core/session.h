#pragma once
#ifndef WISP_SESSION_H
#define WISP_SESSION_H

#include <windows.h>
#include "wisp_bool.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WISP_SESSION_MAGIC  0x53485357
#define WISP_SESSION_VERSION 2

#define WISP_SESSION_MAX_PANES  4
#define WISP_SESSION_MAX_TABS   8
#define WISP_SESSION_MAX_SCROLLBACK 500

typedef struct {
    bool enabled;
    int  autosave_interval;
    int  max_scrollback_lines;
    bool prompt_on_restore;
} SessionCfg;

bool session_exists(void);
void session_path(char *out, int out_size);
bool session_save_file(const void *data, size_t size);
bool session_read_file(void *data, size_t *size);
void session_delete(void);

#ifdef __cplusplus
}
#endif

#endif
