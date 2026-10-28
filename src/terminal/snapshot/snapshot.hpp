/* Transliterated from Ghostty src/terminal/snapshot/snapshot.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Complete terminal snapshot encoding and restoration.
 *
 * Wisp, differences in shape rather than behavior:
 *   - Every component error set flattens into one enum, `Error`.
 *   - `Decoder` keeps its source reader between calls, so it is a template
 *     over the source type where upstream stores an interface pointer.
 *     `decode` and `decodeExact` are templates for the same reason.
 *   - `?Terminal` is `has_terminal` plus `terminal`; `?Progress` is a
 *     `bool *has` out-param; `std.EnumMap(Key, u64)` is an array plus
 *     presence flags.
 *   - `std.Io` is not threaded through: the native Terminal takes none.
 *   - The 512-byte stackFallback allocator for the record scratch buffer is
 *     not needed: record::Writer buffers into a std::string.
 */
#pragma once
#ifndef WISP_TERMINAL_SNAPSHOT_SNAPSHOT_HPP
#define WISP_TERMINAL_SNAPSHOT_SNAPSHOT_HPP

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <string>

#include "../../vt/page.hpp"
#include "../../vt/page_list.hpp"
#include "../../vt/screen.hpp"
#include "../../vt/screen_set.hpp"
#include "../../vt/terminal.hpp"
#include "../../zigstd/allocator.hpp"
#include "../../zigstd/reader.hpp"
#include "checkpoint.hpp"
#include "continuation.hpp"
#include "envelope.hpp"
#include "history.hpp"
#include "page.hpp"
#include "record.hpp"
#include "screen.hpp"
#include "terminal.hpp"

namespace wisp {
namespace terminal {
namespace snapshot {

typedef ::wisp::vt::Terminal Terminal;
typedef ::wisp::vt::Screen TerminalScreen;
typedef ::wisp::vt::PageList TerminalPageList;
typedef ::wisp::vt::ScreenSet::Key TerminalScreenKey;
typedef ::wisp::zigstd::Allocator Allocator;
typedef ::wisp::zigstd::ReadError ReadError;

/* Re-export continuation to make it a bit more ergonomic to reference. */
typedef continuation::Value Continuation;

/* Wisp: the union of every component error set this file can return. */
enum class Error : uint8_t {
    none = 0,

    /* std.Io.Reader.Error / std.Io.Writer.Error / Allocator.Error */
    EndOfStream,
    ReadFailed,
    WriteFailed,
    OutOfMemory,

    /* envelope.DecodeError */
    InvalidMagic,
    UnsupportedVersion,

    /* record errors */
    PayloadTooLarge,
    InvalidTag,
    InvalidChecksum,
    PayloadNotExhausted,
    UnexpectedRecordTag,

    /* terminal.EncodeError / terminal.DecodeError */
    InvalidDimensions,
    InvalidScrollingRegion,
    InvalidScreenCount,
    InvalidActiveScreenKey,
    InvalidPreviousCodepoint,
    InvalidScrollbackLimit,
    ScrollbackLimitOverflow,
    InvalidTabStops,
    InvalidPalette,
    StringTooLong,

    /* screen.EncodeError / screen.DecodeError */
    InvalidCursorFlags,
    InvalidCharsetState,
    InvalidKittyKeyboardIndex,
    InvalidKittyKeyboardFlags,
    InvalidSavedCursorFlags,
    InvalidUri,
    InvalidExplicitId,
    PageCountOverflow,
    InvalidKey,
    InvalidWideCell,
    TooManyGraphemes,
    InvalidRowCellCount,
    InvalidKind,
    InvalidDestinationCapacity,
    IntegrityViolation,
    InvalidPageDimensions,
    NoPages,
    InsufficientRows,
    OutOfSpace,
    InvalidPageCount,

    /* history errors */
    RowCountOverflow,
    PageSizeOverflow,
    MaxSizeExceeded,
    MaxLinesExceeded,
    ExistingHistory,

    /* continuation.EncodeError / continuation.DecodeError */
    Overflow,
    NoPendingState,
    NonCanonicalContinuation,
    ReplayWouldCommit,
    ContinuationLimitExceeded,

    /* A SCREEN or HISTORY names a key not declared by TERMINAL. */
    UnexpectedScreenKey,

    /* More than one SCREEN names the same key. */
    DuplicateScreen,

    /* `next` was called before `ready` completed successfully. */
    DecoderNotReady,

    /* A prior `next` call failed and invalidated the stream position. */
    DecoderFailed,

    /* A HISTORY names a key not declared by TERMINAL. */
    UnexpectedHistoryKey,

