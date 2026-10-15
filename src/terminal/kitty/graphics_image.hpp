/* Transliterated from Ghostty src/terminal/kitty/graphics_image.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Wisp, differences in shape rather than behavior:
 *   - This file is ported in full. `kitty_graphics` was false when the
 *     surrounding subsystem was ported and this file was omitted; it is
 *     in scope now. Nothing here is gated on `glyph_protocol` or
 *     `tmux_control_mode`, so nothing is omitted for those.
 *   - Error unions become an `Error` enum return (`Error::none` on
 *     success) with the value through an out parameter. Upstream's
 *     `Image.Error` set is hoisted to namespace scope (C++ needs it
 *     before `LoadingImage`, which upstream declares first) and
 *     re-exposed as `Image::Error`. `Allocator.Error` adds the
 *     `OutOfMemory` member.
 *   - `?T` is a `has_x` flag beside the field (`display`, `frame`), or a
 *     null pointer where the payload is a slice (`temporary_directory`).
 *   - `[]const u8` / `[]u8` is a pointer plus a length.
 *   - `union(enum) Data` is a tag enum plus one field per payload.
 *   - `union(enum) Limits.temporary_file` is a bool plus the directory
 *     slice.
 *   - `packed struct(u32) Image.metadata` is a plain struct, like
 *     graphics_command.hpp's `Transmission.Usage`; nothing in this file
 *     bit-casts it, so no bitCast helper is provided.
 *   - `comptime medium` in `readFile` is a runtime parameter and
 *     upstream's `@compileError` for other mediums is an `assert`.
 *   - `log.warn` / `log.debug` have no sink in the port and are comments.
 *   - `defer` / `errdefer` are RAII guards.
 *   - `@import("../../quirks.zig").inlineAssert` is plain `assert`.
 *   - Upstream's declaration order is kept except that `Error`, `Image`
 *     and the file-local `validSharedMemoryName` / `isPathInDir`
 *     helpers are hoisted above `LoadingImage`, which uses all of them.
 *     Upstream declares them after. Order within each group is
 *     upstream's.
 *
 * Wisp, the two external seams:
 *   - `decompressZlib` cannot be transliterated: there is no
 *     `std.compress.flate` here. It calls `zigstd::inflateZlib`
 *     (src/zigstd/inflate.hpp), whose `false` return is upstream's
 *     `error.DecompressionFailed`.
 *   - `sys.decode_png` is an optional function pointer that upstream
 *     itself leaves null for its `.lib` artifact. It is declared in
 *     src/terminal/sys.hpp and left null, so PNG transmission yields
 *     `error.UnsupportedFormat` (for the direct medium) or
 *     `error.UnsupportedMedium` (for every other medium, which fails up
 *     front). That is upstream's own supported configuration.
 *
 * Wisp, the I/O seam: upstream threads a `std.Io` through every call
 * that touches the filesystem. The port has no `std.Io`, so the handful
 * of operations this file needs (open, stat, seek, read, canonical path,
 * delete) are Win32 calls in the `file_io` namespace below and the `io`
 * parameter is dropped from every signature. The declarations are
 * hand-written `dllimport` prototypes, as in zigstd/allocator.hpp and
 * vt/page.hpp, so that no port header pulls in windows.h (which defines
 * RGB and shadows terminal::RGB).
 */

#pragma once
#ifndef WISP_TERMINAL_KITTY_GRAPHICS_IMAGE_HPP
#define WISP_TERMINAL_KITTY_GRAPHICS_IMAGE_HPP

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../vt/fastmem.hpp"
#include "../../vt/page_list.hpp"
#include "../../zigstd/allocator.hpp"
#include "../../zigstd/array_list.hpp"
#include "../../zigstd/inflate.hpp"
#include "../sys.hpp"
#include "graphics_animation.hpp"
#include "graphics_command.hpp"
#include "windows.hpp"

/* Wisp: the Win32 surface the file and temporary-file mediums need. See
 * the I/O seam note above for why these are declared rather than
 * included.
 *
 * Wisp: every prototype here matches windows.h's exactly, so a translation
 * unit that pulls windows.h in as well — which screen.hpp's consumers now
 * do, since Screen carries the Kitty image storage — sees a redeclaration
 * rather than an `extern "C"` overload (MSVC C2733), in either include
 * order. Two consequences:
 *
 *   - the pointer parameters that are only ever passed null name
 *     windows.h's pointee types (`LPSECURITY_ATTRIBUTES`, `LPOVERLAPPED`)
 *     rather than `void *`;
 *   - `SetFilePointerEx` and `GetFileSizeEx` take or return a
 *     `LARGE_INTEGER`, a *union* that cannot be declared compatibly
 *     without windows.h's definition, so `seekTo` and `stat` below use
 *     `SetFilePointer` and `GetFileSize`, whose prototypes are scalar
 *     throughout. Those are the same operations split across a low and a
 *     high 32-bit half, with `GetLastError` disambiguating the in-band
 *     0xFFFFFFFF failure value, as the Win32 docs require. */
struct _SECURITY_ATTRIBUTES;
struct _OVERLAPPED;
extern "C" {
__declspec(dllimport) void *__stdcall CreateFileW(const wchar_t *, unsigned long, unsigned long,
                                                  struct _SECURITY_ATTRIBUTES *, unsigned long,
                                                  unsigned long, void *);
__declspec(dllimport) int __stdcall CloseHandle(void *);
__declspec(dllimport) int __stdcall ReadFile(void *, void *, unsigned long, unsigned long *,
                                             struct _OVERLAPPED *);
__declspec(dllimport) unsigned long __stdcall SetFilePointer(void *, long, long *, unsigned long);
__declspec(dllimport) unsigned long __stdcall GetLastError(void);
__declspec(dllimport) unsigned long __stdcall GetFileType(void *);
__declspec(dllimport) unsigned long __stdcall GetFinalPathNameByHandleW(void *, wchar_t *,
                                                                       unsigned long,
                                                                       unsigned long);
__declspec(dllimport) int __stdcall DeleteFileW(const wchar_t *);
__declspec(dllimport) int __stdcall MultiByteToWideChar(unsigned int, unsigned long, const char *,
                                                        int, wchar_t *, int);
__declspec(dllimport) int __stdcall WideCharToMultiByte(unsigned int, unsigned long,
                                                        const wchar_t *, int, char *, int,
                                                        const char *, int *);
__declspec(dllimport) unsigned long __stdcall GetFileSize(void *, unsigned long *);
__declspec(dllimport) unsigned long __stdcall GetFileAttributesW(const wchar_t *);
}

