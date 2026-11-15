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
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
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

    // The Allocator requirements construct a rebound allocator from this one, `A a(b)`, and ask no
    // conversion; this one converts implicitly, as std::allocator does, and explicit_allocator
    // below does not.
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

/**
 How an allocator of the test's own propagates: on a move assignment, and whether two of them are
 always equal.
*/
enum class propagation : std::uint8_t {
    /** Neither: a move assignment between two that differ copies the image. */
    none,

    /** It propagates on a move assignment. */
    on_move,

    /** Two of them are always equal, whatever they count in. */
    always_equal,
};

/**
 A stateful allocator of the program's own, neither standard nor polymorphic, whose rebinding
 constructor is explicit, as the Allocator requirements allow: a rebound allocator is constructed
 from it, `A a(b)`, never converted to. It counts through a pointer, and `How` says how it
 propagates.
*/
template <class T, propagation How = propagation::none>
class explicit_allocator {
public:
    /** The type allocated. */
    using value_type = T;

    /** Whether it propagates on a move assignment. */
    using propagate_on_container_move_assignment = std::bool_constant<How == propagation::on_move>;

    /** Whether two of them are always equal. */
    using is_always_equal = std::bool_constant<How == propagation::always_equal>;

    /** The same allocator for another type. */
    template <class U>
    struct rebind {
        /** The allocator of `U`. */
        using other = explicit_allocator<U, How>;
    };

    /** An allocator that counts in `counts`, which outlives it. */
    explicit explicit_allocator(allocator_counts* counts) noexcept : counts_(counts) {}

    /** The same allocator for another type, constructed from it and never converted to. */
    template <class U>
    explicit explicit_allocator(const explicit_allocator<U, How>& other) noexcept
        : counts_(other.counts()) {}

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

    /** Whether two allocators count in the same place, or are always equal. */
    template <class U>
    friend bool operator==(const explicit_allocator& a,
                           const explicit_allocator<U, How>& b) noexcept {
        return How == propagation::always_equal || a.counts_ == b.counts();
    }

private:
    allocator_counts* counts_;
};

/** A document of `Traits` whose storage owns its image in an @ref explicit_allocator. */
template <class Traits, propagation How = propagation::none>
using explicit_document =
    wd::basic_document<Traits, wd::basic_owned<explicit_allocator<std::byte, How>>>;

/** What an allocator of @ref aligning_allocator does: its counts, and whether it under-aligns. */
struct alignment_control {
    /** The allocations it made. */
    std::uint64_t allocations = 0;

    /** The deallocations it made. */
    std::uint64_t deallocations = 0;

    /** Whether each block it hands out is 8 bytes past a 16-aligned address. */
    bool under_align = false;
};

/**
 An allocator that, when told to, aligns every block to 8 bytes and never to 16, whatever the type
 asks, as an allocator that ignores an over-aligned request may: a block of the image, alignas(16),
 is over-aligned wherever std::max_align_t's alignment is 8, as on arm64 macOS and on MSVC.
*/
template <class T>
class aligning_allocator {
public:
    /** The type allocated. */
    using value_type = T;

    /** An allocator that does as `control` says and counts there; `control` outlives it. */
    explicit aligning_allocator(alignment_control* control) noexcept : control_(control) {}

    // Implicit, as std::allocator's conversion is: the Allocator requirements ask only `A a(b)`.
    /** The same allocator for another type. */
    template <class U>
    // NOLINTNEXTLINE(misc-explicit-constructor)
    aligning_allocator(const aligning_allocator<U>& other) noexcept : control_(other.control()) {}

    /** Room for `n` objects of `T`, 16-aligned or, when told to, 8 bytes past it. */
    T* allocate(std::size_t n) {
        ++control_->allocations;
        auto* block =
            static_cast<std::byte*>(::operator new((n * sizeof(T)) + 16, std::align_val_t{16}));
        return reinterpret_cast<T*>(block + offset());
    }

    /** Gives back what @ref allocate made. */
    void deallocate(T* p, std::size_t /*n*/) noexcept {
        ++control_->deallocations;
        // A block is given back from the 16-aligned address it was made at.
        auto* block = reinterpret_cast<std::byte*>(p);
        const std::size_t past = reinterpret_cast<std::uintptr_t>(block) % 16;
        ::operator delete(block - past, std::align_val_t{16});
    }

