/* Transliterated from Ghostty src/terminal/hash_map.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * This file contains a fork of the Zig stdlib HashMap implementation tuned
 * for use with our terminal page representation.
 *
 * The main goal we need to achieve that wasn't possible with the stdlib
 * HashMap is to utilize offsets rather than full pointers so that we can
 * copy around the entire backing memory and keep the hash map working.
 *
 * Additionally, for serialization/deserialization purposes, we need to be
 * able to create a HashMap instance and manually set the offsets up. The
 * stdlib HashMap does not export Metadata so this isn't possible.
 *
 * Also, I want to be able to understand possible capacity for a given K,V
 * type and fixed memory amount. The stdlib HashMap doesn't publish its
 * internal allocation size calculation.
 *
 * Finally, I removed many of the APIs that we'll never require for our
 * usage just so that this file is smaller, easier to understand, and has
 * less opportunity for bugs.
 *
 * Besides these shortcomings, the stdlib HashMap has some great qualities
 * that we want to keep, namely the fact that it is backed by a single large
 * allocation rather than pointers to separate allocations. This is important
 * because our terminal page representation is backed by a single large
 * allocation so we can give the HashMap a slice of memory to operate in.
 *
 * This fork diverges from the stdlib in one significant way: removal uses
 * backward-shift deletion (Knuth vol. 3, section 6.4, algorithm R) rather
 * than tombstones. A fixed-capacity map cannot outgrow tombstone buildup
 * the way an allocating map does, so tombstones require either unbounded
 * probe lengths or periodic in-place rebuilds with subtle bookkeeping.
 * Backward-shift deletion instead restores the table after every removal
 * to the exact state it would be in had the removed key never been
 * inserted. Probe chains are therefore always minimal for the insertion
 * order, there is no fragmentation to repair, and lookup cost depends only
 * on the current load factor.
 *
 * Pointer stability: insertion never moves existing entries, but removal
 * may move *other* entries within a probe cluster. Any key or value
 * pointers previously returned by the map must be considered invalidated
 * by any removal.
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: Allocator.Error!T is a bool return (false for OutOfMemory) with T
 * through an out parameter; ?T is the same shape. The `...Context` and
 * `...Adapted` variants take the context by value, as upstream; the
 * context-less forms construct a default Context, which is what upstream's
 * `undefined` context means for the zero-sized contexts that allow them.
 */

#pragma once
#ifndef WISP_VT_HASH_MAP_HPP
#define WISP_VT_HASH_MAP_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "size.hpp"
#include "../zigstd/wyhash.hpp"

namespace wisp {
namespace vt {
namespace hash_map {

/* The default allows every raw slot to be occupied. Callers whose maps see
 * removal-heavy churn should choose a lower value to bound probe lengths. */
static const uint8_t default_max_load_percentage = 100;

/* Wisp: AutoContext(K) for keys with a unique representation (integers,
 * Offsets, and structs of them without padding), which is every key this
 * map is used with. Upstream hashes the key's bytes with Wyhash seed 0
 * (the wasm32 splitmix branch does not apply to this target) and compares
 * with ==. */
template <typename K>
struct AutoContext {
    uint64_t hash(const K &key) const {
        /* LLVM 21 (Zig 0.16) failed to inline this which resulted
         * in a measurable almost 2x slowdown on our hyperlink map
         * benchmark. So, force it. */
        return ::wisp::zigstd::Wyhash::hash(0, &key, sizeof(K));
    }

    bool eql(const K &a, const K &b) const { return memcmp(&a, &b, sizeof(K)) == 0; }
};

inline size_t alignForward(size_t v, size_t a) { return (v + a - 1) / a * a; }

/* Fork of stdlib.HashMap as of Zig 0.12 modified to use offsets for
 * the key/values pointer, and backward-shift deletion in place of
 * tombstones. The metadata is still a pointer to limit the amount of
 * arithmetic required to access it. See the file comment for full details. */
template <typename K, typename V, typename Context, uint8_t max_load_percentage>
struct HashMapUnmanaged {
    static_assert(max_load_percentage > 0, "max_load_percentage > 0");
    static_assert(max_load_percentage <= 100, "max_load_percentage <= 100");

