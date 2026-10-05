/* Transliterated from Ghostty src/terminal/kitty/dnd_drop.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Kitty drag and drop protocol (OSC 72) state machine.
 *
 * Wisp, differences in shape rather than behavior:
 *   - `*std.Io.Writer` is `std::string *`, so only the allocator errors
 *     remain; `(Allocator.Error || Writer.Error)!?Event` is a bool return
 *     (false is OutOfMemory) plus the optional Event through out params.
 *   - `?T` is a `has_x` flag plus the value; `[]const []const u8` is a
 *     pointer plus a length.
 *   - log.* has no sink yet and is a comment.
 */

#pragma once
#ifndef WISP_TERMINAL_KITTY_DND_DROP_HPP
#define WISP_TERMINAL_KITTY_DND_DROP_HPP

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "../../zigstd/allocator.hpp"
#include "../../zigstd/array_list.hpp"
#include "../osc.hpp"
#include "dnd_command.hpp"
#include "dnd_response.hpp"

namespace wisp {
namespace terminal {
namespace kitty {
namespace dnd {

/* Maximum accumulated size of a client-sent MIME list (the accepted
 * list of a `t=m` status update). Matches kitty's MIME_LIST_SIZE_CAP. */
static const size_t max_mime_list_bytes = 1024 * 1024;

/* A protocol state change an embedder may need to act on, returned by
 * `handleCommand` and delivered through the stream handler's
 * `drag_and_drop` effect. This is a flat enum so it can cross a C API
 * unchanged; any details are read back from `Terminal.kitty_dnd`. */
enum class Event : uint8_t {
    /* The client registered (t=a), re-registered, or unregistered
     * (t=A) to accept drops. An embedder may want to use this
     * to setup the proper mime types to accept (e.g. on macOS)
     * or not (unregistered). */
    registration,

    /* The client answered the drag currently over the terminal.
     * `State.clientAccepted` has the answer. Embedders can refresh the
     * OS drag feedback immediately rather than on the next move. */
    acceptance,

    /* The client concluded a drop, performing no operation (it
     * canceled), a copy, or a move. The held drop data has been freed. */
    concluded_none,
    concluded_copy,
    concluded_move,
};

/* The conclusion event for a performed operation. */
inline Event Event_concluded(Operation op) {
    switch (op) {
    case Operation::none: return Event::concluded_none;
    case Operation::copy: return Event::concluded_copy;
    case Operation::move: return Event::concluded_move;
    }
    return Event::concluded_none;
}

/* The per-terminal drop target state.
 *
 * The primary entrypoint is `handleCommand` which takes a `*?*State`
 * slot that it can heap allocate into when DnD activates and free when
 * it deactivates.
 *
 * The normal lifecycle:
 *
 *   1. The stream handler feeds every OSC 72 command received from the
 *      client to `handleCommand`. The client registers (t=a), which
 *      allocates the state into the slot and yields a `registration`
 *      event so the embedder can register any declared MIME types
 *      with the OS. Until then `handleCommand` only answers stateless
 *      commands (queries, error responses).
 *   2. A native drag enters or moves over the terminal. When the slot
 *      is non-null, the embedder calls `dragMove` with the pointer
 *      position, the operations the drag source allows, and the MIME
 *      types it can serve if dropped. This sends the client a t=m
 *      move event; when the slot is null the embedder should handle
 *      the drag as it would without the protocol.
 *   3. The client answers with its acceptance (t=m:o=N), recorded by
 *      `handleCommand` which yields an `acceptance` event. The
 *      embedder reads `clientAccepted` then and on subsequent moves
 *      to give the OS drag session its feedback.
 *   4. The drag either leaves, and the embedder calls `dragLeave` to
 *      send the t=m leave event, or drops: the embedder captures the
 *      representations it advertised and calls `dragDrop`, which
 *      copies and holds them and sends the client a t=M drop event. A
 *      new drag entering before the client concludes discards the
 *      held drop.
 *   5. The client requests data (t=r:x=N), which `handleCommand`
 *      serves from the held copies, and then concludes the drop
 *      (t=r:o=N), which frees them and yields a `concluded_*` event
 *      naming the operation the client performed.
 *   6. The client unregisters (t=A) and `handleCommand` frees the
 *      state, yielding a final `registration` event, or the terminal
 *      is deinitialized and calls `destroy`.
 *
 * All calls must use the allocator the state was created with (the
 * terminal's) and require the same synchronization as any other
 * terminal mutation. */
struct State {
    /* One dropped representation: a MIME type and its data. */
    struct Item {
        ZStr mime;
        ZStr data;

