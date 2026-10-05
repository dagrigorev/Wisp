/* Transliterated from Ghostty src/unicode/grapheme.zig and the grapheme
 * break implementation of uucode (src/grapheme.zig)
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * Copyright (c) Jacob Sandlund and uucode contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: upstream gets BreakState and computeGraphemeBreakNoControl from the
 * uucode package; both are transliterated here. The Precompute table is
 * filled on first use instead of at comptime.
 */

#pragma once
#ifndef WISP_VT_UNICODE_GRAPHEME_HPP
#define WISP_VT_UNICODE_GRAPHEME_HPP

#include <stddef.h>
#include <stdint.h>

#include "props.hpp"

namespace wisp {
namespace vt {
namespace unicode {

typedef GraphemeBreakNoControl GB;

/* uucode grapheme.BreakState: packed struct(u3) {
 *     after_linker: bool, base: Base }. */
struct BreakState {
    enum class Base : uint8_t { default_, extended_pictographic, regional_indicator };

    bool after_linker;
    Base base;

    BreakState() : after_linker(false), base(Base::default_) {}

    static const size_t table_len = 3 * 2 - 1;

    size_t tableIndex() const { return (size_t)after_linker | ((size_t)base << 1); }

    static BreakState fromTableIndex(size_t i) {
        BreakState s;
        s.after_linker = (i & 1) != 0;
        s.base = (Base)((i >> 1) & 0x3);
        return s;
    }
};

namespace grapheme_detail {

inline bool isIndicConjunctBreakExtendNoControl(GB gb) {
    return gb == GB::indic_conjunct_break_extend || gb == GB::zwj;
}

inline bool isIndicConjunctBreakLinkerNoControl(GB gb) {
    return gb == GB::indic_conjunct_break_linker_extend || gb == GB::indic_conjunct_break_linker_other;
}

/* Despite `emoji_modifier` being `extend` according to
 * GraphemeBreakProperty.txt and UAX #29 (in addition to tests in
 * GraphemeBreakTest.txt), UTS #51 states: `emoji_modifier_sequence :=
 * emoji_modifier_base emoji_modifier` in ED-13 (emoji modifier sequence) under
 * 1.4.4 (Emoji Modifiers) [...] Here we decide to diverge from the grapheme
 * break spec, which is allowed under "tailored" grapheme clusters. */
inline bool isExtendNoControl(GB gb) {
    return gb == GB::zwnj || gb == GB::indic_conjunct_break_extend ||
           gb == GB::indic_conjunct_break_linker_extend;
}

inline bool isExtendedPictographicNoControl(GB gb) {
    return gb == GB::extended_pictographic || gb == GB::emoji_modifier_base;
}

inline bool keepsExtendedPictographic(GB gb) {
    switch (gb) {
    /* Keep state if in possibly valid sequence */
    case GB::indic_conjunct_break_extend:        /* extend */
    case GB::indic_conjunct_break_linker_extend: /* extend */
    case GB::zwnj:                               /* extend */
    case GB::zwj:
    case GB::extended_pictographic:
    case GB::emoji_modifier_base:
    case GB::emoji_modifier: return true;

    default: return false;
    }
}

} /* namespace grapheme_detail */

inline bool computeGraphemeBreakNoControl(GB gb1, GB gb2, BreakState *state) {
    using namespace grapheme_detail;

    /* Whether the next flag is set depends only on the input, not on the
     * rules below, which only read `after_linker` and only write `base`. */
    const bool after_linker =
        isIndicConjunctBreakLinkerNoControl(gb1) || (state->after_linker && isIndicConjunctBreakExtendNoControl(gb1));
    state->after_linker = after_linker && isIndicConjunctBreakExtendNoControl(gb2);

    switch (state->base) {
    case BreakState::Base::regional_indicator:
        if (gb1 != GB::regional_indicator || gb2 != GB::regional_indicator) {
            state->base = BreakState::Base::default_;
        }
        break;

    case BreakState::Base::extended_pictographic:
        if (!keepsExtendedPictographic(gb1)) state->base = BreakState::Base::default_;
        if (!keepsExtendedPictographic(gb2)) state->base = BreakState::Base::default_;
        break;

    case BreakState::Base::default_: break;
    }

    /* GB6: L x (L | V | LV | VT) */
    if (gb1 == GB::l) {
        if (gb2 == GB::l || gb2 == GB::v || gb2 == GB::lv || gb2 == GB::lvt) return false;
    }

    /* GB7: (LV | V) x (V | T) */
    if (gb1 == GB::lv || gb1 == GB::v) {
        if (gb2 == GB::v || gb2 == GB::t) return false;
    }

    /* GB8: (LVT | T) x T */
    if (gb1 == GB::lvt || gb1 == GB::t) {
        if (gb2 == GB::t) return false;
    }

    /* GB9a: SpacingMark */
    if (gb2 == GB::spacing_mark) return false;

    /* GB9b: Prepend */
    if (gb1 == GB::prepend) return false;

    /* GB9c: InCB=Linker InCB=Extend* x InCB=Consonant */
    if (after_linker && gb2 == GB::indic_conjunct_break_consonant) {
        state->base = BreakState::Base::default_;
        return false;
    }

    /* GB11: Emoji ZWJ sequence and Emoji modifier sequence */
    if (isExtendedPictographicNoControl(gb1)) {
        if (isExtendNoControl(gb2) || gb2 == GB::zwj) {
            state->base = BreakState::Base::extended_pictographic;
            return false;
        }

        if (gb1 == GB::emoji_modifier_base && gb2 == GB::emoji_modifier) {
            state->base = BreakState::Base::extended_pictographic;
            return false;
        }
    } else if (state->base == BreakState::Base::extended_pictographic) {
        if ((isExtendNoControl(gb1) || gb1 == GB::emoji_modifier) && (isExtendNoControl(gb2) || gb2 == GB::zwj)) {
            return false;
        } else if (gb1 == GB::zwj && isExtendedPictographicNoControl(gb2)) {
            state->base = BreakState::Base::default_;
            return false;
        } else {
            state->base = BreakState::Base::default_;
        }
    }

    /* GB12 and GB13: Regional Indicator */
    if (gb1 == GB::regional_indicator && gb2 == GB::regional_indicator) {
        if (state->base == BreakState::Base::default_) {
            state->base = BreakState::Base::regional_indicator;
            return false;
        } else {
            state->base = BreakState::Base::default_;
            return true;
        }
    }

    /* GB9: x (Extend | ZWJ) */
    if (isExtendNoControl(gb2) || gb2 == GB::zwj) return false;

    /* GB999: Otherwise, break everywhere */
    return true;
}

/* Width change requested by a codepoint that continues a grapheme cluster. */
enum class GraphemeWidthEffect {
    /* Do not append the codepoint to the cluster and leave break state as it
     * was before seeing it. */
    ignore,

