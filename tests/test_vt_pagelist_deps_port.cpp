/* Transliterated from the test blocks in Ghostty src/tripwire.zig,
 * src/datastruct/intrusive_linked_list.zig, src/terminal/mem.zig and
 * src/terminal/compress/lz4.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 * Wisp: lz4.zig's `test { _ = @import("lz4_differential.zig"); }` pulls in
 * a comparison against the reference liblz4, which Wisp does not vendor.
 */

#include <stdlib.h>
#include <string.h>

#include "test_helpers.h"
#include "../vt/tripwire.hpp"
#include "../vt/mem.hpp"
#include "../vt/compress/lz4.hpp"
#include "../datastruct/intrusive_linked_list.hpp"
#include "../zigstd/random.hpp"

using namespace wisp;

/* ---- tripwire.zig ---- */

enum class IoPoint { read, write };
enum class IoError { none, OutOfMemory };
typedef tripwire::Module<IoPoint, IoError, 2> io;

TEST(tripwire, test) {
    /* Reset should work */
    ASSERT_TRUE(io::end(tripwire::ResetMode::reset));

    /* By default, its pass-through */
    ASSERT_TRUE(io::check(IoPoint::read) == IoError::none);

    /* Always trip */
    io::errorAlways(IoPoint::read, IoError::OutOfMemory);
    ASSERT_TRUE(io::check(IoPoint::read) == IoError::OutOfMemory);
    /* Happens again */
    ASSERT_TRUE(io::check(IoPoint::read) == IoError::OutOfMemory);
    ASSERT_TRUE(io::end(tripwire::ResetMode::reset));
}

enum class FooBar { none, Foo, Bar };
TEST(tripwire, module_as_error_set) {
    typedef tripwire::Module<IoPoint, FooBar, 2> io2;
    ASSERT_TRUE(io2::end(tripwire::ResetMode::reset));
}

TEST(tripwire, errorAfter) {
    /* Trip after 2 calls (on the 3rd call) */
    io::errorAfter(IoPoint::read, IoError::OutOfMemory, 2);

    /* First two calls succeed */
    ASSERT_TRUE(io::check(IoPoint::read) == IoError::none);
    ASSERT_TRUE(io::check(IoPoint::read) == IoError::none);

    /* Third call and on trips */
    ASSERT_TRUE(io::check(IoPoint::read) == IoError::OutOfMemory);
    ASSERT_TRUE(io::check(IoPoint::read) == IoError::OutOfMemory);

    ASSERT_TRUE(io::end(tripwire::ResetMode::reset));
}

enum class ReadPoint { read };
TEST(tripwire, errorAfter_untripped_error_if_min_not_reached) {
    typedef tripwire::Module<ReadPoint, IoError, 1> io3;
    io3::errorAfter(ReadPoint::read, IoError::OutOfMemory, 2);
    /* Only call once, not enough to trip */
    ASSERT_TRUE(io3::check(ReadPoint::read) == IoError::none);
    /* end should fail because tripwire was set but never tripped */
    ASSERT_FALSE(io3::end(tripwire::ResetMode::reset));
}

/* ---- intrusive_linked_list.zig ---- */

struct LNode {
    uint32_t data;
    LNode *prev;
    LNode *next;
    explicit LNode(uint32_t d) : data(d), prev(nullptr), next(nullptr) {}
};

TEST(intrusive_linked_list, basic_DoublyLinkedList_test) {
    datastruct::IntrusiveDoublyLinkedList<LNode> list;

    LNode one(1), two(2), three(3), four(4), five(5);

    list.append(&two);               /* {2} */
    list.append(&five);              /* {2, 5} */
    list.prepend(&one);              /* {1, 2, 5} */
    list.insertBefore(&five, &four); /* {1, 2, 4, 5} */
    list.insertAfter(&two, &three);  /* {1, 2, 3, 4, 5} */

    /* Traverse forwards. */
    {
        uint32_t index = 1;
        for (LNode *it = list.first; it; it = it->next) {
            ASSERT_TRUE(it->data == index);
            index += 1;
        }
    }

    /* Traverse backwards. */
    {
        uint32_t index = 1;
        for (LNode *it = list.last; it; it = it->prev) {
            ASSERT_TRUE(it->data == (6 - index));
            index += 1;
        }
    }

    (void)list.popFirst(); /* {2, 3, 4, 5} */
    (void)list.pop();      /* {2, 3, 4} */
    list.remove(&three);   /* {2, 4} */

    ASSERT_TRUE(list.first->data == 2);
    ASSERT_TRUE(list.last->data == 4);
}