        Item() : mime(), data() {}
        Item(ZStr m, ZStr d) : mime(m), data(d) {}
    };

    /* The maximum number of dropped items. Embedders provide a small
     * curated set of representations (see dnd.zig), so this is a
     * generous bound that keeps the MIME list assembly on the stack. */
    static const size_t max_items = 16;

    /* The MIME list of the current drag plus the pre-joined move-event
     * payload ("mime1 mime2 " with a trailing space after every entry,
     * matching kitty) so per-move encoding is allocation-free. */
    struct Offered {
        ZStr *mimes;
        size_t mimes_len;
        ZStr payload;

        Offered() : mimes(nullptr), mimes_len(0), payload() {}

        static bool init(zigstd::Allocator alloc, const ZStr *mimes, size_t mimes_len, Offered *out) {
            ZStr *copies = alloc.allocT<ZStr>(mimes_len);
            if (copies == nullptr) return false;

            size_t payload_len = 0;
            for (size_t i = 0; i < mimes_len; i++) payload_len += mimes[i].len + 1;

            uint8_t *payload = alloc.allocT<uint8_t>(payload_len);
            if (payload == nullptr) {
                alloc.freeT<ZStr>(copies, mimes_len);
                return false;
            }

            size_t offset = 0;
            for (size_t i = 0; i < mimes_len; i++) {
                memcpy(payload + offset, mimes[i].ptr, mimes[i].len);
                payload[offset + mimes[i].len] = ' ';
                copies[i] = ZStr((const char *)(payload + offset), mimes[i].len);
                offset += mimes[i].len + 1;
            }

            out->mimes = copies;
            out->mimes_len = mimes_len;
            out->payload = ZStr((const char *)payload, payload_len);
            return true;
        }

        void deinit(zigstd::Allocator alloc) const {
            alloc.freeT<ZStr>(mimes, mimes_len);
            alloc.freeT<uint8_t>((uint8_t *)payload.ptr, payload.len);
        }

        bool eql(const ZStr *other, size_t other_len) const {
            if (mimes_len != other_len) return false;
            for (size_t i = 0; i < mimes_len; i++) {
                if (mimes[i].len != other[i].len) return false;
                if (memcmp(mimes[i].ptr, other[i].ptr, mimes[i].len) != 0) return false;
            }
            return true;
        }
    };

    struct DropTarget {
        /* Multiplexer client ID from registration, echoed in every
         * drop-side message the terminal sends. */
        uint32_t client_id; /* = 0 */

        /* The MIME list the client registered with (the t=a payload),
         * space-separated as received and accumulated across chunks.
         * Only needed by embedders that must register types with the
         * OS ahead of a drag; kitty frees it after doing so, we keep
         * it so the `registration` event can be acted on from here. */
        zigstd::ArrayListUnmanaged<uint8_t> registered_mimes;

        /* True while the pointer of a native drag is over the terminal. */
        bool hovered; /* = false */

        /* True after the native drop until the client concludes it. */
        bool dropped; /* = false */

        /* The client's response to the current drag, null until the
         * client has responded. `none` means the client rejected it. */
        bool has_accepted;
        Operation accepted; /* = null */

        /* True while a chunked t=m acceptance is being accumulated. */
        bool accept_in_progress; /* = false */

