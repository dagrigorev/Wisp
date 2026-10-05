/* Transliterated from the test blocks in Ghostty src/terminal/stream.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Mapping from Zig, beyond test_parser_port.cpp's:
 *   Stream(H) = .init(.{ .handler = .{} })    Stream<H> s(H());
 *   .allocator = alloc                         Stream<H>::Options with allocator
 *   comptime action / value                    const Action &a (a.tag, a.<field>)
 *
 * Not here yet:
 *   "test Action" only reifies the C ABI type, which is not carried over.
 */

#include "test_helpers.h"
#include "vt_stream.hpp"

#include <string>
#include <vector>

using namespace wisp::terminal;
using stream::Action;
using stream::Stream;
typedef Action::Key K;

static std::string utf8(uint32_t cp) {
    std::string s;
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F));
    } else {
        s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F));
        s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F));
    }
    return s;
}

template <typename H>
static void feed_bytes(Stream<H> &s, const char *str) {
    for (const char *c = str; *c; c++) s.next((uint8_t)*c);
}

/* ─── stream: print ─────────────────────────────────────────────────────── */

struct PrintH {
    bool has_c; uint32_t c;
    PrintH() : has_c(true), c(0) {}
    void vt(const Action &a) {
        switch (a.tag) {
            case K::print: has_c = true; c = a.cp; break;
            default: break;
        }
    }
};

TEST(stream, print) {
    Stream<PrintH> s((PrintH()));
    s.next('x');
    ASSERT_TRUE(s.handler.c == 'x');
}

struct PrintSliceH {
    bool has_c; uint32_t c;
    explicit PrintSliceH(bool has = true) : has_c(has), c(0) {}
    void vt(const Action &a) {
        switch (a.tag) {
            case K::print: has_c = true; c = a.cp; break;
            case K::print_slice: has_c = true; c = a.print_slice.cps[a.print_slice.len - 1]; break;
            default: break;
        }
    }
};

TEST(simd, print_invalid_utf_8) {
    Stream<PrintSliceH> s((PrintSliceH()));
    const uint8_t in[] = { 0xFF };
    s.nextSlice(in, 1);
    ASSERT_TRUE(s.handler.c == 0xFFFD);
}

TEST(simd, complete_incomplete_utf_8) {
    Stream<PrintSliceH> s((PrintSliceH(false)));
    const uint8_t a[] = { 0xE0 }, b[] = { 0xA0 }, c[] = { 0x80 };
    s.nextSlice(a, 1); /* 3 byte */
    ASSERT_FALSE(s.handler.has_c);
    s.nextSlice(b, 1); /* still incomplete */
    ASSERT_FALSE(s.handler.has_c);
    s.nextSlice(c, 1);
    ASSERT_TRUE(s.handler.has_c && s.handler.c == 0x800);
}

/* ─── ground-state controls ─────────────────────────────────────────────── */

struct BufH {
    uint32_t buf[128];
    size_t len;
    BufH() : len(0) {}
    void vt(const Action &a) {
        switch (a.tag) {
            case K::print: buf[len++] = a.cp; break;
            case K::print_slice:
                for (size_t i = 0; i < a.print_slice.len; i++) buf[len++] = a.print_slice.cps[i];
                break;
            default: break;
        }
    }
};

TEST(stream, ground_state_C0_controls_are_executed_not_printed) {
    /* Every C0 control except ESC must never produce a print action
     * in the ground state. ESC is excluded because it begins an
     * escape sequence rather than executing. */
    for (unsigned c = 0; c < 0x20; c++) {
        if (c == 0x1B) continue;

        /* Scalar path. */
        {
            Stream<BufH> s((BufH()));
            s.next('A');
            s.next((uint8_t)c);
            s.next('B');
            ASSERT_TRUE(s.handler.len == 2);
            ASSERT_TRUE(s.handler.buf[0] == 'A');
            ASSERT_TRUE(s.handler.buf[1] == 'B');
        }

        /* Batched path. */
        {
            Stream<BufH> s((BufH()));
            const uint8_t in[] = { 'A', 'B', (uint8_t)c, 'C', 'D' };
            s.nextSlice(in, 5);
            ASSERT_TRUE(s.handler.len == 4);
            for (size_t i = 0; i < 4; i++) ASSERT_TRUE(s.handler.buf[i] == (uint32_t)"ABCD"[i]);
        }

        /* Batched path with a run long enough to exercise the
         * vectorized printable-run scan on either side of the control. */
        {
            Stream<BufH> s((BufH()));
            uint8_t input[65];
            memset(input, 'A', sizeof(input));
            input[32] = (uint8_t)c;
            s.nextSlice(input, sizeof(input));
            ASSERT_TRUE(s.handler.len == 64);
            for (size_t i = 0; i < s.handler.len; i++) ASSERT_TRUE(s.handler.buf[i] == 'A');
        }
    }
}

TEST(stream, ground_state_UTF_8_decoded_C1_controls_are_ignored) {
    /* Every C1 control that arrives as well-formed UTF-8 (a two byte
     * 0xC2-lead sequence) must be dropped: not printed and not
     * interpreted as a control (e.g. U+009B must not start a CSI). */
    for (unsigned c = 0x80; c < 0xA0; c++) {
        const uint8_t enc[2] = { 0xC2, (uint8_t)c };

        /* Scalar path. */
        {
            Stream<BufH> s((BufH()));
            s.next('A');
            s.next(enc[0]);
            s.next(enc[1]);
            s.next('B');
            ASSERT_TRUE(s.handler.len == 2);
            ASSERT_TRUE(s.handler.buf[0] == 'A');
            ASSERT_TRUE(s.handler.buf[1] == 'B');
        }

        /* Batched path. */
        {
            Stream<BufH> s((BufH()));
            const uint8_t in[] = { 'A', 'B', enc[0], enc[1], 'C', 'D' };
            s.nextSlice(in, 6);
            ASSERT_TRUE(s.handler.len == 4);
            for (size_t i = 0; i < 4; i++) ASSERT_TRUE(s.handler.buf[i] == (uint32_t)"ABCD"[i]);
        }

        /* Batched path with a run long enough to exercise the
         * vectorized printable-run scan on either side of the control. */
        {
            Stream<BufH> s((BufH()));
            uint8_t input[66];
            memset(input, 'A', sizeof(input));
            input[32] = enc[0];
            input[33] = enc[1];
            s.nextSlice(input, sizeof(input));
            ASSERT_TRUE(s.handler.len == 64);
            for (size_t i = 0; i < s.handler.len; i++) ASSERT_TRUE(s.handler.buf[i] == 'A');
        }
    }

    /* U+00A0 (NBSP), just past the C1 range, must still print. */
    {
        Stream<BufH> s((BufH()));
        const uint8_t in[] = { 0xC2, 0xA0 };
        s.nextSlice(in, 2);
        ASSERT_TRUE(s.handler.len == 1);
        ASSERT_TRUE(s.handler.buf[0] == 0xA0);
    }

    /* A codepoint whose continuation byte falls in the C1 range must
     * still print: "Ü" is 0xC3 0x9C. */
    {
        Stream<BufH> s((BufH()));
        s.nextSlice("\xC3\x9C");
        ASSERT_TRUE(s.handler.len == 1);
        ASSERT_TRUE(s.handler.buf[0] == 0xDC);
    }

    /* A raw C1 byte is ill-formed UTF-8, not a decoded C1: it must
     * still produce a U+FFFD replacement. */
    {
        Stream<BufH> s((BufH()));
        const uint8_t in[] = { 0x9B };
        s.nextSlice(in, 1);
        ASSERT_TRUE(s.handler.len == 1);
        ASSERT_TRUE(s.handler.buf[0] == 0xFFFD);
    }
}

/* ─── CSI ───────────────────────────────────────────────────────────────── */

struct CufH {
    uint16_t amount;
    CufH() : amount(0) {}
    void vt(const Action &a) { if (a.tag == K::cursor_right) amount = a.cursor.value; }
};

TEST(stream, cursor_right_CUF) {
    Stream<CufH> s((CufH()));
    s.nextSlice("\x1B[C");
    ASSERT_TRUE(s.handler.amount == 1);

    s.nextSlice("\x1B[5C");
    ASSERT_TRUE(s.handler.amount == 5);

    s.handler.amount = 0;
    s.nextSlice("\x1B[5;4C");
    ASSERT_TRUE(s.handler.amount == 0);

    s.handler.amount = 0;
    s.nextSlice("\x1b[?3C");
    ASSERT_TRUE(s.handler.amount == 0);
}

