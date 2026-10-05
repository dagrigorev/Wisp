/* Transliterated from the test blocks in Ghostty src/terminal/Terminal.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names. Built with
 * WISP_IS_TEST and WISP_SLOW_RUNTIME_SAFETY, as upstream's tests run.
 *
 * Wisp: the tests upstream gates on build_options.kitty_graphics or
 * build_options.glyph_protocol are not ported, matching the build
 * configuration of src/vt/terminal.hpp.
 */

#include <string.h>

#include <string>

#include "test_helpers.h"
#include "res/glitch.inc"
#include "../vt/terminal.hpp"
#include "../vt/formatter.hpp"
#include "../zigstd/random.hpp"

using namespace wisp;
using namespace wisp::vt;

typedef page::Page Page;
typedef page::Row Row;
typedef page::Cell Cell;
typedef point::Point Point;
typedef PageList::Pin Pin;
typedef terminal::osc::semantic_prompt::Command SemanticPromptCommand;
typedef terminal::sgr::Attribute::Tag A;

static zigstd::Allocator talloc() { return zigstd::testing_allocator(); }

/* Wisp: var t = try init(io, alloc, .{...}); defer t.deinit(alloc); */
struct TermHolder {
    Terminal t;
    bool ok;
    zigstd::Allocator alloc;
    explicit TermHolder(const Terminal::Options &o) : alloc(talloc()) { ok = Terminal::init(alloc, o, &t); }
    TermHolder(zigstd::Allocator a, const Terminal::Options &o) : alloc(a) { ok = Terminal::init(alloc, o, &t); }
    bool reinit(const Terminal::Options &o) {
        if (ok) t.deinit(alloc);
        ok = Terminal::init(alloc, o, &t);
        return ok;
    }
    ~TermHolder() {
        if (ok) t.deinit(alloc);
    }
};

#define TERM(v, c, r)                                                                                                      Terminal::Options v##_opts((size::CellCountInt)(c), (size::CellCountInt)(r));                                          TermHolder v##_holder(v##_opts);                                                                                       ASSERT_TRUE(v##_holder.ok);                                                                                            Terminal &v = v##_holder.t


#define TERM_A(v, a, c, r)                                                                                                 Terminal::Options v##_opts((size::CellCountInt)(c), (size::CellCountInt)(r));                                           TermHolder v##_holder((a), v##_opts);                                                                                   ASSERT_TRUE(v##_holder.ok);                                                                                             Terminal &v = v##_holder.t

/* Wisp: for ("abc") |c| try t.print(c); */
#define PRINT_EACH(t, str)                                                                                                 do {                                                                                                                       const char *_s = (str);                                                                                                for (size_t _i = 0; _s[_i] != 0; _i++) ASSERT_TRUE((t).print((unsigned char)_s[_i]));                              } while (0)

#define EXPECT_STR(expected, actual)                                                                                       do {                                                                                                                       const std::string _actual = (actual);                                                                                  ASSERT_STR_EQ(expected, _actual.c_str());                                                                          } while (0)

#define NOERR(e) ASSERT_TRUE((e) == PageList::IncreaseCapacityError::none)

static terminal::sgr::Attribute attr(A t) { return terminal::sgr::Attribute::make(t); }

static PageList::IncreaseCapacityError startLink(Screen &s, const char *uri, const char *id = nullptr) {
    return s.startHyperlink((const uint8_t *)uri, strlen(uri), (const uint8_t *)id, id ? strlen(id) : 0);
}

static SemanticPromptCommand semanticPromptCmd(SemanticPromptCommand::Action action, const char *options) {
    SemanticPromptCommand c = SemanticPromptCommand::init(action);
    c.options_unvalidated = terminal::osc::ZStr(options, strlen(options));
    return c;
}

/* Wisp: try testing.expectEqualSlices(u21, &.{...}, list_cell.node.page().lookupGrapheme(cell).?); */
#define EXPECT_GRAPHEMES(list_cell, cell, ...)                                                                             do {                                                                                                                       static const uint32_t _want[] = {__VA_ARGS__};                                                                         const size_t _want_len = sizeof(_want) / sizeof(_want[0]);                                                             size_t _got_len = 0;                                                                                                   const uint32_t *_got = (list_cell).node->page()->lookupGrapheme((cell), &_got_len);                                    ASSERT_TRUE(_got != nullptr);                                                                                          ASSERT_TRUE(_want_len == _got_len);                                                                                    for (size_t _i = 0; _i < _want_len; _i++) ASSERT_TRUE(_want[_i] == _got[_i]);                                      } while (0)

/* Wisp: fn testPrintSliceDifferential(io, alloc, rand, ops, cols, rows) */
static void testPrintSliceDifferential(zigstd::Random rand, size_t ops, size::CellCountInt cols,
                                       size::CellCountInt rows) {
    TERM(t1, cols, rows);
    TERM(t2, cols, rows);

    /* Alphabet of interesting codepoints: ascii, latin-1, combining
     * marks, CJK (wide), emoji (wide), ZWJ, variation selectors. */
    static const uint32_t alphabet[] = {
        'a',     'b',     'Z',    '0',    ' ',     0x10,   0x1F,   0x7F,
        0xE9,
        0xFF,    0x301,   0x4E00, 0x4E01, 0x1F600, 0x200D, 0xFE0F, 'x',
        'y',     0x1F9D1, 0x0308, 0xAD,   0x3042,  0xAC00, 'q',    'r',
        's',     't',     'u',    'v',    'w',     '1',    '2',    0x1F1E6,
        0x1F1E7, 0x1100,  0x1161, 0x11A8, 0x200C,  0x0430, 0x03B1,
    };
    const size_t alphabet_len = sizeof(alphabet) / sizeof(alphabet[0]);

    uint32_t cps_buf[64];
    const size_t cps_buf_len = sizeof(cps_buf) / sizeof(cps_buf[0]);
    size_t last_n = 0;
    (void)last_n;

    for (size_t op_i = 0; op_i < ops; op_i++) {
        const uint8_t op = rand.intRangeAtMost<uint8_t>(0, 20);
        if (op <= 9) {
            /* Print a run of codepoints (most common op). */
            const size_t n = rand.intRangeAtMost<size_t>(1, cps_buf_len);
            last_n = n;
            for (size_t i = 0; i < n; i++) cps_buf[i] = alphabet[rand.intRangeLessThan<size_t>(0, alphabet_len)];

            /* t1: per-codepoint print */
            for (size_t i = 0; i < n; i++) ASSERT_TRUE(t1.print(cps_buf[i]));

            /* t2: printSlice with random chunking */
            size_t i = 0;
            while (i < n) {
                const size_t chunk = rand.intRangeAtMost<size_t>(1, n - i);
                ASSERT_TRUE(t2.printSlice(cps_buf + i, chunk));
                i += chunk;
            }
        } else if (op == 10) {
            t1.carriageReturn();
            t2.carriageReturn();
            ASSERT_TRUE(t1.linefeed());
            ASSERT_TRUE(t2.linefeed());
        } else if (op == 11) {
            const size_t row = rand.intRangeAtMost<size_t>(1, rows);
            const size_t col = rand.intRangeAtMost<size_t>(1, cols);
            t1.setCursorPos(row, col);
            t2.setCursorPos(row, col);
        } else if (op == 12) {
            terminal::sgr::Attribute at;
            switch (rand.intRangeAtMost<uint8_t>(0, 3)) {
            case 0: at = terminal::sgr::Attribute::make(A::unset); break;
            case 1: at = terminal::sgr::Attribute::make(A::bold); break;
            case 2: {
                const uint8_t r = rand.int_<uint8_t>();
                const uint8_t g = rand.int_<uint8_t>();
                const uint8_t b = rand.int_<uint8_t>();
                at = terminal::sgr::Attribute::makeRgb(A::direct_color_fg, r, g, b);
                break;
            }
            default: at = terminal::sgr::Attribute::makeName(A::fg_8, (uint16_t)terminal::Name::red); break;
            }
            NOERR(t1.setAttribute(at));
            NOERR(t2.setAttribute(at));
        } else if (op == 13) {
            const bool v = rand.boolean();
            t1.modes.set(terminal::modes::Mode::insert, v);
            t2.modes.set(terminal::modes::Mode::insert, v);
        } else if (op == 14) {
            const bool v = rand.boolean();
            t1.modes.set(terminal::modes::Mode::wraparound, v);
            t2.modes.set(terminal::modes::Mode::wraparound, v);
        } else if (op == 15) {
            const bool v = rand.boolean();
            t1.modes.set(terminal::modes::Mode::grapheme_cluster, v);
            t2.modes.set(terminal::modes::Mode::grapheme_cluster, v);
        } else if (op == 16) {
            /* Margins. */
            t1.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
            t2.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
            const size_t left = rand.intRangeAtMost<size_t>(1, (size_t)cols / 2);
            const size_t right = rand.intRangeAtMost<size_t>((size_t)cols / 2, cols);
            t1.setLeftAndRightMargin(left, right);
            t2.setLeftAndRightMargin(left, right);
        } else if (op == 17) {
            t1.setLeftAndRightMargin(0, 0);
            t2.setLeftAndRightMargin(0, 0);
        } else if (op == 18) {
            NOERR(startLink(*t1.screens.active, "http://example.com"));
            NOERR(startLink(*t2.screens.active, "http://example.com"));
        } else if (op == 19) {
            t1.screens.active->endHyperlink();
            t2.screens.active->endHyperlink();
        } else {
            const terminal::charsets::Charset set =
                rand.boolean() ? terminal::charsets::Charset::dec_special : terminal::charsets::Charset::utf8;
            t1.configureCharset(terminal::charsets::Slots::G0, set);
            t2.configureCharset(terminal::charsets::Slots::G0, set);
        }

        /* Cursor state must match exactly after every op. */
        ASSERT_TRUE(t1.screens.active->cursor.x == t2.screens.active->cursor.x);
        ASSERT_TRUE(t1.screens.active->cursor.y == t2.screens.active->cursor.y);
        ASSERT_TRUE(t1.screens.active->cursor.pending_wrap == t2.screens.active->cursor.pending_wrap);

        /* Full screen contents must match after every op. */
        {
            const std::string str1 = t1.screens.active->dumpStringAlloc(Point::screen());
            const std::string str2 = t2.screens.active->dumpStringAlloc(Point::screen());
            ASSERT_STR_EQ(str1.c_str(), str2.c_str());
        }
    }

    /* Page integrity (styles refcounts, grapheme maps, etc.) must hold. */
    ASSERT_TRUE(t1.screens.active->cursor.page_pin->node->page()->verifyIntegrity() == page::PageError::none);
    ASSERT_TRUE(t2.screens.active->cursor.page_pin->node->page()->verifyIntegrity() == page::PageError::none);
}

static void expectGraphemeWidthParity(const uint32_t *cps, size_t cps_len) {
    TERM(t, 80, 5);

    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    size_t expected = 0;
    size_t i = 0;
    while (i < cps_len) {
        const unicode::GraphemeWidth result = unicode::graphemeWidth(cps + i, cps_len - i);
        ASSERT_TRUE(result.len > 0);
        i += result.len;
        expected += result.width;
    }

    for (size_t j = 0; j < cps_len; j++) ASSERT_TRUE(t.print(cps[j]));
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(expected == t.screens.active->cursor.x);
}

#define GW(...)                                                                                                            do {                                                                                                                       static const uint32_t _cps[] = {__VA_ARGS__};                                                                          expectGraphemeWidthParity(_cps, sizeof(_cps) / sizeof(_cps[0]));                                                   } while (0)

TEST(terminal, Terminal_forwards_optional_scrollback_limits) {
    const size_t max_lines = 123;
    TERM(t, 80, 24);
    t_opts.max_scrollback_bytes = Maybe<size_t>();
    ASSERT_TRUE(t_holder.reinit(t_opts));
    t_opts.max_scrollback_lines = Maybe<size_t>((size_t)max_lines);
    ASSERT_TRUE(t_holder.reinit(t_opts));

    ASSERT_TRUE(SIZE_MAX == t.screens.active->pages.limits.bytes.explicit_);
    ASSERT_TRUE(max_lines == t.screens.active->pages.limits.lines.explicit_);
}

TEST(terminal, Terminal_setScrollbackMaxBytes) {
    TERM(t, 80, 3);
    t_opts.max_scrollback_bytes = Maybe<size_t>();
    ASSERT_TRUE(t_holder.reinit(t_opts));

    Screen *primary = t.screens.get(ScreenSet::Key::primary);
    const size_t page_rows = primary->pages.pages.first->capacity().rows;

    /* Build several complete pages of history so lowering the byte limit has
     * existing allocations to prune immediately. */
    for (size_t i_ = (size_t)(0); i_ < (size_t)(4 * page_rows); i_++) ASSERT_TRUE(t.linefeed());
    const size_t old_page_size = primary->pages.page_size;
    t.setScrollbackMaxBytes(Maybe<size_t>((size_t)1));
    ASSERT_TRUE(1 == primary->pages.limits.bytes.explicit_);
    ASSERT_TRUE(primary->pages.page_size < old_page_size);
    ASSERT_TRUE(primary->pages.page_size <= primary->pages.limits.max(PageList::Limits::Key::bytes));
    ASSERT_TRUE(!primary->no_scrollback);

    /* Zero switches Screen behavior as well as PageList accounting, discards
     * all retained history, and prevents future linefeeds from recreating it. */
    t.setScrollbackMaxBytes(Maybe<size_t>((size_t)0));
    ASSERT_TRUE(0 == primary->pages.limits.bytes.explicit_);
    ASSERT_TRUE(primary->no_scrollback);
    ASSERT_TRUE(primary->pages.rows == primary->pages.total_rows);
    ASSERT_TRUE(primary->pages.viewport == PageList::Viewport::active);

    for (size_t i_ = (size_t)(0); i_ < (size_t)(page_rows); i_++) ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(primary->pages.rows == primary->pages.total_rows);

    /* Re-enabling unlimited scrollback affects subsequent output. */
    t.setScrollbackMaxBytes(Maybe<size_t>());
    ASSERT_TRUE(SIZE_MAX == primary->pages.limits.bytes.explicit_);
    ASSERT_TRUE(!primary->no_scrollback);
    for (size_t i_ = (size_t)(0); i_ < (size_t)(page_rows); i_++) ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(primary->pages.total_rows > primary->pages.rows);
}

TEST(terminal, Terminal_setScrollbackMaxLines) {
    TERM(t, 80, 3);
    t_opts.max_scrollback_bytes = Maybe<size_t>();
    ASSERT_TRUE(t_holder.reinit(t_opts));
    t_opts.max_scrollback_lines = Maybe<size_t>();
    ASSERT_TRUE(t_holder.reinit(t_opts));

    Screen *primary = t.screens.get(ScreenSet::Key::primary);
    const size_t page_rows = primary->pages.pages.first->capacity().rows;

    for (size_t i_ = (size_t)(0); i_ < (size_t)(4 * page_rows); i_++) ASSERT_TRUE(t.linefeed());
    const size_t old_total_rows = primary->pages.total_rows;
    t.setScrollbackMaxLines(Maybe<size_t>((size_t)page_rows));
    ASSERT_TRUE(page_rows == primary->pages.limits.lines.explicit_);
    ASSERT_TRUE(primary->pages.total_rows < old_total_rows);
    ASSERT_TRUE(!primary->pages.limits.exceeded(&primary->pages, PageList::Limits::Key::lines));

    const size_t limited_total_rows = primary->pages.total_rows;
    t.setScrollbackMaxLines(Maybe<size_t>());
    ASSERT_TRUE(SIZE_MAX == primary->pages.limits.lines.explicit_);
    ASSERT_TRUE(limited_total_rows == primary->pages.total_rows);
    for (size_t i_ = (size_t)(0); i_ < (size_t)(3 * page_rows); i_++) ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(primary->pages.total_rows - primary->pages.rows > page_rows);
}

TEST(terminal, Terminal_setScrollback_only_affects_primary_screen) {
    TERM(t, 80, 3);

    {
        bool ok = true;
        (void)t.switchScreen(ScreenSet::Key::alternate, &ok);
        ASSERT_TRUE(ok);
    }
    Screen *primary = t.screens.get(ScreenSet::Key::primary);
    Screen *alternate = t.screens.get(ScreenSet::Key::alternate);

    t.setScrollbackMaxBytes(Maybe<size_t>((size_t)123));
    t.setScrollbackMaxLines(Maybe<size_t>((size_t)456));

    ASSERT_TRUE(123 == primary->pages.limits.bytes.explicit_);
    ASSERT_TRUE(456 == primary->pages.limits.lines.explicit_);
    ASSERT_TRUE(!primary->no_scrollback);

    ASSERT_TRUE(0 == alternate->pages.limits.bytes.explicit_);
    ASSERT_TRUE(SIZE_MAX == alternate->pages.limits.lines.explicit_);
    ASSERT_TRUE(alternate->no_scrollback);
    ASSERT_TRUE(alternate == t.screens.active);
}

TEST(terminal, Terminal__resize_resets_synchronized_output) {
    TERM(t, 10, 5);

    t.modes.set(terminal::modes::Mode::synchronized_output, true);
    ASSERT_TRUE(t.resize(talloc(), Terminal::Resize(10, 5)) == Terminal::ResizeError::none);
    ASSERT_TRUE(!t.modes.get(terminal::modes::Mode::synchronized_output));
}

TEST(terminal, Terminal__resize_rejects_zero_dimensions_before_mutation) {
    TERM(t, 10, 5);

    t.width_px = 100;
    t.height_px = 100;
    t.flags.dirty.clear = false;

    {
        Terminal::Resize r(0, 5);
        Terminal::Resize::CellSize cs = {9, 18};
        r.cell_size_px = cs;
        ASSERT_TRUE(t.resize(talloc(), r) == Terminal::ResizeError::InvalidValue);
    }
    {
        Terminal::Resize r(10, 0);
        Terminal::Resize::CellSize cs = {9, 18};
        r.cell_size_px = cs;
        ASSERT_TRUE(t.resize(talloc(), r) == Terminal::ResizeError::InvalidValue);
    }

    ASSERT_TRUE(10 == t.cols);
    ASSERT_TRUE(5 == t.rows);
    ASSERT_TRUE(100 == t.width_px);
    ASSERT_TRUE(100 == t.height_px);
    ASSERT_TRUE(!t.flags.dirty.clear);
}

TEST(terminal, Terminal__resize_preserves_pixel_dimensions_when_omitted) {
    TERM(t, 10, 5);

    t.width_px = 90;
    t.height_px = 90;
    ASSERT_TRUE(t.resize(talloc(), Terminal::Resize(20, 10)) == Terminal::ResizeError::none);

    ASSERT_TRUE(90 == t.width_px);
    ASSERT_TRUE(90 == t.height_px);
}

TEST(terminal, Terminal__resize_updates_pixels_without_changing_cell_dimensions) {
    TERM(t, 10, 5);

    {
        Terminal::Resize r(10, 5);
        Terminal::Resize::CellSize cs = {9, 18};
        r.cell_size_px = cs;
        ASSERT_TRUE(t.resize(talloc(), r) == Terminal::ResizeError::none);
    }

    ASSERT_TRUE(90 == t.width_px);
    ASSERT_TRUE(90 == t.height_px);
}

TEST(terminal, Terminal__resize_pixel_dimensions_saturate) {
    TERM(t, 2, 3);

    {
        Terminal::Resize r(2, 3);
        Terminal::Resize::CellSize cs = {UINT32_MAX, UINT32_MAX};
        r.cell_size_px = cs;
        ASSERT_TRUE(t.resize(talloc(), r) == Terminal::ResizeError::none);
    }

    ASSERT_TRUE(UINT32_MAX == t.width_px);
    ASSERT_TRUE(UINT32_MAX == t.height_px);
}

TEST(terminal, Terminal__resize_preserves_tabstops_on_allocation_failure) {
    zigstd::FailingAllocator failing(talloc(), SIZE_MAX);
    const zigstd::Allocator alloc = failing.allocator();
    TERM_A(t, alloc, 10, 1);

    failing.fail_index = failing.alloc_index;
    {
        Terminal::Resize r(513, 1);
        ASSERT_TRUE(t.resize(alloc, r) == Terminal::ResizeError::OutOfMemory);
    }

    ASSERT_TRUE(10 == t.cols);
    ASSERT_TRUE(t.tabstops.get(8));
}

TEST(terminal, Terminal__resize_failure_paths_preserve_consistent_state) {
    typedef Terminal::resize_tw tw;
    const Terminal::ResizeTw tags[] = {Terminal::ResizeTw::tabstops, Terminal::ResizeTw::primary_screen,
                                       Terminal::ResizeTw::alternate_screen};
    for (size_t ti = 0; ti < 3; ti++) {
        const Terminal::ResizeTw tag = tags[ti];
        struct TwEnd {
            ~TwEnd() { (void)tw::end(tripwire::ResetMode::reset); }
        } tw_end;
        (void)tw_end;

        TERM(t, 10, 3);

        ASSERT_TRUE(t.printString("primary"));
        {
            bool ok = true;
            (void)t.switchScreen(ScreenSet::Key::alternate, &ok);
            ASSERT_TRUE(ok);
        }
        ASSERT_TRUE(t.printString("alternate"));
        {
            bool ok = true;
            (void)t.switchScreen(ScreenSet::Key::primary, &ok);
            ASSERT_TRUE(ok);
        }

        t.width_px = 100;
        t.height_px = 50;
        t.modes.set(terminal::modes::Mode::synchronized_output, true);
        t.flags.dirty.clear = false;
        t.scrolling_region.top = 1;
        t.scrolling_region.bottom = 2;
        t.scrolling_region.left = 1;
        t.scrolling_region.right = 8;
        t.tabstops.unset(8);
        t.tabstops.set(3);

        Screen *primary = t.screens.get(ScreenSet::Key::primary);
        Screen *alternate = t.screens.get(ScreenSet::Key::alternate);
        const size_t alternate_generation = t.screens.generation(ScreenSet::Key::alternate);
        ASSERT_TRUE(primary->pages.pages.first == primary->pages.pages.last);
        ASSERT_TRUE(alternate->pages.pages.first == alternate->pages.pages.last);

        const Terminal before = t;
        const Screen before_primary = *primary;
        const Screen before_alternate = *alternate;
        const std::string before_primary_page((const char *)primary->pages.pages.first->page()->memory,
                                              primary->pages.pages.first->page()->memory_len);
        const std::string before_alternate_page((const char *)alternate->pages.pages.first->page()->memory,
                                                alternate->pages.pages.first->page()->memory_len);
        tw::errorAlways(tag, Terminal::ResizeError::OutOfMemory);

        /* A failure after the primary screen has resized is recovered by
         * dropping the inactive alternate and completing the resize. This
         * also proves the alternate tripwire is after the primary resize
         * rather than acting as a preflight check. */
        if (tag == Terminal::ResizeTw::alternate_screen) {
            {
                Terminal::Resize r(513, 4);
                Terminal::Resize::CellSize cs = {9, 18};
                r.cell_size_px = cs;
                ASSERT_TRUE(t.resize(talloc(), r) == Terminal::ResizeError::none);
            }

            ASSERT_TRUE(513 == t.cols);
            ASSERT_TRUE(4 == t.rows);
            ASSERT_TRUE(4617 == t.width_px);
            ASSERT_TRUE(72 == t.height_px);
            ASSERT_TRUE(!t.modes.get(terminal::modes::Mode::synchronized_output));
            ASSERT_TRUE(t.flags.dirty.clear);
            ASSERT_TRUE(513 == primary->pages.cols);
            ASSERT_TRUE(4 == primary->pages.rows);
            ASSERT_TRUE(nullptr == t.screens.get(ScreenSet::Key::alternate));
            ASSERT_TRUE(alternate_generation + 1 == t.screens.generation(ScreenSet::Key::alternate));
            ASSERT_TRUE(ScreenSet::Key::primary == t.screens.active_key);
            ASSERT_TRUE(primary == t.screens.active);
            ASSERT_TRUE(t.tabstops.get(8));
            ASSERT_TRUE(!t.tabstops.get(3));

            /* The alternate is recreated lazily at the new terminal size. */
            {
                bool ok = true;
                (void)t.switchScreen(ScreenSet::Key::alternate, &ok);
                ASSERT_TRUE(ok);
            }
            Screen *replacement = t.screens.get(ScreenSet::Key::alternate);
            ASSERT_TRUE(513 == replacement->pages.cols);
            ASSERT_TRUE(4 == replacement->pages.rows);
            ASSERT_TRUE(replacement->pages.getCell(Point::active()).value.cell->isEmpty());
            continue;
        }

        {
            Terminal::Resize r(513, 4);
            Terminal::Resize::CellSize cs = {9, 18};
            r.cell_size_px = cs;
            ASSERT_TRUE(t.resize(talloc(), r) == Terminal::ResizeError::OutOfMemory);
        }

        ASSERT_TRUE(before.width_px == t.width_px);
        ASSERT_TRUE(before.height_px == t.height_px);
        ASSERT_TRUE(memcmp(&before.modes, &t.modes, sizeof(t.modes)) == 0);
        ASSERT_TRUE(before.cols == t.cols);
        ASSERT_TRUE(before.rows == t.rows);
        ASSERT_TRUE(memcmp(&before.scrolling_region, &t.scrolling_region, sizeof(t.scrolling_region)) == 0);
        ASSERT_TRUE(memcmp(&before.flags, &t.flags, sizeof(t.flags)) == 0);
        ASSERT_TRUE(memcmp(&before.tabstops, &t.tabstops, sizeof(t.tabstops)) == 0);
        ASSERT_TRUE(before.screens.active_key == t.screens.active_key);
        ASSERT_TRUE(before.screens.active == t.screens.active);

        ASSERT_TRUE(before_primary.pages.cols == primary->pages.cols);
        ASSERT_TRUE(before_primary.pages.rows == primary->pages.rows);
        ASSERT_TRUE(before_primary.pages.total_rows == primary->pages.total_rows);
        ASSERT_TRUE(memcmp(&before_primary.cursor, &primary->cursor, sizeof(primary->cursor)) == 0);
        ASSERT_TRUE(before_primary_page.size() == primary->pages.pages.first->page()->memory_len);
        ASSERT_TRUE(memcmp(before_primary_page.data(), primary->pages.pages.first->page()->memory,
                           before_primary_page.size()) == 0);

        ASSERT_TRUE(before_alternate.pages.cols == alternate->pages.cols);
        ASSERT_TRUE(before_alternate.pages.rows == alternate->pages.rows);
        ASSERT_TRUE(before_alternate.pages.total_rows == alternate->pages.total_rows);
        ASSERT_TRUE(memcmp(&before_alternate.cursor, &alternate->cursor, sizeof(alternate->cursor)) == 0);
        ASSERT_TRUE(before_alternate_page.size() == alternate->pages.pages.first->page()->memory_len);
        ASSERT_TRUE(memcmp(before_alternate_page.data(), alternate->pages.pages.first->page()->memory,
                           before_alternate_page.size()) == 0);
    }
}

TEST(terminal, Terminal__alternate_resize_failure_replaces_active_alternate_screen) {
    typedef Terminal::resize_tw tw;
    struct TwEnd {
        ~TwEnd() { (void)tw::end(tripwire::ResetMode::reset); }
    } tw_end;
    (void)tw_end;

    TERM(t, 10, 3);

    {
        bool ok = true;
        (void)t.switchScreen(ScreenSet::Key::alternate, &ok);
        ASSERT_TRUE(ok);
    }
    ASSERT_TRUE(ScreenSet::Key::alternate == t.screens.active_key);
    t.screens.active->charset.gl = terminal::charsets::Slots::G1;
    ASSERT_TRUE(t.printString("alternate"));
    const size_t generation = t.screens.generation(ScreenSet::Key::alternate);

    tw::errorAlways(Terminal::ResizeTw::alternate_screen, Terminal::ResizeError::OutOfMemory);
    ASSERT_TRUE(t.resize(talloc(), Terminal::Resize(20, 4)) == Terminal::ResizeError::none);

    Screen *alternate = t.screens.get(ScreenSet::Key::alternate);
    ASSERT_TRUE(ScreenSet::Key::alternate == t.screens.active_key);
    ASSERT_TRUE(alternate == t.screens.active);
    ASSERT_TRUE(20 == alternate->pages.cols);
    ASSERT_TRUE(4 == alternate->pages.rows);
    ASSERT_TRUE(alternate->pages.getCell(Point::active()).value.cell->isEmpty());
    ASSERT_TRUE(terminal::charsets::Slots::G1 == alternate->charset.gl);
    ASSERT_TRUE(generation + 1 == t.screens.generation(ScreenSet::Key::alternate));
}

TEST(terminal, Terminal__alternate_resize_replacement_failure_falls_back_to_primary) {
    typedef Terminal::resize_tw tw;
    struct TwEnd {
        ~TwEnd() { (void)tw::end(tripwire::ResetMode::reset); }
    } tw_end;
    (void)tw_end;

    TERM(t, 10, 3);

    {
        bool ok = true;
        (void)t.switchScreen(ScreenSet::Key::alternate, &ok);
        ASSERT_TRUE(ok);
    }
    tw::errorAlways(Terminal::ResizeTw::alternate_screen, Terminal::ResizeError::OutOfMemory);
    tw::errorAlways(Terminal::ResizeTw::alternate_screen_init, Terminal::ResizeError::OutOfMemory);
    ASSERT_TRUE(t.resize(talloc(), Terminal::Resize(20, 4)) == Terminal::ResizeError::none);

    Screen *primary = t.screens.get(ScreenSet::Key::primary);
    ASSERT_TRUE(nullptr == t.screens.get(ScreenSet::Key::alternate));
    ASSERT_TRUE(ScreenSet::Key::primary == t.screens.active_key);
    ASSERT_TRUE(primary == t.screens.active);
    ASSERT_TRUE(20 == primary->pages.cols);
    ASSERT_TRUE(4 == primary->pages.rows);
}

TEST(terminal, Terminal__setPwd_preserves_a_sentinel_on_allocation_failure) {
    zigstd::FailingAllocator failing(talloc(), SIZE_MAX);
    const zigstd::Allocator alloc = failing.allocator();
    Terminal::Options t_opts((size::CellCountInt)5, (size::CellCountInt)1);
    Terminal t;
    ASSERT_TRUE(Terminal::init(alloc, t_opts, &t));
    struct TDeinit {
        Terminal *t;
        zigstd::Allocator a;
        ~TDeinit() { t->deinit(a); }
    } t_deinit = {&t, alloc};
    (void)t_deinit;

    ASSERT_TRUE(t.pwd.ensureTotalCapacityPrecise(alloc, 3));
    failing.fail_index = failing.alloc_index;
    ASSERT_TRUE(!t.setPwd("pwd"));
    ASSERT_TRUE(t.getPwd() == nullptr);
}

TEST(terminal, Terminal__setPwd_accepts_its_current_value) {
    TERM(t, 5, 1);

    ASSERT_TRUE(t.setPwd("file:///tmp"));
    ASSERT_TRUE(t.setPwd(t.getPwd()));
    EXPECT_STR("file:///tmp", t.getPwd());
}

TEST(terminal, Terminal__setTitle_preserves_a_sentinel_on_allocation_failure) {
    zigstd::FailingAllocator failing(talloc(), SIZE_MAX);
    const zigstd::Allocator alloc = failing.allocator();
    Terminal::Options t_opts((size::CellCountInt)5, (size::CellCountInt)1);
    Terminal t;
    ASSERT_TRUE(Terminal::init(alloc, t_opts, &t));
    struct TDeinit {
        Terminal *t;
        zigstd::Allocator a;
        ~TDeinit() { t->deinit(a); }
    } t_deinit = {&t, alloc};
    (void)t_deinit;

    ASSERT_TRUE(t.title.ensureTotalCapacityPrecise(alloc, 5));
    failing.fail_index = failing.alloc_index;
    ASSERT_TRUE(!t.setTitle("title"));
    ASSERT_TRUE(t.getTitle() == nullptr);
}

TEST(terminal, Terminal__setTitle_accepts_its_current_value) {
    TERM(t, 5, 1);

    ASSERT_TRUE(t.setTitle("Ghostty"));
    ASSERT_TRUE(t.setTitle(t.getTitle()));
    EXPECT_STR("Ghostty", t.getTitle());
}

TEST(terminal, Terminal__setCursorPos_saturates_overflowing_origin_offsets) {
    TERM(t, 10, 10);

    t.scrolling_region.top = 2;
    t.scrolling_region.bottom = 7;
    t.scrolling_region.left = 3;
    t.scrolling_region.right = 8;
    t.modes.set(terminal::modes::Mode::origin, true);

    t.setCursorPos(SIZE_MAX, SIZE_MAX);
    ASSERT_TRUE(8 == t.screens.active->cursor.x);
    ASSERT_TRUE(7 == t.screens.active->cursor.y);
}

TEST(terminal, Terminal__input_with_no_control_characters) {
    TERM(t, 40, 40);

    /* Basic grid writing */
    PRINT_EACH(t, "hello");
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(5 == t.screens.active->cursor.x);
    {
        EXPECT_STR("hello", t.plainString());
    }

    /* The first row should be dirty */
    ASSERT_TRUE(t.isDirty(Point::screen(5, 0)));
    ASSERT_TRUE(!t.isDirty(Point::screen(5, 1)));
}

TEST(terminal, Terminal__input_with_basic_wraparound) {
    TERM(t, 5, 40);

    /* Basic grid writing */
    PRINT_EACH(t, "helloworldabc12");
    ASSERT_TRUE(2 == t.screens.active->cursor.y);
    ASSERT_TRUE(4 == t.screens.active->cursor.x);
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);
    {
        EXPECT_STR("hello\nworld\nabc12", t.plainString());
    }
}

TEST(terminal, Terminal__input_with_basic_wraparound_dirty) {
    TERM(t, 5, 40);

    PRINT_EACH(t, "hello");
    ASSERT_TRUE(t.isDirty(Point::screen(4, 0)));
    t.clearDirty();
    ASSERT_TRUE(t.print('w'));

    /* Old row is dirty because cursor moved from there */
    ASSERT_TRUE(t.isDirty(Point::screen(4, 0)));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 1)));
}

TEST(terminal, Terminal__input_that_forces_scroll) {
    TERM(t, 1, 5);

    /* Basic grid writing */
    PRINT_EACH(t, "abcdef");
    ASSERT_TRUE(4 == t.screens.active->cursor.y);
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    {
        EXPECT_STR("b\nc\nd\ne\nf", t.plainString());
    }
}

TEST(terminal, Terminal__input_unique_style_per_cell) {
    TERM(t, 30, 30);

    for (size_t y = 0; y < (size_t)(t.rows); y++) {
        for (size_t x = 0; x < (size_t)(t.cols); x++) {
            t.setCursorPos(y, x);
            NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, (uint8_t)x, (uint8_t)y, 0)));
            ASSERT_TRUE(t.print('x'));
        }
    }
}

TEST(terminal, Terminal__input_glitch_text) {
    const char *const glitch = glitch_data;
    const size_t glitch_len = sizeof(glitch_data) - 1;
    TERM(t, 30, 30);

    /* Get our initial grapheme capacity. */
    size_t grapheme_cap;
    {
        auto *page = t.screens.active->pages.pages.first;
        grapheme_cap = page->capacity().grapheme_bytes;
    }

    /* Print glitch text until our capacity changes */
    while (true) {
        auto *page = t.screens.active->pages.pages.first;
        if (page->capacity().grapheme_bytes != grapheme_cap) break;
        ASSERT_TRUE(t.printString(glitch, glitch_len));
    }

    /* We're testing to make sure that grapheme capacity gets increased. */
    const auto page = t.screens.active->pages.pages.first;
    ASSERT_TRUE(page->capacity().grapheme_bytes > grapheme_cap);
}

TEST(terminal, Terminal__zero_width_character_at_start) {
    TERM(t, 80, 80);

    /* This used to crash the terminal. This is not allowed so we should
     * just ignore it. */
    ASSERT_TRUE(t.print(0x200D));

    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(0 == t.screens.active->cursor.x);

    /* Should not be dirty since we changed nothing. */
    ASSERT_TRUE(!t.isDirty(Point::screen(0, 0)));
}

TEST(terminal, Terminal__zero_width_character_attaches_to_pending_wrap_cell) {
    TERM(t, 2, 2);

    /* Disable grapheme clustering to exercise the fallback path. */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, false);

    ASSERT_TRUE(t.print('x'));
    ASSERT_TRUE(t.print(0xE5));
    /* Combining low line. */
    ASSERT_TRUE(t.print(0x0332));

    EXPECT_STR("x\xC3\xA5\xCC\xB2", t.plainString());
}

TEST(terminal, Terminal__caps_zero_width_codepoints_attached_to_one_cell) {
    TERM(t, 2, 2);

    t.modes.set(terminal::modes::Mode::grapheme_cluster, false);
    ASSERT_TRUE(t.print('A'));

    const auto initial_capacity = t.screens.active->cursor.page_pin->node->capacity().grapheme_bytes;
    for (size_t i_ = (size_t)(0); i_ < (size_t)(page::grapheme_max_len * 4); i_++) ASSERT_TRUE(t.print(0x0301));

    const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
    size_t glen = 0;
    ASSERT_TRUE(list_cell.node->page()->lookupGrapheme(list_cell.cell, &glen) != nullptr);
    ASSERT_TRUE(page::grapheme_max_len == glen);
    ASSERT_TRUE(initial_capacity == list_cell.node->capacity().grapheme_bytes);
}

TEST(terminal, Terminal__print_single_very_long_line) {
    TERM(t, 5, 5);

    /* This would crash for issue 1400. So the assertion here is
     * that we simply do not crash. */
    for (size_t i_ = (size_t)(0); i_ < (size_t)(1000); i_++) ASSERT_TRUE(t.print('x'));
}

TEST(terminal, Terminal__print_wide_char) {
    TERM(t, 80, 80);

    /* Smiley face */
    ASSERT_TRUE(t.print(0x1F600));
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(2 == t.screens.active->cursor.x);

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x1F600 == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
    }

    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
}

