/* Transliterated from Ghostty src/terminal/paste.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Pasting into a terminal.
 *
 * This is the single place that turns "the user pasted" into bytes for
 * the pty, applying the terminal's current state:
 *
 *   * Mode 5522 (Kitty clipboard protocol paste events) set, a
 *     user-initiated clipboard paste, and the embedder able to serve
 *     the program's follow-up clipboard read: send a paste event
 *     listing the clipboard's MIME types with a fresh one-time password
 *     and record a one-time read grant for it. No data is read.
 *   * Otherwise: write the first text representation, with unsafe bytes
 *     replaced (xterm behavior), framed per mode 2004 (bracketed paste)
 *     or with newlines converted to carriage returns if not.
 *
 * The precedence (5522 event, else 2004 framing, else plain) and the
 * safety rule live only here so every embedder of the terminal shares
 * one implementation.
 *
 * Wisp, differences in shape rather than behavior:
 *   - `union(enum)` is a tag enum plus one field per payload; `?T` is a
 *     `has_x` flag plus the value.
 *   - `*std.Io.Writer` is `std::string *`, so the writer never fails and
 *     the reader's WriteFailed cannot occur.
 *   - The error union return is an Error enum plus the bool result
 *     through an out parameter.
 *   - There is no std.Io, so the entropy source is sys.randomSecure.
 */

#pragma once
#ifndef WISP_TERMINAL_PASTE_HPP
#define WISP_TERMINAL_PASTE_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

#include "../input/paste.hpp"
#include "../vt/terminal.hpp"
#include "../zigstd/allocator.hpp"
#include "clipboard.hpp"
#include "kitty/clipboard_grants.hpp"
#include "kitty/clipboard_response.hpp"

namespace wisp {
namespace terminal {
namespace paste {

typedef ::wisp::terminal::osc::ZStr ZStr;
namespace clip = ::wisp::terminal::clipboard;
namespace kitty_clipboard = ::wisp::terminal::kitty::clipboard;
namespace input_paste = ::wisp::input::paste;

/* Why a paste happened. Only clipboard pastes may become paste
 * events, which is why only they carry a location: it's meaningless
 * for text insertion.
 *
 * C: GhosttyPasteSource, flattened next to the location since C has
 * no tagged unions. */
struct Source {
    enum class Tag : uint8_t {
        /* The user pasted from a clipboard: keybind, menu, middle click.
         * The payload is the clipboard the contents came from. */
        clipboard,

        /* Text inserted some other way: IME commit, drag and drop,
         * scripted input. */
        text,
    };

    Tag tag;
    clip::Location clipboard;

    Source() : tag(Tag::clipboard), clipboard(clip::Location::standard) {}

    static Source makeClipboard(clip::Location loc) {
        Source s;
        s.tag = Tag::clipboard;
        s.clipboard = loc;
        return s;
    }
    static Source makeText() {
        Source s;
        s.tag = Tag::text;
        return s;
    }
};

/* The representations available for a paste, in the embedder's
 * preferred order. */
struct Contents {
    enum class Tag : uint8_t {
        /* Every representation already in memory. For text the embedder
         * holds anyway (an IME commit, dropped text) or small clipboards. */
        memory,

        /* Representations read on demand, so nothing is loaded that isn't
         * pasted. This is the form for a real clipboard, whose non-text
         * items may be huge. */
        reader,
    };

    /* The on-demand form: the MIME types available plus the reader
     * that produces the data of any one of them. */
    struct Reader {
        /* The MIME types available, in preferred order. */
        const ZStr *mimes;
        size_t mimes_len;

        /* Produces the data of any entry of `mimes`, which is passed
         * through to it as is. A paste reads at most once: the text
         * representation being pasted, never anything else and never
         * anything for a paste event. There is no requirement across
         * paste calls, so a source that changes between an unsafe
         * refusal and the embedder's confirmed retry simply pastes
         * its current contents. */
        clip::MimeReader read;