struct DecModeH {
    modes::Mode mode;
    DecModeH() : mode((modes::Mode)1) {}
    void vt(const Action &a) {
        switch (a.tag) {
            case K::set_mode: mode = a.mode; break;
            case K::reset_mode: mode = (modes::Mode)1; break;
            default: break;
        }
    }
};

TEST(stream, dec_set_mode_SM_and_reset_mode_RM) {
    Stream<DecModeH> s((DecModeH()));
    s.nextSlice("\x1B[?6h");
    ASSERT_TRUE(s.handler.mode == modes::Mode::origin);

    s.nextSlice("\x1B[?6l");
    ASSERT_TRUE(s.handler.mode == (modes::Mode)1);

    s.handler.mode = (modes::Mode)1;
    s.nextSlice("\x1B[6 h");
    ASSERT_TRUE(s.handler.mode == (modes::Mode)1);
}

struct AnsiModeH {
    bool has_mode; modes::Mode mode;
    AnsiModeH() : has_mode(false), mode(modes::Mode::cursor_keys) {}
    void vt(const Action &a) {
        switch (a.tag) {
            case K::set_mode: has_mode = true; mode = a.mode; break;
            case K::reset_mode: has_mode = false; break;
            default: break;
        }
    }
};

TEST(stream, ansi_set_mode_SM_and_reset_mode_RM) {
    Stream<AnsiModeH> s((AnsiModeH()));
    s.nextSlice("\x1B[4h");
    ASSERT_TRUE(s.handler.has_mode && s.handler.mode == modes::Mode::insert);

    s.nextSlice("\x1B[4l");
    ASSERT_FALSE(s.handler.has_mode);

    s.handler.has_mode = false;
    s.nextSlice("\x1B[>5h");
    ASSERT_FALSE(s.handler.has_mode);
}

struct DecrqmH {
    size_t calls;
    bool has_mode; modes::Mode mode;
    bool has_raw; Action::RawMode raw;
    DecrqmH() : calls(0), has_mode(false), mode(modes::Mode::cursor_keys), has_raw(false) {
        raw.mode = 0; raw.ansi = false;
    }
    void vt(const Action &a) {
        switch (a.tag) {
            case K::request_mode: calls += 1; has_mode = true; mode = a.mode; break;
            case K::request_mode_unknown: calls += 1; has_raw = true; raw = a.request_mode_unknown; break;
            default: break;
        }
    }
};

TEST(stream, DECRQM_dispatch) {
    struct Case {
        const char *input;
        bool has_mode; modes::Mode mode;
        bool has_raw; uint16_t raw_mode; bool raw_ansi;
    };
    const modes::Mode none = modes::Mode::cursor_keys;
    const Case cases[] = {
        { "\x1b[4$p", true, modes::Mode::insert, false, 0, false },
        { "\x1b[?4$p", true, modes::Mode::slow_scroll, false, 0, false },
        { "\x1b[9999$p", false, none, true, 9999, true },
        { "\x1b[?9999$p", false, none, true, 9999, false },
        { "\x1b[4p", false, none, false, 0, false },
        { "\x1b[?4p", false, none, false, 0, false },
        { "\x1b[4!p", false, none, false, 0, false },
        { "\x1b[4 p", false, none, false, 0, false },
        { "\x1b[>4$p", false, none, false, 0, false },
        { "\x1b[?4!p", false, none, false, 0, false },
        { "\x1b[$p", false, none, false, 0, false },
        { "\x1b[?$p", false, none, false, 0, false },
        { "\x1b[4;20$p", false, none, false, 0, false },
        { "\x1b[?4;7$p", false, none, false, 0, false },
        { "\x1b[4:20$p", false, none, false, 0, false },
    };
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        const Case &c = cases[k];
        const size_t len = strlen(c.input);
        for (size_t split = 0; split < len + 1; split++) {
            Stream<DecrqmH> s((DecrqmH()));
            s.nextSlice(c.input, split);
            if (split < len) ASSERT_TRUE(s.handler.calls == 0);
            s.nextSlice(c.input + split, len - split);
            ASSERT_TRUE(s.handler.calls == ((c.has_mode || c.has_raw) ? 1u : 0u));
            ASSERT_TRUE(s.handler.has_mode == c.has_mode);
            if (c.has_mode) ASSERT_TRUE(s.handler.mode == c.mode);
            ASSERT_TRUE(s.handler.has_raw == c.has_raw);
            if (c.has_raw) {
                ASSERT_TRUE(s.handler.raw.mode == c.raw_mode);
                ASSERT_TRUE(s.handler.raw.ansi == c.raw_ansi);
            }
        }
    }
}

struct IgnoreH {
    bool has_mode;
    IgnoreH() : has_mode(false) {}
    void vt(const Action &) {}
};

TEST(stream, ansi_set_mode_SM_and_reset_mode_RM_with_unknown_value) {
    Stream<IgnoreH> s((IgnoreH()));
    s.nextSlice("\x1B[6h");
    ASSERT_FALSE(s.handler.has_mode);

    s.nextSlice("\x1B[6l");
    ASSERT_FALSE(s.handler.has_mode);
}

struct CalledH {
    K want;
    bool called;
    explicit CalledH(K k) : want(k), called(false) {}
    void vt(const Action &a) { if (a.tag == want) called = true; }
};

TEST(stream, restore_mode) {
    Stream<CalledH> s((CalledH(K::top_and_bottom_margin)));
    feed_bytes(s, "\x1B[?42r");
    ASSERT_FALSE(s.handler.called);
}

struct PopH {
    uint16_t n;
    PopH() : n(0) {}
    void vt(const Action &a) { if (a.tag == K::kitty_keyboard_pop) n = a.value16; }
};

TEST(stream, pop_kitty_keyboard_with_no_params_defaults_to_1) {
    Stream<PopH> s((PopH()));
    feed_bytes(s, "\x1B[<u");
    ASSERT_TRUE(s.handler.n == 1);
}

struct DecscaH {
    bool has_v; ansi::ProtectedMode v;
    DecscaH() : has_v(false), v(ansi::ProtectedMode::off) {}
    void vt(const Action &a) {
        switch (a.tag) {
            case K::protected_mode_off: has_v = true; v = ansi::ProtectedMode::off; break;
            case K::protected_mode_iso: has_v = true; v = ansi::ProtectedMode::iso; break;
            case K::protected_mode_dec: has_v = true; v = ansi::ProtectedMode::dec; break;
            default: break;
        }
    }
};

TEST(stream, DECSCA) {
    Stream<DecscaH> s((DecscaH()));
    feed_bytes(s, "\x1B[\"q");
    ASSERT_TRUE(s.handler.has_v && s.handler.v == ansi::ProtectedMode::off);
    feed_bytes(s, "\x1B[0\"q");
    ASSERT_TRUE(s.handler.has_v && s.handler.v == ansi::ProtectedMode::off);
    feed_bytes(s, "\x1B[2\"q");
    ASSERT_TRUE(s.handler.has_v && s.handler.v == ansi::ProtectedMode::off);
    feed_bytes(s, "\x1B[1\"q");
    ASSERT_TRUE(s.handler.has_v && s.handler.v == ansi::ProtectedMode::dec);
}

struct EdH {
    bool has; csi::EraseDisplay mode; bool protected_;
    EdH() : has(false), mode(csi::EraseDisplay::below), protected_(false) {}
    void set(csi::EraseDisplay m, bool p) { has = true; mode = m; protected_ = p; }
    void vt(const Action &a) {
        switch (a.tag) {
            case K::erase_display_below: set(csi::EraseDisplay::below, a.flag); break;
            case K::erase_display_above: set(csi::EraseDisplay::above, a.flag); break;
            case K::erase_display_complete: set(csi::EraseDisplay::complete, a.flag); break;
            case K::erase_display_scrollback: set(csi::EraseDisplay::scrollback, a.flag); break;
            case K::erase_display_scroll_complete: set(csi::EraseDisplay::scroll_complete, a.flag); break;
            default: break;
        }
    }
};

