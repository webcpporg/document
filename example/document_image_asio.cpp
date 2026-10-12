// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Images on the wire, from a source the program trusts: a server sends three documents' sealed
// images with async_write_image, and a client reads them with async_read_image into a pool of two
// blocks, each opened in place, with no parsing and no allocation on the receiving side. A
// coroutine reads the first stream and a callback the second; each prints its document with
// stringify while the image is held in its block, and the blocks the pool has free then. A third
// stream carries the three images in one gather write, with async_write_images. The context is
// drained with poll and restart, so the output does not depend on timing.

#include <webcpp/document.hpp>
#include <webcpp/document/asio.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <cstddef>
#include <iostream>
#include <string_view>
#include <vector>

namespace asio = boost::asio;
namespace wd = webcpp::document;
using asio::ip::tcp;

namespace {

// The size of each block: the largest image of the documents, which is well under it.
constexpr std::size_t block_bytes = 4096;

// The documents the server sends, as JSON text laid out for the reader.
constexpr std::string_view ada = R"({
    "name": "Ada",
    "born": 1815,
    "langs": ["Analytical"]
})";

constexpr std::string_view grace = R"({
    "name": "Grace",
    "langs": ["COBOL"]
})";

constexpr std::string_view alan = R"({
    "name": "Alan",
    "n": 1500
})";

// tag::documents[]
/**
 A document parsed from JSON text and shrunk to its size, so that its image holds what the document
 needs and no more.
*/
wd::document document_of(std::string_view text) {
    wd::document made = wd::parse_or_throw(text);
    // A shrink that fails leaves the document as valid as it was, only larger.
    static_cast<void>(made.shrink_to_fit());
    return made;
}

/** The documents the server sends, in order. */
std::vector<wd::document> documents() {
    std::vector<wd::document> sent;
    sent.push_back(document_of(ada));
    sent.push_back(document_of(grace));
    sent.push_back(document_of(alan));
    return sent;
}

// end::documents[]

// tag::pool[]
/** A pool of two blocks: the most the client leases at once. */
wd::image_pool pool_of_two() {
    return wd::image_pool::create(block_bytes, 2).value();
}

// end::pool[]

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

// tag::held[]
/**
 Prints a document as the client holds it: its text, whether its image is sealed, and the blocks the
 pool has free while the image's block is leased.
*/
void print_held(const wd::leased_document<>& image, const wd::image_pool& pool) {
    std::cout << image.get().stringify() << ": " << (image.sealed() ? "sealed" : "not sealed")
              << ", blocks available: " << pool.available() << '\n';
}

// end::held[]

// tag::coroutine[]
/** Sends each document's image, awaiting each write, then shuts the write side down. */
asio::awaitable<void> send_in_coroutine(tcp::socket& server, std::vector<wd::document>& sent) {
    for (wd::document& document : sent) {
        auto [ec] =
            co_await wd::async_write_image(server, document, asio::as_tuple(asio::use_awaitable));
        if (ec) {
            std::cout << "error: " << ec.message() << '\n';
            co_return;
        }
    }
    server.shutdown(tcp::socket::shutdown_send);
}

/**
 Receives images until eof, awaiting each read in turn. Each image is printed while its block is
 leased, and the block goes back when the loop's iteration ends.
*/
asio::awaitable<void> receive_in_coroutine(tcp::socket& client, wd::image_pool& pool, bool& done) {
    for (;;) {
        auto [ec, image] =
            co_await wd::async_read_image(client, pool, asio::as_tuple(asio::use_awaitable));
        if (ec == asio::error::eof) {
            std::cout << "eof, blocks available: " << pool.available() << '\n';
            done = true;
            co_return;
        }
        if (ec) {
            std::cout << "error: " << ec.message() << '\n';
            done = true;
            co_return;
        }
        print_held(image, pool);
    }
}

/** Sends the documents over a link and receives them in a coroutine. */
bool images_in_coroutine(asio::io_context& io, tcp::acceptor& acceptor,
                         std::vector<wd::document>& sent) {
    std::cout << "images, received in a coroutine:\n";
    link c(io, acceptor);
    wd::image_pool pool = pool_of_two();
    bool done = false;
    asio::co_spawn(io, send_in_coroutine(c.server, sent), asio::detached);
    asio::co_spawn(io, receive_in_coroutine(c.client, pool, done), asio::detached);
    return drain_until(io, [&done] { return done; });
}

