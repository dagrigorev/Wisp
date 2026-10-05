/* Corresponds to Ghostty src/terminal/hash_map.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * An offset-addressed, open-addressed hash map for use inside a Page.
 *
 * PROVENANCE. Upstream's hash_map.zig is a fork of Zig's standard library
 * HashMap, adapted for offset addressing. Transliterating a stdlib fork into
 * C++ would carry over a lot of API that exists only because Zig's stdlib had
 * it, so this is written natively against the same design and the same
 * documented requirements, scoped to what the Page actually needs.
 *
 * The requirements, from upstream's own header:
 *
 *   - Offsets rather than pointers, so the whole backing allocation can be
 *     copied or serialized and the map keeps working.
 *   - Metadata reachable by the caller, so a map can be reconstructed by
 *     setting offsets up by hand.
 *   - A published capacity calculation, so a caller can work out what fits in
 *     a fixed amount of memory.
 *   - One large allocation, matching how a Page is stored.
 *   - Backward-shift deletion rather than tombstones. A fixed-capacity map
 *     cannot outgrow tombstone buildup the way an allocating one does, so
 *     tombstones would mean unbounded probe lengths or periodic in-place
 *     rebuilds. Backward shift instead restores the table to the state it
 *     would have had if the removed key were never inserted, so probe chains
 *     stay minimal and lookup cost depends only on the live load factor.
 *
 * Both real consumers map Offset(Cell) to a small integer — graphemes in
 * page.zig and hyperlink IDs in hyperlink.zig — so K and V are required to be
 * trivially copyable here.
 *
 * POINTER STABILITY. Insertion never moves existing entries, but removal may
 * move other entries within a probe cluster. Treat any key or value pointer
 * as invalidated by any removal.
 */

#pragma once
#ifndef WISP_TERMINAL_HASH_MAP_HPP
#define WISP_TERMINAL_HASH_MAP_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "size.hpp"

namespace wisp {
namespace terminal {

/* Every raw slot may be occupied by default. Callers with removal-heavy churn
 * should pick a lower value to bound probe lengths. */
static const uint8_t DEFAULT_MAX_LOAD_PERCENTAGE = 100;

/* One metadata byte per slot: the high bit marks the slot used, the low seven
 * hold a fingerprint of the hash. Comparing fingerprints rejects most
 * non-matching slots without touching the key array at all. */
struct HashMapMetadata {
    uint8_t bits;

    bool used() const { return (bits & 0x80) != 0; }
    bool free() const { return (bits & 0x80) == 0; }
    uint8_t fingerprint() const { return (uint8_t)(bits & 0x7F); }

    void fill(uint8_t fp) { bits = (uint8_t)(0x80 | (fp & 0x7F)); }
    void clear() { bits = 0; }

    /* Taken from the high bits, which linear probing does not consume for the
     * bucket index, so the two are independent. */
    static uint8_t take_fingerprint(uint64_t hash) {
        return (uint8_t)((hash >> 56) & 0x7F);
    }
};

template <typename K, typename V, uint8_t MaxLoadPercentage = DEFAULT_MAX_LOAD_PERCENTAGE>
struct OffsetHashMap {
    typedef uint32_t Size;

    struct Layout {
        Size   cap;              /* slot count, a power of two */
        size_t metadata_start;
        size_t keys_start;
        size_t values_start;
        size_t total_size;

        /* Layout for a given slot capacity. cap is rounded up to a power of
         * two so the bucket index is a mask rather than a modulo. */
        static Layout init(Size requested) {
            Layout l;
            if (requested == 0) {
                l.cap = 0;
                l.metadata_start = l.keys_start = l.values_start = 0;
                l.total_size = 0;
                return l;
            }

            Size cap = 1;
            while (cap < requested) cap <<= 1;

            const size_t md_start = 0;
            const size_t md_end = md_start + (size_t)cap * sizeof(HashMapMetadata);

            const size_t keys_start = align_forward(md_end, alignof(K));
            const size_t keys_end = keys_start + (size_t)cap * sizeof(K);

            const size_t values_start = align_forward(keys_end, alignof(V));
            const size_t values_end = values_start + (size_t)cap * sizeof(V);

            l.cap = cap;
            l.metadata_start = md_start;
            l.keys_start = keys_start;
            l.values_start = values_start;
            l.total_size = values_end;
            return l;
        }
    };

