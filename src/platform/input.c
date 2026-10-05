#include <windows.h>
#include <string.h>
#include <stdint.h>
#include "wisp_bool.h"
#include "input.h"
#include "../core/str_util.h"

/* ─── Modifier helper ────────────────────────────────────────────────────── */

static bool is_ctrl()  { return (GetKeyState(VK_CONTROL) & 0x8000) != 0; }
static bool is_shift() { return (GetKeyState(VK_SHIFT)   & 0x8000) != 0; }
static bool is_alt()   { return (GetKeyState(VK_MENU)    & 0x8000) != 0; }

/* Xterm modifier parameter encoding: Shift=1, Alt=2, Ctrl=4, Meta=8 */
static int modifier_param(void) {
    int mod = 0;
    if (is_shift()) mod |= 1;
    if (is_alt())   mod |= 2;
    if (is_ctrl())  mod |= 4;
    return mod + 1; /* xterm adds 1 */
}

/* Build cursor key sequence; app_mode uses SS3 (ESC O) instead of CSI (ESC [) */
static int cursor_seq(char *buf, char letter, bool app_mode) {
    int mod = modifier_param();
    if (mod == 1) {
        /* No modifier */
        if (app_mode) {
            buf[0] = '\x1B'; buf[1] = 'O'; buf[2] = letter; return 3;
        } else {
            buf[0] = '\x1B'; buf[1] = '['; buf[2] = letter; return 3;
        }
    }
    /* With modifier: ESC [ 1 ; <mod> <letter> */
    int n = 0;
    buf[n++] = '\x1B'; buf[n++] = '['; buf[n++] = '1'; buf[n++] = ';';
    buf[n++] = (char)('0' + mod);
    buf[n++] = letter;
    return n;
}

/* Build tilde key sequence: ESC [ <num> ~ or ESC [ <num> ; <mod> ~ */
static int tilde_seq(char *buf, int num) {
    int mod = modifier_param();
    int n = 0;
    buf[n++] = '\x1B'; buf[n++] = '[';
    if (num >= 10) { buf[n++] = (char)('0' + num/10); }
    buf[n++] = (char)('0' + num % 10);
    if (mod != 1) { buf[n++] = ';'; buf[n++] = (char)('0' + mod); }
    buf[n++] = '~';
    return n;
}

/* ─── Function key sequences ─────────────────────────────────────────────── */

static int fkey_seq(char *buf, int fnum) {
    /* Standard xterm F-key sequences */
    static const int FKEY_NUMS[] = { 11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 23, 24 };
    /* F1–F4 use SS3 in some modes */
    if (fnum >= 1 && fnum <= 4) {
        buf[0] = '\x1B'; buf[1] = 'O'; buf[2] = (char)('O' + fnum); return 3;
    }
    if (fnum >= 5 && fnum <= 12) {
        return tilde_seq(buf, FKEY_NUMS[fnum - 1]);
    }
    return 0;
}

/* Pending UTF-16 high surrogate from WM_CHAR. */
static WCHAR g_pending_high_surrogate = 0;

/* ─── input_translate ────────────────────────────────────────────────────── */

