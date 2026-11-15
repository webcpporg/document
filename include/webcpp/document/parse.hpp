// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 `JSON.parse`: JSON text read into a new document, with JSON.parse's rules, its limits, and, with
 exceptions, the sugar that throws.
*/

#ifndef WEBCPP_DOCUMENT_PARSE_HPP
#define WEBCPP_DOCUMENT_PARSE_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/fwd.hpp>
#include <webcpp/document/document.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/traits.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace webcpp::document {

/**
 The limits and the revivers of a parse.

 @see "Parsing", in the guide.
*/
struct parse_options {
    /** The deepest nesting of arrays and objects; one deeper is @ref errc::capacity. */
    std::uint64_t max_depth = 256;

    /** The most bytes of text, every chunk counted; past it is @ref errc::capacity. */
    std::uint64_t max_bytes = std::numeric_limits<std::uint64_t>::max();

    /**
     The largest image an owned document grows to, 64 MiB; past it is @ref errc::capacity.

     Each value takes at least 192 bytes of image, so a short value's image is a hundred times its
     text or more: 2 MB of `[0,0,...]` makes an image of 268 MB. The default is low so that text
     from the network is refused early: under 1 GiB, 11 MB of such text cost 9 seconds and 2.2 GB
     of memory before it failed; under 64 MiB, 700 KB fails in 0.4 seconds with 137 MB. A program
     that parses larger documents raises it.
    */
    std::uint64_t max_document_bytes = std::uint64_t{64} << 20U;

    /** Whether a string in exactly Date.prototype.toJSON's format becomes a date. */
    bool revive_dates = false;

    /** Whether a string valid in the traits' bytes policy becomes bytes. */
    bool revive_bytes = false;
};

/**
 `JSON.parse(text)`: reads JSON text into a new owned document.

 It accepts what RFC 8259 and `JSON.parse` accept: no comments, no trailing commas, whitespace of
 space, tab, line feed and carriage return only. A key written twice keeps the last value in the
 first one's place, as JavaScript does. A lone surrogate, `"\\ud800"`, is refused with either
 encoding, since Boost.JSON's parser, which document uses, cannot deliver one.

 @tparam Traits The traits of the document.
 @param text The text.
 @param o The limits and the revivers.
 @return The document; @ref errc::syntax for malformed text, @ref errc::range for a number the
 traits' `number` refuses, @ref errc::capacity past a limit or, with exceptions, when memory runs
 out.
 @see "Parsing", in the guide.
*/
template <class Traits = default_traits>
// Nothing escapes: with exceptions the parser makes an allocator with no room errc::capacity, and
// with no selector no callback throws, though the analysis sees a callback's exception rethrown.
// NOLINTNEXTLINE(bugprone-exception-escape)
result<basic_document<Traits, owned>> parse(std::string_view text,
                                            const parse_options& o = {}) noexcept {
    basic_parser<Traits, owned> p(o);
    return p.finish(text);
}

/**
 `JSON.parse(text)`: reads JSON text into a new owned document allocated through `alloc`, as
 @ref parse does.

 The document's image and the parse's working state are allocated through `alloc`; a text given
 whole, as here, leaves Boost.JSON's parser no state to suspend, so nothing else allocates. With
 exceptions, an allocator that has no room is @ref errc::capacity; without them it ends the
 program, as the allocator itself does.

 @tparam Traits The traits of the document.
 @tparam Allocator An allocator of `std::byte`.
 @param text The text.
 @param o The limits and the revivers.
 @param alloc The allocator of the document and of the parse's working state.
 @return The document; the errors of @ref parse, and @ref errc::capacity when `alloc` has no room.
 @see "Allocators", in the guide.
*/
template <class Traits = default_traits, detail::byte_allocator Allocator>
// Nothing escapes: with exceptions the parser makes an allocator with no room errc::capacity, and
// with no selector no callback throws, though the analysis sees a callback's exception rethrown.
// NOLINTNEXTLINE(bugprone-exception-escape)
result<basic_document<Traits, basic_owned<Allocator>>> parse(std::string_view text,
                                                             const parse_options& o,
                                                             const Allocator& alloc) noexcept {
    basic_parser<Traits, basic_owned<Allocator>> p(o, alloc);
    return p.finish(text);
}

