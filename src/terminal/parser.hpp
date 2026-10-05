/* Transliterated from Ghostty src/terminal/Parser.zig and
 * src/terminal/parse_table.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * VT-series parser for escape and control sequences.
 *
 * This is implemented directly as the state machine described on
 * vt100.net: https://vt100.net/emu/dec_ansi_parser
 *
 * TRANSLITERATION. This is upstream's code rewritten line for line: the same
 * states in the same order, the same transition table built the same way,
 * the same field names and the same control flow. Where Zig has no direct
 * C++ equivalent the mapping is:
 *
 *   [3]?Action           Next — three Actions, each with a present flag
 *   union(enum) Action   a tag plus one field per payload
 *   []u8 / []u16 slices  a pointer and a length
 *   StaticBitSet         SepList, a bitmask with the same set/isSet/count
 *   *|= and +|=          explicit saturating arithmetic
 *   comptime table       built once on first use by the same genTable logic
 *
 * Comments are upstream's unless marked "Wisp:".
 *
 * OSC bytes go to osc.hpp, the transliteration of osc.zig, which decodes
 * each command into a typed value.
 */

#pragma once
#ifndef WISP_TERMINAL_PARSER_HPP
#define WISP_TERMINAL_PARSER_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "osc.hpp"

namespace wisp {
namespace terminal {
namespace parser {

/* ─── Parser.zig ─────────────────────────────────────────────────────────── */

/* States for the state machine */
enum class State : uint8_t {
    ground,
    escape,
    escape_intermediate,
    csi_entry,
    csi_intermediate,
    csi_param,
    csi_ignore,
    dcs_entry,
    dcs_param,
    dcs_intermediate,
    dcs_passthrough,
    dcs_ignore,
    osc_string,
    sos_pm_apc_string,
};

static const size_t STATE_COUNT = 14;

/* Transition action is an action that can be taken during a state
 * transition. This is more of an internal action, not one used by
 * end users, typically. */
enum class TransitionAction : uint8_t {
    none,
    ignore,
    print,
    execute,
    collect,
    param,
    esc_dispatch,
    csi_dispatch,
    put,
    osc_put,
    apc_put,
};

/* Maximum number of intermediate characters during parsing. This is
 * 4 because we also use the intermediates array for UTF8 decoding which
 * can be at most 4 bytes. */
static const size_t MAX_INTERMEDIATE = 4;

/* Maximum number of CSI parameters. This is arbitrary. Practically, the
 * only CSI command that uses more than 3 parameters is the SGR command
 * which can be infinitely long. 24 is a reasonable limit based on empirical
 * data. This used to be 16 but Kakoune has a SGR command that uses 17
 * parameters.
 *
 * We could in the future make this the static limit and then allocate after
 * but that's a lot more work and practically its so rare to exceed this
 * number. I implore TUI authors to not use more than this number of CSI
 * params, but I suspect we'll introduce a slow path with heap allocation
 * one day. */
static const size_t MAX_PARAMS = 24;

/* The list of separators used for CSI params. The value of the
 * bit can be mapped to Sep. The index of this bit set specifies
 * the separator AFTER that param. For example: 0;4:3 would have
 * index 1 set.
 *
 * Wisp: std.StaticBitSet(MAX_PARAMS). */
struct SepList {
    uint32_t mask;

    SepList() : mask(0) {}

    static SepList initEmpty() { return SepList(); }
    void set(size_t i) { mask |= (uint32_t)1u << i; }
    bool isSet(size_t i) const { return (mask >> i) & 1u; }

    size_t count() const {
        size_t n = 0;
        for (uint32_t m = mask; m; m &= m - 1) n++;
        return n;
    }
};

/* The separator used for CSI params. */
enum class Sep : uint8_t { semicolon = 0, colon = 1 };

/* Action is the action that a caller of the parser is expected to
 * take as a result of some input character. */
struct Action {
    enum class Tag : uint8_t {
        print,
        execute,
        csi_dispatch,
        esc_dispatch,
        osc_dispatch,
        dcs_hook,
        dcs_put,
        dcs_unhook,
        apc_start,
        apc_put,
        apc_end,
    };

