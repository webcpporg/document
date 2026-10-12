// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 Literals: values written in C++ as JavaScript writes `{...}` and `[...]`, plain C++ values that a
 document converts, by its traits, when it stores them.
*/

#ifndef WEBCPP_DOCUMENT_LITERAL_HPP
#define WEBCPP_DOCUMENT_LITERAL_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/fwd.hpp>
#include <webcpp/document/kind.hpp>
#include <webcpp/document/number.hpp>
#include <webcpp/document/traits.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace webcpp::document {

namespace detail {

/** The C++ value a @ref literal holds. */
enum class literal_form : std::uint8_t {
    /** `nullptr` or @ref null. */
    null,

    /** A `bool`. */
    boolean,

    /** A signed integer, held as `std::int64_t`. */
    int64,

    /** An unsigned integer, held as `std::uint64_t`. */
    uint64,

    /** A finite value of a decimal type, held as its digits. */
    decimal,

    /** A NaN or an infinity of a decimal type, which no document stores. */
    non_finite,

    /** A binary64 floating-point value, held as its bits, which only a binary `number` stores. */
    binary64,

    /** A string in UTF-8. */
    string,

    /** A string in UTF-16. */
    u16string,

    /** A date. */
    date,

    /** Bytes. */
    bytes,

    /** An object. */
    object,

    /** An array. */
    array,
};

/**
 A binary64 floating-point value as its bits, which a @ref literal holds without naming `double`.
*/
struct binary64 {
    /** The IEEE 754 bits. */
    std::uint64_t bits = 0;
};

/** A NaN or an infinity of a decimal type. */
struct non_finite_decimal {};

}  // namespace detail

/**
 One value of a literal, converted implicitly from the C++ value it is written as.

 It holds `bool`, any integer (character types aside), the decimal types, a binary64
 floating-point value (stored only by traits whose `number` is binary, such as @ref js_traits),
 text in UTF-8 or UTF-16, a @ref date_ms, bytes, `nullptr` or @ref null, and an @ref object or an
 @ref array. Text and bytes are copied, so a literal owns what it holds.
*/
class literal {
public:
    // A literal converts implicitly from each value it holds, as JavaScript's `[1, "x"]` needs no
    // conversion written: `wd::array{1, "x"}`. Hence each constructor's exemption from
    // misc-explicit-constructor.

    /**
     A boolean, constrained so that a pointer does not convert to it.

     @tparam B `bool`.
     @param v The value.
    */
    template <std::same_as<bool> B>
    literal(B v) noexcept;  // NOLINT(misc-explicit-constructor)

    /**
     An integer, of any width and signedness, character types aside.

     @tparam I The integer type.
     @param v The value.
    */
    template <std::integral I>
        requires(!std::same_as<I, bool> && !std::same_as<I, char> && !std::same_as<I, char8_t> &&
                 !std::same_as<I, char16_t> && !std::same_as<I, char32_t> &&
                 !std::same_as<I, wchar_t>)
    literal(I v) noexcept;  // NOLINT(misc-explicit-constructor)

    /**
     A value of a decimal type: finite, or held as one no document stores.

     @tparam D The decimal type.
     @param v The value.
    */
    template <decimal_type D>
    literal(D v) noexcept;  // NOLINT(misc-explicit-constructor)

    /**
     A binary64 floating-point value, which only traits whose `number` is binary store.

     @tparam F The floating-point type, of 8 bytes.
     @param v The value.
    */
    template <std::floating_point F>
        requires(sizeof(F) == 8)
    literal(F v) noexcept;  // NOLINT(misc-explicit-constructor)

    /**
     Text in UTF-8, copied.

     @param v The text, terminated by a zero.
    */
    literal(const char* v);  // NOLINT(misc-explicit-constructor)

    /**
     Text in UTF-8, copied.

     @param v The text.
    */
    literal(std::string_view v);  // NOLINT(misc-explicit-constructor)

    /**
     Text in UTF-8, copied.

     @param v The text.
    */
    literal(const std::string& v);  // NOLINT(misc-explicit-constructor)

    /**
     Text in UTF-16, copied.

     @param v The text.
    */
    literal(std::u16string_view v);  // NOLINT(misc-explicit-constructor)

    /**
     A date.

     @param v The date.
    */
    literal(date_ms v) noexcept;  // NOLINT(misc-explicit-constructor)

    /**
     Bytes, copied.

     @param v The bytes.
    */
    literal(std::span<const std::byte> v);  // NOLINT(misc-explicit-constructor)

    /**
     JavaScript's `null`.

     @param v `nullptr`.
    */
    literal(std::nullptr_t v) noexcept;  // NOLINT(misc-explicit-constructor)

    /**
     JavaScript's `null`.

     @param v @ref null.
    */
    literal(null_t v) noexcept;  // NOLINT(misc-explicit-constructor)

    /**
     An object.

     @param v The object.
    */
    literal(object v);  // NOLINT(misc-explicit-constructor)

    /**
     An array.

     @param v The array.
    */
    literal(array v);  // NOLINT(misc-explicit-constructor)

    /**
     Calls `f` with the value this literal holds, as one of: @ref null_t, `bool`, `std::int64_t`,
     `std::uint64_t`, `const decimal_digits&` (a finite decimal), `detail::non_finite_decimal`,
     `detail::binary64`, `std::string_view`, `std::u16string_view`, @ref date_ms,
     `std::span<const std::byte>`, `const object&` or `const array&`.

     @tparam F The callable, invocable with each of those.
     @param f The callable.
     @return What `f` returns.
    */
    template <class F>
    decltype(auto) visit(F&& f) const;

private:
    detail::literal_form form_ = detail::literal_form::null;
    std::uint64_t scalar_ = 0;
    decimal_digits digits_{};
    std::string text_;
    std::u16string units_;
    std::vector<std::byte> bytes_;
    std::vector<object> object_;
    std::vector<array> array_;
};

/** One member of an @ref object literal: its key, in UTF-8, and its value. */
struct object_member {
    /** The key, in UTF-8. */
    std::string key;

    /** The value. */
    literal value;
};

/**
 An object, as JavaScript writes `{...}`: `wd::object{{"city", "Recife"}, {"zip", "50000"}}`.

 Its members are stored in the order written, by the traits' key order, and a key written twice
 keeps the last value, in the first one's place, as JavaScript does.
*/
class object {
public:
    /** An empty object, `{}`. */
    object() = default;

    /**
     An object of these members.

     @param members The members, in the order written.
    */
    object(std::initializer_list<object_member> members);

    /**
     The members, in the order written.

     @return A view of them.
    */
    [[nodiscard]] std::span<const object_member> members() const noexcept;

private:
    std::vector<object_member> members_;
};

/** An array, as JavaScript writes `[...]`: `wd::array{1, "x", nullptr}`. */
class array {
public:
    /** An empty array, `[]`. */
    array() = default;

    /**
     An array of these elements.

     @param elements The elements, in order.
    */
    array(std::initializer_list<literal> elements);

    /**
     The elements, in order.

     @return A view of them.
    */
    [[nodiscard]] std::span<const literal> elements() const noexcept;

private:
    std::vector<literal> elements_;
};

}  // namespace webcpp::document

#endif  // WEBCPP_DOCUMENT_LITERAL_HPP
