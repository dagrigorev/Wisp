/* ─────────────────────────────────────────────────────────────────────────────
 * Wisp — a GPU-accelerated terminal emulator for Windows.
 *
 * Wisp is a terminal emulator and nothing else. It does not implement a shell;
 * it hosts one (cmd.exe, PowerShell, WSL, …) over ConPTY and renders what that
 * program writes. Everything on screen is the hosted program's output plus the
 * minimum chrome needed to manage tabs and splits.
 * ───────────────────────────────────────────────────────────────────────────── */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellscalingapi.h>
#include <stdio.h>
#include <string.h>
#include <exception>

#include "core/str_util.h"
#include "core/log.h"
#include "core/unicode.h"
#include "platform/config.h"
#include "platform/pty.h"
#include "platform/input.h"
#include "terminal/vt_pane.hpp"
#include "terminal/renderer.h"
#include "terminal/layout.h"
#include "window.h"

/* ─── Limits ─────────────────────────────────────────────────────────────── */

#define WISP_MAX_TABS   16
#define WISP_MAX_PANES   4

#define TIMER_BLINK      1
#define TIMER_TITLE      2

#define MIN_FONT_PT      6.0f
#define MAX_FONT_PT     72.0f

/* ─── Model ──────────────────────────────────────────────────────────────── */

typedef struct {
    wisp::terminal::VtPane term;   /* the ported terminal and its view */
    PtySession   pty;
    RECT         rect;        /* pixel rect inside the client area */
    bool         initialized;
    int          wheel_accum;
} Pane;

typedef enum {
    SPLIT_NONE,
    SPLIT_VERTICAL,    /* panes side by side */
    SPLIT_HORIZONTAL,  /* panes stacked */
} SplitDirection;

typedef struct {
    Pane           panes[WISP_MAX_PANES];
    int            pane_count;
    int            active_pane;
    SplitDirection split;
    bool           initialized;
} Tab;

static HWND             g_hwnd = NULL;
static Config           g_cfg = {0};
static Renderer         g_renderer = {0};
static Tab              g_tabs[WISP_MAX_TABS];
static int              g_tab_count = 0;
static int              g_active_tab = 0;
static CRITICAL_SECTION g_lock;
static bool             g_suppress_char = false;
static float            g_font_pt = 0.0f;
static RECT             g_tab_rects[WISP_MAX_TABS];

static void layout_panes(void);
static void sync_window_title(void);

static D2D1_COLOR_F to_d2d(Color4F c) {
    return D2D1::ColorF(c.r, c.g, c.b, c.a);
}

template <typename T> static T dpi_scale(T v) {
    return (T)(v * g_renderer.dpi / 96.0f + 0.5f);
}

/* Resolve general.theme: an explicit path, else themes/<name>.toml next to the
   executable, else the per-user themes directory. */
static void apply_configured_theme(Config *cfg) {
    if (!cfg || !cfg->general.theme[0]) return;

    if (strchr(cfg->general.theme, '\\') || strchr(cfg->general.theme, '/') ||
        strchr(cfg->general.theme, ':')) {
        if (!config_apply_theme_file(cfg, cfg->general.theme))
            WISP_LOG_WARN("Theme file not found: %s", cfg->general.theme);
        return;
    }

    char exe_path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    char *slash = strrchr(exe_path, '\\');
    if (slash) *slash = '\0';

    char theme_path[MAX_PATH];
    _snprintf(theme_path, MAX_PATH, "%s\\themes\\%s.toml", exe_path, cfg->general.theme);
    if (config_apply_theme_file(cfg, theme_path)) return;

    char appdata[MAX_PATH] = {0};
    GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    _snprintf(theme_path, MAX_PATH, "%s\\Wisp\\themes\\%s.toml", appdata, cfg->general.theme);
    if (!config_apply_theme_file(cfg, theme_path))
        WISP_LOG_WARN("Configured theme was not found: %s", cfg->general.theme);
}

/* Height of the tab strip. Ghostty-style: no strip at all for a single tab. */
static int tab_bar_height(void) {
    if (!g_cfg.tabs.enabled || g_tab_count <= 1) return 0;
    return dpi_scale(32);
}

static Tab *active_tab(void) {
    if (g_active_tab < 0 || g_active_tab >= g_tab_count) return NULL;
    Tab *t = &g_tabs[g_active_tab];
    return t->initialized ? t : NULL;
}