namespace wisp {
namespace terminal {
namespace kitty {
namespace graphics_image {

typedef ::wisp::zigstd::Allocator Allocator;
typedef ::wisp::zigstd::ArrayListUnmanaged<uint8_t> ByteList;

namespace animation = ::wisp::terminal::kitty::graphics_animation;
namespace command = ::wisp::terminal::kitty::graphics;
namespace kitty_windows = ::wisp::terminal::kitty::windows;
namespace sys = ::wisp::terminal::sys;
namespace fastmem = ::wisp::fastmem;
typedef ::wisp::vt::PageList PageList;

/* Maximum width or height of an image. Taken directly from Kitty. */
static const uint32_t max_dimension = 10000;

/* Maximum size in bytes, taken from Kitty. */
static const size_t max_size = 400 * 1024 * 1024; /* 400MB */

/* Wisp: std.fs.max_path_bytes for Windows: the WTF-8 encoding of the
 * longest path the platform allows. */
static const size_t max_path_bytes = 32767 * 3 + 1;

/* Wisp: std.posix.NAME_MAX. Only used as the default argument to
 * validSharedMemoryName from readSharedMemory. */
static const size_t name_max = 255;

/* Wisp: upstream's `Image.Error` plus `Allocator.Error`. Hoisted to
 * namespace scope because LoadingImage, declared first upstream, returns
 * it. Member order is upstream's, with `none` for the success case. */
enum class Error : uint8_t {
    none,
    InsufficientData,
    InvalidData,
    DecompressionFailed,
    DimensionsRequired,
    DimensionsTooLarge,
    FilePathTooLong,
    TemporaryFileNotInTempDir,
    TemporaryFileNotNamedCorrectly,
    UnsupportedFormat,
    UnsupportedMedium,
    UnsupportedDepth,
    /* Wisp: Allocator.Error. */
    OutOfMemory,
    /* Wisp: upstream's `validatedFilePath` has an inferred error set that
     * includes kitty_windows.Error, and upstream's tests assert on
     * error.NotDriveAbsolute coming out of it. Every other caller maps
     * these to error.InvalidData, exactly as upstream's readFile does. */
    UncPath,
    DevicePath,
    ReservedDeviceName,
    NotDriveAbsolute,
};

/* Wisp: kitty_windows.Error values surfacing through the Error set above. */
inline Error fromWindowsError(::wisp::terminal::kitty::windows::Error err) {
    switch (err) {
    case ::wisp::terminal::kitty::windows::Error::none: return Error::none;
    case ::wisp::terminal::kitty::windows::Error::UncPath: return Error::UncPath;
    case ::wisp::terminal::kitty::windows::Error::DevicePath: return Error::DevicePath;
    case ::wisp::terminal::kitty::windows::Error::ReservedDeviceName:
        return Error::ReservedDeviceName;
    case ::wisp::terminal::kitty::windows::Error::NotDriveAbsolute:
        return Error::NotDriveAbsolute;
    }
    return Error::InvalidData;
}

/* ──────────────────────────────────────────────────────────────────────
 * Wisp: the std.Io.File / std.Io.Dir operations this file uses. Each one
 * is named for the upstream call it stands in for. Everything is a plain
 * bool success return; upstream's error values are all mapped to
 * error.InvalidData by the call sites, so the specific errno is not
 * carried.
 * ────────────────────────────────────────────────────────────────────── */
namespace file_io {

typedef void *Handle;
static const Handle invalid_handle = (Handle)(intptr_t)-1;

/* Wisp: WTF-8 to UTF-16 for the Win32 "W" entry points. The result is
 * heap allocated with `alloc` and NUL terminated; null is OutOfMemory or
 * an unconvertible path. */
inline wchar_t *toWide(Allocator alloc, const char *path, size_t path_len, size_t *out_len) {
    /* An empty path can never name a file; Win32 would reject it anyway
     * but MultiByteToWideChar returns 0 for both "empty" and "error". */
    if (path_len == 0) return nullptr;
    if (path_len > (size_t)0x7FFFFFFF) return nullptr;
    const int n = ::MultiByteToWideChar(65001 /* CP_UTF8 */, 0, path, (int)path_len, nullptr, 0);
    if (n <= 0) return nullptr;
    wchar_t *buf = alloc.allocT<wchar_t>((size_t)n + 1);
    if (buf == nullptr) return nullptr;
    const int written = ::MultiByteToWideChar(65001, 0, path, (int)path_len, buf, n);
    if (written != n) {
        alloc.freeT<wchar_t>(buf, (size_t)n + 1);
        return nullptr;
    }
    buf[n] = 0;
    if (out_len != nullptr) *out_len = (size_t)n;
    return buf;
}

/* Wisp: UTF-16 back to WTF-8, into a caller-provided buffer. */
inline bool fromWide(const wchar_t *src, size_t src_len, char *buf, size_t buf_len,
                     size_t *out_len) {
    if (src_len == 0) {
        *out_len = 0;
        return true;
    }
    if (src_len > (size_t)0x7FFFFFFF || buf_len > (size_t)0x7FFFFFFF) return false;
    const int n =
        ::WideCharToMultiByte(65001, 0, src, (int)src_len, buf, (int)buf_len, nullptr, nullptr);
    if (n <= 0) return false;
    *out_len = (size_t)n;
    return true;
}

/* Wisp: std.Io.Dir.cwd().openFile(io, path, .{}). Shared read access so
 * a client that still holds the file open does not make this fail. */
inline Handle openFile(Allocator alloc, const char *path, size_t path_len) {
    size_t wide_len = 0;
    wchar_t *wide = toWide(alloc, path, path_len, &wide_len);
    if (wide == nullptr) return invalid_handle;
    const Handle h = ::CreateFileW(wide, 0x80000000ul /* GENERIC_READ */,
                                   0x1ul | 0x2ul | 0x4ul /* FILE_SHARE_* */, nullptr,
                                   3ul /* OPEN_EXISTING */, 0x02000000ul /* BACKUP_SEMANTICS */,
                                   nullptr);
    alloc.freeT<wchar_t>(wide, wide_len + 1);
    return h;
}

inline void closeFile(Handle h) {
    if (h != invalid_handle) ::CloseHandle(h);
}

/* Wisp: std.Io.File.stat(io), reduced to the two fields this file reads:
 * whether the kind is `.file` and the size in bytes. */
inline bool stat(Handle h, bool *is_file, uint64_t *size) {
    /* Wisp: GetFileSizeEx in two halves, see the declaration note. */
    unsigned long size_high = 0;
    const unsigned long size_low = ::GetFileSize(h, &size_high);
    if (size_low == 0xFFFFFFFFul && ::GetLastError() != 0ul) return false;
    const long long sz = (long long)(((unsigned long long)size_high << 32) |
                                     (unsigned long long)size_low);
    /* FILE_TYPE_DISK is 1. A pipe or character device is not a regular
     * file, and neither is a directory. */
    const unsigned long type = ::GetFileType(h);
    *is_file = type == 1ul;
    *size = (uint64_t)sz;
    return true;
}

/* Wisp: std.Io.File.realPath(io, buf). GetFinalPathNameByHandleW with
 * VOLUME_NAME_DOS yields a `\\?\`-prefixed path; Zig's realPath strips
 * that prefix, turning `\\?\UNC\server\share` back into
 * `\\server\share`, and so does this. */
inline bool realPath(Allocator alloc, Handle h, char *buf, size_t buf_len, size_t *out_len) {
    const unsigned long need = ::GetFinalPathNameByHandleW(h, nullptr, 0, 0ul /* VOLUME_NAME_DOS */);
    if (need == 0) return false;
    wchar_t *wide = alloc.allocT<wchar_t>((size_t)need + 1);
    if (wide == nullptr) return false;
    const unsigned long got = ::GetFinalPathNameByHandleW(h, wide, need + 1, 0ul);
    bool ok = got != 0 && got <= need;
    size_t wide_len = (size_t)got;
    const wchar_t *start = wide;
    if (ok) {
        /* Strip the `\\?\` prefix, mapping `\\?\UNC\` to `\\`. */
        if (wide_len >= 4 && wide[0] == L'\\' && wide[1] == L'\\' && wide[2] == L'?' &&
            wide[3] == L'\\') {
            if (wide_len >= 8 && (wide[4] == L'U' || wide[4] == L'u') &&
                (wide[5] == L'N' || wide[5] == L'n') && (wide[6] == L'C' || wide[6] == L'c') &&
                wide[7] == L'\\') {
                /* `\\?\UNC\server\share` -> `\\server\share`: reuse the
                 * two leading backslashes by starting at index 6. */
                wide[6] = L'\\';
                start = wide + 6;
                wide_len -= 6;
            } else {
                start = wide + 4;
                wide_len -= 4;
            }
        }
        ok = fromWide(start, wide_len, buf, buf_len, out_len);
    }
    alloc.freeT<wchar_t>(wide, (size_t)need + 1);
    return ok;
}

/* Wisp: std.Io.File.Reader.seekTo(offset). */
inline bool seekTo(Handle h, uint64_t offset) {
    if (offset > (uint64_t)0x7FFFFFFFFFFFFFFFull) return false;
    /* Wisp: SetFilePointerEx in two halves, see the declaration note. */
    long distance_high = (long)(unsigned long)(offset >> 32);
    const unsigned long distance_low = (unsigned long)(offset & 0xFFFFFFFFull);
    const unsigned long moved =
        ::SetFilePointer(h, (long)distance_low, &distance_high, 0ul /* FILE_BEGIN */);
    return !(moved == 0xFFFFFFFFul && ::GetLastError() != 0ul);
}

/* Wisp: one std.Io.Reader read; `*out_read` short of `len` means end of
 * file. */
inline bool read(Handle h, uint8_t *buf, size_t len, size_t *out_read) {
    *out_read = 0;
    while (*out_read < len) {
        size_t want = len - *out_read;
        if (want > 0x40000000u) want = 0x40000000u;
        unsigned long got = 0;
        if (::ReadFile(h, buf + *out_read, (unsigned long)want, &got, nullptr) == 0) return false;
        if (got == 0) break;
        *out_read += (size_t)got;
    }
    return true;
}

/* Wisp: std.Io.Dir.cwd().deleteFile(io, path). */
inline bool deleteFile(Allocator alloc, const char *path, size_t path_len) {
    size_t wide_len = 0;
    wchar_t *wide = toWide(alloc, path, path_len, &wide_len);
    if (wide == nullptr) return false;
    const int ok = ::DeleteFileW(wide);
    alloc.freeT<wchar_t>(wide, wide_len + 1);
    return ok != 0;
}

/* Wisp: std.Io.Dir.cwd().realPathFile(io, path, buf) — open, canonicalize,
 * close. Used for directories as well, which is why openFile passes
 * FILE_FLAG_BACKUP_SEMANTICS. */
inline bool realPathAt(Allocator alloc, const char *path, size_t path_len, char *buf,
                       size_t buf_len, size_t *out_len) {
    const Handle h = openFile(alloc, path, path_len);
    if (h == invalid_handle) return false;
    const bool ok = realPath(alloc, h, buf, buf_len, out_len);
    closeFile(h);
    return ok;
}

} /* namespace file_io */

/* Wisp: the minimum of datastruct/main.zig's LimitedAllocator that
 * decodePng uses. Upstream rejects any single allocation or resize larger
 * than the limit; this vtable has no resize or remap, so only alloc is
 * checked. The full type is not ported.
 *
 * "An allocator that rejects any single allocation or resize larger than
 * a configured byte limit. The limit applies to each request
 * independently, not to the total amount of memory currently allocated."
 */
struct LimitedAllocator {
    Allocator child;
    size_t limit;