    /* More than one HISTORY names the same key. */
    DuplicateHistory,

    /* FINISH was followed by additional bytes. */
    TrailingData,
};

#define WISP_SNAPSHOT_MAP(name) \
    case Src::name: return Error::name;

inline Error fromTerminal(terminal_record::Error e) {
    typedef terminal_record::Error Src;
    switch (e) {
        case Src::none: return Error::none;
        WISP_SNAPSHOT_MAP(EndOfStream)
        WISP_SNAPSHOT_MAP(ReadFailed)
        WISP_SNAPSHOT_MAP(WriteFailed)
        WISP_SNAPSHOT_MAP(OutOfMemory)
        WISP_SNAPSHOT_MAP(InvalidDimensions)
        WISP_SNAPSHOT_MAP(InvalidScrollingRegion)
        WISP_SNAPSHOT_MAP(InvalidScreenCount)
        WISP_SNAPSHOT_MAP(InvalidActiveScreenKey)
        WISP_SNAPSHOT_MAP(InvalidPreviousCodepoint)
        WISP_SNAPSHOT_MAP(InvalidScrollbackLimit)
        WISP_SNAPSHOT_MAP(ScrollbackLimitOverflow)
        WISP_SNAPSHOT_MAP(InvalidTabStops)
        WISP_SNAPSHOT_MAP(InvalidPalette)
        WISP_SNAPSHOT_MAP(StringTooLong)
        WISP_SNAPSHOT_MAP(PayloadTooLarge)
        WISP_SNAPSHOT_MAP(InvalidTag)
        WISP_SNAPSHOT_MAP(InvalidChecksum)
        WISP_SNAPSHOT_MAP(PayloadNotExhausted)
        WISP_SNAPSHOT_MAP(UnexpectedRecordTag)
    }
    return Error::EndOfStream;
}

inline Error fromScreen(screen::Error e) {
    typedef screen::Error Src;
    switch (e) {
        case Src::none: return Error::none;
        WISP_SNAPSHOT_MAP(EndOfStream)
        WISP_SNAPSHOT_MAP(ReadFailed)
        WISP_SNAPSHOT_MAP(WriteFailed)
        WISP_SNAPSHOT_MAP(InvalidCursorFlags)
        WISP_SNAPSHOT_MAP(InvalidCharsetState)
        WISP_SNAPSHOT_MAP(InvalidKittyKeyboardIndex)
        WISP_SNAPSHOT_MAP(InvalidKittyKeyboardFlags)
        WISP_SNAPSHOT_MAP(InvalidSavedCursorFlags)
        WISP_SNAPSHOT_MAP(InvalidUri)
        WISP_SNAPSHOT_MAP(InvalidExplicitId)
        WISP_SNAPSHOT_MAP(StringTooLong)
        WISP_SNAPSHOT_MAP(PageCountOverflow)
        WISP_SNAPSHOT_MAP(InvalidKey)
        WISP_SNAPSHOT_MAP(InvalidWideCell)
        WISP_SNAPSHOT_MAP(TooManyGraphemes)
        WISP_SNAPSHOT_MAP(PayloadTooLarge)
        WISP_SNAPSHOT_MAP(InvalidDimensions)
        WISP_SNAPSHOT_MAP(InvalidRowCellCount)
        WISP_SNAPSHOT_MAP(InvalidKind)
        WISP_SNAPSHOT_MAP(OutOfMemory)
        WISP_SNAPSHOT_MAP(InvalidDestinationCapacity)
        WISP_SNAPSHOT_MAP(InvalidTag)
        WISP_SNAPSHOT_MAP(InvalidChecksum)
        WISP_SNAPSHOT_MAP(PayloadNotExhausted)
        WISP_SNAPSHOT_MAP(IntegrityViolation)
        WISP_SNAPSHOT_MAP(InvalidPageDimensions)
        WISP_SNAPSHOT_MAP(NoPages)
        WISP_SNAPSHOT_MAP(InsufficientRows)
        WISP_SNAPSHOT_MAP(OutOfSpace)
        WISP_SNAPSHOT_MAP(UnexpectedRecordTag)
        WISP_SNAPSHOT_MAP(InvalidPageCount)
    }
    return Error::EndOfStream;
}

inline Error fromHistory(history::Error e) {
    typedef history::Error Src;
    switch (e) {
        case Src::none: return Error::none;
        WISP_SNAPSHOT_MAP(EndOfStream)
        WISP_SNAPSHOT_MAP(ReadFailed)
        WISP_SNAPSHOT_MAP(WriteFailed)
        WISP_SNAPSHOT_MAP(InvalidKey)
        WISP_SNAPSHOT_MAP(PageCountOverflow)
        WISP_SNAPSHOT_MAP(InvalidUri)
        WISP_SNAPSHOT_MAP(InvalidExplicitId)
        WISP_SNAPSHOT_MAP(StringTooLong)
        WISP_SNAPSHOT_MAP(InvalidWideCell)
        WISP_SNAPSHOT_MAP(TooManyGraphemes)
        WISP_SNAPSHOT_MAP(PayloadTooLarge)
        WISP_SNAPSHOT_MAP(InvalidDimensions)
        WISP_SNAPSHOT_MAP(InvalidRowCellCount)
        WISP_SNAPSHOT_MAP(InvalidKind)
        WISP_SNAPSHOT_MAP(OutOfMemory)
        WISP_SNAPSHOT_MAP(InvalidDestinationCapacity)
        WISP_SNAPSHOT_MAP(InvalidTag)
        WISP_SNAPSHOT_MAP(InvalidChecksum)
        WISP_SNAPSHOT_MAP(PayloadNotExhausted)
        WISP_SNAPSHOT_MAP(IntegrityViolation)
        WISP_SNAPSHOT_MAP(UnexpectedRecordTag)
        WISP_SNAPSHOT_MAP(InvalidPageDimensions)
        WISP_SNAPSHOT_MAP(RowCountOverflow)
        WISP_SNAPSHOT_MAP(PageSizeOverflow)
        WISP_SNAPSHOT_MAP(MaxSizeExceeded)
        WISP_SNAPSHOT_MAP(MaxLinesExceeded)
        WISP_SNAPSHOT_MAP(ExistingHistory)
        WISP_SNAPSHOT_MAP(UnexpectedScreenKey)
    }
    return Error::EndOfStream;
}

inline Error fromPage(page::Error e) {
    typedef page::Error Src;
    switch (e) {
        case Src::none: return Error::none;
        WISP_SNAPSHOT_MAP(EndOfStream)
        WISP_SNAPSHOT_MAP(ReadFailed)
        WISP_SNAPSHOT_MAP(WriteFailed)
        WISP_SNAPSHOT_MAP(InvalidUri)
        WISP_SNAPSHOT_MAP(InvalidExplicitId)
        WISP_SNAPSHOT_MAP(StringTooLong)
        WISP_SNAPSHOT_MAP(InvalidWideCell)
        WISP_SNAPSHOT_MAP(TooManyGraphemes)
        WISP_SNAPSHOT_MAP(PayloadTooLarge)
        WISP_SNAPSHOT_MAP(InvalidDimensions)
        WISP_SNAPSHOT_MAP(InvalidRowCellCount)
        WISP_SNAPSHOT_MAP(InvalidKind)
        WISP_SNAPSHOT_MAP(OutOfMemory)
        WISP_SNAPSHOT_MAP(InvalidDestinationCapacity)
        WISP_SNAPSHOT_MAP(InvalidTag)
        WISP_SNAPSHOT_MAP(InvalidChecksum)
        WISP_SNAPSHOT_MAP(PayloadNotExhausted)
        WISP_SNAPSHOT_MAP(IntegrityViolation)
        WISP_SNAPSHOT_MAP(UnexpectedRecordTag)
    }
    return Error::EndOfStream;
}

inline Error fromContinuationEncode(continuation::EncodeError e) {
    typedef continuation::EncodeError Src;
    switch (e) {
        case Src::none: return Error::none;
        WISP_SNAPSHOT_MAP(Overflow)
        WISP_SNAPSHOT_MAP(NoPendingState)
        WISP_SNAPSHOT_MAP(NonCanonicalContinuation)
        WISP_SNAPSHOT_MAP(ReplayWouldCommit)
        WISP_SNAPSHOT_MAP(PayloadTooLarge)
        WISP_SNAPSHOT_MAP(WriteFailed)
    }
    return Error::WriteFailed;
}

inline Error fromContinuationValidate(continuation::ValidateError e) {
    typedef continuation::ValidateError Src;
    switch (e) {
        case Src::none: return Error::none;
        WISP_SNAPSHOT_MAP(Overflow)
        WISP_SNAPSHOT_MAP(NoPendingState)
        WISP_SNAPSHOT_MAP(NonCanonicalContinuation)
        WISP_SNAPSHOT_MAP(ReplayWouldCommit)
    }
    return Error::NoPendingState;
}

inline Error fromContinuationDecode(continuation::DecodeError e) {
    typedef continuation::DecodeError Src;
    switch (e) {
        case Src::none: return Error::none;
        case Src::Allocator: return Error::OutOfMemory;
        WISP_SNAPSHOT_MAP(NoPendingState)
        WISP_SNAPSHOT_MAP(NonCanonicalContinuation)
        WISP_SNAPSHOT_MAP(ReplayWouldCommit)
        WISP_SNAPSHOT_MAP(EndOfStream)
        WISP_SNAPSHOT_MAP(InvalidTag)
        WISP_SNAPSHOT_MAP(ContinuationLimitExceeded)
        WISP_SNAPSHOT_MAP(InvalidChecksum)
        WISP_SNAPSHOT_MAP(PayloadNotExhausted)
        WISP_SNAPSHOT_MAP(UnexpectedRecordTag)
    }
    return Error::EndOfStream;
}

inline Error fromCheckpointEncode(checkpoint::EncodeError e) {
    typedef checkpoint::EncodeError Src;
    switch (e) {
        case Src::none: return Error::none;
        WISP_SNAPSHOT_MAP(WriteFailed)
        WISP_SNAPSHOT_MAP(PayloadTooLarge)
    }
    return Error::WriteFailed;
}

inline Error fromCheckpointDecode(checkpoint::DecodeError e) {
    typedef checkpoint::DecodeError Src;
    switch (e) {
        case Src::none: return Error::none;
        WISP_SNAPSHOT_MAP(EndOfStream)
        WISP_SNAPSHOT_MAP(InvalidTag)
        WISP_SNAPSHOT_MAP(UnexpectedRecordTag)
        WISP_SNAPSHOT_MAP(InvalidChecksum)
        WISP_SNAPSHOT_MAP(PayloadNotExhausted)
    }
    return Error::EndOfStream;
}

inline Error fromEnvelope(envelope::Error e) {
    typedef envelope::Error Src;
    switch (e) {
        case Src::none: return Error::none;
        WISP_SNAPSHOT_MAP(EndOfStream)
        WISP_SNAPSHOT_MAP(InvalidMagic)
        WISP_SNAPSHOT_MAP(UnsupportedVersion)
    }
    return Error::EndOfStream;
}

#undef WISP_SNAPSHOT_MAP

struct EncodeOptions {
    Continuation continuation;
};

/* Encode one complete terminal snapshot.
 *
 * Encoding starts at the destination's current position. Only one record
 * payload is buffered at a time; completed records stream immediately. On
 * failure, the destination may contain a snapshot prefix without its required
 * FINISH marker, and an output failure may have written part of a record. */
inline Error encode(std::string *destination, const Terminal *t, const EncodeOptions &options) {
    /* Continuation errors must not emit even the snapshot envelope. */
    Error err = fromContinuationValidate(continuation::validate(options.continuation));
    if (err != Error::none) return err;

    record::Writer stream;
    stream.init(destination);

    /* 1. Envelope */
    envelope::encode(stream.writer());

    /* 2. Terminal */
    err = fromTerminal(terminal_record::encode(t, &stream));
    if (err != Error::none) return err;

    /* 3. Primary and alt screen */
    err = fromScreen(screen::encode(t->screens.get(TerminalScreenKey::primary), TerminalScreenKey::primary, &stream));
    if (err != Error::none) return err;
    if (const TerminalScreen *alternate = t->screens.get(TerminalScreenKey::alternate)) {
        err = fromScreen(screen::encode(alternate, TerminalScreenKey::alternate, &stream));
        if (err != Error::none) return err;
    }

    /* 4. Standard Stream continuation. */
    err = fromContinuationEncode(continuation::encode(options.continuation, &stream));
    if (err != Error::none) return err;

    /* 5. Ready marker. */
    err = fromCheckpointEncode(checkpoint::encode(checkpoint::Kind::ready, &stream));
    if (err != Error::none) return err;

    /* 6. History */
    err = fromHistory(
        history::encode(t->screens.get(TerminalScreenKey::primary), TerminalScreenKey::primary, &stream));
    if (err != Error::none) return err;
    if (const TerminalScreen *alternate = t->screens.get(TerminalScreenKey::alternate)) {
        err = fromHistory(history::encode(alternate, TerminalScreenKey::alternate, &stream));
        if (err != Error::none) return err;
    }

    /* 7. Finish */
    return fromCheckpointEncode(checkpoint::encode(checkpoint::Kind::finish, &stream));
}

struct DecodeOptions {
    /* Largest non-ground continuation the decoder may allocate and return.
     * Set this to zero when only ground-state snapshots are acceptable. */
    size_t max_continuation_bytes;
};

/* One decoded Terminal and the bytes needed to resume its Stream.
 *
 * `decode` returns this with all history present; `Decoder.ready` returns
 * it at READY, when the terminal is complete for interactive use but its
 * scrollback is still arriving through `Decoder.next`.
 *
 * A successful decode owns both values. Keep this result alive until the
 * Terminal's Stream has been restored, and always finish by calling `deinit`.
 * The usual restoration sequence is:
 *
 *   1. Inspect `continuation` and use its length to size continuation tracking.
 *   2. Call `toOwned` once and store the returned Terminal at its final address.
 *   3. Create the persistent, read-only standard TerminalStream for that
 *      address. If `continuation` contains bytes, feed them exactly once and
 *      verify that re-exporting the continuation returns the same bytes.
 *   4. Call `deinit` to release the decoded continuation. The transferred
 *      Terminal and its Stream remain owned by the caller.
 *   5. Process PTY bytes that came after the snapshot cut.
 *
 * Do not create a Stream against the address of `terminal` in this struct.
 * `toOwned` moves the Terminal, which would leave such a Stream pointing at
 * its old address. */
struct Decoded {
    /* Present until `toOwned` transfers the Terminal to the caller. Callers
     * may inspect it, but must transfer it before attaching a persistent
     * TerminalStream. */
    bool has_terminal = false;
    Terminal terminal;

