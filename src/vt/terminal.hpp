/* Transliterated from Ghostty src/terminal/Terminal.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * The primary terminal emulation structure. This represents a single
 * "terminal" containing a grid of characters and exposes various operations
 * on that grid. This also maintains the scrollback buffer.
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: ported in upstream's build configuration with kitty_graphics and
 * glyph_protocol disabled, so the Kitty graphics storage and the Glyph
 * Protocol glossary are not present; the tests upstream gates on those
 * options are not ported either. std.Io is not modelled: `gpa()` is the
 * active screen's allocator and there is no `io()`.
 */

#pragma once
#ifndef WISP_VT_TERMINAL_HPP
#define WISP_VT_TERMINAL_HPP

#include "../terminal/ansi.hpp"
#include "../terminal/charsets.hpp"
#include "../terminal/color.hpp"
#include "../terminal/modes.hpp"
#include "../terminal/mouse.hpp"
#include "../terminal/kitty/dnd_drop.hpp"
#include "../terminal/osc.hpp"
#include "../terminal/sgr.hpp"
#include "screen_set.hpp"
#include "tabstops.hpp"
#include "unicode/grapheme.hpp"

namespace wisp {
namespace vt {

struct Terminal {
    typedef page::Page Page;
    typedef page::Cell Cell;
    typedef page::Row Row;
    typedef PageList::Pin Pin;
    typedef terminal::ansi::CursorStyle AnsiCursorStyle;

    /* Default tabstop interval */
    static const size_t TABSTOP_INTERVAL = 8;

    /* The various color configurations a terminal maintains and that can
     * be set dynamically via OSC, with defaults usually coming from a
     * configuration. */
    struct Colors {
        terminal::DynamicRGB background;
        terminal::DynamicRGB foreground;
        terminal::DynamicRGB cursor;
        terminal::DynamicPalette palette;

        static Colors default_() {
            Colors c;
            c.background = terminal::DynamicRGB::unset();
            c.foreground = terminal::DynamicRGB::unset();
            c.cursor = terminal::DynamicRGB::unset();
            c.palette = terminal::DynamicPalette::default_();
            return c;
        }
    };

    /* This is a set of dirty flags the renderer can use to determine
     * what parts of the screen need to be redrawn. It is up to the renderer
     * to clear these flags.
     *
     * This only contains dirty flags for terminal state, not for the screen
     * state. The screen state has its own dirty flags. */
    struct Dirty {
        /* Set when the color palette is modified in any way. */
        bool palette;

        /* Set when the reverse colors mode is modified. */
        bool reverse_colors;

        /* Screen clear of some kind. This can be due to a screen change,
         * erase display, etc. */
        bool clear;

        /* Set when the pre-edit is modified. */
        bool preedit;

        Dirty() : palette(false), reverse_colors(false), clear(false), preedit(false) {}
    };

    /* Scrolling region is the area of the screen designated where scrolling
     * occurs. When scrolling the screen, only this viewport is scrolled. */
    struct ScrollingRegion {
        /* Top and bottom of the scroll region (0-indexed)
         * Precondition: top < bottom */
        size::CellCountInt top;
        size::CellCountInt bottom;

        /* Left/right scroll regions.
         * Precondition: right > left
         * Precondition: right <= cols - 1 */
        size::CellCountInt left;
        size::CellCountInt right;
    };

    /* Terminal-level cursor state shared by all screens. */
    struct Cursor {
        /* Whether the current cursor appearance follows the configured defaults. */
        bool is_default; /* = true */

        /* Configured style restored by DECSCUSR default and RIS. */
        Screen::CursorStyle default_style; /* = .block */

        /* Configured blink restored by DECSCUSR default and RIS. Null selects
         * the terminal emulator default, which is blinking. */
        Maybe<bool> default_blink; /* = false */

        Cursor() : is_default(true), default_style(Screen::CursorStyle::block), default_blink(false) {}
    };

    struct Options {
        size::CellCountInt cols;
        size::CellCountInt rows;

        /* The maximum size of scrollback in bytes. Null is unlimited and zero
         * disables scrollback. */
        Maybe<size_t> max_scrollback_bytes; /* = 10_000 */

