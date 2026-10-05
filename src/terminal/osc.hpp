/* Transliterated from Ghostty src/terminal/osc.zig, src/terminal/osc/
 * encoding.zig and these files under src/terminal/osc/parsers/:
 * change_window_title.zig, change_window_icon.zig, hyperlink.zig,
 * report_pwd.zig, mouse_shape.zig, clipboard_operation.zig, color.zig,
 * kitty_color.zig, kitty_dnd_protocol.zig, rxvt_extension.zig,
 * kitty_text_sizing.zig, context_signal.zig, iterm2.zig,
 * kitty_clipboard_protocol.zig, semantic_prompt.zig, osc9.zig,
 * kitty_desktop_notification.zig; and src/terminal/osc/kitty_metadata.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * OSC (Operating System Command) related functions and types.
 *
 * OSC is another set of control sequences for terminal programs that start
 * with "ESC ]". Unlike CSI or standard ESC sequences, they may contain strings
 * and other irregular formatting so a dedicated parser is created to handle
 * it.
 *
 * TRANSLITERATION, see parser.hpp for the Zig-to-C++ mapping. Additionally:
 *
 *   ?Allocator          a flag; allocation is malloc/realloc/free
 *   [:0]const u8        ZStr, a pointer and length whose byte at len is NUL
 *   std.Io.Writer       Writer, which models the fixed and allocating writers
 *                       closely enough that upstream's tests on buffer
 *                       capacity hold exactly
 *
 * Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: every file under osc/parsers/ is transliterated here. Upstream's
 * types that live beside each parser (kitty_text_sizing.OSC,
 * context_signal.Command, ...) are namespaces ahead of Command; the parse
 * functions are under parsers::. kitty/color.zig is kitty/color.hpp and
 * os/string_encoding.zig is ../os/string_encoding.hpp.
 */

#pragma once
#ifndef WISP_TERMINAL_OSC_HPP
#define WISP_TERMINAL_OSC_HPP

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "color.hpp"
#include "kitty/color.hpp"
#include "../os/string_encoding.hpp"
#include "../datastruct/segmented_list.hpp"

namespace wisp {
namespace terminal {
namespace osc {

/* ─── encoding.zig ───────────────────────────────────────────────────────── */

/* Wisp: std.unicode.Utf8View.init — strict UTF-8 validation. Rejects
 * overlong encodings, surrogates and anything above U+10FFFF. On success
 * calls back once per codepoint. */
template <typename F>
inline bool utf8_view_each(const uint8_t *s, size_t len, F f) {
    size_t i = 0;
    while (i < len) {
        const uint8_t b = s[i];
        uint32_t cp;
        size_t n;
        uint32_t min;

        if (b < 0x80) { cp = b; n = 1; min = 0; }
        else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; n = 2; min = 0x80; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; n = 3; min = 0x800; }
        else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; n = 4; min = 0x10000; }
        else return false;

        if (i + n > len) return false;
        for (size_t k = 1; k < n; k++) {
            if ((s[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if (cp < min) return false;                        /* overlong */
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;    /* surrogate */
        if (cp > 0x10FFFF) return false;

        if (!f(cp)) return false;
        i += n;
    }
    return true;
}

/* Kitty defines "Escape code safe UTF-8" as valid UTF-8 with the
 * additional requirement of not containing any C0 escape codes
 * (0x00-0x1f), DEL (0x7f) and C1 escape codes (0x80-0x9f).
 *
 * Used by OSC 66 (text sizing) and OSC 99 (Kitty notifications).
 *
 * See: https://sw.kovidgoyal.net/kitty/desktop-notifications/#safe-utf8 */
inline bool isSafeUtf8(const char *s, size_t len) {
    return utf8_view_each((const uint8_t *)s, len, [](uint32_t cp) {
        if (cp <= 0x1f || cp == 0x7f || (cp >= 0x80 && cp <= 0x9f)) {
            return false;
        }
        return true;
    });
}

inline bool isSafeUtf8(const char *s) { return isSafeUtf8(s, strlen(s)); }

/* ─── osc.zig ────────────────────────────────────────────────────────────── */

/* Wisp: [:0]const u8. */
struct ZStr {
    const char *ptr;
    size_t      len;

    ZStr() : ptr(""), len(0) {}
    ZStr(const char *p, size_t n) : ptr(p), len(n) {}

    bool eql(const char *s) const {
        return strlen(s) == len && memcmp(ptr, s, len) == 0;
    }
};

/* The terminator used to end an OSC command. For OSC commands that demand
 * a response, we try to match the terminator used in the request since that
 * is most likely to be accepted by the calling program. */
enum class Terminator : uint8_t {
    /* The preferred string terminator is ESC followed by \ */
    st,

    /* Some applications and terminals use BELL (0x07) as the string
     * terminator. */
    bel,
};

/* Initialize the terminator based on the last byte seen. If the
 * last byte is a BEL then we use BEL, otherwise we just assume ST.
 *
 * Wisp: ?u8 is has_ch plus ch. */
inline Terminator terminator_init(bool has_ch, uint8_t ch) {
    if (!has_ch) return Terminator::st;
    return ch == 0x07 ? Terminator::bel : Terminator::st;
}

/* The terminator as a string. This is static memory so it doesn't
 * need to be freed. */
inline const char *terminator_string(Terminator t) {
    return t == Terminator::st ? "\x1b\\" : "\x07";
}

/* ─── osc/parsers/color.zig: types ──────────────────────────────────────── */

namespace color {

/* The possible operations we support for colors. */
enum class Operation : uint8_t {
    osc_4,
    osc_5,
    osc_10,
    osc_11,
    osc_12,
    osc_13,
    osc_14,
    osc_15,
    osc_16,
    osc_17,
    osc_18,
    osc_19,
    osc_104,
    osc_105,
    osc_110,
    osc_111,
    osc_112,
    osc_113,
    osc_114,
    osc_115,
    osc_116,
    osc_117,
    osc_118,
    osc_119,
};

struct Target {
    enum class Tag : uint8_t { palette, special, dynamic };
    Tag tag;
    uint8_t palette;
    ::wisp::terminal::Special special;
    ::wisp::terminal::Dynamic dynamic;

    static Target makePalette(uint8_t idx) {
        Target t = Target();
        t.tag = Tag::palette;
        t.palette = idx;
        return t;
    }
    static Target makeSpecial(::wisp::terminal::Special sp) {
        Target t = Target();
        t.tag = Tag::special;
        t.special = sp;
        return t;
    }
    static Target makeDynamic(::wisp::terminal::Dynamic d) {
        Target t = Target();
        t.tag = Tag::dynamic;
        t.dynamic = d;
        return t;
    }

    bool eql(const Target &o) const {
        if (tag != o.tag) return false;
        switch (tag) {
            case Tag::palette: return palette == o.palette;
            case Tag::special: return special == o.special;
            case Tag::dynamic: return dynamic == o.dynamic;
        }
        return false;
    }

    Target()
        : tag(Tag::palette), palette(0),
          special(::wisp::terminal::Special::bold),
          dynamic(::wisp::terminal::Dynamic::foreground) {}
};

struct ColoredTarget {
    Target target;
    ::wisp::terminal::RGB color;
};

/* A single operation related to the terminal color palette. */
struct Request {
    enum class Tag : uint8_t { set, query, reset, reset_palette, reset_special };
    Tag tag;
    ColoredTarget set;
    Target query;
    Target reset;

    Request() : tag(Tag::reset_palette), set(), query(), reset() {}

    bool eql(const Request &o) const {
        if (tag != o.tag) return false;
        switch (tag) {
            case Tag::set:
                return set.target.eql(o.set.target) && set.color.eql(o.set.color);
            case Tag::query: return query.eql(o.query);
            case Tag::reset: return reset.eql(o.reset);
            case Tag::reset_palette:
            case Tag::reset_special:
                return true;
        }
        return false;
    }
};

/* A segmented list is used to avoid copying when many operations
 * are given in a single OSC. In most cases, OSC 4/104/etc. send
 * very few so the prealloc is optimized for that.
 *
 * The exact prealloc value is chosen arbitrarily assuming most
 * color ops have very few. If we can get empirical data on more
 * typical values we can switch to that. */
typedef ::wisp::datastruct::SegmentedList<Request, 2> List;

} /* namespace color */

/* ─── osc/parsers/kitty_text_sizing.zig: types ─────────────────────────── */

/* Kitty's text sizing protocol (OSC 66)
 * Specification: https://sw.kovidgoyal.net/kitty/text-sizing-protocol/ */
namespace kitty_text_sizing {

static const size_t max_payload_length = 4096;

enum class VAlign : uint8_t {
    top,
    bottom,
    center,
};

enum class HAlign : uint8_t {
    left,
    right,
    center,
};

struct OSC {
    uint8_t scale;         /* u3 = 1, 1 - 7 */
    uint8_t width;         /* u3 = 0, 0 - 7 (0 means default) */
    uint8_t numerator;     /* u4 = 0 */
    uint8_t denominator;   /* u4 = 0 */
    VAlign valign;         /* = .top */
    HAlign halign;         /* = .left */
    ZStr text;

    OSC()
        : scale(1), width(0), numerator(0), denominator(0),
          valign(VAlign::top), halign(HAlign::left), text() {}

    enum class UpdateError : uint8_t { none, UnknownKey, InvalidValue };

    UpdateError update(uint8_t key, const char *value, size_t value_len) {
        /* All values are numeric, so we can do a small hack here.
         * Wisp: std.fmt.parseInt(u4, value, 10); '+' accepted, '_'
         * separators not reproduced (see parsers::color::parse_u9). */
        size_t i = 0;
        if (i < value_len && value[i] == '+') i++;
        if (i >= value_len) return UpdateError::InvalidValue;
        unsigned v = 0;
        for (; i < value_len; i++) {
            if (value[i] < '0' || value[i] > '9') return UpdateError::InvalidValue;
            v = v * 10 + (unsigned)(value[i] - '0');
            if (v > 15) return UpdateError::InvalidValue;
        }

        switch (key) {
            case 's':
                if (v == 0) return UpdateError::InvalidValue;
                if (v > 7) return UpdateError::InvalidValue;
                scale = (uint8_t)v;
                break;
            case 'w':
                if (v > 7) return UpdateError::InvalidValue;
                width = (uint8_t)v;
                break;
            case 'n': numerator = (uint8_t)v; break;
            case 'd': denominator = (uint8_t)v; break;
            case 'v':
                if (v > 2) return UpdateError::InvalidValue;
                valign = (VAlign)v;
                break;
            case 'h':
                if (v > 2) return UpdateError::InvalidValue;
                halign = (HAlign)v;
                break;
            default: return UpdateError::UnknownKey;
        }
        return UpdateError::none;
    }
};

} /* namespace kitty_text_sizing */

/* ─── osc/parsers/context_signal.zig: types ────────────────────────────── */

/* OSC 3008: Hierarchical Context Signalling (UAPI spec)
 * Specification: https://uapi-group.org/specifications/specs/osc_context/
 *
 * OSC 3008 allows programs to signal context changes to the terminal emulator.
 * Each context has an identifier and metadata fields. Contexts are hierarchical
 * and form a stack. */
namespace context_signal {

/* Maximum length of a context identifier (per spec). */
static const size_t max_context_id_len = 64;

/* Wisp: std.meta.stringToEnum over a name table. */
inline bool string_to_enum(const char *const *names, size_t count,
                           const char *s, size_t len, size_t *out) {
    for (size_t i = 0; i < count; i++) {
        if (strlen(names[i]) == len && memcmp(names[i], s, len) == 0) {
            *out = i;
            return true;
        }
    }
    return false;
}

/* Context types defined by the specification. */
enum class ContextType : uint8_t {
    boot,
    container,
    vm,
    elevate,
    chpriv,
    subcontext,
    remote,
    shell,
    command,
    app,
    service,
    session,
};

/* Wisp: ?ContextType is the bool return plus *out. */
inline bool ContextType_parse(const char *value, size_t len, ContextType *out) {
    static const char *const names[] = {
        "boot", "container", "vm", "elevate", "chpriv", "subcontext",
        "remote", "shell", "command", "app", "service", "session",
    };
    size_t i;
    if (!string_to_enum(names, 12, value, len, &i)) return false;
    *out = (ContextType)i;
    return true;
}
inline bool ContextType_parse(const char *value, ContextType *out) {
    return ContextType_parse(value, strlen(value), out);
}

/* Exit status for the `exit` end-sequence field. */
enum class ExitStatus : uint8_t {
    success,
    failure,
    crash,
    interrupt,
};

inline bool ExitStatus_parse(const char *value, size_t len, ExitStatus *out) {
    static const char *const names[] = { "success", "failure", "crash", "interrupt" };
    size_t i;
    if (!string_to_enum(names, 4, value, len, &i)) return false;
    *out = (ExitStatus)i;
    return true;
}
inline bool ExitStatus_parse(const char *value, ExitStatus *out) {
    return ExitStatus_parse(value, strlen(value), out);
}

/* Metadata fields that can appear in OSC 3008 sequences.
 * Fields are read lazily from the raw string using the `read` method. */
enum class Field : uint8_t {
    /* Start sequence fields */
    type,
    user,
    hostname,
    machineid,
    bootid,
    pid,
    pidfdid,
    comm,
    cwd,
    cmdline,
    vm,
    container,
    targetuser,
    targethost,
    sessionid,

    /* End sequence fields */
    exit,
    status,
    signal,
};

inline const char *Field_key(Field f) {
    static const char *const names[] = {
        "type", "user", "hostname", "machineid", "bootid", "pid", "pidfdid",
        "comm", "cwd", "cmdline", "vm", "container", "targetuser",
        "targethost", "sessionid", "exit", "status", "signal",
    };
    return names[(size_t)f];
}

/* Wisp: the shared part of Field.read — the value of the first
 * semicolon-separated key=value pair whose key matches. Upstream converts
 * that first match and returns, whether or not it converts. */
inline bool Field_find(Field self, const char *raw, size_t raw_len,
                       const char **value, size_t *value_len) {
    const char *key = Field_key(self);
    const size_t key_len = strlen(key);
    size_t start = 0;
    bool more = true;
    while (more) {
        const char *semi = (const char *)memchr(raw + start, ';', raw_len - start);
        const size_t end = semi ? (size_t)(semi - raw) : raw_len;
        const char *full = raw + start;
        const size_t full_len = end - start;
        if (semi) start = end + 1; else more = false;

        /* Parse key=value */
        const char *eq = (const char *)memchr(full, '=', full_len);
        if (!eq) continue;
        const size_t eql_idx = (size_t)(eq - full);
        if (eql_idx == key_len && memcmp(full, key, key_len) == 0) {
            *value = full + eql_idx + 1;
            *value_len = full_len - eql_idx - 1;
            return true;
        }
    }
    /* Not found */
    return false;
}

/* A single OSC 3008 context signal command. */
struct Command {
    enum class Action : uint8_t {
        /* OSC 3008;start=<id> — initiates, updates, or returns to a context. */
        start,
        /* OSC 3008;end=<id> — terminates a context. */
        end,
    };

    Action action;
    /* The context identifier. Must be 1-64 characters in the 32..126 byte range. */
    ZStr id;
    /* Raw unparsed metadata fields after the context ID.
     * Fields are semicolon-separated key=value pairs.
     * Parsed lazily via `readOption`. */
    ZStr metadata;

    Command() : action(Action::start), id(), metadata() {}

    /* Read a metadata field value from the raw fields string.
     * Returns null if the field is not present or malformed.
     *
     * Wisp: option.Type() depends on the field, so there is one reader per
     * type: .type, .exit, the u64 fields (.pid, .pidfdid, .status) and the
     * string fields. Each returns false for null. */
    bool readType(ContextType *out) const {
        const char *v; size_t n;
        if (!Field_find(Field::type, metadata.ptr, metadata.len, &v, &n)) return false;
        return ContextType_parse(v, n, out);
    }
    bool readExit(ExitStatus *out) const {
        const char *v; size_t n;
        if (!Field_find(Field::exit, metadata.ptr, metadata.len, &v, &n)) return false;
        return ExitStatus_parse(v, n, out);
    }
    bool readU64(Field option, uint64_t *out) const {
        /* assert(option is .pid, .pidfdid or .status) */
        const char *v; size_t n;
        if (!Field_find(option, metadata.ptr, metadata.len, &v, &n)) return false;
        for (size_t i = 0; i < n; i++) {
            if (v[i] < '0' || v[i] > '9') return false;
        }
        /* std.fmt.parseInt(u64, value, 10) catch null */
        if (n == 0) return false;
        uint64_t r = 0;
        for (size_t i = 0; i < n; i++) {
            const uint64_t d = (uint64_t)(v[i] - '0');
            if (r > (UINT64_MAX - d) / 10) return false;
            r = r * 10 + d;
        }
        *out = r;
        return true;
    }
    bool readString(Field option, ZStr *out) const {
        /* assert(option is a string field) */
        const char *v; size_t n;
        if (!Field_find(option, metadata.ptr, metadata.len, &v, &n)) return false;
        if (n == 0) return false;
        *out = ZStr(v, n);
        return true;
    }
};

} /* namespace context_signal */

/* ─── osc/kitty_metadata.zig ────────────────────────────────────────────── */

/* Helpers for parsing metadata shared by Kitty OSC protocols.
 *
 * Kitty OSC 99 and OSC 5522 encode metadata as colon-separated `key=value`
 * fields. The iterator in this module lazily searches that metadata for one
 * key, preserving the order of repeated values without allocating.
 *
 * Parsing is intentionally tolerant. Whitespace around keys and values is
 * trimmed, while malformed fields, non-matching keys, and invalid values are
 * skipped. Returned values are slices of the original metadata and remain
 * valid only as long as that input remains valid. */
namespace kitty_metadata {

/* Wisp: std.ascii.whitespace */
inline bool is_ascii_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0b || c == 0x0c;
}

/* Return an iterator over values whose key exactly matches `key`.
 *
 * If `valid_value_characters` is non-null, every byte in a returned value must
 * appear in that character set. Passing null disables value validation.
 *
 * Wisp: upstream makes key and validator comptime parameters of the type;
 * here they are runtime fields. */
struct ValueIterator {
    const char *key;
    const char *valid_value_characters;   /* nullptr: no validation */
    const char *metadata;
    size_t      metadata_len;
    size_t      pos;

    /* Initialize an iterator borrowing `metadata`. */
    ValueIterator(const char *key_, const char *valid, const char *md, size_t md_len)
        : key(key_), valid_value_characters(valid), metadata(md),
          metadata_len(md_len), pos(0) {}

    /* Return the next valid matching value, or null when none remain.
     * The returned slice borrows the metadata passed to `init`. */
    bool next(ZStr *out) {
        const size_t key_len = strlen(key);
        while (pos < metadata_len) {
            const char *colon = (const char *)memchr(metadata + pos, ':', metadata_len - pos);
            const size_t end = colon ? (size_t)(colon - metadata) : metadata_len;
            const char *field = metadata + pos;
            const size_t field_len = end - pos;
            pos = end < metadata_len ? end + 1 : end;

            const char *eq = (const char *)memchr(field, '=', field_len);
            if (!eq) continue;
            const size_t equals = (size_t)(eq - field);

            const char *k = field;
            size_t k_len = equals;
            while (k_len > 0 && is_ascii_whitespace(k[0])) { k++; k_len--; }
            while (k_len > 0 && is_ascii_whitespace(k[k_len - 1])) k_len--;
            if (!(k_len == key_len && memcmp(k, key, key_len) == 0)) continue;

            const char *v = field + equals + 1;
            size_t v_len = field_len - equals - 1;
            while (v_len > 0 && is_ascii_whitespace(v[0])) { v++; v_len--; }
            while (v_len > 0 && is_ascii_whitespace(v[v_len - 1])) v_len--;
            if (valid_value_characters) {
                bool ok = true;
                for (size_t i = 0; i < v_len; i++) {
                    if (!strchr(valid_value_characters, v[i]) || v[i] == 0) { ok = false; break; }
                }
                if (!ok) continue;
            }

            *out = ZStr(v, v_len);
            return true;
        }

        return false;
    }
};

} /* namespace kitty_metadata */

/* ─── osc/parsers/kitty_clipboard_protocol.zig: types ──────────────────── */

/* Kitty's clipboard protocol (OSC 5522)
 * Specification: https://sw.kovidgoyal.net/kitty/clipboard/
 * https://rockorager.dev/misc/bracketed-paste-mime/ */
namespace kitty_clipboard_protocol {

/* Wisp: std.meta.stringToEnum over a name table. */
inline bool name_to_index(const char *const *names, size_t count,
                          const char *s, size_t len, size_t *out) {
    for (size_t i = 0; i < count; i++) {
        if (strlen(names[i]) == len && memcmp(names[i], s, len) == 0) {
            *out = i;
            return true;
        }
    }
    return false;
}

enum class Location : uint8_t {
    primary,
};

inline bool Location_init(const char *str, size_t len, Location *out) {
    static const char *const names[] = { "primary" };
    size_t i;
    if (!name_to_index(names, 1, str, len, &i)) return false;
    *out = (Location)i;
    return true;
}

enum class Operation : uint8_t {
    read,
    walias,
    wdata,
    write,
};

inline const char *const *Operation_names() {
    static const char *const names[] = { "read", "walias", "wdata", "write" };
    return names;
}

inline bool Operation_init(const char *str, size_t len, Operation *out) {
    size_t i;
    if (!name_to_index(Operation_names(), 4, str, len, &i)) return false;
    *out = (Operation)i;
    return true;
}

/* Wisp: the `{t}` format of an Operation. */
inline const char *Operation_name(Operation op) { return Operation_names()[(size_t)op]; }

/* Wisp: errno.h defines EBUSY, EFBIG, EINVAL, EIO, ENOSYS and EPERM as
 * macros, so those members carry a trailing underscore. */
enum class Status : uint8_t {
    DATA,
    DONE,
    EBUSY_,
    EFBIG_,
    EINVAL_,
    EIO_,
    ENOSYS_,
    EPERM_,
    OK,
};

inline const char *const *Status_names() {
    static const char *const names[] = {
        "DATA", "DONE", "EBUSY", "EFBIG", "EINVAL", "EIO", "ENOSYS", "EPERM", "OK",
    };
    return names;
}

inline bool Status_init(const char *str, size_t len, Status *out) {
    size_t i;
    if (!name_to_index(Status_names(), 9, str, len, &i)) return false;
    *out = (Status)i;
    return true;
}

/* Wisp: the `{t}` format of a Status. The trailing underscore the errno
 * macros force is not part of the name. */
inline const char *Status_name(Status st) { return Status_names()[(size_t)st]; }

enum class Option : uint8_t {
    id,
    loc,
    mime,
    name,
    password,
    pw,
    status,
    type,
};

inline const char *Option_name(Option o) {
    static const char *const names[] = {
        "id", "loc", "mime", "name", "password", "pw", "status", "type",
    };
    return names[(size_t)o];
}

/* Characters that are valid in identifiers. */
static const char valid_identifier_characters[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_+.";

inline bool isValidIdentifier(const char *str, size_t len) {
    if (len == 0) return false;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == 0 || !strchr(valid_identifier_characters, str[i])) return false;
    }
    return true;
}

struct OSC {
    /* The raw metadata that was received. It can be parsed by using the
     * `readOption` method. */
    ZStr metadata;
    /* The raw payload. It may be Base64 encoded, check the `e` option.
     * Wisp: ?[]const u8 is has_payload plus payload. */
    bool has_payload;
    ZStr payload;
    /* The terminator that was used in case we need to send a response. */
    Terminator terminator;

    OSC() : metadata(), has_payload(false), payload(), terminator(Terminator::st) {}

    /* Decode an option from the metadata.
     *
     * Wisp: key.Type() depends on the option, so there is one reader per
     * type: the string options (.id, .mime, .name, .password, .pw), .loc,
     * .status and .type. Each returns false for null. Option.read takes only
     * the first matching value and returns null if it does not parse. */
    bool readString(Option key, ZStr *out) const {
        /* assert(key is .id, .mime, .name, .password or .pw) */
        kitty_metadata::ValueIterator it(Option_name(key), nullptr, metadata.ptr, metadata.len);
        ZStr value;
        if (!it.next(&value)) return false;
        if (key == Option::id) {
            /* parseIdentifier */
            if (!isValidIdentifier(value.ptr, value.len)) return false;
        }
        *out = value;
        return true;
    }
    bool readLoc(Location *out) const {
        kitty_metadata::ValueIterator it("loc", nullptr, metadata.ptr, metadata.len);
        ZStr value;
        if (!it.next(&value)) return false;
        return Location_init(value.ptr, value.len, out);
    }
    bool readStatus(Status *out) const {
        kitty_metadata::ValueIterator it("status", nullptr, metadata.ptr, metadata.len);
        ZStr value;
        if (!it.next(&value)) return false;
        return Status_init(value.ptr, value.len, out);
    }
    bool readType(Operation *out) const {
        kitty_metadata::ValueIterator it("type", nullptr, metadata.ptr, metadata.len);
        ZStr value;
        if (!it.next(&value)) return false;
        return Operation_init(value.ptr, value.len, out);
    }
};

} /* namespace kitty_clipboard_protocol */

/* ─── osc/parsers/semantic_prompt.zig: types ───────────────────────────── */

/* https://gitlab.freedesktop.org/Per_Bothner/specifications/blob/master/proposals/semantic-prompts.md */
namespace semantic_prompt {

/* ClickEvents can either be a click_events=1 or click_events=2.
 * The click_events=1 sends a click event with the absolute coordinates
 * of the click.
 * The click_events=2 sends a click event with the coordinates of the click
 * relative to the prompt area.
 * See https://github.com/ghostty-org/ghostty/issues/10865 and
 * https://github.com/kovidgoyal/kitty/issues/9500
 * for further details. */
enum class ClickEvents : uint8_t { absolute, relative };

enum class Option : uint8_t {
    aid,
    cl,
    prompt_kind,
    err,
    cmdline,
    cmdline_url,

