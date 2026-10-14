/* Transliterated from Ghostty src/terminal/kitty/graphics_storage.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Wisp, differences in shape rather than behavior:
 *
 *   - This file is ported in full. `kitty_graphics` was false when the
 *     surrounding subsystem was ported and this file was omitted; it is
 *     in scope now. Nothing here is gated on `glyph_protocol` or
 *     `tmux_control_mode`, so nothing is omitted for those.
 *
 *   - `std.Io` is not carried over (see screen.hpp), so the `io`
 *     parameter is dropped from every signature: `nextGeneration`,
 *     `markMutated`, `setLimit`, `addImage`, `addPendingImage`,
 *     `addPlacement`, `scrollMarginsBegin`, `reapGarbagePlacements`,
 *     `resolveParent`, `markImageContentChanged`, `convertImageToRgba`,
 *     `reserveAnimationBytes`, `animationTick`, `clearScreen`,
 *     `delete_`, `deleteAnimationFrame`, `evictImage`,
 *     `evictImageExcept` and `PendingImage::complete`.
 *
 *   - `Allocator.Error!void` is `bool` (false = OutOfMemory);
 *     `Allocator.Error!PendingImage` is `bool` plus an out parameter.
 *     `ParentError!PlacementKey` is a `ParentError` return (`none` on
 *     success) plus an out parameter.
 *
 *   - `?T` is `Maybe<T>` (`imageById`, `imageByNumber`, `animationTick`,
 *     `placeholderTarget`, `resolveChain`'s result, `exclude_id`), a
 *     plain nullable pointer where the payload is already a pointer
 *     (`imagePtrByIdOrNumber`, `loading`), or a nullable pointer to the
 *     payload (`removeOrphans`'s `delete_unused: ?Allocator`).
 *
 *   - `union(enum) Placement.Location` is a tag enum plus one field per
 *     payload. The tag member for `.virtual` is spelled `virtual_`
 *     because `virtual` is a C++ keyword; likewise `ImageStorage.delete`
 *     is `delete_` and `Placement.Relative`'s payload accessor keeps
 *     upstream's name.
 *
 *   - `std.AutoHashMapUnmanaged(K, V)` is the local
 *     `AutoHashMapUnmanaged` below: a slot array with tombstones and a
 *     linear-scan lookup, in the spirit of PageList's `PinSet`. It has
 *     exactly the operations this file uses and, importantly, the same
 *     two properties the code relies on: removing the *current* entry
 *     during iteration is safe and leaves the iteration intact, and
 *     key/value pointers are invalidated only by an insertion that
 *     grows the table. Iteration order is unspecified, as upstream's
 *     is; every place that must pick one of several entries orders them
 *     explicitly (`Candidate.lessThan`, `generation`,
 *     `PlacementId.preferredOver`).
 *
 *   - `Image.metadata.placement_count` is upstream's `u30`, carried in a
 *     `uint32_t` by graphics_image.hpp. `std.math.add(u30, ...)`
 *     therefore cannot be written as a C++ overflow check, so
 *     `addPlacement` compares against `placement_count_max` below. The
 *     limit value is identical to upstream's.
 *
 *   - `defer` / `errdefer` are RAII guards. The two guards here
 *     (`delete_`'s mutation marker, `evictImageExcept`'s orphan reap)
 *     run on every exit path including the early `return false` inside
 *     the candidate search, exactly as Zig's `defer` does.
 *
 *   - `log.debug` / `log.info` / `log.warn` have no sink in the port and
 *     are comments.
 *
 *   - `@import("../../quirks.zig").inlineAssert` is plain `assert`.
 *
 *   - `+|` / `-|` saturating operators are the `sat*` helpers below;
 *     `std.math.divCeil(u32, a, b) catch 0` is written with a `uint64_t`
 *     numerator because the saturated numerator can be `maxInt(u32)` and
 *     `a + b - 1` would wrap.
 *
 *   - `std.ArrayListUnmanaged(T).orderedRemove` has no counterpart in
 *     zigstd::ArrayListUnmanaged, so `deleteAnimationFrame` uses the
 *     file-local `orderedRemoveFrame` helper, which is a transliteration
 *     of it for `Animation.Frame`.
 *
 *   - `deleteIntersecting`'s `filter_ctx: anytype` plus `comptime filter:
 *     ?fn(@TypeOf(filter_ctx), Placement) bool` is an `int32_t`
 *     context plus a plain function pointer (null = no filter). Both
 *     call sites in `delete_` use exactly that: `{}` with a null filter,
 *     and `v.z` with the z-equality filter.
 *
 *   - Declaration order is upstream's except that C++ requires a type
 *     to be complete before use, so the nested types `PendingImage`,
 *     `ScrollOp`, `ParentError`, `ResolvedChain`, `PlaceholderTarget`,
 *     `PlacementId`, `PlacementKey`, `Placement` and `ScrollMargins` are
 *     declared before the fields and methods that name them (upstream
 *     declares most of them between the methods). `PendingImage::complete`
 *     names `ImageStorage`, which is still incomplete there, so its body
 *     is defined after the struct. Order within each group is upstream's.
 *
 * Wisp, the one behavioral note:
 *
 *   - `scrollMarginsBegin` opens with `assert(self == &s.kitty_images)`.
 *     Wisp's `Screen` carries no `kitty_images` member (it is ported in
 *     upstream's `kitty_graphics == false` shape, see screen.hpp), so
 *     that assert has no counterpart and is omitted with this note. The
 *     method still derives the screen from the terminal exactly as
 *     upstream does, so a caller whose storage is not the active
 *     screen's is as wrong here as it is upstream — just not asserted.
 *     Connecting `ImageStorage` into `Screen` is a separate change.
 *
 * Wisp, the external seam:
 *
 *   - `convertImageToRgba` calls `pixel.rgbaFromFormat`, whose upstream
 *     signature is `(Allocator, Format, []const u8) Allocator.Error![]u8`.
 *     graphics_pixel.hpp maps it to `Error rgbaFromFormat(Allocator,
 *     Format, const uint8_t *, size_t, uint8_t **, size_t *)`, so the
 *     `Allocator.Error` surfaces as `pixel::Error::OutOfMemory` and is
 *     turned into this file's `false` return.
 */

#pragma once
#ifndef WISP_TERMINAL_KITTY_GRAPHICS_STORAGE_HPP
#define WISP_TERMINAL_KITTY_GRAPHICS_STORAGE_HPP

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <atomic>

#include "../../vt/page_list.hpp"
#include "../../vt/point.hpp"
#include "../../vt/screen.hpp"
#include "../../vt/size.hpp"
#include "../../vt/terminal.hpp"
#include "../../zigstd/allocator.hpp"
#include "../../zigstd/array_list.hpp"
#include "graphics_animation.hpp"
#include "graphics_command.hpp"
#include "graphics_image.hpp"
#include "graphics_pixel.hpp"

namespace wisp {
namespace terminal {
namespace kitty {
namespace graphics_storage {

typedef ::wisp::zigstd::Allocator Allocator;

namespace animation = ::wisp::terminal::kitty::graphics_animation;
namespace command = ::wisp::terminal::kitty::graphics;
namespace pixel = ::wisp::terminal::kitty::graphics_pixel;
namespace point = ::wisp::vt::point;
namespace vtsize = ::wisp::vt::size;

typedef ::wisp::vt::PageList PageList;
typedef ::wisp::vt::Screen Screen;
typedef ::wisp::vt::Terminal Terminal;
typedef ::wisp::terminal::kitty::graphics_image::LoadingImage LoadingImage;
typedef ::wisp::terminal::kitty::graphics_image::Image Image;
typedef ::wisp::terminal::kitty::graphics_image::Rect Rect;
typedef ::wisp::terminal::kitty::graphics::Command Command;

/* Wisp: `?T` for value types, shared with the rest of the port. */
using ::wisp::vt::Maybe;

/* Wisp: upstream's `Image.metadata.placement_count` is a `u30`, so
 * `std.math.add` on it overflows at 2^30 - 1. graphics_image.hpp carries
 * the field in a `uint32_t`, so the limit is spelled out here. */
static const uint32_t placement_count_max = ((uint32_t)1 << 30) - 1;

/* Wisp: Zig's `+|` for u32 / u64 / i32, and `-|` for u32. */
static inline uint32_t satAddU32(uint32_t a, uint32_t b) {
    const uint64_t sum = (uint64_t)a + (uint64_t)b;
    return sum > (uint64_t)0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)sum;
}
static inline uint32_t satSubU32(uint32_t a, uint32_t b) { return a > b ? a - b : 0; }
static inline uint64_t satAddU64(uint64_t a, uint64_t b) {
    const uint64_t sum = a + b;
    return sum < a ? 0xFFFFFFFFFFFFFFFFull : sum;
}
static inline int32_t satAddI32(int32_t a, int32_t b) {
    const int64_t sum = (int64_t)a + (int64_t)b;
    if (sum > (int64_t)2147483647) return 2147483647;
    if (sum < (int64_t)(-2147483647 - 1)) return (-2147483647 - 1);
    return (int32_t)sum;
}

/* Wisp: std.ArrayListUnmanaged(Animation.Frame).orderedRemove(i). */
static inline animation::Animation::Frame orderedRemoveFrame(animation::Animation *anim,
                                                             size_t i) {
    animation::Animation::Frame old = anim->frames.items[i];
    const size_t tail = anim->frames.items_len - i - 1;
    if (tail > 0) {
        memmove((void *)(anim->frames.items + i), (const void *)(anim->frames.items + i + 1),
                tail * sizeof(animation::Animation::Frame));
    }
    anim->frames.items_len -= 1;
    return old;
}

/* ──────────────────────────────────────────────────────────────────────
 * Wisp: std.AutoHashMapUnmanaged(K, V). See the header note.
 * ────────────────────────────────────────────────────────────────────── */
template <typename K, typename V>
struct AutoHashMapUnmanaged {
    /* `key` is first so a key pointer can be cast back to its slot. */
    struct Slot {
        K key;
        V value;
        bool used;
    };

    struct Entry {
        K *key_ptr;
        V *value_ptr;

        Entry() : key_ptr(nullptr), value_ptr(nullptr) {}
    };

    struct GetOrPutResult {
        K *key_ptr;
        V *value_ptr;
        bool found_existing;

        GetOrPutResult() : key_ptr(nullptr), value_ptr(nullptr), found_existing(false) {}
    };

    /* Iteration skips tombstones, so removing the entry just yielded
     * (the only removal this file performs mid-iteration) leaves the
     * remaining entries intact. */
    struct Iterator {
        Slot *slots;
        size_t slots_len;
        size_t i;

        bool next(Entry *out) {
            while (i < slots_len) {
                Slot *s = slots + i;
                i += 1;
                if (!s->used) continue;
                out->key_ptr = &s->key;
                out->value_ptr = &s->value;
                return true;
            }
            return false;
        }
    };

    struct KeyIterator {
        Slot *slots;
        size_t slots_len;
        size_t i;

        K *next() {
            while (i < slots_len) {
                Slot *s = slots + i;
                i += 1;
                if (!s->used) continue;
                return &s->key;
            }
            return nullptr;
        }
    };

    Slot *slots;
    size_t slots_len; /* high-water mark of occupied-or-tombstoned slots */
    size_t capacity;
    size_t len; /* live entries */

    AutoHashMapUnmanaged() : slots(nullptr), slots_len(0), capacity(0), len(0) {}

    size_t count() const { return len; }

    Iterator iterator() const {
        Iterator it;
        it.slots = slots;
        it.slots_len = slots_len;
        it.i = 0;
        return it;
    }

    KeyIterator keyIterator() const {
        KeyIterator it;
        it.slots = slots;
        it.slots_len = slots_len;
        it.i = 0;
        return it;
    }

    V *getPtr(const K &k) const {
        for (size_t i = 0; i < slots_len; i += 1) {
            if (slots[i].used && slots[i].key == k) return &slots[i].value;
        }
        return nullptr;
    }

    bool contains(const K &k) const { return getPtr(k) != nullptr; }

