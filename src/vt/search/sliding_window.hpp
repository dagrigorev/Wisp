/* Transliterated from Ghostty src/terminal/search/sliding_window.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: departures from a mechanical mapping, all of them forced by a
 * dependency that this port either lacks or ported differently:
 *
 *   - `CircBuf(T, default)` from src/datastruct/circ_buf.zig is NOT ported
 *     in Wisp. Rather than substitute a different container, the subset
 *     this file actually uses is transliterated below as `CircBuf<T>`.
 *     Only the members SlidingWindow calls are carried over (init, deinit,
 *     len, capacity, empty, clear, iterator, ensureUnusedCapacity, resize,
 *     rotateToZero, appendAssumeCapacity, appendSliceAssumeCapacity,
 *     deleteOldest, getPtrSlice, advance, storageOffset). `append`,
 *     `first`, `last` and `Iterator.seekBy`/`reset` are not used here and
 *     are omitted.
 *
 *   - CircBuf's comptime `default` fill has no C++ analogue for a slot type
 *     that owns memory (Meta holds a std::vector). Storage slots are real
 *     C++ objects: default-constructed on allocation and assigned a fresh
 *     `T()` where upstream memsets to `default`. For `uint8_t` that is
 *     byte-identical to upstream's `0`; for `Meta` upstream's fill is
 *     `undefined`, so a value-initialized Meta is at least as defined.
 *
 *   - `std.ascii.indexOfIgnoreCase` is not ported in Wisp, so the one
 *     function used is transliterated below as `asciiIndexOfIgnoreCase`.
 *
 *   - `std.MultiArrayList(Chunk)` is a `std::vector<Chunk>`, matching the
 *     choice already made by the ported highlight.zig. The reverse fixup
 *     therefore reverses one vector of structs instead of four parallel
 *     field arrays, and indexes `.node`/`.serial`/`.start`/`.end` directly.
 *
 *   - `result.chunks = self.chunk_buf` aliases upstream's MultiArrayList.
 *     Here it copies the vector. The documented contract (chunks valid
 *     only until the next `next()`/`append()`) is unchanged and still
 *     honored; the copy is merely stricter than upstream.
 *
 *   - `std.Io.Writer.Allocating` is a `std::string`, matching the ported
 *     formatter.zig whose writes cannot fail. Upstream's `catch return
 *     error.OutOfMemory` paths around the writer therefore have no
 *     counterpart.
 *
 *   - `Meta.deinit(alloc)` takes no allocator: `cell_map` is a
 *     std::vector, which owns and releases its own storage.
 *
 *   - `Allocator.Error!T` is a bool return plus an out-parameter, as
 *     elsewhere in this port; false is OutOfMemory.
 *
 *   - `std.debug.runtime_safety` is `vt::slow_runtime_safety`. Upstream's
 *     extra `std.valgrind.runningOnValgrind()` bail-out in
 *     `assertIntegrity` has no counterpart here (Wisp has no valgrind
 *     binding); the check is already gated off outside slow-safety builds.
 */

#pragma once
#ifndef WISP_VT_SEARCH_SLIDING_WINDOW_HPP
#define WISP_VT_SEARCH_SLIDING_WINDOW_HPP

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../formatter.hpp"
#include "../highlight.hpp"

namespace wisp {
namespace vt {
namespace search {

typedef point::Coordinate Coordinate;
typedef page::Page Page;
typedef page::Row Row;

/* Wisp: std.ascii.indexOfIgnoreCase, pulled in from the Zig standard
 * library because it is the only part of std.ascii this file uses.
 * Returns false when there is no match. */
inline bool asciiIndexOfIgnoreCase(const uint8_t *haystack, size_t haystack_len, const uint8_t *needle,
                                   size_t needle_len, size_t *out) {
    if (needle_len > haystack_len) return false;
    if (needle_len == 0) {
        *out = 0;
        return true;
    }
    const size_t last = haystack_len - needle_len;
    for (size_t i = 0; i <= last; i++) {
        size_t j = 0;
        for (; j < needle_len; j++) {
            const uint8_t a = haystack[i + j];
            const uint8_t b = needle[j];
            const uint8_t la = (a >= 'A' && a <= 'Z') ? (uint8_t)(a + 32) : a;
            const uint8_t lb = (b >= 'A' && b <= 'Z') ? (uint8_t)(b + 32) : b;
            if (la != lb) break;
        }
        if (j == needle_len) {
            *out = i;
            return true;
        }
    }
    return false;
}

/* Wisp: a circular buffer containing type T.
 *
 * Transliterated from src/datastruct/circ_buf.zig's `CircBuf(T, default)`,
 * limited to the members SlidingWindow uses. See this file's header
 * comment for why it lives here and how `default` is handled. */
template <typename T>
struct CircBuf {
    // Implementation note: there's a lot of unsafe addition of usize
    // here in this implementation that can technically overflow. If someone
    // wants to fix this and make it overflow safe (use subtractions for
    // checks prior to additions) then I welcome it. In reality, we'd
    // have to be a really, really large terminal screen to even worry
    // about this so I'm punting it.

    T *storage;
    size_t storage_len;
    size_t head;
    size_t tail;