TEST(terminal, Terminal__print_wide_char_at_edge_creates_spacer_head) {
    TERM(t, 10, 10);

    t.setCursorPos(1, 10);
    /* Smiley face */
    ASSERT_TRUE(t.print(0x1F600));
    ASSERT_TRUE(1 == t.screens.active->cursor.y);
    ASSERT_TRUE(2 == t.screens.active->cursor.x);

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(9, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::spacer_head == cell->wide());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 1)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x1F600 == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 1)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
    }

    /* Our first row just had a spacer head added which does not affect
     * rendering so only the place where the wide char was printed
     * should be marked.
     * BUT old row is dirty because cursor moved from there */
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 1)));
}

TEST(terminal, Terminal__print_wide_char_with_1_column_width) {
    TERM(t, 1, 2);

    /* 0x1F600 */
    ASSERT_TRUE(t.print(0x1F600));

    /* This prints a space so we should be dirty. */
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
}

TEST(terminal, Terminal__print_wide_char_in_single_width_terminal) {
    TERM(t, 1, 80);

    /* Smiley face */
    ASSERT_TRUE(t.print(0x1F600));
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }

    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
}

TEST(terminal, Terminal__print_over_wide_char_at_0_0) {
    TERM(t, 80, 80);

    /* Smiley face */
    ASSERT_TRUE(t.print(0x1F600));
    t.setCursorPos(0, 0);
    ASSERT_TRUE(t.print('A'));

    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(1 == t.screens.active->cursor.x);

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('A' == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }

    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
    ASSERT_TRUE(!t.isDirty(Point::screen(0, 1)));
}

TEST(terminal, Terminal__print_over_wide_char_at_col_0_corrupts_previous_row) {
    /* Crash found by AFL++ fuzzer (afl-out/stream/default/crashes/id:000002).
     *
     * printCell, when overwriting a wide cell with a narrow cell at x<=1
     * and y>0, sets the last cell of the previous row to .narrow — even
     * when that cell is a .spacer_tail rather than a .spacer_head. This
     * orphans the .wide cell at cols-2. */
    TERM(t, 10, 3);

    /* Fill rows 0 and 1 with wide chars (5 per row on a 10-col terminal). */
    for (size_t i_ = (size_t)(0); i_ < (size_t)(10); i_++) ASSERT_TRUE(t.print(0x4E2D));

    /* Move cursor to row 1, col 0 (on top of a wide char) and print a
     * narrow character. This triggers printCell's .wide branch which
     * corrupts row 0's last cell: col 9 changes from .spacer_tail to
     * .narrow, orphaning the .wide at col 8. */
    t.setCursorPos(2, 1);
    ASSERT_TRUE(t.print('A'));

    /* Row 1, col 0 should be narrow (we just overwrote the wide char). */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 1)).value;
        ASSERT_TRUE(Cell::Wide::narrow == list_cell.cell->wide());
    }
    /* Row 0, col 8 should still be .wide (the last wide char on the row). */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(8, 0)).value;
        ASSERT_TRUE(Cell::Wide::wide == list_cell.cell->wide());
    }
    /* Row 0, col 9 must remain .spacer_tail to pair with the .wide at col 8. */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(9, 0)).value;
        ASSERT_TRUE(Cell::Wide::spacer_tail == list_cell.cell->wide());
    }
}

TEST(terminal, Terminal__print_over_wide_spacer_tail) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.print(0x6A4B));
    t.setCursorPos(1, 2);
    ASSERT_TRUE(t.print('X'));

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('X' == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }

    {
        EXPECT_STR(" X", t.plainString());
    }

    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
}

TEST(terminal, Terminal__print_over_wide_char_with_bold) {
    TERM(t, 80, 80);

    NOERR(t.setAttribute(attr(A::bold)));
    /* Smiley face */
    ASSERT_TRUE(t.print(0x1F600));
    /* verify we have styles in our style map */
    {
        Page *page = t.screens.active->cursor.page_pin->node->page();
        ASSERT_TRUE(1 == page->styles.count());
    }

    /* Go back and overwrite with no style */
    t.setCursorPos(0, 0);
    NOERR(t.setAttribute(attr(A::unset)));
    /* Smiley face */
    ASSERT_TRUE(t.print('A'));

    /* verify our style is gone */
    {
        Page *page = t.screens.active->cursor.page_pin->node->page();
        ASSERT_TRUE(0 == page->styles.count());
    }

    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
}

TEST(terminal, Terminal__print_over_wide_char_with_bg_color) {
    TERM(t, 80, 80);

    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 0xFF, 0, 0)));
    /* Smiley face */
    ASSERT_TRUE(t.print(0x1F600));
    /* verify we have styles in our style map */
    {
        Page *page = t.screens.active->cursor.page_pin->node->page();
        ASSERT_TRUE(1 == page->styles.count());
    }

    /* Go back and overwrite with no style */
    t.setCursorPos(0, 0);
    NOERR(t.setAttribute(attr(A::unset)));
    /* Smiley face */
    ASSERT_TRUE(t.print('A'));

    /* verify our style is gone */
    {
        Page *page = t.screens.active->cursor.page_pin->node->page();
        ASSERT_TRUE(0 == page->styles.count());
    }

    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
}

TEST(terminal, Terminal__print_multicodepoint_grapheme__disabled_mode_2027) {
    TERM(t, 80, 80);

    /* https://github.com/mitchellh/ghostty/issues/289
     * This is: 👨‍👩‍👧 (which may or may not render correctly) */
    ASSERT_TRUE(t.print(0x1F468));
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(t.print(0x1F469));
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(t.print(0x1F467));

    /* We should have 6 cells taken up */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(6 == t.screens.active->cursor.x);

    /* Assert various properties about our screen to verify
     * we have all expected cells. */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x1F468 == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
        size_t cps_len = 0;
        const uint32_t *cps = list_cell.node->page()->lookupGrapheme(cell, &cps_len);
        ASSERT_TRUE(cps != nullptr);
        ASSERT_TRUE(1 == cps_len);
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(!cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
        { size_t _glen = 0; ASSERT_TRUE(list_cell.node->page()->lookupGrapheme(cell, &_glen) == nullptr); }
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(2, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x1F469 == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
        size_t cps_len = 0;
        const uint32_t *cps = list_cell.node->page()->lookupGrapheme(cell, &cps_len);
        ASSERT_TRUE(cps != nullptr);
        ASSERT_TRUE(1 == cps_len);
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(3, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(!cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
        { size_t _glen = 0; ASSERT_TRUE(list_cell.node->page()->lookupGrapheme(cell, &_glen) == nullptr); }
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(4, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x1F467 == cell->contentCodepoint());
        ASSERT_TRUE(!cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
        { size_t _glen = 0; ASSERT_TRUE(list_cell.node->page()->lookupGrapheme(cell, &_glen) == nullptr); }
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(5, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(!cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
        { size_t _glen = 0; ASSERT_TRUE(list_cell.node->page()->lookupGrapheme(cell, &_glen) == nullptr); }
    }

    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
}

TEST(terminal, Terminal__enabling_grapheme_mode_handles_stored_breaks) {
    TERM(t, 5, 1);

    t.modes.set(terminal::modes::Mode::grapheme_cluster, false);
    ASSERT_TRUE(t.print('a'));
    /* Zero width space is stored on the prior cell. */
    ASSERT_TRUE(t.print(0x200B));

    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);
    ASSERT_TRUE(t.print(0x0301));

    EXPECT_STR("a\xE2\x80\x8B\xCC\x81", t.plainString());
}

TEST(terminal, Terminal__graphemeWidth_parity) {
    GW(0x2764, 0xFE0F);
    GW('x', 0xFE0F, 0xFE0F);
    GW(0x231A, 0xFE0E, 0xFE0F);
    GW(0x1F3F4, 0x200D, 0x2620, 0xFE0F);
    GW(0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467);
    GW(0x23, 0xFE0F, 0x20E3);
    GW('1', 0x20E3);
    GW(0x1F44B, 0x1F3FF);
    GW(0x1F1E6, 0x1F1E7, 0x1F1E8);
    GW('a', 'b');
    GW(0x0301, 0x0302);
}

TEST(terminal, Terminal__VS16_doesn_t_make_character_with_2027_disabled) {
    TERM(t, 5, 5);

    /* Disable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, false);

    /* Heart */
    ASSERT_TRUE(t.print(0x2764));
    /* VS16 to make wide */
    ASSERT_TRUE(t.print(0xFE0F));

    {
        EXPECT_STR("\xE2\x9D\xA4\xEF\xB8\x8F", t.plainString());
    }

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x2764 == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
        size_t cps_len = 0;
        const uint32_t *cps = list_cell.node->page()->lookupGrapheme(cell, &cps_len);
        ASSERT_TRUE(cps != nullptr);
        ASSERT_TRUE(1 == cps_len);
    }
}

TEST(terminal, Terminal__ignored_VS16_doesn_t_mark_dirty) {
    TERM(t, 5, 5);

    /* Disable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, false);

    /* Heart */
    ASSERT_TRUE(t.print(0x2764));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));

    t.clearDirty();
    /* VS16 to make wide */
    ASSERT_TRUE(t.print(0xFE0F));
    ASSERT_TRUE(!t.isDirty(Point::screen(0, 0)));
}

TEST(terminal, Terminal__print_invalid_VS16_non_grapheme) {
    TERM(t, 80, 80);

    /* https://github.com/mitchellh/ghostty/issues/1482 */
    ASSERT_TRUE(t.print('x'));
    ASSERT_TRUE(t.print(0xFE0F));

    /* We should have 1 narrow cell. */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(1 == t.screens.active->cursor.x);

    /* Assert various properties about our screen to verify
     * we have all expected cells. */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('x' == cell->contentCodepoint());
        ASSERT_TRUE(!cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
    }
}

TEST(terminal, Terminal__invalid_VS16_doesn_t_mark_dirty) {
    TERM(t, 5, 5);

    /* Disable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, false);

    ASSERT_TRUE(t.print('x'));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));

    t.clearDirty();
    /* VS16 to make wide */
    ASSERT_TRUE(t.print(0xFE0F));
    ASSERT_TRUE(!t.isDirty(Point::screen(0, 0)));
}

TEST(terminal, Terminal__variation_selectors_apply_to_preceding_codepoint) {
    TERM(t, 5, 5);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* Pirate flag: black flag + ZWJ + skull and crossbones + VS16. */
    ASSERT_TRUE(t.print(0x1F3F4));
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(t.print(0x2620));
    ASSERT_TRUE(t.print(0xFE0F));

    const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
    const Cell *cell = list_cell.cell;
    ASSERT_TRUE(0x1F3F4 == cell->contentCodepoint());
    ASSERT_TRUE(cell->hasGrapheme());
    EXPECT_GRAPHEMES(list_cell, cell, 0x200D, 0x2620, 0xFE0F);
}

TEST(terminal, Terminal__print_multicodepoint_grapheme__mode_2027) {
    TERM(t, 80, 80);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* https://github.com/mitchellh/ghostty/issues/289
     * This is: 👨‍👩‍👧 (which may or may not render correctly) */
    ASSERT_TRUE(t.print(0x1F468));
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(t.print(0x1F469));
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(t.print(0x1F467));

    /* We should have 2 cells taken up. It is one character but "wide". */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(2 == t.screens.active->cursor.x);

    /* Row should be dirty */
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));

    /* Assert various properties about our screen to verify
     * we have all expected cells. */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x1F468 == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
        size_t cps_len = 0;
        const uint32_t *cps = list_cell.node->page()->lookupGrapheme(cell, &cps_len);
        ASSERT_TRUE(cps != nullptr);
        ASSERT_TRUE(4 == cps_len);
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(!cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
    }
}

TEST(terminal, Terminal__keypad_sequence_VS15) {
    TERM(t, 80, 80);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* This is: "#︎" (number sign with text presentation selector)
     * # Number sign (valid base) */
    ASSERT_TRUE(t.print(0x23));
    /* VS15 (text presentation selector) */
    ASSERT_TRUE(t.print(0xFE0E));

    /* VS15 should combine with the base character into a single grapheme cluster,
     * taking 1 cell (narrow character). */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(1 == t.screens.active->cursor.x);

    /* Row should be dirty */
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));

    /* The base emoji should be in cell 0 with the skin tone as a grapheme */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x23 == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }
}

TEST(terminal, Terminal__keypad_sequence_VS16) {
    TERM(t, 80, 80);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* This is: "#️" (number sign with emoji presentation selector)
     * # Number sign (valid base) */
    ASSERT_TRUE(t.print(0x23));
    /* VS16 (emoji presentation selector) */
    ASSERT_TRUE(t.print(0xFE0F));

    /* VS16 should combine with the base character into a single grapheme cluster,
     * taking 2 cells (wide character). */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(2 == t.screens.active->cursor.x);

    /* Row should be dirty */
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));

    /* The base emoji should be in cell 0 with the skin tone as a grapheme */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x23 == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
    }
}

TEST(terminal, Terminal__Fitzpatrick_skin_tone_next_valid_base) {
    TERM(t, 80, 80);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* This is: "👋🏿" (waving hand with dark skin tone)
     * 👋 Waving hand (valid base) */
    ASSERT_TRUE(t.print(0x1F44B));
    /* 🏿 Dark skin tone modifier */
    ASSERT_TRUE(t.print(0x1F3FF));

    /* The skin tone should combine with the base emoji into a single grapheme cluster,
     * taking 2 cells (wide character). */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(2 == t.screens.active->cursor.x);

    /* Row should be dirty */
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));

    /* The base emoji should be in cell 0 with the skin tone as a grapheme */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x1F44B == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
    }
}

TEST(terminal, Terminal__Fitzpatrick_skin_tone_next_to_non_base) {
    TERM(t, 80, 80);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* This is: "🏿" (which may not render correctly in your editor!)
     * " */
    ASSERT_TRUE(t.print(0x22));
    /* Dark skin tone */
    ASSERT_TRUE(t.print(0x1F3FF));
    /* " */
    ASSERT_TRUE(t.print(0x22));

    /* We should have 4 cells taken up. Importantly, the skin tone
     * should not join with the quotes. */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(4 == t.screens.active->cursor.x);

    /* Row should be dirty */
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));

    /* Assert various properties about our screen to verify
     * we have all expected cells. */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x22 == cell->contentCodepoint());
        ASSERT_TRUE(!cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x1F3FF == cell->contentCodepoint());
        ASSERT_TRUE(!cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(3, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x22 == cell->contentCodepoint());
        ASSERT_TRUE(!cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }
}

TEST(terminal, Terminal__multicodepoint_grapheme_marks_dirty_on_every_codepoint) {
    TERM(t, 80, 80);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* https://github.com/mitchellh/ghostty/issues/289
     * This is: 👨‍👩‍👧 (which may or may not render correctly) */
    ASSERT_TRUE(t.print(0x1F468));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
    t.clearDirty();
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
    t.clearDirty();
    ASSERT_TRUE(t.print(0x1F469));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
    t.clearDirty();
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
    t.clearDirty();
    ASSERT_TRUE(t.print(0x1F467));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));

    /* We should have 2 cells taken up. It is one character but "wide". */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(2 == t.screens.active->cursor.x);
}

TEST(terminal, Terminal__VS15_to_make_narrow_character) {
    TERM(t, 5, 5);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* Umbrella with rain drops, width=2 */
    ASSERT_TRUE(t.print(0x2614));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
    t.clearDirty();

    /* We should have 2 cells taken up. It is one character but "wide". */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(2 == t.screens.active->cursor.x);

    /* VS15 to make narrow */
    ASSERT_TRUE(t.print(0xFE0E));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
    t.clearDirty();

    /* VS15 should send us back a cell since our char is no longer wide. */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(1 == t.screens.active->cursor.x);

    {
        EXPECT_STR("\xE2\x98\x94\xEF\xB8\x8E", t.plainString());
    }

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x2614 == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
        size_t cps_len = 0;
        const uint32_t *cps = list_cell.node->page()->lookupGrapheme(cell, &cps_len);
        ASSERT_TRUE(cps != nullptr);
        ASSERT_TRUE(1 == cps_len);
    }
}

TEST(terminal, Terminal__VS15_on_already_narrow_emoji) {
    TERM(t, 5, 5);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* Thunder cloud and rain, width=1 */
    ASSERT_TRUE(t.print(0x26C8));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
    t.clearDirty();
    /* VS15 to make narrow */
    ASSERT_TRUE(t.print(0xFE0E));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
    t.clearDirty();

    /* Character takes up one cell */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(1 == t.screens.active->cursor.x);

    {
        EXPECT_STR("\xE2\x9B\x88\xEF\xB8\x8E", t.plainString());
    }

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x26C8 == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
        size_t cps_len = 0;
        const uint32_t *cps = list_cell.node->page()->lookupGrapheme(cell, &cps_len);
        ASSERT_TRUE(cps != nullptr);
        ASSERT_TRUE(1 == cps_len);
    }
}

TEST(terminal, Terminal__print_invalid_VS15_following_emoji_is_wide) {
    TERM(t, 80, 80);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* 🧠 */
    ASSERT_TRUE(t.print(0x1F9E0));
    /* not valid with U+1F9E0 as base */
    ASSERT_TRUE(t.print(0xFE0E));

    /* We should have 2 cells taken up. It is one character but "wide". */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(2 == t.screens.active->cursor.x);

    /* Assert various properties about our screen to verify
     * we have all expected cells. */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x1F9E0 == cell->contentCodepoint());
        ASSERT_TRUE(!cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
    }
}

TEST(terminal, Terminal__print_invalid_VS15_in_emoji_ZWJ_sequence) {
    TERM(t, 80, 80);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* 👩 */
    ASSERT_TRUE(t.print(0x1F469));
    /* not valid with U+1F469 as base */
    ASSERT_TRUE(t.print(0xFE0E));
    /* ZWJ */
    ASSERT_TRUE(t.print(0x200D));
    /* 👦 */
    ASSERT_TRUE(t.print(0x1F466));

    /* We should have 2 cells taken up. It is one character but "wide". */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(2 == t.screens.active->cursor.x);

    /* Assert various properties about our screen to verify
     * we have all expected cells. */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x1F469 == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        EXPECT_GRAPHEMES(list_cell, cell, 0x200D, 0x1F466);
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
    }
}

TEST(terminal, Terminal__VS15_to_make_narrow_character_with_pending_wrap) {
    TERM(t, 4, 5);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    ASSERT_TRUE(t.modes.get(terminal::modes::Mode::wraparound));

    /* Lemon, width=2 */
    ASSERT_TRUE(t.print(0x1F34B));
    /* Umbrella with rain drops, width=2 */
    ASSERT_TRUE(t.print(0x2614));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
    t.clearDirty();

    /* We only move to the end of the line because we're in a pending wrap
     * state. */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(3 == t.screens.active->cursor.x);
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);

    /* VS15 to make narrow */
    ASSERT_TRUE(t.print(0xFE0E));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
    t.clearDirty();

    /* VS15 should clear the pending wrap state */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(3 == t.screens.active->cursor.x);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);

    {
        EXPECT_STR("\xF0\x9F\x8D\x8B\xE2\x98\x94\xEF\xB8\x8E", t.plainString());
    }

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(2, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x2614 == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
        size_t cps_len = 0;
        const uint32_t *cps = list_cell.node->page()->lookupGrapheme(cell, &cps_len);
        ASSERT_TRUE(cps != nullptr);
        ASSERT_TRUE(1 == cps_len);
    }

    /* VS15 should not affect the previous grapheme */
    {
        const auto lemon_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value.cell;
        ASSERT_TRUE(0x1F34B == lemon_cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::wide == lemon_cell->wide());
        const auto spacer_cell = t.screens.active->pages.getCell(Point::screen(1, 0)).value.cell;
        ASSERT_TRUE(0 == spacer_cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::spacer_tail == spacer_cell->wide());
    }
}

TEST(terminal, Terminal__VS15_narrows_wide_cell_under_cursor_with_wraparound_disabled) {
    TERM(t, 5, 5);

    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);
    t.modes.set(terminal::modes::Mode::wraparound, false);

    /* First create a wide cell spanning columns 4 and 5. */
    t.setCursorPos(1, 4);
    ASSERT_TRUE(t.print(0x2614));

    /* Make column 4 the right margin and put the cursor on the wide base.
     * With wraparound disabled, grapheme lookup selects the cell under the
     * cursor when it has content. */
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setLeftAndRightMargin(1, 4);
    t.setCursorPos(1, 4);
    ASSERT_TRUE(t.print(0xFE0E));

    ASSERT_TRUE(3 == t.screens.active->cursor.x);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);
    const auto base = t.screens.active->pages.getCell(Point::screen(3, 0)).value.cell;
    ASSERT_TRUE(Cell::Wide::narrow == base->wide());
    ASSERT_TRUE(base->hasGrapheme());
    const auto tail = t.screens.active->pages.getCell(Point::screen(4, 0)).value.cell;
    ASSERT_TRUE(Cell::Wide::narrow == tail->wide());
}

TEST(terminal, Terminal__VS15_narrows_wide_cell_under_restored_pending_cursor) {
    TERM(t, 5, 5);

    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setLeftAndRightMargin(1, 4);

    /* Save a pending-wrap cursor at column 4. */
    t.setCursorPos(1, 4);
    ASSERT_TRUE(t.print('X'));
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);
    t.saveCursor();

    /* Widen the margin and replace that cell with a wide character. */
    t.setLeftAndRightMargin(1, 5);
    t.setCursorPos(1, 4);
    ASSERT_TRUE(t.print(0x2614));

    /* Restoring also restores pending_wrap, so grapheme lookup selects the
     * wide base under the cursor rather than its spacer tail. */
    t.restoreCursor();
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);
    ASSERT_TRUE(t.print(0xFE0E));

    ASSERT_TRUE(4 == t.screens.active->cursor.x);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);
    const auto base = t.screens.active->pages.getCell(Point::screen(3, 0)).value.cell;
    ASSERT_TRUE(Cell::Wide::narrow == base->wide());
    ASSERT_TRUE(base->hasGrapheme());
    const auto tail = t.screens.active->pages.getCell(Point::screen(4, 0)).value.cell;
    ASSERT_TRUE(Cell::Wide::narrow == tail->wide());
}

TEST(terminal, Terminal__VS16_to_make_wide_character_on_next_line) {
    TERM(t, 3, 5);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    t.cursorRight(2);
    ASSERT_TRUE(t.print('#'));
    ASSERT_TRUE(2 == t.screens.active->cursor.x);
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);
    ASSERT_TRUE(t.isDirty(Point::screen(2, 0)));
    t.clearDirty();

    /* VS16 to make wide */
    ASSERT_TRUE(t.print(0xFE0F));

    ASSERT_TRUE(t.isDirty(Point::screen(2, 0)));
    t.clearDirty();
    ASSERT_TRUE(1 == t.screens.active->cursor.y);
    ASSERT_TRUE(2 == t.screens.active->cursor.x);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);

    {
        /* Previous cell turns into spacer_head */
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(2, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(!cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::spacer_head == cell->wide());
    }
    {
        /* '#' cell is wide */
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 1)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('#' == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        EXPECT_GRAPHEMES(list_cell, cell, 0xFE0F);
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
    }
    {
        /* spacer_tail */
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 1)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(!cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
    }
}

TEST(terminal, Terminal__VS16_to_make_wide_character_on_next_line_with_hyperlink) {
    /* Regression test for the crash fixed in print's grapheme `.wide` path:
     * writing a spacer_head at the screen edge before row.wrap was set. */
    TERM(t, 3, 5);

    /* Enable grapheme clustering and activate a hyperlink so printCell
     * calls cursorSetHyperlink (which runs page integrity checks). */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);
    NOERR(startLink(*t.screens.active, "http://example.com"));

    t.cursorRight(2);
    ASSERT_TRUE(t.print('#'));
    ASSERT_TRUE(2 == t.screens.active->cursor.x);
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);

    /* Without the fix, this panicked with UnwrappedSpacerHead.
     * VS16 to make wide */
    ASSERT_TRUE(t.print(0xFE0F));

    ASSERT_TRUE(1 == t.screens.active->cursor.y);
    ASSERT_TRUE(2 == t.screens.active->cursor.x);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);

    {
        /* Previous cell turns into spacer_head and remains hyperlinked. */
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(2, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::spacer_head == cell->wide());
        ASSERT_TRUE(cell->hyperlink());
        ASSERT_TRUE(list_cell.row->wrap());
    }
    {
        /* '#' cell is now wide and still hyperlinked. */
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 1)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('#' == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        EXPECT_GRAPHEMES(list_cell, cell, 0xFE0F);
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
        ASSERT_TRUE(cell->hyperlink());
    }
    {
        /* spacer_tail inherits hyperlink as part of the same grapheme cell. */
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 1)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
        ASSERT_TRUE(cell->hyperlink());
    }
}

TEST(terminal, Terminal__VS16_widening_when_the_spacer_tail_grows_the_page) {
    /* Regression test for a stale cell pointer in print's grapheme `.wide`
     * path: writing the spacer tail can grow the page to fit the hyperlink,
     * which replaces the page and invalidates the pointer to the wide cell. */
    TERM(t, 20, 10);

    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);
    NOERR(startLink(*t.screens.active, "http://example.com"));

    /* Fill the page hyperlink map until a single slot is left. The '#' below
     * takes that slot so the spacer tail is what forces the page to grow. */
    while (true) {
        Page *page = t.screens.active->cursor.page_pin->node->page();
        const auto map = page->hyperlink_map.map(page->memory);
        if (map.maxLoad() - map.count() == 1) break;
        ASSERT_TRUE(t.print('x'));
    }

    const auto x = t.screens.active->cursor.x;
    const auto y = t.screens.active->cursor.y;
    ASSERT_TRUE(t.print('#'));

    /* Without the fix this crashed appending to a freed page. */
    ASSERT_TRUE(t.print(0xFE0F));

    {
        /* '#' is wide and carries the VS16 grapheme. */
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(x, y)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('#' == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
        ASSERT_TRUE(cell->hasGrapheme());
        EXPECT_GRAPHEMES(list_cell, cell, 0xFE0F);
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(x + 1, y)).value;
        ASSERT_TRUE(Cell::Wide::spacer_tail == list_cell.cell->wide());
    }
}

TEST(terminal, Terminal__grapheme_transfer_when_widening_wraps_to_the_next_line) {
    /* Covers print's grapheme `.wide` path where the previous cell already
     * holds grapheme data and has to be moved to the wrapped row. */
    TERM(t, 3, 5);

    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);
    t.cursorRight(2);

    /* A narrow emoji, then ZWJ, then a second emoji. The ZWJ attaches
     * without changing the width, so the cell has grapheme data by the time
     * the second emoji widens it. */
    ASSERT_TRUE(t.print(0x263A));
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(t.print(0x2764));

    {
        /* The old cell becomes a spacer head on the wrapped row. */
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(2, 0)).value;
        ASSERT_TRUE(Cell::Wide::spacer_head == list_cell.cell->wide());
        ASSERT_TRUE(list_cell.row->wrap());
    }
    {
        /* The grapheme moved with the base codepoint. */
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 1)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x263A == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
        EXPECT_GRAPHEMES(list_cell, cell, 0x200D, 0x2764);
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 1)).value;
        ASSERT_TRUE(Cell::Wide::spacer_tail == list_cell.cell->wide());
    }
}

TEST(terminal, Terminal__VS16_to_make_wide_character_with_pending_wrap) {
    TERM(t, 3, 5);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    t.cursorRight(1);
    ASSERT_TRUE(t.print('#'));
    ASSERT_TRUE(2 == t.screens.active->cursor.x);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);

    /* VS16 to make wide */
    ASSERT_TRUE(t.print(0xFE0F));

    ASSERT_TRUE(2 == t.screens.active->cursor.x);
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);

    {
        /* '#' cell is wide */
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('#' == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        EXPECT_GRAPHEMES(list_cell, cell, 0xFE0F);
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
    }
    {
        /* spacer_tail */
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(2, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(!cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
    }
}

TEST(terminal, Terminal__VS16_to_make_wide_character_with_mode_2027) {
    TERM(t, 5, 5);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* Heart */
    ASSERT_TRUE(t.print(0x2764));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
    t.clearDirty();
    /* VS16 to make wide */
    ASSERT_TRUE(t.print(0xFE0F));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
    t.clearDirty();

    {
        EXPECT_STR("\xE2\x9D\xA4\xEF\xB8\x8F", t.plainString());
    }

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x2764 == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
        size_t cps_len = 0;
        const uint32_t *cps = list_cell.node->page()->lookupGrapheme(cell, &cps_len);
        ASSERT_TRUE(cps != nullptr);
        ASSERT_TRUE(1 == cps_len);
    }
}

TEST(terminal, Terminal__VS16_repeated_with_mode_2027) {
    TERM(t, 5, 5);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* Heart */
    ASSERT_TRUE(t.print(0x2764));
    /* VS16 to make wide */
    ASSERT_TRUE(t.print(0xFE0F));
    /* Heart */
    ASSERT_TRUE(t.print(0x2764));
    /* VS16 to make wide */
    ASSERT_TRUE(t.print(0xFE0F));

    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));

    {
        EXPECT_STR("\xE2\x9D\xA4\xEF\xB8\x8F\xE2\x9D\xA4\xEF\xB8\x8F", t.plainString());
    }

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x2764 == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
        size_t cps_len = 0;
        const uint32_t *cps = list_cell.node->page()->lookupGrapheme(cell, &cps_len);
        ASSERT_TRUE(cps != nullptr);
        ASSERT_TRUE(1 == cps_len);
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(2, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x2764 == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
        size_t cps_len = 0;
        const uint32_t *cps = list_cell.node->page()->lookupGrapheme(cell, &cps_len);
        ASSERT_TRUE(cps != nullptr);
        ASSERT_TRUE(1 == cps_len);
    }
}

TEST(terminal, Terminal__print_invalid_VS16_grapheme) {
    TERM(t, 80, 80);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* https://github.com/mitchellh/ghostty/issues/1482 */
    ASSERT_TRUE(t.print('x'));
    /* invalid VS16 */
    ASSERT_TRUE(t.print(0xFE0F));

    /* We should have 1 cells taken up, and narrow. */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(1 == t.screens.active->cursor.x);

    /* Assert various properties about our screen to verify
     * we have all expected cells. */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('x' == cell->contentCodepoint());
        ASSERT_TRUE(!cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }
}

TEST(terminal, Terminal__print_invalid_VS16_with_second_char) {
    TERM(t, 80, 80);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* https://github.com/mitchellh/ghostty/issues/1482 */
    ASSERT_TRUE(t.print('x'));
    ASSERT_TRUE(t.print(0xFE0F));
    ASSERT_TRUE(t.print('y'));

    /* We should have 2 cells taken up, from two separate narrow characters. */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(2 == t.screens.active->cursor.x);

    /* Assert various properties about our screen to verify
     * we have all expected cells. */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('x' == cell->contentCodepoint());
        ASSERT_TRUE(!cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('y' == cell->contentCodepoint());
        ASSERT_TRUE(!cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }
}

TEST(terminal, Terminal__print_grapheme_o___o_with_nonspacing_mark__should_be_narrow) {
    TERM(t, 5, 5);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    ASSERT_TRUE(t.print('o'));
    /* combining grave accent */
    ASSERT_TRUE(t.print(0x0300));

    /* We should have 1 cell taken up. */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(1 == t.screens.active->cursor.x);

    /* Assert various properties about our screen to verify
     * we have all expected cells. */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('o' == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        EXPECT_GRAPHEMES(list_cell, cell, 0x0300);
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }
}

TEST(terminal, Terminal__print_Devanagari_grapheme_should_be_wide) {
    TERM(t, 5, 5);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* क्‍ष */
    ASSERT_TRUE(t.print(0x0915));
    ASSERT_TRUE(t.print(0x094D));
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(t.print(0x0937));

    /* We should have 2 cells taken up. It is one character but "wide". */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(2 == t.screens.active->cursor.x);

    /* Assert various properties about our screen to verify
     * we have all expected cells. */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x0915 == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        EXPECT_GRAPHEMES(list_cell, cell, 0x094D, 0x200D, 0x0937);
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
    }
}

TEST(terminal, Terminal__print_Devanagari_grapheme_should_be_wide_on_next_line) {
    TERM(t, 3, 5);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    t.cursorRight(2);

    /* क्‍ष */
    ASSERT_TRUE(t.print(0x0915));
    ASSERT_TRUE(t.print(0x094D));
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(2 == t.screens.active->cursor.x);
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);

    /* This one increases the width to wide */
    ASSERT_TRUE(t.print(0x0937));

    /* We should have 2 cells taken up. It is one character but "wide". */
    ASSERT_TRUE(1 == t.screens.active->cursor.y);
    ASSERT_TRUE(2 == t.screens.active->cursor.x);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);

    {
        /* Previous cell turns into spacer_head */
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(2, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(!cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::spacer_head == cell->wide());
    }
    {
        /* Devanagari grapheme is wide */
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 1)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x0915 == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        EXPECT_GRAPHEMES(list_cell, cell, 0x094D, 0x200D, 0x0937);
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 1)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
    }
}

TEST(terminal, Terminal__print_Devanagari_grapheme_should_be_wide_on_next_page) {
    const size::CellCountInt rows = page::std_capacity().rows;
    const size::CellCountInt cols = page::std_capacity().cols;
    TERM(t, cols, rows);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    t.cursorDown(rows - 1);

    for (size_t i_ = (size_t)(rows), n_i_ = (size_t)(t.screens.active->pages.pages.first->capacity().rows); i_ < n_i_; i_++) {
        ASSERT_TRUE(t.index());
    }

    t.cursorRight(cols - 1);

    ASSERT_TRUE(cols - 1 == t.screens.active->cursor.x);
    ASSERT_TRUE(rows - 1 == t.screens.active->cursor.y);

    /* क्‍ष */
    ASSERT_TRUE(t.print(0x0915));
    ASSERT_TRUE(t.print(0x094D));
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(cols - 1 == t.screens.active->cursor.x);
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);

    /* This one increases the width to wide */
    ASSERT_TRUE(t.print(0x0937));

    /* We should have 2 cells taken up. It is one character but "wide". */
    ASSERT_TRUE(rows - 1 == t.screens.active->cursor.y);
    ASSERT_TRUE(2 == t.screens.active->cursor.x);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);

    {
        /* Previous cell turns into spacer_head */
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(cols - 1, rows - 2)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(!cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::spacer_head == cell->wide());
    }
    {
        /* Devanagari grapheme is wide */
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, rows - 1)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x0915 == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        EXPECT_GRAPHEMES(list_cell, cell, 0x094D, 0x200D, 0x0937);
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(1, rows - 1)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
    }
}

TEST(terminal, Terminal__print_invalid_VS16_with_second_char__combining_) {
    TERM(t, 80, 80);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* https://github.com/mitchellh/ghostty/issues/1482 */
    ASSERT_TRUE(t.print('n'));
    /* invalid VS16 */
    ASSERT_TRUE(t.print(0xFE0F));
    /* combining tilde */
    ASSERT_TRUE(t.print(0x0303));

    /* We should have 1 cells taken up, and narrow. */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(1 == t.screens.active->cursor.x);

    /* Assert various properties about our screen to verify
     * we have all expected cells. */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('n' == cell->contentCodepoint());
        ASSERT_TRUE(cell->hasGrapheme());
        EXPECT_GRAPHEMES(list_cell, cell, 0x0303);
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }
}

TEST(terminal, Terminal__overwrite_grapheme_should_clear_grapheme_data) {
    TERM(t, 5, 5);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* Thunder cloud and rain */
    ASSERT_TRUE(t.print(0x26C8));
    /* VS15 to make narrow */
    ASSERT_TRUE(t.print(0xFE0E));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
    t.clearDirty();

    t.setCursorPos(1, 1);
    ASSERT_TRUE(t.print('A'));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));

    {
        EXPECT_STR("A", t.plainString());
    }

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('A' == cell->contentCodepoint());
        ASSERT_TRUE(!cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }
}

TEST(terminal, Terminal__overwrite_multicodepoint_grapheme_clears_grapheme_data) {
    TERM(t, 10, 10);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* https://github.com/mitchellh/ghostty/issues/289
     * This is: 👨‍👩‍👧 (which may or may not render correctly) */
    ASSERT_TRUE(t.print(0x1F468));
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(t.print(0x1F469));
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(t.print(0x1F467));

    /* We should have 2 cells taken up. It is one character but "wide". */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(2 == t.screens.active->cursor.x);

    /* We should have one cell with graphemes */
    Page *page = t.screens.active->cursor.page_pin->node->page();
    ASSERT_TRUE(1 == page->graphemeCount());

    /* Move back and overwrite wide */
    t.setCursorPos(1, 1);
    t.clearDirty();
    ASSERT_TRUE(t.print('X'));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));

    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(1 == t.screens.active->cursor.x);
    ASSERT_TRUE(0 == page->graphemeCount());

    {
        EXPECT_STR("X", t.plainString());
    }
}

TEST(terminal, Terminal__overwrite_multicodepoint_grapheme_tail_clears_grapheme_data) {
    TERM(t, 10, 10);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* https://github.com/mitchellh/ghostty/issues/289
     * This is: 👨‍👩‍👧 (which may or may not render correctly) */
    ASSERT_TRUE(t.print(0x1F468));
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(t.print(0x1F469));
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(t.print(0x1F467));

    /* We should have 2 cells taken up. It is one character but "wide". */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(2 == t.screens.active->cursor.x);

    /* We should have one cell with graphemes */
    Page *page = t.screens.active->cursor.page_pin->node->page();
    ASSERT_TRUE(1 == page->graphemeCount());

    /* Move back and overwrite wide */
    t.setCursorPos(1, 2);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR(" X", t.plainString());
    }

    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(2 == t.screens.active->cursor.x);
    ASSERT_TRUE(0 == page->graphemeCount());
}

