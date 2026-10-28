/* Transliterated from the test blocks in Ghostty src/input/key_encode.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Wisp:
 *   - The test bodies were converted from upstream mechanically (struct
 *     literals become immediately-invoked lambdas filling the same fields;
 *     `std.Io.Writer.fixed` buffers become std::string), then the case
 *     tables, bufPrint expectations and ctrlSeq optionals by hand.
 *   - The two `test "modifier sequence values"` blocks nested in CsiUMods
 *     and KittyMods are named CsiUMods_/KittyMods_modifier_sequence_values.
 *   - Tests that upstream skips on every non-macOS target are not ported
 *     (Wisp targets Windows); each is marked where it would be.
 *   - The private `kitty` encoder is `kittyEncode` (see key_encode.hpp).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "test_helpers.h"
#include "../src/input/key_encode.hpp"

using namespace wisp;
using namespace wisp::input;
using namespace wisp::input::key_encode;

TEST(input_key_encode, CsiUMods_modifier_sequence_values) {
    /* This is all sort of trivially seen by looking at the code but
     * we want to make sure we never regress this. */
    CsiUMods mods;
    ASSERT_EQ(1, (int)mods.seqInt());

    mods = CsiUMods();
    mods.shift = true;
    ASSERT_EQ(2, (int)mods.seqInt());

    mods = CsiUMods();
    mods.alt = true;
    ASSERT_EQ(3, (int)mods.seqInt());

    mods = CsiUMods();
    mods.ctrl = true;
    ASSERT_EQ(5, (int)mods.seqInt());

    mods = CsiUMods();
    mods.alt = true;
    mods.shift = true;
    ASSERT_EQ(4, (int)mods.seqInt());

    mods = CsiUMods();
    mods.ctrl = true;
    mods.shift = true;
    ASSERT_EQ(6, (int)mods.seqInt());

    mods = CsiUMods();
    mods.alt = true;
    mods.ctrl = true;
    ASSERT_EQ(7, (int)mods.seqInt());

    mods = CsiUMods();
    mods.alt = true;
    mods.ctrl = true;
    mods.shift = true;
    ASSERT_EQ(8, (int)mods.seqInt());
}

TEST(input_key_encode, KittyMods_modifier_sequence_values) {
    /* This is all sort of trivially seen by looking at the code but
     * we want to make sure we never regress this. */
    KittyMods mods;
    ASSERT_EQ(1, (int)mods.seqInt());

    mods = KittyMods();
    mods.shift = true;
    ASSERT_EQ(2, (int)mods.seqInt());

    mods = KittyMods();
    mods.alt = true;
    ASSERT_EQ(3, (int)mods.seqInt());

    mods = KittyMods();
    mods.ctrl = true;
    ASSERT_EQ(5, (int)mods.seqInt());

    mods = KittyMods();
    mods.alt = true;
    mods.shift = true;
    ASSERT_EQ(4, (int)mods.seqInt());

    mods = KittyMods();
    mods.ctrl = true;
    mods.shift = true;
    ASSERT_EQ(6, (int)mods.seqInt());

    mods = KittyMods();
    mods.alt = true;
    mods.ctrl = true;
    ASSERT_EQ(7, (int)mods.seqInt());

    mods = KittyMods();
    mods.alt = true;
    mods.ctrl = true;
    mods.shift = true;
    ASSERT_EQ(8, (int)mods.seqInt());
}

TEST(input_key_encode, KittySequence_backspace) {

    /* Plain */
    {
        std::string writer;
        KittySequence seq = [&] { KittySequence s; s.key = 127; s.final = 117; return s; }();
        seq.encode(&writer);
        ASSERT_TRUE(std::string("\x1b[127u") == writer);
    }

    /* Release event */
    {
        std::string writer;
        KittySequence seq = [&] { KittySequence s; s.key = 127; s.final = 117; s.event = KittySequence::Event::release; return s; }();
        seq.encode(&writer);
        ASSERT_TRUE(std::string("\x1b[127;1:3u") == writer);
    }

    /* Shift */
    {
        std::string writer;
        KittySequence seq = [&] { KittySequence s; s.key = 127; s.final = 117; s.mods = [&] { KittyMods m; m.shift = true; return m; }(); return s; }();
        seq.encode(&writer);
        ASSERT_TRUE(std::string("\x1b[127;2u") == writer);
    }
}

TEST(input_key_encode, KittySequence_text) {

    /* Plain */
    {
        std::string writer;
        KittySequence seq = [&] { KittySequence s; s.key = 127; s.final = 117; s.text = "A"; s.text_len = 1; return s; }();
        seq.encode(&writer);
        ASSERT_TRUE(std::string("\x1b[127;;65u") == writer);
    }

    /* Release */
    {
        std::string writer;
        KittySequence seq = [&] { KittySequence s; s.key = 127; s.final = 117; s.event = KittySequence::Event::release; s.text = "A"; s.text_len = 1; return s; }();
        seq.encode(&writer);
        ASSERT_TRUE(std::string("\x1b[127;1:3;65u") == writer);
    }

    /* Shift */
    {
        std::string writer;
        KittySequence seq = [&] { KittySequence s; s.key = 127; s.final = 117; s.mods = [&] { KittyMods m; m.shift = true; return m; }(); s.text = "A"; s.text_len = 1; return s; }();
        seq.encode(&writer);
        ASSERT_TRUE(std::string("\x1b[127;2;65u") == writer);
    }
}

TEST(input_key_encode, KittySequence_text_with_control_characters) {

    /* By itself */
    {
        std::string writer;
        KittySequence seq = [&] { KittySequence s; s.key = 127; s.final = 117; s.text = "\x0a"; s.text_len = 1; return s; }();
        seq.encode(&writer);
        ASSERT_TRUE(std::string("\x1b[127u") == writer);
    }

    /* With other printables */
    {
        std::string writer;
        KittySequence seq = [&] { KittySequence s; s.key = 127; s.final = 117; s.text = "A\x0a"; s.text_len = 2; return s; }();
        seq.encode(&writer);
        ASSERT_TRUE(std::string("\x1b[127;;65u") == writer);
    }
}

