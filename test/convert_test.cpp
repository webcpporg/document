// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Conversions between C++ values and the tree, under the default traits and JavaScript's: every
// settable type made an item and stored by value, numbers first made what the traits' number makes
// of them; text validated and transcoded; dates in and out of JavaScript's range; bytes and their
// text forms; literals nested with a repeated key; nodes built in the tree read back as every
// readable type, with their type and range failures; and structural equality.

#include <webcpp/document/detail/convert.hpp>

#include <boost/core/lightweight_test.hpp>
#include <boost/decimal/decimal128_t.hpp>
#include <boost/decimal/decimal32_t.hpp>
#include <boost/decimal/decimal64_t.hpp>
#include <boost/decimal/decimal_fast128_t.hpp>
#include <boost/decimal/decimal_fast32_t.hpp>
#include <boost/decimal/decimal_fast64_t.hpp>

#include "require.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wd = webcpp::document;
namespace detail = webcpp::document::detail;

namespace {

using boost::decimal::decimal128_t;
using boost::decimal::decimal32_t;
using boost::decimal::decimal64_t;
using boost::decimal::decimal_fast128_t;
using boost::decimal::decimal_fast32_t;
using boost::decimal::decimal_fast64_t;

using defaults = wd::default_traits;
using js = wd::js_traits;

/** Default traits that write bytes as base64url. */
struct url_traits : wd::default_traits {
    using bytes = wd::base64url;
};

constexpr std::uint64_t mebibyte = std::uint64_t{1} << 20U;

// R12 and R13: characters are no numbers, and a floating-point value converts only into traits
// whose number is binary.
static_assert(!detail::preparable<defaults, char>);
static_assert(!detail::readable<defaults, char>);
static_assert(!detail::preparable<defaults, double>);
static_assert(!detail::readable<defaults, double>);
static_assert(detail::preparable<js, double>);
static_assert(detail::readable<js, float>);
static_assert(detail::preparable<defaults, std::u16string>);
static_assert(detail::readable<defaults, std::u16string_view>);

// A tree in an owned region, its root assigned and read through the conversions.
template <class Traits>
struct fixture {
    detail::region r = *detail::region::owned(4096, 16 * mebibyte);
    detail::tree<Traits> t = *detail::tree<Traits>::format(r);

    [[nodiscard]] const detail::node<Traits>* root() const { return t.find(t.root()); }

    template <class T>
    wd::result<void> put(const T& v) {
        detail::prepared<Traits> p;
        if (auto made = detail::prepare(v, p); !made.has_value()) {
            return made;
        }
        return t.assign(t.root(), p.value());
    }

    wd::result<void> put_number(std::string_view text) {
        detail::prepared<Traits> p;
        if (auto made = detail::prepare_number(text, p); !made.has_value()) {
            return made;
        }
        return t.assign(t.root(), p.value());
    }

    wd::result<void> put_item(const detail::item<Traits>& i) { return t.assign(t.root(), i); }

    // Assigns an item built for the tree directly to the root, which must take it.
    void place(const detail::item<Traits>& i) {
        webcpp::test::require(BOOST_TEST(t.assign(t.root(), i).has_value()));
    }

    // Sets the member `key` of the root, an object, to an item.
    void member(std::string_view key, const detail::item<Traits>& i) {
        webcpp::test::require(BOOST_TEST(t.set_member(t.root(), key, i).has_value()));
    }

    template <class T>
    [[nodiscard]] wd::result<T> get() const {
        return detail::read<T>(t, root());
    }

    [[nodiscard]] wd::kind kind() const { return root()->kind; }

    [[nodiscard]] detail::repr repr() const { return root()->number; }

    [[nodiscard]] std::int64_t integer() const { return root()->template load<std::int64_t>(); }

