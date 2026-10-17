#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "config.h"
#include "../core/str_util.h"
#include "../core/log.h"

/* ─── Catppuccin Mocha Default Palette ───────────────────────────────────── */

static const uint32_t MOCHA_ANSI[16] = {
    0x45475A, /* black */
    0xF38BA8, /* red */
    0xA6E3A1, /* green */
    0xF9E2AF, /* yellow */
    0x89B4FA, /* blue */
    0xCBA6F7, /* magenta */
    0x94E2D5, /* cyan */
    0xBAC2DE, /* white */
    0x585B70, /* bright black */
    0xF38BA8, /* bright red */
    0xA6E3A1, /* bright green */
    0xF9E2AF, /* bright yellow */
    0x89B4FA, /* bright blue */
    0xCBA6F7, /* bright magenta */
    0x94E2D5, /* bright cyan */
    0xA6ADC8, /* bright white */
};

void config_defaults(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));

    /* General */
    cfg->general.shell[0] = 0;  /* empty = auto-detect; see resolve_shell() */
    cfg->general.scrollback    = 10000;
    cfg->general.confirm_exit  = true;
    strcpy(cfg->general.bell, "visual");
    strcpy(cfg->general.default_cwd, "~");
    strcpy(cfg->general.theme, "material-cyber-dark");
    strcpy(cfg->general.title, "Wisp - ${cwd}");

    /* Font */
    /* A Nerd Font by default: prompts such as oh-my-posh draw their
     * separators with private-use codepoints (U+E0B0 and neighbours), and
     * DirectWrite cannot fall back for those — no font claims the private use
     * area, so a font without them renders .notdef boxes. font.cpp falls back
     * to Cascadia Code and then Consolas if this one is not installed. */
    wcscpy(cfg->font.family, L"GeistMono NF");
    cfg->font.size          = 13.0f;
    cfg->font.ligatures     = true;
    cfg->font.bold_is_bright = true;

    /* Cursor */
    cfg->cursor.style        = CURSOR_BLOCK;
    cfg->cursor.blink        = true;
    cfg->cursor.blink_rate_ms = 530;

    /* Colors — Catppuccin Mocha */
    cfg->colors.background = 0x1E1E2E;
    cfg->colors.foreground = 0xCDD6F4;
    cfg->colors.cursor     = 0xF5E0DC;
    cfg->colors.selection  = 0x45475A;
    for (int i = 0; i < 16; i++) cfg->colors.ansi[i] = MOCHA_ANSI[i];

    /* Keybinds */
    strcpy(cfg->keybinds.copy,     "Ctrl+Shift+C");
    strcpy(cfg->keybinds.paste,    "Ctrl+Shift+V");
    strcpy(cfg->keybinds.new_tab,  "Ctrl+Shift+T");
    strcpy(cfg->keybinds.zoom_in,  "Ctrl+Shift+Equal");
    strcpy(cfg->keybinds.zoom_out, "Ctrl+Shift+Minus");

    /* Tabs */
    cfg->tabs.enabled  = true;
    strcpy(cfg->tabs.position, "top");
    cfg->tabs.max_tabs = 20;

    /* Scrollbar */
    cfg->scrollbar.enabled  = true;
    cfg->scrollbar.width_px = 8;
}

/* ─── Color parsing ──────────────────────────────────────────────────────── */

uint32_t config_parse_color(const char *hex) {
    if (!hex) return 0;
    if (*hex == '#') hex++;
    if (strlen(hex) != 6) return 0;
    char *end;
    unsigned long v = strtoul(hex, &end, 16);
    if (end != hex + 6) return 0;
    return (uint32_t)v;
}

/* ─── Minimal TOML Parser ────────────────────────────────────────────────── */

typedef struct {
    Config *cfg;
    char    section[64];
} ParseCtx;

/* Diagnostics for the most recent parse. Reset by config_parse_file(). */
static int  g_parse_error_count = 0;
static char g_parse_error_first[512] = {0};