    // We could remove this and just use math with head/tail to figure
    // it out, but our usage of circular buffers stores so much data that
    // this minor overhead is not worth optimizing out.
    bool full;

    /* Wisp: `[2][]T`. */
    struct Slices {
        T *ptr[2];
        size_t len[2];
    };

    struct Iterator {
        /* Wisp: upstream holds `buf: Self` by value, so an iterator is a
         * snapshot of the storage pointer and head/tail/full at creation,
         * and copies of an iterator are independent. The snapshot is spelled
         * out field by field because a nested struct cannot hold its
         * enclosing class by value in C++. Semantics are unchanged. */
        T *buf_storage;
        size_t buf_storage_len;
        size_t buf_head;
        size_t buf_tail;
        bool buf_full;
        size_t idx;
        enum class Direction { forward, reverse };
        Direction direction;

        /* Wisp: buf.len() */
        size_t bufLen() const {
            if (buf_full) return buf_storage_len;
            if (buf_head >= buf_tail) return buf_head - buf_tail;
            return buf_storage_len - (buf_tail - buf_head);
        }

        /* Wisp: buf.capacity() */
        size_t bufCapacity() const { return buf_storage_len; }

        T *next() {
            if (idx >= bufLen()) return nullptr;

            // Get our index from the tail
            const size_t tail_idx = direction == Direction::forward ? idx : bufLen() - idx - 1;

            // Translate the tail index to a storage index
            const size_t storage_idx = (buf_tail + tail_idx) % bufCapacity();
            idx += 1;
            return &buf_storage[storage_idx];
        }
    };

    CircBuf() : storage(nullptr), storage_len(0), head(0), tail(0), full(true) {}

    /// Initialize a new circular buffer that can store size elements.
    static bool init(zigstd::Allocator alloc, size_t size, CircBuf<T> *out) {
        T *buf = nullptr;
        if (size > 0) {
            buf = alloc.allocT<T>(size);
            if (!buf) return false;
            for (size_t i = 0; i < size; i++) new (&buf[i]) T();
        }

        out->storage = buf;
        out->storage_len = size;
        out->head = 0;
        out->tail = 0;
        out->full = size == 0;
        return true;
    }

    void deinit(zigstd::Allocator alloc) {
        if (storage_len > 0) {
            for (size_t i = 0; i < storage_len; i++) storage[i].~T();
            alloc.freeT<T>(storage, storage_len);
        }
        storage = nullptr;
        storage_len = 0;
        head = 0;
        tail = 0;
        full = true;
    }

    /// Append a single value to the buffer, assuming there is capacity.
    void appendAssumeCapacity(const T &v) {
        assert(!full);
        storage[head] = v;
        head += 1;
        if (head >= storage_len) head = 0;
        full = head == tail;
    }

    /// Append a slice to the buffer.
    void appendSliceAssumeCapacity(const T *slice, size_t slice_len) {
        const Slices s = getPtrSlice(len(), slice_len);
        for (size_t i = 0; i < s.len[0]; i++) s.ptr[0][i] = slice[i];
        for (size_t i = 0; i < s.len[1]; i++) s.ptr[1][i] = slice[s.len[0] + i];
    }

    /// Clear the buffer.
    void clear() {
        head = 0;
        tail = 0;
        full = false;
    }

    /// Iterate over the circular buffer.
    Iterator iterator(typename Iterator::Direction direction) const {
        Iterator it;
        it.buf_storage = storage;
        it.buf_storage_len = storage_len;
        it.buf_head = head;
        it.buf_tail = tail;
        it.buf_full = full;
        it.idx = 0;
        it.direction = direction;
        return it;
    }

    /// Ensures that there is enough capacity to store amount more
    /// items via append.
    bool ensureUnusedCapacity(zigstd::Allocator alloc, size_t amount) {
        const size_t new_cap = len() + amount;
        if (new_cap <= capacity()) return true;
        return resize(alloc, new_cap);
    }

    /// Resize the buffer to the given size (larger or smaller).
    /// If larger, new values will be set to the default value.
    bool resize(zigstd::Allocator alloc, size_t size) {
        // Rotate to zero so it is aligned.
        rotateToZero();

        // Reallocate, this adds to the end so we're ready to go.
        const size_t prev_len = len();
        const size_t prev_cap = storage_len;
        /* Wisp: Allocator.realloc on a slot type that owns memory. The
         * port's allocator has no in-place resize, so this is the
         * alloc + move + free that upstream's realloc degrades to. New
         * slots are default-constructed, which is upstream's
         * `@memset(storage[prev_cap..], default)`. */
        T *new_storage = nullptr;
        if (size > 0) {
            new_storage = alloc.allocT<T>(size);
            if (!new_storage) return false;
            for (size_t i = 0; i < size; i++) new (&new_storage[i]) T();
        }
        const size_t retained_len = prev_len < size ? prev_len : size;
        for (size_t i = 0; i < retained_len; i++) new_storage[i] = storage[i];
        if (prev_cap > 0) {
            for (size_t i = 0; i < prev_cap; i++) storage[i].~T();
            alloc.freeT<T>(storage, prev_cap);
        }
        storage = new_storage;
        storage_len = size;

        // Fix up our head/tail for the new capacity.
        head = retained_len == size ? 0 : retained_len;
        tail = 0;
        full = retained_len == size;
        return true;
    }

