/* Transliterated from Ghostty src/terminal/stream.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: named vt_stream.hpp because stream.hpp is still the earlier
 * reimplementation the app runs on; this file replaces it once
 * stream_terminal.zig is ported on top of Terminal.zig.
 *
 * Wisp, differences in shape rather than behavior:
 *   - `handler.vt(comptime action, value)` is `handler.vt(const Action &)`,
 *     with Action a tag plus one field per payload. Upstream specializes per
 *     action at comptime; here the handler switches on the tag.
 *   - The optional `vtRaw` handler hook (used by Ghostty's inspector) is not
 *     carried over. Every handler here takes the fast paths, as a handler
 *     without vtRaw does upstream.
 *   - Continuation tracking is `has_continuation` plus the tracker, since
 *     the tracker owns an allocator and a list rather than being an
 *     optional value. It stays off unless continuation_max_bytes is given,
 *     which is upstream's behavior too.
 *   - log.* and logUnsupportedOnce have no sink yet and are comments.
 *   - SIMD paths are their scalar equivalents (see ../simd/vt.hpp).
 */

#pragma once
#ifndef WISP_TERMINAL_VT_STREAM_HPP
#define WISP_TERMINAL_VT_STREAM_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ansi.hpp"
#include "charsets.hpp"
#include "device_attributes.hpp"
#include "device_status.hpp"
#include "kitty/key.hpp"
#include "modes.hpp"
#include "mouse.hpp"
#include "osc.hpp"
#include "parser.hpp"
#include "sgr.hpp"
#include "utf8_decoder.hpp"
#include "../simd/vt.hpp"
#include "../zigstd/unicode.hpp"
#include "stream_continuation.hpp"

namespace wisp {
namespace terminal {
namespace stream {

typedef ::wisp::terminal::parser::Parser Parser;
typedef ::wisp::terminal::parser::State PState;

/* The possible actions that can be emitted by the Stream
 * function for handling. */
struct Action {
    enum class Key : uint8_t {
        print,
        print_slice,
        print_repeat,
        bell,
        backspace,
        horizontal_tab,
        horizontal_tab_back,
        linefeed,
        carriage_return,
        enquiry,
        invoke_charset,
        cursor_up,
        cursor_down,
        cursor_left,
        cursor_right,
        cursor_col,
        cursor_row,
        cursor_col_relative,
        cursor_row_relative,
        cursor_pos,
        cursor_style,
        erase_display_below,
        erase_display_above,
        erase_display_complete,
        erase_display_scrollback,
        erase_display_scroll_complete,
        erase_line_right,
        erase_line_left,
        erase_line_complete,
        erase_line_right_unless_pending_wrap,
        delete_chars,
        erase_chars,
        insert_lines,
        insert_blanks,
        delete_lines,
        scroll_up,
        scroll_down,
        tab_clear_current,
        tab_clear_all,
        tab_set,
        tab_reset,
        index,
        next_line,
        reverse_index,
        full_reset,
        set_mode,
        reset_mode,
        save_mode,
        restore_mode,
        request_mode,
        request_mode_unknown,
        top_and_bottom_margin,
        left_and_right_margin,
        left_and_right_margin_ambiguous,
        save_cursor,
        restore_cursor,
        modify_key_format,
        mouse_shift_capture,
        protected_mode_off,
        protected_mode_iso,
        protected_mode_dec,
        size_report,
        title_push,
        title_pop,
        xtversion,
        device_attributes,
        device_status,
        kitty_keyboard_query,
        kitty_keyboard_push,
        kitty_keyboard_pop,
        kitty_keyboard_set,
        kitty_keyboard_set_or,
        kitty_keyboard_set_not,
        dcs_hook,
        dcs_put,
        dcs_unhook,
        apc_start,
        apc_end,
        apc_put,
        apc_put_slice,
        end_hyperlink,
        active_status_display,
        decaln,
        window_title,
        report_pwd,
        show_desktop_notification,
        progress_report,
        start_hyperlink,
        clipboard_contents,
        mouse_shape,
        configure_charset,
        set_attribute,
        kitty_color_report,
        color_operation,
        semantic_prompt,
        kitty_clipboard,
        kitty_dnd,
    };
    typedef Key Tag;

    /* Field types */

    /* A run of printable codepoints. This is emitted instead of
     * individual print actions when the stream can decode multiple
     * printable codepoints at once, so handlers can process them in
     * batch with per-run rather than per-codepoint overhead (see
     * Terminal.printSlice). A naive handler can simply loop and
     * handle each codepoint like a print action.
     *
     * The slice is only valid for the duration of the handler call. */
    struct PrintSlice {
        const uint32_t *cps;
        size_t len;
    };

    struct ApcPutSlice {
        const uint8_t *bytes;
        size_t len;
    };

    struct ApcEnd {
        /* False when CAN, SUB, or another aborting transition ended the APC. */
        bool terminated;
    };

    struct InvokeCharset {
        charsets::ActiveSlot bank;
        charsets::Slots charset;
        bool locking;
    };

    struct CursorMovement {
        /* The value of the cursor movement. Depending on the tag of this
         * union this may be an absolute value or it may be a relative
         * value. For example, `cursor_up` is relative, but `cursor_row`
         * is absolute. */
        uint16_t value;
    };

    struct CursorPos {
        uint16_t row;
        uint16_t col;
    };

    struct RawMode {
        uint16_t mode;
        bool ansi;
    };

    struct Margin {
        uint16_t top_left;
        uint16_t bottom_right;
    };

    struct WindowTitle { const char *title; size_t len; };
    struct ReportPwd { const char *url; size_t len; };

    struct ShowDesktopNotification {
        osc::ZStr title;
        osc::ZStr body;
    };

    struct StartHyperlink {
        osc::ZStr uri;
        bool has_id; /* Wisp: id: ?[]const u8 */
        osc::ZStr id;
    };

    struct ClipboardContents {
        uint8_t kind;
        osc::ZStr data;
        osc::Terminator terminator;
    };

    struct ConfigureCharset {
        charsets::Slots slot;
        charsets::Charset charset;
    };

    struct ColorOperation {
        osc::color::Operation op;
        osc::color::List requests;
        osc::Terminator terminator;
    };

    typedef decltype(osc::Command::kitty_color_protocol) KittyColorReport;
    typedef osc::semantic_prompt::Command SemanticPrompt;
    typedef osc::kitty_clipboard_protocol::OSC KittyClipboard;
    typedef decltype(osc::Command::kitty_dnd_protocol) KittyDnd;

    Key tag;

    /* Wisp: payloads, by the upstream field type they carry. */
    uint32_t cp;                          /* print */
    PrintSlice print_slice;               /* print_slice */
    size_t count;                         /* print_repeat, delete_chars,
                                           * erase_chars, insert_lines,
                                           * insert_blanks, delete_lines,
                                           * scroll_up, scroll_down */
    uint16_t value16;                     /* horizontal_tab(_back), title_push,
                                           * title_pop, kitty_keyboard_pop */
    InvokeCharset invoke_charset;
    CursorMovement cursor;                /* cursor_* movements */
    CursorPos cursor_pos;
    ansi::CursorStyle cursor_style;
    bool flag;                            /* erase_* (protected),
                                           * mouse_shift_capture */
    modes::Mode mode;                     /* set/reset/save/restore/request_mode */
    RawMode request_mode_unknown;
    Margin margin;                        /* top_and_bottom/left_and_right */
    ansi::ModifyKeyFormat modify_key_format;
    csi::SizeReportStyle size_report;
    device_attributes::Req device_attributes;
    device_status::Request device_status;
    kitty::KeyFlags kitty_flags;          /* kitty_keyboard_push/set* */
    parser::Action::DCS dcs_hook;
    uint8_t byte;                         /* dcs_put, apc_put */
    ApcEnd apc_end;
    ApcPutSlice apc_put_slice;
    ansi::StatusDisplay active_status_display;
    WindowTitle window_title;
    ReportPwd report_pwd;
    ShowDesktopNotification show_desktop_notification;
    osc::ProgressReport progress_report;
    StartHyperlink start_hyperlink;
    ClipboardContents clipboard_contents;
    mouse::Shape mouse_shape;
    ConfigureCharset configure_charset;
    sgr::Attribute set_attribute;
    KittyColorReport kitty_color_report;
    ColorOperation color_operation;
    SemanticPrompt semantic_prompt;
    KittyClipboard kitty_clipboard;
    KittyDnd kitty_dnd;

