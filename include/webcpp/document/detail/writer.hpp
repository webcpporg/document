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

 Its working state, the stack and the piece `stringify_to` holds back, is in the document's
 allocator, rebound, and is the only thing it allocates besides its output.
*/

#ifndef WEBCPP_DOCUMENT_DETAIL_WRITER_HPP
#define WEBCPP_DOCUMENT_DETAIL_WRITER_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/allocation.hpp>
#include <webcpp/document/detail/tree.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/number.hpp>
#include <webcpp/document/quote.hpp>
#include <webcpp/document/storage.hpp>
#include <webcpp/document/traits.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace webcpp::document::detail {

/** The most spaces of one level: `JSON.stringify` clamps its third argument to 10. */
inline constexpr std::uint32_t max_indent = 10;

/**
 The frames the writer's stack has room for at first, 16 bytes each: a document nested 16 deep
 is written with one allocation of its stack.
*/
inline constexpr std::size_t first_frames = 16;

/**
 The room the piece `stringify_to` holds back takes at first when its chunk is larger: a chunk of
 the default size or smaller is allocated once, at its size, and a larger one, which may be larger
 than the whole text, grows by doubling to what the text needs.
*/
inline constexpr std::size_t first_piece_room = 4096;

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

/**
 Runs `allocate`, which allocates through the writer's allocator: with exceptions, the allocator's
 failure (@ref unless_allocation_fails) is @ref errc::capacity; without, an allocator that fails
 ends the program, as it does itself.
*/
template <class Allocate>
result<void> allocating(Allocate&& allocate) {
    return unless_allocation_fails(
        [&]() -> result<void> {
            std::forward<Allocate>(allocate)();
            return {};
        },
        [] { return result<void>(errc::capacity); });
}

/** The output of `stringify`: the text appended to a string. */
class string_output {
public:
    /** An output that appends to `text`, which outlives it. */
    explicit string_output(std::string& text) noexcept : text_(&text) {}

    /** Appends `text`; the string's `std::bad_alloc` propagates, the output's only failure. */
    result<void> put(std::string_view text) {
        text_->append(text);
        return {};
    }

private:
    /** The string appended to. */
    std::string* text_;
};

/**
 The output of `stringify_to`: the text handed to a sink in pieces of at most `chunk` bytes, the
 piece it holds back until it is full in `Allocator`, rebound.
*/
template <class Sink, class Allocator>
class chunked_output {
public:
    /**
     An output to `sink`, which outlives it, in pieces of at most `chunk` bytes, at least 1, the
     piece it holds back in `alloc`.
    */
    chunked_output(Sink& sink, std::uint64_t chunk, const Allocator& alloc) noexcept
        // A chunk of 0 would never hand anything over; one past the address space is all of it.
        : sink_(&sink),
          chunk_(static_cast<std::size_t>(
              std::clamp<std::uint64_t>(chunk, 1, std::numeric_limits<std::size_t>::max()))),
          pending_(piece_allocator(alloc)) {}

    /**
     Takes `text`, handing the sink each full piece; the sink's failure is returned, and with
     exceptions @ref errc::capacity when the allocator has no room for the piece held back.
    */
    result<void> put(std::string_view text) {
        while (!text.empty()) {
            // A full piece of the text itself is handed over without a copy.
            if (pending_.empty() && text.size() >= chunk_) {
                if (const result<void> handed = hand(text.substr(0, chunk_)); !handed) {
                    return handed;
                }
                text.remove_prefix(chunk_);
                continue;
            }
            const std::size_t taken = std::min(chunk_ - pending_.size(), text.size());
            if (const result<void> room = make_room(pending_.size() + taken); !room) {
                return room;
            }
            pending_.append(text.substr(0, taken));
            text.remove_prefix(taken);
            if (pending_.size() == chunk_) {
                const result<void> handed = hand(pending_);
                pending_.clear();
                if (!handed) {
                    return handed;
                }
            }
        }
        return {};
    }

