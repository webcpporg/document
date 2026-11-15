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

#include <webcpp/document/detail/allocation.hpp>
#include <webcpp/document/detail/fwd.hpp>
#include <webcpp/document/detail/selector.hpp>
#include <webcpp/document/document.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/value.hpp>

#include <boost/config.hpp>
#include <boost/json/basic_parser_impl.hpp>
#include <boost/json/error.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
#include <exception>
#include <new>
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

/**
 A selector's callback, its type erased as `std::function` erases it, its object allocated through
 the parser's allocator, rebound, which `std::function` cannot take.

 Its object is the callable a selector was given, decayed, made by `make`; the callback is moved,
 never copied, and calls it as `std::function` does, its `void` made success.
*/
template <class Traits, class Storage>
class delivery {
public:
    /** The allocator of the parser, which holds the callable. */
    using allocator_type = Storage::allocator_type;

    /** The read-only handle a callback is given. */
    using handle = basic_const_value<Traits, Storage>;

    /**
     A callback holding `f`, decayed, in storage allocated through `alloc`; what the allocator or
     the callable's constructor throws propagates, and the storage goes back to `alloc`.
    */
    template <class F>
        requires selector_callback<std::decay_t<F>, Traits, Storage>
    static delivery make(F&& f, const allocator_type& alloc) {
        using held = std::decay_t<F>;
        using held_allocator = rebound<allocator_type, held>;
        using held_traits = std::allocator_traits<held_allocator>;
        held_allocator a(alloc);
        storage_for<held> storage(a);
        held_traits::construct(a, storage.get(), std::forward<F>(f));
        storage.release();
        // A callable that is a function pointer makes its storage a pointer to a pointer.
        return delivery(static_cast<void*>(storage.get()), &operations_of<held>, alloc);
    }

    /** Takes the callable of `other`, which holds none after it. */
    delivery(delivery&& other) noexcept
        : object_(std::exchange(other.object_, nullptr)), ops_(other.ops_), alloc_(other.alloc_) {}

    /** A callback is not copied: it owns its callable. */
    delivery(const delivery&) = delete;

    /** A callback is not copied: it owns its callable. */
    delivery& operator=(const delivery&) = delete;

    /** A callback is not assigned: a polymorphic allocator, which it may hold, is not. */
    delivery& operator=(delivery&&) = delete;

    /** Destroys the callable and gives its storage back to the allocator. */
    ~delivery() {
        if (object_ != nullptr) {
            ops_->destroy(object_, alloc_);
        }
    }

    /**
     Calls the callable with a value and its path, as `std::function` would: with a copy of the
     handle, its `void` made success; what it throws propagates.
    */
    result<void> operator()(const handle& v, path_view where) {
        return ops_->call(object_, v, where);
    }

private:
    /** What a callable of one type does: be called, and be destroyed with its storage. */
    struct operations {
        /** Calls the callable at `object` with a value and its path. */
        result<void> (*call)(void* object, const handle& v, path_view where);

        /** Destroys the callable at `object` and gives its storage back to `alloc`. */
        void (*destroy)(void* object, const allocator_type& alloc) noexcept;
    };

    /**
     Storage for one callable of type `T`, allocated through an allocator, which goes back to it
     unless it is released once the callable is built in it.
    */
    template <class T>
    class storage_for {
    public:
        /** The allocator's traits, rebound to `T`. */
        using traits = std::allocator_traits<rebound<allocator_type, T>>;

        /** Storage for one `T` from `allocator`, which outlives it; what the allocator throws. */
        explicit storage_for(rebound<allocator_type, T>& allocator)
            : allocator_(&allocator), place_(traits::allocate(allocator, 1)) {}

        /** The storage is the one being made: it is not copied. */
        storage_for(const storage_for&) = delete;

        /** The storage is the one being made: it is not copied. */
        storage_for& operator=(const storage_for&) = delete;

        /** The storage is the one being made: it is not moved. */
        storage_for(storage_for&&) = delete;

        /** The storage is the one being made: it is not moved. */
        storage_for& operator=(storage_for&&) = delete;

        /** Gives the storage back unless it was released. */
        ~storage_for() {
            if (!released_) {
                traits::deallocate(*allocator_, place_, 1);
            }
        }

        /** The storage's address. */
        [[nodiscard]] T* get() const noexcept { return std::to_address(place_); }

        /** Keeps the storage, which the callable built in it now owns. */
        void release() noexcept { released_ = true; }

    private:
        /** The allocator. */
        rebound<allocator_type, T>* allocator_;

        /** The storage. */
        traits::pointer place_;

        /** Whether the storage is kept. */
        bool released_ = false;
    };

