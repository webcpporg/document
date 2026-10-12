// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// async_read_document over a loopback TCP connection: a writer sends NDJSON and concatenated JSON
// in odd-sized pieces, the reader catching up between them; the reader reads with a callback and,
// with exceptions, in a coroutine with use_awaitable; a skipped line is reported with its line and
// the stream goes on; the end of the stream is eof, after the last document, every time; a
// document the parser already holds completes without a read, never inside the call; a buffer of
// several pieces (Beast's multi_buffer) or of Boost.Asio's second model (a string, a buffer with
// only that model's members) is read like a string, its held bytes before any read.

#include <webcpp/document/asio.hpp>
#include <webcpp/document/stringify.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/multi_buffer.hpp>
#include <boost/core/lightweight_test.hpp>

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#endif

#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace asio = boost::asio;
namespace wd = webcpp::document;
using asio::ip::tcp;

namespace {

using outcomes = std::vector<std::string>;

// A connected pair of loopback TCP sockets: the writer's end and the reader's.
struct connection {
    explicit connection(asio::io_context& io) : writer(io), reader(io) {
        tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        reader.connect(acceptor.local_endpoint());
        acceptor.accept(writer);
    }

    tcp::socket writer;
    tcp::socket reader;
};

// Drains the context with poll until `done` holds, or fails when the context has no work left, or
// after a hundred thousand turns in a row that ran no handler, a count and not a time: the bytes a
// write sent reach the reader's socket through the system, so a poll may find them only on a later
// turn.
void drain_until(asio::io_context& io, const std::function<bool()>& done) {
    constexpr std::size_t max_idle = 100'000;
    for (std::size_t idle = 0; !done();) {
        if (io.stopped()) {
            io.restart();
        }
        const std::size_t ran = io.poll();
        idle = ran == 0 ? idle + 1 : 0;
        if ((ran == 0 && io.stopped()) || idle == max_idle) {
            break;
        }
    }
    webcpp::test::require(BOOST_TEST(done()));
}

// A document or an error as the reader records it.
std::string outcome(const boost::system::error_code& ec, const wd::document& d,
                    const wd::stream_parser& p) {
    if (ec) {
        return "error " + ec.message() + " at line " + std::to_string(p.error_line());
    }
    return wd::stringify(d);
}

// Reads every document with a callback, each read started after the last one's completion, until
// eof, which it records as "eof", or a hundred outcomes, which no stream of these tests reaches.
class callback_reader {
public:
    callback_reader(tcp::socket& socket, wd::stream_parser& parser)
        : socket_(socket), parser_(parser) {}

    void start() {
        wd::async_read_document(socket_, buffer_, parser_,
                                [this](boost::system::error_code ec, const wd::document& d) {
                                    if (ec == asio::error::eof) {
                                        read.emplace_back("eof");
                                        return;
                                    }
                                    read.push_back(outcome(ec, d, parser_));
                                    // Posted rather than called, so that no function of the
                                    // reader calls itself, which misc-no-recursion refuses.
                                    if (read.size() < 100) {
                                        asio::post(socket_.get_executor(), [this] { start(); });
                                    }
                                });
    }