TEST(terminal, Terminal__print_breaks_valid_grapheme_cluster_with_Prepend___ASCII_for_speed) {
    TERM(t, 5, 5);
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* Make sure we're not at cursor.x == 0 for the next char. */
    ASSERT_TRUE(t.print('_'));

    /* U+0600 ARABIC NUMBER SIGN (Prepend) */
    ASSERT_TRUE(t.print(0x0600));
    ASSERT_TRUE(t.print('1'));

    /* We should have 3 cells taken up, each narrow. Note that this is
     * **incorrect** grapheme break behavior, since a Prepend code point should
     * not break with the one following it per UAX #29 GB9b. However, as an
     * optimization we assume a grapheme break when c <= 255, and note that
     * this deviation only affects these very uncommon scenarios (e.g. the
     * Arabic number sign should precede Arabic-script digits). */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(3 == t.screens.active->cursor.x);
    /* This is what we'd expect if we did break correctly:
     * try testing.expectEqual(@as(usize, 2), t.screens.active->cursor.x); */

    /* Assert various properties about our screen to verify
     * we have all expected cells. */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x0600 == cell->contentCodepoint());
        ASSERT_TRUE(!cell->hasGrapheme());
        /* This is what we'd expect if we did break correctly:
         * try testing.expect(cell->hasGrapheme());
         * try testing.expectEqualSlices(u21, &.{'1'}, list_cell.node.page().lookupGrapheme(cell).?); */
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(2, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('1' == cell->contentCodepoint());
        /* This is what we'd expect if we did break correctly:
         * try testing.expectEqual(@as(u21, 0), cell.content.codepoint.data); */
        ASSERT_TRUE(!cell->hasGrapheme());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }
}

TEST(terminal, Terminal__print_writes_to_bottom_if_scrolled) {
    TERM(t, 5, 2);

    /* Basic grid writing */
    PRINT_EACH(t, "hello");
    t.setCursorPos(0, 0);

    /* Make newlines so we create scrollback
     * 3 pushes hello off the screen */
    ASSERT_TRUE(t.index());
    ASSERT_TRUE(t.index());
    ASSERT_TRUE(t.index());
    {
        EXPECT_STR("", t.plainString());
    }

    /* Scroll to the top */
    t.screens.active->scroll(PageList::Scroll::top());
    {
        EXPECT_STR("hello", t.plainString());
    }

    /* Type */
    ASSERT_TRUE(t.print('A'));
    t.screens.active->scroll(PageList::Scroll::active());
    {
        EXPECT_STR("\nA", t.plainString());
    }

    ASSERT_TRUE(t.isDirty(Point::active(t.screens.active->cursor.x, t.screens.active->cursor.y)));
}

TEST(terminal, Terminal__print_charset) {
    TERM(t, 80, 80);

    /* G1 should have no effect */
    t.configureCharset(terminal::charsets::Slots::G1, terminal::charsets::Charset::dec_special);
    t.configureCharset(terminal::charsets::Slots::G2, terminal::charsets::Charset::dec_special);
    t.configureCharset(terminal::charsets::Slots::G3, terminal::charsets::Charset::dec_special);

    /* No dirty to configure charset */
    ASSERT_TRUE(!t.isDirty(Point::screen(0, 0)));

    /* Basic grid writing */
    ASSERT_TRUE(t.print('`'));
    t.configureCharset(terminal::charsets::Slots::G0, terminal::charsets::Charset::utf8);
    ASSERT_TRUE(t.print('`'));
    t.configureCharset(terminal::charsets::Slots::G0, terminal::charsets::Charset::ascii);
    ASSERT_TRUE(t.print('`'));
    t.configureCharset(terminal::charsets::Slots::G0, terminal::charsets::Charset::dec_special);
    ASSERT_TRUE(t.print('`'));
    {
        EXPECT_STR("```\xE2\x97\x86", t.plainString());
    }

    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
}

TEST(terminal, Terminal__print_charset_outside_of_ASCII) {
    TERM(t, 80, 80);

    /* G1 should have no effect */
    t.configureCharset(terminal::charsets::Slots::G1, terminal::charsets::Charset::dec_special);
    t.configureCharset(terminal::charsets::Slots::G2, terminal::charsets::Charset::dec_special);
    t.configureCharset(terminal::charsets::Slots::G3, terminal::charsets::Charset::dec_special);

    /* No dirty to configure charset */
    ASSERT_TRUE(!t.isDirty(Point::screen(0, 0)));

    /* Basic grid writing */
    t.configureCharset(terminal::charsets::Slots::G0, terminal::charsets::Charset::dec_special);
    ASSERT_TRUE(t.print('`'));
    ASSERT_TRUE(t.print(0x1F600));
    {
        EXPECT_STR("\xE2\x97\x86 ", t.plainString());
    }

    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
}

TEST(terminal, Terminal__print_invoke_charset) {
    TERM(t, 80, 80);

    t.configureCharset(terminal::charsets::Slots::G1, terminal::charsets::Charset::dec_special);

    ASSERT_TRUE(t.print('`'));

    /* Invokecharset but should not mark dirty on its own */
    t.clearDirty();
    t.invokeCharset(terminal::charsets::ActiveSlot::GL, terminal::charsets::Slots::G1, false);
    ASSERT_TRUE(!t.isDirty(Point::screen(0, 0)));
    ASSERT_TRUE(t.print('`'));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
    ASSERT_TRUE(t.print('`'));
    t.invokeCharset(terminal::charsets::ActiveSlot::GL, terminal::charsets::Slots::G0, false);
    ASSERT_TRUE(t.print('`'));
    {
        EXPECT_STR("`\xE2\x97\x86\xE2\x97\x86`", t.plainString());
    }
}

TEST(terminal, Terminal__print_invoke_charset_single) {
    TERM(t, 80, 80);

    t.configureCharset(terminal::charsets::Slots::G1, terminal::charsets::Charset::dec_special);

    /* Basic grid writing */
    ASSERT_TRUE(t.print('`'));
    t.invokeCharset(terminal::charsets::ActiveSlot::GL, terminal::charsets::Slots::G1, true);
    ASSERT_TRUE(t.print('`'));
    ASSERT_TRUE(t.print('`'));
    {
        EXPECT_STR("`\xE2\x97\x86`", t.plainString());
    }
}

/* Wisp: upstream test "Terminal__print_kitty_unicode_placeholder" is gated on build_options.kitty_graphics, which this
 * build disables; not ported. */


TEST(terminal, Terminal__soft_wrap) {
    TERM(t, 3, 80);

    /* Basic grid writing */
    PRINT_EACH(t, "hello");
    ASSERT_TRUE(1 == t.screens.active->cursor.y);
    ASSERT_TRUE(2 == t.screens.active->cursor.x);
    {
        EXPECT_STR("hel\nlo", t.plainString());
    }
}

TEST(terminal, Terminal__soft_wrap_with_semantic_prompt) {
    TERM(t, 3, 80);

    /* Mark our prompt. */
    ASSERT_TRUE(t.semanticPrompt(SemanticPromptCommand::init(SemanticPromptCommand::Action::prompt_start)));
    /* Should not make anything dirty on its own. */
    ASSERT_TRUE(!t.isDirty(Point::screen(0, 0)));

    /* Write and wrap */
    PRINT_EACH(t, "hello");
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        ASSERT_TRUE(Row::SemanticPrompt::prompt == list_cell.row->semantic_prompt());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 1)).value;
        ASSERT_TRUE(Row::SemanticPrompt::prompt_continuation == list_cell.row->semantic_prompt());
    }
}

TEST(terminal, Terminal__disabled_wraparound_with_wide_char_and_one_space) {
    TERM(t, 5, 5);

    t.modes.set(terminal::modes::Mode::wraparound, false);

    /* This puts our cursor at the end and there is NO SPACE for a
     * wide character. */
    ASSERT_TRUE(t.printString("AAAA"));
    t.clearDirty();
    /* Police car light */
    ASSERT_TRUE(t.print(0x1F6A8));
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(4 == t.screens.active->cursor.x);

    {
        EXPECT_STR("AAAA", t.plainString());
    }

    /* Make sure we printed nothing */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(4, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }

    /* Should not be dirty since we didn't modify anything */
    ASSERT_TRUE(!t.isDirty(Point::screen(0, 0)));
}

TEST(terminal, Terminal__disabled_wraparound_with_wide_char_and_no_space) {
    TERM(t, 5, 5);

    t.modes.set(terminal::modes::Mode::wraparound, false);

    /* This puts our cursor at the end and there is NO SPACE for a
     * wide character. */
    ASSERT_TRUE(t.printString("AAAAA"));
    t.clearDirty();
    /* Police car light */
    ASSERT_TRUE(t.print(0x1F6A8));
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(4 == t.screens.active->cursor.x);

    {
        EXPECT_STR("AAAAA", t.plainString());
    }

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(4, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('A' == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }

    /* Should not be dirty since we didn't modify anything */
    ASSERT_TRUE(!t.isDirty(Point::screen(0, 0)));
}

TEST(terminal, Terminal__disabled_wraparound_with_wide_grapheme_and_half_space) {
    TERM(t, 5, 5);

    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);
    t.modes.set(terminal::modes::Mode::wraparound, false);

    /* This puts our cursor at the end and there is NO SPACE for a
     * wide character. */
    ASSERT_TRUE(t.printString("AAAA"));
    /* Heart */
    ASSERT_TRUE(t.print(0x2764));
    t.clearDirty();
    /* VS16 to make wide */
    ASSERT_TRUE(t.print(0xFE0F));
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(4 == t.screens.active->cursor.x);

    {
        EXPECT_STR("AAAA\xE2\x9D\xA4", t.plainString());
    }

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(4, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x2764 == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }

    /* Should not be dirty since we didn't modify anything */
    ASSERT_TRUE(!t.isDirty(Point::screen(0, 0)));
}

TEST(terminal, Terminal__print_right_margin_wrap) {
    TERM(t, 10, 5);

    ASSERT_TRUE(t.printString("123456789"));
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setLeftAndRightMargin(3, 5);
    t.setCursorPos(1, 5);
    ASSERT_TRUE(t.printString("XY"));

    {
        EXPECT_STR("1234X6789\n  Y", t.plainString());
    }

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 0)).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(!row->wrap());
    }
}

TEST(terminal, Terminal__print_right_margin_wrap_dirty_tracking) {
    TERM(t, 10, 5);

    ASSERT_TRUE(t.printString("123456789"));
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setLeftAndRightMargin(3, 5);
    t.setCursorPos(1, 5);

    /* Writing our X on the first line should mark only that line dirty. */
    t.clearDirty();
    ASSERT_TRUE(t.print('X'));
    ASSERT_TRUE(t.isDirty(Point::screen(4, 0)));
    ASSERT_TRUE(!t.isDirty(Point::screen(2, 1)));

    /* Writing our Y should wrap. It marks both rows dirty because the
     * cursor moved. */
    t.clearDirty();
    ASSERT_TRUE(t.print('Y'));
    ASSERT_TRUE(t.isDirty(Point::screen(4, 0)));
    ASSERT_TRUE(t.isDirty(Point::screen(2, 1)));

    {
        EXPECT_STR("1234X6789\n  Y", t.plainString());
    }
}

TEST(terminal, Terminal__print_right_margin_outside) {
    TERM(t, 10, 5);

    ASSERT_TRUE(t.printString("123456789"));
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setLeftAndRightMargin(3, 5);
    t.setCursorPos(1, 6);
    t.clearDirty();
    ASSERT_TRUE(t.printString("XY"));

    {
        EXPECT_STR("12345XY89", t.plainString());
    }

    ASSERT_TRUE(t.isDirty(Point::screen(5, 0)));
}

TEST(terminal, Terminal__print_right_margin_outside_wrap) {
    TERM(t, 10, 5);

    ASSERT_TRUE(t.printString("123456789"));
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setLeftAndRightMargin(3, 5);
    t.setCursorPos(1, 10);
    ASSERT_TRUE(t.printString("XY"));

    {
        EXPECT_STR("123456789X\n  Y", t.plainString());
    }
}

TEST(terminal, Terminal__print_wide_char_at_right_margin_does_not_create_spacer_head) {
    TERM(t, 10, 10);

    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setLeftAndRightMargin(3, 5);
    t.setCursorPos(1, 5);
    /* Smiley face */
    ASSERT_TRUE(t.print(0x1F600));
    ASSERT_TRUE(1 == t.screens.active->cursor.y);
    ASSERT_TRUE(4 == t.screens.active->cursor.x);

    /* Both rows dirty because the cursor moved */
    ASSERT_TRUE(t.isDirty(Point::screen(4, 0)));
    ASSERT_TRUE(t.isDirty(Point::screen(4, 1)));

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(4, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());

        const Row *row = list_cell.row;
        ASSERT_TRUE(!row->wrap());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(2, 1)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x1F600 == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(3, 1)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
    }
}

TEST(terminal, Terminal__print_with_hyperlink) {
    TERM(t, 80, 80);

    /* Setup our hyperlink and print */
    NOERR(startLink(*t.screens.active, "http://example.com"));
    ASSERT_TRUE(t.printString("123456"));

    /* Verify all our cells have a hyperlink */
    for (size_t x = 0; x < (size_t)(6); x++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen((uint32_t)(x), 0)).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(row->hyperlink());
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(cell->hyperlink());
        hyperlink::Id id = 0;
        const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
        ASSERT_TRUE(id_found);
        ASSERT_TRUE(1 == id);
    }

    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
}

TEST(terminal, Terminal__print_over_cell_with_same_hyperlink) {
    TERM(t, 80, 80);

    /* Setup our hyperlink and print */
    NOERR(startLink(*t.screens.active, "http://example.com"));
    ASSERT_TRUE(t.printString("123456"));
    t.setCursorPos(1, 1);
    ASSERT_TRUE(t.printString("123456"));

    /* Verify all our cells have a hyperlink */
    for (size_t x = 0; x < (size_t)(6); x++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen((uint32_t)(x), 0)).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(row->hyperlink());
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(cell->hyperlink());
        hyperlink::Id id = 0;
        const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
        ASSERT_TRUE(id_found);
        ASSERT_TRUE(1 == id);
    }

    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
}

TEST(terminal, Terminal__print_and_end_hyperlink) {
    TERM(t, 80, 80);

    /* Setup our hyperlink and print */
    NOERR(startLink(*t.screens.active, "http://example.com"));
    ASSERT_TRUE(t.printString("123"));
    t.screens.active->endHyperlink();
    ASSERT_TRUE(t.printString("456"));

    /* Verify all our cells have a hyperlink */
    for (size_t x = 0; x < (size_t)(3); x++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen((uint32_t)(x), 0)).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(row->hyperlink());
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(cell->hyperlink());
        hyperlink::Id id = 0;
        const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
        ASSERT_TRUE(id_found);
        ASSERT_TRUE(1 == id);
    }
    for (size_t x = (size_t)(3), n_x = (size_t)(6); x < n_x; x++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen((uint32_t)(x), 0)).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(row->hyperlink());
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(!cell->hyperlink());
    }

    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
}

TEST(terminal, Terminal__print_and_change_hyperlink) {
    TERM(t, 80, 80);

    /* Setup our hyperlink and print */
    NOERR(startLink(*t.screens.active, "http://one.example.com"));
    ASSERT_TRUE(t.printString("123"));
    NOERR(startLink(*t.screens.active, "http://two.example.com"));
    ASSERT_TRUE(t.printString("456"));

    /* Verify all our cells have a hyperlink */
    for (size_t x = 0; x < (size_t)(3); x++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen((uint32_t)(x), 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(cell->hyperlink());
        hyperlink::Id id = 0;
        const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
        ASSERT_TRUE(id_found);
        ASSERT_TRUE(1 == id);
    }
    for (size_t x = (size_t)(3), n_x = (size_t)(6); x < n_x; x++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen((uint32_t)(x), 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(cell->hyperlink());
        hyperlink::Id id = 0;
        const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
        ASSERT_TRUE(id_found);
        ASSERT_TRUE(2 == id);
    }

    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
}

TEST(terminal, Terminal__overwrite_hyperlink) {
    TERM(t, 80, 80);

    /* Setup our hyperlink and print */
    NOERR(startLink(*t.screens.active, "http://one.example.com"));
    ASSERT_TRUE(t.printString("123"));
    t.setCursorPos(1, 1);
    t.screens.active->endHyperlink();
    ASSERT_TRUE(t.printString("456"));

    /* Verify all our cells have a hyperlink */
    for (size_t x = 0; x < (size_t)(3); x++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen((uint32_t)(x), 0)).value;
        Page *page = list_cell.node->page();
        const Row *row = list_cell.row;
        ASSERT_TRUE(!row->hyperlink());
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(!cell->hyperlink());
        { hyperlink::Id _hid = 0; ASSERT_TRUE(!page->lookupHyperlink(cell, &_hid)); }
        ASSERT_TRUE(0 == page->hyperlink_set.count());
    }

    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
}

TEST(terminal, Terminal__print_wide_char_at_right_edge_with_hyperlink) {
    TERM(t, 10, 5);

    NOERR(startLink(*t.screens.active, "http://example.com"));

    /* Move cursor to the last column (1-indexed) */
    t.setCursorPos(1, 10);

    /* Print a wide character; this will call printCell(0, .spacer_head)
     * at the right edge before calling printWrap, triggering the
     * integrity violation.
     * U+4E2D '中' */
    ASSERT_TRUE(t.print(0x4E2D));

    /* Cursor wraps to row 2, after the wide char + spacer tail */
    ASSERT_TRUE(1 == t.screens.active->cursor.y);
    ASSERT_TRUE(2 == t.screens.active->cursor.x);

    /* Row 0, col 9: spacer head with hyperlink */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(9, 0)).value;
        ASSERT_TRUE(Cell::Wide::spacer_head == list_cell.cell->wide());
        ASSERT_TRUE(list_cell.cell->hyperlink());
        ASSERT_TRUE(list_cell.row->wrap());
    }
    /* Row 1, col 0: the wide char with hyperlink */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 1)).value;
        ASSERT_TRUE(0x4E2D == list_cell.cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::wide == list_cell.cell->wide());
        ASSERT_TRUE(list_cell.cell->hyperlink());
    }
    /* Row 1, col 1: spacer tail with hyperlink */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 1)).value;
        ASSERT_TRUE(Cell::Wide::spacer_tail == list_cell.cell->wide());
        ASSERT_TRUE(list_cell.cell->hyperlink());
    }
}

/* Wisp: this test drives the terminal through the VT stream, which lands
 * with the port of stream_terminal.zig. The body is transliterated and
 * enabled from there; nothing else in this file depends on it. */
#if 0
TEST(terminal, Terminal__scrollClear_across_pages_keeps_cursor_hyperlink_refs_page_local) {

    /* Minimized from a 774-byte AFL fuzz input. Reading it:
     *
     * A                 print, so REP has something to repeat
     * ESC [ 48111 b     REP, filling the page and spilling onto a second
     * ESC ] 8 ; ; 0x93  OSC 8; the C1 byte terminates the OSC and makes
     * the URI non-empty, so a hyperlink starts
     * ESC [ 11 A        CUU, moving the cursor back onto the first page
     * ESC [ 22 J        ED 22, i.e. scroll_complete -> Screen.scrollClear
     * B                 print, which attaches the cursor hyperlink
     * ESC ] 8 ; ; ESC   OSC 8 with an empty URI, ending the hyperlink
     *
     * The grid must be wide enough to fill a page from a single REP, so
     * this does not reproduce at 80x24. */
    const char *input = "A\x1b[48111b\x1b]8;;\x93\x1b[11A\x1b[22JB\x1b]8;;\x1b";

    TERM(t, 200, 50);

    {
        auto s = t.vtStream();
        s.nextSlice(input, strlen(input));
    }

    /* With slow runtime safety on, the page integrity checks during the
     * stream above already catch the bug. Verify the ref counts explicitly
     * as well so this test is meaningful with runtime safety off: every
     * cell holding a hyperlink ID owns a reference, so a count below the
     * number of holding cells means a live cell points at an entry that
     * was already freed. */
    for (PageList::List::Node *node = t.screens.active->pages.pages.first; node != nullptr; node = node->next) {
        Page *page = node->page();
        const size_t cap = page->hyperlink_set.layout.cap;
        if (cap == 0) continue;

        uint32_t *holders = talloc().allocT<uint32_t>(cap);
        ASSERT_TRUE(holders != nullptr);
        memset(holders, 0, cap * sizeof(uint32_t));

        for (size_t y = 0; y < page->size.rows; y++) {
            Row *row = page->getRow(y);
            if (!row->hyperlink()) continue;
            Cell *cells = page->getCells(row);
            for (size_t x = 0; x < page->size.cols; x++) {
                Cell *cell = &cells[x];
                if (!cell->hyperlink()) continue;
                hyperlink::Id id;
                if (!page->lookupHyperlink(cell, &id)) continue;
                if (id < cap) holders[id] += 1;
            }
        }

        for (size_t id = 0; id < cap; id++) {
            const uint32_t held = holders[id];
            if (held == 0) continue;
            const size::CellCountInt refs = page->hyperlink_set.refCount(page->memory, (hyperlink::Id)id);
            ASSERT_TRUE(refs >= held);
        }

        talloc().freeT<uint32_t>(holders, cap);
    }

    /* If the cursor still has an active hyperlink, its own extra
     * reference must live on the cursor's page. */
    const Screen::Cursor *cursor = &t.screens.active->cursor;
    if (cursor->hyperlink_id != 0) {
        Page *page = cursor->page_pin.node->page();
        ASSERT_TRUE(page->hyperlink_set.refCount(page->memory, cursor->hyperlink_id) > 0);
    }
}
#endif

TEST(terminal, Terminal__linefeed_and_carriage_return) {
    TERM(t, 80, 80);

    /* Print and CR. */
    PRINT_EACH(t, "hello");
    t.clearDirty();
    t.carriageReturn();

    /* CR should not mark row dirty because it doesn't change rendering. */
    ASSERT_TRUE(!t.isDirty(Point::screen(0, 0)));

    ASSERT_TRUE(t.linefeed());

    /* LF marks row dirty due to cursor movement */
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 1)));

    PRINT_EACH(t, "world");
    ASSERT_TRUE(1 == t.screens.active->cursor.y);
    ASSERT_TRUE(5 == t.screens.active->cursor.x);
    {
        EXPECT_STR("hello\nworld", t.plainString());
    }
}

TEST(terminal, Terminal__linefeed_unsets_pending_wrap) {
    TERM(t, 5, 80);

    /* Basic grid writing */
    PRINT_EACH(t, "hello");
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap == true);
    t.clearDirty();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 1)));
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap == false);
}

TEST(terminal, Terminal__linefeed_mode_automatic_carriage_return) {
    TERM(t, 10, 10);

    /* Basic grid writing */
    t.modes.set(terminal::modes::Mode::linefeed, true);
    ASSERT_TRUE(t.printString("123456"));
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('X'));
    {
        EXPECT_STR("123456\nX", t.plainString());
    }
}

TEST(terminal, Terminal__carriage_return_unsets_pending_wrap) {
    TERM(t, 5, 80);

    /* Basic grid writing */
    PRINT_EACH(t, "hello");
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap == true);
    t.carriageReturn();
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap == false);
}

TEST(terminal, Terminal__carriage_return_origin_mode_moves_to_left_margin) {
    TERM(t, 5, 80);

    t.modes.set(terminal::modes::Mode::origin, true);
    t.screens.active->cursor.x = 0;
    t.scrolling_region.left = 2;
    t.carriageReturn();
    ASSERT_TRUE(2 == t.screens.active->cursor.x);
}

TEST(terminal, Terminal__carriage_return_left_of_left_margin_moves_to_zero) {
    TERM(t, 5, 80);

    t.screens.active->cursor.x = 1;
    t.scrolling_region.left = 2;
    t.carriageReturn();
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
}

TEST(terminal, Terminal__carriage_return_right_of_left_margin_moves_to_left_margin) {
    TERM(t, 5, 80);

    t.screens.active->cursor.x = 3;
    t.scrolling_region.left = 2;
    t.carriageReturn();
    ASSERT_TRUE(2 == t.screens.active->cursor.x);
}

TEST(terminal, Terminal__backspace) {
    TERM(t, 80, 80);

    /* BS */
    PRINT_EACH(t, "hello");
    t.backspace();
    ASSERT_TRUE(t.print('y'));
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(5 == t.screens.active->cursor.x);
    {
        EXPECT_STR("helly", t.plainString());
    }
}

TEST(terminal, Terminal__horizontal_tabs) {
    TERM(t, 20, 5);

    /* HT */
    ASSERT_TRUE(t.print('1'));
    t.horizontalTab();
    ASSERT_TRUE(8 == t.screens.active->cursor.x);

    /* HT */
    t.horizontalTab();
    ASSERT_TRUE(16 == t.screens.active->cursor.x);

    /* HT at the end */
    t.horizontalTab();
    ASSERT_TRUE(19 == t.screens.active->cursor.x);
    t.horizontalTab();
    ASSERT_TRUE(19 == t.screens.active->cursor.x);
}

TEST(terminal, Terminal__horizontal_tabs_starting_on_tabstop) {
    TERM(t, 20, 5);

    t.setCursorPos(t.screens.active->cursor.y, 9);
    ASSERT_TRUE(t.print('X'));
    t.setCursorPos(t.screens.active->cursor.y, 9);
    t.horizontalTab();
    ASSERT_TRUE(t.print('A'));

    {
        EXPECT_STR("        X       A", t.plainString());
    }
}

TEST(terminal, Terminal__horizontal_tabs_with_right_margin) {
    TERM(t, 20, 5);

    t.scrolling_region.left = 2;
    t.scrolling_region.right = 5;
    t.setCursorPos(t.screens.active->cursor.y, 1);
    ASSERT_TRUE(t.print('X'));
    t.horizontalTab();
    ASSERT_TRUE(t.print('A'));

    {
        EXPECT_STR("X    A", t.plainString());
    }
}

TEST(terminal, Terminal__horizontal_tabs_back) {
    TERM(t, 20, 5);

    /* Edge of screen */
    t.setCursorPos(t.screens.active->cursor.y, 20);

    /* HT */
    t.horizontalTabBack();
    ASSERT_TRUE(16 == t.screens.active->cursor.x);

    /* HT */
    t.horizontalTabBack();
    ASSERT_TRUE(8 == t.screens.active->cursor.x);

    /* HT */
    t.horizontalTabBack();
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    t.horizontalTabBack();
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
}

TEST(terminal, Terminal__horizontal_tabs_back_starting_on_tabstop) {
    TERM(t, 20, 5);

    t.setCursorPos(t.screens.active->cursor.y, 9);
    ASSERT_TRUE(t.print('X'));
    t.setCursorPos(t.screens.active->cursor.y, 9);
    t.horizontalTabBack();
    ASSERT_TRUE(t.print('A'));

    {
        EXPECT_STR("A       X", t.plainString());
    }
}

TEST(terminal, Terminal__horizontal_tabs_with_left_margin_in_origin_mode) {
    TERM(t, 20, 5);

    t.modes.set(terminal::modes::Mode::origin, true);
    t.scrolling_region.left = 2;
    t.scrolling_region.right = 5;
    t.setCursorPos(1, 2);
    ASSERT_TRUE(t.print('X'));
    t.horizontalTabBack();
    ASSERT_TRUE(t.print('A'));

    {
        EXPECT_STR("  AX", t.plainString());
    }
}

TEST(terminal, Terminal__horizontal_tab_back_with_cursor_before_left_margin) {
    TERM(t, 20, 5);

    t.modes.set(terminal::modes::Mode::origin, true);
    t.saveCursor();
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setLeftAndRightMargin(5, 0);
    t.restoreCursor();
    t.horizontalTabBack();
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("X", t.plainString());
    }
}

TEST(terminal, Terminal__cursorPos_resets_wrap) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);
    t.setCursorPos(1, 1);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("XBCDE", t.plainString());
    }
}

TEST(terminal, Terminal__cursorPos_off_the_screen) {
    TERM(t, 5, 5);

    t.setCursorPos(500, 500);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("\n\n\n\n    X", t.plainString());
    }
}

TEST(terminal, Terminal__cursorPos_relative_to_origin) {
    TERM(t, 5, 5);

    t.scrolling_region.top = 2;
    t.scrolling_region.bottom = 3;
    t.modes.set(terminal::modes::Mode::origin, true);
    t.setCursorPos(1, 1);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("\n\nX", t.plainString());
    }
}

TEST(terminal, Terminal__cursorPos_relative_to_origin_with_left_right) {
    TERM(t, 5, 5);

    t.scrolling_region.top = 2;
    t.scrolling_region.bottom = 3;
    t.scrolling_region.left = 2;
    t.scrolling_region.right = 4;
    t.modes.set(terminal::modes::Mode::origin, true);
    t.setCursorPos(1, 1);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("\n\n  X", t.plainString());
    }
}

TEST(terminal, Terminal__cursorPos_limits_with_full_scroll_region) {
    TERM(t, 5, 5);

    t.scrolling_region.top = 2;
    t.scrolling_region.bottom = 3;
    t.scrolling_region.left = 2;
    t.scrolling_region.right = 4;
    t.modes.set(terminal::modes::Mode::origin, true);
    t.setCursorPos(500, 500);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("\n\n\n    X", t.plainString());
    }
}

TEST(terminal, Terminal__setCursorPos__original_test_) {
    TERM(t, 80, 80);

    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    ASSERT_TRUE(0 == t.screens.active->cursor.y);

    /* Setting it to 0 should keep it zero (1 based) */
    t.setCursorPos(0, 0);
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    ASSERT_TRUE(0 == t.screens.active->cursor.y);

    /* Should clamp to size */
    t.setCursorPos(81, 81);
    ASSERT_TRUE(79 == t.screens.active->cursor.x);
    ASSERT_TRUE(79 == t.screens.active->cursor.y);

    /* Should reset pending wrap */
    t.setCursorPos(0, 80);
    ASSERT_TRUE(t.print('c'));
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);
    t.setCursorPos(0, 80);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);

    /* Origin mode */
    t.modes.set(terminal::modes::Mode::origin, true);

    /* No change without a scroll region */
    t.setCursorPos(81, 81);
    ASSERT_TRUE(79 == t.screens.active->cursor.x);
    ASSERT_TRUE(79 == t.screens.active->cursor.y);

    /* Set the scroll region */
    t.setTopAndBottomMargin(10, t.rows);
    t.setCursorPos(0, 0);
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    ASSERT_TRUE(9 == t.screens.active->cursor.y);

    t.setCursorPos(1, 1);
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    ASSERT_TRUE(9 == t.screens.active->cursor.y);

    t.setCursorPos(100, 0);
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    ASSERT_TRUE(79 == t.screens.active->cursor.y);

    t.setTopAndBottomMargin(10, 11);
    t.setCursorPos(2, 0);
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    ASSERT_TRUE(10 == t.screens.active->cursor.y);
}

TEST(terminal, Terminal__setTopAndBottomMargin_simple) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.setTopAndBottomMargin(0, 0);

    t.clearDirty();
    t.scrollDown(1);

    /* Mark the rows we moved as dirty. */
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 3)));

    {
        EXPECT_STR("\nABC\nDEF\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__setTopAndBottomMargin_top_only) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.setTopAndBottomMargin(2, 0);

    t.clearDirty();
    t.scrollDown(1);

    /* This is dirty because the cursor moves from this row */
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 3)));

    {
        EXPECT_STR("ABC\n\nDEF\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__setTopAndBottomMargin_top_and_bottom) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.setTopAndBottomMargin(1, 2);

    t.clearDirty();
    t.scrollDown(1);

    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(!t.isDirty(Point::active(0, 2)));

    {
        EXPECT_STR("\nABC\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__setTopAndBottomMargin_top_equal_to_bottom) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.setTopAndBottomMargin(2, 2);

    t.clearDirty();
    t.scrollDown(1);

    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 3)));

    {
        EXPECT_STR("\nABC\nDEF\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__setLeftAndRightMargin_simple) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setLeftAndRightMargin(0, 0);

    t.clearDirty();
    t.eraseChars(1);

    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(!t.isDirty(Point::active(0, 1)));

    {
        EXPECT_STR(" BC\nDEF\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__setLeftAndRightMargin_left_only) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setLeftAndRightMargin(2, 0);
    ASSERT_TRUE(1 == t.scrolling_region.left);
    ASSERT_TRUE(t.cols - 1 == t.scrolling_region.right);
    t.setCursorPos(1, 2);

    t.clearDirty();
    t.insertLines(1);

    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 3)));

    {
        EXPECT_STR("A\nDBC\nGEF\n HI", t.plainString());
    }
}

TEST(terminal, Terminal__setLeftAndRightMargin_left_and_right) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setLeftAndRightMargin(1, 2);
    t.setCursorPos(1, 2);

    t.clearDirty();
    t.insertLines(1);

    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 3)));

    {
        EXPECT_STR("  C\nABF\nDEI\nGH", t.plainString());
    }
}

TEST(terminal, Terminal__setLeftAndRightMargin_left_equal_right) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setLeftAndRightMargin(2, 2);
    t.setCursorPos(1, 2);

    t.clearDirty();
    t.insertLines(1);

    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 3)));

    {
        EXPECT_STR("\nABC\nDEF\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__setLeftAndRightMargin_mode_69_unset) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, false);
    t.setLeftAndRightMargin(1, 2);
    t.setCursorPos(1, 2);

    t.clearDirty();
    t.insertLines(1);

    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 3)));

    {
        EXPECT_STR("\nABC\nDEF\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__insertLines_simple) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.setCursorPos(2, 2);

    PageList::Node *node = t.screens.active->cursor.page_pin->node;
    const uint64_t serial = node->serial;
    t.clearDirty();
    t.insertLines(1);
    ASSERT_TRUE(!t.screens.active->pages.nodeIsValid(node, serial));

    ASSERT_TRUE(!t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 3)));

    {
        EXPECT_STR("ABC\n\nDEF\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__insertLines_colors_with_bg_color) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.setCursorPos(2, 2);

    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 0xFF, 0, 0)));
    t.insertLines(1);

    {
        EXPECT_STR("ABC\n\nDEF\nGHI", t.plainString());
    }

    for (size_t x = 0; x < (size_t)(t.cols); x++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active((uint32_t)(x), 1)).value;
        ASSERT_TRUE(list_cell.cell->content_tag() == Cell::ContentTag::bg_color_rgb);
        {
            const Cell::RGB _rgb = list_cell.cell->contentColorRgb();
            ASSERT_TRUE(_rgb.r == 0xFF && _rgb.g == 0 && _rgb.b == 0);
        }
    }
}

TEST(terminal, Terminal__insertLines_handles_style_refs) {
    TERM(t, 5, 3);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());

    /* For the line being deleted, create a refcounted style */
    NOERR(t.setAttribute(attr(A::bold)));
    ASSERT_TRUE(t.printString("GHI"));
    NOERR(t.setAttribute(attr(A::unset)));

    /* verify we have styles in our style map */
    Page *page = t.screens.active->cursor.page_pin->node->page();
    ASSERT_TRUE(1 == page->styles.count());

    t.setCursorPos(2, 2);
    t.insertLines(1);

    {
        EXPECT_STR("ABC\n\nDEF", t.plainString());
    }

    /* verify we have no styles in our style map */
    ASSERT_TRUE(0 == page->styles.count());
}