    /* https://sw.kovidgoyal.net/kitty/shell-integration/#notes-for-shell-developers
     * Kitty supports a "redraw" option for prompt_start. This is extended
     * by Ghostty with the "last" option. See Redraw the type for more details. */
    redraw,

    /* Use a special key instead of arrow keys to move the cursor on
     * mouse click. Useful if arrow keys have side-effets like triggering
     * auto-complete. The shell integration script should bind the special
     * key as needed.
     * See: https://sw.kovidgoyal.net/kitty/shell-integration/#notes-for-shell-developers */
    special_key,

    /* If true, the shell is capable of handling mouse click events.
     * Ghostty will then send a click event to the shell when the user
     * clicks somewhere in the prompt. The shell can then move the cursor
     * to that position or perform some other appropriate action. If false,
     * Ghostty may generate a number of fake key events to move the cursor
     * which is not very robust.
     * See: https://sw.kovidgoyal.net/kitty/shell-integration/#notes-for-shell-developers */
    click_events,

    /* Not technically an option that can be set with k=v and only
     * present currently with command 'D' but its easier to just
     * parse it into our options. */
    exit_code,
};

/* The `cl` option specifies what kind of cursor key sequences are handled
 * by the application for click-to-move-cursor functionality.
 *
 * `line` allows movement within one input line. `multiple` allows movement
 * across lines with left/right sequences. The two vertical modes additionally
 * allow up/down sequences, with `smart_vertical` permitting editor-aware
 * column clamping. */
enum class Click : uint8_t {
    /* Value: "line". Allows motion within a single input line using
     * standard left/right arrow escape sequences. Only a single left/right
     * sequence should be emitted for double-width characters. */
    line,

    /* Value: "m". Allows movement between different lines in the same
     * group, but only using left/right arrow escape sequences. */
    multiple,

    /* Value: "v". Like `multiple` but cursor up/down should be used. The
     * terminal should be conservative when moving between lines: move the
     * cursor left to the start of line, emit the needed up/down sequences,
     * then move the cursor right to the clicked destination. */
    conservative_vertical,

    /* Value: "w". Like `conservative_vertical` but specifies that there
     * are no spurious spaces at the end of the line, and the application
     * editor handles "smart vertical movement" (moving 2 lines up from
     * position 20, where the intermediate line is 15 chars wide and the
     * destination is 18 chars wide, ends at position 18). */
    smart_vertical,
};

inline bool parseClick(const char *value, size_t len, Click *out) {
    if (len == 1) {
        switch (value[0]) {
            case 'm': *out = Click::multiple; return true;
            case 'v': *out = Click::conservative_vertical; return true;
            case 'w': *out = Click::smart_vertical; return true;
            default: return false;
        }
    }
    if (len == 4 && memcmp(value, "line", 4) == 0) {
        *out = Click::line;
        return true;
    }
    return false;
}

enum class PromptKind : uint8_t {
    initial,
    right,
    continuation,
    secondary,
};

inline bool PromptKind_init(uint8_t c, PromptKind *out) {
    switch (c) {
        case 'i': *out = PromptKind::initial; return true;
        case 'r': *out = PromptKind::right; return true;
        case 'c': *out = PromptKind::continuation; return true;
        case 's': *out = PromptKind::secondary; return true;
        default: return false;
    }
}

/* The values for the `redraw` extension to OSC133. This was
 * started by Kitty[1] and extended by Ghostty (the "last" option).
 *
 * [1]: https://sw.kovidgoyal.net/kitty/shell-integration/#notes-for-shell-developers
 *
 * Wisp: `true` and `false` are C++ keywords, so those members are
 * true_ and false_. */
enum class Redraw : uint8_t {
    /* The shell supports redrawing the full prompt and all continuations.
     * This is the default value, it does not need to be explicitly set
     * unless it is to reset a prior other value. */
    true_,

    /* The shell does NOT support redrawing. In this case, Ghostty will NOT
     * clear any prompt lines on resize. */
    false_,

