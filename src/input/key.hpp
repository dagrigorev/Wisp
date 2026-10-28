/* Transliterated from Ghostty src/input/key.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp, differences in shape rather than behavior:
 *   - `Key` is an enum class whose declaration order, and so whose integer
 *     values, match upstream. Tags that are C++ keywords get a trailing
 *     underscore (`delete_`); `keyName` returns upstream's tag names, which
 *     is what `fromW3C`, `w3c` and `keypad` operate on.
 *   - Comptime tables become runtime lookups over `codepoint_map` and the
 *     tag-name table with identical results.
 *   - `imguiKey` is not ported: Wisp has no Dear ImGui (the inspector is
 *     not part of this port).
 *   - `builtin.target.os.tag.isDarwin()` is resolved for this Windows port:
 *     `ctrlOrSuper` uses ctrl.
 *   - `bindingHash` feeds Wyhash the bytes `std.hash.autoHash` would: the
 *     c_int key (4 bytes), the u21 codepoint (3 bytes), and the u16 binding
 *     mods (2 bytes), little-endian.
 */
#pragma once
#ifndef WISP_INPUT_KEY_HPP
#define WISP_INPUT_KEY_HPP

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

#include "../zigstd/wyhash.hpp"
#include "key_mods.hpp"

namespace wisp {
namespace input {

/* The action associated with an input event. This is backed by a c_int
 * so that we can use the enum as-is for our embedding API.
 *
 * IMPORTANT: Any changes here update include/ghostty.h */
enum class Action : int {
    release,
    press,
    repeat,
};

/* Wisp: X(cpp_name, "upstream tag name") in upstream declaration order. */
#define WISP_INPUT_KEYS(X)                                                  \
    X(unidentified, "unidentified")                                         \
    /* "Writing System Keys" § 3.1.1 */                                     \
    X(backquote, "backquote")                                               \
    X(backslash, "backslash")                                               \
    X(bracket_left, "bracket_left")                                         \
    X(bracket_right, "bracket_right")                                       \
    X(comma, "comma")                                                       \
    X(digit_0, "digit_0")                                                   \
    X(digit_1, "digit_1")                                                   \
    X(digit_2, "digit_2")                                                   \
    X(digit_3, "digit_3")                                                   \
    X(digit_4, "digit_4")                                                   \
    X(digit_5, "digit_5")                                                   \
    X(digit_6, "digit_6")                                                   \
    X(digit_7, "digit_7")                                                   \
    X(digit_8, "digit_8")                                                   \
    X(digit_9, "digit_9")                                                   \
    X(equal, "equal")                                                       \
    X(intl_backslash, "intl_backslash")                                     \
    X(intl_ro, "intl_ro")                                                   \
    X(intl_yen, "intl_yen")                                                 \
    X(key_a, "key_a")                                                       \
    X(key_b, "key_b")                                                       \
    X(key_c, "key_c")                                                       \
    X(key_d, "key_d")                                                       \
    X(key_e, "key_e")                                                       \
    X(key_f, "key_f")                                                       \
    X(key_g, "key_g")                                                       \
    X(key_h, "key_h")                                                       \
    X(key_i, "key_i")                                                       \
    X(key_j, "key_j")                                                       \
    X(key_k, "key_k")                                                       \
    X(key_l, "key_l")                                                       \
    X(key_m, "key_m")                                                       \
    X(key_n, "key_n")                                                       \
    X(key_o, "key_o")                                                       \
    X(key_p, "key_p")                                                       \
    X(key_q, "key_q")                                                       \
    X(key_r, "key_r")                                                       \
    X(key_s, "key_s")                                                       \
    X(key_t, "key_t")                                                       \
    X(key_u, "key_u")                                                       \
    X(key_v, "key_v")                                                       \
    X(key_w, "key_w")                                                       \
    X(key_x, "key_x")                                                       \
    X(key_y, "key_y")                                                       \
    X(key_z, "key_z")                                                       \
    X(minus, "minus")                                                       \
    X(period, "period")                                                     \
    X(quote, "quote")                                                       \
    X(semicolon, "semicolon")                                               \
    X(slash, "slash")                                                       \
    /* "Functional Keys" § 3.1.2 */                                         \
    X(alt_left, "alt_left")                                                 \
    X(alt_right, "alt_right")                                               \
    X(backspace, "backspace")                                               \
    X(caps_lock, "caps_lock")                                               \
    X(context_menu, "context_menu")                                         \
    X(control_left, "control_left")                                         \
    X(control_right, "control_right")                                       \
    X(enter, "enter")                                                       \
    X(meta_left, "meta_left")                                               \
    X(meta_right, "meta_right")                                             \
    X(shift_left, "shift_left")                                             \
    X(shift_right, "shift_right")                                           \
    X(space, "space")                                                       \
    X(tab, "tab")                                                           \
    X(convert, "convert")                                                   \
    X(kana_mode, "kana_mode")                                               \
    X(non_convert, "non_convert")                                           \
    /* "Control Pad Section" § 3.2 */                                       \
    X(delete_, "delete")                                                    \
    X(end, "end")                                                           \
    X(help, "help")                                                         \
    X(home, "home")                                                         \
    X(insert, "insert")                                                     \
    X(page_down, "page_down")                                               \
    X(page_up, "page_up")                                                   \
    /* "Arrow Pad Section" § 3.3 */                                         \
    X(arrow_down, "arrow_down")                                             \
    X(arrow_left, "arrow_left")                                             \
    X(arrow_right, "arrow_right")                                           \
    X(arrow_up, "arrow_up")                                                 \
    /* "Numpad Section" § 3.4 */                                            \
    X(num_lock, "num_lock")                                                 \
    X(numpad_0, "numpad_0")                                                 \
    X(numpad_1, "numpad_1")                                                 \
    X(numpad_2, "numpad_2")                                                 \
    X(numpad_3, "numpad_3")                                                 \
    X(numpad_4, "numpad_4")                                                 \
    X(numpad_5, "numpad_5")                                                 \
    X(numpad_6, "numpad_6")                                                 \
    X(numpad_7, "numpad_7")                                                 \
    X(numpad_8, "numpad_8")                                                 \
    X(numpad_9, "numpad_9")                                                 \
    X(numpad_add, "numpad_add")                                             \
    X(numpad_backspace, "numpad_backspace")                                 \
    X(numpad_clear, "numpad_clear")                                         \
    X(numpad_clear_entry, "numpad_clear_entry")                             \
    X(numpad_comma, "numpad_comma")                                         \
    X(numpad_decimal, "numpad_decimal")                                     \
    X(numpad_divide, "numpad_divide")                                       \
    X(numpad_enter, "numpad_enter")                                         \
    X(numpad_equal, "numpad_equal")                                         \
    X(numpad_memory_add, "numpad_memory_add")                               \
    X(numpad_memory_clear, "numpad_memory_clear")                           \
    X(numpad_memory_recall, "numpad_memory_recall")                         \
    X(numpad_memory_store, "numpad_memory_store")                           \
    X(numpad_memory_subtract, "numpad_memory_subtract")                     \
    X(numpad_multiply, "numpad_multiply")                                   \
    X(numpad_paren_left, "numpad_paren_left")                               \
    X(numpad_paren_right, "numpad_paren_right")                             \
    X(numpad_subtract, "numpad_subtract")                                   \
    X(numpad_separator, "numpad_separator")                                 \
    X(numpad_up, "numpad_up")                                               \
    X(numpad_down, "numpad_down")                                           \
    X(numpad_right, "numpad_right")                                         \
    X(numpad_left, "numpad_left")                                           \
    X(numpad_begin, "numpad_begin")                                         \
    X(numpad_home, "numpad_home")                                           \
    X(numpad_end, "numpad_end")                                             \
    X(numpad_insert, "numpad_insert")                                       \
    X(numpad_delete, "numpad_delete")                                       \
    X(numpad_page_up, "numpad_page_up")                                     \
    X(numpad_page_down, "numpad_page_down")                                 \
    /* "Function Section" § 3.5 */                                          \
    X(escape, "escape")                                                     \
    X(f1, "f1")                                                             \
    X(f2, "f2")                                                             \
    X(f3, "f3")                                                             \
    X(f4, "f4")                                                             \
    X(f5, "f5")                                                             \
    X(f6, "f6")                                                             \
    X(f7, "f7")                                                             \
    X(f8, "f8")                                                             \
    X(f9, "f9")                                                             \
    X(f10, "f10")                                                           \
    X(f11, "f11")                                                           \
    X(f12, "f12")                                                           \
    X(f13, "f13")                                                           \
    X(f14, "f14")                                                           \
    X(f15, "f15")                                                           \
    X(f16, "f16")                                                           \
    X(f17, "f17")                                                           \
    X(f18, "f18")                                                           \
    X(f19, "f19")                                                           \
    X(f20, "f20")                                                           \
    X(f21, "f21")                                                           \
    X(f22, "f22")                                                           \
    X(f23, "f23")                                                           \
    X(f24, "f24")                                                           \
    X(f25, "f25")                                                           \
    X(fn, "fn")                                                             \
    X(fn_lock, "fn_lock")                                                   \
    X(print_screen, "print_screen")                                         \
    X(scroll_lock, "scroll_lock")                                           \
    X(pause, "pause")                                                       \
    /* "Media Keys" § 3.6 */                                                \
    X(browser_back, "browser_back")                                         \
    X(browser_favorites, "browser_favorites")                               \
    X(browser_forward, "browser_forward")                                   \
    X(browser_home, "browser_home")                                         \
    X(browser_refresh, "browser_refresh")                                   \
    X(browser_search, "browser_search")                                     \
    X(browser_stop, "browser_stop")                                         \
    X(eject, "eject")                                                       \
    X(launch_app_1, "launch_app_1")                                         \
    X(launch_app_2, "launch_app_2")                                         \
    X(launch_mail, "launch_mail")                                           \
    X(media_play_pause, "media_play_pause")                                 \
    X(media_select, "media_select")                                         \
    X(media_stop, "media_stop")                                             \
    X(media_track_next, "media_track_next")                                 \
    X(media_track_previous, "media_track_previous")                         \
    X(power, "power")                                                       \
    X(sleep, "sleep")                                                       \
    X(audio_volume_down, "audio_volume_down")                               \
    X(audio_volume_mute, "audio_volume_mute")                               \
    X(audio_volume_up, "audio_volume_up")                                   \
    X(wake_up, "wake_up")                                                   \
    /* "Legacy, Non-standard, and Special Keys" § 3.7 */                    \
    X(copy, "copy")                                                         \
    X(cut, "cut")                                                           \
    X(paste, "paste")

/* The set of key codes that Ghostty is aware of. These represent
 * physical keys on the keyboard. The logical key (or key string)
 * is the string that is generated by the key event and that is up
 * to the apprt to provide.
 *
 * Note that these are layout-independent. For example, the "a"
 * key on a US keyboard is the same as the "ф" key on a Russian
 * keyboard, but both will report the "a" enum value in the key
 * event. These values are based on the W3C standard. See:
 * https://www.w3.org/TR/uievents-code
 *
 * Layout-dependent strings are provided in the KeyEvent struct as
 * UTF-8 and are produced by the associated apprt. Ghostty core has
 * no mechanism to map input events to strings without the apprt.
 *
 * IMPORTANT: Any changes here update include/ghostty.h ghostty_input_key_e */
enum class Key : int {
#define WISP_INPUT_KEY_ENUM(cpp, name) cpp,
    WISP_INPUT_KEYS(WISP_INPUT_KEY_ENUM)
#undef WISP_INPUT_KEY_ENUM
};

/* Wisp: @typeInfo(Key).@"enum".fields.len */
static const size_t key_count = 0
#define WISP_INPUT_KEY_COUNT(cpp, name) +1
    WISP_INPUT_KEYS(WISP_INPUT_KEY_COUNT)
#undef WISP_INPUT_KEY_COUNT
    ;

/* Wisp: @tagName(key) */
inline const char *keyName(Key key) {
    static const char *const names[] = {
#define WISP_INPUT_KEY_NAME(cpp, name) name,
        WISP_INPUT_KEYS(WISP_INPUT_KEY_NAME)
#undef WISP_INPUT_KEY_NAME
    };
    const size_t i = (size_t)key;
    return i < key_count ? names[i] : nullptr;
}

/* Wisp: std.meta.stringToEnum(Key, s) */
inline bool keyFromName(const std::string &s, Key *out) {
    for (size_t i = 0; i < key_count; i++) {
        if (s == keyName((Key)i)) {
            *out = (Key)i;
            return true;
        }
    }
    return false;
}

namespace key_detail {

struct CodepointEntry {
    uint32_t cp;
    Key key;
};

inline const CodepointEntry *codepointMap(size_t *len) {
    static const CodepointEntry map[] = {
        {'a', Key::key_a},
        {'b', Key::key_b},
        {'c', Key::key_c},
        {'d', Key::key_d},
        {'e', Key::key_e},
        {'f', Key::key_f},
        {'g', Key::key_g},
        {'h', Key::key_h},
        {'i', Key::key_i},
        {'j', Key::key_j},
        {'k', Key::key_k},
        {'l', Key::key_l},
        {'m', Key::key_m},
        {'n', Key::key_n},
        {'o', Key::key_o},
        {'p', Key::key_p},
        {'q', Key::key_q},
        {'r', Key::key_r},
        {'s', Key::key_s},
        {'t', Key::key_t},
        {'u', Key::key_u},
        {'v', Key::key_v},
        {'w', Key::key_w},
        {'x', Key::key_x},
        {'y', Key::key_y},
        {'z', Key::key_z},
        {'0', Key::digit_0},
        {'1', Key::digit_1},
        {'2', Key::digit_2},
        {'3', Key::digit_3},
        {'4', Key::digit_4},
        {'5', Key::digit_5},
        {'6', Key::digit_6},
        {'7', Key::digit_7},
        {'8', Key::digit_8},
        {'9', Key::digit_9},
        {';', Key::semicolon},
        {' ', Key::space},
        {'\'', Key::quote},
        {',', Key::comma},
        {'`', Key::backquote},
        {'.', Key::period},
        {'/', Key::slash},
        {'-', Key::minus},
        {'=', Key::equal},
        {'[', Key::bracket_left},
        {']', Key::bracket_right},
        {'\\', Key::backslash},

        /* Control characters */
        {'\t', Key::tab},

        /* Keypad entries. We just assume keypad with the numpad_ prefix
         * so that has some special meaning. These must also always be last,
         * so that our `fromASCII` function doesn't accidentally map them
         * over normal numerics and other keys. */
        {'0', Key::numpad_0},
        {'1', Key::numpad_1},
        {'2', Key::numpad_2},
        {'3', Key::numpad_3},
        {'4', Key::numpad_4},
        {'5', Key::numpad_5},
        {'6', Key::numpad_6},
        {'7', Key::numpad_7},
        {'8', Key::numpad_8},
        {'9', Key::numpad_9},
        {'.', Key::numpad_decimal},
        {'/', Key::numpad_divide},
        {'*', Key::numpad_multiply},
        {'-', Key::numpad_subtract},
        {'+', Key::numpad_add},
        {'=', Key::numpad_equal},
    };
    *len = sizeof(map) / sizeof(map[0]);
    return map;
}

} /* namespace key_detail */

/* Returns true if this is a keypad key. */
inline bool keypad(Key self) {
    const char *name = keyName(self);
    return name != nullptr && strncmp(name, "numpad_", 7) == 0;
}

/* Converts an ASCII character to a key, if possible. This returns
 * null if the character is unknown.
 *
 * Note that this can't distinguish between physical keys, i.e. '0'
 * may be from the number row or the keypad, but it always maps
 * to '.zero'.
 *
 * This is what we want, we want people to create keybindings that
 * are independent of the physical key.
 *
 * Wisp: `?Key` is the bool return plus `*out`. */
inline bool fromASCII(uint8_t ch, Key *out) {
    size_t len = 0;
    const key_detail::CodepointEntry *map = key_detail::codepointMap(&len);
    for (size_t i = 0; i < len; i++) {
        /* No ASCII characters should ever map to a keypad key */
        if (keypad(map[i].key)) continue;

        if (map[i].cp == (uint32_t)ch) {
            *out = map[i].key;
            return true;
        }
    }

    return false;
}

/* Converts a W3C key code to a Ghostty key enum value.
 *
 * All required W3C key codes are supported, but there are a number of
 * non-standard key codes that are not supported. In the case the value is
 * invalid or unsupported, this function will return null. */
inline bool fromW3C(const std::string &code, Key *out) {
    const size_t result_len = 128;

    /* If the code is bigger than our buffer it can't possibly match. */
    if (code.size() > result_len) return false;

    /* First just check the whole thing lowercased, this is the simple case */
    std::string lower = code;
    for (char &c : lower) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    if (keyFromName(lower, out)) return true;

    /* We need to convert FooBar to foo_bar */
    std::string w;
    for (size_t i = 0; i < code.size(); i++) {
        const char ch = code[i];
        if (ch >= 'a' && ch <= 'z') {
            if (w.size() >= result_len) return false;
            w.push_back(ch);
        } else if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            /* Caps and numbers trigger underscores */
            if (i > 0) {
                if (w.size() >= result_len) return false;
                w.push_back('_');
            }
            if (w.size() >= result_len) return false;
            w.push_back((ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch);
        } else {
            /* We don't know of any key codes that aren't alphanumeric. */
            return false;
        }
    }

    return keyFromName(w, out);
}

/* Converts a Ghostty key enum value to a W3C key code. */
inline std::string w3c(Key self) {
    const char *name = keyName(self);
    const size_t n = strlen(name);
    std::string w;
    size_t i = 0;
    while (i < n) {
        if (i == 0) {
            w.push_back((char)toupper((unsigned char)name[i]));
        } else if (name[i] == '_') {
            i += 1;
            w.push_back((char)toupper((unsigned char)name[i]));
        } else {
            w.push_back(name[i]);
        }

        i += 1;
    }
    return w;
}

/* True if this key represents a printable character. */
inline bool printable(Key self) {
    size_t len = 0;
    const key_detail::CodepointEntry *map = key_detail::codepointMap(&len);
    for (size_t i = 0; i < len; i++) {
        if (map[i].key == self) return true;
    }

    return false;
}

/* True if this key is a modifier. */
inline bool modifier(Key self) {
    switch (self) {
        case Key::shift_left:
        case Key::control_left:
        case Key::alt_left:
        case Key::meta_left:
        case Key::shift_right:
        case Key::control_right:
        case Key::alt_right:
        case Key::meta_right: return true;

        default: return false;
    }
}

/* Whether this key should be remappable by the operating system.
 *
 * On certain OSes (namely Linux and the BSDs) certain keys like the
 * functional keys are expected to be remappable by the user, such as
 * in the very common use case of swapping the Caps Lock key with the
 * Escape key with the XKB option `caps:swapescape`.
 *
 * However, the way XKB implements this is by essentially acting as a
 * software key remapper that destroys all information about the original
 * physical key, leading to very annoying bugs like #7309 where the
 * physical key `XKB_KEY_c` gets remapped into `XKB_KEY_Cyrillic_tse`,
 * which causes all of our physical key handling to completely break down.
 * _Very naughty._
 *
 * As a compromise, given that writing system keys (§3.1.1) comprise the
 * majority of keys that "change meaning [...] based on the current locale
 * and keyboard layout", we allow all other keys to be remapped by default
 * since they should be fairly harmless. We might consider making this
 * configurable, but for now this should at least placate most people. */
inline bool shouldBeRemappable(Key self) {
    switch (self) {
        /* "Writing System Keys" § 3.1.1 */
        case Key::backquote:
        case Key::backslash:
        case Key::bracket_left:
        case Key::bracket_right:
        case Key::comma:
        case Key::digit_0:
        case Key::digit_1:
        case Key::digit_2:
        case Key::digit_3:
        case Key::digit_4:
        case Key::digit_5:
        case Key::digit_6:
        case Key::digit_7:
        case Key::digit_8:
        case Key::digit_9:
        case Key::equal:
        case Key::intl_backslash:
        case Key::intl_ro:
        case Key::intl_yen:
        case Key::key_a:
        case Key::key_b:
        case Key::key_c:
        case Key::key_d:
        case Key::key_e:
        case Key::key_f:
        case Key::key_g:
        case Key::key_h:
        case Key::key_i:
        case Key::key_j:
        case Key::key_k:
        case Key::key_l:
        case Key::key_m:
        case Key::key_n:
        case Key::key_o:
        case Key::key_p:
        case Key::key_q:
        case Key::key_r:
        case Key::key_s:
        case Key::key_t:
        case Key::key_u:
        case Key::key_v:
        case Key::key_w:
        case Key::key_x:
        case Key::key_y:
        case Key::key_z:
        case Key::minus:
        case Key::period:
        case Key::quote:
        case Key::semicolon:
        case Key::slash: return false;

        default: return true;
    }
}

/* Returns the codepoint representing this key, or null if the key is not
 * printable
 *
 * Wisp: `?u21` is the bool return plus `*out`. */
inline bool codepoint(Key self, uint32_t *out) {
    size_t len = 0;
    const key_detail::CodepointEntry *map = key_detail::codepointMap(&len);
    for (size_t i = 0; i < len; i++) {
        if (map[i].key == self) {
            *out = map[i].cp;
            return true;
        }
    }

    return false;
}

/* true if this key is one of the left or right versions of super (MacOS)
 * or ctrl.
 *
 * Wisp: the non-Darwin branch; see the header. */
inline bool ctrlOrSuper(Key self) { return self == Key::control_left || self == Key::control_right; }

/* true if this key is either left or right shift. */
inline bool leftOrRightShift(Key self) { return self == Key::shift_left || self == Key::shift_right; }

/* true if this key is either left or right alt. */
inline bool leftOrRightAlt(Key self) { return self == Key::alt_left || self == Key::alt_right; }

/* A generic key input event. This is the information that is necessary
 * regardless of apprt in order to generate the proper terminal
 * control sequences for a given key press.
 *
 * Some apprts may not be able to provide all of this information, such
 * as GLFW. In this case, the apprt should provide as much information
 * as it can and it should be expected that the terminal behavior
 * will not be totally correct. */
struct KeyEvent {
    /* The action: press, release, etc. */
    Action action = Action::press;

