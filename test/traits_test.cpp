// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The traits and what they choose: array-index keys, dates as Date.prototype.toJSON writes them
// (Node 26, measured), base64 (RFC 4648's vectors), UTF-8 and UTF-16 validation and transcoding,
// the scalar and number codecs, the concept traits satisfy, and the tags that tell traits apart.

#include <webcpp/document/detail/utf.hpp>
#include <webcpp/document/traits.hpp>

#include <boost/core/lightweight_test.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace wd = webcpp::document;

namespace {

void array_indices() {
    BOOST_TEST(wd::detail::array_index("0") == 0U);
    BOOST_TEST(wd::detail::array_index("7") == 7U);
    BOOST_TEST(wd::detail::array_index("10") == 10U);
    BOOST_TEST(wd::detail::array_index("4294967294") == 4294967294U);
    for (const std::string_view no : {
             "4294967295",
             "01",
             "-0",
             "-1",
             "1.0",
             " 1",
             "1e2",
             "",
             "a",
             "00",
             "99999999999999999999",
         }) {
        BOOST_TEST(!wd::detail::array_index(no));
    }
}

std::string date_text(std::int64_t ms) {
    return std::string(wd::detail::write_date(wd::date_ms(std::chrono::milliseconds(ms))).view());
}

void dates_as_javascript_writes_them() {
    BOOST_TEST_EQ(date_text(0), "1970-01-01T00:00:00.000Z");
    BOOST_TEST_EQ(date_text(-1), "1969-12-31T23:59:59.999Z");
    BOOST_TEST_EQ(date_text(1791774245678), "2026-10-12T03:04:05.678Z");
    BOOST_TEST_EQ(date_text(8640000000000000), "+275760-09-13T00:00:00.000Z");
    BOOST_TEST_EQ(date_text(-8640000000000000), "-271821-04-20T00:00:00.000Z");
    BOOST_TEST_EQ(date_text(-62198755200000), "-000001-01-01T00:00:00.000Z");
    BOOST_TEST_EQ(date_text(253402300800000), "+010000-01-01T00:00:00.000Z");
    BOOST_TEST_EQ(date_text(253402300799999), "9999-12-31T23:59:59.999Z");
    BOOST_TEST_EQ(date_text(951782400000), "2000-02-29T00:00:00.000Z");
    for (std::int64_t ms : {
             0LL,
             -1LL,
             1791774245678LL,
             8640000000000000LL,
             -8640000000000000LL,
             -62198755200000LL,
             253402300800000LL,
             951782400000LL,
         }) {
        auto back = wd::detail::read_date(date_text(ms));
        BOOST_TEST(back && back->time_since_epoch().count() == ms);
    }
    for (const std::string_view no : {
             "2026-10-12",
             "2026-10-12T03:04:05Z",
             "2026-10-12T03:04:05.678+00:00",
             "2026-13-01T00:00:00.000Z",
             "2026-02-30T00:00:00.000Z",
             "+275760-09-13T00:00:00.001Z",
             "10000-01-01T00:00:00.000Z",
         }) {
        BOOST_TEST(!wd::detail::read_date(no));
    }
}

std::string base64_of(std::string_view raw, bool url) {
    std::string out(wd::detail::base64_size(raw.size(), !url), '?');
    wd::detail::write_base64(std::as_bytes(std::span(raw.data(), raw.size())), out.data(), url);
    return out;
}

// RFC 4648 section 10's vectors, and the URL alphabet.
void base64_vectors() {
    BOOST_TEST_EQ(base64_of("", false), "");
    BOOST_TEST_EQ(base64_of("f", false), "Zg==");
    BOOST_TEST_EQ(base64_of("fo", false), "Zm8=");
    BOOST_TEST_EQ(base64_of("foo", false), "Zm9v");
    BOOST_TEST_EQ(base64_of("foob", false), "Zm9vYg==");
    BOOST_TEST_EQ(base64_of("fooba", false), "Zm9vYmE=");
    BOOST_TEST_EQ(base64_of("foobar", false), "Zm9vYmFy");
    BOOST_TEST_EQ(base64_of("\xfb\xff", true), "-_8");
    BOOST_TEST_EQ(base64_of("\xfb\xff", false), "+/8=");
    std::array<std::byte, 8> out{};
    BOOST_TEST(wd::detail::read_base64("Zm9vYmFy", out.data(), false) == 6U);
    BOOST_TEST(wd::detail::read_base64("-_8", out.data(), true) == 2U);
    for (const std::string_view bad : {"Zg=", "Z", "Zm9v!", "Zg==Zg==", "Zm=v"}) {
        BOOST_TEST(!wd::detail::read_base64(bad, out.data(), false));
    }
}

void utf() {
    BOOST_TEST(wd::detail::valid_utf8("a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80"));
    BOOST_TEST_EQ(wd::detail::utf16_length("a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80"),
                  5U);  // Node: 5
    for (const std::string_view bad :
         {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\xff"}) {
        BOOST_TEST(!wd::detail::valid_utf8(bad));
    }
    BOOST_TEST(wd::detail::valid_utf16(u"a\xd83d\xde00"));
    BOOST_TEST(!wd::detail::valid_utf16(u"\xd800"));
}

// Valid UTF-8 goes to UTF-16 and back unchanged, at every length a character has.
void transcoding() {
    for (const std::string_view text : {
             "",
             "a",
             "\xc3\xa9",
             "\xe2\x82\xac",
             "\xf0\x9f\x98\x80",
             "\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80",
             "\xef\xbf\xbf\xf0\x90\x80\x80\xf4\x8f\xbf\xbf",
             "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80z",
         }) {
        if (!BOOST_TEST(wd::detail::valid_utf8(text))) {
            return;
        }
        std::u16string units(wd::detail::utf16_length(text), u'?');
        BOOST_TEST_EQ(wd::detail::to_utf16(text, units.data()), units.size());
        BOOST_TEST(wd::detail::valid_utf16(units));
        BOOST_TEST_EQ(wd::detail::utf8_size(units), text.size());
        std::string back(wd::detail::utf8_size(units), '?');
        BOOST_TEST_EQ(wd::detail::to_utf8(units, back.data()), back.size());
        BOOST_TEST_EQ(back, text);
    }
    std::array<char16_t, 2> units{};
    BOOST_TEST_EQ(wd::detail::to_utf16("\xf0\x9f\x98\x80", units.data()), 2U);
    BOOST_TEST(units[0] == u'\xd83d' && units[1] == u'\xde00');
    BOOST_TEST_EQ(wd::detail::utf8_size(u"a\x00e9\x20ac\xd83d\xde00"), 10U);
    BOOST_TEST(!wd::detail::valid_utf16(u"\xde00\xd83d"));
    BOOST_TEST(!wd::detail::valid_utf16(u"a\xd83d"));
}

void codecs() {
    std::array<std::byte, 16> raw{};
    wd::scalar_codec<std::int64_t>::store(-2, raw.data());
    BOOST_TEST_EQ(wd::scalar_codec<std::int64_t>::load(raw.data()), -2);
    wd::scalar_codec<bool>::store(true, raw.data());
    BOOST_TEST(raw[0] == std::byte{1});
    using dec = boost::decimal::decimal128_t;
    auto n = wd::number_codec<dec>::parse("1.50");
    BOOST_TEST(n && std::string(wd::number_codec<dec>::write(*n).view()) == "1.5");
    BOOST_TEST(wd::number_codec<dec>::integral(*wd::number_codec<dec>::parse("1.0")) == 1);
    BOOST_TEST(!wd::number_codec<dec>::parse("1e6145"));
    auto d = wd::number_codec<double>::parse("123456789012345678901234567890");
    BOOST_TEST(d &&
               std::string(wd::number_codec<double>::write(*d).view()) == "1.2345678901234568e+29");
    BOOST_TEST(std::string(wd::number_codec<double>::write(0.1).view()) == "0.1");
    static_assert(wd::stored_scalar<bool> && wd::stored_scalar<std::int64_t> &&
                  wd::stored_scalar<dec> && wd::stored_scalar<double> &&
                  wd::stored_scalar<wd::date_ms>);
    static_assert(!wd::stored_scalar<std::string>);
}

// Every codec stores its bytes least significant first, so an image reads the same anywhere.
void scalar_bytes() {
    std::array<std::byte, 16> raw{};
    wd::scalar_codec<std::int64_t>::store(0x0102030405060708, raw.data());
    BOOST_TEST(raw[0] == std::byte{8} && raw[7] == std::byte{1});
    wd::scalar_codec<double>::store(1.0, raw.data());
    BOOST_TEST(raw[0] == std::byte{0} && raw[6] == std::byte{0xf0} && raw[7] == std::byte{0x3f});
    BOOST_TEST_EQ(wd::scalar_codec<double>::load(raw.data()), 1.0);
    wd::scalar_codec<wd::date_ms>::store(wd::date_ms(std::chrono::milliseconds(-1)), raw.data());
    BOOST_TEST(raw[0] == std::byte{0xff} && raw[7] == std::byte{0xff});
    BOOST_TEST_EQ(wd::scalar_codec<wd::date_ms>::load(raw.data()).time_since_epoch().count(), -1);
    wd::scalar_codec<bool>::store(false, raw.data());
    BOOST_TEST(raw[0] == std::byte{0} && !wd::scalar_codec<bool>::load(raw.data()));
    using dec = boost::decimal::decimal128_t;
    // 1 with the exponent 0 is the coefficient 1 and the biased exponent 6176 << 49 in the high
    // word, IEEE 754's BID encoding.
    const dec one(1, 0);
    wd::scalar_codec<dec>::store(one, raw.data());
    BOOST_TEST(raw[0] == std::byte{1} && raw[1] == std::byte{0} && raw[15] == std::byte{0x30} &&
               raw[14] == std::byte{0x40});
    BOOST_TEST(wd::scalar_codec<dec>::load(raw.data()) == one);
}

void binary_digits() {
    using codec = wd::number_codec<double>;
    const wd::decimal_digits tenth = codec::digits(0.1);
    BOOST_TEST(!tenth.negative);
    BOOST_TEST_EQ(tenth.count, 1U);
    BOOST_TEST_EQ(tenth.digits[0], '1');
    BOOST_TEST_EQ(tenth.point, 0);
    const wd::decimal_digits zero = codec::digits(-0.0);
    BOOST_TEST(!zero.negative && zero.count == 0 && zero.point == 0);
    for (const double v : {
             0.1,
             -2.5,
             1e21,
             1e-7,
             5e-324,
             2.2250738585072014e-308,
             1.7976931348623157e308,
             123456789012345680.0,
             0.0,
         }) {
        auto back = codec::from_digits(codec::digits(v));
        BOOST_TEST(back && *back == v);
    }
    BOOST_TEST(codec::integral(-0.0) == 0);
    BOOST_TEST(!codec::integral(9223372036854775808.0));
    BOOST_TEST(codec::integral(-9223372036854775808.0) == std::numeric_limits<std::int64_t>::min());
    BOOST_TEST(!codec::integral(1.5));
    BOOST_TEST(!codec::integral(std::numeric_limits<double>::quiet_NaN()));
    BOOST_TEST_EQ(codec::from_int64(123456789012345678), 123456789012345680.0);
}

// A double's text, read as JSON.parse reads it and written as JavaScript writes it.
void binary_text() {
    using codec = wd::number_codec<double>;
    BOOST_TEST_EQ(std::string(codec::write(std::numeric_limits<double>::infinity()).view()),
                  "null");
    BOOST_TEST_EQ(std::string(codec::write(std::numeric_limits<double>::quiet_NaN()).view()),
                  "null");
    BOOST_TEST_EQ(std::string(codec::write(-0.0).view()), "0");
    // Subnormal values, which JSON.parse keeps and JavaScript writes with their shortest digits.
    for (const std::string_view text : {"5e-324", "2.225073858507201e-308"}) {
        auto parsed = codec::parse(text);
        BOOST_TEST(parsed && *parsed != 0.0 &&
                   std::string(codec::write(*parsed).view()) == std::string(text));
    }
    // JSON.parse makes 1e400 Infinity and 1e-400 zero, and the codec does too.
    constexpr double infinity = std::numeric_limits<double>::infinity();
    BOOST_TEST(codec::parse("1e400") == infinity);
    BOOST_TEST(codec::parse("-1e400") == -infinity);
    BOOST_TEST(codec::parse("1e-400") == 0.0);
    BOOST_TEST(codec::parse("123456789e99999999999999999999") == infinity);
    BOOST_TEST(codec::parse("0.000001e-99999999999999999999") == 0.0);
    BOOST_TEST(codec::parse("0e99999") == 0.0);
    BOOST_TEST(codec::parse("1.7976931348623158e308") == std::numeric_limits<double>::max());
    BOOST_TEST(codec::parse("1.7976931348623159e308") == infinity);
    for (const std::string_view bad :
         {"", "-", "+1", "01", "1.", ".5", "1e", "1e+", "inf", "nan", "0x10", "1 ", "--1"}) {
        BOOST_TEST(codec::parse(bad).error() == wd::errc::syntax);
    }
    wd::decimal_digits huge;
    huge.count = 1;
    huge.digits[0] = '1';
    huge.point = 400;
    BOOST_TEST(codec::from_digits(huge) == infinity);
    huge.negative = true;
    BOOST_TEST(codec::from_digits(huge) == -infinity);
    huge.negative = false;
    huge.point = -400;
    BOOST_TEST(codec::from_digits(huge) == 0.0);
}

struct narrow_traits : wd::default_traits {
    using integer = std::int32_t;
};

struct object_int_traits : wd::default_traits {
    using object = int;
};

void traits_concept() {
    static_assert(wd::document_traits<wd::default_traits>);
    static_assert(wd::document_traits<wd::js_traits>);
    static_assert(!wd::document_traits<narrow_traits>);
    static_assert(!wd::document_traits<object_int_traits>);
}

void tags_tell_traits_apart() {
    static_assert(wd::traits_tag<wd::default_traits>() != wd::traits_tag<wd::js_traits>());

    struct sorted_traits : wd::default_traits {
        using object = wd::sorted;
    };

    static_assert(wd::traits_tag<wd::default_traits>() != wd::traits_tag<sorted_traits>());

    struct same_traits : wd::default_traits {};

    static_assert(wd::traits_tag<same_traits>() == wd::traits_tag<wd::default_traits>());
}

}  // namespace

int main() {
    array_indices();
    dates_as_javascript_writes_them();
    base64_vectors();
    utf();
    transcoding();
    codecs();
    scalar_bytes();
    binary_digits();
    binary_text();
    traits_concept();
    tags_tell_traits_apart();
    return boost::report_errors();
}