        /* The client's accepted MIME list: space-separated while
         * accumulating, converted to NUL-separated (with a trailing
         * NUL) once complete, matching kitty's in-place conversion. */
        zigstd::ArrayListUnmanaged<uint8_t> accepted_mimes;

        /* The MIME types of the current native drag, in the order
         * that data request indices refer to. */
        bool has_offered;
        Offered offered; /* = null */

        /* The data captured at drop time, parallel to `offered`. */
        bool has_items;
        Item *items; /* = null */
        size_t items_len;

        DropTarget()
            : client_id(0), registered_mimes(), hovered(false), dropped(false), has_accepted(false),
              accepted(Operation::none), accept_in_progress(false), accepted_mimes(), has_offered(false),
              offered(), has_items(false), items(nullptr), items_len(0) {}
    };

    /* Chunk reassembly for client commands. This is the only part of
     * the state cleared by a terminal reset (RIS), matching kitty. */
    Chunking chunking; /* = .{} */

    /* Drop target state for the registered client. */
    DropTarget drop; /* = .{} */

    State() : chunking(), drop() {}

    /* Allocate a fresh state. Done by `handleCommand` on registration. */
    static State *create(zigstd::Allocator alloc) {
        State *state = alloc.create<State>();
        if (state == nullptr) return nullptr;
        *state = State();
        return state;
    }

    /* Free the per-drag data (offered MIME list and held drop items). */
    void freeDragData(zigstd::Allocator alloc) {
        if (drop.has_offered) {
            drop.offered.deinit(alloc);
            drop.has_offered = false;
        }
        if (drop.has_items) {
            for (size_t i = 0; i < drop.items_len; i++) {
                alloc.freeT<uint8_t>((uint8_t *)drop.items[i].mime.ptr, drop.items[i].mime.len);
                alloc.freeT<uint8_t>((uint8_t *)drop.items[i].data.ptr, drop.items[i].data.len);
            }
            alloc.freeT<Item>(drop.items, drop.items_len);
            drop.has_items = false;
            drop.items = nullptr;
            drop.items_len = 0;
        }
    }

    void deinit(zigstd::Allocator alloc) {
        freeDragData(alloc);
        drop.accepted_mimes.deinit(alloc);
        drop.registered_mimes.deinit(alloc);
    }

    /* Free the state and everything it holds. */
    void destroy(zigstd::Allocator alloc) {
        deinit(alloc);
        alloc.destroy<State>(this);
    }

    /* Iterate the MIME types the client registered with, in order.
     * Empty when the client declared none, which is the common case.
     * The list is only needed to register exotic types with the OS,
     * such as macOS pasteboard stuff.
     * Wisp: std.mem.TokenIterator(u8, .scalar) over ' '. */
    struct MimeIterator {
        const uint8_t *data;
        size_t len;
        size_t index;

        bool next(ZStr *out) {
            while (index < len && data[index] == ' ') index += 1;
            if (index >= len) return false;
            const size_t start = index;
            while (index < len && data[index] != ' ') index += 1;
            *out = ZStr((const char *)(data + start), index - start);
            return true;
        }
    };

    MimeIterator registeredMimes() const {
        MimeIterator it;
        it.data = drop.registered_mimes.items;
        it.len = drop.registered_mimes.items_len;
        it.index = 0;
        return it;
    }

    /* Record one chunk of a registration's MIME list.
     *
     * `continuation` is true for every chunk but the first of a chunked
     * registration. Returns the registration event once the list is complete. */
    bool register_(zigstd::Allocator alloc, ZStr payload, bool continuation, bool more, bool *has_event,
                   Event *event) {
        zigstd::ArrayListUnmanaged<uint8_t> *list = &drop.registered_mimes;
        if (!continuation) list->clearRetainingCapacity();

        /* Matching kitty, an over-cap chunk is dropped and does not
         * complete the registration. */
        if (list->items_len + payload.len > max_mime_list_bytes) {
            *has_event = false;
            return true;
        }
        if (!list->appendSlice(alloc, (const uint8_t *)payload.ptr, payload.len)) return false;

        if (more) {
            *has_event = false;
        } else {
            *has_event = true;
            *event = Event::registration;
        }
        return true;
    }

