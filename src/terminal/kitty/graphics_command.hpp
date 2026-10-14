/* Transliterated from Ghostty src/terminal/kitty/graphics_command.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * The Kitty graphics protocol APC command grammar: the key/value control
 * data parser and the typed command tree it builds.
 *
 * Wisp, differences in shape rather than behavior:
 *   - This file is ported in full. Upstream's kitty_graphics build option
 *     gates its *callers*, not this file's contents; nothing here is
 *     omitted. Note that apc.hpp still routes the 'G' identifier to the
 *     ignore state (it carries `kitty_graphics = false`); wiring that up
 *     is not this file's business.
 *   - Error unions are an `Error` enum return (`Error::none` on success)
 *     with the value through an out parameter. `error.Overflow` and
 *     `error.InvalidCharacter` are the two `std.fmt.parseInt` errors.
 *   - `?T` is a bool return plus an out parameter (KV::get, index,
 *     Command::transmission, Command::display), matching apc.hpp.
 *   - `union(Action)` Control is the `Action` tag plus one field per
 *     payload; `delete` is a C++ keyword so that field is `delete_`.
 *     Likewise `Delete.Action` is a `Tag` enum plus one field per payload.
 *   - `packed struct(u32)` Usage/Background are plain structs with an
 *     explicit bitCast, so the little-endian field order upstream relies
 *     on is spelled out rather than implied by the layout.
 *   - `lib.Enum(lib.target, ...)` is a plain `enum class`.
 *   - `*std.Io.Writer` is `std::string *`; writes cannot fail, so
 *     Response::encode has no error path.
 *   - `std.ArrayList(u8)` is zigstd::ArrayListUnmanaged<uint8_t>, and
 *     `[]const u8` is a pointer plus a length.
 *   - `simd.base64` is src/simd/base64.hpp, whose decode is a bool
 *     return plus the decoded length out.
 *   - log.warn has no sink yet and is a comment.
 *   - Upstream's declaration order is kept except that the payload types
 *     (CompositionMode, Transmission, Display, the animation structs and
 *     Delete) are hoisted above Parser/Response/Command, and
 *     Parser::parseString and Parser::complete are defined out of line
 *     after Command. C++ requires a definition before use; Zig does not.
 */

#pragma once
#ifndef WISP_TERMINAL_KITTY_GRAPHICS_COMMAND_HPP
#define WISP_TERMINAL_KITTY_GRAPHICS_COMMAND_HPP

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "../../simd/base64.hpp"
#include "../../zigstd/allocator.hpp"
#include "../../zigstd/array_list.hpp"

namespace wisp {
namespace terminal {
namespace kitty {
namespace graphics {

/* Wisp: the error set of every function in this file, flattened. The
 * parseInt errors (Overflow, InvalidCharacter) are named because
 * upstream's tests assert on error.Overflow specifically. */
enum class Error : uint8_t {
    none,
    OutOfMemory,
    InvalidFormat,
    InvalidData,
    Overflow,
    InvalidCharacter,
};

/* Wisp: std.fmt.parseInt(u32, buf, 10). Leading '+' is accepted and a
 * leading '-' is error.InvalidCharacter, as for any unsigned type.
 *
 * Wisp, departure: Zig's parseInt also accepts '_' as a digit separator.
 * That is not reproduced here — a '_' is error.InvalidCharacter — because
 * the Kitty control-data grammar can never deliver one (an unparsed
 * single non-digit byte is stored as its ASCII code before we get here,
 * and a multi-byte run containing '_' is only reachable from input no
 * client produces). No upstream test covers it. */
inline Error parseIntU32(const uint8_t *buf, size_t len, uint32_t *out) {
    if (len == 0) return Error::InvalidCharacter;
    size_t i = 0;
    if (buf[0] == '+') {
        i = 1;
    } else if (buf[0] == '-') {
        return Error::InvalidCharacter;
    }
    if (i >= len) return Error::InvalidCharacter;

    /* kv_temp is at most 11 bytes, so the accumulator cannot itself
     * overflow before the range check below. */
    uint64_t acc = 0;
    for (; i < len; i++) {
        const uint8_t c = buf[i];
        if (c < '0' || c > '9') return Error::InvalidCharacter;
        acc = acc * 10 + (uint64_t)(c - '0');
        if (acc > 0xFFFFFFFFull) return Error::Overflow;
    }
    *out = (uint32_t)acc;
    return Error::none;
}

/* Wisp: std.fmt.parseInt(i32, buf, 10). See parseIntU32 on '_'. */
inline Error parseIntI32(const uint8_t *buf, size_t len, int32_t *out) {
    if (len == 0) return Error::InvalidCharacter;
    size_t i = 0;
    bool negative = false;
    if (buf[0] == '+') {
        i = 1;
    } else if (buf[0] == '-') {
        i = 1;
        negative = true;
    }
    if (i >= len) return Error::InvalidCharacter;

    uint64_t acc = 0;
    for (; i < len; i++) {
        const uint8_t c = buf[i];
        if (c < '0' || c > '9') return Error::InvalidCharacter;
        acc = acc * 10 + (uint64_t)(c - '0');
        if (acc > (negative ? 2147483648ull : 2147483647ull)) return Error::Overflow;
    }
    /* Wisp: negating through the unsigned type so -2147483648 is not UB. */
    *out = negative ? (int32_t)(uint32_t)(0u - (uint32_t)acc) : (int32_t)(uint32_t)acc;
    return Error::none;
}

/* Wisp: @bitCast between i32 and u32. */
inline uint32_t bitCastToU32(int32_t v) {
    uint32_t r;
    memcpy(&r, &v, sizeof r);
    return r;
}
inline int32_t bitCastToI32(uint32_t v) {
    int32_t r;
    memcpy(&r, &v, sizeof r);
    return r;
}

/// The key-value pairs for the control information for a command. The
/// recognized keys are always single ASCII letters and the values are either
/// single characters or 32-bit unsigned integers. Unknown non-letter keys are
/// ignored, matching how unknown letter keys are ignored when building a
/// command.
///
/// This is deliberately a dense value table plus a presence bitmap. There are
/// only 52 possible keys(technically KIP has fever than 52 possible .. at the moment of writing), so a lookup is one machine-word bit test followed by
/// an indexed load. The values are undefined until their presence bit is set.
///
/// Other alternatives were considered and should probably be revisited in the
/// future. Some of them were:
/// A `[52]?u32` table is 416 bytes versus 216 bytes for this layout, and
/// its larger copies erased the smaller lookup code.
/// `std.EnumMap` added optional-return lowering,
/// `std.StaticBitSet(52)` kept the same size but generated larger code
/// for the packed mask. Passing the table by pointer removed the copies
/// but did not improve end-to-end throughput. Keep this representation
/// unless a new benchmark shows an improvement in the zig compilers codegen.
///
/// For the value itself: if it is a single printable ASCII character it is the
/// ASCII code. Otherwise, it is parsed as a 32-bit unsigned integer.
struct KV {
    uint32_t values[52];
    uint64_t present; /* = 0 */

