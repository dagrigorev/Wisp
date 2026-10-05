/* Transliterated from the test blocks in Ghostty src/terminal/osc.zig,
 * src/terminal/osc/encoding.zig and, under src/terminal/osc/parsers/:
 * change_window_title.zig, change_window_icon.zig, hyperlink.zig,
 * report_pwd.zig, mouse_shape.zig and clipboard_operation.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * These are upstream's tests: same inputs, same assertions, same names.
 *
 * Mapping from Zig, beyond test_parser_port.cpp's:
 *   Parser.init(null)             Parser p;            (no allocator)
 *   Parser.init(testing.allocator) Parser p(true);
 *   p.end(null)                   p.end()
 *   p.end('\x1b')                 p.end('\x1b')
 *   cmd == .change_window_title   cmd->key == Key::change_window_title
 *   cap.writer.buffer.len         cap.writer.capacity
 *   cap.trailing().len            cap.trailing_len()
 *
 */

#include "test_helpers.h"
#include "osc.hpp"

#include <string>

using namespace wisp::terminal::osc;

typedef Command::Key Key;

static void feed(Parser &p, const char *s) {
    for (const char *c = s; *c; c++) p.next((uint8_t)*c);
}

/* ─── osc.zig ────────────────────────────────────────────────────────────── */

TEST(osc, Parser_allocating_captures_have_a_hard_limit) {
    const char *prefixes[] = {"52;", "66;", "72;", "99;", "5522;"};
    const size_t limit = Parser::MAX_BUF + 1;

    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        Parser p(true);
        p.max_allocating_bytes = limit;

        feed(p, prefixes[i]);
        for (size_t k = 0; k < limit; k++) p.next('a');

        Parser::Capture &cap = p.capture;
        ASSERT_TRUE(p.has_capture);
        ASSERT_EQ(cap.trailing_len(), limit);
        ASSERT_EQ(cap.writer.capacity, limit);

        p.next('a');
        ASSERT_TRUE(p.state == Parser::State::invalid);
        ASSERT_EQ(cap.trailing_len(), limit);
        ASSERT_EQ(cap.writer.capacity, limit);
    }
}

TEST(osc, Parser_nextSlice_allocating_captures_have_a_hard_limit) {
    const size_t limit = Parser::MAX_BUF + 1;

    Parser p(true);
    p.max_allocating_bytes = limit;

    char *data = (char *)malloc(limit);
    ASSERT_TRUE(data != nullptr);
    memset(data, 'a', limit);

    /* Exactly at the limit stays valid and bounded. */
    p.nextSlice("52;");
    p.nextSlice((const uint8_t *)data, limit);
    Parser::Capture &cap = p.capture;
    ASSERT_TRUE(p.state != Parser::State::invalid);
    ASSERT_EQ(cap.trailing_len(), limit);
    ASSERT_EQ(cap.writer.capacity, limit);

    /* One more byte overflows: the state becomes invalid and the
     * retained bytes and allocation stay bounded. */
    p.nextSlice("a");
    ASSERT_TRUE(p.state == Parser::State::invalid);
    ASSERT_EQ(cap.trailing_len(), limit);
    ASSERT_EQ(cap.writer.capacity, limit);
    ASSERT_TRUE(p.end() == nullptr);

    free(data);
}

TEST(osc, Parser_nextSlice_overflowing_slice_is_truncated_at_the_limit) {
    Parser p(true);
    p.max_allocating_bytes = 4;

    p.nextSlice("52;abcdef");
    ASSERT_TRUE(p.state == Parser::State::invalid);
    ASSERT_TRUE(p.end() == nullptr);

    Parser::Capture &cap = p.capture;
    ASSERT_EQ(cap.trailing_len(), 4u);
    ASSERT_TRUE(memcmp(cap.trailing(), "abcd", 4) == 0);
    ASSERT_EQ(cap.writer.capacity, 4u);
}

/* ─── encoding.zig ───────────────────────────────────────────────────────── */

TEST(encoding, isSafeUtf8) {
    ASSERT_TRUE(isSafeUtf8("Hello world!"));
    ASSERT_TRUE(isSafeUtf8("\xe5\xae\x89\xe5\x85\xa8\xe7\x9a\x84\xe3\x83\xa6"
                           "\xe3\x83\x8b\xe3\x82\xb3\xe3\x83\xbc\xe3\x83\x89"
                           "\xe2\x98\x80\xef\xb8\x8f"));
    ASSERT_FALSE(isSafeUtf8("No linebreaks\nallowed"));
    ASSERT_FALSE(isSafeUtf8("\x07no bells"));
    ASSERT_FALSE(isSafeUtf8("\x1b]9;no OSCs\x1b\\\x1b[m"));
    ASSERT_FALSE(isSafeUtf8("\x9f" "8-bit escapes are clever, but no"));
}

/* ─── parsers/change_window_title.zig ────────────────────────────────────── */

TEST(change_window_title, OSC_0_change_window_title) {
    Parser p;
    p.next('0');
    p.next(';');
    p.next('a');
    p.next('b');
    const Command *cmd = p.end();
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Key::change_window_title);
    ASSERT_TRUE(cmd->change_window_title.eql("ab"));
}

TEST(change_window_title, OSC_0_longer_than_buffer) {
    Parser p;

    p.next('0');
    p.next(';');
    for (size_t i = 0; i < Parser::MAX_BUF + 2; i++) p.next('a');

    ASSERT_TRUE(p.end() == nullptr);
}

TEST(change_window_title, OSC_0_one_shorter_than_buffer_length) {
    Parser p;

    p.next('0');
    p.next(';');
    for (size_t i = 0; i < Parser::MAX_BUF - 1; i++) p.next('a');

    const Command *cmd = p.end();
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Key::change_window_title);
    ASSERT_EQ(cmd->change_window_title.len, Parser::MAX_BUF - 1);
    for (size_t i = 0; i < cmd->change_window_title.len; i++) {
        ASSERT_EQ(cmd->change_window_title.ptr[i], 'a');
    }
}

TEST(change_window_title, OSC_0_exactly_at_buffer_length) {
    Parser p;

    p.next('0');
    p.next(';');
    for (size_t i = 0; i < Parser::MAX_BUF; i++) p.next('a');

    /* This should be null because we always reserve space for a null
     * terminator. */
    ASSERT_TRUE(p.end() == nullptr);
}

TEST(change_window_title, OSC_2_change_window_title_with_2) {
    Parser p;
    p.next('2');
    p.next(';');
    p.next('a');
    p.next('b');
    const Command *cmd = p.end();
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Key::change_window_title);
    ASSERT_TRUE(cmd->change_window_title.eql("ab"));
}

TEST(change_window_title, OSC_2_change_window_title_with_utf8) {
    Parser p;
    p.next('2');
    p.next(';');
    /* '—' EM DASH U+2014 (E2 80 94) */
    p.next(0xE2);
    p.next(0x80);
    p.next(0x94);

    p.next(' ');
    /* '‐' HYPHEN U+2010 (E2 80 90)
     * Intententionally chosen to conflict with the 0x90 C1 control */
    p.next(0xE2);
    p.next(0x80);
    p.next(0x90);
    const Command *cmd = p.end();
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Key::change_window_title);
    ASSERT_TRUE(cmd->change_window_title.eql("\xe2\x80\x94\x20\xe2\x80\x90"));
}

TEST(change_window_title, OSC_2_change_window_title_empty) {
    Parser p;
    p.next('2');
    p.next(';');
    const Command *cmd = p.end();
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Key::change_window_title);
    ASSERT_TRUE(cmd->change_window_title.eql(""));
}

/* ─── parsers/change_window_icon.zig ─────────────────────────────────────── */

TEST(change_window_icon, OSC_1_change_window_icon) {
    Parser p;
    p.next('1');
    p.next(';');
    p.next('a');
    p.next('b');
    const Command *cmd = p.end();
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Key::change_window_icon);
    ASSERT_TRUE(cmd->change_window_icon.eql("ab"));
}

/* ─── parsers/hyperlink.zig ──────────────────────────────────────────────── */

TEST(hyperlink, OSC_8_hyperlink) {
    Parser p;
    feed(p, "8;;http://example.com");

    const Command *cmd = p.end('\x1b');
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Key::hyperlink_start);
    ASSERT_TRUE(cmd->hyperlink_start.uri.eql("http://example.com"));
}

TEST(hyperlink, OSC_8_hyperlink_with_id_set) {
    Parser p;
    feed(p, "8;id=foo;http://example.com");

    const Command *cmd = p.end('\x1b');
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Key::hyperlink_start);
    ASSERT_TRUE(cmd->hyperlink_start.has_id);
    ASSERT_TRUE(cmd->hyperlink_start.id.eql("foo"));
    ASSERT_TRUE(cmd->hyperlink_start.uri.eql("http://example.com"));
}

TEST(hyperlink, OSC_8_hyperlink_with_empty_id) {
    Parser p;
    feed(p, "8;id=;http://example.com");

    const Command *cmd = p.end('\x1b');
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Key::hyperlink_start);
    ASSERT_FALSE(cmd->hyperlink_start.has_id);
    ASSERT_TRUE(cmd->hyperlink_start.uri.eql("http://example.com"));
}

TEST(hyperlink, OSC_8_hyperlink_with_incomplete_key) {
    Parser p;
    feed(p, "8;id;http://example.com");

    const Command *cmd = p.end('\x1b');
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Key::hyperlink_start);
    ASSERT_FALSE(cmd->hyperlink_start.has_id);
    ASSERT_TRUE(cmd->hyperlink_start.uri.eql("http://example.com"));
}

TEST(hyperlink, OSC_8_hyperlink_with_empty_key) {
    Parser p;
    feed(p, "8;=value;http://example.com");

    const Command *cmd = p.end('\x1b');
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Key::hyperlink_start);
    ASSERT_FALSE(cmd->hyperlink_start.has_id);
    ASSERT_TRUE(cmd->hyperlink_start.uri.eql("http://example.com"));
}

TEST(hyperlink, OSC_8_hyperlink_with_empty_key_and_id) {
    Parser p;
    feed(p, "8;=value:id=foo;http://example.com");

    const Command *cmd = p.end('\x1b');
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Key::hyperlink_start);
    ASSERT_TRUE(cmd->hyperlink_start.has_id);
    ASSERT_TRUE(cmd->hyperlink_start.id.eql("foo"));
    ASSERT_TRUE(cmd->hyperlink_start.uri.eql("http://example.com"));
}

TEST(hyperlink, OSC_8_hyperlink_with_empty_uri) {
    Parser p;
    feed(p, "8;id=foo;");

    const Command *cmd = p.end('\x1b');
    ASSERT_TRUE(cmd == nullptr);
}

TEST(hyperlink, OSC_8_hyperlink_end) {
    Parser p;
    feed(p, "8;;");

    const Command *cmd = p.end('\x1b');
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Key::hyperlink_end);
}

/* ─── parsers/report_pwd.zig ─────────────────────────────────────────────── */

TEST(report_pwd, OSC_7_report_pwd) {
    Parser p;
    feed(p, "7;file:///tmp/example");

    const Command *cmd = p.end();
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Key::report_pwd);
    ASSERT_TRUE(cmd->report_pwd.value.eql("file:///tmp/example"));
}

TEST(report_pwd, OSC_7_report_pwd_empty) {
    Parser p;
    feed(p, "7;");
    const Command *cmd = p.end();
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Key::report_pwd);
    ASSERT_TRUE(cmd->report_pwd.value.eql(""));
}

/* ─── parsers/mouse_shape.zig ────────────────────────────────────────────── */

TEST(mouse_shape, OSC_22_pointer_cursor) {
    Parser p;
    feed(p, "22;pointer");

    const Command *cmd = p.end();
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Key::mouse_shape);
    ASSERT_TRUE(cmd->mouse_shape.value.eql("pointer"));
}

/* ─── osc.zig: the two that needed the clipboard parser ──────────────────── */

TEST(osc, Parser_nextSlice_matches_per_byte_parsing) {
    const char *input = "52;c;aGVsbG8=";
    const size_t len = strlen(input);

    /* Every two-way split of the input must parse identically to
     * the byte-at-a-time path. */
    for (size_t split = 0; split < len + 1; split++) {
        Parser p(true);
        p.nextSlice((const uint8_t *)input, split);
        p.nextSlice((const uint8_t *)input + split, len - split);

        const Command *cmd = p.end();
        ASSERT_TRUE(cmd != nullptr);
        ASSERT_TRUE(cmd->key == Key::clipboard_contents);
        ASSERT_EQ(cmd->clipboard_contents.kind, 'c');
        ASSERT_TRUE(cmd->clipboard_contents.data.eql("aGVsbG8="));
    }
}

TEST(osc, Parser_allocating_capture_limit_includes_parser_added_bytes) {
    Parser p(true);
    p.max_allocating_bytes = 4;

    feed(p, "52;abcd");
    ASSERT_TRUE(p.end() == nullptr);
    ASSERT_TRUE(p.state == Parser::State::invalid);

    Parser::Capture &cap = p.capture;
    ASSERT_EQ(cap.trailing_len(), 4u);
    ASSERT_EQ(cap.writer.capacity, 4u);
}

/* ─── parsers/clipboard_operation.zig ────────────────────────────────────── */

TEST(clipboard_operation, OSC_52_get_set_clipboard) {
    Parser p;
    feed(p, "52;s;?");

    const Command *cmd = p.end();
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Key::clipboard_contents);
    ASSERT_TRUE(cmd->clipboard_contents.kind == 's');
    ASSERT_TRUE(cmd->clipboard_contents.data.eql("?"));
    ASSERT_TRUE(cmd->clipboard_contents.terminator == Terminator::st);
}

TEST(clipboard_operation, OSC_52_get_clipboard_with_BEL_terminator) {
    Parser p;
    feed(p, "52;c;?");

    const Command *cmd = p.end(0x07);
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Key::clipboard_contents);
    ASSERT_TRUE(cmd->clipboard_contents.terminator == Terminator::bel);
}

TEST(clipboard_operation, OSC_52_get_set_clipboard_optional_parameter) {
    Parser p;
    feed(p, "52;;?");

    const Command *cmd = p.end();
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Key::clipboard_contents);
    ASSERT_TRUE(cmd->clipboard_contents.kind == 'c');
    ASSERT_TRUE(cmd->clipboard_contents.data.eql("?"));
}

TEST(clipboard_operation, OSC_52_get_set_clipboard_with_allocator) {
    Parser p(true);
    feed(p, "52;s;?");

    const Command *cmd = p.end();
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Key::clipboard_contents);
    ASSERT_TRUE(cmd->clipboard_contents.kind == 's');
    ASSERT_TRUE(cmd->clipboard_contents.data.eql("?"));
}

TEST(clipboard_operation, OSC_52_clear_clipboard) {
    Parser p;
    feed(p, "52;;");

    const Command *cmd = p.end();
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Key::clipboard_contents);
    ASSERT_TRUE(cmd->clipboard_contents.kind == 'c');
    ASSERT_TRUE(cmd->clipboard_contents.data.eql(""));
}

/* rxvt_extension.zig */

TEST(rxvt_extension, OSC_777_show_desktop_notification_with_title) {
    Parser p; /* .init(null) */

    const char *input = "777;notify;Title;Body";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);

    Command *cmd = p.end('\x1b');
    ASSERT_TRUE(cmd != nullptr);
    ASSERT_TRUE(cmd->key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd->show_desktop_notification.title.eql("Title"));
    ASSERT_TRUE(cmd->show_desktop_notification.body.eql("Body"));
}