TEST(stream, DECED_DECSED) {
    Stream<EdH> s((EdH()));
    struct { const char *in; csi::EraseDisplay mode; bool prot; } cases[] = {
        { "\x1B[?J", csi::EraseDisplay::below, true },
        { "\x1B[?0J", csi::EraseDisplay::below, true },
        { "\x1B[?1J", csi::EraseDisplay::above, true },
        { "\x1B[?2J", csi::EraseDisplay::complete, true },
        { "\x1B[?3J", csi::EraseDisplay::scrollback, true },
        { "\x1B[J", csi::EraseDisplay::below, false },
        { "\x1B[0J", csi::EraseDisplay::below, false },
        { "\x1B[1J", csi::EraseDisplay::above, false },
        { "\x1B[2J", csi::EraseDisplay::complete, false },
        { "\x1B[3J", csi::EraseDisplay::scrollback, false },
        /* Invalid and ignored by the handler */
        { "\x1B[>0J", csi::EraseDisplay::scrollback, false },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        feed_bytes(s, cases[i].in);
        ASSERT_TRUE(s.handler.has && s.handler.mode == cases[i].mode);
        ASSERT_TRUE(s.handler.protected_ == cases[i].prot);
    }
}

struct ElH {
    bool has; csi::EraseLine mode; bool protected_;
    ElH() : has(false), mode(csi::EraseLine::right), protected_(false) {}
    void set(csi::EraseLine m, bool p) { has = true; mode = m; protected_ = p; }
    void vt(const Action &a) {
        switch (a.tag) {
            case K::erase_line_right: set(csi::EraseLine::right, a.flag); break;
            case K::erase_line_left: set(csi::EraseLine::left, a.flag); break;
            case K::erase_line_complete: set(csi::EraseLine::complete, a.flag); break;
            case K::erase_line_right_unless_pending_wrap: set(csi::EraseLine::right_unless_pending_wrap, a.flag); break;
            default: break;
        }
    }
};

TEST(stream, DECEL_DECSEL) {
    Stream<ElH> s((ElH()));
    struct { const char *in; csi::EraseLine mode; bool prot; } cases[] = {
        { "\x1B[?K", csi::EraseLine::right, true },
        { "\x1B[?0K", csi::EraseLine::right, true },
        { "\x1B[?1K", csi::EraseLine::left, true },
        { "\x1B[?2K", csi::EraseLine::complete, true },
        { "\x1B[K", csi::EraseLine::right, false },
        { "\x1B[0K", csi::EraseLine::right, false },
        { "\x1B[1K", csi::EraseLine::left, false },
        { "\x1B[2K", csi::EraseLine::complete, false },
        /* Invalid and ignored by the handler */
        { "\x1B[<1K", csi::EraseLine::complete, false },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        feed_bytes(s, cases[i].in);
        ASSERT_TRUE(s.handler.has && s.handler.mode == cases[i].mode);
        ASSERT_TRUE(s.handler.protected_ == cases[i].prot);
    }
}

struct StyleH {
    bool has; ansi::CursorStyle style;
    StyleH() : has(false), style(ansi::CursorStyle::default_) {}
    void vt(const Action &a) { if (a.tag == K::cursor_style) { has = true; style = a.cursor_style; } }
};

TEST(stream, DECSCUSR) {
    Stream<StyleH> s((StyleH()));
    s.nextSlice("\x1B[ q");
    ASSERT_TRUE(s.handler.has && s.handler.style == ansi::CursorStyle::default_);

    s.nextSlice("\x1B[1 q");
    ASSERT_TRUE(s.handler.style == ansi::CursorStyle::blinking_block);

    /* Invalid and ignored by the handler */
    s.nextSlice("\x1B[?0 q");
    ASSERT_TRUE(s.handler.style == ansi::CursorStyle::blinking_block);
}

TEST(stream, DECSCUSR_without_space) {
    Stream<StyleH> s((StyleH()));
    s.nextSlice("\x1B[q");
    ASSERT_FALSE(s.handler.has);

    s.nextSlice("\x1B[1q");
    ASSERT_FALSE(s.handler.has);
}

struct ShiftH {
    bool has; bool escape;
    ShiftH() : has(false), escape(false) {}
    void vt(const Action &a) { if (a.tag == K::mouse_shift_capture) { has = true; escape = a.flag; } }
};

TEST(stream, XTSHIFTESCAPE) {
    Stream<ShiftH> s((ShiftH()));
    s.nextSlice("\x1B[>2s");
    ASSERT_FALSE(s.handler.has);

    s.nextSlice("\x1B[>s");
    ASSERT_TRUE(s.handler.has && s.handler.escape == false);

    s.nextSlice("\x1B[>0s");
    ASSERT_TRUE(s.handler.escape == false);

    s.nextSlice("\x1B[>1s");
    ASSERT_TRUE(s.handler.escape == true);

    /* Invalid and ignored by the handler */
    s.nextSlice("\x1B[1 s");
    ASSERT_TRUE(s.handler.escape == true);
}

TEST(stream, change_window_title_with_invalid_utf_8) {
    {
        Stream<CalledH> s((CalledH(K::window_title)));
        s.nextSlice("\x1b]2;abc\x1b\\");
        ASSERT_TRUE(s.handler.called);
    }

    {
        Stream<CalledH> s((CalledH(K::window_title)));
        s.nextSlice("\x1b]2;abc\xc0\x1b\\");
        ASSERT_FALSE(s.handler.called);
    }
}

struct ClipH {
    std::string data;
    uint8_t kind;
    bool has_term; osc::Terminator terminator;
    size_t count;
    ClipH() : kind(0), has_term(false), terminator(osc::Terminator::st), count(0) {}
    void vt(const Action &a) {
        if (a.tag != K::clipboard_contents) return;
        count += 1;
        kind = a.clipboard_contents.kind;
        has_term = true;
        terminator = a.clipboard_contents.terminator;
        data.append(a.clipboard_contents.data.ptr, a.clipboard_contents.data.len);
    }
};

TEST(stream, osc_52_large_payload_in_chunks) {
    /* A payload that far exceeds the fixed buffer so the allocating
     * capture is used, fed in chunk sizes that don't align with the
     * sequence so the bulk path sees every kind of boundary. */
    const std::string prefix = "\x1b]52;c;";
    const size_t payload_len = 150000;
    std::string input = prefix;
    for (size_t i = 0; i < payload_len; i++) input += (char)('A' + (i % 26));
    input += "\x1b\\";

    Stream<ClipH>::Options opts;
    opts.allocator = true;
    Stream<ClipH> s(ClipH(), opts);

    size_t i = 0;
    while (i < input.size()) {
        const size_t end = i + 4093 < input.size() ? i + 4093 : input.size();
        s.nextSlice(input.data() + i, end - i);
        i = end;
    }

    ASSERT_TRUE(s.handler.count == 1);
    ASSERT_TRUE(s.handler.kind == 'c');
    ASSERT_TRUE(s.handler.has_term && s.handler.terminator == osc::Terminator::st);
    ASSERT_TRUE(s.handler.data == input.substr(prefix.size(), payload_len));
}

/* Records every dispatch relevant to OSC processing in a
 * normalized text form so streams fed different ways can be
 * compared byte-for-byte. */
struct JournalH {
    std::string journal;
    void vt(const Action &a) {
        switch (a.tag) {
            case K::clipboard_contents:
                journal += "clip kind=";
                journal += (char)a.clipboard_contents.kind;
                journal += a.clipboard_contents.terminator == osc::Terminator::st ? " term=st" : " term=bel";
                journal += " data=";
                journal.append(a.clipboard_contents.data.ptr, a.clipboard_contents.data.len);
                journal += "\n";
                break;
            case K::kitty_clipboard:
                journal += "kitty meta=";
                journal.append(a.kitty_clipboard.metadata.ptr, a.kitty_clipboard.metadata.len);
                journal += " payload=";
                if (a.kitty_clipboard.has_payload) journal.append(a.kitty_clipboard.payload.ptr, a.kitty_clipboard.payload.len);
                else journal += "null";
                journal += "\n";
                break;
            case K::window_title:
                journal += "title ";
                journal.append(a.window_title.title, a.window_title.len);
                journal += "\n";
                break;
            /* Normalize prints to per-codepoint so the per-byte
             * and slice paths journal identically. */
            case K::print: journal += "print " + utf8(a.cp) + "\n"; break;
            case K::print_slice:
                for (size_t i = 0; i < a.print_slice.len; i++)
                    journal += "print " + utf8(a.print_slice.cps[i]) + "\n";
                break;
            default: break;
        }
    }
};

