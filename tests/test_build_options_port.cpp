/* Transliterated from the test blocks in Ghostty
 * src/terminal/build_options.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Upstream has one test, `parse`, nested inside `Options.Features`, plus
 * a bare `test { _ = Options.Features; }` reference block that exists
 * only to pull the nested tests in. That reference block has nothing to
 * assert and is not ported.
 *
 * Wisp: upstream compares whole Features values with
 * `testing.expectEqual`, which for a packed struct compares the backing
 * integer. Here each comparison goes through `Features::eql`, which is
 * field-wise -- a C++ bool is not a bit, so the struct has padding and
 * comparing it bytewise would be undefined. The assertions are otherwise
 * upstream's, in upstream's order.
 */

#include <string.h>

#include "test_helpers.h"
#include "../vt/build_options.hpp"

using namespace wisp;

namespace build_options = wisp::vt::build_options;

typedef build_options::Features Features;

/* Wisp: upstream writes `try Features.parse(s)`, where a parse failure
 * propagates as an error and fails the test. The port returns false with
 * the value in an out parameter, so both halves are asserted in the test
 * body below. These helpers deliberately do not assert: test_helpers'
 * ASSERT returns from the enclosing function, so it is only usable in a
 * void one. */
static bool parseInto(const char *s, Features *out) {
    return Features::parse(s, strlen(s), out);
}

TEST(build_options, parse) {
    const Features none = Features::none();
    Features got;

    /* Empty input keeps the defaults. */
    ASSERT_TRUE(parseInto("", &got));
    ASSERT_TRUE(Features().eql(got));

    /* All three token forms, comma separated. */
    {
        Features expect;
        expect.snapshot = false;
        ASSERT_TRUE(parseInto("-snapshot", &got));
        ASSERT_TRUE(expect.eql(got));
    }
    {
        Features expect;
        expect.snapshot = false;
        expect.formatter = false;
        ASSERT_TRUE(parseInto("-snapshot,+render_state,-formatter,color", &got));
        ASSERT_TRUE(expect.eql(got));
    }

    /* The special "all" name refers to every feature. */
    ASSERT_TRUE(parseInto("-all", &got));
    ASSERT_TRUE(none.eql(got));
    ASSERT_TRUE(parseInto("-all,+all", &got));
    ASSERT_TRUE(Features().eql(got));
    {
        Features render_only = none;
        render_only.render_state = true;
        ASSERT_TRUE(parseInto("-all,+render-state", &got));
        ASSERT_TRUE(render_only.eql(got));
    }

    /* Hyphens are interchangeable with underscores. */
    {
        Features expect;
        expect.input_encode = false;
        expect.grid_introspection = false;
        ASSERT_TRUE(parseInto("-input-encode,-grid_introspection", &got));
        ASSERT_TRUE(expect.eql(got));
    }

    /* Later tokens win. */
    ASSERT_TRUE(parseInto("-snapshot,+snapshot", &got));
    ASSERT_TRUE(Features().eql(got));

    /* Unknown names and dangling signs are errors. */
    ASSERT_FALSE(parseInto("bogus", &got));
    ASSERT_FALSE(parseInto("-snapshot,-", &got));
}
