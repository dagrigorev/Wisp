/* Transliterated from Ghostty src/terminal/PageList.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Maintains a linked list of pages to make up a terminal screen
 * and provides higher level operations on top of those pages to
 * make it slightly easier to work with.
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp mapping:
 *   Allocator.Error!T          bool (false = OutOfMemory) + out param
 *   ?T                         Maybe<T> or bool + out param
 *   union(enum)                tag enum + payload fields
 *   std.AutoArrayHashMap set   PinSet (insertion-ordered keys, swapRemove)
 *   memory_pool.Managed(Pin)   PinPool
 *   log.*                      comments
 * The wasm page pool and freestanding/Darwin allocator paths are not
 * carried over (Windows only).
 */

#pragma once
#ifndef WISP_VT_PAGE_LIST_HPP
#define WISP_VT_PAGE_LIST_HPP

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "../datastruct/intrusive_linked_list.hpp"
#include "../datastruct/untouched_pool.hpp"
#include "../zigstd/allocator.hpp"
#include "compress/lz4.hpp"
#include "compress/page.hpp"
#include "fastmem.hpp"
#include "mem.hpp"
#include "page.hpp"
#include "point.hpp"
#include "size.hpp"
#include "style.hpp"
#include "tripwire.hpp"

namespace wisp {
namespace vt {

/* Wisp: ?T for value types. */
template <typename T>
struct Maybe {
    bool has;
    T value;

    Maybe() : has(false), value() {}
    Maybe(T v) : has(true), value(v) {}
    static Maybe none() { return Maybe(); }
    T orelse(T d) const { return has ? value : d; }
};

/* Wisp: the tripwire error type for Allocator.Error-only functions. */
enum class AllocTw { none, OutOfMemory };

struct PageList {
    typedef page::Page Page;
    typedef page::Row Row;
    typedef page::Capacity Capacity;

    /* The number of pages we preheat the page pool with. For operating systems
     * that support it, pages are demand-paged (see PagePool) so this only
     * costs us address space. For other operating systems, we don't preheat. */
    static const size_t page_preheat = 4;

    /* The number of nodes we preheat the node pool with. Unlike pages, nodes
     * are ordinary heap memory so every idle preheated node costs real
     * memory. A new PageList needs exactly one node for its first page, so
     * we preheat that one and let the pool grow on demand. */
    static const size_t node_preheat = 1;

    /* The number of pins we preheat the pin pool with: the viewport pin that
     * every PageList tracks and the cursor pin that every Screen tracks.
     * Selections, searches, and so on grow the pool on demand. */
    static const size_t pin_preheat = 2;

    /* The byte size required for a standard page.
     * Wisp: Page.layout(std_capacity).total_size, a compile-time constant
     * upstream; checked against the layout by the port's tests. */
#if defined(WISP_IS_TEST) && WISP_IS_TEST
    static const size_t std_size = 380928;
#else
    static const size_t std_size = 401408;
#endif

    /* A single node within the PageList linked list.
     *
     * This isn't pub because you can access the type via List.Node. */
    struct Node {
        Node *prev; /* = null */
        Node *next; /* = null */
        struct Data {
            enum class Tag : uint8_t { resident, compressed } tag;

            /* Wisp: union(enum) { resident: Page, compressed: compression.Page }.
             * compression.Page begins with the Page it compresses, so the
             * resident Page is stored as `compressed.page` and the encoded
             * fields are meaningful only when the tag is compressed. */
            compress::Page compressed;

            Page &resident() { return compressed.page; }
            const Page &resident() const { return compressed.page; }
        } data;
        uint64_t serial;

        /* How the backing memory of the embedded Page was allocated. Pool-owned
         * memory is always a full standard-size item from the memory
         * pool, regardless of the page layout size. Heap-owned memory
         * is allocated directly with the page allocator and is exactly
         * `Page.memory.len` bytes.
         *
         * This must never be inferred from the memory length: heap-owned
         * pages can be smaller than the standard size (e.g. compacted
         * pages), and returning one to the pool would corrupt it.
         *
         * This has no default on purpose so that every construction site
         * is forced to make an explicit decision. */
        enum class Owned : uint8_t { pool, heap } owned;

        /* The backing-memory representation currently stored by this node. */
        enum class Storage { resident, compressed };

        /* Wisp: `node.* = .{ .data = .{ .resident = page }, ... }`. */
        void initResident(const Page &p, uint64_t serial_, Owned owned_) {
            prev = nullptr;
            next = nullptr;
            data.tag = Data::Tag::resident;
            data.compressed.page = p;
            data.compressed.encoded = nullptr;
            data.compressed.encoded_len = 0;
            data.compressed.alloc = zigstd::c_allocator();
            serial = serial_;
            owned = owned_;
        }

        /* Return the terminal page stored in this node.
         *
         * WARNING: This will DECOMPRESS compressed pages! Only use this if
         * you need access to the underlying memory. If you only need access to
         * metadata (row, col counts etc) then use the other metadata functions. */
        Page *page() {
            if (data.tag == Data::Tag::resident) return &data.resident();
            return restore(RestoreMode::preserve);
        }

        /* Return the terminal page only when its raw memory is resident.
         *
         * Unlike `page`, this never restores a compressed node. This is useful
         * for diagnostics which can display node metadata without touching the
         * discarded mapping, but may inspect page contents when they are already
         * available. */
        Page *pageIfResident() {
            if (data.tag == Data::Tag::resident) return &data.resident();
            return nullptr;
        }

        /* Read-only page with full memory access that doesn't change
         * this node's storage state: if a node is compressed it remains
         * compressed.
         *
         * Resident pages are borrowed directly. So they're basically free.
         *
         * Compressed pages are decoded into an independently owned Page
         * by the caller so callers can inspect their contents. Because resident
         * pages are still borrowed, you have to continue to have exclusive
         * access to the PageList during this operation. */
        struct PreservedPage {
            enum class Tag { borrowed, owned } tag;
            const Page *borrowed;
            struct {
                Page page;
                zigstd::Allocator alloc;
            } owned;

            /* Return the read-only page represented by this value. */
            const Page *page() const { return tag == Tag::borrowed ? borrowed : &owned.page; }

            /* Release storage owned by this preserved page. */
            void deinit() {
                switch (tag) {
                case Tag::borrowed: break;
                /* The clone buffer came from the caller's allocator rather
                 * than Page's OS allocator, so it must not use Page.deinit. */
                case Tag::owned:
                    owned.alloc.free(owned.page.memory, owned.page.memory_len, page_size_min);
                    break;
                }
            }
        };

        /* Return read-only page contents without changing this node's storage.
         *
         * `alloc` is unused for a resident node. A compressed node uses it for an
         * exact-sized, page-aligned decode buffer owned by the returned value.
         *
         * The caller must call `PreservedPage.deinit` when finished. See
         * `PreservedPage` for the synchronization required by its borrowed
         * resident representation. Wisp: false is OutOfMemory. */
        bool pagePreservingState(zigstd::Allocator alloc, PreservedPage *out) const {
            if (data.tag == Data::Tag::resident) {
                out->tag = PreservedPage::Tag::borrowed;
                out->borrowed = &data.resident();
                return true;
            }

            const compress::Page &compressed = data.compressed;
            uint8_t *memory = alloc.alignedAlloc(compressed.page.memory_len, page_size_min);
            if (!memory) return false;

            Page page_;
            const lz4::DecompressError err = compressed.cloneBuf(memory, compressed.page.memory_len, &page_);
            if (err != lz4::DecompressError::none) {
                /* The encoded data was produced by our codec and remains
                 * immutable while compressed. Failure is internal
                 * corruption, matching the normal restoration boundary. */
                alloc.free(memory, compressed.page.memory_len, page_size_min);
                /* log.err("failed to clone compressed page err={}") */
                fprintf(stderr, "failed to clone compressed terminal page\n");
                abort();
            }

            out->tag = PreservedPage::Tag::owned;
            out->owned.page = page_;
            out->owned.alloc = alloc;
            return true;
        }

        /* Return the node's backing-memory representation without restoring it. */
        Storage storage() const {
            return data.tag == Data::Tag::resident ? Storage::resident : Storage::compressed;
        }

        /* Return a page which the caller knows is already resident.
         *
         * This avoids the representation check in hot paths which already hold
         * live pointers into the page mapping. Such pointers are only valid while
         * the page is resident. Prefer `page` unless the caller can establish
         * that invariant independently. */
        Page *pageAssumeResident() { return &data.resident(); }

        /* Return the number of populated rows without accessing page memory. */
        size::CellCountInt rows() const { return metadata()->size.rows; }

        /* Return the current column count without accessing page memory. */
        size::CellCountInt cols() const { return metadata()->size.cols; }

        /* Return the page capacity without accessing page memory. */
        Capacity capacity() const { return metadata()->capacity; }

        /* Return the embedded Page metadata without restoring its memory.
         * Wisp: both union arms keep the Page at the same place. */
        const Page *metadata() const { return &data.compressed.page; }

        enum class RestoreMode {
            /* Decode the compressed representation back into the raw mapping. */
            preserve,

            /* Discard the compressed representation without decoding it. The
             * caller must not read the page contents before overwriting them. */
            discard,
        };

        /* Restore this node to the resident representation.
         *
         * Preserve mode reconstructs the page contents and is used by `page`.
         * Discard mode skips decoding for callers which will overwrite or destroy
         * the page. Both modes recommit the retained mapping, free the encoded
         * allocation, and leave the node in a valid resident state. */
#ifdef _MSC_VER
        __declspec(noinline)
#endif
        Page *restore(RestoreMode mode) {
            if (data.tag == Data::Tag::resident) return &data.resident();
            compress::Page *compressed = &data.compressed;

            /* Decommit only discarded the physical pages. Recommit prepares the
             * still-reserved mapping for decoding or reuse by the caller. */
            mem::recommit(compressed->page.memory, compressed->page.memory_len);

            Page restored;
            switch (mode) {
            case RestoreMode::preserve: {
                const lz4::DecompressError err = compressed->restore(&restored);
                if (err != lz4::DecompressError::none) {
                    /* Compressed nodes retain the immutable encoding and exact
                     * output mapping used to create them. Any decode error is an
                     * internal codec bug or memory corruption. Keep this switch
                     * exhaustive so new decoder errors require classification.
                     * log.err("failed to restore compressed page err={}") */
                    fprintf(stderr, "failed to restore compressed terminal page\n");
                    abort();
                }
                break;
            }
            case RestoreMode::discard: restored = compressed->page; break;
            }

            /* Remove our compressed data */
            compressed->deinit();

            /* We're now a resident, non-compressed node. */
            data.tag = Data::Tag::resident;
            data.compressed.page = restored;
            return &data.resident();
        }

        bool isCompressed() const { return data.tag == Data::Tag::compressed; }
    };

    /* The list of pages in the screen. These are expected to be in order
     * where the first page is the topmost page (scrollback) and the last is
     * the bottommost page (the current active page). */
    typedef datastruct::IntrusiveDoublyLinkedList<Node> List;

    /* The memory pool we get page nodes from.
     *
     * We don't use a std memory pool here because it is backed by an arena
     * and we end up paying a lot of wasted memory for the growth factor when
     * in practice we don't usually use many nodes.
     *
     * We don't need the "untouched" property of our UntouchedPool but
     * this gives us a GPA-allocated pool so we reuse it here. */
    typedef datastruct::UntouchedPool<Node, alignof(Node)> NodePool;

    /* Wisp: [std_size]u8, page-aligned. */
    struct PageItem {
        uint8_t bytes[std_size];
    };

    /* The memory pool we use for page memory buffers. We use a separate pool
     * so we can allocate these with a page allocator. We have to use a page
     * allocator because we need memory that is zero-initialized and page-aligned.
     *
     * Untouched pools never read/write to the items so that we can
     * use demand-paging on operating systems that support it. This makes
     * it so that an idle item in the pool costs no physical memory,
     * only virtual memory.
     *
     * Contract for PageList is that every path that returns an item
     * to the pool must zero it. */
    typedef datastruct::UntouchedPool<PageItem, page_size_min> PagePool;

    struct Pin;
    struct PageIterator;
    struct RowIterator;
    struct CellIterator;
    struct PromptIterator;
    struct Cell;

    enum class Direction { left_up, right_down };

    /* Represents an exact x/y coordinate within the screen. This is called
     * a "pin" because it is a fixed point within the pagelist direct to
     * a specific page pointer and memory offset. The benefit is that this
     * point remains valid even through scrolling without any additional work.
     *
     * A downside is that  the pin is only valid until the pagelist is modified
     * in a way that may invalidate page pointers or shuffle rows, such as resizing,
     * erasing rows, etc.
     *
     * A pin can also be "tracked" which means that it will be updated as the
     * PageList is modified.
     *
     * The PageList maintains a list of active pin references and keeps them
     * all up to date as the pagelist is modified. This isn't cheap so callers
     * should limit the number of active pins as much as possible. */
    struct Pin {
        Node *node;
        size::CellCountInt y; /* = 0 */
        size::CellCountInt x; /* = 0 */

        /* This is flipped to true for tracked pins that were tracking
         * a page that got pruned for any reason and where the tracked pin
         * couldn't be moved to a sensical location. Users of the tracked
         * pin could use this data and make their own determination of
         * semantics. */
        bool garbage; /* = false */

        Pin() : node(nullptr), y(0), x(0), garbage(false) {}
        explicit Pin(Node *n, size::CellCountInt y_ = 0, size::CellCountInt x_ = 0)
            : node(n), y(y_), x(x_), garbage(false) {}

        /* Wisp: `Pin{ .node = n, .x = x }` */
        static Pin at(Node *n, size::CellCountInt x_, size::CellCountInt y_) { return Pin(n, y_, x_); }

        Page::RowAndCell rowAndCell() const {
            const Page::RowAndCell rac = node->page()->getRowAndCell(x, y);
            return rac;
        }

        enum class CellSubset { all, left, right };

        /* Returns the cells for the row that this pin is on. The subset determines
         * what subset of the cells are returned. The "left/right" subsets are
         * inclusive of the x coordinate of the pin.
         * Wisp: the slice is ptr + *len. */
        page::Cell *cells(CellSubset subset, size_t *len) const {
            Page *page = node->page();
            const Page::RowAndCell rac = page->getRowAndCell(x, y);
            page::Cell *all = page->getCells(rac.row);
            switch (subset) {
            case CellSubset::all: *len = page->size.cols; return all;
            case CellSubset::left: *len = (size_t)x + 1; return all;
            default: *len = (size_t)page->size.cols - x; return all + x;
            }
        }

        /* Returns the grapheme codepoints for the given cell. These are only
         * the EXTRA codepoints and not the first codepoint. */
        uint32_t *grapheme(const page::Cell *cell, size_t *len) const {
            return node->page()->lookupGrapheme(cell, len);
        }

        /* Returns the style for the given cell in this pin. */
        style::Style style(const page::Cell *cell) const {
            if (cell->style_id() == style::default_id) return style::Style();
            Page *page = node->page();
            return *page->styles.get((const void *)page->memory, cell->style_id());
        }

        /* Check if this pin is dirty. */
        bool isDirty() const {
            Page *page = node->page();
            return page->dirty || page->getRowAndCell(x, y).row->dirty();
        }

        /* Mark this pin location as dirty. */
        void markDirty() const { rowAndCell().row->setDirty(true); }

        /* Iterators. These are the same as PageList iterator funcs but operate
         * on pins rather than points. This is MUCH more efficient than calling
         * pointFromPin and building up the iterator from points.
         *
         * The limit pin is inclusive. */
        PageIterator pageIterator(Direction direction, Maybe<Pin> limit) const;
        RowIterator rowIterator(Direction direction, Maybe<Pin> limit) const;
        CellIterator cellIterator(Direction direction, Maybe<Pin> limit) const;
        PromptIterator promptIterator(Direction direction, Maybe<Pin> limit) const;

        /* Returns true if this pin is between the top and bottom, inclusive. */
        bool isBetween(const Pin &top, const Pin &bottom) const {
            if (slow_runtime_safety) {
                if (top.node == bottom.node) {
                    /* If top is bottom, must be ordered. */
                    assert(top.y <= bottom.y);
                    if (top.y == bottom.y) {
                        assert(top.x <= bottom.x);
                    }
                } else {
                    /* If top is not bottom, top must be before bottom. */
                    Node *n = top.node->next;
                    for (; n; n = n->next) {
                        if (n == bottom.node) break;
                    }
                    assert(n != nullptr);
                }
            }

            if (node == top.node) {
                /* If our pin is the top page and our y is less than the top y
                 * then we can't possibly be between the top and bottom. */
                if (y < top.y) return false;

                /* If our y is after the top y but we're on the same page
                 * then we're between the top and bottom if our y is less
                 * than or equal to the bottom y if its the same page. If the
                 * bottom is another page then it means that the range is
                 * at least the full top page and since we're the same page
                 * we're in the range. */
                if (y > top.y) {
                    return node == bottom.node ? y <= bottom.y : true;
                }

                /* Otherwise our y is the same as the top y, so we need to
                 * check the x coordinate. */
                assert(y == top.y);
                if (x < top.x) return false;
            }
            if (node == bottom.node) {
                /* Our page is the bottom page so we're between the top and
                 * bottom if our y is less than the bottom y. */
                if (y > bottom.y) return false;
                if (y < bottom.y) return true;

                /* If our y is the same, then we're between if we're before
                 * or equal to the bottom x. */
                assert(y == bottom.y);
                return x <= bottom.x;
            }

            /* Our page isn't the top or bottom so we need to check if
             * our page is somewhere between the top and bottom.
             *
             * Since our loop starts at top.page.next we need to check that
             * top != bottom because if they're the same then we can't possibly
             * be between them. */
            if (top.node == bottom.node) return false;
            for (Node *n = top.node->next; n; n = n->next) {
                if (n == bottom.node) break;
                if (n == node) return true;
            }

            return false;
        }

        /* Returns true if self is before other. This is very expensive since
         * it requires traversing the linked list of pages. This should not
         * be called in performance critical paths. */
        bool before(const Pin &other) const {
            if (node == other.node) {
                if (y < other.y) return true;
                if (y > other.y) return false;
                return x < other.x;
            }

            for (Node *n = node->next; n; n = n->next) {
                if (n == other.node) return true;
            }

            return false;
        }

        bool eql(const Pin &other) const { return node == other.node && y == other.y && x == other.x; }

        /* Move the pin left n columns. n must fit within the size. */
        Pin left(size_t n) const {
            assert(n <= x);
            Pin result = *this;
            result.x -= n <= 0xFFFF ? (size::CellCountInt)n : result.x;
            return result;
        }

        /* Move the pin right n columns. n must fit within the size. */
        Pin right(size_t n) const {
            assert(x + n < node->cols());
            Pin result = *this;
            const size_t add = n <= 0xFFFF ? n : 0xFFFF;
            result.x = (size::CellCountInt)((size_t)result.x + add > 0xFFFF ? 0xFFFF : result.x + add);
            return result;
        }

        /* Move the pin left n columns, stopping at the start of the row. */
        Pin leftClamp(size::CellCountInt n) const {
            Pin result = *this;
            result.x = result.x > n ? (size::CellCountInt)(result.x - n) : 0;
            return result;
        }

        /* Move the pin right n columns, stopping at the end of the row. */
        Pin rightClamp(size::CellCountInt n) const {
            Pin result = *this;
            const uint32_t sum = (uint32_t)x + n > 0xFFFF ? 0xFFFF : (uint32_t)x + n;
            const uint32_t last = (uint32_t)node->cols() - 1;
            result.x = (size::CellCountInt)(sum < last ? sum : last);
            return result;
        }

        Maybe<Pin> leftWrap(size_t n) const;
        Maybe<Pin> rightWrap(size_t n) const;
        struct Overflow;
        Maybe<Pin> down(size_t n) const;
        Maybe<Pin> up(size_t n) const;
        static size::CellCountInt castCell(size_t v) { return v <= 0xFFFF ? (size::CellCountInt)v : 0xFFFF; }
        static size::CellCountInt minCell(size::CellCountInt a, size::CellCountInt b) { return a < b ? a : b; }
        Overflow downOverflow(size_t n) const;
        Overflow upOverflow(size_t n) const;
    };

    /* Wisp: union(enum) { offset: Pin, overflow: struct { end, remaining } }. */
    struct Pin::Overflow {
        enum class Tag { offset, overflow } tag;
        Pin offset;
        struct {
            Pin end;
            size_t remaining;
        } overflow;

        static Overflow makeOffset(const Pin &p) {
            Overflow o;
            o.tag = Tag::offset;
            o.offset = p;
            o.overflow.remaining = 0;
            return o;
        }
        static Overflow makeOverflow(const Pin &end, size_t remaining) {
            Overflow o;
            o.tag = Tag::overflow;
            o.overflow.end = end;
            o.overflow.remaining = remaining;
            return o;
        }
    };

    /* ------------------------------------------------------------------ */
    /* Iterators                                                            */

    struct Chunk {
        Node *node;

        /* Start y index (inclusive) of this chunk in the page. */
        size::CellCountInt start;

        /* End y index (exclusive) of this chunk in the page. */
        size::CellCountInt end;

        Chunk() : node(nullptr), start(0), end(0) {}
        Chunk(Node *n, size::CellCountInt s, size::CellCountInt e) : node(n), start(s), end(e) {}

        /* Wisp: the slice is ptr + *len. */
        page::Row *rows(size_t *len) const {
            Page *page = node->page();
            page::Row *rows_ptr = page->rows.ptr(page->memory);
            *len = (size_t)end - start;
            return rows_ptr + start;
        }

        /* Returns true if this chunk represents every row in the page. */
        bool fullPage() const { return start == 0 && end == node->rows(); }

        /* Returns true if this chunk overlaps with the given other chunk
         * in any way. */
        bool overlaps(const Chunk &other) const {
            if (node != other.node) return false;
            if (end <= other.start) return false;
            if (start >= other.end) return false;
            return true;
        }
    };

    struct PageIterator {
        Maybe<Pin> row; /* = null */

        struct Limit {
            enum class Tag { none, count, row } tag;
            size_t count;
            Pin row;
            Limit() : tag(Tag::none), count(0), row() {}
        } limit;

        Direction direction; /* = .right_down */

        typedef PageList::Chunk Chunk;

        PageIterator() : row(), limit(), direction(Direction::right_down) {}

        /* Wisp: ?Chunk is bool + out. */
        bool next(Chunk *out) {
            switch (direction) {
            case Direction::left_up: return nextUp(out);
            default: return nextDown(out);
            }
        }

        bool nextDown(Chunk *out) {
            /* Get our current row location */
            if (!row.has) return false;
            const Pin r = row.value;

            switch (limit.tag) {
            case Limit::Tag::none:
                /* If we have no limit, then we consume this entire page. Our
                 * next row is the next page. */
                if (r.node->next) {
                    row = Pin(r.node->next);
                } else {
                    row = Maybe<Pin>::none();
                }

                *out = Chunk(r.node, r.y, r.node->rows());
                return true;

            case Limit::Tag::count: {
                assert(limit.count > 0); /* should be handled already */
                const size_t available = (size_t)r.node->rows() - r.y;
                const size_t len = available < limit.count ? available : limit.count;
                limit.count -= len;
                row = limit.count > 0 ? r.down(len) : Maybe<Pin>::none();

                *out = Chunk(r.node, r.y, (size::CellCountInt)((size_t)r.y + len));
                return true;
            }

            default: {
                const Pin &limit_row = limit.row;
                /* If this is not the same page as our limit then we
                 * can consume the entire page. */
                if (limit_row.node != r.node) {
                    if (r.node->next) {
                        row = Pin(r.node->next);
                    } else {
                        row = Maybe<Pin>::none();
                    }

                    *out = Chunk(r.node, r.y, r.node->rows());
                    return true;
                }

                /* If this is the same page then we only consume up to
                 * the limit row. */
                row = Maybe<Pin>::none();
                if (r.y > limit_row.y) return false;
                *out = Chunk(r.node, r.y, (size::CellCountInt)(limit_row.y + 1));
                return true;
            }
            }
        }

        bool nextUp(Chunk *out) {
            /* Get our current row location */
            if (!row.has) return false;
            const Pin r = row.value;

            switch (limit.tag) {
            case Limit::Tag::none:
                /* If we have no limit, then we consume this entire page. Our
                 * next row is the next page. */
                if (Node *next_page = r.node->prev) {
                    row = Pin(next_page, (size::CellCountInt)(next_page->rows() - 1));
                } else {
                    row = Maybe<Pin>::none();
                }

                *out = Chunk(r.node, 0, (size::CellCountInt)(r.y + 1));
                return true;

            case Limit::Tag::count: {
                assert(limit.count > 0); /* should be handled already */
                const size_t available = (size_t)r.y + 1;
                const size_t len = available < limit.count ? available : limit.count;
                limit.count -= len;
                row = limit.count > 0 ? r.up(len) : Maybe<Pin>::none();

                *out = Chunk(r.node, (size::CellCountInt)(available - len), (size::CellCountInt)available);
                return true;
            }

            default: {
                const Pin &limit_row = limit.row;
                /* If this is not the same page as our limit then we
                 * can consume the entire page. */
                if (limit_row.node != r.node) {
                    if (Node *next_page = r.node->prev) {
                        row = Pin(next_page, (size::CellCountInt)(next_page->rows() - 1));
                    } else {
                        row = Maybe<Pin>::none();
                    }

                    *out = Chunk(r.node, 0, (size::CellCountInt)(r.y + 1));
                    return true;
                }

                /* If this is the same page then we only consume up to
                 * the limit row. */
                row = Maybe<Pin>::none();
                if (r.y < limit_row.y) return false;
                *out = Chunk(r.node, limit_row.y, (size::CellCountInt)(r.y + 1));
                return true;
            }
            }
        }
    };

    struct RowIterator {
        PageIterator page_it;
        Maybe<Chunk> chunk; /* = null */
        size::CellCountInt offset; /* = 0 */

        RowIterator() : page_it(), chunk(), offset(0) {}

        bool next(Pin *out) {
            if (!chunk.has) return false;
            const Chunk c = chunk.value;
            const Pin row(c.node, offset);

            switch (page_it.direction) {
            case Direction::right_down:
                /* Increase our offset in the chunk */
                offset += 1;

                /* If we are beyond the chunk end, we need to move to the next chunk. */
                if (offset >= c.end) {
                    Chunk nc;
                    if (page_it.next(&nc)) {
                        chunk = nc;
                        offset = nc.start;
                    } else {
                        chunk = Maybe<Chunk>::none();
                    }
                }
                break;

            case Direction::left_up:
                /* If we are at the start of the chunk, we need to move to the
                 * previous chunk. */
                if (offset == 0) {
                    Chunk nc;
                    if (page_it.next(&nc)) {
                        chunk = nc;
                        offset = (size::CellCountInt)(nc.end - 1);
                    } else {
                        chunk = Maybe<Chunk>::none();
                    }
                } else {
                    /* If we're at the start of the chunk and its a non-zero
                     * offset then we've reached a limit. */
                    if (offset == c.start) {
                        chunk = Maybe<Chunk>::none();
                    } else {
                        offset -= 1;
                    }
                }
                break;
            }

            *out = row;
            return true;
        }
    };

    struct CellIterator {
        RowIterator row_it;
        Maybe<Pin> cell; /* = null */

        CellIterator() : row_it(), cell() {}

        bool next(Pin *out) {
            if (!cell.has) return false;
            const Pin c = cell.value;

            switch (row_it.page_it.direction) {
            case Direction::right_down:
                if ((size_t)c.x + 1 < c.node->cols()) {
                    /* We still have cells in this row, increase x. */
                    Pin copy = c;
                    copy.x += 1;
                    cell = copy;
                } else {
                    /* We need to move to the next row. */
                    Pin n;
                    if (row_it.next(&n)) {
                        cell = n;
                    } else {
                        cell = Maybe<Pin>::none();
                    }
                }
                break;

            case Direction::left_up:
                if (c.x > 0) {
                    /* We still have cells in this row, decrease x. */
                    Pin copy = c;
                    copy.x -= 1;
                    cell = copy;
                } else {
                    /* We need to move to the previous row and last col */
                    Pin next_cell;
                    if (row_it.next(&next_cell)) {
                        Pin copy = next_cell;
                        copy.x = (size::CellCountInt)(next_cell.node->cols() - 1);
                        cell = copy;
                    } else {
                        cell = Maybe<Pin>::none();
                    }
                }
                break;
            }

            *out = c;
            return true;
        }
    };

    struct PromptIterator {
        /* The pin that we are currently at. Also the starting pin when
         * initializing. */
        Maybe<Pin> current;

        /* The pin to end at or null if we end when we can't traverse
         * anymore. */
        Maybe<Pin> limit;

        /* The direction to do the traversal. */
        Direction direction;

        PromptIterator() : current(), limit(), direction(Direction::left_up) {}

        static PromptIterator empty() { return PromptIterator(); }

        /* Return the next pin that represents the first row in a prompt.
         * From here, you can find the prompt input, command output, etc. */
        bool next(Pin *out) {
            switch (direction) {
            case Direction::left_up: return nextLeftUp(out);
            default: return nextRightDown(out);
            }
        }

        bool nextRightDown(Pin *out) {
            typedef page::Row::SemanticPrompt SP;
            /* Start at our current pin. If we have no current it means
             * we reached the end and we're done. */
            if (!current.has) return false;
            const Pin start = current.value;

            /* We need to traverse downwards and look for prompts. */
            Maybe<Pin> cur = start;
            while (cur.has) {
                const Pin p = cur.value;
                /* Check our limit. */
                const bool at_limit = limit.has ? limit.value.eql(p) : false;

                const Page::RowAndCell rac = p.rowAndCell();
                switch (rac.row->semantic_prompt()) {
                /* This row isn't a prompt. Keep looking. */
                case SP::none:
                    if (at_limit) goto done;
                    break;

                /* This is a prompt line or continuation line. In either
                 * case we consider the first line the prompt, and then
                 * skip over any remaining prompt lines. This handles the
                 * case where scrollback pruned the prompt. */
                default: {
                    /* If we're at our limit just return this prompt. */
                    if (at_limit) {
                        current = Maybe<Pin>::none();
                        *out = p.left(p.x);
                        return true;
                    }

                    /* Skip over any continuation lines that follow this prompt,
                     * up to our limit. */
                    Pin end_pin = p;
                    for (;;) {
                        const Maybe<Pin> next_pin = end_pin.down(1);
                        if (!next_pin.has) break;
                        bool stop = false;
                        switch (next_pin.value.rowAndCell().row->semantic_prompt()) {
                        case SP::prompt_continuation:
                            if (limit.has && limit.value.eql(next_pin.value)) stop = true;
                            break;

                        default:
                            current = next_pin.value;
                            *out = p.left(p.x);
                            return true;
                        }
                        if (stop) break;
                        end_pin = next_pin.value;
                    }

                    current = Maybe<Pin>::none();
                    *out = p.left(p.x);
                    return true;
                }
                }
                cur = p.down(1);
            }
        done:
            current = Maybe<Pin>::none();
            return false;
        }

        bool nextLeftUp(Pin *out) {
            typedef page::Row::SemanticPrompt SP;
            /* Start at our current pin. If we have no current it means
             * we reached the end and we're done. */
            if (!current.has) return false;
            const Pin start = current.value;

            /* We need to traverse upwards and look for prompts. */
            Maybe<Pin> cur = start;
            while (cur.has) {
                const Pin p = cur.value;
                /* Check our limit. */
                const bool at_limit = limit.has ? limit.value.eql(p) : false;

                const Page::RowAndCell rac = p.rowAndCell();
                switch (rac.row->semantic_prompt()) {
                /* This row isn't a prompt. Keep looking. */
                case SP::none:
                    if (at_limit) goto done;
                    break;

                /* This is a prompt line. */
                case SP::prompt:
                    current = at_limit ? Maybe<Pin>::none() : p.up(1);
                    *out = p.left(p.x);
                    return true;

                /* If this is a prompt continuation, then we continue
                 * looking for the start of the prompt OR a non-prompt
                 * line, whichever is first. The non-prompt line is to handle
                 * poorly behaved programs or scrollback that's been cut-off. */
                default: {
                    /* If we're at our limit just return this continuation as prompt. */
                    if (at_limit) {
                        current = Maybe<Pin>::none();
                        *out = p.left(p.x);
                        return true;
                    }

                    Pin end_pin = p;
                    for (;;) {
                        const Maybe<Pin> prior_ = end_pin.up(1);
                        if (!prior_.has) break;
                        const Pin prior = prior_.value;
                        if (limit.has && limit.value.eql(prior)) break;

                        switch (prior.rowAndCell().row->semantic_prompt()) {
                        /* No prompt. That means our last pin is good! */
                        case SP::none:
                            current = prior;
                            *out = end_pin.left(end_pin.x);
                            return true;

                        /* Prompt! Found it! */
                        case SP::prompt:
                            current = prior.up(1);
                            *out = prior.left(prior.x);
                            return true;

                        /* Prompt continuation, keep looking. */
                        default: break;
                        }
                        end_pin = prior;
                    }

                    /* No prior rows, trimmed scrollback probably. */
                    current = Maybe<Pin>::none();
                    *out = p.left(p.x);
                    return true;
                }
                }
                cur = p.up(1);
            }
        done:
            current = Maybe<Pin>::none();
            return false;
        }
    };