    /* Set after an allocation is rejected for exceeding the limit. This
     * remains set until the caller clears it. */
    bool limit_exceeded; /* = false */

    LimitedAllocator(Allocator child_, size_t limit_)
        : child(child_), limit(limit_), limit_exceeded(false) {}

    bool check(size_t size) {
        if (size <= limit) return true;
        limit_exceeded = true;
        return false;
    }

    static uint8_t *allocFn(void *ctx, size_t len, size_t alignment) {
        LimitedAllocator *self = (LimitedAllocator *)ctx;
        if (!self->check(len)) return nullptr;
        return self->child.vtable->alloc(self->child.ptr, len, alignment);
    }
    static void freeFn(void *ctx, uint8_t *memory, size_t len, size_t alignment) {
        LimitedAllocator *self = (LimitedAllocator *)ctx;
        self->child.vtable->free(self->child.ptr, memory, len, alignment);
    }

    Allocator allocator() {
        static const Allocator::VTable vt = {allocFn, freeFn};
        Allocator a = {this, &vt};
        return a;
    }
};

/* Wisp: hoisted above LoadingImage, which calls both. Upstream declares
 * them near the bottom of the file. */

/* Returns whether a name follows the POSIX shared memory name format. */
inline bool validSharedMemoryName(const char *name, size_t name_len, size_t name_max_) {
    if (name_len < 2 || name_len > name_max_ || name[0] != '/') return false;
    for (size_t i = 1; i < name_len; i += 1) {
        const char c = name[i];
        if (c == '/' || c == 0) return false;
    }

    return true;
}

/* Returns true if `path` is `dir` or is contained within it, requiring a
 * path-separator boundary so similarly prefixed directories do not match. */
inline bool isPathInDir(const char *dir, size_t dir_len, const char *path, size_t path_len) {
    /* Wisp: std.mem.startsWith(u8, path, dir). */
    if (dir_len == 0 || path_len < dir_len || memcmp(path, dir, dir_len) != 0) return false;
    /* Wisp: std.fs.path.isSep on Windows, which is kitty_windows.isSep. */
    if (path_len == dir_len || kitty_windows::isSep((uint8_t)dir[dir_len - 1])) return true;
    return kitty_windows::isSep((uint8_t)path[dir_len]);
}

/* Image represents a single image whose metadata is fully known.
 *
 * Complete image data is always fully decoded raw pixels: loading inflates
 * any zlib-compressed payload and decodes PNG into RGBA before an image is
 * completed, so `compression` is always `.none` and `format` is never `.png`
 * for a stored image. Pending image data reserves the exact decoded byte
 * length that will be attached later.
 *
 * Wisp: hoisted above LoadingImage, whose first field is an Image.
 * Upstream declares it after. */
struct Image {
    /* Wisp: upstream's `Image.Error`, which is the namespace-scope enum. */
    typedef ::wisp::terminal::kitty::graphics_image::Error Error;

