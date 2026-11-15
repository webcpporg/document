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

#include <boost/throw_exception.hpp>

#include <cstdint>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace webcpp::document {

/** The size of the pieces `stringify_to` hands its sink, unless told otherwise. */
inline constexpr std::uint64_t default_chunk = 4096;

// A handle is taken by value, as every handle is passed.
// NOLINTBEGIN(performance-unnecessary-value-param)
/**
 `JSON.stringify(v, null, indent)`: writes a value as JSON text.

 Keys come out in the policy's order; strings are escaped as `JSON.stringify` escapes them;
 numbers are written as JavaScript writes them; a date as Date.prototype.toJSON writes it; bytes by
 the traits' bytes policy.

 The text is a `std::string`, the program's value in the standard allocator whatever the
 document's. The writer's stack, a frame of 16 bytes for each container it is inside, with room
 for 16 frames at first and twice the room each time it is full, is in the document's allocator,
 rebound, and freed when it returns.

 @tparam Traits The document's traits.
 @tparam Storage The document's storage.
 @param v The value.
 @param indent The spaces of each level, clamped to 10; 0 writes compactly.
 @return The text; empty when the value is `undefined`, since `JSON.stringify(undefined)` returns
 no text.
 @throws std::bad_alloc When the standard allocator has no room for the text or, with exceptions,
 the document's allocator none for the stack; without exceptions, an allocator that fails ends the
 program, as it does itself.
 @see "Writing JSON", in the guide.
*/
template <class Traits, class Storage>
std::string stringify(basic_const_value<Traits, Storage> v, std::uint32_t indent = 0) {
    std::string text;
    const auto* t = detail::access::tree_of(v);
    if (t == nullptr) {
        return text;
    }
    detail::string_output out(text);
    // A string output fails only by throwing: a failure returned is the stack's, with exceptions,
    // which the text a std::string returns cannot carry.
    if (!detail::write_json(*t, detail::access::node_of(v), indent, out, t->get_allocator())) {
        boost::throw_exception(std::bad_alloc());
    }
    return text;
}

// NOLINTEND(performance-unnecessary-value-param)

/**
 `JSON.stringify(doc, null, indent)`: writes a document's value as JSON text.

 @tparam Traits The document's traits.
 @tparam Storage The document's storage.
 @param d The document.
 @param indent The spaces of each level, clamped to 10; 0 writes compactly.
 @return The text, as @ref stringify of its root writes it.
 @throws std::bad_alloc As @ref stringify of its root throws it.
*/
template <class Traits, class Storage>
std::string stringify(const basic_document<Traits, Storage>& d, std::uint32_t indent = 0) {
    return webcpp::document::stringify(d.root(), indent);
}

// A handle is taken by value, as every handle is passed.
// NOLINTBEGIN(performance-unnecessary-value-param)
/**
 Writes a value as @ref stringify does, in pieces of at most `chunk` bytes, to a sink.

 It allocates only its working state, in the document's allocator, rebound, and frees it when it
 returns: its stack, a frame of 16 bytes for each container it is inside, with room for 16 frames
 at first and twice the room each time it is full; and the piece it holds back until it is full,
 given the smaller of `chunk` and 4096 bytes the first time it holds more than a string's own
 small buffer does, then, for a larger chunk, twice its room, up to `chunk`, each time it is full.
 What the sink allocates is the program's own.

 @tparam Traits The document's traits.
 @tparam Storage The document's storage.
 @tparam Sink A callable taking `std::string_view` and returning `void` or `result<void>`.
 @param v The value.
 @param sink The sink.
 @param indent The spaces of each level, clamped to 10; 0 writes compactly.
 @param chunk The largest piece, at least 1.
 @return Success, or the first failure the sink returned, which stops the writing; with
 exceptions, @ref errc::capacity when the document's allocator has no room for the stack or the
 piece, which stops it too, the sink not called again. An exception the sink throws propagates.
 The document is unchanged either way; without exceptions, an allocator that fails ends the
 program, as it does itself.
 @see "Writing JSON", in the guide.
*/
template <class Traits, class Storage, class Sink>
    requires detail::sink<std::remove_reference_t<Sink>>
result<void> stringify_to(basic_const_value<Traits, Storage> v, Sink&& sink,
                          std::uint32_t indent = 0, std::uint64_t chunk = default_chunk) {
    const auto* t = detail::access::tree_of(v);
    if (t == nullptr) {
        return {};
    }
    const typename Storage::allocator_type alloc = t->get_allocator();
    // The sink is called through a reference: it is used in place, never forwarded or copied.
    detail::chunked_output<std::remove_reference_t<Sink>, typename Storage::allocator_type> out(
        sink, chunk, alloc);
    if (const result<void> written =
            detail::write_json(*t, detail::access::node_of(v), indent, out, alloc);
        !written) {
        return written;
    }
    return out.flush();
}

// NOLINTEND(performance-unnecessary-value-param)

/**
 Writes a document's value as @ref stringify does, in pieces of at most `chunk` bytes, to a sink.

 @tparam Traits The document's traits.
 @tparam Storage The document's storage.
 @tparam Sink A callable taking `std::string_view` and returning `void` or `result<void>`.
 @param d The document.
 @param sink The sink.
 @param indent The spaces of each level, clamped to 10; 0 writes compactly.
 @param chunk The largest piece, at least 1.
 @return Success, the first failure the sink returned, or with exceptions @ref errc::capacity, as
 @ref stringify_to of its root returns them.
*/
template <class Traits, class Storage, class Sink>
    requires detail::sink<std::remove_reference_t<Sink>>
result<void> stringify_to(const basic_document<Traits, Storage>& d, Sink&& sink,
                          std::uint32_t indent = 0, std::uint64_t chunk = default_chunk) {
    return webcpp::document::stringify_to(d.root(), std::forward<Sink>(sink), indent, chunk);
}

// The members, declared with the handles and the document, which are complete here.

template <class Traits, class Storage>
std::string basic_const_value<Traits, Storage>::stringify(std::uint32_t indent) const {
    return webcpp::document::stringify(*this, indent);
}

template <class Traits, class Storage>
std::string basic_document<Traits, Storage>::stringify(std::uint32_t indent) const {
    return webcpp::document::stringify(*this, indent);
}

}  // namespace webcpp::document

#endif  // WEBCPP_DOCUMENT_STRINGIFY_HPP
