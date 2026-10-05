#include "unicode.h"
#include "str_util.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>

void wisp_unicode_init_process(void) {
    /*
     * zsh derives multibyte behaviour from the locale.  On Windows we make the
     * process contract explicit: UTF-8 inside WISP, UTF-16 at Win32 boundaries.
     * Console CP changes are harmless for the GUI build and help console tests
     * or tools launched from a console.
     */
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    if (GetEnvironmentVariableW(L"LANG", NULL, 0) == 0)
        SetEnvironmentVariableW(L"LANG", L"C.UTF-8");
    if (GetEnvironmentVariableW(L"LC_CTYPE", NULL, 0) == 0)
        SetEnvironmentVariableW(L"LC_CTYPE", L"C.UTF-8");

    WISP_LOG_DEBUG("Unicode mode initialized: internal=UTF-8, win32=UTF-16, consoleCP=UTF-8");
}

static bool decode_one(const unsigned char *s, int len, int *used, uint32_t *cp) {
    if (!s || len <= 0) return false;
    unsigned char b0 = s[0];
    if (b0 < 0x80) { *used = 1; *cp = b0; return true; }

    int need = 0;
    uint32_t v = 0;
    uint32_t minv = 0;
    if ((b0 & 0xE0) == 0xC0) { need = 2; v = b0 & 0x1F; minv = 0x80; }
    else if ((b0 & 0xF0) == 0xE0) { need = 3; v = b0 & 0x0F; minv = 0x800; }
    else if ((b0 & 0xF8) == 0xF0) { need = 4; v = b0 & 0x07; minv = 0x10000; }
    else return false;

    if (len < need) return false;
    for (int i = 1; i < need; ++i) {
        if ((s[i] & 0xC0) != 0x80) return false;
        v = (v << 6) | (uint32_t)(s[i] & 0x3F);
    }

    if (v < minv) return false;                 /* overlong */
    if (v >= 0xD800 && v <= 0xDFFF) return false; /* surrogate */
    if (v > 0x10FFFF) return false;

    *used = need;
    *cp = v;
    return true;
}

bool wisp_utf8_validate_n(const char *s, int len) {
    if (!s) return len == 0;
    if (len < 0) len = (int)strlen(s);
    int i = 0;
    while (i < len) {
        int used = 0; uint32_t cp = 0;
        if (!decode_one((const unsigned char *)s + i, len - i, &used, &cp)) return false;
        i += used;
    }
    return true;
}

int wisp_utf8_next_offset(const char *s, int len, int offset) {
    if (!s || len <= 0) return 0;
    if (offset < 0) offset = 0;
    if (offset >= len) return len;
    int used = 0; uint32_t cp = 0;
    if (!decode_one((const unsigned char *)s + offset, len - offset, &used, &cp)) return offset + 1;
    return offset + used;
}

int wisp_utf8_prev_offset(const char *s, int offset) {
    if (!s || offset <= 0) return 0;
    int p = offset - 1;
    while (p > 0 && (((unsigned char)s[p] & 0xC0) == 0x80)) p--;
    return p;
}

static bool in_range(uint32_t cp, uint32_t a, uint32_t b) { return cp >= a && cp <= b; }

int wisp_utf8_codepoint_width(uint32_t cp) {
    if (cp == 0) return 0;
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;

    /* Combining marks: zero columns. */
    if (in_range(cp, 0x0300, 0x036F) || in_range(cp, 0x1AB0, 0x1AFF) ||
        in_range(cp, 0x1DC0, 0x1DFF) || in_range(cp, 0x20D0, 0x20FF) ||
        in_range(cp, 0xFE20, 0xFE2F)) return 0;

    /* East Asian wide/full-width + emoji ranges. Cyrillic is intentionally NOT here. */
    if (in_range(cp, 0x1100, 0x115F) || in_range(cp, 0x2329, 0x232A) ||
        in_range(cp, 0x2E80, 0xA4CF) || in_range(cp, 0xAC00, 0xD7A3) ||
        in_range(cp, 0xF900, 0xFAFF) || in_range(cp, 0xFE10, 0xFE19) ||
        in_range(cp, 0xFE30, 0xFE6F) || in_range(cp, 0xFF00, 0xFF60) ||
        in_range(cp, 0xFFE0, 0xFFE6) || in_range(cp, 0x1F300, 0x1FAFF)) return 2;

    return 1;
}

