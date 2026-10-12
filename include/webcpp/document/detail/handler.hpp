// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The handler of Boost.JSON's `basic_parser` that builds a document: each completed value becomes
 an item stored by the tree's operations, and each value a selector matches is delivered when it
 completes.

 Under an object a value is set as a member, a repeated key replacing the value in place, as
 JavaScript does; under an array it is appended; at the top it is the root. The handler keeps the
 text's path of the value being read, keys and indices in the text, which the selectors match and
 `on` does not renumber.
*/

#ifndef WEBCPP_DOCUMENT_DETAIL_HANDLER_HPP
#define WEBCPP_DOCUMENT_DETAIL_HANDLER_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/convert.hpp>
#include <webcpp/document/detail/selector.hpp>
#include <webcpp/document/detail/tree.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/kind.hpp>
#include <webcpp/document/parser.hpp>
#include <webcpp/document/value.hpp>

#include <boost/json/error.hpp>
#include <boost/system/error_code.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
#include <exception>
#endif

namespace webcpp::document::detail {

/** A container being read: its node, its kind, the next element's index in the text. */
struct frame {
    /** The container's node id. */
    std::uint64_t id = 0;

    /** Whether it is an array; else an object. */
    bool is_array = false;

    /** The next element's index in the text, which `on` does not renumber. */
    std::uint64_t next_index = 0;
};

/** The handler of Boost.JSON's `basic_parser` that builds the document of a @ref basic_parser. */
template <class Traits, class Storage>
class handler {
public:
    /** The largest object Boost.JSON hands the handler: no limit of its own. */
    static constexpr std::size_t max_object_size = static_cast<std::size_t>(-1);

    /** The largest array Boost.JSON hands the handler: no limit of its own. */
    static constexpr std::size_t max_array_size = static_cast<std::size_t>(-1);

    /** The largest key Boost.JSON hands the handler: no limit of its own. */
    static constexpr std::size_t max_key_size = static_cast<std::size_t>(-1);

    /** The largest string Boost.JSON hands the handler: no limit of its own. */
    static constexpr std::size_t max_string_size = static_cast<std::size_t>(-1);

    /** The handler of `owner`, which outlives it. */
    explicit handler(basic_parser<Traits, Storage>& owner) noexcept : owner_(&owner) {}

    /** The text begins: the document is made, or formatted in the external buffer. */
    bool on_document_begin(boost::system::error_code& ec) {
        reset();
        result<basic_document<Traits, Storage>> made = make_document();
        if (!made.has_value()) {
            return fail(ec, made.error());
        }
        owner_->document_.emplace(std::move(made).unsafe_value());
        return true;
    }

    /** The text ends. */
    bool on_document_end([[maybe_unused]] boost::system::error_code& ec) { return true; }

    /** An object begins: an empty object is stored and its frame pushed. */
    bool on_object_begin(boost::system::error_code& ec) {
        return begin_container(webcpp::document::kind::object, ec);
    }

    /** An object ends: its frame popped, and the object delivered when a selector matches it. */
    bool on_object_end([[maybe_unused]] std::size_t n, boost::system::error_code& ec) {
        return end_container(ec);
    }

    /** An array begins: an empty array is stored and its frame pushed. */
    bool on_array_begin(boost::system::error_code& ec) {
        return begin_container(webcpp::document::kind::array, ec);
    }

    /** An array ends: its frame popped, and the array delivered when a selector matches it. */
    bool on_array_end([[maybe_unused]] std::size_t n, boost::system::error_code& ec) {
        return end_container(ec);
    }

    /** A part of a key, which the next part or `on_key` completes. */
    bool on_key_part(std::string_view s, [[maybe_unused]] std::size_t n,
                     [[maybe_unused]] boost::system::error_code& ec) {
        key_.append(s);
        return true;
    }

    /** A key, completed by its last part. */
    bool on_key(std::string_view s, [[maybe_unused]] std::size_t n,
                [[maybe_unused]] boost::system::error_code& ec) {
        key_.append(s);
        return true;
    }

    /** A part of a string, which the next part or `on_string` completes. */
    bool on_string_part(std::string_view s, [[maybe_unused]] std::size_t n,
                        [[maybe_unused]] boost::system::error_code& ec) {
        text_.append(s);
        return true;
    }

    /** A string, completed by its last part: stored, or revived as a date or bytes. */
    bool on_string(std::string_view s, [[maybe_unused]] std::size_t n,
                   boost::system::error_code& ec) {
        text_.append(s);
        const parse_options& o = owner_->options_;
        if (const result<void> made = prepare_string(text_, o.revive_dates, o.revive_bytes, value_);
            !made.has_value()) {
            return fail(ec, made.error());
        }
        return scalar(ec);
    }

    /** A part of a number's text, which the next part or the number's callback completes. */
    bool on_number_part(std::string_view s, [[maybe_unused]] boost::system::error_code& ec) {
        text_.append(s);
        return true;
    }

