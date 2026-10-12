// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The streaming parser: JSON text fed in chunks of any size, down to one byte, and values delivered,
 through selectors, as they complete.
*/

#ifndef WEBCPP_DOCUMENT_PARSER_HPP
#define WEBCPP_DOCUMENT_PARSER_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/fwd.hpp>
#include <webcpp/document/detail/selector.hpp>
#include <webcpp/document/document.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/value.hpp>

#include <boost/json/basic_parser_impl.hpp>
#include <boost/system/error_code.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace webcpp::document {

/**
 The path in the text of a value a selector delivers: each step a member's key or an element's
 index in the text, which `on` does not renumber.

 It is valid during the callback that receives it.
*/
class path_view {
public:
    /** The path of the root, which has no step. */
    path_view() noexcept = default;

    /**
     The path of these steps.

     @param steps The steps, from the root.
    */
    explicit path_view(std::span<const detail::path_step> steps) noexcept;

    /**
     How many steps the path has.

     @return The count; 0 for the root.
    */
    [[nodiscard]] std::size_t size() const noexcept;

    /**
     Whether a step is an element's.

     @param i The step, below @ref size.
     @return Whether it is an index rather than a key.
    */
    [[nodiscard]] bool is_index(std::size_t i) const noexcept;

    /**
     A member's key.

     @param i The step, below @ref size, a key's.
     @return The key, in UTF-8.
    */
    [[nodiscard]] std::string_view key(std::size_t i) const noexcept;

    /**
     An element's index in the text.

     @param i The step, below @ref size, an index's.
     @return The index.
    */
    [[nodiscard]] std::uint64_t index(std::size_t i) const noexcept;

    /**
     The path as normalised JSONPath: `$['items'][3]`, a key's `'` and `\` escaped.

     @return The text.
    */
    [[nodiscard]] std::string to_string() const;

private:
    std::span<const detail::path_step> steps_;
};

namespace detail {

/**
 A selector's callback: invocable with a read-only handle and its path, returning `void` or
 `result<void>`.
*/
template <class F, class Traits, class Storage>
concept selector_callback =
    std::invocable<F&, basic_const_value<Traits, Storage>, path_view> &&
    (std::same_as<std::invoke_result_t<F&, basic_const_value<Traits, Storage>, path_view>, void> ||
     std::same_as<std::invoke_result_t<F&, basic_const_value<Traits, Storage>, path_view>,
                  result<void>>);

/** A selector and its callback, as `on` or `observe` registered it. */
template <class Traits, class Storage>
struct subscription {
    /** The places it matches. */
    selector where;

    /**
     Whether the value is dropped after its delivery, as `on` asks, or kept, as `observe` does.
    */
    bool take = false;

    /** The callback, its `void` made success. */
    std::function<result<void>(basic_const_value<Traits, Storage>, path_view)> deliver;
};

}  // namespace detail

/**
 The streaming parser: `write` takes the text in chunks, `finish` returns the document.

 Boost.JSON's `basic_parser` is fed each chunk, and its handler builds the document node by node,
 by ids, so that the document may grow and move between chunks. A failed `write` leaves the parser
 failed: every later call returns the same error, until @ref reset. The parser raises nothing of
 its own, since its input is the network's; an exception a callback throws propagates from `write`
 or `finish`, and leaves the parser failed.

 @tparam Traits The traits of the document.
 @tparam Storage @ref owned, or @ref external to build it in a buffer of its user's.
*/
template <class Traits, class Storage>
class basic_parser {
public:
    /**
     A parser of an owned document.

     @param o The limits and the revivers.
    */
    explicit basic_parser(const parse_options& o = {}) noexcept
        requires std::same_as<Storage, owned>;

    /**
     A parser of a document in a buffer its user owns, formatted when the text begins.

     @param buffer The buffer, 16-aligned, which outlives the parser and its document.
     @param o The limits and the revivers.
    */
    explicit basic_parser(std::span<std::byte> buffer, const parse_options& o = {}) noexcept
        requires std::same_as<Storage, external>;

    /** A parser is not copied: its handler and its document refer to it. */
    basic_parser(const basic_parser&) = delete;

    /** A parser is not copied: its handler and its document refer to it. */
    basic_parser& operator=(const basic_parser&) = delete;

    /** A parser is not moved: its handler and its document refer to it. */
    basic_parser(basic_parser&&) = delete;

    /** A parser is not moved: its handler and its document refer to it. */
    basic_parser& operator=(basic_parser&&) = delete;

    /** Destroys the parser and the document it was building. */
    ~basic_parser() = default;

    /**
     Feeds the next chunk of the text, delivering each value a selector matches as it completes.

     @param chunk The chunk: any split of the text, down to one byte.
     @return Success; @ref errc::syntax for malformed text, @ref errc::range for a number the
     traits refuse, @ref errc::capacity past a limit, or the error a callback returned.
    */
    result<void> write(std::string_view chunk);

    /**
     Ends the text and returns the document, without the values `on` took.

     @return The document; the errors of @ref write, and @ref errc::syntax for text that ends
     early.
    */
    result<basic_document<Traits, Storage>> finish();

    /** Makes the parser ready for another text, its selectors kept. */
    void reset() noexcept;

    /**
     How many bytes of the text were consumed.

     @return The count; after an @ref errc::syntax, the offset where the error was found.
    */
    [[nodiscard]] std::uint64_t offset() const noexcept;

    /**
     The code of Boost.JSON behind an @ref errc::syntax.

     @return The code, or none.
    */
    [[nodiscard]] boost::system::error_code cause() const noexcept;

    /**
     Delivers each value whose path matches `selector` once it completes, then removes it from the
     document, its blocks zeroed and freed: a stream of a million elements needs the memory of one.

     The selector is `$` then any sequence of `.name`, `['name']`, `[n]`, `[*]` and `.*`. A scalar
     completes when its token ends, an array or object when it closes; a value inside another that
     also matches is delivered first.

     @tparam F A callable taking a read-only handle and a @ref path_view, returning `void` or
     `result<void>`.
     @param selector The selector.
     @param f The callback.
     @return Success; @ref errc::type for a malformed selector.
    */
    template <class F>
    result<void> on(std::string_view selector, F&& f)
        requires detail::selector_callback<std::remove_cvref_t<F>, Traits, Storage>;

    /**
     Delivers each value whose path matches `selector` once it completes, and keeps it.

     @tparam F A callable taking a read-only handle and a @ref path_view, returning `void` or
     `result<void>`.
     @param selector The selector, as @ref on takes it.
     @param f The callback.
     @return Success; @ref errc::type for a malformed selector.
    */
    template <class F>
    result<void> observe(std::string_view selector, F&& f)
        requires detail::selector_callback<std::remove_cvref_t<F>, Traits, Storage>;

private:
    template <class, class>
    friend class detail::handler;

    parse_options options_;
    std::span<std::byte> buffer_;
    std::optional<basic_document<Traits, Storage>> document_;
    std::vector<detail::subscription<Traits, Storage>> subscriptions_;
    boost::json::basic_parser<detail::handler<Traits, Storage>> json_;
    result<void> failure_;
    std::uint64_t consumed_ = 0;
};

/** The streaming parser of a @ref document. */
using parser = basic_parser<>;

}  // namespace webcpp::document

#include <webcpp/document/detail/handler.hpp>

#endif  // WEBCPP_DOCUMENT_PARSER_HPP
