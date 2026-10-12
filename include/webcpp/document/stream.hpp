// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 Streams of documents: bytes in, in chunks of any size, and documents out as the bytes are
 consumed, framed as JSON texts one after another, as NDJSON, or as JSON text sequences (RFC 7464).
*/

#ifndef WEBCPP_DOCUMENT_STREAM_HPP
#define WEBCPP_DOCUMENT_STREAM_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/fwd.hpp>
#include <webcpp/document/document.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/parser.hpp>

#include <boost/json/error.hpp>
#include <boost/system/errc.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace webcpp::document {

/**
 How the documents of a stream are told apart.

 @see "Streams of documents", in the guide.
*/
enum class framing : std::uint8_t {
    /**
     Documents one after another, any JSON whitespace or none between them: indented JSON and
     `{"a":1}    {"b":2}` alike.

     A document ends where its value closes, a top-level number at the next byte that cannot
     continue it. Nothing tells where a bad document ends, so a stream of this framing stops at its
     first error.
    */
    concatenated,

    /**
     NDJSON, JSON Lines: one document per line.

     A document ends at its line's `\\n`, which JSON never holds inside a string; JSON whitespace
     around it, `\\r` included, and empty lines are ignored, and the last line may end without one.
    */
    lines,

    /**
     JSON text sequences, RFC 7464: each document in a record that 0x1E opens, which ends before
     the next 0x1E; a document may span lines, and its record holds only whitespace after it.
    */
    rfc7464,
};

/**
 What a stream does with a bad document.

 @see "Streams of documents", in the guide.
*/
enum class on_error : std::uint8_t {
    /** The first error ends the stream: it is returned again, by every call, until a reset. */
    stop,

    /**
     A bad document is reported in its place and dropped, and the stream goes on: with
     @ref framing::lines at the next line, with @ref framing::rfc7464 at the next 0x1E.

     It is not allowed with @ref framing::concatenated, which has no place to go on from.
    */
    skip,
};

/**
 The options of a stream: the limits and the revivers of each of its documents, its framing and
 its policy on errors.

 Each document is held to the limits a parse holds one text to: `max_depth`,
 `max_document_bytes`, and `max_bytes`, which counts the bytes of one document from its first
 byte, not those of the stream.

 @see "Streams of documents", in the guide.
*/
struct stream_options : parse_options {
    /** How the documents are told apart; documents one after another unless told otherwise. */
    webcpp::document::framing framing = framing::concatenated;

    /** What a bad document does; it stops the stream unless told otherwise. */
    webcpp::document::on_error on_error = on_error::stop;
};

namespace detail {

/**
 A callback of the push form of a stream parser: invocable with each document, or the error of
 each document dropped, returning `void` or `result<void>`.
*/
template <class F, class Traits>
concept stream_callback =
    std::invocable<F&, result<basic_document<Traits>>&&> &&
    (std::same_as<std::invoke_result_t<F&, result<basic_document<Traits>>&&>, void> ||
     std::same_as<std::invoke_result_t<F&, result<basic_document<Traits>>&&>, result<void>>);

/** The byte that opens a record of RFC 7464, ASCII's record separator. */
inline constexpr char record_separator = '\x1e';

/** How many of the first bytes of `bytes` are JSON whitespace: space, tab, `\\n` and `\\r`. */
constexpr std::size_t leading_whitespace(std::string_view bytes) noexcept {
    const std::size_t found = bytes.find_first_not_of(" \t\n\r");
    return found == std::string_view::npos ? bytes.size() : found;
}

}  // namespace detail

/**
 The parser of a stream of documents: `write` takes the bytes in chunks of any size, down to one
 byte, and each document is delivered as soon as the bytes that end it are consumed.

 It is sans-IO: it reads no socket or file, so it serves any source, a socket read by hand, an
 actor's messages, or JavaScript through Emscripten; `<webcpp/document/asio.hpp>` reads it from
 Boost.Asio's streams. Each document is parsed as @ref parse parses one text, with the same key
 order, numbers and limits, into a new owned document. The parser reads one document at a time:
 at a document's end it moves it to a queue that @ref next takes from, or hands it to the
 callback of the push form, and begins the next on the bytes that follow, in the same chunk.

 It raises nothing of its own, since its input is the network's: every failure is returned. An
 exception the callback of the push form throws propagates; the bytes up to the document it was
 given are consumed, which @ref offset counts, and the parser takes the rest by another `write`.

 @tparam Traits The traits of the documents.
 @see "Streams of documents", in the guide.
*/
template <class Traits>
class basic_stream_parser {
public:
    /**
     A parser of a stream framed as `o` says.

     `on_error::skip` with `framing::concatenated` is refused: every call of the parser returns
     `boost::system::errc::invalid_argument`.

     @param o The framing, the policy on errors, and the limits and revivers of each document.
    */
    explicit basic_stream_parser(const stream_options& o = {}) : options_(o), inner_(o) {
        refuse_options();
    }

