/* Transliterated from Ghostty src/datastruct/untouched_pool.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 * Wisp: `Allocator.Error!T` is a null return / false. std.ArrayList(ItemPtr)
 * is FreeList below (items + len + capacity from the gpa, grown with
 * ArrayList's growth formula).
 */

#pragma once
#ifndef WISP_DATASTRUCT_UNTOUCHED_POOL_HPP
#define WISP_DATASTRUCT_UNTOUCHED_POOL_HPP

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "../zigstd/allocator.hpp"

namespace wisp {
namespace datastruct {

/* std.heap.ArenaAllocator.ResetMode */
struct PoolResetMode {
    enum class Tag { free_all, retain_capacity, retain_with_limit } tag;
    size_t limit;
    static PoolResetMode free_all() { PoolResetMode m = {Tag::free_all, 0}; return m; }
    static PoolResetMode retain_capacity() { PoolResetMode m = {Tag::retain_capacity, 0}; return m; }
    static PoolResetMode retain_with_limit(size_t l) { PoolResetMode m = {Tag::retain_with_limit, l}; return m; }
};

/* A fixed-size item pool whose bookkeeping never touches item memory.
 *
 * std.heap.MemoryPool keeps its free list inside the items intrusively,
 * meaning it has to touch allocated item memory. For memory that is
 * demand-paged, that write forces the OS to create a physical page
 * for nothing (to mark it free!).
 *
 * This pool keeps the free list in a separate array allocated from a
 * general-purpose allocator and allocates every item individually from
 * the item allocator with the requested alignment. The pool never reads
 * or writes an item until it is needed.
 *
 * Contracts:
 *
 *   - The pool never zeroes. Callers that need zeroed items must use an
 *     item allocator that returns zeroed memory and must zero (or
 *     decommit) an item before destroy().
 *   - destroy() never allocates and never fails: create() reserves a
 *     free-list slot for every live item before allocating a new one.
 *   - The pool tracks free items only. Every item must be returned with
 *     destroy() or release() before reset() or deinit(), or it leaks.
 *     This is asserted in safe builds.
 *
 * The tradeoff is that this isn't as fast as std.heap.MemoryPool, its
 * not as cache friendly. But benchmarks show that the cost is minimal
 * and if the tradeoff of not touching the memory is important, then
 * this pays off. */

template <typename Item, size_t alignment>
struct UntouchedPool {
    static const size_t item_size = sizeof(Item);
    static const size_t item_alignment = alignment > alignof(Item) ? alignment : alignof(Item);
    typedef Item *ItemPtr;

    typedef PoolResetMode ResetMode;

    struct FreeList {
        ItemPtr *items;
        size_t len;
        size_t capacity;

        ItemPtr pop() {
            if (len == 0) return nullptr;
            len -= 1;
            return items[len];
        }
        void appendAssumeCapacity(ItemPtr item) {
            assert(len < capacity);
            items[len++] = item;
        }
        bool ensureTotalCapacityPrecise(const zigstd::Allocator &gpa, size_t new_capacity) {
            if (capacity >= new_capacity) return true;
            ItemPtr *n = gpa.allocT<ItemPtr>(new_capacity);
            if (!n) return false;
            if (len) memcpy(n, items, len * sizeof(ItemPtr));
            if (capacity) gpa.freeT<ItemPtr>(items, capacity);
            items = n;
            capacity = new_capacity;
            return true;
        }
        /* ArrayList.ensureTotalCapacity: growCapacity(current, minimum). */
        bool ensureTotalCapacity(const zigstd::Allocator &gpa, size_t new_capacity) {
            if (capacity >= new_capacity) return true;
            const size_t init_capacity = 64 / sizeof(ItemPtr) > 1 ? 64 / sizeof(ItemPtr) : 1;
            size_t n = capacity;
            for (;;) {
                n += n / 2 + init_capacity;
                if (n >= new_capacity) break;
            }
            return ensureTotalCapacityPrecise(gpa, n);
        }
        void deinit(const zigstd::Allocator &gpa) {
            if (capacity) gpa.freeT<ItemPtr>(items, capacity);
            items = nullptr;
            len = capacity = 0;
        }
    };

