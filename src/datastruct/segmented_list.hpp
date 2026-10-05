/* Transliterated from Ghostty src/datastruct/segmented_list.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's own note: "Code taken from 0.15.2 `std.segmented_list` (MIT
 * license)." Zig's standard library is MIT licensed as well.
 *
 * TRANSLITERATION, see src/terminal/parser.hpp for the general mapping.
 * Specific to this file:
 *
 *   Allocator            malloc/free. Elements are raw storage, as Zig's
 *                        allocator.alloc(T, n) returns undefined memory, so T
 *                        must be trivially copyable.
 *   allocator.resize     treated as always failing, so shrinkCapacity always
 *                        takes the fresh-allocation branch. Both branches
 *                        leave the list in the same observable state.
 *   Allocator.Error      a bool return; false means out of memory
 *
 * Comments are upstream's unless marked "Wisp:".
 */

#pragma once
#ifndef WISP_DATASTRUCT_SEGMENTED_LIST_HPP
#define WISP_DATASTRUCT_SEGMENTED_LIST_HPP

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace wisp {
namespace datastruct {

/* std.math.log2_int: floor(log2(x)) for x > 0. */
inline size_t log2_int(size_t x) {
    size_t r = 0;
    while (x >>= 1) r++;
    return r;
}

/* TODO look into why this std.math function was changed in
 * fc9430f56798a53f9393a697f4ccd6bf9981b970. */
inline size_t log2_int_ceil(size_t x) {
    assert(x != 0);
    const size_t log2_val = log2_int(x);
    if (((size_t)1 << log2_val) == x) return log2_val;
    return log2_val + 1;
}

/* Imagine that `fn at(self: *Self, index: usize) &T` is a customer asking for
 * a box from a warehouse, based on a flat array, boxes ordered from 0 to
 * N - 1. But the warehouse actually stores boxes in shelves of increasing
 * powers of 2 sizes. So when the customer requests a box index, we have to
 * translate it to shelf index and box index within that shelf.
 *
 * With a preallocated shelf, which must be a power of 2, the equations are:
 *
 * shelf_index = floor(log2(customer_index + prealloc)) - log2(prealloc) - 1
 * shelf_count = ceil(log2(box_count + prealloc)) - log2(prealloc) - 1
 * box_index = customer_index + prealloc - 2 ** (log2(prealloc) + 1 + shelf)
 * shelf_size = prealloc * 2 ** (shelf_index + 1)
 *
 * Wisp: upstream's comment draws the warehouse out in full; the equations
 * are what the code below implements. */

/* This is a stack data structure where pointers to indexes have the same
 * lifetime as the data structure itself, unlike ArrayList where append()
 * invalidates all existing element pointers. The tradeoff is that elements
 * are not guaranteed to be contiguous. For that, use ArrayList. Note however
 * that most elements are contiguous, making this data structure
 * cache-friendly.
 *
 * Because it never has to copy elements from an old location to a new
 * location, it does not require its elements to be copyable, and it avoids
 * wasting memory when backed by an ArenaAllocator. Note that the append()
 * and pop() convenience methods perform a copy, but you can instead use
 * addOne(), at(), setCapacity(), and shrinkCapacity() to avoid copying
 * items.
 *
 * This data structure has O(1) append and O(1) pop.
 *
 * It supports preallocated elements, making it especially well suited when
 * the expected maximum size is small. `prealloc_item_count` must be 0, or a
 * power of 2. */
template <typename T, size_t prealloc_item_count>
struct SegmentedList {
    typedef size_t ShelfIndex;

    static const size_t prealloc_count = prealloc_item_count;

    /* we don't use the prealloc_exp constant when prealloc_item_count is 0
     * but lazy-init may still be triggered by other code so supply a value */
    static ShelfIndex prealloc_exp() {
        return prealloc_item_count == 0 ? 0 : log2_int(prealloc_item_count);
    }

    /* Wisp: a zero-length array is not C++, so the prealloc segment always
     * has at least one slot; with prealloc_item_count == 0 it is never
     * used. */
    T      prealloc_segment[prealloc_item_count ? prealloc_item_count : 1];
    T    **dynamic_segments;
    size_t dynamic_segments_len;
    size_t len;

    SegmentedList() : dynamic_segments(nullptr), dynamic_segments_len(0), len(0) {}

    void deinit() {
        freeShelves(dynamic_segments_len, 0);
        free(dynamic_segments);
        dynamic_segments = nullptr;
        dynamic_segments_len = 0;
        len = 0;
    }

    T *at(size_t i) {
        assert(i < len);
        return uncheckedAt(i);
    }

    const T *at(size_t i) const {
        assert(i < len);
        return uncheckedAt(i);
    }

    size_t count() const { return len; }

    bool append(const T &item) {
        T *new_item_ptr = addOne();
        if (!new_item_ptr) return false;
        *new_item_ptr = item;
        return true;
    }

    bool appendSlice(const T *items, size_t n) {
        for (size_t i = 0; i < n; i++) {
            if (!append(items[i])) return false;
        }
        return true;
    }

    /* Wisp: ?T is the bool return plus *out. */
    bool pop(T *out) {
        if (len == 0) return false;

        const size_t index = len - 1;
        *out = *uncheckedAt(index);
        len = index;
        return true;
    }

    /* Wisp: null means out of memory. */
    T *addOne() {
        const size_t new_length = len + 1;
        if (!growCapacity(new_length)) return nullptr;
        T *result = uncheckedAt(len);
        len = new_length;
        return result;
    }

    /* Reduce length to `new_len`.
     * Invalidates pointers for the elements at index new_len and beyond. */
    void shrinkRetainingCapacity(size_t new_len) {
        assert(new_len <= len);
        len = new_len;
    }

    /* Invalidates all element pointers. */
    void clearRetainingCapacity() { len = 0; }

    /* Invalidates all element pointers. */
    void clearAndFree() {
        setCapacity(0);
        len = 0;
    }

    /* Grows or shrinks capacity to match usage.
     * TODO update this and related methods to match the conventions set by
     * ArrayList */
    bool setCapacity(size_t new_capacity) {
        if (prealloc_item_count != 0) {
            if (new_capacity <= ((size_t)1 << (prealloc_exp() + dynamic_segments_len))) {
                shrinkCapacity(new_capacity);
                return true;
            }
        }
        return growCapacity(new_capacity);
    }

    /* Only grows capacity, or retains current capacity. */
    bool growCapacity(size_t new_capacity) {
        const ShelfIndex new_cap_shelf_count = shelfCount(new_capacity);
        const ShelfIndex old_shelf_count = dynamic_segments_len;
        if (new_cap_shelf_count <= old_shelf_count) return true;

        T **new_dynamic_segments =
            (T **)malloc(sizeof(T *) * new_cap_shelf_count);
        if (!new_dynamic_segments) return false;

        ShelfIndex i = 0;
        while (i < old_shelf_count) {
            new_dynamic_segments[i] = dynamic_segments[i];
            i += 1;
        }
        while (i < new_cap_shelf_count) {
            T *shelf = (T *)malloc(sizeof(T) * shelfSize(i));
            if (!shelf) {
                /* errdefer: free what this call allocated, and the new
                 * segment list. */
                while (i > old_shelf_count) {
                    i -= 1;
                    free(new_dynamic_segments[i]);
                }
                free(new_dynamic_segments);
                return false;
            }
            new_dynamic_segments[i] = shelf;
            i += 1;
        }

        free(dynamic_segments);
        dynamic_segments = new_dynamic_segments;
        dynamic_segments_len = new_cap_shelf_count;
        return true;
    }

    /* Only shrinks capacity or retains current capacity.
     * It may fail to reduce the capacity in which case the capacity will
     * remain unchanged. */
    void shrinkCapacity(size_t new_capacity) {
        if (new_capacity <= prealloc_item_count) {
            const size_t l = dynamic_segments_len;
            freeShelves(l, 0);
            free(dynamic_segments);
            dynamic_segments = nullptr;
            dynamic_segments_len = 0;
            return;
        }

        const ShelfIndex new_cap_shelf_count = shelfCount(new_capacity);
        const ShelfIndex old_shelf_count = dynamic_segments_len;
        assert(new_cap_shelf_count <= old_shelf_count);
        if (new_cap_shelf_count == old_shelf_count) return;

        /* freeShelves() must be called before resizing the dynamic
         * segments, but we don't know if resizing the dynamic segments
         * will work until we try it. So we must allocate a fresh memory
         * buffer in order to reduce capacity. */
        T **new_dynamic_segments =
            (T **)malloc(sizeof(T *) * (new_cap_shelf_count ? new_cap_shelf_count : 1));
        if (!new_dynamic_segments) return;
        freeShelves(old_shelf_count, new_cap_shelf_count);

        /* Wisp: allocator.resize is treated as failing; see the header.
         * Good thing we allocated that new memory slice. */
        memcpy(new_dynamic_segments, dynamic_segments,
               sizeof(T *) * new_cap_shelf_count);
        free(dynamic_segments);
        dynamic_segments = new_dynamic_segments;
        dynamic_segments_len = new_cap_shelf_count;
    }

    void shrink(size_t new_len) {
        assert(new_len <= len);
        /* TODO take advantage of the new realloc semantics */
        len = new_len;
    }

    void writeToSlice(T *dest, size_t dest_len, size_t start) {
        const size_t end = start + dest_len;
        assert(end <= len);

        size_t i = start;
        if (end <= prealloc_item_count) {
            memcpy(dest + (i - start), prealloc_segment + i, sizeof(T) * (end - i));
            return;
        } else if (i < prealloc_item_count) {
            memcpy(dest + (i - start), prealloc_segment + i,
                   sizeof(T) * (prealloc_item_count - i));
            i = prealloc_item_count;
        }

        while (i < end) {
            const ShelfIndex shelf_index = shelfIndex(i);
            const size_t copy_start = boxIndex(i, shelf_index);
            const size_t want_end = copy_start + end - i;
            const size_t size = shelfSize(shelf_index);
            const size_t copy_end = size < want_end ? size : want_end;
            memcpy(dest + (i - start), dynamic_segments[shelf_index] + copy_start,
                   sizeof(T) * (copy_end - copy_start));
            i += (copy_end - copy_start);
        }
    }

    T *uncheckedAt(size_t index) {
        if (index < prealloc_item_count) return &prealloc_segment[index];
        const ShelfIndex shelf_index = shelfIndex(index);
        const size_t box_index = boxIndex(index, shelf_index);
        return &dynamic_segments[shelf_index][box_index];
    }

    const T *uncheckedAt(size_t index) const {
        return const_cast<SegmentedList *>(this)->uncheckedAt(index);
    }

    /* ─── iterators ──────────────────────────────────────────────────────── */

    template <typename ListPtr, typename ElementPtr>
    struct BaseIterator {
        ListPtr    list;
        size_t     index;
        size_t     box_index;
        ShelfIndex shelf_index;
        size_t     shelf_size;

        /* Wisp: ?ElementPtr is a nullable pointer. */
        ElementPtr next() {
            if (index >= list->len) return nullptr;
            if (index < prealloc_item_count) {
                ElementPtr ptr = &list->prealloc_segment[index];
                index += 1;
                if (index == prealloc_item_count) {
                    box_index = 0;
                    shelf_index = 0;
                    shelf_size = prealloc_item_count * 2;
                }
                return ptr;
            }

            ElementPtr ptr = &list->dynamic_segments[shelf_index][box_index];
            index += 1;
            box_index += 1;
            if (box_index == shelf_size) {
                shelf_index += 1;
                box_index = 0;
                shelf_size *= 2;
            }
            return ptr;
        }

        ElementPtr prev() {
            if (index == 0) return nullptr;

            index -= 1;
            if (index < prealloc_item_count) return &list->prealloc_segment[index];

            if (box_index == 0) {
                shelf_index -= 1;
                shelf_size /= 2;
                box_index = shelf_size - 1;
            } else {
                box_index -= 1;
            }

            return &list->dynamic_segments[shelf_index][box_index];
        }

        ElementPtr peek() {
            if (index >= list->len) return nullptr;
            if (index < prealloc_item_count) return &list->prealloc_segment[index];

            return &list->dynamic_segments[shelf_index][box_index];
        }

        void set(size_t i) {
            index = i;
            if (i < prealloc_item_count) return;
            shelf_index = shelfIndex(i);
            box_index = boxIndex(i, shelf_index);
            shelf_size = shelfSize(shelf_index);
        }
    };

    typedef BaseIterator<SegmentedList *, T *> Iterator;
    typedef BaseIterator<const SegmentedList *, const T *> ConstIterator;

    Iterator iterator(size_t start_index) {
        Iterator it;
        it.list = this;
        it.index = 0;
        it.box_index = 0;
        it.shelf_index = 0;
        it.shelf_size = 0;
        it.set(start_index);
        return it;
    }

    ConstIterator constIterator(size_t start_index) const {
        ConstIterator it;
        it.list = this;
        it.index = 0;
        it.box_index = 0;
        it.shelf_index = 0;
        it.shelf_size = 0;
        it.set(start_index);
        return it;
    }

private:
    static ShelfIndex shelfCount(size_t box_count) {
        if (prealloc_item_count == 0) return log2_int_ceil(box_count + 1);
        return log2_int_ceil(box_count + prealloc_item_count) - prealloc_exp() - 1;
    }

    static size_t shelfSize(ShelfIndex shelf_index) {
        if (prealloc_item_count == 0) return (size_t)1 << shelf_index;
        return (size_t)1 << (shelf_index + (prealloc_exp() + 1));
    }

    static ShelfIndex shelfIndex(size_t list_index) {
        if (prealloc_item_count == 0) return log2_int(list_index + 1);
        return log2_int(list_index + prealloc_item_count) - prealloc_exp() - 1;
    }

    static size_t boxIndex(size_t list_index, ShelfIndex shelf_index) {
        if (prealloc_item_count == 0) {
            return (list_index + 1) - ((size_t)1 << shelf_index);
        }
        return list_index + prealloc_item_count -
               ((size_t)1 << ((prealloc_exp() + 1) + shelf_index));
    }

    void freeShelves(ShelfIndex from_count, ShelfIndex to_count) {
        ShelfIndex i = from_count;
        while (i != to_count) {
            i -= 1;
            free(dynamic_segments[i]);
        }
    }
};

} /* namespace datastruct */
} /* namespace wisp */

#endif /* WISP_DATASTRUCT_SEGMENTED_LIST_HPP */