    /** A parser is not copied: its inner parser refers to itself. */
    basic_stream_parser(const basic_stream_parser&) = delete;

    /** A parser is not copied: its inner parser refers to itself. */
    basic_stream_parser& operator=(const basic_stream_parser&) = delete;

    /** A parser is not moved: its inner parser refers to itself. */
    basic_stream_parser(basic_stream_parser&&) = delete;

    /** A parser is not moved: its inner parser refers to itself. */
    basic_stream_parser& operator=(basic_stream_parser&&) = delete;

    /** Destroys the parser, the document it was reading and those @ref next has not taken. */
    ~basic_stream_parser() = default;

    /**
     Consumes the next bytes of the stream, queueing each document they complete for @ref next.

     @param bytes The bytes: any split of the stream, down to one byte.
     @return How many documents, and dropped documents' errors, it queued; or the error that
     stopped the stream: @ref errc::syntax for malformed text, @ref errc::range for a number the
     traits refuse, @ref errc::capacity past a limit. The documents completed before it stay
     queued.
    */
    result<std::size_t> write(std::string_view bytes) {
        auto queue = [this](entry&& e) -> result<void> {
            queue_.push_back(std::move(e));
            return {};
        };
        return consume(bytes, queue);
    }

    /**
     Consumes the next bytes of the stream, handing each document they complete to a callback, in
     order, after those @ref next has not taken.

     @tparam F A callable taking a `result<basic_document<Traits>>`, returning `void` or
     `result<void>`.
     @param bytes The bytes: any split of the stream, down to one byte.
     @param on_document The callback: given each document, and with `on_error::skip` the error of
     each document dropped, in the stream's order. It may not call the parser's `write`, `finish`
     or `reset`. A failure it returns stops the stream with that error.
     @return How many documents, and dropped documents' errors, it handed over; or the error that
     stopped the stream, as @ref write returns it.
    */
    template <class F>
        requires detail::stream_callback<F, Traits>
    result<std::size_t> write(std::string_view bytes, F&& on_document) {
        auto hand = [this, &on_document](entry&& e) {
            return hand_over(std::move(e), on_document);
        };
        if (const result<void> drained = drain(hand); !drained.has_value()) {
            return drained.error();
        }
        return consume(bytes, hand);
    }

    /**
     Takes the oldest document the stream completed.

     @return The document; none when no document is complete; with `on_error::skip`, the error of
     a document dropped, in its place, which @ref error_line names; with `on_error::stop`, once
     the documents before it are taken, the error that stopped the stream, every time.
    */
    result<std::optional<basic_document<Traits>>> next() {
        if (!queue_.empty()) {
            entry e = std::move(queue_.front());
            queue_.pop_front();
            if (!e.item.has_value()) {
                note_error(e);
                return e.item.error();
            }
            ++delivered_;
            return std::optional<basic_document<Traits>>(std::move(e.item).unsafe_value());
        }
        if (!failure_.has_value()) {
            return yield_failure().error();
        }
        return std::optional<basic_document<Traits>>();
    }

    /**
     Ends the stream, closing its last document, a top-level number or a last line without its
     `\\n`, and queueing it for @ref next.

     After it the parser takes no more bytes, which is @ref errc::syntax, until @ref reset; an
     empty `write`, and another `finish`, complete nothing.

     @return How many documents, and dropped documents' errors, it queued; or the error that
     stopped the stream, as @ref write returns it, and @ref errc::syntax for a document that the
     stream's end cuts short.
    */
    result<std::size_t> finish() {
        auto queue = [this](entry&& e) -> result<void> {
            queue_.push_back(std::move(e));
            return {};
        };
        return end(queue);
    }

    /**
     Ends the stream, handing the last document to a callback, after those @ref next has not
     taken.

     @tparam F A callable taking a `result<basic_document<Traits>>`, returning `void` or
     `result<void>`.
     @param on_document The callback, as the push form of @ref write takes it.
     @return How many documents, and dropped documents' errors, it handed over; or the error that
     stopped the stream, as the other `finish` returns it.
    */
    template <class F>
        requires detail::stream_callback<F, Traits>
    result<std::size_t> finish(F&& on_document) {
        auto hand = [this, &on_document](entry&& e) {
            return hand_over(std::move(e), on_document);
        };
        if (const result<void> drained = drain(hand); !drained.has_value()) {
            return drained.error();
        }
        return end(hand);
    }