        /* The maximum number of physical scrollback rows, excluding the active
         * area. Null is unlimited. The effective limit permits at least one
         * standard page and only complete historical pages are pruned. */
        Maybe<size_t> max_scrollback_lines; /* = null */

        Colors colors; /* = .default */

        /* The default mode state. When the terminal gets a reset, it
         * will revert back to this state. */
        terminal::modes::ModePacked default_modes;

        /* Cursor state restored by DECSCUSR default and RIS. */
        Screen::CursorStyle default_cursor_style; /* = .block */
        Maybe<bool> default_cursor_blink;         /* = false */

        Options(size::CellCountInt c, size::CellCountInt r)
            : cols(c), rows(r), max_scrollback_bytes((size_t)10000), max_scrollback_lines(), colors(Colors::default_()),
              default_modes(), default_cursor_style(Screen::CursorStyle::block), default_cursor_blink(false) {}
    };

    /* Wisp: std.ArrayList(u8). Only the operations Terminal uses are
     * provided, with upstream's names, so the allocation failure paths
     * upstream tests are the same here. */
    struct ByteList {
        uint8_t *items;
        size_t len;
        size_t capacity;

        ByteList() : items(nullptr), len(0), capacity(0) {}

        void deinit(zigstd::Allocator alloc) {
            if (capacity > 0) alloc.free(items, capacity);
            items = nullptr;
            len = 0;
            capacity = 0;
        }

        void clearRetainingCapacity() { len = 0; }

        /* Wisp: false is OutOfMemory. */
        bool ensureTotalCapacity(zigstd::Allocator alloc, size_t new_capacity) {
            if (capacity >= new_capacity) return true;
            /* std.ArrayList grows geometrically; the exact growth is not
             * observable, only that the capacity is at least what was asked
             * for and that a failure leaves the list untouched. */
            size_t better = capacity;
            while (better < new_capacity) better = better + (better / 2) + 8;
            uint8_t *n = alloc.alloc(better);
            if (!n) return false;
            if (len) memcpy(n, items, len);
            if (capacity > 0) alloc.free(items, capacity);
            items = n;
            capacity = better;
            return true;
        }

        bool ensureTotalCapacityPrecise(zigstd::Allocator alloc, size_t new_capacity) {
            if (capacity >= new_capacity) return true;
            uint8_t *n = alloc.alloc(new_capacity);
            if (!n) return false;
            if (len) memcpy(n, items, len);
            if (capacity > 0) alloc.free(items, capacity);
            items = n;
            capacity = new_capacity;
            return true;
        }
    };

    /* The set of screens behind this terminal (e.g. primary vs alternate). */
    ScreenSet screens;

    /* Whether we're currently writing to the status line (DECSASD and DECSSDT).
     * We don't support a status line currently so we just black hole this
     * data so that it doesn't mess up our main display. */
    terminal::ansi::StatusDisplay status_display; /* = .main */

    /* Kitty drag and drop protocol (OSC 72) state, allocated when a
     * client registers to accept drops and freed when it unregisters. */
    terminal::kitty::dnd::State *kitty_dnd; /* = null */

    /* Where the tabstops are. */
    Tabstops tabstops;

    /* The size of the terminal. */
    size::CellCountInt rows;
    size::CellCountInt cols;

    /* The size of the screen in pixels. This is used for pty events and images */
    uint32_t width_px;  /* = 0 */
    uint32_t height_px; /* = 0 */

    /* The current scrolling region. */
    ScrollingRegion scrolling_region;

    /* The last reported pwd, if any. */
    ByteList pwd;

    /* The title of the terminal as set by escape sequences (e.g. OSC 0/2). */
    ByteList title;

    /* The color state for this terminal. */
    Colors colors;

    /* The previous printed character. This is used for the repeat previous
     * char CSI (ESC [ <n> b). */
    Maybe<uint32_t> previous_char; /* = null */

    /* The modes that this terminal currently has active. */
    terminal::modes::ModeState modes;

    /* Terminal-level cursor state. */
    Cursor cursor;

    /* The most recently set mouse shape for the terminal. */
    terminal::mouse::Shape mouse_shape; /* = .text */

    /* These are just a packed set of flags we may set on the terminal. */
    struct Flags {
        /* This supports a Kitty extension where programs using semantic
         * prompts (OSC133) can annotate their new prompts with `redraw=0` to
         * disable clearing the prompt on resize. */
        terminal::osc::semantic_prompt::Redraw shell_redraws_prompt; /* = .true */