    Action() { init(Key::bell); }
    explicit Action(Key k) { init(k); }

private:
    void init(Key k) {
        tag = k;
        cp = 0;
        print_slice.cps = nullptr;
        print_slice.len = 0;
        count = 0;
        value16 = 0;
        invoke_charset.bank = charsets::ActiveSlot::GL;
        invoke_charset.charset = charsets::Slots::G0;
        invoke_charset.locking = false;
        cursor.value = 0;
        cursor_pos.row = 0;
        cursor_pos.col = 0;
        cursor_style = ansi::CursorStyle::default_;
        flag = false;
        mode = modes::Mode::cursor_keys;
        request_mode_unknown.mode = 0;
        request_mode_unknown.ansi = false;
        margin.top_left = 0;
        margin.bottom_right = 0;
        modify_key_format = ansi::ModifyKeyFormat::legacy;
        size_report = csi::SizeReportStyle::csi_14_t;
        device_attributes = device_attributes::Req::primary;
        device_status = device_status::Request::operating_status;
        memset(&dcs_hook, 0, sizeof(dcs_hook));
        byte = 0;
        apc_end.terminated = false;
        apc_put_slice.bytes = nullptr;
        apc_put_slice.len = 0;
        active_status_display = ansi::StatusDisplay::main;
        window_title.title = "";
        window_title.len = 0;
        report_pwd.url = "";
        report_pwd.len = 0;
        start_hyperlink.has_id = false;
        clipboard_contents.kind = 0;
        clipboard_contents.terminator = osc::Terminator::st;
        mouse_shape = mouse::Shape::default_;
        configure_charset.slot = charsets::Slots::G0;
        configure_charset.charset = charsets::Charset::utf8;
        memset(&kitty_color_report, 0, sizeof(kitty_color_report));
        color_operation.op = osc::color::Operation::osc_4;
        color_operation.terminator = osc::Terminator::st;
        memset(&kitty_dnd, 0, sizeof(kitty_dnd));
    }
};

/* Wisp: std.unicode.utf8ValidateSlice lives in zigstd so the kitty
 * clipboard protocol can share it. */
using ::wisp::zigstd::utf8ValidateSlice;

/* Returns a type that can process a stream of tty control characters.
 * This will call the `vt` function on type T with the following signature:
 *
 *   fn(comptime action: Action.Key, value: Action.Value(action)) void
 *
 * Wisp: `void vt(const Action &action)`.
 *
 * The handler type T can choose to react to whatever actions it cares
 * about in its pursuit of implementing a terminal emulator or other
 * functionality.
 *
 * Note that printable text is delivered via `print_slice` actions
 * (runs of codepoints) whenever the stream can decode multiple
 * codepoints at once, and via `print` actions otherwise. Handlers
 * that care about text must handle both.
 *
 * The Handler type must also have a `deinit` function. */
template <typename H>
struct Stream {
    typedef H Handler;

    Handler handler;
    Parser parser;
    UTF8Decoder utf8decoder;

    /* Wisp: `?Tracker` is the tracker plus a flag, since Tracker owns an
     * allocator and a list rather than being an optional value. */
    bool has_continuation;
    stream_continuation::Tracker continuation;

    struct Options {
        /* Allocator to use. If this is not set then the stream
         * will be fully allocation free. There are some operations
         * that will be dropped in this case such as OSC 52 clipboard
         * ops.
         *
         * Wisp: ?Allocator is a flag. */
        bool allocator;

        /* Maximum size in bytes of the continuation suffix. If this is
         * null or zero then continuation tracking is disabled. This is
         * only applied when `allocator` is non-null; without an allocator
         * continuation tracking is disabled. Feeding this continuation
         * suffix into an equivalent stream at ground reconstructs the
         * unfinished state without repeating committed terminal effects.
         * Continuation tracking is only supported by TerminalStream. */
        stream_continuation::Maybe<size_t> continuation_max_bytes; /* = null */

        Options() : allocator(false), continuation_max_bytes() {}
    };

    /* Initialize a stream. Without an allocator, operations that require
     * heap allocation are dropped.
     *
     * As a concrete example of something that requires heap allocation,
     * consider OSC 52 (clipboard operations) which can be arbitrarily
     * large.
     *
     * This takes ownership of the handler and will call deinit
     * when the stream is deinitialized.
     *
     * Wisp: Stream is not copyable (it holds the parser), so init is a
     * constructor that takes the handler by value. */
    explicit Stream(const Handler &h, Options options = Options())
        : handler(h), parser(), utf8decoder(), has_continuation(false), continuation() {
        // Initialize the parser
        if (options.allocator) parser.osc_parser.alloc = true;

        // Initialize the continuation tracker if one is requested.
        if (options.allocator && options.continuation_max_bytes.has &&
            options.continuation_max_bytes.value > 0) {
            continuation = stream_continuation::Tracker::init(zigstd::c_allocator(),
                                                              options.continuation_max_bytes.value);
            has_continuation = true;
        }
    }

    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;

    void deinit() {
        if (has_continuation) continuation.deinit();
        parser.osc_parser.deinit();
        handler.deinit();
    }

    /* Wisp: the error set of writeContinuation. */
    enum class ContinuationError : uint8_t {
        none,
        ContinuationDisabled,
        ContinuationUnavailable,
    };

    /* Write the current continuation suffix directly to a caller-owned
     * writer. The caller must pause and serialize access to this Stream. */
    ContinuationError writeContinuation(std::string *writer) const {
        if (!has_continuation) return ContinuationError::ContinuationDisabled;
        if (continuation.broken) return ContinuationError::ContinuationUnavailable;
        continuation.write(writer);
        return ContinuationError::none;
    }

    /* True when no continuation suffix is needed to reproduce the
     * stream's current parsing state. */
    bool ground() const {
        /* Parser ground alone is not sufficient because the UTF-8
         * decoder may have some state. */
        return parser.state == PState::ground && utf8decoder.state == 0;
    }

    /* Update the continuation suffix after one complete feed call.
     * Must only be called when tracking is enabled. */
    void trackContinuation(const uint8_t *input, size_t len) {
        /* If we're in a ground state, we have no continuation suffix
         * to track by definition. */
        if (ground()) {
            continuation.reset();
            return;
        }

        /* Retain the part of this feed needed to replay the unfinished
         * state. When the parser is grounded here, the feed must have
         * ended inside a UTF-8 codepoint instead, because the ground
         * check above covers both state machines. */
        continuation.append(parser.state != PState::ground
                                ? stream_continuation::Tracker::Pending::vt
                                : stream_continuation::Tracker::Pending::utf8,
                            input, len);
    }

    /* Process a string of characters. */
    void nextSlice(const uint8_t *input, size_t len) {
        nextSliceUntracked(input, len);

        /* Continuation tracking is opt-in and this branch predicts
         * perfectly, so disabled streams pay nothing else here. */
        if (has_continuation) trackContinuation(input, len);
    }
    void nextSlice(const char *s, size_t len) { nextSlice((const uint8_t *)s, len); }
    void nextSlice(const char *s) { nextSlice((const uint8_t *)s, strlen(s)); }

    /* Process a string of characters, but only the shortest prefix
     * needed to reach the ground state.
     *
     * The ground state is when the stream isn't in the middle of any
     * type of sequence: UTF-8, ESC, CSI, OSC, etc. It is the stateless
     * point of the stream.
     *
     * If the stream is already at ground then this consumes nothing
     * and returns zero. A non-null return is the number of bytes consumed
     * before reaching ground, including the byte that reaches it. A null
     * return means the full slice was consumed without reaching ground.
     *
     * Wisp: ?usize is the bool return plus *consumed. */
    bool nextSliceUntilGround(const uint8_t *input, size_t len, size_t *consumed) {
        const bool reached = nextSliceUntilGroundUntracked(input, len, consumed);
        const size_t consumed_len = reached ? *consumed : len;
        if (has_continuation && consumed_len > 0) trackContinuation(input, consumed_len);
        return reached;
    }

    /* Like nextSlice but takes one byte and is necessarily a scalar
     * operation that can't use SIMD. Prefer nextSlice if you can and
     * try to get multiple bytes at once. */
    void next(uint8_t c) {
        nextUntracked(c);
        if (has_continuation) trackContinuation(&c, 1);
    }

private:
    bool nextSliceUntilGroundUntracked(const uint8_t *input, size_t len, size_t *consumed) {
        if (ground()) {
            *consumed = 0;
            return true;
        }

        /* Process UTF-8 if we're within that state. */
        size_t offset = 0;
        while (utf8decoder.state != 0) {
            if (offset >= len) return false;
            nextUtf8(input[offset]);
            offset += 1;
        }

        /* Process non-UTF-8 */
        if (parser.state != PState::ground) {
            offset += consumeUntilGround(input + offset, len - offset);
        }

        if (ground()) {
            *consumed = offset;
            return true;
        }
        return false;
    }

    void nextSliceUntracked(const uint8_t *input, size_t len) {
        /* This is the maximum number of codepoints we can decode
         * at one time for this function call. This is somewhat arbitrary
         * so if someone can demonstrate a better number then we can switch. */
        static const size_t cp_buf_len = 4096;
        uint32_t cp_buf[cp_buf_len];

        /* Split the input into chunks that fit into cp_buf. */
        size_t i = 0;
        while (true) {
            const size_t n = cp_buf_len < len - i ? cp_buf_len : len - i;
            nextSliceCapped(input + i, n, cp_buf);
            i += n;
            if (i >= len) break;
        }
    }

