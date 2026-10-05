/* Transliterated from the test blocks in Ghostty src/terminal/apc.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Wisp: the tests gated on build_options.kitty_graphics or
 * build_options.glyph_protocol are not ported, matching the build
 * configuration of apc.hpp.
 */

#include <string.h>

#include <string>

#include "test_helpers.h"
#include "../terminal/apc.hpp"

using namespace wisp;

namespace apc = wisp::terminal::apc;

typedef apc::Handler Handler;
typedef apc::Command Command;
typedef apc::StateKey StateKey;
typedef apc::Protocol Protocol;

static zigstd::Allocator talloc() { return zigstd::testing_allocator(); }

/* Wisp: `for ("...") |c| h.feed(alloc, c);` */
static void feedAll(Handler *h, const char *s) {
    for (size_t i = 0; s[i] != 0; i++) h->feed(talloc(), (uint8_t)s[i]);
}

static void feedSlice(Handler *h, const char *s) { h->feedSlice(talloc(), (const uint8_t *)s, strlen(s)); }

/* Wisp: `defer h.deinit();` */
struct HandlerHolder {
    Handler h;
    ~HandlerHolder() { h.deinit(); }
};

static std::string content(const Command &c) {
    return std::string((const char *)c.unknown.content, c.unknown.content_len);
}

TEST(apc, unknown_APC_command) {
    Handler h;
    h.start();
    feedAll(&h, "Xabcdef1234");
    Command result;
    ASSERT_TRUE(!h.end(&result));
}

TEST(apc, capture_unknown_APC_command) {
    HandlerHolder hh;
    Handler &h = hh.h;
    h.unknown_max_bytes = 5;
    h.start();
    feedSlice(&h, "abcd;payload");

    Command result;
    ASSERT_TRUE(h.end(&result));
    ASSERT_TRUE(result.key == Command::Key::unknown);
    ASSERT_TRUE(content(result) == "abcd;");
    ASSERT_TRUE(result.unknown.truncated);
    result.deinit(talloc());
}

TEST(apc, capture_short_unknown_APC_command) {
    HandlerHolder hh;
    Handler &h = hh.h;
    h.unknown_max_bytes = 16;
    h.start();
    h.feed(talloc(), 'X');

    Command result;
    ASSERT_TRUE(h.end(&result));
    ASSERT_TRUE(content(result) == "X");
    ASSERT_TRUE(!result.unknown.truncated);
    result.deinit(talloc());

    h.unknown_max_bytes = 1;
    h.start();
    feedSlice(&h, "XYZ");
    ASSERT_TRUE(h.end(&result));
    ASSERT_TRUE(content(result) == "X");
    ASSERT_TRUE(result.unknown.truncated);
    result.deinit(talloc());
}

TEST(apc, disabled_known_APC_protocol_is_not_unknown) {
    HandlerHolder hh;
    Handler &h = hh.h;
    h.unknown_max_bytes = 64;
    h.enable(Protocol::glyph, false);
    h.start();
    feedSlice(&h, "25a1;q;cp=E0A0");
    Command result;
    ASSERT_TRUE(!h.end(&result));

    /* An incomplete known protocol identifier is malformed, not unknown. */
    h.start();
    feedSlice(&h, "25a");
    ASSERT_TRUE(!h.end(&result));
}

TEST(apc, identify_with_unrecognized_command) {
    Handler h;
    h.start();
    feedAll(&h, "abcd;payload");
    Command result;
    ASSERT_TRUE(!h.end(&result));
}

TEST(apc, identify_buffer_overflow) {
    Handler h;
    h.start();
    feedAll(&h, "abcde;payload");
    Command result;
    ASSERT_TRUE(!h.end(&result));
}

TEST(apc, identify_with_no_input) {
    Handler h;
    h.start();
    Command result;
    ASSERT_TRUE(!h.end(&result));
}

TEST(apc, identify_with_unknown_partial_input) {
    Handler h;
    h.start();
    feedAll(&h, "25a");
    Command result;
    ASSERT_TRUE(!h.end(&result));
}

TEST(apc, feedSlice_unknown_APC_command_is_ignored) {
    Handler h;
    h.start();
    feedSlice(&h, "Xabcdef1234");
    ASSERT_TRUE(h.state.key == StateKey::ignore);
    feedSlice(&h, "more data that is dropped");
    Command result;
    ASSERT_TRUE(!h.end(&result));
}

TEST(apc, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
