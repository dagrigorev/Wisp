/* Transliterated from Ghostty src/terminal/ScreenSet.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * A ScreenSet holds multiple terminal screens. This is initially created
 * to handle simple primary vs alternate screens, but could be extended
 * in the future to handle N screens.
 *
 * One of the goals of this is to allow lazy initialization of screens
 * as needed. The primary screen is always initialized, but the alternate
 * screen may not be until first used.
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 * Wisp: std.EnumMap(Key, *Screen) is an array of nullable pointers.
 */

#pragma once
#ifndef WISP_VT_SCREEN_SET_HPP
#define WISP_VT_SCREEN_SET_HPP

#include <new>

#include "screen.hpp"

namespace wisp {
namespace vt {

struct ScreenSet {
    /* The possible keys for screens in the screen set. */
    enum class Key : uint8_t { primary, alternate };
    static const size_t key_count = 2;

    /* The key value of the currently active screen. Useful for simple
     * comparisons, e.g. "is this screen the primary screen". */
    Key active_key;

    /* The active screen pointer. */
    Screen *active;

    /* All screens that are initialized. */
    Screen *all[key_count];

    /* Monotonic generation counter for each screen key. This changes whenever
     * screen storage is removed or replaced so external handles can distinguish a
     * newly initialized screen from stale references into destroyed storage. */
    size_t generations[key_count];

    /* Wisp: false is OutOfMemory. */
    static bool init(zigstd::Allocator alloc, const Screen::Options &opts, ScreenSet *out) {
        /* We need to initialize our initial primary screen */
        Screen *screen = alloc.create<Screen>();
        if (!screen) return false;
        new (screen) Screen();
        if (!Screen::init(alloc, opts, screen)) {
            alloc.destroy(screen);
            return false;
        }
        out->active_key = Key::primary;
        out->active = screen;
        out->all[(size_t)Key::primary] = screen;
        out->all[(size_t)Key::alternate] = nullptr;
        out->generations[0] = 0;
        out->generations[1] = 0;
        return true;
    }

    void deinit(zigstd::Allocator alloc) {
        /* Destroy all initialized screens */
        for (size_t i = 0; i < key_count; i++) {
            if (!all[i]) continue;
            all[i]->deinit();
            alloc.destroy(all[i]);
        }
    }

    /* Get the screen for the given key, if it is initialized. */
    Screen *get(Key key) const { return all[(size_t)key]; }

    /* Get the current generation for the given screen key. */
    size_t generation(Key key) const { return generations[(size_t)key]; }

    /* Get the screen for the given key, initializing it if necessary.
     * Wisp: null is OutOfMemory. */
    Screen *getInit(zigstd::Allocator alloc, Key key, const Screen::Options &opts) {
        if (Screen *screen = get(key)) return screen;
        Screen *screen = alloc.create<Screen>();
        if (!screen) return nullptr;
        new (screen) Screen();
        if (!Screen::init(alloc, opts, screen)) {
            alloc.destroy(screen);
            return nullptr;
        }
        all[(size_t)key] = screen;
        return screen;
    }

    /* Remove a key from the set. The primary screen cannot be removed (asserted). */
    void remove(zigstd::Allocator alloc, Key key) {
        assert(key != Key::primary);
        if (Screen *screen = all[(size_t)key]) {
            all[(size_t)key] = nullptr;
            generations[(size_t)key] = generation(key) + 1;
            screen->deinit();
            alloc.destroy(screen);
        }
    }

    /* Switch the active screen to the given key. Requires that the
     * screen is initialized. */
    void switchTo(Key key) {
        active_key = key;
        active = all[(size_t)key];
        assert(active != nullptr);
    }
};

} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_SCREEN_SET_HPP */
