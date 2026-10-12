// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The literals, before any document stores them: each C++ value a literal accepts, held in its own
// form and visited as exactly one type; objects and arrays nested, empty, and holding their members
// in the order written, a repeated key twice; and what a literal refuses at compile time.

#include <webcpp/document/literal.hpp>

#include <boost/core/lightweight_test.hpp>
#include <boost/decimal/decimal128_t.hpp>
#include <boost/decimal/decimal32_t.hpp>
#include <boost/decimal/decimal64_t.hpp>
#include <boost/decimal/decimal_fast128_t.hpp>
#include <boost/decimal/decimal_fast32_t.hpp>
#include <boost/decimal/decimal_fast64_t.hpp>

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace wd = webcpp::document;

namespace {

using boost::decimal::decimal128_t;
using boost::decimal::decimal32_t;
using boost::decimal::decimal64_t;
using boost::decimal::decimal_fast128_t;
using boost::decimal::decimal_fast32_t;
using boost::decimal::decimal_fast64_t;

// NOLINTNEXTLINE(misc-no-recursion)
std::string describe(const wd::literal& v);

std::string hex(std::uint64_t value, std::size_t digits) {
    constexpr std::string_view alphabet = "0123456789abcdef";
    std::string out(digits, '0');
    for (std::size_t i = digits; i > 0; --i) {
        out[i - 1] = alphabet[value % 16];
        value /= 16;
    }
    return out;
}

// Writes each form with a prefix of its own, so that a value held in the wrong form never reads
// the same: `i:` a signed integer, `u:` an unsigned one, `d:` a decimal's sign, digits and point,
// `b:` a binary64's bits, `s:` UTF-8 text, `w:` UTF-16 units, `t:` a date, `x:` bytes.
struct describer {
    std::string operator()(wd::null_t /*unused*/) const { return "null"; }

    std::string operator()(bool v) const { return v ? "true" : "false"; }

    std::string operator()(std::int64_t v) const { return "i:" + std::to_string(v); }

    std::string operator()(std::uint64_t v) const { return "u:" + std::to_string(v); }

    std::string operator()(const wd::decimal_digits& d) const {
        return "d:" + std::string(d.negative ? "-" : "") + std::string(d.digits.data(), d.count) +
               "e" + std::to_string(d.point);
    }

    std::string operator()(wd::detail::non_finite_decimal /*unused*/) const { return "nonfinite"; }

    std::string operator()(wd::detail::binary64 v) const { return "b:" + hex(v.bits, 16); }

    std::string operator()(std::string_view v) const { return "s:" + std::string(v); }

    std::string operator()(std::u16string_view v) const {
        std::string out = "w:";
        for (const char16_t unit : v) {
            out += hex(unit, 4);
        }
        return out;
    }

    std::string operator()(wd::date_ms v) const {
        return "t:" + std::to_string(v.time_since_epoch().count());
    }

    std::string operator()(std::span<const std::byte> v) const {
        std::string out = "x:";
        for (const std::byte b : v) {
            out += hex(std::to_integer<std::uint8_t>(b), 2);
        }
        return out;
    }

    // Recurses through describe, as deep as the literal is written.
    // NOLINTNEXTLINE(misc-no-recursion)
    std::string operator()(const wd::object& o) const {
        std::string out = "{";
        for (const wd::object_member& m : o.members()) {
            if (out.size() > 1) {
                out += ',';
            }
            out += m.key + ":" + describe(m.value);
        }
        return out + "}";
    }