/* kitty_dnd_protocol.zig */

static Command *feed_end(Parser &p, const char *input, bool has_ch, uint8_t ch) {
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    return has_ch ? p.end(ch) : p.end();
}

TEST(kitty_dnd_protocol, OSC_72_metadata_only_no_payload) {
    Parser p(true);
    Command *cmd = feed_end(p, "72;t=a", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_dnd_protocol);
    ASSERT_TRUE(cmd->kitty_dnd_protocol.metadata.eql("t=a"));
    ASSERT_FALSE(cmd->kitty_dnd_protocol.has_payload);
}

TEST(kitty_dnd_protocol, OSC_72_metadata_and_empty_payload) {
    Parser p(true);
    Command *cmd = feed_end(p, "72;t=a;", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_dnd_protocol);
    ASSERT_TRUE(cmd->kitty_dnd_protocol.metadata.eql("t=a"));
    ASSERT_TRUE(cmd->kitty_dnd_protocol.has_payload);
    ASSERT_TRUE(cmd->kitty_dnd_protocol.payload.eql(""));
}

TEST(kitty_dnd_protocol, OSC_72_metadata_and_non_empty_payload) {
    Parser p(true);
    Command *cmd = feed_end(p, "72;t=a:i=5;text/plain text/uri-list", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_dnd_protocol);
    ASSERT_TRUE(cmd->kitty_dnd_protocol.metadata.eql("t=a:i=5"));
    ASSERT_TRUE(cmd->kitty_dnd_protocol.has_payload);
    ASSERT_TRUE(cmd->kitty_dnd_protocol.payload.eql("text/plain text/uri-list"));
}

TEST(kitty_dnd_protocol, OSC_72_empty_metadata_with_payload) {
    Parser p(true);
    Command *cmd = feed_end(p, "72;;payload", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_dnd_protocol);
    ASSERT_TRUE(cmd->kitty_dnd_protocol.metadata.eql(""));
    ASSERT_TRUE(cmd->kitty_dnd_protocol.has_payload);
    ASSERT_TRUE(cmd->kitty_dnd_protocol.payload.eql("payload"));
}

TEST(kitty_dnd_protocol, OSC_72_BEL_terminator_recorded) {
    Parser p(true);
    Command *cmd = feed_end(p, "72;t=q", true, 0x07);
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_dnd_protocol);
    ASSERT_TRUE(cmd->kitty_dnd_protocol.terminator == Terminator::bel);
}

/* kitty_color.zig */

namespace kc = wisp::terminal::kitty::color;

static const char *kitty_color_input =
    "21;foreground=?;background=rgb:f0/f8/ff;cursor=aliceblue;cursor_text;"
    "visual_bell=;selection_foreground=#xxxyyzz;selection_background=?;"
    "selection_background=#aabbcc;2=?;3=rgbi:1.0/1.0/1.0";

TEST(kitty_color, OSC_21_kitty_color_protocol) {
    typedef kc::Kind Kind;
    typedef kc::Request::Tag RT;

    Parser p(true);
    Command *cmd = feed_end(p, kitty_color_input, true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_color_protocol);
    const kc::RequestList &list = cmd->kitty_color_protocol.list;
    ASSERT_TRUE(list.len == 9);
    {
        const kc::Request &item = list.items[0];
        ASSERT_TRUE(item.tag == RT::query);
        ASSERT_TRUE(item.query.eql(Kind::makeSpecial(kc::Special::foreground)));
    }
    {
        const kc::Request &item = list.items[1];
        ASSERT_TRUE(item.tag == RT::set);
        ASSERT_TRUE(item.set.key.eql(Kind::makeSpecial(kc::Special::background)));
        ASSERT_TRUE(item.set.color.r == 0xf0);
        ASSERT_TRUE(item.set.color.g == 0xf8);
        ASSERT_TRUE(item.set.color.b == 0xff);
    }
    {
        const kc::Request &item = list.items[2];
        ASSERT_TRUE(item.tag == RT::set);
        ASSERT_TRUE(item.set.key.eql(Kind::makeSpecial(kc::Special::cursor)));
        ASSERT_TRUE(item.set.color.r == 0xf0);
        ASSERT_TRUE(item.set.color.g == 0xf8);
        ASSERT_TRUE(item.set.color.b == 0xff);
    }
    {
        const kc::Request &item = list.items[3];
        ASSERT_TRUE(item.tag == RT::reset);
        ASSERT_TRUE(item.reset.eql(Kind::makeSpecial(kc::Special::cursor_text)));
    }
    {
        const kc::Request &item = list.items[4];
        ASSERT_TRUE(item.tag == RT::reset);
        ASSERT_TRUE(item.reset.eql(Kind::makeSpecial(kc::Special::visual_bell)));
    }
    {
        const kc::Request &item = list.items[5];
        ASSERT_TRUE(item.tag == RT::query);
        ASSERT_TRUE(item.query.eql(Kind::makeSpecial(kc::Special::selection_background)));
    }
    {
        const kc::Request &item = list.items[6];
        ASSERT_TRUE(item.tag == RT::set);
        ASSERT_TRUE(item.set.key.eql(Kind::makeSpecial(kc::Special::selection_background)));
        ASSERT_TRUE(item.set.color.r == 0xaa);
        ASSERT_TRUE(item.set.color.g == 0xbb);
        ASSERT_TRUE(item.set.color.b == 0xcc);
    }
    {
        const kc::Request &item = list.items[7];
        ASSERT_TRUE(item.tag == RT::query);
        ASSERT_TRUE(item.query.eql(Kind::makePalette(2)));
    }
    {
        const kc::Request &item = list.items[8];
        ASSERT_TRUE(item.tag == RT::set);
        ASSERT_TRUE(item.set.key.eql(Kind::makePalette(3)));
        ASSERT_TRUE(item.set.color.r == 0xff);
        ASSERT_TRUE(item.set.color.g == 0xff);
        ASSERT_TRUE(item.set.color.b == 0xff);
    }
}

TEST(kitty_color, OSC_21_kitty_color_protocol_without_allocator) {
    Parser p; /* .init(null) */
    ASSERT_TRUE(feed_end(p, "21;foreground=?", true, '\x1b') == nullptr);
}

TEST(kitty_color, OSC_21_kitty_color_protocol_double_reset) {
    Parser p(true);
    Command *cmd = feed_end(p, kitty_color_input, true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_color_protocol);

    p.reset();
    p.reset();
}

TEST(kitty_color, OSC_21_kitty_color_protocol_reset_after_invalid) {
    Parser p(true);
    Command *cmd = feed_end(p, kitty_color_input, true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_color_protocol);

    p.reset();

    ASSERT_TRUE(p.state == Parser::State::start);
    p.next('X');
    ASSERT_TRUE(p.state == Parser::State::invalid);

    p.reset();
}

TEST(kitty_color, OSC_21_kitty_color_protocol_no_key) {
    Parser p(true);
    Command *cmd = feed_end(p, "21;", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_color_protocol);
    ASSERT_TRUE(cmd->kitty_color_protocol.list.len == 0);
}

/* kitty/color.zig */

TEST(kitty_color, OSC_kitty_color_protocol_kind_string) {
    char buf[256];
    {
        const size_t n = kc::Kind::makeSpecial(kc::Special::foreground).format(buf, sizeof(buf));
        ASSERT_TRUE(n == 10 && strcmp(buf, "foreground") == 0);
    }
    {
        const size_t n = kc::Kind::makePalette(42).format(buf, sizeof(buf));
        ASSERT_TRUE(n == 2 && strcmp(buf, "42") == 0);
    }

    ASSERT_TRUE(kc::Kind::makePalette(42).hasTerminalQueryColor());
    ASSERT_TRUE(kc::Kind::makeSpecial(kc::Special::foreground).hasTerminalQueryColor());
    ASSERT_FALSE(kc::Kind::makeSpecial(kc::Special::selection_background).hasTerminalQueryColor());
}

/* kitty_text_sizing.zig */

namespace kts = wisp::terminal::osc::kitty_text_sizing;

TEST(kitty_text_sizing, OSC_66_empty_parameters) {
    Parser p; /* .init(null) */
    Command *cmd = feed_end(p, "66;;bobr", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_text_sizing);
    ASSERT_TRUE(cmd->kitty_text_sizing.scale == 1);
    ASSERT_TRUE(cmd->kitty_text_sizing.text.eql("bobr"));
}

TEST(kitty_text_sizing, OSC_66_single_parameter) {
    Parser p;
    Command *cmd = feed_end(p, "66;s=2;kurwa", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_text_sizing);
    ASSERT_TRUE(cmd->kitty_text_sizing.scale == 2);
    ASSERT_TRUE(cmd->kitty_text_sizing.text.eql("kurwa"));
}

TEST(kitty_text_sizing, OSC_66_multiple_parameters) {
    Parser p;
    Command *cmd = feed_end(p, "66;s=2:w=7:n=13:d=15:v=1:h=2;long", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_text_sizing);
    ASSERT_TRUE(cmd->kitty_text_sizing.scale == 2);
    ASSERT_TRUE(cmd->kitty_text_sizing.width == 7);
    ASSERT_TRUE(cmd->kitty_text_sizing.numerator == 13);
    ASSERT_TRUE(cmd->kitty_text_sizing.denominator == 15);
    ASSERT_TRUE(cmd->kitty_text_sizing.valign == kts::VAlign::bottom);
    ASSERT_TRUE(cmd->kitty_text_sizing.halign == kts::HAlign::center);
    ASSERT_TRUE(cmd->kitty_text_sizing.text.eql("long"));
}

TEST(kitty_text_sizing, OSC_66_scale_is_zero) {
    Parser p;
    Command *cmd = feed_end(p, "66;s=0;nope", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_text_sizing);
    ASSERT_TRUE(cmd->kitty_text_sizing.scale == 1);
}

TEST(kitty_text_sizing, OSC_66_invalid_parameters) {
    Parser p;
    Command *cmd = feed_end(p, "66;w=8:v=3:n=16;", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_text_sizing);
    ASSERT_TRUE(cmd->kitty_text_sizing.width == 0);
    ASSERT_TRUE(cmd->kitty_text_sizing.valign == kts::VAlign::top);
    ASSERT_TRUE(cmd->kitty_text_sizing.numerator == 0);
}

TEST(kitty_text_sizing, OSC_66_UTF_8) {
    Parser p;
    Command *cmd = feed_end(p, "66;;\360\237\221\273\351\255\221\351\255\205\351\255\215\351\255\211\343\202\264\343\203\274\343\202\271\343\203\203\343\203\206\343\202\243", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_text_sizing);
    ASSERT_TRUE(cmd->kitty_text_sizing.text.eql("\360\237\221\273\351\255\221\351\255\205\351\255\215\351\255\211\343\202\264\343\203\274\343\202\271\343\203\203\343\203\206\343\202\243"));
}

TEST(kitty_text_sizing, OSC_66_unsafe_UTF_8) {
    Parser p;
    ASSERT_TRUE(feed_end(p, "66;;\n", true, '\x1b') == nullptr);
}

TEST(kitty_text_sizing, OSC_66_overlong_UTF_8) {
    Parser p;
    for (const char *c = "66;;"; *c; c++) p.next((uint8_t)*c);
    for (int i = 0; i < 1025; i++) {
        for (const char *c = "bobr"; *c; c++) p.next((uint8_t)*c);
    }
    ASSERT_TRUE(p.end('\x1b') == nullptr);
}

/* context_signal.zig */

namespace cs = wisp::terminal::osc::context_signal;

static std::string repeat_a(size_t n) { return std::string(n, 'a'); }

TEST(context_signal, OSC_3008_basic_start_command) {
    Parser p; /* .init(null) */
    Command *cmd = feed_end(p, "3008;start=abc123", false, 0);
    ASSERT_TRUE(cmd && cmd->key == Command::Key::context_signal);
    ASSERT_TRUE(cmd->context_signal.action == cs::Command::Action::start);
    ASSERT_TRUE(cmd->context_signal.id.eql("abc123"));
    ASSERT_TRUE(cmd->context_signal.metadata.eql(""));
}

TEST(context_signal, OSC_3008_basic_end_command) {
    Parser p;
    Command *cmd = feed_end(p, "3008;end=abc123", false, 0);
    ASSERT_TRUE(cmd && cmd->key == Command::Key::context_signal);
    ASSERT_TRUE(cmd->context_signal.action == cs::Command::Action::end);
    ASSERT_TRUE(cmd->context_signal.id.eql("abc123"));
    ASSERT_TRUE(cmd->context_signal.metadata.eql(""));
}

TEST(context_signal, OSC_3008_start_with_metadata_fields) {
    Parser p;
    Command *cmd = feed_end(p, "3008;start=bed86fab93af4328bbed0a1224af6d40;type=container;user=lennart;hostname=zeta", false, 0);
    ASSERT_TRUE(cmd && cmd->key == Command::Key::context_signal);
    ASSERT_TRUE(cmd->context_signal.action == cs::Command::Action::start);
    ASSERT_TRUE(cmd->context_signal.id.eql("bed86fab93af4328bbed0a1224af6d40"));

    /* Read individual fields */
    cs::ContextType t;
    ZStr v;
    ASSERT_TRUE(cmd->context_signal.readType(&t) && t == cs::ContextType::container);
    ASSERT_TRUE(cmd->context_signal.readString(cs::Field::user, &v) && v.eql("lennart"));
    ASSERT_TRUE(cmd->context_signal.readString(cs::Field::hostname, &v) && v.eql("zeta"));
}

TEST(context_signal, OSC_3008_start_with_all_common_fields) {
    Parser p;
    Command *cmd = feed_end(p, "3008;start=myctx;type=shell;user=root;hostname=myhost;machineid=3deb5353d3ba43d08201c136a47ead7b;bootid=d4a3d0fdf2e24fdea6d971ce73f4fbf2;pid=1062862;pidfdid=1063162;comm=bash", false, 0);
    ASSERT_TRUE(cmd && cmd->key == Command::Key::context_signal);
    const cs::Command &c = cmd->context_signal;
    cs::ContextType t;
    ZStr v;
    uint64_t n;
    ASSERT_TRUE(c.readType(&t) && t == cs::ContextType::shell);
    ASSERT_TRUE(c.readString(cs::Field::user, &v) && v.eql("root"));
    ASSERT_TRUE(c.readString(cs::Field::hostname, &v) && v.eql("myhost"));
    ASSERT_TRUE(c.readString(cs::Field::machineid, &v) && v.eql("3deb5353d3ba43d08201c136a47ead7b"));
    ASSERT_TRUE(c.readString(cs::Field::bootid, &v) && v.eql("d4a3d0fdf2e24fdea6d971ce73f4fbf2"));
    ASSERT_TRUE(c.readU64(cs::Field::pid, &n) && n == 1062862);
    ASSERT_TRUE(c.readU64(cs::Field::pidfdid, &n) && n == 1063162);
    ASSERT_TRUE(c.readString(cs::Field::comm, &v) && v.eql("bash"));
}

