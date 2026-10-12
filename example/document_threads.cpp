// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Threads that exchange documents without copying or parsing them: a producer parses documents and
// pushes them through a queue to a consumer, which keeps them; one parsed document is shared by
// reader threads, each holding a shared_document copy, over a pool in an ordinary buffer; and a
// writer that changes a shared document makes a copy of its own first, so the others keep the
// document as it was. The threads print nothing: main prints every result, in a fixed order, once
// they are joined.

// tag::includes[]
#include <webcpp/document.hpp>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wd = webcpp::document;
// end::includes[]

namespace {

// tag::queue[]
// A queue for one producer and one consumer: a push wakes the consumer, and close tells it that
// nothing more comes.
template <class T>
class queue {
public:
    void push(T value) {
        {
            const std::scoped_lock lock(mutex_);
            items_.push_back(std::move(value));
        }
        ready_.notify_one();
    }

    void close() {
        {
            const std::scoped_lock lock(mutex_);
            closed_ = true;
        }
        ready_.notify_all();
    }

    // The oldest value, waiting while the queue is open; nothing once it is closed and empty.
    std::optional<T> pop() {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return closed_ || !items_.empty(); });
        if (items_.empty()) {
            return std::nullopt;
        }
        T value = std::move(items_.front());
        items_.pop_front();
        return value;
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<T> items_;
    bool closed_ = false;
};

// end::queue[]

// tag::exchange[]
// What the producer sends: a document, and the address of its image before the push. Moving a
// document moves its handle and never the bytes of its image, so the consumer finds them there.
struct parcel {
    wd::document doc;
    const std::byte* image = nullptr;
};

// What the consumer keeps: the documents in arrival order, how many of them still have their image
// where the producer left it, and how many texts failed to parse.
struct exchanged {
    std::vector<wd::document> documents;
    std::size_t same_image = 0;
    std::size_t failed = 0;
};

// The producer parses each text and pushes the document; the consumer keeps each one it receives.
// The two threads write different members of the result, and main reads it after both are joined.
exchanged through_a_queue(std::array<std::string_view, 3> texts) {
    queue<parcel> arrivals;
    exchanged result;
    std::thread consumer([&arrivals, &result] {
        while (std::optional<parcel> next = arrivals.pop()) {
            if (next->doc.image().data() == next->image) {
                ++result.same_image;
            }
            result.documents.push_back(std::move(next->doc));
        }
    });
    std::thread producer([&arrivals, &result, texts] {
        for (const std::string_view text : texts) {
            wd::result<wd::document> parsed = wd::parse(text);
            if (parsed.has_error()) {
                ++result.failed;
                continue;
            }
            const std::byte* image = parsed->image().data();
            arrivals.push(parcel{.doc = std::move(*parsed), .image = image});
        }
        arrivals.close();
    });
    producer.join();
    consumer.join();
    return result;
}

// end::exchange[]

// tag::readers[]
// The name and birth year of a shared document, read through a const reference: a reader never
// writes.
std::string describe(const wd::shared_document<>& doc) {
    return std::string(doc["name"].value_or(std::string_view("nobody"))) + " was born in " +
           std::to_string(doc["born"].value_or(std::int64_t{0}));
}

// A reader holds its copy until it returns, then drops it in its own thread.
void reader_task(const wd::shared_document<>& holder, std::string& out) {
    out = describe(holder);
}

// end::readers[]

// tag::copy_on_write[]
// make_mutable copies the image into a block of the writer's own when other holders exist, so the
// writer changes its copy and the others keep the document as it was. The library does not
// synchronise writers: a block with several writers, or a writer and readers of that same block,
// must wait for each other by the program's own means, such as a mutex. This writer changes only
// its copy, which no other holder reads.
void writer_task(wd::shared_document<> holder, std::string& out) {
    if (holder.make_mutable().has_error() || holder["name"].set("Lovelace").has_error()) {
        out = "error";
        return;
    }
    out = describe(holder);
}