    /* Wisp: `values: [52]u32 = undefined`. Value-initialized here: upstream
     * relies on the invariant that a slot is only read once its presence bit
     * is set, which C++ would not keep if the port ever read one early. */
    KV() : present(0) {
        for (size_t i = 0; i < 52; i++) values[i] = 0;
    }

    /* Wisp: `?u6` is -1 for null. */
    static int index(uint8_t key) {
        if (key >= 'a' && key <= 'z') return (int)(key - 'a');
        if (key >= 'A' && key <= 'Z') return (int)(26 + key - 'A');
        return -1;
    }

    bool get(uint8_t key, uint32_t *out) const {
        const int idx = index(key);
        if (idx < 0) return false;
        if ((present & ((uint64_t)1 << idx)) == 0) return false;
        *out = values[idx];
        return true;
    }

    void put(uint8_t key, uint32_t value) {
        const int idx = index(key);
        if (idx < 0) return;
        values[idx] = value;
        present |= (uint64_t)1 << idx;
    }
};

/* Wisp: upstream declares CompositionMode at the end of the file; it is
 * hoisted here because the animation structs use it by value. */
enum class CompositionMode : uint8_t {
    alpha_blend, /* 0 */
    overwrite,   /* 1 */
};

struct Transmission {
    /* Wisp: hoisted ahead of the fields; Zig has no ordering requirement. */
    enum class Format : uint8_t {
        rgb,  /* 24 */
        rgba, /* 32 */
        png,  /* 100 */
        /* The following are not supported directly via the protocol
         * but they are formats that a png may decode to that we
         * support. */
        gray_alpha,
        gray,
    };

    enum class Medium : uint8_t {
        direct,         /* d */
        file,           /* f */
        temporary_file, /* t */
        shared_memory,  /* s */
    };

    enum class Compression : uint8_t {
        none,
        zlib_deflate, /* z */
    };

    /// Usage hints allow for optimising resource consumption strategies.
    ///
    /// https://sw.kovidgoyal.net/kitty/graphics-protocol/#usage-hints
    ///
    /// Wisp: `packed struct(u32)`. The padding bits are kept so the
    /// bit-cast is lossless, as upstream's `_padding: u31` is.
    struct Usage {
        /// Image with this usage hint is assumed to be used for only a short
        /// time, so may be evicted before other images if memory pressure is
        /// encountered.
        bool transient;  /* = false */
        uint32_t _padding; /* u31 = 0 */

        Usage() : transient(false), _padding(0) {}

        /* Wisp: @bitCast(v); `transient` is bit 0. */
        static Usage bitCast(uint32_t v) {
            Usage r;
            r.transient = (v & 1) != 0;
            r._padding = v >> 1;
            return r;
        }
    };

    Format format;        /* = .rgba, f */
    /// Set when "f" carried a value we don't recognize. Kitty rejects these
    /// while handling the command so the EINVAL response can carry the image
    /// id, so we defer to `LoadingImage.init`. `format` is meaningless when
    /// this is set.
    bool format_unknown;  /* = false */
    Medium medium;        /* = .direct, t */
    uint32_t width;       /* = 0, s */
    uint32_t height;      /* = 0, v */
    uint32_t size;        /* = 0, S */
    uint32_t offset;      /* = 0, O */
    uint32_t image_id;    /* = 0, i */
    uint32_t image_number; /* = 0, I */
    uint32_t placement_id; /* = 0, p */
    Compression compression; /* = .none, o */
    bool more_chunks;     /* = false, m */
    Usage usage;          /* = .default, N */

    Transmission()
        : format(Format::rgba), format_unknown(false), medium(Medium::direct), width(0), height(0), size(0),
          offset(0), image_id(0), image_number(0), placement_id(0), compression(Compression::none),
          more_chunks(false), usage() {}

    static uint8_t formatBpp(Format format_) {
        switch (format_) {
        case Format::gray: return 1;
        case Format::gray_alpha: return 2;
        case Format::rgb: return 3;
        case Format::rgba: return 4;
        case Format::png: assert(false); /* Must be validated before */
        }
        /* Wisp: upstream's `unreachable`. NDEBUG drops the assert above, so
         * a defined value is returned rather than reading past the switch. */
        return 0;
    }

    static Error parse(const KV &kv, Transmission *out) {
        Transmission result;
        uint32_t v;
        if (kv.get('f', &v)) {
            switch (v) {
            case 24: result.format = Format::rgb; break;
            case 0:
            case 32: result.format = Format::rgba; break;
            case 100: result.format = Format::png; break;
            default:
                /* Defer returning an error until the image is processed. */
                result.format_unknown = true;
                result.format = Format::rgba;
                break;
            }
        }

        if (kv.get('t', &v)) {
            if (v > 0xFF) return Error::InvalidFormat; /* std.math.cast(u8, v) */
            const uint8_t c = (uint8_t)v;
            switch (c) {
            case 'd': result.medium = Medium::direct; break;
            case 'f': result.medium = Medium::file; break;
            case 't': result.medium = Medium::temporary_file; break;
            case 's': result.medium = Medium::shared_memory; break;
            default: return Error::InvalidFormat;
            }
        }

        if (kv.get('s', &v)) {
            result.width = v;
        }

        if (kv.get('v', &v)) {
            result.height = v;
        }

        if (kv.get('S', &v)) {
            result.size = v;
        }

        if (kv.get('O', &v)) {
            result.offset = v;
        }

        if (kv.get('i', &v)) {
            result.image_id = v;
        }

        if (kv.get('I', &v)) {
            result.image_number = v;
        }

        if (kv.get('p', &v)) {
            result.placement_id = v;
        }

        if (kv.get('o', &v)) {
            if (v > 0xFF) return Error::InvalidFormat; /* std.math.cast(u8, v) */
            const uint8_t c = (uint8_t)v;
            switch (c) {
            case 'z': result.compression = Compression::zlib_deflate; break;
            default: return Error::InvalidFormat;
            }
        }

        /* If the transmission medium is a local-only medium, ignore the "m"
         * key. The Kitty graphics protocol specification does not explicitly
         * call out this behavior (although the "m" key is only mentioned in
         * connection with remote clients) but that's how it's implemented in
         * Kitty and at least one client (mpv) relies on this behavior when
         * using the shared memory transmission medium.
         *
         * https://sw.kovidgoyal.net/kitty/graphics-protocol/#the-transmission-medium
         * https://github.com/kovidgoyal/kitty/blob/ccc3bee9af794f332b4e9adcd714a649f639c397/kitty/graphics.c#L547-L592
         */
        if (result.medium == Medium::direct) {
            if (kv.get('m', &v)) {
                result.more_chunks = v > 0;
            }
        }

        if (kv.get('N', &v)) {
            result.usage = Usage::bitCast(v);
        }

        *out = result;
        return Error::none;
    }
};

struct Display {
    enum class CursorMovement : uint8_t {
        after, /* 0 */
        none,  /* 1 */
    };

