/* Transliterated from Ghostty src/terminal/build_options.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * The optional feature gates of the terminal module, and the parser for
 * the token list that selects them.
 *
 * Wisp: this file is a PARTIAL port, and the omissions are structural
 * rather than a matter of effort. Upstream's `Options` is Zig build-script
 * code: it takes a `*std.Build` and a `*std.Build.Module`, calls
 * `b.addOptions()`, and emits each field with `opts.addOption` inside a
 * `comptime inline for` over the struct's fields, so that importing
 * `terminal_options` yields them. None of that has a C++ analogue -- CMake
 * carries that role here -- so the following are deliberately not ported,
 * on the same grounds as a GTK runtime or a Metal backend:
 *
 *   - `Options` itself, and its `add(b, m)`.
 *   - `Options.Artifact` (ghostty vs libghostty-vt). This port builds one
 *     artifact.
 *   - The `artifact`, `oniguruma`, `simd`, `slow_runtime_safety`, `c_abi`
 *     and `version` fields, and the synthesized `tmux_control_mode`
 *     option. The port spells its equivalents as compiler defines
 *     (`WISP_IS_TEST`, `WISP_SLOW_RUNTIME_SAFETY`) set by CMake.
 *   - `Options.kittyGraphics(target)`, which force-disables the feature on
 *     freestanding targets because they cannot read a clock. This port
 *     targets Windows only, where that cannot arise.
 *
 * What is ported is the part with behavior and a test: `Features`, its
 * `parse`, and `eqlName`. That matters beyond completeness, because the
 * port's `Wisp:` notes already cite `build_options.glyph_protocol`,
 * `build_options.tmux_control_mode` and the like (see apc.hpp and
 * dcs.hpp) without any such declaration existing. This gives those notes
 * a referent.
 *
 * Wisp: `kitty_graphics` is true here, as upstream defaults it. It was
 * false for this port until 2026-10-16; `glyph_protocol` remains off, and
 * `tmux_control_mode` is synthesized from `oniguruma` upstream rather
 * than being a Features field at all.
 *
 * Wisp, differences in shape rather than behavior:
 *   - `error{UnknownFeature}!Features` becomes a bool return (false on an
 *     unknown feature or a dangling sign) with the result as an out
 *     parameter. There is one error, so no error enum is needed.
 *   - Upstream is a `packed struct` of bools and its test builds "no
 *     features" by bitcasting a zero of the backing integer. A C++ `bool`
 *     is not a bit, so `none()` names that value instead, and equality is
 *     field-wise via the table below rather than by bytes -- comparing
 *     padding would be undefined.
 *   - `inline for` over the struct's fields becomes a loop over
 *     `fields()`, a static table of name and member pointer. Adding a
 *     feature means adding a field and a table row; they are adjacent so
 *     they stay in step.
 */
#pragma once
#ifndef WISP_VT_BUILD_OPTIONS_HPP
#define WISP_VT_BUILD_OPTIONS_HPP

#include <stddef.h>

namespace wisp {
namespace vt {
namespace build_options {

/* Optional features for lib artifacts. Disabling a feature removes its C
 * API exports and any stream-integrated handling from the build, primarily
 * so size-conscious embedders (e.g. wasm) can trim the binary. The Ghostty
 * application itself always builds with every feature enabled.
 *
 * Field names are the accepted tokens for `-Dvt-features`, which acts like
 * `-Dcpu`: comma-separated modifications applied on top of the default
 * (all enabled) set. `+feature` and `feature` enable, `-feature` disables.
 * The special name `all` refers to every feature, so
 * `-Dvt-features=-all,+render-state` builds only the render state API.
 * Hyphens in names are interchangeable with underscores. */
struct Features {
    /* Terminal state serialization: encode a full terminal (screen
     * contents, scrollback, cursor, styles, modes, etc.) to a compact
     * binary format and decode it back into a live terminal. Used for
     * session persistence, window restoration, and moving terminal state
     * between processes. */
    bool snapshot = true;

    /* Textual export of terminal contents as plain text, VT (replayable
     * escape sequences), or HTML, with optional extras such as cursor
     * position, palette, modes, and tabstops. Used for copy-to-clipboard,
     * dump-to-file, and debugging. */
    bool formatter = true;

    /* Text selection: select word/line/output/all, selection ordering,
     * adjustment, and containment math, and the click/drag gesture state
     * machine (single/double/triple click behaviors). */
    bool selection = true;

    /* Terminal search: find matches for a string in the active area and
     * scrollback (ASCII case-insensitive), with results that survive
     * primary/alternate screen switches, resize, and scrollback
     * pruning. */
    bool search = true;

    /* The render state API: a coherent, update-in-place view of the
     * visible screen (rows, cells, styles, cursor, colors, palette)
     * designed to drive a renderer at frame rates. */
    bool render_state = true;