    /// Rotate the data so that it is zero-aligned.
    void rotateToZero() {
        // If we're already at zero then do nothing.
        if (tail == 0) return;

        // We use std.mem.rotate to rotate our storage in-place.
        std::rotate(storage, storage + tail, storage + storage_len);

        // Then fix up our head and tail.
        head = len() % storage_len;
        tail = 0;
    }

    /// Returns if the buffer is currently empty. To check if its
    /// full, just check the "full" attribute.
    bool empty() const { return !full && head == tail; }

    /// Returns the total capacity allocated for this buffer.
    size_t capacity() const { return storage_len; }

    /// Returns the length in elements that are used.
    size_t len() const {
        if (full) return storage_len;
        if (head >= tail) return head - tail;
        return storage_len - (tail - head);
    }

    /// Delete the oldest n values from the buffer. If there are less
    /// than n values in the buffer, it'll delete everything.
    void deleteOldest(size_t n) {
        assert(n <= storage_len);

        // Special case n == 0 otherwise we will accidentally break
        // our circular buffer.
        if (n == 0) return;

        // Clear the values back to default
        const Slices s = getPtrSlice(0, n);
        for (size_t i = 0; i < s.len[0]; i++) s.ptr[0][i] = T();
        for (size_t i = 0; i < s.len[1]; i++) s.ptr[1][i] = T();

        // If we're not full, we can just advance the tail. We know
        // it'll be less than the length because otherwise we'd be full.
        const size_t l = len();
        tail += (l < n ? l : n);
        if (tail >= storage_len) tail -= storage_len;
        full = false;
    }

    /// Returns a pointer to the value at offset with the given length,
    /// and considers this full amount of data "written" if it is beyond
    /// the end of our buffer. This never "rotates" the buffer because
    /// the offset can only be within the size of the buffer.
    Slices getPtrSlice(size_t offset, size_t slice_len) {
        Slices result;

        // Special case the empty slice fast-path.
        if (slice_len == 0) {
            result.ptr[0] = nullptr;
            result.len[0] = 0;
            result.ptr[1] = nullptr;
            result.len[1] = 0;
            return result;
        }

        // Note: this assertion is very important, it hints the compiler
        // which generates ~10% faster code than without it.
        assert(offset + slice_len <= capacity());

        // End offset is the last offset (exclusive) for our slice.
        // We use exclusive because it makes the math easier and it
        // matches Zigs slicing parameterization.
        const size_t end_offset = offset + slice_len;

        // If our slice can't fit it in our length, then we need to advance.
        if (end_offset > len()) advance(end_offset - len());

        // Our start and end indexes into the storage buffer
        const size_t start_idx = storageOffset(offset);
        const size_t end_idx = storageOffset(end_offset - 1);

        // Optimistically, our data fits in one slice
        if (end_idx >= start_idx) {
            result.ptr[0] = storage + start_idx;
            result.len[0] = end_idx + 1 - start_idx;
            result.ptr[1] = storage;
            result.len[1] = 0; // So there is an empty slice
            return result;
        }

        result.ptr[0] = storage + start_idx;
        result.len[0] = storage_len - start_idx;
        result.ptr[1] = storage;
        result.len[1] = end_idx + 1;
        return result;
    }

    /// Advances the head/tail so that we can store amount.
    void advance(size_t amount) {
        assert(amount <= storage_len - len());

        // Optimistically add our amount
        head += amount;

        // If we exceeded the length of the buffer, wrap around.
        if (head >= storage_len) head = head - storage_len;

        // If we're full, we have to keep tail lined up.
        if (full) tail = head;

        // We're full if the head reached the tail. The head can never
        // pass the tail because advance asserts amount is only in
        // available space left
        full = head == tail;
    }

    /// For a given offset from zero, this returns the offset in the
    /// storage buffer where this data can be found.
    size_t storageOffset(size_t offset) const {
        assert(offset < storage_len);

        // This should be subtraction ideally to avoid overflows but
        // it would take a really, really, huge buffer to overflow.
        const size_t fits_offset = tail + offset;
        if (fits_offset < storage_len) return fits_offset;
        return fits_offset - storage_len;
    }
};

/// Searches page nodes via a sliding window. The sliding window maintains
/// the invariant that data isn't pruned until (1) we've searched it and
/// (2) we've accounted for overlaps across pages to fit the needle.
///
/// The sliding window is first initialized empty. Pages are then appended
/// in the order to search them. The sliding window supports both a forward
/// and reverse order specified via `init`. The pages should be appended
/// in the correct order matching the search direction.
///
/// All appends grow the window. The window is only pruned when a search
/// is done (positive or negative match) via `next()`.
///
/// To avoid unnecessary memory growth, the recommended usage is to
/// call `next()` until it returns null and then `append` the next page
/// and repeat the process. This will always maintain the minimum
/// required memory to search for the needle.
///
/// The caller is responsible for providing the pages and ensuring they're
/// in the proper order. The SlidingWindow itself doesn't own the pages, but
/// it will contain pointers to them in order to return selections. If any
/// pages become invalid, the caller should clear the sliding window and
/// start over.
struct SlidingWindow {
    typedef highlight::Flattened FlattenedHighlight;

