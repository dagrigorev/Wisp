/* Transliterated from Ghostty src/terminal/kitty/graphics_pixel.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Pixel-buffer operations for the Kitty graphics protocol: format
 * conversion to RGBA, background fill, and rectangle composition.
 * These are the primitives behind animation frame loading (a=f) and
 * frame composition (a=c). See graphics_animation.hpp for the
 * animation model built on top of them.
 *
 * Wisp, differences in shape rather than behavior:
 *   - This file is ported in full. `kitty_graphics` was false when the
 *     surrounding subsystem was ported and this file was omitted; it is
 *     in scope now.
 *   - `Allocator.Error![]u8` becomes an `Error` enum return
 *     (`Error::none` on success) with the buffer through a pointer and
 *     a length out parameter. `OutOfMemory` is the only member, as it
 *     is the only error upstream can return (see rgbaFromFormat).
 *   - `[]u8` / `[]const u8` is a pointer plus a length. Where upstream
 *     slices a sub-range (`dst[off..][0..n]`) the port passes
 *     `ptr + off` with length `n` and asserts the range is in bounds,
 *     which is Zig's own bounds check.
 *   - Upstream's declaration order, function order and control flow are
 *     kept, except that the `swizzle` namespace is hoisted to the top
 *     because C++ needs it defined before use.
 *
 * Wisp, the external seam — `wuffs`:
 *   Upstream imports `wuffs` here but uses it *only* for four pixel
 *   swizzles, never for image decoding. Rather than pull the library
 *   into the port, the `swizzle` namespace below implements those four
 *   operations directly against their specification, the same judgement
 *   already made for src/zigstd/inflate.hpp. Each carries its own
 *   `Wisp:` note. The composite is the only one with any arithmetic in
 *   it; its rounding is spelled out at `rgbaSrcOver` and is verified
 *   byte-for-byte against the vectors in upstream's own
 *   pkg/wuffs/src/swizzle.zig `rgbaSrcOver` test.
 */

#pragma once
#ifndef WISP_TERMINAL_KITTY_GRAPHICS_PIXEL_HPP
#define WISP_TERMINAL_KITTY_GRAPHICS_PIXEL_HPP

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../zigstd/allocator.hpp"
#include "graphics_command.hpp"

