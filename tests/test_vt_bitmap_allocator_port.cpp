/* Transliterated from the test blocks in Ghostty
 * src/terminal/bitmap_allocator.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 * Wisp: u21 is uint32_t (@sizeOf(u21) == 4); testing.allocator's aligned
 * buffer is _aligned_malloc.
 */

#include "test_helpers.h"
#include "../vt/bitmap_allocator.hpp"

#include <malloc.h>

using namespace wisp::vt;
using bitmap_allocator::BitmapAllocator;
using bitmap_allocator::findFreeChunks;

struct AlignedBuf {
    uint8_t *ptr;
    size_t len;
    explicit AlignedBuf(size_t n) : ptr((uint8_t *)_aligned_malloc(n, 8)), len(n) {}
    ~AlignedBuf() { _aligned_free(ptr); }
};

static bool all(const uint8_t *p, size_t n, uint8_t v) {
    for (size_t i = 0; i < n; i++) if (p[i] != v) return false;
    return true;
}

TEST(bitmap_allocator, findFreeChunks_single_found) {
    uint64_t bitmaps[] = {
        0x7FFFFFFFFFFFF1FFull, /* 0b01111111_..._11110001_11111111 */
    };
    size_t idx;
    ASSERT_TRUE(findFreeChunks(bitmaps, 1, 2, &idx));
    ASSERT_TRUE(idx == 9);
    ASSERT_TRUE(bitmaps[0] == 0x7FFFFFFFFFFFF7FFull);
}

TEST(bitmap_allocator, findFreeChunks_single_not_found) {
    uint64_t bitmaps[] = { 0x78FFFFFFFFFFFFFFull };
    size_t idx;
    ASSERT_FALSE(findFreeChunks(bitmaps, 1, 4, &idx));
}

TEST(bitmap_allocator, findFreeChunks_multiple_found) {
    uint64_t bitmaps[] = {
        0x78FFFFFFFFFFFF8Full,
        0x7FC1FFFFFFFFC1FFull,
    };
    size_t idx;
    ASSERT_TRUE(findFreeChunks(bitmaps, 2, 4, &idx));
    ASSERT_TRUE(idx == 73);
    ASSERT_TRUE(bitmaps[1] == 0x7FC1FFFFFFFFDFFFull);
}

TEST(bitmap_allocator, findFreeChunks_exactly_64_chunks) {
    uint64_t bitmaps[] = { 0 };
    size_t idx;
    ASSERT_TRUE(findFreeChunks(bitmaps, 1, 64, &idx));
    ASSERT_TRUE(bitmaps[0] == UINT64_MAX);
    ASSERT_TRUE(idx == 0);
}

TEST(bitmap_allocator, findFreeChunks_larger_than_64_chunks) {
    uint64_t bitmaps[] = { 0, 0 };
    size_t idx;
    ASSERT_TRUE(findFreeChunks(bitmaps, 2, 65, &idx));
    ASSERT_TRUE(bitmaps[0] == UINT64_MAX);
    ASSERT_TRUE(bitmaps[1] == 1);
    ASSERT_TRUE(idx == 0);
}

TEST(bitmap_allocator, findFreeChunks_larger_than_64_chunks_not_at_beginning) {
    uint64_t bitmaps[] = { 0x00FFFFFFFFFFFFFFull, 0, 0 };
    size_t idx;
    ASSERT_TRUE(findFreeChunks(bitmaps, 3, 65, &idx));
    ASSERT_TRUE(bitmaps[0] == UINT64_MAX);
    ASSERT_TRUE(bitmaps[1] == 0x01FFFFFFFFFFFFFFull);
    ASSERT_TRUE(bitmaps[2] == 0);
    ASSERT_TRUE(idx == 56);
}

TEST(bitmap_allocator, findFreeChunks_larger_than_64_chunks_exact) {
    uint64_t bitmaps[] = { 0, 0 };
    size_t idx;
    ASSERT_TRUE(findFreeChunks(bitmaps, 2, 128, &idx));
    ASSERT_TRUE(bitmaps[0] == UINT64_MAX);
    ASSERT_TRUE(bitmaps[1] == UINT64_MAX);
    ASSERT_TRUE(idx == 0);
}

TEST(bitmap_allocator, BitmapAllocator_layout) {
    typedef BitmapAllocator<4> Alloc;
    const size_t cap = 64 * 4;

    const Alloc::Layout layout = Alloc::layout(cap);

    /* We expect to use one bitmap since the cap is bytes. */
    ASSERT_TRUE(layout.bitmap_count == 1);
}

