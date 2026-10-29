/* Transliterated from Ghostty src/input/Binding.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * A binding maps some input trigger to an action. When the trigger
 * occurs, the action is performed.
 *
 * Wisp: Set is documented at its declaration below.
 *
 * Wisp, differences in shape rather than behavior:
 *   - Errors are the enum `Error` (InvalidFormat, InvalidAction plus
 *     parseSingle's UnexpectedChain/UnexpectedSequence).
 *   - `Action` (a union(enum) of 88 actions) is a tag plus one field per
 *     payload type. WISP_BINDING_ACTIONS lists every tag in upstream order
 *     with its payload kind; the comptime loops over the union fields
 *     (parse, format, clone, hash, equal) are switches over that kind.
 *   - `Action.Scoped(s)` builds a new union type at comptime. C++ cannot,
 *     so `scoped(s)` returns whether the action belongs to scope `s`; the
 *     action itself is the scoped value.
 *   - `[]const u8` payloads are std::string, so `clone` is a copy.
 *   - The GTK gobject type registration is omitted (no GTK runtime).
 *   - `std.fmt.parseInt` (base 10, optional sign, `_` digit separators)
 *     and `std.fmt.parseFloat` are reimplemented in `detail::`.
 *   - Hashes feed Wyhash the same fields as upstream's autoHash calls; the
 *     exact values are not part of any upstream test.
 *   - Trigger's case folding uses vt/unicode/case_folding.hpp, generated
 *     from the UCD that the pinned uucode ships.
 */
#pragma once
#ifndef WISP_INPUT_BINDING_HPP
#define WISP_INPUT_BINDING_HPP

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <limits>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "../datastruct/comparison.hpp"
#include "../vt/unicode/case_folding.hpp"
#include "../zigstd/unicode.hpp"
#include "../zigstd/wyhash.hpp"
#include "key.hpp"
#include "key_mods.hpp"

namespace wisp {
namespace input {
namespace binding {

enum class Error : uint8_t {
    none = 0,
    InvalidFormat,
    InvalidAction,

    /* parseSingle */
    UnexpectedChain,
    UnexpectedSequence,
};

/* Flags the full binding-scoped flags that can be set per binding. */
struct Flags {
    /* True if this binding should consume the input when the
     * action is triggered. */
    bool consumed = true;

    /* True if this binding should be forwarded to all active surfaces
     * in the application. */
    bool all = false;

    /* True if this binding is global. Global bindings should work system-wide
     * and not just while Ghostty is focused. This may not work on all platforms.
     * See the keybind config documentation for more information. */
    bool global = false;

    /* True if this binding should only be triggered if the action can be
     * performed. If the action can't be performed then the binding acts as
     * if it doesn't exist. */
    bool performable = false;

    /* C type */
    typedef uint8_t C;

    /* Converts this to a C-compatible value.
     *
     * Sync with ghostty.h for enums. */
    C cval() const { return (C)((consumed ? 1 : 0) | (all ? 2 : 0) | (global ? 4 : 0) | (performable ? 8 : 0)); }

    bool operator==(const Flags &o) const { return cval() == o.cval(); }
};

namespace detail {

/* Wisp: std.meta.stringToEnum over a name table indexed by enum value. */
template <typename E, size_t N>
bool enumFromName(const char *const (&names)[N], const std::string &s, E *out) {
    for (size_t i = 0; i < N; i++) {
        if (s == names[i]) {
            *out = (E)i;
            return true;
        }
    }
    return false;
}

/* Wisp: std.fmt.parseInt(T, value, 10). Accepts an optional sign and `_`
 * between digits; rejects empty input, leading/trailing `_` and overflow. */
template <typename T>
bool parseIntBase10(const std::string &value, T *out) {
    if (value.empty()) return false;
    size_t i = 0;
    bool negative = false;
    if (value[0] == '+' || value[0] == '-') {
        negative = value[0] == '-';
        i = 1;
    }
    if (i >= value.size()) return false;
    if (value[i] == '_' || value.back() == '_') return false;

    /* Accumulate as a negative number so the minimum fits, as Zig does. */
    typedef typename std::conditional<std::is_signed<T>::value, int64_t, uint64_t>::type Wide;
    long double acc = 0;
    for (; i < value.size(); i++) {
        const char c = value[i];
        if (c == '_') continue;
        if (c < '0' || c > '9') return false;
        acc = acc * 10 + (c - '0');
        if (acc > (long double)std::numeric_limits<T>::max() + 1) return false;
    }
    if (negative) acc = -acc;
    if (acc < (long double)std::numeric_limits<T>::min() || acc > (long double)std::numeric_limits<T>::max()) {
        return false;
    }
    *out = (T)(Wide)acc;
    return true;
}

/* Wisp: std.fmt.parseFloat. Decimal (with optional exponent), `inf`,
 * `infinity` and `nan` (case-insensitive) with an optional sign, and `_`
 * between digits; the whole input must be consumed. */
inline bool parseFloat32(const std::string &value, float *out) {
    if (value.empty()) return false;
    std::string cleaned;
    for (size_t i = 0; i < value.size(); i++) {
        if (value[i] == '_') {
            if (i == 0 || i + 1 == value.size()) return false;
            continue;
        }
        cleaned.push_back(value[i]);
    }
    if (cleaned.empty() || isspace((unsigned char)cleaned[0])) return false;
    /* Hex floats are accepted by strtod but are a separate Zig syntax with
     * the same meaning, so allowing them changes nothing observable. */
    char *end = nullptr;
    errno = 0;
    const float v = strtof(cleaned.c_str(), &end);
    if (end == cleaned.c_str() || *end != '\0') return false;
    *out = v;
    return true;
}

/* Wisp: Zig's `{d}` for floats, the shortest decimal that round-trips. */
inline std::string formatFloat(float v) {
    char buf[64];
    for (int precision = 1; precision <= 9; precision++) {
        snprintf(buf, sizeof buf, "%.*g", precision, (double)v);
        if (strtof(buf, nullptr) == v) break;
    }
    return buf;
}

inline void hashBytes(zigstd::Wyhash *h, uint64_t v, size_t n) {
    uint8_t b[8];
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(v >> (8 * i));
    h->update(b, n);
}

} /* namespace detail */

/* The set of actions that a keybinding can take. */
struct Action {
    enum class CrashThread { main, io, render };

    struct CursorKey {
        std::string normal;
        std::string application;

        CursorKey clone() const { return *this; }

        std::tuple<const std::string &, const std::string &> fields() const { return std::tie(normal, application); }
    };

    enum class NavigateSearch { previous, next };

    enum class AdjustSelection {
        left,
        right,
        up,
        down,
        page_up,
        page_down,
        home,
        end,
        beginning_of_line,
        end_of_line,
    };

    enum class SplitDirection {
        right,
        down,
        left,
        up,
        auto_, /* splits along the larger direction */
    };

    enum class SplitFocusDirection {
        previous,
        next,
        up,
        left,
        down,
        right,
    };

    enum class SplitResizeDirection { up, down, left, right };

    enum class GotoWindow { previous, next };

    struct SplitResizeParameter {
        SplitResizeDirection direction;
        uint16_t amount;

        std::tuple<const SplitResizeDirection &, const uint16_t &> fields() const {
            return std::tie(direction, amount);
        }
    };

    enum class CopyToClipboard {
        plain,
        vt,
        html,

        /* This type will mix multiple distinct types with a set content-type
         * such as text/html for html, so that the OS/application can choose
         * what is best when pasting. */
        mixed,
    };

    struct WriteScreen {
        enum class Action { copy, paste, open };
        enum class Format { plain, vt, html };

        Action action;
        Format emit;

        static WriteScreen copy() { return {Action::copy, Format::plain}; }
        static WriteScreen paste() { return {Action::paste, Format::plain}; }
        static WriteScreen open() { return {Action::open, Format::plain}; }

        static Error parse(const std::string &param, WriteScreen *out);

        WriteScreen clone() const { return *this; }

        std::string format() const;

        std::tuple<const Action &, const Format &> fields() const { return std::tie(action, emit); }
    };

    /* Extern because it is used in the embedded runtime ABI. */
    enum class InspectorMode { toggle, show, hide };

    enum class CloseTabMode { this_, other, right };

    enum class OpenConfig {
        /* Open the config in the OS default editor. */
        os_open,