    struct CSI {
        const uint8_t  *intermediates;
        size_t          intermediates_len;
        const uint16_t *params;
        size_t          params_len;
        SepList         params_sep;
        uint8_t         final_;
    };

    struct ESC {
        const uint8_t *intermediates;
        size_t         intermediates_len;
        uint8_t        final_;
    };

    struct DCS {
        const uint8_t  *intermediates;
        size_t          intermediates_len;
        const uint16_t *params;
        size_t          params_len;
        uint8_t         final_;
    };

    Tag tag;

    /* Draw character to the screen. This is a unicode codepoint. */
    uint32_t print;

    /* Execute the C0 or C1 function. Also the byte of dcs_put and
     * apc_put. */
    uint8_t byte;

    /* Execute the CSI command. Note that pointers within this
     * structure are only valid until the next call to "next". */
    CSI csi_dispatch;

    /* Execute the ESC command. */
    ESC esc_dispatch;

    /* Execute the OSC command. */
    ::wisp::terminal::osc::Command osc_dispatch;

    /* DCS-related events. */
    DCS dcs_hook;

    Action()
        : tag(Tag::print), print(0), byte(0), csi_dispatch(), esc_dispatch(),
          osc_dispatch(), dcs_hook() {
        memset(&csi_dispatch, 0, sizeof(csi_dispatch));
        memset(&esc_dispatch, 0, sizeof(esc_dispatch));
        memset(&dcs_hook, 0, sizeof(dcs_hook));
    }
};

/* Wisp: [3]?Action. */
struct Next {
    Action action[3];
    bool   present[3];

    Next() { present[0] = present[1] = present[2] = false; }