    /* The keycode of the physical key that was pressed. This is agnostic
     * to the layout. Layout-dependent matching can only be done via the
     * UTF-8 or unshifted codepoint. */
    Key key = Key::unidentified;

    /* Mods are the modifiers that are pressed. */
    Mods mods;

    /* The mods that were consumed in order to generate the text
     * in utf8. This has the mods set that were consumed, so to
     * get the set of mods that are effective you must negate
     * mods with this.
     *
     * This field is meaningless if utf8 is empty. */
    Mods consumed_mods;

    /* Composing is true when this key event is part of a dead key
     * composition sequence and we're in the middle of it. */
    bool composing = false;

    /* The utf8 sequence that was generated by this key event.
     * This will be an empty string if there is no text generated.
     * If composing is true and this is non-empty, this is preedit
     * text.
     *
     * Wisp: a borrowed slice, pointer plus length. */
    const char *utf8 = "";
    size_t utf8_len = 0;

    /* The codepoint for this key when it is unshifted. For example,
     * shift+a is "A" in UTF-8 but unshifted would provide 'a'. */
    uint32_t unshifted_codepoint = 0;

    /* Returns the effective modifiers for this event. The effective
     * modifiers are the mods that should be considered for keybindings. */
    Mods effectiveMods() const {
        if (utf8_len == 0) return mods;
        return mods.unset(consumed_mods);
    }