        Reader() : mimes(nullptr), mimes_len(0), read() {}
    };

    Tag tag;
    const clip::Content *memory;
    size_t memory_len;
    Reader reader;

    Contents() : tag(Tag::memory), memory(nullptr), memory_len(0), reader() {}

    /* The number of representations. */
    size_t len() const {
        switch (tag) {
        case Tag::memory: return memory_len;
        case Tag::reader: return reader.mimes_len;
        }
        return 0;
    }

    /* The MIME type of representation `index`. */
    ZStr mime(size_t index) const {
        switch (tag) {
        case Tag::memory: return memory[index].mime;
        case Tag::reader: return reader.mimes[index];
        }
        return ZStr();
    }
};

/* A paste of clipboard contents into the terminal. What actually gets
 * written depends on terminal state; see `paste`. */
struct Request {
    /* Why this paste happened, and for a clipboard paste, from which
     * clipboard. Only a user-initiated clipboard paste may become a
     * paste event; text insertion always writes text. */
    Source source; /* = .{ .clipboard = .standard } */

    /* The representations available. Borrowed only during the
     * duration of the paste function call. */
    Contents contents;

    /* Write data that could inject commands (see `paste`). The usual
     * flow is to call with false, confirm with the user on
     * error.UnsafePaste, and call again with true. */
    bool allow_unsafe; /* = false */

    Request() : source(), contents(), allow_unsafe(false) {}
};

/* What a caller supplies to `paste`: the terminal state the decision
 * depends on, the session state an event records into, and the sink. */
struct Context {
    struct KittyClipboard {
        /* Session grants. A paste event records its one-time password
         * here so the program's follow-up read is served without a
         * prompt. */
        kitty_clipboard::Grants *grants;

        KittyClipboard() : grants(nullptr) {}
    };

    /* The terminal whose modes decide the encoding. */
    const vt::Terminal *terminal;

    /* Allocator for transient state: the buffered read and a paste
     * event's grant. Must be the allocator `kitty_clipboard.grants`
     * is freed with. */
    zigstd::Allocator alloc;

    /* Receives the encoded text in chunks, or the event. */
    std::string *writer;

    /* Kitty clipboard protocol session state, or null if the embedder
     * does not serve Kitty clipboard reads (`clipboard.Read`). A
     * paste event is that protocol: it is only useful if the
     * program's follow-up read can be answered, since otherwise the
     * read would be refused and the user's paste would vanish. With
     * null, `paste` always writes text. */
    bool has_kitty_clipboard;
    KittyClipboard kitty_clipboard_;

    Context()
        : terminal(nullptr), alloc(zigstd::c_allocator()), writer(nullptr), has_kitty_clipboard(false),
          kitty_clipboard_() {}
};

enum class Error : uint8_t {
    none,
    OutOfMemory,
    EntropyUnavailable,
    ReadFailed,

