// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Two processes share documents without copying or parsing them: the producer formats a pool of
// blocks in Boost.Interprocess's shared memory, leases a block for each document, seals them and
// sends each reference's offset through a message queue; the consumer, this program run again,
// adopts each offset and prints the document it finds there. A damaged image is refused by its
// seal.

// tag::includes[]
#include <webcpp/document.hpp>
#include <webcpp/document/shared.hpp>

#include <boost/interprocess/detail/os_thread_functions.hpp>
#include <boost/interprocess/ipc/message_queue.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace wd = webcpp::document;
namespace bip = boost::interprocess;
// end::includes[]

namespace {

// tag::layout[]
// The room of each block's image, and the blocks of the pool: 8 blocks of 4096 bytes, the size of
// a parsed document's image.
constexpr std::size_t block_bytes = 4096;
constexpr std::size_t blocks = 8;

// The queue holds every offset at once, the end of the stream included.
constexpr std::size_t queue_messages = 8;

// The end of the stream: no document's image is at offset 0.
constexpr std::uint64_t end_of_stream = 0;
// end::layout[]

// tag::names[]
// The segment and the queue of one run, named by the producer's process id, so that two runs
// never share them.
struct names {
    std::string segment;
    std::string queue;
};

names names_for(std::string_view pid) {
    const std::string stem = "wcpp_img_" + std::string(pid);
    return {.segment = stem, .queue = stem + "_q"};
}

// Removes what a run may have left under the names; a name that is not there is no error.
void remove_names(const names& run) {
    static_cast<void>(bip::shared_memory_object::remove(run.segment.c_str()));
    static_cast<void>(bip::message_queue::remove(run.queue.c_str()));
}

// Removes the names when the producer's run ends, however it ends.
class remover {
public:
    explicit remover(names run) : run_(std::move(run)) {}

    remover(const remover&) = delete;
    remover& operator=(const remover&) = delete;
    remover(remover&&) = delete;
    remover& operator=(remover&&) = delete;

