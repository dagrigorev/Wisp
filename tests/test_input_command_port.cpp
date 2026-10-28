/* Transliterated from the test blocks in Ghostty src/input/command.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include "test_helpers.h"

#include "../src/input/command.hpp"

using namespace wisp::input::command;

TEST(input_command, command_defaults) {
    /* This just ensures that defaults is analyzed and works. */
    ASSERT_TRUE(defaults().size() > 0);
    ASSERT_EQ((long)defaults().size(), (long)defaultsC().size());
}

/* Wisp: upstream checks the table at comptime; pin what that guarantees. */
TEST(input_command, zz_Wisp_defaults_table) {
    const std::vector<Command> &d = defaults();
    ASSERT_EQ(90L, (long)d.size());
    for (size_t i = 1; i < d.size(); i++) ASSERT_FALSE(Command::lessThan(d[i], d[i - 1]));
    ASSERT_TRUE(std::string("Change Tab Title\xE2\x80\xA6") == d.front().title);
    ASSERT_TRUE(std::string("Undo") == d.back().title);
    ASSERT_TRUE(Command::lessThan(Command(Action(), "Close Tab"), Command(Action(), "close tabs to the right")));
    ASSERT_FALSE(Command::lessThan(Command(Action(), "close tabs to the right"), Command(Action(), "Close Tab")));

    bool found = false;
    for (size_t i = 0; i < d.size(); i++) {
        if (d[i].title != "Ghostty") continue;
        found = true;
        ASSERT_TRUE(std::string("text") == defaultsC()[i].action_key);
        ASSERT_TRUE(std::string("text:\xF0\x9F\x91\xBB") == defaultsC()[i].action);
        ASSERT_TRUE(std::string("Ghostty") == defaultsC()[i].title);
    }
    ASSERT_TRUE(found);
}
