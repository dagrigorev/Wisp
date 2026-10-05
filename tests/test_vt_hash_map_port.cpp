/* Transliterated from the test blocks in Ghostty src/terminal/hash_map.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names. The random
 * tests draw from the same seeded Xoshiro256 (src/zigstd/random.hpp). The
 * oracle map is std::unordered_map in place of std.AutoHashMapUnmanaged.
 * A `void` value type is an empty struct.
 */

#include "test_helpers.h"
#include "../vt/hash_map.hpp"
#include "../zigstd/random.hpp"

#include <malloc.h>

#include <unordered_map>
#include <vector>

using namespace wisp::vt;
using namespace wisp::vt::hash_map;
using wisp::zigstd::DefaultPrng;
using wisp::zigstd::Random;

template <typename K, typename V, uint8_t L = default_max_load_percentage>
struct Auto {
    typedef HashMapUnmanaged<K, V, AutoContext<K>, L> Map;
};

struct Buf {
    uint8_t *ptr;
    size_t len;
    Buf(size_t n, size_t align) : ptr((uint8_t *)_aligned_malloc(n ? n : 1, align)), len(n) {}
    ~Buf() { _aligned_free(ptr); }
};

template <typename Map>
struct Fixture {
    typename Map::Layout layout;
    Buf buf;
    Map map;
    explicit Fixture(typename Map::Size cap)
        : layout(Map::layoutForCapacity(cap)), buf(layout.total_size, Map::base_align),
          map(Map::init(size::OffsetBuf::init(buf.ptr), layout)) {}
};

template <typename Map, typename V, typename K>
static V getv(const Map &m, const K &k) {
    V v = V();
    const bool ok = m.get(k, &v);
    (void)ok;
    return v;
}

/* Verify the canonical placement invariant that backward-shift deletion
 * maintains: every used entry is reachable from its home slot without
 * crossing a free slot. This is exactly the property lookups depend on. */
template <typename Map, typename C>
static bool expectCanonical(const Map &map, C ctx) {
    const size_t cap = map.capacity();
    const size_t mask = cap - 1;
    size_t used = 0;
    for (size_t idx = 0; idx < cap; idx++) {
        if (!map.metadata[idx].isUsed()) continue;
        used += 1;

        size_t probe = (size_t)(ctx.hash(map.keys[idx]) & mask);
        while (probe != idx) {
            if (!map.metadata[probe].isUsed()) return false;
            probe = (probe + 1) & mask;
        }
    }
    return map.count() == used;
}

TEST(hash_map, HashMap_basic_usage) {
    typedef Auto<uint32_t, uint32_t>::Map Map;
    Fixture<Map> f(16);
    Map &map = f.map;

    const uint32_t count = 5;
    uint32_t i = 0;
    uint32_t total = 0;
    while (i < count) {
        ASSERT_TRUE(map.put(i, i));
        total += i;
        i += 1;
    }

    uint32_t sum = 0;
    Map::Iterator it = map.iterator();
    Map::Entry kv;
    while (it.next(&kv)) sum += *kv.key_ptr;
    ASSERT_TRUE(total == sum);

    i = 0;
    sum = 0;
    while (i < count) {
        uint32_t v;
        ASSERT_TRUE(map.get(i, &v) && v == i);
        sum += v;
        i += 1;
    }
    ASSERT_TRUE(total == sum);
}

TEST(hash_map, HashMap_ensureTotalCapacity) {
    typedef Auto<int32_t, int32_t>::Map Map;
    Fixture<Map> f(32);
    Map &map = f.map;

    const Map::Size initial_capacity = map.capacity();
    ASSERT_TRUE(initial_capacity >= 20);
    for (int32_t i = 0; i < 20; i++) {
        Map::KV prev;
        ASSERT_FALSE(map.fetchPutAssumeCapacity(i, i + 10, &prev));
    }
    /* shouldn't resize from putAssumeCapacity */
    ASSERT_TRUE(initial_capacity == map.capacity());
}

TEST(hash_map, HashMap_ensureUnusedCapacity_with_removals) {
    typedef Auto<int32_t, int32_t>::Map Map;
    Fixture<Map> f(32);
    Map &map = f.map;

    for (int32_t i = 0; i < 100; i++) {
        ASSERT_TRUE(map.ensureUnusedCapacity(1));
        map.putAssumeCapacity(i, i);
        (void)map.remove(i);
    }
}

