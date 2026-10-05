/* Transliterated from Ghostty src/terminal/kitty/color.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: upstream's OSC struct lives here, but it holds an osc.Terminator,
 * and osc.hpp includes this file, so the struct is declared in osc.hpp as
 * Command::kitty_color_protocol. Its Request and list type are here.
 */

#pragma once
#ifndef WISP_TERMINAL_KITTY_COLOR_HPP
#define WISP_TERMINAL_KITTY_COLOR_HPP

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../color.hpp"

namespace wisp {
namespace terminal {
namespace kitty {
namespace color {

enum class Special : uint8_t {
    foreground,
    background,
    selection_foreground,
    selection_background,
    cursor,
    cursor_text,
    visual_bell,
    second_transparent_background,
};

/* Wisp: @tagName for Special. */
inline const char *special_name(Special s) {
    switch (s) {
        case Special::foreground: return "foreground";
        case Special::background: return "background";
        case Special::selection_foreground: return "selection_foreground";
        case Special::selection_background: return "selection_background";
        case Special::cursor: return "cursor";
        case Special::cursor_text: return "cursor_text";
        case Special::visual_bell: return "visual_bell";
        case Special::second_transparent_background:
            return "second_transparent_background";
    }
    return "";
}

static const size_t special_count = 8;

struct Kind {
    static const size_t max = 255 + special_count;

    enum class Tag : uint8_t { palette, special };
    Tag tag;
    uint8_t palette;
    Special special;

    Kind() : tag(Tag::palette), palette(0), special(Special::foreground) {}

    static Kind makePalette(uint8_t p) {
        Kind k;
        k.tag = Tag::palette;
        k.palette = p;
        return k;
    }
    static Kind makeSpecial(Special s) {
        Kind k;
        k.tag = Tag::special;
        k.special = s;
        return k;
    }

    bool eql(const Kind &o) const {
        if (tag != o.tag) return false;
        return tag == Tag::palette ? palette == o.palette : special == o.special;
    }

    /* Wisp: ?Kind is the bool return plus *out. */
    static bool parse(const char *key, size_t len, Kind *out) {
        /* std.meta.stringToEnum(Special, key) */
        for (size_t i = 0; i < special_count; i++) {
            const char *name = special_name((Special)i);
            if (strlen(name) == len && memcmp(name, key, len) == 0) {
                *out = makeSpecial((Special)i);
                return true;
            }
        }

        /* std.fmt.parseUnsigned(u8, key, 10). Wisp: Zig also accepts '_'
         * digit separators, which appear in none of upstream's tests and are
         * not reproduced. */
        if (len == 0) return false;
        unsigned v = 0;
        for (size_t i = 0; i < len; i++) {
            if (key[i] < '0' || key[i] > '9') return false;
            v = v * 10 + (unsigned)(key[i] - '0');
            if (v > 255) return false;
        }
        *out = makePalette((uint8_t)v);
        return true;
    }

    /* Returns true when a terminal has built-in state for this key.
     *
     * Unsupported special colors may still be valid Kitty protocol keys, but
     * libghostty-vt cannot report them because Terminal does not store them. */
    bool hasTerminalQueryColor() const {
        switch (tag) {
            case Tag::palette: return true;
            case Tag::special:
                switch (special) {
                    case Special::foreground:
                    case Special::background:
                    case Special::cursor:
                        return true;
                    default:
                        return false;
                }
        }
        return false;
    }

    /* Wisp: format(writer). Writes into buf (NUL-terminated when cap > 0)
     * and returns the length the formatted value needs. */
    size_t format(char *buf, size_t cap) const {
        char tmp[40];
        size_t n;
        if (tag == Tag::palette) {
            unsigned p = palette;
            char digits[4];
            size_t d = 0;
            do {
                digits[d++] = (char)('0' + p % 10);
                p /= 10;
            } while (p);
            for (n = 0; n < d; n++) tmp[n] = digits[d - 1 - n];
        } else {
            const char *name = special_name(special);
            n = strlen(name);
            memcpy(tmp, name, n);
        }
        if (cap > 0) {
            const size_t c = n < cap - 1 ? n : cap - 1;
            memcpy(buf, tmp, c);
            buf[c] = 0;
        }
        return n;
    }
};

struct Request {
    enum class Tag : uint8_t { query, set, reset };
    Tag tag;
    Kind query;
    struct {
        Kind key;
        ::wisp::terminal::RGB color;
    } set;
    Kind reset;

    Request() : tag(Tag::query), query(), set(), reset() {}
};

/* Wisp: std.ArrayList(Request). Growth follows Zig's growCapacity
 * (cap + cap/2 + init_capacity), which nothing observes. A plain struct,
 * shallow-copied like a Zig value; the owner calls deinit. */
struct RequestList {
    Request *items;
    size_t   len;
    size_t   capacity;

    RequestList() : items(nullptr), len(0), capacity(0) {}

    void deinit() {
        free(items);
        items = nullptr;
        len = 0;
        capacity = 0;
    }

    bool append(const Request &r) {
        if (len == capacity) {
            const size_t init_capacity =
                64 / sizeof(Request) > 1 ? 64 / sizeof(Request) : 1;
            size_t new_cap = capacity;
            new_cap += new_cap / 2 + init_capacity;
            Request *n = (Request *)realloc(items, new_cap * sizeof(Request));
            if (!n) return false;
            items = n;
            capacity = new_cap;
        }
        items[len++] = r;
        return true;
    }
};

} /* namespace color */
} /* namespace kitty */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_KITTY_COLOR_HPP */
