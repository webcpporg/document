// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// async_read_document over a loopback TCP connection: a writer sends NDJSON and concatenated JSON
// in odd-sized pieces, the reader catching up between them; the reader reads with a callback and,
// with exceptions, in a coroutine with use_awaitable; a skipped line is reported with its line and
// the stream goes on; the end of the stream is eof, after the last document, every time; a
// document the parser already holds completes without a read, never inside the call.

#include <webcpp/document/asio.hpp>
#include <webcpp/document/stringify.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <boost/core/lightweight_test.hpp>

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#endif

#include "require.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
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

// Drains the context with poll until `done` holds, or fails after ten seconds: the bytes a write
// sent reach the reader's socket through the system, so a poll may find them only on a later turn.
void drain_until(asio::io_context& io, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        if (io.stopped()) {
            io.restart();
        }
        io.poll();
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
    auto buffer = asio::dynamic_buffer(storage);
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
    auto buffer = asio::dynamic_buffer(storage);
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
    auto filled = asio::dynamic_buffer(held);
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

}  // namespace

int main() {
    a_callback_reads_documents();
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
    a_coroutine_reads_documents();
#endif
    completions();
    return boost::report_errors();
}
