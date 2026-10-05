/* Transliterated from the test blocks in Ghostty src/terminal/stream_terminal.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Wisp: the tests gated on build_options.kitty_graphics, glyph_protocol or
 * tmux_control_mode are not ported, matching the build configuration of
 * stream_terminal.hpp.
 */

#include <stdio.h>
#include <string.h>

#include <string>

#include "test_helpers.h"
#include "../terminal/stream_terminal.hpp"
#include "../vt/formatter.hpp"

using namespace wisp;

namespace st = wisp::terminal::stream_terminal;
namespace osc = wisp::terminal::osc;
namespace modes = wisp::terminal::modes;

typedef st::Handler Handler;
typedef wisp::terminal::stream::Stream<Handler> Stream;

static zigstd::Allocator talloc() { return zigstd::testing_allocator(); }

/* Wisp: `var t: Terminal = try .init(...); defer t.deinit(alloc);` */
struct TermHolder {
    vt::Terminal t;
    bool ok;
    TermHolder(unsigned cols, unsigned rows) {
        vt::Terminal::Options o((vt::size::CellCountInt)cols, (vt::size::CellCountInt)rows);
        ok = vt::Terminal::init(talloc(), o, &t);
    }
    ~TermHolder() {
        if (ok) t.deinit(talloc());
    }
};

/* Wisp: `var s: Stream = .init(.{ .allocator = alloc, .handler = handler });
 * defer s.deinit();` The stream is not copyable, so it is constructed in
 * place around the handler. */
struct StreamHolder {
    Stream s;
    explicit StreamHolder(const Handler &h) : s(h, streamOptions()) {}
    ~StreamHolder() { s.deinit(); }

    static Stream::Options streamOptions() {
        Stream::Options o;
        o.allocator = true;
        return o;
    }
};

/* Wisp: ASSERT_STR_EQ expands to several statements, so a temporary
 * std::string argument would dangle; bind it first. */
#define EXPECT_STR(expected, actual)                                                               \
    do {                                                                                           \
        const std::string _actual = (actual);                                                      \
        ASSERT_STR_EQ(expected, _actual.c_str());                                                  \
    } while (0)

#define TERM(v, c, r)                                                                                          \
    TermHolder v##_holder((c), (r));                                                                           \
    ASSERT_TRUE(v##_holder.ok);                                                                                \
    vt::Terminal &v = v##_holder.t

/* ─── render hold ──────────────────────────────────────────────────────── */

static bool g_hold_events[8];
static size_t g_hold_len = 0;
static void holdFn(Handler *, bool held) {
    g_hold_events[g_hold_len] = held;
    g_hold_len += 1;
}

static bool holdEventsAre(const bool *want, size_t want_len) {
    if (g_hold_len != want_len) return false;
    for (size_t i = 0; i < want_len; i++) {
        if (g_hold_events[i] != want[i]) return false;
    }
    return true;
}

TEST(stream_terminal, render_hold_effect_fires_on_synchronized_output_transitions_only) {
    TERM(t, 80, 24);

    g_hold_len = 0;

    Handler handler = Handler::init(&t);
    handler.effects.render_hold = &holdFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    static const bool want[] = {true, false};

    /* Reset without a hold is ignored */
    s.nextSlice("\x1b[?2026l");
    ASSERT_TRUE(0 == g_hold_len);

    /* A set during a hold is ignored since the screen is half-drawn */
    s.nextSlice("\x1b[?2026hA\x1b[?2026hB\x1b[?2026l\x1b[?2026l");
    ASSERT_TRUE(holdEventsAre(want, 2));

    /* Save and restore report the same way as set and reset */
    g_hold_len = 0;
    s.nextSlice("\x1b[?2026s\x1b[?2026h\x1b[?2026r\x1b[?2026r");
    ASSERT_TRUE(holdEventsAre(want, 2));
    ASSERT_TRUE(!t.modes.get(modes::Mode::synchronized_output));

    /* Full reset */
    g_hold_len = 0;
    s.nextSlice("\x1b[?2026h\x1b" "c\x1b" "c");
    ASSERT_TRUE(holdEventsAre(want, 2));

    /* Resize */
    g_hold_len = 0;
    s.nextSlice("\x1b[?2026h");
    ASSERT_TRUE(s.handler.resize(vt::Terminal::Resize(80, 24)));
    ASSERT_TRUE(s.handler.resize(vt::Terminal::Resize(80, 24)));
    ASSERT_TRUE(holdEventsAre(want, 2));
}

/* Wisp: upstream's "render hold effect can snapshot the frame when the hold
 * begins" drives render.zig's RenderState, which is not ported; not ported. */

TEST(stream_terminal, resize_clears_synchronized_output_on_unchanged_cell_dimensions) {
    TERM(t, 80, 24);

    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    t.modes.set(modes::Mode::synchronized_output, true);
    vt::Terminal::Resize r(80, 24);
    vt::Terminal::Resize::CellSize cs;
    cs.width = 9;
    cs.height = 18;
    r.cell_size_px = vt::Maybe<vt::Terminal::Resize::CellSize>(cs);
    ASSERT_TRUE(s.handler.resize(r));

    ASSERT_TRUE(!t.modes.get(modes::Mode::synchronized_output));
    ASSERT_TRUE(720 == t.width_px);
    ASSERT_TRUE(432 == t.height_px);
}

/* ─── unknown APC ──────────────────────────────────────────────────────── */

static size_t g_apc_count = 0;
static char g_apc_content[16];
static size_t g_apc_content_len = 0;
static bool g_apc_truncated = false;

static void unknownSequenceFn(Handler *, Handler::UnknownSequence value) {
    switch (value.tag) {
    case Handler::UnknownSequence::Tag::apc: {
        g_apc_content_len = value.apc.content_len;
        memcpy(g_apc_content, value.apc.content, value.apc.content_len);
        g_apc_truncated = value.apc.truncated;
        break;
    }
    }
    g_apc_count += 1;
}

TEST(stream_terminal, unknown_APC_effect_callback) {
    TERM(t, 80, 24);

    g_apc_count = 0;

    Handler handler = Handler::init(&t);
    handler.unknown_sequence = &unknownSequenceFn;
    handler.apc_handler.unknown_max_bytes = 8;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Unknown OSC commands retain their legacy behavior and are ignored. */
    s.nextSlice("\x1B]999;abcdef\x07");
    s.nextSlice("\x1B_abcd;payload\x1B\\");

    ASSERT_TRUE(1 == g_apc_count);
    ASSERT_TRUE(std::string(g_apc_content, g_apc_content_len) == "abcd;pay");
    ASSERT_TRUE(g_apc_truncated);
}

/* ─── resize ───────────────────────────────────────────────────────────── */

/* Wisp: the tests' `writePty` collectors. Upstream uses a per-test struct
 * with globals; the shape is the same. */
static char g_pty_response[4096];
static size_t g_pty_response_len = 0;
static size_t g_pty_calls = 0;

static void writePtyFn(Handler *, const char *data, size_t len) {
    memcpy(g_pty_response + g_pty_response_len, data, len);
    g_pty_response_len += len;
    g_pty_calls += 1;
}

/* Upstream's collectors that overwrite rather than append. */
static void writePtyLastFn(Handler *, const char *data, size_t len) {
    memcpy(g_pty_response, data, len);
    g_pty_response_len = len;
    g_pty_calls += 1;
}

static void writePtyCountFn(Handler *, const char *, size_t) { g_pty_calls += 1; }

static std::string ptyResponse() { return std::string(g_pty_response, g_pty_response_len); }

static void resetPty() {
    g_pty_response_len = 0;
    g_pty_calls = 0;
}

/* Wisp: `.{ .cols = c, .rows = r, .cell_size_px = .{ .width = w, .height = h } }` */
static vt::Terminal::Resize resizeOf(unsigned cols, unsigned rows) {
    return vt::Terminal::Resize((vt::size::CellCountInt)cols, (vt::size::CellCountInt)rows);
}
static vt::Terminal::Resize resizeOf(unsigned cols, unsigned rows, uint32_t w, uint32_t h) {
    vt::Terminal::Resize r = resizeOf(cols, rows);
    vt::Terminal::Resize::CellSize cs;
    cs.width = w;
    cs.height = h;
    r.cell_size_px = vt::Maybe<vt::Terminal::Resize::CellSize>(cs);
    return r;
}

TEST(stream_terminal, resize_reports_mode_2048_geometry) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    t.modes.set(modes::Mode::in_band_size_reports, true);
    ASSERT_TRUE(s.handler.resize(resizeOf(100, 40, 9, 18)));

    EXPECT_STR("\x1B[48;40;100;720;900t", ptyResponse());
}

TEST(stream_terminal, resize_suppresses_mode_2048_reports) {
    resetPty();

    TERM(t, 80, 24);
    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyCountFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Disabled mode suppresses a report even with pixels and a callback. */
    ASSERT_TRUE(s.handler.resize(resizeOf(80, 24, 9, 18)));
    ASSERT_TRUE(0 == g_pty_calls);

    /* Missing pixel geometry suppresses a report even with the mode enabled. */
    t.modes.set(modes::Mode::in_band_size_reports, true);
    ASSERT_TRUE(s.handler.resize(resizeOf(80, 24)));
    ASSERT_TRUE(0 == g_pty_calls);

    /* A read-only stream has no write effect and remains successful. */
    TERM(readonly_terminal, 80, 24);
    readonly_terminal.modes.set(modes::Mode::in_band_size_reports, true);
    StreamHolder readonly_sh(Handler::init(&readonly_terminal));
    ASSERT_TRUE(readonly_sh.s.handler.resize(resizeOf(80, 24, 9, 18)));
    ASSERT_TRUE(0 == g_pty_calls);
}

TEST(stream_terminal, resize_failure_preserves_terminal_state_and_does_not_write) {
    zigstd::FailingAllocator failing(talloc(), SIZE_MAX);
    const zigstd::Allocator alloc = failing.allocator();
    vt::Terminal::Options opts((vt::size::CellCountInt)10, (vt::size::CellCountInt)1);
    vt::Terminal t;
    ASSERT_TRUE(vt::Terminal::init(alloc, opts, &t));

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyCountFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    t.modes.set(modes::Mode::synchronized_output, true);
    t.modes.set(modes::Mode::in_band_size_reports, true);
    failing.fail_index = failing.alloc_index;
    ASSERT_TRUE(!s.handler.resize(resizeOf(513, 1, 9, 18)));

    ASSERT_TRUE(t.modes.get(modes::Mode::synchronized_output));
    ASSERT_TRUE(0 == g_pty_calls);
    ASSERT_TRUE(10 == t.cols);
    ASSERT_TRUE(0 == t.width_px);
    ASSERT_TRUE(0 == t.height_px);

    t.deinit(alloc);
}

static void writePtyNoopFn(Handler *, const char *, size_t) {}

TEST(stream_terminal, resize_effects_do_not_change_canonical_terminal_state) {
    TERM(authoritative, 10, 5);
    TERM(readonly, 10, 5);

    Handler authoritative_handler = Handler::init(&authoritative);
    authoritative_handler.effects.write_pty = &writePtyNoopFn;
    StreamHolder authoritative_sh(authoritative_handler);
    StreamHolder readonly_sh(Handler::init(&readonly));

    authoritative.modes.set(modes::Mode::in_band_size_reports, true);
    readonly.modes.set(modes::Mode::in_band_size_reports, true);
    const vt::Terminal::Resize value = resizeOf(20, 10, 9, 18);
    ASSERT_TRUE(authoritative_sh.s.handler.resize(value));
    ASSERT_TRUE(readonly_sh.s.handler.resize(value));

    ASSERT_TRUE(authoritative.cols == readonly.cols);
    ASSERT_TRUE(authoritative.rows == readonly.rows);
    ASSERT_TRUE(authoritative.width_px == readonly.width_px);
    ASSERT_TRUE(authoritative.height_px == readonly.height_px);
    ASSERT_TRUE(authoritative.modes.values.bits == readonly.modes.values.bits);
    ASSERT_TRUE(authoritative.modes.saved.bits == readonly.modes.saved.bits);
    ASSERT_TRUE(authoritative.modes.default_.bits == readonly.modes.default_.bits);
    ASSERT_TRUE(authoritative.scrolling_region.top == readonly.scrolling_region.top &&
                authoritative.scrolling_region.bottom == readonly.scrolling_region.bottom &&
                authoritative.scrolling_region.left == readonly.scrolling_region.left &&
                authoritative.scrolling_region.right == readonly.scrolling_region.right);
}

/* ─── basics ───────────────────────────────────────────────────────────── */

TEST(stream_terminal, basic_print) {
    TERM(t, 10, 10);

    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    s.nextSlice("Hello");
    ASSERT_TRUE(5 == t.screens.active->cursor.x);
    ASSERT_TRUE(0 == t.screens.active->cursor.y);

    EXPECT_STR("Hello", t.plainString());
}

TEST(stream_terminal, semantic_failure_is_sticky_while_processing_continues) {
    zigstd::FailingAllocator failing(talloc(), SIZE_MAX);
    const zigstd::Allocator alloc = failing.allocator();
    vt::Terminal::Options opts((vt::size::CellCountInt)10, (vt::size::CellCountInt)2);
    vt::Terminal t;
    ASSERT_TRUE(vt::Terminal::init(alloc, opts, &t));

    {
        StreamHolder sh(Handler::init(&t));
        Stream &s = sh.s;
        ASSERT_TRUE(!s.handler.semantic_failure);

        /* Setting the title is a terminal-owned semantic update. Force its
         * allocation to fail at the central vtFallible boundary. */
        failing.fail_index = failing.alloc_index;
        s.nextSlice("\x1B]2;unavailable\x1B\\");
        ASSERT_TRUE(s.handler.semantic_failure);

        /* Later input and RIS remain best-effort and never clear the diagnostic. */
        failing.fail_index = SIZE_MAX;
        s.nextSlice("ignored");
        s.nextSlice("\x1B" "c");
        s.nextSlice("OK");
        ASSERT_TRUE(s.handler.semantic_failure);

        EXPECT_STR("OK", t.plainString());

        /* A new execution root starts without inheriting the diagnostic. */
        Handler fresh = Handler::init(&t);
        ASSERT_TRUE(!fresh.semantic_failure);
        fresh.deinit();
    }

    t.deinit(alloc);
}

TEST(stream_terminal, cursor_movement) {
    TERM(t, 10, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Move cursor using escape sequences */
    s.nextSlice("Hello\x1B[1;1H");
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    ASSERT_TRUE(0 == t.screens.active->cursor.y);

    /* Move to position 2,3 */
    s.nextSlice("\x1B[2;3H");
    ASSERT_TRUE(2 == t.screens.active->cursor.x);
    ASSERT_TRUE(1 == t.screens.active->cursor.y);
}

TEST(stream_terminal, erase_operations) {
    TERM(t, 20, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Print some text */
    s.nextSlice("Hello World");
    ASSERT_TRUE(11 == t.screens.active->cursor.x);
    ASSERT_TRUE(0 == t.screens.active->cursor.y);

    /* Move cursor to position 1,6 and erase from cursor to end of line */
    s.nextSlice("\x1B[1;6H");
    s.nextSlice("\x1B[K");

    EXPECT_STR("Hello", t.plainString());
}

TEST(stream_terminal, tabs) {
    TERM(t, 80, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    s.nextSlice("A\tB");
    ASSERT_TRUE(9 == t.screens.active->cursor.x);

    EXPECT_STR("A       B", t.plainString());
}

TEST(stream_terminal, modes) {
    TERM(t, 80, 24);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Test wraparound mode */
    ASSERT_TRUE(t.modes.get(modes::Mode::wraparound));
    s.nextSlice("\x1B[?7l"); /* Disable wraparound */
    ASSERT_TRUE(!t.modes.get(modes::Mode::wraparound));
    s.nextSlice("\x1B[?7h"); /* Enable wraparound */
    ASSERT_TRUE(t.modes.get(modes::Mode::wraparound));
}

TEST(stream_terminal, scrolling_regions) {
    TERM(t, 80, 24);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Set scrolling region from line 5 to 20 */
    s.nextSlice("\x1B[5;20r");
    ASSERT_TRUE(4 == t.scrolling_region.top);
    ASSERT_TRUE(19 == t.scrolling_region.bottom);
    ASSERT_TRUE(0 == t.scrolling_region.left);
    ASSERT_TRUE(79 == t.scrolling_region.right);
}

TEST(stream_terminal, charsets) {
    TERM(t, 80, 24);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Configure G0 as DEC special graphics */
    s.nextSlice("\x1B(0");
    s.nextSlice("`"); /* Should print diamond character */

    EXPECT_STR("\xE2\x97\x86", t.plainString()); /* ◆ */
}

TEST(stream_terminal, alt_screen) {
    TERM(t, 10, 5);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Write to primary screen */
    s.nextSlice("Primary");
    ASSERT_TRUE(vt::ScreenSet::Key::primary == t.screens.active_key);

    /* Switch to alt screen */
    s.nextSlice("\x1B[?1049h");
    ASSERT_TRUE(vt::ScreenSet::Key::alternate == t.screens.active_key);

    /* Write to alt screen */
    s.nextSlice("Alt");

    /* Switch back to primary */
    s.nextSlice("\x1B[?1049l");
    ASSERT_TRUE(vt::ScreenSet::Key::primary == t.screens.active_key);

    EXPECT_STR("Primary", t.plainString());
}

TEST(stream_terminal, cursor_save_and_restore) {
    TERM(t, 80, 24);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Move cursor to 10,15 */
    s.nextSlice("\x1B[10;15H");
    ASSERT_TRUE(14 == t.screens.active->cursor.x);
    ASSERT_TRUE(9 == t.screens.active->cursor.y);

    /* Save cursor */
    s.nextSlice("\x1B" "7");

    /* Move cursor elsewhere */
    s.nextSlice("\x1B[1;1H");
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    ASSERT_TRUE(0 == t.screens.active->cursor.y);

    /* Restore cursor */
    s.nextSlice("\x1B" "8");
    ASSERT_TRUE(14 == t.screens.active->cursor.x);
    ASSERT_TRUE(9 == t.screens.active->cursor.y);
}

TEST(stream_terminal, attributes) {
    TERM(t, 80, 24);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Set bold and write text */
    s.nextSlice("\x1B[1mBold\x1B[0m");

    /* Verify we can write attributes - just check the string was written */
    EXPECT_STR("Bold", t.plainString());
}

/* ─── DCS ──────────────────────────────────────────────────────────────── */

/* Wisp: the tests' shared `expectResponse` over the writePty capture. */
static void expectPtyResponseImpl(const char *expected, size_t expected_len, const char *file, int line) {
    if (g_pty_calls != 1) {
        fprintf(stderr, "  FAIL  %s:%d: expected 1 write_pty call, got %zu\n", file, line, g_pty_calls);
        g_fail_count++;
        return;
    }
    const std::string got = ptyResponse();
    if (got != std::string(expected, expected_len)) {
        fprintf(stderr, "  FAIL  %s:%d: write_pty response mismatch\n", file, line);
        g_fail_count++;
        return;
    }
    resetPty();
}

#define EXPECT_PTY(expected)                                                                                   \
    do {                                                                                                       \
        expectPtyResponseImpl((expected), sizeof(expected) - 1, __FILE__, __LINE__);                            \
        if (g_fail_count != 0) return;                                                                         \
    } while (0)

/* Wisp: std.fmt.bytesToHex(s, .upper) */
static std::string hexUpper(const char *s) {
    static const char *digits = "0123456789ABCDEF";
    std::string out;
    for (size_t i = 0; s[i] != 0; i++) {
        const uint8_t b = (uint8_t)s[i];
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0xF]);
    }
    return out;
}
static std::string hexLower(const char *s) {
    static const char *digits = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; s[i] != 0; i++) {
        const uint8_t b = (uint8_t)s[i];
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0xF]);
    }
    return out;
}

