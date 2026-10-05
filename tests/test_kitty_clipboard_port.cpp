/* Transliterated from the test blocks in Ghostty's
 * src/terminal/kitty/clipboard_*.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include <stdio.h>
#include <string.h>

#include <string>

#include "test_helpers.h"
#include "../terminal/kitty/clipboard_command.hpp"
#include "../terminal/kitty/clipboard_grants.hpp"
#include "../terminal/kitty/clipboard_response.hpp"
#include "../terminal/kitty/clipboard_write.hpp"

using namespace wisp;

/* Wisp: terminal::clipboard and terminal::kitty::clipboard are distinct
 * namespaces, so neither is pulled in wholesale. */
namespace clipboard = wisp::terminal::kitty::clipboard;
namespace osc = wisp::terminal::osc;
namespace sys = wisp::terminal::sys;
namespace terminal = wisp::terminal;

typedef clipboard::Grants Grants;

static zigstd::Allocator talloc() { return zigstd::testing_allocator(); }

/* Wisp: grants take a pointer and a length; the tests pass C strings. */
static bool grant(Grants *g, const char *pw, Grants::Direction dir, bool one_time) {
    return g->grant(talloc(), (const uint8_t *)pw, strlen(pw), dir, one_time);
}
static bool use(Grants *g, const char *pw, Grants::Direction dir) {
    return g->use(talloc(), (const uint8_t *)pw, strlen(pw), dir);
}

/* Wisp: `defer grants.deinit(alloc)`. */
struct GrantsHolder {
    Grants g;
    ~GrantsHolder() { g.deinit(talloc()); }
};

TEST(kitty_clipboard, grants__basic_grant_and_use) {
    GrantsHolder h;
    Grants &grants = h.g;
    ASSERT_TRUE(!use(&grants, "pw1", Grants::Direction::read));

    ASSERT_TRUE(grant(&grants, "pw1", Grants::Direction::read, false));
    ASSERT_TRUE(use(&grants, "pw1", Grants::Direction::read));
    /* Persistent grants survive use. */
    ASSERT_TRUE(use(&grants, "pw1", Grants::Direction::read));
    ASSERT_TRUE(!use(&grants, "pw1", Grants::Direction::write));
}

TEST(kitty_clipboard, grants__one_time_consumed_on_check) {
    GrantsHolder h;
    Grants &grants = h.g;
    ASSERT_TRUE(grant(&grants, "otp", Grants::Direction::read, true));
    ASSERT_TRUE(use(&grants, "otp", Grants::Direction::read));
    ASSERT_TRUE(!use(&grants, "otp", Grants::Direction::read));
}

TEST(kitty_clipboard, grants__one_time_consumed_even_on_direction_mismatch) {
    GrantsHolder h;
    Grants &grants = h.g;
    ASSERT_TRUE(grant(&grants, "otp", Grants::Direction::read, true));
    ASSERT_TRUE(!use(&grants, "otp", Grants::Direction::write));
    ASSERT_TRUE(!use(&grants, "otp", Grants::Direction::read));
}

TEST(kitty_clipboard, grants__directions_are_independent) {
    GrantsHolder h;
    Grants &grants = h.g;
    ASSERT_TRUE(grant(&grants, "pw", Grants::Direction::read, false));
    ASSERT_TRUE(grant(&grants, "pw", Grants::Direction::write, false));
    ASSERT_TRUE(use(&grants, "pw", Grants::Direction::read));
    ASSERT_TRUE(use(&grants, "pw", Grants::Direction::write));
}

TEST(kitty_clipboard, grants__capacity_evicts_the_oldest) {
    GrantsHolder h;
    Grants &grants = h.g;

    char buf[8];
    for (size_t i = 0; i < Grants::max_entries + 1; i++) {
        snprintf(buf, sizeof buf, "pw%zu", i);
        ASSERT_TRUE(grant(&grants, buf, Grants::Direction::read, false));
    }

    /* The oldest grant was evicted; the newest survives. */
    ASSERT_TRUE(!use(&grants, "pw0", Grants::Direction::read));
    snprintf(buf, sizeof buf, "pw%zu", Grants::max_entries);
    ASSERT_TRUE(use(&grants, buf, Grants::Direction::read));
}

TEST(kitty_clipboard, generateOtp__length_and_alphabet) {
    uint8_t otp[clipboard::otp_len];
    ASSERT_TRUE(clipboard::generateOtp(otp) == sys::RandomSecureError::none);
    for (size_t i = 0; i < clipboard::otp_len; i++) {
        ASSERT_TRUE(memchr(clipboard::otp_alphabet, otp[i], clipboard::otp_alphabet_len) != nullptr);
    }

    /* Two passwords don't collide (a repeat would mean no entropy). */
    uint8_t other[clipboard::otp_len];
    ASSERT_TRUE(clipboard::generateOtp(other) == sys::RandomSecureError::none);
    ASSERT_TRUE(memcmp(otp, other, clipboard::otp_len) != 0);
}

/* Wisp: upstream passes std.Io.failing; here the override stands in for a
 * source with no entropy. */
static sys::RandomSecureError failingRandom(uint8_t *, size_t) {
    return sys::RandomSecureError::EntropyUnavailable;
}

TEST(kitty_clipboard, generateOtp__no_entropy_is_an_error_never_a_weak_password) {
    sys::random_secure() = &failingRandom;
    uint8_t otp[clipboard::otp_len];
    const sys::RandomSecureError err = clipboard::generateOtp(otp);
    sys::random_secure() = nullptr;
    ASSERT_TRUE(err == sys::RandomSecureError::EntropyUnavailable);
}

