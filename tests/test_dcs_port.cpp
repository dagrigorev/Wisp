/* Transliterated from the test blocks in Ghostty src/terminal/dcs.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Wisp: upstream's "tmux enter and implicit exit" is gated on
 * build_options.tmux_control_mode, which this build disables; not ported.
 */

#include <string.h>

#include <string>

#include "test_helpers.h"
#include "../terminal/dcs.hpp"

using namespace wisp;

namespace dcs = wisp::terminal::dcs;

typedef dcs::Handler Handler;
typedef dcs::Command Command;
typedef dcs::DCS DCS;
typedef dcs::StateKey StateKey;
typedef Command::DECRQSS DECRQSS;

static zigstd::Allocator talloc() { return zigstd::testing_allocator(); }

/* Wisp: `.{ .intermediates = "+", .final = 'q' }` */
static DCS dcsOf(const char *intermediates, uint8_t final_) {
    DCS d;
    d.intermediates = (const uint8_t *)intermediates;
    d.intermediates_len = intermediates != nullptr ? strlen(intermediates) : 0;
    d.params = nullptr;
    d.params_len = 0;
    d.final_ = final_;
    return d;
}

/* Wisp: `var h: Handler = .{}; defer h.deinit();` */
struct HandlerHolder {
    Handler h;
    ~HandlerHolder() { h.deinit(); }
};

/* Wisp: `for ("...") |byte| _ = h.put(byte);` */
static void putAll(Handler *h, const char *s) {
    Command unused;
    for (size_t i = 0; s[i] != 0; i++) (void)h->put((uint8_t)s[i], &unused);
}

/* Wisp: `cmd.xtgettcap.next().?` as a std::string. */
static bool nextKey(Command *cmd, std::string *out) {
    const uint8_t *p = nullptr;
    size_t n = 0;
    if (!cmd->xtgettcap.next(&p, &n)) return false;
    *out = std::string((const char *)p, n);
    return true;
}

TEST(dcs, unknown_DCS_command) {
    HandlerHolder hh;
    Handler &h = hh.h;
    Command cmd;
    ASSERT_TRUE(!h.hook(talloc(), dcsOf(nullptr, 'A'), &cmd));
    ASSERT_TRUE(h.state.key == StateKey::ignore);
    ASSERT_TRUE(!h.unhook(&cmd));
    ASSERT_TRUE(h.state.key == StateKey::inactive);
}

TEST(dcs, XTGETTCAP_command) {
    HandlerHolder hh;
    Handler &h = hh.h;
    Command cmd;
    ASSERT_TRUE(!h.hook(talloc(), dcsOf("+", 'q'), &cmd));
    putAll(&h, "536D756C78");
    ASSERT_TRUE(h.unhook(&cmd));
    ASSERT_TRUE(cmd.key == Command::Key::xtgettcap);
    std::string key;
    ASSERT_TRUE(nextKey(&cmd, &key) && key == "536D756C78");
    ASSERT_TRUE(!nextKey(&cmd, &key));
    cmd.deinit();
}

TEST(dcs, XTGETTCAP_mixed_case) {
    HandlerHolder hh;
    Handler &h = hh.h;
    Command cmd;
    ASSERT_TRUE(!h.hook(talloc(), dcsOf("+", 'q'), &cmd));
    putAll(&h, "536d756C78");
    ASSERT_TRUE(h.unhook(&cmd));
    ASSERT_TRUE(cmd.key == Command::Key::xtgettcap);
    std::string key;
    ASSERT_TRUE(nextKey(&cmd, &key) && key == "536D756C78");
    ASSERT_TRUE(!nextKey(&cmd, &key));
    cmd.deinit();
}

TEST(dcs, XTGETTCAP_command_multiple_keys) {
    HandlerHolder hh;
    Handler &h = hh.h;
    Command cmd;
    ASSERT_TRUE(!h.hook(talloc(), dcsOf("+", 'q'), &cmd));
    putAll(&h, "536D756C78;536D756C78");
    ASSERT_TRUE(h.unhook(&cmd));
    ASSERT_TRUE(cmd.key == Command::Key::xtgettcap);
    std::string key;
    ASSERT_TRUE(nextKey(&cmd, &key) && key == "536D756C78");
    ASSERT_TRUE(nextKey(&cmd, &key) && key == "536D756C78");
    ASSERT_TRUE(!nextKey(&cmd, &key));
    cmd.deinit();
}

TEST(dcs, XTGETTCAP_command_invalid_data) {
    HandlerHolder hh;
    Handler &h = hh.h;
    Command cmd;
    ASSERT_TRUE(!h.hook(talloc(), dcsOf("+", 'q'), &cmd));
    putAll(&h, "who;536D756C78");
    ASSERT_TRUE(h.unhook(&cmd));
    ASSERT_TRUE(cmd.key == Command::Key::xtgettcap);
    std::string key;
    ASSERT_TRUE(nextKey(&cmd, &key) && key == "WHO");
    ASSERT_TRUE(nextKey(&cmd, &key) && key == "536D756C78");
    ASSERT_TRUE(!nextKey(&cmd, &key));
    cmd.deinit();
}

TEST(dcs, DECRQSS_command) {
    HandlerHolder hh;
    Handler &h = hh.h;
    Command cmd;
    ASSERT_TRUE(!h.hook(talloc(), dcsOf("$", 'q'), &cmd));
    putAll(&h, "m");
    ASSERT_TRUE(h.unhook(&cmd));
    ASSERT_TRUE(cmd.key == Command::Key::decrqss);
    ASSERT_TRUE(cmd.decrqss == DECRQSS::sgr);
    cmd.deinit();
}