    /** An integer, whose text the handler stores by value; Boost.JSON's value is not used. */
    bool on_int64([[maybe_unused]] std::int64_t i, std::string_view s,
                  boost::system::error_code& ec) {
        return number(s, ec);
    }

    /** An integer, whose text the handler stores by value; Boost.JSON's value is not used. */
    bool on_uint64([[maybe_unused]] std::uint64_t u, std::string_view s,
                   boost::system::error_code& ec) {
        return number(s, ec);
    }

    /**
     A number, whose text the handler stores by value: Boost.JSON's interface names a `double`,
     which `number_precision::none` leaves uncomputed, and the handler never reads it.
    */
    bool on_double([[maybe_unused]] double d, std::string_view s, boost::system::error_code& ec) {
        return number(s, ec);
    }

    /** `true` or `false`. */
    bool on_bool(bool b, boost::system::error_code& ec) {
        if (const result<void> made = prepare(b, value_); !made.has_value()) {
            return fail(ec, made.error());
        }
        return scalar(ec);
    }

    /** `null`. */
    bool on_null(boost::system::error_code& ec) {
        if (const result<void> made = prepare(nullptr, value_); !made.has_value()) {
            return fail(ec, made.error());
        }
        return scalar(ec);
    }

    /** A part of a comment, which JSON refuses before the handler sees one. */
    bool on_comment_part([[maybe_unused]] std::string_view s,
                         [[maybe_unused]] boost::system::error_code& ec) {
        return true;
    }

    /** A comment, which JSON refuses before the handler sees one. */
    bool on_comment([[maybe_unused]] std::string_view s,
                    [[maybe_unused]] boost::system::error_code& ec) {
        return true;
    }

    /** Forgets the text read so far, for another text. */
    void reset() noexcept {
        frames_.clear();
        steps_ = 0;
        key_.clear();
        text_.clear();
    }

private:
    /** The bytes policy whose bytes are an object's shape, revived when the object ends. */
    static constexpr bool bytes_as_objects = std::same_as<typename Traits::bytes, javascript_bytes>;

    /** A new document: owned, within the options' limit, or formatted in the external buffer. */
    result<basic_document<Traits, Storage>> make_document() noexcept {
        if constexpr (std::same_as<Storage, owned>) {
            return basic_document<Traits, owned>::create(owner_->options_.max_document_bytes);
        } else {
            return format<Traits>(owner_->buffer_);
        }
    }

    /** The document being built, which `on_document_begin` made before any other callback. */
    [[nodiscard]] basic_document<Traits, Storage>& document() const noexcept {
        // Boost.JSON calls on_document_begin first, and goes no further when it fails.
        return *owner_->document_;  // NOLINT(bugprone-unchecked-optional-access)
    }

    /** The tree of the document being built. */
    [[nodiscard]] tree<Traits>& tree_of() const noexcept { return *access::tree_of(document()); }

    /** Records `error` as the parser's failure and hands it to Boost.JSON, which stops. */
    bool fail(boost::system::error_code& ec, boost::system::error_code error) noexcept {
        owner_->failure_ = error;
        ec = error;
        return false;
    }

    /** Steps into the value that begins: its key, or its index in the text, joins the path. */
    void enter() {
        if (frames_.empty()) {
            return;
        }
        if (steps_ == path_.size()) {
            path_.emplace_back();
        }
        // A step keeps its key's buffer when it is left, for the next value at its depth.
        path_step& step = path_[steps_++];
        frame& parent = frames_.back();
        step.is_index = parent.is_array;
        if (parent.is_array) {
            step.key.clear();
            step.index = parent.next_index++;
        } else {
            step.key.assign(key_);
            step.index = 0;
            key_.clear();
        }
    }

    /** Steps out of the value that completed. */
    void leave() noexcept {
        if (!frames_.empty()) {
            --steps_;
        }
    }

    /**
     Stores `v` where the value that begins goes: the root, a member set by its key (a repeated key
     replaces the value in its first place), or an element appended; returns its id.
    */
    result<std::uint64_t> place(const item<Traits>& v) noexcept {
        tree<Traits>& t = tree_of();
        if (frames_.empty()) {
            if (const result<void> done = t.assign(root_id, v); !done.has_value()) {
                return done.error();
            }
            return root_id;
        }
        const frame& parent = frames_.back();
        if (!parent.is_array) {
            return t.set_member(parent.id, path_[steps_ - 1].key, v);
        }
        const std::uint64_t at = t.count(parent.id);
        if (const result<void> done = t.insert_elements(parent.id, at, std::span(&v, 1));
            !done.has_value()) {
            return done.error();
        }
        return t.element(parent.id, at)->id;
    }

    /** Stores the scalar prepared in `value_`, then delivers it. */
    bool scalar(boost::system::error_code& ec) {
        enter();
        const result<std::uint64_t> id = place(value_.value());
        // The item of a UTF-8 string points into the pending text, which the tree has now copied.
        text_.clear();
        if (!id.has_value()) {
            return fail(ec, id.error());
        }
        return complete(id.unsafe_value(), ec);
    }

