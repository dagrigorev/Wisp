/* Transliterated from Ghostty src/terminal/stream_continuation.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Wisp, differences in shape rather than behavior:
 *   - `?usize` is `Maybe<size_t>`; the error union of `validate` is a
 *     ValidateError enum whose `none` is upstream's success.
 *   - `*std.Io.Writer` is `std::string *`, so `Tracker::write` cannot fail
 *     and returns void where upstream returns std.Io.Writer.Error!void.
 *   - findVTReplayStart is the scalar backward scan. Upstream also has a
 *     vectorized path; as elsewhere in this port the SIMD paths are their
 *     scalar equivalents (see ../simd/vt.hpp).
 *   - `validate` is part of the module's API but has no caller here yet: its
 *     users are snapshot/continuation.zig and snapshot/snapshot.zig, which
 *     are not ported.
 */

#pragma once
#ifndef WISP_TERMINAL_STREAM_CONTINUATION_HPP
#define WISP_TERMINAL_STREAM_CONTINUATION_HPP

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "parser.hpp"
#include "utf8_decoder.hpp"
#include "../zigstd/allocator.hpp"
#include "../zigstd/array_list.hpp"

namespace wisp {
namespace terminal {
namespace stream_continuation {

typedef ::wisp::terminal::parser::Parser Parser;
typedef ::wisp::terminal::parser::State PState;
typedef ::wisp::terminal::parser::Action Action;
typedef ::wisp::terminal::UTF8Decoder UTF8Decoder;

template <typename T>
struct Maybe {
    bool has;
    T value;

    Maybe() : has(false), value() {}
    explicit Maybe(T v) : has(true), value(v) {}

    static Maybe<T> none() { return Maybe<T>(); }
};

/* Errors possible while validating a snapshot continuation.
 * Wisp: `none` is upstream's success. */
enum class ValidateError : uint8_t {
    none,

    /* Nonempty input left both the VT parser and UTF-8 decoder at ground.
     *
     * A continuation exists only to reconstruct state that was unfinished at
     * the snapshot cut. Input that returns to ground is a complete PTY
     * fragment, not a continuation. Replaying it would repeat work already
     * represented by the Terminal snapshot. For example, `ESC [ 3 1 m`
     * completes an SGR command and must not be stored as a continuation. */
    NoPendingState,

    /* The input does not begin at its effective replay start.
     *
     * A later ESC supersedes earlier VT parser state, and a pending UTF-8
     * codepoint begins at its lead byte. Any prefix before that effective
     * start is unnecessary and would disappear when the restored Stream
     * exports its continuation again. Rejecting the prefix preserves the
     * byte-identical re-export invariant and prevents unrelated prior input
     * from being replayed. */
    NonCanonicalContinuation,

