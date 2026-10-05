/* Transliterated from the test blocks in Ghostty's src/terminal/kitty/dnd_*.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include <string.h>

#include <string>

#include "test_helpers.h"
#include "../simd/base64.hpp"
#include "../terminal/kitty/dnd_command.hpp"
#include "../terminal/kitty/dnd_drop.hpp"
#include "../terminal/kitty/dnd_response.hpp"

using namespace wisp;

namespace dnd = wisp::terminal::kitty::dnd;
namespace osc = wisp::terminal::osc;
namespace simd = wisp::simd;

typedef dnd::Metadata Metadata;
typedef dnd::EventType EventType;
typedef dnd::Operation Operation;
typedef dnd::Request Request;
typedef dnd::Chunking Chunking;

static osc::ZStr z(const char *s) { return osc::ZStr(s, strlen(s)); }

/* Wisp: `Metadata.parse(raw).?` */
static Metadata parseOk(const char *raw) {
    Metadata m;
    const bool ok = Metadata::parse(z(raw), &m);
    if (!ok) {
        /* Callers only use this where upstream unwraps the optional. */
        Metadata bad;
        bad.client_id = 0xDEADBEEF;
        return bad;
    }
    return m;
}
static bool parseFails(const char *raw) {
    Metadata m;
    return !Metadata::parse(z(raw), &m);
}

TEST(kitty_dnd, Metadata__empty) {
    const Metadata meta = parseOk("");
    ASSERT_TRUE(!meta.has_type);
    ASSERT_TRUE(!meta.more);
    ASSERT_TRUE(0 == meta.client_id);
}

TEST(kitty_dnd, Metadata__all_keys) {
    const Metadata meta = parseOk("t=m:m=1:i=3:o=2:x=10:y=5:X=320:Y=200");
    ASSERT_TRUE(meta.has_type && EventType::status == meta.type);
    ASSERT_TRUE(meta.more);
    ASSERT_TRUE(3 == meta.client_id);
    ASSERT_TRUE(2 == meta.operation);
    ASSERT_TRUE(10 == meta.cell_x);
    ASSERT_TRUE(5 == meta.cell_y);
    ASSERT_TRUE(320 == meta.pixel_x);
    ASSERT_TRUE(200 == meta.pixel_y);
}

TEST(kitty_dnd, Metadata__all_event_types) {
    struct Case {
        const char *raw;
        EventType want;
    };
    static const Case cases[] = {
        {"t=a", EventType::register_},   {"t=A", EventType::unregister},
        {"t=m", EventType::status},      {"t=M", EventType::drop},
        {"t=r", EventType::request},     {"t=R", EventType::request_error},
        {"t=o", EventType::offer},       {"t=p", EventType::present},
        {"t=P", EventType::start_drag},  {"t=e", EventType::drag_event},
        {"t=E", EventType::drag_error},  {"t=k", EventType::remote_data},
        {"t=q", EventType::query},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const Metadata meta = parseOk(cases[i].raw);
        ASSERT_TRUE(meta.has_type && cases[i].want == meta.type);
    }
}

TEST(kitty_dnd, Metadata__case_sensitive_coordinate_keys) {
    const Metadata meta = parseOk("x=10:Y=200");
    ASSERT_TRUE(10 == meta.cell_x);
    ASSERT_TRUE(0 == meta.cell_y);
    ASSERT_TRUE(0 == meta.pixel_x);
    ASSERT_TRUE(200 == meta.pixel_y);
}

TEST(kitty_dnd, Metadata__negative_coordinates) {
    const Metadata meta = parseOk("t=m:x=-1:y=-1");
    ASSERT_TRUE(-1 == meta.cell_x);
    ASSERT_TRUE(-1 == meta.cell_y);
}

TEST(kitty_dnd, Metadata__malformed_inputs_rejected) {
    /* Unknown event type. */
    ASSERT_TRUE(parseFails("t=z"));
    /* Unknown key. */
    ASSERT_TRUE(parseFails("z=1"));
    /* Missing '=' mid-stream. */
    ASSERT_TRUE(parseFails("x10"));
    /* No digits. */
    ASSERT_TRUE(parseFails("x=notanumber"));
    ASSERT_TRUE(parseFails("x=-"));
    /* Too large. */
    ASSERT_TRUE(parseFails("i=4294967296"));
    ASSERT_TRUE(parseFails("i=99999999999"));
    /* Garbage after value. */
    ASSERT_TRUE(parseFails("x=1z"));
    /* No whitespace tolerance, matching kitty. */
    ASSERT_TRUE(parseFails("t=a: x=1"));
    ASSERT_TRUE(parseFails("t = a"));
    /* Truncated mid-construct, matching kitty's final-state check.
     * Nothing state-changing may be parsed out of these: e.g. "t=r:x="
     * must not be treated as a drop conclusion. */
    ASSERT_TRUE(parseFails("t"));
    ASSERT_TRUE(parseFails("t="));
    ASSERT_TRUE(parseFails("x="));
    ASSERT_TRUE(parseFails("t=r:x="));
    ASSERT_TRUE(parseFails("x=1:t="));
}