TEST(input_key_encode, KittySequence_special_no_mods) {
    std::string writer;
    KittySequence seq = [&] { KittySequence s; s.key = 1; s.final = 65; return s; }();
    seq.encode(&writer);
    ASSERT_TRUE(std::string("\x1b[A") == writer);
}

TEST(input_key_encode, KittySequence_special_mods_only) {
    std::string writer;
    KittySequence seq = [&] { KittySequence s; s.key = 1; s.final = 65; s.mods = [&] { KittyMods m; m.shift = true; return m; }(); return s; }();
    seq.encode(&writer);
    ASSERT_TRUE(std::string("\x1b[1;2A") == writer);
}

TEST(input_key_encode, KittySequence_special_mods_and_event) {
    std::string writer;
    KittySequence seq = [&] { KittySequence s; s.key = 1; s.final = 65; s.event = KittySequence::Event::release; s.mods = [&] { KittyMods m; m.shift = true; return m; }(); return s; }();
    seq.encode(&writer);
    ASSERT_TRUE(std::string("\x1b[1;2:3A") == writer);
}

TEST(input_key_encode, kitty_plain_text) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::key_a; e.mods = [&] { Mods m;  return m; }(); e.utf8 = "abcd"; e.utf8_len = 4; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("abcd") == writer);
}

TEST(input_key_encode, kitty_repeat_with_just_disambiguate) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::key_a; e.action = Action::repeat; e.mods = [&] { Mods m;  return m; }(); e.utf8 = "a"; e.utf8_len = 1; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("a") == writer);
}

TEST(input_key_encode, kitty_enter_backspace_tab) {
    {
        std::string writer;
        kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::enter; e.mods = [&] { Mods m;  return m; }(); e.utf8 = ""; e.utf8_len = 0; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; return f; }(); return o; }());
        ASSERT_TRUE(std::string("\x0d") == writer);
    }
    {
        /* DECBKM reset */
        std::string writer;
        kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::backspace; e.mods = [&] { Mods m;  return m; }(); e.utf8 = ""; e.utf8_len = 0; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; return f; }(); o.backarrow_key_mode = false; return o; }());
        ASSERT_TRUE(std::string("\x7f") == writer);
    }
    {
        /* DECBKM set (Kitty does not support DECBKM so there should be no change) */
        std::string writer;
        kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::backspace; e.mods = [&] { Mods m;  return m; }(); e.utf8 = ""; e.utf8_len = 0; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; return f; }(); o.backarrow_key_mode = true; return o; }());
        ASSERT_TRUE(std::string("\x7f") == writer);
    }
    {
        std::string writer;
        kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::tab; e.mods = [&] { Mods m;  return m; }(); e.utf8 = ""; e.utf8_len = 0; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; return f; }(); return o; }());
        ASSERT_TRUE(std::string("\x09") == writer);
    }

    /* No release events if "report_all" is not set */
    {
        std::string writer;
        kittyEncode(&writer, [&] { KeyEvent e; e.action = Action::release; e.key = Key::enter; e.mods = [&] { Mods m;  return m; }(); e.utf8 = ""; e.utf8_len = 0; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_events = true; return f; }(); return o; }());
        ASSERT_TRUE(std::string("") == writer);
    }
    {
        std::string writer;
        kittyEncode(&writer, [&] { KeyEvent e; e.action = Action::release; e.key = Key::backspace; e.mods = [&] { Mods m;  return m; }(); e.utf8 = ""; e.utf8_len = 0; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_events = true; return f; }(); return o; }());
        ASSERT_TRUE(std::string("") == writer);
    }
    {
        std::string writer;
        kittyEncode(&writer, [&] { KeyEvent e; e.action = Action::release; e.key = Key::tab; e.mods = [&] { Mods m;  return m; }(); e.utf8 = ""; e.utf8_len = 0; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_events = true; return f; }(); return o; }());
        ASSERT_TRUE(std::string("") == writer);
    }

    /* Release events if "report_all" is set */
    {
        std::string writer;
        kittyEncode(&writer, [&] { KeyEvent e; e.action = Action::release; e.key = Key::enter; e.mods = [&] { Mods m;  return m; }(); e.utf8 = ""; e.utf8_len = 0; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_events = true; f.report_all = true; return f; }(); return o; }());
        ASSERT_TRUE(std::string("\x1b[13;1:3u") == writer);
    }
    {
        std::string writer;
        kittyEncode(&writer, [&] { KeyEvent e; e.action = Action::release; e.key = Key::backspace; e.mods = [&] { Mods m;  return m; }(); e.utf8 = ""; e.utf8_len = 0; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_events = true; f.report_all = true; return f; }(); return o; }());
        ASSERT_TRUE(std::string("\x1b[127;1:3u") == writer);
    }
    {
        std::string writer;
        kittyEncode(&writer, [&] { KeyEvent e; e.action = Action::release; e.key = Key::tab; e.mods = [&] { Mods m;  return m; }(); e.utf8 = ""; e.utf8_len = 0; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_events = true; f.report_all = true; return f; }(); return o; }());
        ASSERT_TRUE(std::string("\x1b[9;1:3u") == writer);
    }
}

TEST(input_key_encode, kitty_shift_backspace_emits_CSI_u) {
    /* Backspace with shift modifier should emit CSI u sequence, not raw 0x7F. */
    /* This is important for programs that want to distinguish shift+backspace. */
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::backspace; e.mods = [&] { Mods m; m.shift = true; return m; }(); e.utf8 = ""; e.utf8_len = 0; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("\x1b[127;2u") == writer);
}

TEST(input_key_encode, kitty_alt_backspace_emits_CSI_u) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::backspace; e.mods = [&] { Mods m; m.alt = true; return m; }(); e.consumed_mods = [&] { Mods m; m.alt = true; return m; }(); e.utf8 = ""; e.utf8_len = 0; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("\x1b[127;3u") == writer);
}

TEST(input_key_encode, kitty_shift_enter_emits_CSI_u) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::enter; e.mods = [&] { Mods m; m.shift = true; return m; }(); e.utf8 = ""; e.utf8_len = 0; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("\x1b[13;2u") == writer);
}

TEST(input_key_encode, kitty_shift_tab_emits_CSI_u) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::tab; e.mods = [&] { Mods m; m.shift = true; return m; }(); e.utf8 = ""; e.utf8_len = 0; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("\x1b[9;2u") == writer);
}