    /* List of pins, known as "tracked" pins. These are pins that are kept
     * up to date automatically through page-modifying operations.
     * Wisp: std.AutoArrayHashMapUnmanaged(*Pin, void) — insertion-ordered
     * keys with swapRemove. */
    struct PinSet {
        /* Wisp: the entries array is allocated from `alloc` (grown like
         * ArrayList) so allocation failures surface as upstream's do; key
         * lookup is a linear scan. */
        zigstd::Allocator alloc;
        Pin **entries;
        size_t len;
        size_t capacity;

        PinSet() : alloc(zigstd::c_allocator()), entries(nullptr), len(0), capacity(0) {}

        Pin *const *keys() const { return entries; }
        size_t count() const { return len; }
        bool contains(const Pin *p) const {
            for (size_t i = 0; i < len; i++)
                if (entries[i] == p) return true;
            return false;
        }
        bool setCapacity(zigstd::Allocator a, size_t n) {
            alloc = a;
            return ensureTotalCapacityPrecise(n);
        }
        bool ensureTotalCapacityPrecise(size_t n) {
            if (capacity >= n) return true;
            Pin **e = alloc.allocT<Pin *>(n);
            if (!e) return false;
            if (len) memcpy(e, entries, len * sizeof(Pin *));
            if (capacity) alloc.freeT<Pin *>(entries, capacity);
            entries = e;
            capacity = n;
            return true;
        }
        void putAssumeCapacityNoClobber(Pin *p) {
            assert(!contains(p));
            assert(len < capacity);
            entries[len++] = p;
        }
        /* Wisp: false is OutOfMemory. */
        bool putNoClobber(Pin *p) {
            if (len == capacity) {
                size_t n = capacity;
                do {
                    n += n / 2 + 8;
                } while (n <= len);
                if (!ensureTotalCapacityPrecise(n)) return false;
            }
            putAssumeCapacityNoClobber(p);
            return true;
        }
        bool swapRemove(const Pin *p) {
            for (size_t i = 0; i < len; i++) {
                if (entries[i] != p) continue;
                entries[i] = entries[len - 1];
                len -= 1;
                return true;
            }
            return false;
        }
        void deinit() {
            if (capacity) alloc.freeT<Pin *>(entries, capacity);
            entries = nullptr;
            len = capacity = 0;
        }
    };

    /* Wisp: std.heap.memory_pool.Managed(Pin). Items come from the gpa and
     * are recycled through a free list; reset frees according to mode. */
    struct PinPool {
        zigstd::Allocator alloc;
        std::vector<Pin *> free_list;
        std::vector<Pin *> all;

        static bool initCapacity(zigstd::Allocator a, size_t preheat, PinPool *out) {
            out->alloc = a;
            out->free_list.clear();
            out->all.clear();
            for (size_t i = 0; i < preheat; i++) {
                Pin *p = a.create<Pin>();
                if (!p) {
                    out->deinit();
                    return false;
                }
                out->all.push_back(p);
                out->free_list.push_back(p);
            }
            return true;
        }
        void deinit() {
            for (Pin *p : all) alloc.destroy(p);
            all.clear();
            free_list.clear();
        }
        Pin *create() {
            if (!free_list.empty()) {
                Pin *p = free_list.back();
                free_list.pop_back();
                return p;
            }
            Pin *p = alloc.create<Pin>();
            if (!p) return nullptr;
            all.push_back(p);
            return p;
        }
        void destroy(Pin *p) { free_list.push_back(p); }

        /* Wisp: MemoryPool.reset destroys every item, retaining the arena
         * capacity per `mode`; freed items are simply returned here. */
        bool reset(const PagePool::ResetMode &mode) {
            (void)mode;
            free_list = all;
            return true;
        }
    };

    /* The pool of memory used for a pagelist. This can be shared between
     * multiple pagelists but it is not threadsafe. */
    struct MemoryPool {
        zigstd::Allocator alloc;
        NodePool nodes;
        PagePool pages;
        PinPool pins;

        typedef PagePool::ResetMode ResetMode;

        /* Wisp: false is OutOfMemory. */
        static bool init(zigstd::Allocator gen_alloc, zigstd::Allocator page_alloc, size_t preheat, MemoryPool *out) {
            if (!NodePool::initCapacity(gen_alloc, gen_alloc, node_preheat, &out->nodes)) return false;
            if (!PagePool::initCapacity(gen_alloc, page_alloc, preheat, &out->pages)) {
                out->nodes.deinit();
                return false;
            }
            if (!PinPool::initCapacity(gen_alloc, pin_preheat, &out->pins)) {
                out->pages.deinit();
                out->nodes.deinit();
                return false;
            }
            out->alloc = gen_alloc;
            return true;
        }

        void deinit() {
            pages.deinit();
            nodes.deinit();
            pins.deinit();
        }

        void reset(ResetMode mode) {
            (void)pages.reset(mode);
            (void)nodes.reset(mode);
            (void)pins.reset(mode);
        }
    };

    /* The viewport location. */
    enum class Viewport {
        /* The viewport is pinned to the active area. By using a specific marker
         * for this instead of tracking the row offset, we eliminate a number of
         * memory writes making scrolling faster. */
        active,

        /* The viewport is pinned to the top of the screen, or the farthest
         * back in the scrollback history. */
        top,

        /* The viewport is pinned to a tracked pin. The tracked pin is ALWAYS
         * s.viewport_pin hence this has no value. We force that value to prevent
         * allocations. */
        pin,
    };

    struct Limits {
        struct Limit {
            /* Explicit is the specified maximum value for this limit
             * by the user or maxInt otherwise. */
            size_t explicit_; /* = maxInt(usize) */

            /* Min is the minimum valid maximum for this entry, so if
             * explicit is lower than this then min wins. */
            size_t min;
        };

        Limit bytes;
        Limit lines;

        /* The limit keys. */
        enum class Key { bytes, lines };

        /* Return unlimited limits with minimums for the given PageList size. */
        static Limits init(size::CellCountInt cols, size::CellCountInt rows) {
            Limits l;
            l.bytes.explicit_ = SIZE_MAX;
            l.bytes.min = minMaxSize(cols, rows);
            l.lines.explicit_ = SIZE_MAX;
            l.lines.min = minMaxLines(cols);
            return l;
        }

        /* Set an explicit limit. Null means unlimited. This does not enforce the
         * new value automatically; the caller must still call `enforce`. */
        void set(Key key, Maybe<size_t> value) {
            switch (key) {
            case Key::bytes: bytes.explicit_ = value.orelse(SIZE_MAX); break;
            case Key::lines: lines.explicit_ = value.orelse(SIZE_MAX); break;
            }
        }

        /* Recalculate the effective minimums for a new PageList size.
         *
         * This must be called whenever either PageList dimension changes so the
         * effective byte and line limits remain valid for the new size. */
        void resize(size::CellCountInt cols, size::CellCountInt rows) {
            bytes.min = minMaxSize(cols, rows);
            lines.min = minMaxLines(cols);
        }

        /* Return the effective maximum for a limit. */
        size_t max(Key key) const {
            switch (key) {
            case Key::bytes: return bytes.explicit_ > bytes.min ? bytes.explicit_ : bytes.min;
            default: return lines.explicit_ > lines.min ? lines.explicit_ : lines.min;
            }
        }

        /* Whether the given limit is currently exceeded. Both limits are
         * heuristics: complete historical pages are the smallest unit that
         * enforcement removes. */
        bool exceeded(const PageList *pagelist, Key limit) const {
            switch (limit) {
            case Key::bytes: return pagelist->page_size > max(Key::bytes);
            default:
                return pagelist->total_rows > pagelist->rows &&
                       pagelist->total_rows - pagelist->rows > max(Key::lines);
            }
        }

        /* Prune complete historical pages until the selected limit is satisfied
         * or the oldest remaining page overlaps the active area. */
        void enforce(PageList *pagelist, Key key) const;

        /* Returns the minimum valid "max size" for a given number of rows and cols
         * such that we can fit the active area AND at least two pages. Note we
         * need the two pages for algorithms to work properly (such as grow) but
         * we don't need to fit double the active area.
         *
         * This min size may not be totally correct in the case that a large
         * number of other dimensions makes our row size in a page very small.
         * But this gives us a nice fast heuristic for determining min/max size.
         * Therefore, if the page size is violated you should always also verify
         * that we have enough space for the active area. */
        static size_t minMaxSize(size::CellCountInt cols, size::CellCountInt rows) {
            /* Get our capacity to fit our rows. If the cols are too big, it may
             * force less rows than we want meaning we need more than one page to
             * represent a viewport. */
            const Capacity cap = initialCapacity(cols);

            /* Calculate the number of standard sized pages we need to represent
             * an active area. */
            const size_t pages_exact = cap.rows >= rows ? 1 : ((size_t)rows + cap.rows - 1) / cap.rows;

            /* We always need at least one page extra so that we
             * can fit partial pages to spread our active area across two pages.
             * Even for caps that can't fit all rows in a single page, we add one
             * because the most extra space we need at any given time is only
             * the partial amount of one page. */
            const size_t pages = pages_exact + 1;
            assert(pages >= 2);

            return PagePool::item_size * pages;
        }

        /* Returns the minimum line limit for a given column count. Line limits are
         * page-granular, so we always permit at least one standard page worth of
         * scrollback rows. */
        static size_t minMaxLines(size::CellCountInt cols) { return initialCapacity(cols).rows; }
    };

    struct IncrementalCompressionState;
    struct IncrementalCompressionStateStorage;

    /* ------------------------------------------------------------------ */
    /* Fields                                                               */

    /* The memory pool we get page nodes, pages from. */
    MemoryPool pool;

    /* The list of pages in the screen. */
    List pages;

    /* The next globally unique reference generation for this PageList. A
     * generation is assigned whenever a page is allocated, reused as new, or
     * changed in place such that existing page coordinates are no longer stable.
     *
     * The serial number can be used to detect whether the page is identical
     * to the page that was originally referenced by a pointer. Since we reuse
     * and pool memory, pointer stability is not guaranteed, but the serial
     * will always be different for different page generations.
     *
     * Developer note: we never do overflow checking on this. If we created
     * a new page every second it'd take 584 billion years to overflow. We're
     * going to risk it. */
    uint64_t page_serial;

    /* The first serial in the current whole-list validity epoch. Only `reset`
     * advances this value, immediately before rebuilding every page. It is not
     * the generation of the first page or the exact minimum live generation.
     * Page generations are not monotonic in list order: replacement and split
     * operations can put a fresh generation before older live pages.
     *
     * A generation below this epoch is definitely invalid, allowing O(1)
     * rejection in `nodeIsValid` and bulk removal of pre-reset search results.
     * A generation at or above it is only potentially valid and must still be
     * checked against the live list before its coordinates are used. */
    uint64_t page_serial_epoch;

    /* Byte size of the raw backing mappings owned by active page nodes. This is
     * logical scrollback accounting and does not change while a mapping is
     * decommitted. It excludes encoded storage and unused preheated pool items. */
    size_t page_size;

    /* Wisp: page_compression, recycle_node, limits... follow below. The
     * compression state type is defined in the compression section. */
    /* PageList-owned state for incremental compression.
     *
     * The position is stored as a page serial rather than a node pointer so it
     * remains safe when PageList operations destroy, replace, or reuse nodes
     * between steps. It also records the first serial which was unallocated at
     * the prior step so new nodes before a valid marker restart safely. If the
     * exact serial no longer exists in the cold prefix, the next step also
     * restarts at the first page.
     *
     * A completed pass is followed by another pass from the oldest cold page.
     * Compression becomes idle only when that verification pass compresses no
     * pages. This converges after pages are restored between incremental steps
     * without requiring consumers to maintain a separate attempt cursor. */
    struct IncrementalCompressionState {
        struct Flags {
            /* Set after any page is compressed during the current traversal.
             * We always run incremental compression until we get a fully
             * no-compression pass (the verification pass). This lets us
             * recompress decompressed pages due to search, scrolling, etc. */
            bool did_compress; /* = false */

            /* Set after a traversal first reaches the active boundary. When this
             * verification traversal reaches the boundary, `did_compress`
             * determines whether to restart once more or finish the pass. */
            bool verifying; /* = false */
        } flags;

        /* Changes whenever PageList activity may affect compression work.
         * Callers use this value (via Terminal.compressionActivity) to
         * determine whether to recompress.
         *
         * The directionality of this doesn't matter. We overflow and
         * wrap when maxxed. The comparison of this value to your saved
         * value is all that matters. There is a possible edge case where you
         * don't compress, a full wraparound happens, and you get the same value,
         * but its so unlikely.
         *
         * This is 48-bits so that 16-bits can be reserved for Terminal to
         * add extra state. Wisp: u48 kept in the low bits of a u64. */
        uint64_t activity_serial; /* = 0 */

        /* Serial of the last page inspected by the traversal. This is
         * intentionally an implementation detail and is reset by PageList
         * operations which restart traversal progress. */
        Maybe<uint64_t> last_serial; /* = null */

        /* First node serial which had not been allocated at the prior step. A
         * node at or above this value before the saved marker requires a restart. */
        uint64_t next_serial; /* = 0 */

        IncrementalCompressionState() : activity_serial(0), last_serial(), next_serial(0) {
            flags.did_compress = false;
            flags.verifying = false;
        }

        /* Record activity without disturbing valid traversal progress. */
        void markActivity() { activity_serial = (activity_serial + 1) & 0xFFFFFFFFFFFFull; }

        /* Discard traversal progress without changing the activity token. */
        void reset() {
            const uint64_t a = activity_serial;
            *this = IncrementalCompressionState();
            activity_serial = a;
        }
    };
/* @@COMPRESSION_STATE@@ */

    /* Continuation state for incremental page compression. This allows
     * compress(.incremental) to work. More details on all that there and
     * in the state struct. */
    IncrementalCompressionState page_compression;

    /* A node available for immediate reuse by createPage, bypassing the
     * memory pool round trip. This is only set during column reflow
     * (resizeCols), which destroys one source page for roughly every
     * destination page it creates: returning a page buffer to the pool
     * decommits it and taking one back recommits it, and that syscall
     * pair per page is a significant part of reflow cost. Always null
     * outside of an in-progress reflow. */
    Node *recycle_node; /* = null */

    /* Limits for scrollback. */
    Limits limits;

    /* The total number of rows represented by this PageList. This is used
     * specifically for scrollbar information so we can have the total size. */
    size_t total_rows;

    /* The list of tracked pins. These are kept up to date automatically. */
    PinSet tracked_pins;

    /* The top-left of certain parts of the screen that are frequently
     * accessed so we don't have to traverse the linked list to find them.
     *
     * For other tags, don't need this:
     *   - screen: pages.first
     *   - history: active row minus one */
    Viewport viewport;

    /* The pin used for when the viewport scrolls. This is always pre-allocated
     * so that scrolling doesn't have a failable memory allocation. This should
     * never be access directly; use `viewport`. */
    Pin *viewport_pin;

    /* The row offset from the top that the viewport pin is at. We
     * store the offset from the top because it doesn't change while more
     * data is printed to the terminal.
     *
     * This is null when it isn't calculated. It is calculated on demand
     * when the viewportRowOffset function is called, because it is only
     * required for certain operations such as rendering the scrollbar.
     *
     * In order to make this more efficient, in many places where the value
     * would be invalidated, we update it in-place instead. This is key to
     * keeping our performance decent in normal cases since recalculating
     * this from scratch, depending on the size of the scrollback and position
     * of the pin, can be very expensive.
     *
     * This is only valid if viewport is `pin`. Every other offset is
     * self-evident or quick to calculate. */
    Maybe<size_t> viewport_pin_row_offset;

    /* The current desired screen dimensions. I say "desired" because individual
     * pages may still be a different size and not yet reflowed since we lazily
     * reflow text. */
    size::CellCountInt cols;
    size::CellCountInt rows;

    /* If this is true then verifyIntegrity will do nothing. This is
     * only present with runtime safety enabled. */
    size_t pause_integrity_checks; /* = 0 */

    PageList()
        : pool(), pages(), page_serial(0), page_serial_epoch(0), page_size(0), page_compression(),
          recycle_node(nullptr), limits(), total_rows(0), tracked_pins(), viewport(Viewport::active),
          viewport_pin(nullptr), viewport_pin_row_offset(), cols(0), rows(0), pause_integrity_checks(0) {}

    /* ------------------------------------------------------------------ */

    /* Calculates the initial capacity for a new page for a given column
     * count. This will attempt to fit within std_size at all times so we
     * can use our memory pool, but if cols is too big, this will return a
     * larger capacity.
     *
     * The returned capacity is always guaranteed to layout properly (not
     * overflow). We are able to support capacities up to the maximum int
     * value of cols, so this will never overflow. */
    static Capacity initialCapacity(size::CellCountInt cols) {
        /* This is an important invariant that ensures that this function
         * can never return an error. We verify here that our standard capacity
         * when increased to maximum possible columns can always support at
         * least one row in memory. (Wisp: comptime check moved to the tests.) */

        Capacity cap;
        Capacity::Adjustment adj;
        adj = Capacity::Adjustment::withCols(cols);
        if (page::std_capacity().adjust(adj, &cap)) {
            /* If we can adjust our standard capacity, we fit within the
             * standard size and we're good! */
            return cap;
        }

        /* This code path means that our standard capacity can't even
         * accommodate our column count! The only solution is to increase
         * our capacity and go non-standard. */
        cap = page::std_capacity();
        cap.cols = cols;
        return cap;
    }

    /* Returns the allocator used for underlying page allocations.
     *
     * `alloc` is the caller-provided allocator. It is used on native freestanding
     * targets, where no OS page allocator is available. Other targets select a
     * platform-specific allocator below. */
    static zigstd::Allocator pageAllocator(zigstd::Allocator alloc) {
        (void)alloc;
        /* In tests we use our testing allocator so we can detect leaks. */
        if (is_test) return zigstd::testing_allocator();

        /* On non-macOS we use our standard Zig page allocator. */
        return zigstd::page_allocator();
    }

    enum class InitTw { init_memory_pool, init_pages, viewport_pin, viewport_pin_track };
    typedef tripwire::Module<InitTw, AllocTw, 4> init_tw;

    struct Options {
        /* The initial active-area size. This can be resized with `resize`. */
        size::CellCountInt cols;
        size::CellCountInt rows;

        /* The maximum number of bytes allocated for pages. The effective limit
         * is raised when necessary to hold the active area. Null is unlimited. */
        Maybe<size_t> max_size;

        /* The maximum number of scrollback rows, excluding the active
         * area. Rows are physical (viewed) rows, so a wrapped row counts as
         * multiple rows. Max line pruning only happens at page-boundaries
         * (the minimum internal allocation size) so in practice the max lines
         * is always slightly larger than configured.
         *
         * Null is unlimited. */
        Maybe<size_t> max_lines;

        Options() : cols(0), rows(0), max_size(), max_lines() {}
        Options(size::CellCountInt c, size::CellCountInt r, Maybe<size_t> ms = Maybe<size_t>(),
                Maybe<size_t> ml = Maybe<size_t>())
            : cols(c), rows(r), max_size(ms), max_lines(ml) {}
    };

    static bool init(zigstd::Allocator alloc, const Options &opts, PageList *out);
    static bool initTrackedPins(zigstd::Allocator alloc, Pin *viewport_pin, PinSet *out);

    enum class InitPagesTw { page_node, page_buf_std, page_buf_non_std };
    typedef tripwire::Module<InitPagesTw, AllocTw, 3> initPages_tw;

    static bool initPages(MemoryPool *pool, uint64_t *serial, size::CellCountInt cols, size::CellCountInt rows,
                          List *out_list, size_t *out_size);

    /* Assert that the PageList is in a valid state. This is a no-op in
     * release builds. */
    void assertIntegrity() const {
        if (!slow_runtime_safety) return;

        const IntegrityError err = verifyIntegrity();
        if (err != IntegrityError::none) {
            /* log.err("PageList integrity check failed: {}") */
            fprintf(stderr, "PageList integrity check failed: %d\n", (int)err);
            abort();
        }
    }

    /* Pause or resume integrity checks. This is useful when you're doing
     * a multi-step operation that temporarily leaves the PageList in an
     * inconsistent state. */
    void pauseIntegrityChecks(bool pause) {
        if (!slow_runtime_safety) return;
        if (pause) {
            pause_integrity_checks += 1;
        } else {
            pause_integrity_checks -= 1;
        }
    }

    enum class IntegrityError {
        none,
        MaxLinesExceeded,
        PageSerialInvalid,
        TotalRowsMismatch,
        TrackedPinInvalid,
        ViewportPinOffsetMismatch,
        ViewportPinInsufficientRows,
    };

    IntegrityError verifyIntegrity() const;
    static void releasePages(MemoryPool *pool, const List &list);
    static void releasePoolPage(MemoryPool *pool, const Page *page);
    void deinit();
    void reset();

    /* ------------------------------------------------------------------ */
    /* Clone                                                                */

    struct Clone {
        /* The top and bottom (inclusive) points of the region to clone.
         * The x coordinate is ignored; the full row is always cloned. */
        point::Point top;
        Maybe<point::Point> bot; /* = null */

        /* If this is non-null then cloning will attempt to remap the tracked
         * pins into the new cloned area and will keep track of the old to
         * new mapping in this map. If this is null, the cloned pagelist will
         * not retain any previously tracked pins except those required for
         * internal operations.
         *
         * Any pins not present in the map were not remapped. */
        typedef std::unordered_map<Pin *, Pin *> TrackedPinsRemap;
        TrackedPinsRemap *tracked_pins; /* = null */

        Clone() : top(), bot(), tracked_pins(nullptr) {}
        explicit Clone(point::Point t, Maybe<point::Point> b = Maybe<point::Point>(), TrackedPinsRemap *r = nullptr)
            : top(t), bot(b), tracked_pins(r) {}
    };

    /* Clone this pagelist from the top to bottom (inclusive).
     *
     * The viewport is always moved to the active area.
     *
     * The cloned pagelist must contain at least enough rows for the active
     * area. If the region specified has less rows than the active area then
     * rows will be added to the bottom of the region to make up the difference.
     *
     * Wisp: `!PageList` — the error is a PageError (OutOfMemory or a
     * cloneFrom capacity error). */
    page::PageError clone(zigstd::Allocator alloc, const Clone &opts, PageList *out) const;

    /* ------------------------------------------------------------------ */
    /* Resize                                                               */

    /* Resize options */
    struct Resize {
        struct Cursor {
            size::CellCountInt x;
            size::CellCountInt y;

            /* When set, this pin preserves right-side blank cells up to the cursor
             * during reflow. */
            Pin *pin; /* = null */

            Cursor() : x(0), y(0), pin(nullptr) {}
            Cursor(size::CellCountInt x_, size::CellCountInt y_, Pin *p = nullptr) : x(x_), y(y_), pin(p) {}
        };

        /* The new cols/cells of the screen. */
        Maybe<size::CellCountInt> cols; /* = null */
        Maybe<size::CellCountInt> rows; /* = null */

        /* Whether to reflow the text. If this is false then the text will
         * be truncated if the new size is smaller than the old size. */
        bool reflow; /* = true */

        /* Set this to the current cursor position in the active area. Some
         * resize/reflow behavior depends on the cursor position. */
        Maybe<Cursor> cursor; /* = null */

        /* Whether the resize may pull rows out of scrollback back into the
         * active area. If false, growing rows always appends blank rows at the
         * bottom and a column reflow keeps the top of the active area on the
         * same content, so a line that is fully in scrollback stays there.
         * A wrapped line with at least one row still in the active area may
         * still unwrap back into view.
         *
         * This should be false for ptys that keep their own screen buffer
         * without scrollback (e.g. Windows ConPTY), since they can't pull
         * rows back and would otherwise get out of sync with us. */
        bool pull_scrollback; /* = true */

        Resize() : cols(), rows(), reflow(true), cursor(), pull_scrollback(true) {}
    };

    /* Resize
     * TODO: docs */
    bool resize(const Resize &opts) {
        struct Guard {
            const PageList *l;
            ~Guard() { l->assertIntegrity(); }
        } guard = {this};

        /* Resizing forces all nodes to be decompressed today so we need to
         * reschedule compression.
         * TODO(mitchellh): Deferred reflow on non-viewport/non-active pages. */
        page_compression.reset();
        page_compression.markActivity();

        {
            /* Resize does not work with 0 values, this should be protected
             * upstream */
            if (opts.cols.has) assert(opts.cols.value > 0);
            if (opts.rows.has) assert(opts.rows.value > 0);
        }

        /* Resizing (especially with reflow) can cause our row offset to
         * become invalid. Rather than do something fancy like we do other
         * places and try to update it in place, we just invalidate it because
         * its too easy to get the logic wrong in here. */
        viewport_pin_row_offset = Maybe<size_t>::none();

        if (!opts.reflow) {
            if (!resizeWithoutReflow(opts)) return false;
            /* Shrinking the active row count turns former active rows into
             * scrollback even without reflow, which can cross the line limit. */
            limits.enforce(this, Limits::Key::lines);
            return true;
        }

        /* Recalculate our minimum limits. This allows grow to work properly when
         * increasing beyond the explicit limits to fit the active area. */
        const Limits old_limits = limits;
        limits.resize(opts.cols.orelse(cols), opts.rows.orelse(rows));

        /* On reflow, the main thing that causes reflow is column changes. If
         * only rows change, reflow is impossible. So we change our behavior based
         * on the change of columns. */
        const size::CellCountInt new_cols = opts.cols.orelse(cols);
        bool ok = true;
        if (new_cols == cols) {
            ok = resizeWithoutReflow(opts);
        } else if (new_cols > cols) {
            /* We grow rows after cols so that we can do our unwrapping/reflow
             * before we do a no-reflow grow. */
            ok = resizeCols(new_cols, opts) && resizeWithoutReflow(opts);
        } else {
            /* We first change our row count so that we have the proper amount
             * we can use when shrinking our cols. */
            Resize copy = opts;
            copy.cols = cols;
            ok = resizeWithoutReflow(copy) && resizeCols(new_cols, opts);
        }
        if (!ok) {
            limits = old_limits;
            return false;
        }

        /* Various resize operations can change our total row count such
         * that our viewport pin is now in the active area and has insufficient
         * space. We need to check for this case and fix it up. */
        switch (viewport) {
        case Viewport::pin:
            if (pinIsActive(*viewport_pin)) viewport = Viewport::active;
            break;
        case Viewport::active:
        case Viewport::top: break;
        }

        /* Column reflow can change the physical history row count, and a row
         * resize can move the active boundary. Both may expose whole old pages
         * that are now eligible for line-limit pruning. */
        limits.enforce(this, Limits::Key::lines);
        return true;
    }

    /* Resize the pagelist with reflow by adding or removing columns. */
    bool resizeCols(size::CellCountInt new_cols, const Resize &opts);

    /* ------------------------------------------------------------------ */
    /* IncreaseCapacity                                                     */

    enum class IncreaseCapacity {
        styles,
        grapheme_bytes,
        hyperlink_bytes,
        string_bytes,
    };

    /* Returns the capacity dimension that must be increased for the
     * given row clone error to succeed on retry, or null if the page
     * only needs to be rehashed at its current capacity. */
    static Maybe<IncreaseCapacity> forCloneError(page::PageError err) {
        switch (err) {
        /* Rehash the sets */
        case page::PageError::StyleSetNeedsRehash:
        case page::PageError::HyperlinkSetNeedsRehash: return Maybe<IncreaseCapacity>::none();

        /* Increase style memory */
        case page::PageError::StyleSetOutOfMemory: return IncreaseCapacity::styles;

        /* Increase string memory */
        case page::PageError::StringAllocOutOfMemory: return IncreaseCapacity::string_bytes;

        /* Increase hyperlink memory */
        case page::PageError::HyperlinkSetOutOfMemory:
        case page::PageError::HyperlinkMapOutOfMemory: return IncreaseCapacity::hyperlink_bytes;

        /* Increase grapheme memory */
        case page::PageError::GraphemeMapOutOfMemory:
        case page::PageError::GraphemeAllocOutOfMemory: return IncreaseCapacity::grapheme_bytes;

        default: assert(false); return Maybe<IncreaseCapacity>::none();
        }
    }

    enum class IncreaseCapacityError {
        none,

        /* An actual system OOM trying to allocate memory. */
        OutOfMemory,

        /* The existing page is already at max capacity for the given
         * adjustment. The caller must create a new page, remove data from
         * the old page, etc. (up to the caller). */
        OutOfSpace,
    };

    /* Increase the capacity of the given page node in the given direction.
     * This will always allocate a new node and remove the old node, so the
     * existing node pointer will be invalid after this call. The newly created
     * node on success is returned.
     *
     * The increase amount is at the control of the PageList implementation,
     * but is guaranteed to always increase by at least one unit in the
     * given dimension. Practically, we'll always increase by much more
     * (we currently double every time) but callers shouldn't depend on that.
     * The only guarantee is some amount of growth.
     *
     * Adjustment can be null if you want to recreate, reclone the page
     * with the same capacity. This is a special case used for rehashing since
     * the logic is otherwise the same. In this case, OutOfMemory is the
     * only possible error. */
    IncreaseCapacityError increaseCapacity(Node *node, Maybe<IncreaseCapacity> adjustment, Node **out);

    /* ------------------------------------------------------------------ */
    /* ReflowCursor                                                         */

    /* We use a cursor to track where we are in the src/dst. This is very
     * similar to Screen.Cursor, so see that for docs on individual fields.
     * We don't use a Screen because we don't need all the same data and we
     * do our best to optimize having direct access to the page memory. */
    struct ReflowCursor {
        size::CellCountInt x;
        size::CellCountInt y;
        bool pending_wrap;
        Node *node;
        Page *page;
        page::Row *page_row;
        page::Cell *page_cell;
        size_t new_rows;

        /* This is the final row count of the reflowed pages. */
        size_t total_rows;

        struct StyleCache {
            const Page *src_page;
            style::Id src_id;
            style::Id dst_id;

            static StyleCache invalid() {
                StyleCache c = {nullptr, style::default_id, style::default_id};
                return c;
            }
        };

        /* Memoizes the most recent source-to-destination style id
         * mapping. Styled cells come in long runs sharing the same style
         * so this lets writeCell bump the destination ref count directly
         * instead of performing a set lookup for every styled cell.
         *
         * The destination id is only valid for the current destination
         * page, which is why this lives on the cursor: every destination
         * page change goes through init() which resets this. */
        StyleCache style_cache;

        /* Memoizes the capacity adjustment for new destination pages
         * (see reflowRow). It only depends on the source page so this is
         * keyed by the source page pointer, which reflow visits
         * sequentially and never revisits. */
        struct CapMemo {
            const Page *src_page;
            Capacity cap;
        };
        Maybe<CapMemo> cap_memo;

        static ReflowCursor init(Node *node) {
            Page *page = node->page();
            page::Row *rows = page->rows.ptr(page->memory);
            ReflowCursor c;
            c.x = 0;
            c.y = 0;
            c.pending_wrap = false;
            c.node = node;
            c.page = page;
            c.page_row = &rows[0];
            c.page_cell = &rows[0].cells().ptr(page->memory)[0];
            c.new_rows = 0;

            /* Initially whatever size our input node is. */
            c.total_rows = node->rows();

            c.style_cache = StyleCache::invalid();
            c.cap_memo = Maybe<CapMemo>::none();
            return c;
        }

