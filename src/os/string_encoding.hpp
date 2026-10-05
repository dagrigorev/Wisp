/* Transliterated from Ghostty src/os/string_encoding.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: *std.Io.Writer is a std::string appended to; it cannot fail, so the
 * error union (std.Io.Writer.Error || error{DecodeError})!void is a bool,
 * false for error.DecodeError.
 */

#pragma once
#ifndef WISP_OS_STRING_ENCODING_HPP
#define WISP_OS_STRING_ENCODING_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

namespace wisp {
namespace os {
namespace string_encoding {

/* Decode date from the buffer that has been encoded in the same way that
 * `bash`'s `printf %q` encodes a string and write it to the writer. If an
 * error is returned garbage may have been written to the buffer. */
inline bool printfQDecode(std::string *writer, const char *buf, size_t buf_len) {
    const char *data;
    size_t data_len;
    /* Strip off `$''` quoting. */
    if (buf_len >= 2 && buf[0] == '$' && buf[1] == '\'') {
        if (buf_len < 3 || buf[buf_len - 1] != '\'') return false;
        data = buf + 2;
        data_len = buf_len - 3;
    }
    /* Strip off `''` quoting. */
    else if (buf_len >= 1 && buf[0] == '\'') {
        if (buf_len < 2 || buf[buf_len - 1] != '\'') return false;
        data = buf + 1;
        data_len = buf_len - 2;
    } else {
        data = buf;
        data_len = buf_len;
    }

    size_t src = 0;

    while (src < data_len) {
        if (data[src] != '\\') {
            writer->push_back(data[src]);
            src += 1;
            continue;
        }
        if (src + 1 >= data_len) return false;
        switch (data[src + 1]) {
            case ' ':
            case '\\':
            case '"':
            case '\'':
            case '$':
                writer->push_back(data[src + 1]);
                src += 2;
                break;
            case 'e':
                writer->push_back((char)0x1b);
                src += 2;
                break;
            case 'n':
                writer->push_back((char)0x0a);
                src += 2;
                break;
            case 'r':
                writer->push_back((char)0x0d);
                src += 2;
                break;
            case 't':
                writer->push_back((char)0x09);
                src += 2;
                break;
            case 'v':
                writer->push_back((char)0x0b);
                src += 2;
                break;
            default:
                return false;
        }
    }
    return true;
}

inline bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline uint8_t hex(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    /* 'A'...'F'; anything else is unreachable */
    return (uint8_t)(c - 'A' + 10);
}

/* Decode data from the buffer that has been URL percent encoded and write
 * it to the given buffer. If an error is returned the garbage may have been
 * written to the writer. */
inline bool urlPercentDecode(std::string *writer, const char *buf, size_t buf_len) {
    size_t src = 0;
    while (src < buf_len) {
        if (buf[src] != '%') {
            writer->push_back(buf[src]);
            src += 1;
            continue;
        }
        if (src + 2 >= buf_len) return false;
        if (!is_hex_digit(buf[src + 1])) return false;
        if (!is_hex_digit(buf[src + 2])) return false;
        writer->push_back((char)(uint8_t)((hex(buf[src + 1]) << 4) | hex(buf[src + 2])));
        src += 3;
    }
    return true;
}

/* Is the given character valid in URI percent encoding? */
inline bool isValidChar(uint8_t c) {
    switch (c) {
        case ' ': case ';': case '=': return false;
        default: return c >= 0x20 && c <= 0x7e;   /* std.ascii.isPrint */
    }
}

/* Write data to the writer after URI percent encoding.
 * Wisp: std.Uri.Component.percentEncode — valid bytes verbatim, others as
 * '%' and two upper-case hex digits. */
inline void urlPercentEncode(std::string *writer, const char *data, size_t len) {
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        const uint8_t c = (uint8_t)data[i];
        if (isValidChar(c)) {
            writer->push_back((char)c);
        } else {
            writer->push_back('%');
            writer->push_back(digits[c >> 4]);
            writer->push_back(digits[c & 15]);
        }
    }
}

} /* namespace string_encoding */
} /* namespace os */
} /* namespace wisp */

#endif /* WISP_OS_STRING_ENCODING_HPP */