/* ---- mem.zig ---- */

static bool allEqual(const uint8_t *p, size_t len, uint8_t v) {
    for (size_t i = 0; i < len; i++)
        if (p[i] != v) return false;
    return true;
}

TEST(mem, decommit_with_zero_fallback_clears_the_dirty_prefix) {
    const size_t memory_len = 2 * 4096;
    uint8_t *memory = (uint8_t *)_aligned_malloc(memory_len, 4096);

    memset(memory, 0xAA, memory_len);
    (void)vt::mem::decommit(vt::mem::DecommitMode::zero, memory, memory_len, memory_len);
    ASSERT_TRUE(allEqual(memory, memory_len, 0));

    /* The tail is already zero by contract, so a partially dirty mapping only
     * needs its dirty prefix cleared. */
    memset(memory, 0xAA, 1024);
    (void)vt::mem::decommit(vt::mem::DecommitMode::zero, memory, memory_len, 1024);
    ASSERT_TRUE(allEqual(memory, memory_len, 0));
    _aligned_free(memory);
}

TEST(mem, strict_decommit_retains_the_mapping_for_recommit) {
    const size_t memory_len = 2 * 4096;
    uint8_t *memory = (uint8_t *)_aligned_malloc(memory_len, 4096);
    memset(memory, 0xAA, memory_len);

    ASSERT_TRUE(vt::mem::decommit(vt::mem::DecommitMode::strict, memory, memory_len, memory_len));
    ASSERT_TRUE(allEqual(memory, memory_len, 0));

    vt::mem::recommit(memory, memory_len);
    memset(memory, 0xBB, memory_len);
    ASSERT_TRUE(allEqual(memory, memory_len, 0xBB));
    _aligned_free(memory);
}

TEST(mem, test_builds_can_reclaim_retained_mappings) {
    ASSERT_TRUE(vt::mem::canReclaim(vt::mem::DecommitMode::zero));
    ASSERT_TRUE(vt::mem::canReclaim(vt::mem::DecommitMode::strict));
}

/* ---- lz4.zig ---- */

using namespace wisp::vt::lz4;
typedef DecompressError DE;

static DE dec(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len, size_t *n) {
    return decompress(in, in_len, out, out_len, n);
}

/* Shared round-trip assertion used by the corpus-style tests below. */
static bool expectRoundTrip(const uint8_t *input, size_t len) {
    size_t bound;
    if (compressBound(len, &bound) != CompressError::none) return false;
    uint8_t *encoded = (uint8_t *)malloc(bound ? bound : 1);
    uint8_t *decoded = (uint8_t *)malloc(len ? len : 1);

    static HashTable table;
    size_t encoded_len = 0, n = 0;
    bool ok = compress(input, len, encoded, bound, table, &encoded_len) == CompressError::none &&
              dec(encoded, encoded_len, decoded, len, &n) == DE::none && n == len &&
              (len == 0 || memcmp(input, decoded, len) == 0);
    free(encoded);
    free(decoded);
    return ok;
}

TEST(lz4, compressBound) {
    size_t n;
    ASSERT_TRUE(compressBound(0, &n) == CompressError::none && n == 16);
    ASSERT_TRUE(compressBound(255, &n) == CompressError::none && n == 272);
    ASSERT_TRUE(compressBound(max_input_size + 1, &n) == CompressError::InputTooLarge);
}

TEST(lz4, literal_only_compatibility_vectors) {
    size_t n;
    uint8_t empty[1];
    const uint8_t z[] = {0};
    ASSERT_TRUE(dec(z, 1, empty, 0, &n) == DE::none && n == 0);

    uint8_t hello[5];
    const uint8_t h[] = {0x50, 'h', 'e', 'l', 'l', 'o'};
    ASSERT_TRUE(dec(h, sizeof h, hello, 5, &n) == DE::none && n == 5);
    ASSERT_TRUE(memcmp(hello, "hello", 5) == 0);

    uint8_t fifteen[15];
    uint8_t encoded[17];
    encoded[0] = 0xF0;
    encoded[1] = 0;
    memset(encoded + 2, 'x', 15);
    ASSERT_TRUE(dec(encoded, 17, fifteen, 15, &n) == DE::none);
    ASSERT_TRUE(allEqual(fifteen, 15, 'x'));
}

