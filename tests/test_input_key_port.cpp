/* Transliterated from the test blocks in Ghostty src/input/key.zig and
 * src/input/key_mods.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Wisp:
 *   - key_mods.zig's unnamed `test {}` inside Mods is named
 *     Mods_backing_layout here.
 *   - "translation macos-option-as-alt" is skipped upstream on every
 *     non-Darwin OS and so is not ported (Wisp targets Windows).
 */

#include <string.h>

#include <string>

#include "test_helpers.h"
#include "../src/input/key.hpp"
#include "../src/config/formatter.hpp"
#include "../src/input/key_mods.hpp"

using namespace wisp;
using namespace wisp::input;

using ::wisp::config::formatter::entryFormatter;

static Mods sided(Mod mod, ModSide side) { return RemapSet::initMods(mod, side); }

/* ---- key.zig ---- */

TEST(input_key, fromASCII_should_not_return_keypad_keys) {
    Key k;
    ASSERT_TRUE(fromASCII('0', &k));
    ASSERT_TRUE(k == Key::digit_0);
    ASSERT_FALSE(fromASCII('*', &k));
}

TEST(input_key, keypad_keys) {
    ASSERT_TRUE(keypad(Key::numpad_0));
    ASSERT_FALSE(keypad(Key::digit_1));
}

TEST(input_key, w3c) {
    /* All our keys should convert to and from the W3C format.
     * We don't support every key in the W3C spec, so we only
     * check the enum fields. */
    for (size_t i = 0; i < key_count; i++) {
        const Key key = (Key)i;
        const std::string w3c_name = w3c(key);
        Key round_trip;
        ASSERT_TRUE(fromW3C(w3c_name, &round_trip));
        ASSERT_TRUE(key == round_trip);
    }
}

TEST(input_key, ctrlOrSuper) {
    Mods m = ctrlOrSuper(Mods());

    ASSERT_TRUE(m.ctrlOrSuper());
}

/* ---- key_mods.zig ---- */

TEST(input_key_mods, Mods_backing_layout) {
    ASSERT_EQ((long)Mods().int_(), (long)0x0);
    Mods shift;
    shift.shift = true;
    ASSERT_EQ((long)shift.int_(), (long)0x1);
}

TEST(input_key_mods, RemapSet_unsided_remap_creates_both_left_and_right_mappings) {
    RemapSet set = RemapSet::empty();
    ASSERT_TRUE(set.parse("ctrl=super") == RemapSet::ParseError::none);
    set.finalize();
    ASSERT_TRUE(sided(Mod::super, ModSide::left) == set.apply(sided(Mod::ctrl, ModSide::left)));
    ASSERT_TRUE(sided(Mod::super, ModSide::left) == set.apply(sided(Mod::ctrl, ModSide::right)));
    set.deinit();
}

TEST(input_key_mods, RemapSet_sided_from_only_maps_that_side) {
    RemapSet set = RemapSet::empty();

    ASSERT_TRUE(set.parse("left_alt=ctrl") == RemapSet::ParseError::none);
    set.finalize();

    const Mods left_alt = sided(Mod::alt, ModSide::left);
    const Mods left_ctrl = sided(Mod::ctrl, ModSide::left);
    ASSERT_TRUE(left_ctrl == set.apply(left_alt));

    const Mods right_alt = sided(Mod::alt, ModSide::right);
    ASSERT_TRUE(right_alt == set.apply(right_alt));
    set.deinit();
}

TEST(input_key_mods, RemapSet_sided_to) {
    RemapSet set = RemapSet::empty();

    ASSERT_TRUE(set.parse("ctrl=right_super") == RemapSet::ParseError::none);
    set.finalize();

    const Mods left_ctrl = sided(Mod::ctrl, ModSide::left);
    const Mods right_super = sided(Mod::super, ModSide::right);
    ASSERT_TRUE(right_super == set.apply(left_ctrl));
    set.deinit();
}

TEST(input_key_mods, RemapSet_both_sides_specified) {
    RemapSet set = RemapSet::empty();

    ASSERT_TRUE(set.parse("left_shift=right_ctrl") == RemapSet::ParseError::none);
    set.finalize();

    const Mods left_shift = sided(Mod::shift, ModSide::left);
    const Mods right_ctrl = sided(Mod::ctrl, ModSide::right);
    ASSERT_TRUE(right_ctrl == set.apply(left_shift));
    set.deinit();
}