template <size_t chunk>
static void layoutChunksCase() {
    typedef BitmapAllocator<chunk> Alloc;
    const size_t caps[] = { 1, chunk, chunk + 1, 48, 64, 512, 1024, 2048, 8192, 8193 };
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        const size_t cap = caps[i];
        const typename Alloc::Layout layout = Alloc::layout(cap);
        const size_t chunks_size = layout.total_size - layout.chunks_start;

        /* Reserved == addressable by the bitmaps. This must match
         * capacityBytes() which is computed from the bitmap count. */
        ASSERT_TRUE(layout.bitmap_count * Alloc::bitmap_bit_size * chunk == chunks_size);

        /* We always reserve at least the requested capacity. */
        ASSERT_TRUE(chunks_size >= cap);
    }
}

TEST(bitmap_allocator, BitmapAllocator_layout_chunks_region_matches_bitmap_addressable_bytes) {
    /* The chunks region must be exactly the bytes addressable by the
     * bitmaps: one chunk per bit of every bitmap. Prior to this being
     * fixed, the region was over-reserved by a factor of chunk_size
     * (~186 KiB of dead space per standard page), while capacities
     * smaller than one full bitmap were under-reserved, allowing
     * out-of-bounds allocations. */
    layoutChunksCase<1>();
    layoutChunksCase<2>();
    layoutChunksCase<4>();
    layoutChunksCase<16>();
    layoutChunksCase<32>();
}

TEST(bitmap_allocator, BitmapAllocator_layout_small_capacity_cannot_alloc_out_of_bounds) {
    /* Regression test: for capacities smaller than one full bitmap of
     * chunks, init marks all bitmap bits as free, so alloc will hand out
     * chunks up to the full bitmap. The layout must reserve that entire
     * addressable region or those allocations would be out of bounds of
     * the backing buffer. */
    typedef BitmapAllocator<16> Alloc;
    const size_t cap = 48; /* 3 chunks, bitmap addresses 64 */

    const Alloc::Layout layout = Alloc::layout(cap);
    AlignedBuf buf(layout.total_size);

    Alloc bm = Alloc::init(size::OffsetBuf::init(buf.ptr), layout);

    /* Allocate every chunk the bitmap can hand out and verify each is
     * fully within the backing buffer. */
    const uintptr_t buf_start = (uintptr_t)buf.ptr;
    const uintptr_t buf_end = buf_start + buf.len;
    size_t count = 0;
    uint8_t *slice;
    while (bm.alloc<uint8_t>((const void *)buf.ptr, 16, &slice)) {
        ASSERT_TRUE((uintptr_t)slice >= buf_start);
        ASSERT_TRUE((uintptr_t)slice + 16 <= buf_end);
        memset(slice, 0xAA, 16);
        count += 1;
    }
    /* else |err| expectEqual(error.OutOfMemory, err): the loop ended on it */
    ASSERT_TRUE(count == Alloc::bitmap_bit_size);
}

TEST(bitmap_allocator, BitmapAllocator_alloc_sequentially) {
    typedef BitmapAllocator<4> Alloc;
    const Alloc::Layout layout = Alloc::layout(64);
    AlignedBuf buf(layout.total_size);
    const void *base = buf.ptr;

    Alloc bm = Alloc::init(size::OffsetBuf::init(buf.ptr), layout);
    uint8_t *ptr;
    ASSERT_TRUE(bm.alloc<uint8_t>(base, 1, &ptr));
    ptr[0] = 'A';

    uint8_t *ptr2;
    ASSERT_TRUE(bm.alloc<uint8_t>(base, 1, &ptr2));
    ASSERT_TRUE(ptr != ptr2);

    /* Should grab the next chunk */
    ASSERT_TRUE((uintptr_t)ptr + 4 == (uintptr_t)ptr2);

    /* Free ptr and next allocation should be back */
    bm.free(base, ptr, 1);
    uint8_t *ptr3;
    ASSERT_TRUE(bm.alloc<uint8_t>(base, 1, &ptr3));
    ASSERT_TRUE(ptr == ptr3);
}