    void nextSliceCapped(const uint8_t *input, size_t len, uint32_t *cp_buf) {
        size_t offset = 0;

        /* If the scalar UTF-8 decoder was in the middle of processing
         * a code sequence, we continue until it's not. */
        while (utf8decoder.state != 0) {
            if (offset >= len) return;
            nextUtf8(input[offset]);
            offset += 1;
        }
        if (offset >= len) return;

        /* If we're not in the ground state then we process until
         * we are. This can happen if the last chunk of input put us
         * in the middle of a control sequence. */
        offset += consumeUntilGround(input + offset, len - offset);
        if (offset >= len) return;
        offset += consumeAllEscapes(input + offset, len - offset);

        /* If we're in the ground state then we can use SIMD to process
         * input until we see an ESC (0x1B), since all other characters
         * up to that point are just UTF-8. */
        while (parser.state == PState::ground && offset < len) {
            const simd::vt::DecodeResult res =
                simd::vt::utf8DecodeUntilControlSeq(input + offset, len - offset, cp_buf);
            const uint32_t *cps = cp_buf;
            const size_t cps_len = res.decoded;

            /* Hand runs of printable codepoints to the handler as
             * print_slice actions so it can process them with
             * per-run rather than per-codepoint overhead. */
            size_t i = 0;
            while (i < cps_len) {
                const uint32_t cp = cps[i];
                /* C0 and UTF-8-decoded C1 controls never enter
                 * printable runs. A codepoint is one of those
                 * exactly when it has no bit set outside 0x9F:
                 * bits 0-4 and bit 7 cover 0x00-0x1F and
                 * 0x80-0x9F and nothing else, so a single
                 * AND-test classifies both ranges. */
                if ((cp & ~(uint32_t)0x9F) == 0) {
                    if (cp <= 0x1F) {
                        /* C0 controls execute rather than print. */
                        execute((uint8_t)cp);
                    } else {
                        /* C1 controls decoded from UTF-8 are ignored.
                         * logUnsupportedOnce("ignoring UTF-8-decoded C1 controls") */
                    }
                    i += 1;
                    continue;
                }

                /* Find the end of the printable run. */
                size_t end = i + 1;
                while (end < cps_len && (cps[end] & ~(uint32_t)0x9F) != 0) end += 1;
                Action a(Action::Key::print_slice);
                a.print_slice.cps = cps + i;
                a.print_slice.len = end - i;
                handler.vt(a);
                i = end;
            }
            /* Consume the bytes we just processed. */
            offset += res.consumed;

            if (offset >= len) return;

            /* If our offset is NOT an escape then we must have a
             * partial UTF-8 sequence. In that case, we pass it off
             * to the scalar parser. */
            if (input[offset] != 0x1B) {
                for (size_t k = offset; k < len; k++) nextUtf8(input[k]);
                return;
            }

            /* Process control sequences until we run out. */
            offset += consumeAllEscapes(input + offset, len - offset);
        }
    }

    /* Parses back-to-back escape sequences until none are left.
     * Returns the number of bytes consumed from the provided input.
     *
     * Expects input to start with 0x1B, use consumeUntilGround first
     * if the stream may be in the middle of an escape sequence. */
    size_t consumeAllEscapes(const uint8_t *input, size_t len) {
        size_t offset = 0;
        while (input[offset] == 0x1B) {
            parser.state = PState::escape;
            parser.clear();
            offset += 1;
            offset += consumeUntilGround(input + offset, len - offset);
            if (offset >= len) return len;
        }
        return offset;
    }

    /* Parses escape sequences until the parser reaches the ground state.
     * Returns the number of bytes consumed from the provided input. */
    size_t consumeUntilGround(const uint8_t *input, size_t len) {
        size_t offset = 0;
        while (parser.state != PState::ground) {
            if (offset >= len) return len;

            /* Fast path for CSI entry: "ESC [" is by far the most
             * common escape sequence prefix, so handle the '[' and
             * the byte that follows it here rather than paying a
             * nextNonUtf8 call for each. */
            if (parser.state == PState::escape && input[offset] == '[') {
                parser.state = PState::csi_entry;
                offset += 1;
                continue;
            }

            if (parser.state == PState::csi_entry) {
                if (csiEntryByte(input[offset])) {
                    offset += 1;
                    continue;
                }
            }

            /* Bulk-consume CSI parameter bytes. */
            if (parser.state == PState::csi_param) {
                offset += consumeCsiParams(input + offset, len - offset);
                if (offset >= len) return len;
                /* If we're still in csi_param then the next byte
                 * isn't a parameter byte; let nextNonUtf8 below
                 * handle it. Otherwise re-check our state. */
                if (parser.state != PState::csi_param) continue;
            }

            /* Bulk-consume APC string bytes into a single slice.
             * APC payloads (e.g. Kitty graphics) can be megabytes
             * of base64 data, so per-byte dispatch is far too slow. */
            if (parser.state == PState::sos_pm_apc_string) {
                offset += consumeApcString(input + offset, len - offset);
                if (offset >= len) return len;

                /* Fast-path normal string termination. This matches
                 * Parser.next's exit and entry actions while avoiding
                 * the generic action loop for every completed APC. */
                if (input[offset] == 0x1B) {
                    parser.clear();
                    parser.state = PState::escape;
                    Action a(Action::Key::apc_end);
                    a.apc_end.terminated = true;
                    handler.vt(a);
                    offset += 1;
                    continue;
                } else if (input[offset] == 0x9C) {
                    parser.state = PState::ground;
                    Action a(Action::Key::apc_end);
                    a.apc_end.terminated = true;
                    handler.vt(a);
                    offset += 1;
                    continue;
                }

                /* Aborting transitions need the scalar path so the
                 * handler can distinguish them from terminators. */
            }

            /* Bulk-consume OSC string bytes into the OSC parser.
             * OSC payloads (e.g. OSC 52 clipboard operations and
             * the kitty clipboard protocol) can be megabytes of
             * base64 data, so per-byte dispatch is far too slow. */
            if (parser.state == PState::osc_string) {
                offset += consumeOscString(input + offset, len - offset);
                if (offset >= len) return len;

                /* Fast-path normal string termination. This matches
                 * Parser.next's exit and entry actions while avoiding
                 * the generic action loop for every completed OSC. */
                if (input[offset] == 0x1B) {
                    /* The sequence should be terminated by a full
                     * ST ("ESC \"); the "\" that should follow is
                     * dispatched as a normal escape sequence. */
                    osc::Command *cmd = parser.osc_parser.end((uint8_t)0x1B);
                    if (cmd) oscDispatch(*cmd);
                    parser.clear();
                    parser.state = PState::escape;
                    offset += 1;
                    continue;
                } else if (input[offset] == 0x07) {
                    /* BEL terminates the string directly. */
                    osc::Command *cmd = parser.osc_parser.end((uint8_t)0x07);
                    if (cmd) oscDispatch(*cmd);
                    parser.state = PState::ground;
                    offset += 1;
                    continue;
                }
                /* CAN/SUB abort and ignored C0 bytes go
                 * through the state machine below. */
            }

            nextNonUtf8(input[offset]);
            offset += 1;
        }
        return offset;
    }

    /* Fast path for a byte in the csi_entry state, the state right
     * after "ESC [". Virtually every CSI sequence spends exactly
     * one byte in this state, on either a digit, a private marker,
     * or a final byte. Returns true if the byte was fully handled;
     * false means the caller must process it through the general
     * state machine. */
    bool csiEntryByte(uint8_t c) {
        if (c >= '0' && c <= '9') {
            /* First parameter digit. */
            parser.state = PState::csi_param;
            /* param_acc is zero (cleared on escape entry)
             * so accumulating is just the digit value. */
            parser.param_acc = (uint16_t)(c - '0');
            parser.param_acc_idx = 1;
        } else if (c == ';') {
            /* An empty first parameter. */
            parser.state = PState::csi_param;
            parser.params[0] = 0;
            parser.params_idx = 1;
        } else if (c >= 0x3C && c <= 0x3F) {
            /* Private marker (e.g. '?' in "ESC [ ? 2004 h"). */
            parser.state = PState::csi_param;
            parser.collect(c);
        } else if (c >= 0x40 && c <= 0x7E) {
            /* A final byte: a parameterless CSI. */
            csiDispatchFinal(c);
        } else {
            /* Defer to the state machine for anything else
             * (C0 controls, intermediates, colon). */
            return false;
        }
        return true;
    }

    /* Wisp: *|= 10 and +|= digit on u16. */
    static uint16_t accumulate(uint16_t acc, uint8_t c) {
        uint32_t v = (uint32_t)acc * 10;
        if (v > 0xFFFF) v = 0xFFFF;
        v += (uint32_t)(c - '0');
        if (v > 0xFFFF) v = 0xFFFF;
        return (uint16_t)v;
    }

    /* Bulk-consume CSI parameter bytes (digits and separators)
     * and, if reached, the final byte (dispatching the CSI).
     * Returns the number of bytes consumed. Stops at the first
     * byte that isn't handled here, leaving the parser in the
     * csi_param state so the caller can process that byte. */
    size_t consumeCsiParams(const uint8_t *input, size_t len) {
        Parser *p = &parser;

        /* Accumulate parser state in locals for the hot loop. */
        uint16_t acc = p->param_acc;
        uint8_t acc_idx = p->param_acc_idx;
        uint8_t idx = p->params_idx;

        size_t offset = 0;
        while (offset < len) {
            const uint8_t c = input[offset];
            if (c >= '0' && c <= '9') {
                /* A parameter digit. */
                if (idx < parser::MAX_PARAMS) {
                    acc = accumulate(acc, c);
                    acc_idx |= 1;
                }
                offset += 1;
            } else if (c == ':' || c == ';') {
                /* A parameter separator. */
                if (idx < parser::MAX_PARAMS) {
                    p->params[idx] = acc;
                    if (c == ':') p->params_sep.set(idx);
                    idx += 1;
                    acc = 0;
                    acc_idx = 0;
                }
                offset += 1;
            } else if (c >= 0x40 && c <= 0x7E) {
                /* A final byte: dispatch the CSI. */
                p->param_acc = acc;
                p->param_acc_idx = acc_idx;
                p->params_idx = idx;
                csiDispatchFinal(c);
                return offset + 1;
            } else {
                /* Anything else (C0 controls, intermediates, etc.)
                 * is handled by the caller. */
                break;
            }
        }

        p->param_acc = acc;
        p->param_acc_idx = acc_idx;
        p->params_idx = idx;
        return offset;
    }