        /* This is set via ESC[4;2m. Any other modify key mode just sets
         * this to false and we act in mode 1 by default. */
        bool modify_other_keys_2; /* = false */

        /* The mouse event mode and format. These are set to the last
         * set mode in modes. You can't get the right event/format to use
         * based on modes alone because modes don't show you what order
         * this was called so we have to track it separately. */
        terminal::mouse::Event mouse_event;   /* = .none */
        terminal::mouse::Format mouse_format; /* = .x10 */

        /* Set via the XTSHIFTESCAPE sequence. If true (XTSHIFTESCAPE = 1)
         * then we want to capture the shift key for the mouse protocol
         * if the configuration allows it. */
        enum class MouseShiftCapture : uint8_t { null_, false_, true_ };
        MouseShiftCapture mouse_shift_capture; /* = .null */

        /* True if the window is focused. */
        bool focused; /* = true */

        /* True if the terminal view may be visible. Unknown visibility is
         * represented as visible so callers behave conservatively. */
        bool visible; /* = true */

        /* Whether a resize may pull rows out of scrollback back into the
         * active area. This should be false if the pty keeps its own screen
         * buffer without scrollback (e.g. Windows ConPTY) so that we stay in
         * sync with it. See PageList.Resize for details. This is configuration
         * rather than terminal state so it is preserved across a full reset. */
        bool resize_pull_scrollback; /* = true */

        /* True if the terminal is in a password entry mode. This is set
         * to true based on termios state. */
        bool password_input; /* = false */

        /* True if the terminal should perform selection scrolling. */
        bool selection_scroll; /* = false */

        /* Dirty flag used only by the search thread. The renderer is expected
         * to set this to true if the viewport was dirty as it was rendering.
         * This is used by the search thread to more efficiently re-search the
         * viewport and active area.
         *
         * Since the renderer is going to inspect the viewport/active area ANYWAYS,
         * this lets our search thread do less work and hold the lock less time,
         * resulting in more throughput for everything. */
        bool search_viewport_dirty; /* = false */

        /* Dirty flags for the renderer. */
        Dirty dirty;

        Flags()
            : shell_redraws_prompt(terminal::osc::semantic_prompt::Redraw::true_), modify_other_keys_2(false),
              mouse_event(terminal::mouse::Event::none), mouse_format(terminal::mouse::Format::x10),
              mouse_shift_capture(MouseShiftCapture::null_), focused(true), visible(true),
              resize_pull_scrollback(true), password_input(false), selection_scroll(false),
              search_viewport_dirty(false), dirty() {}
    };
    Flags flags;

    /* Returns the current color for an xterm OSC color target.
     *
     * Unsupported dynamic and special colors return null. The cursor color
     * follows xterm-style reporting and falls back to the foreground color when
     * no explicit cursor color is set. */
    /* Wisp: DynamicRGB.get() is get(&out) plus a bool. */
    static Maybe<terminal::RGB> dynGet(const terminal::DynamicRGB &c) {
        terminal::RGB rgb;
        if (!c.get(&rgb)) return Maybe<terminal::RGB>::none();
        return Maybe<terminal::RGB>(rgb);
    }

    /* Returns the current color for an xterm OSC color target.
     *
     * Unsupported dynamic and special colors return null. The cursor color
     * follows xterm-style reporting and falls back to the foreground color when
     * no explicit cursor color is set. */
    Maybe<terminal::RGB> colorForXterm(const terminal::osc::color::Target &target) const {
        typedef terminal::osc::color::Target Target;
        switch (target.tag) {
        case Target::Tag::palette: return Maybe<terminal::RGB>(colors.palette.current.colors[target.palette]);
        case Target::Tag::dynamic:
            switch (target.dynamic) {
            case terminal::Dynamic::foreground: return dynGet(colors.foreground);
            case terminal::Dynamic::background: return dynGet(colors.background);
            case terminal::Dynamic::cursor: {
                const Maybe<terminal::RGB> c = dynGet(colors.cursor);
                return c.has ? c : dynGet(colors.foreground);
            }
            default: return Maybe<terminal::RGB>::none();
            }
        default: return Maybe<terminal::RGB>::none();
        }
    }

