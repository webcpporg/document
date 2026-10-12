// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Shared documents: a pool formatted over memory the program gives and attached again; the count
// of references as copies are made, moved and dropped; the last holder zeroing its block and
// giving it back; make_mutable with one holder and with several; a reference released and adopted;
// the seal; a full pool; no allocation by the pool or its documents. Natively, threads that copy
// and drop references, lease and give back blocks and copy on write, all at once.
//
// test/Jamfile builds this file twice: as the build asks (shared), and natively with exceptions,
// whatever the build asks, where WEBCPP_TEST_DOCUMENT_INTERPROCESS also shares a pool between two
// processes through Boost.Interprocess's shared memory, which needs exceptions
// (shared_interprocess): this program runs itself again as the second process, as
// Boost.Interprocess's own tests do.

#include <webcpp/document/shared.hpp>

#include <webcpp/document/document.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/stringify.hpp>

#include <boost/config.hpp>
#include <boost/core/lightweight_test.hpp>

#include "require.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef WEBCPP_TARGET_NATIVE
#include <thread>
#endif

#ifdef WEBCPP_TEST_DOCUMENT_INTERPROCESS
#include <boost/interprocess/detail/os_thread_functions.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#endif

#ifdef _MSC_VER
#include <malloc.h>
#endif

namespace wd = webcpp::document;

namespace {

// The calls of the global operator new this program has made, from any thread. It is mutable
// global state, which no library holds: it is this test program's own instrument, the one place
// where the replaced operator new below can record what it sees.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<std::size_t> global_allocations{0};

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
    global_allocations.fetch_add(1, std::memory_order_relaxed);
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
    global_allocations.fetch_add(1, std::memory_order_relaxed);
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

constexpr std::size_t block_bytes = 4096;

// The program's memory for a pool: 16-aligned, here the process heap.
class region {
public:
    explicit region(std::size_t size)
        : bytes_(static_cast<std::byte*>(::operator new(size, std::align_val_t{16})), size) {
        // Garbage, which formatting the pool clears.
        std::ranges::fill(bytes_, std::byte{0xA5});
    }

    region(const region&) = delete;
    region& operator=(const region&) = delete;
    region(region&&) = delete;
    region& operator=(region&&) = delete;

    ~region() { ::operator delete(bytes_.data(), std::align_val_t{16}); }

