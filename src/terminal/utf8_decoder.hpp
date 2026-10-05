/* Transliterated from Ghostty src/terminal/UTF8Decoder.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * DFA-based non-allocating error-replacing UTF-8 decoder.
 *
 * This implementation is based largely on the excellent work of
 * Bjoern Hoehrmann, with slight modifications to support error-
 * replacement.
 *
 * For details on Bjoern's DFA-based UTF-8 decoder, see
 * http://bjoern.hoehrmann.de/utf-8/decoder/dfa (MIT licensed)
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 */

#pragma once
#ifndef WISP_TERMINAL_UTF8_DECODER_HPP
#define WISP_TERMINAL_UTF8_DECODER_HPP

#include <stdint.h>

namespace wisp {
namespace terminal {

struct UTF8Decoder {
    /* DFA states */
    static const uint8_t ACCEPT_STATE = 0;
    static const uint8_t REJECT_STATE = 12;

    /* This is where we accumulate our current codepoint. */
    uint32_t accumulator; /* u21 = 0 */
    /* The internal state of the DFA. */
    uint8_t state;        /* = ACCEPT_STATE */

    UTF8Decoder() : accumulator(0), state(ACCEPT_STATE) {}

    /* Wisp: struct { ?u21, bool } */
    struct Result {
        bool has_codepoint;
        uint32_t codepoint;
        bool consumed;
    };

    /* Takes the next byte in the utf-8 sequence and emits a tuple of
     * - The codepoint that was generated, if there is one.
     * - A boolean that indicates whether the provided byte was consumed.
     *
     * The only case where the byte is not consumed is if an ill-formed
     * sequence is reached, in which case a replacement character will be
     * emitted and the byte will not be consumed.
     *
     * If the byte is not consumed, the caller is responsible for calling
     * again with the same byte before continuing. */
    Result next(uint8_t byte) {
        static const uint8_t char_classes[256] = {
           0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
           0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
           0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
           0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
           1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,  9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
           7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,  7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
           8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2,  2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
          10,3,3,3,3,3,3,3,3,3,3,3,3,4,3,3, 11,6,6,6,5,8,8,8,8,8,8,8,8,8,8,8,
        };

        static const uint8_t transitions[] = {
           0,12,24,36,60,96,84,12,12,12,48,72, 12,12,12,12,12,12,12,12,12,12,12,12,
          12, 0,12,12,12,12,12, 0,12, 0,12,12, 12,24,12,12,12,12,12,24,12,24,12,12,
          12,12,12,12,12,12,12,24,12,12,12,12, 12,24,12,12,12,12,12,12,12,24,12,12,
          12,12,12,12,12,12,12,36,12,36,12,12, 12,36,12,12,12,12,12,36,12,36,12,12,
          12,36,12,12,12,12,12,12,12,12,12,12,
        };

        const uint8_t char_class = char_classes[byte];

        const uint8_t initial_state = state;

        if (state != ACCEPT_STATE) {
            accumulator = (accumulator << 6) & 0x1FFFFF; /* u21 */
            accumulator |= (uint32_t)(byte & 0x3F);
        } else {
            accumulator = ((uint32_t)0xFF >> char_class) & byte;
        }

        state = transitions[state + char_class];

        Result r;
        if (state == ACCEPT_STATE) {
            /* Emit the fully decoded codepoint. */
            r.has_codepoint = true;
            r.codepoint = accumulator;
            r.consumed = true;
            accumulator = 0;
            return r;
        } else if (state == REJECT_STATE) {
            accumulator = 0;
            state = ACCEPT_STATE;
            /* Emit a replacement character. If we rejected the first byte
             * in a sequence, then it was consumed, otherwise it was not. */
            r.has_codepoint = true;
            r.codepoint = 0xFFFD;
            r.consumed = initial_state == ACCEPT_STATE;
            return r;
        } else {
            /* Emit nothing, we're in the middle of a sequence. */
            r.has_codepoint = false;
            r.codepoint = 0;
            r.consumed = true;
            return r;
        }
    }
};

} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_UTF8_DECODER_HPP */
