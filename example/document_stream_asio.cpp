// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Streams of documents over TCP on 127.0.0.1, in one program: a server writes NDJSON, then
// concatenated JSON, in odd-sized pieces; the client reads the NDJSON in a coroutine with
// async_read_document, and the concatenated JSON with a callback that restarts its read through
// post. The context is drained with poll and restart, so the output does not depend on timing.

#include <webcpp/document.hpp>
#include <webcpp/document/asio.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace asio = boost::asio;
namespace wd = webcpp::document;
using asio::ip::tcp;

namespace {

// tag::link[]
/**
 A TCP connection on the loopback address: the server's end, written to, and the client's end,
 read from.
*/
struct link {
    link(asio::io_context& io, tcp::acceptor& acceptor) : server(io), client(io) {
        client.connect(acceptor.local_endpoint());
        acceptor.accept(server);
    }

    tcp::socket server;
    tcp::socket client;
};

// end::link[]

// tag::drain[]
/**
 Runs the context's ready handlers, polling until `done` holds. The bytes a write sent reach the
 client's socket through the system, so a poll may find them only on a later turn; the loop gives up
 when the context has no work left, or after a hundred thousand turns in a row that ran no handler,
 a count and not a time.
*/
template <class Done>
bool drain_until(asio::io_context& io, Done done) {
    constexpr std::size_t max_idle = 100'000;
    for (std::size_t idle = 0; !done();) {
        if (io.stopped()) {
            io.restart();
        }
        const std::size_t ran = io.poll();
        idle = ran == 0 ? idle + 1 : 0;
        if ((ran == 0 && io.stopped()) || idle == max_idle) {
            std::cerr << "error: the exchange did not finish\n";
            return false;
        }
    }
    return true;
}

// end::drain[]

// tag::send[]
/**
 Writes `text` to the server's end in pieces of 1, 3, 7, 2 and 13 bytes in turn. After each piece
 the context is drained until the client's parser has consumed it, so the pieces arrive one at a
 time; then the write side is shut down, which the client reads as eof.
*/
bool send_in_pieces(asio::io_context& io, tcp::socket& server, std::string_view text,
                    const wd::stream_parser& parser) {
    constexpr std::array<std::size_t, 5> sizes{1, 3, 7, 2, 13};
    std::uint64_t sent = 0;
    for (std::size_t turn = 0; !text.empty(); ++turn) {
        const std::string_view piece = text.substr(0, sizes[turn % sizes.size()]);
        text.remove_prefix(piece.size());
        asio::write(server, asio::buffer(piece.data(), piece.size()));
        sent += piece.size();
        if (!drain_until(io, [&parser, sent] { return parser.offset() == sent; })) {
            return false;
        }
    }
    server.shutdown(tcp::socket::shutdown_send);
    return true;
}

// end::send[]

// tag::coroutine[]
/**
 Reads the client's documents until eof, awaiting each read in turn. The parser's finish delivers
 its last document before the eof.
*/
asio::awaitable<void> read_in_coroutine(tcp::socket& client, wd::stream_parser& parser) {
    std::string storage;
    const auto buffer = asio::dynamic_buffer(storage);
    for (;;) {
        auto [ec, made] = co_await wd::async_read_document(client, buffer, parser,
                                                           asio::as_tuple(asio::use_awaitable));
        if (ec == asio::error::eof) {
            std::cout << "eof\n";
            co_return;
        }
        if (ec) {
            std::cout << "error: " << ec.message() << '\n';
            co_return;
        }
        std::cout << made.stringify() << '\n';
    }
}

/** Sends NDJSON over a link and reads it in a coroutine, one document printed per line. */
bool ndjson_in_coroutine(asio::io_context& io, tcp::acceptor& acceptor) {
    std::cout << "NDJSON, read in a coroutine:\n";
    link c(io, acceptor);
    wd::stream_options options;
    options.framing = wd::framing::lines;
    wd::stream_parser parser(options);
    constexpr std::string_view text =
        "{\"id\":1,\"tags\":[\"a\",\"b\"]}\n"
        "{\"id\":2,\"ok\":false}\r\n"
        "\n"
        "[3,null]\n"
        "\"last\"";
    bool done = false;
    asio::co_spawn(io, read_in_coroutine(c.client, parser),
                   [&done](const std::exception_ptr&) { done = true; });
    return send_in_pieces(io, c.server, text, parser) && drain_until(io, [&done] { return done; });
}

// end::coroutine[]

// tag::callback[]
/**
 Reads documents with a callback, each read started after the one before it completes. The next
 read is posted rather than called, so no read is started from inside its own callback.
*/
class callback_reader {
public:
    callback_reader(tcp::socket& client, wd::stream_parser& parser, bool& done)
        : client_(client), parser_(parser), done_(done), buffer_(asio::dynamic_buffer(storage_)) {}

    /** Starts the read of the next document. */
    void start() {
        wd::async_read_document(client_, buffer_, parser_,
                                [this](boost::system::error_code ec, const wd::document& made) {
                                    if (ec == asio::error::eof) {
                                        std::cout << "eof\n";
                                        done_ = true;
                                        return;
                                    }
                                    if (ec) {
                                        std::cout << "error: " << ec.message() << '\n';
                                        done_ = true;
                                        return;
                                    }
                                    std::cout << made.stringify() << '\n';
                                    asio::post(client_.get_executor(), [this] { start(); });
                                });
    }

private:
    tcp::socket& client_;
    wd::stream_parser& parser_;
    bool& done_;
    std::string storage_;
    asio::dynamic_string_buffer<char, std::char_traits<char>, std::allocator<char>> buffer_;
};

/** Sends concatenated JSON over a link and reads it with a callback. */
bool concatenated_with_callback(asio::io_context& io, tcp::acceptor& acceptor) {
    std::cout << "concatenated JSON, read with a callback:\n";
    link c(io, acceptor);
    wd::stream_parser parser;
    constexpr std::string_view text =
        "{\"a\":1}    {\"b\":[true,null]}\n"
        "[1][2]\n"
        "{\n  \"c\": \"x\"\n}\n"
        "42";
    bool done = false;
    callback_reader reader(c.client, parser, done);
    reader.start();
    return send_in_pieces(io, c.server, text, parser) && drain_until(io, [&done] { return done; });
}

// end::callback[]

}  // namespace

int main() {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    if (!ndjson_in_coroutine(io, acceptor) || !concatenated_with_callback(io, acceptor)) {
        return 1;
    }
}
