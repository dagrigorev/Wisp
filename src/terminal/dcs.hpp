/* Transliterated from Ghostty src/terminal/dcs.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Wisp, differences in shape rather than behavior:
 *   - Ported with tmux_control_mode = false, matching kitty_graphics and
 *     glyph_protocol; the tmux prongs and their gated test are omitted.
 *   - `?Command` is a bool return plus the Command through an out
 *     parameter; `union(enum)` is a tag enum plus one field per payload.
 *   - std.Io.Writer.Allocating is zigstd::ArrayListUnmanaged(u8), so
 *     XTGETTCAP carries the allocator it frees with.
 *   - DECRQSS.encode writes into a std::string rather than a caller
 *     buffer, since Terminal.printAttributes returns a std::string here;
 *     max_response_bytes stays as upstream's documented bound and the
 *     test still asserts the response fits it.
 *   - log.* has no sink yet and is a comment.
 */

#pragma once
#ifndef WISP_TERMINAL_DCS_HPP
#define WISP_TERMINAL_DCS_HPP

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "../vt/terminal.hpp"
#include "../zigstd/allocator.hpp"
#include "../zigstd/array_list.hpp"
#include "parser.hpp"

namespace wisp {
namespace terminal {
namespace dcs {

typedef ::wisp::terminal::parser::Action::DCS DCS;

struct Command {
    enum class Key : uint8_t {
        /* XTGETTCAP */
        xtgettcap,

        /* DECRQSS */
        decrqss,
    };

    struct XTGETTCAP {
        zigstd::ArrayListUnmanaged<uint8_t> data;
        zigstd::Allocator alloc;
        size_t i; /* = 0 */

        XTGETTCAP() : data(), alloc(zigstd::c_allocator()), i(0) {}

        void deinit() { data.deinit(alloc); }

        /* Returns the next terminfo key being requested and null
         * when there are no more keys. The returned value is NOT hex-decoded
         * because we expect to use a comptime lookup table. */
        bool next(const uint8_t **out, size_t *out_len) {
            const uint8_t *items = data.items;
            const size_t items_len = data.items_len;
            if (i >= items_len) return false;
            const uint8_t *rem = items + i;
            const size_t rem_len = items_len - i;
            size_t idx = rem_len;
            for (size_t k = 0; k < rem_len; k++) {
                if (rem[k] == ';') {
                    idx = k;
                    break;
                }
            }

            /* Note that if we're at the end, idx + 1 is len + 1 so we're over
             * the end but that's okay because our check above is >= so we'll
             * never read. */
            i += idx + 1;

            *out = rem;
            *out_len = idx;
            return true;
        }
    };

    /* Supported DECRQSS settings */
    enum class DECRQSS : uint8_t {
        none,
        sgr,
        decscusr,
        decstbm,
        decslrm,
    };

    /* Fixed upper bound for an encoded DECRPSS response. The comptime
     * calculated max at the time of writing this was around 63 so this
     * leaves a ton of space for future stuff. We don't do the comptime
     * calculation cause it complicated the implementation a bit too
     * much. */
    static const size_t max_response_bytes = 256;

    Key key;
    XTGETTCAP xtgettcap;
    DECRQSS decrqss;

    Command() : key(Key::decrqss), xtgettcap(), decrqss(DECRQSS::none) {}

    void deinit() {
        switch (key) {
        case Key::xtgettcap: xtgettcap.deinit(); break;
        case Key::decrqss: break;
        }
    }

