/* Transliterated from Ghostty src/terminal/kitty/clipboard_command.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Kitty clipboard protocol (OSC 5522) request decoding.
 *
 * OSC 5522 format: `<OSC>5522;metadata;payload<ST>`
 *
 * Per the spec: "metadata is a colon separated list of key-value
 * pairs and payload is base64 encoded data."
 *
 * This contains the logic for parsing this.
 *
 * Wisp, differences in shape rather than behavior:
 *   - `error{...}!?Metadata` is a ParseError return plus the Metadata
 *     through an out parameter; `dropped` is the null result.
 *   - `[]const u8` is osc::ZStr.
 *   - The MIME iterator is a small struct with `next`, standing in for
 *     std.mem.TokenIterator.
 */

#pragma once
#ifndef WISP_TERMINAL_KITTY_CLIPBOARD_COMMAND_HPP
#define WISP_TERMINAL_KITTY_CLIPBOARD_COMMAND_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../simd/base64.hpp"
#include "../../zigstd/allocator.hpp"
#include "../../zigstd/unicode.hpp"
#include "../clipboard.hpp"
#include "../osc.hpp"

namespace wisp {
namespace terminal {
namespace kitty {
namespace clipboard {

typedef ::wisp::terminal::osc::ZStr ZStr;
typedef ::wisp::terminal::osc::kitty_clipboard_protocol::Operation Operation;

/* Maximum id length, nothing specified but this is the limit used
 * in Kitty's source so we'll match it. */
static const size_t max_id_len = 512;

/* Maximum decoded password length. Kitty has no limit. Passwords are
 * UUID-sized in practice so anything longer simply never matches a
 * stored grant anyway. */
static const size_t max_pw_len = 128;

/* Maximum decoded MIME type length. Kitty has no limit but real MIME
 * types are tiny; anything longer drops the packet. */
static const size_t max_mime_len = 256;

/* Maximum decoded name length. Kitty has no limit but names are shown
 * in permission prompts so anything longer drops the packet. */
static const size_t max_name_len = 256;

/* Wisp: std.base64.standard.Encoder.calcSize. */
inline size_t base64CalcSize(size_t source_len) { return ((source_len + 2) / 3) * 4; }

/* The decoded, validated metadata of one OSC 5522 sequence.
 *
 * All slice values are allocated from the allocator given to parse and
 * are sized to their contents. Callers are expected to pass an arena
 * scoped to handling the packet. There is no deinit. */
struct Metadata {
    /* Wisp: `error{ OutOfMemory, InvalidValue }!?Metadata` flattened.
     * `dropped` is upstream's null result. */
    enum class ParseError : uint8_t {
        none,
        dropped,
        OutOfMemory,
        InvalidValue,
    };

    Operation op;

    /* The clipboard this operation targets. Per the spec: "To read
     * from the primary selection instead of the clipboard, add the
     * key `loc=primary` to the metadata section." Any other value
     * means the clipboard, so only standard and primary are possible
     * here. */
    ::wisp::terminal::clipboard::Location loc; /* = .standard */

    /* Sanitized id: invalid characters stripped, truncated to
     * max_id_len. Empty means no id. */
    ZStr id; /* = "" */

    /* Decoded mime metadata value. Empty means absent; kitty treats an
     * empty mime the same as a missing one everywhere it matters (a
     * wdata packet with either commits the transaction). */
    ZStr mime; /* = "" */

    /* Decoded password. Empty means absent. Per the spec:
     * "Specifying a password without a human friendly name is
     * equivalent to not specifying a password and the terminal must
     * treat the request as though it had no password." */
    ZStr pw; /* = "" */

    /* Decoded human friendly name of the requesting program, shown in
     * permission prompts. Empty means absent. Its presence opts into
     * password grants. */
    ZStr name; /* = "" */

    Metadata()
        : op(Operation::read), loc(::wisp::terminal::clipboard::Location::standard), id(), mime(), pw(), name() {}

    struct Raw {
        bool has_op;
        ZStr op;
        ZStr loc;
        ZStr id;
        ZStr mime;
        ZStr pw;
        ZStr name;

        Raw() : has_op(false), op(), loc(), id(), mime(), pw(), name() {}