    bool has(int i) const { return present[i]; }
    const Action &operator[](int i) const { return action[i]; }
};

/* ─── parse_table.zig ────────────────────────────────────────────────────── */

/* The primary export of this section is "table", which contains a
 * generated state transition table for VT emulation.
 *
 * This is based on the vt100.net state machine:
 * https://vt100.net/emu/dec_ansi_parser
 * But has some modifications:
 *
 *   * csi_param accepts the colon character (':') since the SGR command
 *     accepts colon as a valid parameter value.
 */

/* Transition is the transition to take within the table */
struct Transition {
    State            state;
    TransitionAction action;
};

/* Wisp: [u8][State]Transition. */
struct Table {
    Transition t[256][STATE_COUNT];
};

/* Wisp: the ?Transition accumulator upstream uses to build the table. */
struct OptionalTable {
    Transition t[256][STATE_COUNT];
    bool       set[256][STATE_COUNT];
};

inline Transition transition(State state, TransitionAction action) {
    Transition tr;
    tr.state = state;
    tr.action = action;
    return tr;
}

inline void single(OptionalTable *t, uint8_t c, State s0, State s1,
                   TransitionAction a) {
    const size_t s0_int = (size_t)s0;
    t->t[c][s0_int] = transition(s1, a);
    t->set[c][s0_int] = true;
}

inline void range(OptionalTable *t, uint8_t from, uint8_t to, State s0,
                  State s1, TransitionAction a) {
    uint8_t i = from;
    while (i <= to) {
        single(t, i, s0, s1, a);
        /* If 'to' is 0xFF, our next pass will overflow. Return early to
         * prevent the loop from executing it's continue expression */
        if (i == to) break;
        i++;
    }
}

/* Function to generate the full state transition table for VT emulation. */
inline void genTable(Table *final_) {
    typedef TransitionAction A;

    /* We accumulate using an "optional" table so we can detect duplicates. */
    static OptionalTable result;
    memset(&result, 0, sizeof(result));

    /* anywhere transitions */
    for (size_t field = 0; field < STATE_COUNT; field++) {
        const State source = (State)field;

        /* anywhere => ground */
        single(&result, 0x18, source, State::ground, A::execute);
        single(&result, 0x1A, source, State::ground, A::execute);
        range(&result, 0x80, 0x8F, source, State::ground, A::execute);
        range(&result, 0x91, 0x97, source, State::ground, A::execute);
        single(&result, 0x99, source, State::ground, A::execute);
        single(&result, 0x9A, source, State::ground, A::execute);
        single(&result, 0x9C, source, State::ground, A::none);

        /* anywhere => escape */
        single(&result, 0x1B, source, State::escape, A::none);

        /* anywhere => sos_pm_apc_string */
        single(&result, 0x98, source, State::sos_pm_apc_string, A::none);
        single(&result, 0x9E, source, State::sos_pm_apc_string, A::none);
        single(&result, 0x9F, source, State::sos_pm_apc_string, A::none);

        /* anywhere => csi_entry */
        single(&result, 0x9B, source, State::csi_entry, A::none);

        /* anywhere => dcs_entry */
        single(&result, 0x90, source, State::dcs_entry, A::none);

        /* anywhere => osc_string */
        single(&result, 0x9D, source, State::osc_string, A::none);
    }

    /* ground */
    {
        /* events */
        single(&result, 0x19, State::ground, State::ground, A::execute);
        range(&result, 0, 0x17, State::ground, State::ground, A::execute);
        range(&result, 0x1C, 0x1F, State::ground, State::ground, A::execute);
        range(&result, 0x20, 0x7F, State::ground, State::ground, A::print);
    }

    /* escape_intermediate */
    {
        const State source = State::escape_intermediate;

        single(&result, 0x19, source, source, A::execute);
        range(&result, 0, 0x17, source, source, A::execute);
        range(&result, 0x1C, 0x1F, source, source, A::execute);
        range(&result, 0x20, 0x2F, source, source, A::collect);
        single(&result, 0x7F, source, source, A::ignore);

        /* => ground */
        range(&result, 0x30, 0x7E, source, State::ground, A::esc_dispatch);
    }

    /* sos_pm_apc_string */
    {
        const State source = State::sos_pm_apc_string;

        /* events */
        single(&result, 0x19, source, source, A::apc_put);
        range(&result, 0, 0x17, source, source, A::apc_put);
        range(&result, 0x1C, 0x1F, source, source, A::apc_put);
        range(&result, 0x20, 0x7F, source, source, A::apc_put);
    }

    /* escape */
    {
        const State source = State::escape;

        /* events */
        single(&result, 0x19, source, source, A::execute);
        range(&result, 0, 0x17, source, source, A::execute);
        range(&result, 0x1C, 0x1F, source, source, A::execute);
        single(&result, 0x7F, source, source, A::ignore);

        /* => ground */
        range(&result, 0x30, 0x4F, source, State::ground, A::esc_dispatch);
        range(&result, 0x51, 0x57, source, State::ground, A::esc_dispatch);
        range(&result, 0x60, 0x7E, source, State::ground, A::esc_dispatch);
        single(&result, 0x59, source, State::ground, A::esc_dispatch);
        single(&result, 0x5A, source, State::ground, A::esc_dispatch);
        single(&result, 0x5C, source, State::ground, A::esc_dispatch);

        /* => escape_intermediate */
        range(&result, 0x20, 0x2F, source, State::escape_intermediate, A::collect);

        /* => sos_pm_apc_string */
        single(&result, 0x58, source, State::sos_pm_apc_string, A::none);
        single(&result, 0x5E, source, State::sos_pm_apc_string, A::none);
        single(&result, 0x5F, source, State::sos_pm_apc_string, A::none);

        /* => dcs_entry */
        single(&result, 0x50, source, State::dcs_entry, A::none);

        /* => csi_entry */
        single(&result, 0x5B, source, State::csi_entry, A::none);

        /* => osc_string */
        single(&result, 0x5D, source, State::osc_string, A::none);
    }

    /* dcs_entry */
    {
        const State source = State::dcs_entry;

        /* events */
        single(&result, 0x19, source, source, A::ignore);
        range(&result, 0, 0x17, source, source, A::ignore);
        range(&result, 0x1C, 0x1F, source, source, A::ignore);
        single(&result, 0x7F, source, source, A::ignore);

        /* => dcs_intermediate */
        range(&result, 0x20, 0x2F, source, State::dcs_intermediate, A::collect);

        /* => dcs_ignore */
        single(&result, 0x3A, source, State::dcs_ignore, A::none);

        /* => dcs_param */
        range(&result, 0x30, 0x39, source, State::dcs_param, A::param);
        single(&result, 0x3B, source, State::dcs_param, A::param);
        range(&result, 0x3C, 0x3F, source, State::dcs_param, A::collect);

        /* => dcs_passthrough */
        range(&result, 0x40, 0x7E, source, State::dcs_passthrough, A::none);
    }

    /* dcs_intermediate */
    {
        const State source = State::dcs_intermediate;

        /* events */
        single(&result, 0x19, source, source, A::ignore);
        range(&result, 0, 0x17, source, source, A::ignore);
        range(&result, 0x1C, 0x1F, source, source, A::ignore);
        range(&result, 0x20, 0x2F, source, source, A::collect);
        single(&result, 0x7F, source, source, A::ignore);

        /* => dcs_ignore */
        range(&result, 0x30, 0x3F, source, State::dcs_ignore, A::none);

        /* => dcs_passthrough */
        range(&result, 0x40, 0x7E, source, State::dcs_passthrough, A::none);
    }

    /* dcs_ignore */
    {
        const State source = State::dcs_ignore;

        /* events */
        single(&result, 0x19, source, source, A::ignore);
        range(&result, 0, 0x17, source, source, A::ignore);
        range(&result, 0x1C, 0x1F, source, source, A::ignore);

        /* High bytes are ignored payload data, overriding the
         * "anywhere" C1 transitions. See dcs_passthrough below for more.
         * In dcs_ignore the additional concern is that a UTF-8 payload in
         * an ignored DCS could otherwise begin a live sequence mid-string
         * (e.g. 0x9B => csi_entry). */
        range(&result, 0x80, 0xFF, source, source, A::ignore);
    }

    /* dcs_param */
    {
        const State source = State::dcs_param;

        /* events */
        single(&result, 0x19, source, source, A::ignore);
        range(&result, 0, 0x17, source, source, A::ignore);
        range(&result, 0x1C, 0x1F, source, source, A::ignore);
        range(&result, 0x30, 0x39, source, source, A::param);
        single(&result, 0x3B, source, source, A::param);
        single(&result, 0x7F, source, source, A::ignore);

        /* => dcs_ignore */
        single(&result, 0x3A, source, State::dcs_ignore, A::none);
        range(&result, 0x3C, 0x3F, source, State::dcs_ignore, A::none);

        /* => dcs_intermediate */
        range(&result, 0x20, 0x2F, source, State::dcs_intermediate, A::collect);

        /* => dcs_passthrough */
        range(&result, 0x40, 0x7E, source, State::dcs_passthrough, A::none);
    }

    /* dcs_passthrough */
    {
        const State source = State::dcs_passthrough;

        /* events */
        single(&result, 0x19, source, source, A::put);
        range(&result, 0, 0x17, source, source, A::put);
        range(&result, 0x1C, 0x1F, source, source, A::put);
        range(&result, 0x20, 0x7E, source, source, A::put);
        single(&result, 0x7F, source, source, A::ignore);

        /* High bytes are payload data, overriding the "anywhere" C1
         * transitions, matching how osc_string handles them below.
         * Ghostty is UTF-8 only, and DCS payloads carry UTF-8 text
         * (e.g. tmux control mode pane content): without this, a
         * UTF-8 continuation byte in the C1 range terminates or
         * corrupts the string and 0xA0-0xFF are silently dropped.
         *
         * This includes 0x9C (8-bit ST) on purpose: a raw 0x9C is
         * indistinguishable from a UTF-8 continuation byte ("Ü" is
         * 0xC3 0x9C), and Ghostty doesn't honor 8-bit C1 controls in
         * the ground state either (they go through UTF-8 decoding).
         * DCS strings terminate via 7-bit ST (ESC \) and abort via
         * CAN/SUB, which are unaffected here. */
        range(&result, 0x80, 0xFF, source, source, A::put);
    }

    /* csi_param */
    {
        const State source = State::csi_param;

        /* events */
        single(&result, 0x19, source, source, A::execute);
        range(&result, 0, 0x17, source, source, A::execute);
        range(&result, 0x1C, 0x1F, source, source, A::execute);
        range(&result, 0x30, 0x39, source, source, A::param);
        single(&result, 0x3A, source, source, A::param);
        single(&result, 0x3B, source, source, A::param);
        single(&result, 0x7F, source, source, A::ignore);

        /* => ground */
        range(&result, 0x40, 0x7E, source, State::ground, A::csi_dispatch);

        /* => csi_ignore */
        range(&result, 0x3C, 0x3F, source, State::csi_ignore, A::none);

        /* => csi_intermediate */
        range(&result, 0x20, 0x2F, source, State::csi_intermediate, A::collect);
    }

    /* csi_ignore */
    {
        const State source = State::csi_ignore;

        /* events */
        single(&result, 0x19, source, source, A::execute);
        range(&result, 0, 0x17, source, source, A::execute);
        range(&result, 0x1C, 0x1F, source, source, A::execute);
        range(&result, 0x20, 0x3F, source, source, A::ignore);
        single(&result, 0x7F, source, source, A::ignore);

        /* => ground */
        range(&result, 0x40, 0x7E, source, State::ground, A::none);
    }

    /* csi_intermediate */
    {
        const State source = State::csi_intermediate;

        /* events */
        single(&result, 0x19, source, source, A::execute);
        range(&result, 0, 0x17, source, source, A::execute);
        range(&result, 0x1C, 0x1F, source, source, A::execute);
        range(&result, 0x20, 0x2F, source, source, A::collect);
        single(&result, 0x7F, source, source, A::ignore);

        /* => ground */
        range(&result, 0x40, 0x7E, source, State::ground, A::csi_dispatch);

        /* => csi_ignore */
        range(&result, 0x30, 0x3F, source, State::csi_ignore, A::none);
    }

    /* csi_entry */
    {
        const State source = State::csi_entry;

        /* events */
        single(&result, 0x19, source, source, A::execute);
        range(&result, 0, 0x17, source, source, A::execute);
        range(&result, 0x1C, 0x1F, source, source, A::execute);
        single(&result, 0x7F, source, source, A::ignore);

        /* => ground */
        range(&result, 0x40, 0x7E, source, State::ground, A::csi_dispatch);

        /* => csi_ignore */
        single(&result, 0x3A, source, State::csi_ignore, A::none);

        /* => csi_intermediate */
        range(&result, 0x20, 0x2F, source, State::csi_intermediate, A::collect);

        /* => csi_param */
        range(&result, 0x30, 0x39, source, State::csi_param, A::param);
        single(&result, 0x3B, source, State::csi_param, A::param);
        range(&result, 0x3C, 0x3F, source, State::csi_param, A::collect);
    }

    /* osc_string */
    {
        const State source = State::osc_string;

        /* events */
        single(&result, 0x19, source, source, A::ignore);
        range(&result, 0, 0x06, source, source, A::ignore);
        range(&result, 0x08, 0x17, source, source, A::ignore);
        range(&result, 0x1C, 0x1F, source, source, A::ignore);
        range(&result, 0x20, 0xFF, source, source, A::osc_put);

        /* XTerm accepts either BEL  or ST  for terminating OSC
         * sequences, and when returning information, uses the same
         * terminator used in a query. */
        single(&result, 0x07, source, State::ground, A::none);
    }

    /* Create our immutable version */
    for (size_t i = 0; i < 256; i++) {
        for (size_t j = 0; j < STATE_COUNT; j++) {
            final_->t[i][j] = result.set[i][j]
                                  ? result.t[i][j]
                                  : transition((State)j, A::none);
        }
    }
}

/* The state transition table.
 *
 * Wisp: upstream generates this at compile time. Here the same genTable runs
 * once, on first use. */
inline const Table &table() {
    static Table t;
    static bool built = false;
    if (!built) {
        genTable(&t);
        built = true;
    }
    return t;
}

/* ─── the parser ─────────────────────────────────────────────────────────── */

struct Parser {
    /* Current state of the state machine */
    State state;