TEST(kitty_dnd, Metadata__trailing_separator_accepted) {
    /* Kitty's state machine accepts the metadata ending right after a
     * value separator. */
    const Metadata meta = parseOk("t=a:");
    ASSERT_TRUE(meta.has_type && EventType::register_ == meta.type);
}

TEST(kitty_dnd, Metadata__u32_boundary_accepted) {
    const Metadata meta = parseOk("i=4294967295");
    ASSERT_TRUE(UINT32_MAX == meta.client_id);
}

TEST(kitty_dnd, Request__classification) {
    /* Conclude. */
    {
        const Request r = Request::init(parseOk("t=r:o=2"));
        ASSERT_TRUE(r.tag == Request::Tag::conclude);
        ASSERT_TRUE(Operation::move == r.conclude);
    }
    /* Conclude with unknown operation is none (canceled). */
    {
        const Request r = Request::init(parseOk("t=r:o=9"));
        ASSERT_TRUE(r.tag == Request::Tag::conclude);
        ASSERT_TRUE(Operation::none == r.conclude);
    }
    /* MIME data request. */
    {
        const Request r = Request::init(parseOk("t=r:x=2"));
        ASSERT_TRUE(r.tag == Request::Tag::mime);
        ASSERT_TRUE(2 == r.mime);
    }
    /* URI file request. */
    {
        const Request r = Request::init(parseOk("t=r:x=1:y=3"));
        ASSERT_TRUE(r.tag == Request::Tag::uri);
        ASSERT_TRUE(1 == r.uri.mime_idx);
        ASSERT_TRUE(3 == r.uri.uri_idx);
    }
    /* Directory handle request. */
    {
        const Request r = Request::init(parseOk("t=r:Y=2:x=1"));
        ASSERT_TRUE(r.tag == Request::Tag::dir);
        ASSERT_TRUE(2 == r.dir.handle);
        ASSERT_TRUE(1 == r.dir.entry);
    }
}

TEST(kitty_dnd, Chunking__reassembly_reuses_first_chunk_metadata) {
    Chunking chunking;

    /* First chunk starts reassembly. */
    const Metadata first = chunking.apply(parseOk("t=m:o=1:m=1"));
    ASSERT_TRUE(chunking.active);
    ASSERT_TRUE(first.has_type && EventType::status == first.type);
    ASSERT_TRUE(first.more);

    /* Continuation metadata is ignored except for `more`. */
    const Metadata second = chunking.apply(parseOk("t=q:o=2:m=1"));
    ASSERT_TRUE(chunking.active);
    ASSERT_TRUE(second.has_type && EventType::status == second.type);
    ASSERT_TRUE(1 == second.operation);
    ASSERT_TRUE(second.more);

    /* Final chunk ends reassembly. */
    const Metadata last = chunking.apply(parseOk("t=q:m=0"));
    ASSERT_TRUE(!chunking.active);
    ASSERT_TRUE(last.has_type && EventType::status == last.type);
    ASSERT_TRUE(!last.more);
}

TEST(kitty_dnd, Chunking__unchunked_commands_pass_through) {
    Chunking chunking;
    const Metadata meta = chunking.apply(parseOk("t=q"));
    ASSERT_TRUE(!chunking.active);
    ASSERT_TRUE(meta.has_type && EventType::query == meta.type);
}

/* ─── dnd_response.zig ─────────────────────────────────────────────────── */

typedef dnd::RequestKeys RequestKeys;
typedef dnd::Errno Errno;
typedef dnd::Encoding Encoding;
typedef dnd::Terminator Terminator;

TEST(kitty_dnd, encode__bare_message) {
    std::string writer;
    dnd::encode(&writer, z("t=q"), 0, z(""), Encoding::plain, Terminator::st);
    ASSERT_STR_EQ("\x1b]72;t=q\x1b\\", writer.c_str());
}

TEST(kitty_dnd, encode__bare_message_with_client_id) {
    std::string writer;
    dnd::encode(&writer, z("t=q"), 7, z(""), Encoding::plain, Terminator::st);
    ASSERT_STR_EQ("\x1b]72;t=q:i=7\x1b\\", writer.c_str());
}

TEST(kitty_dnd, encode__plain_payload_single_chunk) {
    std::string writer;
    dnd::encode(&writer, z("t=m:x=1:y=2"), 0, z("text/plain "), Encoding::plain, Terminator::st);
    ASSERT_STR_EQ("\x1b]72;t=m:x=1:y=2:m=0;text/plain \x1b\\", writer.c_str());
}

