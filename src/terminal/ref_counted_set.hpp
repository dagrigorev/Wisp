/* Ported from Ghostty src/terminal/ref_counted_set.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * A reference counted set, used to intern per-page styles and hyperlinks so
 * that a cell stores a small ID rather than a whole attribute record.
 *
 * Built from an open-addressed hash table with linear probing and Robin Hood
 * hashing, plus a flat array of items. The table maps a value to an item ID,
 * which indexes the item array; holding the ID lets a caller adjust a refcount
 * later without hashing the value again. ID 0 is reserved and never assigned.
 *
 * Items with zero references are kept until their bucket is needed by
 * something else, so a value re-added before that point is resurrected rather
 * than reinserted.
 *
 * Everything is addressed by offset from a base pointer so the backing buffer
 * stays relocatable, as with the rest of the page structures.
 *
 * Porting notes:
 *   - Zig error unions become an AddResult status plus an out parameter.
 *   - Zig's optional `deleted` context method, detected there with @hasDecl,
 *     is required here. Give it an empty body if the value owns nothing.
 *   - assert_integrity is compiled in and callable. Upstream keeps the
 *     equivalent behind a disabled flag because it is O(n) per mutation; it is
 *     exposed here because the tests use it as the correctness net, this
 *     structure having no external specification to check against.
 */

#pragma once
#ifndef WISP_TERMINAL_REF_COUNTED_SET_HPP
#define WISP_TERMINAL_REF_COUNTED_SET_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "size.hpp"

namespace wisp {
namespace terminal {

/* The load factor at which the table reports itself full. */
#define WISP_RCS_LOAD_FACTOR 0.8125

/* Probe sequences longer than this are astronomically unlikely with a uniform
 * hash — roughly (1/table_cap)^32 — but a crafted input can subvert that, so
 * an insert that would exceed the array reports out-of-memory instead. */
static const size_t RCS_MAX_PSL = 32;

enum class AddResult {
    ok,

    /* No room. Remove items, or grow and reinitialize. */
    out_of_memory,

    /* Many dead items hold low IDs that cannot be reused in place. The caller
     * should rehash into a fresh set. */
    needs_rehash,
};

template <typename T, typename IdT, typename RefCountInt, typename Context>
struct RefCountedSet {
    typedef IdT Id;

    /* Alignment the backing memory must start on. Published so a container
     * laying this out among other structures can align the region correctly. */
    static const size_t base_align =
        alignof(IdT) > alignof(T) ? alignof(IdT) : alignof(T);

    struct Metadata {
        /* The bucket this item is referenced from. */
        Id bucket;
        /* Distance from the item's ideal bucket to its actual one. */
        Id psl;
        RefCountInt ref;

        Metadata() : bucket((Id)-1), psl(0), ref(0) {}
    };

    struct Item {
        T value;
        Metadata meta;
        Item() : value(), meta() {}
    };

    struct Layout {
        size_t cap;          /* one more than the storable item count */
        size_t table_cap;
        Id     table_mask;
        size_t table_start;
        size_t items_start;
        size_t total_size;

        static Layout init(size_t cap) {
            Layout l;

            /* A zero-capacity set is valid and holds nothing. */
            if (cap == 0) {
                l.cap = 0; l.table_cap = 0; l.table_mask = 0;
                l.table_start = 0; l.items_start = 0; l.total_size = 0;
                return l;
            }

            size_t table_cap = 1;
            while (table_cap < cap) table_cap <<= 1;

            const size_t items_cap = (size_t)(WISP_RCS_LOAD_FACTOR * (double)table_cap);

            const size_t table_start = 0;
            const size_t table_end = table_start + table_cap * sizeof(Id);
            const size_t items_start = align_forward(table_end, alignof(Item));
            const size_t items_end = items_start + items_cap * sizeof(Item);

            l.cap = items_cap;
            l.table_cap = table_cap;
            l.table_mask = (Id)(table_cap - 1);
            l.table_start = table_start;
            l.items_start = items_start;
            l.total_size = items_end;
            return l;
        }
    };