    /**
     Calls the callable of type `F` at `object` with a copy of the handle, as `std::function`
     passes its argument, so that a callable taking the handle by value or as an rvalue is called
     as one taking a const reference is; its `void` made success.
    */
    template <class F>
    static result<void> call_as(void* object, const handle& v, path_view where) {
        F& f = *static_cast<F*>(object);
        if constexpr (std::same_as<std::invoke_result_t<F&, handle, path_view>, void>) {
            std::invoke(f, handle(v), where);
            return {};
        } else {
            return std::invoke(f, handle(v), where);
        }
    }

    /** Destroys the callable of type `F` at `object` and gives its storage back to `alloc`. */
    template <class F>
    static void destroy_as(void* object, const allocator_type& alloc) noexcept {
        using held_allocator = rebound<allocator_type, F>;
        using held_traits = std::allocator_traits<held_allocator>;
        held_allocator a(alloc);
        F* const f = static_cast<F*>(object);
        const auto place = std::pointer_traits<typename held_traits::pointer>::pointer_to(*f);
        held_traits::destroy(a, f);
        held_traits::deallocate(a, place, 1);
    }

    /** The operations of a callable of type `F`. */
    template <class F>
    static constexpr operations operations_of{.call = &call_as<F>, .destroy = &destroy_as<F>};

    /** A callback of the callable at `object`, built, whose operations are `ops`. */
    delivery(void* object, const operations* ops, const allocator_type& alloc) noexcept
        : object_(object), ops_(ops), alloc_(alloc) {}

    /** The callable, in storage of the allocator; null once moved from. */
    void* object_;

    /** What the callable does. */
    const operations* ops_;

    /** The allocator of the callable's storage. */
    BOOST_ATTRIBUTE_NO_UNIQUE_ADDRESS allocator_type alloc_;
};

/** A selector and its callback, as `on` or `observe` registered it, in the parser's allocator. */
template <class Traits, class Storage>
struct subscription {
    /** The places it matches. */
    basic_selector<typename Storage::allocator_type> where;

    /**
     Whether the value is dropped after its delivery, as `on` asks, or kept, as `observe` does.
    */
    bool take = false;

    /** The callback, its `void` made success. */
    delivery<Traits, Storage> deliver;
};

}  // namespace detail

/**
 The streaming parser: `write` takes the text in chunks, `finish` returns the document.

 Boost.JSON's `basic_parser` is fed each chunk, and its handler builds the document node by node,
 by ids, so that the document may grow and move between chunks. A failed `write` leaves the parser
 failed: every later call returns the same error, until @ref reset. The parser raises nothing of
 its own, since its input is the network's; an exception a callback throws propagates from `write`
 or `finish`, and leaves the parser failed: every later call throws it again, until @ref reset.

 Its working state, the handler's frames, path and pending text, the selectors and their
 callbacks, is allocated through the document's allocator, rebound. With exceptions, an allocator
 that has no room is @ref errc::capacity, and fails the parser; without them it ends the program,
 as the allocator itself does. A text given whole to @ref finish_with never suspends Boost.JSON's
 parser, and allocates nothing else. A text fed in chunks by @ref write suspends it at each
 chunk's end, and Boost.JSON keeps its suspension stack in the global heap, which no allocator
 reaches: one allocation at the first suspension, and one more at each suspension deeper than
 every earlier one, the last kept until the parser is destroyed, across @ref reset.

 @tparam Traits The traits of the document.
 @tparam Storage @ref basic_owned or @ref basic_external of an allocator of `std::byte`: @ref owned
 and `pmr::owned`, or @ref external and `pmr::external` to build it in a buffer of its user's. The
 allocator holds an owned document's image and the parser's working state.
 @see "Parsing", in the guide.
*/
template <class Traits, class Storage>
class basic_parser {
public:
    /** The allocator of the document and of the parser's working state. */
    using allocator_type = Storage::allocator_type;

    /**
     A parser of an owned document.

     @param o The limits and the revivers.
     @param alloc The allocator of the document it makes and of its own working state.
    */
    explicit basic_parser(const parse_options& o = parse_options(),
                          const allocator_type& alloc = allocator_type()) noexcept
        requires detail::owned_storage<Storage>
        : options_(o),
          alloc_(alloc),
          subscriptions_(subscriptions_allocator(alloc)),
          json_(json_options(o), *this) {}

    /**
     A parser of a document in a buffer its user owns, formatted when the text begins.

     @param buffer The buffer, 16-aligned, which outlives the parser and its document.
     @param o The limits and the revivers.
     @param alloc The allocator of the document's working state and of the parser's own.
    */
    explicit basic_parser(std::span<std::byte> buffer, const parse_options& o = parse_options(),
                          const allocator_type& alloc = allocator_type()) noexcept
        requires detail::external_storage<Storage>
        : options_(o),
          alloc_(alloc),
          buffer_(buffer),
          subscriptions_(subscriptions_allocator(alloc)),
          json_(json_options(o), *this) {}

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
     @ref errc::range for a number the traits refuse, @ref errc::capacity past a limit or, with
     exceptions, when the allocator has no room, or the error a callback returned.
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
        const std::size_t used = feed(true, chunk, ec);
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
    result<basic_document<Traits, Storage>> finish() { return finish_with(std::string_view()); }