TEST(kitty_dnd, encode__base64_payload_chunking) {
    std::string out;

    /* Exactly one byte more than a chunk to force two chunks. */
    const std::string data(dnd::max_chunk_raw + 1, 'A');
    dnd::encode(&out, z("t=r:x=1"), 3, osc::ZStr(data.data(), data.size()), Encoding::base64, Terminator::st);

    /* First chunk: full header, m=1, 4096 base64 chars. */
    const std::string prefix = "\x1b]72;t=r:x=1:i=3:m=1;";
    ASSERT_TRUE(out.compare(0, prefix.size(), prefix) == 0);
    const size_t first_end = out.find("\x1b\\");
    ASSERT_TRUE(first_end != std::string::npos);
    ASSERT_TRUE(prefix.size() + 4096 == first_end);

    /* Second chunk: m=0 with the single remaining byte. */
    const std::string rest = out.substr(first_end + 2);
    const std::string second_prefix = "\x1b]72;t=r:x=1:i=3:m=0;";
    ASSERT_TRUE(rest.compare(0, second_prefix.size(), second_prefix) == 0);

    /* Decodes back to the original data. */
    std::string decoded;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t end = out.find("\x1b\\", pos);
        if (end == std::string::npos) end = out.size();
        const std::string osc_pkt = out.substr(pos, end - pos);
        pos = end + 2;
        if (osc_pkt.empty()) continue;
        const size_t payload_start = osc_pkt.find(';');
        const size_t payload_sep = osc_pkt.find(';', payload_start + 1);
        const std::string payload = osc_pkt.substr(payload_sep + 1);
        uint8_t buf[4096];
        size_t n = 0;
        ASSERT_TRUE(simd::base64::decodeStrict((const uint8_t *)payload.data(), payload.size(), buf, sizeof buf,
                                               simd::base64::Padding::required, &n));
        decoded.append((const char *)buf, n);
    }
    ASSERT_TRUE(decoded == data);
}

TEST(kitty_dnd, encodeError__with_and_without_description) {
    {
        std::string writer;
        RequestKeys keys;
        keys.x = 2;
        dnd::encodeError(&writer, dnd::ErrorKind::drop, keys, 0, Errno::ENOENT_,
                         z("drop data request index out of bounds"), Terminator::st);
        ASSERT_STR_EQ("\x1b]72;t=R:x=2:m=0;ENOENT:drop data request index out of bounds\x1b\\", writer.c_str());
    }
    {
        std::string writer;
        dnd::encodeError(&writer, dnd::ErrorKind::drag, RequestKeys(), 5, Errno::EPERM_, z(""), Terminator::st);
        ASSERT_STR_EQ("\x1b]72;t=E:i=5:m=0;EPERM\x1b\\", writer.c_str());
    }
}

TEST(kitty_dnd, RequestKeys__only_non_zero_keys_written) {
    {
        std::string s;
        RequestKeys().format(&s);
        ASSERT_STR_EQ("", s.c_str());
    }
    {
        std::string s;
        RequestKeys keys;
        keys.x = 1;
        keys.y = 2;
        keys.Y = 3;
        keys.format(&s);
        ASSERT_STR_EQ(":x=1:y=2:Y=3", s.c_str());
    }
    {
        std::string s;
        RequestKeys keys;
        keys.Y = 4;
        keys.format(&s);
        ASSERT_STR_EQ(":Y=4", s.c_str());
    }
}

/* ─── dnd_test.zig ─────────────────────────────────────────────────────── */
/* End-to-end tests for the OSC 72 protocol state machine, validating
 * wire behavior against kitty's implementation (using kitty_tests/dnd.py as
 * an oracle for the expected bytes). */

typedef dnd::State State;
typedef dnd::Event Event;
typedef State::MoveEvent MoveEvent;
typedef State::Item Item;

static zigstd::Allocator talloc() { return zigstd::testing_allocator(); }

/* A test harness holding the lazily allocated protocol state and an
 * output collector. */
struct Harness {
    State *state; /* = null */
    std::string output;

    Harness() : state(nullptr) {}

    ~Harness() {
        if (state != nullptr) state->destroy(talloc());
    }

    /* The registered state; asserts a client has registered. */
    State *registered() { return state; }

    /* Feed one client command, as it would arrive from the OSC parser,
     * returning the event the stream handler would pass to its effect. */
    bool command(const char *metadata, const char *payload, bool *has_event, Event *event) {
        osc::Command cmd;
        cmd.key = osc::Command::Key::kitty_dnd_protocol;
        cmd.kitty_dnd_protocol.metadata = z(metadata);
        cmd.kitty_dnd_protocol.has_payload = payload != nullptr;
        cmd.kitty_dnd_protocol.payload = payload != nullptr ? z(payload) : osc::ZStr();
        cmd.kitty_dnd_protocol.terminator = dnd::Terminator::st;
        return dnd::handleCommand(&state, talloc(), &output, cmd, has_event, event);
    }

    /* Wisp: `_ = try h.command(...)`, where the event is unused. */
    void cmd(const char *metadata, const char *payload) {
        bool has_event = false;
        Event event = Event::registration;
        (void)command(metadata, payload, &has_event, &event);
    }

    /* Wisp: `(try h.command(...)).?` */
    Event cmdEvent(const char *metadata, const char *payload, bool *has_event) {
        Event event = Event::registration;
        (void)command(metadata, payload, has_event, &event);
        return event;
    }

