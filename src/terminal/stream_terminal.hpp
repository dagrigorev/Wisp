/* Transliterated from Ghostty src/terminal/stream_terminal.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Wisp, differences in shape rather than behavior:
 *   - `vt(comptime action, value)` is `vt(const Action &)`; the handler
 *     switches on the tag where upstream specializes at comptime.
 *   - Ported with kitty_graphics = false, glyph_protocol = false and
 *     tmux_control_mode = false, matching the rest of the port. The APC
 *     and DCS handlers already reflect that.
 *   - `*std.Io.Writer` is `std::string *`; write_pty takes a pointer and
 *     a length rather than a sentinel-terminated slice, so the callback
 *     never needs the terminating NUL upstream reserves a byte for.
 *   - `?T` is a `has_x` flag plus the value, error unions are an Error
 *     enum, and `union(enum)` is a tag plus one field per payload.
 *   - log.* has no sink yet and is a comment.
 */

#pragma once
#ifndef WISP_TERMINAL_STREAM_TERMINAL_HPP
#define WISP_TERMINAL_STREAM_TERMINAL_HPP

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "../simd/base64.hpp"
#include "../vt/terminal.hpp"
#include "../zigstd/allocator.hpp"
#include "../zigstd/base64.hpp"
#include "apc.hpp"
#include "clipboard.hpp"
#include "dcs.hpp"
#include "device_attributes.hpp"
#include "device_status.hpp"
#include "kitty/clipboard_command.hpp"
#include "kitty/clipboard_grants.hpp"
#include "kitty/clipboard_response.hpp"
#include "kitty/clipboard_write.hpp"
#include "kitty/dnd_drop.hpp"
#include "paste.hpp"
#include "size_report.hpp"
#include "terminfo.hpp"
#include "vt_stream.hpp"

namespace wisp {
namespace terminal {
namespace stream_terminal {

typedef ::wisp::terminal::osc::ZStr ZStr;
namespace clip = ::wisp::terminal::clipboard;
namespace kitty_clipboard = ::wisp::terminal::kitty::clipboard;
namespace kitty_dnd = ::wisp::terminal::kitty::dnd;
namespace paste_pkg = ::wisp::terminal::paste;

struct Handler;

/* A stream handler that updates terminal state. By default, it is
 * readonly in the sense that it only updates terminal state and ignores
 * all other sequences that require a response or otherwise have side
 * effects (e.g. clipboards).
 *
 * You can manually set various effects callbacks in the `effects` field
 * to implement certain effects such as bells, titles, clipboard, etc. */
struct Handler {
    /* Maximum byte length accepted for `terminfo_name`. */
    static const size_t max_terminfo_name_bytes = 128;

    /* The size of the chunks a paste streams to write_pty in. */
    static const size_t paste_chunk_size = 4096;

    /* Callbacks for certain effects that handlers may have. These
     * may or may not fully replace internal handling of certain effects,
     * but they allow for the handler to trigger or query external
     * effects.
     *
     * Wisp: `.readonly` is the default-constructed Effects, every
     * callback null. */
    struct Effects {
        /* Called when the terminal needs to write data back to the pty,
         * e.g. in response to a DECRQM query. The data is only valid
         * during the lifetime of the call so callers must copy it
         * if it needs to be stored or used after the call returns. */
        void (*write_pty)(Handler *, const char *data, size_t len);

        /* Called when the bell is rung (BEL). */
        void (*bell)(Handler *);

        /* Called when the running program requests a desktop notification
         * via OSC 9 or OSC 777. The title and body are borrowed and only
         * valid for the duration of the callback. */
        void (*desktop_notification)(Handler *, stream::Action::ShowDesktopNotification);

        /* Called when drag and drop protocol state changes in a way the
         * embedder may need to act on: the running program registering
         * or unregistering to accept drops, answering a drag, or
         * concluding a drop. The event says what changed; the details
         * are read from `handler.terminal.kitty_dnd` (Kitty's OSC 72 is
         * the only drag and drop protocol today). Native drag events
         * flow the other way, by calling `kitty.dnd.State` directly. */
        void (*drag_and_drop)(Handler *, kitty_dnd::Event);