        /* Reflow the provided row in to this cursor. */
        bool reflowRow(PageList *list, const Pin &row, Pin *cursor_pin) {
            Page *src_page = row.node->page();
            const page::Row *src_row = row.rowAndCell().row;
            const size::CellCountInt src_y = row.y;
            page::Cell *cells = src_row->cells().ptr(src_page->memory);

            /* Calculate the columns in this row. First up we trim non-semantic
             * rightmost blanks. */
            size_t cols_len = src_page->size.cols;
            if (!src_row->wrap()) {
                while (cols_len > 0) {
                    if (!cells[cols_len - 1].isEmpty()) break;
                    cols_len -= 1;
                }

                /* If the row has a semantic prompt then the blank row is meaningful
                 * so we just consider pretend the first cell of the row isn't empty. */
                if (cols_len == 0 && src_row->semantic_prompt() != page::Row::SemanticPrompt::none) cols_len = 1;
            }

            /* Handle tracked pin adjustments. We also note whether any
             * tracked pin is on this row at all so that the per-cell loop
             * below can skip pin scans entirely for the overwhelmingly
             * common case of a row with no pins. Note we compare nodes
             * rather than pages since a node owns exactly one page; this
             * is cheaper and avoids `page()` restoring unrelated
             * compressed nodes purely for a comparison. */
            bool row_has_pins = false;
            {
                for (size_t i = 0; i < list->tracked_pins.count(); i++) {
                    Pin *p = list->tracked_pins.keys()[i];
                    if (p->node != row.node || p->y != src_y) continue;

                    /* This row has pins */
                    row_has_pins = true;

                    if (cursor_pin != nullptr && p == cursor_pin) continue;

                    /* If this pin is in the blanks on the right and past the end
                     * of the dst col width then we move it to the end of the dst
                     * col width instead. */
                    if (p->x >= cols_len) {
                        const size_t lim = (size_t)page->size.cols - 1 - x;
                        p->x = (size::CellCountInt)(p->x < lim ? p->x : lim);
                    }

                    /* We increase our col len to at least include this pin.
                     * This ensures that blank rows with pins are processed,
                     * so that the pins can be properly remapped. */
                    if ((size_t)p->x + 1 > cols_len) cols_len = (size_t)p->x + 1;
                }
            }

            /* If the cursor is after blanks on the right, those cells are still
             * before the next write and must reflow with it. */
            if (cursor_pin) {
                if (cursor_pin->node == row.node && cursor_pin->y == src_y) {
                    if ((size_t)cursor_pin->x + 1 > cols_len) cols_len = (size_t)cursor_pin->x + 1;
                }
            }

            /* Defer processing of blank rows so that blank rows
             * at the end of the page list are never written. */
            if (cols_len == 0) {
                /* If this blank row was a wrap continuation somehow
                 * then we won't need to write it since it should be
                 * a part of the previously written row. */
                if (!src_row->wrap_continuation()) new_rows += 1;
                return true;
            }

            /* Inherit increased styles or grapheme bytes from the src page
             * we're reflowing from for new pages.
             *
             * This only depends on the source page, which we process row
             * by row, so memoize it: computing the adjustment requires a
             * full page layout calculation which is much too expensive to
             * do for every row. */
            Capacity cap;
            if (cap_memo.has && cap_memo.value.src_page == src_page) {
                cap = cap_memo.value.cap;
            } else {
                /* Source page changed, fall through to recompute. */
                cap = computeAndMemoizeCap(src_page);
            }

            /* Our row isn't blank, write any new rows we deferred. */
            while (new_rows > 0) {
                if (!cursorScrollOrNewPage(list, cap)) return false;
                new_rows -= 1;
            }

            copyRowMetadata(src_row);

            size_t sx = 0;
            while (sx < cols_len) {
                if (pending_wrap) {
                    page_row->setWrap(true);
                    if (!cursorScrollOrNewPage(list, cap)) return false;
                    copyRowMetadata(src_row);
                    page_row->setWrapContinuation(true);
                }

                /* Fast path: bulk-copy a run of simple cells directly
                 * into the destination row. The vast majority of cells
                 * are narrow cells or complete wide pairs, have no
                 * managed memory (graphemes, hyperlinks), and share a
                 * single style in long runs, so this avoids the
                 * per-cell state machine below for most of the work.
                 * Rows with tracked pins take the slow path so pin
                 * remapping behaves identically. */
                if (!row_has_pins) {
                    const size_t a = cols_len - sx;
                    const size_t b = (size_t)page->size.cols - x;
                    const size_t max_run = a < b ? a : b;
                    const page::Cell *window = cells + sx;
                    const size_t run = bulkRunLength(window, max_run);
                    if (run > 0 && copyRun(window, run, src_page)) {
                        sx += run;
                        continue;
                    }
                }

                /* Move any tracked pins from the source. */
                if (row_has_pins) {
                    for (size_t i = 0; i < list->tracked_pins.count(); i++) {
                        Pin *p = list->tracked_pins.keys()[i];
                        if (p->node != row.node || p->y != src_y || p->x != sx) continue;

                        p->node = node;
                        p->x = x;
                        p->y = y;
                    }
                }

                WriteResult result;
                const IncreaseCapacityError err = writeCell(list, &cells[sx], src_page, &result);
                if (err == IncreaseCapacityError::none) {
                    switch (result) {
                    /* Wrote the cell, move to the next. */
                    case WriteResult::success: sx += 1; break;

                    /* Wrote the cell but request to skip the next so skip it.
                     * This is used for things like spacers. */
                    case WriteResult::skip_next:
                        /* Remap any tracked pins at the skipped position (x+1)
                         * since we won't process that cell in the loop. */
                        if (row_has_pins) {
                            for (size_t i = 0; i < list->tracked_pins.count(); i++) {
                                Pin *p = list->tracked_pins.keys()[i];
                                if (p->node != row.node || p->y != src_y || p->x != sx + 1) continue;

                                p->node = node;
                                p->x = x;
                                p->y = y;
                            }
                        }

                        sx += 2;
                        break;

                    /* Didn't write the cell, repeat writing this same cell. */
                    case WriteResult::repeat: break;
                    }
                } else {
                    switch (err) {
                    /* System out of memory, we can't fix this. */
                    case IncreaseCapacityError::OutOfMemory: return false;

                    /* We reached the capacity of a single page and can't
                     * add any more of some type of managed memory. When this
                     * happens we split out the current row we're working on
                     * into a new page and continue from there. */
                    default:
                        if (y == 0) {
                            /* If we're already on the first-row, we can't split
                             * any further, so we just ignore bad cells and take
                             * corrupted (but valid) cell contents.
                             * log.warn("reflowRow OutOfSpace on first row, discarding cell managed memory") */
                            sx += 1;
                            cursorForward();
                        } else {
                            /* Move our last row to a new page. */
                            if (!moveLastRowToNewPage(list, cap)) return false;

                            /* Do NOT increment x so that we retry writing
                             * the same existing cell. */
                        }
                        break;
                    }
                }
            }

            /* If the source row isn't wrapped then we should scroll afterwards. */
            if (!src_row->wrap()) {
                new_rows += 1;
            }
            return true;
        }

        /* Compute and memoize the new-page capacity for the given source
         * page. See the call site in reflowRow for details. */
        Capacity computeAndMemoizeCap(const Page *src_page) {
            Capacity cap;
            if (!src_page->capacity.adjust(Capacity::Adjustment::withCols(page->size.cols), &cap)) {
                cap = src_page->capacity;
                cap.cols = page->size.cols;
                /* We're already a non-standard page. We don't want to
                 * inherit a massive set of rows, so cap it at our std size. */
                const size::CellCountInt std_rows = page::std_capacity().rows;
                cap.rows = src_page->size.rows < std_rows ? src_page->size.rows : std_rows;
            }

            CapMemo memo = {src_page, cap};
            cap_memo = memo;
            return cap;
        }

        /* True if this cell can be copied verbatim as part of a bulk
         * run: a narrow plain-text or bg-color cell with no managed
         * memory (graphemes, hyperlinks) and no special reflow handling
         * (wide characters, spacers, Kitty virtual placeholders). For
         * these cells writeCell reduces to a copy of the raw cell plus
         * a style ref count adjustment. */
        static bool bulkCopyable(const page::Cell &cell) {
            switch (cell.content_tag()) {
            case page::Cell::ContentTag::codepoint:
                if (cell.wide() != page::Cell::Wide::narrow) return false;
                if (cell.hyperlink()) return false;
                /* Placeholders must set a row flag, so they take
                 * the slow path. */
                if (cell.contentCodepoint() == kitty_placeholder) return false;
                return true;

            /* Grapheme data must be cloned cell-by-cell. */
            case page::Cell::ContentTag::codepoint_grapheme: return false;

            /* These are guaranteed to have no style or grapheme data
             * (see writeCell) so they are pure copies. The style
             * check is defensive so that a bg cell can never join or
             * extend a styled run. */
            default: return cell.style_id() == style::default_id;
            }
        }

        /* The group length for the vectorized bulk run scan below: the
         * SIMD lane count where the target supports it, otherwise a
         * plain unrolled group like other cell scans use (e.g. the
         * render state scans). */
        static const size_t bulk_group_len = 8;

        /* Masked compare helper covering every cell field that a run
         * must share to be copied by copyRun: given that the first cell
         * of a run passed the full bulkCopyable predicate, equality on
         * these fields implies the same for every subsequent cell, with
         * the same style.
         *
         * Note this is slightly stricter than bulkCopyable (e.g. a
         * bg-color cell won't extend an unstyled text run even though it
         * is copyable): that only splits the copy into multiple runs,
         * which is still correct. */
        typedef page::Mask<page::Cell, bulk_group_len> CellMask;
        static CellMask BulkRunMask() {
            return CellMask(page::fields::cell_content_tag() | page::fields::cell_style_id() |
                            page::fields::cell_wide() | page::fields::cell_hyperlink());
        }

        /* Masked compare helper for detecting the Kitty virtual
         * placeholder codepoint in text cells. Placeholders take the
         * slow path (they must set a row flag), so they terminate a run. */
        static CellMask PlaceholderMask() { return CellMask(page::fields::cell_content_codepoint_data()); }

        /* The length of the prefix of cells that can be copied at once
         * with copyRun: bulk-copyable cells sharing a single style. The
         * scan uses masked compares of the raw cell bits (see
         * BulkRunMask), which is significantly cheaper than the
         * field-wise predicate for this hot loop. */
        static size_t bulkRunLength(const page::Cell *cells, size_t cells_len) {
            if (cells_len == 0) return 0;
            const page::Cell first = cells[0];
            const CellMask bulk = BulkRunMask();
            const CellMask placeholder = PlaceholderMask();

            /* A run may start on a bulk-copyable narrow cell or on a wide pair. */
            const bool pair_start = first.content_tag() == page::Cell::ContentTag::codepoint &&
                                    first.wide() == page::Cell::Wide::wide && !first.hyperlink();
            if (!pair_start && !bulkCopyable(first)) return 0;

            /* Patterns for each cell shape admitted to the run. */
            page::Cell proto = first;
            proto.setWide(page::Cell::Wide::narrow);
            const uint64_t narrow_pattern = bulk.pattern(proto);
            proto.setWide(page::Cell::Wide::wide);
            const uint64_t wide_pattern = bulk.pattern(proto);
            proto.setWide(page::Cell::Wide::spacer_tail);
            const uint64_t tail_pattern = bulk.pattern(proto);

            /* Only text cells can contain a placeholder; for bg color
             * tags the content bits are a color, so we skip the check for
             * those (the tag is part of BulkRunMask, making tags uniform
             * per run). */
            const bool check_placeholder = first.content_tag() == page::Cell::ContentTag::codepoint;
            const uint64_t placeholder_pattern = placeholder.pattern(page::Cell::init(kitty_placeholder));

            size_t len = 0;
            for (;;) {
                /* Vectorized scan: check whole groups of narrow cells at
                 * once. If a group fully matches, the run extends by the
                 * whole group; otherwise fall through to the scalar loop
                 * below, which finds the exact end of the run within it
                 * or continues through a wide pair. */
                while (cells_len - len >= bulk_group_len) {
                    if (!bulk.eql(cells, len, narrow_pattern)) break;
                    if (check_placeholder && placeholder.eqlAny(cells, len, placeholder_pattern)) break;
                    len += bulk_group_len;
                }

                bool again = false;
                while (len < cells_len) {
                    const page::Cell cell = cells[len];
                    if (bulk.eqlScalar(cell, narrow_pattern)) {
                        if (check_placeholder && placeholder.eqlScalar(cell, placeholder_pattern)) return len;
                        len += 1;
                        continue;
                    }

                    /* Wide pairs are consumed atomically so the run
                     * (or window) end can never split a pair. */
                    if (len + 1 < cells_len && bulk.eqlScalar(cell, wide_pattern) &&
                        bulk.eqlScalar(cells[len + 1], tail_pattern)) {
                        len += 2;
                        /* Re-enter the vectorized loop: narrow groups
                         * commonly follow a stretch of pairs. */
                        again = true;
                        break;
                    }

                    return len;
                }
                if (again) continue;

                break;
            }

            return len;
        }

        /* Copy a run of bulk-copyable cells (see bulkCopyable) sharing
         * one style into the destination row at the current position,
         * then advance the cursor. The run must fit in the remaining
         * columns of the destination row.
         *
         * Returns false without any state change if the style could not
         * be mapped into the destination page without a capacity
         * change; the caller should fall back to writeCell which
         * handles growing capacity. */
        bool copyRun(const page::Cell *src_cells, size_t src_len, const Page *src_page) {
            assert(!pending_wrap);
            assert(src_len >= 1);
            assert(src_len <= (size_t)page->size.cols - x);

            const style::Id style_id = src_cells[0].style_id();
            const uint16_t n = (uint16_t)src_len;

            /* Resolve the destination style id for this run and take one
             * reference per cell. This mirrors the per-cell style logic
             * in writeCell, including the memoization (see StyleCache). */
            style::Id dst_style_id;
            if (style_id == style::default_id) {
                dst_style_id = style::default_id;
            } else if (style_cache.src_page == src_page && style_cache.src_id == style_id) {
                const style::Id id = style_cache.dst_id;
                page->styles.useMultiple((const void *)page->memory, id, n);
                dst_style_id = id;
            } else {
                const style::Style st = *src_page->styles.get((const void *)src_page->memory, style_id);

                /* Any error here (set full or needs rehash) is handled
                 * by falling back to the slow path, which grows capacity.
                 * No state has been modified yet at this point. */
                bool is_null;
                style::Id added;
                if (page->styles.addWithId((const void *)page->memory, st, style_id, &is_null, &added) !=
                    ref_counted_set::AddError::none)
                    return false;
                const style::Id id = is_null ? style_id : added;

                /* addWithId took one reference, take the rest. */
                if (n > 1) page->styles.useMultiple((const void *)page->memory, id, (uint16_t)(n - 1));

                style_cache.src_page = src_page;
                style_cache.src_id = style_id;
                style_cache.dst_id = id;

                dst_style_id = id;
            }

            /* Copy the raw cell contents. */
            page::Cell *dst_cells = page_cell;
            memcpy(dst_cells, src_cells, src_len * sizeof(page::Cell));

            /* If the style resolved to a different id in the destination
             * page then rewrite the copied cells to point at it. */
            if (dst_style_id != style_id) {
                for (size_t i = 0; i < src_len; i++) dst_cells[i].setStyleId(dst_style_id);
            }
            if (dst_style_id != style::default_id) page_row->setStyled(true);

            /* Advance the cursor, matching what repeated cursorForward
             * calls after each cell write would have done. */
            const size::CellCountInt pcols = page->size.cols;
            if ((size_t)x + n == pcols) {
                x = (size::CellCountInt)(pcols - 1);
                page_cell = page_cell + n - 1;
                pending_wrap = true;
            } else {
                x = (size::CellCountInt)(x + n);
                page_cell = page_cell + n;
            }

            return true;
        }

        enum class WriteResult { success, repeat, skip_next };

        /* Write a cell. On error, this will not unwrite the cell but
         * the cell may be incomplete (but valid). For example, if the source
         * cell is styled and we failed to allocate space for styles, the
         * written cell may not be styled but it is valid.
         *
         * The key failure to recognize for callers is when we can't increase
         * capacity in our destination page. In this case, the caller may want
         * to split the page at this row, rewrite the row into a new page
         * and continue from there.
         *
         * But this function guarantees the terminal/page will be in a
         * coherent state even on error. */
        IncreaseCapacityError writeCell(PageList *list, const page::Cell *cell, const Page *src_page,
                                        WriteResult *out) {
            /* Initialize self.page_cell with basic, unmanaged memory contents. */
            {
                /* This must not fail because we want to make sure we atomically
                 * setup our page cell to be valid. */

                /* Copy cell contents. */
                switch (cell->content_tag()) {
                case page::Cell::ContentTag::codepoint:
                case page::Cell::ContentTag::codepoint_grapheme:
                    switch (cell->wide()) {
                    case page::Cell::Wide::narrow: *page_cell = *cell; break;

                    case page::Cell::Wide::wide:
                        if (page->size.cols > 1) {
                            if (x == page->size.cols - 1) {
                                /* If there's a wide character in the last column of
                                 * the reflowed page then we need to insert a spacer
                                 * head and wrap before handling it. */
                                page::Cell head;
                                head.setContentTag(page::Cell::ContentTag::codepoint);
                                head.setContentCodepoint(0);
                                head.setWide(page::Cell::Wide::spacer_head);
                                *page_cell = head;

                                /* Move to the next row (this sets pending wrap
                                 * which will cause us to wrap on the next
                                 * iteration). */
                                cursorForward();

                                /* Decrement the source position so that when we
                                 * loop we'll process this source cell again,
                                 * since we can't copy it into a spacer head. */
                                *out = WriteResult::repeat;
                                return IncreaseCapacityError::none;
                            } else {
                                *page_cell = *cell;
                            }
                        } else {
                            /* Edge case, when resizing to 1 column, wide
                             * characters are just destroyed and replaced
                             * with empty narrow cells. */
                            page_cell->setContentCodepoint(0);
                            page_cell->setWide(page::Cell::Wide::narrow);
                            cursorForward();

                            /* Skip spacer tail so it doesn't cause a wrap. */
                            *out = WriteResult::skip_next;
                            return IncreaseCapacityError::none;
                        }
                        break;

                    case page::Cell::Wide::spacer_tail:
                        if (page->size.cols > 1) {
                            *page_cell = *cell;
                        } else {
                            /* Edge case, when resizing to 1 column, wide
                             * characters are just destroyed and replaced
                             * with empty narrow cells, so we should just
                             * discard any spacer tails. */
                            *out = WriteResult::success;
                            return IncreaseCapacityError::none;
                        }
                        break;

                    case page::Cell::Wide::spacer_head:
                        /* Spacer heads should be ignored. If we need a
                         * spacer head in our reflowed page, it is added
                         * when processing the wide cell it belongs to. */
                        *out = WriteResult::success;
                        return IncreaseCapacityError::none;
                    }
                    break;

                default:
                    /* These are guaranteed to have no style or grapheme
                     * data associated with them so we can fast path them. */
                    *page_cell = *cell;
                    cursorForward();
                    *out = WriteResult::success;
                    return IncreaseCapacityError::none;
                }

                /* These will create issues by trying to clone managed memory that
                 * isn't set if the current dst row needs to be moved to a new page.
                 * They'll be fixed once we do properly copy the relevant memory. */
                page_cell->setContentTag(page::Cell::ContentTag::codepoint);
                page_cell->setHyperlink(false);
                page_cell->setStyleId(style::default_id);

                /* Copy Kitty virtual placeholder status */
                if (cell->codepoint() == kitty_placeholder) {
                    page_row->setKittyVirtualPlaceholder(true);
                }
            }

            /* From this point on we're moving on to failable, managed memory.
             * If we reach an error, we do the minimal cleanup necessary to
             * not leave dangling memory but otherwise we gracefully degrade
             * into some functional but not strictly correct cell. */

            /* Copy grapheme data. */
            if (cell->content_tag() == page::Cell::ContentTag::codepoint_grapheme) {
                /* Copy the graphemes */
                size_t cps_len = 0;
                const uint32_t *cps = src_page->lookupGrapheme(cell, &cps_len);

                /* If our page can't support an additional cell
                 * with graphemes then we increase capacity. */
                if (page->graphemeCount() >= page->graphemeCapacity()) {
                    const IncreaseCapacityError e = increaseCapacity(list, IncreaseCapacity::grapheme_bytes);
                    if (e != IncreaseCapacityError::none) return e;
                }

                /* Attempt to allocate the space that would be required
                 * for these graphemes, and if it's not available, then
                 * increase capacity. Keep trying until we succeed. */
                for (;;) {
                    uint32_t *slice;
                    if (page->grapheme_alloc.alloc<uint32_t>((const void *)page->memory, cps_len, &slice)) {
                        page->grapheme_alloc.free((const void *)page->memory, slice, cps_len);
                        break;
                    } else {
                        /* Grow our capacity until we can fit the extra bytes. */
                        const IncreaseCapacityError e = increaseCapacity(list, IncreaseCapacity::grapheme_bytes);
                        if (e != IncreaseCapacityError::none) return e;
                    }
                }

                const page::PageError e = page->setGraphemes(page_row, page_cell, cps, cps_len);
                if (e != page::PageError::none) {
                    /* This shouldn't fail since we made sure we have space
                     * above. There is no reasonable behavior we can take here
                     * so we have a warn level log. This is ALMOST non-recoverable,
                     * though we choose to recover by corrupting the cell
                     * to a non-grapheme codepoint.
                     * log.err("setGraphemes failed after capacity increase err={}") */
                    if (slow_runtime_safety) {
                        /* Force a crash with safe builds. */
                        abort();
                    }

                    /* Unsafe builds we throw away grapheme data! */
                    page_cell->setContentTag(page::Cell::ContentTag::codepoint);
                    page_cell->setContentCodepoint(0xFFFD);
                }
            }

            /* Copy hyperlink data. */
            if (cell->hyperlink()) {
                do {
                    hyperlink::Id src_id;
                    const bool found = src_page->lookupHyperlink(cell, &src_id);
                    assert(found);
                    (void)found;
                    const hyperlink::PageEntry *src_link =
                        src_page->hyperlink_set.get((const void *)src_page->memory, src_id);

                    /* If our page can't support an additional cell
                     * with a hyperlink then we increase capacity. */
                    if (page->hyperlinkCount() >= page->hyperlinkCapacity()) {
                        const IncreaseCapacityError e = increaseCapacity(list, IncreaseCapacity::hyperlink_bytes);
                        if (e != IncreaseCapacityError::none) return e;
                    }

                    /* Ensure that the string alloc has sufficient capacity
                     * to dupe the link (and the ID if it's not implicit).
                     * Grow our capacity until the hyperlink fits. */
                    while (!hyperlinkStringsFit(src_link)) {
                        const IncreaseCapacityError e = increaseCapacity(list, IncreaseCapacity::string_bytes);
                        if (e != IncreaseCapacityError::none) return e;
                    }

                    hyperlink::PageEntry dst_link;
                    if (!src_link->dupe(src_page, page, &dst_link)) {
                        /* This shouldn't fail since we did a capacity
                         * check above.
                         * log.err("link dupe failed with capacity check err={}") */
                        if (slow_runtime_safety) abort();
                        break;
                    }

                    hyperlink::Id dst_id;
                    bool is_null;
                    hyperlink::Id added;
                    const ref_counted_set::AddError add_err = page->hyperlink_set.addWithIdContext(
                        (const void *)page->memory, dst_link, src_id, hyperlink::SetContext(page), &is_null, &added);
                    if (add_err != ref_counted_set::AddError::none) {
                        /* Always free our original link in case the increaseCap
                         * call fails so we aren't leaking memory. */
                        dst_link.free_(page);

                        /* If the add failed then either the set needs to grow
                         * or it needs to be rehashed. Either one of those can
                         * be accomplished by increasing capacity, either with
                         * no actual change or with an increased hyperlink cap. */
                        {
                            const IncreaseCapacityError e = increaseCapacity(
                                list, add_err == ref_counted_set::AddError::OutOfMemory
                                          ? Maybe<IncreaseCapacity>(IncreaseCapacity::hyperlink_bytes)
                                          : Maybe<IncreaseCapacity>::none());
                            if (e != IncreaseCapacityError::none) return e;
                        }

                        /* The increaseCapacity call above swapped self.page
                         * for a new page, so the string capacity check done
                         * before the first dupe no longer applies. Re-establish
                         * it against the current page before duping again. */
                        while (!hyperlinkStringsFit(src_link)) {
                            const IncreaseCapacityError e = increaseCapacity(list, IncreaseCapacity::string_bytes);
                            if (e != IncreaseCapacityError::none) return e;
                        }

                        /* We need to recreate the link into the new page. */
                        hyperlink::PageEntry dst_link2;
                        if (!src_link->dupe(src_page, page, &dst_link2)) {
                            /* This shouldn't fail since we did a capacity
                             * check above.
                             * log.err("link dupe failed with capacity check err={}") */
                            if (slow_runtime_safety) abort();
                            break;
                        }

                        /* We assume this one will succeed. We dupe the link
                         * again, and don't have to worry about the other one
                         * because increasing the capacity naturally clears up
                         * any managed memory not associated with a cell yet. */
                        const ref_counted_set::AddError add_err2 =
                            page->hyperlink_set.addWithIdContext((const void *)page->memory, dst_link2, src_id,
                                                                 hyperlink::SetContext(page), &is_null, &added);
                        if (add_err2 != ref_counted_set::AddError::none) {
                            /* This shouldn't happen since we increased capacity
                             * above so we handle it like the other similar
                             * cases and log it, crash in safe builds, and
                             * remove the hyperlink in unsafe builds.
                             * log.err("addWithIdContext failed after capacity increase err={}") */
                            if (slow_runtime_safety) abort();

                            dst_link2.free_(page);
                            break;
                        }
                    }
                    dst_id = is_null ? src_id : added;

                    /* We expect this to succeed due to the hyperlinkCapacity
                     * check we did before. If it doesn't succeed let's
                     * log it, crash (in safe builds), and clear our state. */
                    if (page->setHyperlink(page_row, page_cell, dst_id) != page::PageError::none) {
                        /* log.err("setHyperlink failed after capacity increase err={}") */
                        if (slow_runtime_safety) abort();

                        /* Unsafe builds we throw away hyperlink data! */
                        page->hyperlink_set.release((const void *)page->memory, dst_id);
                        page_cell->setHyperlink(false);
                        break;
                    }
                } while (false);
            }

            /* Copy style data. */
            if (cell->hasStyling()) {
                do {
                    /* Fast path: styled cells come in long runs sharing the
                     * same style. If this source style was just mapped into
                     * the current destination page, bump the ref count
                     * directly and skip the set lookup. The destination id is
                     * guaranteed alive because a previously written cell in
                     * this page holds a reference, and the cache is reset
                     * whenever the destination page changes (see init). */
                    if (style_cache.src_page == src_page && style_cache.src_id == cell->style_id()) {
                        const style::Id id = style_cache.dst_id;
                        page->styles.use((const void *)page->memory, id);
                        page_row->setStyled(true);
                        page_cell->setStyleId(id);
                        break;
                    }

                    const style::Style st = *src_page->styles.get((const void *)src_page->memory, cell->style_id());

                    bool is_null;
                    style::Id added;
                    style::Id id;
                    const ref_counted_set::AddError add_err =
                        page->styles.addWithId((const void *)page->memory, st, cell->style_id(), &is_null, &added);
                    if (add_err != ref_counted_set::AddError::none) {
                        /* If the add failed then either the set needs to grow
                         * or it needs to be rehashed. Either one of those can
                         * be accomplished by increasing capacity, either with
                         * no actual change or with an increased style cap. */
                        const IncreaseCapacityError e = increaseCapacity(
                            list, add_err == ref_counted_set::AddError::OutOfMemory
                                      ? Maybe<IncreaseCapacity>(IncreaseCapacity::styles)
                                      : Maybe<IncreaseCapacity>::none());
                        if (e != IncreaseCapacityError::none) return e;

                        /* We assume this one will succeed. */
                        if (page->styles.addWithId((const void *)page->memory, st, cell->style_id(), &is_null,
                                                   &added) != ref_counted_set::AddError::none) {
                            /* Should not fail since we just modified capacity
                             * above. Log it, crash in safe builds, clear style
                             * in unsafe builds.
                             * log.err("addWithId failed after capacity increase err={}") */
                            if (slow_runtime_safety) abort();

                            page_cell->setStyleId(style::default_id);
                            break;
                        }
                    }
                    id = is_null ? cell->style_id() : added;

                    /* Update our style cache with the latest style set so runs
                     * of cells with the same style are faster to write. */
                    style_cache.src_page = src_page;
                    style_cache.src_id = cell->style_id();
                    style_cache.dst_id = id;

                    page_row->setStyled(true);
                    page_cell->setStyleId(id);
                } while (false);
            }

            cursorForward();
            *out = WriteResult::success;
            return IncreaseCapacityError::none;
        }

        /* Create a new page in the provided list with the provided
         * capacity then clone the row currently being worked on to
         * it and delete it from the old page. Places cursor in the
         * same position it was in in the old row in the new one.
         *
         * Asserts that the cursor is on the final row of the page.
         *
         * Expects that the provided capacity is sufficient to copy
         * the row.
         *
         * If this is the only row in the page, the page is removed
         * from the list after cloning the row. */
        bool moveLastRowToNewPage(PageList *list, const Capacity &cap) {
            assert(y == page->size.rows - 1);
            assert(!pending_wrap);

            Node *old_node = node;
            Page *old_page = page;
            page::Row *old_row = page_row;
            const size::CellCountInt old_x = x;

            /* Our total row count never changes, because we're removing one
             * row from the last page and moving it into a new page. */
            const size_t old_total_rows = total_rows;

            if (!cursorNewPage(list, cap)) {
                total_rows = old_total_rows;
                return false;
            }
            assert(node != old_node);
            assert(y == 0);

            /* We have no cleanup for our old state from here on out. No failures! */

            /* Restore the x position of the cursor. */
            cursorAbsolute(old_x, 0);

            /* Copy our old data. This should NOT fail because we have the
             * capacity of the old page which already fits the data we requested. */
            if (page->cloneRowFrom(old_page, page_row, old_row) != page::PageError::none) {
                /* log.err("error cloning single row for moveLastRowToNewPage err={}") */
                fprintf(stderr, "unexpected copy row failure\n");
                abort();
            }

            /* Move any tracked pins from that last row into this new node. */
            {
                for (size_t i = 0; i < list->tracked_pins.count(); i++) {
                    Pin *p = list->tracked_pins.keys()[i];
                    if (p->node->page() != old_page || p->y != old_page->size.rows - 1) continue;

                    p->node = node;
                    p->y = y;
                    /* p.x remains the same since we're copying the row as-is */
                }
            }

            /* Reset the row on the old page and truncate it. The retired
             * storage must be left in the default state (see resetRow). */
            old_page->resetRow(old_row);
            old_page->size.rows -= 1;

            /* If that was the last row in that page
             * then we should remove it from the list. */
            if (old_page->size.rows == 0) {
                list->pages.remove(old_node);
                list->destroyNode(old_node);
            }

            total_rows = old_total_rows;
            return true;
        }

        /* Increase the capacity of the current page. */
        IncreaseCapacityError increaseCapacity(PageList *list, Maybe<IncreaseCapacity> adjustment) {
            const size::CellCountInt old_x = x;
            const size::CellCountInt old_y = y;
            const size_t old_total_rows = total_rows;

            Node *new_node;
            {
                /* Pause integrity checks because the total row count won't
                 * be correct during a reflow. */
                list->pauseIntegrityChecks(true);
                const IncreaseCapacityError e = list->increaseCapacity(node, adjustment, &new_node);
                list->pauseIntegrityChecks(false);
                if (e != IncreaseCapacityError::none) return e;
            }
            /* We must not fail after this, we've modified our self.node
             * and we need to fix it up. */

            *this = init(new_node);
            cursorAbsolute(old_x, old_y);
            total_rows = old_total_rows;
            return IncreaseCapacityError::none;
        }
        IncreaseCapacityError increaseCapacity(PageList *list, IncreaseCapacity adjustment) {
            return increaseCapacity(list, Maybe<IncreaseCapacity>(adjustment));
        }

        /* True if the string allocator of the current page can fit the
         * allocations that duping the given source hyperlink performs.
         * The test allocations are freed before returning.
         *
         * This must mirror the exact allocation pattern of
         * hyperlink.PageEntry.dupe: the URI and the explicit ID (if any)
         * are allocated separately. The string allocator rounds every
         * allocation up to its chunk size and requires each allocation to
         * be contiguous, so a single combined allocation of the total byte
         * length can succeed where the two separate allocations performed
         * by dupe would fail. */
        bool hyperlinkStringsFit(const hyperlink::PageEntry *src_link) const {
            uint8_t *uri_buf;
            if (!page->string_alloc.alloc<uint8_t>((const void *)page->memory, src_link->uri.len, &uri_buf))
                return false;

            bool ok = true;
            switch (src_link->id.tag) {
            case hyperlink::PageEntry::Id::Tag::implicit: break;
            case hyperlink::PageEntry::Id::Tag::explicit_: {
                uint8_t *id_buf;
                if (!page->string_alloc.alloc<uint8_t>((const void *)page->memory, src_link->id.explicit_.len,
                                                       &id_buf)) {
                    ok = false;
                    break;
                }
                page->string_alloc.free((const void *)page->memory, id_buf, src_link->id.explicit_.len);
                break;
            }
            }

            page->string_alloc.free((const void *)page->memory, uri_buf, src_link->uri.len);
            return ok;
        }

        /* True if this cursor is at the bottom of the page by capacity,
         * i.e. we can't scroll anymore. */
        bool bottom() const { return y == page->capacity.rows - 1; }

        void cursorForward() {
            if (x == page->size.cols - 1) {
                pending_wrap = true;
            } else {
                page_cell = page_cell + 1;
                x += 1;
            }
        }

        /* Create a new row and move the cursor down.
         *
         * Asserts that the cursor is on the bottom row of the
         * page and that there is capacity to add a new one. */
        void cursorScroll() {
            /* Scrolling requires that we're on the bottom of our page.
             * We also assert that we have capacity because reflow always
             * works within the capacity of the page. */
            assert(y == page->size.rows - 1);
            assert(page->size.rows < page->capacity.rows);

            /* Increase our page size */
            page->size.rows += 1;

            /* With the increased page size, safely move down a row. */
            page::Row *row = page_row + 1;
            page_row = row;
            page_cell = &row->cells().ptr(page->memory)[0];
            pending_wrap = false;
            x = 0;
            y += 1;
        }