    /* For decoded results, nonempty bytes are allocator-owned and remain
     * valid until `deinit`. Ground needs no replay. Bytes must be replayed
     * exactly once after the Terminal has reached its final address. */
    Continuation continuation;

    /* Advisory complete logical history extent declared by each restored
     * screen, including the resident overlap already carried by that
     * screen's own pages. A client may size scrollback UI from this at
     * READY, before HISTORY pages arrive; native row totals always remain
     * derived from the pages actually applied. */
    bool history_rows_present[::wisp::vt::ScreenSet::key_count] = {false, false};
    uint64_t history_rows[::wisp::vt::ScreenSet::key_count] = {0, 0};

    /* Destroy an untransferred Terminal and always free continuation bytes.
     * After `toOwned`, this leaves the caller-owned Terminal untouched. */
    void deinit(Allocator alloc) {
        if (has_terminal) terminal.deinit(alloc);
        has_terminal = false;
        continuation = Continuation::makeGround();
    }

    /* Transfer the Terminal while retaining the continuation for replay.
     *
     * This may be called exactly once. Store the returned value directly at
     * its final address before creating the TerminalStream that will replay
     * `continuation`. */
    Terminal toOwned() {
        assert(has_terminal);
        const Terminal result = terminal;
        has_terminal = false;
        return result;
    }
};

/* Incrementally restore one snapshot: renderable state first, then history.
 *
 * The snapshot record order makes a terminal renderable at the READY
 * marker, after only a small prefix of the stream. `Decoder` exposes
 * that boundary: `ready` returns a complete, integrity-checked `Decoded` while
 * scrollback is still in flight, and each `next` call then applies one
 * history PAGE record off the critical path. `snapshot.decode` is the
 * one-shot form and is implemented over this type.
 *
 * The terminal is live between `next` calls: the caller may render it and
 * even feed it PTY bytes that arrived after the snapshot cut. `next`
 * re-validates its destination on every call and degrades to discarding,
 * consuming and validating wire bytes without applying them, when live
 * changes have made a page inapplicable. See `next` for the drop rules.
 *
 * The decoder owns no allocations and needs no deinit. It retains the
 * source pointer given to `init`; the source must stay valid, and must not
 * be read elsewhere, until decoding ends. The decoder itself is movable
 * between calls. After any returned error both the decoder and the stream
 * position are invalid: abandon the decoder, and destroy an already
 * transferred Terminal or keep it with partial history, but do not resume. */
template <typename SourceT>
struct Decoder {
    static const size_t key_count = ::wisp::vt::ScreenSet::key_count;

