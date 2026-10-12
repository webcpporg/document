// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 Numbers as JSON writes them and as JavaScript prints them: a finite number's decimal digits, read
 exactly from JSON text, written as ECMA-262's Number::toString writes them, and converted to and
 from the integers and the decimal types a document stores.
*/

#ifndef WEBCPP_DOCUMENT_NUMBER_HPP
#define WEBCPP_DOCUMENT_NUMBER_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/errors.hpp>

#include <boost/decimal/cmath.hpp>
#include <boost/decimal/decimal128_t.hpp>
#include <boost/decimal/decimal32_t.hpp>
#include <boost/decimal/decimal64_t.hpp>
#include <boost/decimal/decimal_fast128_t.hpp>
#include <boost/decimal/decimal_fast32_t.hpp>
#include <boost/decimal/decimal_fast64_t.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace webcpp::document {

/**
 A finite decimal number as JavaScript's Number::toString sees it: its value is
 `0.d1 d2 ... dk` times ten to the power `point`, with `k` the `count` of its significant digits.

 Zero has no digits: `count` is 0, `point` 0 and `negative` false. Any other value has between 1
 and 34 digits, the first and the last of them not `'0'`, so that every number has exactly one
 `decimal_digits`: `1.5`, `1.50` and `15e-1` are all `{false, "15", 2, 1}`.
*/
struct decimal_digits {
    /** Whether the number is below zero; never true for zero. */
    bool negative = false;

    /** The significant digits, `'0'` to `'9'`, in the first `count` places. */
    std::array<char, 34> digits{};

    /** How many significant digits there are, `k`; 0 for zero. */
    std::uint32_t count = 0;

    /** Where the decimal point is, `n`: after this many digits, which may be negative. */
    std::int32_t point = 0;
};

/**
 Text of at most 64 characters, held by value, so that writing a number or a date allocates
 nothing.
*/
struct number_text {
    /** The characters, in the first `size` places. */
    std::array<char, 64> chars{};

    /** How many characters there are. */
    std::uint32_t size = 0;

    /**
     The text, valid while this object lives.

     @return A view of the first `size` characters.
    */
    [[nodiscard]] std::string_view view() const noexcept { return {chars.data(), size}; }
};

/**
 The decimal types of Boost.Decimal a document converts to and from: `decimal32_t`, `decimal64_t`,
 `decimal128_t` and their `fast` variants.

 @tparam T The type.
*/
template <class T>
concept decimal_type =
    std::same_as<T, boost::decimal::decimal32_t> || std::same_as<T, boost::decimal::decimal64_t> ||
    std::same_as<T, boost::decimal::decimal128_t> ||
    std::same_as<T, boost::decimal::decimal_fast32_t> ||
    std::same_as<T, boost::decimal::decimal_fast64_t> ||
    std::same_as<T, boost::decimal::decimal_fast128_t>;

namespace detail {

/** The most significant digits a @ref decimal_digits holds, decimal128's precision. */
inline constexpr std::uint32_t max_significant_digits = 34;

/** The significant digits of a number's text, gathered one written digit at a time. */
struct digit_reader {
    /** The digits gathered so far. */
    decimal_digits out;

    /** Zeros read since the last digit that is not `'0'`, stored only if another such digit
     * follows. */
    std::size_t pending_zeros = 0;

    /** Where the point falls, counted from the first significant digit, so far. */
    std::int64_t point = 0;

    /** Whether a digit that is not `'0'` has been read. */
    bool started = false;

    /** Whether a 35th significant digit was read. */
    bool too_many = false;

    /** Takes the next digit of the integer part, or of the fraction when `fraction` holds. */
    void take(char c, bool fraction) noexcept {
        if (!started) {
            // A leading zero is no digit; in the fraction it moves the point.
            if (c == '0') {
                if (fraction) {
                    --point;
                }
                return;
            }
            started = true;
        }
        if (!fraction) {
            ++point;
        }
        if (c == '0') {
            ++pending_zeros;
            return;
        }
        if (out.count + pending_zeros >= max_significant_digits) {
            too_many = true;
            return;
        }
        for (; pending_zeros > 0; --pending_zeros) {
            out.digits[out.count++] = '0';
        }
        out.digits[out.count++] = c;
    }
};

/** Whether `text` holds a decimal digit at `at`. */
inline bool digit_at(std::string_view text, std::size_t at) noexcept {
    return at < text.size() && text[at] >= '0' && text[at] <= '9';
}

/** A number's text read from left to right, its grammar checked and its parts gathered. */
struct number_scanner {
    /** The text. */
    std::string_view text;