TEST(input_key_encode, kitty_enter_with_all_flags) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::enter; e.mods = [&] { Mods m;  return m; }(); e.utf8 = ""; e.utf8_len = 0; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_events = true; f.report_alternates = true; f.report_all = true; f.report_associated = true; return f; }(); return o; }());
    const std::string actual = writer;
    ASSERT_TRUE(std::string("[13u") == actual.substr(1));
}

TEST(input_key_encode, kitty_ctrl_with_all_flags) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::control_left; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); e.utf8 = ""; e.utf8_len = 0; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_events = true; f.report_alternates = true; f.report_all = true; f.report_associated = true; return f; }(); return o; }());
    const std::string actual = writer;
    ASSERT_TRUE(std::string("[57442;5u") == actual.substr(1));
}

TEST(input_key_encode, kitty_ctrl_release_with_ctrl_mod_set) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.action = Action::release; e.key = Key::control_left; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); e.utf8 = ""; e.utf8_len = 0; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_events = true; f.report_alternates = true; f.report_all = true; f.report_associated = true; return f; }(); return o; }());
    const std::string actual = writer;
    ASSERT_TRUE(std::string("[57442;5:3u") == actual.substr(1));
}

TEST(input_key_encode, kitty_delete) {
    {
        std::string writer;
        kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::delete_; e.mods = [&] { Mods m;  return m; }(); e.utf8 = "\x7f"; e.utf8_len = 1; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; return f; }(); return o; }());
        ASSERT_TRUE(std::string("\x1b[3~") == writer);
    }
}

TEST(input_key_encode, kitty_composing_with_no_modifier) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::key_a; e.mods = [&] { Mods m; m.shift = true; return m; }(); e.composing = true; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("") == writer);
}

TEST(input_key_encode, kitty_composing_with_modifier) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::shift_left; e.mods = [&] { Mods m; m.shift = true; return m; }(); e.composing = true; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_all = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("\x1b[57441;2u") == writer);
}

TEST(input_key_encode, kitty_composed_text_with_report_all) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::unidentified; e.mods = [&] { Mods m;  return m; }(); e.utf8 = "\xc3\xbb"; e.utf8_len = 2; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_events = true; f.report_alternates = true; f.report_all = true; f.report_associated = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("\xc3\xbb") == writer);
}

/* A key whose base (unshifted) keysym is a dead key has no unshifted */
/* codepoint, so it has no kitty entry and falls back to writing the text */
/* directly. That must not happen on release or the text is inserted twice. */
TEST(input_key_encode, kitty_text_fallback_on_release) {
    for (bool report_all : {false, true}) {
        std::string writer;
        kittyEncode(&writer, [&] { KeyEvent e; e.action = Action::release; e.key = Key::unidentified; e.mods = [&] { Mods m; m.shift = true; return m; }(); e.utf8 = "!"; e.utf8_len = 1; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_events = true; f.report_alternates = true; f.report_all = report_all; f.report_associated = true; return f; }(); return o; }());
        ASSERT_TRUE(std::string("") == writer);
    }
}

TEST(input_key_encode, kitty_text_fallback_on_repeat) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.action = Action::repeat; e.key = Key::unidentified; e.mods = [&] { Mods m; m.shift = true; return m; }(); e.utf8 = "!"; e.utf8_len = 1; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_events = true; f.report_alternates = true; f.report_associated = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("!") == writer);
}

TEST(input_key_encode, kitty_shift_a_on_US_keyboard) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::key_a; e.mods = [&] { Mods m; m.shift = true; return m; }(); e.utf8 = "A"; e.utf8_len = 1; e.unshifted_codepoint = 97; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_alternates = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("\x1b[97:65;2u") == writer);
}

TEST(input_key_encode, kitty_matching_unshifted_codepoint) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::key_a; e.mods = [&] { Mods m; m.shift = true; return m; }(); e.utf8 = "A"; e.utf8_len = 1; e.unshifted_codepoint = 65; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_alternates = true; return f; }(); return o; }());
    /* WARNING: This is not a valid encoding. This is a hypothetical encoding */
    /* just to test that our logic is correct around matching unshifted */
    /* codepoints. We get an alternate here because the unshifted_codepoint does */
    /* not match the base key */
    ASSERT_TRUE(std::string("\x1b[65::97;2u") == writer);
}

TEST(input_key_encode, kitty_report_alternates_with_caps) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::key_j; e.mods = [&] { Mods m; m.caps_lock = true; return m; }(); e.utf8 = "J"; e.utf8_len = 1; e.unshifted_codepoint = 106; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_all = true; f.report_alternates = true; f.report_associated = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("\x1b[106;65;74u") == writer);
}

TEST(input_key_encode, kitty_report_alternates_colon_shift) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::semicolon; e.mods = [&] { Mods m; m.shift = true; return m; }(); e.utf8 = ":"; e.utf8_len = 1; e.unshifted_codepoint = 59; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_all = true; f.report_alternates = true; f.report_associated = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("\x1b[59:58;2;58u") == writer);
}

TEST(input_key_encode, kitty_report_alternates_with_ru_layout) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::semicolon; e.mods = [&] { Mods m;  return m; }(); e.utf8 = "\xd1\x87"; e.utf8_len = 2; e.unshifted_codepoint = 0x447; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_all = true; f.report_alternates = true; f.report_associated = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("\x1b[1095::59;;1095u") == writer);
}

TEST(input_key_encode, kitty_report_alternates_with_ru_layout_shifted) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::semicolon; e.mods = [&] { Mods m; m.shift = true; return m; }(); e.utf8 = "\xd0\xa7"; e.utf8_len = 2; e.unshifted_codepoint = 0x447; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_all = true; f.report_alternates = true; f.report_associated = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("\x1b[1095:1063:59;2;1063u") == writer);
}

TEST(input_key_encode, kitty_report_alternates_with_ru_layout_caps_lock) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::semicolon; e.mods = [&] { Mods m; m.caps_lock = true; return m; }(); e.utf8 = "\xd0\xa7"; e.utf8_len = 2; e.unshifted_codepoint = 0x447; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_all = true; f.report_alternates = true; f.report_associated = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("\x1b[1095::59;65;1063u") == writer);
}

