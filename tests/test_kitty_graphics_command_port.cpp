/* Transliterated from the test blocks in Ghostty's
 * src/terminal/kitty/graphics_command.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names. All 38
 * upstream `test "..."` blocks are ported; none are omitted. The upstream
 * name is quoted above each test, since the test macro needs an
 * identifier.
 */

#include <stdio.h>
#include <string.h>

#include <string>

#include "test_helpers.h"
#include "../terminal/kitty/graphics_command.hpp"

using namespace wisp;

namespace graphics = wisp::terminal::kitty::graphics;

typedef graphics::Error Error;
typedef graphics::Parser Parser;
typedef graphics::Command Command;
typedef graphics::Transmission Transmission;
typedef graphics::Display Display;
typedef graphics::Response Response;
typedef graphics::AnimationControl AnimationControl;
typedef graphics::CompositionMode CompositionMode;

/* Wisp: PORTING_CONVENTIONS.md — ASSERT_STR_EQ expands to several
 * statements, so a temporary std::string argument would be destroyed
 * before the comparison. Bind both sides through a named local. */
#define EXPECT_STR(expected, actual)                                                                           \
    do {                                                                                                       \
        const std::string _actual = (actual);                                                                  \
        ASSERT_STR_EQ(expected, _actual.c_str());                                                              \
    } while (0)

static zigstd::Allocator talloc() { return zigstd::testing_allocator(); }

/* Wisp: `defer p.deinit()`. */
struct ParserHolder {
    Parser p;
    explicit ParserHolder(size_t max_bytes) : p(Parser::init(talloc(), max_bytes)) {}
    ~ParserHolder() { p.deinit(); }
};

/* Wisp: `defer command.deinit(alloc)`. */
struct CommandHolder {
    Command c;
    ~CommandHolder() { c.deinit(talloc()); }
};

/* Wisp: `for (input) |c| try p.feed(c)`; the Zig string literal's NUL
 * sentinel is not part of the iterated slice. */
static Error feedAll(Parser *p, const char *input) {
    for (size_t i = 0; input[i] != 0; i++) {
        const Error err = p->feed((uint8_t)input[i]);
        if (err != Error::none) return err;
    }
    return Error::none;
}

static Error feedSlice(Parser *p, const char *input) {
    return p->feedSlice((const uint8_t *)input, strlen(input));
}

static Error parseString(const char *input, Command *out) {
    return Parser::parseString(talloc(), (const uint8_t *)input, strlen(input), out);
}

/* Wisp: `expectEqualStrings(expected, command.data)`; command.data is a
 * pointer and a length, not NUL-terminated. */
static bool dataEql(const Command &c, const char *expected) {
    const size_t n = strlen(expected);
    return n == c.data_len && (n == 0 || memcmp(c.data, expected, n) == 0);
}

static bool dataEql(const Command &a, const Command &b) {
    return a.data_len == b.data_len && (a.data_len == 0 || memcmp(a.data, b.data, a.data_len) == 0);
}

/* test "transmission command" */
TEST(kitty_graphics_command, transmission_command) {
    ParserHolder ph(1024 * 1024);
    Parser &p = ph.p;

    const char *input = "f=24,s=10,v=20";
    ASSERT_TRUE(feedAll(&p, input) == Error::none);
    CommandHolder ch;
    ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::transmit);
    const Transmission v = command.control.transmit;
    ASSERT_TRUE(Transmission::Format::rgb == v.format);
    ASSERT_EQ(10, v.width);
    ASSERT_EQ(20, v.height);
    ASSERT_TRUE(false == v.usage.transient);
}

/* test "transmission command with transient hint" */
TEST(kitty_graphics_command, transmission_command_with_transient_hint) {
    ParserHolder ph(1024 * 1024);
    Parser &p = ph.p;

    const char *input = "f=24,s=10,v=20,N=1";
    ASSERT_TRUE(feedAll(&p, input) == Error::none);
    CommandHolder ch;
    ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::transmit);
    const Transmission v = command.control.transmit;
    ASSERT_TRUE(Transmission::Format::rgb == v.format);
    ASSERT_EQ(10, v.width);
    ASSERT_EQ(20, v.height);
    ASSERT_TRUE(true == v.usage.transient);
}