// end::coroutine[]

// tag::callback[]
/**
 Sends each document's image with async_write_image, the next one started after the last one
 completed, then shuts the write side down. The next write is posted, not called, so that no write
 starts inside the callback of the one before it.
*/
class callback_sender {
public:
    callback_sender(tcp::socket& server, std::vector<wd::document>& sent)
        : server_(server), sent_(sent) {}

    /** Starts the first write. */
    void start() { send_next(); }

private:
    void send_next() {
        if (next_ == sent_.size()) {
            server_.shutdown(tcp::socket::shutdown_send);
            return;
        }
        wd::async_write_image(server_, sent_[next_], [this](boost::system::error_code ec) {
            if (ec) {
                std::cout << "error: " << ec.message() << '\n';
                return;
            }
            ++next_;
            asio::post(server_.get_executor(), [this] { send_next(); });
        });
    }

    tcp::socket& server_;
    std::vector<wd::document>& sent_;
    std::size_t next_ = 0;
};

/**
 Receives images with async_read_image, the next read started after the last one completed, until
 eof. The image is printed inside its callback, where its block is leased; the block goes back when
 the callback returns.
*/
class callback_receiver {
public:
    callback_receiver(tcp::socket& client, wd::image_pool& pool, bool& done)
        : client_(client), pool_(pool), done_(done) {}

    /** Starts the first read. */
    void start() { read_next(); }

private:
    void read_next() {
        wd::async_read_image(
            client_, pool_, [this](boost::system::error_code ec, wd::leased_document<> image) {
                if (ec == asio::error::eof) {
                    std::cout << "eof, blocks available: " << pool_.available() << '\n';
                    done_ = true;
                    return;
                }
                if (ec) {
                    std::cout << "error: " << ec.message() << '\n';
                    done_ = true;
                    return;
                }
                print_held(image, pool_);
                asio::post(client_.get_executor(), [this] { read_next(); });
            });
    }

    tcp::socket& client_;
    wd::image_pool& pool_;
    bool& done_;
};

/** Sends the documents over a link and receives them with a callback. */
bool images_with_callback(asio::io_context& io, tcp::acceptor& acceptor,
                          std::vector<wd::document>& sent) {
    std::cout << "images, received with a callback:\n";
    link c(io, acceptor);
    wd::image_pool pool = pool_of_two();
    bool done = false;
    callback_sender sender(c.server, sent);
    callback_receiver receiver(c.client, pool, done);
    sender.start();
    receiver.start();
    return drain_until(io, [&done] { return done; });
}

// end::callback[]

// tag::batch[]
/**
 Sends the three images in one gather write with async_write_images, then receives them in a
 coroutine, as the first exchange does.

 Tip: the write reads the images in place and completes with the bytes it wrote, so the documents
 stay unchanged until it completes.
*/
bool images_in_one_write(asio::io_context& io, tcp::acceptor& acceptor,
                         std::vector<wd::document>& sent) {
    std::cout << "images, sent in one write:\n";
    link c(io, acceptor);
    wd::image_pool pool = pool_of_two();
    bool done = false;
    wd::async_write_images(c.server, sent, [&c](boost::system::error_code ec, std::size_t bytes) {
        if (ec) {
            std::cout << "error: " << ec.message() << '\n';
        } else {
            std::cout << "written: " << bytes << " bytes\n";
        }
        c.server.shutdown(tcp::socket::shutdown_send);
    });
    asio::co_spawn(io, receive_in_coroutine(c.client, pool, done), asio::detached);
    return drain_until(io, [&done] { return done; });
}

// end::batch[]

}  // namespace

int main() {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    std::vector<wd::document> sent = documents();
    if (!images_in_coroutine(io, acceptor, sent) || !images_with_callback(io, acceptor, sent) ||
        !images_in_one_write(io, acceptor, sent)) {
        return 1;
    }
}
