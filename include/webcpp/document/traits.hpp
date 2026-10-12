// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The traits of a document: the C++ types it stores its scalars as, how each is stored, read and
 written, the encoding of its strings, how it writes bytes, and the order of an object's keys.
*/

#ifndef WEBCPP_DOCUMENT_TRAITS_HPP
#define WEBCPP_DOCUMENT_TRAITS_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/errors.hpp>
#include <webcpp/document/number.hpp>

#include <boost/charconv/chars_format.hpp>
#include <boost/charconv/from_chars.hpp>
#include <boost/charconv/to_chars.hpp>
#include <boost/decimal/decimal128_t.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace webcpp::document {

/**
 The encoding of strings and keys that stores UTF-8, the default.

 @see "Strings, dates and bytes", in the guide.
*/
struct utf8 {
    /** The encoding's name in the image's layout tag. */
    static constexpr std::string_view name = "utf8";
};

/**
 The encoding of strings and keys that stores UTF-16 code units, as JavaScript holds strings.

 @see "Strings, dates and bytes", in the guide.
*/
struct utf16 {
    /** The encoding's name in the image's layout tag. */
    static constexpr std::string_view name = "utf16";
};

/**
 The bytes policy that writes bytes as base64, RFC 4648 with padding, the default.

 @see "Strings, dates and bytes", in the guide.
*/
struct base64 {
    /** The policy's name in the image's layout tag. */
    static constexpr std::string_view name = "base64";
};

/** The bytes policy that writes bytes as base64url, RFC 4648 section 5, without padding. */
struct base64url {
    /** The policy's name in the image's layout tag. */
    static constexpr std::string_view name = "base64url";
};

/** The bytes policy that writes bytes as `JSON.stringify` writes a Uint8Array: `{"0":1,"1":2}`. */
struct javascript_bytes {
    /** The policy's name in the image's layout tag. */
    static constexpr std::string_view name = "javascript";
};

/**
 The object policy of JavaScript: array-index keys ascending, then the others as created.

 @see "Key order", in the guide.
*/
struct javascript_order {
    /** The policy's name in the image's layout tag. */
    static constexpr std::string_view name = "javascript";
};

/** The object policy that keeps every key in the order it was created, whatever the key. */
struct insertion_order {
    /** The policy's name in the image's layout tag. */
    static constexpr std::string_view name = "insertion";
};

/**
 The object policy that orders keys by their UTF-16 code units, with either encoding, as
 JavaScript's `sort()` and RFC 8785 do, for canonical JSON.
*/
struct sorted {
    /** The policy's name in the image's layout tag. */
    static constexpr std::string_view name = "sorted";
};

/** The array policy: no holes, and writing past the end fills with `null`. */
struct dense {
    /** The policy's name in the image's layout tag. */
    static constexpr std::string_view name = "dense";
};

/** A date as JavaScript's `Date` holds one: milliseconds since the epoch, 1970-01-01T00:00:00Z. */
using date_ms = std::chrono::sys_time<std::chrono::milliseconds>;

namespace detail {

/** Writes the `size` low bytes of `bits` at `out`, least significant first. */
inline void store_little(std::uint64_t bits, std::byte* out, std::size_t size) noexcept {
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>((bits >> (8U * i)) & 0xffU);
    }
}

/** Reads `size` bytes written by @ref store_little. */
inline std::uint64_t load_little(const std::byte* in, std::size_t size) noexcept {
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < size; ++i) {
        bits |= std::to_integer<std::uint64_t>(in[i]) << (8U * i);
    }
    return bits;
}

}  // namespace detail

/**
 How a scalar type is stored in a node: its name in the image's layout tag, its size, and its
 bytes, written the same on every target.

 Each specialisation has `name`, `size`, `store(const T&, std::byte*)` and
 `load(const std::byte*)`; the primary template has none, so a type without one is no
 @ref stored_scalar.

 @tparam T The scalar type.
*/
template <class T>
struct scalar_codec;

/** How a `bool` is stored: one byte, 0 or 1, since `bool`'s size is not fixed by the standard. */
template <>
struct scalar_codec<bool> {
    /** The codec's name in the image's layout tag. */
    static constexpr std::string_view name = "bool";

    /** How many bytes it takes. */
    static constexpr std::size_t size = 1;

    /**
     Writes `v` into `out`.

     @param v The value.
     @param out Where to write its `size` bytes.
    */
    static void store(const bool& v, std::byte* out) noexcept {
        out[0] = v ? std::byte{1} : std::byte{0};
    }

    /**
     Reads a value written by @ref store.

     @param in Its `size` bytes.
     @return The value.
    */
    static bool load(const std::byte* in) noexcept { return in[0] != std::byte{0}; }
};