/* test "feedSlice matches per-byte feed" */
TEST(kitty_graphics_command, feedSlice_matches_per_byte_feed) {
    const char *input = "f=24,s=10,v=20;aGVsbG8gd29ybGQ=";

    ParserHolder ph1(1024 * 1024);
    Parser &p1 = ph1.p;
    ASSERT_TRUE(feedAll(&p1, input) == Error::none);
    CommandHolder ch1;
    ASSERT_TRUE(p1.complete(talloc(), &ch1.c) == Error::none);

    ParserHolder ph2(1024 * 1024);
    Parser &p2 = ph2.p;
    ASSERT_TRUE(feedSlice(&p2, input) == Error::none);
    CommandHolder ch2;
    ASSERT_TRUE(p2.complete(talloc(), &ch2.c) == Error::none);

    ASSERT_TRUE(ch1.c.control.tag == Command::Action::transmit);
    ASSERT_TRUE(ch2.c.control.tag == Command::Action::transmit);
    ASSERT_TRUE(dataEql(ch1.c, ch2.c));
}

/* test "feedSlice across slice boundaries" */
TEST(kitty_graphics_command, feedSlice_across_slice_boundaries) {
    ParserHolder ph(1024 * 1024);
    Parser &p = ph.p;

    ASSERT_TRUE(feedSlice(&p, "f=24,s=10") == Error::none);
    ASSERT_TRUE(feedSlice(&p, ",v=20;aGVsbG8g") == Error::none);
    ASSERT_TRUE(feedSlice(&p, "d29ybGQ=") == Error::none);
    CommandHolder ch;
    ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::transmit);

    /* The payload is base64-decoded on completion. */
    ASSERT_TRUE(dataEql(command, "hello world"));
}

/* test "feedSlice respects max_bytes" */
TEST(kitty_graphics_command, feedSlice_respects_max_bytes) {
    ParserHolder ph(4);
    Parser &p = ph.p;

    ASSERT_TRUE(feedSlice(&p, "f=24;ab") == Error::none);
    ASSERT_TRUE(feedSlice(&p, "cde") == Error::OutOfMemory);
}

/* test "transmission ignores 'm' if medium is not direct" */
TEST(kitty_graphics_command, transmission_ignores_m_if_medium_is_not_direct) {
    ParserHolder ph(1024 * 1024);
    Parser &p = ph.p;

    const char *input = "a=t,t=t,m=1";
    ASSERT_TRUE(feedAll(&p, input) == Error::none);
    CommandHolder ch;
    ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::transmit);
    const Transmission v = command.control.transmit;
    ASSERT_TRUE(Transmission::Medium::temporary_file == v.medium);
    ASSERT_TRUE(!v.more_chunks);
}

/* test "transmission respects 'm' if medium is direct" */
TEST(kitty_graphics_command, transmission_respects_m_if_medium_is_direct) {
    ParserHolder ph(1024 * 1024);
    Parser &p = ph.p;

    const char *input = "a=t,t=d,m=1";
    ASSERT_TRUE(feedAll(&p, input) == Error::none);
    CommandHolder ch;
    ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::transmit);
    const Transmission v = command.control.transmit;
    ASSERT_TRUE(Transmission::Medium::direct == v.medium);
    ASSERT_TRUE(v.more_chunks);
}

/* test "query command" */
TEST(kitty_graphics_command, query_command) {
    ParserHolder ph(1024 * 1024);
    Parser &p = ph.p;

    const char *input = "i=31,s=1,v=1,a=q,t=d,f=24;QUFBQQ";
    ASSERT_TRUE(feedAll(&p, input) == Error::none);
    CommandHolder ch;
    ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::query);
    const Transmission v = command.control.query;
    ASSERT_TRUE(Transmission::Medium::direct == v.medium);
    ASSERT_EQ(1, v.width);
    ASSERT_EQ(1, v.height);
    ASSERT_EQ(31, v.image_id);
    ASSERT_TRUE(dataEql(command, "AAAA"));
}

/* test "display command" */
TEST(kitty_graphics_command, display_command) {
    ParserHolder ph(1024 * 1024);
    Parser &p = ph.p;

    const char *input = "a=p,U=1,i=31,c=80,r=120";
    ASSERT_TRUE(feedAll(&p, input) == Error::none);
    CommandHolder ch;
    ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::display);
    const Display v = command.control.display;
    ASSERT_EQ(80, v.columns);
    ASSERT_EQ(120, v.rows);
    ASSERT_EQ(31, v.image_id);
}