TEST(lz4, overlapping_match_compatibility_vector) {
    /* One literal 'a', followed by a four-byte match at distance one. */
    uint8_t output[5];
    size_t n;
    const uint8_t in[] = {0x10, 'a', 0x01, 0x00};
    ASSERT_TRUE(dec(in, sizeof in, output, 5, &n) == DE::none && n == 5);
    ASSERT_TRUE(memcmp(output, "aaaaa", 5) == 0);
}

TEST(lz4, extended_overlapping_match_compatibility_vector) {
    /* One literal followed by a 274-byte match. The match extension is
     * encoded as 255 + 0 after the low token nibble's initial 15 bytes. */
    uint8_t output[275];
    size_t n;
    const uint8_t in[] = {0x1F, 'a', 0x01, 0x00, 0xFF, 0x00};
    ASSERT_TRUE(dec(in, sizeof in, output, sizeof output, &n) == DE::none && n == sizeof output);
    ASSERT_TRUE(allEqual(output, sizeof output, 'a'));
}

TEST(lz4, short_offset_compatibility_vectors) {
    /* These blocks end immediately after their match. Besides covering the
     * repeating-pattern paths, they verify that the decoder uses exact copies
     * when the block does not provide the standard trailing-literal margin. */
    size_t n;
    uint8_t offset_two[6];
    const uint8_t a[] = {0x20, 'a', 'b', 0x02, 0x00};
    ASSERT_TRUE(dec(a, sizeof a, offset_two, 6, &n) == DE::none);
    ASSERT_TRUE(memcmp(offset_two, "ababab", 6) == 0);

    uint8_t offset_three[9];
    const uint8_t b[] = {0x32, 'a', 'b', 'c', 0x03, 0x00};
    ASSERT_TRUE(dec(b, sizeof b, offset_three, 9, &n) == DE::none);
    ASSERT_TRUE(memcmp(offset_three, "abcabcabc", 9) == 0);

    uint8_t offset_four[8];
    const uint8_t c[] = {0x40, 'a', 'b', 'c', 'd', 0x04, 0x00};
    ASSERT_TRUE(dec(c, sizeof c, offset_four, 8, &n) == DE::none);
    ASSERT_TRUE(memcmp(offset_four, "abcdabcd", 8) == 0);
}

TEST(lz4, bounded_wild_copies_are_overwritten_by_final_literals) {
    /* The first sequence's nine-byte match leaves the five final literals
     * required by the LZ4 block format. Its logical one-byte tail is copied as
     * a word and the following literal sequence overwrites the extra bytes. */
    size_t n;
    uint8_t repeated_byte[15];
    const uint8_t a[] = {0x15, 'a', 0x01, 0x00, 0x50, '1', '2', '3', '4', '5'};
    ASSERT_TRUE(dec(a, sizeof a, repeated_byte, 15, &n) == DE::none);
    ASSERT_TRUE(memcmp(repeated_byte, "aaaaaaaaaa12345", 15) == 0);

    /* Exercise the same bounded tail copy with a non-overlapping offset. */
    uint8_t word_offset[22];
    const uint8_t b[] = {0x85, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 0x08, 0x00,
                         0x50, '1', '2', '3', '4', '5'};
    ASSERT_TRUE(dec(b, sizeof b, word_offset, 22, &n) == DE::none);
    ASSERT_TRUE(memcmp(word_offset, "abcdefghabcdefgha12345", 22) == 0);
}

TEST(lz4, maximum_match_offset_compatibility_vector) {
    const size_t literal_len = 0xFFFF;
    const size_t extension_len = (literal_len - 15) / 255 + 1;
    const size_t encoded_len = 1 + extension_len + literal_len + 2;
    uint8_t *encoded = (uint8_t *)malloc(encoded_len);
    const size_t output_len = literal_len + 4;
    uint8_t *output = (uint8_t *)malloc(output_len);

    size_t op = 0;
    encoded[op] = 0xF0;
    op += 1;
    detail::writeLength(encoded, &op, literal_len - 15);
    for (size_t i = 0; i < literal_len; i++) encoded[op + i] = (uint8_t)i;
    op += literal_len;
    detail::writeIntAt<uint16_t>(encoded, op, 0xFFFF);
    op += 2;

    size_t n;
    ASSERT_TRUE(encoded_len == op);
    ASSERT_TRUE(dec(encoded, encoded_len, output, output_len, &n) == DE::none && n == output_len);
    ASSERT_TRUE(memcmp(encoded + 1 + extension_len, output + literal_len, 4) == 0);
    free(encoded);
    free(output);
}

