/* Transliterated from Ghostty src/terminal/apc.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Wisp, differences in shape rather than behavior:
 *   - Ported with kitty_graphics = false and glyph_protocol = false, so the
 *     `.kitty` and `.glyph` state prongs and Command members, and the tests
 *     gated on those build options, are not carried over. Identification
 *     still recognizes both so a disabled protocol is ignored rather than
 *     reported as unknown, which is upstream's behavior in this build.
 *   - `?Command` is a bool return plus the Command through an out parameter;
 *     `union(enum)` is a tag enum plus one field per payload.
 *   - std.EnumMap/std.EnumSet over Protocol are plain arrays indexed by
 *     the protocol.
 *   - log.* has no sink yet and is a comment.
 */

#pragma once
#ifndef WISP_TERMINAL_APC_HPP
#define WISP_TERMINAL_APC_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../zigstd/allocator.hpp"
#include "../zigstd/array_list.hpp"

namespace wisp {
namespace terminal {
namespace apc {

namespace glyph {
/* Wisp: apc/glyph.zig's identifier, which the identify state needs even
 * with glyph_protocol disabled. */
static const char identifier[] = "25a1";
static const size_t identifier_len = sizeof(identifier) - 1;
} /* namespace glyph */

/* Possible APC command types. */
enum class Protocol : uint8_t {
    kitty,
    glyph,
};
static const size_t protocol_count = 2;

/* Returns the default maximum bytes for the given protocol. */
inline size_t Protocol_defaultMaxBytes(Protocol self) {
    switch (self) {
    /* Kitty graphics payloads can be very large (e.g. full images
     * encoded as base64), so the default is set to 65 MiB. */
    case Protocol::kitty: return 65 * 1024 * 1024;
    /* Glyph protocol messages carry single glyf outlines which
     * are small, but base64 encoding inflates them. 1 MiB is
     * generous for any single simple-glyph record. */
    case Protocol::glyph: return 1 * 1024 * 1024;
    }
    return 0;
}

/* Return the largest default buffer limit across every APC protocol.
 * Consumers that must retain any unfinished APC can derive their limit
 * here instead of duplicating a particular protocol's current default. */
inline size_t Protocol_maxDefaultBytes() {
    size_t result = 0;
    for (size_t i = 0; i < protocol_count; i++) {
        const size_t v = Protocol_defaultMaxBytes((Protocol)i);
        if (v > result) result = v;
    }
    return result;
}

/* An unsupported APC returned by `Handler.end`. */
struct Unknown {
    uint8_t *content;
    size_t content_len;
    bool truncated;

    Unknown() : content(nullptr), content_len(0), truncated(false) {}

    void deinit(zigstd::Allocator alloc) {
        if (content_len > 0) alloc.freeT<uint8_t>(content, content_len);
        content = nullptr;
        content_len = 0;
    }
};

/* UnknownBuilder is responsible for accumulating bytes for an
 * unidentified APC command if unknown capture is enabled. */
struct UnknownBuilder {
    zigstd::ArrayListUnmanaged<uint8_t> data; /* = .empty */
    zigstd::Allocator alloc;
    size_t max_bytes;
    bool truncated; /* = false */

    UnknownBuilder() : data(), alloc(zigstd::c_allocator()), max_bytes(0), truncated(false) {}

    static UnknownBuilder init(zigstd::Allocator a, size_t max_bytes_) {
        UnknownBuilder b;
        b.alloc = a;
        b.max_bytes = max_bytes_;
        return b;
    }

    void deinit() {
        data.deinit(alloc);
        data = zigstd::ArrayListUnmanaged<uint8_t>();
    }

    /* Append some bytes to the unknown capture. This flags as truncated
     * if allocation fails or we reach our byte limit, therefore
     * it can't fail. */
    void append(const uint8_t *bytes, size_t bytes_len) {
        if (bytes_len == 0) return;
        const size_t current = data.items_len;

        /* Determine how many bytes we can store in this append and
         * if it is less than our input, then we have to note we're
         * truncating. */
        const size_t room = max_bytes > current ? max_bytes - current : 0;
        const size_t retained = bytes_len < room ? bytes_len : room;
        if (retained < bytes_len) truncated = true;

        /* If we require more bytes than our capacity allows then we
         * need to grow. */
        const size_t required = current + retained;
        if (required > data.capacity) {
            size_t doubled = data.capacity * 2;
            if (doubled < 1) doubled = 1;
            size_t capacity = required > doubled ? required : doubled;
            if (capacity > max_bytes) capacity = max_bytes;
            if (!data.ensureTotalCapacity(alloc, capacity)) {
                truncated = true;
                return;
            }
        }

        for (size_t i = 0; i < retained; i++) data.appendAssumeCapacity(bytes[i]);
    }