    /** Hands the sink what is left; the sink's failure is returned. */
    result<void> flush() {
        if (pending_.empty()) {
            return {};
        }
        const result<void> handed = hand(pending_);
        pending_.clear();
        return handed;
    }

private:
    /** The allocator of the piece held back. */
    using piece_allocator = rebound<Allocator, char>;

    /** Hands one piece to the sink, a sink returning `void` succeeding. */
    result<void> hand(std::string_view piece) {
        if constexpr (std::same_as<std::invoke_result_t<Sink&, std::string_view>, void>) {
            std::invoke(*sink_, piece);
            return {};
        } else {
            return std::invoke(*sink_, piece);
        }
    }

    /**
     Makes room in the piece for `size` bytes, at most the chunk, before it takes them: the smaller
     of the chunk and @ref first_piece_room the first time, then twice the room, up to the chunk,
     so that each growth is one allocation.
    */
    result<void> make_room(std::size_t size) {
        if (size <= pending_.capacity()) {
            return {};
        }
        std::size_t room = room_ == 0 ? std::min(chunk_, first_piece_room) : room_;
        while (room < size) {
            room = room > chunk_ / 2 ? chunk_ : 2 * room;
        }
        if (const result<void> made = allocating([this, room] { pending_.reserve(room); }); !made) {
            return made;
        }
        room_ = room;
        return {};
    }

    /** The sink, used in place. */
    Sink* sink_;

    /** The largest piece. */
    std::size_t chunk_;

    /** The room the piece was last given, 0 before it was given any. */
    std::size_t room_ = 0;

    /** The text not yet handed over, shorter than a chunk. */
    std::basic_string<char, std::char_traits<char>, piece_allocator> pending_;
};

/**
 The walk of @ref write_json over one subtree, which keeps the containers it is inside on a stack
 of its own, in `Allocator`, rebound: depth is not bounded by the API, and a recursion would be
 bounded by the thread's stack.

 Tip: the stack holds ids, never nodes, and each step finds its node again, so that nothing the
 walk holds points into the image across a call of the output.
*/
template <class Traits, class Out, class Allocator>
class json_writer {
public:
    /**
     A walk of `t` writing to `out` with `indent` spaces a level, already clamped, its stack in
     `alloc`.
    */
    json_writer(const tree_view<Traits>& t, std::uint32_t indent, Out& out,
                const Allocator& alloc) noexcept
        : tree_(&t), indent_(indent), out_(&out), stack_(frame_allocator(alloc)) {}

    /**
     Writes the subtree of `root`, a node that is not `undefined`; the output's failure ends it,
     and with exceptions @ref errc::capacity when the allocator has no room for the stack.
    */
    result<void> run(const node<Traits>& root) {
        write_value(root);
        while (status_ && !stack_.empty()) {
            step();
        }
        return status_;
    }

private:
    /** A container the walk is inside. */
    struct frame {
        /** The id of the next child to write, or 0 when every child is written. */
        std::uint64_t next = 0;

        /** Whether it is an object, else an array. */
        bool object = false;

        /** Whether no child is written yet. */
        bool first = true;
    };

    /** The allocator of the stack. */
    using frame_allocator = rebound<Allocator, frame>;

    /** Whether strings and keys are stored as UTF-16 code units. */
    static constexpr bool utf16_text = std::same_as<typename Traits::string, utf16>;

    /** Writes the next child of the innermost container, or closes it. */
    void step() {
        frame& top = stack_.back();
        if (top.next == 0) {
            const bool object = top.object;
            stack_.pop_back();
            new_line(stack_.size());
            put(object ? "}" : "]");
            return;
        }
        const node<Traits>* child = tree_->find(top.next);
        if (child == nullptr) {
            // The document changed under the walk; what is left of the container is not written.
            top.next = 0;
            return;
        }
        const bool first = top.first;
        const bool object = top.object;
        const node<Traits>* after = tree_->next_sibling(*child);
        top.first = false;
        top.next = after == nullptr ? 0 : after->id;
        // `top` is not used past here: writing a container pushes a frame, which may move it.
        if (!first) {
            put(",");
        }
        new_line(stack_.size());
        if (object) {
            write_key(*child);
            put(indent_ == 0 ? ":" : ": ");
        }
        write_value(*child);
    }