TEST(stream_terminal, DECRQSS_responses) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* SGR */
    s.nextSlice("\x1B[1m\x1BP$qm\x1B\\");
    EXPECT_PTY("\x1BP1$r0;1m\x1B\\");

    /* Overline */
    s.nextSlice("\x1B[0;53m\x1BP$qm\x1B\\");
    EXPECT_PTY("\x1BP1$r0;53m\x1B\\");

    /* Requests larger than the parser's fixed request buffer are ignored,
     * and the next DCS command must still be processed normally. */
    s.nextSlice("\x1BP$qfoo\x1B\\");
    ASSERT_TRUE(0 == g_pty_calls);
    ASSERT_TRUE(!s.handler.semantic_failure);
    s.nextSlice("\x1BP$qm\x1B\\");
    EXPECT_PTY("\x1BP1$r0;53m\x1B\\");
}

TEST(stream_terminal, DECRQSS_without_write_effect_is_ignored) {
    TERM(t, 80, 24);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    s.nextSlice("\x1BP$qm\x1B\\");
    ASSERT_TRUE(!s.handler.semantic_failure);
}

TEST(stream_terminal, XTGETTCAP_responses) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* The full capability table comes from the static terminfo map; this
     * checks the wiring for a valued and a valueless (boolean) capability. */
    s.nextSlice(("\x1BP+q" + hexUpper("Co") + "\x1B\\").c_str());
    {
        const std::string want = "\x1BP1+r" + hexUpper("Co") + "=" + hexUpper("256") + "\x1B\\";
        expectPtyResponseImpl(want.data(), want.size(), __FILE__, __LINE__);
        if (g_fail_count != 0) return;
    }
    s.nextSlice(("\x1BP+q" + hexUpper("am") + "\x1B\\").c_str());
    {
        const std::string want = "\x1BP1+r" + hexUpper("am") + "\x1B\\";
        expectPtyResponseImpl(want.data(), want.size(), __FILE__, __LINE__);
        if (g_fail_count != 0) return;
    }

    /* One response per requested key; lowercase hex is normalized by the
     * DCS parser. The capture holds the last ("Co") reply. */
    s.nextSlice(("\x1BP+q" + hexLower("am") + ";" + hexLower("Co") + "\x1B\\").c_str());
    ASSERT_TRUE(2 == g_pty_calls);
    {
        const std::string want = "\x1BP1+r" + hexUpper("Co") + "=" + hexUpper("256") + "\x1B\\";
        EXPECT_STR(want.c_str(), ptyResponse());
    }
    resetPty();

    /* Unknown and malformed keys are skipped without an error. */
    s.nextSlice("\x1BP+qWHO;5;GG\x1B\\");
    ASSERT_TRUE(0 == g_pty_calls);
    ASSERT_TRUE(!s.handler.semantic_failure);
}

TEST(stream_terminal, XTGETTCAP_without_write_effect_is_ignored) {
    TERM(t, 80, 24);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    s.nextSlice(("\x1BP+q" + hexUpper("TN") + ";" + hexUpper("am") + "\x1B\\").c_str());
    ASSERT_TRUE(!s.handler.semantic_failure);
}

TEST(stream_terminal, XTGETTCAP_TN_responses) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    const std::string tn_query = "\x1BP+q" + hexUpper("TN") + "\x1B\\";

    /* While no name is configured the query goes unanswered. */
    s.nextSlice(tn_query.c_str());
    ASSERT_TRUE(0 == g_pty_calls);

    /* A configured name is reported hex-encoded. */
    s.handler.has_terminfo_name = true;
    s.handler.terminfo_name = osc::ZStr("xterm-256color", 14);
    s.nextSlice(tn_query.c_str());
    {
        const std::string want = "\x1BP1+r" + hexUpper("TN") + "=" + hexUpper("xterm-256color") + "\x1B\\";
        expectPtyResponseImpl(want.data(), want.size(), __FILE__, __LINE__);
        if (g_fail_count != 0) return;
    }

    /* A maximum-length name is still reported in full. */
    const std::string max_name(Handler::max_terminfo_name_bytes, 'a');
    s.handler.terminfo_name = osc::ZStr(max_name.data(), max_name.size());
    s.nextSlice(tn_query.c_str());
    {
        const std::string want = "\x1BP1+r" + hexUpper("TN") + "=" + hexUpper(max_name.c_str()) + "\x1B\\";
        expectPtyResponseImpl(want.data(), want.size(), __FILE__, __LINE__);
        if (g_fail_count != 0) return;
    }

    /* An empty name is silent; "Co" is still answered. */
    s.handler.terminfo_name = osc::ZStr("", 0);
    s.nextSlice(("\x1BP+q" + hexUpper("TN") + ";" + hexUpper("Co") + "\x1B\\").c_str());
    {
        const std::string want = "\x1BP1+r" + hexUpper("Co") + "=" + hexUpper("256") + "\x1B\\";
        expectPtyResponseImpl(want.data(), want.size(), __FILE__, __LINE__);
        if (g_fail_count != 0) return;
    }

    /* As are names beyond the maximum length. */
    const std::string too_long(Handler::max_terminfo_name_bytes + 1, 'a');
    s.handler.terminfo_name = osc::ZStr(too_long.data(), too_long.size());
    s.nextSlice(tn_query.c_str());
    ASSERT_TRUE(0 == g_pty_calls);
    ASSERT_TRUE(!s.handler.semantic_failure);
}

TEST(stream_terminal, DCS_command_memory_is_released) {
    TERM(t, 80, 24);

    Stream s(Handler::init(&t), StreamHolder::streamOptions());

    /* A completed command transfers its allocation to Command; dcsCommand
     * must release it even when there is no write effect. */
    s.nextSlice("\x1BP+q536D756C78\x1B\\");

    /* An incomplete command remains owned by the handler and must be released
     * when the stream is deinitialized. testing.allocator detects either leak. */
    s.nextSlice("\x1BP+q536D756C78");
    s.deinit();
}

TEST(stream_terminal, DECALN_screen_alignment) {
    TERM(t, 10, 3);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Run DECALN */
    s.nextSlice("\x1B#8");

    /* Verify entire screen is filled with 'E' */
    EXPECT_STR("EEEEEEEEEE\nEEEEEEEEEE\nEEEEEEEEEE", t.plainString());

    /* Cursor should be at 1,1 */
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
}

TEST(stream_terminal, full_reset) {
    TERM(t, 80, 24);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Make some changes */
    s.nextSlice("Hello");
    s.nextSlice("\x1B[10;20H");
    s.nextSlice("\x1B[5;20r"); /* Set scroll region */
    s.nextSlice("\x1B[?7l");   /* Disable wraparound */
    /* Wisp: the glyph glossary half of this test is gated on
     * build_options.glyph_protocol, which this build disables. */

    /* Full reset */
    s.nextSlice("\x1B" "c");

    /* Verify reset state */
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(0 == t.scrolling_region.top);
    ASSERT_TRUE(23 == t.scrolling_region.bottom);
    ASSERT_TRUE(t.modes.get(modes::Mode::wraparound));
}

TEST(stream_terminal, ignores_query_actions) {
    TERM(t, 80, 24);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* These should be ignored without error */
    s.nextSlice("\x1B[c");                    /* Device attributes */
    s.nextSlice("\x1B[5n");                   /* Device status report */
    s.nextSlice("\x1B[6n");                   /* Cursor position report */
    s.nextSlice("\x1B]4;0;?\x1B\\");          /* OSC color query */
    s.nextSlice("\x1B]21;foreground=?\x1B\\"); /* Kitty color query */
    s.nextSlice("\x1B]52;c;%%%invalid-base64%%%\x1B\\");
    s.nextSlice("\x1B_Ga=p,i=999\x1B\\");            /* Missing Kitty image */
    s.nextSlice("\x1B_25a1;r;cp=41;%%%invalid%%%\x1B\\"); /* Rejected glyph */

    /* Query, malformed input, protocol failure responses, and external-effect
     * failures do not imply that terminal-owned semantic state diverged. */
    ASSERT_TRUE(!s.handler.semantic_failure);

    /* Terminal should still be functional */
    s.nextSlice("Test");
    EXPECT_STR("Test", t.plainString());
}

/* ─── OSC colors ───────────────────────────────────────────────────────── */