TEST(context_signal, OSC_3008_end_with_exit_metadata) {
    Parser p;
    Command *cmd = feed_end(p, "3008;end=myctx;exit=success;status=0", false, 0);
    ASSERT_TRUE(cmd && cmd->key == Command::Key::context_signal);
    ASSERT_TRUE(cmd->context_signal.action == cs::Command::Action::end);
    ASSERT_TRUE(cmd->context_signal.id.eql("myctx"));
    cs::ExitStatus e;
    uint64_t n;
    ASSERT_TRUE(cmd->context_signal.readExit(&e) && e == cs::ExitStatus::success);
    ASSERT_TRUE(cmd->context_signal.readU64(cs::Field::status, &n) && n == 0);
}

TEST(context_signal, OSC_3008_end_with_failure_exit) {
    Parser p;
    Command *cmd = feed_end(p, "3008;end=myctx;exit=failure;status=1;signal=SIGKILL", false, 0);
    ASSERT_TRUE(cmd && cmd->key == Command::Key::context_signal);
    cs::ExitStatus e;
    uint64_t n;
    ZStr v;
    ASSERT_TRUE(cmd->context_signal.readExit(&e) && e == cs::ExitStatus::failure);
    ASSERT_TRUE(cmd->context_signal.readU64(cs::Field::status, &n) && n == 1);
    ASSERT_TRUE(cmd->context_signal.readString(cs::Field::signal, &v) && v.eql("SIGKILL"));
}

TEST(context_signal, OSC_3008_unknown_fields_are_ignored) {
    Parser p;
    Command *cmd = feed_end(p, "3008;start=myctx;type=shell;unknownfield=value;user=root", false, 0);
    ASSERT_TRUE(cmd && cmd->key == Command::Key::context_signal);
    cs::ContextType t;
    ZStr v;
    ASSERT_TRUE(cmd->context_signal.readType(&t) && t == cs::ContextType::shell);
    ASSERT_TRUE(cmd->context_signal.readString(cs::Field::user, &v) && v.eql("root"));
}

TEST(context_signal, OSC_3008_missing_field_returns_null) {
    Parser p;
    Command *cmd = feed_end(p, "3008;start=myctx;user=lennart", false, 0);
    ASSERT_TRUE(cmd && cmd->key == Command::Key::context_signal);
    cs::ContextType t;
    ZStr v;
    uint64_t n;
    ASSERT_FALSE(cmd->context_signal.readType(&t));
    ASSERT_FALSE(cmd->context_signal.readString(cs::Field::hostname, &v));
    ASSERT_FALSE(cmd->context_signal.readU64(cs::Field::pid, &n));
}

TEST(context_signal, OSC_3008_invalid_prefix) {
    Parser p;
    ASSERT_TRUE(feed_end(p, "3008;bogus=abc123", false, 0) == nullptr);
}

TEST(context_signal, OSC_3008_empty_data) {
    /* Can't really produce empty data after "3008;" because the state machine
     * won't write a writer for that case, but we test the edge case where
     * only "start=" is present with no ID. */
    Parser p;
    ASSERT_TRUE(feed_end(p, "3008;start=", false, 0) == nullptr);
}

TEST(context_signal, OSC_3008_max_length_context_ID) {
    Parser p;
    const std::string id = repeat_a(64);
    const std::string input = "3008;start=" + id;
    Command *cmd = feed_end(p, input.c_str(), false, 0);
    ASSERT_TRUE(cmd && cmd->key == Command::Key::context_signal);
    ASSERT_TRUE(cmd->context_signal.id.eql(id.c_str()));
}

TEST(context_signal, OSC_3008_over_length_context_ID) {
    Parser p;
    const std::string input = "3008;start=" + repeat_a(65);
    ASSERT_TRUE(feed_end(p, input.c_str(), false, 0) == nullptr);
}

TEST(context_signal, OSC_3008_context_type_enum_coverage) {
    struct T { const char *str; cs::ContextType expected; };
    const T types[] = {
        { "boot", cs::ContextType::boot },
        { "container", cs::ContextType::container },
        { "vm", cs::ContextType::vm },
        { "elevate", cs::ContextType::elevate },
        { "chpriv", cs::ContextType::chpriv },
        { "subcontext", cs::ContextType::subcontext },
        { "remote", cs::ContextType::remote },
        { "shell", cs::ContextType::shell },
        { "command", cs::ContextType::command },
        { "app", cs::ContextType::app },
        { "service", cs::ContextType::service },
        { "session", cs::ContextType::session },
    };

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        cs::ContextType t;
        ASSERT_TRUE(cs::ContextType_parse(types[i].str, &t) && t == types[i].expected);
    }

    cs::ContextType t;
    ASSERT_FALSE(cs::ContextType_parse("invalid", &t));
}

TEST(context_signal, OSC_3008_exit_status_enum_coverage) {
    cs::ExitStatus e;
    ASSERT_TRUE(cs::ExitStatus_parse("success", &e) && e == cs::ExitStatus::success);
    ASSERT_TRUE(cs::ExitStatus_parse("failure", &e) && e == cs::ExitStatus::failure);
    ASSERT_TRUE(cs::ExitStatus_parse("crash", &e) && e == cs::ExitStatus::crash);
    ASSERT_TRUE(cs::ExitStatus_parse("interrupt", &e) && e == cs::ExitStatus::interrupt);
    ASSERT_FALSE(cs::ExitStatus_parse("invalid", &e));
}

TEST(context_signal, OSC_3008_spec_example_container_start) {
    /* From the spec: a new container "foobar" invoked by user "lennart" on host "zeta" */
    Parser p;
    Command *cmd = feed_end(p, "3008;start=bed86fab93af4328bbed0a1224af6d40;type=container;user=lennart;hostname=zeta;machineid=3deb5353d3ba43d08201c136a47ead7b;bootid=d4a3d0fdf2e24fdea6d971ce73f4fbf2;pid=1062862;pidfdid=1063162;comm=systemd-nspawn;container=foobar", false, 0);
    ASSERT_TRUE(cmd && cmd->key == Command::Key::context_signal);
    const cs::Command &c = cmd->context_signal;
    cs::ContextType t;
    ZStr v;
    uint64_t n;
    ASSERT_TRUE(c.action == cs::Command::Action::start);
    ASSERT_TRUE(c.id.eql("bed86fab93af4328bbed0a1224af6d40"));
    ASSERT_TRUE(c.readType(&t) && t == cs::ContextType::container);
    ASSERT_TRUE(c.readString(cs::Field::user, &v) && v.eql("lennart"));
    ASSERT_TRUE(c.readString(cs::Field::hostname, &v) && v.eql("zeta"));
    ASSERT_TRUE(c.readString(cs::Field::comm, &v) && v.eql("systemd-nspawn"));
    ASSERT_TRUE(c.readString(cs::Field::container, &v) && v.eql("foobar"));
    ASSERT_TRUE(c.readU64(cs::Field::pid, &n) && n == 1062862);
}

TEST(context_signal, OSC_3008_spec_example_context_end) {
    /* From the spec: context end */
    Parser p;
    Command *cmd = feed_end(p, "3008;end=bed86fab93af4328bbed0a1224af6d40", false, 0);
    ASSERT_TRUE(cmd && cmd->key == Command::Key::context_signal);
    ASSERT_TRUE(cmd->context_signal.action == cs::Command::Action::end);
    ASSERT_TRUE(cmd->context_signal.id.eql("bed86fab93af4328bbed0a1224af6d40"));
}

TEST(context_signal, OSC_3008_cwd_and_cmdline_fields) {
    Parser p;
    Command *cmd = feed_end(p, "3008;start=myctx;type=command;cwd=/home/user;cmdline=ls -la", false, 0);
    ASSERT_TRUE(cmd && cmd->key == Command::Key::context_signal);
    ZStr v;
    ASSERT_TRUE(cmd->context_signal.readString(cs::Field::cwd, &v) && v.eql("/home/user"));
    ASSERT_TRUE(cmd->context_signal.readString(cs::Field::cmdline, &v) && v.eql("ls -la"));
}

TEST(context_signal, OSC_3008_start_command_with_no_fields) {
    Parser p;
    Command *cmd = feed_end(p, "3008;start=simpleid", false, 0);
    ASSERT_TRUE(cmd && cmd->key == Command::Key::context_signal);
    ASSERT_TRUE(cmd->context_signal.action == cs::Command::Action::start);
    ASSERT_TRUE(cmd->context_signal.id.eql("simpleid"));
    cs::ContextType t;
    ZStr v;
    cs::ExitStatus e;
    ASSERT_FALSE(cmd->context_signal.readType(&t));
    ASSERT_FALSE(cmd->context_signal.readString(cs::Field::user, &v));
    ASSERT_FALSE(cmd->context_signal.readExit(&e));
}

/* iterm2.zig */

TEST(iterm2, OSC_1337_test_valid_unimplemented_key_with_no_value) {
    Parser p(true);
    ASSERT_TRUE(feed_end(p, "1337;SetBadgeFormat", true, '\x1b') == nullptr);
}

TEST(iterm2, OSC_1337_test_valid_unimplemented_key_with_empty_value) {
    Parser p(true);
    ASSERT_TRUE(feed_end(p, "1337;SetBadgeFormat=", true, '\x1b') == nullptr);
}

TEST(iterm2, OSC_1337_test_valid_unimplemented_key_with_non_empty_value) {
    Parser p(true);
    ASSERT_TRUE(feed_end(p, "1337;SetBadgeFormat=abc123", true, '\x1b') == nullptr);
}

TEST(iterm2, OSC_1337_test_valid_key_with_lower_case_and_with_no_value) {
    Parser p(true);
    ASSERT_TRUE(feed_end(p, "1337;setbadgeformat", true, '\x1b') == nullptr);
}

TEST(iterm2, OSC_1337_test_valid_key_with_lower_case_and_with_empty_value) {
    Parser p(true);
    ASSERT_TRUE(feed_end(p, "1337;setbadgeformat=", true, '\x1b') == nullptr);
}

TEST(iterm2, OSC_1337_test_valid_key_with_lower_case_and_with_non_empty_value) {
    Parser p(true);
    ASSERT_TRUE(feed_end(p, "1337;setbadgeformat=abc123", true, '\x1b') == nullptr);
}

TEST(iterm2, OSC_1337_test_invalid_key_with_no_value) {
    Parser p(true);
    ASSERT_TRUE(feed_end(p, "1337;BobrKurwa", true, '\x1b') == nullptr);
}

TEST(iterm2, OSC_1337_test_invalid_key_with_empty_value) {
    Parser p(true);
    ASSERT_TRUE(feed_end(p, "1337;BobrKurwa=", true, '\x1b') == nullptr);
}

TEST(iterm2, OSC_1337_test_invalid_key_with_non_empty_value) {
    Parser p(true);
    ASSERT_TRUE(feed_end(p, "1337;BobrKurwa=abc123", true, '\x1b') == nullptr);
}

TEST(iterm2, OSC_1337_test_Copy_with_no_value) {
    Parser p(true);
    ASSERT_TRUE(feed_end(p, "1337;Copy", true, '\x1b') == nullptr);
}

TEST(iterm2, OSC_1337_test_Copy_with_empty_value) {
    Parser p(true);
    ASSERT_TRUE(feed_end(p, "1337;Copy=", true, '\x1b') == nullptr);
}

TEST(iterm2, OSC_1337_test_Copy_with_only_prefix_colon) {
    Parser p(true);
    ASSERT_TRUE(feed_end(p, "1337;Copy=:", true, '\x1b') == nullptr);
}

TEST(iterm2, OSC_1337_test_Copy_with_question_mark) {
    Parser p(true);
    ASSERT_TRUE(feed_end(p, "1337;Copy=:?", true, '\x1b') == nullptr);
}

/* "OSC: 1337: test Copy with non-empty value that is invalid base64" is
 * skipped upstream (error.SkipZigTest): for performance reasons, base64 is
 * not checked right now. */

TEST(iterm2, OSC_1337_test_Copy_with_non_empty_value_that_is_valid_base64_but_not_prefixed_with_a_colon) {
    Parser p(true);
    ASSERT_TRUE(feed_end(p, "1337;Copy=YWJjMTIz", true, '\x1b') == nullptr);
}

TEST(iterm2, OSC_1337_test_Copy_with_non_empty_value_that_is_valid_base64) {
    Parser p(true);
    Command *cmd = feed_end(p, "1337;Copy=:YWJjMTIz", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::clipboard_contents);
    ASSERT_TRUE(cmd->clipboard_contents.kind == 'c');
    ASSERT_TRUE(cmd->clipboard_contents.data.eql("YWJjMTIz"));
}

TEST(iterm2, OSC_1337_test_CurrentDir_with_no_value) {
    Parser p(true);
    ASSERT_TRUE(feed_end(p, "1337;CurrentDir", true, '\x1b') == nullptr);
}

TEST(iterm2, OSC_1337_test_CurrentDir_with_empty_value) {
    Parser p(true);
    ASSERT_TRUE(feed_end(p, "1337;CurrentDir=", true, '\x1b') == nullptr);
}

TEST(iterm2, OSC_1337_test_CurrentDir_with_non_empty_value) {
    Parser p(true);
    Command *cmd = feed_end(p, "1337;CurrentDir=abc123", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::report_pwd);
    ASSERT_TRUE(cmd->report_pwd.value.eql("abc123"));
}

/* kitty_clipboard_protocol.zig */

namespace kcp = wisp::terminal::osc::kitty_clipboard_protocol;

TEST(kitty_clipboard_protocol, OSC_5522_empty_metadata_and_missing_payload) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_TRUE(k.metadata.eql(""));
    ASSERT_FALSE(k.has_payload);
    ASSERT_FALSE(k.readString(kcp::Option::id, &v));
    ASSERT_FALSE(k.readLoc(&loc));
    ASSERT_FALSE(k.readString(kcp::Option::mime, &v));
    ASSERT_FALSE(k.readString(kcp::Option::name, &v));
    ASSERT_FALSE(k.readString(kcp::Option::password, &v));
    ASSERT_FALSE(k.readString(kcp::Option::pw, &v));
    ASSERT_FALSE(k.readStatus(&st));
    ASSERT_FALSE(k.readType(&op));
}

TEST(kitty_clipboard_protocol, OSC_5522_empty_metadata_and_empty_payload) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;;", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_TRUE(k.metadata.eql(""));
    ASSERT_TRUE(k.has_payload && k.payload.eql(""));
    ASSERT_FALSE(k.readString(kcp::Option::id, &v));
    ASSERT_FALSE(k.readLoc(&loc));
    ASSERT_FALSE(k.readString(kcp::Option::mime, &v));
    ASSERT_FALSE(k.readString(kcp::Option::name, &v));
    ASSERT_FALSE(k.readString(kcp::Option::password, &v));
    ASSERT_FALSE(k.readString(kcp::Option::pw, &v));
    ASSERT_FALSE(k.readStatus(&st));
    ASSERT_FALSE(k.readType(&op));
}

TEST(kitty_clipboard_protocol, OSC_5522_non_empty_metadata_and_payload) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;type=read;dGV4dC9wbGFpbg==", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_TRUE(k.metadata.eql("type=read"));
    ASSERT_TRUE(k.has_payload && k.payload.eql("dGV4dC9wbGFpbg=="));
    ASSERT_FALSE(k.readString(kcp::Option::id, &v));
    ASSERT_FALSE(k.readLoc(&loc));
    ASSERT_FALSE(k.readString(kcp::Option::mime, &v));
    ASSERT_FALSE(k.readString(kcp::Option::name, &v));
    ASSERT_FALSE(k.readString(kcp::Option::password, &v));
    ASSERT_FALSE(k.readString(kcp::Option::pw, &v));
    ASSERT_FALSE(k.readStatus(&st));
    ASSERT_TRUE(k.readType(&op) && op == kcp::Operation::read);
}