    bool cmdNoEvent(const char *metadata, const char *payload) {
        bool has_event = false;
        Event event = Event::registration;
        (void)command(metadata, payload, &has_event, &event);
        return !has_event;
    }

    void clear() { output.clear(); }
};

#define EXPECT_OUTPUT(h, expected)                                                                             \
    do {                                                                                                       \
        ASSERT_STR_EQ(expected, (h).output.c_str());                                                           \
        (h).clear();                                                                                           \
    } while (0)

/* Wisp: `.{ .cell_x = ..., ... }` */
static MoveEvent moveEv(uint32_t cx, uint32_t cy, int32_t px, int32_t py, bool copy, bool move = false) {
    MoveEvent ev;
    ev.cell_x = cx;
    ev.cell_y = cy;
    ev.pixel_x = px;
    ev.pixel_y = py;
    ev.operations.copy = copy;
    ev.operations.move = move;
    return ev;
}

static bool dragMove1(State *s, std::string *w, const MoveEvent &ev, const char *m0, const char *m1 = nullptr) {
    osc::ZStr mimes[2];
    mimes[0] = z(m0);
    if (m1 != nullptr) mimes[1] = z(m1);
    return s->dragMove(talloc(), w, ev, mimes, m1 != nullptr ? 2 : 1);
}

TEST(kitty_dnd, dnd__query_response) {
    Harness h;

    /* Works without any registration, matching kitty, and allocates
     * nothing. */
    h.cmd("t=q", nullptr);
    EXPECT_OUTPUT(h, "\x1b]72;t=q\x1b\\");
    ASSERT_TRUE(h.state == nullptr);
}

TEST(kitty_dnd, dnd__query_response_echoes_client_id) {
    Harness h;

    h.cmd("t=q:i=31", nullptr);
    EXPECT_OUTPUT(h, "\x1b]72;t=q:i=31\x1b\\");
}

TEST(kitty_dnd, dnd__register_and_unregister) {
    Harness h;

    ASSERT_TRUE(h.state == nullptr);

    /* Registration allocates the state and reports it, with the
     * declared MIME list readable from the state. */
    {
        bool has_event = false;
        ASSERT_TRUE(h.cmdEvent("t=a", "text/plain text/uri-list", &has_event) == Event::registration);
        ASSERT_TRUE(has_event);
    }
    EXPECT_OUTPUT(h, "");
    {
        State::MimeIterator it = h.registered()->registeredMimes();
        osc::ZStr v;
        ASSERT_TRUE(it.next(&v) && std::string(v.ptr, v.len) == "text/plain");
        ASSERT_TRUE(it.next(&v) && std::string(v.ptr, v.len) == "text/uri-list");
        ASSERT_TRUE(!it.next(&v));
    }

    /* Machine ID declaration is accepted and ignored. */
    ASSERT_TRUE(h.cmdNoEvent("t=a:x=1", "1:deadbeef"));
    EXPECT_OUTPUT(h, "");
    ASSERT_TRUE(h.state != nullptr);

    /* Re-registration replaces the list. */
    {
        bool has_event = false;
        ASSERT_TRUE(h.cmdEvent("t=a", "image/png", &has_event) == Event::registration);
        ASSERT_TRUE(has_event);
    }
    {
        State::MimeIterator it = h.registered()->registeredMimes();
        osc::ZStr v;
        ASSERT_TRUE(it.next(&v) && std::string(v.ptr, v.len) == "image/png");
        ASSERT_TRUE(!it.next(&v));
    }

    /* Registering without a list is the common case. */
    {
        bool has_event = false;
        ASSERT_TRUE(h.cmdEvent("t=a", nullptr, &has_event) == Event::registration);
        ASSERT_TRUE(has_event);
    }
    {
        State::MimeIterator it = h.registered()->registeredMimes();
        osc::ZStr v;
        ASSERT_TRUE(!it.next(&v));
    }

    /* Unregistration frees it and reports the change. */
    {
        bool has_event = false;
        ASSERT_TRUE(h.cmdEvent("t=A", nullptr, &has_event) == Event::registration);
        ASSERT_TRUE(has_event);
    }
    EXPECT_OUTPUT(h, "");
    ASSERT_TRUE(h.state == nullptr);

    /* Unregistering again changes nothing. */
    ASSERT_TRUE(h.cmdNoEvent("t=A", nullptr));
    EXPECT_OUTPUT(h, "");
    ASSERT_TRUE(h.state == nullptr);
}

TEST(kitty_dnd, dnd__no_state_before_registration) {
    Harness h;

    /* State-dependent commands from an unregistered client allocate
     * nothing; a data request gets the error kitty sends from its
     * zeroed state. */
    h.cmd("t=m:o=1", "text/plain");
    h.cmd("t=r", nullptr);
    EXPECT_OUTPUT(h, "");
    h.cmd("t=r:x=1", nullptr);
    EXPECT_OUTPUT(h, "\x1b]72;t=R:x=1:m=0;ENOENT:no drop data available\x1b\\");
    ASSERT_TRUE(h.state == nullptr);
}