    /** Makes the parser ready for another stream: the documents not taken and any error dropped. */
    void reset() noexcept {
        inner_.reset();
        queue_.clear();
        failure_ = {};
        failure_at_ = {};
        last_error_ = {};
        phase_ = first_phase();
        offset_ = 0;
        newlines_ = 0;
        ends_line_ = false;
        delivered_ = 0;
        refuse_options();
    }

    /**
     The line of the last byte consumed, from 1; a `\\n` belongs to the line it ends.

     @return The line; 1 before any byte.
    */
    [[nodiscard]] std::uint64_t line() const noexcept {
        return 1 + newlines_ - (ends_line_ ? 1 : 0);
    }

    /**
     How many bytes of the stream were consumed.

     @return The count; after the stream stopped, the offset where its error was found.
    */
    [[nodiscard]] std::uint64_t offset() const noexcept { return offset_; }

    /**
     Whether @ref finish ended the stream, until @ref reset.

     Tip: `async_read_document` completes with `eof` without reading once it holds, for a socket
     reports its end to one read only on Linux's epoll.

     @return `true` after `finish`.
    */
    [[nodiscard]] bool finished() const noexcept { return phase_ == phase::finished; }

    /**
     How many documents were delivered, by @ref next or to a callback.

     @return The count.
    */
    [[nodiscard]] std::uint64_t documents() const noexcept { return delivered_; }

    /**
     The line where the error last delivered was found: by @ref next, to a callback, or returned
     by `write` or `finish`.

     @return The line, from 1; 0 before any error.
    */
    [[nodiscard]] std::uint64_t error_line() const noexcept { return last_error_.line; }

    /**
     The offset in the stream where the error last delivered was found.

     @return The offset of the byte that the error was found at; 0 before any error.
    */
    [[nodiscard]] std::uint64_t error_offset() const noexcept { return last_error_.offset; }

    /**
     The code of Boost.JSON behind the error last delivered, when it is an @ref errc::syntax that
     Boost.JSON's parser found.

     @return The code, `extra_data` for a second document on a line or in a record; or none.
    */
    [[nodiscard]] boost::system::error_code error_cause() const noexcept {
        return last_error_.cause;
    }

private:
    /** Where the bytes stand between documents. */
    enum class phase : std::uint8_t {
        /** rfc7464, before the first record: whitespace, then 0x1E. */
        outside,

        /** No document begun: whitespace, a line's end, or a record's 0x1E. */
        between,

        /** A document being read. */
        document,

        /** rfc7464, the record's document delivered: whitespace until the next 0x1E. */
        tail,

        /** on_error::skip: the rest of a bad document dropped. */
        skipping,

        /** The stream ended. */
        finished,
    };

    /** Where an error was found, and the code of Boost.JSON behind it. */
    struct place {
        /** The line, from 1. */
        std::uint64_t line = 0;

        /** The offset in the stream. */
        std::uint64_t offset = 0;

        /** Boost.JSON's code, or none. */
        boost::system::error_code cause;
    };

    /** A document completed, or a dropped document's error with its place. */
    struct entry {
        /** The document, or the error. */
        result<basic_document<Traits>> item;

        /** The error's place; nothing for a document. */
        place at;
    };

    /** The phase a stream begins in: rfc7464's first byte must open a record. */
    [[nodiscard]] phase first_phase() const noexcept {
        return options_.framing == framing::rfc7464 ? phase::outside : phase::between;
    }

    /** Fails the parser when its options ask to skip in a framing that cannot. */
    void refuse_options() noexcept {
        if (options_.framing == framing::concatenated && options_.on_error == on_error::skip) {
            failure_ = boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
            failure_at_ = {.line = 1, .offset = 0, .cause = {}};
        }
    }

    /** The error that stopped the stream, recorded as the error last delivered. */
    result<void> yield_failure() noexcept {
        last_error_ = failure_at_;
        return failure_;
    }

    /** Records a dropped document's place as the error last delivered. */
    void note_error(const entry& e) noexcept { last_error_ = e.at; }

    /** Where the next byte is: the place of an error found at it. */
    [[nodiscard]] place here(boost::system::error_code cause) const noexcept {
        return {.line = 1 + newlines_, .offset = offset_, .cause = cause};
    }

    /** Hands every document the queue holds to `hand`, oldest first. */
    template <class Hand>
    result<void> drain(Hand& hand) {
        while (!queue_.empty()) {
            entry e = std::move(queue_.front());
            queue_.pop_front();
            if (const result<void> handed = hand(std::move(e)); !handed.has_value()) {
                return yield_failure();
            }
        }
        return {};
    }

