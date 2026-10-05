/* Tests for src/terminal/ref_counted_set.hpp.
 *
 * Related to Ghostty src/terminal/ref_counted_set.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * This structure has no external specification to check against, so the
 * strategy here is to assert its own invariants after every mutation via
 * check_integrity, and to drive it with a deliberately bad hash so that the
 * Robin Hood displacement and backward-shift deletion paths — the parts most
 * likely to be gotten wrong — are exercised rather than bypassed.
 */

#include "test_helpers.h"
#include "ref_counted_set.hpp"

using namespace wisp::terminal;

/* A well-behaved context over uint32_t values. */
struct GoodCtx {
    uint64_t hash(const uint32_t &v) const {
        /* splitmix64 finalizer — good avalanche, so probe chains stay short. */
        uint64_t x = v + 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    }
    bool eql(const uint32_t &a, const uint32_t &b) const { return a == b; }
    void deleted(const uint32_t &) const {}
};

/* A deliberately terrible context: every value collides into one of four
 * buckets, forcing long probe sequences and heavy displacement. */
struct BadCtx {
    uint64_t hash(const uint32_t &v) const { return v % 4; }
    bool eql(const uint32_t &a, const uint32_t &b) const { return a == b; }
    void deleted(const uint32_t &) const {}
};

typedef RefCountedSet<uint32_t, uint16_t, uint16_t, GoodCtx> GoodSet;
typedef RefCountedSet<uint32_t, uint16_t, uint16_t, BadCtx>  BadSet;

/* 64-bit aligned backing store. */
struct Backing {
    uint64_t words[4096];
    uint8_t *base() { return reinterpret_cast<uint8_t *>(words); }
    Backing() { memset(words, 0, sizeof(words)); }
};

/* ─── layout ─────────────────────────────────────────────────────────────── */

TEST(rcs, layout_zero_capacity) {
    GoodSet::Layout l = GoodSet::Layout::init(0);
    ASSERT_EQ(l.cap, 0);
    ASSERT_EQ(l.table_cap, 0);
    ASSERT_EQ(l.total_size, 0);
}

TEST(rcs, layout_rounds_table_to_power_of_two) {
    GoodSet::Layout l = GoodSet::Layout::init(10);
    ASSERT_EQ(l.table_cap, 16);
    ASSERT_EQ(l.table_mask, 15);

    /* Item capacity is the load factor applied to the table. */
    ASSERT_EQ(l.cap, (size_t)(0.8125 * 16));
}

TEST(rcs, layout_exact_power_of_two_is_not_doubled) {
    GoodSet::Layout l = GoodSet::Layout::init(16);
    ASSERT_EQ(l.table_cap, 16);
}

TEST(rcs, capacity_for_count_allows_for_load_factor_and_reserved_id) {
    /* Room for n items means more than n slots: ID 0 is reserved and the
     * table is only filled to the load factor. */
    const size_t need = GoodSet::capacity_for_count(13);
    ASSERT_TRUE(need > 13);

    GoodSet::Layout l = GoodSet::Layout::init(need);
    ASSERT_TRUE(l.cap > 13);
}

/* ─── zero capacity ──────────────────────────────────────────────────────── */

TEST(rcs, zero_capacity_lookup_is_empty_not_a_read_past_the_end) {
    Backing b;
    GoodSet::Layout l = GoodSet::Layout::init(0);
    GoodSet s = GoodSet::init(OffsetBuf::init(b.base()), l, GoodCtx());

    /* Must short-circuit: probing a zero-size table would read whatever
     * follows the set in the backing buffer. */
    ASSERT_EQ(s.lookup(b.base(), 42), 0);
    ASSERT_EQ(s.count(), 0);
}

/* ─── basic behavior ─────────────────────────────────────────────────────── */