    enum class Direction { forward, reverse };

    struct Meta {
        PageList::Node *node;
        uint64_t serial;
        size::CellCountInt rows;
        /* Wisp: std.ArrayList(point.Coordinate). A std::vector because
         * that is what the ported PageFormatter's point_map takes. */
        std::vector<Coordinate> cell_map;

        Meta() : node(nullptr), serial(0), rows(0), cell_map() {}

        /* Wisp: deinit(alloc) — the vector owns its storage. */
        void deinit() {
            cell_map.clear();
            cell_map.shrink_to_fit();
        }
    };

    typedef CircBuf<uint8_t> DataBuf;
    typedef CircBuf<Meta> MetaBuf;

    /// The allocator to use for all the data within this window. We
    /// store this rather than passing it around because its already
    /// part of multiple elements (eg. Meta's CellMap) and we want to
    /// ensure we always use a consistent allocator. Additionally, only
    /// a small amount of sliding windows are expected to be in use
    /// at any one time so the memory overhead isn't that large.
    zigstd::Allocator alloc;

    /// The data buffer is a circular buffer of u8 that contains the
    /// encoded page text that we can use to search for the needle.
    DataBuf data;

    /// The meta buffer is a circular buffer that contains the metadata
    /// about the pages we're searching. This usually isn't that large
    /// so callers must iterate through it to find the offset to map
    /// data to meta.
    MetaBuf meta;

    /// Buffer that can fit any amount of chunks necessary for next
    /// to never fail allocation.
    std::vector<FlattenedHighlight::Chunk> chunk_buf;

    /// Offset into data for our current state. This handles the
    /// situation where our search moved through meta[0] but didn't
    /// do enough to prune it.
    size_t data_offset; /* = 0 */

    /// The needle we're searching for. Does own the memory.
    uint8_t *needle;
    size_t needle_len;

    /// The search direction. If the direction is forward then pages should
    /// be appended in forward linked list order from the PageList. If the
    /// direction is reverse then pages should be appended in reverse order.
    ///
    /// This is important because in most cases, a reverse search is going
    /// to be more desirable to search from the end of the active area
    /// backwards so more recent data is found first. Supporting both is
    /// trivial though and will let us do more complex optimizations in the
    /// future (e.g. starting from the viewport and doing a forward/reverse
    /// concurrently from that point).
    Direction direction;

    /// A buffer to store the overlap search data. This is used to search
    /// overlaps between pages where the match starts on one page and
    /// ends on another. The length is always `needle.len * 2`.
    uint8_t *overlap_buf;
    size_t overlap_buf_len;

    SlidingWindow()
        : alloc(), data(), meta(), chunk_buf(), data_offset(0), needle(nullptr), needle_len(0),
          direction(Direction::forward), overlap_buf(nullptr), overlap_buf_len(0) {}

    /// Information copied from a page while appending it to the window.
    ///
    /// Both values remain safe after the node's preserved page is released.
    /// In particular, callers use `last_row_wrapped` to decide whether an
    /// adjacent page can contribute to a cross-page match without reading the
    /// node again.
    struct AppendResult {
        size_t content_len;
        bool last_row_wrapped;
    };

    /* Wisp: Allocator.Error!SlidingWindow. false is OutOfMemory. */
    static bool init(zigstd::Allocator alloc, Direction direction, const uint8_t *needle_unowned,
                     size_t needle_unowned_len, SlidingWindow *out) {
        DataBuf data;
        if (!DataBuf::init(alloc, 0, &data)) return false;

        MetaBuf meta;
        if (!MetaBuf::init(alloc, 0, &meta)) {
            data.deinit(alloc);
            return false;
        }

        uint8_t *needle = nullptr;
        if (needle_unowned_len > 0) {
            needle = alloc.dupe(needle_unowned, needle_unowned_len);
            if (!needle) {
                meta.deinit(alloc);
                data.deinit(alloc);
                return false;
            }
        }
        switch (direction) {
        case Direction::forward: break;
        case Direction::reverse: std::reverse(needle, needle + needle_unowned_len); break;
        }

        uint8_t *overlap_buf = nullptr;
        const size_t overlap_buf_len = needle_unowned_len * 2;
        if (overlap_buf_len > 0) {
            overlap_buf = alloc.alloc(overlap_buf_len);
            if (!overlap_buf) {
                if (needle) alloc.free(needle, needle_unowned_len);
                meta.deinit(alloc);
                data.deinit(alloc);
                return false;
            }
        }

        out->alloc = alloc;
        out->data = data;
        out->meta = meta;
        out->chunk_buf.clear();
        out->data_offset = 0;
        out->needle = needle;
        out->needle_len = needle_unowned_len;
        out->direction = direction;
        out->overlap_buf = overlap_buf;
        out->overlap_buf_len = overlap_buf_len;
        return true;
    }

