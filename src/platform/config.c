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
    wcscpy(cfg->font.family, L"Cascadia Code");
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

static void trim_inline(char *s) {
    /* Remove trailing comment and whitespace */
    char *p = s;
    bool in_str = false;
    while (*p) {
        if (*p == '"') in_str = !in_str;
        if (!in_str && *p == '#') { *p = '\0'; break; }
        p++;
    }
    /* Right-trim */
    p = s + strlen(s);
    while (p > s && (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\r' || p[-1] == '\n')) p--;
    *p = '\0';
}

static char *unquote(char *s) {
    if (!s) return s;
    size_t l = strlen(s);
    if (l >= 2 && s[0] == '"' && s[l-1] == '"') {
        s[l-1] = '\0';
        s++;

        /* Minimal TOML string unescaping.
         * Important for Windows paths: default_cwd = "C:\\work"
         * must become C:\work, not C:\\work. */
        char *src = s;
        char *dst = s;
        while (*src) {
            if (*src == '\\' && src[1]) {
                src++;
                switch (*src) {
                    case 'n':  *dst++ = '\n'; break;
                    case 'r':  *dst++ = '\r'; break;
                    case 't':  *dst++ = '\t'; break;
                    case '"': *dst++ = '"'; break;
                    case '\\': *dst++ = '\\'; break;
                    default:
                        *dst++ = *src;
                        break;
                }
                src++;
            } else {
                *dst++ = *src++;
            }
        }
        *dst = '\0';
        return s;
    }
    return s;
}

static void apply_kv(ParseCtx *ctx, const char *key, char *val) {
    Config *c = ctx->cfg;
    const char *sec = ctx->section;
    char *v = unquote(val);

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

    while (fgets(line, sizeof(line), f)) {
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
        trim_inline(val);

        apply_kv(&ctx, key, val);
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
