/* Transliterated from Ghostty src/datastruct/comparison.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * The contents of this file is largely based on testing.zig from the Zig 0.15.1
 * stdlib, distributed under the MIT license, copyright (c) Zig contributors
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: upstream switches on @typeInfo(T). C++14 has no reflection, so each
 * @typeInfo arm is an overload, and aggregate types opt in the way Zig's
 * `@hasDecl` checks do:
 *   - `.bool/.int/.float/.enum`: arithmetic and enum types, compared with ==.
 *   - `[]const u8`, `[:0]const u8`: std::string.
 *   - `[]const [:0]const u8`: std::vector<std::string>.
 *   - `.optional`: any type with `has` and `value` members (vt::Maybe and
 *     friends).
 *   - `.array`: std::array<T, N> and std::vector<T>.
 *   - `.void`: zigstd-style `Void`.
 *   - `.struct`: a type with `bool equal(const T &) const` uses it (the
 *     `@hasDecl(T, "equal")` arm); otherwise it must provide `fields()`
 *     returning a std::tuple of its fields (std::tie), the field-by-field
 *     fallback.
 *   - `.union(enum)`: a type with `tag` (an enum whose values are payload
 *     indices) and `payloads()` returning a std::tuple of every payload;
 *     only the active one is compared. `equal` still takes precedence.
 * Types matching none of these fail to compile, as upstream's
 * @compileError("unsupported field type") does.
 */
#pragma once
#ifndef WISP_DATASTRUCT_COMPARISON_HPP
#define WISP_DATASTRUCT_COMPARISON_HPP

#include <math.h>
#include <stddef.h>
#include <stdio.h>