TEST(stream_terminal, OSC_4_set_and_reset_palette) {
    TERM(t, 10, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Save default color */
    const terminal::RGB default_color_0 = (*t.colors.palette.original)[0];

    /* Set color 0 to red */
    s.nextSlice("\x1b]4;0;rgb:ff/00/00\x1b\\");
    ASSERT_TRUE(0xff == t.colors.palette.current[0].r);
    ASSERT_TRUE(0x00 == t.colors.palette.current[0].g);
    ASSERT_TRUE(0x00 == t.colors.palette.current[0].b);
    ASSERT_TRUE(t.colors.palette.mask.isSet(0));

    /* Reset color 0 */
    s.nextSlice("\x1b]104;0\x1b\\");
    ASSERT_TRUE(default_color_0.eql(t.colors.palette.current[0]));
    ASSERT_TRUE(!t.colors.palette.mask.isSet(0));
}

TEST(stream_terminal, OSC_104_reset_all_palette_colors) {
    TERM(t, 10, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Set multiple colors */
    s.nextSlice("\x1b]4;0;rgb:ff/00/00\x1b\\");
    s.nextSlice("\x1b]4;1;rgb:00/ff/00\x1b\\");
    s.nextSlice("\x1b]4;2;rgb:00/00/ff\x1b\\");
    ASSERT_TRUE(t.colors.palette.mask.isSet(0));
    ASSERT_TRUE(t.colors.palette.mask.isSet(1));
    ASSERT_TRUE(t.colors.palette.mask.isSet(2));

    /* Reset all palette colors */
    s.nextSlice("\x1b]104\x1b\\");
    ASSERT_TRUE((*t.colors.palette.original)[0].eql(t.colors.palette.current[0]));
    ASSERT_TRUE((*t.colors.palette.original)[1].eql(t.colors.palette.current[1]));
    ASSERT_TRUE((*t.colors.palette.original)[2].eql(t.colors.palette.current[2]));
    ASSERT_TRUE(!t.colors.palette.mask.isSet(0));
    ASSERT_TRUE(!t.colors.palette.mask.isSet(1));
    ASSERT_TRUE(!t.colors.palette.mask.isSet(2));
}

TEST(stream_terminal, OSC_10_set_and_reset_foreground_color) {
    TERM(t, 10, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    terminal::RGB c;

    /* Initially unset */
    ASSERT_TRUE(!t.colors.foreground.get(&c));

    /* Set foreground to red */
    s.nextSlice("\x1b]10;rgb:ff/00/00\x1b\\");
    ASSERT_TRUE(t.colors.foreground.get(&c));
    ASSERT_TRUE(0xff == c.r);
    ASSERT_TRUE(0x00 == c.g);
    ASSERT_TRUE(0x00 == c.b);

    /* Reset foreground */
    s.nextSlice("\x1b]110\x1b\\");
    ASSERT_TRUE(!t.colors.foreground.get(&c));
}

TEST(stream_terminal, OSC_11_set_and_reset_background_color) {
    TERM(t, 10, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    terminal::RGB default_;
    default_.r = 0x10;
    default_.g = 0x20;
    default_.b = 0x30;
    t.colors.background.has_default = true;
    t.colors.background.default_ = default_;

    terminal::RGB c;

    /* Set background to green */
    s.nextSlice("\x1b]11;rgb:00/ff/00\x1b\\");
    ASSERT_TRUE(t.colors.background.get(&c));
    ASSERT_TRUE(0x00 == c.r);
    ASSERT_TRUE(0xff == c.g);
    ASSERT_TRUE(0x00 == c.b);

    /* Reset background */
    s.nextSlice("\x1b]111\x1b\\");
    ASSERT_TRUE(t.colors.background.get(&c));
    ASSERT_TRUE(default_.eql(c));
    ASSERT_TRUE(!t.colors.background.has_override);

    /* A reset color continues to follow later configuration changes. */
    terminal::RGB updated;
    updated.r = 0x40;
    updated.g = 0x50;
    updated.b = 0x60;
    t.colors.background.default_ = updated;
    ASSERT_TRUE(t.colors.background.get(&c));
    ASSERT_TRUE(updated.eql(c));
}

TEST(stream_terminal, OSC_12_set_and_reset_cursor_color) {
    TERM(t, 10, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Set cursor to blue */
    s.nextSlice("\x1b]12;rgb:00/00/ff\x1b\\");
    terminal::RGB c;
    ASSERT_TRUE(t.colors.cursor.get(&c));
    ASSERT_TRUE(0x00 == c.r);
    ASSERT_TRUE(0x00 == c.g);
    ASSERT_TRUE(0xff == c.b);

    /* Reset cursor */
    s.nextSlice("\x1b]112\x1b\\");
    /* After reset, cursor might be null (using default) */
}

TEST(stream_terminal, OSC_color_query_responses) {
    TERM(t, 10, 10);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    s.nextSlice("\x1b]10;?\x1b\\");
    ASSERT_TRUE(0 == g_pty_calls);

    s.nextSlice("\x1b]11;?\x1b\\");
    ASSERT_TRUE(0 == g_pty_calls);

    s.nextSlice("\x1b]4;2;rgb:12/34/56;2;?\x1b\\");
    EXPECT_STR("\x1b]4;2;rgb:1212/3434/5656\x1b\\", ptyResponse());
    resetPty();

    s.nextSlice("\x1b]10;rgb:01/02/03\x1b\\");
    s.nextSlice("\x1b]11;rgb:04/05/06\x1b\\");
    s.nextSlice("\x1b]12;rgb:07/08/09\x1b\\");
    s.nextSlice("\x1b]10;?;?;?\x1b\\");
    EXPECT_STR("\x1b]10;rgb:0101/0202/0303\x1b\\"
               "\x1b]11;rgb:0404/0505/0606\x1b\\"
               "\x1b]12;rgb:0707/0808/0909\x1b\\",
               ptyResponse());
    resetPty();

    s.nextSlice("\x1b]112\x1b\\");
    s.nextSlice("\x1b]12;?\x07");
    EXPECT_STR("\x1b]12;rgb:0101/0202/0303\x07", ptyResponse());
}

/* ─── kitty color protocol ─────────────────────────────────────────────── */

TEST(stream_terminal, kitty_color_protocol_set_palette) {
    TERM(t, 10, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Set palette color 5 to magenta using kitty protocol */
    s.nextSlice("\x1b]21;5=rgb:ff/00/ff\x1b\\");
    ASSERT_TRUE(0xff == t.colors.palette.current[5].r);
    ASSERT_TRUE(0x00 == t.colors.palette.current[5].g);
    ASSERT_TRUE(0xff == t.colors.palette.current[5].b);
    ASSERT_TRUE(t.colors.palette.mask.isSet(5));
    ASSERT_TRUE(t.flags.dirty.palette);
}

TEST(stream_terminal, kitty_color_protocol_reset_palette) {
    TERM(t, 10, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Set and then reset palette color */
    const terminal::RGB original = (*t.colors.palette.original)[7];
    s.nextSlice("\x1b]21;7=rgb:aa/bb/cc\x1b\\");
    ASSERT_TRUE(t.colors.palette.mask.isSet(7));

    s.nextSlice("\x1b]21;7=\x1b\\");
    ASSERT_TRUE(original.eql(t.colors.palette.current[7]));
    ASSERT_TRUE(!t.colors.palette.mask.isSet(7));
}

TEST(stream_terminal, kitty_color_protocol_set_foreground) {
    TERM(t, 10, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Set foreground using kitty protocol */
    s.nextSlice("\x1b]21;foreground=rgb:12/34/56\x1b\\");
    terminal::RGB fg;
    ASSERT_TRUE(t.colors.foreground.get(&fg));
    ASSERT_TRUE(0x12 == fg.r);
    ASSERT_TRUE(0x34 == fg.g);
    ASSERT_TRUE(0x56 == fg.b);
}

TEST(stream_terminal, kitty_color_protocol_set_background) {
    TERM(t, 10, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Set background using kitty protocol */
    s.nextSlice("\x1b]21;background=rgb:78/9a/bc\x1b\\");
    terminal::RGB bg;
    ASSERT_TRUE(t.colors.background.get(&bg));
    ASSERT_TRUE(0x78 == bg.r);
    ASSERT_TRUE(0x9a == bg.g);
    ASSERT_TRUE(0xbc == bg.b);
}

TEST(stream_terminal, kitty_color_protocol_set_cursor) {
    TERM(t, 10, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Set cursor using kitty protocol */
    s.nextSlice("\x1b]21;cursor=rgb:de/f0/12\x1b\\");
    terminal::RGB cursor;
    ASSERT_TRUE(t.colors.cursor.get(&cursor));
    ASSERT_TRUE(0xde == cursor.r);
    ASSERT_TRUE(0xf0 == cursor.g);
    ASSERT_TRUE(0x12 == cursor.b);
}

TEST(stream_terminal, kitty_color_protocol_reset_foreground) {
    TERM(t, 10, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    terminal::RGB c;

    /* Set and reset foreground */
    s.nextSlice("\x1b]21;foreground=rgb:11/22/33\x1b\\");
    ASSERT_TRUE(t.colors.foreground.get(&c));

    s.nextSlice("\x1b]21;foreground=\x1b\\");
    /* After reset, should be unset */
    ASSERT_TRUE(!t.colors.foreground.get(&c));
}

TEST(stream_terminal, kitty_color_protocol_query_responses) {
    TERM(t, 10, 10);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    s.nextSlice("\x1b]21;background=?\x1b\\");
    EXPECT_STR("\x1b]21;background=\x1b\\", ptyResponse());
    resetPty();

    s.nextSlice("\x1b]21;foreground=rgb:12/34/56;2=rgb:aa/bb/cc\x1b\\");
    s.nextSlice("\x1b]21;foreground=?;background=?;2=?\x1b\\");
    EXPECT_STR("\x1b]21;foreground=rgb:12/34/56;background=;2=rgb:aa/bb/cc\x1b\\", ptyResponse());
}

TEST(stream_terminal, palette_dirty_flag_set_on_color_change) {
    TERM(t, 10, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Clear dirty flag */
    t.flags.dirty.palette = false;

    /* Setting palette color should set dirty flag */
    s.nextSlice("\x1b]4;0;rgb:ff/00/00\x1b\\");
    ASSERT_TRUE(t.flags.dirty.palette);

    /* Clear and test reset */
    t.flags.dirty.palette = false;
    s.nextSlice("\x1b]104;0\x1b\\");
    ASSERT_TRUE(t.flags.dirty.palette);

    /* Clear and test kitty protocol */
    t.flags.dirty.palette = false;
    s.nextSlice("\x1b]21;1=rgb:00/ff/00\x1b\\");
    ASSERT_TRUE(t.flags.dirty.palette);
}

/* ─── semantic prompts ─────────────────────────────────────────────────── */

typedef vt::page::Cell Cell;

TEST(stream_terminal, semantic_prompt_fresh_line) {
    TERM(t, 10, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    s.nextSlice("Hello");
    s.nextSlice("\x1b]133;L\x07");
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    ASSERT_TRUE(1 == t.screens.active->cursor.y);
}

TEST(stream_terminal, semantic_prompt_fresh_line_new_prompt) {
    TERM(t, 10, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Write some text and then send OSC 133;A (fresh_line_new_prompt) */
    s.nextSlice("Hello");
    s.nextSlice("\x1b]133;A\x07");

    /* Should do a fresh line (carriage return + index) */
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    ASSERT_TRUE(1 == t.screens.active->cursor.y);

    /* Should set cursor semantic_content to prompt */
    ASSERT_TRUE(Cell::SemanticContent::prompt == t.screens.active->cursor.semantic_content);

    /* Test with redraw option */
    s.nextSlice("prompt$ ");
    s.nextSlice("\x1b]133;A;redraw=1\x07");
    ASSERT_TRUE(t.flags.shell_redraws_prompt == terminal::osc::semantic_prompt::Redraw::true_);
}

TEST(stream_terminal, semantic_prompt_end_of_input_then_start_output) {
    TERM(t, 10, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Write some text and then send OSC 133;A (fresh_line_new_prompt) */
    s.nextSlice("Hello");
    s.nextSlice("\x1b]133;A\x07");
    s.nextSlice("prompt$ ");
    s.nextSlice("\x1b]133;B\x07");
    ASSERT_TRUE(Cell::SemanticContent::input == t.screens.active->cursor.semantic_content);
    s.nextSlice("\x1b]133;C\x07");
    ASSERT_TRUE(Cell::SemanticContent::output == t.screens.active->cursor.semantic_content);
}

TEST(stream_terminal, semantic_prompt_prompt_start) {
    TERM(t, 10, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Write some text */
    s.nextSlice("Hello");

    /* OSC 133;P marks the start of a prompt (without fresh line behavior) */
    s.nextSlice("\x1b]133;P\x07");
    ASSERT_TRUE(Cell::SemanticContent::prompt == t.screens.active->cursor.semantic_content);
    ASSERT_TRUE(5 == t.screens.active->cursor.x);
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
}

TEST(stream_terminal, semantic_prompt_new_command) {
    TERM(t, 10, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Write some text */
    s.nextSlice("Hello");
    s.nextSlice("\x1b]133;N\x07");

    /* Should behave like fresh_line_new_prompt - cursor moves to column 0
     * on next line since we had content */
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    ASSERT_TRUE(1 == t.screens.active->cursor.y);
    ASSERT_TRUE(Cell::SemanticContent::prompt == t.screens.active->cursor.semantic_content);
}

TEST(stream_terminal, semantic_prompt_new_command_at_column_zero) {
    TERM(t, 10, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* OSC 133;N when already at column 0 should stay on same line */
    s.nextSlice("\x1b]133;N\x07");
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(Cell::SemanticContent::prompt == t.screens.active->cursor.semantic_content);
}

TEST(stream_terminal, semantic_prompt_end_prompt_start_input_terminate_eol_clears_on_linefeed) {
    TERM(t, 10, 10);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Set input terminated by EOL */
    s.nextSlice("\x1b]133;I\x07");
    ASSERT_TRUE(Cell::SemanticContent::input == t.screens.active->cursor.semantic_content);

    /* Linefeed should reset semantic content to output */
    s.nextSlice("\n");
    ASSERT_TRUE(Cell::SemanticContent::output == t.screens.active->cursor.semantic_content);
}

/* ─── effect callbacks ─────────────────────────────────────────────────── */

static size_t g_bell_count = 0;
static void bellFn(Handler *) { g_bell_count += 1; }

TEST(stream_terminal, bell_effect_callback) {
    TERM(t, 80, 24);

    /* Test bell with null callback (default readonly effects) doesn't crash */
    {
        StreamHolder sh(Handler::init(&t));
        Stream &s = sh.s;

        s.nextSlice("\x07");

        /* Terminal should still be functional after bell */
        s.nextSlice("AfterBell");
        EXPECT_STR("AfterBell", t.plainString());
    }

    t.fullReset();

    /* Test bell with a callback */
    {
        g_bell_count = 0;

        Handler handler = Handler::init(&t);
        handler.effects.bell = &bellFn;

        StreamHolder sh(handler);
        Stream &s = sh.s;

        s.nextSlice("\x07");
        ASSERT_TRUE(1 == g_bell_count);

        s.nextSlice("\x07\x07");
        ASSERT_TRUE(3 == g_bell_count);
    }
}

static size_t g_notify_count = 0;
static std::string g_notify_title;
static std::string g_notify_body;

static void desktopNotificationFn(Handler *, wisp::terminal::stream::Action::ShowDesktopNotification n) {
    g_notify_count += 1;
    g_notify_title.assign(n.title.ptr, n.title.len);
    g_notify_body.assign(n.body.ptr, n.body.len);
}

TEST(stream_terminal, desktop_notification_effect_callback) {
    TERM(t, 80, 24);

    /* A null callback (the default readonly effects) silently ignores
     * notifications and leaves the terminal usable. */
    {
        StreamHolder sh(Handler::init(&t));
        Stream &s = sh.s;

        s.nextSlice("\x1B]9;Ignored\x1B\\AfterNotification");
        EXPECT_STR("AfterNotification", t.plainString());
    }

    t.fullReset();

    g_notify_count = 0;
    g_notify_title.clear();
    g_notify_body.clear();

    Handler handler = Handler::init(&t);
    handler.effects.desktop_notification = &desktopNotificationFn;

    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* OSC 9 is split across writes and carries only a body. */
    s.nextSlice("\x1B]9;Build ");
    ASSERT_TRUE(0 == g_notify_count);
    s.nextSlice("complete\x1B\\");
    ASSERT_TRUE(1 == g_notify_count);
    ASSERT_STR_EQ("", g_notify_title.c_str());
    ASSERT_STR_EQ("Build complete", g_notify_body.c_str());

    /* OSC 777 preserves its separate title and body fields. */
    s.nextSlice("\x1B]777;notify;Codex;Needs attention\x07");
    ASSERT_TRUE(2 == g_notify_count);
    ASSERT_STR_EQ("Codex", g_notify_title.c_str());
    ASSERT_STR_EQ("Needs attention", g_notify_body.c_str());
}

static size_t g_progress_count = 0;
static osc::ProgressReport::State g_progress_state = osc::ProgressReport::State::remove;
static bool g_progress_has = false;
static uint8_t g_progress_value = 0;

static void progressReportFn(Handler *, osc::ProgressReport report) {
    g_progress_count += 1;
    g_progress_state = report.state;
    g_progress_has = report.has_progress;
    g_progress_value = report.progress;
}

TEST(stream_terminal, progress_report_effect_callback) {
    TERM(t, 80, 24);

    /* A null callback (the default readonly effects) silently ignores reports. */
    {
        StreamHolder sh(Handler::init(&t));
        sh.s.nextSlice("\x1B]9;4;1;25\x1B\\");
    }

    g_progress_count = 0;
    g_progress_state = osc::ProgressReport::State::remove;
    g_progress_has = false;

    Handler handler = Handler::init(&t);
    handler.effects.progress_report = &progressReportFn;

    StreamHolder sh(handler);
    Stream &s = sh.s;

    struct Case {
        const char *sequence;
        osc::ProgressReport::State state;
        bool has_progress;
        uint8_t progress;
    };
    static const Case cases[] = {
        {"\x1B]9;4;0;\x1B\\", osc::ProgressReport::State::remove, false, 0},
        {"\x1B]9;4;1;42\x07", osc::ProgressReport::State::set, true, 42},
        {"\x1B]9;4;2;7\x1B\\", osc::ProgressReport::State::error, true, 7},
        {"\x1B]9;4;3\x1B\\", osc::ProgressReport::State::indeterminate, false, 0},
        {"\x1B]9;4;4;75\x1B\\", osc::ProgressReport::State::pause, true, 75},
    };
    const size_t cases_len = sizeof(cases) / sizeof(cases[0]);

    for (size_t i = 0; i < cases_len; i++) {
        const size_t expected_count = i + 1;
        /* Split each sequence to verify parsing survives PTY read boundaries. */
        const size_t seq_len = strlen(cases[i].sequence);
        const size_t midpoint = seq_len / 2;
        s.nextSlice(cases[i].sequence, midpoint);
        ASSERT_TRUE(expected_count - 1 == g_progress_count);
        s.nextSlice(cases[i].sequence + midpoint, seq_len - midpoint);
        ASSERT_TRUE(expected_count == g_progress_count);
        ASSERT_TRUE(cases[i].state == g_progress_state);
        ASSERT_TRUE(cases[i].has_progress == g_progress_has);
        if (cases[i].has_progress) ASSERT_TRUE(cases[i].progress == g_progress_value);
    }

    /* A full reset (RIS) removes any active progress bar. */
    s.nextSlice("\x1B]9;4;1;50\x1B\\");
    ASSERT_TRUE(cases_len + 1 == g_progress_count);
    ASSERT_TRUE(osc::ProgressReport::State::set == g_progress_state);
    s.nextSlice("\x1B" "c");
    ASSERT_TRUE(cases_len + 2 == g_progress_count);
    ASSERT_TRUE(osc::ProgressReport::State::remove == g_progress_state);
    ASSERT_TRUE(!g_progress_has);
}

/* ─── clipboard ────────────────────────────────────────────────────────── */

namespace clip = wisp::terminal::clipboard;

static size_t g_cw_count = 0;
static clip::Write::Result g_cw_result;
static clip::Location g_cw_last_location = clip::Location::standard;
static size_t g_cw_last_contents_len = 0;
static std::string g_cw_last_mime;
static std::string g_cw_last_data;
static bool g_cw_have_capture = false;

static void clearWriteCapture() {
    g_cw_last_mime.clear();
    g_cw_last_data.clear();
    g_cw_last_contents_len = 0;
    g_cw_have_capture = false;
}

static void clipboardWriteFn(Handler *, clip::Write write) {
    clearWriteCapture();
    g_cw_count += 1;
    g_cw_last_location = write.location;
    g_cw_last_contents_len = write.contents_len;
    if (write.contents_len > 0) {
        g_cw_last_mime.assign(write.contents[0].mime.ptr, write.contents[0].mime.len);
        g_cw_last_data.assign(write.contents[0].data.ptr, write.contents[0].data.len);
        g_cw_have_capture = true;
    }
    write.reply(g_cw_result);
}

TEST(stream_terminal, clipboard_write_effect_callback) {
    TERM(t, 80, 24);

    /* A null callback (the default readonly effects) silently ignores writes. */
    {
        StreamHolder sh(Handler::init(&t));
        Stream &s = sh.s;

        s.nextSlice("\x1B]52;c;aGVsbG8=\x1B\\");

        /* Terminal should still be functional after the ignored sequence */
        s.nextSlice("AfterClipboard");
        EXPECT_STR("AfterClipboard", t.plainString());
    }

    t.fullReset();

    g_cw_count = 0;
    g_cw_result = clip::Write::Result::make(clip::Write::Result::Tag::denied);
    clearWriteCapture();

    Handler handler = Handler::init(&t);
    handler.effects.clipboard_write = &clipboardWriteFn;

    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Selectors are normalized and payloads are decoded before the callback. */
    struct Case {
        const char *sequence;
        clip::Location location;
        const char *data;
        size_t data_len;
    };
    static const Case cases[] = {
        {"\x1B]52;c;aGVsbG8=\x1B\\", clip::Location::standard, "hello", 5},
        {"\x1B]52;s;d29ybGQ=\x07", clip::Location::selection, "world", 5},
        {"\x1B]52;p;cHJpbWFyeQ==\x1B\\", clip::Location::primary, "primary", 7},
        {"\x1B]52;0;Y3V0\x1B\\", clip::Location::standard, "cut", 3},
        {"\x1B]52;x;ZmFsbGJhY2s=\x1B\\", clip::Location::standard, "fallback", 8},
        {"\x1B]52;c;YQBi\x1B\\", clip::Location::standard, "a\x00" "b", 3},
        /* Missing padding is tolerated for OSC 52 since it has no way
         * to report errors to the client, matching kitty. */
        {"\x1B]52;c;dW5wYWRkZWQ\x1B\\", clip::Location::standard, "unpadded", 8},
    };
    const size_t cases_len = sizeof(cases) / sizeof(cases[0]);

    for (size_t i = 0; i < cases_len; i++) {
        const size_t expected_count = i + 1;
        s.nextSlice(cases[i].sequence);
        ASSERT_TRUE(expected_count == g_cw_count);
        ASSERT_TRUE(cases[i].location == g_cw_last_location);
        ASSERT_TRUE(1 == g_cw_last_contents_len);
        ASSERT_STR_EQ("text/plain", g_cw_last_mime.c_str());
        ASSERT_TRUE(g_cw_last_data == std::string(cases[i].data, cases[i].data_len));
    }

    /* Empty data is a clear, represented by an empty contents slice. */
    s.nextSlice("\x1B]52;s;\x1B\\");
    ASSERT_TRUE(cases_len + 1 == g_cw_count);
    ASSERT_TRUE(clip::Location::selection == g_cw_last_location);
    ASSERT_TRUE(0 == g_cw_last_contents_len);
    ASSERT_TRUE(!g_cw_have_capture);

    /* Reads and malformed base64 are ignored. The whole request is
     * discarded on invalid characters (including whitespace) rather
     * than decoding around them, per the Kitty clipboard spec that
     * governs OSC 52 base64 handling. */
    s.nextSlice("\x1B]52;c;?\x1B\\");
    s.nextSlice("\x1B]52;c;***\x1B\\");
    s.nextSlice("\x1B]52;c;SGVs!!!bG8=\x1B\\");
    s.nextSlice("\x1B]52;c;aGVs bG8=\x1B\\");
    ASSERT_TRUE(cases_len + 1 == g_cw_count);

    /* OSC 1337 Copy shares the normalized clipboard write path. */
    s.nextSlice("\x1B]1337;Copy=:aVRlcm0y\x1B\\");
    ASSERT_TRUE(cases_len + 2 == g_cw_count);
    ASSERT_TRUE(clip::Location::standard == g_cw_last_location);
    ASSERT_STR_EQ("text/plain", g_cw_last_mime.c_str());
    ASSERT_STR_EQ("iTerm2", g_cw_last_data.c_str());

    /* Parsing across write boundaries still invokes exactly one atomic write. */
    s.nextSlice("\x1B]52;p;ZnJh");
    s.nextSlice("Z21lbnRlZA==\x1B");
    s.nextSlice("\\");
    ASSERT_TRUE(cases_len + 3 == g_cw_count);
    ASSERT_TRUE(clip::Location::primary == g_cw_last_location);
    ASSERT_STR_EQ("text/plain", g_cw_last_mime.c_str());
    ASSERT_STR_EQ("fragmented", g_cw_last_data.c_str());

    /* Reply results are intentionally ignored for protocols without a
     * write acknowledgement. The denied reply above did not stop later writes. */
    ASSERT_TRUE(g_cw_result.tag == clip::Write::Result::Tag::denied);
}

static std::string g_cr_written;
static size_t g_cr_count = 0;
static clip::Location g_cr_last_location = clip::Location::standard;
static const osc::ZStr *g_cr_last_mimes = nullptr;
static size_t g_cr_last_mimes_len = 0;
static bool g_cr_last_list = true;
static std::string g_cr_last_name = "unset";
static bool g_cr_last_granted = true;
static bool g_cr_last_can_remember = true;
static bool g_cr_has_result = true;
static clip::Read::Result g_cr_result;
static bool g_cr_reply_twice = false;

static void writePtyAppendFn(Handler *, const char *data, size_t len) { g_cr_written.append(data, len); }

static void clipboardReadFn(Handler *, clip::Read read) {
    g_cr_count += 1;
    g_cr_last_location = read.location;
    g_cr_last_mimes = read.mimes;
    g_cr_last_mimes_len = read.mimes_len;
    g_cr_last_list = read.list;
    g_cr_last_name.assign(read.name.ptr, read.name.len);
    g_cr_last_granted = read.granted;
    g_cr_last_can_remember = read.can_remember;
    if (g_cr_has_result) read.reply(g_cr_result);
    if (g_cr_reply_twice) {
        static const clip::Content again[] = {clip::Content(osc::ZStr("text/plain", 10), osc::ZStr("again", 5))};
        clip::Read::Result::Success sx;
        sx.contents = again;
        sx.contents_len = 1;
        read.reply(clip::Read::Result::makeSuccess(sx));
    }
}

TEST(stream_terminal, clipboard_read_effect_callback) {
    TERM(t, 80, 24);

    g_cr_written.clear();
    g_cr_count = 0;
    g_cr_reply_twice = false;
    g_cr_has_result = true;
    {
        static const clip::Content hello[] = {
            clip::Content(osc::ZStr("text/plain", 10), osc::ZStr("hello", 5))};
        clip::Read::Result::Success sx;
        sx.contents = hello;
        sx.contents_len = 1;
        g_cr_result = clip::Read::Result::makeSuccess(sx);
    }

    /* A null callback (the default readonly effects) silently ignores reads. */
    {
        Handler handler = Handler::init(&t);
        handler.effects.write_pty = &writePtyAppendFn;
        StreamHolder sh(handler);
        sh.s.nextSlice("\x1B]52;c;?\x1B\\");
        ASSERT_TRUE(0 == g_cr_written.size());
    }

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyAppendFn;
    handler.effects.clipboard_read = &clipboardReadFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Success echoes the normalized selector and request terminator. */
    s.nextSlice("\x1B]52;c;?\x1B\\");
    ASSERT_TRUE(1 == g_cr_count);
    ASSERT_TRUE(clip::Location::standard == g_cr_last_location);
    ASSERT_TRUE(1 == g_cr_last_mimes_len);
    ASSERT_TRUE(g_cr_last_mimes[0].eql("text/plain"));
    ASSERT_TRUE(!g_cr_last_list);
    ASSERT_STR_EQ("", g_cr_last_name.c_str());
    ASSERT_TRUE(!g_cr_last_granted);
    ASSERT_TRUE(!g_cr_last_can_remember);
    ASSERT_STR_EQ("\x1B]52;c;aGVsbG8=\x1B\\", g_cr_written.c_str());

    g_cr_written.clear();
    s.nextSlice("\x1B]52;p;?\x07");
    ASSERT_TRUE(clip::Location::primary == g_cr_last_location);
    ASSERT_STR_EQ("\x1B]52;p;aGVsbG8=\x07", g_cr_written.c_str());

    /* Only the first text representation is used. */
    g_cr_written.clear();
    {
        static const clip::Content mixed[] = {
            clip::Content(osc::ZStr("image/png", 9), osc::ZStr("\x89PNG", 4)),
            clip::Content(osc::ZStr("UTF8_STRING", 11), osc::ZStr("hi", 2)),
        };
        clip::Read::Result::Success sx;
        sx.contents = mixed;
        sx.contents_len = 2;
        /* OSC 52 has no session passwords, so remember is ignored. */
        sx.remember = true;
        g_cr_result = clip::Read::Result::makeSuccess(sx);
    }
    s.nextSlice("\x1B]52;s;?\x1B\\");
    ASSERT_TRUE(clip::Location::selection == g_cr_last_location);
    ASSERT_STR_EQ("\x1B]52;s;aGk=\x1B\\", g_cr_written.c_str());

    /* Every failure, no text, and no reply all answer with an empty
     * clipboard. */
    {
        struct ResultCase {
            bool has;
            clip::Read::Result::Tag tag;
        };
        static const ResultCase results[] = {
            {true, clip::Read::Result::Tag::denied},   {true, clip::Read::Result::Tag::unsupported},
            {true, clip::Read::Result::Tag::busy},     {true, clip::Read::Result::Tag::io_error},
            {true, clip::Read::Result::Tag::success},  {false, clip::Read::Result::Tag::denied},
        };
        for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
            g_cr_written.clear();
            g_cr_has_result = results[i].has;
            if (results[i].tag == clip::Read::Result::Tag::success) {
                g_cr_result = clip::Read::Result::makeSuccess(clip::Read::Result::Success());
            } else {
                g_cr_result = clip::Read::Result::make(results[i].tag);
            }
            s.nextSlice("\x1B]52;c;?\x1B\\");
            ASSERT_STR_EQ("\x1B]52;c;\x1B\\", g_cr_written.c_str());
        }
    }

    /* A second reply is ignored. */
    g_cr_written.clear();
    g_cr_has_result = true;
    {
        static const clip::Content hello[] = {
            clip::Content(osc::ZStr("text/plain", 10), osc::ZStr("hello", 5))};
        clip::Read::Result::Success sx;
        sx.contents = hello;
        sx.contents_len = 1;
        g_cr_result = clip::Read::Result::makeSuccess(sx);
    }
    g_cr_reply_twice = true;
    s.nextSlice("\x1B]52;c;?\x1B\\");
    ASSERT_STR_EQ("\x1B]52;c;aGVsbG8=\x1B\\", g_cr_written.c_str());
    g_cr_reply_twice = false;
}

static size_t g_cwaf_count = 0;
static void clipboardWriteReplySuccessFn(Handler *, clip::Write write) {
    g_cwaf_count += 1;
    write.reply(clip::Write::Result::makeSuccess(false));
}

TEST(stream_terminal, clipboard_write_allocation_failure_is_ignored) {
    TERM(t, 80, 24);

    g_cwaf_count = 0;

    Handler handler = Handler::init(&t);
    handler.effects.clipboard_write = &clipboardWriteReplySuccessFn;

    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Only the decoded scratch data uses the terminal allocator here. Swap in
     * an allocator that always fails, then restore it before terminal teardown. */
    {
        const zigstd::Allocator alloc = t.screens.active->alloc;
        zigstd::FailingAllocator failing(talloc(), 0);
        t.screens.active->alloc = failing.allocator();
        s.nextSlice("\x1B]52;c;aGVsbG8=\x1B\\");
        t.screens.active->alloc = alloc;
    }
    ASSERT_TRUE(0 == g_cwaf_count);
    ASSERT_TRUE(!s.handler.semantic_failure);
}

/* ─── Kitty clipboard (OSC 5522) ───────────────────────────────────────── */

/* Shared capture state for the Kitty clipboard (OSC 5522) tests below:
 * records every pty response and the most recent clipboard write. */
namespace KittyClipboardCapture {

static char responses[1024];
static size_t responses_len = 0;

/* Write capture. A null write_result returns without replying. */
static size_t write_count = 0;
static bool has_write_result = true;
static clip::Write::Result write_result;
static bool write_reply_twice = false;
static clip::Location last_location = clip::Location::standard;
static size_t last_contents_len = 0;
static char last_mimes[8][64];
static size_t last_mime_lens[8];
static char last_data[8][256];
static size_t last_data_lens[8];
static char last_write_name[64];
static size_t last_write_name_len = 0;
static bool last_write_granted = false;
static bool last_write_can_remember = false;

/* Read capture. A null read_result returns without replying. */
static size_t read_count = 0;
static bool has_read_result = false;
static clip::Read::Result read_result;
static bool read_reply_twice = false;
static clip::Location last_read_location = clip::Location::standard;
static char last_read_mimes[8][64];
static size_t last_read_mime_lens[8];
static size_t last_read_mimes_len = 0;
static bool last_read_list = false;
static char last_read_name[64];
static size_t last_read_name_len = 0;
static bool last_read_granted = false;
static bool last_read_can_remember = false;

static void reset() {
    responses_len = 0;
    write_count = 0;
    has_write_result = true;
    write_result = clip::Write::Result::makeSuccess(false);
    write_reply_twice = false;
    last_location = clip::Location::standard;
    last_contents_len = 0;
    for (size_t i = 0; i < 8; i++) {
        last_mime_lens[i] = 0;
        last_data_lens[i] = 0;
        last_read_mime_lens[i] = 0;
    }
    last_write_name_len = 0;
    last_write_granted = false;
    last_write_can_remember = false;
    read_count = 0;
    has_read_result = false;
    read_reply_twice = false;
    last_read_location = clip::Location::standard;
    last_read_mimes_len = 0;
    last_read_list = false;
    last_read_name_len = 0;
    last_read_granted = false;
    last_read_can_remember = false;
}

static void writePty(Handler *, const char *data, size_t len) {
    memcpy(responses + responses_len, data, len);
    responses_len += len;
}

static void clipboardWrite(Handler *, clip::Write write) {
    write_count += 1;
    last_location = write.location;
    last_contents_len = write.contents_len;
    const size_t n = write.contents_len < 8 ? write.contents_len : 8;
    for (size_t i = 0; i < n; i++) {
        last_mime_lens[i] = write.contents[i].mime.len;
        memcpy(last_mimes[i], write.contents[i].mime.ptr, write.contents[i].mime.len);
        last_data_lens[i] = write.contents[i].data.len;
        memcpy(last_data[i], write.contents[i].data.ptr, write.contents[i].data.len);
    }
    last_write_name_len = write.name.len;
    memcpy(last_write_name, write.name.ptr, write.name.len);
    last_write_granted = write.granted;
    last_write_can_remember = write.can_remember;
    if (has_write_result) write.reply(write_result);
    if (write_reply_twice) write.reply(clip::Write::Result::make(clip::Write::Result::Tag::io_error));
}

static void clipboardRead(Handler *, clip::Read read) {
    read_count += 1;
    last_read_location = read.location;
    last_read_mimes_len = read.mimes_len;
    const size_t n = read.mimes_len < 8 ? read.mimes_len : 8;
    for (size_t i = 0; i < n; i++) {
        last_read_mime_lens[i] = read.mimes[i].len;
        memcpy(last_read_mimes[i], read.mimes[i].ptr, read.mimes[i].len);
    }
    last_read_list = read.list;
    last_read_name_len = read.name.len;
    memcpy(last_read_name, read.name.ptr, read.name.len);
    last_read_granted = read.granted;
    last_read_can_remember = read.can_remember;
    if (has_read_result) read.reply(read_result);
    if (read_reply_twice) read.reply(clip::Read::Result::make(clip::Read::Result::Tag::denied));
}

static std::string responseSlice() { return std::string(responses, responses_len); }
static std::string readMimeAt(size_t i) { return std::string(last_read_mimes[i], last_read_mime_lens[i]); }
static std::string readName() { return std::string(last_read_name, last_read_name_len); }
static std::string writeName() { return std::string(last_write_name, last_write_name_len); }
static std::string mimeAt(size_t i) { return std::string(last_mimes[i], last_mime_lens[i]); }
static std::string dataAt(size_t i) { return std::string(last_data[i], last_data_lens[i]); }

} /* namespace KittyClipboardCapture */

namespace KC = KittyClipboardCapture;

TEST(stream_terminal, kitty_clipboard_write_transaction_round_trip) {
    TERM(t, 80, 24);

    KC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &KC::writePty;
    handler.effects.clipboard_write = &KC::clipboardWrite;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Begin a write, stream two MIME types (one chunked), alias the
     * plain text, and commit. Only the commit produces a response. */
    s.nextSlice("\x1B]5522;type=write:id=42\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9wbGFpbg==;R2hvc3Q=\x1B\\"); /* "Ghost" */
    s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9wbGFpbg==;dHk=\x1B\\");     /* "ty" */
    s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9odG1s;PGI+aGk8L2I+\x1B\\"); /* "<b>hi</b>" */
    /* Alias "TEXT UTF8_STRING" -> text/plain. */
    s.nextSlice("\x1B]5522;type=walias:mime=dGV4dC9wbGFpbg==;VEVYVCBVVEY4X1NUUklORw==\x1B\\");
    ASSERT_TRUE(0 == KC::write_count);
    ASSERT_TRUE(0 == KC::responses_len);

    s.nextSlice("\x1B]5522;type=wdata\x1B\\");
    ASSERT_TRUE(1 == KC::write_count);
    ASSERT_TRUE(clip::Location::standard == KC::last_location);
    ASSERT_TRUE(4 == KC::last_contents_len);
    EXPECT_STR("text/plain", KC::mimeAt(0));
    EXPECT_STR("Ghostty", KC::dataAt(0));
    EXPECT_STR("text/html", KC::mimeAt(1));
    EXPECT_STR("<b>hi</b>", KC::dataAt(1));
    EXPECT_STR("TEXT", KC::mimeAt(2));
    EXPECT_STR("Ghostty", KC::dataAt(2));
    EXPECT_STR("UTF8_STRING", KC::mimeAt(3));
    EXPECT_STR("Ghostty", KC::dataAt(3));
    EXPECT_STR("\x1B]5522;type=write:status=DONE:id=42\x1B\\", KC::responseSlice());

    /* A commit with no transaction in flight is silently ignored. */
    s.nextSlice("\x1B]5522;type=wdata\x1B\\");
    ASSERT_TRUE(1 == KC::write_count);
}

TEST(stream_terminal, kitty_clipboard_write_result_maps_to_response_status) {
    TERM(t, 80, 24);

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &KC::writePty;
    handler.effects.clipboard_write = &KC::clipboardWrite;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    struct Case {
        bool has_result;
        clip::Write::Result::Tag tag;
        const char *response;
    };
    static const Case cases[] = {
        {true, clip::Write::Result::Tag::success, "\x1B]5522;type=write:status=DONE\x1B\\"},
        {true, clip::Write::Result::Tag::denied, "\x1B]5522;type=write:status=EPERM\x1B\\"},
        {true, clip::Write::Result::Tag::unsupported, "\x1B]5522;type=write:status=ENOSYS\x1B\\"},
        {true, clip::Write::Result::Tag::busy, "\x1B]5522;type=write:status=EBUSY\x1B\\"},
        {true, clip::Write::Result::Tag::invalid_data, "\x1B]5522;type=write:status=EINVAL\x1B\\"},
        {true, clip::Write::Result::Tag::io_error, "\x1B]5522;type=write:status=EIO\x1B\\"},
        /* No reply at all is a denial rather than silence. */
        {false, clip::Write::Result::Tag::denied, "\x1B]5522;type=write:status=EPERM\x1B\\"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        KC::reset();
        KC::has_write_result = cases[i].has_result;
        if (cases[i].tag == clip::Write::Result::Tag::success) {
            KC::write_result = clip::Write::Result::makeSuccess(false);
        } else {
            KC::write_result = clip::Write::Result::make(cases[i].tag);
        }

        /* An immediately-committed write with no data is a clear. */
        s.nextSlice("\x1B]5522;type=write\x1B\\");
        s.nextSlice("\x1B]5522;type=wdata\x1B\\");
        ASSERT_TRUE(1 == KC::write_count);
        ASSERT_TRUE(0 == KC::last_contents_len);
        EXPECT_STR(cases[i].response, KC::responseSlice());
    }

    /* A second reply is ignored. */
    KC::reset();
    KC::write_reply_twice = true;
    s.nextSlice("\x1B]5522;type=write\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata\x1B\\");
    EXPECT_STR("\x1B]5522;type=write:status=DONE\x1B\\", KC::responseSlice());

    /* The response echoes the request terminator, unlike kitty which
     * always uses ST. */
    KC::reset();
    s.nextSlice("\x1B]5522;type=write:loc=primary\x07");
    s.nextSlice("\x1B]5522;type=wdata\x07");
    ASSERT_TRUE(clip::Location::primary == KC::last_location);
    EXPECT_STR("\x1B]5522;type=write:status=DONE\x07", KC::responseSlice());

    /* A system without a primary selection answers a loc=primary write
     * with ENOSYS, echoing the id. */
    KC::reset();
    KC::write_result = clip::Write::Result::make(clip::Write::Result::Tag::unsupported);
    s.nextSlice("\x1B]5522;type=write:loc=primary:id=p1\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata\x1B\\");
    ASSERT_TRUE(clip::Location::primary == KC::last_location);
    EXPECT_STR("\x1B]5522;type=write:status=ENOSYS:id=p1\x1B\\", KC::responseSlice());
}

TEST(stream_terminal, kitty_clipboard_write_without_clipboard_effect_responds_ENOSYS) {
    TERM(t, 80, 24);

    KC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &KC::writePty;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* The transaction fails as soon as it begins; the rest of it is
     * ignored without further responses. */
    s.nextSlice("\x1B]5522;type=write:id=x\x1B\\");
    EXPECT_STR("\x1B]5522;type=write:status=ENOSYS:id=x\x1B\\", KC::responseSlice());
    s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9wbGFpbg==;R2hvc3Q=\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata\x1B\\");
    EXPECT_STR("\x1B]5522;type=write:status=ENOSYS:id=x\x1B\\", KC::responseSlice());
}

TEST(stream_terminal, kitty_clipboard_read_without_effect_is_denied_with_EPERM) {
    TERM(t, 80, 24);

    KC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &KC::writePty;
    handler.effects.clipboard_write = &KC::clipboardWrite;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* The denial never includes loc (only OK responses do) and echoes
     * the sanitized id. */
    s.nextSlice("\x1B]5522;type=read:loc=primary:id=*4 2*;dGV4dC9wbGFpbg==\x1B\\");
    EXPECT_STR("\x1B]5522;type=read:status=EPERM:id=42\x1B\\", KC::responseSlice());

    /* A missing payload is an empty MIME list, still answered. */
    KC::reset();
    s.nextSlice("\x1B]5522;type=read\x07");
    EXPECT_STR("\x1B]5522;type=read:status=EPERM\x07", KC::responseSlice());

    /* An undecodable payload is dropped without a response. */
    KC::reset();
    s.nextSlice("\x1B]5522;type=read;!!!\x1B\\");
    ASSERT_TRUE(0 == KC::responses_len);
}

TEST(stream_terminal, kitty_clipboard_read_round_trip) {
    TERM(t, 80, 24);

    KC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &KC::writePty;
    handler.effects.clipboard_read = &KC::clipboardRead;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    static const clip::Content contents[] = {
        /* Unrequested representations are never served, and the
         * served ones follow request order, not reply order. */
        clip::Content(osc::ZStr("image/png", 9), osc::ZStr("\x89PNG", 4)),
        clip::Content(osc::ZStr("text/html", 9), osc::ZStr("<b>hi</b>", 9)),
        clip::Content(osc::ZStr("text/plain", 10), osc::ZStr("Ghostty", 7)),
    };
    static const osc::ZStr available[] = {osc::ZStr("text/plain", 10), osc::ZStr("text/html", 9)};
    {
        clip::Read::Result::Success sx;
        sx.contents = contents;
        sx.contents_len = 3;
        sx.available = available;
        sx.available_len = 2;
        KC::read_result = clip::Read::Result::makeSuccess(sx);
        KC::has_read_result = true;
    }

    /* Request the targets listing plus two types from the primary
     * selection: ". text/plain text/html". */
    s.nextSlice("\x1B]5522;type=read:loc=primary:id=r1;LiB0ZXh0L3BsYWluIHRleHQvaHRtbA==\x1B\\");
    ASSERT_TRUE(1 == KC::read_count);
    ASSERT_TRUE(clip::Location::primary == KC::last_read_location);
    ASSERT_TRUE(2 == KC::last_read_mimes_len);
    EXPECT_STR("text/plain", KC::readMimeAt(0));
    EXPECT_STR("text/html", KC::readMimeAt(1));
    ASSERT_TRUE(KC::last_read_list);
    EXPECT_STR("", KC::readName());
    ASSERT_TRUE(!KC::last_read_granted);
    ASSERT_TRUE(!KC::last_read_can_remember);
    EXPECT_STR("\x1B]5522;type=read:status=OK:loc=primary:id=r1\x1B\\"
               "\x1B]5522;type=read:status=DATA:id=r1:mime=Lg==;dGV4dC9wbGFpbiB0ZXh0L2h0bWwK\x1B\\"
               "\x1B]5522;type=read:status=DATA:id=r1:mime=dGV4dC9wbGFpbg==;R2hvc3R0eQ==\x1B\\"
               "\x1B]5522;type=read:status=DATA:id=r1:mime=dGV4dC9odG1s;PGI+aGk8L2I+\x1B\\"
               "\x1B]5522;type=read:status=DONE:id=r1\x1B\\",
               KC::responseSlice());

    /* Without the listing request `available` is ignored. The response
     * echoes the request terminator. */
    KC::responses_len = 0;
    s.nextSlice("\x1B]5522;type=read:id=r2;dGV4dC9wbGFpbg==\x07");
    ASSERT_TRUE(clip::Location::standard == KC::last_read_location);
    ASSERT_TRUE(1 == KC::last_read_mimes_len);
    ASSERT_TRUE(!KC::last_read_list);
    EXPECT_STR("\x1B]5522;type=read:status=OK:id=r2\x07"
               "\x1B]5522;type=read:status=DATA:id=r2:mime=dGV4dC9wbGFpbg==;R2hvc3R0eQ==\x07"
               "\x1B]5522;type=read:status=DONE:id=r2\x07",
               KC::responseSlice());

    /* A listing-only request carries no types. */
    KC::responses_len = 0;
    s.nextSlice("\x1B]5522;type=read;Lg==\x1B\\");
    ASSERT_TRUE(0 == KC::last_read_mimes_len);
    ASSERT_TRUE(KC::last_read_list);
    EXPECT_STR("\x1B]5522;type=read:status=OK\x1B\\"
               "\x1B]5522;type=read:status=DATA:mime=Lg==;dGV4dC9wbGFpbiB0ZXh0L2h0bWwK\x1B\\"
               "\x1B]5522;type=read:status=DONE\x1B\\",
               KC::responseSlice());
}

TEST(stream_terminal, kitty_clipboard_read_result_maps_to_response_status) {
    TERM(t, 80, 24);

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &KC::writePty;
    handler.effects.clipboard_read = &KC::clipboardRead;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    struct Case {
        bool has_result;
        clip::Read::Result::Tag tag;
        const char *response;
    };
    static const Case cases[] = {
        {true, clip::Read::Result::Tag::denied, "\x1B]5522;type=read:status=EPERM:id=x\x1B\\"},
        {true, clip::Read::Result::Tag::unsupported, "\x1B]5522;type=read:status=ENOSYS:id=x\x1B\\"},
        {true, clip::Read::Result::Tag::busy, "\x1B]5522;type=read:status=EBUSY:id=x\x1B\\"},
        {true, clip::Read::Result::Tag::io_error, "\x1B]5522;type=read:status=EIO:id=x\x1B\\"},
        /* No reply at all is a denial rather than silence. */
        {false, clip::Read::Result::Tag::denied, "\x1B]5522;type=read:status=EPERM:id=x\x1B\\"},
        /* A success with nothing to serve is still OK then DONE. */
        {true, clip::Read::Result::Tag::success,
         "\x1B]5522;type=read:status=OK:id=x\x1B\\"
         "\x1B]5522;type=read:status=DONE:id=x\x1B\\"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        KC::reset();
        KC::has_read_result = cases[i].has_result;
        if (cases[i].tag == clip::Read::Result::Tag::success) {
            KC::read_result = clip::Read::Result::makeSuccess(clip::Read::Result::Success());
        } else {
            KC::read_result = clip::Read::Result::make(cases[i].tag);
        }
        s.nextSlice("\x1B]5522;type=read:id=x;dGV4dC9wbGFpbg==\x1B\\");
        ASSERT_TRUE(1 == KC::read_count);
        EXPECT_STR(cases[i].response, KC::responseSlice());
    }

    /* A second reply is ignored. */
    KC::reset();
    {
        static const clip::Content hello[] = {
            clip::Content(osc::ZStr("text/plain", 10), osc::ZStr("hello", 5))};
        clip::Read::Result::Success sx;
        sx.contents = hello;
        sx.contents_len = 1;
        KC::read_result = clip::Read::Result::makeSuccess(sx);
        KC::has_read_result = true;
    }
    KC::read_reply_twice = true;
    s.nextSlice("\x1B]5522;type=read;dGV4dC9wbGFpbg==\x1B\\");
    EXPECT_STR("\x1B]5522;type=read:status=OK\x1B\\"
               "\x1B]5522;type=read:status=DATA:mime=dGV4dC9wbGFpbg==;aGVsbG8=\x1B\\"
               "\x1B]5522;type=read:status=DONE\x1B\\",
               KC::responseSlice());
}

TEST(stream_terminal, kitty_clipboard_read_caps_requested_types) {
    TERM(t, 80, 24);

    KC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &KC::writePty;
    handler.effects.clipboard_read = &KC::clipboardRead;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* "a/0 a/1 a/2 a/3 a/4 a/5 .": extras are dropped but the listing
     * request after them still counts. */
    KC::read_result = clip::Read::Result::makeSuccess(clip::Read::Result::Success());
    KC::has_read_result = true;
    s.nextSlice("\x1B]5522;type=read;YS8wIGEvMSBhLzIgYS8zIGEvNCBhLzUgLg==\x1B\\");
    ASSERT_TRUE(1 == KC::read_count);
    ASSERT_TRUE(wisp::terminal::kitty::clipboard::max_read_mimes == KC::last_read_mimes_len);
    EXPECT_STR("a/0", KC::readMimeAt(0));
    EXPECT_STR("a/3", KC::readMimeAt(wisp::terminal::kitty::clipboard::max_read_mimes - 1));
    ASSERT_TRUE(KC::last_read_list);
}

TEST(stream_terminal, kitty_clipboard_read_password_grants) {
    TERM(t, 80, 24);

    KC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &KC::writePty;
    handler.effects.clipboard_read = &KC::clipboardRead;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Every read requests a data type ("text/plain"): a request with
     * no data types never consults the grants at all.
     *
     * pw="secret", name="app": the first request isn't granted but the
     * reply may ask to remember it. */
    KC::has_read_result = true;
    {
        clip::Read::Result::Success sx;
        sx.remember = true;
        KC::read_result = clip::Read::Result::makeSuccess(sx);
    }
    s.nextSlice("\x1B]5522;type=read:pw=c2VjcmV0:name=YXBw;dGV4dC9wbGFpbg==\x1B\\");
    ASSERT_TRUE(1 == KC::read_count);
    EXPECT_STR("app", KC::readName());
    ASSERT_TRUE(!KC::last_read_granted);
    ASSERT_TRUE(KC::last_read_can_remember);

    /* The same password is now granted; a different one is not. */
    KC::read_result = clip::Read::Result::makeSuccess(clip::Read::Result::Success());
    s.nextSlice("\x1B]5522;type=read:pw=c2VjcmV0:name=YXBw;dGV4dC9wbGFpbg==\x1B\\");
    ASSERT_TRUE(KC::last_read_granted);
    s.nextSlice("\x1B]5522;type=read:pw=b3RoZXI=:name=YXBw;dGV4dC9wbGFpbg==\x1B\\");
    ASSERT_TRUE(!KC::last_read_granted);
    ASSERT_TRUE(KC::last_read_can_remember);

    /* A password without a name doesn't count: it is neither granted
     * nor rememberable, even if the reply asks. */
    {
        clip::Read::Result::Success sx;
        sx.remember = true;
        KC::read_result = clip::Read::Result::makeSuccess(sx);
    }
    s.nextSlice("\x1B]5522;type=read:pw=c2VjcmV0;dGV4dC9wbGFpbg==\x1B\\");
    EXPECT_STR("", KC::readName());
    ASSERT_TRUE(!KC::last_read_granted);
    ASSERT_TRUE(!KC::last_read_can_remember);
    s.nextSlice("\x1B]5522;type=read:pw=b3RoZXI=;dGV4dC9wbGFpbg==\x1B\\");
    ASSERT_TRUE(!KC::last_read_can_remember);
    KC::read_result = clip::Read::Result::makeSuccess(clip::Read::Result::Success());
    s.nextSlice("\x1B]5522;type=read:pw=b3RoZXI=:name=YXBw;dGV4dC9wbGFpbg==\x1B\\");
    ASSERT_TRUE(!KC::last_read_granted);

    /* A grant is advisory: the request is still forwarded and the
     * embedder may deny it. */
    KC::responses_len = 0;
    KC::read_result = clip::Read::Result::make(clip::Read::Result::Tag::denied);
    s.nextSlice("\x1B]5522;type=read:id=d:pw=c2VjcmV0:name=YXBw;dGV4dC9wbGFpbg==\x1B\\");
    ASSERT_TRUE(KC::last_read_granted);
    EXPECT_STR("\x1B]5522;type=read:status=EPERM:id=d\x1B\\", KC::responseSlice());

    /* Grants are freed with the stream (the testing allocator catches
     * the leak otherwise). */
}

TEST(stream_terminal, kitty_clipboard_read_targets_only_never_consumes_a_one_time_grant) {
    TERM(t, 80, 24);

    KC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &KC::writePty;
    handler.effects.clipboard_read = &KC::clipboardRead;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* A one-time read grant, as minted for a paste event. */
    ASSERT_TRUE(s.handler.kitty_clipboard_grants.grant(
        talloc(), (const uint8_t *)"otp", 3, wisp::terminal::kitty::clipboard::Grants::Direction::read, true));

    /* A targets-only read (payload ".") is prompt-exempt so it never
     * consults, and must not burn, the one-time password. */
    KC::has_read_result = true;
    KC::read_result = clip::Read::Result::makeSuccess(clip::Read::Result::Success());
    s.nextSlice("\x1B]5522;type=read:pw=b3Rw:name=YXBw;Lg==\x1B\\");
    ASSERT_TRUE(1 == KC::read_count);
    ASSERT_TRUE(KC::last_read_list);
    ASSERT_TRUE(0 == KC::last_read_mimes_len);
    ASSERT_TRUE(!KC::last_read_granted);

    /* The follow-up data read still consumes the grant, exactly once. */
    s.nextSlice("\x1B]5522;type=read:pw=b3Rw:name=YXBw;dGV4dC9wbGFpbg==\x1B\\");
    ASSERT_TRUE(KC::last_read_granted);
    s.nextSlice("\x1B]5522;type=read:pw=b3Rw:name=YXBw;dGV4dC9wbGFpbg==\x1B\\");
    ASSERT_TRUE(!KC::last_read_granted);
}

TEST(stream_terminal, kitty_clipboard_write_password_grants) {
    TERM(t, 80, 24);

    KC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &KC::writePty;
    handler.effects.clipboard_write = &KC::clipboardWrite;
    handler.effects.clipboard_read = &KC::clipboardRead;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* pw="secret", name="app": the first commit isn't granted but the
     * reply may ask to remember it. */
    KC::write_result = clip::Write::Result::makeSuccess(true);
    s.nextSlice("\x1B]5522;type=write:pw=c2VjcmV0:name=YXBw\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata\x1B\\");
    ASSERT_TRUE(1 == KC::write_count);
    EXPECT_STR("app", KC::writeName());
    ASSERT_TRUE(!KC::last_write_granted);
    ASSERT_TRUE(KC::last_write_can_remember);

    /* The same password is now granted; a different one is not. */
    KC::write_result = clip::Write::Result::makeSuccess(false);
    s.nextSlice("\x1B]5522;type=write:pw=c2VjcmV0:name=YXBw\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata\x1B\\");
    ASSERT_TRUE(KC::last_write_granted);
    s.nextSlice("\x1B]5522;type=write:pw=b3RoZXI=:name=YXBw\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata\x1B\\");
    ASSERT_TRUE(!KC::last_write_granted);
    ASSERT_TRUE(KC::last_write_can_remember);

    /* Directions are independent: a write grant doesn't satisfy reads. */
    KC::has_read_result = true;
    KC::read_result = clip::Read::Result::makeSuccess(clip::Read::Result::Success());
    s.nextSlice("\x1B]5522;type=read:pw=c2VjcmV0:name=YXBw\x1B\\");
    ASSERT_TRUE(!KC::last_read_granted);

    /* A password without a name doesn't count: it is neither granted
     * nor rememberable, even if the reply asks. */
    KC::write_result = clip::Write::Result::makeSuccess(true);
    s.nextSlice("\x1B]5522;type=write:pw=c2VjcmV0\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata\x1B\\");
    EXPECT_STR("", KC::writeName());
    ASSERT_TRUE(!KC::last_write_granted);
    ASSERT_TRUE(!KC::last_write_can_remember);

    /* A grant is advisory: the request is still forwarded and the
     * embedder may deny it. */
    KC::responses_len = 0;
    KC::write_result = clip::Write::Result::make(clip::Write::Result::Tag::denied);
    s.nextSlice("\x1B]5522;type=write:id=d:pw=c2VjcmV0:name=YXBw\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata\x1B\\");
    ASSERT_TRUE(KC::last_write_granted);
    EXPECT_STR("\x1B]5522;type=write:status=EPERM:id=d\x1B\\", KC::responseSlice());

    /* Grants are freed with the stream (the testing allocator catches
     * the leak otherwise). */
}

TEST(stream_terminal, kitty_clipboard_malformed_packets_are_silently_dropped) {
    TERM(t, 80, 24);

    KC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &KC::writePty;
    handler.effects.clipboard_write = &KC::clipboardWrite;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Missing type, unknown type, bare metadata record, invalid mime
     * base64, and orphaned transaction packets all drop silently. */
    s.nextSlice("\x1B]5522;loc=primary\x1B\\");
    s.nextSlice("\x1B]5522;type=bobr\x1B\\");
    s.nextSlice("\x1B]5522;type=read:bare\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata:mime=!!!;R2hvc3Q=\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9wbGFpbg==;R2hvc3Q=\x1B\\");
    s.nextSlice("\x1B]5522;type=walias:mime=dGV4dC9wbGFpbg==;VEVYVA==\x1B\\");
    ASSERT_TRUE(0 == KC::write_count);
    ASSERT_TRUE(0 == KC::responses_len);
    ASSERT_TRUE(!s.handler.semantic_failure);

    /* The terminal is still functional afterwards. */
    s.nextSlice("ok");
    EXPECT_STR("ok", t.plainString());
}

TEST(stream_terminal, kitty_clipboard_new_write_replaces_in_flight_transaction) {
    TERM(t, 80, 24);

    KC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &KC::writePty;
    handler.effects.clipboard_write = &KC::clipboardWrite;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    s.nextSlice("\x1B]5522;type=write:id=old\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9wbGFpbg==;b2xk\x1B\\"); /* "old" */
    s.nextSlice("\x1B]5522;type=write:id=new\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9wbGFpbg==;bmV3\x1B\\"); /* "new" */
    s.nextSlice("\x1B]5522;type=wdata\x1B\\");

    ASSERT_TRUE(1 == KC::write_count);
    ASSERT_TRUE(1 == KC::last_contents_len);
    EXPECT_STR("new", KC::dataAt(0));
    EXPECT_STR("\x1B]5522;type=write:status=DONE:id=new\x1B\\", KC::responseSlice());
}

TEST(stream_terminal, kitty_clipboard_invalid_write_packets_abort_with_EINVAL) {
    TERM(t, 80, 24);

    KC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &KC::writePty;
    handler.effects.clipboard_write = &KC::clipboardWrite;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    static const char *const invalid_packets[] = {
        /* Alias payload decodes to a non-UTF-8 byte. */
        "\x1B]5522;type=walias:mime=dGV4dC9wbGFpbg==;/w==\x1B\\",
        /* Alias has no target MIME type. */
        "\x1B]5522;type=walias;VEVYVA==\x1B\\",
        /* Alias target MIME decodes to non-UTF-8 bytes. */
        "\x1B]5522;type=walias:mime=//4=;VEVYVA==\x1B\\",
        /* Write data MIME decodes to non-UTF-8 bytes. */
        "\x1B]5522;type=wdata:mime=//4=;R2hvc3Q=\x1B\\",
    };

    for (size_t i = 0; i < sizeof(invalid_packets) / sizeof(invalid_packets[0]); i++) {
        KC::responses_len = 0;
        s.nextSlice("\x1B]5522;type=write:id=w\x1B\\");
        s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9wbGFpbg==;R2hvc3Q=\x1B\\");
        s.nextSlice(invalid_packets[i]);
        EXPECT_STR("\x1B]5522;type=write:status=EINVAL:id=w\x1B\\", KC::responseSlice());
        ASSERT_TRUE(!s.handler.semantic_failure);

        /* The transaction is gone: a commit does nothing further. */
        s.nextSlice("\x1B]5522;type=wdata\x1B\\");
        ASSERT_TRUE(0 == KC::write_count);
        EXPECT_STR("\x1B]5522;type=write:status=EINVAL:id=w\x1B\\", KC::responseSlice());
    }
}

TEST(stream_terminal, kitty_clipboard_invalid_read_text_does_not_abort_a_write) {
    TERM(t, 80, 24);

    KC::reset();
    KC::has_read_result = true;
    KC::read_result = clip::Read::Result::makeSuccess(clip::Read::Result::Success());

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &KC::writePty;
    handler.effects.clipboard_read = &KC::clipboardRead;
    handler.effects.clipboard_write = &KC::clipboardWrite;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    s.nextSlice("\x1B]5522;type=write:id=w\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9wbGFpbg==;R2hvc3Q=\x1B\\");

    /* The read payload decodes to a non-UTF-8 byte. It is dropped without
     * invoking the clipboard effect or disturbing the write transaction. */
    s.nextSlice("\x1B]5522;type=read;/w==\x1B\\");
    ASSERT_TRUE(0 == KC::read_count);
    ASSERT_TRUE(0 == KC::responses_len);

    s.nextSlice("\x1B]5522;type=wdata\x1B\\");
    ASSERT_TRUE(1 == KC::write_count);
    EXPECT_STR("\x1B]5522;type=write:status=DONE:id=w\x1B\\", KC::responseSlice());
}

TEST(stream_terminal, kitty_clipboard_oversized_text_write_aborts_with_EFBIG) {
    TERM(t, 80, 24);

    KC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &KC::writePty;
    handler.effects.clipboard_write = &KC::clipboardWrite;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Shrink the limit so the test doesn't have to stream the
     * default 64MiB. */
    s.handler.kitty_clipboard_write_max_bytes = 4;

    s.nextSlice("\x1B]5522;type=write:id=w\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9wbGFpbg==;SGVsbA==\x1B\\"); /* "Hell" */
    ASSERT_TRUE(0 == KC::responses_len);
    s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9wbGFpbg==;bw==\x1B\\"); /* "o" */
    EXPECT_STR("\x1B]5522;type=write:status=EFBIG:id=w\x1B\\", KC::responseSlice());
    ASSERT_TRUE(!s.handler.semantic_failure);

    /* The transaction is gone: later data and the commit do nothing. */
    s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9wbGFpbg==;IQ==\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata\x1B\\");
    ASSERT_TRUE(0 == KC::write_count);
    EXPECT_STR("\x1B]5522;type=write:status=EFBIG:id=w\x1B\\", KC::responseSlice());
}

TEST(stream_terminal, kitty_clipboard_invalid_wdata_chunk_aborts_with_EINVAL) {
    TERM(t, 80, 24);

    KC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &KC::writePty;
    handler.effects.clipboard_write = &KC::clipboardWrite;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    s.nextSlice("\x1B]5522;type=write:id=w\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9wbGFpbg==;SGVsbG8=\x1B\\"); /* "Hello" */
    s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9wbGFpbg==;!!!bad!!!\x1B\\");
    EXPECT_STR("\x1B]5522;type=write:status=EINVAL:id=w\x1B\\", KC::responseSlice());
    ASSERT_TRUE(!s.handler.semantic_failure);

    /* The transaction is gone: later data and the commit do nothing. */
    s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9wbGFpbg==;V29ybGQ=\x1B\\"); /* "World" */
    s.nextSlice("\x1B]5522;type=wdata\x1B\\");
    ASSERT_TRUE(0 == KC::write_count);
    EXPECT_STR("\x1B]5522;type=write:status=EINVAL:id=w\x1B\\", KC::responseSlice());
}

TEST(stream_terminal, kitty_clipboard_wdata_chunks_split_one_base64_stream) {
    TERM(t, 80, 24);

    KC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &KC::writePty;
    handler.effects.clipboard_write = &KC::clipboardWrite;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* "some data" encoded as one stream, split at non-group
     * boundaries across packets. */
    s.nextSlice("\x1B]5522;type=write\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9wbGFpbg==;c29\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9wbGFpbg==;tZSBk\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9wbGFpbg==;YXRh\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata\x1B\\");

    ASSERT_TRUE(1 == KC::write_count);
    EXPECT_STR("some data", KC::dataAt(0));
    EXPECT_STR("\x1B]5522;type=write:status=DONE\x1B\\", KC::responseSlice());
}

TEST(stream_terminal, kitty_clipboard_unpadded_wdata_stream_aborts_at_commit) {
    TERM(t, 80, 24);

    KC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &KC::writePty;
    handler.effects.clipboard_write = &KC::clipboardWrite;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* "Hello" without its final padding byte: every packet decodes,
     * but the stream ends mid-group so the commit reports EINVAL. */
    s.nextSlice("\x1B]5522;type=write:id=w\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9wbGFpbg==;SGVsbG8\x1B\\");
    ASSERT_TRUE(0 == KC::responses_len);
    s.nextSlice("\x1B]5522;type=wdata\x1B\\");
    ASSERT_TRUE(0 == KC::write_count);
    EXPECT_STR("\x1B]5522;type=write:status=EINVAL:id=w\x1B\\", KC::responseSlice());
    ASSERT_TRUE(!s.handler.semantic_failure);
}

TEST(stream_terminal, kitty_clipboard_in_flight_transaction_is_freed_on_deinit) {
    TERM(t, 80, 24);

    KC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &KC::writePty;
    handler.effects.clipboard_write = &KC::clipboardWrite;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Never committed: stream deinit must free the transaction (the
     * testing allocator catches the leak otherwise). */
    s.nextSlice("\x1B]5522;type=write\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9wbGFpbg==;R2hvc3Q=\x1B\\");
    ASSERT_TRUE(0 == KC::write_count);
}

TEST(stream_terminal, kitty_clipboard_allocation_failure_is_ignored) {
    TERM(t, 80, 24);

    KC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &KC::writePty;
    handler.effects.clipboard_write = &KC::clipboardWrite;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Only transaction state uses the terminal allocator here. Swap in
     * an allocator that always fails, then restore it before teardown. */
    {
        const zigstd::Allocator alloc = t.screens.active->alloc;
        zigstd::FailingAllocator failing(talloc(), 0);
        t.screens.active->alloc = failing.allocator();
        s.nextSlice("\x1B]5522;type=write\x1B\\");
        t.screens.active->alloc = alloc;
    }

    /* Clipboard writes are external effects, best-effort like OSC 52;
     * the failed transaction never started and is not a semantic
     * failure. */
    ASSERT_TRUE(!s.handler.semantic_failure);
    s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9wbGFpbg==;R2hvc3Q=\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata\x1B\\");
    ASSERT_TRUE(0 == KC::write_count);
    ASSERT_TRUE(0 == KC::responses_len);
}

TEST(stream_terminal, kitty_clipboard_without_write_pty_still_commits_writes) {
    TERM(t, 80, 24);

    KC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.clipboard_write = &KC::clipboardWrite;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    s.nextSlice("\x1B]5522;type=write\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata:mime=dGV4dC9wbGFpbg==;R2hvc3Q=\x1B\\");
    s.nextSlice("\x1B]5522;type=wdata\x1B\\");
    ASSERT_TRUE(1 == KC::write_count);
    EXPECT_STR("Ghost", KC::dataAt(0));

    /* Reads are dropped without a way to respond. */
    s.nextSlice("\x1B]5522;type=read\x1B\\");
    ASSERT_TRUE(0 == KC::responses_len);
}


/* ─── mode reports, titles, keyboard query ─────────────────────────────── */

TEST(stream_terminal, request_mode_DECRQM_with_write_pty_callback) {
    TERM(t, 80, 24);

    /* Without callback, DECRQM should not crash */
    {
        StreamHolder sh(Handler::init(&t));
        Stream &s = sh.s;

        /* DECRQM for mode 7 (wraparound) — should be silently ignored */
        s.nextSlice("\x1B[?7$p");
        s.nextSlice("\x1B[4$p");
    }

    t.fullReset();

    /* With callback, DECRQM should produce a response */
    {
        resetPty();

        Handler handler = Handler::init(&t);
        handler.effects.write_pty = &writePtyLastFn;
        StreamHolder sh(handler);
        Stream &s = sh.s;

        /* Wraparound mode (7) is set by default */
        s.nextSlice("\x1B[?7$p");
        EXPECT_STR("\x1B[?7;1$y", ptyResponse());

        /* Disable wraparound and query again */
        s.nextSlice("\x1B[?7l");
        s.nextSlice("\x1B[?7$p");
        EXPECT_STR("\x1B[?7;2$y", ptyResponse());

        /* A large unknown mode must not alias wraparound mode 7. */
        const uint64_t before_values = t.modes.values.bits;
        const uint64_t before_saved = t.modes.saved.bits;
        s.nextSlice("\x1B[?32775$p");
        EXPECT_STR("\x1B[?32775;0$y", ptyResponse());
        ASSERT_TRUE(before_values == t.modes.values.bits);
        ASSERT_TRUE(before_saved == t.modes.saved.bits);

        /* Query an unknown mode */
        s.nextSlice("\x1B[?9999$p");
        EXPECT_STR("\x1B[?9999;0$y", ptyResponse());

        /* Query DECECM, which Ghostty recognizes but does not allow changing */
        s.nextSlice("\x1B[?117$p");
        EXPECT_STR("\x1B[?117;4$y", ptyResponse());
    }
}

TEST(stream_terminal, request_mode_DECRQM_ANSI_responses) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    struct ModeCase {
        const char *num;
        modes::Mode mode;
    };
    static const ModeCase mode_cases[] = {
        {"2", modes::Mode::disable_keyboard},
        {"4", modes::Mode::insert},
        {"12", modes::Mode::send_receive_mode},
        {"20", modes::Mode::linefeed},
    };

    for (size_t mi = 0; mi < sizeof(mode_cases) / sizeof(mode_cases[0]); mi++) {
        static const bool enabled_cases[] = {false, true, false};
        for (size_t ei = 0; ei < 3; ei++) {
            const bool enabled = enabled_cases[ei];
            const std::string set_seq = std::string("\x1b[") + mode_cases[mi].num + (enabled ? "h" : "l");
            s.nextSlice(set_seq.c_str());
            ASSERT_TRUE(enabled == t.modes.get(mode_cases[mi].mode));
            const std::string query = std::string("\x1b[") + mode_cases[mi].num + "$p";
            const std::string want =
                std::string("\x1b[") + mode_cases[mi].num + (enabled ? ";1$y" : ";2$y");
            for (size_t split = 0; split <= query.size(); split++) {
                resetPty();
                s.nextSlice(query.data(), split);
                if (split < query.size()) ASSERT_TRUE(0 == g_pty_calls);
                s.nextSlice(query.data() + split, query.size() - split);
                ASSERT_TRUE(1 == g_pty_calls);
                EXPECT_STR(want.c_str(), ptyResponse());
                ASSERT_TRUE(enabled == t.modes.get(mode_cases[mi].mode));
            }
        }
    }

    /* The two namespaces must report independent states for mode 4. */
    s.nextSlice("\x1b[4h\x1b[?4l");
    struct QueryCase {
        const char *query;
        const char *want;
    };
    static const QueryCase cases[] = {
        {"\x1b[4$p", "\x1b[4;1$y"},
        {"\x1b[?4$p", "\x1b[?4;2$y"},
        {"\x1b[9999$p", "\x1b[9999;0$y"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        resetPty();
        s.nextSlice(cases[i].query);
        ASSERT_TRUE(1 == g_pty_calls);
        EXPECT_STR(cases[i].want, ptyResponse());
    }
}

TEST(stream_terminal, stream__CSI_W_with_intermediate_but_no_params) {
    /* Regression test from AFL++ crash. CSI ? W without
     * parameters caused an out-of-bounds access on input.params[0]. */
    vt::Terminal::Options opts((vt::size::CellCountInt)80, (vt::size::CellCountInt)24);
    opts.max_scrollback_bytes = vt::Maybe<size_t>((size_t)100);
    vt::Terminal t;
    ASSERT_TRUE(vt::Terminal::init(talloc(), opts, &t));

    {
        StreamHolder sh(Handler::init(&t));
        sh.s.nextSlice("\x1b[?W");
    }

    t.deinit(talloc());
}

static size_t g_title_changed_count = 0;
static void titleChangedFn(Handler *) { g_title_changed_count += 1; }

TEST(stream_terminal, window_title_effect_is_called) {
    TERM(t, 80, 24);

    g_title_changed_count = 0;

    Handler handler = Handler::init(&t);
    handler.effects.title_changed = &titleChangedFn;

    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Set window title via OSC 2 */
    s.nextSlice("\x1b]2;Hello World\x1b\\");
    ASSERT_TRUE(t.getTitle() != nullptr);
    ASSERT_STR_EQ("Hello World", t.getTitle());
    ASSERT_TRUE(1 == g_title_changed_count);
}

TEST(stream_terminal, window_title_effect_not_called_without_callback) {
    TERM(t, 80, 24);
    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Should not crash when no callback is set */
    s.nextSlice("\x1b]2;Hello World\x1b\\");

    /* Title should still be set on terminal state */
    ASSERT_TRUE(t.getTitle() != nullptr);
    ASSERT_STR_EQ("Hello World", t.getTitle());

    /* Terminal should still be functional */
    s.nextSlice("Test");
    EXPECT_STR("Test", t.plainString());
}

TEST(stream_terminal, window_title_effect_with_empty_title) {
    TERM(t, 80, 24);

    g_title_changed_count = 0;

    Handler handler = Handler::init(&t);
    handler.effects.title_changed = &titleChangedFn;

    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Set empty window title */
    s.nextSlice("\x1b]2;\x1b\\");
    ASSERT_TRUE(t.getTitle() == nullptr);
    ASSERT_TRUE(1 == g_title_changed_count);
}

TEST(stream_terminal, kitty_keyboard_query) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;

    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Default kitty keyboard flags should be 0 */
    s.nextSlice("\x1b[?u");
    EXPECT_STR("\x1b[?0u", ptyResponse());

    /* Push kitty keyboard mode with flags and query again */
    resetPty();
    s.nextSlice("\x1b[>1u"); /* push with disambiguate flag */
    s.nextSlice("\x1b[?u");
    EXPECT_STR("\x1b[?1u", ptyResponse());
}


/* ─── xtversion, size reports, enquiry ─────────────────────────────────── */

static st::ZStr xtversionFn(Handler *) { return st::ZStr("ghostty 1.2.3", 13); }
static st::ZStr xtversionEmptyFn(Handler *) { return st::ZStr("", 0); }

static bool sizeFn918(Handler *, wisp::terminal::size_report::Size *out) {
    *out = wisp::terminal::size_report::Size(24, 80, 9, 18);
    return true;
}
static bool sizeFn816(Handler *, wisp::terminal::size_report::Size *out) {
    *out = wisp::terminal::size_report::Size(24, 80, 8, 16);
    return true;
}

TEST(stream_terminal, xtversion_default) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Without xtversion effect set, should report "libghostty" */
    s.nextSlice("\x1b[>0q");
    EXPECT_STR("\x1bP>|libghostty\x1b\\", ptyResponse());
}

TEST(stream_terminal, xtversion_with_effect) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    handler.effects.xtversion = &xtversionFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    s.nextSlice("\x1b[>0q");
    EXPECT_STR("\x1bP>|ghostty 1.2.3\x1b\\", ptyResponse());
}

TEST(stream_terminal, xtversion_with_empty_string_effect) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    handler.effects.xtversion = &xtversionEmptyFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Empty string from effect should fall back to "libghostty" */
    s.nextSlice("\x1b[>0q");
    EXPECT_STR("\x1bP>|libghostty\x1b\\", ptyResponse());
}

TEST(stream_terminal, size_report_csi_14_t_with_effect) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    handler.effects.size = &sizeFn918;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* CSI 14 t - report text area size in pixels */
    s.nextSlice("\x1b[14t");
    EXPECT_STR("\x1b[4;432;720t", ptyResponse());
}

TEST(stream_terminal, mode_2048_enable_reports_current_geometry_and_disable_is_silent) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    handler.effects.size = &sizeFn816;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    s.nextSlice("\x1b[?2048h");
    s.nextSlice("\x1b[?2048h");

    ASSERT_TRUE(2 == g_pty_calls);
    EXPECT_STR("\x1b[48;24;80;384;640t", ptyResponse());

    s.nextSlice("\x1b[?2048l");
    ASSERT_TRUE(2 == g_pty_calls);
    ASSERT_TRUE(!t.modes.get(modes::Mode::in_band_size_reports));
}

TEST(stream_terminal, mode_2048_enable_tolerates_missing_effects) {
    resetPty();

    {
        TERM(no_size_terminal, 80, 24);
        Handler no_size_handler = Handler::init(&no_size_terminal);
        no_size_handler.effects.write_pty = &writePtyCountFn;
        StreamHolder no_size_sh(no_size_handler);

        no_size_sh.s.nextSlice("\x1b[?2048h");
        ASSERT_TRUE(no_size_terminal.modes.get(modes::Mode::in_band_size_reports));
        ASSERT_TRUE(0 == g_pty_calls);
    }

    {
        TERM(no_write_terminal, 80, 24);
        Handler no_write_handler = Handler::init(&no_write_terminal);
        no_write_handler.effects.size = &sizeFn816;
        StreamHolder no_write_sh(no_write_handler);

        no_write_sh.s.nextSlice("\x1b[?2048h");
        ASSERT_TRUE(no_write_terminal.modes.get(modes::Mode::in_band_size_reports));
        ASSERT_TRUE(0 == g_pty_calls);
    }
}

TEST(stream_terminal, size_report_csi_16_t_with_effect) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    handler.effects.size = &sizeFn918;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* CSI 16 t - report cell size in pixels */
    s.nextSlice("\x1b[16t");
    EXPECT_STR("\x1b[6;18;9t", ptyResponse());
}

TEST(stream_terminal, size_report_csi_18_t_with_effect) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    handler.effects.size = &sizeFn918;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* CSI 18 t - report text area size in characters */
    s.nextSlice("\x1b[18t");
    EXPECT_STR("\x1b[8;24;80t", ptyResponse());
}

TEST(stream_terminal, size_report_no_effect_callback) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Without size effect, size reports should be silently ignored */
    s.nextSlice("\x1b[14t");
    ASSERT_TRUE(0 == g_pty_calls);
}

TEST(stream_terminal, size_report_csi_21_t_title_disabled_by_default) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Set a title first */
    s.nextSlice("\x1b]2;My Title\x1b\\");

    /* CSI 21 t - report title (no size effect needed) */
    s.nextSlice("\x1b[21t");
    ASSERT_TRUE(0 == g_pty_calls);
}

TEST(stream_terminal, size_report_csi_21_t_title_enabled) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    handler.title_report = true;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Set a title first */
    s.nextSlice("\x1b]2;My Title\x1b\\");

    /* CSI 21 t - report title (no size effect needed) */
    s.nextSlice("\x1b[21t");
    EXPECT_STR("\x1b]lMy Title\x1b\\", ptyResponse());
}

static st::ZStr enquiryFn(Handler *) { return st::ZStr("ghostty", 7); }
static st::ZStr enquiryEmptyFn(Handler *) { return st::ZStr("", 0); }

TEST(stream_terminal, enquiry_no_effect) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* ENQ without enquiry effect should not write anything */
    s.nextSlice("\x05");
    ASSERT_TRUE(0 == g_pty_calls);
}

TEST(stream_terminal, enquiry_with_effect) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    handler.effects.enquiry = &enquiryFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    s.nextSlice("\x05");
    EXPECT_STR("ghostty", ptyResponse());
}

TEST(stream_terminal, enquiry_with_empty_response) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    handler.effects.enquiry = &enquiryEmptyFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Empty enquiry response should not write anything */
    s.nextSlice("\x05");
    ASSERT_TRUE(0 == g_pty_calls);
}


/* ─── device status, device attributes ─────────────────────────────────── */

namespace ds = wisp::terminal::device_status;
namespace da = wisp::terminal::device_attributes;

TEST(stream_terminal, device_status__operating_status) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* CSI 5 n — operating status report */
    s.nextSlice("\x1B[5n");
    EXPECT_STR("\x1B[0n", ptyResponse());
}

TEST(stream_terminal, device_status__cursor_position) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Default position is 0,0 — reported as 1,1 */
    s.nextSlice("\x1B[6n");
    EXPECT_STR("\x1B[1;1R", ptyResponse());

    s.nextSlice("\x1B[5;10H");
    s.nextSlice("\x1B[6n");
    EXPECT_STR("\x1B[5;10R", ptyResponse());
}

TEST(stream_terminal, device_status__cursor_position_with_origin_mode) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Set a scroll region */
    s.nextSlice("\x1B[5;20r");
    /* Enable origin mode */
    s.nextSlice("\x1B[?6h");
    /* Move within the region */
    s.nextSlice("\x1B[3;5H");
    /* Query position */
    s.nextSlice("\x1B[6n");
    /* Should report position relative to the scroll region */
    EXPECT_STR("\x1B[3;5R", ptyResponse());
}

static bool colorSchemeDarkFn(Handler *, ds::ColorScheme *out) {
    *out = ds::ColorScheme::dark;
    return true;
}
static bool colorSchemeLightFn(Handler *, ds::ColorScheme *out) {
    *out = ds::ColorScheme::light;
    return true;
}

TEST(stream_terminal, device_status__color_scheme_dark) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    handler.effects.color_scheme = &colorSchemeDarkFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    s.nextSlice("\x1B[?996n");
    EXPECT_STR("\x1B[?997;1n", ptyResponse());
}

TEST(stream_terminal, device_status__color_scheme_light) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    handler.effects.color_scheme = &colorSchemeLightFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    s.nextSlice("\x1B[?996n");
    EXPECT_STR("\x1B[?997;2n", ptyResponse());
}