    outcomes read;

private:
    tcp::socket& socket_;
    wd::stream_parser& parser_;
    std::string storage_;
    asio::dynamic_string_buffer<char, std::char_traits<char>, std::allocator<char>> buffer_ =
        asio::dynamic_buffer(storage_);
};

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
// Reads every document in a coroutine, with use_awaitable and as_tuple, until eof.
asio::awaitable<void> read_in_coroutine(tcp::socket& socket, wd::stream_parser& parser,
                                        outcomes& read) {
    std::string storage;
    const auto buffer = asio::dynamic_buffer(storage);
    for (;;) {
        auto [ec, d] = co_await wd::async_read_document(socket, buffer, parser,
                                                        asio::as_tuple(asio::use_awaitable));
        if (ec == asio::error::eof) {
            read.emplace_back("eof");
            co_return;
        }
        read.push_back(outcome(ec, d, parser));
    }
}
#endif

// Sends `text` in pieces of 1, 3, 7, 2 and 13 bytes in turn, letting the reader's parser consume
// each before the next, then ends the stream.
void send_in_pieces(asio::io_context& io, tcp::socket& writer, std::string_view text,
                    const wd::stream_parser& parser) {
    constexpr std::array<std::size_t, 5> sizes{1, 3, 7, 2, 13};
    std::size_t turn = 0;
    std::uint64_t sent = 0;
    while (!text.empty()) {
        const std::string_view piece = text.substr(0, sizes.at(turn++ % sizes.size()));
        asio::write(writer, asio::buffer(piece.data(), piece.size()));
        text.remove_prefix(piece.size());
        sent += piece.size();
        // The pause: the context drained until the reader read the piece.
        drain_until(io, [&parser, sent] { return parser.offset() == sent; });
    }
    writer.shutdown(tcp::socket::shutdown_send);
}

constexpr std::string_view ndjson =
    "{\"id\":1,\"tags\":[\"a\",\"b\"]}\n"
    "{\"id\":2,\"name\":\"\\u00e9\"}\r\n"
    "\n"
    "{\"id\":3,bad}\n"
    "[4,5]\n"
    "6";

outcomes ndjson_read() {
    return {
        R"({"id":1,"tags":["a","b"]})",
        R"({"id":2,"name":"é"})",
        "error malformed JSON text at line 4",
        "[4,5]",
        "6",
        "eof",
    };
}

constexpr std::string_view concatenated =
    "{\n  \"a\": 1\n}\n{\"b\":[true,null]}{\"c\":\"x\"}  [1] 2 3";

outcomes concatenated_read() {
    return {R"({"a":1})", R"({"b":[true,null]})", R"({"c":"x"})", "[1]", "2", "3", "eof"};
}

wd::stream_options options_of(wd::framing f, wd::on_error e) {
    wd::stream_options o;
    o.framing = f;
    o.on_error = e;
    return o;
}

// The reader with a callback reads NDJSON, a bad line reported and skipped, and concatenated JSON.
void a_callback_reads_documents() {
    {
        asio::io_context io;
        connection c(io);
        wd::stream_parser parser(options_of(wd::framing::lines, wd::on_error::skip));
        callback_reader reader(c.reader, parser);
        reader.start();
        send_in_pieces(io, c.writer, ndjson, parser);
        drain_until(io, [&reader] { return !reader.read.empty() && reader.read.back() == "eof"; });
        BOOST_TEST(reader.read == ndjson_read());
        BOOST_TEST_EQ(parser.documents(), 4U);
    }
    {
        asio::io_context io;
        connection c(io);
        wd::stream_parser parser;
        callback_reader reader(c.reader, parser);
        reader.start();
        send_in_pieces(io, c.writer, concatenated, parser);
        drain_until(io, [&reader] { return !reader.read.empty() && reader.read.back() == "eof"; });
        BOOST_TEST(reader.read == concatenated_read());
    }
}

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
// The reader in a coroutine reads the same documents.
void a_coroutine_reads_documents() {
    {
        asio::io_context io;
        connection c(io);
        wd::stream_parser parser(options_of(wd::framing::lines, wd::on_error::skip));
        outcomes read;
        asio::co_spawn(io, read_in_coroutine(c.reader, parser, read), asio::detached);
        send_in_pieces(io, c.writer, ndjson, parser);
        drain_until(io, [&read] { return !read.empty() && read.back() == "eof"; });
        BOOST_TEST(read == ndjson_read());
    }
    {
        asio::io_context io;
        connection c(io);
        wd::stream_parser parser;
        outcomes read;
        asio::co_spawn(io, read_in_coroutine(c.reader, parser, read), asio::detached);
        send_in_pieces(io, c.writer, concatenated, parser);
        drain_until(io, [&read] { return !read.empty() && read.back() == "eof"; });
        BOOST_TEST(read == concatenated_read());
    }
}
#endif

// A document the parser holds completes without a read, after the call returns; once the stream
// ended, every read completes with eof; an error that stopped the stream completes every read.
void completions() {
    asio::io_context io;
    connection c(io);
    wd::stream_parser parser(options_of(wd::framing::lines, wd::on_error::stop));
    BOOST_TEST_EQ(parser.write("[1]\n[2]\n").value(), 2U);
    std::string storage;
    const auto buffer = asio::dynamic_buffer(storage);
    outcomes read;
    const auto record = [&read, &parser](boost::system::error_code ec, const wd::document& d) {
        read.push_back(ec == asio::error::eof ? "eof" : outcome(ec, d, parser));
    };
    wd::async_read_document(c.reader, buffer, parser, record);
    BOOST_TEST(read.empty());
    drain_until(io, [&read] { return read.size() == 1; });
    BOOST_TEST_EQ(read.back(), "[1]");

    // Bytes the buffer already holds are the parser's before any read.
    std::string held = "[3]\n";
    const auto filled = asio::dynamic_buffer(held);
    wd::async_read_document(c.reader, filled, parser, record);
    wd::async_read_document(c.reader, filled, parser, record);
    drain_until(io, [&read] { return read.size() == 3; });
    BOOST_TEST((read == outcomes{"[1]", "[2]", "[3]"}));

    c.writer.shutdown(tcp::socket::shutdown_send);
    wd::async_read_document(c.reader, buffer, parser, record);
    drain_until(io, [&read] { return read.size() == 4; });
    wd::async_read_document(c.reader, buffer, parser, record);
    drain_until(io, [&read] { return read.size() == 5; });
    BOOST_TEST((read == outcomes{"[1]", "[2]", "[3]", "eof", "eof"}));

    // A stream stopped by its error completes every read with it.
    wd::stream_parser stopped(options_of(wd::framing::lines, wd::on_error::stop));
    BOOST_TEST(stopped.write("[1,}\n").error() == wd::errc::syntax);
    outcomes failed;
    const auto failing = [&failed, &stopped](boost::system::error_code ec, const wd::document& d) {
        failed.push_back(outcome(ec, d, stopped));
    };
    wd::async_read_document(c.reader, buffer, stopped, failing);
    wd::async_read_document(c.reader, buffer, stopped, failing);
    drain_until(io, [&failed] { return failed.size() == 2; });
    BOOST_TEST(failed == outcomes(2, "error malformed JSON text at line 1"));
}

// The documents the buffer tests read: strings and numbers a piece boundary falls inside of.
constexpr std::string_view split_text =
    R"({"name":"a string cut anywhere","n":12345.678e1} [10203040,"x y z"] {"k":[1,22,333]})";

outcomes split_read() {
    return {
        R"({"name":"a string cut anywhere","n":123456.78})",
        R"([10203040,"x y z"])",
        R"({"k":[1,22,333]})",
    };
}

// Puts `text` into `buffer` in pieces of `step` bytes, each its own prepare and commit, which
// gives Beast's multi_buffer one element for each once the spare room of the last is returned.
// Tip: the first element is never shrunk, so 512 spaces, JSON whitespace, fill it first.
void fill_in_pieces(boost::beast::multi_buffer& buffer, std::string_view text, std::size_t step) {
    const std::string blank(512, ' ');
    buffer.commit(asio::buffer_copy(buffer.prepare(blank.size()), asio::buffer(blank)));
    while (!text.empty()) {
        const std::string_view piece = text.substr(0, step);
        buffer.commit(asio::buffer_copy(buffer.prepare(piece.size()),
                                        asio::buffer(piece.data(), piece.size())));
        buffer.shrink_to_fit();
        text.remove_prefix(piece.size());
    }
}

std::size_t pieces_of(const boost::beast::multi_buffer& buffer) {
    const auto data = buffer.data();
    return static_cast<std::size_t>(
        std::distance(asio::buffer_sequence_begin(data), asio::buffer_sequence_end(data)));
}

// Starts `count` reads of one buffer, each after the last one's completion, and records them.
template <class Buffer>
void read_all(asio::io_context& io, tcp::socket& socket, Buffer& buffer, wd::stream_parser& parser,
              outcomes& read, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t before = read.size();
        wd::async_read_document(
            socket, buffer, parser,
            [&read, &parser](boost::system::error_code ec, const wd::document& d) {
                read.push_back(ec == asio::error::eof ? "eof" : outcome(ec, d, parser));
            });
        drain_until(io, [&read, before] { return read.size() == before + 1; });
    }
}

