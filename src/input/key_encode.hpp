/* Transliterated from Ghostty src/input/key_encode.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp, differences in shape rather than behavior:
 *   - `*std.Io.Writer` is `std::string *`, so encoding cannot fail and the
 *     functions return void (legacyAltPrefix keeps its bool).
 *   - `builtin.os.tag` / `isDarwin()` are resolved for this Windows port:
 *     the macOS-only branches (option-as-alt handling in kitty, legacy
 *     modifyOtherKeys and legacyAltPrefix; command+key text suppression)
 *     are the non-macOS arms. `Options.macos_option_as_alt` is kept since
 *     it is part of the public options.
 *   - Packed structs CsiUMods and KittyMods are plain structs with
 *     `int_()`.
 *   - `?u21` alternates are a presence flag plus value.
 *   - The private `kitty` encoder is `kittyEncode`: `kitty` already names
 *     the kitty.zig namespace here.
 *   - `copyToBuf` is not ported; nothing in key_encode.zig calls it.
 */
#pragma once
#ifndef WISP_INPUT_KEY_ENCODE_HPP
#define WISP_INPUT_KEY_ENCODE_HPP

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <string>
#include <vector>

#include "../terminal/kitty/key.hpp"
#include "../terminal/modes.hpp"
#include "../vt/terminal.hpp"
#include "../zigstd/unicode.hpp"
#include "config.hpp"
#include "function_keys.hpp"
#include "key.hpp"
#include "kitty.hpp"

namespace wisp {
namespace input {
namespace key_encode {

typedef ::wisp::terminal::kitty::KeyFlags KittyFlags;
typedef ::wisp::vt::Terminal Terminal;
typedef kitty::Entry KittyEntry;

/* Options that affect key encoding behavior. This is a mix of behavior
 * from terminal state as well as application configuration. */
struct Options {
    /* Terminal DEC mode 1 */
    bool cursor_key_application = false;

    /* Terminal DEC mode 66 */
    bool keypad_key_application = false;

    /* DEC Backarrow Key Mode (DECBKM)
     * See https://vt100.net/dec/ek-vt3xx-tp-002.pdf page 170
     * If `false` (the default), `backspace` emits 0x7f
     * If `true`, `backspace` emits 0x08 */
    bool backarrow_key_mode = false;

    /* Terminal DEC mode 1035 */
    bool ignore_keypad_with_numlock = false;

    /* Terminal DEC mode 1036 */
    bool alt_esc_prefix = false;

    /* xterm "modifyOtherKeys mode 2". Details here:
     * https://invisible-island.net/xterm/modified-keys.html */
    bool modify_other_keys_state_2 = false;

    /* Kitty keyboard protocol flags. */
    KittyFlags kitty_flags = KittyFlags::disabled();

    /* Determines whether the "option" key on macOS is treated
     * as "alt" or not. See the Ghostty `macos_option-as-alt` config
     * docs for a more detailed description of why this is needed. */
    OptionAsAlt macos_option_as_alt = OptionAsAlt::false_;

    static Options default_() {
        Options o;
        o.cursor_key_application = false;
        o.keypad_key_application = false;
        o.ignore_keypad_with_numlock = false;
        o.alt_esc_prefix = false;
        o.modify_other_keys_state_2 = false;
        o.kitty_flags = KittyFlags::disabled();
        o.macos_option_as_alt = OptionAsAlt::false_;
        return o;
    }

    /* Initialize our options from the terminal state.
     *
     * Note that `macos_option_as_alt` cannot be determined from
     * terminal state so it must be set manually after this call. */
    static Options fromTerminal(const Terminal *t) {
        typedef ::wisp::terminal::modes::Mode Mode;
        Options o;
        o.alt_esc_prefix = t->modes.get(Mode::alt_esc_prefix);
        o.cursor_key_application = t->modes.get(Mode::cursor_keys);
        o.keypad_key_application = t->modes.get(Mode::keypad_keys);
        o.backarrow_key_mode = t->modes.get(Mode::backarrow_key_mode);
        o.ignore_keypad_with_numlock = t->modes.get(Mode::ignore_keypad_with_numlock);
        o.modify_other_keys_state_2 = t->flags.modify_other_keys_2;
        o.kitty_flags = t->screens.active->kitty_keyboard.current();

        /* These can't be known from the terminal state. */
        o.macos_option_as_alt = OptionAsAlt::false_;
        return o;
    }
};

/* Returns true if this is an ASCII control character, matches libc implementation. */
inline bool isControl(uint32_t cp) { return cp < 0x20 || cp == 0x7F; }

/* Returns true if this string is comprised of a single
 * control character. This returns false for multi-byte strings. */
inline bool isControlUtf8(const char *str, size_t len) { return len == 1 && isControl((uint8_t)str[0]); }

/* This is the bitmask for fixterm CSI u modifiers. */
struct CsiUMods {
    bool shift = false;
    bool alt = false;
    bool ctrl = false;