    /* The shell supports redrawing only the LAST line of the prompt.
     * Ghostty will only clear the last line of the prompt on resize.
     *
     * This is specifically introduced because Bash only redraws the last
     * line. It is literally the only shell that does this and it does this
     * because its bad and they should feel bad. Don't be like Bash. */
    last,
};

inline const char *Option_key(Option self) {
    switch (self) {
        case Option::aid: return "aid";
        case Option::cl: return "cl";
        case Option::prompt_kind: return "k";
        case Option::err: return "err";
        case Option::redraw: return "redraw";
        case Option::special_key: return "special_key";
        case Option::click_events: return "click_events";
        case Option::cmdline: return "cmdline";
        case Option::cmdline_url: return "cmdline_url";

        /* special case, handled before ever calling key */
        case Option::exit_code: break;
    }
    return "";
}

/* Wisp: the shared part of Option.read — find the value of the first
 * key=value whose key matches. Upstream converts that first match and
 * returns, whether or not it converts. Not for .exit_code. */
inline bool Option_find(Option self, const char *raw, size_t raw_len,
                        const char **value, size_t *value_len) {
    const char *key = Option_key(self);
    const size_t key_len = strlen(key);
    const char *remaining = raw;
    size_t remaining_len = raw_len;
    while (remaining_len > 0) {
        /* Length of the next value is up to the `;` or the
         * end of the string. */
        const char *semi = (const char *)memchr(remaining, ';', remaining_len);
        const size_t len = semi ? (size_t)(semi - remaining) : remaining_len;

        /* Grab our full value and move our cursor past the `;` */
        const char *full = remaining;

        /* Parse our key=value and verify our key matches our
         * expectation. */
        const char *eq = (const char *)memchr(full, '=', len);
        if (eq) {
            const size_t eql_idx = (size_t)(eq - full);
            if (eql_idx == key_len && memcmp(full, key, key_len) == 0) {
                *value = full + eql_idx + 1;
                *value_len = len - eql_idx - 1;
                return true;
            }
        }

        /* No match! */
        if (len < remaining_len) {
            remaining += len + 1;
            remaining_len -= len + 1;
            continue;
        }

        break;
    }

    /* Not found */
    return false;
}

/* Read the option value from the raw options string.
 *
 * The raw options string is the raw unparsed data after the
 * OSC 133 command. e.g. for `133;A;aid=14;cl=line`, the
 * raw options string would be `aid=14;cl=line`.
 *
 * Any errors in the raw string will return null since the OSC133
 * specification says to ignore unknown or malformed options.
 *
 * Wisp: self.Type() depends on the option, so Option.read is one overload
 * per result type, each false for null: ZStr for .aid, .err, .cmdline and
 * .cmdline_url; Click for .cl; PromptKind for .prompt_kind; Redraw for
 * .redraw; bool for .special_key; ClickEvents for .click_events; int32_t for
 * .exit_code. */
inline bool Option_read(Option self, const char *raw, size_t raw_len, ZStr *out) {
    /* assert(self is .aid, .err, .cmdline or .cmdline_url) */
    const char *v; size_t n;
    if (!Option_find(self, raw, raw_len, &v, &n)) return false;
    *out = ZStr(v, n);
    return true;
}
inline bool Option_read(Option self, const char *raw, size_t raw_len, Click *out) {
    /* assert(self == .cl) */
    const char *v; size_t n;
    if (!Option_find(self, raw, raw_len, &v, &n)) return false;
    return parseClick(v, n, out);
}
inline bool Option_read(Option self, const char *raw, size_t raw_len, PromptKind *out) {
    /* assert(self == .prompt_kind) */
    const char *v; size_t n;
    if (!Option_find(self, raw, raw_len, &v, &n)) return false;
    if (n != 1) return false;
    return PromptKind_init((uint8_t)v[0], out);
}
inline bool Option_read(Option self, const char *raw, size_t raw_len, Redraw *out) {
    /* assert(self == .redraw) */
    const char *v; size_t n;
    if (!Option_find(self, raw, raw_len, &v, &n)) return false;
    if (n == 1 && v[0] == '0') { *out = Redraw::false_; return true; }
    if (n == 1 && v[0] == '1') { *out = Redraw::true_; return true; }
    if (n == 4 && memcmp(v, "last", 4) == 0) { *out = Redraw::last; return true; }
    return false;
}
inline bool Option_read(Option self, const char *raw, size_t raw_len, bool *out) {
    /* assert(self == .special_key) */
    const char *v; size_t n;
    if (!Option_find(self, raw, raw_len, &v, &n)) return false;
    if (n != 1) return false;
    switch (v[0]) {
        case '0': *out = false; return true;
        case '1': *out = true; return true;
        default: return false;
    }
}
inline bool Option_read(Option self, const char *raw, size_t raw_len, ClickEvents *out) {
    /* assert(self == .click_events) */
    const char *v; size_t n;
    if (!Option_find(self, raw, raw_len, &v, &n)) return false;
    if (n != 1) return false;
    switch (v[0]) {
        case '1': *out = ClickEvents::absolute; return true;
        case '2': *out = ClickEvents::relative; return true;
        default: return false;
    }
}
inline bool Option_read(Option self, const char *raw, size_t raw_len, int32_t *out) {
    /* assert(self == .exit_code) */
    (void)self;
    if (raw_len == 0) return false;
    /* If we're looking for exit_code we special case it.
     * as the first value. */
    const char *semi = (const char *)memchr(raw, ';', raw_len);
    const size_t len = semi ? (size_t)(semi - raw) : raw_len;

    /* std.fmt.parseInt(i32, full, 10) catch null */
    size_t i = 0;
    bool neg = false;
    if (i < len && (raw[i] == '+' || raw[i] == '-')) {
        neg = raw[i] == '-';
        i++;
    }
    if (i >= len) return false;
    int64_t v = 0;
    for (; i < len; i++) {
        if (raw[i] < '0' || raw[i] > '9') return false;
        v = v * 10 + (raw[i] - '0');
        if (v > (int64_t)INT32_MAX + 1) return false;
    }
    if (neg) v = -v;
    if (v > INT32_MAX || v < INT32_MIN) return false;
    *out = (int32_t)v;
    return true;
}
inline bool Option_read(Option self, const char *raw, ZStr *out) { return Option_read(self, raw, strlen(raw), out); }
inline bool Option_read(Option self, const char *raw, Click *out) { return Option_read(self, raw, strlen(raw), out); }
inline bool Option_read(Option self, const char *raw, PromptKind *out) { return Option_read(self, raw, strlen(raw), out); }
inline bool Option_read(Option self, const char *raw, Redraw *out) { return Option_read(self, raw, strlen(raw), out); }
inline bool Option_read(Option self, const char *raw, bool *out) { return Option_read(self, raw, strlen(raw), out); }
inline bool Option_read(Option self, const char *raw, ClickEvents *out) { return Option_read(self, raw, strlen(raw), out); }
inline bool Option_read(Option self, const char *raw, int32_t *out) { return Option_read(self, raw, strlen(raw), out); }

/* A single semantic prompt command.
 *
 * Technically according to the spec, not all commands have options
 * but it is easier to be "liberal in what we accept" here since
 * all except one do and the spec does also say to ignore unknown
 * options. So, I think this is a fair interpretation. */
struct Command {
    enum class Action : uint8_t {
        fresh_line,                           /* 'L' */
        fresh_line_new_prompt,                /* 'A' */
        new_command,                          /* 'N' */
        prompt_start,                         /* 'P' */
        end_prompt_start_input,               /* 'B' */
        end_prompt_start_input_terminate_eol, /* 'I' */
        end_input_start_output,               /* 'C' */
        end_command,                          /* 'D' */
    };

    Action action;
    ZStr options_unvalidated;

    Command() : action(Action::fresh_line), options_unvalidated() {}

    static Command init(Action action) {
        Command c;
        c.action = action;
        c.options_unvalidated = ZStr();
        return c;
    }

    /* Read an option for this command. Returns null if unset or invalid.
     * Wisp: overloaded on the result type, see Option_read. */
    template <typename T>
    bool readOption(Option option, T *out) const {
        return Option_read(option, options_unvalidated.ptr, options_unvalidated.len, out);
    }

    /* Write the decoded command line (if any) to the writer. If an error
     * occurs garbage may have been written to the writer.
     *
     * Wisp: false is error.DecodeError. */
    bool writeCommandLine(std::string *writer) const {
        ZStr command_line;
        if (readOption(Option::cmdline, &command_line)) {
            return ::wisp::os::string_encoding::printfQDecode(
                writer, command_line.ptr, command_line.len);
        }
        if (readOption(Option::cmdline_url, &command_line)) {
            return ::wisp::os::string_encoding::urlPercentDecode(
                writer, command_line.ptr, command_line.len);
        }
        return true;
    }
};

} /* namespace semantic_prompt */

/* ─── osc.zig: Command.ProgressReport ──────────────────────────────────── */

struct ProgressReport {
    enum class State : uint8_t {
        remove,
        set,
        error,
        indeterminate,
        pause,
    };

    State state;
    /* Wisp: ?u8 = null is has_progress plus progress. */
    bool has_progress;
    uint8_t progress;

    ProgressReport() : state(State::remove), has_progress(false), progress(0) {}

    /* sync with ghostty_action_progress_report_s */
    struct C {
        int state;
        int8_t progress;
    };

    C cval() const {
        C c;
        c.state = (int)state;
        c.progress = has_progress
            ? (int8_t)(progress > 100 ? 100 : progress)
            : (int8_t)-1;
        return c;
    }
};

/* ─── osc/parsers/kitty_desktop_notification.zig: types ────────────────── */

/* Kitty's desktop notification protocol (OSC 99)
 * Specification: https://sw.kovidgoyal.net/kitty/desktop-notifications/ */
namespace kitty_desktop_notification {

static const size_t MAX_PLAIN_PAYLOAD_BYTES = 2048;
static const size_t MAX_ENCODED_PAYLOAD_BYTES = 4096;

/* Wisp: std.meta.stringToEnum over a name table. */
inline bool name_to_index(const char *const *names, size_t count,
                          const char *s, size_t len, size_t *out) {
    for (size_t i = 0; i < count; i++) {
        if (strlen(names[i]) == len && memcmp(names[i], s, len) == 0) {
            *out = i;
            return true;
        }
    }
    return false;
}

/* Wisp: packed struct { focus: bool, report: bool }. */
struct Action {
    bool focus;
    bool report;

    static Action default_() {
        Action a;
        a.focus = true;
        a.report = false;
        return a;
    }

    static Action make(bool focus, bool report) {
        Action a;
        a.focus = focus;
        a.report = report;
        return a;
    }

    bool eql(const Action &o) const { return focus == o.focus && report == o.report; }

    static Action init(const char *str, size_t len);
};

/* This is similar to the packed struct parser used in the configs. The
 * differences are that a literal `true` or `false` value does not turn on/off
 * all the values, and the negation prefix is `-` not `no-`.
 *
 * Wisp: parsePackedStruct(Action, str), with Action's two fields spelled
 * out in place of the inline for over @typeInfo. */
inline Action Action::init(const char *str, size_t len) {
    Action result = default_();

    /* We split each value by "," */
    size_t start = 0;
    bool more = true;
    while (more) {
        const char *comma = (const char *)memchr(str + start, ',', len - start);
        const size_t end = comma ? (size_t)(comma - str) : len;
        const char *raw = str + start;
        size_t raw_len = end - start;
        if (comma) start = end + 1; else more = false;

        /* Determine the field we're looking for and the value. If the
         * field is prefixed with "-" then we set the value to false. */
        while (raw_len > 0 && kitty_metadata::is_ascii_whitespace(raw[0])) { raw++; raw_len--; }
        while (raw_len > 0 && kitty_metadata::is_ascii_whitespace(raw[raw_len - 1])) raw_len--;
        const char *part = raw;
        size_t part_len = raw_len;
        bool value = true;
        if (raw_len >= 1 && raw[0] == '-') {
            part = raw + 1;
            part_len = raw_len - 1;
            value = false;
        }

        if (part_len == 5 && memcmp(part, "focus", 5) == 0) {
            result.focus = value;
            continue;
        }
        if (part_len == 6 && memcmp(part, "report", 6) == 0) {
            result.report = value;
            continue;
        }

        /* No field matched */
        return default_();
    }

    return result;
}

enum class Occasion : uint8_t {
    always,
    invisible,
    unfocused,
};

inline Occasion Occasion_init(const char *str, size_t len) {
    static const char *const names[] = { "always", "invisible", "unfocused" };
    size_t i;
    if (!name_to_index(names, 3, str, len, &i)) return Occasion::always; /* .default */
    return (Occasion)i;
}

enum class Payload : uint8_t {
    alive,
    body,
    buttons,
    close,
    icon,
    query,
    title,
    /* This is a special value to indicate that an unknown payload value was
     * specified and it should be ignored. */
    unknown,
};

inline Payload Payload_init(const char *str, size_t len) {
    if (len == 1 && str[0] == '?') return Payload::query;
    /* The string `query` is not allowed, it should be a single question
     * mark if you want a query. */
    if (len == 5 && memcmp(str, "query", 5) == 0) return Payload::unknown;
    static const char *const names[] = {
        "alive", "body", "buttons", "close", "icon", "query", "title", "unknown",
    };
    size_t i;
    if (!name_to_index(names, 8, str, len, &i)) return Payload::unknown;
    return (Payload)i;
}

enum class Urgency : uint8_t {
    low,
    normal,
    high,
};

inline Urgency Urgency_init(const char *str, size_t len) {
    if (len != 1) return Urgency::normal; /* .default */
    switch (str[0]) {
        case '0': return Urgency::low;
        case '1': return Urgency::normal;
        case '2': return Urgency::high;
        default: return Urgency::normal;
    }
}

enum class Option : uint8_t {
    /* What action(s) should be taken when a notification is clicked. */
    a,
    /* Should a notification be sent to the application when the notification
     * is closed? */
    c,
    /* Are we done with the notification, and it is ready to be sent? */
    d,
    /* Is the payload encoded with Base64? */
    e,
    /* The name of the application that is sending the notification. */
    f,
    /* Identifier for icon data. Only used when the payload is icon data. */
    g,
    /* Identifier for the notification. */
    i,
    /* Icon name. */
    n,
    /* When to honor the notification request. */
    o,
    /* Type of the payload. */
    p,
    /* The sound name to play with the notification. */
    s,
    /* The type of the notification. */
    t,
    /* The urgency of the notification. */
    u,
    /* When to auto-close the notification. */
    w,
};

inline const char *Option_name(Option o) {
    static const char *const names[] = {
        "a", "c", "d", "e", "f", "g", "i", "n", "o", "p", "s", "t", "u", "w",
    };
    return names[(size_t)o];
}

/* Characters that are valid in identifiers. */
static const char valid_identifier_characters[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_+.";

/* Characters that are valid in a metadata value. Including `=` is technically
 * against the spec but is needed since Base64 encoded values (with padding)
 * are valid for some options. Including `?` is technically against the spec
 * but is needed since it is a valid value for the `p` option. */
static const char valid_metadata_value_characters[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_+."
    "/,(){}[]*&^%$#@!`~=?";

inline bool isValidIdentifier(const char *str, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (str[i] == 0 || !strchr(valid_identifier_characters, str[i])) return false;
    }
    return true;
}

/* Parse the protocol's booleans */
inline bool parseBool(const char *str, size_t len, bool *out) {
    if (len != 1) return false;
    switch (str[0]) {
        case '0': *out = false; return true;
        case '1': *out = true; return true;
        default: return false;
    }
}

/* Read the option value from the raw metadata string.
 *
 * Unknown and malformed values are ignored. Optional values return null;
 * all other values return the protocol default.
 *
 * Wisp: key.Type() depends on the option, so Option.read is one function per
 * result type: Option_readAction (.a), Option_readBool (.c .d .e),
 * Option_readOptional (.f .g .i, false for null), Option_readIterator
 * (.n .t), Option_readOccasion (.o), Option_readPayload (.p),
 * Option_readString (.s), Option_readUrgency (.u), Option_readW (.w). */
inline kitty_metadata::ValueIterator Option_readIterator(Option key, const char *md, size_t len) {
    /* assert(key is .n or .t) */
    return kitty_metadata::ValueIterator(Option_name(key), valid_metadata_value_characters, md, len);
}

inline bool Option_first(Option key, const char *md, size_t len, ZStr *value) {
    kitty_metadata::ValueIterator it(Option_name(key), valid_metadata_value_characters, md, len);
    return it.next(value);
}

inline Action Option_readAction(const char *md, size_t len) {
    ZStr v;
    if (!Option_first(Option::a, md, len, &v)) return Action::default_();
    return Action::init(v.ptr, v.len);
}

inline bool Option_readBool(Option key, const char *md, size_t len) {
    /* assert(key is .c, .d or .e); defaults: c false, d true, e false */
    const bool def = key == Option::d;
    ZStr v;
    if (!Option_first(key, md, len, &v)) return def;
    bool b;
    if (!parseBool(v.ptr, v.len, &b)) return def;
    return b;
}

inline bool Option_readOptional(Option key, const char *md, size_t len, ZStr *out) {
    /* assert(key is .f, .g or .i) */
    ZStr v;
    if (!Option_first(key, md, len, &v)) return false;
    if (key == Option::g || key == Option::i) {
        /* parseIdentifier */
        if (!isValidIdentifier(v.ptr, v.len)) return false;
    }
    *out = v;
    return true;
}

inline Occasion Option_readOccasion(const char *md, size_t len) {
    ZStr v;
    if (!Option_first(Option::o, md, len, &v)) return Occasion::always;
    return Occasion_init(v.ptr, v.len);
}

inline Payload Option_readPayload(const char *md, size_t len) {
    ZStr v;
    if (!Option_first(Option::p, md, len, &v)) return Payload::title;
    return Payload_init(v.ptr, v.len);
}

inline ZStr Option_readString(const char *md, size_t len) {
    /* .s */
    ZStr v;
    if (!Option_first(Option::s, md, len, &v)) return ZStr("system", 6);
    return v;
}

inline Urgency Option_readUrgency(const char *md, size_t len) {
    ZStr v;
    if (!Option_first(Option::u, md, len, &v)) return Urgency::normal;
    return Urgency_init(v.ptr, v.len);
}

inline int32_t Option_readW(const char *md, size_t len) {
    ZStr v;
    if (!Option_first(Option::w, md, len, &v)) return -1;
    /* Zig's integer parser allows '_', we don't */
    if (memchr(v.ptr, '_', v.len)) return -1;
    /* std.fmt.parseInt(i32, value, 10) */
    size_t i = 0;
    bool neg = false;
    if (i < v.len && (v.ptr[i] == '+' || v.ptr[i] == '-')) {
        neg = v.ptr[i] == '-';
        i++;
    }
    if (i >= v.len) return -1;
    int64_t r = 0;
    for (; i < v.len; i++) {
        if (v.ptr[i] < '0' || v.ptr[i] > '9') return -1;
        r = r * 10 + (v.ptr[i] - '0');
        if (r > (int64_t)INT32_MAX + 1) return -1;
    }
    if (neg) r = -r;
    if (r > INT32_MAX || r < INT32_MIN) return -1;
    /* negative values less than -1 are not allowed */
    if (r < -1) return -1;
    return (int32_t)r;
}

struct OSC {
    /* The raw metadata that was received. It can be parsed by using the
     * `readOption` method. */
    ZStr metadata;
    /* The raw payload. It may be Base64 encoded, check the `e` option. */
    ZStr payload;
    /* The terminator that was used in case we need to send a response. */
    Terminator terminator;

    OSC() : metadata(), payload(), terminator(Terminator::st) {}

    /* Decode an option from the metadata. Wisp: one reader per result type,
     * see Option_read* above. */
    Action readA() const { return Option_readAction(metadata.ptr, metadata.len); }
    bool readBool(Option key) const { return Option_readBool(key, metadata.ptr, metadata.len); }
    bool readOptional(Option key, ZStr *out) const {
        return Option_readOptional(key, metadata.ptr, metadata.len, out);
    }
    kitty_metadata::ValueIterator readIterator(Option key) const {
        return Option_readIterator(key, metadata.ptr, metadata.len);
    }
    Occasion readO() const { return Option_readOccasion(metadata.ptr, metadata.len); }
    Payload readP() const { return Option_readPayload(metadata.ptr, metadata.len); }
    ZStr readS() const { return Option_readString(metadata.ptr, metadata.len); }
    Urgency readU() const { return Option_readUrgency(metadata.ptr, metadata.len); }
    int32_t readW() const { return Option_readW(metadata.ptr, metadata.len); }
};

} /* namespace kitty_desktop_notification */

struct Command {
    /* NOTE: Order matters, see LibEnum documentation. */
    enum class Key : uint8_t {
        invalid,
        change_window_title,
        change_window_icon,
        semantic_prompt,
        clipboard_contents,
        report_pwd,
        mouse_shape,
        color_operation,
        kitty_color_protocol,
        show_desktop_notification,
        hyperlink_start,
        hyperlink_end,
        conemu_sleep,
        conemu_show_message_box,
        conemu_change_tab_title,
        conemu_progress_report,
        conemu_wait_input,
        conemu_guimacro,
        conemu_run_process,
        conemu_output_environment_variable,
        conemu_xterm_emulation,
        conemu_comment,
        kitty_text_sizing,
        kitty_clipboard_protocol,
        kitty_dnd_protocol,
        context_signal,
        kitty_desktop_notification,
    };

