/* Transliterated from Ghostty src/terminal/kitty/windows.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Wisp, differences in shape rather than behavior:
 *   - `Error!void` is a plain `Error` return with an added `none` member.
 *   - `[]const u8` is a `const char *` plus a length.
 *   - upstream declares checkPath and checkCanonicalPath before the
 *     isSep / isReservedDeviceName helpers they call; C++ needs the
 *     helpers first, so the two groups are swapped. Order within each
 *     group is upstream's.
 *   - `std.mem.splitAny`, `std.mem.findAny`, `std.mem.trimEnd`,
 *     `std.ascii.eqlIgnoreCase`, `std.ascii.isAlphabetic` and
 *     `std.ascii.isDigit` are open-coded as file-local helpers.
 *
 * Windows path checks for the Kitty graphics protocol file and
 * temporary file mediums.
 *
 * On POSIX the file transmission blocklist is a few prefixes
 * (`/proc`, `/sys`, `/dev`) applied to the canonical path of the
 * opened file. Windows needs more care because some paths are
 * dangerous to even open: a UNC path makes this process resolve and
 * authenticate to the named SMB server, and the Win32 device and NT
 * object namespaces reach raw volumes and named pipes, where the
 * open or the read can block.
 *
 * Everything here is pure string logic, so it is tested on every host.
 */

#pragma once
#ifndef WISP_TERMINAL_KITTY_WINDOWS_HPP
#define WISP_TERMINAL_KITTY_WINDOWS_HPP

#include <stddef.h>
#include <stdint.h>

namespace wisp {
namespace terminal {
namespace kitty {
namespace windows {

enum class Error : uint8_t {
    /* Wisp: the success case of upstream's `Error!void`. */
    none,

    /* A UNC path such as `\\server\share`, in any mix of `\` and `/`.
     * Opening it resolves the host name and authenticates to the
     * server over SMB. */
    UncPath,

    /* A Win32 device path (`\\.\`, `\\?\`) or an NT object path
     * (`\??\`). These reach raw volumes, named pipes, and through
     * `GLOBALROOT` any kernel object. */
    DevicePath,

    /* A component names a reserved DOS device such as CON or COM1.
     * Windows resolves these to the device from inside any directory. */
    ReservedDeviceName,