    ~remover() { remove_names(run_); }

private:
    names run_;
};

// end::names[]

// tag::produce[]
// The document a JSON text makes: parsed, its image copied into a block of the pool, then sealed.
wd::result<wd::shared_document<>> sealed_from_text(wd::shared_pool pool, std::string_view text) {
    wd::result<wd::document> parsed = wd::parse(text);
    if (parsed.has_error()) {
        return parsed.error();
    }
    wd::result<wd::shared_document<>> made =
        wd::shared_document<>::from_image(pool, parsed->image());
    if (made.has_error()) {
        return made.error();
    }
    if (made->seal().has_error()) {
        return wd::errc::type;
    }
    return made;
}

// Releases a document's reference and sends its offset, the document now the consumer's.
void hand_over(bip::message_queue& queue, wd::shared_document<>& doc) {
    const std::uint64_t offset = doc.release();
    queue.send(&offset, sizeof offset, 0);
}

// The producer: it makes the documents, hands each over and runs the consumer, which prints them.
int produce(const char* program) {
    const names run = names_for(std::to_string(bip::ipcdetail::get_current_process_id()));
    remove_names(run);
    const remover cleanup(run);

    const wd::result<std::size_t> size = wd::shared_pool::region_bytes(block_bytes, blocks);
    if (size.has_error()) {
        std::cout << "error: " << size.error().message() << '\n';
        return 1;
    }
    bip::shared_memory_object memory(bip::create_only, run.segment.c_str(), bip::read_write);
    memory.truncate(static_cast<bip::offset_t>(*size));
    const bip::mapped_region mapped(memory, bip::read_write);
    const std::span<std::byte> bytes(static_cast<std::byte*>(mapped.get_address()),
                                     mapped.get_size());
    const wd::result<wd::shared_pool> formatted = wd::shared_pool::format(bytes, block_bytes);
    if (formatted.has_error()) {
        std::cout << "error: " << formatted.error().message() << '\n';
        return 1;
    }
    const wd::shared_pool pool = *formatted;
    bip::message_queue queue(bip::create_only, run.queue.c_str(), queue_messages,
                             sizeof(std::uint64_t));

    // Three documents parsed or built in place, and a fourth whose sealed image is then damaged.
    wd::result<wd::shared_document<>> ada =
        sealed_from_text(pool, R"({"name":"Ada","born":1815,"langs":["Analytical"]})");
    wd::result<wd::shared_document<>> grace =
        sealed_from_text(pool, R"({"name":"Grace","langs":["COBOL"]})");
    wd::result<wd::shared_document<>> alan = wd::shared_document<>::create(pool);
    wd::result<wd::shared_document<>> edsger = sealed_from_text(pool, R"({"name":"Edsger"})");
    if (ada.has_error() || grace.has_error() || alan.has_error() || edsger.has_error()) {
        std::cout << "error: a document could not be made\n";
        return 1;
    }
    if (alan->root().set(wd::object{{"name", "Alan"}, {"n", 1500}}).has_error() ||
        alan->seal().has_error()) {
        std::cout << "error: a document could not be made\n";
        return 1;
    }

    // One byte of the sealed image changes after the seal; any change breaks a seal.
    constexpr std::size_t damaged_byte = 100;
    pool.region()[edsger->offset() + damaged_byte] ^= std::byte{0xff};

    hand_over(queue, *ada);
    hand_over(queue, *grace);
    hand_over(queue, *alan);
    hand_over(queue, *edsger);
    queue.send(&end_of_stream, sizeof end_of_stream, 0);

    std::cout << "available before the consumer: " << pool.available() << '\n' << std::flush;

    // The consumer is this program run again, as Boost.Interprocess's own tests run theirs; the
    // parent waits for it, so its output comes before the parent's last line.
    const std::string command =
        "\"" + std::string(program) + "\" consumer " + run.segment + " " + run.queue;
    // NOLINTNEXTLINE(bugprone-command-processor,cert-env33-c)
    if (std::system(command.c_str()) != 0) {
        std::cout << "error: the consumer failed\n";
        return 1;
    }
    std::cout << "available after the consumer: " << pool.available() << '\n';
    return 0;
}

// end::produce[]

// tag::consume[]
// The consumer: it adopts each offset the producer sends, prints the document and drops it, so
// that its block returns to the pool; an offset the seal refuses is dropped by the pool, unread.
int consume(const char* segment, const char* queue_name) {
    const bip::shared_memory_object memory(bip::open_only, segment, bip::read_write);
    const bip::mapped_region mapped(memory, bip::read_write);
    const std::span<std::byte> bytes(static_cast<std::byte*>(mapped.get_address()),
                                     mapped.get_size());
    const wd::result<wd::shared_pool> attached = wd::shared_pool::attach(bytes);
    if (attached.has_error()) {
        std::cout << "error: " << attached.error().message() << '\n';
        return 1;
    }
    const wd::shared_pool pool = *attached;
    bip::message_queue queue(bip::open_only, queue_name);

    for (;;) {
        std::uint64_t offset = 0;
        std::size_t received = 0;
        unsigned int priority = 0;
        queue.receive(&offset, sizeof offset, received, priority);
        if (offset == end_of_stream) {
            return 0;
        }

        wd::result<wd::shared_document<>> adopted = wd::shared_document<>::adopt(pool, offset);
        if (adopted.has_value()) {
            std::cout << adopted->get().stringify() << ": use count " << adopted->use_count()
                      << '\n';
        } else {
            std::cout << "refused: " << adopted.error().message() << '\n';
            // A refused reference stays counted until it is given back: the pool drops it by its
            // offset, without reading the image, and the block returns to the pool.
            if (const wd::result<void> dropped = pool.drop(offset); dropped.has_error()) {
                std::cout << "error: " << dropped.error().message() << '\n';
                return 1;
            }
        }
    }
}

// end::consume[]

}  // namespace

// tag::main[]
int main(int argc, char* argv[]) {
    if (argc == 4 && std::string_view(argv[1]) == "consumer") {
        return consume(argv[2], argv[3]);
    }
    return produce(argv[0]);
}

// end::main[]
