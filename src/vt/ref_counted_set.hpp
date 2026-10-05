/* Transliterated from Ghostty src/terminal/ref_counted_set.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: AddError!Id is an AddError return (none on success) with the Id
 * through an out parameter; AddError!?Id adds a `*used_requested` flag for
 * the null case. The optional Context.deleted callback is detected with
 * SFINAE, as upstream's @hasDecl. Item fields are ordered by descending
 * alignment, the order Zig's auto layout uses, so @sizeOf(Item) matches.
 */

#pragma once
#ifndef WISP_VT_REF_COUNTED_SET_HPP
#define WISP_VT_REF_COUNTED_SET_HPP

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <type_traits>

#include "size.hpp"

namespace wisp {
namespace vt {
namespace ref_counted_set {

/* Wisp: @hasDecl(Context, "deleted"). */
template <typename C, typename T, typename = void>
struct HasDeleted : std::false_type {};
template <typename C, typename T>
struct HasDeleted<C, T, decltype((void)std::declval<const C &>().deleted(std::declval<const T &>()))>
    : std::true_type {};

template <typename C, typename T>
inline typename std::enable_if<HasDeleted<C, T>::value>::type callDeleted(const C &ctx, const T &v) {
    ctx.deleted(v);
}
template <typename C, typename T>
inline typename std::enable_if<!HasDeleted<C, T>::value>::type callDeleted(const C &, const T &) {}

/* Possible errors for `add` and `addWithId`. */
enum class AddError : uint8_t {
    none,

    /* There is not enough memory to add a new item.
     * Remove items or grow and reinitialize. */
    OutOfMemory,

    /* The set needs to be rehashed, as there are many dead
     * items with lower IDs which are inaccessible for reuse. */
    NeedsRehash,
};

/* A reference counted set.
 *
 * This set is created with some capacity in mind. You can determine
 * the exact memory requirement of a given capacity by calling `layout`
 * and checking the total size.
 *
 * When the set exceeds capacity, an `OutOfMemory` or `NeedsRehash` error
 * is returned from any memory-using methods. The caller is responsible
 * for determining a path forward.
 *
 * This set is reference counted. Each item in the set has an associated
 * reference count. The caller is responsible for calling release for an
 * item when it is no longer being used. Items with 0 references will be
 * kept until another item is written to their bucket. This allows items
 * to be resurrected if they are re-added before they get overwritten.
 *
 * The backing data structure of this set is an open addressed hash table
 * with linear probing and Robin Hood hashing, and a flat array of items.
 *
 * The table maps values to item IDs, which are indices in the item array
 * which contain the item's value and its reference count. Item IDs can be
 * used to efficiently access an item and update its reference count after
 * it has been added to the table, to avoid having to use the hash map to
 * look the value back up.
 *
 * ID 0 is reserved and will never be assigned.
 *
 * Parameters:
 *
 * `Context`
 *   A type containing methods to define behaviors.
 *
 *   - `fn hash(*Context, T) u64`    - Return a hash for an item.
 *
 *   - `fn eql(*Context, T, T) bool` - Check two items for equality.
 *     The first of the two items passed in is guaranteed to be from
 *     a value passed in to an `add` or `lookup` function, the second
 *     is guaranteed to be a value already resident in the set.
 *
 *   - `fn deleted(*Context, T) void` - [OPTIONAL] Deletion callback.
 *     If present, called whenever an item is finally deleted.
 *     Useful if the item has memory that needs to be freed. */
template <typename T, typename IdT, typename RefCountInt, typename ContextT>
struct RefCountedSet {
    /* Re-export these types so they can be referenced by the caller. */
    typedef IdT Id;
    typedef ContextT Context;

    static Id maxId() { return (Id)~(Id)0; }

    /* This is the max load until the set returns OutOfMemory and
     * requires more capacity.
     *
     * Experimentally, this load factor works quite well. */
    static constexpr double load_factor = 0.8125;

