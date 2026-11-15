// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A document's allocator holds its image: created, grown several times, shrunk, copied into another
// resource, moved, and move-assigned across resources, a pmr::document makes no call of the global
// operator new, and gives back to its resource all it took; the image is the same bytes in any
// allocator; any allocator of std::byte works, a stateful one of the program's own among them; an
// external document carries its allocator for its working state; and, with exceptions, an
// allocator that has no room is errc::capacity with nothing changed.

#include <webcpp/document.hpp>

#include <boost/core/lightweight_test.hpp>

#include "counting.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace wd = webcpp::document;

namespace {

using webcpp::test::arena;
using webcpp::test::global_count;
using webcpp::test::require;

constexpr std::size_t mebibyte = std::size_t{1} << 20U;

/** What a stateful allocator of the test's own counts. */
struct allocator_counts {
    /** The allocations it made. */
    std::uint64_t allocations = 0;

    /** The deallocations it made. */
    std::uint64_t deallocations = 0;
};

/**
 An allocator of the program's own, stateful and neither standard nor polymorphic: it counts
 through a pointer and allocates with the aligned global operator new, so that any allocator is
 shown to give the image its 16-byte alignment through the rebind.
*/
template <class T>
class tallying_allocator {
public:
    /** The type allocated. */
    using value_type = T;

    /** An allocator that counts in `counts`, which outlives it. */
    explicit tallying_allocator(allocator_counts* counts) noexcept : counts_(counts) {}

    // The Allocator requirements convert an allocator to its rebound type implicitly.
    /** The same allocator for another type. */
    template <class U>
    // NOLINTNEXTLINE(misc-explicit-constructor)
    tallying_allocator(const tallying_allocator<U>& other) noexcept : counts_(other.counts()) {}

    /** Room for `n` objects of `T`, aligned for `T`. */
    T* allocate(std::size_t n) {
        ++counts_->allocations;
        return static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t{alignof(T)}));
    }

    /** Gives back what @ref allocate made. */
    void deallocate(T* p, std::size_t /*n*/) noexcept {
        ++counts_->deallocations;
        ::operator delete(p, std::align_val_t{alignof(T)});
    }

    /** The counts. */
    [[nodiscard]] allocator_counts* counts() const noexcept { return counts_; }

    /** Whether two allocators count in the same place. */
    template <class U>
    friend bool operator==(const tallying_allocator& a, const tallying_allocator<U>& b) noexcept {
        return a.counts_ == b.counts();
    }

private:
    allocator_counts* counts_;
};

/** The bytes of `d`'s image, copied. */
template <class Document>
std::vector<std::byte> bytes_of(const Document& d) {
    const std::span<const std::byte> image = d.image();
    return {image.begin(), image.end()};
}

/** Whether two documents' images are the same bytes; it allocates nothing. */
template <class A, class B>
bool same_image(const A& a, const B& b) noexcept {
    return std::ranges::equal(a.image(), b.image());
}

/** A document of the default storage holding an object, an array and strings, parsed. */
wd::document sample() {
    auto parsed = wd::parse(R"({"name":"document","list":[1,2.5,"three",[true,null]],"x":{}})");
    require(BOOST_TEST(parsed.has_value()));
    return std::move(*parsed);
}

// A pmr::document is created, grown three times and shrunk in its resource, with no call of the
// global operator new, and gives back all it took when destroyed; every block the resource gave it
// was asked with the alignment of 16.
void image_in_resource() {
    arena a(4 * mebibyte);
    const global_count global;
    {
        auto made = wd::pmr::document::create(a.allocator());
        require(BOOST_TEST(made.has_value()));
        wd::pmr::document d = std::move(*made);
        BOOST_TEST(d.get_allocator().resource() == &a.resource());
        BOOST_TEST_EQ(d.capacity(), 4096U);
        BOOST_TEST(d.reserve(16384).has_value());
        BOOST_TEST(d.reserve(65536).has_value());
        BOOST_TEST(d.reserve(262144).has_value());
        BOOST_TEST_EQ(d.capacity(), 262144U);
        BOOST_TEST(d.shrink_to_fit().has_value());
        BOOST_TEST_LT(d.capacity(), 4096U);
        // Created, grown three times and shrunk: five buffers, the last one held.
        BOOST_TEST_EQ(a.resource().allocations(), 5U);
        BOOST_TEST_EQ(a.resource().deallocations(), 4U);
    }
    BOOST_TEST_EQ(global.news(), 0U);
    BOOST_TEST_EQ(global.deletes(), 0U);
    BOOST_TEST_EQ(a.resource().bytes_in_use(), 0U);
    BOOST_TEST_EQ(a.resource().largest_alignment(), 16U);
}

