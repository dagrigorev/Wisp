/* Transliterated from the test blocks in Ghostty src/terminal/modes.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include "test_helpers.h"
#include "modes.hpp"

using namespace wisp::terminal::modes;

/* We have this here so that we explicitly fail when we change the
 * size of modes. The size of modes is NOT particularly important,
 * we just want to be mentally aware when it happens. */
TEST(modes, ModeState_size) {
    ASSERT_TRUE(sizeof(ModePacked) == 8);
}

TEST(modes, ModeTag_order) {
    ModeTag t;
    t.value = 1;
    t.ansi = false;
    ASSERT_TRUE(t.bits() == 1);
}

TEST(modes, modeFromInt) {
    Mode m;
    ASSERT_TRUE(modeFromInt(4, true, &m) && m == Mode::insert);
    ASSERT_FALSE(modeFromInt(9, true, &m));
    ASSERT_TRUE(modeFromInt(9, false, &m) && m == Mode::mouse_event_x10);
    ASSERT_FALSE(modeFromInt(14, true, &m));
}

TEST(modes, ModeState) {
    ModeState state;

    /* Normal set/get */
    ASSERT_FALSE(state.get(Mode::cursor_keys));
    state.set(Mode::cursor_keys, true);
    ASSERT_TRUE(state.get(Mode::cursor_keys));

    /* Save/restore */
    state.save(Mode::cursor_keys);
    state.set(Mode::cursor_keys, false);
    ASSERT_FALSE(state.get(Mode::cursor_keys));
    ASSERT_TRUE(state.restore(Mode::cursor_keys));
    ASSERT_TRUE(state.get(Mode::cursor_keys));
}

TEST(modes, ModeState_set_default_updates_current_and_reset_value) {
    ModeState state;

    state.setDefault(Mode::grapheme_cluster, true);
    ASSERT_TRUE(state.get(Mode::grapheme_cluster));
    ASSERT_TRUE(state.default_.field(Mode::grapheme_cluster));

    state.set(Mode::grapheme_cluster, false);
    ASSERT_FALSE(state.get(Mode::grapheme_cluster));
    ASSERT_TRUE(state.default_.field(Mode::grapheme_cluster));

    state.setDefault(Mode::grapheme_cluster, true);
    ASSERT_TRUE(state.get(Mode::grapheme_cluster));

    state.set(Mode::grapheme_cluster, false);
    state.reset();
    ASSERT_TRUE(state.get(Mode::grapheme_cluster));
}

TEST(modes, default_configurable_modes) {
    ASSERT_TRUE(defaultConfigurable(Mode::grapheme_cluster));
    ASSERT_TRUE(defaultConfigurable(Mode::wraparound));
    ASSERT_FALSE(defaultConfigurable(Mode::alt_screen));
    ASSERT_FALSE(defaultConfigurable(Mode::cursor_blinking));
}

TEST(modes, getReport_known_DEC_mode) {
    ModeState state;
    const Report report = state.getReport(Report::Tag::make(1));
    ASSERT_TRUE(report.state == Report::State::reset);
    ASSERT_TRUE(report.tag.ansi == false);
    ASSERT_TRUE(report.tag.value == 1);

    state.set(Mode::cursor_keys, true);
    const Report report2 = state.getReport(Report::Tag::make(1));
    ASSERT_TRUE(report2.state == Report::State::set);
}

TEST(modes, getReport_known_ANSI_mode) {
    ModeState state;
    state.set(Mode::insert, true);
    const Report report = state.getReport(Report::Tag::make(4, true));
    ASSERT_TRUE(report.state == Report::State::set);
    ASSERT_TRUE(report.tag.ansi == true);
}

TEST(modes, getReport_DECECM_permanently_reset) {
    const ModeState state;
    const Report report = state.getReport(Report::Tag::make(117, false));
    ASSERT_TRUE(report.state == Report::State::permanently_reset);
    ASSERT_TRUE(report.tag.ansi == false);
}

TEST(modes, getReport_unknown_mode) {
    const ModeState state;
    const Report report = state.getReport(Report::Tag::make(9999));
    ASSERT_TRUE(report.state == Report::State::not_recognized);
}

static Report make_report(uint16_t value, bool ansi, Report::State st) {
    Report r;
    r.tag = Report::Tag::make(value, ansi);
    r.state = st;
    return r;
}

TEST(modes, Report_encode_DEC_mode_set) {
    std::string w;
    make_report(1, false, Report::State::set).encode(&w);
    ASSERT_TRUE(w == "\x1B[?1;1$y");
}

TEST(modes, Report_encode_DEC_mode_reset) {
    std::string w;
    make_report(1, false, Report::State::reset).encode(&w);
    ASSERT_TRUE(w == "\x1B[?1;2$y");
}

TEST(modes, Report_encode_ANSI_mode) {
    std::string w;
    make_report(4, true, Report::State::set).encode(&w);
    ASSERT_TRUE(w == "\x1B[4;1$y");
}

TEST(modes, Report_encode_not_recognized) {
    std::string w;
    make_report(9999, false, Report::State::not_recognized).encode(&w);
    ASSERT_TRUE(w == "\x1B[?9999;0$y");
}

TEST(modes, getReport_large_unknown_modes) {
    const ModeState state;
    /* These would alias ANSI insert mode and fixed-status DEC mode 117. */
    const Report::Tag tags[] = {
        Report::Tag::make(32772, true),
        Report::Tag::make(32885),
    };
    for (size_t i = 0; i < 2; i++) {
        const Report report = state.getReport(tags[i]);
        ASSERT_TRUE(report.state == Report::State::not_recognized);
        ASSERT_TRUE(report.tag.eql(tags[i]));
    }
}

TEST(modes, Report_encode_maximum_size) {
    std::string w;
    make_report(65535, false, Report::State::permanently_reset).encode(&w);
    ASSERT_TRUE(w == "\x1b[?65535;4$y");
    ASSERT_TRUE(Report::max_size == w.size());
}
