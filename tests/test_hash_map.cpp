/* Tests for src/terminal/hash_map.hpp.
 *
 * Corresponds to Ghostty src/terminal/hash_map.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * The risky part of this structure is backward-shift deletion: a botched
 * shift leaves a free slot in front of a live entry, lookup stops early, and
 * the entry is silently lost. So the tests drive it with a colliding hash and
 * assert reachability after every removal rather than only checking counts.
 */

#include "test_helpers.h"
#include "hash_map.hpp"

using namespace wisp::terminal;

/* Well-distributed hash. */
static uint64_t good_hash(uint32_t v) {
    uint64_t x = v + 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

/* Deliberately terrible: everything lands in one of four buckets, so almost
 * every entry probes and removals routinely shift long runs. */
static uint64_t bad_hash(uint32_t v) { return v % 4; }

struct GoodHashFn { uint64_t operator()(uint32_t v) const { return good_hash(v); } };
struct BadHashFn  { uint64_t operator()(uint32_t v) const { return bad_hash(v); } };

typedef OffsetHashMap<uint32_t, uint32_t> Map;
typedef OffsetHashMap<uint32_t, uint32_t, 80> Map80;

struct Backing {
    uint64_t words[4096];
    uint8_t *base() { return reinterpret_cast<uint8_t *>(words); }
    Backing() { memset(words, 0, sizeof(words)); }
};

/* ─── layout ─────────────────────────────────────────────────────────────── */

TEST(hashmap, layout_zero_capacity) {
    Map::Layout l = Map::Layout::init(0);
    ASSERT_EQ(l.cap, 0);
    ASSERT_EQ(l.total_size, 0);
}

TEST(hashmap, layout_rounds_to_power_of_two) {
    Map::Layout l = Map::Layout::init(10);
    ASSERT_EQ(l.cap, 16);

    Map::Layout l2 = Map::Layout::init(16);
    ASSERT_EQ(l2.cap, 16);

    Map::Layout l3 = Map::Layout::init(17);
    ASSERT_EQ(l3.cap, 32);
}

TEST(hashmap, layout_regions_are_ordered_and_aligned) {
    Map::Layout l = Map::Layout::init(64);

    ASSERT_TRUE(l.keys_start >= l.metadata_start + l.cap * sizeof(HashMapMetadata));
    ASSERT_TRUE(l.values_start >= l.keys_start + l.cap * sizeof(uint32_t));
    ASSERT_TRUE(l.total_size >= l.values_start + l.cap * sizeof(uint32_t));

    ASSERT_EQ(l.keys_start % alignof(uint32_t), 0);
    ASSERT_EQ(l.values_start % alignof(uint32_t), 0);
}

TEST(hashmap, capacity_for_count_respects_load_factor) {
    /* At 100% a slot per entry is enough. */
    ASSERT_EQ(Map::capacity_for_count(10), 10);

    /* At 80% it needs headroom. */
    ASSERT_TRUE(Map80::capacity_for_count(10) > 10);
}

/* ─── metadata ───────────────────────────────────────────────────────────── */

TEST(hashmap, metadata_used_free_and_fingerprint) {
    HashMapMetadata m;
    m.clear();
    ASSERT_TRUE(m.free());
    ASSERT_FALSE(m.used());

    m.fill(0x7F);
    ASSERT_TRUE(m.used());
    ASSERT_EQ(m.fingerprint(), 0x7F);

    /* The fingerprint must not bleed into the used bit. */
    m.fill(0xFF);
    ASSERT_TRUE(m.used());
    ASSERT_EQ(m.fingerprint(), 0x7F);

    m.clear();
    ASSERT_TRUE(m.free());
}

TEST(hashmap, fingerprint_comes_from_high_bits) {
    /* The bucket index uses the low bits, so the fingerprint must use high
     * ones or it would carry no information the index does not already have. */
    const uint64_t a = 0x0000000000000001ULL;
    const uint64_t b = 0x0000000000000002ULL;
    ASSERT_EQ(HashMapMetadata::take_fingerprint(a),
              HashMapMetadata::take_fingerprint(b));

    const uint64_t c = 0x7F00000000000000ULL;
    ASSERT_EQ(HashMapMetadata::take_fingerprint(c), 0x7F);
}

/* ─── basics ─────────────────────────────────────────────────────────────── */

TEST(hashmap, put_get_contains) {
    Backing b;
    Map::Layout l = Map::Layout::init(64);
    Map m = Map::init(OffsetBuf::init(b.base()), l);

    ASSERT_EQ(m.count(), 0);
    ASSERT_TRUE(m.put(b.base(), good_hash(42), 42, 1234));
    ASSERT_EQ(m.count(), 1);

    ASSERT_TRUE(m.contains(b.base(), good_hash(42), 42));
    uint32_t *v = m.get(b.base(), good_hash(42), 42);
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(*v, 1234);

    ASSERT_FALSE(m.contains(b.base(), good_hash(43), 43));
    ASSERT_NULL(m.get(b.base(), good_hash(43), 43));
}

TEST(hashmap, get_or_put_reports_existing) {
    Backing b;
    Map::Layout l = Map::Layout::init(64);
    Map m = Map::init(OffsetBuf::init(b.base()), l);

    Map::GetOrPutResult r;
    ASSERT_TRUE(m.get_or_put(b.base(), good_hash(7), 7, &r));
    ASSERT_FALSE(r.found_existing);
    *r.value_ptr = 100;

    ASSERT_TRUE(m.get_or_put(b.base(), good_hash(7), 7, &r));
    ASSERT_TRUE(r.found_existing);
    ASSERT_EQ(*r.value_ptr, 100);

    /* Still one entry. */
    ASSERT_EQ(m.count(), 1);
}

TEST(hashmap, put_overwrites_value_not_count) {
    Backing b;
    Map::Layout l = Map::Layout::init(64);
    Map m = Map::init(OffsetBuf::init(b.base()), l);

    ASSERT_TRUE(m.put(b.base(), good_hash(5), 5, 1));
    ASSERT_TRUE(m.put(b.base(), good_hash(5), 5, 2));
    ASSERT_EQ(m.count(), 1);
    ASSERT_EQ(*m.get(b.base(), good_hash(5), 5), 2);
}

TEST(hashmap, zero_capacity_is_inert) {
    Backing b;
    Map::Layout l = Map::Layout::init(0);
    Map m = Map::init(OffsetBuf::init(b.base()), l);

    /* Must not probe a zero-size table. */
    ASSERT_FALSE(m.contains(b.base(), good_hash(1), 1));
    ASSERT_FALSE(m.put(b.base(), good_hash(1), 1, 1));
    ASSERT_EQ(m.count(), 0);
}

TEST(hashmap, clear_empties_without_losing_capacity) {
    Backing b;
    Map::Layout l = Map::Layout::init(64);
    Map m = Map::init(OffsetBuf::init(b.base()), l);

    for (uint32_t i = 0; i < 20; i++) m.put(b.base(), good_hash(i), i, i);
    ASSERT_EQ(m.count(), 20);

    m.clear(b.base());
    ASSERT_EQ(m.count(), 0);
    ASSERT_FALSE(m.contains(b.base(), good_hash(3), 3));

    ASSERT_TRUE(m.put(b.base(), good_hash(3), 3, 9));
    ASSERT_EQ(*m.get(b.base(), good_hash(3), 3), 9);
}

/* ─── capacity ───────────────────────────────────────────────────────────── */

TEST(hashmap, refuses_past_the_load_ceiling) {
    Backing b;
    Map80::Layout l = Map80::Layout::init(16);
    Map80 m = Map80::init(OffsetBuf::init(b.base()), l);

    /* 80% of 16 slots. */
    ASSERT_EQ(m.max_len(), 12);

    uint32_t inserted = 0;
    for (uint32_t i = 0; i < 100; i++) {
        if (!m.put(b.base(), good_hash(i), i, i)) break;
        inserted++;
    }

    ASSERT_EQ(inserted, 12);
    ASSERT_EQ(m.count(), 12);
}

TEST(hashmap, full_map_still_finds_existing_keys) {
    Backing b;
    Map::Layout l = Map::Layout::init(16);
    Map m = Map::init(OffsetBuf::init(b.base()), l);

    uint32_t n = 0;
    for (uint32_t i = 0; i < 100; i++) {
        if (!m.put(b.base(), good_hash(i), i, i * 3)) break;
        n++;
    }
    ASSERT_TRUE(n > 0);

    /* A full table must not break lookup of what it already holds. */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t *v = m.get(b.base(), good_hash(i), i);
        ASSERT_NOT_NULL(v);
        ASSERT_EQ(*v, i * 3);
    }
}