    /* Wisp: `union(enum)` is a tag enum plus one field per payload. */
    struct Data {
        enum class Tag : uint8_t {
            /* Owned, decoded image bytes. The empty default is not allocated. */
            complete,

            /* Expected decoded byte length for a payload that has not arrived. */
            pending,
        };

        Tag tag; /* = .complete */
        const uint8_t *complete;
        size_t complete_len;
        size_t pending;

        Data() : tag(Tag::complete), complete(nullptr), complete_len(0), pending(0) {}

        /* Wisp: `.{ .complete = data }`. */
        static Data initComplete(const uint8_t *data, size_t data_len) {
            Data r;
            r.tag = Tag::complete;
            r.complete = data;
            r.complete_len = data_len;
            return r;
        }

        /* Wisp: `.{ .pending = expected_len }`. */
        static Data initPending(size_t expected_len) {
            Data r;
            r.tag = Tag::pending;
            r.pending = expected_len;
            return r;
        }

        /* Bytes reserved against the storage limit. */
        size_t len() const {
            switch (tag) {
            case Tag::complete: return complete_len;
            case Tag::pending: return pending;
            }
            return 0;
        }

        /* Returns decoded bytes when the payload is complete.
         * Wisp: `?[]const u8` is a bool return plus out parameters. */
        bool bytes(const uint8_t **out, size_t *out_len) const {
            switch (tag) {
            case Tag::complete:
                *out = complete;
                *out_len = complete_len;
                return true;
            case Tag::pending: return false;
            }
            return false;
        }

        bool isPending() const { return tag == Tag::pending; }

        void deinit(Allocator alloc) {
            switch (tag) {
            case Tag::complete:
                if (complete_len > 0) alloc.freeT<uint8_t>((uint8_t *)complete, complete_len);
                break;
            case Tag::pending: break;
            }
        }
    };

    /* Wisp: `packed struct(u32)`; the padding bits of upstream's
     * `placement_count: u30` are kept inside that field's width. */
    struct Metadata {
        /* The image's transient usage hint, used to prioritize eviction. */
        bool transient; /* = false */

        /* Set this if the image was loaded without an ID or number. Such
         * images must not receive responses. Kitty gives these client ID
         * 0 (unaddressable); our storage keys everything by one public
         * u32 ID, so they get an ID from the upper half of the range
         * that is guaranteed unused at assignment time, but a client
         * that explicitly transmits that ID later can still replace
         * them. */
        bool implicit_id; /* = false */

        /* Number of placements referencing this image. */
        uint32_t placement_count; /* u30 = 0 */

        Metadata() : transient(false), implicit_id(false), placement_count(0) {}
    };

    uint32_t id;                              /* = 0 */
    uint32_t number;                          /* = 0 */
    uint32_t width;                           /* = 0 */
    uint32_t height;                          /* = 0 */
    command::Transmission::Format format;     /* = .rgb */
    command::Transmission::Compression compression; /* = .none */
    Data data;                                /* = .{ .complete = "" } */
    Metadata metadata;                        /* = .{} */

    /* Unique, monotonically increasing stamp assigned each time an
     * image is added to (or replaced in) an ImageStorage. A changed
     * generation for a given image ID means the image contents may
     * have changed, even if the dimensions and byte length are the
     * same (e.g. a retransmission of the same ID). Stamps order by
     * transmission time. Zero means "never stored".
     *
     * For animated images this also changes whenever the frame that
     * should be displayed changes (advance, edit, or delete of the
     * current frame), since consumers key texture caches off it. */
    uint64_t generation; /* = 0 */

    /* Animation state, non-null once any animation command (a=f,
     * a=a) has attached animation state to this image. Owned by the
     * image; replaced/retransmitted images drop it, which implements
     * the protocol's "retransmission resets the animation" rule.
     *
     * This is only ever attached to images stored in an ImageStorage
     * and must only be mutated through the storage's own pointer
     * (Image values are copied around freely; copies share this
     * pointer and never own it). */
    animation::Animation *animation_; /* = null */

    Image()
        : id(0), number(0), width(0), height(0), format(command::Transmission::Format::rgb),
          compression(command::Transmission::Compression::none), data(), metadata(), generation(0),
          animation_(nullptr) {}

    void deinit(Allocator alloc) {
        data.deinit(alloc);
        if (animation_ != nullptr) {
            animation::Animation *anim = animation_;
            anim->deinit(alloc);
            alloc.destroy(anim);
            animation_ = nullptr;
        }
    }

    /* The pixel data that should be displayed for this image. For an
     * animated image this is the current animation frame; otherwise
     * (and for the root frame) it is the image's own data. */
    Data renderData() const {
        if (animation_ != nullptr) {
            const animation::Animation *anim = animation_;
            if (anim->current_index > 0) {
                const animation::Animation::Frame &f = anim->frames.items[anim->current_index - 1];
                return Data::initComplete(f.data, f.data_len);
            }
        }

        return data;
    }

    /* The pixel data of the given 1-based animation frame number, or
     * null if the frame doesn't exist. Frame 1 (the root frame)
     * always exists as long as the image data is complete, even for
     * images without animation state.
     *
     * The returned slice is owned by the image (or its animation)
     * and remains valid until the image or frame is mutated.
     *
     * Wisp: `?[]const u8` is a bool return plus out parameters. */
    bool frameData(uint32_t number_, const uint8_t **out, size_t *out_len) const {
        switch (number_) {
        case 0: return false;
        case 1: return data.bytes(out, out_len);
        default: {
            const animation::Animation *anim = animation_;
            if (anim == nullptr) return false;
            /* Minus 2 because frame is 1-based and frame 1 is the
             * image base data, so the animation frames start at frame 2. */
            const uint32_t idx = number_ - 2;
            if ((size_t)idx >= anim->frames.items_len) return false;
            *out = anim->frames.items[idx].data;
            *out_len = anim->frames.items[idx].data_len;
            return true;
        }
        }
    }

    /* Total bytes of pixel data reserved against the storage limit
     * for this image: the base data plus any animation frames. */
    size_t storageSize() const {
        size_t total = data.len();
        if (animation_ != nullptr) total += animation_->frameBytes();
        return total;
    }

    /* Mostly for logging */
    Image withoutData() const {
        Image copy = *this;
        if (copy.data.tag == Data::Tag::complete) copy.data = Data::initComplete(nullptr, 0);
        return copy;
    }
};

/* An image that is still being loaded. The image should be initialized
 * using init on the first chunk and then addData for each subsequent
 * chunk. Once all chunks have been added, complete should be called
 * to finalize the image. */
struct LoadingImage {
    struct FrameContext {
        /* The frame parameters from the initial a=f command. Chunked
         * continuations only contribute payload bytes; all parameters
         * come from the command that started the load, matching the
         * protocol's requirement that chunks repeat a=f. */
        command::AnimationFrameLoading cmd;