static uint8_t g_counter = 0;
static sys::RandomSecureError countingRandom(uint8_t *buffer, size_t len) {
    for (size_t i = 0; i < len; i++) {
        buffer[i] = g_counter;
        g_counter = (uint8_t)(g_counter + 1);
    }
    return sys::RandomSecureError::none;
}

TEST(kitty_clipboard, generateOtp__sys_override_supplies_entropy) {
    sys::random_secure() = &countingRandom;
    uint8_t otp[clipboard::otp_len];
    const sys::RandomSecureError err = clipboard::generateOtp(otp);
    sys::random_secure() = nullptr;
    ASSERT_TRUE(err == sys::RandomSecureError::none);
    for (size_t i = 0; i < clipboard::otp_len; i++) {
        ASSERT_TRUE(memchr(clipboard::otp_alphabet, otp[i], clipboard::otp_alphabet_len) != nullptr);
    }
}

/* ─── clipboard_command.zig ────────────────────────────────────────────── */

typedef clipboard::Metadata Metadata;
typedef clipboard::Payload Payload;
typedef osc::kitty_clipboard_protocol::Operation Operation;

static osc::ZStr z(const char *s) { return osc::ZStr(s, strlen(s)); }

/* Wisp: `var arena: std.heap.ArenaAllocator = .init(testing.allocator);
 * defer arena.deinit();` */
struct ArenaHolder {
    zigstd::ArenaAllocator arena;
    ArenaHolder() : arena(talloc()) {}
    ~ArenaHolder() { arena.deinit(); }
    zigstd::Allocator allocator() { return arena.allocator(); }
};

TEST(kitty_clipboard, metadata__empty_is_dropped) {
    ArenaHolder arena;
    Metadata meta;
    ASSERT_TRUE(Metadata::parse(arena.allocator(), z(""), &meta) == Metadata::ParseError::dropped);
}

TEST(kitty_clipboard, metadata__record_without_eq_is_dropped) {
    ArenaHolder arena;
    Metadata meta;
    ASSERT_TRUE(Metadata::parse(arena.allocator(), z("type=read:bare"), &meta) == Metadata::ParseError::dropped);
    ASSERT_TRUE(Metadata::parse(arena.allocator(), z("bare:type=read"), &meta) == Metadata::ParseError::dropped);
}

TEST(kitty_clipboard, metadata__missing_or_unknown_type_is_dropped) {
    ArenaHolder arena;
    Metadata meta;
    ASSERT_TRUE(Metadata::parse(arena.allocator(), z("loc=primary"), &meta) == Metadata::ParseError::dropped);
    ASSERT_TRUE(Metadata::parse(arena.allocator(), z("type=bobr"), &meta) == Metadata::ParseError::dropped);
    ASSERT_TRUE(Metadata::parse(arena.allocator(), z("type="), &meta) == Metadata::ParseError::dropped);
}

TEST(kitty_clipboard, metadata__duplicate_keys_keep_the_last_occurrence) {
    ArenaHolder arena;
    Metadata meta;
    ASSERT_TRUE(Metadata::parse(arena.allocator(), z("type=bobr:type=read"), &meta) == Metadata::ParseError::none);
    ASSERT_TRUE(Operation::read == meta.op);
    ASSERT_TRUE(Metadata::parse(arena.allocator(), z("type=read:type=bobr"), &meta) ==
                Metadata::ParseError::dropped);
}

TEST(kitty_clipboard, metadata__basic_read) {
    ArenaHolder arena;
    Metadata meta;
    ASSERT_TRUE(Metadata::parse(arena.allocator(), z("type=read"), &meta) == Metadata::ParseError::none);
    ASSERT_TRUE(Operation::read == meta.op);
    ASSERT_TRUE(meta.loc == terminal::clipboard::Location::standard);
    ASSERT_TRUE(0 == meta.id.len);
}

TEST(kitty_clipboard, metadata__unknown_keys_ignored) {
    ArenaHolder arena;
    Metadata meta;
    ASSERT_TRUE(Metadata::parse(arena.allocator(), z("type=read:bobr=kurwa"), &meta) ==
                Metadata::ParseError::none);
    ASSERT_TRUE(Operation::read == meta.op);
}

TEST(kitty_clipboard, metadata__loc) {
    ArenaHolder arena;
    Metadata meta;
    ASSERT_TRUE(Metadata::parse(arena.allocator(), z("type=read:loc=primary"), &meta) ==
                Metadata::ParseError::none);
    ASSERT_TRUE(meta.loc == terminal::clipboard::Location::primary);
    /* Anything other than "primary" means the clipboard; it is not an
     * error. */
    ASSERT_TRUE(Metadata::parse(arena.allocator(), z("type=read:loc=bobr"), &meta) == Metadata::ParseError::none);
    ASSERT_TRUE(meta.loc == terminal::clipboard::Location::standard);
}

TEST(kitty_clipboard, metadata__id_sanitized) {
    ArenaHolder arena;
    {
        Metadata meta;
        ASSERT_TRUE(Metadata::parse(arena.allocator(), z("type=read:id=abc-123_x.Y+z"), &meta) ==
                    Metadata::ParseError::none);
        ASSERT_TRUE(std::string(meta.id.ptr, meta.id.len) == "abc-123_x.Y+z");
    }
    {
        /* Invalid characters are stripped, not rejected. */
        Metadata meta;
        ASSERT_TRUE(Metadata::parse(arena.allocator(), z("type=read:id=*4 2*"), &meta) ==
                    Metadata::ParseError::none);
        ASSERT_TRUE(std::string(meta.id.ptr, meta.id.len) == "42");
    }
}