TEST(bitmap_allocator, BitmapAllocator_alloc_non_byte) {
    typedef BitmapAllocator<4> Alloc;
    const Alloc::Layout layout = Alloc::layout(128);
    AlignedBuf buf(layout.total_size);
    const void *base = buf.ptr;

    Alloc bm = Alloc::init(size::OffsetBuf::init(buf.ptr), layout);
    uint32_t *ptr;
    ASSERT_TRUE(bm.alloc<uint32_t>(base, 1, &ptr));
    ptr[0] = 'A';

    uint32_t *ptr2;
    ASSERT_TRUE(bm.alloc<uint32_t>(base, 1, &ptr2));
    ASSERT_TRUE(ptr != ptr2);
    ASSERT_TRUE((uintptr_t)ptr + 4 == (uintptr_t)ptr2);

    /* Free ptr and next allocation should be back */
    bm.free(base, ptr, 1);
    uint32_t *ptr3;
    ASSERT_TRUE(bm.alloc<uint32_t>(base, 1, &ptr3));
    ASSERT_TRUE(ptr == ptr3);
}

TEST(bitmap_allocator, BitmapAllocator_alloc_non_byte_multi_chunk) {
    typedef BitmapAllocator<4 * sizeof(uint32_t)> Alloc;
    const Alloc::Layout layout = Alloc::layout(128);
    AlignedBuf buf(layout.total_size);
    const void *base = buf.ptr;

    Alloc bm = Alloc::init(size::OffsetBuf::init(buf.ptr), layout);
    uint32_t *ptr;
    ASSERT_TRUE(bm.alloc<uint32_t>(base, 6, &ptr));
    for (size_t i = 0; i < 6; i++) ptr[i] = 'A';

    uint32_t *ptr2;
    ASSERT_TRUE(bm.alloc<uint32_t>(base, 1, &ptr2));
    ASSERT_TRUE(ptr != ptr2);
    ASSERT_TRUE((uintptr_t)ptr + (sizeof(uint32_t) * 4 * 2) == (uintptr_t)ptr2);

    /* Free ptr and next allocation should be back */
    bm.free(base, ptr, 6);
    uint32_t *ptr3;
    ASSERT_TRUE(bm.alloc<uint32_t>(base, 1, &ptr3));
    ASSERT_TRUE(ptr == ptr3);
}

TEST(bitmap_allocator, BitmapAllocator_alloc_large) {
    typedef BitmapAllocator<2> Alloc;
    const Alloc::Layout layout = Alloc::layout(256);
    AlignedBuf buf(layout.total_size);
    const void *base = buf.ptr;

    Alloc bm = Alloc::init(size::OffsetBuf::init(buf.ptr), layout);
    uint8_t *ptr;
    ASSERT_TRUE(bm.alloc<uint8_t>(base, 129, &ptr));
    ptr[0] = 'A';
    bm.free(base, ptr, 129);
}

typedef BitmapAllocator<1> A1;
static const size_t BB = A1::bitmap_bit_size;

static bool bitmapsZero(A1 &bm, const void *base, size_t n) {
    const uint64_t *b = bm.bitmap.ptr(base);
    for (size_t i = 0; i < n; i++) if (b[i] != 0) return false;
    return true;
}

TEST(bitmap_allocator, BitmapAllocator_alloc_and_free_one_bitmap) {
    const A1::Layout layout = A1::layout(BB * 3);
    AlignedBuf buf(layout.total_size);
    const void *base = buf.ptr;
    A1 bm = A1::init(size::OffsetBuf::init(buf.ptr), layout);

    /* Allocate exactly one bitmap worth of bytes. */
    uint8_t *slice;
    ASSERT_TRUE(bm.alloc<uint8_t>(base, BB, &slice));

    memset(slice, 0x11, BB);
    ASSERT_TRUE(all(slice, BB, 0x11));

    /* Free it */
    ASSERT_TRUE(bm.isAllocated(base, slice, BB));
    bm.free(base, slice, BB);
    ASSERT_FALSE(bm.isAllocated(base, slice, BB));

    /* All of our bitmaps should be free. */
    ASSERT_TRUE(bitmapsZero(bm, base, 3));
}

