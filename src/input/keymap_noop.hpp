/* Transliterated from Ghostty src/input/KeymapNoop.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * A noop implementation of the keymap interface so that the embedded
 * library can compile on non-macOS platforms.
 *
 * Wisp: init/reload cannot fail here, so they return void; translate's
 * `[]u8` out buffer is a pointer plus length and is unused, as upstream.
 */
#pragma once
#ifndef WISP_INPUT_KEYMAP_NOOP_HPP
#define WISP_INPUT_KEYMAP_NOOP_HPP

#include <stddef.h>
#include <stdint.h>

#include "key_mods.hpp"

namespace wisp {
namespace input {

struct KeymapNoop {
    struct State {};
    struct Translation {
        const char *text = "";
        bool composing = false;
        Mods mods;
    };

    static KeymapNoop init() { return KeymapNoop(); }

    void deinit() const {}

    void reload() {}

    Translation translate(uint8_t *out, size_t out_len, State *state, uint16_t code, const Mods &mods) const {
        (void)out;
        (void)out_len;
        (void)state;
        (void)code;
        (void)mods;
        Translation t;
        t.text = "";
        t.composing = false;
        return t;
    }
};

} /* namespace input */
} /* namespace wisp */

#endif /* WISP_INPUT_KEYMAP_NOOP_HPP */