    uint32_t image_id;            /* = 0, i */
    uint32_t image_number;        /* = 0, I */
    uint32_t placement_id;        /* = 0, p */
    uint32_t x;                   /* = 0, x */
    uint32_t y;                   /* = 0, y */
    uint32_t width;               /* = 0, w */
    uint32_t height;              /* = 0, h */
    uint32_t x_offset;            /* = 0, X */
    uint32_t y_offset;            /* = 0, Y */
    uint32_t columns;             /* = 0, c */
    uint32_t rows;                /* = 0, r */
    CursorMovement cursor_movement; /* = .after, C */
    bool virtual_placement;       /* = false, U */
    uint32_t parent_id;           /* = 0, P */
    uint32_t parent_placement_id; /* = 0, Q */
    int32_t horizontal_offset;    /* = 0, H */
    int32_t vertical_offset;      /* = 0, V */
    int32_t z;                    /* = 0, z */

    Display()
        : image_id(0), image_number(0), placement_id(0), x(0), y(0), width(0), height(0), x_offset(0),
          y_offset(0), columns(0), rows(0), cursor_movement(CursorMovement::after), virtual_placement(false),
          parent_id(0), parent_placement_id(0), horizontal_offset(0), vertical_offset(0), z(0) {}

    /* Wisp: upstream declares `!Display` but its inferred error set is
     * empty — no key here can fail — so this returns by value. */
    static Display parse(const KV &kv) {
        Display result;
        uint32_t v;

        if (kv.get('i', &v)) {
            result.image_id = v;
        }

        if (kv.get('I', &v)) {
            result.image_number = v;
        }

        if (kv.get('p', &v)) {
            result.placement_id = v;
        }

        if (kv.get('x', &v)) {
            result.x = v;
        }

        if (kv.get('y', &v)) {
            result.y = v;
        }

        if (kv.get('w', &v)) {
            result.width = v;
        }

        if (kv.get('h', &v)) {
            result.height = v;
        }

        if (kv.get('X', &v)) {
            result.x_offset = v;
        }

        if (kv.get('Y', &v)) {
            result.y_offset = v;
        }

        if (kv.get('c', &v)) {
            result.columns = v;
        }

        if (kv.get('r', &v)) {
            result.rows = v;
        }

        if (kv.get('C', &v)) {
            /* Kitty only tests this against 1 so any other value moves the cursor. */
            result.cursor_movement = v == 1 ? CursorMovement::none : CursorMovement::after;
        }

        if (kv.get('U', &v)) {
            /* Kitty stores this in a bool, so any non-zero value is virtual. */
            result.virtual_placement = v != 0;
        }

        if (kv.get('z', &v)) {
            /* We can bitcast here because of how we parse it earlier. */
            result.z = bitCastToI32(v);
        }

        if (kv.get('P', &v)) {
            result.parent_id = v;
        }

        if (kv.get('Q', &v)) {
            result.parent_placement_id = v;
        }

        if (kv.get('H', &v)) {
            /* We can bitcast here because of how we parse it earlier. */
            result.horizontal_offset = bitCastToI32(v);
        }

        if (kv.get('V', &v)) {
            /* We can bitcast here because of how we parse it earlier. */
            result.vertical_offset = bitCastToI32(v);
        }

        return result;
    }
};

struct AnimationFrameLoading {
    /// The canvas background color as a 32-bit RGBA value where R is
    /// the most significant byte (0xRRGGBBAA), e.g. Y=4278190335
    /// (0xff0000ff) is opaque red. Bit-cast from the little-endian
    /// u32 value, so the field order is reversed from the byte order.
    struct Background {
        uint8_t a; /* = 0 */
        uint8_t b; /* = 0 */
        uint8_t g; /* = 0 */
        uint8_t r; /* = 0 */

        Background() : a(0), b(0), g(0), r(0) {}

        /* Wisp: @bitCast(v) of a little-endian packed struct(u32): the
         * first field holds the least significant byte. */
        static Background bitCast(uint32_t v) {
            Background s;
            s.a = (uint8_t)(v & 0xFF);
            s.b = (uint8_t)((v >> 8) & 0xFF);
            s.g = (uint8_t)((v >> 16) & 0xFF);
            s.r = (uint8_t)((v >> 24) & 0xFF);
            return s;
        }
    };

    /// Frame data is transmitted exactly like image data, so an a=f
    /// command carries a full set of transmission keys: medium (t),
    /// format (f), the frame rectangle size (s/v), chunking (m), etc.
    /// The image id/number here identify the (existing) image the
    /// frame belongs to.
    Transmission transmission; /* = .{} */

    /// The left/top edge (pixels) of the rectangle within the frame
    /// that the transmitted data updates.
    uint32_t x; /* = 0, x */
    uint32_t y; /* = 0, y */

    /// The 1-based frame number whose pixels serve as the base canvas
    /// for a newly created frame. Zero means no base frame: the
    /// canvas is filled with `background` instead.
    uint32_t create_frame; /* = 0, c */

    /// The 1-based frame number of an existing frame to edit. Zero
    /// (or any value past the next frame number) creates a new frame.
    uint32_t edit_frame; /* = 0, r */

    /// The gap in milliseconds before the next frame is shown. For
    /// new frames, zero selects the default gap and a negative value
    /// creates a gapless (never displayed) frame. For edits, zero
    /// leaves the gap unchanged.
    int32_t gap_ms; /* = 0, z */

    CompositionMode composition_mode; /* = .alpha_blend, X */
    Background background;            /* = .{}, Y */

    AnimationFrameLoading()
        : transmission(), x(0), y(0), create_frame(0), edit_frame(0), gap_ms(0),
          composition_mode(CompositionMode::alpha_blend), background() {}