        /* The generation of the target image when the load began.
         * A different generation at completion means the image was
         * replaced or evicted mid-transmission and the frame must be
         * discarded rather than composed onto the wrong image. */
        uint64_t image_generation;

        FrameContext() : cmd(), image_generation(0) {}
    };

    /* The limits of the Kitty Graphics protocol we should allow.
     *
     * This can be used to restrict the type of images and other
     * parameters for resource or security reasons. Note that depending
     * on how libghostty is compiled, some of these may be fully unsupported
     * and ignored (e.g. "file" on wasm32-freestanding). */
    struct Limits {
        bool file;
        /* Wisp: `union(enum) { enabled: struct { directory }, disabled }`
         * is the tag as a bool plus the directory slice. */
        bool temporary_file_enabled;
        /* The directory to expect temporary files in. */
        const char *temporary_file_directory;
        size_t temporary_file_directory_len;
        bool shared_memory;

        Limits()
            : file(false), temporary_file_enabled(false), temporary_file_directory(nullptr),
              temporary_file_directory_len(0), shared_memory(false) {}

        /* Enables all filesystem-related image transmission mediums. `path`
         * is the temporary directory to expect files in when files are
         * transmitted using said medium. */
        static Limits allWithTempDir(const char *path, size_t path_len) {
            Limits r;
            r.file = true;
            r.temporary_file_enabled = true;
            r.temporary_file_directory = path;
            r.temporary_file_directory_len = path_len;
            r.shared_memory = true;
            return r;
        }

        static Limits direct() {
            Limits r;
            r.file = false;
            r.temporary_file_enabled = false;
            r.shared_memory = false;
            return r;
        }
    };

    struct SharedMemoryRange {
        size_t start;
        size_t end;

        SharedMemoryRange() : start(0), end(0) {}
    };

    /* The in-progress image. The first chunk must have all the metadata
     * so this comes from that initially. */
    Image image;

    /* The data that is being built up. */
    ByteList data; /* = .empty */

    /* This is non-null when a transmit and display command is given
     * so that we display the image after it is fully loaded. */
    bool has_display; /* = false (null) */
    command::Display display;

    /* This is non-null when this load is an animation frame
     * transmission (a=f) rather than a new image. On completion the
     * data is composed into the target image's animation instead of
     * being stored as an image. */
    bool has_frame; /* = false (null) */
    FrameContext frame;

    /* Quiet is the quiet settings for the initial load command. This is
     * used if q isn't set on subsequent chunks. */
    command::Command::Quiet quiet;

    /* Response identifiers from the initial load command. Subsequent chunks
     * omit these, so completion responses must use the saved values. */
    command::Response response; /* = .{} */

    /* The temporary directory for file transmission (null means that
     * temporary directory transmission is disabled).
     * Wisp: a null pointer is upstream's null. */
    const char *temporary_directory;
    size_t temporary_directory_len;

    LoadingImage()
        : image(), data(), has_display(false), display(), has_frame(false), frame(),
          quiet(command::Command::Quiet::no), response(), temporary_directory(nullptr),
          temporary_directory_len(0) {}

    /* Initialize a chunked immage from the first image transmission.
     * If this is a multi-chunk image, this should only be the FIRST
     * chunk. */
    static Error init(Allocator alloc, const command::Command *cmd, Limits limits,
                      LoadingImage *out) {
        /* Build our initial image from the properties sent via the control.
         * These can be overwritten by the data loading process. For example,
         * PNG loading sets the width/height from the data. */
        command::Transmission t;
        const bool have_t = cmd->transmission(&t);
        assert(have_t); /* Wisp: upstream's `.?` */
        (void)have_t;

        /* Validated here rather than while parsing so the response can
         * carry the image id, matching Kitty's initialize_load_data. */
        if (t.format_unknown) return Error::UnsupportedFormat;
        LoadingImage result;
        result.image.id = t.image_id;
        result.image.number = t.image_number;
        result.image.width = t.width;
        result.image.height = t.height;
        result.image.compression = t.compression;
        result.image.format = t.format;
        result.image.metadata.transient = t.usage.transient;

        result.has_display = cmd->display(&result.display);
        result.quiet = cmd->quiet;
        result.response.id = t.image_id;
        result.response.image_number = t.image_number;
        result.response.placement_id = t.placement_id;
        if (limits.temporary_file_enabled) {
            result.temporary_directory = limits.temporary_file_directory;
            result.temporary_directory_len = limits.temporary_file_directory_len;
        } else {
            result.temporary_directory = nullptr;
            result.temporary_directory_len = 0;
        }

        /* Special case for the direct medium, we just add the chunk directly. */
        if (t.medium == command::Transmission::Medium::direct) {
            const Error err = result.addData(alloc, cmd->data, cmd->data_len);
            if (err != Error::none) {
                result.deinit(alloc);
                return err;
            }
            *out = result;
            return Error::none;
        }

        /* Verify our capabilities and limits allow this. */
        {
            /* Special case if we don't support decoding PNGs and the format
             * is a PNG we can save a lot of memory/effort buffering the
             * data but failing up front. */
            if (t.format == command::Transmission::Format::png && sys::decode_png() == nullptr) {
                result.deinit(alloc);
                return Error::UnsupportedMedium;
            }

            /* Verify the medium is allowed */
            switch (t.medium) {
            case command::Transmission::Medium::direct:
                assert(false); /* Wisp: upstream's `unreachable` */
                break;
            case command::Transmission::Medium::file:
                if (!limits.file) {
                    result.deinit(alloc);
                    return Error::UnsupportedMedium;
                }
                break;
            case command::Transmission::Medium::temporary_file:
                if (!limits.temporary_file_enabled) {
                    result.deinit(alloc);
                    return Error::UnsupportedMedium;
                }
                break;
            case command::Transmission::Medium::shared_memory:
                if (!limits.shared_memory) {
                    result.deinit(alloc);
                    return Error::UnsupportedMedium;
                }
                break;
            }
        }

        /* Otherwise, the payload data is guaranteed to be a path. */

        /* Wisp: upstream rejects an embedded NUL here only for
         * `builtin.os.tag != .windows`; this port targets Windows, so the
         * check is omitted. The Win32 path conversion in file_io stops at
         * the NUL and the open then fails with InvalidData anyway. */

        /* Depending on the medium, load the data from the path. */
        {
            Error err = Error::none;
            switch (t.medium) {
            case command::Transmission::Medium::direct:
                assert(false); /* handled above */
                break;
            case command::Transmission::Medium::file:
                err = result.readFile(command::Transmission::Medium::file, alloc, t,
                                      (const char *)cmd->data, cmd->data_len);
                break;
            case command::Transmission::Medium::temporary_file:
                err = result.readFile(command::Transmission::Medium::temporary_file, alloc, t,
                                      (const char *)cmd->data, cmd->data_len);
                break;
            case command::Transmission::Medium::shared_memory:
                err = result.readSharedMemory(alloc, t, (const char *)cmd->data, cmd->data_len);
                break;
            }
            if (err != Error::none) {
                result.deinit(alloc);
                return err;
            }
        }

        *out = result;
        return Error::none;
    }