    /* Returns a unique hash for this key event to be used for tracking
     * uniquess specifically with bindings. This omits fields that are
     * irrelevant for bindings. */
    uint64_t bindingHash() const {
        zigstd::Wyhash hasher = zigstd::Wyhash::init(0);

        /* These are all the fields that are explicitly part of Trigger. */
        const uint32_t key_int = (uint32_t)(int)key;
        const uint8_t key_bytes[4] = {(uint8_t)key_int, (uint8_t)(key_int >> 8), (uint8_t)(key_int >> 16),
                                      (uint8_t)(key_int >> 24)};
        hasher.update(key_bytes, 4);
        const uint8_t cp_bytes[3] = {(uint8_t)unshifted_codepoint, (uint8_t)(unshifted_codepoint >> 8),
                                     (uint8_t)(unshifted_codepoint >> 16)};
        hasher.update(cp_bytes, 3);
        const Mods::Backing m = mods.binding().int_();
        const uint8_t mods_bytes[2] = {(uint8_t)m, (uint8_t)(m >> 8)};
        hasher.update(mods_bytes, 2);

        /* Notes on unmapped things and why:
         *
         * - action: we don't have action-specific bindings right now
         *   AND we want to know if a key resulted in a binding regardless
         *   of action because a press should also ignore a release and so on.
         *
         * We can add to this if there is other confusion. */

        return hasher.final_();
    }
};

/* This sets either "ctrl" or "super" to true (but not both)
 * on mods depending on if the build target is Mac or not. On
 * Mac, we default to super (i.e. super+c for copy) and on
 * non-Mac we default to ctrl (i.e. ctrl+c for copy).
 *
 * Wisp: the non-Darwin branch; see the header. */
inline Mods ctrlOrSuper(const Mods &mods) {
    Mods copy = mods;
    copy.ctrl = true;
    return copy;
}

} /* namespace input */
} /* namespace wisp */

#endif /* WISP_INPUT_KEY_HPP */