TEST(input_key_encode, kitty_report_alternates_with_hu_layout_release) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.action = Action::release; e.key = Key::bracket_left; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); e.utf8 = ""; e.utf8_len = 0; e.unshifted_codepoint = 0x151; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_all = true; f.report_alternates = true; f.report_associated = true; f.report_events = true; return f; }(); return o; }());
    const std::string actual = writer;
    ASSERT_TRUE(std::string("[337::91;5:3u") == actual.substr(1));
}

/* macOS generates utf8 text for arrow keys. */
TEST(input_key_encode, kitty_up_arrow_with_utf8) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::arrow_up; e.mods = [&] { Mods m;  return m; }(); e.utf8 = "\x1e"; e.utf8_len = 1; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("\x1b[A") == writer);
}

TEST(input_key_encode, kitty_shift_tab) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::tab; e.mods = [&] { Mods m; m.shift = true; return m; }(); e.utf8 = ""; e.utf8_len = 0; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_alternates = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("\x1b[9;2u") == writer);
}

TEST(input_key_encode, kitty_left_shift) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::shift_left; e.mods = [&] { Mods m;  return m; }(); e.utf8 = ""; e.utf8_len = 0; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_alternates = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("") == writer);
}

TEST(input_key_encode, kitty_left_shift_with_report_all) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::shift_left; e.mods = [&] { Mods m;  return m; }(); e.utf8 = ""; e.utf8_len = 0; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_all = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("\x1b[57441u") == writer);
}

/* Wisp: "kitty: report associated with alt text on macOS with option" is macOS-only upstream (SkipZigTest elsewhere); not ported. */

/* Wisp: "kitty: report associated with alt text on macOS with alt" is macOS-only upstream (SkipZigTest elsewhere); not ported. */

TEST(input_key_encode, kitty_report_associated_with_modifiers) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::key_j; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); e.utf8 = "j"; e.utf8_len = 1; e.unshifted_codepoint = 106; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_all = true; f.report_alternates = true; f.report_associated = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("\x1b[106;5u") == writer);
}

TEST(input_key_encode, kitty_report_associated) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::key_j; e.mods = [&] { Mods m; m.shift = true; return m; }(); e.utf8 = "J"; e.utf8_len = 1; e.unshifted_codepoint = 106; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_all = true; f.report_alternates = true; f.report_associated = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("\x1b[106:74;2;74u") == writer);
}

TEST(input_key_encode, kitty_report_associated_on_release) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.action = Action::release; e.key = Key::key_j; e.mods = [&] { Mods m; m.shift = true; return m; }(); e.utf8 = "J"; e.utf8_len = 1; e.unshifted_codepoint = 106; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_all = true; f.report_alternates = true; f.report_associated = true; f.report_events = true; return f; }(); return o; }());
    const std::string actual = writer;
    ASSERT_TRUE(std::string("[106:74;2:3u") == actual.substr(1));
}

TEST(input_key_encode, kitty_alternates_omit_control_characters) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::delete_; e.mods = [&] { Mods m;  return m; }(); e.utf8 = "\x7f"; e.utf8_len = 1; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_alternates = true; f.report_all = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("\x1b[3~") == writer);
}

TEST(input_key_encode, kitty_enter_with_utf8_dead_key_state) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::enter; e.utf8 = "A"; e.utf8_len = 1; e.unshifted_codepoint = 13; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_alternates = true; f.report_all = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("A") == writer);
}

TEST(input_key_encode, kitty_keypad_number) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::numpad_1; e.mods = [&] { Mods m;  return m; }(); e.utf8 = "1"; e.utf8_len = 1; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_events = true; f.report_alternates = true; f.report_all = true; f.report_associated = true; return f; }(); return o; }());
    const std::string actual = writer;
    ASSERT_TRUE(std::string("[57400;;49u") == actual.substr(1));
}

TEST(input_key_encode, kitty_backspace_with_utf8_dead_key_state) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::backspace; e.utf8 = "A"; e.utf8_len = 1; e.unshifted_codepoint = 13; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_events = true; f.report_alternates = true; f.report_all = true; f.report_associated = true; return f; }(); return o; }());
    ASSERT_TRUE(std::string("") == writer);
}

TEST(input_key_encode, legacy_backspace_with_utf8_dead_key_state) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::backspace; e.utf8 = "A"; e.utf8_len = 1; e.unshifted_codepoint = 13; return e; }(), [&] { Options o;  return o; }());
    ASSERT_TRUE(std::string("") == writer);
}

TEST(input_key_encode, kitty_backspace_DECBKM_reset_report_all_true) {
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::backspace; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_events = true; f.report_alternates = true; f.report_all = true; f.report_associated = true; return f; }(); o.backarrow_key_mode = false; return o; }());
    ASSERT_TRUE(std::string("\x1b[127u") == writer);
}

TEST(input_key_encode, kitty_backspace_DECBKM_set_report_all_true) {
    /* Kitty does not support DECBKM so there should be no difference. */
    std::string writer;
    kittyEncode(&writer, [&] { KeyEvent e; e.key = Key::backspace; return e; }(), [&] { Options o; o.kitty_flags = [&] { KittyFlags f; f.disambiguate = true; f.report_events = true; f.report_alternates = true; f.report_all = true; f.report_associated = true; return f; }(); o.backarrow_key_mode = true; return o; }());
    ASSERT_TRUE(std::string("\x1b[127u") == writer);
}

TEST(input_key_encode, legacy_enter_with_utf8_dead_key_state) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::enter; e.utf8 = "A"; e.utf8_len = 1; e.unshifted_codepoint = 13; return e; }(), [&] { Options o;  return o; }());
    ASSERT_TRUE(std::string("A") == writer);
}

TEST(input_key_encode, legacy_esc_with_utf8_dead_key_state) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::escape; e.utf8 = "A"; e.utf8_len = 1; e.unshifted_codepoint = 13; return e; }(), [&] { Options o;  return o; }());
    ASSERT_TRUE(std::string("A") == writer);
}