static Pane *active_pane(void) {
    Tab *t = active_tab();
    if (!t) return NULL;
    if (t->active_pane < 0 || t->active_pane >= t->pane_count) return NULL;
    Pane *p = &t->panes[t->active_pane];
    return p->initialized ? p : NULL;
}

/* ─── PTY plumbing ───────────────────────────────────────────────────────── */

/* The terminal's replies to the hosted program: device attributes, cursor
 * position reports, XTGETTCAP answers and the like. Called from inside a feed,
 * so g_lock is already held. */
static void pane_pty_reply(void *ctx, const char *data, size_t len) {
    Pane *pane = (Pane *)ctx;
    if (!pane || !pane->initialized || len == 0) return;
    pty_write(&pane->pty, data, (int)len);
}

static void on_pty_data(const char *buf, int len, void *ud) {
    Pane *pane = (Pane *)ud;
    if (!pane) return;
    EnterCriticalSection(&g_lock);
    if (pane->initialized) pane->term.feed(buf, (size_t)len);
    LeaveCriticalSection(&g_lock);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* Resolve general.default_cwd into a real directory for the hosted program.
   "~" is expanded to the user profile, and a directory that does not exist is
   reported as NULL so ConPTY inherits our own working directory instead of
   failing the spawn outright. */
static const wchar_t *resolve_start_dir(wchar_t *buf, size_t cap) {
    const char *cfg_cwd = g_cfg.general.default_cwd;
    if (!cfg_cwd[0]) return NULL;

    if (cfg_cwd[0] == '~' && (cfg_cwd[1] == '\0' || cfg_cwd[1] == '\\' || cfg_cwd[1] == '/')) {
        wchar_t home[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return NULL;
        _snwprintf(buf, cap - 1, L"%s%S", home, cfg_cwd + 1);
        buf[cap - 1] = L'\0';
    } else {
        int wlen = 0;
        wchar_t *w = u8_to_u16(cfg_cwd, &wlen);
        if (!w) return NULL;
        wcsncpy(buf, w, cap - 1);
        buf[cap - 1] = L'\0';
        str_free(w);
    }

    DWORD attr = GetFileAttributesW(buf);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        WISP_LOG_WARN("default_cwd is not a directory, falling back: %s", cfg_cwd);
        return NULL;
    }
    return buf;
}

/* True if cmdline names this very executable. Hosting ourselves would spawn a
   Wisp that spawns a Wisp — a fork bomb — so it is refused outright. */
static bool is_self(const wchar_t *cmdline) {
    wchar_t self[MAX_PATH];
    if (!GetModuleFileNameW(NULL, self, MAX_PATH)) return false;

    const wchar_t *self_base = wcsrchr(self, L'\\');
    self_base = self_base ? self_base + 1 : self;

    const wchar_t *base = wcsrchr(cmdline, L'\\');
    base = base ? base + 1 : cmdline;

    if (_wcsicmp(base, self_base) == 0) return true;
    /* Also catch the extension-less spelling, e.g. "wisp". */
    size_t n = wcslen(base);
    return n && _wcsnicmp(base, self_base, n) == 0 &&
           _wcsicmp(self_base + n, L".exe") == 0;
}

/* Resolve the command line for the hosted program. An empty general.shell
   means auto-detect: PowerShell 7, then Windows PowerShell, then COMSPEC. */
static const wchar_t *shell_command_line(wchar_t *buf, size_t cap) {
    if (g_cfg.general.shell[0]) {
        int wlen = 0;
        wchar_t *w = u8_to_u16(g_cfg.general.shell, &wlen);
        if (w) {
            wcsncpy(buf, w, cap - 1);
            buf[cap - 1] = L'\0';
            str_free(w);
            if (!is_self(buf)) return buf;
            WISP_LOG_WARN("general.shell points at Wisp itself, ignoring it");
        }
    }

    static const wchar_t *candidates[] = {L"pwsh.exe", L"powershell.exe"};
    for (int i = 0; i < 2; ++i) {
        wchar_t found[MAX_PATH];
        if (SearchPathW(NULL, candidates[i], NULL, MAX_PATH, found, NULL)) {
            wcsncpy(buf, found, cap - 1);
            buf[cap - 1] = L'\0';
            return buf;
        }
    }

    wchar_t comspec[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"COMSPEC", comspec, MAX_PATH);
    if (n > 0 && n < MAX_PATH && !is_self(comspec)) {
        wcsncpy(buf, comspec, cap - 1);
        buf[cap - 1] = L'\0';
        return buf;
    }

    wcsncpy(buf, L"cmd.exe", cap - 1);
    buf[cap - 1] = L'\0';
    return buf;
}

static bool pane_start(Pane *pane) {
    if (!pane) return false;

    /* Reset the plain fields. The terminal is not memset: it owns heap state
     * (pin pools, pages) that init/deinit manage, and clearing its bytes
     * underneath would leave those pointers dangling. */
    pane->initialized = false;
    pane->wheel_accum = 0;
    const RECT rc = pane->rect;

    int cols = g_renderer.cols > 0 ? g_renderer.cols : 80;
    int rows = g_renderer.rows > 0 ? g_renderer.rows : 24;
    if (rc.right > rc.left && rc.bottom > rc.top && g_renderer.cell_w > 0.0f) {
        cols = (int)((rc.right - rc.left) / g_renderer.cell_w);
        rows = (int)((rc.bottom - rc.top) / g_renderer.cell_h);
    }
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    /* The scrollback config is in lines; the port budgets it in bytes.
     * A row costs roughly one cell per column plus row overhead, so this is
     * the same order of magnitude as the old per-line ring. */
    const size_t scrollback_bytes =
        (size_t)(g_cfg.general.scrollback > 0 ? g_cfg.general.scrollback : 0) *
        (size_t)cols * sizeof(wisp::vt::page::Cell);
    if (!pane->term.init((unsigned)cols, (unsigned)rows, scrollback_bytes)) {
        WISP_LOG_ERROR("terminal init failed");
        return false;
    }
    pane->term.ctx = pane;
    pane->term.write_fn = &pane_pty_reply;

    if (!pty_create(&pane->pty, cols, rows)) {
        WISP_LOG_ERROR("pty_create failed");
        pane->term.deinit();
        return false;
    }

    wchar_t cmd[512];
    wchar_t cwd[MAX_PATH];
    const wchar_t *wcwd = resolve_start_dir(cwd, MAX_PATH);

    if (!pty_spawn(&pane->pty, shell_command_line(cmd, 512), wcwd, on_pty_data, pane)) {
        WISP_LOG_ERROR("pty_spawn failed");
        pty_close(&pane->pty);
        pane->term.deinit();
        return false;
    }

    pane->initialized = true;
    return true;
}

static void pane_stop(Pane *pane) {
    if (!pane || !pane->initialized) return;
    EnterCriticalSection(&g_lock);
    pane->initialized = false;
    LeaveCriticalSection(&g_lock);
    pty_close(&pane->pty);
    pane->term.deinit();
}

static void pane_write(Pane *pane, const char *bytes, int len) {
    if (!pane || !pane->initialized || len <= 0) return;
    pane->term.resetViewport();
    pty_write(&pane->pty, bytes, len);
}

/* ─── Tabs ───────────────────────────────────────────────────────────────── */

static bool tab_open(void) {
    if (g_tab_count >= WISP_MAX_TABS) return false;
    Tab *t = &g_tabs[g_tab_count];
    memset(t, 0, sizeof(*t));
    t->pane_count = 1;
    t->active_pane = 0;
    t->split = SPLIT_NONE;
    t->initialized = true;
    g_tab_count++;
    g_active_tab = g_tab_count - 1;
    layout_panes();
    if (!pane_start(&t->panes[0])) {
        t->initialized = false;
        g_tab_count--;
        if (g_active_tab >= g_tab_count) g_active_tab = g_tab_count - 1;
        return false;
    }
    layout_panes();
    sync_window_title();
    return true;
}

static void tab_close(int index) {
    if (index < 0 || index >= g_tab_count) return;
    Tab *t = &g_tabs[index];
    for (int i = 0; i < t->pane_count; ++i) pane_stop(&t->panes[i]);
    t->initialized = false;

    for (int i = index; i < g_tab_count - 1; ++i) g_tabs[i] = g_tabs[i + 1];
    g_tab_count--;

    if (g_tab_count == 0) {
        PostMessage(g_hwnd, WM_CLOSE, 0, 0);
        return;
    }
    if (g_active_tab >= g_tab_count) g_active_tab = g_tab_count - 1;
    layout_panes();
    sync_window_title();
}

static void pane_split(SplitDirection dir) {
    Tab *t = active_tab();
    if (!t || t->pane_count >= WISP_MAX_PANES) return;
    if (t->split == SPLIT_NONE) t->split = dir;
    Pane *np = &t->panes[t->pane_count];
    memset(np, 0, sizeof(*np));
    t->pane_count++;
    layout_panes();
    if (!pane_start(np)) {
        t->pane_count--;
        return;
    }
    t->active_pane = t->pane_count - 1;
    layout_panes();
}

static void pane_close_active(void) {
    Tab *t = active_tab();
    if (!t) return;
    if (t->pane_count <= 1) { tab_close(g_active_tab); return; }

    int idx = t->active_pane;
    pane_stop(&t->panes[idx]);
    for (int i = idx; i < t->pane_count - 1; ++i) t->panes[i] = t->panes[i + 1];
    t->pane_count--;
    if (t->active_pane >= t->pane_count) t->active_pane = t->pane_count - 1;
    if (t->pane_count == 1) t->split = SPLIT_NONE;
    layout_panes();
}

/* ─── Layout ─────────────────────────────────────────────────────────────── */

static void pane_apply_rect(Pane *p, RECT rc) {
    p->rect = rc;
    if (!p->initialized || g_renderer.cell_w <= 0.0f) return;

    int cols = (int)((rc.right - rc.left) / g_renderer.cell_w);
    int rows = (int)((rc.bottom - rc.top) / g_renderer.cell_h);
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols == p->term.view.cols && rows == p->term.view.rows) return;

    EnterCriticalSection(&g_lock);
    (void)p->term.resize((unsigned)cols, (unsigned)rows);
    LeaveCriticalSection(&g_lock);
    pty_resize(&p->pty, cols, rows);
}