    enum class State : uint8_t {
        /* `ready` has not been called. */
        start,

        /* READY validated; HISTORY sequences remain before FINISH. */
        history,

        /* FINISH validated. `next` returns null. */
        finished,

        /* A call returned an error; the stream position is unknown. */
        failed,
    };

    struct Sequence {
        TerminalScreenKey key;

        /* PAGE records not yet consumed from this sequence. */
        uint32_t remaining;

        /* False once any page in this sequence was dropped. Later pages
         * are older, and applying one above a gap would corrupt scrollback
         * order, so a single drop discards the remainder of the sequence. */
        bool apply;
    };

    struct History {
        /* The generation observed at READY for every screen the snapshot
         * declared. Presence is the declaration check for HISTORY routing;
         * the value detects screens removed or replaced by live terminal
         * use between calls. */
        bool generations_present[key_count];
        size_t generations[key_count];

        /* Keys whose HISTORY sequence was already consumed. */
        bool routed[key_count];

        /* HISTORY sequences that have not begun streaming yet. */
        size_t pending;

        /* The sequence currently streaming PAGE records. */
        bool has_current;
        Sequence current;

        /* Terminal width at READY. Encoded pages assume this width and are
         * dropped rather than mixed into a screen reflowed to another. */
        ::wisp::vt::size::CellCountInt cols;
    };