    static Error parse(const KV &kv, AnimationFrameLoading *out) {
        AnimationFrameLoading result;
        {
            const Error err = Transmission::parse(kv, &result.transmission);
            if (err != Error::none) return err;
        }

        uint32_t v;
        if (kv.get('x', &v)) {
            result.x = v;
        }

        if (kv.get('y', &v)) {
            result.y = v;
        }

        if (kv.get('c', &v)) {
            result.create_frame = v;
        }

        if (kv.get('r', &v)) {
            result.edit_frame = v;
        }

        if (kv.get('z', &v)) {
            /* 'z' is one of the keys Parser.finishValue parses as an
             * i32, so the u32 in the KV table is the bit pattern of
             * that i32 and this reconstructs it exactly. */
            result.gap_ms = bitCastToI32(v);
        }

        /* Tested only against 1 like Kitty. */
        if (kv.get('X', &v)) {
            result.composition_mode = v == 1 ? CompositionMode::overwrite : CompositionMode::alpha_blend;
        }

        if (kv.get('Y', &v)) {
            result.background = Background::bitCast(v);
        }

        *out = result;
        return Error::none;
    }
};

struct AnimationFrameComposition {
    uint32_t image_id;     /* = 0, i */
    uint32_t image_number; /* = 0, I */
    uint32_t placement_id; /* = 0, p */

    /// The 1-based frame number being edited (the destination).
    uint32_t dest_frame; /* = 0, c */

    /// The 1-based frame number whose pixels are composed onto the
    /// destination (the source).
    uint32_t source_frame; /* = 0, r */

    /// The left/top edge (pixels) of the destination rectangle.
    uint32_t x; /* = 0, x */
    uint32_t y; /* = 0, y */

    /// The size of the rectangle to compose. Zero selects the full
    /// image width/height.
    uint32_t width;  /* = 0, w */
    uint32_t height; /* = 0, h */

    /// The left/top edge (pixels) of the source rectangle.
    uint32_t left_edge; /* = 0, X */
    uint32_t top_edge;  /* = 0, Y */

    CompositionMode composition_mode; /* = .alpha_blend, C */

    AnimationFrameComposition()
        : image_id(0), image_number(0), placement_id(0), dest_frame(0), source_frame(0), x(0), y(0), width(0),
          height(0), left_edge(0), top_edge(0), composition_mode(CompositionMode::alpha_blend) {}

    /* Cannot fail: every key is stored as parsed, with no range
     * validation (Kitty range-checks these while handling the
     * command; see the comment above the execution-deferral tests). */
    static AnimationFrameComposition parse(const KV &kv) {
        AnimationFrameComposition result;
        uint32_t v;

        if (kv.get('i', &v)) {
            result.image_id = v;
        }

        if (kv.get('I', &v)) {
            result.image_number = v;
        }

        if (kv.get('p', &v)) {
            result.placement_id = v;
        }

        if (kv.get('c', &v)) {
            result.dest_frame = v;
        }

        if (kv.get('r', &v)) {
            result.source_frame = v;
        }

        if (kv.get('x', &v)) {
            result.x = v;
        }

        if (kv.get('y', &v)) {
            result.y = v;
        }

        if (kv.get('w', &v)) {
            result.width = v;
        }

        if (kv.get('h', &v)) {
            result.height = v;
        }

        if (kv.get('X', &v)) {
            result.left_edge = v;
        }

        if (kv.get('Y', &v)) {
            result.top_edge = v;
        }

        if (kv.get('C', &v)) {
            /* Kitty tests this against zero for compose so any non-zero overwrites */
            result.composition_mode = v == 0 ? CompositionMode::alpha_blend : CompositionMode::overwrite;
        }

        return result;
    }
};

struct AnimationControl {
    enum class AnimationAction : uint8_t {
        invalid,  /* 0 */
        stop,     /* 1 */
        run_wait, /* 2 */
        run,      /* 3 */
    };

    uint32_t image_id;       /* = 0, i */
    uint32_t image_number;   /* = 0, I */
    uint32_t placement_id;   /* = 0, p */
    AnimationAction action;  /* = .invalid, s */

    /// The 1-based frame number whose gap is set from gap_ms.
    uint32_t frame; /* = 0, r */

    /// The new gap for `frame` in milliseconds. Zero is ignored and a
    /// negative value makes the frame gapless.
    int32_t gap_ms; /* = 0, z */

    /// The 1-based frame number to make current (client-driven
    /// animation). Zero is ignored.
    uint32_t current_frame; /* = 0, c */

    /// The loop count: zero is ignored, 1 loops forever, and any
    /// larger value plays that many minus one loops.
    uint32_t loops; /* = 0, v */

    AnimationControl()
        : image_id(0), image_number(0), placement_id(0), action(AnimationAction::invalid), frame(0), gap_ms(0),
          current_frame(0), loops(0) {}

    /* Cannot fail: every key is stored as parsed. Out-of-range values
     * (frame numbers, states) are silently ignored during execution,
     * matching Kitty. */
    static AnimationControl parse(const KV &kv) {
        AnimationControl result;
        uint32_t v;

        if (kv.get('i', &v)) {
            result.image_id = v;
        }

        if (kv.get('I', &v)) {
            result.image_number = v;
        }

        if (kv.get('p', &v)) {
            result.placement_id = v;
        }

        if (kv.get('s', &v)) {
            /* Kitty ignores values it doesn't know, leaving the animation
             * state untouched, which is what `.invalid` means here. */
            switch (v) {
            case 1: result.action = AnimationAction::stop; break;
            case 2: result.action = AnimationAction::run_wait; break;
            case 3: result.action = AnimationAction::run; break;
            default: result.action = AnimationAction::invalid; break;
            }
        }

        if (kv.get('r', &v)) {
            result.frame = v;
        }

        if (kv.get('z', &v)) {
            /* 'z' is one of the keys Parser.finishValue parses as an
             * i32, so the u32 in the KV table is the bit pattern of
             * that i32 and this reconstructs it exactly. */
            result.gap_ms = bitCastToI32(v);
        }

        if (kv.get('c', &v)) {
            result.current_frame = v;
        }

        if (kv.get('v', &v)) {
            result.loops = v;
        }

        return result;
    }
};

struct Delete {
    /* Wisp: `union(enum)` is a Tag plus one field per payload. */
    struct Action {
        struct AnimationFrames {
            bool delete_;          /* = false, uppercase */
            uint32_t image_id;     /* = 0, i */
            uint32_t image_number; /* = 0, I */
            uint32_t frame;        /* = 0, r */

            AnimationFrames() : delete_(false), image_id(0), image_number(0), frame(0) {}
        };