/* test "delete command" */
TEST(kitty_graphics_command, delete_command) {
    ParserHolder ph(1024 * 1024);
    Parser &p = ph.p;

    const char *input = "a=d,d=p,x=3,y=4";
    ASSERT_TRUE(feedAll(&p, input) == Error::none);
    CommandHolder ch;
    ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::delete_);
    const graphics::Delete::Action v = command.control.delete_.action;
    ASSERT_TRUE(v.tag == graphics::Delete::Action::Tag::intersect_cell);
    const graphics::Delete::Action::IntersectCell dv = v.intersect_cell;
    ASSERT_TRUE(!dv.delete_);
    ASSERT_EQ(3, dv.x);
    ASSERT_EQ(4, dv.y);
}

/* test "no control data" */
TEST(kitty_graphics_command, no_control_data) {
    ParserHolder ph(1024 * 1024);
    Parser &p = ph.p;

    const char *input = ";QUFBQQ";
    ASSERT_TRUE(feedAll(&p, input) == Error::none);
    CommandHolder ch;
    ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::transmit);
    ASSERT_TRUE(dataEql(command, "AAAA"));
}

/* test "ignore unknown keys (long)" */
TEST(kitty_graphics_command, ignore_unknown_keys_long) {
    ParserHolder ph(1024 * 1024);
    Parser &p = ph.p;

    const char *input = "f=24,s=10,v=20,hello=world";
    ASSERT_TRUE(feedAll(&p, input) == Error::none);
    CommandHolder ch;
    ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::transmit);
    const Transmission v = command.control.transmit;
    ASSERT_TRUE(Transmission::Format::rgb == v.format);
    ASSERT_EQ(10, v.width);
    ASSERT_EQ(20, v.height);
}

/* test "ignore unknown keys (non-letter)" */
TEST(kitty_graphics_command, ignore_unknown_keys_non_letter) {
    ParserHolder ph(1024 * 1024);
    Parser &p = ph.p;

    const char *input = "f=24,s=10,v=20,!=1";
    ASSERT_TRUE(feedAll(&p, input) == Error::none);
    CommandHolder ch;
    ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::transmit);
    const Transmission v = command.control.transmit;
    ASSERT_TRUE(Transmission::Format::rgb == v.format);
    ASSERT_EQ(10, v.width);
    ASSERT_EQ(20, v.height);
}

/* test "ignore very long values" */
TEST(kitty_graphics_command, ignore_very_long_values) {
    ParserHolder ph(1024 * 1024);
    Parser &p = ph.p;

    const char *input = "f=24,s=10,v=2000000000000000000000000000000000000000";
    ASSERT_TRUE(feedAll(&p, input) == Error::none);
    CommandHolder ch;
    ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::transmit);
    const Transmission v = command.control.transmit;
    ASSERT_TRUE(Transmission::Format::rgb == v.format);
    ASSERT_EQ(10, v.width);
    ASSERT_EQ(0, v.height);
}

/* test "ensure very large negative values don't get skipped" */
TEST(kitty_graphics_command, ensure_very_large_negative_values_dont_get_skipped) {
    ParserHolder ph(1024 * 1024);
    Parser &p = ph.p;

    const char *input = "a=p,i=1,z=-2000000000";
    ASSERT_TRUE(feedAll(&p, input) == Error::none);
    CommandHolder ch;
    ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::display);
    const Display v = command.control.display;
    ASSERT_EQ(1, v.image_id);
    ASSERT_EQ(-2000000000, v.z);
}

/* test "ensure proper overflow error for u32" */
TEST(kitty_graphics_command, ensure_proper_overflow_error_for_u32) {
    ParserHolder ph(1024 * 1024);
    Parser &p = ph.p;

    const char *input = "a=p,i=10000000000";
    ASSERT_TRUE(feedAll(&p, input) == Error::none);
    Command command;
    ASSERT_TRUE(p.complete(talloc(), &command) == Error::Overflow);
}

/* test "ensure proper overflow error for i32" */
TEST(kitty_graphics_command, ensure_proper_overflow_error_for_i32) {
    ParserHolder ph(1024 * 1024);
    Parser &p = ph.p;

    const char *input = "a=p,i=1,z=-9999999999";
    ASSERT_TRUE(feedAll(&p, input) == Error::none);
    Command command;
    ASSERT_TRUE(p.complete(talloc(), &command) == Error::Overflow);
}

