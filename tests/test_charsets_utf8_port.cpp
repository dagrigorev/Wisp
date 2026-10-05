/* Transliterated from the test blocks in Ghostty src/terminal/charsets.zig
 * and src/terminal/UTF8Decoder.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include "test_helpers.h"
#include "charsets.hpp"
#include "utf8_decoder.hpp"

#include <string.h>

using namespace wisp::terminal;

/* charsets.zig: the unnamed test. Wisp: a table is a pointer, so its
 * length is checked through the Table type it points into. */
TEST(charsets, table_lengths) {
    const charsets::Charset all[] = {
        charsets::Charset::utf8, charsets::Charset::ascii,
        charsets::Charset::british, charsets::Charset::dec_special,
    };
    for (size_t i = 0; i < 4; i++) {
        /* utf8 has no table */
        if (all[i] == charsets::Charset::utf8) continue;

        const uint16_t *tbl = charsets::table(all[i]);
        ASSERT_TRUE(tbl != nullptr);
        ASSERT_TRUE(sizeof(charsets::Table::v) / sizeof(uint16_t) == 256);
    }
}

TEST(UTF8Decoder, ASCII) {
    UTF8Decoder d;
    char out[14] = {0};
    const char *in = "Hello, World!";
    for (size_t i = 0; in[i]; i++) {
        const UTF8Decoder::Result res = d.next((uint8_t)in[i]);
        ASSERT_TRUE(res.consumed);
        if (res.has_codepoint) out[i] = (char)res.codepoint;
    }

    ASSERT_TRUE(strcmp(out, "Hello, World!") == 0);
}

TEST(UTF8Decoder, Well_formed_utf_8) {
    UTF8Decoder d;
    uint32_t out[4];
    size_t i = 0;
    /* 4 bytes, 3 bytes, 2 bytes, 1 byte: "😄✤ÁA" */
    const char *in = "\360\237\230\204\342\234\244\303\201A";
    for (const char *b = in; *b; b++) {
        bool consumed = false;
        while (!consumed) {
            const UTF8Decoder::Result res = d.next((uint8_t)*b);
            consumed = res.consumed;
            /* There are no errors in this sequence, so
             * every byte should be consumed first try. */
            ASSERT_TRUE(consumed == true);
            if (res.has_codepoint) {
                out[i] = res.codepoint;
                i += 1;
            }
        }
    }

    const uint32_t want[] = { 0x1F604, 0x2724, 0xC1, 0x41 };
    ASSERT_TRUE(i == 4 && memcmp(out, want, sizeof(want)) == 0);
}

TEST(UTF8Decoder, Partially_invalid_utf_8) {
    UTF8Decoder d;
    uint32_t out[5];
    size_t i = 0;
    /* Illegally terminated sequence, valid sequence, illegal surrogate pair. */
    const char *in = "\360\237" "\360\237\230\204" "\355\240\200";
    for (const char *b = in; *b; b++) {
        bool consumed = false;
        while (!consumed) {
            const UTF8Decoder::Result res = d.next((uint8_t)*b);
            consumed = res.consumed;
            if (res.has_codepoint) {
                ASSERT_TRUE(i < 5);
                out[i] = res.codepoint;
                i += 1;
            }
        }
    }

    const uint32_t want[] = { 0xFFFD, 0x1F604, 0xFFFD, 0xFFFD, 0xFFFD };
    ASSERT_TRUE(i == 5 && memcmp(out, want, sizeof(want)) == 0);
}
