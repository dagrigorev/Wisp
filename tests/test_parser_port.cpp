/* Transliterated from the test blocks in Ghostty src/terminal/Parser.zig and
 * src/terminal/parse_table.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * These are upstream's tests, not Wisp's: same inputs, same assertions, same
 * names. Passing them is what "exact" means for parser.hpp.
 *
 * Mapping from Zig:
 *   a[0] == null           !a.has(0)
 *   a[1].? == .print       a.has(1) && a[1].tag == Tag::print
 *   d.params.len           d.params_len
 *   d.final                d.final_
 */

#include "test_helpers.h"
#include "parser.hpp"

using namespace wisp::terminal::parser;

typedef Action::Tag Tag;

static void feed_silent(Parser &p, const char *s) {
    for (const char *c = s; *c; c++) {
        const Next a = p.next((uint8_t)*c);
        ASSERT_FALSE(a.has(0));
        ASSERT_FALSE(a.has(1));
        ASSERT_FALSE(a.has(2));
    }
}

/* ─── Parser.zig ─────────────────────────────────────────────────────────── */

TEST(parser, unnamed) {
    Parser p; /* init() */
    (void)p.next(0x9E);
    ASSERT_TRUE(p.state == State::sos_pm_apc_string);
    (void)p.next(0x9C);
    ASSERT_TRUE(p.state == State::ground);

    {
        const Next a = p.next('a');
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_FALSE(a.has(0));
        ASSERT_TRUE(a.has(1) && a[1].tag == Tag::print);
        ASSERT_FALSE(a.has(2));
    }

    {
        const Next a = p.next(0x19);
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_FALSE(a.has(0));
        ASSERT_TRUE(a.has(1) && a[1].tag == Tag::execute);
        ASSERT_FALSE(a.has(2));
    }
}

TEST(parser, esc_ESC_paren_B) {
    Parser p; /* init() */
    (void)p.next(0x1B);
    (void)p.next('(');

    {
        const Next a = p.next('B');
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_FALSE(a.has(0));
        ASSERT_TRUE(a.has(1) && a[1].tag == Tag::esc_dispatch);
        ASSERT_FALSE(a.has(2));

        const Action::ESC &d = a[1].esc_dispatch;
        ASSERT_TRUE(d.final_ == 'B');
        ASSERT_TRUE(d.intermediates_len == 1);
        ASSERT_TRUE(d.intermediates[0] == '(');
    }
}

TEST(parser, csi_ESC_bracket_H) {
    Parser p; /* init() */
    (void)p.next(0x1B);
    (void)p.next(0x5B);

    {
        const Next a = p.next(0x48);
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_FALSE(a.has(0));
        ASSERT_TRUE(a.has(1) && a[1].tag == Tag::csi_dispatch);
        ASSERT_FALSE(a.has(2));

        const Action::CSI &d = a[1].csi_dispatch;
        ASSERT_TRUE(d.final_ == 0x48);
        ASSERT_TRUE(d.params_len == 0);
    }
}

TEST(parser, csi_ESC_bracket_1_semicolon_4_H) {
    Parser p; /* init() */
    (void)p.next(0x1B);
    (void)p.next(0x5B);
    (void)p.next(0x31); /* 1 */
    (void)p.next(0x3B); /* ; */
    (void)p.next(0x34); /* 4 */

    {
        const Next a = p.next(0x48); /* H */
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_FALSE(a.has(0));
        ASSERT_TRUE(a.has(1) && a[1].tag == Tag::csi_dispatch);
        ASSERT_FALSE(a.has(2));

        const Action::CSI &d = a[1].csi_dispatch;
        ASSERT_TRUE(d.final_ == 'H');
        ASSERT_TRUE(d.params_len == 2);
        ASSERT_EQ(d.params[0], 1);
        ASSERT_EQ(d.params[1], 4);
    }
}

