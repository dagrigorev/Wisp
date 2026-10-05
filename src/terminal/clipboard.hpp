/* Transliterated from Ghostty src/terminal/clipboard.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Wisp, differences in shape rather than behavior:
 *   - `[]const u8` is osc::ZStr (pointer plus length); `[]const T` for other
 *     T is a pointer plus a `_len` field.
 *   - `union(enum)` is a tag enum plus a payload struct.
 *   - `*std.Io.Writer` is `std::string *`, as elsewhere in the port.
 */

#pragma once
#ifndef WISP_TERMINAL_CLIPBOARD_HPP
#define WISP_TERMINAL_CLIPBOARD_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

#include "osc.hpp"

namespace wisp {
namespace terminal {
namespace clipboard {

typedef ::wisp::terminal::osc::ZStr ZStr;

/* The clipboard destination for a write.
 * Wisp: non-exhaustive upstream; the integer value is what crosses the
 * boundary, so callers check before converting. */
enum class Location : int {
    standard = 0,
    selection = 1,
    primary = 2,
};

/* MIME types that name plain text across the platforms terminals run
 * on. We accept the union everywhere since serving text under any of these
 * names is harmless. */
inline bool isTextMime(ZStr mime) {
    static const char *const names[] = {
        "text/plain",
        "text/plain;charset=utf-8",
        "UTF8_STRING",
        "TEXT",
        "STRING",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (mime.eql(names[i])) return true;
    }
    return false;
}

/* A single representation of clipboard data.
 *
 * The MIME type and data are borrowed and only valid for the duration of a
 * clipboard write callback. Data is binary-safe. */
struct Content {
    ZStr mime;
    ZStr data;

    Content() : mime(), data() {}
    Content(ZStr m, ZStr d) : mime(m), data(d) {}
};

/* Requests content of a specific mime-type. For now this is used for
 * on-demand clipboard access since content can be large (particularly
 * non-text content), but it is generic so that this could handle other
 * mime-typed sources in the future like maybe drag-and-drop.
 *
 * C: GhosttyMimeReader */
struct MimeReader {
    /* The data could not be read.
     * Wisp: Error is the error set flattened to an enum; WriteFailed is
     * upstream's std.Io.Writer.Error propagated through. */
    enum class Error : uint8_t {
        none,
        ReadFailed,
        WriteFailed,
    };

    /* Passed through to `read_fn`. */
    void *ctx; /* = null */

    /* Write all the data of the representation named by `mime` to
     * `sink`, in as many writes as is convenient. The mime and sink
     * are borrowed, only valid for the duration of the call, and
     * nothing written to the sink is retained, so the data may be
     * borrowed from anywhere. Return error.ReadFailed if the data
     * can't be produced and propagate error.WriteFailed from the
     * sink. */
    Error (*read_fn)(void *ctx, ZStr mime, std::string *sink);

    MimeReader() : ctx(nullptr), read_fn(nullptr) {}

    Error read(ZStr mime, std::string *sink) const { return read_fn(ctx, mime, sink); }
};

/* A request from the running program to write a clipboard.
 *
 * Writes are synchronous: the effect callback must answer through `reply`
 * before it returns, and the request (including its reply context) is
 * invalid afterwards. An embedder that needs user consent must block until
 * it has an answer; the VT stream waits with it. Protocols without a write
 * acknowledgement (OSC 52, OSC 1337 Copy) discard the reply.
 *
 * Contents are borrowed and only valid for the duration of the callback.
 * An empty contents slice clears the destination. */
struct Write {
    /* The status of a clipboard write reply.
     * Wisp: non-exhaustive upstream. */
    enum class Status : int {
        success = 0,
        denied = 1,
        unsupported = 2,
        busy = 3,
        invalid_data = 4,
        io_error = 5,
    };

    /* The reply to a clipboard write. */
    struct Result {
        enum class Tag : uint8_t {
            /* The write was denied by policy or the user. */
            denied,

            /* The embedder cannot write this clipboard. */
            unsupported,

            /* The clipboard is temporarily unavailable. */
            busy,

            /* One or more representations contain invalid data. */
            invalid_data,

            /* Writing the clipboard failed. */
            io_error,

            /* The write succeeded. */
            success,
        };

        struct Success {
            /* Record a session grant so future requests from the same
             * program skip the permission prompt. Only honored when the
             * request set `can_remember`. */
            bool remember; /* = false */

            Success() : remember(false) {}
        };

        Tag tag;
        Success success;

        Result() : tag(Tag::denied), success() {}

        static Result make(Tag t) {
            Result r;
            r.tag = t;
            return r;
        }
        static Result makeSuccess(bool remember) {
            Result r;
            r.tag = Tag::success;
            r.success.remember = remember;
            return r;
        }
    };