TEST(hash_map, HashMap_clearRetainingCapacity) {
    typedef Auto<uint32_t, uint32_t>::Map Map;
    Fixture<Map> f(16);
    Map &map = f.map;

    map.clearRetainingCapacity();

    ASSERT_TRUE(map.put(1, 1));
    ASSERT_TRUE((getv<Map, uint32_t>(map, 1u)) == 1);
    ASSERT_TRUE(map.count() == 1);

    map.clearRetainingCapacity();
    map.putAssumeCapacity(1, 1);
    ASSERT_TRUE((getv<Map, uint32_t>(map, 1u)) == 1);
    ASSERT_TRUE(map.count() == 1);

    const Map::Size actual_cap = map.capacity();
    ASSERT_TRUE(actual_cap > 0);

    map.clearRetainingCapacity();
    map.clearRetainingCapacity();
    ASSERT_TRUE(map.count() == 0);
    ASSERT_TRUE(map.capacity() == actual_cap);
    ASSERT_FALSE(map.contains(1));
}

TEST(hash_map, HashMap_ensureTotalCapacity_with_existing_elements) {
    typedef Auto<uint32_t, uint32_t>::Map Map;
    Fixture<Map> f(8);
    Map &map = f.map;

    ASSERT_TRUE(map.put(0, 0));
    ASSERT_TRUE(map.count() == 1);
    ASSERT_TRUE(map.capacity() == 8);

    ASSERT_FALSE(map.ensureTotalCapacity(65)); /* error.OutOfMemory */
    ASSERT_TRUE(map.count() == 1);
    ASSERT_TRUE(map.capacity() == 8);
}

TEST(hash_map, HashMap_remove) {
    typedef Auto<uint32_t, uint32_t>::Map Map;
    Fixture<Map> f(32);
    Map &map = f.map;

    for (uint32_t i = 0; i < 16; i++) ASSERT_TRUE(map.put(i, i));

    for (uint32_t i = 0; i < 16; i++) {
        if (i % 3 == 0) (void)map.remove(i);
    }
    ASSERT_TRUE(map.count() == 10);
    Map::Iterator it = map.iterator();
    Map::Entry kv;
    while (it.next(&kv)) {
        ASSERT_TRUE(*kv.key_ptr == *kv.value_ptr);
        ASSERT_TRUE(*kv.key_ptr % 3 != 0);
    }

    for (uint32_t i = 0; i < 16; i++) {
        if (i % 3 == 0) {
            ASSERT_FALSE(map.contains(i));
        } else {
            uint32_t v;
            ASSERT_TRUE(map.get(i, &v) && v == i);
        }
    }
}

TEST(hash_map, HashMap_reverse_removes) {
    typedef Auto<uint32_t, uint32_t>::Map Map;
    Fixture<Map> f(32);
    Map &map = f.map;

    for (uint32_t i = 0; i < 16; i++) ASSERT_TRUE(map.putNoClobber(i, i));

    uint32_t i = 16;
    while (i > 0) {
        (void)map.remove(i - 1);
        ASSERT_FALSE(map.contains(i - 1));
        for (uint32_t j = 0; j < i - 1; j++) {
            uint32_t v;
            ASSERT_TRUE(map.get(j, &v) && v == j);
        }
        i -= 1;
    }

    ASSERT_TRUE(map.count() == 0);
}

TEST(hash_map, HashMap_multiple_removes_on_same_metadata) {
    typedef Auto<uint32_t, uint32_t>::Map Map;
    Fixture<Map> f(32);
    Map &map = f.map;

    for (uint32_t i = 0; i < 16; i++) ASSERT_TRUE(map.put(i, i));

    (void)map.remove(7);
    (void)map.remove(15);
    (void)map.remove(14);
    (void)map.remove(13);
    ASSERT_FALSE(map.contains(7));
    ASSERT_FALSE(map.contains(15));
    ASSERT_FALSE(map.contains(14));
    ASSERT_FALSE(map.contains(13));

    for (uint32_t i = 0; i < 13; i++) {
        if (i == 7) {
            ASSERT_FALSE(map.contains(i));
        } else {
            uint32_t v;
            ASSERT_TRUE(map.get(i, &v) && v == i);
        }
    }

    ASSERT_TRUE(map.put(15, 15));
    ASSERT_TRUE(map.put(13, 13));
    ASSERT_TRUE(map.put(14, 14));
    ASSERT_TRUE(map.put(7, 7));
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t v;
        ASSERT_TRUE(map.get(i, &v) && v == i);
    }
}