TEST(parser, csi_SGR_ESC_bracket_38_colon_2_m) {
    Parser p; /* init() */
    (void)p.next(0x1B);
    (void)p.next('[');
    (void)p.next('3');
    (void)p.next('8');
    (void)p.next(':');
    (void)p.next('2');

    {
        const Next a = p.next('m');
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_FALSE(a.has(0));
        ASSERT_TRUE(a.has(1) && a[1].tag == Tag::csi_dispatch);
        ASSERT_FALSE(a.has(2));

        const Action::CSI &d = a[1].csi_dispatch;
        ASSERT_TRUE(d.final_ == 'm');
        ASSERT_TRUE(d.params_len == 2);
        ASSERT_EQ(d.params[0], 38);
        ASSERT_TRUE(d.params_sep.isSet(0));
        ASSERT_EQ(d.params[1], 2);
        ASSERT_FALSE(d.params_sep.isSet(1));
    }
}

TEST(parser, csi_SGR_colon_followed_by_semicolon) {
    Parser p; /* init() */
    (void)p.next(0x1B);
    feed_silent(p, "[48:2");

    {
        const Next a = p.next('m');
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_FALSE(a.has(0));
        ASSERT_TRUE(a.has(1) && a[1].tag == Tag::csi_dispatch);
        ASSERT_FALSE(a.has(2));
    }

    (void)p.next(0x1B);
    (void)p.next('[');
    {
        const Next a = p.next('H');
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_FALSE(a.has(0));
        ASSERT_TRUE(a.has(1) && a[1].tag == Tag::csi_dispatch);
        ASSERT_FALSE(a.has(2));
    }
}

TEST(parser, csi_SGR_mixed_colon_and_semicolon) {
    Parser p; /* init() */
    (void)p.next(0x1B);
    feed_silent(p, "[38:5:1;48:5:0");

    {
        const Next a = p.next('m');
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_FALSE(a.has(0));
        ASSERT_TRUE(a.has(1) && a[1].tag == Tag::csi_dispatch);
        ASSERT_FALSE(a.has(2));
    }
}

TEST(parser, csi_SGR_ESC_bracket_48_colon_2_m) {
    Parser p; /* init() */
    (void)p.next(0x1B);
    feed_silent(p, "[48:2:240:143:104");

    {
        const Next a = p.next('m');
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_FALSE(a.has(0));
        ASSERT_TRUE(a.has(1) && a[1].tag == Tag::csi_dispatch);
        ASSERT_FALSE(a.has(2));

        const Action::CSI &d = a[1].csi_dispatch;
        ASSERT_TRUE(d.final_ == 'm');
        ASSERT_TRUE(d.params_len == 5);
        ASSERT_EQ(d.params[0], 48);
        ASSERT_TRUE(d.params_sep.isSet(0));
        ASSERT_EQ(d.params[1], 2);
        ASSERT_TRUE(d.params_sep.isSet(1));
        ASSERT_EQ(d.params[2], 240);
        ASSERT_TRUE(d.params_sep.isSet(2));
        ASSERT_EQ(d.params[3], 143);
        ASSERT_TRUE(d.params_sep.isSet(3));
        ASSERT_EQ(d.params[4], 104);
        ASSERT_FALSE(d.params_sep.isSet(4));
    }
}

TEST(parser, csi_SGR_ESC_bracket_4_colon_3_m_colon) {
    Parser p; /* init() */
    (void)p.next(0x1B);
    (void)p.next('[');
    (void)p.next('4');
    (void)p.next(':');
    (void)p.next('3');

    {
        const Next a = p.next('m');
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_FALSE(a.has(0));
        ASSERT_TRUE(a.has(1) && a[1].tag == Tag::csi_dispatch);
        ASSERT_FALSE(a.has(2));

        const Action::CSI &d = a[1].csi_dispatch;
        ASSERT_TRUE(d.final_ == 'm');
        ASSERT_TRUE(d.params_len == 2);
        ASSERT_EQ(d.params[0], 4);
        ASSERT_TRUE(d.params_sep.isSet(0));
        ASSERT_EQ(d.params[1], 3);
        ASSERT_FALSE(d.params_sep.isSet(1));
    }
}