        /* Open the config in a new window using $EDITOR or $VISUAL */
        new_window,
    };

/* Wisp: X(cpp_name, "upstream name", PayloadKind) in upstream order. */
#define WISP_BINDING_ACTIONS(X)                                         \
    X(ignore, "ignore", Void)                                           \
    X(unbind, "unbind", Void)                                           \
    X(csi, "csi", Str)                                                  \
    X(esc, "esc", Str)                                                  \
    X(text, "text", Str)                                                \
    X(cursor_key, "cursor_key", CursorKey)                              \
    X(reset, "reset", Void)                                             \
    X(copy_to_clipboard, "copy_to_clipboard", CopyToClipboard)          \
    X(paste_from_clipboard, "paste_from_clipboard", Void)               \
    X(paste_from_selection, "paste_from_selection", Void)               \
    X(copy_url_to_clipboard, "copy_url_to_clipboard", Void)             \
    X(copy_title_to_clipboard, "copy_title_to_clipboard", Void)         \
    X(increase_font_size, "increase_font_size", F32)                    \
    X(decrease_font_size, "decrease_font_size", F32)                    \
    X(reset_font_size, "reset_font_size", Void)                         \
    X(set_font_size, "set_font_size", F32)                              \
    X(search, "search", Str)                                            \
    X(search_selection, "search_selection", Void)                       \
    X(navigate_search, "navigate_search", NavigateSearch)               \
    X(start_search, "start_search", Void)                               \
    X(end_search, "end_search", Void)                                   \
    X(clear_screen, "clear_screen", Void)                               \
    X(select_all, "select_all", Void)                                   \
    X(scroll_to_top, "scroll_to_top", Void)                             \
    X(scroll_to_bottom, "scroll_to_bottom", Void)                       \
    X(scroll_to_selection, "scroll_to_selection", Void)                 \
    X(scroll_to_row, "scroll_to_row", Usize)                            \
    X(scroll_page_up, "scroll_page_up", Void)                           \
    X(scroll_page_down, "scroll_page_down", Void)                       \
    X(scroll_page_fractional, "scroll_page_fractional", F32)            \
    X(scroll_page_lines, "scroll_page_lines", I16)                      \
    X(adjust_selection, "adjust_selection", AdjustSelection)            \
    X(jump_to_prompt, "jump_to_prompt", I16)                            \
    X(write_scrollback_file, "write_scrollback_file", WriteScreen)      \
    X(write_screen_file, "write_screen_file", WriteScreen)              \
    X(write_selection_file, "write_selection_file", WriteScreen)        \
    X(new_window, "new_window", Void)                                   \
    X(new_tab, "new_tab", Void)                                         \
    X(previous_tab, "previous_tab", Void)                               \
    X(next_tab, "next_tab", Void)                                       \
    X(last_tab, "last_tab", Void)                                       \
    X(goto_tab, "goto_tab", Usize)                                      \
    X(move_tab, "move_tab", Isize)                                      \
    X(move_tab_to_new_window, "move_tab_to_new_window", Void)           \
    X(toggle_tab_overview, "toggle_tab_overview", Void)                 \
    X(prompt_surface_title, "prompt_surface_title", Void)               \
    X(prompt_tab_title, "prompt_tab_title", Void)                       \
    X(prompt_window_title, "prompt_window_title", Void)                 \
    X(set_surface_title, "set_surface_title", Str)                      \
    X(set_tab_title, "set_tab_title", Str)                              \
    X(set_window_title, "set_window_title", Str)                        \
    X(new_split, "new_split", SplitDirection)                           \
    X(goto_split, "goto_split", SplitFocusDirection)                    \
    X(goto_window, "goto_window", GotoWindow)                           \
    X(toggle_split_zoom, "toggle_split_zoom", Void)                     \
    X(toggle_readonly, "toggle_readonly", Void)                         \
    X(resize_split, "resize_split", SplitResizeParameter)               \
    X(equalize_splits, "equalize_splits", Void)                         \
    X(reset_window_size, "reset_window_size", Void)                     \
    X(inspector, "inspector", InspectorMode)                            \
    X(show_gtk_inspector, "show_gtk_inspector", Void)                   \
    X(show_on_screen_keyboard, "show_on_screen_keyboard", Void)         \
    X(open_config, "open_config", OpenConfig)                           \
    X(reload_config, "reload_config", Void)                             \
    X(close_surface, "close_surface", Void)                             \
    X(close_tab, "close_tab", CloseTabMode)                             \
    X(close_window, "close_window", Void)                               \
    X(close_all_windows, "close_all_windows", Void)                     \
    X(toggle_maximize, "toggle_maximize", Void)                         \
    X(toggle_fullscreen, "toggle_fullscreen", Void)                     \
    X(toggle_window_decorations, "toggle_window_decorations", Void)     \
    X(toggle_window_float_on_top, "toggle_window_float_on_top", Void)   \
    X(toggle_secure_input, "toggle_secure_input", Void)                 \
    X(toggle_mouse_reporting, "toggle_mouse_reporting", Void)           \
    X(toggle_command_palette, "toggle_command_palette", Void)           \
    X(toggle_quick_terminal, "toggle_quick_terminal", Void)             \
    X(toggle_visibility, "toggle_visibility", Void)                     \
    X(toggle_background_opacity, "toggle_background_opacity", Void)    \
    X(check_for_updates, "check_for_updates", Void)                     \
    X(undo, "undo", Void)                                               \
    X(redo, "redo", Void)                                               \
    X(end_key_sequence, "end_key_sequence", Void)                       \
    X(activate_key_table, "activate_key_table", Str)                    \
    X(activate_key_table_once, "activate_key_table_once", Str)          \
    X(deactivate_key_table, "deactivate_key_table", Void)               \
    X(deactivate_all_key_tables, "deactivate_all_key_tables", Void)     \
    X(quit, "quit", Void)                                               \
    X(crash, "crash", CrashThread)

    /* Wisp: @typeInfo(Action).@"union".tag_type */
    enum class Key : int {
#define WISP_BINDING_ACTION_TAG(cpp, name, kind) cpp,
        WISP_BINDING_ACTIONS(WISP_BINDING_ACTION_TAG)
#undef WISP_BINDING_ACTION_TAG
    };

    /* Wisp: the payload type of each tag. */
    enum class Kind {
        Void,
        Str,
        CursorKey,
        CopyToClipboard,
        F32,
        NavigateSearch,
        Usize,
        I16,
        AdjustSelection,
        WriteScreen,
        Isize,
        SplitDirection,
        SplitFocusDirection,
        GotoWindow,
        SplitResizeParameter,
        InspectorMode,
        OpenConfig,
        CloseTabMode,
        CrashThread,
    };

    static const size_t key_count = 0
#define WISP_BINDING_ACTION_COUNT(cpp, name, kind) +1
        WISP_BINDING_ACTIONS(WISP_BINDING_ACTION_COUNT)
#undef WISP_BINDING_ACTION_COUNT
        ;

    /* Wisp: @tagName */
    static const char *keyName(Key k) {
        static const char *const names[] = {
#define WISP_BINDING_ACTION_NAME(cpp, name, kind) name,
            WISP_BINDING_ACTIONS(WISP_BINDING_ACTION_NAME)
#undef WISP_BINDING_ACTION_NAME
        };
        return names[(size_t)k];
    }

    static Kind keyKind(Key k) {
        static const Kind kinds[] = {
#define WISP_BINDING_ACTION_KIND(cpp, name, kind) Kind::kind,
            WISP_BINDING_ACTIONS(WISP_BINDING_ACTION_KIND)
#undef WISP_BINDING_ACTION_KIND
        };
        return kinds[(size_t)k];
    }

    /* The active tag. */
    Key tag = Key::ignore;

    /* Wisp: one field per payload type; only the one for `tag`'s kind is
     * meaningful. */
    std::string str;
    CursorKey cursor_key;
    CopyToClipboard copy_to_clipboard = CopyToClipboard::mixed;
    float f32 = 0;
    NavigateSearch navigate_search = NavigateSearch::previous;
    uint64_t usize = 0;
    int16_t i16 = 0;
    AdjustSelection adjust_selection = AdjustSelection::left;
    WriteScreen write_screen = WriteScreen::copy();
    int64_t isize = 0;
    SplitDirection split_direction = SplitDirection::auto_;
    SplitFocusDirection split_focus_direction = SplitFocusDirection::previous;
    GotoWindow goto_window = GotoWindow::previous;
    SplitResizeParameter split_resize_parameter = {SplitResizeDirection::up, 0};
    InspectorMode inspector_mode = InspectorMode::toggle;
    OpenConfig open_config = OpenConfig::os_open;
    CloseTabMode close_tab_mode = CloseTabMode::this_;
    CrashThread crash_thread = CrashThread::main;

    Kind kind() const { return keyKind(tag); }

    /* Wisp: `.name` / `.{ .name = value }` constructors. */
    static Action make(Key k) {
        Action a;
        a.tag = k;
        return a;
    }
    static Action makeStr(Key k, const std::string &s) {
        Action a;
        a.tag = k;
        a.str = s;
        return a;
    }

    /* ---- enum name tables (std.meta.stringToEnum / {t}) ---- */
    static const char *const (&navigateSearchNames())[2] {
        static const char *const n[2] = {"previous", "next"};
        return n;
    }
    static const char *const (&adjustSelectionNames())[10] {
        static const char *const n[10] = {"left", "right", "up", "down", "page_up", "page_down",
                                          "home", "end", "beginning_of_line", "end_of_line"};
        return n;
    }
    static const char *const (&splitDirectionNames())[5] {
        static const char *const n[5] = {"right", "down", "left", "up", "auto"};
        return n;
    }
    static const char *const (&splitFocusDirectionNames())[6] {
        static const char *const n[6] = {"previous", "next", "up", "left", "down", "right"};
        return n;
    }
    static const char *const (&splitResizeDirectionNames())[4] {
        static const char *const n[4] = {"up", "down", "left", "right"};
        return n;
    }
    static const char *const (&gotoWindowNames())[2] {
        static const char *const n[2] = {"previous", "next"};
        return n;
    }
    static const char *const (&copyToClipboardNames())[4] {
        static const char *const n[4] = {"plain", "vt", "html", "mixed"};
        return n;
    }
    static const char *const (&writeScreenActionNames())[3] {
        static const char *const n[3] = {"copy", "paste", "open"};
        return n;
    }
    static const char *const (&writeScreenFormatNames())[3] {
        static const char *const n[3] = {"plain", "vt", "html"};
        return n;
    }
    static const char *const (&inspectorModeNames())[3] {
        static const char *const n[3] = {"toggle", "show", "hide"};
        return n;
    }
    static const char *const (&closeTabModeNames())[3] {
        static const char *const n[3] = {"this", "other", "right"};
        return n;
    }
    static const char *const (&openConfigNames())[2] {
        static const char *const n[2] = {"os_open", "new_window"};
        return n;
    }
    static const char *const (&crashThreadNamesArr())[3] {
        static const char *const n[3] = {"main", "io", "render"};
        return n;
    }

    template <typename E, size_t N>
    static Error parseEnum(const char *const (&names)[N], const std::string &value, E *out) {
        if (!detail::enumFromName(names, value, out)) return Error::InvalidFormat;
        return Error::none;
    }

    static Error parseSplitFocusDirection(const std::string &input, SplitFocusDirection *out) {
        if (detail::enumFromName(splitFocusDirectionNames(), input, out)) return Error::none;
        /* For backwards compatibility we map "top" and "bottom" onto the enum
         * values "up" and "down" */
        if (input == "top") {
            *out = SplitFocusDirection::up;
            return Error::none;
        } else if (input == "bottom") {
            *out = SplitFocusDirection::down;
            return Error::none;
        }
        return Error::InvalidFormat;
    }

