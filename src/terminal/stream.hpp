/* Reimplemented after Ghostty src/terminal/stream.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * REIMPLEMENTED, NOT TRANSLITERATED. This file was written from Ghostty's
 * design and verified against Wisp's own tests, without the upstream source
 * to hand. It follows upstream's structure but has not been checked against
 * it line by line, and its behaviour will differ in places. It is due to be
 * replaced by a transliteration checked against upstream's own tests, as
 * parser.hpp has been.
 *
 * Bytes from a program, turned into things happening to a terminal.
 *
 * The parser says what arrived; this says what it means. CSI 5 A is a
 * csi_dispatch with a final byte of A as far as the state machine is
 * concerned, and it is the cursor moving up five rows only here. Keeping the
 * two apart is what makes the state machine testable without a terminal and
 * this testable without worrying about bytes.
 *
 * Three things live here that have nowhere else to be:
 *
 * UTF-8 decoding. The parser works in bytes because the escape sequence
 * grammar is ASCII, but what gets printed is a codepoint, so the decoding
 * happens between the two. It has to survive being cut mid-character for the
 * same reason the parser does.
 *
 * How wide a character is, which decides whether it takes one cell or two,
 * and whether it is a combining mark that joins the character before it
 * rather than taking a cell at all.
 *
 * SGR. sgr.hpp was ported early and has had no caller since; this is it. The
 * attributes it produces are applied to the cursor's style, which is what
 * every subsequently printed character is then written with.
 */

#pragma once
#ifndef WISP_TERMINAL_STREAM_HPP
#define WISP_TERMINAL_STREAM_HPP

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "terminal.hpp"
#include "parser.hpp"
#include "sgr.hpp"

namespace wisp {
namespace terminal {

/* ─── how wide is a character ────────────────────────────────────────────── */

/* Columns a codepoint occupies: 0, 1 or 2.
 *
 * PLACEHOLDER. The real answer is a table generated from the Unicode
 * database, which upstream builds at compile time and which is thousands of
 * ranges. This covers the cases that decide whether ordinary output looks
 * right — combining marks take no cell, the CJK and emoji blocks take two,
 * everything else takes one — and is wrong in the places a generated table
 * exists to get right. It is deliberately one function so that replacing it
 * is replacing one function. */
inline int stream_codepoint_width(uint32_t cp) {
    if (cp == 0) return 0;

    /* Combining marks, which attach to the character before them. */
    if ((cp >= 0x0300 && cp <= 0x036F) ||   /* combining diacriticals */
        (cp >= 0x1AB0 && cp <= 0x1AFF) ||
        (cp >= 0x1DC0 && cp <= 0x1DFF) ||
        (cp >= 0x20D0 && cp <= 0x20FF) ||   /* combining marks for symbols */
        (cp >= 0xFE00 && cp <= 0xFE0F) ||   /* variation selectors */
        (cp >= 0xFE20 && cp <= 0xFE2F)) {
        return 0;
    }

    /* Zero-width joiner and the direction marks. */
    if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF) return 0;

    if ((cp >= 0x1100 && cp <= 0x115F) ||   /* Hangul jamo */
        (cp >= 0x2E80 && cp <= 0x303E) ||   /* CJK radicals, punctuation */
        (cp >= 0x3041 && cp <= 0x33FF) ||   /* kana, CJK compatibility */
        (cp >= 0x3400 && cp <= 0x4DBF) ||
        (cp >= 0x4E00 && cp <= 0x9FFF) ||   /* CJK unified ideographs */
        (cp >= 0xA000 && cp <= 0xA4CF) ||   /* Yi */
        (cp >= 0xAC00 && cp <= 0xD7A3) ||   /* Hangul syllables */
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE6F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) ||   /* fullwidth forms */
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x1F300 && cp <= 0x1F64F) || /* emoji */
        (cp >= 0x1F900 && cp <= 0x1F9FF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD)) {
        return 2;
    }

    return 1;
}