    /* Returns the current color for a Kitty color protocol key.
     *
     * Only palette, foreground, background, and cursor colors are backed by
     * Terminal state. Unsupported keys, or supported dynamic colors without a
     * value, return null. */
    Maybe<terminal::RGB> colorForKitty(const terminal::kitty::color::Kind &key) const {
        typedef terminal::kitty::color::Kind Kind;
        switch (key.tag) {
        case Kind::Tag::palette: return Maybe<terminal::RGB>(colors.palette.current.colors[key.palette]);
        default:
            switch (key.special) {
            case terminal::kitty::color::Special::foreground: return dynGet(colors.foreground);
            case terminal::kitty::color::Special::background: return dynGet(colors.background);
            case terminal::kitty::color::Special::cursor: return dynGet(colors.cursor);
            default: return Maybe<terminal::RGB>::none();
            }
        }
    }

    /* Initialize a new terminal. Wisp: false is OutOfMemory. */
    static bool init(zigstd::Allocator alloc, const Options &opts, Terminal *out) {
        const size::CellCountInt cols = opts.cols;
        const size::CellCountInt rows = opts.rows;

        ScreenSet screen_set;
        {
            Screen::Options screen_opts(cols, rows, opts.max_scrollback_bytes, opts.max_scrollback_lines);
            if (!ScreenSet::init(alloc, screen_opts, &screen_set)) return false;
        }

        Terminal result;
        result.cols = cols;
        result.rows = rows;
        result.screens = screen_set;
        if (Tabstops::init(alloc, cols, TABSTOP_INTERVAL, &result.tabstops) != Tabstops::Error::none) {
            screen_set.deinit(alloc);
            return false;
        }
        result.scrolling_region.top = 0;
        result.scrolling_region.bottom = (size::CellCountInt)(rows - 1);
        result.scrolling_region.left = 0;
        result.scrolling_region.right = (size::CellCountInt)(cols - 1);
        result.colors = opts.colors;
        result.modes.values = opts.default_modes;
        result.modes.default_ = opts.default_modes;
        result.cursor.default_style = opts.default_cursor_style;
        result.cursor.default_blink = opts.default_cursor_blink;

        result.setCursorStyle(AnsiCursorStyle::default_);
        *out = result;
        return true;
    }

    Terminal()
        : status_display(terminal::ansi::StatusDisplay::main), kitty_dnd(nullptr), rows(0), cols(0), width_px(0), height_px(0),
          previous_char(), mouse_shape(terminal::mouse::Shape::text) {
        scrolling_region.top = 0;
        scrolling_region.bottom = 0;
        scrolling_region.left = 0;
        scrolling_region.right = 0;
        colors = Colors::default_();
    }

    void deinit(zigstd::Allocator alloc) {
        tabstops.deinit(alloc);
        screens.deinit(alloc);
        colors.palette.deinit();
        pwd.deinit(alloc);
        title.deinit(alloc);
        if (kitty_dnd != nullptr) kitty_dnd->destroy(alloc);
    }

    /* The general allocator we should use for this terminal. */
    zigstd::Allocator gpa() { return screens.active->alloc; }

    /* Change the cursor's current shape and blink behavior.
     *
     * The terminal parser uses this for DECSCUSR (`CSI Ps SP q`), but the behavior
     * is general: `.default` selects the configured defaults, while any other
     * value selects a concrete appearance until it is changed again or reset. */
    void setCursorStyle(AnsiCursorStyle value) {
        /* Remember whether future configuration changes should update the visible
         * cursor. An explicit appearance must remain in effect until the program
         * selects the default again. */
        cursor.is_default = value == AnsiCursorStyle::default_;

        /* Convert the request into the concrete values used by the renderer and
         * terminal mode state. A null default blink means the emulator default. */
        bool blinking;
        switch (value) {
        case AnsiCursorStyle::default_: blinking = cursor.default_blink.orelse(true); break;
        case AnsiCursorStyle::steady_block:
        case AnsiCursorStyle::steady_bar:
        case AnsiCursorStyle::steady_underline: blinking = false; break;
        default: blinking = true; break;
        }
        modes.set(terminal::modes::Mode::cursor_blinking, blinking);

        Screen::CursorStyle style;
        switch (value) {
        case AnsiCursorStyle::default_: style = cursor.default_style; break;
        case AnsiCursorStyle::blinking_block:
        case AnsiCursorStyle::steady_block: style = Screen::CursorStyle::block; break;
        case AnsiCursorStyle::blinking_bar:
        case AnsiCursorStyle::steady_bar: style = Screen::CursorStyle::bar; break;
        default: style = Screen::CursorStyle::underline; break;
        }
        screens.active->cursor.cursor_style = style;
    }