        struct Id {
            bool delete_;          /* = false, uppercase */
            uint32_t image_id;     /* = 0, i */
            uint32_t placement_id; /* = 0, p */

            Id() : delete_(false), image_id(0), placement_id(0) {}
        };

        struct Newest {
            bool delete_;          /* = false, uppercase */
            uint32_t image_number; /* = 0, I */
            uint32_t placement_id; /* = 0, p */

            Newest() : delete_(false), image_number(0), placement_id(0) {}
        };

        struct IntersectCell {
            bool delete_; /* = false, uppercase */
            uint32_t x;   /* = 0, x */
            uint32_t y;   /* = 0, y */

            IntersectCell() : delete_(false), x(0), y(0) {}
        };

        struct IntersectCellZ {
            bool delete_; /* = false, uppercase */
            uint32_t x;   /* = 0, x */
            uint32_t y;   /* = 0, y */
            int32_t z;    /* = 0, z */

            IntersectCellZ() : delete_(false), x(0), y(0), z(0) {}
        };

        struct Range {
            bool delete_;   /* = false, uppercase */
            uint32_t first; /* = 0, x */
            uint32_t last;  /* = 0, y */

            Range() : delete_(false), first(0), last(0) {}
        };

        struct Column {
            bool delete_; /* = false, uppercase */
            uint32_t x;   /* = 0, x */

            Column() : delete_(false), x(0) {}
        };

        struct Row {
            bool delete_; /* = false, uppercase */
            uint32_t y;   /* = 0, y */

            Row() : delete_(false), y(0) {}
        };

        struct Z {
            bool delete_; /* = false, uppercase */
            int32_t z;    /* = 0, z */

            Z() : delete_(false), z(0) {}
        };

        enum class Tag : uint8_t {
            all,              /* a/A */
            id,               /* i/I */
            newest,           /* n/N */
            intersect_cursor, /* c/C */
            animation_frames, /* f/F */
            intersect_cell,   /* p/P */
            intersect_cell_z, /* q/Q */
            range,            /* r/R */
            column,           /* x/X */
            row,              /* y/Y */
            z,                /* z/Z */
        };

        Tag tag;

        /* a/A */
        bool all;

        /* i/I */
        Id id;

        /* n/N */
        Newest newest;

        /* c/C, */
        bool intersect_cursor;

        /* f/F */
        AnimationFrames animation_frames;

        /* p/P */
        IntersectCell intersect_cell;

        /* q/Q */
        IntersectCellZ intersect_cell_z;

        /* r/R */
        Range range;

        /* x/X */
        Column column;

        /* y/Y */
        Row row;

        /* z/Z */
        Z z;

        Action() : tag(Tag::all), all(false), id(), newest(), intersect_cursor(false), animation_frames(),
                   intersect_cell(), intersect_cell_z(), range(), column(), row(), z() {}

        static Error parse(const KV &kv, Action *out) {
            uint8_t what;
            {
                uint32_t value;
                if (!kv.get('d', &value)) {
                    what = 'a';
                } else {
                    if (value > 0xFF) return Error::InvalidFormat; /* std.math.cast(u8, value) */
                    what = (uint8_t)value;
                }
            }

            uint32_t v;
            switch (what) {
            case 'a':
            case 'A': {
                Action result;
                result.tag = Tag::all;
                result.all = what == 'A';
                *out = result;
                return Error::none;
            }

            case 'i':
            case 'I': {
                Action result;
                result.tag = Tag::id;
                result.id.delete_ = what == 'I';
                if (kv.get('i', &v)) {
                    result.id.image_id = v;
                }
                if (kv.get('p', &v)) {
                    result.id.placement_id = v;
                }

                *out = result;
                return Error::none;
            }

            case 'n':
            case 'N': {
                Action result;
                result.tag = Tag::newest;
                result.newest.delete_ = what == 'N';
                if (kv.get('I', &v)) {
                    result.newest.image_number = v;
                }
                if (kv.get('p', &v)) {
                    result.newest.placement_id = v;
                }

                *out = result;
                return Error::none;
            }

            case 'c':
            case 'C': {
                Action result;
                result.tag = Tag::intersect_cursor;
                result.intersect_cursor = what == 'C';
                *out = result;
                return Error::none;
            }

            case 'f':
            case 'F': {
                Action result;
                result.tag = Tag::animation_frames;
                result.animation_frames.delete_ = what == 'F';
                if (kv.get('i', &v)) {
                    result.animation_frames.image_id = v;
                }
                if (kv.get('I', &v)) {
                    result.animation_frames.image_number = v;
                }
                if (kv.get('r', &v)) {
                    result.animation_frames.frame = v;
                }

                *out = result;
                return Error::none;
            }

            case 'p':
            case 'P': {
                Action result;
                result.tag = Tag::intersect_cell;
                result.intersect_cell.delete_ = what == 'P';
                if (kv.get('x', &v)) {
                    result.intersect_cell.x = v;
                }
                if (kv.get('y', &v)) {
                    result.intersect_cell.y = v;
                }

                *out = result;
                return Error::none;
            }

            case 'q':
            case 'Q': {
                Action result;
                result.tag = Tag::intersect_cell_z;
                result.intersect_cell_z.delete_ = what == 'Q';
                if (kv.get('x', &v)) {
                    result.intersect_cell_z.x = v;
                }
                if (kv.get('y', &v)) {
                    result.intersect_cell_z.y = v;
                }
                if (kv.get('z', &v)) {
                    /* We can bitcast here because of how we parse it earlier. */
                    result.intersect_cell_z.z = bitCastToI32(v);
                }

                *out = result;
                return Error::none;
            }

            case 'r':
            case 'R': {
                /* Both bounds default to zero when omitted and no
                 * validation is performed; an inverted or zero range
                 * simply matches no images. */
                Action result;
                result.tag = Tag::range;
                result.range.delete_ = what == 'R';
                if (kv.get('x', &v)) {
                    result.range.first = v;
                }
                if (kv.get('y', &v)) {
                    result.range.last = v;
                }

                *out = result;
                return Error::none;
            }

            case 'x':
            case 'X': {
                Action result;
                result.tag = Tag::column;
                result.column.delete_ = what == 'X';
                if (kv.get('x', &v)) {
                    result.column.x = v;
                }

                *out = result;
                return Error::none;
            }

            case 'y':
            case 'Y': {
                Action result;
                result.tag = Tag::row;
                result.row.delete_ = what == 'Y';
                if (kv.get('y', &v)) {
                    result.row.y = v;
                }

                *out = result;
                return Error::none;
            }

            case 'z':
            case 'Z': {
                Action result;
                result.tag = Tag::z;
                result.z.delete_ = what == 'Z';
                if (kv.get('z', &v)) {
                    /* We can bitcast here because of how we parse it earlier. */
                    result.z.z = bitCastToI32(v);
                }

                *out = result;
                return Error::none;
            }

            default: return Error::InvalidFormat;
            }
        }
    };

