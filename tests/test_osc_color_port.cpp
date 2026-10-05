/* Transliterated from the test blocks in Ghostty
 * src/terminal/osc/parsers/color.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * These are upstream's tests: same inputs, same assertions, same names.
 *
 * Mapping from Zig, beyond test_osc_port.cpp's:
 *   try parseColor(alloc, op, body)   List list; parseColor(op, body, len, &list)
 *   expectEqual(Request{...}, x)      x.eql(req)
 *   inline for DynamicColor fields    loop over the ten Dynamic values
 */

#include "test_helpers.h"
#include "osc.hpp"

#include <stdio.h>

using namespace wisp::terminal;
using osc::color::Operation;
using osc::color::Request;
using osc::color::Target;
using osc::color::List;

static const RGB red(255, 0, 0);
static const RGB blue(0, 0, 255);

static bool parse(Operation op, const char *body, List *list) {
    return osc::parsers::color::parseColor(op, body, strlen(body), list);
}

static Request set_req(Target t, RGB c) {
    Request r;
    r.tag = Request::Tag::set;
    r.set.target = t;
    r.set.color = c;
    return r;
}

static Request query_req(Target t) {
    Request r;
    r.tag = Request::Tag::query;
    r.query = t;
    return r;
}

static Request reset_req(Target t) {
    Request r;
    r.tag = Request::Tag::reset;
    r.reset = t;
    return r;
}

static Request tag_req(Request::Tag tag) {
    Request r;
    r.tag = tag;
    return r;
}

/* Wisp: @field(Operation, "osc_{d}") for a Dynamic's value. Operation's
 * members run osc_4, osc_5, osc_10..osc_19, osc_104, osc_105, osc_110.. */
static Operation dynamic_op(Dynamic d) {
    return (Operation)((unsigned)d - 10 + 2);
}
static Operation dynamic_reset_op(Dynamic d) {
    return (Operation)((unsigned)d - 10 + 14);
}

static const Dynamic all_dynamic[] = {
    Dynamic::foreground,           Dynamic::background,
    Dynamic::cursor,               Dynamic::pointer_foreground,
    Dynamic::pointer_background,   Dynamic::tektronix_foreground,
    Dynamic::tektronix_background, Dynamic::highlight_background,
    Dynamic::tektronix_cursor,     Dynamic::highlight_foreground,
};

static const unsigned special_count = 5;

TEST(color, OSC_4_empty_param) {
    osc::Parser p; /* .init(null) */

    const char *input = "4;;";
    for (const char *c = input; *c; c++) p.next((uint8_t)*c);

    osc::Command *cmd = p.end('\x1b');
    ASSERT_TRUE(cmd == nullptr);
}

TEST(color, OSC_4) {
    char body[32];

    /* Test every palette index */
    for (unsigned idx = 0; idx < 255; idx++) {
        /* Simple color set
         * printf '\e]4;0;red\\' */
        {
            snprintf(body, sizeof(body), "%u;red", idx);
            List list;
            ASSERT_TRUE(parse(Operation::osc_4, body, &list));
            ASSERT_TRUE(list.count() == 1);
            ASSERT_TRUE(list.at(0)->eql(set_req(Target::makePalette((uint8_t)idx), red)));
            list.deinit();
        }

        /* Simple color query
         * printf '\e]4;0;?\\' */
        {
            snprintf(body, sizeof(body), "%u;?", idx);
            List list;
            ASSERT_TRUE(parse(Operation::osc_4, body, &list));
            ASSERT_TRUE(list.count() == 1);
            ASSERT_TRUE(list.at(0)->eql(query_req(Target::makePalette((uint8_t)idx))));
            list.deinit();
        }

        /* Trailing invalid data produces results up to that point
         * printf '\e]4;0;red;\e\\' */
        {
            snprintf(body, sizeof(body), "%u;red;", idx);
            List list;
            ASSERT_TRUE(parse(Operation::osc_4, body, &list));
            ASSERT_TRUE(list.count() == 1);
            ASSERT_TRUE(list.at(0)->eql(set_req(Target::makePalette((uint8_t)idx), red)));
            list.deinit();
        }

        /* Whitespace doesn't produce a working value in xterm but we
         * allow it because Kitty does and it seems harmless.
         *
         * printf '\e]4;0;red \e\\' */
        {
            snprintf(body, sizeof(body), "%u;red ", idx);
            List list;
            ASSERT_TRUE(parse(Operation::osc_4, body, &list));
            ASSERT_TRUE(list.count() == 1);
            ASSERT_TRUE(list.at(0)->eql(set_req(Target::makePalette((uint8_t)idx), red)));
            list.deinit();
        }
    }

    /* Test every special color */
    for (unsigned i = 0; i < special_count; i++) {
        const Special special = (Special)i;

        /* Simple color set
         * printf '\e]4;256;red\\' */
        {
            snprintf(body, sizeof(body), "%u;red", 256 + i);
            List list;
            ASSERT_TRUE(parse(Operation::osc_4, body, &list));
            ASSERT_TRUE(list.count() == 1);
            ASSERT_TRUE(list.at(0)->eql(set_req(Target::makeSpecial(special), red)));
            list.deinit();
        }
    }
}