    /* This hashmap is specially designed for sizes that fit in a u32. */
    typedef uint32_t Size;

    /* u64 hashes guarantee us that the fingerprint bits will never be used
     * to compute the index of a slot, maximizing the use of entropy. */
    typedef uint64_t Hash;

    /* The part of the map's state that changes after init. It lives
     * in the backing buffer so that the map can be handed around by
     * value; its zero value is the empty map. */
    struct Header {
        Size size;
    };

    static const size_t header_align = alignof(Header);
    static const size_t key_align = alignof(K);
    static const size_t val_align = alignof(V);
    static const size_t base_align =
        header_align > key_align
            ? (header_align > val_align ? header_align : val_align)
            : (key_align > val_align ? key_align : val_align);

    /* Metadata for a slot. It can be in two states: free or used.
     * To the used state, we add 7 bits from the slot's key hash. These
     * are used as a fast way to disambiguate between entries without
     * having to use the equality function. If two fingerprints are
     * different, we know that we don't have to compare the keys at all.
     * The 7 bits are the highest ones from a 64 bit hash. This way, not
     * only we use the `log2(capacity)` lowest bits from the hash to determine
     * a slot index, but we use 7 more bits to quickly resolve collisions
     * when multiple elements with different hashes end up wanting to be in the same slot.
     * Not using the equality function means we don't have to read into
     * the entries array, likely avoiding a cache miss and a potentially
     * costly function call.
     *
     * Wisp: packed struct(u8) { fingerprint: u7, used: u1 } — fingerprint in
     * the low seven bits, used in the top bit. */
    struct Metadata {
        uint8_t bits;

        uint8_t fingerprint() const { return bits & 0x7F; }

        bool isUsed() const { return (bits >> 7) == 1; }

        bool isFree() const {
            /* A free slot is always the all-zero byte: `fill` sets the
             * used bit and removal zeroes the whole byte. Comparing the
             * full byte (rather than testing the used bit) lets the
             * optimizer fuse this with the fingerprint comparison in
             * probe loops into single-byte compares. */
            return bits == 0;
        }

        static uint8_t takeFingerprint(Hash hash) {
            return (uint8_t)((hash >> (64 - 7)) & 0x7F);
        }

        void fill(uint8_t fp) { bits = (uint8_t)(0x80 | (fp & 0x7F)); }
    };
    static_assert(sizeof(Metadata) == 1, "Metadata is one byte");

    /* The backing buffer holds a `Header` (the size counter) followed
     * by the `Metadata`s, then the keys and values arrays. Everything
     * the map needs that does not change after init, the capacity and
     * the entry pointers, lives in this struct instead of the buffer,
     * so that a zero-filled buffer is a valid empty map and init never
     * has to write to it.
     * Pointer to the slot metadata. The header sits right before it. */
    Metadata *metadata;

    /* The key and value arrays. */
    K *keys;
    V *values;

    /* The number of slots. Always zero or a power of two. */
    Size cap;

    struct Entry {
        K *key_ptr;
        V *value_ptr;
    };

    struct KV {
        K key;
        V value;
    };

    struct GetOrPutResult {
        K *key_ptr;
        V *value_ptr;
        bool found_existing;
    };

    /* The memory layout for the underlying buffer for a given capacity.
     * All offsets are from the start of the buffer. */
    struct Layout {
        /* The total size of the buffer required. The buffer is expected
         * to be aligned to `base_align`. */
        size_t total_size;

        /* The offset to the start of the slot metadata. The header
         * occupies the bytes before it. */
        size_t metadata_start;

        /* The offset to the start of the keys data. */
        size_t keys_start;

        /* The offset to the start of the values data. */
        size_t vals_start;

        /* The capacity that was used to calculate this layout. */
        Size capacity;
    };