    void deinit() {
        if (overlap_buf_len > 0) alloc.free(overlap_buf, overlap_buf_len);
        if (needle_len > 0) alloc.free(needle, needle_len);
        chunk_buf.clear();
        chunk_buf.shrink_to_fit();
        data.deinit(alloc);

        MetaBuf::Iterator meta_it = meta.iterator(MetaBuf::Iterator::Direction::forward);
        while (Meta *m = meta_it.next()) m->deinit();
        meta.deinit(alloc);
    }

    /// Clear all data but retain allocated capacity.
    void clearAndRetainCapacity() {
        MetaBuf::Iterator meta_it = meta.iterator(MetaBuf::Iterator::Direction::forward);
        while (Meta *m = meta_it.next()) m->deinit();
        meta.clear();
        data.clear();
        data_offset = 0;
    }

    /// Search the window for the next occurrence of the needle. As
    /// the window moves, the window will prune itself while maintaining
    /// the invariant that the window is always big enough to contain
    /// the needle.
    ///
    /// This returns a flattened highlight on a match. The
    /// flattened highlight requires allocation and is therefore more expensive
    /// than a normal selection, but it is more efficient to render since it
    /// has all the information without having to dereference pointers into
    /// the terminal state.
    ///
    /// The flattened highlight chunks reference internal memory for this
    /// sliding window and are only valid until the next call to `next()`
    /// or `append()`. If the caller wants to retain the flattened highlight
    /// then they should clone it.
    ///
    /// Wisp: ?FlattenedHighlight. false is null.
    bool next(FlattenedHighlight *out) {
        // An empty needle represents an inactive search. Searching for it
        // would otherwise produce a zero-length match, which highlight()
        // cannot represent because its end offset is inclusive.
        if (needle_len == 0) return false;

        DataBuf::Slices slices;
        {
            // If we have less data then the needle then we can't possibly match
            const size_t data_len = data.len();
            if (data_len < needle_len) return false;

            slices = data.getPtrSlice(data_offset, data_len - data_offset);
        }

        // Search the first slice for the needle.
        {
            size_t idx = 0;
            if (asciiIndexOfIgnoreCase(slices.ptr[0], slices.len[0], needle, needle_len, &idx)) {
                *out = highlight(idx, needle_len);
                return true;
            }
        }

        // Search the overlap buffer for the needle.
        if (slices.len[0] > 0 && slices.len[1] > 0) {
            // Wisp: `overlap:` labeled block; the break becomes a goto-free
            // fallthrough to the last-slice search below.
            bool matched = false;
            size_t match_start = 0;

            // Get up to needle.len - 1 bytes from each side (as much as
            // we can) and store it in the overlap buffer.
            size_t prefix_len;
            const uint8_t *prefix;
            {
                const size_t len_ = slices.len[0] < needle_len - 1 ? slices.len[0] : needle_len - 1;
                const size_t idx = slices.len[0] - len_;
                prefix = slices.ptr[0] + idx;
                prefix_len = len_;
            }
            size_t suffix_len;
            const uint8_t *suffix;
            {
                const size_t len_ = slices.len[1] < needle_len - 1 ? slices.len[1] : needle_len - 1;
                suffix = slices.ptr[1];
                suffix_len = len_;
            }
            const size_t overlap_len = prefix_len + suffix_len;
            assert(overlap_len <= overlap_buf_len);
            if (prefix_len > 0) memcpy(overlap_buf, prefix, prefix_len);
            if (suffix_len > 0) memcpy(overlap_buf + prefix_len, suffix, suffix_len);

            // Search the overlap
            size_t idx = 0;
            if (asciiIndexOfIgnoreCase(overlap_buf, overlap_len, needle, needle_len, &idx)) {
                // We found a match in the overlap buffer. We need to map the
                // index back to the data buffer in order to get our selection.
                matched = true;
                match_start = slices.len[0] - prefix_len + idx;
            }

            if (matched) {
                *out = highlight(match_start, needle_len);
                return true;
            }
        }

        // Search the last slice for the needle.
        {
            size_t idx = 0;
            if (asciiIndexOfIgnoreCase(slices.ptr[1], slices.len[1], needle, needle_len, &idx)) {
                *out = highlight(slices.len[0] + idx, needle_len);
                return true;
            }
        }

        // Special case 1-lengthed needles to delete the entire buffer.
        if (needle_len == 1) {
            clearAndRetainCapacity();
            assertIntegrity();
            return false;
        }

        // No match. We keep `needle.len - 1` bytes available to
        // handle the future overlap case.
        {
            /* Wisp: `prune:` labeled block, with the Zig while/else
             * expressed as a `found` flag. */
            /* Wisp: `break :prune` is a `prune` flag; both exits fall
             * through to the data_offset fixup after this block, exactly
             * as the labeled block does upstream. */
            MetaBuf::Iterator meta_it = meta.iterator(MetaBuf::Iterator::Direction::reverse);
            size_t saved = 0;
            bool found = false;
            while (Meta *m = meta_it.next()) {
                const size_t needed = needle_len - 1 - saved;
                if (m->cell_map.size() >= needed) {
                    // We save up to this meta. We set our data offset
                    // to exactly where it needs to be to continue
                    // searching.
                    data_offset = m->cell_map.size() - needed;
                    found = true;
                    break;
                }

                saved += m->cell_map.size();
            }
            bool prune = found;
            if (!found) {
                // If we exited the while loop naturally then we
                // never got the amount we needed and so there is
                // nothing to prune.
                assert(saved < needle_len - 1);
            }

            if (prune) {
                const size_t prune_count = meta.len() - meta_it.idx;
                if (prune_count == 0) {
                    // This can happen if we need to save up to the first
                    // meta value to retain our window.
                    prune = false;
                } else {
                    // We can now delete all the metas up to but NOT including
                    // the meta we found through meta_it.
                    meta_it = meta.iterator(MetaBuf::Iterator::Direction::forward);
                    size_t prune_data_len = 0;
                    for (size_t i = 0; i < prune_count; i++) {
                        Meta *m = meta_it.next();
                        prune_data_len += m->cell_map.size();
                        m->deinit();
                    }
                    meta.deleteOldest(prune_count);
                    data.deleteOldest(prune_data_len);
                }
            }
            (void)prune;
        }

        // Our data offset now moves to needle.len - 1 from the end so
        // that we can handle the overlap case.
        data_offset = data.len() - needle_len + 1;

        assertIntegrity();
        return false;
    }