    /** What it does and counts. */
    [[nodiscard]] alignment_control* control() const noexcept { return control_; }

    /** Whether two allocators do as the same control says. */
    template <class U>
    friend bool operator==(const aligning_allocator& a, const aligning_allocator<U>& b) noexcept {
        return a.control_ == b.control();
    }

private:
    /** The bytes past a 16-aligned address it hands out. */
    [[nodiscard]] std::size_t offset() const noexcept { return control_->under_align ? 8 : 0; }

    alignment_control* control_;
};

/** The tag a @ref tagging_allocator's pointers carry, which an address alone does not give. */
constexpr std::uint64_t pointer_tag = 0x5ea1;

/**
 A pointer that is not a plain one, as an allocator of a segment of shared memory may hand out:
 an address and a tag, which a pointer rebuilt from the address alone does not carry.
*/
template <class T>
class tagged_pointer {
public:
    /** The type pointed to. */
    using element_type = T;

    /** A difference of pointers. */
    using difference_type = std::ptrdiff_t;

    /** A null pointer. */
    tagged_pointer() noexcept = default;

    // A null pointer converts to it implicitly, as the NullablePointer requirements ask.
    /** A null pointer. */
    // NOLINTNEXTLINE(misc-explicit-constructor)
    tagged_pointer(std::nullptr_t /*null*/) noexcept {}

    /** The pointer to `address` that carries `tag`. */
    tagged_pointer(T* address, std::uint64_t tag) noexcept : address_(address), tag_(tag) {}

    /** What `std::pointer_traits` rebuilds from an object alone: its address, with no tag. */
    template <class U = T>
    static tagged_pointer pointer_to(U& object) noexcept {
        return tagged_pointer(std::addressof(object), 0);
    }

    /** The address. */
    T* operator->() const noexcept { return address_; }

    /** Whether it is not null. */
    explicit operator bool() const noexcept { return address_ != nullptr; }

    /** The tag. */
    [[nodiscard]] std::uint64_t tag() const noexcept { return tag_; }

    /** Whether two pointers have the same address and tag. */
    friend bool operator==(const tagged_pointer& a, const tagged_pointer& b) noexcept = default;

private:
    T* address_ = nullptr;
    std::uint64_t tag_ = 0;
};

/** What a @ref tagging_allocator counts. */
struct tag_counts {
    /** The allocations it made. */
    std::uint64_t allocations = 0;

    /** The deallocations it made. */
    std::uint64_t deallocations = 0;

    /** The deallocations given a pointer without the tag it handed out. */
    std::uint64_t untagged = 0;
};

/**
 An allocator whose pointer to the image's blocks is a @ref tagged_pointer, and to anything else a
 plain one: it counts a block given back through a pointer that lost its tag.
*/
template <class T>
class tagging_allocator {
public:
    /** Whether its pointer is tagged: for the image's blocks. */
    static constexpr bool tagged = std::same_as<T, wd::detail::image_block>;

    /** The type allocated. */
    using value_type = T;

    /** Its pointer. */
    using pointer = std::conditional_t<tagged, tagged_pointer<T>, T*>;

    /** An allocator that counts in `counts`, which outlives it. */
    explicit tagging_allocator(tag_counts* counts) noexcept : counts_(counts) {}

    // Implicit, as std::allocator's conversion is: the Allocator requirements ask only `A a(b)`.
    /** The same allocator for another type. */
    template <class U>
    // NOLINTNEXTLINE(misc-explicit-constructor)
    tagging_allocator(const tagging_allocator<U>& other) noexcept : counts_(other.counts()) {}

    /** Room for `n` objects of `T`, aligned for `T`. */
    pointer allocate(std::size_t n) {
        ++counts_->allocations;
        auto* p = static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t{alignof(T)}));
        if constexpr (tagged) {
            return pointer(p, pointer_tag);
        } else {
            return p;
        }
    }

    /** Gives back what @ref allocate made, counting a pointer that lost its tag. */
    void deallocate(pointer p, std::size_t /*n*/) noexcept {
        ++counts_->deallocations;
        if constexpr (tagged) {
            counts_->untagged += p.tag() == pointer_tag ? 0 : 1;
            ::operator delete(p.operator->(), std::align_val_t{alignof(T)});
        } else {
            ::operator delete(p, std::align_val_t{alignof(T)});
        }
    }

    /** The counts. */
    [[nodiscard]] tag_counts* counts() const noexcept { return counts_; }

    /** Whether two allocators count in the same place. */
    template <class U>
    friend bool operator==(const tagging_allocator& a, const tagging_allocator<U>& b) noexcept {
        return a.counts_ == b.counts();
    }