static void layout_panes(void) {
    Tab *t = active_tab();
    if (!t || !g_hwnd) return;

    RECT client;
    GetClientRect(g_hwnd, &client);

    RECT area = {client.left, client.top + tab_bar_height(), client.right, client.bottom};
    if (area.right <= area.left || area.bottom <= area.top) return;

    int n = t->pane_count;
    int gap = n > 1 ? dpi_scale(1) : 0;

    for (int i = 0; i < n; ++i) {
        RECT rc = area;
        if (n > 1 && t->split == SPLIT_VERTICAL) {
            int w = (area.right - area.left - gap * (n - 1)) / n;
            rc.left  = area.left + i * (w + gap);
            rc.right = rc.left + w;
        } else if (n > 1) {
            int h = (area.bottom - area.top - gap * (n - 1)) / n;
            rc.top    = area.top + i * (h + gap);
            rc.bottom = rc.top + h;
        }
        pane_apply_rect(&t->panes[i], rc);
    }
}

/* ─── Chrome ─────────────────────────────────────────────────────────────── */

static void draw_tab_bar(void) {
    int h = tab_bar_height();
    if (h <= 0) return;

    RECT client;
    GetClientRect(g_hwnd, &client);

    ID2D1RenderTarget *rt = g_renderer.render_target;
    D2D1_COLOR_F bg = to_d2d(g_renderer.bg_color);

    /* The strip sits slightly behind the terminal background so the active tab,
       which is painted in the terminal background, reads as connected to it. */
    D2D1_COLOR_F strip = bg;
    strip.r *= 0.82f; strip.g *= 0.82f; strip.b *= 0.82f;

    g_renderer.bg_brush->SetColor(strip);
    D2D1_RECT_F strip_rc = {0.0f, 0.0f, (float)client.right, (float)h};
    rt->FillRectangle(strip_rc, g_renderer.bg_brush);

    int max_w = dpi_scale(220);
    int min_w = dpi_scale(90);
    int w = client.right / (g_tab_count ? g_tab_count : 1);
    if (w > max_w) w = max_w;
    if (w < min_w) w = min_w;

    for (int i = 0; i < g_tab_count; ++i) {
        RECT tr = {i * w, 0, (i + 1) * w, h};
        g_tab_rects[i] = tr;

        bool is_active = (i == g_active_tab);
        if (is_active) {
            g_renderer.bg_brush->SetColor(bg);
            D2D1_RECT_F rc = {(float)tr.left, 0.0f, (float)tr.right, (float)h};
            rt->FillRectangle(rc, g_renderer.bg_brush);
        }

        const char *title = "shell";
        Tab *t = &g_tabs[i];
        if (t->initialized && t->pane_count > 0 &&
            t->panes[t->active_pane].term.view.title[0])
            title = t->panes[t->active_pane].term.view.title;

        wchar_t label[128];
        int wlen = 0;
        wchar_t *w16 = u8_to_u16(title, &wlen);
        if (w16) {
            _snwprintf(label, 127, L"%s", w16);
            label[127] = L'\0';
            str_free(w16);
        } else {
            wcscpy(label, L"shell");
        }

        D2D1_COLOR_F fg = to_d2d(g_renderer.fg_color);
        if (!is_active) fg.a = 0.55f;
        g_renderer.fg_brush->SetColor(fg);

        if (g_renderer.font.factory && g_renderer.font.fmt_normal) {
            IDWriteTextLayout *layout = NULL;
            float text_w = (float)(tr.right - tr.left - dpi_scale(22));
            g_renderer.font.factory->CreateTextLayout(
                label, (UINT32)wcslen(label), g_renderer.font.fmt_normal,
                text_w > 0.0f ? text_w : 1.0f, (float)h, &layout);
            if (layout) {
                D2D1_POINT_2F orig = {(float)(tr.left + dpi_scale(12)), (float)dpi_scale(7)};
                rt->DrawTextLayout(orig, layout, g_renderer.fg_brush,
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
                layout->Release();
            }
        }
    }
}

static void sync_window_title(void) {
    if (!g_hwnd) return;
    Pane *p = active_pane();

    char buf[320];
    if (p && p->term.view.title[0]) _snprintf(buf, sizeof(buf), "%s — Wisp", p->term.view.title);
    else                         _snprintf(buf, sizeof(buf), "Wisp");
    buf[sizeof(buf) - 1] = '\0';

    int wlen = 0;
    wchar_t *w = u8_to_u16(buf, &wlen);
    if (w) { SetWindowTextW(g_hwnd, w); str_free(w); }
}

/* ─── Clipboard ──────────────────────────────────────────────────────────── */

static void copy_selection(void) {
    Pane *p = active_pane();
    if (!p || !g_renderer.sel_valid) return;
    if (!OpenClipboard(g_hwnd)) return;

    int sr = g_renderer.sel_start_row, sc = g_renderer.sel_start_col;
    int er = g_renderer.sel_end_row,   ec = g_renderer.sel_end_col;
    if (sr > er || (sr == er && sc > ec)) {
        int tr = sr, tc = sc; sr = er; sc = ec; er = tr; ec = tc;
    }

    static char text[65536];
    int ti = 0;

    EnterCriticalSection(&g_lock);
    for (int r = sr; r <= er && ti < 65520; r++) {
        int c0 = (r == sr) ? sc : 0;
        int c1 = (r == er) ? ec : p->term.view.cols - 1;

        /* Trailing blanks are padding, not content — stop at the last glyph. */
        int last_ns = c0 - 1;
        for (int c = c0; c <= c1; c++) {
            const ScreenCell *cell = screen_visible_cell(&p->term.view, r, c);
            if (cell && !cell->wide_cont && cell->ch > ' ') last_ns = c;
        }
        for (int c = c0; c <= last_ns && ti < 65512; c++) {
            const ScreenCell *cell = screen_visible_cell(&p->term.view, r, c);
            if (cell && cell->wide_cont) continue;
            uint32_t ch = cell ? cell->ch : ' ';
            if (!ch) ch = ' ';
            char enc[4];
            int n = utf8_encode(ch, enc);
            if (ti + n >= 65512) break;
            memcpy(text + ti, enc, (size_t)n);
            ti += n;
        }
        if (r < er) { text[ti++] = '\r'; text[ti++] = '\n'; }
    }
    text[ti] = '\0';
    LeaveCriticalSection(&g_lock);

    int wchars = 0;
    wchar_t *wtext = wisp_utf8_to_utf16_clipboard(text, ti, &wchars);
    if (wtext) {
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, ((size_t)wchars + 1) * sizeof(wchar_t));
        if (hg) {
            memcpy(GlobalLock(hg), wtext, ((size_t)wchars + 1) * sizeof(wchar_t));
            GlobalUnlock(hg);
            EmptyClipboard();
            SetClipboardData(CF_UNICODETEXT, hg);
        }
        str_free(wtext);
    }
    CloseClipboard();
}

