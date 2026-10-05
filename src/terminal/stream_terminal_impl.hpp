/* Out-of-line definitions for stream_terminal.hpp. See that file's header
 * comment for the transliteration notes; this exists so the declarations
 * read in upstream's order without the definitions between them.
 *
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 */

#pragma once
#ifndef WISP_TERMINAL_STREAM_TERMINAL_IMPL_HPP
#define WISP_TERMINAL_STREAM_TERMINAL_IMPL_HPP

#include "stream_terminal.hpp"

namespace wisp {
namespace terminal {
namespace stream_terminal {

/* Wisp: Zig's `+|` saturating add. */
inline size_t satAdd(size_t a, size_t b) {
    const size_t r = a + b;
    return r < a ? SIZE_MAX : r;
}

inline void Handler::writePty(const char *data, size_t len) {
    void (*func)(Handler *, const char *, size_t) = effects.write_pty;
    if (func == nullptr) return;
    func(this, data, len);
}

inline void Handler::unknownSequence(UnknownSequence value) {
    void (*func)(Handler *, UnknownSequence) = unknown_sequence;
    if (func == nullptr) return;
    func(this, value);
}

/* Report that a render hold began or ended. See `Effects.render_hold`. */
inline void Handler::renderHold(bool held) {
    void (*func)(Handler *, bool) = effects.render_hold;
    if (func == nullptr) return;
    func(this, held);
}

inline void Handler::bell() {
    void (*func)(Handler *) = effects.bell;
    if (func == nullptr) return;
    func(this);
}

inline void Handler::desktopNotification(stream::Action::ShowDesktopNotification notification) {
    void (*func)(Handler *, stream::Action::ShowDesktopNotification) = effects.desktop_notification;
    if (func == nullptr) return;
    func(this, notification);
}

inline void Handler::progressReport(osc::ProgressReport report) {
    void (*func)(Handler *, osc::ProgressReport) = effects.progress_report;
    if (func == nullptr) return;
    func(this, report);
}

/* Resize the terminal and apply any side effects (if supported)
 * as a result of that.
 *
 * This is different than a direct `Terminal.resize` operation
 * because it also handles the side effects like mode 2048 in-band
 * size reports if write_pty is set. */
inline bool Handler::resize(const vt::Terminal::Resize &value) {
    /* Resize always turns off synchronized output, ending its hold. */
    const bool sync = terminal->modes.get(modes::Mode::synchronized_output);
    if (terminal->resize(terminal->gpa(), value) != vt::Terminal::ResizeError::none) return false;
    if (sync) renderHold(false);

    /* Mode 2048 reports require complete, current cell pixel geometry. */
    if (!value.cell_size_px.has) return true;

    /* If we have no in-band size reports enabled then do nothing. */
    if (!terminal->modes.get(modes::Mode::in_band_size_reports)) return true;

    /* If we have no write_pty effect, do nothing. */
    if (effects.write_pty == nullptr) return true;

    std::string writer;
    size_report::Size s;
    s.rows = value.rows;
    s.columns = value.cols;
    s.cell_width = value.cell_size_px.value.width;
    s.cell_height = value.cell_size_px.value.height;
    size_report::encode(&writer, size_report::Style::mode_2048, s);
    writePty(writer.data(), writer.size());
    return true;
}

inline void Handler::horizontalTab(uint16_t count) {
    for (uint16_t i = 0; i < count; i++) {
        const vt::size::CellCountInt x = terminal->screens.active->cursor.x;
        terminal->horizontalTab();
        if (x == terminal->screens.active->cursor.x) break;
    }
}

inline void Handler::horizontalTabBack(uint16_t count) {
    for (uint16_t i = 0; i < count; i++) {
        const vt::size::CellCountInt x = terminal->screens.active->cursor.x;
        terminal->horizontalTabBack();
        if (x == terminal->screens.active->cursor.x) break;
    }
}

/* Answer an XTGETTCAP "TN" query from the configured terminfo name.
 * Unset, empty, or over-long names leave the query unanswered. */
inline void Handler::writeTerminfoName() {
    if (!has_terminfo_name) return;
    const ZStr name = terminfo_name;
    if (name.len == 0 || name.len > max_terminfo_name_bytes) return;

    /* Values are hex-encoded uppercase, matching the static map. */
    std::string buf;
    buf.append("\x1bP1+r544E=");
    static const char *digits = "0123456789ABCDEF";
    for (size_t i = 0; i < name.len; i++) {
        const uint8_t b = (uint8_t)name.ptr[i];
        buf.push_back(digits[b >> 4]);
        buf.push_back(digits[b & 0xF]);
    }
    buf.append("\x1b\\");
    writePty(buf.data(), buf.size());
}

inline bool Handler::dcsCommand(dcs::Command *cmd) {
    switch (cmd->key) {
    case dcs::Command::Key::decrqss: {
        std::string response;
        dcs::Command::encodeDECRQSS(cmd->decrqss, terminal, &response);
        writePty(response.data(), response.size());
        break;
    }

    case dcs::Command::Key::xtgettcap: {
        if (effects.write_pty == nullptr) return true;
        const uint8_t *key = nullptr;
        size_t key_len = 0;
        while (cmd->xtgettcap.next(&key, &key_len)) {
            /* Hex-encoded "TN", the XTGETTCAP key naming the terminfo entry.
             * The static map also carries this key with Ghostty's own name, so
             * it is intercepted before the lookup: an embedder that never
             * configured a name must not be reported as Ghostty's entry. */
            if (key_len == 4 && memcmp(key, "544E", 4) == 0) {
                writeTerminfoName();
                continue;
            }
            const char *resp = terminfo::xtgettcapGet(key, key_len);
            if (resp == nullptr) continue;
            writePty(resp, strlen(resp));
        }
        break;
    }
    }
    return true;
}

inline bool Handler::dcsHook(const parser::Action::DCS &value) {
    dcs::Command cmd;
    if (!dcs_handler.hook(terminal->gpa(), value, &cmd)) return true;
    const bool ok = dcsCommand(&cmd);
    cmd.deinit();
    return ok;
}

inline bool Handler::dcsPut(uint8_t value) {
    dcs::Command cmd;
    if (!dcs_handler.put(value, &cmd)) return true;
    const bool ok = dcsCommand(&cmd);
    cmd.deinit();
    return ok;
}

inline bool Handler::dcsUnhook() {
    dcs::Command cmd;
    if (!dcs_handler.unhook(&cmd)) return true;
    const bool ok = dcsCommand(&cmd);
    cmd.deinit();
    return ok;
}

inline void Handler::reportDeviceAttributes(device_attributes::Req req) {
    device_attributes::Attributes (*func)(Handler *) = effects.device_attributes;
    if (func == nullptr) return;
    const device_attributes::Attributes attrs = func(this);

    std::string aw;
    attrs.encode(req, &aw);
    writePty(aw.data(), aw.size());
}

inline void Handler::reportEnquiry() {
    ZStr (*func)(Handler *) = effects.enquiry;
    if (func == nullptr) return;
    const ZStr response = func(this);
    if (response.len == 0) return;
    if (response.len >= 256) return;
    writePty(response.ptr, response.len);
}

inline void Handler::reportXtversion() {
    ZStr version = ZStr("", 0);
    if (effects.xtversion != nullptr) version = effects.xtversion(this);
    if (version.len == 0) version = ZStr("libghostty", 10);
    if (version.len > 256) return;
    std::string resp;
    resp.append("\x1bP>|");
    resp.append(version.ptr, version.len);
    resp.append("\x1b\\");
    writePty(resp.data(), resp.size());
}

inline void Handler::queryKittyKeyboard() {
    /* Max response is "\x1b[?31u\x00" (7 bytes): the flags are a u5 (max 31). */
    char buf[32];
    snprintf(buf, sizeof buf, "\x1b[?%uu",
             (unsigned)terminal->screens.active->kitty_keyboard.current().int_());
    writePty(buf, strlen(buf));
}

inline void Handler::sendModeReport(const modes::Report &report) {
    std::string buf;
    report.encode(&buf);
    writePty(buf.data(), buf.size());
}

inline void Handler::requestMode(modes::Mode mode) {
    modes::Report report = terminal->modes.getReport(modes::Report::Tag::fromMode(mode));

    /* Kitty paste events (mode 5522) can't work without a clipboard
     * read effect, so if that isn't set mark it as unrecognized. */
    if (mode == modes::Mode::kitty_paste_events && effects.clipboard_read == nullptr) {
        report.state = modes::Report::State::not_recognized;
    }

    sendModeReport(report);
}

inline void Handler::requestModeUnknown(uint16_t mode_raw, bool ansi) {
    const modes::Report report = terminal->modes.getReport(modes::Report::Tag::make(mode_raw, ansi));
    sendModeReport(report);
}

inline void Handler::sendVisibilityReport() {
    if (effects.write_pty == nullptr) return;

    std::string writer;
    device_status::encodeVisibilityReport(&writer, terminal->flags.visible
                                                       ? device_status::Visibility::potentially_visible
                                                       : device_status::Visibility::not_visible);
    writePty(writer.data(), writer.size());
}

inline void Handler::deviceStatus(device_status::Request req) {
    switch (req) {
    case device_status::Request::operating_status: writePty("\x1B[0n", 4); break;

    case device_status::Request::cursor_position: {
        size_t x, y;
        if (terminal->modes.get(modes::Mode::origin)) {
            const vt::size::CellCountInt cx = terminal->screens.active->cursor.x;
            const vt::size::CellCountInt cy = terminal->screens.active->cursor.y;
            x = cx > terminal->scrolling_region.left ? (size_t)(cx - terminal->scrolling_region.left) : 0;
            y = cy > terminal->scrolling_region.top ? (size_t)(cy - terminal->scrolling_region.top) : 0;
        } else {
            x = terminal->screens.active->cursor.x;
            y = terminal->screens.active->cursor.y;
        }

        char buf[64];
        snprintf(buf, sizeof buf, "\x1B[%zu;%zuR", y + 1, x + 1);
        writePty(buf, strlen(buf));
        break;
    }

    case device_status::Request::color_scheme: {
        bool (*func)(Handler *, device_status::ColorScheme *) = effects.color_scheme;
        if (func == nullptr) return;
        device_status::ColorScheme scheme;
        if (!func(this, &scheme)) return;
        std::string writer;
        device_status::encodeColorSchemeReport(&writer, scheme);
        writePty(writer.data(), writer.size());
        break;
    }

    case device_status::Request::visibility: sendVisibilityReport(); break;
    }
}

inline void Handler::reportMode2048() {
    bool (*get_size)(Handler *, size_report::Size *) = effects.size;
    if (get_size == nullptr) return;
    size_report::Size current;
    if (!get_size(this, &current)) return;

    std::string writer;
    size_report::encode(&writer, size_report::Style::mode_2048, current);
    writePty(writer.data(), writer.size());
}

inline void Handler::reportSize(csi::SizeReportStyle style) {
    std::string aw;

    /* Build the response. */
    switch (style) {
    case csi::SizeReportStyle::csi_21_t: {
        if (!title_report) return;
        ZStr title = ZStr("", 0);
        const char *t = terminal->getTitle();
        if (t != nullptr) title = ZStr(t, strlen(t));
        aw.append("\x1b]l");
        aw.append(title.ptr, title.len);
        aw.append("\x1b\\");
        break;
    }

    case csi::SizeReportStyle::csi_14_t:
    case csi::SizeReportStyle::csi_16_t:
    case csi::SizeReportStyle::csi_18_t: {
        bool (*get_size)(Handler *, size_report::Size *) = effects.size;
        if (get_size == nullptr) return;
        size_report::Size s;
        if (!get_size(this, &s)) return;
        size_report::Style report_style = size_report::Style::csi_14_t;
        switch (style) {
        case csi::SizeReportStyle::csi_14_t: report_style = size_report::Style::csi_14_t; break;
        case csi::SizeReportStyle::csi_16_t: report_style = size_report::Style::csi_16_t; break;
        case csi::SizeReportStyle::csi_18_t: report_style = size_report::Style::csi_18_t; break;
        case csi::SizeReportStyle::csi_21_t: break; /* unreachable */
        }
        size_report::encode(&aw, report_style, s);
        break;
    }
    }

    writePty(aw.data(), aw.size());
}

inline bool Handler::windowTitle(ZStr title_raw) {
    /* Prevent DoS attacks by limiting title length. */
    const size_t max_title_len = 1024;
    ZStr title = title_raw;
    if (title_raw.len > max_title_len) {
        /* log.warn("title length {d} exceeds max length {d}, truncating") */
        title = ZStr(title_raw.ptr, max_title_len);
    }

    if (!terminal->setTitle(title.ptr, title.len)) return false;

    void (*func)(Handler *) = effects.title_changed;
    if (func == nullptr) return true;
    func(this);
    return true;
}

inline bool Handler::reportPwd(ZStr url_raw) {
    /* Prevent DoS attacks by limiting url length. Headroom for
     * Linux PATH_MAX (4096) plus URI scheme/host and percent-encoding. */
    const size_t max_url_len = 4096;
    ZStr url = url_raw;
    if (url_raw.len > max_url_len) {
        /* log.warn("pwd url length {d} exceeds max length {d}, truncating") */
        url = ZStr(url_raw.ptr, max_url_len);
    }

    /* We store the raw payload unparsed. Embedders read it via
     * getPwd() and are responsible for decoding any URI scheme. */
    if (!terminal->setPwd(url.ptr, url.len)) return false;

    void (*func)(Handler *) = effects.pwd_changed;
    if (func == nullptr) return true;
    func(this);
    return true;
}

inline bool Handler::setMode(modes::Mode mode, bool enabled) {
    /* Synchronized output is reported as a render hold. We only report
     * real changes. Reporting a set during a hold would be harmful
     * because the screen is half-drawn at that point and the callback
     * is expected to capture it. */
    if (mode == modes::Mode::synchronized_output) {
        if (terminal->modes.get(mode) == enabled) return true;
        terminal->modes.set(mode, enabled);
        renderHold(enabled);
        return true;
    }

    /* Set the mode on the terminal */
    terminal->modes.set(mode, enabled);

    /* Some modes require additional processing */
    switch (mode) {
    case modes::Mode::autorepeat:
    case modes::Mode::reverse_colors: break;

    case modes::Mode::origin: terminal->setCursorPos(1, 1); break;

    case modes::Mode::enable_left_and_right_margin:
        if (!enabled) {
            terminal->scrolling_region.left = 0;
            terminal->scrolling_region.right = (vt::size::CellCountInt)(terminal->cols - 1);
        }
        break;

    case modes::Mode::alt_screen_legacy:
        if (!terminal->switchScreenMode(vt::Terminal::SwitchScreenMode::mode_47, enabled)) return false;
        break;
    case modes::Mode::alt_screen:
        if (!terminal->switchScreenMode(vt::Terminal::SwitchScreenMode::mode_1047, enabled)) return false;
        break;
    case modes::Mode::alt_screen_save_cursor_clear_enter:
        if (!terminal->switchScreenMode(vt::Terminal::SwitchScreenMode::mode_1049, enabled)) return false;
        break;

    case modes::Mode::save_cursor:
        if (enabled) {
            terminal->saveCursor();
        } else {
            terminal->restoreCursor();
        }
        break;

    case modes::Mode::enable_mode_3: break;

    case modes::Mode::_132_column:
        if (terminal->deccolm(terminal->screens.active->alloc,
                              enabled ? vt::Terminal::DeccolmMode::cols_132
                                      : vt::Terminal::DeccolmMode::cols_80) !=
            vt::Terminal::ResizeError::none)
            return false;
        break;

    /* Handled above */
    case modes::Mode::synchronized_output: break; /* unreachable */

    case modes::Mode::linefeed:
    case modes::Mode::focus_event: break;

    /* Enabling mode 2048 reports already-committed pixel geometry.
     * Waiting for the next resize leaves late-enabling clients without
     * the dimensions they need for their first image frame. */
    case modes::Mode::in_band_size_reports:
        if (enabled) reportMode2048();
        break;

    case modes::Mode::report_visibility:
        if (enabled) sendVisibilityReport();
        break;

    case modes::Mode::mouse_event_x10:
        terminal->flags.mouse_event = enabled ? mouse::Event::x10 : mouse::Event::none;
        break;
    case modes::Mode::mouse_event_normal:
        terminal->flags.mouse_event = enabled ? mouse::Event::normal : mouse::Event::none;
        break;
    case modes::Mode::mouse_event_button:
        terminal->flags.mouse_event = enabled ? mouse::Event::button : mouse::Event::none;
        break;
    case modes::Mode::mouse_event_any:
        terminal->flags.mouse_event = enabled ? mouse::Event::any : mouse::Event::none;
        break;

    case modes::Mode::mouse_format_utf8:
        terminal->flags.mouse_format = enabled ? mouse::Format::utf8 : mouse::Format::x10;
        break;
    case modes::Mode::mouse_format_sgr:
        terminal->flags.mouse_format = enabled ? mouse::Format::sgr : mouse::Format::x10;
        break;
    case modes::Mode::mouse_format_urxvt:
        terminal->flags.mouse_format = enabled ? mouse::Format::urxvt : mouse::Format::x10;
        break;
    case modes::Mode::mouse_format_sgr_pixels:
        terminal->flags.mouse_format = enabled ? mouse::Format::sgr_pixels : mouse::Format::x10;
        break;

    default: break;
    }
    return true;
}

inline void Handler::writeXtermColorReport(std::string *writer, const osc::color::Target &target,
                                           terminal::RGB c, osc::Terminator terminator) {
    char buf[64];
    switch (target.tag) {
    case osc::color::Target::Tag::palette: {
        snprintf(buf, sizeof buf, "\x1b]4;%u;", (unsigned)target.palette);
        writer->append(buf);
        const size_t n = c.encodeRgb16(buf, sizeof buf);
        writer->append(buf, n);
        writer->append(osc::terminator_string(terminator));
        break;
    }
    case osc::color::Target::Tag::dynamic:
        switch (target.dynamic) {
        case terminal::Dynamic::foreground:
        case terminal::Dynamic::background:
        case terminal::Dynamic::cursor: {
            snprintf(buf, sizeof buf, "\x1b]%u;", (unsigned)target.dynamic);
            writer->append(buf);
            const size_t n = c.encodeRgb16(buf, sizeof buf);
            writer->append(buf, n);
            writer->append(osc::terminator_string(terminator));
            break;
        }
        default: break;
        }
        break;
    case osc::color::Target::Tag::special: break;
    }
}

inline bool Handler::colorOperation(const osc::color::List *requests, osc::Terminator terminator) {
    if (requests->count() == 0) return true;

    std::string response;

    for (size_t i = 0; i < requests->count(); i++) {
        const osc::color::Request *req = requests->at(i);
        switch (req->tag) {
        case osc::color::Request::Tag::set: {
            const osc::color::ColoredTarget &set = req->set;
            switch (set.target.tag) {
            case osc::color::Target::Tag::palette:
                terminal->flags.dirty.palette = true;
                terminal->colors.palette.set(set.target.palette, set.color);
                break;
            case osc::color::Target::Tag::dynamic:
                switch (set.target.dynamic) {
                case terminal::Dynamic::foreground: terminal->colors.foreground.set(set.color); break;
                case terminal::Dynamic::background: terminal->colors.background.set(set.color); break;
                case terminal::Dynamic::cursor: terminal->colors.cursor.set(set.color); break;
                default: break;
                }
                break;
            case osc::color::Target::Tag::special: break;
            }
            break;
        }

        case osc::color::Request::Tag::reset: {
            const osc::color::Target &target = req->reset;
            switch (target.tag) {
            case osc::color::Target::Tag::palette:
                terminal->flags.dirty.palette = true;
                terminal->colors.palette.reset(target.palette);
                break;
            case osc::color::Target::Tag::dynamic:
                switch (target.dynamic) {
                case terminal::Dynamic::foreground: terminal->colors.foreground.reset(); break;
                case terminal::Dynamic::background: terminal->colors.background.reset(); break;
                case terminal::Dynamic::cursor: terminal->colors.cursor.reset(); break;
                default: break;
                }
                break;
            case osc::color::Target::Tag::special: break;
            }
            break;
        }

        case osc::color::Request::Tag::reset_palette: {
            terminal::PaletteMask *mask = &terminal->colors.palette.mask;
            for (size_t idx = 0; idx < 256; idx++) {
                if (!mask->isSet(idx)) continue;
                terminal->flags.dirty.palette = true;
                terminal->colors.palette.reset((uint8_t)idx);
            }
            *mask = terminal::PaletteMask::initEmpty();
            break;
        }

        case osc::color::Request::Tag::query: {
            if (effects.write_pty == nullptr) continue;
            const vt::Maybe<terminal::RGB> maybe_c = terminal->colorForXterm(req->query);
            if (!maybe_c.has) continue;
            writeXtermColorReport(&response, req->query, maybe_c.value, terminator);
            break;
        }

        case osc::color::Request::Tag::reset_special: break;
        }
    }

    if (!response.empty()) writePty(response.data(), response.size());
    return true;
}

inline bool Handler::kittyColorOperation(const stream::Action::KittyColorReport &request) {
    std::string response;

    for (size_t i = 0; i < request.list.len; i++) {
        const kitty::color::Request &item = request.list.items[i];
        switch (item.tag) {
        case kitty::color::Request::Tag::set: {
            const auto &v = item.set;
            switch (v.key.tag) {
            case kitty::color::Kind::Tag::palette:
                terminal->flags.dirty.palette = true;
                terminal->colors.palette.set(v.key.palette, v.color);
                break;
            case kitty::color::Kind::Tag::special:
                switch (v.key.special) {
                case kitty::color::Special::foreground: terminal->colors.foreground.set(v.color); break;
                case kitty::color::Special::background: terminal->colors.background.set(v.color); break;
                case kitty::color::Special::cursor: terminal->colors.cursor.set(v.color); break;
                default: break;
                }
                break;
            }
            break;
        }
        case kitty::color::Request::Tag::reset: {
            const kitty::color::Kind &key = item.reset;
            switch (key.tag) {
            case kitty::color::Kind::Tag::palette:
                terminal->flags.dirty.palette = true;
                terminal->colors.palette.reset(key.palette);
                break;
            case kitty::color::Kind::Tag::special:
                switch (key.special) {
                case kitty::color::Special::foreground: terminal->colors.foreground.reset(); break;
                case kitty::color::Special::background: terminal->colors.background.reset(); break;
                case kitty::color::Special::cursor: terminal->colors.cursor.reset(); break;
                default: break;
                }
                break;
            }
            break;
        }
        case kitty::color::Request::Tag::query: {
            if (effects.write_pty == nullptr) continue;
            const kitty::color::Kind &key = item.query;
            const vt::Maybe<terminal::RGB> maybe_c = terminal->colorForKitty(key);
            if (!maybe_c.has) {
                if (!key.hasTerminalQueryColor()) continue;
                if (response.empty()) response.append("\x1b]21");
                response.push_back(';');
                {
                    char kbuf[64];
                    key.format(kbuf, sizeof kbuf);
                    response.append(kbuf);
                }
                response.push_back('=');
                continue;
            }

            if (response.empty()) response.append("\x1b]21");
            response.push_back(';');
            {
                char kbuf[64];
                key.format(kbuf, sizeof kbuf);
                response.append(kbuf);
            }
            response.push_back('=');
            char buf[32];
            const size_t n = maybe_c.value.encodeRgb8(buf, sizeof buf);
            response.append(buf, n);
            break;
        }
        }
    }

    if (!response.empty()) {
        response.append(osc::terminator_string(request.terminator));
        writePty(response.data(), response.size());
    }
    return true;
}

inline void Handler::apcEnd(bool terminated) {
    const zigstd::Allocator alloc = terminal->gpa();
    apc::Command result;
    if (!apc_handler.end(&result)) return;
    switch (result.key) {
    case apc::Command::Key::unknown: {
        if (terminated) {
            UnknownSequence seq;
            seq.tag = UnknownSequence::Tag::apc;
            seq.apc.content = result.unknown.content;
            seq.apc.content_len = result.unknown.content_len;
            seq.apc.truncated = result.unknown.truncated;
            unknownSequence(seq);
        }
        break;
    }
    }
    result.deinit(alloc);
}

/* Reply target for clipboard writes on protocols without a write
 * acknowledgement (OSC 52, OSC 1337 Copy): the reply is accepted
 * and discarded. */
inline void ignoreWriteReply(void *, clip::Write::Result) {}

/* Reply state for one synchronous clipboard read. This lives on the
 * clipboardRead stack frame, so it is only valid during the callback. */
struct ClipboardReadState {
    Handler *handler;
    clip::Location location;
    osc::Terminator terminator;
    bool replied; /* = false */