    /* Reads the data from a shared memory segment. */
    Error readSharedMemory(Allocator alloc, command::Transmission t, const char *path,
                           size_t path_len) {
        (void)alloc;
        (void)t;
        (void)path;
        (void)path_len;

        /* android does not support POSIX shared memory.
         * windows is currently unsupported, does it support shm? */
        return Error::UnsupportedMedium;

        /* Wisp: the POSIX body (validSharedMemoryName, shm_open,
         * fstat, sharedMemoryRange, mmap, appendSlice) is unreachable on
         * Windows — upstream returns error.UnsupportedMedium from the
         * first comptime branch — so it is not transliterated.
         * `sharedMemoryRange` and `validSharedMemoryName` are ported
         * because upstream's tests call them directly. */
    }

    /* Returns the byte range to copy from a shared memory object. */
    Error sharedMemoryRange(command::Transmission t, size_t stat_size,
                            SharedMemoryRange *out) const {
        /* Wisp: `?usize` is a has-flag plus the value. */
        bool have_expected_size = false;
        size_t expected_size = 0;
        switch (image.format) {
        /* PNG dimensions come from the decoded data. */
        case command::Transmission::Format::png: have_expected_size = false; break;

        /* Validate before multiplying because protocol dimensions are
         * u32 values and may otherwise overflow in safe builds. */
        case command::Transmission::Format::gray:
        case command::Transmission::Format::gray_alpha:
        case command::Transmission::Format::rgb:
        case command::Transmission::Format::rgba: {
            if (image.width > max_dimension || image.height > max_dimension) {
                return Error::DimensionsTooLarge;
            }

            const size_t bpp = (size_t)command::Transmission::formatBpp(image.format);
            have_expected_size = true;
            expected_size = (size_t)image.width * (size_t)image.height * bpp;
            break;
        }
        }

        /* Get our start offset and validate its within the range of
         * the statted data.
         * Wisp: std.math.cast(usize, u32) cannot fail on a 64-bit host. */
        const size_t start = (size_t)t.offset;
        if (start > stat_size) return Error::InvalidData;

        /* Validate that our length is within the stat range too. */
        const size_t available = stat_size - start;
        size_t data_size;
        if (t.size > 0) {
            data_size = (size_t)t.size;
        } else if (image.compression == command::Transmission::Compression::none &&
                   have_expected_size) {
            data_size = expected_size;
        } else {
            data_size = available;
        }
        if (data_size > max_size || data_size > available) {
            return Error::InvalidData;
        }

        /* data_size <= available guarantees this addition cannot overflow. */
        out->start = start;
        out->end = start + data_size;
        return Error::none;
    }

    /* Reads the data from a temporary file and returns it. This allocates
     * and does not free any of the data, so the caller must free it.
     *
     * This will also delete the temporary file if it is in a safe location.
     *
     * Wisp: `medium` is comptime upstream. */
    Error readFile(command::Transmission::Medium medium, Allocator alloc, command::Transmission t,
                   const char *path, size_t path_len) {
        switch (medium) {
        case command::Transmission::Medium::file:
        case command::Transmission::Medium::temporary_file: break;
        default:
            /* Wisp: upstream's @compileError("readFile only supports file
             * and temporary_file"). */
            assert(false);
            return Error::UnsupportedMedium;
        }

        /* Some Windows paths are dangerous to even open, so the raw path
         * is checked before the open. The canonical path of the opened
         * file is checked again in validatedFilePath. See kitty_windows
         * for what is refused and why. */
        if (kitty_windows::checkPath(path, path_len) != kitty_windows::Error::none) {
            /* log.warn("invalid image path: {}", .{err}) */
            return Error::InvalidData;
        }

        /* The canonical path buffer is sized for the longest path the
         * platform allows, which is about 96 KiB on Windows, so it is
         * heap allocated rather than placed on the stack of whichever
         * host thread feeds the stream. It is allocated before the open
         * so the deferred temporary file deletion below can still use it. */
        uint8_t *abs_buf = alloc.alloc(max_path_bytes);
        if (abs_buf == nullptr) return Error::OutOfMemory;
        AbsBufGuard abs_guard(alloc, abs_buf);

        /* Open our file right away before we do validation. This avoids
         * TOCTOU issues. */
        const file_io::Handle file = file_io::openFile(alloc, path, path_len);
        if (file == file_io::invalid_handle) {
            /* log.warn("failed to open image file: {}", .{err}) */
            return Error::InvalidData;
        }

        /* We'll populate a delete path if this is a temporary file. */
        FileGuard file_guard(alloc, file);

        /* Derive the path from the open handle so the file we validate is the
         * exact file we read. Resolving a path before opening it would allow a
         * cooperating process to swap a symlink or directory entry in between. */
        const char *abs_path = nullptr;
        size_t abs_path_len = 0;
        if (validatedFilePath(alloc, file, (char *)abs_buf, max_path_bytes, &abs_path,
                              &abs_path_len) != Error::none) {
            /* log.warn("failed to validate image file path: {}", .{err}) */
            return Error::InvalidData;
        }

        /* Temporary file logic */
        if (medium == command::Transmission::Medium::temporary_file) {
            assert(temporary_directory != nullptr);
            bool in_temp_dir = false;
            const Error err = isPathInTempDir(alloc, temporary_directory, temporary_directory_len,
                                              abs_path, abs_path_len, &in_temp_dir);
            if (err != Error::none) return err;
            if (!in_temp_dir) return Error::TemporaryFileNotInTempDir;
            /* Wisp: std.mem.indexOf(u8, abs_path, "tty-graphics-protocol") */
            if (!containsSubstring(abs_path, abs_path_len, "tty-graphics-protocol"))
                return Error::TemporaryFileNotNamedCorrectly;
            file_guard.delete_path = abs_path;
            file_guard.delete_path_len = abs_path_len;
        }

        /* File must be a regular file */
        {
            bool is_file = false;
            uint64_t stat_size = 0;
            if (file_io::stat(file, &is_file, &stat_size)) {
                if (!is_file) {
                    /* log.warn("file is not a regular file kind={}", .{stat.kind}) */
                    return Error::InvalidData;
                }
            } else {
                /* log.warn("failed to stat file: {}", .{err}) */
                return Error::InvalidData;
            }
        }

        if (t.offset > 0) {
            if (!file_io::seekTo(file, (uint64_t)t.offset)) {
                /* log.warn("failed to seek to offset {}: {}", .{ t.offset, err }) */
                return Error::InvalidData;
            }
        }

        /* Read the file */
        ByteList managed; /* = .empty */
        ManagedGuard managed_guard(alloc, &managed);
        if (t.size > 0) {
            /* S is the exact number of bytes to read, not a maximum:
             * https://sw.kovidgoyal.net/kitty/graphics-protocol/#local-client */
            const size_t size = (size_t)t.size;
            if (size > max_size) return Error::InvalidData;

            /* Read exact size */
            uint8_t *buf = alloc.alloc(size);
            if (buf == nullptr) return Error::OutOfMemory;
            size_t got = 0;
            if (!file_io::read(file, buf, size, &got) || got != size) {
                /* log.warn("failed to read image file: {}", .{err}) */
                alloc.free(buf, size);
                return Error::InvalidData;
            }
            managed.items = buf;
            managed.items_len = size;
            managed.capacity = size;
        } else {
            /* Wisp: reader.appendRemaining(alloc, &managed, .limited(max_size)),
             * open-coded as a grow-and-read loop. */
            static const size_t chunk = 4096;
            for (;;) {
                if (managed.items_len >= max_size) {
                    /* Nothing more is allowed; if there are still bytes
                     * left the limit was exceeded. */
                    uint8_t probe = 0;
                    size_t probe_got = 0;
                    if (!file_io::read(file, &probe, 1, &probe_got)) return Error::InvalidData;
                    if (probe_got != 0) {
                        /* log.warn("failed to read image file: {?}", .{buf_reader.err}) */
                        return Error::InvalidData;
                    }
                    break;
                }
                size_t want = chunk;
                if (managed.items_len + want > max_size) want = max_size - managed.items_len;
                if (!managed.ensureUnusedCapacity(alloc, want)) return Error::OutOfMemory;
                size_t got = 0;
                if (!file_io::read(file, managed.items + managed.items_len, want, &got)) {
                    /* log.warn("failed to read image file: {?}", .{buf_reader.err}) */
                    return Error::InvalidData;
                }
                managed.items_len += got;
                if (got < want) break;
            }
        }

        /* Set our data */
        assert(data.items_len == 0);
        data.items = managed.items;
        data.items_len = managed.items_len;
        data.capacity = managed.capacity;
        managed_guard.release();
        return Error::none;
    }