TEST(input_key_encode, legacy_alt_escape_with_modify_other_state_2) {

    {
        std::string writer;
        legacy(&writer, [&] { KeyEvent e; e.key = Key::escape; e.mods = [&] { Mods m; m.alt = true; return m; }(); return e; }(), [&] { Options o;  return o; }());
        ASSERT_TRUE(std::string("\x1b\x1b") == writer);
    }

    {
        std::string writer;
        legacy(&writer, [&] { KeyEvent e; e.key = Key::escape; e.mods = [&] { Mods m; m.alt = true; return m; }(); return e; }(), [&] { Options o; o.modify_other_keys_state_2 = true; return o; }());
        ASSERT_TRUE(std::string("\x1b[27;3;27~") == writer);
    }
}

TEST(input_key_encode, legacy_ctrl_shift_minus_underscore_on_US) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::minus; e.mods = [&] { Mods m; m.ctrl = true; m.shift = true; return m; }(); e.utf8 = "_"; e.utf8_len = 1; return e; }(), [&] { Options o;  return o; }());
    ASSERT_TRUE(std::string("\x1f") == writer);
}

TEST(input_key_encode, legacy_ctrl_alt_c) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::key_c; e.mods = [&] { Mods m; m.ctrl = true; m.alt = true; return m; }(); e.utf8 = "c"; e.utf8_len = 1; return e; }(), [&] { Options o;  return o; }());
    ASSERT_TRUE(std::string("\x1b\x03") == writer);
}

TEST(input_key_encode, legacy_alt_c) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::key_c; e.utf8 = "c"; e.utf8_len = 1; e.mods = [&] { Mods m; m.alt = true; return m; }(); return e; }(), [&] { Options o; o.alt_esc_prefix = true; o.macos_option_as_alt = OptionAsAlt::true_; return o; }());
    ASSERT_TRUE(std::string("\x1b" "c") == writer);
}

TEST(input_key_encode, legacy_alt_e_only_unshifted) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::key_e; e.unshifted_codepoint = 101; e.mods = [&] { Mods m; m.alt = true; return m; }(); return e; }(), [&] { Options o; o.alt_esc_prefix = true; o.macos_option_as_alt = OptionAsAlt::true_; return o; }());
    ASSERT_TRUE(std::string("\x1b" "e") == writer);
}

TEST(input_key_encode, legacy_alt_unicode) {
    struct Case {
        std::string text;
        uint32_t codepoint;
        std::string expected;
    };
    const Case cases[] = {
        {"\xc3\xa9", 0xE9, "\x1b" "\xc3\xa9"},
        {"\xc5\x91", 0x151, "\x1b" "\xc5\x91"},
        {"\xe7\x95\x8c", 0x754C, "\x1b" "\xe7\x95\x8c"},
        {"\xf0\x9f\x98\x80", 0x1F600, "\x1b" "\xf0\x9f\x98\x80"},
    };
    for (const Case &c : cases) {
        std::string writer;
        legacy(&writer, [&] { KeyEvent e; e.key = Key::unidentified; e.utf8 = c.text.data(); e.utf8_len = c.text.size(); e.unshifted_codepoint = c.codepoint; e.mods = [&] { Mods m; m.alt = true; return m; }(); return e; }(), [&] { Options o; o.alt_esc_prefix = true; o.macos_option_as_alt = OptionAsAlt::true_; return o; }());
        ASSERT_TRUE(c.expected == writer);
    }
}

TEST(input_key_encode, legacy_alt_unicode_only_unshifted) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::unidentified; e.unshifted_codepoint = 0x754C; e.mods = [&] { Mods m; m.alt = true; return m; }(); return e; }(), [&] { Options o; o.alt_esc_prefix = true; o.macos_option_as_alt = OptionAsAlt::true_; return o; }());
    ASSERT_TRUE(std::string("\x1b\xe7\x95\x8c") == writer);
}

TEST(input_key_encode, legacy_alt_unicode_without_unshifted) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::unidentified; e.utf8 = "\xf0\x9f\x98\x80"; e.utf8_len = 4; e.mods = [&] { Mods m; m.alt = true; return m; }(); return e; }(), [&] { Options o; o.alt_esc_prefix = true; o.macos_option_as_alt = OptionAsAlt::true_; return o; }());
    ASSERT_TRUE(std::string("\x1b\xf0\x9f\x98\x80") == writer);
}

/* Wisp: "legacy: alt+x macos" is macOS-only upstream (SkipZigTest elsewhere); not ported. */

/* Wisp: "legacy: shift+alt+. macos" is macOS-only upstream (SkipZigTest elsewhere); not ported. */

TEST(input_key_encode, legacy_alt) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::key_f; e.utf8 = "\xd1\x84"; e.utf8_len = 2; e.mods = [&] { Mods m; m.alt = true; return m; }(); return e; }(), [&] { Options o; o.alt_esc_prefix = true; o.macos_option_as_alt = OptionAsAlt::true_; return o; }());
    ASSERT_TRUE(std::string("\x1b\xd1\x84") == writer);
}

TEST(input_key_encode, legacy_ctrl_c) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::key_c; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); e.utf8 = "c"; e.utf8_len = 1; return e; }(), [&] { Options o;  return o; }());
    ASSERT_TRUE(std::string("\x03") == writer);
}

TEST(input_key_encode, legacy_ctrl_space) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::space; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); e.utf8 = " "; e.utf8_len = 1; return e; }(), [&] { Options o;  return o; }());
    ASSERT_TRUE(std::string("\x00", 1) == writer);
}

TEST(input_key_encode, legacy_ctrl_shift_backspace) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::backspace; e.mods = [&] { Mods m; m.ctrl = true; m.shift = true; return m; }(); return e; }(), [&] { Options o;  return o; }());
    ASSERT_TRUE(std::string("\x08") == writer);
}

TEST(input_key_encode, legacy_backspace_DECBKM_reset) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::backspace; e.mods = [&] { Mods m;  return m; }(); return e; }(), [&] { Options o; o.backarrow_key_mode = false; return o; }());
    ASSERT_TRUE(std::string("\x7f") == writer);
}

TEST(input_key_encode, legacy_backspace_DECBKM_reset_with_ctrl) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::backspace; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); return e; }(), [&] { Options o; o.backarrow_key_mode = false; return o; }());
    ASSERT_TRUE(std::string("\x08") == writer);
}