    /* Iterates the entries of the map. Any mutation of the map
     * invalidates the iterator: removal may move entries across the
     * iteration cursor. */
    struct Iterator {
        const HashMapUnmanaged *hm;
        Size index; /* = 0 */

        bool next(Entry *out) {
            /* assert(it.index <= it.hm.cap) */
            if (hm->header()->size == 0) return false;

            const Size c = hm->cap;
            while (index != c) {
                if (hm->metadata[index].isUsed()) {
                    out->key_ptr = &hm->keys[index];
                    out->value_ptr = &hm->values[index];
                    index += 1;
                    return true;
                }
                index += 1;
            }

            return false;
        }
    };

    template <typename T>
    struct FieldIterator {
        size_t len;
        const Metadata *metadata;
        T *items;

        T *next() {
            while (len > 0) {
                len -= 1;
                const bool used = metadata[0].isUsed();
                T *item = &items[0];
                metadata += 1;
                items += 1;
                if (used) return item;
            }
            return nullptr;
        }
    };

    typedef FieldIterator<K> KeyIterator;
    typedef FieldIterator<V> ValueIterator;

    /* Initialize a hash map with a given capacity and a buffer. The
     * buffer must fit within the size defined by `layoutForCapacity`. */
    static HashMapUnmanaged init(size::OffsetBuf buf, Layout layout) {
        HashMapUnmanaged map = initAssumeZeroed(buf, layout);
        map.clearRetainingCapacity();
        return map;
    }

    /* Like `init`, but for a buffer that the caller guarantees is
     * already zero-filled. Nothing is written: an all-zero metadata
     * byte is a free slot (see `Metadata.isFree`) and a zero header
     * is an empty map. Behavior is undefined if the header and
     * metadata region is not zero. */
    static HashMapUnmanaged initAssumeZeroed(size::OffsetBuf buf, Layout layout) {
        /* assert(base_align.check(@intFromPtr(buf.start()))) */
        HashMapUnmanaged m;
        m.metadata = (Metadata *)(void *)(buf.start() + layout.metadata_start);
        m.keys = buf.member<K>(layout.keys_start).ptr(buf);
        m.values = buf.member<V>(layout.vals_start).ptr(buf);
        m.cap = layout.capacity;
        return m;
    }

    bool ensureTotalCapacity(Size new_size) {
        if (new_size > header()->size) {
            return checkCapacity(new_size - header()->size);
        }
        return true;
    }

    bool ensureUnusedCapacity(Size additional_size) {
        return ensureTotalCapacity(count() + additional_size);
    }

    void clearRetainingCapacity() {
        initMetadatas();
        header()->size = 0;
    }

    Size count() const { return header()->size; }

    Header *header() const {
        return (Header *)(void *)metadata - 1;
    }

    Size capacity() const { return cap; }

    /* Maximum number of entries the map will hold. This is less than
     * capacity when max_load_percentage is below 100, which keeps free
     * slots in every probe chain and bounds probe lengths. */
    Size maxLoad() const { return maxLoadForCapacity(cap); }

    Iterator iterator() const {
        Iterator it;
        it.hm = this;
        it.index = 0;
        return it;
    }

    KeyIterator keyIterator() const {
        KeyIterator it;
        it.len = cap;
        it.metadata = metadata;
        it.items = keys;
        return it;
    }

    ValueIterator valueIterator() const {
        ValueIterator it;
        it.len = cap;
        it.metadata = metadata;
        it.items = values;
        return it;
    }

    /* Insert an entry in the map. Assumes it is not already present. */
    bool putNoClobber(const K &key, const V &value) {
        return putNoClobberContext(key, value, Context());
    }
    bool putNoClobberContext(const K &key, const V &value, Context ctx) {
        /* assert(!self.containsContext(key, ctx)) */
        if (!checkCapacity(1)) return false;

        putAssumeCapacityNoClobberContext(key, value, ctx);
        return true;
    }