TEST(terminal, Terminal__insertLines_outside_of_scroll_region) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.setTopAndBottomMargin(3, 4);
    t.setCursorPos(2, 2);

    t.clearDirty();
    t.insertLines(1);

    ASSERT_TRUE(!t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(!t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(!t.isDirty(Point::active(0, 2)));

    {
        EXPECT_STR("ABC\nDEF\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__insertLines_top_bottom_scroll_region) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("123"));
    t.setTopAndBottomMargin(1, 3);
    t.setCursorPos(2, 2);

    t.clearDirty();
    t.insertLines(1);

    ASSERT_TRUE(!t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));
    ASSERT_TRUE(!t.isDirty(Point::active(0, 3)));

    {
        EXPECT_STR("ABC\n\nDEF\n123", t.plainString());
    }
}

TEST(terminal, Terminal__insertLines_across_page_boundary_marks_all_shifted_rows_dirty) {
    TERM(t, 10, 5);
    t_opts.max_scrollback_bytes = Maybe<size_t>((size_t)1024);
    ASSERT_TRUE(t_holder.reinit(t_opts));

    PageList::List::Node *first_page = t.screens.active->pages.pages.first;
    const auto first_page_nrows = first_page->capacity().rows;

    /* Fill up the first page minus 3 rows */
    for (size_t i_ = (size_t)(0); i_ < (size_t)(first_page_nrows - 3); i_++) ASSERT_TRUE(t.linefeed());

    /* Add content that will cross a page boundary */
    ASSERT_TRUE(t.printString("1AAAA"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("2BBBB"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("3CCCC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("4DDDD"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("5EEEE"));

    /* Verify we now have a second page */
    PageList::List::Node *second_page = first_page->next;
    const uint64_t first_serial = first_page->serial;
    const uint64_t second_serial = second_page->serial;

    t.setCursorPos(1, 1);
    t.clearDirty();
    t.insertLines(1);
    ASSERT_TRUE(!t.screens.active->pages.nodeIsValid(first_page, first_serial));
    ASSERT_TRUE(!t.screens.active->pages.nodeIsValid(second_page, second_serial));

    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 3)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 4)));

    {
        EXPECT_STR("\n1AAAA\n2BBBB\n3CCCC\n4DDDD", t.plainString());
    }
}

TEST(terminal, Terminal__insertLines_hyperlink_dense_row_crosses_page_boundary) {
    /* Regression test for the cross-page copy of insertLines: when the
     * shifted row carries more unique hyperlinks than the destination
     * page's hyperlink capacity, the copy must increase the destination
     * page's capacity and retry rather than corrupting the page list. */
    TERM(t, 10, 5);
    t_opts.max_scrollback_bytes = Maybe<size_t>((size_t)1024);
    ASSERT_TRUE(t_holder.reinit(t_opts));

    PageList &pages = t.screens.active->pages;

    /* Fill the first page so it is exactly full, then two more rows so
     * the second page holds the last two active rows (y=3 and y=4). */
    const auto first_page_rows = pages.pages.first->capacity().rows;
    for (size_t i_ = (size_t)(0); i_ < (size_t)(first_page_rows + 1); i_++) ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(pages.pages.first != pages.pages.last);
    ASSERT_TRUE(2 == pages.pages.last->rows());

    /* Marker rows so we can verify the shift afterwards. */
    t.setCursorPos(1, 1);
    ASSERT_TRUE(t.printString("0"));
    t.setCursorPos(2, 1);
    ASSERT_TRUE(t.printString("1"));
    t.setCursorPos(4, 1);
    ASSERT_TRUE(t.printString("3"));
    t.setCursorPos(5, 1);
    ASSERT_TRUE(t.printString("4"));

    /* Fill the last row of the first page (active y=2) with unique
     * hyperlinks: more than the second page can hold with its default
     * hyperlink capacity. Writing them grows the first page's capacity
     * as needed; the second page keeps its default capacity. */
    t.setCursorPos(3, 1);
    for (size_t i = 0; i < (size_t)(10); i++) {
        char buf[64];
        snprintf(buf, sizeof buf, "http://example.com/%zu", (size_t)i);
        const char *uri = buf;
        NOERR(startLink(*t.screens.active, uri));
        ASSERT_TRUE(t.print((uint32_t)('A' + i)));
        t.screens.active->endHyperlink();
    }
    {
        const Pin pin = pages.pin(Point::active(0, 2)).value;
        ASSERT_TRUE(pages.pages.first == pin.node);
        ASSERT_TRUE(pin.node->rows() - 1 == pin.y);
    }
    ASSERT_TRUE(pages.pages.last->page()->hyperlink_set.layout.cap < 10);

    /* Insert a line at the top: every row shifts down by one and the
     * dense row crosses the page boundary into the second page. */
    t.setCursorPos(1, 1);
    t.insertLines(1);

    {
        EXPECT_STR("\n0\n1\nABCDEFGHIJ\n3", t.plainString());
    }

    /* The second page's hyperlink capacity had to grow to receive the
     * row, proving the capacity-retry path ran. */
    ASSERT_TRUE(pages.pages.last->page()->hyperlink_set.layout.cap >= 10);

    /* Every cell of the dense row must still resolve to a real
     * hyperlink entry with the correct URI. A half-applied shift
     * leaves cells whose hyperlink flag is set but that have no map
     * entry, which aborts in clearCells later. */
    for (size_t x = 0; x < (size_t)(10); x++) {
        const PageList::Cell list_cell = pages.getCell(Point::active((uint32_t)(x), 3)).value;
        ASSERT_TRUE(list_cell.cell->hyperlink());
        Page *page = list_cell.node->page();
        hyperlink::Id id = 0;
        ASSERT_TRUE(page->lookupHyperlink(list_cell.cell, &id));
        const auto link = page->hyperlink_set.get((const void *)page->memory, id);
        char buf[64];
        snprintf(buf, sizeof buf, "http://example.com/%zu", (size_t)x);
        const char *uri = buf;
        EXPECT_STR(uri, std::string((const char *)link->uri.slice((const void *)page->memory), link->uri.len));
    }

    /* All pages must pass integrity checks. */
    for (PageList::List::Node *node = pages.pages.first; node != nullptr; node = node->next)
        node->page()->assertIntegrity();
}

TEST(terminal, Terminal__insertLines__legacy_test_) {
    TERM(t, 2, 5);

    /* Initial value */
    ASSERT_TRUE(t.print('A'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('B'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('C'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('D'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('E'));

    /* Move to row 2 */
    t.setCursorPos(2, 1);

    /* Insert two lines */
    t.insertLines(2);

    {
        EXPECT_STR("A\n\n\nB\nC", t.plainString());
    }
}

TEST(terminal, Terminal__insertLines_zero) {
    TERM(t, 2, 5);

    /* This should do nothing */
    t.setCursorPos(1, 1);
    t.insertLines(0);
}

TEST(terminal, Terminal__insertLines_with_scroll_region) {
    TERM(t, 2, 6);

    /* Initial value */
    ASSERT_TRUE(t.print('A'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('B'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('C'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('D'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('E'));

    t.setTopAndBottomMargin(1, 2);
    t.setCursorPos(1, 1);

    t.clearDirty();
    t.insertLines(1);

    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(!t.isDirty(Point::active(0, 2)));

    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("X\nA\nC\nD\nE", t.plainString());
    }
}

TEST(terminal, Terminal__insertLines_more_than_remaining) {
    TERM(t, 2, 5);

    /* Initial value */
    ASSERT_TRUE(t.print('A'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('B'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('C'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('D'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('E'));

    /* Move to row 2 */
    t.setCursorPos(2, 1);

    /* Insert a bunch of  lines */
    t.clearDirty();
    t.insertLines(20);

    ASSERT_TRUE(!t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));

    {
        EXPECT_STR("A", t.plainString());
    }
}

TEST(terminal, Terminal__insertLines_resets_pending_wrap) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);
    t.insertLines(1);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);
    ASSERT_TRUE(t.print('B'));

    {
        EXPECT_STR("B\nABCDE", t.plainString());
    }
}

TEST(terminal, Terminal__insertLines_resets_wrap) {
    TERM(t, 3, 3);

    ASSERT_TRUE(t.print('1'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "ABCDEF");
    t.setCursorPos(1, 1);
    t.insertLines(1);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("X\n1\nABC", t.plainString());
    }

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 2)).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(!row->wrap());
    }
}

TEST(terminal, Terminal__insertLines_multi_codepoint_graphemes) {
    TERM(t, 5, 5);

    /* Disable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());

    /* This is: 👨‍👩‍👧 (which may or may not render correctly) */
    ASSERT_TRUE(t.print(0x1F468));
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(t.print(0x1F469));
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(t.print(0x1F467));

    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.setCursorPos(2, 2);
    t.insertLines(1);

    {
        EXPECT_STR("ABC\n\n\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__insertLines_left_right_scroll_region) {
    TERM(t, 10, 10);

    ASSERT_TRUE(t.printString("ABC123"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF456"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI789"));
    t.scrolling_region.left = 1;
    t.scrolling_region.right = 3;
    t.setCursorPos(2, 2);

    t.clearDirty();
    t.insertLines(1);

    ASSERT_TRUE(!t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 3)));

    {
        EXPECT_STR("ABC123\nD   56\nGEF489\n HI7", t.plainString());
    }
}

TEST(terminal, Terminal__scrollUp_simple) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.setCursorPos(2, 2);

    const Screen::Cursor cursor = t.screens.active->cursor;
    const auto viewport_before = t.screens.active->pages.getTopLeft(point::Tag::viewport);
    ASSERT_TRUE(t.scrollUp(1));
    ASSERT_TRUE(cursor.x == t.screens.active->cursor.x);
    ASSERT_TRUE(cursor.y == t.screens.active->cursor.y);

    /* Viewport should have moved. Our entire page should've scrolled!
     * The viewport moving will cause our render state to make the full
     * frame as dirty. */
    const auto viewport_after = t.screens.active->pages.getTopLeft(point::Tag::viewport);
    ASSERT_TRUE(!viewport_before.eql(viewport_after));

    {
        EXPECT_STR("DEF\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__scrollUp_moves_hyperlink) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    NOERR(startLink(*t.screens.active, "http://example.com"));
    ASSERT_TRUE(t.printString("DEF"));
    t.screens.active->endHyperlink();
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.setCursorPos(2, 2);
    ASSERT_TRUE(t.scrollUp(1));

    {
        EXPECT_STR("DEF\nGHI", t.plainString());
    }

    for (size_t x = 0; x < (size_t)(3); x++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport((uint32_t)(x), 0)).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(row->hyperlink());
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(cell->hyperlink());
        hyperlink::Id id = 0;
        const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
        ASSERT_TRUE(id_found);
        ASSERT_TRUE(1 == id);
        Page *page = list_cell.node->page();
        ASSERT_TRUE(1 == page->hyperlink_set.count());
    }
    for (size_t x = 0; x < (size_t)(3); x++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport((uint32_t)(x), 1)).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(!row->hyperlink());
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(!cell->hyperlink());
        hyperlink::Id id = 0;
        const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
        (void)id_found;
        ASSERT_TRUE(!id_found);
    }
}

TEST(terminal, Terminal__scrollUp_clears_hyperlink) {
    TERM(t, 5, 5);

    NOERR(startLink(*t.screens.active, "http://example.com"));
    ASSERT_TRUE(t.printString("ABC"));
    t.screens.active->endHyperlink();
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.setCursorPos(2, 2);
    ASSERT_TRUE(t.scrollUp(1));

    {
        EXPECT_STR("DEF\nGHI", t.plainString());
    }

    for (size_t x = 0; x < (size_t)(3); x++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport((uint32_t)(x), 0)).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(!row->hyperlink());
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(!cell->hyperlink());
        hyperlink::Id id = 0;
        const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
        (void)id_found;
        ASSERT_TRUE(!id_found);
    }
}

TEST(terminal, Terminal__scrollUp_top_bottom_scroll_region) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.setTopAndBottomMargin(2, 3);
    t.setCursorPos(1, 1);

    t.clearDirty();
    ASSERT_TRUE(t.scrollUp(1));

    /* This is dirty because the cursor moves from this row */
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));

    {
        EXPECT_STR("ABC\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__scrollUp_left_right_scroll_region) {
    TERM(t, 10, 10);

    ASSERT_TRUE(t.printString("ABC123"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF456"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI789"));
    t.scrolling_region.left = 1;
    t.scrolling_region.right = 3;
    t.setCursorPos(2, 2);

    const Screen::Cursor cursor = t.screens.active->cursor;
    t.clearDirty();
    ASSERT_TRUE(t.scrollUp(1));
    ASSERT_TRUE(cursor.x == t.screens.active->cursor.x);
    ASSERT_TRUE(cursor.y == t.screens.active->cursor.y);

    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));

    {
        EXPECT_STR("AEF423\nDHI756\nG   89", t.plainString());
    }
}

TEST(terminal, Terminal__scrollUp_left_right_scroll_region_hyperlink) {
    TERM(t, 10, 10);

    ASSERT_TRUE(t.printString("ABC123"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    NOERR(startLink(*t.screens.active, "http://example.com"));
    ASSERT_TRUE(t.printString("DEF456"));
    t.screens.active->endHyperlink();
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI789"));
    t.scrolling_region.left = 1;
    t.scrolling_region.right = 3;
    t.setCursorPos(2, 2);
    ASSERT_TRUE(t.scrollUp(1));

    {
        EXPECT_STR("AEF423\nDHI756\nG   89", t.plainString());
    }

    /* First row gets some hyperlinks */
    {
        for (size_t x = 0; x < (size_t)(1); x++) {
            const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport((uint32_t)(x), 0)).value;
            const Cell *cell = list_cell.cell;
            ASSERT_TRUE(!cell->hyperlink());
            hyperlink::Id id = 0;
            const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
            (void)id_found;
            ASSERT_TRUE(!id_found);
        }
        for (size_t x = (size_t)(1), n_x = (size_t)(4); x < n_x; x++) {
            const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport((uint32_t)(x), 0)).value;
            const Row *row = list_cell.row;
            ASSERT_TRUE(row->hyperlink());
            const Cell *cell = list_cell.cell;
            ASSERT_TRUE(cell->hyperlink());
            hyperlink::Id id = 0;
            const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
            ASSERT_TRUE(id_found);
            ASSERT_TRUE(1 == id);
            Page *page = list_cell.node->page();
            ASSERT_TRUE(1 == page->hyperlink_set.count());
        }
        for (size_t x = (size_t)(4), n_x = (size_t)(6); x < n_x; x++) {
            const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport((uint32_t)(x), 0)).value;
            const Cell *cell = list_cell.cell;
            ASSERT_TRUE(!cell->hyperlink());
            hyperlink::Id id = 0;
            const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
            (void)id_found;
            ASSERT_TRUE(!id_found);
        }
    }

    /* Second row preserves hyperlink where we didn't scroll */
    {
        for (size_t x = 0; x < (size_t)(1); x++) {
            const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport((uint32_t)(x), 1)).value;
            const Row *row = list_cell.row;
            ASSERT_TRUE(row->hyperlink());
            const Cell *cell = list_cell.cell;
            ASSERT_TRUE(cell->hyperlink());
            hyperlink::Id id = 0;
            const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
            ASSERT_TRUE(id_found);
            ASSERT_TRUE(1 == id);
            Page *page = list_cell.node->page();
            ASSERT_TRUE(1 == page->hyperlink_set.count());
        }
        for (size_t x = (size_t)(1), n_x = (size_t)(4); x < n_x; x++) {
            const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport((uint32_t)(x), 1)).value;
            const Cell *cell = list_cell.cell;
            ASSERT_TRUE(!cell->hyperlink());
            hyperlink::Id id = 0;
            const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
            (void)id_found;
            ASSERT_TRUE(!id_found);
        }
        for (size_t x = (size_t)(4), n_x = (size_t)(6); x < n_x; x++) {
            const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport((uint32_t)(x), 1)).value;
            const Row *row = list_cell.row;
            ASSERT_TRUE(row->hyperlink());
            const Cell *cell = list_cell.cell;
            ASSERT_TRUE(cell->hyperlink());
            hyperlink::Id id = 0;
            const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
            ASSERT_TRUE(id_found);
            ASSERT_TRUE(1 == id);
            Page *page = list_cell.node->page();
            ASSERT_TRUE(1 == page->hyperlink_set.count());
        }
    }
}

TEST(terminal, Terminal__scrollUp_preserves_pending_wrap) {
    TERM(t, 5, 5);

    t.setCursorPos(1, 5);
    ASSERT_TRUE(t.print('A'));
    t.setCursorPos(2, 5);
    ASSERT_TRUE(t.print('B'));
    t.setCursorPos(3, 5);
    ASSERT_TRUE(t.print('C'));
    ASSERT_TRUE(t.scrollUp(1));
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("    B\n    C\n\nX", t.plainString());
    }
}

TEST(terminal, Terminal__scrollUp_full_top_bottom_region) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("top"));
    t.setCursorPos(5, 1);
    ASSERT_TRUE(t.printString("ABCDE"));
    t.setTopAndBottomMargin(2, 5);

    t.clearDirty();
    ASSERT_TRUE(t.scrollUp(4));

    /* This is dirty because the cursor moves from this row */
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));

    {
        EXPECT_STR("top", t.plainString());
    }
}

TEST(terminal, Terminal__scrollUp_full_top_bottomleft_right_scroll_region) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("top"));
    t.setCursorPos(5, 1);
    ASSERT_TRUE(t.printString("ABCDE"));
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setTopAndBottomMargin(2, 5);
    t.setLeftAndRightMargin(2, 4);

    t.clearDirty();
    ASSERT_TRUE(t.scrollUp(4));

    /* This is dirty because the cursor moves from this row */
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    for (size_t y = (size_t)(1); y < (size_t)(5); y++) ASSERT_TRUE(t.isDirty(Point::active(0, (uint32_t)(y))));

    {
        EXPECT_STR("top\n\n\n\nA   E", t.plainString());
    }
}

TEST(terminal, Terminal__scrollUp_creates_scrollback_in_primary_screen) {
    /* When in primary screen with full-width scroll region at top,
     * scrollUp (CSI S) should push lines into scrollback like xterm. */
    TERM(t, 5, 5);
    t_opts.max_scrollback_bytes = Maybe<size_t>((size_t)10);
    ASSERT_TRUE(t_holder.reinit(t_opts));

    /* Fill the screen with content */
    ASSERT_TRUE(t.printString("AAAAA"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("BBBBB"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("CCCCC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DDDDD"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("EEEEE"));

    t.clearDirty();

    /* Scroll up by 1, which should push "AAAAA" into scrollback */
    ASSERT_TRUE(t.scrollUp(1));

    /* The cursor row (new empty row) should be dirty */
    ASSERT_TRUE(t.screens.active->cursor.page_row->dirty());

    /* The active screen should now show BBBBB through EEEEE plus one blank line */
    {
        EXPECT_STR("BBBBB\nCCCCC\nDDDDD\nEEEEE", t.plainString());
    }

    /* Now scroll to the top to see scrollback - AAAAA should be there */
    t.screens.active->scroll(PageList::Scroll::top());
    {
        const std::string str = t.plainString();
        /* Should see AAAAA in scrollback */
        EXPECT_STR("AAAAA\nBBBBB\nCCCCC\nDDDDD\nEEEEE", str);
    }
}

TEST(terminal, Terminal__scrollUp_with_max_scrollback_bytes_zero) {
    /* When max_scrollback_bytes is 0, scrollUp should still work but not retain history */
    TERM(t, 5, 5);
    t_opts.max_scrollback_bytes = Maybe<size_t>((size_t)0);
    ASSERT_TRUE(t_holder.reinit(t_opts));

    ASSERT_TRUE(t.printString("AAAAA"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("BBBBB"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("CCCCC"));

    ASSERT_TRUE(t.scrollUp(1));

    /* Active screen should show scrolled content */
    {
        EXPECT_STR("BBBBB\nCCCCC", t.plainString());
    }

    /* Scroll to top - should be same as active since no scrollback */
    t.screens.active->scroll(PageList::Scroll::top());
    {
        EXPECT_STR("BBBBB\nCCCCC", t.plainString());
    }
}

TEST(terminal, Terminal__scrollUp_with_max_scrollback_bytes_zero_and_top_margin) {
    /* When max_scrollback_bytes is 0 and top margin is set, should use deleteLines path */
    TERM(t, 5, 5);
    t_opts.max_scrollback_bytes = Maybe<size_t>((size_t)0);
    ASSERT_TRUE(t_holder.reinit(t_opts));

    ASSERT_TRUE(t.printString("AAAAA"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("BBBBB"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("CCCCC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DDDDD"));

    /* Set top margin (not at row 0) */
    t.setTopAndBottomMargin(2, 5);

    ASSERT_TRUE(t.scrollUp(1));

    {
        const std::string str = t.plainString();
        /* First row preserved, rest scrolled */
        EXPECT_STR("AAAAA\nCCCCC\nDDDDD", str);
    }
}

TEST(terminal, Terminal__scrollUp_with_max_scrollback_bytes_zero_and_left_right_margin) {
    /* When max_scrollback_bytes is 0 with left/right margins, uses deleteLines path */
    TERM(t, 10, 5);
    t_opts.max_scrollback_bytes = Maybe<size_t>((size_t)0);
    ASSERT_TRUE(t_holder.reinit(t_opts));

    ASSERT_TRUE(t.printString("AAAAABBBBB"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("CCCCCDDDDD"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("EEEEEFFFFF"));

    /* Set left/right margins (columns 2-6, 1-indexed = indices 1-5) */
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setLeftAndRightMargin(2, 6);

    ASSERT_TRUE(t.scrollUp(1));

    {
        const std::string str = t.plainString();
        /* cols 1-5 scroll, col 0 and cols 6+ preserved */
        EXPECT_STR("ACCCCDBBBB\nCEEEEFDDDD\nE     FFFF", str);
    }
}

TEST(terminal, Terminal__scrollDown_simple) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.setCursorPos(2, 2);

    const Screen::Cursor cursor = t.screens.active->cursor;
    t.clearDirty();
    t.scrollDown(1);
    ASSERT_TRUE(cursor.x == t.screens.active->cursor.x);
    ASSERT_TRUE(cursor.y == t.screens.active->cursor.y);

    for (size_t y = (size_t)(0); y < (size_t)(5); y++) ASSERT_TRUE(t.isDirty(Point::active(0, (uint32_t)(y))));

    {
        EXPECT_STR("\nABC\nDEF\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__scrollDown_hyperlink_moves) {
    TERM(t, 5, 5);

    NOERR(startLink(*t.screens.active, "http://example.com"));
    ASSERT_TRUE(t.printString("ABC"));
    t.screens.active->endHyperlink();
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.setCursorPos(2, 2);
    t.scrollDown(1);

    {
        EXPECT_STR("\nABC\nDEF\nGHI", t.plainString());
    }

    for (size_t x = 0; x < (size_t)(3); x++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport((uint32_t)(x), 1)).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(row->hyperlink());
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(cell->hyperlink());
        hyperlink::Id id = 0;
        const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
        ASSERT_TRUE(id_found);
        ASSERT_TRUE(1 == id);
        Page *page = list_cell.node->page();
        ASSERT_TRUE(1 == page->hyperlink_set.count());
    }
    for (size_t x = 0; x < (size_t)(3); x++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport((uint32_t)(x), 0)).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(!row->hyperlink());
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(!cell->hyperlink());
        hyperlink::Id id = 0;
        const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
        (void)id_found;
        ASSERT_TRUE(!id_found);
    }
}

TEST(terminal, Terminal__scrollDown_outside_of_scroll_region) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.setTopAndBottomMargin(3, 4);
    t.setCursorPos(2, 2);

    const Screen::Cursor cursor = t.screens.active->cursor;
    t.clearDirty();
    t.scrollDown(1);
    ASSERT_TRUE(cursor.x == t.screens.active->cursor.x);
    ASSERT_TRUE(cursor.y == t.screens.active->cursor.y);

    ASSERT_TRUE(!t.isDirty(Point::active(0, 0)));

    /* This is dirty because the cursor moves from this row */
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 3)));

    {
        EXPECT_STR("ABC\nDEF\n\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__scrollDown_left_right_scroll_region) {
    TERM(t, 10, 10);

    ASSERT_TRUE(t.printString("ABC123"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF456"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI789"));
    t.scrolling_region.left = 1;
    t.scrolling_region.right = 3;
    t.setCursorPos(2, 2);

    const Screen::Cursor cursor = t.screens.active->cursor;
    t.clearDirty();
    t.scrollDown(1);
    ASSERT_TRUE(cursor.x == t.screens.active->cursor.x);
    ASSERT_TRUE(cursor.y == t.screens.active->cursor.y);

    for (size_t y = (size_t)(0); y < (size_t)(4); y++) ASSERT_TRUE(t.isDirty(Point::active(0, (uint32_t)(y))));

    {
        EXPECT_STR("A   23\nDBC156\nGEF489\n HI7", t.plainString());
    }
}

TEST(terminal, Terminal__scrollDown_left_right_scroll_region_hyperlink) {
    TERM(t, 10, 10);

    NOERR(startLink(*t.screens.active, "http://example.com"));
    ASSERT_TRUE(t.printString("ABC123"));
    t.screens.active->endHyperlink();
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF456"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI789"));
    t.scrolling_region.left = 1;
    t.scrolling_region.right = 3;
    t.setCursorPos(2, 2);
    t.scrollDown(1);

    {
        EXPECT_STR("A   23\nDBC156\nGEF489\n HI7", t.plainString());
    }

    /* First row preserves hyperlink where we didn't scroll */
    {
        for (size_t x = 0; x < (size_t)(1); x++) {
            const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport((uint32_t)(x), 0)).value;
            const Row *row = list_cell.row;
            ASSERT_TRUE(row->hyperlink());
            const Cell *cell = list_cell.cell;
            ASSERT_TRUE(cell->hyperlink());
            hyperlink::Id id = 0;
            const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
            ASSERT_TRUE(id_found);
            ASSERT_TRUE(1 == id);
            Page *page = list_cell.node->page();
            ASSERT_TRUE(1 == page->hyperlink_set.count());
        }
        for (size_t x = (size_t)(1), n_x = (size_t)(4); x < n_x; x++) {
            const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport((uint32_t)(x), 0)).value;
            const Cell *cell = list_cell.cell;
            ASSERT_TRUE(!cell->hyperlink());
            hyperlink::Id id = 0;
            const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
            (void)id_found;
            ASSERT_TRUE(!id_found);
        }
        for (size_t x = (size_t)(4), n_x = (size_t)(6); x < n_x; x++) {
            const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport((uint32_t)(x), 0)).value;
            const Row *row = list_cell.row;
            ASSERT_TRUE(row->hyperlink());
            const Cell *cell = list_cell.cell;
            ASSERT_TRUE(cell->hyperlink());
            hyperlink::Id id = 0;
            const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
            ASSERT_TRUE(id_found);
            ASSERT_TRUE(1 == id);
            Page *page = list_cell.node->page();
            ASSERT_TRUE(1 == page->hyperlink_set.count());
        }
    }

    /* Second row gets some hyperlinks */
    {
        for (size_t x = 0; x < (size_t)(1); x++) {
            const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport((uint32_t)(x), 1)).value;
            const Cell *cell = list_cell.cell;
            ASSERT_TRUE(!cell->hyperlink());
            hyperlink::Id id = 0;
            const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
            (void)id_found;
            ASSERT_TRUE(!id_found);
        }
        for (size_t x = (size_t)(1), n_x = (size_t)(4); x < n_x; x++) {
            const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport((uint32_t)(x), 1)).value;
            const Row *row = list_cell.row;
            ASSERT_TRUE(row->hyperlink());
            const Cell *cell = list_cell.cell;
            ASSERT_TRUE(cell->hyperlink());
            hyperlink::Id id = 0;
            const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
            ASSERT_TRUE(id_found);
            ASSERT_TRUE(1 == id);
            Page *page = list_cell.node->page();
            ASSERT_TRUE(1 == page->hyperlink_set.count());
        }
        for (size_t x = (size_t)(4), n_x = (size_t)(6); x < n_x; x++) {
            const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport((uint32_t)(x), 1)).value;
            const Cell *cell = list_cell.cell;
            ASSERT_TRUE(!cell->hyperlink());
            hyperlink::Id id = 0;
            const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
            (void)id_found;
            ASSERT_TRUE(!id_found);
        }
    }
}

TEST(terminal, Terminal__scrollDown_outside_of_left_right_scroll_region) {
    TERM(t, 10, 10);

    ASSERT_TRUE(t.printString("ABC123"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF456"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI789"));
    t.scrolling_region.left = 1;
    t.scrolling_region.right = 3;
    t.setCursorPos(1, 1);

    const Screen::Cursor cursor = t.screens.active->cursor;
    t.clearDirty();
    t.scrollDown(1);
    ASSERT_TRUE(cursor.x == t.screens.active->cursor.x);
    ASSERT_TRUE(cursor.y == t.screens.active->cursor.y);

    for (size_t y = (size_t)(0); y < (size_t)(4); y++) ASSERT_TRUE(t.isDirty(Point::active(0, (uint32_t)(y))));

    {
        EXPECT_STR("A   23\nDBC156\nGEF489\n HI7", t.plainString());
    }
}

TEST(terminal, Terminal__scrollDown_preserves_pending_wrap) {
    TERM(t, 5, 10);

    t.setCursorPos(1, 5);
    ASSERT_TRUE(t.print('A'));
    t.setCursorPos(2, 5);
    ASSERT_TRUE(t.print('B'));
    t.setCursorPos(3, 5);
    ASSERT_TRUE(t.print('C'));
    t.scrollDown(1);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("\n    A\n    B\nX   C", t.plainString());
    }
}

TEST(terminal, Terminal__eraseChars_simple_operation) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABC");
    t.setCursorPos(1, 1);
    t.clearDirty();
    t.eraseChars(2);
    ASSERT_TRUE(t.print('X'));

    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(!t.isDirty(Point::active(0, 1)));

    {
        EXPECT_STR("X C", t.plainString());
    }
}

TEST(terminal, Terminal__eraseChars_minimum_one) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABC");
    t.setCursorPos(1, 1);
    t.clearDirty();
    t.eraseChars(0);
    ASSERT_TRUE(t.print('X'));
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("XBC", t.plainString());
    }
}

TEST(terminal, Terminal__eraseChars_beyond_screen_edge) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "  ABC");
    t.setCursorPos(1, 4);
    t.eraseChars(10);

    {
        EXPECT_STR("  A", t.plainString());
    }
}

TEST(terminal, Terminal__eraseChars_wide_character) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.print(0x6A4B));
    PRINT_EACH(t, "BC");
    t.setCursorPos(1, 1);
    t.eraseChars(1);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("X BC", t.plainString());
    }
}

TEST(terminal, Terminal__eraseChars_resets_pending_wrap) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);
    t.eraseChars(1);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("ABCDX", t.plainString());
    }
}

TEST(terminal, Terminal__eraseChars_resets_wrap) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE123");
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 0)).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(row->wrap());
    }

    t.setCursorPos(1, 1);
    t.eraseChars(1);

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 0)).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(!row->wrap());
    }

    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("XBCDE\n123", t.plainString());
    }
}

TEST(terminal, Terminal__eraseChars_preserves_background_sgr) {
    TERM(t, 10, 10);

    PRINT_EACH(t, "ABC");
    t.setCursorPos(1, 1);
    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 0xFF, 0, 0)));
    t.eraseChars(2);

    {
        EXPECT_STR("  C", t.plainString());
        {
            const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 0)).value;
            ASSERT_TRUE(list_cell.cell->content_tag() == Cell::ContentTag::bg_color_rgb);
            {
                const Cell::RGB _rgb = list_cell.cell->contentColorRgb();
                ASSERT_TRUE(_rgb.r == 0xFF && _rgb.g == 0 && _rgb.b == 0);
            }
        }
        {
            const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(1, 0)).value;
            ASSERT_TRUE(list_cell.cell->content_tag() == Cell::ContentTag::bg_color_rgb);
            {
                const Cell::RGB _rgb = list_cell.cell->contentColorRgb();
                ASSERT_TRUE(_rgb.r == 0xFF && _rgb.g == 0 && _rgb.b == 0);
            }
        }
    }
}

TEST(terminal, Terminal__eraseChars_handles_refcounted_styles) {
    TERM(t, 10, 10);

    NOERR(t.setAttribute(attr(A::bold)));
    ASSERT_TRUE(t.print('A'));
    ASSERT_TRUE(t.print('B'));
    NOERR(t.setAttribute(attr(A::unset)));
    ASSERT_TRUE(t.print('C'));

    /* verify we have styles in our style map */
    Page *page = t.screens.active->cursor.page_pin->node->page();
    ASSERT_TRUE(1 == page->styles.count());

    t.setCursorPos(1, 1);
    t.eraseChars(2);

    /* verify we have no styles in our style map */
    ASSERT_TRUE(0 == page->styles.count());
}

TEST(terminal, Terminal__eraseChars_protected_attributes_respected_with_iso) {
    TERM(t, 5, 5);

    t.setProtectedMode(terminal::ansi::ProtectedMode::iso);
    PRINT_EACH(t, "ABC");
    t.setCursorPos(1, 1);
    t.eraseChars(2);

    {
        EXPECT_STR("ABC", t.plainString());
    }
}

TEST(terminal, Terminal__eraseChars_protected_attributes_ignored_with_dec_most_recent) {
    TERM(t, 5, 5);

    t.setProtectedMode(terminal::ansi::ProtectedMode::iso);
    PRINT_EACH(t, "ABC");
    t.setProtectedMode(terminal::ansi::ProtectedMode::dec);
    t.setProtectedMode(terminal::ansi::ProtectedMode::off);
    t.setCursorPos(1, 1);
    t.eraseChars(2);

    {
        EXPECT_STR("  C", t.plainString());
    }
}

TEST(terminal, Terminal__eraseChars_protected_attributes_ignored_with_dec_set) {
    TERM(t, 5, 5);

    t.setProtectedMode(terminal::ansi::ProtectedMode::dec);
    PRINT_EACH(t, "ABC");
    t.setCursorPos(1, 1);
    t.eraseChars(2);

    {
        EXPECT_STR("  C", t.plainString());
    }
}

TEST(terminal, Terminal__eraseChars_wide_char_boundary_conditions) {
    TERM(t, 8, 1);

    ASSERT_TRUE(t.printString("\xF0\x9F\x98\x80" "a\xF0\x9F\x98\x80" "b\xF0\x9F\x98\x80"));
    {
        EXPECT_STR("\xF0\x9F\x98\x80" "a\xF0\x9F\x98\x80" "b\xF0\x9F\x98\x80", t.plainString());
    }

    t.setCursorPos(1, 2);
    t.eraseChars(3);
    t.screens.active->cursor.page_pin->node->page()->assertIntegrity();

    {
        EXPECT_STR("     b\xF0\x9F\x98\x80", t.plainString());
    }
}

TEST(terminal, Terminal__eraseChars_wide_char_splits_proper_cell_boundaries) {
    TERM(t, 30, 1);

    /* This is a test for a bug: https://github.com/ghostty-org/ghostty/issues/2817
     * To explain the setup:
     * (1) We need our wide characters starting on an even (1-based) column.
     * (2) We need our cursor to be in the middle somewhere.
     * (3) We need our count to be less than our cursor X and on a split cell.
     * The bug was that we split the wrong cell boundaries. */

    ASSERT_TRUE(t.printString("x\xE9\xA3\x9F\xE3\x81\xB9\xE3\x81\xA6\xE4\xB8\x8B\xE3\x81\x95\xE3\x81\x84"));
    {
        EXPECT_STR("x\xE9\xA3\x9F\xE3\x81\xB9\xE3\x81\xA6\xE4\xB8\x8B\xE3\x81\x95\xE3\x81\x84", t.plainString());
    }

    /* At: て */
    t.setCursorPos(1, 6);
    /* Delete: て下 */
    t.eraseChars(4);
    t.screens.active->cursor.page_pin->node->page()->assertIntegrity();

    {
        EXPECT_STR("x\xE9\xA3\x9F\xE3\x81\xB9    \xE3\x81\x95\xE3\x81\x84", t.plainString());
    }
}

TEST(terminal, Terminal__eraseChars_wide_char_wrap_boundary_conditions) {
    TERM(t, 8, 3);

    ASSERT_TRUE(t.printString(".......\xF0\x9F\x98\x80" "abcde\xF0\x9F\x98\x80......"));
    {
        EXPECT_STR(".......\n\xF0\x9F\x98\x80" "abcde\n\xF0\x9F\x98\x80......", t.plainString());

        EXPECT_STR(".......\xF0\x9F\x98\x80" "abcde\xF0\x9F\x98\x80......", t.plainStringUnwrapped());
    }

    t.setCursorPos(2, 2);
    t.eraseChars(3);
    t.screens.active->cursor.page_pin->node->page()->assertIntegrity();

    {
        EXPECT_STR(".......\n    cde\n\xF0\x9F\x98\x80......", t.plainString());

        EXPECT_STR(".......     cde\n\xF0\x9F\x98\x80......", t.plainStringUnwrapped());
    }
}

TEST(terminal, Terminal__eraseChars_clearing_wrapped_wide_char_marks_spacer_head_row_dirty) {
    TERM(t, 5, 3);

    /* The wide char doesn't fit so it wraps, leaving a spacer head at
     * the end of the first row. */
    ASSERT_TRUE(t.printString("ABCD\xE5\xAD\x97"));
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(4, 0)).value;
        ASSERT_TRUE(Cell::Wide::spacer_head == list_cell.cell->wide());
        ASSERT_TRUE(list_cell.row->wrap());
    }

    t.setCursorPos(2, 1);
    t.clearDirty();
    t.eraseChars(1);
    t.screens.active->cursor.page_pin->node->page()->assertIntegrity();

    /* Erasing the wide char also clears the spacer head on the previous
     * row, so that row must be dirty too. */
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 1)));
    ASSERT_TRUE(!t.isDirty(Point::screen(0, 2)));

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(4, 0)).value;
        ASSERT_TRUE(Cell::Wide::narrow == list_cell.cell->wide());
    }
}

TEST(terminal, Terminal__reverseIndex) {
    TERM(t, 2, 5);

    /* Initial value */
    ASSERT_TRUE(t.print('A'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('B'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('C'));
    t.reverseIndex();
    ASSERT_TRUE(t.print('D'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());

    {
        EXPECT_STR("A\nBD\nC", t.plainString());
    }
}

TEST(terminal, Terminal__reverseIndex_from_the_top) {
    TERM(t, 2, 5);

    ASSERT_TRUE(t.print('A'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('B'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());

    t.setCursorPos(1, 1);
    t.reverseIndex();
    ASSERT_TRUE(t.print('D'));

    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    t.setCursorPos(1, 1);
    t.reverseIndex();
    ASSERT_TRUE(t.print('E'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());

    {
        EXPECT_STR("E\nD\nA\nB", t.plainString());
    }
}

TEST(terminal, Terminal__reverseIndex_top_of_scrolling_region) {
    TERM(t, 2, 10);

    /* Initial value */
    t.setCursorPos(2, 1);
    ASSERT_TRUE(t.print('A'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('B'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('C'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('D'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());

    /* Set our scroll region */
    t.setTopAndBottomMargin(2, 5);
    t.setCursorPos(2, 1);
    t.reverseIndex();
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("\nX\nA\nB\nC", t.plainString());
    }
}

TEST(terminal, Terminal__reverseIndex_top_of_screen) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.print('A'));
    t.setCursorPos(2, 1);
    ASSERT_TRUE(t.print('B'));
    t.setCursorPos(3, 1);
    ASSERT_TRUE(t.print('C'));
    t.setCursorPos(1, 1);
    t.reverseIndex();
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("X\nA\nB\nC", t.plainString());
    }
}

TEST(terminal, Terminal__reverseIndex_not_top_of_screen) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.print('A'));
    t.setCursorPos(2, 1);
    ASSERT_TRUE(t.print('B'));
    t.setCursorPos(3, 1);
    ASSERT_TRUE(t.print('C'));
    t.setCursorPos(2, 1);
    t.reverseIndex();
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("X\nB\nC", t.plainString());
    }
}

TEST(terminal, Terminal__reverseIndex_top_bottom_margins) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.print('A'));
    t.setCursorPos(2, 1);
    ASSERT_TRUE(t.print('B'));
    t.setCursorPos(3, 1);
    ASSERT_TRUE(t.print('C'));
    t.setTopAndBottomMargin(2, 3);
    t.setCursorPos(2, 1);
    t.reverseIndex();

    {
        EXPECT_STR("A\n\nB", t.plainString());
    }
}

TEST(terminal, Terminal__reverseIndex_outside_top_bottom_margins) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.print('A'));
    t.setCursorPos(2, 1);
    ASSERT_TRUE(t.print('B'));
    t.setCursorPos(3, 1);
    ASSERT_TRUE(t.print('C'));
    t.setTopAndBottomMargin(2, 3);
    t.setCursorPos(1, 1);
    t.reverseIndex();

    {
        EXPECT_STR("A\nB\nC", t.plainString());
    }
}

TEST(terminal, Terminal__reverseIndex_left_right_margins) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.setCursorPos(2, 1);
    ASSERT_TRUE(t.printString("DEF"));
    t.setCursorPos(3, 1);
    ASSERT_TRUE(t.printString("GHI"));
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setLeftAndRightMargin(2, 3);
    t.setCursorPos(1, 2);
    t.reverseIndex();

    {
        EXPECT_STR("A\nDBC\nGEF\n HI", t.plainString());
    }
}