    /* Convert the current capture state to an Unknown where allocator
     * ownership shifts to Unknown. Removes any accumulated unknown
     * capture in this struct.
     *
     * This can't fail because if there is an allocator issue we return
     * an empty truncate-flagged Unknown. */
    Unknown toOwned() {
        Unknown result;
        uint8_t *content = nullptr;
        size_t content_len = 0;
        if (!data.toOwnedSlice(alloc, &content, &content_len)) {
            data.deinit(alloc);
            data = zigstd::ArrayListUnmanaged<uint8_t>();
            result.content = nullptr;
            result.content_len = 0;
            result.truncated = true;
            return result;
        }

        result.content = content;
        result.content_len = content_len;
        result.truncated = truncated;
        return result;
    }
};

/* A recognized or unsupported APC command. */
struct Command {
    enum class Key : uint8_t {
        unknown,
    };

    Key key;
    Unknown unknown;

    Command() : key(Key::unknown), unknown() {}

    void deinit(zigstd::Allocator alloc) {
        switch (key) {
        case Key::unknown: unknown.deinit(alloc); break;
        }
    }
};

enum class StateKey : uint8_t {
    /* We're not in the middle of an APC command yet. */
    inactive,

    /* We got an unrecognized APC sequence or the APC sequence we
     * recognized became invalid. We're just dropping bytes. */
    ignore,

    /* We're waiting to identify the APC sequence. The way this is done
     * is pretty fluid depending on supported APC protocols, but for now
     * our rule is:
     *
     *  * 'G' - immediate transition to Kitty graphics protocol
     *  * Buffer up to `;` and the bytes before dictate the protocol.
     *    If we overflow then we're immediately invalid because we don't
     *    support anything longer than this. */
    identify,

    /* An unsupported APC retained for the optional unknown callback. */
    unknown,
};

struct State {
    StateKey key;

    struct Identify {
        uint8_t len; /* = 0 */
        uint8_t buf[glyph::identifier_len];
    } identify;

    UnknownBuilder unknown;

    State() : key(StateKey::inactive), unknown() {
        identify.len = 0;
        memset(identify.buf, 0, sizeof identify.buf);
    }

    void deinit() {
        switch (key) {
        case StateKey::inactive:
        case StateKey::ignore:
        case StateKey::identify: break;
        case StateKey::unknown: unknown.deinit(); break;
        }
    }
};

/* APC command handler. This should be hooked into a terminal.Stream handler.
 * The start/feed/end functions are meant to be called from the terminal.Stream
 * apcStart, apcPut, and apcEnd functions, respectively. */
struct Handler {
    State state; /* = .inactive */

    /* Maximum content bytes retained for unsupported APC identifiers. Zero
     * drops and ignores unknown APC values. */
    size_t unknown_max_bytes; /* = 0 */

    /* Maximum bytes each APC protocol can buffer. This is to prevent
     * malicious input from causing us to allocate too much memory. */
    size_t max_bytes[protocol_count];

    /* Protocols recognized by this APC handler. When a protocol is absent,
     * matching APC sequences are ignored and are not reported as unknown. */
    bool enabled[protocol_count];

    Handler() : state(), unknown_max_bytes(0) {
        for (size_t i = 0; i < protocol_count; i++) {
            max_bytes[i] = Protocol_defaultMaxBytes((Protocol)i);
            enabled[i] = true;
        }
    }

    void deinit() { state.deinit(); }

    void start() {
        state.deinit();
        state = State();
        state.key = StateKey::identify;
    }

    /* Enable or disable APC protocol recognition for future APC sequences.
     * This does not affect any APC command already being parsed. */
    void enable(Protocol protocol, bool enabled_) { enabled[(size_t)protocol] = enabled_; }

    /* Transition from protocol identification to bounded unknown capture. */
    void beginUnknown(zigstd::Allocator alloc, const uint8_t *prefix, size_t prefix_len, const uint8_t *suffix,
                      size_t suffix_len) {
        const size_t max_bytes_ = unknown_max_bytes;
        if (max_bytes_ == 0) {
            state.key = StateKey::ignore;
            return;
        }

        /* Build the replacement before overwriting identify because prefix
         * points into that union field. */
        UnknownBuilder unknown = UnknownBuilder::init(alloc, max_bytes_);
        unknown.append(prefix, prefix_len);
        unknown.append(suffix, suffix_len);
        state.key = StateKey::unknown;
        state.unknown = unknown;
    }