    /* Wisp: the parseParameter arm for one payload kind. */
    Error parseParameter(const std::string &param) {
        switch (kind()) {
            case Kind::CopyToClipboard: return parseEnum(copyToClipboardNames(), param, &copy_to_clipboard);
            case Kind::F32: return detail::parseFloat32(param, &f32) ? Error::none : Error::InvalidFormat;
            case Kind::NavigateSearch: return parseEnum(navigateSearchNames(), param, &navigate_search);
            case Kind::Usize: return detail::parseIntBase10(param, &usize) ? Error::none : Error::InvalidFormat;
            case Kind::I16: return detail::parseIntBase10(param, &i16) ? Error::none : Error::InvalidFormat;
            case Kind::AdjustSelection: return parseEnum(adjustSelectionNames(), param, &adjust_selection);
            case Kind::WriteScreen: return WriteScreen::parse(param, &write_screen);
            case Kind::Isize: return detail::parseIntBase10(param, &isize) ? Error::none : Error::InvalidFormat;
            case Kind::SplitDirection: return parseEnum(splitDirectionNames(), param, &split_direction);
            case Kind::SplitFocusDirection: return parseSplitFocusDirection(param, &split_focus_direction);
            case Kind::GotoWindow: return parseEnum(gotoWindowNames(), param, &goto_window);
            case Kind::SplitResizeParameter: {
                /* Only tuples are supported to avoid ambiguity with field
                 * ordering */
                std::string parts[3];
                size_t count = 0;
                size_t start = 0;
                while (true) {
                    const size_t comma = param.find(',', start);
                    if (count < 3) parts[count] = param.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                    count++;
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
                if (count < 2) return Error::InvalidFormat;
                SplitResizeParameter value;
                Error err = parseEnum(splitResizeDirectionNames(), parts[0], &value.direction);
                if (err != Error::none) return err;
                if (!detail::parseIntBase10(parts[1], &value.amount)) return Error::InvalidFormat;

                /* If we have extra parameters it is an error */
                if (count > 2) return Error::InvalidFormat;

                split_resize_parameter = value;
                return Error::none;
            }
            case Kind::InspectorMode: return parseEnum(inspectorModeNames(), param, &inspector_mode);
            case Kind::OpenConfig: return parseEnum(openConfigNames(), param, &open_config);
            case Kind::CloseTabMode: return parseEnum(closeTabModeNames(), param, &close_tab_mode);
            case Kind::CrashThread: return parseEnum(crashThreadNamesArr(), param, &crash_thread);
            case Kind::Void:
            case Kind::Str:
            case Kind::CursorKey: break;
        }
        assert(false && "unreachable");
        return Error::InvalidFormat;
    }

    /* Wisp: `@hasDecl(field.type, "default")`; sets the default and
     * returns true for the payload types that declare one. */
    bool setDefault() {
        switch (kind()) {
            case Kind::SplitDirection: split_direction = SplitDirection::auto_; return true;
            case Kind::CopyToClipboard: copy_to_clipboard = CopyToClipboard::mixed; return true;
            case Kind::CloseTabMode: close_tab_mode = CloseTabMode::this_; return true;
            case Kind::OpenConfig: open_config = OpenConfig::os_open; return true;
            default: return false;
        }
    }

    /* Parse an action in the format of "key=value" where key is the
     * action name and value is the action parameter. The parameter
     * is optional depending on the action. */
    static Error parse(const std::string &input, Action *out) {
        /* Split our action by colon. A colon may not exist for some
         * actions so it is optional. The part preceding the colon is the
         * action name. */
        const size_t colonIdx = input.find(':');
        const bool has_colon = colonIdx != std::string::npos;
        const std::string action = input.substr(0, has_colon ? colonIdx : input.size());

        /* An action name is always required */
        if (action.empty()) return Error::InvalidFormat;

        for (size_t i = 0; i < key_count; i++) {
            if (action != keyName((Key)i)) continue;
            Action result = make((Key)i);

            switch (result.kind()) {
                /* If the field type is void we expect no value */
                case Kind::Void:
                    if (has_colon) return Error::InvalidFormat;
                    *out = result;
                    return Error::none;

                case Kind::Str:
                    if (!has_colon) return Error::InvalidFormat;
                    result.str = input.substr(colonIdx + 1);
                    *out = result;
                    return Error::none;

                /* Cursor keys can't be set currently */
                case Kind::CursorKey: return Error::InvalidAction;

                default: {
                    /* Get the parameter after the colon. The parameter
                     * can be optional for action types that can have a
                     * "default" decl. */
                    if (!has_colon) {
                        if (result.setDefault()) {
                            *out = result;
                            return Error::none;
                        }

                        return Error::InvalidFormat;
                    }

                    const Error err = result.parseParameter(input.substr(colonIdx + 1));
                    if (err != Error::none) return err;
                    *out = result;
                    return Error::none;
                }
            }
        }

        return Error::InvalidAction;
    }

    /* The scope of an action. The scope is the context in which an action
     * must be executed. */
    enum class Scope { app, surface };

    /* Returns the scope of an action. */
    Scope scope() const {
        switch (tag) {
            /* Doesn't really matter, so we'll see app. */
            case Key::ignore:
            case Key::unbind: return Scope::app;

            /* Obviously app actions. */
            case Key::open_config:
            case Key::reload_config:
            case Key::close_all_windows:
            case Key::quit:
            case Key::toggle_quick_terminal:
            case Key::toggle_visibility:
            case Key::check_for_updates:
            case Key::show_gtk_inspector: return Scope::app;

            /* These are app but can be special-cased in a surface context. */
            case Key::new_window:
            case Key::undo:
            case Key::redo: return Scope::app;

            /* Obviously surface actions. */
            case Key::csi:
            case Key::esc:
            case Key::text:
            case Key::cursor_key:
            case Key::search:
            case Key::navigate_search:
            case Key::search_selection:
            case Key::start_search:
            case Key::end_search:
            case Key::reset:
            case Key::copy_to_clipboard:
            case Key::copy_url_to_clipboard:
            case Key::copy_title_to_clipboard:
            case Key::paste_from_clipboard:
            case Key::paste_from_selection:
            case Key::increase_font_size:
            case Key::decrease_font_size:
            case Key::reset_font_size:
            case Key::set_font_size:
            case Key::prompt_surface_title:
            case Key::prompt_tab_title:
            case Key::prompt_window_title:
            case Key::set_surface_title:
            case Key::set_tab_title:
            case Key::set_window_title:
            case Key::clear_screen:
            case Key::select_all:
            case Key::scroll_to_top:
            case Key::scroll_to_bottom:
            case Key::scroll_to_selection:
            case Key::scroll_to_row:
            case Key::scroll_page_up:
            case Key::scroll_page_down:
            case Key::scroll_page_fractional:
            case Key::scroll_page_lines:
            case Key::adjust_selection:
            case Key::jump_to_prompt:
            case Key::write_scrollback_file:
            case Key::write_screen_file:
            case Key::write_selection_file:
            case Key::close_surface:
            case Key::close_tab:
            case Key::close_window:
            case Key::toggle_maximize:
            case Key::toggle_fullscreen:
            case Key::toggle_window_decorations:
            case Key::toggle_window_float_on_top:
            case Key::toggle_secure_input:
            case Key::toggle_mouse_reporting:
            case Key::toggle_command_palette:
            case Key::toggle_background_opacity:
            case Key::show_on_screen_keyboard:
            case Key::reset_window_size:
            case Key::activate_key_table:
            case Key::activate_key_table_once:
            case Key::deactivate_key_table:
            case Key::deactivate_all_key_tables:
            case Key::end_key_sequence:
            case Key::crash: return Scope::surface;

            /* These are less obvious surface actions. They're surface
             * actions because they are relevant to the surface they
             * come from. For example `new_window` needs to be sourced to
             * a surface so inheritance can be done correctly. */
            case Key::new_tab:
            case Key::previous_tab:
            case Key::next_tab:
            case Key::last_tab:
            case Key::goto_tab:
            case Key::move_tab:
            case Key::move_tab_to_new_window:
            case Key::toggle_tab_overview:
            case Key::new_split:
            case Key::goto_split:
            case Key::goto_window:
            case Key::toggle_split_zoom:
            case Key::toggle_readonly:
            case Key::resize_split:
            case Key::equalize_splits:
            case Key::inspector: return Scope::surface;
        }
        return Scope::surface;
    }

    /* Returns the scoped version of this action. If the action is not
     * scoped to the given scope then this returns null.
     *
     * Wisp: see the header; true means this action is the scoped value. */
    bool scoped(Scope s) const { return scope() == s; }

    /* Implements the formatter for the fmt package. This encodes the
     * action back into the format used by parse. */
    std::string format() const {
        /* All actions start with the tag. */
        std::string writer = keyName(tag);

        /* Only write the value depending on the type if it's not void */
        if (kind() != Kind::Void) {
            writer.push_back(':');
            formatValue(&writer);
        }
        return writer;
    }

    void formatValue(std::string *writer) const {
        switch (kind()) {
            case Kind::Void: break;
            case Kind::Str: writer->append(str); break;
            case Kind::CursorKey: assert(false && "formatting not supported"); break;
            case Kind::CopyToClipboard: writer->append(copyToClipboardNames()[(size_t)copy_to_clipboard]); break;
            case Kind::F32: writer->append(detail::formatFloat(f32)); break;
            case Kind::NavigateSearch: writer->append(navigateSearchNames()[(size_t)navigate_search]); break;
            case Kind::Usize: writer->append(std::to_string(usize)); break;
            case Kind::I16: writer->append(std::to_string(i16)); break;
            case Kind::AdjustSelection: writer->append(adjustSelectionNames()[(size_t)adjust_selection]); break;
            case Kind::WriteScreen: writer->append(write_screen.format()); break;
            case Kind::Isize: writer->append(std::to_string(isize)); break;
            case Kind::SplitDirection: writer->append(splitDirectionNames()[(size_t)split_direction]); break;
            case Kind::SplitFocusDirection:
                writer->append(splitFocusDirectionNames()[(size_t)split_focus_direction]);
                break;
            case Kind::GotoWindow: writer->append(gotoWindowNames()[(size_t)goto_window]); break;
            case Kind::SplitResizeParameter:
                writer->append(splitResizeDirectionNames()[(size_t)split_resize_parameter.direction]);
                writer->push_back(',');
                writer->append(std::to_string(split_resize_parameter.amount));
                break;
            case Kind::InspectorMode: writer->append(inspectorModeNames()[(size_t)inspector_mode]); break;
            case Kind::OpenConfig: writer->append(openConfigNames()[(size_t)open_config]); break;
            case Kind::CloseTabMode: writer->append(closeTabModeNames()[(size_t)close_tab_mode]); break;
            case Kind::CrashThread: writer->append(crashThreadNamesArr()[(size_t)crash_thread]); break;
        }
    }

    /* Clone this action with the given allocator. The allocator
     * should be an arena-style allocator since fine-grained
     * deallocation is not possible.
     *
     * Wisp: payloads are values, so this is a copy. */
    Action clone() const { return *this; }

    /* Returns a hash code that can be used to uniquely identify this
     * action. */
    uint64_t hash() const {
        zigstd::Wyhash hasher = zigstd::Wyhash::init(0);
        hashIncremental(&hasher);
        return hasher.final_();
    }

    /* Hash the action into the given hasher. */
    void hashIncremental(zigstd::Wyhash *hasher) const {
        /* Always has the active tag. */
        detail::hashBytes(hasher, (uint64_t)(int)tag, 1);

        /* Hash the value of the field. */
        switch (kind()) {
            /* Do nothing for void */
            case Kind::Void: break;

            /* Floats are hashed by their bits. This is totally not
             * portable and there are edge cases such as NaNs and
             * signed zeros but these are not cases we expect for
             * our bindings. */
            case Kind::F32: {
                uint32_t bits;
                memcpy(&bits, &f32, 4);
                detail::hashBytes(hasher, bits, 4);
                break;
            }

            /* Everything else automatically handle. */
            case Kind::Str: hasher->update((const uint8_t *)str.data(), str.size()); break;
            case Kind::CursorKey:
                hasher->update((const uint8_t *)cursor_key.normal.data(), cursor_key.normal.size());
                hasher->update((const uint8_t *)cursor_key.application.data(), cursor_key.application.size());
                break;
            case Kind::Usize: detail::hashBytes(hasher, usize, 8); break;
            case Kind::I16: detail::hashBytes(hasher, (uint16_t)i16, 2); break;
            case Kind::Isize: detail::hashBytes(hasher, (uint64_t)isize, 8); break;
            case Kind::WriteScreen:
                detail::hashBytes(hasher, (uint64_t)write_screen.action, 1);
                detail::hashBytes(hasher, (uint64_t)write_screen.emit, 1);
                break;
            case Kind::SplitResizeParameter:
                detail::hashBytes(hasher, (uint64_t)split_resize_parameter.direction, 1);
                detail::hashBytes(hasher, split_resize_parameter.amount, 2);
                break;
            case Kind::CopyToClipboard: detail::hashBytes(hasher, (uint64_t)copy_to_clipboard, 1); break;
            case Kind::NavigateSearch: detail::hashBytes(hasher, (uint64_t)navigate_search, 1); break;
            case Kind::AdjustSelection: detail::hashBytes(hasher, (uint64_t)adjust_selection, 1); break;
            case Kind::SplitDirection: detail::hashBytes(hasher, (uint64_t)split_direction, 1); break;
            case Kind::SplitFocusDirection: detail::hashBytes(hasher, (uint64_t)split_focus_direction, 1); break;
            case Kind::GotoWindow: detail::hashBytes(hasher, (uint64_t)goto_window, 1); break;
            case Kind::InspectorMode: detail::hashBytes(hasher, (uint64_t)inspector_mode, 1); break;
            case Kind::OpenConfig: detail::hashBytes(hasher, (uint64_t)open_config, 1); break;
            case Kind::CloseTabMode: detail::hashBytes(hasher, (uint64_t)close_tab_mode, 1); break;
            case Kind::CrashThread: detail::hashBytes(hasher, (uint64_t)crash_thread, 1); break;
        }
    }

    /* Compares two actions for equality. */
    bool equal(const Action &other) const {
        using ::wisp::datastruct::comparison::deepEqual;
        if (tag != other.tag) return false;
        switch (kind()) {
            case Kind::Void: return true;
            case Kind::Str: return deepEqual(str, other.str);
            case Kind::CursorKey: return deepEqual(cursor_key, other.cursor_key);
            case Kind::CopyToClipboard: return deepEqual(copy_to_clipboard, other.copy_to_clipboard);
            case Kind::F32: return deepEqual(f32, other.f32);
            case Kind::NavigateSearch: return deepEqual(navigate_search, other.navigate_search);
            case Kind::Usize: return deepEqual(usize, other.usize);
            case Kind::I16: return deepEqual(i16, other.i16);
            case Kind::AdjustSelection: return deepEqual(adjust_selection, other.adjust_selection);
            case Kind::WriteScreen: return deepEqual(write_screen, other.write_screen);
            case Kind::Isize: return deepEqual(isize, other.isize);
            case Kind::SplitDirection: return deepEqual(split_direction, other.split_direction);
            case Kind::SplitFocusDirection: return deepEqual(split_focus_direction, other.split_focus_direction);
            case Kind::GotoWindow: return deepEqual(goto_window, other.goto_window);
            case Kind::SplitResizeParameter: return deepEqual(split_resize_parameter, other.split_resize_parameter);
            case Kind::InspectorMode: return deepEqual(inspector_mode, other.inspector_mode);
            case Kind::OpenConfig: return deepEqual(open_config, other.open_config);
            case Kind::CloseTabMode: return deepEqual(close_tab_mode, other.close_tab_mode);
            case Kind::CrashThread: return deepEqual(crash_thread, other.crash_thread);
        }
        return false;
    }
};

inline Error Action::WriteScreen::parse(const std::string &param, WriteScreen *out) {
    /* If we don't have a `,`, default to the plain format. This is
     * also very important for backwards compatibility before Ghostty
     * 1.3 which didn't support output formats. */
    const size_t idx = param.find(',');
    if (idx == std::string::npos) {
        WriteScreen result;
        const Error err = ::wisp::input::binding::Action::parseEnum(::wisp::input::binding::Action::writeScreenActionNames(), param, &result.action);
        if (err != Error::none) return err;
        result.emit = Format::plain;
        *out = result;
        return Error::none;
    }

    WriteScreen result;
    Error err = ::wisp::input::binding::Action::parseEnum(::wisp::input::binding::Action::writeScreenActionNames(), param.substr(0, idx), &result.action);
    if (err != Error::none) return err;
    err = ::wisp::input::binding::Action::parseEnum(::wisp::input::binding::Action::writeScreenFormatNames(), param.substr(idx + 1), &result.emit);
    if (err != Error::none) return err;
    *out = result;
    return Error::none;
}

inline std::string Action::WriteScreen::format() const {
    std::string writer = ::wisp::input::binding::Action::writeScreenActionNames()[(size_t)action];
    writer.push_back(',');
    writer.append(::wisp::input::binding::Action::writeScreenFormatNames()[(size_t)emit]);
    return writer;
}

/* Trigger is the associated key state that can trigger an action.
 * This is an extern struct because this is also used in the C API.
 *
 * This must be kept in sync with include/ghostty.h ghostty_input_trigger_s */
struct Trigger {
    /* The extern struct used for triggers in the C API. */
    struct C {
        enum class Tag : int {
            physical,
            unicode,
            catch_all,
        };

