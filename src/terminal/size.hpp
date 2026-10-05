/* Ported from Ghostty src/terminal/size.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Offset-based addressing for page memory.
 *
 * A Page is a single contiguous allocation holding several variable-length
 * arrays (cells, styles, grapheme bytes, hyperlinks, strings). Rather than
 * storing absolute pointers to each, everything is stored as a byte offset
 * from the allocation's base. That keeps a Page relocatable: it can be
 * memcpy'd, written to disk or moved between allocations without any pointer
 * fixups, because no absolute address is ever stored inside it.
 *
 * Every offset is measured from the "true base" — byte zero of the whole
 * allocation, not of the sub-structure that owns the field. That way callers
 * always pass the same base pointer down, and no code has to sum up a chain
 * of intermediate offsets to resolve an address.
 */

#pragma once
#ifndef WISP_TERMINAL_SIZE_HPP
#define WISP_TERMINAL_SIZE_HPP

#include <stddef.h>
#include <stdint.h>
#include <assert.h>

namespace wisp {
namespace terminal {

/* The maximum size of a page in bytes. Everything else here is derived from
 * this, so widening a page means changing this one constant. */
constexpr uint64_t MAX_PAGE_SIZE = UINT32_MAX;

/* The int type that can contain the maximum memory offset in bytes, derived
 * from the maximum page size. */
typedef uint32_t OffsetInt;

/* Total number of cells possible in each dimension (row/col). 2^16 is enough
 * for any reasonable terminal size and keeps cell IDs 16-bit. */
typedef uint16_t CellCountInt;

/* Total number of styles and hyperlinks possible in a page. These match
 * CellCountInt because a cell in a single row has at most one style, which
 * makes splitting a page by splitting rows straightforward.
 *
 * Note that the ref-counted set is one value short of this, but that is a
 * theoretical limit we accept: reaching it needs a single-row page at maximum
 * width where every cell has a unique style. */
typedef CellCountInt StyleCountInt;
typedef CellCountInt HyperlinkCountInt;

/* Bytes available for grapheme and string data. Both are unbounded given
 * malicious input, so each gets a 2^32 (4GB) ceiling. */
typedef uint32_t GraphemeBytesInt;
typedef uint32_t StringBytesInt;

/* Round v up to the next multiple of alignment, which must be a power of
 * two. Lives here rather than beside its first caller because every
 * offset-based structure needs it to lay out its members. */
inline size_t align_forward(size_t v, size_t alignment) {
    return (v + alignment - 1) & ~(alignment - 1);
}

/* ─── base address helpers ───────────────────────────────────────────────── */

struct OffsetBuf;

inline uintptr_t int_from_base(const void *base) {
    return reinterpret_cast<uintptr_t>(base);
}
inline uintptr_t int_from_base(const OffsetBuf &base);

/* ─── Offset ─────────────────────────────────────────────────────────────── */

/* The offset from the base address of a page to the start of some data,
 * typed for ease of use. */
template <typename T>
struct Offset {
    OffsetInt offset = 0;

    /* A slice of T stored as a base offset plus a length. */
    struct Slice {
        Offset<T> offset = {};
        size_t    len = 0;

        template <typename Base>
        T *slice(Base base) const { return offset.ptr(base); }
    };

    /* Pointer to the start of the data, properly typed. */
    template <typename Base>
    T *ptr(Base base) const {
        /* The offset must be aligned for T because the return type is
         * naturally aligned. This COULD be relaxed to return arbitrary
         * alignment, but nothing needs that. */
        const uintptr_t addr = int_from_base(base) + offset;
        assert(addr % alignof(T) == 0);
        return reinterpret_cast<T *>(addr);
    }
};

/* ─── OffsetBuf ──────────────────────────────────────────────────────────── */

/* A buffer offset from some base pointer.
 *
 * Offset-based structures take this as their initialization parameter so they
 * know which segment of memory they own, while still recording their offset
 * fields against the true base. */
struct OffsetBuf {
    /* The true base pointer of the backing memory — byte zero of the
     * allocation. This plus `offset` is what every caller passes down, so the
     * offsets stored in structures stay correct. */
    uint8_t *base = nullptr;

    /* Offset from base to where *this* structure begins. Used to build up a
     * chain of offset-based structures while keeping the base pointer handed
     * to functions the true base. */
    size_t offset = 0;

    /* A zero-offset buffer from a base. */
    static OffsetBuf init(void *base) { return init_offset(base, 0); }

    static OffsetBuf init_offset(void *base, size_t offset) {
        OffsetBuf b;
        b.base = reinterpret_cast<uint8_t *>(base);
        b.offset = offset;
        return b;
    }

    /* Where this buffer's own data starts. Anything before this belongs to
     * something else. */
    uint8_t *start() const { return base + offset; }

    /* An Offset for a child member. The result is measured against the true
     * base so later callers can keep passing that base in. */
    template <typename T>
    Offset<T> member(size_t len) const {
        Offset<T> o;
        o.offset = static_cast<OffsetInt>(offset + len);
        return o;
    }

    /* Advance the current offset. */
    OffsetBuf add(size_t n) const {
        OffsetBuf b;
        b.base = base;
        b.offset = offset + n;
        return b;
    }

    /* Collapse the offset into the base, yielding a zero-offset buffer.
     * Similar to add(), but every offset is merged into base. */
    OffsetBuf rebase(size_t n) const {
        OffsetBuf b;
        b.base = start() + n;
        b.offset = 0;
        return b;
    }
};

inline uintptr_t int_from_base(const OffsetBuf &base) {
    return reinterpret_cast<uintptr_t>(base.base);
}

/* The offset from some base pointer to an actual pointer to T. */
template <typename T, typename Base>
Offset<T> get_offset(Base base, const T *ptr) {
    const uintptr_t base_int = int_from_base(base);
    const uintptr_t ptr_int = reinterpret_cast<uintptr_t>(ptr);
    Offset<T> o;
    o.offset = static_cast<OffsetInt>(ptr_int - base_int);
    return o;
}

/* Offset must stay exactly OffsetInt-sized: a Page stores many of these, and
 * the upstream design depends on the packing. */
static_assert(sizeof(Offset<uint8_t>) == sizeof(OffsetInt),
              "Offset must be exactly OffsetInt wide");

} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_SIZE_HPP */