TEST(hash_map, HashMap_put_and_remove_loop_in_random_order) {
    typedef Auto<uint32_t, uint32_t>::Map Map;
    Fixture<Map> f(64);
    Map &map = f.map;

    std::vector<uint32_t> keys;

    const uint32_t size = 32;
    const uint32_t iterations = 100;

    uint32_t i = 0;
    while (i < size) {
        keys.push_back(i);
        i += 1;
    }
    DefaultPrng prng = DefaultPrng::init(0);
    Random random(&prng);

    while (i < iterations) {
        random.shuffle(keys.data(), keys.size());

        for (size_t k = 0; k < keys.size(); k++) ASSERT_TRUE(map.put(keys[k], keys[k]));
        ASSERT_TRUE(map.count() == size);

        for (size_t k = 0; k < keys.size(); k++) (void)map.remove(keys[k]);
        ASSERT_TRUE(map.count() == 0);
        i += 1;
    }
}

TEST(hash_map, HashMap_put) {
    typedef Auto<uint32_t, uint32_t>::Map Map;
    Fixture<Map> f(32);
    Map &map = f.map;

    for (uint32_t i = 0; i < 16; i++) ASSERT_TRUE(map.put(i, i));

    for (uint32_t i = 0; i < 16; i++) {
        uint32_t v;
        ASSERT_TRUE(map.get(i, &v) && v == i);
    }

    for (uint32_t i = 0; i < 16; i++) ASSERT_TRUE(map.put(i, i * 16 + 1));

    for (uint32_t i = 0; i < 16; i++) {
        uint32_t v;
        ASSERT_TRUE(map.get(i, &v) && v == i * 16 + 1);
    }
}

TEST(hash_map, HashMap_put_full_load) {
    typedef Auto<size_t, size_t>::Map Map;
    const size_t cap = 16;
    Fixture<Map> f(cap);
    Map &map = f.map;

    for (size_t i = 0; i < cap; i++) ASSERT_TRUE(map.put(i, i));
    for (size_t i = 0; i < cap; i++) {
        size_t v;
        ASSERT_TRUE(map.get(i, &v) && v == i);
    }

    ASSERT_FALSE(map.put(cap, cap)); /* error.OutOfMemory */
}

TEST(hash_map, HashMap_putAssumeCapacity) {
    typedef Auto<uint32_t, uint32_t>::Map Map;
    Fixture<Map> f(32);
    Map &map = f.map;

    for (uint32_t i = 0; i < 20; i++) map.putAssumeCapacityNoClobber(i, i);

    uint32_t sum = 0;
    for (uint32_t i = 0; i < 20; i++) sum += *map.getPtr(i);
    ASSERT_TRUE(sum == 190);

    for (uint32_t i = 0; i < 20; i++) map.putAssumeCapacity(i, 1);

    sum = 0;
    for (uint32_t i = 0; i < 20; i++) sum += getv<Map, uint32_t>(map, i);
    ASSERT_TRUE(sum == 20);
}

TEST(hash_map, HashMap_repeat_putAssumeCapacity_remove) {
    typedef Auto<uint32_t, uint32_t>::Map Map;
    const uint32_t cap = 32;
    Fixture<Map> f(cap);
    Map &map = f.map;

    const uint32_t limit = cap;

    for (uint32_t i = 0; i < limit; i++) map.putAssumeCapacityNoClobber(i, i);

    /* Repeatedly delete/insert an entry without resizing the map.
     * Put to different keys so entries don't land in the just-freed slot. */
    for (uint32_t i = 0; i < 10 * limit; i++) {
        ASSERT_TRUE(map.remove(i));
        if (i % 2 == 0) {
            map.putAssumeCapacityNoClobber(limit + i, i);
        } else {
            map.putAssumeCapacity(limit + i, i);
        }
    }

    for (uint32_t i = 9 * limit; i < 10 * limit; i++) {
        uint32_t v;
        ASSERT_TRUE(map.get(limit + i, &v) && v == i);
    }
    ASSERT_TRUE(map.count() == limit);
}