    /* Returns the minimum capacity needed to store `n` items,
     * accounting for the load factor and the reserved ID 0. */
    static size_t capacityForCount(size_t n) {
        if (n == 0) return 0;
        /* +1 because ID 0 is reserved, so we need at least n+1 slots. */
        return (size_t)ceil((double)(n + 1) / load_factor);
    }

    /* Set item */
    struct Item {
        struct Metadata {
            /* The bucket in the hash table where this item
             * is referenced. */
            Id bucket; /* = std.math.maxInt(Id) */

            /* The length of the probe sequence between this
             * item's starting bucket and the bucket it's in,
             * used for Robin Hood hashing. */
            Id psl; /* = 0 */

            /* The reference count for this item. */
            RefCountInt ref; /* = 0 */

            Metadata() : bucket(maxId()), psl(0), ref(0) {}
        };

        /* The value this item represents. */
        T value; /* = undefined */

        /* Metadata for this item. */
        Metadata meta; /* = .{} */

        Item() : value(), meta() {}
    };

    struct Layout {
        size_t cap;
        size_t table_cap;
        Id table_mask;
        size_t table_start;
        size_t items_start;
        size_t total_size;

        /* Returns the memory layout for the given base offset and
         * desired capacity. The layout can be used by the caller to
         * determine how much memory to allocate, and the layout must
         * be used to initialize the set so that the set knows all
         * the offsets for the various buffers.
         *
         * The capacity passed for cap will be used for the hash table,
         * which has a load factor of `0.8125` (13/16), so the number of
         * items which can actually be stored in the set will be smaller.
         *
         * The laid out capacity will be at least `cap`, but may be higher,
         * since it is rounded up to the next power of 2 for efficiency.
         *
         * The returned layout `cap` property will be 1 more than the number
         * of items that the set can actually store, since ID 0 is reserved. */
        static Layout init(size_t cap) {
            /* assert(cap <= @as(usize, @intCast(std.math.maxInt(Id))) + 1) */

            Layout l;
            /* Zero-cap set is valid, return special case */
            if (cap == 0) {
                l.cap = 0;
                l.table_cap = 0;
                l.table_mask = 0;
                l.table_start = 0;
                l.items_start = 0;
                l.total_size = 0;
                return l;
            }

            size_t table_cap = 1;
            while (table_cap < cap) table_cap <<= 1;
            const size_t items_cap = (size_t)(load_factor * (double)table_cap);

            const Id table_mask = (Id)(table_cap - 1);

            const size_t table_start = 0;
            const size_t table_end = table_start + table_cap * sizeof(Id);

            const size_t items_start = (table_end + alignof(Item) - 1) / alignof(Item) * alignof(Item);
            const size_t items_end = items_start + items_cap * sizeof(Item);

            const size_t total_size = items_end;

            l.cap = items_cap;
            l.table_cap = table_cap;
            l.table_mask = table_mask;
            l.table_start = table_start;
            l.items_start = items_start;
            l.total_size = total_size;
            return l;
        }
    };

    static const size_t base_align_ = alignof(Context) > alignof(Layout)
        ? alignof(Context) : alignof(Layout);
    static const size_t base_align2_ = alignof(Item) > alignof(Id) ? alignof(Item) : alignof(Id);
    static const size_t base_align = base_align_ > base_align2_ ? base_align_ : base_align2_;

    /* A hash table of item indices */
    size::Offset<Id> table;

    /* By keeping track of the max probe sequence length
     * we can bail out early when looking up values that
     * aren't present. */
    Id max_psl; /* = 0 */

    /* We keep track of how many items have a PSL of any
     * given length, so that we can shrink max_psl when
     * we delete items.
     *
     * A probe sequence of length 32 or more is astronomically
     * unlikely. Roughly a (1/table_cap)^32 -- with any normal
     * table capacity that is so unlikely that it's not worth
     * handling.
     *
     * However, that assumes a uniform hash function, which
     * is not guaranteed and can be subverted with a crafted
     * input. We handle this gracefully by returning an error
     * anywhere where we're about to insert if there's any
     * item with a PSL in the last slot of the stats array. */
    Id psl_stats[32]; /* = @splat(0) */