    SourceT *source;
    State state;
    History history_state;

    /* The decoder begins reading only when `ready` is called. */
    static Decoder init(SourceT *src) {
        Decoder d;
        d.source = src;
        d.state = State::start;
        return d;
    }

    /* Decode through the READY marker and return the renderable state.
     *
     * The result is transactional and complete for interactive use: the
     * Terminal, its continuation, and the advisory history extents are all
     * validated through READY. Scrollback history is not present yet and
     * arrives through `next`. Asserts this is the first call.
     *
     * Wisp: `*out` is written only on success. */
    Error ready(Allocator alloc, const DecodeOptions &options, Decoded *out) {
        assert(state == State::start);
        const Error err = readyInner(alloc, options, out);
        if (err != Error::none) state = State::failed;
        return err;
    }

    /* One history PAGE record consumed by `next`. */
    struct Progress {
        /* The screen whose HISTORY sequence contained the decoded page. */
        TerminalScreenKey key;

        /* Rows prepended above that screen's existing content, or zero
         * when the page was consumed and validated but dropped. */
        size_t rows;

        /* PAGE records still pending in the same HISTORY sequence. */
        uint32_t remaining;

        bool operator==(const Progress &o) const {
            return key == o.key && rows == o.rows && remaining == o.remaining;
        }
    };