    /* Convert an input mods value into the CSI u mods value. */
    static CsiUMods fromInput(const Mods &mods) {
        CsiUMods m;
        m.shift = mods.shift;
        m.alt = mods.alt;
        m.ctrl = mods.ctrl;
        return m;
    }

    /* Returns the raw int value of this packed struct. */
    uint8_t int_() const { return (uint8_t)((shift ? 1 : 0) | (alt ? 2 : 0) | (ctrl ? 4 : 0)); }

    /* Returns the integer value sent as part of the CSI u sequence.
     * This adds 1 to the bitmask value as described in the spec. */
    uint8_t seqInt() const {
        const uint8_t raw = int_();
        return (uint8_t)(raw + 1);
    }
};

/* This is the bitfields for Kitty modifiers. */
struct KittyMods {
    bool shift = false;
    bool alt = false;
    bool ctrl = false;
    bool super = false;
    bool hyper = false;
    bool meta = false;
    bool caps_lock = false;
    bool num_lock = false;

    /* Convert an input mods value into the CSI u mods value. */
    static KittyMods fromInput(Action action, Key k, const Mods &mods) {
        (void)action;
        (void)k;
        KittyMods m;
        m.shift = mods.shift;
        m.alt = mods.alt;
        m.ctrl = mods.ctrl;
        m.super = mods.super;
        m.caps_lock = mods.caps_lock;
        m.num_lock = mods.num_lock;
        return m;
    }

    /* Returns true if the modifiers prevent printable text.
     *
     * The alt_prevents_text parameter determines whether or not the Alt
     * modifier prevents printable text. On Linux, this is always true. On
     * macOS, this is only true if macos-option-as-alt is set. */
    bool preventsText(bool alt_prevents_text) const {
        return (alt && alt_prevents_text) || ctrl || super || hyper || meta;
    }

    /* Returns the raw int value of this packed struct. */
    uint8_t int_() const {
        return (uint8_t)((shift ? 1 : 0) | (alt ? 2 : 0) | (ctrl ? 4 : 0) | (super ? 8 : 0) | (hyper ? 16 : 0) |
                         (meta ? 32 : 0) | (caps_lock ? 64 : 0) | (num_lock ? 128 : 0));
    }

    /* Returns the integer value sent as part of the Kitty sequence.
     * This adds 1 to the bitmask value as described in the spec. */
    uint16_t seqInt() const {
        const uint16_t raw = int_();
        return (uint16_t)(raw + 1);
    }
};

namespace detail {

inline void print(std::string *writer, const char *fmt, ...) {
    char buf[64];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    if (n > 0) writer->append(buf, (size_t)n);
}

} /* namespace detail */

/* Represents a kitty key sequence and has helpers for encoding it.
 * The sequence from the Kitty specification:
 *
 * CSI unicode-key-code:alternate-key-codes ; modifiers:event-type ; text-as-codepoints u */
struct KittySequence {
    /* Values for the event code (see "event-type" in above comment).
     * Note that Kitty omits the ":1" for the press event but other
     * terminals include it. We'll include it. */
    enum class Event : uint8_t {
        none = 0,
        press = 1,
        repeat = 2,
        release = 3,
    };

    uint32_t key = 0;
    uint8_t final = 0;
    KittyMods mods;
    Event event = Event::none;
    bool has_alternates[2] = {false, false};
    uint32_t alternates[2] = {0, 0};
    const char *text = "";
    size_t text_len = 0;

    void encode(std::string *writer) const {
        if (final == 'u' || final == '~') return encodeFull(writer);
        return encodeSpecial(writer);
    }

