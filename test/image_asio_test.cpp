// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Images read and written with Boost.Asio over a loopback TCP connection: a writer sends several
// documents' images with async_write_image, each sealed before it is written, and the reader
// receives each into a pool's block, with a callback and, with exceptions, in a coroutine with
// use_awaitable; each equals its source. A pool of two blocks with two leases held refuses the
// third read with errc::capacity, without reading the stream, and reads the third image once a
// block is given back. An image cut short by the end of the stream is refused, and the end of a
// stream is eof, every time. A batch of images written with async_write_images in one call arrives
// whole, each image sealed; an empty batch and a batch that fails to seal write nothing.

#include <webcpp/document/asio.hpp>
#include <webcpp/document/image_stream.hpp>
#include <webcpp/document/parse.hpp>
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

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace asio = boost::asio;
namespace wd = webcpp::document;
using asio::ip::tcp;

namespace {

using outcomes = std::vector<std::string>;
using leased = wd::leased_document<>;

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

// A JSON array of thirty strings of sixty bytes: an image of about two kilobytes.
std::string wide_array() {
    std::string text = "[";
    for (int i = 0; i < 30; ++i) {
        text += (i == 0 ? "\"" : ",\"") + std::string(60, 'v') + "\"";
    }
    return text + "]";
}

// The size of each block of a test's pool: the largest image of `sources`, which is about 9 KB,
// fits one.
constexpr std::size_t block_bytes = 16384;

// The documents the writer sends, each parsed from JSON text and shrunk to its size.
std::vector<wd::document> sources() {
    const std::vector<std::string> texts{
        R"({"id":1})",
        R"({"tags":["a","b"],"name":"\u00e9"})",
        wide_array(),
    };
    std::vector<wd::document> documents;
    for (const std::string& text : texts) {
        auto parsed = wd::parse(text);
        webcpp::test::require(BOOST_TEST(parsed.has_value()));
        webcpp::test::require(BOOST_TEST(parsed->shrink_to_fit().has_value()));
        documents.push_back(std::move(*parsed));
    }
    return documents;
}

// A pool of `blocks` blocks of `block_bytes`.
wd::image_pool make_pool(std::size_t blocks) {
    auto made = wd::image_pool::create(block_bytes, blocks);
    webcpp::test::require(BOOST_TEST(made.has_value()));
    return std::move(*made);
}

// Whether a leased document is sealed and equals its source, byte for byte and as a document.
bool same(const leased& got, const wd::document& source) {
    const auto got_image = got.image();
    const auto source_image = source.image();
    return got.sealed() && std::ranges::equal(got_image, source_image) &&
           wd::stringify(got.get()) == wd::stringify(source);
}

// What one read completed with, as a test records it.
std::string outcome(const boost::system::error_code& ec, const leased& image,
                    const wd::document& source) {
    if (ec) {
        return "error " + ec.message();
    }
    if (image.empty()) {
        return "no document";
    }
    return same(image, source) ? "same" : "different";
}

// A writer that sends each document with async_write_image, the next one started after the last
// one completed, and shuts its end down after the last.
class callback_writer {
public:
    callback_writer(tcp::socket& socket, std::vector<wd::document>& documents)
        : socket_(socket), documents_(documents) {}

    void start() { write_next(); }

    std::size_t sent = 0;
    boost::system::error_code error;

private:
    void write_next() {
        if (sent == documents_.size()) {
            socket_.shutdown(tcp::socket::shutdown_send);
            return;
        }
        wd::async_write_image(socket_, documents_[sent], [this](boost::system::error_code ec) {
            if (ec) {
                error = ec;
                return;
            }
            ++sent;
            // Posted rather than called, so that no function of the writer calls itself.
            asio::post(socket_.get_executor(), [this] { write_next(); });
        });
    }

    tcp::socket& socket_;
    std::vector<wd::document>& documents_;
};

// A reader that reads each image with a callback into the pool, the next read started after the
// last one completed, until eof, which it records as "eof", or a hundred outcomes.
class callback_reader {
public:
    callback_reader(tcp::socket& socket, wd::image_pool& pool,
                    const std::vector<wd::document>& sources)
        : socket_(socket), pool_(pool), sources_(sources) {}

