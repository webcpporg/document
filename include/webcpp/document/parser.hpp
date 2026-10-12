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
#include <boost/json/error.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
#include <exception>
#endif

namespace webcpp::document {

/**
 The path in the text of a value a selector delivers: each step a member's key or an element's
 index in the text, which `on` does not renumber.

 It is valid during the callback that receives it.

 @see "Selectors", in the guide.
*/
class path_view {
public:
    /** The path of the root, which has no step. */
    path_view() noexcept = default;

    /**
     The path of these steps.

     @param steps The steps, from the root.
    */
    explicit path_view(std::span<const detail::path_step> steps) noexcept : steps_(steps) {}

    /**
     How many steps the path has.

     @return The count; 0 for the root.
    */
    [[nodiscard]] std::size_t size() const noexcept { return steps_.size(); }

    /**
     Whether a step is an element's.

     @param i The step, below @ref size.
     @return Whether it is an index rather than a key.
    */
    [[nodiscard]] bool is_index(std::size_t i) const noexcept { return steps_[i].is_index; }

    /**
     A member's key.

     @param i The step, below @ref size, a key's.
     @return The key, in UTF-8.
    */
    [[nodiscard]] std::string_view key(std::size_t i) const noexcept { return steps_[i].key; }

    /**
     An element's index in the text.

     @param i The step, below @ref size, an index's.
     @return The index.
    */
    [[nodiscard]] std::uint64_t index(std::size_t i) const noexcept { return steps_[i].index; }

    /**
     The path as a selector of its place, which @ref basic_parser::on parses back: `$['items'][3]`.

     A key is quoted, its `'` and `\` escaped and every other byte written as it is, control
     characters included; so it is not RFC 9535's normalized path, which escapes those too.

     @return The text.
    */
    [[nodiscard]] std::string to_string() const {
        std::string out = "$";
        for (const detail::path_step& step : steps_) {
            if (step.is_index) {
                out += '[';
                out += std::to_string(step.index);
                out += ']';
                continue;
            }
            // The two escapes the selectors read, so that the text is a selector of this place.
            out += "['";
            for (const char c : step.key) {
                if (c == '\'' || c == '\\') {
                    out += '\\';
                }
                out += c;
            }
            out += "']";
        }
        return out;
    }

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
 or `finish`, and leaves the parser failed: every later call throws it again, until @ref reset.

 @tparam Traits The traits of the document.
 @tparam Storage @ref owned, or @ref external to build it in a buffer of its user's.
 @see "Parsing", in the guide.
*/
template <class Traits, class Storage>
class basic_parser {
public:
    /**
     A parser of an owned document.

     @param o The limits and the revivers.
    */
    explicit basic_parser(const parse_options& o = {}) noexcept
        requires std::same_as<Storage, owned>
        : options_(o), json_(json_options(o), *this) {}

    /**
     A parser of a document in a buffer its user owns, formatted when the text begins.

     @param buffer The buffer, 16-aligned, which outlives the parser and its document.
     @param o The limits and the revivers.
    */
    explicit basic_parser(std::span<std::byte> buffer, const parse_options& o = {}) noexcept
        requires std::same_as<Storage, external>
        : options_(o), buffer_(buffer), json_(json_options(o), *this) {}

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
     @return Success; @ref errc::syntax for malformed text, a second text after the first included,
     @ref errc::range for a number the traits refuse, @ref errc::capacity past a limit, or the
     error a callback returned.
    */
    result<void> write(std::string_view chunk) {
        if (!failure_.has_value()) {
            return failed();
        }
        if (ended_) {
            return end_again();
        }
        if (chunk.size() > options_.max_bytes - given_) {
            failure_ = errc::capacity;
            return failure_;
        }
        given_ += chunk.size();
        if (chunk.empty()) {
            return {};
        }
        boost::system::error_code ec;
        const std::size_t used = json_.write_some(true, chunk.data(), chunk.size(), ec);
        // Boost.JSON stops where a second text begins, without an error, and its next call would
        // begin that text as a new document.
        if (!ec && used < chunk.size()) {
            ec = boost::json::error::extra_data;
        }
        return settle(ec, used);
    }

