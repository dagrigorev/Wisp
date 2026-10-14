/* Transliterated from the test blocks in Ghostty's
 * src/terminal/kitty/graphics_pixel.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's six tests: same names, same inputs, same assertions, in
 * upstream's order.
 *
 * Wisp, the test-harness seams:
 *   - `testing.expectEqualSlices(u8, ...)` is the local `EXPECT_BYTES`
 *     below, which compares length then bytes.
 *   - `testing.allocator`'s leak check is `zigstd::testing_state().live`,
 *     asserted by the trailing `zz_Wisp_no_leaks` test.
 *   - `var buf: [8]u8 = undefined` is a value-initialized local; the
 *     first `fillBackground` call writes every byte, so the initial
 *     contents are never read either way.
 *
 * Wisp, tests beyond upstream: the `swizzle` namespace in
 * graphics_pixel.hpp is original code standing in for wuffs (see that
 * file's header), so the Wisp_swizzle_* tests below exercise it
 * directly: each format conversion on known bytes, and the composite at
 * alpha 0, 255 and a mid value where the truncating rounding is
 * visible. The composite's four-pixel vector is taken verbatim from
 * upstream's own pkg/wuffs/src/swizzle.zig `rgbaSrcOver` test, so those
 * expected bytes are wuffs' real output rather than the port's.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "test_helpers.h"
#include "../terminal/kitty/graphics_pixel.hpp"
#include "../terminal/kitty/graphics_command.hpp"
#include "../zigstd/allocator.hpp"

using namespace wisp;

namespace px = wisp::terminal::kitty::graphics_pixel;
namespace cmdns = wisp::terminal::kitty::graphics;

typedef px::Error Error;
typedef cmdns::Transmission Transmission;
typedef cmdns::CompositionMode CompositionMode;
typedef cmdns::AnimationFrameLoading::Background Background;

static zigstd::Allocator testAlloc() { return zigstd::testing_allocator(); }

/* Wisp: testing.expectEqualSlices(u8, expected, actual). */
static int bytesEqual(const uint8_t *expected, size_t expected_len, const uint8_t *actual,
                      size_t actual_len) {
    if (expected_len != actual_len) return 0;
    if (expected_len == 0) return 1;
    return memcmp(expected, actual, expected_len) == 0;
}

#define EXPECT_BYTES(expected_arr, actual_ptr, actual_len)                                     \
    do {                                                                                       \
        const uint8_t *_e = (expected_arr);                                                    \
        const size_t _elen = sizeof(expected_arr);                                             \
        ASSERT_TRUE(bytesEqual(_e, _elen, (actual_ptr), (actual_len)));                        \
    } while (0)

/* ── Upstream's tests ────────────────────────────────────────────────── */

TEST(kitty_graphics_pixel, rgba_conversion) {
    const zigstd::Allocator alloc = testAlloc();

    {
        const uint8_t rgb[] = {1, 2, 3, 4, 5, 6};
        uint8_t *result = nullptr;
        size_t result_len = 0;
        ASSERT_TRUE(Error::none ==
                    px::rgbaFromFormat(alloc, Transmission::Format::rgb, rgb, sizeof rgb,
                                       &result, &result_len));
        const uint8_t expect[] = {1, 2, 3, 255, 4, 5, 6, 255};
        EXPECT_BYTES(expect, result, result_len);
        alloc.free(result, result_len);
    }

    {
        const uint8_t gray[] = {7, 8};
        uint8_t *result = nullptr;
        size_t result_len = 0;
        ASSERT_TRUE(Error::none ==
                    px::rgbaFromFormat(alloc, Transmission::Format::gray, gray, sizeof gray,
                                       &result, &result_len));
        const uint8_t expect[] = {7, 7, 7, 255, 8, 8, 8, 255};
        EXPECT_BYTES(expect, result, result_len);
        alloc.free(result, result_len);
    }

    {
        const uint8_t ga[] = {7, 100, 8, 200};
        uint8_t *result = nullptr;
        size_t result_len = 0;
        ASSERT_TRUE(Error::none ==
                    px::rgbaFromFormat(alloc, Transmission::Format::gray_alpha, ga, sizeof ga,
                                       &result, &result_len));
        const uint8_t expect[] = {7, 7, 7, 100, 8, 8, 8, 200};
        EXPECT_BYTES(expect, result, result_len);
        alloc.free(result, result_len);
    }

    {
        const uint8_t rgba[] = {1, 2, 3, 4};
        uint8_t *result = nullptr;
        size_t result_len = 0;
        ASSERT_TRUE(Error::none ==
                    px::rgbaFromFormat(alloc, Transmission::Format::rgba, rgba, sizeof rgba,
                                       &result, &result_len));
        EXPECT_BYTES(rgba, result, result_len);
        alloc.free(result, result_len);
    }
}