    /* Wisp: `?V` is a bool return plus an out parameter. */
    bool get(const K &k, V *out) const {
        V *p = getPtr(k);
        if (p == nullptr) return false;
        *out = *p;
        return true;
    }

    /* Wisp: `?Entry` is a bool return plus an out parameter. */
    bool getEntry(const K &k, Entry *out) const {
        for (size_t i = 0; i < slots_len; i += 1) {
            if (slots[i].used && slots[i].key == k) {
                out->key_ptr = &slots[i].key;
                out->value_ptr = &slots[i].value;
                return true;
            }
        }
        return false;
    }

    /* Wisp: false is OutOfMemory. */
    bool ensureUnusedCapacity(Allocator alloc, size_t additional) {
        if (capacity - len >= additional) return true;
        size_t want = capacity;
        while (want < len + additional) want = want + want / 2 + 8;
        return reallocCompact(alloc, want);
    }

    bool reallocCompact(Allocator alloc, size_t new_capacity) {
        Slot *n = alloc.allocT<Slot>(new_capacity);
        if (n == nullptr) return false;
        size_t w = 0;
        for (size_t i = 0; i < slots_len; i += 1) {
            if (!slots[i].used) continue;
            n[w] = slots[i];
            w += 1;
        }
        if (capacity > 0) alloc.freeT<Slot>(slots, capacity);
        slots = n;
        capacity = new_capacity;
        slots_len = w;
        len = w;
        return true;
    }

    GetOrPutResult getOrPutAssumeCapacity(const K &k) {
        GetOrPutResult r;
        for (size_t i = 0; i < slots_len; i += 1) {
            if (slots[i].used && slots[i].key == k) {
                r.key_ptr = &slots[i].key;
                r.value_ptr = &slots[i].value;
                r.found_existing = true;
                return r;
            }
        }

        /* Reuse a tombstone if there is one, else append. */
        size_t idx = slots_len;
        for (size_t i = 0; i < slots_len; i += 1) {
            if (!slots[i].used) {
                idx = i;
                break;
            }
        }
        assert(idx < capacity);
        if (idx == slots_len) slots_len += 1;
        slots[idx].key = k;
        slots[idx].used = true;
        len += 1;
        r.key_ptr = &slots[idx].key;
        r.value_ptr = &slots[idx].value;
        r.found_existing = false;
        return r;
    }

    /* Wisp: false is OutOfMemory. */
    bool getOrPut(Allocator alloc, const K &k, GetOrPutResult *out) {
        if (!contains(k)) {
            if (!ensureUnusedCapacity(alloc, 1)) return false;
        }
        *out = getOrPutAssumeCapacity(k);
        return true;
    }

    void removeByPtr(K *key_ptr) {
        Slot *s = (Slot *)(void *)key_ptr; /* key is the first member */
        assert(s->used);
        s->used = false;
        len -= 1;
    }

    void clearRetainingCapacity() {
        for (size_t i = 0; i < slots_len; i += 1) slots[i].used = false;
        slots_len = 0;
        len = 0;
    }

    void deinit(Allocator alloc) {
        if (capacity > 0) alloc.freeT<Slot>(slots, capacity);
        slots = nullptr;
        slots_len = 0;
        capacity = 0;
        len = 0;
    }
};

/* Backing implementation for the generation counter. We use a
 * lock-free atomic counter where we can, but not all targets support
 * 64-bit atomic operations (e.g. 32-bit ARM Android), so we fall back
 * to a mutex-protected counter on those. This is a cold path (only
 * invoked on content mutations) so the mutex cost is irrelevant.
 *
 * Wisp: the fallback branch is selected by upstream's comptime
 * `@bitSizeOf(usize) >= 64`. The port targets x64 only, so only the
 * atomic branch is transliterated. */
struct GenerationCounter {
    std::atomic<uint64_t> value;

    GenerationCounter() : value(0) {}

    uint64_t next() { return value.fetch_add(1, std::memory_order_relaxed) + 1; }
};

/* Process-global counter backing all generation stamps (see
 * ImageStorage.generation and Image.generation). This is global rather
 * than per-storage so that stamps are unique across every storage in
 * the process: two mutation events never produce the same value, even
 * across separate screens (main vs. alt), storage resets, or separate
 * terminals. This lets consumers use a generation value alone as a
 * cache key without any ambiguity.
 *
 * Thread-safe because separate terminals may mutate their storages
 * from different threads.
 *
 * Wisp: a header-only port cannot have a file-scope mutable global, so
 * the counter lives in a function-local static. */
inline GenerationCounter &generationCounter() {
    static GenerationCounter counter;
    return counter;
}

/* Returns the next generation stamp. Stamps are unique and strictly
 * monotonically increasing process-wide, starting at 1 (0 is reserved
 * to mean "never stamped"). */
inline uint64_t nextGeneration() { return generationCounter().next(); }

/* An image storage is associated with a terminal screen (i.e. main
 * screen, alt screen) and contains all the transmitted images and
 * placements. */
struct ImageStorage {
    /* ── Wisp: nested types, hoisted above their users. ───────────── */

    /* Internal placement IDs are assigned by us for placements created
     * without an explicit ID (p=0); external IDs are client-specified.
     * The two are separate namespaces, hence the tag.
     *
     * Wisp: upstream's `packed struct { tag: enum(u1), id: u32 }`.
     * Nothing bit-casts it, so it is a plain struct. */
    struct PlacementId {
        enum class Tag : uint8_t { internal, external };

        Tag tag;
        uint32_t id;

        PlacementId() : tag(Tag::internal), id(0) {}
        PlacementId(Tag t, uint32_t i) : tag(t), id(i) {}

        static PlacementId internal(uint32_t i) { return PlacementId(Tag::internal, i); }
        static PlacementId external(uint32_t i) { return PlacementId(Tag::external, i); }

        /* Deterministic preference order used when an operation must
         * pick a single placement out of several (kitty uses creation
         * order, which our map doesn't track): external IDs win over
         * internal ones, and lower IDs win within a tag. */
        bool preferredOver(const PlacementId &other) const {
            if (tag != other.tag) return tag == Tag::external;
            return id < other.id;
        }

        /* Wisp: std.meta.eql, for the hash map and PlacementKey.eql. */
        bool operator==(const PlacementId &o) const { return tag == o.tag && id == o.id; }
        bool operator!=(const PlacementId &o) const { return !(*this == o); }
    };

    /* Every placement is uniquely identified by the image ID and the
     * placement ID. If an image ID isn't specified it is assumed to be 0.
     * Likewise, if a placement ID isn't specified it is assumed to be 0. */
    struct PlacementKey {
        uint32_t image_id;
        PlacementId placement_id;

        PlacementKey() : image_id(0), placement_id() {}
        PlacementKey(uint32_t img, const PlacementId &p) : image_id(img), placement_id(p) {}

        bool eql(const PlacementKey &other) const {
            return image_id == other.image_id && placement_id == other.placement_id;
        }

        /* Wisp: for the hash map's key comparison. */
        bool operator==(const PlacementKey &o) const { return eql(o); }
        bool operator!=(const PlacementKey &o) const { return !eql(o); }
    };

    struct Placement {
        /* Wisp: `relative`'s payload. Declared first because Location
         * names it. */
        struct Relative {
            /* The parent placement. Resolved to a concrete key when
             * the placement is created, so the parent is guaranteed
             * to exist at that point. */
            PlacementKey parent;

            /* Cell offsets from the parent's origin (H=/V=). */
            int32_t horizontal_offset; /* = 0 */
            int32_t vertical_offset;   /* = 0 */

            Relative() : parent(), horizontal_offset(0), vertical_offset(0) {}
        };

        /* Wisp: `union(enum)` is a tag enum plus one field per payload.
         * `.virtual` is spelled `virtual_`; `virtual` is a C++ keyword. */
        struct Location {
            enum class Tag : uint8_t {
                /* Exactly placed on a screen pin. */
                pin,

                /* Virtual placement (U=1) for unicode placeholders. */
                virtual_,

                /* Placed relative to a parent placement (P=/Q=). The
                 * placement has no screen position of its own; it is
                 * positioned at render time by resolving the parent chain
                 * to its root (see resolveChain) and therefore follows the
                 * parent through scrolls automatically. Its lifetime is
                 * tied to the parent: removing any ancestor removes it
                 * too (see removeOrphans). */
                relative,
            };

            Tag tag;
            PageList::Pin *pin;
            Relative relative;

            Location() : tag(Tag::virtual_), pin(nullptr), relative() {}

            static Location makePin(PageList::Pin *p) {
                Location l;
                l.tag = Tag::pin;
                l.pin = p;
                return l;
            }
            static Location makeVirtual() {
                Location l;
                l.tag = Tag::virtual_;
                return l;
            }
            static Location makeRelative(const Relative &r) {
                Location l;
                l.tag = Tag::relative;
                l.relative = r;
                return l;
            }
        };

        /* The location where this placement should be drawn. */
        Location location;

        /* Offset of the x/y from the top-left of the cell. */
        uint32_t x_offset; /* = 0 */
        uint32_t y_offset; /* = 0 */

        /* Source rectangle for the image to pull from */
        uint32_t source_x;      /* = 0 */
        uint32_t source_y;      /* = 0 */
        uint32_t source_width;  /* = 0 */
        uint32_t source_height; /* = 0 */

        /* The columns/rows this image occupies. */
        uint32_t columns; /* = 0 */
        uint32_t rows;    /* = 0 */

        /* The z-index for this placement. */
        int32_t z; /* = 0 */

        Placement()
            : location(), x_offset(0), y_offset(0), source_x(0), source_y(0), source_width(0),
              source_height(0), columns(0), rows(0), z(0) {}

        void deinit(Screen *s) const {
            switch (location.tag) {
            case Location::Tag::pin: s->pages.untrackPin(location.pin); break;
            case Location::Tag::virtual_:
            case Location::Tag::relative: break;
            }
        }

        /* Multiply two protocol-controlled values without allowing them to
         * wrap. Placement geometry is exposed as u32, so values larger than
         * that are represented by the largest possible value. */
        static uint32_t saturatingMul(uint32_t lhs, uint32_t rhs) {
            const uint64_t product = (uint64_t)lhs * (uint64_t)rhs;
            if (product > (uint64_t)0xFFFFFFFFu) return 0xFFFFFFFFu;
            return (uint32_t)product;
        }

        /* Scale a dimension by an aspect ratio and round to the nearest
         * integer. The u64 intermediate can hold the product of two u32s as
         * well as the rounding adjustment. */
        static uint32_t scaleDimension(uint32_t value, uint32_t numerator,
                                       uint32_t denominator) {
            if (denominator == 0) return 0;

            const uint64_t rounded =
                ((uint64_t)value * (uint64_t)numerator + (uint64_t)denominator / 2) /
                (uint64_t)denominator;
            if (rounded > (uint64_t)0xFFFFFFFFu) return 0xFFFFFFFFu;
            return (uint32_t)rounded;
        }

        struct SourceRect {
            uint32_t x;
            uint32_t y;
            uint32_t width;
            uint32_t height;

            SourceRect() : x(0), y(0), width(0), height(0) {}
        };

        /* Returns the requested source rectangle intersected with the image.
         * A zero width or height requests the full corresponding image
         * dimension before intersection, as defined by the Kitty protocol. */
        SourceRect sourceRect(const Image &image) const {
            const uint32_t x = source_x < image.width ? source_x : image.width;
            const uint32_t y = source_y < image.height ? source_y : image.height;
            const uint32_t want_w = source_width > 0 ? source_width : image.width;
            const uint32_t want_h = source_height > 0 ? source_height : image.height;
            SourceRect r;
            r.x = x;
            r.y = y;
            r.width = want_w < (image.width - x) ? want_w : (image.width - x);
            r.height = want_h < (image.height - y) ? want_h : (image.height - y);
            return r;
        }

        struct CellOffset {
            uint32_t x;
            uint32_t y;

            CellOffset() : x(0), y(0) {}
        };