struct IdentityContext {
    uint64_t hash(uint32_t key) const { return key; }
    bool eql(uint32_t a, uint32_t b) const { return a == b; }
};

TEST(hash_map, HashMap_no_clobber_move_after_remove_at_max_load) {
    typedef HashMapUnmanaged<uint32_t, uint32_t, IdentityContext, 80> Map;
    Fixture<Map> f(16);
    Map &map = f.map;

    /* Fill the map to its maximum load. */
    const uint32_t max_load = map.maxLoad();
    for (uint32_t i = 0; i < max_load; i++) {
        map.putAssumeCapacityNoClobberContext(i, i, IdentityContext());
    }

    /* Model a managed-cell move: remove the source and insert the value at
     * a destination known to be absent. This must work at maximum load for
     * any number of moves since removal genuinely frees a slot. */
    for (uint32_t i = 0; i < 100; i++) {
        const uint32_t src = i;
        const uint32_t dst = i + max_load;
        ASSERT_TRUE(map.removeContext(src, IdentityContext()));
        map.putAssumeCapacityNoClobberContext(dst, dst, IdentityContext());

        ASSERT_TRUE(max_load == map.count());
        uint32_t v;
        ASSERT_TRUE(map.getContext(dst, IdentityContext(), &v) && v == dst);
        ASSERT_TRUE(expectCanonical(map, IdentityContext()));
    }
}

struct Const14Context {
    uint64_t hash(uint32_t) const { return 14; }
    bool eql(uint32_t a, uint32_t b) const { return a == b; }
};

TEST(hash_map, HashMap_removal_keeps_colliding_clusters_findable) {
    /* All keys hash to the same home slot near the end of the table so
     * that clusters wrap around the index mask. This exercises the cyclic
     * arithmetic in backward-shift deletion. */
    typedef HashMapUnmanaged<uint32_t, uint32_t, Const14Context, default_max_load_percentage> Map;
    const size_t cap = 16;
    Fixture<Map> f(cap);
    Map &map = f.map;

    /* Fill half the table: the cluster spans the wraparound point. */
    for (uint32_t i = 0; i < cap / 2; i++) {
        map.putAssumeCapacityNoClobberContext(i, i, Const14Context());
    }

    /* Remove from the middle of the cluster and verify all remaining
     * entries stay findable after every removal. */
    size_t removed = 0;
    const uint32_t order[] = { 3, 0, 7, 4, 1, 6, 2, 5 };
    for (size_t o = 0; o < 8; o++) {
        ASSERT_TRUE(map.removeContext(order[o], Const14Context()));
        removed += 1;

        for (uint32_t k = 0; k < cap / 2; k++) {
            uint32_t v = 0;
            const bool has = map.getContext(k, Const14Context(), &v);
            if (map.containsContext(k, Const14Context())) {
                ASSERT_TRUE(has && k == v);
            }
        }
        ASSERT_TRUE(cap / 2 - removed == map.count());
        ASSERT_TRUE(expectCanonical(map, Const14Context()));
    }
}

TEST(hash_map, HashMap_removal_from_a_completely_full_table) {
    typedef Auto<uint32_t, uint32_t>::Map Map;
    const size_t cap = 64;
    Fixture<Map> f(cap);
    Map &map = f.map;

    /* A 100% load factor allows filling every raw slot, so removal cannot
     * rely on a free slot to terminate its cluster scan. */
    for (uint32_t i = 0; i < cap; i++) map.putAssumeCapacityNoClobber(i, i);
    ASSERT_TRUE(cap == map.count());

    /* Remove every other key, verifying everything else stays findable. */
    size_t expected = cap;
    for (uint32_t i = 0; i < cap; i++) {
        if (i % 2 != 0) continue;
        ASSERT_TRUE(map.remove(i));
        expected -= 1;
        ASSERT_TRUE(expected == map.count());
    }

    for (uint32_t i = 0; i < cap; i++) {
        uint32_t v;
        if (i % 2 == 0) {
            ASSERT_FALSE(map.get(i, &v));
        } else {
            ASSERT_TRUE(map.get(i, &v) && v == i);
        }
    }
    ASSERT_TRUE(expectCanonical(map, AutoContext<uint32_t>()));
}

