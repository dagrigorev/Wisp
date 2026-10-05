/* Tests for src/terminal/bitmap_allocator.hpp.
 *
 * Ported from the test cases in Ghostty src/terminal/bitmap_allocator.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 */

#include "test_helpers.h"
#include "bitmap_allocator.hpp"

using namespace wisp::terminal;

/* A 64-bit aligned backing buffer for allocator tests. */
struct Backing {
    uint64_t words[512];
    uint8_t *base() { return reinterpret_cast<uint8_t *>(words); }
    Backing() { memset(words, 0, sizeof(words)); }
};

/* ─── bit helpers ────────────────────────────────────────────────────────── */

/* Zig's @clz/@ctz return the bit width for a zero input. The x86 bsr/bsf
 * instructions do not define their output there, so this is the case most
 * likely to diverge in the port. */
TEST(bitmap, clz_ctz_zero_input) {
    ASSERT_EQ(clz64(0), 64);
    ASSERT_EQ(ctz64(0), 64);

    ASSERT_EQ(clz64(1), 63);
    ASSERT_EQ(ctz64(1), 0);

    ASSERT_EQ(clz64(~(uint64_t)0), 0);
    ASSERT_EQ(ctz64(~(uint64_t)0), 0);

    ASSERT_EQ(clz64((uint64_t)1 << 63), 0);
    ASSERT_EQ(ctz64((uint64_t)1 << 63), 63);
}

TEST(bitmap, popcount) {
    ASSERT_EQ(popcount64(0), 0);
    ASSERT_EQ(popcount64(~(uint64_t)0), 64);
    ASSERT_EQ(popcount64(0xF0F0F0F0F0F0F0F0ULL), 32);
    ASSERT_EQ(popcount64(1), 1);
}

/* ─── find_free_chunks ───────────────────────────────────────────────────── */

TEST(bitmap, find_free_chunks_single_found) {
    /* One word whose byte at bits 8..15 is 0b11110001, leaving bits 9, 10
     * and 11 free. */
    uint64_t bitmaps[1] = { 0x7FFFFFFFFFFFF1FFULL };

    const size_t idx = find_free_chunks(bitmaps, 1, 2);
    ASSERT_EQ(idx, 9);

    /* The two lowest free bits of the gap are now marked used. */
    ASSERT_TRUE(bitmaps[0] == 0x7FFFFFFFFFFFF7FFULL);
}

TEST(bitmap, find_free_chunks_single_not_found) {
    /* The only gap is 4 bits wide at the top, but it is not 4 *free* bits in
     * a row anywhere the scan can use. */
    uint64_t bitmaps[1] = { 0x78FFFFFFFFFFFFFFULL };
    const size_t idx = find_free_chunks(bitmaps, 1, 4);
    ASSERT_TRUE(idx == BITMAP_NOT_FOUND);
}

TEST(bitmap, find_free_chunks_multiple_found) {
    uint64_t bitmaps[2] = {
        0x78FFFFFFFFFFFF8FULL,
        0x7FC1FFFFFFFFC1FFULL,
    };

    const size_t idx = find_free_chunks(bitmaps, 2, 4);

    /* The first run of 4 wide enough is in the second word, at bit 9,
     * i.e. absolute chunk 64 + 9 = 73. */
    ASSERT_EQ(idx, 73);
    ASSERT_TRUE(bitmaps[1] == 0x7FC1FFFFFFFFDFFFULL);
}

TEST(bitmap, find_free_chunks_exactly_64) {
    uint64_t bitmaps[1] = { 0 };

    const size_t idx = find_free_chunks(bitmaps, 1, 64);

    ASSERT_EQ(idx, 0);
    ASSERT_TRUE(bitmaps[0] == ~(uint64_t)0);
}

/* The >64 path is a separate algorithm from the <=64 one, so each shape of
 * crossing a word boundary gets its own case. */
TEST(bitmap, find_free_chunks_larger_than_64) {
    uint64_t bitmaps[2] = { 0, 0 };

    const size_t idx = find_free_chunks(bitmaps, 2, 65);

    ASSERT_EQ(idx, 0);
    ASSERT_TRUE(bitmaps[0] == ~(uint64_t)0);
    ASSERT_TRUE(bitmaps[1] == 0x1ULL);
}

