/* Transliterated from Ghostty src/terminal/StringMap.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: everything in this file behind upstream's `build_options.oniguruma`
 * is omitted, because Wisp does not link Oniguruma and so that flag is
 * false. Under that configuration upstream's own `searchIterator` is
 * literally `void` and all three of this file's tests begin with
 *
 *     if (comptime !build_options.oniguruma) return error.SkipZigTest;
 *
 * so there is nothing to transliterate and no test to port. Omitted with it:
 * `oni_search_retry_limit`, `searchIteratorOni`, `SearchIterator` and
 * `Match`. Substituting another regex engine would not be a transliteration,
 * because the retry-limit behaviour (`error.RetryLimitInSearch` and
 * `MatchStackLimitOver` yielding no match) is Oniguruma-specific.
 *
 * Wisp: `Screen.selectionStringMap`, which is what constructs a StringMap,
 * lives in Screen.zig upstream and is ported there — see
 * `Screen::selectionStringMap` in formatter.hpp, beside `selectionString`.
 */

#pragma once
#ifndef WISP_VT_STRING_MAP_HPP
#define WISP_VT_STRING_MAP_HPP

#include <stddef.h>

#include <string>

#include "formatter.hpp"

namespace wisp {
namespace vt {

/* A string along with the mapping of each individual byte in the string
 * to the point in the screen. */
struct StringMap {
    /* Wisp: upstream's `[:0]const u8` is allocator-owned; the port's
     * formatters write into a std::string, which owns its own storage. */
    std::string string;

    /* Mapping of string byte offsets to pins. See PinMap for the
     * storage details. */
    formatter::PinMap::Map map;

    /* Wisp: upstream frees `string` and the map through the allocator. Both
     * own their storage here, so this only drops it early; it exists so call
     * sites read like upstream's `defer map.deinit(alloc)`. */
    void deinit() {
        string.clear();
        string.shrink_to_fit();
        map.points.clear();
        map.points.shrink_to_fit();
        map.nodes.clear();
        map.nodes.shrink_to_fit();
    }
};

/* ====================================================================== */
/* Screen.zig functions that construct a StringMap                        */

/* Wisp: Screen.zig's selectionStringMap. Upstream routes both this and
 * selectionString through selectionStringImpl, whose only difference is the
 * optional pin map handed to the formatter; selectionString is in
 * formatter.hpp, and this is the `pins` arm of the same function. */
inline StringMap Screen::selectionStringMap(const SelectionString &opts) {
    StringMap result;

    /* Create a formatter and use that to emit our text. */
    formatter::Options fopts(formatter::Format::plain, true, opts.trim);
    formatter::ScreenFormatter f = formatter::ScreenFormatter::init(this, fopts);
    f.content.tag = formatter::ScreenFormatter::Content::Tag::selection;
    f.content.selection = opts.sel;

    formatter::PinMap pm;
    pm.map = &result.map;
    f.pin_map = Maybe<formatter::PinMap>(pm);

    /* Emit. */
    f.format(&result.string);
    return result;
}

}  // namespace vt
}  // namespace wisp

#endif  // WISP_VT_STRING_MAP_HPP