    // NOLINTNEXTLINE(misc-no-recursion)
    std::string operator()(const wd::array& a) const {
        std::string out = "[";
        for (const wd::literal& e : a.elements()) {
            if (out.size() > 1) {
                out += ',';
            }
            out += describe(e);
        }
        return out + "]";
    }
};

// Describes a literal, the containers through describer, which calls it back for each value.
// NOLINTNEXTLINE(misc-no-recursion)
std::string describe(const wd::literal& v) {
    return v.visit(describer{});
}

// Whether `visit` calls its callable with a `T`, exactly, as the literal's form.
template <class T>
bool visits_as(const wd::literal& v) {
    return v.visit(
        [](const auto& held) { return std::same_as<std::remove_cvref_t<decltype(held)>, T>; });
}

// Character types are text in C++ and never numbers, and `bool` is never an integer.
static_assert(!std::is_convertible_v<char, wd::literal>);
static_assert(!std::is_convertible_v<char8_t, wd::literal>);
static_assert(!std::is_convertible_v<char16_t, wd::literal>);
static_assert(!std::is_convertible_v<char32_t, wd::literal>);
static_assert(!std::is_convertible_v<wchar_t, wd::literal>);
static_assert(!std::is_constructible_v<wd::literal, char>);
static_assert(std::is_convertible_v<signed char, wd::literal>);
static_assert(std::is_convertible_v<unsigned char, wd::literal>);
static_assert(std::is_convertible_v<bool, wd::literal>);

// A pointer that is not text does not become a boolean, as it would in C++.
static_assert(!std::is_convertible_v<int*, wd::literal>);
static_assert(!std::is_convertible_v<const int*, wd::literal>);
static_assert(!std::is_convertible_v<const void*, wd::literal>);
static_assert(!std::is_constructible_v<wd::literal, const void*>);
static_assert(std::is_convertible_v<const char*, wd::literal>);
static_assert(std::is_convertible_v<std::nullptr_t, wd::literal>);

// A binary64 only: a `float` is no literal. JavaScript's `undefined` is no value a literal holds.
static_assert(!std::is_convertible_v<float, wd::literal>);
static_assert(std::is_convertible_v<double, wd::literal>);
static_assert(!std::is_convertible_v<wd::undefined_t, wd::literal>);

// Every value converts implicitly, as JavaScript's `[1, "x"]` needs nothing written.
static_assert(std::is_convertible_v<std::string, wd::literal>);
static_assert(std::is_convertible_v<std::string_view, wd::literal>);
static_assert(std::is_convertible_v<std::u16string_view, wd::literal>);
static_assert(std::is_convertible_v<std::span<const std::byte>, wd::literal>);
static_assert(std::is_convertible_v<wd::date_ms, wd::literal>);
static_assert(std::is_convertible_v<wd::null_t, wd::literal>);
static_assert(std::is_convertible_v<wd::object, wd::literal>);
static_assert(std::is_convertible_v<wd::array, wd::literal>);
static_assert(std::is_convertible_v<decimal128_t, wd::literal>);

// A member is written `{"key", value}` through its constructor, as a std::pair is.
static_assert(!std::is_aggregate_v<wd::object_member>);
static_assert(std::is_constructible_v<wd::object_member, const char*, int>);

// What converts to a view or a span through a conversion of its own converts too, and text
// written `u"..."` as `"..."` does.
static_assert(std::is_convertible_v<std::u16string, wd::literal>);
static_assert(std::is_convertible_v<std::vector<std::byte>, wd::literal>);
static_assert(std::is_convertible_v<const char16_t*, wd::literal>);

void every_form() {
    BOOST_TEST(visits_as<wd::null_t>(nullptr));
    BOOST_TEST(visits_as<wd::null_t>(wd::null));
    BOOST_TEST(visits_as<bool>(true));
    BOOST_TEST(visits_as<std::int64_t>(-5));
    BOOST_TEST(visits_as<std::uint64_t>(5U));
    BOOST_TEST(visits_as<wd::decimal_digits>(decimal128_t(15, -1)));
    BOOST_TEST(
        visits_as<wd::detail::non_finite_decimal>(std::numeric_limits<decimal64_t>::quiet_NaN()));
    BOOST_TEST(visits_as<wd::detail::binary64>(1.5));
    BOOST_TEST(visits_as<std::string_view>("x"));
    BOOST_TEST(visits_as<std::u16string_view>(std::u16string_view(u"x")));
    BOOST_TEST(visits_as<wd::date_ms>(wd::date_ms(std::chrono::milliseconds(1))));
    BOOST_TEST(visits_as<std::span<const std::byte>>(std::span<const std::byte>()));
    BOOST_TEST(visits_as<wd::object>(wd::object()));
    BOOST_TEST(visits_as<wd::array>(wd::array()));

    BOOST_TEST_EQ(describe(nullptr), "null");
    BOOST_TEST_EQ(describe(wd::null), "null");
    BOOST_TEST_EQ(describe(true), "true");
    BOOST_TEST_EQ(describe(false), "false");
}

void integers_by_signedness() {
    BOOST_TEST_EQ(describe(std::int8_t{-128}), "i:-128");
    BOOST_TEST_EQ(describe(std::int16_t{-32768}), "i:-32768");
    BOOST_TEST_EQ(describe(std::int32_t{-2147483647 - 1}), "i:-2147483648");
    BOOST_TEST_EQ(describe(std::numeric_limits<std::int64_t>::min()), "i:-9223372036854775808");
    BOOST_TEST_EQ(describe(std::numeric_limits<std::int64_t>::max()), "i:9223372036854775807");
    BOOST_TEST_EQ(describe(std::uint8_t{255}), "u:255");
    BOOST_TEST_EQ(describe(std::uint16_t{65535}), "u:65535");
    BOOST_TEST_EQ(describe(std::uint32_t{4294967295U}), "u:4294967295");
    BOOST_TEST_EQ(describe(std::numeric_limits<std::uint64_t>::max()), "u:18446744073709551615");
    BOOST_TEST_EQ(describe(static_cast<signed char>(-1)), "i:-1");
    BOOST_TEST_EQ(describe(static_cast<unsigned char>(1)), "u:1");
    // A signed integer is held as signed whatever its value, an unsigned one as unsigned.
    BOOST_TEST_EQ(describe(std::int64_t{7}), "i:7");
    BOOST_TEST_EQ(describe(std::uint64_t{0}), "u:0");
    // `0` is an int, never a null pointer constant taken as null or as text.
    BOOST_TEST_EQ(describe(0), "i:0");
    BOOST_TEST_EQ(describe(wd::array{0, 0U}), "[i:0,u:0]");
}

void decimals_as_their_digits() {
    // Every member of a cohort holds the same digits: 1.5, 1.50 and 15e-1.
    BOOST_TEST_EQ(describe(decimal32_t(15, -1)), "d:15e1");
    BOOST_TEST_EQ(describe(decimal32_t(150, -2)), "d:15e1");
    BOOST_TEST_EQ(describe(decimal64_t(-15, -1)), "d:-15e1");
    BOOST_TEST_EQ(describe(decimal128_t(1, 2)), "d:1e3");
    BOOST_TEST_EQ(describe(decimal_fast32_t(25, -3)), "d:25e-1");
    BOOST_TEST_EQ(describe(decimal_fast64_t(7)), "d:7e1");
    BOOST_TEST_EQ(describe(decimal_fast128_t(123456789)), "d:123456789e9");
    // Zero has one form, with no digits, the negative zero included.
    BOOST_TEST_EQ(describe(decimal128_t(0)), "d:e0");
    BOOST_TEST_EQ(describe(-decimal64_t(0)), "d:e0");
    // The 34 digits of decimal128's precision, at its largest exponent.
    BOOST_TEST_EQ(describe(std::numeric_limits<decimal128_t>::max()),
                  "d:" + std::string(34, '9') + "e6145");
}

void non_finite_decimals() {
    BOOST_TEST_EQ(describe(std::numeric_limits<decimal32_t>::quiet_NaN()), "nonfinite");
    BOOST_TEST_EQ(describe(std::numeric_limits<decimal64_t>::infinity()), "nonfinite");
    BOOST_TEST_EQ(describe(-std::numeric_limits<decimal128_t>::infinity()), "nonfinite");
    BOOST_TEST_EQ(describe(std::numeric_limits<decimal_fast64_t>::signaling_NaN()), "nonfinite");
}

void binary64_as_its_bits() {
    BOOST_TEST_EQ(describe(1.5), "b:3ff8000000000000");
    BOOST_TEST_EQ(describe(0.1), "b:3fb999999999999a");
    // The sign of zero and the infinities are the bits': the document decides what it stores.
    BOOST_TEST_EQ(describe(-0.0), "b:8000000000000000");
    BOOST_TEST_EQ(describe(std::numeric_limits<double>::infinity()), "b:7ff0000000000000");
}

void text_is_copied() {
    BOOST_TEST_EQ(describe("Recife"), "s:Recife");
    BOOST_TEST_EQ(describe(std::string_view("a\0b", 3)), std::string("s:a\0b", 5));
    BOOST_TEST_EQ(describe(""), "s:");
    // A literal does not check UTF-8: the document does, by its encoding, when it stores it.
    BOOST_TEST_EQ(describe(std::string_view("\xff")), "s:\xff");
    BOOST_TEST_EQ(describe(std::u16string_view(u"\u00e9\U0001F600")), "w:00e9d83dde00");
    // A lone surrogate is held: only UTF-16 traits store one.
    BOOST_TEST_EQ(describe(std::u16string_view(u"\xd800", 1)), "w:d800");
    // The literal owns its text: the source may go before it.
    std::string source = "first";
    const wd::literal held = source;
    source.assign("other");
    BOOST_TEST_EQ(describe(held), "s:first");
    const wd::literal moved = std::string("temporary");
    BOOST_TEST_EQ(describe(moved), "s:temporary");
    const std::u16string owned_units = u"z";
    BOOST_TEST_EQ(describe(owned_units), "w:007a");
    BOOST_TEST_EQ(describe(wd::array{owned_units, u"y"}), "[w:007a,w:0079]");
    std::u16string units = u"ab";
    const wd::literal held_units = std::u16string_view(units);
    units.assign(u"zz");
    BOOST_TEST_EQ(describe(held_units), "w:00610062");
}

void a_null_text_pointer_is_null() {
    const char* none = nullptr;
    BOOST_TEST(visits_as<wd::null_t>(none));
    const char16_t* no_units = nullptr;
    BOOST_TEST(visits_as<wd::null_t>(no_units));
    BOOST_TEST_EQ(describe(wd::array{u"x", no_units}), "[w:0078,null]");
}

void dates_and_bytes() {
    BOOST_TEST_EQ(describe(wd::date_ms(std::chrono::milliseconds(1791774245678))),
                  "t:1791774245678");
    BOOST_TEST_EQ(describe(wd::date_ms(std::chrono::milliseconds(-1))), "t:-1");
    // A date outside JavaScript's range is held: the document refuses it when it stores it.
    BOOST_TEST_EQ(describe(wd::date_ms(std::chrono::milliseconds(8640000000000001))),
                  "t:8640000000000001");
    std::array<std::byte, 3> raw{std::byte{0x00}, std::byte{0xff}, std::byte{0x10}};
    const wd::literal bytes = std::span<const std::byte>(raw);
    raw[0] = std::byte{0x01};
    BOOST_TEST_EQ(describe(bytes), "x:00ff10");
    BOOST_TEST_EQ(describe(std::span<const std::byte>()), "x:");
    // A vector of bytes converts as a span does, a temporary one moved in.
    BOOST_TEST_EQ(describe(std::vector<std::byte>{std::byte{0x2a}}), "x:2a");
    const std::vector<std::byte> byte_vector{std::byte{0x01}, std::byte{0x02}};
    BOOST_TEST_EQ(describe(wd::array{byte_vector}), "[x:0102]");
}

void empty_containers() {
    BOOST_TEST(wd::object().members().empty());
    BOOST_TEST(wd::array().elements().empty());
    BOOST_TEST(wd::object{}.members().empty());
    BOOST_TEST(wd::array{}.elements().empty());
    BOOST_TEST_EQ(describe(wd::object()), "{}");
    BOOST_TEST_EQ(describe(wd::array()), "[]");
}

void nesting() {
    BOOST_TEST_EQ(describe(wd::array{1, wd::array{6}, wd::object{{"a", wd::array{}}}}),
                  "[i:1,[i:6],{a:[]}]");
    // An array whose one element is an array names that element a literal: `wd::array{wd::array{}}`
    // is a copy of the inner array where the compiler applies CWG 1467 to every class (MSVC, Clang
    // before 20), and `[[]]` where it applies it to aggregates only, as CWG 2137 says.
    BOOST_TEST_EQ(describe(wd::array{wd::literal(wd::array{})}), "[[]]");
    BOOST_TEST_EQ(describe(wd::array{wd::literal(wd::array{wd::literal(wd::array{})})}), "[[[]]]");
    BOOST_TEST_EQ(describe(wd::object{{"o", wd::object()}}), "{o:{}}");
    // Five levels: an object in an array in an array in an object in an object.
    const wd::array tags{"a", wd::array{true, nullptr, wd::object{{"k", 2.0}}}};
    const wd::object address{{"city", std::string("Recife")}, {"zip", 50000U}};
    const wd::object user{{"name", "Ana"}, {"tags", tags}, {"address", address}};
    const wd::object deep{{"user", user}, {"when", wd::date_ms(std::chrono::milliseconds(0))}};
    BOOST_TEST_EQ(describe(deep),
                  "{user:{name:s:Ana,tags:[s:a,[true,null,{k:b:4000000000000000}]],"
                  "address:{city:s:Recife,zip:u:50000}},when:t:0}");
    // A literal copied into another holds the same tree.
    const wd::array copies{deep, deep};
    BOOST_TEST_EQ(describe(copies), "[" + describe(deep) + "," + describe(deep) + "]");
}

void containers_inspected() {
    const wd::array a{1, "x", nullptr};
    BOOST_TEST_EQ(a.elements().size(), 3U);
    BOOST_TEST_EQ(describe(a.elements()[1]), "s:x");
    const wd::object o{{"city", "Recife"}, {"zip", "50000"}};
    const std::span<const wd::object_member> members = o.members();
    if (!BOOST_TEST_EQ(members.size(), 2U)) {
        return;
    }
    BOOST_TEST_EQ(members[0].key, "city");
    BOOST_TEST_EQ(describe(members[0].value), "s:Recife");
    BOOST_TEST_EQ(members[1].key, "zip");
    BOOST_TEST_EQ(describe(members[1].value), "s:50000");
    // visit gives the callable's result, of whatever type it returns.
    const wd::literal held = o;
    const std::size_t count = held.visit([]<class T>([[maybe_unused]] const T& v) -> std::size_t {
        if constexpr (std::same_as<T, wd::object>) {
            return v.members().size();
        } else {
            return 0;
        }
    });
    BOOST_TEST_EQ(count, 2U);
}

void object_members_in_order() {
    // Held as written: the document applies its traits' key order, and a repeated key's rule,
    // when it stores the object.
    const wd::object o{{"b", 1}, {"2", 2}, {"a", 3}, {"1", 4}, {"b", 5}};
    const wd::object_member member("k", wd::array{});
    BOOST_TEST_EQ(member.key, "k");
    BOOST_TEST_EQ(describe(member.value), "[]");
    std::vector<std::string> keys;
    for (const wd::object_member& m : o.members()) {
        keys.push_back(m.key);
    }
    BOOST_TEST((keys == std::vector<std::string>{"b", "2", "a", "1", "b"}));
    BOOST_TEST_EQ(describe(o), "{b:i:1,2:i:2,a:i:3,1:i:4,b:i:5}");
    // A key is any text, the empty one and one holding a zero included.
    BOOST_TEST_EQ(describe(wd::object{{"", 1}, {std::string("k\0", 2), 2}}),
                  std::string("{:i:1,k\0:i:2}", 13));
}

}  // namespace

int main() {
    every_form();
    integers_by_signedness();
    decimals_as_their_digits();
    non_finite_decimals();
    binary64_as_its_bits();
    text_is_copied();
    a_null_text_pointer_is_null();
    dates_and_bytes();
    empty_containers();
    nesting();
    containers_inspected();
    object_members_in_order();
    return boost::report_errors();
}