TEST(hash_map, HashMap_random_operations_against_an_oracle) {
    typedef Auto<uint32_t, uint32_t>::Map Map;
    const uint32_t cap = 64;
    Fixture<Map> f(cap);
    Map &map = f.map;

    std::unordered_map<uint32_t, uint32_t> oracle;

    DefaultPrng prng = DefaultPrng::init(0xdeadbeef);
    Random random(&prng);

    /* A small key space forces frequent hits, misses, and re-insertions
     * at every load factor from empty to completely full. */
    const uint32_t key_space = cap + cap / 2;
    for (size_t n = 0; n < 20000; n++) {
        const uint32_t key = random.uintLessThan<uint32_t>(key_space);
        switch (random.uintLessThan<uint8_t>(4)) {
            case 0: case 1: {
                const uint32_t value = random.int_<uint32_t>();
                if (map.put(key, value)) {
                    oracle[key] = value;
                } else {
                    /* Map is full: the oracle must not know this key
                     * (put on an existing key always succeeds). */
                    ASSERT_TRUE(oracle.find(key) == oracle.end());
                    ASSERT_TRUE(map.count() == map.capacity());
                }
                break;
            }
            case 2:
                ASSERT_TRUE((oracle.erase(key) == 1) == map.remove(key));
                break;
            case 3: {
                uint32_t v = 0;
                const bool has = map.get(key, &v);
                const auto it = oracle.find(key);
                ASSERT_TRUE(has == (it != oracle.end()));
                if (has) ASSERT_TRUE(v == it->second);
                break;
            }
        }

        ASSERT_TRUE(oracle.size() == map.count());
    }

    /* Final full comparison plus the canonical placement invariant. */
    for (auto it = oracle.begin(); it != oracle.end(); ++it) {
        uint32_t v;
        ASSERT_TRUE(map.get(it->first, &v) && v == it->second);
    }
    ASSERT_TRUE(expectCanonical(map, AutoContext<uint32_t>()));
}

TEST(hash_map, HashMap_getOrPut) {
    typedef Auto<uint32_t, uint32_t>::Map Map;
    Fixture<Map> f(32);
    Map &map = f.map;

    for (uint32_t i = 0; i < 10; i++) ASSERT_TRUE(map.put(i * 2, 2));

    for (uint32_t i = 0; i < 20; i++) {
        Map::Entry e;
        ASSERT_TRUE(map.getOrPutValue(i, 1, &e));
    }

    uint32_t sum = 0;
    for (uint32_t i = 0; i < 20; i++) sum += getv<Map, uint32_t>(map, i);

    ASSERT_TRUE(sum == 30);
}

TEST(hash_map, HashMap_basic_hash_map_usage) {
    typedef Auto<int32_t, int32_t>::Map Map;
    Fixture<Map> f(32);
    Map &map = f.map;

    bool had;
    Map::KV prev;
    ASSERT_TRUE(map.fetchPut(1, 11, &had, &prev) && !had);
    ASSERT_TRUE(map.fetchPut(2, 22, &had, &prev) && !had);
    ASSERT_TRUE(map.fetchPut(3, 33, &had, &prev) && !had);
    ASSERT_TRUE(map.fetchPut(4, 44, &had, &prev) && !had);

    ASSERT_TRUE(map.putNoClobber(5, 55));
    ASSERT_TRUE(map.fetchPut(5, 66, &had, &prev) && had && prev.value == 55);
    ASSERT_TRUE(map.fetchPut(5, 55, &had, &prev) && had && prev.value == 66);

    Map::GetOrPutResult gop1;
    ASSERT_TRUE(map.getOrPut(5, &gop1));
    ASSERT_TRUE(gop1.found_existing == true);
    ASSERT_TRUE(*gop1.value_ptr == 55);
    *gop1.value_ptr = 77;
    Map::Entry e;
    ASSERT_TRUE(map.getEntry(5, &e) && *e.value_ptr == 77);

    Map::GetOrPutResult gop2;
    ASSERT_TRUE(map.getOrPut(99, &gop2));
    ASSERT_TRUE(gop2.found_existing == false);
    *gop2.value_ptr = 42;
    ASSERT_TRUE(map.getEntry(99, &e) && *e.value_ptr == 42);

    Map::Entry gop3;
    ASSERT_TRUE(map.getOrPutValue(5, 5, &gop3));
    ASSERT_TRUE(*gop3.value_ptr == 77);

    Map::Entry gop4;
    ASSERT_TRUE(map.getOrPutValue(100, 41, &gop4));
    ASSERT_TRUE(*gop4.value_ptr == 41);

    ASSERT_TRUE(map.contains(2));
    ASSERT_TRUE(map.getEntry(2, &e) && *e.value_ptr == 22);
    ASSERT_TRUE((getv<Map, int32_t>(map, 2)) == 22);

    Map::KV rmv1;
    ASSERT_TRUE(map.fetchRemove(2, &rmv1));
    ASSERT_TRUE(rmv1.key == 2);
    ASSERT_TRUE(rmv1.value == 22);
    ASSERT_FALSE(map.fetchRemove(2, &rmv1));
    ASSERT_TRUE(map.remove(2) == false);
    ASSERT_FALSE(map.getEntry(2, &e));
    int32_t v;
    ASSERT_FALSE(map.get(2, &v));

    ASSERT_TRUE(map.remove(3) == true);
}

