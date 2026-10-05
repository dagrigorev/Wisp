/* Transliterated from Ghostty src/terminal/compress/Page.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * A compressed terminal page which retains its resident virtual mapping.
 *
 * Terminal pages have two kinds of state: the large `Page.memory` allocation
 * and a comparatively small `Page` value containing offsets, dimensions,
 * dirty state, and allocator metadata. Not all of the latter state lives in
 * the backing memory, so preserving only the memory bytes is insufficient.
 * We preserve the complete `Page` value instead. This follows the same model
 * as `Page.cloneBuf`: page internals use offsets, so a shallow page copy remains
 * valid when its memory contents are restored at the same address.
 *
 * The resident memory is deliberately not freed by this type. `PageList`
 * keeps the virtual range allocated while asking the operating system to
 * discard its physical pages. Keeping the range has two useful properties:
 * the embedded page never contains a dangling pointer, and restoring the page
 * does not require a fallible allocation.
 *
 * The intended state transition is:
 *
 * 1. Create this value while the source page is resident.
 * 2. Ask the OS to decommit the source page's memory.
 * 3. Replace the PageList node's resident state with this value.
 * 4. To restore, recommit the retained range and call `restore`.
 * 5. After committing the resident state, call `deinit` to free the encoded
 *    bytes.
 *
 * If decommit is unavailable or fails, the caller should deinitialize the
 * compressed candidate and leave the source page resident. This type does not
 * perform any virtual-memory operations itself.
 *
 * The planned native implementation uses `MADV_DONTNEED` on Linux and pairs
 * `MADV_FREE_REUSABLE` with `MADV_FREE_REUSE` on Darwin. Targets without a
 * reliable retained-mapping decommit operation should leave page compression
 * disabled rather than free and later reallocate the resident memory.
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 * Wisp: Valgrind hooks are omitted (no Valgrind on Windows).
 */

#pragma once
#ifndef WISP_VT_COMPRESS_PAGE_HPP
#define WISP_VT_COMPRESS_PAGE_HPP

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "../page.hpp"
#include "lz4.hpp"
#include "../../zigstd/allocator.hpp"

namespace wisp {
namespace vt {
namespace compress {

typedef page::Page TerminalPage;

struct Page {
    /* Complete page metadata together with the retained resident mapping.
     *
     * The bytes in `page.memory` may have been discarded by the OS while this
     * value is in compressed state. They must not be read until `restore` has
     * successfully decoded the page. */
    TerminalPage page;

    /* Exact raw LZ4 block for `page.memory`. */
    uint8_t *encoded;
    size_t encoded_len;

    /* Allocator which owns `encoded`.
     *
     * The allocator's backing state must outlive this compressed page. Storing
     * the allocator here lets a PageList node restore and discard its compressed
     * state without needing a reference back to the PageList. */
    zigstd::Allocator alloc;

    /* Return the largest scratch buffer that can produce a useful compressed
     * representation for `raw_len` bytes.
     *
     * This is deliberately smaller than the general LZ4 compression bound. The
     * compressed representation includes an additional slice compared to a
     * resident terminal page, so an encoded block which fills more than this
     * buffer cannot reduce resident memory. Limiting the output here lets a
     * PageList borrow a standard page-pool item as scratch instead of retaining a
     * larger, compression-bound allocation.
     *
     * Callers are expected to reuse the scratch memory when practical. The
     * scratch buffer and hash table are never retained by this type. */
    static lz4::CompressError requiredScratch(size_t raw_len, size_t *out);

    /* Create a compressed representation of `source`.
     *
     * `source` is not modified and continues to own its backing allocation. The
     * scratch buffer must be at least `requiredScratch(source.memory.len)` bytes.
     * It must not overlap the source memory. On success, the returned value aliases
     * the source's resident mapping and owns only its exact-sized `encoded`
     * allocation. The allocator and its backing state must remain valid until
     * `deinit` is called.
     *
     * Returns null if retaining the compressed representation would not use less
     * memory than the resident representation. An `OutputTooSmall` result from
     * the codec also means the encoding crossed that break-even point and is
     * therefore returned as null. This comparison includes the in-memory size of
     * both representation structs but does not include allocator metadata or the
     * retained virtual address range.
     *
     * Wisp: `(Allocator.Error || lz4.CompressError)!?Page` is InitResult plus
     * out; `ok_null` is the null success. */
    enum class InitResult { ok, ok_null, OutOfMemory, InputTooLarge, OutputTooSmall };
    static InitResult init(zigstd::Allocator alloc, const TerminalPage *source, uint8_t *scratch, size_t scratch_len, lz4::HashTable &table,
                           Page *out);

