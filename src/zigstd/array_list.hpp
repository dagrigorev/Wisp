/* std.ArrayListUnmanaged, the pieces the terminal port uses.
 * Derived from Zig lib/std/array_list.zig
 * Copyright (c) Zig contributors — MIT License, see THIRD_PARTY_NOTICES.md
 *
 * Wisp: `Allocator.Error!void` is a bool return (false is OutOfMemory).
 * Only the operations the port calls are here. T must be trivially
 * copyable: the list moves items with memcpy, as Zig's does.
 */

#pragma once
#ifndef WISP_ZIGSTD_ARRAY_LIST_HPP
#define WISP_ZIGSTD_ARRAY_LIST_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "allocator.hpp"

namespace wisp {
namespace zigstd {

template <typename T>
struct ArrayListUnmanaged {
    T *items;
    size_t items_len;
    size_t capacity;

    /* Wisp: `.empty` */
    ArrayListUnmanaged() : items(nullptr), items_len(0), capacity(0) {}

    void deinit(Allocator alloc) {
        if (capacity > 0) alloc.freeT<T>(items, capacity);
        items = nullptr;
        items_len = 0;
        capacity = 0;
    }

    bool ensureTotalCapacity(Allocator alloc, size_t new_capacity) {
        if (capacity >= new_capacity) return true;
        /* Zig's growth factor. */
        size_t better = capacity;
        while (better < new_capacity) better = better + better / 2 + 8;
        T *new_items = alloc.allocT<T>(better);
        if (new_items == nullptr) return false;
        if (items_len > 0) memcpy((void *)new_items, (const void *)items, items_len * sizeof(T));
        if (capacity > 0) alloc.freeT<T>(items, capacity);
        items = new_items;
        capacity = better;
        return true;
    }

    bool ensureUnusedCapacity(Allocator alloc, size_t additional) {
        return ensureTotalCapacity(alloc, items_len + additional);
    }

    /* The slice of capacity beyond items_len. */
    T *unusedCapacitySlice() { return items + items_len; }
    size_t unusedCapacityLen() const { return capacity - items_len; }

    void appendAssumeCapacity(const T &item) {
        items[items_len] = item;
        items_len += 1;
    }

    bool append(Allocator alloc, const T &item) {
        if (!ensureUnusedCapacity(alloc, 1)) return false;
        appendAssumeCapacity(item);
        return true;
    }

    bool appendSlice(Allocator alloc, const T *src, size_t n) {
        if (!ensureUnusedCapacity(alloc, n)) return false;
        if (n > 0) memcpy((void *)(items + items_len), (const void *)src, n * sizeof(T));
        items_len += n;
        return true;
    }

    /* Transfer ownership of the items to the caller, who frees them with
     * the same allocator. The list is left empty. */
    bool toOwnedSlice(Allocator alloc, T **out_ptr, size_t *out_len) {
        const size_t n = items_len;
        if (n == 0) {
            deinit(alloc);
            *out_ptr = nullptr;
            *out_len = 0;
            return true;
        }
        T *owned = alloc.allocT<T>(n);
        if (owned == nullptr) return false;
        memcpy((void *)owned, (const void *)items, n * sizeof(T));
        deinit(alloc);
        *out_ptr = owned;
        *out_len = n;
        return true;
    }

    void clearRetainingCapacity() { items_len = 0; }

    void clearAndFree(Allocator alloc) { deinit(alloc); }
};

} /* namespace zigstd */
} /* namespace wisp */

#endif /* WISP_ZIGSTD_ARRAY_LIST_HPP */
