/* Transliterated from the test blocks in Ghostty
 * src/terminal/stream_continuation.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include <stdio.h>
#include <string.h>

#include <string>

#include "test_helpers.h"
#include "../terminal/stream_continuation.hpp"

using namespace wisp;

namespace sc = wisp::terminal::stream_continuation;

typedef sc::BoundaryScanner BoundaryScanner;
typedef BoundaryScanner::Effect Effect;
typedef sc::Tracker Tracker;

static zigstd::Allocator talloc() { return zigstd::testing_allocator(); }

/* Wisp: `tracker.append(.vt, "literal")` with the length taken from the
 * literal, and `Tracker.init`/`deinit` as a scope guard. */
struct TrackerHolder {
    Tracker t;
    explicit TrackerHolder(size_t max_bytes) { t = Tracker::init(talloc(), max_bytes); }
    ~TrackerHolder() { t.deinit(); }
};

#define APPEND(tr, pending, lit) (tr).append(Tracker::Pending::pending, (const uint8_t *)(lit), sizeof(lit) - 1)

/* Wisp: `findVTReplayStart("literal")` returning `?usize`. */
static sc::Maybe<size_t> vtStart(const char *s, size_t len) {
    return sc::findVTReplayStart((const uint8_t *)s, len);
}
static sc::Maybe<size_t> utf8Start(const char *s, size_t len) {
    return sc::findUtf8ReplayStart((const uint8_t *)s, len);
}

static bool isNone(const sc::Maybe<size_t> &m) { return !m.has; }
static bool isAt(const sc::Maybe<size_t> &m, size_t idx) { return m.has && m.value == idx; }

TEST(stream_continuation, boundary_scanner_classifies_effects_and_ground) {
    BoundaryScanner scanner;

    ASSERT_TRUE(scanner.ground());
    ASSERT_TRUE(Effect::committed == scanner.next('A'));
    ASSERT_TRUE(scanner.ground());

    ASSERT_TRUE(Effect::uncommitted == scanner.next(0x1B));
    ASSERT_TRUE(!scanner.ground());
    ASSERT_TRUE(Effect::uncommitted == scanner.next('['));
    ASSERT_TRUE(Effect::uncommitted == scanner.next('1'));

    /* BEL commits an execute action without changing the unfinished CSI. */
    ASSERT_TRUE(Effect::omittable == scanner.next(0x07));
    ASSERT_TRUE(!scanner.ground());

    ASSERT_TRUE(Effect::committed == scanner.next('m'));
    ASSERT_TRUE(scanner.ground());
}

TEST(stream_continuation, boundary_scanner_preserves_unfinished_builder_actions) {
    {
        BoundaryScanner apc;
        ASSERT_TRUE(Effect::uncommitted == apc.next(0x1B));
        ASSERT_TRUE(Effect::uncommitted == apc.next('_'));
        ASSERT_TRUE(Effect::uncommitted == apc.next('G'));
        ASSERT_TRUE(Effect::committed == apc.next(0x1B));
    }

    {
        BoundaryScanner dcs;
        ASSERT_TRUE(Effect::uncommitted == dcs.next(0x1B));
        ASSERT_TRUE(Effect::uncommitted == dcs.next('P'));
        ASSERT_TRUE(Effect::uncommitted == dcs.next('q'));
        ASSERT_TRUE(Effect::uncommitted == dcs.next('x'));
        ASSERT_TRUE(Effect::committed == dcs.next(0x1B));
    }
}

TEST(stream_continuation, boundary_scanner_handles_UTF_8_and_malformed_retries) {
    {
        BoundaryScanner valid;
        ASSERT_TRUE(Effect::uncommitted == valid.next(0xF0));
        ASSERT_TRUE(!valid.ground());
        ASSERT_TRUE(Effect::uncommitted == valid.next(0x9F));
        ASSERT_TRUE(Effect::uncommitted == valid.next(0x98));
        ASSERT_TRUE(Effect::committed == valid.next(0x84));
        ASSERT_TRUE(valid.ground());
    }

    {
        BoundaryScanner malformed;
        ASSERT_TRUE(Effect::uncommitted == malformed.next(0xE0));
        ASSERT_TRUE(Effect::uncommitted == malformed.next(0xA0));
        ASSERT_TRUE(malformed.next(0xF0) != Effect::uncommitted);
        ASSERT_TRUE(!malformed.ground());
    }
}