/** How a `std::int64_t` is stored: eight bytes, least significant first. */
template <>
struct scalar_codec<std::int64_t> {
    /** The codec's name in the image's layout tag. */
    static constexpr std::string_view name = "int64";

    /** How many bytes it takes. */
    static constexpr std::size_t size = 8;

    /**
     Writes `v` into `out`.

     @param v The value.
     @param out Where to write its `size` bytes.
    */
    static void store(const std::int64_t& v, std::byte* out) noexcept {
        detail::store_little(std::bit_cast<std::uint64_t>(v), out, size);
    }

    /**
     Reads a value written by @ref store.

     @param in Its `size` bytes.
     @return The value.
    */
    static std::int64_t load(const std::byte* in) noexcept {
        return std::bit_cast<std::int64_t>(detail::load_little(in, size));
    }
};

/** How a `decimal128_t` is stored: its 16 bytes of IEEE 754 BID, least significant first. */
template <>
struct scalar_codec<boost::decimal::decimal128_t> {
    /** The codec's name in the image's layout tag. */
    static constexpr std::string_view name = "decimal128";

    /** How many bytes it takes. */
    static constexpr std::size_t size = 16;

    /**
     Writes `v` into `out`.

     @param v The value.
     @param out Where to write its `size` bytes.
    */
    static void store(const boost::decimal::decimal128_t& v, std::byte* out) noexcept {
        // The members low and high name the halves on either byte order.
        const auto bits = std::bit_cast<boost::int128::uint128_t>(v);
        detail::store_little(bits.low, out, 8);
        detail::store_little(bits.high, out + 8, 8);
    }

    /**
     Reads a value written by @ref store.

     @param in Its `size` bytes.
     @return The value.
    */
    static boost::decimal::decimal128_t load(const std::byte* in) noexcept {
        boost::int128::uint128_t bits{};
        bits.low = detail::load_little(in, 8);
        bits.high = detail::load_little(in + 8, 8);
        return std::bit_cast<boost::decimal::decimal128_t>(bits);
    }
};

/**
 How a `double` is stored, for the traits @ref js_traits, as its IEEE 754 binary64 bits, least
 significant first.
*/
template <>
struct scalar_codec<double> {
    /** The codec's name in the image's layout tag. */
    static constexpr std::string_view name = "binary64";

    /** How many bytes it takes. */
    static constexpr std::size_t size = 8;

    /**
     Writes `v` into `out`.

     @param v The value.
     @param out Where to write its `size` bytes.
    */
    static void store(const double& v, std::byte* out) noexcept {
        detail::store_little(std::bit_cast<std::uint64_t>(v), out, size);
    }

    /**
     Reads a value written by @ref store.

     @param in Its `size` bytes.
     @return The value.
    */
    static double load(const std::byte* in) noexcept {
        return std::bit_cast<double>(detail::load_little(in, size));
    }
};

/** How a @ref date_ms is stored: its milliseconds as a `std::int64_t`. */
template <>
struct scalar_codec<date_ms> {
    /** The codec's name in the image's layout tag. */
    static constexpr std::string_view name = "date-ms";

    /** How many bytes it takes. */
    static constexpr std::size_t size = 8;

    /**
     Writes `v` into `out`.

     @param v The value.
     @param out Where to write its `size` bytes.
    */
    static void store(const date_ms& v, std::byte* out) noexcept {
        const std::int64_t ms = v.time_since_epoch().count();
        detail::store_little(std::bit_cast<std::uint64_t>(ms), out, size);
    }

    /**
     Reads a value written by @ref store.

     @param in Its `size` bytes.
     @return The value.
    */
    static date_ms load(const std::byte* in) noexcept {
        const auto ms = std::bit_cast<std::int64_t>(detail::load_little(in, size));
        return date_ms(std::chrono::milliseconds(ms));
    }
};

/**
 A type a node can store as a scalar: trivially copyable, with a @ref scalar_codec whose size is
 1, 2, 4, 8 or 16 bytes.

 @tparam T The type.
*/
template <class T>
concept stored_scalar =
    std::is_trivially_copyable_v<T> &&
    requires {
        { scalar_codec<T>::name } -> std::convertible_to<std::string_view>;
        { scalar_codec<T>::size } -> std::convertible_to<std::size_t>;
    } &&
    (scalar_codec<T>::size == 1 || scalar_codec<T>::size == 2 || scalar_codec<T>::size == 4 ||
     scalar_codec<T>::size == 8 || scalar_codec<T>::size == 16);

/**
 How the traits' `number` type is read from JSON text, written as JavaScript writes it, and
 converted; the primary template has none.

 Each specialisation has the static members of `number_codec<boost::decimal::decimal128_t>`.

 @tparam T The number type.
 @see "Numbers", in the guide.
*/
template <class T>
struct number_codec;

