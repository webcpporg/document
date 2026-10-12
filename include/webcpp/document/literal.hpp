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

#include <bit>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
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

// A literal holds objects and arrays, which hold literals: copying or destroying one recurses as
// deep as it is written, hence the exemption of each of the four classes from misc-no-recursion.
/**
 One value of a literal, converted implicitly from the C++ value it is written as.

 It holds `bool`, any integer (character types aside), the decimal types, a binary64
 floating-point value (stored only by traits whose `number` is binary, such as @ref js_traits),
 text in UTF-8 or UTF-16, a @ref date_ms, bytes, `nullptr` or @ref null, and an @ref object or an
 @ref array. Text and bytes are copied, so a literal owns what it holds.
*/
class literal {  // NOLINT(misc-no-recursion)
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
    literal(B v) noexcept  // NOLINT(misc-explicit-constructor)
        : form_(detail::literal_form::boolean), scalar_(v ? 1U : 0U) {}

    /**
     An integer, of any width and signedness, character types aside.

     @tparam I The integer type.
     @param v The value.
    */
    template <std::integral I>
        requires(!std::same_as<I, bool> && !std::same_as<I, char> && !std::same_as<I, char8_t> &&
                 !std::same_as<I, char16_t> && !std::same_as<I, char32_t> &&
                 !std::same_as<I, wchar_t>)
    literal(I v) noexcept  // NOLINT(misc-explicit-constructor)
        : form_(std::is_signed_v<I> ? detail::literal_form::int64 : detail::literal_form::uint64),
          // A negative value wraps to its two's complement, which visit casts back.
          scalar_(static_cast<std::uint64_t>(v)) {
        static_assert(sizeof(I) <= sizeof(std::uint64_t),
                      "a literal holds integers of at most 64 bits");
    }

    /**
     A value of a decimal type: finite, or held as one no document stores.

     @tparam D The decimal type.
     @param v The value.
    */
    template <decimal_type D>
    literal(D v) noexcept {  // NOLINT(misc-explicit-constructor)
        if (!boost::decimal::isfinite(v)) {
            form_ = detail::literal_form::non_finite;
            return;
        }
        form_ = detail::literal_form::decimal;
        digits_ = from_decimal(v);
    }

    /**
     A binary64 floating-point value, which only traits whose `number` is binary store.

     @tparam F The floating-point type, of 8 bytes.
     @param v The value.
    */
    template <std::floating_point F>
        requires(sizeof(F) == 8)
    literal(F v) noexcept  // NOLINT(misc-explicit-constructor)
        : form_(detail::literal_form::binary64), scalar_(std::bit_cast<std::uint64_t>(v)) {
        static_assert(std::numeric_limits<F>::is_iec559, "a literal holds IEEE 754 binary64 only");
    }

    /**
     Text in UTF-8, copied; a null pointer holds `null`.

     @param v The text, terminated by a zero, or a null pointer.
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
     Text in UTF-16, copied; a null pointer holds `null`.

     @param v The text, terminated by a zero, or a null pointer.
    */
    literal(const char16_t* v);  // NOLINT(misc-explicit-constructor)

    /**
     Text in UTF-16, taken from a `std::u16string`, which would otherwise need two conversions,
     to a view and to a literal, that C++ does not chain.

     @tparam S `std::u16string`, a template parameter, so that it is chosen only for a
     `std::u16string` itself.
     @param v The text.
    */
    template <std::same_as<std::u16string> S>
    literal(S v)  // NOLINT(misc-explicit-constructor)
        : form_(detail::literal_form::u16string), units_(std::move(v)) {}

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
     Bytes, taken from a `std::vector<std::byte>`, which would otherwise need two conversions, to
     a span and to a literal, that C++ does not chain.

     @tparam V `std::vector<std::byte>`.
     @param v The bytes.
    */
    template <std::same_as<std::vector<std::byte>> V>
    literal(V v)  // NOLINT(misc-explicit-constructor)
        : form_(detail::literal_form::bytes), bytes_(std::move(v)) {}

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

    // A boolean as 0 or 1, an integer, a binary64's bits or a date's milliseconds.
    std::uint64_t scalar_ = 0;

    decimal_digits digits_{};
    std::string text_;
    std::u16string units_;
    std::vector<std::byte> bytes_;

    // The object or the array, as one element: they are incomplete here, and a literal is copied
    // (an initializer list holds its elements const).
    std::vector<object> object_;
    std::vector<array> array_;
};

// Copied and destroyed with its literal, recursively: see literal's exemption.
/** One member of an @ref object literal: its key, in UTF-8, and its value. */
struct object_member {  // NOLINT(misc-no-recursion)
    /**
     A member, written `{"key", value}` inside an @ref object, as a `std::pair` is: a constructor
     rather than an aggregate, which tools would ask to write with designated initializers.

     @param key The key, in UTF-8.
     @param value The value.
    */
    object_member(std::string key, literal value);