private:
    tag_counts* counts_;
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

// A move assignment is noexcept where it never copies: for the standard allocator, for an external
// document whatever its allocator, and for an allocator that propagates or is always equal; an
// owned document of polymorphic allocators, which may differ, may copy and throw.
static_assert(std::is_nothrow_move_assignable_v<wd::document>);
static_assert(
    std::is_nothrow_move_assignable_v<wd::basic_document<wd::default_traits, wd::external>>);
static_assert(
    std::is_nothrow_move_assignable_v<wd::basic_document<wd::default_traits, wd::pmr::external>>);
static_assert(std::is_nothrow_move_assignable_v<
              explicit_document<wd::default_traits, propagation::always_equal>>);
static_assert(
    std::is_nothrow_move_assignable_v<explicit_document<wd::default_traits, propagation::on_move>>);
static_assert(!std::is_nothrow_move_assignable_v<wd::pmr::document>);
static_assert(!std::is_nothrow_move_assignable_v<explicit_document<wd::default_traits>>);

// An allocator that propagates on a move assignment is taken with the image: the target's own image
// is given back to its own allocator, nothing is copied, and the target allocates in the source's
// allocator from then on.
void propagating_allocator() {
    allocator_counts target_counts;
    allocator_counts source_counts;
    using document = explicit_document<wd::default_traits, propagation::on_move>;
    using allocator = explicit_allocator<std::byte, propagation::on_move>;
    auto target = document::create(allocator(&target_counts));
    auto source = document::create(allocator(&source_counts));
    require(BOOST_TEST(target.has_value() && source.has_value()));
    BOOST_TEST(source->root().set(wd::object{{"name", "document"}}).has_value());
    const std::byte* image = source->image().data();
    const std::uint64_t made = source_counts.allocations;
    *target = std::move(*source);
    BOOST_TEST(target->image().data() == image);
    BOOST_TEST(target->get_allocator().counts() == &source_counts);
    BOOST_TEST_EQ(source_counts.allocations, made);
    BOOST_TEST_EQ(target_counts.allocations, target_counts.deallocations);
    BOOST_TEST(target->reserve(65536).has_value());
    BOOST_TEST_EQ(source_counts.allocations, made + 1);
    BOOST_TEST((*target)["name"].get<std::string_view>().value() == "document");
}

// An allocator whose instances are always equal, though each counts in its own place, lets a move
// assignment take the image as it is, with no copy; what it allocated is given back through the
// target's, which equals it.
void always_equal_allocator() {
    allocator_counts target_counts;
    allocator_counts source_counts;
    {
        using document = explicit_document<wd::default_traits, propagation::always_equal>;
        using allocator = explicit_allocator<std::byte, propagation::always_equal>;
        auto target = document::create(allocator(&target_counts));
        auto source = document::create(allocator(&source_counts));
        require(BOOST_TEST(target.has_value() && source.has_value()));
        const std::byte* image = source->image().data();
        const std::uint64_t made = target_counts.allocations + source_counts.allocations;
        *target = std::move(*source);
        BOOST_TEST(target->image().data() == image);
        BOOST_TEST(target->get_allocator().counts() == &target_counts);
        BOOST_TEST_EQ(target_counts.allocations + source_counts.allocations, made);
    }
    BOOST_TEST_EQ(target_counts.allocations + source_counts.allocations,
                  target_counts.deallocations + source_counts.deallocations);
}