    [[nodiscard]] std::span<std::byte> bytes() const { return bytes_; }

private:
    std::span<std::byte> bytes_;
};

std::size_t region_bytes(std::size_t blocks) {
    const auto bytes = wd::shared_pool::region_bytes(block_bytes, blocks);
    webcpp::test::require(BOOST_TEST(bytes.has_value()));
    return *bytes;
}

wd::shared_pool format_pool(const region& r) {
    auto pool = wd::shared_pool::format(r.bytes(), block_bytes);
    webcpp::test::require(BOOST_TEST(pool.has_value()));
    return *pool;
}

wd::shared_document<> make_document(wd::shared_pool pool, std::string_view name) {
    auto made = wd::shared_document<>::create(pool);
    webcpp::test::require(BOOST_TEST(made.has_value()));
    webcpp::test::require(BOOST_TEST(
        made->root().set(wd::object{{"name", name}, {"list", wd::array{1, 2, 3}}}).has_value()));
    return std::move(*made);
}

// Whether every byte of the bytes is zero.
bool zero(std::span<const std::byte> bytes) {
    return std::ranges::all_of(bytes, [](std::byte b) { return b == std::byte{0}; });
}

// The image of block `index` of a pool at the start of `bytes`, found by offsets alone.
std::span<const std::byte> image_of(std::span<const std::byte> bytes, std::size_t index) {
    const std::size_t stride = wd::shared_pool::block_header_bytes + block_bytes;
    return bytes.subspan(
        wd::shared_pool::header_bytes + (index * stride) + wd::shared_pool::block_header_bytes,
        block_bytes);
}

// Whether every block's image in the pool at the start of `bytes` is zero.
bool images_zero(std::span<const std::byte> bytes, std::size_t blocks) {
    for (std::size_t i = 0; i < blocks; ++i) {
        if (!zero(image_of(bytes, i))) {
            return false;
        }
    }
    return true;
}

void the_pool() {
    BOOST_TEST(wd::shared_pool::region_bytes(block_bytes, 0).error() == wd::errc::range);
    BOOST_TEST(wd::shared_pool::region_bytes(100, 1).error() == wd::errc::range);
    BOOST_TEST(wd::shared_pool::region_bytes(32, 1).error() == wd::errc::range);
    BOOST_TEST_EQ(region_bytes(3), wd::shared_pool::header_bytes +
                                       (3 * (wd::shared_pool::block_header_bytes + block_bytes)));

    const region r(region_bytes(3) + 100);
    // Not 16-aligned, too small, or blocks of a size the pool refuses.
    BOOST_TEST(wd::shared_pool::format(r.bytes().subspan(8), block_bytes).error() ==
               wd::errc::range);
    BOOST_TEST(
        wd::shared_pool::format(r.bytes().first(region_bytes(1) - 16), block_bytes).error() ==
        wd::errc::capacity);
    BOOST_TEST(wd::shared_pool::format(r.bytes(), 100).error() == wd::errc::range);
    // Memory that holds no pool is refused.
    BOOST_TEST(wd::shared_pool::attach(r.bytes()).error() == wd::errc::not_a_document);

    const wd::shared_pool pool = format_pool(r);
    BOOST_TEST_EQ(pool.blocks(), 3U);
    BOOST_TEST_EQ(pool.block_bytes(), block_bytes);
    BOOST_TEST_EQ(pool.available(), 3U);
    BOOST_TEST(images_zero(r.bytes(), 3));

    // Attached again over the same memory, it is the same pool.
    auto attached = wd::shared_pool::attach(r.bytes());
    if (BOOST_TEST(attached.has_value())) {
        BOOST_TEST_EQ(attached->blocks(), 3U);
        BOOST_TEST_EQ(attached->block_bytes(), block_bytes);
        BOOST_TEST(attached->region().data() == r.bytes().data());
    }
    // A region shorter than the pool it holds is refused.
    BOOST_TEST(wd::shared_pool::attach(r.bytes().first(region_bytes(3) - 16)).error() ==
               wd::errc::not_a_document);
}

void counts() {
    const region r(region_bytes(4));
    const wd::shared_pool pool = format_pool(r);

    const wd::shared_document<> empty;
    BOOST_TEST_EQ(empty.use_count(), 0U);
    BOOST_TEST(empty.image().empty());

    wd::shared_document<> a = make_document(pool, "a");
    BOOST_TEST_EQ(a.use_count(), 1U);
    BOOST_TEST_EQ(pool.available(), 3U);
    {
        const wd::shared_document<> b = a;  // NOLINT(performance-unnecessary-copy-initialization)
        BOOST_TEST_EQ(a.use_count(), 2U);
        BOOST_TEST_EQ(b.use_count(), 2U);
        BOOST_TEST_EQ(b.offset(), a.offset());
        BOOST_TEST(b.image().data() == a.image().data());
        BOOST_TEST_EQ(b["name"].get<std::string_view>().value(), "a");

        wd::shared_document<> c;
        c = b;
        BOOST_TEST_EQ(a.use_count(), 3U);
        wd::shared_document<> d = std::move(c);
        BOOST_TEST_EQ(a.use_count(), 3U);
        BOOST_TEST_EQ(c.use_count(), 0U);  // NOLINT(bugprone-use-after-move)
        BOOST_TEST(c.image().empty());

        // A write through one holder is every holder's: they share the image.
        BOOST_TEST(d["name"].set("written").has_value());
        BOOST_TEST_EQ(a["name"].get<std::string_view>().value(), "written");

        // Assigning another document drops this one's reference.
        const wd::shared_document<> other = make_document(pool, "other");
        d = other;
        BOOST_TEST_EQ(a.use_count(), 2U);
        BOOST_TEST_EQ(other.use_count(), 2U);
        // Assigning a document to itself changes nothing.
        d = *&d;
        BOOST_TEST_EQ(other.use_count(), 2U);
    }
    BOOST_TEST_EQ(a.use_count(), 1U);
    BOOST_TEST_EQ(pool.available(), 3U);
}

void the_last_holder_returns_a_zeroed_block() {
    const region r(region_bytes(2));
    const wd::shared_pool pool = format_pool(r);
    {
        wd::shared_document<> a = make_document(pool, "secret value");
        BOOST_TEST(a["text"].set(std::string(1000, 's')).has_value());
        const wd::shared_document<> b = a;
        BOOST_TEST_EQ(pool.available(), 1U);
        BOOST_TEST(!zero(image_of(r.bytes(), 0)));
        a = wd::shared_document<>();
        // b still holds it.
        BOOST_TEST_EQ(pool.available(), 1U);
        BOOST_TEST_EQ(b["name"].get<std::string_view>().value(), "secret value");
    }
    BOOST_TEST_EQ(pool.available(), 2U);
    BOOST_TEST(zero(image_of(r.bytes(), 0)));
    BOOST_TEST(zero(image_of(r.bytes(), 1)));

    // The block given back is the next one leased, and holds nothing of the last document.
    const wd::shared_document<> next = make_document(pool, "next");
    BOOST_TEST_EQ(next.offset(),
                  wd::shared_pool::header_bytes + wd::shared_pool::block_header_bytes);
    const auto image = next.image();
    BOOST_TEST(
        std::ranges::search(image, std::as_bytes(std::span(std::string_view("secret")))).empty());
}

void make_mutable() {
    const region r(region_bytes(3));
    const wd::shared_pool pool = format_pool(r);

    // One holder: the same block, written in place.
    wd::shared_document<> alone = make_document(pool, "alone");
    const std::uint64_t offset = alone.offset();
    BOOST_TEST(alone.make_mutable().has_value());
    BOOST_TEST_EQ(alone.offset(), offset);
    BOOST_TEST_EQ(alone.use_count(), 1U);
    BOOST_TEST_EQ(pool.available(), 2U);

    // Several holders: the writer gets a copy in a new block; the others keep the document as it
    // was.
    const wd::shared_document<> reader = alone;
    wd::shared_document<> writer = alone;
    BOOST_TEST_EQ(alone.use_count(), 3U);
    BOOST_TEST(writer.make_mutable().has_value());
    BOOST_TEST(writer.offset() != offset);
    BOOST_TEST_EQ(writer.use_count(), 1U);
    BOOST_TEST_EQ(reader.use_count(), 2U);
    BOOST_TEST_EQ(pool.available(), 1U);
    BOOST_TEST(writer["name"].set("changed").has_value());
    BOOST_TEST(writer["list"].push(4).has_value());
    BOOST_TEST_EQ(wd::stringify(writer.get()), R"({"name":"changed","list":[1,2,3,4]})");
    BOOST_TEST_EQ(wd::stringify(reader.get()), R"({"name":"alone","list":[1,2,3]})");

    // No block left for another copy: the holder keeps its document, unchanged.
    wd::shared_document<> third = alone;
    const wd::shared_document<> full = make_document(pool, "full");
    BOOST_TEST(third.make_mutable().error() == wd::errc::capacity);
    BOOST_TEST_EQ(third.offset(), offset);
    BOOST_TEST_EQ(alone.use_count(), 3U);

    // Nothing to copy.
    wd::shared_document<> empty;
    BOOST_TEST(empty.make_mutable().error() == wd::errc::type);
}

void release_and_adopt() {
    const region r(region_bytes(2));
    const wd::shared_pool pool = format_pool(r);
    wd::shared_document<> a = make_document(pool, "handed over");
    wd::shared_document<> copy = a;
    const std::uint64_t offset = copy.release();
    BOOST_TEST_EQ(copy.use_count(), 0U);
    BOOST_TEST_EQ(offset, a.offset());
    // The released reference still counts.
    BOOST_TEST_EQ(a.use_count(), 2U);
    a = wd::shared_document<>();
    BOOST_TEST_EQ(pool.available(), 1U);

    // Adopted through a pool attached again, as another process would.
    auto attached = wd::shared_pool::attach(r.bytes());
    webcpp::test::require(BOOST_TEST(attached.has_value()));
    auto adopted = wd::shared_document<>::adopt(*attached, offset);
    if (BOOST_TEST(adopted.has_value())) {
        BOOST_TEST_EQ(adopted->use_count(), 1U);
        BOOST_TEST_EQ((*adopted)["name"].get<std::string_view>().value(), "handed over");
    }
    *adopted = wd::shared_document<>();
    BOOST_TEST_EQ(pool.available(), 2U);

    // Offsets that are no block's image, and a block nobody holds, are refused.
    BOOST_TEST(wd::shared_document<>::adopt(pool, offset + 16).error() == wd::errc::range);
    BOOST_TEST(wd::shared_document<>::adopt(pool, 0).error() == wd::errc::range);
    BOOST_TEST(wd::shared_document<>::adopt(pool, r.bytes().size() + 4096).error() ==
               wd::errc::range);
    BOOST_TEST(wd::shared_document<>::adopt(pool, offset).error() == wd::errc::range);
    BOOST_TEST(wd::shared_document<>().release() == 0U);
}

void from_an_image_and_sealed() {
    const region r(region_bytes(2));
    const wd::shared_pool pool = format_pool(r);
    auto parsed = wd::parse(R"({"from":"an image","n":[1,2]})");
    webcpp::test::require(BOOST_TEST(parsed.has_value()));
    webcpp::test::require(BOOST_TEST(parsed->shrink_to_fit().has_value()));
    webcpp::test::require(BOOST_TEST(parsed->seal().has_value()));

    auto shared = wd::shared_document<>::from_image(pool, parsed->image());
    if (!BOOST_TEST(shared.has_value())) {
        return;
    }
    BOOST_TEST_EQ(wd::stringify(shared->get()), wd::stringify(*parsed));
    // The image now fills its block, so it holds more than its source did.
    BOOST_TEST_EQ(shared->image().size(), block_bytes);
    BOOST_TEST(!shared->sealed());
    BOOST_TEST((*shared)["text"].set(std::string(2000, 't')).has_value());
    BOOST_TEST(shared->seal().has_value());
    BOOST_TEST(shared->sealed());
    // Adopted, a sealed image is verified; a copy on write keeps the seal, its bytes the same.
    wd::shared_document<> copy = *shared;
    const std::uint64_t offset = copy.release();
    auto adopted = wd::shared_document<>::adopt(pool, offset);
    if (!BOOST_TEST(adopted.has_value())) {
        return;
    }
    BOOST_TEST(adopted->sealed());
    BOOST_TEST(shared->make_mutable().has_value());
    BOOST_TEST(shared->offset() != adopted->offset());
    BOOST_TEST(shared->sealed());
    BOOST_TEST_EQ(pool.available(), 0U);
    *adopted = wd::shared_document<>();
    BOOST_TEST_EQ(pool.available(), 1U);

    // A damaged image is refused before a block is leased.
    std::vector<std::byte> damaged(parsed->image().begin(), parsed->image().end());
    damaged[100] ^= std::byte{1};
    BOOST_TEST(wd::shared_document<>::from_image(pool, damaged).error() ==
               wd::errc::damaged_document);
    BOOST_TEST_EQ(pool.available(), 1U);

    // An image larger than a block is refused.
    std::vector<std::byte> large(block_bytes + 16);
    std::ranges::copy(parsed->image(), large.begin());
    const std::uint64_t size = large.size();
    std::memcpy(large.data() + 24, &size, sizeof size);
    BOOST_TEST(wd::shared_document<>::from_image(pool, large, wd::verify::none).error() ==
               wd::errc::capacity);
    BOOST_TEST_EQ(pool.available(), 1U);
}

// A receiver refuses an offset whose image is damaged, then gives its reference back by the offset
// alone, without opening the image: the block returns zeroed, and bad offsets are refused.
void a_refused_reference_is_dropped() {
    const region r(region_bytes(2));
    const wd::shared_pool pool = format_pool(r);
    std::uint64_t offset = 0;
    {
        wd::shared_document<> doc = make_document(pool, "damaged later");
        webcpp::test::require(BOOST_TEST(doc.seal().has_value()));
        wd::shared_document<> handed = doc;
        offset = handed.release();
    }
    BOOST_TEST_EQ(pool.available(), 1U);
    // A byte of the image changed, its header's magic included: the image cannot be opened.
    r.bytes()[static_cast<std::size_t>(offset)] ^= std::byte{0xFF};
    BOOST_TEST(wd::shared_document<>::adopt(pool, offset).error() == wd::errc::damaged_document);
    BOOST_TEST_EQ(pool.available(), 1U);

    BOOST_TEST(pool.drop(offset).has_value());
    BOOST_TEST_EQ(pool.available(), 2U);
    BOOST_TEST(images_zero(r.bytes(), 2));
    // Its reference is gone: a second drop is refused, and so are offsets that name no block.
    BOOST_TEST(pool.drop(offset).error() == wd::errc::range);
    BOOST_TEST(pool.drop(0).error() == wd::errc::range);
    BOOST_TEST(pool.drop(offset + 16).error() == wd::errc::range);
    BOOST_TEST(pool.drop(r.bytes().size() + 4096).error() == wd::errc::range);
    BOOST_TEST(pool.drop(std::numeric_limits<std::uint64_t>::max()).error() == wd::errc::range);
    BOOST_TEST(wd::shared_pool().drop(offset).error() == wd::errc::range);

    // Dropping one of several references leaves the others' document as it was.
    const wd::shared_document<> kept = make_document(pool, "kept");
    wd::shared_document<> extra = kept;
    const std::uint64_t extra_offset = extra.release();
    BOOST_TEST_EQ(kept.use_count(), 2U);
    BOOST_TEST(pool.drop(extra_offset).has_value());
    BOOST_TEST_EQ(kept.use_count(), 1U);
    BOOST_TEST_EQ(kept["name"].get<std::string_view>().value(), "kept");
}

void a_full_pool() {
    const region r(region_bytes(1));
    const wd::shared_pool pool = format_pool(r);
    const wd::shared_document<> only = make_document(pool, "only");
    BOOST_TEST(wd::shared_document<>::create(pool).error() == wd::errc::capacity);
    BOOST_TEST_EQ(pool.available(), 0U);
}

// The pool and its documents allocate nothing: lease, copy, move, release, adopt, make_mutable,
// seal and drop.
void sharing_allocates_nothing() {
    const region r(region_bytes(3));
    const wd::shared_pool pool = format_pool(r);
    const std::size_t before = global_allocations.load();
    {
        auto made = wd::shared_document<>::create(pool);
        webcpp::test::require(BOOST_TEST(made.has_value()));
        const wd::shared_document<> a = std::move(*made);
        wd::shared_document<> b = a;
        wd::shared_document<> c = std::move(b);
        const std::uint64_t offset = c.release();
        auto adopted = wd::shared_document<>::adopt(pool, offset);
        BOOST_TEST(adopted.has_value());
        BOOST_TEST(adopted->make_mutable().has_value());
        BOOST_TEST(adopted->seal().has_value());
        BOOST_TEST_EQ(a.use_count(), 1U);
    }
    BOOST_TEST_EQ(global_allocations.load(), before);
    BOOST_TEST_EQ(pool.available(), 3U);
}

#ifdef WEBCPP_TARGET_NATIVE

constexpr std::size_t threads = 8;

// Runs `work(t)` on `threads` threads at once, then joins them.
template <class Work>
void on_threads(Work work) {
    std::vector<std::thread> running;
    running.reserve(threads);
    for (std::size_t t = 0; t < threads; ++t) {
        running.emplace_back(work, t);
    }
    for (std::thread& thread : running) {
        thread.join();
    }
}

// Threads copy and drop references to one document, all at once.
void threads_copy_and_drop() {
    const region r(region_bytes(2));
    const wd::shared_pool pool = format_pool(r);
    wd::shared_document<> shared = make_document(pool, "shared");
    std::atomic<std::size_t> wrong{0};
    on_threads([&](std::size_t) {
        for (std::size_t i = 0; i < 20000; ++i) {
            // The copies are what the test makes, each a reference added and then dropped.
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            const wd::shared_document<> copy = shared;
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            const wd::shared_document<> other = copy;
            if (copy.use_count() < 3 || other.offset() != shared.offset()) {
                wrong.fetch_add(1);
            }
        }
    });
    BOOST_TEST_EQ(wrong.load(), 0U);
    BOOST_TEST_EQ(shared.use_count(), 1U);
    BOOST_TEST_EQ(pool.available(), 1U);
    shared = wd::shared_document<>();
    BOOST_TEST_EQ(pool.available(), 2U);
    BOOST_TEST(zero(image_of(r.bytes(), 0)));
}

// Threads lease blocks, write their own value, read it back and give the blocks back, more
// threads than blocks: no block is ever held twice, and every block comes back zeroed.
void threads_lease_and_give_back() {
    constexpr std::size_t blocks = 4;
    const region r(region_bytes(blocks));
    const wd::shared_pool pool = format_pool(r);
    std::atomic<std::size_t> wrong{0};
    std::atomic<std::size_t> leased{0};
    on_threads([&](std::size_t t) {
        for (std::size_t i = 0; i < 5000; ++i) {
            auto made = wd::shared_document<>::create(pool);
            if (!made.has_value()) {
                // Every block is leased by the other threads.
                continue;
            }
            leased.fetch_add(1);
            const std::int64_t mine =
                (static_cast<std::int64_t>(t) * 100000) + static_cast<std::int64_t>(i);
            if (!made->root().set(mine).has_value() || made->use_count() != 1 ||
                made->root().get<std::int64_t>().value() != mine) {
                wrong.fetch_add(1);
            }
            const wd::shared_document<> copy = *made;
            if (copy.root().get<std::int64_t>().value() != mine) {
                wrong.fetch_add(1);
            }
        }
    });
    BOOST_TEST_EQ(wrong.load(), 0U);
    BOOST_TEST_GT(leased.load(), 0U);
    BOOST_TEST_EQ(pool.available(), blocks);
    for (std::size_t i = 0; i < blocks; ++i) {
        BOOST_TEST(zero(image_of(r.bytes(), i)));
    }
}

// Threads copy one document on write, all at once: each gets its own block, and the document
// they copied stays as it was.
void threads_copy_on_write() {
    constexpr std::size_t blocks = threads + 1;
    const region r(region_bytes(blocks));
    const wd::shared_pool pool = format_pool(r);
    const wd::shared_document<> original = make_document(pool, "original");
    std::atomic<std::size_t> wrong{0};
    for (std::size_t round = 0; round < 50; ++round) {
        on_threads([&](std::size_t t) {
            wd::shared_document<> mine = original;
            if (!mine.make_mutable().has_value() || mine.offset() == original.offset() ||
                !mine["name"].set("thread " + std::to_string(t)).has_value() ||
                mine["name"].get<std::string_view>().value() != "thread " + std::to_string(t)) {
                wrong.fetch_add(1);
            }
        });
    }
    BOOST_TEST_EQ(wrong.load(), 0U);
    BOOST_TEST_EQ(original.use_count(), 1U);
    BOOST_TEST_EQ(original["name"].get<std::string_view>().value(), "original");
    BOOST_TEST_EQ(pool.available(), blocks - 1);
}

#endif  // WEBCPP_TARGET_NATIVE

#ifdef WEBCPP_TEST_DOCUMENT_INTERPROCESS

namespace bip = boost::interprocess;

// The bytes before the pool in the shared memory: where one process leaves the offset of a
// reference it released for the other.
constexpr std::size_t mailbox_bytes = 64;
constexpr std::size_t blocks = 4;

std::size_t segment_bytes() {
    return mailbox_bytes + region_bytes(blocks);
}

// The second process: it maps the shared memory at an address of its own, adopts the reference
// the first released, which it then holds alone, reads it, copies it on write into a block of its
// own with its answer, releases that one for the first, and drops the first's document, whose
// block it zeroes and gives back.
int consumer(const char* name) {
    const bip::shared_memory_object memory(bip::open_only, name, bip::read_write);
    const bip::mapped_region mapped(memory, bip::read_write);
    const std::span<std::byte> bytes(static_cast<std::byte*>(mapped.get_address()),
                                     mapped.get_size());
    auto pool = wd::shared_pool::attach(bytes.subspan(mailbox_bytes, region_bytes(blocks)));
    webcpp::test::require(BOOST_TEST(pool.has_value()));
    std::uint64_t offset = 0;
    std::memcpy(&offset, bytes.data(), sizeof offset);

    auto adopted = wd::shared_document<>::adopt(*pool, offset);
    webcpp::test::require(BOOST_TEST(adopted.has_value()));
    BOOST_TEST_EQ(adopted->use_count(), 1U);
    BOOST_TEST(adopted->sealed());
    BOOST_TEST_EQ((*adopted)["name"].get<std::string_view>().value(), "from the producer");

    wd::shared_document<> answer = *adopted;
    BOOST_TEST(answer.make_mutable().has_value());
    BOOST_TEST(answer["name"].set("from the consumer").has_value());
    const std::uint64_t answered = answer.release();
    std::memcpy(bytes.data() + sizeof offset, &answered, sizeof answered);
    *adopted = wd::shared_document<>();
    BOOST_TEST_EQ(pool->available(), blocks - 1);
    return boost::report_errors();
}

// The first process makes a document in a pool in shared memory, seals it, releases a copy's
// reference for the second process and drops its own, runs the second process, then adopts the
// answer it left.
void two_processes(const char* program) {
    const std::string name = "wcpp_doc_" + std::to_string(bip::ipcdetail::get_current_process_id());
    bip::shared_memory_object::remove(name.c_str());
    {
        bip::shared_memory_object memory(bip::create_only, name.c_str(), bip::read_write);
        memory.truncate(static_cast<bip::offset_t>(segment_bytes()));
        const bip::mapped_region mapped(memory, bip::read_write);
        const std::span<std::byte> bytes(static_cast<std::byte*>(mapped.get_address()),
                                         mapped.get_size());
        auto pool = wd::shared_pool::format(bytes.subspan(mailbox_bytes, region_bytes(blocks)),
                                            block_bytes);
        webcpp::test::require(BOOST_TEST(pool.has_value()));
        {
            wd::shared_document<> doc = make_document(*pool, "from the producer");
            BOOST_TEST(doc.seal().has_value());
            wd::shared_document<> handed = doc;
            const std::uint64_t offset = handed.release();
            std::memcpy(bytes.data(), &offset, sizeof offset);
        }
        // The producer holds nothing: the released reference keeps the document alive.
        BOOST_TEST_EQ(pool->available(), blocks - 1);

        // The second process is this program, run again by the shell, as Boost.Interprocess's own
        // tests run theirs.
        const std::string command = std::string("\"") + program + "\" consumer " + name;
        // NOLINTNEXTLINE(bugprone-command-processor,cert-env33-c)
        BOOST_TEST_EQ(std::system(command.c_str()), 0);

        std::uint64_t answered = 0;
        std::memcpy(&answered, bytes.data() + sizeof answered, sizeof answered);
        auto answer = wd::shared_document<>::adopt(*pool, answered);
        if (BOOST_TEST(answer.has_value())) {
            BOOST_TEST_EQ(answer->use_count(), 1U);
            BOOST_TEST_EQ((*answer)["name"].get<std::string_view>().value(), "from the consumer");
            *answer = wd::shared_document<>();
        }
        BOOST_TEST_EQ(pool->available(), blocks);
        BOOST_TEST(images_zero(bytes.subspan(mailbox_bytes), blocks));
    }
    bip::shared_memory_object::remove(name.c_str());
}

#endif  // WEBCPP_TEST_DOCUMENT_INTERPROCESS

}  // namespace

int main(int argc, char* argv[]) {
#ifdef WEBCPP_TEST_DOCUMENT_INTERPROCESS
    const std::span<char*> arguments(argv, static_cast<std::size_t>(argc));
    if (arguments.size() == 3 && std::string_view(arguments[1]) == "consumer") {
        return consumer(arguments[2]);
    }
    two_processes(arguments[0]);
#else
    static_cast<void>(argc);
    static_cast<void>(argv);
#endif
    the_pool();
    counts();
    the_last_holder_returns_a_zeroed_block();
    make_mutable();
    release_and_adopt();
    from_an_image_and_sealed();
    a_refused_reference_is_dropped();
    a_full_pool();
    sharing_allocates_nothing();
#ifdef WEBCPP_TARGET_NATIVE
    threads_copy_and_drop();
    threads_lease_and_give_back();
    threads_copy_on_write();
#endif
    return boost::report_errors();
}