TEST(stream, osc_bulk_path_matches_per_byte_path) {
    const std::string cases[] = {
        "\x1b]52;c;aGVsbG8=\x1b\\",
        "\x1b]52;c;aGVsbG8=\x07",
        "\x1b]52;;aGVsbG8=\x07",
        /* Ignored C0 byte embedded in the payload. */
        "\x1b]52;c;aGVs\x01" "bG8=\x07",
        /* CAN and SUB aborts. */
        "\x1b]52;c;aGVsbG8=\x18",
        "\x1b]52;c;aGVsbG8=\x1a",
        /* C1 byte embedded in the payload is data. */
        "\x1b]0;ab\x9c" "cd\x07",
        /* Terminated with trailing printable text. */
        "\x1b]0;a title\x07x",
        /* Back-to-back sequences. */
        "\x1b]2;another title\x1b\\\x1b]0;t2\x07",
        "\x1b]5522;type=write;aGVsbG8=\x1b\\",
        /* Invalid OSC number. */
        "\x1b]999;junk\x07",
        /* Exceeds the fixed buffer: allocating capture. */
        "\x1b]52;c;" + std::string(3000, 'y') + "\x1b\\",
        /* Exceeds the fixed buffer: overflow, no dispatch. */
        "\x1b]0;" + std::string(3000, 'x') + "\x07",
    };

    Stream<JournalH>::Options opts;
    opts.allocator = true;

    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        const std::string &c = cases[k];

        /* Reference: byte-at-a-time. */
        Stream<JournalH> ref(JournalH(), opts);
        for (size_t i = 0; i < c.size(); i++) ref.next((uint8_t)c[i]);

        /* The whole slice at once. */
        {
            Stream<JournalH> s(JournalH(), opts);
            s.nextSlice(c.data(), c.size());
            ASSERT_TRUE(ref.handler.journal == s.handler.journal);
        }

        /* Split into two slices at every possible boundary. */
        for (size_t split = 0; split < c.size() + 1; split++) {
            Stream<JournalH> s(JournalH(), opts);
            s.nextSlice(c.data(), split);
            s.nextSlice(c.data() + split, c.size() - split);
            ASSERT_TRUE(ref.handler.journal == s.handler.journal);
        }
    }
}

TEST(stream, insert_characters) {
    Stream<CalledH> s((CalledH(K::insert_blanks)));
    feed_bytes(s, "\x1B[42@");
    ASSERT_TRUE(s.handler.called);

    s.handler.called = false;
    feed_bytes(s, "\x1B[?42@");
    ASSERT_FALSE(s.handler.called);
}

struct IchH {
    bool has; size_t value;
    IchH() : has(false), value(0) {}
    void vt(const Action &a) { if (a.tag == K::insert_blanks) { has = true; value = a.count; } }
};

TEST(stream, insert_characters_explicit_zero_clamps_to_1) {
    Stream<IchH> s((IchH()));
    feed_bytes(s, "\x1B[0@");
    ASSERT_TRUE(s.handler.has && s.handler.value == 1);
}

struct ScoscH {
    bool called; bool bad;
    ScoscH() : called(false), bad(false) {}
    void vt(const Action &a) {
        switch (a.tag) {
            case K::left_and_right_margin: bad = true; break; /* @panic("bad") */
            case K::left_and_right_margin_ambiguous: called = true; break;
            default: break;
        }
    }
};

TEST(stream, SCOSC) {
    Stream<ScoscH> s((ScoscH()));
    feed_bytes(s, "\x1B[s");
    ASSERT_FALSE(s.handler.bad);
    ASSERT_TRUE(s.handler.called);
}

TEST(stream, SCORC) {
    Stream<CalledH> s((CalledH(K::restore_cursor)));
    feed_bytes(s, "\x1B[u");
    ASSERT_TRUE(s.handler.called);
}

TEST(stream, too_many_csi_params) {
    /* .cursor_right => unreachable */
    Stream<CalledH> s((CalledH(K::cursor_right)));
    s.nextSlice("\x1B[1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1C");
    ASSERT_FALSE(s.handler.called);
}

TEST(stream, csi_param_too_long) {
    Stream<IgnoreH> s((IgnoreH()));
    const std::string in = std::string("\x1B[") + std::string(255, '1') + "C";
    s.nextSlice(in.c_str());
}

struct SizeH {
    bool has; csi::SizeReportStyle style;
    SizeH() : has(false), style(csi::SizeReportStyle::csi_14_t) {}
    void vt(const Action &a) { if (a.tag == K::size_report) { has = true; style = a.size_report; } }
};

TEST(stream, send_report_with_CSI_t) {
    Stream<SizeH> s((SizeH()));

    s.nextSlice("\x1b[14t");
    ASSERT_TRUE(s.handler.has && s.handler.style == csi::SizeReportStyle::csi_14_t);

    s.nextSlice("\x1b[16t");
    ASSERT_TRUE(s.handler.style == csi::SizeReportStyle::csi_16_t);

    s.nextSlice("\x1b[18t");
    ASSERT_TRUE(s.handler.style == csi::SizeReportStyle::csi_18_t);

    s.nextSlice("\x1b[21t");
    ASSERT_TRUE(s.handler.style == csi::SizeReportStyle::csi_21_t);
}

TEST(stream, invalid_CSI_t) {
    Stream<SizeH> s((SizeH()));
    s.nextSlice("\x1b[19t");
    ASSERT_FALSE(s.handler.has);
}

struct TitleH {
    K want;
    bool has; uint16_t index;
    explicit TitleH(K k) : want(k), has(false), index(0) {}
    void vt(const Action &a) { if (a.tag == want) { has = true; index = a.value16; } }
};

TEST(stream, CSI_t_push_title) {
    Stream<TitleH> s((TitleH(K::title_push)));
    s.nextSlice("\x1b[22;0t");
    ASSERT_TRUE(s.handler.has && s.handler.index == 0);
}

TEST(stream, CSI_t_push_title_with_explicit_window) {
    Stream<TitleH> s((TitleH(K::title_push)));
    s.nextSlice("\x1b[22;2t");
    ASSERT_TRUE(s.handler.has && s.handler.index == 0);
}

TEST(stream, CSI_t_push_title_with_explicit_icon) {
    Stream<TitleH> s((TitleH(K::title_push)));
    s.nextSlice("\x1b[22;1t");
    ASSERT_FALSE(s.handler.has);
}

TEST(stream, CSI_t_push_title_with_index) {
    Stream<TitleH> s((TitleH(K::title_push)));
    s.nextSlice("\x1b[22;0;5t");
    ASSERT_TRUE(s.handler.has && s.handler.index == 5);
}

TEST(stream, CSI_t_pop_title) {
    Stream<TitleH> s((TitleH(K::title_pop)));
    s.nextSlice("\x1b[23;0t");
    ASSERT_TRUE(s.handler.has && s.handler.index == 0);
}

TEST(stream, CSI_t_pop_title_with_explicit_window) {
    Stream<TitleH> s((TitleH(K::title_pop)));
    s.nextSlice("\x1b[23;2t");
    ASSERT_TRUE(s.handler.has && s.handler.index == 0);
}

TEST(stream, CSI_t_pop_title_with_explicit_icon) {
    Stream<TitleH> s((TitleH(K::title_pop)));
    s.nextSlice("\x1b[23;1t");
    ASSERT_FALSE(s.handler.has);
}

TEST(stream, CSI_t_pop_title_with_index) {
    Stream<TitleH> s((TitleH(K::title_pop)));
    s.nextSlice("\x1b[23;0;5t");
    ASSERT_TRUE(s.handler.has && s.handler.index == 5);
}

struct LastH {
    bool has; K action;
    LastH() : has(false), action(K::bell) {}
    void vt(const Action &a) { has = true; action = a.tag; }
};

TEST(stream, CSI_W_clear_tab_stops) {
    Stream<LastH> s((LastH()));

    s.nextSlice("\x1b[2W");
    ASSERT_TRUE(s.handler.has && s.handler.action == K::tab_clear_current);

    s.nextSlice("\x1b[5W");
    ASSERT_TRUE(s.handler.action == K::tab_clear_all);
}