// A stateful allocator of the program's own, neither standard nor polymorphic, whose rebinding
// constructor is explicit, holds the image and every operation's working state, under both traits:
// a parse whole and in chunks with a selector, the changes through handles, a missing member's key
// and its copy, Object.assign, the array methods, compaction, a copy, and stringify_to of a value
// deeper than its stack's own room in chunks larger than its piece's. It is given back all it gave.
template <class Traits>
void explicit_rebinding() {
    allocator_counts counts;
    const explicit_allocator<std::byte> alloc(&counts);
    using parser = wd::basic_parser<Traits, wd::basic_owned<explicit_allocator<std::byte>>>;
    const std::string key(120, 'k');
    const std::string text(120, 't');
    const std::string json = R"({"list":[1,")" + text + R"(",{"x":true}],")" + key + R"(":)" +
                             std::string(40, '[') + std::string(40, ']') + "}";
    {
        auto whole = wd::parse<Traits>(json, wd::parse_options(), alloc);
        require(BOOST_TEST(whole.has_value()));
        parser p(wd::parse_options(), alloc);
        std::uint64_t seen = 0;
        BOOST_TEST(p.on("$.list[*]", [&seen](const auto&, wd::path_view) { ++seen; }).has_value());
        for (std::size_t at = 0; at < json.size(); at += 7) {
            BOOST_TEST(p.write(std::string_view(json).substr(at, 7)).has_value());
        }
        const auto chunked = p.finish();
        require(BOOST_TEST(chunked.has_value()));
        BOOST_TEST_EQ(seen, 3U);
        auto& d = *whole;
        const auto missing = d[std::string(150, 'm')];
        auto copy = missing;
        BOOST_TEST(copy.set(wd::object{{key, text}, {"list", wd::array{1, text}}}).has_value());
        BOOST_TEST(d["o"].set(wd::object()).has_value());
        BOOST_TEST(d["o"].assign_from(d["list"]).has_value());
        BOOST_TEST(d["list"].push(text, d["o"]).has_value());
        const auto sliced = d["list"].slice(1);
        const auto joined = d["list"].concat(d["list"], text);
        const auto popped = d["list"].pop();
        const auto spliced = d["list"].splice(0, 1, text);
        require(BOOST_TEST(sliced.has_value() && joined.has_value() && popped.has_value() &&
                           spliced.has_value()));
        BOOST_TEST(d.compact().has_value());
        auto cloned = d.clone();
        require(BOOST_TEST(cloned.has_value()));
        BOOST_TEST(*cloned == d);
        std::string out;
        BOOST_TEST(wd::stringify_to(
                       d, [&out](std::string_view piece) { out.append(piece); }, 10, 1U << 20U)
                       .has_value());
        BOOST_TEST(out == wd::stringify(d, 10));
        BOOST_TEST_GT(out.size(), 8192U);
    }
    BOOST_TEST_GT(counts.allocations, 0U);
    BOOST_TEST_EQ(counts.allocations, counts.deallocations);
}

// An allocator that gives the image a block not aligned to 16 has it given back, and the operation
// is errc::capacity with nothing changed: a document that cannot be made, read from an image,
// grown, shrunk, compacted or copied; a heap at another alignment would put its first block where
// the tree does not look for it. The allocator is given back every block it made.
void under_aligned_image_is_capacity() {
    using storage = wd::basic_owned<aligning_allocator<std::byte>>;
    using document = wd::basic_document<wd::default_traits, storage>;
    alignment_control control;
    const aligning_allocator<std::byte> alloc(&control);
    const wd::document source = sample();
    {
        control.under_align = true;
        BOOST_TEST(document::create(alloc).error() == wd::errc::capacity);
        BOOST_TEST(wd::from_image(source.image(), alloc).error() == wd::errc::capacity);
        BOOST_TEST_EQ(control.allocations, 2U);
        BOOST_TEST_EQ(control.deallocations, 2U);

        control.under_align = false;
        auto made = wd::from_image(source.image(), alloc);
        require(BOOST_TEST(made.has_value()));
        BOOST_TEST(made->reserve(16384).has_value());
        const std::vector<std::byte> before = bytes_of(*made);
        control.under_align = true;
        BOOST_TEST(made->reserve(65536).error() == wd::errc::capacity);
        BOOST_TEST(made->shrink_to_fit().error() == wd::errc::capacity);
        BOOST_TEST(made->compact().error() == wd::errc::capacity);
        BOOST_TEST(made->clone().error() == wd::errc::capacity);
        BOOST_TEST_EQ(made->capacity(), 16384U);
        BOOST_TEST(bytes_of(*made) == before);
        BOOST_TEST(made->root()["name"].set("changed").has_value());
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
        // A move assignment between allocators that differ copies the image, and has no result to
        // return the failure in: it throws std::bad_alloc and leaves both documents as they were.
        alignment_control other_control;
        auto target = document::create(aligning_allocator<std::byte>(&other_control));
        require(BOOST_TEST(target.has_value()));
        other_control.under_align = true;
        const std::vector<std::byte> target_before = bytes_of(*target);
        const std::vector<std::byte> source_before = bytes_of(*made);
        bool thrown = false;
        try {
            *target = std::move(*made);
        } catch (const std::bad_alloc&) {
            thrown = true;
        }
        BOOST_TEST(thrown);
        BOOST_TEST(bytes_of(*target) == target_before);
        BOOST_TEST(bytes_of(*made) == source_before);
        other_control.under_align = false;
#endif
    }
    BOOST_TEST_EQ(control.allocations, control.deallocations);
}