InputEvent input_translate(WPARAM vk, WCHAR ch, LPARAM lParam, bool app_cursor_keys) {
    InputEvent ev = {0};
    (void)lParam;

    bool ctrl  = is_ctrl();
    bool shift = is_shift();
    bool alt   = is_alt();

    /* ── Special key combinations first ───────────────────────────────────── */

    /* Ctrl+Shift combos → terminal actions */
    if (ctrl && shift) {
        switch (vk) {
            case 'C': ev.action = INPUT_COPY;        return ev;
            case 'V': ev.action = INPUT_PASTE;       return ev;
            case 'T': ev.action = INPUT_NEW_TAB;     return ev;
            case 'W': ev.action = INPUT_CLOSE_TAB;   return ev;
            case VK_TAB: ev.action = INPUT_PREV_TAB; return ev;
            case VK_OEM_PLUS:  ev.action = INPUT_ZOOM_IN;  return ev;
            case VK_OEM_MINUS: ev.action = INPUT_ZOOM_OUT; return ev;
            case VK_UP:    ev.action = INPUT_SCROLL_UP;        return ev;
            case VK_DOWN:  ev.action = INPUT_SCROLL_DOWN;      return ev;
            case VK_PRIOR: ev.action = INPUT_SCROLL_PAGE_UP;   return ev;
            case VK_NEXT:  ev.action = INPUT_SCROLL_PAGE_DOWN; return ev;
            default: break;
        }
    }

    if (ctrl && !shift && vk == VK_TAB) {
        ev.action = INPUT_NEXT_TAB; return ev;
    }

    if (shift && !ctrl && (vk == VK_PRIOR || vk == VK_NEXT)) {
        ev.action = (vk == VK_PRIOR) ? INPUT_SCROLL_PAGE_UP : INPUT_SCROLL_PAGE_DOWN;
        return ev;
    }

    /* ── Navigation keys → VT sequences ──────────────────────────────────── */

    ev.action = INPUT_CHAR;

    switch (vk) {
        case VK_UP:     ev.len = cursor_seq(ev.bytes, 'A', app_cursor_keys); return ev;
        case VK_DOWN:   ev.len = cursor_seq(ev.bytes, 'B', app_cursor_keys); return ev;
        case VK_RIGHT:  ev.len = cursor_seq(ev.bytes, 'C', app_cursor_keys); return ev;
        case VK_LEFT:   ev.len = cursor_seq(ev.bytes, 'D', app_cursor_keys); return ev;
        case VK_HOME:   ev.len = tilde_seq(ev.bytes, 1);  return ev;
        case VK_END:    ev.len = tilde_seq(ev.bytes, 4);  return ev;
        case VK_INSERT: ev.len = tilde_seq(ev.bytes, 2);  return ev;
        case VK_DELETE: ev.len = tilde_seq(ev.bytes, 3);  return ev;
        case VK_PRIOR:  ev.len = tilde_seq(ev.bytes, 5);  return ev;   /* Page Up */
        case VK_NEXT:   ev.len = tilde_seq(ev.bytes, 6);  return ev;   /* Page Down */
        case VK_F1:  ev.len = fkey_seq(ev.bytes, 1);  return ev;
        case VK_F2:  ev.len = fkey_seq(ev.bytes, 2);  return ev;
        case VK_F3:  ev.len = fkey_seq(ev.bytes, 3);  return ev;
        case VK_F4:  ev.len = fkey_seq(ev.bytes, 4);  return ev;
        case VK_F5:  ev.len = fkey_seq(ev.bytes, 5);  return ev;
        case VK_F6:  ev.len = fkey_seq(ev.bytes, 6);  return ev;
        case VK_F7:  ev.len = fkey_seq(ev.bytes, 7);  return ev;
        case VK_F8:  ev.len = fkey_seq(ev.bytes, 8);  return ev;
        case VK_F9:  ev.len = fkey_seq(ev.bytes, 9);  return ev;
        case VK_F10: ev.len = fkey_seq(ev.bytes, 10); return ev;
        case VK_F11: ev.len = fkey_seq(ev.bytes, 11); return ev;
        case VK_F12: ev.len = fkey_seq(ev.bytes, 12); return ev;

        /* Backspace → DEL (0x7F) */
        case VK_BACK:
            if (ctrl) { ev.bytes[0] = '\x17'; ev.len = 1; } /* Ctrl+W */
            else       { ev.bytes[0] = '\x7F'; ev.len = 1; }
            return ev;

        /* Tab */
        case VK_TAB:
            ev.bytes[0] = '\t'; ev.len = 1;
            return ev;

        /* Enter */
        case VK_RETURN:
            ev.bytes[0] = '\r'; ev.len = 1;
            return ev;

        /* Escape */
        case VK_ESCAPE:
            ev.bytes[0] = '\x1B'; ev.len = 1;
            return ev;

        /* Ctrl+key combinations */
        default:
            if (ctrl && vk >= 'A' && vk <= 'Z') {
                ev.bytes[0] = (char)(vk - 'A' + 1); /* Ctrl+A = 0x01, etc. */
                ev.len = 1;
                return ev;
            }
            if (ctrl && vk == VK_OEM_4) { ev.bytes[0] = '\x1B'; ev.len = 1; return ev; } /* Ctrl+[ = ESC */
            if (ctrl && vk == VK_OEM_5) { ev.bytes[0] = '\x1C'; ev.len = 1; return ev; } /* Ctrl+\ */
            if (ctrl && vk == VK_OEM_6) { ev.bytes[0] = '\x1D'; ev.len = 1; return ev; } /* Ctrl+] */
            break;
    }

    /* ── Regular character (from WM_CHAR) ─────────────────────────────────── */

    if (ch == 0) { ev.action = INPUT_NONE; return ev; }

    /* Alt key: prefix with ESC */
    if (alt && !ctrl) {
        ev.bytes[0] = '\x1B';
        ev.len = 1;
    }

    /* Encode UTF-16 WM_CHAR input to UTF-8.  Cyrillic is one UTF-16 code unit;
     * surrogate pairs are combined so emoji/non-BMP input is not corrupted. */
    uint32_t cp = (uint32_t)ch;
    if (ch >= 0xD800 && ch <= 0xDBFF) {
        g_pending_high_surrogate = ch;
        ev.action = INPUT_NONE;
        return ev;
    }
    if (ch >= 0xDC00 && ch <= 0xDFFF) {
        if (g_pending_high_surrogate) {
            cp = 0x10000u + ((((uint32_t)g_pending_high_surrogate - 0xD800u) << 10)
                            | ((uint32_t)ch - 0xDC00u));
            g_pending_high_surrogate = 0;
        } else {
            cp = 0xFFFDu;
        }
    } else {
        g_pending_high_surrogate = 0;
    }

    char mbuf[4] = {0};
    int n = utf8_encode(cp, mbuf);
    for (int i = 0; i < n && ev.len < 31; i++) ev.bytes[ev.len++] = mbuf[i];
    ev.action = INPUT_CHAR;
    return ev;
}

/* ─── input_suppress_char ────────────────────────────────────────────────── */

bool input_suppress_char(WPARAM vk, LPARAM lParam) {
    (void)lParam;
    /* Suppress WM_CHAR for keys we handle entirely in WM_KEYDOWN */
    switch (vk) {
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
        case VK_HOME: case VK_END: case VK_INSERT: case VK_DELETE:
        case VK_PRIOR: case VK_NEXT:
        case VK_F1: case VK_F2: case VK_F3: case VK_F4:
        case VK_F5: case VK_F6: case VK_F7: case VK_F8:
        case VK_F9: case VK_F10: case VK_F11: case VK_F12:
        case VK_ESCAPE:
        case VK_BACK:
        case VK_RETURN:
        case VK_TAB:
            return true;
        default:
            /* Suppress Ctrl+letter when it's a control character we handle */
            if ((GetKeyState(VK_CONTROL) & 0x8000) && vk >= 'A' && vk <= 'Z')
                return true;
            return false;
    }
}