TEST(stream_continuation, findVTReplayStart_finds_the_latest_ESC_across_vector_boundaries) {
    ASSERT_TRUE(isNone(vtStart("", 0)));
    ASSERT_TRUE(isNone(vtStart("no escapes here", 15)));
    ASSERT_TRUE(isAt(vtStart("\x1b", 1), 0));
    ASSERT_TRUE(isAt(vtStart("\x1b[1m\x20\x1b[2", 9), 5));

    /* Exercise both the vector loop and the scalar remainder with replay
     * starts at every position of a buffer larger than any vector width. */
    char buf[193];
    memset(buf, 'a', sizeof buf);
    ASSERT_TRUE(isNone(vtStart(buf, sizeof buf)));
    for (size_t idx = 0; idx < sizeof buf; idx++) {
        memset(buf, 'a', sizeof buf);
        buf[idx] = 0x1B;
        ASSERT_TRUE(isAt(vtStart(buf, sizeof buf), idx));

        /* The latest ESC wins. */
        if (idx > 0) {
            buf[idx - 1] = 0x1B;
            ASSERT_TRUE(isAt(vtStart(buf, sizeof buf), idx));
        }
    }
}

TEST(stream_continuation, findUtf8ReplayStart_finds_the_pending_lead_byte) {
    ASSERT_TRUE(isNone(utf8Start("", 0)));
    ASSERT_TRUE(isAt(utf8Start("text\xF0", 5), 4));
    ASSERT_TRUE(isAt(utf8Start("text\xF0\x9F", 6), 4));
    ASSERT_TRUE(isAt(utf8Start("text\xF0\x9F\x98", 7), 4));
    ASSERT_TRUE(isAt(utf8Start("\xE0\xA0", 2), 0));

    /* A malformed prefix rejected earlier doesn't hide the pending lead. */
    ASSERT_TRUE(isAt(utf8Start("\xE0\xA0\xF0", 3), 2));

    /* Continuation bytes of a sequence that started in an earlier input have
     * no replay start of their own. */
    ASSERT_TRUE(isNone(utf8Start("\x9F", 1)));
    ASSERT_TRUE(isNone(utf8Start("\x9F\x98", 2)));
}

TEST(stream_continuation, tracker_retains_and_normalizes_replay_safe_bytes) {
    TrackerHolder holder(64);
    Tracker &tracker = holder.t;

    APPEND(tracker, vt, "committed\x1b[1\x07");
    APPEND(tracker, vt, ";2");
    ASSERT_TRUE(!tracker.broken);

    {
        std::string writer;
        tracker.write(&writer);
        ASSERT_TRUE(writer == std::string("\x1b[1;2"));
    }

    /* A feed with a later replay start drops the previous suffix entirely. */
    APPEND(tracker, vt, "committed\x1b]0;t");
    {
        std::string writer;
        tracker.write(&writer);
        ASSERT_TRUE(writer == std::string("\x1b]0;t"));
    }

    /* An incomplete UTF-8 codepoint seeds at its lead byte and grows with
     * continuation bytes from later feeds. */
    APPEND(tracker, utf8, "committed\xF0");
    APPEND(tracker, utf8, "\x9F");
    {
        std::string writer;
        tracker.write(&writer);
        ASSERT_TRUE(writer == std::string("\xF0\x9F"));
    }
}

TEST(stream_continuation, tracker_cap_reset_and_broken_recovery) {
    TrackerHolder holder(4);
    Tracker &tracker = holder.t;

    APPEND(tracker, vt, "\x1b[123");
    ASSERT_TRUE(tracker.broken);

    /* Feeds without a replay start are dropped while broken. */
    APPEND(tracker, vt, "4");
    ASSERT_TRUE(tracker.broken);
    ASSERT_TRUE(0 == tracker.bytes.items_len);

    /* Suffixes that grow past the cap break tracking. */
    tracker.reset();
    APPEND(tracker, vt, "\x1b[1");
    APPEND(tracker, vt, "23");
    ASSERT_TRUE(tracker.broken);

    /* A new replay start that fits recovers without a reset. */
    APPEND(tracker, vt, "\x1b[");
    ASSERT_TRUE(!tracker.broken);
    {
        std::string writer;
        tracker.write(&writer);
        ASSERT_TRUE(writer == std::string("\x1b["));
    }

    APPEND(tracker, vt, "\x1b[123");
    ASSERT_TRUE(tracker.broken);
    tracker.reset();
    ASSERT_TRUE(!tracker.broken);
    {
        std::string writer;
        tracker.write(&writer);
        ASSERT_TRUE(0 == writer.size());
    }
}

/* Wisp: upstream's "tracker reports writer failure and defers allocation
 * failure" has two halves. The writer-failure half asserts
 * error.WriteFailed from a fixed 1-byte writer; a std::string writer cannot
 * fail, so only the allocation half is ported. */
TEST(stream_continuation, tracker_defers_allocation_failure) {
    zigstd::FailingAllocator failing(talloc(), 0);
    Tracker failing_tracker = Tracker::init(failing.allocator(), 64);
    ASSERT_TRUE(!failing_tracker.broken);

    /* The best-effort initial reservation failed, so the first required
     * retention retries allocation and marks the tracker broken. */
    APPEND(failing_tracker, vt, "\x1b[");
    ASSERT_TRUE(failing_tracker.broken);

    failing_tracker.deinit();
}

TEST(stream_continuation, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