TEST(parser, csi_SGR_with_many_blank_and_colon) {
    Parser p; /* init() */
    (void)p.next(0x1B);
    feed_silent(p, "[58:2::240:143:104");

    {
        const Next a = p.next('m');
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_FALSE(a.has(0));
        ASSERT_TRUE(a.has(1) && a[1].tag == Tag::csi_dispatch);
        ASSERT_FALSE(a.has(2));

        const Action::CSI &d = a[1].csi_dispatch;
        ASSERT_TRUE(d.final_ == 'm');
        ASSERT_TRUE(d.params_len == 6);
        ASSERT_EQ(d.params[0], 58);
        ASSERT_TRUE(d.params_sep.isSet(0));
        ASSERT_EQ(d.params[1], 2);
        ASSERT_TRUE(d.params_sep.isSet(1));
        ASSERT_EQ(d.params[2], 0);
        ASSERT_TRUE(d.params_sep.isSet(2));
        ASSERT_EQ(d.params[3], 240);
        ASSERT_TRUE(d.params_sep.isSet(3));
        ASSERT_EQ(d.params[4], 143);
        ASSERT_TRUE(d.params_sep.isSet(4));
        ASSERT_EQ(d.params[5], 104);
        ASSERT_FALSE(d.params_sep.isSet(5));
    }
}

/* This is from a Kakoune actual SGR sequence. */
TEST(parser, csi_SGR_mixed_colon_and_semicolon_with_blank) {
    Parser p; /* init() */
    (void)p.next(0x1B);
    feed_silent(p, "[;4:3;38;2;175;175;215;58:2::190:80:70");

    {
        const Next a = p.next('m');
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_FALSE(a.has(0));
        ASSERT_TRUE(a.has(1) && a[1].tag == Tag::csi_dispatch);
        ASSERT_FALSE(a.has(2));

        const Action::CSI &d = a[1].csi_dispatch;
        ASSERT_TRUE(d.final_ == 'm');
        ASSERT_EQ(d.params_len, 14u);
        ASSERT_EQ(d.params[0], 0);
        ASSERT_FALSE(d.params_sep.isSet(0));
        ASSERT_EQ(d.params[1], 4);
        ASSERT_TRUE(d.params_sep.isSet(1));
        ASSERT_EQ(d.params[2], 3);
        ASSERT_FALSE(d.params_sep.isSet(2));
        ASSERT_EQ(d.params[3], 38);
        ASSERT_FALSE(d.params_sep.isSet(3));
        ASSERT_EQ(d.params[4], 2);
        ASSERT_FALSE(d.params_sep.isSet(4));
        ASSERT_EQ(d.params[5], 175);
        ASSERT_FALSE(d.params_sep.isSet(5));
        ASSERT_EQ(d.params[6], 175);
        ASSERT_FALSE(d.params_sep.isSet(6));
        ASSERT_EQ(d.params[7], 215);
        ASSERT_FALSE(d.params_sep.isSet(7));
        ASSERT_EQ(d.params[8], 58);
        ASSERT_TRUE(d.params_sep.isSet(8));
        ASSERT_EQ(d.params[9], 2);
        ASSERT_TRUE(d.params_sep.isSet(9));
        ASSERT_EQ(d.params[10], 0);
        ASSERT_TRUE(d.params_sep.isSet(10));
        ASSERT_EQ(d.params[11], 190);
        ASSERT_TRUE(d.params_sep.isSet(11));
        ASSERT_EQ(d.params[12], 80);
        ASSERT_TRUE(d.params_sep.isSet(12));
        ASSERT_EQ(d.params[13], 70);
        ASSERT_FALSE(d.params_sep.isSet(13));
    }
}