    /* The client's acceptance response for the drag currently over the
     * terminal, for OS drag feedback. Null when the client hasn't
     * responded yet (embedders should fall back to their default,
     * typically copy) or `none` when the client rejected the drag. */
    bool clientAccepted(Operation *out) const {
        if (drop.accept_in_progress) return false;
        if (!drop.has_accepted) return false;
        *out = drop.accepted;
        return true;
    }

    /* Clear the per-drag state while preserving the registration,
     * mirroring kitty's reset_drop. Called when a new drag enters and
     * when a drop concludes. */
    void resetDrop(zigstd::Allocator alloc) {
        freeDragData(alloc);
        drop.accepted_mimes.clearAndFree(alloc);
        drop.hovered = false;
        drop.dropped = false;
        drop.has_accepted = false;
        drop.accept_in_progress = false;
    }

    /* Handle a t=m acceptance status update from the client, mirroring
     * kitty's drop_set_status. */
    bool acceptStatus(zigstd::Allocator alloc, const Metadata &meta, ZStr payload, bool *has_event, Event *event) {
        DropTarget *d = &drop;
        if (!d->accept_in_progress) {
            d->accepted_mimes.clearRetainingCapacity();
            d->accept_in_progress = true;
            d->has_accepted = true;
            d->accepted = Operation_fromProtocol(meta.operation);
        }

        if (payload.len > 0) {
            /* Matching kitty, an over-cap list stops accumulating and
             * never finalizes, leaving the acceptance unanswered. */
            if (d->accepted_mimes.items_len + payload.len > max_mime_list_bytes) {
                *has_event = false;
                return true;
            }
            if (!d->accepted_mimes.appendSlice(alloc, (const uint8_t *)payload.ptr, payload.len)) return false;
        }

        if (meta.more) {
            *has_event = false;
            return true;
        }
        d->accept_in_progress = false;
        if (d->accepted_mimes.items_len > 0) {
            for (size_t i = 0; i < d->accepted_mimes.items_len; i++) {
                if (d->accepted_mimes.items[i] == ' ') d->accepted_mimes.items[i] = 0;
            }
            if (!d->accepted_mimes.append(alloc, 0)) return false;
        }
        *has_event = true;
        *event = Event::acceptance;
        return true;
    }

    /* A native drag position report from the embedder. */
    struct MoveEvent {
        /* Grid cell under the pointer, zero-based from the top-left. */
        uint32_t cell_x;
        uint32_t cell_y;

        /* Pointer position in pixels relative to the top-left of the
         * terminal's content area. */
        int32_t pixel_x;
        int32_t pixel_y;

        /* The operations the drag source allows. */
        Operations operations;

        MoveEvent() : cell_x(0), cell_y(0), pixel_x(0), pixel_y(0), operations() {}
    };

    /* Shared implementation of move and drop events, mirroring kitty's
     * drop_move_on_child. */
    bool moveEvent(zigstd::Allocator alloc, std::string *writer, const MoveEvent &ev, const ZStr *mimes,
                   size_t mimes_len, bool is_drop) {
        if (!drop.hovered) {
            resetDrop(alloc);
            drop.hovered = true;
        }
        if (is_drop) {
            drop.dropped = true;
            drop.hovered = false;
        }

        /* (Re)build the offered MIME list when it changed. */
        if (!drop.has_offered || !drop.offered.eql(mimes, mimes_len)) {
            if (drop.has_offered) drop.offered.deinit(alloc);
            drop.has_offered = false;
            if (!Offered::init(alloc, mimes, mimes_len, &drop.offered)) return false;
            drop.has_offered = true;
        }

        char header_buf[96];
        snprintf(header_buf, sizeof header_buf, "t=%c:x=%u:y=%u:X=%d:Y=%d:o=%u", is_drop ? 'M' : 'm',
                 (unsigned)ev.cell_x, (unsigned)ev.cell_y, (int)ev.pixel_x, (int)ev.pixel_y,
                 (unsigned)ev.operations.protocolValue());

        /* The MIME list is sent with every move event, matching kitty
         * (the spec suggests only the first, but kitty always sends it
         * and clients depend on that). */
        encode(writer, ZStr(header_buf, strlen(header_buf)), drop.client_id, drop.offered.payload,
               Encoding::plain, Terminator::st);
        return true;
    }