/* test "all i32 values" */
TEST(kitty_graphics_command, all_i32_values) {
    {
        /* 'z' (usually z-axis values) */
        ParserHolder ph(1024 * 1024);
        Parser &p = ph.p;
        const char *input = "a=p,i=1,z=-1";
        ASSERT_TRUE(feedAll(&p, input) == Error::none);
        CommandHolder ch;
        ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
        const Command &command = ch.c;

        ASSERT_TRUE(command.control.tag == Command::Action::display);
        const Display v = command.control.display;
        ASSERT_EQ(1, v.image_id);
        ASSERT_EQ(-1, v.z);
    }

    {
        /* 'H' (relative placement, horizontal offset) */
        ParserHolder ph(1024 * 1024);
        Parser &p = ph.p;
        const char *input = "a=p,i=1,H=-1";
        ASSERT_TRUE(feedAll(&p, input) == Error::none);
        CommandHolder ch;
        ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
        const Command &command = ch.c;

        ASSERT_TRUE(command.control.tag == Command::Action::display);
        const Display v = command.control.display;
        ASSERT_EQ(1, v.image_id);
        ASSERT_EQ(-1, v.horizontal_offset);
    }

    {
        /* 'V' (relative placement, vertical offset) */
        ParserHolder ph(1024 * 1024);
        Parser &p = ph.p;
        const char *input = "a=p,i=1,V=-1";
        ASSERT_TRUE(feedAll(&p, input) == Error::none);
        CommandHolder ch;
        ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
        const Command &command = ch.c;

        ASSERT_TRUE(command.control.tag == Command::Action::display);
        const Display v = command.control.display;
        ASSERT_EQ(1, v.image_id);
        ASSERT_EQ(-1, v.vertical_offset);
    }
}

/* test "response: encode nothing without ID or image number" */
TEST(kitty_graphics_command, response_encode_nothing_without_ID_or_image_number) {
    /* Wisp: `std.Io.Writer.fixed(&buf)` with a 1024 byte buffer; the port
     * writes into a std::string, which cannot overflow, and no upstream
     * assertion here depends on the capacity. */
    std::string writer;

    Response r;
    r.encode(&writer);
    EXPECT_STR("", writer);
}

/* test "response: encode with only image id" */
TEST(kitty_graphics_command, response_encode_with_only_image_id) {
    std::string writer;

    Response r;
    r.id = 4;
    r.encode(&writer);
    /* 11 bytes: ESC _ G i = 4 ; O K ESC backslash */
    EXPECT_STR("\x1b" "_Gi=4;OK" "\x1b" "\\", writer);
}

/* test "response: encode with only image number" */
TEST(kitty_graphics_command, response_encode_with_only_image_number) {
    std::string writer;

    Response r;
    r.image_number = 4;
    r.encode(&writer);
    EXPECT_STR("\x1b" "_GI=4;OK" "\x1b" "\\", writer);
}

/* test "response: encode with image ID and number" */
TEST(kitty_graphics_command, response_encode_with_image_ID_and_number) {
    std::string writer;

    Response r;
    r.id = 12;
    r.image_number = 4;
    r.encode(&writer);
    /* 16 bytes: ESC _ G i = 1 2 , I = 4 ; O K ESC backslash */
    EXPECT_STR("\x1b" "_Gi=12,I=4;OK" "\x1b" "\\", writer);
}

/* test "delete range command 1" */
TEST(kitty_graphics_command, delete_range_command_1) {
    ParserHolder ph(1024 * 1024);
    Parser &p = ph.p;

    const char *input = "a=d,d=r,x=3,y=4";
    ASSERT_TRUE(feedAll(&p, input) == Error::none);
    CommandHolder ch;
    ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::delete_);
    const graphics::Delete::Action v = command.control.delete_.action;
    ASSERT_TRUE(v.tag == graphics::Delete::Action::Tag::range);
    const graphics::Delete::Action::Range range = v.range;
    ASSERT_TRUE(!range.delete_);
    ASSERT_EQ(3, range.first);
    ASSERT_EQ(4, range.last);
}