    /* The backing store of items */
    size::Offset<Item> items;

    /* The number of living items currently stored in the set. */
    size_t living; /* = 0 */

    /* The next index to store an item at.
     * Id 0 is reserved for unused items. */
    Id next_id; /* = 1 */

    Layout layout;

    /* An instance of the context structure. */
    Context context;

    RefCountedSet() : table(), max_psl(0), items(), living(0), next_id(1), layout(), context() {
        memset(psl_stats, 0, sizeof(psl_stats));
    }

    static RefCountedSet init(size::OffsetBuf base, Layout l, Context context) {
        const size::Offset<Id> t = base.member<Id>(l.table_start);
        const size::Offset<Item> it = base.member<Item>(l.items_start);

        memset(t.ptr(base), 0, l.table_cap * sizeof(Id));
        Item *ip = it.ptr(base);
        for (size_t i = 0; i < l.cap; i++) ip[i] = Item();

        return initFromParts(t, it, l, context);
    }

    /* Like `init`, but for backing memory that the caller guarantees
     * is already zero-filled (e.g. fresh OS pages). This writes
     * nothing to the backing buffer, so the OS pages behind the table
     * and items stay untouched until the first `add`.
     *
     * Behavior is undefined if the backing memory is not zero. */
    static RefCountedSet initAssumeZeroed(size::OffsetBuf base, Layout l, Context context) {
        return initFromParts(base.member<Id>(l.table_start), base.member<Item>(l.items_start), l, context);
    }

    static RefCountedSet initFromParts(size::Offset<Id> t, size::Offset<Item> it, Layout l, Context context) {
        RefCountedSet s;
        s.table = t;
        s.items = it;
        s.layout = l;
        s.context = context;
        return s;
    }

    /* Add an item to the set if not present and increment its ref count.
     *
     * Returns the item's ID.
     *
     * If the set has no more room, then an OutOfMemory error is returned. */
    template <typename B>
    AddError add(const B &base, const T &value, Id *out) {
        return addContext(base, value, context, out);
    }
    template <typename B>
    AddError addContext(const B &base, const T &value, const Context &ctx, Id *out) {
        Item *its = items.ptr(base);

        /* Trim dead items from the end of the list. */
        while (next_id > 1 && its[next_id - 1].meta.ref == 0) {
            next_id -= 1;
            deleteItem(base, next_id, ctx);
        }

        /* If the item already exists, return it. */
        Id id;
        if (lookupContext(base, value, ctx, &id)) {
            /* Notify the context that the value is "deleted" because
             * we're reusing the existing value in the set. This allows
             * callers to clean up any resources associated with the value. */
            callDeleted(ctx, value);

            its[id].meta.ref += 1;
            *out = id;
            return AddError::none;
        }

        /* While it should be statistically impossible to exceed the
         * bounds of `psl_stats`, the hash function is not perfect and
         * in such a case we want to remain stable. If we're about to
         * insert an item and there's something with a PSL of `len - 1`,
         * we may end up with a PSL of `len` which would exceed the bounds.
         * In such a case, we claim to be out of memory. */
        if (psl_stats[32 - 1] > 0) return AddError::OutOfMemory;

        /* If the item doesn't exist, we need an available ID. */
        if (next_id >= layout.cap) {
            /* Arbitrarily chosen, threshold for rehashing.
             * If less than 90% of currently allocated IDs
             * correspond to living items, we should rehash.
             * Otherwise, claim we're out of memory because
             * we assume that we'll end up running out of
             * memory or rehashing again very soon if we
             * rehash with only a few IDs left. */
            const double rehash_threshold = 0.9;
            if (living < (Id)((double)layout.cap * rehash_threshold)) {
                return AddError::NeedsRehash;
            }

            /* If we don't have at least 10% dead items then
             * we claim we're out of memory. */
            return AddError::OutOfMemory;
        }

        id = insert(base, value, next_id, ctx);
        its[id].meta.ref += 1;
        /* assert(items[id].meta.ref == 1) */
        living += 1;

        /* Its possible insert returns a different ID by reusing a
         * dead item so we only need to update next id if we used it. */
        if (id == next_id) next_id += 1;

        *out = id;
        return AddError::none;
    }