        /* Wisp: extern union { physical: key.Key, unicode: u32 }. */
        union Key {
            ::wisp::input::Key physical;
            uint32_t unicode;
        };

        Tag tag = Tag::physical;
        C::Key key = {::wisp::input::Key::unidentified};
        Mods mods;
    };

    /* Wisp: union(C.Tag) { physical: key.Key, unicode: u21, catch_all }. */
    struct Key {
        C::Tag tag = C::Tag::physical;

        /* key is the "physical" version. This is the same as mapped for
         * standard US keyboard layouts. For non-US keyboard layouts, this
         * is used to bind to a physical key location rather than a translated
         * key. */
        ::wisp::input::Key physical = ::wisp::input::Key::unidentified;

        /* This is used for binding to keys that produce a certain unicode
         * codepoint. This is useful for binding to keys that don't have a
         * registered keycode with Ghostty. */
        uint32_t unicode = 0;

        /* A catch-all key that matches any key press that is otherwise
         * unbound. (no payload) */

        static Key makePhysical(::wisp::input::Key k) {
            Key r;
            r.tag = C::Tag::physical;
            r.physical = k;
            return r;
        }
        static Key makeUnicode(uint32_t cp) {
            Key r;
            r.tag = C::Tag::unicode;
            r.unicode = cp;
            return r;
        }
        static Key makeCatchAll() {
            Key r;
            r.tag = C::Tag::catch_all;
            return r;
        }
    };

    /* The key that has to be pressed for a binding to take action. */
    Key key;

    /* The key modifiers that must be active for this to match. */
    Mods mods;

    /* Wisp: the bool fields of key.Mods, by upstream name. */
    static bool modField(Mods *mods, const std::string &name, bool **field) {
        if (name == "shift") *field = &mods->shift;
        else if (name == "ctrl") *field = &mods->ctrl;
        else if (name == "alt") *field = &mods->alt;
        else if (name == "super") *field = &mods->super;
        else if (name == "caps_lock") *field = &mods->caps_lock;
        else if (name == "num_lock") *field = &mods->num_lock;
        else return false;
        return true;
    }

    /* The values that are backwards compatible with Ghostty 1.1.x.
     * Ghostty 1.2+ doesn't support these anymore since we moved to
     * W3C key codes. */
    /* Wisp: std.StaticStringMap(Key); `keys()`/`values()` are the entries. */
    struct BackwardsEntry {
        const char *name;
        bool unicode;
        uint32_t cp;
        ::wisp::input::Key physical;

        Key value() const { return unicode ? Key::makeUnicode(cp) : Key::makePhysical(physical); }
    };

    static const BackwardsEntry *backwardsCompatibleKeys(size_t *len) {
        typedef ::wisp::input::Key K;
        static const BackwardsEntry table[] = {
            {"zero", true, '0', K::unidentified},
            {"one", true, '1', K::unidentified},
            {"two", true, '2', K::unidentified},
            {"three", true, '3', K::unidentified},
            {"four", true, '4', K::unidentified},
            {"five", true, '5', K::unidentified},
            {"six", true, '6', K::unidentified},
            {"seven", true, '7', K::unidentified},
            {"eight", true, '8', K::unidentified},
            {"nine", true, '9', K::unidentified},
            {"plus", true, '+', K::unidentified},
            {"apostrophe", true, '\'', K::unidentified},
            {"grave_accent", false, 0, K::backquote},
            {"left_bracket", false, 0, K::bracket_left},
            {"right_bracket", false, 0, K::bracket_right},
            {"up", false, 0, K::arrow_up},
            {"down", false, 0, K::arrow_down},
            {"left", false, 0, K::arrow_left},
            {"right", false, 0, K::arrow_right},
            {"kp_0", false, 0, K::numpad_0},
            {"kp_1", false, 0, K::numpad_1},
            {"kp_2", false, 0, K::numpad_2},
            {"kp_3", false, 0, K::numpad_3},
            {"kp_4", false, 0, K::numpad_4},
            {"kp_5", false, 0, K::numpad_5},
            {"kp_6", false, 0, K::numpad_6},
            {"kp_7", false, 0, K::numpad_7},
            {"kp_8", false, 0, K::numpad_8},
            {"kp_9", false, 0, K::numpad_9},
            {"kp_add", false, 0, K::numpad_add},
            {"kp_subtract", false, 0, K::numpad_subtract},
            {"kp_multiply", false, 0, K::numpad_multiply},
            {"kp_divide", false, 0, K::numpad_divide},
            {"kp_decimal", false, 0, K::numpad_decimal},
            {"kp_enter", false, 0, K::numpad_enter},
            {"kp_equal", false, 0, K::numpad_equal},
            {"kp_separator", false, 0, K::numpad_separator},
            {"kp_left", false, 0, K::numpad_left},
            {"kp_right", false, 0, K::numpad_right},
            {"kp_up", false, 0, K::numpad_up},
            {"kp_down", false, 0, K::numpad_down},
            {"kp_page_up", false, 0, K::numpad_page_up},
            {"kp_page_down", false, 0, K::numpad_page_down},
            {"kp_home", false, 0, K::numpad_home},
            {"kp_end", false, 0, K::numpad_end},
            {"kp_insert", false, 0, K::numpad_insert},
            {"kp_delete", false, 0, K::numpad_delete},
            {"kp_begin", false, 0, K::numpad_begin},
            {"left_shift", false, 0, K::shift_left},
            {"right_shift", false, 0, K::shift_right},
            {"left_control", false, 0, K::control_left},
            {"right_control", false, 0, K::control_right},
            {"left_alt", false, 0, K::alt_left},
            {"right_alt", false, 0, K::alt_right},
            {"left_super", false, 0, K::meta_left},
            {"right_super", false, 0, K::meta_right},

            /* Physical variants. This is a blunt approach to this but its
             * glue for backwards compatibility so I'm not too worried about
             * making this super nice. */
            {"physical:zero", false, 0, K::digit_0},
            {"physical:one", false, 0, K::digit_1},
            {"physical:two", false, 0, K::digit_2},
            {"physical:three", false, 0, K::digit_3},
            {"physical:four", false, 0, K::digit_4},
            {"physical:five", false, 0, K::digit_5},
            {"physical:six", false, 0, K::digit_6},
            {"physical:seven", false, 0, K::digit_7},
            {"physical:eight", false, 0, K::digit_8},
            {"physical:nine", false, 0, K::digit_9},
            {"physical:apostrophe", false, 0, K::quote},
            {"physical:grave_accent", false, 0, K::backquote},
            {"physical:left_bracket", false, 0, K::bracket_left},
            {"physical:right_bracket", false, 0, K::bracket_right},
            {"physical:up", false, 0, K::arrow_up},
            {"physical:down", false, 0, K::arrow_down},
            {"physical:left", false, 0, K::arrow_left},
            {"physical:right", false, 0, K::arrow_right},
            {"physical:kp_0", false, 0, K::numpad_0},
            {"physical:kp_1", false, 0, K::numpad_1},
            {"physical:kp_2", false, 0, K::numpad_2},
            {"physical:kp_3", false, 0, K::numpad_3},
            {"physical:kp_4", false, 0, K::numpad_4},
            {"physical:kp_5", false, 0, K::numpad_5},
            {"physical:kp_6", false, 0, K::numpad_6},
            {"physical:kp_7", false, 0, K::numpad_7},
            {"physical:kp_8", false, 0, K::numpad_8},
            {"physical:kp_9", false, 0, K::numpad_9},
            {"physical:kp_add", false, 0, K::numpad_add},
            {"physical:kp_subtract", false, 0, K::numpad_subtract},
            {"physical:kp_multiply", false, 0, K::numpad_multiply},
            {"physical:kp_divide", false, 0, K::numpad_divide},
            {"physical:kp_decimal", false, 0, K::numpad_decimal},
            {"physical:kp_enter", false, 0, K::numpad_enter},
            {"physical:kp_equal", false, 0, K::numpad_equal},
            {"physical:kp_separator", false, 0, K::numpad_separator},
            {"physical:kp_left", false, 0, K::numpad_left},
            {"physical:kp_right", false, 0, K::numpad_right},
            {"physical:kp_up", false, 0, K::numpad_up},
            {"physical:kp_down", false, 0, K::numpad_down},
            {"physical:kp_page_up", false, 0, K::numpad_page_up},
            {"physical:kp_page_down", false, 0, K::numpad_page_down},
            {"physical:kp_home", false, 0, K::numpad_home},
            {"physical:kp_end", false, 0, K::numpad_end},
            {"physical:kp_insert", false, 0, K::numpad_insert},
            {"physical:kp_delete", false, 0, K::numpad_delete},
            {"physical:kp_begin", false, 0, K::numpad_begin},
            {"physical:left_shift", false, 0, K::shift_left},
            {"physical:right_shift", false, 0, K::shift_right},
            {"physical:left_control", false, 0, K::control_left},
            {"physical:right_control", false, 0, K::control_right},
            {"physical:left_alt", false, 0, K::alt_left},
            {"physical:right_alt", false, 0, K::alt_right},
            {"physical:left_super", false, 0, K::meta_left},
            {"physical:right_super", false, 0, K::meta_right},
        };
        *len = sizeof(table) / sizeof(table[0]);
        return table;
    }