    /* Asserts there is enough capacity to store the new key-value pair.
     * Clobbers any existing data. To detect if a put would clobber
     * existing data, see `getOrPutAssumeCapacity`. */
    void putAssumeCapacity(const K &key, const V &value) {
        putAssumeCapacityContext(key, value, Context());
    }
    void putAssumeCapacityContext(const K &key, const V &value, Context ctx) {
        const GetOrPutResult gop = getOrPutAssumeCapacityContext(key, ctx);
        *gop.value_ptr = value;
    }

    /* Insert an entry in the map. Assumes it is not already present,
     * and that no allocation is needed. */
    void putAssumeCapacityNoClobber(const K &key, const V &value) {
        putAssumeCapacityNoClobberContext(key, value, Context());
    }
    void putAssumeCapacityNoClobberContext(const K &key, const V &value, Context ctx) {
        /* assert(!self.containsContext(key, ctx)) */

        /* A free slot must exist for the probe below to terminate.
         * assert(self.header().size < self.cap) */

        const Hash hash = ctx.hash(key);
        const size_t mask = cap - 1;
        size_t idx = (size_t)(hash & mask);

        while (metadata[idx].isUsed()) {
            idx = (idx + 1) & mask;
        }

        metadata[idx].fill(Metadata::takeFingerprint(hash));
        keys[idx] = key;
        values[idx] = value;
        header()->size += 1;
    }

    /* Inserts a new `Entry` into the hash map, returning the previous one, if any.
     * Wisp: false is OutOfMemory; *had is whether *prev holds the old entry. */
    bool fetchPut(const K &key, const V &value, bool *had, KV *prev) {
        return fetchPutContext(key, value, Context(), had, prev);
    }
    bool fetchPutContext(const K &key, const V &value, Context ctx, bool *had, KV *prev) {
        GetOrPutResult gop;
        if (!getOrPutContext(key, ctx, &gop)) return false;
        *had = false;
        if (gop.found_existing) {
            *had = true;
            prev->key = *gop.key_ptr;
            prev->value = *gop.value_ptr;
        }
        *gop.value_ptr = value;
        return true;
    }

    /* Inserts a new `Entry` into the hash map, returning the previous one, if any.
     * If insertion happens, asserts there is enough capacity without allocating. */
    bool fetchPutAssumeCapacity(const K &key, const V &value, KV *prev) {
        return fetchPutAssumeCapacityContext(key, value, Context(), prev);
    }
    bool fetchPutAssumeCapacityContext(const K &key, const V &value, Context ctx, KV *prev) {
        const GetOrPutResult gop = getOrPutAssumeCapacityContext(key, ctx);
        bool result = false;
        if (gop.found_existing) {
            result = true;
            prev->key = *gop.key_ptr;
            prev->value = *gop.value_ptr;
        }
        *gop.value_ptr = value;
        return result;
    }

    /* If there is an `Entry` with a matching key, it is deleted from
     * the hash map, and then returned from this function. Removal may
     * move other entries: any previously returned key or value
     * pointers are invalidated. */
    bool fetchRemove(const K &key, KV *out) { return fetchRemoveContext(key, Context(), out); }
    bool fetchRemoveContext(const K &key, Context ctx, KV *out) {
        size_t idx;
        if (!getIndex(key, ctx, &idx)) return false;
        out->key = keys[idx];
        out->value = values[idx];
        removeByIndexContext(idx, ctx);
        return true;
    }

    /* Find the index containing the data for the given key.
     * Whether this function returns null is almost always
     * branched on after this function returns, and this function
     * returns null/not null from separate code paths.  We
     * want the optimizer to remove that branch and instead directly
     * fuse the basic blocks after the branch to the basic blocks
     * from this function.  To encourage that, this function is
     * marked as inline. */
    template <typename AK, typename C>
    bool getIndex(const AK &key, C ctx, size_t *out) const {
        if (header()->size == 0) return false;

        const Hash hash = ctx.hash(key);
        const size_t mask = cap - 1;
        const uint8_t fingerprint = Metadata::takeFingerprint(hash);
        /* Don't loop indefinitely when there are no free slots. */
        Size limit = cap;
        size_t idx = (size_t)(hash & mask);

        while (!metadata[idx].isFree() && limit != 0) {
            if (metadata[idx].isUsed() && metadata[idx].fingerprint() == fingerprint) {
                if (ctx.eql(key, keys[idx])) {
                    *out = idx;
                    return true;
                }
            }

            limit -= 1;
            idx = (idx + 1) & mask;
        }

        return false;
    }

