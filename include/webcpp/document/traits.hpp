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

#include <boost/decimal/decimal128_t.hpp>

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace webcpp::document {

/** The encoding of strings and keys that stores UTF-8, the default. */
struct utf8 {
    /** The encoding's name in the image's layout tag. */
    static constexpr std::string_view name = "utf8";
};

/** The encoding of strings and keys that stores UTF-16 code units, as JavaScript holds strings. */
struct utf16 {
    /** The encoding's name in the image's layout tag. */
    static constexpr std::string_view name = "utf16";
};

/** The bytes policy that writes bytes as base64, RFC 4648 with padding, the default. */
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

/** The object policy of JavaScript: array-index keys ascending, then the others as created. */
struct javascript_order {
    /** The policy's name in the image's layout tag. */
    static constexpr std::string_view name = "javascript";
};

/** The object policy that keeps every key in the order it was created, whatever the key. */
struct insertion_order {
    /** The policy's name in the image's layout tag. */
    static constexpr std::string_view name = "insertion";
};

/** The object policy that orders keys by their code units, for canonical JSON. */
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
    static void store(const bool& v, std::byte* out) noexcept;

    /**
     Reads a value written by @ref store.

     @param in Its `size` bytes.
     @return The value.
    */
    static bool load(const std::byte* in) noexcept;
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
    static void store(const std::int64_t& v, std::byte* out) noexcept;

    /**
     Reads a value written by @ref store.

     @param in Its `size` bytes.
     @return The value.
    */
    static std::int64_t load(const std::byte* in) noexcept;
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
    static void store(const boost::decimal::decimal128_t& v, std::byte* out) noexcept;

    /**
     Reads a value written by @ref store.

     @param in Its `size` bytes.
     @return The value.
    */
    static boost::decimal::decimal128_t load(const std::byte* in) noexcept;
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
    static void store(const double& v, std::byte* out) noexcept;

    /**
     Reads a value written by @ref store.

     @param in Its `size` bytes.
     @return The value.
    */
    static double load(const std::byte* in) noexcept;
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
    static void store(const date_ms& v, std::byte* out) noexcept;

    /**
     Reads a value written by @ref store.

     @param in Its `size` bytes.
     @return The value.
    */
    static date_ms load(const std::byte* in) noexcept;
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
    static result<boost::decimal::decimal128_t> parse(std::string_view json_number) noexcept;

    /**
     Writes a value as JavaScript's Number::toString writes its digits.

     @param v The value, finite.
     @return The text.
    */
    static number_text write(const boost::decimal::decimal128_t& v) noexcept;

    /**
     The value as a signed 64-bit integer, when it is one.

     @param v The value.
     @return The integer, or nothing when `v` is not an integer or does not fit.
    */
    static std::optional<std::int64_t> integral(const boost::decimal::decimal128_t& v) noexcept;

    /**
     The value of a signed 64-bit integer, exactly.

     @param v The integer.
     @return The value.
    */
    static boost::decimal::decimal128_t from_int64(std::int64_t v) noexcept;

    /**
     Whether a value is finite.

     @param v The value.
     @return False for NaN and the infinities.
    */
    static bool finite(const boost::decimal::decimal128_t& v) noexcept;

    /**
     The digits of a finite value, normalised.

     @param v The value, finite.
     @return Its digits.
    */
    static decimal_digits digits(const boost::decimal::decimal128_t& v) noexcept;

    /**
     The value of a number's digits, exactly.

     @param d The digits.
     @return The value; @ref errc::range when its exponent is outside `decimal128_t`'s range.
    */
    static result<boost::decimal::decimal128_t> from_digits(const decimal_digits& d) noexcept;
};

/**
 The number codec of `double`, for the traits @ref js_traits, with which numbers round as
 JavaScript's do, and NaN and the infinities, which only arithmetic makes, are written `null`.
*/
template <>
struct number_codec<double> {
    /** Whether the type stores NaN and the infinities: a double does, and writes them `null`. */
    static constexpr bool stores_non_finite = true;

    /**
     Reads a JSON number's text, rounded to the nearest double, as `JSON.parse` does.

     @param json_number The text.
     @return The value; @ref errc::range past a double's range, which `JSON.parse` makes
     `Infinity`.
    */
    static result<double> parse(std::string_view json_number) noexcept;

    /**
     Writes a value as JavaScript's Number::toString does: the shortest digits that read back.

     @param v The value.
     @return The text; `null` for NaN and the infinities.
    */
    static number_text write(const double& v) noexcept;

    /**
     The value as a signed 64-bit integer, when it is one.

     @param v The value.
     @return The integer, or nothing when `v` is not an integer or does not fit.
    */
    static std::optional<std::int64_t> integral(const double& v) noexcept;

    /**
     The double nearest a signed 64-bit integer.

     @param v The integer.
     @return The value, rounded as JavaScript rounds it.
    */
    static double from_int64(std::int64_t v) noexcept;

    /**
     Whether a value is finite.

     @param v The value.
     @return False for NaN and the infinities.
    */
    static bool finite(const double& v) noexcept;

    /**
     The shortest digits that read back as a finite value.

     @param v The value, finite.
     @return Its digits.
    */
    static decimal_digits digits(const double& v) noexcept;

    /**
     The double nearest a number's digits.

     @param d The digits.
     @return The value; @ref errc::range past a double's range.
    */
    static result<double> from_digits(const decimal_digits& d) noexcept;
};

/**
 The default traits: exact numbers, UTF-8 strings, base64 bytes and JavaScript's key order.

 A number is stored by value: an integer that fits `integer` as one, any other as `number`.
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

/** The traits of JavaScript itself: doubles, UTF-16 strings, and bytes written as a Uint8Array. */
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

/**
 The traits' identity in the image's layout tag: an FNV-1a hash, 32 bits, of each scalar codec's
 name and size (`boolean`, `integer`, `number`, `date`), then the names of the `string`, `bytes`,
 `object` and `array` policies, each followed by a zero byte.

 @tparam Traits The traits.
 @return The hash: two traits that store or order differently have different tags.
*/
template <class Traits>
constexpr std::uint32_t traits_tag() noexcept;

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
inline std::optional<std::uint32_t> array_index(std::string_view key) noexcept;

/**
 Date.prototype.toJSON's text of a date, unquoted: `2026-10-12T03:04:05.678Z`, or with a sign and
 six digits for a year outside 0 to 9999.
*/
inline number_text write_date(date_ms t) noexcept;

/** The date that text in exactly @ref write_date's format names, within @ref date_limit_ms. */
inline std::optional<date_ms> read_date(std::string_view text) noexcept;

/** The size of the base64 text of `bytes` bytes, with padding or without. */
inline std::uint64_t base64_size(std::uint64_t bytes, bool padded) noexcept;

/** Writes the base64 text of `in` at `out`: with `url`, base64url's `-_` and no padding. */
inline void write_base64(std::span<const std::byte> in, char* out, bool url) noexcept;

/** Reads base64 text into `out`, which has room for it, and returns the size read. */
inline result<std::uint64_t> read_base64(std::string_view text, std::byte* out, bool url) noexcept;

}  // namespace detail

}  // namespace webcpp::document

#endif  // WEBCPP_DOCUMENT_TRAITS_HPP