TEST(input_key_mods, RemapSet_multiple_parses_accumulate) {
    RemapSet set = RemapSet::empty();

    ASSERT_TRUE(set.parse("left_ctrl=super") == RemapSet::ParseError::none);
    ASSERT_TRUE(set.parse("left_alt=ctrl") == RemapSet::ParseError::none);
    set.finalize();

    const Mods left_ctrl = sided(Mod::ctrl, ModSide::left);
    const Mods left_super = sided(Mod::super, ModSide::left);
    ASSERT_TRUE(left_super == set.apply(left_ctrl));

    const Mods left_alt = sided(Mod::alt, ModSide::left);
    const Mods left_ctrl_result = sided(Mod::ctrl, ModSide::left);
    ASSERT_TRUE(left_ctrl_result == set.apply(left_alt));
    set.deinit();
}

TEST(input_key_mods, RemapSet_error_on_missing_assignment) {
    RemapSet set = RemapSet::empty();

    ASSERT_TRUE(set.parse("ctrl") == RemapSet::ParseError::MissingAssignment);
    ASSERT_TRUE(set.parse("") == RemapSet::ParseError::MissingAssignment);
    set.deinit();
}

TEST(input_key_mods, RemapSet_error_on_invalid_modifier) {
    RemapSet set = RemapSet::empty();

    ASSERT_TRUE(set.parse("invalid=ctrl") == RemapSet::ParseError::InvalidMod);
    ASSERT_TRUE(set.parse("ctrl=invalid") == RemapSet::ParseError::InvalidMod);
    ASSERT_TRUE(set.parse("middle_ctrl=super") == RemapSet::ParseError::InvalidMod);
    set.deinit();
}

TEST(input_key_mods, RemapSet_isRemapped_checks_mask) {
    RemapSet set = RemapSet::empty();

    ASSERT_TRUE(set.parse("ctrl=super") == RemapSet::ParseError::none);
    set.finalize();

    Mods ctrl;
    ctrl.ctrl = true;
    Mods alt;
    alt.alt = true;
    Mods shift;
    shift.shift = true;
    ASSERT_TRUE(set.isRemapped(ctrl));
    ASSERT_FALSE(set.isRemapped(alt));
    ASSERT_FALSE(set.isRemapped(shift));
    set.deinit();
}

TEST(input_key_mods, RemapSet_clone_creates_independent_copy) {
    RemapSet set = RemapSet::empty();

    ASSERT_TRUE(set.parse("ctrl=super") == RemapSet::ParseError::none);
    set.finalize();

    RemapSet cloned = set.clone();

    Mods ctrl;
    ctrl.ctrl = true;
    ASSERT_TRUE(set.equal(cloned));
    ASSERT_TRUE(cloned.isRemapped(ctrl));
    cloned.deinit();
    set.deinit();
}

TEST(input_key_mods, RemapSet_equal_compares_correctly) {
    RemapSet set1 = RemapSet::empty();
    RemapSet set2 = RemapSet::empty();

    ASSERT_TRUE(set1.equal(set2));

    ASSERT_TRUE(set1.parse("ctrl=super") == RemapSet::ParseError::none);
    ASSERT_FALSE(set1.equal(set2));

    ASSERT_TRUE(set2.parse("ctrl=super") == RemapSet::ParseError::none);
    ASSERT_TRUE(set1.equal(set2));

    ASSERT_TRUE(set1.parse("alt=shift") == RemapSet::ParseError::none);
    ASSERT_FALSE(set1.equal(set2));
    set1.deinit();
    set2.deinit();
}

TEST(input_key_mods, RemapSet_parseCLI_basic) {
    RemapSet set = RemapSet::empty();

    ASSERT_TRUE(set.parseCLI("ctrl=super") == RemapSet::CliError::none);
    ASSERT_EQ((long)2, (long)set.map.count());
    set.deinit();
}

TEST(input_key_mods, RemapSet_parseCLI_empty_clears) {
    RemapSet set = RemapSet::empty();

    ASSERT_TRUE(set.parseCLI("ctrl=super") == RemapSet::CliError::none);
    ASSERT_EQ((long)2, (long)set.map.count());

    ASSERT_TRUE(set.parseCLI("") == RemapSet::CliError::none);
    ASSERT_EQ((long)0, (long)set.map.count());
    set.deinit();
}

TEST(input_key_mods, RemapSet_parseCLI_invalid) {
    RemapSet set = RemapSet::empty();

    ASSERT_TRUE(set.parseCLI("foo=bar") == RemapSet::CliError::InvalidValue);
    ASSERT_TRUE(set.parseCLI("ctrl") == RemapSet::CliError::InvalidValue);
    set.deinit();
}

