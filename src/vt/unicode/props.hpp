/* Transliterated from Ghostty src/unicode/props.zig, props_table.zig,
 * lut.zig and main.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Property set per codepoint that Ghostty cares about.
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: upstream builds props_table.zig at build time from the uucode
 * package. props_table.inc next to this file is the same table, generated
 * from the same Unicode Character Database with the same rules, and the
 * packed struct is stored as the bits of its u16 backing integer.
 */

#pragma once
#ifndef WISP_VT_UNICODE_PROPS_HPP
#define WISP_VT_UNICODE_PROPS_HPP

#include <stdint.h>

namespace wisp {
namespace vt {
namespace unicode {

/* uucode types.GraphemeBreakNoControl */
enum class GraphemeBreakNoControl : uint8_t {
    other,
    prepend,
    regional_indicator,
    spacing_mark,
    l,
    v,
    t,
    lv,
    lvt,
    zwj,
    zwnj,
    extended_pictographic,
    emoji_modifier_base,
    emoji_modifier,
    /* extend, ==
     *   zwnj +
     *   indic_conjunct_break_extend +
     *   indic_conjunct_break_linker_extend */
    indic_conjunct_break_extend,
    indic_conjunct_break_linker_extend,
    /* InCB=Linker with Grapheme_Cluster_Break=Other (U+1CF5, U+1CF6, U+11A3A
     * as of Unicode 18), so unlike `indic_conjunct_break_linker_extend` it is not
     * part of `extend`. */
    indic_conjunct_break_linker_other,
    indic_conjunct_break_consonant,
};

/* Wisp: packed struct(u16). Bits: width 0..1, width_zero_in_grapheme 2,
 * grapheme_break 3..7, emoji_vs_base 8, padding 9..15. */
struct Properties {
    uint16_t bits;

    /* Codepoint width. We clamp to [0, 2] since Ghostty handles control
     * characters and we max out at 2 for wide characters (i.e. 3-em dash
     * becomes a 2-em dash). */
    uint8_t width() const { return (uint8_t)(bits & 0x3); }

    /* Whether the code point does not contribute to the width of a grapheme
     * cluster (not used for single code point cells). */
    bool width_zero_in_grapheme() const { return (bits & 0x4) != 0; }

    /* Grapheme break property. */
    GraphemeBreakNoControl grapheme_break() const {
        return (GraphemeBreakNoControl)((bits >> 3) & 0x1F);
    }

    /* Emoji VS compatibility */
    bool emoji_vs_base() const { return (bits & 0x100) != 0; }

    bool eql(const Properties &o) const { return bits == o.bits; }
};

namespace props_detail {
#include "props_table.inc"
} /* namespace props_detail */

/* lut.zig Tables(Elem) */
struct Table {
    /* Given a codepoint, returns the mapping for that codepoint. */
    static Properties get(uint32_t cp) {
        const uint32_t high = cp >> 8;
        const uint32_t low = cp & 0xFF;
        Properties p;
        p.bits = props_detail::props_stage3[props_detail::props_stage2[props_detail::props_stage1[high] + low]];
        return p;
    }
};

static const Table table = Table();

/* Returns the terminal display width of a codepoint in terminal
 * grid cells: 0, 1, or 2.
 *
 * This is the same width table the terminal uses when laying out
 * printed text: 0 for zero-width codepoints (controls, combining
 * marks, default-ignorables, surrogates), 2 for wide codepoints
 * (East Asian Wide/Fullwidth, regional indicators, clamped at 2),
 * and 1 otherwise.
 *
 * This operates on a single codepoint and cannot account for
 * grapheme-cluster-level width rules (VS16, combining sequences);
 * callers needing cluster-accurate widths should use graphemeWidth().
 * Summing per-codepoint widths is only correct when mode 2027 is
 * disabled. */
inline uint8_t codepointWidth(uint32_t cp) { return Table::get(cp).width(); }

} /* namespace unicode */
} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_UNICODE_PROPS_HPP */
