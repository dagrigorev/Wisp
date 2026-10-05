/* Transliterated from Ghostty src/terminal/size.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: src/vt/ holds the exact transliteration of Ghostty's terminal data
 * layer (size, page, PageList, Screen, Terminal and what they need),
 * namespace wisp::vt. It is built beside the earlier reimplementation in
 * src/terminal/, which the app still runs on, and replaces it once
 * stream_terminal.zig is ported.
 */

#pragma once
#ifndef WISP_VT_SIZE_HPP
#define WISP_VT_SIZE_HPP

#include <stddef.h>
#include <stdint.h>

namespace wisp {
namespace vt {
namespace size {

/* The maximum size of a page in bytes. We use a u16 here because any
 * smaller bit size by Zig is upgraded anyways to a u16 on mainstream
 * CPU architectures, and because 65KB is a reasonable page size. To
 * support better configurability, we derive everything from this. */
static const uint64_t max_page_size = 0xFFFFFFFFu;

/* The int type that can contain the maximum memory offset in bytes,
 * derived from the maximum terminal page size. */
typedef uint32_t OffsetInt;

/* Int types for maximum values of things. A lot of these sizes are
 * based on "X is enough for any reasonable use case" principles.
 * The goal is that a user can have the maxInt amount of all of these
 * present at one time and be able to address them in a single Page.zig. */

/* Total number of cells that are possible in each dimension (row/col).
 * Based on 2^16 being enough for any reasonable terminal size and allowing
 * IDs to remain 16-bit. */
typedef uint16_t CellCountInt;

/* Total number of styles and hyperlinks that are possible in a page.
 * We match CellCountInt here because each cell in a single row can have at
 * most one style, making it simple to split a page by splitting rows.
 *
 * Note due to the way RefCountedSet works, we are short one value, but
 * this is a theoretical limit we accept. A page with a single row max
 * columns wide would be one short of having every cell have a unique style. */
typedef CellCountInt StyleCountInt;
typedef CellCountInt HyperlinkCountInt;

/* Total number of bytes that can be taken up by grapheme data and string
 * data. Both of these technically unlimited with malicious input, but
 * we choose a reasonable limit of 2^32 (4GB) per. */
typedef uint32_t GraphemeBytesInt;
typedef uint32_t StringBytesInt;

struct OffsetBuf;

/* Wisp: intFromBase — a pointer or an OffsetBuf. */
inline uintptr_t intFromBase(const void *base) { return (uintptr_t)base; }
inline uintptr_t intFromBase(const OffsetBuf &base);

/* The offset from the base address of the page to the start of some data.
 * This is typed for ease of use.
 *
 * This is a packed struct so we can attach methods to an int. */
template <typename T>
struct Offset {
    OffsetInt offset; /* = 0 */

    Offset() : offset(0) {}
    explicit Offset(OffsetInt o) : offset(o) {}

    /* A slice of type T that stores via a base offset and len. */
    struct Slice {
        Offset offset; /* = .{} */
        size_t len;    /* = 0 */

        Slice() : offset(), len(0) {}

        /* Returns a slice for the data, properly typed.
         * Wisp: the pointer; the length is `len`. */
        template <typename B>
        T *slice(const B &base) const { return offset.ptr(base); }
    };

    /* Returns a pointer to the start of the data, properly typed. */
    template <typename B>
    T *ptr(const B &base) const {
        /* The offset must be properly aligned for the type since
         * our return type is naturally aligned. We COULD modify this
         * to return arbitrary alignment, but its not something we need. */
        const uintptr_t addr = intFromBase(base) + offset;
        /* assert(addr % @alignOf(T) == 0) */
        return (T *)addr;
    }

    bool operator==(const Offset &o) const { return offset == o.offset; }
    bool operator!=(const Offset &o) const { return offset != o.offset; }
};

/* Represents a buffer that is offset from some base pointer.
 * Offset-based structures should use this as their initialization
 * parameter so that they can know what segment of memory they own
 * while at the same time initializing their offset fields to be
 * against the true base.
 *
 * The term "true base" is used to describe the base address of
 * the allocation, which i.e. can include memory that you do NOT
 * own and is used by some other structures. All offsets are against
 * this "true base" so that to determine addresses structures don't
 * need to add up all the intermediary offsets. */
struct OffsetBuf {
    /* The true base pointer to the backing memory. This is
     * "byte zero" of the allocation. This plus the offset make
     * it easy to pass in the base pointer in all usage to this
     * structure and the offsets are correct. */
    uint8_t *base;

    /* Offset from base where the beginning of /this/ data
     * structure is located. We use this so that we can slowly
     * build up a chain of offset-based structures but always
     * have the base pointer sent into functions be the true base. */
    size_t offset; /* = 0 */

    /* Initialize a zero-offset buffer from a base. */
    static OffsetBuf init(const void *base) { return initOffset(base, 0); }

    /* Initialize from some base pointer and offset. */
    static OffsetBuf initOffset(const void *base, size_t offset) {
        OffsetBuf b;
        b.base = (uint8_t *)intFromBase(base);
        b.offset = offset;
        return b;
    }

    /* The base address for the start of the data for the user
     * of this OffsetBuf. This is where your data structure should
     * begin; anything before this is NOT your memory. */
    uint8_t *start() const { return base + offset; }

    /* Returns an Offset calculation for some child member of
     * your struct. The offset is against the true base pointer
     * so that future callers can pass that in as the base. */
    template <typename T>
    Offset<T> member(size_t len) const { return Offset<T>((OffsetInt)(offset + len)); }

    /* Add an offset to the current offset. */
    OffsetBuf add(size_t off) const {
        OffsetBuf b;
        b.base = base;
        b.offset = offset + off;
        return b;
    }

    /* Rebase the offset to have a zero offset by rebasing onto start.
     * This is similar to `add` but all of the offsets are merged into base. */
    OffsetBuf rebase(size_t off) const {
        OffsetBuf b;
        b.base = start() + off;
        b.offset = 0;
        return b;
    }
};

inline uintptr_t intFromBase(const OffsetBuf &base) { return (uintptr_t)base.base; }

/* Get the offset for a given type from some base pointer to the
 * actual pointer to the type. */
template <typename T, typename B>
inline Offset<T> getOffset(const B &base, const T *ptr) {
    const uintptr_t base_int = intFromBase(base);
    const uintptr_t ptr_int = (uintptr_t)ptr;
    const uintptr_t offset = ptr_int - base_int;
    return Offset<T>((OffsetInt)offset);
}

} /* namespace size */
} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_SIZE_HPP */