/* test "delete range command 2" */
TEST(kitty_graphics_command, delete_range_command_2) {
    ParserHolder ph(1024 * 1024);
    Parser &p = ph.p;

    const char *input = "a=d,d=R,x=5,y=11";
    ASSERT_TRUE(feedAll(&p, input) == Error::none);
    CommandHolder ch;
    ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::delete_);
    const graphics::Delete::Action v = command.control.delete_.action;
    ASSERT_TRUE(v.tag == graphics::Delete::Action::Tag::range);
    const graphics::Delete::Action::Range range = v.range;
    ASSERT_TRUE(range.delete_);
    ASSERT_EQ(5, range.first);
    ASSERT_EQ(11, range.last);
}

/* test "delete range command 3" */
TEST(kitty_graphics_command, delete_range_command_3) {
    /* An inverted range is accepted; it simply matches no images. This
     * matches kitty, which does no validation and filters with
     * `x <= image_id <= y`. */
    ParserHolder ph(1024 * 1024);
    Parser &p = ph.p;

    const char *input = "a=d,d=R,x=5,y=4";
    ASSERT_TRUE(feedAll(&p, input) == Error::none);
    CommandHolder ch;
    ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::delete_);
    const graphics::Delete::Action v = command.control.delete_.action;
    ASSERT_TRUE(v.tag == graphics::Delete::Action::Tag::range);
    const graphics::Delete::Action::Range range = v.range;
    ASSERT_TRUE(range.delete_);
    ASSERT_EQ(5, range.first);
    ASSERT_EQ(4, range.last);
}

/* test "delete range command 4" */
TEST(kitty_graphics_command, delete_range_command_4) {
    /* An omitted upper bound defaults to zero, which matches no images
     * since image IDs are always non-zero. */
    ParserHolder ph(1024 * 1024);
    Parser &p = ph.p;

    const char *input = "a=d,d=R,x=5";
    ASSERT_TRUE(feedAll(&p, input) == Error::none);
    CommandHolder ch;
    ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::delete_);
    const graphics::Delete::Action v = command.control.delete_.action;
    ASSERT_TRUE(v.tag == graphics::Delete::Action::Tag::range);
    const graphics::Delete::Action::Range range = v.range;
    ASSERT_TRUE(range.delete_);
    ASSERT_EQ(5, range.first);
    ASSERT_EQ(0, range.last);
}

/* test "delete range command 5" */
TEST(kitty_graphics_command, delete_range_command_5) {
    ParserHolder ph(1024 * 1024);
    Parser &p = ph.p;

    const char *input = "a=d,d=R,y=5";
    ASSERT_TRUE(feedAll(&p, input) == Error::none);
    CommandHolder ch;
    ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::delete_);
    const graphics::Delete::Action v = command.control.delete_.action;
    ASSERT_TRUE(v.tag == graphics::Delete::Action::Tag::range);
    const graphics::Delete::Action::Range range = v.range;
    ASSERT_TRUE(range.delete_);
    ASSERT_EQ(0, range.first);
    ASSERT_EQ(5, range.last);
}