TEST(terminal, Terminal__reverseIndex_outside_left_right_margins) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.setCursorPos(2, 1);
    ASSERT_TRUE(t.printString("DEF"));
    t.setCursorPos(3, 1);
    ASSERT_TRUE(t.printString("GHI"));
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setLeftAndRightMargin(2, 3);
    t.setCursorPos(1, 1);
    t.reverseIndex();

    {
        EXPECT_STR("ABC\nDEF\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__index) {
    TERM(t, 2, 5);

    ASSERT_TRUE(t.index());
    ASSERT_TRUE(t.print('A'));

    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));

    {
        EXPECT_STR("\nA", t.plainString());
    }
}

TEST(terminal, Terminal__index_from_the_bottom) {
    TERM(t, 2, 5);

    t.setCursorPos(5, 1);
    ASSERT_TRUE(t.print('A'));
    /* undo moving right from 'A' */
    t.cursorLeft(1);

    t.clearDirty();
    ASSERT_TRUE(t.index());
    ASSERT_TRUE(t.print('B'));

    ASSERT_TRUE(t.isDirty(Point::active(0, 3)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 4)));

    {
        EXPECT_STR("\n\n\nA\nB", t.plainString());
    }
}

TEST(terminal, Terminal__index_scrolling_with_hyperlink) {
    TERM(t, 2, 5);

    t.setCursorPos(5, 1);
    NOERR(startLink(*t.screens.active, "http://example.com"));
    ASSERT_TRUE(t.print('A'));
    t.screens.active->endHyperlink();
    /* undo moving right from 'A' */
    t.cursorLeft(1);
    ASSERT_TRUE(t.index());
    ASSERT_TRUE(t.print('B'));

    {
        EXPECT_STR("\n\n\nA\nB", t.plainString());
    }

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport(0, 3)).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(row->hyperlink());
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(cell->hyperlink());
        hyperlink::Id id = 0;
        const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
        ASSERT_TRUE(id_found);
        ASSERT_TRUE(1 == id);
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport(0, 4)).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(!row->hyperlink());
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(!cell->hyperlink());
        hyperlink::Id id = 0;
        const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
        (void)id_found;
        ASSERT_TRUE(!id_found);
    }
}

TEST(terminal, Terminal__index_outside_of_scrolling_region) {
    TERM(t, 2, 5);

    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    t.setTopAndBottomMargin(2, 5);
    ASSERT_TRUE(t.index());
    ASSERT_TRUE(1 == t.screens.active->cursor.y);
}

TEST(terminal, Terminal__index_from_the_bottom_outside_of_scroll_region) {
    TERM(t, 2, 5);

    t.setTopAndBottomMargin(1, 2);
    t.setCursorPos(5, 1);
    ASSERT_TRUE(t.print('A'));
    t.clearDirty();
    ASSERT_TRUE(t.index());
    ASSERT_TRUE(t.print('B'));
    ASSERT_TRUE(t.isDirty(Point::active(0, 4)));

    {
        EXPECT_STR("\n\n\n\nAB", t.plainString());
    }
}

TEST(terminal, Terminal__index_no_scroll_region__top_of_screen) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.print('A'));
    t.clearDirty();
    ASSERT_TRUE(t.index());
    ASSERT_TRUE(t.print('X'));

    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));

    {
        EXPECT_STR("A\n X", t.plainString());
    }
}

TEST(terminal, Terminal__index_bottom_of_primary_screen) {
    TERM(t, 5, 5);

    t.setCursorPos(5, 1);
    ASSERT_TRUE(t.print('A'));
    t.clearDirty();
    ASSERT_TRUE(t.index());
    ASSERT_TRUE(t.print('X'));

    ASSERT_TRUE(t.isDirty(Point::active(0, 3)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 4)));

    {
        EXPECT_STR("\n\n\nA\n X", t.plainString());
    }
}

TEST(terminal, Terminal__index_bottom_of_primary_screen_background_sgr) {
    TERM(t, 5, 5);

    t.setCursorPos(5, 1);
    ASSERT_TRUE(t.print('A'));
    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 0xFF, 0, 0)));
    ASSERT_TRUE(t.index());

    {
        EXPECT_STR("\n\n\nA", t.plainString());
        for (size_t x = 0; x < (size_t)(5); x++) {
            const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active((uint32_t)(x), 4)).value;
            ASSERT_TRUE(list_cell.cell->content_tag() == Cell::ContentTag::bg_color_rgb);
            {
                const Cell::RGB _rgb = list_cell.cell->contentColorRgb();
                ASSERT_TRUE(_rgb.r == 0xFF && _rgb.g == 0 && _rgb.b == 0);
            }
        }
    }
}

TEST(terminal, Terminal__index_inside_scroll_region) {
    TERM(t, 5, 5);

    t.setTopAndBottomMargin(1, 3);
    ASSERT_TRUE(t.print('A'));
    t.clearDirty();
    ASSERT_TRUE(t.index());
    ASSERT_TRUE(t.print('X'));

    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));

    {
        EXPECT_STR("A\n X", t.plainString());
    }
}

TEST(terminal, Terminal__index_bottom_of_scroll_region_with_hyperlinks) {
    TERM(t, 5, 5);

    t.setTopAndBottomMargin(1, 2);
    ASSERT_TRUE(t.print('A'));
    ASSERT_TRUE(t.index());
    t.carriageReturn();
    NOERR(startLink(*t.screens.active, "http://example.com"));
    ASSERT_TRUE(t.print('B'));
    t.screens.active->endHyperlink();
    ASSERT_TRUE(t.index());
    t.carriageReturn();
    ASSERT_TRUE(t.print('C'));

    {
        EXPECT_STR("B\nC", t.plainString());
    }

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport(0, 0)).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(row->hyperlink());
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(cell->hyperlink());
        hyperlink::Id id = 0;
        const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
        ASSERT_TRUE(id_found);
        ASSERT_TRUE(1 == id);
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport(0, 1)).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(!row->hyperlink());
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(!cell->hyperlink());
        hyperlink::Id id = 0;
        const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
        (void)id_found;
        ASSERT_TRUE(!id_found);
    }
}

TEST(terminal, Terminal__index_bottom_of_scroll_region_clear_hyperlinks) {
    TERM(t, 5, 5);
    t_opts.max_scrollback_bytes = Maybe<size_t>((size_t)0);
    ASSERT_TRUE(t_holder.reinit(t_opts));

    t.setTopAndBottomMargin(2, 3);
    t.setCursorPos(2, 1);
    NOERR(startLink(*t.screens.active, "http://example.com"));
    ASSERT_TRUE(t.print('A'));
    t.screens.active->endHyperlink();
    ASSERT_TRUE(t.index());
    t.carriageReturn();
    ASSERT_TRUE(t.print('B'));
    ASSERT_TRUE(t.index());
    t.carriageReturn();
    ASSERT_TRUE(t.print('C'));

    {
        EXPECT_STR("\nB\nC", t.plainString());
    }

    for (size_t y = (size_t)(1), n_y = (size_t)(3); y < n_y; y++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::viewport(0, (uint32_t)(y))).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(!row->hyperlink());
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(!cell->hyperlink());
        hyperlink::Id id = 0;
        const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
        (void)id_found;
        ASSERT_TRUE(!id_found);
        Page *page = list_cell.node->page();
        ASSERT_TRUE(0 == page->hyperlink_set.count());
    }
}

TEST(terminal, Terminal__index_bottom_of_scroll_region_with_background_SGR) {
    TERM(t, 5, 5);

    t.setTopAndBottomMargin(1, 3);
    t.setCursorPos(4, 1);
    ASSERT_TRUE(t.print('B'));
    t.setCursorPos(3, 1);
    ASSERT_TRUE(t.print('A'));
    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 0xFF, 0, 0)));
    ASSERT_TRUE(t.index());

    {
        EXPECT_STR("\nA\n\nB", t.plainString());
    }

    for (size_t x = 0; x < (size_t)(t.cols); x++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active((uint32_t)(x), 2)).value;
        ASSERT_TRUE(list_cell.cell->content_tag() == Cell::ContentTag::bg_color_rgb);
        {
            const Cell::RGB _rgb = list_cell.cell->contentColorRgb();
            ASSERT_TRUE(_rgb.r == 0xFF && _rgb.g == 0 && _rgb.b == 0);
        }
    }
}

TEST(terminal, Terminal__index_bottom_of_primary_screen_with_scroll_region) {
    TERM(t, 5, 5);

    t.setTopAndBottomMargin(1, 3);
    t.setCursorPos(3, 1);
    ASSERT_TRUE(t.print('A'));
    t.setCursorPos(5, 1);
    t.clearDirty();
    ASSERT_TRUE(t.index());
    ASSERT_TRUE(t.index());
    ASSERT_TRUE(t.index());
    ASSERT_TRUE(t.print('X'));

    for (size_t y = 0; y < 4; y++) ASSERT_TRUE(!t.isDirty(Point::active(0, (uint32_t)y)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 4)));

    {
        EXPECT_STR("\n\nA\n\nX", t.plainString());
    }
}

TEST(terminal, Terminal__index_outside_left_right_margin) {
    TERM(t, 10, 5);

    t.setTopAndBottomMargin(1, 3);
    t.scrolling_region.left = 3;
    t.scrolling_region.right = 5;
    t.setCursorPos(3, 3);
    ASSERT_TRUE(t.print('A'));
    t.setCursorPos(3, 1);
    t.clearDirty();
    ASSERT_TRUE(t.index());
    ASSERT_TRUE(t.print('X'));

    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));

    {
        EXPECT_STR("\n\nX A", t.plainString());
    }
}

TEST(terminal, Terminal__index_inside_left_right_margin) {
    TERM(t, 10, 5);

    ASSERT_TRUE(t.printString("AAAAAA"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("AAAAAA"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("AAAAAA"));
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setTopAndBottomMargin(1, 3);
    t.setLeftAndRightMargin(1, 3);
    t.setCursorPos(3, 1);

    t.clearDirty();
    ASSERT_TRUE(t.index());

    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));

    ASSERT_TRUE(2 == t.screens.active->cursor.y);
    ASSERT_TRUE(0 == t.screens.active->cursor.x);

    {
        EXPECT_STR("AAAAAA\nAAAAAA\n   AAA", t.plainString());
    }
}

TEST(terminal, Terminal__index_bottom_of_scroll_region_creates_scrollback) {
    TERM(t, 5, 5);

    t.setTopAndBottomMargin(1, 3);
    ASSERT_TRUE(t.printString("1\n2\n3"));
    t.setCursorPos(4, 1);
    ASSERT_TRUE(t.print('X'));
    t.setCursorPos(3, 1);
    ASSERT_TRUE(t.index());
    ASSERT_TRUE(t.print('Y'));

    {
        EXPECT_STR("2\n3\nY\nX", t.screens.active->dumpStringAlloc(Point::viewport()));
    }
    {
        EXPECT_STR("1\n2\n3\nY\nX", t.screens.active->dumpStringAlloc(Point::screen()));
    }
}

TEST(terminal, Terminal__index_bottom_of_scroll_region_no_scrollback) {
    TERM(t, 5, 5);
    t_opts.max_scrollback_bytes = Maybe<size_t>((size_t)0);
    ASSERT_TRUE(t_holder.reinit(t_opts));

    t.setTopAndBottomMargin(1, 3);
    t.setCursorPos(4, 1);
    ASSERT_TRUE(t.print('B'));
    t.setCursorPos(3, 1);
    ASSERT_TRUE(t.print('A'));
    t.clearDirty();
    ASSERT_TRUE(t.index());
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("\nA\n X\nB", t.plainString());
    }
}

TEST(terminal, Terminal__index_bottom_of_scroll_region_blank_line_preserves_SGR) {
    TERM(t, 5, 5);

    t.setTopAndBottomMargin(1, 3);
    ASSERT_TRUE(t.printString("1\n2\n3"));
    t.setCursorPos(4, 1);
    ASSERT_TRUE(t.print('X'));
    t.setCursorPos(3, 1);
    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 0xFF, 0, 0)));
    ASSERT_TRUE(t.index());

    {
        EXPECT_STR("2\n3\n\nX", t.screens.active->dumpStringAlloc(Point::viewport()));
    }
    {
        EXPECT_STR("1\n2\n3\n\nX", t.screens.active->dumpStringAlloc(Point::screen()));
    }
    for (size_t x = 0; x < (size_t)(t.cols); x++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active((uint32_t)(x), 2)).value;
        ASSERT_TRUE(list_cell.cell->content_tag() == Cell::ContentTag::bg_color_rgb);
        {
            const Cell::RGB _rgb = list_cell.cell->contentColorRgb();
            ASSERT_TRUE(_rgb.r == 0xFF && _rgb.g == 0 && _rgb.b == 0);
        }
    }
}

TEST(terminal, Terminal__index_bottom_of_scroll_region_with_top_margin_and_background_SGR) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("1\n2\n3\n4\n5"));
    t.setTopAndBottomMargin(2, 4);
    t.setCursorPos(4, 1);
    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 0xFF, 0, 0)));
    ASSERT_TRUE(t.index());

    /* The region (rows 2-4) scrolled up, rows outside are unchanged. */
    {
        EXPECT_STR("1\n3\n4\n\n5", t.plainString());
    }

    /* The cursor is on the new blank row. */
    ASSERT_TRUE(3 == t.screens.active->cursor.y);

    /* The new blank row must be filled with our background color. */
    for (size_t x = 0; x < (size_t)(t.cols); x++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active((uint32_t)(x), 3)).value;
        ASSERT_TRUE(list_cell.cell->content_tag() == Cell::ContentTag::bg_color_rgb);
        {
            const Cell::RGB _rgb = list_cell.cell->contentColorRgb();
            ASSERT_TRUE(_rgb.r == 0xFF && _rgb.g == 0 && _rgb.b == 0);
        }
    }
}

TEST(terminal, Terminal__index_bottom_of_alt_screen_full_region) {
    TERM(t, 5, 3);

    ASSERT_TRUE(t.switchScreenMode(Terminal::SwitchScreenMode::mode_1049, true));
    ASSERT_TRUE(t.printString("A\nB\nC"));
    ASSERT_TRUE(t.index());
    t.carriageReturn();
    ASSERT_TRUE(t.print('D'));

    /* Content scrolled up and the scrolled-out row is discarded, NOT
     * moved into scrollback (the alt screen has none). */
    {
        EXPECT_STR("B\nC\nD", t.plainString());
    }
    {
        EXPECT_STR("B\nC\nD", t.screens.active->dumpStringAlloc(Point::screen()));
    }

    /* Primary screen is untouched. */
    ASSERT_TRUE(t.switchScreenMode(Terminal::SwitchScreenMode::mode_1049, false));
    {
        EXPECT_STR("", t.plainString());
    }
}

TEST(terminal, Terminal__index_bottom_of_alt_screen_top_region) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.switchScreenMode(Terminal::SwitchScreenMode::mode_1049, true));
    ASSERT_TRUE(t.printString("1\n2\n3\n4\n5"));

    /* Region at the top of the screen, excluding the last row. On the
     * alt screen this must NOT create scrollback. */
    t.setTopAndBottomMargin(1, 4);
    t.setCursorPos(4, 1);
    ASSERT_TRUE(t.index());
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("2\n3\n4\nX\n5", t.plainString());
    }
    {
        EXPECT_STR("2\n3\n4\nX\n5", t.screens.active->dumpStringAlloc(Point::screen()));
    }
}

TEST(terminal, Terminal__scrollUp_top_region_no_scrollback) {
    TERM(t, 5, 5);
    t_opts.max_scrollback_bytes = Maybe<size_t>((size_t)0);
    ASSERT_TRUE(t_holder.reinit(t_opts));

    ASSERT_TRUE(t.printString("A\nB\nC\nD\nE"));
    t.setTopAndBottomMargin(1, 3);
    ASSERT_TRUE(t.scrollUp(1));

    /* The region scrolled and the scrolled-out row is discarded. */
    {
        EXPECT_STR("B\nC\n\nD\nE", t.plainString());
    }
    {
        EXPECT_STR("B\nC\n\nD\nE", t.screens.active->dumpStringAlloc(Point::screen()));
    }
}

TEST(terminal, Terminal__cursorUp_basic) {
    TERM(t, 5, 5);

    t.setCursorPos(3, 1);
    ASSERT_TRUE(t.print('A'));
    t.cursorUp(10);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR(" X\n\nA", t.plainString());
    }
}

TEST(terminal, Terminal__cursorUp_below_top_scroll_margin) {
    TERM(t, 5, 5);

    t.setTopAndBottomMargin(2, 4);
    t.setCursorPos(3, 1);
    ASSERT_TRUE(t.print('A'));
    t.cursorUp(5);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("\n X\nA", t.plainString());
    }
}

TEST(terminal, Terminal__cursorUp_above_top_scroll_margin) {
    TERM(t, 5, 5);

    t.setTopAndBottomMargin(3, 5);
    t.setCursorPos(3, 1);
    ASSERT_TRUE(t.print('A'));
    t.setCursorPos(2, 1);
    t.cursorUp(10);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("X\n\nA", t.plainString());
    }
}

TEST(terminal, Terminal__cursorUp_resets_wrap) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);
    t.cursorUp(1);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("ABCDX", t.plainString());
    }
}

TEST(terminal, Terminal__cursorLeft_no_wrap) {
    TERM(t, 10, 5);

    ASSERT_TRUE(t.print('A'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('B'));
    t.cursorLeft(10);

    {
        EXPECT_STR("A\nB", t.plainString());
    }
}

TEST(terminal, Terminal__cursorLeft_unsets_pending_wrap_state) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);
    t.cursorLeft(1);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("ABCXE", t.plainString());
    }
}

TEST(terminal, Terminal__cursorLeft_unsets_pending_wrap_state_with_longer_jump) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);
    t.cursorLeft(3);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("AXCDE", t.plainString());
    }
}

TEST(terminal, Terminal__cursorLeft_reverse_wrap_with_pending_wrap_state) {
    TERM(t, 5, 5);

    t.modes.set(terminal::modes::Mode::wraparound, true);
    t.modes.set(terminal::modes::Mode::reverse_wrap, true);

    PRINT_EACH(t, "ABCDE");
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);
    t.cursorLeft(1);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("ABCDX", t.plainString());
    }
}

TEST(terminal, Terminal__cursorLeft_reverse_wrap_extended_with_pending_wrap_state) {
    TERM(t, 5, 5);

    t.modes.set(terminal::modes::Mode::wraparound, true);
    t.modes.set(terminal::modes::Mode::reverse_wrap_extended, true);

    PRINT_EACH(t, "ABCDE");
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);
    t.cursorLeft(1);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("ABCDX", t.plainString());
    }
}

TEST(terminal, Terminal__cursorLeft_reverse_wrap) {
    TERM(t, 5, 5);

    t.modes.set(terminal::modes::Mode::wraparound, true);
    t.modes.set(terminal::modes::Mode::reverse_wrap, true);

    PRINT_EACH(t, "ABCDE1");
    t.cursorLeft(2);
    ASSERT_TRUE(t.print('X'));
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);

    {
        EXPECT_STR("ABCDX\n1", t.plainString());
    }
}

TEST(terminal, Terminal__cursorLeft_reverse_wrap_with_no_soft_wrap) {
    TERM(t, 5, 5);

    t.modes.set(terminal::modes::Mode::wraparound, true);
    t.modes.set(terminal::modes::Mode::reverse_wrap, true);

    PRINT_EACH(t, "ABCDE");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('1'));
    t.cursorLeft(2);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("ABCDE\nX", t.plainString());
    }
}

TEST(terminal, Terminal__cursorLeft_reverse_wrap_before_left_margin) {
    TERM(t, 5, 5);

    t.modes.set(terminal::modes::Mode::wraparound, true);
    t.modes.set(terminal::modes::Mode::reverse_wrap, true);
    t.setTopAndBottomMargin(3, 0);
    t.cursorLeft(1);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("\n\nX", t.plainString());
    }
}

TEST(terminal, Terminal__cursorLeft_extended_reverse_wrap) {
    TERM(t, 5, 5);

    t.modes.set(terminal::modes::Mode::wraparound, true);
    t.modes.set(terminal::modes::Mode::reverse_wrap_extended, true);

    PRINT_EACH(t, "ABCDE");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('1'));
    t.cursorLeft(2);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("ABCDX\n1", t.plainString());
    }
}

TEST(terminal, Terminal__cursorLeft_extended_reverse_wrap_bottom_wraparound) {
    TERM(t, 5, 3);

    t.modes.set(terminal::modes::Mode::wraparound, true);
    t.modes.set(terminal::modes::Mode::reverse_wrap_extended, true);

    PRINT_EACH(t, "ABCDE");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('1'));
    t.cursorLeft(1 + t.cols + 1);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("ABCDE\n1\n    X", t.plainString());
    }
}

TEST(terminal, Terminal__cursorLeft_extended_reverse_wrap_is_priority_if_both_set) {
    TERM(t, 5, 3);

    t.modes.set(terminal::modes::Mode::wraparound, true);
    t.modes.set(terminal::modes::Mode::reverse_wrap, true);
    t.modes.set(terminal::modes::Mode::reverse_wrap_extended, true);

    PRINT_EACH(t, "ABCDE");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('1'));
    t.cursorLeft(1 + t.cols + 1);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("ABCDE\n1\n    X", t.plainString());
    }
}

TEST(terminal, Terminal__cursorLeft_extended_reverse_wrap_above_top_scroll_region) {
    TERM(t, 5, 5);

    t.modes.set(terminal::modes::Mode::wraparound, true);
    t.modes.set(terminal::modes::Mode::reverse_wrap_extended, true);

    t.setTopAndBottomMargin(3, 0);
    t.setCursorPos(2, 1);
    t.cursorLeft(1000);

    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
}

TEST(terminal, Terminal__cursorLeft_reverse_wrap_on_first_row) {
    TERM(t, 5, 5);

    t.modes.set(terminal::modes::Mode::wraparound, true);
    t.modes.set(terminal::modes::Mode::reverse_wrap, true);

    t.setTopAndBottomMargin(3, 0);
    t.setCursorPos(1, 2);
    t.cursorLeft(1000);

    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
}

TEST(terminal, Terminal__cursorDown_basic) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.print('A'));
    t.cursorDown(10);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("A\n\n\n\n X", t.plainString());
    }
}

TEST(terminal, Terminal__cursorDown_above_bottom_scroll_margin) {
    TERM(t, 5, 5);

    t.setTopAndBottomMargin(1, 3);
    ASSERT_TRUE(t.print('A'));
    t.cursorDown(10);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("A\n\n X", t.plainString());
    }
}

TEST(terminal, Terminal__cursorDown_below_bottom_scroll_margin) {
    TERM(t, 5, 5);

    t.setTopAndBottomMargin(1, 3);
    ASSERT_TRUE(t.print('A'));
    t.setCursorPos(4, 1);
    t.cursorDown(10);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("A\n\n\n\nX", t.plainString());
    }
}

TEST(terminal, Terminal__cursorDown_resets_wrap) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);
    t.cursorDown(1);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("ABCDE\n    X", t.plainString());
    }
}

TEST(terminal, Terminal__cursorRight_resets_wrap) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);
    t.cursorRight(1);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("ABCDX", t.plainString());
    }
}

TEST(terminal, Terminal__cursorRight_to_the_edge_of_screen) {
    TERM(t, 5, 5);

    t.cursorRight(100);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("    X", t.plainString());
    }
}

TEST(terminal, Terminal__cursorRight_left_of_right_margin) {
    TERM(t, 5, 5);

    t.scrolling_region.right = 2;
    t.cursorRight(100);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("  X", t.plainString());
    }
}

TEST(terminal, Terminal__cursorRight_right_of_right_margin) {
    TERM(t, 5, 5);

    t.scrolling_region.right = 2;
    t.setCursorPos(1, 4);
    t.cursorRight(100);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("    X", t.plainString());
    }
}

TEST(terminal, Terminal__deleteLines_simple) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.setCursorPos(2, 2);

    PageList::Node *node = t.screens.active->cursor.page_pin->node;
    const uint64_t serial = node->serial;
    t.clearDirty();
    t.deleteLines(1);
    ASSERT_TRUE(!t.screens.active->pages.nodeIsValid(node, serial));

    ASSERT_TRUE(!t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 3)));

    {
        EXPECT_STR("ABC\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__deleteLines_colors_with_bg_color) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("ABC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI"));
    t.setCursorPos(2, 2);

    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 0xFF, 0, 0)));
    t.deleteLines(1);

    {
        EXPECT_STR("ABC\nGHI", t.plainString());
    }

    for (size_t x = 0; x < (size_t)(t.cols); x++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active((uint32_t)(x), 4)).value;
        ASSERT_TRUE(list_cell.cell->content_tag() == Cell::ContentTag::bg_color_rgb);
        {
            const Cell::RGB _rgb = list_cell.cell->contentColorRgb();
            ASSERT_TRUE(_rgb.r == 0xFF && _rgb.g == 0 && _rgb.b == 0);
        }
    }
}

TEST(terminal, Terminal__deleteLines_across_page_boundary_marks_all_shifted_rows_dirty) {
    TERM(t, 10, 5);
    t_opts.max_scrollback_bytes = Maybe<size_t>((size_t)1024);
    ASSERT_TRUE(t_holder.reinit(t_opts));

    PageList::List::Node *first_page = t.screens.active->pages.pages.first;
    const auto first_page_nrows = first_page->capacity().rows;

    /* Fill up the first page minus 3 rows */
    for (size_t i_ = (size_t)(0); i_ < (size_t)(first_page_nrows - 3); i_++) ASSERT_TRUE(t.linefeed());

    /* Add content that will cross a page boundary */
    ASSERT_TRUE(t.printString("1AAAA"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("2BBBB"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("3CCCC"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("4DDDD"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("5EEEE"));

    /* Verify we now have a second page */
    PageList::List::Node *second_page = first_page->next;
    const uint64_t first_serial = first_page->serial;
    const uint64_t second_serial = second_page->serial;

    t.setCursorPos(1, 1);
    t.clearDirty();
    t.deleteLines(1);
    ASSERT_TRUE(!t.screens.active->pages.nodeIsValid(first_page, first_serial));
    ASSERT_TRUE(!t.screens.active->pages.nodeIsValid(second_page, second_serial));

    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 3)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 4)));

    {
        EXPECT_STR("2BBBB\n3CCCC\n4DDDD\n5EEEE", t.plainString());
    }
}

TEST(terminal, Terminal__deleteLines_hyperlink_dense_row_crosses_page_boundary) {
    /* Regression test for the cross-page copy of deleteLines: when the
     * shifted row carries more unique hyperlinks than the destination
     * page's hyperlink capacity, the copy must increase the destination
     * page's capacity and retry rather than corrupting the page list.
     *
     * This is the mirror of the insertLines variant: the dense row
     * starts as the first row of the second page and is pulled up into
     * the first page, which also happens to be the cursor's page so
     * this exercises the cursor accounting of the capacity increase. */
    TERM(t, 10, 5);
    t_opts.max_scrollback_bytes = Maybe<size_t>((size_t)1024);
    ASSERT_TRUE(t_holder.reinit(t_opts));

    PageList &pages = t.screens.active->pages;

    /* Fill the first page so it is exactly full, then two more rows so
     * the second page holds the last two active rows (y=3 and y=4). */
    const auto first_page_rows = pages.pages.first->capacity().rows;
    for (size_t i_ = (size_t)(0); i_ < (size_t)(first_page_rows + 1); i_++) ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(pages.pages.first != pages.pages.last);
    ASSERT_TRUE(2 == pages.pages.last->rows());

    /* Marker rows so we can verify the shift afterwards. */
    t.setCursorPos(1, 1);
    ASSERT_TRUE(t.printString("0"));
    t.setCursorPos(2, 1);
    ASSERT_TRUE(t.printString("1"));
    t.setCursorPos(3, 1);
    ASSERT_TRUE(t.printString("2"));
    t.setCursorPos(5, 1);
    ASSERT_TRUE(t.printString("4"));

    /* Fill the first row of the second page (active y=3) with unique
     * hyperlinks: more than the first page can hold with its default
     * hyperlink capacity. Writing them grows the second page's
     * capacity as needed; the first page keeps its default capacity. */
    t.setCursorPos(4, 1);
    for (size_t i = 0; i < (size_t)(10); i++) {
        char buf[64];
        snprintf(buf, sizeof buf, "http://example.com/%zu", (size_t)i);
        const char *uri = buf;
        NOERR(startLink(*t.screens.active, uri));
        ASSERT_TRUE(t.print((uint32_t)('A' + i)));
        t.screens.active->endHyperlink();
    }
    {
        const Pin pin = pages.pin(Point::active(0, 3)).value;
        ASSERT_TRUE(pages.pages.last == pin.node);
        ASSERT_TRUE(0 == pin.y);
    }
    ASSERT_TRUE(pages.pages.first->page()->hyperlink_set.layout.cap < 10);

    /* Delete the top line: every row shifts up by one and the dense
     * row crosses the page boundary into the first page. */
    t.setCursorPos(1, 1);
    t.deleteLines(1);

    {
        EXPECT_STR("1\n2\nABCDEFGHIJ\n4", t.plainString());
    }

    /* The first page's hyperlink capacity had to grow to receive the
     * row, proving the capacity-retry path ran. */
    ASSERT_TRUE(pages.pages.first->page()->hyperlink_set.layout.cap >= 10);

    /* Every cell of the dense row must still resolve to a real
     * hyperlink entry with the correct URI. A half-applied shift
     * leaves cells whose hyperlink flag is set but that have no map
     * entry, which aborts in clearCells later. */
    for (size_t x = 0; x < (size_t)(10); x++) {
        const PageList::Cell list_cell = pages.getCell(Point::active((uint32_t)(x), 2)).value;
        ASSERT_TRUE(list_cell.cell->hyperlink());
        Page *page = list_cell.node->page();
        hyperlink::Id id = 0;
        ASSERT_TRUE(page->lookupHyperlink(list_cell.cell, &id));
        const auto link = page->hyperlink_set.get((const void *)page->memory, id);
        char buf[64];
        snprintf(buf, sizeof buf, "http://example.com/%zu", (size_t)x);
        const char *uri = buf;
        EXPECT_STR(uri, std::string((const char *)link->uri.slice((const void *)page->memory), link->uri.len));
    }

    /* All pages must pass integrity checks. */
    for (PageList::List::Node *node = pages.pages.first; node != nullptr; node = node->next)
        node->page()->assertIntegrity();
}

TEST(terminal, Terminal__deleteLines__legacy_) {
    TERM(t, 80, 80);

    /* Initial value */
    ASSERT_TRUE(t.print('A'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('B'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('C'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('D'));

    t.cursorUp(2);
    t.deleteLines(1);

    ASSERT_TRUE(t.print('E'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());

    /* We should be */
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    ASSERT_TRUE(2 == t.screens.active->cursor.y);

    {
        EXPECT_STR("A\nE\nD", t.plainString());
    }
}

TEST(terminal, Terminal__deleteLines_with_scroll_region) {
    TERM(t, 80, 80);

    /* Initial value */
    ASSERT_TRUE(t.print('A'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('B'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('C'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('D'));

    t.setTopAndBottomMargin(1, 3);
    t.setCursorPos(1, 1);

    t.clearDirty();
    t.deleteLines(1);

    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));
    ASSERT_TRUE(!t.isDirty(Point::active(0, 3)));

    ASSERT_TRUE(t.print('E'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());

    /* We should be
     * try testing.expectEqual(@as(usize, 0), t.screens.active->cursor.x);
     * try testing.expectEqual(@as(usize, 2), t.screens.active->cursor.y); */

    {
        EXPECT_STR("E\nC\n\nD", t.plainString());
    }
}

TEST(terminal, Terminal__deleteLines_with_scroll_region__large_count) {
    TERM(t, 80, 80);

    /* Initial value */
    ASSERT_TRUE(t.print('A'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('B'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('C'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('D'));

    t.setTopAndBottomMargin(1, 3);
    t.setCursorPos(1, 1);

    t.clearDirty();
    t.deleteLines(5);

    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));
    ASSERT_TRUE(!t.isDirty(Point::active(0, 3)));

    ASSERT_TRUE(t.print('E'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());

    /* We should be
     * try testing.expectEqual(@as(usize, 0), t.screens.active->cursor.x);
     * try testing.expectEqual(@as(usize, 2), t.screens.active->cursor.y); */

    {
        EXPECT_STR("E\n\n\nD", t.plainString());
    }
}

TEST(terminal, Terminal__deleteLines_with_scroll_region__cursor_outside_of_region) {
    TERM(t, 80, 80);

    /* Initial value */
    ASSERT_TRUE(t.print('A'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('B'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('C'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('D'));

    t.setTopAndBottomMargin(1, 3);
    t.setCursorPos(4, 1);

    t.clearDirty();
    t.deleteLines(1);

    for (size_t y = 0; y < 4; y++) ASSERT_TRUE(!t.isDirty(Point::active(0, (uint32_t)y)));

    {
        EXPECT_STR("A\nB\nC\nD", t.plainString());
    }
}

TEST(terminal, Terminal__deleteLines_resets_pending_wrap) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);
    t.deleteLines(1);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);
    ASSERT_TRUE(t.print('B'));

    {
        EXPECT_STR("B", t.plainString());
    }
}

TEST(terminal, Terminal__deleteLines_resets_wrap) {
    TERM(t, 3, 3);

    ASSERT_TRUE(t.print('1'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "ABCDEF");

    t.setTopAndBottomMargin(1, 2);
    t.setCursorPos(1, 1);
    t.deleteLines(1);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("XBC\n\nDEF", t.plainString());
    }

    for (size_t y = 0; y < (size_t)(t.rows); y++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, (uint32_t)(y))).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(!row->wrap());
    }
}

TEST(terminal, Terminal__deleteLines_left_right_scroll_region) {
    TERM(t, 10, 10);

    ASSERT_TRUE(t.printString("ABC123"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF456"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI789"));
    t.scrolling_region.left = 1;
    t.scrolling_region.right = 3;
    t.setCursorPos(2, 2);

    t.clearDirty();
    t.deleteLines(1);

    ASSERT_TRUE(!t.isDirty(Point::active(0, 0)));
    for (size_t y = (size_t)(1); y < (size_t)(3); y++) ASSERT_TRUE(t.isDirty(Point::active(0, (uint32_t)(y))));

    {
        EXPECT_STR("ABC123\nDHI756\nG   89", t.plainString());
    }
}

TEST(terminal, Terminal__deleteLines_left_right_scroll_region_from_top) {
    TERM(t, 10, 10);

    ASSERT_TRUE(t.printString("ABC123"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF456"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI789"));
    t.scrolling_region.left = 1;
    t.scrolling_region.right = 3;
    t.setCursorPos(1, 2);

    t.clearDirty();
    t.deleteLines(1);

    for (size_t y = (size_t)(0); y < (size_t)(3); y++) ASSERT_TRUE(t.isDirty(Point::active(0, (uint32_t)(y))));

    {
        EXPECT_STR("AEF423\nDHI756\nG   89", t.plainString());
    }
}

TEST(terminal, Terminal__deleteLines_left_right_scroll_region_high_count) {
    TERM(t, 10, 10);

    ASSERT_TRUE(t.printString("ABC123"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DEF456"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GHI789"));
    t.scrolling_region.left = 1;
    t.scrolling_region.right = 3;
    t.setCursorPos(2, 2);

    t.clearDirty();
    t.deleteLines(100);

    ASSERT_TRUE(!t.isDirty(Point::active(0, 0)));
    for (size_t y = (size_t)(1); y < (size_t)(3); y++) ASSERT_TRUE(t.isDirty(Point::active(0, (uint32_t)(y))));

    {
        EXPECT_STR("ABC123\nD   56\nG   89", t.plainString());
    }
}

TEST(terminal, Terminal__deleteLines_wide_character_spacer_head) {
    TERM(t, 5, 3);

    /* Initial value
     * +-----+
     * |AAAAA| < Wrapped
     * |BBBB*| < Wrapped     (continued)
     * |WWCCC| < Non-wrapped (continued)
     * +-----+
     * where * represents a spacer head cell
     * and WW is the wide character. */
    ASSERT_TRUE(t.printString("AAAAABBBB\xF0\x9F\x98\x80" "CCC"));

    /* Delete the top line
     * +-----+
     * |BBBB | < Non-wrapped
     * |WWCCC| < Non-wrapped
     * |     | < Non-wrapped
     * +-----+
     * This should convert the spacer head to
     * a regular empty cell, and un-set wrap. */
    t.setCursorPos(1, 1);
    t.deleteLines(1);

    {
        const std::string str = t.plainString();
        const std::string unwrapped_str = t.plainStringUnwrapped();
        EXPECT_STR("BBBB\n\xF0\x9F\x98\x80" "CCC", str);
        EXPECT_STR("BBBB\n\xF0\x9F\x98\x80" "CCC", unwrapped_str);
    }
}

TEST(terminal, Terminal__deleteLines_wide_character_spacer_head_left_scroll_margin) {
    TERM(t, 5, 3);

    /* Initial value
     * +-----+
     * |AAAAA| < Wrapped
     * |BBBB*| < Wrapped     (continued)
     * |WWCCC| < Non-wrapped (continued)
     * +-----+
     * where * represents a spacer head cell
     * and WW is the wide character. */
    ASSERT_TRUE(t.printString("AAAAABBBB\xF0\x9F\x98\x80" "CCC"));

    t.scrolling_region.left = 2;

    /* Delete the top line
     * ###  <- scrolling region
     * +-----+
     * |AABB | < Wrapped
     * |BBCCC| < Wrapped     (continued)
     * |WW   | < Non-wrapped (continued)
     * +-----+
     * This should convert the spacer head to
     * a regular empty cell, but due to the
     * left scrolling margin, wrap state should
     * remain. */
    t.setCursorPos(1, 3);
    t.deleteLines(1);

    {
        const std::string str = t.plainString();
        const std::string unwrapped_str = t.plainStringUnwrapped();
        EXPECT_STR("AABB\nBBCCC\n\xF0\x9F\x98\x80", str);
        EXPECT_STR("AABB BBCCC\xF0\x9F\x98\x80", unwrapped_str);
    }
}

TEST(terminal, Terminal__deleteLines_wide_character_spacer_head_right_scroll_margin) {
    TERM(t, 5, 3);

    /* Initial value
     * +-----+
     * |AAAAA| < Wrapped
     * |BBBB*| < Wrapped     (continued)
     * |WWCCC| < Non-wrapped (continued)
     * +-----+
     * where * represents a spacer head cell
     * and WW is the wide character. */
    ASSERT_TRUE(t.printString("AAAAABBBB\xF0\x9F\x98\x80" "CCC"));

    t.scrolling_region.right = 3;

    /* Delete the top line
     * ####   <- scrolling region
     * +-----+
     * |BBBBA| < Wrapped
     * |WWCC | < Wrapped     (continued)
     * |    C| < Non-wrapped (continued)
     * +-----+
     * This should convert the spacer head to
     * a regular empty cell, but due to the
     * right scrolling margin, wrap state should
     * remain. */
    t.setCursorPos(1, 1);
    t.deleteLines(1);

    {
        const std::string str = t.plainString();
        const std::string unwrapped_str = t.plainStringUnwrapped();
        EXPECT_STR("BBBBA\n\xF0\x9F\x98\x80" "CC\n    C", str);
        EXPECT_STR("BBBBA\xF0\x9F\x98\x80" "CC     C", unwrapped_str);
    }
}

TEST(terminal, Terminal__deleteLines_wide_character_spacer_head_left_and_right_scroll_margin) {
    TERM(t, 5, 3);

    /* Initial value
     * +-----+
     * |AAAAA| < Wrapped
     * |BBBB*| < Wrapped     (continued)
     * |WWCCC| < Non-wrapped (continued)
     * +-----+
     * where * represents a spacer head cell
     * and WW is the wide character. */
    ASSERT_TRUE(t.printString("AAAAABBBB\xF0\x9F\x98\x80" "CCC"));

    t.scrolling_region.right = 3;
    t.scrolling_region.left = 2;

    /* Delete the top line
     * ##   <- scrolling region
     * +-----+
     * |AABBA| < Wrapped
     * |BBCC*| < Wrapped     (continued)
     * |WW  C| < Non-wrapped (continued)
     * +-----+
     * Because there is both a left scrolling
     * margin > 1 and a right scrolling margin
     * the spacer head should remain, and the
     * wrap state should be untouched. */
    t.setCursorPos(1, 3);
    t.deleteLines(1);

    {
        const std::string str = t.plainString();
        const std::string unwrapped_str = t.plainStringUnwrapped();
        EXPECT_STR("AABBA\nBBCC\n\xF0\x9F\x98\x80  C", str);
        EXPECT_STR("AABBABBCC\xF0\x9F\x98\x80  C", unwrapped_str);
    }
}

TEST(terminal, Terminal__deleteLines_wide_character_spacer_head_left____2__and_right_scroll_margin) {
    TERM(t, 5, 3);

    /* Initial value
     * +-----+
     * |AAAAA| < Wrapped
     * |BBBB*| < Wrapped     (continued)
     * |WWCCC| < Non-wrapped (continued)
     * +-----+
     * where * represents a spacer head cell
     * and WW is the wide character. */
    ASSERT_TRUE(t.printString("AAAAABBBB\xF0\x9F\x98\x80" "CCC"));

    t.scrolling_region.right = 3;
    t.scrolling_region.left = 1;

    /* Delete the top line
     * ###   <- scrolling region
     * +-----+
     * |ABBBA| < Wrapped
     * |B CC | < Wrapped     (continued)
     * |    C| < Non-wrapped (continued)
     * +-----+
     * Because the left margin is 1, the wide
     * char is split, and therefore removed,
     * along with the spacer head - however,
     * wrap state should be untouched. */
    t.setCursorPos(1, 2);
    t.deleteLines(1);

    {
        const std::string str = t.plainString();
        const std::string unwrapped_str = t.plainStringUnwrapped();
        EXPECT_STR("ABBBA\nB CC\n    C", str);
        EXPECT_STR("ABBBAB CC     C", unwrapped_str);
    }
}

TEST(terminal, Terminal__deleteLines_wide_characters_split_by_left_right_scroll_region_boundaries) {
    TERM(t, 5, 2);

    /* Initial value
     * +-----+
     * |AAAAA|
     * |WWBWW|
     * +-----+
     * where WW represents a wide character */
    ASSERT_TRUE(t.printString("AAAAA\n\xF0\x9F\x98\x80" "B\xF0\x9F\x98\x80"));

    t.scrolling_region.right = 3;
    t.scrolling_region.left = 1;

    /* Delete the top line
     * ###   <- scrolling region
     * +-----+
     * |A B A|
     * |     |
     * +-----+
     * The two wide chars, because they're
     * split by the edge of the scrolling
     * region, get removed. */
    t.setCursorPos(1, 2);
    t.deleteLines(1);

    {
        EXPECT_STR("A B A", t.plainString());
    }
}

TEST(terminal, Terminal__deleteLines_zero) {
    TERM(t, 2, 5);

    /* This should do nothing */
    t.setCursorPos(1, 1);
    t.deleteLines(0);
}

TEST(terminal, Terminal__default_style_is_empty) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.print('A'));

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('A' == cell->contentCodepoint());
        ASSERT_TRUE(0 == cell->style_id());
    }
}

TEST(terminal, Terminal__bold_style) {
    TERM(t, 5, 5);

    NOERR(t.setAttribute(attr(A::bold)));
    ASSERT_TRUE(t.print('A'));

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('A' == cell->contentCodepoint());
        ASSERT_TRUE(cell->style_id() != 0);
        Page *page = t.screens.active->cursor.page_pin->node->page();
        ASSERT_TRUE(page->styles.refCount((const void *)page->memory, t.screens.active->cursor.style_id) > 1);
    }
}

TEST(terminal, Terminal__garbage_collect_overwritten) {
    TERM(t, 5, 5);

    NOERR(t.setAttribute(attr(A::bold)));
    ASSERT_TRUE(t.print('A'));
    t.setCursorPos(1, 1);
    NOERR(t.setAttribute(attr(A::unset)));
    ASSERT_TRUE(t.print('B'));

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('B' == cell->contentCodepoint());
        ASSERT_TRUE(cell->style_id() == 0);
    }

    /* verify we have no styles in our style map */
    Page *page = t.screens.active->cursor.page_pin->node->page();
    ASSERT_TRUE(0 == page->styles.count());
}

TEST(terminal, Terminal__do_not_garbage_collect_old_styles_in_use) {
    TERM(t, 5, 5);

    NOERR(t.setAttribute(attr(A::bold)));
    ASSERT_TRUE(t.print('A'));
    NOERR(t.setAttribute(attr(A::unset)));
    ASSERT_TRUE(t.print('B'));

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('B' == cell->contentCodepoint());
        ASSERT_TRUE(cell->style_id() == 0);
    }

    /* verify we have no styles in our style map */
    Page *page = t.screens.active->cursor.page_pin->node->page();
    ASSERT_TRUE(1 == page->styles.count());
}