    /// Return a flattened highlight for the given start and length.
    ///
    /// The flattened highlight can be used to render the highlight
    /// in the most efficient way because it doesn't require a terminal
    /// lock to access terminal data to compare whether some viewport
    /// matches the highlight (because it doesn't need to traverse
    /// the page nodes).
    ///
    /// The start index is assumed to be relative to the offset. i.e.
    /// index zero is actually at `self.data[self.data_offset]`. The
    /// selection will account for the offset.
    FlattenedHighlight highlight(size_t start_offset, size_t len_) {
        const size_t start = start_offset + data_offset;
        const size_t end = start + len_ - 1;
        if (slow_runtime_safety) {
            assert(start < data.len());
            assert(start + len_ <= data.len());
        }

        // Clear our previous chunk buffer to store this result
        chunk_buf.clear();
        FlattenedHighlight result = FlattenedHighlight::empty();

        // A reverse cross-page match needs the row count of the last meta in
        // search order after the chunks themselves are reversed. Snapshot it
        // while traversing Meta rather than dereferencing a PageList node after
        // the terminal lock has been released.
        Maybe<size::CellCountInt> cross_page_end_rows;

        // Go through the meta nodes to find our start.
        struct Tl {
            /// If non-null, we need to continue searching for the bottom-right.
            bool br_has;
            MetaBuf::Iterator br_it;
            size_t br_consumed;

            /// Data to prune, both are lengths.
            size_t prune_meta;
            size_t prune_data;
        };
        /* Wisp: upstream's labeled block yields `tl` in every path that does
         * not hit the `unreachable`, so the Zig value is never read
         * uninitialized. Here the precondition is an assert, which a build
         * with NDEBUG drops — so zero it, rather than leave a read of an
         * uninitialized `br_has` reachable (MSVC C4701). */
        Tl tl = Tl();
        tl.br_has = false;
        {
            /* Wisp: `tl:` labeled block. The Zig while/else's `unreachable`
             * is a precondition, kept as an assert on the found flag. */
            MetaBuf::Iterator meta_it = meta.iterator(MetaBuf::Iterator::Direction::forward);
            size_t meta_consumed = 0;
            bool found = false;
            while (Meta *m = meta_it.next()) {
                // Always increment our consumed count so that our index
                // is right for the end search if we do it.
                const size_t prior_meta_consumed = meta_consumed;
                meta_consumed += m->cell_map.size();

                // meta_i is the index we expect to find the match in the
                // cell map within this meta if it contains it.
                const size_t meta_i = start - prior_meta_consumed;

                // This meta doesn't contain the match. This means we
                // can also prune this set of data because we only look
                // forward.
                if (meta_i >= m->cell_map.size()) continue;

                // Now we look for the end. In MOST cases it is the same as
                // our starting chunk because highlights are usually small and
                // not on a boundary, so let's optimize for that.
                const size_t end_i = end - prior_meta_consumed;
                if (end_i < m->cell_map.size()) {
                    // The entire highlight is within this meta.
                    const Coordinate start_map = m->cell_map[meta_i];
                    const Coordinate end_map = m->cell_map[end_i];
                    result.top_x = start_map.x;
                    result.bot_x = end_map.x;
                    FlattenedHighlight::Chunk c;
                    c.node = m->node;
                    c.serial = m->serial;
                    c.start = (size::CellCountInt)start_map.y;
                    c.end = (size::CellCountInt)(end_map.y + 1);
                    chunk_buf.push_back(c);

                    tl.br_has = false;
                    tl.br_consumed = 0;
                    tl.prune_meta = meta_it.idx - 1;
                    tl.prune_data = prior_meta_consumed;
                    found = true;
                    break;
                } else {
                    // We found the meta that contains the start of the match
                    // only. Consume this entire node from our start offset.
                    const Coordinate map = m->cell_map[meta_i];
                    result.top_x = map.x;
                    FlattenedHighlight::Chunk c;
                    c.node = m->node;
                    c.serial = m->serial;
                    c.start = (size::CellCountInt)map.y;
                    c.end = m->rows;
                    chunk_buf.push_back(c);

                    tl.br_has = true;
                    tl.br_it = meta_it;
                    tl.br_consumed = meta_consumed;
                    tl.prune_meta = meta_it.idx - 1;
                    tl.prune_data = prior_meta_consumed;
                    found = true;
                    break;
                }
            }
            // Precondition that the start index is within the data buffer.
            assert(found);
            (void)found;
        }

        // Search for our end.
        if (tl.br_has) {
            MetaBuf::Iterator meta_it = tl.br_it;
            size_t meta_consumed = tl.br_consumed;
            bool found = false;
            while (Meta *m = meta_it.next()) {
                // meta_i is the index we expect to find the match in the
                // cell map within this meta if it contains it.
                const size_t meta_i = end - meta_consumed;
                if (meta_i >= m->cell_map.size()) {
                    // This meta doesn't contain the match. We still add it
                    // to our results because we want the full flattened list.
                    FlattenedHighlight::Chunk c;
                    c.node = m->node;
                    c.serial = m->serial;
                    c.start = 0;
                    c.end = m->rows;
                    chunk_buf.push_back(c);

                    meta_consumed += m->cell_map.size();
                    continue;
                }

                // We found it
                const Coordinate map = m->cell_map[meta_i];
                result.bot_x = map.x;
                cross_page_end_rows = Maybe<size::CellCountInt>(m->rows);
                FlattenedHighlight::Chunk c;
                c.node = m->node;
                c.serial = m->serial;
                c.start = 0;
                c.end = (size::CellCountInt)(map.y + 1);
                chunk_buf.push_back(c);
                found = true;
                break;
            }
            // Precondition that the end index is within the data buffer.
            assert(found);
            (void)found;
        }

        // Our offset into the current meta block is the start index
        // minus the amount of data fully consumed. We then add one
        // to move one past the match so we don't repeat it.
        data_offset = start - tl.prune_data + 1;

        // If we went beyond our initial meta node we can prune.
        if (tl.prune_meta > 0) {
            // Deinit all our memory in the meta blocks prior to our
            // match.
            MetaBuf::Iterator meta_it = meta.iterator(MetaBuf::Iterator::Direction::forward);
            size_t meta_consumed = 0;
            for (size_t i = 0; i < tl.prune_meta; i++) {
                Meta *m = meta_it.next();
                meta_consumed += m->cell_map.size();
                m->deinit();
            }
            (void)meta_consumed;
            if (slow_runtime_safety) {
                assert(meta_it.idx == tl.prune_meta);
                assert(meta_it.next()->node == chunk_buf[0].node);
            }
            meta.deleteOldest(tl.prune_meta);

            // Delete all the data up to our current index.
            assert(tl.prune_data > 0);
            data.deleteOldest(tl.prune_data);
        }

        switch (direction) {
        case Direction::forward: break;
        case Direction::reverse: {
            /* Wisp: upstream slices the MultiArrayList into four parallel
             * field arrays. Here one vector of structs is reversed and the
             * fields are indexed directly. */
            const size_t n = chunk_buf.size();

            if (n > 1) {
                // Reverse all our chunks. This should be pretty obvious why.
                std::reverse(chunk_buf.begin(), chunk_buf.end());

                // Now normally with forward traversal with multiple pages,
                // the suffix of the first page and the prefix of the last
                // page are used.
                //
                // For a reverse traversal, this is inverted (since the
                // pages are in reverse order we get the suffix of the last
                // page and the prefix of the first page). So we need to
                // invert this.
                //
                // We DON'T need to do this for any middle pages because
                // they always use the full page.
                //
                // This is a fixup that makes our start/end match the
                // same logic as the loops above if they were in forward
                // order.
                assert(n >= 2);
                chunk_buf[0].start = (size::CellCountInt)(chunk_buf[0].end - 1);
                chunk_buf[0].end = cross_page_end_rows.value;
                chunk_buf[n - 1].end = (size::CellCountInt)(chunk_buf[n - 1].start + 1);
                chunk_buf[n - 1].start = 0;
            } else {
                // For a single chunk, the y values are in reverse order
                // (start is the screen-end, end is the screen-start).
                // Swap them to get proper top-to-bottom order.
                const size::CellCountInt start_y = chunk_buf[0].start;
                chunk_buf[0].start = (size::CellCountInt)(chunk_buf[0].end - 1);
                chunk_buf[0].end = (size::CellCountInt)(start_y + 1);
            }

            // X values also need to be reversed since the top/bottom
            // are swapped for the nodes.
            const size::CellCountInt top_x = result.top_x;
            result.top_x = result.bot_x;
            result.bot_x = top_x;
        } break;
        }

        // Copy over our MultiArrayList so it points to the proper memory.
        result.chunks = chunk_buf;
        return result;
    }

