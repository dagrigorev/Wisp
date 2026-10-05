/* Transliterated from Ghostty src/terminal/bitmap_allocator.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: Allocator.Error![]T is a bool return (false for OutOfMemory) and
 * a T** out; the slice length is the n the caller passed. free takes the
 * pointer and element count the slice had.
 */

#pragma once
#ifndef WISP_VT_BITMAP_ALLOCATOR_HPP
#define WISP_VT_BITMAP_ALLOCATOR_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "size.hpp"

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace wisp {
namespace vt {
namespace bitmap_allocator {

/* Wisp: @clz / @ctz / @popCount on u64, defined for 0 (64). */
inline unsigned clz64(uint64_t v) {
    if (v == 0) return 64;
#ifdef _MSC_VER
    unsigned long idx;
    _BitScanReverse64(&idx, v);
    return 63 - (unsigned)idx;
#else
    return (unsigned)__builtin_clzll(v);
#endif
}

inline unsigned ctz64(uint64_t v) {
    if (v == 0) return 64;
#ifdef _MSC_VER
    unsigned long idx;
    _BitScanForward64(&idx, v);
    return (unsigned)idx;
#else
    return (unsigned)__builtin_ctzll(v);
#endif
}

inline unsigned popcount64(uint64_t v) {
    unsigned n = 0;
    while (v) { v &= v - 1; n++; }
    return n;
}

inline size_t alignForward(size_t v, size_t a) { return (v + a - 1) / a * a; }

/* Find `n` sequential free chunks in the given bitmaps and return the index
 * of the first chunk. If no chunks are found, return `null`. This also updates
 * the bitmap to mark the chunks as used.
 *
 * Wisp: ?usize is the bool return plus *out. */
inline bool findFreeChunks(uint64_t *bitmaps, size_t bitmaps_len, size_t n, size_t *out) {
    /* NOTE: This is a naive implementation that just iterates through the
     * bitmaps. There is very likely a more efficient way to do this but
     * I'm not a bit twiddling expert. Perhaps even SIMD could be used here
     * but unsure. Contributor friendly: let's benchmark and improve this! */

    /* Large chunks require special handling. */
    if (n > 64) {
        size_t i = 0;
    search:
        while (i < bitmaps_len) {
            /* Number of chunks available at the end of this bitmap. */
            const size_t prefix = clz64(bitmaps[i]);

            /* If there are no chunks available at the end of this bitmap
             * then we can't start in it, so we'll try the next one. */
            if (prefix == 0) {
                i += 1;
                continue;
            }

            /* Starting position if we manage to find the span we need here. */
            const size_t start_bitmap = i;
            const size_t start_bit = 64 - prefix;

            /* The remaining number of sequential free chunks we need to find. */
            size_t rem = n - prefix;

            i += 1;
            while (rem > 64) {
                /* We ran out of bitmaps, there's no sufficiently large gap. */
                if (i >= bitmaps_len) return false;

                /* There's more than 64 remaining chunks and this bitmap has
                 * content in it, so we try starting again with this bitmap. */
                if (bitmaps[i] != 0) goto search;

                /* This bitmap is completely free, we can subtract 64 from
                 * our remaining number. */
                rem -= 64;
                i += 1;
            }

            /* Wisp: upstream indexes bitmaps[i] here without a bounds
             * check; past the end is a safety panic in Zig's checked
             * builds and undefined in release. No gap can end past the
             * last bitmap, so this is that case answered as not found. */
            if (i >= bitmaps_len) return false;

            /* If the number of available chunks at the start of this bitmap
             * is less than the remaining required, we have to try again. */
            if (ctz64(bitmaps[i]) < rem) continue;

            const size_t suffix = (n - prefix) % 64;

            /* Found! Mark everything between our start and end as used. */
            bitmaps[start_bitmap] |= (~(uint64_t)0 >> start_bit) << start_bit;
            const size_t full_bitmaps = (n - prefix - suffix) / 64;
            for (size_t k = 0; k < full_bitmaps; k++) {
                bitmaps[start_bitmap + 1 + k] = UINT64_MAX;
            }
            if (suffix > 0) bitmaps[i] |= ~(uint64_t)0 >> (64 - suffix);

            *out = start_bitmap * 64 + start_bit;
            return true;
        }

        return false;
    }

    /* assert(n <= @bitSizeOf(u64)) */
    for (size_t idx = 0; idx < bitmaps_len; idx++) {
        uint64_t *bitmap = &bitmaps[idx];
        /* Shift the bitmap to find `n` sequential free chunks.
         * EXAMPLE:
         * n = 4
         * shifted = 001111001011110010
         *         & 000111100101111001
         *         & 000011110010111100
         *         & 000001111001011110
         *         = 000001000000010000
         *                ^       ^
         * In this example there are 2 places with at least 4 sequential 1s.
         * Work on the inverted word so that free chunks are 1 bits. */
        const uint64_t free_ = ~*bitmap;
        uint64_t shifted = free_;
        for (size_t i = 1; i < n; i++) shifted &= free_ >> i;

        /* If we have zero then we have no matches */
        if (shifted == 0) continue;

        /* Trailing zeroes gets us the index of the first bit index with at
         * least `n` sequential 1s. In the example above, that would be `4`. */
        const unsigned bit = ctz64(shifted);

        /* Calculate the mask so we can mark it as used */
        const uint64_t mask = (UINT64_MAX >> (64 - n)) << bit;
        *bitmap |= mask;

        *out = (idx * 64) + bit;
        return true;
    }

    return false;
}

/* A relatively naive bitmap allocator that uses memory offsets against
 * a fixed backing buffer so that the backing buffer can be easily moved
 * without having to update pointers.
 *
 * The chunk size determines the size of each chunk in bytes. This is the
 * minimum distributed unit of memory. For example, if you request a
 * 1-byte allocation, you'll use a chunk of chunk_size bytes. Likewise,
 * if your chunk size is 4, and you request a 5-byte allocation, you'll
 * use 2 chunks.
 *
 * The allocator is susceptible to fragmentation. If you allocate and free
 * memory in a way that leaves small holes in the memory, you may not be
 * able to allocate large chunks of memory even if there is enough free
 * memory in aggregate. To avoid fragmentation, use a chunk size that is
 * large enough to cover most of your allocations.
 *
 * Notes for contributors: this is highly contributor friendly part of
 * the code. If you can improve this, add tests, show benchmarks, then
 * please do so! */
template <size_t chunk_size>
struct BitmapAllocator {
    static_assert((chunk_size & (chunk_size - 1)) == 0 && chunk_size > 0,
                  "chunk_size must be a power of two");

    static const size_t base_align = alignof(uint64_t);
    static const size_t bitmap_bit_size = 64;

    /* The bitmap of available chunks. Each bit represents a chunk. A
     * 0 means the chunk is free and a 1 means it's used, so an
     * all-zero bitmap is a fully free allocator. */
    size::Offset<uint64_t> bitmap;
    size_t bitmap_count;

    /* Lowest bitmap word index that may contain a free bit; words
     * below it are fully allocated, so alloc scans can start here
     * instead of at zero. */
    size_t search_start; /* = 0 */

    /* The contiguous buffer of chunks. */
    size::Offset<uint8_t> chunks;

    struct Layout {
        size_t total_size;
        size_t bitmap_count;
        size_t bitmap_start;
        size_t chunks_start;
    };

    /* Initialize the allocator map with a given buf and memory layout. */
    static BitmapAllocator init(size::OffsetBuf buf, Layout l) {
        /* assert(base_align.check(@intFromPtr(buf.start()))) */

        /* Clear our bitmaps to note that all chunks are free. */
        const size::Offset<uint64_t> bm = buf.member<uint64_t>(l.bitmap_start);
        memset(bm.ptr(buf), 0, l.bitmap_count * sizeof(uint64_t));

        return initAssumeZeroed(buf, l);
    }

    /* Initialize the allocator map over memory that the caller
     * guarantees is already zero-filled.
     *
     * This writes nothing to the buffer: an all-zero bitmap already
     * marks every chunk as free. Behavior is undefined if the bitmap
     * region is not zero. */
    static BitmapAllocator initAssumeZeroed(size::OffsetBuf buf, Layout l) {
        /* assert(base_align.check(@intFromPtr(buf.start()))) */
        BitmapAllocator a;
        a.bitmap = buf.member<uint64_t>(l.bitmap_start);
        a.bitmap_count = l.bitmap_count;
        a.search_start = 0;
        a.chunks = buf.member<uint8_t>(l.chunks_start);
        return a;
    }

    /* Returns the number of bytes required to allocate n elements of
     * type T. This accounts for the chunk size alignment used by the
     * bitmap allocator. */
    template <typename T>
    static size_t bytesRequired(size_t n) {
        const size_t byte_count = sizeof(T) * n;
        return alignForward(byte_count, chunk_size);
    }

    /* Allocate n elements of type T. This will return error.OutOfMemory
     * if there isn't enough space in the backing buffer.
     *
     * Use (size.zig).getOffset to get the base offset from the backing
     * memory for portable storage. */
    template <typename T, typename B>
    bool alloc(const B &base, size_t n, T **out) {
        /* note: we don't handle alignment yet, we just require that all
         * types are properly aligned. This is a limitation that should be
         * fixed but we haven't needed it. Contributor friendly: add tests
         * and fix this. */
        static_assert(chunk_size % alignof(T) == 0, "chunk alignment");
        /* assert(n > 0) */

        /* std.math.mul(usize, @sizeOf(T), n) catch return error.OutOfMemory */
        if (n > SIZE_MAX / sizeof(T)) return false;
        const size_t byte_count = sizeof(T) * n;
        const size_t chunk_count = (byte_count + chunk_size - 1) / chunk_size;

        /* Find the index of the free chunk. This also marks it as used.
         * Words below search_start have no free bits, so no free span
         * can start in or cross them and it is safe to skip them. */
        uint64_t *bitmaps = bitmap.ptr(base);
        const size_t start = search_start < bitmap_count ? search_start : bitmap_count;
        size_t rel;
        if (!findFreeChunks(bitmaps + start, bitmap_count - start, chunk_count, &rel)) return false;
        const size_t idx = start * bitmap_bit_size + rel;

        /* Advance past any words the allocation just filled so the
         * next scan starts at the first word with a free bit. */
        size_t new_start = start;
        while (new_start < bitmap_count && bitmaps[new_start] == UINT64_MAX) {
            new_start += 1;
        }
        search_start = new_start;

        uint8_t *c = chunks.ptr(base);
        *out = (T *)(void *)&c[idx * chunk_size];
        return true;
    }

    template <typename T, typename B>
    void free(const B &base, const T *slice_ptr, size_t slice_len) {
        /* Convert the slice of whatever type to a slice of bytes. We
         * can then use the byte len and chunk size to determine the
         * number of chunks that were allocated. */
        const size_t bytes_len = slice_len * sizeof(T);
        const size_t aligned_len = alignForward(bytes_len, chunk_size);
        const size_t chunk_count = aligned_len / chunk_size;

        /* From the pointer, we can calculate the exact index. */
        const uint8_t *c = chunks.ptr(base);
        const size_t chunk_idx = ((uintptr_t)slice_ptr - (uintptr_t)c) / chunk_size;

        /* The freed word gains free bits, so scans must not skip it. */
        const size_t word = chunk_idx / bitmap_bit_size;
        if (word < search_start) search_start = word;

        uint64_t *bitmaps = bitmap.ptr(base);

        /* Current bitmap index. */
        size_t i = chunk_idx / 64;
        /* Number of chunks we still have to mark as free. */
        size_t rem = chunk_count;

        /* Mark any bits in the starting bitmap that need to be marked. */
        {
            /* Bit index. */
            const size_t bit = chunk_idx % 64;
            /* Number of bits we need to mark in this bitmap. */
            const size_t bits = rem < 64 - bit ? rem : 64 - bit;

            bitmaps[i] &= ~((~(uint64_t)0 >> (64 - bits)) << bit);
            rem -= bits;
        }

        /* Mark any full bitmaps worth of bits that need to be marked. */
        i += 1;
        while (rem > 64) {
            bitmaps[i] = 0;
            rem -= 64;
            i += 1;
        }

        /* Mark any bits at the start of this last bitmap if it needs it. */
        if (rem > 0) {
            bitmaps[i] &= ~(~(uint64_t)0 >> (64 - rem));
        }
    }

    /* Returns the total capacity in bytes. */
    size_t capacityBytes() const { return bitmap_count * bitmap_bit_size * chunk_size; }

    /* Returns the number of bytes currently in use. */
    template <typename B>
    size_t usedBytes(const B &base) const {
        const uint64_t *bitmaps = bitmap.ptr(base);
        size_t used_chunks = 0;
        for (size_t i = 0; i < bitmap_count; i++) used_chunks += popcount64(bitmaps[i]);
        return used_chunks * chunk_size;
    }

    /* For testing only. */
    template <typename T, typename B>
    bool isAllocated(const B &base, const T *slice_ptr, size_t slice_len) const {
        const size_t bytes_len = slice_len * sizeof(T);
        const size_t aligned_len = alignForward(bytes_len, chunk_size);
        const size_t chunk_count = aligned_len / chunk_size;

        const uint8_t *c = chunks.ptr(base);
        const size_t chunk_idx = ((uintptr_t)slice_ptr - (uintptr_t)c) / chunk_size;

        const uint64_t *bitmaps = bitmap.ptr(base);

        for (size_t i = chunk_idx; i < chunk_idx + chunk_count; i++) {
            const size_t bm = i / bitmap_bit_size;
            const size_t bit = i % bitmap_bit_size;
            if ((bitmaps[bm] & ((uint64_t)1 << bit)) == 0) return false;
        }

        return true;
    }

    /* Get the layout for the given capacity. The capacity is in
     * number of bytes, not chunks. The capacity will likely be
     * rounded up to the nearest chunk size and bitmap size so
     * everything is perfectly divisible. */
    static Layout layout(size_t cap) {
        /* Align the cap forward to our chunk size so we always have
         * a full chunk at the end. */
        const size_t aligned_cap = alignForward(cap, chunk_size);

        /* Calculate the number of bitmaps. We need 1 bitmap per 64 chunks.
         * We align the chunk count forward so our bitmaps are full so we
         * don't have to handle the case where we have a partial bitmap. */
        const size_t chunk_count = aligned_cap / chunk_size;
        const size_t aligned_chunk_count = alignForward(chunk_count, 64);
        const size_t bitmap_count = aligned_chunk_count / 64;

        const size_t bitmap_start = 0;
        const size_t bitmap_end = sizeof(uint64_t) * bitmap_count;
        const size_t chunks_start = alignForward(bitmap_end, alignof(uint8_t));

        /* The chunks region must be exactly the bytes addressable by
         * the bitmaps: one chunk per bit of every bitmap. Anything more
         * is unreachable waste, while anything less would let alloc
         * hand out memory beyond the region, since init marks every
         * bitmap bit as free. */
        const size_t chunks_end = chunks_start + (aligned_chunk_count * chunk_size);
        const size_t total_size = chunks_end;

        Layout l;
        l.total_size = total_size;
        l.bitmap_count = bitmap_count;
        l.bitmap_start = bitmap_start;
        l.chunks_start = chunks_start;
        return l;
    }
};

} /* namespace bitmap_allocator */
} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_BITMAP_ALLOCATOR_HPP */
