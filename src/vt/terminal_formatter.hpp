/* Transliterated from Ghostty src/terminal/formatter.zig (TerminalFormatter)
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: upstream keeps TerminalFormatter in formatter.zig next to the other
 * formatters. Here it lives in its own header because it needs Terminal,
 * which includes screen.hpp, which formatter.hpp also includes; splitting
 * it keeps formatter.hpp free of a dependency on Terminal.
 *
 * Wisp: std.Io.Writer is std::string (writes can't fail), so format returns
 * void where upstream returns std.Io.Writer.Error!void, and the discarding
 * writer used to count the extra bytes for the pin map is a throwaway
 * std::string.
 */

#pragma once
#ifndef WISP_VT_TERMINAL_FORMATTER_HPP
#define WISP_VT_TERMINAL_FORMATTER_HPP

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "formatter.hpp"
#include "terminal.hpp"

namespace wisp {
namespace vt {
namespace formatter {

struct TerminalFormatter {
    /* The terminal to format. */
    const Terminal *terminal;

    /* The common options */
    Options opts;

    /* The content to include. */
    ScreenFormatter::Content content;

    /* Extra stuff to emit, such as terminal modes, palette, cursor, etc.
     * This information is ONLY emitted when the format is "vt". */
    struct Extra {
        /* Emit the palette using OSC 4 sequences. */
        bool palette;

        /* Emit terminal modes that differ from their defaults using CSI h/l
         * sequences. Defaults are according to the Ghostty defaults which
         * are generally match most terminal defaults. This will include
         * things like current screen, bracketed mode, mouse event reporting,
         * etc. */
        bool modes;

        /* Emit scrolling region state using DECSTBM and DECSLRM sequences. */
        bool scrolling_region;

        /* Emit tabstop positions by clearing all tabs (CSI 3 g) and setting
         * each configured tabstop with HTS. */
        bool tabstops;

        /* Emit the present working directory using OSC 7. */
        bool pwd;

        /* Emit keyboard modes such as ModifyOtherKeys using CSI > 4 m
         * sequences. */
        bool keyboard;

        /* The screen extras to emit. TerminalFormatter always only
         * emits data for the currently active screen. If you want to emit
         * data for all screens, you should manually construct a no-content
         * terminal formatter, followed by screen formatters. */
        ScreenFormatter::Extra screen;

        /* Emit nothing. */
        static Extra none() {
            Extra e;
            e.palette = false;
            e.modes = false;
            e.scrolling_region = false;
            e.tabstops = false;
            e.pwd = false;
            e.keyboard = false;
            e.screen = ScreenFormatter::Extra::none();
            return e;
        }

        /* Emit style-relevant information only such as palettes. */
        static Extra styles() {
            Extra e = none();
            e.palette = true;
            e.screen = ScreenFormatter::Extra::styles();
            return e;
        }

        /* Emit everything. This reconstructs the terminal state as closely
         * as possible. */
        static Extra all() {
            Extra e;
            e.palette = true;
            e.modes = true;
            e.scrolling_region = true;
            e.tabstops = true;
            e.pwd = true;
            e.keyboard = true;
            e.screen = ScreenFormatter::Extra::all();
            return e;
        }
    } extra;

    /* If non-null, then `map` will contain the Pin of every byte
     * byte written to the writer offset by the byte index. It is the
     * caller's responsibility to free the map.
     *
     * Note that some emitted bytes may not correspond to any Pin, such as
     * the extra data around terminal state (palette, modes, etc.). For these,
     * we'll map it to the most previous pin so there is some continuity but
     * its an arbitrary choice.
     *
     * Warning: there is a significant performance hit to track this */
    Maybe<PinMap> pin_map;

    static TerminalFormatter init(const Terminal *terminal, const Options &opts) {
        TerminalFormatter f;
        f.terminal = terminal;
        f.opts = opts;
        f.content.tag = ScreenFormatter::Content::Tag::selection;
        f.content.selection = Maybe<Selection>();
        f.extra = Extra::styles();
        f.pin_map = Maybe<PinMap>();
        return f;
    }