    /* Slots needed to hold n entries at the configured load factor. */
    static Size capacity_for_count(Size n) {
        if (n == 0) return 0;
        /* Round up: n * 100 / MaxLoadPercentage. */
        return (Size)(((uint64_t)n * 100 + MaxLoadPercentage - 1) / MaxLoadPercentage);
    }

    Offset<HashMapMetadata> metadata;
    Offset<K>               keys;
    Offset<V>               values;
    Size                    cap;
    Size                    len;

    static OffsetHashMap init(OffsetBuf buf, const Layout &l) {
        OffsetHashMap m = init_assume_zeroed(buf, l);
        /* Only the metadata needs clearing; keys and values are meaningless
         * while their slot is free. */
        if (l.cap) memset(m.metadata.ptr(buf), 0, (size_t)l.cap * sizeof(HashMapMetadata));
        return m;
    }

    /* For memory the caller guarantees is already zeroed. Writes nothing, so
     * fresh pages stay untouched until the first insert. */
    static OffsetHashMap init_assume_zeroed(OffsetBuf buf, const Layout &l) {
        OffsetHashMap m;
        m.metadata = buf.member<HashMapMetadata>(l.metadata_start);
        m.keys = buf.member<K>(l.keys_start);
        m.values = buf.member<V>(l.values_start);
        m.cap = l.cap;
        m.len = 0;
        return m;
    }

    Size count() const { return len; }

    /* The load ceiling in slots. */
    Size max_len() const {
        return (Size)(((uint64_t)cap * MaxLoadPercentage) / 100);
    }

    template <typename Base>
    void clear(Base base) {
        if (cap) memset(metadata.ptr(base), 0, (size_t)cap * sizeof(HashMapMetadata));
        len = 0;
    }

    /* ─── lookup ─────────────────────────────────────────────────────────── */

    /* Slot holding key, or cap if absent. */
    template <typename Base>
    Size find(Base base, uint64_t hash, const K &key) const {
        if (cap == 0) return 0;

        HashMapMetadata *md = metadata.ptr(base);
        K *ks = keys.ptr(base);

        const Size mask = cap - 1;
        const uint8_t fp = HashMapMetadata::take_fingerprint(hash);
        Size i = (Size)(hash & mask);

        /* A free slot ends the probe: with backward-shift deletion there are
         * no tombstones, so nothing can live past it. */
        for (Size probes = 0; probes < cap; probes++) {
            if (md[i].free()) return cap;
            if (md[i].fingerprint() == fp && ks[i] == key) return i;
            i = (Size)((i + 1) & mask);
        }
        return cap;
    }

    template <typename Base>
    V *get(Base base, uint64_t hash, const K &key) const {
        const Size i = find(base, hash, key);
        if (i == cap) return nullptr;
        return &values.ptr(base)[i];
    }

    template <typename Base>
    bool contains(Base base, uint64_t hash, const K &key) const {
        return find(base, hash, key) != cap;
    }

    /* ─── insertion ──────────────────────────────────────────────────────── */

    struct GetOrPutResult {
        V   *value_ptr;
        bool found_existing;
    };

    /* Find key, inserting it if absent. Returns false only when the map is
     * full, in which case nothing is modified. */
    template <typename Base>
    bool get_or_put(Base base, uint64_t hash, const K &key, GetOrPutResult *out) {
        if (cap == 0) return false;

        HashMapMetadata *md = metadata.ptr(base);
        K *ks = keys.ptr(base);
        V *vs = values.ptr(base);

        const Size mask = cap - 1;
        const uint8_t fp = HashMapMetadata::take_fingerprint(hash);
        Size i = (Size)(hash & mask);

        for (Size probes = 0; probes < cap; probes++) {
            if (md[i].free()) {
                /* Refuse before exceeding the load ceiling, so probe lengths
                 * stay bounded. */
                if (len >= max_len()) return false;

                md[i].fill(fp);
                ks[i] = key;
                len++;
                out->value_ptr = &vs[i];
                out->found_existing = false;
                return true;
            }
            if (md[i].fingerprint() == fp && ks[i] == key) {
                out->value_ptr = &vs[i];
                out->found_existing = true;
                return true;
            }
            i = (Size)((i + 1) & mask);
        }

        return false;
    }