        /* Returns the placement offset clamped within the first cell. Pixel
         * geometry may temporarily be unavailable, in which case there is no
         * valid offset within the cell. */
        CellOffset cellOffset(const Terminal *t) const {
            const uint32_t cell_width = t->width_px / t->cols;
            const uint32_t cell_height = t->height_px / t->rows;

            CellOffset r;
            r.x = cell_width > 0 ? (x_offset < cell_width - 1 ? x_offset : cell_width - 1) : 0;
            r.y = cell_height > 0 ? (y_offset < cell_height - 1 ? y_offset : cell_height - 1) : 0;
            return r;
        }

        struct PixelSize {
            uint32_t width;
            uint32_t height;

            PixelSize() : width(0), height(0) {}
        };

        /* Returns the size of this placement's image in pixels,
         * taking into account the source rectangle, specified
         * rows/columns, and aspect ratio. */
        PixelSize pixelSize(const Image &image, const Terminal *t) const {
            const SourceRect source = sourceRect(image);
            const uint32_t width = source.width;
            const uint32_t height = source.height;

            PixelSize out;

            /* If we don't have any specified cols or rows then the placement
             * should be the native size of the image, and doesn't need to be
             * re-scaled. */
            if (columns == 0 && rows == 0) {
                out.width = width;
                out.height = height;
                return out;
            }

            /* We calculate the size of a cell so that we can multiply
             * it by the specified cols/rows to get the correct px size.
             *
             * We assume that the width is divided evenly by the column
             * count and the height by the row count, because it should be. */
            const uint32_t cell_width = t->width_px / t->cols;
            const uint32_t cell_height = t->height_px / t->rows;
            const CellOffset cell_offset = cellOffset(t);

            /* If we have a specified cols AND rows then we calculate
             * the width and height from them directly, we don't need
             * to adjust for aspect ratio. */
            if (columns > 0 && rows > 0) {
                const uint32_t calc_width = saturatingMul(cell_width, columns);
                const uint32_t calc_height = saturatingMul(cell_height, rows);

                out.width = satSubU32(calc_width, cell_offset.x);
                out.height = satSubU32(calc_height, cell_offset.y);
                return out;
            }

            /* Either the columns or the rows were specified, but not both,
             * so we need to calculate the other one based on the aspect ratio. */

            /* If only the columns were specified, we determine
             * the height of the image based on the aspect ratio. */
            if (columns > 0) {
                const uint32_t calc_width =
                    satSubU32(saturatingMul(cell_width, columns), cell_offset.x);
                const uint32_t calc_height = scaleDimension(calc_width, height, width);

                out.width = calc_width;
                out.height = calc_height;
                return out;
            }

            /* Otherwise, only the rows were specified, so we
             * determine the width based on the aspect ratio. */
            {
                const uint32_t calc_height =
                    satSubU32(saturatingMul(cell_height, rows), cell_offset.y);
                const uint32_t calc_width = scaleDimension(calc_height, width, height);

                out.width = calc_width;
                out.height = calc_height;
                return out;
            }
        }

        struct GridSize {
            uint32_t cols;
            uint32_t rows;

            GridSize() : cols(0), rows(0) {}
        };

        /* Returns the size in grid cells that this placement takes up. */
        GridSize gridSize(const Image &image, const Terminal *t) const {
            GridSize out;

            /* If we have a specified columns and rows then this is trivial. */
            if (columns > 0 && rows > 0) {
                out.cols = columns;
                out.rows = rows;
                return out;
            }

            /* Otherwise we calculate the pixel size, divide by
             * cell size, and round up to the nearest integer. */
            const PixelSize calc_size = pixelSize(image, t);
            const CellOffset cell_offset = cellOffset(t);
            const uint32_t cell_width = t->width_px / t->cols;
            const uint32_t cell_height = t->height_px / t->rows;
            /* Wisp: `std.math.divCeil(u32, a, b) catch 0`. The numerator can
             * be maxInt(u32) after saturation, so the +b-1 is done in u64. */
            out.cols = cell_width == 0
                           ? 0
                           : (uint32_t)(((uint64_t)satAddU32(calc_size.width, cell_offset.x) +
                                         (uint64_t)cell_width - 1) /
                                        (uint64_t)cell_width);
            out.rows = cell_height == 0
                           ? 0
                           : (uint32_t)(((uint64_t)satAddU32(calc_size.height, cell_offset.y) +
                                         (uint64_t)cell_height - 1) /
                                        (uint64_t)cell_height);
            return out;
            /* NOTE: Above `divCeil`s can only fail if the cell size is 0,
             *       in such a case it seems safe to return 0 for this. */
        }

        /* Permanently clip `count` rows off the top of this placement,
         * which currently spans `span` grid rows, by shrinking the
         * source rectangle (the requested source rect is materialized
         * into explicit values in the process). Used when a scroll
         * within margins pushes the placement past the top margin.
         * Requires 0 < count < span. Returns false if no visible
         * source pixels would remain, in which case the placement is
         * unmodified and should be deleted. */
        bool clipTop(const Image &image, const Terminal *t, uint32_t count, uint32_t span) {
            assert(count > 0 && count < span);
            const SourceRect src = sourceRect(image);
            const uint32_t crop =
                rows > 0
                    /* Scaled placement: each grid row shows an equal
                     * fraction of the source. */
                    ? (uint32_t)((uint64_t)src.height * (uint64_t)count / (uint64_t)span)
                    /* Native size: each grid row shows one cell height of
                     * source pixels (kitty does the same). */
                    : saturatingMul(t->height_px / t->rows, count);
            if (crop >= src.height) return false;
            source_x = src.x;
            source_y = src.y + crop;
            source_width = src.width;
            source_height = src.height - crop;
            if (rows > 0) rows -= count;
            return true;
        }

        /* clipTop, but clipping `count` rows off the bottom. */
        bool clipBottom(const Image &image, const Terminal *t, uint32_t count, uint32_t span) {
            assert(count > 0 && count < span);
            const SourceRect src = sourceRect(image);
            /* An empty source can never be clipped smaller. This also
             * guarantees we never store an explicit zero source height,
             * which would be reinterpreted as "full image". */
            if (src.height == 0) return false;
            if (rows > 0) {
                const uint32_t crop =
                    (uint32_t)((uint64_t)src.height * (uint64_t)count / (uint64_t)span);
                if (crop >= src.height) return false;
                source_height = src.height - crop;
                rows -= count;
            } else {
                /* The remaining rows can show this many source pixels,
                 * with the first cell losing cellOffset pixels to the
                 * placement's y offset. */
                const uint32_t visible = saturatingMul(t->height_px / t->rows, span - count);
                const uint32_t offset = cellOffset(t).y;
                if (visible <= offset) return false;
                const uint32_t avail = visible - offset;
                source_height = src.height < avail ? src.height : avail;
            }
            source_x = src.x;
            source_y = src.y;
            source_width = src.width;
            return true;
        }

        /* Returns a selection of the entire rectangle this placement
         * occupies within the screen. This can return null for a virtual
         * placement or when unavailable pixel geometry makes it empty.
         *
         * Relative placements also return null: they have no screen
         * position of their own, so geometric queries (delete by
         * intersection, row, column, etc.) never match them directly.
         * They are instead removed when their parent chain is removed.
         *
         * Wisp: `?Rect` is a bool return plus an out parameter. */
        bool rect(const Image &image, const Terminal *t, Rect *out) const {
            const GridSize grid_size = gridSize(image, t);
            /* Wisp: value-initialized; upstream's switch covers every tag
             * and the `virtual`/`relative` arms return. */
            PageList::Pin *pin = nullptr;
            switch (location.tag) {
            case Location::Tag::pin: pin = location.pin; break;
            case Location::Tag::virtual_:
            case Location::Tag::relative: return false;
            default: return false;
            }
            if (pin->garbage) return false;

            /* A zero pixel-sized placement can produce a zero grid size when
             * pixel geometry is unavailable. It occupies no rectangle. */
            if (grid_size.cols == 0 || grid_size.rows == 0) return false;

            const PageList::Pin::Overflow o = pin->downOverflow(grid_size.rows - 1);
            PageList::Pin br =
                o.tag == PageList::Pin::Overflow::Tag::offset ? o.offset : o.overflow.end;
            {
                /* We need to sub one here because the x value is
                 * one width already. So if the image is width "1"
                 * then we add zero to X because X itself is width 1. */
                const uint32_t extended = satAddU32((uint32_t)pin->x, grid_size.cols - 1);
                const uint32_t limit = (uint32_t)t->cols - 1;
                br.x = (vtsize::CellCountInt)(extended < limit ? extended : limit);
            }

            out->top_left = *pin;
            out->bottom_right = br;
            return true;
        }
    };

    typedef AutoHashMapUnmanaged<uint32_t, Image> ImageMap;
    typedef AutoHashMapUnmanaged<PlacementKey, Placement> PlacementMap;

    /* Identifies one exact pending image transmission. The generation is
     * assigned by this storage when the pending image is inserted, so a
     * later replacement of the same ID cannot consume stale payload bytes. */
    struct PendingImage {
        uint32_t id;
        uint64_t generation;

        PendingImage() : id(0), generation(0) {}

        /* Attach owned decoded bytes if this exact pending transmission is
         * still resident. On true, storage owns `data`; on false, the caller
         * retains ownership. Completion preserves the image generation (and
         * therefore eviction age) while marking storage content as mutated.
         *
         * Wisp: defined after ImageStorage; it names the incomplete type. */
        bool complete(ImageStorage *storage, const uint8_t *data, size_t data_len) const;
    };

    /* How the row operation behind a scrollMarginsBegin/end pair moves
     * tracked pins, which determines how much repositioning work the
     * placements need. */
    enum class ScrollOp {
        /* The operation shifts the entire active window down (it
         * creates scrollback), so every placement anchored in the
         * active area changes active position and must be restored. */
        window_shift,

        /* The operation moves rows in place within the scrolling
         * region: pins anchored outside the region rows keep their
         * active position without help, so only placements anchored
         * inside the region rows need handling. This is the common
         * case (e.g. a status bar image beside a scrolling pane
         * costs almost nothing per scroll). */
        in_place,
    };

    /* State returned by scrollMarginsBegin: the final active-area
     * position every surviving placement pin must be restored to once
     * the scrolled rows have settled. Every begin must be paired with
     * exactly one end() call after the scroll's row operations
     * complete. */
    struct ScrollMargins {
        struct Restore {
            PageList::Pin *pin;
            vtsize::CellCountInt x;
            uint32_t y;

            Restore() : pin(nullptr), x(0), y(0) {}
        };

        Screen *screen;
        ::wisp::zigstd::ArrayListUnmanaged<Restore> restores; /* = .empty */

        ScrollMargins() : screen(nullptr), restores() {}

        /* Reposition every recorded placement pin to its post-scroll
         * position and release the state. Must be called exactly
         * once, after the scroll's row operations complete. */
        void end() {
            /* Wisp: `for (0..n)` snapshot; nothing here mutates the list,
             * but the bound is snapshotted on principle. */
            const size_t n = restores.items_len;
            for (size_t i = 0; i < n; i += 1) {
                const Restore &restore = restores.items[i];
                const Maybe<PageList::Pin> p = screen->pages.pin(
                    point::Point::active(restore.x, restore.y));
                if (!p.has) continue;
                *restore.pin = p.value;
            }
            restores.deinit(screen->alloc);
        }
    };

    /* Maximum number of parent links in a relative placement chain,
     * matching the minimum specified in the spec and the actual
     * limit defined by Kitty at the time of authoring. */
    static const size_t parent_chain_limit = 8;

    /* Wisp: upstream's `ParentError` error set, plus `none` for success. */
    enum class ParentError {
        none,
        /* The parent image (P=) does not exist. */
        ParentImageNotFound,
        /* The parent image exists but the requested placement (Q=, or
         * any placement when Q is omitted) does not. */
        ParentPlacementNotFound,
        /* The placement refers to itself as its own parent. */
        SelfParent,
        /* The parent chain loops back to the placement being created. */
        Cycle,
        /* The parent chain exceeds parent_chain_limit links. */
        TooDeep,
        /* An ancestor in the chain no longer exists. This should not
         * happen since removeOrphans keeps chains intact, but we
         * check anyway rather than trusting the invariant. */
        AncestorNotFound,
    };