    /* Encode the response for this request. */
    static void encodeDECRQSS(DECRQSS self, vt::Terminal *t, std::string *response) {
        std::string body;

        switch (self) {
        case DECRQSS::none: break;
        case DECRQSS::sgr: {
            body.append(t->printAttributes());
            body.push_back('m');
            break;
        }
        case DECRQSS::decscusr: {
            const bool blink = t->modes.get(terminal::modes::Mode::cursor_blinking);
            uint8_t style = 0;
            switch (t->screens.active->cursor.cursor_style) {
            case vt::Screen::CursorStyle::block:
            case vt::Screen::CursorStyle::block_hollow: style = blink ? 1 : 2; break;
            case vt::Screen::CursorStyle::underline: style = blink ? 3 : 4; break;
            case vt::Screen::CursorStyle::bar: style = blink ? 5 : 6; break;
            }
            char buf[16];
            snprintf(buf, sizeof buf, "%u q", (unsigned)style);
            body.append(buf);
            break;
        }
        case DECRQSS::decstbm: {
            char buf[32];
            snprintf(buf, sizeof buf, "%u;%ur", (unsigned)(t->scrolling_region.top + 1),
                     (unsigned)(t->scrolling_region.bottom + 1));
            body.append(buf);
            break;
        }
        case DECRQSS::decslrm: {
            if (t->modes.get(terminal::modes::Mode::enable_left_and_right_margin)) {
                char buf[32];
                snprintf(buf, sizeof buf, "%u;%us", (unsigned)(t->scrolling_region.left + 1),
                         (unsigned)(t->scrolling_region.right + 1));
                body.append(buf);
            }
            break;
        }
        }

        const bool valid = !body.empty();
        response->clear();
        response->append("\x1bP");
        response->push_back(valid ? '1' : '0');
        response->append("$r");
        response->append(body);
        response->append("\x1b\\");
    }
};

/* Wisp: the State union's tag. */
enum class StateKey : uint8_t {
    /* We're not in a DCS state at the moment. */
    inactive,

    /* We're hooked, but its an unknown DCS command or one that went
     * invalid due to some bad input, so we're ignoring the rest. */
    ignore,

    /* XTGETTCAP */
    xtgettcap,

    /* DECRQSS */
    decrqss,
};

struct State {
    StateKey key;

    /* XTGETTCAP */
    zigstd::ArrayListUnmanaged<uint8_t> xtgettcap;

    /* DECRQSS */
    struct {
        uint8_t data[2];
        uint8_t len; /* = 0 */
    } decrqss;

    State() : key(StateKey::inactive), xtgettcap() {
        decrqss.data[0] = 0;
        decrqss.data[1] = 0;
        decrqss.len = 0;
    }

    void deinit(zigstd::Allocator alloc) {
        switch (key) {
        case StateKey::inactive:
        case StateKey::ignore: break;

        case StateKey::xtgettcap: xtgettcap.deinit(alloc); break;
        case StateKey::decrqss: break;
        }
    }
};

/* DCS command handler. This should be hooked into a terminal.Stream handler.
 * The hook/put/unhook functions are meant to be called from the
 * terminal.stream dcsHook, dcsPut, and dcsUnhook functions, respectively. */
struct Handler {
    State state; /* = .{ .inactive = {} } */

    /* Maximum bytes any DCS command can take. This is to prevent
     * malicious input from causing us to allocate too much memory.
     * This is arbitrarily set to 1MB today, increase if needed. */
    size_t max_bytes; /* = 1024 * 1024 */

    /* Wisp: the allocator hook was called with, kept so put/unhook and
     * deinit can free without threading it through every call. */
    zigstd::Allocator alloc;

    Handler() : state(), max_bytes(1024 * 1024), alloc(zigstd::c_allocator()) {}

    void discard() {
        state.deinit(alloc);
        state = State();
    }

    void deinit() { discard(); }

    struct Hook {
        State state;
        bool has_command; /* = null */
        Command command;

        Hook() : state(), has_command(false), command() {}
    };

    /* Wisp: `!?Hook` is a bool return (false is an error) plus `has_hook`. */
    bool tryHook(zigstd::Allocator a, const DCS &dcs, bool *has_hook, Hook *out) {
        *has_hook = false;
        switch (dcs.intermediates_len) {
        case 0:
            /* Wisp: tmux control mode ('p') is gated on
             * build_options.tmux_control_mode, which this build disables. */
            return true;

        case 1:
            switch (dcs.intermediates[0]) {
            case '+':
                switch (dcs.final_) {
                /* XTGETTCAP
                 * https://github.com/mitchellh/ghostty/issues/517 */
                case 'q': {
                    Hook hk;
                    hk.state.key = StateKey::xtgettcap;
                    if (!hk.state.xtgettcap.ensureTotalCapacity(a, 128 /* Arbitrary choice */)) return false;
                    *out = hk;
                    *has_hook = true;
                    return true;
                }

                default: return true;
                }

            case '$':
                switch (dcs.final_) {
                /* DECRQSS */
                case 'q': {
                    Hook hk;
                    hk.state.key = StateKey::decrqss;
                    *out = hk;
                    *has_hook = true;
                    return true;
                }

                default: return true;
                }

            default: return true;
            }

        default: return true;
        }
    }