    /* Minimum capacity needed to store n items, allowing for the load factor
     * and the reserved ID 0. */
    static size_t capacity_for_count(size_t n) {
        if (n == 0) return 0;
        const double need = (double)(n + 1) / WISP_RCS_LOAD_FACTOR;
        size_t r = (size_t)need;
        if ((double)r < need) r++;
        return r;
    }

    Offset<Id>   table;
    Id           max_psl;
    Id           psl_stats[RCS_MAX_PSL];
    Offset<Item> items;
    size_t       living;
    Id           next_id;
    Layout       layout;
    Context      context;

    /* Initialize over a buffer, clearing the table and items. */
    static RefCountedSet init(OffsetBuf base, const Layout &l, Context ctx) {
        Offset<Id> table = base.member<Id>(l.table_start);
        Offset<Item> items = base.member<Item>(l.items_start);

        memset(table.ptr(base), 0, l.table_cap * sizeof(Id));
        Item *ip = items.ptr(base);
        for (size_t i = 0; i < l.cap; i++) ip[i] = Item();

        return init_from_parts(table, items, l, ctx);
    }

    /* Like init, for memory the caller guarantees is already zeroed. Writes
     * nothing, so the pages behind the table and items stay untouched until
     * the first add. Behavior is undefined if the memory is not zero. */
    static RefCountedSet init_assume_zeroed(OffsetBuf base, const Layout &l, Context ctx) {
        return init_from_parts(base.member<Id>(l.table_start),
                               base.member<Item>(l.items_start), l, ctx);
    }

    static RefCountedSet init_from_parts(Offset<Id> table, Offset<Item> items,
                                         const Layout &l, Context ctx) {
        RefCountedSet s;
        s.table = table;
        s.items = items;
        s.layout = l;
        s.context = ctx;
        s.max_psl = 0;
        s.living = 0;
        s.next_id = 1;
        memset(s.psl_stats, 0, sizeof(s.psl_stats));
        return s;
    }

    /* ─── queries ────────────────────────────────────────────────────────── */

    size_t count() const { return living; }

    template <typename Base>
    T *get(Base base, Id id) const {
        assert(id > 0);
        assert((size_t)id < layout.cap);
        Item *its = items.ptr(base);
        assert(its[id].meta.ref > 0);
        return &its[id].value;
    }

    template <typename Base>
    RefCountInt ref_count(Base base, Id id) const {
        assert(id > 0);
        assert((size_t)id < layout.cap);
        return items.ptr(base)[id].meta.ref;
    }

    /* Find a value's ID, or 0 if absent. */
    template <typename Base>
    Id lookup(Base base, const T &value) const {
        /* A zero-capacity set has a zero-size table, so probing it would read
         * whatever follows the set in the backing buffer. */
        if (layout.table_cap == 0) return 0;

        const Id *tbl = table.ptr(base);
        const Item *its = items.ptr(base);

        const uint64_t hash = context.hash(value);

        for (size_t i = 0; i <= (size_t)max_psl; i++) {
            const size_t p = (size_t)((hash + i) & layout.table_mask);
            const Id id = tbl[p];

            /* An empty bucket ends the probe sequence: the value could not
             * have probed past it. */
            if (id == 0) return 0;

            const Item &item = its[id];

            /* An item with a shorter PSL never sits mid-sequence — it would
             * have been swapped out on the way in — so the value is absent. */
            if ((size_t)item.meta.psl < i) return 0;

            if ((size_t)item.meta.psl == i && item.meta.ref > 0 &&
                context.eql(value, item.value)) {
                return id;
            }
        }

        return 0;
    }

    /* ─── mutation ───────────────────────────────────────────────────────── */