    void encodeFull(std::string *writer) const {
        /* Key section */
        detail::print(writer, "\x1B[%u", (unsigned)key);
        /* Write our alternates */
        if (has_alternates[0]) detail::print(writer, ":%u", (unsigned)alternates[0]);
        if (has_alternates[1]) {
            if (!has_alternates[0]) {
                detail::print(writer, "::%u", (unsigned)alternates[1]);
            } else {
                detail::print(writer, ":%u", (unsigned)alternates[1]);
            }
        }

        /* Mods and events section */
        const unsigned seq_mods = mods.seqInt();
        bool emit_prior = false;
        if (event != Event::none && event != Event::press) {
            detail::print(writer, ";%u:%u", seq_mods, (unsigned)event);
            emit_prior = true;
        } else if (seq_mods > 1) {
            detail::print(writer, ";%u", seq_mods);
            emit_prior = true;
        }

        /* Text section */
        if (text_len > 0) {
            zigstd::Utf8View view;
            if (zigstd::Utf8View::init(text, text_len, &view)) {
                size_t count = 0;
                uint32_t cp = 0;
                while (view.nextCodepoint(&cp)) {
                    /* If the codepoint is non-printable ASCII character, skip. */
                    if (isControl(cp)) continue;

                    /* We need to add our ";". We need to add two if we didn't emit
                     * the modifier section. We only do this initially. */
                    if (count == 0) {
                        if (!emit_prior) writer->push_back(';');
                        writer->push_back(';');
                    } else {
                        writer->push_back(':');
                    }

                    detail::print(writer, "%u", (unsigned)cp);
                    count += 1;
                }
            }
            /* Wisp: invalid UTF-8 means we have no text. */
        }

        writer->push_back((char)final);
    }

