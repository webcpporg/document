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

#include <boost/decimal/decimal128_t.hpp>
#include <boost/decimal/decimal32_t.hpp>
#include <boost/decimal/decimal64_t.hpp>
#include <boost/decimal/decimal_fast128_t.hpp>
#include <boost/decimal/decimal_fast32_t.hpp>
#include <boost/decimal/decimal_fast64_t.hpp>

#include <array>
#include <concepts>
#include <cstdint>
#include <optional>
#include <string_view>

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

/**
 Reads a JSON number's text exactly, never through a double.

 The text follows JSON's grammar, `-? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE] [+-]? [0-9]+)?`.
 Leading zeros and trailing zeros are dropped, so that `1.50`, `1.5` and `15e-1` read the same.

 @param json_number The number's text, as a JSON parser delivers it.
 @return Its digits; @ref errc::syntax for text outside the grammar, @ref errc::range for a 35th
 significant digit or an exponent that leaves `std::int32_t`.
*/
inline result<decimal_digits> scan_number(std::string_view json_number) noexcept;

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
*/
inline number_text write_javascript(const decimal_digits& d) noexcept;

/**
 Converts a number to a signed 64-bit integer, when it is one.

 @param d The number.
 @return Its value when it is an integer from `INT64_MIN` to `INT64_MAX`, else nothing.
*/
inline std::optional<std::int64_t> to_int64(const decimal_digits& d) noexcept;

/**
 Converts a number to an unsigned 64-bit integer, when it is one.

 @param d The number.
 @return Its value when it is an integer from 0 to `UINT64_MAX`, else nothing.
*/
inline std::optional<std::uint64_t> to_uint64(const decimal_digits& d) noexcept;

/**
 The digits of a signed 64-bit integer.

 @param v The integer.
 @return Its digits, without trailing zeros: `1000` is `{false, "1", 1, 4}`.
*/
inline decimal_digits from_int64(std::int64_t v) noexcept;

/**
 The digits of an unsigned 64-bit integer.

 @param v The integer.
 @return Its digits, without trailing zeros.
*/
inline decimal_digits from_uint64(std::uint64_t v) noexcept;

/**
 Converts a number to a decimal type, exactly.

 @tparam Decimal The decimal type.
 @param d The number.
 @return The value; @ref errc::range when `Decimal` cannot hold it exactly: more digits than its
 precision, or an exponent outside its range.
*/
template <decimal_type Decimal>
result<Decimal> to_decimal(const decimal_digits& d) noexcept;

/**
 The digits of a finite value of a decimal type, normalised: every member of a cohort, `1.5` and
 `1.50`, has the same digits.

 @tparam Decimal The decimal type.
 @param v The value, which must be finite.
 @return Its digits.
*/
template <decimal_type Decimal>
decimal_digits from_decimal(Decimal v) noexcept;

/**
 Converts a number to `decimal128_t`, exactly, as @ref to_decimal does.

 @param d The number.
 @return The value; @ref errc::range when its exponent is outside `decimal128_t`'s range.
*/
inline result<boost::decimal::decimal128_t> to_decimal128(const decimal_digits& d) noexcept;

/**
 The digits of a finite `decimal128_t`, normalised, as @ref from_decimal gives them.

 @param v The value, which must be finite.
 @return Its digits.
*/
inline decimal_digits from_decimal128(boost::decimal::decimal128_t v) noexcept;

}  // namespace webcpp::document

#endif  // WEBCPP_DOCUMENT_NUMBER_HPP