namespace wisp {
namespace terminal {
namespace kitty {
namespace graphics_pixel {

typedef ::wisp::zigstd::Allocator Allocator;

namespace command = ::wisp::terminal::kitty::graphics;

/* Wisp: the error set of every function in this file, flattened. Only
 * `Allocator.Error` reaches a caller; upstream's `error.WuffsError` and
 * `error.Overflow` are `unreachable` in rgbaFromFormat and the
 * replacement swizzles cannot produce them at all. */
enum class Error : uint8_t {
    none,
    OutOfMemory,
};

/* ── Wisp: stand-in for `wuffs.swizzle`. ─────────────────────────────
 *
 * Wisp: these four functions replace `pkg/wuffs/src/swizzle.zig`, which
 * is a thin wrapper over wuffs' `pixel_swizzler`. They are written from
 * the specification of the exact format pairs upstream asks for, not
 * transliterated, because the C library behind them cannot be. They are
 * the only non-mechanical code in this file.
 *
 * Upstream requests these wuffs format pairs:
 *   rgbToRgba  RGB           -> RGBA_PREMUL,    blend SRC
 *   gToRgba    Y             -> RGBA_PREMUL,    blend SRC
 *   gaToRgba   YA_NONPREMUL  -> RGBA_NONPREMUL, blend SRC
 *   rgbaSrcOver RGBA_NONPREMUL -> RGBA_NONPREMUL, blend SRC_OVER
 *
 * The first three have an opaque or passed-through alpha, so no alpha
 * arithmetic happens in any of them: PREMUL and NONPREMUL agree when
 * alpha is 0xFF, and upstream's own comment on `gaToRgba` records that
 * the YA_NONPREMUL pair was chosen precisely so the grey/alpha pair is
 * copied with "no alpha math". Only `rgbaSrcOver` composites. */
namespace swizzle {

/* Wisp: wuffs RGB -> RGBA_PREMUL, blend SRC. Widens each 3-byte pixel
 * to 4 bytes by appending an opaque alpha; the colour bytes are copied
 * unchanged (alpha 0xFF makes premultiplied and straight alpha equal).
 * `src_len` must be a multiple of 3, which is wuffs' own
 * `src.len % src_size == 0` assertion. */
inline Error rgbToRgba(Allocator alloc, const uint8_t *src, size_t src_len, uint8_t **out_ptr,
                       size_t *out_len) {
    assert(src_len % 3 == 0);
    const size_t dst_len = src_len / 3 * 4;
    uint8_t *dst = alloc.alloc(dst_len);
    if (dst == nullptr) return Error::OutOfMemory;
    for (size_t i = 0, j = 0; i < src_len; i += 3, j += 4) {
        dst[j + 0] = src[i + 0];
        dst[j + 1] = src[i + 1];
        dst[j + 2] = src[i + 2];
        dst[j + 3] = 0xFF;
    }
    *out_ptr = dst;
    *out_len = dst_len;
    return Error::none;
}

/* Wisp: wuffs Y -> RGBA_PREMUL, blend SRC. Replicates each 1-byte grey
 * sample into R, G and B and appends an opaque alpha. */
inline Error gToRgba(Allocator alloc, const uint8_t *src, size_t src_len, uint8_t **out_ptr,
                     size_t *out_len) {
    const size_t dst_len = src_len * 4;
    uint8_t *dst = alloc.alloc(dst_len);
    if (dst == nullptr) return Error::OutOfMemory;
    for (size_t i = 0, j = 0; i < src_len; i += 1, j += 4) {
        const uint8_t y = src[i];
        dst[j + 0] = y;
        dst[j + 1] = y;
        dst[j + 2] = y;
        dst[j + 3] = 0xFF;
    }
    *out_ptr = dst;
    *out_len = dst_len;
    return Error::none;
}

/* Wisp: wuffs YA_NONPREMUL -> RGBA_NONPREMUL, blend SRC. Replicates the
 * grey sample into R, G and B and carries the supplied alpha across
 * untouched — see upstream's comment on `gaToRgba`: the nonpremul pair
 * is chosen so that no alpha math happens. `src_len` must be a multiple
 * of 2. */
inline Error gaToRgba(Allocator alloc, const uint8_t *src, size_t src_len, uint8_t **out_ptr,
                      size_t *out_len) {
    assert(src_len % 2 == 0);
    const size_t dst_len = src_len / 2 * 4;
    uint8_t *dst = alloc.alloc(dst_len);
    if (dst == nullptr) return Error::OutOfMemory;
    for (size_t i = 0, j = 0; i < src_len; i += 2, j += 4) {
        const uint8_t y = src[i + 0];
        dst[j + 0] = y;
        dst[j + 1] = y;
        dst[j + 2] = y;
        dst[j + 3] = src[i + 1];
    }
    *out_ptr = dst;
    *out_len = dst_len;
    return Error::none;
}

/* Wisp: wuffs' `composite_nonpremul_nonpremul_u32_axxx`, the per-pixel
 * kernel behind RGBA_NONPREMUL -> RGBA_NONPREMUL with blend SRC_OVER.
 *
 * Composite `src` over `dst` in place. Both are straight
 * (non-premultiplied) alpha RGBA of the same length.
 *
 * The rounding is wuffs': every step truncates, and the only concession
 * to precision is that 8-bit channels are widened to 16 bits by
 * multiplying by 0x101 (so 0xFF maps to 0xFFFF exactly) and narrowed
 * again by a plain `>> 8`. In order:
 *
 *   1. widen all eight channels: v16 = v8 * 0x101
 *   2. premultiply the destination: d = (d * da) / 0xFFFF
 *   3. ia = 0xFFFF - sa
 *   4. da' = sa + (da * ia) / 0xFFFF
 *      d'  = (s * sa) / 0xFFFF + (d * ia) / 0xFFFF
 *   5. un-premultiply, guarded: if da' != 0, d' = (d' * 0xFFFF) / da'
 *   6. narrow: v8 = v16 >> 8
 *
 * There is NO rounding-to-nearest anywhere and no special case for a
 * transparent source or destination. Upstream's `graphics_pixel.zig`
 * test and upstream's `pkg/wuffs/src/swizzle.zig` test both assert that
 * compositing over a *transparent destination* reproduces the source
 * byte-for-byte; that is not a special case in the code, it falls out of
 * this arithmetic (step 2 zeroes the destination, step 5 divides by
 * exactly the `sa` that step 4 produced, and the truncations cancel).
 * Verified by hand against all four pixels of upstream's wuffs
 * `rgbaSrcOver` test vector and re-asserted in the port's tests.
 *
 * The three colour channels use one identical formula, so wuffs' own
 * choice to pack the little-endian RGBA bytes as 0xAABBGGRR (making its
 * "r" the port's byte 2) makes no difference to the result; the port
 * works on bytes in memory order. */
inline void rgbaSrcOver(uint8_t *dst, size_t dst_len, const uint8_t *src, size_t src_len) {
    assert(dst_len == src_len);
    assert(dst_len % 4 == 0);
    (void)src_len; /* Wisp: only the assert reads it; NDEBUG drops that. */

    for (size_t i = 0; i < dst_len; i += 4) {
        /* Convert from 8-bit color to 16-bit color. */
        const uint32_t sa = 0x101u * (uint32_t)src[i + 3];
        const uint32_t da_in = 0x101u * (uint32_t)dst[i + 3];

        /* Calculate the inverse of the src-alpha: how much of the dst
         * to keep. */
        const uint32_t ia = 0xFFFFu - sa;

        /* Composite src (nonpremul) over dst (premul). */
        const uint32_t da = sa + ((da_in * ia) / 0xFFFFu);

        for (size_t ch = 0; ch < 3; ch += 1) {
            const uint32_t s = 0x101u * (uint32_t)src[i + ch];
            uint32_t d = 0x101u * (uint32_t)dst[i + ch];

            /* Convert dst from nonpremul to premul. */
            d = (d * da_in) / 0xFFFFu;

            d = ((s * sa) / 0xFFFFu) + ((d * ia) / 0xFFFFu);

            /* Convert dst from premul to nonpremul. */
            if (da != 0) d = (d * 0xFFFFu) / da;

            /* Convert from 16-bit color to 8-bit color. */
            dst[i + ch] = (uint8_t)(d >> 8);
        }

        dst[i + 3] = (uint8_t)(da >> 8);
    }
}

} /* namespace swizzle */

/// Convert pixel data in the given format to a freshly allocated RGBA
/// buffer. The caller owns the result; the input is not freed.
///
/// The input length must be a multiple of the format's bytes per
/// pixel; image loading validates data length against the image
/// dimensions before storing it.
inline Error rgbaFromFormat(Allocator alloc, command::Transmission::Format format,
                            const uint8_t *data, size_t data_len, uint8_t **out_ptr,
                            size_t *out_len) {
    /* Wisp: upstream's `switch` yields an error union that the `catch`
     * below narrows. Here each arm returns directly; the narrowing is
     * unnecessary because the replacement swizzles only ever fail with
     * OutOfMemory. */
    switch (format) {
    case command::Transmission::Format::rgba: {
        uint8_t *dup = alloc.dupe(data, data_len);
        if (dup == nullptr) return Error::OutOfMemory;
        *out_ptr = dup;
        *out_len = data_len;
        return Error::none;
    }
    case command::Transmission::Format::rgb:
        return swizzle::rgbToRgba(alloc, data, data_len, out_ptr, out_len);
    case command::Transmission::Format::gray:
        return swizzle::gToRgba(alloc, data, data_len, out_ptr, out_len);
    case command::Transmission::Format::gray_alpha:
        return swizzle::gaToRgba(alloc, data, data_len, out_ptr, out_len);

    /* PNG is decoded to RGBA during image loading. */
    case command::Transmission::Format::png: assert(false); break;
    }

    /* Wisp: upstream's `unreachable` for .png. NDEBUG drops the assert
     * above, so a defined, empty result is produced rather than falling
     * off the end of the function. */
    *out_ptr = nullptr;
    *out_len = 0;
    return Error::none;
}

/// Fill an RGBA buffer with the given background color.
inline void fillBackground(uint8_t *data, size_t data_len,
                           command::AnimationFrameLoading::Background bg) {
    /* Wisp: `@bitCast(bg)` of a little-endian `packed struct(u32)` whose
     * first field is `a`; graphics_command.hpp's Background::bitCast is
     * the inverse of this. */
    const uint32_t raw = (uint32_t)bg.a | ((uint32_t)bg.b << 8) | ((uint32_t)bg.g << 16) |
                         ((uint32_t)bg.r << 24);
    if (raw == 0) {
        memset(data, 0, data_len);
        return;
    }
    size_t i = 0;
    while (i + 4 <= data_len) {
        data[i + 0] = bg.r;
        data[i + 1] = bg.g;
        data[i + 2] = bg.b;
        data[i + 3] = bg.a;
        i += 4;
    }
}

/* Wisp: declared before its callers; upstream declares it after them. */
inline void composeRow(uint8_t *dst, size_t dst_len, const uint8_t *src, size_t src_len,
                       command::CompositionMode mode) {
    switch (mode) {
    case command::CompositionMode::overwrite:
        assert(dst_len == src_len);
        memcpy(dst, src, dst_len);
        break;
    case command::CompositionMode::alpha_blend:
        swizzle::rgbaSrcOver(dst, dst_len, src, src_len);
        break;
    }
}

/// Compose the `src` rectangle (src_width x src_height RGBA pixels)
/// onto the `dst` canvas (dst_width x dst_height RGBA pixels) with
/// its top-left corner at (x, y). Portions of the rectangle outside
/// the canvas are silently clipped, matching Kitty's a=f behavior.
inline void composeRect(uint8_t *dst, size_t dst_len, uint32_t dst_width, uint32_t dst_height,
                        const uint8_t *src, size_t src_len, uint32_t src_width,
                        uint32_t src_height, uint32_t x, uint32_t y,
                        command::CompositionMode mode) {
    /* Wisp: the slice lengths are only read by the bounds asserts below,
     * which NDEBUG drops. */
    (void)dst_len;
    (void)src_len;
    if (x >= dst_width || y >= dst_height) return;
    const size_t width = (size_t)(src_width < dst_width - x ? src_width : dst_width - x);
    const size_t height = (size_t)(src_height < dst_height - y ? src_height : dst_height - y);
    for (size_t row = 0; row < height; row += 1) {
        const size_t dst_off = (((size_t)y + row) * (size_t)dst_width + (size_t)x) * 4;
        const size_t src_off = row * (size_t)src_width * 4;
        /* Wisp: Zig's slice bounds check on `dst[dst_off..][0..width*4]`. */
        assert(dst_off + width * 4 <= dst_len);
        assert(src_off + width * 4 <= src_len);
        composeRow(dst + dst_off, width * 4, src + src_off, width * 4, mode);
    }
}

/// Compose a width x height rectangle between two full-size RGBA
/// canvases sharing the same `canvas_width` stride, reading from
/// (src_x, src_y) in `src` and writing at (dst_x, dst_y) in `dst`.
/// The caller must have validated that both rectangles are within
/// bounds; a=c reports out-of-bounds rectangles as errors rather
/// than clipping.
inline void composeCanvasRect(uint8_t *dst, size_t dst_len, const uint8_t *src, size_t src_len,
                              uint32_t canvas_width, uint32_t width, uint32_t height,
                              uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y,
                              command::CompositionMode mode) {
    /* Wisp: the slice lengths are only read by the bounds asserts below,
     * which NDEBUG drops. */
    (void)dst_len;
    (void)src_len;
    /* Wisp: Zig evaluates `for (0..height)` once; `height` is a value
     * parameter here so there is nothing for the body to mutate, but the
     * bound is snapshotted anyway per the conventions. */
    const size_t rows = (size_t)height;
    for (size_t row = 0; row < rows; row += 1) {
        const size_t dst_off =
            (((size_t)dst_y + row) * (size_t)canvas_width + (size_t)dst_x) * 4;
        const size_t src_off =
            (((size_t)src_y + row) * (size_t)canvas_width + (size_t)src_x) * 4;
        /* Wisp: Zig's slice bounds check. */
        assert(dst_off + (size_t)width * 4 <= dst_len);
        assert(src_off + (size_t)width * 4 <= src_len);
        composeRow(dst + dst_off, (size_t)width * 4, src + src_off, (size_t)width * 4, mode);
    }
}

} /* namespace graphics_pixel */
} /* namespace kitty */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_KITTY_GRAPHICS_PIXEL_HPP */