/* ─── UTF-8 ──────────────────────────────────────────────────────────────── */

/* A decoder that can be stopped in the middle of a character.
 *
 * The same problem the parser has: a read can end anywhere, including between
 * the bytes of one character. So the partial state is a field rather than a
 * local. */
struct Utf8 {
    uint32_t cp;         /* what has been accumulated */
    uint8_t  remaining;  /* continuation bytes still expected */

    Utf8() : cp(0), remaining(0) {}
};

/* Feed one byte, and get back however many codepoints it completed.
 *
 * Usually none or one. Two happens when a byte both ends a broken character
 * and begins a good one, which is why this returns a count rather than a
 * bool: an ASCII byte interrupting a half-finished sequence is two separate
 * pieces of news.
 *
 * Invalid input yields U+FFFD, the replacement character, rather than being
 * dropped — including a sequence that was cut short by something other than
 * its own continuation bytes. A terminal that silently swallowed bad bytes
 * would leave a user staring at output with a hole in it and no reason for
 * it; the replacement at least says something arrived that could not be
 * read. */
inline int utf8_next(Utf8 *u, uint8_t b, uint32_t out[2]) {
    if (u->remaining > 0) {
        if ((b & 0xC0) != 0x80) {
            /* Not a continuation byte, so the character was truncated. What
             * was collected is unreadable and says so, and the byte that
             * interrupted it is not part of it — it starts a new character,
             * which is decoded here rather than pushed back. */
            u->remaining = 0;
            u->cp = 0;

            out[0] = 0xFFFD;
            if (b < 0x80) {
                out[1] = b;
                return 2;
            }

            /* The interrupting byte begins a sequence of its own. */
            return 1 + utf8_next(u, b, out + 1);
        }

        u->cp = (u->cp << 6) | (uint32_t)(b & 0x3F);
        u->remaining--;
        if (u->remaining > 0) return 0;

        out[0] = u->cp;
        u->cp = 0;
        return 1;
    }

    if (b < 0x80) {
        out[0] = b;
        return 1;
    }
    if ((b & 0xE0) == 0xC0) {
        u->cp = (uint32_t)(b & 0x1F);
        u->remaining = 1;
        return 0;
    }
    if ((b & 0xF0) == 0xE0) {
        u->cp = (uint32_t)(b & 0x0F);
        u->remaining = 2;
        return 0;
    }
    if ((b & 0xF8) == 0xF0) {
        u->cp = (uint32_t)(b & 0x07);
        u->remaining = 3;
        return 0;
    }

    /* A continuation byte with nothing to continue, or a length nobody
     * defines. */
    out[0] = 0xFFFD;
    return 1;
}

/* ─── the stream ─────────────────────────────────────────────────────────── */

/* Where answers go.
 *
 * Some sequences are questions — where is the cursor, what kind of terminal
 * are you — and a program that asks one usually waits for the answer before
 * doing anything else. A terminal that swallowed the question would leave it
 * waiting, which from the user's side looks like the program hanging. So the
 * stream is given somewhere to write the reply: in practice the pty the
 * program is reading from. */
typedef void (*StreamRespond)(void *ctx, const char *data, size_t len);

struct Stream {
    Terminal *terminal;
    parser::Parser parser;
    Utf8 utf8;

    StreamRespond respond;
    void         *respond_ctx;

    Stream()
        : terminal(nullptr), parser(), utf8(), respond(nullptr),
          respond_ctx(nullptr) {}
};

inline void stream_reply(Stream *s, const char *data, size_t len) {
    if (s->respond) s->respond(s->respond_ctx, data, len);
}

/* A fixed reply, measured rather than counted. Counting the bytes of an escape
 * sequence by hand is exactly the kind of thing that is wrong by one, and a
 * reply one byte too long sends the program a NUL it did not ask for. */
inline void stream_reply_str(Stream *s, const char *text) {
    stream_reply(s, text, strlen(text));
}