    /* The result of resolving a relative placement's parent chain:
     * the chain's root placement (always pin or virtual, never
     * relative) and the total cell offset from the root's origin. */
    struct ResolvedChain {
        PlacementKey root_key;
        Placement root;
        int32_t horizontal_offset;
        int32_t vertical_offset;

        ResolvedChain() : root_key(), root(), horizontal_offset(0), vertical_offset(0) {}
    };

    /* Wisp: placeholderTarget's anonymous `struct { key, placement }`. */
    struct PlaceholderTarget {
        PlacementKey key;
        Placement placement;

        PlaceholderTarget() : key(), placement() {}
    };

    /* ── Fields, in upstream's order. ─────────────────────────────── */

    /* Dirty is set to true if placements or images change. This is
     * purely informational for the renderer and doesn't affect the
     * correctness of the program. The renderer must set this to false
     * if it cares about this value.
     *
     * Note that dirty is also set by scrolling and resizing (outside
     * of this struct) because those move placement pins, even though
     * the set of images/placements itself is unchanged. See generation
     * for a signal that only tracks content mutations.
     *
     * Invariant: dirty is always set when generation changes
     * (markMutated sets both); dirty set without a generation change
     * means a geometry-only event. */
    bool dirty; /* = false */

    /* Generation stamp of the last content mutation to this storage:
     * any image transmit/replace, placement add, or delete of either.
     * Zero means the storage has never been mutated (and is therefore
     * empty).
     *
     * Unlike dirty, this is NOT updated by scrolling/resizing, so an
     * unchanged generation means the placement set and all image data
     * are identical; only placement geometry (pins) may have moved.
     * Values come from a process-global monotonic counter, so a value
     * observed from any storage never recurs for different content,
     * even across screen switches or storage resets.
     *
     * This field must only be written via markMutated. */
    uint64_t generation; /* = 0 */

    /* This is the next automatically assigned image ID for images
     * transmitted without an ID or number. We start mid-way through
     * the u32 range to stay clear of the low IDs client programs
     * typically pick. See nextImageId. */
    uint32_t next_image_id; /* = 2147483647 */

    /* This is the next automatically assigned placement ID. This is never
     * user-facing so we can start at 0. This is 32-bits because we use
     * the same space for external placement IDs. We can start at zero
     * because any number is valid. */
    uint32_t next_internal_placement_id; /* = 0 */

    /* The set of images that are currently known. */
    ImageMap images; /* = .{} */

    /* The set of placements for loaded images. */
    PlacementMap placements; /* = .{} */

    /* Non-null if there is an in-progress loading image. */
    LoadingImage *loading; /* = null */

    /* The limits of what medium types are allowed for image loading. */
    LoadingImage::Limits image_limits; /* = .direct */

    /* The total bytes of image data that have been loaded and the limit.
     * If the limit is reached, the oldest images will be evicted to make
     * space. Unused images take priority. */
    size_t total_bytes;                             /* = 0 */
    size_t total_limit;                             /* = 320 * 1000 * 1000 (320MB) */

    ImageStorage()
        : dirty(false), generation(0), next_image_id(2147483647),
          next_internal_placement_id(0), images(), placements(), loading(nullptr),
          image_limits(LoadingImage::Limits::direct()), total_bytes(0),
          total_limit(320 * 1000 * 1000) {}

    /* ── Methods, in upstream's order. ───────────────────────────── */

    void deinit(Allocator alloc, Screen *s) {
        if (loading != nullptr) loading->destroy(alloc);

        clearPlacements(s);
        placements.deinit(alloc);

        ImageMap::Iterator it = images.iterator();
        ImageMap::Entry kv;
        while (it.next(&kv)) kv.value_ptr->deinit(alloc);
        images.deinit(alloc);
    }

    /* Kitty image protocol is enabled if we have a non-zero limit. */
    bool enabled() const { return total_limit != 0; }

    /* Record a content mutation: marks the storage dirty and assigns a
     * fresh generation stamp. Must be called by anything that changes
     * the set of images or placements (or image contents), including
     * the animation command handlers in graphics_exec.zig.
     *
     * Do NOT call this for geometry-only events (scrolling, resizing,
     * screen switches); those must set only the dirty flag directly.
     * Bumping the generation for geometry changes would break the
     * contract that an unchanged generation means unchanged contents. */
    void markMutated() {
        dirty = true;
        generation = nextGeneration();
    }

    /* Sets the limit in bytes for the total amount of image data that
     * can be loaded. If this limit is lower, this will do an eviction
     * if necessary. If the value is zero, then Kitty image protocol will
     * be disabled. */
    void setLimit(Allocator alloc, Screen *s, size_t limit) {
        /* Special case disabling by quickly deleting all */
        if (limit == 0) {
            const LoadingImage::Limits saved_limits = image_limits;
            deinit(alloc, s);
            *this = ImageStorage();
            image_limits = saved_limits;
            markMutated();
        }

        /* If we re lowering our limit, check if we need to evict.
         * Wisp: upstream does not return from the limit == 0 branch
         * above; the fall-through is kept. */
        if (limit < total_bytes) {
            const size_t req_bytes = total_bytes - limit;
            /* log.info("evicting images to lower limit, evicting={}", req_bytes) */
            if (!evictImage(alloc, s, req_bytes)) {
                /* log.warn("failed to evict enough images for required bytes") */
            }
        }

        total_limit = limit;
    }

    /* Wisp: `mode: enum { explicit, implicit }`. */
    enum class NextImageIdMode { explicit_, implicit };

    /* Returns the next ID to automatically assign to an image that
     * was transmitted without an explicit ID (i=). The result is
     * never zero (zero means "no ID") and never an ID currently in
     * use, so an automatic ID never replaces an existing image. */
    uint32_t nextImageId(NextImageIdMode mode) {
        /* Starting ID depends on mode. */
        uint32_t id = 0;
        switch (mode) {
        /* Yeah, starting from 1 is wasteful. This matches what Kitty
         * does. In the future we can probably cache a low-water-mark
         * based on delete behavior. */
        case NextImageIdMode::explicit_: id = 1; break;
        case NextImageIdMode::implicit: id = next_image_id; break;
        }

        /* Go through all possible images. We have +2 because we can
         * exceed the total by 1 (new ID).
         *
         * Wisp: Zig evaluates the `for (0..n)` range once; the trip
         * count is snapshotted here for the same reason. */
        const size_t count = images.count();
        const size_t trips = count + 2;
        for (size_t i = 0; i < trips; i += 1) {
            if (id != 0 && !images.contains(id)) break;
            id += 1; /* `+%=` */
        }

        /* Zero is never allowed. */
        if (id == 0) id = 1;

        /* Keep track of our next image id for implicit to speed that up. */
        if (mode == NextImageIdMode::implicit) next_image_id = id + 1; /* `+%` */

        return id;
    }

    /* Add an image to the storage. This will automatically free any existing
     * image with the same ID. Prefer addPendingImage for pending data so the
     * caller receives a completion token.
     *
     * Wisp: false is OutOfMemory. */
    bool addImage(Allocator alloc, Screen *s, const Image &img) {
        const size_t new_len = img.data.len();

        /* If the image itself is over the limit, then error immediately */
        if (new_len > total_limit) return false;

        /* Credit an existing image's reservation before calculating the
         * replacement size. In particular, a completed live transmission
         * replacing pending snapshot metadata must be able to reuse the
         * reservation without evicting its own ID. */
        size_t old_len = 0;
        {
            Image old;
            if (images.get(img.id, &old)) old_len = old.storageSize();
        }
        assert(old_len <= total_bytes);

        /* Reserve map capacity before evicting anything so allocation failure
         * cannot leave storage partially evicted. Existing keys need no room. */
        if (!images.contains(img.id)) {
            if (!images.ensureUnusedCapacity(alloc, 1)) return false;
        }

        /* If this would put us over the limit, then evict. */
        const size_t total = total_bytes - old_len + new_len;
        if (total > total_limit) {
            const size_t req_bytes = total - total_limit;
            /* log.info("evicting images to make space for {} bytes", req_bytes) */
            if (!evictImageExcept(alloc, s, req_bytes, Maybe<uint32_t>(img.id))) {
                /* log.warn("failed to evict enough images for required bytes") */
                return false;
            }
        }

        const ImageMap::GetOrPutResult gop = images.getOrPutAssumeCapacity(img.id);

        /* log.debug("addImage image={}", img.withoutData()) */

        if (gop.found_existing) {
            /* Retransmitting a specific image ID replaces the old image and all
             * of its placements, as required by the Kitty graphics protocol. */
            removePlacementsByImageId(s, img.id);

            /* Relative placements parented to the removed placements go too. */
            (void)removeOrphans(s, nullptr);

            /* Replacing an image drops its animation frames with it,
             * implementing the protocol rule that retransmitting the
             * base image resets the animation. */
            total_bytes -= gop.value_ptr->storageSize();
            gop.value_ptr->deinit(alloc);
        }

        *gop.value_ptr = img;
        gop.value_ptr->metadata.placement_count = 0;
        total_bytes += new_len;

        /* Stamp the stored image with a fresh generation. This gives
         * every add/replace a unique stamp even when the same image ID
         * is retransmitted with identical dimensions, so consumers
         * (e.g. renderer texture caches) can detect content changes. */
        markMutated();
        gop.value_ptr->generation = generation;
        return true;
    }

    /* Add an image whose decoded payload bytes have not arrived yet.
     * Returns the token required to complete this exact transmission.
     *
     * Wisp: false is OutOfMemory; the token comes back through `out`. */
    bool addPendingImage(Allocator alloc, Screen *s, const Image &img, PendingImage *out) {
        assert(img.data.tag == Image::Data::Tag::pending);
        if (!addImage(alloc, s, img)) return false;
        Image stored;
        const bool have = images.get(img.id, &stored);
        assert(have); /* Wisp: upstream's `.?` */
        (void)have;
        out->id = stored.id;
        out->generation = stored.generation;
        return true;
    }

    /* Add a placement for a given image. The caller must verify in advance
     * the image exists to prevent memory corruption. On success, storage
     * owns `p`; on error, the caller retains ownership. The screen must own
     * any tracked pin in `p` and in the placement it replaces.
     *
     * Wisp: false is OutOfMemory. */
    bool addPlacement(Allocator alloc, Screen *s, uint32_t image_id, uint32_t placement_id,
                      const Placement &p) {
        assert(images.getPtr(image_id) != nullptr);
        /* log.debug("placement image_id={} placement_id={} placement={}") */

        /* When we add a placement, take this opportunity to reap garbage.
         * Garbage are placements that disappeared off scrollback (were
         * pruned). We don't clear Kitty state in the hot path so we do
         * this opportunistically here. */
        reapGarbagePlacements(s);

        /* The important piece here is that the placement ID needs to
         * be marked internal if it is zero. This allows multiple placements
         * to be added for the same image. If it is non-zero, then it is
         * an external placement ID and we can only have one placement
         * per (image id, placement id) pair. */
        PlacementKey key;
        key.image_id = image_id;
        if (placement_id == 0) {
            /* Wisp: upstream's `defer self.next_internal_placement_id +%= 1`
             * inside the labeled block yields the pre-increment value. */
            const uint32_t internal_id = next_internal_placement_id;
            next_internal_placement_id += 1; /* `+%=` */
            key.placement_id = PlacementId::internal(internal_id);
        } else {
            key.placement_id = PlacementId::external(placement_id);
        }

        PlacementMap::GetOrPutResult gop;
        if (!placements.getOrPut(alloc, key, &gop)) return false;
        if (gop.found_existing) {
            gop.value_ptr->deinit(s);
        } else {
            Image *img = images.getPtr(image_id);
            assert(img != nullptr); /* Wisp: upstream's `.?` */
            /* Wisp: `std.math.add(u30, placement_count, 1) catch` — see the
             * placement_count_max note at the top of this file. */
            if (img->metadata.placement_count >= placement_count_max) {
                placements.removeByPtr(gop.key_ptr);
                return false;
            }
            img->metadata.placement_count += 1;
        }
        *gop.value_ptr = p;

        /* This always mutates */
        markMutated();
        return true;
    }

