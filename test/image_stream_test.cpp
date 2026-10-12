// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A stream of images read into a pool's blocks and opened in place: several images in one write,
// split at every byte, one byte at a time and through prepare and commit; the pool's backpressure;
// a reused block that holds nothing of the last document; headers refused, a stream cut short, an
// image larger than a block and garbage, each stopping the reader until its reset; leases moved
// and destroyed in any order; a pool and a reader moved while blocks are leased; and no allocation
// while a stream is read, every global operator new of this program counted.

#include <webcpp/document/image_stream.hpp>

#include <webcpp/document/document.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/stringify.hpp>

#include <boost/config.hpp>
#include <boost/core/lightweight_test.hpp>

#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#include <malloc.h>
#endif

namespace wd = webcpp::document;

namespace {

// The calls of the global operator new this program has made. It is mutable global state, which
// no library holds: it is this test program's own instrument, the one place where the replaced
// operator new below can record what it sees.
std::size_t global_allocations = 0;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// Memory of `size` bytes aligned to `alignment`; the UCRT of MSVC has no aligned_alloc, and memory
// from its _aligned_malloc is given back with _aligned_free.
void* aligned_memory(std::size_t size, std::size_t alignment) {
    const std::size_t rounded =
        size == 0 ? alignment : (size + alignment - 1) / alignment * alignment;
#ifdef _MSC_VER
    return _aligned_malloc(rounded, alignment);
#else
    return std::aligned_alloc(alignment, rounded);
#endif
}

// Gives back what aligned_memory took.
void free_aligned_memory(void* memory) noexcept {
#ifdef _MSC_VER
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

}  // namespace

// The replacements of the global operator new and operator delete, for this program alone, which
// count every allocation, an aligned one included: the forms without std::nothrow_t, which the
// standard library's nothrow forms call. The standard library declares them with parameter names
// reserved to the implementation, hence the exemption on each. None is ever inlined: GCC, seeing
// the malloc or the free inside one, would pair it with the other's call and warn of a mismatch.

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
BOOST_NOINLINE void* operator new(std::size_t size) {
    ++global_allocations;
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) {
        std::abort();
    }
    return memory;
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
BOOST_NOINLINE void operator delete(void* memory) noexcept {
    std::free(memory);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
BOOST_NOINLINE void operator delete(void* memory, std::size_t /*size*/) noexcept {
    std::free(memory);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
BOOST_NOINLINE void* operator new(std::size_t size, std::align_val_t alignment) {
    ++global_allocations;
    void* memory = aligned_memory(size, static_cast<std::size_t>(alignment));
    if (memory == nullptr) {
        std::abort();
    }
    return memory;
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
BOOST_NOINLINE void operator delete(void* memory, std::align_val_t /*alignment*/) noexcept {
    free_aligned_memory(memory);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
BOOST_NOINLINE void operator delete(void* memory, std::size_t /*size*/,
                                    std::align_val_t /*alignment*/) noexcept {
    free_aligned_memory(memory);
}

namespace {

// Documents of several sizes, each shrunk to its content so that their images differ in size.
std::vector<wd::document> make_sources() {
    const std::array<std::string_view, 4> texts{
        R"({"a":1,"b":[true,false,null],"c":"text"})",
        R"([1,2,3])",
        R"("a string")",
        R"({"nested":{"deeper":{"deepest":[1,{"x":"y"}]}},"n":-12.5e3})",
    };
    std::vector<wd::document> sources;
    for (const std::string_view text : texts) {
        auto parsed = wd::parse(text);
        webcpp::test::require(BOOST_TEST(parsed.has_value()));
        webcpp::test::require(BOOST_TEST(parsed->shrink_to_fit().has_value()));
        sources.push_back(std::move(*parsed));
    }
    // A larger one, with room left in its image.
    auto large = wd::document::create();
    webcpp::test::require(BOOST_TEST(large.has_value()));
    webcpp::test::require(BOOST_TEST(large->root().set(wd::object()).has_value()));
    for (std::uint64_t i = 0; i < 20; ++i) {
        webcpp::test::require(
            BOOST_TEST((*large)["k" + std::to_string(i)].set(std::string(40, 'v')).has_value()));
    }
    sources.push_back(std::move(*large));
    return sources;
}

// The images of `sources`, one after another.
std::vector<std::byte> concatenate(const std::vector<wd::document>& sources) {
    std::vector<std::byte> stream;
    for (const wd::document& source : sources) {
        const std::span<const std::byte> image = source.image();
        stream.insert(stream.end(), image.begin(), image.end());
    }
    return stream;
}

// The largest image of `sources`, rounded up to 16.
std::size_t largest_image(const std::vector<wd::document>& sources) {
    std::size_t largest = 0;
    for (const wd::document& source : sources) {
        largest = std::max(largest, source.image().size());
    }
    return (largest + 15) / 16 * 16;
}

wd::image_pool make_pool(std::size_t block_bytes, std::size_t blocks) {
    auto made = wd::image_pool::create(block_bytes, blocks);
    webcpp::test::require(BOOST_TEST(made.has_value()));
    return std::move(*made);
}

// Whether a leased document is its source read back: the same image, byte for byte, and the same
// value.
bool same(const wd::leased_document<>& lease, const wd::document& source) {
    return std::ranges::equal(lease.image(), source.image()) &&
           wd::stringify(lease.get()) == wd::stringify(source);
}

// Whether the bytes hold `needle` anywhere.
bool holds(std::span<const std::byte> bytes, std::string_view needle) {
    const auto found = std::ranges::search(bytes, std::as_bytes(std::span(needle)));
    return !found.empty();
}

// Takes every document the reader holds.
std::vector<wd::leased_document<>> take_all(wd::image_reader& reader) {
    std::vector<wd::leased_document<>> taken;
    for (;;) {
        auto next = reader.next();
        webcpp::test::require(BOOST_TEST(next.has_value()));
        std::optional<wd::leased_document<>>& document = *next;
        if (!document.has_value()) {
            return taken;
        }
        taken.push_back(std::move(*document));
    }
}

// Takes the next document the reader holds, which the test needs.
wd::leased_document<> take_one(wd::image_reader& reader) {
    auto next = reader.next();
    webcpp::test::require(BOOST_TEST(next.has_value()));
    std::optional<wd::leased_document<>>& document = *next;
    if (!document.has_value()) {
        BOOST_ERROR("the reader holds no document");
        std::exit(boost::report_errors());
    }
    return std::move(*document);
}

void several_images_in_one_write() {
    const std::vector<wd::document> sources = make_sources();
    const std::vector<std::byte> stream = concatenate(sources);
    wd::image_pool pool = make_pool(largest_image(sources), sources.size());
    wd::image_reader reader(pool);

    const auto written = reader.write(stream);
    if (!BOOST_TEST(written.has_value())) {
        return;
    }
    BOOST_TEST_EQ(*written, stream.size());
    BOOST_TEST_EQ(reader.offset(), stream.size());
    BOOST_TEST_EQ(reader.documents(), sources.size());
    BOOST_TEST_EQ(pool.available(), 0U);

    const std::vector<wd::leased_document<>> taken = take_all(reader);
    if (!BOOST_TEST_EQ(taken.size(), sources.size())) {
        return;
    }
    for (std::size_t i = 0; i < sources.size(); ++i) {
        BOOST_TEST(same(taken[i], sources[i]));
    }
    BOOST_TEST(reader.finish().has_value());
}

void every_split() {
    const std::vector<wd::document> sources = make_sources();
    const std::vector<std::byte> stream = concatenate(sources);
    wd::image_pool pool = make_pool(largest_image(sources), 2);
    const std::span<const std::byte> all(stream);

    for (std::size_t split = 0; split <= stream.size(); ++split) {
        wd::image_reader reader(pool);
        std::size_t delivered = 0;
        bool equal = true;
        // The push form: each document is checked and given back before the next needs a block.
        const auto on_document = [&](wd::leased_document<>&& lease) {
            equal = equal && delivered < sources.size() && same(lease, sources[delivered]);
            ++delivered;
        };
        const auto first = reader.write(all.first(split), on_document);
        const auto second = reader.write(all.subspan(split), on_document);
        if (!BOOST_TEST(first.has_value() && second.has_value())) {
            return;
        }
        BOOST_TEST_EQ(*first + *second, stream.size());
        BOOST_TEST(reader.finish().has_value());
        if (!BOOST_TEST(equal && delivered == sources.size())) {
            return;
        }
        BOOST_TEST_EQ(pool.available(), 2U);
    }
}

void one_byte_at_a_time() {
    const std::vector<wd::document> sources = make_sources();
    const std::vector<std::byte> stream = concatenate(sources);
    wd::image_pool pool = make_pool(largest_image(sources), sources.size());
    wd::image_reader reader(pool);

    std::vector<wd::leased_document<>> taken;
    for (const std::byte b : stream) {
        const auto written = reader.write(std::span(&b, 1));
        if (!BOOST_TEST(written.has_value() && *written == 1)) {
            return;
        }
        std::vector<wd::leased_document<>> more = take_all(reader);
        std::ranges::move(more, std::back_inserter(taken));
    }
    BOOST_TEST(reader.finish().has_value());
    if (!BOOST_TEST_EQ(taken.size(), sources.size())) {
        return;
    }
    for (std::size_t i = 0; i < sources.size(); ++i) {
        BOOST_TEST(same(taken[i], sources[i]));
    }
}

// The zero-copy form: the stream's bytes are read straight into the room prepare gives, a few at
// a time, as a socket's reads would land.
void prepare_and_commit() {
    const std::vector<wd::document> sources = make_sources();
    const std::vector<std::byte> stream = concatenate(sources);
    wd::image_pool pool = make_pool(largest_image(sources), sources.size());
    wd::image_reader reader(pool);

    std::size_t at = 0;
    std::size_t completed = 0;
    while (at < stream.size()) {
        auto room = reader.prepare();
        if (!BOOST_TEST(room.has_value() && !room->empty())) {
            return;
        }
        // The header first, then the rest of its image: a read never crosses an image's end.
        const std::size_t n = std::min({room->size(), stream.size() - at, std::size_t{37}});
        std::memcpy(room->data(), stream.data() + at, n);
        at += n;
        const auto committed = reader.commit(n);
        if (!BOOST_TEST(committed.has_value())) {
            return;
        }
        completed += *committed;
    }
    BOOST_TEST_EQ(completed, sources.size());
    const std::vector<wd::leased_document<>> taken = take_all(reader);
    if (!BOOST_TEST_EQ(taken.size(), sources.size())) {
        return;
    }
    for (std::size_t i = 0; i < sources.size(); ++i) {
        BOOST_TEST(same(taken[i], sources[i]));
    }

    // With every block leased, a new reader has no room.
    wd::image_reader again(pool);
    BOOST_TEST_EQ(pool.available(), 0U);
    BOOST_TEST(again.prepare().error() == wd::errc::capacity);
}

void backpressure() {
    const std::vector<wd::document> sources = make_sources();
    const std::vector<std::byte> stream = concatenate(sources);
    wd::image_pool pool = make_pool(largest_image(sources), 2);
    wd::image_reader reader(pool);
    const std::span<const std::byte> all(stream);

    // Two blocks hold the first two images; the third waits for a block.
    const std::size_t two = sources[0].image().size() + sources[1].image().size();
    const auto written = reader.write(all);
    if (!BOOST_TEST(written.has_value())) {
        return;
    }
    BOOST_TEST_EQ(*written, two);
    BOOST_TEST_EQ(pool.available(), 0U);
    BOOST_TEST(pool.acquire().error() == wd::errc::capacity);

    std::vector<wd::leased_document<>> held = take_all(reader);
    if (!BOOST_TEST_EQ(held.size(), 2U)) {
        return;
    }
    // Leases held, the pool stays full: nothing more is taken, and nothing is lost.
    const auto refused = reader.write(all.subspan(two));
    BOOST_TEST(refused.has_value() && *refused == 0);
    BOOST_TEST(reader.prepare().error() == wd::errc::capacity);

    // A lease given back makes room for one more image.
    held.erase(held.begin());
    BOOST_TEST_EQ(pool.available(), 1U);
    const std::size_t third = sources[2].image().size();
    const auto resumed = reader.write(all.subspan(two));
    if (!BOOST_TEST(resumed.has_value())) {
        return;
    }
    BOOST_TEST_EQ(*resumed, third);
    BOOST_TEST(same(take_one(reader), sources[2]));
    BOOST_TEST(same(held.front(), sources[1]));
}

void a_reused_block_holds_nothing_of_the_last() {
    // A document with a secret in every member, and a small one read after it into the same block.
    auto secret = wd::document::create();
    webcpp::test::require(BOOST_TEST(secret.has_value()));
    webcpp::test::require(BOOST_TEST(secret->root().set(wd::object()).has_value()));
    for (std::uint64_t i = 0; i < 30; ++i) {
        webcpp::test::require(BOOST_TEST(
            (*secret)["k" + std::to_string(i)].set(std::string("SECRET-") + std::to_string(i))));
    }
    auto small = wd::parse(R"({"a":1})");
    webcpp::test::require(BOOST_TEST(small.has_value() && small->shrink_to_fit().has_value()));
    BOOST_TEST_LT(small->image().size(), secret->image().size());

    wd::image_pool pool = make_pool(secret->image().size(), 1);
    {
        wd::image_reader reader(pool);
        BOOST_TEST(reader.write(secret->image()).has_value());
        wd::leased_document<> lease = take_one(reader);
        BOOST_TEST(holds(lease.image(), "SECRET-"));
        // Written through the lease too, which the release zeroes with the rest.
        BOOST_TEST(lease["k0"].set("SECRET-written").has_value());
    }
    {
        // A block given back is zero, whole.
        auto block = pool.acquire();
        if (!BOOST_TEST(block.has_value())) {
            return;
        }
        BOOST_TEST(
            std::ranges::all_of(block->bytes(), [](std::byte b) { return b == std::byte{}; }));
    }

    wd::image_reader reader(pool);
    BOOST_TEST(reader.write(small->image()).has_value());
    const wd::leased_document<> lease = take_one(reader);
    BOOST_TEST(same(lease, *small));
    BOOST_TEST_EQ(lease.image().size(), small->image().size());
    BOOST_TEST(!holds(lease.image(), "SECRET-"));
    BOOST_TEST(lease["k0"].is_undefined());
}

// An image of the default traits, its header rewritten by `edit`.
template <class Edit>
std::vector<std::byte> edited_image(Edit edit) {
    auto doc = wd::parse(R"({"a":[1,2]})");
    webcpp::test::require(BOOST_TEST(doc.has_value() && doc->shrink_to_fit().has_value()));
    std::vector<std::byte> image(doc->image().begin(), doc->image().end());
    wd::image_header header{};
    std::memcpy(&header, image.data(), sizeof header);
    edit(header);
    std::memcpy(image.data(), &header, sizeof header);
    return image;
}

// A stream that `reader` refuses with `refused`: the documents before it are delivered, then the
// reader stops with the error, and a reset reads a good stream again.
void refused_then_reset(const std::vector<std::byte>& bad, wd::errc refused) {
    const std::vector<wd::document> sources = make_sources();
    const std::vector<std::byte> good = concatenate(sources);
    wd::image_pool pool = make_pool(largest_image(sources) + 4096, sources.size() + 1);
    wd::image_reader reader(pool);

    std::vector<std::byte> stream(
        good.begin(), good.begin() + static_cast<std::ptrdiff_t>(sources[0].image().size()));
    stream.insert(stream.end(), bad.begin(), bad.end());
    const auto written = reader.write(stream);
    BOOST_TEST(written.error() == refused);

    // The document before it, then the error, which stays.
    const wd::leased_document<> first = take_one(reader);
    BOOST_TEST(same(first, sources[0]));
    BOOST_TEST(reader.next().error() == refused);
    BOOST_TEST(reader.write(good).error() == refused);
    BOOST_TEST(reader.prepare().error() == refused);
    BOOST_TEST(reader.finish().error() == refused);
    // Only the lease taken holds a block.
    BOOST_TEST_EQ(pool.available(), pool.blocks() - 1);

    reader.reset();
    BOOST_TEST_EQ(reader.offset(), 0U);
    BOOST_TEST_EQ(reader.documents(), 0U);
    const auto again = reader.write(good);
    BOOST_TEST(again.has_value() && *again == good.size());
    const std::vector<wd::leased_document<>> taken = take_all(reader);
    BOOST_TEST_EQ(taken.size(), sources.size());
    BOOST_TEST(reader.finish().has_value());
}

void refused_streams() {
    // An image of other traits.
    auto js = wd::parse<wd::js_traits>(R"({"a":1})");
    webcpp::test::require(BOOST_TEST(js.has_value()));
    const std::vector<std::byte> other_traits(js->image().begin(), js->image().end());
    refused_then_reset(other_traits, wd::errc::incompatible_document);

    // Another format version, another byte order, reserved bytes that are not zero.
    refused_then_reset(edited_image([](wd::image_header& h) { h.version = 2; }),
                       wd::errc::incompatible_document);
    refused_then_reset(edited_image([](wd::image_header& h) { h.byte_order = 0x04030201; }),
                       wd::errc::incompatible_document);
    refused_then_reset(edited_image([](wd::image_header& h) { h.reserved_4 = 1; }),
                       wd::errc::incompatible_document);

    // Garbage, a size below the header's, and a size no block holds.
    refused_then_reset(std::vector<std::byte>(200, std::byte{'x'}), wd::errc::not_a_document);
    refused_then_reset(edited_image([](wd::image_header& h) { h.size = 32; }),
                       wd::errc::not_a_document);
    refused_then_reset(edited_image([](wd::image_header& h) { h.size = std::uint64_t{1} << 40U; }),
                       wd::errc::capacity);

    // A sound header over a heap that is not: the whole image is read, then refused.
    refused_then_reset(edited_image([](wd::image_header& h) { h.container += 16; }),
                       wd::errc::not_a_document);
}

void a_size_larger_than_a_block() {
    const std::vector<wd::document> sources = make_sources();
    wd::image_pool pool = make_pool(256, 2);
    wd::image_reader reader(pool);
    // Refused at its header, before a byte more is taken.
    const auto written = reader.write(sources[0].image());
    BOOST_TEST(written.error() == wd::errc::capacity);
    BOOST_TEST_EQ(reader.offset(), 64U);
    BOOST_TEST_EQ(pool.available(), 2U);
}

void a_stream_cut_short() {
    const std::vector<wd::document> sources = make_sources();
    const std::vector<std::byte> stream = concatenate(sources);
    wd::image_pool pool = make_pool(largest_image(sources), sources.size());

    // Cut inside the last image's header, and inside its heap.
    const wd::document& final_source = sources.back();
    const std::size_t last = stream.size() - final_source.image().size();
    for (const std::size_t cut : {last + 10, stream.size() - 1}) {
        wd::image_reader reader(pool);
        const auto written = reader.write(std::span(stream).first(cut));
        BOOST_TEST(written.has_value() && *written == cut);
        BOOST_TEST(reader.finish().error() == wd::errc::not_a_document);
        // The block of the image cut short is given back; the complete ones stay delivered.
        BOOST_TEST_EQ(pool.available(), 1U);
        std::size_t complete = 0;
        for (auto next = reader.next(); next.has_value(); next = reader.next()) {
            const std::optional<wd::leased_document<>>& document = *next;
            if (!document.has_value() || complete == sources.size()) {
                BOOST_ERROR("a document missing or one too many");
                break;
            }
            BOOST_TEST(same(*document, sources[complete]));
            ++complete;
        }
        BOOST_TEST_EQ(complete, sources.size() - 1);
        BOOST_TEST(reader.next().error() == wd::errc::not_a_document);
        BOOST_TEST_EQ(pool.available(), sources.size());
    }

    // A stream that ends between images is whole, and an empty one too.
    wd::image_reader reader(pool);
    BOOST_TEST(reader.finish().has_value());
    BOOST_TEST(reader.write(std::span(stream).first(last)).has_value());
    BOOST_TEST(reader.finish().has_value());
}

void leases_in_any_order() {
    const std::vector<wd::document> sources = make_sources();
    const std::vector<std::byte> stream = concatenate(sources);
    const std::size_t blocks = sources.size();
    wd::image_pool pool = make_pool(largest_image(sources), blocks);

    for (std::size_t round = 0; round < 3; ++round) {
        wd::image_reader reader(pool);
        BOOST_TEST(reader.write(stream).has_value());
        std::vector<wd::leased_document<>> held = take_all(reader);
        if (!BOOST_TEST_EQ(held.size(), blocks)) {
            return;
        }
        // Moved: the moved-from lease holds nothing, and the block stays leased.
        const wd::leased_document<> moved = std::move(held[1]);
        BOOST_TEST(held[1].image().empty());
        BOOST_TEST(same(moved, sources[1]));
        BOOST_TEST_EQ(pool.available(), 0U);

        // Move-assigned onto a lease: the lease's own block goes back.
        held[3] = std::move(held[0]);
        BOOST_TEST_EQ(pool.available(), 1U);
        BOOST_TEST(same(held[3], sources[0]));

        // A lease written in place, which its image shows.
        BOOST_TEST(held[4]["added"].set(true).has_value());
        BOOST_TEST(held[4]["added"].get<bool>().value());

        // Destroyed in an order that depends on the round.
        std::ranges::rotate(held, held.begin() + static_cast<std::ptrdiff_t>(round));
        held.clear();
        BOOST_TEST_EQ(pool.available(), blocks - 1);
        BOOST_TEST(same(moved, sources[1]));
    }
    BOOST_TEST_EQ(pool.available(), blocks);
}

void a_pool_and_a_reader_move() {
    const std::vector<wd::document> sources = make_sources();
    const std::vector<std::byte> stream = concatenate(sources);
    wd::image_pool first = make_pool(largest_image(sources), sources.size());
    wd::image_reader reader(first);

    // Half a stream, then the pool moved, then the reader moved: the leases and the bytes stay.
    const std::size_t half = stream.size() / 2;
    BOOST_TEST(reader.write(std::span(stream).first(half)).has_value());
    const wd::image_pool pool = std::move(first);
    wd::image_reader moved = std::move(reader);
    BOOST_TEST(moved.write(std::span(stream).subspan(half)).has_value());
    const std::vector<wd::leased_document<>> taken = take_all(moved);
    if (!BOOST_TEST_EQ(taken.size(), sources.size())) {
        return;
    }
    for (std::size_t i = 0; i < sources.size(); ++i) {
        BOOST_TEST(same(taken[i], sources[i]));
    }
    BOOST_TEST_EQ(pool.available(), 0U);
    // NOLINTBEGIN(bugprone-use-after-move): the state a move leaves
    BOOST_TEST_EQ(first.blocks(), 0U);
    BOOST_TEST(first.acquire().error() == wd::errc::capacity);

    // The moved-from reader is a new reader on the same pool.
    BOOST_TEST_EQ(reader.offset(), 0U);
    BOOST_TEST(reader.prepare().error() == wd::errc::capacity);
    // NOLINTEND(bugprone-use-after-move)
}

// A stream read in every form, its documents taken and given back, allocates nothing: the pool
// is allocated once, before, and each document lives in its block.
void reading_allocates_nothing() {
    const std::vector<wd::document> sources = make_sources();
    const std::vector<std::byte> stream = concatenate(sources);
    const std::span<const std::byte> all(stream);
    wd::image_pool pool = make_pool(largest_image(sources), 2);
    wd::image_reader reader(pool);

    const std::size_t before = global_allocations;
    std::size_t bytes = 0;
    std::size_t delivered = 0;
    for (std::size_t round = 0; round < 3; ++round) {
        // The push form, in pieces of 100 bytes.
        for (std::size_t at = 0; at < all.size(); at += 100) {
            const auto written =
                reader.write(all.subspan(at, std::min<std::size_t>(100, all.size() - at)),
                             [&](wd::leased_document<>&& lease) {
                                 bytes += lease.image().size();
                                 ++delivered;
                             });
            webcpp::test::require(BOOST_TEST(written.has_value()));
        }
        // The pull form, one image at a time.
        for (const wd::document& source : sources) {
            const auto written = reader.write(source.image());
            webcpp::test::require(BOOST_TEST(written.has_value()));
            auto next = reader.next();
            webcpp::test::require(BOOST_TEST(next.has_value()));
            const std::optional<wd::leased_document<>>& document = *next;
            if (document.has_value()) {
                bytes += document->image().size();
                ++delivered;
            }
        }
    }
    BOOST_TEST(reader.finish().has_value());
    BOOST_TEST_EQ(global_allocations - before, 0U);
    BOOST_TEST_EQ(delivered, 6 * sources.size());
    BOOST_TEST_EQ(bytes, 6 * stream.size());
}

void the_pool() {
    BOOST_TEST(wd::image_pool::create(100, 2).error() == wd::errc::range);
    BOOST_TEST(wd::image_pool::create(48, 2).error() == wd::errc::range);
    BOOST_TEST(wd::image_pool::create(4096, 0).error() == wd::errc::range);
    BOOST_TEST(
        wd::image_pool::create(std::numeric_limits<std::size_t>::max() / 32 * 16, 4).error() ==
        wd::errc::capacity);

    wd::image_pool pool = make_pool(4096, 2);
    BOOST_TEST_EQ(pool.block_bytes(), 4096U);
    BOOST_TEST_EQ(pool.blocks(), 2U);
    BOOST_TEST_EQ(pool.available(), 2U);

    std::vector<std::byte> image;
    {
        auto a = pool.acquire();
        auto b = pool.acquire();
        if (!BOOST_TEST(a.has_value() && b.has_value())) {
            return;
        }
        BOOST_TEST_EQ(a->bytes().size(), 4096U);
        BOOST_TEST_EQ(reinterpret_cast<std::uintptr_t>(a->bytes().data()) % 16, 0U);
        BOOST_TEST_EQ(reinterpret_cast<std::uintptr_t>(b->bytes().data()) % 16, 0U);
        BOOST_TEST(a->bytes().data() != b->bytes().data());
        BOOST_TEST(pool.acquire().error() == wd::errc::capacity);

        // A document built in a block, its image read by a reader into the other block.
        auto built = wd::leased_document<>::format(std::move(*a));
        if (!BOOST_TEST(built.has_value())) {
            return;
        }
        BOOST_TEST(a->bytes().empty());
        BOOST_TEST(built->root().set(wd::object{{"in", "a block"}}).has_value());
        *b = wd::pool_block();
        BOOST_TEST_EQ(pool.available(), 1U);
        wd::image_reader reader(pool);
        const auto written = reader.write(built->image());
        BOOST_TEST(written.has_value() && *written == built->image().size());
        const wd::leased_document<> read = take_one(reader);
        BOOST_TEST_EQ(wd::stringify(read.get()), R"({"in":"a block"})");
        image.assign(read.image().begin(), read.image().end());
    }
    BOOST_TEST_EQ(pool.available(), 2U);

    // An image copied into a block by hand and opened there.
    auto block = pool.acquire();
    if (!BOOST_TEST(block.has_value())) {
        return;
    }
    std::ranges::copy(image, block->bytes().begin());
    auto opened = wd::leased_document<>::open(std::move(*block));
    if (!BOOST_TEST(opened.has_value())) {
        return;
    }
    BOOST_TEST_EQ(wd::stringify(opened->get()), R"({"in":"a block"})");
    BOOST_TEST_EQ((*opened)["in"].get<std::string_view>().value(), "a block");

    // A block that holds no image is refused, and given back.
    auto empty = pool.acquire();
    if (!BOOST_TEST(empty.has_value())) {
        return;
    }
    BOOST_TEST(wd::leased_document<>::open(std::move(*empty)).error() == wd::errc::not_a_document);
    BOOST_TEST_EQ(pool.available(), 1U);
}

// The empty lease: a default leased document holds no block and no document, as a moved-from one
// does, and assigning a lease to it gives back the block it held.
void an_empty_lease() {
    wd::leased_document<> empty;
    BOOST_TEST(empty.empty());
    BOOST_TEST(empty.image().empty());
    BOOST_TEST(!empty.sealed());

    wd::image_pool pool = make_pool(4096, 1);
    auto acquired = pool.acquire();
    webcpp::test::require(BOOST_TEST(acquired.has_value()));
    auto formatted = wd::leased_document<>::format(std::move(*acquired));
    webcpp::test::require(BOOST_TEST(formatted.has_value()));
    wd::leased_document<> held = std::move(*formatted);
    BOOST_TEST(!held.empty());
    BOOST_TEST_EQ(pool.available(), 0U);

    empty = std::move(held);
    BOOST_TEST(!empty.empty());
    // NOLINTNEXTLINE(bugprone-use-after-move): the state a move leaves
    BOOST_TEST(held.empty());
    BOOST_TEST_EQ(pool.available(), 0U);

    empty = wd::leased_document<>();
    BOOST_TEST(empty.empty());
    BOOST_TEST_EQ(pool.available(), 1U);
}

}  // namespace

int main() {
    several_images_in_one_write();
    every_split();
    one_byte_at_a_time();
    prepare_and_commit();
    backpressure();
    a_reused_block_holds_nothing_of_the_last();
    refused_streams();
    a_size_larger_than_a_block();
    a_stream_cut_short();
    leases_in_any_order();
    a_pool_and_a_reader_move();
    reading_allocates_nothing();
    the_pool();
    an_empty_lease();
    return boost::report_errors();
}