    uint32_t image_id;     /* = 0, i */
    uint32_t image_number; /* = 0, I */
    uint32_t placement_id; /* = 0, p */
    Action action;

    Delete() : image_id(0), image_number(0), placement_id(0), action() {}

    static Error parse(const KV &kv, Delete *out) {
        Delete result;
        uint32_t v;
        result.image_id = kv.get('i', &v) ? v : 0;
        result.image_number = kv.get('I', &v) ? v : 0;
        result.placement_id = kv.get('p', &v) ? v : 0;
        const Error err = Action::parse(kv, &result.action);
        if (err != Error::none) return err;
        *out = result;
        return Error::none;
    }
};

struct Command;

/// Command parser parses the Kitty graphics protocol escape sequence.
struct Parser {
    enum class State : uint8_t {
        /// Parsing k/v pairs. The "ignore" variants are in that state
        /// but ignore any data because we know they're invalid.
        control_key,
        control_key_ignore,
        control_value,
        control_value_ignore,

        /// Collecting the data payload blob.
        data,
    };

    zigstd::Allocator alloc;

    /// This is the list of KV pairs that we're building up.
    KV kv;

    /// This is used as a buffer to store the key/value of a KV pair. The value
    /// of a KV pair is at most a 32-bit integer which at most is 10 characters
    /// (4294967295), plus one character for the sign bit on signed ints.
    uint8_t kv_temp[11];
    /* Wisp: u4. Never reaches 12, because accumulateValue resets it there. */
    uint8_t kv_temp_len;
    uint8_t kv_current; /* Current kv key */

    /// This is the list we use to collect the bytes from the data payload.
    /// The Kitty Graphics protocol specification seems to imply that the
    /// payload content of a single command should never exceed 4096 bytes,
    /// but Kitty itself supports larger payloads, so we use an ArrayList
    /// here instead of a fixed buffer so that we can too.
    zigstd::ArrayListUnmanaged<uint8_t> data;

    /// Maximum bytes the data payload can take. This is to prevent
    /// malicious input from causing us to allocate too much memory.
    size_t max_bytes;

    /// Internal state for parsing.
    State state;

    Parser()
        : alloc(zigstd::c_allocator()), kv(), kv_temp_len(0), kv_current(0), data(), max_bytes(0),
          state(State::control_key) {
        /* Wisp: `kv_temp = undefined`. Value-initialized: upstream relies on
         * the invariant that only kv_temp[0..kv_temp_len] is ever read, and
         * the valgrind re-poisoning in `init` has no counterpart here. */
        memset(kv_temp, 0, sizeof kv_temp);
    }

    /// Initialize the parser. The allocator given will be used for both
    /// temporary data and long-lived values such as the final image blob.
    static Parser init(zigstd::Allocator alloc_, size_t max_bytes_) {
        Parser result;
        result.alloc = alloc_;
        result.data = zigstd::ArrayListUnmanaged<uint8_t>(); /* .empty */
        result.kv = KV();
        result.kv_temp_len = 0;
        result.kv_current = 0;
        result.max_bytes = max_bytes_;
        result.state = State::control_key;
        return result;
    }

    void deinit() { data.deinit(alloc); }

    /// Parse a complete command string.
    static Error parseString(zigstd::Allocator alloc_, const uint8_t *data_, size_t data_len, Command *out);

    /// Feed a single byte to the parser.
    ///
    /// The first byte to start parsing should be the byte immediately following
    /// the "G" in the APC sequence, i.e. "\x1b_G123" the first byte should
    /// be "1".
    Error feed(uint8_t c) {
        switch (state) {
        case State::control_key:
            switch (c) {
            /* '=' means the key is complete and we're moving to the value. */
            case '=':
                if (kv_temp_len != 1) {
                    /* All control keys are a single character right now so
                     * if we're not a single character just ignore follow-up
                     * data. */
                    state = State::control_value_ignore;
                    kv_temp_len = 0;
                } else {
                    kv_current = kv_temp[0];
                    kv_temp_len = 0;
                    state = State::control_value;
                }
                return Error::none;

            /* This can be encountered if we have a sequence with no
             * control data, only payload data (i.e. "\x1b_G;<data>").
             *
             * Kitty treats this as valid so we do as well. */
            case ';': state = State::data; return Error::none;

            default: return accumulateValue(c, State::control_key_ignore);
            }

        case State::control_key_ignore:
            switch (c) {
            case '=': state = State::control_value_ignore; return Error::none;
            default: return Error::none;
            }

        case State::control_value:
            switch (c) {
            case ',': return finishValue(State::control_key); /* move to next key */
            case ';': return finishValue(State::data);        /* move to data */
            default: return accumulateValue(c, State::control_value_ignore);
            }

        case State::control_value_ignore:
            switch (c) {
            case ',': state = State::control_key_ignore; return Error::none;
            case ';': state = State::data; return Error::none;
            default: return Error::none;
            }

        case State::data:
            if (data.items_len >= max_bytes) return Error::OutOfMemory;
            if (!data.append(alloc, c)) return Error::OutOfMemory;
            return Error::none;
        }
        return Error::none;
    }

    /// Feed a slice of bytes to the parser. This is equivalent to
    /// calling feed for each byte in order, but once we're in the data
    /// state the remainder of the slice is appended in bulk, avoiding
    /// per-byte overhead for large payloads.
    Error feedSlice(const uint8_t *bytes, size_t bytes_len) {
        const uint8_t *rem = bytes;
        size_t rem_len = bytes_len;
        while (rem_len > 0) {
            if (state == State::data) {
                if (data.items_len + rem_len > max_bytes) {
                    return Error::OutOfMemory;
                }
                if (!data.appendSlice(alloc, rem, rem_len)) return Error::OutOfMemory;
                return Error::none;
            }

            const Error err = feed(rem[0]);
            if (err != Error::none) return err;
            rem += 1;
            rem_len -= 1;
        }
        return Error::none;
    }

    /// Complete the parsing. This must be called after all the
    /// bytes have been fed to the parser.
    ///
    /// The allocator given will be used for the long-lived data
    /// of the final command.
    Error complete(zigstd::Allocator alloc_, Command *out);