TEST(kitty_dnd, dnd__move_event_carries_position_operations_and_mime_list) {
    Harness h;

    h.cmd("t=a", "text/plain");

    ASSERT_TRUE(dragMove1(h.registered(), &h.output, moveEv(5, 3, 100, 60, true), "text/plain", "text/uri-list"));

    /* Note the trailing space after every MIME entry, matching kitty. */
    EXPECT_OUTPUT(h, "\x1b]72;t=m:x=5:y=3:X=100:Y=60:o=1:m=0;text/plain text/uri-list \x1b\\");
}

TEST(kitty_dnd, dnd__move_event_echoes_registration_client_id) {
    Harness h;

    h.cmd("t=a:i=7", "");
    ASSERT_TRUE(dragMove1(h.registered(), &h.output, moveEv(1, 2, 8, 16, true, true), "text/plain"));
    EXPECT_OUTPUT(h, "\x1b]72;t=m:x=1:y=2:X=8:Y=16:o=3:i=7:m=0;text/plain \x1b\\");
}

TEST(kitty_dnd, dnd__re_registration_updates_client_id_in_place) {
    Harness h;

    h.cmd("t=a:i=7", "");
    State *state = h.registered();
    h.cmd("t=a:i=9", "");
    /* Same allocation, new client ID. */
    ASSERT_TRUE(h.state == state);
    ASSERT_TRUE(9 == state->drop.client_id);
}

TEST(kitty_dnd, dnd__mime_list_sent_on_every_move) {
    Harness h;

    h.cmd("t=a", "");
    const MoveEvent ev = moveEv(0, 0, 0, 0, true);
    ASSERT_TRUE(dragMove1(h.registered(), &h.output, ev, "text/plain"));
    h.clear();

    /* Kitty resends the list even when unchanged; clients depend on it. */
    ASSERT_TRUE(dragMove1(h.registered(), &h.output, ev, "text/plain"));
    EXPECT_OUTPUT(h, "\x1b]72;t=m:x=0:y=0:X=0:Y=0:o=1:m=0;text/plain \x1b\\");
}

TEST(kitty_dnd, dnd__leave_event) {
    Harness h;

    h.cmd("t=a", "");
    ASSERT_TRUE(dragMove1(h.registered(), &h.output, moveEv(0, 0, 0, 0, true), "text/plain"));
    h.clear();

    h.registered()->dragLeave(talloc(), &h.output);
    EXPECT_OUTPUT(h, "\x1b]72;t=m:x=-1:y=-1\x1b\\");
}

TEST(kitty_dnd, dnd__client_acceptance_recorded) {
    Harness h;

    h.cmd("t=a", "");
    {
        dnd::Operation op;
        ASSERT_TRUE(!h.registered()->clientAccepted(&op));
    }

    {
        bool has_event = false;
        ASSERT_TRUE(h.cmdEvent("t=m:o=1", "text/plain", &has_event) == Event::acceptance);
        ASSERT_TRUE(has_event);
    }
    EXPECT_OUTPUT(h, "");
    {
        dnd::Operation op;
        ASSERT_TRUE(h.registered()->clientAccepted(&op));
        ASSERT_TRUE(dnd::Operation::copy == op);
    }

    /* Rejection. */
    {
        bool has_event = false;
        ASSERT_TRUE(h.cmdEvent("t=m:o=0", "", &has_event) == Event::acceptance);
        ASSERT_TRUE(has_event);
    }
    {
        dnd::Operation op;
        ASSERT_TRUE(h.registered()->clientAccepted(&op));
        ASSERT_TRUE(dnd::Operation::none == op);
    }
}

TEST(kitty_dnd, dnd__chunked_client_acceptance) {
    Harness h;

    h.cmd("t=a", "");

    /* Chunked accept: continuation metadata is ignored, the acceptance
     * is pending until the final chunk. */
    ASSERT_TRUE(h.cmdNoEvent("t=m:o=2:m=1", "text/pl"));
    {
        dnd::Operation op;
        ASSERT_TRUE(!h.registered()->clientAccepted(&op));
    }
    ASSERT_TRUE(h.cmdNoEvent("t=m:m=1", "ain text"));
    {
        bool has_event = false;
        ASSERT_TRUE(h.cmdEvent("t=m:m=0", "/html", &has_event) == Event::acceptance);
        ASSERT_TRUE(has_event);
    }
    {
        dnd::Operation op;
        ASSERT_TRUE(h.registered()->clientAccepted(&op));
        ASSERT_TRUE(dnd::Operation::move == op);
    }

    /* The accumulated list was converted to NUL-separated entries. */
    {
        const std::string got((const char *)h.registered()->drop.accepted_mimes.items,
                              h.registered()->drop.accepted_mimes.items_len);
        ASSERT_TRUE(got == std::string("text/plain\x00text/html\x00", 21));
    }
}