TEST(stream, CSI_W_tab_set) {
    Stream<LastH> s((LastH()));

    s.nextSlice("\x1b[W");
    ASSERT_TRUE(s.handler.has && s.handler.action == K::tab_set);

    s.handler.has = false;
    s.nextSlice("\x1b[0W");
    ASSERT_TRUE(s.handler.has && s.handler.action == K::tab_set);

    s.handler.has = false;
    s.nextSlice("\x1b[>W");
    ASSERT_FALSE(s.handler.has);

    s.handler.has = false;
    s.nextSlice("\x1b[99W");
    ASSERT_FALSE(s.handler.has);
}

TEST(stream, CSI_question_W_reset_tab_stops) {
    Stream<LastH> s((LastH()));

    s.nextSlice("\x1b[?2W");
    ASSERT_FALSE(s.handler.has);

    s.nextSlice("\x1b[?5W");
    ASSERT_TRUE(s.handler.has && s.handler.action == K::tab_reset);

    /* Invalid and ignored by the handler */
    s.handler.has = false;
    s.nextSlice("\x1b[?1;2;3W");
    ASSERT_FALSE(s.handler.has);
}

TEST(stream, SGR_with_17_plus_parameters_for_underline_color) {
    Stream<CalledH> s((CalledH(K::set_attribute)));

    /* Kakoune-style SGR with underline color as 17th parameter
     * This tests the fix where param 17 was being dropped */
    s.nextSlice("\x1b[4:3;38;2;51;51;51;48;2;170;170;170;58;2;255;97;136;0m");
    ASSERT_TRUE(s.handler.called);
}

TEST(stream, tab_clear_with_overflowing_param) {
    /* Regression test for a fuzz crash: CSI with a parameter value that
     * saturates to 65535 (u16 max) causes @enumFromInt to panic when
     * converting to TabClear (enum(u8)). */
    Stream<CalledH> s((CalledH(K::tab_clear_current)));
    /* This is the exact input from the fuzz crash (minus the mode byte):
     * CSI with a huge numeric param that saturates to 65535, followed by 'g'. */
    s.nextSlice("\x1b[388888888888888888888888888888888888g\x1b[0m");
    ASSERT_FALSE(s.handler.called);
}

/* ─── APC ───────────────────────────────────────────────────────────────── */

/* A test handler that accumulates APC bytes regardless of whether they
 * arrive per-byte (apc_put) or in bulk (apc_put_slice). */
struct ApcTestHandler {
    uint8_t buf[256];
    size_t len, slices, puts, started, ended;
    ApcTestHandler() : len(0), slices(0), puts(0), started(0), ended(0) {}
    void vt(const Action &a) {
        switch (a.tag) {
            case K::apc_start: started += 1; break;
            case K::apc_end: ended += 1; break;
            case K::apc_put: buf[len++] = a.byte; puts += 1; break;
            case K::apc_put_slice:
                memcpy(buf + len, a.apc_put_slice.bytes, a.apc_put_slice.len);
                len += a.apc_put_slice.len;
                slices += 1;
                break;
            default: break;
        }
    }
    std::string str() const { return std::string((const char *)buf, len); }
};

TEST(stream, apc_bulk_slice) {
    Stream<ApcTestHandler> s((ApcTestHandler()));
    s.nextSlice("\x1b_Gf=24,s=10,v=20;aGVsbG8=\x1b\\");

    ASSERT_TRUE(s.handler.started == 1);
    ASSERT_TRUE(s.handler.ended == 1);
    ASSERT_TRUE(s.handler.str() == "Gf=24,s=10,v=20;aGVsbG8=");

    /* With SIMD enabled the body must arrive as a single slice.
     * Wisp: build_options.simd is off, so upstream skips this. */
}

TEST(stream, apc_bulk_slice_C1_ST) {
    Stream<ApcTestHandler> s((ApcTestHandler()));
    s.nextSlice("\x1b_Gpayload\x9c");

    ASSERT_TRUE(s.handler.started == 1);
    ASSERT_TRUE(s.handler.ended == 1);
    ASSERT_TRUE(s.handler.str() == "Gpayload");
}

TEST(stream, apc_bulk_slice_split_across_inputs) {
    Stream<ApcTestHandler> s((ApcTestHandler()));
    s.nextSlice("\x1b_Gf=24,s=10");
    s.nextSlice(",v=20;aGVs");
    s.nextSlice("bG8=\x1b\\");

    ASSERT_TRUE(s.handler.started == 1);
    ASSERT_TRUE(s.handler.ended == 1);
    ASSERT_TRUE(s.handler.str() == "Gf=24,s=10,v=20;aGVsbG8=");
}

TEST(stream, apc_bulk_slice_keeps_C0_bytes_as_data) {
    Stream<ApcTestHandler> s((ApcTestHandler()));
    /* BEL does not terminate an APC string; it is payload data. */
    s.nextSlice("\x1b_Gx\x07y\x1b\\");

    ASSERT_TRUE(s.handler.ended == 1);
    ASSERT_TRUE(s.handler.str() == "Gx\x07y");
}

TEST(stream, apc_aborted_by_CAN) {
    Stream<ApcTestHandler> s((ApcTestHandler()));
    /* CAN (0x18) aborts the APC string via the anywhere => ground
     * transition. Exiting the sos_pm_apc_string state emits apc_end,
     * and the trailing bytes are printed, not treated as APC data. */
    s.nextSlice("\x1b_Gabcdefghijklmnopqrstuvwxyz0123456789\x18" "def");

    ASSERT_TRUE(s.handler.started == 1);
    ASSERT_TRUE(s.handler.ended == 1);
    ASSERT_TRUE(s.handler.str() == "Gabcdefghijklmnopqrstuvwxyz0123456789");
}

TEST(stream, apc_scalar_path_matches) {
    Stream<ApcTestHandler> s((ApcTestHandler()));
    feed_bytes(s, "\x1b_Gf=24;aGVsbG8=\x1b\\");

    ASSERT_TRUE(s.handler.started == 1);
    ASSERT_TRUE(s.handler.ended == 1);
    ASSERT_TRUE(s.handler.str() == "Gf=24;aGVsbG8=");
}

TEST(stream, apc_vector_boundaries_match_scalar_path) {
    const size_t positions[] = { 15, 16, 17, 31, 32, 33, 63, 64, 65 };
    const uint8_t controls[] = { 0x18, 0x1A, 0x1B, 0x80, 0xFF };

    for (size_t pi = 0; pi < 9; pi++) for (size_t ci = 0; ci < 5; ci++) {
        const size_t position = positions[pi];
        uint8_t input[96];
        memcpy(input, "\x1b_G", 3);
        memset(input + 3, 'a', position);
        input[3 + position] = controls[ci];
        input[4 + position] = '\\';
        const size_t n = 5 + position;

        Stream<ApcTestHandler> bulk((ApcTestHandler()));
        bulk.nextSlice(input, n);
        Stream<ApcTestHandler> scalar((ApcTestHandler()));
        for (size_t i = 0; i < n; i++) scalar.next(input[i]);

        ASSERT_TRUE(scalar.handler.started == bulk.handler.started);
        ASSERT_TRUE(scalar.handler.ended == bulk.handler.ended);
        ASSERT_TRUE(scalar.handler.str() == bulk.handler.str());
    }
}

/* ─── continuation ─────────────────────────────────────────────────────────
 *
 * Wisp: `?usize` from nextSliceUntilGround is the bool return plus
 * *consumed, and writeContinuation returns a ContinuationError rather than
 * an error union. A std::string writer cannot fail, so upstream's
 * error.WriteFailed cases (which use a too-small fixed buffer) have no
 * counterpart and are noted where they occur.
 */

static wisp::zigstd::Allocator contAlloc() { return wisp::zigstd::testing_allocator(); }

struct ContinuationTestHandler {
    size_t committed;
    bool apc_active;
    uint8_t apc_buf[256];
    size_t apc_len;
    bool dcs_active;

    ContinuationTestHandler()
        : committed(0), apc_active(false), apc_len(0), dcs_active(false) {}

    void deinit() {}