    /** Writes a scalar whole, or opens a container and pushes its frame. */
    void write_value(const node<Traits>& n) {
        switch (n.kind) {
            case webcpp::document::kind::undefined:
            case webcpp::document::kind::null: put("null"); return;
            case webcpp::document::kind::boolean:
                put(n.template load<typename Traits::boolean>() ? "true" : "false");
                return;
            case webcpp::document::kind::number: write_number(n); return;
            case webcpp::document::kind::string: write_string(n); return;
            case webcpp::document::kind::date: write_date_of(n); return;
            case webcpp::document::kind::bytes: write_bytes(n); return;
            case webcpp::document::kind::array: open(n, false); return;
            case webcpp::document::kind::object: open(n, true); return;
        }
    }

    /**
     Opens a container: `[]` or `{}` when it is empty, else its frame, then its opener, so that a
     stack with no room writes nothing of it.
    */
    void open(const node<Traits>& n, bool object) {
        if (!status_) {
            return;
        }
        const node<Traits>* first = tree_->first_child(n.id);
        if (first == nullptr) {
            put(object ? "{}" : "[]");
            return;
        }
        if (push(frame{.next = first->id, .object = object, .first = true})) {
            put(object ? "{" : "[");
        }
    }

    /**
     Pushes a frame, room made first: @ref first_frames frames, then twice the room each time it is
     full; with exceptions, an allocator with no room stops the writing with @ref errc::capacity.
    */
    bool push(const frame& f) {
        if (stack_.size() == stack_.capacity()) {
            const std::size_t room = stack_.capacity() == 0 ? first_frames : 2 * stack_.capacity();
            if (const result<void> made = allocating([this, room] { stack_.reserve(room); });
                !made) {
                status_ = made;
                return false;
            }
        }
        // There is room: it does not allocate.
        stack_.push_back(f);
        return true;
    }

    /** Writes a number as JavaScript's Number::toString writes the traits' `number`. */
    void write_number(const node<Traits>& n) {
        using number = Traits::number;
        using codec = number_codec<number>;
        if (n.number != repr::integer) {
            put(codec::write(n.template load<number>()).view());
            return;
        }
        const auto integer = n.template load<typename Traits::integer>();
        if constexpr (std::floating_point<number>) {
            // The integer is the value of a binary number, written with that number's shortest
            // digits, as JavaScript writes 2^62 as 4611686018427388000.
            put(codec::write(codec::from_int64(integer)).view());
        } else {
            put(webcpp::document::write_javascript(webcpp::document::from_int64(integer)).view());
        }
    }

    /** Writes a date as Date.prototype.toJSON does, quoted. */
    void write_date_of(const node<Traits>& n) {
        put("\"");
        put(write_date(n.template load<typename Traits::date>()).view());
        put("\"");
    }

    /** Writes bytes by the traits' policy: base64 or base64url quoted, or an object of bytes. */
    void write_bytes(const node<Traits>& n) {
        const std::span<const std::byte> bytes = n.text.view();
        if constexpr (std::same_as<typename Traits::bytes, javascript_bytes>) {
            write_byte_object(bytes);
        } else {
            constexpr bool url = std::same_as<typename Traits::bytes, base64url>;
            // A part whose size is a multiple of 3 has no padding: only the last one may.
            constexpr std::size_t part_bytes = 192;
            std::array<char, (part_bytes / 3) * 4> text{};
            put("\"");
            for (std::size_t at = 0; at < bytes.size(); at += part_bytes) {
                const std::span<const std::byte> part =
                    bytes.subspan(at, std::min(part_bytes, bytes.size() - at));
                write_base64(part, text.data(), url);
                put({text.data(), static_cast<std::size_t>(base64_size(part.size(), !url))});
            }
            put("\"");
        }
    }