    /* The canonical path of an opened file is not `X:\...`, so the
     * file is not on a local volume with a drive letter. The canonical
     * path of a file on an SMB share is a UNC path, for example. */
    NotDriveAbsolute,
};

/* Wisp: std.ascii.isAlphabetic, kept local so this header needs no
 * locale-sensitive <ctype.h>. */
inline bool isAlphabetic(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/* Wisp: std.ascii.isDigit. */
inline bool isDigit(uint8_t c) { return c >= '0' && c <= '9'; }

/* Wisp: std.ascii.toLower for a single byte. */
inline uint8_t toLower(uint8_t c) { return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + 32) : c; }

/* Wisp: std.ascii.eqlIgnoreCase, with `b` a NUL-terminated literal as
 * every call site passes one. */
inline bool eqlIgnoreCase(const char *a, size_t a_len, const char *b) {
    size_t i = 0;
    for (; i < a_len; i += 1) {
        if (b[i] == 0) return false;
        if (toLower((uint8_t)a[i]) != toLower((uint8_t)b[i])) return false;
    }
    return b[i] == 0;
}

/* Returns true if `c` is a Windows path separator. */
inline bool isSep(uint8_t c) {
    /* std.fs.path.PathType.windows.isSep */
    return c == '/' || c == '\\';
}

/* Returns true if a path component names a reserved DOS device: CON,
 * PRN, AUX, NUL, COM0 to COM9, LPT0 to LPT9, and the superscript digit
 * variants. */
inline bool isReservedDeviceName(const char *component, size_t component_len) {
    /* The name ends at the extension or stream separator. */
    /* Wisp: std.mem.findAny(u8, component, ".:") orelse component.len */
    size_t end = component_len;
    for (size_t i = 0; i < component_len; i += 1) {
        if (component[i] == '.' || component[i] == ':') {
            end = i;
            break;
        }
    }

    /* Wisp: std.mem.trimEnd(u8, component[0..end], " ") — only the end
     * is trimmed, so the stem still starts at the component. */
    const char *stem = component;
    size_t stem_len = end;
    while (stem_len > 0 && stem[stem_len - 1] == ' ') stem_len -= 1;
    if (stem_len < 3) return false;

    const char *base = stem;
    const char *rest = stem + 3;
    const size_t rest_len = stem_len - 3;
    if (rest_len == 0) {
        return eqlIgnoreCase(base, 3, "CON") || eqlIgnoreCase(base, 3, "PRN") ||
               eqlIgnoreCase(base, 3, "AUX") || eqlIgnoreCase(base, 3, "NUL");
    }

    if (!eqlIgnoreCase(base, 3, "COM") && !eqlIgnoreCase(base, 3, "LPT")) return false;
    switch (rest_len) {
    case 1: return isDigit((uint8_t)rest[0]);
    /* U+00B9, U+00B2, U+00B3 (sup 1, sup 2, sup 3) encoded as WTF-8. */
    case 2:
        return (uint8_t)rest[0] == 0xC2 &&
               ((uint8_t)rest[1] == 0xB9 || (uint8_t)rest[1] == 0xB2 || (uint8_t)rest[1] == 0xB3);
    default: return false;
    }
}

/* Check a path supplied by a client before it is opened. This never
 * touches the filesystem. A plain local path of any shape passes:
 * drive-absolute, drive-relative, rooted, or relative. Whether it
 * resolves to a local regular file is decided after the open with
 * `checkCanonicalPath`. */
inline Error checkPath(const char *path, size_t path_len) {
    if (path_len >= 2 && isSep((uint8_t)path[0]) && isSep((uint8_t)path[1])) {
        /* `\\?\` and `\\.\` are the Win32 device namespace. Anything
         * else after two separators is a server name. */
        if (path_len >= 4 && (path[2] == '?' || path[2] == '.') && isSep((uint8_t)path[3]))
            return Error::DevicePath;
        return Error::UncPath;
    }

    if (path_len >= 4 && isSep((uint8_t)path[0]) && path[1] == '?' && path[2] == '?' &&
        isSep((uint8_t)path[3]))
        return Error::DevicePath;

    /* Wisp: std.mem.splitAny(u8, path, "\\/") — it yields the empty
     * components a leading, trailing or doubled separator produces, and
     * one empty component for an empty path, so the loop body runs at
     * least once. */
    size_t start = 0;
    for (;;) {
        size_t i = start;
        while (i < path_len && !isSep((uint8_t)path[i])) i += 1;
        if (isReservedDeviceName(path + start, i - start)) return Error::ReservedDeviceName;
        if (i >= path_len) break;
        start = i + 1;
    }

    return Error::none;
}

/* Returns true if `path` is a drive-absolute path (`C:\...`), the
 * only shape the canonical path of a file on a local volume with a
 * drive letter takes. */
inline bool isDriveAbsolute(const char *path, size_t path_len) {
    return path_len >= 3 && isAlphabetic((uint8_t)path[0]) && path[1] == ':' && path[2] == '\\';
}

/* Check the canonical path of an opened file, as produced by
 * `GetFinalPathNameByHandle`. The file must be on a local volume with
 * a drive letter, and the checks of `checkPath` apply as well. */
inline Error checkCanonicalPath(const char *path, size_t path_len) {
    if (!isDriveAbsolute(path, path_len)) return Error::NotDriveAbsolute;
    return checkPath(path, path_len);
}

} /* namespace windows */
} /* namespace kitty */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_KITTY_WINDOWS_HPP */
