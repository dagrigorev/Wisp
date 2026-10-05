/* Ported from Ghostty src/terminal/hyperlink.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * OSC 8 hyperlinks attached to cells.
 *
 * A cell does not store a hyperlink ID. Hyperlinks are rare, so paying two
 * bytes in every cell to describe something almost no cell has would be
 * wasteful. Instead a cell carries a single bit, and the cells that do have a
 * link are looked up in a side map keyed by the cell's own offset.
 *
 * The link data itself is interned in a ref-counted set, so a run of cells
 * covered by one OSC 8 sequence shares a single entry.
 *
 * CYCLE. hyperlink.zig and page.zig import each other upstream: a link's
 * strings live in the page's string storage, and the page's layout needs the
 * map and set sizes. Page is forward declared here and PageEntry::free is
 * declared but not defined; page.hpp defines it once Page is complete. See
 * docs/GHOSTTY_PORT_ORDER.md.
 */

#pragma once
#ifndef WISP_TERMINAL_HYPERLINK_HPP
#define WISP_TERMINAL_HYPERLINK_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "size.hpp"
#include "hash_map.hpp"
#include "ref_counted_set.hpp"

namespace wisp {
namespace terminal {

struct Page;   /* defined in page.hpp; see CYCLE above */

namespace hyperlink {

/* A hyperlink identifier. At most the number of cells in a page, since a cell
 * holds at most one link. */
typedef HyperlinkCountInt Id;

/* Maps a cell — keyed by its offset from the page base — to its link ID.
 *
 * The 80% load factor is lower than the default because this map sees
 * removals as cells are overwritten, and a lower ceiling keeps probe runs
 * short under that churn. */
typedef OffsetHashMap<OffsetInt, Id, 80> Map;

/* ─── page entry ─────────────────────────────────────────────────────────── */

/* A hyperlink committed to page memory.
 *
 * Both the URI and an explicit ID are slices into the page's string storage
 * rather than pointers, so the page stays relocatable. Resolving either needs
 * the page base, which is why hash and eql take one. */
struct PageEntry {
    /* Where the identity of the link comes from. */
    enum class IdKind : uint8_t {
        /* An ID given explicitly in the OSC 8 sequence. Two links with the
         * same explicit ID are the same link even if their URIs differ. */
        explicit_id = 0,

        /* No ID was supplied, so one is generated from a counter on the
         * screen. Two implicit links are the same only if the counter
         * matches, which is what keeps separate OSC 8 runs distinct. */
        implicit_id = 1,
    };

    IdKind                 kind;
    Offset<uint8_t>::Slice explicit_id;   /* valid when kind is explicit */
    OffsetInt              implicit_id;   /* valid when kind is implicit */
    Offset<uint8_t>::Slice uri;

    PageEntry() : kind(IdKind::implicit_id), explicit_id(), implicit_id(0), uri() {}

    /* FNV-1a over the identity and the URI.
     *
     * The kind is mixed in first so an explicit ID cannot collide with an
     * implicit counter that happens to share its bytes. */
    uint64_t hash(const uint8_t *base) const {
        uint64_t h = 1469598103934665603ULL;
        h = mix_byte(h, (uint8_t)kind);

        if (kind == IdKind::explicit_id) {
            h = mix_bytes(h, explicit_id.offset.ptr(base), explicit_id.len);
        } else {
            for (int i = 0; i < 4; i++) {
                h = mix_byte(h, (uint8_t)((implicit_id >> (i * 8)) & 0xFF));
            }
        }

        return mix_bytes(h, uri.offset.ptr(base), uri.len);
    }

    /* Compare two entries that may live in different pages, which is why each
     * side carries its own base. */
    bool eql(const uint8_t *self_base,
             const PageEntry &other, const uint8_t *other_base) const {
        if (kind != other.kind) return false;

        if (kind == IdKind::explicit_id) {
            if (explicit_id.len != other.explicit_id.len) return false;
            if (memcmp(explicit_id.offset.ptr(self_base),
                       other.explicit_id.offset.ptr(other_base),
                       explicit_id.len) != 0) {
                return false;
            }
        } else if (implicit_id != other.implicit_id) {
            return false;
        }

        if (uri.len != other.uri.len) return false;
        return memcmp(uri.offset.ptr(self_base),
                      other.uri.offset.ptr(other_base), uri.len) == 0;
    }

    /* Release this entry's strings back to the page's string storage.
     * Defined in page.hpp, where Page is complete. */
    void free(Page *page) const;

    /* Copy this entry into another page, allocating its strings there.
     *
     * An entry is only meaningful next to the page its slices point into, so
     * moving a link between pages means materializing its strings in the
     * destination. self_base is this entry's page, since the source is by
     * definition not the destination.
     *
     * Returns false with nothing allocated if the destination's string
     * storage is full. Defined in page.hpp, where Page is complete. */
    bool dupe(const uint8_t *self_base, Page *dst, PageEntry *out) const;

private:
    static uint64_t mix_byte(uint64_t h, uint8_t b) {
        h ^= (uint64_t)b;
        return h * 1099511628211ULL;
    }
    static uint64_t mix_bytes(uint64_t h, const uint8_t *p, size_t n) {
        for (size_t i = 0; i < n; i++) h = mix_byte(h, p[i]);
        return h;
    }
};

/* ─── the interned set ───────────────────────────────────────────────────── */

/* Wires PageEntry into RefCountedSet.
 *
 * The context carries the page bases because a PageEntry is only meaningful
 * relative to one. src_base differs from base when a link is being looked up
 * or copied from another page, which is what makes moving a link between
 * pages possible without first materializing its strings. */
struct Context {
    uint8_t *base;      /* page holding entries already in the set */
    uint8_t *src_base;  /* page holding the entry being looked up, if different */
    Page    *page;      /* destination page, for releasing strings on delete */

    Context() : base(nullptr), src_base(nullptr), page(nullptr) {}

    const uint8_t *lookup_base() const { return src_base ? src_base : base; }

    uint64_t hash(const PageEntry &e) const { return e.hash(lookup_base()); }

    bool eql(const PageEntry &a, const PageEntry &b) const {
        return a.eql(lookup_base(), b, base);
    }

    /* Called when the set finally drops an entry, so its strings go back to
     * the page rather than leaking for the page's lifetime. */
    void deleted(const PageEntry &e) const {
        if (page) e.free(page);
    }
};

typedef RefCountedSet<PageEntry, Id, CellCountInt, Context> Set;

} /* namespace hyperlink */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_HYPERLINK_HPP */