    /* Change the default cursor shape.
     *
     * If the cursor currently follows its defaults, the visible shape changes
     * immediately. Otherwise the new shape is saved for the next reset or default
     * selection, such as DECSCUSR `CSI 0 SP q`. */
    void setDefaultCursorStyle(Screen::CursorStyle configured_style) {
        /* Always retain the new default, even while an explicit appearance is
         * active, so a later reset or default request can restore it. */
        cursor.default_style = configured_style;

        /* Do not overwrite an appearance explicitly selected by the program. */
        if (cursor.is_default) setCursorStyle(AnsiCursorStyle::default_);
    }

    /* Change the default cursor blink behavior.
     *
     * Null selects the terminal emulator default (blinking). Like the default
     * shape, this is applied immediately only when the cursor currently follows
     * its defaults; otherwise it is saved for the next reset or default selection. */
    void setDefaultCursorBlink(Maybe<bool> blink) {
        /* Keep the configured value separate from the currently resolved mode so
         * null can continue to mean "use the emulator default." */
        cursor.default_blink = blink;

        /* Do not overwrite blink behavior explicitly selected by the program. */
        if (cursor.is_default) setCursorStyle(AnsiCursorStyle::default_);
    }

    /* Change the primary screen's maximum scrollback allocation in bytes.
     *
     * Null removes the byte limit and zero disables scrollback. Disabling
     * scrollback also immediately erases retained history and changes future
     * scrolling to use the no-scrollback path. The alternate screen is
     * intentionally unaffected because it never retains scrollback. */
    void setScrollbackMaxBytes(Maybe<size_t> max) {
        Screen *primary = screens.get(ScreenSet::Key::primary);
        primary->pages.setMaxBytes(max);
        primary->no_scrollback = max.has && max.value == 0;

        if (primary->no_scrollback) primary->eraseHistory(Maybe<point::Point>());
    }

    /* Change the primary screen's maximum number of physical scrollback lines.
     *
     * Null removes the line limit. The alternate screen is intentionally
     * unaffected because it never retains scrollback. */
    void setScrollbackMaxLines(Maybe<size_t> max) {
        Screen *primary = screens.get(ScreenSet::Key::primary);
        primary->pages.setMaxLines(max);
    }

    /* ------------------------------------------------------------------ */
    /* Wisp: defined in terminal_impl.hpp. */

    /* Set the charset into the given slot. */
    void configureCharset(terminal::charsets::Slots slot, terminal::charsets::Charset set);

    /* Invoke the charset in slot into the active slot. If single is true,
     * then this will only be invoked for a single character. */
    void invokeCharset(terminal::charsets::ActiveSlot active, terminal::charsets::Slots slot, bool single);

    /* Carriage return moves the cursor to the first column. */
    void carriageReturn();

    /* Linefeed moves the cursor to the next line. Wisp: false is OutOfMemory. */
    bool linefeed();

    /* Backspace moves the cursor back a column (but not less than 0). */
    void backspace();

    void cursorUp(size_t count_req);
    void cursorDown(size_t count_req);
    void cursorRight(size_t count_req);
    void cursorLeft(size_t count_req);

    /* Save cursor position and further state. */
    void saveCursor();

    /* Restore cursor position and other state. */
    void restoreCursor();

    /* Set the character protection mode for the terminal. */
    void setProtectedMode(terminal::ansi::ProtectedMode mode);

    /* Print UTF-8 encoded string to the terminal. */
    bool printString(const char *str, size_t len);
    bool printString(const char *str) { return printString(str, strlen(str)); }

    bool print(uint32_t c);
    void printCell(uint32_t unmapped_c, Cell::Wide wide);
    bool printWrap();