    void vt(const Action &a) {
        switch (a.tag) {
        case K::apc_start: apc_active = true; break;
        case K::apc_put:
            apc_buf[apc_len] = a.byte;
            apc_len += 1;
            break;
        case K::apc_put_slice:
            memcpy(apc_buf + apc_len, a.apc_put_slice.bytes, a.apc_put_slice.len);
            apc_len += a.apc_put_slice.len;
            break;
        case K::dcs_hook: dcs_active = true; break;
        case K::dcs_put: break;
        case K::apc_end:
            apc_active = false;
            apc_len = 0;
            committed += 1;
            break;
        case K::dcs_unhook:
            dcs_active = false;
            committed += 1;
            break;
        case K::print: committed += 1; break;
        case K::print_slice: committed += a.print_slice.len; break;
        case K::print_repeat: committed += a.count; break;
        default: committed += 1; break;
        }
    }

    std::string apcStr() const { return std::string((const char *)apc_buf, apc_len); }
};

struct ContinuationNullHandler {
    void deinit() {}
    void vt(const Action &) {}
};

typedef Stream<ContinuationTestHandler> ContStream;
typedef Stream<ContinuationNullHandler> NullStream;

/* Wisp: `.{ .allocator = alloc, .continuation_max_bytes = n }` */
static ContStream::Options contOptions(size_t max_bytes) {
    ContStream::Options o;
    o.allocator = true;
    o.continuation_max_bytes = wisp::terminal::stream_continuation::Maybe<size_t>(max_bytes);
    return o;
}

TEST(stream, nextSliceUntilGround_stops_at_the_earliest_boundary) {
    ContStream s((ContinuationTestHandler()));

    s.nextSlice("\x1b[31");
    ASSERT_TRUE(!s.ground());
    s.handler.committed = 0;

    static const char input[] = "mABC\x1b[";
    size_t consumed = 0;
    ASSERT_TRUE(s.nextSliceUntilGround((const uint8_t *)input, sizeof input - 1, &consumed));
    ASSERT_TRUE(1 == consumed);
    ASSERT_TRUE(s.ground());
    ASSERT_TRUE(1 == s.handler.committed);

    /* The suffix was not inspected by the handler and can be processed after
     * the caller performs work at the boundary. */
    s.nextSlice(input + consumed, sizeof input - 1 - consumed);
    ASSERT_TRUE(!s.ground());
    ASSERT_TRUE(wisp::terminal::parser::State::csi_entry == s.parser.state);
    ASSERT_TRUE(4 == s.handler.committed);

    s.deinit();
}

TEST(stream, nextSliceUntilGround_consumes_all_input_without_a_boundary) {
    NullStream s((ContinuationNullHandler()));

    size_t consumed = 99;
    ASSERT_TRUE(s.nextSliceUntilGround((const uint8_t *)"unprocessed", 11, &consumed));
    ASSERT_TRUE(0 == consumed);
    ASSERT_TRUE(s.ground());

    s.nextSlice("\x1b[");
    ASSERT_TRUE(!s.nextSliceUntilGround((const uint8_t *)"123", 3, &consumed));
    ASSERT_TRUE(!s.ground());

    /* A boundary on the final byte is distinguishable from exhausting the
     * input while still pending. */
    ASSERT_TRUE(s.nextSliceUntilGround((const uint8_t *)"m", 1, &consumed));
    ASSERT_TRUE(1 == consumed);
    ASSERT_TRUE(s.ground());

    s.deinit();
}

TEST(stream, nextSliceUntilGround_handles_UTF_8_boundaries) {
    /* A completed codepoint is committed synchronously, and the printable
     * suffix remains untouched. */
    {
        ContStream valid((ContinuationTestHandler()));
        static const uint8_t lead[] = {0xF0};
        valid.nextSlice(lead, 1);
        static const uint8_t valid_input[] = {0x9F, 0x98, 0x84, 'X'};
        size_t consumed = 0;
        ASSERT_TRUE(valid.nextSliceUntilGround(valid_input, 4, &consumed));
        ASSERT_TRUE(3 == consumed);
        ASSERT_TRUE(valid.ground());
        ASSERT_TRUE(1 == valid.handler.committed);
        valid.deinit();
    }

    /* A malformed continuation emits the replacement codepoint and retries
     * the same byte. Ground is observed after that complete byte operation. */
    {
        ContStream malformed((ContinuationTestHandler()));
        static const uint8_t bad[] = {0xE0, 0xA0};
        malformed.nextSlice(bad, 2);
        size_t consumed = 0;
        ASSERT_TRUE(malformed.nextSliceUntilGround((const uint8_t *)"A!", 2, &consumed));
        ASSERT_TRUE(1 == consumed);
        ASSERT_TRUE(malformed.ground());
        ASSERT_TRUE(2 == malformed.handler.committed);
        malformed.deinit();
    }

    /* If the retried byte is ESC, the stream has begun VT state at the end of
     * that byte and must continue to the following ground boundary. */
    {
        ContStream retry_escape((ContinuationTestHandler()));
        static const uint8_t bad[] = {0xE0, 0xA0};
        retry_escape.nextSlice(bad, 2);
        size_t consumed = 0;
        ASSERT_TRUE(retry_escape.nextSliceUntilGround((const uint8_t *)"\x1b[mX", 4, &consumed));
        ASSERT_TRUE(3 == consumed);
        ASSERT_TRUE(retry_escape.ground());
        retry_escape.deinit();
    }
}

TEST(stream, nextSliceUntilGround_handles_aborts_and_bulk_strings) {
    {
        ContStream aborted((ContinuationTestHandler()));
        aborted.nextSlice("\x1b[123");
        static const uint8_t can_x[] = {0x18, 'X'};
        size_t consumed = 0;
        ASSERT_TRUE(aborted.nextSliceUntilGround(can_x, 2, &consumed));
        ASSERT_TRUE(1 == consumed);
        ASSERT_TRUE(aborted.ground());
        aborted.deinit();
    }

    {
        ContStream apc((ContinuationTestHandler()));
        apc.nextSlice("\x1b_Gseed");
        uint8_t input[131];
        memset(input, 'a', 128);
        input[128] = 0x1B;
        input[129] = '\\';
        input[130] = 'X';
        size_t consumed = 0;
        ASSERT_TRUE(apc.nextSliceUntilGround(input, 131, &consumed));
        ASSERT_TRUE(130 == consumed);
        ASSERT_TRUE(apc.ground());
        apc.deinit();
    }
}

TEST(stream, nextSliceUntilGround_tracks_only_the_consumed_prefix) {
    NullStream::Options o;
    o.allocator = true;
    o.continuation_max_bytes = wisp::terminal::stream_continuation::Maybe<size_t>((size_t)64);
    NullStream s(ContinuationNullHandler(), o);

    s.nextSlice("\x1b[31");
    size_t consumed = 0;
    ASSERT_TRUE(s.nextSliceUntilGround((const uint8_t *)"mX\x1b[", 4, &consumed));
    ASSERT_TRUE(1 == consumed);

    {
        std::string out;
        ASSERT_TRUE(NullStream::ContinuationError::none == s.writeContinuation(&out));
        ASSERT_TRUE(0 == out.size());
    }

    s.nextSlice("\x1b[");
    ASSERT_TRUE(!s.nextSliceUntilGround((const uint8_t *)"123", 3, &consumed));
    {
        std::string out;
        ASSERT_TRUE(NullStream::ContinuationError::none == s.writeContinuation(&out));
        ASSERT_TRUE(out == std::string("\x1b[123"));
    }

    s.deinit();
}