/** The number codec of `decimal128_t`, exact: what is stored is what was sent. */
template <>
struct number_codec<boost::decimal::decimal128_t> {
    /** Whether the type stores NaN and the infinities: a decimal refuses them. */
    static constexpr bool stores_non_finite = false;

    /**
     Reads a JSON number's text, exactly.

     @param json_number The text.
     @return The value; @ref errc::range past 34 significant digits or `decimal128_t`'s exponents.
    */
    static result<boost::decimal::decimal128_t> parse(std::string_view json_number) noexcept {
        const result<decimal_digits> scanned = scan_number(json_number);
        if (!scanned) {
            return scanned.error();
        }
        return to_decimal128(scanned.unsafe_value());
    }

    /**
     Writes a value as JavaScript's Number::toString writes its digits.

     @param v The value, finite.
     @return The text.
    */
    static number_text write(const boost::decimal::decimal128_t& v) noexcept {
        return write_javascript(from_decimal128(v));
    }

    /**
     The value as a signed 64-bit integer, when it is one.

     @param v The value.
     @return The integer, or nothing when `v` is not an integer or does not fit.
    */
    static std::optional<std::int64_t> integral(const boost::decimal::decimal128_t& v) noexcept {
        if (!finite(v)) {
            return std::nullopt;
        }
        return to_int64(from_decimal128(v));
    }

    /**
     The value of a signed 64-bit integer, exactly.

     @param v The integer.
     @return The value.
    */
    static boost::decimal::decimal128_t from_int64(std::int64_t v) noexcept {
        // Through the digits, so the value is the member of its cohort that parse gives: the same
        // number stores the same bytes whichever way it came. 19 digits always fit 34.
        return to_decimal128(document::from_int64(v)).unsafe_value();
    }

    /**
     Whether a value is finite.

     @param v The value.
     @return False for NaN and the infinities.
    */
    static bool finite(const boost::decimal::decimal128_t& v) noexcept {
        return boost::decimal::isfinite(v);
    }

    /**
     The digits of a finite value, normalised.

     @param v The value, finite.
     @return Its digits.
    */
    static decimal_digits digits(const boost::decimal::decimal128_t& v) noexcept {
        return from_decimal128(v);
    }

    /**
     The value of a number's digits, exactly.

     @param d The digits.
     @return The value; @ref errc::range when its exponent is outside `decimal128_t`'s range.
    */
    static result<boost::decimal::decimal128_t> from_digits(const decimal_digits& d) noexcept {
        return to_decimal128(d);
    }
};

/**
 The number codec of `double`, for the traits @ref js_traits, with which numbers round as
 JavaScript's do, a number past a double's range is an infinity, and NaN and the infinities are
 written `null`.
*/
template <>
struct number_codec<double> {
    /** Whether the type stores NaN and the infinities: a double does, and writes them `null`. */
    static constexpr bool stores_non_finite = true;

    /**
     Reads a JSON number's text, rounded to the nearest double, as `JSON.parse` does.

     @param json_number The text.
     @return The value, an infinity past a double's range, as `JSON.parse` makes `1e400`
     `Infinity`; @ref errc::syntax for text outside JSON's grammar.
    */
    static result<double> parse(std::string_view json_number) noexcept {
        if (!json_grammar(json_number)) {
            return make_error_code(errc::syntax);
        }
        return read(json_number, at_least_one(json_number));
    }

    /**
     Writes a value as JavaScript's Number::toString does: the shortest digits that read back.

     @param v The value.
     @return The text; `null` for NaN and the infinities.
    */
    static number_text write(const double& v) noexcept {
        if (!finite(v)) {
            number_text text;
            for (const char c : std::string_view("null")) {
                text.chars[text.size++] = c;
            }
            return text;
        }
        return write_javascript(digits(v));
    }

    /**
     The value as a signed 64-bit integer, when it is one.

     @param v The value.
     @return The integer, or nothing when `v` is not an integer or does not fit.
    */
    static std::optional<std::int64_t> integral(const double& v) noexcept {
        // 2^63 is exact as a double, so the range test is exact too; -0 is the integer 0.
        constexpr double limit = 9223372036854775808.0;
        if (!finite(v) || std::trunc(v) != v || v < -limit || v >= limit) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(v);
    }

    /**
     The double nearest a signed 64-bit integer.

     @param v The integer.
     @return The value, rounded as JavaScript rounds it.
    */
    static double from_int64(std::int64_t v) noexcept {
        // The conversion rounds to nearest, ties to even, as JavaScript's Number(BigInt) does.
        return static_cast<double>(v);
    }

    /**
     Whether a value is finite.

     @param v The value.
     @return False for NaN and the infinities.
    */
    static bool finite(const double& v) noexcept { return std::isfinite(v); }