    /* Returns the canonical path of an open file after applying the file
     * transmission blocklist.
     *
     * Wisp: `![]const u8` is an Error return plus out parameters, and the
     * canonical path is written into the caller's `buf`. */
    static Error validatedFilePath(Allocator alloc, file_io::Handle file, char *buf, size_t buf_len,
                                   const char **out, size_t *out_len) {
        size_t path_len = 0;
        if (!file_io::realPath(alloc, file, buf, buf_len, &path_len)) return Error::InvalidData;
        const char *path = buf;

        const kitty_windows::Error werr = kitty_windows::checkCanonicalPath(path, path_len);
        if (werr != kitty_windows::Error::none) {
            *out = path;
            *out_len = path_len;
            return fromWindowsError(werr);
        }
        *out = path;
        *out_len = path_len;
        return Error::none;

        /* Wisp: upstream's POSIX blocklist (/proc/, /sys/, /dev/ except
         * /dev/shm/) is behind `builtin.os.tag != .windows` and is not
         * reachable in this port. */
    }

    /* Returns true if path appears to be in a temporary directory.
     * Copies logic from Kitty. */
    static Error isPathInTempDir(Allocator alloc, const char *dir, size_t dir_len,
                                 const char *path, size_t path_len, bool *out) {
        if (isPathInDir("/tmp", 4, path, path_len)) {
            *out = true;
            return Error::none;
        }
        if (isPathInDir("/dev/shm", 8, path, path_len)) {
            *out = true;
            return Error::none;
        }
        if (isPathInDir(dir, dir_len, path, path_len)) {
            *out = true;
            return Error::none;
        }

        /* The temporary dir is sometimes a symlink. On macOS for
         * example /tmp is /private/var/... On Windows the directory a
         * host passes (typically GetTempPath output) regularly differs
         * from the canonical path in case or uses 8.3 short names, and
         * resolving it covers both. The buffer is heap allocated for
         * the reason given in readFile. */
        uint8_t *buf = alloc.alloc(max_path_bytes);
        if (buf == nullptr) return Error::OutOfMemory;
        AbsBufGuard guard(alloc, buf);
        size_t real_dir_len = 0;
        if (!file_io::realPathAt(alloc, dir, dir_len, (char *)buf, max_path_bytes,
                                 &real_dir_len)) {
            *out = false;
            return Error::none;
        }
        if (isPathInDir((const char *)buf, real_dir_len, path, path_len)) {
            *out = true;
            return Error::none;
        }

        *out = false;
        return Error::none;
    }

    void deinit(Allocator alloc) {
        image.deinit(alloc);
        data.deinit(alloc);
    }

    void destroy(Allocator alloc) {
        deinit(alloc);
        alloc.destroy(this);
    }

    /* Adds a chunk of data to the image. Use this if the image
     * is coming in chunks (the "m" parameter in the protocol). */
    Error addData(Allocator alloc, const uint8_t *chunk, size_t chunk_len) {
        /* If no data, skip */
        if (chunk_len == 0) return Error::none;

        /* If our data would get too big, return an error */
        if (data.items_len + chunk_len > max_size) {
            /* log.warn("image data too large max_size={}", .{max_size}) */
            return Error::InvalidData;
        }

        /* Ensure we have enough room to add the data
         * to the end of the ArrayList before doing so. */
        if (!data.ensureUnusedCapacity(alloc, chunk_len)) return Error::OutOfMemory;

        const size_t start_i = data.items_len;
        data.items_len = start_i + chunk_len;
        fastmem::copy<uint8_t>(data.items + start_i, chunk, chunk_len);
        return Error::none;
    }

    /* Complete the chunked image, returning a completed image. */
    Error complete(Allocator alloc, Image *out) {
        Image *img = &image;

        /* Decompress the data if it is compressed. */
        {
            const Error err = decompress(alloc);
            if (err != Error::none) return err;
        }

        /* Decode the png if we have to */
        if (img->format == command::Transmission::Format::png) {
            const Error err = decodePng(alloc);
            if (err != Error::none) return err;
        }

        /* Validate our dimensions. */
        if (img->width == 0 || img->height == 0) return Error::DimensionsRequired;
        if (img->width > max_dimension || img->height > max_dimension)
            return Error::DimensionsTooLarge;

        /* Data length must be what we expect. */
        const uint8_t bpp = command::Transmission::formatBpp(img->format);
        const size_t expected_len = (size_t)img->width * (size_t)img->height * (size_t)bpp;
        const size_t actual_len = data.items_len;
        if (has_frame) {
            /* Kitty allows animation frames to exceed their expected length
             * and just truncates it. Not sure if thats expected but lets
             * allow it too. */
            if (actual_len < expected_len) {
                /* log.warn("insufficient frame data image id={} expected_len={}
                 * actual_len={}", ...) */
                return Error::InsufficientData;
            }
            data.items_len = expected_len;
        } else if (actual_len != expected_len) {
            /* log.warn("unexpected length image id={} width={} height={} bpp={}
             * expected_len={} actual_len={}", ...) */
            return Error::InvalidData;
        }

        /* Everything looks good, copy the image data over. */
        Image result = image;
        uint8_t *owned = nullptr;
        size_t owned_len = 0;
        if (!data.toOwnedSlice(alloc, &owned, &owned_len)) return Error::OutOfMemory;
        result.data = Image::Data::initComplete(owned, owned_len);
        /* Wisp: upstream's `errdefer result.deinit(alloc)` guards nothing;
         * no operation after this point can fail. */
        image = Image();
        *out = result;
        return Error::none;
    }