    Location location;
    const Content *contents;
    size_t contents_len;

    /* Name of the writing program for permission prompts, if the
     * protocol carries one. Empty otherwise. */
    ZStr name;

    /* True if the terminal already holds a session grant for this
     * request (kitty clipboard protocol passwords). The embedder should
     * skip any permission prompt and perform the write. */
    bool granted;

    /* True if the program supplied a session password, so the embedder
     * may offer to remember the user's decision via
     * Result.Success.remember. When false, remember is ignored. */
    bool can_remember;

    /* Terminal-owned reply state, only valid during the callback. */
    void *reply_ctx;
    void (*reply_fn)(void *, Result);

    Write()
        : location(Location::standard), contents(nullptr), contents_len(0), name(), granted(false),
          can_remember(false), reply_ctx(nullptr), reply_fn(nullptr) {}

    /* Answer the write. May be called at most once; later calls are
     * ignored. */
    void reply(Result result) const { reply_fn(reply_ctx, result); }
};

/* A request from the running program to read a clipboard.
 *
 * Reads are synchronous: the effect callback must answer through `reply`
 * before it returns, and the request (including its reply context) is
 * invalid afterwards. An embedder that needs user consent must block until
 * it has an answer; the VT stream waits with it. */
struct Read {
    /* The status of a clipboard read reply.
     * Wisp: non-exhaustive upstream. */
    enum class Status : int {
        success = 0,
        denied = 1,
        unsupported = 2,
        busy = 3,
        io_error = 4,
    };

    /* The reply to a clipboard read. */
    struct Result {
        enum class Tag : uint8_t {
            /* The read was denied by policy or the user. */
            denied,

            /* The embedder cannot read this clipboard. */
            unsupported,

            /* The clipboard is temporarily unavailable. */
            busy,

            /* Reading the clipboard failed. */
            io_error,

            /* The read succeeded. All memory is borrowed for the duration
             * of the reply call. */
            success,
        };

        struct Success {
            /* Representations of the clipboard contents, one per
             * requested MIME type the clipboard has. Protocols that
             * carry a single text value use the first entry with a
             * text MIME type (see isTextMime). */
            const Content *contents; /* = &.{} */
            size_t contents_len;

            /* All MIME types available on the clipboard. Only used when
             * the request set `list`. */
            const ZStr *available; /* = &.{} */
            size_t available_len;

            /* Record a session grant so future requests from the same
             * program skip the permission prompt. Only honored when the
             * request set `can_remember`. */
            bool remember; /* = false */

            Success()
                : contents(nullptr), contents_len(0), available(nullptr), available_len(0), remember(false) {}
        };

        Tag tag;
        Success success;

        Result() : tag(Tag::denied), success() {}

        static Result make(Tag t) {
            Result r;
            r.tag = t;
            return r;
        }
        static Result makeSuccess(const Success &s) {
            Result r;
            r.tag = Tag::success;
            r.success = s;
            return r;
        }
    };

    Location location;

    /* The MIME types the program wants, in order of preference. The
     * reply should carry every requested representation the clipboard
     * has; unrequested ones are ignored. Protocols that only carry text
     * (OSC 52) request "text/plain". */
    const ZStr *mimes;
    size_t mimes_len;

    /* The program also wants the list of MIME types available on the
     * clipboard, delivered as Result.Success.available. */
    bool list;

    /* Name of the requesting program for permission prompts, if the
     * protocol carries one. Empty otherwise. */
    ZStr name;

    /* True if the terminal already holds a session grant for this
     * request (kitty clipboard protocol passwords). The embedder should
     * skip any permission prompt and serve the read.
     *
     * Always false when mimes is empty: such a request is served
     * without a prompt (kitty's targets-listing exemption), so the
     * terminal never consults grants for it and a one-time password
     * is preserved for the follow-up data read. */
    bool granted;

    /* True if the program supplied a session password, so the embedder
     * may offer to remember the user's decision via
     * Result.Success.remember. When false, remember is ignored. */
    bool can_remember;

    /* Terminal-owned reply state, only valid during the callback.
     *
     * The result is delivered through a call rather than returned so
     * the terminal consumes it while the embedder's memory is still
     * alive; a returned slice would have to outlive the callback. */
    void *reply_ctx;
    void (*reply_fn)(void *, Result);

    Read()
        : location(Location::standard), mimes(nullptr), mimes_len(0), list(false), name(), granted(false),
          can_remember(false), reply_ctx(nullptr), reply_fn(nullptr) {}

    /* Answer the read. May be called at most once; later calls are
     * ignored. Result memory is borrowed only for the duration of this
     * call. */
    void reply(Result result) const { reply_fn(reply_ctx, result); }
};

} /* namespace clipboard */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_CLIPBOARD_HPP */