    /* Decode one history PAGE record and prepend it to its screen in `t`.
     *
     * `t` must be the Terminal restored by this decoder's `ready` call,
     * wherever the caller now stores it. Returns null once FINISH has
     * validated, which completes the snapshot; the source is
     * left positioned exactly after it. Further calls return null.
     *
     * Wire validation stays strict: framing, CRCs, record order, routing,
     * and both marker records are always enforced. Application is
     * forgiving, because the terminal is live and may have changed since
     * READY. A page is consumed but dropped, reported as zero rows, when:
     *
     *   * its screen was removed, or removed and recreated, since READY
     *     (detected through ScreenSet generations);
     *   * the terminal was resized since READY, since encoded pages assume
     *     the encoded width;
     *   * prepending it would exceed the screen's scrollback limits, e.g.
     *     because post-cut output already consumed that budget, making
     *     immediate eviction the only alternative.
     *
     * Once a page in a sequence is dropped, the rest of that sequence is
     * discarded too: later pages are older, and applying one above a gap
     * would corrupt scrollback ordering. History for other screens is
     * unaffected.
     *
     * Wisp: `?Progress` is `*has` plus `*out`. */
    Error next(Allocator alloc, Terminal *t, Progress *out, bool *has) {
        *has = false;
        switch (state) {
            /* `ready` must succeed before history exists to decode, and a
             * failed decoder no longer knows its stream position. */
            case State::start: return Error::DecoderNotReady;
            case State::failed: return Error::DecoderFailed;
            case State::finished: return Error::none;
            case State::history: break;
        }
        const Error err = nextInner(alloc, t, out, has);
        if (err != Error::none) state = State::failed;
        return err;
    }

private:
    Error readyInner(Allocator alloc, const DecodeOptions &options, Decoded *out) {
        SourceT *reader = source;

        /* Read the envelope, which is currently just a verification step. */
        {
            envelope::Error eerr = envelope::Error::none;
            if (!envelope::decode(reader, &eerr)) return fromEnvelope(eerr);
        }

        /* TERMINAL establishes terminal-wide state and allocates empty
         * screen slots with their final routing. SCREEN values replace those
         * slots in place so ScreenSet pointers, including the active
         * pointer, stay valid. */
        Terminal result;
        Error err = fromTerminal(terminal_record::decode(reader, alloc, &result));
        if (err != Error::none) return err;
        struct ResultGuard {
            Terminal *t;
            Allocator a;
            bool armed;
            ~ResultGuard() {
                if (armed) t->deinit(a);
            }
        } result_guard = { &result, alloc, true };

        /* TERMINAL initializes exactly the number of screen slots it
         * declared. Decode that many SCREEN sequences and route each one by
         * its encoded key. */
        size_t screen_count = 0;
        for (size_t i = 0; i < key_count; i++) {
            if (result.screens.all[i] != nullptr) screen_count += 1;
        }
        TerminalScreen::Options screen_options;
        {
            const TerminalScreen *primary = result.screens.get(TerminalScreenKey::primary);
            const size_t explicit_bytes = primary->pages.limits.bytes.explicit_;
            const size_t explicit_lines = primary->pages.limits.lines.explicit_;
            screen_options = TerminalScreen::Options(
                result.cols, result.rows,
                explicit_bytes == SIZE_MAX ? ::wisp::vt::Maybe<size_t>() : ::wisp::vt::Maybe<size_t>(explicit_bytes),
                explicit_lines == SIZE_MAX ? ::wisp::vt::Maybe<size_t>() : ::wisp::vt::Maybe<size_t>(explicit_lines));
        }

        bool routed[key_count] = {false, false};
        bool history_rows_present[key_count] = {false, false};
        uint64_t history_rows[key_count] = {0, 0};
        for (size_t i = 0; i < screen_count; i++) {
            screen::Decoded decoded;
            err = fromScreen(screen::decode(reader, alloc, screen_options, &decoded));
            if (err != Error::none) return err;

            TerminalScreen *slot = result.screens.get(decoded.key);
            if (slot == nullptr) {
                decoded.deinit();
                return Error::UnexpectedScreenKey;
            }
            if (routed[(size_t)decoded.key]) {
                decoded.deinit();
                return Error::DuplicateScreen;
            }
            routed[(size_t)decoded.key] = true;
            history_rows_present[(size_t)decoded.key] = true;
            history_rows[(size_t)decoded.key] = decoded.history_rows;

            slot->deinit();
            *slot = decoded.screen;
        }

        /* CONTINUATION is required after every active screen. Nonempty bytes
         * are owned locally until the READY prefix validates. */
        Continuation decoded_continuation;
        err = fromContinuationDecode(continuation::decode(reader, &decoded_continuation, options.max_continuation_bytes));
        if (err != Error::none) return err;

        /* READY marks the exact envelope-through-CONTINUATION boundary. */
        err = fromCheckpointDecode(checkpoint::decode(checkpoint::Kind::ready, source));
        if (err != Error::none) return err;

        /* Record where history may be applied. The declared keys route the
         * HISTORY sequences, and the generations detect declared screens
         * that live terminal use destroys or replaces before their history
         * arrives. */
        History h;
        for (size_t i = 0; i < key_count; i++) {
            h.generations_present[i] = result.screens.all[i] != nullptr;
            h.generations[i] = h.generations_present[i] ? result.screens.generation((TerminalScreenKey)i) : 0;
            h.routed[i] = false;
        }
        h.pending = screen_count;
        h.has_current = false;
        h.cols = result.cols;

        state = State::history;
        history_state = h;
        result_guard.armed = false;
        out->has_terminal = true;
        out->terminal = result;
        out->continuation = decoded_continuation;
        for (size_t i = 0; i < key_count; i++) {
            out->history_rows_present[i] = history_rows_present[i];
            out->history_rows[i] = history_rows[i];
        }
        return Error::none;
    }