TEST(kitty_clipboard_protocol, OSC_5522_empty_id) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;id=", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_FALSE(k.readString(kcp::Option::id, &v));
}

TEST(kitty_clipboard_protocol, OSC_5522_valid_id) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;id=5c076ad9-d36f-4705-847b-d4dbf356cc0d", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_TRUE(k.readString(kcp::Option::id, &v) && v.eql("5c076ad9-d36f-4705-847b-d4dbf356cc0d"));
}

TEST(kitty_clipboard_protocol, OSC_5522_invalid_id) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;id=*42*", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_FALSE(k.readString(kcp::Option::id, &v));
}

TEST(kitty_clipboard_protocol, OSC_5522_invalid_status) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;status=BOBR", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_FALSE(k.readStatus(&st));
}

TEST(kitty_clipboard_protocol, OSC_5522_valid_status) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;status=DONE", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_TRUE(k.readStatus(&st) && st == kcp::Status::DONE);
}

TEST(kitty_clipboard_protocol, OSC_5522_invalid_location) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;loc=bobr", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_FALSE(k.readLoc(&loc));
}

TEST(kitty_clipboard_protocol, OSC_5522_valid_location) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;loc=primary", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_TRUE(k.readLoc(&loc) && loc == kcp::Location::primary);
}

TEST(kitty_clipboard_protocol, OSC_5522_password_1) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;pw=R2hvc3R0eQ==:name=Qk9CUiBLVVJXQQ==", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_TRUE(k.readString(kcp::Option::pw, &v) && v.eql("R2hvc3R0eQ=="));
    ASSERT_TRUE(k.readString(kcp::Option::name, &v) && v.eql("Qk9CUiBLVVJXQQ=="));
}

TEST(kitty_clipboard_protocol, OSC_5522_password_2) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;password=R2hvc3R0eQ==", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_TRUE(k.readString(kcp::Option::password, &v) && v.eql("R2hvc3R0eQ=="));
}

TEST(kitty_clipboard_protocol, OSC_5522_example_1) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;type=read:status=OK", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_FALSE(k.has_payload);
    ASSERT_FALSE(k.readString(kcp::Option::id, &v));
    ASSERT_FALSE(k.readLoc(&loc));
    ASSERT_FALSE(k.readString(kcp::Option::mime, &v));
    ASSERT_FALSE(k.readString(kcp::Option::name, &v));
    ASSERT_FALSE(k.readString(kcp::Option::password, &v));
    ASSERT_FALSE(k.readString(kcp::Option::pw, &v));
    ASSERT_TRUE(k.readStatus(&st) && st == kcp::Status::OK);
    ASSERT_TRUE(k.readType(&op) && op == kcp::Operation::read);
}

TEST(kitty_clipboard_protocol, OSC_5522_example_2) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;type=read:mime=dGV4dC9wbGFpbg==;R2hvc3R0eQ==", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_TRUE(k.has_payload && k.payload.eql("R2hvc3R0eQ=="));
    ASSERT_FALSE(k.readString(kcp::Option::id, &v));
    ASSERT_FALSE(k.readLoc(&loc));
    ASSERT_TRUE(k.readString(kcp::Option::mime, &v) && v.eql("dGV4dC9wbGFpbg=="));
    ASSERT_FALSE(k.readString(kcp::Option::name, &v));
    ASSERT_FALSE(k.readString(kcp::Option::password, &v));
    ASSERT_FALSE(k.readString(kcp::Option::pw, &v));
    ASSERT_FALSE(k.readStatus(&st));
    ASSERT_TRUE(k.readType(&op) && op == kcp::Operation::read);
}

TEST(kitty_clipboard_protocol, OSC_5522_example_3) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;type=read:status=OK", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_FALSE(k.has_payload);
    ASSERT_FALSE(k.readString(kcp::Option::id, &v));
    ASSERT_FALSE(k.readLoc(&loc));
    ASSERT_FALSE(k.readString(kcp::Option::mime, &v));
    ASSERT_FALSE(k.readString(kcp::Option::name, &v));
    ASSERT_FALSE(k.readString(kcp::Option::password, &v));
    ASSERT_FALSE(k.readString(kcp::Option::pw, &v));
    ASSERT_TRUE(k.readStatus(&st) && st == kcp::Status::OK);
    ASSERT_TRUE(k.readType(&op) && op == kcp::Operation::read);
}

TEST(kitty_clipboard_protocol, OSC_5522_example_4) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;type=write", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_FALSE(k.has_payload);
    ASSERT_FALSE(k.readString(kcp::Option::id, &v));
    ASSERT_FALSE(k.readLoc(&loc));
    ASSERT_FALSE(k.readString(kcp::Option::mime, &v));
    ASSERT_FALSE(k.readString(kcp::Option::name, &v));
    ASSERT_FALSE(k.readString(kcp::Option::password, &v));
    ASSERT_FALSE(k.readString(kcp::Option::pw, &v));
    ASSERT_FALSE(k.readStatus(&st));
    ASSERT_TRUE(k.readType(&op) && op == kcp::Operation::write);
}

TEST(kitty_clipboard_protocol, OSC_5522_example_5) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;type=wdata:mime=dGV4dC9wbGFpbg==;R2hvc3R0eQ==", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_TRUE(k.has_payload && k.payload.eql("R2hvc3R0eQ=="));
    ASSERT_FALSE(k.readString(kcp::Option::id, &v));
    ASSERT_FALSE(k.readLoc(&loc));
    ASSERT_TRUE(k.readString(kcp::Option::mime, &v) && v.eql("dGV4dC9wbGFpbg=="));
    ASSERT_FALSE(k.readString(kcp::Option::name, &v));
    ASSERT_FALSE(k.readString(kcp::Option::password, &v));
    ASSERT_FALSE(k.readString(kcp::Option::pw, &v));
    ASSERT_FALSE(k.readStatus(&st));
    ASSERT_TRUE(k.readType(&op) && op == kcp::Operation::wdata);
}

TEST(kitty_clipboard_protocol, OSC_5522_example_6) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;type=wdata", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_FALSE(k.has_payload);
    ASSERT_FALSE(k.readString(kcp::Option::id, &v));
    ASSERT_FALSE(k.readLoc(&loc));
    ASSERT_FALSE(k.readString(kcp::Option::mime, &v));
    ASSERT_FALSE(k.readString(kcp::Option::name, &v));
    ASSERT_FALSE(k.readString(kcp::Option::password, &v));
    ASSERT_FALSE(k.readString(kcp::Option::pw, &v));
    ASSERT_FALSE(k.readStatus(&st));
    ASSERT_TRUE(k.readType(&op) && op == kcp::Operation::wdata);
}

TEST(kitty_clipboard_protocol, OSC_5522_example_7) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;type=write:status=DONE", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_FALSE(k.has_payload);
    ASSERT_FALSE(k.readString(kcp::Option::id, &v));
    ASSERT_FALSE(k.readLoc(&loc));
    ASSERT_FALSE(k.readString(kcp::Option::mime, &v));
    ASSERT_FALSE(k.readString(kcp::Option::name, &v));
    ASSERT_FALSE(k.readString(kcp::Option::password, &v));
    ASSERT_FALSE(k.readString(kcp::Option::pw, &v));
    ASSERT_TRUE(k.readStatus(&st) && st == kcp::Status::DONE);
    ASSERT_TRUE(k.readType(&op) && op == kcp::Operation::write);
}

TEST(kitty_clipboard_protocol, OSC_5522_example_8) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;type=write:status=EPERM", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_FALSE(k.has_payload);
    ASSERT_FALSE(k.readString(kcp::Option::id, &v));
    ASSERT_FALSE(k.readLoc(&loc));
    ASSERT_FALSE(k.readString(kcp::Option::mime, &v));
    ASSERT_FALSE(k.readString(kcp::Option::name, &v));
    ASSERT_FALSE(k.readString(kcp::Option::password, &v));
    ASSERT_FALSE(k.readString(kcp::Option::pw, &v));
    ASSERT_TRUE(k.readStatus(&st) && st == kcp::Status::EPERM_);
    ASSERT_TRUE(k.readType(&op) && op == kcp::Operation::write);
}

TEST(kitty_clipboard_protocol, OSC_5522_example_9) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;type=walias:mime=dGV4dC9wbGFpbg==;dGV4dC9odG1sIGFwcGxpY2F0aW9uL2pzb24=", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_TRUE(k.has_payload && k.payload.eql("dGV4dC9odG1sIGFwcGxpY2F0aW9uL2pzb24="));
    ASSERT_FALSE(k.readString(kcp::Option::id, &v));
    ASSERT_FALSE(k.readLoc(&loc));
    ASSERT_TRUE(k.readString(kcp::Option::mime, &v) && v.eql("dGV4dC9wbGFpbg=="));
    ASSERT_FALSE(k.readString(kcp::Option::name, &v));
    ASSERT_FALSE(k.readString(kcp::Option::password, &v));
    ASSERT_FALSE(k.readString(kcp::Option::pw, &v));
    ASSERT_FALSE(k.readStatus(&st));
    ASSERT_TRUE(k.readType(&op) && op == kcp::Operation::walias);
}

TEST(kitty_clipboard_protocol, OSC_5522_example_10) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;type=read:status=OK:password=Qk9CUiBLVVJXQQ==", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_FALSE(k.has_payload);
    ASSERT_FALSE(k.readString(kcp::Option::id, &v));
    ASSERT_FALSE(k.readLoc(&loc));
    ASSERT_FALSE(k.readString(kcp::Option::mime, &v));
    ASSERT_FALSE(k.readString(kcp::Option::name, &v));
    ASSERT_TRUE(k.readString(kcp::Option::password, &v) && v.eql("Qk9CUiBLVVJXQQ=="));
    ASSERT_FALSE(k.readString(kcp::Option::pw, &v));
    ASSERT_TRUE(k.readStatus(&st) && st == kcp::Status::OK);
    ASSERT_TRUE(k.readType(&op) && op == kcp::Operation::read);
}

TEST(kitty_clipboard_protocol, OSC_5522_example_11) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;type=read:status=DATA:mime=dGV4dC9wbGFpbg==", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_FALSE(k.has_payload);
    ASSERT_FALSE(k.readString(kcp::Option::id, &v));
    ASSERT_FALSE(k.readLoc(&loc));
    ASSERT_TRUE(k.readString(kcp::Option::mime, &v) && v.eql("dGV4dC9wbGFpbg=="));
    ASSERT_FALSE(k.readString(kcp::Option::name, &v));
    ASSERT_FALSE(k.readString(kcp::Option::password, &v));
    ASSERT_FALSE(k.readString(kcp::Option::pw, &v));
    ASSERT_TRUE(k.readStatus(&st) && st == kcp::Status::DATA);
    ASSERT_TRUE(k.readType(&op) && op == kcp::Operation::read);
}

TEST(kitty_clipboard_protocol, OSC_5522_example_12) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;type=read:mime=dGV4dC9wbGFpbg==:password=Qk9CUiBLVVJXQQ==", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_FALSE(k.has_payload);
    ASSERT_FALSE(k.readString(kcp::Option::id, &v));
    ASSERT_FALSE(k.readLoc(&loc));
    ASSERT_TRUE(k.readString(kcp::Option::mime, &v) && v.eql("dGV4dC9wbGFpbg=="));
    ASSERT_FALSE(k.readString(kcp::Option::name, &v));
    ASSERT_TRUE(k.readString(kcp::Option::password, &v) && v.eql("Qk9CUiBLVVJXQQ=="));
    ASSERT_FALSE(k.readString(kcp::Option::pw, &v));
    ASSERT_FALSE(k.readStatus(&st));
    ASSERT_TRUE(k.readType(&op) && op == kcp::Operation::read);
}

TEST(kitty_clipboard_protocol, OSC_5522_example_13) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;type=read:status=OK", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_FALSE(k.has_payload);
    ASSERT_FALSE(k.readString(kcp::Option::id, &v));
    ASSERT_FALSE(k.readLoc(&loc));
    ASSERT_FALSE(k.readString(kcp::Option::mime, &v));
    ASSERT_FALSE(k.readString(kcp::Option::name, &v));
    ASSERT_FALSE(k.readString(kcp::Option::password, &v));
    ASSERT_FALSE(k.readString(kcp::Option::pw, &v));
    ASSERT_TRUE(k.readStatus(&st) && st == kcp::Status::OK);
    ASSERT_TRUE(k.readType(&op) && op == kcp::Operation::read);
}

TEST(kitty_clipboard_protocol, OSC_5522_example_14) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;type=read:status=DATA:mime=dGV4dC9wbGFpbg==;Qk9CUiBLVVJXQQ==", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_TRUE(k.has_payload && k.payload.eql("Qk9CUiBLVVJXQQ=="));
    ASSERT_FALSE(k.readString(kcp::Option::id, &v));
    ASSERT_FALSE(k.readLoc(&loc));
    ASSERT_TRUE(k.readString(kcp::Option::mime, &v) && v.eql("dGV4dC9wbGFpbg=="));
    ASSERT_FALSE(k.readString(kcp::Option::name, &v));
    ASSERT_FALSE(k.readString(kcp::Option::password, &v));
    ASSERT_FALSE(k.readString(kcp::Option::pw, &v));
    ASSERT_TRUE(k.readStatus(&st) && st == kcp::Status::DATA);
    ASSERT_TRUE(k.readType(&op) && op == kcp::Operation::read);
}

TEST(kitty_clipboard_protocol, OSC_5522_example_15) {
    Parser p(true);
    Command *cmd = feed_end(p, "5522;type=read:status=OK", true, '\x1b');
    ASSERT_TRUE(cmd && cmd->key == Command::Key::kitty_clipboard_protocol);
    const kcp::OSC &k = cmd->kitty_clipboard_protocol;
    ZStr v;
    kcp::Location loc;
    kcp::Status st;
    kcp::Operation op;
    (void)v; (void)loc; (void)st; (void)op;
    ASSERT_FALSE(k.has_payload);
    ASSERT_FALSE(k.readString(kcp::Option::id, &v));
    ASSERT_FALSE(k.readLoc(&loc));
    ASSERT_FALSE(k.readString(kcp::Option::mime, &v));
    ASSERT_FALSE(k.readString(kcp::Option::name, &v));
    ASSERT_FALSE(k.readString(kcp::Option::password, &v));
    ASSERT_FALSE(k.readString(kcp::Option::pw, &v));
    ASSERT_TRUE(k.readStatus(&st) && st == kcp::Status::OK);
    ASSERT_TRUE(k.readType(&op) && op == kcp::Operation::read);
}

/* kitty_metadata.zig */

TEST(kitty_metadata, ValueIterator_skips_malformed_and_prefix_matching_keys) {
    const char *md = "id-extra=wrong:id: id = first :id=second";
    kitty_metadata::ValueIterator it("id", nullptr, md, strlen(md));

    ZStr v;
    ASSERT_TRUE(it.next(&v) && v.eql("first"));
    ASSERT_TRUE(it.next(&v) && v.eql("second"));
    ASSERT_FALSE(it.next(&v));
}

TEST(kitty_metadata, ValueIterator_skips_values_containing_disallowed_characters) {
    const char *md = "i=a?:i=abc";
    kitty_metadata::ValueIterator it("i", "abc", md, strlen(md));

    ZStr v;
    ASSERT_TRUE(it.next(&v) && v.eql("abc"));
    ASSERT_FALSE(it.next(&v));
}