    static bool backwardsCompatibleKey(const std::string &part, Key *out) {
        size_t len = 0;
        const BackwardsEntry *table = backwardsCompatibleKeys(&len);
        for (size_t i = 0; i < len; i++) {
            if (part == table[i].name) {
                *out = table[i].value();
                return true;
            }
        }
        return false;
    }

    /* Parse a single trigger. The input is expected to be ONLY the trigger
     * (i.e. in the sequence `a=ignore` input is only `a`). The trigger may
     * not be part of a sequence (i.e. `a>b`). This parses exactly a single
     * trigger. */
    static Error parse(const std::string &input, Trigger *out) {
        if (input.empty()) return Error::InvalidFormat;
        Trigger result;
        std::string rem = input;
        while (!rem.empty()) {
            const size_t plus = rem.find('+');
            const size_t idx = plus == std::string::npos ? rem.size() : plus;
            const std::string part = rem.substr(0, idx);
            rem = idx >= rem.size() ? std::string() : rem.substr(idx + 1);

            /* Check if its a modifier */
            bool *field = nullptr;
            if (modField(&result.mods, part, &field)) {
                /* Repeat not allowed */
                if (*field) return Error::InvalidFormat;
                *field = true;
                continue;
            }

            /* Alias modifiers */
            bool aliased = false;
            for (const ModAlias &pair : alias) {
                if (part == pair.name) {
                    /* Repeat not allowed */
                    bool *f = &result.mods.shift;
                    switch (pair.mod) {
                        case Mod::shift: f = &result.mods.shift; break;
                        case Mod::ctrl: f = &result.mods.ctrl; break;
                        case Mod::alt: f = &result.mods.alt; break;
                        case Mod::super: f = &result.mods.super; break;
                    }
                    if (*f) return Error::InvalidFormat;
                    *f = true;
                    aliased = true;
                    break;
                }
            }
            if (aliased) continue;

            /* Anything after this point is a key and we only support
             * single keys. */
            if (!result.isKeyUnset()) return Error::InvalidFormat;

            /* If the part is empty it means that it is actually
             * a literal `+`, which we treat as a Unicode character. */
            if (part.empty()) {
                result.key = Key::makeUnicode('+');
                continue;
            }

            /* Check if its a key */
            ::wisp::input::Key keyval = ::wisp::input::Key::unidentified;
            if (part != "unidentified" && keyFromName(part, &keyval)) {
                result.key = Key::makePhysical(keyval);
                continue;
            }

            /* If we're still unset and we have exactly one unicode
             * character then we can use that as a key. */
            if (result.isKeyUnset()) {
                /* Invalid UTF8 drops to invalid format */
                zigstd::Utf8View view;
                if (zigstd::Utf8View::init(part.data(), part.size(), &view)) {
                    /* No codepoints or multiple codepoints drops to invalid format */
                    uint32_t cp = 0;
                    uint32_t extra = 0;
                    if (view.nextCodepoint(&cp) && !view.nextCodepoint(&extra)) {
                        result.key = Key::makeUnicode(cp);
                        continue;
                    }
                }
            }

            /* Look for a matching w3c name next. */
            ::wisp::input::Key w3c_key = ::wisp::input::Key::unidentified;
            if (fromW3C(part, &w3c_key)) {
                result.key = Key::makePhysical(w3c_key);
                continue;
            }

            /* Check for catch_all. We do this near the end since its unlikely
             * in most cases that we're setting a catch-all key. */
            if (part == "catch_all") {
                result.key = Key::makeCatchAll();
                continue;
            }

            /* If we're still unset then we look for backwards compatible
             * keys with Ghostty 1.1.x. We do this last so its least likely
             * to impact performance for modern users. */
            Key old_key;
            if (backwardsCompatibleKey(part, &old_key)) {
                result.key = old_key;
                continue;
            }

            /* We didn't recognize this value */
            return Error::InvalidFormat;
        }

        *out = result;
        return Error::none;
    }

    /* Returns true if this trigger has no key set. */
    bool isKeyUnset() const {
        switch (key.tag) {
            case C::Tag::physical: return key.physical == ::wisp::input::Key::unidentified;
            case C::Tag::unicode:
            case C::Tag::catch_all: return false;
        }
        return false;
    }

    /* Returns a hash code that can be used to uniquely identify this trigger. */
    uint64_t hash() const {
        zigstd::Wyhash hasher = zigstd::Wyhash::init(0);
        hashIncremental(&hasher);
        return hasher.final_();
    }

    /* Hash the trigger into the given hasher. */
    void hashIncremental(zigstd::Wyhash *hasher) const {
        detail::hashBytes(hasher, (uint64_t)(int)key.tag, 4);
        switch (key.tag) {
            case C::Tag::physical: detail::hashBytes(hasher, (uint64_t)(int)key.physical, 4); break;
            case C::Tag::unicode: {
                uint32_t folded[3];
                foldedCodepoint(key.unicode, folded);
                for (size_t i = 0; i < 3; i++) detail::hashBytes(hasher, folded[i], 3);
                break;
            }
            case C::Tag::catch_all: break;
        }
        detail::hashBytes(hasher, mods.binding().int_(), 2);
    }

    /* The codepoint we use for comparisons. Case folding can result
     * in more codepoints so we need to use a 3 element array. */
    static void foldedCodepoint(uint32_t cp, uint32_t out[3]) {
        /* ASCII fast path */
        if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) {
            out[0] = (cp >= 'A' && cp <= 'Z') ? cp - 'A' + 'a' : cp;
            out[1] = 0;
            out[2] = 0;
            return;
        }

        /* Unicode slow path. Case folding can result in more codepoints.
         * If more codepoints are produced then we return the codepoint
         * as-is which isn't correct but until we have a failing test
         * then I don't want to handle this. */
        uint32_t mapping[3];
        const size_t len = ::wisp::vt::unicode::caseFoldingFull(cp, mapping);
        out[0] = 0;
        out[1] = 0;
        out[2] = 0;
        for (size_t i = 0; i < len; i++) out[i] = mapping[i];
    }

    /* Returns true if two triggers are equal. */
    bool equal(const Trigger &other) const {
        if (mods != other.mods) return false;
        if (key.tag != other.key.tag) return false;
        switch (key.tag) {
            case C::Tag::physical: return key.physical == other.key.physical;
            case C::Tag::unicode: return key.unicode == other.key.unicode;
            case C::Tag::catch_all: return true;
        }
        return false;
    }

    /* Returns true if two triggers are equal using folded codepoints. */
    bool foldedEqual(const Trigger &other) const {
        if (mods != other.mods) return false;
        if (key.tag != other.key.tag) return false;
        switch (key.tag) {
            case C::Tag::physical: return key.physical == other.key.physical;
            case C::Tag::unicode: {
                uint32_t a[3];
                uint32_t b[3];
                foldedCodepoint(key.unicode, a);
                foldedCodepoint(other.key.unicode, b);
                return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
            }
            case C::Tag::catch_all: return true;
        }
        return false;
    }

    /* Convert the trigger to a C API compatible trigger. */
    C cval() const {
        C c;
        c.tag = key.tag;
        switch (key.tag) {
            case C::Tag::physical: c.key.physical = key.physical; break;
            case C::Tag::unicode: c.key.unicode = key.unicode; break;
            /* catch_all has no associated value so its an error
             * for a C consumer to look at it. */
            case C::Tag::catch_all: break;
        }
        c.mods = mods;
        return c;
    }

    /* Format implementation for fmt package. */
    std::string format() const {
        std::string writer;
        /* Modifiers first */
        if (mods.super) writer.append("super+");
        if (mods.ctrl) writer.append("ctrl+");
        if (mods.alt) writer.append("alt+");
        if (mods.shift) writer.append("shift+");

        /* Key */
        switch (key.tag) {
            case C::Tag::physical: writer.append(keyName(key.physical)); break;
            case C::Tag::unicode: {
                uint8_t buf[4];
                const size_t n = zigstd::utf8Encode(key.unicode, buf);
                writer.append((const char *)buf, n);
                break;
            }
            case C::Tag::catch_all: writer.append("catch_all"); break;
        }
        return writer;
    }
};

/* Wisp: the Binding struct itself (upstream's file-level fields). */
struct Binding {
    /* The trigger that needs to be performed to execute the action. */
    Trigger trigger;

    /* The action to take if this binding matches */
    Action action;

    /* Boolean flags that can be set per binding. */
    Flags flags;
};

/* An iterator that yields each trigger in a sequence of triggers. For
 * example, the sequence "ctrl+a>ctrl+b" would yield "ctrl+a" and then
 * "ctrl+b". The iterator approach allows us to parse a sequence of
 * triggers without allocations. */
struct SequenceIterator {
    /* The input of triggers. This is expected to be ONLY triggers. Things
     * like the "unconsumed:" prefix or action must be stripped before
     * passing to this iterator. */
    std::string input;
    size_t i = 0;