TEST(lz4, round_trips_boundary_sized_inputs) {
    const size_t lengths[] = {0,   1,   3,   4,   5,   12,  15,  16,    19,    20,    254,
                              255, 256, 269, 270, 271, 510, 511, 512, 65535, 65536, 65537};

    for (size_t len : lengths) {
        uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
        for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(i * 31);
        ASSERT_TRUE(expectRoundTrip(buf, len));
        free(buf);
    }
}

TEST(lz4, round_trips_compressible_page_sized_inputs) {
    const size_t page_len = 400 * 1024;

    uint8_t *zeros = (uint8_t *)calloc(page_len, 1);
    ASSERT_TRUE(expectRoundTrip(zeros, page_len));
    free(zeros);

    uint8_t *structured = (uint8_t *)calloc(page_len, 1);
    for (size_t i = 0; i < page_len / 8; i++) {
        structured[i * 8] = (uint8_t)(' ' + i % 95);
        structured[i * 8 + 4] = (uint8_t)((i / 80) % 16);
    }
    ASSERT_TRUE(expectRoundTrip(structured, page_len));
    free(structured);
}

TEST(lz4, round_trips_deterministic_random_inputs) {
    zigstd::DefaultPrng prng = zigstd::DefaultPrng::init(0x4C5A3401);
    zigstd::Random random(&prng);

    for (int k = 0; k < 256; k++) {
        const size_t len = random.uintLessThan<size_t>(32 * 1024);
        uint8_t *input = (uint8_t *)malloc(len ? len : 1);
        random.bytes(input, len);
        ASSERT_TRUE(expectRoundTrip(input, len));
        free(input);
    }
}

TEST(lz4, compress_reports_short_output) {
    const char *input = "a terminal page needs enough output space";
    static HashTable table;
    uint8_t output[4];
    size_t n;
    ASSERT_TRUE(compress((const uint8_t *)input, strlen(input), output, 4, table, &n) ==
                CompressError::OutputTooSmall);
}

TEST(lz4, decompress_rejects_malformed_blocks) {
    uint8_t output[32];
    size_t n;

    const uint8_t a[] = {0xF0};
    ASSERT_TRUE(dec(a, 1, output, 32, &n) == DE::TruncatedInput);
    const uint8_t b[] = {0x10, 'a', 1};
    ASSERT_TRUE(dec(b, 3, output, 5, &n) == DE::TruncatedInput);
    const uint8_t c[] = {0x10, 'a', 0, 0};
    ASSERT_TRUE(dec(c, 4, output, 5, &n) == DE::InvalidOffset);
    const uint8_t d[] = {0x10, 'a', 2, 0};
    ASSERT_TRUE(dec(d, 4, output, 5, &n) == DE::InvalidOffset);
    const uint8_t e[] = {0x10, 'a', 1, 0};
    ASSERT_TRUE(dec(e, 4, output, 4, &n) == DE::OutputTooSmall);
    const uint8_t f[] = {0};
    ASSERT_TRUE(dec(f, 1, output, 1, &n) == DE::OutputSizeMismatch);
}

/* ---- compress/Page.zig ---- */

#include "../vt/compress/page.hpp"

typedef vt::compress::Page CPage;
typedef vt::page::Page TerminalPage;
namespace lz4 = wisp::vt::lz4;

static uint8_t *dupeBytes(const uint8_t *p, size_t len) {
    uint8_t *d = (uint8_t *)malloc(len ? len : 1);
    memcpy(d, p, len);
    return d;
}