    /* Move the cursor to the next line in the scrolling region, scrolling if
     * necessary. */
    bool index();

    /* Insert amount spaces at the current cursor position. */
    void insertBlanks(size_t count);

    /* Print the previous printed character a repeated amount of times. */
    bool printRepeat(size_t count_req);

    /* Print multiple codepoints to the terminal at once. */
    bool printSlice(const uint32_t *cps, size_t cps_len);

    /* The width class of a printSlice batch. Each batch contains only
     * codepoints of a single width class because they fill cells
     * differently: wide codepoints occupy a (wide, spacer_tail) cell
     * pair while narrow codepoints occupy a single cell. */
    enum class PrintSliceWidth : uint8_t { narrow, wide };

    bool printSliceFast(const uint32_t *cps, size_t cps_len, bool grapheme_cluster, bool allow_unicode,
                        size_t *out);
    static bool printSliceEligible(uint32_t cp, PrintSliceWidth width);
    static void printSliceStoreRun(Cell *cells, const uint32_t *cps, size_t from, size_t to,
                                   uint64_t template_bits);
    static uint64_t printSliceCheckExpected(style::Id style_id);
    bool printSliceFill(PrintSliceWidth width, const uint32_t *cps, size_t cps_len, bool grapheme_cluster,
                        bool allow_unicode, size_t *out);

    /* Set the pwd for the terminal. Wisp: false is OutOfMemory. */
    bool setPwd(const char *pwd_new, size_t len);
    bool setPwd(const char *pwd_new) { return setPwd(pwd_new, strlen(pwd_new)); }

    /* Returns the pwd for the terminal, if any. */
    const char *getPwd() const;

    /* Set the title for the terminal. Wisp: false is OutOfMemory. */
    bool setTitle(const char *t, size_t len);
    bool setTitle(const char *t) { return setTitle(t, strlen(t)); }

    /* Returns the title for the terminal, if any. */
    const char *getTitle() const;

    /* Switch to the given screen. Wisp: ok is false for OutOfMemory. */
    Screen *switchScreen(ScreenSet::Key key, bool *ok);

    /* Modal screen changes. These map to the literal terminal
     * modes to enable or disable alternate screen modes. They each
     * have subtle behaviors so we define them as an enum here. */
    enum class SwitchScreenMode : uint8_t {
        /* Legacy alternate screen mode. This goes to the alternate
         * screen or primary screen and only copies the cursor. The
         * screen is not erased. */
        mode_47,

        /* Alternate screen mode where the alternate screen is cleared
         * on exit. The primary screen is never cleared. The cursor is
         * copied. */
        mode_1047,

        /* Save primary screen cursor, switch to alternate screen,
         * and clear the alternate screen on entry. On exit,
         * do not clear the screen, and restore the cursor on the
         * primary screen. */
        mode_1049,
    };

    /* Switch screen via a mode switch (e.g. mode 47, 1047, 1049). */
    bool switchScreenMode(SwitchScreenMode mode, bool enabled);

    /* Return the current string value of the terminal. */
    std::string plainString() const;

    /* Same as plainString, but respects row wrap state. */
    std::string plainStringUnwrapped() const;

    /* Full reset. */
    void fullReset();

    /* Returns true if the point is dirty, used for testing. */
    bool isDirty(const point::Point &pt) const;

    /* Clear all dirty bits. Testing only. */
    void clearDirty();

    PageList::IncreaseCapacityError setAttribute(const terminal::sgr::Attribute &attr);

    /* Print the active attributes as a string (DECRQSS). */
    std::string printAttributes() const;

    /* The modes for DECCOLM. */
    enum class DeccolmMode : uint8_t { cols_80 = 0, cols_132 = 1 };

    /* A terminal resize expressed in cells with optional per-cell pixel
     * geometry. */
    struct Resize {
        size::CellCountInt cols;
        size::CellCountInt rows;
        struct CellSize {
            uint32_t width;
            uint32_t height;
        };
        Maybe<CellSize> cell_size_px; /* = null */

        Resize() : cols(0), rows(0), cell_size_px() {}
        Resize(size::CellCountInt c, size::CellCountInt r) : cols(c), rows(r), cell_size_px() {}
    };