    bool getEntry(const K &key, Entry *out) const { return getEntryContext(key, Context(), out); }
    bool getEntryContext(const K &key, Context ctx, Entry *out) const {
        return getEntryAdapted(key, ctx, out);
    }
    template <typename AK, typename C>
    bool getEntryAdapted(const AK &key, C ctx, Entry *out) const {
        size_t idx;
        if (getIndex(key, ctx, &idx)) {
            out->key_ptr = &keys[idx];
            out->value_ptr = &values[idx];
            return true;
        }
        return false;
    }

    /* Insert an entry if the associated key is not already present, otherwise update preexisting value. */
    bool put(const K &key, const V &value) { return putContext(key, value, Context()); }
    bool putContext(const K &key, const V &value, Context ctx) {
        GetOrPutResult result;
        if (!getOrPutContext(key, ctx, &result)) return false;
        *result.value_ptr = value;
        return true;
    }

    /* Get an optional pointer to the actual key associated with adapted key, if present. */
    K *getKeyPtr(const K &key) const { return getKeyPtrContext(key, Context()); }
    K *getKeyPtrContext(const K &key, Context ctx) const { return getKeyPtrAdapted(key, ctx); }
    template <typename AK, typename C>
    K *getKeyPtrAdapted(const AK &key, C ctx) const {
        size_t idx;
        if (getIndex(key, ctx, &idx)) return &keys[idx];
        return nullptr;
    }

    /* Get a copy of the actual key associated with adapted key, if present. */
    bool getKey(const K &key, K *out) const { return getKeyContext(key, Context(), out); }
    bool getKeyContext(const K &key, Context ctx, K *out) const { return getKeyAdapted(key, ctx, out); }
    template <typename AK, typename C>
    bool getKeyAdapted(const AK &key, C ctx, K *out) const {
        size_t idx;
        if (getIndex(key, ctx, &idx)) {
            *out = keys[idx];
            return true;
        }
        return false;
    }

    /* Get an optional pointer to the value associated with key, if present. */
    V *getPtr(const K &key) const { return getPtrContext(key, Context()); }
    V *getPtrContext(const K &key, Context ctx) const { return getPtrAdapted(key, ctx); }
    template <typename AK, typename C>
    V *getPtrAdapted(const AK &key, C ctx) const {
        size_t idx;
        if (getIndex(key, ctx, &idx)) return &values[idx];
        return nullptr;
    }

    /* Get a copy of the value associated with key, if present. */
    bool get(const K &key, V *out) const { return getContext(key, Context(), out); }
    bool getContext(const K &key, Context ctx, V *out) const { return getAdapted(key, ctx, out); }
    template <typename AK, typename C>
    bool getAdapted(const AK &key, C ctx, V *out) const {
        size_t idx;
        if (getIndex(key, ctx, &idx)) {
            *out = values[idx];
            return true;
        }
        return false;
    }

    bool getOrPut(const K &key, GetOrPutResult *out) { return getOrPutContext(key, Context(), out); }
    bool getOrPutContext(const K &key, Context ctx, GetOrPutResult *out) {
        if (!getOrPutContextAdapted(key, ctx, out)) return false;
        if (!out->found_existing) {
            *out->key_ptr = key;
        }
        return true;
    }
    template <typename AK, typename C>
    bool getOrPutAdapted(const AK &key, C key_ctx, GetOrPutResult *out) {
        return getOrPutContextAdapted(key, key_ctx, out);
    }
    template <typename AK, typename C>
    bool getOrPutContextAdapted(const AK &key, C key_ctx, GetOrPutResult *out) {
        if (!checkCapacity(1)) {
            /* The map is full. Try to do the lookup anyway; if we find
             * an existing item, we can return it. Otherwise return the
             * error, we could not add another. */
            size_t index;
            if (!getIndex(key, key_ctx, &index)) return false;
            out->key_ptr = &keys[index];
            out->value_ptr = &values[index];
            out->found_existing = true;
            return true;
        }
        *out = getOrPutAssumeCapacityAdapted(key, key_ctx);
        return true;
    }