/* This is from a Kakoune actual SGR sequence also. */
TEST(parser, csi_SGR_mixed_colon_and_semicolon_setting_underline_bg_fg) {
    Parser p; /* init() */
    (void)p.next(0x1B);
    feed_silent(p, "[4:3;38;2;51;51;51;48;2;170;170;170;58;2;255;97;136");

    {
        const Next a = p.next('m');
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_FALSE(a.has(0));
        ASSERT_TRUE(a.has(1) && a[1].tag == Tag::csi_dispatch);
        ASSERT_FALSE(a.has(2));

        const Action::CSI &d = a[1].csi_dispatch;
        ASSERT_TRUE(d.final_ == 'm');
        ASSERT_EQ(d.params_len, 17u);
        ASSERT_EQ(d.params[0], 4);
        ASSERT_TRUE(d.params_sep.isSet(0));
        ASSERT_EQ(d.params[1], 3);
        ASSERT_FALSE(d.params_sep.isSet(1));
        ASSERT_EQ(d.params[2], 38);
        ASSERT_FALSE(d.params_sep.isSet(2));
        ASSERT_EQ(d.params[3], 2);
        ASSERT_FALSE(d.params_sep.isSet(3));
        ASSERT_EQ(d.params[4], 51);
        ASSERT_FALSE(d.params_sep.isSet(4));
        ASSERT_EQ(d.params[5], 51);
        ASSERT_FALSE(d.params_sep.isSet(5));
        ASSERT_EQ(d.params[6], 51);
        ASSERT_FALSE(d.params_sep.isSet(6));
        ASSERT_EQ(d.params[7], 48);
        ASSERT_FALSE(d.params_sep.isSet(7));
        ASSERT_EQ(d.params[8], 2);
        ASSERT_FALSE(d.params_sep.isSet(8));
        ASSERT_EQ(d.params[9], 170);
        ASSERT_FALSE(d.params_sep.isSet(9));
        ASSERT_EQ(d.params[10], 170);
        ASSERT_FALSE(d.params_sep.isSet(10));
        ASSERT_EQ(d.params[11], 170);
        ASSERT_FALSE(d.params_sep.isSet(11));
        ASSERT_EQ(d.params[12], 58);
        ASSERT_FALSE(d.params_sep.isSet(12));
        ASSERT_EQ(d.params[13], 2);
        ASSERT_FALSE(d.params_sep.isSet(13));
        ASSERT_EQ(d.params[14], 255);
        ASSERT_FALSE(d.params_sep.isSet(14));
        ASSERT_EQ(d.params[15], 97);
        ASSERT_FALSE(d.params_sep.isSet(15));
        ASSERT_EQ(d.params[16], 136);
        ASSERT_FALSE(d.params_sep.isSet(16));
    }
}

TEST(parser, csi_colon_for_non_m_final) {
    Parser p; /* init() */
    (void)p.next(0x1B);
    feed_silent(p, "[38:2h");

    ASSERT_TRUE(p.state == State::ground);
}

TEST(parser, csi_request_mode_decrqm) {
    Parser p; /* init() */
    (void)p.next(0x1B);
    feed_silent(p, "[?2026$");

    {
        const Next a = p.next('p');
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_FALSE(a.has(0));
        ASSERT_TRUE(a.has(1) && a[1].tag == Tag::csi_dispatch);
        ASSERT_FALSE(a.has(2));

        const Action::CSI &d = a[1].csi_dispatch;
        ASSERT_TRUE(d.final_ == 'p');
        ASSERT_EQ(d.intermediates_len, 2u);
        ASSERT_EQ(d.params_len, 1u);
        ASSERT_EQ(d.intermediates[0], '?');
        ASSERT_EQ(d.intermediates[1], '$');
        ASSERT_EQ(d.params[0], 2026);
    }
}

TEST(parser, csi_change_cursor) {
    Parser p; /* init() */
    (void)p.next(0x1B);
    feed_silent(p, "[3 ");

    {
        const Next a = p.next('q');
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_FALSE(a.has(0));
        ASSERT_TRUE(a.has(1) && a[1].tag == Tag::csi_dispatch);
        ASSERT_FALSE(a.has(2));

        const Action::CSI &d = a[1].csi_dispatch;
        ASSERT_TRUE(d.final_ == 'q');
        ASSERT_EQ(d.intermediates_len, 1u);
        ASSERT_EQ(d.params_len, 1u);
        ASSERT_EQ(d.intermediates[0], ' ');
        ASSERT_EQ(d.params[0], 3);
    }
}