TEST(bitmap, find_free_chunks_larger_than_64_not_at_beginning) {
    /* The first word is used except for its top 8 bits, so a 65-chunk run has
     * to begin at bit 56 and spill into the next word. */
    uint64_t bitmaps[3] = { 0x00FFFFFFFFFFFFFFULL, 0, 0 };

    const size_t idx = find_free_chunks(bitmaps, 3, 65);

    ASSERT_EQ(idx, 56);
    ASSERT_TRUE(bitmaps[0] == ~(uint64_t)0);
    ASSERT_TRUE(bitmaps[1] == 0x01FFFFFFFFFFFFFFULL);
    ASSERT_TRUE(bitmaps[2] == 0);
}

TEST(bitmap, find_free_chunks_larger_than_64_exact) {
    uint64_t bitmaps[2] = { 0, 0 };

    const size_t idx = find_free_chunks(bitmaps, 2, 128);

    ASSERT_EQ(idx, 0);
    ASSERT_TRUE(bitmaps[0] == ~(uint64_t)0);
    ASSERT_TRUE(bitmaps[1] == ~(uint64_t)0);
}

/* Guard added in the port: a prefix in the final word would otherwise let the
 * scan read one word past the end. Zig catches this with bounds checking. */
TEST(bitmap, find_free_chunks_run_past_last_word_is_rejected) {
    uint64_t bitmaps[1] = { 0x00FFFFFFFFFFFFFFULL };
    const size_t idx = find_free_chunks(bitmaps, 1, 65);
    ASSERT_TRUE(idx == BITMAP_NOT_FOUND);
}

/* ─── layout ─────────────────────────────────────────────────────────────── */

TEST(bitmap, layout_one_bitmap) {
    typedef BitmapAllocator<4> Alloc;
    Alloc::Layout l = Alloc::layout(64 * 4);

    /* The cap is in bytes, so 64 chunks of 4 bytes is exactly one word. */
    ASSERT_EQ(l.bitmap_count, 1);
}

TEST(bitmap, layout_chunks_region_matches_addressable_bytes) {
    typedef BitmapAllocator<4> Alloc;

    /* Every bit of every bitmap word must map to exactly one chunk: more is
     * unreachable waste, less would let alloc hand out memory past the end. */
    const size_t caps[] = { 4, 16, 64 * 4, 64 * 4 + 4, 1000, 4096 };
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        Alloc::Layout l = Alloc::layout(caps[i]);
        const size_t addressable = l.bitmap_count * 64 * 4;
        ASSERT_EQ(l.total_size - l.chunks_start, addressable);
    }
}

TEST(bitmap, layout_rounds_up_to_full_words) {
    typedef BitmapAllocator<4> Alloc;

    /* One byte still needs a whole word of bitmap. */
    Alloc::Layout l = Alloc::layout(1);
    ASSERT_EQ(l.bitmap_count, 1);

    /* One chunk past a full word needs a second. */
    Alloc::Layout l2 = Alloc::layout(64 * 4 + 1);
    ASSERT_EQ(l2.bitmap_count, 2);
}

/* ─── alloc / free ───────────────────────────────────────────────────────── */

TEST(bitmap, alloc_sequentially) {
    typedef BitmapAllocator<4> Alloc;
    Backing backing;

    Alloc::Layout l = Alloc::layout(8 * 4);
    Alloc a = Alloc::init(OffsetBuf::init(backing.base()), l);

    uint8_t *p = a.alloc<uint8_t>(backing.base(), 1);
    ASSERT_NOT_NULL(p);
    p[0] = 'A';

    uint8_t *q = a.alloc<uint8_t>(backing.base(), 1);
    ASSERT_NOT_NULL(q);
    ASSERT_TRUE(q != p);

    /* One chunk apart, in order. */
    ASSERT_EQ((long)(q - p), 4);

    ASSERT_TRUE(a.is_allocated(backing.base(), p, 1));
    ASSERT_TRUE(a.is_allocated(backing.base(), q, 1));
}