// An allocator whose pointer is not a plain one is given back the very pointers it handed out, tag
// and all: the image's pointer is kept as the allocator gave it, never rebuilt from an address,
// whether the image is made, grown, changed, shrunk, compacted, copied or moved.
void fancy_pointers_kept() {
    tag_counts counts;
    using storage = wd::basic_owned<tagging_allocator<std::byte>>;
    using document = wd::basic_document<wd::default_traits, storage>;
    const tagging_allocator<std::byte> alloc(&counts);
    {
        auto made = document::create(alloc);
        require(BOOST_TEST(made.has_value()));
        BOOST_TEST(made->reserve(65536).has_value());
        BOOST_TEST(made->root()
                       .set(wd::object{{"name", "document"}, {"list", wd::array{1, 2}}})
                       .has_value());
        BOOST_TEST(made->shrink_to_fit().has_value());
        BOOST_TEST(made->compact().has_value());
        auto copy = made->clone();
        require(BOOST_TEST(copy.has_value()));
        document moved(std::move(*made));
        *copy = std::move(moved);
        BOOST_TEST(copy->root()["name"].get<std::string_view>().value() == "document");
    }
    BOOST_TEST_GT(counts.allocations, 0U);
    BOOST_TEST_EQ(counts.allocations, counts.deallocations);
    BOOST_TEST_EQ(counts.untagged, 0U);
}