int wisp_utf8_display_width_n(const char *s, int len) {
    if (!s) return 0;
    if (len < 0) len = (int)strlen(s);
    int width = 0;
    int i = 0;
    while (i < len) {
        int used = 0; uint32_t cp = 0;
        if (!decode_one((const unsigned char *)s + i, len - i, &used, &cp)) {
            width += 1;
            i += 1;
            continue;
        }
        width += wisp_utf8_codepoint_width(cp);
        i += used;
    }
    return width;
}

int wisp_utf8_display_width(const char *s) {
    return s ? wisp_utf8_display_width_n(s, (int)strlen(s)) : 0;
}

int wisp_utf8_display_width_skip_ansi(const char *s, int len) {
    if (!s) return 0;
    if (len < 0) len = (int)strlen(s);
    int width = 0;
    int i = 0;
    while (i < len) {
        if ((unsigned char)s[i] == 0x1B) {
            i++;
            if (i < len && s[i] == '[') {
                i++;
                while (i < len && !(s[i] >= 0x40 && s[i] <= 0x7E)) i++;
                if (i < len) i++;
            } else if (i < len && s[i] >= 0x40 && s[i] <= 0x5F) {
                i++;
            } else if (i < len && s[i] == ']') {
                i++;
                while (i < len && !(s[i] == 0x1B || s[i] == 0x07)) i++;
                if (i < len && s[i] == 0x1B) i++;
                if (i < len && s[i] == '\\') i++;
            } else if (i < len && s[i] >= 0x20 && s[i] <= 0x2F) {
                i++;
                while (i < len && s[i] >= 0x20 && s[i] <= 0x2F) i++;
                if (i < len && s[i] >= 0x40 && s[i] <= 0x7E) i++;
            }
            continue;
        }
        int used = 0; uint32_t cp = 0;
        if (!decode_one((const unsigned char *)s + i, len - i, &used, &cp)) {
            width += 1;
            i += 1;
            continue;
        }
        width += wisp_utf8_codepoint_width(cp);
        i += used;
    }
    return width;
}

static char *bytes_to_utf8_codepage(UINT cp, const char *bytes, int len, int *out_len) {
    if (!bytes) return str_dup("");
    if (len < 0) len = (int)strlen(bytes);
    if (len == 0) { if (out_len) *out_len = 0; return str_dup(""); }

    int wn = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, bytes, len, NULL, 0);
    if (wn <= 0) wn = MultiByteToWideChar(cp, 0, bytes, len, NULL, 0);
    if (wn <= 0) return NULL;

    wchar_t *w = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, (size_t)(wn + 1) * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(cp, 0, bytes, len, w, wn);
    w[wn] = 0;

    int n = WideCharToMultiByte(CP_UTF8, 0, w, wn, NULL, 0, NULL, NULL);
    if (n <= 0) { str_free(w); return NULL; }
    char *out = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)n + 1);
    if (!out) { str_free(w); return NULL; }
    WideCharToMultiByte(CP_UTF8, 0, w, wn, out, n, NULL, NULL);
    out[n] = '\0';
    str_free(w);
    if (out_len) *out_len = n;
    return out;
}

char *wisp_bytes_to_utf8_for_terminal(const char *bytes, int len, int *out_len) {
    if (!bytes) return str_dup("");
    if (len < 0) len = (int)strlen(bytes);
    if (wisp_utf8_validate_n(bytes, len)) {
        char *copy = str_ndup(bytes, (size_t)len);
        if (out_len) *out_len = len;
        return copy;
    }

    /* Windows console programs commonly write OEM bytes (CP866 for ru-RU). */
    char *oem = bytes_to_utf8_codepage(GetOEMCP(), bytes, len, out_len);
    if (oem && wisp_utf8_validate_n(oem, out_len ? *out_len : -1)) return oem;
    str_free(oem);

    char *acp = bytes_to_utf8_codepage(GetACP(), bytes, len, out_len);
    if (acp) return acp;

    /* Last-resort byte-preserving fallback. */
    if (out_len) *out_len = len;
    return str_ndup(bytes, (size_t)len);
}

char *wisp_utf16_to_utf8_clipboard(const wchar_t *w, int *out_len) {
    return u16_to_u8(w, out_len);
}

wchar_t *wisp_utf8_to_utf16_clipboard(const char *s, int len, int *out_wchars) {
    if (!s) return NULL;
    if (len < 0) return u8_to_u16(s, out_wchars);
    char *tmp = str_ndup(s, (size_t)len);
    if (!tmp) return NULL;
    wchar_t *w = u8_to_u16(tmp, out_wchars);
    str_free(tmp);
    return w;
}