    template <typename Base>
    bool put(Base base, uint64_t hash, const K &key, const V &value) {
        GetOrPutResult r;
        if (!get_or_put(base, hash, key, &r)) return false;
        *r.value_ptr = value;
        return true;
    }

    /* ─── removal ────────────────────────────────────────────────────────── */

    /* Remove key. Returns whether it was present.
     *
     * Backward-shift deletion, Knuth vol. 3 section 6.4 algorithm R: after
     * clearing the slot, walk forward and pull back any entry that probed past
     * the hole, so no live entry is left stranded behind a free slot. With
     * tombstones this would instead accumulate until a rebuild. */
    template <typename Base, typename HashFn>
    bool remove(Base base, uint64_t hash, const K &key, HashFn hash_of) {
        const Size start = find(base, hash, key);
        if (start == cap) return false;

        HashMapMetadata *md = metadata.ptr(base);
        K *ks = keys.ptr(base);
        V *vs = values.ptr(base);

        const Size mask = cap - 1;

        Size hole = start;
        md[hole].clear();
        len--;

        Size i = (Size)((hole + 1) & mask);
        for (Size probes = 0; probes < cap; probes++) {
            if (md[i].free()) break;

            /* Where this entry would ideally sit. */
            const Size ideal = (Size)(hash_of(ks[i]) & mask);

            /* Move it back only if the hole lies within its probe run, that
             * is if it is not already at or before its ideal slot relative to
             * the hole. The cyclic comparison is what makes this correct
             * across the wrap point. */
            const Size dist_hole = (Size)((hole - ideal) & mask);
            const Size dist_i = (Size)((i - ideal) & mask);

            if (dist_hole < dist_i) {
                md[hole] = md[i];
                ks[hole] = ks[i];
                vs[hole] = vs[i];
                md[i].clear();
                hole = i;
            }

            i = (Size)((i + 1) & mask);
        }

        return true;
    }

    /* ─── iteration ──────────────────────────────────────────────────────── */

    struct Iterator {
        HashMapMetadata *md;
        K *ks;
        V *vs;
        Size cap;
        Size i;

        bool next(K **out_key, V **out_value) {
            while (i < cap) {
                const Size cur = i;
                i++;
                if (md[cur].used()) {
                    *out_key = &ks[cur];
                    *out_value = &vs[cur];
                    return true;
                }
            }
            return false;
        }
    };

    template <typename Base>
    Iterator iterator(Base base) const {
        Iterator it;
        it.md = metadata.ptr(base);
        it.ks = keys.ptr(base);
        it.vs = values.ptr(base);
        it.cap = cap;
        it.i = 0;
        return it;
    }

    /* ─── invariants ─────────────────────────────────────────────────────── */

    /* Every stored key must be findable.
     *
     * This is the property backward-shift deletion exists to preserve, and it
     * is not implied by the metadata being self-consistent: a botched shift
     * leaves a free slot in front of a live entry, after which lookup stops
     * early and the entry is silently lost. O(n); intended for tests. */
    template <typename Base, typename HashFn>
    bool check_reachable(Base base, HashFn hash_of) const {
        HashMapMetadata *md = metadata.ptr(base);
        K *ks = keys.ptr(base);

        Size seen = 0;
        for (Size i = 0; i < cap; i++) {
            if (!md[i].used()) continue;
            seen++;
            if (find(base, hash_of(ks[i]), ks[i]) != i) return false;
        }
        return seen == len;
    }
};

} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_HASH_MAP_HPP */