        /* Called in response to a color scheme DSR query (CSI ? 996 n).
         * Returns the current color scheme. Return null to silently
         * ignore the query.
         * Wisp: `?ColorScheme` is the bool return plus *out. */
        bool (*color_scheme)(Handler *, device_status::ColorScheme *out);

        /* Called in response to a device attributes query (CSI c,
         * CSI > c, CSI = c). Returns the response to encode and
         * write back to the pty. */
        device_attributes::Attributes (*device_attributes)(Handler *);

        /* Called in response to ENQ (0x05). Returns the raw response
         * bytes to write back to the pty. The returned memory must be
         * valid for the lifetime of the call. */
        ZStr (*enquiry)(Handler *);

        /* Called for XTWINOPS size queries (CSI 14/16/18 t) and when VT input
         * enables in-band size reports (mode 2048). Returns the current
         * terminal geometry used for encoding. Return null to suppress the
         * XTWINOPS response or mode 2048 report. */
        bool (*size)(Handler *, size_report::Size *out);

        /* Called when the terminal title changes via escape sequences
         * (e.g. OSC 0/2). The new title can be queried via
         * handler.terminal.getTitle(). */
        void (*title_changed)(Handler *);

        /* Called when the terminal pwd changes via escape sequences
         * (e.g. OSC 7). The new pwd can be queried via
         * handler.terminal.getPwd(). */
        void (*pwd_changed)(Handler *);

        /* Called when the running program reports progress via OSC 9;4. */
        void (*progress_report)(Handler *, osc::ProgressReport);

        /* Called when the running program writes to a clipboard. */
        void (*clipboard_write)(Handler *, clip::Write);

        /* Called when the running program requests clipboard contents
         * (OSC 52 with a "?" payload, or a Kitty clipboard (OSC 5522)
         * read). Answering one lets the program read the user's
         * clipboard, so the embedder is expected to mediate consent.
         *
         * Reads are synchronous: the callback must answer through
         * `read.reply` before it returns, so an embedder that needs to
         * ask the user must block (e.g. run a modal prompt) while the
         * stream waits. Returning without a reply, or replying with any
         * failure, answers the program with an empty clipboard (OSC 52)
         * or the matching protocol status (OSC 5522) so it doesn't hang.
         * If this is null, OSC 52 reads are ignored and OSC 5522 reads
         * are refused with EPERM.
         *
         * OSC 5522 requests carry the program's MIME list, name, and
         * password grant state; a reply that sets `remember` records a
         * session grant so later requests with the same password arrive
         * with `granted` set. Kitty itself serves a request for only the
         * targets listing (`list` with no `mimes`) without prompting.
         *
         * Installing this also enables Kitty paste events (mode 5522):
         * `paste` sends the program an event instead of the text, and
         * the program's follow-up read arrives here with `granted` set
         * since the user already pasted. See `paste`. */
        void (*clipboard_read)(Handler *, clip::Read);

        /* Called in response to an XTVERSION query. Returns the version
         * string to report (e.g. "ghostty 1.2.3"). The returned memory
         * must be valid for the lifetime of the call. The maximum length
         * is 256 bytes; longer strings will be silently ignored. */
        ZStr (*xtversion)(Handler *);

        /* Called with `true` when the running program asks the terminal
         * to stop updating the screen, and with `false` when it allows
         * updates again. The time in between is a "render hold". Programs
         * use a hold so that the user never sees a half-drawn frame.
         *
         * Today the only source of a hold is synchronized output (mode
         * 2026). The hold begins when VT input sets the mode. It ends
         * when VT input resets the mode, on a full reset, and on a resize
         * through `Handler.resize`. The calls always come in pairs:
         * setting the mode during a hold does nothing, and neither does
         * resetting it when there is no hold. Writing `terminal.modes`
         * directly never calls this.
         *
         * When a hold begins, nothing after the sequence that began it
         * has been processed yet, so the terminal contains exactly the
         * frame the program wants left on screen. A renderer can capture
         * that frame from within this callback (e.g. `RenderState.update`)
         * and then skip updates until the hold ends. Checking the mode
         * before each draw instead can't do this. It leaves whatever was
         * drawn last on screen, and it loses a finished frame entirely
         * when one hold ends and the next begins between two draws.
         *
         * The terminal has no clock so it never ends a hold on its own.
         * The caller must use a timeout so that a program that never
         * releases its hold can't freeze the screen. */
        void (*render_hold)(Handler *, bool);