TEST(input_key_encode, legacy_backspace_DECBKM_set) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::backspace; e.mods = [&] { Mods m;  return m; }(); return e; }(), [&] { Options o; o.backarrow_key_mode = true; return o; }());
    ASSERT_TRUE(std::string("\x08") == writer);
}

TEST(input_key_encode, legacy_backspace_DECBKM_set_with_ctrl) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::backspace; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); return e; }(), [&] { Options o; o.backarrow_key_mode = true; return o; }());
    ASSERT_TRUE(std::string("\x7f") == writer);
}

TEST(input_key_encode, legacy_ctrl_shift_char_with_modify_other_state_2) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::key_h; e.mods = [&] { Mods m; m.ctrl = true; m.shift = true; return m; }(); e.utf8 = "H"; e.utf8_len = 1; return e; }(), [&] { Options o; o.modify_other_keys_state_2 = true; return o; }());
    ASSERT_TRUE(std::string("\x1b[27;6;72~") == writer);
}

TEST(input_key_encode, legacy_ctrl_char_with_modify_other_state_2) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::key_p; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); e.utf8 = "p"; e.utf8_len = 1; return e; }(), [&] { Options o; o.modify_other_keys_state_2 = true; return o; }());
    ASSERT_TRUE(std::string("\x1b[27;5;112~") == writer);
}

TEST(input_key_encode, legacy_ctrl_char_without_modify_other_state_2) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::key_p; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); e.utf8 = "p"; e.utf8_len = 1; return e; }(), [&] { Options o;  return o; }());
    ASSERT_TRUE(std::string("\x10") == writer);
}

TEST(input_key_encode, legacy_ctrl_shift_char_with_modify_other_state_2_and_consumed_mods) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::key_h; e.mods = [&] { Mods m; m.ctrl = true; m.shift = true; return m; }(); e.consumed_mods = [&] { Mods m; m.shift = true; return m; }(); e.utf8 = "H"; e.utf8_len = 1; return e; }(), [&] { Options o; o.modify_other_keys_state_2 = true; return o; }());
    ASSERT_TRUE(std::string("\x1b[27;6;72~") == writer);
}

TEST(input_key_encode, legacy_alt_digit_with_modify_other_state_2) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::digit_8; e.mods = [&] { Mods m; m.alt = true; return m; }(); e.consumed_mods = [&] { Mods m;  return m; }(); e.utf8 = "8"; e.utf8_len = 1; return e; }(), [&] { Options o; o.modify_other_keys_state_2 = true; o.macos_option_as_alt = OptionAsAlt::true_; return o; }());
    ASSERT_TRUE(std::string("\x1b[27;3;56~") == writer);
}

/* Wisp: "legacy: alt+digit with modify other state 2 and macos-option-as-alt = false" is macOS-only upstream (SkipZigTest elsewhere); not ported. */

TEST(input_key_encode, legacy_fixterm_awkward_letters) {
    {
        std::string writer;
        legacy(&writer, [&] { KeyEvent e; e.key = Key::key_i; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); e.utf8 = "i"; e.utf8_len = 1; return e; }(), [&] { Options o;  return o; }());
        ASSERT_TRUE(std::string("\x1b[105;5u") == writer);
    }
    {
        std::string writer;
        legacy(&writer, [&] { KeyEvent e; e.key = Key::key_m; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); e.utf8 = "m"; e.utf8_len = 1; return e; }(), [&] { Options o;  return o; }());
        ASSERT_TRUE(std::string("\x1b[109;5u") == writer);
    }
    {
        std::string writer;
        legacy(&writer, [&] { KeyEvent e; e.key = Key::bracket_left; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); e.utf8 = "["; e.utf8_len = 1; return e; }(), [&] { Options o;  return o; }());
        ASSERT_TRUE(std::string("\x1b[91;5u") == writer);
    }
    {
        std::string writer;
        legacy(&writer, [&] { KeyEvent e; e.key = Key::digit_2; e.mods = [&] { Mods m; m.ctrl = true; m.shift = true; return m; }(); e.utf8 = "@"; e.utf8_len = 1; e.unshifted_codepoint = 50; return e; }(), [&] { Options o;  return o; }());
        ASSERT_TRUE(std::string("\x1b[64;5u") == writer);
    }
}

/* These tests apply Kitty's behavior to CSIu where ctrl+shift+letter */
/* is sent as the unshifted letter with the shift modifier present. */
TEST(input_key_encode, legacy_ctrl_shift_letter_ascii) {
    {
        std::string writer;
        legacy(&writer, [&] { KeyEvent e; e.key = Key::key_m; e.mods = [&] { Mods m; m.ctrl = true; m.shift = true; return m; }(); e.utf8 = "M"; e.utf8_len = 1; e.unshifted_codepoint = 109; return e; }(), [&] { Options o;  return o; }());
        ASSERT_TRUE(std::string("\x1b[109;6u") == writer);
    }
}

TEST(input_key_encode, legacy_shift_function_key_should_use_all_mods) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::arrow_up; e.mods = [&] { Mods m; m.shift = true; return m; }(); e.consumed_mods = [&] { Mods m; m.shift = true; return m; }(); return e; }(), [&] { Options o;  return o; }());
    ASSERT_TRUE(std::string("\x1b[1;2A") == writer);
}

TEST(input_key_encode, legacy_keypad_enter) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::numpad_enter; e.mods = [&] { Mods m;  return m; }(); e.consumed_mods = [&] { Mods m;  return m; }(); return e; }(), [&] { Options o;  return o; }());
    ASSERT_TRUE(std::string("\x0d") == writer);
}

TEST(input_key_encode, legacy_keypad_1) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::numpad_1; e.mods = [&] { Mods m;  return m; }(); e.consumed_mods = [&] { Mods m;  return m; }(); e.utf8 = "1"; e.utf8_len = 1; return e; }(), [&] { Options o;  return o; }());
    ASSERT_TRUE(std::string("1") == writer);
}