        /* Create a new page in the provided list with the provided
         * capacity and one row and move the cursor in to it at 0,0 */
        bool cursorNewPage(PageList *list, const Capacity &cap) {
            /* Remember our new row count so we can restore it
             * after reinitializing our cursor on the new page. */
            const size_t saved_new_rows = new_rows;

            Node *n = list->createPage(CreatePage(cap));
            if (!n) return false;
            n->page()->size.rows = 1;
            list->pages.insertAfter(node, n);

            *this = init(n);
            new_rows = saved_new_rows;
            return true;
        }

        /* Performs `cursorScroll` or `cursorNewPage` as necessary
         * depending on if the cursor is currently at the bottom. */
        bool cursorScrollOrNewPage(PageList *list, const Capacity &cap) {
            /* The functions below may overwrite self so we need to cache
             * our total rows. We add one because no matter what when this
             * returns we'll have one more row added. */
            const size_t new_total_rows = total_rows + 1;

            bool ok = true;
            if (bottom()) {
                ok = cursorNewPage(list, cap);
            } else {
                cursorScroll();
            }
            total_rows = new_total_rows;
            return ok;
        }

        void cursorAbsolute(size::CellCountInt nx, size::CellCountInt ny) {
            assert(nx < page->size.cols);
            assert(ny < page->size.rows);

            page::Row *row;
            if (ny == y) {
                row = page_row;
            } else if (ny < y) {
                row = page_row - (y - ny);
            } else {
                row = page_row + (ny - y);
            }
            page_row = row;
            page_cell = &row->cells().ptr(page->memory)[nx];
            pending_wrap = false;
            x = nx;
            y = ny;
        }

        size_t countTrailingEmptyCells() const {
            /* If the row is wrapped, all empty cells are meaningful. */
            if (page_row->wrap()) return 0;

            const page::Cell *cells = page_cell;
            const size_t len = (size_t)page->size.cols - x;
            for (size_t i = 0; i < len; i++) {
                const size_t rev_i = len - i - 1;
                if (!cells[rev_i].isEmpty()) return i;
            }

            /* If the row has a semantic prompt then the blank row is meaningful
             * so we always return all but one so that the row is drawn. */
            if (page_row->semantic_prompt() != page::Row::SemanticPrompt::none) return len - 1;

            return len;
        }

        void copyRowMetadata(const page::Row *other) { page_row->setSemanticPrompt(other->semantic_prompt()); }
    };

    /* ------------------------------------------------------------------ */
    /* Page allocation                                                      */

    /* Options for createPage and createPageExt. */
    struct CreatePage {
        /* The capacity to allocate the page with. */
        Capacity cap;

        /* Force the page backing memory to be an exact-size heap
         * allocation even if it would fit within a standard-size pool
         * item. This is used when compacting pages to their minimum
         * size, since a pool item always retains a full std_size buffer
         * regardless of the page layout. */
        bool exact_size; /* = false */

        explicit CreatePage(const Capacity &c, bool exact = false) : cap(c), exact_size(exact) {}
    };

    /* One PageList-pooled page which has not yet joined the live page sequence. */
    struct PageAllocation {
        PageList *destination;
        Node *node;

        /* Return the fresh page storage for the caller to populate. */
        Page *page() { return node->pageAssumeResident(); }

        /* Release an uncommitted page back to its PageList's pools.
         *
         * This is safe to call after `finalize` succeeds, so callers can defer
         * it unconditionally. */
        void deinit() {
            if (!node) return;
            destroyNodeExt(&destination->pool, node, nullptr);
            node = nullptr;
        }

        enum class Location {
            /* Prepend the page to the start of the list (oldest history). */
            prepend,
        };

        enum class FinalizeError {
            none,
            InvalidPageDimensions,
            RowCountOverflow,
            PageSizeOverflow,
            MaxSizeExceeded,
            MaxLinesExceeded,
        };

        /* Finalize this complete page and transfer its ownership to the PageList.
         * The parameter determines where it goes into the PageList.
         *
         * Existing pages and tracked pins keep their identity. A pinned viewport
         * keeps showing the same content while its cached absolute row offset
         * moves down by the number of newly inserted rows. */
        FinalizeError finalize(Location location) {
            switch (location) {
            case Location::prepend: return prepend();
            }
            return FinalizeError::none;
        }

        FinalizeError prepend() {
            PageList *dest = destination;
            Node *n = node;

            /* Validate the populated page and all resulting accounting before
             * publishing the detached node into the live list. */
            if (n->cols() == 0 || n->rows() == 0) return FinalizeError::InvalidPageDimensions;
            if (dest->total_rows > SIZE_MAX - n->rows()) return FinalizeError::RowCountOverflow;
            const size_t new_total_rows = dest->total_rows + n->rows();
            const size_t node_size =
                n->owned == Node::Owned::pool ? PagePool::item_size : n->pageAssumeResident()->memory_len;
            if (dest->page_size > SIZE_MAX - node_size) return FinalizeError::PageSizeOverflow;
            const size_t new_page_size = dest->page_size + node_size;

            /* Restored history is exact data, so reject a page which cannot
             * coexist with the receiving PageList's configured limits. */
            if (new_page_size > dest->limits.max(Limits::Key::bytes)) {
                return FinalizeError::MaxSizeExceeded;
            }
            if (new_total_rows - dest->rows > dest->limits.max(Limits::Key::lines)) {
                return FinalizeError::MaxLinesExceeded;
            }

            /* No fallible work remains. Publish the page and update every cached
             * quantity affected by inserting rows above the existing first page. */
            dest->pages.prepend(n);
            dest->page_size = new_page_size;
            dest->total_rows = new_total_rows;
            if (dest->viewport == Viewport::pin) {
                if (dest->viewport_pin_row_offset.has) {
                    dest->viewport_pin_row_offset.value += n->rows();
                }
            }
            dest->page_compression.markActivity();

            dest->assertIntegrity();
            node = nullptr;
            return FinalizeError::none;
        }
    };

    /* Allocate a new page using the PageList's memory pools.
     *
     * The page is detached: it doesn't contribute to the memory limits or
     * row counts or anything in the PageList. The caller must call `finalize`
     * to add it to the PageList at the appropriate place, or `deinit` to
     * throw it away. Wisp: false is OutOfMemory. */
    bool allocatePage(const Capacity &capacity, PageAllocation *out) {
        Node *n = createPageExt(&pool, CreatePage(capacity), &page_serial, nullptr);
        if (!n) return false;
        out->destination = this;
        out->node = n;
        return true;
    }

    /* Create a new page node. This does not add it to the list and this
     * does not do any memory size accounting with max_size/page_size.
     * Wisp: null is OutOfMemory. */
    Node *createPage(const CreatePage &opts) {
        /* If we have a node available for recycling (only during reflow,
         * see recycle_node), reuse it directly rather than going through
         * the memory pool. */
        if (Node *node = recycle_node) {
            do {
                /* Only a standard pool-owned resident node can be rebuilt
                 * in place for a standard-size layout. */
                if (opts.exact_size) break;
                if (node->owned != Node::Owned::pool) break;
                if (node->data.tag != Node::Data::Tag::resident) break;
                const Page::Layout layout = Page::layout(opts.cap);
                if (layout.total_size > std_size) break;

                recycle_node = nullptr;

                /* The pool guarantees that buffers it hands out are zeroed.
                 * A pool-owned page dirties only its Page.memory prefix of
                 * the underlying standard-size item, so zeroing that prefix
                 * re-establishes the guarantee (this mirrors destroyNodeExt,
                 * minus the decommit). */
                Page *page = &node->data.resident();
                uint8_t *item = page->memory;
                memset(page->memory, 0, page->memory_len);

                /* Accounting: a pool-owned node always accounts for a full
                 * pool item in page_size, so destroying the node and
                 * creating a new pooled one is a net zero. */

                node->initResident(Page::initBuf(size::OffsetBuf::init(item), layout), page_serial,
                                   Node::Owned::pool);
                node->page()->size.rows = 0;
                page_serial += 1;
                return node;
            } while (false);
        }

        return createPageExt(&pool, opts, &page_serial, &page_size);
    }

    static Node *createPageExt(MemoryPool *pool, const CreatePage &opts, uint64_t *serial, size_t *total_size) {
        Node *node = pool->nodes.create();
        if (!node) return nullptr;

        const Page::Layout layout = Page::layout(opts.cap);
        const bool pooled = !opts.exact_size && layout.total_size <= std_size;
        const zigstd::Allocator page_alloc = pool->pages.allocator;

        /* It would be better to encode this into the Zig error handling
         * system but that is a big undertaking and we only have a few
         * centralized call sites so it is handled on its own currently. */
        assert(layout.total_size <= size::max_page_size);

        /* Our page buffer comes from our standard memory pool if it
         * is within our standard size since this is what the pool
         * dispenses. Otherwise, we use the heap allocator to allocate. */
        uint8_t *page_buf;
        size_t page_buf_len;
        if (pooled) {
            PageItem *buf = pool->pages.create();
            if (!buf) {
                pool->nodes.destroy(node);
                return nullptr;
            }
            mem::recommit(buf->bytes, std_size);
            page_buf = buf->bytes;
            page_buf_len = std_size;
        } else {
            page_buf = page_alloc.alignedAlloc(layout.total_size, page_size_min);
            if (!page_buf) {
                pool->nodes.destroy(node);
                return nullptr;
            }
            page_buf_len = layout.total_size;
        }

        /* In runtime safety modes, allocators fill with 0xAA. On freestanding
         * (WASM), the WasmAllocator reuses freed slots without zeroing.
         *
         * Otherwise, we rely on pool item buffers being zeroed: fresh items
         * come from the OS page allocator (zeroed pages), destroyNodeExt
         * zeroes buffers before returning them to the pool, and the pool
         * never writes into its items (see PagePool).
         * Wisp: the test allocator is the C heap, so tests zero here. */
        if (is_test) memset(page_buf, 0, page_buf_len);

        node->initResident(Page::initBuf(size::OffsetBuf::init(page_buf), layout), *serial,
                           pooled ? Node::Owned::pool : Node::Owned::heap);
        node->page()->size.rows = 0;
        *serial += 1;

        if (total_size) {
            /* Accumulate page size now. We don't assert or check max size
             * because we may exceed it here temporarily as we are allocating
             * pages before destroy. */
            *total_size += page_buf_len;
        }

        return node;
    }
    /* ------------------------------------------------------------------ */
    /* resizeWithoutReflow                                                  */

    bool resizeWithoutReflow(const Resize &opts) {
        /* We only set the new minimums if we're not reflowing. If we are
         * reflowing, then the outer resize call handles this for us. */
        const Limits old_limits = limits;
        if (!opts.reflow) limits.resize(opts.cols.orelse(cols), opts.rows.orelse(rows));

        /* Important! We have to do cols first because cols may cause us to
         * destroy pages if we're increasing cols which will free up page_size
         * so that when we call grow() in the row mods, we won't prune. */
        if (opts.cols.has) {
            const size::CellCountInt new_cols = opts.cols.value;
            /* Any column change without reflow should not result in row counts
             * changing. */
            const size_t old_total_rows = total_rows;

            if (new_cols < cols) {
                /* Making our columns smaller. We always have space for this
                 * in existing pages so we need to go through the pages,
                 * resize the columns, and clear any cells that are beyond
                 * the new size. */
                PageIterator it = pageIterator(Direction::right_down, point::Point::screen(), Maybe<point::Point>());
                Chunk chunk;
                while (it.next(&chunk)) {
                    Page *page = chunk.node->page();
                    page::Row *prows = page->rows.ptr(page->memory);
                    for (size_t i = 0; i < page->size.rows; i++) {
                        page->clearCells(&prows[i], new_cols, cols);
                    }

                    page->size.cols = new_cols;
                    page->assertIntegrity();
                }

                /* Update all our tracked pins. If they have an X
                 * beyond the edge, clamp it. */
                for (size_t i = 0; i < tracked_pins.count(); i++) {
                    Pin *p = tracked_pins.keys()[i];
                    if (p->x >= new_cols) p->x = (size::CellCountInt)(new_cols - 1);
                }

                cols = new_cols;
            } else if (new_cols > cols) {
                /* Make our columns larger. This is a bit more complicated because
                 * pages may not have the capacity for this. If they don't have
                 * the capacity we need to allocate a new page and copy the data.
                 *
                 * See the comment in the while loop when setting self.cols */
                const size::CellCountInt old_cols = cols;

                PageIterator it = pageIterator(Direction::right_down, point::Point::screen(), Maybe<point::Point>());
                Chunk chunk;
                while (it.next(&chunk)) {
                    /* We need to restore our old cols after we resize because
                     * we have an assertion on this and we want to be able to
                     * call this method multiple times. */
                    cols = old_cols;
                    if (!resizeWithoutReflowGrowCols(new_cols, chunk)) {
                        limits = old_limits;
                        return false;
                    }
                }

                cols = new_cols;
            }
            assert(total_rows == old_total_rows);
            (void)old_total_rows;
        }

        if (opts.rows.has) {
            const size::CellCountInt new_rows = opts.rows.value;
            if (new_rows < rows) {
                /* Making rows smaller, we simply change our rows value. Changing
                 * the row size doesn't affect anything else since max size and
                 * so on are all byte-based.
                 *
                 * If our rows are shrinking, we prefer to trim trailing
                 * blank lines from the active area instead of creating
                 * history if we can.
                 *
                 * This matches macOS Terminal.app behavior. I chose to match that
                 * behavior because it seemed fine in an ocean of differing behavior
                 * between terminal apps. I'm completely open to changing it as long
                 * as resize behavior isn't regressed in a user-hostile way. */
                const size::CellCountInt trimmed = trimTrailingBlankRows((size::CellCountInt)(rows - new_rows));

                /* Account for our trimmed rows in the total row cache */
                total_rows -= trimmed;

                /* If we didn't trim enough, just modify our row count and this
                 * will create additional history. */
                rows = new_rows;
            } else if (new_rows > rows) {
                /* Making rows larger we adjust our row count, and then grow
                 * to the row count.
                 *
                 * If our rows increased and our cursor is NOT at the bottom,
                 * we want to try to preserve the y value of the old cursor.
                 * In other words, we don't want to "pull down" scrollback.
                 * This is purely a UX feature.
                 *
                 * If we're not allowed to pull scrollback at all then we
                 * always do this regardless of the cursor. */
                bool pull;
                if (!opts.pull_scrollback) {
                    pull = false;
                } else if (!opts.cursor.has) {
                    pull = true;
                } else {
                    pull = opts.cursor.value.y >= rows - 1;
                }
                if (!pull) {
                    /* We just grow our rows and we're done. Cursor does
                     * NOT change for this since we're not pulling down
                     * scrollback. */
                    const size_t delta = (size_t)new_rows - rows;
                    rows = new_rows;
                    for (size_t i = 0; i < delta; i++) {
                        Node *g;
                        if (!grow(&g)) {
                            limits = old_limits;
                            return false;
                        }
                    }
                } else {
                    /* This must be set BEFORE any calls to grow() so that
                     * grow() doesn't prune pages that we need for the active
                     * area. */
                    rows = new_rows;

                    /* Cursor is at the bottom or we don't care about cursors.
                     * In this case, if we have enough rows in our pages, we
                     * just update our rows and we're done. This effectively
                     * "pulls down" scrollback.
                     *
                     * This traversal intentionally reads only node metadata. A
                     * compressed history page pulled into the active area remains
                     * compressed until a renderer or other content consumer goes
                     * through `Node.page`, which restores it transparently.
                     *
                     * If we don't have enough scrollback, we add the difference,
                     * to the active area. */
                    size_t count = 0;
                    bool enough = false;
                    for (Node *p = pages.first; p; p = p->next) {
                        count += p->rows();
                        if (count >= new_rows) {
                            enough = true;
                            break;
                        }
                    }
                    if (!enough) {
                        assert(count < new_rows);
                        for (size_t i = count; i < new_rows; i++) {
                            Node *g;
                            if (!grow(&g)) {
                                limits = old_limits;
                                return false;
                            }
                        }
                    }

                    /* Make sure that the viewport pin isn't below the active
                     * area, since that will lead to all sorts of problems. */
                    switch (viewport) {
                    case Viewport::pin:
                        if (pinIsActive(*viewport_pin)) viewport = Viewport::active;
                        break;
                    default: break;
                    }
                }
            }

            if (slow_runtime_safety) {
                /* We never have less rows than our active screen has. */
                assert(totalRows() >= rows);
            }
        }
        return true;
    }

    bool resizeWithoutReflowGrowCols(size::CellCountInt new_cols, const Chunk &chunk) {
        assert(new_cols > cols);
        Page *page = chunk.node->page();

        /* Update our col count */
        const size::CellCountInt old_cols = cols;
        cols = new_cols;

        /* Unlikely fast path: we have capacity in the page. This
         * is only true if we resized to less cols earlier. */
        if (page->capacity.cols >= new_cols) {
            /* If any row has a spacer head at the old last column, it will
             * be invalid at the new (wider) size. Fall through to the slow
             * path which handles spacer heads correctly via cloneRowFrom. */
            bool fast = true;
            page::Row *prows = page->rows.ptr(page->memory);
            for (size_t i = 0; i < page->size.rows; i++) {
                const page::Cell *cells = page->getCells(&prows[i]);
                if (cells[old_cols - 1].wide() == page::Cell::Wide::spacer_head) {
                    fast = false;
                    break;
                }
            }

            if (fast) {
                page->size.cols = new_cols;
                return true;
            }
        }

        /* Likely slow path: we don't have capacity, so we need
         * to allocate a page, and copy the old data into it.
         *
         * Try to fit our new column size into our existing page capacity.
         * If that doesn't work then use a non-standard page with the
         * given columns. */
        Capacity cap;
        if (!page->capacity.adjust(Capacity::Adjustment::withCols(new_cols), &cap)) {
            /* We verify all maxed out page layouts don't overflow, */
            cap = page->capacity;
            cap.cols = new_cols;

            /* We're growing columns so we can only get less rows so use
             * the lesser of our capacity and size so we minimize wasted
             * rows. */
            cap.rows = page->size.rows < cap.rows ? page->size.rows : cap.rows;
        }

        /* On error, we need to undo all the pages we've added. */
        Node *prev = chunk.node->prev;

        /* Keeps track of all our copied rows. Assertions at the end is that
         * we copied exactly our page size. */
        size::CellCountInt copied = 0;

        /* This function has an unfortunate side effect in that it causes memory
         * fragmentation on rows if the columns are increasing in a way that
         * shrinks capacity rows. If we have pages that don't divide evenly then
         * we end up creating a final page that is not using its full capacity.
         * If this chunk isn't the last chunk in the page list, then we've created
         * a page where we'll never reclaim that capacity. This makes our max size
         * calculation incorrect since we'll throw away data even though we have
         * excess capacity. To avoid this, we try to fill our previous page
         * first if it has capacity.
         *
         * This can fail for many reasons (can't fit styles/graphemes, etc.) so
         * if it fails then we give up and drop back into creating new pages. */
        if (prev) {
            do {
                Node *prev_node = prev;
                Page *prev_page = prev_node->page();

                /* We only want scenarios where we have excess capacity. */
                if (prev_page->size.rows >= prev_page->capacity.rows) break;

                /* We can copy as much as we can to fill the capacity or our
                 * current page size. */
                const size_t a = (size_t)prev_page->capacity.rows - prev_page->size.rows;
                const size_t len = a < page->size.rows ? a : page->size.rows;

                page::Row *src_rows = page->rows.ptr(page->memory);
                page::Row *dst_rows = prev_page->rows.ptr(prev_page->memory) + prev_page->size.rows;
                bool failed = false;
                for (size_t i = 0; i < len; i++) {
                    prev_page->size.rows += 1;
                    copied += 1;
                    if (prev_page->cloneRowFrom(page, &dst_rows[i], &src_rows[i]) != page::PageError::none) {
                        /* If an error happens, we undo our row copy and break out
                         * into creating a new page. */
                        prev_page->size.rows -= 1;
                        copied -= 1;
                        failed = true;
                        break;
                    }
                }
                if (failed) break;

                assert(copied == len);
                assert(prev_page->size.rows <= prev_page->capacity.rows);

                /* Remap any tracked pins that pointed to rows we just copied to prev. */
                for (size_t i = 0; i < tracked_pins.count(); i++) {
                    Pin *p = tracked_pins.keys()[i];
                    if (p->node != chunk.node || p->y >= len) continue;
                    p->node = prev_node;
                    p->y = (size::CellCountInt)(p->y + prev_page->size.rows - len);
                }
            } while (false);
        }

        /* If we have an error, we clear the rows we just added to our prev page. */
        const size::CellCountInt prev_copied = copied;
        auto errdefer = [&]() {
            /* We delete any of the nodes we added. */
            {
                Node *it = chunk.node->prev;
                while (it) {
                    Node *node = it;
                    if (node == prev) break;
                    it = node->prev;
                    pages.remove(node);
                    destroyNode(node);
                }
            }
            if (prev_copied > 0) {
                Page *prev_page = prev->page();
                const size::CellCountInt prev_size = (size::CellCountInt)(prev_page->size.rows - prev_copied);
                page::Row *prev_rows = prev_page->rows.ptr(prev_page->memory);
                for (size_t i = prev_size; i < prev_page->size.rows; i++) prev_page->resetRow(&prev_rows[i]);
                prev_page->size.rows = prev_size;
            }
            cols = old_cols;
        };

        /* We need to loop because our col growth may force us
         * to split pages. */
        while (copied < page->size.rows) {
            Node *new_node = createPage(CreatePage(cap));
            if (!new_node) {
                errdefer();
                return false;
            }
            Page *new_page = new_node->page();

            /* The length we can copy into the new page is at most the number
             * of rows in our cap. But if we can finish our source page we use that. */
            const size_t remaining = (size_t)page->size.rows - copied;
            const size_t len = cap.rows < remaining ? cap.rows : remaining;

            /* Perform the copy */
            const size::CellCountInt y_start = copied;
            page::Row *src_rows = page->rows.ptr(page->memory) + y_start;
            page::Row *dst_rows = new_page->rows.ptr(new_page->memory);
            for (size_t i = 0; i < len; i++) {
                new_page->size.rows += 1;
                if (new_page->cloneRowFrom(page, &dst_rows[i], &src_rows[i]) == page::PageError::none) {
                    copied += 1;
                } else {
                    /* I don't THINK this should be possible, because while our
                     * row count may diminish due to the adjustment, our
                     * prior capacity should have been sufficient to hold all the
                     * managed memory.
                     * log.warn("unexpected cloneRowFrom failure during resizeWithoutReflowGrowCols")
                     *
                     * We can actually safely handle this though by exiting
                     * this loop early and cutting our copy short. */
                    new_page->size.rows -= 1;
                    break;
                }
            }
            const size::CellCountInt y_end = copied;

            /* Insert our new page */
            pages.insertBefore(chunk.node, new_node);

            /* Update our tracked pins that pointed to this previous page. */
            for (size_t i = 0; i < tracked_pins.count(); i++) {
                Pin *p = tracked_pins.keys()[i];
                if (p->node != chunk.node || p->y < y_start || p->y >= y_end) continue;
                p->node = new_node;
                p->y -= y_start;
            }
            new_page->assertIntegrity();
        }
        assert(copied == page->size.rows);

        /* Our prior errdeferes are invalid after this point so ensure
         * we don't have any more errors.
         *
         * Remove the old page.
         * Deallocate the old page. */
        pages.remove(chunk.node);
        destroyNode(chunk.node);
        return true;
    }

    /* Returns the number of trailing blank lines, not to exceed max. Max
     * is used to limit our traversal in the case of large scrollback. */
    size::CellCountInt trailingBlankLines(size::CellCountInt max) const {
        size::CellCountInt count = 0;

        /* Go through our pages backwards since we're counting trailing blanks. */
        for (Node *node = pages.last; node; node = node->prev) {
            Page *page = node->page();
            const size_t len = node->rows();
            page::Row *prows = page->rows.ptr(page->memory);
            for (size_t i = 0; i < len; i++) {
                const size_t rev_i = len - i - 1;
                const page::Cell *cells = prows[rev_i].cells().ptr(page->memory);

                /* If the row has any text then we're done. */
                if (page::Cell::hasTextAny(cells, node->cols())) return count;

                /* Inc count, if we're beyond max then we're done. */
                count += 1;
                if (count >= max) return count;
            }
        }

        return count;
    }

    /* Trims up to max trailing blank rows from the pagelist and returns the
     * number of rows trimmed. A blank row is any row with no text (but may
     * have styling).
     *
     * IMPORTANT: This function does NOT update `total_rows`. It returns the
     * number of rows trimmed, and the caller is responsible for decrementing
     * `total_rows` by this amount. */
    size::CellCountInt trimTrailingBlankRows(size::CellCountInt max) {
        size::CellCountInt trimmed = 0;
        Node *invalidated_node = nullptr;
        const Pin bl_pin = getBottomRight(point::Tag::screen).value;
        RowIterator it = bl_pin.rowIterator(Direction::left_up, Maybe<Pin>());
        Pin row_pin;
        while (it.next(&row_pin)) {
            size_t cells_len;
            const page::Cell *cells = row_pin.cells(Pin::CellSubset::all, &cells_len);

            /* If the row has any text then we're done. */
            if (page::Cell::hasTextAny(cells, cells_len)) return trimmed;

            /* If our tracked pins are in this row then we cannot trim it
             * because it implies some sort of importance. If we trimmed this
             * we'd invalidate this pin, as well. */
            for (size_t i = 0; i < tracked_pins.count(); i++) {
                const Pin *p = tracked_pins.keys()[i];
                if (p->node != row_pin.node || p->y != row_pin.y) continue;
                return trimmed;
            }

            /* No text, we can trim this row. Because it has
             * no text we can also be sure it has no styling
             * so we don't need to worry about memory. */
            if (row_pin.node->rows() > 1 && invalidated_node != row_pin.node) {
                /* Shrinking a retained page changes its valid row-coordinate range. */
                invalidateNodeLayout(row_pin.node);
                invalidated_node = row_pin.node;
            }

            /* The row has no text but can still carry metadata (e.g. a
             * blank prompt continuation line) and background-colored
             * cells. The retired storage is re-exposed by the grow()
             * fast path without any clearing, so it must be left in the
             * default state. */
            row_pin.node->page()->resetRow(row_pin.rowAndCell().row);

            row_pin.node->page()->size.rows -= 1;
            if (row_pin.node->page()->size.rows == 0) {
                erasePage(row_pin.node);
            } else {
                row_pin.node->page()->assertIntegrity();
            }

            trimmed += 1;
            if (trimmed >= max) return trimmed;
        }

        return trimmed;
    }
    /* ------------------------------------------------------------------ */
    /* Scroll                                                               */

    /* Scroll options. */
    struct Scroll {
        enum class Tag {
            /* Scroll to the active area. This is also sometimes referred to as
             * the "bottom" of the screen. This makes it so that the end of the
             * screen is fully visible since the active area is the bottom
             * rows/cols of the screen. */
            active,

            /* Scroll to the top of the screen, which is the farthest back in
             * the scrollback history. */
            top,

            /* Scroll to the given absolute row from the top. A value of zero
             * is the top row. This row will be the first visible row in the viewport.
             * Scrolling into or below the active area will clamp to the active area. */
            row,

            /* Scroll up (negative) or down (positive) by the given number of
             * rows. This is clamped to the "top" and "active" top left. */
            delta_row,

            /* Jump forwards (positive) or backwards (negative) a set number of
             * prompts. If the absolute value is greater than the number of prompts
             * in either direction, jump to the furthest prompt in that direction. */
            delta_prompt,

            /* Scroll directly to a specific pin in the page. This will be set
             * as the top left of the viewport (ignoring the pin x value). */
            pin,
        } tag;
        size_t row;
        ptrdiff_t delta;
        Pin pin;

        static Scroll active() { Scroll s; s.tag = Tag::active; return s; }
        static Scroll top() { Scroll s; s.tag = Tag::top; return s; }
        static Scroll rowAt(size_t n) { Scroll s; s.tag = Tag::row; s.row = n; return s; }
        static Scroll deltaRow(ptrdiff_t n) { Scroll s; s.tag = Tag::delta_row; s.delta = n; return s; }
        static Scroll deltaPrompt(ptrdiff_t n) { Scroll s; s.tag = Tag::delta_prompt; s.delta = n; return s; }
        static Scroll pinAt(const Pin &p) { Scroll s; s.tag = Tag::pin; s.pin = p; return s; }

        Scroll() : tag(Tag::active), row(0), delta(0), pin() {}
    };

    /* Scroll the viewport. This will never create new scrollback, allocate
     * pages, etc. This can only be used to move the viewport within the
     * previously allocated pages. */
    void scroll(const Scroll &behavior) {
        struct Guard {
            const PageList *l;
            ~Guard() { l->assertIntegrity(); }
        } guard = {this};

        /* Special case no-scrollback mode to never allow scrolling. */
        if (limits.bytes.explicit_ == 0) {
            viewport = Viewport::active;
            return;
        }

        /* Moving the viewport changes which historical pages are visible. Restart
         * traversal so pages which leave the viewport are reconsidered after the
         * renderer's idle delay. False positives from clamped scrolling are cheap. */
        struct Restart {
            PageList *l;
            ~Restart() {
                l->page_compression.reset();
                l->page_compression.markActivity();
            }
        } restart = {this};

        switch (behavior.tag) {
        case Scroll::Tag::active: viewport = Viewport::active; break;
        case Scroll::Tag::top: viewport = Viewport::top; break;
        case Scroll::Tag::pin: {
            const Pin &p = behavior.pin;
            if (pinIsActive(p)) {
                viewport = Viewport::active;
                return;
            } else if (pinIsTop(p)) {
                viewport = Viewport::top;
                return;
            }

            *viewport_pin = p;
            viewport = Viewport::pin;
            viewport_pin_row_offset = Maybe<size_t>::none(); /* invalidate cache */
            break;
        }
        case Scroll::Tag::row: {
            const size_t n = behavior.row;
            /* If we're at the top, pin the top. */
            if (n == 0) {
                viewport = Viewport::top;
                break;
            }

            /* If we're below the top of the active area, pin the active area. */
            if (n >= total_rows - rows) {
                viewport = Viewport::active;
                break;
            }

            /* See if there are any other faster paths we can take. */
            if (viewport == Viewport::pin && viewport_pin_row_offset.has) {
                /* If we have a pin and we already calculated a row offset,
                 * then we can efficiently calculate the delta and move
                 * that much from that pin. */
                const ptrdiff_t delta = (ptrdiff_t)n - (ptrdiff_t)viewport_pin_row_offset.value;
                scroll(Scroll::deltaRow(delta));
                return;
            }

            /* We have an accurate row offset so store it to prevent
             * calculating this again. */
            viewport_pin_row_offset = n;
            viewport = Viewport::pin;

            /* Slow path, we've just got to traverse the linked list and
             * get to our row. As a slight speedup, let's pick the traversal
             * that's likely faster based on our absolute row and total rows. */
            const size_t midpoint = total_rows / 2;
            if (n < midpoint) {
                /* Iterate forward from the first node. */
                size_t rem = n;
                for (Node *node = pages.first; node; node = node->next) {
                    if (rem < node->rows()) {
                        if (rem > 0xFFFF) {
                            viewport = Viewport::active;
                            return;
                        }
                        *viewport_pin = Pin(node, (size::CellCountInt)rem);
                        return;
                    }

                    rem -= node->rows();
                }
            } else {
                /* Iterate backwards from the last node. */
                size_t rem = total_rows - n;
                for (Node *node = pages.last; node; node = node->prev) {
                    if (rem <= node->rows()) {
                        const size_t yy = node->rows() - rem;
                        if (yy > 0xFFFF) {
                            viewport = Viewport::active;
                            return;
                        }
                        *viewport_pin = Pin(node, (size::CellCountInt)yy);
                        return;
                    }

                    rem -= node->rows();
                }
            }

            /* If we reached here, then we couldn't find the offset.
             * This feels impossible? Just clamp to active, screw it lol. */
            viewport = Viewport::active;
            break;
        }
        case Scroll::Tag::delta_prompt: scrollPrompt(behavior.delta); break;
        case Scroll::Tag::delta_row: {
            const ptrdiff_t n = behavior.delta;
            const size_t amount = n < 0 ? (size_t)0 - (size_t)n : (size_t)n; /* @abs */

            switch (viewport) {
            /* If we're at the top and we're scrolling backwards,
             * we don't have to do anything, because there's nowhere to go. */
            case Viewport::top:
                if (n <= 0) return;
                break;

            /* If we're at active and we're scrolling forwards, we don't
             * have to do anything because it'll result in staying in
             * the active. */
            case Viewport::active:
                if (n >= 0) return;
                break;

            /* If we're already a pin type, then we can fast-path our
             * delta by simply moving the pin. This has the added benefit
             * that we can update our row offset cache efficiently, too. */
            case Viewport::pin:
                if (n == 0) return;
                if (n < 0) {
                    const Pin::Overflow o = viewport_pin->upOverflow(amount);
                    if (o.tag == Pin::Overflow::Tag::offset) {
                        *viewport_pin = o.offset;
                        if (viewport_pin_row_offset.has) viewport_pin_row_offset.value -= amount;
                        return;
                    }
                    /* If we overflow up we're at the top. */
                    viewport = Viewport::top;
                    return;
                } else {
                    const Pin::Overflow o = viewport_pin->downOverflow(amount);
                    /* If we offset its a valid pin but we still have to
                     * check if we're in the active area. */
                    if (o.tag == Pin::Overflow::Tag::offset) {
                        if (pinIsActive(o.offset)) {
                            viewport = Viewport::active;
                        } else {
                            *viewport_pin = o.offset;
                            if (viewport_pin_row_offset.has) viewport_pin_row_offset.value += amount;
                        }
                        return;
                    }
                    /* If we overflow down we're at active. */
                    viewport = Viewport::active;
                    return;
                }
            }

            /* Slow path: we have to calculate the new pin by moving
             * from our viewport. */
            const Pin top = getTopLeft(point::Tag::viewport);
            Pin p;
            if (n < 0) {
                const Pin::Overflow o = top.upOverflow(amount);
                p = o.tag == Pin::Overflow::Tag::offset ? o.offset : o.overflow.end;
            } else {
                const Pin::Overflow o = top.downOverflow(amount);
                p = o.tag == Pin::Overflow::Tag::offset ? o.offset : o.overflow.end;
            }

            /* If we are still within the active area, then we pin the
             * viewport to active. This isn't EXACTLY the same behavior as
             * other scrolling because normally when you scroll the viewport
             * is pinned to _that row_ even if new scrollback is created.
             * But in a terminal when you get to the bottom and back into the
             * active area, you usually expect that the viewport will now
             * follow the active area. */
            if (pinIsActive(p)) {
                viewport = Viewport::active;
                return;
            }

            /* If we're at the top, then just set the top. This is a lot
             * more efficient everywhere. We must check this after the
             * active check above because we prefer active if they overlap. */
            if (pinIsTop(p)) {
                viewport = Viewport::top;
                return;
            }

            /* Pin is not active so we need to track it. */
            *viewport_pin = p;
            viewport = Viewport::pin;
            viewport_pin_row_offset = Maybe<size_t>::none(); /* invalidate cache */
            break;
        }
        }
    }