    /* Bulk-consume APC string bytes and dispatch them as a single
     * apc_put_slice action. Returns the number of bytes consumed.
     * Stops at the first byte that is not an apc_put byte in the
     * parse table, leaving it for the caller to process through
     * the state machine. CAN, SUB, ESC, and most C1 bytes exit
     * or abort the string state; 0xA0-0xFF are ignored by the
     * table (not payload), so they can't be bulk-consumed either. */
    size_t consumeApcString(const uint8_t *input, size_t len) {
        size_t end = 0;
        while (end < len) {
            const uint8_t b = input[end];
            /* Not apc_put bytes: CAN/SUB/ESC and most C1 exit
             * or abort the state; 0xA0-0xFF are ignored by it. */
            if (b == 0x18 || b == 0x1A || b == 0x1B || b >= 0x80) break;
            /* Everything else is an apc_put byte. */
            end += 1;
        }

        if (end > 0) {
            Action a(Action::Key::apc_put_slice);
            a.apc_put_slice.bytes = input;
            a.apc_put_slice.len = end;
            handler.vt(a);
        }
        return end;
    }

    /* Bulk-consume OSC string bytes into the OSC parser. Returns
     * the number of bytes consumed. Stops at the first byte that
     * is not an osc_put byte in the parse table, leaving it for
     * the caller to process through the state machine. Every byte
     * >= 0x20 is an osc_put byte; bytes below either terminate or
     * abort the string (BEL, CAN, SUB, ESC) or are ignored. */
    size_t consumeOscString(const uint8_t *input, size_t len) {
        size_t end = 0;
        while (end < len) {
            /* Not osc_put bytes: BEL/CAN/SUB/ESC terminate or
             * abort the state; other C0 bytes are ignored by it. */
            if (input[end] <= 0x1F) break;
            /* Everything else is an osc_put byte. */
            end += 1;
        }

        if (end > 0) parser.osc_parser.nextSlice(input, end);
        return end;
    }

    void nextUntracked(uint8_t c) {
        /* The scalar path can be responsible for decoding UTF-8. */
        if (parser.state == PState::ground) {
            nextUtf8(c);
            return;
        }

        nextNonUtf8(c);
    }

    /* Process the next byte and print as necessary.
     *
     * This assumes we're in the UTF-8 decoding state. If we may not
     * be in the UTF-8 decoding state call nextSlice or next. */
    void nextUtf8(uint8_t c) {
        /* assert(self.parser.state == .ground) */
        const UTF8Decoder::Result res = utf8decoder.next(c);
        if (res.has_codepoint) handleCodepoint(res.codepoint);
        if (!res.consumed) {
            /* We optimize for the scenario where the text being
             * printed in the terminal ISN'T full of ill-formed
             * UTF-8 sequences. */
            const UTF8Decoder::Result retry = utf8decoder.next(c);
            /* It should be impossible for the decoder
             * to not consume the byte twice in a row.
             * assert(retry[1] == true) */
            if (retry.has_codepoint) handleCodepoint(retry.codepoint);
        }
    }

    /* To be called whenever the utf-8 decoder produces a codepoint.
     *
     * This function is abstracted this way to handle the case where
     * the decoder emits a 0x1B after rejecting an ill-formed sequence. */
    void handleCodepoint(uint32_t c) {
        /* C0 control or a C1 control decoded from UTF-8: exactly
         * the codepoints with no bit set outside 0x9F (bits 0-4
         * and bit 7 cover 0x00-0x1F and 0x80-0x9F and nothing
         * else), so the printable fast path stays a single
         * AND-test. */
        if ((c & ~(uint32_t)0x9F) == 0) {
            /* ESC */
            if (c == 0x1B) {
                parser.state = PState::escape;
                parser.clear();
                return;
            }

            /* Ignore C1 that came via UTF-8 decoding, matching xterm. */
            if (c > 0x1F) {
                /* logUnsupportedOnce("ignoring UTF-8-decoded C1 controls") */
                return;
            }

            execute((uint8_t)c);
            return;
        }
        print(c);
    }

    /* Process the next character and call any callbacks if necessary.
     *
     * This assumes that we're not in the UTF-8 decoding state. If
     * we may be in the UTF-8 decoding state call nextSlice or next. */
    void nextNonUtf8(uint8_t c) {
        /* assert(self.parser.state != .ground) */

        /* Fast path for CSI entry. */
        if (parser.state == PState::escape && c == '[') {
            parser.state = PState::csi_entry;
            return;
        }

        /* Fast path for CSI params. */
        if (parser.state == PState::csi_param) {
            /* csi_param is the most common parser state
             * other than ground by a fairly wide margin.
             *
             * ref: https://github.com/qwerasd205/asciinema-stats */
            bool handled = true;
            if (c <= 0x0F) {
                /* A C0 escape (yes, this is valid): */
                execute(c);
            } else if ((c >= 0x10 && c <= 0x17) || c == 0x19 || (c >= 0x1C && c <= 0x1F)) {
                /* We ignore C0 escapes > 0xF since execute
                 * doesn't have processing for them anyway: */
            } else if (c == 0x18 || c == 0x1A) {
                /* We don't currently have any handling for
                 * 0x18 or 0x1A, but they should still move
                 * the parser state to ground. */
                parser.state = PState::ground;
            } else if (c >= '0' && c <= '9') {
                /* A parameter digit: */
                if (parser.params_idx < parser::MAX_PARAMS) {
                    parser.param_acc = accumulate(parser.param_acc, c);
                    /* The parser's CSI param action uses param_acc_idx
                     * to decide if there's a final param that needs to
                     * be consumed or not, but it doesn't matter really
                     * what it is as long as it's not 0. */
                    parser.param_acc_idx |= 1;
                }
            } else if (c == ':' || c == ';') {
                /* A parameter separator: */
                if (parser.params_idx < parser::MAX_PARAMS) {
                    parser.params[parser.params_idx] = parser.param_acc;
                    if (c == ':') parser.params_sep.set(parser.params_idx);
                    parser.params_idx += 1;

                    parser.param_acc = 0;
                    parser.param_acc_idx = 0;
                }
            } else if (c >= 0x40 && c <= 0x7E) {
                /* A final byte: dispatch the CSI directly. */
                csiDispatchFinal(c);
            } else if (c == 0x7F) {
                /* Explicitly ignored: */
            } else {
                /* Defer to the state machine to
                 * handle any other characters: */
                handled = false;
            }
            if (handled) return;
        }

        /* Fast path for CSI entry, the state right after "ESC [". */
        if (parser.state == PState::csi_entry) {
            if (csiEntryByte(c)) return;
        }

        const parser::Next actions = parser.next(c);

        for (int k = 0; k < 3; k++) {
            if (!actions.has(k)) continue;
            const parser::Action &action = actions[k];

            typedef parser::Action::Tag PT;
            switch (action.tag) {
                case PT::print: print(action.print); break;
                case PT::execute: execute(action.byte); break;
                case PT::csi_dispatch: csiDispatch(action.csi_dispatch); break;
                case PT::esc_dispatch: escDispatch(action.esc_dispatch); break;
                case PT::osc_dispatch: oscDispatch(action.osc_dispatch); break;
                case PT::dcs_hook: {
                    Action a(Action::Key::dcs_hook);
                    a.dcs_hook = action.dcs_hook;
                    handler.vt(a);
                    break;
                }
                case PT::dcs_put: {
                    Action a(Action::Key::dcs_put);
                    a.byte = action.byte;
                    handler.vt(a);
                    break;
                }
                case PT::dcs_unhook: handler.vt(Action(Action::Key::dcs_unhook)); break;
                case PT::apc_start: handler.vt(Action(Action::Key::apc_start)); break;
                case PT::apc_put: {
                    Action a(Action::Key::apc_put);
                    a.byte = action.byte;
                    handler.vt(a);
                    break;
                }
                case PT::apc_end: {
                    Action a(Action::Key::apc_end);
                    a.apc_end.terminated = c == 0x1B || c == 0x9C;
                    handler.vt(a);
                    break;
                }
            }
        }
    }

    /* Finalize and dispatch a CSI directly from parser state for
     * the fast paths in nextNonUtf8, without going through
     * Parser.next. This must match the behavior of the parser's
     * csi_dispatch action. */
    void csiDispatchFinal(uint8_t c) {
        Parser *p = &parser;
        p->state = PState::ground;

        /* Ignore sequences with too many parameters, matching the
         * parser's behavior of dropping the dispatch entirely. */
        if (p->params_idx >= parser::MAX_PARAMS) return;

        /* Finalize the last parameter if we have one. */
        if (p->param_acc_idx > 0) {
            p->params[p->params_idx] = p->param_acc;
            p->params_idx += 1;
        }

        parser::Action::CSI action;
        action.intermediates = p->intermediates;
        action.intermediates_len = p->intermediates_idx;
        action.params = p->params;
        action.params_len = p->params_idx;
        action.params_sep = p->params_sep;
        action.final_ = c;

        /* We only allow colon or mixed separators for the 'm' command. */
        if (c != 'm' && p->params_sep.count() > 0) {
            /* log.warn("CSI colon or mixed separators only allowed for 'm' command") */
            return;
        }

        csiDispatch(action);
    }