    /* Replaying the input would perform handler-visible work.
     *
     * The Terminal snapshot already contains every mutation committed before
     * its capture cut. A continuation may rebuild unfinished parser or
     * builder state, but it must not mutate the Terminal or repeat an external
     * effect while doing so. For example, BEL inside an unfinished CSI would
     * ring again even though the CSI itself remains pending. */
    ReplayWouldCommit,
};

/* Find where replay must begin when a feed ends inside a VT sequence.
 *
 * VT parsers treat ESC specially: it abandons the previous parser state and
 * starts a fresh escape state no matter what was being parsed. Because of
 * that rule, feeding the bytes from the last ESC onward into a grounded
 * Stream recreates the unfinished state at the end of `input`.
 *
 * The caller must only use this when the VT parser ended outside ground.
 * The returned value is the index of the last ESC in `input`. A null result
 * means the sequence began in an earlier feed, so this entire input must be
 * appended to the continuation bytes already retained. */
inline Maybe<size_t> findVTReplayStart(const uint8_t *input, size_t len) {
    const uint8_t esc = 0x1B;
    size_t rem = len;
    while (rem > 0) {
        rem -= 1;
        if (input[rem] == esc) return Maybe<size_t>(rem);
    }
    return Maybe<size_t>::none();
}

/* Find where replay must begin when a feed ends inside a UTF-8 codepoint.
 *
 * A UTF-8 codepoint starts with a lead byte and may have up to three
 * continuation bytes. If the lead byte is in this input, replay must begin
 * there so the decoder sees the complete partial codepoint again. Since an
 * incomplete codepoint can contain at most three bytes, only the final three
 * input bytes need to be searched.
 *
 * The caller must only use this when the VT parser is at ground and the
 * UTF-8 decoder ended mid-codepoint. The returned value is the lead byte's
 * index. A null result means the lead byte was in an earlier feed, so this
 * entire input must be appended to the continuation bytes already retained. */
inline Maybe<size_t> findUtf8ReplayStart(const uint8_t *input, size_t len) {
    const size_t max_pending = 3;
    size_t idx = len;
    while (idx > 0 && len - idx < max_pending) {
        idx -= 1;
        if (input[idx] >= 0xC0) return Maybe<size_t>(idx);
    }
    return Maybe<size_t>::none();
}

/* Classifies bytes of a continuation suffix for export.
 *
 * The retained suffix reconstructs unfinished state exactly, but it can
 * contain bytes whose handler-visible work already committed the first
 * time (e.g. a C0 control executed inside an unfinished CSI sequence).
 * This implements a minimal VT stream processor (to avoid circular imports
 * with stream.zig) so `Tracker.write` can omit those bytes and replay
 * never repeats a terminal effect. It only runs at export time, never on
 * the feed path.
 *
 * This is allocation-free. */
struct BoundaryScanner {
    Parser parser;
    UTF8Decoder utf8decoder;

    /* How one byte affects committed work and the continuation suffix. */
    enum class Effect : uint8_t {
        /* The byte commits no handler-visible work. It must remain because it
         * may still build the unfinished VT, UTF-8, APC, or DCS state. */
        uncommitted,

        /* The byte commits handler-visible work but also changes a parser
         * state tag or reaches ground. It cannot be omitted in isolation;
         * boundary analysis decides whether the surrounding prefix is safe. */
        committed,

        /* The byte commits handler-visible work without changing either
         * state-machine tag or reaching ground. It can be omitted without
         * changing the unfinished state, avoiding a duplicate effect. */
        omittable,
    };

    /* Wisp: upstream's `init()` returns a value. The parser owns a
     * non-copyable OSC parser here, so the default constructor does the same
     * job and callers declare the scanner directly. */
    BoundaryScanner() : parser(), utf8decoder() {}

    BoundaryScanner(const BoundaryScanner &) = delete;
    BoundaryScanner &operator=(const BoundaryScanner &) = delete;

    /* True only when neither state machine needs prior bytes to continue. */
    bool ground() const { return parser.state == PState::ground && utf8decoder.state == 0; }

    /* Consume one byte using the same scalar UTF-8 retry and VT transitions
     * as Stream, then classify its effect on continuation construction. */
    Effect next(uint8_t c) {
        /* Preserve the state tags so we can recognize committed controls that
         * do not contribute to the unfinished sequence. */
        const PState parser_state = parser.state;
        const uint8_t utf8_state = utf8decoder.state;

        /* A byte is committed when replaying it would repeat handler-visible
         * work already captured outside the continuation suffix. Actions that
         * only build unfinished APC or DCS state are not committed because
         * replay must reconstruct that state. */
        bool committed = false;
        if (parser.state == PState::ground) {
            /* Match Stream's scalar UTF-8 path, including retrying a byte that
             * follows a malformed sequence. */
            const UTF8Decoder::Result res = utf8decoder.next(c);
            if (res.has_codepoint) committed = codepoint(res.codepoint) || committed;
            if (!res.consumed) {
                const UTF8Decoder::Result retry = utf8decoder.next(c);
                (void)retry.consumed; /* assert(retry[1]) */
                if (retry.has_codepoint) committed = codepoint(retry.codepoint) || committed;
            }
        } else {
            const parser::Next actions = parser.next(c);
            for (int i = 0; i < 3; i++) {
                if (!actions.has(i)) continue;
                switch (actions[i].tag) {
                /* These actions only build standard handler state. Their
                 * matching end/unhook actions are committed work. */
                case Action::Tag::dcs_hook:
                case Action::Tag::dcs_put:
                case Action::Tag::apc_start:
                case Action::Tag::apc_put: break;
                default: committed = true; break;
                }
                if (committed) break;
            }
        }

        if (!committed) return Effect::uncommitted;

        /* A committed byte is independently omittable only while the stream
         * remains unfinished and neither state-machine tag changes. */
        const bool state_changed = parser_state != parser.state || utf8_state != utf8decoder.state;
        return (!state_changed && !ground()) ? Effect::omittable : Effect::committed;
    }