    /** Where the next character is. */
    std::size_t at = 0;

    /** The significand's digits. */
    digit_reader reader;

    /** The exponent written, its magnitude saturated at 2^32, past every `std::int32_t`. */
    std::int64_t exponent = 0;

    /** Consumes the next character when it is `c`, and says whether it was. */
    bool consume(char c) noexcept {
        if (at < text.size() && text[at] == c) {
            ++at;
            return true;
        }
        return false;
    }

    /** Reads `-? (0 | [1-9][0-9]*) (. [0-9]+)?`, false for text outside it. */
    bool significand() noexcept {
        reader.out.negative = consume('-');
        if (!digit_at(text, at)) {
            return false;
        }
        if (!consume('0')) {
            for (; digit_at(text, at); ++at) {
                reader.take(text[at], false);
            }
        }
        if (!consume('.')) {
            return true;
        }
        if (!digit_at(text, at)) {
            return false;
        }
        for (; digit_at(text, at); ++at) {
            reader.take(text[at], true);
        }
        return true;
    }

    /** Reads `([eE] [+-]? [0-9]+)?`, false for text outside it. */
    bool exponent_part() noexcept {
        if (!consume('e') && !consume('E')) {
            return true;
        }
        const bool negative = consume('-');
        if (!negative) {
            consume('+');
        }
        if (!digit_at(text, at)) {
            return false;
        }
        constexpr std::int64_t saturated = 4'294'967'296;
        for (; digit_at(text, at); ++at) {
            exponent = std::min(saturated, (exponent * 10) + (text[at] - '0'));
        }
        exponent = negative ? -exponent : exponent;
        return true;
    }
};

/** The digits of `coefficient` times ten to the power `exponent`, without trailing zeros. */
template <class Unsigned>
decimal_digits from_coefficient(Unsigned coefficient, std::int64_t exponent,
                                bool negative) noexcept {
    decimal_digits out;
    if (coefficient == 0U) {
        return out;
    }
    // 39 digits hold any 128-bit coefficient; a decimal type's has at most 34.
    std::array<char, 40> reversed{};
    std::size_t total = 0;
    for (; coefficient != 0U && total < reversed.size(); coefficient /= 10U) {
        reversed[total++] = static_cast<char>('0' + static_cast<std::uint32_t>(coefficient % 10U));
    }
    std::size_t trailing = 0;
    while (reversed[trailing] == '0') {
        ++trailing;
    }
    for (std::size_t i = total; i > trailing && out.count < max_significant_digits; --i) {
        out.digits[out.count++] = reversed[i - 1];
    }
    out.negative = negative;
    out.point = static_cast<std::int32_t>(static_cast<std::int64_t>(total) + exponent);
    return out;
}

/** The magnitude of a number when it is an integer of at most `limit`, else nothing. */
inline std::optional<std::uint64_t> magnitude(const decimal_digits& d,
                                              std::uint64_t limit) noexcept {
    if (d.count == 0) {
        return 0U;
    }
    // A fraction, or more digits than UINT64_MAX's 20.
    if (std::cmp_less(d.point, d.count) || d.point > 20) {
        return std::nullopt;
    }
    std::uint64_t v = 0;
    for (std::int32_t i = 0; i < d.point; ++i) {
        const std::uint64_t digit =
            std::cmp_less(i, d.count)
                ? static_cast<std::uint64_t>(d.digits[static_cast<std::size_t>(i)] - '0')
                : std::uint64_t{0};
        if (v > (limit - digit) / 10U) {
            return std::nullopt;
        }
        v = (v * 10U) + digit;
    }
    return v;
}

/** Whether two numbers have the same sign, digits and point. */
inline bool same_number(const decimal_digits& a, const decimal_digits& b) noexcept {
    if (a.negative != b.negative || a.count != b.count || a.point != b.point) {
        return false;
    }
    for (std::uint32_t i = 0; i < a.count; ++i) {
        if (a.digits[i] != b.digits[i]) {
            return false;
        }
    }
    return true;
}

/** The normalised digits of a decimal type's value, zero for a value that is not finite. */
template <decimal_type Decimal>
decimal_digits digits_of_decimal(Decimal v) noexcept {
    if (!boost::decimal::isfinite(v)) {
        return {};
    }
    int exponent = 0;  // frexp10's out parameter is an int.
    const auto coefficient = boost::decimal::frexp10(v, &exponent);
    return from_coefficient(coefficient, exponent, boost::decimal::signbit(v));
}

}  // namespace detail