        /* No effects means that the stream effectively becomes readonly
         * that only affects pure terminal state and ignores all side
         * effects beyond that. */
        Effects()
            : write_pty(nullptr), bell(nullptr), desktop_notification(nullptr), drag_and_drop(nullptr),
              color_scheme(nullptr), device_attributes(nullptr), enquiry(nullptr), size(nullptr),
              title_changed(nullptr), pwd_changed(nullptr), progress_report(nullptr), clipboard_write(nullptr),
              clipboard_read(nullptr), xtversion(nullptr), render_hold(nullptr) {}
    };

    /* A sequence unsupported by the active handler. Payload data is borrowed
     * only for the duration of the handler callback. */
    struct UnknownSequence {
        enum class Tag : uint8_t { apc };

        /* Content between a string sequence's introducer and terminator. */
        struct String {
            const uint8_t *content;
            size_t content_len;
            bool truncated;

            String() : content(nullptr), content_len(0), truncated(false) {}
        };

        Tag tag;
        String apc;

        UnknownSequence() : tag(Tag::apc), apc() {}
    };

    /* Wisp: the error set of `paste`. */
    enum class PasteError : uint8_t {
        none,
        OutOfMemory,
        EntropyUnavailable,

        /* The data could inject commands and allow_unsafe was false.
         * Nothing was written. */
        UnsafePaste,

        /* The contents reader failed. Nothing was written. */
        ReadFailed,

        /* No write_pty effect is set, so nothing can be written. */
        NoWritePty,
    };

    /* A paste request; see `paste`. */
    typedef paste_pkg::Request Paste;

    /* The terminal state to modify. */
    vt::Terminal *terminal;

    /* True after an error prevented a terminal-owned semantic update.
     *
     * When an error happens during terminal processing, streams continue
     * forward and remain best-effort. A terminal can't really stop in
     * the middle it must go on. But this is flagged to true to let
     * consumers know some sort of unhandle-able error state happened
     * (e.g. an allocation failure).
     *
     * Only non-handled outcomes set this. Gracefully handled outcomes
     * that don't meaningfully negatively impact the terminal state
     * such as hitting Kitty image limits, failure to write a response,
     * do not flag this. */
    bool semantic_failure; /* = false */

    Effects effects; /* = .readonly */

    /* Whether CSI 21 t may report the terminal title. This is disabled by
     * default because reporting an attacker-controlled title to the pty can
     * inject text into the input stream of the foreground process. */
    bool title_report; /* = false */

    /* The APC command handler maintains the APC state. APC is like
     * CSI or OSC, but it is a private escape sequence that is used
     * to send commands to the terminal emulator. This is used by
     * the kitty graphics protocol. */
    apc::Handler apc_handler; /* = .{} */

    /* The DCS command handler maintains state for DCS queries. */
    dcs::Handler dcs_handler; /* = .{} */

    /* The in-flight Kitty clipboard protocol (OSC 5522) write
     * transaction, if any. Null means no transaction is active.
     * Heap-allocated since transactions are rare and short-lived. */
    kitty_clipboard::WriteState *kitty_clipboard_write; /* = null */

    /* Kitty clipboard protocol (OSC 5522) session password grants,
     * recorded when a clipboard_read or clipboard_write reply asks to
     * remember the user's decision. */
    kitty_clipboard::Grants kitty_clipboard_grants; /* = .{} */

    /* Maximum total decoded bytes accumulated by one Kitty clipboard
     * protocol (OSC 5522) write transaction, captured when the
     * transaction begins. Data beyond the limit fails the transaction
     * with EFBIG. */
    size_t kitty_clipboard_write_max_bytes; /* = kitty_clipboard.max_write_size */

    /* Called for sequence identifiers not supported by this library.
     * Currently, only APC is reported. Content is borrowed and only valid
     * for the duration of the callback. Set `apc_handler.unknown_max_bytes`
     * before starting the Stream to enable APC capture. */
    void (*unknown_sequence)(Handler *, UnknownSequence); /* = null */