TEST(rcs, add_lookup_release) {
    Backing b;
    GoodSet::Layout l = GoodSet::Layout::init(32);
    GoodSet s = GoodSet::init(OffsetBuf::init(b.base()), l, GoodCtx());

    uint16_t id = 0;
    ASSERT_TRUE(s.add(b.base(), 1234, &id) == AddResult::ok);
    ASSERT_TRUE(id != 0);
    ASSERT_TRUE(s.check_integrity(b.base()));
    ASSERT_TRUE(s.check_reachable(b.base()));

    ASSERT_EQ(s.lookup(b.base(), 1234), id);
    ASSERT_EQ(s.count(), 1);
    ASSERT_EQ(s.ref_count(b.base(), id), 1);
    ASSERT_EQ(*s.get(b.base(), id), 1234);

    s.release(b.base(), id);
    ASSERT_EQ(s.count(), 0);
    ASSERT_TRUE(s.check_integrity(b.base()));
    ASSERT_TRUE(s.check_reachable(b.base()));
}

TEST(rcs, id_zero_is_never_assigned) {
    Backing b;
    GoodSet::Layout l = GoodSet::Layout::init(32);
    GoodSet s = GoodSet::init(OffsetBuf::init(b.base()), l, GoodCtx());

    for (uint32_t v = 0; v < 10; v++) {
        uint16_t id = 0;
        ASSERT_TRUE(s.add(b.base(), v, &id) == AddResult::ok);
        ASSERT_TRUE(id != 0);
    }
}

TEST(rcs, adding_same_value_dedups_and_refcounts) {
    Backing b;
    GoodSet::Layout l = GoodSet::Layout::init(32);
    GoodSet s = GoodSet::init(OffsetBuf::init(b.base()), l, GoodCtx());

    uint16_t a = 0, c = 0;
    ASSERT_TRUE(s.add(b.base(), 7, &a) == AddResult::ok);
    ASSERT_TRUE(s.add(b.base(), 7, &c) == AddResult::ok);

    ASSERT_EQ(a, c);
    ASSERT_EQ(s.ref_count(b.base(), a), 2);
    /* Two references, one living item. */
    ASSERT_EQ(s.count(), 1);
    ASSERT_TRUE(s.check_integrity(b.base()));
    ASSERT_TRUE(s.check_reachable(b.base()));
}

TEST(rcs, use_takes_another_reference) {
    Backing b;
    GoodSet::Layout l = GoodSet::Layout::init(32);
    GoodSet s = GoodSet::init(OffsetBuf::init(b.base()), l, GoodCtx());

    uint16_t id = 0;
    ASSERT_TRUE(s.add(b.base(), 5, &id) == AddResult::ok);
    s.use(b.base(), id);
    ASSERT_EQ(s.ref_count(b.base(), id), 2);

    s.release(b.base(), id);
    ASSERT_EQ(s.ref_count(b.base(), id), 1);
    ASSERT_EQ(s.count(), 1);
}

TEST(rcs, release_multiple) {
    Backing b;
    GoodSet::Layout l = GoodSet::Layout::init(32);
    GoodSet s = GoodSet::init(OffsetBuf::init(b.base()), l, GoodCtx());

    uint16_t id = 0;
    ASSERT_TRUE(s.add(b.base(), 5, &id) == AddResult::ok);
    s.use(b.base(), id);
    s.use(b.base(), id);
    ASSERT_EQ(s.ref_count(b.base(), id), 3);

    s.release_multiple(b.base(), id, 3);
    ASSERT_EQ(s.ref_count(b.base(), id), 0);
    ASSERT_EQ(s.count(), 0);
}

TEST(rcs, dead_item_is_resurrected_by_readding) {
    Backing b;
    GoodSet::Layout l = GoodSet::Layout::init(32);
    GoodSet s = GoodSet::init(OffsetBuf::init(b.base()), l, GoodCtx());

    uint16_t id = 0;
    ASSERT_TRUE(s.add(b.base(), 99, &id) == AddResult::ok);
    s.release(b.base(), id);
    ASSERT_EQ(s.count(), 0);

    /* The bucket has not been reused yet, so the same ID comes back. */
    uint16_t again = 0;
    ASSERT_TRUE(s.add(b.base(), 99, &again) == AddResult::ok);
    ASSERT_EQ(again, id);
    ASSERT_EQ(s.count(), 1);
    ASSERT_TRUE(s.check_integrity(b.base()));
    ASSERT_TRUE(s.check_reachable(b.base()));
}

/* ─── add_with_id ────────────────────────────────────────────────────────── */