    /**
     The shortest digits that read back as a finite value.

     @param v The value, finite.
     @return Its digits.
    */
    static decimal_digits digits(const double& v) noexcept {
        decimal_digits d;
        if (!finite(v) || v == 0) {
            return d;
        }
        // Without a precision, to_chars writes the shortest digits that read back, as
        // `d.ddde+xx`: the value is 0.dddd times ten to the exponent plus one. It is
        // Boost.Charconv's because some libc++ lacks the standard library's floating-point
        // overloads.
        std::array<char, 32> buffer{};
        const auto written =
            boost::charconv::to_chars(buffer.data(), buffer.data() + buffer.size(), v,
                                      boost::charconv::chars_format::scientific);
        const std::string_view text(buffer.data(),
                                    static_cast<std::size_t>(written.ptr - buffer.data()));
        const std::size_t e = text.find('e');
        std::size_t i = 0;
        if (text[0] == '-') {
            d.negative = true;
            i = 1;
        }
        for (; i < e; ++i) {
            if (text[i] != '.') {
                d.digits[d.count++] = text[i];
            }
        }
        while (d.count > 0 && d.digits[d.count - 1] == '0') {
            --d.count;
        }
        std::string_view exponent = text;
        exponent.remove_prefix(e + 1);
        if (exponent.front() == '+') {
            exponent.remove_prefix(1);
        }
        std::int32_t power = 0;
        std::from_chars(exponent.data(), exponent.data() + exponent.size(), power);
        d.point = power + 1;
        return d;
    }

    /**
     The double nearest a number's digits.

     @param d The digits.
     @return The value, an infinity past a double's range, as JavaScript rounds it; @ref errc::range
     for more digits than `d` holds.
    */
    static result<double> from_digits(const decimal_digits& d) noexcept {
        if (d.count > d.digits.size()) {
            return make_error_code(errc::range);
        }
        if (d.count == 0) {
            return 0.0;
        }
        // The digits as an integer and a power of ten, `-123e-5`, which from_chars rounds.
        std::array<char, 64> buffer{};
        std::size_t size = 0;
        if (d.negative) {
            buffer[size++] = '-';
        }
        for (std::uint32_t i = 0; i < d.count; ++i) {
            buffer[size++] = d.digits[i];
        }
        buffer[size++] = 'e';
        const std::int64_t power = std::int64_t{d.point} - std::int64_t{d.count};
        const auto written =
            std::to_chars(buffer.data() + size, buffer.data() + buffer.size(), power);
        size = static_cast<std::size_t>(written.ptr - buffer.data());
        return read({buffer.data(), size}, d.point > 0);
    }

private:
    /** Whether text follows JSON's grammar of a number,
     * `-?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?`. */
    static bool json_grammar(std::string_view text) noexcept {
        const auto digit = [&](std::size_t i) {
            return i < text.size() && text[i] >= '0' && text[i] <= '9';
        };
        std::size_t i = 0;
        if (i < text.size() && text[i] == '-') {
            ++i;
        }
        if (!digit(i)) {
            return false;
        }
        if (text[i] == '0') {
            ++i;
        } else {
            while (digit(i)) {
                ++i;
            }
        }
        if (i < text.size() && text[i] == '.') {
            ++i;
            if (!digit(i)) {
                return false;
            }
            while (digit(i)) {
                ++i;
            }
        }
        if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
            ++i;
            if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
                ++i;
            }
            if (!digit(i)) {
                return false;
            }
            while (digit(i)) {
                ++i;
            }
        }
        return i == text.size();
    }

    /**
     Whether a number in JSON's grammar that is not zero is at least one in magnitude: whether
     its first significant digit stands at a power of ten that is not negative.
    */
    static bool at_least_one(std::string_view text) noexcept {
        // The power of the first significant digit: the integer part's length less one, or minus
        // the place of the fraction's first digit that is not '0'; then the exponent, saturated,
        // since only its sign beside that power matters.
        std::size_t i = text.front() == '-' ? 1 : 0;
        const std::size_t integer_begin = i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            ++i;
        }
        std::int64_t power = static_cast<std::int64_t>(i - integer_begin) - 1;
        if (text[integer_begin] == '0') {
            power = 0;
            if (i < text.size() && text[i] == '.') {
                ++i;
                while (i < text.size() && text[i] == '0') {
                    ++i;
                    --power;
                }
                --power;
            }
        }
        const std::size_t e = text.find_first_of("eE");
        if (e == std::string_view::npos) {
            return power >= 0;
        }
        std::size_t k = e + 1;
        const bool negative = text[k] == '-';
        if (text[k] == '+' || text[k] == '-') {
            ++k;
        }
        constexpr std::int64_t saturated = 1'000'000'000'000;
        std::int64_t exponent = 0;
        for (; k < text.size(); ++k) {
            exponent = std::min((exponent * 10) + (text[k] - '0'), saturated);
        }
        return power + (negative ? -exponent : exponent) >= 0;
    }

    /**
     Reads text that Boost.Charconv's from_chars accepts whole, rounded to the nearest double as
     JavaScript rounds it; `large` says which way a value out of range lies.
    */
    static result<double> read(std::string_view text, bool large) noexcept {
        double value = 0;
        const auto [end, code] =
            boost::charconv::from_chars(text.data(), text.data() + text.size(), value);
        if (end != text.data() + text.size()) {
            return make_error_code(errc::syntax);
        }
        if (code == std::errc::result_out_of_range) {
            // Some standard libraries report a subnormal value out of range while giving it, which
            // JSON.parse keeps. Otherwise JSON.parse makes a value too small for a double zero,
            // and one too large Infinity, whatever from_chars left in `value`.
            if (finite(value) && value != 0) {
                return value;
            }
            const bool negative = text.front() == '-';
            if (large) {
                constexpr double infinity = std::numeric_limits<double>::infinity();
                return negative ? -infinity : infinity;
            }
            return negative ? -0.0 : 0.0;
        }
        if (code != std::errc{}) {
            return make_error_code(errc::syntax);
        }
        return value;
    }
};