TEST(bitmap, alloc_non_byte) {
    typedef BitmapAllocator<4> Alloc;
    Backing backing;

    Alloc::Layout l = Alloc::layout(64 * 4);
    Alloc a = Alloc::init(OffsetBuf::init(backing.base()), l);

    uint32_t *p = a.alloc<uint32_t>(backing.base(), 1);
    ASSERT_NOT_NULL(p);
    *p = 0xDEADBEEF;
    ASSERT_TRUE(*p == 0xDEADBEEF);
    ASSERT_EQ(a.used_bytes(backing.base()), 4);
}

TEST(bitmap, alloc_multi_chunk) {
    typedef BitmapAllocator<4> Alloc;
    Backing backing;

    Alloc::Layout l = Alloc::layout(64 * 4);
    Alloc a = Alloc::init(OffsetBuf::init(backing.base()), l);

    /* 4 x uint32_t is 16 bytes, which is 4 chunks. */
    uint32_t *p = a.alloc<uint32_t>(backing.base(), 4);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(a.used_bytes(backing.base()), 16);
    ASSERT_TRUE(a.is_allocated(backing.base(), p, 4));
}

TEST(bitmap, alloc_large_spanning_words) {
    typedef BitmapAllocator<4> Alloc;
    Backing backing;

    Alloc::Layout l = Alloc::layout(128 * 4);
    Alloc a = Alloc::init(OffsetBuf::init(backing.base()), l);
    ASSERT_EQ(l.bitmap_count, 2);

    /* 65 chunks — crosses the first word boundary. */
    uint8_t *p = a.alloc<uint8_t>(backing.base(), 65 * 4);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(a.used_bytes(backing.base()), 65 * 4);
}

TEST(bitmap, alloc_out_of_memory) {
    typedef BitmapAllocator<4> Alloc;
    Backing backing;

    Alloc::Layout l = Alloc::layout(4 * 4);
    Alloc a = Alloc::init(OffsetBuf::init(backing.base()), l);

    /* layout() rounds up to a full word, so capacity is 64 chunks. */
    ASSERT_EQ(a.capacity_bytes(), 64 * 4);

    uint8_t *p = a.alloc<uint8_t>(backing.base(), 64 * 4);
    ASSERT_NOT_NULL(p);

    /* Nothing left. */
    uint8_t *q = a.alloc<uint8_t>(backing.base(), 1);
    ASSERT_NULL(q);
}

TEST(bitmap, alloc_and_free_round_trip) {
    typedef BitmapAllocator<4> Alloc;
    Backing backing;

    Alloc::Layout l = Alloc::layout(64 * 4);
    Alloc a = Alloc::init(OffsetBuf::init(backing.base()), l);

    uint8_t *p = a.alloc<uint8_t>(backing.base(), 16);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(a.used_bytes(backing.base()), 16);

    a.free(backing.base(), p, 16);
    ASSERT_EQ(a.used_bytes(backing.base()), 0);

    /* The freed space is reusable, and comes back at the same place. */
    uint8_t *q = a.alloc<uint8_t>(backing.base(), 16);
    ASSERT_TRUE(q == p);
}

TEST(bitmap, free_half_word) {
    typedef BitmapAllocator<4> Alloc;
    Backing backing;

    Alloc::Layout l = Alloc::layout(64 * 4);
    Alloc a = Alloc::init(OffsetBuf::init(backing.base()), l);

    uint8_t *p = a.alloc<uint8_t>(backing.base(), 32 * 4);
    ASSERT_NOT_NULL(p);
    uint8_t *q = a.alloc<uint8_t>(backing.base(), 32 * 4);
    ASSERT_NOT_NULL(q);
    ASSERT_EQ(a.used_bytes(backing.base()), 64 * 4);

    a.free(backing.base(), p, 32 * 4);
    ASSERT_EQ(a.used_bytes(backing.base()), 32 * 4);
    ASSERT_TRUE(a.is_allocated(backing.base(), q, 32 * 4));

    a.free(backing.base(), q, 32 * 4);
    ASSERT_EQ(a.used_bytes(backing.base()), 0);
}