    /**
     Writes bytes as `JSON.stringify` writes a Uint8Array: an object of the keys `"0"` to `"n-1"`,
     indented as any object is.
    */
    void write_byte_object(std::span<const std::byte> bytes) {
        if (bytes.empty()) {
            put("{}");
            return;
        }
        // The value is at the walk's depth, and its entries one level deeper.
        const std::size_t level = stack_.size();
        put("{");
        for (std::size_t i = 0; i < bytes.size() && status_; ++i) {
            if (i != 0) {
                put(",");
            }
            new_line(level + 1);
            put("\"");
            put_decimal(i);
            put(indent_ == 0 ? "\":" : "\": ");
            put_decimal(std::to_integer<std::uint64_t>(bytes[i]));
        }
        new_line(level);
        put("}");
    }

    /** Writes an unsigned integer in decimal. */
    void put_decimal(std::uint64_t value) {
        number_text text;
        put_digits(text, value, 1);
        put(text.view());
    }

    /** Writes a member's key, quoted. */
    void write_key(const node<Traits>& n) {
        if constexpr (utf16_text) {
            quote(n.key.units());
        } else {
            quote(n.key.chars());
        }
    }

    /** Writes a string, quoted. */
    void write_string(const node<Traits>& n) {
        if constexpr (utf16_text) {
            quote(n.text.units());
        } else {
            quote(n.text.chars());
        }
    }

    /**
     QuoteJSONString of a key or a string: UTF-8, valid as the document holds it, or UTF-16.
    */
    template <class Text>
    void quote(Text text) {
        const auto pass = [this](std::string_view piece) {
            put(piece);
            return status_.has_value();
        };
        quote_json(text, pass);
    }

    /** Writes a line break and the spaces of `level` levels; nothing when writing compactly. */
    void new_line(std::size_t level) {
        if (indent_ == 0) {
            return;
        }
        constexpr std::string_view spaces = "                                                ";
        put("\n");
        std::uint64_t left = static_cast<std::uint64_t>(level) * indent_;
        while (left > 0 && status_) {
            const std::size_t n =
                static_cast<std::size_t>(std::min<std::uint64_t>(left, spaces.size()));
            put(spaces.substr(0, n));
            left -= n;
        }
    }

    /** Hands text to the output, unless an earlier failure stopped the writing. */
    void put(std::string_view text) {
        if (!status_ || text.empty()) {
            return;
        }
        status_ = out_->put(text);
    }

    /** The tree read. */
    const tree_view<Traits>* tree_;

    /** The spaces of a level, already clamped. */
    std::uint32_t indent_;

    /** The output written to. */
    Out* out_;

    /** The containers the walk is inside, the innermost last. */
    std::vector<frame, frame_allocator> stack_;

    /** The first failure, which stops the writing, or success. */
    result<void> status_{};
};

/**
 Writes the subtree of `root` as `JSON.stringify(value, null, indent)` does, `indent` clamped to
 @ref max_indent, its stack in `alloc`, rebound; a null `root`, `undefined`, writes nothing.
*/
template <class Traits, class Out, class Allocator>
    requires output<Out>
result<void> write_json(const tree_view<Traits>& t, const node<Traits>* root, std::uint32_t indent,
                        Out& out, const Allocator& alloc) {
    if (root == nullptr || root->kind == webcpp::document::kind::undefined) {
        return {};
    }
    json_writer<Traits, Out, Allocator> writer(t, std::min(indent, max_indent), out, alloc);
    return writer.run(*root);
}

}  // namespace webcpp::document::detail

#endif  // WEBCPP_DOCUMENT_DETAIL_WRITER_HPP
