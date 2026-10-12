// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 Streams of documents read from Boost.Asio's streams: `async_read_document`, which reads the next
 document of a stream with any completion token, and the images of documents, `async_read_image`,
 which moves an image into a pool's blocks, and `async_write_image` and `async_write_images`, which
 write one document's image or a range's, in one gather write. The one
 header of document that includes Boost.Asio; it builds natively only.
*/

#ifndef WEBCPP_DOCUMENT_ASIO_HPP
#define WEBCPP_DOCUMENT_ASIO_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/heap.hpp>
#include <webcpp/document/document.hpp>
#include <webcpp/document/image_stream.hpp>
#include <webcpp/document/stream.hpp>
#include <webcpp/document/value.hpp>

#include <boost/asio/async_result.hpp>  // lint-world: reads the stream its caller gives
#include <boost/asio/buffer.hpp>        // lint-world: reads the stream its caller gives
#include <boost/asio/compose.hpp>       // lint-world: reads the stream its caller gives
#include <boost/asio/error.hpp>         // lint-world: reads the stream its caller gives
#include <boost/asio/immediate.hpp>     // lint-world: reads the stream its caller gives
#include <boost/assert.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace webcpp::document {

namespace detail {

/**
 A dynamic buffer of Boost.Asio's first model, `DynamicBuffer_v1`, held by reference, with the
 members @ref read_document_op calls.
*/
template <class DynamicBuffer>
class buffer_v1_ref {
public:
    /** The adapter of a buffer the caller keeps alive. */
    explicit buffer_v1_ref(DynamicBuffer& buffer) noexcept : buffer_(&buffer) {}

    /** The number of bytes the buffer holds. */
    [[nodiscard]] std::size_t size() const noexcept { return buffer_->size(); }

    /** The most bytes the buffer may hold. */
    [[nodiscard]] std::size_t max_size() const noexcept { return buffer_->max_size(); }

    /** The number of bytes the buffer holds without reallocating. */
    [[nodiscard]] std::size_t capacity() const noexcept { return buffer_->capacity(); }

    /** The bytes the buffer holds, as a sequence of buffers. */
    [[nodiscard]] auto data() const noexcept { return buffer_->data(); }

    /** The room for `n` more bytes at the end of the buffer. */
    auto prepare(std::size_t n) { return buffer_->prepare(n); }

    /** Moves the first `n` bytes of the prepared room into the bytes the buffer holds. */
    void commit(std::size_t n) { buffer_->commit(n); }

    /** Removes the first `n` bytes the buffer holds. */
    void consume(std::size_t n) { buffer_->consume(n); }

private:
    DynamicBuffer* buffer_;
};

/**
 A dynamic buffer of Boost.Asio's second model, `DynamicBuffer_v2`, held by value as Boost.Asio's
 own reads hold it, with the first model's members made of it: `prepare` grows the buffer by the
 room it returns, and `commit` shrinks it back to the bytes a read gave.
*/
template <class DynamicBuffer>
class buffer_v2_copy {
public:
    /** The adapter of a copy, which names the storage the caller keeps alive. */
    explicit buffer_v2_copy(DynamicBuffer buffer) : buffer_(std::move(buffer)) {}

    /** The number of bytes the buffer holds. */
    [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }

    /** The most bytes the buffer may hold. */
    [[nodiscard]] std::size_t max_size() const noexcept { return buffer_.max_size(); }

    /** The number of bytes the buffer holds without reallocating. */
    [[nodiscard]] std::size_t capacity() const noexcept { return buffer_.capacity(); }

    /** The bytes the buffer holds, as a sequence of buffers. */
    [[nodiscard]] auto data() const noexcept { return buffer_.data(0, buffer_.size()); }

    /** Grows the buffer by `n` bytes and returns the room they make. */
    auto prepare(std::size_t n) {
        const std::size_t first = buffer_.size();
        buffer_.grow(n);
        prepared_ = n;
        return buffer_.data(first, n);
    }

    /** Keeps the first `n` bytes of the room `prepare` made and shrinks the buffer by the rest. */
    void commit(std::size_t n) {
        buffer_.shrink(prepared_ - std::min(n, prepared_));
        prepared_ = 0;
    }