TEST(bitmap_allocator, BitmapAllocator_search_hint_skips_full_words) {
    const A1::Layout layout = A1::layout(BB * 3);
    AlignedBuf buf(layout.total_size);
    const void *base = buf.ptr;
    A1 bm = A1::init(size::OffsetBuf::init(buf.ptr), layout);
    ASSERT_TRUE(bm.search_start == 0);

    /* Fill the first bitmap word exactly with single-chunk allocations. */
    uint8_t *first = nullptr;
    for (size_t i = 0; i < BB; i++) {
        uint8_t *slice;
        ASSERT_TRUE(bm.alloc<uint8_t>(base, 1, &slice));
        if (i == 0) first = slice;
    }

    /* The first word is exhausted so scans start at the second word. */
    ASSERT_TRUE(bm.search_start == 1);

    /* The next allocation is the first chunk of the second word, so
     * the skipped-word index arithmetic must still yield the right
     * chunk address. */
    uint8_t *next;
    ASSERT_TRUE(bm.alloc<uint8_t>(base, 1, &next));
    ASSERT_TRUE((uintptr_t)first + BB == (uintptr_t)next);

    /* Freeing a chunk in the first word lowers the hint so the freed
     * space is found again. */
    bm.free(base, first, 1);
    ASSERT_TRUE(bm.search_start == 0);
    uint8_t *again;
    ASSERT_TRUE(bm.alloc<uint8_t>(base, 1, &again));
    ASSERT_TRUE(first == again);
}

TEST(bitmap_allocator, BitmapAllocator_search_hint_does_not_skip_partial_words) {
    const A1::Layout layout = A1::layout(BB * 2);
    AlignedBuf buf(layout.total_size);
    const void *base = buf.ptr;
    A1 bm = A1::init(size::OffsetBuf::init(buf.ptr), layout);

    /* Allocate all but 4 chunks of the first word. */
    uint8_t *first = nullptr;
    for (size_t i = 0; i < BB - 4; i++) {
        uint8_t *slice;
        ASSERT_TRUE(bm.alloc<uint8_t>(base, 1, &slice));
        if (i == 0) first = slice;
    }

    /* An 8-chunk run can't fit the 4 remaining bits (small runs never
     * span words), so it comes from the second word — but the hint
     * must stay at the first word, which still has free bits. */
    uint8_t *big;
    ASSERT_TRUE(bm.alloc<uint8_t>(base, 8, &big));
    ASSERT_TRUE((uintptr_t)first + BB == (uintptr_t)big);
    ASSERT_TRUE(bm.search_start == 0);

    /* A 4-chunk run fits the first word's remaining bits exactly; a
     * hint that skipped the partial word would wrongly place this in
     * the second word (or report OutOfMemory once that filled). */
    uint8_t *small;
    ASSERT_TRUE(bm.alloc<uint8_t>(base, 4, &small));
    ASSERT_TRUE((uintptr_t)first + BB - 4 == (uintptr_t)small);

    /* Now the first word is full and the hint advances past it. */
    ASSERT_TRUE(bm.search_start == 1);
}

/* Wisp: the remaining upstream tests share one shape — allocate a list of
 * sizes, fill each with its own byte, check none clobbered another, free
 * them in the listed order, and check every bitmap word is zero again. */
struct Spec { size_t len; uint8_t fill; };

static void allocFillFree(size_t words, const Spec *specs, size_t n_specs,
                          const size_t *free_order) {
    const A1::Layout layout = A1::layout(BB * words);
    AlignedBuf buf(layout.total_size);
    const void *base = buf.ptr;
    A1 bm = A1::init(size::OffsetBuf::init(buf.ptr), layout);

    uint8_t *slices[4];
    for (size_t i = 0; i < n_specs; i++) {
        ASSERT_TRUE(bm.alloc<uint8_t>(base, specs[i].len, &slices[i]));
        memset(slices[i], specs[i].fill, specs[i].len);
        for (size_t k = 0; k <= i; k++) ASSERT_TRUE(all(slices[k], specs[k].len, specs[k].fill));
    }

    for (size_t j = 0; j < n_specs; j++) {
        const size_t i = free_order[j];
        ASSERT_TRUE(bm.isAllocated(base, slices[i], specs[i].len));
        bm.free(base, slices[i], specs[i].len);
        ASSERT_FALSE(bm.isAllocated(base, slices[i], specs[i].len));
    }

    /* All of our bitmaps should be free. */
    ASSERT_TRUE(bitmapsZero(bm, base, words));
}

TEST(bitmap_allocator, BitmapAllocator_alloc_and_free_half_bitmap) {
    const Spec s[] = { { BB / 2, 0x11 } };
    const size_t order[] = { 0 };
    allocFillFree(3, s, 1, order);
}