/* semantic_prompt.zig */

namespace sp = wisp::terminal::osc::semantic_prompt;

TEST(semantic_prompt, OSC_133_end_input_start_output) {
    Parser p; /* .init(null) */
    const char *input = "133;C";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_input_start_output);
    { ZStr v1; ASSERT_FALSE(cmd.semantic_prompt.readOption(sp::Option::aid, &v1)); }
    { sp::Click v2; ASSERT_FALSE(cmd.semantic_prompt.readOption(sp::Option::cl, &v2)); }
}

TEST(semantic_prompt, OSC_133_end_input_start_output_extra_contents) {
    Parser p; /* .init(null) */
    const char *input = "133;Cextra";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    ASSERT_TRUE(p.end() == nullptr);
}

TEST(semantic_prompt, OSC_133_end_input_start_output_with_options) {
    Parser p; /* .init(null) */
    const char *input = "133;C;aid=foo";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_input_start_output);
    { ZStr v3; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::aid, &v3) && v3.eql("foo")); }
}

TEST(semantic_prompt, OSC_133_end_input_start_output_with_cmdline) {
    std::string w;
    Parser p; /* .init(null) */
    const char *input = "133;C;cmdline=echo bobr kurwa";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_input_start_output);
    ASSERT_TRUE(cmd.semantic_prompt.writeCommandLine(&w));
    ASSERT_TRUE(w == "echo bobr kurwa");
}

TEST(semantic_prompt, OSC_133_end_input_start_output_with_cmdline_3) {
    std::string w;
    Parser p; /* .init(null) */
    const char *input = "133;C;cmdline=echo bobr\\nkurwa";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.writeCommandLine(&w));
    ASSERT_TRUE(w == "echo bobr\nkurwa");
}

TEST(semantic_prompt, OSC_133_end_input_start_output_with_cmdline_4) {
    std::string w;
    Parser p; /* .init(null) */
    const char *input = "133;C;cmdline=$'echo bobr kurwa'";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_input_start_output);
    ASSERT_TRUE(cmd.semantic_prompt.writeCommandLine(&w));
    ASSERT_TRUE(w == "echo bobr kurwa");
}

TEST(semantic_prompt, OSC_133_end_input_start_output_with_cmdline_5) {
    std::string w;
    Parser p; /* .init(null) */
    const char *input = "133;C;cmdline='echo bobr kurwa'";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_input_start_output);
    ASSERT_TRUE(cmd.semantic_prompt.writeCommandLine(&w));
    ASSERT_TRUE(w == "echo bobr kurwa");
}

TEST(semantic_prompt, OSC_133_end_input_start_output_with_cmdline_6) {
    std::string w;
    Parser p; /* .init(null) */
    const char *input = "133;C;cmdline='echo bobr kurwa";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_input_start_output);
    ASSERT_FALSE(cmd.semantic_prompt.writeCommandLine(&w));
}

TEST(semantic_prompt, OSC_133_end_input_start_output_with_cmdline_7) {
    std::string w;
    Parser p; /* .init(null) */
    const char *input = "133;C;cmdline=$'echo bobr kurwa";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_input_start_output);
    ASSERT_FALSE(cmd.semantic_prompt.writeCommandLine(&w));
}

TEST(semantic_prompt, OSC_133_end_input_start_output_with_cmdline_8) {
    std::string w;
    Parser p; /* .init(null) */
    const char *input = "133;C;cmdline=$'";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_input_start_output);
    ASSERT_FALSE(cmd.semantic_prompt.writeCommandLine(&w));
}

TEST(semantic_prompt, OSC_133_end_input_start_output_with_cmdline_9) {
    std::string w;
    Parser p; /* .init(null) */
    const char *input = "133;C;cmdline=";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_input_start_output);
    ASSERT_TRUE(cmd.semantic_prompt.writeCommandLine(&w));
    ASSERT_TRUE(w == "");
}

TEST(semantic_prompt, OSC_133_end_input_start_output_with_cmdline_url_1) {
    std::string w;
    Parser p; /* .init(null) */
    const char *input = "133;C;cmdline_url=echo bobr kurwa";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_input_start_output);
    ASSERT_TRUE(cmd.semantic_prompt.writeCommandLine(&w));
    ASSERT_TRUE(w == "echo bobr kurwa");
}

TEST(semantic_prompt, OSC_133_end_input_start_output_with_cmdline_url_2) {
    std::string w;
    Parser p; /* .init(null) */
    const char *input = "133;C;cmdline_url=echo bobr%20kurwa";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_input_start_output);
    ASSERT_TRUE(cmd.semantic_prompt.writeCommandLine(&w));
    ASSERT_TRUE(w == "echo bobr kurwa");
}

TEST(semantic_prompt, OSC_133_end_input_start_output_with_cmdline_url_3) {
    std::string w;
    Parser p; /* .init(null) */
    const char *input = "133;C;cmdline_url=echo bobr%3bkurwa";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_input_start_output);
    ASSERT_TRUE(cmd.semantic_prompt.writeCommandLine(&w));
    ASSERT_TRUE(w == "echo bobr;kurwa");
}

TEST(semantic_prompt, OSC_133_end_input_start_output_with_cmdline_url_4) {
    std::string w;
    Parser p; /* .init(null) */
    const char *input = "133;C;cmdline_url=echo bobr%3kurwa";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_input_start_output);
    ASSERT_FALSE(cmd.semantic_prompt.writeCommandLine(&w));
}

TEST(semantic_prompt, OSC_133_end_input_start_output_with_cmdline_url_5) {
    std::string w;
    Parser p; /* .init(null) */
    const char *input = "133;C;cmdline_url=echo bobr%kurwa";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_input_start_output);
    ASSERT_FALSE(cmd.semantic_prompt.writeCommandLine(&w));
}

TEST(semantic_prompt, OSC_133_end_input_start_output_with_cmdline_url_6) {
    std::string w;
    Parser p; /* .init(null) */
    const char *input = "133;C;cmdline_url=echo bobr kurwa%20";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_input_start_output);
    ASSERT_TRUE(cmd.semantic_prompt.writeCommandLine(&w));
    ASSERT_TRUE(w == "echo bobr kurwa ");
}

TEST(semantic_prompt, OSC_133_end_input_start_output_with_cmdline_url_7) {
    std::string w;
    Parser p; /* .init(null) */
    const char *input = "133;C;cmdline_url=echo bobr kurwa%2";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_input_start_output);
    ASSERT_FALSE(cmd.semantic_prompt.writeCommandLine(&w));
}

TEST(semantic_prompt, OSC_133_end_input_start_output_with_cmdline_url_8) {
    std::string w;
    Parser p; /* .init(null) */
    const char *input = "133;C;cmdline_url=echo bobr kurwa%";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_input_start_output);
    ASSERT_FALSE(cmd.semantic_prompt.writeCommandLine(&w));
}

TEST(semantic_prompt, OSC_133_fresh_line) {
    Parser p; /* .init(null) */
    const char *input = "133;L";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::fresh_line);
}

TEST(semantic_prompt, OSC_133_fresh_line_extra_contents) {
    /* Random */
    {
        Parser p; /* .init(null) */
        const char *input = "133;Lol";
        for (const char *c = input; *c; c++) p.next((uint8_t)*c);
        ASSERT_TRUE(p.end() == nullptr);
    }
    /* Options */
    {
        Parser p; /* .init(null) */
        const char *input = "133;L;aid=foo";
        for (const char *c = input; *c; c++) p.next((uint8_t)*c);
        ASSERT_TRUE(p.end() == nullptr);
    }
}

TEST(semantic_prompt, OSC_133_fresh_line_new_prompt) {
    Parser p; /* .init(null) */
    const char *input = "133;A";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::fresh_line_new_prompt);
    { ZStr v4; ASSERT_FALSE(cmd.semantic_prompt.readOption(sp::Option::aid, &v4)); }
    { sp::Click v5; ASSERT_FALSE(cmd.semantic_prompt.readOption(sp::Option::cl, &v5)); }
}

TEST(semantic_prompt, OSC_133_fresh_line_new_prompt_with_aid) {
    Parser p; /* .init(null) */
    const char *input = "133;A;aid=14";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::fresh_line_new_prompt);
    { ZStr v6; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::aid, &v6) && v6.eql("14")); }
}

TEST(semantic_prompt, OSC_133_fresh_line_new_prompt_with_in_aid) {
    Parser p; /* .init(null) */
    const char *input = "133;A;aid=a=b";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::fresh_line_new_prompt);
    { ZStr v7; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::aid, &v7) && v7.eql("a=b")); }
}

TEST(semantic_prompt, OSC_133_fresh_line_new_prompt_with_cl_line) {
    Parser p; /* .init(null) */
    const char *input = "133;A;cl=line";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::fresh_line_new_prompt);
    { sp::Click v8; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::cl, &v8) && v8 == sp::Click::line); }
}

TEST(semantic_prompt, OSC_133_fresh_line_new_prompt_with_cl_m) {
    Parser p; /* .init(null) */
    const char *input = "133;A;cl=m";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::fresh_line_new_prompt);
    { sp::Click v9; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::cl, &v9) && v9 == sp::Click::multiple); }
}

TEST(semantic_prompt, OSC_133_fresh_line_new_prompt_with_invalid_cl) {
    Parser p; /* .init(null) */
    const char *input = "133;A;cl=invalid";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::fresh_line_new_prompt);
    { sp::Click v10; ASSERT_FALSE(cmd.semantic_prompt.readOption(sp::Option::cl, &v10)); }
}

TEST(semantic_prompt, OSC_133_fresh_line_new_prompt_with_trailing) {
    Parser p; /* .init(null) */
    const char *input = "133;A;";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::fresh_line_new_prompt);
}

TEST(semantic_prompt, OSC_133_fresh_line_new_prompt_with_bare_key) {
    Parser p; /* .init(null) */
    const char *input = "133;A;barekey";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::fresh_line_new_prompt);
    { ZStr v11; ASSERT_FALSE(cmd.semantic_prompt.readOption(sp::Option::aid, &v11)); }
    { sp::Click v12; ASSERT_FALSE(cmd.semantic_prompt.readOption(sp::Option::cl, &v12)); }
}

TEST(semantic_prompt, OSC_133_fresh_line_new_prompt_with_multiple_options) {
    Parser p; /* .init(null) */
    const char *input = "133;A;aid=foo;cl=line";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::fresh_line_new_prompt);
    { ZStr v13; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::aid, &v13) && v13.eql("foo")); }
    { sp::Click v14; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::cl, &v14) && v14 == sp::Click::line); }
}

TEST(semantic_prompt, OSC_133_fresh_line_new_prompt_default_redraw) {
    Parser p; /* .init(null) */
    const char *input = "133;A";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::fresh_line_new_prompt);
    { sp::Redraw v15; ASSERT_FALSE(cmd.semantic_prompt.readOption(sp::Option::redraw, &v15)); }
}

TEST(semantic_prompt, OSC_133_fresh_line_new_prompt_with_redraw_0) {
    Parser p; /* .init(null) */
    const char *input = "133;A;redraw=0";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::fresh_line_new_prompt);
    { sp::Redraw v16; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::redraw, &v16) && v16 == sp::Redraw::false_); }
}

TEST(semantic_prompt, OSC_133_fresh_line_new_prompt_with_redraw_1) {
    Parser p; /* .init(null) */
    const char *input = "133;A;redraw=1";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::fresh_line_new_prompt);
    { sp::Redraw v17; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::redraw, &v17) && v17 == sp::Redraw::true_); }
}

TEST(semantic_prompt, OSC_133_fresh_line_new_prompt_with_invalid_redraw) {
    Parser p; /* .init(null) */
    const char *input = "133;A;redraw=x";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::fresh_line_new_prompt);
    { sp::Redraw v18; ASSERT_FALSE(cmd.semantic_prompt.readOption(sp::Option::redraw, &v18)); }
}

TEST(semantic_prompt, OSC_133_prompt_start) {
    Parser p; /* .init(null) */
    const char *input = "133;P";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::prompt_start);
    { sp::PromptKind v19; ASSERT_FALSE(cmd.semantic_prompt.readOption(sp::Option::prompt_kind, &v19)); }
}

TEST(semantic_prompt, OSC_133_prompt_start_with_k_i) {
    Parser p; /* .init(null) */
    const char *input = "133;P;k=i";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::prompt_start);
    { sp::PromptKind v20; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::prompt_kind, &v20) && v20 == sp::PromptKind::initial); }
}

TEST(semantic_prompt, OSC_133_prompt_start_with_k_r) {
    Parser p; /* .init(null) */
    const char *input = "133;P;k=r";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::prompt_start);
    { sp::PromptKind v21; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::prompt_kind, &v21) && v21 == sp::PromptKind::right); }
}

TEST(semantic_prompt, OSC_133_prompt_start_with_k_c) {
    Parser p; /* .init(null) */
    const char *input = "133;P;k=c";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::prompt_start);
    { sp::PromptKind v22; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::prompt_kind, &v22) && v22 == sp::PromptKind::continuation); }
}

TEST(semantic_prompt, OSC_133_prompt_start_with_k_s) {
    Parser p; /* .init(null) */
    const char *input = "133;P;k=s";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::prompt_start);
    { sp::PromptKind v23; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::prompt_kind, &v23) && v23 == sp::PromptKind::secondary); }
}

TEST(semantic_prompt, OSC_133_prompt_start_with_invalid_k) {
    Parser p; /* .init(null) */
    const char *input = "133;P;k=x";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::prompt_start);
    { sp::PromptKind v24; ASSERT_FALSE(cmd.semantic_prompt.readOption(sp::Option::prompt_kind, &v24)); }
}

TEST(semantic_prompt, OSC_133_prompt_start_extra_contents) {
    Parser p; /* .init(null) */
    const char *input = "133;Pextra";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    ASSERT_TRUE(p.end() == nullptr);
}

TEST(semantic_prompt, OSC_133_new_command) {
    Parser p; /* .init(null) */
    const char *input = "133;N";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::new_command);
    { ZStr v25; ASSERT_FALSE(cmd.semantic_prompt.readOption(sp::Option::aid, &v25)); }
    { sp::Click v26; ASSERT_FALSE(cmd.semantic_prompt.readOption(sp::Option::cl, &v26)); }
}

TEST(semantic_prompt, OSC_133_new_command_with_aid) {
    Parser p; /* .init(null) */
    const char *input = "133;N;aid=foo";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::new_command);
    { ZStr v27; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::aid, &v27) && v27.eql("foo")); }
}

TEST(semantic_prompt, OSC_133_new_command_with_cl_line) {
    Parser p; /* .init(null) */
    const char *input = "133;N;cl=line";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::new_command);
    { sp::Click v28; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::cl, &v28) && v28 == sp::Click::line); }
}

TEST(semantic_prompt, OSC_133_new_command_with_multiple_options) {
    Parser p; /* .init(null) */
    const char *input = "133;N;aid=foo;cl=line";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::new_command);
    { ZStr v29; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::aid, &v29) && v29.eql("foo")); }
    { sp::Click v30; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::cl, &v30) && v30 == sp::Click::line); }
}

TEST(semantic_prompt, OSC_133_new_command_extra_contents) {
    Parser p; /* .init(null) */
    const char *input = "133;Nextra";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    ASSERT_TRUE(p.end() == nullptr);
}

