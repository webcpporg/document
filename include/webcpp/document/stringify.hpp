// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 `JSON.stringify`: a value written as JSON text, whole into a `std::string`, or in pieces to a
 sink of the program's own, the same bytes either way.
*/

#ifndef WEBCPP_DOCUMENT_STRINGIFY_HPP
#define WEBCPP_DOCUMENT_STRINGIFY_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/writer.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/value.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace webcpp::document {

/** The size of the pieces `stringify_to` hands its sink, unless told otherwise. */
inline constexpr std::uint64_t default_chunk = 4096;

/**
 `JSON.stringify(v, null, indent)`: writes a value as JSON text.

 Keys come out in the policy's order; strings are escaped as `JSON.stringify` escapes them;
 numbers are written as JavaScript writes them; a date as Date.prototype.toJSON writes it; bytes by
 the traits' bytes policy.

 @tparam Traits The document's traits.
 @tparam Storage The document's storage.
 @param v The value.
 @param indent The spaces of each level, clamped to 10; 0 writes compactly.
 @return The text; empty when the value is `undefined`, since `JSON.stringify(undefined)` returns
 no text. The standard library's `std::bad_alloc` is its only failure.
*/
template <class Traits, class Storage>
std::string stringify(basic_const_value<Traits, Storage> v, std::uint32_t indent = 0);

/**
 `JSON.stringify(doc, null, indent)`: writes a document's value as JSON text.

 @tparam Traits The document's traits.
 @tparam Storage The document's storage.
 @param d The document.
 @param indent The spaces of each level, clamped to 10; 0 writes compactly.
 @return The text, as @ref stringify of its root writes it.
*/
template <class Traits, class Storage>
std::string stringify(const basic_document<Traits, Storage>& d, std::uint32_t indent = 0);

/**
 Writes a value as @ref stringify does, in pieces of at most `chunk` bytes, to a sink.

 @tparam Traits The document's traits.
 @tparam Storage The document's storage.
 @tparam Sink A callable taking `std::string_view` and returning `void` or `result<void>`.
 @param v The value.
 @param sink The sink.
 @param indent The spaces of each level, clamped to 10; 0 writes compactly.
 @param chunk The largest piece, at least 1.
 @return Success, or the first failure the sink returned, which stops the writing; an exception
 the sink throws propagates. The document is unchanged either way.
*/
template <class Traits, class Storage, class Sink>
    requires detail::sink<std::remove_reference_t<Sink>>
result<void> stringify_to(basic_const_value<Traits, Storage> v, Sink&& sink,
                          std::uint32_t indent = 0, std::uint64_t chunk = default_chunk);

/**
 Writes a document's value as @ref stringify does, in pieces of at most `chunk` bytes, to a sink.

 @tparam Traits The document's traits.
 @tparam Storage The document's storage.
 @tparam Sink A callable taking `std::string_view` and returning `void` or `result<void>`.
 @param d The document.
 @param sink The sink.
 @param indent The spaces of each level, clamped to 10; 0 writes compactly.
 @param chunk The largest piece, at least 1.
 @return Success, or the first failure the sink returned.
*/
template <class Traits, class Storage, class Sink>
    requires detail::sink<std::remove_reference_t<Sink>>
result<void> stringify_to(const basic_document<Traits, Storage>& d, Sink&& sink,
                          std::uint32_t indent = 0, std::uint64_t chunk = default_chunk);

}  // namespace webcpp::document

#endif  // WEBCPP_DOCUMENT_STRINGIFY_HPP