    /* Jump the viewport forwards (positive) or backwards (negative) a set number of
     * prompts (delta). */
    void scrollPrompt(ptrdiff_t delta) {
        /* If we aren't jumping any prompts then we don't need to do anything. */
        if (delta == 0) return;
        const size_t delta_start = delta < 0 ? (size_t)0 - (size_t)delta : (size_t)delta;
        size_t delta_rem = delta_start;

        /* We start at the row before or after our viewport depending on the
         * delta so that we don't land back on our current viewport. */
        Pin start_pin;
        {
            const Pin tl = getTopLeft(point::Tag::viewport);

            /* If we're moving up we can just move the viewport up because
             * promptIterator handles jumpting to the start of prompts. */
            if (delta <= 0) {
                const Maybe<Pin> u = tl.up(1);
                if (!u.has) return;
                start_pin = u.value;
            } else {
                /* If we're moving down and we're presently at some kind of
                 * prompt, we need to skip all the continuation lines because
                 * promptIterator can't know if we're cutoff or continuing. */
                const Maybe<Pin> d = tl.down(1);
                if (!d.has) return;
                Pin adjusted = d.value;
                if (tl.rowAndCell().row->semantic_prompt() != page::Row::SemanticPrompt::none) {
                    while (adjusted.rowAndCell().row->semantic_prompt() ==
                           page::Row::SemanticPrompt::prompt_continuation) {
                        const Maybe<Pin> dd = adjusted.down(1);
                        if (!dd.has) break;
                        adjusted = dd.value;
                    }
                }

                start_pin = adjusted;
            }
        }

        /* Go through prompts delta times */
        PromptIterator it =
            start_pin.promptIterator(delta > 0 ? Direction::right_down : Direction::left_up, Maybe<Pin>());
        Maybe<Pin> prompt_pin;
        Pin next;
        while (it.next(&next)) {
            prompt_pin = next;
            delta_rem -= 1;
            if (delta_rem == 0) break;
        }

        /* If we found a prompt, we move to it. If the prompt is in the active
         * area we keep our viewport as active because we can't scroll DOWN
         * into the active area. Otherwise, we scroll up to the pin. */
        if (prompt_pin.has) {
            const Pin &p = prompt_pin.value;
            if (pinIsActive(p)) {
                viewport = Viewport::active;
            } else {
                *viewport_pin = p;
                viewport = Viewport::pin;
                viewport_pin_row_offset = Maybe<size_t>::none(); /* invalidate cache */
            }
        }
    }

    /* Clear the screen by scrolling written contents up into the scrollback.
     * This will not update the viewport. */
    bool scrollClear() {
        struct Guard {
            const PageList *l;
            ~Guard() { l->assertIntegrity(); }
        } guard = {this};

        /* Go through the active area backwards to find the first non-empty
         * row. We use this to determine how many rows to scroll up. */
        size_t non_empty = 0;
        {
            Node *node = pages.last;
            size_t n = 0;
            bool done = false;
            while (!done) {
                Page *current_page = node->page();
                page::Row *prows = current_page->rows.ptr(current_page->memory);
                for (size_t i = 0; i < node->rows() && !done; i++) {
                    const size_t rev_i = node->rows() - i - 1;
                    const page::Row row = prows[rev_i];
                    const page::Cell *cells = row.cells().ptr(current_page->memory);
                    for (size_t k = 0; k < cols; k++) {
                        if (!cells[k].isEmpty()) {
                            non_empty = rows - n;
                            done = true;
                            break;
                        }
                    }
                    if (done) break;

                    n += 1;
                    if (n > rows) {
                        non_empty = 0;
                        done = true;
                    }
                }
                if (done) break;

                node = node->prev;
                if (!node) {
                    non_empty = 0;
                    break;
                }
            }
        }

        /* Scroll */
        for (size_t i = 0; i < non_empty; i++) {
            Node *g;
            if (!grow(&g)) return false;
        }
        return true;
    }

    /* Give a live node a new generation before changing its coordinate layout in
     * place.
     *
     * This must be called when a node remains at the same address and stays in the
     * list, but a mutation changes which logical row a `(node, y)` coordinate
     * identifies or whether that coordinate is still in range. Examples include
     * rotating rows after an erase, truncating a page's row range, reinitializing
     * the sole remaining page, or shortening the source page of a split. Without
     * a new generation, a cached pointer, serial, and coordinate could still pass
     * `nodeIsValid` while referring to a different row than it originally did.
     * Screen and Terminal fast paths which manipulate Page rows directly must use
     * this because they bypass PageList's own row-mutation helpers.
     *
     * The caller must pass a node which is currently live in this PageList. Call
     * this at the point the operation commits to changing the layout and before
     * the first such change. When an operation is intended to be atomic, finish
     * its failable preparation first so a failed operation does not needlessly
     * invalidate references. One call per affected node is sufficient when an
     * operation performs several layout changes without exposing intermediate
     * references.
     *
     * This helper is only needed for in-place changes. Removing or replacing a
     * node already invalidates old references through the live-list check in
     * `nodeIsValid`, and newly allocated or reused nodes receive a fresh serial as
     * part of their initialization. Ordinary cell or style changes which preserve
     * the meaning of page coordinates do not require a new generation solely for
     * that reason.
     *
     * The only state changed here is `node.serial` and the next-generation counter
     * `page_serial`. Consequently, every previously captured pointer-plus-serial
     * pair for this node becomes invalid while new references can capture the new
     * generation. This does not advance `page_serial_epoch`: only `reset` starts a
     * new whole-list validity epoch.
     *
     * This also does not mutate the page, update tracked pins or viewport state,
     * adjust row or memory accounting, mark cells dirty, or notify incremental
     * compression. The surrounding operation remains responsible for all such
     * bookkeeping required by its layout change. */
    void invalidateNodeLayout(Node *node) {
        node->serial = page_serial;
        page_serial += 1;
    }

    /* Compact a page to use the minimum required memory for the contents
     * it stores. Returns the new node pointer if compaction occurred, or null
     * if the page was already compact or compaction would not provide any
     * savings.
     *
     * The compacted page is always an exact-size heap allocation, never
     * a pool item, since a pool item always retains a full std_size
     * buffer regardless of the page layout. Note that this means that
     * when compacting a pool-owned node, the freed pool item is returned
     * to the pool free list, so the memory savings are only fully
     * realized once the pool itself is reset or freed.
     *
     * If this returns OOM, the PageList is left unchanged and no dangling
     * memory references exist. It is safe to ignore the error and continue using
     * the uncompacted page.
     * Wisp: false is OutOfMemory; *out is null when no compaction happened. */
    bool compact(Node *node, Node **out) {
        struct Guard {
            const PageList *l;
            ~Guard() { l->assertIntegrity(); }
        } guard = {this};
        *out = nullptr;
        Page *page = node->page();

        /* We should never have empty rows in our pagelist anyways... */
        assert(page->size.rows > 0);

        /* Compute the minimum capacity required for this page's content */
        const Capacity req_cap = page->exactRowCapacity(0, page->size.rows);
        const size_t new_size = Page::layout(req_cap).total_size;

        /* The memory this node currently retains. A pool-owned node always
         * retains a full pool item no matter its layout size. */
        const size_t old_size = node->owned == Node::Owned::pool ? PagePool::item_size : page->memory_len;
        if (new_size >= old_size) return true;

        /* Create the new smaller page */
        Node *new_node = createPage(CreatePage(req_cap, true));
        if (!new_node) return false;
        Page *new_page = new_node->page();
        new_page->size = page->size;
        new_page->dirty = page->dirty;
        if (new_page->cloneFrom(page, 0, page->size.rows) != page::PageError::none) {
            /* cloneFrom should not fail when compacting since req_cap is
             * computed to exactly fit the source content and our expectation
             * of exactRowCapacity ensures it can fit all the requested
             * data.
             * log.err("compact clone failed err={}")
             *
             * In this case, let's gracefully degrade by pretending we
             * didn't need to compact. */
            destroyNode(new_node);
            return true;
        }

        /* Fix up all tracked pins to point to the new page */
        for (size_t i = 0; i < tracked_pins.count(); i++) {
            Pin *p = tracked_pins.keys()[i];
            if (p->node != node) continue;
            p->node = new_node;
        }

        /* Insert the new page and destroy the old one */
        pages.insertBefore(node, new_node);
        pages.remove(node);
        destroyNode(node);
        page_compression.markActivity();

        new_page->assertIntegrity();
        *out = new_node;
        return true;
    }
    /* ------------------------------------------------------------------ */
    /* split / scrollbar / viewport / grow                                  */

    enum class SplitError {
        none,
        /* Allocator OOM */
        OutOfMemory,
        /* Page can't be split further because it is already a single row. */
        OutOfSpace,
    };

    /* Split the given node in the PageList at the given pin.
     *
     * The row at the pin and after will be moved into a new page with
     * the same capacity as the original page. Alternatively, you can "split
     * above" by splitting the row following the desired split row.
     *
     * Since the split happens below the pin, the pin remains valid. */
    SplitError split(const Pin p_in) {
        if (slow_runtime_safety) assert(pinIsValid(p_in));

        /* Ran into a bug that I can only explain via aliasing. If a tracked
         * pin is passed in, its possible Zig will alias the memory and then
         * when we modify it later it updates our p here. Copying the node
         * fixes this. */
        const Pin p = p_in;
        Node *original_node = p.node;
        Page *page = original_node->page();

        /* A page that is already 1 row can't be split. In the future we can
         * theoretically maybe split by soft-wrapping multiple pages but that
         * seems crazy and the rest of our PageList can't handle heterogeneously
         * sized pages today. */
        if (page->size.rows <= 1) return SplitError::OutOfSpace;

        /* Splitting at row 0 is a no-op since there's nothing before the split point. */
        if (p.y == 0) return SplitError::none;

        /* At this point we're doing actual modification so make sure
         * on the return that we're good. */
        struct Guard {
            const PageList *l;
            ~Guard() { l->assertIntegrity(); }
        } guard = {this};

        /* Create a new node with the same capacity of managed memory. */
        Node *target = createPage(CreatePage(page->capacity));
        if (!target) return SplitError::OutOfMemory;

        /* Determine how many rows we're copying */
        const size::CellCountInt y_start = p.y;
        const size::CellCountInt y_end = page->size.rows;
        target->page()->size.rows = (size::CellCountInt)(y_end - y_start);
        assert(target->rows() <= target->capacity().rows);

        /* Copy our old data. This should NOT fail because we have the
         * capacity of the old page which already fits the data we requested. */
        if (target->page()->cloneFrom(page, y_start, y_end) != page::PageError::none) {
            /* log.err("error cloning rows for split err={}")
             *
             * Rather than crash, we return an OutOfSpace to show that
             * we couldn't split and let our callers gracefully handle it.
             * Realistically though... this should not happen. */
            destroyNode(target);
            return SplitError::OutOfSpace;
        }

        /* From this point forward there is no going back. We have no
         * error handling. It is possible but we haven't written it.
         *
         * Failable split work is complete; shortening the source changes its row range. */
        invalidateNodeLayout(original_node);
        page_compression.markActivity();

        /* Move any tracked pins from the copied rows */
        for (size_t i = 0; i < tracked_pins.count(); i++) {
            Pin *tracked = tracked_pins.keys()[i];
            if (tracked->node->page() != page || tracked->y < p.y) continue;

            tracked->node = target;
            tracked->y -= p.y;
            /* p.x remains the same since we're copying the row as-is */
        }

        /* Reset our rows. They are retired into unused page capacity,
         * which the grow() fast path re-exposes without any clearing, so
         * they must be left in the default state. */
        page::Row *prows = page->rows.ptr(page->memory);
        for (size_t y = y_start; y < y_end; y++) {
            page->resetRow(&prows[y]);
        }
        page->size.rows -= (size::CellCountInt)(y_end - y_start);

        pages.insertAfter(original_node, target);
        return SplitError::none;
    }

    /* This represents the state necessary to render a scrollbar for this
     * PageList. It has the total size, the offset, and the size of the viewport. */
    struct Scrollbar {
        /* Total size of the scrollable area. */
        size_t total;

        /* The offset into the total area that the viewport is at. This is
         * guaranteed to be less than or equal to total. This includes the
         * visible row. */
        size_t offset;

        /* The length of the visible area. This is including the offset row. */
        size_t len;

        /* A zero-sized scrollable region. */
        static Scrollbar zero() {
            Scrollbar s = {0, 0, 0};
            return s;
        }

        /* Sync with: ghostty_action_scrollbar_s */
        struct C {
            uint64_t total;
            uint64_t offset;
            uint64_t len;
        };

        C cval() const {
            C c = {(uint64_t)total, (uint64_t)offset, (uint64_t)len};
            return c;
        }

        /* Comparison for scrollbars. */
        bool eql(const Scrollbar &other) const {
            return total == other.total && offset == other.offset && len == other.len;
        }
    };

    /* Return the scrollbar state for this PageList.
     *
     * This is amortized O(1): the total is maintained incrementally and
     * the viewport offset is cached. The first call after the viewport
     * moves to an arbitrary pin (e.g. scrolling to a selection) may cost
     * O(pages) to compute the offset, after which it is cached again.
     * See viewportRowOffset for more details. */
    Scrollbar scrollbar() {
        /* If we have no scrollback, special case no scrollbar.
         * We need to do this because the way PageList works is that
         * it always has SOME extra space (due to the way we allocate by page).
         * So even with no scrollback we have some growth. It is architecturally
         * much simpler to just hide that for no-scrollback cases. */
        if (limits.bytes.explicit_ == 0) {
            Scrollbar s = {rows, 0, rows};
            return s;
        }

        Scrollbar s = {total_rows, viewportRowOffset(), rows /* Length is always rows */};
        return s;
    }

    /* Returns the offset of the current viewport from the top of the
     * screen.
     *
     * This is potentially expensive to calculate because if the viewport
     * is a pin and the pin is near the beginning of the scrollback, we
     * will traverse a lot of linked list nodes.
     *
     * The result is cached so repeated calls are cheap. */
    size_t viewportRowOffset() {
        switch (viewport) {
        case Viewport::top: return 0;
        case Viewport::active: return total_rows - rows;
        default: break;
        }

        /* We assert integrity on this code path because it verifies
         * that the cached value is correct. */
        struct Guard {
            const PageList *l;
            ~Guard() { l->assertIntegrity(); }
        } guard = {this};

        /* Return cached value if available */
        if (viewport_pin_row_offset.has) return viewport_pin_row_offset.value;

        /* Traverse from the end and count rows until we reach the
         * viewport pin. We count backwards because most of the time
         * a user is scrolling near the active area. */
        size_t top_offset = 0;
        {
            size_t offset = 0;
            bool found = false;
            for (Node *n = pages.last; n; n = n->prev) {
                offset += n->rows();
                if (n == viewport_pin->node) {
                    assert(n->rows() > viewport_pin->y);
                    offset -= viewport_pin->y;
                    top_offset = total_rows - offset;
                    found = true;
                    break;
                }
            }

            /* Invalid pins are not possible. */
            assert(found);
            (void)found;
        }

        /* The offset is from the bottom and our cached value and this
         * function returns from the top, so we need to invert it. */
        viewport_pin_row_offset = top_offset;
        return top_offset;
    }

    /* This fixes up the viewport data when rows are removed from the
     * PageList. This will update a viewport to `active` if row removal
     * puts the viewport into the active area, to `top` if the viewport
     * is now at row 0, and updates any row offset caches as necessary.
     *
     * This is unit tested transitively through other tests such as
     * eraseRows. */
    void fixupViewport(size_t removed) {
        /* Page removal can mark every pin on the removed page as garbage. The
         * viewport pin is an internal navigation anchor that is always remapped,
         * so it remains valid after the removal. */
        viewport_pin->garbage = false;

        switch (viewport) {
        case Viewport::active: break;

        /* For pin, we check if our pin is now in the active area and if so
         * we move our viewport back to the active area. */
        case Viewport::pin:
            if (pinIsActive(*viewport_pin)) {
                viewport = Viewport::active;
            } else if (viewport_pin_row_offset.has) {
                /* If we have a cached row offset, we need to update it
                 * to account for the erased rows. */
                if (viewport_pin_row_offset.value < removed) {
                    viewport = Viewport::top;
                } else {
                    viewport_pin_row_offset.value -= removed;
                }
            }
            break;

        /* For top, we move back to active if our erasing moved our
         * top page into the active area. */
        case Viewport::top:
            if (pinIsActive(Pin(pages.first))) {
                viewport = Viewport::active;
            }
            break;
        }
    }

    /* Change the maximum logical page allocation at runtime. Null removes the
     * explicit byte limit and zero disables scrollback.
     *
     * Lowering the limit immediately removes eligible complete historical pages.
     * The effective limit may still be raised to fit the active area, and a page
     * which overlaps the active area is never split solely to satisfy this limit. */
    void setMaxBytes(Maybe<size_t> max) {
        limits.set(Limits::Key::bytes, max);
        limits.enforce(this, Limits::Key::bytes);
        if (limits.bytes.explicit_ == 0) viewport = Viewport::active;
        assertIntegrity();
    }

    /* Change the maximum number of physical scrollback rows at runtime. Null
     * removes the explicit line limit.
     *
     * Lowering the limit immediately removes eligible complete historical pages.
     * The effective limit always permits at least one standard page of history,
     * and a page which overlaps the active area is never split for enforcement. */
    void setMaxLines(Maybe<size_t> max) {
        limits.set(Limits::Key::lines, max);
        limits.enforce(this, Limits::Key::lines);
        assertIntegrity();
    }

    /* Grow the active area by exactly one row.
     *
     * This may allocate, but also may not if our current page has more
     * capacity we can use. This will prune scrollback if necessary to
     * adhere to max_size and max_lines.
     *
     * This returns the newly allocated page node if there is one.
     * Wisp: false is OutOfMemory; *out is the ?*List.Node result. */
    bool grow(Node **out) {
        struct Guard {
            const PageList *l;
            ~Guard() { l->assertIntegrity(); }
        } guard = {this};
        *out = nullptr;

        /* Growing can move a complete page behind the active boundary. */
        page_compression.markActivity();

        Node *last = pages.last;
        if (last->capacity().rows > last->rows()) {
            /* Fast path: we have capacity in the last page. The exposed
             * row requires no clearing work here: rows in unused page
             * capacity are always in the default zero state, either
             * because the page memory was never used (pool buffers are
             * zeroed) or because whatever retired the row reset it (see
             * Page.resetRow). */
            Page *page = last->page();
            page->size.rows += 1;
            page->assertIntegrity();

            /* Increase our total rows by one */
            total_rows += 1;

            /* Growing inside the last page moves the active boundary without
             * allocating; that alone can make the first page wholly historical. */
            limits.enforce(this, Limits::Key::lines);
            return true;
        }

        /* Slower path: we have no space, we need to allocate a new page.
         *
         * Get the layout first so our failable work is done early.
         * We'll need this for both paths. */
        const Capacity cap = initialCapacity(cols);

        /* If allocation would exceed our max size, we prune the first page.
         * We don't need to reallocate because we can simply reuse that first
         * page.
         *
         * We only take this path if we have more than one page since pruning
         * reuses the popped page. It is possible to have a single page and
         * exceed the max size if that page was adjusted to be larger after
         * initial allocation. */
        if (pages.first != nullptr && pages.first != pages.last &&
            page_size + PagePool::item_size > limits.max(Limits::Key::bytes)) {
            do {
                Node *first = pages.popFirst();
                assert(first != last);

                /* Decrease our total row count from the pruned page */
                total_rows -= first->rows();

                /* If our total row count is now less than our required
                 * rows then we can't prune. The "+ 1" is because we'll add one
                 * more row below. */
                if (total_rows + 1 < rows) {
                    pages.prepend(first);
                    assert(pages.first == first);
                    total_rows += first->rows();
                    break;
                }

                /* If we have a pin viewport cache then we need to update it. */
                if (viewport == Viewport::pin) {
                    if (viewport_pin_row_offset.has) {
                        /* If our offset is less than the number of rows in the
                         * pruned page, then we are now at the top. */
                        if (viewport_pin_row_offset.value < first->rows()) {
                            viewport = Viewport::top;
                        } else {
                            /* Otherwise, our viewport pin is below what we pruned
                             * so we just decrement our offset. */
                            viewport_pin_row_offset.value -= first->rows();
                        }
                    }
                }

                /* Update any tracked pins that point to this page to point to the
                 * new first page to the top-left, and mark them as garbage. */
                for (size_t i = 0; i < tracked_pins.count(); i++) {
                    Pin *p = tracked_pins.keys()[i];
                    if (p->node != first) continue;
                    p->node = pages.first;
                    p->y = 0;
                    p->x = 0;
                    p->garbage = true;
                }
                viewport_pin->garbage = false;

                switch (first->owned) {
                /* Pool-owned pages are reused below. */
                case Node::Owned::pool: break;

                /* Heap-owned pages can't be reused because they may be
                 * any size (larger or smaller than a standard page), so
                 * just destroy them. */
                case Node::Owned::heap: destroyNode(first); break;
                }
                if (first->owned == Node::Owned::heap) break;

                /* Reset our memory */
                Page *old = first->restore(Node::RestoreMode::discard);
                uint8_t *buf = old->memory;
                memset(buf, 0, old->memory_len);
                assert(old->memory_len <= std_size);

                /* Initialize our new page and reinsert it as the last */
                first->data.tag = Node::Data::Tag::resident;
                first->data.compressed.page = Page::initBuf(size::OffsetBuf::init(buf), Page::layout(cap));
                Page *page = first->page();
                page->size.rows = 1;
                pages.insertAfter(last, first);
                total_rows += 1;

                /* Reusing the node gives it a fresh generation. Do not begin a new
                 * page_serial_epoch here: generations are not monotonic in list order,
                 * so older live successors may have lower generations. The epoch only
                 * advances when reset invalidates the entire list. */
                first->serial = page_serial;
                page_serial += 1;

                /* In this case we do NOT need to update page_size because
                 * we're reusing an existing page so nothing has changed. */

                page->assertIntegrity();

                /* Byte-limit recycling may leave history above the independent line
                 * limit, so enforce it after the recycled page becomes the new tail. */
                limits.enforce(this, Limits::Key::lines);
                *out = first;
                return true;
            } while (false);
        }

        /* We need to allocate a new memory buffer. */
        Node *next_node = createPage(CreatePage(cap));
        if (!next_node) return false;
        /* we don't errdefer this because we've added it to the linked
         * list and its fine to have dangling unused pages. */
        pages.append(next_node);
        Page *page = next_node->page();
        page->size.rows = 1;

        /* We should never be more than our max size here because we've
         * verified the case above. */
        page->assertIntegrity();

        /* Record the increased row count */
        total_rows += 1;

        /* Appending a page can cross the line limit and can make the oldest
         * active-boundary page wholly historical. */
        limits.enforce(this, Limits::Key::lines);
        *out = next_node;
        return true;
    }
    /* ------------------------------------------------------------------ */
    /* Compression                                                          */

    /* Temporary output memory used while creating a compressed page.
     *
     * Standard-sized output borrows a page-pool item so repeated compression can
     * reuse the same virtual mapping. Oversized pages use a temporary allocation
     * from the page allocator and release it immediately after compression.
     *
     * A borrowed item goes back to the pool through zero-mode decommit, which
     * only has to clear the bytes the encoder wrote. Callers therefore pass the
     * dirty length to `deinit` rather than paying to clear the whole item. */
    struct CompressionScratch {
        enum class Tag { pooled, allocated } tag;
        PageItem *pooled;
        uint8_t *allocated;
        size_t allocated_len;

        static bool init(MemoryPool *pool, size_t required, size_t raw_len, CompressionScratch *out) {
            assert(required <= raw_len);

            if (required <= std_size) {
                PageItem *memory = pool->pages.create();
                if (!memory) return false;
                mem::recommit(memory->bytes, std_size);
                out->tag = Tag::pooled;
                out->pooled = memory;
                return true;
            }

            const zigstd::Allocator page_alloc = pool->pages.allocator;
            uint8_t *m = page_alloc.alignedAlloc(raw_len, page_size_min);
            if (!m) return false;
            out->tag = Tag::allocated;
            out->allocated = m;
            out->allocated_len = raw_len;
            return true;
        }

        uint8_t *bytes() { return tag == Tag::pooled ? pooled->bytes : allocated; }

        void deinit(MemoryPool *pool, size_t dirty_len) {
            switch (tag) {
            case Tag::pooled:
                (void)mem::decommit(mem::DecommitMode::zero, pooled->bytes, std_size, dirty_len);
                pool->pages.destroy(pooled);
                break;
            case Tag::allocated: {
                const zigstd::Allocator page_alloc = pool->pages.allocator;
                page_alloc.free(allocated, allocated_len, page_size_min);
                break;
            }
            }
        }
    };

    /* Result of one incremental compression step. */
    enum class IncrementalCompressionResult {
        /* Strict retained-mapping reclamation is unavailable on this target. */
        unsupported,

        /* More cold pages or a verification pass remain after this invocation's
         * candidate-bounded work. */
        pending,

        /* A complete verification pass compressed zero pages. */
        complete,
    };

    /* Iterate complete historical pages which do not intersect the viewport.
     *
     * All boundaries come from PageList pins and Page metadata. Advancing this
     * iterator never restores a compressed page or reads its backing memory. */
    struct CompressionIterator {
        Node *current;
        Node *active;
        Node *viewport_first;
        Node *viewport_last;

        static CompressionIterator init(const PageList *self) {
            CompressionIterator it;
            it.current = self->pages.first;
            it.active = self->getTopLeft(point::Tag::active).node;
            it.viewport_first = self->getTopLeft(point::Tag::viewport).node;
            it.viewport_last = self->getBottomRight(point::Tag::viewport).value.node;
            return it;
        }

        Node *next() {
            while (current != active) {
                /* The viewport is a contiguous node range. Once traversal reaches
                 * its first node, advance through the complete visible range and
                 * resume at the next offscreen page. */
                if (current == viewport_first) {
                    while (current != active && current != viewport_last) {
                        current = current->next;
                    }

                    if (current == active) return nullptr;
                    current = current->next;
                    continue;
                }

                Node *node = current;
                current = node->next;
                return node;
            }

            return nullptr;
        }

        bool done() const { return current == active; }
    };

    /* Bound candidate inspection independently from compression work. Skipping
     * an already-compressed page is cheap, but still counts toward this limit. */
    static const size_t incremental_compression_max_inspected = 8;

    /* Failure injection for the final reclamation step. The operating-system
     * failure path cannot otherwise be exercised by tests because terminal_mem
     * deliberately simulates successful decommit in test builds. */
    enum class CompressPageTw { decommit };
    enum class CompressPageTwError { none, DecommitFailed };
    typedef tripwire::Module<CompressPageTw, CompressPageTwError, 1> compressPage_tw;

    enum class CompressMode { incremental, drain, full };

    /* Compress eligible nodes, saving a significant amount of memory.
     *
     * Eligible nodes are complete pages before the active boundary which do not
     * intersect the viewport. The boundary page is excluded because it may
     * contain both scrollback and active rows; visible pages remain resident for
     * immediate redraw and scrolling.
     *
     * Compression requires a system that supports reclaiming physical memory for
     * virtual allocations while retaining their address ranges.
     *
     * Compression is SLOW (relatively), so incremental compression during idle
     * periods is recommended. Incremental mode performs one bounded step and its
     * result specifies whether to continue immediately. Drain mode performs
     * incremental steps until the pass and its verification pass finish. Full
     * mode visits every currently eligible node once without using incremental
     * state.
     *
     * PageList tracks mutations which require a later incremental pass in its
     * compression state. On supported targets, full compression returns
     * `complete`, indicating that it has no continuation to schedule rather than
     * that every page was compressed. */
    IncrementalCompressionResult compress(CompressMode mode) {
        switch (mode) {
        case CompressMode::incremental: return compressIncremental();
        case CompressMode::drain:
            for (;;) {
                switch (compressIncremental()) {
                case IncrementalCompressionResult::pending: continue;
                case IncrementalCompressionResult::complete: return IncrementalCompressionResult::complete;
                case IncrementalCompressionResult::unsupported: return IncrementalCompressionResult::unsupported;
                }
            }
        default:
            /* Match incremental mode's unsupported result. Full compression
             * has no useful work to perform without strict reclamation. */
            if (!mem::canReclaim(mem::DecommitMode::strict)) {
                page_compression.reset();
                return IncrementalCompressionResult::unsupported;
            }

            compressFull();

            /* Full compression has no continuation. Discard any partial
             * incremental cursor so later activity starts at the oldest page. */
            page_compression.reset();

            return IncrementalCompressionResult::complete;
        }
    }

    /* Perform one candidate-bounded incremental cold-history compression step. */
    IncrementalCompressionResult compressIncremental() {
        IncrementalCompressionState *state = &page_compression;

        /* If we can't reclaim virtual memory, compression is unsupported. */
        if (!mem::canReclaim(mem::DecommitMode::strict)) {
            state->reset();
            return IncrementalCompressionResult::unsupported;
        }

        /* Find the node following the exact continuation marker within the cold
         * prefix. A missing marker means the list changed between steps, so begin
         * again at the current first page. This lookup does not touch page memory. */
        CompressionIterator it = CompressionIterator::init(this);
        if (state->last_serial.has) {
            const uint64_t last_serial = state->last_serial.value;
            bool found = false;
            while (Node *node = it.next()) {
                /* A newly allocated or replacement node appeared before the
                 * marker. Restart immediately so that node cannot be skipped. */
                if (node->serial >= state->next_serial) break;

                /* Not a match? Keep looking */
                if (node->serial != last_serial) continue;

                /* Match! The iterator already points to the next offscreen node. */
                found = true;
                break;
            }

            if (!found) {
                /* Not found or otherwise invalid. Reset */
                state->last_serial = Maybe<uint64_t>::none();
                it = CompressionIterator::init(this);
            }
        }

        /* Keep track of our next_serial */
        state->next_serial = page_serial;

        /* We cap the number of pages we look at to do our best to
         * time-bound the incremental compression. */
        size_t inspected_pages = 0;
        while (inspected_pages < incremental_compression_max_inspected) {
            Node *node = it.next();
            if (!node) break;

            state->last_serial = node->serial;
            inspected_pages += 1;

            /* If this page is already compressed, ignore it. */
            if (node->isCompressed()) continue;

            /* Compression is substantially more expensive even if it fails.
             * So we just try it. */
            if (compressPage(node)) state->flags.did_compress = true;
            break;
        }

        /* If we didn't reach our active node, then we still have work to do. */
        if (!it.done()) return IncrementalCompressionResult::pending;

        /* We reached our active node. So we're done, except that we always
         * do one pass after the first success so we can recompress nodes that
         * were possibly decompressed (e.g. by search, inspector, whatever). */
        if (!state->flags.verifying || state->flags.did_compress) {
            const uint64_t activity_serial = state->activity_serial;
            *state = IncrementalCompressionState();
            state->flags.verifying = true;
            state->activity_serial = activity_serial;
            return IncrementalCompressionResult::pending;
        }

        /* Leave the state fresh while idle so later activity naturally begins at
         * the oldest cold page, including pages restored after this pass. */
        state->reset();
        return IncrementalCompressionResult::complete;
    }