        static bool parse(ZStr raw, Raw *out) {
            Raw result;

            /* This visits every record even though an empty raw string
             * yields one empty record. Validating the complete structure
             * before decoding also ensures the last duplicate value wins. */
            size_t start = 0;
            while (true) {
                size_t end = start;
                while (end < raw.len && raw.ptr[end] != ':') end += 1;
                const ZStr record(raw.ptr + start, end - start);

                size_t eql_idx = 0;
                bool found = false;
                for (; eql_idx < record.len; eql_idx++) {
                    if (record.ptr[eql_idx] == '=') {
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
                const ZStr key(record.ptr, eql_idx);
                const ZStr value(record.ptr + eql_idx + 1, record.len - eql_idx - 1);

                if (key.eql("type")) {
                    result.has_op = true;
                    result.op = value;
                } else if (key.eql("loc")) {
                    result.loc = value;
                } else if (key.eql("id")) {
                    result.id = value;
                } else if (key.eql("mime")) {
                    result.mime = value;
                } else if (key.eql("pw")) {
                    result.pw = value;
                } else if (key.eql("name")) {
                    result.name = value;
                }
                /* Unknown keys are ignored. */

                if (end >= raw.len) break;
                start = end + 1;
            }

            *out = result;
            return true;
        }
    };

    /* Sanitize the ID according to the spec:
     *
     * Valid ids must include only characters from the set: [a-zA-Z0-9-_+.].
     * Any other characters must be stripped out from the id by the terminal
     * emulator before retransmitting it. */
    static bool sanitizeId(zigstd::Allocator alloc, ZStr value, ZStr *out) {
        size_t n = 0;
        for (size_t i = 0; i < value.len; i++) {
            const char c = value.ptr[i];
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                            c == '-' || c == '_' || c == '+' || c == '.';
            if (!ok) continue;
            if (n >= max_id_len) break;
            n += 1;
        }
        if (n == 0) {
            *out = ZStr();
            return true;
        }
        uint8_t *buf = alloc.allocT<uint8_t>(n);
        if (buf == nullptr) return false;
        size_t w = 0;
        for (size_t i = 0; i < value.len && w < n; i++) {
            const char c = value.ptr[i];
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                            c == '-' || c == '_' || c == '+' || c == '.';
            if (!ok) continue;
            buf[w] = (uint8_t)c;
            w += 1;
        }
        *out = ZStr((const char *)buf, n);
        return true;
    }

    /* Wisp: decodeValue's error set. */
    enum class DecodeError : uint8_t {
        none,
        OutOfMemory,
        Overflow,
        InvalidBase64,
        InvalidUtf8,
    };

    /* Base64-decode a metadata value. Per the spec these values "are
     * UTF-8 strings that are base64 encoded", so the decoded result
     * must be valid UTF-8, and the encoding is strict RFC 4648 with
     * required padding per the spec's "Encoding of payloads" section. */
    static DecodeError decodeValue(zigstd::Allocator alloc, ZStr value, size_t max_len, ZStr *out) {
        /* Avoid hostile large payloads. */
        if (value.len > base64CalcSize(max_len)) return DecodeError::Overflow;

        /* Decode */
        const size_t buf_len = simd::base64::maxLen((const uint8_t *)value.ptr, value.len);
        uint8_t *buf = alloc.allocT<uint8_t>(buf_len == 0 ? 1 : buf_len);
        if (buf == nullptr) return DecodeError::OutOfMemory;
        size_t decoded_len = 0;
        if (!simd::base64::decodeStrict((const uint8_t *)value.ptr, value.len, buf, buf_len,
                                        simd::base64::Padding::required, &decoded_len))
            return DecodeError::InvalidBase64;

        /* Must be valid UTF-8 */
        if (!zigstd::utf8ValidateSlice((const char *)buf, decoded_len)) return DecodeError::InvalidUtf8;
        if (decoded_len > max_len) return DecodeError::Overflow;
        *out = ZStr((const char *)buf, decoded_len);
        return DecodeError::none;
    }

    /* Parse the metadata field. The raw value is expected to be exactly
     * the metadata (prefix and payload and separators stripped out).
     *
     * A null result means the packet should be silently dropped. An
     * InvalidValue error means a decoded textual value was not valid
     * UTF-8 or exceeded a local safety limit. Callers use the raw
     * operation to decide whether that invalid value aborts an in-flight
     * write transaction. */
    static ParseError parse(zigstd::Allocator alloc, ZStr raw, Metadata *out) {
        Raw fields;
        if (!Raw::parse(raw, &fields)) return ParseError::dropped;
        if (!fields.has_op) return ParseError::dropped;

        Metadata result;
        if (!osc::kitty_clipboard_protocol::Operation_init(fields.op.ptr, fields.op.len, &result.op))
            return ParseError::dropped;
        result.loc = fields.loc.eql("primary") ? ::wisp::terminal::clipboard::Location::primary
                                               : ::wisp::terminal::clipboard::Location::standard;
        if (!sanitizeId(alloc, fields.id, &result.id)) return ParseError::OutOfMemory;

        switch (decodeValue(alloc, fields.mime, max_mime_len, &result.mime)) {
        case DecodeError::none: break;
        case DecodeError::OutOfMemory: return ParseError::OutOfMemory;
        case DecodeError::Overflow:
        case DecodeError::InvalidBase64:
        case DecodeError::InvalidUtf8: return ParseError::InvalidValue;
        }
        switch (decodeValue(alloc, fields.pw, max_pw_len, &result.pw)) {
        case DecodeError::none: break;
        case DecodeError::OutOfMemory: return ParseError::OutOfMemory;
        case DecodeError::InvalidBase64:
        case DecodeError::InvalidUtf8: return ParseError::InvalidValue;
        /* An over-long password behaves as if none was given: it can
         * never match a stored grant. */
        case DecodeError::Overflow: result.pw = ZStr(); break;
        }
        switch (decodeValue(alloc, fields.name, max_name_len, &result.name)) {
        case DecodeError::none: break;
        case DecodeError::OutOfMemory: return ParseError::OutOfMemory;
        case DecodeError::Overflow:
        case DecodeError::InvalidBase64:
        case DecodeError::InvalidUtf8: return ParseError::InvalidValue;
        }
        *out = result;
        return ParseError::none;
    }

    /* Return the last recognized operation from syntactically valid raw
     * metadata. This intentionally does not decode any values, so callers
     * can still classify an InvalidValue parse error. */
    static bool operation(ZStr raw, Operation *out) {
        Raw fields;
        if (!Raw::parse(raw, &fields)) return false;
        if (!fields.has_op) return false;
        return osc::kitty_clipboard_protocol::Operation_init(fields.op.ptr, fields.op.len, out);
    }
};

/* A decoded base64 payload of one OSC 5522 sequence, e.g. the MIME
 * type list of a read request or the alias list of a walias packet.
 * The data slice aliases the allocated buf. */
struct Payload {
    /* Wisp: `error{ OutOfMemory, Invalid }!Payload`. */
    enum class InitError : uint8_t {
        none,
        OutOfMemory,
        Invalid,
    };

