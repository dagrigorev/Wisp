/* Transliterated from the test blocks in Ghostty
 * src/datastruct/comparison.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Wisp: the anonymous test types become C++ types with the opt-ins described
 * in comparison.hpp: structs provide `fields()`, `union(enum)` types provide
 * `tag` plus `payloads()`, optionals are `Opt<T>`. `@Vector(4, f32)` is
 * std::array<float, 4> and the anonymous tuple is std::tuple.
 */

#include <stdint.h>

#include <array>
#include <string>
#include <tuple>
#include <vector>

#include "test_helpers.h"
#include "../src/datastruct/comparison.hpp"

using namespace wisp::datastruct::comparison;

template <typename T>
struct Opt {
    bool has = false;
    T value = T();
    Opt() {}
    Opt(T v) : has(true), value(v) {}
};

/* Tests based on the `expectEqual` tests in the Zig stdlib */
TEST(comparison, expectApproxEqual_union_enum) {
    struct T {
        enum class Tag { a, b } tag;
        int32_t a = 0;
        float b = 0;
        std::tuple<int32_t, float> payloads() const { return std::make_tuple(a, b); }
    };

    T b10;
    b10.tag = T::Tag::b;
    b10.b = 10.0f;
    T b10plus;
    b10plus.tag = T::Tag::b;
    b10plus.b = 10.000001f;

    ASSERT_TRUE(expectApproxEqual(b10, b10plus));
}

TEST(comparison, expectApproxEqual_nested_array) {
    const std::array<std::array<float, 2>, 2> a = {{{{1.0f, 0.0f}}, {{0.0f, 1.0f}}}};
    const std::array<std::array<float, 2>, 2> b = {{{{1.000001f, 0.0f}}, {{0.0f, 0.999999f}}}};

    ASSERT_TRUE(expectApproxEqual(a, b));
}

TEST(comparison, expectApproxEqual_vector) {
    const std::array<float, 4> a = {{4.0f, 4.0f, 4.0f, 4.0f}};
    const std::array<float, 4> b = {{4.000001f, 4.000001f, 4.000001f, 4.000001f}};

    ASSERT_TRUE(expectApproxEqual(a, b));
}

TEST(comparison, expectApproxEqual_struct) {
    const std::tuple<int, float> a = std::make_tuple(1, 1.0f);
    const std::tuple<int, float> b = std::make_tuple(1, 0.999999f);

    ASSERT_TRUE(expectApproxEqual(a, b));
}

TEST(comparison, deepEqual_void) { ASSERT_TRUE(deepEqual(Void(), Void())); }

TEST(comparison, deepEqual_bool) {
    ASSERT_TRUE(deepEqual(true, true));
    ASSERT_TRUE(deepEqual(false, false));
    ASSERT_FALSE(deepEqual(true, false));
    ASSERT_FALSE(deepEqual(false, true));
}

TEST(comparison, deepEqual_int) {
    ASSERT_TRUE(deepEqual<int32_t>(42, 42));
    ASSERT_TRUE(deepEqual<int32_t>(-100, -100));
    ASSERT_FALSE(deepEqual<int32_t>(42, 43));
    ASSERT_TRUE(deepEqual<uint64_t>(0, 0));
    ASSERT_FALSE(deepEqual<uint64_t>(0, 1));
}

TEST(comparison, deepEqual_float) {
    ASSERT_TRUE(deepEqual<float>(1.0f, 1.0f));
    ASSERT_FALSE(deepEqual<float>(1.0f, 1.1f));
    ASSERT_TRUE(deepEqual<double>(3.14159, 3.14159));
    ASSERT_FALSE(deepEqual<double>(3.14159, 3.14158));
}

TEST(comparison, deepEqual_enum) {
    enum class Color { red, green, blue };
    ASSERT_TRUE(deepEqual(Color::red, Color::red));
    ASSERT_TRUE(deepEqual(Color::blue, Color::blue));
    ASSERT_FALSE(deepEqual(Color::red, Color::green));
    ASSERT_FALSE(deepEqual(Color::green, Color::blue));
}

TEST(comparison, deepEqual_const_u8) {
    ASSERT_TRUE(deepEqual<std::string>("hello", "hello"));
    ASSERT_TRUE(deepEqual<std::string>("", ""));
    ASSERT_FALSE(deepEqual<std::string>("hello", "world"));
    ASSERT_FALSE(deepEqual<std::string>("hello", "hell"));
    ASSERT_FALSE(deepEqual<std::string>("hello", "hello!"));
}