    /* Report a native drag moving over the terminal, sending a t=m
     * move event to the client. `mimes` is the list of MIME types the
     * terminal can provide for this drag, in the order data request
     * indices will refer to. */
    bool dragMove(zigstd::Allocator alloc, std::string *writer, const MoveEvent &ev, const ZStr *mimes,
                  size_t mimes_len) {
        return moveEvent(alloc, writer, ev, mimes, mimes_len, false);
    }

    /* Report a native drop onto the terminal. The items' data is
     * copied and held so the client's data requests can be served; it
     * is freed when the client concludes the drop, a new drag enters,
     * or the client unregisters.
     *
     * Sends a t=M drop event listing the items' MIME types. */
    bool dragDrop(zigstd::Allocator alloc, std::string *writer, const MoveEvent &ev, const Item *items,
                  size_t items_len) {
        /* Copy the items so they can be served after this call returns.
         * Items beyond the cap are dropped so the held list always
         * matches the advertised MIME list. */
        const size_t accepted_len = items_len < max_items ? items_len : max_items;
        Item *copies = alloc.allocT<Item>(accepted_len);
        if (copies == nullptr) return false;
        size_t copied = 0;
        for (size_t i = 0; i < accepted_len; i++) {
            uint8_t *mime = alloc.allocT<uint8_t>(items[i].mime.len);
            if (mime == nullptr) break;
            memcpy(mime, items[i].mime.ptr, items[i].mime.len);
            uint8_t *data = alloc.allocT<uint8_t>(items[i].data.len);
            if (data == nullptr) {
                alloc.freeT<uint8_t>(mime, items[i].mime.len);
                break;
            }
            memcpy(data, items[i].data.ptr, items[i].data.len);
            copies[i] = Item(ZStr((const char *)mime, items[i].mime.len),
                             ZStr((const char *)data, items[i].data.len));
            copied += 1;
        }
        if (copied != accepted_len) {
            for (size_t i = 0; i < copied; i++) {
                alloc.freeT<uint8_t>((uint8_t *)copies[i].mime.ptr, copies[i].mime.len);
                alloc.freeT<uint8_t>((uint8_t *)copies[i].data.ptr, copies[i].data.len);
            }
            alloc.freeT<Item>(copies, accepted_len);
            return false;
        }

        /* The move handling below resets per-drag state when this drop
         * arrives without a preceding move, so the items are attached
         * after it runs. Collect the MIME list first. */
        ZStr mimes_buf[max_items];
        for (size_t i = 0; i < accepted_len; i++) mimes_buf[i] = copies[i].mime;

        if (!moveEvent(alloc, writer, ev, mimes_buf, accepted_len, true)) {
            for (size_t i = 0; i < accepted_len; i++) {
                alloc.freeT<uint8_t>((uint8_t *)copies[i].mime.ptr, copies[i].mime.len);
                alloc.freeT<uint8_t>((uint8_t *)copies[i].data.ptr, copies[i].data.len);
            }
            alloc.freeT<Item>(copies, accepted_len);
            return false;
        }

        /* assert(self.drop.items == null) */
        drop.has_items = true;
        drop.items = copies;
        drop.items_len = accepted_len;
        return true;
    }