    Key key;

    /* Set the window title of the terminal
     *
     * If title mode 0 is set text is expect to be hex encoded (i.e. utf-8
     * with each code unit further encoded with two hex digits).
     *
     * If title mode 2 is set or the terminal is setup for unconditional
     * utf-8 titles text is interpreted as utf-8. Else text is interpreted
     * as latin1. */
    ZStr change_window_title;

    /* Set the icon of the terminal window. The name of the icon is not
     * well defined, so this is currently ignored by Ghostty at the time
     * of writing this. We just parse it so that we don't get parse errors
     * in the log. */
    ZStr change_window_icon;

    /* OSC 7. Reports the current working directory of the shell. This is
     * a moderately flawed escape sequence but one that many major terminals
     * support so we also support it. To understand the flaws, read through
     * this terminal-wg issue:
     * https://gitlab.freedesktop.org/terminal-wg/specifications/-/issues/20 */
    struct {
        /* The reported pwd value. This is not checked for validity. It
         * should be a file URL but it is up to the caller to utilize this
         * value. */
        ZStr value;
    } report_pwd;

    /* OSC 22. Set the mouse shape. There doesn't seem to be a standard
     * naming scheme for cursors but it looks like terminals such as Foot
     * are moving towards using the W3C CSS cursor names. For OSC parsing,
     * we just parse whatever string is given. */
    struct {
        ZStr value;
    } mouse_shape;

    /* Set or get clipboard contents. If data is "?", then the current
     * clipboard contents are sent to the pty. Otherwise, the contents
     * are set on the clipboard. */
    struct {
        uint8_t    kind;
        ZStr       data;
        Terminator terminator;   /* = .st */
    } clipboard_contents;

    /* OSC color operations to set, reset, or report color settings. Some
     * OSCs allow multiple operations to be specified in a single OSC so we
     * need a list-like datastructure to manage them. We use
     * std.SegmentedList because it minimizes the number of allocations and
     * copies because a large majority of the time there will be only one
     * operation per OSC.
     *
     * Currently, these OSCs are handled by `color_operation`:
     *
     * 4, 5, 10-19, 104, 105, 110-119
     *
     * Wisp: the list is owned by the Command and freed by Parser::reset, as
     * upstream's is. A Command copied out of the parser is a shallow copy, as
     * a Zig struct copy is, and is valid until the parser's next reset. */
    struct {
        color::Operation op;
        color::List      requests;
        Terminator       terminator;   /* = .st */
    } color_operation;

    /* Kitty color protocol, OSC 21
     * https://sw.kovidgoyal.net/kitty/color-stack/#id1
     *
     * Wisp: kitty_color.OSC, declared here (see kitty/color.hpp). */
    struct {
        /* list of requests */
        ::wisp::terminal::kitty::color::RequestList list;

        /* We must reply with the same string terminator (ST) as used in the
         * request. */
        Terminator terminator;   /* = .st */
    } kitty_color_protocol;

    /* OSC 133 and OSC 9;12 semantic prompts */
    semantic_prompt::Command semantic_prompt;

    /* Kitty desktop notifications (OSC 99) */
    kitty_desktop_notification::OSC kitty_desktop_notification;

    /* Kitty clipboard protocol (OSC 5522) */
    kitty_clipboard_protocol::OSC kitty_clipboard_protocol;

    /* OSC 3008: hierarchical context signalling */
    context_signal::Command context_signal;

    /* ConEmu sleep (OSC 9;1) */
    struct {
        uint16_t duration_ms;
    } conemu_sleep;

    /* ConEmu show GUI message box (OSC 9;2) */
    ZStr conemu_show_message_box;

    /* ConEmu change tab title (OSC 9;3)
     * Wisp: union(enum) { reset, value } is a tag plus value. */
    struct {
        enum class Tag : uint8_t { reset, value };
        Tag tag;
        ZStr value;
    } conemu_change_tab_title;

    /* ConEmu progress report (OSC 9;4) */
    ProgressReport conemu_progress_report;

    /* ConEmu wait input (OSC 9;5): no payload */

    /* ConEmu GUI macro (OSC 9;6) */
    ZStr conemu_guimacro;

    /* ConEmu run process (OSC 9;7) */
    ZStr conemu_run_process;

    /* ConEmu output environment variable (OSC 9;8) */
    ZStr conemu_output_environment_variable;

    /* ConEmu XTerm keyboard and output emulation (OSC 9;10)
     * https://conemu.github.io/en/TerminalModes.html
     *
     * Wisp: each ?bool is has_x plus x. */
    struct {
        /* null => do not change
         * false => turn off
         * true => turn on */
        bool has_keyboard;
        bool keyboard;
        /* null => do not change
         * false => turn off
         * true => turn on */
        bool has_output;
        bool output;
    } conemu_xterm_emulation;

    /* ConEmu comment (OSC 9;11) */
    ZStr conemu_comment;

    /* Kitty text sizing protocol (OSC 66) */
    kitty_text_sizing::OSC kitty_text_sizing;

    /* Kitty's drag and drop protocol (OSC 72), osc/parsers/kitty_dnd_protocol.zig
     * OSC. Only the raw metadata and payload are captured. */
    struct {
        /* The raw metadata that was received. */
        ZStr metadata;
        /* The raw payload. Null (has_payload false) when the OSC had no
         * `;` after the metadata; an empty payload is distinct from no
         * payload. */
        bool has_payload;
        ZStr payload;
        /* The terminator used for this OSC, so any response can match it. */
        Terminator terminator;
    } kitty_dnd_protocol;

    /* Show a desktop notification (OSC 9 or OSC 777) */
    struct {
        ZStr title;
        ZStr body;
    } show_desktop_notification;

    /* Start a hyperlink (OSC 8) */
    struct {
        bool has_id;   /* Wisp: id: ?[:0]const u8 = null */
        ZStr id;
        ZStr uri;
    } hyperlink_start;

    Command() : key(Key::invalid), change_window_title(), change_window_icon() {
        report_pwd.value = ZStr();
        mouse_shape.value = ZStr();
        clipboard_contents.kind = 0;
        clipboard_contents.data = ZStr();
        clipboard_contents.terminator = Terminator::st;
        color_operation.op = color::Operation::osc_4;
        color_operation.terminator = Terminator::st;
        kitty_color_protocol.terminator = Terminator::st;
        conemu_sleep.duration_ms = 0;
        conemu_change_tab_title.tag = decltype(conemu_change_tab_title)::Tag::reset;
        conemu_xterm_emulation.has_keyboard = false;
        conemu_xterm_emulation.keyboard = false;
        conemu_xterm_emulation.has_output = false;
        conemu_xterm_emulation.output = false;
        hyperlink_start.has_id = false;
        hyperlink_start.id = ZStr();
        hyperlink_start.uri = ZStr();
    }
};

/* Wisp: std.Io.Writer, restricted to the two backings osc.zig uses. For the
 * allocating backing, `capacity` is what upstream's tests call
 * writer.buffer.len. */
struct Writer {
    bool   allocating;
    char  *buf;
    size_t capacity;
    size_t end;

    Writer() : allocating(false), buf(nullptr), capacity(0), end(0) {}

    size_t buffered_len() const { return end; }

    /* Wisp: writer.writeByte on the underlying writer. A fixed writer fails
     * when full. An allocating one grows as std.Io.Writer.Allocating does —
     * which is not bounded by the capture's max_bytes, and that is why
     * upstream's parsers can fail on their NUL terminator in one backing and
     * not the other. */
    bool writeByte(uint8_t b) {
        if (end >= capacity) {
            if (!allocating) return false;
            const size_t want = capacity ? capacity * 2 : 1;
            char *p = (char *)realloc(buf, want);
            if (!p) return false;
            buf = p;
            capacity = want;
        }
        buf[end++] = (char)b;
        return true;
    }

    bool writeAll(const uint8_t *bytes, size_t n) {
        for (size_t i = 0; i < n; i++) {
            if (!writeByte(bytes[i])) return false;
        }
        return true;
    }
};

struct Parser {
    /* Maximum size of a "normal" OSC. */
    static const size_t MAX_BUF = 2048;

    /* Maximum size of an OSC that requires dynamically allocated storage.
     * OSC input is untrusted, so these captures must have a finite bound. */
    static const size_t MAX_ALLOCATING_BUF = 8 * 1024 * 1024;

    enum class State : uint8_t {
        start,
        invalid,

        /* OSC command prefixes. Not all of these are valid OSCs, but may be
         * needed to "bridge" to a valid OSC (e.g. to support OSC 777 we need
         * to have a state "77" even though there is no OSC 77). */
        s0, s1, s2, s3, s4, s5, s6, s7, s8, s9,
        s30, s300, s3008,
        s10, s11, s12, s13, s14, s15, s16, s17, s18, s19,
        s21, s22,
        s52, s55, s66, s72, s77, s99,
        s104,
        s110, s111, s112, s113, s114, s115, s116, s117, s118, s119,
        s133, s552, s777, s1337, s5522,
    };

    struct Capture {
        enum class Mode : uint8_t { fixed, allocating };

        Writer writer;
        size_t max_bytes;

        static void fixed(Capture *c, char *buf, size_t len) {
            c->writer = Writer();
            c->writer.allocating = false;
            c->writer.buf = buf;
            c->writer.capacity = len;
            c->max_bytes = len;
        }

        static bool allocating(Capture *c, size_t max_bytes) {
            c->writer = Writer();
            c->writer.allocating = true;
            const size_t initial = MAX_BUF < max_bytes ? MAX_BUF : max_bytes;
            c->writer.buf = (char *)malloc(initial ? initial : 1);
            if (!c->writer.buf) return false;
            c->writer.capacity = initial;
            c->max_bytes = max_bytes;
            return true;
        }

        /* Append one byte without permitting the backing allocation to grow
         * beyond max_bytes. Allocating.Writer normally grows super-linearly,
         * so grow it explicitly to keep the allocation itself bounded too. */
        bool writeByte(uint8_t byte) {
            if (writer.buffered_len() >= max_bytes) return false;

            if (writer.allocating && writer.end >= writer.capacity) {
                size_t doubled = writer.capacity * 2;
                if (doubled < writer.capacity) doubled = (size_t)-1;  /* *| */
                const size_t grown = doubled > 1 ? doubled : 1;
                const size_t new_capacity = grown < max_bytes ? grown : max_bytes;
                char *p = (char *)realloc(writer.buf, new_capacity);
                if (!p) return false;
                writer.buf = p;
                writer.capacity = new_capacity;
            }

            return writer.writeByte(byte);
        }