TEST(parser, osc_112_incomplete_sequence) {
    Parser p; /* init() */
    p.osc_parser.alloc = true;

    (void)p.next(0x1B);
    (void)p.next(']');
    (void)p.next('1');
    (void)p.next('1');
    (void)p.next('2');

    {
        const Next a = p.next(0x07);
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_TRUE(a.has(0) && a[0].tag == Tag::osc_dispatch);
        ASSERT_FALSE(a.has(1));
        ASSERT_FALSE(a.has(2));

        const wisp::terminal::osc::Command &cmd = a[0].osc_dispatch;
        ASSERT_TRUE(cmd.key == wisp::terminal::osc::Command::Key::color_operation);
        ASSERT_TRUE(cmd.color_operation.terminator == wisp::terminal::osc::Terminator::bel);
        ASSERT_TRUE(cmd.color_operation.op == wisp::terminal::osc::color::Operation::osc_112);
        ASSERT_TRUE(cmd.color_operation.requests.count() == 1);
        auto it = cmd.color_operation.requests.constIterator(0);
        {
            const wisp::terminal::osc::color::Request *op = it.next();
            ASSERT_TRUE(op != nullptr);
            ASSERT_TRUE(op->tag == wisp::terminal::osc::color::Request::Tag::reset);
            wisp::terminal::osc::color::Request want;
            want.tag = wisp::terminal::osc::color::Request::Tag::reset;
            want.reset = wisp::terminal::osc::color::Target::makeDynamic(
                wisp::terminal::Dynamic::cursor);
            ASSERT_TRUE(op->eql(want));
        }
        ASSERT_TRUE(it.next() == nullptr);
    }
}

TEST(parser, osc_104_empty) {
    Parser p; /* init() */
    p.osc_parser.alloc = true;

    (void)p.next(0x1B);
    (void)p.next(']');
    (void)p.next('1');
    (void)p.next('0');
    (void)p.next('4');

    {
        const Next a = p.next(0x07);
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_TRUE(a.has(0) && a[0].tag == Tag::osc_dispatch);
        ASSERT_FALSE(a.has(1));
        ASSERT_FALSE(a.has(2));

        const wisp::terminal::osc::Command &cmd = a[0].osc_dispatch;
        ASSERT_TRUE(cmd.key == wisp::terminal::osc::Command::Key::color_operation);
        ASSERT_TRUE(cmd.color_operation.terminator == wisp::terminal::osc::Terminator::bel);
        ASSERT_TRUE(cmd.color_operation.op == wisp::terminal::osc::color::Operation::osc_104);
        ASSERT_TRUE(cmd.color_operation.requests.count() == 1);
        auto it = cmd.color_operation.requests.constIterator(0);
        {
            const wisp::terminal::osc::color::Request *op = it.next();
            ASSERT_TRUE(op != nullptr);
            ASSERT_TRUE(op->tag == wisp::terminal::osc::color::Request::Tag::reset_palette);
        }
        ASSERT_TRUE(it.next() == nullptr);
    }
}

TEST(parser, csi_too_many_params) {
    Parser p; /* init() */
    (void)p.next(0x1B);
    (void)p.next('[');
    for (int i = 0; i < 100; i++) {
        (void)p.next('1');
        (void)p.next(';');
    }
    (void)p.next('1');

    {
        const Next a = p.next('C');
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_FALSE(a.has(0));
        ASSERT_FALSE(a.has(1));
        ASSERT_FALSE(a.has(2));
    }
}

