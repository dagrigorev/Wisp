/* Wisp: shared scaffolding for the tests transliterated from Ghostty's
 * Screen.zig, Selection.zig and formatter.zig test blocks. */

#pragma once

#include <string.h>
#include <string>

#include "test_helpers.h"
#include "../vt/formatter.hpp"

using namespace wisp;
using namespace wisp::vt;

typedef PageList::Pin Pin;
typedef page::Page Page;
typedef page::Row Row;
typedef page::Cell Cell;
typedef point::Point Point;

static zigstd::Allocator talloc() { return zigstd::testing_allocator(); }

struct ScreenHolder {
    Screen s;
    bool ok;
    ScreenHolder(const Screen::Options &o) { ok = Screen::init(talloc(), o, &s); }
    ~ScreenHolder() {
        if (ok) s.deinit();
    }
};

/* Wisp: shorthands for upstream's repeated test scaffolding.
 *   var s = try Screen.init(io, alloc, .{...}); defer s.deinit();   SCREEN(s, cols, rows, sb)
 *   try s.testWriteString(str)                                        WRITE(s, str)
 *   expectEqualStrings(str, dumpStringAlloc(alloc, .{ .tag = .{} }))  EXPECT_DUMP(s, tag, str) */
#define SCREEN_OPTS(var, opts)                                                                                             ScreenHolder var##_holder(opts);                                                                                       ASSERT_TRUE(var##_holder.ok);                                                                                          Screen &var = var##_holder.s
#define SCREEN(var, c, r, sb) SCREEN_OPTS(var, Screen::Options((c), (r), (sb)))
#define WRITE(s, str) ASSERT_TRUE((s).testWriteString(str))
#define EXPECT_STR(expected, actual)                                                                                       do {                                                                                                                       const std::string _actual = (actual);                                                                                  ASSERT_STR_EQ(expected, _actual.c_str());                                                                          } while (0)
#define EXPECT_DUMP(s, tag, expected) EXPECT_STR(expected, (s).dumpStringAlloc(Point::tag()))
#define EXPECT_DUMP_UNWRAPPED(s, tag, expected) EXPECT_STR(expected, (s).dumpStringAllocUnwrapped(Point::tag()))
#define NOERR(e) ASSERT_TRUE((e) == PageList::IncreaseCapacityError::none)

static const size_t unlimited = SIZE_MAX;

static terminal::sgr::Attribute attr(terminal::sgr::Attribute::Tag t) { return terminal::sgr::Attribute::make(t); }
typedef terminal::sgr::Attribute::Tag A;

static PageList::IncreaseCapacityError startLink(Screen &s, const char *uri, const char *id = nullptr) {
    return s.startHyperlink((const uint8_t *)uri, strlen(uri), (const uint8_t *)id, id ? strlen(id) : 0);
}

/* Wisp: expectEqual(point.Point{...}, pointFromPin(...).?) */
static bool ptEq(const Maybe<Point> &m, const Point &p) { return m.has && m.value.eql(p); }
/* Wisp: expectEqual(expected Selection, actual ?Selection) */
static bool selEq(const Maybe<Selection> &m, const Selection &e) {
    return m.has && m.value.bounds.tag == e.bounds.tag && m.value.eql(e);
}
static bool rgbEq(const Cell::RGB &a, uint8_t r, uint8_t g, uint8_t b) { return a.r == r && a.g == g && a.b == b; }
static Pin pinAt(const Screen &s, const Point &p) { return s.pages.pin(p).value; }

/* Wisp: var sel = s.selectX(...).?; defer sel.deinit(&s); */
struct SelDeinit {
    Selection *sel;
    Screen *screen;
    ~SelDeinit() { sel->deinit(screen); }
};
#define SEL(var, scr, e)                                                                                                   const Maybe<Selection> var##_maybe = (e);                                                                              ASSERT_TRUE(var##_maybe.has);                                                                                          Selection var = var##_maybe.value;                                                                                     SelDeinit var##_deinit = {&var, &(scr)};                                                                               (void)var##_deinit

/* Wisp: defer s.pages.pauseIntegrityChecks(false); */
struct PauseGuard {
    PageList *pl;
    ~PauseGuard() { pl->pauseIntegrityChecks(false); }
};

/* Wisp: .{ .pin = p, .whitespace = null, .semantic_prompt_boundary = b } */
static Screen::SelectLine selLine(const Pin &p, bool whitespace_null = false, bool semantic_prompt_boundary = true) {
    Screen::SelectLine l(p);
    if (whitespace_null) {
        l.whitespace = nullptr;
        l.whitespace_len = SIZE_MAX;
    }
    l.semantic_prompt_boundary = semantic_prompt_boundary;
    return l;
}