        /* Append a slice without permitting the backing allocation to
         * grow beyond max_bytes. This matches the byte-at-a-time
         * semantics of writeByte: bytes are retained up to exactly
         * max_bytes and the first byte that doesn't fit fails the
         * write. */
        bool writeSlice(const uint8_t *bytes, size_t len) {
            const size_t avail = max_bytes - writer.buffered_len();
            const size_t n = len < avail ? len : avail;

            if (writer.allocating) {
                const size_t needed = writer.end + n;
                if (needed > writer.capacity) {
                    size_t doubled = writer.capacity * 2;
                    if (doubled < writer.capacity) doubled = (size_t)-1;
                    const size_t grown = doubled > needed ? doubled : needed;
                    const size_t new_capacity =
                        grown < max_bytes ? grown : max_bytes;
                    char *p = (char *)realloc(writer.buf, new_capacity);
                    if (!p) return false;
                    writer.buf = p;
                    writer.capacity = new_capacity;
                }
            }

            if (!writer.writeAll(bytes, n)) return false;
            if (n < len) return false;
            return true;
        }

        void deinit() {
            if (writer.allocating) {
                free(writer.buf);
                writer.buf = nullptr;
                writer.capacity = 0;
            }
        }

        /* Return the captured trailing data. This is the data from the
         * point that trailing data capture was requested. */
        char *trailing() { return writer.buf; }
        size_t trailing_len() const { return writer.end; }
    };

    /* Optional allocator used to accept data longer than MAX_BUF.
     * This only applies to some commands (e.g. OSC 52) that can
     * reasonably exceed MAX_BUF.
     *
     * Wisp: ?Allocator — whether one was provided. */
    bool alloc;

    /* Maximum number of bytes retained by an allocating capture.
     * This is configurable primarily so callers and tests can choose a
     * smaller policy than the default. */
    size_t max_allocating_bytes;

    /* Current state of the parser. */
    State state;

    /* Buffer for temporary storage of OSC data */
    char buffer[MAX_BUF];

    /* Capture state. If this is set then we're actively capturing the
     * bytes coming into the parser.
     *
     * Wisp: ?Capture is has_capture plus capture. */
    bool    has_capture;
    Capture capture;

    /* The command that is the result of parsing. */
    Command command;

    explicit Parser(bool with_alloc = false)
        : alloc(with_alloc), max_allocating_bytes(MAX_ALLOCATING_BUF),
          state(State::start), has_capture(false), capture(), command() {}

    ~Parser() { deinit(); }

    Parser(const Parser &) = delete;
    Parser &operator=(const Parser &) = delete;

    /* This must be called to clean up any allocated memory. */
    void deinit() { reset(); }

    /* Reset the parser state. */
    void reset() {
        /* If we're capturing, then stop it. */
        if (has_capture) capture.deinit();

        /* Handle any cleanup that individual OSCs require. */
        if (command.key == Command::Key::kitty_color_protocol && alloc) {
            command.kitty_color_protocol.list.deinit();
        }
        if (command.key == Command::Key::color_operation && alloc) {
            command.color_operation.requests.deinit();
        }

        state = State::start;
        has_capture = false;
        command = Command();
    }

    /* Make sure that we have an allocator. If we don't, set the state to
     * invalid so that any additional OSC data is discarded. */
    bool ensureAllocator() {
        if (alloc) return true;
        /* log.warn("An allocator is required to process OSC ...") */
        state = State::invalid;
        return false;
    }

    /* Begin capturing trailing data. All inputs to next from this point
     * forward will be captured into the `self.capture.writer` buffer
     * which may be backed by either a fixed size or allocating buffer
     * depending on mode.
     *
     * Get the trailing data using `capture.trailing()`. Do not access
     * the writer directly. */
    void captureTrailing(Capture::Mode mode) {
        switch (mode) {
            case Capture::Mode::fixed:
                Capture::fixed(&capture, buffer, MAX_BUF);
                has_capture = true;
                return;

            case Capture::Mode::allocating:
                if (!alloc) {
                    /* We don't have an allocator - fall back to a fixed
                     * buffer and hope that it's big enough. */
                    captureTrailing(Capture::Mode::fixed);
                    return;
                }

                if (!Capture::allocating(&capture, max_allocating_bytes)) {
                    /* The allocator failed for some reason, fall back to a
                     * fixed buffer and hope that it's big enough. */
                    captureTrailing(Capture::Mode::fixed);
                    return;
                }
                has_capture = true;
                return;
        }
    }

    /* Consume a slice of bytes, advancing the parser state. This is
     * equivalent to calling `next` for each byte in order, but is much
     * faster once a data capture is active because the remaining bytes
     * are appended to the capture in bulk. */
    void nextSlice(const uint8_t *input, size_t len) {
        if (state == State::invalid) return;

        /* Run the state machine byte-at-a-time until a capture begins.
         * The command prefix before a capture starts is only a handful
         * of bytes so this loop is short in practice. */
        size_t offset = 0;
        while (!has_capture) {
            if (offset >= len) return;
            next(input[offset]);
            offset += 1;
            if (state == State::invalid) return;
        }

        const size_t rem = len - offset;
        if (rem == 0) return;
        if (!capture.writeSlice(input + offset, rem)) {
            /* We have overflowed our buffer or had some other error, set
             * the state to invalid so that we discard any further input. */
            state = State::invalid;
        }
    }

    void nextSlice(const char *s) { nextSlice((const uint8_t *)s, strlen(s)); }

    /* Consume the next character c and advance the parser state. */
    void next(uint8_t c);