    /* Append the codepoint but leave the current cluster width unchanged. */
    no_change,

    /* Make the cluster occupy two terminal cells. */
    wide,

    /* Make the cluster occupy one terminal cell. */
    narrow,
};

/* Result of measuring the first grapheme cluster in a codepoint slice. */
struct GraphemeWidth {
    /* Number of codepoints consumed from the input slice. */
    size_t len;

    /* Display width in terminal cells. */
    uint8_t width;

    bool eql(const GraphemeWidth &o) const { return len == o.len && width == o.width; }
};

/* This is all the structures and data for the precomputed lookup table
 * for all possible permutations of state and grapheme break properties.
 * Precomputation requires 2^13 keys of byte-sized values so the whole
 * table is 8KB. */
struct Precompute {
    /* Wisp: packed struct(u13) { state: u3, gb1: u5, gb2: u5 }. */
    static size_t index(GB gb1, GB gb2, const BreakState &state) {
        return state.tableIndex() | ((size_t)gb1 << 3) | ((size_t)gb2 << 8);
    }

    /* Wisp: packed struct(u8) { result: bool, state: u3, padding: u4 }. */
    struct Value {
        uint8_t bits;
        bool result() const { return (bits & 1) != 0; }
        BreakState state() const { return BreakState::fromTableIndex((bits >> 1) & 0x7); }
    };

    static const size_t data_len = 8192;

