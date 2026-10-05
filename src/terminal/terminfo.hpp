/* The XTGETTCAP half of Ghostty src/terminfo/Source.zig and ghostty.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: Ghostty builds `terminfo.ghostty.xtgettcapMap()` at comptime from
 * its terminfo entry. There is no comptime here, so tools/gen_terminfo.py
 * reproduces that table and emits terminfo_xtgettcap.inc; this header is
 * the lookup over it. The terminfo source encoder (Source.encode) is not
 * needed by the stream handler and is not ported.
 */

#pragma once
#ifndef WISP_TERMINAL_TERMINFO_HPP
#define WISP_TERMINAL_TERMINFO_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace wisp {
namespace terminal {
namespace terminfo {

/* One row of the XTGETTCAP map: a hex-encoded capability name and the
 * FULL response to send for it, escape sequences included. The response
 * is NUL-terminated so it can be handed to a write_pty callback that
 * wants a sentinel without copying, as upstream's is. */
struct XtgettcapEntry {
    const char *key;
    const char *response;
};

#include "terminfo_xtgettcap.inc"

static const size_t xtgettcap_entries_len = sizeof(xtgettcap_entries) / sizeof(xtgettcap_entries[0]);

/* Returns a StaticStringMap for all of the capabilities in this terminfo.
 * The value is the value that should be sent as a response to XTGETTCAP.
 * Important: the value is the FULL response included the escape sequences.
 *
 * Wisp: std.StaticStringMap.get over a sorted table. */
inline const char *xtgettcapGet(const uint8_t *key, size_t key_len) {
    size_t lo = 0;
    size_t hi = xtgettcap_entries_len;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const char *k = xtgettcap_entries[mid].key;
        const size_t k_len = strlen(k);
        const size_t n = k_len < key_len ? k_len : key_len;
        int cmp = n == 0 ? 0 : memcmp(k, key, n);
        if (cmp == 0) {
            if (k_len < key_len) cmp = -1;
            else if (k_len > key_len) cmp = 1;
        }
        if (cmp == 0) return xtgettcap_entries[mid].response;
        if (cmp < 0) lo = mid + 1;
        else hi = mid;
    }
    return nullptr;
}

} /* namespace terminfo */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_TERMINFO_HPP */
