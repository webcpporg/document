// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The configuration of document, which each of its headers includes first: whether document is
 built without exceptions, and the constraint its throwing members carry.

 @see "Testing", in the guide.
*/

#ifndef WEBCPP_DOCUMENT_CONFIG_HPP
#define WEBCPP_DOCUMENT_CONFIG_HPP

#include <webcpp/config.hpp>

#include <cstdint>

// MrDocs parses document as a build with exceptions does, where the macro is not defined: this
// definition, undone at once, is the one the reference lists, and the parse goes on with every API
// that throws in it.
#ifdef __MRDOCS__
/**
 Defined when document is built without exceptions: when webcpp has none (the compiler has none,
 or the developer defined `WEBCPP_NO_EXCEPTIONS` for every webcpp library), or when the developer
 defines it alone.

 document's core never throws, so the macro changes none of it. It removes the members that throw
 (`value::operator=`, `as`, `as_string`, `as_bool`, `parse_or_throw`) and the exception types: a
 call to one of those members does not compile, and the compiler names `detail::exceptions_enabled`.

 @see "Testing", in the guide.
*/
#define WEBCPP_DOCUMENT_NO_EXCEPTIONS
#undef WEBCPP_DOCUMENT_NO_EXCEPTIONS
#endif

#if !defined(WEBCPP_HAS_EXCEPTIONS) && !defined(WEBCPP_DOCUMENT_NO_EXCEPTIONS)
#define WEBCPP_DOCUMENT_NO_EXCEPTIONS
#endif

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14
/**
 Marks a class that names a place in a document and owns what it needs to find it again, a
 handle or a range of a container's children, so that GCC's `-Wdangling-reference` does not take
 one made from a temporary handle, `for (auto key : doc["a"].keys())`, for a dangling reference.
*/
#define WEBCPP_DOCUMENT_NO_DANGLING [[gnu::no_dangling]]
#else
/**
 Marks a class that names a place in a document and owns what it needs to find it again, a
 handle or a range of a container's children; empty outside GCC 14 and later, whose
 `-Wdangling-reference` it quiets.
*/
#define WEBCPP_DOCUMENT_NO_DANGLING
#endif

namespace webcpp::document {

/**
 The version of the image's format that this library writes and opens.

 The library raises it when the layout of an image changes, and then provides a migration from the
 previous version; none exists yet, since 1 is the first. It is the library's constant, not an
 option: a new Boost release does not change it, and the format test watches that the layout the
 Boost libraries give the image has not changed.

 @see "Images", in the guide.
*/
inline constexpr std::uint32_t format_version = 1;

}  // namespace webcpp::document

namespace webcpp::document::detail {

#ifdef WEBCPP_DOCUMENT_NO_EXCEPTIONS
/** Whether this build has document's throwing members: it has none. */
inline constexpr bool exceptions_on = false;
#else
/** Whether this build has document's throwing members: it has them. */
inline constexpr bool exceptions_on = true;
#endif

/**
 The constraint of every member that throws: it depends on `T`, so it is checked where the member
 is called, and a build without exceptions reports a call by this name.
*/
template <class T>
concept exceptions_enabled = exceptions_on && sizeof(T) != 0;

}  // namespace webcpp::document::detail

#endif  // WEBCPP_DOCUMENT_CONFIG_HPP