TEST(compress_page, compressed_Page_retained_mapping_round_trip) {
    vt::page::Capacity cap(12, 9);
    cap.styles = 8;
    cap.grapheme_bytes = 128;
    cap.string_bytes = 128;
    TerminalPage resident;
    ASSERT_TRUE(TerminalPage::init(cap, &resident));
    resident.size.cols = 10;
    resident.size.rows = 7;
    resident.dirty = true;

    /* Put state in both the backing memory and the Page value. In particular,
     * the style and hyperlink sets retain live counters outside Page.memory. */
    TerminalPage::RowAndCell rac = resident.getRowAndCell(2, 3);
    *rac.cell = vt::page::Cell::init('A');
    ASSERT_TRUE(resident.appendGrapheme(rac.row, rac.cell, 0x0301) == vt::page::PageError::none);

    vt::style::Style bold;
    bold.flags.bold = true;
    vt::size::StyleCountInt style_id;
    ASSERT_TRUE(resident.styles.add((const void *)resident.memory, bold, &style_id) ==
                vt::ref_counted_set::AddError::none);
    rac.cell->setStyleId(style_id);
    rac.row->setStyled(true);

    vt::hyperlink::Hyperlink link;
    link.uri = (const uint8_t *)"https://ghostty.org/docs";
    link.uri_len = strlen("https://ghostty.org/docs");
    link.id = vt::hyperlink::Hyperlink::Id::makeExplicit((const uint8_t *)"compressed-page", 15);
    vt::hyperlink::Id hyperlink_id;
    ASSERT_TRUE(resident.insertHyperlink(link, &hyperlink_id) == vt::page::PageError::none);
    ASSERT_TRUE(resident.setHyperlink(rac.row, rac.cell, hyperlink_id) == vt::page::PageError::none);
    ASSERT_TRUE(resident.verifyIntegrity() == vt::page::PageError::none);

    uint8_t *expected = dupeBytes(resident.memory, resident.memory_len);
    uint8_t *const memory_ptr = resident.memory;
    const size_t memory_len = resident.memory_len;

    size_t required;
    ASSERT_TRUE(CPage::requiredScratch(resident.memory_len, &required) == lz4::CompressError::none);
    uint8_t *scratch = (uint8_t *)malloc(required);
    static lz4::HashTable table;
    CPage compressed;
    ASSERT_TRUE(CPage::init(zigstd::testing_allocator(), &resident, scratch, required, table, &compressed) == CPage::InitResult::ok);

    ASSERT_TRUE(memory_ptr == compressed.page.memory);
    ASSERT_TRUE(memory_len == compressed.page.memory_len);
    ASSERT_TRUE(compressed.encoded_len < resident.memory_len);
    uint8_t *expected_encoded = dupeBytes(compressed.encoded, compressed.encoded_len);

    /* Virtual-memory operations belong to PageList, so clearing the contents
     * models a successful decommit: none of the resident bytes remain. */
    memset(resident.memory, 0, resident.memory_len);

    /* A clone decodes into independent storage for read-only consumers which
     * must not change the compressed representation. In particular, it does
     * not recommit or overwrite the retained source mapping. */
    uint8_t *clone_memory = (uint8_t *)_aligned_malloc(resident.memory_len, 4096);
    TerminalPage cloned;
    ASSERT_TRUE(compressed.cloneBuf(clone_memory, resident.memory_len, &cloned) == lz4::DecompressError::none);
    ASSERT_TRUE(cloned.memory != memory_ptr);
    ASSERT_TRUE(allEqual(resident.memory, resident.memory_len, 0));
    ASSERT_TRUE(memcmp(expected_encoded, compressed.encoded, compressed.encoded_len) == 0);
    ASSERT_TRUE(memcmp(expected, cloned.memory, memory_len) == 0);
    ASSERT_TRUE(resident.size.cols == cloned.size.cols && resident.size.rows == cloned.size.rows);
    ASSERT_TRUE(cloned.dirty);
    ASSERT_TRUE(cloned.verifyIntegrity() == vt::page::PageError::none);

    TerminalPage restored;
    ASSERT_TRUE(compressed.restore(&restored) == lz4::DecompressError::none);
    ASSERT_TRUE(memory_ptr == restored.memory);
    ASSERT_TRUE(memory_len == restored.memory_len);
    ASSERT_TRUE(memcmp(expected, restored.memory, memory_len) == 0);
    ASSERT_TRUE(resident.size.cols == restored.size.cols && resident.size.rows == restored.size.rows);
    ASSERT_TRUE(restored.dirty);
    ASSERT_TRUE(restored.verifyIntegrity() == vt::page::PageError::none);

    TerminalPage::RowAndCell restored_rac = restored.getRowAndCell(2, 3);
    size_t glen = 0;
    const uint32_t *g = restored.lookupGrapheme(restored_rac.cell, &glen);
    ASSERT_TRUE(g && glen == 1 && g[0] == 0x0301);
    ASSERT_TRUE(restored.styles.get((const void *)restored.memory, restored_rac.cell->style_id())->flags.bold);

    vt::hyperlink::Id restored_hyperlink_id;
    ASSERT_TRUE(restored.lookupHyperlink(restored_rac.cell, &restored_hyperlink_id));
    const vt::hyperlink::PageEntry *restored_hyperlink =
        restored.hyperlink_set.get((const void *)restored.memory, restored_hyperlink_id);
    ASSERT_TRUE(restored_hyperlink->uri.len == link.uri_len);
    ASSERT_TRUE(memcmp(restored_hyperlink->uri.slice((const void *)restored.memory), link.uri, link.uri_len) == 0);

    _aligned_free(clone_memory);
    free(expected_encoded);
    compressed.deinit();
    free(scratch);
    free(expected);
    resident.deinit();
}