    /* Add a value if absent and take a reference. */
    template <typename Base>
    AddResult add(Base base, const T &value, Id *out_id) {
        Item *its = items.ptr(base);

        /* Trim dead items off the end so their IDs can be reused. */
        while (next_id > 1 && its[next_id - 1].meta.ref == 0) {
            next_id--;
            delete_item(base, next_id);
        }

        if (Id existing = lookup(base, value)) {
            /* The caller's value is being discarded in favor of the resident
             * one, so let the context release anything it owns. */
            context.deleted(value);
            its[existing].meta.ref += 1;
            *out_id = existing;
            return AddResult::ok;
        }

        /* Refuse rather than overflow psl_stats. See RCS_MAX_PSL. */
        if (psl_stats[RCS_MAX_PSL - 1] > 0) return AddResult::out_of_memory;

        if ((size_t)next_id >= layout.cap) {
            /* If under 90% of allocated IDs are live, a rehash reclaims
             * enough to be worth it. Otherwise say we are full, because
             * rehashing with only a few IDs free would just recur. */
            if (living < (size_t)(0.9 * (double)layout.cap)) {
                return AddResult::needs_rehash;
            }
            return AddResult::out_of_memory;
        }

        const Id id = insert(base, value, next_id);
        its[id].meta.ref += 1;
        assert(its[id].meta.ref == 1);
        living++;

        /* insert may have reused a dead item's ID instead of the one offered. */
        if (id == next_id) next_id++;

        *out_id = id;
        return AddResult::ok;
    }

    /* Add a value, preferring the given ID. On success *out_id is the ID used,
     * which may differ from the request. */
    template <typename Base>
    AddResult add_with_id(Base base, const T &value, Id id, Id *out_id) {
        Item *its = items.ptr(base);
        assert(id > 0);

        if (id < next_id) {
            if (its[id].meta.ref == 0) {
                /* The requested ID is dead. If the value is already resident
                 * under a different ID, use that one instead. */
                if (Id existing = lookup(base, value)) {
                    context.deleted(value);
                    its[existing].meta.ref += 1;
                    *out_id = existing;
                    return AddResult::ok;
                }

                if (psl_stats[RCS_MAX_PSL - 1] > 0) return AddResult::out_of_memory;

                delete_item(base, id);
                const Id added = insert(base, value, id);
                its[added].meta.ref += 1;
                living++;
                *out_id = added;
                return AddResult::ok;
            }

            if (context.eql(value, its[id].value)) {
                context.deleted(value);
                its[id].meta.ref += 1;
                *out_id = id;
                return AddResult::ok;
            }
        }

        return add(base, value, out_id);
    }

    /* Take another reference. The item must already have one. */
    template <typename Base>
    void use(Base base, Id id) {
        assert(id > 0);
        assert((size_t)id < layout.cap);
        Item *its = items.ptr(base);
        /* Calling use on a dead item means a missing acquire or a double
         * release somewhere; either way the refcount is already wrong. */
        assert(its[id].meta.ref > 0);
        its[id].meta.ref += 1;
    }

    /* Release one reference. */
    template <typename Base>
    void release(Base base, Id id) {
        assert(id > 0);
        assert((size_t)id < layout.cap);
        Item *its = items.ptr(base);
        assert(its[id].meta.ref > 0);
        its[id].meta.ref -= 1;
        if (its[id].meta.ref == 0) living--;
    }

    template <typename Base>
    void release_multiple(Base base, Id id, RefCountInt n) {
        assert(id > 0);
        assert((size_t)id < layout.cap);
        Item *its = items.ptr(base);
        assert(its[id].meta.ref >= n);
        its[id].meta.ref -= n;
        if (its[id].meta.ref == 0) living--;
    }

    /* ─── iteration ──────────────────────────────────────────────────────── */

    /* Visits live entries in ascending ID order. Any mutation invalidates it. */
    struct Iterator {
        Item *its;
        Id    id;
        Id    end;

        bool next(Id *out_id, T **out_value) {
            while (id < end) {
                const Id cur = id;
                id++;
                if (its[cur].meta.ref == 0) continue;
                *out_id = cur;
                *out_value = &its[cur].value;
                return true;
            }
            return false;
        }
    };