    Error nextInner(Allocator alloc, Terminal *t, Progress *out, bool *has) {
        History *hs = &history_state;

        while (true) {
            /* Stream the current sequence's next PAGE if one remains. */
            if (hs->has_current) {
                if (hs->current.remaining > 0) {
                    const Error err = nextPage(alloc, t, &hs->current, out);
                    if (err != Error::none) return err;
                    *has = true;
                    return Error::none;
                }
                hs->has_current = false;
            }

            /* All sequences are consumed: FINISH ends the snapshot, leaving
             * trailing transport bytes unread. */
            if (hs->pending == 0) {
                const Error err = fromCheckpointDecode(checkpoint::decode(checkpoint::Kind::finish, source));
                if (err != Error::none) return err;
                if (::wisp::vt::slow_runtime_safety) {
                    for (size_t i = 0; i < key_count; i++) {
                        if (!hs->generations_present[i]) continue;
                        TerminalScreen *restored = t->screens.get((TerminalScreenKey)i);
                        if (restored == nullptr) continue;
                        restored->pages.assertIntegrity();
                        restored->assertIntegrity();
                    }
                }
                state = State::finished;
                return Error::none;
            }

            /* Begin the next HISTORY sequence. Keys route the sequences
             * order-independently, but each must name a declared screen and
             * may not repeat. */
            hs->pending -= 1;
            history::Decoder<SourceT> manifest;
            const Error err = fromHistory(manifest.init(source));
            if (err != Error::none) return err;
            const TerminalScreenKey key = manifest.header.key;
            if (!hs->generations_present[(size_t)key]) {
                return Error::UnexpectedHistoryKey;
            }
            if (hs->routed[(size_t)key]) return Error::DuplicateHistory;
            hs->routed[(size_t)key] = true;
            hs->has_current = true;
            hs->current.key = key;
            hs->current.remaining = manifest.header.page_count;
            hs->current.apply = true;
        }
    }