    ClipboardReadState()
        : handler(nullptr), location(clip::Location::standard), terminator(osc::Terminator::st),
          replied(false) {}

    void respond(ZStr data) {
        std::string aw;
        char kind;
        switch (location) {
        case clip::Location::selection: kind = 's'; break;
        case clip::Location::primary: kind = 'p'; break;
        default: kind = 'c'; break;
        }
        aw.append("\x1b]52;");
        aw.push_back(kind);
        aw.push_back(';');
        zigstd::base64::encodeWriter(&aw, data.ptr, data.len);
        aw.append(osc::terminator_string(terminator));

        handler->writePty(aw.data(), aw.size());
    }

    static void reply(void *ctx, clip::Read::Result result) {
        ClipboardReadState *self = (ClipboardReadState *)ctx;
        if (self->replied) {
            /* log.warn("clipboard read replied more than once, ignoring") */
            return;
        }

        /* OSC 52 carries a single text value. */
        ZStr data = ZStr("", 0);
        switch (result.tag) {
        case clip::Read::Result::Tag::denied:
        case clip::Read::Result::Tag::unsupported:
        case clip::Read::Result::Tag::busy:
        case clip::Read::Result::Tag::io_error: break;
        case clip::Read::Result::Tag::success: {
            for (size_t i = 0; i < result.success.contents_len; i++) {
                if (clip::isTextMime(result.success.contents[i].mime)) {
                    data = result.success.contents[i].data;
                    break;
                }
            }
            break;
        }
        }

        self->respond(data);
        self->replied = true;
    }
};

inline void Handler::clipboardRead(clip::Location location, osc::Terminator terminator) {
    void (*func)(Handler *, clip::Read) = effects.clipboard_read;
    if (func == nullptr) return;

    ClipboardReadState state;
    state.handler = this;
    state.location = location;
    state.terminator = terminator;

    static const ZStr text_plain_mimes[] = {ZStr("text/plain", 10)};
    clip::Read read;
    read.location = location;
    read.mimes = text_plain_mimes;
    read.mimes_len = 1;
    read.list = false;
    read.name = ZStr("", 0);
    read.granted = false;
    read.can_remember = false;
    read.reply_ctx = &state;
    read.reply_fn = &ClipboardReadState::reply;
    func(this, read);

    /* The program is waiting on us, so a callback that returned
     * without a (successful) reply gets an empty clipboard rather
     * than silence. */
    if (!state.replied) state.respond(ZStr("", 0));
}

inline bool Handler::clipboardContents(uint8_t kind, ZStr data, osc::Terminator terminator) {
    clip::Location location;
    switch (kind) {
    case 's': location = clip::Location::selection; break;
    case 'p': location = clip::Location::primary; break;
    default: location = clip::Location::standard; break;
    }

    /* OSC 52 uses a "?" payload to request the clipboard contents. */
    if (data.len == 1 && data.ptr[0] == '?') {
        clipboardRead(location, terminator);
        return true;
    }

    void (*func)(Handler *, clip::Write) = effects.clipboard_write;
    if (func == nullptr) return true;

    /* OSC 52 uses an empty payload to clear the selected clipboard. */
    if (data.len == 0) {
        clip::Write w;
        w.location = location;
        w.contents = nullptr;
        w.contents_len = 0;
        w.name = ZStr("", 0);
        w.granted = false;
        w.can_remember = false;
        w.reply_ctx = this;
        w.reply_fn = &ignoreWriteReply;
        func(this, w);
        return true;
    }

    /* Decode the base64 payload with the SIMD decoder (the same one
     * used for Kitty clipboard payloads) rather than the scalar std
     * implementation; clipboard payloads can be megabytes. The
     * Kitty clipboard spec governs OSC 52 base64 handling too: a
     * request with characters outside the base64 alphabet is
     * discarded entirely (never partially decoded), while a
     * missing-padding tail is tolerated since OSC 52 has no way
     * to report errors to the client. */
    const zigstd::Allocator alloc = terminal->gpa();
    const size_t buf_len = simd::base64::maxLen((const uint8_t *)data.ptr, data.len);
    uint8_t *buf = alloc.allocT<uint8_t>(buf_len);
    if (buf == nullptr) return false;
    size_t decoded_len = 0;
    if (!simd::base64::decodeStrict((const uint8_t *)data.ptr, data.len, buf, buf_len,
                                    simd::base64::Padding::optional, &decoded_len)) {
        /* log.warn("OSC 52 clipboard write is not valid base64, ignoring") */
        alloc.freeT<uint8_t>(buf, buf_len);
        return true;
    }

    const clip::Content contents[] = {clip::Content(ZStr("text/plain", 10), ZStr((const char *)buf, decoded_len))};
    clip::Write w;
    w.location = location;
    w.contents = contents;
    w.contents_len = 1;
    w.name = ZStr("", 0);
    w.granted = false;
    w.can_remember = false;
    w.reply_ctx = this;
    w.reply_fn = &ignoreWriteReply;
    func(this, w);
    alloc.freeT<uint8_t>(buf, buf_len);
    return true;
}

/* Encode and write a single response packet. Unlike kitty, which
 * always terminates responses with ST, we echo the terminator of
 * the request being answered, matching our other OSC responses. */
inline void Handler::kittyClipboardRespond(const kitty_clipboard::Response *response) {
    if (effects.write_pty == nullptr) return;

    std::string aw;
    response->encode(&aw);
    writePty(aw.data(), aw.size());
}

/* Drop any in-flight write transaction without responding. */
inline void Handler::kittyClipboardAbort() {
    if (kitty_clipboard_write != nullptr) {
        const zigstd::Allocator alloc = terminal->gpa();
        kitty_clipboard_write->deinit(alloc);
        alloc.destroy<kitty_clipboard::WriteState>(kitty_clipboard_write);
        kitty_clipboard_write = nullptr;
    }
}

/* Answer a write transaction with its final status and drop it.
 * The id echoed is the one from the transaction's opening write
 * packet, matching kitty. */
inline void Handler::kittyClipboardFinish(const kitty_clipboard::WriteState *state,
                                          kitty_clipboard::Status status, osc::Terminator terminator) {
    kitty_clipboard::Response r;
    r.op = kitty_clipboard::Operation::write;
    r.status = status;
    r.id = state->id;
    r.terminator = terminator;
    kittyClipboardRespond(&r);
    kittyClipboardAbort();
}

/* Reply state for one synchronous Kitty clipboard read. This lives
 * on the kittyClipboardRead stack frame, so it is only valid during
 * the callback. */
struct KittyClipboardReadState {
    Handler *handler;
    bool primary;
    ZStr id;