TEST(rcs, add_with_id_reuses_matching_live_id) {
    Backing b;
    GoodSet::Layout l = GoodSet::Layout::init(32);
    GoodSet s = GoodSet::init(OffsetBuf::init(b.base()), l, GoodCtx());

    uint16_t id = 0;
    ASSERT_TRUE(s.add(b.base(), 11, &id) == AddResult::ok);

    uint16_t got = 0;
    ASSERT_TRUE(s.add_with_id(b.base(), 11, id, &got) == AddResult::ok);
    ASSERT_EQ(got, id);
    ASSERT_EQ(s.ref_count(b.base(), id), 2);
}

TEST(rcs, add_with_id_dead_id_resolving_to_existing_value) {
    /* The case upstream calls out by name: the requested ID is dead, but the
     * value is already present under a different ID, so that one must win
     * rather than the value being inserted twice. */
    Backing b;
    GoodSet::Layout l = GoodSet::Layout::init(32);
    GoodSet s = GoodSet::init(OffsetBuf::init(b.base()), l, GoodCtx());

    uint16_t first = 0, second = 0;
    ASSERT_TRUE(s.add(b.base(), 100, &first) == AddResult::ok);
    ASSERT_TRUE(s.add(b.base(), 200, &second) == AddResult::ok);

    /* Kill the first ID, keep the second value alive. */
    s.release(b.base(), first);

    uint16_t got = 0;
    ASSERT_TRUE(s.add_with_id(b.base(), 200, first, &got) == AddResult::ok);
    ASSERT_EQ(got, second);
    ASSERT_EQ(s.ref_count(b.base(), second), 2);
    ASSERT_TRUE(s.check_integrity(b.base()));
    ASSERT_TRUE(s.check_reachable(b.base()));
}

/* ─── iteration ──────────────────────────────────────────────────────────── */

TEST(rcs, iterator_visits_live_entries_in_id_order) {
    Backing b;
    GoodSet::Layout l = GoodSet::Layout::init(64);
    GoodSet s = GoodSet::init(OffsetBuf::init(b.base()), l, GoodCtx());

    uint16_t ids[5];
    for (uint32_t i = 0; i < 5; i++) {
        ASSERT_TRUE(s.add(b.base(), i * 17 + 1, &ids[i]) == AddResult::ok);
    }

    /* Kill the middle one; it must be skipped, not reported. */
    s.release(b.base(), ids[2]);

    GoodSet::Iterator it = s.iterator(b.base());
    uint16_t last = 0;
    size_t seen = 0;
    uint16_t id;
    uint32_t *val;
    while (it.next(&id, &val)) {
        ASSERT_TRUE(id > last);
        last = id;
        ASSERT_TRUE(id != ids[2]);
        seen++;
    }
    ASSERT_EQ(seen, 4);
}

/* ─── collisions, displacement, backward-shift ───────────────────────────── */

TEST(rcs, bad_hash_keeps_all_values_findable) {
    /* Every value lands in one of four buckets, so almost everything probes.
     * If displacement or PSL bookkeeping is wrong, lookups start missing. */
    Backing b;
    BadSet::Layout l = BadSet::Layout::init(64);
    BadSet s = BadSet::init(OffsetBuf::init(b.base()), l, BadCtx());

    const uint32_t n = 30;
    uint16_t ids[30];

    for (uint32_t v = 0; v < n; v++) {
        ASSERT_TRUE(s.add(b.base(), v, &ids[v]) == AddResult::ok);
        ASSERT_TRUE(s.check_integrity(b.base()));
        ASSERT_TRUE(s.check_reachable(b.base()));
    }

    /* Everything inserted must still be findable under its own ID. */
    for (uint32_t v = 0; v < n; v++) {
        ASSERT_EQ(s.lookup(b.base(), v), ids[v]);
    }
    ASSERT_EQ(s.count(), n);
}