// A multi_buffer whose pieces are small: each read asks for at most five bytes, so the documents
// arrive cut inside a string and a number.
void a_multi_buffer_with_small_blocks() {
    asio::io_context io;
    connection c(io);
    wd::stream_parser parser;
    boost::beast::multi_buffer buffer(5);
    asio::write(c.writer, asio::buffer(split_text.data(), split_text.size()));
    c.writer.shutdown(tcp::socket::shutdown_send);
    outcomes read;
    read_all(io, c.reader, buffer, parser, read, 4);
    BOOST_TEST((read == outcomes{split_read()[0], split_read()[1], split_read()[2], "eof"}));
    BOOST_TEST_EQ(buffer.size(), 0U);
}

// Bytes a multi_buffer holds in several pieces, left by an earlier protocol read, are the parser's
// before any read: the stream sends nothing, and the documents still come.
void a_multi_buffer_holds_bytes() {
    asio::io_context io;
    connection c(io);
    wd::stream_parser parser;
    boost::beast::multi_buffer buffer;
    fill_in_pieces(buffer, split_text, 3);
    BOOST_TEST_GT(pieces_of(buffer), 10U);
    outcomes read;
    read_all(io, c.reader, buffer, parser, read, 3);
    BOOST_TEST(read == split_read());
    BOOST_TEST_EQ(buffer.size(), 0U);
}