    void print(uint32_t c) {
        Action a(Action::Key::print);
        a.cp = c;
        handler.vt(a);
    }

    void vt0(Action::Key k) { handler.vt(Action(k)); }

    void invokeCharset(charsets::ActiveSlot bank, charsets::Slots charset, bool locking) {
        Action a(Action::Key::invoke_charset);
        a.invoke_charset.bank = bank;
        a.invoke_charset.charset = charset;
        a.invoke_charset.locking = locking;
        handler.vt(a);
    }

    void execute(uint8_t c) {
        /* If the character is > 0x7F, it's a C1 (8-bit) control,
         * which is strictly equivalent to `ESC` plus `c - 0x40`. */
        if (c > 0x7F) {
            /* log.info("executing C1 0x{x} as ESC {c}") */
            parser::Action::ESC esc;
            esc.intermediates = nullptr;
            esc.intermediates_len = 0;
            esc.final_ = (uint8_t)(c - 0x40);
            escDispatch(esc);
            return;
        }

        typedef ansi::C0 C0;
        switch ((C0)c) {
            /* We ignore SOH/STX: https://github.com/microsoft/terminal/issues/10786 */
            case C0::NUL: case C0::SOH: case C0::STX: break;

            case C0::ENQ: vt0(Action::Key::enquiry); break;
            case C0::BEL: vt0(Action::Key::bell); break;
            case C0::BS: vt0(Action::Key::backspace); break;
            case C0::HT: {
                Action a(Action::Key::horizontal_tab);
                a.value16 = 1;
                handler.vt(a);
                break;
            }
            case C0::LF: case C0::VT: case C0::FF: vt0(Action::Key::linefeed); break;
            case C0::CR: vt0(Action::Key::carriage_return); break;
            case C0::SO: invokeCharset(charsets::ActiveSlot::GL, charsets::Slots::G1, false); break;
            case C0::SI: invokeCharset(charsets::ActiveSlot::GL, charsets::Slots::G0, false); break;

            default: /* logUnsupportedOnce("invalid C0 character, ignoring") */ break;
        }
    }

    /* Wisp: the recurring `switch (input.params.len) { 0 => 1, 1 =>
     * input.params[0], else => { log.warn(...); return; } }`. False is the
     * `else` arm. */
    static bool param0or1(const parser::Action::CSI &input, uint16_t *out) {
        switch (input.params_len) {
            case 0: *out = 1; return true;
            case 1: *out = input.params[0]; return true;
            default: return false;
        }
    }

    void cursorMove(Action::Key k, const parser::Action::CSI &input) {
        uint16_t v;
        if (!param0or1(input, &v)) {
            /* log.warn("invalid cursor ... command") */
            return;
        }
        Action a(k);
        a.cursor.value = v;
        handler.vt(a);
    }

    void countAction(Action::Key k, const parser::Action::CSI &input) {
        uint16_t v;
        if (!param0or1(input, &v)) {
            /* log.warn("invalid ... command") */
            return;
        }
        Action a(k);
        a.count = v;
        handler.vt(a);
    }

    void value16Action(Action::Key k, uint16_t v) {
        Action a(k);
        a.value16 = v;
        handler.vt(a);
    }

    void modeAction(Action::Key k, modes::Mode m) {
        Action a(k);
        a.mode = m;
        handler.vt(a);
    }

    void flagAction(Action::Key k, bool v) {
        Action a(k);
        a.flag = v;
        handler.vt(a);
    }