    /** Hands a document, or a dropped document's error, to the push form's callback. */
    template <class F>
    result<void> hand_over(entry&& e, F& on_document) {
        if (e.item.has_value()) {
            ++delivered_;
        } else {
            note_error(e);
        }
        using returned = std::invoke_result_t<F&, result<basic_document<Traits>>&&>;
        if constexpr (std::same_as<returned, void>) {
            on_document(std::move(e.item));
            return {};
        } else {
            const result<void> handed = on_document(std::move(e.item));
            if (!handed.has_value()) {
                return stop(handed.error(), here({}));
            }
            return {};
        }
    }

    /** Consumes `bytes`, giving each entry it completes to `sink`; returns how many it gave. */
    template <class Sink>
    result<std::size_t> consume(std::string_view bytes, Sink& sink) {
        if (!failure_.has_value()) {
            return yield_failure().error();
        }
        if (bytes.empty()) {
            return 0;
        }
        if (phase_ == phase::finished) {
            static_cast<void>(stop(errc::syntax, here(boost::json::error::extra_data)));
            return yield_failure().error();
        }
        std::size_t completed = 0;
        while (!bytes.empty()) {
            if (const result<void> stepped = step(bytes, sink, completed); !stepped.has_value()) {
                return yield_failure().error();
            }
        }
        return completed;
    }

    /** Ends the stream, giving the last entry to `sink`; returns how many it gave. */
    template <class Sink>
    result<std::size_t> end(Sink& sink) {
        if (!failure_.has_value()) {
            return yield_failure().error();
        }
        if (phase_ == phase::finished) {
            return 0;
        }
        const bool reading = phase_ == phase::document;
        // Finished before the last document is given, which may throw, and again after a skip.
        phase_ = phase::finished;
        std::size_t completed = 0;
        result<void> closed;
        if (reading) {
            closed = close(sink, completed);
            phase_ = phase::finished;
        }
        if (!closed.has_value()) {
            return yield_failure().error();
        }
        return completed;
    }

    /** Consumes the first bytes of `bytes` as the phase reads them. */
    template <class Sink>
    result<void> step(std::string_view& bytes, Sink& sink, std::size_t& completed) {
        if (phase_ == phase::document) {
            return read(bytes, sink, completed);
        }
        if (phase_ == phase::skipping) {
            skip(bytes);
            return {};
        }
        return separate(bytes, sink, completed);
    }

    /**
     Consumes what lies between documents: whitespace, and with rfc7464 the 0x1E that opens a
     record; a byte that begins a document starts it.
    */
    template <class Sink>
    result<void> separate(std::string_view& bytes, Sink& sink, std::size_t& completed) {
        if (const std::size_t blank = detail::leading_whitespace(bytes); blank != 0) {
            advance(bytes, blank);
            return {};
        }
        if (options_.framing == framing::rfc7464 && bytes.front() == detail::record_separator) {
            advance(bytes, 1);
            phase_ = phase::between;
            return {};
        }
        if (phase_ == phase::between) {
            phase_ = phase::document;
            return {};
        }
        // Text before the first record, or after a record's document in its record.
        const boost::system::error_code cause =
            phase_ == phase::tail ? boost::system::error_code(boost::json::error::extra_data)
                                  : boost::system::error_code();
        return refuse(errc::syntax, cause, sink, completed);
    }

    /**
     Feeds the document being read the bytes up to its line's end or its record's, and delivers it
     once it is complete: at once with concatenated and rfc7464, at its line's end with lines.
    */
    template <class Sink>
    result<void> read(std::string_view& bytes, Sink& sink, std::size_t& completed) {
        const std::string_view part = bytes.substr(0, bound(bytes));
        if (part.empty()) {
            return close_at(bytes, sink, completed);
        }
        if (inner_.complete()) {
            // lines: the document ended, and the rest of its line may hold only whitespace, not a
            // second document.
            const std::size_t blank = detail::leading_whitespace(part);
            advance(bytes, blank);
            if (blank < part.size()) {
                return refuse(errc::syntax, boost::json::error::extra_data, sink, completed);
            }
            return {};
        }
        const std::uint64_t before = inner_.offset();
        const result<std::size_t> used = inner_.write_part(part);
        if (!used.has_value()) {
            advance(bytes, static_cast<std::size_t>(inner_.offset() - before));
            return refuse(used.error(), inner_.cause(), sink, completed);
        }
        advance(bytes, *used);
        // A document of lines waits for its line's end, which the next step looks for.
        if (!inner_.complete() || options_.framing == framing::lines) {
            return {};
        }
        phase_ = options_.framing == framing::concatenated ? phase::between : phase::tail;
        return deliver(inner_.take(), sink, completed);
    }

