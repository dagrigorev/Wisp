/* Transliterated from Ghostty src/input/command.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp, differences in shape rather than behavior:
 *   - `[:0]const u8` fields are std::string; Command.C points into them
 *     (and into a caller-owned action string for cval).
 *   - `defaults`/`defaultsC` are comptime slices upstream; here they are
 *     built once on first use and returned by reference.
 *   - i18n: Wisp has no gettext catalog, so `i18n.N_` is the identity and
 *     `translated` returns the command unchanged.
 *   - Non-ASCII titles are written as escaped UTF-8 bytes.
 */
#pragma once

#include "binding.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <string>
#include <vector>

namespace wisp {
namespace input {
namespace command {

using binding::Action;

/* A command is a named binding action that can be executed from
 * something like a command palette.
 *
 * A command must be associated with a binding; all commands can be
 * mapped to traditional `keybind` configurations. This restriction
 * makes it so that there is nothing special about commands and likewise
 * it makes it trivial and consistent to define custom commands.
 *
 * For apprt implementers: a command palette doesn't have to make use
 * of all the fields here. We try to provide as much information as
 * possible to make it easier to implement a command palette in the way
 * that makes the most sense for the application. */
struct Command {
    Action action;
    std::string title;
    std::string description = "";

    Command() = default;
    Command(const Action &a, std::string t, std::string d = "")
        : action(a), title(std::move(t)), description(std::move(d)) {}

    /* ghostty_command_s */
    struct C {
        const char *action_key;
        const char *action;
        const char *title;
        const char *description;
    };

    Command clone() const { return Command(action.clone(), title, description); }

    bool equal(const Command &other) const {
        if (action.hash() != other.action.hash()) return false;
        if (title != other.title) return false;
        if (description != other.description) return false;
        return true;
    }

    /* Convert this command to a C struct at runtime.
     *
     * This shares memory with the original command.
     *
     * Wisp: the action string is written to `*storage`, which the caller
     * owns and must keep alive as long as the result. */
    C cval(std::string *storage) const {
        *storage = action.format();
        C c;
        c.action_key = Action::keyName(action.tag);
        c.action = storage->c_str();
        c.title = title.c_str();
        c.description = description.c_str();
        return c;
    }

    Command translated() const { return Command(action, title, description); }