    /** Removes the first `n` bytes the buffer holds. */
    void consume(std::size_t n) { buffer_.consume(n); }

private:
    DynamicBuffer buffer_;
    std::size_t prepared_ = 0;
};

/**
 The composed operation of @ref async_read_document, which takes the parser's next document, and
 reads the stream into the buffer, and the buffer into the parser, only while there is none.
*/
template <class AsyncReadStream, class Buffer, class Traits>
class read_document_op {
public:
    /**
     The operation on a stream, its buffer (an adapter of either model) and its parser; the stream
     and the parser outlive it.
    */
    read_document_op(AsyncReadStream& stream, Buffer buffer, basic_stream_parser<Traits>& parser)
        : stream_(&stream), buffer_(std::move(buffer)), parser_(&parser) {}

    /**
     Runs a step: at the start, when a read completes, or when an immediate completion is
     dispatched.
    */
    template <class Self>
    void operator()(Self& self, boost::system::error_code ec = {}, std::size_t n = 0) {
        if (step_ == step::completing) {
            self.complete(code_, std::move(document_));
            return;
        }
        bool ended = false;
        if (step_ == step::reading) {
            buffer_.commit(n);
            ended = ec == boost::asio::error::eof;
            if (ec && !ended) {
                read_failure_ = ec;
            }
        }
        feed();
        if (ended) {
            // The stream ended: finish delivers what the parser holds, its failure kept for next().
            static_cast<void>(parser_->finish());
        }
        result<std::optional<basic_document<Traits>>> taken = parser_->next();
        if (!taken.has_value()) {
            return complete(self, taken.error(), nothing());
        }
        if (std::optional<basic_document<Traits>>& ready = *taken; ready.has_value()) {
            return complete(self, {}, std::move(*ready));
        }
        // A stream already ended completes with eof without a read: a socket reports its end to
        // one read only, and Asio's epoll reactor does not try a read again after it.
        if (ended || parser_->finished()) {
            return complete(self, boost::asio::error::eof, nothing());
        }
        if (read_failure_) {
            return complete(self, read_failure_, nothing());
        }
        const std::size_t room = read_size();
        if (room == 0) {
            return complete(self, boost::asio::error::no_buffer_space, nothing());
        }
        step_ = step::reading;
        stream_->async_read_some(buffer_.prepare(room), std::move(self));
    }

private:
    /** Where the operation stands. */
    enum class step : std::uint8_t {
        /** Called by the initiating function: no read started yet. */
        starting,

        /** A read is in flight. */
        reading,

        /** An immediate completion is dispatched, its arguments held. */
        completing,
    };

    /**
     The most bytes one read asks for, as Boost.Asio's own reads into a dynamic buffer do: what the
     buffer holds room for, at least 512 and at most 65536, within its `max_size`.
    */
    [[nodiscard]] std::size_t read_size() const noexcept {
        const std::size_t size = buffer_.size();
        const std::size_t wanted = std::max<std::size_t>(512, buffer_.capacity() - size);
        return std::min(wanted, std::min<std::size_t>(65536, buffer_.max_size() - size));
    }

    /** Writes every byte the buffer holds into the parser, which keeps any failure for next(). */
    void feed() {
        const auto data = buffer_.data();
        for (auto it = boost::asio::buffer_sequence_begin(data);
             it != boost::asio::buffer_sequence_end(data); ++it) {
            const boost::asio::const_buffer bytes(*it);
            if (bytes.size() == 0) {
                continue;
            }
            static_cast<void>(parser_->write(
                std::string_view(static_cast<const char*>(bytes.data()), bytes.size())));
        }
        buffer_.consume(buffer_.size());
    }

    /** The empty document an error completes with. */
    static basic_document<Traits> nothing() noexcept {
        return access::make_document<Traits, owned>(region());
    }

    /**
     Completes the operation; from the initiating function, through the immediate completion its
     handler asks for, never inside it.
    */
    template <class Self>
    void complete(Self& self, boost::system::error_code code, basic_document<Traits>&& made) {
        if (step_ != step::starting) {
            self.complete(code, std::move(made));
            return;
        }
        step_ = step::completing;
        code_ = code;
        document_ = std::move(made);
        const auto io = self.get_io_executor();
        boost::asio::async_immediate(io, std::move(self));
    }