TEST(stream, continuation_lifecycle) {
    {
        ContStream disabled((ContinuationTestHandler()));
        std::string out;
        ASSERT_TRUE(ContStream::ContinuationError::ContinuationDisabled ==
                    disabled.writeContinuation(&out));
        disabled.deinit();
    }

    {
        ContStream::Options o;
        o.allocator = true;
        o.continuation_max_bytes = wisp::terminal::stream_continuation::Maybe<size_t>((size_t)0);
        ContStream zero_capacity(ContinuationTestHandler(), o);
        std::string out;
        ASSERT_TRUE(ContStream::ContinuationError::ContinuationDisabled ==
                    zero_capacity.writeContinuation(&out));
        zero_capacity.deinit();
    }

    {
        ContStream::Options o;
        o.allocator = false;
        o.continuation_max_bytes = wisp::terminal::stream_continuation::Maybe<size_t>((size_t)64);
        ContStream no_allocator(ContinuationTestHandler(), o);
        std::string out;
        ASSERT_TRUE(ContStream::ContinuationError::ContinuationDisabled ==
                    no_allocator.writeContinuation(&out));
        no_allocator.deinit();
    }

    {
        ContStream tracked(ContinuationTestHandler(), contOptions(64));

        tracked.nextSlice("complete input");
        {
            std::string out;
            ASSERT_TRUE(ContStream::ContinuationError::none == tracked.writeContinuation(&out));
            ASSERT_TRUE(0 == out.size());
        }

        /* Wisp: upstream also asserts error.WriteFailed here from a 1-byte
         * fixed writer. A std::string writer cannot fail, so instead the
         * suffix is simply written in full. */
        tracked.nextSlice("\x1b[");
        {
            std::string out;
            ASSERT_TRUE(ContStream::ContinuationError::none == tracked.writeContinuation(&out));
            ASSERT_TRUE(out == std::string("\x1b["));
        }
        tracked.deinit();
    }

    {
        wisp::zigstd::FailingAllocator failing(contAlloc(), 0);
        NullStream::Options o;
        o.allocator = true;
        o.continuation_max_bytes = wisp::terminal::stream_continuation::Maybe<size_t>((size_t)64);
        NullStream failing_stream(ContinuationNullHandler(), o);
        /* Wisp: the stream's allocator is a flag, so the tracker is rebuilt
         * on the failing allocator to reproduce upstream's init, where the
         * best-effort initial reservation itself fails. */
        failing_stream.continuation.deinit();
        failing_stream.continuation =
            wisp::terminal::stream_continuation::Tracker::init(failing.allocator(), 64);
        failing_stream.nextSlice("\x1b[");
        std::string out;
        ASSERT_TRUE(NullStream::ContinuationError::ContinuationUnavailable ==
                    failing_stream.writeContinuation(&out));
        failing_stream.deinit();
    }
}

TEST(stream, continuation_suffixes_are_replay_safe) {
    struct Case {
        const char *input;
        size_t input_len;
        const char *expected;
        size_t expected_len;
    };
    static const Case cases[] = {
        {"text\x1b", 5, "\x1b", 1},
        {"text\x1b[12;", 9, "\x1b[12;", 5},
        {"text\x1b[1\x07;2", 10, "\x1b[1;2", 5},
        {"text\x1b]2;hello", 13, "\x1b]2;hello", 9},
        {"text\x1b_Gabc", 9, "\x1b_Gabc", 5},
        {"text\x1bP+qabc", 10, "\x1bP+qabc", 6},
        {"text\xE0\xA0\xF0", 7, "\xF0", 1},
        {"text\x1b[12\x1b", 9, "\x1b", 1},
        {"text\x1b[12\x9D""2;title", 14, "\x1b[12\x9D""2;title", 10},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ContStream s(ContinuationTestHandler(), contOptions(1024));
        s.nextSlice(cases[i].input, cases[i].input_len);

        std::string out;
        ASSERT_TRUE(ContStream::ContinuationError::none == s.writeContinuation(&out));
        ASSERT_TRUE(out == std::string(cases[i].expected, cases[i].expected_len));
        s.deinit();
    }
}

TEST(stream, continuation_reconstructs_every_unfinished_VT_state) {
    struct Case {
        const char *input;
        wisp::terminal::parser::State state;
    };
    static const Case cases[] = {
        {"\x1b", wisp::terminal::parser::State::escape},
        {"\x1b(", wisp::terminal::parser::State::escape_intermediate},
        {"\x1b[", wisp::terminal::parser::State::csi_entry},
        {"\x1b[1", wisp::terminal::parser::State::csi_param},
        {"\x1b[1$", wisp::terminal::parser::State::csi_intermediate},
        {"\x1b[:", wisp::terminal::parser::State::csi_ignore},
        {"\x1bP", wisp::terminal::parser::State::dcs_entry},
        {"\x1bP1", wisp::terminal::parser::State::dcs_param},
        {"\x1bP1$", wisp::terminal::parser::State::dcs_intermediate},
        {"\x1bP1q", wisp::terminal::parser::State::dcs_passthrough},
        {"\x1bP:", wisp::terminal::parser::State::dcs_ignore},
        {"\x1b]2;title", wisp::terminal::parser::State::osc_string},
        {"\x1b_Gpayload", wisp::terminal::parser::State::sos_pm_apc_string},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ContStream source(ContinuationTestHandler(), contOptions(1024));
        source.nextSlice(cases[i].input);
        ASSERT_TRUE(cases[i].state == source.parser.state);

        std::string out;
        ASSERT_TRUE(ContStream::ContinuationError::none == source.writeContinuation(&out));

        ContStream restored(ContinuationTestHandler(), contOptions(1024));
        restored.nextSlice(out.data(), out.size());
        ASSERT_TRUE(0 == restored.handler.committed);
        ASSERT_TRUE(source.parser.state == restored.parser.state);
        ASSERT_TRUE(source.utf8decoder.state == restored.utf8decoder.state);

        restored.deinit();
        source.deinit();
    }

    /* Parser ground is still unfinished while the UTF-8 decoder is waiting
     * for the remaining bytes of a codepoint. */
    {
        ContStream utf8(ContinuationTestHandler(), contOptions(4));
        utf8.next(0xF0);
        ASSERT_TRUE(wisp::terminal::parser::State::ground == utf8.parser.state);
        ASSERT_TRUE(utf8.utf8decoder.state != 0);
        std::string out;
        ASSERT_TRUE(ContStream::ContinuationError::none == utf8.writeContinuation(&out));
        ASSERT_TRUE(out == std::string("\xF0", 1));
        utf8.deinit();
    }
}

TEST(stream, continuation_is_chunking_independent_and_idempotent) {
    static const char input[] = "committed\x1b[1\x07;2";
    const size_t input_len = sizeof input - 1;

    ContStream bulk(ContinuationTestHandler(), contOptions(1024));
    bulk.nextSlice(input, input_len);

    ContStream scalar(ContinuationTestHandler(), contOptions(1024));
    for (size_t i = 0; i < input_len; i++) scalar.next((uint8_t)input[i]);

    std::string bulk_out;
    ASSERT_TRUE(ContStream::ContinuationError::none == bulk.writeContinuation(&bulk_out));
    std::string scalar_out;
    ASSERT_TRUE(ContStream::ContinuationError::none == scalar.writeContinuation(&scalar_out));
    ASSERT_TRUE(bulk_out == scalar_out);

    ContStream restored(ContinuationTestHandler(), contOptions(1024));
    restored.nextSlice(bulk_out.data(), bulk_out.size());
    ASSERT_TRUE(0 == restored.handler.committed);

    std::string restored_out;
    ASSERT_TRUE(ContStream::ContinuationError::none == restored.writeContinuation(&restored_out));
    ASSERT_TRUE(bulk_out == restored_out);

    bulk.handler.committed = 0;
    restored.handler.committed = 0;
    bulk.nextSlice("mZ");
    restored.next('m');
    restored.next('Z');
    ASSERT_TRUE(bulk.handler.committed == restored.handler.committed);

    restored.deinit();
    scalar.deinit();
    bulk.deinit();
}

TEST(stream, continuation_rebuilds_APC_handler_input) {
    ContStream source(ContinuationTestHandler(), contOptions(1024));
    source.nextSlice("committed\x1b_Gabc");

    std::string out;
    ASSERT_TRUE(ContStream::ContinuationError::none == source.writeContinuation(&out));

    ContStream restored(ContinuationTestHandler(), contOptions(1024));
    restored.nextSlice(out.data(), out.size());
    ASSERT_TRUE(0 == restored.handler.committed);
    ASSERT_TRUE(restored.handler.apc_active);
    ASSERT_TRUE(source.handler.apcStr() == restored.handler.apcStr());

    source.handler.committed = 0;
    restored.handler.committed = 0;
    source.nextSlice("\x1b\\");
    restored.nextSlice("\x1b\\");
    ASSERT_TRUE(source.handler.committed == restored.handler.committed);
    ASSERT_TRUE(!source.handler.apc_active);
    ASSERT_TRUE(!restored.handler.apc_active);

    restored.deinit();
    source.deinit();
}