    /* Free the encoded block.
     *
     * This intentionally does not free `page.memory`. The PageList node which
     * supplied the source page continues to own that pool or heap allocation. */
    void deinit() {
        alloc.free(encoded, encoded_len);
        encoded = nullptr;
        encoded_len = 0;
    }

    /* Restore the embedded terminal page into its retained resident mapping.
     *
     * The caller must recommit `page.memory` before calling this on platforms
     * which require an explicit recommit operation. The compressed value remains
     * valid whether decoding succeeds or fails, allowing the caller to retry or
     * discard it. On success the returned page aliases the same resident mapping;
     * it does not own a new allocation. */
    lz4::DecompressError restore(TerminalPage *out) const {
        TerminalPage result = page;
        size_t n;
        const lz4::DecompressError err = lz4::decompress(encoded, encoded_len, result.memory, result.memory_len, &n);
        if (err != lz4::DecompressError::none) return err;
        *out = result;
        return lz4::DecompressError::none;
    }

    /* Clone this page into caller-owned memory without restoring its mapping.
     *
     * `memory` must be at least as large as the retained page mapping. Decoding
     * writes only to that buffer, so both the discarded contents of `page.memory`
     * and this value's encoded representation remain unchanged. The returned Page
     * borrows `memory`; the caller must keep it alive for the Page's lifetime and
     * release it directly rather than calling `TerminalPage.deinit`. */
    lz4::DecompressError cloneBuf(uint8_t *memory, size_t memory_len, TerminalPage *out) const {
        assert(memory_len >= page.memory_len);
        (void)memory_len;

        /* Page internals are offsets into the backing buffer, so all metadata can
         * be copied verbatim when paired with an equally laid-out mapping. */
        TerminalPage result = page;
        result.memory = memory;
        result.memory_len = page.memory_len;
        size_t n;
        const lz4::DecompressError err = lz4::decompress(encoded, encoded_len, result.memory, result.memory_len, &n);
        if (err != lz4::DecompressError::none) return err;
        *out = result;
        return lz4::DecompressError::none;
    }
};

inline lz4::CompressError Page::requiredScratch(size_t raw_len, size_t *out) {
    /* Validate the codec's input limit before doing arithmetic based on the
     * raw size. The bound itself is intentionally not returned; see above. */
    size_t bound;
    const lz4::CompressError err = lz4::compressBound(raw_len, &bound);
    if (err != lz4::CompressError::none) return err;

    const size_t representation_overhead = sizeof(Page) - sizeof(TerminalPage);
    if (raw_len <= representation_overhead) {
        *out = 0;
        return lz4::CompressError::none;
    }

    /* The savings comparison is strict, so reserve one fewer byte than the
     * break-even encoded size. */
    *out = raw_len - representation_overhead - 1;
    return lz4::CompressError::none;
}

inline Page::InitResult Page::init(zigstd::Allocator alloc, const TerminalPage *source, uint8_t *scratch, size_t scratch_len,
                                   lz4::HashTable &table, Page *out) {
    size_t required;
    if (requiredScratch(source->memory_len, &required) != lz4::CompressError::none) return InitResult::InputTooLarge;
    if (scratch_len < required) return InitResult::OutputTooSmall;
    if (required == 0) return InitResult::ok_null;

    size_t encoded_len;
    switch (lz4::compress(source->memory, source->memory_len, scratch, required, table, &encoded_len)) {
    case lz4::CompressError::none: break;
    /* The scratch limit is the largest representation worth retaining.
     * Running out of room therefore means compression cannot save memory,
     * rather than that the caller failed to provide the documented size. */
    case lz4::CompressError::OutputTooSmall: return InitResult::ok_null;
    case lz4::CompressError::InputTooLarge: return InitResult::InputTooLarge;
    }

    /* requiredScratch already accounts for the representation structs. The
     * retained virtual range contributes no resident bytes after PageList
     * decommits it. */
    assert(sizeof(Page) + encoded_len < sizeof(TerminalPage) + source->memory_len);

    uint8_t *dup = alloc.dupe(scratch, encoded_len);
    if (!dup) return InitResult::OutOfMemory;

    out->page = *source;
    out->encoded = dup;
    out->encoded_len = encoded_len;
    out->alloc = alloc;
    return InitResult::ok;
}

} /* namespace compress */
} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_COMPRESS_PAGE_HPP */