    AsyncReadStream* stream_;
    Buffer buffer_;
    basic_stream_parser<Traits>* parser_;
    step step_ = step::starting;
    boost::system::error_code read_failure_;
    boost::system::error_code code_;
    basic_document<Traits> document_ = nothing();
};

/**
 The composed operation of @ref async_read_image, which asks the reader for the room the next
 bytes of the image go into, reads some of that room from the stream into the block, and commits
 what it read, until the reader holds an image whole and takes it.
*/
template <class AsyncReadStream, class Traits>
class read_image_op {
public:
    /** The operation on a stream, reading images into the blocks of a pool, which outlive it. */
    read_image_op(AsyncReadStream& stream, image_pool& pool) noexcept
        : stream_(&stream), reader_(pool) {}

    /**
     Runs a step: at the start, when a read completes, or when an immediate completion is
     dispatched.
    */
    template <class Self>
    void operator()(Self& self, boost::system::error_code ec = {}, std::size_t n = 0) {
        if (step_ == step::completing) {
            self.complete(code_, std::move(image_));
            return;
        }
        if (step_ == step::reading) {
            if (ec && ec != boost::asio::error::eof) {
                return complete(self, ec);
            }
            const auto committed = reader_.commit(n);
            if (!committed.has_value()) {
                return complete(self, committed.error());
            }
            if (committed.unsafe_value() != 0) {
                return take(self);
            }
            if (ec == boost::asio::error::eof) {
                return end(self);
            }
        }
        // The first step, or a read that does not complete the image: the room for its next bytes.
        const auto room = reader_.prepare();
        if (!room.has_value()) {
            return complete(self, room.error());
        }
        const std::span<std::byte> bytes = room.unsafe_value();
        step_ = step::reading;
        // Some of the room, never a loop of Boost.Asio's own, so that no function of the
        // operation reaches itself through the completion of a read.
        stream_->async_read_some(boost::asio::buffer(bytes.data(), bytes.size()), std::move(self));
    }

private:
    /** Where the operation stands. */
    enum class step : std::uint8_t {
        /** Called by the initiating function: no read started yet. */
        starting,

        /** A read is in flight. */
        reading,

        /** An immediate completion is dispatched, its arguments held. */
        completing,
    };

    /** Takes the image the reader completed: it holds it until it is taken. */
    template <class Self>
    void take(Self& self) {
        auto taken = reader_.next();
        if (!taken.has_value()) {
            return complete(self, taken.error());
        }
        std::optional<leased_document<Traits>>& ready = taken.unsafe_value();
        BOOST_ASSERT(ready.has_value());
        complete(self, {}, std::move(*ready));
    }

    /** The stream ended: an image cut short is an error, the end between images is eof. */
    template <class Self>
    void end(Self& self) {
        const auto finished = reader_.finish();
        if (!finished.has_value()) {
            return complete(self, finished.error());
        }
        complete(self, boost::asio::error::eof);
    }

    /**
     Completes the operation, from the initiating function through the immediate completion its
     handler asks for, never inside it. An error carries the empty lease.
    */
    template <class Self>
    void complete(Self& self, boost::system::error_code code,
                  leased_document<Traits>&& made = leased_document<Traits>()) {
        if (step_ != step::starting) {
            self.complete(code, std::move(made));
            return;
        }
        step_ = step::completing;
        code_ = code;
        image_ = std::move(made);
        const auto io = self.get_io_executor();
        boost::asio::async_immediate(io, std::move(self));
    }

    AsyncReadStream* stream_;
    basic_image_reader<Traits> reader_;
    step step_ = step::starting;
    boost::system::error_code code_;
    leased_document<Traits> image_;
};

/**
 The composed operation of @ref async_write_image, which seals the document, then writes its image
 as one buffer, completing with the error of the seal or of the write.
*/
template <class AsyncWriteStream, class Traits, class Storage>
class write_image_op {
public:
    /** The operation on a stream and a document, which outlive it. */
    write_image_op(AsyncWriteStream& stream, basic_document<Traits, Storage>& document) noexcept
        : stream_(&stream), document_(&document) {}