TEST(dcs, DECRQSS_invalid_command) {
    HandlerHolder hh;
    Handler &h = hh.h;
    Command cmd;
    ASSERT_TRUE(!h.hook(talloc(), dcsOf("$", 'q'), &cmd));
    putAll(&h, "z");
    ASSERT_TRUE(h.unhook(&cmd));
    ASSERT_TRUE(cmd.key == Command::Key::decrqss);
    ASSERT_TRUE(cmd.decrqss == DECRQSS::none);
    cmd.deinit();

    h.discard();

    ASSERT_TRUE(!h.hook(talloc(), dcsOf("$", 'q'), &cmd));
    putAll(&h, "\" q");
    ASSERT_TRUE(!h.unhook(&cmd));
}

/* Wisp: upstream encodes into a caller buffer; here the response is a
 * std::string, still asserted to fit max_response_bytes. */
static void expectResponse(vt::Terminal *term, DECRQSS request, const char *expected) {
    std::string encoded;
    Command::encodeDECRQSS(request, term, &encoded);
    ASSERT_TRUE(encoded.size() <= Command::max_response_bytes);
    ASSERT_STR_EQ(expected, encoded.c_str());
}

TEST(dcs, DECRQSS_response_encoding) {
    vt::Terminal::Options opts((vt::size::CellCountInt)80, (vt::size::CellCountInt)24);
    vt::Terminal t;
    ASSERT_TRUE(vt::Terminal::init(talloc(), opts, &t));

    expectResponse(&t, DECRQSS::none, "\x1BP0$r\x1B\\");
    expectResponse(&t, DECRQSS::sgr, "\x1BP1$r0m\x1B\\");

    ASSERT_TRUE(t.setAttribute(terminal::sgr::Attribute::make(terminal::sgr::Attribute::Tag::bold)) ==
                vt::PageList::IncreaseCapacityError::none);
    ASSERT_TRUE(t.setAttribute(terminal::sgr::Attribute::makeUnderline(
                    terminal::sgr::Attribute::Underline::curly)) ==
                vt::PageList::IncreaseCapacityError::none);
    expectResponse(&t, DECRQSS::sgr, "\x1BP1$r0;1;4:3m\x1B\\");

    t.setCursorStyle(terminal::ansi::CursorStyle::steady_underline);
    expectResponse(&t, DECRQSS::decscusr, "\x1BP1$r4 q\x1B\\");

    t.scrolling_region.top = 4;
    t.scrolling_region.bottom = 19;
    expectResponse(&t, DECRQSS::decstbm, "\x1BP1$r5;20r\x1B\\");

    expectResponse(&t, DECRQSS::decslrm, "\x1BP0$r\x1B\\");
    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.scrolling_region.left = 2;
    t.scrolling_region.right = 69;
    expectResponse(&t, DECRQSS::decslrm, "\x1BP1$r3;70s\x1B\\");

    t.deinit(talloc());
}

TEST(dcs, DECRQSS_largest_response_fits_fixed_buffer) {
    typedef terminal::sgr::Attribute A;
    vt::Terminal::Options opts((vt::size::CellCountInt)80, (vt::size::CellCountInt)24);
    vt::Terminal t;
    ASSERT_TRUE(vt::Terminal::init(talloc(), opts, &t));

    ASSERT_TRUE(t.setAttribute(A::make(A::Tag::bold)) == vt::PageList::IncreaseCapacityError::none);
    ASSERT_TRUE(t.setAttribute(A::make(A::Tag::faint)) == vt::PageList::IncreaseCapacityError::none);
    ASSERT_TRUE(t.setAttribute(A::make(A::Tag::italic)) == vt::PageList::IncreaseCapacityError::none);
    ASSERT_TRUE(t.setAttribute(A::makeUnderline(A::Underline::dashed)) ==
                vt::PageList::IncreaseCapacityError::none);
    ASSERT_TRUE(t.setAttribute(A::make(A::Tag::blink)) == vt::PageList::IncreaseCapacityError::none);
    ASSERT_TRUE(t.setAttribute(A::make(A::Tag::inverse)) == vt::PageList::IncreaseCapacityError::none);
    ASSERT_TRUE(t.setAttribute(A::make(A::Tag::invisible)) == vt::PageList::IncreaseCapacityError::none);
    ASSERT_TRUE(t.setAttribute(A::make(A::Tag::strikethrough)) == vt::PageList::IncreaseCapacityError::none);
    ASSERT_TRUE(t.setAttribute(A::make(A::Tag::overline)) == vt::PageList::IncreaseCapacityError::none);
    ASSERT_TRUE(t.setAttribute(A::makeRgb(A::Tag::direct_color_fg, 255, 255, 255)) ==
                vt::PageList::IncreaseCapacityError::none);
    ASSERT_TRUE(t.setAttribute(A::makeRgb(A::Tag::direct_color_bg, 255, 255, 255)) ==
                vt::PageList::IncreaseCapacityError::none);

    std::string encoded;
    Command::encodeDECRQSS(DECRQSS::sgr, &t, &encoded);

    const char *expected = "\x1BP1$r0;1;2;3;4:5;53;5;7;8;9"
                           ";38:2::255:255:255"
                           ";48:2::255:255:255m\x1B\\";
    ASSERT_STR_EQ(expected, encoded.c_str());
    ASSERT_TRUE(encoded.size() <= Command::max_response_bytes);

    t.deinit(talloc());
}

TEST(dcs, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