    /* Adjust placements for a scroll of the terminal's scrolling region
     * by `delta` rows (negative moves content up). This implements the
     * Kitty graphics protocol behavior for scrolls bounded by margins:
     * placements entirely inside the scrolling region move with the content
     * and are clipped when the move would extend them past a margin (deleted
     * when fully clipped), while every other placement (straddling a margin,
     * outside the region, or in the scrollback) must not move at all.
     *
     * This is split into two phases around the row operation because
     * the various scroll implementations move tracked pins in
     * inconsistent ways (some shift the active area so every pin moves,
     * some rotate rows in place and adjust only pins inside the
     * region). We record the desired final position of every placement
     * up front in the returned state, whose end() repositions the pins
     * once the rows have settled, making the result independent of the
     * path taken. Every call must be paired with exactly one end().
     *
     * Callers must skip this entirely when the scrolling region is the
     * full screen: a full-screen scroll moves placements with their
     * anchored rows (possibly into the scrollback) via pin tracking,
     * which already matches kitty's marginless scroll behavior. */
    ScrollMargins scrollMarginsBegin(Terminal *t, int64_t delta, ScrollOp op) {
        Screen *s = t->screens.active;
        /* Wisp: upstream asserts `self == &s.kitty_images`; Screen carries
         * no kitty_images in this configuration. See the header note. */
        ScrollMargins result;
        result.screen = s;

        const int64_t top = (int64_t)t->scrolling_region.top;
        const int64_t bottom = (int64_t)t->scrolling_region.bottom;

        bool mutated = false;
        PlacementMap::Iterator it = placements.iterator();
        PlacementMap::Entry entry;
        while (it.next(&entry)) {
            Placement *p = entry.value_ptr;

            /* Virtual placements follow their placeholder cells and
             * relative placements follow their parents, so neither is
             * ever adjusted by scrolls, matching kitty. */
            if (p->location.tag != Placement::Location::Tag::pin) continue;
            PageList::Pin *pin = p->location.pin;

            /* Pruned placements are reaped by reapGarbagePlacements. */
            if (pin->garbage) continue;

            /* Placements anchored outside the active area (scrollback)
             * are never touched by a scroll within the margins. */
            const Maybe<point::Point> pt = s->pages.pointFromPin(point::Tag::active, *pin);
            if (!pt.has) continue;
            const point::Coordinate coord = pt.value.coord();
            const int64_t y = (int64_t)coord.y;

            /* An in-place operation doesn't touch pins anchored outside
             * the region rows, and a placement anchored there can never
             * be entirely inside the region, so it needs no work at all. */
            if (op == ScrollOp::in_place && (y < top || y > bottom)) continue;

            /* The final position defaults to unmoved; restoring the pin
             * to this exact spot afterwards is what stops the row
             * operations from dragging the placement around. */
            int64_t final_y = y;

            /* Wisp: upstream's `inside:` labeled block. `removed` carries
             * the `continue` that targets the outer loop. */
            bool removed = false;
            do {
                Image image;
                if (!images.get(entry.key_ptr->image_id, &image)) break;
                const Placement::GridSize grid = p->gridSize(image, t);

                /* Unknown geometry (e.g. pixel sizes not yet reported)
                 * means we can't know the extent, so we can't consider
                 * the placement contained. */
                if (grid.rows == 0 || grid.cols == 0) break;

                /* Only placements entirely within the scrolling region
                 * move. Kitty only has top/bottom margins so it only
                 * checks rows; the column check is our generalization
                 * for left/right margins and is trivially true for
                 * full-width regions. */
                if (y < top || y + (int64_t)grid.rows - 1 > bottom) break;
                const int64_t x = (int64_t)coord.x;
                {
                    const int64_t right_edge = x + (int64_t)grid.cols - 1;
                    const int64_t cols_limit = (int64_t)t->cols - 1;
                    const int64_t clamped =
                        right_edge < cols_limit ? right_edge : cols_limit;
                    if (x < (int64_t)t->scrolling_region.left ||
                        clamped > (int64_t)t->scrolling_region.right)
                        break;
                }

                const int64_t new_y = y + delta;
                const int64_t rows = (int64_t)grid.rows;
                const int64_t top_clip = (top - new_y) > 0 ? (top - new_y) : 0;
                const int64_t bottom_clip_raw = new_y + rows - 1 - bottom;
                const int64_t bottom_clip = bottom_clip_raw > 0 ? bottom_clip_raw : 0;
                /* Wisp: upstream's `clip:` labeled block. `visible` is
                 * assigned on every path, so it is never read unset. */
                bool visible;
                do {
                    if (top_clip >= rows || bottom_clip >= rows) {
                        visible = false;
                        break;
                    }

                    if (top_clip > 0) {
                        if (!p->clipTop(image, t, (uint32_t)top_clip, grid.rows)) {
                            visible = false;
                            break;
                        }
                        mutated = true;
                        final_y = top;
                        visible = true;
                        break;
                    }

                    if (bottom_clip > 0) {
                        if (!p->clipBottom(image, t, (uint32_t)bottom_clip, grid.rows)) {
                            visible = false;
                            break;
                        }
                        mutated = true;
                    }

                    final_y = new_y;
                    visible = true;
                } while (0);

                /* Scrolled or clipped entirely out of the region: the
                 * placement is deleted, like kitty. The image itself is
                 * retained for future placements. */
                if (!visible) {
                    removePlacement(s, entry);
                    mutated = true;
                    removed = true;
                }
            } while (0);
            if (removed) continue;

            ScrollMargins::Restore restore;
            restore.pin = pin;
            restore.x = pin->x;
            restore.y = (uint32_t)final_y;
            if (!result.restores.append(s->alloc, restore)) {
                /* OOM: this placement is left to the whims of the
                 * generic pin tracking. Degraded, but safe. */
                /* log.warn("OOM adjusting image placement for scroll") */
                continue;
            }
        }

        if (mutated) {
            /* Placements deleted by clipping may orphan relative placements.
             * Orphans have no pins so this never touches the restores. */
            (void)removeOrphans(s, nullptr);
            markMutated();
        }

        return result;
    }

    /* Reap pin-backed placements whose tracked content has been pruned
     * from history, along with any relative placements orphaned by
     * that, marking the content mutation.
     *
     * Virtual and relative placements have no tracked screen location and
     * are never pruned themselves. Kitty removes placements once they
     * scroll out of retained history. */
    void reapGarbagePlacements(Screen *s) {
        bool removed = false;
        PlacementMap::Iterator it = placements.iterator();
        PlacementMap::Entry entry;
        while (it.next(&entry)) {
            if (entry.value_ptr->location.tag != Placement::Location::Tag::pin) continue;
            PageList::Pin *pin = entry.value_ptr->location.pin;
            if (!pin->garbage) continue;

            removePlacement(s, entry);
            removed = true;
        }
        if (!removed) return;

        /* If we removed a placement, then also remove any orphan
         * children if this was a parent. */
        (void)removeOrphans(s, nullptr);

        markMutated();
    }

    void clearPlacements(Screen *s) {
        PlacementMap::Iterator it = placements.iterator();
        PlacementMap::Entry entry;
        while (it.next(&entry)) entry.value_ptr->deinit(s);
        placements.clearRetainingCapacity();
    }

    void removePlacementsByImageId(Screen *s, uint32_t image_id) {
        PlacementMap::Iterator it = placements.iterator();
        PlacementMap::Entry entry;
        while (it.next(&entry)) {
            if (entry.key_ptr->image_id != image_id) continue;
            removePlacement(s, entry);
        }
    }

    /* Remove a placement by its map entry, releasing any tracked pin
     * and keeping the image's placement count in sync. The entry must
     * point into the placements map (via iteration or getEntry). */
    void removePlacement(Screen *s, const PlacementMap::Entry &entry) {
        entry.value_ptr->deinit(s);
        Image *img = images.getPtr(entry.key_ptr->image_id);
        assert(img != nullptr); /* Wisp: upstream's `.?` */
        assert(img->metadata.placement_count > 0);
        img->metadata.placement_count -= 1;
        placements.removeByPtr(entry.key_ptr);
    }

    /* Remove relative placements whose parent placement no longer
     * exists, keeping a relative placement's lifetime tied to its
     * parent chain. Chains can be several links deep, so this loops
     * until a pass removes nothing to take out entire orphaned
     * subtrees. Returns true if anything was removed.
     *
     * If `delete_unused` is non-null, an image left without placements
     * by the reap is freed using it. This matches uppercase delete
     * semantics; every other removal path retains image data and
     * passes null.
     *
     * This must be called, outside of any placements iteration, after
     * every operation that removes placements. Kitty gets the same
     * effect lazily by dropping unresolvable placements while drawing;
     * we do it eagerly because our renderer never mutates terminal
     * state. */
    bool removeOrphans(Screen *s, const Allocator *delete_unused) {
        bool removed_any = false;
        bool removed = true;
        while (removed) {
            removed = false;
            PlacementMap::Iterator it = placements.iterator();
            PlacementMap::Entry entry;
            while (it.next(&entry)) {
                if (entry.value_ptr->location.tag != Placement::Location::Tag::relative)
                    continue;
                const Placement::Relative rel = entry.value_ptr->location.relative;
                if (placements.contains(rel.parent)) continue;

                /* Parent is gone, remove this placement. */
                const uint32_t image_id = entry.key_ptr->image_id;
                removePlacement(s, entry);
                removed = true;
                removed_any = true;

                if (delete_unused != nullptr) deleteIfUnused(*delete_unused, image_id);
            }
        }

        return removed_any;
    }

    /* Resolve and validate the parent reference (P=/Q=) of a relative
     * placement, returning the concrete key of the parent placement.
     * `child` is the key of the placement being created when it is
     * addressable (an explicit placement ID); null for placements that
     * will receive a fresh internal ID, which nothing can refer to yet.
     *
     * Checks: the parent must exist, the placement must not
     * parent itself, and the resulting ancestor chain must be acyclic
     * and within parent_chain_limit.
     *
     * Wisp: `ParentError!PlacementKey` is a ParentError return (`none`
     * on success) plus an out parameter; `?PlacementKey child` is a
     * Maybe. */
    ParentError resolveParent(Screen *s, const Maybe<PlacementKey> &child,
                              uint32_t parent_image_id, uint32_t parent_placement_id,
                              PlacementKey *out) {
        /* Reap placements whose content has been pruned from history
         * before resolving: a pruned parent must be reported as missing,
         * not accepted and then immediately reaped by addPlacement's own
         * sweep, which would store an orphan. */
        reapGarbagePlacements(s);

        /* If the parent doesn't exist we're already failed. */
        if (!images.contains(parent_image_id)) {
            return ParentError::ParentImageNotFound;
        }

        /* Find the parent. Wisp: upstream's `parent:` labeled block. */
        PlacementKey parent;
        {
            /* An explicit parent placement ID (Q=) selects exactly that
             * placement. */
            if (parent_placement_id > 0) {
                const PlacementKey key(parent_image_id,
                                       PlacementId::external(parent_placement_id));
                if (!placements.contains(key)) {
                    return ParentError::ParentPlacementNotFound;
                }
                parent = key;
            } else {
                /* No Q: pick a placement of the parent image. Kitty picks
                 * the oldest surviving placement; our placement map doesn't
                 * track creation order so we pick by PlacementId.preferredOver
                 * instead. This is unspecified so any behavior here is fine.
                 * In practice the parent image has a single placement, and
                 * clients use Q when it doesn't. */
                Maybe<PlacementId> best = Maybe<PlacementId>::none();
                PlacementMap::KeyIterator it = placements.keyIterator();
                for (PlacementKey *key = it.next(); key != nullptr; key = it.next()) {
                    if (key->image_id != parent_image_id) continue;
                    const PlacementId id = key->placement_id;
                    if (!best.has) {
                        best = Maybe<PlacementId>(id);
                        continue;
                    }
                    if (id.preferredOver(best.value)) best = Maybe<PlacementId>(id);
                }
                if (!best.has) return ParentError::ParentPlacementNotFound;

                parent = PlacementKey(parent_image_id, best.value);
            }
        }

        /* A placement cannot be its own parent. This must be checked
         * before the chain walk so it reports EINVAL, not ECYCLE. */
        if (child.has && parent.eql(child.value)) return ParentError::SelfParent;

        /* Walk the would-be ancestor chain. `depth` counts parent links,
         * starting at one for the link we are about to create. */
        size_t depth = 1;
        PlacementKey key = parent;
        while (true) {
            if (child.has && key.eql(child.value)) return ParentError::Cycle;
            Placement p;
            if (!placements.get(key, &p)) return ParentError::AncestorNotFound;
            /* A pin or virtual placement is the chain root. */
            if (p.location.tag != Placement::Location::Tag::relative) break;
            const Placement::Relative rel = p.location.relative;
            if (depth >= parent_chain_limit) return ParentError::TooDeep;
            depth += 1;
            key = rel.parent;
        }

        *out = parent;
        return ParentError::none;
    }