// A document that holds nothing allocates nothing, in its allocator or the global heap, reads as a
// moved-from document does, refuses a change and a copy with errc::type, and takes an image by a
// move assignment.
template <class Storage>
void holding_nothing() {
    using document = wd::basic_document<wd::default_traits, Storage>;
    arena a(mebibyte);
    const wd::document source = sample();
    // Literals hold their values in the standard allocator: made before anything is counted.
    const wd::object members{{"key", wd::array{1, "text"}}};
    const wd::array elements{1, 2};
    const global_count global;
    document none = document::nothing(a.allocator());
    BOOST_TEST(none.get_allocator().resource() == &a.resource());
    BOOST_TEST(none.root().is_undefined());
    BOOST_TEST(none.image().empty());
    BOOST_TEST_EQ(none.capacity(), 0U);
    auto root = none.root();
    BOOST_TEST(root.set(1).error() == wd::errc::type);
    BOOST_TEST(root.set(members).error() == wd::errc::type);
    BOOST_TEST(root["key"].set(elements).error() == wd::errc::type);
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

/** The error of `made`; none when it holds a document. */
template <class Document>
boost::system::error_code error_of(const wd::result<Document>& made) {
    return made.has_value() ? boost::system::error_code() : made.error();
}

/**
 Runs `op` over `failing`, which allows each budget from 0 until `op` succeeds: each failure must be
 errc::capacity, and at least one budget must fail.
*/
template <class Op>
void capacity_until_it_fits(webcpp::test::interprocess_failing_resource& failing, Op op) {
    constexpr std::uint64_t unlimited = std::uint64_t{1} << 40U;
    for (std::uint64_t budget = 0;; ++budget) {
        failing.allow(budget);
        const boost::system::error_code failed = op();
        failing.allow(unlimited);
        if (!failed) {
            BOOST_TEST_GT(budget, 0U);
            return;
        }
        BOOST_TEST(failed == wd::errc::capacity);
        // An operation that kept failing would never end the loop.
        if (!BOOST_TEST_LT(budget, 10000U)) {
            return;
        }
    }
}

// An allocator of Boost.Interprocess fails with its own bad_alloc, which derives from
// std::exception alone: it is errc::capacity as std::bad_alloc is, at every allocation of the
// image, of a change's staging, of a prepared value, of a missing member's key, of the parser's
// working state and selectors, of the writer's, and of the array methods' copies.
void interprocess_failure_is_capacity() {
    webcpp::test::interprocess_failing_resource failing(std::pmr::new_delete_resource(), 0);
    const std::pmr::polymorphic_allocator<std::byte> alloc(&failing);
    const std::string key(120, 'k');
    const std::string text(120, 't');
    const std::string json = R"({"list":[1,")" + text + R"(",[true]],")" + key + R"(":)" +
                             std::string(40, '[') + std::string(40, ']') + "}";
    const wd::object members{{key, text}, {"list", wd::array{1, text, wd::object{{key, true}}}}};
    const auto source = wd::parse(json);
    require(BOOST_TEST(source.has_value()));
    capacity_until_it_fits(failing, [&] { return error_of(wd::pmr::document::create(alloc)); });
    capacity_until_it_fits(failing, [&] {
        auto d = wd::from_image(source->image(), alloc);
        return d.has_value() ? d->reserve(1U << 20U).error() : d.error();
    });
    capacity_until_it_fits(failing,
                           [&] { return error_of(wd::parse(json, wd::parse_options(), alloc)); });
    capacity_until_it_fits(failing, [&]() -> boost::system::error_code {
        wd::pmr::parser p(wd::parse_options(), alloc);
        if (const auto added = p.on("$.list[*]", [](const auto&, wd::path_view) {}); !added) {
            return added.error();
        }
        for (std::size_t at = 0; at < json.size(); ++at) {
            if (const auto written = p.write(std::string_view(json).substr(at, 1)); !written) {
                return written.error();
            }
        }
        return error_of(p.finish());
    });
    capacity_until_it_fits(failing, [&]() -> boost::system::error_code {
        auto d = wd::from_image(source->image(), alloc);
        if (!d.has_value()) {
            return d.error();
        }
        if (const auto set = (*d)["o"].set(wd::object()); !set) {
            return set.error();
        }
        return (*d)["o"][key].set(members).error();
    });
    capacity_until_it_fits(failing, [&]() -> boost::system::error_code {
        auto d = wd::from_image(source->image(), alloc);
        if (!d.has_value()) {
            return d.error();
        }
        std::string out;
        return wd::stringify_to(
                   *d, [&out](std::string_view piece) { out.append(piece); }, 10, 1U << 20U)
            .error();
    });
    capacity_until_it_fits(failing, [&]() -> boost::system::error_code {
        auto d = wd::from_image(source->image(), alloc);
        if (!d.has_value()) {
            return d.error();
        }
        if (const auto copied = (*d)["list"].concat((*d)["list"], members); !copied) {
            return copied.error();
        }
        return d->compact().error();
    });
}

// A move assignment that cannot copy the image into an allocator of Boost.Interprocess, which
// fails with its own bad_alloc, throws std::bad_alloc and leaves the source as it was.
void interprocess_failure_throws_from_a_move() {
    webcpp::test::interprocess_failing_resource failing(std::pmr::new_delete_resource(), 1);
    const std::pmr::polymorphic_allocator<std::byte> alloc(&failing);
    const auto source = wd::parse(R"({"name":"document","list":[1,2]})");
    require(BOOST_TEST(source.has_value()));
    auto target = wd::pmr::document::create(alloc);
    auto other = wd::from_image(source->image());
    require(BOOST_TEST(target.has_value() && other.has_value()));
    wd::pmr::document from = wd::pmr::document::nothing();
    {
        auto copied = wd::from_image(source->image(), std::pmr::polymorphic_allocator<std::byte>());
        require(BOOST_TEST(copied.has_value()));
        from = std::move(*copied);
    }
    failing.allow(0);
    bool thrown = false;
    try {
        *target = std::move(from);
    } catch (const std::bad_alloc&) {
        thrown = true;
    }
    failing.allow(std::uint64_t{1} << 40U);
    BOOST_TEST(thrown);
    // The assignment threw before it took anything: `from` is read as it was.
    BOOST_TEST(same_image(from, *other));  // NOLINT(bugprone-use-after-move)
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
    propagating_allocator();
    always_equal_allocator();
    explicit_rebinding<wd::default_traits>();
    explicit_rebinding<wd::js_traits>();
    under_aligned_image_is_capacity();
    fancy_pointers_kept();
    holding_nothing<wd::pmr::owned>();
    holding_nothing<wd::pmr::external>();
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
    copies_select_allocator();
    full_allocator_is_capacity();
    interprocess_failure_is_capacity();
    interprocess_failure_throws_from_a_move();
#endif
    return boost::report_errors();
}