    /* Intermediate tracking. */
    uint8_t intermediates[MAX_INTERMEDIATE];
    uint8_t intermediates_idx;

    /* Param tracking, building */
    uint16_t params[MAX_PARAMS];
    SepList  params_sep;
    uint8_t  params_idx;
    uint16_t param_acc;
    uint8_t  param_acc_idx;

    /* Parser for OSC sequences */
    ::wisp::terminal::osc::Parser osc_parser;

    Parser()
        : state(State::ground), intermediates_idx(0), params_sep(),
          params_idx(0), param_acc(0), param_acc_idx(0), osc_parser() {
        memset(intermediates, 0, sizeof(intermediates));
        memset(params, 0, sizeof(params));
    }

    void clear() {
        intermediates_idx = 0;
        params_idx = 0;
        params_sep = SepList::initEmpty();
        param_acc = 0;
        param_acc_idx = 0;
    }

    void collect(uint8_t c) {
        if (intermediates_idx >= MAX_INTERMEDIATE) {
            /* log.warn("invalid intermediates count") */
            return;
        }

        intermediates[intermediates_idx] = c;
        intermediates_idx += 1;
    }

    /* Next consumes the next character c and returns the actions to execute.
     * Up to 3 actions may need to be executed -- in order -- representing
     * the state exit, transition, and entry actions. */
    Next next(uint8_t c);

private:
    bool doAction(TransitionAction action, uint8_t c, Action *out);
};

/* Wisp: *|= for u16. */
inline uint16_t sat_mul_u16(uint16_t a, uint16_t b) {
    const uint32_t r = (uint32_t)a * b;
    return r > 0xFFFF ? (uint16_t)0xFFFF : (uint16_t)r;
}

/* Wisp: +|= for u16. */
inline uint16_t sat_add_u16(uint16_t a, uint16_t b) {
    const uint32_t r = (uint32_t)a + b;
    return r > 0xFFFF ? (uint16_t)0xFFFF : (uint16_t)r;
}

inline bool Parser::doAction(TransitionAction action, uint8_t c, Action *out) {
    switch (action) {
        case TransitionAction::none:
        case TransitionAction::ignore:
            return false;

        case TransitionAction::print:
            out->tag = Action::Tag::print;
            out->print = c;
            return true;

        case TransitionAction::execute:
            out->tag = Action::Tag::execute;
            out->byte = c;
            return true;

        case TransitionAction::collect:
            collect(c);
            return false;

        case TransitionAction::param: {
            /* Semicolon separates parameters. If we encounter a semicolon
             * we need to store and move on to the next parameter. */
            if (c == ';' || c == ':') {
                /* Ignore too many parameters */
                if (params_idx >= MAX_PARAMS) return false;

                /* Set param final value */
                params[params_idx] = param_acc;
                if (c == ':') params_sep.set(params_idx);
                params_idx += 1;

                /* Reset current param value to 0 */
                param_acc = 0;
                param_acc_idx = 0;
                return false;
            }

            /* A numeric value. Add it to our accumulator. */
            param_acc = sat_mul_u16(param_acc, 10);
            param_acc = sat_add_u16(param_acc, (uint16_t)(c - '0'));

            /* Increment our accumulator index. If we overflow then
             * we're out of bounds and we exit immediately. */
            const uint8_t before = param_acc_idx;
            param_acc_idx = (uint8_t)(param_acc_idx + 1);
            const bool overflow = param_acc_idx < before;
            if (overflow) return false;

            /* The client is expected to perform no action. */
            return false;
        }

        case TransitionAction::osc_put:
            osc_parser.next(c);
            return false;

        case TransitionAction::csi_dispatch: {
            /* Ignore too many parameters */
            if (params_idx >= MAX_PARAMS) return false;

            /* Finalize parameters if we have one */
            if (param_acc_idx > 0) {
                params[params_idx] = param_acc;
                params_idx += 1;
            }

            out->tag = Action::Tag::csi_dispatch;
            out->csi_dispatch.intermediates = intermediates;
            out->csi_dispatch.intermediates_len = intermediates_idx;
            out->csi_dispatch.params = params;
            out->csi_dispatch.params_len = params_idx;
            out->csi_dispatch.params_sep = params_sep;
            out->csi_dispatch.final_ = c;

            /* We only allow colon or mixed separators for the 'm' command. */
            if (c != 'm' && params_sep.count() > 0) {
                /* warnCsiSepMismatch(result.csi_dispatch) */
                return false;
            }

            return true;
        }

        case TransitionAction::esc_dispatch:
            out->tag = Action::Tag::esc_dispatch;
            out->esc_dispatch.intermediates = intermediates;
            out->esc_dispatch.intermediates_len = intermediates_idx;
            out->esc_dispatch.final_ = c;
            return true;

        case TransitionAction::put:
            out->tag = Action::Tag::dcs_put;
            out->byte = c;
            return true;

        case TransitionAction::apc_put:
            out->tag = Action::Tag::apc_put;
            out->byte = c;
            return true;
    }
    return false;
}

inline Next Parser::next(uint8_t c) {
    const Transition effect = table().t[c][(size_t)state];

    const State next_state = effect.state;
    const TransitionAction action = effect.action;

    Next result;

    /* When going from one state to another, the actions take place in this
     * order:
     *
     * 1. exit action from old state
     * 2. transition action
     * 3. entry action to new state */

    /* Exit depends on current state */
    if (state != next_state) {
        switch (state) {
            case State::osc_string:
                if (const ::wisp::terminal::osc::Command *cmd =
                        osc_parser.end(c)) {
                    result.action[0].tag = Action::Tag::osc_dispatch;
                    result.action[0].osc_dispatch = *cmd;
                    result.present[0] = true;
                }
                break;
            case State::dcs_passthrough:
                result.action[0].tag = Action::Tag::dcs_unhook;
                result.present[0] = true;
                break;
            case State::sos_pm_apc_string:
                result.action[0].tag = Action::Tag::apc_end;
                result.present[0] = true;
                break;
            default:
                break;
        }
    }

    result.present[1] = doAction(action, c, &result.action[1]);

    /* Entry depends on new state */
    if (state != next_state) {
        switch (next_state) {
            case State::escape:
            case State::dcs_entry:
            case State::csi_entry:
                clear();
                break;

            case State::osc_string:
                osc_parser.reset();
                break;

            case State::dcs_passthrough: {
                /* Ignore too many parameters */
                if (params_idx >= MAX_PARAMS) break;
                /* Finalize parameters */
                if (param_acc_idx > 0) {
                    params[params_idx] = param_acc;
                    params_idx += 1;
                }
                result.action[2].tag = Action::Tag::dcs_hook;
                result.action[2].dcs_hook.intermediates = intermediates;
                result.action[2].dcs_hook.intermediates_len = intermediates_idx;
                result.action[2].dcs_hook.params = params;
                result.action[2].dcs_hook.params_len = params_idx;
                result.action[2].dcs_hook.final_ = c;
                result.present[2] = true;
                break;
            }

            case State::sos_pm_apc_string:
                result.action[2].tag = Action::Tag::apc_start;
                result.present[2] = true;
                break;

            default:
                break;
        }
    }

    /* After generating the actions, we set our next state. */
    state = next_state;
    return result;
}

} /* namespace parser */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_PARSER_HPP */