    /* Resolve a relative placement's parent chain to its root,
     * accumulating the cell offsets (H=/V=) of every link on the way.
     * The placement's position is the root's origin plus the accumulated
     * offsets.
     *
     * Returns null when the chain is broken or too deep. Neither can
     * normally happen for stored placements (resolveParent validates
     * chains and removeOrphans removes broken ones), with one kitty
     * quirk: replacing an ancestor placement can deepen the chains
     * below it beyond parent_chain_limit after the fact.
     *
     * Wisp: `?ResolvedChain` is a Maybe. */
    Maybe<ResolvedChain> resolveChain(const Placement::Relative &rel) const {
        int32_t horizontal = rel.horizontal_offset;
        int32_t vertical = rel.vertical_offset;
        PlacementKey key = rel.parent;
        size_t depth = 1;
        while (true) {
            Placement p;
            if (!placements.get(key, &p)) return Maybe<ResolvedChain>::none();
            switch (p.location.tag) {
            case Placement::Location::Tag::relative: {
                const Placement::Relative parent_rel = p.location.relative;
                if (depth >= parent_chain_limit) return Maybe<ResolvedChain>::none();
                depth += 1;
                horizontal = satAddI32(horizontal, parent_rel.horizontal_offset);
                vertical = satAddI32(vertical, parent_rel.vertical_offset);
                key = parent_rel.parent;
                break;
            }

            case Placement::Location::Tag::pin:
            case Placement::Location::Tag::virtual_: {
                ResolvedChain r;
                r.root_key = key;
                r.root = p;
                r.horizontal_offset = horizontal;
                r.vertical_offset = vertical;
                return Maybe<ResolvedChain>(r);
            }
            }
        }
    }

    /* Returns the placement that a unicode placeholder cell referencing
     * this image/placement ID pair targets, or null if there is none. A
     * zero placement ID targets one of the image's virtual placements,
     * chosen by PlacementId.preferredOver so the choice is stable (an
     * image rarely has more than one). This is the shared lookup used
     * both for sizing placeholder runs and for positioning relative
     * placements whose chain roots at a virtual placement. */
    Maybe<PlaceholderTarget> placeholderTarget(uint32_t image_id,
                                               uint32_t placement_id) const {
        if (placement_id > 0) {
            const PlacementKey key(image_id, PlacementId::external(placement_id));
            Placement p;
            if (!placements.get(key, &p)) return Maybe<PlaceholderTarget>::none();
            PlaceholderTarget r;
            r.key = key;
            r.placement = p;
            return Maybe<PlaceholderTarget>(r);
        }

        Maybe<PlacementKey> best = Maybe<PlacementKey>::none();
        PlacementMap::Iterator it = placements.iterator();
        PlacementMap::Entry entry;
        while (it.next(&entry)) {
            if (entry.key_ptr->image_id != image_id) continue;
            if (entry.value_ptr->location.tag != Placement::Location::Tag::virtual_) continue;
            if (!best.has) {
                best = Maybe<PlacementKey>(*entry.key_ptr);
                continue;
            }
            if (entry.key_ptr->placement_id.preferredOver(best.value.placement_id)) {
                best = Maybe<PlacementKey>(*entry.key_ptr);
            }
        }

        if (!best.has) return Maybe<PlaceholderTarget>::none();
        PlaceholderTarget r;
        r.key = best.value;
        const bool have = placements.get(best.value, &r.placement);
        assert(have); /* Wisp: upstream's `.?` */
        (void)have;
        return Maybe<PlaceholderTarget>(r);
    }

    /* Get an image by its ID. If the image doesn't exist, null is returned. */
    Maybe<Image> imageById(uint32_t image_id) const {
        Image img;
        if (!images.get(image_id, &img)) return Maybe<Image>::none();
        return Maybe<Image>(img);
    }

    /* Get an image by its number. If the image doesn't exist, return null. */
    Maybe<Image> imageByNumber(uint32_t image_number) const {
        Maybe<Image> newest = Maybe<Image>::none();

        ImageMap::Iterator it = images.iterator();
        ImageMap::Entry kv;
        while (it.next(&kv)) {
            if (kv.value_ptr->number == image_number) {
                if (!newest.has || kv.value_ptr->generation > newest.value.generation) {
                    newest = Maybe<Image>(*kv.value_ptr);
                }
            }
        }

        return newest;
    }

    /* Get a mutable pointer to a stored image, by ID or (newest by)
     * number, following the protocol's id/number addressing. Used by
     * the animation commands, which mutate images in place. The
     * pointer is invalidated by any operation that adds or removes
     * images. */
    Image *imagePtrByIdOrNumber(uint32_t image_id, uint32_t image_number) const {
        if (image_id != 0) return images.getPtr(image_id);

        Image *newest = nullptr;
        ImageMap::Iterator it = images.iterator();
        ImageMap::Entry kv;
        while (it.next(&kv)) {
            if (kv.value_ptr->number != image_number) continue;
            if (newest == nullptr || kv.value_ptr->generation > newest->generation) {
                newest = kv.value_ptr;
            }
        }

        return newest;
    }

    /* Record that the displayed content of an image changed without
     * the image being re-added: marks the storage mutated and stamps
     * the image with the fresh generation so consumers (e.g. the
     * renderer's texture cache) replace what they hold. Used when an
     * animation changes which frame is current or edits the current
     * frame's pixels. */
    void markImageContentChanged(Image *img) {
        markMutated();
        img->generation = generation;
    }

    /* Convert a stored image's base data to RGBA in place, adjusting
     * byte accounting. All animation composition happens in RGBA;
     * this is called before the first composition into an image.
     *
     * The pixels are unchanged visually but the stored representation
     * changed, so the image is stamped with a fresh generation.
     *
     * Wisp: false is OutOfMemory. */
    bool convertImageToRgba(Allocator alloc, Image *img) {
        if (img->format == command::Transmission::Format::rgba) return true;
        const uint8_t *old;
        size_t old_len;
        if (!img->data.bytes(&old, &old_len)) return true;

        uint8_t *rgba;
        size_t rgba_len;
        if (pixel::rgbaFromFormat(alloc, img->format, old, old_len, &rgba, &rgba_len) !=
            pixel::Error::none)
            return false;
        total_bytes -= old_len;
        total_bytes += rgba_len;
        img->data.deinit(alloc);
        img->data = Image::Data::initComplete(rgba, rgba_len);
        img->format = command::Transmission::Format::rgba;
        markImageContentChanged(img);
        return true;
    }

    /* Reserve `bytes` of storage for animation frame data belonging
     * to `image_id`, evicting other images if needed, mirroring how
     * image transmission reserves space.
     *
     * Errors if the space cannot be made available. On success the caller
     * owns the reservation and must either attach the frame data to the
     * image or call releaseAnimationBytes.
     *
     * Wisp: false is OutOfMemory. */
    bool reserveAnimationBytes(Allocator alloc, Screen *s, uint32_t image_id, size_t bytes) {
        if (bytes > total_limit) return false;

        const size_t total = total_bytes + bytes;
        if (total > total_limit) {
            const size_t req_bytes = total - total_limit;
            /* Excess this large cannot be recovered by evicting other
             * images (evictImageExcept also requires it). */
            if (req_bytes > total_limit) return false;
            /* log.info("evicting images for animation frame, evicting={}", req_bytes) */
            if (!evictImageExcept(alloc, s, req_bytes, Maybe<uint32_t>(image_id))) {
                /* log.warn("failed to evict enough images for animation frame") */
                return false;
            }
        }

        total_bytes += bytes;
        return true;
    }

    /* Release a reservation made by reserveAnimationBytes, or credit
     * bytes freed by deleting animation frame data. */
    void releaseAnimationBytes(size_t bytes) {
        assert(bytes <= total_bytes);
        total_bytes -= bytes;
    }

    /* Advance every running animation to the frame that should be
     * displayed at `now_ms` and report when the next frame change is
     * due, as a delay in milliseconds relative to `now_ms`. Null
     * means no running animation needs a future tick.
     *
     * `now_ms` is a monotonic timestamp on a clock of the caller's
     * choosing. The same clock must be used for every call. The
     * caller is expected to be the renderer, ticking once per frame
     * build and scheduling a wakeup for the returned delay. */
    Maybe<uint64_t> animationTick(uint64_t now_ms) {
        Maybe<uint64_t> min_delay = Maybe<uint64_t>::none();

        ImageMap::Iterator it = images.iterator();
        ImageMap::Entry entry;
        while (it.next(&entry)) {
            Image *img = entry.value_ptr;

            /* The gates below mirror Kitty's image_is_animatable. */

            /* No animation state was ever attached (plain image). */
            animation::Animation *anim = img->animation_;
            if (anim == nullptr) continue;

            /* Stopped is the initial state of every animation: frames
             * then only change client-driven (a=a c=N), never by time. */
            if (anim->state == animation::Animation::State::stopped) continue;

            /* Only the root frame exists; there is nothing to advance
             * to yet even in the running state. */
            if (anim->frames.items_len == 0) continue;

            /* Unplaced images don't animate. This is our simpler
             * approximation of Kitty's "is actually drawn" visibility
             * gate; it is what stops an image that was transmitted but
             * never placed from waking the renderer forever. */
            if (img->metadata.placement_count == 0) continue;

            /* The base pixel data hasn't arrived yet (e.g. an image
             * restored from a snapshot); nothing can be displayed. */
            if (img->data.isPending()) continue;

            /* A zero total duration means every frame is gapless and
             * no frame can ever be displayed, so the animation can
             * never advance. This also guards the gapless-skip loop
             * below from never terminating. */
            if (anim->durationMs() == 0) continue;

            /* A finite loop budget (a=a v=N) that ran out on an
             * earlier tick froze playback on the last frame for good. */
            if (anim->max_loops > 0 && anim->current_loop >= anim->max_loops) continue;

            /* Wisp: upstream's `shown_at:` labeled block. */
            uint64_t shown_at;
            if (!anim->has_frame_shown_at_ms) {
                /* First tick since playback started (or since the
                 * current frame changed through another path, e.g.
                 * a=a c=N): the frame is considered shown as of now,
                 * and its gap starts counting from here. */
                shown_at = now_ms;
            } else if (anim->frame_shown_at_ms > now_ms) {
                /* A timestamp from the future means the caller's clock
                 * restarted; re-anchor rather than stalling until the
                 * old timestamp comes around again. */
                shown_at = now_ms;
            } else {
                shown_at = anim->frame_shown_at_ms;
            }
            anim->has_frame_shown_at_ms = true;
            anim->frame_shown_at_ms = shown_at;

            /* The current frame is replaced once its gap has elapsed.
             * We advance at most one displayed frame per tick with no
             * catch-up, exactly like Kitty: if ticks lag behind the
             * gaps, the animation slows down rather than skipping. */
            uint64_t next_at = satAddU64(shown_at, anim->gapAt(anim->current_index));
            if (now_ms >= next_at) {
                /* Wisp: upstream's `advance:` labeled block. */
                bool advanced = true;
                /* Walk forward to the next displayable frame. This is
                 * a loop only because gapless (gap=0) frames are never
                 * displayed and are stepped over; the durationMs gate
                 * above guarantees a displayable frame exists. */
                const uint32_t count = anim->frameCount();
                uint32_t idx = anim->current_index;
                while (true) {
                    const uint32_t next = (idx + 1) % count;
                    if (next == 0) {
                        /* Wrapping past the last frame back to the
                         * root. A loading-state (a=a s=2) animation
                         * refuses the wrap: it parks on the last frame
                         * awaiting more frames from the client. */
                        if (anim->state == animation::Animation::State::loading) {
                            advanced = false;
                            break;
                        }

                        /* Each wrap completes a loop; a finite budget
                         * that just ran out parks on the last frame. */
                        anim->current_loop += 1;
                        if (anim->max_loops > 0 && anim->current_loop >= anim->max_loops) {
                            advanced = false;
                            break;
                        }
                    }
                    idx = next;
                    if (anim->gapAt(idx) != 0) break;
                }

                if (advanced) {
                    /* Show the chosen frame: restart its gap timer and
                     * stamp a fresh generation so consumers (the renderer
                     * texture cache, the C API) pick up the new pixels. */
                    anim->current_index = idx;
                    anim->has_frame_shown_at_ms = true;
                    anim->frame_shown_at_ms = now_ms;
                    markImageContentChanged(img);
                    next_at = satAddU64(now_ms, anim->gapAt(idx));
                }
            }

            /* Schedule the next tick. A parked animation left next_at
             * in the past and so never schedules one; it is woken by
             * its trigger instead (a new frame arriving, or an a=a
             * command changing the state). */
            if (next_at > now_ms) {
                const uint64_t delay = next_at - now_ms;
                min_delay = Maybe<uint64_t>(
                    min_delay.has ? (min_delay.value < delay ? min_delay.value : delay) : delay);
            }
        }

        return min_delay;
    }