// A document that begins in the held pieces and ends in the stream.
void a_multi_buffer_holds_the_start() {
    asio::io_context io;
    connection c(io);
    wd::stream_parser parser;
    boost::beast::multi_buffer buffer;
    const std::size_t cut = split_text.find("cut") + 1;
    fill_in_pieces(buffer, split_text.substr(0, cut), 4);
    BOOST_TEST_GT(pieces_of(buffer), 1U);
    const std::string_view rest = split_text.substr(cut);
    asio::write(c.writer, asio::buffer(rest.data(), rest.size()));
    c.writer.shutdown(tcp::socket::shutdown_send);
    outcomes read;
    read_all(io, c.reader, buffer, parser, read, 3);
    BOOST_TEST(read == split_read());
}

// A buffer of Boost.Asio's second model and of no other: the members of DynamicBuffer_v2 over a
// string, a copy of it naming the same string.
class second_model_buffer {
public:
    using const_buffers_type = asio::const_buffer;
    using mutable_buffers_type = asio::mutable_buffer;

    explicit second_model_buffer(std::string& s, std::size_t max = std::string::npos) noexcept
        : string_(&s), max_(max) {}

    [[nodiscard]] std::size_t size() const noexcept { return std::min(string_->size(), max_); }

    [[nodiscard]] std::size_t max_size() const noexcept { return max_; }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return std::min(string_->capacity(), max_);
    }

    [[nodiscard]] const_buffers_type data(std::size_t pos, std::size_t n) const noexcept {
        return asio::buffer(asio::buffer(*string_) + pos, n);
    }

    mutable_buffers_type data(std::size_t pos, std::size_t n) noexcept {
        return asio::buffer(asio::buffer(*string_) + pos, n);
    }

    void grow(std::size_t n) { string_->resize(size() + n); }

    void shrink(std::size_t n) { string_->resize(n > size() ? 0 : size() - n); }

    void consume(std::size_t n) { string_->erase(0, std::min(n, string_->size())); }

private:
    std::string* string_;
    std::size_t max_;
};

static_assert(asio::is_dynamic_buffer_v2<second_model_buffer>::value);
static_assert(!asio::is_dynamic_buffer_v1<second_model_buffer>::value);

// A buffer of both models, as asio::dynamic_buffer makes of a string, that counts the members of
// each model the operation calls, so that the test sees which model the operation used.
struct counts {
    std::size_t grows = 0;
    std::size_t prepares = 0;
};

class counting_buffer {
    using base = asio::dynamic_string_buffer<char, std::char_traits<char>, std::allocator<char>>;

public:
    using const_buffers_type = base::const_buffers_type;
    using mutable_buffers_type = base::mutable_buffers_type;

    counting_buffer(std::string& s, counts& seen) : base_(s), seen_(&seen) {}

    [[nodiscard]] std::size_t size() const noexcept { return base_.size(); }

    [[nodiscard]] std::size_t max_size() const noexcept { return base_.max_size(); }

    [[nodiscard]] std::size_t capacity() const noexcept { return base_.capacity(); }

    [[nodiscard]] const_buffers_type data() const noexcept { return base_.data(); }

    [[nodiscard]] const_buffers_type data(std::size_t pos, std::size_t n) const noexcept {
        return base_.data(pos, n);
    }

    mutable_buffers_type data(std::size_t pos, std::size_t n) noexcept {
        return base_.data(pos, n);
    }

    void grow(std::size_t n) {
        ++seen_->grows;
        base_.grow(n);
    }

    void shrink(std::size_t n) { base_.shrink(n); }

    mutable_buffers_type prepare(std::size_t n) {
        ++seen_->prepares;
        return base_.prepare(n);
    }

    void commit(std::size_t n) { base_.commit(n); }

    void consume(std::size_t n) { base_.consume(n); }

private:
    base base_;
    counts* seen_;
};

static_assert(asio::is_dynamic_buffer_v1<counting_buffer>::value);
static_assert(asio::is_dynamic_buffer_v2<counting_buffer>::value);

