/* Transliterated from Ghostty src/terminal/charsets.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 * Comptime tables are built once, on first use.
 */

#pragma once
#ifndef WISP_TERMINAL_CHARSETS_HPP
#define WISP_TERMINAL_CHARSETS_HPP

#include <stddef.h>
#include <stdint.h>

namespace wisp {
namespace terminal {
namespace charsets {

/* The available charset slots for a terminal. */
enum class Slots : uint8_t { G0, G1, G2, G3 };

/* The name of the active slots. */
enum class ActiveSlot : uint8_t { GL, GR };

/* The list of supported character sets and their associated tables. */
enum class Charset : uint8_t { utf8, ascii, british, dec_special };

/* Our table length is 256 so we can contain all ASCII chars. */
static const size_t table_len = 255 + 1;

struct Table {
    uint16_t v[table_len];
};

/* Creates a table that maps ASCII to ASCII as a getting started point. */
inline Table initTable() {
    Table result;
    size_t i = 0;
    while (i < table_len) {
        result.v[i] = (uint16_t)i;
        i += 1;
    }
    /* assert(i == table_len) */
    return result;
}

/* Just a basic c => c ascii table */
inline const Table &ascii_table() {
    static const Table tbl = initTable();
    return tbl;
}

/* https://vt100.net/docs/vt220-rm/chapter2.html */
inline const Table &british_table() {
    static const Table tbl = [] {
        Table t = initTable();
        t.v[0x23] = 0x00a3;
        return t;
    }();
    return tbl;
}

/* https://en.wikipedia.org/wiki/DEC_Special_Graphics */
inline const Table &dec_special_table() {
    static const Table tbl = [] {
        Table t = initTable();
        t.v[0x60] = 0x25C6;
        t.v[0x61] = 0x2592;
        t.v[0x62] = 0x2409;
        t.v[0x63] = 0x240C;
        t.v[0x64] = 0x240D;
        t.v[0x65] = 0x240A;
        t.v[0x66] = 0x00B0;
        t.v[0x67] = 0x00B1;
        t.v[0x68] = 0x2424;
        t.v[0x69] = 0x240B;
        t.v[0x6a] = 0x2518;
        t.v[0x6b] = 0x2510;
        t.v[0x6c] = 0x250C;
        t.v[0x6d] = 0x2514;
        t.v[0x6e] = 0x253C;
        t.v[0x6f] = 0x23BA;
        t.v[0x70] = 0x23BB;
        t.v[0x71] = 0x2500;
        t.v[0x72] = 0x23BC;
        t.v[0x73] = 0x23BD;
        t.v[0x74] = 0x251C;
        t.v[0x75] = 0x2524;
        t.v[0x76] = 0x2534;
        t.v[0x77] = 0x252C;
        t.v[0x78] = 0x2502;
        t.v[0x79] = 0x2264;
        t.v[0x7a] = 0x2265;
        t.v[0x7b] = 0x03C0;
        t.v[0x7c] = 0x2260;
        t.v[0x7d] = 0x00A3;
        t.v[0x7e] = 0x00B7;
        return t;
    }();
    return tbl;
}

/* The table for the given charset. This returns a pointer to a
 * slice that is guaranteed to be 255 chars that can be used to map
 * ASCII to the given charset.
 *
 * Wisp: the slice is a pointer to table_len entries. */
inline const uint16_t *table(Charset set) {
    switch (set) {
        case Charset::british: return british_table().v;
        case Charset::dec_special: return dec_special_table().v;

        /* utf8 is not a table, callers should double-check if the
         * charset is utf8 and NOT use tables. */
        case Charset::utf8: break; /* unreachable */

        /* recommended that callers just map ascii directly but we can
         * support a table */
        case Charset::ascii: return ascii_table().v;
    }
    return nullptr;
}

} /* namespace charsets */

/* ─── Wisp adapters for the reimplemented terminal.hpp ───────────────────
 * Not upstream. terminal.hpp is not transliterated yet; these keep its
 * existing calls working on top of the tables above until Terminal.zig
 * replaces it. */

using charsets::Charset;

inline bool charset_from_designator(uint8_t b, Charset *out) {
    switch (b) {
        case 'B': *out = Charset::ascii; return true;
        case '0': *out = Charset::dec_special; return true;
        case 'A': *out = Charset::british; return true;
        default: return false;
    }
}

/* Terminal.print: values above u8 pass through; utf8 is not a table. */
inline uint32_t charset_map(Charset cs, uint32_t cp) {
    if (cp > 0xFF || cs == Charset::utf8) return cp;
    return charsets::table(cs)[cp];
}

} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_CHARSETS_HPP */