TEST(terminal, Terminal__print_with_style_marks_the_row_as_styled) {
    TERM(t, 5, 5);

    NOERR(t.setAttribute(attr(A::bold)));
    ASSERT_TRUE(t.print('A'));
    NOERR(t.setAttribute(attr(A::unset)));
    ASSERT_TRUE(t.print('B'));

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        ASSERT_TRUE(list_cell.row->styled());
    }
}

TEST(terminal, Terminal__DECALN) {
    TERM(t, 2, 2);

    /* Initial value */
    ASSERT_TRUE(t.print('A'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.print('B'));
    ASSERT_TRUE(t.decaln());

    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(0 == t.screens.active->cursor.x);

    for (size_t y = (size_t)(0); y < (size_t)(t.rows); y++) ASSERT_TRUE(t.isDirty(Point::active(0, (uint32_t)(y))));

    {
        EXPECT_STR("EE\nEE", t.plainString());
    }
}

TEST(terminal, Terminal__decaln_reset_margins) {
    TERM(t, 3, 3);

    /* Initial value */
    t.modes.set(terminal::modes::Mode::origin, true);
    t.setTopAndBottomMargin(2, 3);
    ASSERT_TRUE(t.decaln());
    t.scrollDown(1);

    {
        EXPECT_STR("\nEEE\nEEE", t.plainString());
    }
}

TEST(terminal, Terminal__decaln_preserves_color) {
    TERM(t, 3, 3);

    /* Initial value */
    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 0xFF, 0, 0)));
    t.modes.set(terminal::modes::Mode::origin, true);
    t.setTopAndBottomMargin(2, 3);
    ASSERT_TRUE(t.decaln());
    t.scrollDown(1);

    {
        EXPECT_STR("\nEEE\nEEE", t.plainString());
    }

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 0)).value;
        ASSERT_TRUE(list_cell.cell->content_tag() == Cell::ContentTag::bg_color_rgb);
        {
            const Cell::RGB _rgb = list_cell.cell->contentColorRgb();
            ASSERT_TRUE(_rgb.r == 0xFF && _rgb.g == 0 && _rgb.b == 0);
        }
    }
}

TEST(terminal, Terminal__DECALN_resets_graphemes_with_protected_mode) {
    TERM(t, 3, 3);

    /* Add protected mode. A previous version of DECALN accidentally preserved
     * protected mode which left dangling managed memory. */
    t.setProtectedMode(terminal::ansi::ProtectedMode::iso);

    /* This is: 👨‍👩‍👧 (which may or may not render correctly) */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);
    ASSERT_TRUE(t.print(0x1F468));
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(t.print(0x1F469));
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(t.print(0x1F467));

    ASSERT_TRUE(t.decaln());

    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    ASSERT_TRUE(t.screens.active->cursor.protected_);
    ASSERT_TRUE(t.screens.active->protected_mode == terminal::ansi::ProtectedMode::iso);

    for (size_t y = (size_t)(0); y < (size_t)(t.rows); y++) ASSERT_TRUE(t.isDirty(Point::active(0, (uint32_t)(y))));

    {
        EXPECT_STR("EEE\nEEE\nEEE", t.plainString());
    }
}

TEST(terminal, Terminal__insertBlanks_zero) {
    TERM(t, 5, 2);

    ASSERT_TRUE(t.print('A'));
    ASSERT_TRUE(t.print('B'));
    ASSERT_TRUE(t.print('C'));
    t.setCursorPos(1, 1);

    t.insertBlanks(0);

    {
        EXPECT_STR("ABC", t.plainString());
    }
}

TEST(terminal, Terminal__insertBlanks) {
    /* NOTE: this is not verified with conformance tests, so these
     * tests might actually be verifying wrong behavior. */
    TERM(t, 5, 2);

    ASSERT_TRUE(t.print('A'));
    ASSERT_TRUE(t.print('B'));
    ASSERT_TRUE(t.print('C'));
    t.setCursorPos(1, 1);

    t.clearDirty();
    t.insertBlanks(2);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(!t.isDirty(Point::active(0, 1)));

    {
        EXPECT_STR("  ABC", t.plainString());
    }
}

TEST(terminal, Terminal__insertBlanks_pushes_off_end) {
    /* NOTE: this is not verified with conformance tests, so these
     * tests might actually be verifying wrong behavior. */
    TERM(t, 3, 2);

    ASSERT_TRUE(t.print('A'));
    ASSERT_TRUE(t.print('B'));
    ASSERT_TRUE(t.print('C'));
    t.setCursorPos(1, 1);

    t.clearDirty();
    t.insertBlanks(2);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("  A", t.plainString());
    }
}

TEST(terminal, Terminal__insertBlanks_more_than_size) {
    /* NOTE: this is not verified with conformance tests, so these
     * tests might actually be verifying wrong behavior. */
    TERM(t, 3, 2);

    ASSERT_TRUE(t.print('A'));
    ASSERT_TRUE(t.print('B'));
    ASSERT_TRUE(t.print('C'));
    t.setCursorPos(1, 1);

    t.clearDirty();
    t.insertBlanks(5);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("", t.plainString());
    }
}

TEST(terminal, Terminal__insertBlanks_no_scroll_region__fits) {
    TERM(t, 10, 10);

    PRINT_EACH(t, "ABC");
    t.setCursorPos(1, 1);

    t.clearDirty();
    t.insertBlanks(2);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("  ABC", t.plainString());
    }
}

TEST(terminal, Terminal__insertBlanks_preserves_background_sgr) {
    TERM(t, 10, 10);

    PRINT_EACH(t, "ABC");
    t.setCursorPos(1, 1);
    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 0xFF, 0, 0)));
    t.insertBlanks(2);

    {
        EXPECT_STR("  ABC", t.plainString());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 0)).value;
        ASSERT_TRUE(list_cell.cell->content_tag() == Cell::ContentTag::bg_color_rgb);
        {
            const Cell::RGB _rgb = list_cell.cell->contentColorRgb();
            ASSERT_TRUE(_rgb.r == 0xFF && _rgb.g == 0 && _rgb.b == 0);
        }
    }
}

TEST(terminal, Terminal__insertBlanks_shift_off_screen) {
    TERM(t, 5, 10);

    PRINT_EACH(t, "  ABC");
    t.setCursorPos(1, 3);
    t.clearDirty();
    t.insertBlanks(2);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("  X A", t.plainString());
    }
}

TEST(terminal, Terminal__insertBlanks_split_multi_cell_character) {
    TERM(t, 5, 10);

    PRINT_EACH(t, "123");
    ASSERT_TRUE(t.print(0x6A4B));
    t.setCursorPos(1, 1);
    t.clearDirty();
    t.insertBlanks(1);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR(" 123", t.plainString());
    }
}

TEST(terminal, Terminal__insertBlanks_inside_left_right_scroll_region) {
    TERM(t, 10, 10);

    t.scrolling_region.left = 2;
    t.scrolling_region.right = 4;
    t.setCursorPos(1, 3);
    PRINT_EACH(t, "ABC");
    t.setCursorPos(1, 3);

    t.clearDirty();
    t.insertBlanks(2);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("  X A", t.plainString());
    }
}

TEST(terminal, Terminal__insertBlanks_outside_left_right_scroll_region) {
    TERM(t, 6, 10);

    t.setCursorPos(1, 4);
    PRINT_EACH(t, "ABC");
    t.scrolling_region.left = 2;
    t.scrolling_region.right = 4;
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);
    t.clearDirty();
    t.insertBlanks(2);
    ASSERT_TRUE(!t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("   ABX", t.plainString());
    }
}

TEST(terminal, Terminal__insertBlanks_left_right_scroll_region_large_count) {
    TERM(t, 10, 10);

    t.modes.set(terminal::modes::Mode::origin, true);
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setLeftAndRightMargin(3, 5);
    t.setCursorPos(1, 1);
    t.clearDirty();
    t.insertBlanks(140);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("  X", t.plainString());
    }
}

TEST(terminal, Terminal__insertBlanks_deleting_graphemes) {
    TERM(t, 5, 5);

    /* Disable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    ASSERT_TRUE(t.printString("ABC"));

    /* This is: 👨‍👩‍👧 (which may or may not render correctly) */
    ASSERT_TRUE(t.print(0x1F468));
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(t.print(0x1F469));
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(t.print(0x1F467));

    /* We should have one cell with graphemes */
    Page *page = t.screens.active->cursor.page_pin->node->page();
    ASSERT_TRUE(1 == page->graphemeCount());

    t.setCursorPos(1, 1);
    t.clearDirty();
    t.insertBlanks(4);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("    A", t.plainString());
    }

    /* We should have no graphemes */
    ASSERT_TRUE(0 == page->graphemeCount());
}

TEST(terminal, Terminal__insertBlanks_shift_graphemes) {
    TERM(t, 5, 5);

    /* Enable grapheme clustering */
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    ASSERT_TRUE(t.printString("A"));

    /* This is: 👨‍👩‍👧 (which may or may not render correctly) */
    ASSERT_TRUE(t.print(0x1F468));
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(t.print(0x1F469));
    ASSERT_TRUE(t.print(0x200D));
    ASSERT_TRUE(t.print(0x1F467));

    /* We should have one cell with graphemes */
    Page *page = t.screens.active->cursor.page_pin->node->page();
    ASSERT_TRUE(1 == page->graphemeCount());

    t.setCursorPos(1, 1);
    t.clearDirty();
    t.insertBlanks(1);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR(" A\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7", t.plainString());
    }

    /* We should have no graphemes */
    ASSERT_TRUE(1 == page->graphemeCount());
}

TEST(terminal, Terminal__insertBlanks_split_multi_cell_character_from_tail) {
    TERM(t, 5, 10);

    ASSERT_TRUE(t.printString("\xE6\xA9\x8B" "123"));
    t.setCursorPos(1, 2);
    t.insertBlanks(1);

    {
        EXPECT_STR("   12", t.plainString());
    }
}

TEST(terminal, Terminal__insertBlanks_shifts_hyperlinks) {
    /* osc "8;;http://example.com"
     * printf "link"
     * printf "\r"
     * csi "3@"
     * echo
     *
     * link should be preserved, blanks should not be linked */

    TERM(t, 10, 2);

    NOERR(startLink(*t.screens.active, "http://example.com"));
    ASSERT_TRUE(t.printString("ABC"));
    t.setCursorPos(1, 1);
    t.insertBlanks(2);

    {
        EXPECT_STR("  ABC", t.plainString());
    }

    /* Verify all our cells have a hyperlink */
    for (size_t x = (size_t)(2), n_x = (size_t)(5); x < n_x; x++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen((uint32_t)(x), 0)).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(row->hyperlink());
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(cell->hyperlink());
        hyperlink::Id id = 0;
        const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
        ASSERT_TRUE(id_found);
        ASSERT_TRUE(1 == id);
    }
    for (size_t x = 0; x < (size_t)(2); x++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen((uint32_t)(x), 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(!cell->hyperlink());
        hyperlink::Id id = 0;
        const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
        (void)id_found;
        ASSERT_TRUE(!id_found);
    }
}

TEST(terminal, Terminal__insertBlanks_pushes_hyperlink_off_end_completely) {
    TERM(t, 3, 2);

    NOERR(startLink(*t.screens.active, "http://example.com"));
    ASSERT_TRUE(t.printString("ABC"));
    t.setCursorPos(1, 1);
    t.insertBlanks(3);

    {
        EXPECT_STR("", t.plainString());
    }

    for (size_t x = 0; x < (size_t)(3); x++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen((uint32_t)(x), 0)).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(!row->hyperlink());
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(!cell->hyperlink());
        hyperlink::Id id = 0;
        const bool id_found = list_cell.node->page()->lookupHyperlink(cell, &id);
        (void)id_found;
        ASSERT_TRUE(!id_found);
    }
}

TEST(terminal, Terminal__insertBlanks_wide_char_straddling_right_margin) {
    /* Crash found by AFL++ fuzzer.
     *
     * When a wide character straddles the right scroll margin (head at the
     * margin, spacer_tail just beyond it), insertBlanks shifts the wide head
     * away via swapCells but leaves the orphaned spacer_tail in place,
     * causing a page integrity violation. */
    TERM(t, 10, 5);

    /* Fill row: A B C D 橋 _ _ _ _ _
     * Positions: 0 1 2 3 4W 5T 6 7 8 9 */
    t.setCursorPos(1, 1);
    PRINT_EACH(t, "ABCD");
    /* wide char: head at 4, spacer_tail at 5 */
    ASSERT_TRUE(t.print(0x6A4B));

    /* Set right margin so the wide head is AT the boundary and the
     * spacer_tail is just outside it. */
    t.scrolling_region.right = 4;

    /* Position cursor at x=2 (1-indexed col 3) and insert one blank.
     * This triggers the swap loop which displaces the wide head at
     * position 4 without clearing the spacer_tail at position 5. */
    t.setCursorPos(1, 3);
    t.insertBlanks(1);

    {
        EXPECT_STR("AB CD", t.plainString());
    }
}

TEST(terminal, Terminal__insertBlanks_wide_char_spacer_tail_orphaned_beyond_right_margin) {
    /* Regression test for AFL++ crash.
     *
     * When insertBlanks clears the entire region from cursor to the right
     * margin (scroll_amount == 0), a wide character whose head is AT the
     * right margin gets cleared but its spacer_tail just beyond the margin
     * is left behind, causing a page integrity violation:
     * "spacer tail not following wide" */
    TERM(t, 10, 5);

    /* Fill cols 0–9 with wide chars: 中中中中中
     * Positions: 0W 1T 2W 3T 4W 5T 6W 7T 8W 9T */
    for (size_t i_ = (size_t)(0); i_ < (size_t)(5); i_++) ASSERT_TRUE(t.print(0x4E2D));

    /* Set left/right margins so that the last wide char (cols 8–9)
     * straddles the boundary: head at col 8 (inside), tail at col 9 (outside). */
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    /* 1-indexed: left=0, right=8 */
    t.setLeftAndRightMargin(1, 9);

    /* Cursor is now at (0, 0) after DECSLRM.  Print a narrow char to
     * advance cursor to col 1. */
    ASSERT_TRUE(t.print('a'));

    /* ICH 8: insert 8 blanks at cursor x=1.
     * rem = right(8) - x(1) + 1 = 8, adjusted_count = 8, scroll_amount = 0.
     * The code clears cols 1–8 without noticing the spacer_tail at col 9. */
    t.insertBlanks(8);

    {
        EXPECT_STR("a", t.plainString());
    }
}

TEST(terminal, Terminal__insert_mode_with_space) {
    TERM(t, 10, 2);

    PRINT_EACH(t, "hello");
    t.setCursorPos(1, 2);
    t.modes.set(terminal::modes::Mode::insert, true);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("hXello", t.plainString());
    }
}

TEST(terminal, Terminal__insert_mode_doesn_t_wrap_pushed_characters) {
    TERM(t, 5, 2);

    PRINT_EACH(t, "hello");
    t.setCursorPos(1, 2);
    t.modes.set(terminal::modes::Mode::insert, true);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("hXell", t.plainString());
    }
}

TEST(terminal, Terminal__insert_mode_does_nothing_at_the_end_of_the_line) {
    TERM(t, 5, 2);

    PRINT_EACH(t, "hello");
    t.modes.set(terminal::modes::Mode::insert, true);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("hello\nX", t.plainString());
    }
}

TEST(terminal, Terminal__insert_mode_with_wide_characters) {
    TERM(t, 5, 2);

    PRINT_EACH(t, "hello");
    t.setCursorPos(1, 2);
    t.modes.set(terminal::modes::Mode::insert, true);
    /* 0x1F600 */
    ASSERT_TRUE(t.print(0x1F600));

    {
        EXPECT_STR("h\xF0\x9F\x98\x80" "el", t.plainString());
    }
}

TEST(terminal, Terminal__insert_mode_with_wide_characters_at_end) {
    TERM(t, 5, 2);

    PRINT_EACH(t, "well");
    t.modes.set(terminal::modes::Mode::insert, true);
    /* 0x1F600 */
    ASSERT_TRUE(t.print(0x1F600));

    {
        EXPECT_STR("well\n\xF0\x9F\x98\x80", t.plainString());
    }
}

TEST(terminal, Terminal__insert_mode_pushing_off_wide_character) {
    TERM(t, 5, 2);

    PRINT_EACH(t, "123");
    /* 0x1F600 */
    ASSERT_TRUE(t.print(0x1F600));
    t.modes.set(terminal::modes::Mode::insert, true);
    t.setCursorPos(1, 1);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("X123", t.plainString());
    }
}

TEST(terminal, Terminal__deleteChars) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    t.setCursorPos(1, 2);

    t.clearDirty();
    t.deleteChars(2);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("ADE", t.plainString());
    }
}

TEST(terminal, Terminal__deleteChars_zero_count) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    t.setCursorPos(1, 2);

    t.clearDirty();
    t.deleteChars(0);
    ASSERT_TRUE(!t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("ABCDE", t.plainString());
    }
}

TEST(terminal, Terminal__deleteChars_more_than_half) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    t.setCursorPos(1, 2);

    t.clearDirty();
    t.deleteChars(3);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("AE", t.plainString());
    }
}

TEST(terminal, Terminal__deleteChars_more_than_line_width) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    t.setCursorPos(1, 2);

    t.clearDirty();
    t.deleteChars(10);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("A", t.plainString());
    }
}

TEST(terminal, Terminal__deleteChars_should_shift_left) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    t.setCursorPos(1, 2);

    t.clearDirty();
    t.deleteChars(1);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("ACDE", t.plainString());
    }
}

TEST(terminal, Terminal__deleteChars_resets_pending_wrap) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);
    t.deleteChars(1);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("ABCDX", t.plainString());
    }
}

TEST(terminal, Terminal__deleteChars_resets_wrap) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE123");
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 0)).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(row->wrap());
    }
    t.setCursorPos(1, 1);
    t.deleteChars(1);

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 0)).value;
        const Row *row = list_cell.row;
        ASSERT_TRUE(!row->wrap());
    }

    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("XCDE\n123", t.plainString());
    }
}

TEST(terminal, Terminal__deleteChars_simple_operation) {
    TERM(t, 10, 10);

    ASSERT_TRUE(t.printString("ABC123"));
    t.setCursorPos(1, 3);

    t.clearDirty();
    t.deleteChars(2);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("AB23", t.plainString());
    }
}

TEST(terminal, Terminal__deleteChars_preserves_background_sgr) {
    TERM(t, 10, 10);

    PRINT_EACH(t, "ABC123");
    t.setCursorPos(1, 3);
    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 0xFF, 0, 0)));
    t.deleteChars(2);

    {
        EXPECT_STR("AB23", t.plainString());
    }
    for (size_t x = (size_t)t.cols - 2; x < (size_t)t.cols; x++) {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active((uint32_t)(x), 0)).value;
        ASSERT_TRUE(list_cell.cell->content_tag() == Cell::ContentTag::bg_color_rgb);
        const Cell::RGB rgb = list_cell.cell->contentColorRgb();
        ASSERT_TRUE(rgb.r == 0xFF && rgb.g == 0 && rgb.b == 0);
    }
}

TEST(terminal, Terminal__deleteChars_outside_scroll_region) {
    TERM(t, 6, 10);

    ASSERT_TRUE(t.printString("ABC123"));
    t.scrolling_region.left = 2;
    t.scrolling_region.right = 4;
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);
    t.clearDirty();
    t.deleteChars(2);
    ASSERT_TRUE(!t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);

    {
        EXPECT_STR("ABC123", t.plainString());
    }
}

TEST(terminal, Terminal__deleteChars_inside_scroll_region) {
    TERM(t, 6, 10);

    ASSERT_TRUE(t.printString("ABC123"));
    t.scrolling_region.left = 2;
    t.scrolling_region.right = 4;
    t.setCursorPos(1, 4);

    t.clearDirty();
    t.deleteChars(1);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("ABC2 3", t.plainString());
    }
}

TEST(terminal, Terminal__deleteChars_split_wide_character_from_spacer_tail) {
    TERM(t, 6, 10);

    ASSERT_TRUE(t.printString("A\xE6\xA9\x8B" "123"));
    t.setCursorPos(1, 3);
    t.deleteChars(1);

    {
        EXPECT_STR("A 123", t.plainString());
    }
}

TEST(terminal, Terminal__deleteChars_split_wide_character_from_wide) {
    TERM(t, 6, 10);

    ASSERT_TRUE(t.printString("\xE6\xA9\x8B" "123"));
    t.setCursorPos(1, 1);
    t.deleteChars(1);

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('1' == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }
}

TEST(terminal, Terminal__deleteChars_split_wide_character_from_end) {
    TERM(t, 6, 10);

    ASSERT_TRUE(t.printString("A\xE6\xA9\x8B" "123"));
    t.setCursorPos(1, 1);
    t.deleteChars(1);

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0x6A4B == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(1, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
    }
}

TEST(terminal, Terminal__deleteChars_with_a_spacer_head_at_the_end) {
    TERM(t, 5, 10);

    ASSERT_TRUE(t.printString("0123\xE6\xA9\x8B" "123"));
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(4, 0)).value;
        const Row *row = list_cell.row;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::spacer_head == cell->wide());
        ASSERT_TRUE(row->wrap());
    }

    t.setCursorPos(1, 1);
    t.deleteChars(1);

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::screen(3, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }
}

TEST(terminal, Terminal__deleteChars_split_wide_character_tail) {
    TERM(t, 5, 5);

    t.setCursorPos(1, t.cols - 1);
    /* 橋 */
    ASSERT_TRUE(t.print(0x6A4B));
    t.carriageReturn();
    t.deleteChars(t.cols - 1);
    ASSERT_TRUE(t.print('0'));

    {
        EXPECT_STR("0", t.plainString());
    }
}

TEST(terminal, Terminal__deleteChars_wide_char_boundary_conditions) {
    TERM(t, 8, 1);

    /* EXPLANATION(qwerasd):
     *
     * There are 3 or 4 boundaries to be concerned with in deleteChars,
     * depending on how you count them. Consider the following terminal:
     *
     * +--------+
     * 0 |.ABCDEF.|
     * : ^      : (^ = cursor)
     * +--------+
     *
     * if we DCH 3 we get
     *
     * +--------+
     * 0 |.DEF....|
     * +--------+
     *
     * The boundaries exist at the following points then:
     *
     * +--------+
     * 0 |.ABCDEF.|
     * :11 22 33:
     * +--------+
     *
     * I'm counting 2 for double since it's both the end of the deleted
     * content and the start of the content that is shifted in to place.
     *
     * Now consider wide characters (represented as `WW`) at these boundaries:
     *
     * +--------+
     * 0 |WWaWWbWW|
     * : ^      : (^ = cursor)
     * : ^^^    : (^ = deleted by DCH 3)
     * +--------+
     *
     * -> DCH 3
     * -> The first 2 wide characters are split & destroyed (verified in xterm)
     *
     * +--------+
     * 0 |..bWW...|
     * +--------+ */

    ASSERT_TRUE(t.printString("\xF0\x9F\x98\x80" "a\xF0\x9F\x98\x80" "b\xF0\x9F\x98\x80"));
    {
        EXPECT_STR("\xF0\x9F\x98\x80" "a\xF0\x9F\x98\x80" "b\xF0\x9F\x98\x80", t.plainString());
    }

    t.setCursorPos(1, 2);
    t.deleteChars(3);
    t.screens.active->cursor.page_pin->node->page()->assertIntegrity();

    {
        EXPECT_STR("  b\xF0\x9F\x98\x80", t.plainString());
    }
}

TEST(terminal, Terminal__deleteChars_wide_char_wrap_boundary_conditions) {
    TERM(t, 8, 3);

    /* EXPLANATION(qwerasd):
     * (cont. from "Terminal: deleteChars wide char boundary conditions")
     *
     * Additionally consider soft-wrapped wide chars (`H` = spacer head):
     *
     * +--------+
     * 0 |.......H…
     * 1 …WWabcdeH…
     * : ^      : (^ = cursor)
     * : ^^^    : (^ = deleted by DCH 3)
     * 2 …WW......|
     * +--------+
     *
     * -> DCH 3
     * -> First wide character split and destroyed, including spacer head,
     * second spacer head removed (verified in xterm).
     * -> Wrap state of row reset
     *
     * +--------+
     * 0 |........|
     * 1 |.cde....|
     * 2 |WW......|
     * +--------+
     * */

    ASSERT_TRUE(t.printString(".......\xF0\x9F\x98\x80" "abcde\xF0\x9F\x98\x80......"));
    {
        EXPECT_STR(".......\n\xF0\x9F\x98\x80" "abcde\n\xF0\x9F\x98\x80......", t.plainString());

        EXPECT_STR(".......\xF0\x9F\x98\x80" "abcde\xF0\x9F\x98\x80......", t.plainStringUnwrapped());
    }

    t.setCursorPos(2, 2);
    t.clearDirty();
    t.deleteChars(3);
    t.screens.active->cursor.page_pin->node->page()->assertIntegrity();

    /* Deleting the wide char also clears the spacer head on the previous
     * row, so that row must be dirty too. */
    ASSERT_TRUE(t.isDirty(Point::screen(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::screen(0, 1)));
    ASSERT_TRUE(!t.isDirty(Point::screen(0, 2)));

    {
        EXPECT_STR(".......\n cde\n\xF0\x9F\x98\x80......", t.plainString());

        EXPECT_STR(".......  cde\n\xF0\x9F\x98\x80......", t.plainStringUnwrapped());
    }
}

TEST(terminal, Terminal__deleteChars_wide_char_across_right_margin) {
    TERM(t, 8, 3);

    /* scroll region
     * VVVVVV
     * +-######-+
     * |.abcdeWW|
     * : ^      : (^ = cursor)
     * +--------+
     *
     * DCH 1 */

    ASSERT_TRUE(t.printString("123456\xE6\xA9\x8B"));
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setLeftAndRightMargin(2, 7);

    {
        EXPECT_STR("123456\xE6\xA9\x8B", t.plainString());
    }

    t.setCursorPos(1, 2);
    t.deleteChars(1);
    t.screens.active->cursor.page_pin->node->page()->assertIntegrity();

    /* NOTE: This behavior is slightly inconsistent with xterm. xterm
     * _visually_ splits the wide character (half the wide character shows
     * up in col 6 and half in col 8). In all other wide char split scenarios,
     * xterm clears the cell. Therefore, we've chosen to clear the cell here.
     * Given we have space, we also could actually preserve it, but I haven't
     * yet found a terminal that behaves that way. We should be open to
     * revisiting this behavior but for now we're going with the simpler
     * impl. */
    {
        EXPECT_STR("13456", t.plainString());
    }
}

TEST(terminal, Terminal__saveCursor) {
    TERM(t, 3, 3);

    NOERR(t.setAttribute(attr(A::bold)));
    t.screens.active->charset.gr = terminal::charsets::Slots::G3;
    t.modes.set(terminal::modes::Mode::origin, true);
    t.saveCursor();
    t.screens.active->charset.gr = terminal::charsets::Slots::G0;
    NOERR(t.setAttribute(attr(A::unset)));
    t.modes.set(terminal::modes::Mode::origin, false);
    t.restoreCursor();
    ASSERT_TRUE(t.screens.active->cursor.style.flags.bold);
    ASSERT_TRUE(t.screens.active->charset.gr == terminal::charsets::Slots::G3);
    ASSERT_TRUE(t.modes.get(terminal::modes::Mode::origin));
}

TEST(terminal, Terminal__saveCursor_position) {
    TERM(t, 10, 5);

    t.setCursorPos(1, 5);
    ASSERT_TRUE(t.print('A'));
    t.saveCursor();
    t.setCursorPos(1, 1);
    ASSERT_TRUE(t.print('B'));
    t.restoreCursor();
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("B   AX", t.plainString());
    }
}

TEST(terminal, Terminal__saveCursor_pending_wrap_state) {
    TERM(t, 5, 5);

    t.setCursorPos(1, 5);
    ASSERT_TRUE(t.print('A'));
    t.saveCursor();
    t.setCursorPos(1, 1);
    ASSERT_TRUE(t.print('B'));
    t.restoreCursor();
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("B   A\nX", t.plainString());
    }
}

TEST(terminal, Terminal__saveCursor_origin_mode) {
    TERM(t, 10, 5);

    t.modes.set(terminal::modes::Mode::origin, true);
    t.saveCursor();
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setLeftAndRightMargin(3, 5);
    t.setTopAndBottomMargin(2, 4);
    t.restoreCursor();
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("X", t.plainString());
    }
}

TEST(terminal, Terminal__saveCursor_resize) {
    TERM(t, 10, 5);

    t.setCursorPos(1, 10);
    t.saveCursor();
    ASSERT_TRUE(t.resize(talloc(), Terminal::Resize(5, 5)) == Terminal::ResizeError::none);
    t.restoreCursor();
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("    X", t.plainString());
    }
}

TEST(terminal, Terminal__saveCursor_protected_pen) {
    TERM(t, 10, 5);

    t.setProtectedMode(terminal::ansi::ProtectedMode::iso);
    ASSERT_TRUE(t.screens.active->cursor.protected_);
    t.setCursorPos(1, 10);
    t.saveCursor();
    t.setProtectedMode(terminal::ansi::ProtectedMode::off);
    ASSERT_TRUE(!t.screens.active->cursor.protected_);
    t.restoreCursor();
    ASSERT_TRUE(t.screens.active->cursor.protected_);
}

TEST(terminal, Terminal__saveCursor_doesn_t_modify_hyperlink_state) {
    TERM(t, 3, 3);

    NOERR(startLink(*t.screens.active, "http://example.com"));
    const auto id = t.screens.active->cursor.hyperlink_id;
    t.saveCursor();
    ASSERT_TRUE(id == t.screens.active->cursor.hyperlink_id);
    t.restoreCursor();
    ASSERT_TRUE(id == t.screens.active->cursor.hyperlink_id);
}

TEST(terminal, Terminal__restoreCursor_uses_default_style_on_OutOfSpace) {
    /* Tests that restoreCursor falls back to default style when
     * manualStyleUpdate fails with OutOfSpace (can't split a 1-row page
     * and styles are at max capacity). */

    /* Use a single row so the page can't be split */
    TERM(t, 10, 1);

    /* Set a style and save the cursor */
    NOERR(t.setAttribute(attr(A::bold)));
    t.saveCursor();

    /* Clear the style */
    NOERR(t.setAttribute(attr(A::unset)));
    ASSERT_TRUE(!t.screens.active->cursor.style.flags.bold);

    /* Fill the style map to max capacity */
    const size::CellCountInt max_styles = (size::CellCountInt)UINT16_MAX;
    while (t.screens.active->cursor.page_pin->node->capacity().styles < max_styles) {
        PageList::Node *new_node = nullptr;
        if (t.screens.active->increaseCapacity(t.screens.active->cursor.page_pin->node,
                                               Maybe<PageList::IncreaseCapacity>(
                                                   PageList::IncreaseCapacity::styles),
                                               &new_node) != PageList::IncreaseCapacityError::none)
            break;
    }

    Page *page = t.screens.active->cursor.page_pin->node->page();
    ASSERT_TRUE(max_styles == page->capacity.styles);

    /* Fill all style slots using the StyleSet's layout capacity which accounts
     * for the load factor. The capacity in the layout is the actual max number
     * of items that can be stored. */
    {
        page->pauseIntegrityChecks(true);
        struct PauseGuard {
            Page *p;
            ~PauseGuard() {
                p->assertIntegrity();
                p->pauseIntegrityChecks(false);
            }
        } pause_guard = {page};
        (void)pause_guard;

        const size_t max_items = page->styles.layout.cap;
        for (size_t n = 1; n < max_items; n++) {
            style::Style st;
            style::RGB rgb;
            rgb.r = (uint8_t)(n & 0xFF);
            rgb.g = (uint8_t)((n >> 8) & 0xFF);
            rgb.b = (uint8_t)((n >> 16) & 0xFF);
            st.bg_color = style::Style::Color::makeRgb(rgb);
            style::Id id;
            if (page->styles.add(page->memory, st, &id) != ref_counted_set::AddError::none) break;
        }
    }

    /* Restore cursor - should fall back to default style since page
     * can't be split (1 row) and styles are at max capacity */
    t.restoreCursor();

    /* The style should be reset to default because OutOfSpace occurred */
    ASSERT_TRUE(!t.screens.active->cursor.style.flags.bold);
    ASSERT_TRUE(style::default_id == t.screens.active->cursor.style_id);
}