/**
 Reads JSON text into a new document in a buffer its user owns, as @ref parse does.

 @tparam Traits The traits of the document.
 @param buffer The buffer, 16-aligned, which the document uses in place.
 @param text The text.
 @param o The limits and the revivers.
 @return The document; the errors of @ref parse, and @ref errc::capacity when the buffer is full.
*/
template <class Traits = default_traits>
// Nothing escapes: with exceptions the parser makes an allocator with no room errc::capacity, and
// with no selector no callback throws, though the analysis sees a callback's exception rethrown.
// NOLINTNEXTLINE(bugprone-exception-escape)
result<basic_document<Traits, external>> parse_into(std::span<std::byte> buffer,
                                                    std::string_view text,
                                                    const parse_options& o = {}) noexcept {
    basic_parser<Traits, external> p(buffer, o);
    return p.finish(text);
}

/**
 Reads JSON text into a new document in a buffer its user owns, as @ref parse_into does, the
 parse's working state and the document's allocated through `alloc`.

 Nothing else allocates, as with @ref parse. With exceptions, an allocator that has no room is
 @ref errc::capacity; without them it ends the program, as the allocator itself does.

 @tparam Traits The traits of the document.
 @tparam Allocator An allocator of `std::byte`.
 @param buffer The buffer, 16-aligned, which the document uses in place.
 @param text The text.
 @param o The limits and the revivers.
 @param alloc The allocator of the working state of the parse and of the document's operations.
 @return The document; the errors of @ref parse_into, and @ref errc::capacity when `alloc` has no
 room.
 @see "Allocators", in the guide.
*/
template <class Traits = default_traits, detail::byte_allocator Allocator>
// Nothing escapes: with exceptions the parser makes an allocator with no room errc::capacity, and
// with no selector no callback throws, though the analysis sees a callback's exception rethrown.
// NOLINTNEXTLINE(bugprone-exception-escape)
result<basic_document<Traits, basic_external<Allocator>>> parse_into(
    std::span<std::byte> buffer, std::string_view text, const parse_options& o,
    const Allocator& alloc) noexcept {
    basic_parser<Traits, basic_external<Allocator>> p(buffer, o, alloc);
    return p.finish(text);
}

/**
 `JSON.parse(text)`, throwing on failure as JavaScript does; only with exceptions.

 @tparam Traits The traits of the document.
 @param text The text.
 @param o The limits and the revivers.
 @return The document.
 @throws syntax_error For malformed text, with the byte offset where it was found.
 @throws range_error For a number the traits' `number` refuses.
 @throws capacity_error Past a limit, or when memory runs out.
 @see "The sugar that throws", in the guide.
*/
template <class Traits = default_traits>
    requires detail::exceptions_enabled<Traits>
basic_document<Traits, owned> parse_or_throw(std::string_view text, const parse_options& o = {});

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS

template <class Traits>
    requires detail::exceptions_enabled<Traits>
basic_document<Traits, owned> parse_or_throw(std::string_view text, const parse_options& o) {
    basic_parser<Traits, owned> p(o);
    result<basic_document<Traits, owned>> made = p.finish(text);
    if (!made.has_value()) {
        detail::raise(made.error(), p.offset());
    }
    return std::move(made).unsafe_value();
}

#endif  // WEBCPP_DOCUMENT_NO_EXCEPTIONS

}  // namespace webcpp::document

#include <webcpp/document/parser.hpp>

#endif  // WEBCPP_DOCUMENT_PARSE_HPP
