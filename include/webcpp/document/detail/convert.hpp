// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The one place where C++ values become the tree's items, and the tree's nodes become C++ values:
 what `set`, the array methods and the parser store, and what `get` reads.

 Numbers are stored by value. A number is first what the traits' `number` makes of it: exact with
 `decimal128_t`, rounded as JavaScript rounds with `double`. Then it is stored as the traits'
 `integer` when it is an integer that fits, else as their `number`: `1`, `1.0` and `1e0` are one
 stored value, and `18446744073709551615` is an exact `number` by default.
*/

#ifndef WEBCPP_DOCUMENT_DETAIL_CONVERT_HPP
#define WEBCPP_DOCUMENT_DETAIL_CONVERT_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/tree.hpp>
#include <webcpp/document/detail/utf.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/kind.hpp>
#include <webcpp/document/literal.hpp>
#include <webcpp/document/number.hpp>
#include <webcpp/document/traits.hpp>

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace webcpp::document::detail {

/** An integer type that is a number: any but `bool` and the character types. */
template <class T>
concept number_integer = std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char> &&
                         !std::same_as<T, char8_t> && !std::same_as<T, char16_t> &&
                         !std::same_as<T, char32_t> && !std::same_as<T, wchar_t>;

/** A floating-point type, which a document converts only when its traits' `number` is one. */
template <class Traits, class T>
concept binary_number = std::floating_point<T> && std::floating_point<typename Traits::number>;

/**
 What @ref prepare turns into an item: the traits' scalars, every integer type, the decimal types,
 a floating-point type into binary traits, text, bytes, a date, `null` and the literals.
*/
template <class Traits, class T>
concept preparable =
    std::same_as<T, bool> || number_integer<T> || decimal_type<T> || binary_number<Traits, T> ||
    std::same_as<T, typename Traits::number> || std::convertible_to<T, std::string_view> ||
    std::convertible_to<T, std::u16string_view> ||
    std::convertible_to<T, std::span<const std::byte>> || std::same_as<T, date_ms> ||
    std::same_as<T, std::nullptr_t> || std::same_as<T, null_t> || std::same_as<T, literal> ||
    std::same_as<T, object> || std::same_as<T, array>;

/**
 What @ref read gives a node as: the traits' scalars, every integer type, the decimal types, a
 floating-point type from binary traits, text, a date and bytes.
*/
template <class Traits, class T>
concept readable =
    std::same_as<T, bool> || number_integer<T> || decimal_type<T> || binary_number<Traits, T> ||
    std::same_as<T, typename Traits::number> || std::same_as<T, std::string> ||
    std::same_as<T, std::string_view> || std::same_as<T, std::u16string> ||
    std::same_as<T, std::u16string_view> || std::same_as<T, date_ms> ||
    std::same_as<T, std::vector<std::byte>> || std::same_as<T, std::span<const std::byte>>;

/**
 A value made ready for the tree: its @ref item and what the item points at, text transcoded to the
 traits' encoding and a literal's entries, owned here until the tree has copied them.
*/
template <class Traits>
class prepared {
public:
    /** A value holding `null`. */
    prepared() noexcept = default;

    /** A prepared value is not copied: its item points into it. */
    prepared(const prepared&) = delete;

    /** A prepared value is not copied: its item points into it. */
    prepared& operator=(const prepared&) = delete;

    /** Moves a prepared value, its item pointing into its new place. */
    prepared(prepared&& other) noexcept;

    /** Moves a prepared value, its item pointing into its new place. */
    prepared& operator=(prepared&& other) noexcept;

    /** Destroys what the item points at. */
    ~prepared() = default;

    /** The item, valid while this object lives and is not changed. */
    [[nodiscard]] const item<Traits>& value() const noexcept;

    /**
     Makes the item a deep copy of the node `id` of `from`, which may be the tree it is stored in.
    */
    void copy_of(const tree<Traits>& from, std::uint64_t id) noexcept;

private:
    item<Traits> item_;
    std::vector<std::byte> text_;
    std::vector<std::vector<literal_entry<Traits>>> entries_;
    std::vector<std::string> keys_;
};

/**
 Turns a C++ value into an item: @ref errc::type for text invalid in its encoding or a
 floating-point value into traits whose `number` is not binary, @ref errc::range for a non-finite
 decimal or a date out of JavaScript's range.
*/
template <class T, class Traits>
    requires preparable<Traits, T>
result<void> prepare(const T& v, prepared<Traits>& out) noexcept;

/**
 Turns a JSON number's text into an item, by value: @ref errc::range when the traits' `number`
 refuses it.
*/
template <class Traits>
result<void> prepare_number(std::string_view json_number, prepared<Traits>& out) noexcept;

/**
 Turns a JSON string, in valid UTF-8, into an item: a string, or with `revive_dates` a date when it
 is exactly Date.prototype.toJSON's text, or with `revive_bytes` bytes when it is text of the
 traits' base64 policy.
*/
template <class Traits>
result<void> prepare_string(std::string_view utf8, bool revive_dates, bool revive_bytes,
                            prepared<Traits>& out) noexcept;

/** The item of an empty container, `null`, or for a root @ref kind::undefined. */
template <class Traits>
item<Traits> empty_item(webcpp::document::kind k) noexcept;

/**
 Reads a node as `T`: @ref errc::type for a null node (undefined) or another kind, @ref errc::range
 for a number that is not an integer, does not fit `T`, or that `T` cannot hold exactly.

 A string asked as @ref date_ms is read when it is exactly Date.prototype.toJSON's text, and as
 bytes when it is text of the traits' base64 policy; an object asked as bytes under
 `javascript_bytes` is read when its keys are exactly `"0"` to `"n-1"` with integers 0 to 255.
 `std::string_view` is read from `utf8` strings only and `std::u16string_view` from `utf16` ones.
*/
template <class T, class Traits>
    requires readable<Traits, T>
result<T> read(const tree<Traits>& t, const node<Traits>* n) noexcept;

/**
 Whether two subtrees are equal, structurally: the same kind; numbers by value; strings, dates and
 bytes by value; arrays element by element; objects with the same keys, each with an equal value,
 in any order. A null node is undefined, equal only to another.
*/
template <class Traits>
bool equal(const tree<Traits>& a, const node<Traits>* x, const tree<Traits>& b,
           const node<Traits>* y) noexcept;

}  // namespace webcpp::document::detail

#endif  // WEBCPP_DOCUMENT_DETAIL_CONVERT_HPP