    /**
     Runs a step: at the start, when the write completes, or when an immediate completion is
     dispatched.
    */
    template <class Self>
    void operator()(Self& self, boost::system::error_code ec = {}, std::size_t n = 0) {
        if (step_ == step::completing) {
            self.complete(code_);
            return;
        }
        if (step_ == step::writing) {
            if (ec) {
                return complete(self, ec);
            }
            remaining_ = remaining_.subspan(n);
        } else {
            // Sealed before the first byte goes out, so that the reader checks what it receives.
            const auto sealed = document_->seal();
            if (!sealed.has_value()) {
                return complete(self, sealed.error());
            }
            remaining_ = document_->image();
        }
        if (remaining_.empty()) {
            return complete(self, {});
        }
        step_ = step::writing;
        // Some of the bytes, again until none is left, so that no function of the operation
        // reaches itself through the completion of a write of Boost.Asio's own.
        stream_->async_write_some(boost::asio::buffer(remaining_.data(), remaining_.size()),
                                  std::move(self));
    }

private:
    /** Where the operation stands. */
    enum class step : std::uint8_t {
        /** Called by the initiating function: nothing written yet. */
        starting,

        /** A write is in flight. */
        writing,

        /** An immediate completion is dispatched, its error held. */
        completing,
    };

    /** Completes the operation, as the read operation's completion does. */
    template <class Self>
    void complete(Self& self, boost::system::error_code code) {
        if (step_ != step::starting) {
            self.complete(code);
            return;
        }
        step_ = step::completing;
        code_ = code;
        const auto io = self.get_io_executor();
        boost::asio::async_immediate(io, std::move(self));
    }

    AsyncWriteStream* stream_;
    basic_document<Traits, Storage>* document_;
    std::span<const std::byte> remaining_;
    step step_ = step::starting;
    boost::system::error_code code_;
};

/**
 A range of documents that async_write_images writes: its elements are lvalues, each of which seals
 its image and shows the bytes of it, as a document, a leased document and a shared document do.
*/
template <class Range>
concept image_range = std::ranges::forward_range<Range> && std::ranges::sized_range<Range> &&
                      std::is_lvalue_reference_v<std::ranges::range_reference_t<Range>> &&
                      requires(std::ranges::range_reference_t<Range> element) {
                          { element.seal() } -> std::same_as<result<void>>;
                          { element.image() } -> std::convertible_to<std::span<const std::byte>>;
                      };

/**
 Seals every document of a range, in order, and points a buffer at each image. The first document
 that fails to seal stops the gathering with its error, and no buffer is made for any image: every
 seal comes before the reserve and the buffers, so a failure allocates nothing.

 The buffers hold no copy of the bytes; they are valid while the documents are kept unchanged.
*/
template <image_range Range>
boost::system::error_code gather_images(Range& documents,
                                        std::vector<boost::asio::const_buffer>& buffers) {
    for (auto& element : documents) {
        if (const result<void> sealed = element.seal(); !sealed.has_value()) {
            return sealed.error();
        }
    }
    buffers.reserve(std::ranges::size(documents));
    for (const auto& element : documents) {
        const std::span<const std::byte> bytes = element.image();
        buffers.emplace_back(bytes.data(), bytes.size());
    }
    return {};
}

/**
 The composed operation of @ref async_write_images, which writes the buffers of a range's images
 in one gather write, which Boost.Asio sends with writev and continues over partial writes. A
 gathering that failed, or a range with no image, completes at once, writing nothing.
*/
template <class AsyncWriteStream>
class write_images_op {
public:
    /**
     The operation on a stream, with the buffers its gathering made and the error that stopped
     it, if any. The buffers point into images the caller keeps alive until the operation completes.
    */
    write_images_op(AsyncWriteStream& stream, boost::system::error_code failure,
                    std::vector<boost::asio::const_buffer>&& buffers) noexcept
        : stream_(&stream), buffers_(std::move(buffers)), failure_(failure) {}

    /**
     Runs a step: at the start, when the write completes, or when an immediate completion is
     dispatched.
    */
    template <class Self>
    void operator()(Self& self, boost::system::error_code ec = {}, std::size_t n = 0) {
        if (step_ == step::completing) {
            self.complete(code_, written_);
            return;
        }
        if (step_ == step::writing) {
            written_ += n;
            if (ec) {
                return complete(self, ec);
            }
        } else if (failure_) {
            return complete(self, failure_);
        }
        consume(n);
        if (first_ == buffers_.size()) {
            return complete(self, {});
        }
        step_ = step::writing;
        // Some of the bytes, again until none is left, so that no function of the operation
        // reaches itself through the completion of a write of Boost.Asio's own.
        stream_->async_write_some(
            std::span<const boost::asio::const_buffer>(buffers_).subspan(first_), std::move(self));
    }

private:
    /** Where the operation stands. */
    enum class step : std::uint8_t {
        /** Called by the initiating function: nothing written yet. */
        starting,