inline void stream_init(Stream *s, Terminal *t) {
    /* Wisp: the parser owns an OSC parser that cannot be copied, so the
     * stream is reset field by field rather than assigned a fresh one. */
    s->terminal = t;
    s->parser.state = parser::State::ground;
    s->parser.clear();
    s->parser.osc_parser.reset();
    s->utf8 = Utf8();
    s->respond = nullptr;
    s->respond_ctx = nullptr;
}

/* ─── SGR ────────────────────────────────────────────────────────────────── */

/* Apply one SGR attribute to the cursor's style.
 *
 * The style is the cursor's rather than the screen's because it is a property
 * of what will be written next, not of anything already on the screen. */
inline void stream_apply_sgr(Terminal *t, const sgr::Attribute &a) {
    typedef sgr::Attribute::Tag AttributeTag;
    style::Style &st = t->active->cursor.style;

    switch (a.tag) {
        case AttributeTag::unset:
            st = style::Style();
            break;

        case AttributeTag::bold: st.flags.bold = true; break;
        case AttributeTag::reset_bold:
            /* SGR 22 turns off both, which is a quirk of the standard rather
             * than an oversight here. */
            st.flags.bold = false;
            st.flags.faint = false;
            break;
        case AttributeTag::faint: st.flags.faint = true; break;
        case AttributeTag::italic: st.flags.italic = true; break;
        case AttributeTag::reset_italic: st.flags.italic = false; break;

        case AttributeTag::underline:
            st.flags.underline = a.underline;
            break;
        case AttributeTag::underline_color:
            st.underline_color.tag = style::StyleColor::Tag::rgb;
            st.underline_color.rgb = a.rgb;
            break;
        case AttributeTag::underline_color_256:
            st.underline_color.tag = style::StyleColor::Tag::palette;
            st.underline_color.palette = a.index;
            break;
        case AttributeTag::reset_underline_color:
            st.underline_color.tag = style::StyleColor::Tag::none;
            break;

        case AttributeTag::overline: st.flags.overline = true; break;
        case AttributeTag::reset_overline: st.flags.overline = false; break;
        case AttributeTag::blink: st.flags.blink = true; break;
        case AttributeTag::reset_blink: st.flags.blink = false; break;
        case AttributeTag::inverse: st.flags.inverse = true; break;
        case AttributeTag::reset_inverse: st.flags.inverse = false; break;
        case AttributeTag::invisible: st.flags.invisible = true; break;
        case AttributeTag::reset_invisible:
            st.flags.invisible = false;
            break;
        case AttributeTag::strikethrough:
            st.flags.strikethrough = true;
            break;
        case AttributeTag::reset_strikethrough:
            st.flags.strikethrough = false;
            break;

        case AttributeTag::direct_color_fg:
            st.fg_color.tag = style::StyleColor::Tag::rgb;
            st.fg_color.rgb = a.rgb;
            break;
        case AttributeTag::direct_color_bg:
            st.bg_color.tag = style::StyleColor::Tag::rgb;
            st.bg_color.rgb = a.rgb;
            break;

        case AttributeTag::fg_8:
        case AttributeTag::bright_fg_8:
            st.fg_color.tag = style::StyleColor::Tag::palette;
            st.fg_color.palette = (uint8_t)a.name;
            break;
        case AttributeTag::fg_256:
            st.fg_color.tag = style::StyleColor::Tag::palette;
            st.fg_color.palette = a.index;
            break;

        case AttributeTag::bg_8:
        case AttributeTag::bright_bg_8:
            st.bg_color.tag = style::StyleColor::Tag::palette;
            st.bg_color.palette = (uint8_t)a.name;
            break;
        case AttributeTag::bg_256:
            st.bg_color.tag = style::StyleColor::Tag::palette;
            st.bg_color.palette = a.index;
            break;

        case AttributeTag::reset_fg:
            st.fg_color.tag = style::StyleColor::Tag::none;
            break;
        case AttributeTag::reset_bg:
            st.bg_color.tag = style::StyleColor::Tag::none;
            break;

        case AttributeTag::unknown:
            /* Something nobody here implements. Ignored rather than guessed
             * at, and not a reason to abandon the rest of the sequence: a
             * program setting one attribute this does not know still meant
             * the others. */
            break;
    }
}