// A buffer of both models is read as the second, as Boost.Asio's own reads do.
void a_buffer_of_both_models_is_read_as_the_second() {
    asio::io_context io;
    connection c(io);
    wd::stream_parser parser;
    std::string storage;
    counts seen;
    counting_buffer buffer(storage, seen);
    asio::write(c.writer, asio::buffer(split_text.data(), split_text.size()));
    c.writer.shutdown(tcp::socket::shutdown_send);
    outcomes read;
    read_all(io, c.reader, buffer, parser, read, 4);
    BOOST_TEST((read == outcomes{split_read()[0], split_read()[1], split_read()[2], "eof"}));
    BOOST_TEST_GT(seen.grows, 0U);
    BOOST_TEST_EQ(seen.prepares, 0U);
    BOOST_TEST(storage.empty());
}

// A buffer of the second model only: contiguous, with a small maximum size so that each read
// cuts the documents, with the bytes it already holds before any read, and with only the start of
// a document held, the rest coming from the stream.
void a_second_model_buffer_reads_documents() {
    {
        asio::io_context io;
        connection c(io);
        wd::stream_parser parser;
        std::string storage;
        second_model_buffer buffer(storage);
        asio::write(c.writer, asio::buffer(split_text.data(), split_text.size()));
        c.writer.shutdown(tcp::socket::shutdown_send);
        outcomes read;
        read_all(io, c.reader, buffer, parser, read, 4);
        BOOST_TEST((read == outcomes{split_read()[0], split_read()[1], split_read()[2], "eof"}));
        BOOST_TEST(storage.empty());
    }
    {
        asio::io_context io;
        connection c(io);
        wd::stream_parser parser;
        std::string storage;
        second_model_buffer buffer(storage, 5);
        asio::write(c.writer, asio::buffer(split_text.data(), split_text.size()));
        c.writer.shutdown(tcp::socket::shutdown_send);
        outcomes read;
        read_all(io, c.reader, buffer, parser, read, 4);
        BOOST_TEST((read == outcomes{split_read()[0], split_read()[1], split_read()[2], "eof"}));
    }
    {
        asio::io_context io;
        connection c(io);
        wd::stream_parser parser;
        std::string storage(split_text);
        second_model_buffer buffer(storage);
        outcomes read;
        read_all(io, c.reader, buffer, parser, read, 3);
        BOOST_TEST(read == split_read());
        BOOST_TEST(storage.empty());
    }
    {
        asio::io_context io;
        connection c(io);
        wd::stream_parser parser;
        const std::size_t cut = split_text.find("cut") + 1;
        std::string storage(split_text.substr(0, cut));
        second_model_buffer buffer(storage);
        const std::string_view rest = split_text.substr(cut);
        asio::write(c.writer, asio::buffer(rest.data(), rest.size()));
        c.writer.shutdown(tcp::socket::shutdown_send);
        outcomes read;
        read_all(io, c.reader, buffer, parser, read, 3);
        BOOST_TEST(read == split_read());
    }
}

// What asio::dynamic_buffer makes of a string, passed as it is: with bytes held, and with a
// maximum size that no read goes past.
void a_string_buffer_holds_bytes_and_a_maximum() {
    {
        asio::io_context io;
        connection c(io);
        wd::stream_parser parser;
        std::string storage(split_text);
        auto buffer = asio::dynamic_buffer(storage);
        outcomes read;
        read_all(io, c.reader, buffer, parser, read, 3);
        BOOST_TEST(read == split_read());
        BOOST_TEST(storage.empty());
    }
    {
        asio::io_context io;
        connection c(io);
        wd::stream_parser parser;
        std::string storage;
        asio::write(c.writer, asio::buffer(split_text.data(), split_text.size()));
        c.writer.shutdown(tcp::socket::shutdown_send);
        outcomes read;
        auto buffer = asio::dynamic_buffer(storage, 5);
        read_all(io, c.reader, buffer, parser, read, 4);
        BOOST_TEST((read == outcomes{split_read()[0], split_read()[1], split_read()[2], "eof"}));
    }
}

}  // namespace

int main() {
    a_callback_reads_documents();
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
    a_coroutine_reads_documents();
#endif
    completions();
    a_multi_buffer_with_small_blocks();
    a_multi_buffer_holds_bytes();
    a_multi_buffer_holds_the_start();
    a_buffer_of_both_models_is_read_as_the_second();
    a_second_model_buffer_reads_documents();
    a_string_buffer_holds_bytes_and_a_maximum();
    return boost::report_errors();
}
