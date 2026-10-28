/* Transliterated from the test blocks in Ghostty src/config/formatter.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Wisp: the test enum provides `tagName` and the packed struct provides
 * `packedFields()`, the opt-ins formatter.hpp describes.
 */

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

#include "test_helpers.h"
#include "../src/config/formatter.hpp"

using namespace wisp::config::formatter;

template <typename T>
struct Opt {
    bool has = false;
    T value = T();
    Opt() {}
    Opt(T v) : has(true), value(v) {}
};

enum class TestEnum { one, two, three };
static const char *tagName(TestEnum e) {
    switch (e) {
        case TestEnum::one: return "one";
        case TestEnum::two: return "two";
        case TestEnum::three: return "three";
    }
    return "";
}

TEST(config_formatter, formatEntry_bool) {
    {
        std::string buf;
        formatEntry<bool>("a", true, &buf);
        ASSERT_TRUE(buf == "a = true\n");
    }

    {
        std::string buf;
        formatEntry<bool>("a", false, &buf);
        ASSERT_TRUE(buf == "a = false\n");
    }
}

TEST(config_formatter, formatEntry_int) {
    {
        std::string buf;
        formatEntry<uint8_t>("a", 123, &buf);
        ASSERT_TRUE(buf == "a = 123\n");
    }
}

TEST(config_formatter, formatEntry_float) {
    {
        std::string buf;
        formatEntry<double>("a", 0.7, &buf);
        ASSERT_TRUE(buf == "a = 0.7\n");
    }
}

TEST(config_formatter, formatEntry_enum) {
    {
        std::string buf;
        formatEntry<TestEnum>("a", TestEnum::two, &buf);
        ASSERT_TRUE(buf == "a = two\n");
    }
}

TEST(config_formatter, formatEntry_void) {
    {
        std::string buf;
        formatEntry<Void>("a", Void(), &buf);
        ASSERT_TRUE(buf == "a = \n");
    }
}

TEST(config_formatter, formatEntry_optional) {
    {
        std::string buf;
        formatEntry<Opt<bool>>("a", Opt<bool>(), &buf);
        ASSERT_TRUE(buf == "a = \n");
    }

    {
        std::string buf;
        formatEntry<Opt<bool>>("a", Opt<bool>(false), &buf);
        ASSERT_TRUE(buf == "a = false\n");
    }
}

TEST(config_formatter, formatEntry_string) {
    {
        std::string buf;
        formatEntry<std::string>("a", "hello", &buf);
        ASSERT_TRUE(buf == "a = hello\n");
    }
}

TEST(config_formatter, formatEntry_packed_struct) {
    struct Value {
        bool one = true;
        bool two = false;
        std::vector<std::pair<const char *, bool>> packedFields() const { return {{"one", one}, {"two", two}}; }
    };

    {
        std::string buf;
        formatEntry<Value>("a", Value(), &buf);
        ASSERT_TRUE(buf == "a = one,no-two\n");
    }
}