    /* Implements a comparison function for std.mem.sortUnstable
     * and similar functions. The sorting is defined by Ghostty
     * to be what we prefer. If a caller wants some other sorting,
     * they should do it themselves. */
    static bool lessThan(const Command &lhs, const Command &rhs) {
        /* Wisp: std.ascii.orderIgnoreCase(...) == .lt */
        const size_t n = std::min(lhs.title.size(), rhs.title.size());
        for (size_t i = 0; i < n; i++) {
            const int a = std::tolower((unsigned char)lhs.title[i]);
            const int b = std::tolower((unsigned char)rhs.title[i]);
            if (a != b) return a < b;
        }
        return lhs.title.size() < rhs.title.size();
    }
};

/* Wisp: a `[]const Command` slice. */
struct Commands {
    const Command *ptr = nullptr;
    size_t len = 0;
};

namespace detail {

using Key = Action::Key;

template <typename F>
inline Action withPayload(Key k, F set) {
    Action a = Action::make(k);
    set(a);
    return a;
}

/* Returns the set of commands associated with this action key by
 * default. Not all actions should have commands. As a general guideline,
 * an action should have a command only if it is useful and reasonable
 * to appear in a command palette. */
inline Commands actionCommands(Key action) {
    /* This is implemented as a function and switch rather than a
     * flat comptime const because we want to ensure we get a compiler
     * error when a new binding is added so that the contributor has
     * to consider whether that new binding should have commands or not. */
    const Commands result = [&]() -> Commands {
        switch (action) {
        /* Note: the use of `comptime` prefix on the return values */
        /* ensures that the data returned is all in the binary and */
        /* and not pointing to the stack. */
        case Key::reset: {
            static const Command result[] = {
                {Action::make(Key::reset),
                 "Reset Terminal",
                 "Reset the terminal to a clean state."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::copy_to_clipboard: {
            static const Command result[] = {
                {withPayload(Key::copy_to_clipboard, [](Action &a) { a.copy_to_clipboard = Action::CopyToClipboard::mixed; }),
                 "Copy to Clipboard",
                 "Copy the selected text to the clipboard in both plain and styled formats."},
                {withPayload(Key::copy_to_clipboard, [](Action &a) { a.copy_to_clipboard = Action::CopyToClipboard::plain; }),
                 "Copy Selection as Plain Text to Clipboard",
                 "Copy the selected text as plain text to the clipboard."},
                {withPayload(Key::copy_to_clipboard, [](Action &a) { a.copy_to_clipboard = Action::CopyToClipboard::vt; }),
                 "Copy Selection as ANSI Sequences to Clipboard",
                 "Copy the selected text as ANSI escape sequences to the clipboard."},
                {withPayload(Key::copy_to_clipboard, [](Action &a) { a.copy_to_clipboard = Action::CopyToClipboard::html; }),
                 "Copy Selection as HTML to Clipboard",
                 "Copy the selected text as HTML to the clipboard."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::copy_url_to_clipboard: {
            static const Command result[] = {
                {Action::make(Key::copy_url_to_clipboard),
                 "Copy URL to Clipboard",
                 "Copy the URL under the cursor to the clipboard."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::copy_title_to_clipboard: {
            static const Command result[] = {
                {Action::make(Key::copy_title_to_clipboard),
                 "Copy Terminal Title to Clipboard",
                 "Copy the terminal title to the clipboard. If the terminal title is not set this has no effect."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::paste_from_clipboard: {
            static const Command result[] = {
                {Action::make(Key::paste_from_clipboard),
                 "Paste from Clipboard",
                 "Paste the contents of the main clipboard."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::paste_from_selection: {
            static const Command result[] = {
                {Action::make(Key::paste_from_selection),
                 "Paste from Selection",
                 "Paste the contents of the selection clipboard."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::start_search: {
            static const Command result[] = {
                {Action::make(Key::start_search),
                 "Start Search",
                 "Start a search if one isn't already active."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::search_selection: {
            static const Command result[] = {
                {Action::make(Key::search_selection),
                 "Search Selection",
                 "Start a search for the current text selection."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::end_search: {
            static const Command result[] = {
                {Action::make(Key::end_search),
                 "End Search",
                 "End the current search if any and hide any GUI elements."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::navigate_search: {
            static const Command result[] = {
                {withPayload(Key::navigate_search, [](Action &a) { a.navigate_search = Action::NavigateSearch::next; }),
                 "Next Search Result",
                 "Navigate to the next search result, if any."},
                {withPayload(Key::navigate_search, [](Action &a) { a.navigate_search = Action::NavigateSearch::previous; }),
                 "Previous Search Result",
                 "Navigate to the previous search result, if any."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::increase_font_size: {
            static const Command result[] = {
                {withPayload(Key::increase_font_size, [](Action &a) { a.f32 = 1; }),
                 "Increase Font Size",
                 "Increase the font size by 1 point."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::decrease_font_size: {
            static const Command result[] = {
                {withPayload(Key::decrease_font_size, [](Action &a) { a.f32 = 1; }),
                 "Decrease Font Size",
                 "Decrease the font size by 1 point."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::reset_font_size: {
            static const Command result[] = {
                {Action::make(Key::reset_font_size),
                 "Reset Font Size",
                 "Reset the font size to the default."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::clear_screen: {
            static const Command result[] = {
                {Action::make(Key::clear_screen),
                 "Clear Screen",
                 "Clear the screen and scrollback."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::select_all: {
            static const Command result[] = {
                {Action::make(Key::select_all),
                 "Select All",
                 "Select all text on the screen."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::scroll_to_top: {
            static const Command result[] = {
                {Action::make(Key::scroll_to_top),
                 "Scroll to Top",
                 "Scroll to the top of the screen."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::scroll_to_bottom: {
            static const Command result[] = {
                {Action::make(Key::scroll_to_bottom),
                 "Scroll to Bottom",
                 "Scroll to the bottom of the screen."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::scroll_to_selection: {
            static const Command result[] = {
                {Action::make(Key::scroll_to_selection),
                 "Scroll to Selection",
                 "Scroll to the selected text."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::scroll_page_up: {
            static const Command result[] = {
                {Action::make(Key::scroll_page_up),
                 "Scroll Page Up",
                 "Scroll the screen up by a page."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::scroll_page_down: {
            static const Command result[] = {
                {Action::make(Key::scroll_page_down),
                 "Scroll Page Down",
                 "Scroll the screen down by a page."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::write_screen_file: {
            static const Command result[] = {
                {withPayload(Key::write_screen_file, [](Action &a) { a.write_screen = Action::WriteScreen::copy(); }),
                 "Copy Screen to Temporary File and Copy Path",
                 "Copy the screen contents to a temporary file and copy the path to the clipboard."},
                {withPayload(Key::write_screen_file, [](Action &a) { a.write_screen = Action::WriteScreen::paste(); }),
                 "Copy Screen to Temporary File and Paste Path",
                 "Copy the screen contents to a temporary file and paste the path to the file."},
                {withPayload(Key::write_screen_file, [](Action &a) { a.write_screen = Action::WriteScreen::open(); }),
                 "Copy Screen to Temporary File and Open",
                 "Copy the screen contents to a temporary file and open it."},
                {withPayload(Key::write_screen_file, [](Action &a) { a.write_screen = Action::WriteScreen{Action::WriteScreen::Action::copy, Action::WriteScreen::Format::html}; }),
                 "Copy Screen as HTML to Temporary File and Copy Path",
                 "Copy the screen contents as HTML to a temporary file and copy the path to the clipboard."},
                {withPayload(Key::write_screen_file, [](Action &a) { a.write_screen = Action::WriteScreen{Action::WriteScreen::Action::paste, Action::WriteScreen::Format::html}; }),
                 "Copy Screen as HTML to Temporary File and Paste Path",
                 "Copy the screen contents as HTML to a temporary file and paste the path to the file."},
                {withPayload(Key::write_screen_file, [](Action &a) { a.write_screen = Action::WriteScreen{Action::WriteScreen::Action::open, Action::WriteScreen::Format::html}; }),
                 "Copy Screen as HTML to Temporary File and Open",
                 "Copy the screen contents as HTML to a temporary file and open it."},
                {withPayload(Key::write_screen_file, [](Action &a) { a.write_screen = Action::WriteScreen{Action::WriteScreen::Action::copy, Action::WriteScreen::Format::vt}; }),
                 "Copy Screen as ANSI Sequences to Temporary File and Copy Path",
                 "Copy the screen contents as ANSI escape sequences to a temporary file and copy the path to the clipboard."},
                {withPayload(Key::write_screen_file, [](Action &a) { a.write_screen = Action::WriteScreen{Action::WriteScreen::Action::paste, Action::WriteScreen::Format::vt}; }),
                 "Copy Screen as ANSI Sequences to Temporary File and Paste Path",
                 "Copy the screen contents as ANSI escape sequences to a temporary file and paste the path to the file."},
                {withPayload(Key::write_screen_file, [](Action &a) { a.write_screen = Action::WriteScreen{Action::WriteScreen::Action::open, Action::WriteScreen::Format::vt}; }),
                 "Copy Screen as ANSI Sequences to Temporary File and Open",
                 "Copy the screen contents as ANSI escape sequences to a temporary file and open it."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::write_selection_file: {
            static const Command result[] = {
                {withPayload(Key::write_selection_file, [](Action &a) { a.write_screen = Action::WriteScreen::copy(); }),
                 "Copy Selection to Temporary File and Copy Path",
                 "Copy the selection contents to a temporary file and copy the path to the clipboard."},
                {withPayload(Key::write_selection_file, [](Action &a) { a.write_screen = Action::WriteScreen::paste(); }),
                 "Copy Selection to Temporary File and Paste Path",
                 "Copy the selection contents to a temporary file and paste the path to the file."},
                {withPayload(Key::write_selection_file, [](Action &a) { a.write_screen = Action::WriteScreen::open(); }),
                 "Copy Selection to Temporary File and Open",
                 "Copy the selection contents to a temporary file and open it."},
                {withPayload(Key::write_selection_file, [](Action &a) { a.write_screen = Action::WriteScreen{Action::WriteScreen::Action::copy, Action::WriteScreen::Format::html}; }),
                 "Copy Selection as HTML to Temporary File and Copy Path",
                 "Copy the selection contents as HTML to a temporary file and copy the path to the clipboard."},
                {withPayload(Key::write_selection_file, [](Action &a) { a.write_screen = Action::WriteScreen{Action::WriteScreen::Action::paste, Action::WriteScreen::Format::html}; }),
                 "Copy Selection as HTML to Temporary File and Paste Path",
                 "Copy the selection contents as HTML to a temporary file and paste the path to the file."},
                {withPayload(Key::write_selection_file, [](Action &a) { a.write_screen = Action::WriteScreen{Action::WriteScreen::Action::open, Action::WriteScreen::Format::html}; }),
                 "Copy Selection as HTML to Temporary File and Open",
                 "Copy the selection contents as HTML to a temporary file and open it."},
                {withPayload(Key::write_selection_file, [](Action &a) { a.write_screen = Action::WriteScreen{Action::WriteScreen::Action::copy, Action::WriteScreen::Format::vt}; }),
                 "Copy Selection as ANSI Sequences to Temporary File and Copy Path",
                 "Copy the selection contents as ANSI escape sequences to a temporary file and copy the path to the clipboard."},
                {withPayload(Key::write_selection_file, [](Action &a) { a.write_screen = Action::WriteScreen{Action::WriteScreen::Action::paste, Action::WriteScreen::Format::vt}; }),
                 "Copy Selection as ANSI Sequences to Temporary File and Paste Path",
                 "Copy the selection contents as ANSI escape sequences to a temporary file and paste the path to the file."},
                {withPayload(Key::write_selection_file, [](Action &a) { a.write_screen = Action::WriteScreen{Action::WriteScreen::Action::open, Action::WriteScreen::Format::vt}; }),
                 "Copy Selection as ANSI Sequences to Temporary File and Open",
                 "Copy the selection contents as ANSI escape sequences to a temporary file and open it."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::new_window: {
            static const Command result[] = {
                {Action::make(Key::new_window),
                 "New Window",
                 "Open a new window."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::new_tab: {
            static const Command result[] = {
                {Action::make(Key::new_tab),
                 "New Tab",
                 "Open a new tab."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::move_tab: {
            static const Command result[] = {
                {withPayload(Key::move_tab, [](Action &a) { a.isize = -1; }),
                 "Move Tab Left",
                 "Move the current tab to the left."},
                {withPayload(Key::move_tab, [](Action &a) { a.isize = 1; }),
                 "Move Tab Right",
                 "Move the current tab to the right."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::move_tab_to_new_window: {
            static const Command result[] = {
                {Action::make(Key::move_tab_to_new_window),
                 "Move Tab to New Window",
                 "Move the current tab to a new window."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::toggle_tab_overview: {
            static const Command result[] = {
                {Action::make(Key::toggle_tab_overview),
                 "Toggle Tab Overview",
                 "Toggle the tab overview."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::prompt_surface_title: {
            static const Command result[] = {
                {Action::make(Key::prompt_surface_title),
                 "Change Terminal Title\xE2\x80\xA6" /* U+2026 */,
                 "Prompt for a new title for the current terminal."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::prompt_tab_title: {
            static const Command result[] = {
                {Action::make(Key::prompt_tab_title),
                 "Change Tab Title\xE2\x80\xA6" /* U+2026 */,
                 "Prompt for a new title for the current tab."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::prompt_window_title: return {};
        case Key::new_split: {
            static const Command result[] = {
                {withPayload(Key::new_split, [](Action &a) { a.split_direction = Action::SplitDirection::left; }),
                 "Split Left",
                 "Split the terminal to the left."},
                {withPayload(Key::new_split, [](Action &a) { a.split_direction = Action::SplitDirection::right; }),
                 "Split Right",
                 "Split the terminal to the right."},
                {withPayload(Key::new_split, [](Action &a) { a.split_direction = Action::SplitDirection::up; }),
                 "Split Up",
                 "Split the terminal up."},
                {withPayload(Key::new_split, [](Action &a) { a.split_direction = Action::SplitDirection::down; }),
                 "Split Down",
                 "Split the terminal down."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::goto_split: {
            static const Command result[] = {
                {withPayload(Key::goto_split, [](Action &a) { a.split_focus_direction = Action::SplitFocusDirection::previous; }),
                 "Focus Split: Previous",
                 "Focus the previous split, if any."},
                {withPayload(Key::goto_split, [](Action &a) { a.split_focus_direction = Action::SplitFocusDirection::next; }),
                 "Focus Split: Next",
                 "Focus the next split, if any."},
                {withPayload(Key::goto_split, [](Action &a) { a.split_focus_direction = Action::SplitFocusDirection::left; }),
                 "Focus Split: Left",
                 "Focus the split to the left, if it exists."},
                {withPayload(Key::goto_split, [](Action &a) { a.split_focus_direction = Action::SplitFocusDirection::right; }),
                 "Focus Split: Right",
                 "Focus the split to the right, if it exists."},
                {withPayload(Key::goto_split, [](Action &a) { a.split_focus_direction = Action::SplitFocusDirection::up; }),
                 "Focus Split: Up",
                 "Focus the split above, if it exists."},
                {withPayload(Key::goto_split, [](Action &a) { a.split_focus_direction = Action::SplitFocusDirection::down; }),
                 "Focus Split: Down",
                 "Focus the split below, if it exists."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::goto_window: {
            static const Command result[] = {
                {withPayload(Key::goto_window, [](Action &a) { a.goto_window = Action::GotoWindow::previous; }),
                 "Focus Window: Previous",
                 "Focus the previous window, if any."},
                {withPayload(Key::goto_window, [](Action &a) { a.goto_window = Action::GotoWindow::next; }),
                 "Focus Window: Next",
                 "Focus the next window, if any."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::toggle_split_zoom: {
            static const Command result[] = {
                {Action::make(Key::toggle_split_zoom),
                 "Toggle Split Zoom",
                 "Toggle the zoom state of the current split."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::toggle_readonly: {
            static const Command result[] = {
                {Action::make(Key::toggle_readonly),
                 "Toggle Read-Only Mode",
                 "Toggle read-only mode for the current surface."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::equalize_splits: {
            static const Command result[] = {
                {Action::make(Key::equalize_splits),
                 "Equalize Splits",
                 "Equalize the size of all splits."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::reset_window_size: {
            static const Command result[] = {
                {Action::make(Key::reset_window_size),
                 "Reset Window Size",
                 "Reset the window size to the default."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::inspector: {
            static const Command result[] = {
                {withPayload(Key::inspector, [](Action &a) { a.inspector_mode = Action::InspectorMode::toggle; }),
                 "Toggle Inspector",
                 "Toggle the inspector."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::show_gtk_inspector: {
            static const Command result[] = {
                {Action::make(Key::show_gtk_inspector),
                 "Show the GTK Inspector",
                 "Show the GTK inspector."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::show_on_screen_keyboard: {
            static const Command result[] = {
                {Action::make(Key::show_on_screen_keyboard),
                 "Show On-Screen Keyboard",
                 "Show the on-screen keyboard if present."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::open_config: {
            static const Command result[] = {
                {withPayload(Key::open_config, [](Action &a) { a.open_config = Action::OpenConfig::os_open; }),
                 "Open Config Using OS editor",
                 "Open the config file with the OS's default editor."},
                {withPayload(Key::open_config, [](Action &a) { a.open_config = Action::OpenConfig::new_window; }),
                 "Open Config in New Terminal Window",
                 "Open the config file in a new window using $EDITOR or $VISUAL."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::reload_config: {
            static const Command result[] = {
                {Action::make(Key::reload_config),
                 "Reload Config",
                 "Reload the config file."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::close_surface: {
            static const Command result[] = {
                {Action::make(Key::close_surface),
                 "Close Terminal",
                 "Close the current terminal."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::close_tab: {
            static const Command result[] = {
                {withPayload(Key::close_tab, [](Action &a) { a.close_tab_mode = Action::CloseTabMode::this_; }),
                 "Close Tab",
                 "Close the current tab."},
                {withPayload(Key::close_tab, [](Action &a) { a.close_tab_mode = Action::CloseTabMode::other; }),
                 "Close Other Tabs",
                 "Close all tabs in this window except the current one."},
                {withPayload(Key::close_tab, [](Action &a) { a.close_tab_mode = Action::CloseTabMode::right; }),
                 "Close Tabs to the Right",
                 "Close all tabs to the right of the current one."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::close_window: {
            static const Command result[] = {
                {Action::make(Key::close_window),
                 "Close Window",
                 "Close the current window."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::close_all_windows: {
            static const Command result[] = {
                {Action::make(Key::close_all_windows),
                 "Close All Windows",
                 "Close all windows."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::toggle_maximize: {
            static const Command result[] = {
                {Action::make(Key::toggle_maximize),
                 "Toggle Maximize",
                 "Toggle the maximized state of the current window."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::toggle_fullscreen: {
            static const Command result[] = {
                {Action::make(Key::toggle_fullscreen),
                 "Toggle Fullscreen",
                 "Toggle the fullscreen state of the current window."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::toggle_window_decorations: {
            static const Command result[] = {
                {Action::make(Key::toggle_window_decorations),
                 "Toggle Window Decorations",
                 "Toggle the window decorations."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::toggle_window_float_on_top: {
            static const Command result[] = {
                {Action::make(Key::toggle_window_float_on_top),
                 "Toggle Float on Top",
                 "Toggle the float on top state of the current window."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::toggle_secure_input: {
            static const Command result[] = {
                {Action::make(Key::toggle_secure_input),
                 "Toggle Secure Input",
                 "Toggle secure input mode."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::toggle_mouse_reporting: {
            static const Command result[] = {
                {Action::make(Key::toggle_mouse_reporting),
                 "Toggle Mouse Reporting",
                 "Toggle whether mouse events are reported to terminal applications."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::toggle_background_opacity: {
            static const Command result[] = {
                {Action::make(Key::toggle_background_opacity),
                 "Toggle Background Opacity",
                 "Toggle the background opacity of a window that started transparent."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::check_for_updates: {
            static const Command result[] = {
                {Action::make(Key::check_for_updates),
                 "Check for Updates",
                 "Check for updates to the application."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::undo: {
            static const Command result[] = {
                {Action::make(Key::undo),
                 "Undo",
                 "Undo the last action."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::redo: {
            static const Command result[] = {
                {Action::make(Key::redo),
                 "Redo",
                 "Redo the last undone action."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::quit: {
            static const Command result[] = {
                {Action::make(Key::quit),
                 "Quit",
                 "Quit the application."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        case Key::text: {
            static const Command result[] = {
                {Action::makeStr(Key::text, "\xF0\x9F\x91\xBB" /* U+1F47B */),
                 "Ghostty",
                 "Put a little Ghostty in your terminal."},
            };
            return {result, sizeof(result) / sizeof(result[0])};
        }

        /* No commands because they're parameterized and there */
        /* aren't obvious values users would use. It is possible that */
        /* these may have commands in the future if there are very */
        /* common values that users tend to use. */
        case Key::csi:
        case Key::esc:
        case Key::cursor_key:
        case Key::set_font_size:
        case Key::set_surface_title:
        case Key::set_tab_title:
        case Key::set_window_title:
        case Key::search:
        case Key::scroll_to_row:
        case Key::scroll_page_fractional:
        case Key::scroll_page_lines:
        case Key::adjust_selection:
        case Key::jump_to_prompt:
        case Key::write_scrollback_file:
        case Key::goto_tab:
        case Key::resize_split:
        case Key::activate_key_table:
        case Key::activate_key_table_once:
        case Key::deactivate_key_table:
        case Key::deactivate_all_key_tables:
        case Key::end_key_sequence:
        case Key::crash: return {};
        /* No commands because I'm not sure they make sense in a command */
        /* palette context. */
        case Key::toggle_command_palette:
        case Key::toggle_quick_terminal:
        case Key::toggle_visibility:
        case Key::previous_tab:
        case Key::next_tab:
        case Key::last_tab: return {};
        /* No commands for obvious reasons */
        case Key::ignore:
        case Key::unbind: return {};
        }
        return {};
    }();

    /* All generated commands should have the same action as the
     * action passed in. */
    for (size_t i = 0; i < result.len; i++) assert(result.ptr[i].action.tag == action);

    return result;
}

}  // namespace detail

inline const std::vector<Command> &defaults() {
    static const std::vector<Command> final_ = [] {
        size_t count = 0;
        for (size_t k = 0; k < Action::key_count; k++) {
            count += detail::actionCommands((Action::Key)k).len;
        }

        std::vector<Command> result;
        result.reserve(count);
        for (size_t k = 0; k < Action::key_count; k++) {
            const Commands commands = detail::actionCommands((Action::Key)k);
            for (size_t i = 0; i < commands.len; i++) result.push_back(commands.ptr[i]);
        }

        std::sort(result.begin(), result.end(), Command::lessThan);

        assert(result.size() == count);
        return result;
    }();
    return final_;
}

/* Defaults in C-compatible form. */
inline const std::vector<Command::C> &defaultsC() {
    struct Holder {
        std::vector<std::string> actions;
        std::vector<Command::C> result;
    };
    /* Wisp: filled in place; moving the strings would invalidate the
     * pointers cval hands out. */
    static Holder holder;
    static const bool filled = [] {
        const std::vector<Command> &d = defaults();
        holder.actions.resize(d.size());
        for (size_t i = 0; i < d.size(); i++) holder.result.push_back(d[i].cval(&holder.actions[i]));
        return true;
    }();
    (void)filled;
    return holder.result;
}

}  // namespace command
}  // namespace input
}  // namespace wisp