TEST(compress_page, compressed_Page_requires_the_maximum_useful_scratch) {
    TerminalPage resident;
    ASSERT_TRUE(TerminalPage::init(vt::page::Capacity(4, 4), &resident));
    uint8_t *expected = dupeBytes(resident.memory, resident.memory_len);

    size_t required, bound;
    ASSERT_TRUE(CPage::requiredScratch(resident.memory_len, &required) == lz4::CompressError::none);
    ASSERT_TRUE(resident.memory_len - (sizeof(CPage) - sizeof(TerminalPage)) - 1 == required);
    ASSERT_TRUE(compressBound(resident.memory_len, &bound) == lz4::CompressError::none);
    ASSERT_TRUE(required < bound);

    uint8_t *scratch = (uint8_t *)malloc(required - 1);
    static lz4::HashTable table;
    CPage compressed;
    ASSERT_TRUE(CPage::init(zigstd::testing_allocator(), &resident, scratch, required - 1, table, &compressed) ==
                CPage::InitResult::OutputTooSmall);
    ASSERT_TRUE(memcmp(expected, resident.memory, resident.memory_len) == 0);
    free(scratch);
    free(expected);
    resident.deinit();
}

TEST(compress_page, compressed_Page_rejects_a_representation_without_savings) {
    vt::page::Capacity cap(4, 4);
    cap.styles = 0;
    cap.grapheme_bytes = 0;
    cap.string_bytes = 0;
    cap.hyperlink_bytes = 0;
    TerminalPage resident;
    ASSERT_TRUE(TerminalPage::init(cap, &resident));

    zigstd::DefaultPrng prng = zigstd::DefaultPrng::init(0x4C5A3402);
    zigstd::Random(&prng).bytes(resident.memory, resident.memory_len);

    size_t required;
    ASSERT_TRUE(CPage::requiredScratch(resident.memory_len, &required) == lz4::CompressError::none);
    uint8_t *scratch = (uint8_t *)malloc(required);
    static lz4::HashTable table;
    /* Wisp: upstream also passes a FailingAllocator(fail_index = 0) to prove
     * that no allocation is attempted; ok_null returns before malloc. */
    CPage compressed;
    ASSERT_TRUE(CPage::init(zigstd::testing_allocator(), &resident, scratch, required, table, &compressed) == CPage::InitResult::ok_null);
    free(scratch);
    resident.deinit();
}

TEST(compress_page, compressed_Page_can_retry_after_malformed_encoded_data) {
    TerminalPage resident;
    ASSERT_TRUE(TerminalPage::init(vt::page::Capacity(8, 8), &resident));
    uint8_t *expected = dupeBytes(resident.memory, resident.memory_len);

    size_t required;
    ASSERT_TRUE(CPage::requiredScratch(resident.memory_len, &required) == lz4::CompressError::none);
    uint8_t *scratch = (uint8_t *)malloc(required);
    static lz4::HashTable table;
    CPage compressed;
    ASSERT_TRUE(CPage::init(zigstd::testing_allocator(), &resident, scratch, required, table, &compressed) == CPage::InitResult::ok);

    const size_t full_len = compressed.encoded_len;
    const uint8_t first_byte = compressed.encoded[0];
    compressed.encoded_len = 1;
    compressed.encoded[0] = 0xF0;

    uint8_t *clone_memory = (uint8_t *)_aligned_malloc(resident.memory_len, 4096);
    TerminalPage out;
    ASSERT_TRUE(compressed.cloneBuf(clone_memory, resident.memory_len, &out) == lz4::DecompressError::TruncatedInput);
    ASSERT_TRUE(compressed.restore(&out) == lz4::DecompressError::TruncatedInput);

    compressed.encoded_len = full_len;
    compressed.encoded[0] = first_byte;
    memset(resident.memory, 0, resident.memory_len);
    TerminalPage restored;
    ASSERT_TRUE(compressed.restore(&restored) == lz4::DecompressError::none);
    ASSERT_TRUE(memcmp(expected, restored.memory, resident.memory_len) == 0);

    _aligned_free(clone_memory);
    compressed.deinit();
    free(scratch);
    free(expected);
    resident.deinit();
}