    void encodeSpecial(std::string *writer) const {
        const unsigned seq_mods = mods.seqInt();
        if (event != Event::none) {
            detail::print(writer, "\x1B[1;%u:%u%c", seq_mods, (unsigned)event, (char)final);
            return;
        }

        if (seq_mods > 1) {
            detail::print(writer, "\x1B[1;%u%c", seq_mods, (char)final);
            return;
        }

        detail::print(writer, "\x1B[%c", (char)final);
    }
};

/* Determines whether the key should be encoded in the xterm
 * "PC-style Function Key" syntax (roughly). This is a hardcoded
 * table of keys and modifiers that result in a specific sequence.
 *
 * Wisp: `?[]const u8` is a pointer that is null when nothing matched. */
inline const std::string *pcStyleFunctionKey(Key keyval, const Mods &mods, bool cursor_key_application,
                                             bool keypad_key_application_req, bool ignore_keypad_with_numlock,
                                             bool modify_other_keys, /* True if state 2 */
                                             bool backarrow_key_mode) {
    typedef function_keys::CursorMode CursorMode;
    typedef function_keys::KeypadMode KeypadMode;
    typedef function_keys::ModifyKeys ModifyKeys;

    /* We only want binding-sensitive mods because lock keys
     * and directional modifiers (left/right) don't matter for
     * pc-style function keys. */
    const Mods::Backing mods_int = mods.binding().int_();

    /* Keypad application keymode isn't super straightforward.
     * On xterm, in VT220 mode, numlock alone is enough to trigger
     * application mode. But in more modern modes, numlock is
     * ignored by default via mode 1035 (default true). If mode
     * 1035 is on, we always are in numerical keypad mode. If
     * mode 1035 is off, we are in application mode if the
     * proper numlock state is pressed. The numlock state is implicitly
     * determined based on the keycode sent (i.e. 1 with numlock
     * on will be kp_end). */
    bool keypad_key_application;
    /* If we're ignoring keypad then this is always false.
     * In other words, we're always in numerical keypad mode. */
    if (ignore_keypad_with_numlock) {
        keypad_key_application = false;
    } else {
        /* If we're not ignoring then we enable the desired state. */
        keypad_key_application = keypad_key_application_req;
    }

    for (const function_keys::Entry &entry : function_keys::keys().get(keyval)) {
        switch (entry.cursor) {
            case CursorMode::any: break;
            case CursorMode::normal:
                if (cursor_key_application) continue;
                break;
            case CursorMode::application:
                if (!cursor_key_application) continue;
                break;
        }

        switch (entry.keypad) {
            case KeypadMode::any: break;
            case KeypadMode::normal:
                if (keypad_key_application) continue;
                break;
            case KeypadMode::application:
                if (!keypad_key_application) continue;
                break;
        }

        switch (entry.modify_other_keys) {
            case ModifyKeys::any: break;
            case ModifyKeys::set:
                if (modify_other_keys) continue;
                break;
            case ModifyKeys::set_other:
                if (!modify_other_keys) continue;
                break;
        }

        const Mods::Backing entry_mods_int = entry.mods.int_();
        if (entry_mods_int == 0) {
            if (mods_int != 0 && !entry.mods_empty_is_any) continue;
            /* mods are either empty, or empty means any so we allow it. */
        } else if (entry_mods_int != mods_int) {
            /* any set mods require an exact match */
            continue;
        }

        if (backarrow_key_mode)
            if (entry.has_sequence_decbkm) return &entry.sequence_decbkm;

        return &entry.sequence;
    }

    return nullptr;
}

/* Returns the C0 byte for the key event if it should be used.
 * This converts a key event into the expected terminal behavior
 * such as Ctrl+C turning into 0x03, amongst many other translations.
 *
 * This will return null if the key event should not be converted
 * into a C0 byte. There are many cases for this and you should read
 * the source code to understand them.
 *
 * Wisp: `?u8` is the bool return plus `*out`. */
inline bool ctrlSeq(Key logical_key, const char *utf8, size_t utf8_len, uint32_t unshifted_codepoint,
                    const Mods &mods, uint8_t *out) {
    Mods ctrl_only_mods;
    ctrl_only_mods.ctrl = true;
    const Mods::Backing ctrl_only = ctrl_only_mods.int_();

    /* If ctrl is not pressed then we never do anything. */
    if (!mods.ctrl) return false;

    uint8_t ch = 0;
    Mods unset_mods;
    {
        /* We need to only get binding modifiers so we strip lock
         * keys, sides, etc. */
        unset_mods = mods.binding();

        /* Remove alt from our modifiers because it does not impact whether
         * we are generating a ctrl sequence and we handle the ESC-prefix
         * logic separately. */
        unset_mods.alt = false;

        /* If we have exactly one UTF8 byte, we assume that is the
         * character we want to convert to a C0 byte. */
        if (utf8_len == 1) {
            ch = (uint8_t)utf8[0];
        } else {
            /* If we have a logical key that maps to a single byte
             * printable character, we use that. History to explain this:
             * this was added to support cyrillic keyboard layouts such
             * as Russian and Mongolian. These layouts have a `c` key that
             * maps to U+0441 (cyrillic small letter "c") but every
             * terminal I've tested encodes this as ctrl+c. */
            uint32_t cp = 0;
            if (codepoint(logical_key, &cp) && cp <= 0xFF) {
                /* For this specific case, we only map to the key if
                 * we have exactly ctrl pressed. This is because shift
                 * would modify the key and we don't know how to do that
                 * properly here (don't have the layout). And we want
                 * to encode shift as CSIu. */
                if (unset_mods.int_() != ctrl_only) return false;
                ch = (uint8_t)cp;
            } else {
                /* Otherwise we don't have a character to convert that
                 * we can reliably map to a C0 byte. */
                return false;
            }
        }

        /* Remove shift if we have something outside of the US letter
         * range. This is so that characters such as `ctrl+shift+-`
         * generate the correct ctrl-seq (used by emacs). */
        if (unset_mods.shift && (ch < 'A' || ch > 'Z')) {
            /* Special case for fixterms awkward case as specified. */
            if (ch != '@') unset_mods.shift = false;
        }

        /* If the character is uppercase, we convert it to lowercase. We
         * rely on the unshifted codepoint to do this. This handles
         * the scenario where we have caps lock pressed. Note that
         * shifted characters are handled above, if we are just pressing
         * shift then the ctrl-only check will fail later and we won't
         * ctrl-seq encode. */
        if (ch >= 'A' && ch <= 'Z' && unshifted_codepoint > 0) {
            if (unshifted_codepoint <= 0xFF) {
                ch = (uint8_t)unshifted_codepoint;
            }
        }

        /* An additional note on caps lock and shift interaction.
         * If we have caps lock set and an ASCII letter is pressed,
         * we lowercase it (above). If we have only control pressed,
         * we process it as a ctrl seq. For example ctrl+M with caps
         * lock but no shift will encode as 0x0D.
         *
         * But, if you press ctrl+shift+m, this will not encode as a
         * ctrl-seq and falls through to CSIu encoding. This lets programs
         * detect the difference between ctrl+M and ctrl+shift+M. This
         * diverges from the fixterms "spec" and most terminals. This
         * only matches Kitty in behavior. But I believe this is a
         * justified divergence because it's a useful distinction. */
    }

    /* After unsetting, we only continue if we have ONLY control set. */
    if (unset_mods.int_() != ctrl_only) return false;

    /* From Kitty's key encoding logic. I tried to discern the exact
     * behavior across different terminals but it's not clear, so I'm
     * just going to repeat what Kitty does. */
    switch (ch) {
        case ' ': *out = 0; return true;
        case '/': *out = 31; return true;
        case '0': *out = 48; return true;
        case '1': *out = 49; return true;
        case '2': *out = 0; return true;
        case '3': *out = 27; return true;
        case '4': *out = 28; return true;
        case '5': *out = 29; return true;
        case '6': *out = 30; return true;
        case '7': *out = 31; return true;
        case '8': *out = 127; return true;
        case '9': *out = 57; return true;
        case '?': *out = 127; return true;
        case '@': *out = 0; return true;
        case '\\': *out = 28; return true;
        case ']': *out = 29; return true;
        case '^': *out = 30; return true;
        case '_': *out = 31; return true;
        case 'a': *out = 1; return true;
        case 'b': *out = 2; return true;
        case 'c': *out = 3; return true;
        case 'd': *out = 4; return true;
        case 'e': *out = 5; return true;
        case 'f': *out = 6; return true;
        case 'g': *out = 7; return true;
        case 'h': *out = 8; return true;
        case 'j': *out = 10; return true;
        case 'k': *out = 11; return true;
        case 'l': *out = 12; return true;
        case 'n': *out = 14; return true;
        case 'o': *out = 15; return true;
        case 'p': *out = 16; return true;
        case 'q': *out = 17; return true;
        case 'r': *out = 18; return true;
        case 's': *out = 19; return true;
        case 't': *out = 20; return true;
        case 'u': *out = 21; return true;
        case 'v': *out = 22; return true;
        case 'w': *out = 23; return true;
        case 'x': *out = 24; return true;
        case 'y': *out = 25; return true;
        case 'z': *out = 26; return true;
        case '~': *out = 30; return true;

        /* These are purposely NOT handled here because of the fixterms
         * specification: https://www.leonerd.org.uk/hacks/fixterms/
         * These are processed as CSI u.
         * 'i' => 0x09,
         * 'm' => 0x0D,
         * '[' => 0x1B, */

        default: return false;
    }
}

inline bool legacyAltPrefix(std::string *writer, const KeyEvent &event, const Mods &binding_mods,
                            const Mods &mods, const Options &opts) {
    (void)mods;

    /* This only takes effect with alt pressed */
    if (!binding_mods.alt || !opts.alt_esc_prefix) return false;

    /* On macOS, we only handle option like alt in certain
     * circumstances. Otherwise, macOS does a unicode translation
     * and we allow that to happen.
     *
     * Wisp: macOS only; see the header. */

    /* A single byte is already the exact text we want to prefix. In
     * particular, this preserves shifted ASCII punctuation. */
    if (event.utf8_len == 1) {
        writer->push_back('\x1B');
        writer->append(event.utf8, event.utf8_len);
        return true;
    }

    uint8_t unshifted_buf[4];
    const char *value = nullptr;
    size_t value_len = 0;
    /* On macOS, Option may translate the text into a different Unicode
     * value. When Option is configured as Alt, use the physical key's
     * unshifted codepoint just as the single-byte implementation did.
     *
     * Wisp: macOS only; see the header. */

    if (event.utf8_len > 0) {
        /* Outside of the macOS translation case, prefix the complete UTF-8
         * text rather than truncating it to a single byte. */
        value = event.utf8;
        value_len = event.utf8_len;
    } else if (event.unshifted_codepoint > 0) {
        /* Frontends may omit UTF-8 while still supplying the physical key's
         * unshifted codepoint. Encode the complete scalar in that case. */
        const size_t len = zigstd::utf8Encode(event.unshifted_codepoint, unshifted_buf);
        if (len == 0) return false;
        value = (const char *)unshifted_buf;
        value_len = len;
    } else {
        return false;
    }

    writer->push_back('\x1B');
    writer->append(value, value_len);
    return true;
}

/* Perform legacy encoding of the key event. "Legacy" in this case
 * is referring to the behavior of traditional terminals, plus
 * xterm's `modifyOtherKeys`, plus Paul Evans's "fixterms" spec.
 * These together combine the legacy protocol because they're all
 * meant to be extensions that do not change any existing behavior
 * and therefore safe to combine. */
inline void legacy(std::string *writer, const KeyEvent &event, const Options &opts) {
    const Mods all_mods = event.mods;
    const Mods effective_mods = event.effectiveMods();
    const Mods binding_mods = effective_mods.binding();

    /* Legacy encoding only does press/repeat */
    if (event.action != Action::press && event.action != Action::repeat) return;

    /* If we're in a dead key state then we never emit a sequence. */
    if (event.composing) return;

    /* If we match a PC style function key then that is our result. */
    if (const std::string *sequence = pcStyleFunctionKey(
            event.key, all_mods, opts.cursor_key_application, opts.keypad_key_application,
            opts.ignore_keypad_with_numlock, opts.modify_other_keys_state_2, opts.backarrow_key_mode)) {
        bool use_pc_style = true;

        /* If we have UTF-8 text, then we never emit PC style function
         * keys. Many function keys (escape, enter, backspace) have
         * a specific meaning when dead keys are active and so we don't
         * want to send that to the terminal. Examples:
         *
         *   - Japanese: escape clears the dead key state
         *   - Korean: escape commits the dead key state
         *   - Korean: backspace should delete a single preedit char
         * */
        if (event.utf8_len > 0) {
            switch (event.key) {
                case Key::backspace:
                case Key::enter:
                case Key::escape:
                    /* We want to ignore control characters. This is because
                     * some apprts (macOS) will send control characters as
                     * UTF-8 encodings and we handle that manually. */
                    if (isControlUtf8(event.utf8, event.utf8_len)) break;

                    /* Backspace encodes nothing because we modified IME.
                     * Enter/escape don't encode the PC-style encoding
                     * because we want to encode committed text. */
                    if (event.key == Key::backspace) return;
                    use_pc_style = false;
                    break;
                default: break;
            }
        }

        if (use_pc_style) {
            writer->append(*sequence);
            return;
        }
    }

    /* In modify other keys state 2, we send the CSI 27 sequence
     * for any char with a modifier. This must happen before converting
     * Ctrl+<char> to a C0 byte because mode 2 encodes those keys, too. */
    const char *utf8 = event.utf8;
    const size_t utf8_len = event.utf8_len;
    if (opts.modify_other_keys_state_2) {
        zigstd::Utf8View view;
        uint32_t cp = 0;
        uint32_t extra = 0;
        /* Assume invalid UTF-8 means we no UTF-8. */
        if (zigstd::Utf8View::init(utf8, utf8_len, &view) && view.nextCodepoint(&cp) &&
            /* We only do this if we have a single codepoint. There shouldn't
             * ever be a multi-codepoint sequence that triggers this. */
            !view.nextCodepoint(&extra)) {
            /* The mods we encode for this are just the binding mods (shift, ctrl,
             * super, alt unless it is actually option).
             *
             * Wisp: the macOS option-as-alt adjustment; see the header. */
            const Mods mods = event.mods.binding();

            /* This copies xterm's `ModifyOtherKeys` function that returns
             * whether modify other keys should be encoded for the given
             * input. */
            bool should_modify;
            if (cp >= 0x40 && cp <= 0x7F) {
                /* xterm IsControlInput */
                should_modify = true;
            } else {
                /* If we have anything other than shift pressed, encode. */
                Mods mods_no_shift = mods;
                mods_no_shift.shift = false;
                if (!mods_no_shift.empty()) {
                    should_modify = true;
                } else if (cp == ' ') {
                    /* We only have shift pressed. We only allow space. */
                    should_modify = true;
                } else {
                    /* This logic isn't complete but I don't fully understand
                     * the rest so I'm going to wait until we can have a
                     * reasonable test scenario. */
                    should_modify = false;
                }
            }

            if (should_modify) {
                const std::vector<Mods> &modifiers = function_keys::modifiers();
                for (size_t i = 0; i < modifiers.size(); i++) {
                    const size_t code = i + 2;
                    if (!mods.equal(modifiers[i])) continue;
                    detail::print(writer, "\x1B[27;%u;%u~", (unsigned)code, (unsigned)cp);
                    return;
                }
            }
        }
    }

    /* If we match a control sequence, we output that directly. For
     * ctrlSeq we have to use all mods because we want it to only
     * match ctrl+<char>. */
    uint8_t ch = 0;
    if (ctrlSeq(event.key, event.utf8, event.utf8_len, event.unshifted_codepoint, all_mods, &ch)) {
        /* C0 sequences support alt-as-esc prefixing. */
        if (binding_mods.alt) {
            writer->push_back('\x1B');
            writer->push_back((char)ch);
            return;
        }

        writer->push_back((char)ch);
        return;
    }

    /* If we have no UTF8 text then the only possibility is the
     * alt-prefix handling of unshifted codepoints... so we process that. */
    if (utf8_len == 0) {
        (void)legacyAltPrefix(writer, event, binding_mods, all_mods, opts);
        return;
    }

    /* Let's see if we should apply fixterms to this codepoint.
     * At this stage of key processing, we only need to apply fixterms
     * to unicode codepoints if we have ctrl set. */
    if (event.mods.ctrl) {
        /* Important: we want to use the original mods here, not the
         * effective mods. The fixterms spec states the shifted chars
         * should be sent uppercase but Kitty changes that behavior
         * so we'll send all the mods. */
        CsiUMods mods = CsiUMods::fromInput(event.mods);

        /* Get our codepoint. If we have more than one codepoint this
         * can't be valid CSIu. */
        zigstd::Utf8View view;
        uint32_t c = 0;
        uint32_t extra = 0;
        if (zigstd::Utf8View::init(event.utf8, event.utf8_len, &view) && view.nextCodepoint(&c) &&
            !view.nextCodepoint(&extra)) {
            /* If our character is A to Z and we have shift set, then
             * we lowercase it. This is a Kitty-specific behavior that
             * we choose to follow and diverge from the fixterms spec.
             * This makes it easier for programs to detect shifted letters
             * for keybindings and is not just theoretical but used by
             * real programs. */
            if (c >= 'A' && c <= 'Z' && mods.shift) {
                /* We want to rely on apprt to send us the correct
                 * unshifted codepoint... */
                c = c - 'A' + 'a';
            }

            /* If our unshifted codepoint is identical to the shifted
             * then we consider shift. Otherwise, we do not because the
             * shift key was used to obtain the character. This is specified
             * by fixterms. */
            if (event.unshifted_codepoint != c) {
                mods.shift = false;
            }

            detail::print(writer, "\x1B[%u;%uu", (unsigned)c, (unsigned)mods.seqInt());
            return;
        }
    }

    /* If we have alt-pressed and alt-esc-prefix is enabled, then
     * we need to prefix the utf8 sequence with an esc. */
    if (legacyAltPrefix(writer, event, binding_mods, all_mods, opts)) return;

    /* If we are on macOS, command+keys do not encode text. It isn't
     * typical for command+keys on macOS to ever encode text. They
     * don't in native text inputs (i.e. TextEdit) and they also don't
     * in other native terminals (Terminal.app officially but also
     * iTerm2).
     *
     * For Linux, we continue to encode text because it is typical.
     * For example on Gnome Console Super+b will encode a "b" character
     * with legacy encoding.
     *
     * Wisp: macOS only; see the header. */

    writer->append(utf8, utf8_len);
}

/* Perform Kitty keyboard protocol encoding of the key event. */
inline void kittyEncode(std::string *writer, const KeyEvent &event, const Options &opts) {
    /* This should never happen but we'll check anyway. */
    if (opts.kitty_flags.int_() == 0) return legacy(writer, event, opts);

    /* We only processed "press" events unless report events is active */
    if (event.action == Action::release) {
        if (!opts.kitty_flags.report_events) return;

        /* Enter, backspace, and tab do not report release events unless "report
         * all" is set */
        if (!opts.kitty_flags.report_all) {
            switch (event.key) {
                case Key::enter:
                case Key::backspace:
                case Key::tab: return;
                default: break;
            }
        }
    }

    const Mods all_mods = event.mods;
    const Mods effective_mods = event.effectiveMods();
    const Mods binding_mods = effective_mods.binding();

    /* Find the entry for this key in the kitty table. */
    bool has_entry = false;
    KittyEntry entry_ = {Key::unidentified, 0, 0, false};
    {
        /* Functional or predefined keys */
        for (size_t i = 0; i < kitty::entries_len; i++) {
            if (kitty::entries[i].key == event.key) {
                entry_ = kitty::entries[i];
                has_entry = true;
                break;
            }
        }

        /* Otherwise, we use our unicode codepoint from UTF8. We
         * always use the unshifted value. */
        if (!has_entry && event.unshifted_codepoint > 0) {
            entry_.key = event.key;
            entry_.code = event.unshifted_codepoint;
            entry_.final = 'u';
            entry_.modifier = false;
            has_entry = true;
        }
    }

    do { /* preprocessing */
        /* When composing, the only keys sent are plain modifiers. */
        if (event.composing) {
            if (has_entry && entry_.modifier) break;

            return;
        }

        /* IME confirmation still sends an enter key so if we have enter
         * and UTF8 text we just send it directly since we assume that is
         * what's happening. See legacy()'s similar logic for more details
         * on how to verify this. */
        if (event.utf8_len > 0) {
            switch (event.key) {
                case Key::enter:
                case Key::backspace:
                    /* See legacy for why we handle this way. */
                    if (isControlUtf8(event.utf8, event.utf8_len)) break;
                    if (event.key == Key::backspace) return;
                    writer->append(event.utf8, event.utf8_len);
                    return;
                default: break;
            }
        }

        /* If we're reporting all then we always send CSI sequences. */
        if (!opts.kitty_flags.report_all) {
            /* Quote:
             * The only exceptions are the Enter, Tab and Backspace keys which
             * still generate the same bytes as in legacy mode this is to allow the
             * user to type and execute commands in the shell such as reset after a
             * program that sets this mode crashes without clearing it.
             *
             * Quote ("report all" mode):
             * Note that all keys are reported as escape codes, including Enter,
             * Tab, Backspace etc. */
            if (binding_mods.empty()) {
                switch (event.key) {
                    case Key::enter: writer->push_back('\r'); return;
                    case Key::tab: writer->push_back('\t'); return;
                    case Key::backspace: writer->push_back((char)0x7F); return;
                    default: break;
                }
            }

            /* Send plain-text non-modified text directly to the terminal.
             * We don't send release events because those are specially encoded. */
            if (event.utf8_len > 0 && binding_mods.empty() && event.action != Action::release) {
                /* We only do this for printable characters. We should
                 * inspect the real unicode codepoint properties here but
                 * the real world issue is usually control characters. */
                zigstd::Utf8View view;
                if (zigstd::Utf8View::init(event.utf8, event.utf8_len, &view)) {
                    bool printable_text = true;
                    uint32_t cp = 0;
                    while (view.nextCodepoint(&cp)) {
                        if (isControl(cp)) {
                            printable_text = false;
                            break;
                        }
                    }

                    if (printable_text) {
                        writer->append(event.utf8, event.utf8_len);
                        return;
                    }
                }
                /* Invalid UTF-8 so let's fallback to encoding the
                 * key press as if it didn't produce UTF-8 text. I'm
                 * not sure what should happen here according to the spec,
                 * since it doesn't specify this behavior. Presumably
                 * this is a caller bug. */
            }
        }
    } while (false);

    if (!has_entry) {
        /* No entry found. If we have UTF-8 text this is a pure text event
         * (e.g. composed/IME text), so send it as-is so programs can
         * still receive it. Release events never insert text, same as the
         * plain-text path above. */
        if (event.action == Action::release) return;
        if (event.utf8_len == 0) return;
        writer->append(event.utf8, event.utf8_len);
        return;
    }
    const KittyEntry entry = entry_;

    /* If this is just a modifier we require "report all" to send the sequence. */
    if (entry.modifier && !opts.kitty_flags.report_all) return;

    KittySequence seq;
    seq.key = entry.code;
    seq.final = entry.final;
    seq.mods = KittyMods::fromInput(event.action, event.key, all_mods);

    if (opts.kitty_flags.report_events) {
        switch (event.action) {
            case Action::press: seq.event = KittySequence::Event::press; break;
            case Action::release: seq.event = KittySequence::Event::release; break;
            case Action::repeat: seq.event = KittySequence::Event::repeat; break;
        }
    }

    if (opts.kitty_flags.report_alternates) {
        zigstd::Utf8View view;
        /* Break early if this is a control key */
        /* Assume invalid UTF-8 means no UTF-8. */
        if (!isControl(seq.key) && zigstd::Utf8View::init(event.utf8, event.utf8_len, &view)) {
            /* If we have a codepoint in our UTF-8 sequence, then we can
             * report the shifted version. */
            uint32_t cp1 = 0;
            if (view.nextCodepoint(&cp1)) {
                /* Set the first alternate (shifted version) */
                if (cp1 != seq.key && seq.mods.shift) {
                    seq.has_alternates[0] = true;
                    seq.alternates[0] = cp1;
                }

                /* We want to know if there are additional codepoints because
                 * our logic below depends on the utf8 being a single codepoint. */
                uint32_t cp2 = 0;
                const bool has_cp2 = view.nextCodepoint(&cp2);

                /* Set the base layout key. We only report this if this codepoint
                 * differs from our pressed key. */
                uint32_t base = 0;
                if (codepoint(event.key, &base)) {
                    if (base != seq.key && (cp1 != base && !has_cp2)) {
                        seq.has_alternates[1] = true;
                        seq.alternates[1] = base;
                    }
                }
            } else {
                /* No UTF-8 so we can't report a shifted key but we can still
                 * report a base layout key. */
                uint32_t base = 0;
                if (codepoint(event.key, &base)) {
                    if (base != seq.key) {
                        seq.has_alternates[1] = true;
                        seq.alternates[1] = base;
                    }
                }
            }
        }
    }

    if (opts.kitty_flags.report_associated && seq.event != KittySequence::Event::release) {
        /* Determine if the Alt modifier should be treated as an actual
         * modifier (in which case it prevents associated text) or as
         * the macOS Option key, which does not prevent associated text.
         *
         * Wisp: the non-macOS arm; see the header. */
        const bool alt_prevents_text = true;

        if (!seq.mods.preventsText(alt_prevents_text)) {
            seq.text = event.utf8;
            seq.text_len = event.utf8_len;
        }
    }

    seq.encode(writer);
}

/* Encode the key event to the writer in the proper format given
 * the options. For example, this will properly encode a key press
 * such as "ctrl+A" to Kitty format if Kitty encoding is enabled.
 *
 * Not all key events will result in output. It is up to the caller
 * to use a writer that can track whether any output was written if
 * they care about that. */
inline void encode(std::string *writer, const KeyEvent &event, const Options &opts) {
    if (opts.kitty_flags.int_() != 0) return kittyEncode(writer, event, opts);
    return legacy(writer, event, opts);
}

} /* namespace key_encode */
} /* namespace input */
} /* namespace wisp */

#endif /* WISP_INPUT_KEY_ENCODE_HPP */