    /// Decodes the payload data from base64 and returns it as a slice.
    /// This function will destroy the contents of self.data, it should
    /// only be used once we are done collecting payload bytes.
    Error decodeData(zigstd::Allocator alloc_, const uint8_t **out_ptr, size_t *out_len) {
        if (data.items_len == 0) {
            *out_ptr = nullptr; /* "" */
            *out_len = 0;
            return Error::none;
        }

        const size_t max_len = simd::base64::maxLen(data.items, data.items_len);
        assert(max_len <= data.items_len);

        /* This is kinda cursed, but we can decode the base64 on top of
         * itself, since it's guaranteed that the encoded size is larger,
         * and any bytes in areas that are written to will have already
         * been used (assuming scalar decoding). */
        size_t decoded_len = 0;
        if (!simd::base64::decode(data.items, data.items_len, data.items, max_len, &decoded_len)) {
            /* log.warn("failed to decode base64 payload data: {}", .{err}) */
            return Error::InvalidData;
        }
        assert(decoded_len <= max_len);

        /* Remove the extra bytes. */
        data.items_len = decoded_len;

        uint8_t *owned = nullptr;
        size_t owned_len = 0;
        if (!data.toOwnedSlice(alloc_, &owned, &owned_len)) return Error::OutOfMemory;
        *out_ptr = owned;
        *out_len = owned_len;
        return Error::none;
    }

    Error accumulateValue(uint8_t c, State overflow_state) {
        const uint8_t idx = kv_temp_len;
        kv_temp_len += 1;
        if (kv_temp_len > sizeof kv_temp) {
            state = overflow_state;
            kv_temp_len = 0;
            return Error::none;
        }
        kv_temp[idx] = c;
        return Error::none;
    }

    Error finishValue(State next_state) {
        /* We can move states right away, we don't use it. */
        state = next_state;

        /* Check for ASCII chars first */
        if (kv_temp_len == 1) {
            const uint8_t c = kv_temp[0];
            if (c < '0' || c > '9') {
                kv.put(kv_current, (uint32_t)c);
                kv_temp_len = 0;
                return Error::none;
            }
        }

        /* Handle integer fields, parsing signed fields accordingly. We still
         * store the fields as u32 as they can be bitcast back later during
         * building of the higher-level command tree. */
        uint32_t v;
        switch (kv_current) {
        case 'z':
        case 'H':
        case 'V': {
            int32_t signed_v;
            const Error err = parseIntI32(kv_temp, kv_temp_len, &signed_v);
            if (err != Error::none) return err;
            v = bitCastToU32(signed_v);
            break;
        }
        default: {
            const Error err = parseIntU32(kv_temp, kv_temp_len, &v);
            if (err != Error::none) return err;
            break;
        }
        }
        kv.put(kv_current, v);

        /* Clear our temp buffer */
        kv_temp_len = 0;
        return Error::none;
    }
};

/// Represents a possible response to a command.
struct Response {
    uint32_t id;           /* = 0 */
    uint32_t image_number; /* = 0 */
    uint32_t placement_id; /* = 0 */
    /// The 1-based animation frame number, echoed as "r=" for the
    /// animation frame actions. This is how clients learn the frame
    /// number assigned to a newly created frame.
    uint32_t frame; /* = 0 */
    const char *message;
    size_t message_len; /* = "OK" */

    Response() : id(0), image_number(0), placement_id(0), frame(0), message("OK"), message_len(2) {}

    /* Wisp: `writer.print("i={}", .{v})`. */
    static void printU32(std::string *writer, const char *key, uint32_t v) {
        char buf[16];
        const int n = snprintf(buf, sizeof buf, "%s%u", key, (unsigned)v);
        writer->append(buf, (size_t)(n < 0 ? 0 : n));
    }

    void encode(std::string *writer) const {
        /* We only encode a result if we have either an id or an image number. */
        if (id == 0 && image_number == 0) return;

        /* Used to cheaply keep track if we need to add a comma before
         * the next key-value pair. */
        bool prior = false;

        writer->append("\x1b" "_G");
        if (id > 0) {
            prior = true;
            printU32(writer, "i=", id);
        }
        if (image_number > 0) {
            if (prior) writer->append(1, ',');
            else prior = true;
            printU32(writer, "I=", image_number);
        }
        if (placement_id > 0) {
            if (prior) writer->append(1, ',');
            else prior = true;
            printU32(writer, "p=", placement_id);
        }
        if (frame > 0) {
            if (prior) writer->append(1, ',');
            else prior = true;
            printU32(writer, "r=", frame);
        }
        writer->append(1, ';');
        writer->append(message, message_len);
        writer->append("\x1b" "\\");
    }

    /// Returns true if this response is not an error.
    bool ok() const { return message_len == 2 && memcmp(message, "OK", 2) == 0; }

    /// Empty response
    bool empty() const { return id == 0 && image_number == 0; }
};

struct Command {
    enum class Action : uint8_t {
        query,                    /* q */
        transmit,                 /* t */
        transmit_and_display,     /* T */
        display,                  /* p */
        delete_,                  /* d */
        transmit_animation_frame, /* f */
        control_animation,        /* a */
        compose_animation,        /* c */
    };

    enum class Quiet : uint8_t {
        no,       /* 0 */
        ok,       /* 1 */
        failures, /* 2 */
    };

    /* Wisp: `union(Action)` is the Action tag plus one field per payload. */
    struct Control {
        struct TransmitAndDisplay {
            Transmission transmission;
            ::wisp::terminal::kitty::graphics::Display display;

            TransmitAndDisplay() : transmission(), display() {}
        };

        struct Identifiers {
            uint32_t image_id;     /* = 0 */
            uint32_t image_number; /* = 0 */
            uint32_t placement_id; /* = 0 */

            Identifiers() : image_id(0), image_number(0), placement_id(0) {}
        };

        Action tag;

        Transmission query;
        Transmission transmit;
        TransmitAndDisplay transmit_and_display;
        ::wisp::terminal::kitty::graphics::Display display;
        Delete delete_;
        AnimationFrameLoading transmit_animation_frame;
        AnimationControl control_animation;
        AnimationFrameComposition compose_animation;

        Control()
            : tag(Action::transmit), query(), transmit(), transmit_and_display(), display(), delete_(),
              transmit_animation_frame(), control_animation(), compose_animation() {}