    /* The effective password, empty when the request had none. */
    ZStr pw;

    /* The requested types; only these are served from a reply. */
    const ZStr *mimes;
    size_t mimes_len;
    bool list;
    osc::Terminator terminator;
    bool replied; /* = false */

    KittyClipboardReadState()
        : handler(nullptr), primary(false), id(), pw(), mimes(nullptr), mimes_len(0), list(false),
          terminator(osc::Terminator::st), replied(false) {}

    /* Answer with a single status packet. */
    void respondStatus(kitty_clipboard::Status status) const {
        kitty_clipboard::Response r;
        r.op = kitty_clipboard::Operation::read;
        r.status = status;
        r.id = id;
        r.terminator = terminator;
        handler->kittyClipboardRespond(&r);
    }

    /* Answer with the full success sequence (OK, listing, DATA
     * chunks, DONE), serving only the requested representations
     * in request order. */
    void respondSuccess(const clip::Read::Result::Success *success) const {
        if (handler->effects.write_pty == nullptr) return;

        clip::Content served_buf[kitty_clipboard::max_read_mimes];
        size_t served_len = 0;
        for (size_t i = 0; i < mimes_len; i++) {
            for (size_t j = 0; j < success->contents_len; j++) {
                const clip::Content &content = success->contents[j];
                if (content.mime.len != mimes[i].len ||
                    memcmp(content.mime.ptr, mimes[i].ptr, mimes[i].len) != 0)
                    continue;
                served_buf[served_len] = content;
                served_len += 1;
                break;
            }
        }

        std::string aw;
        kitty_clipboard::ReadSuccess rs;
        rs.primary = primary;
        rs.id = id;
        rs.list = list;
        rs.available = success->available;
        rs.available_len = success->available_len;
        rs.contents = served_buf;
        rs.contents_len = served_len;
        rs.terminator = terminator;
        rs.encode(&aw);

        handler->writePty(aw.data(), aw.size());
    }