/**
 The default traits: exact numbers, UTF-8 strings, base64 bytes and JavaScript's key order.

 A number is stored by value: an integer that fits `integer` as one, any other as `number`.

 @see "Traits", in the guide.
*/
struct default_traits {
    /** The type of `true` and `false`. */
    using boolean = bool;

    /** The type of a number that is an integer and fits. */
    using integer = std::int64_t;

    /** The type of any other number: exact, 34 significant digits. */
    using number = boost::decimal::decimal128_t;

    /** The encoding strings and keys are stored in. */
    using string = utf8;

    /** The type of a date. */
    using date = date_ms;

    /** How `stringify` writes bytes. */
    using bytes = base64;

    /** The order of an object's keys. */
    using object = javascript_order;

    /** The behaviour of arrays. */
    using array = dense;
};

/**
 The traits of JavaScript itself: doubles, UTF-16 strings, and bytes written as a Uint8Array.

 @see "Traits", in the guide.
*/
struct js_traits : default_traits {
    /** The type of a number that is not an integer that fits: JavaScript's own double. */
    using number = double;

    /** The encoding strings and keys are stored in: UTF-16 code units, as JavaScript holds them. */
    using string = utf16;

    /** How `stringify` writes bytes: as `JSON.stringify` writes a Uint8Array. */
    using bytes = javascript_bytes;
};

/**
 What a document's traits must give: the scalar types, each a @ref stored_scalar, a
 @ref number_codec for `number`, and one of the encodings and policies.

 This version requires `bool`, `std::int64_t` and @ref date_ms for `boolean`, `integer` and `date`;
 `number` is the traits' choice.

 @tparam T The traits.
 @see "Traits", in the guide.
*/
template <class T>
concept document_traits =
    std::same_as<typename T::boolean, bool> && std::same_as<typename T::integer, std::int64_t> &&
    std::same_as<typename T::date, date_ms> && stored_scalar<typename T::number> &&
    requires { number_codec<typename T::number>::stores_non_finite; } &&
    (std::same_as<typename T::string, utf8> || std::same_as<typename T::string, utf16>) &&
    (std::same_as<typename T::bytes, base64> || std::same_as<typename T::bytes, base64url> ||
     std::same_as<typename T::bytes, javascript_bytes>) &&
    (std::same_as<typename T::object, javascript_order> ||
     std::same_as<typename T::object, insertion_order> ||
     std::same_as<typename T::object, sorted>) &&
    std::same_as<typename T::array, dense>;

namespace detail {

/** FNV-1a's 32-bit offset basis. */
inline constexpr std::uint32_t fnv1a_basis = 2166136261U;

/** FNV-1a's 32-bit prime. */
inline constexpr std::uint32_t fnv1a_prime = 16777619U;

/** An FNV-1a hash with one more byte. */
constexpr std::uint32_t fnv1a_byte(std::uint32_t hash, std::uint32_t byte) noexcept {
    return static_cast<std::uint32_t>((hash ^ (byte & 0xffU)) * fnv1a_prime);
}

/** An FNV-1a hash with the bytes of a name, then a zero byte. */
constexpr std::uint32_t fnv1a_name(std::uint32_t hash, std::string_view name) noexcept {
    for (const char c : name) {
        hash = fnv1a_byte(hash, static_cast<unsigned char>(c));
    }
    return fnv1a_byte(hash, 0);
}

/** An FNV-1a hash with a scalar codec's name, its zero byte, and its size as one byte. */
template <class T>
constexpr std::uint32_t fnv1a_scalar(std::uint32_t hash) noexcept {
    return fnv1a_byte(fnv1a_name(hash, scalar_codec<T>::name),
                      static_cast<std::uint32_t>(scalar_codec<T>::size));
}

}  // namespace detail