    /* Returns the next trigger in the sequence if there is no parsing error.
     *
     * Wisp: `?Trigger` is `*has` plus `*out`. */
    Error next(Trigger *out, bool *has) {
        *has = false;
        if (done()) return Error::none;
        const std::string rem = input.substr(i);
        const size_t gt = rem.find('>');
        const size_t idx = gt == std::string::npos ? rem.size() : gt;
        i += idx + 1;
        const Error err = Trigger::parse(rem.substr(0, idx), out);
        if (err != Error::none) return err;
        *has = true;
        return Error::none;
    }

    /* Returns true if there are no more triggers to parse. */
    bool done() const { return i > input.size(); }
};

/* Full binding parser. The binding parser is implemented as an iterator
 * which yields elements to support multi-key sequences without allocation. */
struct Parser {
    SequenceIterator trigger_it;
    Action action;
    Flags flags;
    bool chain = false;

    struct Elem {
        enum class Tag {
            /* A leader trigger in a sequence. */
            leader,

            /* The final trigger and action in a sequence. */
            binding,

            /* A chained action `chain=<action>` that should be appended
             * to the previous binding. Note that any action is parsed, including
             * invalid actions for chains such as `unbind`. We expect downstream
             * consumers to validate that the action is valid for chaining. */
            chain,
        };
        Tag tag = Tag::leader;
        Trigger leader;
        Binding binding;
        Action chain;
    };

    static Error init(const std::string &raw_input, Parser *out) {
        Flags flags;
        size_t start_idx = 0;
        Error err = parseFlags(raw_input, &flags, &start_idx);
        if (err != Error::none) return err;
        const std::string input = raw_input.substr(start_idx);

        /* Find the equal sign. This is more complicated than it seems on
         * the surface because we need to ignore equal signs that are
         * part of the trigger. */
        size_t eql_idx = std::string::npos;
        {
            /* TODO: We should change this parser into a real state machine
             * based parser that parses the trigger fully, then yields the
             * action after. The loop below is a total mess. */
            size_t offset = 0;
            while (true) {
                const size_t idx = input.find('=', offset);
                if (idx == std::string::npos) break;
                /* Find: '=+ctrl' or '==action' */
                if (idx < input.size() - 1 && (input[idx + 1] == '+' || input[idx + 1] == '=')) {
                    offset = idx + 1;
                    continue;
                }

                /* Looks like the real equal sign. */
                eql_idx = idx;
                break;
            }

            if (eql_idx == std::string::npos) return Error::InvalidFormat;
        }

        /* Detect chains. Chains must not have flags. */
        const bool chain = input.substr(0, eql_idx) == "chain";
        if (chain && start_idx > 0) return Error::InvalidFormat;

        /* Sequence iterator goes up to the equal, action is after. We can
         * parse the action now. */
        Parser result;
        /* This is kind of hacky but we put a dummy trigger
         * for chained inputs. The `next` will never yield this
         * because we have chain set. When we find a nicer way to
         * do this we can remove it, the e2e is tested. */
        result.trigger_it.input = chain ? std::string("a") : input.substr(0, eql_idx);
        err = Action::parse(input.substr(eql_idx + 1), &result.action);
        if (err != Error::none) return err;
        result.flags = flags;
        result.chain = chain;
        *out = result;
        return Error::none;
    }

    static Error parseFlags(const std::string &raw_input, Flags *flags_out, size_t *start_out) {
        Flags flags;

        size_t start_idx = 0;
        std::string input = raw_input;
        while (true) {
            /* Find the next prefix */
            const size_t idx = input.find(':');
            if (idx == std::string::npos) break;
            const std::string prefix = input.substr(0, idx);

            /* If the prefix is one of our flags then set it. */
            if (prefix == "all") {
                if (flags.all) return Error::InvalidFormat;
                flags.all = true;
            } else if (prefix == "global") {
                if (flags.global) return Error::InvalidFormat;
                flags.global = true;
            } else if (prefix == "unconsumed") {
                if (!flags.consumed) return Error::InvalidFormat;
                flags.consumed = false;
            } else if (prefix == "performable") {
                if (flags.performable) return Error::InvalidFormat;
                flags.performable = true;
            } else {
                /* If we don't recognize the prefix then we're done. We
                 * let any unknown prefix fallthrough to trigger-specific
                 * parsing in case there are trigger-specific prefixes
                 * (none currently but historically there was `physical:`
                 * at one point). Breaking here lets us always implement new
                 * prefixes. */
                break;
            }

            /* Move past the prefix */
            start_idx += idx + 1;
            input = input.substr(idx + 1);
        }

        *flags_out = flags;
        *start_out = start_idx;
        return Error::none;
    }

    /* Wisp: `?Elem` is `*has` plus `*out`. */
    Error next(Elem *out, bool *has) {
        *has = false;
        /* Get our trigger. If we're out of triggers then we're done. */
        Trigger trigger;
        bool has_trigger = false;
        const Error err = trigger_it.next(&trigger, &has_trigger);
        if (err != Error::none) return err;
        if (!has_trigger) return Error::none;

        Elem elem;
        if (!trigger_it.done()) {
            /* If this is our last trigger then it is our final binding.
             * Global/all bindings can't be sequences */
            if (flags.global || flags.all) return Error::InvalidFormat;
            elem.tag = Elem::Tag::leader;
            elem.leader = trigger;
        } else if (chain) {
            /* If we're a chain then return it as-is. */
            elem.tag = Elem::Tag::chain;
            elem.chain = action;
        } else {
            /* Out of triggers, yield the final action. */
            elem.tag = Elem::Tag::binding;
            elem.binding.trigger = trigger;
            elem.binding.action = action;
            elem.binding.flags = flags;
        }
        *out = elem;
        *has = true;
        return Error::none;
    }

    void reset() { trigger_it.i = 0; }
};

/* Parse a single, non-sequenced binding. To support sequences you must
 * use parse. This is a convenience function for single bindings aimed
 * primarily at tests.
 *
 * This doesn't support `chain` either, since chaining requires some
 * stateful concept of a prior binding. */
inline Error parseSingle(const std::string &raw_input, Binding *out) {
    Parser p;
    Error err = Parser::init(raw_input, &p);
    if (err != Error::none) return err;
    Parser::Elem elem;
    bool has = false;
    err = p.next(&elem, &has);
    if (err != Error::none) return err;
    if (!has) return Error::InvalidFormat;
    switch (elem.tag) {
        case Parser::Elem::Tag::leader: return Error::UnexpectedSequence;
        case Parser::Elem::Tag::binding: *out = elem.binding; return Error::none;
        case Parser::Elem::Tag::chain: return Error::UnexpectedChain;
    }
    return Error::InvalidFormat;
}

/* Returns true if lhs should be sorted before rhs */
inline bool lessThan(const Binding &lhs, const Binding &rhs) {
    size_t lhs_count = 0;
    if (lhs.trigger.mods.super) lhs_count += 1;
    if (lhs.trigger.mods.ctrl) lhs_count += 1;
    if (lhs.trigger.mods.shift) lhs_count += 1;
    if (lhs.trigger.mods.alt) lhs_count += 1;
    size_t rhs_count = 0;
    if (rhs.trigger.mods.super) rhs_count += 1;
    if (rhs.trigger.mods.ctrl) rhs_count += 1;
    if (rhs.trigger.mods.shift) rhs_count += 1;
    if (rhs.trigger.mods.alt) rhs_count += 1;

    if (lhs_count != rhs_count) return lhs_count > rhs_count;

    if (lhs.trigger.mods.int_() != rhs.trigger.mods.int_()) return lhs.trigger.mods.int_() > rhs.trigger.mods.int_();

    /* Wisp: upstream switches on physical/unicode only. */
    const int lhs_key = lhs.trigger.key.tag == Trigger::C::Tag::physical ? (int)lhs.trigger.key.physical
                                                                          : (int)lhs.trigger.key.unicode;
    const int rhs_key = rhs.trigger.key.tag == Trigger::C::Tag::physical ? (int)rhs.trigger.key.physical
                                                                          : (int)rhs.trigger.key.unicode;

    return lhs_key < rhs_key;
}

/* A structure that contains a set of bindings and focuses on fast lookup.
 * The use case is that this will be called on EVERY key input to look
 * for an associated action so it must be fast.
 *
 * Wisp: `std.ArrayHashMapUnmanaged(K, V, Context, true)` is an
 * insertion-ordered vector of entries with upstream's swap-remove; lookups
 * compare the Context hash then `bindingSetEqual` (Trigger.foldedEqual,
 * Action.equal). `ChainParent` holds the entry index and owning set where
 * upstream holds entry pointers; both are invalidated at the same points
 * (every put and remove clears it). Leader sets are heap-allocated with
 * new/delete and released by `deinit`, as upstream's alloc.create/destroy. */
struct Set {
    struct Value;

    /* Leaf node of a set is an action to trigger. This is a "leaf" compared
     * to the inner nodes which are "leaders" for sequences. */
    struct Leaf {
        Action action;
        Flags flags;

        Leaf clone() const { return {action.clone(), flags}; }

        uint64_t hash() const {
            zigstd::Wyhash hasher = zigstd::Wyhash::init(0);
            action.hashIncremental(&hasher);
            detail::hashBytes(&hasher, flags.cval(), 1);
            return hasher.final_();
        }
    };

    /* Leaf node of a set that triggers multiple actions in sequence. */
    struct LeafChained {
        std::vector<Action> actions;
        Flags flags;

        LeafChained clone() const {
            LeafChained result;
            for (const Action &a : actions) result.actions.push_back(a.clone());
            result.flags = flags;
            return result;
        }

        void deinit() { actions.clear(); }
    };

    /* A generic leaf node that can be used to unify the handling of
     * leaf and leaf_chained. */
    struct GenericLeaf {
        Flags flags;
        std::vector<Action> actions;

        const std::vector<Action> &actionsSlice() const { return actions; }
    };

    /* The entry type for the forward mapping of trigger to action. */
    struct Value {
        enum class Tag {
            /* This key is a leader key in a sequence. You must follow the given
             * set to find the next key in the sequence. */
            leader,

            /* This trigger completes a sequence and the value is the action
             * to take along with the flags that may define binding behavior. */
            leaf,

            /* A set of actions to take in response to a trigger. */
            leaf_chained,
        };

        Tag tag = Tag::leaf;
        Set *leader = nullptr;
        Leaf leaf;
        LeafChained leaf_chained;

        /* Writes the configuration entries for the binding
         * that this value is part of.
         *
         * The value may be part of multiple configuration entries
         * if they're all part of the same prefix sequence (e.g. 'a>b', 'a>c').
         * These will result in multiple separate entries in the configuration.
         *
         * Wisp: upstream's fixed buffer writer is `storage` plus its `end`,
         * shared between nested levels; rewinding moves `end` and leaves the
         * bytes in place, exactly as `buffer.end = pos` does. */
        template <typename Formatter>
        void formatEntries(std::string *storage, size_t *end, Formatter formatter) const;

