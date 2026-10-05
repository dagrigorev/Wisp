#pragma once
#ifndef WISP_CONFIG_H
#define WISP_CONFIG_H

#include <windows.h>
#include "wisp_bool.h"
#include <stdint.h>
#include "../core/session.h"


#ifdef __cplusplus
extern "C" {
#endif

/* ─── Color palette (16 + special) ──────────────────────────────────────── */

typedef struct {
    uint32_t background;    /* ARGB */
    uint32_t foreground;
    uint32_t cursor;
    uint32_t selection;
    uint32_t ansi[16];      /* 0-7 normal, 8-15 bright */
} ColorPalette;

/* ─── Config Sections ────────────────────────────────────────────────────── */

typedef struct {
    char  shell[256];       /* empty = auto-detect, else path to exe */
    int   scrollback;
    bool  confirm_exit;
    char  bell[16];         /* "visual", "audio", "none" */
    char  default_cwd[MAX_PATH];
    char  theme[64];        /* built-in or user theme name */
    char  title[256];       /* window title template */
} ConfigGeneral;

typedef struct {
    wchar_t family[128];
    float   size;
    bool    ligatures;
    bool    bold_is_bright;
} ConfigFont;

typedef enum {
    CURSOR_BLOCK,
    CURSOR_BAR,
    CURSOR_UNDERLINE,
} CursorStyle;

typedef struct {
    CursorStyle style;
    bool        blink;
    int         blink_rate_ms;
} ConfigCursor;

typedef struct {
    char copy[32];
    char paste[32];
    char new_tab[32];
    char zoom_in[32];
    char zoom_out[32];
} ConfigKeybinds;

typedef struct {
    bool enabled;
    char position[8]; /* "top" or "bottom" */
    int  max_tabs;
} ConfigTabs;

typedef struct {
    bool enabled;
    int  width_px;
} ConfigScrollbar;

typedef struct {
    bool enabled;
    int  autosave_interval;
    int  max_scrollback_lines;
    bool prompt_on_restore;
} ConfigSession;

typedef struct {
    ConfigGeneral  general;
    ConfigFont     font;
    ConfigCursor   cursor;
    ColorPalette   colors;
    ConfigKeybinds keybinds;
    ConfigTabs     tabs;
    ConfigScrollbar scrollbar;
    ConfigSession  session;
} Config;

/* ─── API ────────────────────────────────────────────────────────────────── */

/* Load defaults into cfg, then parse file if it exists */
void config_defaults(Config *cfg);
bool config_load(Config *cfg, const char *toml_path);
bool config_save_defaults(const char *toml_path);

/* Apply colors from a theme TOML file. Theme file may contain [colors]. */
bool config_apply_theme_file(Config *cfg, const char *toml_path);

/* Resolve config file path: %APPDATA%\Wisp\Wisp.toml */
void config_path(char *out, int out_size);

/* Parse "#RRGGBB" hex color string; returns 0 on failure */
uint32_t config_parse_color(const char *hex);


#ifdef __cplusplus
}
#endif

#endif /* WISP_CONFIG_H */