    template <typename Base>
    Iterator iterator(Base base) const {
        Iterator it;
        it.its = items.ptr(base);
        it.id = 1;
        it.end = next_id;
        return it;
    }

    /* ─── internals ──────────────────────────────────────────────────────── */

    /* Remove an item, unlink it from the table and free its ID. */
    template <typename Base>
    void delete_item(Base base, Id id) {
        Id *tbl = table.ptr(base);
        Item *its = items.ptr(base);

        const Item item = its[id];

        /* Never made it into the table. */
        if ((size_t)item.meta.bucket > layout.table_cap) return;

        assert(tbl[item.meta.bucket] == id);

        context.deleted(item.value);

        psl_stats[item.meta.psl] -= 1;
        tbl[item.meta.bucket] = 0;
        its[id] = Item();

        /* Backward-shift deletion: pull each following member of the probe
         * chain back one bucket so no chain is left with a hole in it, which
         * would make everything past the hole unfindable. */
        Id p = item.meta.bucket;
        Id n = (Id)((p + 1) & layout.table_mask);

        while (tbl[n] != 0 && its[tbl[n]].meta.psl > 0) {
            its[tbl[n]].meta.bucket = p;
            psl_stats[its[tbl[n]].meta.psl] -= 1;
            its[tbl[n]].meta.psl -= 1;
            psl_stats[its[tbl[n]].meta.psl] += 1;
            tbl[p] = tbl[n];
            p = n;
            n = (Id)((p + 1) & layout.table_mask);
        }

        while (max_psl > 0 && psl_stats[max_psl] == 0) max_psl--;

        tbl[p] = 0;
    }

    /* Insert a value under new_id, or under a dead item's smaller ID if one is
     * displaced along the way. Returns the ID actually used. */
    template <typename Base>
    Id insert(Base base, const T &value, Id new_id) {
        Id *tbl = table.ptr(base);
        Item *its = items.ptr(base);

        Item new_item;
        new_item.value = value;
        new_item.meta.psl = 0;
        new_item.meta.ref = 0;

        const uint64_t hash = context.hash(value);

        Id    held_id = new_id;
        Item *held = &new_item;
        Id    chosen_id = new_id;

        /* Reap a dead item from this value's probe run first, through
         * delete_item so the run is repaired by backward shift.
         *
         * Upstream instead reaps inline in the loop below: it parks the held
         * item in the dead slot and breaks. That leaves the rest of the run
         * untouched, so the PSLs along it are no longer non-decreasing — and
         * lookup's early exit (psl < i) relies on exactly that ordering, so
         * live items past the reaped slot stop being findable.
         *
         * It needs a pathological hash to show up, which is why it survives in
         * practice: real style hashing keeps runs 1-2 buckets long. A test
         * here with every value colliding into four buckets reproduces it,
         * losing 10 of 16 live items on a single add. Doing the reap first
         * costs one extra probe pass in a rare path and keeps the invariant
         * that everything else depends on. */
        /* A run can hold several dead items, and each delete_item shifts the
         * rest of it back, so rescan from the start after every reap rather
         * than continuing through a run that has moved underneath us. */
        for (;;) {
            Id dead = 0;
            for (size_t i = 0; i < layout.table_cap; i++) {
                const Id p = (Id)((hash + i) & layout.table_mask);
                const Id id = tbl[p];
                if (id == 0) break;          /* run ends */
                if (its[id].meta.ref == 0) { dead = id; break; }
            }
            if (dead == 0) break;

            /* Prefer the smallest reusable ID so the item array stays dense
             * at the front. */
            if (dead < chosen_id) chosen_id = dead;
            delete_item(base, dead);
        }

        for (size_t i = 0; i < layout.table_cap - 1; i++) {
            const Id p = (Id)((hash + i) & layout.table_mask);
            const Id id = tbl[p];

            /* Empty bucket — park the held item and stop. */
            if (id == 0) {
                tbl[p] = held_id;
                held->meta.bucket = p;
                psl_stats[held->meta.psl] += 1;
                if (held->meta.psl > max_psl) max_psl = held->meta.psl;
                break;
            }

            Item *item = &its[id];

            /* Dead items were already reaped above, so anything still here is
             * live and the run only terminates on an empty bucket. */
            assert(item->meta.ref > 0);

            /* Robin Hood: a resident that is closer to home than the held item
             * gets displaced. Ties break on refcount, so frequently used items
             * drift toward the front of their chain and are found sooner. */
            if (item->meta.psl < held->meta.psl ||
                (item->meta.psl == held->meta.psl && item->meta.ref < held->meta.ref)) {
                tbl[p] = held_id;
                held->meta.bucket = p;
                psl_stats[held->meta.psl] += 1;
                if (held->meta.psl > max_psl) max_psl = held->meta.psl;

                held_id = id;
                held = item;
                psl_stats[item->meta.psl] -= 1;
            }

            held->meta.psl += 1;
        }

        /* The chosen ID may have changed by reusing a dead item's, so make the
         * bucket point at the right one. */
        tbl[new_item.meta.bucket] = chosen_id;

        its[chosen_id] = new_item;
        return chosen_id;
    }