    /* Compress every fully historical resident page which is currently cold. */
    void compressFull() {
        CompressionIterator it = CompressionIterator::init(this);
        while (Node *node = it.next()) {
            /* Don't restore an already-compressed page just to recompress it. */
            if (node->isCompressed()) continue;

            /* Failure leaves this node resident and unchanged. */
            (void)compressPage(node);
        }
    }

    /* Attempt to compress one resident page while retaining its raw mapping.
     *
     * Compression is opportunistic: every failure leaves the page resident and
     * usable. Candidate selection and retry policy belong to `compress`; this
     * primitive only performs one state transition. */
    bool compressPage(Node *node) {
        /* Recompression requires first restoring the raw page and is a policy
         * decision, so this primitive only accepts resident nodes. */
        if (node->isCompressed()) return false;

        Page *page = node->page();

        /* The scratch size is capped just below the representation's break-even
         * point. Codec limits and pages too small to cover the compressed-state
         * overhead simply make this page ineligible for compression. */
        size_t required;
        if (compress::Page::requiredScratch(page->memory_len, &required) != lz4::CompressError::none) return false;
        if (required == 0) return false;

        /* Build the compressed candidate without changing the node. This scope is
         * intentional: its defer releases the borrowed or temporary scratch before
         * we attempt to discard the source mapping below. Only the candidate's
         * exact-sized encoded allocation survives the scope. */
        compress::Page compressed;
        {
            CompressionScratch scratch;
            if (!CompressionScratch::init(&pool, required, page->memory_len, &scratch)) return false;

            /* The encoder writes at most `required` bytes and reports exactly
             * how many on success. Track that so returning the scratch only
             * clears the prefix it dirtied instead of the whole item. */
            size_t dirty_len = required;

            static lz4::HashTable table;
            const compress::Page::InitResult r = compress::Page::init(pool.alloc, page, scratch.bytes(), required, table, &compressed);
            if (r == compress::Page::InitResult::ok) dirty_len = compressed.encoded_len;
            scratch.deinit(&pool, dirty_len);
            /* Null means compression crossed the break-even point. The node and its
             * resident mapping are still untouched in this case. */
            if (r != compress::Page::InitResult::ok) return false;
        }

        /* Strict decommit is the final fallible step. It either discards the whole
         * raw mapping or leaves it untouched, so failure can safely free the
         * candidate and preserve the resident node exactly as it was. */
        const bool decommit_allowed = compressPage_tw::check(CompressPageTw::decommit) == CompressPageTwError::none;
        if (!decommit_allowed || !mem::decommit(mem::DecommitMode::strict, compressed.page.memory,
                                                compressed.page.memory_len, compressed.page.memory_len)) {
            compressed.deinit();
            return false;
        }

        /* Publish the new state only after both the encoded allocation and the
         * retained-mapping decommit have succeeded. */
        node->data.tag = Node::Data::Tag::compressed;
        node->data.compressed = compressed;
        return true;
    }

    /* Destroy the memory of the given node in the PageList linked list
     * and return it to the pool. The node is assumed to already be removed
     * from the linked list.
     *
     * IMPORTANT: This function does NOT update `total_rows`. The caller is
     * responsible for accounting for the removed rows. This function only
     * updates `page_size` (byte accounting), not row accounting. */
    void destroyNode(Node *node) { destroyNodeExt(&pool, node, &page_size); }

    static void destroyNodeExt(MemoryPool *pool, Node *node, size_t *total_size) {
        Page *page = node->restore(Node::RestoreMode::discard);

        /* Update our accounting for page size. This must mirror what was
         * added at creation time: a pool-owned page always accounts for a
         * full pool item even if its layout is smaller, while a heap-owned
         * page accounts for its exact memory length. */
        if (total_size) *total_size -= node->owned == Node::Owned::pool ? PagePool::item_size : page->memory_len;

        switch (node->owned) {
        case Node::Owned::pool: {
            assert(page->memory_len <= std_size);

            /* Reset the memory to zero (and decommit it, where
             * supported) so it can be reused. */
            PageItem *item = (PageItem *)page->memory;
            (void)mem::decommit(mem::DecommitMode::zero, item->bytes, std_size, page->memory_len);
            pool->pages.destroy(item);
            break;
        }

        case Node::Owned::heap: {
            const zigstd::Allocator page_alloc = pool->pages.allocator;
            page_alloc.free(page->memory, page->memory_len, page_size_min);
            break;
        }
        }

        pool->nodes.destroy(node);
    }

    /* Clone the given source row into the row at `dst_y` of the given
     * node's page, increasing the node's capacity as necessary to fit the
     * source row's managed memory (styles, hyperlinks, etc.).
     *
     * Since increasing capacity replaces the node in the page list, the
     * (possibly replaced) node is returned and the caller must use it in
     * place of the old node. The source must NOT be on the given node
     * since the node's page memory may be freed on capacity increase. */
    Node *cloneRowGrowCapacity(Node *node, size_t dst_y, Page *src_page, const page::Row *src_row) {
        assert(src_page != node->page());

        Node *current = node;
        for (;;) {
            Page *cur_page = current->page();
            page::Row *cur_rows = cur_page->rows.ptr(cur_page->memory);
            const page::PageError err = cur_page->cloneRowFrom(src_page, &cur_rows[dst_y], src_row);
            if (err != page::PageError::none) {
                /* Adjust our page capacity to make room for what we
                 * didn't have space for. */
                Node *n;
                const IncreaseCapacityError e = increaseCapacity(current, forCloneError(err), &n);
                switch (e) {
                /* We can't gracefully recover from either of these
                 * here: our callers have already rotated rows, so
                 * returning an error would leave the page list
                 * half-mutated (and corrupt), so a crash is better. */
                case IncreaseCapacityError::OutOfMemory:
                    fprintf(stderr, "increaseCapacity system allocator OOM\n");
                    abort();
                case IncreaseCapacityError::OutOfSpace:
                    fprintf(stderr, "increaseCapacity OutOfSpace\n");
                    abort();
                default: break;
                }
                current = n;

                /* Retry the row copy with the increased capacity. */
                continue;
            }

            return current;
        }
    }
    /* ------------------------------------------------------------------ */
    /* Erase                                                                */

    /* Wisp: `fastmem.rotateOnce(Row, rows[a..b])` */
    static void rotateRows(page::Row *rows, size_t a, size_t b) { fastmem::rotateOnce<page::Row>(rows + a, b - a); }

    /* Fast-path function to erase exactly 1 row. Erasing means that the row
     * is completely REMOVED, not just cleared. All rows following the removed
     * row will be shifted up by 1 to fill the empty space.
     *
     * Unlike eraseRows, eraseRow does not change the size of any pages. The
     * caller is responsible for adjusting the row count of the final page if
     * that behavior is required.
     * Wisp: upstream's `!void` has no reachable errors. */
    void eraseRow(const point::Point &pt) {
        struct Guard {
            const PageList *l;
            ~Guard() { l->assertIntegrity(); }
        } guard = {this};
        const Pin pn = pin(pt).value;

        Node *node = pn.node;
        Page *page = node->page();
        page::Row *prows = page->rows.ptr(page->memory);

        /* Erasing history may restore compressed pages. Mark unconditionally
         * because incrementing the activity token is cheaper than locating the
         * active boundary. */
        page_compression.markActivity();

        /* In order to move the following rows up we rotate the rows array by 1.
         * The rotate operation turns e.g. [ 0 1 2 3 ] in to [ 1 2 3 0 ], which
         * works perfectly to move all of our elements where they belong.
         * Rotating rows changes which logical row cached coordinates identify. */
        invalidateNodeLayout(node);
        rotateRows(prows, pn.y, node->rows());

        /* We adjust the tracked pins in this page, moving up any that were below
         * the removed row. */
        for (size_t i = 0; i < tracked_pins.count(); i++) {
            Pin *p = tracked_pins.keys()[i];
            if (p->node == node && p->y > pn.y) p->y -= 1;
        }

        /* If we have a pinned viewport, we need to adjust for active area. */
        fixupViewport(1);

        /* Mark the whole page as dirty.
         *
         * Technically we only need to mark rows from the erased row to the end
         * of the page as dirty, but that's slower and this is a hot function. */
        page->dirty = true;

        /* We iterate through all of the following pages in order to move their
         * rows up by 1 as well. */
        while (Node *next = node->next) {
            Page *next_page = next->page();
            page::Row *next_rows = next_page->rows.ptr(next_page->memory);

            /* We take the top row of the page and clone it in to the bottom
             * row of the previous page, which gets rid of the top row that was
             * rotated down in the previous page, and accounts for the row in
             * this page that will be rotated down as well.
             *
             * The copy may replace the destination node in order to
             * increase its capacity. We can discard the replacement
             * because we advance to the next node below, and the
             * replacement is already linked in its place (so e.g. the
             * `node.prev` access in the pin fixups below is correct). */
            (void)cloneRowGrowCapacity(node, (size_t)node->rows() - 1, next_page, &next_rows[0]);

            node = next;
            page = next_page;
            prows = next_rows;

            /* Rotating this page moves every cached row coordinate up by one. */
            invalidateNodeLayout(node);
            rotateRows(prows, 0, node->rows());

            /* Mark the whole page as dirty. */
            page->dirty = true;

            /* Our tracked pins for this page need to be updated.
             * If the pin is in row 0 that means the corresponding row has
             * been moved to the previous page. Otherwise, move it up by 1. */
            for (size_t i = 0; i < tracked_pins.count(); i++) {
                Pin *p = tracked_pins.keys()[i];
                if (p->node != node) continue;
                if (p->y == 0) {
                    p->node = node->prev;
                    p->y = (size::CellCountInt)(p->node->rows() - 1);
                    continue;
                }
                p->y -= 1;
            }
        }

        /* Reset the final row which was rotated from the top of the page.
         * A full reset (not just clearing cells) so no metadata from the
         * erased row is retained by the new blank row. */
        page->resetRow(&prows[node->rows() - 1]);
    }

    /* A variant of eraseRow that shifts only a bounded number of following
     * rows up, filling the space they leave behind with blank rows.
     *
     * `limit` is exclusive of the erased row. A limit of 1 will erase the target
     * row and shift the row below in to its position, leaving a blank row below. */
    void eraseRowBounded(const point::Point &pt, size_t limit) {
        struct Guard {
            const PageList *l;
            ~Guard() { l->assertIntegrity(); }
        } guard = {this};

        /* This function has a lot of repeated code in it because it is a hot path.
         *
         * To get a better idea of what's happening, read eraseRow first for more
         * in-depth explanatory comments. To avoid repetition, the only comments for
         * this function are for where it differs from eraseRow. */

        const Pin pn = pin(pt).value;

        Node *node = pn.node;
        Page *page = node->page();
        page::Row *prows = page->rows.ptr(page->memory);

        /* Erasing history may restore compressed pages. Mark unconditionally
         * because incrementing the activity token is cheaper than locating the
         * active boundary. */
        page_compression.markActivity();

        /* If the row limit is less than the remaining rows before the end of the
         * page, then we clear the row, rotate it to the end of the boundary limit
         * and update our pins. */
        if ((size_t)node->rows() - pn.y > limit) {
            /* Rotating this bounded region changes its cached row coordinates. */
            invalidateNodeLayout(node);
            page->resetRow(&prows[pn.y]);
            rotateRows(prows, pn.y, pn.y + limit + 1);

            /* Mark the whole page as dirty.
             *
             * Technically we only need to mark from the erased row to the
             * limit but this is a hot function, so we want to minimize work. */
            page->dirty = true;

            /* If our viewport is a pin and our pin is within the erased
             * region we need to maybe shift our cache up. We do this here instead
             * of in the pin loop below because its unlikely to be true and we
             * don't want to run the conditional N times. */
            if (viewport == Viewport::pin && viewport_pin_row_offset.has) {
                const Pin *p = viewport_pin;
                if (!(p->node != node || p->y < pn.y || p->y > pn.y + limit || p->y == 0))
                    viewport_pin_row_offset.value -= 1;
            }

            /* Update pins in the shifted region. */
            for (size_t i = 0; i < tracked_pins.count(); i++) {
                Pin *p = tracked_pins.keys()[i];
                if (p->node == node && p->y >= pn.y && p->y <= pn.y + limit) {
                    if (p->y == 0) {
                        p->x = 0;
                    } else {
                        p->y -= 1;
                    }
                }
            }

            return;
        }

        /* Rotating this suffix changes which logical row its coordinates identify. */
        invalidateNodeLayout(node);
        rotateRows(prows, pn.y, node->rows());

        /* Mark the whole page as dirty.
         *
         * Technically we only need to mark rows from the erased row to the end
         * of the page as dirty, but that's slower and this is a hot function. */
        page->dirty = true;

        /* We need to keep track of how many rows we've shifted so that we can
         * determine at what point we need to do a partial shift on subsequent
         * pages. */
        size_t shifted = (size_t)node->rows() - pn.y;

        /* Update tracked pins. */
        {
            /* See the other places we do something similar in this function
             * for a detailed explanation. */
            if (viewport == Viewport::pin && viewport_pin_row_offset.has) {
                const Pin *p = viewport_pin;
                if (!(p->node != node || p->y < pn.y || p->y == 0)) viewport_pin_row_offset.value -= 1;
            }

            for (size_t i = 0; i < tracked_pins.count(); i++) {
                Pin *p = tracked_pins.keys()[i];
                if (p->node == node && p->y >= pn.y) {
                    if (p->y == 0) {
                        p->x = 0;
                    } else {
                        p->y -= 1;
                    }
                }
            }
        }

        while (Node *next = node->next) {
            Page *next_page = next->page();
            page::Row *next_rows = next_page->rows.ptr(next_page->memory);

            /* The copy may replace the destination node in order to
             * increase its capacity. We can discard the replacement
             * because we advance to the next node below, and the
             * replacement is already linked in its place (so e.g. the
             * `node.prev` access in the pin fixups below is correct). */
            (void)cloneRowGrowCapacity(node, (size_t)node->rows() - 1, next_page, &next_rows[0]);

            node = next;
            page = next_page;
            prows = next_rows;

            /* We check to see if this page contains enough rows to satisfy the
             * specified limit, accounting for rows we've already shifted in prior
             * pages.
             *
             * The logic here is very similar to the one before the loop. */
            const size_t shifted_limit = limit - shifted;
            if (node->rows() > shifted_limit) {
                /* Rotating this bounded prefix changes its cached row coordinates. */
                invalidateNodeLayout(node);
                page->resetRow(&prows[0]);
                rotateRows(prows, 0, shifted_limit + 1);

                /* Mark the whole page as dirty.
                 *
                 * Technically we only need to mark from the erased row to the
                 * limit but this is a hot function, so we want to minimize work. */
                page->dirty = true;

                /* See the other places we do something similar in this function
                 * for a detailed explanation. */
                if (viewport == Viewport::pin && viewport_pin_row_offset.has) {
                    const Pin *p = viewport_pin;
                    if (!(p->node != node || p->y > shifted_limit)) viewport_pin_row_offset.value -= 1;
                }

                /* Update pins in the shifted region. */
                for (size_t i = 0; i < tracked_pins.count(); i++) {
                    Pin *p = tracked_pins.keys()[i];
                    if (p->node != node || p->y > shifted_limit) continue;
                    if (p->y == 0) {
                        p->node = node->prev;
                        p->y = (size::CellCountInt)(p->node->rows() - 1);
                        continue;
                    }
                    p->y -= 1;
                }

                return;
            }

            /* Rotating the whole page moves every cached row coordinate up by one. */
            invalidateNodeLayout(node);
            rotateRows(prows, 0, node->rows());

            /* Mark the whole page as dirty. */
            page->dirty = true;

            /* Account for the rows shifted in this node. */
            shifted += node->rows();

            /* See the other places we do something similar in this function
             * for a detailed explanation. */
            if (viewport == Viewport::pin && viewport_pin_row_offset.has) {
                if (viewport_pin->node == node) viewport_pin_row_offset.value -= 1;
            }

            /* Update tracked pins. */
            for (size_t i = 0; i < tracked_pins.count(); i++) {
                Pin *p = tracked_pins.keys()[i];
                if (p->node != node) continue;
                if (p->y == 0) {
                    p->node = node->prev;
                    p->y = (size::CellCountInt)(p->node->rows() - 1);
                    continue;
                }
                p->y -= 1;
            }
        }

        /* We reached the end of the page list before the limit, so we reset
         * the final row since it was rotated down from the top of this page. */
        page->resetRow(&prows[node->rows() - 1]);
    }

    /* Erase all history rows, optionally up to a bottom-left bound.
     * This always starts from the beginning of the history area. */
    void eraseHistory(Maybe<point::Point> bl_pt) { eraseRows(point::Point::history(), bl_pt); }

    /* Erase active area rows, from the top of the active area to the
     * given row (inclusive). */
    void eraseActive(size::CellCountInt y) {
        assert(y < rows);
        eraseRows(point::Point::active(), point::Point::active(0, y));
    }

    /* Erase rows from tl_pt to bl_pt (inclusive), physically removing
     * them rather than just clearing their contents. If a point falls
     * in the middle of a page, remaining rows in that page are shifted
     * and the page becomes underutilized (size < capacity).
     *
     * Callers must ensure that the erased range only removes pages from
     * the front or back of the linked list, never the middle. The pin and row
     * accounting in this operation is only defined for those boundary ranges.
     * Use the public eraseHistory/eraseActive wrappers which enforce this. */
    void eraseRows(const point::Point &tl_pt, Maybe<point::Point> bl_pt) {
        struct Guard {
            const PageList *l;
            ~Guard() { l->assertIntegrity(); }
        } guard = {this};
        page_compression.markActivity();

        /* The count of rows that was erased. */
        size_t erased = 0;

        /* A pageIterator iterates one page at a time from the back forward.
         * "back" here is in terms of scrollback, but actually the front of the
         * linked list. */
        PageIterator it = pageIterator(Direction::right_down, tl_pt, bl_pt);
        Chunk chunk;
        while (it.next(&chunk)) {
            /* If the chunk is a full page, deinit thit page and remove it from
             * the linked list. */
            if (chunk.fullPage()) {
                /* A rare special case is that we're deleting everything
                 * in our linked list. erasePage requires at least one other
                 * page so to handle this we reinit this page, set it to zero
                 * size which will let us grow our active area back. */
                if (chunk.node->next == nullptr && chunk.node->prev == nullptr) {
                    /* Reinitializing the sole page invalidates every coordinate in it. */
                    invalidateNodeLayout(chunk.node);
                    Page *page = chunk.node->page();
                    erased += page->size.rows;
                    page->reinit();
                    page->size.rows = 0;
                    break;
                }

                erased += chunk.node->rows();
                erasePage(chunk.node);
                continue;
            }

            /* Moving the retained suffix changes every captured row coordinate. */
            invalidateNodeLayout(chunk.node);

            /* We are modifying our chunk so make sure it is in a good state. */
            Page *page = chunk.node->page();

            /* The chunk is not a full page so we need to move the rows.
             * This is a cheap operation because we're just moving cell offsets,
             * not the actual cell contents. */
            assert(chunk.start == 0);
            page::Row *prows = page->rows.ptr(page->memory);
            const size_t scroll_amount = (size_t)chunk.node->rows() - chunk.end;
            for (size_t i = 0; i < scroll_amount; i++) {
                page::Row *src = &prows[i + chunk.end];
                page::Row *dst = &prows[i];
                const page::Row old_dst = *dst;
                *dst = *src;
                *src = old_dst;

                /* Mark the moved row as dirty. */
                dst->setDirty(true);
            }

            /* Reset our remaining rows that we didn't shift or swapped.
             * These are retired into unused page capacity, which the
             * grow() fast path re-exposes without any clearing, so they
             * must be left in the default state. */
            for (size_t i = scroll_amount; i < chunk.node->rows(); i++) {
                page->resetRow(&prows[i]);
            }

            /* Update any tracked pins to shift their y. If it was in the erased
             * row then we move it to the top of this page. */
            for (size_t i = 0; i < tracked_pins.count(); i++) {
                Pin *p = tracked_pins.keys()[i];
                if (p->node != chunk.node) continue;
                if (p->y >= chunk.end) {
                    p->y -= (size::CellCountInt)chunk.end;
                } else {
                    p->y = 0;
                    p->x = 0;
                }
            }

            /* Our new size is the amount we scrolled */
            page->size.rows = (size::CellCountInt)scroll_amount;
            erased += chunk.end;
            page->assertIntegrity();
        }

        /* Update our total row count */
        total_rows -= erased;

        /* If we deleted active, we need to regrow because one of our invariants
         * is that we always have full active space. */
        if (tl_pt.tag == point::Tag::active) {
            for (size_t i = 0; i < erased; i++) {
                Node *g;
                if (!grow(&g)) {
                    /* If this fails its a pretty big issue actually... but I don't
                     * want to turn this function into an error-returning function
                     * because erasing active is so rare and even if it happens failing
                     * is even more rare...
                     * log.err("failed to regrow active area after erase err={}") */
                    return;
                }
            }
        }

        /* If we have a pinned viewport, we need to adjust for active area. */
        fixupViewport(erased);
    }

    /* Erase a single page, freeing all its resources. The page must be
     * at the front or back of the linked list (not the middle) and must
     * NOT be the final page in the entire list (i.e. must not make the
     * list empty).
     *
     * IMPORTANT: This function does NOT update `total_rows`. The caller is
     * responsible for accounting for the removed rows before or after calling
     * this function. */
    void erasePage(Node *node) {
        /* Must not be the final page. */
        assert(node->next != nullptr || node->prev != nullptr);

        /* We only support erasing from the front or back, never the middle. The
         * public erase operations maintain this contract by construction. */
        assert(node->prev == nullptr || node->next == nullptr);

        /* Update any tracked pins to move to the previous or next page. */
        for (size_t i = 0; i < tracked_pins.count(); i++) {
            Pin *p = tracked_pins.keys()[i];
            if (p->node != node) continue;
            p->node = node->prev ? node->prev : node->next;
            p->y = 0;
            p->x = 0;

            /* This doesn't get marked garbage because the tracked pin
             * movement is sensical. */
        }

        /* Remove the page from the linked list */
        pages.remove(node);
        destroyNode(node);
    }
    /* ------------------------------------------------------------------ */
    /* Pins and points                                                      */

    /* Returns the pin for the given point. The pin is NOT tracked so it
     * is only valid as long as the pagelist isn't modified.
     *
     * This will return null if the point is out of bounds. The caller
     * should clamp the point to the bounds of the coordinate space if
     * necessary. */
    Maybe<Pin> pin(const point::Point &pt) const {
        /* getTopLeft is much more expensive than checking the cols bounds
         * so we do this first. */
        const size::CellCountInt x = pt.coord().x;
        if (x >= cols) return Maybe<Pin>::none();

        /* Grab the top left and move to the point. */
        const Maybe<Pin> d = getTopLeft(pt.tag).down(pt.coord().y);
        if (!d.has) return Maybe<Pin>::none();
        Pin p = d.value;
        /* Incomplete reflow can leave a page narrower than the desired width.
         * Never manufacture an out-of-bounds pin for that page. */
        if (x >= p.node->cols()) return Maybe<Pin>::none();
        p.x = x;
        return p;
    }

    /* Convert the given pin to a tracked pin. A tracked pin will always be
     * automatically updated as the pagelist is modified. If the point the
     * pin points to is removed completely, the tracked pin will be updated
     * to the top-left of the screen. Wisp: null is OutOfMemory. */
    Pin *trackPin(const Pin &p) {
        if (slow_runtime_safety) assert(pinIsValid(p));

        /* Create our tracked pin */
        Pin *tracked = pool.pins.create();
        if (!tracked) return nullptr;
        *tracked = p;

        /* Add it to the tracked list */
        if (!tracked_pins.putNoClobber(tracked)) {
            pool.pins.destroy(tracked);
            return nullptr;
        }

        return tracked;
    }

    /* Untrack a previously tracked pin. This will deallocate the pin. */
    void untrackPin(Pin *p) {
        assert(p != viewport_pin);
        if (tracked_pins.swapRemove(p)) {
            pool.pins.destroy(p);
        }
    }

    size_t countTrackedPins() const { return tracked_pins.count(); }

    /* Returns the tracked pins for this pagelist. The slice is owned by the
     * pagelist and is only valid until the pagelist is modified. */
    Pin *const *trackedPins(size_t *len) const {
        *len = tracked_pins.count();
        return tracked_pins.keys();
    }

    /* Checks if a pin is valid for this pagelist. This is a very slow and
     * expensive operation since we traverse the entire linked list in the
     * worst case. Only for runtime safety/debug. */
    bool pinIsValid(const Pin &p) const {
        for (const Node *node = pages.first; node; node = node->next) {
            if (node != p.node) continue;
            return p.y < node->rows() && p.x < node->cols();
        }

        return false;
    }

    /* Returns whether a node pointer and serial still identify the same page in
     * this list. The node pointer is only compared and is safe even if the node
     * has been destroyed or reused since the serial was captured. */
    bool nodeIsValid(const Node *target, uint64_t serial) const {
        /* Reset invalidates the whole prior epoch, so reject those generations
         * without scanning the live list. */
        if (serial < page_serial_epoch) return false;

        for (const Node *node = pages.first; node; node = node->next) {
            if (node == target) return node->serial == serial;
        }

        return false;
    }

    /* Returns the viewport for the given pin, preferring to pin to
     * "active" if the pin is within the active area. */
    bool pinIsActive(const Pin &p) const {
        /* If the pin is in the active page, then we can quickly determine
         * if we're beyond the end. */
        const Pin active = getTopLeft(point::Tag::active);
        if (p.node == active.node) return p.y >= active.y;

        for (Node *node = active.node->next; node; node = node->next) {
            /* This loop is pretty fast because the active area is
             * never that large so this is at most one, two nodes for
             * reasonable terminals (including very large real world
             * ones).
             *
             * A node forward in the active area is our node, so we're
             * definitely in the active area. */
            if (node == p.node) return true;
        }

        return false;
    }

    /* Returns true if the pin is at the top of the scrollback area. */
    bool pinIsTop(const Pin &p) const { return p.y == 0 && p.node == pages.first; }

    /* Convert a pin to a point in the given context. If the pin can't fit
     * within the given tag (i.e. its in the history but you requested active),
     * then this will return null.
     *
     * Note that this can be a very expensive operation depending on the tag and
     * the location of the pin. This works by traversing the linked list of pages
     * in the tagged region.
     *
     * Therefore, this is recommended only very rarely. */
    Maybe<point::Point> pointFromPin(point::Tag tag, const Pin &p) const {
        const Pin tl = getTopLeft(tag);

        /* Count our first page which is special because it may be partial. */
        point::Coordinate coord(p.x, 0);
        if (p.node == tl.node) {
            /* If our top-left is after our y then we're outside the range. */
            if (tl.y > p.y) return Maybe<point::Point>::none();
            coord.y = (uint32_t)(p.y - tl.y);
        } else {
            uint64_t y = (uint64_t)coord.y + (tl.node->rows() - tl.y);
            if (y > 0xFFFFFFFFu) return Maybe<point::Point>::none();
            bool found = false;
            for (Node *node = tl.node->next; node; node = node->next) {
                if (node == p.node) {
                    y += p.y;
                    if (y > 0xFFFFFFFFu) return Maybe<point::Point>::none();
                    found = true;
                    break;
                }

                y += node->rows();
                if (y > 0xFFFFFFFFu) return Maybe<point::Point>::none();
            }
            /* We never saw our node, meaning we're outside the range. */
            if (!found) return Maybe<point::Point>::none();
            coord.y = (uint32_t)y;
        }

        return point::Point(tag, coord);
    }

    /* Get the cell at the given point, or null if the cell does not
     * exist or is out of bounds.
     *
     * Warning: this is slow and should not be used in performance critical paths */
    Maybe<Cell> getCell(const point::Point &pt) const;

    /* Log a debug diagram of the page list to the provided writer.
     * Wisp: false is error.TooManyTrackedPinsInRow. */
    bool diagram(std::string *writer) const;

    /* Wisp: highlight.Untracked (defined here so PageList doesn't depend
     * on highlight.hpp; highlight::Untracked aliases it). */
    struct HighlightUntracked {
        Pin start;
        Pin end;

        bool eql(const HighlightUntracked &other) const { return start.eql(other.start) && end.eql(other.end); }
    };

    /* Returns the boundaries of the given semantic content type for
     * the prompt at the given pin. The pin row MUST be the first row
     * of a prompt, otherwise the results may be nonsense.
     *
     * To get prompt pins, use promptIterator. Warning that if there are
     * no semantic prompts ever present, promptIterator will iterate the
     * entire PageList. Downstream callers should keep track of a flag if
     * they've ever seen semantic prompt operations to prevent this performance
     * case.
     *
     * Note that some semantic content type such as "input" is usually
     * nested within prompt boundaries, so the returned boundaries may include
     * prompt text. */
    Maybe<HighlightUntracked> highlightSemanticContent(const Pin &at, page::Cell::SemanticContent content) const;
    /* ------------------------------------------------------------------ */
    /* Iterator constructors, bounds, stats                                 */

    PromptIterator promptIterator(Direction direction, const point::Point &tl_pt,
                                  Maybe<point::Point> bl_pt = Maybe<point::Point>()) const {
        const Pin tl_pin = pin(tl_pt).value;
        Pin bl_pin;
        if (bl_pt.has) {
            bl_pin = pin(bl_pt.value).value;
        } else {
            const Maybe<Pin> br = getBottomRight(tl_pt.tag);
            if (!br.has) return PromptIterator::empty();
            bl_pin = br.value;
        }

        switch (direction) {
        case Direction::right_down: return tl_pin.promptIterator(Direction::right_down, bl_pin);
        default: return bl_pin.promptIterator(Direction::left_up, tl_pin);
        }
    }

    CellIterator cellIterator(Direction direction, const point::Point &tl_pt,
                              Maybe<point::Point> bl_pt = Maybe<point::Point>()) const {
        const Pin tl_pin = pin(tl_pt).value;
        Pin bl_pin;
        if (bl_pt.has) {
            bl_pin = pin(bl_pt.value).value;
        } else {
            const Maybe<Pin> br = getBottomRight(tl_pt.tag);
            if (!br.has) return CellIterator();
            bl_pin = br.value;
        }

        switch (direction) {
        case Direction::right_down: return tl_pin.cellIterator(Direction::right_down, bl_pin);
        default: return bl_pin.cellIterator(Direction::left_up, tl_pin);
        }
    }

    /* Create an iterator that can be used to iterate all the rows in
     * a region of the screen from the given top-left. The tag of the
     * top-left point will also determine the end of the iteration,
     * so convert from one reference point to another to change the
     * iteration bounds. */
    RowIterator rowIterator(Direction direction, const point::Point &tl_pt,
                            Maybe<point::Point> bl_pt = Maybe<point::Point>()) const {
        const Pin tl_pin = pin(tl_pt).value;
        Pin bl_pin;
        if (bl_pt.has) {
            bl_pin = pin(bl_pt.value).value;
        } else {
            const Maybe<Pin> br = getBottomRight(tl_pt.tag);
            if (!br.has) return RowIterator();
            bl_pin = br.value;
        }

        switch (direction) {
        case Direction::right_down: return tl_pin.rowIterator(Direction::right_down, bl_pin);
        default: return bl_pin.rowIterator(Direction::left_up, tl_pin);
        }
    }

    /* Return an iterator that iterates through the rows in the tagged area
     * of the point. The iterator returns row "chunks", which are the largest
     * contiguous set of rows in a single backing page for a given portion of
     * the point region.
     *
     * This is a more efficient way to iterate through the data in a region,
     * since you can do simple pointer math and so on.
     *
     * If bl_pt is non-null, iteration will stop at the bottom left point
     * (inclusive). If bl_pt is null, the entire region specified by the point
     * tag will be iterated over. tl_pt and bl_pt must be the same tag, and
     * bl_pt must be greater than or equal to tl_pt.
     *
     * If direction is left_up, iteration will go from bl_pt to tl_pt. If
     * direction is right_down, iteration will go from tl_pt to bl_pt.
     * Both inclusive. */
    PageIterator pageIterator(Direction direction, const point::Point &tl_pt,
                              Maybe<point::Point> bl_pt = Maybe<point::Point>()) const {
        const Pin tl_pin = pin(tl_pt).value;
        Pin bl_pin;
        if (bl_pt.has) {
            bl_pin = pin(bl_pt.value).value;
        } else {
            const Maybe<Pin> br = getBottomRight(tl_pt.tag);
            if (!br.has) return PageIterator();
            bl_pin = br.value;
        }

        if (slow_runtime_safety) {
            assert(tl_pin.eql(bl_pin) || tl_pin.before(bl_pin));
        }

        switch (direction) {
        case Direction::right_down: return tl_pin.pageIterator(Direction::right_down, bl_pin);
        default: return bl_pin.pageIterator(Direction::left_up, tl_pin);
        }
    }

