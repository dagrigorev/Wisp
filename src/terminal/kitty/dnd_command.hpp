/* Transliterated from Ghostty src/terminal/kitty/dnd_command.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Wisp, differences in shape rather than behavior:
 *   - `?Metadata` is a bool return plus an out parameter; `?EventType`
 *     inside Metadata is a `has_type` flag plus the value.
 *   - `union(enum) Request` is a tag enum plus one field per payload.
 *   - `packed struct(u2) Operations` is two bools plus protocolValue.
 */

#pragma once
#ifndef WISP_TERMINAL_KITTY_DND_COMMAND_HPP
#define WISP_TERMINAL_KITTY_DND_COMMAND_HPP

#include <stddef.h>
#include <stdint.h>

#include "../osc.hpp"

namespace wisp {
namespace terminal {
namespace kitty {
namespace dnd {

typedef ::wisp::terminal::osc::ZStr ZStr;

/* The event type, i.e. values for the `t` metadata key. A single OSC 72
 * code is used for both directions of the protocol, so most types have
 * one meaning when received by the terminal from a client and another
 * when sent by the terminal to a client.
 *
 * The `drop` and `request_response` types are only ever sent by the
 * terminal; kitty parses but ignores them when received and we do the
 * same. */
enum class EventType : uint8_t {
    /* 'a': (recv) client registers to accept drops. With x=1 the payload
     * is the client's machine ID for remote drop support instead. */
    register_ = 'a',

    /* 'A': (recv) client unregisters from accepting drops. */
    unregister = 'A',

    /* 'm': (recv) client reports acceptance status for the drag currently
     * over the terminal: `o` is the chosen operation and the payload is
     * the accepted MIME list. (send) pointer moved over the terminal
     * during a drag, or with x=-1,y=-1 the drag left the window. */
    status = 'm',

    /* 'M': (send only) items were dropped onto the terminal. */
    drop = 'M',

    /* 'r': (recv) client requests drop data, or with x=y=Y=0 concludes
     * the drop with `o` as the performed operation. (send) drop data
     * response chunks. */
    request = 'r',

    /* 'R': (send only) error response to a data request. */
    request_error = 'R',

    /* 'o': (recv) drag source control: x=1 enables offering drags (payload
     * optionally the client machine ID), x=2 disables, x=0 offers a MIME
     * list for a new drag. (send) request that the client start a drag at
     * the given position. */
    offer = 'o',

    /* 'p': (recv) pre-sent data for an offered drag: x>=0 is a 0-based
     * MIME index, x<0 attaches drag image -x. */
    present = 'p',

    /* 'P': (recv) x=-1 starts the offered drag, x>=0 changes the drag
     * image mid-drag. */
    start_drag = 'P',

    /* 'e': (recv) drag data for MIME index `y` of an in-progress drag.
     * (send) drag status events (accepted, dropped, finished, ...). */
    drag_event = 'e',

    /* 'E': (recv) client aborts the whole drag (y=-1) or reports an error
     * for MIME index `y`. (send) drag start response (OK or error). */
    drag_error = 'E',

    /* 'k': (recv) remote file data for a drag. (send) request for remote
     * file data. */
    remote_data = 'k',

    /* 'q': (recv) query protocol support. (send) the query response. */
    query = 'q',
};

/* Wisp: std.enums.fromInt(EventType, byte). */
inline bool EventType_fromInt(uint8_t v, EventType *out) {
    switch (v) {
    case 'a':
    case 'A':
    case 'm':
    case 'M':
    case 'r':
    case 'R':
    case 'o':
    case 'p':
    case 'P':
    case 'e':
    case 'E':
    case 'k':
    case 'q': *out = (EventType)v; return true;
    default: return false;
    }
}

/* A drop operation. Values match the protocol's `o` key. */
enum class Operation : uint8_t {
    none = 0,
    copy = 1,
    move = 2,
};

/* Convert a protocol `o` value the way kitty does: anything other
 * than copy or move means none. */
inline Operation Operation_fromProtocol(uint32_t v) {
    switch (v) {
    case 1: return Operation::copy;
    case 2: return Operation::move;
    default: return Operation::none;
    }
}

/* The set of operations allowed by a drag source, sent as a bitmask in
 * the `o` key of move and drop events. */
struct Operations {
    bool copy; /* = false */
    bool move; /* = false */

    Operations() : copy(false), move(false) {}

    uint8_t protocolValue() const { return (uint8_t)((copy ? 1 : 0) | (move ? 2 : 0)); }
};

/* Decoded OSC 72 metadata. */
struct Metadata {
    /* Event type (`t`). Null when the metadata had no `t` key; such
     * commands parse successfully but are ignored, matching kitty. */
    bool has_type;
    EventType type; /* = null */

    /* Chunking flag (`m`): true when more chunks follow. */
    bool more; /* = false */

    /* Multiplexer client ID (`i`), echoed in every response so a
     * terminal multiplexer can route responses to the correct client. */
    uint32_t client_id; /* = 0 */

    /* Operation (`o`): meaning depends on the event type, commonly
     * 0=none/reject, 1=copy, 2=move, 3=copy or move. */
    uint32_t operation; /* = 0 */

    /* `x`, `y`, `X`, `Y` keys. */
    int32_t cell_x;  /* = 0 */
    int32_t cell_y;  /* = 0 */
    int32_t pixel_x; /* = 0 */
    int32_t pixel_y; /* = 0 */

    Metadata()
        : has_type(false), type(EventType::register_), more(false), client_id(0), operation(0), cell_x(0),
          cell_y(0), pixel_x(0), pixel_y(0) {}