TEST(terminal, Terminal__setProtectedMode) {
    TERM(t, 3, 3);

    ASSERT_TRUE(!t.screens.active->cursor.protected_);
    t.setProtectedMode(terminal::ansi::ProtectedMode::off);
    ASSERT_TRUE(!t.screens.active->cursor.protected_);
    t.setProtectedMode(terminal::ansi::ProtectedMode::iso);
    ASSERT_TRUE(t.screens.active->cursor.protected_);
    t.setProtectedMode(terminal::ansi::ProtectedMode::dec);
    ASSERT_TRUE(t.screens.active->cursor.protected_);
    t.setProtectedMode(terminal::ansi::ProtectedMode::off);
    ASSERT_TRUE(!t.screens.active->cursor.protected_);
}

TEST(terminal, Terminal__eraseLine_simple_erase_right) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    t.setCursorPos(1, 3);
    t.clearDirty();
    t.eraseLine(terminal::csi::EraseLine::right, false);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("AB", t.plainString());
    }
}

TEST(terminal, Terminal__eraseLine_resets_pending_wrap) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);
    t.eraseLine(terminal::csi::EraseLine::right, false);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);
    ASSERT_TRUE(t.print('B'));

    {
        EXPECT_STR("ABCDB", t.plainString());
    }
}

TEST(terminal, Terminal__eraseLine_resets_wrap) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE123");
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 0)).value;
        ASSERT_TRUE(list_cell.row->wrap());
    }

    t.setCursorPos(1, 1);
    t.eraseLine(terminal::csi::EraseLine::right, false);

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 0)).value;
        ASSERT_TRUE(!list_cell.row->wrap());
    }
    ASSERT_TRUE(t.print('X'));

    {
        EXPECT_STR("X\n123", t.plainString());
    }
}

TEST(terminal, Terminal__eraseLine_right_preserves_background_sgr) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    t.setCursorPos(1, 2);
    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 0xFF, 0, 0)));
    t.eraseLine(terminal::csi::EraseLine::right, false);

    {
        EXPECT_STR("A", t.plainString());
        for (size_t x = (size_t)(1), n_x = (size_t)(5); x < n_x; x++) {
            const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active((uint32_t)(x), 0)).value;
            ASSERT_TRUE(list_cell.cell->content_tag() == Cell::ContentTag::bg_color_rgb);
            {
                const Cell::RGB _rgb = list_cell.cell->contentColorRgb();
                ASSERT_TRUE(_rgb.r == 0xFF && _rgb.g == 0 && _rgb.b == 0);
            }
        }
    }
}

TEST(terminal, Terminal__eraseLine_right_wide_character) {
    TERM(t, 10, 5);

    PRINT_EACH(t, "AB");
    ASSERT_TRUE(t.print(0x6A4B));
    PRINT_EACH(t, "DE");
    t.setCursorPos(1, 4);
    t.clearDirty();
    t.eraseLine(terminal::csi::EraseLine::right, false);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("AB", t.plainString());
    }
}

TEST(terminal, Terminal__eraseLine_right_protected_attributes_respected_with_iso) {
    TERM(t, 5, 5);

    t.setProtectedMode(terminal::ansi::ProtectedMode::iso);
    PRINT_EACH(t, "ABC");
    t.setCursorPos(1, 1);
    t.clearDirty();
    t.eraseLine(terminal::csi::EraseLine::right, false);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("ABC", t.plainString());
    }
}

TEST(terminal, Terminal__eraseLine_right_protected_attributes_ignored_with_dec_most_recent) {
    TERM(t, 5, 5);

    t.setProtectedMode(terminal::ansi::ProtectedMode::iso);
    PRINT_EACH(t, "ABC");
    t.setProtectedMode(terminal::ansi::ProtectedMode::dec);
    t.setProtectedMode(terminal::ansi::ProtectedMode::off);
    t.setCursorPos(1, 2);
    t.clearDirty();
    t.eraseLine(terminal::csi::EraseLine::right, false);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("A", t.plainString());
    }
}

TEST(terminal, Terminal__eraseLine_right_protected_attributes_ignored_with_dec_set) {
    TERM(t, 5, 5);

    t.setProtectedMode(terminal::ansi::ProtectedMode::dec);
    PRINT_EACH(t, "ABC");
    t.setCursorPos(1, 2);
    t.clearDirty();
    t.eraseLine(terminal::csi::EraseLine::right, false);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("A", t.plainString());
    }
}

TEST(terminal, Terminal__eraseLine_right_protected_requested) {
    TERM(t, 10, 5);

    PRINT_EACH(t, "12345678");
    t.setCursorPos(t.screens.active->cursor.y + 1, 6);
    t.setProtectedMode(terminal::ansi::ProtectedMode::dec);
    ASSERT_TRUE(t.print('X'));
    t.setCursorPos(t.screens.active->cursor.y + 1, 4);
    t.clearDirty();
    t.eraseLine(terminal::csi::EraseLine::right, true);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("123  X", t.plainString());
    }
}

TEST(terminal, Terminal__eraseLine_simple_erase_left) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    t.setCursorPos(1, 3);
    t.clearDirty();
    t.eraseLine(terminal::csi::EraseLine::left, false);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("   DE", t.plainString());
    }
}

TEST(terminal, Terminal__eraseLine_left_resets_wrap) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);
    t.clearDirty();
    t.eraseLine(terminal::csi::EraseLine::left, false);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);
    ASSERT_TRUE(t.print('B'));

    {
        EXPECT_STR("    B", t.plainString());
    }
}

TEST(terminal, Terminal__eraseLine_left_preserves_background_sgr) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    t.setCursorPos(1, 2);
    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 0xFF, 0, 0)));
    t.eraseLine(terminal::csi::EraseLine::left, false);

    {
        EXPECT_STR("  CDE", t.plainString());
        for (size_t x = 0; x < (size_t)(2); x++) {
            const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active((uint32_t)(x), 0)).value;
            ASSERT_TRUE(list_cell.cell->content_tag() == Cell::ContentTag::bg_color_rgb);
            {
                const Cell::RGB _rgb = list_cell.cell->contentColorRgb();
                ASSERT_TRUE(_rgb.r == 0xFF && _rgb.g == 0 && _rgb.b == 0);
            }
        }
    }
}

TEST(terminal, Terminal__eraseLine_left_wide_character) {
    TERM(t, 10, 5);

    PRINT_EACH(t, "AB");
    ASSERT_TRUE(t.print(0x6A4B));
    PRINT_EACH(t, "DE");
    t.setCursorPos(1, 3);
    t.clearDirty();
    t.eraseLine(terminal::csi::EraseLine::left, false);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("    DE", t.plainString());
    }
}

TEST(terminal, Terminal__eraseLine_left_protected_attributes_respected_with_iso) {
    TERM(t, 5, 5);

    t.setProtectedMode(terminal::ansi::ProtectedMode::iso);
    PRINT_EACH(t, "ABC");
    t.setCursorPos(1, 1);
    t.clearDirty();
    t.eraseLine(terminal::csi::EraseLine::left, false);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("ABC", t.plainString());
    }
}

TEST(terminal, Terminal__eraseLine_left_protected_attributes_ignored_with_dec_most_recent) {
    TERM(t, 5, 5);

    t.setProtectedMode(terminal::ansi::ProtectedMode::iso);
    PRINT_EACH(t, "ABC");
    t.setProtectedMode(terminal::ansi::ProtectedMode::dec);
    t.setProtectedMode(terminal::ansi::ProtectedMode::off);
    t.setCursorPos(1, 2);
    t.clearDirty();
    t.eraseLine(terminal::csi::EraseLine::left, false);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("  C", t.plainString());
    }
}

TEST(terminal, Terminal__eraseLine_left_protected_attributes_ignored_with_dec_set) {
    TERM(t, 5, 5);

    t.setProtectedMode(terminal::ansi::ProtectedMode::dec);
    PRINT_EACH(t, "ABC");
    t.setCursorPos(1, 2);
    t.clearDirty();
    t.eraseLine(terminal::csi::EraseLine::left, false);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("  C", t.plainString());
    }
}

TEST(terminal, Terminal__eraseLine_left_protected_requested) {
    TERM(t, 10, 5);

    PRINT_EACH(t, "123456789");
    t.setCursorPos(t.screens.active->cursor.y + 1, 6);
    t.setProtectedMode(terminal::ansi::ProtectedMode::dec);
    ASSERT_TRUE(t.print('X'));
    t.setCursorPos(t.screens.active->cursor.y + 1, 8);
    t.clearDirty();
    t.eraseLine(terminal::csi::EraseLine::left, true);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("     X  9", t.plainString());
    }
}

TEST(terminal, Terminal__eraseLine_complete_preserves_background_sgr) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    t.setCursorPos(1, 2);
    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 0xFF, 0, 0)));
    t.eraseLine(terminal::csi::EraseLine::complete, false);

    {
        EXPECT_STR("", t.plainString());
        for (size_t x = 0; x < (size_t)(5); x++) {
            const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active((uint32_t)(x), 0)).value;
            ASSERT_TRUE(list_cell.cell->content_tag() == Cell::ContentTag::bg_color_rgb);
            {
                const Cell::RGB _rgb = list_cell.cell->contentColorRgb();
                ASSERT_TRUE(_rgb.r == 0xFF && _rgb.g == 0 && _rgb.b == 0);
            }
        }
    }
}

TEST(terminal, Terminal__eraseLine_complete_resets_wrap) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE123");
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 0)).value;
        ASSERT_TRUE(list_cell.row->wrap());
    }

    t.setCursorPos(1, 1);
    t.eraseLine(terminal::csi::EraseLine::complete, false);

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 0)).value;
        ASSERT_TRUE(!list_cell.row->wrap());
    }
    ASSERT_TRUE(t.print('X'));
    {
        Terminal::Resize r(10, 5);
        ASSERT_TRUE(t.resize(talloc(), r) == Terminal::ResizeError::none);
    }

    {
        EXPECT_STR("X\n123", t.plainString());
    }
}

TEST(terminal, Terminal__eraseLine_complete_protected_attributes_respected_with_iso) {
    TERM(t, 5, 5);

    t.setProtectedMode(terminal::ansi::ProtectedMode::iso);
    PRINT_EACH(t, "ABC");
    t.setCursorPos(1, 1);
    t.clearDirty();
    t.eraseLine(terminal::csi::EraseLine::complete, false);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("ABC", t.plainString());
    }
}

TEST(terminal, Terminal__eraseLine_complete_protected_attributes_ignored_with_dec_most_recent) {
    TERM(t, 5, 5);

    t.setProtectedMode(terminal::ansi::ProtectedMode::iso);
    PRINT_EACH(t, "ABC");
    t.setProtectedMode(terminal::ansi::ProtectedMode::dec);
    t.setProtectedMode(terminal::ansi::ProtectedMode::off);
    t.setCursorPos(1, 2);
    t.clearDirty();
    t.eraseLine(terminal::csi::EraseLine::complete, false);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("", t.plainString());
    }
}

TEST(terminal, Terminal__eraseLine_complete_protected_attributes_ignored_with_dec_set) {
    TERM(t, 5, 5);

    t.setProtectedMode(terminal::ansi::ProtectedMode::dec);
    PRINT_EACH(t, "ABC");
    t.setCursorPos(1, 2);
    t.clearDirty();
    t.eraseLine(terminal::csi::EraseLine::complete, false);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("", t.plainString());
    }
}

TEST(terminal, Terminal__eraseLine_complete_protected_requested) {
    TERM(t, 10, 5);

    PRINT_EACH(t, "123456789");
    t.setCursorPos(t.screens.active->cursor.y + 1, 6);
    t.setProtectedMode(terminal::ansi::ProtectedMode::dec);
    ASSERT_TRUE(t.print('X'));
    t.setCursorPos(t.screens.active->cursor.y + 1, 8);
    t.clearDirty();
    t.eraseLine(terminal::csi::EraseLine::complete, true);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("     X", t.plainString());
    }
}

TEST(terminal, Terminal__tabClear_single) {
    TERM(t, 30, 5);

    t.horizontalTab();
    t.tabClear(terminal::csi::TabClear::current);
    ASSERT_TRUE(!t.isDirty(Point::active(0, 0)));
    t.setCursorPos(1, 1);
    t.horizontalTab();
    ASSERT_TRUE(16 == t.screens.active->cursor.x);
}

TEST(terminal, Terminal__tabClear_all) {
    TERM(t, 30, 5);

    t.tabClear(terminal::csi::TabClear::all);
    ASSERT_TRUE(!t.isDirty(Point::active(0, 0)));
    t.setCursorPos(1, 1);
    t.horizontalTab();
    ASSERT_TRUE(29 == t.screens.active->cursor.x);
}

TEST(terminal, Terminal__printRepeat_simple) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("A"));
    ASSERT_TRUE(t.printRepeat(1));
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("AA", t.plainString());
    }
}

TEST(terminal, Terminal__printRepeat_wrap) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("    A"));
    ASSERT_TRUE(t.printRepeat(1));
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("    A\nA", t.plainString());
    }
}

TEST(terminal, Terminal__printRepeat_no_previous_character) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printRepeat(1));
    ASSERT_TRUE(!t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("", t.plainString());
    }
}

TEST(terminal, Terminal__printSlice_simple_ascii) {
    TERM(t, 10, 3);

    {
        static const uint32_t cps[] = {'h', 'e', 'l', 'l', 'o'};
        ASSERT_TRUE(t.printSlice(cps, sizeof(cps) / sizeof(cps[0])));
    }
    ASSERT_TRUE(5 == t.screens.active->cursor.x);
    ASSERT_TRUE(t.previous_char.has && 'o' == t.previous_char.value);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));

    {
        EXPECT_STR("hello", t.plainString());
    }
}

TEST(terminal, Terminal__printSlice_wraps_and_scrolls) {
    TERM(t, 5, 2);

    /* 12 chars: fills row 1 (5), row 2 (5), wraps+scrolls, 2 more. */
    {
        static const uint32_t cps[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l'};
        ASSERT_TRUE(t.printSlice(cps, sizeof(cps) / sizeof(cps[0])));
    }

    {
        EXPECT_STR("fghij\nkl", t.plainString());
    }
    ASSERT_TRUE(2 == t.screens.active->cursor.x);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);
}

TEST(terminal, Terminal__printSlice_pending_wrap_state) {
    TERM(t, 5, 2);

    {
        static const uint32_t cps[] = {'a', 'b', 'c', 'd', 'e'};
        ASSERT_TRUE(t.printSlice(cps, sizeof(cps) / sizeof(cps[0])));
    }
    ASSERT_TRUE(4 == t.screens.active->cursor.x);
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);

    {
        EXPECT_STR("abcde", t.plainString());
    }
}

TEST(terminal, Terminal__printSlice_differential_fuzz_vs_print) {

    /* Multiple seeds and terminal sizes for coverage, including a
     * tiny terminal to stress wrap/scroll edge cases. */
    zigstd::DefaultPrng prng = zigstd::DefaultPrng::init(0xC0FFEE);
    zigstd::Random rand(&prng);
    testPrintSliceDifferential(rand, 500, 80, 24);
    testPrintSliceDifferential(rand, 500, 10, 4);
    testPrintSliceDifferential(rand, 500, 5, 2);
    testPrintSliceDifferential(rand, 200, 2, 2);
}

TEST(terminal, Terminal__printAttributes) {
    TERM(t, 5, 5);

    /* Wisp: printAttributes returns the formatted string; upstream writes
     * into the caller's buffer. */
    struct UnsetGuard {
        Terminal *t;
        ~UnsetGuard() { (void)t->setAttribute(terminal::sgr::Attribute::make(A::unset)); }
    };

    {
        NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_fg, 1, 2, 3)));
        UnsetGuard guard = {&t};
        (void)guard;
        EXPECT_STR("0;38:2::1:2:3", t.printAttributes());
    }

    {
        NOERR(t.setAttribute(attr(A::bold)));
        NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 1, 2, 3)));
        UnsetGuard guard = {&t};
        (void)guard;
        EXPECT_STR("0;1;48:2::1:2:3", t.printAttributes());
    }

    {
        NOERR(t.setAttribute(attr(A::bold)));
        NOERR(t.setAttribute(attr(A::faint)));
        NOERR(t.setAttribute(attr(A::italic)));
        NOERR(t.setAttribute(terminal::sgr::Attribute::makeUnderline(terminal::sgr::Attribute::Underline::single)));
        NOERR(t.setAttribute(attr(A::blink)));
        NOERR(t.setAttribute(attr(A::inverse)));
        NOERR(t.setAttribute(attr(A::invisible)));
        NOERR(t.setAttribute(attr(A::strikethrough)));
        NOERR(t.setAttribute(attr(A::overline)));
        NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_fg, 100, 200, 255)));
        NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 101, 102, 103)));
        UnsetGuard guard = {&t};
        (void)guard;
        EXPECT_STR("0;1;2;3;4;53;5;7;8;9;38:2::100:200:255;48:2::101:102:103", t.printAttributes());
    }

    struct Case {
        terminal::sgr::Attribute::Underline underline;
        const char *expected;
    };
    static const Case cases[] = {
        {terminal::sgr::Attribute::Underline::single, "0;4"},
        {terminal::sgr::Attribute::Underline::double_, "0;4:2"},
        {terminal::sgr::Attribute::Underline::curly, "0;4:3"},
        {terminal::sgr::Attribute::Underline::dotted, "0;4:4"},
        {terminal::sgr::Attribute::Underline::dashed, "0;4:5"},
    };
    for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
        const Case &c = cases[ci];
        NOERR(t.setAttribute(terminal::sgr::Attribute::makeUnderline(c.underline)));
        EXPECT_STR(c.expected, t.printAttributes());
    }

    NOERR(t.setAttribute(attr(A::unset)));
    { EXPECT_STR("0", t.printAttributes()); }
}

TEST(terminal, Terminal__eraseDisplay_simple_erase_below) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABC");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "DEF");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "GHI");
    t.setCursorPos(2, 2);

    t.clearDirty();
    t.eraseDisplay(terminal::csi::EraseDisplay::below, false);

    ASSERT_TRUE(!t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 2)));

    {
        EXPECT_STR("ABC\nD", t.plainString());
    }
}

TEST(terminal, Terminal__eraseDisplay_erase_below_preserves_SGR_bg) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABC");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "DEF");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "GHI");
    t.setCursorPos(2, 2);

    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 0xFF, 0, 0)));
    t.eraseDisplay(terminal::csi::EraseDisplay::below, false);

    {
        EXPECT_STR("ABC\nD", t.plainString());
        for (size_t x = (size_t)(1), n_x = (size_t)(5); x < n_x; x++) {
            const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active((uint32_t)(x), 1)).value;
            ASSERT_TRUE(list_cell.cell->content_tag() == Cell::ContentTag::bg_color_rgb);
            {
                const Cell::RGB _rgb = list_cell.cell->contentColorRgb();
                ASSERT_TRUE(_rgb.r == 0xFF && _rgb.g == 0 && _rgb.b == 0);
            }
        }
    }
}

TEST(terminal, Terminal__eraseDisplay_below_split_multi_cell) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("AB\xE6\xA9\x8B" "C"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DE\xE6\xA9\x8B" "F"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GH\xE6\xA9\x8BI"));
    t.setCursorPos(2, 4);
    t.eraseDisplay(terminal::csi::EraseDisplay::below, false);

    {
        EXPECT_STR("AB\xE6\xA9\x8B" "C\nDE", t.plainString());
    }
}

TEST(terminal, Terminal__eraseDisplay_below_protected_attributes_respected_with_iso) {
    TERM(t, 5, 5);

    t.setProtectedMode(terminal::ansi::ProtectedMode::iso);
    PRINT_EACH(t, "ABC");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "DEF");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "GHI");
    t.setCursorPos(2, 2);
    t.eraseDisplay(terminal::csi::EraseDisplay::below, false);

    {
        EXPECT_STR("ABC\nDEF\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__eraseDisplay_below_protected_attributes_ignored_with_dec_most_recent) {
    TERM(t, 5, 5);

    t.setProtectedMode(terminal::ansi::ProtectedMode::iso);
    PRINT_EACH(t, "ABC");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "DEF");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "GHI");
    t.setProtectedMode(terminal::ansi::ProtectedMode::dec);
    t.setProtectedMode(terminal::ansi::ProtectedMode::off);
    t.setCursorPos(2, 2);
    t.eraseDisplay(terminal::csi::EraseDisplay::below, false);

    {
        EXPECT_STR("ABC\nD", t.plainString());
    }
}

TEST(terminal, Terminal__eraseDisplay_below_protected_attributes_ignored_with_dec_set) {
    TERM(t, 5, 5);

    t.setProtectedMode(terminal::ansi::ProtectedMode::dec);
    PRINT_EACH(t, "ABC");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "DEF");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "GHI");
    t.setCursorPos(2, 2);
    t.eraseDisplay(terminal::csi::EraseDisplay::below, false);

    {
        EXPECT_STR("ABC\nD", t.plainString());
    }
}

TEST(terminal, Terminal__eraseDisplay_below_protected_attributes_respected_with_force) {
    TERM(t, 5, 5);

    t.setProtectedMode(terminal::ansi::ProtectedMode::dec);
    PRINT_EACH(t, "ABC");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "DEF");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "GHI");
    t.setCursorPos(2, 2);
    t.eraseDisplay(terminal::csi::EraseDisplay::below, true);

    {
        EXPECT_STR("ABC\nDEF\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__eraseDisplay_simple_erase_above) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABC");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "DEF");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "GHI");
    t.setCursorPos(2, 2);

    t.clearDirty();
    t.eraseDisplay(terminal::csi::EraseDisplay::above, false);
    ASSERT_TRUE(t.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(t.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(!t.isDirty(Point::active(0, 2)));

    {
        EXPECT_STR("\n  F\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__eraseDisplay_erase_above_preserves_SGR_bg) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABC");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "DEF");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "GHI");
    t.setCursorPos(2, 2);

    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 0xFF, 0, 0)));
    t.eraseDisplay(terminal::csi::EraseDisplay::above, false);

    {
        EXPECT_STR("\n  F\nGHI", t.plainString());
        for (size_t x = 0; x < (size_t)(2); x++) {
            const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active((uint32_t)(x), 1)).value;
            ASSERT_TRUE(list_cell.cell->content_tag() == Cell::ContentTag::bg_color_rgb);
            {
                const Cell::RGB _rgb = list_cell.cell->contentColorRgb();
                ASSERT_TRUE(_rgb.r == 0xFF && _rgb.g == 0 && _rgb.b == 0);
            }
        }
    }
}

TEST(terminal, Terminal__eraseDisplay_above_split_multi_cell) {
    TERM(t, 5, 5);

    ASSERT_TRUE(t.printString("AB\xE6\xA9\x8B" "C"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("DE\xE6\xA9\x8B" "F"));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.printString("GH\xE6\xA9\x8BI"));
    t.setCursorPos(2, 3);
    t.eraseDisplay(terminal::csi::EraseDisplay::above, false);

    {
        EXPECT_STR("\n    F\nGH\xE6\xA9\x8BI", t.plainString());
    }
}

TEST(terminal, Terminal__eraseDisplay_above_protected_attributes_respected_with_iso) {
    TERM(t, 5, 5);

    t.setProtectedMode(terminal::ansi::ProtectedMode::iso);
    PRINT_EACH(t, "ABC");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "DEF");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "GHI");
    t.setCursorPos(2, 2);
    t.eraseDisplay(terminal::csi::EraseDisplay::above, false);

    {
        EXPECT_STR("ABC\nDEF\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__eraseDisplay_above_protected_attributes_ignored_with_dec_most_recent) {
    TERM(t, 5, 5);

    t.setProtectedMode(terminal::ansi::ProtectedMode::iso);
    PRINT_EACH(t, "ABC");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "DEF");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "GHI");
    t.setProtectedMode(terminal::ansi::ProtectedMode::dec);
    t.setProtectedMode(terminal::ansi::ProtectedMode::off);
    t.setCursorPos(2, 2);
    t.eraseDisplay(terminal::csi::EraseDisplay::above, false);

    {
        EXPECT_STR("\n  F\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__eraseDisplay_above_protected_attributes_ignored_with_dec_set) {
    TERM(t, 5, 5);

    t.setProtectedMode(terminal::ansi::ProtectedMode::dec);
    PRINT_EACH(t, "ABC");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "DEF");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "GHI");
    t.setCursorPos(2, 2);
    t.eraseDisplay(terminal::csi::EraseDisplay::above, false);

    {
        EXPECT_STR("\n  F\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__eraseDisplay_above_protected_attributes_respected_with_force) {
    TERM(t, 5, 5);

    t.setProtectedMode(terminal::ansi::ProtectedMode::dec);
    PRINT_EACH(t, "ABC");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "DEF");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "GHI");
    t.setCursorPos(2, 2);
    t.eraseDisplay(terminal::csi::EraseDisplay::above, true);

    {
        EXPECT_STR("ABC\nDEF\nGHI", t.plainString());
    }
}

TEST(terminal, Terminal__eraseDisplay_protected_complete) {
    TERM(t, 10, 5);

    ASSERT_TRUE(t.print('A'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "123456789");
    t.setCursorPos(t.screens.active->cursor.y + 1, 6);
    t.setProtectedMode(terminal::ansi::ProtectedMode::dec);
    ASSERT_TRUE(t.print('X'));
    t.setCursorPos(t.screens.active->cursor.y + 1, 4);

    t.clearDirty();
    t.eraseDisplay(terminal::csi::EraseDisplay::complete, true);
    for (size_t y = (size_t)(0); y < (size_t)(t.rows); y++) ASSERT_TRUE(t.isDirty(Point::active(0, (uint32_t)(y))));

    {
        EXPECT_STR("\n     X", t.plainString());
    }
}

TEST(terminal, Terminal__eraseDisplay_protected_below) {
    TERM(t, 10, 5);

    ASSERT_TRUE(t.print('A'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "123456789");
    t.setCursorPos(t.screens.active->cursor.y + 1, 6);
    t.setProtectedMode(terminal::ansi::ProtectedMode::dec);
    ASSERT_TRUE(t.print('X'));
    t.setCursorPos(t.screens.active->cursor.y + 1, 4);
    t.eraseDisplay(terminal::csi::EraseDisplay::below, true);

    {
        EXPECT_STR("A\n123  X", t.plainString());
    }
}

TEST(terminal, Terminal__eraseDisplay_scroll_complete) {
    TERM(t, 10, 5);

    ASSERT_TRUE(t.print('A'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    t.eraseDisplay(terminal::csi::EraseDisplay::scroll_complete, false);

    {
        EXPECT_STR("", t.plainString());
    }
}

TEST(terminal, Terminal__eraseDisplay_protected_above) {
    TERM(t, 10, 3);

    ASSERT_TRUE(t.print('A'));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "123456789");
    t.setCursorPos(t.screens.active->cursor.y + 1, 6);
    t.setProtectedMode(terminal::ansi::ProtectedMode::dec);
    ASSERT_TRUE(t.print('X'));
    t.setCursorPos(t.screens.active->cursor.y + 1, 8);
    t.eraseDisplay(terminal::csi::EraseDisplay::above, true);

    {
        EXPECT_STR("\n     X  9", t.plainString());
    }
}

TEST(terminal, Terminal__eraseDisplay_complete_preserves_cursor) {
    TERM(t, 5, 5);

    /* Set our cursur */
    NOERR(t.setAttribute(attr(A::bold)));
    ASSERT_TRUE(t.printString("AAAA"));
    ASSERT_TRUE(t.screens.active->cursor.style_id != style::default_id);

    /* Erasing the display may detect that our style is no longer in use
     * and prune our style, which we don't want because its still our
     * active cursor. */
    t.eraseDisplay(terminal::csi::EraseDisplay::complete, false);
    ASSERT_TRUE(t.screens.active->cursor.style_id != style::default_id);
}

TEST(terminal, Terminal__semantic_prompt) {
    TERM(t, 10, 5);

    /* Prompt */
    ASSERT_TRUE(t.semanticPrompt(SemanticPromptCommand::init(SemanticPromptCommand::Action::fresh_line_new_prompt)));
    PRINT_EACH(t, "hello");
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(5 == t.screens.active->cursor.x);
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(t.screens.active->cursor.x - 1, t.screens.active->cursor.y)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::SemanticContent::prompt == cell->semantic_content());

        const Row *row = list_cell.row;
        ASSERT_TRUE(Row::SemanticPrompt::prompt == row->semantic_prompt());
    }

    /* Start input but end it on EOL */
    ASSERT_TRUE(t.semanticPrompt(SemanticPromptCommand::init(SemanticPromptCommand::Action::end_prompt_start_input_terminate_eol)));
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());

    /* Write some output */
    ASSERT_TRUE(1 == t.screens.active->cursor.y);
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    PRINT_EACH(t, "world");
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(t.screens.active->cursor.x - 1, t.screens.active->cursor.y)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::SemanticContent::output == cell->semantic_content());

        const Row *row = list_cell.row;
        ASSERT_TRUE(Row::SemanticPrompt::none == row->semantic_prompt());
    }
}

TEST(terminal, Terminal__semantic_prompt_continuations) {
    TERM(t, 10, 5);

    /* Prompt */
    ASSERT_TRUE(t.semanticPrompt(SemanticPromptCommand::init(SemanticPromptCommand::Action::fresh_line_new_prompt)));
    PRINT_EACH(t, "hello");
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(5 == t.screens.active->cursor.x);
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(t.screens.active->cursor.x - 1, t.screens.active->cursor.y)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::SemanticContent::prompt == cell->semantic_content());

        const Row *row = list_cell.row;
        ASSERT_TRUE(Row::SemanticPrompt::prompt == row->semantic_prompt());
    }

    /* Start input but end it on EOL */
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.semanticPrompt(semanticPromptCmd(SemanticPromptCommand::Action::prompt_start, "k=c")));

    /* Write some output */
    ASSERT_TRUE(1 == t.screens.active->cursor.y);
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    PRINT_EACH(t, "world");
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(t.screens.active->cursor.x - 1, t.screens.active->cursor.y)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::SemanticContent::prompt == cell->semantic_content());

        const Row *row = list_cell.row;
        ASSERT_TRUE(Row::SemanticPrompt::prompt_continuation == row->semantic_prompt());
    }
}

TEST(terminal, Terminal__index_in_prompt_mode_marks_new_row_as_prompt_continuation) {
    /* This tests the Fish shell workaround: when in prompt mode and we get
     * a newline, assume the new row is a prompt continuation (since Fish
     * doesn't emit OSC133 k=s markers for continuation lines). */
    TERM(t, 10, 5);

    /* Start a prompt */
    ASSERT_TRUE(t.semanticPrompt(SemanticPromptCommand::init(SemanticPromptCommand::Action::prompt_start)));
    PRINT_EACH(t, "hello");

    /* Verify first row is marked as prompt */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 0)).value;
        ASSERT_TRUE(Row::SemanticPrompt::prompt == list_cell.row->semantic_prompt());
    }

    /* Now do a linefeed while still in prompt mode */
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());

    /* The new row should automatically be marked as prompt continuation */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 1)).value;
        ASSERT_TRUE(Row::SemanticPrompt::prompt_continuation == list_cell.row->semantic_prompt());
    }

    /* The cursor semantic content should still be prompt */
    ASSERT_TRUE(Cell::SemanticContent::prompt == t.screens.active->cursor.semantic_content);
}

TEST(terminal, Terminal__index_in_input_mode_does_not_mark_new_row_as_prompt) {
    /* Input mode should NOT trigger prompt continuation on newline
     * (only prompt mode does, not input mode) */
    TERM(t, 10, 5);

    /* Start a prompt then switch to input */
    ASSERT_TRUE(t.semanticPrompt(SemanticPromptCommand::init(SemanticPromptCommand::Action::prompt_start)));
    PRINT_EACH(t, "$ ");
    ASSERT_TRUE(t.semanticPrompt(SemanticPromptCommand::init(SemanticPromptCommand::Action::end_prompt_start_input)));
    PRINT_EACH(t, "echo \\");

    /* Linefeed while in input mode */
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());

    /* The new row should be marked as prompt continuation */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 1)).value;
        ASSERT_TRUE(Row::SemanticPrompt::prompt_continuation == list_cell.row->semantic_prompt());
    }

    /* Our cursor should still be in input */
    ASSERT_TRUE(Cell::SemanticContent::input == t.screens.active->cursor.semantic_content);
}

TEST(terminal, Terminal__index_in_output_mode_does_not_mark_new_row_as_prompt) {
    /* Output mode should NOT trigger prompt continuation */
    TERM(t, 10, 5);

    /* Complete prompt cycle: prompt -> input -> output */
    ASSERT_TRUE(t.semanticPrompt(SemanticPromptCommand::init(SemanticPromptCommand::Action::prompt_start)));
    PRINT_EACH(t, "$ ");
    ASSERT_TRUE(t.semanticPrompt(SemanticPromptCommand::init(SemanticPromptCommand::Action::end_prompt_start_input)));
    PRINT_EACH(t, "ls");
    ASSERT_TRUE(t.semanticPrompt(SemanticPromptCommand::init(SemanticPromptCommand::Action::end_input_start_output)));

    /* Linefeed while in output mode */
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());

    /* The new row should NOT be marked as a prompt */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 1)).value;
        ASSERT_TRUE(Row::SemanticPrompt::none == list_cell.row->semantic_prompt());
    }
}

TEST(terminal, Terminal__OSC133C_at_x_0_on_prompt_row_clears_prompt_mark) {
    /* This tests the second Fish heuristic: when Fish emits a newline
     * then immediately sends OSC133C (start output) at column 0, we
     * should clear the prompt continuation mark we just set. */
    TERM(t, 10, 5);

    /* Start a prompt */
    ASSERT_TRUE(t.semanticPrompt(SemanticPromptCommand::init(SemanticPromptCommand::Action::prompt_start)));
    PRINT_EACH(t, "$ echo \\");

    /* Simulate Fish behavior: newline first (which marks next row as prompt) */
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());

    /* Verify the new row is marked as prompt continuation */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 1)).value;
        ASSERT_TRUE(Row::SemanticPrompt::prompt_continuation == list_cell.row->semantic_prompt());
    }

    /* Now Fish sends OSC133C at column 0 (cursor is still at x=0) */
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    ASSERT_TRUE(t.semanticPrompt(SemanticPromptCommand::init(SemanticPromptCommand::Action::end_input_start_output)));

    /* The prompt continuation should be cleared */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 1)).value;
        ASSERT_TRUE(Row::SemanticPrompt::none == list_cell.row->semantic_prompt());
    }
}

TEST(terminal, Terminal__OSC133C_at_x_0_on_prompt_row_does_not_clear_prompt_mark) {
    /* If we're not at column 0, we shouldn't clear the prompt mark */
    TERM(t, 10, 5);

    /* Start a prompt on a row */
    ASSERT_TRUE(t.semanticPrompt(SemanticPromptCommand::init(SemanticPromptCommand::Action::prompt_start)));
    PRINT_EACH(t, "$ ");

    /* Move to a new line and mark it as prompt continuation manually */
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.semanticPrompt(semanticPromptCmd(SemanticPromptCommand::Action::prompt_start, "k=c")));
    PRINT_EACH(t, "> ");

    /* Verify the row is marked as prompt continuation */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 1)).value;
        ASSERT_TRUE(Row::SemanticPrompt::prompt_continuation == list_cell.row->semantic_prompt());
    }

    /* Now send OSC133C but cursor is NOT at column 0 */
    ASSERT_TRUE(t.screens.active->cursor.x > 0);
    ASSERT_TRUE(t.semanticPrompt(SemanticPromptCommand::init(SemanticPromptCommand::Action::end_input_start_output)));

    /* The prompt continuation should NOT be cleared (we're not at x=0) */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 1)).value;
        ASSERT_TRUE(Row::SemanticPrompt::prompt_continuation == list_cell.row->semantic_prompt());
    }
}

TEST(terminal, Terminal__multiple_newlines_in_prompt_mode_marks_all_rows) {
    /* Multiple newlines should each mark their row as prompt continuation */
    TERM(t, 10, 5);

    /* Start a prompt */
    ASSERT_TRUE(t.semanticPrompt(SemanticPromptCommand::init(SemanticPromptCommand::Action::prompt_start)));
    PRINT_EACH(t, "line1");

    /* Multiple newlines */
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "line2");
    t.carriageReturn();
    ASSERT_TRUE(t.linefeed());
    PRINT_EACH(t, "line3");

    /* First row should be prompt */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 0)).value;
        ASSERT_TRUE(Row::SemanticPrompt::prompt == list_cell.row->semantic_prompt());
    }

    /* Second and third rows should be prompt continuation */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 1)).value;
        ASSERT_TRUE(Row::SemanticPrompt::prompt_continuation == list_cell.row->semantic_prompt());
    }
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 2)).value;
        ASSERT_TRUE(Row::SemanticPrompt::prompt_continuation == list_cell.row->semantic_prompt());
    }
}

TEST(terminal, Terminal__OSC133A_click_events_1_sets_click_to_click_events) {
    TERM(t, 10, 5);

    /* Verify default state is none */
    ASSERT_TRUE(Screen::SemanticPrompt::SemanticClick::Kind::none == t.screens.active->semantic_prompt.click.tag);

    /* OSC 133;A with click_events=1 */
    ASSERT_TRUE(t.semanticPrompt(semanticPromptCmd(SemanticPromptCommand::Action::fresh_line_new_prompt, "click_events=1")));

    ASSERT_TRUE(Screen::SemanticPrompt::SemanticClick::Kind::click_events == t.screens.active->semantic_prompt.click.tag);
    ASSERT_TRUE(terminal::osc::semantic_prompt::ClickEvents::absolute == t.screens.active->semantic_prompt.click.click_events);
}