    /** A number's last part completes its text, stored by value. */
    bool number(std::string_view s, boost::system::error_code& ec) {
        text_.append(s);
        if (const result<void> made = prepare_number(text_, value_); !made.has_value()) {
            text_.clear();
            return fail(ec, made.error());
        }
        return scalar(ec);
    }

    /** Stores an empty container where the value that begins goes, and pushes its frame. */
    bool begin_container(webcpp::document::kind k, boost::system::error_code& ec) {
        enter();
        const result<std::uint64_t> id = place(empty_item<Traits>(k));
        if (!id.has_value()) {
            return fail(ec, id.error());
        }
        frames_.push_back(frame{
            .id = id.unsafe_value(),
            .is_array = k == webcpp::document::kind::array,
            .next_index = 0,
        });
        return true;
    }

    /** Pops the container's frame, revives an object of bytes, and delivers the container. */
    bool end_container(boost::system::error_code& ec) {
        const frame done = frames_.back();
        frames_.pop_back();
        if constexpr (bytes_as_objects) {
            if (!done.is_array && owner_->options_.revive_bytes) {
                if (const result<void> revived = revive_bytes(done.id); !revived.has_value()) {
                    return fail(ec, revived.error());
                }
            }
        }
        return complete(done.id, ec);
    }

    /** Turns the object `id` into bytes when it has the shape `JSON.stringify` gives a Uint8Array.
     */
    result<void> revive_bytes(std::uint64_t id) noexcept {
        tree<Traits>& t = tree_of();
        result<std::vector<std::byte>> bytes = read<std::vector<std::byte>>(t, t.find(id));
        if (!bytes.has_value()) {
            return {};
        }
        prepared<Traits> made;
        if (const result<void> held =
                prepare(std::span<const std::byte>(bytes.unsafe_value()), made);
            !held.has_value()) {
            return held;
        }
        return t.assign(id, made.value());
    }

    /** Delivers the value that completed, then steps out of it. */
    bool complete(std::uint64_t id, boost::system::error_code& ec) {
        const result<void> delivered = deliver(id);
        leave();
        if (!delivered.has_value()) {
            return fail(ec, delivered.error());
        }
        return true;
    }

    /**
     Delivers node `id`, the value that completed, to each subscription its path matches, in the
     order they were added, then takes it from the document when an `on` matched.
    */
    result<void> deliver(std::uint64_t id) {
        const std::span<const path_step> path(path_.data(), steps_);
        bool take = false;
        for (subscription<Traits, Storage>& s : owner_->subscriptions_) {
            if (!s.where.matches(path)) {
                continue;
            }
            if (const result<void> called = call(s, id, path); !called.has_value()) {
                return called;
            }
            take = take || s.take;
        }
        if (!take) {
            return {};
        }
        return take_out();
    }

    /**
     Calls a subscription's callback with a read-only handle of node `id`; with exceptions, an
     exception it throws is kept for `write` or `finish` to rethrow, and fails the parser.
    */
    result<void> call(subscription<Traits, Storage>& s, std::uint64_t id,
                      std::span<const path_step> path) {
        const basic_const_value<Traits, Storage> v = access::handle_of(document(), id);
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
        try {
            return s.deliver(v, path_view(path));
        } catch (...) {
            owner_->thrown_ = std::current_exception();
            return boost::json::make_error_code(boost::json::error::exception);
        }
#else
        return s.deliver(v, path_view(path));
#endif
    }

    /**
     Takes the value that completed from the document, its blocks zeroed and freed: an element is
     the array's last, a member is found by its key, and the root is left holding `undefined`.
    */
    result<void> take_out() noexcept {
        tree<Traits>& t = tree_of();
        if (frames_.empty()) {
            return t.assign(root_id, empty_item<Traits>(webcpp::document::kind::undefined));
        }
        const frame& parent = frames_.back();
        if (parent.is_array) {
            return t.erase_elements(parent.id, t.count(parent.id) - 1, 1);
        }
        if (const result<bool> erased = t.erase_member(parent.id, path_[steps_ - 1].key);
            !erased.has_value()) {
            return erased.error();
        }
        return {};
    }

    /** The parser, whose document, options and subscriptions the handler uses. */
    basic_parser<Traits, Storage>* owner_;

    /** The containers being read, the innermost last. */
    std::vector<frame> frames_;

    /** The text's path of the value being read, its first `steps_` entries; the rest are spare. */
    std::vector<path_step> path_;

    /** How many entries of `path_` are the path. */
    std::size_t steps_ = 0;

    /** The key being read, or read and waiting for its value. */
    std::string key_;

    /** The string or number being read. */
    std::string text_;

    /** The scalar being stored, kept to reuse its buffers. */
    prepared<Traits> value_;
};

}  // namespace webcpp::document::detail

#endif  // WEBCPP_DOCUMENT_DETAIL_HANDLER_HPP
