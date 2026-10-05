/* Transliterated from Ghostty src/terminal/kitty/clipboard_response.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Kitty clipboard protocol (OSC 5522) response encoding.
 *
 * Wisp, differences in shape rather than behavior:
 *   - `*std.Io.Writer` is `std::string *`, so encoding cannot fail and
 *     every encode returns void.
 *   - `?[]const u8` is a `has_x` flag plus the ZStr.
 *   - `[]const []const u8` is a pointer plus a length.
 */

#pragma once
#ifndef WISP_TERMINAL_KITTY_CLIPBOARD_RESPONSE_HPP
#define WISP_TERMINAL_KITTY_CLIPBOARD_RESPONSE_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

#include "../../zigstd/base64.hpp"
#include "../clipboard.hpp"
#include "../osc.hpp"

namespace wisp {
namespace terminal {
namespace kitty {
namespace clipboard {

namespace b64 = ::wisp::zigstd::base64;
typedef ::wisp::terminal::osc::ZStr ZStr;
typedef ::wisp::terminal::clipboard::Content Content;
typedef ::wisp::terminal::osc::kitty_clipboard_protocol::Operation Operation;
typedef ::wisp::terminal::osc::kitty_clipboard_protocol::Status Status;
typedef ::wisp::terminal::osc::Terminator Terminator;

/* Maximum raw (pre-base64) bytes per DATA packet in read responses.
 * This is specified by the protocol. */
static const size_t read_chunk_size = 4096;

/* Maximum requested MIME types served by a single read request.
 * Requests beyond this simply see no DATA packets for the extras,
 * which is how the protocol communicates an unavailable type anyway. */
static const size_t max_read_mimes = 4;

/* The special MIME type that requests the list of available types. */
static const char targets_mime[] = ".";

/* Maximum MIME types reported in a paste event's targets listing. */
static const size_t max_listing_mimes = 16;

/* A single response packet. */
struct Response {
    Operation op;
    Status status;
    bool primary; /* = false */
    ZStr id;      /* = "" */
    bool has_mime;
    ZStr mime; /* = null */
    bool has_pw;
    ZStr pw; /* = null */
    /* The raw payload; the encoder base64-encodes it. An empty payload
     * emits no payload section at all (no ';'). */
    ZStr payload;           /* = "" */
    Terminator terminator;  /* = .st */

    Response()
        : op(Operation::read), status(Status::OK), primary(false), id(), has_mime(false), mime(),
          has_pw(false), pw(), payload(), terminator(Terminator::st) {}

    /* Encode the escape prefix and metadata section only: everything
     * up to (and not including) the payload section and terminator. */
    void encodeMetadata(std::string *writer) const {
        /* The exact order of fields here matches what Kitty does. */
        writer->append("\x1b]5522;type=");
        writer->append(osc::kitty_clipboard_protocol::Operation_name(op));
        writer->append(":status=");
        writer->append(osc::kitty_clipboard_protocol::Status_name(status));
        if (primary) writer->append(":loc=primary");
        if (id.len > 0) {
            writer->append(":id=");
            writer->append(id.ptr, id.len);
        }
        if (has_mime) {
            writer->append(":mime=");
            b64::encodeWriter(writer, mime.ptr, mime.len);
        }
        if (has_pw) {
            writer->append(":pw=");
            b64::encodeWriter(writer, pw.ptr, pw.len);
        }
    }

    /* Encode the response. Errors may result in partially written data
     * so it is up to callers to buffer it if they need to. */
    void encode(std::string *writer) const {
        encodeMetadata(writer);
        if (payload.len > 0) {
            writer->append(";");
            b64::encodeWriter(writer, payload.ptr, payload.len);
        }
        writer->append(osc::terminator_string(terminator));
    }
};

/* Encode a full successful read response: the OK packet, the targets
 * listing if requested, DATA chunks for each served representation,
 * and the final DONE packet. This is also the shape of an unsolicited
 * paste event (list=true, pw set to the one-time password). */
struct ReadSuccess {
    bool primary; /* = false */
    ZStr id;      /* = "" */

    /* One-time password echoed in every packet. Only used for
     * terminal-initiated paste events. */
    bool has_pw;
    ZStr pw; /* = null */

    /* True when the targets ('.') listing was requested. */
    bool list; /* = false */