TEST(kitty_clipboard, metadata__id_truncated_to_max) {
    ArenaHolder arena;
    std::string raw = "type=read:id=";
    raw.append(clipboard::max_id_len + 100, 'a');
    Metadata meta;
    ASSERT_TRUE(Metadata::parse(arena.allocator(), osc::ZStr(raw.data(), raw.size()), &meta) ==
                Metadata::ParseError::none);
    ASSERT_TRUE(clipboard::max_id_len == meta.id.len);
}

TEST(kitty_clipboard, metadata__mime_decoded) {
    ArenaHolder arena;
    /* "text/plain" */
    Metadata meta;
    ASSERT_TRUE(Metadata::parse(arena.allocator(), z("type=wdata:mime=dGV4dC9wbGFpbg=="), &meta) ==
                Metadata::ParseError::none);
    ASSERT_TRUE(std::string(meta.mime.ptr, meta.mime.len) == "text/plain");
}

TEST(kitty_clipboard, metadata__invalid_mime_base64_reported) {
    ArenaHolder arena;

    /* Invalid base64 in a metadata value aborts an in-flight write
     * per the spec, so like invalid UTF-8 it is reported rather than
     * silently dropping the packet. Unpadded or whitespace-laced
     * values are invalid too: metadata values use the same strict
     * encoding as payloads. */
    static const char *const cases[] = {
        "type=wdata:mime=!!!",
        /* "text/plain" without its padding. */
        "type=wdata:mime=dGV4dC9wbGFpbg",
        /* "text/plain" with its final byte replaced by '!'. */
        "type=wdata:mime=dGV4dC9wbGFpbg=!",
        /* A newline inside otherwise valid base64. */
        "type=wdata:mime=dGV4dC9w\nbGFpbg==",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Metadata meta;
        ASSERT_TRUE(Metadata::parse(arena.allocator(), z(cases[i]), &meta) == Metadata::ParseError::InvalidValue);
        Operation op;
        ASSERT_TRUE(Metadata::operation(z(cases[i]), &op));
        ASSERT_TRUE(Operation::wdata == op);
    }
}

TEST(kitty_clipboard, metadata__invalid_mime_utf8_reported) {
    ArenaHolder arena;
    /* base64 of 0xff 0xfe */
    Metadata meta;
    ASSERT_TRUE(Metadata::parse(arena.allocator(), z("type=wdata:mime=//4="), &meta) ==
                Metadata::ParseError::InvalidValue);
    Operation op;
    ASSERT_TRUE(Metadata::operation(z("type=wdata:mime=//4="), &op));
    ASSERT_TRUE(Operation::wdata == op);
}

TEST(kitty_clipboard, metadata__pw_and_name) {
    ArenaHolder arena;
    /* pw="secret", name="app" */
    Metadata meta;
    ASSERT_TRUE(Metadata::parse(arena.allocator(), z("type=read:pw=c2VjcmV0:name=YXBw"), &meta) ==
                Metadata::ParseError::none);
    ASSERT_TRUE(std::string(meta.pw.ptr, meta.pw.len) == "secret");
    ASSERT_TRUE(std::string(meta.name.ptr, meta.name.len) == "app");
}