    /* Encoding of host input events into the byte sequences a terminal
     * application expects: keyboard input (including the Kitty keyboard
     * protocol), mouse reporting, focus reporting, and (bracketed)
     * paste. */
    bool input_encode = true;

    /* Color utilities: luminance, perceived luminance, and contrast math,
     * color string parsing, the default 256-color palette and palette
     * generation from configurable bases, and the X11 color name table. */
    bool color = true;

    /* Direct inspection of grid contents at a position via grid
     * references, plus tracked grid references that stay valid across
     * terminal updates. Deliberately not built for render loops (use
     * `render_state` for that). */
    bool grid_introspection = true;

    /* The APC glyph protocol: a Ghostty extension that lets terminal
     * applications register custom font glyphs (glyf outlines) for
     * private-use-area codepoints. Disabled sequences are still consumed
     * and safely ignored. */
    bool glyph_protocol = true;

    /* The Kitty graphics protocol: APC command parsing, image
     * transmission and storage (PNG decoding via the sys interface), and
     * placement tracking, plus the read APIs a renderer uses to draw
     * placed images. Disabled sequences are still consumed and safely
     * ignored. */
    bool kitty_graphics = true;

    /* Wisp: stands in for `inline for` over the struct's fields. */
    struct Field {
        const char *name;
        bool Features::*member;
    };
    static const Field *fields(size_t *len) {
        static const Field table[] = {
            {"snapshot", &Features::snapshot},
            {"formatter", &Features::formatter},
            {"selection", &Features::selection},
            {"search", &Features::search},
            {"render_state", &Features::render_state},
            {"input_encode", &Features::input_encode},
            {"color", &Features::color},
            {"grid_introspection", &Features::grid_introspection},
            {"glyph_protocol", &Features::glyph_protocol},
            {"kitty_graphics", &Features::kitty_graphics},
        };
        *len = sizeof(table) / sizeof(table[0]);
        return table;
    }

    /* Wisp: upstream's test builds this by bitcasting a zero of the
     * packed struct's backing integer. */
    static Features none() {
        Features f;
        size_t n = 0;
        const Field *t = fields(&n);
        for (size_t i = 0; i < n; i++) f.*(t[i].member) = false;
        return f;
    }

    /* Wisp: field-wise, never by bytes; padding is undefined. */
    bool eql(const Features &other) const {
        size_t n = 0;
        const Field *t = fields(&n);
        for (size_t i = 0; i < n; i++) {
            if (this->*(t[i].member) != other.*(t[i].member)) return false;
        }
        return true;
    }

    /* Feature name equality where '-' in the input matches '_' in the
     * field name. */
    static bool eqlName(const char *input, size_t input_len, const char *name) {
        size_t name_len = 0;
        while (name[name_len] != 0) name_len++;
        if (input_len != name_len) return false;
        for (size_t i = 0; i < input_len; i++) {
            const char norm = (input[i] == '-') ? '_' : input[i];
            if (norm != name[i]) return false;
        }
        return true;
    }

    /* Wisp: returns false where upstream returns error.UnknownFeature. */
    static bool parse(const char *list, size_t list_len, Features *out) {
        /* Modifications apply on top of the default set. */
        Features result;

        /* Wisp: std.mem.splitAny(u8, list, ", ") splits on either
         * delimiter, so both are tested for here. The <= bound yields the
         * trailing empty token a split produces, which the length check
         * below skips exactly as upstream's `raw.len == 0` does. */
        size_t i = 0;
        while (i <= list_len) {
            size_t end = i;
            while (end < list_len && list[end] != ',' && list[end] != ' ') end++;

            const char *raw = list + i;
            const size_t raw_len = end - i;
            i = end + 1;

            if (raw_len == 0) continue;

            const char *name = raw;
            size_t name_len = raw_len;
            bool enable = true;
            if (name[0] == '+') {
                name = name + 1;
                name_len--;
            } else if (name[0] == '-') {
                enable = false;
                name = name + 1;
                name_len--;
            }

            /* A dangling sign with no name is malformed. */
            if (name_len == 0) return false;

            size_t n = 0;
            const Field *t = fields(&n);

            /* The special name "all" refers to every feature. */
            if (eqlName(name, name_len, "all")) {
                for (size_t f = 0; f < n; f++) result.*(t[f].member) = enable;
                continue;
            }

            bool found = false;
            for (size_t f = 0; f < n; f++) {
                if (eqlName(name, name_len, t[f].name)) {
                    result.*(t[f].member) = enable;
                    found = true;
                }
            }
            if (!found) return false;
        }

        *out = result;
        return true;
    }
};

} /* namespace build_options */
} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_BUILD_OPTIONS_HPP */
