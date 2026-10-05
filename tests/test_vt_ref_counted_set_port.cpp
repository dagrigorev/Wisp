/* Transliterated from the test blocks in Ghostty
 * src/terminal/ref_counted_set.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include "test_helpers.h"
#include "../vt/ref_counted_set.hpp"
#include "../zigstd/hash_int.hpp"

#include <malloc.h>

using namespace wisp::vt;
using namespace wisp::vt::ref_counted_set;

struct U32Context {
    uint64_t hash(uint32_t value) const { return wisp::zigstd::hash::int_(value); }
    bool eql(uint32_t a, uint32_t b) const { return a == b; }
};

typedef RefCountedSet<uint32_t, uint16_t, uint16_t, U32Context> TestSet;

struct Buf {
    uint8_t *ptr;
    explicit Buf(size_t n) : ptr((uint8_t *)_aligned_malloc(n, TestSet::base_align)) {}
    ~Buf() { _aligned_free(ptr); }
};

static uint16_t addOk(TestSet &set, const void *buf, uint32_t v) {
    uint16_t id = 0;
    const AddError e = set.add(buf, v, &id);
    (void)e;
    return id;
}

TEST(ref_counted_set, addWithId_dead_id_resolving_to_an_existing_value) {
    const TestSet::Layout layout = TestSet::Layout::init(8);
    Buf buf(layout.total_size);
    const void *base = buf.ptr;

    TestSet set = TestSet::init(size::OffsetBuf::init(buf.ptr), layout, U32Context());

    /* Create a dead item between two live ones so that it can't
     * be reaped by the trim loop in `add`, then release it. This
     * mirrors a page style set after a styled run is erased. */
    const uint16_t live = addOk(set, base, 11);
    const uint16_t released = addOk(set, base, 22);
    const uint16_t last = addOk(set, base, 33);
    set.release(base, released);
    ASSERT_TRUE(set.count() == 2);

    /* Request the dead ID for a value that is already live under
     * a different ID: we must resolve to the existing item and
     * take a reference, without changing the living count. */
    bool is_null;
    uint16_t resolved;
    ASSERT_TRUE(set.addWithId(base, 11, released, &is_null, &resolved) == AddError::none);
    ASSERT_FALSE(is_null);
    ASSERT_TRUE(live == resolved);
    ASSERT_TRUE(set.refCount(base, live) == 2);
    ASSERT_TRUE(set.count() == 2);

    /* The living count must agree with the iterator. */
    TestSet::Iterator it = set.iterator(base);
    size_t iterated = 0;
    TestSet::Entry e;
    while (it.next(&e)) iterated += 1;
    ASSERT_TRUE(set.count() == iterated);

    /* The dead slot is still reusable for a value that is not
     * in the set: the requested ID must be used (null return). */
    uint16_t unused;
    ASSERT_TRUE(set.addWithId(base, 44, released, &is_null, &unused) == AddError::none);
    ASSERT_TRUE(is_null);
    ASSERT_TRUE(set.refCount(base, released) == 1);
    ASSERT_TRUE(set.count() == 3);

    (void)last;
}

TEST(ref_counted_set, iterator_visits_live_entries_in_ID_order) {
    const TestSet::Layout layout = TestSet::Layout::init(8);
    Buf buf(layout.total_size);
    const void *base = buf.ptr;

    TestSet set = TestSet::init(size::OffsetBuf::init(buf.ptr), layout, U32Context());
    const uint16_t first = addOk(set, base, 11);
    const uint16_t released = addOk(set, base, 22);
    const uint16_t last = addOk(set, base, 33);
    (void)addOk(set, base, 11);
    set.release(base, released);

    TestSet::Iterator it = set.iterator(base);

    TestSet::Entry first_entry;
    ASSERT_TRUE(it.next(&first_entry));
    ASSERT_TRUE(first == first_entry.id);
    ASSERT_TRUE(*first_entry.value_ptr == 11);

    TestSet::Entry last_entry;
    ASSERT_TRUE(it.next(&last_entry));
    ASSERT_TRUE(last == last_entry.id);
    ASSERT_TRUE(*last_entry.value_ptr == 33);

    TestSet::Entry none;
    ASSERT_FALSE(it.next(&none));
}