    GetOrPutResult getOrPutAssumeCapacity(const K &key) {
        return getOrPutAssumeCapacityContext(key, Context());
    }
    GetOrPutResult getOrPutAssumeCapacityContext(const K &key, Context ctx) {
        const GetOrPutResult result = getOrPutAssumeCapacityAdapted(key, ctx);
        if (!result.found_existing) {
            *result.key_ptr = key;
        }
        return result;
    }
    template <typename AK, typename C>
    GetOrPutResult getOrPutAssumeCapacityAdapted(const AK &key, C ctx) {
        const Hash hash = ctx.hash(key);
        const size_t mask = cap - 1;
        const uint8_t fingerprint = Metadata::takeFingerprint(hash);
        Size limit = cap;
        size_t idx = (size_t)(hash & mask);

        while (!metadata[idx].isFree() && limit != 0) {
            if (metadata[idx].isUsed() && metadata[idx].fingerprint() == fingerprint) {
                K *test_key = &keys[idx];
                if (ctx.eql(key, *test_key)) {
                    GetOrPutResult r;
                    r.key_ptr = test_key;
                    r.value_ptr = &values[idx];
                    r.found_existing = true;
                    return r;
                }
            }

            limit -= 1;
            idx = (idx + 1) & mask;
        }

        /* The caller guaranteed capacity for at least one new entry, so
         * the probe must have ended at a free slot. Anything else means
         * the assume-capacity contract was violated and we would be
         * silently overwriting a live entry.
         * assert(metadata[0].isFree()) */

        metadata[idx].fill(fingerprint);
        GetOrPutResult r;
        r.key_ptr = &keys[idx];
        r.value_ptr = &values[idx];
        r.found_existing = false;
        /* new_key.* = undefined; new_value.* = undefined; */
        header()->size += 1;
        return r;
    }

    bool getOrPutValue(const K &key, const V &value, Entry *out) {
        return getOrPutValueContext(key, value, Context(), out);
    }
    bool getOrPutValueContext(const K &key, const V &value, Context ctx, Entry *out) {
        GetOrPutResult res;
        if (!getOrPutAdapted(key, ctx, &res)) return false;
        if (!res.found_existing) {
            *res.key_ptr = key;
            *res.value_ptr = value;
        }
        out->key_ptr = res.key_ptr;
        out->value_ptr = res.value_ptr;
        return true;
    }

    /* Return true if there is a value associated with key in the map. */
    bool contains(const K &key) const { return containsContext(key, Context()); }
    bool containsContext(const K &key, Context ctx) const { return containsAdapted(key, ctx); }
    template <typename AK, typename C>
    bool containsAdapted(const AK &key, C ctx) const {
        size_t idx;
        return getIndex(key, ctx, &idx);
    }

