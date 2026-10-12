// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The errors document reports, as `boost::system::error_code` values of a category of its own, the
 result type of every operation that can fail, and, with exceptions, the exceptions of the sugar.

 @see "Testing", in the guide.
*/

#ifndef WEBCPP_DOCUMENT_ERRORS_HPP
#define WEBCPP_DOCUMENT_ERRORS_HPP

#include <webcpp/document/config.hpp>

#include <boost/system/error_category.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
#include <boost/throw_exception.hpp>

#include <stdexcept>
#endif

namespace webcpp::document {

/**
 The errors document reports, as `boost::system::error_code` values of the category
 @ref error_category.

 The values start at 1, so an `error_code` of value 0 is no error. An `errc` converts to an
 `error_code` and compares with one: `made.error() == wd::errc::syntax`.

 @note The values are fixed: they are never reused for a different meaning, so a stored value
 keeps its meaning across versions.
 @see "Errors", in the guide.
*/
enum class errc : int {
    /** Malformed JSON text, JavaScript's `SyntaxError`. */
    syntax = 1,

    /**
     A value of another kind, a write through nothing, or text invalid in the encoding,
     JavaScript's `TypeError`.
    */
    type = 2,

    /**
     A number that does not fit or is not exact, an index out of range where JavaScript throws, or
     a date out of range, JavaScript's `RangeError`.
    */
    range = 3,

    /** No room left: a full external buffer, an owned document at its limit, a parser's limit. */
    capacity = 4,

    /** Bytes that are not a document's image. */
    not_a_document = 5,

    /** An image of another format version, nonzero reserved bytes, layout, byte order or traits. */
    incompatible_document = 6,

    /** A sealed image whose bytes do not match its seal: changed, torn or cut short. */
    damaged_document = 7,
};

namespace detail {

/** The human message of an @ref errc value, or the unknown message for any other value. */
inline const char* message_of(int value) noexcept {
    switch (static_cast<errc>(value)) {
        case errc::syntax: return "malformed JSON text";
        case errc::type: return "a value of another kind";
        case errc::range: return "a number or index out of range";
        case errc::capacity: return "no room left";
        case errc::not_a_document: return "not a document's image";
        case errc::incompatible_document:
            return "an image of another format, layout, byte order or traits";
        case errc::damaged_document: return "a sealed image whose bytes do not match its seal";
    }
    return "unknown webcpp.document error";
}

/**
 A constant category with a fixed identity, so it holds no mutable state.

 @note The identity is the ASCII of "document".
*/
class category final : public boost::system::error_category {
public:
    /** Constructs the category with its fixed identity. */
    constexpr category() noexcept : boost::system::error_category(0x646f63756d656e74ULL) {}

    /** The category's name, "webcpp.document". */
    const char* name() const noexcept override { return "webcpp.document"; }

    /** The human message of the error `value`, as @ref message_of spells it. */
    std::string message(int value) const override { return message_of(value); }