TEST(terminal, Terminal__OSC133A_click_events_2_sets_click_to_click_events__relative_) {
    TERM(t, 10, 5);

    /* Verify default state is none */
    ASSERT_TRUE(Screen::SemanticPrompt::SemanticClick::Kind::none == t.screens.active->semantic_prompt.click.tag);

    /* OSC 133;A with click_events=2 */
    ASSERT_TRUE(t.semanticPrompt(semanticPromptCmd(SemanticPromptCommand::Action::fresh_line_new_prompt, "click_events=2")));

    ASSERT_TRUE(Screen::SemanticPrompt::SemanticClick::Kind::click_events == t.screens.active->semantic_prompt.click.tag);
    ASSERT_TRUE(terminal::osc::semantic_prompt::ClickEvents::relative == t.screens.active->semantic_prompt.click.click_events);
}

TEST(terminal, Terminal__OSC133A_click_events_0_does_not_set_click_events) {
    TERM(t, 10, 5);

    /* OSC 133;A with click_events=0 */
    ASSERT_TRUE(t.semanticPrompt(semanticPromptCmd(SemanticPromptCommand::Action::fresh_line_new_prompt, "click_events=0")));

    /* Should remain none since click_events=0 doesn't activate anything */
    ASSERT_TRUE(Screen::SemanticPrompt::SemanticClick::Kind::none == t.screens.active->semantic_prompt.click.tag);
}

TEST(terminal, Terminal__OSC133A_cl_option_sets_click_to_cl_value) {
    TERM(t, 10, 5);

    /* OSC 133;A with cl=m (multiple) */
    ASSERT_TRUE(t.semanticPrompt(semanticPromptCmd(SemanticPromptCommand::Action::fresh_line_new_prompt, "cl=m")));

    ASSERT_TRUE(Screen::SemanticPrompt::SemanticClick::Kind::cl == t.screens.active->semantic_prompt.click.tag);
    ASSERT_TRUE(terminal::osc::semantic_prompt::Click::multiple == t.screens.active->semantic_prompt.click.cl);
}

TEST(terminal, Terminal__OSC133A_cl_line_sets_click_to_line) {
    TERM(t, 10, 5);

    ASSERT_TRUE(t.semanticPrompt(semanticPromptCmd(SemanticPromptCommand::Action::fresh_line_new_prompt, "cl=line")));

    ASSERT_TRUE(Screen::SemanticPrompt::SemanticClick::Kind::cl == t.screens.active->semantic_prompt.click.tag);
    ASSERT_TRUE(terminal::osc::semantic_prompt::Click::line == t.screens.active->semantic_prompt.click.cl);
}

TEST(terminal, Terminal__OSC133A_click_events_1_takes_priority_over_cl) {
    TERM(t, 10, 5);

    /* OSC 133;A with both click_events=1 and cl=m */
    ASSERT_TRUE(t.semanticPrompt(semanticPromptCmd(SemanticPromptCommand::Action::fresh_line_new_prompt, "click_events=1;cl=m")));

    /* click_events should take priority */
    ASSERT_TRUE(Screen::SemanticPrompt::SemanticClick::Kind::click_events == t.screens.active->semantic_prompt.click.tag);
    ASSERT_TRUE(terminal::osc::semantic_prompt::ClickEvents::absolute == t.screens.active->semantic_prompt.click.click_events);
}

TEST(terminal, Terminal__OSC133A_click_events_0_falls_back_to_cl) {
    TERM(t, 10, 5);

    /* OSC 133;A with click_events=0 and cl=v */
    ASSERT_TRUE(t.semanticPrompt(semanticPromptCmd(SemanticPromptCommand::Action::fresh_line_new_prompt, "click_events=0;cl=v")));

    /* Should fall back to cl since click_events is disabled */
    ASSERT_TRUE(Screen::SemanticPrompt::SemanticClick::Kind::cl == t.screens.active->semantic_prompt.click.tag);
    ASSERT_TRUE(terminal::osc::semantic_prompt::Click::conservative_vertical == t.screens.active->semantic_prompt.click.cl);
}

TEST(terminal, Terminal__OSC133A_no_click_options_leaves_click_as_none) {
    TERM(t, 10, 5);

    /* OSC 133;A with no click-related options */
    ASSERT_TRUE(t.semanticPrompt(semanticPromptCmd(SemanticPromptCommand::Action::fresh_line_new_prompt, "aid=123")));

    ASSERT_TRUE(Screen::SemanticPrompt::SemanticClick::Kind::none == t.screens.active->semantic_prompt.click.tag);
}

TEST(terminal, Terminal__cursorIsAtPrompt) {
    TERM(t, 10, 3);

    ASSERT_TRUE(!t.cursorIsAtPrompt());
    ASSERT_TRUE(t.semanticPrompt(SemanticPromptCommand::init(SemanticPromptCommand::Action::prompt_start)));
    ASSERT_TRUE(t.cursorIsAtPrompt());
    PRINT_EACH(t, "$ ");

    /* Input is also a prompt */
    ASSERT_TRUE(t.semanticPrompt(SemanticPromptCommand::init(SemanticPromptCommand::Action::end_prompt_start_input)));
    ASSERT_TRUE(t.cursorIsAtPrompt());
    PRINT_EACH(t, "ls");

    /* But once we say we're starting output, we're not a prompt
     * (cursor is not at x=0, so the Fish heuristic doesn't trigger) */
    ASSERT_TRUE(t.semanticPrompt(SemanticPromptCommand::init(SemanticPromptCommand::Action::end_input_start_output)));
    /* Still a prompt because this line has a prompt */
    ASSERT_TRUE(t.cursorIsAtPrompt());
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(!t.cursorIsAtPrompt());

    /* Until we know we're at a prompt again */
    ASSERT_TRUE(t.linefeed());
    ASSERT_TRUE(t.semanticPrompt(SemanticPromptCommand::init(SemanticPromptCommand::Action::prompt_start)));
    ASSERT_TRUE(t.cursorIsAtPrompt());
}

TEST(terminal, Terminal__cursorIsAtPrompt_alternate_screen) {
    TERM(t, 3, 2);

    ASSERT_TRUE(!t.cursorIsAtPrompt());
    ASSERT_TRUE(t.semanticPrompt(SemanticPromptCommand::init(SemanticPromptCommand::Action::prompt_start)));
    ASSERT_TRUE(t.cursorIsAtPrompt());

    /* Secondary screen is never a prompt */
    ASSERT_TRUE(t.switchScreenMode(Terminal::SwitchScreenMode::mode_1049, true));
    ASSERT_TRUE(!t.cursorIsAtPrompt());
    ASSERT_TRUE(t.semanticPrompt(SemanticPromptCommand::init(SemanticPromptCommand::Action::prompt_start)));
    ASSERT_TRUE(!t.cursorIsAtPrompt());
}

TEST(terminal, Terminal__cursor_defaults_update_current_default_cursor) {
    Terminal::Options t_opts((size::CellCountInt)10, (size::CellCountInt)10);
    t_opts.default_cursor_style = Screen::CursorStyle::bar;
    t_opts.default_cursor_blink = Maybe<bool>(true);
    TermHolder t_holder(t_opts);
    ASSERT_TRUE(t_holder.ok);
    Terminal &t = t_holder.t;

    /* Initialization applies the configured defaults. */
    ASSERT_TRUE(t.cursor.is_default);
    ASSERT_TRUE(Screen::CursorStyle::bar == t.screens.active->cursor.cursor_style);
    ASSERT_TRUE(t.modes.get(terminal::modes::Mode::cursor_blinking));

    /* Configuration changes are immediately visible while the cursor still
     * follows its defaults. */
    t.setDefaultCursorStyle(Screen::CursorStyle::underline);
    t.setDefaultCursorBlink(Maybe<bool>(false));
    ASSERT_TRUE(t.cursor.is_default);
    ASSERT_TRUE(Screen::CursorStyle::underline == t.screens.active->cursor.cursor_style);
    ASSERT_TRUE(!t.modes.get(terminal::modes::Mode::cursor_blinking));

    /* Null restores the terminal emulator's blinking default. */
    t.setDefaultCursorBlink(Maybe<bool>());
    ASSERT_TRUE(t.modes.get(terminal::modes::Mode::cursor_blinking));
}

TEST(terminal, Terminal__cursor_defaults_do_not_override_explicit_cursor) {
    TERM(t, 10, 10);

    t.setCursorStyle(terminal::ansi::CursorStyle::blinking_bar);
    ASSERT_TRUE(!t.cursor.is_default);
    ASSERT_TRUE(Screen::CursorStyle::bar == t.screens.active->cursor.cursor_style);
    ASSERT_TRUE(t.modes.get(terminal::modes::Mode::cursor_blinking));

    /* New defaults are retained without replacing the explicit appearance. */
    t.setDefaultCursorStyle(Screen::CursorStyle::underline);
    t.setDefaultCursorBlink(Maybe<bool>(false));
    ASSERT_TRUE(Screen::CursorStyle::underline == t.cursor.default_style);
    ASSERT_TRUE(t.cursor.default_blink.has && !t.cursor.default_blink.value);
    ASSERT_TRUE(Screen::CursorStyle::bar == t.screens.active->cursor.cursor_style);
    ASSERT_TRUE(t.modes.get(terminal::modes::Mode::cursor_blinking));

    /* Selecting the default applies the values that changed above. */
    t.setCursorStyle(terminal::ansi::CursorStyle::default_);
    ASSERT_TRUE(t.cursor.is_default);
    ASSERT_TRUE(Screen::CursorStyle::underline == t.screens.active->cursor.cursor_style);
    ASSERT_TRUE(!t.modes.get(terminal::modes::Mode::cursor_blinking));

    /* A full reset also leaves the cursor on the configured defaults. */
    t.setCursorStyle(terminal::ansi::CursorStyle::steady_block);
    t.fullReset();
    ASSERT_TRUE(t.cursor.is_default);
    ASSERT_TRUE(Screen::CursorStyle::underline == t.screens.active->cursor.cursor_style);
    ASSERT_TRUE(!t.modes.get(terminal::modes::Mode::cursor_blinking));
}

TEST(terminal, Terminal__fullReset_with_a_non_empty_pen) {
    TERM(t, 80, 80);

    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_fg, 0xFF, 0, 0x7F)));
    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 0xFF, 0, 0x7F)));
    t.screens.active->cursor.semantic_content = Cell::SemanticContent::input;
    t.fullReset();

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(t.screens.active->cursor.x, t.screens.active->cursor.y)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(cell->style_id() == 0);
    }

    ASSERT_TRUE(0 == t.screens.active->cursor.style_id);
    ASSERT_TRUE(Cell::SemanticContent::output == t.screens.active->cursor.semantic_content);
}

TEST(terminal, Terminal__fullReset_hyperlink) {
    TERM(t, 80, 80);

    NOERR(startLink(*t.screens.active, "http://example.com"));
    t.fullReset();
    ASSERT_TRUE(0 == t.screens.active->cursor.hyperlink_id);
}

TEST(terminal, Terminal__fullReset_with_a_non_empty_saved_cursor) {
    TERM(t, 80, 80);

    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_fg, 0xFF, 0, 0x7F)));
    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 0xFF, 0, 0x7F)));
    t.saveCursor();
    t.fullReset();

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(t.screens.active->cursor.x, t.screens.active->cursor.y)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(cell->style_id() == 0);
    }

    ASSERT_TRUE(0 == t.screens.active->cursor.style_id);
}

TEST(terminal, Terminal__fullReset_origin_mode) {
    TERM(t, 10, 10);

    t.setCursorPos(3, 5);
    t.modes.set(terminal::modes::Mode::origin, true);
    t.fullReset();

    /* Origin mode should be reset and the cursor should be moved */
    ASSERT_TRUE(0 == t.screens.active->cursor.y);
    ASSERT_TRUE(0 == t.screens.active->cursor.x);
    ASSERT_TRUE(!t.modes.get(terminal::modes::Mode::origin));
}

TEST(terminal, Terminal__fullReset_status_display) {
    TERM(t, 10, 10);

    t.status_display = terminal::ansi::StatusDisplay::status_line;
    t.fullReset();
    ASSERT_TRUE(t.status_display == terminal::ansi::StatusDisplay::main);
}

/* Wisp: upstream test "Terminal__fullReset_preserves_kitty_graphics_limits" is gated on build_options.kitty_graphics, which this
 * build disables; not ported. */


TEST(terminal, Terminal__fullReset_clears_alt_screen_kitty_keyboard_state) {
    TERM(t, 10, 10);

    ASSERT_TRUE(t.switchScreenMode(Terminal::SwitchScreenMode::mode_1049, true));
    {
        terminal::kitty::KeyFlags f;
        f.disambiguate = true;
        f.report_events = false;
        f.report_alternates = true;
        f.report_all = true;
        f.report_associated = true;
        t.screens.active->kitty_keyboard.push(f);
    }
    ASSERT_TRUE(t.switchScreenMode(Terminal::SwitchScreenMode::mode_1049, false));

    t.fullReset();
    ASSERT_TRUE(t.screens.get(ScreenSet::Key::alternate) == nullptr);
}

TEST(terminal, Terminal__fullReset_default_modes) {
    Terminal::Options t_opts((size::CellCountInt)10, (size::CellCountInt)10);
    t_opts.default_modes.setField(terminal::modes::Mode::grapheme_cluster, true);
    TermHolder t_holder(t_opts);
    ASSERT_TRUE(t_holder.ok);
    Terminal &t = t_holder.t;
    ASSERT_TRUE(t.modes.get(terminal::modes::Mode::grapheme_cluster));
    t.fullReset();
    ASSERT_TRUE(t.modes.get(terminal::modes::Mode::grapheme_cluster));
}

TEST(terminal, Terminal__fullReset_tracked_pins) {
    TERM(t, 80, 80);

    /* Create a tracked pin */
    Pin *p = t.screens.active->pages.trackPin(*t.screens.active->cursor.page_pin);
    ASSERT_TRUE(p != nullptr);
    t.fullReset();
    ASSERT_TRUE(t.screens.active->pages.pinIsValid(*p));
}

TEST(terminal, Terminal__resize_less_cols_with_wide_char_then_print) {
    TERM(t, 3, 3);

    ASSERT_TRUE(t.print('x'));
    /* 0x1F600 */
    ASSERT_TRUE(t.print(0x1F600));
    ASSERT_TRUE(t.resize(talloc(), Terminal::Resize(2, 3)) == Terminal::ResizeError::none);
    t.setCursorPos(1, 2);
    /* 0x1F600 */
    ASSERT_TRUE(t.print(0x1F600));
}

TEST(terminal, Terminal__resize_with_left_and_right_margin_set) {
    const auto cols = 70;
    const auto rows = 23;
    TERM(t, cols, rows);

    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    ASSERT_TRUE(t.print('0'));
    t.modes.set(terminal::modes::Mode::enable_mode_3, true);
    ASSERT_TRUE(t.resize(talloc(), Terminal::Resize(cols, rows)) == Terminal::ResizeError::none);
    t.setLeftAndRightMargin(2, 0);
    ASSERT_TRUE(t.printRepeat(1850));
    (void)t.modes.restore(terminal::modes::Mode::enable_mode_3);
    ASSERT_TRUE(t.resize(talloc(), Terminal::Resize(cols, rows)) == Terminal::ResizeError::none);
}

TEST(terminal, Terminal__resize_without_scrollback_pull) {
    TERM(t, 5, 3);
    t.flags.resize_pull_scrollback = false;

    /* This is configuration so it should survive a reset. */
    t.fullReset();
    ASSERT_TRUE(!t.flags.resize_pull_scrollback);

    ASSERT_TRUE(t.printString("1\n2\n3\n4\n5"));
    ASSERT_TRUE(t.resize(talloc(), Terminal::Resize(5, 5)) == Terminal::ResizeError::none);
    ASSERT_TRUE(2 == t.screens.active->cursor.y);
    {
        EXPECT_STR("3\n4\n5", t.plainString());
    }
}

TEST(terminal, Terminal__resize_with_wraparound_off) {
    const auto cols = 4;
    const auto rows = 2;
    TERM(t, cols, rows);

    t.modes.set(terminal::modes::Mode::wraparound, false);
    ASSERT_TRUE(t.print('0'));
    ASSERT_TRUE(t.print('1'));
    ASSERT_TRUE(t.print('2'));
    ASSERT_TRUE(t.print('3'));
    const auto new_cols = 2;
    ASSERT_TRUE(t.resize(talloc(), Terminal::Resize(new_cols, rows)) == Terminal::ResizeError::none);

    EXPECT_STR("01", t.plainString());
}

TEST(terminal, Terminal__resize_with_wraparound_on) {
    const auto cols = 4;
    const auto rows = 2;
    TERM(t, cols, rows);

    t.modes.set(terminal::modes::Mode::wraparound, true);
    ASSERT_TRUE(t.print('0'));
    ASSERT_TRUE(t.print('1'));
    ASSERT_TRUE(t.print('2'));
    ASSERT_TRUE(t.print('3'));
    const auto new_cols = 2;
    ASSERT_TRUE(t.resize(talloc(), Terminal::Resize(new_cols, rows)) == Terminal::ResizeError::none);

    EXPECT_STR("01\n23", t.plainString());
}

TEST(terminal, Terminal__resize_with_high_unique_style_per_cell) {
    TERM(t, 30, 30);

    for (size_t y = 0; y < (size_t)(t.rows); y++) {
        for (size_t x = 0; x < (size_t)(t.cols); x++) {
            t.setCursorPos(y, x);
            NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, (uint8_t)x, (uint8_t)y, 0)));
            ASSERT_TRUE(t.print('x'));
        }
    }

    ASSERT_TRUE(t.resize(talloc(), Terminal::Resize(60, 30)) == Terminal::ResizeError::none);
}

TEST(terminal, Terminal__resize_with_high_unique_style_per_cell_with_wrapping) {
    TERM(t, 30, 30);

    const uint16_t cell_count = (uint16_t)(t.rows * t.cols);
    for (size_t i = 0; i < (size_t)(cell_count); i++) {
        const uint8_t r = (uint8_t)(i >> 8);
        const uint8_t g = (uint8_t)(i & 0xFF);

        NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, r, g, 0)));
        ASSERT_TRUE(t.print('x'));
    }

    ASSERT_TRUE(t.resize(talloc(), Terminal::Resize(60, 30)) == Terminal::ResizeError::none);
}

TEST(terminal, Terminal__resize_with_reflow_and_saved_cursor) {
    TERM(t, 2, 3);
    ASSERT_TRUE(t.printString("1A2B"));
    t.setCursorPos(2, 2);
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(t.screens.active->cursor.x, t.screens.active->cursor.y)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('B' == cell->contentCodepoint());
    }

    {
        EXPECT_STR("1A\n2B", t.plainString());
    }

    t.saveCursor();
    ASSERT_TRUE(t.resize(talloc(), Terminal::Resize(5, 3)) == Terminal::ResizeError::none);
    t.restoreCursor();

    {
        EXPECT_STR("1A2B", t.plainString());
    }

    /* Verify our cursor is still in the same place */
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(t.screens.active->cursor.x, t.screens.active->cursor.y)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('B' == cell->contentCodepoint());
    }
}

TEST(terminal, Terminal__resize_with_reflow_and_saved_cursor_pending_wrap) {
    TERM(t, 2, 3);
    ASSERT_TRUE(t.printString("1A2B"));
    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(t.screens.active->cursor.x, t.screens.active->cursor.y)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE('B' == cell->contentCodepoint());
    }

    {
        EXPECT_STR("1A\n2B", t.plainString());
    }

    t.saveCursor();
    ASSERT_TRUE(t.resize(talloc(), Terminal::Resize(5, 3)) == Terminal::ResizeError::none);
    t.restoreCursor();

    {
        EXPECT_STR("1A2B", t.plainString());
    }

    /* Pending wrap should be reset */
    ASSERT_TRUE(t.print('X'));
    {
        EXPECT_STR("1A2BX", t.plainString());
    }
}

TEST(terminal, Terminal__DECCOLM_without_DEC_mode_40) {
    TERM(t, 5, 5);

    t.modes.set(terminal::modes::Mode::_132_column, true);
    ASSERT_TRUE(t.deccolm(talloc(), Terminal::DeccolmMode::cols_132) == Terminal::ResizeError::none);
    ASSERT_TRUE(5 == t.cols);
    ASSERT_TRUE(5 == t.rows);
    ASSERT_TRUE(!t.modes.get(terminal::modes::Mode::_132_column));
}

TEST(terminal, Terminal__DECCOLM_unset) {
    TERM(t, 5, 5);

    t.modes.set(terminal::modes::Mode::enable_mode_3, true);
    ASSERT_TRUE(t.deccolm(talloc(), Terminal::DeccolmMode::cols_80) == Terminal::ResizeError::none);
    ASSERT_TRUE(80 == t.cols);
    ASSERT_TRUE(5 == t.rows);
}

TEST(terminal, Terminal__DECCOLM_resets_pending_wrap) {
    TERM(t, 5, 5);

    PRINT_EACH(t, "ABCDE");
    ASSERT_TRUE(t.screens.active->cursor.pending_wrap);

    t.modes.set(terminal::modes::Mode::enable_mode_3, true);
    ASSERT_TRUE(t.deccolm(talloc(), Terminal::DeccolmMode::cols_80) == Terminal::ResizeError::none);
    ASSERT_TRUE(80 == t.cols);
    ASSERT_TRUE(5 == t.rows);
    ASSERT_TRUE(!t.screens.active->cursor.pending_wrap);
}

TEST(terminal, Terminal__DECCOLM_preserves_SGR_bg) {
    TERM(t, 5, 5);

    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 0xFF, 0, 0)));
    t.modes.set(terminal::modes::Mode::enable_mode_3, true);
    ASSERT_TRUE(t.deccolm(talloc(), Terminal::DeccolmMode::cols_80) == Terminal::ResizeError::none);

    {
        const PageList::Cell list_cell = t.screens.active->pages.getCell(Point::active(0, 0)).value;
        ASSERT_TRUE(list_cell.cell->content_tag() == Cell::ContentTag::bg_color_rgb);
        {
            const Cell::RGB _rgb = list_cell.cell->contentColorRgb();
            ASSERT_TRUE(_rgb.r == 0xFF && _rgb.g == 0 && _rgb.b == 0);
        }
    }
}

TEST(terminal, Terminal__DECCOLM_resets_scroll_region) {
    TERM(t, 5, 5);

    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setTopAndBottomMargin(2, 3);
    t.setLeftAndRightMargin(3, 5);

    t.modes.set(terminal::modes::Mode::enable_mode_3, true);
    ASSERT_TRUE(t.deccolm(talloc(), Terminal::DeccolmMode::cols_80) == Terminal::ResizeError::none);

    ASSERT_TRUE(t.modes.get(terminal::modes::Mode::enable_left_and_right_margin));
    ASSERT_TRUE(0 == t.scrolling_region.top);
    ASSERT_TRUE(4 == t.scrolling_region.bottom);
    ASSERT_TRUE(0 == t.scrolling_region.left);
    ASSERT_TRUE(79 == t.scrolling_region.right);
}

TEST(terminal, Terminal__mode_47_alt_screen_plain) {
    TERM(t, 5, 5);

    /* Print on primary screen */
    ASSERT_TRUE(t.printString("1A"));

    /* Go to alt screen with mode 47 */
    ASSERT_TRUE(t.switchScreenMode(Terminal::SwitchScreenMode::mode_47, true));
    ASSERT_TRUE(ScreenSet::Key::alternate == t.screens.active_key);

    /* Screen should be empty */
    {
        EXPECT_STR("", t.plainString());
    }

    /* Print on alt screen. This should be off center because
     * we copy the cursor over from the primary screen */
    ASSERT_TRUE(t.printString("2B"));
    {
        EXPECT_STR("  2B", t.plainString());
    }

    /* Go back to primary */
    ASSERT_TRUE(t.switchScreenMode(Terminal::SwitchScreenMode::mode_47, false));
    ASSERT_TRUE(ScreenSet::Key::primary == t.screens.active_key);

    /* Primary screen should still have the original content */
    {
        EXPECT_STR("1A", t.plainString());
    }

    /* Go back to alt screen with mode 47 */
    ASSERT_TRUE(t.switchScreenMode(Terminal::SwitchScreenMode::mode_47, true));
    ASSERT_TRUE(ScreenSet::Key::alternate == t.screens.active_key);

    /* Screen should retain content */
    {
        EXPECT_STR("  2B", t.plainString());
    }
}

TEST(terminal, Terminal__mode_47_copies_cursor_both_directions) {
    TERM(t, 5, 5);

    /* Color our cursor red */
    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_fg, 0xFF, 0, 0x7F)));

    /* Go to alt screen with mode 47 */
    ASSERT_TRUE(t.switchScreenMode(Terminal::SwitchScreenMode::mode_47, true));
    ASSERT_TRUE(ScreenSet::Key::alternate == t.screens.active_key);

    /* Verify that our style is set */
    {
        ASSERT_TRUE(t.screens.active->cursor.style_id != style::default_id);
        Page *page = t.screens.active->cursor.page_pin->node->page();
        ASSERT_TRUE(1 == page->styles.count());
        ASSERT_TRUE(page->styles.refCount((const void *)page->memory, t.screens.active->cursor.style_id) > 0);
    }

    /* Set a new style */
    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_fg, 0, 0xFF, 0)));

    /* Go back to primary */
    ASSERT_TRUE(t.switchScreenMode(Terminal::SwitchScreenMode::mode_47, false));
    ASSERT_TRUE(ScreenSet::Key::primary == t.screens.active_key);

    /* Verify that our style is still set */
    {
        ASSERT_TRUE(t.screens.active->cursor.style_id != style::default_id);
        Page *page = t.screens.active->cursor.page_pin->node->page();
        ASSERT_TRUE(1 == page->styles.count());
        ASSERT_TRUE(page->styles.refCount((const void *)page->memory, t.screens.active->cursor.style_id) > 0);
    }
}

TEST(terminal, Terminal__mode_1047_alt_screen_plain) {
    TERM(t, 5, 5);

    /* Print on primary screen */
    ASSERT_TRUE(t.printString("1A"));

    /* Go to alt screen with mode 47 */
    ASSERT_TRUE(t.switchScreenMode(Terminal::SwitchScreenMode::mode_1047, true));
    ASSERT_TRUE(ScreenSet::Key::alternate == t.screens.active_key);

    /* Screen should be empty */
    {
        EXPECT_STR("", t.plainString());
    }

    /* Print on alt screen. This should be off center because
     * we copy the cursor over from the primary screen */
    ASSERT_TRUE(t.printString("2B"));
    {
        EXPECT_STR("  2B", t.plainString());
    }

    /* Go back to primary */
    ASSERT_TRUE(t.switchScreenMode(Terminal::SwitchScreenMode::mode_1047, false));
    ASSERT_TRUE(ScreenSet::Key::primary == t.screens.active_key);

    /* Primary screen should still have the original content */
    {
        EXPECT_STR("1A", t.plainString());
    }

    /* Go back to alt screen with mode 1047 */
    ASSERT_TRUE(t.switchScreenMode(Terminal::SwitchScreenMode::mode_1047, true));
    ASSERT_TRUE(ScreenSet::Key::alternate == t.screens.active_key);

    /* Screen should be empty */
    {
        EXPECT_STR("", t.plainString());
    }
}

TEST(terminal, Terminal__mode_1047_copies_cursor_both_directions) {
    TERM(t, 5, 5);

    /* Color our cursor red */
    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_fg, 0xFF, 0, 0x7F)));

    /* Go to alt screen with mode 47 */
    ASSERT_TRUE(t.switchScreenMode(Terminal::SwitchScreenMode::mode_1047, true));
    ASSERT_TRUE(ScreenSet::Key::alternate == t.screens.active_key);

    /* Verify that our style is set */
    {
        ASSERT_TRUE(t.screens.active->cursor.style_id != style::default_id);
        Page *page = t.screens.active->cursor.page_pin->node->page();
        ASSERT_TRUE(1 == page->styles.count());
        ASSERT_TRUE(page->styles.refCount((const void *)page->memory, t.screens.active->cursor.style_id) > 0);
    }

    /* Set a new style */
    NOERR(t.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_fg, 0, 0xFF, 0)));

    /* Go back to primary */
    ASSERT_TRUE(t.switchScreenMode(Terminal::SwitchScreenMode::mode_1047, false));
    ASSERT_TRUE(ScreenSet::Key::primary == t.screens.active_key);

    /* Verify that our style is still set */
    {
        ASSERT_TRUE(t.screens.active->cursor.style_id != style::default_id);
        Page *page = t.screens.active->cursor.page_pin->node->page();
        ASSERT_TRUE(1 == page->styles.count());
        ASSERT_TRUE(page->styles.refCount((const void *)page->memory, t.screens.active->cursor.style_id) > 0);
    }
}

TEST(terminal, Terminal__mode_1049_alt_screen_plain) {
    TERM(t, 5, 5);

    /* Print on primary screen */
    ASSERT_TRUE(t.printString("1A"));

    /* Go to alt screen with mode 47 */
    ASSERT_TRUE(t.switchScreenMode(Terminal::SwitchScreenMode::mode_1049, true));
    ASSERT_TRUE(ScreenSet::Key::alternate == t.screens.active_key);

    /* Screen should be empty */
    {
        EXPECT_STR("", t.plainString());
    }

    /* Print on alt screen. This should be off center because
     * we copy the cursor over from the primary screen */
    ASSERT_TRUE(t.printString("2B"));
    {
        EXPECT_STR("  2B", t.plainString());
    }

    /* Go back to primary */
    ASSERT_TRUE(t.switchScreenMode(Terminal::SwitchScreenMode::mode_1049, false));
    ASSERT_TRUE(ScreenSet::Key::primary == t.screens.active_key);

    /* Primary screen should still have the original content */
    {
        EXPECT_STR("1A", t.plainString());
    }

    /* Write, our cursor should be restored back. */
    ASSERT_TRUE(t.printString("C"));
    {
        EXPECT_STR("1AC", t.plainString());
    }

    /* Go back to alt screen with mode 1049 */
    ASSERT_TRUE(t.switchScreenMode(Terminal::SwitchScreenMode::mode_1049, true));
    ASSERT_TRUE(ScreenSet::Key::alternate == t.screens.active_key);

    /* Screen should be empty */
    {
        EXPECT_STR("", t.plainString());
    }
}

TEST(terminal, Terminal__deleteLines_wide_char_at_right_margin_with_full_clear) {
    TERM(t, 80, 24);

    /* Place a wide character at col 39 (1-indexed) on several rows.
     * The wide cell lands at col 38 (0-indexed) with spacer_tail at col 39. */
    t.setCursorPos(10, 39);
    /* '中' */
    ASSERT_TRUE(t.print(0x4E2D));

    /* Set left/right scroll margins so scrolling_region.right = 38.
     * clearCells will clear cells[4..39], which includes the wide cell
     * at col 38 but NOT the spacer_tail at col 39. */
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setLeftAndRightMargin(5, 39);

    /* scrollUp with count >= region height causes deleteLines to clear
     * ALL rows without any shifting, so rowWillBeShifted is never called
     * and the orphaned spacer_tail at col 39 triggers a page integrity
     * violation in clearCells. */
    ASSERT_TRUE(t.scrollUp(t.rows));
}

/* Wisp: upstream test "Terminal__glyph_APC_stores_session_glossary_entries" is gated on build_options.glyph_protocol, which this
 * build disables; not ported. */


TEST(terminal, Terminal__scroll_region_linefeed_recycled_row_has_default_metadata) {
    TERM(t, 5, 5);

    /* A soft-wrapped line across rows 0-2 so that row 1 has both wrap
     * flags set. Mark row 1 as a prompt as well (OSC 133 A would). */
    for (size_t i_ = (size_t)(0); i_ < (size_t)(12); i_++) ASSERT_TRUE(t.print('A'));
    t.screens.active->pages.getCell(Point::active(0, 1)).value.row->setSemanticPrompt(Row::SemanticPrompt::prompt);

    /* DECSTBM rows 2-4, cursor to the region bottom, and linefeed:
     * row 1 is discarded and its Row storage recycled as the new
     * blank region-bottom row. */
    t.setTopAndBottomMargin(2, 4);
    t.setCursorPos(4, 1);
    ASSERT_TRUE(t.linefeed());

    {
        const PageList::Cell rac = t.screens.active->pages.getCell(Point::active(0, 3)).value;
        ASSERT_TRUE(!rac.row->wrap());
        ASSERT_TRUE(!rac.row->wrap_continuation());
        ASSERT_TRUE(Row::SemanticPrompt::none == rac.row->semantic_prompt());
    }
}

TEST(terminal, Terminal__alt_screen_scroll_up_recycled_row_has_default_metadata) {
    TERM(t, 5, 3);

    ASSERT_TRUE(t.switchScreenMode(Terminal::SwitchScreenMode::mode_1049, true));

    /* A soft-wrapped line across rows 0-1 and a prompt mark on row 0. */
    for (size_t i_ = (size_t)(0); i_ < (size_t)(7); i_++) ASSERT_TRUE(t.print('A'));
    t.screens.active->pages.getCell(Point::active()).value.row->setSemanticPrompt(Row::SemanticPrompt::prompt);

    /* Scroll up: with no scrollback, row 0 is discarded and its Row
     * storage recycled as the new blank bottom row. */
    ASSERT_TRUE(t.scrollUp(1));

    {
        const PageList::Cell rac = t.screens.active->pages.getCell(Point::active(0, 2)).value;
        ASSERT_TRUE(!rac.row->wrap());
        ASSERT_TRUE(!rac.row->wrap_continuation());
        ASSERT_TRUE(Row::SemanticPrompt::none == rac.row->semantic_prompt());
    }
}

TEST(terminal, Terminal__insertLines_count_over_region_blanks_row_metadata) {
    TERM(t, 5, 5);

    /* A soft-wrapped line across rows 0-2 so that row 1 has both wrap
     * flags set, plus a prompt mark on row 1. */
    for (size_t i_ = (size_t)(0); i_ < (size_t)(12); i_++) ASSERT_TRUE(t.print('A'));
    t.screens.active->pages.getCell(Point::active(0, 1)).value.row->setSemanticPrompt(Row::SemanticPrompt::prompt);

    /* Insert more lines than remain in the region: every row from the
     * cursor to the region bottom is blanked in place, with no shifts. */
    t.setCursorPos(2, 1);
    t.insertLines(10);

    for (size_t y = (size_t)(1), n_y = (size_t)(5); y < n_y; y++) {
        const PageList::Cell rac = t.screens.active->pages.getCell(Point::active(0, (uint32_t)(y))).value;
        ASSERT_TRUE(!rac.row->wrap());
        ASSERT_TRUE(!rac.row->wrap_continuation());
        ASSERT_TRUE(Row::SemanticPrompt::none == rac.row->semantic_prompt());
    }
}

TEST(terminal, Terminal__deleteLines_count_over_region_blanks_row_metadata) {
    TERM(t, 5, 5);

    for (size_t i_ = (size_t)(0); i_ < (size_t)(12); i_++) ASSERT_TRUE(t.print('A'));
    t.screens.active->pages.getCell(Point::active(0, 1)).value.row->setSemanticPrompt(Row::SemanticPrompt::prompt);

    t.setCursorPos(2, 1);
    t.deleteLines(10);

    for (size_t y = (size_t)(1), n_y = (size_t)(5); y < n_y; y++) {
        const PageList::Cell rac = t.screens.active->pages.getCell(Point::active(0, (uint32_t)(y))).value;
        ASSERT_TRUE(!rac.row->wrap());
        ASSERT_TRUE(!rac.row->wrap_continuation());
        ASSERT_TRUE(Row::SemanticPrompt::none == rac.row->semantic_prompt());
    }
}

TEST(terminal, Terminal__deleteLines_blank_row_does_not_retain_semantic_prompt) {
    TERM(t, 5, 3);

    /* Mark row 0 as a prompt row, then delete it. The blank row that
     * appears at the region bottom reuses the deleted row's storage
     * and must not read as a prompt (e.g. for prompt navigation). */
    ASSERT_TRUE(t.print('$'));
    t.screens.active->pages.getCell(Point::active()).value.row->setSemanticPrompt(Row::SemanticPrompt::prompt);

    t.setCursorPos(1, 1);
    t.deleteLines(1);

    {
        const PageList::Cell rac = t.screens.active->pages.getCell(Point::active(0, 2)).value;
        ASSERT_TRUE(!rac.row->wrap());
        ASSERT_TRUE(!rac.row->wrap_continuation());
        ASSERT_TRUE(Row::SemanticPrompt::none == rac.row->semantic_prompt());
    }
}

TEST(terminal, Terminal__eraseDisplay_complete_ignores_stale_prompt_on_recycled_row) {
    TERM(t, 10, 3);

    /* Screen content that must NOT enter the scrollback on a clear. */
    ASSERT_TRUE(t.printString("hello"));

    /* Mark row 1 as a prompt row and then discard it with a region
     * scroll, recycling its storage as the blank bottom row. */
    t.screens.active->pages.getCell(Point::active(0, 1)).value.row->setSemanticPrompt(Row::SemanticPrompt::prompt);
    t.setTopAndBottomMargin(2, 3);
    t.setCursorPos(3, 1);
    ASSERT_TRUE(t.linefeed());
    t.setTopAndBottomMargin(0, 0);

    /* ED2: since no prompt is on screen, this must NOT take the
     * scroll-and-clear path that pushes content into scrollback. A
     * stale prompt flag on the recycled blank bottom row would. */
    t.eraseDisplay(terminal::csi::EraseDisplay::complete, false);

    ASSERT_TRUE(t.screens.active->pages.rows == t.screens.active->pages.total_rows);
}

/* @@TESTS@@ */

/* Wisp: std.testing.allocator's leak check. Runs last (registration order). */
TEST(terminal, zz_Wisp_no_leaks) { ASSERT_TRUE(zigstd::testing_state().live == 0); }