    void format(std::string *writer) const {
        /* Emit palette before screen content if using VT format. Technically
         * we could do this after but this way if replay is slow for whatever
         * reason the colors will be right right away. */
        if (extra.palette && opts.emit != Format::plain) {
            switch (opts.emit) {
            case Format::plain: break;

            case Format::vt: {
                for (size_t i = 0; i < 256; i++) {
                    const terminal::RGB rgb = terminal->colors.palette.current.colors[i];
                    char buf[64];
                    snprintf(buf, sizeof buf, "\x1b]4;%u;rgb:%02x/%02x/%02x\x1b\\", (unsigned)i,
                             (unsigned)rgb.r, (unsigned)rgb.g, (unsigned)rgb.b);
                    writer->append(buf);
                }
                break;
            }

            /* For HTML, we emit CSS to setup our palette variables. */
            case Format::html: {
                writer->append("<style>:root{");
                for (size_t i = 0; i < 256; i++) {
                    const terminal::RGB rgb = terminal->colors.palette.current.colors[i];
                    char buf[64];
                    snprintf(buf, sizeof buf, "--vt-palette-%u: #%02x%02x%02x;", (unsigned)i,
                             (unsigned)rgb.r, (unsigned)rgb.g, (unsigned)rgb.b);
                    writer->append(buf);
                }
                writer->append("}</style>");
                break;
            }
            }

            /* If we have a pin_map, add the bytes we wrote to map. */
            if (pin_map.has) appendExtraToPinMap(extraPalette(), false);
        }

        /* Emit terminal modes that differ from defaults. We probably have
         * some modes we want to emit before and some after, but for now for
         * simplicity we just emit them all before. If we make this more complex
         * later we should add test cases for it. */
        if (opts.emit == Format::vt && extra.modes) {
            const terminal::modes::ModeEntry *entries = terminal::modes::entries();
            for (size_t i = 0; i < terminal::modes::entries_len; i++) {
                const terminal::modes::Mode mode = entries[i].mode;
                const bool current = terminal->modes.get(mode);
                const bool default_val = terminal::modes::getPacked(&terminal->modes.default_, mode);
                if (current == default_val) continue;

                const terminal::modes::ModeTag tag = terminal::modes::ModeTag::fromMode(mode);
                char buf[32];
                snprintf(buf, sizeof buf, "\x1b[%s%u%s", tag.ansi ? "" : "?", (unsigned)tag.value,
                         current ? "h" : "l");
                writer->append(buf);
            }

            if (pin_map.has) appendExtraToPinMap(extraModes(), false);
        }

        /* Emit tabstop positions before the screen contents because setting
         * them moves the cursor. Screen formatting will restore the requested
         * cursor position afterwards. */
        if (opts.emit == Format::vt && extra.tabstops) {
            /* Clear all tabs (CSI 3 g) */
            writer->append("\x1b[3g");

            /* Set each configured tabstop by moving cursor and using HTS */
            for (size_t col = 0; col < (size_t)terminal->cols; col++) {
                if (terminal->tabstops.get(col)) {
                    /* Move cursor to the column (1-indexed) */
                    char buf[32];
                    snprintf(buf, sizeof buf, "\x1b[%uG", (unsigned)(col + 1));
                    writer->append(buf);
                    /* Set tab (HTS) */
                    writer->append("\x1bH");
                }
            }

            /* Screen contents are formatted relative to the top-left. */
            writer->append("\x1b[H");

            if (pin_map.has) appendExtraToPinMap(extraTabstops(), false);
        }

        ScreenFormatter screen_formatter = ScreenFormatter::init(terminal->screens.active, opts);
        screen_formatter.content = content;
        screen_formatter.pin_map = pin_map;
        screen_formatter.format(writer);

        /* Extra terminal state to emit after the screen contents so that
         * it doesn't impact the emitted contents. */
        if (opts.emit == Format::vt) {
            /* Emit scrolling region using DECSTBM and DECSLRM */
            if (extra.scrolling_region) {
                const Terminal::ScrollingRegion *region = &terminal->scrolling_region;

                /* DECSTBM: top and bottom margins (1-indexed)
                 * Only emit if not the full screen */
                if (region->top != 0 || region->bottom != terminal->rows - 1) {
                    char buf[32];
                    snprintf(buf, sizeof buf, "\x1b[%u;%ur", (unsigned)(region->top + 1),
                             (unsigned)(region->bottom + 1));
                    writer->append(buf);
                }

                /* DECSLRM: left and right margins (1-indexed)
                 * Only emit if not the full width */
                if (region->left != 0 || region->right != terminal->cols - 1) {
                    char buf[32];
                    snprintf(buf, sizeof buf, "\x1b[%u;%us", (unsigned)(region->left + 1),
                             (unsigned)(region->right + 1));
                    writer->append(buf);
                }
            }

            /* Emit keyboard modes such as ModifyOtherKeys */
            if (extra.keyboard) {
                /* Only emit if modify_other_keys_2 is true */
                if (terminal->flags.modify_other_keys_2) writer->append("\x1b[>4;2m");
            }

            /* Emit present working directory using OSC 7 */
            if (extra.pwd) {
                if (terminal->pwd.len > 0) {
                    writer->append("\x1b]7;");
                    writer->append((const char *)terminal->pwd.items, terminal->pwd.len);
                    writer->append("\x1b\\");
                }
            }

            if (pin_map.has) appendExtraToPinMap(extraAfter(), true);
        }

        /* Emit extra screen state last because terminal state such
         * as scrolling regions can move the cursor, so we have to set
         * cursor last. */
        screen_formatter.content.tag = ScreenFormatter::Content::Tag::none;
        screen_formatter.extra = extra.screen;
        screen_formatter.format(writer);
    }

private:
    /* Wisp: the `extra_formatter` clones upstream builds to count the bytes
     * one extra group writes. Each returns an Extra with only that group
     * enabled. */
    Extra extraPalette() const {
        Extra e = Extra::none();
        e.palette = true;
        return e;
    }
    Extra extraModes() const {
        Extra e = Extra::none();
        e.modes = true;
        return e;
    }
    Extra extraTabstops() const {
        Extra e = Extra::none();
        e.tabstops = true;
        return e;
    }
    Extra extraAfter() const {
        Extra e = Extra::none();
        e.scrolling_region = extra.scrolling_region;
        e.keyboard = extra.keyboard;
        e.pwd = extra.pwd;
        return e;
    }

    /* Map the bytes one extra group wrote to a single pin: the top left,
     * which ensures the node pointer is always properly initialized, or
     * for the trailing groups the last pin in the map. */
    void appendExtraToPinMap(Extra group, bool prefer_last) const {
        std::string discarding;
        TerminalFormatter extra_formatter = *this;
        extra_formatter.content.tag = ScreenFormatter::Content::Tag::none;
        extra_formatter.pin_map = Maybe<PinMap>();
        extra_formatter.extra = group;
        extra_formatter.format(&discarding);

        PinMap::Map *m = pin_map.value.map;
        Maybe<Pin> pin = Maybe<Pin>();
        if (prefer_last) pin = m->getLastOrNull();
        if (!pin.has) pin = Maybe<Pin>(terminal->screens.active->pages.getTopLeft(point::Tag::screen));
        m->append(pin.value, discarding.size());
    }
};

} /* namespace formatter */
} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_TERMINAL_FORMATTER_HPP */