TEST(hash_map, HashMap_ensureUnusedCapacity) {
    typedef Auto<uint64_t, uint64_t>::Map Map;
    const uint32_t cap = 64;
    Fixture<Map> f(cap);
    Map &map = f.map;

    ASSERT_TRUE(map.ensureUnusedCapacity(32));
    ASSERT_FALSE(map.ensureUnusedCapacity(cap + 1)); /* error.OutOfMemory */
}

TEST(hash_map, HashMap_removeByPtr) {
    typedef Auto<int32_t, uint64_t>::Map Map;
    Fixture<Map> f(64);
    Map &map = f.map;

    for (int32_t i = 0; i < 10; i++) ASSERT_TRUE(map.put(i, 0));

    ASSERT_TRUE(map.count() == 10);

    for (int32_t i = 0; i < 10; i++) {
        int32_t *key_ptr = map.getKeyPtr(i);
        ASSERT_TRUE(key_ptr != nullptr);

        if (key_ptr) map.removeByPtr(key_ptr);
    }

    ASSERT_TRUE(map.count() == 0);
}

TEST(hash_map, HashMap_removeByPtr_0_sized_key) {
    typedef Auto<int32_t, uint64_t>::Map Map;
    Fixture<Map> f(64);
    Map &map = f.map;

    ASSERT_TRUE(map.put(0, 0));

    ASSERT_TRUE(map.count() == 1);

    int32_t *key_ptr = map.getKeyPtr(0);
    ASSERT_TRUE(key_ptr != nullptr);

    if (key_ptr) map.removeByPtr(key_ptr);

    ASSERT_TRUE(map.count() == 0);
}

struct Void { bool operator==(const Void &) const { return true; } };

TEST(hash_map, HashMap_repeat_fetchRemove) {
    typedef Auto<uint64_t, Void>::Map Map;
    Fixture<Map> f(64);
    Map &map = f.map;

    map.putAssumeCapacity(0, Void());
    map.putAssumeCapacity(1, Void());
    map.putAssumeCapacity(2, Void());
    map.putAssumeCapacity(3, Void());

    /* fetchRemove() should make slots available. */
    for (size_t i = 0; i < 10; i++) {
        Map::KV kv;
        ASSERT_TRUE(map.fetchRemove(3, &kv));
        map.putAssumeCapacity(3, Void());
    }

    Void v;
    ASSERT_TRUE(map.get(0, &v));
    ASSERT_TRUE(map.get(1, &v));
    ASSERT_TRUE(map.get(2, &v));
    ASSERT_TRUE(map.get(3, &v));
}