    /// Add a new node to the sliding window. This will always grow
    /// the sliding window; data isn't pruned until it is consumed
    /// via a search (via next()).
    ///
    /// Returns the number of bytes of content added to the sliding window.
    /// The total bytes will be larger since this omits metadata, but it is
    /// an accurate measure of the text content size added.
    ///
    /// Wisp: Allocator.Error!AppendResult. false is OutOfMemory.
    bool append(PageList::Node *node, AppendResult *out) {
        PageList::Node::PreservedPage preserved;
        if (!node->pagePreservingState(alloc, &preserved)) return false;

        const Page *page = preserved.page();
        const bool last_row_wrapped = page->getRow(page->size.rows - 1)->wrap();
        const bool ok = appendPage(node, page, last_row_wrapped, out);
        preserved.deinit();
        return ok;
    }

    /// Append a node only when its last row is soft wrapped.
    ///
    /// This acquires one preserved page for both the wrap check and formatting
    /// so a compressed node is decoded at most once. It is used when loading
    /// the overlap around an otherwise complete search region.
    ///
    /// Wisp: Allocator.Error!?AppendResult. false is OutOfMemory; a true
    /// return with `!out->has` is upstream's null.
    bool appendIfWrapped(PageList::Node *node, Maybe<AppendResult> *out) {
        PageList::Node::PreservedPage preserved;
        if (!node->pagePreservingState(alloc, &preserved)) return false;

        const Page *page = preserved.page();
        const bool last_row_wrapped = page->getRow(page->size.rows - 1)->wrap();
        if (!last_row_wrapped) {
            preserved.deinit();
            *out = Maybe<AppendResult>();
            return true;
        }
        AppendResult result;
        const bool ok = appendPage(node, page, last_row_wrapped, &result);
        preserved.deinit();
        if (!ok) return false;
        *out = Maybe<AppendResult>(result);
        return true;
    }