TEST(bitmap, free_spanning_multiple_words) {
    typedef BitmapAllocator<4> Alloc;
    Backing backing;

    Alloc::Layout l = Alloc::layout(192 * 4);
    Alloc a = Alloc::init(OffsetBuf::init(backing.base()), l);
    ASSERT_EQ(l.bitmap_count, 3);

    /* 1.5 words' worth, so the free path exercises a partial head word, a
     * whole middle word and a partial tail. */
    uint8_t *p = a.alloc<uint8_t>(backing.base(), 96 * 4);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(a.used_bytes(backing.base()), 96 * 4);

    a.free(backing.base(), p, 96 * 4);
    ASSERT_EQ(a.used_bytes(backing.base()), 0);
}

/* ─── search hint ────────────────────────────────────────────────────────── */

TEST(bitmap, search_hint_skips_full_words) {
    typedef BitmapAllocator<4> Alloc;
    Backing backing;

    Alloc::Layout l = Alloc::layout(128 * 4);
    Alloc a = Alloc::init(OffsetBuf::init(backing.base()), l);

    /* Fill the first word exactly. */
    uint8_t *p = a.alloc<uint8_t>(backing.base(), 64 * 4);
    ASSERT_NOT_NULL(p);

    /* The hint should now skip it. */
    ASSERT_EQ(a.search_start, 1);
}

TEST(bitmap, search_hint_does_not_skip_partial_words) {
    typedef BitmapAllocator<4> Alloc;
    Backing backing;

    Alloc::Layout l = Alloc::layout(128 * 4);
    Alloc a = Alloc::init(OffsetBuf::init(backing.base()), l);

    /* Leave one chunk free in the first word. */
    uint8_t *p = a.alloc<uint8_t>(backing.base(), 63 * 4);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(a.search_start, 0);
}

TEST(bitmap, free_moves_search_hint_back) {
    typedef BitmapAllocator<4> Alloc;
    Backing backing;

    Alloc::Layout l = Alloc::layout(128 * 4);
    Alloc a = Alloc::init(OffsetBuf::init(backing.base()), l);

    uint8_t *p = a.alloc<uint8_t>(backing.base(), 64 * 4);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(a.search_start, 1);

    /* Freeing into word 0 must pull the hint back, or the space would be
     * permanently unreachable. */
    a.free(backing.base(), p, 64 * 4);
    ASSERT_EQ(a.search_start, 0);

    uint8_t *q = a.alloc<uint8_t>(backing.base(), 4);
    ASSERT_TRUE(q == p);
}

/* ─── misc ───────────────────────────────────────────────────────────────── */

TEST(bitmap, bytes_required) {
    typedef BitmapAllocator<4> Alloc;

    ASSERT_EQ(Alloc::bytes_required<uint8_t>(1), 4);
    ASSERT_EQ(Alloc::bytes_required<uint8_t>(4), 4);
    ASSERT_EQ(Alloc::bytes_required<uint8_t>(5), 8);
    ASSERT_EQ(Alloc::bytes_required<uint32_t>(1), 4);
    ASSERT_EQ(Alloc::bytes_required<uint32_t>(2), 8);
}

TEST(bitmap, init_assume_zeroed_matches_init) {
    typedef BitmapAllocator<4> Alloc;

    Backing b1;
    Backing b2;

    Alloc::Layout l = Alloc::layout(64 * 4);

    Alloc a1 = Alloc::init(OffsetBuf::init(b1.base()), l);
    /* Backing is already zeroed by its constructor, which is the precondition. */
    Alloc a2 = Alloc::init_assume_zeroed(OffsetBuf::init(b2.base()), l);

    ASSERT_EQ(a1.used_bytes(b1.base()), 0);
    ASSERT_EQ(a2.used_bytes(b2.base()), 0);

    uint8_t *p1 = a1.alloc<uint8_t>(b1.base(), 8);
    uint8_t *p2 = a2.alloc<uint8_t>(b2.base(), 8);
    ASSERT_NOT_NULL(p1);
    ASSERT_NOT_NULL(p2);

    /* Same relative placement from their respective bases. */
    ASSERT_EQ((long)(p1 - b1.base()), (long)(p2 - b2.base()));
}