/* ─── views onto the parser's actions ───────────────────────────────────── */

/* Wisp: parser.hpp is now a transliteration of upstream, whose CSI action
 * carries a private marker such as '?' as the first intermediate and marks a
 * colon as the separator after a parameter. The dispatch below was written
 * against the reimplemented parser that came before, so these views present
 * the exact actions in the terms it reads. They go when stream.zig is
 * transliterated and the dispatch is replaced with upstream's. */
struct CsiView {
    uint8_t         final_byte;
    uint8_t         private_marker;
    const uint8_t  *intermediates;       /* after the private marker */
    size_t          intermediate_count;
    const uint16_t *params;
    size_t          param_count;
    parser::SepList params_sep;

    explicit CsiView(const parser::Action::CSI &c)
        : final_byte(c.final_), private_marker(0),
          intermediates(c.intermediates),
          intermediate_count(c.intermediates_len), params(c.params),
          param_count(c.params_len), params_sep(c.params_sep) {
        if (intermediate_count > 0 && intermediates[0] >= 0x3C &&
            intermediates[0] <= 0x3F) {
            private_marker = intermediates[0];
            intermediates++;
            intermediate_count--;
        }
    }
};

/* The value of a parameter, or a default when it was omitted or zero. */
inline uint16_t csi_param(const CsiView &a, size_t index, uint16_t fallback) {
    if (index >= a.param_count) return fallback;
    const uint16_t v = a.params[index];
    return v == 0 ? fallback : v;
}

/* The raw value, for the parameters where zero means zero. */
inline uint16_t csi_param_raw(const CsiView &a, size_t index, uint16_t fallback) {
    if (index >= a.param_count) return fallback;
    return a.params[index];
}

/* ─── dispatch ───────────────────────────────────────────────────────────── */

inline void stream_execute(Terminal *t, uint8_t b) {
    switch (b) {
        case 0x07: break;                      /* BEL: nothing visual */
        case 0x08: terminal_backspace(t); break;
        case 0x09: terminal_horizontal_tab(t, 1); break;
        case 0x0A:                             /* LF */
        case 0x0B:                             /* VT, which acts as LF */
        case 0x0C:                             /* FF, likewise */
            terminal_linefeed(t);
            break;
        case 0x0D: terminal_carriage_return(t); break;
        case 0x0E: terminal_shift_out(t); break;   /* SO: print from G1 */
        case 0x0F: terminal_shift_in(t); break;    /* SI: back to G0 */
        default: break;
    }
}