TEST(hash_map, OffsetHashMap_basic_usage) {
    typedef AutoOffsetHashMap<uint32_t, uint32_t, default_max_load_percentage>::Type OffsetMap;
    const OffsetMap::Layout layout = OffsetMap::layout(16);
    Buf buf(layout.total_size, OffsetMap::base_align);
    OffsetMap offset_map = OffsetMap::init(size::OffsetBuf::init(buf.ptr), layout);
    OffsetMap::Unmanaged map = offset_map.map((const void *)buf.ptr);

    const uint32_t count = 5;
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        ASSERT_TRUE(map.put(i, i));
        total += i;
    }

    uint32_t sum = 0;
    OffsetMap::Unmanaged::Iterator it = map.iterator();
    OffsetMap::Unmanaged::Entry kv;
    while (it.next(&kv)) sum += *kv.key_ptr;
    ASSERT_TRUE(total == sum);

    sum = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v;
        ASSERT_TRUE(map.get(i, &v) && v == i);
        sum += v;
    }
    ASSERT_TRUE(total == sum);
}

TEST(hash_map, OffsetHashMap_remake_map) {
    typedef AutoOffsetHashMap<uint32_t, uint32_t, default_max_load_percentage>::Type OffsetMap;
    const OffsetMap::Layout layout = OffsetMap::layout(16);
    Buf buf(layout.total_size, OffsetMap::base_align);
    OffsetMap offset_map = OffsetMap::init(size::OffsetBuf::init(buf.ptr), layout);

    {
        OffsetMap::Unmanaged map = offset_map.map((const void *)buf.ptr);
        ASSERT_TRUE(map.put(5, 5));
    }

    {
        OffsetMap::Unmanaged map = offset_map.map((const void *)buf.ptr);
        uint32_t v;
        ASSERT_TRUE(map.get(5, &v) && v == 5);
    }
}

TEST(hash_map, OffsetHashMap_maximum_load_leaves_probe_headroom) {
    typedef AutoOffsetHashMap<uint32_t, uint32_t, 80>::Type OffsetMap;
    const uint32_t requested_size = 16;
    const OffsetMap::Layout layout = OffsetMap::layout(requested_size);
    Buf buf(layout.total_size, OffsetMap::base_align);

    const OffsetMap offset_map = OffsetMap::init(size::OffsetBuf::init(buf.ptr), layout);
    OffsetMap::Unmanaged map = offset_map.map((const void *)buf.ptr);

    ASSERT_TRUE(map.capacity() > requested_size);
    ASSERT_TRUE(map.maxLoad() >= requested_size);
    ASSERT_TRUE(map.maxLoad() < map.capacity());

    for (uint32_t i = 0; i < requested_size; i++) ASSERT_TRUE(map.put(i, i));
    for (size_t n = 0; n < 100; n++) {
        for (uint32_t i = 0; i < requested_size; i++) {
            ASSERT_TRUE(map.remove(i));
            ASSERT_TRUE(map.put(i, i));
        }
    }

    for (uint32_t i = 0; i < requested_size; i++) {
        uint32_t v;
        ASSERT_TRUE(map.get(i, &v) && v == i);
    }
}

TEST(hash_map, layoutForCapacity_no_overflow_for_large_capacity) {
    /* Test that layoutForCapacity correctly handles large capacities without overflow.
     * Prior to the fix, new_capacity (u32) was multiplied before widening to usize,
     * causing overflow when new_capacity * @sizeOf(K) exceeded 2^32.
     * See: https://github.com/ghostty-org/ghostty/issues/9862 */
    typedef Auto<uint64_t, uint64_t>::Map Map;

    /* Use 2^30 capacity - this would overflow in u32 when multiplied by @sizeOf(u64)=8
     * 0x40000000 * 8 = 0x2_0000_0000 which wraps to 0 in u32 */
    const Map::Size large_cap = (Map::Size)1 << 30;
    const Map::Layout layout = Map::layoutForCapacity(large_cap);

    /* With the fix, total_size should be at least cap * (sizeof(K) + sizeof(V))
     * = 2^30 * 16 = 2^34 bytes = 16 GiB
     * Without the fix, this would wrap and produce a much smaller value. */
    const size_t min_expected = (size_t)large_cap * (sizeof(uint64_t) + sizeof(uint64_t));
    ASSERT_TRUE(layout.total_size >= min_expected);

    /* Also verify the individual offsets don't wrap */
    ASSERT_TRUE(layout.keys_start > 0);
    ASSERT_TRUE(layout.vals_start > layout.keys_start);
}