TEST(parser, csi_sgr_with_up_to_our_max_parameters) {
    for (size_t max = 1; max < MAX_PARAMS + 1; max++) {
        Parser p; /* init() */
        (void)p.next(0x1B);
        (void)p.next('[');

        for (size_t i = 0; i < max - 1; i++) {
            (void)p.next('1');
            (void)p.next(';');
        }
        (void)p.next('2');

        {
            const Next a = p.next('H');
            ASSERT_TRUE(p.state == State::ground);
            ASSERT_FALSE(a.has(0));
            ASSERT_TRUE(a.has(1) && a[1].tag == Tag::csi_dispatch);
            ASSERT_FALSE(a.has(2));

            const Action::CSI &csi = a[1].csi_dispatch;
            ASSERT_EQ(csi.params_len, max);
            ASSERT_EQ(csi.params[max - 1], 2);
        }
    }
}

TEST(parser, csi_sgr_beyond_our_max_drops_it) {
    /* Has to be +2 for the loops below */
    const size_t max = MAX_PARAMS + 2;

    Parser p; /* init() */
    (void)p.next(0x1B);
    (void)p.next('[');

    for (size_t i = 0; i < max - 1; i++) {
        (void)p.next('1');
        (void)p.next(';');
    }
    (void)p.next('2');

    {
        const Next a = p.next('H');
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_FALSE(a.has(0));
        ASSERT_FALSE(a.has(1));
        ASSERT_FALSE(a.has(2));
    }
}

TEST(parser, dcs_XTGETTCAP) {
    Parser p; /* init() */
    (void)p.next(0x1B);
    feed_silent(p, "P+");

    {
        const Next a = p.next('q');
        ASSERT_TRUE(p.state == State::dcs_passthrough);
        ASSERT_FALSE(a.has(0));
        ASSERT_FALSE(a.has(1));
        ASSERT_TRUE(a.has(2) && a[2].tag == Tag::dcs_hook);

        const Action::DCS &hook = a[2].dcs_hook;
        ASSERT_EQ(hook.intermediates_len, 1u);
        ASSERT_EQ(hook.intermediates[0], '+');
        ASSERT_EQ(hook.params_len, 0u);
        ASSERT_EQ(hook.final_, 'q');
    }
}

TEST(parser, dcs_params) {
    Parser p; /* init() */
    (void)p.next(0x1B);
    feed_silent(p, "P1000");

    {
        const Next a = p.next('p');
        ASSERT_TRUE(p.state == State::dcs_passthrough);
        ASSERT_FALSE(a.has(0));
        ASSERT_FALSE(a.has(1));
        ASSERT_TRUE(a.has(2) && a[2].tag == Tag::dcs_hook);

        const Action::DCS &hook = a[2].dcs_hook;
        ASSERT_EQ(hook.params_len, 1u);
        ASSERT_EQ(hook.params[0], 1000);
        ASSERT_EQ(hook.final_, 'p');
    }
}

TEST(parser, dcs_too_many_params) {
    /* Regression test for a crash found by fuzzing (afl). When a DCS
     * sequence has more than MAX_PARAMS parameters and param_acc_idx > 0,
     * entering dcs_passthrough wrote to params[params_idx] without a
     * bounds check, causing an out-of-bounds access. */
    Parser p; /* init() */
    (void)p.next(0x1B); /* ESC */
    (void)p.next('P');  /* DCS entry */

    /* Feed a digit then MAX_PARAMS semicolons to fill all param slots. */
    (void)p.next('6');
    for (size_t i = 0; i < MAX_PARAMS; i++) (void)p.next(';');
    /* Feed another digit so param_acc_idx > 0 while params_idx == MAX_PARAMS. */
    (void)p.next('7');

    /* A final byte triggers entry to dcs_passthrough. The DCS should
     * be dropped entirely, consistent with how CSI handles overflow. */
    const Next a = p.next('p');
    ASSERT_FALSE(a.has(0));
    ASSERT_FALSE(a.has(1));
    ASSERT_FALSE(a.has(2));
}

/* ─── parse_table.zig ────────────────────────────────────────────────────── */

