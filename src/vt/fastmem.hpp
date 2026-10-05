/* Transliterated from Ghostty src/fastmem.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 * Wisp: libc is always linked, so move/copy are memmove/memcpy. Slices are
 * pointer plus length.
 */

#pragma once
#ifndef WISP_VT_FASTMEM_HPP
#define WISP_VT_FASTMEM_HPP

#include <stddef.h>
#include <string.h>

namespace wisp {
namespace fastmem {

/* Same as @memmove but prefers libc memmove if it is
 * available because it is generally much faster?. */
template <typename T>
inline void move(T *dest, const T *source, size_t len) {
    memmove(dest, source, len * sizeof(T));
}

/* Same as @memcpy but prefers libc memcpy if it is available
 * because it is generally much faster. */
template <typename T>
inline void copy(T *dest, const T *source, size_t len) {
    memcpy(dest, source, len * sizeof(T));
}

/* Moves the first item to the end.
 * For the reverse of this, use `fastmem.rotateOnceR`.
 *
 * Same as std.mem.rotate(T, items, 1) but more efficient by using memmove
 * and a tmp var for the single rotated item instead of 3 calls to reverse.
 *
 * e.g. `0 1 2 3` -> `1 2 3 0`. */
template <typename T>
inline void rotateOnce(T *items, size_t len) {
    const T tmp = items[0];
    move(items, items + 1, len - 1);
    items[len - 1] = tmp;
}

/* Moves the last item to the start.
 * Reverse operation of `fastmem.rotateOnce`.
 *
 * e.g. `0 1 2 3` -> `3 0 1 2`. */
template <typename T>
inline void rotateOnceR(T *items, size_t len) {
    const T tmp = items[len - 1];
    move(items + 1, items, len - 1);
    items[0] = tmp;
}

/* Rotates a new item in to the end of a slice.
 * The first item from the slice is removed and returned.
 *
 * e.g. rotating `4` in to `0 1 2 3` makes it `1 2 3 4` and returns `0`.
 *
 * For the reverse of this, use `fastmem.rotateInR`. */
template <typename T>
inline T rotateIn(T *items, size_t len, T item) {
    const T removed = items[0];
    move(items, items + 1, len - 1);
    items[len - 1] = item;
    return removed;
}

/* Rotates a new item in to the start of a slice.
 * The last item from the slice is removed and returned.
 *
 * e.g. rotating `4` in to `0 1 2 3` makes it `4 0 1 2` and returns `3`.
 *
 * Reverse operation of `fastmem.rotateIn`. */
template <typename T>
inline T rotateInR(T *items, size_t len, T item) {
    const T removed = items[len - 1];
    move(items + 1, items, len - 1);
    items[0] = item;
    return removed;
}

} /* namespace fastmem */
} /* namespace wisp */

#endif /* WISP_VT_FASTMEM_HPP */