    /* Remove the entry at the given index using backward-shift deletion
     * (Knuth vol. 3, section 6.4, algorithm R): rather than marking the
     * slot with a tombstone, restore the table to the state it would be
     * in had the removed key never been inserted. Any entry whose probe
     * sequence passes over the hole is moved into it, which moves the
     * hole further along the cluster, until the cluster ends at a free
     * slot. */
    void removeByIndexContext(size_t idx, Context ctx) {
        const size_t mask = (size_t)cap - 1;

        /* A completely full table has no free slot to terminate the
         * scan, so bound it to one full cycle. That is sufficient: the
         * hole only ever moves forward to slots the scan has already
         * visited, so each entry needs to be considered exactly once. */
        size_t hole = idx;
        size_t j = idx;
        Size limit = cap - 1;
        while (limit != 0) {
            j = (j + 1) & mask;
            if (metadata[j].isFree()) break;

            /* The entry at `j` may move into the hole only if the hole
             * lies on its probe path, i.e. cyclically within [home, j).
             * Otherwise the move would place it before its home slot
             * and lookups could no longer find it. */
            const size_t home = (size_t)(ctx.hash(keys[j]) & mask);
            if (((hole - home) & mask) < ((j - home) & mask)) {
                metadata[hole] = metadata[j];
                keys[hole] = keys[j];
                values[hole] = values[j];
                hole = j;
            }
            limit -= 1;
        }

        metadata[hole].bits = 0;
        /* keys_ptr[hole] = undefined; values_ptr[hole] = undefined; */
        header()->size -= 1;
    }

    /* If there is an `Entry` with a matching key, it is deleted from
     * the hash map, and this function returns true.  Otherwise this
     * function returns false. Removal may move other entries: any
     * previously returned key or value pointers are invalidated. */
    bool remove(const K &key) { return removeContext(key, Context()); }
    bool removeContext(const K &key, Context ctx) {
        size_t idx;
        if (!getIndex(key, ctx, &idx)) return false;
        removeByIndexContext(idx, ctx);
        return true;
    }

    /* Delete the entry with key pointed to by key_ptr from the hash map.
     * key_ptr is assumed to be a valid pointer to a key that is present
     * in the hash map. Removal may move other entries: any previously
     * returned key or value pointers are invalidated. */
    void removeByPtr(K *key_ptr) { removeByPtrContext(key_ptr, Context()); }
    void removeByPtrContext(K *key_ptr, Context ctx) {
        const size_t idx = (size_t)(key_ptr - keys);
        removeByIndexContext(idx, ctx);
    }

    void initMetadatas() { memset(metadata, 0, sizeof(Metadata) * cap); }

    /* Returns an error if the map cannot hold `new_count` more entries.
     * This map is fixed-capacity so nothing can be done to make room;
     * the caller must grow the backing memory and rebuild the map. */
    bool checkCapacity(Size new_count) const {
        const Size available = maxLoad() - header()->size;
        return !(new_count > available);
    }

    static Size maxLoadForCapacity(Size c) {
        if (c == 0) return 0;
        return (Size)(((uint64_t)c * max_load_percentage) / 100);
    }

    /* Returns the memory layout for the buffer for a given capacity.
     * The actual size may be able to fit more than the given capacity
     * because capacity is rounded up to the next power of two. This is
     * a design requirement for this hash map implementation. */
    static Layout layoutForCapacity(Size new_capacity) {
        /* assert(new_capacity == 0 or std.math.isPowerOfTwo(new_capacity)) */

        /* Cast to usize to prevent overflow in size calculations.
         * See: https://github.com/ziglang/zig/pull/19048 */
        const size_t c = new_capacity;

        /* Pack our header, metadata, keys, and values. */
        const size_t meta_start = sizeof(Header);
        const size_t meta_end = meta_start + c * sizeof(Metadata);
        const size_t keys_start = alignForward(meta_end, key_align);
        const size_t keys_end = keys_start + c * sizeof(K);
        const size_t vals_start = alignForward(keys_end, val_align);
        const size_t vals_end = vals_start + c * sizeof(V);

        /* Our total memory size required is the end of our values
         * aligned to the base required alignment. */
        const size_t total_size = alignForward(vals_end, base_align);

        Layout l;
        l.total_size = total_size;
        l.metadata_start = meta_start;
        l.keys_start = keys_start;
        l.vals_start = vals_start;
        l.capacity = new_capacity;
        return l;
    }

