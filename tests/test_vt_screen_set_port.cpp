/* Transliterated from the test blocks in Ghostty src/terminal/ScreenSet.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names. Built with
 * WISP_IS_TEST and WISP_SLOW_RUNTIME_SAFETY, as upstream's tests run.
 */

#include "test_helpers.h"
#include "../vt/screen_set.hpp"

using namespace wisp;
using namespace wisp::vt;

typedef ScreenSet::Key Key;

static zigstd::Allocator talloc() { return zigstd::testing_allocator(); }

struct SetHolder {
    ScreenSet set;
    bool ok;
    SetHolder() { ok = ScreenSet::init(talloc(), Screen::Options::default_(), &set); }
    ~SetHolder() {
        if (ok) set.deinit(talloc());
    }
};

TEST(screen_set, ScreenSet) {
    SetHolder h;
    ASSERT_TRUE(h.ok);
    ScreenSet &set = h.set;
    ASSERT_TRUE(Key::primary == set.active_key);
    ASSERT_TRUE(0 == set.generation(Key::primary));
    ASSERT_TRUE(0 == set.generation(Key::alternate));

    /* Initialize a secondary screen */
    ASSERT_TRUE(set.getInit(talloc(), Key::alternate, Screen::Options::default_()) != nullptr);
    ASSERT_TRUE(0 == set.generation(Key::alternate));

    set.switchTo(Key::alternate);
    ASSERT_TRUE(Key::alternate == set.active_key);
}

TEST(screen_set, ScreenSet_generations) {
    SetHolder h;
    ASSERT_TRUE(h.ok);
    ScreenSet &set = h.set;

    ASSERT_TRUE(0 == set.generation(Key::primary));
    ASSERT_TRUE(0 == set.generation(Key::alternate));

    /* A no-op removal doesn't change the generation. */
    set.remove(talloc(), Key::alternate);
    ASSERT_TRUE(0 == set.generation(Key::alternate));

    /* Initializing a screen doesn't change the generation. */
    ASSERT_TRUE(set.getInit(talloc(), Key::alternate, Screen::Options::default_()) != nullptr);
    ASSERT_TRUE(0 == set.generation(Key::alternate));

    const size_t alternate_generation = set.generation(Key::alternate);
    set.remove(talloc(), Key::alternate);
    ASSERT_TRUE(alternate_generation + 1 == set.generation(Key::alternate));

    /* Reinitializing keeps the generation from the last removal, so stale
     * handles can distinguish the new screen from the destroyed screen. */
    ASSERT_TRUE(set.getInit(talloc(), Key::alternate, Screen::Options::default_()) != nullptr);
    ASSERT_TRUE(alternate_generation + 1 == set.generation(Key::alternate));
    ASSERT_TRUE(0 == set.generation(Key::primary));
}

/* Wisp: std.testing.allocator's leak check. Runs last (registration order). */
TEST(screen_set, zz_Wisp_no_leaks) { ASSERT_TRUE(zigstd::testing_state().live == 0); }