    /* ─── integrity ──────────────────────────────────────────────────────── */

    /* Verify the structure's invariants. O(n); intended for tests.
     *
     * Upstream keeps the equivalent behind a disabled flag for performance.
     * It is the only specification this structure has, so the tests call it
     * after every mutation. */
    template <typename Base>
    bool check_integrity(Base base) const {
        const Id *tbl = table.ptr(base);
        const Item *its = items.ptr(base);

        Id seen[RCS_MAX_PSL];
        memset(seen, 0, sizeof(seen));

        /* Every item that claims a bucket must be the one the table holds
         * there. ID 0 is reserved and never assigned, so skip it. */
        for (size_t id = 1; id < (size_t)next_id; id++) {
            const Item &item = its[id];
            if (item.meta.bucket == (Id)-1) continue;
            if ((size_t)item.meta.bucket >= layout.table_cap) return false;
            if (tbl[item.meta.bucket] != (Id)id) return false;
            if ((size_t)item.meta.psl >= RCS_MAX_PSL) return false;
            seen[item.meta.psl] += 1;
        }

        if (memcmp(seen, psl_stats, sizeof(seen)) != 0) return false;

        memset(seen, 0, sizeof(seen));

        /* Every occupied bucket must agree with its item, and the item must
         * actually be reachable at its recorded distance from home. This is
         * the invariant the probe loops depend on. */
        for (size_t bucket = 0; bucket < layout.table_cap; bucket++) {
            const Id id = tbl[bucket];
            if (id == 0) continue;

            const Item &item = its[id];
            if (item.meta.bucket == (Id)-1) continue;
            if ((size_t)item.meta.bucket != bucket) return false;

            const uint64_t hash = context.hash(item.value);
            const size_t p = (size_t)((hash + item.meta.psl) & layout.table_mask);
            if (p != bucket) return false;

            seen[item.meta.psl] += 1;
        }

        if (memcmp(seen, psl_stats, sizeof(seen)) != 0) return false;

        /* max_psl must actually bound the distribution, or lookup would stop
         * probing before reaching a resident item. */
        for (size_t i = (size_t)max_psl + 1; i < RCS_MAX_PSL; i++) {
            if (psl_stats[i] != 0) return false;
        }

        return true;
    }

    /* Every live item must be findable by lookup.
     *
     * This is separate from check_integrity because the two can disagree: the
     * bucket and PSL bookkeeping can be perfectly self-consistent while
     * lookup's early-exit still walks past an item. Reachability is the
     * property callers actually depend on, so it is checked on its own.
     */
    template <typename Base>
    bool check_reachable(Base base) const {
        const Item *its = items.ptr(base);
        for (size_t id = 1; id < (size_t)next_id; id++) {
            if (its[id].meta.ref == 0) continue;
            if (lookup(base, its[id].value) != (Id)id) return false;
        }
        return true;
    }
};

} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_REF_COUNTED_SET_HPP */