    /**
     Ends the text and returns the document, without the values `on` took.

     After it the parser takes no more text, which is @ref errc::syntax, until @ref reset.

     @return The document; the errors of @ref write, and @ref errc::syntax for text that ends
     early.
    */
    result<basic_document<Traits, Storage>> finish() {
        if (!failure_.has_value()) {
            return failed().error();
        }
        if (ended_) {
            return end_again().error();
        }
        // No more text, as Boost.JSON's own stream_parser ends one.
        boost::system::error_code ec;
        static_cast<void>(json_.write_some(false, nullptr, 0, ec));
        if (const result<void> settled = settle(ec, 0); !settled.has_value()) {
            return settled.error();
        }
        ended_ = true;
        // The text began, so on_document_begin made the document, or the text failed.
        result<basic_document<Traits, Storage>> made(
            std::move(*document_));  // NOLINT(bugprone-unchecked-optional-access)
        document_.reset();
        return made;
    }

    /** Makes the parser ready for another text, its selectors kept. */
    void reset() noexcept {
        json_.reset();
        json_.handler().reset();
        document_.reset();
        failure_ = {};
        cause_ = {};
        consumed_ = 0;
        given_ = 0;
        ended_ = false;
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
        thrown_ = nullptr;
#endif
    }

    /**
     How many bytes of the text were consumed.

     @return The count; after an @ref errc::syntax, the offset where the error was found.
    */
    [[nodiscard]] std::uint64_t offset() const noexcept { return consumed_; }

    /**
     The code of Boost.JSON behind an @ref errc::syntax.

     @return The code, or none.
    */
    [[nodiscard]] boost::system::error_code cause() const noexcept { return cause_; }

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
     @see "Selectors", in the guide.
    */
    template <class F>
    result<void> on(std::string_view selector, F&& f)
        requires detail::selector_callback<std::decay_t<F>, Traits, Storage>
    {
        return subscribe(selector, std::forward<F>(f), true);
    }

    /**
     Delivers each value whose path matches `selector` once it completes, and keeps it.

     @tparam F A callable taking a read-only handle and a @ref path_view, returning `void` or
     `result<void>`.
     @param selector The selector, as @ref on takes it.
     @param f The callback.
     @return Success; @ref errc::type for a malformed selector.
     @see "Selectors", in the guide.
    */
    template <class F>
    result<void> observe(std::string_view selector, F&& f)
        requires detail::selector_callback<std::decay_t<F>, Traits, Storage>
    {
        return subscribe(selector, std::forward<F>(f), false);
    }

private:
    template <class, class>
    friend class detail::handler;

    template <class>
    friend class basic_stream_parser;

    /**
     Feeds a part of the text and stops where the text's value ends, as a stream of documents
     reads one document after another; returns how many bytes it used.

     The part is not empty. Its bytes past `max_bytes` are not fed: a text not complete within the
     limit is @ref errc::capacity.
    */
    result<std::size_t> write_part(std::string_view part) {
        if (!failure_.has_value()) {
            return failed().error();
        }
        const std::uint64_t room = options_.max_bytes - given_;
        const std::string_view fed =
            part.substr(0, static_cast<std::size_t>(std::min<std::uint64_t>(room, part.size())));
        if (fed.empty()) {
            failure_ = errc::capacity;
            return failure_.error();
        }
        boost::system::error_code ec;
        const std::size_t used = json_.write_some(true, fed.data(), fed.size(), ec);
        given_ += used;
        if (const result<void> settled = settle(ec, used); !settled.has_value()) {
            return settled.error();
        }
        if (!json_.done() && fed.size() < part.size()) {
            failure_ = errc::capacity;
            return failure_.error();
        }
        return used;
    }