// The default storage is unchanged: create calls the global operator new once, for its image, and
// an image is the same bytes whatever allocated it.
void default_storage_unchanged() {
    {
        const global_count global;
        const auto made = wd::document::create();
        require(BOOST_TEST(made.has_value()));
        BOOST_TEST_EQ(global.news(), 1U);
    }
    const wd::document source = sample();
    arena a(mebibyte);
    auto copy = wd::from_image(source.image(), a.allocator());
    require(BOOST_TEST(copy.has_value()));
    BOOST_TEST(same_image(*copy, source));
}

// A move takes the image and the allocator; the document moved from holds nothing.
void move_takes_allocator() {
    arena a(mebibyte);
    const wd::document source = sample();
    auto made = wd::from_image(source.image(), a.allocator());
    require(BOOST_TEST(made.has_value()));
    const global_count global;
    wd::pmr::document moved(std::move(*made));
    BOOST_TEST(moved.get_allocator().resource() == &a.resource());
    BOOST_TEST(made->image().empty());
    BOOST_TEST(same_image(moved, source));
    const wd::pmr::document again(std::move(moved));
    BOOST_TEST(same_image(again, source));
    BOOST_TEST_EQ(global.news(), 0U);
}

// A move assignment between resources copies the image into the target's resource, one copy of
// its bytes, and frees the source's in its own; between equal resources it takes the image.
void move_assign_across_resources() {
    arena a(mebibyte);
    arena b(mebibyte);
    const wd::document source = sample();
    auto in_a = wd::pmr::document::create(a.allocator());
    auto in_b = wd::from_image(source.image(), b.allocator());
    require(BOOST_TEST(in_a.has_value() && in_b.has_value()));
    const std::uint64_t a_before = a.resource().allocations();
    const global_count global;
    *in_a = std::move(*in_b);
    BOOST_TEST_EQ(global.news(), 0U);
    BOOST_TEST(in_a->get_allocator().resource() == &a.resource());
    BOOST_TEST(same_image(*in_a, source));
    BOOST_TEST(in_b->image().empty());
    BOOST_TEST_EQ(a.resource().allocations(), a_before + 1);
    BOOST_TEST_EQ(b.resource().bytes_in_use(), 0U);

    // Between documents of one resource, the image itself is taken.
    auto other = wd::pmr::document::create(a.allocator());
    require(BOOST_TEST(other.has_value()));
    const std::uint64_t a_taken = a.resource().allocations();
    *other = std::move(*in_a);
    BOOST_TEST_EQ(a.resource().allocations(), a_taken);
    BOOST_TEST(same_image(*other, source));

    // A document moved from, assigned to one of another resource, leaves it holding nothing.
    *in_b = std::move(*in_a);
    BOOST_TEST(in_b->image().empty());
    BOOST_TEST(in_b->get_allocator().resource() == &b.resource());
}

// clone copies into the document's own resource, clone(alloc) into another's, and from_image into
// any allocator's.
void clone_into_resources() {
    arena a(mebibyte);
    arena b(mebibyte);
    const wd::document source = sample();
    auto in_a = wd::from_image(source.image(), a.allocator());
    require(BOOST_TEST(in_a.has_value()));
    const global_count global;
    auto same = in_a->clone();
    auto other = in_a->clone(b.allocator());
    require(BOOST_TEST(same.has_value() && other.has_value()));
    BOOST_TEST_EQ(global.news(), 0U);
    BOOST_TEST(same->get_allocator().resource() == &a.resource());
    BOOST_TEST(other->get_allocator().resource() == &b.resource());
    BOOST_TEST(same_image(*same, source));
    BOOST_TEST(same_image(*other, source));
}

// An external document carries the allocator of its working state: format, open, compact_into and
// clone keep it, and none of them calls the global operator new.
void external_carries_allocator() {
    arena a(mebibyte);
    const wd::document source = sample();
    alignas(16) std::array<std::byte, 8192> buffer{};
    alignas(16) std::array<std::byte, 8192> compacted{};
    std::copy(source.image().begin(), source.image().end(), buffer.begin());
    const global_count global;
    auto opened = wd::open(std::span(buffer), a.allocator());
    require(BOOST_TEST(opened.has_value()));
    BOOST_TEST(opened->get_allocator().resource() == &a.resource());
    BOOST_TEST(same_image(*opened, source));
    auto packed = opened->compact_into(compacted);
    require(BOOST_TEST(packed.has_value()));
    BOOST_TEST(packed->get_allocator().resource() == &a.resource());
    auto owned = opened->clone();
    require(BOOST_TEST(owned.has_value()));
    BOOST_TEST(owned->get_allocator().resource() == &a.resource());
    alignas(16) std::array<std::byte, 4096> empty{};
    auto formatted = wd::format(std::span(empty), a.allocator());
    require(BOOST_TEST(formatted.has_value()));
    BOOST_TEST(formatted->root().is_null());
    BOOST_TEST_EQ(global.news(), 0U);
}