    void feed(zigstd::Allocator alloc, uint8_t byte) {
        switch (state.key) {
        case StateKey::inactive: return; /* unreachable */

        /* We're ignoring this APC command, likely because we don't
         * recognize it so there is no need to store the data in memory. */
        case StateKey::ignore: return;

        /* Unsupported APC content is retained only when enabled. */
        case StateKey::unknown: state.unknown.append(&byte, 1); return;

        /* We identify the APC command by the first byte. */
        case StateKey::identify: {
            State::Identify *id = &state.identify;

            /* Kitty graphics is detected immediately on the `G` byte,
             * since commands begin immediately after with no termination
             * character after the 'G'.
             * Wisp: build_options.kitty_graphics is false here. */
            if (id->len == 0 && byte == 'G') {
                state.key = StateKey::ignore;
                return;
            }

            /* If we hit `;` then identify... */
            if (byte == ';') {
                const uint8_t *str = id->buf;
                const size_t str_len = id->len;
                if (str_len == glyph::identifier_len &&
                    memcmp(str, glyph::identifier, glyph::identifier_len) == 0) {
                    /* Wisp: build_options.glyph_protocol is false here. */
                    state.key = StateKey::ignore;
                } else {
                    /* Wisp: beginUnknown replaces the identify state, so the
                     * prefix is copied out of it first, as upstream notes. */
                    uint8_t prefix[glyph::identifier_len];
                    memcpy(prefix, str, str_len);
                    beginUnknown(alloc, prefix, str_len, &byte, 1);
                }

                return;
            }

            /* If we're out of identification space, the identifier is
             * unsupported. Preserve the buffered prefix before replacing
             * the identify union state. */
            if (id->len >= sizeof id->buf) {
                uint8_t prefix[glyph::identifier_len];
                memcpy(prefix, id->buf, id->len);
                beginUnknown(alloc, prefix, id->len, nullptr, 0);
                return;
            }

            const size_t expected_idx = id->len;
            id->buf[id->len] = byte;
            id->len += 1;

            /* Once the buffered input is no longer a prefix of a known
             * protocol, it is an unsupported identifier. */
            if (unknown_max_bytes > 0 && byte != (uint8_t)glyph::identifier[expected_idx]) {
                uint8_t prefix[glyph::identifier_len];
                memcpy(prefix, id->buf, id->len);
                beginUnknown(alloc, prefix, id->len, nullptr, 0);
            }
            return;
        }
        }
    }

    /* Feed a slice of bytes to the handler. This is equivalent to
     * calling feed for each byte in order, but protocol payload bytes
     * are passed through in bulk so large payloads (e.g. Kitty graphics
     * images) avoid per-byte dispatch overhead. */
    void feedSlice(zigstd::Allocator alloc, const uint8_t *bytes, size_t bytes_len) {
        const uint8_t *rem = bytes;
        size_t rem_len = bytes_len;
        while (rem_len > 0) {
            switch (state.key) {
            case StateKey::inactive: return; /* unreachable */

            /* We're ignoring this APC command; drop the whole slice. */
            case StateKey::ignore: return;

            /* We're capturing an unknown APC command, so store it. */
            case StateKey::unknown: state.unknown.append(rem, rem_len); return;

            /* Identification consumes at most a few bytes; step
             * through them one at a time until the state changes. */
            case StateKey::identify:
                feed(alloc, rem[0]);
                rem += 1;
                rem_len -= 1;
                break;
            }
        }
    }

    /* Complete the current APC. The caller owns a returned result and must
     * call `Command.deinit` with the allocator used while feeding the APC. */
    bool end(Command *out) {
        bool result = false;
        switch (state.key) {
        case StateKey::inactive: break; /* unreachable */
        case StateKey::ignore:
        case StateKey::identify: break;
        case StateKey::unknown: {
            Command cmd;
            cmd.key = Command::Key::unknown;
            cmd.unknown = state.unknown.toOwned();
            *out = cmd;
            result = true;
            break;
        }
        }

        state.deinit();
        state = State();
        return result;
    }
};

} /* namespace apc */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_APC_HPP */