TEST(rcs, backward_shift_preserves_probe_chains) {
    /* Deleting from the middle of a chain must pull the rest back. If it left
     * a hole, every value past it would become unreachable. */
    Backing b;
    BadSet::Layout l = BadSet::Layout::init(64);
    BadSet s = BadSet::init(OffsetBuf::init(b.base()), l, BadCtx());

    const uint32_t n = 24;
    uint16_t ids[24];
    for (uint32_t v = 0; v < n; v++) {
        ASSERT_TRUE(s.add(b.base(), v, &ids[v]) == AddResult::ok);
    }
    ASSERT_TRUE(s.check_integrity(b.base()));
    ASSERT_TRUE(s.check_reachable(b.base()));

    /* Kill every third value, forcing repeated deletes out of chain middles.
     * add() reaps dead items from the end, so drive that via add(). */
    for (uint32_t v = 0; v < n; v += 3) {
        s.release(b.base(), ids[v]);
    }

    /* Force reaping and reinsertion. */
    uint16_t tmp = 0;
    ASSERT_TRUE(s.add(b.base(), 9999, &tmp) == AddResult::ok);
    ASSERT_TRUE(s.check_integrity(b.base()));
    ASSERT_TRUE(s.check_reachable(b.base()));

    /* Everything not released must still be findable. */
    for (uint32_t v = 0; v < n; v++) {
        if (v % 3 == 0) continue;
        ASSERT_EQ(s.lookup(b.base(), v), ids[v]);
    }
}

TEST(rcs, churn_keeps_invariants) {
    /* Interleave adds and releases so dead items, resurrection, reaping and
     * displacement all overlap. Integrity is checked throughout. */
    Backing b;
    BadSet::Layout l = BadSet::Layout::init(64);
    BadSet s = BadSet::init(OffsetBuf::init(b.base()), l, BadCtx());

    uint16_t live[16];
    uint32_t vals[16];
    size_t live_n = 0;

    for (uint32_t round = 0; round < 200; round++) {
        if (live_n < 10) {
            const uint32_t v = round * 7 + 1;
            uint16_t id = 0;
            if (s.add(b.base(), v, &id) == AddResult::ok) {
                live[live_n] = id;
                vals[live_n] = v;
                live_n++;
            }
        } else {
            /* Drop the oldest. */
            s.release(b.base(), live[0]);
            for (size_t i = 1; i < live_n; i++) {
                live[i - 1] = live[i];
                vals[i - 1] = vals[i];
            }
            live_n--;
        }

        ASSERT_TRUE(s.check_integrity(b.base()));
        ASSERT_TRUE(s.check_reachable(b.base()));
    }

    /* Whatever is still held must still resolve to its own ID. */
    for (size_t i = 0; i < live_n; i++) {
        ASSERT_EQ(s.lookup(b.base(), vals[i]), live[i]);
    }
}

TEST(rcs, max_psl_bounds_the_distribution) {
    /* lookup stops probing after max_psl, so if max_psl ever understated the
     * real distribution a resident item would become invisible. */
    Backing b;
    BadSet::Layout l = BadSet::Layout::init(64);
    BadSet s = BadSet::init(OffsetBuf::init(b.base()), l, BadCtx());

    for (uint32_t v = 0; v < 20; v++) {
        uint16_t id = 0;
        ASSERT_TRUE(s.add(b.base(), v, &id) == AddResult::ok);
    }

    /* check_integrity verifies psl_stats is empty above max_psl. */
    ASSERT_TRUE(s.check_integrity(b.base()));
    ASSERT_TRUE(s.check_reachable(b.base()));
    ASSERT_TRUE(s.max_psl > 0);
}

/* ─── capacity ───────────────────────────────────────────────────────────── */

TEST(rcs, filling_the_set_reports_rather_than_overflows) {
    Backing b;
    GoodSet::Layout l = GoodSet::Layout::init(16);
    GoodSet s = GoodSet::init(OffsetBuf::init(b.base()), l, GoodCtx());

    /* Add until it refuses. It must refuse rather than corrupt anything. */
    bool refused = false;
    for (uint32_t v = 0; v < 1000; v++) {
        uint16_t id = 0;
        const AddResult r = s.add(b.base(), v, &id);
        if (r != AddResult::ok) { refused = true; break; }
    }

    ASSERT_TRUE(refused);
    ASSERT_TRUE(s.check_integrity(b.base()));
    ASSERT_TRUE(s.check_reachable(b.base()));
}