    void csiDispatch(const parser::Action::CSI &input) {
        /* The branch hints here are based on real world data
         * which indicates that the most common CSI finals are:
         *
         * 1. m
         * 2. H
         * 3. K
         * 4. A
         * 5. C
         * 6. X
         * 7. l
         * 8. h
         * 9. r
         *
         * Together, these 9 finals make up about 96% of all
         * CSI sequences encountered in real world scenarios.
         *
         * ref: https://github.com/qwerasd205/asciinema-stats */

        typedef Action::Key K;
        const size_t ilen = input.intermediates_len;
        const uint8_t *im = input.intermediates;

        switch (input.final_) {
            /* CUU - Cursor Up */
            case 'A': case 'k':
                if (ilen == 0) cursorMove(K::cursor_up, input);
                /* else log.warn("ignoring unimplemented CSI A with intermediates") */
                break;

            /* CUD - Cursor Down */
            case 'B':
                if (ilen == 0) cursorMove(K::cursor_down, input);
                break;

            /* CUF - Cursor Right */
            case 'C':
                if (ilen == 0) cursorMove(K::cursor_right, input);
                break;

            /* CUB - Cursor Left */
            case 'D': case 'j':
                if (ilen == 0) cursorMove(K::cursor_left, input);
                break;

            /* CNL - Cursor Next Line */
            case 'E':
                if (ilen == 0) {
                    uint16_t v;
                    if (!param0or1(input, &v)) return;
                    Action a(K::cursor_down);
                    a.cursor.value = v;
                    handler.vt(a);
                    vt0(K::carriage_return);
                }
                break;

            /* CPL - Cursor Previous Line */
            case 'F':
                if (ilen == 0) {
                    uint16_t v;
                    if (!param0or1(input, &v)) return;
                    Action a(K::cursor_up);
                    a.cursor.value = v;
                    handler.vt(a);
                    vt0(K::carriage_return);
                }
                break;

            /* HPA - Cursor Horizontal Position Absolute
             * TODO: test */
            case 'G': case '`':
                if (ilen == 0) cursorMove(K::cursor_col, input);
                break;

            /* CUP - Set Cursor Position.
             * TODO: test */
            case 'H': case 'f':
                if (ilen == 0) {
                    Action a(K::cursor_pos);
                    switch (input.params_len) {
                        case 0: a.cursor_pos.row = 1; a.cursor_pos.col = 1; break;
                        case 1: a.cursor_pos.row = input.params[0]; a.cursor_pos.col = 1; break;
                        case 2: a.cursor_pos.row = input.params[0]; a.cursor_pos.col = input.params[1]; break;
                        default: /* log.warn("invalid CUP command") */ return;
                    }
                    handler.vt(a);
                }
                break;

            /* CHT - Cursor Horizontal Tabulation */
            case 'I':
                if (ilen == 0) {
                    uint16_t v;
                    if (!param0or1(input, &v)) return;
                    value16Action(K::horizontal_tab, v);
                }
                break;

            /* Erase Display */
            case 'J': {
                bool protected_;
                if (ilen == 0) protected_ = false;
                else if (ilen == 1 && im[0] == '?') protected_ = true;
                else {
                    /* log.warn("invalid erase display command") */
                    return;
                }

                csi::EraseDisplay mode;
                if (input.params_len == 0) mode = csi::EraseDisplay::below;
                else if (input.params_len == 1) {
                    /* std.enums.fromInt(csi.EraseDisplay, input.params[0]) */
                    switch (input.params[0]) {
                        case 0: case 1: case 2: case 3: case 22:
                            mode = (csi::EraseDisplay)input.params[0];
                            break;
                        default: return; /* log.warn("invalid erase display command") */
                    }
                } else return;

                switch (mode) {
                    case csi::EraseDisplay::below: flagAction(K::erase_display_below, protected_); break;
                    case csi::EraseDisplay::above: flagAction(K::erase_display_above, protected_); break;
                    case csi::EraseDisplay::complete: flagAction(K::erase_display_complete, protected_); break;
                    case csi::EraseDisplay::scrollback: flagAction(K::erase_display_scrollback, protected_); break;
                    case csi::EraseDisplay::scroll_complete: flagAction(K::erase_display_scroll_complete, protected_); break;
                }
                break;
            }

            /* Erase Line */
            case 'K': {
                bool protected_;
                if (ilen == 0) protected_ = false;
                else if (ilen == 1 && im[0] == '?') protected_ = true;
                else return; /* log.warn("invalid erase line command") */

                csi::EraseLine mode;
                if (input.params_len == 0) mode = csi::EraseLine::right;
                else if (input.params_len == 1 && input.params[0] < 3) mode = (csi::EraseLine)input.params[0];
                else return; /* log.warn("invalid erase line command") */

                switch (mode) {
                    case csi::EraseLine::right: flagAction(K::erase_line_right, protected_); break;
                    case csi::EraseLine::left: flagAction(K::erase_line_left, protected_); break;
                    case csi::EraseLine::complete: flagAction(K::erase_line_complete, protected_); break;
                    case csi::EraseLine::right_unless_pending_wrap:
                        flagAction(K::erase_line_right_unless_pending_wrap, protected_);
                        break;
                    default: /* log.warn("invalid erase line mode") */ break;
                }
                break;
            }

            /* IL - Insert Lines
             * TODO: test */
            case 'L': if (ilen == 0) countAction(K::insert_lines, input); break;

            /* DL - Delete Lines
             * TODO: test */
            case 'M': if (ilen == 0) countAction(K::delete_lines, input); break;

            /* Delete Character (DCH) */
            case 'P': if (ilen == 0) countAction(K::delete_chars, input); break;

            /* Scroll Up (SD) */
            case 'S': if (ilen == 0) countAction(K::scroll_up, input); break;

            /* Scroll Down (SD) */
            case 'T': if (ilen == 0) countAction(K::scroll_down, input); break;

            /* Cursor Tabulation Control */
            case 'W':
                if (ilen == 0) {
                    if (input.params_len == 0 ||
                        (input.params_len == 1 && input.params[0] == 0)) {
                        vt0(K::tab_set);
                        return;
                    }

                    if (input.params_len == 1) {
                        switch (input.params[0]) {
                            case 2: vt0(K::tab_clear_current); break;
                            case 5: vt0(K::tab_clear_all); break;
                            default: break;
                        }
                    }

                    /* log.warn("invalid cursor tabulation control") */
                    return;
                } else if (ilen == 1) {
                    if (im[0] == '?' && input.params_len == 1 && input.params[0] == 5) {
                        vt0(K::tab_reset);
                    }
                    /* else log.warn("invalid cursor tabulation control") */
                }
                break;

            /* Erase Characters (ECH) */
            case 'X': if (ilen == 0) countAction(K::erase_chars, input); break;

            /* CHT - Cursor Horizontal Tabulation Back */
            case 'Z':
                if (ilen == 0) {
                    uint16_t v;
                    if (!param0or1(input, &v)) return;
                    value16Action(K::horizontal_tab_back, v);
                }
                break;

            /* HPR - Cursor Horizontal Position Relative */
            case 'a': if (ilen == 0) cursorMove(K::cursor_col_relative, input); break;

            /* Repeat Previous Char (REP) */
            case 'b': if (ilen == 0) countAction(K::print_repeat, input); break;

            /* c - Device Attributes (DA1) */
            case 'c': {
                bool have = true;
                device_attributes::Req req = device_attributes::Req::primary;
                if (ilen == 0) req = device_attributes::Req::primary;
                else if (ilen == 1 && im[0] == '>') req = device_attributes::Req::secondary;
                else if (ilen == 1 && im[0] == '=') req = device_attributes::Req::tertiary;
                else have = false;

                if (have) {
                    Action a(K::device_attributes);
                    a.device_attributes = req;
                    handler.vt(a);
                } else {
                    /* logUnsupportedOnce("invalid device attributes command") */
                    return;
                }
                break;
            }

            /* VPA - Cursor Vertical Position Absolute */
            case 'd': if (ilen == 0) cursorMove(K::cursor_row, input); break;

            /* VPR - Cursor Vertical Position Relative */
            case 'e': if (ilen == 0) cursorMove(K::cursor_row_relative, input); break;

            /* TBC - Tab Clear
             * TODO: test */
            case 'g':
                if (ilen == 0) {
                    if (input.params_len != 1) {
                        /* log.warn("invalid tab clear command") */
                        return;
                    }
                    /* std.enums.fromInt(csi.TabClear, input.params[0]):
                     * TabClear is enum(u8), so values above 255 do not
                     * convert. */
                    if (input.params[0] > 0xFF) {
                        /* log.warn("invalid tab clear mode") */
                        return;
                    }
                    switch ((csi::TabClear)input.params[0]) {
                        case csi::TabClear::current: vt0(K::tab_clear_current); break;
                        case csi::TabClear::all: vt0(K::tab_clear_all); break;
                        default: /* log.warn("unknown tab clear mode") */ break;
                    }
                }
                break;

            /* SM - Set Mode */
            case 'h':
            /* RM - Reset Mode */
            case 'l': {
                bool ansi_mode;
                if (ilen == 0) ansi_mode = true;
                else if (ilen == 1 && im[0] == '?') ansi_mode = false;
                else {
                    /* log.warn("invalid set mode command") */
                    break;
                }

                for (size_t i = 0; i < input.params_len; i++) {
                    const uint16_t mode_int = input.params[i];
                    modes::Mode mode;
                    if (modes::modeFromInt(mode_int, ansi_mode, &mode)) {
                        modeAction(input.final_ == 'h' ? K::set_mode : K::reset_mode, mode);
                    } else {
                        /* logUnsupportedOnce("unimplemented mode") */
                    }
                }
                break;
            }

            /* SGR - Select Graphic Rendition */
            case 'm':
                if (ilen == 0) {
                    /* This is the most common case. */
                    sgr::Parser p(input.params, input.params_len, input.params_sep);
                    sgr::Attribute attr;
                    while (p.next(&attr)) {
                        Action a(K::set_attribute);
                        a.set_attribute = attr;
                        handler.vt(a);
                    }
                } else if (ilen == 1) {
                    if (im[0] == '>') {
                        Action a(K::modify_key_format);
                        if (input.params_len == 0) {
                            /* Reset */
                            a.modify_key_format = ansi::ModifyKeyFormat::legacy;
                            handler.vt(a);
                            break;
                        }

                        ansi::ModifyKeyFormat format;
                        switch (input.params[0]) {
                            case 0: format = ansi::ModifyKeyFormat::legacy; break;
                            case 1: format = ansi::ModifyKeyFormat::cursor_keys; break;
                            case 2: format = ansi::ModifyKeyFormat::function_keys; break;
                            case 4: format = ansi::ModifyKeyFormat::other_keys_none; break;
                            default: /* log.warn("invalid setModifyKeyFormat") */ return;
                        }

                        if (input.params_len > 2) {
                            /* log.warn("invalid setModifyKeyFormat") */
                            break;
                        }

                        if (input.params_len == 2) {
                            switch (format) {
                                /* We don't support any of the subparams yet for these. */
                                case ansi::ModifyKeyFormat::legacy: break;
                                case ansi::ModifyKeyFormat::cursor_keys: break;
                                case ansi::ModifyKeyFormat::function_keys: break;

                                /* We only support the numeric form. */
                                case ansi::ModifyKeyFormat::other_keys_none:
                                    if (input.params[1] == 2) format = ansi::ModifyKeyFormat::other_keys_numeric;
                                    break;
                                case ansi::ModifyKeyFormat::other_keys_numeric_except: break;
                                case ansi::ModifyKeyFormat::other_keys_numeric: break;
                            }
                        }

                        a.modify_key_format = format;
                        handler.vt(a);
                    } else {
                        /* logUnsupportedOnce("unknown CSI m with intermediate") */
                    }
                } else {
                    /* Nothing, but I wanted a place to put this comment:
                     * there are others forms of CSI m that have intermediates.
                     * `vim --clean` uses `CSI ? 4 m` and I don't know what
                     * that means. */
                }
                break;

            /* TODO: test */
            case 'n': {
                /* Handle deviceStatusReport first */
                if (ilen == 0 || im[0] == '?') {
                    if (input.params_len != 1) {
                        /* log.warn("invalid device status report command") */
                        return;
                    }

                    bool question;
                    if (ilen == 0) question = false;
                    else if (ilen == 1 && im[0] == '?') question = true;
                    else {
                        /* log.warn("invalid set mode command") */
                        return;
                    }

                    device_status::Request req;
                    if (!device_status::reqFromInt(input.params[0], question, &req)) {
                        /* log.warn("invalid device status report command") */
                        return;
                    }

                    Action a(K::device_status);
                    a.device_status = req;
                    handler.vt(a);
                    return;
                }

                /* Handle other forms of CSI n */
                if (ilen == 1) {
                    if (im[0] == '>') {
                        /* This isn't strictly correct. CSI > n has parameters that
                         * control what exactly is being disabled. However, we
                         * only support reverting back to modify other keys in
                         * numeric except format. */
                        Action a(K::modify_key_format);
                        a.modify_key_format = ansi::ModifyKeyFormat::other_keys_numeric_except;
                        handler.vt(a);
                    }
                    /* else log.warn("unknown CSI n with intermediate") */
                }
                break;
            }

            /* DECRQM - Request Mode */
            case 'p':
                if (ilen == 1 || ilen == 2) {
                    bool ansi_mode;
                    if (ilen == 1 && im[0] == '$') ansi_mode = true;
                    else if (ilen == 2 && im[0] == '?' && im[1] == '$') ansi_mode = false;
                    else {
                        /* log.warn("ignoring unimplemented CSI p with intermediates") */
                        break;
                    }

                    if (input.params_len != 1) {
                        /* log.warn("invalid DECRQM command") */
                        break;
                    }

                    const uint16_t mode_raw = input.params[0];
                    modes::Mode m;
                    if (modes::modeFromInt(mode_raw, ansi_mode, &m)) {
                        modeAction(K::request_mode, m);
                    } else {
                        Action a(K::request_mode_unknown);
                        a.request_mode_unknown.mode = mode_raw;
                        a.request_mode_unknown.ansi = ansi_mode;
                        handler.vt(a);
                    }
                }
                break;

            case 'q':
                if (ilen == 1) {
                    switch (im[0]) {
                        /* DECSCUSR - Select Cursor Style
                         * TODO: test */
                        case ' ': {
                            ansi::CursorStyle style;
                            if (input.params_len == 0) style = ansi::CursorStyle::default_;
                            else if (input.params_len == 1) {
                                switch (input.params[0]) {
                                    case 0: style = ansi::CursorStyle::default_; break;
                                    case 1: style = ansi::CursorStyle::blinking_block; break;
                                    case 2: style = ansi::CursorStyle::steady_block; break;
                                    case 3: style = ansi::CursorStyle::blinking_underline; break;
                                    case 4: style = ansi::CursorStyle::steady_underline; break;
                                    case 5: style = ansi::CursorStyle::blinking_bar; break;
                                    case 6: style = ansi::CursorStyle::steady_bar; break;
                                    default: /* log.warn("invalid cursor style value") */ return;
                                }
                            } else {
                                /* log.warn("invalid set cursor style command") */
                                return;
                            }
                            Action a(K::cursor_style);
                            a.cursor_style = style;
                            handler.vt(a);
                            break;
                        }

                        /* DECSCA */
                        case '"': {
                            bool have = true;
                            ansi::ProtectedMode mode = ansi::ProtectedMode::off;
                            if (input.params_len == 0) mode = ansi::ProtectedMode::off;
                            else if (input.params_len == 1) {
                                switch (input.params[0]) {
                                    case 0: case 2: mode = ansi::ProtectedMode::off; break;
                                    case 1: mode = ansi::ProtectedMode::dec; break;
                                    default: have = false; break;
                                }
                            } else have = false;

                            if (!have) {
                                /* log.warn("invalid set protected mode command") */
                                return;
                            }

                            switch (mode) {
                                case ansi::ProtectedMode::off: vt0(K::protected_mode_off); break;
                                case ansi::ProtectedMode::iso: vt0(K::protected_mode_iso); break;
                                case ansi::ProtectedMode::dec: vt0(K::protected_mode_dec); break;
                            }
                            break;
                        }

                        /* XTVERSION */
                        case '>': vt0(K::xtversion); break;
                        default: /* log.warn("ignoring unimplemented CSI q with intermediates") */ break;
                    }
                }
                break;

            case 'r':
                if (ilen == 0) {
                    /* DECSTBM - Set Top and Bottom Margins */
                    Action a(K::top_and_bottom_margin);
                    switch (input.params_len) {
                        case 0: a.margin.top_left = 0; a.margin.bottom_right = 0; break;
                        case 1: a.margin.top_left = input.params[0]; a.margin.bottom_right = 0; break;
                        case 2: a.margin.top_left = input.params[0]; a.margin.bottom_right = input.params[1]; break;
                        default: /* log.warn("invalid DECSTBM command") */ return;
                    }
                    handler.vt(a);
                } else if (ilen == 1) {
                    /* Restore Mode */
                    if (im[0] == '?') {
                        for (size_t i = 0; i < input.params_len; i++) {
                            modes::Mode mode;
                            if (modes::modeFromInt(input.params[i], false, &mode)) {
                                modeAction(K::restore_mode, mode);
                            }
                            /* else log.warn("unimplemented restore mode") */
                        }
                    }
                    /* else log.warn("unknown CSI s with intermediate") */
                }
                break;

            case 's':
                if (ilen == 0) {
                    /* DECSLRM */
                    switch (input.params_len) {
                        /* CSI S is ambiguous with zero params so we defer
                         * to our handler to do the proper logic. If mode 69
                         * is set, then we should invoke DECSLRM, otherwise
                         * we should invoke SC. */
                        case 0: vt0(K::left_and_right_margin_ambiguous); break;
                        case 1: {
                            Action a(K::left_and_right_margin);
                            a.margin.top_left = input.params[0];
                            a.margin.bottom_right = 0;
                            handler.vt(a);
                            break;
                        }
                        case 2: {
                            Action a(K::left_and_right_margin);
                            a.margin.top_left = input.params[0];
                            a.margin.bottom_right = input.params[1];
                            handler.vt(a);
                            break;
                        }
                        default: /* log.warn("invalid DECSLRM command") */ break;
                    }
                } else if (ilen == 1) {
                    switch (im[0]) {
                        case '?':
                            for (size_t i = 0; i < input.params_len; i++) {
                                modes::Mode mode;
                                if (modes::modeFromInt(input.params[i], false, &mode)) {
                                    modeAction(K::save_mode, mode);
                                }
                                /* else log.warn("unimplemented save mode") */
                            }
                            break;

                        /* XTSHIFTESCAPE */
                        case '>': {
                            bool capture;
                            if (input.params_len == 0) capture = false;
                            else if (input.params_len == 1) {
                                if (input.params[0] == 0) capture = false;
                                else if (input.params[0] == 1) capture = true;
                                else break; /* log.warn("invalid XTSHIFTESCAPE command") */
                            } else break; /* log.warn("invalid XTSHIFTESCAPE command") */

                            flagAction(K::mouse_shift_capture, capture);
                            break;
                        }

                        default: /* log.warn("unknown CSI s with intermediate") */ break;
                    }
                }
                break;

            /* XTWINOPS */
            case 't':
                if (ilen == 0) {
                    if (input.params_len > 0) {
                        switch (input.params[0]) {
                            case 14: case 16: case 18: case 21: {
                                if (input.params_len != 1) {
                                    /* log.warn("ignoring CSI N t with extra parameters") */
                                    break;
                                }
                                Action a(K::size_report);
                                switch (input.params[0]) {
                                    /* report the text area size in pixels */
                                    case 14: a.size_report = csi::SizeReportStyle::csi_14_t; break;
                                    /* report cell size in pixels */
                                    case 16: a.size_report = csi::SizeReportStyle::csi_16_t; break;
                                    /* report screen size in characters */
                                    case 18: a.size_report = csi::SizeReportStyle::csi_18_t; break;
                                    /* report window title */
                                    default: a.size_report = csi::SizeReportStyle::csi_21_t; break;
                                }
                                handler.vt(a);
                                break;
                            }
                            case 22: case 23:
                                if ((input.params_len == 2 || input.params_len == 3) &&
                                    /* we only support window title */
                                    (input.params[1] == 0 || input.params[1] == 2)) {
                                    /* push/pop title */
                                    const uint16_t index = input.params_len == 3 ? input.params[2] : 0;
                                    value16Action(input.params[0] == 22 ? K::title_push : K::title_pop, index);
                                }
                                /* else logUnsupportedOnce("ignoring CSI 22/23 t with extra parameters") */
                                break;
                            default:
                                /* logUnsupportedOnce("ignoring CSI t with unimplemented parameter") */
                                break;
                        }
                    }
                    /* else log.err("ignoring CSI t with no parameters") */
                }
                break;

            case 'u':
                if (ilen == 0) {
                    vt0(K::restore_cursor);
                } else if (ilen == 1) {
                    /* Kitty keyboard protocol */
                    switch (im[0]) {
                        case '?': vt0(K::kitty_keyboard_query); break;

                        case '>': {
                            uint8_t flags = 0;
                            if (input.params_len == 1) {
                                /* std.math.cast(u5, input.params[0]) */
                                if (input.params[0] > 31) {
                                    /* log.warn("invalid pushKittyKeyboard command") */
                                    break;
                                }
                                flags = (uint8_t)input.params[0];
                            }

                            Action a(K::kitty_keyboard_push);
                            a.kitty_flags = kitty::KeyFlags::fromInt(flags);
                            handler.vt(a);
                            break;
                        }

                        case '<': {
                            const uint16_t number = input.params_len == 1 ? input.params[0] : 1;
                            value16Action(K::kitty_keyboard_pop, number);
                            break;
                        }

                        case '=': {
                            uint8_t flags = 0;
                            if (input.params_len >= 1) {
                                if (input.params[0] > 31) {
                                    /* log.warn("invalid setKittyKeyboard command") */
                                    break;
                                }
                                flags = (uint8_t)input.params[0];
                            }

                            const uint16_t number = input.params_len >= 2 ? input.params[1] : 1;

                            K action_tag;
                            switch (number) {
                                case 1: action_tag = K::kitty_keyboard_set; break;
                                case 2: action_tag = K::kitty_keyboard_set_or; break;
                                case 3: action_tag = K::kitty_keyboard_set_not; break;
                                default: /* log.warn("invalid setKittyKeyboard command") */ return;
                            }

                            Action a(action_tag);
                            a.kitty_flags = kitty::KeyFlags::fromInt(flags);
                            handler.vt(a);
                            break;
                        }

                        default: /* log.warn("unknown CSI s with intermediate") */ break;
                    }
                }
                /* else log.warn("ignoring unimplemented CSI u") */
                break;

            /* ICH - Insert Blanks */
            case '@':
                if (ilen == 0) {
                    size_t v;
                    switch (input.params_len) {
                        case 0: v = 1; break;
                        case 1: v = input.params[0] > 1 ? input.params[0] : 1; break;
                        default: /* log.warn("invalid ICH command") */ return;
                    }
                    Action a(K::insert_blanks);
                    a.count = v;
                    handler.vt(a);
                }
                break;

            /* DECSASD - Select Active Status Display */
            case '}': {
                /* Verify we're getting a DECSASD command */
                if (ilen != 1 || im[0] != '$') break;
                if (input.params_len != 1) break;

                ansi::StatusDisplay display;
                switch (input.params[0]) {
                    case 0: display = ansi::StatusDisplay::main; break;
                    case 1: display = ansi::StatusDisplay::status_line; break;
                    default: /* log.warn("unimplemented CSI callback") */ return;
                }

                Action a(K::active_status_display);
                a.active_status_display = display;
                handler.vt(a);
                break;
            }

            default: /* log.warn("unimplemented CSI action") */ break;
        }
    }