/* test "delete range command 6" */
TEST(kitty_graphics_command, delete_range_command_6) {
    /* Both bounds omitted defaults to the empty range [0, 0]. */
    ParserHolder ph(1024 * 1024);
    Parser &p = ph.p;

    const char *input = "a=d,d=r";
    ASSERT_TRUE(feedAll(&p, input) == Error::none);
    CommandHolder ch;
    ASSERT_TRUE(p.complete(talloc(), &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::delete_);
    const graphics::Delete::Action v = command.control.delete_.action;
    ASSERT_TRUE(v.tag == graphics::Delete::Action::Tag::range);
    const graphics::Delete::Action::Range range = v.range;
    ASSERT_TRUE(!range.delete_);
    ASSERT_EQ(0, range.first);
    ASSERT_EQ(0, range.last);
}

/* Kitty range-checks non-flag keys while handling the command, not while
 * parsing, so a bad value never drops the command (which would make a
 * response impossible even when the client gave an id). The flag keys
 * "a", "d", "t" and "o" are the exception. */

/* test "unknown format value is deferred to execution" */
TEST(kitty_graphics_command, unknown_format_value_is_deferred_to_execution) {
    CommandHolder ch;
    ASSERT_TRUE(parseString("a=t,f=42,i=31,s=1,v=1", &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::transmit);
    ASSERT_TRUE(command.control.transmit.format_unknown);
    ASSERT_EQ(31, command.control.transmit.image_id);
}

/* test "zero format value is rgba" */
TEST(kitty_graphics_command, zero_format_value_is_rgba) {
    CommandHolder ch;
    ASSERT_TRUE(parseString("a=t,f=0,i=31,s=1,v=1", &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::transmit);
    const Transmission v = command.control.transmit;
    ASSERT_TRUE(!v.format_unknown);
    ASSERT_TRUE(Transmission::Format::rgba == v.format);
}

/* test "known format values are not unknown" */
TEST(kitty_graphics_command, known_format_values_are_not_unknown) {
    struct Entry {
        const char *input;
        Transmission::Format format;
    };
    const Entry entries[] = {
        {"f=24", Transmission::Format::rgb},
        {"f=32", Transmission::Format::rgba},
        {"f=100", Transmission::Format::png},
    };

    for (size_t i = 0; i < sizeof entries / sizeof entries[0]; i++) {
        const Entry &entry = entries[i];
        CommandHolder ch;
        ASSERT_TRUE(parseString(entry.input, &ch.c) == Error::none);

        const Transmission v = ch.c.control.transmit;
        ASSERT_TRUE(!v.format_unknown);
        ASSERT_TRUE(entry.format == v.format);
    }
}

/* test "quiet value above two suppresses all responses" */
TEST(kitty_graphics_command, quiet_value_above_two_suppresses_all_responses) {
    CommandHolder ch;
    ASSERT_TRUE(parseString("a=t,q=3,i=31,s=1,v=1", &ch.c) == Error::none);

    ASSERT_TRUE(Command::Quiet::failures == ch.c.quiet);
}

/* test "cursor movement value above one moves the cursor" */
TEST(kitty_graphics_command, cursor_movement_value_above_one_moves_the_cursor) {
    CommandHolder ch;
    ASSERT_TRUE(parseString("a=p,i=31,C=2", &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::display);
    ASSERT_TRUE(Display::CursorMovement::after == command.control.display.cursor_movement);
}

/* test "virtual placement value above one is virtual" */
TEST(kitty_graphics_command, virtual_placement_value_above_one_is_virtual) {
    CommandHolder ch;
    ASSERT_TRUE(parseString("a=p,i=31,U=2", &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::display);
    ASSERT_TRUE(command.control.display.virtual_placement);
}

/* test "animation frame composition value above one alpha blends" */
TEST(kitty_graphics_command, animation_frame_composition_value_above_one_alpha_blends) {
    CommandHolder ch;
    ASSERT_TRUE(parseString("a=f,i=31,X=2", &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::transmit_animation_frame);
    ASSERT_TRUE(CompositionMode::alpha_blend == command.control.transmit_animation_frame.composition_mode);
}

/* test "animation compose value above one overwrites" */
TEST(kitty_graphics_command, animation_compose_value_above_one_overwrites) {
    CommandHolder ch;
    ASSERT_TRUE(parseString("a=c,i=31,C=2", &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::compose_animation);
    ASSERT_TRUE(CompositionMode::overwrite == command.control.compose_animation.composition_mode);
}

/* test "animation control action above three is ignored" */
TEST(kitty_graphics_command, animation_control_action_above_three_is_ignored) {
    CommandHolder ch;
    ASSERT_TRUE(parseString("a=a,i=31,s=4", &ch.c) == Error::none);
    const Command &command = ch.c;

    ASSERT_TRUE(command.control.tag == Command::Action::control_animation);
    ASSERT_TRUE(AnimationControl::AnimationAction::invalid == command.control.control_animation.action);
}

/* test "unknown flag key values still fail parsing" */
TEST(kitty_graphics_command, unknown_flag_key_values_still_fail_parsing) {
    /* Kitty rejects these while parsing too. See gen/apc_parsers.py. */
    const char *inputs[] = {
        "a=z,i=31",      /* action */
        "a=d,d=w,i=31",  /* delete action */
        "a=t,t=q,i=31",  /* transmission medium */
        "a=t,o=q,i=31",  /* compression */
    };

    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
        Command command;
        ASSERT_TRUE(parseString(inputs[i], &command) == Error::InvalidFormat);
    }
}

TEST(kitty_graphics_command, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