    [[nodiscard]] Traits::number number() const {
        return root()->template load<typename Traits::number>();
    }
};

// No node: what a handle that names nothing reads.
template <class Traits>
constexpr const detail::node<Traits>* nothing = nullptr;

// Items built for the tree directly, without the conversions under test.

template <class Traits>
detail::item<Traits> integer_item(std::int64_t v) {
    detail::item<Traits> i;
    i.kind = wd::kind::number;
    i.number = detail::repr::integer;
    i.store(v);
    return i;
}

template <class Traits>
detail::item<Traits> number_item(const typename Traits::number& v) {
    detail::item<Traits> i;
    i.kind = wd::kind::number;
    i.number = detail::repr::number;
    i.store(v);
    return i;
}

template <class Traits>
detail::item<Traits> boolean_item(bool v) {
    detail::item<Traits> i;
    i.kind = wd::kind::boolean;
    i.store(v);
    return i;
}

template <class Traits>
detail::item<Traits> date_item(std::int64_t ms) {
    detail::item<Traits> i;
    i.kind = wd::kind::date;
    i.store(wd::date_ms(std::chrono::milliseconds(ms)));
    return i;
}

// A string in the traits' encoding, which must outlive the item.
template <class Traits>
detail::item<Traits> string_item(std::string_view utf8) {
    static_assert(std::same_as<typename Traits::string, wd::utf8>);
    detail::item<Traits> i;
    i.kind = wd::kind::string;
    i.text = std::as_bytes(std::span(utf8.data(), utf8.size()));
    return i;
}

template <class Traits>
detail::item<Traits> string_item(std::u16string_view utf16) {
    static_assert(std::same_as<typename Traits::string, wd::utf16>);
    detail::item<Traits> i;
    i.kind = wd::kind::string;
    i.text = std::as_bytes(std::span(utf16.data(), utf16.size()));
    return i;
}

template <class Traits>
detail::item<Traits> bytes_item(std::span<const std::byte> bytes) {
    detail::item<Traits> i;
    i.kind = wd::kind::bytes;
    i.text = bytes;
    return i;
}

template <class Traits>
detail::item<Traits> container_item(wd::kind k) {
    detail::item<Traits> i;
    i.kind = k;
    return i;
}

std::vector<std::byte> bytes_of(std::string_view text) {
    const auto view = std::as_bytes(std::span(text.data(), text.size()));
    return {view.begin(), view.end()};
}

decimal128_t decimal(std::string_view text) {
    return *wd::number_codec<decimal128_t>::parse(text);
}

// A key or a string as UTF-8, whatever the traits' encoding.
template <class Traits>
std::string text_of(const detail::byte_string& s) {
    if constexpr (std::same_as<typename Traits::string, wd::utf16>) {
        const std::u16string_view units = s.units();
        std::string out(static_cast<std::size_t>(detail::utf8_size(units)), '\0');
        detail::to_utf8(units, out.data());
        return out;
    } else {
        return std::string(s.chars());
    }
}

// A node and its subtree in a compact JSON-like text, children in the policy's order; the
// recursion is as deep as the small trees these cases build.
template <class Traits>
// NOLINTNEXTLINE(misc-no-recursion)
void write_node(const detail::tree<Traits>& t, const detail::node<Traits>& n, std::string& out) {
    using number = Traits::number;
    switch (n.kind) {
        case wd::kind::undefined: out += "undefined"; return;
        case wd::kind::null: out += "null"; return;
        case wd::kind::boolean: out += n.template load<bool>() ? "true" : "false"; return;
        case wd::kind::number:
            out += n.number == detail::repr::integer
                       ? std::to_string(n.template load<std::int64_t>())
                       : std::string(
                             wd::number_codec<number>::write(n.template load<number>()).view());
            return;
        case wd::kind::string: out += '"' + text_of<Traits>(n.text) + '"'; return;
        case wd::kind::date:
            out += "date(" +
                   std::to_string(n.template load<wd::date_ms>().time_since_epoch().count()) + ")";
            return;
        case wd::kind::bytes: out += "bytes(" + std::to_string(n.text.size) + ")"; return;
        case wd::kind::array:
        case wd::kind::object: {
            const bool is_object = n.kind == wd::kind::object;
            out += is_object ? '{' : '[';
            for (const auto* c = t.first_child(n.id); c != nullptr; c = t.next_sibling(*c)) {
                if (c != t.first_child(n.id)) {
                    out += ',';
                }
                if (is_object) {
                    out += '"' + text_of<Traits>(c->key) + "\":";
                }
                write_node(t, *c, out);
            }
            out += is_object ? '}' : ']';
            return;
        }
    }
}

template <class Traits>
std::string dump(const fixture<Traits>& f) {
    std::string out;
    write_node(f.t, *f.root(), out);
    return out;
}

// Stores `v` and checks it is the integer `expected`, read back as its own type.
template <class Traits, class T>
void stores_integer(const T& v, std::int64_t expected) {
    fixture<Traits> f;
    webcpp::test::require(BOOST_TEST(f.put(v).has_value()));
    BOOST_TEST(f.kind() == wd::kind::number);
    BOOST_TEST(f.repr() == detail::repr::integer);
    BOOST_TEST_EQ(f.integer(), expected);
    const wd::result<T> back = f.template get<T>();
    BOOST_TEST(back.has_value() && *back == v);
}

// Every integer width is stored by value, as the traits' integer when it is one, and read back as
// its own type; with a binary number, an integer first rounds as JavaScript rounds it.
template <class Traits>
void integers_of_every_width() {
    stores_integer<Traits>(std::int8_t{-128}, -128);
    stores_integer<Traits>(static_cast<signed char>(-5), -5);
    stores_integer<Traits>(std::int16_t{32767}, 32767);
    stores_integer<Traits>(std::numeric_limits<std::int32_t>::min(), -2147483648LL);
    stores_integer<Traits>(std::numeric_limits<std::int64_t>::min(),
                           std::numeric_limits<std::int64_t>::min());
    stores_integer<Traits>(std::uint8_t{255}, 255);
    stores_integer<Traits>(std::uint16_t{65535}, 65535);
    stores_integer<Traits>(std::numeric_limits<std::uint32_t>::max(), 4294967295LL);
    stores_integer<Traits>(std::uint64_t{0}, 0);
    stores_integer<Traits>(std::int64_t{0}, 0);
    stores_integer<Traits>(-7LL, -7);
    stores_integer<Traits>(9UL, 9);

    fixture<Traits> f;
    BOOST_TEST(f.put(std::numeric_limits<std::int64_t>::max()).has_value());
    if constexpr (std::same_as<Traits, js>) {
        // 2^63 - 1 rounds to 2^63, which is no int64.
        BOOST_TEST(f.repr() == detail::repr::number);
        BOOST_TEST(f.number() == 9223372036854775808.0);
        BOOST_TEST(f.template get<std::int64_t>().error() == wd::errc::range);
        BOOST_TEST(f.template get<std::uint64_t>() == std::uint64_t{9223372036854775808ULL});
    } else {
        BOOST_TEST(f.repr() == detail::repr::integer);
        BOOST_TEST(f.template get<std::int64_t>() == std::numeric_limits<std::int64_t>::max());
    }
}

// R4 by default: 1, 1.0, 1e0 and 1.5e1 are integers, from text or from a decimal.
void numbers_by_value() {
    struct row {
        std::string_view text;
        std::int64_t value;
    };

    for (const row& r : {
             row{.text = "1", .value = 1},
             row{.text = "1.0", .value = 1},
             row{.text = "1e0", .value = 1},
             row{.text = "1.5e1", .value = 15},
             row{.text = "-0", .value = 0},
             row{.text = "10E-1", .value = 1},
         }) {
        fixture<defaults> f;
        BOOST_TEST(f.put_number(r.text).has_value());
        BOOST_TEST(f.repr() == detail::repr::integer);
        BOOST_TEST_EQ(f.integer(), r.value);
    }
    for (const decimal128_t v : {
             decimal128_t{1},
             decimal128_t{10, -1},
             decimal128_t{1, 0},
             decimal128_t{15, 0},
             decimal128_t{100, -2},
         }) {
        fixture<defaults> f;
        BOOST_TEST(f.put(v).has_value());
        BOOST_TEST(f.repr() == detail::repr::integer);
    }
}

// R4 by default: 1.50 is a number, normalised, so that one value stores the same bytes however it
// came.
void numbers_are_normalised() {
    fixture<defaults> text;
    BOOST_TEST(text.put_number("1.50").has_value());
    BOOST_TEST(text.repr() == detail::repr::number);
    BOOST_TEST(text.number() == decimal128_t(15, -1));
    BOOST_TEST(text.get<std::int64_t>().error() == wd::errc::range);
    fixture<defaults> cohort;
    BOOST_TEST(cohort.put(decimal128_t{150, -2}).has_value());
    BOOST_TEST(cohort.root()->scalar == text.root()->scalar);
    fixture<defaults> literal;
    BOOST_TEST(literal.put(wd::literal(decimal64_t{1500, -3})).has_value());
    BOOST_TEST(literal.root()->scalar == text.root()->scalar);
}

// R4 by default: 1e20 and UINT64_MAX are exact numbers, and 123456789012345678 an exact integer.
void large_numbers_stay_exact() {
    fixture<defaults> large;
    BOOST_TEST(large.put_number("1e20").has_value());
    BOOST_TEST(large.repr() == detail::repr::number);
    BOOST_TEST(large.get<std::uint64_t>().error() == wd::errc::range);
    BOOST_TEST(large.get<decimal128_t>() == decimal128_t(1, 20));

    constexpr std::uint64_t most = std::numeric_limits<std::uint64_t>::max();
    fixture<defaults> exact;
    BOOST_TEST(exact.put(most).has_value());
    BOOST_TEST(exact.repr() == detail::repr::number);
    BOOST_TEST(exact.get<std::uint64_t>() == most);
    BOOST_TEST(exact.get<std::int64_t>().error() == wd::errc::range);
    fixture<defaults> exact_text;
    BOOST_TEST(exact_text.put_number("18446744073709551615").has_value());
    BOOST_TEST(exact_text.root()->scalar == exact.root()->scalar);

    // R4's example: exact by default.
    fixture<defaults> wide;
    BOOST_TEST(wide.put_number("123456789012345678").has_value());
    BOOST_TEST(wide.repr() == detail::repr::integer);
    BOOST_TEST_EQ(wide.integer(), 123456789012345678LL);
}

// R4 with js_traits: a number is first a double, so 123456789012345678 is stored as Node writes it,
// 123456789012345680, whichever way it came; 1.50 and 1e20 are numbers.
void numbers_by_value_as_javascript() {
    fixture<js> text;
    BOOST_TEST(text.put_number("123456789012345678").has_value());
    BOOST_TEST(text.repr() == detail::repr::integer);
    BOOST_TEST_EQ(text.integer(), 123456789012345680LL);
    fixture<js> integer;
    BOOST_TEST(integer.put(std::int64_t{123456789012345678}).has_value());
    BOOST_TEST(integer.repr() == detail::repr::integer);
    BOOST_TEST_EQ(integer.integer(), 123456789012345680LL);
    fixture<js> literal;
    BOOST_TEST(literal.put(wd::array{std::uint64_t{123456789012345678}}).has_value());
    BOOST_TEST_EQ(dump(literal), "[123456789012345680]");

    for (const std::string_view one : {"1", "1.0", "1e0", "-0"}) {
        fixture<js> f;
        BOOST_TEST(f.put_number(one).has_value());
        BOOST_TEST(f.repr() == detail::repr::integer);
    }
    fixture<js> fraction;
    BOOST_TEST(fraction.put_number("1.50").has_value());
    BOOST_TEST(fraction.repr() == detail::repr::number);
    BOOST_TEST(fraction.number() == 1.5);
    fixture<js> large;
    BOOST_TEST(large.put_number("1e20").has_value());
    BOOST_TEST(large.repr() == detail::repr::number);
    BOOST_TEST(large.number() == 1e20);
    fixture<js> most;
    BOOST_TEST(most.put(std::numeric_limits<std::uint64_t>::max()).has_value());
    BOOST_TEST(most.repr() == detail::repr::number);
    BOOST_TEST(most.get<std::uint64_t>().error() == wd::errc::range);
    // A decimal becomes the double nearest it.
    fixture<js> tenth;
    BOOST_TEST(tenth.put(decimal128_t{1, -1}).has_value());
    BOOST_TEST(tenth.number() == 0.1);
}

// A number's text is read by the traits' codec: grammar, digits and range as it says.
void numbers_from_text() {
    detail::prepared<defaults> p;
    BOOST_TEST(detail::prepare_number("01", p).error() == wd::errc::syntax);
    BOOST_TEST(detail::prepare_number("1.", p).error() == wd::errc::syntax);
    BOOST_TEST(detail::prepare_number("12345678901234567890123456789012345", p).error() ==
               wd::errc::range);
    BOOST_TEST(detail::prepare_number("1e9999", p).error() == wd::errc::range);
    BOOST_TEST(detail::prepare_number("1e400", p).has_value());
    BOOST_TEST(detail::prepare_number("-9223372036854775808", p).has_value());
    BOOST_TEST(p.value().number == detail::repr::integer);
    BOOST_TEST(detail::prepare_number("-9223372036854775809", p).has_value());
    BOOST_TEST(p.value().number == detail::repr::number);

    detail::prepared<js> q;
    // JSON.parse makes 1e400 Infinity, which js_traits stores.
    BOOST_TEST(detail::prepare_number("1e400", q).has_value());
    BOOST_TEST(q.value().number == detail::repr::number);
    BOOST_TEST(wd::scalar_codec<double>::load(q.value().scalar.data()) ==
               std::numeric_limits<double>::infinity());
    BOOST_TEST(detail::prepare_number("01", q).error() == wd::errc::syntax);
    // JSON.parse rounds 40 digits, which js_traits keeps.
    fixture<js> forty;
    BOOST_TEST(forty.put_number("1234567890123456789012345678901234567890").has_value());
    BOOST_TEST(forty.repr() == detail::repr::number);
    BOOST_TEST(forty.number() == 1.2345678901234568e39);
}

// A NaN or an infinity: a decimal is a range error, a double is stored (R5).
void non_finite_numbers() {
    fixture<defaults> f;
    BOOST_TEST(f.put(std::numeric_limits<decimal128_t>::quiet_NaN()).error() == wd::errc::range);
    BOOST_TEST(f.put(std::numeric_limits<decimal64_t>::infinity()).error() == wd::errc::range);
    BOOST_TEST(f.put(-std::numeric_limits<decimal32_t>::infinity()).error() == wd::errc::range);
    BOOST_TEST(f.put(wd::array{1, std::numeric_limits<decimal_fast64_t>::quiet_NaN()}).error() ==
               wd::errc::range);
    BOOST_TEST(f.kind() == wd::kind::null);

    fixture<js> g;
    BOOST_TEST(g.put(std::numeric_limits<decimal128_t>::quiet_NaN()).error() == wd::errc::range);
    BOOST_TEST(g.put(std::numeric_limits<double>::quiet_NaN()).has_value());
    BOOST_TEST(g.repr() == detail::repr::number);
    BOOST_TEST(std::isnan(*g.get<double>()));
    BOOST_TEST(std::isnan(*g.get<float>()));
    BOOST_TEST(g.get<std::int64_t>().error() == wd::errc::range);
    BOOST_TEST(g.get<decimal64_t>().error() == wd::errc::range);
    BOOST_TEST(g.put(-std::numeric_limits<double>::infinity()).has_value());
    BOOST_TEST(g.get<double>() == -std::numeric_limits<double>::infinity());
    BOOST_TEST(g.get<float>() == -std::numeric_limits<float>::infinity());
    BOOST_TEST(g.put(wd::literal(std::numeric_limits<double>::infinity())).has_value());
    BOOST_TEST(g.number() == std::numeric_limits<double>::infinity());
    // -0 is the integer 0, as JSON.stringify writes it.
    BOOST_TEST(g.put(-0.0).has_value());
    BOOST_TEST(g.repr() == detail::repr::integer);
    BOOST_TEST_EQ(g.integer(), 0);
}

// Doubles go into binary traits only, and come out exactly or not at all.
void binary_numbers() {
    fixture<js> f;
    BOOST_TEST(f.put(1.5).has_value());
    BOOST_TEST(f.repr() == detail::repr::number);
    BOOST_TEST(f.get<double>() == 1.5);
    BOOST_TEST(f.get<float>() == 1.5F);
    BOOST_TEST(f.get<std::int32_t>().error() == wd::errc::range);
    BOOST_TEST(f.get<decimal32_t>() == decimal32_t(15, -1));
    BOOST_TEST(f.put(2.0).has_value());
    BOOST_TEST(f.repr() == detail::repr::integer);
    BOOST_TEST(f.get<double>() == 2.0);
    BOOST_TEST(f.put(0.5F).has_value());
    BOOST_TEST(f.number() == 0.5);
    BOOST_TEST(f.put(wd::literal(0.25)).has_value());
    BOOST_TEST(f.number() == 0.25);
    BOOST_TEST(f.put(0.1).has_value());
    BOOST_TEST(f.get<float>().error() == wd::errc::range);
    BOOST_TEST(f.get<decimal64_t>() == decimal64_t(1, -1));
    BOOST_TEST(f.put(1e300).has_value());
    BOOST_TEST(f.get<float>().error() == wd::errc::range);
    BOOST_TEST(f.get<std::uint64_t>().error() == wd::errc::range);

    fixture<defaults> g;
    BOOST_TEST(g.put(wd::literal(1.5)).error() == wd::errc::type);
    BOOST_TEST(g.put(wd::array{1, wd::object{{"x", 2.0}}}).error() == wd::errc::type);
    BOOST_TEST(g.kind() == wd::kind::null);
}

// The decimal types go in by value and come out exactly or with a range error.
void decimal_types() {
    fixture<defaults> f;
    BOOST_TEST(f.put(decimal32_t{15, -1}).has_value());
    BOOST_TEST(f.number() == decimal128_t(15, -1));
    BOOST_TEST(f.get<decimal32_t>() == decimal32_t(15, -1));
    BOOST_TEST(f.get<decimal_fast32_t>() == decimal_fast32_t(15, -1));
    BOOST_TEST(f.get<decimal64_t>() == decimal64_t(15, -1));
    BOOST_TEST(f.get<decimal_fast64_t>() == decimal_fast64_t(15, -1));
    BOOST_TEST(f.get<decimal_fast128_t>() == decimal_fast128_t(15, -1));
    BOOST_TEST(f.put(decimal_fast64_t{-25, -2}).has_value());
    BOOST_TEST(f.number() == decimal128_t(-25, -2));
    BOOST_TEST(f.put(decimal_fast128_t{7}).has_value());
    BOOST_TEST(f.repr() == detail::repr::integer);
    BOOST_TEST(f.get<decimal32_t>() == decimal32_t(7));

    // 34 digits fit decimal128 only.
    BOOST_TEST(f.put_number("1234567890123456789012345678901.234").has_value());
    BOOST_TEST(f.get<decimal64_t>().error() == wd::errc::range);
    BOOST_TEST(f.get<decimal128_t>() == decimal("1234567890123456789012345678901.234"));
    // An integer of eight digits does not fit decimal32's seven.
    BOOST_TEST(f.put(12345678).has_value());
    BOOST_TEST(f.get<decimal32_t>().error() == wd::errc::range);
    BOOST_TEST(f.get<decimal64_t>() == decimal64_t(12345678));
    // An exponent past decimal32's.
    BOOST_TEST(f.put_number("1e200").has_value());
    BOOST_TEST(f.get<decimal32_t>().error() == wd::errc::range);
}

// Text: UTF-8 validated, transcoded into the traits' encoding, and read back as each text type,
// a view only in its own encoding (R11).
void text_in_utf8() {
    const std::string_view text = "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80";
    const std::u16string_view units = u"aé€\U0001F600";
    fixture<defaults> f;
    BOOST_TEST(f.put(text).has_value());
    BOOST_TEST(f.kind() == wd::kind::string);
    BOOST_TEST(f.root()->text.chars() == text);
    BOOST_TEST(f.get<std::string_view>() == text);
    BOOST_TEST(f.get<std::string>() == std::string(text));
    BOOST_TEST(f.get<std::u16string>() == std::u16string(units));
    BOOST_TEST(f.get<std::u16string>()->size() == 5);
    BOOST_TEST(f.get<std::u16string_view>().error() == wd::errc::type);

    BOOST_TEST(f.put(units).has_value());
    BOOST_TEST(f.root()->text.chars() == text);
    BOOST_TEST(f.put(std::u16string(units)).has_value());
    BOOST_TEST(f.root()->text.chars() == text);
    BOOST_TEST(f.put(std::string("plain")).has_value());
    BOOST_TEST(f.get<std::string_view>() == "plain");
    BOOST_TEST(f.put("chars").has_value());
    BOOST_TEST(f.get<std::string_view>() == "chars");
    BOOST_TEST(f.put("").has_value());
    BOOST_TEST(f.kind() == wd::kind::string);
    BOOST_TEST(f.get<std::string_view>() == "");
}

// Invalid UTF-8, a surrogate written in UTF-8, and a lone surrogate in UTF-16 are refused, and
// change nothing; a null pointer is null.
void invalid_text_is_refused() {
    fixture<defaults> f;
    BOOST_TEST(f.put("kept").has_value());
    BOOST_TEST(f.put(std::string_view("\xff")).error() == wd::errc::type);
    BOOST_TEST(f.put(std::string_view("\xed\xa0\x80")).error() == wd::errc::type);
    BOOST_TEST(f.put(std::string_view("\xc3")).error() == wd::errc::type);
    BOOST_TEST(f.put(std::u16string_view(u"a\xd800")).error() == wd::errc::type);
    BOOST_TEST(f.get<std::string_view>() == "kept");

    // A null pointer is null, as a literal holds it.
    BOOST_TEST(f.put(static_cast<const char*>(nullptr)).has_value());
    BOOST_TEST(f.kind() == wd::kind::null);
}

// With utf16, text is held as JavaScript holds it, a lone surrogate included.
void text_in_utf16() {
    const std::string_view text = "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80";
    const std::u16string_view units = u"aé€\U0001F600";
    fixture<js> f;
    BOOST_TEST(f.put(text).has_value());
    BOOST_TEST(f.root()->text.units() == units);
    BOOST_TEST(f.get<std::u16string_view>() == units);
    BOOST_TEST(f.get<std::u16string>() == std::u16string(units));
    BOOST_TEST(f.get<std::string>() == std::string(text));
    BOOST_TEST(f.get<std::string_view>().error() == wd::errc::type);
    BOOST_TEST(f.put(units).has_value());
    BOOST_TEST(f.root()->text.units() == units);
    BOOST_TEST(f.put(std::string_view("\xff")).error() == wd::errc::type);
    BOOST_TEST(f.put(u"").has_value());
    BOOST_TEST(f.get<std::u16string_view>() == u"");

    const std::u16string_view lone = u"x\xd800";
    BOOST_TEST(f.put(lone).has_value());
    BOOST_TEST(f.root()->text.units() == lone);
    BOOST_TEST(f.get<std::u16string>() == std::u16string(lone));
    // UTF-8 cannot hold it.
    BOOST_TEST(f.get<std::string>().error() == wd::errc::type);
}

// A date within JavaScript's range of plus or minus 8.64e15 ms, and a string that is exactly
// Date.prototype.toJSON's text read as one.
template <class Traits>
void dates() {
    using std::chrono::milliseconds;
    constexpr std::int64_t limit = 8'640'000'000'000'000;
    fixture<Traits> f;
    BOOST_TEST(f.put(wd::date_ms(milliseconds(limit))).has_value());
    BOOST_TEST(f.kind() == wd::kind::date);
    BOOST_TEST(f.template get<wd::date_ms>() == wd::date_ms(milliseconds(limit)));
    BOOST_TEST(f.put(wd::date_ms(milliseconds(-limit))).has_value());
    BOOST_TEST(f.put(wd::date_ms(milliseconds(limit + 1))).error() == wd::errc::range);
    BOOST_TEST(f.put(wd::date_ms(milliseconds(-limit - 1))).error() == wd::errc::range);
    BOOST_TEST(f.put(wd::array{wd::date_ms(milliseconds(limit + 1))}).error() == wd::errc::range);
    BOOST_TEST(f.template get<wd::date_ms>() == wd::date_ms(milliseconds(-limit)));
    BOOST_TEST(f.template get<std::string>().error() == wd::errc::type);
    BOOST_TEST(f.template get<std::int64_t>().error() == wd::errc::type);

    BOOST_TEST(f.put("2026-10-12T03:04:05.678Z").has_value());
    BOOST_TEST(f.kind() == wd::kind::string);
    BOOST_TEST(f.template get<wd::date_ms>() == wd::date_ms(milliseconds(1791774245678)));
    BOOST_TEST(f.put("+275760-09-13T00:00:00.000Z").has_value());
    BOOST_TEST(f.template get<wd::date_ms>() == wd::date_ms(milliseconds(limit)));
    for (const std::string_view other : {
             "x",
             "2026-10-12",
             "2026-10-12T03:04:05.678",
             "+275760-09-13T00:00:00.001Z",
             "2026-02-30T00:00:00.000Z",
             "2026-10-12T03:04:05.678Z\xc3\xa9",
         }) {
        BOOST_TEST(f.put(other).has_value());
        BOOST_TEST(f.template get<wd::date_ms>().error() == wd::errc::type);
    }
    BOOST_TEST(f.put(true).has_value());
    BOOST_TEST(f.template get<wd::date_ms>().error() == wd::errc::type);
}

// Bytes are stored as they are and read as a vector or a view; a string of the policy's base64
// reads as bytes, which a view cannot.
void bytes_and_base64() {
    const std::vector<std::byte> foo = bytes_of("foo");
    fixture<defaults> f;
    BOOST_TEST(f.put(std::span<const std::byte>(foo)).has_value());
    BOOST_TEST(f.kind() == wd::kind::bytes);
    BOOST_TEST(f.get<std::vector<std::byte>>() == foo);
    BOOST_TEST(std::ranges::equal(*f.get<std::span<const std::byte>>(), foo));
    BOOST_TEST(f.get<std::string>().error() == wd::errc::type);
    BOOST_TEST(f.put(foo).has_value());
    BOOST_TEST(f.get<std::vector<std::byte>>() == foo);
    BOOST_TEST(f.put(std::span<const std::byte>()).has_value());
    BOOST_TEST(f.kind() == wd::kind::bytes);
    BOOST_TEST(f.get<std::vector<std::byte>>()->empty());

    f.place(bytes_item<defaults>(foo));
    BOOST_TEST(f.get<std::vector<std::byte>>() == foo);
    BOOST_TEST(f.get<std::string_view>().error() == wd::errc::type);

    f.place(string_item<defaults>("Zm9v"));
    BOOST_TEST(f.get<std::vector<std::byte>>() == foo);
    BOOST_TEST(f.get<std::span<const std::byte>>().error() == wd::errc::type);
    f.place(string_item<defaults>("Zm8="));
    BOOST_TEST(f.get<std::vector<std::byte>>() == bytes_of("fo"));
    for (const std::string_view other : {"Zm9", "Zm9v!", "Zm-_", "Zm9w===="}) {
        f.place(string_item<defaults>(other));
        BOOST_TEST(f.get<std::vector<std::byte>>().error() == wd::errc::type);
    }
    f.place(integer_item<defaults>(1));
    BOOST_TEST(f.get<std::vector<std::byte>>().error() == wd::errc::type);
    f.place(container_item<defaults>(wd::kind::object));
    BOOST_TEST(f.get<std::vector<std::byte>>().error() == wd::errc::type);

    fixture<url_traits> u;
    u.place(string_item<url_traits>("-_8"));
    BOOST_TEST(u.get<std::vector<std::byte>>() ==
               (std::vector<std::byte>{std::byte{0xfb}, std::byte{0xff}}));
    u.place(string_item<url_traits>("Zm8="));
    BOOST_TEST(u.get<std::vector<std::byte>>().error() == wd::errc::type);
}

// With javascript_bytes, bytes read from an object shaped as JSON.stringify writes a Uint8Array:
// keys exactly "0" to "n-1", each an integer from 0 to 255; a string is no bytes.
void javascript_bytes_shape() {
    using item = detail::item<js>;
    fixture<js> f;
    f.place(container_item<js>(wd::kind::object));
    BOOST_TEST(f.get<std::vector<std::byte>>()->empty());
    f.member("1", integer_item<js>(111));
    f.member("0", integer_item<js>(102));
    f.member("2", number_item<js>(111.0));
    BOOST_TEST(f.get<std::vector<std::byte>>() == bytes_of("foo"));

    const auto refused = [](std::initializer_list<std::pair<std::string_view, item>> members) {
        fixture<js> g;
        g.place(container_item<js>(wd::kind::object));
        for (const auto& [key, value] : members) {
            g.member(key, value);
        }
        BOOST_TEST(g.get<std::vector<std::byte>>().error() == wd::errc::type);
    };
    refused({{"0", integer_item<js>(1)}, {"2", integer_item<js>(2)}});
    refused({{"0", integer_item<js>(256)}});
    refused({{"0", integer_item<js>(-1)}});
    refused({{"0", number_item<js>(1.5)}});
    refused({{"0", integer_item<js>(1)}, {"x", integer_item<js>(2)}});
    refused({{"00", integer_item<js>(1)}});
    refused({{"0", boolean_item<js>(true)}});

    f.place(string_item<js>(u"Zm9v"));
    BOOST_TEST(f.get<std::vector<std::byte>>().error() == wd::errc::type);
    f.place(container_item<js>(wd::kind::array));
    BOOST_TEST(f.get<std::vector<std::byte>>().error() == wd::errc::type);
}

// A JSON string is a string, unless a reviver asks for a date or bytes and it is one.
void strings_revived() {
    detail::prepared<defaults> p;
    BOOST_TEST(detail::prepare_string("Zm9v", false, false, p).has_value());
    BOOST_TEST(p.value().kind == wd::kind::string);
    BOOST_TEST(detail::prepare_string("Zm9v", false, true, p).has_value());
    BOOST_TEST(p.value().kind == wd::kind::bytes);
    BOOST_TEST(std::ranges::equal(p.value().text, bytes_of("foo")));
    BOOST_TEST(detail::prepare_string("Zm9", true, true, p).has_value());
    BOOST_TEST(p.value().kind == wd::kind::string);
    BOOST_TEST(detail::prepare_string("2026-10-12T03:04:05.678Z", true, false, p).has_value());
    BOOST_TEST(p.value().kind == wd::kind::date);
    BOOST_TEST(detail::prepare_string("2026-10-12T03:04:05.678Z", false, true, p).has_value());
    BOOST_TEST(p.value().kind == wd::kind::string);
    BOOST_TEST(detail::prepare_string("\xff", false, false, p).error() == wd::errc::type);

    fixture<js> f;
    detail::prepared<js> q;
    BOOST_TEST(detail::prepare_string("Zm9v", true, true, q).has_value());
    BOOST_TEST(q.value().kind == wd::kind::string);
    BOOST_TEST(f.put_item(q.value()).has_value());
    BOOST_TEST(f.get<std::u16string_view>() == u"Zm9v");
    BOOST_TEST(detail::prepare_string("2026-10-12T03:04:05.678Z", true, true, q).has_value());
    BOOST_TEST(f.put_item(q.value()).has_value());
    BOOST_TEST(f.get<wd::date_ms>() == wd::date_ms(std::chrono::milliseconds(1791774245678)));
}

// null, booleans, and the items of empty containers and of a root that holds nothing.
template <class Traits>
void null_booleans_and_empties() {
    fixture<Traits> f;
    BOOST_TEST(f.put(true).has_value());
    BOOST_TEST(f.kind() == wd::kind::boolean);
    BOOST_TEST(f.template get<bool>() == true);
    BOOST_TEST(f.template get<std::int32_t>().error() == wd::errc::type);
    BOOST_TEST(f.put(false).has_value());
    BOOST_TEST(f.template get<bool>() == false);
    BOOST_TEST(f.put(nullptr).has_value());
    BOOST_TEST(f.kind() == wd::kind::null);
    BOOST_TEST(f.template get<bool>().error() == wd::errc::type);
    BOOST_TEST(f.put(true).has_value());
    BOOST_TEST(f.put(wd::null).has_value());
    BOOST_TEST(f.kind() == wd::kind::null);

    BOOST_TEST(f.put_item(detail::empty_item<Traits>(wd::kind::object)).has_value());
    BOOST_TEST_EQ(dump(f), "{}");
    BOOST_TEST(f.put_item(detail::empty_item<Traits>(wd::kind::array)).has_value());
    BOOST_TEST_EQ(dump(f), "[]");
    BOOST_TEST(f.put_item(detail::empty_item<Traits>(wd::kind::null)).has_value());
    BOOST_TEST_EQ(dump(f), "null");
    BOOST_TEST(f.put_item(detail::empty_item<Traits>(wd::kind::undefined)).has_value());
    BOOST_TEST(f.kind() == wd::kind::undefined);
    BOOST_TEST(f.template get<bool>().error() == wd::errc::type);
    BOOST_TEST(detail::empty_item<Traits>(wd::kind::number).kind == wd::kind::null);

    // A null node is undefined: nothing reads from it.
    BOOST_TEST(detail::read<bool>(f.t, nothing<Traits>).error() == wd::errc::type);
    BOOST_TEST(detail::read<std::string>(f.t, f.t.find(999)).error() == wd::errc::type);
}

// Literals nested, every form among them; a repeated key keeps its last value in its first place,
// as JavaScript does; a literal is refused whole when one of its values is.
template <class Traits>
void literals() {
    const std::vector<std::byte> foo = bytes_of("foo");
    fixture<Traits> f;
    BOOST_TEST(
        f.put(wd::object{
                  {"a", wd::array{1, "x", wd::object{{"b", true}}}}, {"c", nullptr}, {"a", 2}})
            .has_value());
    BOOST_TEST_EQ(dump(f), R"({"a":2,"c":null})");
    BOOST_TEST_EQ(f.t.count(f.t.root()), 2U);

    BOOST_TEST(
        f.put(wd::array{1, wd::array{}, wd::object{}, decimal128_t{15, -1}, "\xc3\xa9", u"ü",
                        wd::date_ms(std::chrono::milliseconds(5)), std::span<const std::byte>(foo),
                        wd::null, false, std::numeric_limits<std::uint64_t>::max(),
                        wd::array{wd::literal(wd::array{wd::object{{"k", "v"}, {"k", "w"}}})}})
            .has_value());
    if constexpr (std::same_as<Traits, js>) {
        BOOST_TEST_EQ(dump(f),
                      R"([1,[],{},1.5,"é","ü",date(5),bytes(3),null,false,18446744073709552000,)"
                      R"([[{"k":"w"}]]])");
    } else {
        BOOST_TEST_EQ(dump(f),
                      R"([1,[],{},1.5,"é","ü",date(5),bytes(3),null,false,18446744073709551615,)"
                      R"([[{"k":"w"}]]])");
    }

    BOOST_TEST(f.put(wd::object{{"b", 1}, {"2", 2}, {"a", 3}, {"1", 4}}).has_value());
    BOOST_TEST_EQ(dump(f), R"({"1":4,"2":2,"b":1,"a":3})");

    // A key or a string invalid in UTF-8 refuses the whole literal, which changes nothing.
    BOOST_TEST(f.put(wd::object{{"ok", 1}, {"\xff", 2}}).error() == wd::errc::type);
    BOOST_TEST(f.put(wd::array{1, wd::array{"\xc3"}}).error() == wd::errc::type);
    BOOST_TEST(f.put(wd::literal(std::numeric_limits<decimal64_t>::quiet_NaN())).error() ==
               wd::errc::range);
    BOOST_TEST_EQ(dump(f), R"({"1":4,"2":2,"b":1,"a":3})");
    BOOST_TEST(f.put(wd::literal(7)).has_value());
    BOOST_TEST_EQ(dump(f), "7");
}

// A prepared value moves with its item pointing into its new place, and is prepared again from
// nothing; on failure it holds null.
void prepared_values_move() {
    fixture<js> f;
    detail::prepared<js> p;
    // The literal outlives the prepared value, which points at its keys and its UTF-16 text.
    const wd::object literal{{"a", wd::array{"\xc3\xa9", u"x"}}, {"b", "text"}};
    BOOST_TEST(detail::prepare(literal, p).has_value());
    detail::prepared<js> q(std::move(p));
    detail::prepared<js> r;
    r = std::move(q);
    BOOST_TEST(f.put_item(r.value()).has_value());
    BOOST_TEST_EQ(dump(f), R"({"a":["é","x"],"b":"text"})");

    BOOST_TEST(detail::prepare(std::u16string_view(u"y"), r).has_value());
    BOOST_TEST(r.value().entries == nullptr);
    BOOST_TEST(f.put_item(r.value()).has_value());
    BOOST_TEST_EQ(dump(f), R"("y")");
    BOOST_TEST(detail::prepare(std::string_view("\xff"), r).error() == wd::errc::type);
    BOOST_TEST(r.value().kind == wd::kind::null);
}

// copy_of makes the item a deep copy of a node, of this tree or another.
void copies() {
    fixture<defaults> f;
    BOOST_TEST(f.put(wd::object{{"a", wd::array{1, "x"}}}).has_value());
    const std::uint64_t a = f.t.member(f.t.root(), "a")->id;
    detail::prepared<defaults> p;
    p.copy_of(f.t, a);
    BOOST_TEST(f.t.set_member(f.t.root(), "b", p.value()).has_value());
    BOOST_TEST_EQ(dump(f), R"({"a":[1,"x"],"b":[1,"x"]})");

    fixture<defaults> g;
    p.copy_of(f.t, f.t.root());
    BOOST_TEST(g.put_item(p.value()).has_value());
    BOOST_TEST_EQ(dump(g), R"({"a":[1,"x"],"b":[1,"x"]})");
}

// Every readable type is read from the kind it reads, and from no other.
template <class Traits>
void every_type_refuses_another_kind() {
    fixture<Traits> f;
    f.place(boolean_item<Traits>(true));
    BOOST_TEST(f.template get<std::int8_t>().error() == wd::errc::type);
    BOOST_TEST(f.template get<std::uint64_t>().error() == wd::errc::type);
    BOOST_TEST(f.template get<decimal64_t>().error() == wd::errc::type);
    BOOST_TEST(f.template get<typename Traits::number>().error() == wd::errc::type);
    BOOST_TEST(f.template get<std::string>().error() == wd::errc::type);
    BOOST_TEST(f.template get<std::string_view>().error() == wd::errc::type);
    BOOST_TEST(f.template get<std::u16string>().error() == wd::errc::type);
    BOOST_TEST(f.template get<std::u16string_view>().error() == wd::errc::type);
    BOOST_TEST(f.template get<wd::date_ms>().error() == wd::errc::type);
    BOOST_TEST(f.template get<std::vector<std::byte>>().error() == wd::errc::type);
    BOOST_TEST(f.template get<std::span<const std::byte>>().error() == wd::errc::type);
    f.place(integer_item<Traits>(1));
    BOOST_TEST(f.template get<bool>().error() == wd::errc::type);
    BOOST_TEST(f.template get<std::string>().error() == wd::errc::type);
    BOOST_TEST(f.template get<wd::date_ms>().error() == wd::errc::type);
    f.place(date_item<Traits>(0));
    BOOST_TEST(f.template get<std::int64_t>().error() == wd::errc::type);
    f.place(container_item<Traits>(wd::kind::array));
    BOOST_TEST(f.template get<std::string>().error() == wd::errc::type);
    BOOST_TEST(f.template get<std::int64_t>().error() == wd::errc::type);
}

// Integers read from either representation by a range check: an int8 of 300 does not fit.
void integers_read_back() {
    fixture<defaults> f;
    f.place(integer_item<defaults>(300));
    BOOST_TEST(f.get<std::int8_t>().error() == wd::errc::range);
    BOOST_TEST(f.get<std::uint8_t>().error() == wd::errc::range);
    BOOST_TEST(f.get<std::int16_t>() == std::int16_t{300});
    BOOST_TEST(f.get<decimal128_t>() == decimal128_t(300));
    f.place(integer_item<defaults>(-129));
    BOOST_TEST(f.get<std::int8_t>().error() == wd::errc::range);
    f.place(integer_item<defaults>(-128));
    BOOST_TEST(f.get<std::int8_t>() == std::int8_t{-128});
    f.place(integer_item<defaults>(-1));
    BOOST_TEST(f.get<std::uint64_t>().error() == wd::errc::range);
    BOOST_TEST(f.get<std::uint32_t>().error() == wd::errc::range);
    BOOST_TEST(f.get<std::int64_t>() == -1);

    // A number that is an integer reads as one; a fraction or a value too wide does not.
    f.place(number_item<defaults>(decimal("250")));
    BOOST_TEST(f.get<std::uint8_t>() == std::uint8_t{250});
    BOOST_TEST(f.get<std::int8_t>().error() == wd::errc::range);
    f.place(number_item<defaults>(decimal("2.5")));
    BOOST_TEST(f.get<std::int64_t>().error() == wd::errc::range);
    f.place(number_item<defaults>(decimal("-9223372036854775809")));
    BOOST_TEST(f.get<std::int64_t>().error() == wd::errc::range);
    f.place(number_item<defaults>(decimal("18446744073709551616")));
    BOOST_TEST(f.get<std::uint64_t>().error() == wd::errc::range);
    BOOST_TEST(f.get<decimal128_t>() == decimal("18446744073709551616"));

    fixture<js> g;
    g.place(number_item<js>(9223372036854775808.0));
    BOOST_TEST(g.get<std::int64_t>().error() == wd::errc::range);
    BOOST_TEST(g.get<std::uint64_t>() == std::uint64_t{9223372036854775808ULL});
    g.place(number_item<js>(18446744073709551616.0));
    BOOST_TEST(g.get<std::uint64_t>().error() == wd::errc::range);
    g.place(number_item<js>(-9223372036854775808.0));
    BOOST_TEST(g.get<std::int64_t>() == std::numeric_limits<std::int64_t>::min());
    g.place(number_item<js>(-0.5));
    BOOST_TEST(g.get<std::int64_t>().error() == wd::errc::range);
    // An integer the double cannot hold exactly is no double.
    g.place(integer_item<js>(9007199254740993));
    BOOST_TEST(g.get<double>().error() == wd::errc::range);
    BOOST_TEST(g.get<std::int64_t>() == 9007199254740993);
    g.place(integer_item<js>(16777217));
    BOOST_TEST(g.get<double>() == 16777217.0);
    BOOST_TEST(g.get<float>().error() == wd::errc::range);
}

// Structural equality: kinds, numbers by value, strings, dates and bytes by value, arrays in
// order, objects by key in any order.
template <class Traits>
void equality() {
    const std::vector<std::byte> foo = bytes_of("foo");
    const auto equal = [](const fixture<Traits>& a, const fixture<Traits>& b) {
        return detail::equal(a.t, a.root(), b.t, b.root());
    };
    fixture<Traits> a;
    fixture<Traits> b;
    BOOST_TEST(
        a.put(wd::object{{"a", 1}, {"b", wd::array{decimal128_t{10, -1}, "x"}}}).has_value());
    BOOST_TEST(b.put(wd::object{{"b", wd::array{1, "x"}}, {"a", 1}}).has_value());
    BOOST_TEST(equal(a, b));
    BOOST_TEST(equal(b, a));
    BOOST_TEST(equal(a, a));
    BOOST_TEST(b.put(wd::object{{"a", 1}}).has_value());
    BOOST_TEST(!equal(a, b));
    BOOST_TEST(!equal(b, a));
    BOOST_TEST(b.put(wd::object{{"a", 1}, {"c", wd::array{1, "x"}}}).has_value());
    BOOST_TEST(!equal(a, b));
    BOOST_TEST(b.put(wd::object{{"a", 1}, {"b", wd::array{1, "y"}}}).has_value());
    BOOST_TEST(!equal(a, b));
    BOOST_TEST(b.put(wd::object{{"a", 1}, {"b", wd::array{1, "x", 2}}}).has_value());
    BOOST_TEST(!equal(a, b));

    const wd::date_ms five(std::chrono::milliseconds(5));
    const wd::date_ms six(std::chrono::milliseconds(6));

    struct row {
        wd::literal x;
        wd::literal y;
        bool same = false;
    };

    for (const row& r : {
             row{1, decimal128_t{1}, true},
             row{1, 2, false},
             row{1, "1", false},
             row{nullptr, wd::null, true},
             row{nullptr, false, false},
             row{true, true, true},
             row{true, false, false},
             row{"x", "x", true},
             row{"x", u"x", true},
             row{"x", "y", false},
             row{five, five, true},
             row{five, six, false},
             row{std::span<const std::byte>(foo), std::span<const std::byte>(foo), true},
             row{std::span<const std::byte>(foo), "foo", false},
             row{wd::array{}, wd::object{}, false},
             row{wd::array{}, wd::array{}, true},
             row{wd::object{{"k", wd::object{}}}, wd::object{{"k", wd::object{}}}, true},
             row{decimal128_t{15, -1}, decimal64_t{150, -2}, true},
         }) {
        BOOST_TEST(a.put(r.x).has_value());
        BOOST_TEST(b.put(r.y).has_value());
        BOOST_TEST_EQ(equal(a, b), r.same);
    }

    // A null node is undefined, equal only to another.
    BOOST_TEST(detail::equal(a.t, nothing<Traits>, b.t, nothing<Traits>));
    BOOST_TEST(!detail::equal(a.t, a.root(), b.t, nothing<Traits>));
    BOOST_TEST(!detail::equal(a.t, nothing<Traits>, b.t, b.root()));
    BOOST_TEST(a.put_item(detail::empty_item<Traits>(wd::kind::undefined)).has_value());
    BOOST_TEST(b.put_item(detail::empty_item<Traits>(wd::kind::undefined)).has_value());
    BOOST_TEST(equal(a, b));
}

}  // namespace

int main() {
    integers_of_every_width<defaults>();
    integers_of_every_width<js>();
    numbers_by_value();
    numbers_are_normalised();
    large_numbers_stay_exact();
    numbers_by_value_as_javascript();
    numbers_from_text();
    non_finite_numbers();
    binary_numbers();
    decimal_types();
    text_in_utf8();
    invalid_text_is_refused();
    text_in_utf16();
    dates<defaults>();
    dates<js>();
    bytes_and_base64();
    javascript_bytes_shape();
    strings_revived();
    null_booleans_and_empties<defaults>();
    null_booleans_and_empties<js>();
    literals<defaults>();
    literals<js>();
    prepared_values_move();
    copies();
    every_type_refuses_another_kind<defaults>();
    every_type_refuses_another_kind<js>();
    integers_read_back();
    equality<defaults>();
    equality<js>();
    return boost::report_errors();
}