static void paste_clipboard(void) {
    Pane *p = active_pane();
    if (!p || !OpenClipboard(g_hwnd)) return;

    HANDLE hd = GetClipboardData(CF_UNICODETEXT);
    if (hd) {
        const wchar_t *wtxt = (const wchar_t *)GlobalLock(hd);
        if (wtxt) {
            int u8len = 0;
            char *txt = wisp_utf16_to_utf8_clipboard(wtxt, &u8len);
            if (txt) {
                /* Bracketed paste lets the hosted program tell pasted text from
                   typed text, so a multi-line paste is not auto-executed. */
                if (p->term.view.bracketed_paste) {
                    pane_write(p, "\x1b[200~", 6);
                    pane_write(p, txt, u8len);
                    pane_write(p, "\x1b[201~", 6);
                } else {
                    pane_write(p, txt, u8len);
                }
                str_free(txt);
            }
            GlobalUnlock(hd);
        }
    }
    CloseClipboard();
}

/* ─── Font zoom ──────────────────────────────────────────────────────────── */

static void apply_font_size(float pt) {
    if (pt < MIN_FONT_PT) pt = MIN_FONT_PT;
    if (pt > MAX_FONT_PT) pt = MAX_FONT_PT;
    if (pt == g_font_pt) return;
    g_font_pt = pt;

    renderer_set_font(&g_renderer, g_cfg.font.family, pt);

    RECT rc;
    GetClientRect(g_hwnd, &rc);
    renderer_resize(&g_renderer, rc.right - rc.left, rc.bottom - rc.top);
    layout_panes();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ─── Window procedure ───────────────────────────────────────────────────── */

static void handle_input_action(InputAction action) {
    switch (action) {
        case INPUT_COPY:      copy_selection(); break;
        case INPUT_PASTE:     paste_clipboard(); break;
        case INPUT_NEW_TAB:   tab_open(); InvalidateRect(g_hwnd, NULL, FALSE); break;
        case INPUT_CLOSE_TAB: tab_close(g_active_tab); InvalidateRect(g_hwnd, NULL, FALSE); break;

        case INPUT_NEXT_TAB:
            if (g_tab_count > 1) {
                g_active_tab = (g_active_tab + 1) % g_tab_count;
                layout_panes(); sync_window_title();
                InvalidateRect(g_hwnd, NULL, FALSE);
            }
            break;

        case INPUT_PREV_TAB:
            if (g_tab_count > 1) {
                g_active_tab = (g_active_tab + g_tab_count - 1) % g_tab_count;
                layout_panes(); sync_window_title();
                InvalidateRect(g_hwnd, NULL, FALSE);
            }
            break;

        case INPUT_ZOOM_IN:  apply_font_size(g_font_pt + 1.0f); break;
        case INPUT_ZOOM_OUT: apply_font_size(g_font_pt - 1.0f); break;

        case INPUT_SCROLL_UP:
        case INPUT_SCROLL_DOWN:
        case INPUT_SCROLL_PAGE_UP:
        case INPUT_SCROLL_PAGE_DOWN: {
            Pane *p = active_pane();
            if (!p) break;
            EnterCriticalSection(&g_lock);
            if      (action == INPUT_SCROLL_UP)        p->term.scrollViewport(1);
            else if (action == INPUT_SCROLL_DOWN)      p->term.scrollViewport(-1);
            else if (action == INPUT_SCROLL_PAGE_UP)   p->term.pageViewport(1);
            else                                       p->term.pageViewport(-1);
            LeaveCriticalSection(&g_lock);
            InvalidateRect(g_hwnd, NULL, FALSE);
            break;
        }

        default: break;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

        case WM_CREATE:
            g_hwnd = hwnd;
            return 0;

        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            renderer_resize(&g_renderer, LOWORD(lParam), HIWORD(lParam));
            layout_panes();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_DPICHANGED: {
            renderer_update_dpi(&g_renderer, (float)HIWORD(wParam));
            RECT *sug = (RECT *)lParam;
            SetWindowPos(hwnd, NULL, sug->left, sug->top,
                         sug->right - sug->left, sug->bottom - sug->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layout_panes();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);

            renderer_begin_frame(&g_renderer);
            draw_tab_bar();

            Tab *t = active_tab();
            if (t) {
                EnterCriticalSection(&g_lock);
                for (int i = 0; i < t->pane_count; ++i) {
                    Pane *p = &t->panes[i];
                    if (!p->initialized) continue;
                    bool active = (i == t->active_pane);
                    bool cursor_shown = active && p->term.view.cursor_visible &&
                                        p->term.view.viewport_offset == 0;
                    renderer_paint_region(&g_renderer, &p->term.view, &p->rect, active,
                                          cursor_shown,
                                          p->term.view.cursor_x, p->term.view.cursor_y);
                }
                LeaveCriticalSection(&g_lock);
            }
            renderer_end_frame(&g_renderer);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;  /* the renderer clears every frame */

        case WM_TIMER:
            if (wParam == TIMER_BLINK) {
                renderer_toggle_cursor_blink(&g_renderer);
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (wParam == TIMER_TITLE) {
                sync_window_title();
                /* Reap panes whose hosted program has exited. */
                Tab *t = active_tab();
                if (t) {
                    for (int i = 0; i < t->pane_count; ++i) {
                        Pane *p = &t->panes[i];
                        if (p->initialized && !pty_is_alive(&p->pty)) {
                            t->active_pane = i;
                            pane_close_active();
                            InvalidateRect(hwnd, NULL, FALSE);
                            break;
                        }
                    }
                }
            }
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            Pane *p = active_pane();
            bool app_cursor = p && p->term.view.app_cursor_keys;

            /* Splits: Ctrl+Shift+E vertical, Ctrl+Shift+O horizontal,
               Ctrl+Shift+] / [ to cycle panes — matching Ghostty's defaults. */
            bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
            if (ctrl && shift) {
                if (wParam == 'E') {
                    pane_split(SPLIT_VERTICAL);
                    InvalidateRect(hwnd, NULL, FALSE);
                    g_suppress_char = true;
                    return 0;
                }
                if (wParam == 'O') {
                    pane_split(SPLIT_HORIZONTAL);
                    InvalidateRect(hwnd, NULL, FALSE);
                    g_suppress_char = true;
                    return 0;
                }
                if (wParam == VK_OEM_6 || wParam == VK_OEM_4) {
                    Tab *t = active_tab();
                    if (t && t->pane_count > 1) {
                        int step = (wParam == VK_OEM_6) ? 1 : t->pane_count - 1;
                        t->active_pane = (t->active_pane + step) % t->pane_count;
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    g_suppress_char = true;
                    return 0;
                }
            }

            InputEvent ev = input_translate(wParam, 0, lParam, app_cursor);
            g_suppress_char = input_suppress_char(wParam, lParam);

            if (ev.action == INPUT_CHAR) {
                pane_write(p, ev.bytes, ev.len);
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (ev.action != INPUT_NONE) {
                handle_input_action(ev.action);
            }
            return 0;
        }

        case WM_CHAR: {
            if (g_suppress_char) { g_suppress_char = false; return 0; }
            Pane *p = active_pane();
            if (!p) return 0;
            char enc[8];
            int n = utf8_encode((unsigned int)(wchar_t)wParam, enc);
            if (n > 0) {
                pane_write(p, enc, n);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);

            int bar = tab_bar_height();
            if (bar > 0 && my < bar) {
                for (int i = 0; i < g_tab_count; ++i) {
                    if (mx >= g_tab_rects[i].left && mx < g_tab_rects[i].right) {
                        g_active_tab = i;
                        layout_panes();
                        sync_window_title();
                        InvalidateRect(hwnd, NULL, FALSE);
                        break;
                    }
                }
                return 0;
            }

            /* Focus the pane under the cursor, then begin a selection. */
            Tab *t = active_tab();
            if (t) {
                for (int i = 0; i < t->pane_count; ++i) {
                    RECT r = t->panes[i].rect;
                    if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom) {
                        t->active_pane = i;
                        break;
                    }
                }
            }

            int col = 0, row = 0;
            renderer_pixel_to_cell(&g_renderer, mx, my, &col, &row);
            g_renderer.sel_start_col = g_renderer.sel_end_col = col;
            g_renderer.sel_start_row = g_renderer.sel_end_row = row;
            g_renderer.sel_active = true;
            g_renderer.sel_valid = false;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!g_renderer.sel_active) return 0;
            int col = 0, row = 0;
            renderer_pixel_to_cell(&g_renderer, GET_X_LPARAM(lParam),
                                   GET_Y_LPARAM(lParam), &col, &row);
            g_renderer.sel_end_col = col;
            g_renderer.sel_end_row = row;
            g_renderer.sel_valid = (g_renderer.sel_start_col != col ||
                                    g_renderer.sel_start_row != row);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        case WM_LBUTTONUP:
            if (g_renderer.sel_active) {
                g_renderer.sel_active = false;
                ReleaseCapture();
                if (g_renderer.sel_valid) copy_selection();
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_RBUTTONDOWN:
            paste_clipboard();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_MOUSEWHEEL: {
            Pane *p = active_pane();
            if (!p) return 0;
            p->wheel_accum += GET_WHEEL_DELTA_WPARAM(wParam);
            int lines = p->wheel_accum / WHEEL_DELTA;
            p->wheel_accum -= lines * WHEEL_DELTA;
            if (lines) {
                EnterCriticalSection(&g_lock);
                p->term.scrollViewport(lines * 3);
                LeaveCriticalSection(&g_lock);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_SETFOCUS: {
            Pane *p = active_pane();
            /* Cursor visibility is terminal state (mode 25); nothing to force here. */
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        case WM_CLOSE:
            if (g_cfg.general.confirm_exit && g_tab_count > 1) {
                if (MessageBoxW(hwnd, L"Close all tabs?", L"Wisp",
                                MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
                    return 0;
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            for (int i = 0; i < g_tab_count; ++i) {
                Tab *t = &g_tabs[i];
                if (!t->initialized) continue;
                for (int j = 0; j < t->pane_count; ++j) pane_stop(&t->panes[j]);
            }
            g_tab_count = 0;
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ─── Entry point ────────────────────────────────────────────────────────── */

static int wisp_run(HINSTANCE hInst, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wisp_unicode_init_process();
    WISP_LOG_INFO("Wisp starting");

    InitializeCriticalSection(&g_lock);

    config_defaults(&g_cfg);
    char cfg_path[MAX_PATH];
    config_path(cfg_path, MAX_PATH);
    config_load(&g_cfg, cfg_path);
    apply_configured_theme(&g_cfg);

    if (!window_register_class(hInst)) return 1;

    HWND hwnd = window_create(hInst, &g_cfg, nCmdShow);
    if (!hwnd) {
        WISP_LOG_ERROR("window_create failed");
        return 1;
    }
    g_hwnd = hwnd;

    if (!renderer_init(&g_renderer, hwnd, &g_cfg)) {
        WISP_LOG_ERROR("renderer_init failed");
        return 1;
    }
    g_font_pt = g_cfg.font.size;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    RECT rc;
    GetClientRect(hwnd, &rc);
    renderer_resize(&g_renderer, rc.right - rc.left, rc.bottom - rc.top);

    if (!tab_open()) {
        MessageBoxW(hwnd, L"Could not start the shell process.", L"Wisp",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    if (g_cfg.cursor.blink)
        SetTimer(hwnd, TIMER_BLINK, (UINT)g_cfg.cursor.blink_rate_ms, NULL);
    SetTimer(hwnd, TIMER_TITLE, 250, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    renderer_destroy(&g_renderer);
    DeleteCriticalSection(&g_lock);
    WISP_LOG_INFO("Wisp exited with code %d", (int)msg.wParam);
    return (int)msg.wParam;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    wisp_log_init_default("Wisp");
    wisp_log_set_max_file_size(10ULL * 1024ULL * 1024ULL);
    wisp_log_install_crash_handlers();

    try {
        int rc = wisp_run(hInst, nCmdShow);
        wisp_log_close();
        return rc;
    } catch (const std::exception &ex) {
        WISP_LOG_ERROR("Unhandled C++ exception: %s", ex.what());
    } catch (...) {
        WISP_LOG_ERROR("Unhandled unknown exception");
    }

    wisp_log_close();
    MessageBoxW(NULL,
                L"Wisp crashed. Details were written to "
                L"%LOCALAPPDATA%\\Wisp\\logs\\wisp.log",
                L"Wisp error", MB_OK | MB_ICONERROR);
    return 1;
}