        /* Wisp: buffer.print at `end`. */
        static void bufPrint(std::string *storage, size_t *end, const std::string &text) {
            if (storage->size() < *end + text.size()) storage->resize(*end + text.size());
            storage->replace(*end, text.size(), text);
            *end += text.size();
        }
    };

    /* A full key-value entry for the set. */
    struct Entry {
        Trigger key;
        Value value;
    };

    struct ReverseEntry {
        Action key;
        Trigger value;
    };

    /* Information about a chain parent entry, including which set it
     * belongs to. This is needed because reverse mappings are only
     * maintained in the root set, but the chain parent entry may be
     * in a nested set (for leader key sequences). */
    struct ChainParent {
        size_t index;
        Set *set;

        Trigger *key_ptr() const { return &set->bindings[index].key; }
        Value *value_ptr() const { return &set->bindings[index].value; }
    };

    /* The set of bindings. */
    std::vector<Entry> bindings;

    /* The reverse mapping of action to binding. Note that multiple
     * bindings can map to the same action and this map will only have
     * the most recently added binding for an action.
     *
     * Sequenced triggers are never present in the reverse map at this time.
     * This is a conscious decision since the primary use case of the reverse
     * map is to support GUI toolkit keyboard accelerators and no mainstream
     * GUI toolkit supports sequences.
     *
     * Performable triggers are also not present in the reverse map. This
     * is so that GUI toolkits don't register performable triggers as
     * menu shortcuts (the primary use case of the reverse map). GUI toolkits
     * such as GTK handle menu shortcuts too early in the event lifecycle
     * for performable to work so this is a conscious decision to ease the
     * integration with GUI toolkits. */
    std::vector<ReverseEntry> reverse;

    /* The chain parent is the information necessary to attach a chained
     * action to the proper location in our mapping. It tracks both the
     * entry in the hashmap and the set it belongs to, which is needed
     * to properly update reverse mappings when converting a leaf to
     * a chained action. */
    bool has_chain_parent = false;
    ChainParent chain_parent = {0, nullptr};

    /* Wisp: hash-map primitives (Context: truncated hash + bindingSetEqual). */
    size_t findBinding(const Trigger &t) const {
        const uint32_t h = (uint32_t)t.hash();
        for (size_t i = 0; i < bindings.size(); i++) {
            if ((uint32_t)bindings[i].key.hash() == h && t.foldedEqual(bindings[i].key)) return i;
        }
        return SIZE_MAX;
    }
    size_t findReverse(const Action &a) const {
        const uint32_t h = (uint32_t)a.hash();
        for (size_t i = 0; i < reverse.size(); i++) {
            if ((uint32_t)reverse[i].key.hash() == h && a.equal(reverse[i].key)) return i;
        }
        return SIZE_MAX;
    }
    void bindingsSwapRemoveAt(size_t i) {
        bindings[i] = bindings.back();
        bindings.pop_back();
    }
    void reverseSwapRemoveAt(size_t i) {
        reverse[i] = reverse.back();
        reverse.pop_back();
    }
    void reversePut(const Action &a, const Trigger &t) {
        const size_t i = findReverse(a);
        if (i != SIZE_MAX) {
            reverse[i].value = t;
            return;
        }
        reverse.push_back({a, t});
    }

    void deinit() {
        /* Clear any leaders if we have them */
        for (Entry &entry : bindings) {
            switch (entry.value.tag) {
                case Value::Tag::leader:
                    entry.value.leader->deinit();
                    delete entry.value.leader;
                    break;

                case Value::Tag::leaf_chained: entry.value.leaf_chained.deinit(); break;

                case Value::Tag::leaf: break;
            }
        }

        bindings.clear();
        reverse.clear();
        has_chain_parent = false;
    }

    enum class RecurseError {
        none,
        OutOfMemory,
        SequenceUnbind,
        NoChainParent,
        UnexpectedEndOfInput,
        InvalidChainAction,
    };

    /* Parse a user input binding and add it to the set. This will handle
     * the "unbind" case, ensure consumed/unconsumed fields are set correctly,
     * handle sequences, etc.
     *
     * If this returns an OutOfMemory error then the set is in a broken
     * state and should not be used again. Any Error returned is validated
     * before any set modifications are made. */
    Error parseAndPut(const std::string &input) {
        /* To make cleanup easier, we ensure that the full sequence is
         * valid before making any set modifications. This is more expensive
         * computationally but it makes cleanup way, way easier. */
        Parser it;
        Error err = Parser::init(input, &it);
        if (err != Error::none) return err;
        while (true) {
            Parser::Elem e;
            bool has = false;
            err = it.next(&e, &has);
            if (err != Error::none) return err;
            if (!has) break;
        }
        it.reset();

        /* We use recursion so that we can utilize the stack as our state
         * for cleanup. */
        Set *updated_set = nullptr;
        const RecurseError rerr = parseAndPutRecurse(this, &it, &updated_set);
        switch (rerr) {
            case RecurseError::none: break;

            /* If this gets sent up to the root then we've unbound
             * all the way up and this put was a success. */
            case RecurseError::SequenceUnbind: updated_set = nullptr; break;

            /* If our parser input was too short then the format
             * is invalid because we handle all valid cases. */
            case RecurseError::UnexpectedEndOfInput: return Error::InvalidFormat;

            /* If we had a chain without a parent then the format is wrong. */
            case RecurseError::NoChainParent: return Error::InvalidFormat;

            /* If we had an invalid action for a chain (e.g. unbind). */
            case RecurseError::InvalidChainAction: return Error::InvalidFormat;

            /* Unrecoverable. Wisp: std::vector reports no allocation failure. */
            case RecurseError::OutOfMemory: return Error::InvalidFormat;
        }

        /* If we have an updated set (a binding was added) then we store
         * it for our chain parent. If we didn't update a set then we clear
         * our chain parent since chaining is no longer valid until a
         * valid binding is saved. */
        if (updated_set != nullptr) {
            /* A successful addition must have recorded a chain parent. */
            assert(updated_set->has_chain_parent);
            if (updated_set != this) {
                has_chain_parent = updated_set->has_chain_parent;
                chain_parent = updated_set->chain_parent;
            }
            assert(has_chain_parent);
        } else {
            has_chain_parent = false;
        }
        return Error::none;
    }

    /* Returns the set that was ultimately updated if a binding was
     * added. Unbind does not return a set since nothing was added. */
    RecurseError parseAndPutRecurse(Set *set, Parser *it, Set **out) {
        Set *root = this;
        Parser::Elem elem;
        bool has = false;
        if (it->next(&elem, &has) != Error::none) assert(false && "unreachable");
        if (!has) return RecurseError::UnexpectedEndOfInput;
        switch (elem.tag) {
            case Parser::Elem::Tag::leader: {
                const Trigger t = elem.leader;
                /* If we have a leader, we need to upsert a set for it.
                 * Since we remove the value, we need to copy it. */
                bool has_old = false;
                Value old;
                {
                    const size_t i = set->findBinding(t);
                    if (i != SIZE_MAX) {
                        has_old = true;
                        old = set->bindings[i].value;
                    }
                }
                if (has_old) {
                    switch (old.tag) {
                        /* We have an existing leader for this key already
                         * so recurse into this set. */
                        case Value::Tag::leader: {
                            Set *s = old.leader;
                            const RecurseError err = root->parseAndPutRecurse(s, it, out);
                            switch (err) {
                                /* Our child put unbound. If our set is empty we
                                 * need to dealloc and continue up. If our set is
                                 * not empty then we're done. */
                                case RecurseError::SequenceUnbind:
                                    if (s->bindings.empty()) {
                                        set->remove(t);
                                        return RecurseError::SequenceUnbind;
                                    }
                                    *out = nullptr;
                                    return RecurseError::none;

                                default: return err;
                            }
                        }

                        case Value::Tag::leaf:
                        case Value::Tag::leaf_chained:
                            /* Remove the existing action. Fallthrough as if
                             * we don't have a leader. */
                            set->remove(t);
                            break;
                    }
                }

                /* Create our new set for this leader */
                Set *next = new Set();

                /* Insert the leader entry */
                {
                    Value v;
                    v.tag = Value::Tag::leader;
                    v.leader = next;
                    const size_t i = set->findBinding(t);
                    if (i != SIZE_MAX) {
                        set->bindings[i].value = v;
                    } else {
                        set->bindings.push_back({t, v});
                    }
                }

                /* Recurse */
                const RecurseError err = root->parseAndPutRecurse(next, it, out);
                switch (err) {
                    /* If our action was to unbind, we restore the old
                     * action if we have it. */
                    case RecurseError::SequenceUnbind: {
                        set->remove(t);
                        if (has_old) {
                            switch (old.tag) {
                                case Value::Tag::leader: assert(false && "unreachable"); break; /* Handled above */
                                case Value::Tag::leaf: set->putFlags(t, old.leaf.action, old.leaf.flags); break;

                                case Value::Tag::leaf_chained: {
                                    /* Rebuild our chain */
                                    set->putFlags(t, old.leaf_chained.actions[0], old.leaf_chained.flags);
                                    for (size_t i = 1; i < old.leaf_chained.actions.size(); i++) {
                                        if (set->appendChain(old.leaf_chained.actions[i]) != RecurseError::none) {
                                            set->remove(t);
                                            break;
                                        }
                                    }
                                    break;
                                }
                            }
                        }

                        *out = nullptr;
                        return RecurseError::none;
                    }

                    default: return err;
                }
            }

            case Parser::Elem::Tag::binding: {
                const Binding &b = elem.binding;
                if (b.action.tag == Action::Key::unbind) {
                    set->remove(b.trigger);
                    return RecurseError::SequenceUnbind;
                }

                set->putFlags(b.trigger, b.action, b.flags);
                *out = set;
                return RecurseError::none;
            }

            case Parser::Elem::Tag::chain: {
                const Action &action = elem.chain;
                /* Chains can only happen on the root. */
                assert(set == root);
                /* Unbind is not valid for chains. */
                if (action.tag == Action::Key::unbind) return RecurseError::InvalidChainAction;
                const RecurseError err = set->appendChain(action);
                if (err != RecurseError::none) return err;
                *out = set;
                return RecurseError::none;
            }
        }
        return RecurseError::UnexpectedEndOfInput;
    }

    /* Add a binding to the set. If the binding already exists then
     * this will overwrite it. */
    void put(const Trigger &t, const Action &action) { putFlags(t, action, Flags()); }