// Any allocator of std::byte works: one of the program's own, stateful, rebound to the image's
// blocks, allocates and frees the image, and copies keep it.
void any_allocator() {
    allocator_counts counts;
    using storage = wd::basic_owned<tallying_allocator<std::byte>>;
    using document = wd::basic_document<wd::default_traits, storage>;
    const tallying_allocator<std::byte> alloc(&counts);
    {
        auto made = document::create(alloc);
        require(BOOST_TEST(made.has_value()));
        BOOST_TEST(made->reserve(65536).has_value());
        const wd::document source = sample();
        auto copy = wd::from_image(source.image(), alloc);
        require(BOOST_TEST(copy.has_value()));
        BOOST_TEST(copy->get_allocator() == alloc);
        BOOST_TEST(same_image(*copy, source));
    }
    BOOST_TEST_EQ(counts.allocations, 3U);
    BOOST_TEST_EQ(counts.deallocations, 3U);
}

// A document that holds nothing allocates nothing, in its allocator or the global heap, reads as a
// moved-from document does, refuses a change and a copy with errc::type, and takes an image by a
// move assignment.
template <class Storage>
void holding_nothing() {
    using document = wd::basic_document<wd::default_traits, Storage>;
    arena a(mebibyte);
    const wd::document source = sample();
    const global_count global;
    document none = document::nothing(a.allocator());
    BOOST_TEST(none.get_allocator().resource() == &a.resource());
    BOOST_TEST(none.root().is_undefined());
    BOOST_TEST(none.image().empty());
    BOOST_TEST_EQ(none.capacity(), 0U);
    BOOST_TEST(none.root().set(1).error() == wd::errc::type);
    BOOST_TEST(none.clone().error() == wd::errc::type);
    BOOST_TEST_EQ(global.news(), 0U);
    BOOST_TEST_EQ(a.resource().allocations(), 0U);
    if constexpr (wd::detail::owned_storage<Storage>) {
        auto made = wd::from_image(source.image(), a.allocator());
        require(BOOST_TEST(made.has_value()));
        none = std::move(*made);
        BOOST_TEST(same_image(none, source));
    }
}

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS

// The sugar's copy asks select_on_container_copy_construction, which gives a polymorphic
// allocator the default resource; the allocator-extended copy takes the one given.
void copies_select_allocator() {
    arena a(mebibyte);
    arena b(mebibyte);
    const wd::document source = sample();
    auto made = wd::from_image(source.image(), a.allocator());
    require(BOOST_TEST(made.has_value()));
    // The copy constructor itself is what is tested, hence a copy where a reference would read.
    const wd::pmr::document copied(*made);  // NOLINT(performance-unnecessary-copy-initialization)
    BOOST_TEST(copied.get_allocator().resource() == std::pmr::get_default_resource());
    const wd::pmr::document placed(*made, b.allocator());
    BOOST_TEST(placed.get_allocator().resource() == &b.resource());
    BOOST_TEST(same_image(placed, source));
    const wd::pmr::document fresh(a.allocator());
    BOOST_TEST(fresh.get_allocator().resource() == &a.resource());
    BOOST_TEST(fresh.root().is_null());
}

// An allocator with no room is errc::capacity, and changes nothing: a document that cannot be made,
// grown, copied or read from an image; and a move assignment that cannot copy throws std::bad_alloc
// and leaves both documents as they were.
void full_allocator_is_capacity() {
    const wd::document source = sample();
    {
        arena tiny(1024);
        BOOST_TEST(wd::pmr::document::create(tiny.allocator()).error() == wd::errc::capacity);
        BOOST_TEST(wd::from_image(source.image(), tiny.allocator()).error() == wd::errc::capacity);
    }
    // Room for one empty document's 4,096 bytes and not for a second.
    arena small(6144);
    auto made = wd::pmr::document::create(small.allocator());
    require(BOOST_TEST(made.has_value()));
    const std::vector<std::byte> before = bytes_of(*made);
    BOOST_TEST(made->reserve(65536).error() == wd::errc::capacity);
    BOOST_TEST_EQ(made->capacity(), 4096U);
    BOOST_TEST(bytes_of(*made) == before);
    BOOST_TEST(made->clone().error() == wd::errc::capacity);
    BOOST_TEST(made->root().is_null());

    arena large(mebibyte);
    auto full = wd::from_image(source.image(), large.allocator());
    require(BOOST_TEST(full.has_value()));
    bool thrown = false;
    try {
        *made = std::move(*full);
    } catch (const std::bad_alloc&) {
        thrown = true;
    }
    BOOST_TEST(thrown);
    BOOST_TEST(bytes_of(*made) == before);
    BOOST_TEST(same_image(*full, source));
}

#endif  // WEBCPP_DOCUMENT_NO_EXCEPTIONS

}  // namespace

int main() {
    image_in_resource();
    default_storage_unchanged();
    move_takes_allocator();
    move_assign_across_resources();
    clone_into_resources();
    external_carries_allocator();
    any_allocator();
    holding_nothing<wd::pmr::owned>();
    holding_nothing<wd::pmr::external>();
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
    copies_select_allocator();
    full_allocator_is_capacity();
#endif
    return boost::report_errors();
}