    /** The key, in UTF-8. */
    std::string key;

    /** The value. */
    literal value;
};

// Copied and destroyed with its literals, recursively: see literal's exemption.
/**
 An object, as JavaScript writes `{...}`: `wd::object{{"city", "Recife"}, {"zip", "50000"}}`.

 Its members are stored in the order written, by the traits' key order, and a key written twice
 keeps the last value, in the first one's place, as JavaScript does.

 @see "Handles", in the guide.
*/
class object {  // NOLINT(misc-no-recursion)
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

// Copied and destroyed with its literals, recursively: see literal's exemption.
/**
 An array, as JavaScript writes `[...]`: `wd::array{1, "x", nullptr}`.

 An array whose one element is an array writes that element as a literal,
 `wd::array{wd::literal(wd::array{1})}` for `[[1]]`: braces around one value of the class's own
 type are a copy of that value where the compiler applies CWG 1467 to every class (MSVC, Clang
 before 20), so `wd::array{wd::array{1}}` is `[1]` there and `[[1]]` elsewhere.

 @see "Handles", in the guide.
*/
class array {  // NOLINT(misc-no-recursion)
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

// The constructors that are not templates are defined once object and array are complete: each
// may destroy the members it has built, the vectors of object and array among them.

inline literal::literal(const char* v)
    : form_(v == nullptr ? detail::literal_form::null : detail::literal_form::string),
      text_(v == nullptr ? std::string() : std::string(v)) {}

inline literal::literal(std::string_view v) : form_(detail::literal_form::string), text_(v) {}

// Taken by reference, as the interface declares it: the string is copied, never moved.
// NOLINTNEXTLINE(modernize-pass-by-value)
inline literal::literal(const std::string& v) : form_(detail::literal_form::string), text_(v) {}

inline literal::literal(const char16_t* v)
    : form_(v == nullptr ? detail::literal_form::null : detail::literal_form::u16string),
      units_(v == nullptr ? std::u16string() : std::u16string(v)) {}

inline literal::literal(std::u16string_view v)
    : form_(detail::literal_form::u16string), units_(v) {}

inline literal::literal(date_ms v) noexcept
    : form_(detail::literal_form::date),
      scalar_(static_cast<std::uint64_t>(v.time_since_epoch().count())) {}

inline literal::literal(std::span<const std::byte> v)
    : form_(detail::literal_form::bytes), bytes_(v.begin(), v.end()) {}

inline literal::literal([[maybe_unused]] std::nullptr_t v) noexcept {}

inline literal::literal([[maybe_unused]] null_t v) noexcept {}

inline literal::literal(object v) : form_(detail::literal_form::object) {
    object_.push_back(std::move(v));
}

inline literal::literal(array v) : form_(detail::literal_form::array) {
    array_.push_back(std::move(v));
}

// A callable that visits an object's or an array's values calls visit again, recursively.
template <class F>
decltype(auto) literal::visit(F&& f) const {  // NOLINT(misc-no-recursion)
    switch (form_) {
        case detail::literal_form::null: break;
        case detail::literal_form::boolean: return std::forward<F>(f)(scalar_ != 0);
        case detail::literal_form::int64:
            return std::forward<F>(f)(static_cast<std::int64_t>(scalar_));
        case detail::literal_form::uint64: return std::forward<F>(f)(std::uint64_t{scalar_});
        case detail::literal_form::decimal: return std::forward<F>(f)(digits_);
        case detail::literal_form::non_finite:
            return std::forward<F>(f)(detail::non_finite_decimal{});
        case detail::literal_form::binary64: return std::forward<F>(f)(detail::binary64{scalar_});
        case detail::literal_form::string: return std::forward<F>(f)(std::string_view(text_));
        case detail::literal_form::u16string:
            return std::forward<F>(f)(std::u16string_view(units_));
        case detail::literal_form::date:
            return std::forward<F>(f)(
                date_ms(std::chrono::milliseconds(static_cast<std::int64_t>(scalar_))));
        case detail::literal_form::bytes:
            return std::forward<F>(f)(std::span<const std::byte>(bytes_));
        case detail::literal_form::object: return std::forward<F>(f)(object_.front());
        case detail::literal_form::array: return std::forward<F>(f)(array_.front());
    }
    return std::forward<F>(f)(null_t{});
}

inline object_member::object_member(std::string key, literal value)
    : key(std::move(key)), value(std::move(value)) {}

inline object::object(std::initializer_list<object_member> members) : members_(members) {}

inline std::span<const object_member> object::members() const noexcept {
    return members_;
}

inline array::array(std::initializer_list<literal> elements) : elements_(elements) {}

inline std::span<const literal> array::elements() const noexcept {
    return elements_;
}

}  // namespace webcpp::document

#endif  // WEBCPP_DOCUMENT_LITERAL_HPP