/* Wisp: `&.{ .{ .mime = ..., .data = ... }, ... }` */
static bool dragDrop1(State *s, std::string *w, const MoveEvent &ev, const Item *items, size_t n) {
    return s->dragDrop(talloc(), w, ev, items, n);
}

TEST(kitty_dnd, dnd__drop_and_data_serving_round_trip) {
    Harness h;

    h.cmd("t=a", "text/plain text/uri-list");

    const MoveEvent ev = moveEv(4, 2, 40, 20, true);
    const Item items[] = {
        Item(z("text/uri-list"), z("file:///tmp/a.txt\r\n")),
        Item(z("text/plain"), z("hello")),
    };
    ASSERT_TRUE(dragDrop1(h.registered(), &h.output, ev, items, 2));
    EXPECT_OUTPUT(h, "\x1b]72;t=M:x=4:y=2:X=40:Y=20:o=1:m=0;text/uri-list text/plain \x1b\\");

    /* Request the second MIME's data: base64 chunk plus the empty
     * end-of-data message. */
    h.cmd("t=r:x=2", nullptr);
    EXPECT_OUTPUT(h, "\x1b]72;t=r:x=2:m=0;aGVsbG8=\x1b\\"
                     "\x1b]72;t=r:x=2\x1b\\");

    /* Out-of-bounds request. */
    h.cmd("t=r:x=3", nullptr);
    EXPECT_OUTPUT(h, "\x1b]72;t=R:x=3:m=0;ENOENT:drop data request index out of bounds\x1b\\");

    /* Conclude: the performed operation is reported, held data is
     * freed, and further requests fail. */
    {
        bool has_event = false;
        ASSERT_TRUE(h.cmdEvent("t=r:o=1", nullptr, &has_event) == Event::concluded_copy);
        ASSERT_TRUE(has_event);
    }
    EXPECT_OUTPUT(h, "");
    ASSERT_TRUE(h.cmdNoEvent("t=r:o=1", nullptr));
    h.cmd("t=r:x=1", nullptr);
    EXPECT_OUTPUT(h, "\x1b]72;t=R:x=1:m=0;ENOENT:no drop data available\x1b\\");
}

TEST(kitty_dnd, dnd__empty_item_served_as_a_single_end_of_data_message) {
    Harness h;

    h.cmd("t=a", "");
    const Item items[] = {Item(z("text/plain"), z(""))};
    ASSERT_TRUE(dragDrop1(h.registered(), &h.output, moveEv(0, 0, 0, 0, true), items, 1));
    h.clear();

    /* Kitty's oracle (test_empty_data) asserts exactly one message:
     * the empty response is itself the end-of-data signal, and a
     * duplicate would be a second completion to the client. */
    h.cmd("t=r:x=1", nullptr);
    EXPECT_OUTPUT(h, "\x1b]72;t=r:x=1\x1b\\");
}

TEST(kitty_dnd, dnd__leave_without_hover_sends_nothing) {
    Harness h;

    /* Client registered but no move was ever forwarded (e.g. it
     * registered mid-drag): kitty only notifies hovered windows. */
    h.cmd("t=a", "");
    h.registered()->dragLeave(talloc(), &h.output);
    EXPECT_OUTPUT(h, "");
}

TEST(kitty_dnd, dnd__data_request_with_no_drop) {
    Harness h;

    h.cmd("t=a", "");
    h.cmd("t=r:x=1", nullptr);
    EXPECT_OUTPUT(h, "\x1b]72;t=R:x=1:m=0;ENOENT:no drop data available\x1b\\");
}

TEST(kitty_dnd, dnd__leave_after_drop_is_ignored) {
    Harness h;

    h.cmd("t=a", "");
    const Item items[] = {Item(z("text/plain"), z("x"))};
    ASSERT_TRUE(dragDrop1(h.registered(), &h.output, moveEv(0, 0, 0, 0, true), items, 1));
    h.clear();

    /* Some toolkits emit a leave for the drop itself; the held data
     * must survive so the client can still fetch it. */
    h.registered()->dragLeave(talloc(), &h.output);
    EXPECT_OUTPUT(h, "");

    h.cmd("t=r:x=1", nullptr);
    EXPECT_OUTPUT(h, "\x1b]72;t=r:x=1:m=0;eA==\x1b\\"
                     "\x1b]72;t=r:x=1\x1b\\");
}

TEST(kitty_dnd, dnd__new_drag_resets_held_drop_data) {
    Harness h;

    h.cmd("t=a", "");
    const MoveEvent ev = moveEv(0, 0, 0, 0, true);
    const Item items[] = {Item(z("text/plain"), z("old"))};
    ASSERT_TRUE(dragDrop1(h.registered(), &h.output, ev, items, 1));
    h.clear();

    /* A new drag entering resets the per-drag state including the held
     * items from the unconcluded previous drop. */
    ASSERT_TRUE(dragMove1(h.registered(), &h.output, ev, "text/plain"));
    h.clear();
    h.cmd("t=r:x=1", nullptr);
    EXPECT_OUTPUT(h, "\x1b]72;t=R:x=1:m=0;ENOENT:no drop data available\x1b\\");
}