TEST(comparison, deepEqual_0_const_u8) {
    ASSERT_TRUE(deepEqual<std::string>("foo", "foo"));
    ASSERT_FALSE(deepEqual<std::string>("foo", "bar"));
    ASSERT_FALSE(deepEqual<std::string>("foo", "fo"));
}

TEST(comparison, deepEqual_const_0_const_u8) {
    const std::vector<std::string> a = {"one", "two", "three"};
    const std::vector<std::string> b = {"one", "two", "three"};
    const std::vector<std::string> c = {"one", "two"};
    const std::vector<std::string> d = {"one", "two", "four"};
    const std::vector<std::string> e = {};

    ASSERT_TRUE(deepEqual(a, b));
    ASSERT_FALSE(deepEqual(a, c));
    ASSERT_FALSE(deepEqual(a, d));
    ASSERT_TRUE(deepEqual(e, e));
    ASSERT_FALSE(deepEqual(a, e));
}

TEST(comparison, deepEqual_optional) {
    ASSERT_TRUE(deepEqual(Opt<int32_t>(), Opt<int32_t>()));
    ASSERT_TRUE(deepEqual(Opt<int32_t>(42), Opt<int32_t>(42)));
    ASSERT_FALSE(deepEqual(Opt<int32_t>(), Opt<int32_t>(42)));
    ASSERT_FALSE(deepEqual(Opt<int32_t>(42), Opt<int32_t>()));
    ASSERT_FALSE(deepEqual(Opt<int32_t>(42), Opt<int32_t>(43)));
}

struct Nested {
    int32_t x;
    int32_t y;
    std::tuple<const int32_t &, const int32_t &> fields() const { return std::tie(x, y); }
};

TEST(comparison, deepEqual_optional_nested) {
    ASSERT_TRUE(deepEqual(Opt<Nested>(), Opt<Nested>()));
    ASSERT_TRUE(deepEqual(Opt<Nested>(Nested{1, 2}), Opt<Nested>(Nested{1, 2})));
    ASSERT_FALSE(deepEqual(Opt<Nested>(Nested{1, 2}), Opt<Nested>(Nested{1, 3})));
    ASSERT_FALSE(deepEqual(Opt<Nested>(Nested{1, 2}), Opt<Nested>()));
}

TEST(comparison, deepEqual_array) {
    ASSERT_TRUE(deepEqual(std::array<int32_t, 3>{{1, 2, 3}}, std::array<int32_t, 3>{{1, 2, 3}}));
    ASSERT_FALSE(deepEqual(std::array<int32_t, 3>{{1, 2, 3}}, std::array<int32_t, 3>{{1, 2, 4}}));
    ASSERT_FALSE(deepEqual(std::array<int32_t, 3>{{1, 2, 3}}, std::array<int32_t, 3>{{0, 2, 3}}));
    ASSERT_TRUE(deepEqual(std::array<int32_t, 0>{}, std::array<int32_t, 0>{}));
}

TEST(comparison, deepEqual_nested_array) {
    typedef std::array<std::array<int32_t, 2>, 2> A;
    const A a = {{{{1, 2}}, {{3, 4}}}};
    const A b = {{{{1, 2}}, {{3, 4}}}};
    const A c = {{{{1, 2}}, {{3, 5}}}};

    ASSERT_TRUE(deepEqual(a, b));
    ASSERT_FALSE(deepEqual(a, c));
}

struct Point {
    int32_t x;
    int32_t y;
    std::tuple<const int32_t &, const int32_t &> fields() const { return std::tie(x, y); }
};

TEST(comparison, deepEqual_struct) {
    ASSERT_TRUE(deepEqual(Point{10, 20}, Point{10, 20}));
    ASSERT_FALSE(deepEqual(Point{10, 20}, Point{10, 21}));
    ASSERT_FALSE(deepEqual(Point{10, 20}, Point{11, 20}));
}

struct Inner {
    int32_t value;
    std::tuple<const int32_t &> fields() const { return std::tie(value); }
};
struct Outer {
    Inner a;
    Inner b;
    std::tuple<const Inner &, const Inner &> fields() const { return std::tie(a, b); }
};

TEST(comparison, deepEqual_struct_nested) {
    const Outer x = {{1}, {2}};
    const Outer y = {{1}, {2}};
    const Outer z = {{1}, {3}};

    ASSERT_TRUE(deepEqual(x, y));
    ASSERT_FALSE(deepEqual(x, z));
}

/* Wisp: @mod is floored modulo; (v % 10 + 10) % 10 matches it for i32. */
static int32_t zmod10(int32_t v) { return (v % 10 + 10) % 10; }

struct Custom {
    int32_t value;

    bool equal(const Custom &other) const { return zmod10(value) == zmod10(other.value); }
};