/**
 Reads a JSON number's text exactly, never through a double.

 The text follows JSON's grammar, `-? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE] [+-]? [0-9]+)?`.
 Leading zeros and trailing zeros are dropped, so that `1.50`, `1.5` and `15e-1` read the same.

 @param json_number The number's text, as a JSON parser delivers it.
 @return Its digits; @ref errc::syntax for text outside the grammar, @ref errc::range for a 35th
 significant digit, or for a number other than zero whose exponent or point leaves
 `std::int32_t`. Zero is zero whatever its exponent: `0e2147483648` reads as 0.
*/
inline result<decimal_digits> scan_number(std::string_view json_number) noexcept {
    detail::number_scanner scanner;
    scanner.text = json_number;
    if (!scanner.significand() || !scanner.exponent_part() || scanner.at != json_number.size()) {
        return errc::syntax;
    }
    constexpr std::int64_t lowest = std::numeric_limits<std::int32_t>::min();
    constexpr std::int64_t highest = std::numeric_limits<std::int32_t>::max();
    if (scanner.reader.too_many) {
        return errc::range;
    }
    // Zero is zero whatever its exponent, as JSON.parse reads it.
    if (scanner.reader.out.count == 0) {
        return decimal_digits{};
    }
    if (scanner.exponent < lowest || scanner.exponent > highest) {
        return errc::range;
    }
    const std::int64_t point = scanner.reader.point + scanner.exponent;
    if (point < lowest || point > highest) {
        return errc::range;
    }
    decimal_digits out = scanner.reader.out;
    out.point = static_cast<std::int32_t>(point);
    return out;
}

/**
 Writes a number as JavaScript's Number::toString writes it, ECMA-262, radix 10.

 With `k` significant digits and the decimal point after `n` of them:

 | Condition | Written as | Example |
 | --- | --- | --- |
 | `k <= n <= 21` | the digits, then `n - k` zeros | `100000000000000000000` |
 | `0 < n <= 21` | the digits with a point after `n` | `123.45` |
 | `-6 < n <= 0` | `0.`, `-n` zeros, the digits | `0.000001` |
 | otherwise | one digit, `.` and the rest if any, `e`, the sign, `n - 1` | `1.5e+300` |

 @param d The number.
 @return Its text, which never holds more than 64 characters.
 @see "Numbers", in the guide.
*/
inline number_text write_javascript(const decimal_digits& d) noexcept {
    number_text t;
    if (d.count == 0) {
        t.chars[t.size++] = '0';
        return t;
    }
    // A count past the array would read beyond it: the digits held are all there are.
    const std::int64_t k = std::min(d.count, detail::max_significant_digits);
    const std::int64_t n = d.point;
    const auto put = [&t](char c) { t.chars[t.size++] = c; };
    // The digits from place `from` to place `to`, a zero for each place past the last digit.
    const auto put_digits = [&](std::int64_t from, std::int64_t to) {
        for (std::int64_t i = from; i < to; ++i) {
            put(i < k ? d.digits[static_cast<std::size_t>(i)] : '0');
        }
    };
    if (d.negative) {
        put('-');
    }
    if (k <= n && n <= 21) {
        put_digits(0, n);
    } else if (0 < n && n <= 21) {
        put_digits(0, n);
        put('.');
        put_digits(n, k);
    } else if (-6 < n && n <= 0) {
        put('0');
        put('.');
        for (std::int64_t i = n; i < 0; ++i) {
            put('0');
        }
        put_digits(0, k);
    } else {
        put_digits(0, 1);
        if (k > 1) {
            put('.');
            put_digits(1, k);
        }
        put('e');
        const std::int64_t e = n - 1;
        put(e < 0 ? '-' : '+');
        std::array<char, 24> buffer{};
        const std::to_chars_result written =
            std::to_chars(buffer.data(), buffer.data() + buffer.size(), e < 0 ? -e : e);
        for (const char* p = buffer.data(); p != written.ptr; ++p) {
            put(*p);
        }
    }
    return t;
}

