/* Transliterated from Ghostty src/terminal/kitty/graphics_animation.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Wisp, differences in shape rather than behavior:
 *   - `std.ArrayListUnmanaged(Frame)` is `zigstd::ArrayListUnmanaged<Frame>`;
 *     `frames.items`/`frames.items.len` become `frames.items`/`frames.items_len`.
 *   - `Frame.data: []u8` is a `uint8_t *data` plus `size_t data_len`.
 *   - `?u64 frame_shown_at_ms` is a `has_frame_shown_at_ms` flag beside
 *     the value.
 *   - upstream declares the nested `State` and `Frame` types after the
 *     fields that use them; C++ needs them first, so they are moved to
 *     the top of the struct. Field and method order is upstream's.
 *   - `frames.append` can fail, so it returns bool here; upstream's
 *     `try` is the caller's problem. `Animation` itself has no fallible
 *     method, so nothing else changes shape.
 *
 * Kitty graphics protocol animation support.
 *
 * https://sw.kovidgoyal.net/kitty/graphics-protocol/#animation
 *
 * An animation is a set of frames attached to an existing image.
 * Frame 1 (the "root frame") is the image's own base data. Frames
 * 2..N are stored here. Unlike Kitty, which stores frames as delta
 * rectangles in a disk cache and composes them lazily, we compose
 * every frame eagerly into a full image-sized RGBA buffer at load
 * time. This trades some memory for a much simpler model: a frame is
 * always ready to display and there are no reference chains to
 * maintain, coalesce, or garbage collect. Frame storage is bounded
 * by the same byte limit as image storage.
 *
 * To keep composition to a single pixel format, an image is
 * converted to RGBA the first time an animation command composes
 * into it, and all transmitted frame data is converted to RGBA
 * before composition. Blending an opaque (alpha=255) source pixel
 * degenerates to a copy, so this loses no information relative to
 * Kitty's separate RGB/RGBA paths. The pixel-level primitives
 * (conversion, fill, rectangle composition) live in
 * graphics_pixel.zig.
 */

#pragma once
#ifndef WISP_TERMINAL_KITTY_GRAPHICS_ANIMATION_HPP
#define WISP_TERMINAL_KITTY_GRAPHICS_ANIMATION_HPP

#include <stddef.h>
#include <stdint.h>

#include "../../zigstd/allocator.hpp"
#include "../../zigstd/array_list.hpp"

namespace wisp {
namespace terminal {
namespace kitty {
namespace graphics_animation {

typedef ::wisp::zigstd::Allocator Allocator;

/* The gap assigned to a newly created frame when the command doesn't
 * specify one (z omitted or z=0). Taken from Kitty (DEFAULT_GAP). */
static const uint32_t default_gap_ms = 40;

/* Animation state for a single image. Heap-allocated and owned by
 * the Image. This is created lazily by the first animation command that
 * needs it. */
struct Animation {
    enum class State : uint8_t {
        /* Not advancing; frames change only via a=a c=N (client-driven). */
        stopped,
        /* Advancing, but playback parks on the last frame waiting
         * for more frames instead of looping (a=a s=2). */
        loading,
        /* Advancing and looping (a=a s=3). */
        running,
    };

    struct Frame {
        /* Fully composed pixel data, always image width * height * 4
         * bytes of RGBA. */
        uint8_t *data;
        size_t data_len;

        /* Milliseconds this frame is displayed before advancing.
         * Zero means gapless: skipped during playback. */
        uint32_t gap_ms;
    };

    /* The frames following the root frame: `frames.items[i]` is
     * protocol frame number `i + 2`. The root frame (frame 1) is the
     * image's base data and is not stored here. */
    ::wisp::zigstd::ArrayListUnmanaged<Frame> frames; /* = .empty */

    /* The gap of the root frame in milliseconds. Zero means gapless:
     * the frame is skipped during playback. Kitty creates the root
     * frame with a zero gap; the client gives it one with a=a,r=1,z=N. */
    uint32_t root_gap_ms; /* = 0 */

    /* The zero-based index of the frame currently displayed: 0 is
     * the root frame, i >= 1 is frames.items[i - 1]. */
    uint32_t current_index; /* = 0 */

    /* Playback state. Every image starts stopped (client-driven). */
    State state; /* = .stopped */

    /* Maximum number of loops to play (0 = infinite). Set from the
     * a=a v key as v-1, per the protocol's off-by-one encoding. */
    uint32_t max_loops; /* = 0 */

    /* Number of completed loops since playback started. */
    uint32_t current_loop; /* = 0 */

    /* Timestamp in milliseconds (on the ticker's clock, see
     * ImageStorage.animationTick) when the current frame was shown.
     * Null means "not yet shown"; the next tick stamps it. */
    bool has_frame_shown_at_ms; /* = false (null) */
    uint64_t frame_shown_at_ms;

    Animation()
        : frames(), root_gap_ms(0), current_index(0), state(State::stopped), max_loops(0),
          current_loop(0), has_frame_shown_at_ms(false), frame_shown_at_ms(0) {}

    void deinit(Allocator alloc) {
        for (size_t i = 0; i < frames.items_len; i += 1) {
            const Frame &frame = frames.items[i];
            alloc.free(frame.data, frame.data_len);
        }
        frames.deinit(alloc);
    }

    /* The total number of frames including the root frame. */
    uint32_t frameCount() const { return (uint32_t)(frames.items_len + 1); }

    /* The gap of the frame at the given zero-based index. */
    uint32_t gapAt(uint32_t index) const {
        if (index == 0) return root_gap_ms;
        return frames.items[index - 1].gap_ms;
    }

    /* Set the gap of the frame at the given zero-based index. */
    void setGapAt(uint32_t index, uint32_t gap_ms) {
        if (index == 0) {
            root_gap_ms = gap_ms;
        } else {
            frames.items[index - 1].gap_ms = gap_ms;
        }
    }

    /* The sum of all frame gaps. An animation with a zero duration
     * (every frame gapless) never advances; this doubles as the
     * guard that keeps the gapless-skip loop in animationTick from
     * spinning forever, exactly like Kitty's animation_duration. */
    uint64_t durationMs() const {
        uint64_t total = root_gap_ms;
        for (size_t i = 0; i < frames.items_len; i += 1) total += frames.items[i].gap_ms;
        return total;
    }

    /* Bytes of frame data held by this animation, counted against
     * the image storage limit. */
    size_t frameBytes() const {
        size_t total = 0;
        for (size_t i = 0; i < frames.items_len; i += 1) total += frames.items[i].data_len;
        return total;
    }
};

} /* namespace graphics_animation */
} /* namespace kitty */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_KITTY_GRAPHICS_ANIMATION_HPP */