    static const Value *data() {
        static Value result[data_len];
        static bool built = false;
        if (!built) {
            const size_t gb_count = (size_t)GB::indic_conjunct_break_consonant + 1;
            for (size_t state_int = 0; state_int < BreakState::table_len; state_int++) {
                for (size_t g1 = 0; g1 < gb_count; g1++) {
                    for (size_t g2 = 0; g2 < gb_count; g2++) {
                        BreakState state = BreakState::fromTableIndex(state_int);
                        const GB gb1 = (GB)g1;
                        const GB gb2 = (GB)g2;
                        const size_t key = index(gb1, gb2, state);
                        const bool v = computeGraphemeBreakNoControl(gb1, gb2, &state);
                        Value value;
                        value.bits = (uint8_t)((v ? 1 : 0) | (state.tableIndex() << 1));
                        result[key] = value;
                    }
                }
            }
            built = true;
        }
        return result;
    }
};

/* Determines if there is a grapheme break between two codepoints. This
 * must be called sequentially maintaining the state between calls.
 *
 * This function does NOT work with control characters. Control characters,
 * line feeds, and carriage returns are expected to be filtered out before
 * calling this function. This is because this function is tuned for
 * Ghostty. */
inline bool graphemeBreak(uint32_t cp1, uint32_t cp2, BreakState *state) {
    const Precompute::Value value =
        Precompute::data()[Precompute::index(Table::get(cp1).grapheme_break(), Table::get(cp2).grapheme_break(),
                                             *state)];
    *state = value.state();
    return value.result();
}

/* Returns the width effect of appending cp after prev within a grapheme.
 *
 * This is the shared width-decision kernel for the streaming terminal
 * printer and for graphemeWidth. It assumes graphemeBreak has already said
 * there is no break between prev and cp; it does not perform segmentation.
 *
 * The .ignore result is important for invalid emoji variation selectors. The
 * terminal does not store those selectors in the cell, so callers must also
 * restore their grapheme break state and leave prev unchanged when they see
 * .ignore. */
inline GraphemeWidthEffect graphemeWidthEffect(uint32_t prev, uint32_t cp) {
    /* Emoji variation selectors modify the width of a valid base:
     * VS16 makes the grapheme wide and VS15 makes it narrow. Check that
     * prev forms a valid variation sequence in emoji-variation-sequences.txt;
     * if it does not, ignore the selector entirely. */
    if (cp == 0xFE0F || cp == 0xFE0E) {
        const Properties prev_props = Table::get(prev);
        if (!prev_props.emoji_vs_base()) return GraphemeWidthEffect::ignore;

        return cp == 0xFE0F ? GraphemeWidthEffect::wide : GraphemeWidthEffect::narrow;
    }

    /* If a code point contributes to the width of a grapheme, the whole
     * grapheme is at least width 2 because the first code point must be at
     * least width 1 to start. Prepend code points could effectively mean
     * the first code point should be width 0, but we don't handle that yet. */
    if (!Table::get(cp).width_zero_in_grapheme()) return GraphemeWidthEffect::wide;

    return GraphemeWidthEffect::no_change;
}

inline bool invalidCodepoint(uint32_t cp) { return cp > 0x10FFFF; }

/* Measures the first grapheme cluster in cps using the same segmentation and
 * width rules as Terminal.print with mode 2027.
 *
 * This is not a streaming API: cps must contain a complete first grapheme
 * cluster or the logical end of the string. If bytes/codepoints arrive in
 * chunks, keep buffering when this consumes all available codepoints and more
 * input may still arrive.
 *
 * For codepoint types wider than u21, values greater than U+10FFFF are
 * accepted so FFI-facing callers can use u32 input without trapping. An
 * invalid value consumes one codepoint at width 1 when it starts the slice,
 * and terminates the current cluster when it appears later. For u21 callers
 * these checks are comptime-dead.
 *
 * Wisp: the comptime type parameter is the `check_invalid` flag. */
inline GraphemeWidth graphemeWidth(const uint32_t *cps, size_t cps_len, bool check_invalid = false) {
    GraphemeWidth result;
    if (cps_len == 0) {
        result.len = 0;
        result.width = 0;
        return result;
    }

    /* The C API accepts u32 codepoints, so it can receive values outside
     * Unicode's range. Guard before narrowing to u21; native u21 callers
     * skip this path at comptime. */
    if (check_invalid && invalidCodepoint(cps[0])) {
        result.len = 1;
        result.width = 1;
        return result;
    }

    size_t len = 1;
    uint8_t width = Table::get(cps[0]).width();
    uint32_t prev = cps[0];
    BreakState state;

    for (; len < cps_len; len += 1) {
        /* Treat invalid u32 input as a boundary so a valid prefix cluster can
         * still be returned without attempting to narrow the invalid value. */
        if (check_invalid && invalidCodepoint(cps[len])) break;

        const uint32_t cp = cps[len];
        const BreakState state_before = state;
        if (graphemeBreak(prev, cp, &state)) break;

        switch (graphemeWidthEffect(prev, cp)) {
        case GraphemeWidthEffect::ignore: state = state_before; break;
        case GraphemeWidthEffect::no_change: prev = cp; break;
        case GraphemeWidthEffect::wide:
            width = 2;
            prev = cp;
            break;
        case GraphemeWidthEffect::narrow:
            width = 1;
            prev = cp;
            break;
        }
    }

    result.len = len;
    result.width = width;
    return result;
}

} /* namespace unicode */
} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_UNICODE_GRAPHEME_HPP */