/* Wisp: std.base64.standard.Encoder.encode. */
static std::string base64Encode(const std::string &src) {
    static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 3 <= src.size()) {
        const unsigned v = ((unsigned char)src[i] << 16) | ((unsigned char)src[i + 1] << 8) |
                           (unsigned char)src[i + 2];
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += alphabet[v & 63];
        i += 3;
    }
    const size_t rem = src.size() - i;
    if (rem == 1) {
        const unsigned v = (unsigned char)src[i] << 16;
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += "==";
    } else if (rem == 2) {
        const unsigned v = ((unsigned char)src[i] << 16) | ((unsigned char)src[i + 1] << 8);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

TEST(kitty_clipboard, metadata__over_long_name_reported) {
    ArenaHolder arena;
    const std::string longname(clipboard::max_name_len + 1, 'n');
    const std::string raw = "type=read:name=" + base64Encode(longname);
    Metadata meta;
    ASSERT_TRUE(Metadata::parse(arena.allocator(), osc::ZStr(raw.data(), raw.size()), &meta) ==
                Metadata::ParseError::InvalidValue);
}

TEST(kitty_clipboard, metadata__empty_name) {
    ArenaHolder arena;
    Metadata meta;
    ASSERT_TRUE(Metadata::parse(arena.allocator(), z("type=read:pw=c2VjcmV0:name="), &meta) ==
                Metadata::ParseError::none);
    ASSERT_TRUE(0 == meta.name.len);
}

TEST(kitty_clipboard, read_prompt_exemption__only_requests_without_data_types) {
    /* A targets-only ('.') read parses to zero data MIME types and is
     * served without a prompt; any data type, even alongside the
     * targets listing, still prompts. */
    ASSERT_TRUE(clipboard::readPromptExempt(0));
    ASSERT_TRUE(!clipboard::readPromptExempt(1));
}

TEST(kitty_clipboard, payload__mime_iterator) {
    /* base64 of "text/plain  text/html\ntext/uri-list" */
    Payload payload;
    ASSERT_TRUE(Payload::init(talloc(), z("dGV4dC9wbGFpbiAgdGV4dC9odG1sCnRleHQvdXJpLWxpc3Q="), &payload) ==
                Payload::InitError::none);
    Payload::MimeIterator it = payload.mimeIterator();
    osc::ZStr v;
    ASSERT_TRUE(it.next(&v) && std::string(v.ptr, v.len) == "text/plain");
    ASSERT_TRUE(it.next(&v) && std::string(v.ptr, v.len) == "text/html");
    ASSERT_TRUE(it.next(&v) && std::string(v.ptr, v.len) == "text/uri-list");
    ASSERT_TRUE(!it.next(&v));
    payload.deinit(talloc());
}

TEST(kitty_clipboard, payload__invalid_base64) {
    static const char *const cases[] = {
        "!!!",
        /* "text/plain" without its padding: payloads use the strict
         * encoding, so missing padding is rejected. */
        "dGV4dC9wbGFpbg",
        /* A newline inside otherwise valid base64. */
        "dGV4dC9w\nbGFpbg==",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Payload payload;
        ASSERT_TRUE(Payload::init(talloc(), z(cases[i]), &payload) == Payload::InitError::Invalid);
    }
}

TEST(kitty_clipboard, payload__decoded_text_must_be_valid_utf8) {
    /* Valid base64 encoding of a single 0xff byte. */
    Payload payload;
    ASSERT_TRUE(Payload::init(talloc(), z("/w=="), &payload) == Payload::InitError::none);
    ASSERT_TRUE(!payload.isValidUtf8());
    payload.deinit(talloc());
}

/* ─── clipboard_response.zig ───────────────────────────────────────────── */

typedef clipboard::Response Response;
typedef clipboard::ReadSuccess ReadSuccess;
typedef clipboard::PasteEvent PasteEvent;
typedef clipboard::Status Status;
typedef clipboard::Terminator Terminator;
typedef terminal::clipboard::Content Content;

TEST(kitty_clipboard, response__basic_status_packet) {
    std::string writer;
    Response r;
    r.op = Operation::write;
    r.status = Status::DONE;
    r.encode(&writer);
    ASSERT_STR_EQ("\x1b]5522;type=write:status=DONE\x1b\\", writer.c_str());
}

TEST(kitty_clipboard, response__id_echo_and_terminator) {
    std::string writer;
    Response r;
    r.op = Operation::write;
    r.status = Status::EPERM_;
    r.id = z("42");
    r.terminator = Terminator::bel;
    r.encode(&writer);
    ASSERT_STR_EQ("\x1b]5522;type=write:status=EPERM:id=42\x07", writer.c_str());
}

TEST(kitty_clipboard, response__key_order_type_status_loc_id_mime_pw_and_payload) {
    std::string writer;
    Response r;
    r.op = Operation::read;
    r.status = Status::DATA;
    r.primary = true;
    r.id = z("x");
    r.has_mime = true;
    r.mime = z("text/plain");
    r.has_pw = true;
    r.pw = z("otp");
    r.payload = z("Ghostty");
    r.encode(&writer);
    ASSERT_STR_EQ("\x1b]5522;type=read:status=DATA:loc=primary:id=x"
                  ":mime=dGV4dC9wbGFpbg==:pw=b3Rw;R2hvc3R0eQ==\x1b\\",
                  writer.c_str());
}

TEST(kitty_clipboard, read_success__empty_request_is_OK_then_DONE) {
    std::string writer;
    ReadSuccess r;
    r.id = z("7");
    r.encode(&writer);
    ASSERT_STR_EQ("\x1b]5522;type=read:status=OK:id=7\x1b\\"
                  "\x1b]5522;type=read:status=DONE:id=7\x1b\\",
                  writer.c_str());
}

TEST(kitty_clipboard, read_success__targets_listing_with_text) {
    std::string writer;
    const osc::ZStr available[] = {z("text/plain")};
    ReadSuccess r;
    r.list = true;
    r.available = available;
    r.available_len = 1;
    r.encode(&writer);
    /* "." => "Lg==", "text/plain\n" => "dGV4dC9wbGFpbgo=" */
    ASSERT_STR_EQ("\x1b]5522;type=read:status=OK\x1b\\"
                  "\x1b]5522;type=read:status=DATA:mime=Lg==;dGV4dC9wbGFpbgo=\x1b\\"
                  "\x1b]5522;type=read:status=DONE\x1b\\",
                  writer.c_str());
}

TEST(kitty_clipboard, read_success__targets_listing_joins_multiple_types) {
    std::string writer;
    const osc::ZStr available[] = {z("text/plain"), z("image/png")};
    ReadSuccess r;
    r.list = true;
    r.available = available;
    r.available_len = 2;
    r.encode(&writer);
    /* Payload is base64 of "text/plain image/png\n". */
    ASSERT_STR_EQ("\x1b]5522;type=read:status=OK\x1b\\"
                  "\x1b]5522;type=read:status=DATA:mime=Lg==;dGV4dC9wbGFpbiBpbWFnZS9wbmcK\x1b\\"
                  "\x1b]5522;type=read:status=DONE\x1b\\",
                  writer.c_str());
}

TEST(kitty_clipboard, read_success__empty_targets_listing_packet_still_sent) {
    std::string writer;
    ReadSuccess r;
    r.list = true;
    r.encode(&writer);
    ASSERT_STR_EQ("\x1b]5522;type=read:status=OK\x1b\\"
                  "\x1b]5522;type=read:status=DATA:mime=Lg==\x1b\\"
                  "\x1b]5522;type=read:status=DONE\x1b\\",
                  writer.c_str());
}

TEST(kitty_clipboard, read_success__data_chunks_under_requested_mime) {
    std::string writer;
    const Content contents[] = {Content(z("text/plain"), z("Ghostty"))};
    ReadSuccess r;
    r.contents = contents;
    r.contents_len = 1;
    r.encode(&writer);
    ASSERT_STR_EQ("\x1b]5522;type=read:status=OK\x1b\\"
                  "\x1b]5522;type=read:status=DATA:mime=dGV4dC9wbGFpbg==;R2hvc3R0eQ==\x1b\\"
                  "\x1b]5522;type=read:status=DONE\x1b\\",
                  writer.c_str());
}

TEST(kitty_clipboard, read_success__each_representation_carries_its_own_data) {
    std::string writer;
    const Content contents[] = {
        Content(z("text/plain"), z("hello")),
        Content(z("image/png"), osc::ZStr("\x89\x50\x4e\x47\x0d\x0a\x1a\x0a", 8)),
    };
    ReadSuccess r;
    r.contents = contents;
    r.contents_len = 2;
    r.encode(&writer);
    ASSERT_STR_EQ("\x1b]5522;type=read:status=OK\x1b\\"
                  "\x1b]5522;type=read:status=DATA:mime=dGV4dC9wbGFpbg==;aGVsbG8=\x1b\\"
                  "\x1b]5522;type=read:status=DATA:mime=aW1hZ2UvcG5n;iVBORw0KGgo=\x1b\\"
                  "\x1b]5522;type=read:status=DONE\x1b\\",
                  writer.c_str());
}

TEST(kitty_clipboard, read_success__chunking_at_read_chunk_size) {
    const std::string data(clipboard::read_chunk_size + 1, 'z');
    std::string writer;
    const Content contents[] = {Content(z("text/plain"), osc::ZStr(data.data(), data.size()))};
    ReadSuccess r;
    r.contents = contents;
    r.contents_len = 1;
    r.encode(&writer);

    /* OK + 2 DATA packets + DONE = 4 packets. */
    size_t count = 0;
    for (size_t i = 0; i + 7 <= writer.size(); i++) {
        if (memcmp(writer.data() + i, "\x1b]5522;", 7) == 0) count += 1;
    }
    ASSERT_TRUE(4 == count);

    /* The first chunk is exactly read_chunk_size bytes, base64 encoded
     * with padding. */
    std::string first;
    zigstd::base64::encodeWriter(&first, data.data(), clipboard::read_chunk_size);
    ASSERT_TRUE(writer.find(first) != std::string::npos);
}

TEST(kitty_clipboard, read_success__no_data_packets_for_empty_clipboard) {
    std::string writer;
    const Content contents[] = {Content(z("text/plain"), z(""))};
    ReadSuccess r;
    r.contents = contents;
    r.contents_len = 1;
    r.encode(&writer);
    ASSERT_STR_EQ("\x1b]5522;type=read:status=OK\x1b\\"
                  "\x1b]5522;type=read:status=DONE\x1b\\",
                  writer.c_str());
}

TEST(kitty_clipboard, read_success__paste_event_carries_pw_in_every_packet) {
    std::string writer;
    const osc::ZStr available[] = {z("text/plain")};
    ReadSuccess r;
    r.list = true;
    r.has_pw = true;
    r.pw = z("otp");
    r.available = available;
    r.available_len = 1;
    r.encode(&writer);
    ASSERT_STR_EQ("\x1b]5522;type=read:status=OK:pw=b3Rw\x1b\\"
                  "\x1b]5522;type=read:status=DATA:mime=Lg==:pw=b3Rw;dGV4dC9wbGFpbgo=\x1b\\"
                  "\x1b]5522;type=read:status=DONE:pw=b3Rw\x1b\\",
                  writer.c_str());
}

TEST(kitty_clipboard, paste_event__pw_in_every_packet_listing_of_every_type) {
    std::string writer;
    const osc::ZStr available[] = {z("text/plain"), z("image/png")};
    PasteEvent e;
    e.pw = z("otp");
    e.available = available;
    e.available_len = 2;
    e.encode(&writer);
    /* Payload is base64 of "text/plain image/png\n". */
    ASSERT_STR_EQ("\x1b]5522;type=read:status=OK:pw=b3Rw\x1b\\"
                  "\x1b]5522;type=read:status=DATA:mime=Lg==:pw=b3Rw;dGV4dC9wbGFpbiBpbWFnZS9wbmcK\x1b\\"
                  "\x1b]5522;type=read:status=DONE:pw=b3Rw\x1b\\",
                  writer.c_str());
}

TEST(kitty_clipboard, paste_event__primary_is_reported_only_on_the_OK_packet) {
    std::string writer;
    const osc::ZStr available[] = {z("text/plain")};
    PasteEvent e;
    e.primary = true;
    e.pw = z("otp");
    e.available = available;
    e.available_len = 1;
    e.terminator = Terminator::bel;
    e.encode(&writer);
    ASSERT_STR_EQ("\x1b]5522;type=read:status=OK:loc=primary:pw=b3Rw\x07"
                  "\x1b]5522;type=read:status=DATA:mime=Lg==:pw=b3Rw;dGV4dC9wbGFpbgo=\x07"
                  "\x1b]5522;type=read:status=DONE:pw=b3Rw\x07",
                  writer.c_str());
}

TEST(kitty_clipboard, paste_event__empty_listing_packet_is_still_sent) {
    std::string writer;
    PasteEvent e;
    e.pw = z("otp");
    e.encode(&writer);
    ASSERT_STR_EQ("\x1b]5522;type=read:status=OK:pw=b3Rw\x1b\\"
                  "\x1b]5522;type=read:status=DATA:mime=Lg==:pw=b3Rw\x1b\\"
                  "\x1b]5522;type=read:status=DONE:pw=b3Rw\x1b\\",
                  writer.c_str());
}

/* ─── clipboard_write.zig ──────────────────────────────────────────────── */

typedef clipboard::WriteState WriteState;

/* Wisp: `const begin_meta: Metadata = .{ .op = .write, .id = "..." };` */
static Metadata writeMeta(const char *id = "") {
    Metadata m;
    m.op = Operation::write;
    m.id = z(id);
    return m;
}
static Metadata wdataMeta(const char *mime) {
    Metadata m;
    m.op = Operation::wdata;
    m.mime = z(mime);
    return m;
}
static Metadata waliasMeta(const char *mime) {
    Metadata m;
    m.op = Operation::walias;
    m.mime = z(mime);
    return m;
}

/* Wisp: `var state: WriteState = try .init(...); defer state.deinit(alloc);` */
struct StateHolder {
    WriteState s;
    bool ok;
    StateHolder(const Metadata &meta, WriteState::Options opts = WriteState::Options()) : s(talloc()) {
        ok = WriteState::init(talloc(), &meta, opts, &s);
    }
    ~StateHolder() {
        if (ok) s.deinit(talloc());
    }
};

/* Wisp: `const committed = try state.commit(alloc); defer committed.deinit(alloc);` */
struct CommittedHolder {
    WriteState::Committed c;
    WriteState::CommitError err;
    explicit CommittedHolder(WriteState *s) { err = s->commit(talloc(), &c); }
    ~CommittedHolder() { c.deinit(talloc()); }
};

static std::string s(osc::ZStr v) { return std::string(v.ptr, v.len); }

TEST(kitty_clipboard, write__basic_transaction) {
    const Metadata begin_meta = writeMeta("42");
    StateHolder h(begin_meta);
    ASSERT_TRUE(h.ok);

    const Metadata m = wdataMeta("text/plain");
    ASSERT_TRUE(h.s.data(talloc(), &m, z("R2hvc3R0eQ==")) == WriteState::DataError::none); /* "Ghostty" */

    CommittedHolder c(&h.s);
    ASSERT_TRUE(c.err == WriteState::CommitError::none);
    ASSERT_TRUE(s(c.c.id) == "42");
    ASSERT_TRUE(c.c.loc == terminal::clipboard::Location::standard);
    ASSERT_TRUE(1 == c.c.contents_len);
    ASSERT_TRUE(s(c.c.contents[0].mime) == "text/plain");
    ASSERT_TRUE(s(c.c.contents[0].data) == "Ghostty");
}

TEST(kitty_clipboard, write__chunked_data_accumulates) {
    const Metadata begin_meta = writeMeta();
    StateHolder h(begin_meta);
    ASSERT_TRUE(h.ok);

    const Metadata m = wdataMeta("text/plain");
    ASSERT_TRUE(h.s.data(talloc(), &m, z("SGVsbG8=")) == WriteState::DataError::none); /* "Hello" */
    ASSERT_TRUE(h.s.data(talloc(), &m, z("V29ybGQ=")) == WriteState::DataError::none); /* "World" */

    CommittedHolder c(&h.s);
    ASSERT_TRUE(c.err == WriteState::CommitError::none);
    ASSERT_TRUE(s(c.c.contents[0].data) == "HelloWorld");
}

TEST(kitty_clipboard, write__multiple_mimes_in_order) {
    const Metadata begin_meta = writeMeta();
    StateHolder h(begin_meta);
    ASSERT_TRUE(h.ok);

    const Metadata m1 = wdataMeta("text/plain");
    const Metadata m2 = wdataMeta("text/html");
    ASSERT_TRUE(h.s.data(talloc(), &m1, z("YQ==")) == WriteState::DataError::none); /* "a" */
    ASSERT_TRUE(h.s.data(talloc(), &m2, z("Yg==")) == WriteState::DataError::none); /* "b" */

    CommittedHolder c(&h.s);
    ASSERT_TRUE(c.err == WriteState::CommitError::none);
    ASSERT_TRUE(2 == c.c.contents_len);
    ASSERT_TRUE(s(c.c.contents[0].mime) == "text/plain");
    ASSERT_TRUE(s(c.c.contents[0].data) == "a");
    ASSERT_TRUE(s(c.c.contents[1].mime) == "text/html");
    ASSERT_TRUE(s(c.c.contents[1].data) == "b");
}

TEST(kitty_clipboard, write__reused_mime_overwrites) {
    const Metadata begin_meta = writeMeta();
    StateHolder h(begin_meta);
    ASSERT_TRUE(h.ok);

    const Metadata m1 = wdataMeta("text/plain");
    const Metadata m2 = wdataMeta("text/html");
    ASSERT_TRUE(h.s.data(talloc(), &m1, z("YQ==")) == WriteState::DataError::none); /* "a" */
    ASSERT_TRUE(h.s.data(talloc(), &m2, z("Yg==")) == WriteState::DataError::none); /* "b" */
    ASSERT_TRUE(h.s.data(talloc(), &m1, z("Yw==")) == WriteState::DataError::none); /* "c" */

    CommittedHolder c(&h.s);
    ASSERT_TRUE(c.err == WriteState::CommitError::none);
    ASSERT_TRUE(2 == c.c.contents_len);
    /* Position preserved, data replaced. */
    ASSERT_TRUE(s(c.c.contents[0].mime) == "text/plain");
    ASSERT_TRUE(s(c.c.contents[0].data) == "c");
}

TEST(kitty_clipboard, write__invalid_base64_chunk_aborts_the_transaction) {
    /* Invalid characters anywhere in the stream are error.Invalid,
     * which the handlers turn into an EINVAL abort. These mirror
     * kitty's own tests for the spec change. */
    static const char *const invalid_payloads[] = {
        "!!!",
        "SGVs!!!bG8=",
        "\nZGF0YSB3aXRoIGEgbmV3bGluZQ==",
    };
    for (size_t i = 0; i < sizeof(invalid_payloads) / sizeof(invalid_payloads[0]); i++) {
        const Metadata begin_meta = writeMeta();
        StateHolder h(begin_meta);
        ASSERT_TRUE(h.ok);
        const Metadata m = wdataMeta("text/plain");
        ASSERT_TRUE(h.s.data(talloc(), &m, z(invalid_payloads[i])) == WriteState::DataError::Invalid);
    }

    /* Also after an earlier valid chunk for the same MIME type. */
    {
        const Metadata begin_meta = writeMeta();
        StateHolder h(begin_meta);
        ASSERT_TRUE(h.ok);
        const Metadata m = wdataMeta("text/plain");
        ASSERT_TRUE(h.s.data(talloc(), &m, z("Z29vZA==")) == WriteState::DataError::none); /* "good" */
        ASSERT_TRUE(h.s.data(talloc(), &m, z("SGVs!!!bG8=")) == WriteState::DataError::Invalid);
    }
}

TEST(kitty_clipboard, write__chunks_split_one_base64_stream_at_arbitrary_boundaries) {
    /* The concatenation of all payloads per MIME type is the base64
     * stream; individual packets need not be a multiple of four bytes. */
    const char *encoded = "c29tZSBkYXRh"; /* "some data" */
    const size_t encoded_len = strlen(encoded);
    static const size_t split_a[] = {3, 7};
    static const size_t split_b[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    struct Split {
        const size_t *ends;
        size_t n;
    };
    static const Split splits[] = {{split_a, 2}, {split_b, 11}};

    for (size_t si = 0; si < sizeof(splits) / sizeof(splits[0]); si++) {
        const Metadata begin_meta = writeMeta();
        StateHolder h(begin_meta);
        ASSERT_TRUE(h.ok);
        const Metadata m = wdataMeta("text/plain");

        size_t prev = 0;
        for (size_t i = 0; i < splits[si].n; i++) {
            const size_t end = splits[si].ends[i];
            ASSERT_TRUE(h.s.data(talloc(), &m, osc::ZStr(encoded + prev, end - prev)) ==
                        WriteState::DataError::none);
            prev = end;
        }
        ASSERT_TRUE(h.s.data(talloc(), &m, osc::ZStr(encoded + prev, encoded_len - prev)) ==
                    WriteState::DataError::none);

        CommittedHolder c(&h.s);
        ASSERT_TRUE(c.err == WriteState::CommitError::none);
        ASSERT_TRUE(s(c.c.contents[0].data) == "some data");
    }
}

TEST(kitty_clipboard, write__incorrectly_padded_stream_aborts_at_commit) {
    const Metadata begin_meta = writeMeta();
    StateHolder h(begin_meta);
    ASSERT_TRUE(h.ok);

    /* "Hello" without its final padding byte: every chunk decodes,
     * but the stream ends mid-group so the commit reports it. */
    const Metadata m = wdataMeta("text/plain");
    ASSERT_TRUE(h.s.data(talloc(), &m, z("SGVsbG8")) == WriteState::DataError::none);
    CommittedHolder c(&h.s);
    ASSERT_TRUE(c.err == WriteState::CommitError::Invalid);
}

TEST(kitty_clipboard, write__incorrectly_padded_stream_aborts_at_MIME_switch) {
    const Metadata begin_meta = writeMeta();
    StateHolder h(begin_meta);
    ASSERT_TRUE(h.ok);

    const Metadata m1 = wdataMeta("text/plain");
    const Metadata m2 = wdataMeta("text/html");
    ASSERT_TRUE(h.s.data(talloc(), &m1, z("SGVsbG8")) == WriteState::DataError::none);
    ASSERT_TRUE(h.s.data(talloc(), &m2, z("PGI+aGk8L2I+")) == WriteState::DataError::Invalid);
}

TEST(kitty_clipboard, write__independently_padded_chunks_accumulate) {
    /* A packet payload ending exactly at terminal padding resets the
     * stream, so clients that base64-encode every chunk independently
     * keep working (matching kitty, which resets its streaming
     * decoder on EOF). Padding followed by more data within a single
     * packet stays invalid. */
    const Metadata begin_meta = writeMeta();
    StateHolder h(begin_meta);
    ASSERT_TRUE(h.ok);

    const Metadata m = wdataMeta("text/plain");
    ASSERT_TRUE(h.s.data(talloc(), &m, z("Z29vZA==")) == WriteState::DataError::none); /* "good" */
    ASSERT_TRUE(h.s.data(talloc(), &m, z("bW9yZQ==")) == WriteState::DataError::none); /* "more" */

    CommittedHolder c(&h.s);
    ASSERT_TRUE(c.err == WriteState::CommitError::none);
    ASSERT_TRUE(s(c.c.contents[0].data) == "goodmore");
}

TEST(kitty_clipboard, write__data_after_padding_within_one_chunk_aborts) {
    const Metadata begin_meta = writeMeta();
    StateHolder h(begin_meta);
    ASSERT_TRUE(h.ok);

    const Metadata m = wdataMeta("text/plain");
    ASSERT_TRUE(h.s.data(talloc(), &m, z("Z29vZA==bW9yZQ==")) == WriteState::DataError::Invalid);
}

TEST(kitty_clipboard, write__aliases_resolve_at_commit) {
    const Metadata begin_meta = writeMeta();
    StateHolder h(begin_meta);
    ASSERT_TRUE(h.ok);

    const Metadata m = wdataMeta("text/plain");
    ASSERT_TRUE(h.s.data(talloc(), &m, z("R2hvc3R0eQ==")) == WriteState::DataError::none); /* "Ghostty" */

    /* Alias "TEXT UTF8_STRING" -> text/plain. */
    const Metadata alias_meta = waliasMeta("text/plain");
    ASSERT_TRUE(h.s.alias(talloc(), &alias_meta, z("VEVYVCBVVEY4X1NUUklORw==")) ==
                WriteState::AliasError::none);

    CommittedHolder c(&h.s);
    ASSERT_TRUE(c.err == WriteState::CommitError::none);
    ASSERT_TRUE(3 == c.c.contents_len);
    ASSERT_TRUE(s(c.c.contents[1].mime) == "TEXT");
    ASSERT_TRUE(s(c.c.contents[1].data) == "Ghostty");
    ASSERT_TRUE(s(c.c.contents[2].mime) == "UTF8_STRING");
    ASSERT_TRUE(s(c.c.contents[2].data) == "Ghostty");
}

TEST(kitty_clipboard, write__alias_payload_must_be_valid_utf8) {
    const Metadata begin_meta = writeMeta();
    StateHolder h(begin_meta);
    ASSERT_TRUE(h.ok);

    const Metadata alias_meta = waliasMeta("text/plain");
    /* Valid base64 encoding of a single 0xff byte. */
    ASSERT_TRUE(h.s.alias(talloc(), &alias_meta, z("/w==")) == WriteState::AliasError::Invalid);
}

TEST(kitty_clipboard, write__default_limit_when_unset) {
    const Metadata begin_meta = writeMeta();
    StateHolder h(begin_meta);
    ASSERT_TRUE(h.ok);
    ASSERT_TRUE(clipboard::max_write_size == h.s.max_size);
}

/* Wisp: `.{ .max_size = n }` */
static WriteState::Options maxSize(size_t n) {
    WriteState::Options o;
    o.max_size = n;
    return o;
}

TEST(kitty_clipboard, write__text_over_limit_rejects_transaction) {
    const Metadata begin_meta = writeMeta();
    StateHolder h(begin_meta, maxSize(8));
    ASSERT_TRUE(h.ok);

    const Metadata m = wdataMeta("text/plain");
    /* "HelloWorld" */
    ASSERT_TRUE(h.s.data(talloc(), &m, z("SGVsbG9Xb3JsZA==")) == WriteState::DataError::TooLarge);
}

TEST(kitty_clipboard, write__text_crossing_limit_across_chunks_rejects_transaction) {
    const Metadata begin_meta = writeMeta();
    StateHolder h(begin_meta, maxSize(8));
    ASSERT_TRUE(h.ok);

    const Metadata m = wdataMeta("text/plain");
    ASSERT_TRUE(h.s.data(talloc(), &m, z("SGVsbG8=")) == WriteState::DataError::none); /* "Hello" */
    ASSERT_TRUE(h.s.data(talloc(), &m, z("V29ybGQ=")) == WriteState::DataError::TooLarge); /* "World" */
}

TEST(kitty_clipboard, write__data_exactly_at_limit_is_accepted) {
    const Metadata begin_meta = writeMeta();
    StateHolder h(begin_meta, maxSize(5));
    ASSERT_TRUE(h.ok);

    const Metadata m = wdataMeta("text/plain");
    ASSERT_TRUE(h.s.data(talloc(), &m, z("SGVsbG8=")) == WriteState::DataError::none); /* "Hello" */

    CommittedHolder c(&h.s);
    ASSERT_TRUE(c.err == WriteState::CommitError::none);
    ASSERT_TRUE(s(c.c.contents[0].data) == "Hello");
}

TEST(kitty_clipboard, write__non_text_under_limit_is_unaffected) {
    const Metadata begin_meta = writeMeta();
    StateHolder h(begin_meta, maxSize(8));
    ASSERT_TRUE(h.ok);

    const Metadata m = wdataMeta("image/png");
    ASSERT_TRUE(h.s.data(talloc(), &m, z("iVBORw==")) == WriteState::DataError::none); /* "\x89PNG" */

    CommittedHolder c(&h.s);
    ASSERT_TRUE(c.err == WriteState::CommitError::none);
    ASSERT_TRUE(s(c.c.contents[0].data) == "\x89PNG");
}

TEST(kitty_clipboard, write__non_text_over_limit_rejects_transaction) {
    const Metadata begin_meta = writeMeta();
    StateHolder h(begin_meta, maxSize(8));
    ASSERT_TRUE(h.ok);

    const Metadata m = wdataMeta("image/png");
    /* "HelloWorld" */
    ASSERT_TRUE(h.s.data(talloc(), &m, z("SGVsbG9Xb3JsZA==")) == WriteState::DataError::TooLarge);
}

TEST(kitty_clipboard, write__total_data_across_MIME_types_is_limited) {
    const Metadata begin_meta = writeMeta();
    StateHolder h(begin_meta, maxSize(8));
    ASSERT_TRUE(h.ok);

    const Metadata m1 = wdataMeta("text/plain");
    const Metadata m2 = wdataMeta("image/png");
    ASSERT_TRUE(h.s.data(talloc(), &m1, z("SGVsbG8=")) == WriteState::DataError::none); /* "Hello" */
    ASSERT_TRUE(h.s.data(talloc(), &m2, z("V29ybGQ=")) == WriteState::DataError::TooLarge); /* "World" */
}

TEST(kitty_clipboard, write__alias_without_data_target_is_dropped) {
    const Metadata begin_meta = writeMeta();
    StateHolder h(begin_meta);
    ASSERT_TRUE(h.ok);

    const Metadata alias_meta = waliasMeta("text/plain");
    ASSERT_TRUE(h.s.alias(talloc(), &alias_meta, z("VEVYVA==")) == WriteState::AliasError::none); /* "TEXT" */

    CommittedHolder c(&h.s);
    ASSERT_TRUE(c.err == WriteState::CommitError::none);
    ASSERT_TRUE(0 == c.c.contents_len);
}

TEST(kitty_clipboard, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