TEST(kitty_dnd, dnd__remote_transfer_requests_refused) {
    Harness h;

    h.cmd("t=a", "");

    /* URI file content request. */
    h.cmd("t=r:x=1:y=2", nullptr);
    EXPECT_OUTPUT(h, "\x1b]72;t=R:x=1:y=2:m=0;EINVAL:remote drop data is not supported\x1b\\");

    /* Directory handle request. */
    h.cmd("t=r:Y=2:x=1", nullptr);
    EXPECT_OUTPUT(h, "\x1b]72;t=R:x=1:Y=2:m=0;EINVAL:remote drop data is not supported\x1b\\");
}

TEST(kitty_dnd, dnd__drag_out_refused) {
    Harness h;

    /* Enabling and disabling offers is accepted silently and allocates
     * nothing. */
    h.cmd("t=o:x=1", nullptr);
    h.cmd("t=o:x=2", nullptr);
    EXPECT_OUTPUT(h, "");
    ASSERT_TRUE(h.state == nullptr);

    /* Offering a drag is refused. */
    h.cmd("t=o:x=1", nullptr);
    h.cmd("t=o:o=3", "text/plain");
    EXPECT_OUTPUT(h, "\x1b]72;t=E:m=0;EPERM:drag out is not supported by this terminal\x1b\\");

    /* Starting a drag is refused, echoing the command's client id. */
    h.cmd("t=P:x=-1:i=9", nullptr);
    EXPECT_OUTPUT(h, "\x1b]72;t=E:i=9:m=0;EPERM:drag out is not supported by this terminal\x1b\\");
    ASSERT_TRUE(h.state == nullptr);
}

TEST(kitty_dnd, dnd__unregister_frees_held_drop_data) {
    Harness h;

    h.cmd("t=a", "");
    const Item items[] = {Item(z("text/plain"), z("x"))};
    ASSERT_TRUE(dragDrop1(h.registered(), &h.output, moveEv(0, 0, 0, 0, true), items, 1));
    h.clear();

    /* The testing allocator would report the held data as leaked if
     * unregistration didn't free the whole state. */
    h.cmd("t=A", nullptr);
    ASSERT_TRUE(h.state == nullptr);
}

TEST(kitty_dnd, dnd__chunked_registration_reuses_first_chunk_metadata) {
    Harness h;

    /* Registration split over two chunks: the first chunk allocates
     * the state and seeds chunk reassembly, so the continuation (which
     * carries a different type) is still treated as the registration. */
    ASSERT_TRUE(h.cmdNoEvent("t=a:i=4:m=1", "text/pla"));
    ASSERT_TRUE(h.state != nullptr);
    {
        bool has_event = false;
        ASSERT_TRUE(h.cmdEvent("t=q:m=0", "in", &has_event) == Event::registration);
        ASSERT_TRUE(has_event);
    }
    EXPECT_OUTPUT(h, "");
    ASSERT_TRUE(4 == h.registered()->drop.client_id);
    {
        State::MimeIterator it = h.registered()->registeredMimes();
        osc::ZStr v;
        ASSERT_TRUE(it.next(&v) && std::string(v.ptr, v.len) == "text/plain");
    }

    /* A query after the chunked command completes works again. */
    h.cmd("t=q", nullptr);
    EXPECT_OUTPUT(h, "\x1b]72;t=q\x1b\\");
}

TEST(kitty_dnd, dnd__malformed_metadata_ignored) {
    Harness h;

    h.cmd("t=a:zz=1", "");
    EXPECT_OUTPUT(h, "");
    ASSERT_TRUE(h.state == nullptr);

    /* Command with no type is ignored, matching kitty (the spec's
     * default of t=a is not honored by the reference implementation). */
    h.cmd("x=1", "");
    EXPECT_OUTPUT(h, "");
    ASSERT_TRUE(h.state == nullptr);
}

TEST(kitty_dnd, dnd__bel_terminator_echoed_in_responses) {
    Harness h;

    osc::Command cmd;
    cmd.key = osc::Command::Key::kitty_dnd_protocol;
    cmd.kitty_dnd_protocol.metadata = z("t=q");
    cmd.kitty_dnd_protocol.has_payload = false;
    cmd.kitty_dnd_protocol.payload = osc::ZStr();
    cmd.kitty_dnd_protocol.terminator = dnd::Terminator::bel;
    bool has_event = false;
    Event event = Event::registration;
    (void)dnd::handleCommand(&h.state, talloc(), &h.output, cmd, &has_event, &event);
    EXPECT_OUTPUT(h, "\x1b]72;t=q\x07");
}