        /** A write is in flight. */
        writing,

        /** An immediate completion is dispatched, its arguments held. */
        completing,
    };

    /** Moves past the `n` bytes a write took, and past any buffer left empty. */
    void consume(std::size_t n) noexcept {
        for (; first_ != buffers_.size(); ++first_) {
            boost::asio::const_buffer& front = buffers_[first_];
            const std::size_t taken = std::min(n, front.size());
            front += taken;
            n -= taken;
            if (front.size() != 0) {
                return;
            }
        }
    }

    /**
     Completes the operation, from the initiating function through an immediate completion, never
     inside it.
    */
    template <class Self>
    void complete(Self& self, boost::system::error_code code) {
        if (step_ != step::starting) {
            self.complete(code, written_);
            return;
        }
        step_ = step::completing;
        code_ = code;
        const auto io = self.get_io_executor();
        boost::asio::async_immediate(io, std::move(self));
    }

    AsyncWriteStream* stream_;
    std::vector<boost::asio::const_buffer> buffers_;
    std::size_t first_ = 0;
    boost::system::error_code failure_;
    step step_ = step::starting;
    boost::system::error_code code_;
    std::size_t written_ = 0;
};

/**
 Starts the composed operation of @ref async_read_document on a buffer already adapted to the
 members it calls, so that both models share one operation.
*/
template <class AsyncReadStream, class Buffer, class Traits, class CompletionToken>
auto start_read_document(AsyncReadStream& stream, Buffer buffer,
                         basic_stream_parser<Traits>& parser, CompletionToken&& token) {
    return boost::asio::async_compose<CompletionToken,
                                      void(boost::system::error_code, basic_document<Traits>)>(
        read_document_op<AsyncReadStream, Buffer, Traits>(stream, std::move(buffer), parser), token,
        stream);
}

}  // namespace detail

#ifndef BOOST_ASIO_NO_DYNAMIC_BUFFER_V1
/**
 Reads the next document of a stream: from the parser when it holds one, else from the stream,
 reading only while the parser has none ready.

 It is a composed operation of Boost.Asio, which takes any completion token: a callback,
 `asio::use_awaitable`, `asio::deferred`, `boost::cobalt::use_op`. Its signature is
 `void(boost::system::error_code, basic_document<Traits>)`, and it completes with the next
 document; with the error of a document dropped, under `on_error::skip`, after which the next call
 goes on with the stream; with the error that stopped the stream, under `on_error::stop`, every
 time; with `asio::error::eof` once the stream ended and the parser's `finish` delivered what it
 held; or with the error of a read. On an error the document is empty. A completion is never
 called inside this function: one that needs no read is dispatched to the handler's immediate
 executor, else posted to the stream's.

 The buffer is a dynamic buffer of either of Boost.Asio's models, and the bytes it already holds,
 left by an earlier read of the same stream, are the parser's before any read. A buffer of the
 first model, `DynamicBuffer_v1` (`asio::streambuf`, Beast's `multi_buffer`, whatever the pieces
 of its `data`), is taken by reference and used through it: the caller keeps it alive until the
 operation completes. A buffer of the second model, `DynamicBuffer_v2` (`grow`, `shrink`,
 `data(pos, n)`: what `asio::dynamic_buffer` makes of a string, or a vector, also when
 `BOOST_ASIO_NO_DYNAMIC_BUFFER_V1` leaves out the first model) is taken by value, as Boost.Asio's
 own reads take it, and the operation keeps its copy: the caller keeps alive the storage it names,
 the string, and the function never needs the original. A buffer of both models is a buffer of
 the second, as in Boost.Asio. One operation at a time reads a stream, its buffer and its parser,
 which outlive it.

 @tparam AsyncReadStream A stream of Boost.Asio's `AsyncReadStream`: a socket, a pipe.
 @tparam DynamicBuffer A dynamic buffer of Boost.Asio's first model, `DynamicBuffer_v1`, and not
 of the second.
 @tparam Traits The traits of the documents.
 @tparam CompletionToken The completion token.
 @param stream The stream.
 @param buffer The buffer each read fills; the parser consumes what it holds before each read.
 @param parser The parser of the stream's framing.
 @param token The completion token.
 @return What the token makes of the operation.
 @see "Streams of documents", in the guide.
*/
template <class AsyncReadStream, class DynamicBuffer, class Traits,
          boost::asio::completion_token_for<void(boost::system::error_code, basic_document<Traits>)>
              CompletionToken =
                  boost::asio::default_completion_token_t<typename AsyncReadStream::executor_type>>
    requires(boost::asio::is_dynamic_buffer_v1<DynamicBuffer>::value &&
             !boost::asio::is_dynamic_buffer_v2<DynamicBuffer>::value)