TEST(bitmap_allocator, BitmapAllocator_alloc_and_free_two_half_bitmaps) {
    const Spec s[] = { { BB / 2, 0x11 }, { BB / 2, 0x22 } };
    const size_t order[] = { 1, 0 };
    allocFillFree(3, s, 2, order);
}

TEST(bitmap_allocator, BitmapAllocator_alloc_and_free_1_5_bitmaps) {
    const Spec s[] = { { 3 * BB / 2, 0x11 } };
    const size_t order[] = { 0 };
    allocFillFree(3, s, 1, order);
}

TEST(bitmap_allocator, BitmapAllocator_alloc_and_free_two_1_5_bitmaps) {
    const Spec s[] = { { 3 * BB / 2, 0x11 }, { 3 * BB / 2, 0x22 } };
    const size_t order[] = { 1, 0 };
    allocFillFree(3, s, 2, order);
}

TEST(bitmap_allocator, BitmapAllocator_alloc_and_free_1_5_bitmaps_offset_by_0_75) {
    /* Then a 1.5 bitmap sized allocation, so that it spans
     * from 0.75 to 2.25, occupying bits in 3 different bitmaps. */
    const Spec s[] = { { 3 * BB / 4, 0x11 }, { 3 * BB / 2, 0x22 } };
    const size_t order[] = { 1, 0 };
    allocFillFree(3, s, 2, order);
}

TEST(bitmap_allocator, BitmapAllocator_alloc_and_free_three_0_75_bitmaps) {
    const Spec s[] = { { 3 * BB / 4, 0x11 }, { 3 * BB / 4, 0x22 }, { 3 * BB / 4, 0x33 } };
    const size_t order[] = { 1, 0, 2 };
    allocFillFree(3, s, 3, order);
}

TEST(bitmap_allocator, BitmapAllocator_alloc_and_free_two_1_5_bitmaps_offset_0_75) {
    const Spec s[] = { { 3 * BB / 4, 0x11 }, { 3 * BB / 2, 0x22 }, { 3 * BB / 2, 0x33 } };
    const size_t order[] = { 1, 0, 2 };
    allocFillFree(4, s, 3, order);
}

TEST(bitmap_allocator, BitmapAllocator_bytesRequired) {
    /* Chunk size of 16 bytes (like grapheme_chunk in page.zig) */
    {
        typedef BitmapAllocator<16> Alloc;

        /* Single byte rounds up to chunk size */
        ASSERT_TRUE(Alloc::bytesRequired<uint8_t>(1) == 16);
        ASSERT_TRUE(Alloc::bytesRequired<uint8_t>(16) == 16);
        ASSERT_TRUE(Alloc::bytesRequired<uint8_t>(17) == 32);

        /* u21 (4 bytes each) */
        ASSERT_TRUE(Alloc::bytesRequired<uint32_t>(1) == 16); /* 4 bytes -> 16 */
        ASSERT_TRUE(Alloc::bytesRequired<uint32_t>(4) == 16); /* 16 bytes -> 16 */
        ASSERT_TRUE(Alloc::bytesRequired<uint32_t>(5) == 32); /* 20 bytes -> 32 */
        ASSERT_TRUE(Alloc::bytesRequired<uint32_t>(6) == 32); /* 24 bytes -> 32 */
    }

    /* Chunk size of 4 bytes */
    {
        typedef BitmapAllocator<4> Alloc;

        ASSERT_TRUE(Alloc::bytesRequired<uint8_t>(1) == 4);
        ASSERT_TRUE(Alloc::bytesRequired<uint8_t>(4) == 4);
        ASSERT_TRUE(Alloc::bytesRequired<uint8_t>(5) == 8);

        /* u32 (4 bytes each) - exactly one chunk per element */
        ASSERT_TRUE(Alloc::bytesRequired<uint32_t>(1) == 4);
        ASSERT_TRUE(Alloc::bytesRequired<uint32_t>(2) == 8);
    }

    /* Chunk size of 32 bytes (like string_chunk in page.zig) */
    {
        typedef BitmapAllocator<32> Alloc;

        ASSERT_TRUE(Alloc::bytesRequired<uint8_t>(1) == 32);
        ASSERT_TRUE(Alloc::bytesRequired<uint8_t>(32) == 32);
        ASSERT_TRUE(Alloc::bytesRequired<uint8_t>(33) == 64);
    }
}