    /* Clear placements intersecting the active screen, then reclaim every
     * image with no remaining placement. Unlike protocol d=A, a terminal
     * clear also reclaims images that were already unplaced. */
    void clearScreen(Allocator alloc, Terminal *t) {
        const size_t placements_before = placements.count();
        const size_t images_before = images.count();

        /* Delete unused placements and images. Orphaned relative
         * placements must be reaped before the image sweep so that
         * their images are reclaimable too. */
        deleteVisiblePlacements(alloc, t, true);
        (void)removeOrphans(t->screens.active, nullptr);
        ImageMap::Iterator image_it = images.iterator();
        ImageMap::Entry entry;
        while (image_it.next(&entry)) {
            deleteIfUnused(alloc, *entry.key_ptr);
        }

        /* Mark mutated only if things changed. */
        if (placements.count() != placements_before || images.count() != images_before)
            markMutated();
    }

    /* Wisp: `delete` is a C++ keyword. */
    /* Delete placements, images. */
    void delete_(Allocator alloc, Terminal *t, const command::Delete::Action &cmd) {
        /* Deletes only ever remove placements/images, so comparing counts
         * before and after tells us whether anything actually changed.
         * Only then do we mark a mutation, so a delete that matches nothing
         * doesn't dirty the image state or bump the generation.
         *
         * Wisp: upstream's `defer` is this RAII guard; it runs after the
         * removeOrphans call at the bottom, like Zig's. */
        struct MutationGuard {
            ImageStorage *self;
            size_t placements_before;
            size_t images_before;

            MutationGuard(ImageStorage *s)
                : self(s), placements_before(s->placements.count()),
                  images_before(s->images.count()) {}
            ~MutationGuard() {
                if (self->placements.count() != placements_before ||
                    self->images.count() != images_before)
                    self->markMutated();
            }
        } mutation_guard(this);

        typedef command::Delete::Action Action;
        switch (cmd.tag) {
        case Action::Tag::all: deleteVisiblePlacements(alloc, t, cmd.all); break;

        case Action::Tag::id:
            deleteById(alloc, t->screens.active, cmd.id.image_id, cmd.id.placement_id,
                       cmd.id.delete_);
            break;

        case Action::Tag::newest: {
            /* Wisp: upstream's `newest:` labeled block. */
            const Maybe<Image> img = imageByNumber(cmd.newest.image_number);
            if (!img.has) break;
            deleteById(alloc, t->screens.active, img.value.id, cmd.newest.placement_id,
                       cmd.newest.delete_);
            break;
        }

        case Action::Tag::intersect_cursor: {
            deleteIntersecting(alloc, t,
                               point::Point::active(t->screens.active->cursor.x,
                                                    t->screens.active->cursor.y),
                               cmd.intersect_cursor, 0, nullptr);
            break;
        }

        case Action::Tag::intersect_cell: {
            /* Wisp: upstream's `intersect_cell:` labeled block. */
            if (cmd.intersect_cell.x <= 0 || cmd.intersect_cell.y <= 0) {
                /* log.warn("delete intersect cell coords must be at least 1") */
                break;
            }

            const uint32_t cx = cmd.intersect_cell.x - 1;
            const uint32_t cy = cmd.intersect_cell.y - 1;
            if (cx > 0xFFFFu) break; /* std.math.cast(size.CellCountInt, ...) */
            if (cy > 0xFFFFu) break;
            deleteIntersecting(alloc, t,
                               point::Point::active((vtsize::CellCountInt)cx, cy),
                               cmd.intersect_cell.delete_, 0, nullptr);
            break;
        }

        case Action::Tag::intersect_cell_z: {
            /* Wisp: upstream's `intersect_cell_z:` labeled block. */
            if (cmd.intersect_cell_z.x <= 0 || cmd.intersect_cell_z.y <= 0) {
                /* log.warn("delete intersect cell coords must be at least 1") */
                break;
            }

            const uint32_t cx = cmd.intersect_cell_z.x - 1;
            const uint32_t cy = cmd.intersect_cell_z.y - 1;
            if (cx > 0xFFFFu) break;
            if (cy > 0xFFFFu) break;
            deleteIntersecting(alloc, t,
                               point::Point::active((vtsize::CellCountInt)cx, cy),
                               cmd.intersect_cell_z.delete_, cmd.intersect_cell_z.z,
                               &zFilter);
            break;
        }

        case Action::Tag::column: {
            /* Wisp: upstream's `column:` labeled block. */
            if (cmd.column.x <= 0) {
                /* log.warn("delete column must be greater than zero") */
                break;
            }

            const uint32_t x = cmd.column.x - 1;
            PlacementMap::Iterator it = placements.iterator();
            PlacementMap::Entry entry;
            while (it.next(&entry)) {
                const Maybe<Image> img = imageById(entry.key_ptr->image_id);
                if (!img.has) continue;
                Rect r;
                if (!entry.value_ptr->rect(img.value, t, &r)) continue;
                if ((uint32_t)r.top_left.x <= x && (uint32_t)r.bottom_right.x >= x) {
                    removePlacement(t->screens.active, entry);
                    if (cmd.column.delete_) deleteIfUnused(alloc, img.value.id);
                }
            }
            break;
        }

        case Action::Tag::row: {
            /* Wisp: upstream's `row:` labeled block. */
            if (cmd.row.y <= 0) {
                /* log.warn("delete row must be greater than zero") */
                break;
            }

            /* v.y is in active coords so we want to convert it to a pin
             * so we can compare by page offsets. */
            const uint32_t ry = cmd.row.y - 1;
            if (ry > 0xFFFFu) break; /* std.math.cast(size.CellCountInt, v.y - 1) */
            const Maybe<PageList::Pin> target_pin =
                t->screens.active->pages.pin(point::Point::active(0, ry));
            if (!target_pin.has) break;

            PlacementMap::Iterator it = placements.iterator();
            PlacementMap::Entry entry;
            while (it.next(&entry)) {
                const Maybe<Image> img = imageById(entry.key_ptr->image_id);
                if (!img.has) continue;
                Rect r;
                if (!entry.value_ptr->rect(img.value, t, &r)) continue;

                /* We need to copy our pin to ensure we are at least at
                 * the top-left x. */
                PageList::Pin target_pin_copy = target_pin.value;
                target_pin_copy.x = r.top_left.x;
                if (target_pin_copy.isBetween(r.top_left, r.bottom_right)) {
                    removePlacement(t->screens.active, entry);
                    if (cmd.row.delete_) deleteIfUnused(alloc, img.value.id);
                }
            }
            break;
        }

        case Action::Tag::z: {
            PlacementMap::Iterator it = placements.iterator();
            PlacementMap::Entry entry;
            while (it.next(&entry)) {
                switch (entry.value_ptr->location.tag) {
                /* Relative placements carry their own z value and
                 * are matched by z deletes just like kitty does. */
                case Placement::Location::Tag::pin:
                case Placement::Location::Tag::relative: break;

                /* Virtual placeholders cannot delete by z according
                 * to the spec. */
                case Placement::Location::Tag::virtual_: continue;
                }

                if (entry.value_ptr->z == cmd.z.z) {
                    const uint32_t image_id = entry.key_ptr->image_id;
                    removePlacement(t->screens.active, entry);
                    if (cmd.z.delete_) deleteIfUnused(alloc, image_id);
                }
            }
            break;
        }

        case Action::Tag::range: {
            /* Wisp: upstream's `range:` labeled block.
             * Both bounds default to zero when omitted. An inverted range
             * or a zero upper bound is not an error, it just selects
             * nothing: image IDs are always non-zero. */
            if (cmd.range.last == 0 || cmd.range.first > cmd.range.last) break;

            /* Remove matching placements in one pass. */
            {
                PlacementMap::Iterator placement_it = placements.iterator();
                PlacementMap::Entry entry;
                while (placement_it.next(&entry)) {
                    if (entry.key_ptr->image_id < cmd.range.first ||
                        entry.key_ptr->image_id > cmd.range.last)
                        continue;

                    removePlacement(t->screens.active, entry);
                }
            }

            /* Uppercase deletion also frees matching images that are now
             * unused, including images that had no placements initially. */
            if (!cmd.range.delete_) break;
            {
                ImageMap::Iterator image_it = images.iterator();
                ImageMap::Entry entry;
                while (image_it.next(&entry)) {
                    const uint32_t image_id = *entry.key_ptr;
                    if (image_id >= cmd.range.first && image_id <= cmd.range.last) {
                        deleteIfUnused(alloc, image_id);
                    }
                }
            }
            break;
        }

        case Action::Tag::animation_frames:
            deleteAnimationFrame(alloc, t->screens.active, cmd.animation_frames);
            break;
        }

        /* Deleting placements orphans any relative placements parented
         * to them (transitively). Their lifetime is tied to the parent
         * so they are removed as well, and an uppercase delete also
         * frees any image the cascade leaves without placements (the
         * per-branch deleteIfUnused calls above ran while the orphans
         * still counted as placements). */
        bool delete_unused = false;
        switch (cmd.tag) {
        case Action::Tag::all: delete_unused = cmd.all; break;
        case Action::Tag::intersect_cursor: delete_unused = cmd.intersect_cursor; break;
        case Action::Tag::id: delete_unused = cmd.id.delete_; break;
        case Action::Tag::newest: delete_unused = cmd.newest.delete_; break;
        case Action::Tag::animation_frames:
            delete_unused = cmd.animation_frames.delete_;
            break;
        case Action::Tag::intersect_cell: delete_unused = cmd.intersect_cell.delete_; break;
        case Action::Tag::intersect_cell_z:
            delete_unused = cmd.intersect_cell_z.delete_;
            break;
        case Action::Tag::range: delete_unused = cmd.range.delete_; break;
        case Action::Tag::column: delete_unused = cmd.column.delete_; break;
        case Action::Tag::row: delete_unused = cmd.row.delete_; break;
        case Action::Tag::z: delete_unused = cmd.z.delete_; break;
        }
        (void)removeOrphans(t->screens.active, delete_unused ? &alloc : nullptr);
    }