inline void stream_csi(Terminal *t, const CsiView &a) {

    Screen *s = t->active;

    /* Private sequences are a different namespace: CSI ? 25 h has nothing to
     * do with CSI 25 h. */
    if (a.private_marker == '?') {
        const bool set = a.final_byte == 'h';
        if (a.final_byte != 'h' && a.final_byte != 'l') return;

        for (size_t i = 0; i < a.param_count; i++) {
            switch (a.params[i]) {
                case 1: t->modes.cursor_keys = set; break;
                case 6:
                    t->modes.origin = set;
                    /* Changing the origin homes the cursor, since the
                     * coordinates it was at mean something else now. */
                    terminal_cursor_position(t, 0, 0);
                    break;
                case 7:
                    t->modes.wraparound = set;
                    terminal_apply_modes(t);
                    break;
                case 1047:
                case 1049:
                    if (set) {
                        terminal_alt_screen_enter(t);
                    } else {
                        terminal_alt_screen_leave(t);
                    }
                    break;
                default: break;
            }
        }
        return;
    }

    if (a.private_marker != 0) return;

    switch (a.final_byte) {
        case '@': terminal_insert_chars(t, csi_param(a, 0, 1)); break;
        case 'A': screen_cursor_up(s, csi_param(a, 0, 1)); break;
        case 'B': screen_cursor_down(s, csi_param(a, 0, 1)); break;
        case 'C': screen_cursor_right(s, csi_param(a, 0, 1)); break;
        case 'D': screen_cursor_left(s, csi_param(a, 0, 1)); break;

        case 'G':
            /* CHA: a column, counted from one. */
            terminal_cursor_position(t, (CellCountInt)(csi_param(a, 0, 1) - 1),
                                     s->cursor.y);
            break;

        case 'H':
        case 'f':
            terminal_cursor_position(t, (CellCountInt)(csi_param(a, 1, 1) - 1),
                                     (CellCountInt)(csi_param(a, 0, 1) - 1));
            break;

        case 'J': screen_erase_display(s, csi_param_raw(a, 0, 0), false); break;
        case 'K': screen_erase_line(s, csi_param_raw(a, 0, 0), false); break;
        case 'L': terminal_insert_lines(t, csi_param(a, 0, 1)); break;
        case 'M': terminal_delete_lines(t, csi_param(a, 0, 1)); break;
        case 'P': terminal_delete_chars(t, csi_param(a, 0, 1)); break;
        case 'X': screen_erase_chars(s, csi_param(a, 0, 1), false); break;
        case 'Z': terminal_reverse_tab(t, csi_param(a, 0, 1)); break;

        case 'd':
            /* VPA: a row, counted from one. */
            terminal_cursor_position(t, s->cursor.x,
                                     (CellCountInt)(csi_param(a, 0, 1) - 1));
            break;

        case 'g': terminal_tab_clear(t, csi_param_raw(a, 0, 0)); break;

        case 'h':
        case 'l': {
            const bool set = a.final_byte == 'h';
            for (size_t i = 0; i < a.param_count; i++) {
                if (a.params[i] == 4) t->modes.insert = set;
            }
            break;
        }

        case 'm': {
            sgr::Parser p(a.params, a.param_count, a.params_sep);
            sgr::Attribute attr;
            while (p.next(&attr)) stream_apply_sgr(t, attr);
            break;
        }

        case 'r':
            terminal_set_scroll_region(
                t, (CellCountInt)(csi_param(a, 0, 1) - 1),
                (CellCountInt)(csi_param(a, 1, (uint16_t)t->rows) - 1));
            break;

        default: break;
    }
}

inline void stream_esc(Terminal *t, const parser::Action::ESC &a) {
    if (a.intermediates_len == 1) {
        /* ESC ( ) * + load a set into G0 to G3. */
        switch (a.intermediates[0]) {
            case '(': terminal_designate_charset(t, 0, a.final_); return;
            case ')': terminal_designate_charset(t, 1, a.final_); return;
            case '*': terminal_designate_charset(t, 2, a.final_); return;
            case '+': terminal_designate_charset(t, 3, a.final_); return;
            default: return;
        }
    }

    if (a.intermediates_len > 0) {
        /* Anything else with intermediates is not implemented, and doing
         * nothing is right until something is — guessing would corrupt
         * output rather than merely not improving it. */
        return;
    }

    switch (a.final_) {
        case '7': terminal_save_cursor(t); break;
        case '8': terminal_restore_cursor(t); break;
        case 'N': terminal_single_shift(t, 2); break;
        case 'O': terminal_single_shift(t, 3); break;
        case 'D': terminal_linefeed(t); break;
        case 'E':
            terminal_carriage_return(t);
            terminal_linefeed(t);
            break;
        case 'H': terminal_tab_set(t); break;
        case 'M': terminal_reverse_index(t); break;
        default: break;
    }
}