    /**
     Copies the human message of the error `value` into `buffer`, cut to fit `size` bytes with
     its terminating zero, and returns the whole message.
    */
    const char* message(int value, char* buffer, std::size_t size) const noexcept override {
        const char* text = message_of(value);
        if (buffer != nullptr && size != 0) {
            const std::string_view spelling(text);
            const std::size_t limit = size - 1;
            const std::size_t count = spelling.size() < limit ? spelling.size() : limit;
            std::memcpy(buffer, spelling.data(), count);
            buffer[count] = '\0';
        }
        return text;
    }
};

/** The one category of document's errors, which @ref error_category returns. */
inline constexpr category the_category{};

}  // namespace detail

/**
 The category of every @ref errc.

 Its `name()` is "webcpp.document", and its `message(value)` the human message of the error, "no
 room left" for @ref errc::capacity, or "unknown webcpp.document error" for a value @ref errc does
 not define.

 @return The category, a constant with a fixed identity.
*/
inline const boost::system::error_category& error_category() noexcept {
    return detail::the_category;
}

/**
 Makes the `error_code` of an @ref errc, as its implicit conversion does.

 @param e The error.
 @return `e` as an `error_code` of the category @ref error_category.
*/
inline boost::system::error_code make_error_code(errc e) noexcept {
    return {static_cast<int>(e), error_category()};
}

/**
 What every operation of document that can fail returns: a `T`, or the `error_code` of the
 failure.

 It is Boost.System's `result`. document's core never throws: every operation of it that can fail
 returns one, with an @ref errc, or the error a sink or a selector's callback of the user's
 returned, passed on unchanged.

 @tparam T The type of the value; `void` for an operation that returns none.
*/
template <class T>
using result = boost::system::result<T>;

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS

/**
 What every exception of document derives from: a `std::runtime_error` holding its code.

 @see "The sugar that throws", in the guide.
*/
class error : public std::runtime_error {
public:
    /**
     Constructs the exception of `code`, whose message is the code's.

     @param code The code of the failure.
    */
    explicit error(boost::system::error_code code)
        : std::runtime_error(code.message()), code_(code) {}

    /**
     The code of the failure.

     @return The code given at construction.
    */
    [[nodiscard]] boost::system::error_code code() const noexcept { return code_; }

private:
    boost::system::error_code code_;
};

/** JavaScript's `SyntaxError`: malformed JSON text, at a byte offset. */
class syntax_error : public error {
public:
    /**
     Constructs the exception of `code` at byte `offset` of the text.

     @param code The code of the failure, @ref errc::syntax.
     @param offset The byte offset of the failure in the text.
    */
    syntax_error(boost::system::error_code code, std::uint64_t offset)
        : error(code), offset_(offset) {}

    /**
     The byte offset of the failure in the text.

     @return The offset given at construction.
    */
    [[nodiscard]] std::uint64_t offset() const noexcept { return offset_; }

private:
    std::uint64_t offset_;
};

/** JavaScript's `TypeError`: a value of another kind, or a write through nothing. */
class type_error : public error {
public:
    /** Constructs the exception of a code, as @ref error does. */
    using error::error;
};

/** JavaScript's `RangeError`: a number that does not fit, or an index out of range. */
class range_error : public error {
public:
    /** Constructs the exception of a code, as @ref error does. */
    using error::error;
};

/** No room left: a full external buffer, a document at its limit, a parser's limit. */
class capacity_error : public error {
public:
    /** Constructs the exception of a code, as @ref error does. */
    using error::error;
};

/** Bytes that are not a document's image, an image of another format, or a damaged one. */
class image_error : public error {
public:
    /** Constructs the exception of a code, as @ref error does. */
    using error::error;
};

namespace detail {

/**
 Throws the exception of `code`'s @ref errc through `boost::throw_exception`; a code of another
 category throws @ref error.
*/
[[noreturn]] inline void raise(boost::system::error_code code, std::uint64_t offset = 0) {
    if (code.category() != error_category()) {
        boost::throw_exception(error(code));
    }
    switch (static_cast<errc>(code.value())) {
        case errc::syntax: boost::throw_exception(syntax_error(code, offset));
        case errc::type: boost::throw_exception(type_error(code));
        case errc::range: boost::throw_exception(range_error(code));
        case errc::capacity: boost::throw_exception(capacity_error(code));
        case errc::not_a_document:
        case errc::incompatible_document:
        case errc::damaged_document: boost::throw_exception(image_error(code));
    }
    boost::throw_exception(error(code));
}

}  // namespace detail

#endif  // WEBCPP_DOCUMENT_NO_EXCEPTIONS

}  // namespace webcpp::document

namespace boost::system {
template <>
struct is_error_code_enum<webcpp::document::errc> : std::true_type {};
}  // namespace boost::system

#endif  // WEBCPP_DOCUMENT_ERRORS_HPP