TEST(kitty_dnd, dnd__kitten_0_47_conversation_replay) {
    /* This replays a conversation recorded from the reference client
     * (`kitten dnd --drop-anywhere=copy --drop text/plain:out.txt`,
     * kitten 0.47.0) driven over a pty by a harness that sent exactly
     * the bytes this engine produces. The kitten accepted the events,
     * wrote the dropped payload to disk intact, and concluded; its
     * client bytes are frozen here as an interop regression test. */
    Harness h;

    /* Startup: register with MIME list and machine ID, then the test
     * harness reset (unregister both directions, re-register). */
    h.cmd("t=a:m=0", "text/uri-list text/plain");
    h.cmd("t=a:x=1:m=0", "1:5cff8247c477900a8727e2281fe890252f8848f87c224dd8dd7fb6303e94ddbd");
    h.cmd("t=A", nullptr);
    ASSERT_TRUE(h.state == nullptr);
    h.cmd("t=o:x=2", nullptr);
    h.cmd("t=a:m=0", "text/uri-list text/plain");
    h.cmd("t=a:x=1:m=0", "1:5cff8247c477900a8727e2281fe890252f8848f87c224dd8dd7fb6303e94ddbd");
    EXPECT_OUTPUT(h, "");
    ASSERT_TRUE(h.state != nullptr);

    /* Native drag moves over the terminal and drops. */
    const MoveEvent ev = moveEv(2, 1, 20, 18, true);
    ASSERT_TRUE(dragMove1(h.registered(), &h.output, ev, "text/plain"));
    EXPECT_OUTPUT(h, "\x1b]72;t=m:x=2:y=1:X=20:Y=18:o=1:m=0;text/plain \x1b\\");

    /* The kitten accepts as a copy of text/plain. */
    h.cmd("t=m:o=1:m=0", "text/plain");
    EXPECT_OUTPUT(h, "");
    {
        dnd::Operation op;
        ASSERT_TRUE(h.registered()->clientAccepted(&op));
        ASSERT_TRUE(dnd::Operation::copy == op);
    }

    const Item items[] = {Item(z("text/plain"), z("hello from ghostty\n"))};
    ASSERT_TRUE(dragDrop1(h.registered(), &h.output, ev, items, 1));
    EXPECT_OUTPUT(h, "\x1b]72;t=M:x=2:y=1:X=20:Y=18:o=1:m=0;text/plain \x1b\\");

    /* The kitten requests the data and concludes with a copy. */
    h.cmd("t=r:x=1", nullptr);
    EXPECT_OUTPUT(h, "\x1b]72;t=r:x=1:m=0;aGVsbG8gZnJvbSBnaG9zdHR5Cg==\x1b\\"
                     "\x1b]72;t=r:x=1\x1b\\");
    h.cmd("t=r:o=1", nullptr);
    EXPECT_OUTPUT(h, "");
    ASSERT_TRUE(!h.registered()->drop.has_items);
}

TEST(kitty_dnd, dnd__large_data_served_in_chunks) {
    Harness h;

    h.cmd("t=a", "");

    /* 3073 bytes: one full chunk plus one byte. */
    const std::string data(3073, 'Z');
    const Item items[] = {
        Item(z("application/octet-stream"), osc::ZStr(data.data(), data.size())),
    };
    ASSERT_TRUE(dragDrop1(h.registered(), &h.output, moveEv(0, 0, 0, 0, true), items, 1));
    h.clear();

    h.cmd("t=r:x=1", nullptr);
    const std::string out = h.output;

    /* First chunk is m=1 with 4096 base64 chars, second is m=0, and
     * the final message is the bare end-of-data marker. */
    const std::string first_prefix = "\x1b]72;t=r:x=1:m=1;";
    ASSERT_TRUE(out.compare(0, first_prefix.size(), first_prefix) == 0);
    ASSERT_TRUE(out.find("\x1b]72;t=r:x=1:m=0;") != std::string::npos);
    const std::string suffix = "\x1b]72;t=r:x=1\x1b\\";
    ASSERT_TRUE(out.size() >= suffix.size() &&
                out.compare(out.size() - suffix.size(), suffix.size(), suffix) == 0);
    h.clear();
}

TEST(kitty_dnd, dnd__over_cap_registration_list_never_completes) {
    Harness h;

    /* Matching kitty, a chunk that would exceed the cap is dropped and
     * the registration is not reported, though the client stays
     * registered (the state exists). */
    const std::string big(dnd::max_mime_list_bytes + 1, 'a');
    {
        bool has_event = false;
        Event event = Event::registration;
        osc::Command cmd;
        cmd.key = osc::Command::Key::kitty_dnd_protocol;
        cmd.kitty_dnd_protocol.metadata = z("t=a");
        cmd.kitty_dnd_protocol.has_payload = true;
        cmd.kitty_dnd_protocol.payload = osc::ZStr(big.data(), big.size());
        cmd.kitty_dnd_protocol.terminator = dnd::Terminator::st;
        (void)dnd::handleCommand(&h.state, talloc(), &h.output, cmd, &has_event, &event);
        ASSERT_TRUE(!has_event);
    }
    ASSERT_TRUE(h.state != nullptr);
    {
        State::MimeIterator it = h.registered()->registeredMimes();
        osc::ZStr v;
        ASSERT_TRUE(!it.next(&v));
    }
}

TEST(kitty_dnd, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