    /** How many of the first bytes of `bytes` the document being read may hold. */
    [[nodiscard]] std::size_t bound(std::string_view bytes) const noexcept {
        if (options_.framing == framing::concatenated) {
            return bytes.size();
        }
        const char end = options_.framing == framing::lines ? '\n' : detail::record_separator;
        return std::min(bytes.find(end), bytes.size());
    }

    /** Ends the document being read at its line's `\\n` or at the next record's 0x1E. */
    template <class Sink>
    result<void> close_at(std::string_view& bytes, Sink& sink, std::size_t& completed) {
        result<basic_document<Traits>> made = closed();
        if (!made.has_value()) {
            return refuse(made.error(), closing_cause_, sink, completed);
        }
        if (options_.framing == framing::lines) {
            advance(bytes, 1);
            phase_ = phase::between;
        } else {
            phase_ = phase::tail;
        }
        return deliver(std::move(made).unsafe_value(), sink, completed);
    }

    /** Ends the document being read where the stream ends. */
    template <class Sink>
    result<void> close(Sink& sink, std::size_t& completed) {
        result<basic_document<Traits>> made = closed();
        if (!made.has_value()) {
            return refuse(made.error(), closing_cause_, sink, completed);
        }
        return deliver(std::move(made).unsafe_value(), sink, completed);
    }

    /**
     The document being read, ended: taken when its value has closed, else finished, which closes
     a top-level number or fails a document cut short; its cause is kept in `closing_cause_`.
    */
    result<basic_document<Traits>> closed() {
        if (inner_.complete()) {
            closing_cause_ = {};
            return inner_.take();
        }
        result<basic_document<Traits>> made = inner_.finish();
        closing_cause_ = inner_.cause();
        inner_.reset();
        return made;
    }

    /** Drops the bytes of a bad document: the rest of its line, or of its record. */
    void skip(std::string_view& bytes) {
        const char end = options_.framing == framing::lines ? '\n' : detail::record_separator;
        const std::size_t at = bytes.find(end);
        if (at == std::string_view::npos) {
            advance(bytes, bytes.size());
            return;
        }
        // The line's `\n` ends the bad line; the 0x1E opens the next record.
        advance(bytes, at + 1);
        phase_ = phase::between;
    }

    /** Gives a completed document to `sink`. */
    template <class Sink>
    result<void> deliver(basic_document<Traits>&& made, Sink& sink, std::size_t& completed) {
        ++completed;
        return sink(entry{.item = std::move(made), .at = {}});
    }

    /**
     The error `code` at the next byte: it stops the stream, or with `on_error::skip` it is given
     to `sink` and the rest of the bad document is dropped.
    */
    template <class Sink>
    result<void> refuse(boost::system::error_code code, boost::system::error_code cause, Sink& sink,
                        std::size_t& completed) {
        inner_.reset();
        if (options_.on_error == on_error::stop) {
            return stop(code, here(cause));
        }
        phase_ = phase::skipping;
        ++completed;
        return sink(entry{.item = code, .at = here(cause)});
    }

    /** Stops the stream with `code`, found at `at`. */
    result<void> stop(boost::system::error_code code, place at) noexcept {
        failure_ = code;
        failure_at_ = at;
        return failure_;
    }

    /** Consumes the first `n` bytes of `bytes`, counting their lines. */
    void advance(std::string_view& bytes, std::size_t n) noexcept {
        if (n == 0) {
            return;
        }
        const std::string_view::const_iterator first = bytes.begin();
        const std::string_view::const_iterator last = first + static_cast<std::ptrdiff_t>(n);
        newlines_ += static_cast<std::uint64_t>(std::count(first, last, '\n'));
        ends_line_ = *(last - 1) == '\n';
        offset_ += n;
        bytes.remove_prefix(n);
    }

    stream_options options_;
    basic_parser<Traits, owned> inner_;
    std::deque<entry> queue_;
    phase phase_ = first_phase();
    result<void> failure_;
    place failure_at_;
    place last_error_;
    boost::system::error_code closing_cause_;
    std::uint64_t offset_ = 0;
    std::uint64_t newlines_ = 0;
    bool ends_line_ = false;
    std::uint64_t delivered_ = 0;
};

/** The parser of a stream of @ref document values. */
using stream_parser = basic_stream_parser<>;

}  // namespace webcpp::document

#endif  // WEBCPP_DOCUMENT_STREAM_HPP