        /// Returns the image and placement identifiers for any action.
        Identifiers identifiers() const {
            Identifiers result;
            switch (tag) {
            case Action::query:
                result.image_id = query.image_id;
                result.image_number = query.image_number;
                result.placement_id = query.placement_id;
                break;
            case Action::transmit:
                result.image_id = transmit.image_id;
                result.image_number = transmit.image_number;
                result.placement_id = transmit.placement_id;
                break;
            case Action::transmit_and_display:
                result.image_id = transmit_and_display.transmission.image_id;
                result.image_number = transmit_and_display.transmission.image_number;
                result.placement_id = transmit_and_display.transmission.placement_id;
                break;
            case Action::display:
                result.image_id = display.image_id;
                result.image_number = display.image_number;
                result.placement_id = display.placement_id;
                break;
            case Action::delete_:
                result.image_id = delete_.image_id;
                result.image_number = delete_.image_number;
                result.placement_id = delete_.placement_id;
                break;
            case Action::transmit_animation_frame:
                result.image_id = transmit_animation_frame.transmission.image_id;
                result.image_number = transmit_animation_frame.transmission.image_number;
                result.placement_id = transmit_animation_frame.transmission.placement_id;
                break;
            case Action::control_animation:
                result.image_id = control_animation.image_id;
                result.image_number = control_animation.image_number;
                result.placement_id = control_animation.placement_id;
                break;
            case Action::compose_animation:
                result.image_id = compose_animation.image_id;
                result.image_number = compose_animation.image_number;
                result.placement_id = compose_animation.placement_id;
                break;
            }
            return result;
        }
    };

    Control control;
    Quiet quiet;         /* = .no */
    const uint8_t *data; /* = "" */
    size_t data_len;

    Command() : control(), quiet(Quiet::no), data(nullptr), data_len(0) {}

    /// Take ownership over the data in this command. If the returned value
    /// has a length of zero, then the data was empty and need not be freed.
    void toOwnedData(const uint8_t **out_ptr, size_t *out_len) {
        const uint8_t *result = data;
        const size_t result_len = data_len;
        data = nullptr; /* "" */
        data_len = 0;
        *out_ptr = result;
        *out_len = result_len;
    }

    /// Returns the transmission data if it has any.
    /* Wisp: `?Transmission` is a bool return plus an out parameter. */
    bool transmission(Transmission *out) const {
        switch (control.tag) {
        case Action::query: *out = control.query; return true;
        case Action::transmit: *out = control.transmit; return true;
        case Action::transmit_and_display: *out = control.transmit_and_display.transmission; return true;
        case Action::transmit_animation_frame: *out = control.transmit_animation_frame.transmission; return true;
        default: return false;
        }
    }

    /// Returns the display data if it has any.
    bool display(::wisp::terminal::kitty::graphics::Display *out) const {
        switch (control.tag) {
        case Action::display: *out = control.display; return true;
        case Action::transmit_and_display: *out = control.transmit_and_display.display; return true;
        default: return false;
        }
    }

    void deinit(zigstd::Allocator alloc) const {
        if (data_len > 0) alloc.freeT<uint8_t>((uint8_t *)data, data_len);
    }
};

/* Wisp: defined out of line because Command is not complete above. */
inline Error Parser::complete(zigstd::Allocator alloc_, Command *out) {
    switch (state) {
    /* We can't ever end in the control key state and be valid.
     * This means the command looked something like "a=1,b" */
    case State::control_key:
    case State::control_key_ignore: return Error::InvalidFormat;

    /* Some commands (i.e. placements) end without extra data so
     * we end in the value state. i.e. "a=1,b=2" */
    case State::control_value: {
        const Error err = finishValue(State::data);
        if (err != Error::none) return err;
        break;
    }
    case State::control_value_ignore: break;

    /* Most commands end in data, i.e. "a=1,b=2;1234" */
    case State::data: break;
    }

    /* Determine our action, which is always a single character. */
    uint8_t action;
    {
        uint32_t value;
        if (!kv.get('a', &value)) {
            action = 't';
        } else {
            if (value > 0xFF) return Error::InvalidFormat; /* std.math.cast(u8, value) */
            action = (uint8_t)value;
        }
    }
    Command::Control control;
    switch (action) {
    case 'q': {
        control.tag = Command::Action::query;
        const Error err = Transmission::parse(kv, &control.query);
        if (err != Error::none) return err;
        break;
    }
    case 't': {
        control.tag = Command::Action::transmit;
        const Error err = Transmission::parse(kv, &control.transmit);
        if (err != Error::none) return err;
        break;
    }
    case 'T': {
        control.tag = Command::Action::transmit_and_display;
        const Error err = Transmission::parse(kv, &control.transmit_and_display.transmission);
        if (err != Error::none) return err;
        control.transmit_and_display.display = Display::parse(kv);
        break;
    }
    case 'p': {
        control.tag = Command::Action::display;
        control.display = Display::parse(kv);
        break;
    }
    case 'd': {
        control.tag = Command::Action::delete_;
        const Error err = Delete::parse(kv, &control.delete_);
        if (err != Error::none) return err;
        break;
    }
    case 'f': {
        control.tag = Command::Action::transmit_animation_frame;
        const Error err = AnimationFrameLoading::parse(kv, &control.transmit_animation_frame);
        if (err != Error::none) return err;
        break;
    }
    case 'a': {
        control.tag = Command::Action::control_animation;
        control.control_animation = AnimationControl::parse(kv);
        break;
    }
    case 'c': {
        control.tag = Command::Action::compose_animation;
        control.compose_animation = AnimationFrameComposition::parse(kv);
        break;
    }
    default: return Error::InvalidFormat;
    }

    /* Determine our quiet value. The spec specifies 0 and 1 but Kitty
     * allows anything greater than 1 to suppress. */
    Command::Quiet quiet = Command::Quiet::no;
    {
        uint32_t v;
        if (kv.get('q', &v)) {
            switch (v) {
            case 0: quiet = Command::Quiet::no; break;
            case 1: quiet = Command::Quiet::ok; break;
            default: quiet = Command::Quiet::failures; break;
            }
        }
    }

    Command result;
    result.control = control;
    result.quiet = quiet;
    {
        const Error err = decodeData(alloc_, &result.data, &result.data_len);
        if (err != Error::none) return err;
    }
    *out = result;
    return Error::none;
}

inline Error Parser::parseString(zigstd::Allocator alloc_, const uint8_t *data_, size_t data_len, Command *out) {
    Parser parser = init(alloc_, 1024 * 1024);
    /* Wisp: `defer parser.deinit()`. */
    struct Guard {
        Parser *p;
        ~Guard() { p->deinit(); }
    } guard = {&parser};
    (void)guard;
    for (size_t i = 0; i < data_len; i++) {
        const Error err = parser.feed(data_[i]);
        if (err != Error::none) return err;
    }
    return parser.complete(alloc_, out);
}

} /* namespace graphics */
} /* namespace kitty */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_KITTY_GRAPHICS_COMMAND_HPP */