/* ─── OSC ────────────────────────────────────────────────────────────────── */

/* OSC commands arrive decoded, from osc.hpp. What is left here is applying
 * them. */

/* OSC 8 — open or close a hyperlink.
 *
 * Everything printed while a link is open belongs to it. The id is how two
 * separated runs of text can be told they are the same link, so that
 * hovering one highlights both. */
inline void stream_osc_hyperlink_start(Terminal *t, const osc::Command &cmd) {
    Cursor &c = t->active->cursor;
    const osc::ZStr &uri = cmd.hyperlink_start.uri;

    /* A URI too long to hold is dropped rather than truncated. A truncated
     * one would still look like a link and go somewhere nobody meant. */
    if (uri.len >= sizeof(c.hyperlink_uri)) {
        c.hyperlink_active = false;
        return;
    }

    memcpy(c.hyperlink_uri, uri.ptr, uri.len);
    c.hyperlink_uri[uri.len] = '\0';
    c.hyperlink_uri_len = uri.len;

    c.hyperlink_id_len = 0;
    c.hyperlink_id[0] = '\0';
    if (cmd.hyperlink_start.has_id &&
        cmd.hyperlink_start.id.len < sizeof(c.hyperlink_id)) {
        memcpy(c.hyperlink_id, cmd.hyperlink_start.id.ptr,
               cmd.hyperlink_start.id.len);
        c.hyperlink_id[cmd.hyperlink_start.id.len] = '\0';
        c.hyperlink_id_len = cmd.hyperlink_start.id.len;
    }

    /* A link without an id gets a fresh implicit one, so that this run is
     * its own link even if the same URI was linked a moment ago. */
    c.hyperlink_implicit = t->next_implicit_link++;
    c.hyperlink_active = true;
}

inline void stream_osc(Terminal *t, const osc::Command &cmd) {
    typedef osc::Command::Key K;

    switch (cmd.key) {
        case K::change_window_title: {
            const size_t n = cmd.change_window_title.len < sizeof(t->title) - 1
                                 ? cmd.change_window_title.len
                                 : sizeof(t->title) - 1;
            memcpy(t->title, cmd.change_window_title.ptr, n);
            t->title[n] = '\0';
            t->title_len = n;
            break;
        }

        case K::hyperlink_start:
            stream_osc_hyperlink_start(t, cmd);
            break;

        case K::hyperlink_end:
            t->active->cursor.hyperlink_active = false;
            break;

        default:
            /* Everything else is decoded but not yet acted on. Silently
             * doing nothing is the correct answer to a request a terminal
             * does not support. */
            break;
    }
}

/* ─── reports ────────────────────────────────────────────────────────────── */

/* DSR and DA — the questions.
 *
 * The answer to "what are you" matters more than it looks. Programs decide
 * what to send from it, and claiming too little means they fall back to
 * plain text while claiming too much means they send things this cannot draw.
 * VT220 with ANSI colour is what the terminals programs are tested against
 * report, so it is the answer that produces the output people expect. */
inline void stream_report(Stream *st, const CsiView &a) {
    Terminal *t = st->terminal;
    char buf[64];

    if (a.final_byte == 'n' && a.private_marker == 0) {
        const uint16_t what = csi_param_raw(a, 0, 0);

        if (what == 5) {
            /* "Are you all right?" — always yes. */
            stream_reply_str(st, "\x1b[0n");
            return;
        }

        if (what == 6) {
            /* Where is the cursor, counted from one, and relative to the
             * scroll region when origin mode says positions are. */
            const CellCountInt base = terminal_origin_row(t);
            const unsigned row = (unsigned)(t->active->cursor.y - base) + 1;
            const unsigned col = (unsigned)t->active->cursor.x + 1;
            const int n = snprintf(buf, sizeof(buf), "\x1b[%u;%uR", row, col);
            if (n > 0) stream_reply(st, buf, (size_t)n);
            return;
        }
        return;
    }

    if (a.final_byte == 'c') {
        if (a.private_marker == 0 && csi_param_raw(a, 0, 0) == 0) {
            /* DA1: a VT220 (62) with ANSI colour (22). */
            stream_reply_str(st, "\x1b[?62;22c");
            return;
        }
        if (a.private_marker == '>') {
            /* DA2: the terminal type and a version. 1 is VT220; the version
             * is Wisp's own and means nothing to anyone else. */
            stream_reply_str(st, "\x1b[>1;10;0c");
            return;
        }
    }
}