    /* Add an item to the set if not present and increment its
     * ref count. If possible, use the provided ID.
     *
     * Returns the item's ID, or null if the provided ID was used.
     *
     * If the set has no more room, then an OutOfMemory error is returned.
     *
     * Wisp: *is_null is true for the null return; otherwise *out holds the
     * returned ID. */
    template <typename B>
    AddError addWithId(const B &base, const T &value, Id id, bool *is_null, Id *out) {
        return addWithIdContext(base, value, id, context, is_null, out);
    }
    template <typename B>
    AddError addWithIdContext(const B &base, const T &value, Id id, const Context &ctx,
                              bool *is_null, Id *out) {
        Item *its = items.ptr(base);

        /* assert(id > 0) */
        *is_null = false;

        if (id < next_id) {
            if (its[id].meta.ref == 0) {
                /* Requested ID is dead, but if the value exists not under
                 * another ID then ref count that and increase the ID. */
                Id existing_id;
                if (lookupContext(base, value, ctx, &existing_id)) {
                    /* Notify the context that the value is "deleted"
                     * because we're reusing the existing value in the
                     * set. This allows callers to clean up any
                     * resources associated with the value. */
                    callDeleted(ctx, value);

                    its[existing_id].meta.ref += 1;
                    *out = existing_id;
                    return AddError::none;
                }

                /* See comment in `addContext` for details. */
                if (psl_stats[32 - 1] > 0) return AddError::OutOfMemory;

                deleteItem(base, id, ctx);
                const Id added_id = insert(base, value, id, ctx);

                its[added_id].meta.ref += 1;

                living += 1;

                if (added_id == id) {
                    *is_null = true;
                } else {
                    *out = added_id;
                }
                return AddError::none;
            } else if (ctx.eql(value, its[id].value)) {
                /* Notify the context that the value is "deleted" because
                 * we're reusing the existing value in the set. This allows
                 * callers to clean up any resources associated with the value. */
                callDeleted(ctx, value);

                its[id].meta.ref += 1;

                *is_null = true;
                return AddError::none;
            }
        }

        return addContext(base, value, ctx, out);
    }

    /* Increment an item's reference count by 1.
     *
     * Asserts that the item's reference count is greater than 0. */
    template <typename B>
    void use(const B &base, Id id) const {
        /* assert(id > 0); assert(id < self.layout.cap) */
        Item *item = &items.ptr(base)[id];

        /* If `use` is being called on an item with 0 references, then
         * either someone forgot to call it before, released too early
         * or lied about releasing. In any case something is wrong and
         * shouldn't be allowed.
         * assert(item.meta.ref > 0) */

        item->meta.ref += 1;
    }

    /* Increment an item's reference count by a specified number.
     *
     * Asserts that the item's reference count is greater than 0. */
    template <typename B>
    void useMultiple(const B &base, Id id, RefCountInt n) const {
        /* assert(id > 0); assert(id < self.layout.cap) */
        Item *item = &items.ptr(base)[id];
        /* assert(item.meta.ref > 0) */
        item->meta.ref += n;
    }

    /* Get an item by its ID without incrementing its reference count.
     *
     * Asserts that the item's reference count is greater than 0. */
    template <typename B>
    T *get(const B &base, Id id) const {
        /* assert(id > 0); assert(id < self.layout.cap) */
        Item *item = &items.ptr(base)[id];
        /* assert(item.meta.ref > 0) */
        return &item->value;
    }

    /* Releases a reference to an item by its ID.
     *
     * Asserts that the item's reference count is greater than 0. */
    template <typename B>
    void release(const B &base, Id id) {
        /* assert(id > 0); assert(id < self.layout.cap) */
        Item *item = &items.ptr(base)[id];

        /* assert(item.meta.ref > 0) */
        item->meta.ref -= 1;
        if (item->meta.ref == 0) living -= 1;
    }