    static void reply(void *ctx, clip::Read::Result result) {
        KittyClipboardReadState *self = (KittyClipboardReadState *)ctx;
        if (self->replied) {
            /* log.warn("clipboard read replied more than once, ignoring") */
            return;
        }
        self->replied = true;

        switch (result.tag) {
        case clip::Read::Result::Tag::denied: self->respondStatus(kitty_clipboard::Status::EPERM_); return;
        case clip::Read::Result::Tag::unsupported: self->respondStatus(kitty_clipboard::Status::ENOSYS_); return;
        case clip::Read::Result::Tag::busy: self->respondStatus(kitty_clipboard::Status::EBUSY_); return;
        case clip::Read::Result::Tag::io_error: self->respondStatus(kitty_clipboard::Status::EIO_); return;
        case clip::Read::Result::Tag::success: break;
        }
        const clip::Read::Result::Success &success = result.success;

        /* Remembering is only offered when the request carried a
         * usable password. */
        if (success.remember && self->pw.len > 0) {
            if (!self->handler->kitty_clipboard_grants.grant(
                    self->handler->terminal->gpa(), (const uint8_t *)self->pw.ptr, self->pw.len,
                    kitty_clipboard::Grants::Direction::read, false)) {
                /* log.warn("error recording clipboard grant err={}") */
            }
        }

        self->respondSuccess(&success);
    }
};

inline bool Handler::kittyClipboardRead(const kitty_clipboard::Metadata *meta, ZStr payload,
                                        osc::Terminator terminator) {
    /* The payload is the requested MIME list. Kitty drops a read
     * request with an undecodable payload without any response. */
    const zigstd::Allocator alloc = terminal->gpa();
    kitty_clipboard::Payload decoded;
    switch (kitty_clipboard::Payload::init(alloc, payload, &decoded)) {
    case kitty_clipboard::Payload::InitError::none: break;
    case kitty_clipboard::Payload::InitError::OutOfMemory: return false;
    case kitty_clipboard::Payload::InitError::Invalid: return true;
    }
    struct PayloadGuard {
        const kitty_clipboard::Payload *p;
        zigstd::Allocator a;
        ~PayloadGuard() { p->deinit(a); }
    } payload_guard = {&decoded, alloc};
    (void)payload_guard;

    if (!decoded.isValidUtf8()) return true;

    /* Without a clipboard_read effect nothing can serve the read.
     * EPERM is the protocol's denial so clients degrade gracefully. */
    void (*func)(Handler *, clip::Read) = effects.clipboard_read;
    if (func == nullptr) {
        kitty_clipboard::Response r;
        r.op = kitty_clipboard::Operation::read;
        r.status = kitty_clipboard::Status::EPERM_;
        r.id = meta->id;
        r.terminator = terminator;
        kittyClipboardRespond(&r);
        return true;
    }

    /* The targets type ('.') asks for the listing of available
     * types rather than data. Requested types beyond the cap are
     * dropped and simply never served, which is how the protocol
     * reports an unavailable type anyway. */
    ZStr mimes_buf[kitty_clipboard::max_read_mimes];
    bool targets = false;
    size_t mimes_len = 0;
    {
        kitty_clipboard::Payload::MimeIterator it = decoded.mimeIterator();
        ZStr mime;
        while (it.next(&mime)) {
            if (mime.eql(kitty_clipboard::targets_mime)) {
                targets = true;
                continue;
            }
            if (mimes_len == kitty_clipboard::max_read_mimes) continue;
            mimes_buf[mimes_len] = mime;
            mimes_len += 1;
        }
    }
    const bool list = targets;

    /* Per the spec a password without a name is no password. A
     * stored grant for it lets the embedder skip its prompt. A
     * prompt-exempt request never consults the grants: the
     * embedder serves it without a prompt anyway, and consuming a
     * one-time paste password on a listing would burn the grant
     * before the follow-up data read. */
    const ZStr pw = meta->name.len > 0 ? meta->pw : ZStr("", 0);
    const bool granted = !kitty_clipboard::readPromptExempt(mimes_len) &&
                         kitty_clipboard_grants.use(alloc, (const uint8_t *)pw.ptr, pw.len,
                                                    kitty_clipboard::Grants::Direction::read);

    KittyClipboardReadState state;
    state.handler = this;
    state.primary = meta->loc == clip::Location::primary;
    state.id = meta->id;
    state.pw = pw;
    state.mimes = mimes_buf;
    state.mimes_len = mimes_len;
    state.list = list;
    state.terminator = terminator;

    clip::Read read;
    read.location = meta->loc;
    read.mimes = mimes_buf;
    read.mimes_len = mimes_len;
    read.list = list;
    read.name = meta->name;
    read.granted = granted;
    read.can_remember = pw.len > 0;
    read.reply_ctx = &state;
    read.reply_fn = &KittyClipboardReadState::reply;
    func(this, read);

    /* The program is waiting on us, so a callback that returned
     * without a reply is answered as a denial rather than silence. */
    if (!state.replied) state.respondStatus(kitty_clipboard::Status::EPERM_);
    return true;
}

inline bool Handler::kittyClipboardWriteBegin(const kitty_clipboard::Metadata *meta,
                                              osc::Terminator terminator) {
    /* A new write silently replaces any in-flight transaction. */
    kittyClipboardAbort();

    /* Without a clipboard_write effect a commit can never succeed,
     * so fail the transaction up front instead of spooling data
     * we'd only throw away. Later wdata packets are ignored. */
    if (effects.clipboard_write == nullptr) {
        kitty_clipboard::Response r;
        r.op = kitty_clipboard::Operation::write;
        r.status = kitty_clipboard::Status::ENOSYS_;
        r.id = meta->id;
        r.terminator = terminator;
        kittyClipboardRespond(&r);
        return true;
    }

    /* Setup our write state */
    const zigstd::Allocator alloc = terminal->gpa();
    kitty_clipboard::WriteState *state = alloc.create<kitty_clipboard::WriteState>();
    if (state == nullptr) return false;
    kitty_clipboard::WriteState::Options opts;
    opts.max_size = kitty_clipboard_write_max_bytes;
    if (!kitty_clipboard::WriteState::init(alloc, meta, opts, state)) {
        alloc.destroy<kitty_clipboard::WriteState>(state);
        return false;
    }
    kitty_clipboard_write = state;
    return true;
}

/* Reply state for one synchronous Kitty clipboard write. This lives
 * on the kittyClipboardCommit stack frame, so it is only valid
 * during the callback. */
struct KittyClipboardWriteReplyState {
    Handler *handler;