    /// Copy one preserved page into the window's owned search buffers.
    ///
    /// No pointer into `page` may escape this function: compressed preserved
    /// pages own temporary decode storage, while resident values borrow node
    /// memory.
    bool appendPage(PageList::Node *node, const Page *page, bool last_row_wrapped, AppendResult *out) {
        // Initialize our metadata for the node.
        Meta meta_;
        meta_.node = node;
        meta_.serial = node->serial;
        meta_.rows = page->size.rows;

        // This is suboptimal but we need to encode the page once to
        // temporary memory, and then copy it into our circular buffer.
        // In the future, we should benchmark and see if we can encode
        // directly into the circular buffer.
        //
        // Wisp: std.Io.Writer.Allocating is a std::string.
        std::string encoded;

        // Encode the page into the buffer.
        formatter::Options opts(formatter::Format::plain, /* unwrap */ true);
        formatter::PageFormatter fmt = formatter::PageFormatter::init(page, &opts);
        {
            formatter::PageFormatter::PointMap pm;
            pm.map = &meta_.cell_map;
            pm.base = 0;
            fmt.point_map = Maybe<formatter::PageFormatter::PointMap>(pm);
        }
        fmt.format(&encoded);
        assert(meta_.cell_map.size() == encoded.size());

        // If the node we're adding isn't soft-wrapped, we add the
        // trailing newline.
        if (!last_row_wrapped) {
            encoded.push_back('\n');
            meta_.cell_map.push_back(meta_.cell_map.empty() ? Coordinate(0, 0) : meta_.cell_map.back());
        }

        // If our written data is empty, then there is nothing to
        // add to our data set.
        if (encoded.size() == 0) {
            assertIntegrity();
            out->content_len = 0;
            out->last_row_wrapped = last_row_wrapped;
            return true;
        }

        // Get our written data. If we're doing a reverse search then we
        // need to reverse all our encodings.
        switch (direction) {
        case Direction::forward: break;
        case Direction::reverse:
            std::reverse(encoded.begin(), encoded.end());
            std::reverse(meta_.cell_map.begin(), meta_.cell_map.end());
            break;
        }

        // Ensure our buffers are big enough to store what we need.
        if (!data.ensureUnusedCapacity(alloc, encoded.size())) {
            meta_.deinit();
            return false;
        }
        if (!meta.ensureUnusedCapacity(alloc, 1)) {
            meta_.deinit();
            return false;
        }
        chunk_buf.reserve(meta.capacity());

        // Append our new node to the circular buffer.
        data.appendSliceAssumeCapacity((const uint8_t *)encoded.data(), encoded.size());
        meta.appendAssumeCapacity(meta_);

        assertIntegrity();
        out->content_len = encoded.size();
        out->last_row_wrapped = last_row_wrapped;
        return true;
    }

    /// Only for tests!
    void testChangeNeedle(const uint8_t *new_, size_t new_len) {
        assert(new_len == needle_len);
        if (needle_len > 0) alloc.free(needle, needle_len);
        needle = alloc.dupe(new_, new_len);
        needle_len = new_len;
    }

    void assertIntegrity() const {
        if (!slow_runtime_safety) return;

        // Integrity check: verify our data matches our metadata exactly.
        MetaBuf::Iterator meta_it = meta.iterator(MetaBuf::Iterator::Direction::forward);
        size_t data_len = 0;
        while (Meta *m = meta_it.next()) data_len += m->cell_map.size();
        assert(data_len == data.len());

        // Integrity check: verify our data offset is within bounds.
        assert(data.len() == 0 || data_offset < data.len());
    }
};

} /* namespace search */
} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_SEARCH_SLIDING_WINDOW_HPP */
