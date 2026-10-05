#pragma once
#ifndef WISP_UNICODE_H
#define WISP_UNICODE_H

#include <windows.h>
#include "wisp_bool.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * unicode.h — zsh-inspired multibyte compatibility layer for WISP.
 *
 * zsh keeps command text as bytes but interprets those bytes through the
 * active multibyte locale when it needs character boundaries or display
 * widths.  WISP uses UTF-8 as its internal byte representation and converts at
 * Win32 boundaries.  These helpers centralise the rules so Cyrillic and other
 * non-ASCII text behaves as characters, not as raw bytes.
 */

void wisp_unicode_init_process(void);

bool wisp_utf8_validate_n(const char *s, int len);
int  wisp_utf8_next_offset(const char *s, int len, int offset);
int  wisp_utf8_prev_offset(const char *s, int offset);
int  wisp_utf8_codepoint_width(uint32_t cp);
int  wisp_utf8_display_width_n(const char *s, int len);
int  wisp_utf8_display_width(const char *s);
int  wisp_utf8_display_width_skip_ansi(const char *s, int len);

char *wisp_bytes_to_utf8_for_terminal(const char *bytes, int len, int *out_len);
char *wisp_utf16_to_utf8_clipboard(const wchar_t *w, int *out_len);
wchar_t *wisp_utf8_to_utf16_clipboard(const char *s, int len, int *out_wchars);

#ifdef __cplusplus
}
#endif

#endif /* WISP_UNICODE_H */