    /* The MIME types available on the clipboard, reported by the
     * targets listing. Kitty reports these space-separated in one
     * packet with a trailing newline when non-empty. */
    const ZStr *available; /* = &.{} */
    size_t available_len;

    /* The representations to serve, in request order. Each entry's
     * data is chunked into DATA packets under its own MIME type. An
     * entry with empty data produces no packets, which is how the
     * protocol communicates an unavailable type. */
    const Content *contents; /* = &.{} */
    size_t contents_len;

    Terminator terminator; /* = .st */

    ReadSuccess()
        : primary(false), id(), has_pw(false), pw(), list(false), available(nullptr), available_len(0),
          contents(nullptr), contents_len(0), terminator(Terminator::st) {}

    /* Encode the targets ('.') listing packet. The listing gets a
     * trailing newline when non-empty. */
    void encodeListing(std::string *writer) const {
        Response listing;
        listing.op = Operation::read;
        listing.status = Status::DATA;
        listing.id = id;
        listing.has_mime = true;
        listing.mime = ZStr(targets_mime, 1);
        listing.has_pw = has_pw;
        listing.pw = pw;
        listing.terminator = terminator;
        listing.encodeMetadata(writer);
        if (available_len > 0) {
            writer->append(";");

            /* Join the types into one chunk before encoding. The
             * listing is a DATA packet, so it shares the pre-encoding
             * chunk size bound of any other data packet; types that
             * don't fit are dropped (a listing that large doesn't
             * happen in practice). */
            char raw[read_chunk_size];
            size_t end = 0;
            for (size_t i = 0; i < available_len; i++) {
                const ZStr mime = available[i];
                const size_t sep = i > 0 ? 1 : 0;
                if (end + sep + mime.len + 1 > sizeof raw) break;
                if (i > 0) raw[end++] = ' ';
                memcpy(raw + end, mime.ptr, mime.len);
                end += mime.len;
            }
            raw[end++] = '\n';
            b64::encodeWriter(writer, raw, end);
        }
        writer->append(osc::terminator_string(terminator));
    }

    void encode(std::string *writer) const {
        /* Initial read response */
        {
            Response r;
            r.op = Operation::read;
            r.status = Status::OK;
            r.primary = primary;
            r.id = id;
            r.has_pw = has_pw;
            r.pw = pw;
            r.terminator = terminator;
            r.encode(writer);
        }

        /* Listing of mimes if requested */
        if (list) encodeListing(writer);

        /* Encoding of each mime-type + content */
        for (size_t ci = 0; ci < contents_len; ci++) {
            const Content &content = contents[ci];
            size_t i = 0;
            while (i < content.data.len) {
                const size_t left = content.data.len - i;
                const size_t n = left < read_chunk_size ? left : read_chunk_size;
                Response r;
                r.op = Operation::read;
                r.status = Status::DATA;
                r.id = id;
                r.has_mime = true;
                r.mime = content.mime;
                r.has_pw = has_pw;
                r.pw = pw;
                r.payload = ZStr(content.data.ptr + i, n);
                r.terminator = terminator;
                r.encode(writer);
                i += n;
            }
        }

        /* Trailing done. */
        {
            Response r;
            r.op = Operation::read;
            r.status = Status::DONE;
            r.id = id;
            r.has_pw = has_pw;
            r.pw = pw;
            r.terminator = terminator;
            r.encode(writer);
        }
    }
};

/* An unsolicited Kitty paste event (mode 5522): a read response that
 * lists the clipboard's available MIME types and carries the one-time
 * password the program uses for its follow-up read. */
struct PasteEvent {
    /* True if the paste came from the primary selection, reported as
     * `loc=primary` on the OK packet. */
    bool primary; /* = false */

    /* The one-time password, echoed in every packet. */
    ZStr pw;

    /* The MIME types available on the clipboard. */
    const ZStr *available;
    size_t available_len;

    Terminator terminator; /* = .st */

    PasteEvent() : primary(false), pw(), available(nullptr), available_len(0), terminator(Terminator::st) {}

    void encode(std::string *writer) const {
        ReadSuccess r;
        r.primary = primary;
        r.has_pw = true;
        r.pw = pw;
        r.list = true;
        r.available = available;
        r.available_len = available_len;
        r.terminator = terminator;
        r.encode(writer);
    }
};

} /* namespace clipboard */
} /* namespace kitty */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_KITTY_CLIPBOARD_RESPONSE_HPP */