    /* Get the top-left of the screen for the given tag. */
    Pin getTopLeft(point::Tag tag) const {
        switch (tag) {
        /* The full screen or history is always just the first page. */
        case point::Tag::screen:
        case point::Tag::history: return Pin(pages.first);

        case point::Tag::viewport:
            switch (viewport) {
            case Viewport::active: return getTopLeft(point::Tag::active);
            case Viewport::top: return getTopLeft(point::Tag::screen);
            default: return *viewport_pin;
            }

        /* The active area is calculated backwards from the last page.
         * This makes getting the active top left slower but makes scrolling
         * much faster because we don't need to update the top left. Under
         * heavy load this makes a measurable difference. */
        default: {
            size_t rem = rows;
            for (Node *node = pages.last; node; node = node->prev) {
                if (rem <= node->rows()) return Pin(node, (size::CellCountInt)(node->rows() - rem));

                rem -= node->rows();
            }

            assert(false); /* assertion: we always have enough rows for active */
            return Pin(pages.first);
        }
        }
    }
    Pin getTopLeft(const point::Point &pt) const { return getTopLeft(pt.tag); }

    /* Returns the bottom right of the screen for the given tag. This can
     * return null because it is possible that a tag is not in the screen
     * (e.g. history does not yet exist). */
    Maybe<Pin> getBottomRight(point::Tag tag) const {
        switch (tag) {
        case point::Tag::screen:
        case point::Tag::active: {
            Node *node = pages.last;
            return Pin(node, (size::CellCountInt)(node->rows() - 1), (size::CellCountInt)(node->cols() - 1));
        }

        case point::Tag::viewport: {
            Pin br = getTopLeft(point::Tag::viewport);
            br = br.down((size_t)rows - 1).value;
            br.x = (size::CellCountInt)(br.node->cols() - 1);
            return br;
        }

        default: {
            Pin br = getTopLeft(point::Tag::active);
            const Maybe<Pin> u = br.up(1);
            if (!u.has) return Maybe<Pin>::none();
            br = u.value;
            br.x = (size::CellCountInt)(br.node->cols() - 1);
            return br;
        }
        }
    }

    /* The total rows in the screen. This is the actual row count currently
     * and not a capacity or maximum.
     *
     * This is very slow, it traverses the full list of pages to count the
     * rows, so it is not pub. This is only used for testing/debugging. */
    size_t totalRows() const {
        size_t n = 0;
        for (const Node *node = pages.first; node; node = node->next) n += node->rows();
        return n;
    }

    /* The total number of pages in this list. This should only be used
     * for tests since it is O(N) over the list of pages. */
    size_t totalPages() const {
        size_t n = 0;
        for (const Node *node = pages.first; node; node = node->next) n += 1;
        return n;
    }

    /* Snapshot of the storage used by page nodes in this list.
     *
     * The raw byte counts describe page backing mappings only. They exclude
     * nodes, allocator metadata, unused preheated pool items, and the small
     * representation values stored in each node. A compressed page retains its
     * raw mapping as virtual address space, but its bytes are counted as
     * decommitted because strict reclamation succeeded before the state was
     * published. */
    struct MemoryStats {
        /* Pages whose raw backing mappings are resident. */
        size_t resident_pages = 0;

        /* Pages represented by encoded storage and a decommitted raw mapping. */
        size_t compressed_pages = 0;

        /* Logical bytes in every raw page mapping. */
        size_t raw_bytes = 0;

        /* Raw mapping bytes which remain resident. */
        size_t resident_raw_bytes = 0;

        /* Raw mapping bytes discarded for compressed pages. */
        size_t decommitted_raw_bytes = 0;

        /* Raw allocation bytes which remain physically resident.
         *
         * This can exceed `resident_raw_bytes` because a pool-owned page uses
         * only part of a standard pool item. Compressing such a page decommits
         * its initialized range, but the unused tail of the item stays resident. */
        size_t resident_backing_bytes = 0;

        /* Exact encoded allocations retained for compressed pages. */
        size_t encoded_bytes = 0;

        /* Estimate resident page backing storage after compression. */
        size_t estimatedResidentBytes() const { return resident_backing_bytes + encoded_bytes; }

        /* Estimate physical bytes avoided by compressed page backing storage. */
        size_t estimatedSavings() const {
            return decommitted_raw_bytes > encoded_bytes ? decommitted_raw_bytes - encoded_bytes : 0;
        }
    };

    /* Return a metadata-only snapshot of page backing storage.
     *
     * This never restores compressed pages. It is intended for diagnostics and
     * other infrequent reporting because it traverses the complete page list. */
    MemoryStats memoryStats() const {
        MemoryStats result;
        for (const Node *node = pages.first; node; node = node->next) {
            const size_t raw_len = node->metadata()->memory_len;
            const size_t backing_len = node->owned == Node::Owned::pool ? PagePool::item_size : raw_len;
            assert(backing_len >= raw_len);
            result.raw_bytes += raw_len;

            switch (node->data.tag) {
            case Node::Data::Tag::resident:
                result.resident_pages += 1;
                result.resident_raw_bytes += raw_len;
                result.resident_backing_bytes += backing_len;
                break;

            case Node::Data::Tag::compressed:
                result.compressed_pages += 1;
                result.decommitted_raw_bytes += raw_len;
                /* Strict reclamation covers only Page.memory. A standard pool
                 * item can have an unused tail which remains resident. */
                result.resident_backing_bytes += backing_len - raw_len;
                result.encoded_bytes += node->data.compressed.encoded_len;
                break;
            }
        }

        return result;
    }

    /* Grow the number of rows available in the page list by n.
     * This is only used for testing so it isn't optimized in any way. */
    bool growRows(size_t n) {
        for (size_t i = 0; i < n; i++) {
            Node *g;
            if (!grow(&g)) return false;
        }
        return true;
    }

    /* Clear all dirty bits on all pages. This is not efficient since it
     * traverses the entire list of pages. This is used for testing/debugging. */
    void clearDirty() {
        for (Node *p = pages.first; p; p = p->next) {
            Page *current_page = p->page();
            current_page->dirty = false;
            page::Row *prows = current_page->rows.ptr(current_page->memory);
            for (size_t i = 0; i < p->rows(); i++) prows[i].setDirty(false);
        }
    }

    /* Returns true if the point is dirty, used for testing. */
    bool isDirty(const point::Point &pt) const;

    /* Mark a point as dirty, used for testing. */
    void markDirty(const point::Point &pt) { pin(pt).value.markDirty(); }

    /* ------------------------------------------------------------------ */
    /* Builder / Cell                                                       */

    /* Build up a PageList manually from a set of Pages.
     *
     * This data structure is transactional: `deinit` releases every page
     * until `finish` is called. This keeps the ownership clear: a complete
     * PageList either owns all its pages or doesn't.
     *
     * This was specifically built to help facilitate snapshot decoding
     * which transfers pages directly, but could be generally useful
     * for other purposes as well. */
    struct Builder {
        MemoryPool pool;
        List pages;
        uint64_t page_serial; /* = 0 */
        size_t page_size;     /* = 0 */
        Options options;
        bool finished; /* = false */

        /* Initialize an empty builder. The options are the final state
         * of the PageList and some validation is done on the finish call
         * to ensure you built up a proper PageList according to those options. */
        static bool init(zigstd::Allocator alloc, const Options &options, Builder *out) {
            if (!MemoryPool::init(alloc, pageAllocator(alloc), page_preheat, &out->pool)) return false;
            out->pages = List();
            out->page_serial = 0;
            out->page_size = 0;
            out->options = options;
            out->finished = false;
            return true;
        }

        /* Release all pages when restoration does not finish.
         *
         * This is safe to call after `finish` succeeds, so callers can defer it
         * unconditionally. */
        void deinit() {
            if (finished) return;

            /* Free all our in-progress pages */
            while (Node *node = pages.popFirst()) destroyNodeExt(&pool, node, &page_size);
            /* Free memory pool */
            pool.deinit();
            finished = true; /* Wisp: self.* = undefined */
        }

        /* Allocate a new page into the PageList with the given capacity.
         *
         * The caller can then take this page and populate it. When `finish`
         * is called, ownership is transferred to the resulting PageList.
         * Until then, this Builder owns the page. Wisp: null is OutOfMemory. */
        Page *allocatePage(const Capacity &capacity) {
            Node *node = createPageExt(&pool, CreatePage(capacity), &page_serial, &page_size);
            if (!node) return nullptr;
            pages.append(node);
            return node->pageAssumeResident();
        }

        enum class FinishError {
            none,
            OutOfMemory,
            InvalidDimensions,
            InvalidPageDimensions,
            NoPages,
            InsufficientRows,
        };

        /* Validate the decoded pages and transfer them into a live PageList.
         * After this succeeds, `deinit` is a no-op because all resources have
         * transferred to the PageList. */
        FinishError finish(PageList *out) {
            /* These are basic validations but they're cheap to do and
             * we want to be careful we don't let corruption from untrusted
             * sources into our PageList which asserts this. */
            if (options.cols == 0 || options.rows == 0) {
                return FinishError::InvalidDimensions;
            }
            if (pages.first == nullptr) return FinishError::NoPages;

            /* Manually count our total rows at this point which we'll
             * need for our PageList cache as well as a safety check. */
            size_t total_rows_ = 0;
            for (Node *current = pages.first; current; current = current->next) {
                if (current->cols() == 0 || current->rows() == 0) {
                    return FinishError::InvalidPageDimensions;
                }
                total_rows_ += current->rows();
            }
            if (total_rows_ < options.rows) return FinishError::InsufficientRows;

            /* Get our active pin */
            Pin active_top;
            {
                size_t rem = options.rows;
                for (Node *current = pages.last; current; current = current->prev) {
                    if (rem <= current->rows()) {
                        active_top = Pin(current, (size::CellCountInt)(current->rows() - rem));
                        break;
                    }
                    rem -= current->rows();
                }
            }

            /* Set our viewport up to the active */
            Pin *viewport_pin_ = pool.pins.create();
            if (!viewport_pin_) return FinishError::OutOfMemory;
            *viewport_pin_ = active_top;

            /* Setup our one viewport tracked pin */
            PinSet tracked_pins_;
            if (!initTrackedPins(pool.alloc, viewport_pin_, &tracked_pins_)) {
                pool.pins.destroy(viewport_pin_);
                return FinishError::OutOfMemory;
            }

            /* Initialize limits */
            Limits limits_ = Limits::init(options.cols, options.rows);
            limits_.set(Limits::Key::bytes, options.max_size);
            limits_.set(Limits::Key::lines, options.max_lines);

            PageList &result = *out;
            result.cols = options.cols;
            result.rows = options.rows;
            result.pool = pool;
            result.pages = pages;
            result.page_serial = page_serial;
            result.page_serial_epoch = 0;
            result.page_size = page_size;
            result.page_compression = IncrementalCompressionState();
            result.recycle_node = nullptr;
            result.limits = limits_;
            result.total_rows = total_rows_;
            result.tracked_pins = tracked_pins_;
            result.viewport = Viewport::active;
            result.viewport_pin = viewport_pin_;
            result.viewport_pin_row_offset = Maybe<size_t>::none();
            result.pause_integrity_checks = 0;
            result.assertIntegrity();
            finished = true;
            return FinishError::none;
        }
    };

    struct Cell {
        Node *node;
        page::Row *row;
        page::Cell *cell;
        size::CellCountInt row_idx;
        size::CellCountInt col_idx;

        /* Returns true if this cell is marked as dirty.
         *
         * This is not very performant this is primarily used for assertions
         * and testing. */
        bool isDirty() const { return node->page()->dirty || row->dirty(); }

        /* Get the cell style.
         *
         * Not meant for non-test usage since this is inefficient. */
        style::Style style() const {
            if (cell->style_id() == style::default_id) return style::Style();
            Page *page = node->page();
            return *page->styles.get((const void *)page->memory, cell->style_id());
        }