    /* Delete only non-virtual placements that intersect the active screen.
     * The protocol defines d=a/A in terms of visible placements, and an
     * uppercase delete only frees data for images selected by those
     * placements. */
    void deleteVisiblePlacements(Allocator alloc, Terminal *t, bool delete_unused) {
        PlacementMap::Iterator it = placements.iterator();
        PlacementMap::Entry entry;
        while (it.next(&entry)) {
            /* Virtual placements are never selected by visible
             * deletes per the protocol. Relative placements are
             * removed with their parents instead (removeOrphans). */
            if (entry.value_ptr->location.tag != Placement::Location::Tag::pin) continue;
            PageList::Pin *pin = entry.value_ptr->location.pin;
            if (pin->garbage) continue;

            /* Placements anchored in the active area are necessarily visible.
             * Only compute their extent when the anchor is already in history
             * and the bottom edge may still intersect the active area. */
            if (!t->screens.active->pages.pointFromPin(point::Tag::active, *pin).has) {
                const Maybe<Image> image = imageById(entry.key_ptr->image_id);
                if (!image.has) continue;
                Rect r;
                if (!entry.value_ptr->rect(image.value, t, &r)) continue;
                if (!t->screens.active->pages
                         .pointFromPin(point::Tag::active, r.bottom_right)
                         .has)
                    continue;
            }

            const uint32_t image_id = entry.key_ptr->image_id;
            removePlacement(t->screens.active, entry);
            if (delete_unused) deleteIfUnused(alloc, image_id);
        }
    }

    void deleteById(Allocator alloc, Screen *s, uint32_t image_id, uint32_t placement_id,
                    bool delete_unused) {
        bool matched = placement_id == 0;

        /* If no placement, we delete all placements with the ID */
        if (placement_id == 0) {
            removePlacementsByImageId(s, image_id);
        } else {
            PlacementMap::Entry entry;
            if (placements.getEntry(PlacementKey(image_id, PlacementId::external(placement_id)),
                                    &entry)) {
                removePlacement(s, entry);
                matched = true;
            }
        }

        /* A placement ID narrows the selection to that exact placement, so an
         * unmatched selector must not free otherwise-unreferenced image data.
         * https://sw.kovidgoyal.net/kitty/graphics-protocol/#deleting-images */
        if (delete_unused && matched) deleteIfUnused(alloc, image_id);
    }

    /* Delete an animation frame (d=f/F). Deletes never produce
     * responses, so all failures are only logged. Kitty behaviors
     * implemented here: on an image without extra frames a lowercase
     * delete is a no-op while an uppercase delete removes the entire
     * image, placements included; the frame number is clamped to the
     * last frame and zero selects the root frame; deleting the root
     * frame promotes frame 2 to be the new root. */
    void deleteAnimationFrame(Allocator alloc, Screen *s,
                              const command::Delete::Action::AnimationFrames &v) {
        if (v.image_id == 0 && v.image_number == 0) {
            /* log.warn("delete animation frames requires image id or number") */
            return;
        }
        Image *img = imagePtrByIdOrNumber(v.image_id, v.image_number);
        if (img == nullptr) {
            /* log.warn("delete animation frames for unknown image id={} number={}") */
            return;
        }

        /* Wisp: upstream's `anim:` labeled block. */
        animation::Animation *anim = nullptr;
        if (img->animation_ != nullptr && img->animation_->frames.items_len > 0) {
            anim = img->animation_;
        } else {
            /* The image is not (or no longer) an animation. The
             * uppercase delete removes the entire image, even when it
             * still has placements. */
            if (!v.delete_) return;
            removePlacementsByImageId(s, img->id);
            ImageMap::Entry entry;
            const bool have = images.getEntry(img->id, &entry);
            assert(have); /* Wisp: upstream's `.?` */
            (void)have;
            total_bytes -= entry.value_ptr->storageSize();
            entry.value_ptr->deinit(alloc);
            images.removeByPtr(entry.key_ptr);
            return;
        }

        /* Clamp the frame number: zero selects the root frame and
         * values past the end select the last frame. */
        const uint32_t count = anim->frameCount();
        uint32_t number = v.frame < count ? v.frame : count;
        if (number == 0) number = 1;

        if (number == 1) {
            /* Deleting the root frame promotes frame 2 to root. The
             * promoted frame's bytes stay reserved; only the old root
             * data is freed. */
            releaseAnimationBytes(img->data.len());
            img->data.deinit(alloc);
            const animation::Animation::Frame promoted = orderedRemoveFrame(anim, 0);
            img->data = Image::Data::initComplete(promoted.data, promoted.data_len);
            anim->root_gap_ms = promoted.gap_ms;
        } else {
            const animation::Animation::Frame removed = orderedRemoveFrame(anim, number - 2);
            releaseAnimationBytes(removed.data_len);
            alloc.free(removed.data, removed.data_len);
        }

        /* Fix up the current frame. */
        const uint32_t removed_idx = number == 1 ? 0 : number - 2;
        const uint32_t remaining = (uint32_t)anim->frames.items_len;
        if (anim->current_index > remaining) {
            anim->current_index = remaining;
            anim->has_frame_shown_at_ms = false;
            markImageContentChanged(img);
            return;
        }
        if (removed_idx == anim->current_index) {
            anim->has_frame_shown_at_ms = false;
            markImageContentChanged(img);
        } else {
            if (removed_idx < anim->current_index) anim->current_index -= 1;
            markMutated();
        }
    }

    /* Delete an image if it is unused. */
    void deleteIfUnused(Allocator alloc, uint32_t image_id) {
        ImageMap::Entry entry;
        if (!images.getEntry(image_id, &entry)) return;
        if (entry.value_ptr->metadata.placement_count > 0) return;

        total_bytes -= entry.value_ptr->storageSize();
        entry.value_ptr->deinit(alloc);
        images.removeByPtr(entry.key_ptr);
    }

    /* Wisp: the `.intersect_cell_z` filter, upstream's anonymous
     * `struct { fn filter(ctx: i32, p: Placement) bool }.filter`. */
    static bool zFilter(int32_t ctx, const Placement &p) { return p.z == ctx; }

    /* Deletes all placements intersecting a screen point. */
    void deleteIntersecting(Allocator alloc, Terminal *t, const point::Point &p,
                            bool delete_unused, int32_t filter_ctx,
                            bool (*filter)(int32_t, const Placement &)) {
        /* Convert our target point to a pin for comparison. */
        const Maybe<PageList::Pin> target_pin = t->screens.active->pages.pin(p);
        if (!target_pin.has) return;

        PlacementMap::Iterator it = placements.iterator();
        PlacementMap::Entry entry;
        while (it.next(&entry)) {
            const Maybe<Image> img = imageById(entry.key_ptr->image_id);
            if (!img.has) continue;
            Rect r;
            if (!entry.value_ptr->rect(img.value, t, &r)) continue;
            if (r.contains(target_pin.value)) {
                if (filter != nullptr && !filter(filter_ctx, *entry.value_ptr)) continue;
                removePlacement(t->screens.active, entry);
                if (delete_unused) deleteIfUnused(alloc, img.value.id);
            }
        }
    }

    /* Evict image to make space. This will evict the oldest image,
     * prioritizing transient images and unused images as recommended by the
     * published Kitty spec.
     *
     * This will evict as many images as necessary to make space for
     * req bytes. */
    bool evictImage(Allocator alloc, Screen *s, size_t req) {
        return evictImageExcept(alloc, s, req, Maybe<uint32_t>::none());
    }

    /* Wisp: upstream's local `Candidate` struct, hoisted because C++
     * cannot declare a type with methods inside a function and use it
     * in an `assert` before the body. */
    struct Candidate {
        uint32_t id;
        uint64_t generation;

        /* Lower values are evicted first:
         * 0: transient, unused
         * 1: not transient, unused
         * 2: transient, used
         * 3: not transient, used */
        uint8_t priority; /* u2 */

        Candidate() : id(0), generation(0), priority(0) {}

        static Candidate init(const Image *img) {
            Candidate c;
            c.id = img->id;
            c.generation = img->generation;
            c.priority = (uint8_t)((img->metadata.transient ? 0 : 1) +
                                   (img->metadata.placement_count > 0 ? 2 : 0));
            return c;
        }

        bool lessThan(const Candidate &rhs) const {
            if (priority != rhs.priority) return priority < rhs.priority;
            if (generation != rhs.generation) return generation < rhs.generation;
            return id < rhs.id;
        }
    };

    /* Evict images while preserving `exclude_id`, used when replacing an
     * existing image whose old reservation has already been credited. */
    bool evictImageExcept(Allocator alloc, Screen *s, size_t req,
                          const Maybe<uint32_t> &exclude_id) {
        assert(req <= total_limit);

        /* Evicting anything is a content mutation. This matters for the
         * setLimit path in particular, which doesn't otherwise mark it.
         * Evicted placements can also orphan relative placements of
         * other images, which must be reaped along with them.
         *
         * Wisp: upstream's `defer` is this RAII guard, so it also runs
         * on the early `return false` from the candidate search. */
        struct EvictGuard {
            ImageStorage *self;
            Screen *s;
            size_t images_before;

            EvictGuard(ImageStorage *st, Screen *sc)
                : self(st), s(sc), images_before(st->images.count()) {}
            ~EvictGuard() {
                if (self->images.count() != images_before) {
                    (void)self->removeOrphans(s, nullptr);
                    self->markMutated();
                }
            }
        } evict_guard(this, s);

        size_t evicted = 0;
        while (evicted < req) {
            /* Wisp: upstream's `candidate:` labeled block. */
            Candidate c;
            {
                Maybe<Candidate> best = Maybe<Candidate>::none();
                ImageMap::Iterator it = images.iterator();
                ImageMap::Entry kv;
                while (it.next(&kv)) {
                    Image *img = kv.value_ptr;
                    if (exclude_id.has && img->id == exclude_id.value) continue;

                    const Candidate current = Candidate::init(img);
                    if (!best.has || current.lessThan(best.value))
                        best = Maybe<Candidate>(current);
                }
                if (!best.has) return false;
                c = best.value;
            }

            {
                PlacementMap::Iterator p_it = placements.iterator();
                PlacementMap::Entry entry;
                while (p_it.next(&entry)) {
                    if (entry.key_ptr->image_id == c.id) {
                        removePlacement(s, entry);
                    }
                }
            }

            ImageMap::Entry entry;
            const bool have = images.getEntry(c.id, &entry);
            assert(have); /* Wisp: upstream's `.?` */
            (void)have;
            const size_t image_len = entry.value_ptr->storageSize();
            /* log.info("evicting image id={} bytes={}", c.id, image_len) */

            evicted += image_len;
            total_bytes -= image_len;

            entry.value_ptr->deinit(alloc);
            images.removeByPtr(entry.key_ptr);
        }

        return true;
    }
};

/* Wisp: defined out of line; the body names ImageStorage, which is
 * incomplete where the method is declared. */
inline bool ImageStorage::PendingImage::complete(ImageStorage *storage, const uint8_t *data,
                                                 size_t data_len) const {
    Image *img = storage->images.getPtr(id);
    if (img == nullptr) return false;
    if (img->generation != generation) return false;

    size_t expected_len;
    switch (img->data.tag) {
    case Image::Data::Tag::complete: return false;
    case Image::Data::Tag::pending: expected_len = img->data.pending; break;
    default: return false;
    }
    if (data_len != expected_len) return false;

    img->data = Image::Data::initComplete(data, data_len);
    storage->markMutated();
    return true;
}

/* Wisp: the test helper `trackPin` from upstream's test section lives in
 * the port's test file, since it is only used there. */

} /* namespace graphics_storage */
} /* namespace kitty */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_KITTY_GRAPHICS_STORAGE_HPP */