    /* Wisp: `?Command` is a bool return plus the Command out parameter. */
    bool hook(zigstd::Allocator a, const DCS &dcs, Command *out) {
        /* assert(self.state == .inactive) */
        alloc = a;

        /* Initialize our state to ignore in case of error */
        state = State();
        state.key = StateKey::ignore;

        /* Try to parse the hook. */
        bool has_hook = false;
        Hook hk;
        if (!tryHook(a, dcs, &has_hook, &hk)) {
            /* log.info("error initializing DCS hook, will ignore hook err={}") */
            return false;
        }
        if (!has_hook) {
            /* log.info("unknown DCS hook: {}") */
            return false;
        }

        state = hk.state;
        if (!hk.has_command) return false;
        *out = hk.command;
        return true;
    }

    /* Wisp: `!?Command`; `*err` reports the error prong. */
    bool tryPut(uint8_t byte, bool *err) {
        *err = false;
        switch (state.key) {
        case StateKey::inactive:
        case StateKey::ignore: break;

        case StateKey::xtgettcap: {
            if (state.xtgettcap.items_len >= max_bytes) {
                *err = true;
                return false;
            }
            if (!state.xtgettcap.append(alloc, byte)) {
                *err = true;
                return false;
            }
            break;
        }

        case StateKey::decrqss: {
            if (state.decrqss.len >= 2) {
                *err = true;
                return false;
            }

            state.decrqss.data[state.decrqss.len] = byte;
            state.decrqss.len += 1;
            break;
        }
        }

        return false;
    }

    /* Put a byte into the DCS handler. This will return a command
     * if a command needs to be executed. */
    bool put(uint8_t byte, Command *out) {
        (void)out;
        bool err = false;
        const bool has_command = tryPut(byte, &err);
        if (err) {
            /* On error we just discard our state and ignore the rest
             * log.info("error putting byte into DCS handler err={}") */
            discard();
            state.key = StateKey::ignore;
            return false;
        }
        return has_command;
    }

    bool unhook(Command *out) {
        /* Note: we do NOT call deinit here on purpose because some commands
         * transfer memory ownership. If state needs cleanup, the switch
         * prong below should handle it. */
        bool result = false;
        switch (state.key) {
        case StateKey::inactive:
        case StateKey::ignore: break;

        case StateKey::xtgettcap: {
            /* Note: purposely do not deinit our state here because
             * we copy it into the resulting command. */
            for (size_t i = 0; i < state.xtgettcap.items_len; i++) {
                uint8_t b = state.xtgettcap.items[i];
                if (b >= 'a' && b <= 'z') b = (uint8_t)(b - 'a' + 'A');
                state.xtgettcap.items[i] = b;
            }
            Command cmd;
            cmd.key = Command::Key::xtgettcap;
            cmd.xtgettcap.data = state.xtgettcap;
            cmd.xtgettcap.alloc = alloc;
            *out = cmd;
            result = true;
            break;
        }

        case StateKey::decrqss: {
            Command cmd;
            cmd.key = Command::Key::decrqss;
            switch (state.decrqss.len) {
            case 0: cmd.decrqss = Command::DECRQSS::none; break;
            case 1:
                switch (state.decrqss.data[0]) {
                case 'm': cmd.decrqss = Command::DECRQSS::sgr; break;
                case 'r': cmd.decrqss = Command::DECRQSS::decstbm; break;
                case 's': cmd.decrqss = Command::DECRQSS::decslrm; break;
                default: cmd.decrqss = Command::DECRQSS::none; break;
                }
                break;
            case 2:
                switch (state.decrqss.data[0]) {
                case ' ':
                    switch (state.decrqss.data[1]) {
                    case 'q': cmd.decrqss = Command::DECRQSS::decscusr; break;
                    default: cmd.decrqss = Command::DECRQSS::none; break;
                    }
                    break;
                default: cmd.decrqss = Command::DECRQSS::none; break;
                }
                break;
            default: cmd.decrqss = Command::DECRQSS::none; break;
            }
            *out = cmd;
            result = true;
            break;
        }
        }

        state = State();
        return result;
    }
};

} /* namespace dcs */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_DCS_HPP */