TEST(input_key_encode, legacy_keypad_1_with_modify_other_state_2) {

    {
        std::string writer;
        legacy(&writer, [&] { KeyEvent e; e.key = Key::numpad_1; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); e.utf8 = "1"; e.utf8_len = 1; return e; }(), [&] { Options o; o.modify_other_keys_state_2 = true; return o; }());
        ASSERT_TRUE(std::string("1") == writer);
    }

    {
        std::string writer;
        legacy(&writer, [&] { KeyEvent e; e.key = Key::numpad_1; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); e.utf8 = "1"; e.utf8_len = 1; return e; }(), [&] { Options o; o.keypad_key_application = true; o.modify_other_keys_state_2 = true; return o; }());
        ASSERT_TRUE(std::string("\x1bO5q") == writer);
    }
}

TEST(input_key_encode, legacy_keypad_1_with_application_keypad) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::numpad_1; e.mods = [&] { Mods m;  return m; }(); e.consumed_mods = [&] { Mods m;  return m; }(); e.utf8 = "1"; e.utf8_len = 1; return e; }(), [&] { Options o; o.keypad_key_application = true; return o; }());
    ASSERT_TRUE(std::string("\x1bOq") == writer);
}

TEST(input_key_encode, legacy_keypad_1_with_application_keypad_and_numlock) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::numpad_1; e.mods = [&] { Mods m; m.num_lock = true; return m; }(); e.consumed_mods = [&] { Mods m;  return m; }(); e.utf8 = "1"; e.utf8_len = 1; return e; }(), [&] { Options o; o.keypad_key_application = true; return o; }());
    ASSERT_TRUE(std::string("\x1bOq") == writer);
}

TEST(input_key_encode, legacy_keypad_1_with_application_keypad_and_numlock_ignore) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::numpad_1; e.mods = [&] { Mods m; m.num_lock = false; return m; }(); e.consumed_mods = [&] { Mods m;  return m; }(); e.utf8 = "1"; e.utf8_len = 1; return e; }(), [&] { Options o; o.keypad_key_application = true; o.ignore_keypad_with_numlock = true; return o; }());
    ASSERT_TRUE(std::string("1") == writer);
}

TEST(input_key_encode, legacy_f1) {

    /* F1 */
    {
        std::string writer;
        legacy(&writer, [&] { KeyEvent e; e.key = Key::f1; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); e.consumed_mods = [&] { Mods m;  return m; }(); return e; }(), [&] { Options o;  return o; }());
        ASSERT_TRUE(std::string("\x1b[1;5P") == writer);
    }

    /* F2 */
    {
        std::string writer;
        legacy(&writer, [&] { KeyEvent e; e.key = Key::f2; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); e.consumed_mods = [&] { Mods m;  return m; }(); return e; }(), [&] { Options o;  return o; }());
        ASSERT_TRUE(std::string("\x1b[1;5Q") == writer);
    }

    /* F3 */
    {
        std::string writer;
        legacy(&writer, [&] { KeyEvent e; e.key = Key::f3; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); e.consumed_mods = [&] { Mods m;  return m; }(); return e; }(), [&] { Options o;  return o; }());
        ASSERT_TRUE(std::string("\x1b[13;5~") == writer);
    }

    /* F4 */
    {
        std::string writer;
        legacy(&writer, [&] { KeyEvent e; e.key = Key::f4; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); e.consumed_mods = [&] { Mods m;  return m; }(); return e; }(), [&] { Options o;  return o; }());
        ASSERT_TRUE(std::string("\x1b[1;5S") == writer);
    }

    /* F5 uses new encoding */
    {
        std::string writer;
        legacy(&writer, [&] { KeyEvent e; e.key = Key::f5; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); e.consumed_mods = [&] { Mods m;  return m; }(); return e; }(), [&] { Options o;  return o; }());
        ASSERT_TRUE(std::string("\x1b[15;5~") == writer);
    }
}

TEST(input_key_encode, legacy_f13_through_f25) {
    struct Case {
        Key key;
        uint8_t code;
    };
    const Case cases[] = {
        {Key::f13, 25},
        {Key::f14, 26},
        {Key::f15, 28},
        {Key::f16, 29},
        {Key::f17, 31},
        {Key::f18, 32},
        {Key::f19, 33},
        {Key::f20, 34},
        {Key::f21, 42},
        {Key::f22, 43},
        {Key::f23, 44},
        {Key::f24, 45},
        {Key::f25, 46},
    };

    for (const Case &c : cases) {
        {
            std::string writer;
            legacy(&writer, [&] { KeyEvent e; e.key = c.key; return e; }(), [&] { Options o;  return o; }());
            char expected[32];
            snprintf(expected, sizeof expected, "\x1b[%u~", (unsigned)c.code);
            ASSERT_TRUE(std::string(expected) == writer);
        }

        {
            std::string writer;
            legacy(&writer, [&] { KeyEvent e; e.key = c.key; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); return e; }(), [&] { Options o; o.modify_other_keys_state_2 = true; return o; }());
            char expected[32];
            snprintf(expected, sizeof expected, "\x1b[%u;5~", (unsigned)c.code);
            ASSERT_TRUE(std::string(expected) == writer);
        }
    }
}

TEST(input_key_encode, legacy_help_and_context_menu) {
    struct Case {
        Key key;
        uint8_t code;
    };
    const Case cases[] = {
        {Key::help, 28},
        {Key::context_menu, 29},
    };

    for (const Case &c : cases) {
        {
            std::string writer;
            legacy(&writer, [&] { KeyEvent e; e.key = c.key; return e; }(), [&] { Options o;  return o; }());
            char expected[32];
            snprintf(expected, sizeof expected, "\x1b[%u~", (unsigned)c.code);
            ASSERT_TRUE(std::string(expected) == writer);
        }

        {
            std::string writer;
            legacy(&writer, [&] { KeyEvent e; e.key = c.key; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); return e; }(), [&] { Options o; o.modify_other_keys_state_2 = true; return o; }());
            char expected[32];
            snprintf(expected, sizeof expected, "\x1b[%u;5~", (unsigned)c.code);
            ASSERT_TRUE(std::string(expected) == writer);
        }
    }
}

TEST(input_key_encode, legacy_left_shift_tab) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::tab; e.mods = [&] { Mods m; m.shift = true; m.sides.shift = ModSide::left; return m; }(); return e; }(), [&] { Options o;  return o; }());
    ASSERT_TRUE(std::string("\x1b[Z") == writer);
}

