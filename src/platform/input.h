#pragma once
#ifndef WISP_INPUT_H
#define WISP_INPUT_H

#include <windows.h>
#include "wisp_bool.h"


#ifdef __cplusplus
extern "C" {
#endif

/* ─── Input event types ──────────────────────────────────────────────────── */

typedef enum {
    INPUT_NONE,
    INPUT_CHAR,         /* Regular character data to send to PTY/shell */
    INPUT_COPY,         /* Ctrl+Shift+C */
    INPUT_PASTE,        /* Ctrl+Shift+V */
    INPUT_NEW_TAB,      /* Ctrl+Shift+T */
    INPUT_CLOSE_TAB,    /* Ctrl+Shift+W */
    INPUT_NEXT_TAB,     /* Ctrl+Tab */
    INPUT_PREV_TAB,     /* Ctrl+Shift+Tab */
    INPUT_ZOOM_IN,      /* Ctrl+Shift+= */
    INPUT_ZOOM_OUT,     /* Ctrl+Shift+- */
    INPUT_SCROLL_UP,       /* Ctrl+Shift+Up */
    INPUT_SCROLL_DOWN,     /* Ctrl+Shift+Down */
    INPUT_SCROLL_PAGE_UP,  /* Shift+PageUp / Ctrl+Shift+PageUp */
    INPUT_SCROLL_PAGE_DOWN /* Shift+PageDown / Ctrl+Shift+PageDown */
} InputAction;

typedef struct {
    InputAction action;
    char        bytes[32];  /* Byte sequence for INPUT_CHAR */
    int         len;        /* Length of byte sequence */
} InputEvent;

/* ─── API ────────────────────────────────────────────────────────────────── */

/* Translate a WM_KEYDOWN/WM_CHAR event into an InputEvent.
   vk:     Virtual key code (from WM_KEYDOWN wParam)
   ch:     Character (from WM_CHAR wParam), 0 if not a char message
   lParam: lParam from the message
   Returns filled InputEvent; action == INPUT_NONE if not handled. */
InputEvent input_translate(WPARAM vk, WCHAR ch, LPARAM lParam, bool app_cursor_keys);

/* Check if a WM_KEYDOWN should suppress the subsequent WM_CHAR */
bool input_suppress_char(WPARAM vk, LPARAM lParam);


#ifdef __cplusplus
}
#endif

#endif /* WISP_INPUT_H */