TEST(parse_table, unnamed) {
    /* This forces evaluation of table, so we're just testing that it
     * succeeds in creation. */
    (void)table();
}

TEST(parse_table, dcs_passthrough_high_bytes_are_payload_data) {
    /* Bytes 0x80-0xFF within a DCS string are payload data, not C1
     * controls. This includes 0x9C (8-bit ST): a raw 0x9C is
     * indistinguishable from a UTF-8 continuation byte (e.g. "Ü" is
     * 0xC3 0x9C) and Ghostty doesn't support 8-bit C1 controls
     * anywhere else. */
    for (size_t c = 0x80; c < 0x100; c++) {
        const Transition entry = table().t[c][(size_t)State::dcs_passthrough];
        ASSERT_TRUE(entry.state == State::dcs_passthrough);
        ASSERT_TRUE(entry.action == TransitionAction::put);
    }
}

TEST(parse_table, dcs_ignore_high_bytes_are_ignored_payload_data) {
    /* Same as dcs_passthrough: a UTF-8 payload inside an ignored DCS
     * must not trigger "anywhere" C1 transitions (e.g. 0x9B beginning
     * a CSI mid-string). */
    for (size_t c = 0x80; c < 0x100; c++) {
        const Transition entry = table().t[c][(size_t)State::dcs_ignore];
        ASSERT_TRUE(entry.state == State::dcs_ignore);
        ASSERT_TRUE(entry.action == TransitionAction::ignore);
    }
}

TEST(parse_table, dcs_passthrough_ESC_CAN_and_SUB_still_exit) {
    /* 7-bit ST (ESC \) is the DCS terminator and CAN/SUB abort, so
     * these must continue to leave dcs_passthrough. dcs_unhook is
     * emitted by the parser on any transition out of dcs_passthrough. */
    const Transition esc = table().t[0x1B][(size_t)State::dcs_passthrough];
    ASSERT_TRUE(esc.state == State::escape);

    const Transition can = table().t[0x18][(size_t)State::dcs_passthrough];
    ASSERT_TRUE(can.state == State::ground);

    const Transition sub = table().t[0x1A][(size_t)State::dcs_passthrough];
    ASSERT_TRUE(sub.state == State::ground);
}

/* ─── Parser.zig: the OSC tests, now that osc.hpp exists ─────────────────── */

TEST(parser, osc_change_window_title) {
    Parser p; /* init() */
    (void)p.next(0x1B);
    (void)p.next(']');
    (void)p.next('0');
    (void)p.next(';');
    (void)p.next('a');
    (void)p.next('b');
    (void)p.next('c');

    {
        const Next a = p.next(0x07); /* BEL */
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_TRUE(a.has(0) && a[0].tag == Tag::osc_dispatch);
        ASSERT_FALSE(a.has(1));
        ASSERT_FALSE(a.has(2));

        const wisp::terminal::osc::Command &cmd = a[0].osc_dispatch;
        ASSERT_TRUE(cmd.key == wisp::terminal::osc::Command::Key::change_window_title);
        ASSERT_TRUE(cmd.change_window_title.eql("abc"));
    }
}

TEST(parser, osc_change_window_title_end_in_esc) {
    Parser p; /* init() */
    (void)p.next(0x1B);
    (void)p.next(']');
    (void)p.next('0');
    (void)p.next(';');
    (void)p.next('a');
    (void)p.next('b');
    (void)p.next('c');

    {
        const Next a = p.next(0x1B);
        (void)p.next('\\');
        ASSERT_TRUE(p.state == State::ground);
        ASSERT_TRUE(a.has(0) && a[0].tag == Tag::osc_dispatch);
        ASSERT_FALSE(a.has(1));
        ASSERT_FALSE(a.has(2));

        const wisp::terminal::osc::Command &cmd = a[0].osc_dispatch;
        ASSERT_TRUE(cmd.key == wisp::terminal::osc::Command::Key::change_window_title);
        ASSERT_TRUE(cmd.change_window_title.eql("abc"));
    }
}