#include <array>
#include <cfloat>
#include <limits>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace wisp {
namespace datastruct {
namespace comparison {

/* Wisp: Zig's `void`. */
struct Void {};

namespace detail {

template <typename...>
struct voider {
    typedef void type;
};
template <typename... Ts>
using void_t = typename voider<Ts...>::type;

template <typename T, typename = void>
struct has_equal : std::false_type {};
template <typename T>
struct has_equal<T, void_t<decltype(std::declval<const T &>().equal(std::declval<const T &>()))>> : std::true_type {};

template <typename T, typename = void>
struct has_fields : std::false_type {};
template <typename T>
struct has_fields<T, void_t<decltype(std::declval<const T &>().fields())>> : std::true_type {};

template <typename T, typename = void>
struct has_payloads : std::false_type {};
template <typename T>
struct has_payloads<T, void_t<decltype(std::declval<const T &>().payloads()), decltype(std::declval<const T &>().tag)>>
    : std::true_type {};

template <typename T, typename = void>
struct is_optional : std::false_type {};
template <typename T>
struct is_optional<T, void_t<decltype(std::declval<const T &>().has), decltype(std::declval<const T &>().value)>>
    : std::true_type {};

template <typename T>
using decay = typename std::decay<T>::type;

} /* namespace detail */

/* A deep equality comparison function that works for most types. We
 * add types as necessary. It defers to `equal` decls on types that support
 * decls. */
template <typename T>
bool deepEqual(const T &old, const T &new_);

namespace detail {

/* Wisp: the overload set standing in for upstream's @typeInfo switch. Each
 * function's tag argument selects the arm. */
struct ScalarTag {};
struct OptionalTag {};
struct StructEqualTag {};
struct StructFieldsTag {};
struct UnionTag {};
struct StringTag {};
struct StringListTag {};
struct VoidTag {};
struct StdArrayTag {};

template <typename T>
struct is_std_array_ : std::false_type {};
template <typename T, size_t N>
struct is_std_array_<std::array<T, N>> : std::true_type {};

template <typename T>
struct arm {
    typedef typename std::conditional<
        std::is_same<T, std::string>::value, StringTag,
        typename std::conditional<
        std::is_same<T, std::vector<std::string>>::value, StringListTag,
        typename std::conditional<
        std::is_same<T, Void>::value, VoidTag,
        typename std::conditional<
        is_std_array_<T>::value, StdArrayTag,
        typename std::conditional<
        std::is_arithmetic<T>::value || std::is_enum<T>::value, ScalarTag,
        typename std::conditional<
            has_equal<T>::value, StructEqualTag,
            typename std::conditional<
                has_payloads<T>::value, UnionTag,
                typename std::conditional<is_optional<T>::value, OptionalTag,
                                          typename std::conditional<has_fields<T>::value, StructFieldsTag,
                                                                    void>::type>::type>::type>::type>::type>::type>::type>::type>::type type;
};

/* Do known named types first */
template <typename T>
bool deepEqualArm(const T &old, const T &new_, StringTag) {
    return old == new_;
}

template <typename T>
bool deepEqualArm(const T &old, const T &new_, StringListTag) {
    if (old.size() != new_.size()) return false;
    for (size_t i = 0; i < old.size(); i++) {
        if (old[i] != new_[i]) return false;
    }

    return true;
}

/* Back into types of types */
template <typename T>
bool deepEqualArm(const T &, const T &, VoidTag) {
    return true;
}

template <typename T>
bool deepEqualArm(const T &old, const T &new_, StdArrayTag) {
    typedef typename T::value_type Elem;
    for (size_t i = 0; i < old.size(); i++) {
        if (!deepEqual<Elem>(old[i], new_[i])) return false;
    }
    return true;
}

template <size_t I, typename Tuple>
typename std::enable_if<(I == std::tuple_size<Tuple>::value), bool>::type tupleEqual(const Tuple &, const Tuple &) {
    return true;
}
template <size_t I, typename Tuple>
typename std::enable_if<(I < std::tuple_size<Tuple>::value), bool>::type tupleEqual(const Tuple &a, const Tuple &b) {
    typedef decay<typename std::tuple_element<I, Tuple>::type> Elem;
    if (!deepEqual<Elem>(std::get<I>(a), std::get<I>(b))) return false;
    return tupleEqual<I + 1>(a, b);
}

template <size_t I, typename Tuple>
typename std::enable_if<(I == std::tuple_size<Tuple>::value), bool>::type tupleIndexEqual(const Tuple &, const Tuple &,
                                                                                          size_t) {
    return false; /* unreachable */
}
template <size_t I, typename Tuple>
typename std::enable_if<(I < std::tuple_size<Tuple>::value), bool>::type tupleIndexEqual(const Tuple &a,
                                                                                         const Tuple &b, size_t idx) {
    if (idx == I) {
        typedef decay<typename std::tuple_element<I, Tuple>::type> Elem;
        return deepEqual<Elem>(std::get<I>(a), std::get<I>(b));
    }
    return tupleIndexEqual<I + 1>(a, b, idx);
}

template <typename T>
bool deepEqualArm(const T &old, const T &new_, ScalarTag) {
    return old == new_;
}

template <typename T>
bool deepEqualArm(const T &old, const T &new_, OptionalTag) {
    if (!old.has && !new_.has) return true;
    if (!old.has || !new_.has) return false;
    return deepEqual(old.value, new_.value);
}

template <typename T>
bool deepEqualArm(const T &old, const T &new_, StructEqualTag) {
    return old.equal(new_);
}

template <typename T>
bool deepEqualArm(const T &old, const T &new_, StructFieldsTag) {
    /* If a struct doesn't declare an "equal" function, we fall back
     * to a recursive field-by-field compare. */
    const auto a = old.fields();
    const auto b = new_.fields();
    return tupleEqual<0>(a, b);
}

template <typename T>
bool deepEqualArm(const T &old, const T &new_, UnionTag) {
    if (old.tag != new_.tag) return false;
    const auto a = old.payloads();
    const auto b = new_.payloads();
    return tupleIndexEqual<0>(a, b, (size_t)old.tag);
}

} /* namespace detail */

template <typename T>
bool deepEqual(const T &old, const T &new_) {
    return detail::deepEqualArm(old, new_, typename detail::arm<T>::type());
}

/* ---- expectApproxEqual ---- */

/* Wisp: error.TestExpectedApproxEqual is a false return. */
template <typename T>
bool expectApproxEqual(const T &expected, const T &actual);

namespace detail {

struct FloatTag {};
struct ArrayTag {};
struct OtherTag {};

template <typename T>
struct is_std_array : std::false_type {};
template <typename T, size_t N>
struct is_std_array<std::array<T, N>> : std::true_type {};

template <typename T>
struct is_tuple : std::false_type {};
template <typename... Ts>
struct is_tuple<std::tuple<Ts...>> : std::true_type {};
struct TupleTag {};

template <typename T>
struct approx_arm {
    typedef typename std::conditional<
        std::is_floating_point<T>::value, FloatTag,
        typename std::conditional<
            is_std_array<T>::value, ArrayTag,
            typename std::conditional<
                is_tuple<T>::value, TupleTag,
                typename std::conditional<
                    has_payloads<T>::value, UnionTag,
                    typename std::conditional<is_optional<T>::value, OptionalTag,
                                              typename std::conditional<has_fields<T>::value, StructFieldsTag,
                                                                        OtherTag>::type>::type>::type>::type>::type>::
        type type;
};

/* Copy of testing.print (not public) */
inline void print(const char *msg) { fputs(msg, stderr); }

template <typename T>
bool approxEqRel(T x, T y, T tolerance) {
    /* std.math.approxEqRel */
    if (x == y) return true;
    if (isnan(x) || isnan(y)) return false;
    const T ax = x < 0 ? -x : x;
    const T ay = y < 0 ? -y : y;
    const T diff = x - y < 0 ? y - x : x - y;
    return diff <= (ax > ay ? ax : ay) * tolerance;
}

template <typename T>
bool approxArm(const T &expected, const T &actual, FloatTag) {
    /* check approximate equality for floats */
    const T sqrt_eps = (T)sqrt(std::numeric_limits<T>::epsilon());
    if (!approxEqRel<T>(expected, actual, sqrt_eps)) {
        print("expected approximately a different value\n");
        return false;
    }
    return true;
}

template <typename T>
bool approxArm(const T &expected, const T &actual, ArrayTag) {
    /* recurse into containers */
    for (size_t index = 0; index < expected.size(); index++) {
        if (!expectApproxEqual(actual[index], expected[index])) {
            print("slices not approximately equal\n");
            return false;
        }
    }
    return true;
}

template <size_t I, typename Tuple>
typename std::enable_if<(I == std::tuple_size<Tuple>::value), bool>::type tupleApprox(const Tuple &, const Tuple &) {
    return true;
}
template <size_t I, typename Tuple>
typename std::enable_if<(I < std::tuple_size<Tuple>::value), bool>::type tupleApprox(const Tuple &a, const Tuple &b) {
    typedef decay<typename std::tuple_element<I, Tuple>::type> Elem;
    if (!expectApproxEqual<Elem>(std::get<I>(a), std::get<I>(b))) return false;
    return tupleApprox<I + 1>(a, b);
}

template <size_t I, typename Tuple>
typename std::enable_if<(I == std::tuple_size<Tuple>::value), bool>::type tupleIndexApprox(const Tuple &,
                                                                                           const Tuple &, size_t) {
    return false;
}
template <size_t I, typename Tuple>
typename std::enable_if<(I < std::tuple_size<Tuple>::value), bool>::type tupleIndexApprox(const Tuple &a,
                                                                                          const Tuple &b, size_t idx) {
    if (idx == I) {
        typedef decay<typename std::tuple_element<I, Tuple>::type> Elem;
        return expectApproxEqual<Elem>(std::get<I>(a), std::get<I>(b));
    }
    return tupleIndexApprox<I + 1>(a, b, idx);
}

template <typename T>
bool approxArm(const T &expected, const T &actual, TupleTag) {
    return tupleApprox<0>(expected, actual);
}

template <typename T>
bool approxArm(const T &expected, const T &actual, StructFieldsTag) {
    const auto a = expected.fields();
    const auto b = actual.fields();
    return tupleApprox<0>(a, b);
}

template <typename T>
bool approxArm(const T &expected, const T &actual, UnionTag) {
    /* unwrap unions, optionals, and error unions */
    if (expected.tag != actual.tag) return false;

    /* we only reach this switch if the tags are equal */
    const auto a = expected.payloads();
    const auto b = actual.payloads();
    return tupleIndexApprox<0>(a, b, (size_t)expected.tag);
}

template <typename T>
bool approxArm(const T &expected, const T &actual, OptionalTag) {
    if (expected.has && actual.has) return expectApproxEqual(expected.value, actual.value);
    /* we only reach this point if there's at least one null or error,
     * in which case expectEqual is all we need */
    return expected.has == actual.has;
}

template <typename T>
bool approxArm(const T &expected, const T &actual, OtherTag) {
    /* fall back to expectEqual for everything else */
    return expected == actual;
}

} /* namespace detail */

/* Generic, recursive equality testing utility using approximate comparison for
 * floats and equality for everything else
 *
 * Based on `testing.expectEqual` and `testing.expectEqualSlices`.
 *
 * The relative tolerance is currently hardcoded to `sqrt(eps(float_type))`. */
template <typename T>
bool expectApproxEqual(const T &expected, const T &actual) {
    return detail::approxArm(expected, actual, typename detail::approx_arm<T>::type());
}

} /* namespace comparison */
} /* namespace datastruct */
} /* namespace wisp */

#endif /* WISP_DATASTRUCT_COMPARISON_HPP */