TEST(stream_terminal, device_status__color_scheme_without_callback) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Without color_scheme effect, query should be silently ignored */
    s.nextSlice("\x1B[?996n");
    ASSERT_TRUE(0 == g_pty_calls);
}

TEST(stream_terminal, visibility_reports) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Mode 2033 is supported and initially disabled. */
    s.nextSlice("\x1B[?2033$p");
    EXPECT_STR("\x1B[?2033;2$y", ptyResponse());

    /* A one-shot query reports the current state without enabling the mode. */
    s.nextSlice("\x1B[?998n");
    EXPECT_STR("\x1B[?999;1n", ptyResponse());
    ASSERT_TRUE(!t.modes.get(modes::Mode::report_visibility));

    /* Enabling always sends an immediate report, even when already enabled. */
    t.flags.visible = false;
    s.nextSlice("\x1B[?2033h");
    EXPECT_STR("\x1B[?999;2n", ptyResponse());
    const size_t count = g_pty_calls;
    s.nextSlice("\x1B[?2033h");
    ASSERT_TRUE(count + 1 == g_pty_calls);

    /* Disabling sends no report. */
    s.nextSlice("\x1B[?2033l");
    ASSERT_TRUE(count + 1 == g_pty_calls);

    /* A terminal reset preserves the view's externally owned visibility. */
    s.nextSlice("\x1B" "c");
    s.nextSlice("\x1B[?998n");
    EXPECT_STR("\x1B[?999;2n", ptyResponse());
}