/**
 The traits' identity in the image's layout tag: an FNV-1a hash, 32 bits, of each scalar codec's
 name and size (`boolean`, `integer`, `number`, `date`), then the names of the `string`, `bytes`,
 `object` and `array` policies, each followed by a zero byte.

 @tparam Traits The traits.
 @return The hash: two traits that store or order differently have different tags.
*/
template <class Traits>
constexpr std::uint32_t traits_tag() noexcept {
    std::uint32_t hash = detail::fnv1a_basis;
    hash = detail::fnv1a_scalar<typename Traits::boolean>(hash);
    hash = detail::fnv1a_scalar<typename Traits::integer>(hash);
    hash = detail::fnv1a_scalar<typename Traits::number>(hash);
    hash = detail::fnv1a_scalar<typename Traits::date>(hash);
    hash = detail::fnv1a_name(hash, Traits::string::name);
    hash = detail::fnv1a_name(hash, Traits::bytes::name);
    hash = detail::fnv1a_name(hash, Traits::object::name);
    return detail::fnv1a_name(hash, Traits::array::name);
}

namespace detail {

/**
 The view of a string or a key in the traits' encoding: `std::string_view` or `std::u16string_view`.
*/
template <class Traits>
using text_view_t = std::conditional_t<std::same_as<typename Traits::string, utf16>,
                                       std::u16string_view, std::string_view>;

/** JavaScript's limit on a date: plus or minus 8.64e15 milliseconds from the epoch. */
inline constexpr std::int64_t date_limit_ms = 8'640'000'000'000'000;

/**
 The array index a key spells: the canonical decimal spelling of an integer from 0 to
 4,294,967,294, so `"7"` but not `"07"`, `"-0"`, `"1.0"` or `"4294967295"`.
*/
inline std::optional<std::uint32_t> array_index(std::string_view key) noexcept {
    if (key.empty() || key.size() > 10 || (key.size() > 1 && key[0] == '0')) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : key) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = (value * 10) + static_cast<std::uint64_t>(c - '0');
    }
    if (value > 4'294'967'294U) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

/** Milliseconds in a day. */
inline constexpr std::int64_t day_ms = 86'400'000;

/** A day of the proleptic Gregorian calendar, with years of any sign. */
struct civil_day {
    /** The year: 0 is 1 BC, -1 is 2 BC. */
    std::int64_t year = 1970;

    /** The month, 1 to 12. */
    std::int64_t month = 1;

    /** The day of the month, 1 to 31. */
    std::int64_t day = 1;
};

/**
 The days from 1970-01-01 to a day, Howard Hinnant's days_from_civil, for any year a 64-bit count
 of milliseconds reaches, which `std::chrono::year` does not.
*/
constexpr std::int64_t days_from_civil(const civil_day& c) noexcept {
    const std::int64_t year = c.month <= 2 ? c.year - 1 : c.year;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const std::int64_t year_of_era = year - (era * 400);
    const std::int64_t day_of_year =
        (((153 * (c.month > 2 ? c.month - 3 : c.month + 9)) + 2) / 5) + c.day - 1;
    const std::int64_t day_of_era =
        (year_of_era * 365) + (year_of_era / 4) - (year_of_era / 100) + day_of_year;
    return (era * 146'097) + day_of_era - 719'468;
}

/** The day that many days after 1970-01-01 is, Howard Hinnant's civil_from_days. */
constexpr civil_day civil_from_days(std::int64_t days) noexcept {
    const std::int64_t z = days + 719'468;
    const std::int64_t era = (z >= 0 ? z : z - 146'096) / 146'097;
    const std::int64_t day_of_era = z - (era * 146'097);
    const std::int64_t year_of_era =
        (day_of_era - (day_of_era / 1460) + (day_of_era / 36'524) - (day_of_era / 146'096)) / 365;
    const std::int64_t day_of_year =
        day_of_era - ((365 * year_of_era) + (year_of_era / 4) - (year_of_era / 100));
    const std::int64_t shifted_month = ((5 * day_of_year) + 2) / 153;
    civil_day c;
    c.day = day_of_year - (((153 * shifted_month) + 2) / 5) + 1;
    c.month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
    c.year = (year_of_era + (era * 400)) + (c.month <= 2 ? 1 : 0);
    return c;
}

/** How many days a month of a year has, February's by the Gregorian leap rule. */
constexpr std::int64_t days_in_month(std::int64_t year, std::int64_t month) noexcept {
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        return leap ? 29 : 28;
    }
    return month == 4 || month == 6 || month == 9 || month == 11 ? 30 : 31;
}

/** Appends `value` to `text` in decimal, with zeros before it up to `width` digits. */
inline void put_digits(number_text& text, std::uint64_t value, std::uint32_t width) noexcept {
    std::array<char, 20> digits{};
    std::uint32_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0);
    for (std::uint32_t i = count; i < width; ++i) {
        text.chars[text.size++] = '0';
    }
    while (count > 0) {
        text.chars[text.size++] = digits[--count];
    }
}

/**
 Date.prototype.toJSON's text of a date, unquoted: `2026-10-12T03:04:05.678Z`, or with a sign and
 six digits for a year outside 0 to 9999.
*/
inline number_text write_date(date_ms t) noexcept {
    const std::int64_t ms = t.time_since_epoch().count();
    // Floor division: -1 ms is the last millisecond of 1969-12-31.
    std::int64_t days = ms / day_ms;
    std::int64_t rest = ms % day_ms;
    if (rest < 0) {
        rest += day_ms;
        --days;
    }
    const civil_day c = civil_from_days(days);
    const auto in_day = static_cast<std::uint64_t>(rest);
    number_text text;
    if (c.year >= 0 && c.year <= 9999) {
        put_digits(text, static_cast<std::uint64_t>(c.year), 4);
    } else {
        text.chars[text.size++] = c.year < 0 ? '-' : '+';
        const std::uint64_t magnitude = c.year < 0 ? 0 - static_cast<std::uint64_t>(c.year)
                                                   : static_cast<std::uint64_t>(c.year);
        put_digits(text, magnitude, 6);
    }
    const auto put = [&text](char separator, std::uint64_t value, std::uint32_t width) {
        text.chars[text.size++] = separator;
        put_digits(text, value, width);
    };
    put('-', static_cast<std::uint64_t>(c.month), 2);
    put('-', static_cast<std::uint64_t>(c.day), 2);
    put('T', in_day / 3'600'000, 2);
    put(':', (in_day / 60'000) % 60, 2);
    put(':', (in_day / 1000) % 60, 2);
    put('.', in_day % 1000, 3);
    text.chars[text.size++] = 'Z';
    return text;
}

/**
 Reads `width` decimal digits of `text` at `at`, then moves `at` past them; nothing when they are
 not all digits.
*/
inline std::optional<std::int64_t> read_digits(std::string_view text, std::size_t& at,
                                               std::size_t width) noexcept {
    if (text.size() - at < width) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        const char c = text[at + i];
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = (value * 10) + (c - '0');
    }
    at += width;
    return value;
}

/** Reads the year of a date's text at its start: four digits, or a sign and six. */
inline std::optional<std::int64_t> read_year(std::string_view text, std::size_t& at) noexcept {
    if (text.empty() || (text[0] != '+' && text[0] != '-')) {
        return read_digits(text, at, 4);
    }
    const bool negative = text[0] == '-';
    ++at;
    const std::optional<std::int64_t> digits = read_digits(text, at, 6);
    // A year from 0 to 9999 is written with four digits, so its six-digit form, -000000 among
    // them, which ECMA-262 forbids, is no text write_date writes.
    if (!digits || (negative ? *digits == 0 : *digits <= 9999)) {
        return std::nullopt;
    }
    return negative ? -*digits : *digits;
}

/**
 Reads the fields of a date's text after its year, `-MM-DDTHH:mm:ss.sssZ`, as milliseconds into
 the year's day; nothing on any other text or a field out of its range.
*/
inline std::optional<std::int64_t> read_day_and_time(std::string_view text, std::size_t at,
                                                     std::int64_t year) noexcept {
    struct field {
        /** The character before the field. */
        char separator;

        /** How many digits the field has; 0 for the closing `Z`, which has none. */
        std::size_t width;

        /** The field's largest value. */
        std::int64_t largest;
    };

    constexpr std::array fields{
        field{.separator = '-', .width = 2, .largest = 12},
        field{.separator = '-', .width = 2, .largest = 31},
        field{.separator = 'T', .width = 2, .largest = 23},
        field{.separator = ':', .width = 2, .largest = 59},
        field{.separator = ':', .width = 2, .largest = 59},
        field{.separator = '.', .width = 3, .largest = 999},
        field{.separator = 'Z', .width = 0, .largest = 0},
    };
    std::array<std::int64_t, 6> values{};
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (at == text.size() || text[at] != fields[i].separator) {
            return std::nullopt;
        }
        ++at;
        if (fields[i].width == 0) {
            break;
        }
        const std::optional<std::int64_t> value = read_digits(text, at, fields[i].width);
        if (!value || *value > fields[i].largest) {
            return std::nullopt;
        }
        values[i] = *value;
    }
    const auto [month, day, hours, minutes, seconds, ms] = values;
    if (at != text.size() || month < 1 || day < 1 || day > days_in_month(year, month)) {
        return std::nullopt;
    }
    const std::int64_t days = days_from_civil({.year = year, .month = month, .day = day});
    return (days * day_ms) + (((((hours * 60) + minutes) * 60) + seconds) * 1000) + ms;
}

/** The date that text in exactly @ref write_date's format names, within @ref date_limit_ms. */
inline std::optional<date_ms> read_date(std::string_view text) noexcept {
    std::size_t at = 0;
    const std::optional<std::int64_t> year = read_year(text, at);
    if (!year) {
        return std::nullopt;
    }
    const std::optional<std::int64_t> ms = read_day_and_time(text, at, *year);
    if (!ms || *ms > date_limit_ms || *ms < -date_limit_ms) {
        return std::nullopt;
    }
    return date_ms(std::chrono::milliseconds(*ms));
}

/** Base64's alphabet, RFC 4648 section 4. */
inline constexpr std::string_view base64_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** Base64url's alphabet, RFC 4648 section 5. */
inline constexpr std::string_view base64url_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/** The six bits a character of the alphabet stands for, or nothing for any other character. */
inline std::optional<std::uint32_t> base64_value(char c, bool url) noexcept {
    const std::size_t at = (url ? base64url_alphabet : base64_alphabet).find(c);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(at);
}

/** The size of the base64 text of `bytes` bytes, with padding or without. */
inline std::uint64_t base64_size(std::uint64_t bytes, bool padded) noexcept {
    const std::uint64_t rest = bytes % 3;
    if (padded || rest == 0) {
        return ((bytes + 2) / 3) * 4;
    }
    return ((bytes / 3) * 4) + rest + 1;
}

/** Writes the base64 text of `in` at `out`: with `url`, base64url's `-_` and no padding. */
inline void write_base64(std::span<const std::byte> in, char* out, bool url) noexcept {
    const std::string_view alphabet = url ? base64url_alphabet : base64_alphabet;
    std::size_t written = 0;
    const auto put = [&](std::uint32_t bits, std::uint32_t shift) {
        out[written++] = alphabet[(bits >> shift) & 0x3fU];
    };
    std::size_t i = 0;
    for (; in.size() - i >= 3; i += 3) {
        const std::uint32_t bits = (std::to_integer<std::uint32_t>(in[i]) << 16U) |
                                   (std::to_integer<std::uint32_t>(in[i + 1]) << 8U) |
                                   std::to_integer<std::uint32_t>(in[i + 2]);
        put(bits, 18);
        put(bits, 12);
        put(bits, 6);
        put(bits, 0);
    }
    const std::size_t rest = in.size() - i;
    if (rest == 0) {
        return;
    }
    std::uint32_t bits = std::to_integer<std::uint32_t>(in[i]) << 16U;
    if (rest == 2) {
        bits |= std::to_integer<std::uint32_t>(in[i + 1]) << 8U;
    }
    put(bits, 18);
    put(bits, 12);
    if (rest == 2) {
        put(bits, 6);
    } else if (!url) {
        out[written++] = '=';
    }
    if (!url) {
        out[written++] = '=';
    }
}

/** Reads base64 text into `out`, which has room for it, and returns the size read. */
inline result<std::uint64_t> read_base64(std::string_view text, std::byte* out, bool url) noexcept {
    // Base64 is padded to a multiple of four characters with at most two '='; base64url has no
    // padding, and a length of 4n + 1 leaves no whole byte.
    std::size_t padding = 0;
    if (!url) {
        if (text.size() % 4 != 0) {
            return make_error_code(errc::syntax);
        }
        while (padding < 2 && padding < text.size() && text[text.size() - 1 - padding] == '=') {
            ++padding;
        }
    } else if (text.size() % 4 == 1) {
        return make_error_code(errc::syntax);
    }
    const std::string_view body(text.data(), text.size() - padding);
    std::uint64_t written = 0;
    std::uint32_t pending = 0;
    std::uint32_t pending_bits = 0;
    for (const char c : body) {
        const std::optional<std::uint32_t> value = base64_value(c, url);
        if (!value) {
            return make_error_code(errc::syntax);
        }
        pending = (pending << 6U) | *value;
        pending_bits += 6;
        if (pending_bits >= 8) {
            pending_bits -= 8;
            out[written++] = static_cast<std::byte>((pending >> pending_bits) & 0xffU);
            pending &= (1U << pending_bits) - 1;
        }
    }
    // The bits of the last character that make no whole byte are zero in canonical text.
    if (pending != 0) {
        return make_error_code(errc::syntax);
    }
    return written;
}

}  // namespace detail

}  // namespace webcpp::document

#endif  // WEBCPP_DOCUMENT_TRAITS_HPP