/* ─── removal and backward shift ─────────────────────────────────────────── */

TEST(hashmap, remove_basic) {
    Backing b;
    Map::Layout l = Map::Layout::init(64);
    Map m = Map::init(OffsetBuf::init(b.base()), l);

    m.put(b.base(), good_hash(1), 1, 10);
    m.put(b.base(), good_hash(2), 2, 20);

    ASSERT_TRUE(m.remove(b.base(), good_hash(1), 1, GoodHashFn()));
    ASSERT_EQ(m.count(), 1);
    ASSERT_FALSE(m.contains(b.base(), good_hash(1), 1));
    ASSERT_TRUE(m.contains(b.base(), good_hash(2), 2));

    /* Removing what is not there must not change anything. */
    ASSERT_FALSE(m.remove(b.base(), good_hash(1), 1, GoodHashFn()));
    ASSERT_EQ(m.count(), 1);
}

TEST(hashmap, remove_from_middle_of_collision_run_keeps_rest_findable) {
    /* The case backward shift exists for. With tombstones or a naive clear,
     * everything after the hole becomes unreachable. */
    Backing b;
    Map::Layout l = Map::Layout::init(64);
    Map m = Map::init(OffsetBuf::init(b.base()), l);

    const uint32_t n = 24;
    for (uint32_t v = 0; v < n; v++) {
        ASSERT_TRUE(m.put(b.base(), bad_hash(v), v, v * 7));
    }
    ASSERT_TRUE(m.check_reachable(b.base(), BadHashFn()));

    /* Remove from the middle of the runs, checking after each one. */
    for (uint32_t v = 1; v < n; v += 3) {
        ASSERT_TRUE(m.remove(b.base(), bad_hash(v), v, BadHashFn()));
        ASSERT_TRUE(m.check_reachable(b.base(), BadHashFn()));
    }

    /* Everything not removed is still there with the right value. */
    for (uint32_t v = 0; v < n; v++) {
        if (v % 3 == 1) {
            ASSERT_FALSE(m.contains(b.base(), bad_hash(v), v));
            continue;
        }
        uint32_t *got = m.get(b.base(), bad_hash(v), v);
        ASSERT_NOT_NULL(got);
        ASSERT_EQ(*got, v * 7);
    }
}