// end::copy_on_write[]

}  // namespace

int main() {
    // The first region: documents parsed by the producer arrive at the consumer in order.
    const exchanged received = through_a_queue(
        {R"({"id":1,"name":"Ada"})", "[1,2,3]", R"({"id":3,"tags":["queue","thread"]})"});
    std::cout << "queue: " << received.documents.size() << " received, " << received.same_image
              << " with the image the producer sent, " << received.failed << " failed to parse\n";
    for (const wd::document& doc : received.documents) {
        std::cout << doc.stringify() << '\n';
    }

    // The second and third regions share one document, stored in a pool over a buffer the program
    // owns: 16-aligned, as a pool requires.
    alignas(16) std::array<std::byte, std::size_t{16} * 1024> buffer{};
    const wd::result<wd::shared_pool> formatted = wd::shared_pool::format(buffer, 2048);
    wd::result<wd::document> parsed = wd::parse(R"({"name":"Ada","born":1815})");
    if (formatted.has_error() || parsed.has_error()) {
        std::cout << "error: the pool or the document was refused\n";
        return 1;
    }
    // A parsed image keeps the room the parser gave it, 4096 bytes at first; shrink_to_fit packs it
    // to its content, so that it fits a block of 2048.
    if (parsed->shrink_to_fit().has_error()) {
        std::cout << "error: the document did not shrink\n";
        return 1;
    }
    const wd::shared_pool pool = *formatted;
    std::cout << "pool: " << pool.blocks() << " blocks of " << pool.block_bytes() << " bytes\n";

    // tag::readers_main[]
    // The image is copied into a block of the pool; the document it makes holds the one reference.
    wd::result<wd::shared_document<>> stored =
        wd::shared_document<>::from_image(pool, parsed->image());
    if (stored.has_error()) {
        std::cout << "error: " << stored.error().message() << '\n';
        return 1;
    }
    wd::shared_document<> shared = std::move(*stored);
    std::cout << "stored: use_count " << shared.use_count() << ", " << pool.available() << " of "
              << pool.blocks() << " blocks free\n";

    // Each reader gets its copy here, before any thread starts, so the count printed is exact.
    constexpr std::size_t reader_count = 4;
    std::vector<wd::shared_document<>> copies(reader_count, shared);
    std::cout << "with " << reader_count << " readers: use_count " << shared.use_count() << '\n';

    std::array<std::string, reader_count> read{};
    std::vector<std::thread> readers;
    readers.reserve(reader_count);
    for (std::size_t i = 0; i < reader_count; ++i) {
        readers.emplace_back(reader_task, std::move(copies[i]), std::ref(read[i]));
    }
    for (std::thread& reader : readers) {
        reader.join();
    }
    std::cout << "after the readers: use_count " << shared.use_count() << ", " << pool.available()
              << " of " << pool.blocks() << " blocks free\n";
    for (std::size_t i = 0; i < reader_count; ++i) {
        std::cout << "reader " << i << ": " << read[i] << '\n';
    }
    // end::readers_main[]

    // tag::copy_on_write_main[]
    // The writer takes its reference before its thread starts; its change leaves the shared
    // document, which main still reads, as it was.
    wd::shared_document<> writer_copy = shared;
    std::string written;
    std::thread writer(writer_task, std::move(writer_copy), std::ref(written));
    writer.join();
    std::cout << "writer: " << written << '\n';
    std::cout << "after the writer: " << describe(shared) << ", use_count " << shared.use_count()
              << ", " << pool.available() << " of " << pool.blocks() << " blocks free\n";

    // The last copy gives its block back to the pool, zeroed.
    shared = wd::shared_document<>();
    std::cout << "last copy dropped: " << pool.available() << " of " << pool.blocks()
              << " blocks free\n";
    // end::copy_on_write_main[]
}