    /**
     Feeds the last chunk of the text, ends the text and returns the document, without the values
     `on` took: what @ref write of `last` then @ref finish return.

     Boost.JSON's parser is told that no text follows, so that it never suspends: a text given
     whole, to a parser that was given none before, allocates nothing outside the allocator, where
     @ref write would have Boost.JSON allocate its suspension stack. After it the parser takes no
     more text, which is @ref errc::syntax, until @ref reset.

     @param last The last chunk: the whole text, or what follows the chunks @ref write took.
     @return The document; the errors of @ref write, and @ref errc::syntax for text that ends
     early.
    */
    result<basic_document<Traits, Storage>> finish_with(std::string_view last) {
        if (!failure_.has_value()) {
            return failed().error();
        }
        if (ended_) {
            return end_again().error();
        }
        if (last.size() > options_.max_bytes - given_) {
            failure_ = errc::capacity;
            return failure_.error();
        }
        given_ += last.size();
        // No more text, as Boost.JSON's own stream_parser ends one.
        boost::system::error_code ec;
        const std::size_t used = feed(false, last, ec);
        // A second text after the first, which write finds the same way.
        if (!ec && used < last.size()) {
            ec = boost::json::error::extra_data;
        }
        if (const result<void> settled = settle(ec, used); !settled.has_value()) {
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

     The selector and the callback, decayed, are held in the allocator.

     @tparam F A callable taking a read-only handle and a @ref path_view, returning `void` or
     `result<void>`.
     @param selector The selector.
     @param f The callback.
     @return Success; @ref errc::type for a malformed selector, and with exceptions
     @ref errc::capacity when the allocator has no room, the parser then unchanged.
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
     @return Success; @ref errc::type for a malformed selector, and with exceptions
     @ref errc::capacity when the allocator has no room, the parser then unchanged.
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
        const std::size_t used = feed(true, fed, ec);
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

    /**
     Feeds Boost.JSON's parser a chunk, `more` telling whether text follows; returns how many bytes
     it used.

     With exceptions, an allocation that fails, of the handler in the allocator or of Boost.JSON's
     suspension stack, propagates out of Boost.JSON's parser, which lets its handler throw and
     fails itself: `std::bad_alloc`, a type derived from it, or Boost.Interprocess's `bad_alloc`.
     It is @ref errc::capacity, which fails the parser until @ref reset.
    */
    std::size_t feed(bool more, std::string_view chunk, boost::system::error_code& ec) {
        return detail::unless_allocation_fails(
            [&] { return json_.write_some(more, chunk.data(), chunk.size(), ec); },
            [&]() -> std::size_t {
                failure_ = errc::capacity;
                ec = failure_.error();
                return 0;
            });
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

    /** A subscription of this parser. */
    using subscription_type = detail::subscription<Traits, Storage>;

    /** The allocator of the subscriptions. */
    using subscriptions_allocator = detail::rebound<allocator_type, subscription_type>;

    /**
     Adds a subscription, its selector and its callback in the allocator; with exceptions, an
     allocator with no room is @ref errc::capacity, and adds nothing.
    */
    template <class F>
    result<void> subscribe(std::string_view text, F&& f, bool take) {
        return detail::unless_allocation_fails([&] { return add(text, std::forward<F>(f), take); },
                                               [] { return result<void>(errc::capacity); });
    }

    /**
     Adds a subscription, made whole before it is added, so that what fails to allocate leaves
     the subscriptions as they were.
    */
    template <class F>
    result<void> add(std::string_view text, F&& f, bool take) {
        result<detail::basic_selector<allocator_type>> parsed =
            detail::basic_selector<allocator_type>::parse(text, alloc_);
        if (!parsed.has_value()) {
            return parsed.error();
        }
        subscriptions_.push_back(subscription_type{
            .where = std::move(parsed).unsafe_value(),
            .take = take,
            .deliver = detail::delivery<Traits, Storage>::make(std::forward<F>(f), alloc_),
        });
        return {};
    }

    parse_options options_;
    BOOST_ATTRIBUTE_NO_UNIQUE_ADDRESS allocator_type alloc_;
    std::span<std::byte> buffer_;
    std::optional<basic_document<Traits, Storage>> document_;
    std::vector<subscription_type, subscriptions_allocator> subscriptions_;
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

namespace pmr {

/**
 The streaming parser of a `pmr::document`, its working state in the document's
 `std::pmr::memory_resource`.

 @see "Allocators", in the guide.
*/
using parser = webcpp::document::basic_parser<default_traits, owned>;

}  // namespace pmr

}  // namespace webcpp::document

#include <webcpp/document/detail/handler.hpp>

#endif  // WEBCPP_DOCUMENT_PARSER_HPP