    /* Wisp: debugDump is a Debug-only helper that writes self.data to a
     * file for capturing test vectors. It is not ported: nothing in the
     * port calls it and it needs a file writer the I/O seam does not
     * provide. */

    /* Decompress the data in-place. */
    Error decompress(Allocator alloc) {
        switch (image.compression) {
        case command::Transmission::Compression::none: return Error::none;
        case command::Transmission::Compression::zlib_deflate: return decompressZlib(alloc);
        }
        return Error::none;
    }

    Error decompressZlib(Allocator alloc) {
        /* Wisp: std.compress.flate cannot be transliterated (there is no
         * Zig stdlib here). zigstd::inflateZlib is the port's inflate;
         * its `false` return is upstream's error.DecompressionFailed,
         * which also covers upstream's max_size read limit. */
        ByteList list; /* = .empty */
        if (!::wisp::zigstd::inflateZlib(alloc, data.items, data.items_len, max_size, &list)) {
            /* log.warn("failed to read decompressed data: {?}", .{stream.err}) */
            list.deinit(alloc);
            return Error::DecompressionFailed;
        }

        /* Empty our current data list, take ownership over managed array list */
        data.deinit(alloc);
        data = list;

        /* Make sure we note that our image is no longer compressed */
        image.compression = command::Transmission::Compression::none;
        return Error::none;
    }

    /* Decode the data as PNG. This will also updated the image dimensions. */
    Error decodePng(Allocator alloc) {
        assert(image.format == command::Transmission::Format::png);

        const sys::DecodePngFn decode_png_fn = sys::decode_png();
        if (decode_png_fn == nullptr) return Error::UnsupportedFormat;

        LimitedAllocator limited(alloc, max_size);
        const Allocator decode_alloc = limited.allocator();
        sys::Image result;
        const sys::DecodeError derr = decode_png_fn(decode_alloc, data.items, data.items_len,
                                                    &result);
        if (derr != sys::DecodeError::none) {
            switch (derr) {
            case sys::DecodeError::InvalidData: return Error::InvalidData;
            case sys::DecodeError::OutOfMemory:
                if (limited.limit_exceeded) return Error::InvalidData;
                return Error::OutOfMemory;
            default: return Error::InvalidData;
            }
        }
        DecodedGuard decoded_guard(decode_alloc, result.data, result.data_len);

        if (result.data_len > max_size) {
            /* log.warn("png image too large size={} max_size={}", ...) */
            return Error::InvalidData;
        }

        /* Replace our data */
        data.deinit(alloc);
        data = ByteList(); /* = .empty */
        if (!data.ensureUnusedCapacity(alloc, result.data_len)) return Error::OutOfMemory;
        if (!data.appendSlice(alloc, result.data, result.data_len)) return Error::OutOfMemory;

        /* Store updated image dimensions */
        image.width = result.width;
        image.height = result.height;
        image.format = command::Transmission::Format::rgba;
        return Error::none;
    }

    /* ── Wisp: the RAII guards standing in for upstream's defer /
     * errdefer, and two small open-coded std.mem helpers. ────────────── */

    /* Wisp: `defer alloc.free(abs_buf)` */
    struct AbsBufGuard {
        Allocator alloc;
        uint8_t *buf;
        AbsBufGuard(Allocator a, uint8_t *b) : alloc(a), buf(b) {}
        ~AbsBufGuard() { alloc.free(buf, max_path_bytes); }
    };

    /* Wisp: upstream's combined `defer { file.close(io); if (delete_path)
     * |p| deleteFile(p); }`. */
    struct FileGuard {
        Allocator alloc;
        file_io::Handle file;
        const char *delete_path; /* = null */
        size_t delete_path_len;
        FileGuard(Allocator a, file_io::Handle f)
            : alloc(a), file(f), delete_path(nullptr), delete_path_len(0) {}
        ~FileGuard() {
            file_io::closeFile(file);
            if (delete_path != nullptr) {
                /* log.warn("failed to delete temporary file: {}", .{err}) */
                (void)file_io::deleteFile(alloc, delete_path, delete_path_len);
            }
        }
    };

    /* Wisp: `errdefer managed.deinit(alloc)` */
    struct ManagedGuard {
        Allocator alloc;
        ByteList *list;
        ManagedGuard(Allocator a, ByteList *l) : alloc(a), list(l) {}
        void release() { list = nullptr; }
        ~ManagedGuard() {
            if (list != nullptr) list->deinit(alloc);
        }
    };

    /* Wisp: `defer decode_alloc.free(result.data)` */
    struct DecodedGuard {
        Allocator alloc;
        uint8_t *data;
        size_t data_len;
        DecodedGuard(Allocator a, uint8_t *d, size_t n) : alloc(a), data(d), data_len(n) {}
        ~DecodedGuard() { alloc.free(data, data_len); }
    };

    /* Wisp: std.mem.indexOf(u8, haystack, needle) != null, with the
     * needle a NUL-terminated literal as the one call site passes one. */
    static bool containsSubstring(const char *haystack, size_t haystack_len, const char *needle) {
        const size_t needle_len = strlen(needle);
        if (needle_len == 0) return true;
        if (haystack_len < needle_len) return false;
        for (size_t i = 0; i + needle_len <= haystack_len; i += 1) {
            if (memcmp(haystack + i, needle, needle_len) == 0) return true;
        }
        return false;
    }
};

/* The rect taken up by some image placement, in grid cells. This will
 * be rounded up to the nearest grid cell since we can't place images
 * in partial grid cells. */
struct Rect {
    PageList::Pin top_left;
    PageList::Pin bottom_right;

    Rect() : top_left(), bottom_right() {}

    /* Returns true if the grid cell is inside this rectangle. Pin.isBetween
     * compares page order, so its interior rows intentionally do not constrain
     * x. Check the column independently and use isBetween only for the row. */
    bool contains(const PageList::Pin &cell) const {
        if (cell.x < top_left.x || cell.x > bottom_right.x) return false;

        PageList::Pin row = cell;
        row.x = top_left.x;
        return row.isBetween(top_left, bottom_right);
    }
};

} /* namespace graphics_image */
} /* namespace kitty */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_KITTY_GRAPHICS_IMAGE_HPP */
