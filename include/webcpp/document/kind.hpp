// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The kinds of a value, JavaScript's `typeof` with `null`, arrays, dates and bytes told apart, and
 the two values that are no C++ type: `undefined` and `null`.
*/

#ifndef WEBCPP_DOCUMENT_KIND_HPP
#define WEBCPP_DOCUMENT_KIND_HPP

#include <webcpp/document/config.hpp>

#include <cstdint>

namespace webcpp::document {

/**
 The kind of a value: JavaScript's `typeof`, with `null`, arrays, dates and bytes told apart.

 @note The values are fixed: they are never reused for a different meaning, so a stored value
 keeps its meaning across versions.
 @see "Handles", in the guide.
*/
enum class kind : std::uint8_t {
    /** JavaScript's `undefined`: never stored; what a handle reads when nothing is there. */
    undefined = 0,

    /** JavaScript's `null`. */
    null = 1,

    /** `true` or `false`. */
    boolean = 2,

    /** A number, held as the traits' `integer` or `number`. */
    number = 3,

    /** A string. */
    string = 4,

    /** A `Date`, held as the traits' `date`. */
    date = 5,

    /** A `Uint8Array`: bytes, stored raw. */
    bytes = 6,

    /** An array. */
    array = 7,

    /** A plain object. */
    object = 8,
};

/**
 The type of @ref undefined, JavaScript's `undefined`, which `set` takes to erase a member.
*/
struct undefined_t {};

/**
 JavaScript's `undefined`: `v.set(wd::undefined)` erases a member, as `delete` does, and stores
 `null` in an element.
*/
inline constexpr undefined_t undefined{};

/**
 The type of @ref null, JavaScript's `null`.
*/
struct null_t {};

/** JavaScript's `null`, which `set` stores, as `nullptr` does. */
inline constexpr null_t null{};

}  // namespace webcpp::document

#endif  // WEBCPP_DOCUMENT_KIND_HPP
