// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The JSON writer `stringify` and `stringify_to` share: it walks a subtree depth-first, with a stack
 of its own rather than recursion, and writes what ECMA-262's SerializeJSONObject,
 SerializeJSONArray and QuoteJSONString write, to an output that `stringify` keeps whole and
 `stringify_to` hands to a sink in chunks.
*/

#ifndef WEBCPP_DOCUMENT_DETAIL_WRITER_HPP
#define WEBCPP_DOCUMENT_DETAIL_WRITER_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/tree.hpp>
#include <webcpp/document/detail/utf.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/number.hpp>
#include <webcpp/document/traits.hpp>

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace webcpp::document::detail {

/** The most spaces of one level: `JSON.stringify` clamps its third argument to 10. */
inline constexpr std::uint32_t max_indent = 10;

/**
 A sink of `stringify_to`: a callable taking `std::string_view` and returning `void` or
 `result<void>`.
*/
template <class S>
concept sink = std::invocable<S&, std::string_view> &&
               (std::same_as<std::invoke_result_t<S&, std::string_view>, void> ||
                std::same_as<std::invoke_result_t<S&, std::string_view>, result<void>>);

/**
 An output of the writer: `put` takes the next text, and a failure it returns stops the writing.
*/
template <class Out>
concept output = requires(Out& out, std::string_view text) {
    { out.put(text) } -> std::same_as<result<void>>;
};

/** The output of `stringify`: the text appended to a string. */
class string_output {
public:
    /** An output that appends to `text`, which outlives it. */
    explicit string_output(std::string& text) noexcept;

    /** Appends `text`; the standard library's `std::bad_alloc` is its only failure. */
    result<void> put(std::string_view text);

private:
    std::string* text_;
};

/** The output of `stringify_to`: the text handed to a sink in pieces of at most `chunk` bytes. */
template <class Sink>
class chunked_output {
public:
    /** An output to `sink`, which outlives it, in pieces of at most `chunk` bytes, at least 1. */
    chunked_output(Sink& sink, std::uint64_t chunk);

    /** Takes `text`, handing the sink each full piece; the sink's failure is returned. */
    result<void> put(std::string_view text);

    /** Hands the sink what is left; the sink's failure is returned. */
    result<void> flush();

private:
    Sink* sink_;
    std::uint64_t chunk_;
    std::string pending_;
};

/**
 Writes the subtree of `root` as `JSON.stringify(value, null, indent)` does, `indent` clamped to
 @ref max_indent; a null `root`, `undefined`, writes nothing.
*/
template <class Traits, class Out>
    requires output<Out>
result<void> write_json(const tree<Traits>& t, const node<Traits>* root, std::uint32_t indent,
                        Out& out);

}  // namespace webcpp::document::detail

#endif  // WEBCPP_DOCUMENT_DETAIL_WRITER_HPP