TEST(stream_terminal, device_status__readonly_ignores_all) {
    TERM(t, 80, 24);

    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* All device status queries should be silently ignored without effects */
    s.nextSlice("\x1B[5n");
    s.nextSlice("\x1B[6n");
    s.nextSlice("\x1B[?996n");
    s.nextSlice("\x1B[?998n");

    /* Terminal should still be functional */
    s.nextSlice("Test");
    EXPECT_STR("Test", t.plainString());
}

static da::Attributes daDefaultFn(Handler *) { return da::Attributes(); }

TEST(stream_terminal, device_attributes__primary_DA) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    handler.effects.device_attributes = &daDefaultFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    s.nextSlice("\x1B[c");
    EXPECT_STR("\x1b[?62;22c", ptyResponse());
}

TEST(stream_terminal, device_attributes__secondary_DA) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    handler.effects.device_attributes = &daDefaultFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    s.nextSlice("\x1B[>c");
    EXPECT_STR("\x1b[>1;0;0c", ptyResponse());
}

TEST(stream_terminal, device_attributes__tertiary_DA) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    handler.effects.device_attributes = &daDefaultFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    s.nextSlice("\x1B[=c");
    EXPECT_STR("\x1bP!|00000000\x1b\\", ptyResponse());
}

TEST(stream_terminal, device_attributes__readonly_ignores) {
    TERM(t, 80, 24);

    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* All DA queries should be silently ignored without effects */
    s.nextSlice("\x1B[c");
    s.nextSlice("\x1B[>c");
    s.nextSlice("\x1B[=c");

    /* Terminal should still be functional */
    s.nextSlice("Test");
    EXPECT_STR("Test", t.plainString());
}