    /* Add a binding to the set with explicit flags. */
    void putFlags(const Trigger &t, const Action &action, const Flags &flags) {
        /* unbind should never go into the set, it should be handled prior */
        assert(action.tag != Action::Key::unbind);

        /* This is true if we're going to track this entry as
         * a reverse mapping. There are certain scenarios we don't.
         * See the reverse map docs for more information. */
        const bool track_reverse = !flags.performable;

        /* No matter what our chained parent becomes invalid because
         * getOrPut invalidates pointers. */
        has_chain_parent = false;

        size_t index = findBinding(t);
        const bool found_existing = index != SIZE_MAX;
        if (!found_existing) {
            bindings.push_back({t, Value()});
            index = bindings.size() - 1;
        }
        has_chain_parent = true;
        chain_parent = {index, this};

        if (found_existing) {
            Value &existing = bindings[index].value;
            switch (existing.tag) {
                /* If we have a leader we need to clean up the memory */
                case Value::Tag::leader:
                    existing.leader->deinit();
                    delete existing.leader;
                    break;

                /* If we have an existing binding for this trigger, we have to
                 * update the reverse mapping to remove the old action. */
                case Value::Tag::leaf:
                    if (track_reverse) {
                        const uint64_t t_hash = t.hash();
                        for (size_t i = 0; i < reverse.size(); i++) {
                            if (t_hash == reverse[i].value.hash()) {
                                reverseSwapRemoveAt(i);
                                break;
                            }
                        }
                    }
                    break;

                /* Chained leaves aren't in the reverse mapping so we just
                 * clear it out. */
                case Value::Tag::leaf_chained: existing.leaf_chained.deinit(); break;
            }
        }

        Value v;
        v.tag = Value::Tag::leaf;
        v.leaf = {action, flags};
        bindings[index].value = v;

        if (track_reverse) reversePut(action, t);

        /* Invariant: after successful put, chain_parent must be valid and point
         * to the entry we just added/updated. */
        assert(has_chain_parent);
        assert(chain_parent.value_ptr()->tag == Value::Tag::leaf);
    }

    /* Append a chained action to the prior set action.
     *
     * It is an error if there is no valid prior chain parent.
     *
     * Wisp: returns RecurseError::NoChainParent or none. */
    RecurseError appendChain(const Action &action) {
        /* Unbind is not a valid chain action; callers must check this. */
        assert(action.tag != Action::Key::unbind);

        if (!has_chain_parent) return RecurseError::NoChainParent;
        const ChainParent parent = chain_parent;
        Value *value = parent.value_ptr();
        switch (value->tag) {
            /* Leader can never be a chain parent. Verified through various
             * assertions and unit tests. */
            case Value::Tag::leader: assert(false && "unreachable"); break;

            /* If it is already a chained action, we just append the
             * action. Easy! */
            case Value::Tag::leaf_chained: value->leaf_chained.actions.push_back(action); break;

            /* If it is a leaf, we need to convert it to a leaf_chained.
             * We also need to be careful to remove any prior reverse
             * mappings for this action since chained actions are not
             * part of the reverse mapping. */
            case Value::Tag::leaf: {
                const Leaf leaf = value->leaf;

                /* Setup our failable actions list first. */
                std::vector<Action> actions;
                actions.push_back(leaf.action);
                actions.push_back(action);

                /* Convert to leaf_chained first, before fixing up reverse
                 * mapping. This is important because fixupReverseForAction
                 * searches for other bindings with the same action, and we
                 * don't want to find this entry (which is now chained). */
                value->tag = Value::Tag::leaf_chained;
                value->leaf_chained.actions = actions;
                value->leaf_chained.flags = leaf.flags;

                /* Clean up our reverse mapping. Chained actions are not
                 * part of the reverse mapping, so we need to fix up the
                 * reverse map (possibly restoring another trigger for the
                 * same action). */
                parent.set->fixupReverseForAction(leaf.action, *parent.key_ptr());
                break;
            }
        }
        return RecurseError::none;
    }

    /* Get a binding for a given trigger. */
    Entry *get(const Trigger &t) {
        const size_t i = findBinding(t);
        return i == SIZE_MAX ? nullptr : &bindings[i];
    }
    const Entry *get(const Trigger &t) const {
        const size_t i = findBinding(t);
        return i == SIZE_MAX ? nullptr : &bindings[i];
    }

    /* Get a trigger for the given action. An action can have multiple
     * triggers so this will return the first one found.
     *
     * Wisp: `?Trigger` is the bool return plus `*out`. */
    bool getTrigger(const Action &a, Trigger *out) const {
        const size_t i = findReverse(a);
        if (i == SIZE_MAX) return false;
        *out = reverse[i].value;
        return true;
    }

    /* Get an entry for the given key event. This will attempt to find
     * a binding using multiple parts of the event in the following order:
     *
     *   1. Physical key (event.physical_key)
     *   2. Unshifted Unicode codepoint (event.unshifted_codepoint)
     * */
    const Entry *getEvent(const KeyEvent &event) const {
        Trigger trigger;
        trigger.mods = event.mods.binding();
        trigger.key = Trigger::Key::makePhysical(event.key);
        if (const Entry *v = get(trigger)) return v;

        /* If our UTF-8 text is exactly one codepoint, we try to match that. */
        if (event.utf8_len > 0) {
            zigstd::Utf8View view;
            if (zigstd::Utf8View::init(event.utf8, event.utf8_len, &view)) {
                /* No codepoints or multiple codepoints drops to invalid format */
                uint32_t cp = 0;
                uint32_t extra = 0;
                if (view.nextCodepoint(&cp) && !view.nextCodepoint(&extra)) {
                    trigger.key = Trigger::Key::makeUnicode(cp);
                    if (const Entry *v = get(trigger)) return v;
                }
            }
        }

        /* Finally fallback to the full unshifted codepoint if we have one.
         * Question: should we be doing this if we have UTF-8 text? I
         * suspect "no" but we don't currently have any failing scenarios
         * to verify this. */
        if (event.unshifted_codepoint > 0) {
            trigger.key = Trigger::Key::makeUnicode(event.unshifted_codepoint);
            if (const Entry *v = get(trigger)) return v;
        }

        /* Fallback to catch_all with modifiers first, then without modifiers. */
        trigger.key = Trigger::Key::makeCatchAll();
        if (const Entry *v = get(trigger)) return v;
        if (!trigger.mods.empty()) {
            trigger.mods = Mods();
            if (const Entry *v = get(trigger)) return v;
        }

        return nullptr;
    }

    /* Remove a binding for a given trigger. */
    void remove(const Trigger &t) { removeExact(t); }

    void removeExact(const Trigger &t) {
        /* Removal always resets our chain parent. We could make this
         * finer grained but the way it is documented is that chaining
         * must happen directly after sets so this works. */
        has_chain_parent = false;

        const size_t i = findBinding(t);
        if (i == SIZE_MAX) return;
        Value entry = bindings[i].value;
        bindingsSwapRemoveAt(i);

        switch (entry.tag) {
            /* For a leader removal, we need to deallocate our child set.
             * Leaders are never part of reverse maps so no other accounting
             * needs to be done. */
            case Value::Tag::leader:
                entry.leader->deinit();
                delete entry.leader;
                break;

            /* For an action we need to fix up the reverse mapping. */
            case Value::Tag::leaf: fixupReverseForAction(entry.leaf.action, t); break;

            /* Chained leaves are never in our reverse mapping so no
             * cleanup is required. */
            case Value::Tag::leaf_chained: entry.leaf_chained.deinit(); break;
        }
    }

    /* Fix up the reverse mapping after removing an action.
     *
     * When an action is removed from a binding (either by removal or by
     * converting to a chained action), we need to update the reverse mapping.
     * If another binding has the same action, we update the reverse mapping
     * to point to that binding. Otherwise, we remove the action from the
     * reverse mapping entirely.
     *
     * The `old` parameter is the trigger that was previously bound to this
     * action. It is used to check if the reverse mapping still points to
     * this trigger; if not, no fixup is needed since the reverse map already
     * points to a different trigger for this action.
     *
     * Note: we'd LIKE to replace this with the most recent binding but
     * our hash map obviously has no concept of ordering so we have to
     * choose whatever. Maybe a switch to an array hash map here. */
    void fixupReverseForAction(const Action &action, const Trigger &old) {
        const size_t ri = findReverse(action);
        if (ri == SIZE_MAX) return;

        /* If our value is not the same as the old trigger, we can
         * ignore it because our reverse mapping points somewhere else. */
        if (!reverse[ri].value.equal(old)) return;

        /* It is the same trigger, so let's now go through our bindings
         * and try to find another trigger that maps to the same action. */
        const uint64_t action_hash = action.hash();
        for (const Entry &it_entry : bindings) {
            switch (it_entry.value.tag) {
                case Value::Tag::leader:
                case Value::Tag::leaf_chained: break;
                case Value::Tag::leaf:
                    if (it_entry.value.leaf.action.hash() == action_hash) {
                        reverse[ri].value = it_entry.key;
                        return;
                    }
                    break;
            }
        }

        /* No other trigger points to this action so we remove
         * the reverse mapping completely. */
        reverseSwapRemoveAt(ri);
    }

    /* Deep clone the set. */
    Set clone() const {
        Set result;
        result.bindings = bindings;
        result.reverse = reverse;

        /* If we have any leaders we need to clone them. */
        for (Entry &entry : result.bindings) {
            switch (entry.value.tag) {
                /* Leaves could have data to clone (i.e. text actions
                 * contain allocated strings). */
                case Value::Tag::leaf: entry.value.leaf = entry.value.leaf.clone(); break;

                case Value::Tag::leaf_chained: entry.value.leaf_chained = entry.value.leaf_chained.clone(); break;

                /* Must be deep cloned. */
                case Value::Tag::leader: {
                    Set *ptr = new Set();
                    *ptr = entry.value.leader->clone();
                    entry.value.leader = ptr;
                    break;
                }
            }
        }

        /* We need to clone the action keys in the reverse map since
         * they may contain allocated values. */
        for (ReverseEntry &r : result.reverse) r.key = r.key.clone();

        return result;
    }
};

template <typename Formatter>
void Set::Value::formatEntries(std::string *storage, size_t *end, Formatter formatter) const {
    switch (tag) {
        case Tag::leader: {
            /* We'll rewind to this position after each sub-entry,
             * sharing the prefix between siblings. */
            const size_t pos = *end;

            for (const Entry &binding : leader->bindings) {
                /* I'm not exactly if this is safe for any arbitrary
                 * writer since the Writer interface does not have any
                 * rewind functions, but for our use case of a
                 * fixed-size buffer writer this should work just fine. */
                *end = pos;
                bufPrint(storage, end, ">" + binding.key.format());
                binding.value.formatEntries(storage, end, formatter);
            }
            break;
        }

        case Tag::leaf: {
            /* When we get to the leaf, the buffer_stream contains
             * the full sequence of keys needed to reach this action. */
            bufPrint(storage, end, "=" + leaf.action.format());
            formatter.formatEntry(storage->substr(0, *end));
            break;
        }

        case Tag::leaf_chained: {
            const size_t pos = *end;
            for (size_t i = 0; i < leaf_chained.actions.size(); i++) {
                if (i == 0) {
                    bufPrint(storage, end, "=" + leaf_chained.actions[i].format());
                } else {
                    *end = 0;
                    bufPrint(storage, end, "chain=" + leaf_chained.actions[i].format());
                }
                formatter.formatEntry(storage->substr(0, *end));
                *end = pos;
            }
            break;
        }
    }
}

} /* namespace binding */
} /* namespace input */
} /* namespace wisp */

#endif /* WISP_INPUT_BINDING_HPP */
