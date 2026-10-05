/* Ported from Ghostty src/terminal/bitmap_allocator.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * A naive bitmap allocator addressing memory by offset against a fixed
 * backing buffer, so the buffer can be moved without fixing up pointers.
 *
 * chunk_size is the minimum distributed unit. A 1-byte request consumes a
 * whole chunk; a 5-byte request with a 4-byte chunk consumes two.
 *
 * This is susceptible to fragmentation: allocating and freeing in a pattern
 * that leaves small holes can make a large request fail even when enough
 * memory is free in aggregate. Choose a chunk size large enough to cover
 * most allocations.
 *
 * Porting notes:
 *   - Zig's @clz/@ctz are defined to return 64 for a zero input. The x86
 *     bsr/bsf instructions leave their destination undefined in that case,
 *     so clz64/ctz64 below special-case zero rather than calling the
 *     intrinsic blindly.
 *   - Zig returns error.OutOfMemory; this returns nullptr.
 *   - Zig passes slices, which carry their length. The C++ signatures take
 *     an explicit element count instead.
 */

#pragma once
#ifndef WISP_TERMINAL_BITMAP_ALLOCATOR_HPP
#define WISP_TERMINAL_BITMAP_ALLOCATOR_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "size.hpp"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace wisp {
namespace terminal {

/* ─── bit helpers ────────────────────────────────────────────────────────── */

/* Count leading zeros, returning 64 for a zero input to match Zig's @clz. */
inline unsigned clz64(uint64_t v) {
    if (v == 0) return 64;
#if defined(_MSC_VER) && defined(_M_X64)
    unsigned long idx;
    _BitScanReverse64(&idx, v);
    return 63u - (unsigned)idx;
#elif defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_clzll(v);
#else
    unsigned n = 0;
    for (int b = 63; b >= 0; --b) {
        if (v & (1ULL << b)) break;
        n++;
    }
    return n;
#endif
}

/* Count trailing zeros, returning 64 for a zero input to match Zig's @ctz. */
inline unsigned ctz64(uint64_t v) {
    if (v == 0) return 64;
#if defined(_MSC_VER) && defined(_M_X64)
    unsigned long idx;
    _BitScanForward64(&idx, v);
    return (unsigned)idx;
#elif defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_ctzll(v);
#else
    unsigned n = 0;
    while (((v >> n) & 1) == 0) n++;
    return n;
#endif
}

/* Population count. Deliberately not __popcnt64: that needs the POPCNT CPU
 * feature, which we do not require elsewhere. */
inline unsigned popcount64(uint64_t v) {
    v = v - ((v >> 1) & 0x5555555555555555ULL);
    v = (v & 0x3333333333333333ULL) + ((v >> 2) & 0x3333333333333333ULL);
    v = (v + (v >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (unsigned)((v * 0x0101010101010101ULL) >> 56);
}

/* align_forward now lives in size.hpp, beside the other layout helpers. */

/* A full 64-bit mask of `n` ones. n must be 1..64; n==0 would shift by 64,
 * which is undefined in C++. */
inline uint64_t ones(unsigned n) {
    assert(n >= 1 && n <= 64);
    return ~(uint64_t)0 >> (64 - n);
}

constexpr size_t BITMAP_NOT_FOUND = (size_t)-1;
constexpr unsigned BITMAP_BIT_SIZE = 64;

/* ─── findFreeChunks ─────────────────────────────────────────────────────── */

/* Find `n` sequential free chunks and return the index of the first, marking
 * them used. Returns BITMAP_NOT_FOUND if there is no such run.
 *
 * A 0 bit is free and a 1 bit is used, so an all-zero bitmap is fully free. */
inline size_t find_free_chunks(uint64_t *bitmaps, size_t len, size_t n) {
    /* Runs longer than one word have to be stitched across words. */
    if (n > BITMAP_BIT_SIZE) {
        size_t i = 0;
        while (i < len) {
            /* Free chunks at the top of this word — the run must start there
             * if it starts in this word at all. */
            const unsigned prefix = clz64(bitmaps[i]);
            if (prefix == 0) { i++; continue; }

            const size_t   start_bitmap = i;
            const unsigned start_bit = 64u - prefix;
            size_t rem = n - prefix;

            i++;
            bool restart = false;
            while (rem > 64) {
                /* Ran out of words: no gap this large exists. */
                if (i >= len) return BITMAP_NOT_FOUND;

                /* More than a word still needed but this one is not empty,
                 * so restart the search from this word. */
                if (bitmaps[i] != 0) { restart = true; break; }

                rem -= 64;
                i++;
            }
            if (restart) continue;

            /* Guard added in the port. The run can extend one word past the
             * last when the prefix starts in the final word; Zig catches that
             * with bounds checking, C++ would read out of bounds. */
            if (i >= len) return BITMAP_NOT_FOUND;

            /* Not enough free chunks at the start of the closing word. */
            if (ctz64(bitmaps[i]) < rem) continue;

            const unsigned suffix = (unsigned)((n - prefix) % 64);

            /* Found it — mark the whole span used. */
            bitmaps[start_bitmap] |= ones(prefix) << start_bit;

            const size_t full = (n - prefix - suffix) / 64;
            for (size_t k = 0; k < full; k++) bitmaps[start_bitmap + 1 + k] = ~(uint64_t)0;

            if (suffix > 0) bitmaps[i] |= ones(suffix);

            return start_bitmap * 64 + start_bit;
        }
        return BITMAP_NOT_FOUND;
    }

    assert(n >= 1 && n <= BITMAP_BIT_SIZE);

    for (size_t idx = 0; idx < len; idx++) {
        /* Work on the inverted word so free chunks are 1 bits, then AND the
         * word with successive right shifts of itself. A bit survives only if
         * it began a run of at least n ones:
         *
         *   n = 4
         *   shifted = 001111001011110010
         *           & 000111100101111001
         *           & 000011110010111100
         *           & 000001111001011110
         *           = 000001000000010000
         *                  ^       ^
         */
        const uint64_t free_bits = ~bitmaps[idx];
        uint64_t shifted = free_bits;
        for (size_t s = 1; s < n; s++) shifted &= free_bits >> s;

        if (shifted == 0) continue;

        /* The lowest surviving bit is where the first long-enough run starts. */
        const unsigned bit = ctz64(shifted);

        bitmaps[idx] |= ones((unsigned)n) << bit;
        return idx * 64 + bit;
    }

    return BITMAP_NOT_FOUND;
}

/* ─── BitmapAllocator ────────────────────────────────────────────────────── */

template <size_t chunk_size>
struct BitmapAllocator {
    static_assert(chunk_size != 0 && (chunk_size & (chunk_size - 1)) == 0,
                  "chunk_size must be a power of two");

    static const size_t base_align = alignof(uint64_t);

    struct Layout {
        size_t total_size;
        size_t bitmap_count;
        size_t bitmap_start;
        size_t chunks_start;
    };

    /* The bitmap of available chunks; one bit per chunk, 0 = free. */
    Offset<uint64_t> bitmap;
    size_t bitmap_count;

    /* Lowest word index that may hold a free bit. Words below are fully
     * allocated, so a scan can start here instead of at zero. */
    size_t search_start;

    /* The contiguous buffer of chunks. */
    Offset<uint8_t> chunks;

    /* Initialize over a buffer, clearing the bitmap so every chunk is free. */
    static BitmapAllocator init(OffsetBuf buf, const Layout &l) {
        assert(reinterpret_cast<uintptr_t>(buf.start()) % base_align == 0);

        Offset<uint64_t> bm = buf.member<uint64_t>(l.bitmap_start);
        memset(bm.ptr(buf), 0, l.bitmap_count * sizeof(uint64_t));

        return init_assume_zeroed(buf, l);
    }

    /* Initialize over memory the caller guarantees is already zeroed.
     *
     * This writes nothing: an all-zero bitmap already marks every chunk free.
     * Behavior is undefined if the bitmap region is not zero. */
    static BitmapAllocator init_assume_zeroed(OffsetBuf buf, const Layout &l) {
        assert(reinterpret_cast<uintptr_t>(buf.start()) % base_align == 0);

        BitmapAllocator self;
        self.bitmap = buf.member<uint64_t>(l.bitmap_start);
        self.bitmap_count = l.bitmap_count;
        self.search_start = 0;
        self.chunks = buf.member<uint8_t>(l.chunks_start);
        return self;
    }

    /* Bytes needed to allocate n elements of T, accounting for chunk
     * alignment. */
    template <typename T>
    static size_t bytes_required(size_t n) {
        return align_forward(sizeof(T) * n, chunk_size);
    }

    /* Allocate n elements of T, or nullptr if the buffer has no room.
     *
     * Use get_offset() to turn the result into a base-relative offset for
     * portable storage. */
    template <typename T, typename Base>
    T *alloc(Base base, size_t n) {
        /* Alignment is not handled: every type is required to be naturally
         * aligned within a chunk. Upstream notes this as a known limitation. */
        assert(chunk_size % alignof(T) == 0);
        assert(n > 0);

        /* Overflow in the byte count means the request cannot fit. */
        if (n > (size_t)-1 / sizeof(T)) return nullptr;
        const size_t byte_count = sizeof(T) * n;
        const size_t chunk_count = (byte_count + chunk_size - 1) / chunk_size;

        /* Words below search_start have no free bits, so no run can start in
         * or cross them and skipping them is safe. */
        uint64_t *bitmaps = bitmap.ptr(base);
        const size_t start = search_start < bitmap_count ? search_start : bitmap_count;

        const size_t rel = find_free_chunks(bitmaps + start, bitmap_count - start, chunk_count);
        if (rel == BITMAP_NOT_FOUND) return nullptr;

        const size_t idx = start * BITMAP_BIT_SIZE + rel;

        /* Advance past whatever this allocation filled, so the next scan
         * starts at the first word that still has a free bit. */
        {
            size_t ns = start;
            while (ns < bitmap_count && bitmaps[ns] == ~(uint64_t)0) ns++;
            search_start = ns;
        }

        uint8_t *chunk_base = chunks.ptr(base);
        return reinterpret_cast<T *>(&chunk_base[idx * chunk_size]);
    }

    /* Free a previous allocation of n elements of T. */
    template <typename T, typename Base>
    void free(Base base, T *ptr, size_t n) {
        const size_t aligned_len = align_forward(sizeof(T) * n, chunk_size);
        const size_t chunk_count = aligned_len / chunk_size;

        uint8_t *chunk_base = chunks.ptr(base);
        const size_t chunk_idx =
            (reinterpret_cast<uintptr_t>(ptr) - reinterpret_cast<uintptr_t>(chunk_base)) / chunk_size;

        /* This word regains free bits, so scans must not skip it any more. */
        const size_t word = chunk_idx / BITMAP_BIT_SIZE;
        if (word < search_start) search_start = word;

        uint64_t *bitmaps = bitmap.ptr(base);

        size_t i = word;
        size_t rem = chunk_count;

        /* Clear the tail of the first word. */
        {
            const unsigned bit = (unsigned)(chunk_idx % 64);
            const unsigned bits = (unsigned)(rem < (size_t)(64 - bit) ? rem : (size_t)(64 - bit));
            bitmaps[i] &= ~(ones(bits) << bit);
            rem -= bits;
        }

        /* Then whole words. */
        i++;
        while (rem > 64) {
            bitmaps[i] = 0;
            rem -= 64;
            i++;
        }

        /* Then the head of the final word. */
        if (rem > 0) bitmaps[i] &= ~ones((unsigned)rem);
    }

    /* Total capacity in bytes. */
    size_t capacity_bytes() const {
        return bitmap_count * BITMAP_BIT_SIZE * chunk_size;
    }

    /* Bytes currently in use. */
    template <typename Base>
    size_t used_bytes(Base base) const {
        const uint64_t *bitmaps = bitmap.ptr(base);
        size_t used_chunks = 0;
        for (size_t i = 0; i < bitmap_count; i++) used_chunks += popcount64(bitmaps[i]);
        return used_chunks * chunk_size;
    }

    /* Whether every chunk backing this allocation is marked used. Upstream
     * restricts this to test builds; it is kept available here because the
     * tests are a separate translation unit. */
    template <typename T, typename Base>
    bool is_allocated(Base base, T *ptr, size_t n) const {
        const size_t aligned_len = align_forward(sizeof(T) * n, chunk_size);
        const size_t chunk_count = aligned_len / chunk_size;

        const uint8_t *chunk_base = chunks.ptr(base);
        const size_t chunk_idx =
            (reinterpret_cast<uintptr_t>(ptr) - reinterpret_cast<uintptr_t>(chunk_base)) / chunk_size;

        const uint64_t *bitmaps = bitmap.ptr(base);

        for (size_t i = chunk_idx; i < chunk_idx + chunk_count; i++) {
            const size_t   w = i / BITMAP_BIT_SIZE;
            const unsigned b = (unsigned)(i % BITMAP_BIT_SIZE);
            if ((bitmaps[w] & ((uint64_t)1 << b)) == 0) return false;
        }
        return true;
    }

    /* The layout for a given capacity in bytes. The capacity is rounded up to
     * the nearest chunk and bitmap size so everything divides evenly. */
    static Layout layout(size_t cap) {
        const size_t aligned_cap = align_forward(cap, chunk_size);

        /* One bitmap word per 64 chunks. The chunk count is aligned forward so
         * every word is full, which avoids handling a partial one. */
        const size_t chunk_count = aligned_cap / chunk_size;
        const size_t aligned_chunk_count = align_forward(chunk_count, 64);
        const size_t bitmap_count = aligned_chunk_count / 64;

        const size_t bitmap_start = 0;
        const size_t bitmap_end = sizeof(uint64_t) * bitmap_count;
        const size_t chunks_start = align_forward(bitmap_end, alignof(uint8_t));

        /* The chunk region must be exactly what the bitmaps address: one chunk
         * per bit. More is unreachable waste; less would let alloc hand out
         * memory past the region, since init marks every bit free. */
        const size_t chunks_end = chunks_start + (aligned_chunk_count * chunk_size);

        Layout l;
        l.total_size = chunks_end;
        l.bitmap_count = bitmap_count;
        l.bitmap_start = bitmap_start;
        l.chunks_start = chunks_start;
        return l;
    }
};

} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_BITMAP_ALLOCATOR_HPP */