auto async_read_document(
    AsyncReadStream& stream, DynamicBuffer& buffer, basic_stream_parser<Traits>& parser,
    CompletionToken&& token =
        boost::asio::default_completion_token_t<typename AsyncReadStream::executor_type>()) {
    return detail::start_read_document(stream, detail::buffer_v1_ref<DynamicBuffer>(buffer), parser,
                                       std::forward<CompletionToken>(token));
}
#endif  // BOOST_ASIO_NO_DYNAMIC_BUFFER_V1

/**
 Reads the next document of a stream into a buffer of Boost.Asio's second model, taken by value:
 the overload of @ref async_read_document for `DynamicBuffer_v2`.

 @tparam AsyncReadStream A stream of Boost.Asio's `AsyncReadStream`: a socket, a pipe.
 @tparam DynamicBuffer A dynamic buffer of Boost.Asio's second model, `DynamicBuffer_v2`; a
 buffer of both models is one of the second.
 @tparam Traits The traits of the documents.
 @tparam CompletionToken The completion token.
 @param stream The stream.
 @param buffer A copy of the buffer, which the operation keeps; the storage it names outlives the
 operation.
 @param parser The parser of the stream's framing.
 @param token The completion token.
 @return What the token makes of the operation.
 @see "Streams of documents", in the guide.
*/
template <class AsyncReadStream, class DynamicBuffer, class Traits,
          boost::asio::completion_token_for<void(boost::system::error_code, basic_document<Traits>)>
              CompletionToken =
                  boost::asio::default_completion_token_t<typename AsyncReadStream::executor_type>>
    requires boost::asio::is_dynamic_buffer_v2<DynamicBuffer>::value
auto async_read_document(
    AsyncReadStream& stream, DynamicBuffer buffer, basic_stream_parser<Traits>& parser,
    CompletionToken&& token =
        boost::asio::default_completion_token_t<typename AsyncReadStream::executor_type>()) {
    return detail::start_read_document(stream,
                                       detail::buffer_v2_copy<DynamicBuffer>(std::move(buffer)),
                                       parser, std::forward<CompletionToken>(token));
}

/**
 Reads the next image of a stream into a block of a pool and opens it there, with no copy and no
 allocation.

 The image's header is read into the block and checked before the rest of the image follows into
 the same block, which then holds the document. It reads exactly the bytes the reader asks for, so
 the stream is left at the end of the image it completed.

 It is a composed operation of Boost.Asio, which takes any completion token. Its signature is
 `void(boost::system::error_code, leased_document<Traits>)`: on success the document, which gives
 its block back when it goes; on error the empty lease (@ref leased_document::empty), with the
 error: @ref errc::capacity when every block of the pool is leased, in which case the stream is
 not read, @ref errc::not_a_document or @ref errc::damaged_document for an image the reader
 refuses, or the error of a read. The end of the stream is `asio::error::eof`, and it too is
 reported as @ref errc::capacity when every block is leased: the reader needs a block to learn
 that the stream ended, so a pool that is full hides the end until a block is given back.

 Images are trusted input: this path is for peers the program trusts.

 @tparam AsyncReadStream A stream of Boost.Asio's `AsyncReadStream`: a socket, a pipe.
 @tparam Traits The traits of the images.
 @tparam CompletionToken The completion token.
 @param stream The stream.
 @param pool The pool whose blocks the image is read into; it outlives the operation and the
 document.
 @param token The completion token.
 @return What the token makes of the operation.
 @see "Images on the wire", in the guide.
*/
template <
    class AsyncReadStream, class Traits = default_traits,
    boost::asio::completion_token_for<void(boost::system::error_code, leased_document<Traits>)>
        CompletionToken =
            boost::asio::default_completion_token_t<typename AsyncReadStream::executor_type>>