    /* Release a specified number of references to an item by its ID.
     *
     * Asserts that the item's reference count is at least `n`. */
    template <typename B>
    void releaseMultiple(const B &base, Id id, Id n) {
        /* assert(id > 0); assert(id < self.layout.cap) */
        Item *item = &items.ptr(base)[id];

        /* assert(item.meta.ref >= n) */
        item->meta.ref -= n;

        if (item->meta.ref == 0) living -= 1;
    }

    /* Get the ref count for an item by its ID. */
    template <typename B>
    RefCountInt refCount(const B &base, Id id) const {
        /* assert(id > 0); assert(id < self.layout.cap) */
        return items.ptr(base)[id].meta.ref;
    }

    /* Get the current number of non-dead items in the set. */
    size_t count() const { return living; }

    /* A live entry returned by `Iterator`. */
    struct Entry {
        Id id;
        T *value_ptr;
    };

    /* Iterates live entries in ascending ID order.
     *
     * Released entries whose reference count reached zero are skipped.
     * Any mutation of the set invalidates the iterator. */
    struct Iterator {
        Item *items;
        Id id; /* = 1 */
        Id end;

        bool next(Entry *out) {
            while (id < end) {
                const Id cur = id;
                id += 1;

                Item *item = &items[cur];
                if (item->meta.ref == 0) continue;

                out->id = cur;
                out->value_ptr = &item->value;
                return true;
            }

            return false;
        }
    };

    /* Return an iterator over the live entries in this set. */
    template <typename B>
    Iterator iterator(const B &base) const {
        Iterator it;
        it.items = items.ptr(base);
        it.id = 1;
        it.end = next_id;
        return it;
    }

    /* Delete an item, removing any references from
     * the table, and freeing its ID to be reused. */
    template <typename B>
    void deleteItem(const B &base, Id id, const Context &ctx) {
        Id *tbl = table.ptr(base);
        Item *its = items.ptr(base);

        const Item item = its[id];

        if (item.meta.bucket > layout.table_cap) return;

        /* assert(table[item.meta.bucket] == id) */

        /* Inform the context struct that we're
         * deleting the dead item's value for good. */
        callDeleted(ctx, item.value);

        psl_stats[item.meta.psl] -= 1;
        tbl[item.meta.bucket] = 0;
        its[id] = Item();

        Id p = item.meta.bucket;
        Id n = (Id)((Id)(p + 1) & layout.table_mask);

        while (tbl[n] != 0 && its[tbl[n]].meta.psl > 0) {
            its[tbl[n]].meta.bucket = p;
            psl_stats[its[tbl[n]].meta.psl] -= 1;
            its[tbl[n]].meta.psl -= 1;
            psl_stats[its[tbl[n]].meta.psl] += 1;
            tbl[p] = tbl[n];
            p = n;
            n = (Id)((Id)(p + 1) & layout.table_mask);
        }

        while (max_psl > 0 && psl_stats[max_psl] == 0) {
            max_psl -= 1;
        }

        tbl[p] = 0;
    }

    /* Find an item in the table and return its ID.
     * If the item does not exist in the table, null is returned. */
    template <typename B>
    bool lookup(const B &base, const T &value, Id *out) const {
        return lookupContext(base, value, context, out);
    }
    template <typename B>
    bool lookupContext(const B &base, const T &value, const Context &ctx, Id *out) const {
        /* A zero-capacity set (a valid special case of Layout.init)
         * contains nothing and has a zero-size table, so we can't
         * probe it: table[0] would read whatever memory follows the
         * set in the backing buffer. */
        if (layout.table_cap == 0) return false;

        const Id *tbl = table.ptr(base);
        const Item *its = items.ptr(base);

        const uint64_t hash = ctx.hash(value);

        for (size_t i = 0; i < (size_t)max_psl + 1; i++) {
            const size_t p = (size_t)((hash + i) & layout.table_mask);
            const Id id = tbl[p];

            /* Empty bucket, our item cannot have probed to
             * any point after this, meaning it's not present. */
            if (id == 0) return false;

            const Item &item = its[id];

            /* An item with a shorter probe sequence length would never
             * end up in the middle of another sequence, since it would
             * be swapped out if inserted before the new sequence, and
             * would not be swapped in if inserted afterwards.
             *
             * As such, our item cannot be present. */
            if (item.meta.psl < i) return false;

            /* If the item is a part of the same probe sequence,
             * we make sure it's not dead and then check to see
             * if it matches the value we're looking for. */
            if (item.meta.psl == i && item.meta.ref > 0 && ctx.eql(value, item.value)) {
                *out = id;
                return true;
            }
        }

        return false;
    }