TEST(input_key_encode, legacy_right_shift_tab) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::tab; e.mods = [&] { Mods m; m.shift = true; m.sides.shift = ModSide::right; return m; }(); return e; }(), [&] { Options o;  return o; }());
    ASSERT_TRUE(std::string("\x1b[Z") == writer);
}

TEST(input_key_encode, legacy_hu_layout_ctrl_sends_proper_codepoint) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::bracket_left; e.mods = [&] { Mods m; m.ctrl = true; return m; }(); e.utf8 = "\xc5\x91"; e.utf8_len = 2; e.unshifted_codepoint = 0x151; return e; }(), [&] { Options o;  return o; }());
    const std::string actual = writer;
    ASSERT_TRUE(std::string("[337;5u") == actual.substr(1));
}

/* Wisp: "legacy: super-only on macOS with text" is macOS-only upstream (SkipZigTest elsewhere); not ported. */

/* Wisp: "legacy: super and other mods on macOS with text" is macOS-only upstream (SkipZigTest elsewhere); not ported. */

TEST(input_key_encode, legacy_backspace_with_DEL_utf8_DECBKM_reset) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::backspace; e.utf8 = "\x7f"; e.utf8_len = 1; e.unshifted_codepoint = 8; return e; }(), [&] { Options o; o.backarrow_key_mode = false; return o; }());
    ASSERT_TRUE(std::string("\x7f") == writer);
}

TEST(input_key_encode, legacy_backspace_with_DEL_utf8_DECBKM_set) {
    std::string writer;
    legacy(&writer, [&] { KeyEvent e; e.key = Key::backspace; e.utf8 = "\x7f"; e.utf8_len = 1; e.unshifted_codepoint = 8; return e; }(), [&] { Options o; o.backarrow_key_mode = true; return o; }());
    ASSERT_TRUE(std::string("\x08") == writer);
}

TEST(input_key_encode, ctrlseq_normal_ctrl_c) {
    uint8_t seq = 0;
    const bool has_seq = ctrlSeq(Key::unidentified, "c", 1, 99, [&] { Mods m; m.ctrl = true; return m; }(), &seq);
    ASSERT_TRUE(has_seq);
    ASSERT_EQ((long)0x03, (long)seq);
}

TEST(input_key_encode, ctrlseq_normal_ctrl_c_right_control) {
    uint8_t seq = 0;
    const bool has_seq = ctrlSeq(Key::unidentified, "c", 1, 99, [&] { Mods m; m.ctrl = true; m.sides.ctrl = ModSide::right; return m; }(), &seq);
    ASSERT_TRUE(has_seq);
    ASSERT_EQ((long)0x03, (long)seq);
}

TEST(input_key_encode, ctrlseq_alt_should_be_allowed) {
    uint8_t seq = 0;
    const bool has_seq = ctrlSeq(Key::unidentified, "c", 1, 99, [&] { Mods m; m.alt = true; m.ctrl = true; return m; }(), &seq);
    ASSERT_TRUE(has_seq);
    ASSERT_EQ((long)0x03, (long)seq);
}

TEST(input_key_encode, ctrlseq_no_ctrl_does_nothing) {
    {
        uint8_t unused = 0;
        ASSERT_FALSE(ctrlSeq(Key::unidentified, "c", 1, 99, [&] { Mods m;  return m; }(), &unused));
    }
}

TEST(input_key_encode, ctrlseq_shifted_non_character) {
    uint8_t seq = 0;
    const bool has_seq = ctrlSeq(Key::unidentified, "_", 1, 45, [&] { Mods m; m.ctrl = true; m.shift = true; return m; }(), &seq);
    ASSERT_TRUE(has_seq);
    ASSERT_EQ((long)0x1F, (long)seq);
}

TEST(input_key_encode, ctrlseq_caps_ascii_letter) {
    uint8_t seq = 0;
    const bool has_seq = ctrlSeq(Key::unidentified, "C", 1, 99, [&] { Mods m; m.ctrl = true; m.caps_lock = true; return m; }(), &seq);
    ASSERT_TRUE(has_seq);
    ASSERT_EQ((long)0x03, (long)seq);
}

TEST(input_key_encode, ctrlseq_shift_does_not_generate_ctrl_seq) {
    {
        uint8_t unused = 0;
        ASSERT_FALSE(ctrlSeq(Key::unidentified, "C", 1, 99, [&] { Mods m; m.shift = true; return m; }(), &unused));
    }
    {
        uint8_t unused = 0;
        ASSERT_FALSE(ctrlSeq(Key::unidentified, "C", 1, 99, [&] { Mods m; m.shift = true; m.ctrl = true; return m; }(), &unused));
    }
}

TEST(input_key_encode, ctrlseq_russian_ctrl_c) {
    uint8_t seq = 0;
    const bool has_seq = ctrlSeq(Key::key_c, "\xd1\x81", 2, 0x0441, [&] { Mods m; m.ctrl = true; return m; }(), &seq);
    ASSERT_TRUE(has_seq);
    ASSERT_EQ((long)0x03, (long)seq);
}

TEST(input_key_encode, ctrlseq_russian_shifted_ctrl_c) {
    uint8_t seq = 0;
    const bool has_seq = ctrlSeq(Key::key_c, "\xd1\x81", 2, 0x0441, [&] { Mods m; m.ctrl = true; m.shift = true; return m; }(), &seq);
    ASSERT_FALSE(has_seq);
}

TEST(input_key_encode, ctrlseq_russian_alt_ctrl_c) {
    uint8_t seq = 0;
    const bool has_seq = ctrlSeq(Key::key_c, "\xd1\x81", 2, 0x0441, [&] { Mods m; m.ctrl = true; m.alt = true; return m; }(), &seq);
    ASSERT_TRUE(has_seq);
    ASSERT_EQ((long)0x03, (long)seq);
}

TEST(input_key_encode, ctrlseq_right_ctrl_c) {
    uint8_t seq = 0;
    const bool has_seq = ctrlSeq(Key::key_c, "\xd1\x81", 2, 99, [&] { Mods m; m.ctrl = true; m.sides.ctrl = ModSide::right; return m; }(), &seq);
    ASSERT_TRUE(has_seq);
    ASSERT_EQ((long)0x03, (long)seq);
}