static da::Attributes daCustomFn(Handler *) {
    static const da::Primary::Feature features[] = {da::Primary::Feature::ansi_color,
                                                   da::Primary::Feature::clipboard};
    da::Attributes attrs;
    attrs.primary.conformance_level = da::ConformanceLevel::vt420;
    attrs.primary.features = features;
    attrs.primary.features_len = 2;
    attrs.secondary.device_type = da::DeviceType::vt420;
    attrs.secondary.firmware_version = 100;
    return attrs;
}

TEST(stream_terminal, device_attributes__custom_response) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyLastFn;
    handler.effects.device_attributes = &daCustomFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    s.nextSlice("\x1B[c");
    EXPECT_STR("\x1b[?64;22;52c", ptyResponse());

    s.nextSlice("\x1B[>c");
    EXPECT_STR("\x1b[>41;100;0c", ptyResponse());
}


/* ─── kitty drag and drop ──────────────────────────────────────────────── */

/* ─── continuation ─────────────────────────────────────────────────────── */

static size_t g_cont_bell = 0;
static size_t g_cont_title = 0;
static size_t g_cont_write = 0;
static size_t g_cont_notification = 0;
static size_t g_cont_clipboard = 0;

static void contReset() {
    g_cont_bell = 0;
    g_cont_title = 0;
    g_cont_write = 0;
    g_cont_notification = 0;
    g_cont_clipboard = 0;
}