TEST(color, OSC_5) {
    char body[32];

    /* Test every special color */
    for (unsigned i = 0; i < special_count; i++) {
        const Special special = (Special)i;

        /* Simple color set
         * printf '\e]4;256;red\\' */
        {
            snprintf(body, sizeof(body), "%u;red", i);
            List list;
            ASSERT_TRUE(parse(Operation::osc_5, body, &list));
            ASSERT_TRUE(list.count() == 1);
            ASSERT_TRUE(list.at(0)->eql(set_req(Target::makeSpecial(special), red)));
            list.deinit();
        }
    }
}

TEST(color, OSC_4_multiple_requests) {
    /* printf '\e]4;0;red;1;blue\e\\' */
    {
        List list;
        ASSERT_TRUE(parse(Operation::osc_4, "0;red;1;blue", &list));
        ASSERT_TRUE(list.count() == 2);
        ASSERT_TRUE(list.at(0)->eql(set_req(Target::makePalette(0), red)));
        ASSERT_TRUE(list.at(1)->eql(set_req(Target::makePalette(1), blue)));
        list.deinit();
    }

    /* Multiple requests with same index overwrite each other
     * printf '\e]4;0;red;0;blue\e\\' */
    {
        List list;
        ASSERT_TRUE(parse(Operation::osc_4, "0;red;0;blue", &list));
        ASSERT_TRUE(list.count() == 2);
        ASSERT_TRUE(list.at(0)->eql(set_req(Target::makePalette(0), red)));
        ASSERT_TRUE(list.at(1)->eql(set_req(Target::makePalette(0), blue)));
        list.deinit();
    }
}

TEST(color, OSC_104) {
    char body[32];

    /* Test every palette index */
    for (unsigned idx = 0; idx < 255; idx++) {
        /* Simple color set
         * printf '\e]104;0\\' */
        {
            snprintf(body, sizeof(body), "%u", idx);
            List list;
            ASSERT_TRUE(parse(Operation::osc_104, body, &list));
            ASSERT_TRUE(list.count() == 1);
            ASSERT_TRUE(list.at(0)->eql(reset_req(Target::makePalette((uint8_t)idx))));
            list.deinit();
        }
    }

    /* Test every special color */
    for (unsigned i = 0; i < special_count; i++) {
        const Special special = (Special)i;

        /* Simple color set
         * printf '\e]104;256\\' */
        {
            snprintf(body, sizeof(body), "%u", 256 + i);
            List list;
            ASSERT_TRUE(parse(Operation::osc_104, body, &list));
            ASSERT_TRUE(list.count() == 1);
            ASSERT_TRUE(list.at(0)->eql(reset_req(Target::makeSpecial(special))));
            list.deinit();
        }
    }
}