    void start() { read_next(); }

    outcomes read;

private:
    void read_next() {
        wd::async_read_image(socket_, pool_, [this](boost::system::error_code ec, leased image) {
            if (ec == asio::error::eof) {
                read.emplace_back("eof");
                return;
            }
            if (index_ == sources_.size()) {
                read.emplace_back("more than the sources");
                return;
            }
            read.push_back(outcome(ec, image, sources_[index_++]));
            // The lease goes back when `image` is destroyed, at the return.
            if (read.size() < 100) {
                asio::post(socket_.get_executor(), [this] { read_next(); });
            }
        });
    }

    tcp::socket& socket_;
    wd::image_pool& pool_;
    const std::vector<wd::document>& sources_;
    std::size_t index_ = 0;
};

// The outcome of one read, its error and its document, held until the test looks at it.
struct completion {
    boost::system::error_code ec;
    leased image;
};

// Starts one read and drains the context until it completes.
completion read_once(asio::io_context& io, tcp::socket& socket, wd::image_pool& pool) {
    std::optional<completion> done;
    wd::async_read_image(socket, pool, [&done](boost::system::error_code ec, leased image) {
        done.emplace(completion{.ec = ec, .image = std::move(image)});
    });
    drain_until(io, [&done] { return done.has_value(); });
    if (!done.has_value()) {
        return {};  // Unreached: drain_until has failed the test.
    }
    return std::move(*done);
}

// The bytes of all the images of the documents, which a batch write must send in full.
std::size_t image_bytes(const std::vector<wd::document>& documents) {
    std::size_t total = 0;
    for (const wd::document& document : documents) {
        total += document.image().size();
    }
    return total;
}

// What one batch write completed with, its error and the bytes it wrote.
struct batch_result {
    boost::system::error_code ec;
    std::size_t bytes = 0;
};

// Starts one batch write of the documents and drains the context until it completes.
batch_result write_once(asio::io_context& io, tcp::socket& socket,
                        std::vector<wd::document>& documents) {
    std::optional<batch_result> done;
    wd::async_write_images(socket, documents,
                           [&done](boost::system::error_code ec, std::size_t bytes) {
                               done.emplace(batch_result{.ec = ec, .bytes = bytes});
                           });
    drain_until(io, [&done] { return done.has_value(); });
    if (!done.has_value()) {
        return {};  // Unreached: drain_until has failed the test.
    }
    return *done;
}

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
// The batch written in a coroutine, awaiting async_write_images with as_tuple(use_awaitable).
asio::awaitable<void> write_batch_in_coroutine(tcp::socket& socket,
                                               std::vector<wd::document>& documents,
                                               batch_result& written) {
    auto [ec, bytes] =
        co_await wd::async_write_images(socket, documents, asio::as_tuple(asio::use_awaitable));
    written = batch_result{.ec = ec, .bytes = bytes};
    socket.shutdown(tcp::socket::shutdown_send);
}

// The writer in a coroutine, awaiting each async_write_image with use_awaitable.
asio::awaitable<void> write_in_coroutine(tcp::socket& socket,
                                         std::vector<wd::document>& documents) {
    for (wd::document& document : documents) {
        co_await wd::async_write_image(socket, document, asio::use_awaitable);
    }
    socket.shutdown(tcp::socket::shutdown_send);
}

// The reader in a coroutine: each image awaited with as_tuple(use_awaitable), then eof.
asio::awaitable<void> read_in_coroutine(tcp::socket& socket, wd::image_pool& pool,
                                        const std::vector<wd::document>& sources, outcomes& read) {
    for (const wd::document& source : sources) {
        auto [ec, image] =
            co_await wd::async_read_image(socket, pool, asio::as_tuple(asio::use_awaitable));
        read.push_back(outcome(ec, image, source));
    }
    auto [ec, image] =
        co_await wd::async_read_image(socket, pool, asio::as_tuple(asio::use_awaitable));
    read.emplace_back(ec == asio::error::eof ? "eof" : "more than the sources");
}
#endif

// Images sent by async_write_image are read by a callback into a pool of four blocks, each equal
// to its source and sealed; the end of the stream is eof, and every block is given back.
void a_callback_reads_images() {
    asio::io_context io;
    connection c(io);
    std::vector<wd::document> documents = sources();
    callback_writer writer(c.writer, documents);
    wd::image_pool pool = make_pool(4);
    callback_reader reader(c.reader, pool, documents);
    writer.start();
    reader.start();
    drain_until(io, [&reader] { return !reader.read.empty() && reader.read.back() == "eof"; });
    BOOST_TEST((reader.read == outcomes{"same", "same", "same", "eof"}));
    BOOST_TEST_EQ(writer.sent, documents.size());
    BOOST_TEST(!writer.error);
    BOOST_TEST_EQ(pool.available(), 4U);
}

// A pool of two blocks with two leases held refuses the third read with errc::capacity, which
// leaves the stream unread: once a block is given back, the third image is the next one read.
void backpressure() {
    asio::io_context io;
    connection c(io);
    std::vector<wd::document> documents = sources();
    for (wd::document& document : documents) {
        webcpp::test::require(BOOST_TEST(document.seal().has_value()));
        const auto image = document.image();
        asio::write(c.writer, asio::buffer(image.data(), image.size()));
    }
    wd::image_pool pool = make_pool(2);

    completion first = read_once(io, c.reader, pool);
    completion second = read_once(io, c.reader, pool);
    webcpp::test::require(BOOST_TEST(!first.ec && !second.ec));
    BOOST_TEST(same(first.image, documents[0]));
    BOOST_TEST(same(second.image, documents[1]));
    BOOST_TEST_EQ(pool.available(), 0U);

    const completion refused = read_once(io, c.reader, pool);
    BOOST_TEST(refused.ec == wd::errc::capacity);
    BOOST_TEST(refused.image.empty());

    first.image = wd::leased_document<>();
    BOOST_TEST_EQ(pool.available(), 1U);
    completion third = read_once(io, c.reader, pool);
    webcpp::test::require(BOOST_TEST(!third.ec && !third.image.empty()));
    BOOST_TEST(same(third.image, documents[2]));

    second.image = wd::leased_document<>();
    third.image = wd::leased_document<>();
    c.writer.shutdown(tcp::socket::shutdown_send);
    const completion end = read_once(io, c.reader, pool);
    BOOST_TEST(end.ec == asio::error::eof);
    BOOST_TEST(end.image.empty());
    BOOST_TEST_EQ(pool.available(), 2U);
}

// A sealed image with one byte flipped on the way is refused with errc::damaged_document, and no
// document is delivered for it.
void flipped_in_transit() {
    asio::io_context io;
    connection c(io);
    std::vector<wd::document> documents = sources();
    webcpp::test::require(BOOST_TEST(documents[0].seal().has_value()));
    const auto image = documents[0].image();
    std::vector<std::byte> sent(image.begin(), image.end());
    sent.back() ^= std::byte{0x01};
    asio::write(c.writer, asio::buffer(sent.data(), sent.size()));
    c.writer.shutdown(tcp::socket::shutdown_send);
    wd::image_pool pool = make_pool(2);
    const completion damaged = read_once(io, c.reader, pool);
    BOOST_TEST(damaged.ec == wd::errc::damaged_document);
    BOOST_TEST(damaged.image.empty());
    BOOST_TEST_EQ(pool.available(), 2U);
}

// An image the stream ends in the middle of is refused with errc::not_a_document.
void cut_short() {
    asio::io_context io;
    connection c(io);
    std::vector<wd::document> documents = sources();
    const auto image = documents[1].image();
    asio::write(c.writer, asio::buffer(image.data(), image.size() - 10));
    c.writer.shutdown(tcp::socket::shutdown_send);
    wd::image_pool pool = make_pool(2);
    const completion cut = read_once(io, c.reader, pool);
    BOOST_TEST(cut.ec == wd::errc::not_a_document);
    BOOST_TEST(cut.image.empty());
}

// The documents of one batch write arrive as one write of all their bytes, and are read back one
// by one, each equal to its source and sealed; the end of the stream is eof, and every block is
// given back.
void a_batch_is_written_in_one_call() {
    asio::io_context io;
    connection c(io);
    std::vector<wd::document> documents = sources();
    wd::image_pool pool = make_pool(4);
    const batch_result written = write_once(io, c.writer, documents);
    webcpp::test::require(BOOST_TEST(!written.ec));
    BOOST_TEST_EQ(written.bytes, image_bytes(documents));
    c.writer.shutdown(tcp::socket::shutdown_send);
    for (const wd::document& source : documents) {
        const completion got = read_once(io, c.reader, pool);
        webcpp::test::require(BOOST_TEST(!got.ec));
        BOOST_TEST(same(got.image, source));
    }
    const completion end = read_once(io, c.reader, pool);
    BOOST_TEST(end.ec == asio::error::eof);
    BOOST_TEST_EQ(pool.available(), 4U);
}

// An empty range completes with 0 bytes and writes nothing: the stream is left unread at its end.
void an_empty_batch_writes_nothing() {
    asio::io_context io;
    connection c(io);
    std::vector<wd::document> none;
    const batch_result written = write_once(io, c.writer, none);
    BOOST_TEST(!written.ec);
    BOOST_TEST_EQ(written.bytes, 0U);
    c.writer.shutdown(tcp::socket::shutdown_send);
    wd::image_pool pool = make_pool(1);
    const completion end = read_once(io, c.reader, pool);
    BOOST_TEST(end.ec == asio::error::eof);
    BOOST_TEST(end.image.empty());
}

// A document that cannot be sealed, as a moved-from one has no image, fails the batch with
// errc::type before any byte is written: the stream is left unread.
void a_seal_failure_writes_nothing() {
    asio::io_context io;
    connection c(io);
    std::vector<wd::document> documents = sources();
    const wd::document kept = std::move(documents[1]);
    const batch_result written = write_once(io, c.writer, documents);
    BOOST_TEST(written.ec == wd::errc::type);
    BOOST_TEST_EQ(written.bytes, 0U);
    c.writer.shutdown(tcp::socket::shutdown_send);
    wd::image_pool pool = make_pool(1);
    const completion end = read_once(io, c.reader, pool);
    BOOST_TEST(end.ec == asio::error::eof);
    BOOST_TEST(end.image.empty());
}

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
// The same batch, written in a coroutine with use_awaitable and read in one with as_tuple.
void batch_in_coroutines() {
    asio::io_context io;
    connection c(io);
    std::vector<wd::document> documents = sources();
    wd::image_pool pool = make_pool(4);
    outcomes read;
    batch_result written;
    asio::co_spawn(io, write_batch_in_coroutine(c.writer, documents, written), asio::detached);
    asio::co_spawn(io, read_in_coroutine(c.reader, pool, documents, read), asio::detached);
    drain_until(io, [&read] { return !read.empty() && read.back() == "eof"; });
    BOOST_TEST((read == outcomes{"same", "same", "same", "eof"}));
    BOOST_TEST(!written.ec);
    BOOST_TEST_EQ(written.bytes, image_bytes(documents));
    BOOST_TEST_EQ(pool.available(), 4U);
}

// The same images, written and read in coroutines with use_awaitable and as_tuple.
void coroutines() {
    asio::io_context io;
    connection c(io);
    std::vector<wd::document> documents = sources();
    wd::image_pool pool = make_pool(4);
    outcomes read;
    asio::co_spawn(io, write_in_coroutine(c.writer, documents), asio::detached);
    asio::co_spawn(io, read_in_coroutine(c.reader, pool, documents, read), asio::detached);
    drain_until(io, [&read] { return !read.empty() && read.back() == "eof"; });
    BOOST_TEST((read == outcomes{"same", "same", "same", "eof"}));
    BOOST_TEST_EQ(pool.available(), 4U);
}
#endif

}  // namespace

int main() {
    a_callback_reads_images();
    backpressure();
    flipped_in_transit();
    cut_short();
    a_batch_is_written_in_one_call();
    an_empty_batch_writes_nothing();
    a_seal_failure_writes_nothing();
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
    batch_in_coroutines();
    coroutines();
#endif
    return boost::report_errors();
}