/**
 Converts a number to a signed 64-bit integer, when it is one.

 @param d The number.
 @return Its value when it is an integer from `INT64_MIN` to `INT64_MAX`, else nothing.
*/
inline std::optional<std::int64_t> to_int64(const decimal_digits& d) noexcept {
    constexpr auto highest = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const std::optional<std::uint64_t> m = detail::magnitude(d, d.negative ? highest + 1 : highest);
    if (!m) {
        return std::nullopt;
    }
    if (!d.negative) {
        return static_cast<std::int64_t>(*m);
    }
    // 2^63 has no std::int64_t to negate.
    return *m > highest ? std::numeric_limits<std::int64_t>::min() : -static_cast<std::int64_t>(*m);
}

/**
 Converts a number to an unsigned 64-bit integer, when it is one.

 @param d The number.
 @return Its value when it is an integer from 0 to `UINT64_MAX`, else nothing.
*/
inline std::optional<std::uint64_t> to_uint64(const decimal_digits& d) noexcept {
    if (d.negative && d.count != 0) {
        return std::nullopt;
    }
    return detail::magnitude(d, std::numeric_limits<std::uint64_t>::max());
}

/**
 The digits of a signed 64-bit integer.

 @param v The integer.
 @return Its digits, without trailing zeros: `1000` is `{false, "1", 1, 4}`.
*/
inline decimal_digits from_int64(std::int64_t v) noexcept {
    // Negated as unsigned, where INT64_MIN's magnitude does not overflow.
    const auto bits = static_cast<std::uint64_t>(v);
    return detail::from_coefficient(v < 0 ? std::uint64_t{0} - bits : bits, 0, v < 0);
}

/**
 The digits of an unsigned 64-bit integer.

 @param v The integer.
 @return Its digits, without trailing zeros.
*/
inline decimal_digits from_uint64(std::uint64_t v) noexcept {
    return detail::from_coefficient(v, 0, false);
}

/**
 Converts a number to a decimal type, exactly.

 @tparam Decimal The decimal type.
 @param d The number.
 @return The value; @ref errc::range when `Decimal` cannot hold it exactly: more digits than its
 precision, or an exponent outside its range.
*/
template <decimal_type Decimal>
result<Decimal> to_decimal(const decimal_digits& d) noexcept {
    using limits = std::numeric_limits<Decimal>;
    // Bounds a little wider than the type's, which keep the exponent below from overflowing
    // inside Boost.Decimal; the round trip at the end decides exactly.
    if (d.count > static_cast<std::uint32_t>(limits::digits) ||
        d.point > limits::max_exponent10 + 1 || d.point < limits::min_exponent10 - limits::digits) {
        return errc::range;
    }
    typename Decimal::significand_type coefficient = 0U;
    for (std::uint32_t i = 0; i < d.count; ++i) {
        coefficient = (coefficient * 10U) + static_cast<std::uint32_t>(d.digits[i] - '0');
    }
    const std::int32_t exponent = d.point - static_cast<std::int32_t>(d.count);
    const Decimal value(coefficient, exponent, d.negative);
    // Rounded into a subnormal, flushed to zero or carried to infinity: not exact.
    if (!detail::same_number(detail::digits_of_decimal(value), d)) {
        return errc::range;
    }
    return value;
}

/**
 The digits of a finite value of a decimal type, normalised: every member of a cohort, `1.5` and
 `1.50`, has the same digits.

 @tparam Decimal The decimal type.
 @param v The value, which must be finite.
 @return Its digits.
*/
template <decimal_type Decimal>
decimal_digits from_decimal(Decimal v) noexcept {
    return detail::digits_of_decimal(v);
}

/**
 Converts a number to `decimal128_t`, exactly, as @ref to_decimal does.

 @param d The number.
 @return The value; @ref errc::range when its exponent is outside `decimal128_t`'s range.
*/
inline result<boost::decimal::decimal128_t> to_decimal128(const decimal_digits& d) noexcept {
    return to_decimal<boost::decimal::decimal128_t>(d);
}

/**
 The digits of a finite `decimal128_t`, normalised, as @ref from_decimal gives them.

 @param v The value, which must be finite.
 @return Its digits.
*/
inline decimal_digits from_decimal128(boost::decimal::decimal128_t v) noexcept {
    return from_decimal(v);
}

}  // namespace webcpp::document

#endif  // WEBCPP_DOCUMENT_NUMBER_HPP
