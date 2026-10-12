// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 Streams of documents read from Boost.Asio's streams: `async_read_document`, which reads the next
 document of a stream with any completion token. The one header of document that includes
 Boost.Asio; it builds natively only.
*/

#ifndef WEBCPP_DOCUMENT_ASIO_HPP
#define WEBCPP_DOCUMENT_ASIO_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/heap.hpp>
#include <webcpp/document/document.hpp>
#include <webcpp/document/stream.hpp>
#include <webcpp/document/value.hpp>

#include <boost/asio/async_result.hpp>  // lint-world: reads the stream its caller gives
#include <boost/asio/buffer.hpp>        // lint-world: reads the stream its caller gives
#include <boost/asio/compose.hpp>       // lint-world: reads the stream its caller gives
#include <boost/asio/error.hpp>         // lint-world: reads the stream its caller gives
#include <boost/asio/immediate.hpp>     // lint-world: reads the stream its caller gives
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace webcpp::document {

namespace detail {

/**
 The composed operation of @ref async_read_document, which takes the parser's next document, and
 reads the stream into the buffer, and the buffer into the parser, only while there is none.
*/
template <class AsyncReadStream, class DynamicBuffer, class Traits>
class read_document_op {
public:
    /** The operation on a stream, its buffer and its parser, which outlive it. */
    read_document_op(AsyncReadStream& stream, DynamicBuffer& buffer,
                     basic_stream_parser<Traits>& parser) noexcept
        : stream_(&stream), buffer_(&buffer), parser_(&parser) {}

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
            buffer_->commit(n);
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
        if (ended) {
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
        stream_->async_read_some(buffer_->prepare(room), std::move(self));
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
        const std::size_t size = buffer_->size();
        const std::size_t wanted = std::max<std::size_t>(512, buffer_->capacity() - size);
        return std::min(wanted, std::min<std::size_t>(65536, buffer_->max_size() - size));
    }

    /** Writes every byte the buffer holds into the parser, which keeps any failure for next(). */
    void feed() {
        const auto data = buffer_->data();
        for (auto it = boost::asio::buffer_sequence_begin(data);
             it != boost::asio::buffer_sequence_end(data); ++it) {
            const boost::asio::const_buffer bytes(*it);
            if (bytes.size() == 0) {
                continue;
            }
            static_cast<void>(parser_->write(
                std::string_view(static_cast<const char*>(bytes.data()), bytes.size())));
        }
        buffer_->consume(buffer_->size());
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
    DynamicBuffer* buffer_;
    basic_stream_parser<Traits>* parser_;
    step step_ = step::starting;
    boost::system::error_code read_failure_;
    boost::system::error_code code_;
    basic_document<Traits> document_ = nothing();
};

}  // namespace detail

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

 One operation at a time reads a stream, its buffer and its parser, which outlive it.

 @tparam AsyncReadStream A stream of Boost.Asio's `AsyncReadStream`: a socket, a pipe.
 @tparam DynamicBuffer A dynamic buffer of Boost.Asio's first version, `DynamicBuffer_v1`: what
 `asio::dynamic_buffer` makes of a string, or `asio::streambuf`.
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
    requires boost::asio::is_dynamic_buffer_v1<DynamicBuffer>::value
auto async_read_document(
    AsyncReadStream& stream, DynamicBuffer& buffer, basic_stream_parser<Traits>& parser,
    CompletionToken&& token =
        boost::asio::default_completion_token_t<typename AsyncReadStream::executor_type>()) {
    return boost::asio::async_compose<CompletionToken,
                                      void(boost::system::error_code, basic_document<Traits>)>(
        detail::read_document_op<AsyncReadStream, DynamicBuffer, Traits>(stream, buffer, parser),
        token, stream);
}

}  // namespace webcpp::document

#endif  // WEBCPP_DOCUMENT_ASIO_HPP