TEST(hashmap, remove_all_then_reinsert) {
    Backing b;
    Map::Layout l = Map::Layout::init(64);
    Map m = Map::init(OffsetBuf::init(b.base()), l);

    const uint32_t n = 30;
    for (uint32_t v = 0; v < n; v++) m.put(b.base(), bad_hash(v), v, v);

    for (uint32_t v = 0; v < n; v++) {
        ASSERT_TRUE(m.remove(b.base(), bad_hash(v), v, BadHashFn()));
        ASSERT_TRUE(m.check_reachable(b.base(), BadHashFn()));
    }
    ASSERT_EQ(m.count(), 0);

    /* No tombstones left behind, so the table is as good as new. */
    for (uint32_t v = 0; v < n; v++) {
        ASSERT_TRUE(m.put(b.base(), bad_hash(v), v, v * 2));
    }
    ASSERT_EQ(m.count(), n);
    ASSERT_TRUE(m.check_reachable(b.base(), BadHashFn()));
}

TEST(hashmap, churn_keeps_everything_reachable) {
    /* Interleave inserts and removals so shifts happen against a table that
     * is constantly changing shape. */
    Backing b;
    Map::Layout l = Map::Layout::init(64);
    Map m = Map::init(OffsetBuf::init(b.base()), l);

    for (uint32_t round = 0; round < 300; round++) {
        const uint32_t v = round % 40;
        if (round % 3 == 0) {
            m.remove(b.base(), bad_hash(v), v, BadHashFn());
        } else {
            m.put(b.base(), bad_hash(v), v, round);
        }
        ASSERT_TRUE(m.check_reachable(b.base(), BadHashFn()));
    }
}

