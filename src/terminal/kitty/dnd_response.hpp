/* Transliterated from Ghostty src/terminal/kitty/dnd_response.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Wisp, differences in shape rather than behavior:
 *   - `*std.Io.Writer` is `std::string *`, so encoding cannot fail.
 *   - `RequestKeys.format` is `RequestKeys::format(std::string *)`.
 *   - errno.h defines several of Errno's names as macros, so those
 *     members carry a trailing underscore; Errno_name gives the wire name.
 */

#pragma once
#ifndef WISP_TERMINAL_KITTY_DND_RESPONSE_HPP
#define WISP_TERMINAL_KITTY_DND_RESPONSE_HPP

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "../../zigstd/base64.hpp"
#include "../osc.hpp"

namespace wisp {
namespace terminal {
namespace kitty {
namespace dnd {

typedef ::wisp::terminal::osc::Terminator Terminator;
typedef ::wisp::terminal::osc::ZStr ZStr;

/* The maximum raw bytes per base64-encoded chunk, chosen by kitty so a
 * chunk is exactly 4096 base64 characters (the protocol's chunk limit). */
static const size_t max_chunk_raw = 3072;

/* The maximum bytes per plain-text (non-base64) chunk. */
static const size_t max_chunk_plain = 4096;

/* Error names used in protocol error payloads. The wire encoding is
 * the tag name itself. This matches kitty's get_errno_name vocabulary,
 * which extends the spec's list with EISDIR, ENOSPC, and OK. */
enum class Errno : uint8_t {
    OK,
    EPERM_,
    ENOENT_,
    EIO_,
    EINVAL_,
    EMFILE_,
    ENOMEM_,
    EFBIG_,
    EISDIR_,
    ENOSPC_,
    EUNKNOWN,
};

/* Wisp: @tagName(errno). */
inline const char *Errno_name(Errno e) {
    static const char *const names[] = {
        "OK", "EPERM", "ENOENT", "EIO", "EINVAL", "EMFILE", "ENOMEM", "EFBIG", "EISDIR", "ENOSPC", "EUNKNOWN",
    };
    return names[(size_t)e];
}

/* The payload encoding for a message. */
enum class Encoding : uint8_t {
    /* Payload bytes are sent as-is (MIME lists, error strings). */
    plain,

    /* Payload bytes are base64-encoded (all binary data). */
    base64,
};

/* The `x`/`y`/`Y` keys of the data request currently being answered,
 * echoed in responses and errors so the client can match them up. Only
 * non-zero keys are written, matching kitty's drop_append_request_keys. */
struct RequestKeys {
    int32_t x; /* = 0 */
    int32_t y; /* = 0 */
    int32_t Y; /* = 0 */

    RequestKeys() : x(0), y(0), Y(0) {}

    void format(std::string *writer) const {
        char buf[32];
        if (x != 0) {
            snprintf(buf, sizeof buf, ":x=%d", (int)x);
            writer->append(buf);
        }
        if (y != 0) {
            snprintf(buf, sizeof buf, ":y=%d", (int)y);
            writer->append(buf);
        }
        if (Y != 0) {
            snprintf(buf, sizeof buf, ":Y=%d", (int)Y);
            writer->append(buf);
        }
    }
};

/* Encode a complete protocol message: one bare OSC when `data` is
 * empty, otherwise one complete OSC per chunk of `data`, each
 * repeating the header. The final chunk carries `m=0`, earlier
 * chunks `m=1`.
 *
 * `header` is the metadata without the OSC introducer, e.g. "t=q" or
 * "t=m:x=5:y=3". The client ID is appended as `:i=N` when non-zero. */
inline void encode(std::string *writer, ZStr header, uint32_t client_id, ZStr data, Encoding encoding,
                   Terminator terminator) {
    /* The client ID is part of the repeated header. */
    char id_buf[16];
    if (client_id != 0) {
        snprintf(id_buf, sizeof id_buf, ":i=%u", (unsigned)client_id);
    } else {
        id_buf[0] = 0;
    }
    const char *id = id_buf;

    if (data.len == 0) {
        writer->append("\x1b]72;");
        writer->append(header.ptr, header.len);
        writer->append(id);
        writer->append(osc::terminator_string(terminator));
        return;
    }

    const size_t limit = encoding == Encoding::base64 ? max_chunk_raw : max_chunk_plain;

    size_t offset = 0;
    while (offset < data.len) {
        const size_t left = data.len - offset;
        const size_t chunk_len = left < limit ? left : limit;
        const char *chunk = data.ptr + offset;
        offset += chunk_len;
        const char last = offset >= data.len ? '0' : '1';

        writer->append("\x1b]72;");
        writer->append(header.ptr, header.len);
        writer->append(id);
        writer->append(":m=");
        writer->push_back(last);
        writer->push_back(';');
        switch (encoding) {
        case Encoding::plain: writer->append(chunk, chunk_len); break;
        case Encoding::base64: zigstd::base64::encodeWriter(writer, chunk, chunk_len); break;
        }
        writer->append(osc::terminator_string(terminator));
    }
}

/* Wisp: the anonymous enum of encodeError's `kind` parameter. */
enum class ErrorKind : uint8_t { drop, drag };

/* Encode an error response. `kind` selects the header: t=R for drop
 * data request errors, t=E for drag offer errors. The payload is
 * "NAME" or "NAME:description", sent plain (not base64). */
inline void encodeError(std::string *writer, ErrorKind kind, RequestKeys keys, uint32_t client_id, Errno errno_,
                        ZStr desc, Terminator terminator) {
    std::string header;
    header.push_back('t');
    header.push_back('=');
    header.push_back(kind == ErrorKind::drop ? 'R' : 'E');
    keys.format(&header);

    /* Description strings are short static messages; size the buffer
     * for the longest error name plus a generous description. */
    std::string payload;
    payload.append(Errno_name(errno_));
    if (desc.len > 0) {
        payload.push_back(':');
        payload.append(desc.ptr, desc.len);
    }

    encode(writer, ZStr(header.data(), header.size()), client_id, ZStr(payload.data(), payload.size()),
           Encoding::plain, terminator);
}

} /* namespace dnd */
} /* namespace kitty */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_KITTY_DND_RESPONSE_HPP */
