// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A stream of documents after an HTTP header read with Boost.Beast, over a loopback TCP pair in one
// program: the server answers a GET with a response header and an NDJSON body; the client reads the
// header with Beast into a multi_buffer, then reads the documents with async_read_document from the
// same multi_buffer. Beast reads the socket in blocks, so the first bytes of the body may already
// be in that buffer; the document reader takes them as its own input, so none are lost or copied.
// The context is drained with poll and restart, so the output does not depend on timing.

#include <webcpp/document.hpp>
#include <webcpp/document/asio.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/multi_buffer.hpp>
#include <boost/beast/http.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace wd = webcpp::document;
using asio::ip::tcp;

namespace {

// tag::connection[]
/**
 A TCP connection on the loopback address: the server's end, written to, and the client's end,
 read from.
*/
struct connection {
    connection(asio::io_context& io, tcp::acceptor& acceptor) : server(io), client(io) {
        client.connect(acceptor.local_endpoint());
        acceptor.accept(server);
    }

    tcp::socket server;
    tcp::socket client;
};

// end::connection[]

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

// tag::server[]
/**
 Writes `bytes` to the socket; false when the write failed, after printing the error.
*/
asio::awaitable<bool> send_bytes(tcp::socket& socket, std::string_view bytes) {
    auto [ec, sent] = co_await asio::async_write(socket, asio::buffer(bytes.data(), bytes.size()),
                                                 asio::as_tuple(asio::use_awaitable));
    if (ec) {
        std::cout << "server: " << ec.message() << '\n';
        co_return false;
    }
    co_return true;
}

/**
 Answers the client's GET: the response header, written by Beast, and the NDJSON body in pieces of
 2, 5, 3 and 11 bytes; the write side is then shut down, which ends the body for the client, since
 the response has no Content-Length.
*/
asio::awaitable<void> serve(tcp::socket& socket) {
    constexpr std::string_view body =
        "{\"id\":1,\"name\":\"ada\"}\n"
        "{\"id\":2,\"tags\":[\"a\",\"b\"]}\n"
        "[3,null]\n"
        "\"last\"";

    beast::multi_buffer buffer;
    http::request<http::empty_body> request;
    auto [read_ec, read_bytes] =
        co_await http::async_read(socket, buffer, request, asio::as_tuple(asio::use_awaitable));
    if (read_ec) {
        std::cout << "server: " << read_ec.message() << '\n';
        co_return;
    }

    // Without a Content-Length the body ends with the connection, so the connection is not kept.
    http::response<http::empty_body> response{http::status::ok, request.version()};
    response.set(http::field::content_type, "application/x-ndjson");
    response.keep_alive(false);
    std::ostringstream header;
    header << response;

    // The header and the body's first piece leave in one write, so the client's first read can take
    // both: the bytes past the header are then in the client's buffer when Beast returns.
    constexpr std::array<std::size_t, 4> sizes{2, 5, 3, 11};
    std::string_view rest = body;
    std::string first = header.str();
    const std::string_view head_piece = rest.substr(0, sizes[0]);
    rest.remove_prefix(head_piece.size());
    first.append(head_piece);
    if (!co_await send_bytes(socket, first)) {
        co_return;
    }
    for (std::size_t turn = 1; !rest.empty(); ++turn) {
        const std::string_view piece = rest.substr(0, sizes[turn % sizes.size()]);
        rest.remove_prefix(piece.size());
        if (!co_await send_bytes(socket, piece)) {
            co_return;
        }
    }
    socket.shutdown(tcp::socket::shutdown_send);
}

// end::server[]

// tag::client[]
/**
 Sends the GET, reads the response header into a multi_buffer with Beast and prints its status and
 content type; then reads the documents of the body from the same buffer, until eof.
*/
asio::awaitable<void> read_response(tcp::socket& socket) {
    http::request<http::empty_body> request{http::verb::get, "/documents", 11};
    request.set(http::field::host, "127.0.0.1");
    auto [request_ec, request_bytes] =
        co_await http::async_write(socket, request, asio::as_tuple(asio::use_awaitable));
    if (request_ec) {
        std::cout << "client: " << request_ec.message() << '\n';
        co_return;
    }

    // tag::header[]
    beast::multi_buffer buffer;
    http::response_parser<http::empty_body> header;
    auto [header_ec, header_bytes] = co_await http::async_read_header(
        socket, buffer, header, asio::as_tuple(asio::use_awaitable));
    if (header_ec) {
        std::cout << "client: " << header_ec.message() << '\n';
        co_return;
    }
    const http::response<http::empty_body>& response = header.get();
    std::cout << "status " << response.result_int() << ' ' << response.reason() << '\n';
    std::cout << "content type " << response[http::field::content_type] << '\n';
    // end::header[]
    // tag::documents[]
    // The buffer may already hold the first bytes of the body: whatever Beast read past the header
    // stays in it. The reader writes those bytes into the parser before it reads the socket again,
    // so the first document is parsed from them, and nothing is copied out of the buffer.
    wd::stream_options options;
    options.framing = wd::framing::lines;
    wd::stream_parser documents(options);
    for (;;) {
        auto [ec, made] = co_await wd::async_read_document(socket, buffer, documents,
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
    // end::documents[]
}

// end::client[]

}  // namespace

int main() {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    connection c(io, acceptor);
    bool server_done = false;
    bool client_done = false;
    asio::co_spawn(io, serve(c.server),
                   [&server_done](const std::exception_ptr&) { server_done = true; });
    asio::co_spawn(io, read_response(c.client),
                   [&client_done](const std::exception_ptr&) { client_done = true; });
    if (!drain_until(io, [&server_done, &client_done] { return server_done && client_done; })) {
        return 1;
    }
}