    /* Report the native drag leaving the terminal, sending the t=m
     * leave event (x=-1, y=-1).
     *
     * Ignored after a drop: some toolkits emit a leave notification
     * for the drop itself, and the held data must survive until the
     * client concludes. */
    void dragLeave(zigstd::Allocator alloc, std::string *writer) {
        if (drop.dropped) return;
        const bool hovered = drop.hovered;
        drop.hovered = false;
        if (drop.has_offered) {
            drop.offered.deinit(alloc);
            drop.has_offered = false;
        }

        /* Only a client that saw the drag enter gets the leave event,
         * matching kitty which notifies hovered windows only. */
        if (!hovered) return;

        const char *header = "t=m:x=-1:y=-1";
        encode(writer, ZStr(header, strlen(header)), drop.client_id, ZStr(), Encoding::plain, Terminator::st);
    }
};

/* Refuse a drag-out command with an error, since ghostty does not
 * implement the terminal side of client-initiated drags yet. */
inline void refuseDragOut(std::string *writer, const Metadata &meta, Terminator terminator) {
    const char *desc = "drag out is not supported by this terminal";
    encodeError(writer, ErrorKind::drag, RequestKeys(), meta.client_id, Errno::EPERM_,
                ZStr(desc, strlen(desc)), terminator);
}

/* Handle a t=r data request or drop conclusion from the client.
 * Requests from an unregistered client (no state) get the same
 * errors kitty sends from its zeroed drop state. */
inline bool dataRequest(State *state, zigstd::Allocator alloc, std::string *writer, const Metadata &meta,
                        Terminator terminator, bool *has_event, Event *event) {
    (void)alloc;
    *has_event = false;

    /* Responses echo the registration's client ID, matching kitty. */
    const uint32_t client_id = state != nullptr ? state->drop.client_id : 0;

    const Request req = Request::init(meta);
    switch (req.tag) {
    case Request::Tag::conclude: {
        /* The client is done with the drop: free the held data and
         * report the operation it performed. Kitty hands that to
         * the still-open OS drag session; ours ended at drop time
         * (see dnd.zig), so the embedder decides what to do with
         * it. A conclusion with no drop in progress is a no-op. */
        if (state == nullptr) return true;
        const bool dropped = state->drop.dropped;
        state->resetDrop(alloc);
        if (dropped) {
            *has_event = true;
            *event = Event_concluded(req.conclude);
        }
        return true;
    }

    case Request::Tag::mime: {
        const int32_t idx = req.mime;
        RequestKeys keys;
        keys.x = idx;
        if (state == nullptr || !state->drop.has_items) {
            const char *desc = "no drop data available";
            encodeError(writer, ErrorKind::drop, keys, client_id, Errno::ENOENT_, ZStr(desc, strlen(desc)),
                        terminator);
            return true;
        }
        const State::Item *items = state->drop.items;
        const size_t items_len = state->drop.items_len;
        if (idx < 1 || (size_t)idx > items_len) {
            const char *desc = "drop data request index out of bounds";
            encodeError(writer, ErrorKind::drop, keys, client_id, Errno::ENOENT_, ZStr(desc, strlen(desc)),
                        terminator);
            return true;
        }

        std::string header;
        header.append("t=r");
        keys.format(&header);

        /* The data chunks followed by the empty end-of-data
         * message, which is how the client detects completion.
         * An empty item is just the end-of-data message alone;
         * clients treat a duplicate as a second completion. */
        const State::Item &item = items[(size_t)(idx - 1)];
        const ZStr header_z(header.data(), header.size());
        if (item.data.len > 0)
            encode(writer, header_z, client_id, item.data, Encoding::base64, terminator);
        encode(writer, header_z, client_id, ZStr(), Encoding::base64, terminator);
        return true;
    }

    /* Remote drop transfers (URI file contents and directory
     * handles). We never advertise remote support (no X=1
     * marker), so a conforming client never sends these. */
    case Request::Tag::uri: {
        RequestKeys keys;
        keys.x = req.uri.mime_idx;
        keys.y = req.uri.uri_idx;
        const char *desc = "remote drop data is not supported";
        encodeError(writer, ErrorKind::drop, keys, client_id, Errno::EINVAL_, ZStr(desc, strlen(desc)),
                    terminator);
        return true;
    }
    case Request::Tag::dir: {
        RequestKeys keys;
        keys.x = req.dir.entry;
        keys.Y = req.dir.handle;
        const char *desc = "remote drop data is not supported";
        encodeError(writer, ErrorKind::drop, keys, client_id, Errno::EINVAL_, ZStr(desc, strlen(desc)),
                    terminator);
        return true;
    }
    }

    return true;
}

/* Process one OSC 72 command received from the client, writing any
 * responses to the writer. Returns the state change the embedder may
 * need to act on, if any. */
inline bool handleCommand(State **slot, zigstd::Allocator alloc, std::string *writer,
                          const osc::Command &cmd, bool *has_event, Event *event) {
    *has_event = false;

    Metadata raw;
    if (!Metadata::parse(cmd.kitty_dnd_protocol.metadata, &raw)) {
        /* log.debug("dropping malformed OSC 72 metadata") */
        return true;
    }

    /* Chunk reassembly lives in the state, so before registration
     * each command stands alone. The only legitimately chunked
     * command before registration is t=a itself, which seeds the
     * reassembly on its first chunk below. */
    const bool continuation = *slot != nullptr ? (*slot)->chunking.active : false;
    const Metadata meta = *slot != nullptr ? (*slot)->chunking.apply(raw) : raw;
    const ZStr payload = cmd.kitty_dnd_protocol.has_payload ? cmd.kitty_dnd_protocol.payload : ZStr();
    if (!meta.has_type) return true;
    const Terminator terminator = cmd.kitty_dnd_protocol.terminator;

    switch (meta.type) {
    case EventType::register_: {
        /* x=1 declares the client's machine ID for remote drop
         * support. We don't support remote drop yet, so accept and ignore. */
        if (meta.cell_x == 1) return true;

        /* Setup our state if we haven't already */
        State *state = *slot;
        if (state == nullptr) {
            state = State::create(alloc);
            if (state == nullptr) return false;
            *slot = state;
            (void)state->chunking.apply(raw);
        }

        /* Update the client ID on every registration */
        state->drop.client_id = meta.client_id;

        return state->register_(alloc, payload, continuation, meta.more, has_event, event);
    }

    case EventType::unregister: {
        State *state = *slot;
        if (state == nullptr) return true;
        state->destroy(alloc);
        *slot = nullptr;
        *has_event = true;
        *event = Event::registration;
        return true;
    }

    case EventType::status: {
        State *state = *slot;
        if (state == nullptr) return true;
        return state->acceptStatus(alloc, meta, payload, has_event, event);
    }

    case EventType::request:
        return dataRequest(*slot, alloc, writer, meta, terminator, has_event, event);

    /* Drag source control. Enabling (x=1, with an optional
     * machine ID payload) and disabling (x=2) offers are
     * accepted and ignored since the terminal never requests a
     * drag start. Offering a MIME list (x=0) for a new drag is
     * refused since drag-out is not implemented. */
    case EventType::offer:
        if (meta.cell_x == 0) refuseDragOut(writer, meta, terminator);
        break;

    /* Drag-out data and start commands. A conforming client
     * never sends these because the terminal never requests a
     * drag start, but refuse them properly if one does. */
    case EventType::present:
    case EventType::start_drag: refuseDragOut(writer, meta, terminator); break;

    /* Responses to drag-out requests the terminal never makes. */
    case EventType::drag_event:
    case EventType::drag_error:
    case EventType::remote_data: break;

    case EventType::query: {
        const char *header = "t=q";
        encode(writer, ZStr(header, strlen(header)), meta.client_id, ZStr(), Encoding::plain, terminator);
        break;
    }

    /* Only ever sent by the terminal. Ignore. */
    case EventType::drop:
    case EventType::request_error: break;
    }

    return true;
}

} /* namespace dnd */
} /* namespace kitty */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_KITTY_DND_DROP_HPP */