    /* The allocator items are allocated from. */
    zigstd::Allocator allocator;

    /* The general-purpose allocator for the free list. */
    zigstd::Allocator gpa;

    /* Free items. The most recently destroyed item is handed out first. */
    FreeList free;

    /* Number of items currently allocated from the item allocator,
     * free or in use. `free.capacity >= live` always holds so that
     * destroy() never has to grow the free list. */
    size_t live;

    /* Create a pool with `preheat` items already allocated.
     * Wisp: false is OutOfMemory (and `out` is deinitialized). */
    static bool initCapacity(zigstd::Allocator gpa_, zigstd::Allocator item_alloc, size_t preheat,
                             UntouchedPool *out) {
        UntouchedPool &self = *out;
        self.allocator = item_alloc;
        self.gpa = gpa_;
        self.free.items = nullptr;
        self.free.len = 0;
        self.free.capacity = 0;
        self.live = 0;

        if (!self.free.ensureTotalCapacityPrecise(gpa_, preheat)) {
            self.deinit();
            return false;
        }
        for (size_t i = 0; i < preheat; i++) {
            ItemPtr item = self.allocItem();
            if (!item) {
                self.deinit();
                return false;
            }
            self.free.appendAssumeCapacity(item);
        }

        return true;
    }

    /* Free all items and the free list. Every item must have been
     * returned with destroy() or release(). */
    void deinit() {
        assert(live == free.len);
        for (size_t i = 0; i < free.len; i++) allocator.free((uint8_t *)free.items[i], item_size, item_alignment);
        free.deinit(gpa);
    }

    /* Free items so that the retained free items fit `mode`. Every
     * item must have been returned with destroy() or release().
     *
     * This mirrors std.heap.MemoryPool.reset; it always succeeds
     * (returns true) because nothing is reallocated. */
    bool reset(ResetMode mode) {
        assert(live == free.len);
        size_t retain_bytes;
        switch (mode.tag) {
        case ResetMode::Tag::free_all: retain_bytes = 0; break;
        case ResetMode::Tag::retain_capacity: return true;
        default: retain_bytes = mode.limit; break;
        }

        const size_t retain_items = retain_bytes / item_size;
        while (free.len > retain_items) {
            ItemPtr item = free.pop();
            allocator.free((uint8_t *)item, item_size, item_alignment);
            live -= 1;
        }

        return true;
    }

    /* Get an item. This pops a free item without touching it or,
     * when none is free, allocates a new one from the item
     * allocator. Wisp: null is OutOfMemory. */
    ItemPtr create() {
        if (ItemPtr item = free.pop()) return item;

        /* Reserve the free-list slot for the new item before
         * allocating it so that destroy() can never fail. */
        if (!free.ensureTotalCapacity(gpa, live + 1)) return nullptr;
        return allocItem();
    }

    /* Return an item to the free list for reuse. The item is not
     * modified or zeroed so it is up to the caller. */
    void destroy(ItemPtr item) {
        assert(free.len < live);
        free.appendAssumeCapacity(item);
    }

    /* Return an item straight to the item allocator instead of the
     * free list. This is for teardown paths that would otherwise
     * have to zero an item only for it to be freed moments later. */
    void release(ItemPtr item) {
        assert(free.len < live);
        allocator.free((uint8_t *)item, item_size, item_alignment);
        live -= 1;
    }

    ItemPtr allocItem() {
        assert(free.capacity > live);
        uint8_t *memory = allocator.alignedAlloc(item_size, item_alignment);
        if (!memory) return nullptr;
        live += 1;
        return (ItemPtr)memory;
    }
};

} /* namespace datastruct */
} /* namespace wisp */

#endif /* WISP_DATASTRUCT_UNTOUCHED_POOL_HPP */
