/* Transliterated from Ghostty src/terminal/highlight.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Highlights are any contiguous sequences of cells that should
 * be called out in some way, most commonly for text selection but
 * also search results or any other purpose.
 *
 * Within the terminal package, a highlight is a generic concept
 * that represents a range of cells.
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 * Wisp: std.MultiArrayList(Chunk) is a std::vector<Chunk>. Untracked is
 * PageList::HighlightUntracked (PageList.zig and highlight.zig import each
 * other; the type lives with PageList and is aliased here).
 */

/* NOTE: The plan is for highlights to ultimately replace Selection
 * completely. Selection is deeply tied to various parts of the Ghostty
 * internals so this may take some time. */

#pragma once
#ifndef WISP_VT_HIGHLIGHT_HPP
#define WISP_VT_HIGHLIGHT_HPP

#include <vector>

#include "screen.hpp"

namespace wisp {
namespace vt {
namespace highlight {

typedef PageList::Pin Pin;
typedef PageList::PageIterator::Chunk PageChunk;

struct Tracked;

/* An untracked highlight is a highlight that stores its highlighted
 * area as a top-left and bottom-right screen pin. Since it is untracked,
 * the pins are only valid for the current terminal state and may not
 * be safe to use after any terminal modifications.
 *
 * For rectangle highlights/selections, the downstream consumer of this
 * code is expected to interpret the pins in whatever shape they want.
 * For example, a rectangular selection would interpret the pins as
 * setting the x bounds for each row between start.y and end.y.
 *
 * To simplify all operations, start MUST be before or equal to end. */
typedef PageList::HighlightUntracked Untracked;

/* A tracked highlight is a highlight that stores its highlighted
 * area as tracked pins within a screen.
 *
 * A tracked highlight ensures that the pins remain valid even as
 * the terminal state changes. Because of this, tracked highlights
 * have more operations available to them.
 *
 * There is more overhead to creating and maintaining tracked highlights.
 * If you're manipulating highlights that are untracked and you're sure
 * that the terminal state won't change, you can use the `initAssume`
 * function. */
struct Tracked {
    Pin *start;
    Pin *end;

    /* Wisp: false is OutOfMemory. */
    static bool init(Screen *screen, const Pin &start, const Pin &end, Tracked *out) {
        Pin *start_tracked = screen->pages.trackPin(start);
        if (!start_tracked) return false;
        Pin *end_tracked = screen->pages.trackPin(end);
        if (!end_tracked) {
            screen->pages.untrackPin(start_tracked);
            return false;
        }
        out->start = start_tracked;
        out->end = end_tracked;
        return true;
    }

    /* Initializes a tracked highlight by assuming that the provided
     * pins are already tracked. This allows callers to perform tracked
     * operations without the overhead of tracking the pins, if the
     * caller can guarantee that the pins are already tracked or that
     * the terminal state will not change.
     *
     * Do not call deinit on highlights created with this function. */
    static Tracked initAssume(Pin *start, Pin *end) {
        Tracked t;
        t.start = start;
        t.end = end;
        return t;
    }

    void deinit(Screen *screen) const {
        screen->pages.untrackPin(start);
        screen->pages.untrackPin(end);
    }
};

/* Untracked.track. Wisp: a free function since Untracked is PageList's. */
inline bool track(const Untracked &self, Screen *screen, Tracked *out) {
    return Tracked::init(screen, self.start, self.end, out);
}

/* A flattened highlight is a highlight that stores its highlighted
 * area as a list of page chunks. This representation allows for
 * traversing the entire highlighted area without needing to read any
 * terminal state or dereference any page nodes (which may have been
 * pruned). */
struct Flattened {
    /* A flattened chunk is almost identical to a PageList.Chunk but
     * we also flatten the serial number. This lets the flattened
     * highlight more robust for comparisons and validity checks with
     * the PageList. */
    struct Chunk {
        PageList::Node *node;
        uint64_t serial;
        size::CellCountInt start;
        size::CellCountInt end;
    };

    /* The page chunks that make up this highlight. This handles the
     * y bounds since chunks[0].start is the first highlighted row
     * and chunks[len - 1].end is the last highlighted row (exclsive). */
    std::vector<Chunk> chunks;

    /* The x bounds of the highlight. `bot_x` may be less than `top_x`
     * for typical left-to-right highlights: can start the selection right
     * of the end on a higher row. */
    size::CellCountInt top_x;
    size::CellCountInt bot_x;

    static Flattened empty() {
        Flattened f;
        f.top_x = 0;
        f.bot_x = 0;
        return f;
    }

    /* Wisp: upstream initializes `.end_x = end.x`, a field that does not
     * exist; Zig never compiles the unused function. bot_x is meant. */
    static Flattened init(const Pin &start, const Pin &end) {
        Flattened result = empty();
        PageList::PageIterator it = start.pageIterator(PageList::Direction::right_down, end);
        PageChunk chunk;
        while (it.next(&chunk)) {
            Chunk c;
            c.node = chunk.node;
            c.serial = chunk.node->serial;
            c.start = (size::CellCountInt)chunk.start;
            c.end = (size::CellCountInt)chunk.end;
            result.chunks.push_back(c);
        }
        result.top_x = start.x;
        result.bot_x = end.x;
        return result;
    }

    void deinit() { chunks.clear(); }

    Flattened clone() const { return *this; }

    Pin startPin() const { return Pin(chunks[0].node, chunks[0].start, top_x); }

    Pin endPin() const {
        const Chunk &last = chunks[chunks.size() - 1];
        return Pin(last.node, (size::CellCountInt)(last.end - 1), bot_x);
    }

    /* Convert to an Untracked highlight. */
    Untracked untracked() const {
        /* Note: we don't use startPin/endPin here because it is slightly
         * faster to reuse the slices. */
        Untracked u;
        u.start = Pin(chunks[0].node, chunks[0].start, top_x);
        const Chunk &last = chunks[chunks.size() - 1];
        u.end = Pin(last.node, (size::CellCountInt)(last.end - 1), bot_x);
        return u;
    }
};

} /* namespace highlight */
} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_HIGHLIGHT_HPP */