    /* Consume one PAGE record from the current sequence, applying it to
     * the live terminal when it is still applicable and discarding it
     * otherwise. */
    Error nextPage(Allocator alloc, Terminal *t, Sequence *sequence, Progress *out) {
        const History *hs = &history_state;

        /* Application is re-decided on every page so live terminal changes
         * between calls degrade to discarding instead of failing decoding. */
        TerminalScreen *destination = nullptr;
        do {
            if (!sequence->apply) break;
            if (t->cols != hs->cols) break;
            TerminalScreen *restored = t->screens.get(sequence->key);
            if (restored == nullptr) break;
            if (t->screens.generation(sequence->key) != hs->generations[(size_t)sequence->key]) {
                break;
            }
            destination = restored;
        } while (false);

        sequence->remaining -= 1;
        size_t rows = 0;
        if (destination == nullptr) {
            /* The record must still be consumed so decoding remains
             * aligned with the following records and FINISH marker. */
            sequence->apply = false;
            const Error err = fromPage(page::discard(source));
            if (err != Error::none) return err;
            rows = 0;
        } else {
            /* The one-shot history decoder rejects a Screen which already has
             * complete history. This incremental path intentionally bypasses
             * that guard: the terminal has been live since READY and may have
             * accumulated new history. Generation and width checks above keep
             * the destination compatible, while per-page finalization enforces
             * its current scrollback limits. */
            const Error err = fromHistory(history::decodePage(source, alloc, destination, &rows));
            switch (err) {
                case Error::none: break;

                /* The page no longer fits the screen's scrollback limits,
                 * e.g. because post-cut output consumed the budget, so it
                 * would be evicted immediately anyway. Its record was fully
                 * consumed, leaving the stream aligned on the next record. */
                case Error::MaxSizeExceeded:
                case Error::MaxLinesExceeded:
                    sequence->apply = false;
                    rows = 0;
                    break;
                default: return err;
            }
        }

        out->key = sequence->key;
        out->rows = rows;
        out->remaining = sequence->remaining;
        return Error::none;
    }
};

/* Restore one complete snapshot into a native Terminal and continuation.
 *
 * This consumes one snapshot through FINISH and leaves any following bytes
 * in the reader for the containing transport. Restoration is transactional:
 * the returned result is either complete and ready or not (error return).
 * Individual record codecs normalize optional semantic state, and history
 * beyond the declared scrollback limits is dropped rather than rejected,
 * while framing, markers, declared sequence counts, and unique
 * cross-record screen routing remain strict.
 *
 * This is the one-shot form of `Decoder`, which can additionally hand the
 * renderable terminal to the caller at READY, before history arrives. */
template <typename SourceT>
inline Error decode(Allocator alloc, SourceT *source, const DecodeOptions &options, Decoded *out) {
    Decoder<SourceT> decoder = Decoder<SourceT>::init(source);
    Decoded result;
    Error err = decoder.ready(alloc, options, &result);
    if (err != Error::none) return err;

    /* The Terminal has not been transferred, so history applies in place. */
    Terminal *t = &result.terminal;
    while (true) {
        typename Decoder<SourceT>::Progress progress;
        bool has = false;
        err = decoder.next(alloc, t, &progress, &has);
        if (err != Error::none) {
            result.deinit(alloc);
            return err;
        }
        if (!has) break;
    }
    *out = result;
    return Error::none;
}

/* Restore one snapshot and require FINISH to be followed by end-of-file.
 *
 * This is intended for bounded snapshot files and buffers. On a live stream,
 * checking for end-of-file may block; use `decode` to stop at FINISH instead. */
template <typename SourceT>
inline Error decodeExact(Allocator alloc, SourceT *source, const DecodeOptions &options, Decoded *out) {
    Decoded result;
    Error err = decode(alloc, source, options, &result);
    if (err != Error::none) return err;

    uint8_t byte = 0;
    ReadError rerr = ReadError::none;
    if (!source->peekByte(&byte, &rerr)) {
        if (rerr == ReadError::EndOfStream) {
            *out = result;
            return Error::none;
        }
        result.deinit(alloc);
        return Error::ReadFailed;
    }
    result.deinit(alloc);
    return Error::TrailingData;
}

} /* namespace snapshot */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_SNAPSHOT_SNAPSHOT_HPP */