TEST(comparison, deepEqual_struct_with_equal_decl) {
    ASSERT_TRUE(deepEqual(Custom{5}, Custom{15}));
    ASSERT_TRUE(deepEqual(Custom{100}, Custom{0}));
    ASSERT_FALSE(deepEqual(Custom{5}, Custom{6}));
}

struct Value {
    enum class Tag { int_, float_, none } tag;
    int32_t int_ = 0;
    float float_ = 0;
    Void none;
    std::tuple<const int32_t &, const float &, const Void &> payloads() const { return std::tie(int_, float_, none); }

    static Value makeInt(int32_t v) {
        Value x;
        x.tag = Tag::int_;
        x.int_ = v;
        return x;
    }
    static Value makeFloat(float v) {
        Value x;
        x.tag = Tag::float_;
        x.float_ = v;
        return x;
    }
    static Value makeNone() {
        Value x;
        x.tag = Tag::none;
        return x;
    }
};

TEST(comparison, deepEqual_union) {
    ASSERT_TRUE(deepEqual(Value::makeInt(42), Value::makeInt(42)));
    ASSERT_FALSE(deepEqual(Value::makeInt(42), Value::makeInt(43)));
    ASSERT_FALSE(deepEqual(Value::makeInt(42), Value::makeFloat(42.0f)));
    ASSERT_TRUE(deepEqual(Value::makeNone(), Value::makeNone()));
    ASSERT_FALSE(deepEqual(Value::makeNone(), Value::makeInt(0)));
}

struct EqValue {
    enum class Tag { num, str } tag;
    int32_t num = 0;
    std::string str;
    std::tuple<const int32_t &, const std::string &> payloads() const { return std::tie(num, str); }

    bool equal(const EqValue &other) const {
        switch (tag) {
            case Tag::num: return other.tag == Tag::num && zmod10(num) == zmod10(other.num);
            case Tag::str: return other.tag == Tag::str && str.size() == other.str.size();
        }
        return false;
    }

    static EqValue makeNum(int32_t v) {
        EqValue x;
        x.tag = Tag::num;
        x.num = v;
        return x;
    }
    static EqValue makeStr(const char *s) {
        EqValue x;
        x.tag = Tag::str;
        x.str = s;
        return x;
    }
};

TEST(comparison, deepEqual_union_with_equal_decl) {
    ASSERT_TRUE(deepEqual(EqValue::makeNum(5), EqValue::makeNum(25)));
    ASSERT_FALSE(deepEqual(EqValue::makeNum(5), EqValue::makeNum(6)));
    ASSERT_TRUE(deepEqual(EqValue::makeStr("abc"), EqValue::makeStr("xyz")));
    ASSERT_FALSE(deepEqual(EqValue::makeStr("abc"), EqValue::makeStr("ab")));
}

struct Item {
    int32_t id;
    std::string name;
    std::tuple<const int32_t &, const std::string &> fields() const { return std::tie(id, name); }
};

TEST(comparison, deepEqual_array_of_structs) {
    const std::array<Item, 2> a = {{{1, "one"}, {2, "two"}}};
    const std::array<Item, 2> b = {{{1, "one"}, {2, "two"}}};
    const std::array<Item, 2> c = {{{1, "one"}, {2, "TWO"}}};

    ASSERT_TRUE(deepEqual(a, b));
    ASSERT_FALSE(deepEqual(a, c));
}

struct Config {
    std::string name;
    Opt<uint16_t> port;
    std::tuple<const std::string &, const Opt<uint16_t> &> fields() const { return std::tie(name, port); }
};

TEST(comparison, deepEqual_struct_with_optional_field) {
    ASSERT_TRUE(deepEqual(Config{"app", Opt<uint16_t>(8080)}, Config{"app", Opt<uint16_t>(8080)}));
    ASSERT_TRUE(deepEqual(Config{"app", Opt<uint16_t>()}, Config{"app", Opt<uint16_t>()}));
    ASSERT_FALSE(deepEqual(Config{"app", Opt<uint16_t>(8080)}, Config{"app", Opt<uint16_t>()}));
    ASSERT_FALSE(deepEqual(Config{"app", Opt<uint16_t>(8080)}, Config{"app", Opt<uint16_t>(8081)}));
}

struct Data {
    std::array<int32_t, 3> values;
    std::tuple<const std::array<int32_t, 3> &> fields() const { return std::tie(values); }
};

TEST(comparison, deepEqual_struct_with_array_field) {
    ASSERT_TRUE(deepEqual(Data{{{1, 2, 3}}}, Data{{{1, 2, 3}}}));
    ASSERT_FALSE(deepEqual(Data{{{1, 2, 3}}}, Data{{{1, 2, 4}}}));
}
