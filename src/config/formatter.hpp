/* Transliterated from Ghostty src/config/formatter.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: upstream switches on @typeInfo(T). C++14 has no reflection, so each
 * arm is a tag-dispatched overload and types opt in the way the Zig arms
 * look for decls:
 *   - `.bool`, `.int`, `.float`: bool, integral, floating types. Floats
 *     print with `{d}`, the shortest text that reads back as the same value.
 *   - `.enum`: the enum must have an ADL-visible `const char *tagName(E)`
 *     returning upstream's tag name (`{t}`).
 *   - `.void`: comparison::Void.
 *   - `.optional`: any type with `has` and `value`.
 *   - `[]const u8`, `[:0]const u8`: std::string.
 *   - `.struct`/`.union` with a `formatEntry` decl: a
 *     `formatEntry(EntryFormatter)` method, called with an entry formatter.
 *   - `.packed` struct without one: a `packedFields()` method returning a
 *     std::vector of (name, bool) pairs in field order.
 *   Anything else fails to compile, as upstream's "missing case for type".
 *   - The unused `Key` import (config/key.zig) is omitted.
 *   - `*std.Io.Writer` is `std::string *`, so formatting cannot fail.
 */
#pragma once
#ifndef WISP_CONFIG_FORMATTER_HPP
#define WISP_CONFIG_FORMATTER_HPP

#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../datastruct/comparison.hpp"

namespace wisp {
namespace config {
namespace formatter {

typedef ::wisp::datastruct::comparison::Void Void;

template <typename T>
void formatEntry(const char *name, const T &value, std::string *writer);

/* The entry formatter type for a given writer. */
struct EntryFormatter {
    const char *name;
    std::string *writer;

    template <typename T>
    void formatEntry(const T &value) const {
        ::wisp::config::formatter::formatEntry<T>(name, value, writer);
    }
};

/* Returns a single entry formatter for the given field name and writer. */
inline EntryFormatter entryFormatter(const char *name, std::string *writer) {
    EntryFormatter f;
    f.name = name;
    f.writer = writer;
    return f;
}

namespace detail {

template <typename...>
struct voider {
    typedef void type;
};
template <typename... Ts>
using void_t = typename voider<Ts...>::type;

template <typename T, typename = void>
struct has_format_entry : std::false_type {};
template <typename T>
struct has_format_entry<T, void_t<decltype(std::declval<const T &>().formatEntry(std::declval<EntryFormatter>()))>>
    : std::true_type {};

template <typename T, typename = void>
struct has_packed_fields : std::false_type {};
template <typename T>
struct has_packed_fields<T, void_t<decltype(std::declval<const T &>().packedFields())>> : std::true_type {};

template <typename T, typename = void>
struct is_optional : std::false_type {};
template <typename T>
struct is_optional<T, void_t<decltype(std::declval<const T &>().has), decltype(std::declval<const T &>().value)>>
    : std::true_type {};

struct BoolTag {};
struct IntTag {};
struct FloatTag {};
struct EnumTag {};
struct VoidTag {};
struct OptionalTag {};
struct StringTag {};
struct DeclTag {};
struct PackedTag {};

template <typename T>
struct arm {
    typedef typename std::conditional<
        std::is_same<T, bool>::value, BoolTag,
        typename std::conditional<
            std::is_integral<T>::value, IntTag,
            typename std::conditional<
                std::is_floating_point<T>::value, FloatTag,
                typename std::conditional<
                    std::is_enum<T>::value, EnumTag,
                    typename std::conditional<
                        std::is_same<T, Void>::value, VoidTag,
                        typename std::conditional<
                            std::is_same<T, std::string>::value, StringTag,
                            typename std::conditional<
                                has_format_entry<T>::value, DeclTag,
                                typename std::conditional<
                                    is_optional<T>::value, OptionalTag,
                                    typename std::conditional<has_packed_fields<T>::value, PackedTag,
                                                              void>::type>::type>::type>::type>::type>::type>::type>::
            type>::type type;
};

/* Wisp: Zig's `{d}` for floats, the shortest decimal that round-trips. */
inline std::string shortestFloat(double v) {
    char buf[64];
    for (int precision = 1; precision <= 17; precision++) {
        snprintf(buf, sizeof buf, "%.*g", precision, v);
        if (strtod(buf, nullptr) == v) break;
    }
    return buf;
}

template <typename T>
void formatArm(const char *name, const T &value, std::string *writer, BoolTag) {
    writer->append(name);
    writer->append(value ? " = true\n" : " = false\n");
}

template <typename T>
void formatArm(const char *name, const T &value, std::string *writer, IntTag) {
    writer->append(name);
    writer->append(" = ");
    writer->append(std::to_string(value));
    writer->push_back('\n');
}

template <typename T>
void formatArm(const char *name, const T &value, std::string *writer, FloatTag) {
    writer->append(name);
    writer->append(" = ");
    writer->append(shortestFloat((double)value));
    writer->push_back('\n');
}

template <typename T>
void formatArm(const char *name, const T &value, std::string *writer, EnumTag) {
    writer->append(name);
    writer->append(" = ");
    writer->append(tagName(value));
    writer->push_back('\n');
}

template <typename T>
void formatArm(const char *name, const T &, std::string *writer, VoidTag) {
    writer->append(name);
    writer->append(" = \n");
}

template <typename T>
void formatArm(const char *name, const T &value, std::string *writer, OptionalTag) {
    if (value.has) {
        formatEntry(name, value.value, writer);
    } else {
        writer->append(name);
        writer->append(" = \n");
    }
}

template <typename T>
void formatArm(const char *name, const T &value, std::string *writer, StringTag) {
    writer->append(name);
    writer->append(" = ");
    writer->append(value);
    writer->push_back('\n');
}

/* Structs of all types require a "formatEntry" function
 * to be defined which will be called to format the value.
 * This is given the formatter in use so that they can
 * call BACK to our formatEntry to write each primitive
 * value. */
template <typename T>
void formatArm(const char *name, const T &value, std::string *writer, DeclTag) {
    value.formatEntry(entryFormatter(name, writer));
}

/* Packed structs we special case. */
template <typename T>
void formatArm(const char *name, const T &value, std::string *writer, PackedTag) {
    writer->append(name);
    writer->append(" = ");
    const auto fields = value.packedFields();
    for (size_t i = 0; i < fields.size(); i++) {
        if (i > 0) writer->push_back(',');
        if (!fields[i].second) writer->append("no-");
        writer->append(fields[i].first);
    }
    writer->push_back('\n');
}

} /* namespace detail */

/* Format a single type with the given name and value. */
template <typename T>
void formatEntry(const char *name, const T &value, std::string *writer) {
    detail::formatArm(name, value, writer, typename detail::arm<T>::type());
}

} /* namespace formatter */
} /* namespace config */
} /* namespace wisp */

#endif /* WISP_CONFIG_FORMATTER_HPP */
