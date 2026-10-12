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
 traits' `number` refuses, @ref errc::capacity past a limit.
 @see "Parsing", in the guide.
*/
template <class Traits = default_traits>
// The parser ends the program where the process heap, which holds its frames and pending text,
// runs out, as the library tolerates; with no selector, nothing else of it throws.
// NOLINTNEXTLINE(bugprone-exception-escape)
result<basic_document<Traits, owned>> parse(std::string_view text,
                                            const parse_options& o = {}) noexcept {
    basic_parser<Traits, owned> p(o);
    if (const result<void> written = p.write(text); !written.has_value()) {
        return written.error();
    }
    return p.finish();
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
// The parser ends the program where the process heap, which holds its frames and pending text,
// runs out, as the library tolerates; with no selector, nothing else of it throws.
// NOLINTNEXTLINE(bugprone-exception-escape)
result<basic_document<Traits, external>> parse_into(std::span<std::byte> buffer,
                                                    std::string_view text,
                                                    const parse_options& o = {}) noexcept {
    basic_parser<Traits, external> p(buffer, o);
    if (const result<void> written = p.write(text); !written.has_value()) {
        return written.error();
    }
    return p.finish();
}

/**
 `JSON.parse(text)`, throwing on failure as JavaScript does; only with exceptions.

 @tparam Traits The traits of the document.
 @param text The text.
 @param o The limits and the revivers.
 @return The document.
 @throws syntax_error For malformed text, with the byte offset where it was found.
 @throws range_error For a number the traits' `number` refuses.
 @throws capacity_error Past a limit.
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
    if (const result<void> written = p.write(text); !written.has_value()) {
        detail::raise(written.error(), p.offset());
    }
    result<basic_document<Traits, owned>> made = p.finish();
    if (!made.has_value()) {
        detail::raise(made.error(), p.offset());
    }
    return std::move(made).unsafe_value();
}

#endif  // WEBCPP_DOCUMENT_NO_EXCEPTIONS

}  // namespace webcpp::document

#include <webcpp/document/parser.hpp>

#endif  // WEBCPP_DOCUMENT_PARSE_HPP