TEST(input_key_mods, RemapSet_parse_aliased_modifiers) {
    RemapSet set = RemapSet::empty();

    ASSERT_TRUE(set.parse("cmd=ctrl") == RemapSet::ParseError::none);
    set.finalize();

    const Mods left_super = sided(Mod::super, ModSide::left);
    const Mods left_ctrl = sided(Mod::ctrl, ModSide::left);
    ASSERT_TRUE(left_ctrl == set.apply(left_super));
    set.deinit();
}

TEST(input_key_mods, RemapSet_parse_aliased_modifiers_command) {
    RemapSet set = RemapSet::empty();

    ASSERT_TRUE(set.parse("command=alt") == RemapSet::ParseError::none);
    set.finalize();

    const Mods left_super = sided(Mod::super, ModSide::left);
    const Mods left_alt = sided(Mod::alt, ModSide::left);
    ASSERT_TRUE(left_alt == set.apply(left_super));
    set.deinit();
}

TEST(input_key_mods, RemapSet_parse_aliased_modifiers_opt_and_option) {
    RemapSet set = RemapSet::empty();

    ASSERT_TRUE(set.parse("opt=super") == RemapSet::ParseError::none);
    set.finalize();

    const Mods left_alt = sided(Mod::alt, ModSide::left);
    const Mods left_super = sided(Mod::super, ModSide::left);
    ASSERT_TRUE(left_super == set.apply(left_alt));

    set.deinit();
    set = RemapSet::empty();

    ASSERT_TRUE(set.parse("option=shift") == RemapSet::ParseError::none);
    set.finalize();

    const Mods left_shift = sided(Mod::shift, ModSide::left);
    ASSERT_TRUE(left_shift == set.apply(left_alt));
    set.deinit();
}

TEST(input_key_mods, RemapSet_parse_aliased_modifiers_control) {
    RemapSet set = RemapSet::empty();

    ASSERT_TRUE(set.parse("control=super") == RemapSet::ParseError::none);
    set.finalize();

    const Mods left_ctrl = sided(Mod::ctrl, ModSide::left);
    const Mods left_super = sided(Mod::super, ModSide::left);
    ASSERT_TRUE(left_super == set.apply(left_ctrl));
    set.deinit();
}

TEST(input_key_mods, RemapSet_parse_aliased_modifiers_on_target_side) {
    RemapSet set = RemapSet::empty();

    ASSERT_TRUE(set.parse("alt=cmd") == RemapSet::ParseError::none);
    set.finalize();

    const Mods left_alt = sided(Mod::alt, ModSide::left);
    const Mods left_super = sided(Mod::super, ModSide::left);
    ASSERT_TRUE(left_super == set.apply(left_alt));
    set.deinit();
}

TEST(input_key_mods, RemapSet_formatEntry_empty) {
    std::string buf;

    const RemapSet set = RemapSet::empty();
    set.formatEntry(entryFormatter("key-remap", &buf));
    ASSERT_TRUE(buf == "key-remap = \n");
}

TEST(input_key_mods, RemapSet_formatEntry_single_sided) {
    std::string buf;

    RemapSet set = RemapSet::empty();

    ASSERT_TRUE(set.parse("left_ctrl=super") == RemapSet::ParseError::none);
    set.finalize();

    set.formatEntry(entryFormatter("key-remap", &buf));
    ASSERT_TRUE(buf == "key-remap = left_ctrl=left_super\n");
    set.deinit();
}

TEST(input_key_mods, RemapSet_formatEntry_unsided_creates_two_entries) {
    std::string buf;

    RemapSet set = RemapSet::empty();

    ASSERT_TRUE(set.parse("ctrl=super") == RemapSet::ParseError::none);
    set.finalize();

    set.formatEntry(entryFormatter("key-remap", &buf));
    /* Unsided creates both left and right mappings */
    ASSERT_TRUE(buf.find("left_ctrl=left_super") != std::string::npos);
    ASSERT_TRUE(buf.find("right_ctrl=left_super") != std::string::npos);
    set.deinit();
}

TEST(input_key_mods, RemapSet_formatEntry_right_sided) {
    std::string buf;

    RemapSet set = RemapSet::empty();

    ASSERT_TRUE(set.parse("left_alt=right_ctrl") == RemapSet::ParseError::none);
    set.finalize();

    set.formatEntry(entryFormatter("key-remap", &buf));
    ASSERT_TRUE(buf == "key-remap = left_alt=right_ctrl\n");
    set.deinit();
}
