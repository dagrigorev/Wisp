/* Transliterated from the "xtgettcap map" test in Ghostty
 * src/terminfo/Source.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Wisp: upstream's test builds a synthetic Source at comptime and calls
 * xtgettcapMap() on it. The table here is generated from Ghostty's own
 * terminfo entry by tools/gen_terminfo.py, so the same assertions are made
 * against that entry's values: `am` (boolean) and `Smulx` (a parameterized
 * string, kept verbatim) are identical to upstream's, `kf1` carries the
 * same "\\EOP", and `kbs` is Ghostty's "^?" rather than the test's "^H", so
 * it encodes 7F instead of 08. Upstream's synthetic `kx` has no counterpart
 * in the real entry. Upstream's "encode" test covers the terminfo source
 * encoder, which is not ported.
 */

#include <string.h>

#include <string>

#include "test_helpers.h"
#include "../terminal/terminfo.hpp"

using namespace wisp;

namespace terminfo = wisp::terminal::terminfo;

/* Wisp: hexencode("am") */
static std::string hexencode(const char *s) {
    static const char *digits = "0123456789ABCDEF";
    std::string out;
    for (size_t i = 0; s[i] != 0; i++) {
        const uint8_t b = (uint8_t)s[i];
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0xF]);
    }
    return out;
}

static const char *get(const char *name) {
    const std::string key = hexencode(name);
    return terminfo::xtgettcapGet((const uint8_t *)key.data(), key.size());
}

TEST(terminfo, xtgettcap_map) {
    ASSERT_TRUE(get("am") != nullptr);
    ASSERT_STR_EQ("\x1b" "P1+r616D\x1b" "\\", get("am"));

    ASSERT_TRUE(get("kbs") != nullptr);
    ASSERT_STR_EQ("\x1b" "P1+r6B6273=7F\x1b" "\\", get("kbs"));

    ASSERT_TRUE(get("kf1") != nullptr);
    ASSERT_STR_EQ("\x1b" "P1+r6B6631=1B4F50\x1b" "\\", get("kf1"));

    ASSERT_TRUE(get("Smulx") != nullptr);
    ASSERT_STR_EQ("\x1b" "P1+r536D756C78=5C455B343A25703125646D\x1b" "\\", get("Smulx"));
}

/* Wisp: the three entries xtgettcapMap adds beside the capability list. */
TEST(terminfo, xtgettcap_map_extra_entries) {
    ASSERT_STR_EQ("\x1b" "P1+r544E=787465726D2D67686F73747479\x1b" "\\", get("TN"));
    ASSERT_STR_EQ("\x1b" "P1+r436F=323536\x1b" "\\", get("Co"));
    ASSERT_STR_EQ("\x1b" "P1+r524742=38\x1b" "\\", get("RGB"));
}

/* Wisp: the lookup is a binary search over the generated table, so every
 * key must be findable and unknown keys must miss. */
TEST(terminfo, xtgettcap_map_lookup) {
    for (size_t i = 0; i < terminfo::xtgettcap_entries_len; i++) {
        const char *key = terminfo::xtgettcap_entries[i].key;
        ASSERT_TRUE(terminfo::xtgettcapGet((const uint8_t *)key, strlen(key)) ==
                    terminfo::xtgettcap_entries[i].response);
    }
    ASSERT_TRUE(get("nope") == nullptr);
    ASSERT_TRUE(terminfo::xtgettcapGet((const uint8_t *)"", 0) == nullptr);
    /* A prefix of a real key is not a match. */
    ASSERT_TRUE(terminfo::xtgettcapGet((const uint8_t *)"61", 2) == nullptr);
}