TEST(hashmap, remove_across_the_wrap_point) {
    /* Backward shift has to compare distances cyclically. Entries whose runs
     * wrap past the end of the table are where a non-cyclic comparison breaks. */
    Backing b;
    Map::Layout l = Map::Layout::init(16);
    Map m = Map::init(OffsetBuf::init(b.base()), l);

    /* bad_hash sends everything to 0..3, and with 16 slots a long run wraps. */
    const uint32_t n = 16;
    uint32_t inserted = 0;
    for (uint32_t v = 0; v < n; v++) {
        if (!m.put(b.base(), bad_hash(v), v, v)) break;
        inserted++;
    }
    ASSERT_TRUE(m.check_reachable(b.base(), BadHashFn()));

    for (uint32_t v = 0; v < inserted; v += 2) {
        ASSERT_TRUE(m.remove(b.base(), bad_hash(v), v, BadHashFn()));
        ASSERT_TRUE(m.check_reachable(b.base(), BadHashFn()));
    }
}

/* ─── iteration ──────────────────────────────────────────────────────────── */

TEST(hashmap, iterator_visits_every_live_entry_once) {
    Backing b;
    Map::Layout l = Map::Layout::init(64);
    Map m = Map::init(OffsetBuf::init(b.base()), l);

    const uint32_t n = 20;
    for (uint32_t v = 0; v < n; v++) m.put(b.base(), good_hash(v), v, v * 5);

    m.remove(b.base(), good_hash(3), 3, GoodHashFn());
    m.remove(b.base(), good_hash(11), 11, GoodHashFn());

    bool seen[20];
    memset(seen, 0, sizeof(seen));

    Map::Iterator it = m.iterator(b.base());
    uint32_t *k;
    uint32_t *v;
    uint32_t visited = 0;
    while (it.next(&k, &v)) {
        ASSERT_TRUE(*k < n);
        ASSERT_FALSE(seen[*k]);   /* never twice */
        seen[*k] = true;
        ASSERT_EQ(*v, *k * 5);
        visited++;
    }

    ASSERT_EQ(visited, n - 2);
    ASSERT_EQ(visited, m.count());
    ASSERT_FALSE(seen[3]);
    ASSERT_FALSE(seen[11]);
}

TEST(hashmap, iterator_on_empty_map) {
    Backing b;
    Map::Layout l = Map::Layout::init(16);
    Map m = Map::init(OffsetBuf::init(b.base()), l);

    Map::Iterator it = m.iterator(b.base());
    uint32_t *k;
    uint32_t *v;
    ASSERT_FALSE(it.next(&k, &v));
}

/* ─── relocation ─────────────────────────────────────────────────────────── */

TEST(hashmap, map_survives_being_copied_to_another_buffer) {
    /* The whole reason for offset addressing: the backing memory can move and
     * the map keeps working, because it stores no absolute pointers. */
    Backing b1;
    Backing b2;

    Map::Layout l = Map::Layout::init(64);
    Map m = Map::init(OffsetBuf::init(b1.base()), l);

    for (uint32_t v = 0; v < 20; v++) {
        ASSERT_TRUE(m.put(b1.base(), good_hash(v), v, v * 11));
    }

    memcpy(b2.base(), b1.base(), l.total_size);

    /* Same map value, different base pointer. */
    for (uint32_t v = 0; v < 20; v++) {
        uint32_t *got = m.get(b2.base(), good_hash(v), v);
        ASSERT_NOT_NULL(got);
        ASSERT_EQ(*got, v * 11);
    }
    ASSERT_TRUE(m.check_reachable(b2.base(), GoodHashFn()));
}