    /* End the sequence and return the command, if any. If the return value
     * is null, then no valid command was found. The optional terminator_ch
     * is the final character in the OSC sequence. This is used to determine
     * the response terminator.
     *
     * The returned pointer is only valid until the next call to the parser.
     * Callers should copy out any data they wish to retain across calls.
     *
     * Wisp: ?u8 is has_ch plus ch. */
    Command *end(bool has_ch, uint8_t ch);
    Command *end() { return end(false, 0); }
    Command *end(uint8_t ch) { return end(true, ch); }
};

inline void Parser::next(uint8_t c) {
    typedef State S;
    typedef Capture::Mode M;

    /* If the state becomes invalid for any reason, just discard
     * any further input. */
    if (state == S::invalid) return;

    /* If a writer has been initialized, we just accumulate the rest of the
     * OSC sequence in the writer's buffer and skip the state machine. */
    if (has_capture) {
        if (!capture.writeByte(c)) {
            /* We have overflowed our buffer or had some other error, set the
             * state to invalid so that we discard any further input. */
            state = S::invalid;
        }
        return;
    }

    switch (state) {
        /* handled above, so should never be here */
        case S::invalid:
            return;

        case S::start:
            switch (c) {
                case '0': state = S::s0; break;
                case '1': state = S::s1; break;
                case '2': state = S::s2; break;
                case '3': state = S::s3; break;
                case '4': state = S::s4; break;
                case '5': state = S::s5; break;
                case '6': state = S::s6; break;
                case '7': state = S::s7; break;
                case '8': state = S::s8; break;
                case '9': state = S::s9; break;
                default: state = S::invalid; break;
            }
            return;

        case S::s3:
            if (c == '0') state = S::s30; else state = S::invalid;
            return;

        case S::s30:
            if (c == '0') state = S::s300; else state = S::invalid;
            return;

        case S::s300:
            if (c == '8') state = S::s3008; else state = S::invalid;
            return;

        case S::s3008:
            if (c == ';') captureTrailing(M::fixed); else state = S::invalid;
            return;

        case S::s1:
            switch (c) {
                case ';': captureTrailing(M::fixed); break;
                case '0': state = S::s10; break;
                case '1': state = S::s11; break;
                case '2': state = S::s12; break;
                case '3': state = S::s13; break;
                case '4': state = S::s14; break;
                case '5': state = S::s15; break;
                case '6': state = S::s16; break;
                case '7': state = S::s17; break;
                case '8': state = S::s18; break;
                case '9': state = S::s19; break;
                default: state = S::invalid; break;
            }
            return;

        case S::s10:
            switch (c) {
                case ';': if (ensureAllocator()) captureTrailing(M::fixed); break;
                case '4': state = S::s104; break;
                default: state = S::invalid; break;
            }
            return;

        case S::s104:
            if (c == ';') {
                if (ensureAllocator()) captureTrailing(M::fixed);
            } else {
                state = S::invalid;
            }
            return;

        case S::s11:
            switch (c) {
                case ';': if (ensureAllocator()) captureTrailing(M::fixed); break;
                case '0': state = S::s110; break;
                case '1': state = S::s111; break;
                case '2': state = S::s112; break;
                case '3': state = S::s113; break;
                case '4': state = S::s114; break;
                case '5': state = S::s115; break;
                case '6': state = S::s116; break;
                case '7': state = S::s117; break;
                case '8': state = S::s118; break;
                case '9': state = S::s119; break;
                default: state = S::invalid; break;
            }
            return;

        case S::s4:
        case S::s12:
        case S::s14:
        case S::s15:
        case S::s16:
        case S::s17:
        case S::s18:
        case S::s19:
        case S::s21:
        case S::s110:
        case S::s111:
        case S::s112:
        case S::s113:
        case S::s114:
        case S::s115:
        case S::s116:
        case S::s117:
        case S::s118:
        case S::s119:
            if (c == ';') {
                if (ensureAllocator()) captureTrailing(M::fixed);
            } else {
                state = S::invalid;
            }
            return;

        case S::s13:
            switch (c) {
                case ';': if (ensureAllocator()) captureTrailing(M::fixed); break;
                case '3': state = S::s133; break;
                default: state = S::invalid; break;
            }
            return;

        case S::s2:
            switch (c) {
                case ';': captureTrailing(M::fixed); break;
                case '1': state = S::s21; break;
                case '2': state = S::s22; break;
                default: state = S::invalid; break;
            }
            return;

        case S::s5:
            switch (c) {
                case ';': if (ensureAllocator()) captureTrailing(M::fixed); break;
                case '2': state = S::s52; break;
                case '5': state = S::s55; break;
                default: state = S::invalid; break;
            }
            return;

        case S::s6:
            if (c == '6') state = S::s66; else state = S::invalid;
            return;

        case S::s52:
        case S::s66:
            if (c == ';') captureTrailing(M::allocating); else state = S::invalid;
            return;

        case S::s55:
            if (c == '2') state = S::s552; else state = S::invalid;
            return;

        case S::s7:
            switch (c) {
                case ';': captureTrailing(M::fixed); break;
                case '2': state = S::s72; break;
                case '7': state = S::s77; break;
                default: state = S::invalid; break;
            }
            return;

        case S::s72:
            if (c == ';') captureTrailing(M::allocating); else state = S::invalid;
            return;

        case S::s77:
            if (c == '7') state = S::s777; else state = S::invalid;
            return;

        case S::s133:
            switch (c) {
                case ';': captureTrailing(M::fixed); break;
                case '7': state = S::s1337; break;
                default: state = S::invalid; break;
            }
            return;

        case S::s552:
            if (c == '2') state = S::s5522; else state = S::invalid;
            return;

        case S::s1337:
            if (c == ';') captureTrailing(M::fixed); else state = S::invalid;
            return;

        case S::s5522:
            if (c == ';') captureTrailing(M::allocating); else state = S::invalid;
            return;

        case S::s9:
            switch (c) {
                case ';': captureTrailing(M::fixed); break;
                case '9': state = S::s99; break;
                default: state = S::invalid; break;
            }
            return;

        case S::s99:
            /* OSC 99 encoded payloads can exceed the fixed buffer. */
            if (c == ';') captureTrailing(M::allocating); else state = S::invalid;
            return;

        case S::s0:
        case S::s22:
        case S::s777:
        case S::s8:
            if (c == ';') captureTrailing(M::fixed); else state = S::invalid;
            return;
    }
}

/* ─── the osc parsers ──────────────────────────────────────────────────── */

namespace parsers {

/* The four simple parsers share this shape upstream, each written out in
 * its own file. Wisp: kept as one helper plus four callers, since the files
 * differ only in which field receives the string. */
inline bool take_nul_terminated(Parser *parser, ZStr *out) {
    if (!parser->has_capture) {
        parser->state = Parser::State::invalid;
        return false;
    }
    Parser::Capture *cap = &parser->capture;
    if (!cap->writer.writeByte(0)) {
        parser->state = Parser::State::invalid;
        return false;
    }
    char *data = cap->trailing();
    const size_t len = cap->trailing_len();
    *out = ZStr(data, len - 1);
    return true;
}

namespace change_window_title {
/* Parse OSC 0 and OSC 2 */
inline Command *parse(Parser *parser, bool, uint8_t) {
    ZStr s;
    if (!take_nul_terminated(parser, &s)) return nullptr;
    parser->command = Command();
    parser->command.key = Command::Key::change_window_title;
    parser->command.change_window_title = s;
    return &parser->command;
}
} /* namespace change_window_title */

namespace change_window_icon {
/* Parse OSC 1 */
inline Command *parse(Parser *parser, bool, uint8_t) {
    ZStr s;
    if (!take_nul_terminated(parser, &s)) return nullptr;
    parser->command = Command();
    parser->command.key = Command::Key::change_window_icon;
    parser->command.change_window_icon = s;
    return &parser->command;
}
} /* namespace change_window_icon */

namespace report_pwd {
/* Parse OSC 7 */
inline Command *parse(Parser *parser, bool, uint8_t) {
    ZStr s;
    if (!take_nul_terminated(parser, &s)) return nullptr;
    parser->command = Command();
    parser->command.key = Command::Key::report_pwd;
    parser->command.report_pwd.value = s;
    return &parser->command;
}
} /* namespace report_pwd */

namespace mouse_shape {
/* Parse OSC 22 */
inline Command *parse(Parser *parser, bool, uint8_t) {
    /* assert(parser.state == .@"22") */
    ZStr s;
    if (!take_nul_terminated(parser, &s)) return nullptr;
    parser->command = Command();
    parser->command.key = Command::Key::mouse_shape;
    parser->command.mouse_shape.value = s;
    return &parser->command;
}
} /* namespace mouse_shape */

namespace hyperlink {
/* Parse OSC 8 hyperlinks */
inline Command *parse(Parser *parser, bool, uint8_t) {
    if (!parser->has_capture) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    Parser::Capture *cap = &parser->capture;
    if (!cap->writer.writeByte(0)) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    char *data = cap->trailing();
    const size_t data_len = cap->trailing_len();

    const char *semi = (const char *)memchr(data, ';', data_len);
    if (!semi) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    const size_t s = (size_t)(semi - data);

    parser->command = Command();
    parser->command.key = Command::Key::hyperlink_start;
    parser->command.hyperlink_start.uri = ZStr(data + s + 1, data_len - 1 - (s + 1));

    data[s] = 0;
    char *kvs = data;
    const size_t kvs_len = s + 1;
    for (size_t i = 0; i < kvs_len; i++) {
        if (kvs[i] == ':') kvs[i] = 0;
    }

    size_t kv_start = 0;
    while (kv_start < kvs_len) {
        /* std.mem.indexOfScalarPos(u8, kvs, kv_start + 1, 0) */
        size_t kv_end = (size_t)-1;
        for (size_t i = kv_start + 1; i < kvs_len; i++) {
            if (kvs[i] == 0) {
                kv_end = i;
                break;
            }
        }
        if (kv_end == (size_t)-1) break;

        char *kv = data + kv_start;
        const size_t kv_len = kv_end + 1 - kv_start;
        const char *eq = (const char *)memchr(kv, '=', kv_len);
        if (!eq) break;
        const size_t v = (size_t)(eq - kv);

        const size_t key_len = v;
        const ZStr value(kv + v + 1, kv_len - 1 - (v + 1));
        if (key_len == 2 && memcmp(kv, "id", 2) == 0) {
            if (value.len > 0) {
                parser->command.hyperlink_start.has_id = true;
                parser->command.hyperlink_start.id = value;
            }
        } else {
            /* log.warn("unknown hyperlink option: '{s}'") */
        }
        kv_start = kv_end + 1;
    }

    if (parser->command.hyperlink_start.uri.len == 0) {
        if (parser->command.hyperlink_start.has_id) {
            parser->state = Parser::State::invalid;
            return nullptr;
        }
        parser->command = Command();
        parser->command.key = Command::Key::hyperlink_end;
    }

    return &parser->command;
}
} /* namespace hyperlink */

namespace clipboard_operation {
/* Parse OSC 52 */
inline Command *parse(Parser *parser, bool has_ch, uint8_t terminator_ch) {
    /* assert(parser.state == .@"52") */
    if (!parser->has_capture) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    Parser::Capture *cap = &parser->capture;

    /* Wisp: unlike the parsers above, this one writes its terminator
     * through the bounded Capture.writeByte, so a capture already at its
     * limit fails here. Upstream tests exactly that. */
    if (!cap->writeByte(0)) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    char *data = cap->trailing();
    const size_t len = cap->trailing_len();
    if (len == 1) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    if (data[0] == ';') {
        parser->command = Command();
        parser->command.key = Command::Key::clipboard_contents;
        parser->command.clipboard_contents.kind = 'c';
        parser->command.clipboard_contents.data = ZStr(data + 1, len - 1 - 1);
        parser->command.clipboard_contents.terminator =
            terminator_init(has_ch, terminator_ch);
    } else {
        if (len < 2) {
            parser->state = Parser::State::invalid;
            return nullptr;
        }
        if (data[1] != ';') {
            parser->state = Parser::State::invalid;
            return nullptr;
        }
        parser->command = Command();
        parser->command.key = Command::Key::clipboard_contents;
        parser->command.clipboard_contents.kind = (uint8_t)data[0];
        parser->command.clipboard_contents.data = ZStr(data + 2, len - 1 - 2);
        parser->command.clipboard_contents.terminator =
            terminator_init(has_ch, terminator_ch);
    }
    return &parser->command;
}
} /* namespace clipboard_operation */

namespace kitty_color {
/* Parse OSC 21, the Kitty Color Protocol. */
inline Command *parse(Parser *parser, bool has_ch, uint8_t terminator_ch) {
    namespace kc = ::wisp::terminal::kitty::color;
    /* assert(parser.state == .@"21") */

    if (!parser->alloc) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    if (!parser->has_capture) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    Parser::Capture *cap = &parser->capture;
    parser->command = Command();
    parser->command.key = Command::Key::kitty_color_protocol;
    parser->command.kitty_color_protocol.terminator = terminator_init(has_ch, terminator_ch);
    kc::RequestList *list = &parser->command.kitty_color_protocol.list;
    const char *data = cap->trailing();
    const size_t data_len = cap->trailing_len();

    /* std.mem.splitScalar(u8, data, ';') */
    size_t kv_start = 0;
    bool more = true;
    while (more) {
        const char *semi = (const char *)memchr(data + kv_start, ';', data_len - kv_start);
        const size_t kv_end = semi ? (size_t)(semi - data) : data_len;
        const char *kv = data + kv_start;
        const size_t kv_len = kv_end - kv_start;
        if (semi) kv_start = kv_end + 1; else more = false;

        if (list->len >= kc::Kind::max * 2) {
            /* log.warn("exceeded limit for number of keys in kitty color
             * protocol, ignoring") */
            parser->state = Parser::State::invalid;
            return nullptr;
        }
        /* std.mem.splitScalar(u8, kv, '=') */
        const char *eq = (const char *)memchr(kv, '=', kv_len);
        const size_t k_len = eq ? (size_t)(eq - kv) : kv_len;
        if (k_len == 0) {
            /* log.warn("zero length key in kitty color protocol") */
            continue;
        }
        kc::Kind key;
        if (!kc::Kind::parse(kv, k_len, &key)) {
            /* log.warn("unknown key in kitty color protocol: {s}") */
            continue;
        }
        /* std.mem.trim(u8, it.rest(), " ") */
        const char *value = eq ? eq + 1 : kv + kv_len;
        size_t value_len = eq ? kv_len - k_len - 1 : 0;
        while (value_len > 0 && value[0] == ' ') { value++; value_len--; }
        while (value_len > 0 && value[value_len - 1] == ' ') value_len--;

        kc::Request req;
        if (value_len == 0) {
            req.tag = kc::Request::Tag::reset;
            req.reset = key;
        } else if (value_len == 1 && value[0] == '?') {
            req.tag = kc::Request::Tag::query;
            req.query = key;
        } else {
            ::wisp::terminal::RGB rgb;
            if (::wisp::terminal::RGB::parse(value, value_len, &rgb) !=
                ::wisp::terminal::ColorError::none) {
                /* log.warn("invalid color format in kitty color protocol") */
                continue;
            }
            req.tag = kc::Request::Tag::set;
            req.set.key = key;
            req.set.color = rgb;
        }
        if (!list->append(req)) {
            /* log.warn("unable to append kitty color protocol option") */
            continue;
        }
    }
    return &parser->command;
}
} /* namespace kitty_color */

namespace kitty_text_sizing {
inline Command *parse(Parser *parser, bool, uint8_t) {
    /* assert(parser.state == .@"66") */

    if (!parser->has_capture) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    Parser::Capture *cap = &parser->capture;

    /* Write a NUL byte to ensure that `text` is NUL-terminated */
    if (!cap->writeByte(0)) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    const char *data = cap->trailing();
    const size_t data_len = cap->trailing_len();

    const char *semi = (const char *)memchr(data, ';', data_len);
    if (!semi) {
        /* log.warn("missing semicolon before payload") */
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    const size_t payload_start = (size_t)(semi - data);
    const ZStr payload(data + payload_start + 1, data_len - 1 - (payload_start + 1));

    /* Payload has to be a URL-safe UTF-8 string,
     * and be under the size limit. */
    if (payload.len > ::wisp::terminal::osc::kitty_text_sizing::max_payload_length) {
        /* log.warn("payload is too long") */
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    if (!isSafeUtf8(payload.ptr, payload.len)) {
        /* log.warn("payload is not escape code safe UTF-8") */
        parser->state = Parser::State::invalid;
        return nullptr;
    }

    parser->command = Command();
    parser->command.key = Command::Key::kitty_text_sizing;
    parser->command.kitty_text_sizing.text = payload;
    ::wisp::terminal::osc::kitty_text_sizing::OSC *cmd = &parser->command.kitty_text_sizing;

    /* Parse any arguments if given */
    if (payload_start > 0) {
        /* std.mem.splitScalar(u8, data[0..payload_start], ':') */
        size_t kv_start = 0;
        bool more = true;
        while (more) {
            const char *colon = (const char *)memchr(data + kv_start, ':', payload_start - kv_start);
            const size_t kv_end = colon ? (size_t)(colon - data) : payload_start;
            const char *kv = data + kv_start;
            const size_t kv_len = kv_end - kv_start;
            if (colon) kv_start = kv_end + 1; else more = false;

            const char *eq = (const char *)memchr(kv, '=', kv_len);
            const size_t k_len = eq ? (size_t)(eq - kv) : kv_len;
            if (k_len != 1) {
                /* log.warn("key must be a single character") */
                continue;
            }

            if (!eq) {
                /* log.warn("missing value") */
                continue;
            }
            /* it.next(): up to the next '=' */
            const char *value = eq + 1;
            const size_t rest = kv_len - k_len - 1;
            const char *eq2 = (const char *)memchr(value, '=', rest);
            const size_t value_len = eq2 ? (size_t)(eq2 - value) : rest;

            if (cmd->update((uint8_t)kv[0], value, value_len) !=
                ::wisp::terminal::osc::kitty_text_sizing::OSC::UpdateError::none) {
                /* log.warn("unknown key" / "invalid value for key") */
                continue;
            }
        }
    }

    return &parser->command;
}
} /* namespace kitty_text_sizing */

namespace context_signal {
/* Parse OSC 3008: hierarchical context signalling.
 *
 * Expected data format (after "3008;" prefix has been consumed by the state machine):
 *   start=<id>[;<field>=<value>]*
 *   end=<id>[;<field>=<value>]* */
inline Command *parse(Parser *parser, bool, uint8_t) {
    typedef ::wisp::terminal::osc::context_signal::Command CS;
    if (!parser->has_capture) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    const char *data = parser->capture.trailing();
    const size_t data_len = parser->capture.trailing_len();
    if (data_len == 0) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }

    /* Determine the action (start= or end=) */
    CS::Action action;
    if (data_len >= 6 && memcmp(data, "start=", 6) == 0) {
        action = CS::Action::start;
    } else if (data_len >= 4 && memcmp(data, "end=", 4) == 0) {
        action = CS::Action::end;
    } else {
        /* log.warn("OSC 3008: expected 'start=' or 'end=' prefix ...") */
        parser->state = Parser::State::invalid;
        return nullptr;
    }

    /* Skip past the "start=" or "end=" prefix */
    const size_t prefix_len = action == CS::Action::start ? 6 : 4;
    const char *rest = data + prefix_len;
    const size_t rest_len = data_len - prefix_len;

    if (rest_len == 0) {
        /* log.warn("OSC 3008: missing context ID") */
        parser->state = Parser::State::invalid;
        return nullptr;
    }

    /* Extract the context ID (up to the first semicolon or end of data) */
    const char *semi = (const char *)memchr(rest, ';', rest_len);
    const size_t id_end = semi ? (size_t)(semi - rest) : rest_len;

    /* Validate context ID length (1-64 chars per spec) */
    if (id_end == 0 || id_end > ::wisp::terminal::osc::context_signal::max_context_id_len) {
        /* log.warn("OSC 3008: context ID length {d} out of range") */
        parser->state = Parser::State::invalid;
        return nullptr;
    }

    /* Validate context ID characters (32..126 byte range per spec) */
    for (size_t i = 0; i < id_end; i++) {
        const uint8_t c = (uint8_t)rest[i];
        if (c < 0x20 || c > 0x7e) {
            /* log.warn("OSC 3008: invalid character in context ID") */
            parser->state = Parser::State::invalid;
            return nullptr;
        }
    }

    /* Extract raw metadata fields (everything after the ID) */
    const ZStr metadata = id_end < rest_len
        ? ZStr(rest + id_end + 1, rest_len - id_end - 1)
        : ZStr();

    parser->command = Command();
    parser->command.key = Command::Key::context_signal;
    parser->command.context_signal.action = action;
    parser->command.context_signal.id = ZStr(rest, id_end);
    parser->command.context_signal.metadata = metadata;

    return &parser->command;
}
} /* namespace context_signal */

namespace iterm2 {

enum class Key : uint8_t {
    AddAnnotation,
    AddHiddenAnnotation,
    Block,
    Button,
    ClearCapturedOutput,
    ClearScrollback,
    Copy,
    CopyToClipboard,
    CurrentDir,
    CursorShape,
    Custom,
    Disinter,
    EndCopy,
    File,
    FileEnd,
    FilePart,
    HighlightCursorLine,
    MultipartFile,
    OpenURL,
    PopKeyLabels,
    PushKeyLabels,
    RemoteHost,
    ReportCellSize,
    ReportVariable,
    RequestAttention,
    RequestUpload,
    SetBackgroundImageFile,
    SetBadgeFormat,
    SetColors,
    SetKeyLabel,
    SetMark,
    SetProfile,
    SetUserVar,
    ShellIntegrationVersion,
    StealFocus,
    UnicodeVersion,
};

/* Instead of using `std.meta.stringToEnum` we set up a StaticStringMap so
 * that we can get ASCII case-insensitive lookups.
 *
 * Wisp: map.get — a linear scan with std.ascii.eqlIgnoreCase. */
inline bool map_get(const char *s, size_t len, Key *out) {
    static const char *const names[] = {
        "AddAnnotation",
        "AddHiddenAnnotation",
        "Block",
        "Button",
        "ClearCapturedOutput",
        "ClearScrollback",
        "Copy",
        "CopyToClipboard",
        "CurrentDir",
        "CursorShape",
        "Custom",
        "Disinter",
        "EndCopy",
        "File",
        "FileEnd",
        "FilePart",
        "HighlightCursorLine",
        "MultipartFile",
        "OpenURL",
        "PopKeyLabels",
        "PushKeyLabels",
        "RemoteHost",
        "ReportCellSize",
        "ReportVariable",
        "RequestAttention",
        "RequestUpload",
        "SetBackgroundImageFile",
        "SetBadgeFormat",
        "SetColors",
        "SetKeyLabel",
        "SetMark",
        "SetProfile",
        "SetUserVar",
        "ShellIntegrationVersion",
        "StealFocus",
        "UnicodeVersion",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strlen(names[i]) != len) continue;
        bool eq = true;
        for (size_t j = 0; j < len; j++) {
            char a = s[j], b = names[i][j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) { eq = false; break; }
        }
        if (eq) {
            *out = (Key)i;
            return true;
        }
    }
    return false;
}

/* Parse OSC 1337
 * https://iterm2.com/documentation-escape-codes.html */
inline Command *parse(Parser *parser, bool, uint8_t) {
    /* assert(parser.state == .@"1337") */

    if (!parser->has_capture) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    Parser::Capture *cap = &parser->capture;
    if (!cap->writer.writeByte(0)) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    char *data = cap->trailing();
    const size_t data_len = cap->trailing_len();

    ZStr key_str;
    bool has_value = false;
    ZStr value_;
    {
        char *eq = (char *)memchr(data, '=', data_len);
        if (!eq) {
            key_str = ZStr(data, data_len - 1);
        } else {
            const size_t index = (size_t)(eq - data);
            data[index] = 0;
            key_str = ZStr(data, index);
            has_value = true;
            value_ = ZStr(data + index + 1, data_len - 1 - (index + 1));
        }
    }

    Key key;
    if (!map_get(key_str.ptr, key_str.len, &key)) {
        parser->command = Command();
        return nullptr;
    }

    switch (key) {
        case Key::Copy: {
            if (!has_value) {
                parser->command = Command();
                return nullptr;
            }
            ZStr value = value_;

            /* Sending a blank entry to clear the clipboard is an OSC 52-ism,
             * make sure that is invalid here. */
            if (value.len == 0) {
                parser->command = Command();
                return nullptr;
            }

            /* base64 value must be prefixed by a colon */
            if (value.ptr[0] != ':') {
                parser->command = Command();
                return nullptr;
            }

            value = ZStr(value.ptr + 1, value.len - 1);

            /* Sending a blank entry to clear the clipboard is an OSC 52-ism,
             * make sure that is invalid here. */
            if (value.len == 0) {
                parser->command = Command();
                return nullptr;
            }

            /* Sending a '?' to query the clipboard is an OSC 52-ism, make sure
             * that is invalid here. */
            if (value.len == 1 && value.ptr[0] == '?') {
                parser->command = Command();
                return nullptr;
            }

            /* It would be better to check for valid base64 data here, but that
             * would mean parsing the base64 data twice in the "normal" case. */

            parser->command = Command();
            parser->command.key = Command::Key::clipboard_contents;
            parser->command.clipboard_contents.kind = 'c';
            parser->command.clipboard_contents.data = value;
            return &parser->command;
        }

        case Key::CurrentDir: {
            if (!has_value) {
                parser->command = Command();
                return nullptr;
            }
            if (value_.len == 0) {
                parser->command = Command();
                return nullptr;
            }
            parser->command = Command();
            parser->command.key = Command::Key::report_pwd;
            parser->command.report_pwd.value = value_;
            return &parser->command;
        }

        default:
            /* log.debug("unimplemented OSC 1337: {t}") */
            parser->command = Command();
            return nullptr;
    }
}
} /* namespace iterm2 */

namespace kitty_clipboard_protocol {
inline Command *parse(Parser *parser, bool has_ch, uint8_t terminator_ch) {
    /* assert(parser.state == .@"5522") */
    if (!parser->has_capture) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    Parser::Capture *cap = &parser->capture;

    const char *data = cap->trailing();
    const size_t data_len = cap->trailing_len();

    ZStr metadata(data, data_len);
    bool has_payload = false;
    ZStr payload;
    const char *semi = (const char *)memchr(data, ';', data_len);
    if (semi) {
        const size_t start = (size_t)(semi - data);
        metadata = ZStr(data, start);
        has_payload = true;
        payload = ZStr(data + start + 1, data_len - (start + 1));
    }

    parser->command = Command();
    parser->command.key = Command::Key::kitty_clipboard_protocol;
    parser->command.kitty_clipboard_protocol.metadata = metadata;
    parser->command.kitty_clipboard_protocol.has_payload = has_payload;
    parser->command.kitty_clipboard_protocol.payload = payload;
    parser->command.kitty_clipboard_protocol.terminator = terminator_init(has_ch, terminator_ch);

    return &parser->command;
}
} /* namespace kitty_clipboard_protocol */

namespace semantic_prompt {
/* Parse OSC 133, semantic prompts */
inline Command *parse(Parser *parser, bool, uint8_t) {
    typedef ::wisp::terminal::osc::semantic_prompt::Command SP;
    typedef SP::Action A;
    if (!parser->has_capture) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    const char *data = parser->capture.trailing();
    const size_t data_len = parser->capture.trailing_len();
    if (data_len == 0) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }

    /* All valid cases terminate within this block. Any fallthroughs
     * are invalid. This makes some of our parse logic a little less
     * repetitive.
     *
     * Wisp: every case but 'L' is the same shape — init the action, then
     * take "<c>" alone or "<c>;options". */
    bool with_options;
    A action;
    switch (data[0]) {
        case 'A': action = A::fresh_line_new_prompt; with_options = true; break;
        case 'B': action = A::end_prompt_start_input; with_options = true; break;
        case 'I': action = A::end_prompt_start_input_terminate_eol; with_options = true; break;
        case 'C': action = A::end_input_start_output; with_options = true; break;
        case 'D': action = A::end_command; with_options = true; break;
        case 'L': action = A::fresh_line; with_options = false; break;
        case 'N': action = A::new_command; with_options = true; break;
        case 'P': action = A::prompt_start; with_options = true; break;
        default: goto invalid;
    }

    if (!with_options) {
        if (data_len > 1) goto invalid;
        parser->command = Command();
        parser->command.key = Command::Key::semantic_prompt;
        parser->command.semantic_prompt = SP::init(action);
        return &parser->command;
    }

    parser->command = Command();
    parser->command.key = Command::Key::semantic_prompt;
    parser->command.semantic_prompt = SP::init(action);
    if (data_len == 1) return &parser->command;
    if (data[1] != ';') goto invalid;
    parser->command.semantic_prompt.options_unvalidated = ZStr(data + 2, data_len - 2);
    return &parser->command;

invalid:
    /* Any fallthroughs are invalid */
    parser->state = Parser::State::invalid;
    return nullptr;
}
} /* namespace semantic_prompt */

namespace osc9 {

/* Wisp: std.fmt.parseUnsigned(T, buf, 10) for T of max_value. '_'
 * separators are not reproduced (see parsers::color::parse_u9). */
inline bool parse_unsigned(const char *buf, size_t len, uint64_t max_value, uint64_t *out) {
    if (len == 0) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] < '0' || buf[i] > '9') return false;
        const uint64_t d = (uint64_t)(buf[i] - '0');
        if (v > (max_value - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

inline Command *xterm_emulation(Parser *parser, bool has_keyboard, bool keyboard, bool output) {
    parser->command = Command();
    parser->command.key = Command::Key::conemu_xterm_emulation;
    parser->command.conemu_xterm_emulation.has_keyboard = has_keyboard;
    parser->command.conemu_xterm_emulation.keyboard = keyboard;
    parser->command.conemu_xterm_emulation.has_output = true;
    parser->command.conemu_xterm_emulation.output = output;
    return &parser->command;
}

/* Wisp: the recurring "write a NUL, re-read trailing, take data[start ..
 * len - 1 :0]" step. False when the write fails, after marking the parser
 * invalid. */
inline bool terminated_tail(Parser *parser, size_t start, ZStr *out) {
    Parser::Capture *cap = &parser->capture;
    if (!cap->writer.writeByte(0)) {
        parser->state = Parser::State::invalid;
        return false;
    }
    const char *data = cap->trailing();
    const size_t data_len = cap->trailing_len();
    *out = ZStr(data + start, data_len - 1 - start);
    return true;
}

/* Parse OSC 9, which could be an iTerm2 notification or a ConEmu extension. */
inline Command *parse(Parser *parser, bool, uint8_t) {
    typedef ProgressReport::State PS;
    if (!parser->has_capture) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    Parser::Capture *cap = &parser->capture;

    /* Check first to see if this is a ConEmu OSC
     * https://conemu.github.io/en/AnsiEscapeCodes.html#ConEmu_specific_OSC */
    {
        const char *data = cap->trailing();
        const size_t data_len = cap->trailing_len();
        if (data_len == 0) goto not_conemu;
        switch (data[0]) {
            /* Check for OSC 9;1 9;10 9;11 9;12 */
            case '1': {
                if (data_len < 2) goto not_conemu;
                switch (data[1]) {
                    /* OSC 9;1 sleep */
                    case ';': {
                        uint64_t num;
                        uint16_t duration_ms = 100;
                        if (parse_unsigned(data + 2, data_len - 2, 0xFFFF, &num)) {
                            duration_ms = (uint16_t)(num < 10000 ? num : 10000);
                        }
                        parser->command = Command();
                        parser->command.key = Command::Key::conemu_sleep;
                        parser->command.conemu_sleep.duration_ms = duration_ms;
                        return &parser->command;
                    }
                    /* OSC 9;10 xterm keyboard and output emulation */
                    case '0': {
                        if (data_len == 2) return xterm_emulation(parser, true, true, true);
                        if (data_len < 4) goto not_conemu;
                        if (data[2] != ';') goto not_conemu;
                        switch (data[3]) {
                            case '0': return xterm_emulation(parser, true, false, false);
                            case '1': return xterm_emulation(parser, true, true, true);
                            case '2': return xterm_emulation(parser, false, false, false);
                            case '3': return xterm_emulation(parser, false, false, true);
                            default: goto not_conemu;
                        }
                    }
                    /* OSC 9;11 comment */
                    case '1': {
                        if (data_len < 3) goto not_conemu;
                        if (data[2] != ';') goto not_conemu;
                        ZStr v;
                        if (!terminated_tail(parser, 3, &v)) return nullptr;
                        parser->command = Command();
                        parser->command.key = Command::Key::conemu_comment;
                        parser->command.conemu_comment = v;
                        return &parser->command;
                    }
                    /* OSC 9;12 mark prompt start */
                    case '2': {
                        parser->command = Command();
                        parser->command.key = Command::Key::semantic_prompt;
                        parser->command.semantic_prompt =
                            ::wisp::terminal::osc::semantic_prompt::Command::init(
                                ::wisp::terminal::osc::semantic_prompt::Command::Action::fresh_line_new_prompt);
                        return &parser->command;
                    }
                    default: goto not_conemu;
                }
            }
            /* OSC 9;2 show message box */
            case '2': {
                if (data_len < 2) goto not_conemu;
                if (data[1] != ';') goto not_conemu;
                ZStr v;
                if (!terminated_tail(parser, 2, &v)) return nullptr;
                parser->command = Command();
                parser->command.key = Command::Key::conemu_show_message_box;
                parser->command.conemu_show_message_box = v;
                return &parser->command;
            }
            /* OSC 9;3 change tab title */
            case '3': {
                if (data_len < 2) goto not_conemu;
                if (data[1] != ';') goto not_conemu;
                if (data_len == 2) {
                    parser->command = Command();
                    parser->command.key = Command::Key::conemu_change_tab_title;
                    parser->command.conemu_change_tab_title.tag =
                        decltype(parser->command.conemu_change_tab_title)::Tag::reset;
                    return &parser->command;
                }
                ZStr v;
                if (!terminated_tail(parser, 2, &v)) return nullptr;
                parser->command = Command();
                parser->command.key = Command::Key::conemu_change_tab_title;
                parser->command.conemu_change_tab_title.tag =
                    decltype(parser->command.conemu_change_tab_title)::Tag::value;
                parser->command.conemu_change_tab_title.value = v;
                return &parser->command;
            }
            /* OSC 9;4 progress report */
            case '4': {
                if (data_len < 2) goto not_conemu;
                if (data[1] != ';') goto not_conemu;
                if (data_len < 3) goto not_conemu;
                PS state;
                switch (data[2]) {
                    case '0': state = PS::remove; break;
                    case '1': state = PS::set; break;
                    case '2': state = PS::error; break;
                    case '3': state = PS::indeterminate; break;
                    case '4': state = PS::pause; break;
                    default: goto not_conemu;
                }
                parser->command = Command();
                parser->command.key = Command::Key::conemu_progress_report;
                parser->command.conemu_progress_report.state = state;
                /* '1' sets .progress = 0 */
                if (state == PS::set) {
                    parser->command.conemu_progress_report.has_progress = true;
                    parser->command.conemu_progress_report.progress = 0;
                }
                switch (state) {
                    case PS::remove:
                    case PS::indeterminate:
                        break;
                    case PS::set:
                    case PS::error:
                    case PS::pause: {
                        if (data_len < 4) break;
                        if (data[3] != ';') break;
                        /* parse the progress value */
                        uint64_t v;
                        if (!parse_unsigned(data + 4, data_len - 4, SIZE_MAX, &v)) {
                            parser->command.conemu_progress_report.has_progress = false;
                        } else {
                            parser->command.conemu_progress_report.has_progress = true;
                            parser->command.conemu_progress_report.progress =
                                (uint8_t)(v > 100 ? 100 : v);
                        }
                        break;
                    }
                }
                return &parser->command;
            }
            /* OSC 9;5 wait for input */
            case '5': {
                parser->command = Command();
                parser->command.key = Command::Key::conemu_wait_input;
                return &parser->command;
            }
            /* OSC 9;6 guimacro */
            case '6': {
                if (data_len < 2) goto not_conemu;
                if (data[1] != ';') goto not_conemu;
                ZStr v;
                if (!terminated_tail(parser, 2, &v)) return nullptr;
                parser->command = Command();
                parser->command.key = Command::Key::conemu_guimacro;
                parser->command.conemu_guimacro = v;
                return &parser->command;
            }
            /* OSC 9;7 run process */
            case '7': {
                if (data_len < 2) goto not_conemu;
                if (data[1] != ';') goto not_conemu;
                ZStr v;
                if (!terminated_tail(parser, 2, &v)) return nullptr;
                parser->command = Command();
                parser->command.key = Command::Key::conemu_run_process;
                parser->command.conemu_run_process = v;
                return &parser->command;
            }
            /* OSC 9;8 output environment variable */
            case '8': {
                if (data_len < 2) goto not_conemu;
                if (data[1] != ';') goto not_conemu;
                ZStr v;
                if (!terminated_tail(parser, 2, &v)) return nullptr;
                parser->command = Command();
                parser->command.key = Command::Key::conemu_output_environment_variable;
                parser->command.conemu_output_environment_variable = v;
                return &parser->command;
            }
            /* OSC 9;9 current working directory */
            case '9': {
                if (data_len < 2) goto not_conemu;
                if (data[1] != ';') goto not_conemu;
                ZStr v;
                if (!terminated_tail(parser, 2, &v)) return nullptr;
                parser->command = Command();
                parser->command.key = Command::Key::report_pwd;
                parser->command.report_pwd.value = v;
                return &parser->command;
            }
            default: goto not_conemu;
        }
    }

not_conemu:
    /* If it's not a ConEmu OSC, it's an iTerm2 notification */
    {
        ZStr body;
        if (!terminated_tail(parser, 0, &body)) return nullptr;
        parser->command = Command();
        parser->command.key = Command::Key::show_desktop_notification;
        parser->command.show_desktop_notification.title = ZStr();
        parser->command.show_desktop_notification.body = body;
        return &parser->command;
    }
}
} /* namespace osc9 */

namespace kitty_desktop_notification {
inline Command *parse(Parser *parser, bool has_ch, uint8_t terminator_ch) {
    namespace kdn = ::wisp::terminal::osc::kitty_desktop_notification;
    /* assert(parser.state == .@"99") */

    if (!parser->has_capture) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    Parser::Capture *cap = &parser->capture;

    const char *data = cap->trailing();
    const size_t data_len = cap->trailing_len();

    const char *semi = (const char *)memchr(data, ';', data_len);
    if (!semi) {
        /* log.warn("missing semicolon before payload") */
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    const size_t payload_start = (size_t)(semi - data);

    const ZStr metadata(data, payload_start);
    const ZStr payload(data + payload_start + 1, data_len - (payload_start + 1));

    const size_t max_payload_bytes =
        kdn::Option_readBool(kdn::Option::e, metadata.ptr, metadata.len)
            ? kdn::MAX_ENCODED_PAYLOAD_BYTES
            : kdn::MAX_PLAIN_PAYLOAD_BYTES;
    if (payload.len > max_payload_bytes) {
        /* log.warn("payload is too large: size={d} max={d}") */
        parser->state = Parser::State::invalid;
        return nullptr;
    }

    /* Payload has to be an escape-code-safe UTF-8 string. */
    if (!isSafeUtf8(payload.ptr, payload.len)) {
        /* log.warn("payload is not escape code safe UTF-8") */
        parser->state = Parser::State::invalid;
        return nullptr;
    }

    parser->command = Command();
    parser->command.key = Command::Key::kitty_desktop_notification;
    parser->command.kitty_desktop_notification.metadata = metadata;
    parser->command.kitty_desktop_notification.payload = payload;
    parser->command.kitty_desktop_notification.terminator = terminator_init(has_ch, terminator_ch);

    return &parser->command;
}
} /* namespace kitty_desktop_notification */

namespace kitty_dnd_protocol {
inline Command *parse(Parser *parser, bool has_ch, uint8_t terminator_ch) {
    /* assert(parser.state == .@"72") */
    if (!parser->has_capture) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    Parser::Capture *cap = &parser->capture;

    const char *data = cap->trailing();
    const size_t data_len = cap->trailing_len();

    ZStr metadata(data, data_len);
    bool has_payload = false;
    ZStr payload;
    const char *sep = (const char *)memchr(data, ';', data_len);
    if (sep) {
        const size_t i = (size_t)(sep - data);
        metadata = ZStr(data, i);
        has_payload = true;
        payload = ZStr(data + i + 1, data_len - (i + 1));
    }

    parser->command = Command();
    parser->command.key = Command::Key::kitty_dnd_protocol;
    parser->command.kitty_dnd_protocol.metadata = metadata;
    parser->command.kitty_dnd_protocol.has_payload = has_payload;
    parser->command.kitty_dnd_protocol.payload = payload;
    parser->command.kitty_dnd_protocol.terminator = terminator_init(has_ch, terminator_ch);

    return &parser->command;
}
} /* namespace kitty_dnd_protocol */

namespace rxvt_extension {
/* Parse OSC 777 */
inline Command *parse(Parser *parser, bool, uint8_t) {
    if (!parser->has_capture) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    Parser::Capture *cap = &parser->capture;
    /* ensure that we are sentinel terminated */
    if (!cap->writer.writeByte(0)) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    char *data = cap->trailing();
    const size_t data_len = cap->trailing_len();
    const char *semi = (const char *)memchr(data, ';', data_len);
    if (!semi) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    const size_t k = (size_t)(semi - data);
    if (!(k == 6 && memcmp(data, "notify", 6) == 0)) {
        /* log.warn("unknown rxvt extension: {s}") */
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    const char *semi2 = (const char *)memchr(data + k + 1, ';', data_len - (k + 1));
    if (!semi2) {
        /* log.warn("rxvt notify extension is missing the title") */
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    const size_t t = (size_t)(semi2 - data);
    data[t] = 0;
    parser->command = Command();
    parser->command.key = Command::Key::show_desktop_notification;
    parser->command.show_desktop_notification.title = ZStr(data + k + 1, t - (k + 1));
    parser->command.show_desktop_notification.body = ZStr(data + t + 1, data_len - 1 - (t + 1));
    return &parser->command;
}
} /* namespace rxvt_extension */

namespace color {

using ::wisp::terminal::osc::color::Operation;
using ::wisp::terminal::osc::color::Target;
using ::wisp::terminal::osc::color::Request;
using ::wisp::terminal::osc::color::List;

/* Wisp: std.fmt.parseInt(u9, s, 10). An optional leading '+' is accepted;
 * a value above 511 overflows. Zig additionally accepts '-' on zero and
 * '_' separators, which appear in none of upstream's tests and are not
 * reproduced. */
inline bool parse_u9(const char *s, size_t len, uint16_t *out) {
    size_t i = 0;
    if (i < len && s[i] == '+') i++;
    if (i >= len) return false;
    unsigned v = 0;
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (unsigned)(s[i] - '0');
        if (v > 511) return false;
    }
    *out = (uint16_t)v;
    return true;
}

/* Wisp: std.mem.tokenizeScalar(u8, buf, ';') — empty tokens are skipped,
 * which is not what split does, and upstream relies on the difference. */
struct TokenIterator {
    const char *buf;
    size_t      len;
    size_t      index;

    bool next(const char **tok, size_t *tok_len) {
        while (index < len && buf[index] == ';') index++;
        if (index >= len) return false;
        const size_t start = index;
        while (index < len && buf[index] != ';') index++;
        *tok = buf + start;
        *tok_len = index - start;
        return true;
    }
};

inline bool is_query(const char *s, size_t len) { return len == 1 && s[0] == '?'; }

/* OSC 4/5 */
inline bool parseGetSetAnsiColor(Operation op, TokenIterator *it, List *result) {
    /* Note: in ANY error scenario below we return the accumulated results.
     * This matches the xterm behavior (see misc.c ChangeAnsiColorRequest) */

    while (true) {
        /* We expect a `c; spec` pair. If either doesn't exist then
         * we return the results up to this point. */
        const char *color_str, *spec_str;
        size_t color_len, spec_len;
        if (!it->next(&color_str, &color_len)) return true;
        if (!it->next(&spec_str, &spec_len)) return true;

        /* Color must be numeric. u9 because that'll fit our palette +
         * special */
        uint16_t color;
        if (!parse_u9(color_str, color_len, &color)) return true;

        /* Parse the color. */
        Target target;
        if (op == Operation::osc_5) {
            /* OSC5 maps directly to the Special enum. */
            if (color > 7 || color > 4) return true;
            target = Target::makeSpecial((::wisp::terminal::Special)color);
        } else {
            /* OSC4 maps 0-255 to palette, 256-259 to special offset
             * by the palette count. */
            if (color <= 255) {
                target = Target::makePalette((uint8_t)color);
            } else {
                const unsigned sp = (unsigned)color - 256;
                if (sp > 7 || sp > 4) return true;
                target = Target::makeSpecial((::wisp::terminal::Special)sp);
            }
        }

        /* "?" always results in a query. */
        if (is_query(spec_str, spec_len)) {
            Request *req = result->addOne();
            if (!req) return false;
            *req = Request();
            req->tag = Request::Tag::query;
            req->query = target;
            continue;
        }

        ::wisp::terminal::RGB rgb;
        if (::wisp::terminal::RGB::parse(spec_str, spec_len, &rgb) !=
            ::wisp::terminal::ColorError::none) {
            return true;
        }
        Request *req = result->addOne();
        if (!req) return false;
        *req = Request();
        req->tag = Request::Tag::set;
        req->set.target = target;
        req->set.color = rgb;
    }
}

/* OSC 104/105: Reset ANSI Colors */
inline bool parseResetAnsiColor(Operation op, TokenIterator *it, List *result) {
    /* Note: xterm stops parsing the reset list on any error, but we're
     * more flexible and try the next value. This matches the behavior of
     * Kitty and I don't see a downside to being more flexible here.
     * Hopefully no one depends on the exact behavior of xterm. */

    while (true) {
        const char *color_str;
        size_t color_len;
        if (!it->next(&color_str, &color_len)) {
            /* If no parameters are given, we reset the full table. */
            if (result->count() == 0) {
                Request *req = result->addOne();
                if (!req) return false;
                *req = Request();
                req->tag = op == Operation::osc_104 ? Request::Tag::reset_palette
                                                    : Request::Tag::reset_special;
            }
            return true;
        }

        /* Empty color strings are ignored, not treated as an error. */
        if (color_len == 0) continue;

        /* Color must be numeric. u9 because that'll fit our palette +
         * special */
        uint16_t color;
        if (!parse_u9(color_str, color_len, &color)) continue;

        /* Parse the color. */
        Target target;
        if (op == Operation::osc_105) {
            /* OSC105 maps directly to the Special enum. */
            if (color > 7 || color > 4) continue;
            target = Target::makeSpecial((::wisp::terminal::Special)color);
        } else {
            /* OSC104 maps 0-255 to palette, 256-259 to special offset
             * by the palette count. */
            if (color <= 255) {
                target = Target::makePalette((uint8_t)color);
            } else {
                const unsigned sp = (unsigned)color - 256;
                if (sp > 7 || sp > 4) continue;
                target = Target::makeSpecial((::wisp::terminal::Special)sp);
            }
        }

        Request *req = result->addOne();
        if (!req) return false;
        *req = Request();
        req->tag = Request::Tag::reset;
        req->reset = target;
    }
}

/* OSC 10-19: Get/Set Dynamic Colors */
inline bool parseGetSetDynamicColor(::wisp::terminal::Dynamic start,
                                    TokenIterator *it, List *result) {
    /* Note: in ANY error scenario below we return the accumulated results.
     * This matches the xterm behavior (see misc.c ChangeColorsRequest) */

    ::wisp::terminal::Dynamic color = start;
    while (true) {
        const char *spec_str;
        size_t spec_len;
        if (!it->next(&spec_str, &spec_len)) return true;

        if (is_query(spec_str, spec_len)) {
            Request *req = result->addOne();
            if (!req) return false;
            *req = Request();
            req->tag = Request::Tag::query;
            req->query = Target::makeDynamic(color);
        } else {
            ::wisp::terminal::RGB rgb;
            if (::wisp::terminal::RGB::parse(spec_str, spec_len, &rgb) !=
                ::wisp::terminal::ColorError::none) {
                return true;
            }
            Request *req = result->addOne();
            if (!req) return false;
            *req = Request();
            req->tag = Request::Tag::set;
            req->set.target = Target::makeDynamic(color);
            req->set.color = rgb;
        }

        /* Each successive value uses the next color so long as it exists. */
        if (!::wisp::terminal::dynamic_next(color, &color)) return true;
    }
}

/* OSC 110-119: Reset Dynamic Colors */
inline bool parseResetDynamicColor(::wisp::terminal::Dynamic color,
                                   TokenIterator *it, List *result) {
    const char *tok;
    size_t tok_len;
    if (it->next(&tok, &tok_len)) return true;
    Request *req = result->addOne();
    if (!req) return false;
    *req = Request();
    req->tag = Request::Tag::reset;
    req->reset = Target::makeDynamic(color);
    return true;
}

/* Parse any color operation string. This should NOT include the operation
 * itself, but only the body of the operation. e.g. for "4;a;b;c" the body
 * should be "a;b;c" and the operation should be set accordingly.
 *
 * Color parsing is fairly complicated so we pull this out to a specialized
 * function rather than go through our OSC parsing state machine. This is
 * much slower and requires more memory (since we need to buffer the full
 * request) but grants us an easier to understand and testable
 * implementation.
 *
 * If color changing ends up being a bottleneck we can optimize this later.
 *
 * Wisp: ParseError!List is the bool return plus *out; false means an
 * allocation failed. */
inline bool parseColor(Operation op, const char *buf, size_t len, List *out) {
    typedef ::wisp::terminal::Dynamic D;
    TokenIterator it;
    it.buf = buf;
    it.len = len;
    it.index = 0;
    switch (op) {
        case Operation::osc_4: return parseGetSetAnsiColor(Operation::osc_4, &it, out);
        case Operation::osc_5: return parseGetSetAnsiColor(Operation::osc_5, &it, out);
        case Operation::osc_104: return parseResetAnsiColor(Operation::osc_104, &it, out);
        case Operation::osc_105: return parseResetAnsiColor(Operation::osc_105, &it, out);
        case Operation::osc_10: return parseGetSetDynamicColor(D::foreground, &it, out);
        case Operation::osc_11: return parseGetSetDynamicColor(D::background, &it, out);
        case Operation::osc_12: return parseGetSetDynamicColor(D::cursor, &it, out);
        case Operation::osc_13: return parseGetSetDynamicColor(D::pointer_foreground, &it, out);
        case Operation::osc_14: return parseGetSetDynamicColor(D::pointer_background, &it, out);
        case Operation::osc_15: return parseGetSetDynamicColor(D::tektronix_foreground, &it, out);
        case Operation::osc_16: return parseGetSetDynamicColor(D::tektronix_background, &it, out);
        case Operation::osc_17: return parseGetSetDynamicColor(D::highlight_background, &it, out);
        case Operation::osc_18: return parseGetSetDynamicColor(D::tektronix_cursor, &it, out);
        case Operation::osc_19: return parseGetSetDynamicColor(D::highlight_foreground, &it, out);
        case Operation::osc_110: return parseResetDynamicColor(D::foreground, &it, out);
        case Operation::osc_111: return parseResetDynamicColor(D::background, &it, out);
        case Operation::osc_112: return parseResetDynamicColor(D::cursor, &it, out);
        case Operation::osc_113: return parseResetDynamicColor(D::pointer_foreground, &it, out);
        case Operation::osc_114: return parseResetDynamicColor(D::pointer_background, &it, out);
        case Operation::osc_115: return parseResetDynamicColor(D::tektronix_foreground, &it, out);
        case Operation::osc_116: return parseResetDynamicColor(D::tektronix_background, &it, out);
        case Operation::osc_117: return parseResetDynamicColor(D::highlight_background, &it, out);
        case Operation::osc_118: return parseResetDynamicColor(D::tektronix_cursor, &it, out);
        case Operation::osc_119: return parseResetDynamicColor(D::highlight_foreground, &it, out);
    }
    return true;
}

/* Parse OSCs 4, 5, 10-19, 104, 110-119 */
inline Command *parse(Parser *parser, bool has_ch, uint8_t terminator_ch) {
    if (!parser->alloc) {
        parser->state = Parser::State::invalid;
        return nullptr;
    }
    /* If we've collected any extra data parse that, otherwise use an empty
     * string. */
    const char *data = "";
    size_t data_len = 0;
    if (parser->has_capture) {
        data = parser->capture.trailing();
        data_len = parser->capture.trailing_len();
    }

    /* Check and make sure that we're parsing the correct OSCs */
    typedef Parser::State S;
    Operation op;
    switch (parser->state) {
        case S::s4: op = Operation::osc_4; break;
        case S::s5: op = Operation::osc_5; break;
        case S::s10: op = Operation::osc_10; break;
        case S::s11: op = Operation::osc_11; break;
        case S::s12: op = Operation::osc_12; break;
        case S::s13: op = Operation::osc_13; break;
        case S::s14: op = Operation::osc_14; break;
        case S::s15: op = Operation::osc_15; break;
        case S::s16: op = Operation::osc_16; break;
        case S::s17: op = Operation::osc_17; break;
        case S::s18: op = Operation::osc_18; break;
        case S::s19: op = Operation::osc_19; break;
        case S::s104: op = Operation::osc_104; break;
        case S::s110: op = Operation::osc_110; break;
        case S::s111: op = Operation::osc_111; break;
        case S::s112: op = Operation::osc_112; break;
        case S::s113: op = Operation::osc_113; break;
        case S::s114: op = Operation::osc_114; break;
        case S::s115: op = Operation::osc_115; break;
        case S::s116: op = Operation::osc_116; break;
        case S::s117: op = Operation::osc_117; break;
        case S::s118: op = Operation::osc_118; break;
        case S::s119: op = Operation::osc_119; break;
        default:
            parser->state = S::invalid;
            return nullptr;
    }

    parser->command = Command();
    parser->command.key = Command::Key::color_operation;
    parser->command.color_operation.op = op;
    if (!parseColor(op, data, data_len, &parser->command.color_operation.requests)) {
        /* log.info("failed to parse OSC ... color request") and an empty
         * list, as upstream falls back to .{} */
        parser->command.color_operation.requests.deinit();
    }
    parser->command.color_operation.terminator = terminator_init(has_ch, terminator_ch);
    return &parser->command;
}

} /* namespace color */

} /* namespace parsers */

inline Command *Parser::end(bool has_ch, uint8_t ch) {
    typedef State S;

    switch (state) {
        case S::start: return nullptr;
        case S::invalid: return nullptr;

        case S::s0:
        case S::s2:
            return parsers::change_window_title::parse(this, has_ch, ch);

        case S::s1:
            return parsers::change_window_icon::parse(this, has_ch, ch);

        case S::s4: case S::s5:
        case S::s10: case S::s11: case S::s12: case S::s13: case S::s14:
        case S::s15: case S::s16: case S::s17: case S::s18: case S::s19:
        case S::s104:
        case S::s110: case S::s111: case S::s112: case S::s113: case S::s114:
        case S::s115: case S::s116: case S::s117: case S::s118: case S::s119:
            return parsers::color::parse(this, has_ch, ch);

        case S::s7:
            return parsers::report_pwd::parse(this, has_ch, ch);

        case S::s8:
            return parsers::hyperlink::parse(this, has_ch, ch);

        case S::s9:
            return parsers::osc9::parse(this, has_ch, ch);

        case S::s21:
            return parsers::kitty_color::parse(this, has_ch, ch);

        case S::s22:
            return parsers::mouse_shape::parse(this, has_ch, ch);

        case S::s52:
            return parsers::clipboard_operation::parse(this, has_ch, ch);

        case S::s55: return nullptr;

        case S::s3:
        case S::s30:
        case S::s300:
            return nullptr;

        case S::s3008:
            return parsers::context_signal::parse(this, has_ch, ch);

        case S::s6: return nullptr;

        case S::s66:
            return parsers::kitty_text_sizing::parse(this, has_ch, ch);

        case S::s72:
            return parsers::kitty_dnd_protocol::parse(this, has_ch, ch);

        case S::s77: return nullptr;

        case S::s99:
            return parsers::kitty_desktop_notification::parse(this, has_ch, ch);

        case S::s133:
            return parsers::semantic_prompt::parse(this, has_ch, ch);

        case S::s552: return nullptr;

        case S::s777: return parsers::rxvt_extension::parse(this, has_ch, ch);

        case S::s1337:
            return parsers::iterm2::parse(this, has_ch, ch);

        case S::s5522:
            return parsers::kitty_clipboard_protocol::parse(this, has_ch, ch);
    }
    return nullptr;
}

} /* namespace osc */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_OSC_HPP */