    void oscDispatch(const osc::Command &cmd) {
        /* The branch hints here are based on real world data
         * which indicates that the most common OSC commands are:
         *
         * 1. hyperlink_end
         * 2. change_window_title
         * 3. change_window_icon
         * 4. hyperlink_start
         * 5. report_pwd
         * 6. color_operation
         * 7. semantic_prompt
         *
         * Together, these 7 commands make up about 96% of all
         * OSC commands encountered in real world scenarios.
         *
         * ref: https://github.com/qwerasd205/asciinema-stats */

        typedef osc::Command::Key CK;
        typedef Action::Key K;
        switch (cmd.key) {
            case CK::semantic_prompt: {
                Action a(K::semantic_prompt);
                a.semantic_prompt = cmd.semantic_prompt;
                handler.vt(a);
                break;
            }

            case CK::change_window_title: {
                const osc::ZStr &title = cmd.change_window_title;
                if (!utf8ValidateSlice(title.ptr, title.len)) {
                    /* log.warn("change title request: invalid utf-8, ignoring request") */
                    return;
                }

                Action a(K::window_title);
                a.window_title.title = title.ptr;
                a.window_title.len = title.len;
                handler.vt(a);
                break;
            }

            case CK::change_window_icon:
                /* logUnsupportedOnce("OSC 1 (change icon) received and ignored") */
                break;

            case CK::clipboard_contents: {
                Action a(K::clipboard_contents);
                a.clipboard_contents.kind = cmd.clipboard_contents.kind;
                a.clipboard_contents.data = cmd.clipboard_contents.data;
                a.clipboard_contents.terminator = cmd.clipboard_contents.terminator;
                handler.vt(a);
                break;
            }

            case CK::report_pwd: {
                Action a(K::report_pwd);
                a.report_pwd.url = cmd.report_pwd.value.ptr;
                a.report_pwd.len = cmd.report_pwd.value.len;
                handler.vt(a);
                break;
            }

            case CK::mouse_shape: {
                mouse::Shape shape;
                if (!mouse::Shape_fromString(cmd.mouse_shape.value.ptr, cmd.mouse_shape.value.len, &shape)) {
                    /* log.warn("unknown cursor shape") */
                    return;
                }

                Action a(K::mouse_shape);
                a.mouse_shape = shape;
                handler.vt(a);
                break;
            }

            case CK::color_operation: {
                Action a(K::color_operation);
                a.color_operation.op = cmd.color_operation.op;
                a.color_operation.requests = cmd.color_operation.requests;
                a.color_operation.terminator = cmd.color_operation.terminator;
                handler.vt(a);
                break;
            }

            case CK::kitty_color_protocol: {
                Action a(K::kitty_color_report);
                a.kitty_color_report = cmd.kitty_color_protocol;
                handler.vt(a);
                break;
            }

            case CK::show_desktop_notification: {
                Action a(K::show_desktop_notification);
                a.show_desktop_notification.title = cmd.show_desktop_notification.title;
                a.show_desktop_notification.body = cmd.show_desktop_notification.body;
                handler.vt(a);
                break;
            }

            case CK::hyperlink_start: {
                Action a(K::start_hyperlink);
                a.start_hyperlink.uri = cmd.hyperlink_start.uri;
                a.start_hyperlink.has_id = cmd.hyperlink_start.has_id;
                a.start_hyperlink.id = cmd.hyperlink_start.id;
                handler.vt(a);
                break;
            }

            case CK::hyperlink_end: vt0(K::end_hyperlink); break;

            case CK::conemu_progress_report: {
                Action a(K::progress_report);
                a.progress_report = cmd.conemu_progress_report;
                handler.vt(a);
                break;
            }

            case CK::kitty_clipboard_protocol: {
                Action a(K::kitty_clipboard);
                a.kitty_clipboard = cmd.kitty_clipboard_protocol;
                handler.vt(a);
                break;
            }

            case CK::kitty_dnd_protocol: {
                Action a(K::kitty_dnd);
                a.kitty_dnd = cmd.kitty_dnd_protocol;
                handler.vt(a);
                break;
            }

            case CK::conemu_sleep:
            case CK::conemu_show_message_box:
            case CK::conemu_change_tab_title:
            case CK::conemu_wait_input:
            case CK::conemu_guimacro:
            case CK::conemu_comment:
            case CK::conemu_xterm_emulation:
            case CK::conemu_output_environment_variable:
            case CK::conemu_run_process:
            case CK::kitty_text_sizing:
            case CK::kitty_desktop_notification:
            case CK::context_signal:
                /* log.debug("unimplemented OSC callback") */
                break;

            case CK::invalid:
                /* This is an invalid internal state, not an invalid OSC
                 * string being parsed. We shouldn't see this. */
                break;
        }
    }