    /** Whether the text's value has ended, so that @ref take may take its document. */
    [[nodiscard]] bool complete() const noexcept { return json_.done(); }

    /** Takes the document of the value that ended, and makes the parser ready for another text. */
    basic_document<Traits, Storage> take() noexcept {
        // complete() holds, so on_document_begin made the document.
        basic_document<Traits, Storage> made(
            std::move(*document_));  // NOLINT(bugprone-unchecked-optional-access)
        reset();
        return made;
    }

    /** Boost.JSON's options: numbers left as text, and the depth limit. */
    static boost::json::parse_options json_options(const parse_options& o) noexcept {
        boost::json::parse_options made;
        made.numbers = boost::json::number_precision::none;
        made.max_depth = static_cast<std::size_t>(
            std::min<std::uint64_t>(o.max_depth, std::numeric_limits<std::size_t>::max()));
        return made;
    }

    /** The error a failed parser returns again, or, with exceptions, the exception it rethrows. */
    [[nodiscard]] result<void> failed() const {
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
        if (thrown_) {
            std::rethrow_exception(thrown_);
        }
#endif
        return failure_;
    }

    /** Text after the end of the text, which fails the parser. */
    result<void> end_again() noexcept {
        failure_ = errc::syntax;
        cause_ = boost::json::error::extra_data;
        return failure_;
    }

    /**
     Counts what Boost.JSON consumed and turns its error into the parser's: the one the handler
     recorded (a tree's, a callback's, or a callback's exception, rethrown), else `too_deep` as
     @ref errc::capacity, `exponent_overflow` as @ref errc::range, and any other as
     @ref errc::syntax, its code kept as the cause.
    */
    result<void> settle(boost::system::error_code ec, std::size_t used) {
        consumed_ += used;
        if (!ec) {
            return {};
        }
        if (!failure_.has_value()) {
            return failed();
        }
        if (ec == boost::json::error::too_deep) {
            failure_ = errc::capacity;
        } else if (ec == boost::json::error::exponent_overflow) {
            failure_ = errc::range;
        } else {
            failure_ = errc::syntax;
        }
        cause_ = ec;
        return failure_;
    }

    /** Adds a subscription, its callback's `void` made success. */
    template <class F>
    result<void> subscribe(std::string_view text, F&& f, bool take) {
        result<detail::selector> parsed = detail::selector::parse(text);
        if (!parsed.has_value()) {
            return parsed.error();
        }
        using callback = std::decay_t<F>;
        detail::subscription<Traits, Storage> added;
        added.where = std::move(parsed).unsafe_value();
        added.take = take;
        if constexpr (std::same_as<std::invoke_result_t<
                                       callback&, basic_const_value<Traits, Storage>, path_view>,
                                   void>) {
            added.deliver = [g = callback(std::forward<F>(f))](
                                const basic_const_value<Traits, Storage>& v,
                                path_view where) mutable -> result<void> {
                g(v, where);
                return {};
            };
        } else {
            added.deliver = callback(std::forward<F>(f));
        }
        subscriptions_.push_back(std::move(added));
        return {};
    }

    parse_options options_;
    std::span<std::byte> buffer_;
    std::optional<basic_document<Traits, Storage>> document_;
    std::vector<detail::subscription<Traits, Storage>> subscriptions_;
    boost::json::basic_parser<detail::handler<Traits, Storage>> json_;
    result<void> failure_;
    boost::system::error_code cause_;
    std::uint64_t consumed_ = 0;
    std::uint64_t given_ = 0;
    bool ended_ = false;
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
    std::exception_ptr thrown_;
#endif
};

/** The streaming parser of a @ref document. */
using parser = basic_parser<>;

}  // namespace webcpp::document

#include <webcpp/document/detail/handler.hpp>

#endif  // WEBCPP_DOCUMENT_PARSER_HPP