TEST(semantic_prompt, OSC_133_end_prompt_start_input) {
    Parser p; /* .init(null) */
    const char *input = "133;B";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_prompt_start_input);
}

TEST(semantic_prompt, OSC_133_end_prompt_start_input_extra_contents) {
    Parser p; /* .init(null) */
    const char *input = "133;Bextra";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    ASSERT_TRUE(p.end() == nullptr);
}

TEST(semantic_prompt, OSC_133_end_prompt_start_input_with_options) {
    Parser p; /* .init(null) */
    const char *input = "133;B;aid=foo";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_prompt_start_input);
    { ZStr v31; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::aid, &v31) && v31.eql("foo")); }
}

TEST(semantic_prompt, OSC_133_end_prompt_start_input_terminate_eol) {
    Parser p; /* .init(null) */
    const char *input = "133;I";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_prompt_start_input_terminate_eol);
}

TEST(semantic_prompt, OSC_133_end_prompt_start_input_terminate_eol_extra_contents) {
    Parser p; /* .init(null) */
    const char *input = "133;Iextra";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    ASSERT_TRUE(p.end() == nullptr);
}

TEST(semantic_prompt, OSC_133_end_prompt_start_input_terminate_eol_with_options) {
    Parser p; /* .init(null) */
    const char *input = "133;I;aid=foo";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_prompt_start_input_terminate_eol);
    { ZStr v32; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::aid, &v32) && v32.eql("foo")); }
}

TEST(semantic_prompt, OSC_133_end_command) {
    Parser p; /* .init(null) */
    const char *input = "133;D";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_command);
    { int32_t v33; ASSERT_FALSE(cmd.semantic_prompt.readOption(sp::Option::exit_code, &v33)); }
    { ZStr v34; ASSERT_FALSE(cmd.semantic_prompt.readOption(sp::Option::aid, &v34)); }
    { ZStr v35; ASSERT_FALSE(cmd.semantic_prompt.readOption(sp::Option::err, &v35)); }
}

TEST(semantic_prompt, OSC_133_end_command_extra_contents) {
    Parser p; /* .init(null) */
    const char *input = "133;Dextra";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    ASSERT_TRUE(p.end() == nullptr);
}

TEST(semantic_prompt, OSC_133_end_command_with_exit_code_0) {
    Parser p; /* .init(null) */
    const char *input = "133;D;0";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_command);
    { int32_t v36; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::exit_code, &v36) && v36 == 0); }
}

TEST(semantic_prompt, OSC_133_end_command_with_exit_code_and_aid) {
    Parser p; /* .init(null) */
    const char *input = "133;D;12;aid=foo";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end();
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
    ASSERT_TRUE(cmd.semantic_prompt.action == sp::Command::Action::end_command);
    { ZStr v37; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::aid, &v37) && v37.eql("foo")); }
    { int32_t v38; ASSERT_TRUE(cmd.semantic_prompt.readOption(sp::Option::exit_code, &v38) && v38 == 12); }
}

TEST(semantic_prompt, Option_read_aid) {
    { ZStr v39; ASSERT_TRUE(sp::Option_read(sp::Option::aid, "aid=test123", &v39) && v39.eql("test123")); }
    { ZStr v40; ASSERT_TRUE(sp::Option_read(sp::Option::aid, "cl=line;aid=myaid;k=i", &v40) && v40.eql("myaid")); }
    { ZStr v41; ASSERT_FALSE(sp::Option_read(sp::Option::aid, "cl=line;k=i", &v41)); }
    { ZStr v42; ASSERT_TRUE(sp::Option_read(sp::Option::aid, "aid=", &v42) && v42.eql("")); }
    { ZStr v43; ASSERT_TRUE(sp::Option_read(sp::Option::aid, "k=i;aid=last", &v43) && v43.eql("last")); }
    { ZStr v44; ASSERT_TRUE(sp::Option_read(sp::Option::aid, "aid=first;k=i", &v44) && v44.eql("first")); }
    { ZStr v45; ASSERT_FALSE(sp::Option_read(sp::Option::aid, "", &v45)); }
    { ZStr v46; ASSERT_FALSE(sp::Option_read(sp::Option::aid, "aid", &v46)); }
    { ZStr v47; ASSERT_TRUE(sp::Option_read(sp::Option::aid, ";;aid=value;;", &v47) && v47.eql("value")); }
}

TEST(semantic_prompt, Option_read_cl) {
    { sp::Click v48; ASSERT_TRUE(sp::Option_read(sp::Option::cl, "cl=line", &v48) && v48 == sp::Click::line); }
    { sp::Click v49; ASSERT_TRUE(sp::Option_read(sp::Option::cl, "cl=m", &v49) && v49 == sp::Click::multiple); }
    { sp::Click v50; ASSERT_TRUE(sp::Option_read(sp::Option::cl, "cl=v", &v50) && v50 == sp::Click::conservative_vertical); }
    { sp::Click v51; ASSERT_TRUE(sp::Option_read(sp::Option::cl, "cl=w", &v51) && v51 == sp::Click::smart_vertical); }
    { sp::Click v52; ASSERT_FALSE(sp::Option_read(sp::Option::cl, "cl=invalid", &v52)); }
    { sp::Click v53; ASSERT_FALSE(sp::Option_read(sp::Option::cl, "aid=foo", &v53)); }
}

TEST(semantic_prompt, Option_read_prompt_kind) {
    { sp::PromptKind v54; ASSERT_TRUE(sp::Option_read(sp::Option::prompt_kind, "k=i", &v54) && v54 == sp::PromptKind::initial); }
    { sp::PromptKind v55; ASSERT_TRUE(sp::Option_read(sp::Option::prompt_kind, "k=r", &v55) && v55 == sp::PromptKind::right); }
    { sp::PromptKind v56; ASSERT_TRUE(sp::Option_read(sp::Option::prompt_kind, "k=c", &v56) && v56 == sp::PromptKind::continuation); }
    { sp::PromptKind v57; ASSERT_TRUE(sp::Option_read(sp::Option::prompt_kind, "k=s", &v57) && v57 == sp::PromptKind::secondary); }
    { sp::PromptKind v58; ASSERT_FALSE(sp::Option_read(sp::Option::prompt_kind, "k=x", &v58)); }
    { sp::PromptKind v59; ASSERT_FALSE(sp::Option_read(sp::Option::prompt_kind, "k=ii", &v59)); }
    { sp::PromptKind v60; ASSERT_FALSE(sp::Option_read(sp::Option::prompt_kind, "k=", &v60)); }
}

TEST(semantic_prompt, Option_read_err) {
    { ZStr v61; ASSERT_TRUE(sp::Option_read(sp::Option::err, "err=some_error", &v61) && v61.eql("some_error")); }
    { ZStr v62; ASSERT_FALSE(sp::Option_read(sp::Option::err, "aid=foo", &v62)); }
}

TEST(semantic_prompt, Option_read_redraw) {
    { sp::Redraw v63; ASSERT_TRUE(sp::Option_read(sp::Option::redraw, "redraw=1", &v63) && v63 == sp::Redraw::true_); }
    { sp::Redraw v64; ASSERT_TRUE(sp::Option_read(sp::Option::redraw, "redraw=0", &v64) && v64 == sp::Redraw::false_); }
    { sp::Redraw v65; ASSERT_TRUE(sp::Option_read(sp::Option::redraw, "redraw=last", &v65) && v65 == sp::Redraw::last); }
    { sp::Redraw v66; ASSERT_FALSE(sp::Option_read(sp::Option::redraw, "redraw=2", &v66)); }
    { sp::Redraw v67; ASSERT_FALSE(sp::Option_read(sp::Option::redraw, "redraw=10", &v67)); }
    { sp::Redraw v68; ASSERT_FALSE(sp::Option_read(sp::Option::redraw, "redraw=", &v68)); }
}

TEST(semantic_prompt, Option_read_special_key) {
    { bool v69; ASSERT_TRUE(sp::Option_read(sp::Option::special_key, "special_key=1", &v69) && v69 == true); }
    { bool v70; ASSERT_TRUE(sp::Option_read(sp::Option::special_key, "special_key=0", &v70) && v70 == false); }
    { bool v71; ASSERT_FALSE(sp::Option_read(sp::Option::special_key, "special_key=x", &v71)); }
}

TEST(semantic_prompt, Option_read_click_events) {
    { sp::ClickEvents v72; ASSERT_FALSE(sp::Option_read(sp::Option::click_events, "click_events=yes", &v72)); }
    { sp::ClickEvents v73; ASSERT_FALSE(sp::Option_read(sp::Option::click_events, "click_events=0", &v73)); }
    { sp::ClickEvents v74; ASSERT_TRUE(sp::Option_read(sp::Option::click_events, "click_events=1", &v74) && v74 == sp::ClickEvents::absolute); }
    { sp::ClickEvents v75; ASSERT_TRUE(sp::Option_read(sp::Option::click_events, "click_events=2", &v75) && v75 == sp::ClickEvents::relative); }
}

TEST(semantic_prompt, Option_read_exit_code) {
    { int32_t v76; ASSERT_TRUE(sp::Option_read(sp::Option::exit_code, "42", &v76) && v76 == 42); }
    { int32_t v77; ASSERT_TRUE(sp::Option_read(sp::Option::exit_code, "0", &v77) && v77 == 0); }
    { int32_t v78; ASSERT_TRUE(sp::Option_read(sp::Option::exit_code, "-1", &v78) && v78 == -1); }
    { int32_t v79; ASSERT_FALSE(sp::Option_read(sp::Option::exit_code, "abc", &v79)); }
    { int32_t v80; ASSERT_TRUE(sp::Option_read(sp::Option::exit_code, "127;aid=foo", &v80) && v80 == 127); }
}

/* osc9.zig */

TEST(osc9, OSC_9_show_desktop_notification) {
    Parser p; /* .init(null) */
    const char *input = "9;Hello world";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd.show_desktop_notification.title.eql(""));
    ASSERT_TRUE(cmd.show_desktop_notification.body.eql("Hello world"));
}

TEST(osc9, OSC_9_show_single_character_desktop_notification) {
    Parser p; /* .init(null) */
    const char *input = "9;H";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd.show_desktop_notification.title.eql(""));
    ASSERT_TRUE(cmd.show_desktop_notification.body.eql("H"));
}

TEST(osc9, OSC_9_1_ConEmu_sleep) {
    Parser p; /* .init(null) */
    const char *input = "9;1;420";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_sleep);
    ASSERT_TRUE(cmd.conemu_sleep.duration_ms == 420);
}

TEST(osc9, OSC_9_1_ConEmu_sleep_with_no_value_default_to_100ms) {
    Parser p; /* .init(null) */
    const char *input = "9;1;";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_sleep);
    ASSERT_TRUE(cmd.conemu_sleep.duration_ms == 100);
}

TEST(osc9, OSC_9_1_conemu_sleep_cannot_exceed_10000ms) {
    Parser p; /* .init(null) */
    const char *input = "9;1;12345";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_sleep);
    ASSERT_TRUE(cmd.conemu_sleep.duration_ms == 10000);
}

TEST(osc9, OSC_9_1_conemu_sleep_invalid_input) {
    Parser p; /* .init(null) */
    const char *input = "9;1;foo";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_sleep);
    ASSERT_TRUE(cmd.conemu_sleep.duration_ms == 100);
}

TEST(osc9, OSC_9_1_conemu_sleep_desktop_notification_1) {
    Parser p; /* .init(null) */
    const char *input = "9;1";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd.show_desktop_notification.body.eql("1"));
}

TEST(osc9, OSC_9_1_conemu_sleep_desktop_notification_2) {
    Parser p; /* .init(null) */
    const char *input = "9;1a";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd.show_desktop_notification.body.eql("1a"));
}

TEST(osc9, OSC_9_2_ConEmu_message_box) {
    Parser p; /* .init(null) */
    const char *input = "9;2;hello world";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_show_message_box);
    ASSERT_TRUE(cmd.conemu_show_message_box.eql("hello world"));
}

TEST(osc9, OSC_9_2_ConEmu_message_box_invalid_input) {
    Parser p; /* .init(null) */
    const char *input = "9;2";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd.show_desktop_notification.body.eql("2"));
}

TEST(osc9, OSC_9_2_ConEmu_message_box_empty_message) {
    Parser p; /* .init(null) */
    const char *input = "9;2;";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_show_message_box);
    ASSERT_TRUE(cmd.conemu_show_message_box.eql(""));
}

TEST(osc9, OSC_9_2_ConEmu_message_box_spaces_only_message) {
    Parser p; /* .init(null) */
    const char *input = "9;2;   ";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_show_message_box);
    ASSERT_TRUE(cmd.conemu_show_message_box.eql("   "));
}

TEST(osc9, OSC_9_2_message_box_desktop_notification_1) {
    Parser p; /* .init(null) */
    const char *input = "9;2";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd.show_desktop_notification.body.eql("2"));
}

TEST(osc9, OSC_9_2_message_box_desktop_notification_2) {
    Parser p; /* .init(null) */
    const char *input = "9;2a";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd.show_desktop_notification.body.eql("2a"));
}

TEST(osc9, OSC_9_3_ConEmu_change_tab_title) {
    Parser p; /* .init(null) */
    const char *input = "9;3;foo bar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_change_tab_title);
    ASSERT_TRUE(cmd.conemu_change_tab_title.value.eql("foo bar"));
}

TEST(osc9, OSC_9_3_ConEmu_change_tab_title_reset) {
    Parser p; /* .init(null) */
    const char *input = "9;3;";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_change_tab_title);
    ASSERT_TRUE(cmd.conemu_change_tab_title.tag == decltype(cmd.conemu_change_tab_title)::Tag::reset);
}

TEST(osc9, OSC_9_3_ConEmu_change_tab_title_spaces_only) {
    Parser p; /* .init(null) */
    const char *input = "9;3;   ";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_change_tab_title);
    ASSERT_TRUE(cmd.conemu_change_tab_title.value.eql("   "));
}

TEST(osc9, OSC_9_3_change_tab_title_desktop_notification_1) {
    Parser p; /* .init(null) */
    const char *input = "9;3";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd.show_desktop_notification.body.eql("3"));
}

TEST(osc9, OSC_9_3_message_box_desktop_notification_2) {
    Parser p; /* .init(null) */
    const char *input = "9;3a";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd.show_desktop_notification.body.eql("3a"));
}

TEST(osc9, OSC_9_4_ConEmu_progress_set) {
    Parser p; /* .init(null) */
    const char *input = "9;4;1;100";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_progress_report);
    ASSERT_TRUE(cmd.conemu_progress_report.state == ProgressReport::State::set);
    ASSERT_TRUE(cmd.conemu_progress_report.has_progress && cmd.conemu_progress_report.progress == 100);
}

TEST(osc9, OSC_9_4_ConEmu_progress_set_overflow) {
    Parser p; /* .init(null) */
    const char *input = "9;4;1;900";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_progress_report);
    ASSERT_TRUE(cmd.conemu_progress_report.state == ProgressReport::State::set);
    ASSERT_TRUE(cmd.conemu_progress_report.has_progress && cmd.conemu_progress_report.progress == 100);
}

