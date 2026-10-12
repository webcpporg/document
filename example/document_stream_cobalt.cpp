// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A stream of NDJSON read with Boost.Cobalt: one coroutine writes the lines to a loopback TCP
// connection in pieces of 1, 3, 7, 2 and 13 bytes, another reads each document with
// async_read_document and cobalt::use_op, until the end of the stream, which the last line
// ends without a newline.

#include <webcpp/document/asio.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/cobalt/join.hpp>
#include <boost/cobalt/main.hpp>
#include <boost/cobalt/op.hpp>
#include <boost/cobalt/result.hpp>
#include <boost/cobalt/task.hpp>
#include <boost/cobalt/this_thread.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>

// GCC 14 warns that Cobalt's task, a coroutine, deletes its frame with an operator delete that
// does not pair with the operator new that made it (-Wmismatched-new-delete); the pair is Cobalt's
// own, from its promise_memory_resource_base, and GCC 15 does not warn.
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ == 14
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

namespace asio = boost::asio;
namespace cobalt = boost::cobalt;
namespace wd = webcpp::document;
using asio::ip::tcp;

namespace {

// The bytes are the point: one document a line, and the line is the framing.
constexpr std::string_view ndjson = R"({"name":"Ada","tags":["math"],"n":1.5e3}
[1, 2.5, null, true, "x"]

"plain text"
{"z":1,"a":2}
{"last":true})";

// tag::send[]
// Sends the text in pieces of 1, 3, 7, 2 and 13 bytes in turn, then shuts the sending side of the
// connection, which is the end of the stream for the reader.
cobalt::task<void> send(tcp::socket& socket, std::string_view text) {
    constexpr std::array<std::size_t, 5> sizes{1, 3, 7, 2, 13};
    for (std::size_t turn = 0; !text.empty(); ++turn) {
        const std::size_t size = std::min(sizes[turn % sizes.size()], text.size());
        co_await asio::async_write(socket, asio::buffer(text.data(), size), cobalt::use_op);
        text.remove_prefix(size);
    }
    socket.shutdown(tcp::socket::shutdown_send);
}

// end::send[]

// tag::read[]
// Reads the documents one at a time and prints each with its number; the end of the stream is
// eof, which is not an error, and a failure of the stream is printed and ends the reader.
cobalt::task<void> read(tcp::socket& socket) {
    wd::stream_options options;
    options.framing = wd::framing::lines;
    wd::stream_parser parser(options);
    std::string storage;
    const auto buffer = asio::dynamic_buffer(storage);
    std::size_t count = 0;
    for (;;) {
        auto next = co_await cobalt::as_result(
            wd::async_read_document(socket, buffer, parser, cobalt::use_op));
        if (next.has_error()) {
            if (next.error() == asio::error::eof) {
                break;
            }
            std::cout << "error: " << next.error().message() << '\n';
            co_return;
        }
        ++count;
        std::cout << count << ' ' << next->stringify() << '\n';
    }
    std::cout << "eof after " << count << " documents\n";
}

// end::read[]

}  // namespace

// tag::main[]
// The connection is made before the coroutines start: the listening socket holds it until accept.
cobalt::main co_main(int /*argc*/, char** /*argv*/) {
    const auto executor = cobalt::this_thread::get_executor();
    tcp::acceptor acceptor(executor, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    tcp::socket writer(executor);
    tcp::socket reader(executor);
    reader.connect(acceptor.local_endpoint());
    acceptor.accept(writer);
    co_await cobalt::join(send(writer, ndjson), read(reader));
    co_return 0;
}

// end::main[]