    /* Apply the Stream.handleCodepoint behavior relevant to replay. Returns
     * whether the accepted codepoint would commit handler-visible work. */
    bool codepoint(uint32_t cp) {
        if (cp == 0x1B) {
            parser.state = PState::escape;
            parser.clear();
            return false;
        }

        /* Match Stream.handleCodepoint: all other accepted codepoints have
         * already caused a handler-visible action (including supported C0s). */
        return true;
    }
};

/* Validate continuation bytes.
 *
 * Ensure that:
 *
 *   - replay ends with either VT or UTF-8 state unfinished
 *   - byte zero is the effective start needed to reconstruct that state
 *   - replay commits no Terminal mutation or external handler effect */
inline ValidateError validate(const uint8_t *bytes, size_t len) {
    /* Empty explicitly requests ground state and needs no replay. */
    if (len == 0) return ValidateError::none;

    /* We need the final parser and decoder states to classify the input, so a
     * committed byte cannot return early. Remember it while scanning the rest. */
    BoundaryScanner scanner;
    bool committed_work = false;
    for (size_t i = 0; i < len; i++) {
        if (scanner.next(bytes[i]) != BoundaryScanner::Effect::uncommitted) committed_work = true;
    }

    /* Nonempty continuation bytes must leave state that future input needs. */
    if (scanner.ground()) return ValidateError::NoPendingState;

    /* The tracker exports the minimal suffix beginning at the effective replay
     * start. A different prefix would not survive byte-identical re-export. */
    const Maybe<size_t> replay_start = scanner.parser.state != PState::ground
                                           ? findVTReplayStart(bytes, len)
                                           : findUtf8ReplayStart(bytes, len);
    if (!replay_start.has || replay_start.value != 0) return ValidateError::NonCanonicalContinuation;

    /* At this point the input is unfinished and minimal, but replay must also
     * be inert with respect to the already-restored Terminal and its effects. */
    if (committed_work) return ValidateError::ReplayWouldCommit;
    return ValidateError::none;
}

/* Retains the input needed to reconstruct unfinished Stream parser state.
 *
 * A feed is one chunk of bytes given to a Stream. It can end in the middle of
 * a VT sequence or a UTF-8 codepoint. To continue in another Stream, that
 * Stream starts from ground (with no sequence in progress) and reads the
 * saved bytes again. The first saved byte is called the replay start.
 *
 * Stream updates this tracker once per feed based on where that replay start
 * is:
 *
 *   - Ground (parser and UTF-8 decoder): `reset`, no suffix is needed.
 *   - Anything unfinished: `append`, which replaces the suffix with the
 *     bytes from the replay start onward when that start is inside this
 *     feed, and otherwise extends the suffix with this whole feed because
 *     the unfinished state began in an earlier one.
 *
 * This keeps the retained bytes minimal: they always begin at the replay
 * start, so the feed path never needs to parse or trim old input. The suffix
 * may still contain a byte whose visible terminal effect already happened,
 * such as BEL inside an unfinished CSI sequence. `write` leaves out those
 * bytes so replay does not perform the same effect twice.
 *
 * If the suffix exceeds `max_bytes`, or retaining it fails, `broken`
 * is set until a later feed ends at ground (`reset`) or contains a new
 * replay start (`replace`), both of which need nothing that was lost.
 *
 * Replay semantics are guaranteed for the standard TerminalStream handler.
 * Custom handlers, including handlers that intercept vtRaw, are unsupported. */
struct Tracker {
    static const size_t initial_capacity = 4096;