        /* Gets the screen point for the given cell.
         *
         * This is REALLY expensive/slow so it isn't pub. This was built
         * for debugging and tests. If you have a need for this outside of
         * this file then consider a different approach and ask yourself very
         * carefully if you really need this. */
        point::Point screenPoint() const {
            uint32_t y = row_idx;
            const Node *n = node;
            while (n->prev) {
                n = n->prev;
                y += n->rows();
            }

            return point::Point::screen(col_idx, y);
        }
    };
/* @@DECLS@@ */
};

/* ====================================================================== */

inline bool PageList::init(zigstd::Allocator alloc, const Options &opts, PageList *out) {
    typedef init_tw tw;
    const size::CellCountInt cols = opts.cols;
    const size::CellCountInt rows = opts.rows;

    /* The screen starts with a single page that is the entire viewport,
     * and we'll split it thereafter if it gets too large and add more as
     * necessary. */
    if (tw::check(InitTw::init_memory_pool) != AllocTw::none) return false;
    MemoryPool pool;
    if (!MemoryPool::init(alloc, pageAllocator(alloc), page_preheat, &pool)) return false;

    if (tw::check(InitTw::init_pages) != AllocTw::none) {
        pool.deinit();
        return false;
    }
    uint64_t page_serial = 0;
    List page_list;
    size_t page_size = 0;
    if (!initPages(&pool, &page_serial, cols, rows, &page_list, &page_size)) {
        pool.deinit();
        return false;
    }

    Limits limits = Limits::init(cols, rows);
    limits.set(Limits::Key::bytes, opts.max_size);
    limits.set(Limits::Key::lines, opts.max_lines);

    /* We always track our viewport pin to ensure this is never an allocation */
    Pin *viewport_pin = nullptr;
    if (tw::check(InitTw::viewport_pin) != AllocTw::none || !(viewport_pin = pool.pins.create())) {
        releasePages(&pool, page_list);
        pool.deinit();
        return false;
    }
    *viewport_pin = Pin(page_list.first);

    PinSet tracked_pins;
    if (tw::check(InitTw::viewport_pin_track) != AllocTw::none || !initTrackedPins(pool.alloc, viewport_pin, &tracked_pins)) {
        releasePages(&pool, page_list);
        pool.deinit();
        return false;
    }

    PageList &result = *out;
    result.cols = cols;
    result.rows = rows;
    result.pool = pool;
    result.pages = page_list;
    result.page_serial = page_serial;
    result.page_serial_epoch = 0;
    result.page_size = page_size;
    result.page_compression = IncrementalCompressionState();
    result.recycle_node = nullptr;
    result.limits = limits;
    result.total_rows = rows;
    result.tracked_pins = tracked_pins;
    result.viewport = Viewport::active;
    result.viewport_pin = viewport_pin;
    result.viewport_pin_row_offset = Maybe<size_t>::none();
    result.pause_integrity_checks = 0;
    result.assertIntegrity();
    return true;
}

/* Create the tracked pin set for a new PageList with the viewport pin
 * already tracked. The set is sized for exactly the viewport pin and the
 * cursor pin that every Screen tracks. */
inline bool PageList::initTrackedPins(zigstd::Allocator alloc, Pin *viewport_pin, PinSet *out) {
    PinSet set;
    if (!set.setCapacity(alloc, pin_preheat)) return false;
    set.putAssumeCapacityNoClobber(viewport_pin);
    *out = set;
    return true;
}

inline bool PageList::initPages(MemoryPool *pool, uint64_t *serial, size::CellCountInt cols,
                                size::CellCountInt rows, List *out_list, size_t *out_size) {
    typedef initPages_tw tw;

    List page_list;
    size_t page_size_ = 0;

    /* Add pages as needed to create our initial viewport. */
    const Capacity cap = initialCapacity(cols);
    const Page::Layout layout = Page::layout(cap);
    const bool pooled = layout.total_size <= std_size;
    const zigstd::Allocator page_alloc = pool->pages.allocator;

    /* Guaranteed by comptime checks in initialCapacity but
     * redundant here for safety. */
    assert(layout.total_size <= size::max_page_size);

    size::CellCountInt rem = rows;
    while (rem > 0) {
        Node *node = nullptr;
        if (tw::check(InitPagesTw::page_node) != AllocTw::none || !(node = pool->nodes.create())) {
            /* If we have an error, we need to release the pages we created. */
            releasePages(pool, page_list);
            return false;
        }

        uint8_t *page_buf;
        size_t page_buf_len;
        if (pooled) {
            PageItem *buf = nullptr;
            if (tw::check(InitPagesTw::page_buf_std) != AllocTw::none || !(buf = pool->pages.create())) {
                pool->nodes.destroy(node);
                releasePages(pool, page_list);
                return false;
            }
            mem::recommit(buf->bytes, std_size);
            page_buf = buf->bytes;
            page_buf_len = std_size;
        } else {
            page_buf = nullptr;
            if (tw::check(InitPagesTw::page_buf_non_std) != AllocTw::none ||
                !(page_buf = page_alloc.alignedAlloc(layout.total_size, page_size_min))) {
                pool->nodes.destroy(node);
                releasePages(pool, page_list);
                return false;
            }
            page_buf_len = layout.total_size;
        }

        /* In runtime safety modes we have to memset because the Zig allocator
         * interface will always memset to 0xAA for undefined. On freestanding
         * (WASM), the WasmAllocator reuses freed slots without zeroing since
         * only fresh memory.grow pages are guaranteed zero by the WASM spec.
         * On native, the OS page allocator (mmap) returns zeroed pages.
         * Wisp: the test allocator is the C heap, so tests zero here. */
        if (is_test) memset(page_buf, 0, page_buf_len);

        /* Initialize the first set of pages to contain our viewport so that
         * the top of the first page is always the active area. */
        node->initResident(Page::initBuf(size::OffsetBuf::init(page_buf), layout), *serial,
                           pooled ? Node::Owned::pool : Node::Owned::heap);
        {
            const size::CellCountInt cap_rows = node->capacity().rows;
            node->page()->size.rows = rem < cap_rows ? rem : cap_rows;
        }
        rem -= node->rows();

        /* Add the page to the list */
        page_list.append(node);
        page_size_ += page_buf_len;

        /* Increment our serial */
        *serial += 1;
    }

    assert(page_list.first != nullptr);

    *out_list = page_list;
    *out_size = page_size_;
    return true;
}

/* Verify the integrity of the PageList. This is expensive and should
 * only be called in debug/test builds. */
inline PageList::IntegrityError PageList::verifyIntegrity() const {
    if (!slow_runtime_safety) return IntegrityError::none;
    if (pause_integrity_checks > 0) return IntegrityError::none;

    /* Our viewport pin should never be garbage */
    assert(!viewport_pin->garbage);

    /* Grab our total rows */
    size_t actual_total = 0;
    {
        for (const Node *node = pages.first; node; node = node->next) {
            actual_total += node->rows();

            /* Every live node must belong to the current validity epoch. */
            if (node->serial < page_serial_epoch) {
                /* log.warn("PageList integrity violation: page serial predates epoch") */
                return IntegrityError::PageSerialInvalid;
            }
        }
    }

    /* Verify that our cached total_rows matches the actual row count */
    if (actual_total != total_rows) {
        /* log.warn("PageList integrity violation: total_rows mismatch") */
        return IntegrityError::TotalRowsMismatch;
    }

    /* A line limit may only be exceeded when the oldest page also contains
     * active rows. Complete historical pages are always eligible for pruning. */
    if (total_rows > rows) {
        const size_t history_rows = total_rows - rows;
        if (history_rows > limits.max(Limits::Key::lines) && pages.first != getTopLeft(point::Tag::active).node) {
            /* log.warn("PageList integrity violation: max lines exceeded") */
            return IntegrityError::MaxLinesExceeded;
        }
    }

    /* Verify that all our tracked pins point to valid pages. */
    for (size_t i = 0; i < tracked_pins.count(); i++) {
        if (!pinIsValid(*tracked_pins.keys()[i])) return IntegrityError::TrackedPinInvalid;
    }

    if (viewport == Viewport::pin) {
        /* Verify that our viewport pin row offset is correct. */
        size_t actual_offset = 0;
        {
            size_t offset = 0;
            const Node *node = pages.last;
            bool found = false;
            for (; node; node = node->prev) {
                offset += node->rows();
                if (node == viewport_pin->node) {
                    offset -= viewport_pin->y;
                    actual_offset = total_rows - offset;
                    found = true;
                    break;
                }
            }

            if (!found) {
                /* log.warn("PageList integrity violation: viewport pin not in list") */
                return IntegrityError::ViewportPinOffsetMismatch;
            }
        }

        if (viewport_pin_row_offset.has) {
            if (viewport_pin_row_offset.value != actual_offset) {
                /* log.warn("PageList integrity violation: viewport pin offset mismatch") */
                return IntegrityError::ViewportPinOffsetMismatch;
            }
        }

        /* Ensure our viewport has enough rows. */
        const size_t vrows = total_rows - actual_offset;
        if (vrows < rows) {
            /* log.warn("PageList integrity violation: viewport pin rows too small") */
            return IntegrityError::ViewportPinInsufficientRows;
        }
    }

    return IntegrityError::none;
}

/* Release every page in the list during a teardown walk (deinit,
 * reset, or an errdefer unwinding a partially built list): heap-owned
 * pages go back to the page allocator, pool-owned pages back to the
 * page pool, and the nodes back to the node pool's free list. */
inline void PageList::releasePages(MemoryPool *pool, const List &list) {
    const zigstd::Allocator page_alloc = pool->pages.allocator;
    Node *it = list.first;
    while (it) {
        Node *node = it;
        it = node->next;
        Page *page = node->restore(Node::RestoreMode::discard);
        switch (node->owned) {
        case Node::Owned::pool: releasePoolPage(pool, page); break;
        case Node::Owned::heap: page_alloc.free(page->memory, page->memory_len, page_size_min); break;
        }
        pool->nodes.destroy(node);
    }
}

/* Release a pool-owned page during a teardown walk. Unlike
 * destroyNodeExt, this does not zero the page: on native, the item goes
 * straight back to the page allocator, and zeroing it first would write
 * the whole page (and fault a decommitted mapping back in) only for it
 * to be unmapped. */
inline void PageList::releasePoolPage(MemoryPool *pool, const Page *page) {
    PageItem *item = (PageItem *)page->memory;
    pool->pages.release(item);
}

/* Deinit the pagelist, freeing all page memory and the memory pool. */
inline void PageList::deinit() {
    /* Verify integrity before cleanup */
    assertIntegrity();

    /* Always deallocate our hashmap. */
    tracked_pins.deinit();

    /* Release every page and node back to the pools, then free the pools. */
    releasePages(&pool, pages);
    pool.deinit();
}

/* Reset the PageList back to an empty state. This is similar to
 * deinit and reinit but it importantly preserves the pointer
 * stability of tracked pins (they're moved to the top-left since
 * all contents are cleared).
 *
 * This can't fail because we always retain at least enough allocated
 * memory to fit the active area. */
inline void PageList::reset() {
    /* Reset discards all scrollback, so there is nothing left to compress. */
    page_compression.reset();

    /* Begin a new whole-list validity epoch before rebuilding from the pools.
     * Every old reference now has a serial below the epoch and can be rejected
     * in O(1), even if the node pool later reuses its pointer address. */
    page_serial_epoch = page_serial;

    /* We need enough pages/nodes to keep our active area. This should
     * never fail since we by definition have allocated a page already
     * that fits our size but I'm not confident to make that assertion. */
    const Capacity cap = initialCapacity(cols);
    assert(cap.rows > 0);

    /* The number of pages we need is the number of rows in the active
     * area divided by the row capacity of a page. */
    const size_t page_count = ((size_t)rows + cap.rows - 1) / cap.rows;

    /* Before resetting our pools we need to release our pages: heap-owned
     * pages go back to the page allocator and pool-owned pages back to
     * the page pool. */
    releasePages(&pool, pages);

    /* Reset our pools to free as much memory as possible while retaining
     * the capacity for at least the minimum number of pages we need.
     * The return value is whether memory was reclaimed or not, but in
     * either case the pool is left in a valid state.
     *
     * Retained page pool items are zero (see PagePool), so there is
     * nothing to scrub before initPages reuses them. */
    (void)pool.pages.reset(PagePool::ResetMode::retain_with_limit(page_count * PagePool::item_size));
    (void)pool.nodes.reset(NodePool::ResetMode::retain_with_limit(page_count * NodePool::item_size));

    /* Initialize our pages. This should not be able to fail since
     * we retained the capacity for the minimum number of pages we need. */
    if (!initPages(&pool, &page_serial, cols, rows, &pages, &page_size)) {
        fprintf(stderr, "initPages failed\n");
        abort();
    }

    /* Our total rows always goes back to the default */
    total_rows = rows;

    /* Update all our tracked pins to point to our first page top-left
     * and mark them as garbage, because it got mangled in a way where
     * semantically it really doesn't make sense. */
    {
        for (size_t i = 0; i < tracked_pins.count(); i++) {
            Pin *p = tracked_pins.keys()[i];
            p->node = pages.first;
            p->x = 0;
            p->y = 0;
            p->garbage = true;
        }

        /* Our viewport pin is never garbage */
        viewport_pin->garbage = false;
    }

    /* Move our viewport back to the active area since everything is gone. */
    viewport = Viewport::active;

    assertIntegrity();
}

/* ---- Pin movement (Wisp: out of line so Overflow can hold Pins) ---- */


/* Move the pin left n cells, wrapping to the previous row as needed.
 *
 * If the offset goes beyond the top of the screen, returns null.
 *
 * TODO: Unit tests. */
inline Maybe<PageList::Pin> PageList::Pin::leftWrap(size_t n) const {
    Pin result = *this;
    size_t remaining = n;
    while (remaining > result.x) {
        remaining -= (size_t)result.x + 1;
        const Maybe<Pin> u = result.up(1);
        if (!u.has) return Maybe<Pin>::none();
        result = u.value;
        /* Crossing a row boundary lands on that destination row's final
         * cell, whose width may differ from ours during reflow. */
        result.x = (size::CellCountInt)(result.node->cols() - 1);
    }

    result.x -= (size::CellCountInt)remaining;
    return result;
}

/* Move the pin right n cells, wrapping to the next row as needed.
 *
 * If the offset goes beyond the bottom of the screen, returns null.
 *
 * TODO: Unit tests. */
inline Maybe<PageList::Pin> PageList::Pin::rightWrap(size_t n) const {
    Pin result = *this;
    size_t remaining = n;
    for (;;) {
        const size_t row_remaining = (size_t)result.node->cols() - result.x - 1;
        if (remaining <= row_remaining) {
            result.x += (size::CellCountInt)remaining;
            return result;
        }

        remaining -= row_remaining + 1;
        const Maybe<Pin> d = result.down(1);
        if (!d.has) return Maybe<Pin>::none();
        result = d.value;
        result.x = 0;
    }
}

/* Move the pin down a certain number of rows, or return null if
 * the pin goes beyond the end of the screen. */
inline Maybe<PageList::Pin> PageList::Pin::down(size_t n) const {
    const Overflow o = downOverflow(n);
    if (o.tag == Overflow::Tag::offset) return o.offset;
    return Maybe<Pin>::none();
}

/* Move the pin up a certain number of rows, or return null if
 * the pin goes beyond the start of the screen. */
inline Maybe<PageList::Pin> PageList::Pin::up(size_t n) const {
    const Overflow o = upOverflow(n);
    if (o.tag == Overflow::Tag::offset) return o.offset;
    return Maybe<Pin>::none();
}

/* Move the offset down n rows. If the offset goes beyond the
 * end of the screen, return the overflow amount. */
inline PageList::Pin::Overflow PageList::Pin::downOverflow(size_t n) const {
    /* Index fits within this page */
    const size_t rows_ = (size_t)node->rows() - ((size_t)y + 1);
    if (n <= rows_) return Overflow::makeOffset(Pin(node, castCell((size_t)y + n), x));

    /* Need to traverse page links to find the page */
    Node *n_ = node;
    size_t n_left = n - rows_;
    for (;;) {
        if (!n_->next) {
            return Overflow::makeOverflow(
                Pin(n_, (size::CellCountInt)(n_->rows() - 1), minCell(x, (size::CellCountInt)(n_->cols() - 1))),
                n_left);
        }
        n_ = n_->next;
        if (n_left <= n_->rows())
            return Overflow::makeOffset(Pin(n_, castCell(n_left - 1), minCell(x, (size::CellCountInt)(n_->cols() - 1))));
        n_left -= n_->rows();
    }
}

/* Move the offset up n rows. If the offset goes beyond the
 * start of the screen, return the overflow amount. */
inline PageList::Pin::Overflow PageList::Pin::upOverflow(size_t n) const {
    /* Index fits within this page */
    if (n <= y) return Overflow::makeOffset(Pin(node, castCell((size_t)y - n), x));

    /* Need to traverse page links to find the page */
    Node *n_ = node;
    size_t n_left = n - y;
    for (;;) {
        if (!n_->prev) {
            return Overflow::makeOverflow(Pin(n_, 0, minCell(x, (size::CellCountInt)(n_->cols() - 1))), n_left);
        }
        n_ = n_->prev;
        if (n_left <= n_->rows())
            return Overflow::makeOffset(
                Pin(n_, castCell((size_t)n_->rows() - n_left), minCell(x, (size::CellCountInt)(n_->cols() - 1))));
        n_left -= n_->rows();
    }
}

/* ---- clone ---- */

inline page::PageError PageList::clone(zigstd::Allocator alloc, const Clone &opts, PageList *out) const {
    PageIterator it = pageIterator(Direction::right_down, opts.top, opts.bot);

    /* First, count our pages so our preheat is exactly what we need. */
    PageIterator it_copy = it;
    size_t page_count = 0;
    {
        Chunk c;
        while (it_copy.next(&c)) page_count += 1;
    }

    /* Setup our pool */
    MemoryPool pool_;
    if (!MemoryPool::init(alloc, pageAllocator(alloc), page_count, &pool_)) return page::PageError::OutOfMemory;

    /* Create our viewport. In a clone, the viewport always goes
     * to the top. */
    Pin *viewport_pin_ = pool_.pins.create();
    PinSet tracked_pins_;
    if (!viewport_pin_ || !initTrackedPins(pool_.alloc, viewport_pin_, &tracked_pins_)) {
        pool_.deinit();
        return page::PageError::OutOfMemory;
    }

    /* Our list of pages */
    List page_list;

    /* Wisp: errdefer releasePages / tracked_pins.deinit / pool.deinit */
    struct Fail {
        MemoryPool *pool;
        List *list;
        PinSet *pins;
        page::PageError fail(page::PageError e) {
            pins->deinit();
            releasePages(pool, *list);
            pool->deinit();
            return e;
        }
    } fail = {&pool_, &page_list, &tracked_pins_};

    /* Copy our pages */
    uint64_t page_serial_ = 0;
    size_t total_rows_ = 0;
    size_t page_size_ = 0;
    Chunk chunk;
    while (it.next(&chunk)) {
        /* Clone the page. We have to use createPageExt here because
         * we don't know if the source page has a standard size. */
        Node *node = createPageExt(&pool_, CreatePage(chunk.node->capacity()), &page_serial_, &page_size_);
        if (!node) return fail.fail(page::PageError::OutOfMemory);

        /* Add the page to the list immediately so that the errdefer
         * above releases it if cloning fails. */
        page_list.append(node);

        Page *dst_page = node->page();
        const Page *src_page = chunk.node->page();
        assert(node->capacity().rows >= chunk.end - chunk.start);
        dst_page->size.rows = (size::CellCountInt)(chunk.end - chunk.start);
        dst_page->size.cols = chunk.node->cols();
        const page::PageError e = dst_page->cloneFrom(src_page, chunk.start, chunk.end);
        if (e != page::PageError::none) {
            dst_page->assertIntegrity();
            return fail.fail(e);
        }

        dst_page->dirty = src_page->dirty;
        dst_page->assertIntegrity();

        total_rows_ += node->rows();

        /* Remap our tracked pins by changing the page and
         * offsetting the Y position based on the chunk start. */
        if (opts.tracked_pins) {
            Clone::TrackedPinsRemap *remap = opts.tracked_pins;
            for (size_t i = 0; i < tracked_pins.count(); i++) {
                Pin *p = tracked_pins.keys()[i];
                /* We're only interested in pins that were within the chunk. */
                if (p->node != chunk.node || p->y < chunk.start || p->y >= chunk.end) continue;
                Pin *new_p = pool_.pins.create();
                if (!new_p) return fail.fail(page::PageError::OutOfMemory);
                *new_p = *p;
                new_p->node = node;
                new_p->y -= (size::CellCountInt)chunk.start;
                assert(remap->find(p) == remap->end());
                (*remap)[p] = new_p;
                if (!tracked_pins_.putNoClobber(new_p)) return fail.fail(page::PageError::OutOfMemory);
            }
        }
    }

    /* Initialize our viewport pin to point to the first cloned page
     * so it points to valid memory. */
    *viewport_pin_ = Pin(page_list.first);

    PageList &result = *out;
    result.pool = pool_;
    result.pages = page_list;
    result.page_serial = page_serial_;
    result.page_serial_epoch = 0;
    result.page_size = page_size_;
    result.page_compression = IncrementalCompressionState();
    result.recycle_node = nullptr;
    result.limits = limits;
    result.cols = cols;
    result.rows = rows;
    result.total_rows = total_rows_;
    result.tracked_pins = tracked_pins_;
    result.viewport = Viewport::active;
    result.viewport_pin = viewport_pin_;
    result.viewport_pin_row_offset = Maybe<size_t>::none();
    result.pause_integrity_checks = 0;

    /* We always need to have enough rows for our viewport because this is
     * a pagelist invariant that other code relies on. */
    if (total_rows_ < rows) {
        const size_t len = rows - total_rows_;
        for (size_t i = 0; i < len; i++) {
            Node *grown;
            if (!result.grow(&grown)) {
                result.tracked_pins.deinit();
                releasePages(&result.pool, result.pages);
                result.pool.deinit();
                return page::PageError::OutOfMemory;
            }

            /* Clear the row. This is not very fast but in reality right
             * now we rarely clone less than the active area and if we do
             * the area is by definition very small. */
            Node *last = result.pages.last;
            Page *page = last->page();
            page::Row *row = &page->rows.ptr(page->memory)[last->rows() - 1];
            page->clearCells(row, 0, result.cols);
        }

        /* Update our total rows to be our row size. */
        result.total_rows = result.rows;
    }

    /* A clone can copy more history than its inherited line limit. */
    result.limits.enforce(&result, Limits::Key::lines);

    result.assertIntegrity();
    return page::PageError::none;
}

/* ---- resizeCols ---- */

inline bool PageList::resizeCols(size::CellCountInt new_cols, const Resize &opts) {
    assert(new_cols != cols);
    const Maybe<Resize::Cursor> cursor = opts.cursor;

    /* The active area is always the last `rows` rows, so a reflow that
     * changes the number of rows our text needs slides the active area
     * over the content. If we aren't allowed to pull scrollback then we
     * track the top of the active area so we can restore it afterwards. */
    Pin *active_top = nullptr;
    if (!opts.pull_scrollback) {
        active_top = trackPin(getTopLeft(point::Tag::active));
        if (!active_top) return false;
    }

    /* If we have a cursor position (x,y), then we try under any col resizing
     * to keep the same number remaining active rows beneath it. This is a
     * very special case if you can imagine clearing the screen (i.e.
     * scrollClear), having an empty active area, and then resizing to less
     * cols then we don't want the active area to "jump" to the bottom and
     * pull down scrollback. */
    struct PreservedCursor {
        Pin *tracked_pin;
        bool untrack;
        size_t remaining_rows;
        size_t wrapped_rows;
    };
    Maybe<PreservedCursor> preserved_cursor;
    if (cursor.has) {
        const Resize::Cursor &c = cursor.value;
        Maybe<Pin> p_;
        if (c.pin) {
            p_ = *c.pin;
        } else {
            p_ = pin(point::Point::active(c.x, c.y));
        }
        if (p_.has) {
            const Pin p = p_.value;
            const Maybe<Pin> active_pin = pin(point::Point::active());

            /* We count how many wraps the cursor had before it to begin with
             * so that we can offset any additional wraps to avoid pushing the
             * original row contents in to the scrollback. */
            size_t wrapped = 0;
            {
                /* If shrinking rows (in the .lt branch of resize, rows shrink
                 * before we get here) pushed the cursor pin above the new active
                 * area, there are no rows to count and iterating .left_up toward
                 * the active-area top would be an invalid (reversed) range. The
                 * preserved-cursor growth below already no-ops for a cursor that
                 * isn't in the active area, so we just count zero here. */
                if (!(active_pin.has && p.before(active_pin.value))) {
                    RowIterator row_it = p.rowIterator(Direction::left_up, active_pin);
                    Pin next;
                    while (row_it.next(&next)) {
                        const page::Row *row = next.rowAndCell().row;
                        if (row->wrap_continuation()) wrapped += 1;
                    }
                }
            }

            PreservedCursor pc;
            if (c.pin) {
                pc.tracked_pin = c.pin;
            } else {
                pc.tracked_pin = trackPin(p);
                if (!pc.tracked_pin) {
                    if (active_top) untrackPin(active_top);
                    return false;
                }
            }
            pc.untrack = c.pin == nullptr;
            pc.remaining_rows = rows > (size_t)c.y + 1 ? rows - ((size_t)c.y + 1) : 0;
            pc.wrapped_rows = wrapped;
            preserved_cursor = pc;
        }
    }

    /* Wisp: the function's defers, run on every exit. */
    struct Defers {
        PageList *self;
        Pin *active_top;
        Maybe<PreservedCursor> *preserved_cursor;
        ~Defers() {
            /* defer if (self.recycle_node) |node| ... (declared last, runs first) */
            if (Node *node = self->recycle_node) {
                self->recycle_node = nullptr;
                self->destroyNode(node);
            }
            if (preserved_cursor->has && preserved_cursor->value.untrack)
                self->untrackPin(preserved_cursor->value.tracked_pin);
            if (active_top) self->untrackPin(active_top);
        }
    } defers = {this, active_top, &preserved_cursor};

    /* Update our cols. We have to do this early because grow() that we
     * may call below relies on this to calculate the proper page size, but
     * after preserved_cursor so that the cursor pin can resolve coordinates in
     * the old active coordinate space. */
    cols = new_cols;

    /* Create the first node that contains our reflow. */
    Node *first_rewritten_node;
    {
        Page *page = pages.first->page();
        Capacity cap;
        if (!page->capacity.adjust(Capacity::Adjustment::withCols(new_cols), &cap)) {
            /* We verify all maxed out page layouts work. */
            cap = page->capacity;
            cap.cols = new_cols;

            /* We're growing columns so we can only get less rows so use
             * the lesser of our capacity and size so we minimize wasted
             * rows. */
            cap.rows = page->size.rows < cap.rows ? page->size.rows : cap.rows;
        }

        Node *node = createPage(CreatePage(cap));
        if (!node) return false;
        node->page()->size.rows = 1;
        first_rewritten_node = node;
    }

    /* We need to grab our rowIterator now before we rewrite our
     * linked list below. */
    RowIterator it = rowIterator(Direction::right_down, point::Point::screen(), Maybe<point::Point>());
    auto errdefer = [&]() {
        /* If an error occurs, we're in a pretty disastrous broken state,
         * but we should still try to clean up our leaked memory. Free
         * any of the remaining orphaned pages from before. If we reflowed
         * successfully this will be null. */
        Node *node_ = it.chunk.has ? it.chunk.value.node : nullptr;
        while (node_) {
            Node *node = node_;
            node_ = node->next;
            destroyNode(node);
        }
    };

    /* Reflowed source pages are stashed for reuse as destination
     * pages (see recycle_node). Whether we succeed or fail, a stashed
     * node must not outlive the reflow. (Wisp: see Defers.) */

    /* Set our new page as the only page. This orphans the existing pages
     * in the list, but that's fine since we're gonna delete them anyway. */
    pages.first = first_rewritten_node;
    pages.last = first_rewritten_node;

    /* Reflow all our rows. */
    {
        ReflowCursor reflow_cursor = ReflowCursor::init(first_rewritten_node);
        Pin row;
        while (it.next(&row)) {
            if (!reflow_cursor.reflowRow(this, row,
                                         preserved_cursor.has ? preserved_cursor.value.tracked_pin : nullptr)) {
                errdefer();
                return false;
            }

            /* Once we're done reflowing a page, we're done with it, so
             * make it available for reuse (or destroy it). Making it
             * immediately available frees memory and makes it more
             * likely in memory constrained environments that the next
             * reflow will work. */
            if (row.y == row.node->rows() - 1) {
                if (recycle_node != nullptr || row.node->owned != Node::Owned::pool ||
                    row.node->data.tag != Node::Data::Tag::resident) {
                    destroyNode(row.node);
                } else {
                    recycle_node = row.node;
                }
            }
        }

        /* At the end of the reflow, setup our total row cache */
        total_rows = reflow_cursor.total_rows;
    }

    /* If our total rows is less than our active rows, we need to grow.
     * This can happen if you're growing columns such that enough active
     * rows unwrap that we no longer have enough. */
    {
        Node *node_it = pages.first;
        size_t total = 0;
        bool reached = false;
        for (; node_it; node_it = node_it->next) {
            total += node_it->rows();
            if (total >= rows) {
                reached = true;
                break;
            }
        }
        if (!reached) {
            for (size_t i = total; i < rows; i++) {
                Node *g;
                if (!grow(&g)) return false;
            }
        }
    }

    /* Reflow can unwrap enough rows that a history viewport pin lands in the
     * active area before we do any preserved-cursor growth below. Switch back
     * to the active viewport now so intermediate grow() integrity checks stay
     * valid. */
    switch (viewport) {
    case Viewport::active:
    case Viewport::top: break;
    case Viewport::pin:
        if (pinIsActive(*viewport_pin)) viewport = Viewport::active;
        break;
    }

    /* If we can't pull scrollback then pad the bottom with blank rows until
     * the old top of the active area is back at the top. If the reflow
     * instead pushed it into scrollback (the text needs more rows than
     * we have) then there is nothing to do. This subsumes the preserved
     * cursor logic below since that also only exists to avoid a pull. */
    if (active_top) {
        const Maybe<point::Point> pt = pointFromPin(point::Tag::active, *active_top);
        if (pt.has) {
            for (size_t i = 0; i < pt.value.c.y; i++) {
                Node *g;
                if (!grow(&g)) return false;
            }
        }
        return true;
    }

    /* See preserved_cursor setup for why. */
    if (preserved_cursor.has) {
        const PreservedCursor &c = preserved_cursor.value;
        const Maybe<point::Point> active_pt = pointFromPin(point::Tag::active, *c.tracked_pin);
        if (active_pt.has) {
            const Maybe<Pin> active_pin = pin(point::Point::active());

            /* We need to determine how many rows we wrapped from the original
             * and subtract that from the remaining rows we expect because if
             * we wrap down we don't want to push our original row contents into
             * the scrollback. */
            size_t wrapped = 0;
            {
                RowIterator row_it = c.tracked_pin->rowIterator(Direction::left_up, active_pin);
                Pin next;
                while (row_it.next(&next)) {
                    const page::Row *row = next.rowAndCell().row;
                    if (row->wrap_continuation()) wrapped += 1;
                }
            }

            const size_t ay1 = (size_t)active_pt.value.c.y + 1;
            const size_t current = rows > ay1 ? rows - ay1 : 0;

            auto satsub = [](size_t a, size_t b) { return a > b ? a - b : 0; };
            size_t req_rows = c.remaining_rows;
            req_rows = satsub(req_rows, satsub(wrapped, c.wrapped_rows));
            req_rows = satsub(req_rows, current);

            while (req_rows > 0) {
                Node *g;
                if (!grow(&g)) return false;
                req_rows -= 1;
            }
        }
    }

    return true;
}

/* ---- increaseCapacity ---- */

inline PageList::IncreaseCapacityError PageList::increaseCapacity(Node *node, Maybe<IncreaseCapacity> adjustment,
                                                                  Node **out) {
    struct Guard {
        const PageList *l;
        ~Guard() { l->assertIntegrity(); }
    } guard = {this};
    Page *page = node->page();

    /* Apply our adjustment */
    Capacity cap = page->capacity;
    if (adjustment.has) {
        /* Wisp: `inline else` over the four fields; the field accessor
         * below stands in for @field. The field ints are u16/u32. */
        const IncreaseCapacity tag = adjustment.value;
        auto get = [&](const Capacity &c) -> uint64_t {
            switch (tag) {
            case IncreaseCapacity::styles: return c.styles;
            case IncreaseCapacity::grapheme_bytes: return c.grapheme_bytes;
            case IncreaseCapacity::hyperlink_bytes: return c.hyperlink_bytes;
            default: return c.string_bytes;
            }
        };
        auto set = [&](Capacity &c, uint64_t v) {
            switch (tag) {
            case IncreaseCapacity::styles: c.styles = (size::StyleCountInt)v; break;
            case IncreaseCapacity::grapheme_bytes: c.grapheme_bytes = (size::GraphemeBytesInt)v; break;
            case IncreaseCapacity::hyperlink_bytes: c.hyperlink_bytes = (size::HyperlinkCountInt)v; break;
            default: c.string_bytes = (size::StringBytesInt)v; break;
            }
        };
        uint64_t max_int;
        switch (tag) {
        case IncreaseCapacity::styles: max_int = (size::StyleCountInt)~(size::StyleCountInt)0; break;
        case IncreaseCapacity::grapheme_bytes: max_int = (size::GraphemeBytesInt)~(size::GraphemeBytesInt)0; break;
        case IncreaseCapacity::hyperlink_bytes:
            max_int = (size::HyperlinkCountInt)~(size::HyperlinkCountInt)0;
            break;
        default: max_int = (size::StringBytesInt)~(size::StringBytesInt)0; break;
        }

        const uint64_t old = get(cap);

        uint64_t new_;
        {
            /* A dimension can be zero for pages with exact
             * capacities (see compact). Doubling zero stays zero,
             * which would break our guarantee that we always
             * increase by at least one unit and turn caller retry
             * loops into infinite loops. Jump straight to the
             * standard default instead: it is what all standard
             * pages start with, so retrying callers are guaranteed
             * enough room for their pending allocation. */
            if (old == 0) {
                const Capacity def(0, 0);
                new_ = get(def);
            } else if (old * 2 > max_int) {
                /* We use checked math to prevent overflow. If there is
                 * an overflow it means we're out of space in this
                 * dimension, since pages can take up to their maxInt
                 * capacity in any category.
                 *
                 * Our final doubling would overflow since maxInt is
                 * 2^N - 1 for an unsignged int of N bits. So, if we overflow
                 * and we haven't used all the bits, use all the bits. */
                if (old < max_int) {
                    new_ = max_int;
                } else {
                    return IncreaseCapacityError::OutOfSpace;
                }
            } else {
                new_ = old * 2;
            }
        }
        set(cap, new_);

        /* If our capacity exceeds the maximum page size, treat it
         * as an OutOfSpace because things like page splitting will
         * help. */
        const Page::Layout layout = Page::layout(cap);
        if (layout.total_size > size::max_page_size) {
            return IncreaseCapacityError::OutOfSpace;
        }

        /* Doubling alone has a really bad behavior in that if you're
         * in a pathological scenario with only one dimension of cap
         * increase, you have to pay repeat double costs. Each time you
         * double we have to reclone the entire page. As the page gets
         * bigger this gets more expensive.
         *
         * Instead, for some dimensions, we do something else: we look
         * at the current utilization, and project that to every row
         * in the page capacity (if it fits). We just assume that your
         * future workload will look the one you're currently filling.
         * If we're wrong, its some wasted capacity but future growth
         * still works in other dimensions.
         *
         * This only applies to the current page being grown. I previously
         * tried a high-water-mark style solution to preallocate pages,
         * which does work really well, but its not clear when you reset
         * the mark. */
        do {
            /* The dimensions we project must measure their current
             * usage in the same units as the capacity field. */
            uint64_t used;
            switch (tag) {
            case IncreaseCapacity::grapheme_bytes:
                used = page->grapheme_alloc.usedBytes((const void *)page->memory);
                break;

            /* Living item count. Note the capacity field is a
             * requested item count that Layout.init rounds in
             * both directions (table to the next power of two,
             * items to the load factor of that), so this is an
             * approximation in capacity units; the headroom
             * below absorbs the error and an undershoot only
             * costs one more (non-ladder) growth event. */
            case IncreaseCapacity::styles: used = page->styles.count(); break;

            default: used = 0; break;
            }
            if (tag != IncreaseCapacity::grapheme_bytes && tag != IncreaseCapacity::styles) break;
            if (used == 0 || page->size.rows == 0) break;

            /* Full-page need at current density, plus 25% headroom
             * for chunk rounding and fragmentation. */
            const uint64_t density = used * (uint64_t)cap.rows / page->size.rows;
            const uint64_t projected_raw = density + density / 4;

            /* Bound the jump to 32× the pre-growth capacity. Arbitrary
             * choice until we can show otherwise. */
            uint64_t projected = projected_raw;
            if (old * 32 < projected) projected = old * 32;
            if (max_int < projected) projected = max_int;
            if (projected <= get(cap)) break;

            /* Only take the projection if the resulting page still fits. */
            Capacity proj_cap = cap;
            set(proj_cap, projected);
            if (Page::layout(proj_cap).total_size > size::max_page_size) break;
            cap = proj_cap;
        } while (false);
    }

    /* log.info("adjusting page capacity={}") */

    /* Create our new page and clone the old page into it. */
    Node *new_node = createPage(CreatePage(cap));
    if (!new_node) return IncreaseCapacityError::OutOfMemory;
    Page *new_page = new_node->page();
    assert(new_page->capacity.rows >= page->capacity.rows);
    assert(new_page->capacity.cols >= page->capacity.cols);
    new_page->size.rows = page->size.rows;
    new_page->size.cols = page->size.cols;
    if (new_page->cloneFrom(page, 0, page->size.rows) != page::PageError::none) {
        /* cloneFrom only errors if there isn't capacity for the data
         * from the source page but we're only increasing capacity so
         * this should never be possible. If it happens, we should crash
         * because we're in no man's land and can't safely recover.
         * log.err("increaseCapacity clone failed err={}") */
        fprintf(stderr, "unexpected clone failure\n");
        abort();
    }

    /* Preserve page-level dirty flag (cloneFrom only copies row data) */
    new_page->dirty = page->dirty;

    /* Must not fail after this because the operations we do after this
     * can't be recovered. */

    /* Fix up all our tracked pins to point to the new page. */
    for (size_t i = 0; i < tracked_pins.count(); i++) {
        Pin *p = tracked_pins.keys()[i];
        if (p->node != node) continue;
        p->node = new_node;
    }

    /* Insert this page and destroy the old page */
    pages.insertBefore(node, new_node);
    pages.remove(node);
    destroyNode(node);
    page_compression.markActivity();

    new_page->assertIntegrity();
    *out = new_node;
    return IncreaseCapacityError::none;
}
/* ---- Pin iterators ---- */

inline PageList::PageIterator PageList::Pin::pageIterator(Direction direction, Maybe<Pin> limit) const {
    if (slow_runtime_safety) {
        if (limit.has) {
            const Pin &l = limit.value;
            /* Check the order according to the iteration direction. */
            switch (direction) {
            case Direction::right_down: assert(eql(l) || before(l)); break;
            case Direction::left_up: assert(eql(l) || l.before(*this)); break;
            }
        }
    }

    PageIterator it;
    it.row = *this;
    if (limit.has) {
        it.limit.tag = PageIterator::Limit::Tag::row;
        it.limit.row = limit.value;
    }
    it.direction = direction;
    return it;
}

inline PageList::RowIterator PageList::Pin::rowIterator(Direction direction, Maybe<Pin> limit) const {
    RowIterator r;
    r.page_it = pageIterator(direction, limit);
    Chunk chunk;
    if (!r.page_it.next(&chunk)) return r;
    r.chunk = chunk;
    r.offset = direction == Direction::right_down ? chunk.start : (size::CellCountInt)(chunk.end - 1);
    return r;
}

inline PageList::CellIterator PageList::Pin::cellIterator(Direction direction, Maybe<Pin> limit) const {
    CellIterator c;
    c.row_it = rowIterator(direction, limit);
    Pin cell;
    if (!c.row_it.next(&cell)) return c;
    cell.x = x;
    c.cell = cell;
    return c;
}

inline PageList::PromptIterator PageList::Pin::promptIterator(Direction direction, Maybe<Pin> limit) const {
    PromptIterator it;
    it.current = *this;
    it.limit = limit;
    it.direction = direction;
    return it;
}

/* ---- getCell / isDirty ---- */

inline Maybe<PageList::Cell> PageList::getCell(const point::Point &pt) const {
    const Maybe<Pin> pt_pin = pin(pt);
    if (!pt_pin.has) return Maybe<Cell>::none();
    const Page::RowAndCell rac = pt_pin.value.node->page()->getRowAndCell(pt_pin.value.x, pt_pin.value.y);
    Cell c;
    c.node = pt_pin.value.node;
    c.row = rac.row;
    c.cell = rac.cell;
    c.row_idx = pt_pin.value.y;
    c.col_idx = pt_pin.value.x;
    return c;
}

inline bool PageList::isDirty(const point::Point &pt) const { return getCell(pt).value.isDirty(); }

/* ---- diagram ---- */

namespace page_list_detail {
inline void writeByteNTimes(std::string *w, char c, size_t n) { w->append(n, c); }
inline void writeUtf8(std::string *w, uint32_t cp) {
    if (cp < 0x80) {
        w->push_back((char)cp);
    } else if (cp < 0x800) {
        w->push_back((char)(0xC0 | (cp >> 6)));
        w->push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        w->push_back((char)(0xE0 | (cp >> 12)));
        w->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        w->push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        w->push_back((char)(0xF0 | (cp >> 18)));
        w->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        w->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        w->push_back((char)(0x80 | (cp & 0x3F)));
    }
}
inline void print(std::string *w, size_t v) { *w += std::to_string(v); }
inline size_t log10Int(size_t v) {
    size_t r = 0;
    while (v >= 10) {
        v /= 10;
        r += 1;
    }
    return r;
}
} /* namespace page_list_detail */

/* Log a debug diagram of the page list to the provided writer.
 *
 * EXAMPLE:
 *
 *      +-----+ = PAGE 0
 *  ... |     |
 *   50 | foo |
 *  ... |     |
 *     +--------+ ACTIVE
 *  124 |     | | 0
 *  125 |Text | | 1
 *      :  ^  : : = PIN 0
 *  126 |Wrap…  | 2
 *      +-----+ :
 *      +-----+ : = PAGE 1
 *    0 …ed   | | 3
 *    1 | etc.| | 4
 *      +-----+ :
 *     +--------+ */
inline bool PageList::diagram(std::string *writer) const {
    using namespace page_list_detail;
    const Pin active_pin = getTopLeft(point::Tag::active);

    bool active = false;
    size_t active_index = 0;

    size_t page_index = 0;
    size_t dcols = 0;

    PageIterator it = pageIterator(Direction::right_down, point::Point::screen(), Maybe<point::Point>());
    Chunk chunk;
    for (; it.next(&chunk); page_index += 1) {
        dcols = chunk.node->cols();

        /* Whether we've just skipped some number of rows and drawn
         * an ellipsis row (this is reset when a row is not skipped). */
        bool skipped = false;

        for (size_t y = 0; y < chunk.node->rows(); y++) {
            /* Active header */
            if (!active && chunk.node == active_pin.node && active_pin.y == y) {
                active = true;
                *writer += "     +-";
                writeByteNTimes(writer, '-', dcols);
                *writer += "--+ ACTIVE";
                *writer += '\n';
            }

            /* Page header */
            if (y == 0) {
                *writer += "      +";
                writeByteNTimes(writer, '-', dcols);
                *writer += '+';
                if (active) *writer += " :";
                *writer += " = PAGE ";
                print(writer, page_index);
                *writer += '\n';
            }

            /* Row contents */
            {
                const page::Row *row = chunk.node->page()->getRow(y);
                const page::Cell *cells = chunk.node->page()->getCells(row);

                bool row_has_content = false;

                for (size_t i = 0; i < dcols; i++) {
                    if (cells[i].hasText()) {
                        row_has_content = true;
                        break;
                    }
                }

                /* We don't want to print this row's contents
                 * unless it has text or is in the active area. */
                if (!active && !row_has_content) {
                    /* If we haven't, draw an ellipsis row. */
                    if (!skipped) {
                        *writer += "  ... :";
                        writeByteNTimes(writer, ' ', dcols);
                        *writer += ':';
                        if (active) *writer += " :";
                        *writer += '\n';
                    }
                    skipped = true;
                    continue;
                }

                skipped = false;

                /* Left pad row number to 5 wide */
                const size_t y_digits = y == 0 ? 0 : log10Int(y);
                writeByteNTimes(writer, ' ', 4 - y_digits);
                print(writer, y);
                *writer += ' ';

                /* Left edge or wrap continuation marker */
                *writer += row->wrap_continuation() ? "\xE2\x80\xA6" : "|";

                /* Row text */
                if (row_has_content) {
                    for (size_t i = 0; i < dcols; i++) {
                        const page::Cell *cell = &cells[i];
                        /* Skip spacer tails, since wide cells are, well, wide. */
                        if (cell->wide() == page::Cell::Wide::spacer_tail) continue;

                        /* Write non-printing bytes as base36, for convenience. */
                        if (cell->codepoint() < ' ') {
                            *writer += "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[cell->codepoint()];
                            continue;
                        }
                        writeUtf8(writer, cell->codepoint());
                        if (cell->hasGrapheme()) {
                            size_t glen = 0;
                            const uint32_t *grapheme = chunk.node->page()->lookupGrapheme(cell, &glen);
                            for (size_t k = 0; k < glen; k++) writeUtf8(writer, grapheme[k]);
                        }
                    }
                } else {
                    writeByteNTimes(writer, ' ', dcols);
                }

                /* Right edge or wrap marker */
                *writer += row->wrap() ? "\xE2\x80\xA6" : "|";
                if (active) {
                    *writer += " | ";
                    print(writer, active_index);
                    active_index += 1;
                }

                *writer += '\n';
            }

            /* Tracked pin marker(s) */
            {
                /* If we have more than 16 tracked pins in a row, oh well,
                 * don't wanna bother making this function allocating. */
                Pin *pin_buf[16];
                size_t pin_count = 0;
                for (size_t i = 0; i < tracked_pins.count(); i++) {
                    Pin *p = tracked_pins.keys()[i];
                    if (p->node != chunk.node) continue;
                    if (p->y != y) continue;
                    pin_buf[pin_count] = p;
                    pin_count += 1;
                    if (pin_count >= 16) return false;
                }

                if (pin_count > 0) {
                    Pin **pins = pin_buf;
                    /* std.mem.sort is a stable block sort; insertion sort is stable too. */
                    for (size_t i = 1; i < pin_count; i++) {
                        Pin *v = pins[i];
                        size_t j = i;
                        while (j > 0 && v->x < pins[j - 1]->x) {
                            pins[j] = pins[j - 1];
                            j -= 1;
                        }
                        pins[j] = v;
                    }

                    *writer += "      :";
                    size_t x = 0;

                    for (size_t i = 0; i < pin_count; i++) {
                        const Pin *p = pins[i];
                        if (x > p->x) continue;
                        writeByteNTimes(writer, ' ', p->x - x);
                        *writer += '^';
                        x = (size_t)p->x + 1;
                    }

                    writeByteNTimes(writer, ' ', dcols - x);
                    *writer += ':';

                    if (active) *writer += " :";

                    *writer += " = PIN";
                    if (pin_count > 1) *writer += "S";

                    x = pins[0]->x;
                    for (size_t i = 0; i < pin_count; i++) {
                        if (pins[i]->x != x) *writer += ',';
                        *writer += ' ';
                        print(writer, i);
                    }

                    *writer += '\n';
                }
            }
        }

        /* Page footer */
        {
            *writer += "      +";
            writeByteNTimes(writer, '-', dcols);
            *writer += '+';
            if (active) *writer += " :";
            *writer += '\n';
        }
    }

    /* Active footer */
    {
        *writer += "     +-";
        writeByteNTimes(writer, '-', dcols);
        *writer += "--+";
        *writer += '\n';
    }
    return true;
}

/* ---- highlightSemanticContent ---- */

inline Maybe<PageList::HighlightUntracked> PageList::highlightSemanticContent(
    const Pin &at, page::Cell::SemanticContent content) const {
    typedef page::Cell::SemanticContent SC;
    /* Performance note: we can do this more efficiently in a single
     * forward-pass. Semantic content operations aren't usually fast path
     * but if someone wants to optimize them someday that's great. */

    Pin end;
    {
        /* Safety assertion, our starting point should be a prompt row.
         * so the first returned prompt should be ourselves. */
        PromptIterator it = at.promptIterator(Direction::right_down, Maybe<Pin>());
        Pin first;
        const bool ok = it.next(&first);
        assert(ok && first.y == at.y);
        (void)ok;

        /* Our end is the end of the line just before the next prompt
         * line, which should exist since we verified we have at least
         * two prompts here. */
        Pin next;
        bool have_end = false;
        if (it.next(&next)) {
            const Maybe<Pin> prev_ = next.up(1);
            if (prev_.has) {
                Pin prev = prev_.value;
                prev.x = (size::CellCountInt)(prev.node->cols() - 1);
                end = prev;
                have_end = true;
            }
        }

        /* Didn't find any further prompt so the end of our zone is
         * the end of the screen. */
        if (!have_end) end = getBottomRight(point::Tag::screen).value;
    }

    switch (content) {
    /* For the prompt, we select all the way up to command output.
     * We include all the input lines, too. */
    case SC::prompt: {
        HighlightUntracked result;
        result.start = at.left(at.x);
        result.end = at;

        CellIterator it = at.cellIterator(Direction::right_down, end);
        Pin p;
        while (it.next(&p)) {
            switch (p.rowAndCell().cell->semantic_content()) {
            case SC::prompt:
            case SC::input: result.end = p; break;
            case SC::output: return result;
            }
        }

        return result;
    }

    /* For input, we include the start of the input to the end of
     * the input, which may include all the prompts in the middle, too. */
    case SC::input: {
        HighlightUntracked result;

        /* Find the start */
        CellIterator it = at.cellIterator(Direction::right_down, end);
        Pin p;
        bool found = false;
        while (!found && it.next(&p)) {
            switch (p.rowAndCell().cell->semantic_content()) {
            case SC::prompt: break;
            case SC::input:
                result.start = p;
                result.end = p;
                found = true;
                break;
            case SC::output: return Maybe<HighlightUntracked>::none();
            }
        }
        /* No input found */
        if (!found) return Maybe<HighlightUntracked>::none();

        /* Find the end */
        while (it.next(&p)) {
            const SC sc = p.rowAndCell().cell->semantic_content();
            /* Prompts can be nested in our input for continuation */
            if (sc == SC::prompt) continue;

            /* Output means we're done */
            if (sc == SC::output) break;

            result.end = p;
        }

        return result;
    }

    default: {
        HighlightUntracked result;

        /* Find the start */
        CellIterator it = at.cellIterator(Direction::right_down, end);
        Pin p;
        bool found = false;
        while (!found && it.next(&p)) {
            const page::Cell *cell = p.rowAndCell().cell;
            switch (cell->semantic_content()) {
            case SC::prompt:
            case SC::input: break;
            case SC::output:
                /* Skip empty cells - they default to .output but aren't real output */
                if (!cell->hasText()) break;
                result.start = p;
                result.end = p;
                found = true;
                break;
            }
        }
        /* No output found */
        if (!found) return Maybe<HighlightUntracked>::none();

        /* Find the end */
        while (it.next(&p)) {
            const page::Cell *cell = p.rowAndCell().cell;
            const SC sc = cell->semantic_content();
            if (sc == SC::prompt || sc == SC::input) break;
            /* Only extend to cells with actual text */
            if (cell->hasText()) result.end = p;
        }

        return result;
    }
    }
}

/* ---- Limits.enforce ---- */

inline void PageList::Limits::enforce(PageList *pagelist, Key key) const {
    if (!exceeded(pagelist, key)) return;

    /* A partially constructed clone can temporarily contain fewer rows than
     * its active area. It has no scrollback to prune. */
    if (pagelist->total_rows <= pagelist->rows) return;

    /* Accumulate the row delta so viewport offsets are fixed up once
     * after all eligible pages have been removed. */
    size_t removed = 0;
    while (exceeded(pagelist, key)) {
        Node *first = pagelist->pages.first;

        /* The page containing the active top may also contain history. Keep
         * that boundary page whole even if its history exceeds the heuristic. */
        if (first == pagelist->getTopLeft(point::Tag::active).node) break;

        if (removed == 0) pagelist->page_compression.markActivity();

        const size_t first_rows = first->rows();

        /* Automatic pruning invalidates the content represented by pins in
         * the removed page. erasePage remaps them to the next page below but
         * only the enforcing caller knows that their original content is gone,
         * so mark them as garbage here. */
        for (size_t i = 0; i < pagelist->tracked_pins.count(); i++) {
            Pin *p = pagelist->tracked_pins.keys()[i];
            if (p->node == first) p->garbage = true;
        }

        /* erasePage updates the list, pin targets, and byte accounting.
         * Row accounting belongs to the caller because erasePage is also used
         * by paths that already adjusted total_rows. */
        pagelist->erasePage(first);
        pagelist->total_rows -= first_rows;
        removed += first_rows;
    }

    /* Reconcile viewport mode and cached row offsets with the combined prefix
     * removal only after every page and pin points into the final list. */
    if (removed > 0) pagelist->fixupViewport(removed);
}
/* @@IMPL@@ */

} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_PAGE_LIST_HPP */