    enum class ResizeError : uint8_t {
        none,
        /* Resize requires allocation */
        OutOfMemory,
        /* Input value was invalid, such as a 0-sized dimension. */
        InvalidValue,
    };

    enum class ResizeTw { tabstops, primary_screen, alternate_screen, alternate_screen_init };
    typedef tripwire::Module<ResizeTw, ResizeError, 4> resize_tw;

    ResizeError deccolm(zigstd::Allocator alloc, DeccolmMode mode);

    /* Resize the underlying terminal. */
    ResizeError resize(zigstd::Allocator alloc, const Resize &opts);

    void deleteChars(size_t count_req);
    void eraseChars(size_t count_req);
    void eraseLine(terminal::csi::EraseLine mode, bool protected_req);
    void eraseDisplay(terminal::csi::EraseDisplay mode, bool protected_req);

    /* Resets all margins and fills the whole screen with the character 'E' */
    bool decaln();

    void setCursorPos(size_t row_req, size_t col_req);
    void setTopAndBottomMargin(size_t top_req, size_t bottom_req);
    void setLeftAndRightMargin(size_t left_req, size_t right_req);

    /* Options for scrolling the viewport of the terminal grid. */
    struct ScrollViewport {
        enum class Tag : uint8_t {
            /* Scroll to the top of the scrollback */
            top,
            /* Scroll to the bottom, i.e. the top of the active area */
            bottom,
            /* Scroll by some delta amount, up is negative. */
            delta,
            /* Scroll to the given absolute row offset from the top of the
             * scrollable area. A value of zero is the top row. The requested
             * row becomes the first visible row of the viewport, clamped so
             * the viewport never scrolls beyond the top of the active area.
             * This is the same row space as PageList.Scrollbar offset. */
            row,
        } tag;
        ptrdiff_t delta;
        size_t row;

        static ScrollViewport makeTop() { ScrollViewport v; v.tag = Tag::top; v.delta = 0; v.row = 0; return v; }
        static ScrollViewport makeBottom() { ScrollViewport v; v.tag = Tag::bottom; v.delta = 0; v.row = 0; return v; }
        static ScrollViewport makeDelta(ptrdiff_t d) { ScrollViewport v; v.tag = Tag::delta; v.delta = d; v.row = 0; return v; }
        static ScrollViewport makeRow(size_t r) { ScrollViewport v; v.tag = Tag::row; v.delta = 0; v.row = r; return v; }
    };

    /* Scroll the viewport of the terminal grid. */
    void scrollViewport(const ScrollViewport &behavior);

    /* Return the current compression activity value. */
    uint64_t compressionActivity() const;

    /* The amount of compression work performed by `compress` before returning. */
    enum class CompressionMode : uint8_t { incremental, full };

    /* The scheduling result of a `compress` call. */
    enum class CompressionResult : uint8_t { unsupported, pending, complete };

    /* Compress cold memory to save resident memory space. */
    CompressionResult compress(CompressionMode mode);

    /* To be called before shifting a row (as in insertLines and deleteLines) */
    void rowWillBeShifted(Page *page, Row *row);

    /* Renew every live page generation in an inclusive range before a full-width
     * line operation moves logical rows between their coordinates. */
    void invalidateFullWidthRowRange(PageList::Node *first, PageList::Node *last);

    /* Insert amount lines at the current cursor row. */
    void insertLines(size_t count);

    /* Removes amount lines from the current cursor row down. */
    void deleteLines(size_t count);

    /* Perform a semantic prompt command. Wisp: false is OutOfMemory. */
    bool semanticPrompt(const terminal::osc::semantic_prompt::Command &cmd);
    bool semanticPromptFreshLine();

    /* Returns true if the cursor is currently at a prompt. */
    bool cursorIsAtPrompt();

    void horizontalTab();
    void horizontalTabBack();
    void tabClear(terminal::csi::TabClear cmd);
    void tabSet();
    void tabReset();

    void reverseIndex();

    /* Scroll the text up by one row. Wisp: false is OutOfMemory. */
    bool scrollUp(size_t count);

    /* Scroll the text down by one row. */
    void scrollDown(size_t count);
};

} /* namespace vt */
} /* namespace wisp */

#include "terminal_impl.hpp"

#endif /* WISP_VT_TERMINAL_HPP */