    /* The effective password, empty when the request had none. */
    ZStr pw;

    /* The replied commit status, mapped 1:1 from the reply result;
     * null until the callback replies. */
    bool has_status;
    kitty_clipboard::Status status; /* = null */

    KittyClipboardWriteReplyState()
        : handler(nullptr), pw(), has_status(false), status(kitty_clipboard::Status::OK) {}

    static void reply(void *ctx, clip::Write::Result result) {
        KittyClipboardWriteReplyState *self = (KittyClipboardWriteReplyState *)ctx;
        if (self->has_status) {
            /* log.warn("clipboard write replied more than once, ignoring") */
            return;
        }
        self->has_status = true;
        switch (result.tag) {
        case clip::Write::Result::Tag::denied: self->status = kitty_clipboard::Status::EPERM_; break;
        case clip::Write::Result::Tag::unsupported: self->status = kitty_clipboard::Status::ENOSYS_; break;
        case clip::Write::Result::Tag::busy: self->status = kitty_clipboard::Status::EBUSY_; break;
        case clip::Write::Result::Tag::invalid_data: self->status = kitty_clipboard::Status::EINVAL_; break;
        case clip::Write::Result::Tag::io_error: self->status = kitty_clipboard::Status::EIO_; break;
        case clip::Write::Result::Tag::success: {
            /* Remembering is only offered when the request
             * carried a usable password. */
            if (result.success.remember && self->pw.len > 0) {
                if (!self->handler->kitty_clipboard_grants.grant(
                        self->handler->terminal->gpa(), (const uint8_t *)self->pw.ptr, self->pw.len,
                        kitty_clipboard::Grants::Direction::write, false)) {
                    /* log.warn("error recording clipboard grant err={}") */
                }
            }
            self->status = kitty_clipboard::Status::DONE;
            break;
        }
        }
    }
};

inline bool Handler::kittyClipboardCommit(kitty_clipboard::WriteState *state, osc::Terminator terminator) {
    const zigstd::Allocator alloc = terminal->gpa();
    kitty_clipboard::WriteState::Committed committed;
    switch (state->commit(alloc, &committed)) {
    case kitty_clipboard::WriteState::CommitError::none: break;
    case kitty_clipboard::WriteState::CommitError::OutOfMemory:
        kittyClipboardFinish(state, kitty_clipboard::Status::EIO_, terminator);
        return false;
    /* The last MIME type's payload stream was not correctly
     * padded, which aborts the transaction. */
    case kitty_clipboard::WriteState::CommitError::Invalid:
        kittyClipboardFinish(state, kitty_clipboard::Status::EINVAL_, terminator);
        return true;
    }
    struct CommittedGuard {
        const kitty_clipboard::WriteState::Committed *c;
        zigstd::Allocator a;
        ~CommittedGuard() { c->deinit(a); }
    } committed_guard = {&committed, alloc};
    (void)committed_guard;

    /* The effect can't be null here (checked when the transaction
     * began) but if an embedder cleared it mid-transaction that's
     * ENOSYS. */
    void (*func)(Handler *, clip::Write) = effects.clipboard_write;
    if (func == nullptr) {
        kittyClipboardFinish(state, kitty_clipboard::Status::ENOSYS_, terminator);
        return true;
    }

    /* Per the spec a password without a name is no password. A
     * stored grant for it lets the embedder skip its prompt. */
    const ZStr pw = committed.name.len > 0 ? committed.pw : ZStr("", 0);
    const bool granted = kitty_clipboard_grants.use(alloc, (const uint8_t *)pw.ptr, pw.len,
                                                    kitty_clipboard::Grants::Direction::write);

    KittyClipboardWriteReplyState reply_state;
    reply_state.handler = this;
    reply_state.pw = pw;

    clip::Write w;
    w.location = committed.loc;
    w.contents = committed.contents;
    w.contents_len = committed.contents_len;
    w.name = committed.name;
    w.granted = granted;
    w.can_remember = pw.len > 0;
    w.reply_ctx = &reply_state;
    w.reply_fn = &KittyClipboardWriteReplyState::reply;
    func(this, w);

    /* The program is waiting on the commit status, so a callback
     * that returned without a reply is answered as a denial rather
     * than silence. */
    kittyClipboardFinish(state, reply_state.has_status ? reply_state.status : kitty_clipboard::Status::EPERM_,
                         terminator);
    return true;
}

inline bool Handler::kittyClipboardData(const kitty_clipboard::Metadata *meta, ZStr payload,
                                        osc::Terminator terminator) {
    /* Data without a transaction is silently ignored. */
    kitty_clipboard::WriteState *state = kitty_clipboard_write;
    if (state == nullptr) return true;

    /* A wdata packet without a MIME type commits the transaction. */
    if (meta->mime.len == 0) return kittyClipboardCommit(state, terminator);

    switch (state->data(terminal->gpa(), meta, payload)) {
    case kitty_clipboard::WriteState::DataError::none: break;

    /* Failing to spool matches kitty's EIO for a failed buffer
     * write. */
    case kitty_clipboard::WriteState::DataError::OutOfMemory:
        kittyClipboardFinish(state, kitty_clipboard::Status::EIO_, terminator);
        return false;

    /* Data over the write limit aborts the transaction and is
     * reported to the client. */
    case kitty_clipboard::WriteState::DataError::TooLarge:
        kittyClipboardFinish(state, kitty_clipboard::Status::EFBIG_, terminator);
        break;

    /* An invalid base64 payload stream aborts the transaction. */
    case kitty_clipboard::WriteState::DataError::Invalid:
        kittyClipboardFinish(state, kitty_clipboard::Status::EINVAL_, terminator);
        break;
    }
    return true;
}

inline bool Handler::kittyClipboardAlias(const kitty_clipboard::Metadata *meta, ZStr payload,
                                         osc::Terminator terminator) {
    /* Aliases without a transaction are silently ignored. Once a
     * transaction exists, a missing target MIME type is invalid and
     * aborts the transaction. */
    kitty_clipboard::WriteState *state = kitty_clipboard_write;
    if (state == nullptr) return true;
    if (meta->mime.len == 0) {
        kittyClipboardFinish(state, kitty_clipboard::Status::EINVAL_, terminator);
        return true;
    }

    switch (state->alias(terminal->gpa(), meta, payload)) {
    case kitty_clipboard::WriteState::AliasError::none: break;
    case kitty_clipboard::WriteState::AliasError::OutOfMemory:
        kittyClipboardFinish(state, kitty_clipboard::Status::EIO_, terminator);
        return false;

    /* An undecodable alias payload aborts the transaction. */
    case kitty_clipboard::WriteState::AliasError::Invalid:
        kittyClipboardFinish(state, kitty_clipboard::Status::EINVAL_, terminator);
        break;
    }
    return true;
}

/* Handle one Kitty clipboard protocol (OSC 5522) packet. */
inline bool Handler::kittyClipboard(const stream::Action::KittyClipboard &v) {
    /* Decode and validate the metadata. */
    zigstd::ArenaAllocator arena(terminal->gpa());
    struct ArenaGuard {
        zigstd::ArenaAllocator *a;
        ~ArenaGuard() { a->deinit(); }
    } arena_guard = {&arena};
    (void)arena_guard;

    kitty_clipboard::Metadata meta;
    switch (kitty_clipboard::Metadata::parse(arena.allocator(), v.metadata, &meta)) {
    case kitty_clipboard::Metadata::ParseError::none: break;
    case kitty_clipboard::Metadata::ParseError::dropped: return true;
    case kitty_clipboard::Metadata::ParseError::OutOfMemory: return false;
    case kitty_clipboard::Metadata::ParseError::InvalidValue: {
        kitty_clipboard::WriteState *state = kitty_clipboard_write;
        if (state == nullptr) return true;
        kitty_clipboard::Operation op;
        if (!kitty_clipboard::Metadata::operation(v.metadata, &op)) return true;
        switch (op) {
        case kitty_clipboard::Operation::wdata:
        case kitty_clipboard::Operation::walias:
            kittyClipboardFinish(state, kitty_clipboard::Status::EINVAL_, v.terminator);
            break;
        case kitty_clipboard::Operation::read:
        case kitty_clipboard::Operation::write: break;
        }
        return true;
    }
    }

    const ZStr payload = v.has_payload ? v.payload : ZStr("", 0);
    switch (meta.op) {
    case kitty_clipboard::Operation::read: return kittyClipboardRead(&meta, payload, v.terminator);
    case kitty_clipboard::Operation::write: return kittyClipboardWriteBegin(&meta, v.terminator);
    case kitty_clipboard::Operation::wdata: return kittyClipboardData(&meta, payload, v.terminator);
    case kitty_clipboard::Operation::walias: return kittyClipboardAlias(&meta, payload, v.terminator);
    }
    return true;
}

/* Handle an OSC 72 drag and drop command. */
inline bool Handler::kittyDnd(const stream::Action::KittyDnd &v) {
    std::string aw;

    /* The state is allocated on registration and owned by the
     * terminal, so it uses the terminal's allocator, not the
     * response's. */
    osc::Command cmd;
    cmd.key = osc::Command::Key::kitty_dnd_protocol;
    cmd.kitty_dnd_protocol = v;
    bool has_event = false;
    kitty_dnd::Event event = kitty_dnd::Event::registration;
    if (!kitty_dnd::handleCommand(&terminal->kitty_dnd, terminal->gpa(), &aw, cmd, &has_event, &event))
        return false;

    if (!aw.empty()) writePty(aw.data(), aw.size());

    if (has_event) {
        void (*func)(Handler *, kitty_dnd::Event) = effects.drag_and_drop;
        if (func == nullptr) return true;
        func(this, event);
    }
    return true;
}

/* Paste into the terminal, applying the terminal's current state
 * as necessary to owner mode 5522, bracketed paste, unsafe paste, etc.
 * Returns true if anything was written to the pty.
 *
 * The output streams to write_pty in chunks of `paste_chunk_size`.
 * The contents are read at most once and only the pasted text
 * representation is ever read, buffered whole while it is checked
 * and encoded; see `terminal.paste`.
 *
 * Wisp: `PtyWriter` exists upstream to chunk a std.Io.Writer into the
 * write_pty effect. The writer here is a std::string, so the encoded
 * paste is assembled whole and then delivered in paste_chunk_size
 * pieces, which is the same sequence of callback calls for any paste
 * that fits one chunk and the same bytes for any that does not. */
inline Handler::PasteError Handler::paste(const Paste &req, bool *out) {
    *out = false;
    if (effects.write_pty == nullptr) return PasteError::NoWritePty;

    std::string buf;

    paste_pkg::Context ctx;
    ctx.terminal = terminal;
    ctx.alloc = terminal->gpa();
    ctx.writer = &buf;
    /* Paste events need the program's follow-up Kitty
     * clipboard read served. */
    if (effects.clipboard_read != nullptr) {
        ctx.has_kitty_clipboard = true;
        ctx.kitty_clipboard_.grants = &kitty_clipboard_grants;
    }

    bool wrote = false;
    const paste_pkg::Error err = paste_pkg::paste(ctx, req, &wrote);

    /* Delivered on error too: a partial paste has its frame closed
     * and the program must see that. */
    for (size_t i = 0; i < buf.size(); i += paste_chunk_size) {
        const size_t n = buf.size() - i < paste_chunk_size ? buf.size() - i : paste_chunk_size;
        writePty(buf.data() + i, n);
    }

    switch (err) {
    case paste_pkg::Error::none: break;
    case paste_pkg::Error::OutOfMemory: return PasteError::OutOfMemory;
    case paste_pkg::Error::EntropyUnavailable: return PasteError::EntropyUnavailable;
    case paste_pkg::Error::ReadFailed: return PasteError::ReadFailed;
    case paste_pkg::Error::UnsafePaste: return PasteError::UnsafePaste;
    }

    *out = wrote;
    return PasteError::none;
}

inline void Handler::vt(const stream::Action &action) {
    if (!vtFallible(action)) {
        semantic_failure = true;
        /* log.warn("error handling VT action action={} err={}") */
    }
}

inline bool Handler::vtFallible(const stream::Action &action) {
    typedef stream::Action::Key K;
    vt::Terminal *t = terminal;

    switch (action.tag) {
    case K::print: return t->print(action.cp);
    case K::print_slice: return t->printSlice(action.print_slice.cps, action.print_slice.len);
    case K::print_repeat: return t->printRepeat(action.count);
    case K::backspace: t->backspace(); break;
    case K::carriage_return: t->carriageReturn(); break;
    case K::linefeed: return t->linefeed();
    case K::index: return t->index();
    case K::next_line: {
        if (!t->index()) return false;
        t->carriageReturn();
        break;
    }
    case K::reverse_index: t->reverseIndex(); break;
    case K::cursor_up: t->cursorUp(action.cursor.value); break;
    case K::cursor_down: t->cursorDown(action.cursor.value); break;
    case K::cursor_left: t->cursorLeft(action.cursor.value); break;
    case K::cursor_right: t->cursorRight(action.cursor.value); break;
    case K::cursor_pos: t->setCursorPos(action.cursor_pos.row, action.cursor_pos.col); break;
    case K::cursor_col: t->setCursorPos(t->screens.active->cursor.y + 1, action.cursor.value); break;
    case K::cursor_row: t->setCursorPos(action.cursor.value, t->screens.active->cursor.x + 1); break;
    case K::cursor_col_relative:
        t->setCursorPos((size_t)t->screens.active->cursor.y + 1,
                        satAdd((size_t)t->screens.active->cursor.x + 1, action.cursor.value));
        break;
    case K::cursor_row_relative:
        t->setCursorPos(satAdd((size_t)t->screens.active->cursor.y + 1, action.cursor.value),
                        (size_t)t->screens.active->cursor.x + 1);
        break;
    case K::cursor_style: t->setCursorStyle(action.cursor_style); break;
    case K::erase_display_below: t->eraseDisplay(csi::EraseDisplay::below, action.flag); break;
    case K::erase_display_above: t->eraseDisplay(csi::EraseDisplay::above, action.flag); break;
    case K::erase_display_complete: t->eraseDisplay(csi::EraseDisplay::complete, action.flag); break;
    case K::erase_display_scrollback: t->eraseDisplay(csi::EraseDisplay::scrollback, action.flag); break;
    case K::erase_display_scroll_complete:
        t->eraseDisplay(csi::EraseDisplay::scroll_complete, action.flag);
        break;
    case K::erase_line_right: t->eraseLine(csi::EraseLine::right, action.flag); break;
    case K::erase_line_left: t->eraseLine(csi::EraseLine::left, action.flag); break;
    case K::erase_line_complete: t->eraseLine(csi::EraseLine::complete, action.flag); break;
    case K::erase_line_right_unless_pending_wrap:
        t->eraseLine(csi::EraseLine::right_unless_pending_wrap, action.flag);
        break;
    case K::delete_chars: t->deleteChars(action.count); break;
    case K::erase_chars: t->eraseChars(action.count); break;
    case K::insert_lines: t->insertLines(action.count); break;
    case K::insert_blanks: t->insertBlanks(action.count); break;
    case K::delete_lines: t->deleteLines(action.count); break;
    case K::scroll_up: return t->scrollUp(action.count);
    case K::scroll_down: t->scrollDown(action.count); break;
    case K::horizontal_tab: horizontalTab(action.value16); break;
    case K::horizontal_tab_back: horizontalTabBack(action.value16); break;
    case K::tab_clear_current: t->tabClear(csi::TabClear::current); break;
    case K::tab_clear_all: t->tabClear(csi::TabClear::all); break;
    case K::tab_set: t->tabSet(); break;
    case K::tab_reset: t->tabReset(); break;
    case K::set_mode: return setMode(action.mode, true);
    case K::reset_mode: return setMode(action.mode, false);
    case K::save_mode: t->modes.save(action.mode); break;
    case K::restore_mode: {
        const bool prev = t->modes.get(action.mode);
        const bool v = t->modes.restore(action.mode);

        /* Restore writes the value directly. Put the old value
         * back for synchronized output so that setMode can see
         * the change and report the render hold. */
        if (action.mode == modes::Mode::synchronized_output) {
            t->modes.set(action.mode, prev);
        }

        return setMode(action.mode, v);
    }
    case K::top_and_bottom_margin:
        t->setTopAndBottomMargin(action.margin.top_left, action.margin.bottom_right);
        break;
    case K::left_and_right_margin:
        t->setLeftAndRightMargin(action.margin.top_left, action.margin.bottom_right);
        break;
    case K::left_and_right_margin_ambiguous: {
        if (t->modes.get(modes::Mode::enable_left_and_right_margin)) {
            t->setLeftAndRightMargin(0, 0);
        } else {
            t->saveCursor();
        }
        break;
    }
    case K::save_cursor: t->saveCursor(); break;
    case K::restore_cursor: t->restoreCursor(); break;
    case K::invoke_charset:
        t->invokeCharset(action.invoke_charset.bank, action.invoke_charset.charset,
                         action.invoke_charset.locking);
        break;
    case K::configure_charset:
        t->configureCharset(action.configure_charset.slot, action.configure_charset.charset);
        break;
    case K::set_attribute: {
        if (action.set_attribute.tag == sgr::Attribute::Tag::unknown) break;
        return t->setAttribute(action.set_attribute) == vt::PageList::IncreaseCapacityError::none;
    }
    case K::protected_mode_off: t->setProtectedMode(ansi::ProtectedMode::off); break;
    case K::protected_mode_iso: t->setProtectedMode(ansi::ProtectedMode::iso); break;
    case K::protected_mode_dec: t->setProtectedMode(ansi::ProtectedMode::dec); break;
    case K::mouse_shift_capture:
        t->flags.mouse_shift_capture =
            action.flag ? vt::Terminal::Flags::MouseShiftCapture::true_ : vt::Terminal::Flags::MouseShiftCapture::false_;
        break;
    case K::kitty_keyboard_push: t->screens.active->kitty_keyboard.push(action.kitty_flags); break;
    case K::kitty_keyboard_pop: t->screens.active->kitty_keyboard.pop((uint8_t)action.value16); break;
    case K::kitty_keyboard_set:
        t->screens.active->kitty_keyboard.set(kitty::KeySetMode::set, action.kitty_flags);
        break;
    case K::kitty_keyboard_set_or:
        t->screens.active->kitty_keyboard.set(kitty::KeySetMode::or_, action.kitty_flags);
        break;
    case K::kitty_keyboard_set_not:
        t->screens.active->kitty_keyboard.set(kitty::KeySetMode::not_, action.kitty_flags);
        break;
    case K::modify_key_format: {
        t->flags.modify_other_keys_2 = false;
        if (action.modify_key_format == ansi::ModifyKeyFormat::other_keys_numeric) {
            t->flags.modify_other_keys_2 = true;
        }
        break;
    }
    case K::active_status_display: t->status_display = action.active_status_display; break;
    case K::decaln: return t->decaln();
    case K::full_reset: {
        /* A reset turns off synchronized output, ending its hold. */
        const bool sync = t->modes.get(modes::Mode::synchronized_output);
        t->fullReset();
        if (sync) renderHold(false);

        /* Full reset clears grants */
        kitty_clipboard_grants.deinit(t->gpa());
        kitty_clipboard_grants = kitty_clipboard::Grants();

        /* Clear the progress bar */
        {
            osc::ProgressReport r;
            r.state = osc::ProgressReport::State::remove;
            progressReport(r);
        }
        break;
    }
    case K::start_hyperlink:
        return t->screens.active->startHyperlink(
                   (const uint8_t *)action.start_hyperlink.uri.ptr, action.start_hyperlink.uri.len,
                   action.start_hyperlink.has_id ? (const uint8_t *)action.start_hyperlink.id.ptr : nullptr,
                   action.start_hyperlink.has_id ? action.start_hyperlink.id.len : 0) ==
               vt::PageList::IncreaseCapacityError::none;
    case K::end_hyperlink: t->screens.active->endHyperlink(); break;
    case K::semantic_prompt: return t->semanticPrompt(action.semantic_prompt);
    case K::mouse_shape: t->mouse_shape = action.mouse_shape; break;
    case K::color_operation:
        if (!colorOperation(&action.color_operation.requests, action.color_operation.terminator)) {
            /* log.warn("error reporting OSC color err={}") */
        }
        break;
    case K::kitty_color_report:
        if (!kittyColorOperation(action.kitty_color_report)) {
            /* log.warn("error reporting Kitty colors err={}") */
        }
        break;
    case K::kitty_clipboard:
        if (!kittyClipboard(action.kitty_clipboard)) {
            /* Clipboard operations are external effects, not terminal
             * state; a failed transaction was already answered.
             * log.warn("error handling kitty clipboard err={}") */
        }
        break;

    /* APC */
    case K::apc_start: apc_handler.start(); break;
    case K::apc_put: apc_handler.feed(t->gpa(), action.byte); break;
    case K::apc_put_slice:
        apc_handler.feedSlice(t->gpa(), action.apc_put_slice.bytes, action.apc_put_slice.len);
        break;
    case K::apc_end: apcEnd(action.apc_end.terminated); break;

    /* Effect-based handlers */
    case K::bell: bell(); break;
    case K::show_desktop_notification: desktopNotification(action.show_desktop_notification); break;
    case K::device_attributes: reportDeviceAttributes(action.device_attributes); break;
    case K::device_status: deviceStatus(action.device_status); break;
    case K::enquiry: reportEnquiry(); break;
    case K::kitty_keyboard_query: queryKittyKeyboard(); break;
    case K::request_mode: requestMode(action.mode); break;
    case K::request_mode_unknown:
        requestModeUnknown(action.request_mode_unknown.mode, action.request_mode_unknown.ansi);
        break;
    case K::size_report: reportSize(action.size_report); break;
    case K::window_title: return windowTitle(ZStr(action.window_title.title, action.window_title.len));
    case K::report_pwd: return reportPwd(ZStr(action.report_pwd.url, action.report_pwd.len));
    case K::progress_report: progressReport(action.progress_report); break;
    case K::xtversion: reportXtversion(); break;
    case K::clipboard_contents:
        if (!clipboardContents(action.clipboard_contents.kind, action.clipboard_contents.data,
                               action.clipboard_contents.terminator)) {
            /* Clipboard operations are external effects, not terminal state.
             * log.warn("error handling clipboard operation err={}") */
        }
        break;
    case K::kitty_dnd:
        if (!kittyDnd(action.kitty_dnd)) {
            /* Drag and drop is a self-contained subsystem: an OOM
             * updating its state or a failure writing a response
             * degrades it without corrupting terminal state, so we log.
             * log.warn("error handling kitty dnd err={}") */
        }
        break;

    case K::dcs_hook: return dcsHook(action.dcs_hook);
    case K::dcs_put: return dcsPut(action.byte);
    case K::dcs_unhook: return dcsUnhook();

    /* Have no terminal-modifying effect */
    case K::title_push:
    case K::title_pop: break;
    }

    return true;
}

} /* namespace stream_terminal */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_STREAM_TERMINAL_IMPL_HPP */
