// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Numbers: JSON text read exactly into decimal digits, written back as JavaScript's
// Number::toString writes them (the expected texts are Node 26's), and converted to and from the
// integers and the decimal types a document stores.

#include <webcpp/document/number.hpp>

#include <boost/core/lightweight_test.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace wd = webcpp::document;

namespace {

wd::decimal_digits digits_of(std::string_view d, std::int32_t point, bool negative = false) {
    wd::decimal_digits out;
    out.negative = negative;
    for (const char c : d) {
        out.digits[out.count++] = c;
    }
    out.point = point;
    return out;
}

std::string javascript(std::string_view d, std::int32_t point, bool negative = false) {
    return std::string(wd::write_javascript(digits_of(d, point, negative)).view());
}

std::string javascript(const wd::decimal_digits& d) {
    return std::string(wd::write_javascript(d).view());
}

// Each row of the formatting table, with Node's text.
void writes_as_javascript_does() {
    BOOST_TEST_EQ(javascript("", 0), "0");
    BOOST_TEST_EQ(javascript("1", 1), "1");
    BOOST_TEST_EQ(javascript("1", 1, true), "-1");
    BOOST_TEST_EQ(javascript("15", 1), "1.5");
    BOOST_TEST_EQ(javascript("1", 3), "100");
    BOOST_TEST_EQ(javascript("12345", 3), "123.45");
    BOOST_TEST_EQ(javascript("1", 21), "100000000000000000000");
    BOOST_TEST_EQ(javascript("1", 22), "1e+21");
    BOOST_TEST_EQ(javascript("15", 22, true), "-1.5e+21");
    BOOST_TEST_EQ(javascript("1", -5), "0.000001");
    BOOST_TEST_EQ(javascript("1", -6), "1e-7");
    BOOST_TEST_EQ(javascript("1234", -6), "1.234e-7");
    BOOST_TEST_EQ(javascript("5", 0), "0.5");
    BOOST_TEST_EQ(javascript("1", 0), "0.1");
    BOOST_TEST_EQ(javascript("12", 0), "0.12");
    BOOST_TEST_EQ(javascript("1", 2), "10");
    BOOST_TEST_EQ(javascript("1234567", 7), "1234567");
    BOOST_TEST_EQ(javascript("123", 1), "1.23");
    BOOST_TEST_EQ(javascript("17976931348623157", 309), "1.7976931348623157e+308");
    // Exact where a double rounds: 18 digits, and below a double's range.
    BOOST_TEST_EQ(javascript("123456789012345678", 21), "123456789012345678000");
    BOOST_TEST_EQ(javascript("1", -323), "1e-324");
    BOOST_TEST_EQ(javascript("1234567890123456789012345678901234", 6144),
                  "1.234567890123456789012345678901234e+6143");
}

std::string rescan(std::string_view text) {
    auto d = wd::scan_number(text);
    if (!d) {
        return "error " + d.error().message();
    }
    return std::string(wd::write_javascript(*d).view());
}

// The edges of the text: signs, zeros, exponents, the 34-digit limit.
void number_edges() {
    BOOST_TEST_EQ(rescan("-0"), "0");
    BOOST_TEST_EQ(rescan("0e0"), "0");
    BOOST_TEST_EQ(rescan("-0.0e-5"), "0");
    BOOST_TEST_EQ(rescan("0.0000001"), "1e-7");
    BOOST_TEST_EQ(rescan("1e21"), "1e+21");
    BOOST_TEST_EQ(rescan("1E+2"), "100");
    BOOST_TEST_EQ(rescan("1.50"), "1.5");
    BOOST_TEST_EQ(rescan("1.000000000000000000000000000000000000000"), "1");
    BOOST_TEST_EQ(rescan("1234567890123456789012345678901234"),
                  "1.234567890123456789012345678901234e+33");
    BOOST_TEST_EQ(rescan("12345678901234567890123456789012345"),
                  "error a number or index out of range");
    BOOST_TEST_EQ(rescan("123456789012345678901234567890123456789"),
                  "error a number or index out of range");
    BOOST_TEST_EQ(rescan("1E400"), "1e+400");
    BOOST_TEST_EQ(rescan("1e-400"), "1e-400");
    BOOST_TEST_EQ(rescan("1e2147483648"), "error a number or index out of range");
    BOOST_TEST_EQ(rescan("0e2147483648"), "0");
    BOOST_TEST_EQ(rescan("-0.000e-99999999999"), "0");
}

// Zero has one form, whatever its text: no digits, no sign, the point at 0.
void zero_has_one_form() {
    for (const std::string_view text : {"0", "-0", "0.000", "-0.0e-5", "0E+99"}) {
        auto d = wd::scan_number(text);
        if (!BOOST_TEST(d.has_value())) {
            continue;
        }
        BOOST_TEST_EQ(d->count, 0U);
        BOOST_TEST_EQ(d->point, 0);
        BOOST_TEST(!d->negative);
    }
}

// Every text outside JSON's grammar is a syntax error, never a guess.
void text_outside_the_grammar() {
    for (const std::string_view text : {
             "",
             "-",
             "+1",
             "01",
             "-01",
             "1.",
             ".5",
             "1e",
             "1e+",
             "1.e5",
             "0x10",
             "1 ",
             " 1",
             "1e5.0",
             "NaN",
             "Infinity",
             "--1",
             "1-",
         }) {
        const auto d = wd::scan_number(text);
        BOOST_TEST(!d.has_value());
        if (!d) {
            BOOST_TEST(d.error() == wd::errc::syntax);
        }
    }
    // Trailing zeros past the 34th significant digit are only counted, and a point keeps them.
    BOOST_TEST_EQ(rescan("123456789012345678901234567890123400000"),
                  "1.234567890123456789012345678901234e+38");
    BOOST_TEST_EQ(rescan("0.000000000000000000000000000000000000001"), "1e-39");
    // The exponent and the point together must fit in std::int32_t.
    BOOST_TEST_EQ(rescan("1e2147483646"), "1e+2147483646");
    BOOST_TEST_EQ(rescan("10e2147483646"), "error a number or index out of range");
    BOOST_TEST_EQ(rescan("1e-2147483648"), "1e-2147483648");
    BOOST_TEST_EQ(rescan("0.01e-2147483648"), "error a number or index out of range");
}

void integers() {
    BOOST_TEST(wd::to_int64(*wd::scan_number("9223372036854775807")) == INT64_MAX);
    BOOST_TEST(!wd::to_int64(*wd::scan_number("9223372036854775808")));
    BOOST_TEST(wd::to_int64(*wd::scan_number("-9223372036854775808")) == INT64_MIN);
    BOOST_TEST(!wd::to_int64(*wd::scan_number("-9223372036854775809")));
    BOOST_TEST(wd::to_int64(*wd::scan_number("1.0")) == 1);
    BOOST_TEST(wd::to_int64(*wd::scan_number("1e0")) == 1);
    BOOST_TEST(wd::to_int64(*wd::scan_number("1.5e1")) == 15);
    BOOST_TEST(!wd::to_int64(*wd::scan_number("1.5")));
    BOOST_TEST(!wd::to_int64(*wd::scan_number("1e20")));
    BOOST_TEST_EQ(javascript(wd::from_int64(INT64_MIN)), "-9223372036854775808");
    BOOST_TEST_EQ(javascript(wd::from_int64(0)), "0");
    // Trailing zeros are dropped: 1000 is one digit with the point after four.
    const wd::decimal_digits thousand = wd::from_int64(-1000);
    BOOST_TEST(thousand.negative);
    BOOST_TEST_EQ(thousand.count, 1U);
    BOOST_TEST_EQ(thousand.point, 4);
    for (std::int64_t v : {
             INT64_MIN,
             INT64_MIN + 1,
             std::int64_t{-1},
             std::int64_t{0},
             std::int64_t{7},
             std::int64_t{120},
             INT64_MAX,
         }) {
        BOOST_TEST(wd::to_int64(wd::from_int64(v)) == v);
    }
}

void unsigned_integers() {
    BOOST_TEST(wd::to_uint64(*wd::scan_number("18446744073709551615")) == UINT64_MAX);
    BOOST_TEST(!wd::to_uint64(*wd::scan_number("18446744073709551616")));
    BOOST_TEST(!wd::to_uint64(*wd::scan_number("-1")));
    BOOST_TEST(wd::to_uint64(*wd::scan_number("-0")) == 0U);
    BOOST_TEST(wd::to_uint64(*wd::scan_number("1.8e1")) == 18U);
    BOOST_TEST(!wd::to_uint64(*wd::scan_number("0.5")));
    BOOST_TEST_EQ(javascript(wd::from_uint64(UINT64_MAX)), "18446744073709551615");
    for (std::uint64_t v : {
             std::uint64_t{0},
             std::uint64_t{1},
             std::uint64_t{10},
             std::uint64_t{9223372036854775808U},
             UINT64_MAX - 1,
             UINT64_MAX,
         }) {
        BOOST_TEST(wd::to_uint64(wd::from_uint64(v)) == v);
    }
}

void decimal128_round_trips() {
    for (const std::string_view text : {
             "0.1",
             "-1.5",
             "1e-400",
             "1e+400",
             "1234567890123456789012345678901234",
             "18446744073709551615",
             "0.000001",
         }) {
        auto d = wd::scan_number(text);
        if (!BOOST_TEST(d.has_value())) {
            continue;
        }
        auto v = wd::to_decimal128(*d);
        if (!BOOST_TEST(v.has_value())) {
            continue;
        }
        BOOST_TEST_EQ(javascript(wd::from_decimal128(*v)), javascript(*d));
    }
    // Normalised: the cohorts of 1.5 are one set of digits.
    const wd::decimal_digits a = wd::from_decimal128(*wd::to_decimal128(*wd::scan_number("1.50")));
    const wd::decimal_digits b = wd::from_decimal128(*wd::to_decimal128(*wd::scan_number("1.5")));
    BOOST_TEST_EQ(a.count, 2U);
    BOOST_TEST_EQ(a.count, b.count);
    BOOST_TEST_EQ(a.point, b.point);
    // decimal128's exponent range: 1e6145 does not fit.
    BOOST_TEST(!wd::to_decimal128(*wd::scan_number("1e6145")));
}

template <class Decimal>
std::string through(std::string_view text) {
    auto d = wd::scan_number(text);
    if (!d) {
        return "error " + d.error().message();
    }
    auto v = wd::to_decimal<Decimal>(*d);
    if (!v) {
        return "error " + v.error().message();
    }
    return javascript(wd::from_decimal(*v));
}

// A decimal type narrower than decimal128 takes what it holds exactly and refuses the rest.
void smaller_decimals() {
    using boost::decimal::decimal32_t;
    using boost::decimal::decimal64_t;
    const std::string range = "error a number or index out of range";
    BOOST_TEST_EQ(through<decimal64_t>("1234567890123456"), "1234567890123456");
    BOOST_TEST_EQ(through<decimal64_t>("12345678901234567"), range);
    BOOST_TEST_EQ(through<decimal64_t>("1234567890123456e200"), "1.234567890123456e+215");
    BOOST_TEST_EQ(through<decimal32_t>("1e200"), range);
    BOOST_TEST_EQ(through<decimal32_t>("1e-200"), range);
    BOOST_TEST_EQ(through<decimal64_t>("1e200"), "1e+200");
    BOOST_TEST_EQ(through<decimal64_t>("-1e-200"), "-1e-200");
    BOOST_TEST_EQ(through<decimal32_t>("-0"), "0");
    // decimal32's subnormals: the smallest is exact, a 7-digit one rounds, below it is nothing.
    BOOST_TEST_EQ(through<decimal32_t>("1e-101"), "1e-101");
    BOOST_TEST_EQ(through<decimal32_t>("1.234567e-100"), range);
    BOOST_TEST_EQ(through<decimal32_t>("1e-102"), range);
    BOOST_TEST_EQ(through<decimal32_t>("9.999999e96"), "9.999999e+96");
    BOOST_TEST_EQ(through<decimal32_t>("1e97"), range);

    const wd::decimal_digits d = wd::from_decimal(decimal32_t{15, -1});
    BOOST_TEST(!d.negative);
    BOOST_TEST_EQ(std::string_view(d.digits.data(), d.count), "15");
    BOOST_TEST_EQ(d.count, 2U);
    BOOST_TEST_EQ(d.point, 1);
}

// Each of the six decimal types takes a number it holds and gives back the same digits.
void every_decimal_type() {
    using namespace boost::decimal;
    for (const std::string_view text : {"0", "1.5", "-1250", "0.000001", "9999999"}) {
        BOOST_TEST_EQ(through<decimal32_t>(text), rescan(text));
        BOOST_TEST_EQ(through<decimal64_t>(text), rescan(text));
        BOOST_TEST_EQ(through<decimal128_t>(text), rescan(text));
        BOOST_TEST_EQ(through<decimal_fast32_t>(text), rescan(text));
        BOOST_TEST_EQ(through<decimal_fast64_t>(text), rescan(text));
        BOOST_TEST_EQ(through<decimal_fast128_t>(text), rescan(text));
    }
    BOOST_TEST_EQ(through<decimal_fast32_t>("12345678"), "error a number or index out of range");
}

}  // namespace

int main() {
    writes_as_javascript_does();
    number_edges();
    zero_has_one_form();
    text_outside_the_grammar();
    integers();
    unsigned_integers();
    decimal128_round_trips();
    smaller_decimals();
    every_decimal_type();
    return boost::report_errors();
}