    /* Returns a layout with enough raw slots to hold `new_size` entries
     * at the configured maximum load factor. */
    static Layout layoutForSize(Size new_size) {
        if (new_size == 0) return layoutForCapacity(0);

        /* Scale the requested number of entries up to the raw slot count
         * required by the load factor. Widen first so `new_size * 100`
         * cannot overflow Size. */
        const uint64_t minimum_capacity =
            ((uint64_t)new_size * 100 + max_load_percentage - 1) / max_load_percentage;

        /* Capacities must be powers of two, so the largest capacity that
         * fits in Size is the highest bit rather than maxInt(Size). */
        const uint64_t max_capacity = (uint64_t)1 << (32 - 1);
        if (minimum_capacity > max_capacity) {
            return layoutForCapacity((Size)max_capacity);
        }

        /* Linear probing uses a mask for wraparound, which requires the
         * final raw capacity to be rounded up to a power of two. */
        uint64_t raw_capacity = 1;
        while (raw_capacity < minimum_capacity) raw_capacity <<= 1;
        return layoutForCapacity((Size)raw_capacity);
    }
};

/* A HashMap type that uses offsets rather than pointers, making it
 * possible to efficiently move around the backing memory without
 * invalidating the HashMap. */
template <typename K, typename V, typename Context, uint8_t max_load_percentage>
struct OffsetHashMap {
    /* This is the pointer-based map that we're wrapping. */
    typedef HashMapUnmanaged<K, V, Context, max_load_percentage> Unmanaged;
    typedef typename Unmanaged::Layout Layout;

    /* This is the alignment that the base pointer must have. */
    static const size_t base_align = Unmanaged::base_align;

    /* The slot metadata in the backing memory. The map's size counter
     * sits immediately before it (see `Unmanaged.Header`). */
    size::Offset<typename Unmanaged::Metadata> metadata; /* = .{} */

    /* The key and value arrays in the backing memory. */
    size::Offset<K> keys;   /* = .{} */
    size::Offset<V> values; /* = .{} */

    /* The number of slots. This never changes after init, so it is
     * kept here rather than in the backing memory. */
    typename Unmanaged::Size capacity; /* = 0 */

    OffsetHashMap() : metadata(), keys(), values(), capacity(0) {}

    /* Returns the total size of the backing memory required for a
     * HashMap with the given capacity. The base ptr must also be
     * aligned to base_align. */
    static Layout layout(typename Unmanaged::Size cap) { return Unmanaged::layoutForSize(cap); }

    /* Initialize a new HashMap with the given capacity and backing
     * memory. The backing memory must be aligned to base_align. */
    static OffsetHashMap init(size::OffsetBuf buf, Layout l) {
        const OffsetHashMap self = initAssumeZeroed(buf, l);
        Unmanaged m = self.map(buf);
        m.clearRetainingCapacity();
        return self;
    }

    /* Like `init`, but for backing memory that the caller guarantees
     * is already zero-filled (e.g. fresh OS pages). This writes
     * nothing to the backing memory: all-zero slot metadata means
     * every slot is free and a zero size counter means empty, so the
     * OS pages behind the map stay untouched until the first insert. */
    static OffsetHashMap initAssumeZeroed(size::OffsetBuf buf, Layout l) {
        /* assert(base_align.check(@intFromPtr(buf.start()))) */
        OffsetHashMap self;
        self.metadata = buf.member<typename Unmanaged::Metadata>(l.metadata_start);
        self.keys = buf.member<K>(l.keys_start);
        self.values = buf.member<V>(l.vals_start);
        self.capacity = l.capacity;
        return self;
    }

    /* Returns the pointer-based map from a base pointer. */
    template <typename B>
    Unmanaged map(const B &base) const {
        Unmanaged m;
        m.metadata = metadata.ptr(base);
        m.keys = keys.ptr(base);
        m.values = values.ptr(base);
        m.cap = capacity;
        return m;
    }
};

template <typename K, typename V, uint8_t max_load_percentage>
struct AutoOffsetHashMap {
    typedef OffsetHashMap<K, V, AutoContext<K>, max_load_percentage> Type;
};

} /* namespace hash_map */
} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_HASH_MAP_HPP */