TEST(kitty_graphics_pixel, fill_background) {
    uint8_t buf[8] = {0};

    {
        Background bg;
        bg.r = 1;
        bg.g = 2;
        bg.b = 3;
        bg.a = 4;
        px::fillBackground(buf, sizeof buf, bg);
        const uint8_t expect[] = {1, 2, 3, 4, 1, 2, 3, 4};
        EXPECT_BYTES(expect, buf, sizeof buf);
    }

    {
        px::fillBackground(buf, sizeof buf, Background());
        const uint8_t expect[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        EXPECT_BYTES(expect, buf, sizeof buf);
    }
}

TEST(kitty_graphics_pixel, compose_rect_overwrite_with_clipping) {
    /* 2x2 canvas, compose a 2x1 rect at (1, 1): only the first pixel
     * of the rect fits, the rest clips off the right edge. */
    uint8_t dst[16];
    memset(dst, 0, sizeof dst);
    const uint8_t src[] = {1, 2, 3, 4, 5, 6, 7, 8};
    px::composeRect(dst, sizeof dst, 2, 2, src, sizeof src, 2, 1, 1, 1,
                    CompositionMode::overwrite);

    uint8_t expect[16];
    memset(expect, 0, sizeof expect);
    expect[12] = 1;
    expect[13] = 2;
    expect[14] = 3;
    expect[15] = 4;
    EXPECT_BYTES(expect, dst, sizeof dst);
}

TEST(kitty_graphics_pixel, compose_rect_entirely_out_of_bounds) {
    uint8_t dst[16];
    memset(dst, 9, sizeof dst);
    uint8_t src[4];
    memset(src, 1, sizeof src);
    px::composeRect(dst, sizeof dst, 2, 2, src, sizeof src, 1, 1, 2, 0,
                    CompositionMode::overwrite);
    px::composeRect(dst, sizeof dst, 2, 2, src, sizeof src, 1, 1, 0, 2,
                    CompositionMode::overwrite);
    uint8_t expect[16];
    memset(expect, 9, sizeof expect);
    EXPECT_BYTES(expect, dst, sizeof dst);
}

TEST(kitty_graphics_pixel, alpha_blend_source_over_semantics) {
    /* Opaque source overwrites exactly. */
    {
        uint8_t dst[4] = {10, 20, 30, 40};
        const uint8_t src[4] = {100, 110, 120, 255};
        px::composeRect(dst, sizeof dst, 1, 1, src, sizeof src, 1, 1, 0, 0,
                        CompositionMode::alpha_blend);
        const uint8_t expect[] = {100, 110, 120, 255};
        EXPECT_BYTES(expect, dst, sizeof dst);
    }

    /* Fully transparent source leaves the destination untouched
     * (like Kitty; see the module doc on rounding). */
    {
        uint8_t dst[4] = {10, 20, 30, 40};
        const uint8_t src[4] = {100, 110, 120, 0};
        px::composeRect(dst, sizeof dst, 1, 1, src, sizeof src, 1, 1, 0, 0,
                        CompositionMode::alpha_blend);
        const uint8_t expect[] = {10, 20, 30, 40};
        EXPECT_BYTES(expect, dst, sizeof dst);
    }

    /* 50% source over opaque destination. */
    {
        uint8_t dst[4] = {0, 0, 0, 255};
        const uint8_t src[4] = {255, 255, 255, 128};
        px::composeRect(dst, sizeof dst, 1, 1, src, sizeof src, 1, 1, 0, 0,
                        CompositionMode::alpha_blend);
        ASSERT_TRUE(255 == dst[3]);
        ASSERT_TRUE(128 == dst[0]);
    }

    /* Blending over a transparent destination yields the source
     * exactly, matching Kitty: wuffs passes the source through
     * untouched when the destination is transparent. */
    {
        uint8_t dst[4] = {0, 0, 0, 0};
        const uint8_t src[4] = {200, 100, 50, 128};
        px::composeRect(dst, sizeof dst, 1, 1, src, sizeof src, 1, 1, 0, 0,
                        CompositionMode::alpha_blend);
        const uint8_t expect[] = {200, 100, 50, 128};
        EXPECT_BYTES(expect, dst, sizeof dst);
    }
}

TEST(kitty_graphics_pixel, compose_canvas_rect) {
    /* 3x1 canvas: copy pixel at x=0 onto x=2. */
    uint8_t canvas[] = {1, 2, 3, 4, 0, 0, 0, 0, 9, 9, 9, 9};
    px::composeCanvasRect(canvas, sizeof canvas, canvas, sizeof canvas, 3, 1, 1, 0, 0, 2, 0,
                          CompositionMode::overwrite);
    const uint8_t expect[] = {1, 2, 3, 4, 0, 0, 0, 0, 1, 2, 3, 4};
    EXPECT_BYTES(expect, canvas, sizeof canvas);
}

/* ── Wisp: tests for the wuffs stand-in swizzles. ────────────────────── */

TEST(kitty_graphics_pixel, Wisp_swizzle_rgbToRgba) {
    const zigstd::Allocator alloc = testAlloc();

    /* Three pixels, including the 0x00/0xFF extremes, to pin the byte
     * order and the appended opaque alpha. */
    const uint8_t src[] = {0, 0, 0, 255, 255, 255, 10, 20, 30};
    uint8_t *out = nullptr;
    size_t out_len = 0;
    ASSERT_TRUE(Error::none == px::swizzle::rgbToRgba(alloc, src, sizeof src, &out, &out_len));
    ASSERT_TRUE(12 == out_len);
    const uint8_t expect[] = {0, 0, 0, 255, 255, 255, 255, 255, 10, 20, 30, 255};
    EXPECT_BYTES(expect, out, out_len);
    alloc.free(out, out_len);

    /* Empty input is a zero-length result, not an error. */
    out = nullptr;
    out_len = 123;
    ASSERT_TRUE(Error::none == px::swizzle::rgbToRgba(alloc, src, 0, &out, &out_len));
    ASSERT_TRUE(0 == out_len);
    alloc.free(out, out_len);
}

TEST(kitty_graphics_pixel, Wisp_swizzle_gToRgba) {
    const zigstd::Allocator alloc = testAlloc();

    const uint8_t src[] = {0, 1, 128, 255};
    uint8_t *out = nullptr;
    size_t out_len = 0;
    ASSERT_TRUE(Error::none == px::swizzle::gToRgba(alloc, src, sizeof src, &out, &out_len));
    ASSERT_TRUE(16 == out_len);
    const uint8_t expect[] = {
        0,   0,   0,   255, /* 0 */
        1,   1,   1,   255, /* 1 */
        128, 128, 128, 255, /* 128 */
        255, 255, 255, 255, /* 255 */
    };
    EXPECT_BYTES(expect, out, out_len);
    alloc.free(out, out_len);
}

TEST(kitty_graphics_pixel, Wisp_swizzle_gaToRgba) {
    const zigstd::Allocator alloc = testAlloc();

    /* The alpha byte is carried across untouched, including 0 — no
     * premultiplication, which is the whole point of upstream choosing
     * the YA_NONPREMUL source format. */
    const uint8_t src[] = {200, 0, 200, 128, 200, 255, 0, 77};
    uint8_t *out = nullptr;
    size_t out_len = 0;
    ASSERT_TRUE(Error::none == px::swizzle::gaToRgba(alloc, src, sizeof src, &out, &out_len));
    ASSERT_TRUE(16 == out_len);
    const uint8_t expect[] = {
        200, 200, 200, 0,   /* grey 200, alpha 0: colour is NOT zeroed */
        200, 200, 200, 128, /* grey 200, alpha 128 */
        200, 200, 200, 255, /* grey 200, opaque */
        0,   0,   0,   77,  /* grey 0, alpha 77 */
    };
    EXPECT_BYTES(expect, out, out_len);
    alloc.free(out, out_len);
}

TEST(kitty_graphics_pixel, Wisp_swizzle_rgbaSrcOver) {
    /* Wisp: this is upstream's own pkg/wuffs/src/swizzle.zig
     * `rgbaSrcOver` test vector, byte for byte — the four interesting
     * cases in one call. Its expected bytes are wuffs' real output, so
     * this is the check that the port's arithmetic and its truncating
     * rounding match the library it replaces.
     *
     * 50% white over opaque black, opaque over anything, transparent
     * source over anything, and anything over a transparent
     * destination (exact source passthrough). */
    uint8_t dst[] = {
        0,  0,  0,  255, /* opaque black */
        10, 20, 30, 40,  /* mostly transparent */
        10, 20, 30, 40,  /* mostly transparent */
        0,  0,  0,  0,   /* fully transparent */
    };
    const uint8_t src[] = {
        255, 255, 255, 128, /* 50% white */
        100, 110, 120, 255, /* opaque */
        100, 110, 120, 0,   /* fully transparent */
        200, 100, 50,  128, /* 50% */
    };
    px::swizzle::rgbaSrcOver(dst, sizeof dst, src, sizeof src);
    const uint8_t expect[] = {
        128, 128, 128, 255, /* mid value: 32896 >> 8 == 128, truncated */
        100, 110, 120, 255,
        10,  20,  30,  40,
        200, 100, 50,  128,
    };
    EXPECT_BYTES(expect, dst, sizeof dst);
}

TEST(kitty_graphics_pixel, Wisp_swizzle_rgbaSrcOver_alpha_extremes_and_rounding) {
    /* Alpha 0: the destination comes back unchanged. */
    {
        uint8_t dst[4] = {11, 22, 33, 44};
        const uint8_t src[4] = {255, 254, 253, 0};
        px::swizzle::rgbaSrcOver(dst, sizeof dst, src, sizeof src);
        const uint8_t expect[] = {11, 22, 33, 44};
        EXPECT_BYTES(expect, dst, sizeof dst);
    }

    /* Alpha 255: the source replaces the destination exactly. */
    {
        uint8_t dst[4] = {11, 22, 33, 44};
        const uint8_t src[4] = {7, 123, 251, 255};
        px::swizzle::rgbaSrcOver(dst, sizeof dst, src, sizeof src);
        const uint8_t expect[] = {7, 123, 251, 255};
        EXPECT_BYTES(expect, dst, sizeof dst);
    }

    /* Mid alpha over an opaque destination, where the truncation is
     * visible. The destination is opaque, so premultiplying and
     * un-premultiplying it are both identities and each channel is
     *     ((s*0x101 * sa) / 0xFFFF + (d*0x101 * ia) / 0xFFFF) >> 8
     * with sa = 0x101 * 128 = 32896 and ia = 0xFFFF - sa = 32639.
     *
     * The first channel of each pixel is the rounding witness:
     *   255 over 0:   32896 >> 8 is exactly 128.5, and truncating gives
     *                 128 where rounding half up would give 129.
     *   0 over 255:   32639 >> 8 is 127.49, giving 127 — so the
     *                 complementary pair sums to 255, not 256. */
    {
        uint8_t dst[8] = {
            0,   0,   0,   255, /* opaque black */
            255, 255, 255, 255, /* opaque white */
        };
        const uint8_t src[8] = {
            255, 100, 1,   128,
            0,   100, 254, 128,
        };
        px::swizzle::rgbaSrcOver(dst, sizeof dst, src, sizeof src);
        const uint8_t expect[] = {
            /* over black: (s*0x101*sa/0xFFFF) >> 8
             *   255 -> 32896 >> 8 == 128 (128.5, truncated)
             *   100 -> 12900 >> 8 == 50  (50.39)
             *   1   -> 128   >> 8 == 0   (0.5, truncated) */
            128, 50, 0, 255,
            /* over white: (s*0x101*sa/0xFFFF + 32639) >> 8
             *   0   -> 32639 >> 8 == 127 (127.49)
             *   100 -> 45539 >> 8 == 177 (177.88)
             *   254 -> 65405 >> 8 == 255 (255.49) */
            127, 177, 255, 255,
        };
        EXPECT_BYTES(expect, dst, sizeof dst);
    }
}

TEST(kitty_graphics_pixel, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