auto async_read_image(
    AsyncReadStream& stream, image_pool& pool,
    CompletionToken&& token =
        boost::asio::default_completion_token_t<typename AsyncReadStream::executor_type>()) {
    return boost::asio::async_compose<CompletionToken,
                                      void(boost::system::error_code, leased_document<Traits>)>(
        detail::read_image_op<AsyncReadStream, Traits>(stream, pool), token, stream);
}

/**
 Writes a document's image to a stream as one buffer, after sealing the document.

 The image carries a checksum of its bytes, which the reader checks, so a byte changed on the way
 is refused with @ref errc::damaged_document. The document is changed by the seal, which is why it
 is taken by non-const reference.

 It is a composed operation of Boost.Asio, which takes any completion token. Its signature is
 `void(boost::system::error_code)`: the error of the seal, which writes nothing, or of the write.
 The caller keeps the document alive until the operation completes: the write reads its image.

 @tparam AsyncWriteStream A stream of Boost.Asio's `AsyncWriteStream`: a socket, a pipe.
 @tparam Traits The traits of the document.
 @tparam Storage The storage of the document, owned or external.
 @tparam CompletionToken The completion token.
 @param stream The stream.
 @param document The document to send, sealed by the operation.
 @param token The completion token.
 @return What the token makes of the operation.
 @see "Sealed images", in the guide.
*/
template <class AsyncWriteStream, class Traits, class Storage,
          boost::asio::completion_token_for<void(boost::system::error_code)> CompletionToken =
              boost::asio::default_completion_token_t<typename AsyncWriteStream::executor_type>>
auto async_write_image(
    AsyncWriteStream& stream, basic_document<Traits, Storage>& document,
    CompletionToken&& token =
        boost::asio::default_completion_token_t<typename AsyncWriteStream::executor_type>()) {
    return boost::asio::async_compose<CompletionToken, void(boost::system::error_code)>(
        detail::write_image_op<AsyncWriteStream, Traits, Storage>(stream, document), token, stream);
}

/**
 Writes the images of a range of documents to a stream in one gather write, after sealing every
 document of the range.

 The seals come first: the first document that fails to seal completes the operation with its
 error, having written nothing. Otherwise the images are written as one gather write, which
 Boost.Asio sends with writev and continues over partial writes, with no copy of their bytes. The
 bytes are read in place, so the documents must not change until the operation completes.

 It is a composed operation of Boost.Asio, which takes any completion token. Its signature is
 `void(boost::system::error_code, std::size_t)`: the bytes written, the sum of the images' sizes on
 success. On an error it is the bytes that reached the stream, which may end inside an image: the
 whole documents sent are the leading ones whose image sizes, added in order, do not exceed it. An
 empty range completes with 0 and writes nothing.

 @tparam AsyncWriteStream A stream of Boost.Asio's `AsyncWriteStream`: a socket, a pipe.
 @tparam Range The range of documents: its elements are lvalues of documents, or of any type that
 seals its image and shows its bytes, such as a leased or a shared document.
 @tparam CompletionToken The completion token.
 @param stream The stream.
 @param documents The range, read by this call only: the operation seals its documents, and the
 caller keeps them alive until it completes. A temporary range is refused.
 @param token The completion token.
 @return What the token makes of the operation.
 @see "Images on the wire", in the guide.
*/
template <class AsyncWriteStream, detail::image_range Range,
          boost::asio::completion_token_for<void(boost::system::error_code, std::size_t)>
              CompletionToken =
                  boost::asio::default_completion_token_t<typename AsyncWriteStream::executor_type>>
auto async_write_images(
    AsyncWriteStream& stream, Range& documents,
    CompletionToken&& token =
        boost::asio::default_completion_token_t<typename AsyncWriteStream::executor_type>()) {
    // The guards come first: the gathering seals every document, and a failure completes below.
    std::vector<boost::asio::const_buffer> buffers;
    const boost::system::error_code failure = detail::gather_images(documents, buffers);
    return boost::asio::async_compose<CompletionToken,
                                      void(boost::system::error_code, std::size_t)>(
        detail::write_images_op<AsyncWriteStream>(stream, failure, std::move(buffers)), token,
        stream);
}

}  // namespace webcpp::document

#endif  // WEBCPP_DOCUMENT_ASIO_HPP