TEST(stream, continuation_cap_and_recovery) {
    /* The raw feed exceeds the cap, but only the unfinished three-byte
     * CSI suffix is retained. */
    {
        ContStream seeded(ContinuationTestHandler(), contOptions(4));
        seeded.nextSlice("committed text\x1b[1");
        std::string out;
        ASSERT_TRUE(ContStream::ContinuationError::none == seeded.writeContinuation(&out));
        ASSERT_TRUE(out == std::string("\x1b[1"));
        seeded.deinit();
    }

    ContStream exceeded(ContinuationTestHandler(), contOptions(4));
    exceeded.nextSlice("\x1b[123");
    {
        std::string out;
        ASSERT_TRUE(ContStream::ContinuationError::ContinuationUnavailable ==
                    exceeded.writeContinuation(&out));
    }

    /* Completing the CSI reaches ground and recovers without rebuilding the
     * Stream. A later unfinished sequence is tracked normally. */
    exceeded.nextSlice("mtext\x1b[");
    {
        std::string out;
        ASSERT_TRUE(ContStream::ContinuationError::none == exceeded.writeContinuation(&out));
        ASSERT_TRUE(out == std::string("\x1b["));
    }

    /* A fresh ESC seed also recovers broken tracking even when the stream
     * never reaches ground: the ESC abandons the previous unfinished
     * state and everything after it is retained. */
    exceeded.nextSlice("\x1b[123");
    {
        std::string out;
        ASSERT_TRUE(ContStream::ContinuationError::ContinuationUnavailable ==
                    exceeded.writeContinuation(&out));
    }
    exceeded.nextSlice("\x1b]0;");
    {
        std::string out;
        ASSERT_TRUE(ContStream::ContinuationError::none == exceeded.writeContinuation(&out));
        ASSERT_TRUE(out == std::string("\x1b]0;"));
    }

    exceeded.deinit();
}

TEST(stream, continuation_spans_multiple_bulk_feeds) {
    /* An unfinished APC grows across feeds that contain no new seed. */
    {
        ContStream apc(ContinuationTestHandler(), contOptions(1024));
        apc.nextSlice("text\x1b_Gab");
        apc.nextSlice("cd");
        apc.nextSlice("ef");
        std::string out;
        ASSERT_TRUE(ContStream::ContinuationError::none == apc.writeContinuation(&out));
        ASSERT_TRUE(out == std::string("\x1b_Gabcdef"));
        apc.deinit();
    }

    /* An incomplete UTF-8 sequence grows across feeds of its
     * continuation bytes. */
    ContStream utf8(ContinuationTestHandler(), contOptions(1024));
    utf8.nextSlice("text\xF0");
    utf8.nextSlice("\x9F");
    {
        std::string out;
        ASSERT_TRUE(ContStream::ContinuationError::none == utf8.writeContinuation(&out));
        ASSERT_TRUE(out == std::string("\xF0\x9F", 2));
    }

    /* A later feed with its own seed drops everything retained earlier. */
    utf8.nextSlice("\x98\x84 done \x1b[38;5");
    {
        std::string out;
        ASSERT_TRUE(ContStream::ContinuationError::none == utf8.writeContinuation(&out));
        ASSERT_TRUE(out == std::string("\x1b[38;5"));
    }

    utf8.deinit();
}

TEST(stream, continuation_exact_cap_and_large_unfinished_string) {
    {
        NullStream::Options o;
        o.allocator = true;
        o.continuation_max_bytes = wisp::terminal::stream_continuation::Maybe<size_t>((size_t)5);
        NullStream exact(ContinuationNullHandler(), o);
        exact.nextSlice("\x1b[123");
        std::string out;
        ASSERT_TRUE(NullStream::ContinuationError::none == exact.writeContinuation(&out));
        ASSERT_TRUE(out == std::string("\x1b[123"));
        exact.deinit();
    }

    const size_t payload_len = 12 * 1024;
    std::string input;
    input.append("\x1b_G", 3);
    input.append(payload_len - 3, 'A');

    NullStream::Options o;
    o.allocator = true;
    o.continuation_max_bytes = wisp::terminal::stream_continuation::Maybe<size_t>(payload_len);
    NullStream large(ContinuationNullHandler(), o);
    large.nextSlice(input.data(), input.size());
    std::string out;
    ASSERT_TRUE(NullStream::ContinuationError::none == large.writeContinuation(&out));
    ASSERT_TRUE(out == input);
    large.deinit();
}

TEST(stream, continuation_allocation_failure_recovers) {
    wisp::zigstd::FailingAllocator failing(contAlloc(), (size_t)-1);
    NullStream::Options o;
    o.allocator = true;
    o.continuation_max_bytes =
        wisp::terminal::stream_continuation::Maybe<size_t>((size_t)(16 * 1024));
    NullStream s(ContinuationNullHandler(), o);
    s.continuation.alloc = failing.allocator();

    std::string input;
    input.append("\x1b[", 2);
    input.append(12 * 1024 - 2, '1');

    failing.fail_index = failing.alloc_index;
    s.nextSlice(input.data(), input.size());
    {
        std::string out;
        ASSERT_TRUE(NullStream::ContinuationError::ContinuationUnavailable ==
                    s.writeContinuation(&out));
    }

    failing.fail_index = (size_t)-1;
    s.next('m');
    {
        std::string out;
        ASSERT_TRUE(NullStream::ContinuationError::none == s.writeContinuation(&out));
        ASSERT_TRUE(0 == out.size());
    }

    s.deinit();
}

TEST(stream, continuation_every_byte_cuts_preserve_future_behavior) {
    static const char *corpora[] = {
        "plain \xF0\x9F\x98\x84 utf8",
        "bad \xE0\xA0\xF0\x9F\x98\x84 utf8",
        "\x1b[1\x07;2mstyled\x1b[0m",
        "\x1b]2;window title\x1b\\text",
        "\x1bP$qm\x1b\\text",
        "\x1b_Ga=q;payload\x1b\\text",
        "\x1b_25a1;s\x1b\\text",
        "\x1b]2;first\x1b\\\x1b_Gsecond",
        "\x1b[12\x9D""2;title\x1b\\text",
        "\x1b[12\x18text\x1b[1\x1Atext",
    };

    for (size_t ci = 0; ci < sizeof corpora / sizeof corpora[0]; ci++) {
        const std::string corpus(corpora[ci]);
        for (size_t cut = 0; cut <= corpus.size(); cut++) {
            ContStream source(ContinuationTestHandler(), contOptions(64 * 1024));
            source.nextSlice(corpus.data(), cut);

            std::string continuation;
            ASSERT_TRUE(ContStream::ContinuationError::none ==
                        source.writeContinuation(&continuation));

            ContStream restored(ContinuationTestHandler(), contOptions(64 * 1024));
            restored.nextSlice(continuation.data(), continuation.size());
            ASSERT_TRUE(0 == restored.handler.committed);
            ASSERT_TRUE(source.handler.apc_active == restored.handler.apc_active);
            ASSERT_TRUE(source.handler.dcs_active == restored.handler.dcs_active);
            if (source.handler.apc_active) {
                ASSERT_TRUE(source.handler.apcStr() == restored.handler.apcStr());
            }

            std::string reexport;
            ASSERT_TRUE(ContStream::ContinuationError::none ==
                        restored.writeContinuation(&reexport));
            ASSERT_TRUE(continuation == reexport);

            source.handler.committed = 0;
            restored.handler.committed = 0;
            source.nextSlice(corpus.data() + cut, corpus.size() - cut);
            size_t offset = cut;
            size_t partition = cut + corpus.size() + 1;
            while (offset < corpus.size()) {
                partition = partition * 1664525u + 1013904223u;
                const size_t room = corpus.size() - offset;
                const size_t len = (1 + partition % 7) < room ? (1 + partition % 7) : room;
                restored.nextSlice(corpus.data() + offset, len);
                offset += len;
            }
            ASSERT_TRUE(source.handler.committed == restored.handler.committed);
            ASSERT_TRUE(source.handler.apc_active == restored.handler.apc_active);
            ASSERT_TRUE(source.handler.dcs_active == restored.handler.dcs_active);

            std::string source_final;
            ASSERT_TRUE(ContStream::ContinuationError::none ==
                        source.writeContinuation(&source_final));
            std::string restored_final;
            ASSERT_TRUE(ContStream::ContinuationError::none ==
                        restored.writeContinuation(&restored_final));
            ASSERT_TRUE(source_final == restored_final);

            restored.deinit();
            source.deinit();
        }
    }
}
