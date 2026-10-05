/* Transliterated from Ghostty src/terminal/Tabstops.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Keep track of the location of tabstops.
 *
 * This is implemented as a bit set. There is a preallocation segment that
 * is used for almost all screen sizes. Then there is a dynamically allocated
 * segment if the screen is larger than the preallocation amount.
 *
 * In reality, tabstops don't need to be the most performant in any metric.
 * This implementation tries to balance denser memory usage (by using a bitset)
 * and minimizing unnecessary allocations.
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 */

#pragma once
#ifndef WISP_VT_TABSTOPS_HPP
#define WISP_VT_TABSTOPS_HPP

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../zigstd/allocator.hpp"
#include "tripwire.hpp"

namespace wisp {
namespace vt {

struct Tabstops {
    /* Unit is the type we use per tabstop unit (see file docs). */
    typedef uint8_t Unit;
    static const size_t unit_bits = 8;

    /* The number of columns we preallocate for. This is kind of high which
     * costs us some memory, but this is more columns than my 6k monitor at
     * 12-point font size, so this should prevent allocation in almost all
     * real world scenarios for the price of wasting at most
     * (columns / sizeOf(Unit)) bytes. */
    static const size_t prealloc_columns = 512;

    /* The number of entries we need for our preallocation. */
    static const size_t prealloc_count = prealloc_columns / unit_bits;

    /* We precompute all the possible masks since we never use a huge bit size. */
    static Unit masks(size_t i) { return (Unit)(1u << i); }

    /* The number of columns this tabstop is set to manage. Use resize()
     * to change this number. */
    size_t cols; /* = 0 */

    /* Preallocated tab stops. */
    Unit prealloc_stops[prealloc_count]; /* = @splat(0) */

    /* Dynamically expanded stops above prealloc stops. */
    Unit *dynamic_stops;       /* = &[0]Unit{} */
    size_t dynamic_stops_len; /* Wisp: slice length */

    Tabstops() : cols(0), dynamic_stops(nullptr), dynamic_stops_len(0) {
        memset(prealloc_stops, 0, sizeof(prealloc_stops));
    }

    /* Returns the entry in the stops array that would contain this column. */
    static size_t entry(size_t col) { return col / unit_bits; }

    static size_t index(size_t col) { return col % unit_bits; }

    enum class Error { none, OutOfMemory };

    static Error init(zigstd::Allocator alloc, size_t cols, size_t interval, Tabstops *out) {
        Tabstops res;
        const Error e = res.resize(alloc, cols);
        if (e != Error::none) return e;
        res.reset(interval);
        *out = res;
        return Error::none;
    }

    void deinit(zigstd::Allocator alloc) {
        if (dynamic_stops_len > 0) alloc.freeT(dynamic_stops, dynamic_stops_len);
        *this = Tabstops();
    }

    /* Set the tabstop at a certain column. The columns are 0-indexed. */
    void set(size_t col) {
        const size_t i = entry(col);
        const size_t idx = index(col);
        if (i < prealloc_count) {
            prealloc_stops[i] |= masks(idx);
            return;
        }

        const size_t dynamic_i = i - prealloc_count;
        assert(dynamic_i < dynamic_stops_len);
        dynamic_stops[dynamic_i] |= masks(idx);
    }

    /* Unset the tabstop at a certain column. The columns are 0-indexed. */
    void unset(size_t col) {
        const size_t i = entry(col);
        const size_t idx = index(col);
        if (i < prealloc_count) {
            prealloc_stops[i] &= (Unit)~masks(idx);
            return;
        }

        const size_t dynamic_i = i - prealloc_count;
        assert(dynamic_i < dynamic_stops_len);
        dynamic_stops[dynamic_i] &= (Unit)~masks(idx);
    }

    /* Get the value of a tabstop at a specific column. The columns are 0-indexed. */
    bool get(size_t col) const {
        const size_t i = entry(col);
        const size_t idx = index(col);
        const Unit mask = masks(idx);
        Unit unit;
        if (i < prealloc_count) {
            unit = prealloc_stops[i];
        } else {
            const size_t dynamic_i = i - prealloc_count;
            assert(dynamic_i < dynamic_stops_len);
            unit = dynamic_stops[dynamic_i];
        }

        return (unit & mask) == mask;
    }

    enum class ResizeTw { dynamic_alloc };
    typedef tripwire::Module<ResizeTw, Error, 1> resize_tw;

    /* Resize this to support up to cols columns.
     * TODO: needs interval to set new tabstops */
    Error resize(zigstd::Allocator alloc, size_t cols_) {
        typedef resize_tw tw;

        /* Do nothing if it fits. */
        if (cols_ <= prealloc_columns) {
            cols = cols_;
            return Error::none;
        }

        /* Number of units needed beyond the preallocated columns. */
        const size_t dynamic_count = (cols_ - prealloc_columns + unit_bits - 1) / unit_bits;
        if (dynamic_count <= dynamic_stops_len) {
            cols = cols_;
            return Error::none;
        }

        {
            const Error e = tw::check(ResizeTw::dynamic_alloc);
            if (e != Error::none) return e;
        }
        const size_t old_len = dynamic_stops_len;
        Unit *n = alloc.realloc(dynamic_stops, dynamic_stops_len, dynamic_count);
        if (!n) return Error::OutOfMemory;
        dynamic_stops = n;
        dynamic_stops_len = dynamic_count;
        memset(dynamic_stops + old_len, 0, dynamic_count - old_len);
        cols = cols_;
        return Error::none;
    }

    /* Return the maximum number of columns this can support currently. */
    size_t capacity() const { return (prealloc_count + dynamic_stops_len) * unit_bits; }

    /* Unset all tabstops and then reset the initial tabstops to the given
     * interval. An interval of 0 sets no tabstops. */
    void reset(size_t interval) {
        memset(prealloc_stops, 0, sizeof(prealloc_stops));
        if (dynamic_stops_len) memset(dynamic_stops, 0, dynamic_stops_len);

        if (interval == 0 || cols <= 1) return;

        for (size_t i = interval; i < cols - 1; i += interval) {
            set(i);
        }
    }
};

} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_TABSTOPS_HPP */