static void record_parse_error(const char *path, int line_no,
                               const char *key, const char *why) {
    char msg[512];
    _snprintf(msg, sizeof(msg), "%s:%d: %s in value for key '%s'",
              path ? path : "(config)", line_no, why, key);
    msg[sizeof(msg)-1] = '\0';

    g_parse_error_count++;
    if (g_parse_error_count == 1) {
        strncpy(g_parse_error_first, msg, sizeof(g_parse_error_first)-1);
        g_parse_error_first[sizeof(g_parse_error_first)-1] = '\0';
    }
    WISP_LOG_ERROR("config: %s", msg);
}

int config_parse_error_count(void) { return g_parse_error_count; }

const char *config_parse_error_first(void) { return g_parse_error_first; }

static void trim_inline(char *s) {
    /* Remove trailing comment and whitespace. A '#' inside a quoted value is
     * data, not a comment, and inside a basic string a backslash escapes the
     * character after it — so "a\"# b" is one string, not a comment. */
    char *p = s;
    char quote = 0;
    while (*p) {
        if (quote == '"' && *p == '\\' && p[1]) { p += 2; continue; }
        if (quote) {
            if (*p == quote) quote = 0;
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if (*p == '#') {
            *p = '\0';
            break;
        }
        p++;
    }
    /* Right-trim */
    p = s + strlen(s);
    while (p > s && (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\r' || p[-1] == '\n')) p--;
    *p = '\0';
}

/* Read exactly n hex digits. Fails on a short run or a non-hex byte, so a
 * truncated escape at end of string is rejected rather than read past. */
static bool parse_hex(const char *src, int n, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        int d;
        char c = src[i];
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = v * 16 + (uint32_t)d;
    }
    *out = v;
    return true;
}

/* Interpret the escapes TOML defines for a basic string, in place.
 *
 * Any other escape is a parse error. Dropping the backslash instead — as this
 * used to — turns shell = "C:\Users\me\sh.exe" into C:Usersmesh.exe, and the
 * only symptom is a spawn failure naming a path nobody wrote. */
static bool unescape_basic(char *s, char *why, size_t why_size) {
    char *src = s;
    char *dst = s;
    while (*src) {
        if (*src != '\\') { *dst++ = *src++; continue; }
        src++;
        switch (*src) {
            case 'b':  *dst++ = '\b'; src++; break;
            case 't':  *dst++ = '\t'; src++; break;
            case 'n':  *dst++ = '\n'; src++; break;
            case 'f':  *dst++ = '\f'; src++; break;
            case 'r':  *dst++ = '\r'; src++; break;
            case '"':  *dst++ = '"';  src++; break;
            case '\\': *dst++ = '\\'; src++; break;
            case 'u':
            case 'U': {
                char     kind = *src;
                int      n    = (kind == 'u') ? 4 : 8;
                uint32_t cp;
                if (!parse_hex(src + 1, n, &cp)) {
                    _snprintf(why, why_size,
                              "escape \\%c needs %d hex digits", kind, n);
                    why[why_size-1] = '\0';
                    return false;
                }
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                    _snprintf(why, why_size,
                              "escape \\%c is not a Unicode scalar value (U+%04X)",
                              kind, (unsigned)cp);
                    why[why_size-1] = '\0';
                    return false;
                }
                /* Safe in place: the UTF-8 encoding is never longer than the
                 * escape it replaces (\uXXXX is 6 source bytes for at most 3
                 * output, \UXXXXXXXX is 10 for at most 4). */
                dst += utf8_encode((unsigned int)cp, dst);
                src += 1 + n;
                break;
            }
            case '\0':
                _snprintf(why, why_size, "trailing backslash escapes nothing");
                why[why_size-1] = '\0';
                return false;
            default:
                _snprintf(why, why_size, "unknown escape \\%c", *src);
                why[why_size-1] = '\0';
                return false;
        }
    }
    *dst = '\0';
    return true;
}

/* ─── Multi-line strings ─────────────────────────────────────────────────── */

/* A multi-line value is accumulated into a buffer of this size, NUL included.
 * Every destination field is smaller (shell[256], default_cwd[MAX_PATH],
 * title[256]) and apply_kv() truncates into it as it always has; this bound is
 * what keeps the accumulation itself inside its buffer. A longer value is a
 * reported parse error rather than a value quietly cut in half. */
#define MULTILINE_MAX 1024

/* Apply the line-ending backslash of a multi-line basic string, in place: an
 * unescaped backslash that ends a line removes the newline and all whitespace
 * that follows it. Runs before unescape_basic(), which has no case for a
 * backslash before a newline and would report it as an unknown escape. */
static void apply_line_ending_backslash(char *s) {
    char *src = s;
    char *dst = s;
    while (*src) {
        if (*src == '\\') {
            /* An escaped backslash is data: leave both bytes to
             * unescape_basic() so \\ at the end of a line is not taken for a
             * line-ending backslash. */
            if (src[1] == '\\') { *dst++ = *src++; *dst++ = *src++; continue; }
            const char *look = src + 1;
            while (*look == ' ' || *look == '\t' || *look == '\r') look++;
            if (*look == '\n') {
                src = (char *)look + 1;
                while (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n') src++;
                continue;
            }
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

/* Consume the body of a multi-line string whose opening delimiter was found on
 * the line just read.
 *
 * rest points just past that delimiter and delim is """ or '''. Further lines
 * are read from f until the closing delimiter, which TOML lets sit on a line of
 * its own; anything after it on that line is whitespace or a comment and is
 * ignored. The escapes of a basic string are left to the caller, so this reads
 * either flavour the same way.
 *
 * Returns false on overflow or end of file, with the reason in why; in both
 * cases the key is the caller's to skip. */
static bool read_multiline(FILE *f, const char *delim, const char *rest,
                           char *out, size_t out_size, int *line_no,
                           char *why, size_t why_size) {
    size_t len      = 0;
    bool   overflow = false;
    bool   first    = true;
    char   line[1024];

    for (;;) {
        const char *close = strstr(rest, delim);
        size_t      take  = close ? (size_t)(close - rest) : strlen(rest);

        /* A newline immediately after the opening delimiter is trimmed. Any
         * other text on that first line, whitespace included, is content. */
        bool trim_opening_newline = first && !close && take == 0;

        if (take > 0) {
            if (len + take + 1 > out_size) overflow = true;
            else { memcpy(out + len, rest, take); len += take; }
        }

        if (close) {
            if (overflow) {
                _snprintf(why, why_size,
                          "multi-line string longer than %d bytes",
                          (int)out_size - 1);
                why[why_size-1] = '\0';
                return false;
            }
            out[len] = '\0';
            return true;
        }

        /* The newline between two body lines is part of the value. */
        if (!trim_opening_newline) {
            if (len + 2 > out_size) overflow = true;
            else out[len++] = '\n';
        }
        first = false;

        if (!fgets(line, sizeof(line), f)) {
            _snprintf(why, why_size, "unterminated multi-line string");
            why[why_size-1] = '\0';
            return false;
        }
        (*line_no)++;
        /* fgets keeps the line ending; the body supplies its own newlines. */
        char *p = line + strlen(line);
        while (p > line && (p[-1] == '\r' || p[-1] == '\n')) p--;
        *p = '\0';
        rest = line;
    }
}

/* Unwrap a TOML value in place.
 *
 * Basic (double-quoted) strings have their escapes interpreted. Literal
 * (single-quoted) strings are passed through verbatim, which is the form a
 * Windows path should normally take: shell = 'C:\Users\me\sh.exe'. Anything
 * else — numbers, booleans — is returned untouched.
 *
 * Returns NULL on a parse error, with the reason in why. */
static char *unquote(char *s, char *why, size_t why_size) {
    if (!s) return s;
    size_t l = strlen(s);
    if (l >= 2 && s[0] == '"' && s[l-1] == '"') {
        s[l-1] = '\0';
        s++;
        if (!unescape_basic(s, why, why_size)) return NULL;
        return s;
    }
    if (l >= 2 && s[0] == '\'' && s[l-1] == '\'') {
        s[l-1] = '\0';
        return s + 1;
    }
    return s;
}

static void apply_kv(ParseCtx *ctx, const char *key, const char *v) {
    Config *c = ctx->cfg;
    const char *sec = ctx->section;

    if (strcmp(sec, "general") == 0) {
        if (!strcmp(key, "shell"))         strncpy(c->general.shell,       v, sizeof(c->general.shell)-1);
        else if (!strcmp(key, "scrollback"))   c->general.scrollback    = atoi(v);
        else if (!strcmp(key, "confirm_exit")) c->general.confirm_exit  = !strcmp(v,"true");
        else if (!strcmp(key, "bell"))         strncpy(c->general.bell, v, sizeof(c->general.bell)-1);
        else if (!strcmp(key, "default_cwd"))  strncpy(c->general.default_cwd, v, sizeof(c->general.default_cwd)-1);
        else if (!strcmp(key, "theme"))        strncpy(c->general.theme, v, sizeof(c->general.theme)-1);
        else if (!strcmp(key, "title"))        strncpy(c->general.title, v, sizeof(c->general.title)-1);
    } else if (strcmp(sec, "font") == 0) {
        if (!strcmp(key, "family")) {
            wchar_t *w = u8_to_u16(v, NULL);
            if (w) { wcsncpy(c->font.family, w, 127); HeapFree(GetProcessHeap(),0,w); }
        } else if (!strcmp(key, "size"))           c->font.size           = (float)atof(v);
        else if (!strcmp(key, "ligatures"))        c->font.ligatures       = !strcmp(v,"true");
        else if (!strcmp(key, "bold_is_bright"))   c->font.bold_is_bright  = !strcmp(v,"true");
    } else if (strcmp(sec, "cursor") == 0) {
        if (!strcmp(key, "style")) {
            if (!strcmp(v,"bar"))        c->cursor.style = CURSOR_BAR;
            else if (!strcmp(v,"underline")) c->cursor.style = CURSOR_UNDERLINE;
            else                         c->cursor.style = CURSOR_BLOCK;
        } else if (!strcmp(key, "blink"))       c->cursor.blink        = !strcmp(v,"true");
        else if (!strcmp(key, "blink_rate_ms")) c->cursor.blink_rate_ms = atoi(v);
    } else if (strcmp(sec, "colors") == 0) {
        uint32_t col = config_parse_color(v);
        if      (!strcmp(key,"background"))    c->colors.background  = col;
        else if (!strcmp(key,"foreground"))    c->colors.foreground  = col;
        else if (!strcmp(key,"cursor"))        c->colors.cursor      = col;
        else if (!strcmp(key,"selection"))     c->colors.selection   = col;
        else if (!strcmp(key,"black"))         c->colors.ansi[0]     = col;
        else if (!strcmp(key,"red"))           c->colors.ansi[1]     = col;
        else if (!strcmp(key,"green"))         c->colors.ansi[2]     = col;
        else if (!strcmp(key,"yellow"))        c->colors.ansi[3]     = col;
        else if (!strcmp(key,"blue"))          c->colors.ansi[4]     = col;
        else if (!strcmp(key,"magenta"))       c->colors.ansi[5]     = col;
        else if (!strcmp(key,"cyan"))          c->colors.ansi[6]     = col;
        else if (!strcmp(key,"white"))         c->colors.ansi[7]     = col;
        else if (!strcmp(key,"bright_black"))  c->colors.ansi[8]     = col;
        else if (!strcmp(key,"bright_red"))    c->colors.ansi[9]     = col;
        else if (!strcmp(key,"bright_green"))  c->colors.ansi[10]    = col;
        else if (!strcmp(key,"bright_yellow")) c->colors.ansi[11]    = col;
        else if (!strcmp(key,"bright_blue"))   c->colors.ansi[12]    = col;
        else if (!strcmp(key,"bright_magenta"))c->colors.ansi[13]    = col;
        else if (!strcmp(key,"bright_cyan"))   c->colors.ansi[14]    = col;
        else if (!strcmp(key,"bright_white"))  c->colors.ansi[15]    = col;
    } else if (strcmp(sec, "keybinds") == 0) {
        if      (!strcmp(key,"copy"))     strncpy(c->keybinds.copy,     v, 31);
        else if (!strcmp(key,"paste"))    strncpy(c->keybinds.paste,    v, 31);
        else if (!strcmp(key,"new_tab"))  strncpy(c->keybinds.new_tab,  v, 31);
        else if (!strcmp(key,"zoom_in"))  strncpy(c->keybinds.zoom_in,  v, 31);
        else if (!strcmp(key,"zoom_out")) strncpy(c->keybinds.zoom_out, v, 31);
    } else if (strcmp(sec, "tabs") == 0) {
        if      (!strcmp(key,"enabled"))   c->tabs.enabled   = !strcmp(v,"true");
        else if (!strcmp(key,"position"))  strncpy(c->tabs.position, v, 7);
        else if (!strcmp(key,"max_tabs"))  c->tabs.max_tabs  = atoi(v);
    } else if (strcmp(sec, "scrollbar") == 0) {
        if      (!strcmp(key,"enabled"))   c->scrollbar.enabled  = !strcmp(v,"true");
        else if (!strcmp(key,"width_px"))  c->scrollbar.width_px = atoi(v);
    }
}

static bool config_parse_file(Config *cfg, const char *toml_path) {
    FILE *f = fopen(toml_path, "r");
    if (!f) return false;

    ParseCtx ctx = { .cfg = cfg, .section = "" };
    char line[1024];
    int  line_no = 0;

    g_parse_error_count = 0;
    g_parse_error_first[0] = '\0';

    while (fgets(line, sizeof(line), f)) {
        line_no++;
        /* Strip line ending */
        char *p = line + strlen(line);
        while (p > line && (p[-1] == '\r' || p[-1] == '\n')) p--;
        *p = '\0';

        /* Trim leading whitespace */
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;

        /* Skip comments and empty lines */
        if (!*s || *s == '#') continue;

        /* Section header */
        if (*s == '[') {
            s++;
            char *end = strchr(s, ']');
            if (end) { *end = '\0'; strncpy(ctx.section, s, sizeof(ctx.section)-1); }
            continue;
        }

        /* key = value */
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = s;
        char *val = eq + 1;

        /* Trim key and value */
        p = key + strlen(key);
        while (p > key && (p[-1] == ' ' || p[-1] == '\t')) { p--; *p = '\0'; }
        while (*val == ' ' || *val == '\t') val++;

        /* A value that does not parse is reported and skipped: the key keeps
         * whatever it had, rather than silently taking a mangled value. */
        char why[128];
        char *v;

        /* A value opened with """ or ''' runs on over the following lines, so
         * it is read whole before it is unescaped — and never passed through
         * trim_inline(), since a '#' anywhere in the body is data. */
        const char *delim = NULL;
        if      (!strncmp(val, "\"\"\"", 3)) delim = "\"\"\"";
        else if (!strncmp(val, "'''",    3)) delim = "'''";

        char body[MULTILINE_MAX];
        if (delim) {
            if (!read_multiline(f, delim, val + 3, body, sizeof(body),
                                &line_no, why, sizeof(why))) {
                record_parse_error(toml_path, line_no, key, why);
                continue;
            }
            /* A multi-line basic string takes the same escapes as a
             * single-line one, plus the line-ending backslash. A multi-line
             * literal string is verbatim. */
            if (*delim == '"') {
                apply_line_ending_backslash(body);
                if (!unescape_basic(body, why, sizeof(why))) {
                    record_parse_error(toml_path, line_no, key, why);
                    continue;
                }
            }
            v = body;
        } else {
            trim_inline(val);
            v = unquote(val, why, sizeof(why));
            if (!v) {
                record_parse_error(toml_path, line_no, key, why);
                continue;
            }
        }

        apply_kv(&ctx, key, v);
    }
    fclose(f);
    return true;
}

bool config_load(Config *cfg, const char *toml_path) {
    return config_parse_file(cfg, toml_path);
}

bool config_apply_theme_file(Config *cfg, const char *toml_path) {
    if (!cfg || !toml_path || !*toml_path) return false;

    Config themed = *cfg;
    if (!config_parse_file(&themed, toml_path)) return false;
    cfg->colors = themed.colors;
    return true;
}

/* ─── Path Resolution ────────────────────────────────────────────────────── */

void config_path(char *out, int out_size) {
    char appdata[MAX_PATH] = {0};
    GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    _snprintf(out, out_size, "%s\\Wisp\\Wisp.toml", appdata);
}

bool config_save_defaults(const char *toml_path) {
    /* Ensure directory exists */
    char dir[MAX_PATH];
    strncpy(dir, toml_path, MAX_PATH-1);
    char *bs = strrchr(dir, '\\');
    if (bs) { *bs = '\0'; CreateDirectoryA(dir, NULL); }

    FILE *f = fopen(toml_path, "w");
    if (!f) return false;

    fprintf(f,
        "[general]\n"
        "shell = \"\"\n"
        "scrollback = 10000\n"
        "confirm_exit = true\n"
        "bell = \"visual\"\n"
        "default_cwd = \"~\"\n"
        "theme = \"material-cyber-dark\"\n"
        "title = \"Wisp - ${cwd}\"\n\n"
        "[font]\n"
        "family = \"Cascadia Code\"\n"
        "size = 13.0\n"
        "ligatures = true\n"
        "bold_is_bright = true\n\n"
        "[cursor]\n"
        "style = \"block\"\n"
        "blink = true\n"
        "blink_rate_ms = 530\n\n"
        "[colors]\n"
        "background  = \"#1e1e2e\"\n"
        "foreground  = \"#cdd6f4\"\n"
        "cursor      = \"#f5e0dc\"\n"
        "selection   = \"#45475a\"\n"
        "black       = \"#45475a\"\n"
        "red         = \"#f38ba8\"\n"
        "green       = \"#a6e3a1\"\n"
        "yellow      = \"#f9e2af\"\n"
        "blue        = \"#89b4fa\"\n"
        "magenta     = \"#cba6f7\"\n"
        "cyan        = \"#94e2d5\"\n"
        "white       = \"#bac2de\"\n"
        "bright_black   = \"#585b70\"\n"
        "bright_red     = \"#f38ba8\"\n"
        "bright_green   = \"#a6e3a1\"\n"
        "bright_yellow  = \"#f9e2af\"\n"
        "bright_blue    = \"#89b4fa\"\n"
        "bright_magenta = \"#cba6f7\"\n"
        "bright_cyan    = \"#94e2d5\"\n"
        "bright_white   = \"#a6adc8\"\n\n"
        "[keybinds]\n"
        "copy  = \"Ctrl+Shift+C\"\n"
        "paste = \"Ctrl+Shift+V\"\n"
        "new_tab = \"Ctrl+Shift+T\"\n"
        "zoom_in = \"Ctrl+Shift+Equal\"\n"
        "zoom_out = \"Ctrl+Shift+Minus\"\n\n"
        "[tabs]\n"
        "enabled = true\n"
        "position = \"top\"\n"
        "max_tabs = 20\n\n"
        "[scrollbar]\n"
        "enabled = true\n"
        "width_px = 8\n"
    );
    fclose(f);
    return true;
}
