/* Transliterated from Ghostty src/terminal/mem.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Virtual-memory operations shared by terminal page owners.
 *
 * Terminal pages use page-aligned, page-multiple mappings. This module can
 * discard the physical pages behind one of those mappings without releasing
 * its virtual address range, then prepare the same range for reuse. It does
 * not allocate memory or decide which terminal pages should be discarded.
 *
 * Decommit releases physical pages only. The address range and its memory
 * accounting (the Linux VMA, the Windows commit charge) stay with the
 * process, so a read after decommit returns zeros or the old contents rather
 * than faulting, and recommit has nothing to acquire that could fail.
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:". Only the
 * Windows and test-build paths are carried over (this is a Windows port).
 */

#pragma once
#ifndef WISP_VT_MEM_HPP
#define WISP_VT_MEM_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "page.hpp"

/* Wisp: declared directly rather than through <windows.h> (see page.hpp),
 * unless a translation unit already pulled windows.h in, which declares it
 * with its own types. */
#ifndef _WINDOWS_
extern "C" __declspec(dllimport) unsigned long __stdcall DiscardVirtualMemory(void *, size_t);
#endif

namespace wisp {
namespace vt {
namespace mem {

/* What guarantee decommit must provide when the OS cannot discard a mapping. */
enum class DecommitMode : uint8_t {
    /* The dirty prefix must read as zero after this call, even when physical
     * reclamation is unavailable. Bytes after the prefix must already be zero. */
    zero,

    /* Physical-memory reclamation is required. Do not touch the mapping when
     * reclamation is unsupported or fails; report the failure to the caller. */
    strict,
};

/* Return whether this target can reclaim physical memory for `mode` while
 * retaining the mapping's virtual address range.
 *
 * Test builds support both modes because `decommit` simulates reclamation by
 * clearing the supplied range. Runtime reclamation is intentionally limited
 * to 64-bit Linux, Darwin, and Windows. Other targets must leave strict
 * callers' memory resident; zero mode still provides its documented memset
 * fallback through `decommit` even when this function returns false. */
inline bool canReclaim(DecommitMode mode) {
    (void)mode;
    /* Tests never call into the OS because their allocator ranges can
     * share mappings with unrelated allocations. `decommit` simulates
     * successful reclamation by zeroing the requested range instead,
     * so both modes are always available to tests on every target. */
    if (is_test) return true;

    /* Compression currently retains complete page mappings for its
     * lifetime. Limit the initial runtime support to 64-bit address
     * spaces where that virtual-memory cost is negligible and where
     * the retained-mapping behavior has been validated. */
    if (sizeof(void *) != 8) return false;

    /* Windows provides DiscardVirtualMemory, which releases the
     * physical pages behind a committed range while keeping it
     * committed, so nothing has to be committed again before reuse.
     * Page memory is already a VirtualAlloc region (see page.zig)
     * and kernel32 is linked by every Windows build. */
    return true;
}

/* Discard physical pages while retaining a mapping's virtual address range.
 *
 * The complete mapping must be page-aligned and a multiple of the minimum
 * system page size. `dirty_len` identifies the prefix whose contents may be
 * nonzero. Strict mode requires the complete mapping to be dirty because a
 * successful discard invalidates all of its contents.
 *
 * The return value reports whether the OS accepted the reclamation request.
 * Test builds return true after simulating reclamation by zeroing dirty bytes.
 * In zero mode, the requested bytes are guaranteed to be zero regardless of
 * the return value. */
inline bool decommit(DecommitMode mode, uint8_t *memory, size_t memory_len, size_t dirty_len) {
    /* assert(memory.len > 0);
     * assert(@intFromPtr(memory.ptr) % std.heap.page_size_min == 0);
     * assert(memory.len % std.heap.page_size_min == 0);
     * assert(dirty_len <= memory.len);
     * if (comptime mode == .strict) assert(dirty_len == memory.len); */

    /* Testing allocator ranges may share an OS mapping with unrelated memory,
     * so madvise is not safe. Zeroing models the only content guarantee callers
     * have after a successful discard. */
    if (is_test) {
        memset(memory, 0, dirty_len);
        return true;
    }

    /* DiscardVirtualMemory releases the physical pages behind the range but
     * leaves it committed, so the commit charge stays with the process and a
     * later access finds a zero page or the old contents instead of faulting.
     * Zero mode clears its dirty prefix first, as on Darwin: the bytes read
     * as zero afterward whether or not the discard took. Strict mode skips
     * that write because its caller replaces the entire mapping after
     * recommit. The call reports failure through its return value rather
     * than the thread's last error. */
    if (mode == DecommitMode::zero) memset(memory, 0, dirty_len);

    const unsigned long rc = ::DiscardVirtualMemory(memory, memory_len);
    if (rc == 0 /* ERROR_SUCCESS */) return true;

    /* Zero mode has already cleared its bytes and strict callers must
     * leave the still-resident mapping alone, so there is nothing more
     * to do for either mode.
     * log.warn("DiscardVirtualMemory failed err={d}") */
    return false;
}

/* Prepare a mapping previously passed to decommit for reuse.
 *
 * Linux, Windows, and test builds need no explicit operation because their
 * mappings stay committed through decommit. Darwin pairs FREE_REUSABLE with
 * FREE_REUSE so pages touched by the caller are accounted to the process
 * again. Failure does not invalidate the retained mapping, so reuse can
 * continue after logging the accounting failure. */
inline void recommit(uint8_t *memory, size_t memory_len) {
    /* assert(memory.len > 0); page-aligned; page-multiple */
    (void)memory;
    (void)memory_len;
}

} /* namespace mem */
} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_MEM_HPP */