    /* Insert the given value into the hash table with the given ID.
     *
     * If runtime safety is enabled, asserts that
     * the value is not already present in the table. */
    template <typename B>
    Id insert(const B &base, const T &value, Id new_id, const Context &ctx) {
        Id *tbl = table.ptr(base);
        Item *its = items.ptr(base);

        /* The new item that we'll put in to the table. */
        Item new_item;
        new_item.value = value;
        new_item.meta.psl = 0;
        new_item.meta.ref = 0;

        const uint64_t hash = ctx.hash(value);

        Id held_id = new_id;
        Item *held_item = &new_item;

        Id chosen_id = new_id;

        for (size_t i = 0; i < layout.table_cap - 1; i++) {
            const Id p = (Id)((hash + i) & layout.table_mask);
            const Id id = tbl[p];

            /* Empty bucket, put our held item in to it and break. */
            if (id == 0) {
                tbl[p] = held_id;
                held_item->meta.bucket = p;
                psl_stats[held_item->meta.psl] += 1;
                if (held_item->meta.psl > max_psl) max_psl = held_item->meta.psl;
                break;
            }

            Item *item = &its[id];

            /* If there's a dead item then we resurrect it
             * for our value so that we can reuse its ID,
             * unless its ID is greater than the one we're
             * given (i.e. prefer smaller IDs). */
            if (item->meta.ref == 0) {
                /* Dead items aren't super common relative
                 * to other places to insert/swap the held
                 * item in to the set.
                 *
                 * Inform the context struct that we're
                 * deleting the dead item's value for good. */
                callDeleted(ctx, item->value);

                /* Reap the dead item. */
                psl_stats[item->meta.psl] -= 1;
                *item = Item();

                /* Only resurrect this item if it has a
                 * smaller id than the one we were given. */
                if (id < new_id) chosen_id = id;

                /* Put the currently held item in to the
                 * bucket of the item that we just reaped. */
                tbl[p] = held_id;
                held_item->meta.bucket = p;
                psl_stats[held_item->meta.psl] += 1;
                if (held_item->meta.psl > max_psl) max_psl = held_item->meta.psl;

                break;
            }

            /* If this item has a lower PSL, or has equal PSL and lower ref
             * count, then we swap it out with our held item. By doing this,
             * items with high reference counts are prioritized for earlier
             * placement. The assumption is that an item which has a higher
             * reference count will be accessed more frequently, so we want
             * to minimize the time it takes to find it. */
            if (item->meta.psl < held_item->meta.psl ||
                (item->meta.psl == held_item->meta.psl && item->meta.ref < held_item->meta.ref)) {
                /* Put our held item in the bucket. */
                tbl[p] = held_id;
                held_item->meta.bucket = p;
                psl_stats[held_item->meta.psl] += 1;
                if (held_item->meta.psl > max_psl) max_psl = held_item->meta.psl;

                /* Pick up the item that has a lower PSL. */
                held_id = id;
                held_item = item;
                psl_stats[item->meta.psl] -= 1;
            }

            /* Advance to the next probe position for our held item. */
            held_item->meta.psl += 1;
        }

        /* Our chosen ID may have changed if we decided
         * to reuse a dead item's ID, so we make sure
         * the chosen bucket contains the correct ID. */
        tbl[new_item.meta.bucket] = chosen_id;

        /* Finally place our new item in to our array. */
        its[chosen_id] = new_item;

        return chosen_id;
    }
};

} /* namespace ref_counted_set */
} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_REF_COUNTED_SET_HPP */