TEST(osc9, OSC_9_4_ConEmu_progress_set_single_digit) {
    Parser p; /* .init(null) */
    const char *input = "9;4;1;9";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_progress_report);
    ASSERT_TRUE(cmd.conemu_progress_report.state == ProgressReport::State::set);
    ASSERT_TRUE(cmd.conemu_progress_report.has_progress && cmd.conemu_progress_report.progress == 9);
}

TEST(osc9, OSC_9_4_ConEmu_progress_set_double_digit) {
    Parser p; /* .init(null) */
    const char *input = "9;4;1;94";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_progress_report);
    ASSERT_TRUE(cmd.conemu_progress_report.state == ProgressReport::State::set);
    ASSERT_TRUE(cmd.conemu_progress_report.has_progress && cmd.conemu_progress_report.progress == 94);
}

TEST(osc9, OSC_9_4_ConEmu_progress_set_extra_semicolon_ignored) {
    Parser p; /* .init(null) */
    const char *input = "9;4;1;100";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_progress_report);
    ASSERT_TRUE(cmd.conemu_progress_report.state == ProgressReport::State::set);
    ASSERT_TRUE(cmd.conemu_progress_report.has_progress && cmd.conemu_progress_report.progress == 100);
}

TEST(osc9, OSC_9_4_ConEmu_progress_remove_with_no_progress) {
    Parser p; /* .init(null) */
    const char *input = "9;4;0;";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_progress_report);
    ASSERT_TRUE(cmd.conemu_progress_report.state == ProgressReport::State::remove);
    ASSERT_FALSE(cmd.conemu_progress_report.has_progress);
}

TEST(osc9, OSC_9_4_ConEmu_progress_remove_with_double_semicolon) {
    Parser p; /* .init(null) */
    const char *input = "9;4;0;;";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_progress_report);
    ASSERT_TRUE(cmd.conemu_progress_report.state == ProgressReport::State::remove);
    ASSERT_FALSE(cmd.conemu_progress_report.has_progress);
}

TEST(osc9, OSC_9_4_ConEmu_progress_remove_ignores_progress) {
    Parser p; /* .init(null) */
    const char *input = "9;4;0;100";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_progress_report);
    ASSERT_TRUE(cmd.conemu_progress_report.state == ProgressReport::State::remove);
    ASSERT_FALSE(cmd.conemu_progress_report.has_progress);
}

TEST(osc9, OSC_9_4_ConEmu_progress_remove_extra_semicolon) {
    Parser p; /* .init(null) */
    const char *input = "9;4;0;100;";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_progress_report);
    ASSERT_TRUE(cmd.conemu_progress_report.state == ProgressReport::State::remove);
}

TEST(osc9, OSC_9_4_ConEmu_progress_error) {
    Parser p; /* .init(null) */
    const char *input = "9;4;2";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_progress_report);
    ASSERT_TRUE(cmd.conemu_progress_report.state == ProgressReport::State::error);
    ASSERT_FALSE(cmd.conemu_progress_report.has_progress);
}

TEST(osc9, OSC_9_4_ConEmu_progress_error_with_progress) {
    Parser p; /* .init(null) */
    const char *input = "9;4;2;100";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_progress_report);
    ASSERT_TRUE(cmd.conemu_progress_report.state == ProgressReport::State::error);
    ASSERT_TRUE(cmd.conemu_progress_report.has_progress && cmd.conemu_progress_report.progress == 100);
}

TEST(osc9, OSC_9_4_progress_pause) {
    Parser p; /* .init(null) */
    const char *input = "9;4;4";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_progress_report);
    ASSERT_TRUE(cmd.conemu_progress_report.state == ProgressReport::State::pause);
    ASSERT_FALSE(cmd.conemu_progress_report.has_progress);
}

TEST(osc9, OSC_9_4_ConEmu_progress_pause_with_progress) {
    Parser p; /* .init(null) */
    const char *input = "9;4;4;100";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_progress_report);
    ASSERT_TRUE(cmd.conemu_progress_report.state == ProgressReport::State::pause);
    ASSERT_TRUE(cmd.conemu_progress_report.has_progress && cmd.conemu_progress_report.progress == 100);
}

TEST(osc9, OSC_9_4_progress_desktop_notification_1) {
    Parser p; /* .init(null) */
    const char *input = "9;4";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd.show_desktop_notification.body.eql("4"));
}

TEST(osc9, OSC_9_4_progress_desktop_notification_2) {
    Parser p; /* .init(null) */
    const char *input = "9;4;";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd.show_desktop_notification.body.eql("4;"));
}

TEST(osc9, OSC_9_4_progress_desktop_notification_3) {
    Parser p; /* .init(null) */
    const char *input = "9;4;5";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd.show_desktop_notification.body.eql("4;5"));
}

TEST(osc9, OSC_9_4_progress_desktop_notification_4) {
    Parser p; /* .init(null) */
    const char *input = "9;4;5a";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd.show_desktop_notification.body.eql("4;5a"));
}

TEST(osc9, OSC_9_5_ConEmu_wait_input) {
    Parser p; /* .init(null) */
    const char *input = "9;5";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_wait_input);
}

TEST(osc9, OSC_9_5_ConEmu_wait_ignores_trailing_characters) {
    Parser p; /* .init(null) */
    const char *input = "9;5;foo";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_wait_input);
}

TEST(osc9, OSC_9_6_ConEmu_guimacro_1) {
    Parser p(true);
    const char *input = "9;6;a";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_guimacro);
    ASSERT_TRUE(cmd.conemu_guimacro.eql("a"));
}

TEST(osc9, OSC_9_6_ConEmu_guimacro_2) {
    Parser p(true);
    const char *input = "9;6;ab";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_guimacro);
    ASSERT_TRUE(cmd.conemu_guimacro.eql("ab"));
}

TEST(osc9, OSC_9_6_ConEmu_guimacro_3_incomplete_desktop_notification) {
    Parser p(true);
    const char *input = "9;6";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd.show_desktop_notification.body.eql("6"));
}

TEST(osc9, OSC_9_7_ConEmu_run_process_1) {
    Parser p(true);
    const char *input = "9;7;ab";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_run_process);
    ASSERT_TRUE(cmd.conemu_run_process.eql("ab"));
}

TEST(osc9, OSC_9_7_ConEmu_run_process_2) {
    Parser p(true);
    const char *input = "9;7;";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_run_process);
    ASSERT_TRUE(cmd.conemu_run_process.eql(""));
}

TEST(osc9, OSC_9_7_ConEmu_run_process_incomplete_desktop_notification) {
    Parser p(true);
    const char *input = "9;7";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd.show_desktop_notification.body.eql("7"));
}

TEST(osc9, OSC_9_8_ConEmu_output_environment_variable_1) {
    Parser p(true);
    const char *input = "9;8;ab";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_output_environment_variable);
    ASSERT_TRUE(cmd.conemu_output_environment_variable.eql("ab"));
}

TEST(osc9, OSC_9_8_ConEmu_output_environment_variable_2) {
    Parser p(true);
    const char *input = "9;8;";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_output_environment_variable);
    ASSERT_TRUE(cmd.conemu_output_environment_variable.eql(""));
}

TEST(osc9, OSC_9_8_ConEmu_output_environment_variable_incomplete_desktop_notification) {
    Parser p(true);
    const char *input = "9;8";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd.show_desktop_notification.body.eql("8"));
}

TEST(osc9, OSC_9_9_ConEmu_set_current_working_directory) {
    Parser p(true);
    const char *input = "9;9;ab";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::report_pwd);
    ASSERT_TRUE(cmd.report_pwd.value.eql("ab"));
}

TEST(osc9, OSC_9_9_ConEmu_set_current_working_directory_incomplete_desktop_notification) {
    Parser p(true);
    const char *input = "9;9";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd.show_desktop_notification.body.eql("9"));
}

TEST(osc9, OSC_9_10_ConEmu_xterm_keyboard_and_output_emulation_1) {
    Parser p(true);
    const char *input = "9;10";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_xterm_emulation);
    ASSERT_TRUE(cmd.conemu_xterm_emulation.has_keyboard);
    ASSERT_TRUE(cmd.conemu_xterm_emulation.has_keyboard && cmd.conemu_xterm_emulation.keyboard == true);
    ASSERT_TRUE(cmd.conemu_xterm_emulation.has_output);
    ASSERT_TRUE(cmd.conemu_xterm_emulation.has_output && cmd.conemu_xterm_emulation.output == true);
}

TEST(osc9, OSC_9_10_ConEmu_xterm_keyboard_and_output_emulation_2) {
    Parser p(true);
    const char *input = "9;10;0";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_xterm_emulation);
    ASSERT_TRUE(cmd.conemu_xterm_emulation.has_keyboard);
    ASSERT_TRUE(cmd.conemu_xterm_emulation.has_keyboard && cmd.conemu_xterm_emulation.keyboard == false);
    ASSERT_TRUE(cmd.conemu_xterm_emulation.has_output);
    ASSERT_TRUE(cmd.conemu_xterm_emulation.has_output && cmd.conemu_xterm_emulation.output == false);
}

TEST(osc9, OSC_9_10_ConEmu_xterm_keyboard_and_output_emulation_3) {
    Parser p(true);
    const char *input = "9;10;1";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_xterm_emulation);
    ASSERT_TRUE(cmd.conemu_xterm_emulation.has_keyboard);
    ASSERT_TRUE(cmd.conemu_xterm_emulation.has_keyboard && cmd.conemu_xterm_emulation.keyboard == true);
    ASSERT_TRUE(cmd.conemu_xterm_emulation.has_output);
    ASSERT_TRUE(cmd.conemu_xterm_emulation.has_output && cmd.conemu_xterm_emulation.output == true);
}

TEST(osc9, OSC_9_10_ConEmu_xterm_keyboard_and_output_emulation_4) {
    Parser p(true);
    const char *input = "9;10;2";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_xterm_emulation);
    ASSERT_FALSE(cmd.conemu_xterm_emulation.has_keyboard);
    ASSERT_TRUE(cmd.conemu_xterm_emulation.has_output);
    ASSERT_TRUE(cmd.conemu_xterm_emulation.has_output && cmd.conemu_xterm_emulation.output == false);
}

TEST(osc9, OSC_9_10_ConEmu_xterm_keyboard_and_output_emulation_5) {
    Parser p(true);
    const char *input = "9;10;3";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_xterm_emulation);
    ASSERT_FALSE(cmd.conemu_xterm_emulation.has_keyboard);
    ASSERT_TRUE(cmd.conemu_xterm_emulation.has_output);
    ASSERT_TRUE(cmd.conemu_xterm_emulation.has_output && cmd.conemu_xterm_emulation.output == true);
}

TEST(osc9, OSC_9_10_ConEmu_xterm_keyboard_and_output_emulation_6) {
    Parser p(true);
    const char *input = "9;10;4";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd.show_desktop_notification.body.eql("10;4"));
}

TEST(osc9, OSC_9_10_ConEmu_xterm_keyboard_and_output_emulation_7) {
    Parser p(true);
    const char *input = "9;10;";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd.show_desktop_notification.body.eql("10;"));
}

TEST(osc9, OSC_9_10_ConEmu_xterm_keyboard_and_output_emulation_8) {
    Parser p(true);
    const char *input = "9;10;abc";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd.show_desktop_notification.body.eql("10;abc"));
}

TEST(osc9, OSC_9_11_ConEmu_comment) {
    Parser p(true);
    const char *input = "9;11;ab";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::conemu_comment);
    ASSERT_TRUE(cmd.conemu_comment.eql("ab"));
}

TEST(osc9, OSC_9_11_ConEmu_comment_incomplete_desktop_notification) {
    Parser p(true);
    const char *input = "9;11";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::show_desktop_notification);
    ASSERT_TRUE(cmd.show_desktop_notification.body.eql("11"));
}

TEST(osc9, OSC_9_12_ConEmu_mark_prompt_start_1) {
    Parser p(true);
    const char *input = "9;12";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
}

TEST(osc9, OSC_9_12_ConEmu_mark_prompt_start_2) {
    Parser p(true);
    const char *input = "9;12;abc";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::semantic_prompt);
}

/* kitty_desktop_notification.zig */

namespace kdn = wisp::terminal::osc::kitty_desktop_notification;

TEST(kitty_desktop_notification, OSC_99_empty_metadata_and_payload) {
    Parser p; /* .init(null) */
    const char *input = "99;;";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.metadata.eql(""));
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql(""));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readA().eql(kdn::Action::default_()));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readBool(kdn::Option::c) == false);
    ASSERT_TRUE(cmd.kitty_desktop_notification.readBool(kdn::Option::d) == true);
    ASSERT_TRUE(cmd.kitty_desktop_notification.readBool(kdn::Option::e) == false);
    { ZStr v; ASSERT_FALSE(cmd.kitty_desktop_notification.readOptional(kdn::Option::f, &v)); }
    { ZStr v; ASSERT_FALSE(cmd.kitty_desktop_notification.readOptional(kdn::Option::g, &v)); }
    { ZStr v; ASSERT_FALSE(cmd.kitty_desktop_notification.readOptional(kdn::Option::i, &v)); }
    {
        kitty_metadata::ValueIterator it = cmd.kitty_desktop_notification.readIterator(kdn::Option::n);
        ZStr v;
        ASSERT_FALSE(it.next(&v));
    }
    ASSERT_TRUE(cmd.kitty_desktop_notification.readO() == kdn::Occasion::always);
    ASSERT_TRUE(cmd.kitty_desktop_notification.readP() == kdn::Payload::title);
    ASSERT_TRUE(cmd.kitty_desktop_notification.readS().eql("system"));
    {
        kitty_metadata::ValueIterator it = cmd.kitty_desktop_notification.readIterator(kdn::Option::t);
        ZStr v;
        ASSERT_FALSE(it.next(&v));
    }
    ASSERT_TRUE(cmd.kitty_desktop_notification.readU() == kdn::Urgency::normal);
    ASSERT_TRUE(cmd.kitty_desktop_notification.readW() == -1);
    ASSERT_TRUE(cmd.kitty_desktop_notification.terminator == Terminator::st);
}

TEST(kitty_desktop_notification, OSC_99_empty_metadata_with_payload) {
    Parser p; /* .init(null) */
    const char *input = "99;;bobr";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.metadata.eql(""));
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("bobr"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readA().eql(kdn::Action::default_()));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readBool(kdn::Option::c) == false);
    ASSERT_TRUE(cmd.kitty_desktop_notification.readBool(kdn::Option::d) == true);
    ASSERT_TRUE(cmd.kitty_desktop_notification.readBool(kdn::Option::e) == false);
    { ZStr v; ASSERT_FALSE(cmd.kitty_desktop_notification.readOptional(kdn::Option::f, &v)); }
    { ZStr v; ASSERT_FALSE(cmd.kitty_desktop_notification.readOptional(kdn::Option::g, &v)); }
    { ZStr v; ASSERT_FALSE(cmd.kitty_desktop_notification.readOptional(kdn::Option::i, &v)); }
    {
        kitty_metadata::ValueIterator it = cmd.kitty_desktop_notification.readIterator(kdn::Option::n);
        ZStr v;
        ASSERT_FALSE(it.next(&v));
    }
    ASSERT_TRUE(cmd.kitty_desktop_notification.readO() == kdn::Occasion::always);
    ASSERT_TRUE(cmd.kitty_desktop_notification.readP() == kdn::Payload::title);
    ASSERT_TRUE(cmd.kitty_desktop_notification.readS().eql("system"));
    {
        kitty_metadata::ValueIterator it = cmd.kitty_desktop_notification.readIterator(kdn::Option::t);
        ZStr v;
        ASSERT_FALSE(it.next(&v));
    }
    ASSERT_TRUE(cmd.kitty_desktop_notification.readU() == kdn::Urgency::normal);
    ASSERT_TRUE(cmd.kitty_desktop_notification.readW() == -1);
}