    /* The name of the terminfo entry this terminal runs as, reported in
     * response to an XTGETTCAP query for "TN".
     *
     * The memory must remain valid for the lifetime of the handler.
     * Empty names and names longer than `max_terminfo_name_bytes` are
     * silently ignored. */
    bool has_terminfo_name;
    ZStr terminfo_name; /* = null */

    Handler()
        : terminal(nullptr), semantic_failure(false), effects(), title_report(false), apc_handler(),
          dcs_handler(), kitty_clipboard_write(nullptr), kitty_clipboard_grants(),
          kitty_clipboard_write_max_bytes(kitty_clipboard::max_write_size), unknown_sequence(nullptr),
          has_terminfo_name(false), terminfo_name() {}

    static Handler init(vt::Terminal *t) {
        Handler h;
        h.terminal = t;
        return h;
    }

    void deinit() {
        kittyClipboardAbort();
        kitty_clipboard_grants.deinit(terminal->gpa());
        apc_handler.deinit();
        dcs_handler.deinit();
    }

    /* Wisp: declared here, defined out of line below so the file reads in
     * upstream's order. */
    void vt(const stream::Action &action);
    bool vtFallible(const stream::Action &action);
    void writePty(const char *data, size_t len);
    void writePty(ZStr data) { writePty(data.ptr, data.len); }
    void unknownSequence(UnknownSequence value);
    /* Wisp: false is Terminal.resize's OutOfMemory. */
    bool resize(const vt::Terminal::Resize &value);
    PasteError paste(const Paste &req, bool *out);

    bool dcsHook(const parser::Action::DCS &value);
    bool dcsPut(uint8_t value);
    bool dcsUnhook();
    bool dcsCommand(dcs::Command *cmd);
    void writeTerminfoName();

    void bell();
    void desktopNotification(stream::Action::ShowDesktopNotification notification);
    void progressReport(osc::ProgressReport report);
    bool clipboardContents(uint8_t kind, ZStr data, osc::Terminator terminator);
    void clipboardRead(clip::Location location, osc::Terminator terminator);
    void renderHold(bool held);
    bool setMode(modes::Mode mode, bool enabled);
    void horizontalTab(uint16_t count);
    void horizontalTabBack(uint16_t count);
    void apcEnd(bool terminated);
    void reportDeviceAttributes(device_attributes::Req req);
    void deviceStatus(device_status::Request req);
    void reportEnquiry();
    void queryKittyKeyboard();
    void sendModeReport(const modes::Report &report);
    void sendVisibilityReport();
    void reportMode2048();
    void requestMode(modes::Mode mode);
    void requestModeUnknown(uint16_t mode, bool ansi);
    void reportSize(csi::SizeReportStyle style);
    bool windowTitle(ZStr title);
    bool reportPwd(ZStr url);
    void reportXtversion();
    bool colorOperation(const osc::color::List *requests, osc::Terminator terminator);
    static void writeXtermColorReport(std::string *writer, const osc::color::Target &target, terminal::RGB c,
                                      osc::Terminator terminator);
    bool kittyColorOperation(const stream::Action::KittyColorReport &v);
    bool kittyClipboard(const stream::Action::KittyClipboard &v);
    bool kittyClipboardRead(const kitty_clipboard::Metadata *meta, ZStr payload, osc::Terminator terminator);
    bool kittyClipboardWriteBegin(const kitty_clipboard::Metadata *meta, osc::Terminator terminator);
    bool kittyClipboardData(const kitty_clipboard::Metadata *meta, ZStr payload, osc::Terminator terminator);
    bool kittyClipboardAlias(const kitty_clipboard::Metadata *meta, ZStr payload, osc::Terminator terminator);
    bool kittyClipboardCommit(kitty_clipboard::WriteState *state, osc::Terminator terminator);
    void kittyClipboardFinish(const kitty_clipboard::WriteState *state, kitty_clipboard::Status status,
                              osc::Terminator terminator);
    void kittyClipboardRespond(const kitty_clipboard::Response *response);
    void kittyClipboardAbort();
    bool kittyDnd(const stream::Action::KittyDnd &v);
};

} /* namespace stream_terminal */
} /* namespace terminal */
} /* namespace wisp */

#include "stream_terminal_impl.hpp"

#endif /* WISP_TERMINAL_STREAM_TERMINAL_HPP */