static void contBell(Handler *) { g_cont_bell += 1; }
static void contTitleChanged(Handler *) { g_cont_title += 1; }
static void contWritePty(Handler *, const char *, size_t) { g_cont_write += 1; }
static void contDesktopNotification(Handler *, wisp::terminal::stream::Action::ShowDesktopNotification) {
    g_cont_notification += 1;
}
static void contClipboardWrite(Handler *, clip::Write write) {
    g_cont_clipboard += 1;
    write.reply(clip::Write::Result::makeSuccess(false));
}

/* Wisp: `Stream.init(.{ ..., .continuation_max_bytes = 1024 })` */
static Stream::Options contStreamOptions() {
    Stream::Options o;
    o.allocator = true;
    o.continuation_max_bytes = wisp::terminal::stream_continuation::Maybe<size_t>((size_t)1024);
    return o;
}

struct ContStreamHolder {
    Stream s;
    explicit ContStreamHolder(const Handler &h) : s(h, contStreamOptions()) {}
    ~ContStreamHolder() { s.deinit(); }
};

TEST(stream_terminal, continuation_reconstructs_standard_stream_without_duplicate_effects) {
    contReset();

    static const char committed[] = "A\n\x07"
                                    "\x1b]2;title\x1b\\"
                                    "\x1b[5n"
                                    "\x1b]9;body\x1b\\"
                                    "\x1b]52;c;aA==\x1b\\";
    const size_t committed_len = sizeof committed - 1;

    TERM(source_terminal, 80, 24);

    Handler source_handler = Handler::init(&source_terminal);
    source_handler.effects.bell = &contBell;
    source_handler.effects.title_changed = &contTitleChanged;
    source_handler.effects.write_pty = &contWritePty;
    source_handler.effects.desktop_notification = &contDesktopNotification;
    source_handler.effects.clipboard_write = &contClipboardWrite;
    ContStreamHolder source_holder(source_handler);
    Stream &source = source_holder.s;

    /* Terminal mutation and all callbacks have already committed. The
     * unfinished CSI is the only input needed to recreate the stream state. */
    {
        std::string input(committed, committed_len);
        input += "\x1b[31";
        source.nextSlice(input.data(), input.size());
    }
    ASSERT_TRUE(1 == g_cont_bell);
    ASSERT_TRUE(1 == g_cont_title);
    ASSERT_TRUE(1 == g_cont_write);
    ASSERT_TRUE(1 == g_cont_notification);
    ASSERT_TRUE(1 == g_cont_clipboard);

    std::string continuation;
    ASSERT_TRUE(Stream::ContinuationError::none == source.writeContinuation(&continuation));
    ASSERT_TRUE(continuation == std::string("\x1b[31"));

    TERM(restored_terminal, 80, 24);

    /* Stand in for restoring the already-committed terminal snapshot. */
    {
        StreamHolder snapshot(Handler::init(&restored_terminal));
        snapshot.s.nextSlice(committed, committed_len);
    }

    const std::string before = restored_terminal.plainString();
    const vt::size::CellCountInt before_x = restored_terminal.screens.active->cursor.x;
    const vt::size::CellCountInt before_y = restored_terminal.screens.active->cursor.y;
    const vt::style::Id before_style = restored_terminal.screens.active->cursor.style_id;
    ASSERT_TRUE(restored_terminal.getTitle() != nullptr);
    const std::string before_title = restored_terminal.getTitle();

    Handler restored_handler = Handler::init(&restored_terminal);
    restored_handler.effects.bell = &contBell;
    restored_handler.effects.title_changed = &contTitleChanged;
    restored_handler.effects.write_pty = &contWritePty;
    restored_handler.effects.desktop_notification = &contDesktopNotification;
    restored_handler.effects.clipboard_write = &contClipboardWrite;
    ContStreamHolder restored_holder(restored_handler);
    Stream &restored = restored_holder.s;

    contReset();
    restored.nextSlice(continuation.data(), continuation.size());
    ASSERT_TRUE(0 == g_cont_bell);
    ASSERT_TRUE(0 == g_cont_title);
    ASSERT_TRUE(0 == g_cont_write);
    ASSERT_TRUE(0 == g_cont_notification);
    ASSERT_TRUE(0 == g_cont_clipboard);
    EXPECT_STR(before.c_str(), restored_terminal.plainString());
    ASSERT_TRUE(before_x == restored_terminal.screens.active->cursor.x);
    ASSERT_TRUE(before_y == restored_terminal.screens.active->cursor.y);
    ASSERT_TRUE(before_style == restored_terminal.screens.active->cursor.style_id);
    ASSERT_TRUE(restored_terminal.getTitle() != nullptr);
    ASSERT_TRUE(before_title == std::string(restored_terminal.getTitle()));

    source.nextSlice("mB");
    restored.nextSlice("mB");
    {
        /* Both sides are temporaries, so both are bound before comparing. */
        const std::string source_text = source_terminal.plainString();
        EXPECT_STR(source_text.c_str(), restored_terminal.plainString());
    }
    ASSERT_TRUE(source_terminal.screens.active->cursor.style_id ==
                restored_terminal.screens.active->cursor.style_id);
}

namespace kdnd = wisp::terminal::kitty::dnd;

TEST(stream_terminal, kitty_dnd__query_response) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    s.nextSlice("\x1B]72;t=q:i=3\x1B\\");
    EXPECT_STR("\x1b]72;t=q:i=3\x1b\\", ptyResponse());
}

TEST(stream_terminal, kitty_dnd__register_drop_and_serve_data) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Client registers to accept drops. */
    s.nextSlice("\x1B]72;t=a;text/plain text/uri-list\x1B\\");
    EXPECT_STR("", ptyResponse());
    ASSERT_TRUE(t.kitty_dnd != nullptr);

    /* A native drop arrives; the embedder feeds it to the terminal
     * state and delivers the produced event bytes itself. */
    {
        std::string aw;
        kdnd::State::MoveEvent ev;
        ev.cell_x = 2;
        ev.cell_y = 1;
        ev.pixel_x = 20;
        ev.pixel_y = 18;
        ev.operations.copy = true;
        const kdnd::State::Item items[] = {
            kdnd::State::Item(osc::ZStr("text/plain", 10), osc::ZStr("hello", 5))};
        ASSERT_TRUE(t.kitty_dnd->dragDrop(talloc(), &aw, ev, items, 1));
        EXPECT_STR("\x1b]72;t=M:x=2:y=1:X=20:Y=18:o=1:m=0;text/plain \x1b\\", aw);
    }

    /* The client requests the data and concludes. */
    s.nextSlice("\x1B]72;t=r:x=1\x1B\\");
    EXPECT_STR("\x1b]72;t=r:x=1:m=0;aGVsbG8=\x1b\\"
               "\x1b]72;t=r:x=1\x1b\\",
               ptyResponse());
    resetPty();

    s.nextSlice("\x1B]72;t=r\x1B\\");
    EXPECT_STR("", ptyResponse());
    ASSERT_TRUE(!t.kitty_dnd->drop.has_items);
}

TEST(stream_terminal, kitty_dnd__state_updates_work_without_write_pty_effect) {
    TERM(t, 80, 24);

    StreamHolder sh(Handler::init(&t));
    Stream &s = sh.s;

    /* Queries produce no output (nowhere to write) but registration
     * state still updates. */
    s.nextSlice("\x1B]72;t=q\x1B\\");
    s.nextSlice("\x1B]72;t=a\x1B\\");
    ASSERT_TRUE(t.kitty_dnd != nullptr);

    /* The terminal remains functional. */
    s.nextSlice("ok");
    EXPECT_STR("ok", t.plainString());
}

TEST(stream_terminal, kitty_dnd__registration_survives_terminal_reset) {
    TERM(t, 80, 24);

    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Start a chunked command, then reset mid-chunk. */
    s.nextSlice("\x1B]72;t=a:i=5\x1B\\");
    s.nextSlice("\x1B]72;t=m:o=1:m=1;text/pl\x1B\\");
    s.nextSlice("\x1B" "c");

    /* Registration survives (matching kitty), chunking was interrupted
     * so a new command is not treated as a continuation. */
    ASSERT_TRUE(t.kitty_dnd != nullptr);
    ASSERT_TRUE(!t.kitty_dnd->chunking.active);
    s.nextSlice("\x1B]72;t=q\x1B\\");
    EXPECT_STR("\x1b]72;t=q\x1b\\", ptyResponse());
}

static kdnd::Event g_dnd_events[8];
static size_t g_dnd_events_len = 0;
static std::string g_dnd_mimes;

static void dragAndDropFn(Handler *handler, kdnd::Event ev) {
    g_dnd_events[g_dnd_events_len] = ev;
    g_dnd_events_len += 1;
    /* Registration details are read from the terminal state. */
    if (ev == kdnd::Event::registration) {
        g_dnd_mimes.clear();
        kdnd::State *state = handler->terminal->kitty_dnd;
        if (state == nullptr) return;
        kdnd::State::MimeIterator it = state->registeredMimes();
        osc::ZStr m;
        while (it.next(&m)) {
            g_dnd_mimes.append(m.ptr, m.len);
            g_dnd_mimes.push_back(',');
        }
    }
}

TEST(stream_terminal, kitty_dnd__effect_reports_registration_acceptance_and_conclusion) {
    TERM(t, 80, 24);

    g_dnd_events_len = 0;
    g_dnd_mimes.clear();

    Handler handler = Handler::init(&t);
    handler.effects.drag_and_drop = &dragAndDropFn;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    /* Registration with a MIME list, read back from the state. */
    s.nextSlice("\x1B]72;t=a;image/png text/plain\x1B\\");
    ASSERT_TRUE(1 == g_dnd_events_len);
    ASSERT_TRUE(g_dnd_events[0] == kdnd::Event::registration);
    EXPECT_STR("image/png,text/plain,", g_dnd_mimes);

    /* A native drag and the client's answer. */
    {
        std::string aw;
        kdnd::State::MoveEvent ev;
        ev.operations.copy = true;
        const kdnd::State::Item items[] = {
            kdnd::State::Item(osc::ZStr("text/plain", 10), osc::ZStr("x", 1))};
        ASSERT_TRUE(t.kitty_dnd->dragDrop(talloc(), &aw, ev, items, 1));
    }
    s.nextSlice("\x1B]72;t=m:o=2;text/plain\x1B\\");
    ASSERT_TRUE(2 == g_dnd_events_len);
    ASSERT_TRUE(g_dnd_events[1] == kdnd::Event::acceptance);

    /* Conclusion carries the performed operation. */
    s.nextSlice("\x1B]72;t=r:o=2\x1B\\");
    ASSERT_TRUE(3 == g_dnd_events_len);
    ASSERT_TRUE(kdnd::Event::concluded_move == g_dnd_events[2]);

    /* Unregistration reports with the state gone. */
    s.nextSlice("\x1B]72;t=A\x1B\\");
    ASSERT_TRUE(4 == g_dnd_events_len);
    ASSERT_TRUE(g_dnd_events[3] == kdnd::Event::registration);
    ASSERT_TRUE(t.kitty_dnd == nullptr);
    EXPECT_STR("", g_dnd_mimes);
}


/* ─── paste ────────────────────────────────────────────────────────────── */

namespace pp = wisp::terminal::paste;
namespace kclip = wisp::terminal::kitty::clipboard;

/* Capture state for the Handler.paste tests below: every pty write and
 * the clipboard reads the program makes afterwards. */
namespace PasteCapture {

static std::string written;
static size_t write_count = 0;
static size_t read_count = 0;
static size_t read_granted_count = 0;
static bool last_read_granted = false;
static char last_read_name[64];
static size_t last_read_name_len = 0;

static void reset() {
    written.clear();
    write_count = 0;
    read_count = 0;
    read_granted_count = 0;
    last_read_granted = false;
    last_read_name_len = 0;
}

static void writePty(Handler *, const char *data, size_t len) {
    written.append(data, len);
    write_count += 1;
}

static void clipboardRead(Handler *, clip::Read read) {
    read_count += 1;
    last_read_granted = read.granted;
    if (read.granted) read_granted_count += 1;
    last_read_name_len = read.name.len;
    memcpy(last_read_name, read.name.ptr, read.name.len);
    static const clip::Content contents[] = {
        clip::Content(osc::ZStr("text/plain", 10), osc::ZStr("Ghostty", 7))};
    clip::Read::Result::Success sx;
    sx.contents = contents;
    sx.contents_len = 1;
    read.reply(clip::Read::Result::makeSuccess(sx));
}

static std::string readName() { return std::string(last_read_name, last_read_name_len); }

} /* namespace PasteCapture */

namespace PC = PasteCapture;

/* Wisp: `.{ .memory = &.{ .{ .mime = m, .data = d } } }` */
static pp::Contents memContents(const clip::Content *items, size_t len) {
    pp::Contents c;
    c.tag = pp::Contents::Tag::memory;
    c.memory = items;
    c.memory_len = len;
    return c;
}

TEST(stream_terminal, paste__no_write_pty_effect_is_an_error) {
    TERM(t, 80, 24);

    Handler handler = Handler::init(&t);
    static const clip::Content items[] = {
        clip::Content(osc::ZStr("text/plain", 10), osc::ZStr("hello", 5))};
    Handler::Paste req;
    req.contents = memContents(items, 1);
    bool did = false;
    ASSERT_TRUE(Handler::PasteError::NoWritePty == handler.paste(req, &did));
    handler.deinit();
}

TEST(stream_terminal, paste__plain_text_converts_newlines_and_strips_unsafe_bytes) {
    TERM(t, 80, 24);

    PC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &PC::writePty;

    /* Newlines are unsafe unbracketed; the embedder confirmed. */
    {
        static const clip::Content items[] = {
            clip::Content(osc::ZStr("text/plain", 10), osc::ZStr("hel\x1blo\nwor\0ld", 13))};
        Handler::Paste req;
        req.contents = memContents(items, 1);
        req.allow_unsafe = true;
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::none == handler.paste(req, &did));
        ASSERT_TRUE(did);
    }
    EXPECT_STR("hel lo\rwor ld", PC::written);
    ASSERT_TRUE(1 == PC::write_count);

    /* The first text representation is used; others are ignored. */
    PC::reset();
    {
        static const clip::Content items[] = {
            clip::Content(osc::ZStr("image/png", 9), osc::ZStr("\x89PNG", 4)),
            clip::Content(osc::ZStr("UTF8_STRING", 11), osc::ZStr("hi", 2)),
            clip::Content(osc::ZStr("text/plain", 10), osc::ZStr("ignored", 7))};
        Handler::Paste req;
        req.contents = memContents(items, 3);
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::none == handler.paste(req, &did));
        ASSERT_TRUE(did);
    }
    EXPECT_STR("hi", PC::written);
    ASSERT_TRUE(1 == PC::write_count);

    handler.deinit();
}

TEST(stream_terminal, paste__unsafe_text_is_refused_unless_allowed) {
    TERM(t, 80, 24);

    PC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &PC::writePty;

    static const clip::Content items[] = {
        clip::Content(osc::ZStr("text/plain", 10), osc::ZStr("rm -rf /\n", 9))};

    {
        Handler::Paste req;
        req.contents = memContents(items, 1);
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::UnsafePaste == handler.paste(req, &did));
    }
    ASSERT_TRUE(0 == PC::write_count);

    {
        Handler::Paste req;
        req.contents = memContents(items, 1);
        req.allow_unsafe = true;
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::none == handler.paste(req, &did));
        ASSERT_TRUE(did);
    }
    EXPECT_STR("rm -rf /\r", PC::written);

    handler.deinit();
}

TEST(stream_terminal, paste__bracketed_paste_frames_the_text) {
    TERM(t, 80, 24);

    PC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &PC::writePty;
    t.modes.set(modes::Mode::bracketed_paste, true);

    /* Newlines are safe inside the frame and are preserved. */
    {
        static const clip::Content items[] = {
            clip::Content(osc::ZStr("text/plain", 10), osc::ZStr("hello\nworld", 11))};
        Handler::Paste req;
        req.contents = memContents(items, 1);
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::none == handler.paste(req, &did));
        ASSERT_TRUE(did);
    }
    EXPECT_STR("\x1b[200~hello\nworld\x1b[201~", PC::written);
    ASSERT_TRUE(1 == PC::write_count);

    /* The frame terminator is not. */
    PC::reset();
    static const clip::Content term_items[] = {
        clip::Content(osc::ZStr("text/plain", 10), osc::ZStr("he\x1b[201~llo", 11))};
    {
        Handler::Paste req;
        req.contents = memContents(term_items, 1);
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::UnsafePaste == handler.paste(req, &did));
    }
    ASSERT_TRUE(0 == PC::write_count);

    /* Allowed, the stripper still defuses it: ESC becomes a space. */
    {
        Handler::Paste req;
        req.contents = memContents(term_items, 1);
        req.allow_unsafe = true;
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::none == handler.paste(req, &did));
        ASSERT_TRUE(did);
    }
    EXPECT_STR("\x1b[200~he [201~llo\x1b[201~", PC::written);

    handler.deinit();
}