TEST(kitty_desktop_notification, OSC_99_payload_size_limits) {
    struct Case {
        const char *metadata;
        size_t payload_size;
        bool valid;
    };
    const Case cases[] = {
        { "", kdn::MAX_PLAIN_PAYLOAD_BYTES, true },
        { "", kdn::MAX_PLAIN_PAYLOAD_BYTES + 1, false },
        { "e=1", kdn::MAX_ENCODED_PAYLOAD_BYTES, true },
        { "e=1", kdn::MAX_ENCODED_PAYLOAD_BYTES + 1, false },
    };

    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        const Case &c = cases[k];
        Parser p(true);

        for (const char *ch = "99;"; *ch; ch++) p.next((uint8_t)*ch);
        for (const char *ch = c.metadata; *ch; ch++) p.next((uint8_t)*ch);
        p.next(';');
        for (size_t i = 0; i < c.payload_size; i++) p.next('a');

        ASSERT_TRUE(c.valid == (p.end('\x1b') != nullptr));
    }
}

TEST(kitty_desktop_notification, OSC_99_unknown_prefix_does_not_hide_dotted_identifier) {
    Parser p; /* .init(null) */
    const char *input = "99;invalid=wrong:i=org.ghostty;payload";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);

    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ZStr v;
    ASSERT_TRUE(cmd.kitty_desktop_notification.readOptional(kdn::Option::i, &v) &&
                v.eql("org.ghostty"));
}

TEST(kitty_desktop_notification, OSC_99_single_parameter_i) {
    Parser p; /* .init(null) */
    const char *input = "99;i=bobr;payload";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    { ZStr v; ASSERT_TRUE(cmd.kitty_desktop_notification.readOptional(kdn::Option::i, &v) && v.eql("bobr")); }
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("payload"));
}

TEST(kitty_desktop_notification, OSC_99_repeated_parameter_i) {
    Parser p; /* .init(null) */
    const char *input = "99;i=bobr:i=foobar;payload";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    { ZStr v; ASSERT_TRUE(cmd.kitty_desktop_notification.readOptional(kdn::Option::i, &v) && v.eql("bobr")); }
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("payload"));
}

TEST(kitty_desktop_notification, OSC_99_multiple_types) {
    Parser p; /* .init(null) */
    const char *input = "99;t=mail: t = chat : t = alert ;notification";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("notification"));
    kitty_metadata::ValueIterator it = cmd.kitty_desktop_notification.readIterator(kdn::Option::t);
    ZStr v;
    ASSERT_TRUE(it.next(&v) && v.eql("mail"));
    ASSERT_TRUE(it.next(&v) && v.eql("chat"));
    ASSERT_TRUE(it.next(&v) && v.eql("alert"));
    ASSERT_FALSE(it.next(&v));
}

TEST(kitty_desktop_notification, OSC_99_a_1) {
    Parser p; /* .init(null) */
    const char *input = "99;a=report,focus;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readA().eql(kdn::Action::make(true, true)));
}

TEST(kitty_desktop_notification, OSC_99_a_2) {
    Parser p; /* .init(null) */
    const char *input = "99;a=report,-focus;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readA().eql(kdn::Action::make(false, true)));
}

TEST(kitty_desktop_notification, OSC_99_a_3) {
    Parser p; /* .init(null) */
    const char *input = "99;a=-report,focus;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readA().eql(kdn::Action::make(true, false)));
}

TEST(kitty_desktop_notification, OSC_99_a_4) {
    Parser p; /* .init(null) */
    const char *input = "99;a=-report,-focus;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readA().eql(kdn::Action::make(false, false)));
}

TEST(kitty_desktop_notification, OSC_99_c_1) {
    Parser p; /* .init(null) */
    const char *input = "99;c=0;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readBool(kdn::Option::c) == false);
}

TEST(kitty_desktop_notification, OSC_99_c_2) {
    Parser p; /* .init(null) */
    const char *input = "99;c=1;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readBool(kdn::Option::c) == true);
}

TEST(kitty_desktop_notification, OSC_99_c_3) {
    Parser p; /* .init(null) */
    const char *input = "99;c=bobr;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readBool(kdn::Option::c) == false);
}

TEST(kitty_desktop_notification, OSC_99_d_1) {
    Parser p; /* .init(null) */
    const char *input = "99;d=0;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readBool(kdn::Option::d) == false);
}

TEST(kitty_desktop_notification, OSC_99_d_2) {
    Parser p; /* .init(null) */
    const char *input = "99;d=1;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readBool(kdn::Option::d) == true);
}

TEST(kitty_desktop_notification, OSC_99_d_3) {
    Parser p; /* .init(null) */
    const char *input = "99;d=bobr;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readBool(kdn::Option::d) == true);
}

TEST(kitty_desktop_notification, OSC_99_e_1) {
    Parser p; /* .init(null) */
    const char *input = "99;e=0;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readBool(kdn::Option::e) == false);
}

TEST(kitty_desktop_notification, OSC_99_e_2) {
    Parser p; /* .init(null) */
    const char *input = "99;e=1;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readBool(kdn::Option::e) == true);
}

TEST(kitty_desktop_notification, OSC_99_e_3) {
    Parser p; /* .init(null) */
    const char *input = "99;e=bobr;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readBool(kdn::Option::e) == false);
}

TEST(kitty_desktop_notification, OSC_99_f_1) {
    Parser p; /* .init(null) */
    const char *input = "99;f=R2hvc3R0eQ==;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    { ZStr v; ASSERT_TRUE(cmd.kitty_desktop_notification.readOptional(kdn::Option::f, &v) && v.eql("R2hvc3R0eQ==")); }
}

TEST(kitty_desktop_notification, OSC_99_f_2) {
    Parser p; /* .init(null) */
    const char *input = "99;c=0:f= R2hvc3R0eQ== ;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    { ZStr v; ASSERT_TRUE(cmd.kitty_desktop_notification.readOptional(kdn::Option::f, &v) && v.eql("R2hvc3R0eQ==")); }
}

TEST(kitty_desktop_notification, OSC_99_g_1) {
    Parser p; /* .init(null) */
    const char *input = "99;c=0:g=7f8a9129-a35d-4e9f-8043-ce2700e15e2c;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    { ZStr v; ASSERT_TRUE(cmd.kitty_desktop_notification.readOptional(kdn::Option::g, &v) && v.eql("7f8a9129-a35d-4e9f-8043-ce2700e15e2c")); }
}

TEST(kitty_desktop_notification, OSC_99_g_2) {
    Parser p; /* .init(null) */
    const char *input = "99;c=0:g=aaa*bbb;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    { ZStr v; ASSERT_FALSE(cmd.kitty_desktop_notification.readOptional(kdn::Option::g, &v)); }
}

TEST(kitty_desktop_notification, OSC_99_i_1) {
    Parser p; /* .init(null) */
    const char *input = "99;;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    { ZStr v; ASSERT_FALSE(cmd.kitty_desktop_notification.readOptional(kdn::Option::i, &v)); }
}

TEST(kitty_desktop_notification, OSC_99_i_2) {
    Parser p; /* .init(null) */
    const char *input = "99;i=bobr;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    { ZStr v; ASSERT_TRUE(cmd.kitty_desktop_notification.readOptional(kdn::Option::i, &v) && v.eql("bobr")); }
}

TEST(kitty_desktop_notification, OSC_99_i_3) {
    Parser p; /* .init(null) */
    const char *input = "99;i=;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    { ZStr v; ASSERT_TRUE(cmd.kitty_desktop_notification.readOptional(kdn::Option::i, &v) && v.eql("")); }
}

TEST(kitty_desktop_notification, OSC_99_i_4) {
    Parser p; /* .init(null) */
    const char *input = "99;i= :;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    { ZStr v; ASSERT_TRUE(cmd.kitty_desktop_notification.readOptional(kdn::Option::i, &v) && v.eql("")); }
}

TEST(kitty_desktop_notification, OSC_99_i_5) {
    Parser p; /* .init(null) */
    const char *input = "99;i= bobr ;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    { ZStr v; ASSERT_TRUE(cmd.kitty_desktop_notification.readOptional(kdn::Option::i, &v) && v.eql("bobr")); }
}

TEST(kitty_desktop_notification, OSC_99_i_6) {
    Parser p; /* .init(null) */
    const char *input = "99;i= bobr : i=kurwa ;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    { ZStr v; ASSERT_TRUE(cmd.kitty_desktop_notification.readOptional(kdn::Option::i, &v) && v.eql("bobr")); }
}

TEST(kitty_desktop_notification, OSC_99_n_1) {
    Parser p; /* .init(null) */
    const char *input = "99;n=R2hvc3R0eQ==;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    kitty_metadata::ValueIterator it = cmd.kitty_desktop_notification.readIterator(kdn::Option::n);
    ZStr v;
    ASSERT_TRUE(it.next(&v) && v.eql("R2hvc3R0eQ=="));
    ASSERT_FALSE(it.next(&v));
}

TEST(kitty_desktop_notification, OSC_99_n_2) {
    Parser p; /* .init(null) */
    const char *input = "99;n=R2hvc3R0eQ==:n=R2hvc3R0eQ==;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    kitty_metadata::ValueIterator it = cmd.kitty_desktop_notification.readIterator(kdn::Option::n);
    ZStr v;
    ASSERT_TRUE(it.next(&v) && v.eql("R2hvc3R0eQ=="));
    ASSERT_TRUE(it.next(&v) && v.eql("R2hvc3R0eQ=="));
    ASSERT_FALSE(it.next(&v));
}

TEST(kitty_desktop_notification, OSC_99_o_1) {
    Parser p; /* .init(null) */
    const char *input = "99;o= ;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readO() == kdn::Occasion::always);
}

TEST(kitty_desktop_notification, OSC_99_o_2) {
    Parser p; /* .init(null) */
    const char *input = "99;o=always;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readO() == kdn::Occasion::always);
}

TEST(kitty_desktop_notification, OSC_99_o_3) {
    Parser p; /* .init(null) */
    const char *input = "99;o=unfocused;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readO() == kdn::Occasion::unfocused);
}

TEST(kitty_desktop_notification, OSC_99_o_4) {
    Parser p; /* .init(null) */
    const char *input = "99;o=invisible;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readO() == kdn::Occasion::invisible);
}

TEST(kitty_desktop_notification, OSC_99_o_5) {
    Parser p; /* .init(null) */
    const char *input = "99;o=bobr;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readO() == kdn::Occasion::always);
}

TEST(kitty_desktop_notification, OSC_99_p_1) {
    Parser p; /* .init(null) */
    const char *input = "99;p=alive;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readP() == kdn::Payload::alive);
}

TEST(kitty_desktop_notification, OSC_99_p_2) {
    Parser p; /* .init(null) */
    const char *input = "99;p=body;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readP() == kdn::Payload::body);
}

TEST(kitty_desktop_notification, OSC_99_p_3) {
    Parser p; /* .init(null) */
    const char *input = "99;p=buttons;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readP() == kdn::Payload::buttons);
}

TEST(kitty_desktop_notification, OSC_99_p_4) {
    Parser p; /* .init(null) */
    const char *input = "99;p=close;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readP() == kdn::Payload::close);
}

TEST(kitty_desktop_notification, OSC_99_p_5) {
    Parser p; /* .init(null) */
    const char *input = "99;p=icon;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readP() == kdn::Payload::icon);
}

TEST(kitty_desktop_notification, OSC_99_p_6) {
    Parser p; /* .init(null) */
    const char *input = "99;p=?;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readP() == kdn::Payload::query);
}

TEST(kitty_desktop_notification, OSC_99_p_7) {
    Parser p; /* .init(null) */
    const char *input = "99;p=title;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readP() == kdn::Payload::title);
}

TEST(kitty_desktop_notification, OSC_99_p_8) {
    Parser p; /* .init(null) */
    const char *input = "99;p=query;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readP() == kdn::Payload::unknown);
}

TEST(kitty_desktop_notification, OSC_99_p_9) {
    Parser p; /* .init(null) */
    const char *input = "99;p=bobr;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readP() == kdn::Payload::unknown);
}

TEST(kitty_desktop_notification, OSC_99_s_1) {
    Parser p; /* .init(null) */
    const char *input = "99;s=R2hvc3R0eQ==;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readS().eql("R2hvc3R0eQ=="));
}

TEST(kitty_desktop_notification, OSC_99_t_1) {
    Parser p; /* .init(null) */
    const char *input = "99;t=R2hvc3R0eQ==;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    kitty_metadata::ValueIterator it = cmd.kitty_desktop_notification.readIterator(kdn::Option::t);
    ZStr v;
    ASSERT_TRUE(it.next(&v) && v.eql("R2hvc3R0eQ=="));
    ASSERT_FALSE(it.next(&v));
}

TEST(kitty_desktop_notification, OSC_99_t_2) {
    Parser p; /* .init(null) */
    const char *input = "99;t=R2hvc3R0eQ==:t=R2hvc3R0eQ==;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    kitty_metadata::ValueIterator it = cmd.kitty_desktop_notification.readIterator(kdn::Option::t);
    ZStr v;
    ASSERT_TRUE(it.next(&v) && v.eql("R2hvc3R0eQ=="));
    ASSERT_TRUE(it.next(&v) && v.eql("R2hvc3R0eQ=="));
    ASSERT_FALSE(it.next(&v));
}

TEST(kitty_desktop_notification, OSC_99_u_1) {
    Parser p; /* .init(null) */
    const char *input = "99;u=0;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readU() == kdn::Urgency::low);
}

TEST(kitty_desktop_notification, OSC_99_u_2) {
    Parser p; /* .init(null) */
    const char *input = "99;u=1;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readU() == kdn::Urgency::normal);
}

TEST(kitty_desktop_notification, OSC_99_u_3) {
    Parser p; /* .init(null) */
    const char *input = "99;u=2;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readU() == kdn::Urgency::high);
}

TEST(kitty_desktop_notification, OSC_99_u_4) {
    Parser p; /* .init(null) */
    const char *input = "99;u=bobr;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readU() == kdn::Urgency::normal);
}

TEST(kitty_desktop_notification, OSC_99_w_1) {
    Parser p; /* .init(null) */
    const char *input = "99;w=0;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readW() == 0);
}

TEST(kitty_desktop_notification, OSC_99_w_2) {
    Parser p; /* .init(null) */
    const char *input = "99;w=-1;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readW() == -1);
}

TEST(kitty_desktop_notification, OSC_99_w_3) {
    Parser p; /* .init(null) */
    const char *input = "99;w=-42;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readW() == -1);
}

TEST(kitty_desktop_notification, OSC_99_w_4) {
    Parser p; /* .init(null) */
    const char *input = "99;w=4294967296;foobar";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);
    Command *cmdp = p.end('\x1b');
    ASSERT_TRUE(cmdp != nullptr);
    const Command &cmd = *cmdp;
    ASSERT_TRUE(cmd.key == Command::Key::kitty_desktop_notification);
    ASSERT_TRUE(cmd.kitty_desktop_notification.payload.eql("foobar"));
    ASSERT_TRUE(cmd.kitty_desktop_notification.readW() == -1);
}