TEST(color, OSC_104_empty_index) {
    List list;
    ASSERT_TRUE(parse(Operation::osc_104, "0;;1", &list));
    ASSERT_TRUE(list.count() == 2);
    ASSERT_TRUE(list.at(0)->eql(reset_req(Target::makePalette(0))));
    ASSERT_TRUE(list.at(1)->eql(reset_req(Target::makePalette(1))));
    list.deinit();
}

TEST(color, OSC_104_invalid_index) {
    List list;
    ASSERT_TRUE(parse(Operation::osc_104, "ffff;1", &list));
    ASSERT_TRUE(list.count() == 1);
    ASSERT_TRUE(list.at(0)->eql(reset_req(Target::makePalette(1))));
    list.deinit();
}

TEST(color, OSC_104_reset_all) {
    List list;
    ASSERT_TRUE(parse(Operation::osc_104, "", &list));
    ASSERT_TRUE(list.count() == 1);
    ASSERT_TRUE(list.at(0)->eql(tag_req(Request::Tag::reset_palette)));
    list.deinit();
}

TEST(color, OSC_105_reset_all) {
    List list;
    ASSERT_TRUE(parse(Operation::osc_105, "", &list));
    ASSERT_TRUE(list.count() == 1);
    ASSERT_TRUE(list.at(0)->eql(tag_req(Request::Tag::reset_special)));
    list.deinit();
}

/* OSC 10-19: Get/Set Dynamic Colors */
TEST(color, OSC_10_to_19_dynamic) {
    for (size_t i = 0; i < sizeof(all_dynamic) / sizeof(all_dynamic[0]); i++) {
        const Dynamic color = all_dynamic[i];
        const Operation op = dynamic_op(color);

        /* Example script:
         * printf '\e]10;red\e\\' */
        {
            List list;
            ASSERT_TRUE(parse(op, "red", &list));
            ASSERT_TRUE(list.count() == 1);
            ASSERT_TRUE(list.at(0)->eql(set_req(Target::makeDynamic(color), red)));
            list.deinit();
        }
    }
}

TEST(color, OSC_10_to_19_dynamic_multiple) {
    /* Example script:
     * printf '\e]11;red;blue\e\\' */
    {
        List list;
        ASSERT_TRUE(parse(Operation::osc_11, "red;blue", &list));
        ASSERT_TRUE(list.count() == 2);
        ASSERT_TRUE(list.at(0)->eql(set_req(Target::makeDynamic(Dynamic::background), red)));
        ASSERT_TRUE(list.at(1)->eql(set_req(Target::makeDynamic(Dynamic::cursor), blue)));
        list.deinit();
    }
}

/* OSC 110-119: Reset Dynamic Colors */
TEST(color, OSC_110_to_119_reset_dynamic) {
    for (size_t i = 0; i < sizeof(all_dynamic) / sizeof(all_dynamic[0]); i++) {
        const Dynamic color = all_dynamic[i];
        const Operation op = dynamic_reset_op(color);

        /* Example script:
         * printf '\e]110\e\\' */
        {
            List list;
            ASSERT_TRUE(parse(op, "", &list));
            ASSERT_TRUE(list.count() == 1);
            ASSERT_TRUE(list.at(0)->eql(reset_req(Target::makeDynamic(color))));
            list.deinit();
        }

        /* xterm allows a trailing semicolon. script to verify:
         *
         * printf '\e]110;\e\\' */
        {
            List list;
            ASSERT_TRUE(parse(op, ";", &list));
            ASSERT_TRUE(list.count() == 1);
            ASSERT_TRUE(list.at(0)->eql(reset_req(Target::makeDynamic(color))));
            list.deinit();
        }

        /* xterm does NOT allow any whitespace
         *
         * printf '\e]110 \e\\' */
        {
            List list;
            ASSERT_TRUE(parse(op, " ", &list));
            ASSERT_TRUE(list.count() == 0);
            list.deinit();
        }
    }
}