TEST(stream_terminal, paste__no_text_representation_writes_nothing) {
    TERM(t, 80, 24);

    PC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &PC::writePty;

    {
        static const clip::Content items[] = {
            clip::Content(osc::ZStr("image/png", 9), osc::ZStr("\x89PNG", 4))};
        Handler::Paste req;
        req.contents = memContents(items, 1);
        bool did = true;
        ASSERT_TRUE(Handler::PasteError::none == handler.paste(req, &did));
        ASSERT_TRUE(!did);
    }
    {
        Handler::Paste req;
        req.contents = memContents(nullptr, 0);
        bool did = true;
        ASSERT_TRUE(Handler::PasteError::none == handler.paste(req, &did));
        ASSERT_TRUE(!did);
    }
    {
        static const clip::Content items[] = {
            clip::Content(osc::ZStr("text/plain", 10), osc::ZStr("", 0))};
        Handler::Paste req;
        req.contents = memContents(items, 1);
        bool did = true;
        ASSERT_TRUE(Handler::PasteError::none == handler.paste(req, &did));
        ASSERT_TRUE(!did);
    }
    ASSERT_TRUE(0 == PC::write_count);

    handler.deinit();
}

TEST(stream_terminal, paste__large_text_streams_to_the_pty_in_chunks) {
    TERM(t, 80, 24);

    PC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &PC::writePty;
    t.modes.set(modes::Mode::bracketed_paste, true);

    /* Two full chunks and a partial one with the frame, never the
     * whole thing at once. */
    const std::string data(10000, 'x');
    {
        const clip::Content items[] = {
            clip::Content(osc::ZStr("text/plain", 10), osc::ZStr(data.data(), data.size()))};
        Handler::Paste req;
        req.contents = memContents(items, 1);
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::none == handler.paste(req, &did));
        ASSERT_TRUE(did);
    }
    const size_t total = data.size() + strlen("\x1b[200~\x1b[201~");
    ASSERT_TRUE((total + Handler::paste_chunk_size - 1) / Handler::paste_chunk_size == PC::write_count);
    ASSERT_TRUE(total == PC::written.size());
    ASSERT_TRUE(0 == PC::written.compare(0, 9, "\x1b[200~xxx"));
    ASSERT_TRUE(0 == PC::written.compare(PC::written.size() - 9, 9, "xxx\x1b[201~"));

    handler.deinit();
}

/* A paste contents reader for the tests: serves fixed data per MIME
 * type in pieces, counting the reads of each representation. */
struct PasteReader {
    const osc::ZStr *mimes;
    size_t mimes_len;
    const osc::ZStr *data;
    size_t piece;
    size_t reads[4];
    /* Fail after this many bytes of a read. */
    bool has_fail_after;
    size_t fail_after;

    PasteReader()
        : mimes(nullptr), mimes_len(0), data(nullptr), piece(3), has_fail_after(false),
          fail_after(0) {
        for (size_t i = 0; i < 4; i++) reads[i] = 0;
    }

    pp::Contents contents() {
        pp::Contents c;
        c.tag = pp::Contents::Tag::reader;
        c.reader.mimes = mimes;
        c.reader.mimes_len = mimes_len;
        c.reader.read.ctx = this;
        c.reader.read.read_fn = &readFn;
        return c;
    }

    static clip::MimeReader::Error readFn(void *ctx, osc::ZStr mime, std::string *sink) {
        PasteReader *self = (PasteReader *)ctx;
        size_t index = self->mimes_len;
        for (size_t i = 0; i < self->mimes_len; i++) {
            if (self->mimes[i].len == mime.len &&
                memcmp(self->mimes[i].ptr, mime.ptr, mime.len) == 0) {
                index = i;
                break;
            }
        }
        if (index == self->mimes_len) return clip::MimeReader::Error::ReadFailed;
        self->reads[index] += 1;
        const osc::ZStr d = self->data[index];
        size_t offset = 0;
        while (offset < d.len) {
            if (self->has_fail_after && offset >= self->fail_after)
                return clip::MimeReader::Error::ReadFailed;
            const size_t n = self->piece < d.len - offset ? self->piece : d.len - offset;
            sink->append(d.ptr + offset, n);
            offset += n;
        }
        return clip::MimeReader::Error::none;
    }
};

TEST(stream_terminal, paste__reader_contents_are_read_on_demand) {
    TERM(t, 80, 24);

    PC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &PC::writePty;
    handler.effects.clipboard_read = &PC::clipboardRead;

    /* Unsafe text is refused from one read with nothing written; the
     * image is never read. */
    static const osc::ZStr two_mimes[] = {osc::ZStr("image/png", 9), osc::ZStr("text/plain", 10)};
    static const osc::ZStr two_data[] = {osc::ZStr("\x89PNG", 4),
                                         osc::ZStr("echo hi\nrm -rf /\n", 17)};
    PasteReader reader;
    reader.mimes = two_mimes;
    reader.mimes_len = 2;
    reader.data = two_data;
    {
        Handler::Paste req;
        req.contents = reader.contents();
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::UnsafePaste == handler.paste(req, &did));
    }
    ASSERT_TRUE(0 == PC::write_count);
    ASSERT_TRUE(0 == reader.reads[0]);
    ASSERT_TRUE(1 == reader.reads[1]);

    /* Allowed, the text is read once, encoded. */
    reader.reads[0] = 0;
    reader.reads[1] = 0;
    {
        Handler::Paste req;
        req.contents = reader.contents();
        req.allow_unsafe = true;
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::none == handler.paste(req, &did));
        ASSERT_TRUE(did);
    }
    EXPECT_STR("echo hi\rrm -rf /\r", PC::written);
    ASSERT_TRUE(0 == reader.reads[0]);
    ASSERT_TRUE(1 == reader.reads[1]);

    /* Safe text is read once too: buffered for the check, then written. */
    PC::reset();
    static const osc::ZStr plain_mime[] = {osc::ZStr("text/plain", 10)};
    static const osc::ZStr hello_data[] = {osc::ZStr("hello world", 11)};
    reader = PasteReader();
    reader.mimes = plain_mime;
    reader.mimes_len = 1;
    reader.data = hello_data;
    {
        Handler::Paste req;
        req.contents = reader.contents();
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::none == handler.paste(req, &did));
        ASSERT_TRUE(did);
    }
    EXPECT_STR("hello world", PC::written);
    ASSERT_TRUE(1 == reader.reads[0]);

    /* Empty text is nothing to paste, found on the one read. */
    PC::reset();
    static const osc::ZStr empty_data[] = {osc::ZStr("", 0)};
    reader = PasteReader();
    reader.mimes = plain_mime;
    reader.mimes_len = 1;
    reader.data = empty_data;
    {
        Handler::Paste req;
        req.contents = reader.contents();
        bool did = true;
        ASSERT_TRUE(Handler::PasteError::none == handler.paste(req, &did));
        ASSERT_TRUE(!did);
    }
    ASSERT_TRUE(0 == PC::write_count);
    ASSERT_TRUE(1 == reader.reads[0]);

    /* A paste event lists the types and reads nothing at all. */
    PC::reset();
    t.modes.set(modes::Mode::kitty_paste_events, true);
    static const osc::ZStr ev_mimes[] = {osc::ZStr("text/plain", 10), osc::ZStr("image/png", 9)};
    static const osc::ZStr ev_data[] = {osc::ZStr("secret", 6), osc::ZStr("\x89PNG", 4)};
    reader = PasteReader();
    reader.mimes = ev_mimes;
    reader.mimes_len = 2;
    reader.data = ev_data;
    {
        Handler::Paste req;
        req.contents = reader.contents();
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::none == handler.paste(req, &did));
        ASSERT_TRUE(did);
    }
    ASSERT_TRUE(PC::written.find(";dGV4dC9wbGFpbiBpbWFnZS9wbmcK\x1b\\") != std::string::npos);
    ASSERT_TRUE(PC::written.find("secret") == std::string::npos);
    ASSERT_TRUE(0 == reader.reads[0]);
    ASSERT_TRUE(0 == reader.reads[1]);

    handler.deinit();
}

TEST(stream_terminal, paste__reader_failure_writes_nothing) {
    TERM(t, 80, 24);

    PC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &PC::writePty;
    t.modes.set(modes::Mode::bracketed_paste, true);

    /* The read is buffered whole before anything is written, so a
     * mid-read failure discards the buffer, checked or not. */
    static const osc::ZStr plain_mime[] = {osc::ZStr("text/plain", 10)};
    static const osc::ZStr hello_data[] = {osc::ZStr("hello world", 11)};
    PasteReader reader;
    reader.mimes = plain_mime;
    reader.mimes_len = 1;
    reader.data = hello_data;
    reader.has_fail_after = true;
    reader.fail_after = 6;

    {
        Handler::Paste req;
        req.contents = reader.contents();
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::ReadFailed == handler.paste(req, &did));
    }
    ASSERT_TRUE(0 == PC::write_count);

    {
        Handler::Paste req;
        req.contents = reader.contents();
        req.allow_unsafe = true;
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::ReadFailed == handler.paste(req, &did));
    }
    ASSERT_TRUE(0 == PC::write_count);

    handler.deinit();
}


/* Wisp: `std.mem.count` over the response captures. */
static size_t countOccurrences(const std::string &haystack, const char *needle) {
    const size_t n = strlen(needle);
    size_t count = 0;
    size_t pos = 0;
    while (true) {
        const size_t found = haystack.find(needle, pos);
        if (found == std::string::npos) break;
        count += 1;
        pos = found + n;
    }
    return count;
}

TEST(stream_terminal, paste__mode_5522_sends_an_event_the_program_can_read_with) {
    TERM(t, 80, 24);

    PC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &PC::writePty;
    handler.effects.clipboard_read = &PC::clipboardRead;
    StreamHolder sh(handler);
    Stream &s = sh.s;
    t.modes.set(modes::Mode::kitty_paste_events, true);
    t.modes.set(modes::Mode::bracketed_paste, true);

    /* Every representation is listed, the data is never written, and
     * the one-time password rides on every packet. */
    {
        static const clip::Content items[] = {
            clip::Content(osc::ZStr("text/plain", 10), osc::ZStr("secret", 6)),
            clip::Content(osc::ZStr("image/png", 9), osc::ZStr("", 0))};
        Handler::Paste req;
        req.contents = memContents(items, 2);
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::none == s.handler.paste(req, &did));
        ASSERT_TRUE(did);
    }
    ASSERT_TRUE(1 == PC::write_count);
    ASSERT_TRUE(3 == countOccurrences(PC::written, "\x1b]5522;"));
    ASSERT_TRUE(PC::written.find("secret") == std::string::npos);
    ASSERT_TRUE(PC::written.find("\x1b[200~") == std::string::npos);

    /* OK packet: parse the (base64) password out. */
    static const char ok_prefix[] = "\x1b]5522;type=read:status=OK:pw=";
    const size_t ok_prefix_len = strlen(ok_prefix);
    ASSERT_TRUE(0 == PC::written.compare(0, ok_prefix_len, ok_prefix));
    const size_t pw_end = PC::written.find("\x1b\\", ok_prefix_len);
    ASSERT_TRUE(pw_end != std::string::npos);
    /* Copied out since the capture buffer is reused below. */
    const std::string pw_b64 = PC::written.substr(ok_prefix_len, pw_end - ok_prefix_len);
    ASSERT_TRUE(zigstd::base64::calcSize(kclip::otp_len) == pw_b64.size());

    /* Listing packet: base64 of "text/plain image/png\n". */
    {
        std::string expected;
        expected += "\x1b]5522;type=read:status=OK:pw=";
        expected += pw_b64;
        expected += "\x1b\\";
        expected += "\x1b]5522;type=read:status=DATA:mime=Lg==:pw=";
        expected += pw_b64;
        expected += ";dGV4dC9wbGFpbiBpbWFnZS9wbmcK\x1b\\";
        expected += "\x1b]5522;type=read:status=DONE:pw=";
        expected += pw_b64;
        expected += "\x1b\\";
        ASSERT_TRUE(expected == PC::written);
    }

    /* The program reads with the password and the name "Paste event":
     * the read arrives granted, exactly once. */
    std::string read;
    read += "\x1b]5522;type=read:pw=";
    read += pw_b64;
    read += ":name=UGFzdGUgZXZlbnQ=;dGV4dC9wbGFpbg==\x1b\\";
    PC::reset();
    s.nextSlice(read.data(), read.size());
    ASSERT_TRUE(1 == PC::read_count);
    ASSERT_TRUE(PC::last_read_granted);
    EXPECT_STR("Paste event", PC::readName());
    EXPECT_STR("\x1b]5522;type=read:status=OK\x1b\\"
               "\x1b]5522;type=read:status=DATA:mime=dGV4dC9wbGFpbg==;R2hvc3R0eQ==\x1b\\"
               "\x1b]5522;type=read:status=DONE\x1b\\",
               PC::written);

    /* The password was one-time: a second read is not granted. */
    PC::reset();
    s.nextSlice(read.data(), read.size());
    ASSERT_TRUE(1 == PC::read_count);
    ASSERT_TRUE(!PC::last_read_granted);
    ASSERT_TRUE(0 == PC::read_granted_count);

    /* Every event mints a fresh password. */
    PC::reset();
    {
        static const clip::Content items[] = {
            clip::Content(osc::ZStr("text/plain", 10), osc::ZStr("secret", 6))};
        Handler::Paste req;
        req.contents = memContents(items, 1);
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::none == s.handler.paste(req, &did));
        ASSERT_TRUE(did);
    }
    ASSERT_TRUE(PC::written.find(pw_b64) == std::string::npos);
}

TEST(stream_terminal, paste__mode_5522_reports_the_selection_as_primary) {
    TERM(t, 80, 24);

    PC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &PC::writePty;
    handler.effects.clipboard_read = &PC::clipboardRead;
    t.modes.set(modes::Mode::kitty_paste_events, true);

    static const clip::Content items[] = {
        clip::Content(osc::ZStr("text/plain", 10), osc::ZStr("x", 1))};
    static const clip::Location locations[] = {clip::Location::primary, clip::Location::selection};

    for (size_t i = 0; i < 2; i++) {
        PC::reset();
        Handler::Paste req;
        req.source = pp::Source::makeClipboard(locations[i]);
        req.contents = memContents(items, 1);
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::none == handler.paste(req, &did));
        ASSERT_TRUE(did);
        static const char want_prefix[] = "\x1b]5522;type=read:status=OK:loc=primary:pw=";
        ASSERT_TRUE(0 == PC::written.compare(0, strlen(want_prefix), want_prefix));
        /* Only on the OK packet. */
        ASSERT_TRUE(1 == countOccurrences(PC::written, "loc=primary"));
    }

    PC::reset();
    {
        Handler::Paste req;
        req.source = pp::Source::makeClipboard(clip::Location::standard);
        req.contents = memContents(items, 1);
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::none == handler.paste(req, &did));
        ASSERT_TRUE(did);
    }
    ASSERT_TRUE(PC::written.find("loc=") == std::string::npos);

    handler.deinit();
}

TEST(stream_terminal, paste__mode_5522_without_clipboard_read_pastes_text) {
    TERM(t, 80, 24);

    PC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &PC::writePty;
    t.modes.set(modes::Mode::kitty_paste_events, true);

    {
        static const clip::Content items[] = {
            clip::Content(osc::ZStr("text/plain", 10), osc::ZStr("hello", 5))};
        Handler::Paste req;
        req.contents = memContents(items, 1);
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::none == handler.paste(req, &did));
        ASSERT_TRUE(did);
    }
    EXPECT_STR("hello", PC::written);
    ASSERT_TRUE(0 == handler.kitty_clipboard_grants.entries_len);

    handler.deinit();
}

TEST(stream_terminal, paste__text_source_never_becomes_an_event) {
    TERM(t, 80, 24);

    PC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &PC::writePty;
    handler.effects.clipboard_read = &PC::clipboardRead;
    t.modes.set(modes::Mode::kitty_paste_events, true);

    {
        static const clip::Content items[] = {
            clip::Content(osc::ZStr("text/plain", 10), osc::ZStr("committed", 9))};
        Handler::Paste req;
        req.source = pp::Source::makeText();
        req.contents = memContents(items, 1);
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::none == handler.paste(req, &did));
        ASSERT_TRUE(did);
    }
    EXPECT_STR("committed", PC::written);
    ASSERT_TRUE(0 == handler.kitty_clipboard_grants.entries_len);

    handler.deinit();
}

/* Wisp: upstream passes `std.Io.failing` as the terminal's Io; here the
 * entropy source is the `sys::random_secure` override hook. */
static wisp::terminal::sys::RandomSecureError failingRandomSecure(uint8_t *, size_t) {
    return wisp::terminal::sys::RandomSecureError::EntropyUnavailable;
}

TEST(stream_terminal, paste__mode_5522_without_entropy_fails_and_records_no_grant) {
    TERM(t, 80, 24);

    PC::reset();

    wisp::terminal::sys::RandomSecureFn saved = wisp::terminal::sys::random_secure();
    wisp::terminal::sys::random_secure() = &failingRandomSecure;

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &PC::writePty;
    handler.effects.clipboard_read = &PC::clipboardRead;
    t.modes.set(modes::Mode::kitty_paste_events, true);

    {
        static const clip::Content items[] = {
            clip::Content(osc::ZStr("text/plain", 10), osc::ZStr("secret", 6))};
        Handler::Paste req;
        req.contents = memContents(items, 1);
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::EntropyUnavailable == handler.paste(req, &did));
    }
    ASSERT_TRUE(0 == PC::write_count);
    ASSERT_TRUE(0 == handler.kitty_clipboard_grants.entries_len);

    /* Text pastes need no entropy and still work. */
    {
        static const clip::Content items[] = {
            clip::Content(osc::ZStr("text/plain", 10), osc::ZStr("hello", 5))};
        Handler::Paste req;
        req.source = pp::Source::makeText();
        req.contents = memContents(items, 1);
        bool did = false;
        ASSERT_TRUE(Handler::PasteError::none == handler.paste(req, &did));
        ASSERT_TRUE(did);
    }
    EXPECT_STR("hello", PC::written);

    handler.deinit();
    wisp::terminal::sys::random_secure() = saved;
}

/* Wisp: upstream's "paste: mode 5522 DECRQM requires clipboard read effect"
 * is gated on `build_options.artifact == .lib`. Wisp is an application, so
 * mode 5522 is disabled in modes.hpp and cannot be set by DECSET; the test
 * is not ported. */

TEST(stream_terminal, full_reset_drops_kitty_clipboard_grants) {
    TERM(t, 80, 24);

    PC::reset();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &PC::writePty;
    handler.effects.clipboard_read = &PC::clipboardRead;
    StreamHolder sh(handler);
    Stream &s = sh.s;

    ASSERT_TRUE(s.handler.kitty_clipboard_grants.grant(
        talloc(), (const uint8_t *)"pw", 2, kclip::Grants::Direction::read, false));
    ASSERT_TRUE(1 == s.handler.kitty_clipboard_grants.entries_len);

    s.nextSlice("\x1B" "c");
    ASSERT_TRUE(0 == s.handler.kitty_clipboard_grants.entries_len);

    /* A read with the old password is no longer granted, and the
     * handler keeps working (grants can be recorded again). */
    PC::reset();
    s.nextSlice("\x1b]5522;type=read:pw=cHc=:name=YXBw;dGV4dC9wbGFpbg==\x1b\\");
    ASSERT_TRUE(1 == PC::read_count);
    ASSERT_TRUE(!PC::last_read_granted);
    ASSERT_TRUE(s.handler.kitty_clipboard_grants.grant(
        talloc(), (const uint8_t *)"pw", 2, kclip::Grants::Direction::read, false));
}

TEST(stream_terminal, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