    /* The data could inject commands and allow_unsafe was false. */
    UnsafePaste,
};

inline Error pasteKittyEvent(const Context &ctx, const Context::KittyClipboard &kitty, const Request &req) {
    uint8_t otp[kitty_clipboard::otp_len];
    if (kitty_clipboard::generateOtp(otp) != sys::RandomSecureError::none) return Error::EntropyUnavailable;

    /* Every representation is listed, never read. The listing is
     * bounded; a clipboard with more types than that is not a thing. */
    ZStr mimes_buf[kitty_clipboard::max_listing_mimes];
    const size_t buf_len = kitty_clipboard::max_listing_mimes;
    const size_t contents_len = req.contents.len();
    const size_t mimes_len = contents_len < buf_len ? contents_len : buf_len;
    for (size_t i = 0; i < mimes_len; i++) mimes_buf[i] = req.contents.mime(i);

    /* The grant is recorded before the event can reach the program,
     * and revoked if the event fails to be written so a failure never
     * leaves a grant for an event that was never sent. Using a
     * one-time grant consumes it, which is the revocation. */
    if (!kitty.grants->grant(ctx.alloc, otp, kitty_clipboard::otp_len,
                             kitty_clipboard::Grants::Direction::read, true))
        return Error::OutOfMemory;

    kitty_clipboard::PasteEvent event;
    /* The protocol only distinguishes the clipboard from the
     * primary selection, so both non-standard locations report as
     * primary. Only a clipboard paste gets here; see `paste`. */
    event.primary = req.source.clipboard != clip::Location::standard;
    event.pw = ZStr((const char *)otp, kitty_clipboard::otp_len);
    event.available = mimes_buf;
    event.available_len = mimes_len;
    event.encode(ctx.writer);
    return Error::none;
}

/* Paste into the terminal, applying the terminal's current state as
 * described in the module docs. Returns true if anything was written
 * to `ctx.writer`: the encoded text or a paste event. False means
 * there was nothing to paste (no non-empty text representation).
 *
 * The safety rule for a text paste (`input.paste.isSafeWith`): a
 * bracketed paste is unsafe only if it contains the bracket terminator
 * (CSI 201~); an unbracketed paste is unsafe if it contains a newline
 * or the terminator. Embedders wanting a stricter rule check
 * `input.paste.isSafe` themselves before calling. A paste event never
 * puts the data on the input stream, so the rule doesn't apply to it.
 *
 * The contents are read at most once per call and buffered whole, so
 * the source needs no stability across reads. Nothing reaches the
 * writer until the read completed and the text passed the rule:
 * every error writes nothing, and a failed event records no grant. */
inline Error paste(const Context &ctx, const Request &req, bool *out) {
    *out = false;

    /* If the source is a clipboard and mode 5522 is enabled and
     * the caller can handle kitty events, then do a kitty event. */
    if (req.source.tag == Source::Tag::clipboard &&
        ctx.terminal->modes.get(terminal::modes::Mode::kitty_paste_events)) {
        if (ctx.has_kitty_clipboard) {
            const Error err = pasteKittyEvent(ctx, ctx.kitty_clipboard_, req);
            if (err != Error::none) return err;
            *out = true;
            return Error::none;
        }
    }

    /* For non-Kitty paste events we can only accept text content. */
    size_t index = 0;
    {
        bool found = false;
        const size_t n = req.contents.len();
        for (size_t i = 0; i < n; i++) {
            if (clip::isTextMime(req.contents.mime(i))) {
                index = i;
                found = true;
                break;
            }
        }
        if (!found) return Error::none;
    }

    const input_paste::Options opts = input_paste::Options::fromTerminal(ctx.terminal);

    /* In-memory contents are used directly to avoid a double copy but
     * reader-based contents are read into memory so we can do the unsafe
     * scan. */
    std::string aw;
    ZStr text;
    switch (req.contents.tag) {
    case Contents::Tag::memory: text = req.contents.memory[index].data; break;
    case Contents::Tag::reader: {
        const clip::MimeReader::Error err = req.contents.reader.read.read(req.contents.reader.mimes[index], &aw);
        switch (err) {
        case clip::MimeReader::Error::none: break;
        /* An allocating writer only fails to allocate. */
        case clip::MimeReader::Error::WriteFailed: return Error::OutOfMemory;
        case clip::MimeReader::Error::ReadFailed: return Error::ReadFailed;
        }
        text = ZStr(aw.data(), aw.size());
        break;
    }
    }
    if (text.len == 0) return Error::none;
    if (!req.allow_unsafe && !input_paste::isSafeWith((const uint8_t *)text.ptr, text.len, opts))
        return Error::UnsafePaste;

    /* The text is copied exactly once per chunk, into the writer,
     * where the encoder strips and converts it in place. */
    input_paste::encodeWriter(ctx.writer, (const uint8_t *)text.ptr, text.len, opts);
    *out = true;
    return Error::none;
}

} /* namespace paste */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_PASTE_HPP */