    zigstd::Allocator alloc;
    size_t max_bytes;
    zigstd::ArrayListUnmanaged<uint8_t> bytes; /* = .empty */
    bool broken;                               /* = false */

    Tracker() : alloc(zigstd::c_allocator()), max_bytes(0), bytes(), broken(false) {}

    /* Which state machine is unfinished at the end of a feed. When the VT
     * parser is outside ground the unfinished state is an escape sequence.
     * When it is grounded, only the UTF-8 decoder can be unfinished, with
     * an incomplete codepoint ending the feed. */
    enum class Pending : uint8_t { vt, utf8 };

    /* Initialize a tracker. */
    static Tracker init(zigstd::Allocator alloc, size_t max_bytes) {
        Tracker result;
        result.alloc = alloc;
        result.max_bytes = max_bytes;
        /* We ignore memory errors here. They'll mark the tracker
         * as broken in a future append. */
        (void)result.bytes.ensureTotalCapacity(
            alloc, max_bytes < initial_capacity ? max_bytes : initial_capacity);
        return result;
    }

    void deinit() { bytes.clearAndFree(alloc); }

    /* Clear the current continuation suffix and broken state while keeping
     * its allocation. Stream calls this after reaching ground because no
     * earlier input is needed to reconstruct the next unfinished state. */
    void reset() {
        broken = false;
        bytes.clearRetainingCapacity();
    }

    /* Retain the part of `input` needed to replay a feed that ended with
     * `pending` state unfinished.
     *
     * When the input contains the replay start for that state, it replaces
     * the retained suffix outright because nothing earlier is needed. The
     * new suffix is complete on its own, so this also repairs a broken
     * tracker. Otherwise the unfinished state began in an earlier feed and
     * this whole input extends the current suffix. */
    void append(Pending pending, const uint8_t *input, size_t len) {
        const Maybe<size_t> start = pending == Pending::vt ? findVTReplayStart(input, len)
                                                           : findUtf8ReplayStart(input, len);
        if (!start.has) {
            extend(input, len);
            return;
        }
        replace(input + start.value, len - start.value);
    }

    /* Write the replay-safe continuation suffix to `writer`. The retained
     * bytes already begin at the replay start; this omits only bytes that
     * would repeat committed terminal effects without contributing to the
     * unfinished parser state (e.g. a BEL inside an unfinished CSI). The
     * tracker must not be broken. */
    void write(std::string *writer) const {
        BoundaryScanner scanner;
        for (size_t i = 0; i < bytes.items_len; i++) {
            const uint8_t c = bytes.items[i];
            if (scanner.next(c) == BoundaryScanner::Effect::omittable) continue;
            writer->push_back((char)c);
        }
    }

private:
    /* Replace the continuation suffix with one that has a new replay start. */
    void replace(const uint8_t *suffix, size_t len) {
        bytes.clearRetainingCapacity();
        if (len > max_bytes) {
            markBroken();
            return;
        }
        if (!bytes.appendSlice(alloc, suffix, len)) {
            markBroken();
            return;
        }
        broken = false;
    }

    /* Extend the continuation suffix with a fragment that contains no replay
     * start of its own. The stream stayed unfinished for the whole fragment,
     * so the existing suffix plus this fragment reproduces the current state. */
    void extend(const uint8_t *fragment, size_t len) {
        if (broken) return;
        /* Wisp: `self.max_bytes -| self.bytes.items.len` is a saturating
         * subtraction. */
        const size_t room = max_bytes > bytes.items_len ? max_bytes - bytes.items_len : 0;
        if (len > room) {
            markBroken();
            return;
        }
        if (!bytes.appendSlice(alloc, fragment, len)) markBroken();
    }

    void markBroken() {
        broken = true;
        bytes.clearRetainingCapacity();
    }
};

} /* namespace stream_continuation */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_STREAM_CONTINUATION_HPP */