    void configureCharset(const uint8_t *intermediates, size_t len, charsets::Charset set) {
        if (len != 1) {
            /* log.warn("invalid charset intermediate") */
            return;
        }

        charsets::Slots slot;
        switch (intermediates[0]) {
            /* TODO: support slots '-', '.', '/' */

            case '(': slot = charsets::Slots::G0; break;
            case ')': slot = charsets::Slots::G1; break;
            case '*': slot = charsets::Slots::G2; break;
            case '+': slot = charsets::Slots::G3; break;
            default: /* log.warn("invalid charset intermediate") */ return;
        }

        Action a(Action::Key::configure_charset);
        a.configure_charset.slot = slot;
        a.configure_charset.charset = set;
        handler.vt(a);
    }

    void escDispatch(const parser::Action::ESC &action) {
        /* The branch hints here are based on real world data
         * which indicates that the most common ESC finals are:
         *
         * 1. B
         * 2. \
         * 3. 0
         * 4. M
         * 5. 8
         * 6. 7
         * 7. >
         * 8. =
         *
         * Together, these 8 finals make up nearly 99% of all
         * ESC sequences encountered in real world scenarios.
         *
         * ref: https://github.com/qwerasd205/asciinema-stats */

        typedef Action::Key K;
        const size_t ilen = action.intermediates_len;

        switch (action.final_) {
            /* Charsets */
            case 'B': configureCharset(action.intermediates, ilen, charsets::Charset::ascii); break;
            case 'A': configureCharset(action.intermediates, ilen, charsets::Charset::british); break;
            case '0': configureCharset(action.intermediates, ilen, charsets::Charset::dec_special); break;

            /* DECSC - Save Cursor */
            case '7':
                if (ilen == 0) vt0(K::save_cursor);
                /* else log.warn("invalid command") */
                break;

            case '8':
                /* DECRC - Restore Cursor */
                if (ilen == 0) {
                    vt0(K::restore_cursor);
                    break;
                }
                /* DECALN - Fill Screen with E */
                if (ilen == 1 && action.intermediates[0] == '#') {
                    vt0(K::decaln);
                    break;
                }
                /* logUnsupportedOnce("unimplemented ESC action") */
                break;

            /* IND - Index */
            case 'D': if (ilen == 0) vt0(K::index); break;

            /* NEL - Next Line */
            case 'E': if (ilen == 0) vt0(K::next_line); break;

            /* HTS - Horizontal Tab Set */
            case 'H': if (ilen == 0) vt0(K::tab_set); break;

            /* RI - Reverse Index */
            case 'M': if (ilen == 0) vt0(K::reverse_index); break;

            /* SS2 - Single Shift 2 */
            case 'N': if (ilen == 0) invokeCharset(charsets::ActiveSlot::GL, charsets::Slots::G2, true); break;

            /* SS3 - Single Shift 3 */
            case 'O': if (ilen == 0) invokeCharset(charsets::ActiveSlot::GL, charsets::Slots::G3, true); break;

            /* SPA - Start of Guarded Area */
            case 'V': if (ilen == 0) vt0(K::protected_mode_iso); break;

            /* EPA - End of Guarded Area */
            case 'W': if (ilen == 0) vt0(K::protected_mode_off); break;

            /* DECID */
            case 'Z':
                if (ilen == 0) {
                    Action a(K::device_attributes);
                    a.device_attributes = device_attributes::Req::primary;
                    handler.vt(a);
                }
                break;

            /* RIS - Full Reset */
            case 'c': if (ilen == 0) vt0(K::full_reset); break;

            /* LS2 - Locking Shift 2 */
            case 'n': if (ilen == 0) invokeCharset(charsets::ActiveSlot::GL, charsets::Slots::G2, false); break;

            /* LS3 - Locking Shift 3 */
            case 'o': if (ilen == 0) invokeCharset(charsets::ActiveSlot::GL, charsets::Slots::G3, false); break;

            /* LS1R - Locking Shift 1 Right */
            case '~': if (ilen == 0) invokeCharset(charsets::ActiveSlot::GR, charsets::Slots::G1, false); break;

            /* LS2R - Locking Shift 2 Right */
            case '}': if (ilen == 0) invokeCharset(charsets::ActiveSlot::GR, charsets::Slots::G2, false); break;

            /* LS3R - Locking Shift 3 Right */
            case '|': if (ilen == 0) invokeCharset(charsets::ActiveSlot::GR, charsets::Slots::G3, false); break;

            /* Set application keypad mode */
            case '=': if (ilen == 0) modeAction(K::set_mode, modes::Mode::keypad_keys); break;

            /* Reset application keypad mode */
            case '>': if (ilen == 0) modeAction(K::reset_mode, modes::Mode::keypad_keys); break;

            /* Sets ST (string terminator). We don't have to do anything
             * because our parser always accepts ST. */
            case '\\': break;

            default: /* logUnsupportedOnce("unimplemented ESC action") */ break;
        }
    }
};

} /* namespace stream */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_VT_STREAM_HPP */