    uint8_t *buf;
    size_t buf_len;
    ZStr data;

    Payload() : buf(nullptr), buf_len(0), data() {}

    /* Decode a base64 payload into freshly allocated memory. The
     * encoding is strict RFC 4648 with required padding per the
     * spec's "Encoding of payloads" section; how an invalid payload
     * is reported (or not) depends on the packet type. */
    static InitError init(zigstd::Allocator alloc, ZStr payload, Payload *out) {
        const size_t buf_len = simd::base64::maxLen((const uint8_t *)payload.ptr, payload.len);
        uint8_t *buf = alloc.allocT<uint8_t>(buf_len == 0 ? 1 : buf_len);
        if (buf == nullptr) return InitError::OutOfMemory;
        size_t data_len = 0;
        if (!simd::base64::decodeStrict((const uint8_t *)payload.ptr, payload.len, buf, buf_len,
                                        simd::base64::Padding::required, &data_len)) {
            alloc.freeT<uint8_t>(buf, buf_len == 0 ? 1 : buf_len);
            return InitError::Invalid;
        }
        out->buf = buf;
        out->buf_len = buf_len == 0 ? 1 : buf_len;
        out->data = ZStr((const char *)buf, data_len);
        return InitError::none;
    }

    void deinit(zigstd::Allocator alloc) const { alloc.freeT<uint8_t>(buf, buf_len); }

    bool isValidUtf8() const { return zigstd::utf8ValidateSlice(data.ptr, data.len); }

    /* Iterate the whitespace-separated MIME types of the payload.
     * Matches Python str.split() used by kitty.
     * Wisp: std.mem.TokenIterator(u8, .any) over std.ascii.whitespace. */
    struct MimeIterator {
        ZStr data;
        size_t index;

        static bool isWhitespace(char c) {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
        }

        void reset() { index = 0; }

        bool next(ZStr *out) {
            while (index < data.len && isWhitespace(data.ptr[index])) index += 1;
            if (index >= data.len) return false;
            const size_t start = index;
            while (index < data.len && !isWhitespace(data.ptr[index])) index += 1;
            *out = ZStr(data.ptr + start, index - start);
            return true;
        }
    };

    MimeIterator mimeIterator() const {
        MimeIterator it;
        it.data = data;
        it.index = 0;
        return it;
    }
};

/* Whether a read request with this many data MIME types (the targets
 * type '.' excluded) is exempt from the user permission prompt. */
inline bool readPromptExempt(size_t data_mimes) { return data_mimes == 0; }

} /* namespace clipboard */
} /* namespace kitty */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_KITTY_CLIPBOARD_COMMAND_HPP */
