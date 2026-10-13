/* Transliterated from the test blocks in Ghostty
 * src/terminal/compress/lz4_differential.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 * Wisp: the suite machinery is in src/vt/compress/lz4_differential.hpp;
 * these are the two upstream `test` blocks. `error.SkipZigTest` has no
 * equivalent in the port's test framework, so the exhaustive test returns
 * without asserting when GHOSTTY_LZ4_SLOW is unset.
 */

#include <stdlib.h>

#include "test_helpers.h"
#include "../vt/compress/lz4_differential.hpp"

using namespace wisp;
using namespace wisp::vt::lz4_differential;

TEST(lz4_differential, light) {
    /* Wisp: 0x4C5A_3403 is a u64 seed. */
    ASSERT_TRUE(runSuite(light_budget, 0x4C5A3403ull));
}

TEST(lz4_differential, exhaustive) {
    /* Slow. Enable explicitly, ideally together with a test filter:
     *   GHOSTTY_LZ4_SLOW=1 zig build test -Dtest-filter="lz4 differential" */
    /* Wisp: std.testing.environ.containsConstant is getenv here. */
    if (getenv("GHOSTTY_LZ4_SLOW") == nullptr) return;

    /* Several independent seeds; the suite is deterministic per seed. */
    for (uint64_t seed = 0; seed < 4; seed++) {
        /* Wisp: 0x4C5A_4000 + seed is u64 addition. */
        ASSERT_TRUE(runSuite(exhaustive_budget, 0x4C5A4000ull + seed));
    }
}

TEST(lz4_differential, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