    /* Parse an unsigned decimal value at `pos`, advancing it. At most
     * 10 digits and at most maxInt(u32), matching kitty. Returns null
     * when there are no digits or the value is too large. */
    static bool parseUnsigned(ZStr raw, size_t *pos, uint32_t *out) {
        const size_t start = *pos;
        uint64_t acc = 0;
        size_t i = start;
        while (i < raw.len && i < start + 10) {
            const uint8_t d = (uint8_t)((uint8_t)raw.ptr[i] - (uint8_t)'0');
            if (d > 9) break;
            acc = acc * 10 + d;
            i += 1;
        }
        if (i == start) return false;
        *pos = i;
        if (acc > UINT32_MAX) return false;
        *out = (uint32_t)acc;
        return true;
    }

    /* Parse raw OSC 72 metadata (the part before the first `;`).
     * Returns null when malformed; callers should ignore the command,
     * matching kitty which rejects the entire command on any error. */
    static bool parse(ZStr raw, Metadata *out) {
        Metadata result;
        size_t pos = 0;
        /* The continue expression consumes the ':' separating a field
         * from the next; the body advances past the field itself. */
        while (pos < raw.len) {
            /* Single-character key. */
            const char key = raw.ptr[pos];
            pos += 1;
            switch (key) {
            case 't':
            case 'm':
            case 'i':
            case 'o':
            case 'x':
            case 'y':
            case 'X':
            case 'Y': break;
            default: return false;
            }

            /* '=' separator. */
            if (pos >= raw.len) return false;
            if (raw.ptr[pos] != '=') return false;
            pos += 1;

            /* Value. */
            if (pos >= raw.len) return false;
            switch (key) {
            case 't': {
                if (!EventType_fromInt((uint8_t)raw.ptr[pos], &result.type)) return false;
                result.has_type = true;
                pos += 1;
                break;
            }

            case 'm':
            case 'i':
            case 'o': {
                uint32_t v;
                if (!parseUnsigned(raw, &pos, &v)) return false;
                switch (key) {
                case 'm': result.more = v != 0; break;
                case 'i': result.client_id = v; break;
                case 'o': result.operation = v; break;
                }
                break;
            }

            case 'x':
            case 'y':
            case 'X':
            case 'Y': {
                const bool negative = raw.ptr[pos] == '-';
                if (negative) pos += 1;
                uint32_t unsigned_;
                if (!parseUnsigned(raw, &pos, &unsigned_)) return false;
                /* Matches kitty's cast of the u32 magnitude to i32,
                 * which wraps rather than erroring on overflow. */
                const int32_t magnitude = (int32_t)unsigned_;
                const int32_t v = negative ? (int32_t)(0u - (uint32_t)magnitude) : magnitude;
                switch (key) {
                case 'x': result.cell_x = v; break;
                case 'y': result.cell_y = v; break;
                case 'X': result.pixel_x = v; break;
                case 'Y': result.pixel_y = v; break;
                }
                break;
            }
            }

            /* Values are separated by ':'. */
            if (pos >= raw.len) break;
            if (raw.ptr[pos] != ':') return false;
            pos += 1;
        }

        *out = result;
        return true;
    }
};

/* A decoded `t=r` data request. The request form is disambiguated by
 * which keys are non-zero, mirroring kitty's drop_process_queue. */
struct Request {
    enum class Tag : uint8_t {
        /* x=y=Y=0: the drop is concluded with the given operation. */
        conclude,

        /* Y=0, y=0, x!=0: request data for the 1-based MIME index x. */
        mime,

        /* Y=0, y!=0: request the contents of the y'th (1-based) file in the
         * text/uri-list MIME at 1-based index x. Remote drops only. */
        uri,

        /* Y!=0: request entry x (1-based) of directory handle Y, or close
         * the handle when x=0. Remote drops only. */
        dir,
    };

    struct Uri {
        int32_t mime_idx;
        int32_t uri_idx;
    };

    struct Dir {
        int32_t handle;
        int32_t entry;
    };

    Tag tag;
    Operation conclude;
    int32_t mime;
    Uri uri;
    Dir dir;

    Request() : tag(Tag::conclude), conclude(Operation::none), mime(0) {
        uri.mime_idx = 0;
        uri.uri_idx = 0;
        dir.handle = 0;
        dir.entry = 0;
    }

    static Request init(const Metadata &meta) {
        Request r;
        if (meta.pixel_y != 0) {
            r.tag = Tag::dir;
            r.dir.handle = meta.pixel_y;
            r.dir.entry = meta.cell_x;
            return r;
        }
        if (meta.cell_y != 0) {
            r.tag = Tag::uri;
            r.uri.mime_idx = meta.cell_x;
            r.uri.uri_idx = meta.cell_y;
            return r;
        }
        if (meta.cell_x != 0) {
            r.tag = Tag::mime;
            r.mime = meta.cell_x;
            return r;
        }
        r.tag = Tag::conclude;
        r.conclude = Operation_fromProtocol(meta.operation);
        return r;
    }
};

/* Chunk reassembly state.
 *
 * While a chunked command is in progress, the metadata of the first
 * chunk is reused for all subsequent chunks; only the `more` flag is
 * taken from each continuation. */
struct Chunking {
    bool active;       /* = false */
    Metadata metadata; /* = .{} */

    Chunking() : active(false), metadata() {}

    /* Returns the effective metadata for a received chunk and updates
     * the reassembly state. */
    Metadata apply(const Metadata &meta) {
        if (active) {
            Metadata copy = metadata;
            copy.more = meta.more;
            active = meta.more;
            return copy;
        }

        if (meta.more) {
            active = true;
            metadata = meta;
        }

        return meta;
    }
};

} /* namespace dnd */
} /* namespace kitty */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_KITTY_DND_COMMAND_HPP */