/* ─── feeding it ─────────────────────────────────────────────────────────── */

inline void stream_print(Stream *s, uint32_t cp) {
    Terminal *t = s->terminal;
    const int width = stream_codepoint_width(cp);

    if (width == 0) {
        /* A combining mark joins the character before it rather than taking a
         * cell. A mark with nothing to join is dropped: it has nothing to
         * modify, and a cell of its own would render as a stray accent. */
        terminal_print_combining(t, cp);
        return;
    }

    terminal_print(t, cp, width);
}

/* Feed bytes. Everything that can go wrong in here has already been decided
 * somewhere lower down, so this reports nothing: a terminal's whole job is to
 * keep going.
 *
 * Wisp: UTF-8 is decoded here, before the parser, and only in the ground
 * state. That is upstream's arrangement — its transition table prints only
 * 0x20-0x7F and says "Ghostty doesn't honor 8-bit C1 controls in the ground
 * state either (they go through UTF-8 decoding)" — but the loop itself is
 * not yet a transliteration of stream.zig. */
inline void stream_dispatch(Stream *s, const parser::Action &a) {
    typedef parser::Action::Tag Tag;

    switch (a.tag) {
        case Tag::print:
            stream_print(s, a.print);
            break;

        case Tag::execute:
            stream_execute(s->terminal, a.byte);
            break;

        case Tag::csi_dispatch: {
            const CsiView v(a.csi_dispatch);
            if (v.final_byte == 'n' || v.final_byte == 'c') {
                stream_report(s, v);
            } else {
                stream_csi(s->terminal, v);
            }
            break;
        }

        case Tag::esc_dispatch:
            stream_esc(s->terminal, a.esc_dispatch);
            break;

        case Tag::osc_dispatch:
            stream_osc(s->terminal, a.osc_dispatch);
            break;

        case Tag::dcs_hook:
        case Tag::dcs_put:
        case Tag::dcs_unhook:
        case Tag::apc_start:
        case Tag::apc_put:
        case Tag::apc_end:
            break;
    }
}

inline void stream_feed(Stream *s, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        const uint8_t b = data[i];

        /* In the ground state a high byte, or any byte while a character is
         * half decoded, belongs to UTF-8 rather than to the parser. */
        if (s->parser.state == parser::State::ground &&
            (b >= 0x80 || s->utf8.remaining > 0)) {
            uint32_t cps[2] = {0, 0};
            const int n = utf8_next(&s->utf8, b, cps);
            for (int k = 0; k < n; k++) {
                /* A control byte that interrupted a character is decoded
                 * back out as itself; it goes to the parser, not the
                 * screen. */
                if (cps[k] < 0x20 || cps[k] == 0x7F || cps[k] == 0x1B) {
                    const parser::Next nx = s->parser.next((uint8_t)cps[k]);
                    for (int j = 0; j < 3; j++) {
                        if (nx.has(j)) stream_dispatch(s, nx[j]);
                    }
                } else {
                    stream_print(s, cps[k]);
                }
            }
            continue;
        }

        const parser::Next nx = s->parser.next(b);
        for (int j = 0; j < 3; j++) {
            if (nx.has(j)) stream_dispatch(s, nx[j]);
        }
    }
}

inline void stream_feed_text(Stream *s, const char *text, size_t len) {
    stream_feed(s, (const uint8_t *)text, len);
}

} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_STREAM_HPP */
