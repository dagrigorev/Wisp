#pragma once
/*
 * str_util.h — Heap-allocated string utilities and UTF-8 / UTF-16 conversion.
 *
 * All returned strings from functions that return char* or wchar_t* are
 * heap-allocated and must be freed by the caller with str_free() or
 * HeapFree(GetProcessHeap(), 0, ptr).
 *
 * OOP / SOLID:
 *   - Interface Segregation: only string operations here; no file I/O,
 *     no allocation policy (arena is separate).
 *   - Abstraction: callers use str_* functions, not raw Win32 Wide APIs.
 */
#ifndef WISP_STR_UTIL_H
#define WISP_STR_UTIL_H

#include <windows.h>
#include <stddef.h>
#include "wisp_bool.h"


#ifdef __cplusplus
extern "C" {
#endif

/* MSVC doesn't provide strncasecmp / strcasecmp. */
#ifndef strncasecmp
#  define strncasecmp _strnicmp
#endif
#ifndef strcasecmp
#  define strcasecmp  _stricmp
#endif

/* ── Heap string helpers ──────────────────────────────────────────────────── */

/* Duplicate a NUL-terminated string onto the heap.  Never returns NULL. */
char    *str_dup(const char *s);

/* Duplicate at most 'n' bytes (result is always NUL-terminated). */
char    *str_ndup(const char *s, size_t n);

/* Join argc strings with sep.  Caller frees the result. */
char    *str_join(char **parts, int count, const char *sep);

/* In-place right-trim of whitespace; returns s. */
char    *str_rtrim(char *s);

/* In-place trim (both ends); returns s. */
char    *str_trim(char *s);

/* True if s starts with prefix. */
bool     str_startswith(const char *s, const char *prefix);

/* Split src on delimiter; fills *parts (heap array of heap strings).
 * Returns count.  Free with str_split_free(). */
int      str_split(const char *src, char delim, char ***parts);
void     str_split_free(char **parts, int count);

/* ── UTF conversion ───────────────────────────────────────────────────────── */

/* UTF-8 → UTF-16 (heap). Returns NULL on error. */
wchar_t *u8_to_u16(const char *utf8, int *out_wlen);

/* UTF-16 → UTF-8 (heap). Returns NULL on error. */
char    *u16_to_u8(const wchar_t *utf16, int *out_len);

/* UTF-8 decode one codepoint from *p; advance *p; return codepoint
 * (or replacement char 0xFFFD on error). */
unsigned int utf8_decode(const char **p);

/* UTF-8 encode codepoint into buf[0..3]; returns byte count written. */
int          utf8_encode(unsigned int cp, char *buf);

/* Free a heap string (thin wrapper for readability). */
static inline void str_free(void *p) {
    if (p) HeapFree(GetProcessHeap(), 0, p);
}


#ifdef __cplusplus
}
#endif

#endif /* WISP_STR_UTIL_H */
