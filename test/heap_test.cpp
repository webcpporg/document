// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The heap of an image and what is built on it: strings of exact size that leave no byte behind,
// an allocator that hands out and frees zeros, probes that leave the heap as it was, regions owned
// and external, grown, copied and shrunk, the same bytes at any address, and the header's check.

#include <webcpp/document/detail/heap.hpp>
#include <webcpp/document/image.hpp>

#include <boost/core/lightweight_test.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace wd = webcpp::document;
namespace detail = webcpp::document::detail;

namespace {

constexpr std::uint64_t mebibyte = std::uint64_t{1} << 20U;

std::span<const std::byte> bytes_of(std::string_view s) {
    return std::as_bytes(std::span(s.data(), s.size()));
}

std::span<const std::byte> image_of(const detail::region& r) {
    return {r.data(), static_cast<std::size_t>(r.size())};
}

bool contains(std::span<const std::byte> image, std::string_view needle) {
    return !std::ranges::search(image, bytes_of(needle)).empty();
}

// The offset of a pointer into a region from its first byte, which survives the region's move.
std::size_t offset_in(const detail::region& r, const void* p) {
    return static_cast<std::size_t>(static_cast<const std::byte*>(p) - r.data());
}

bool reads_at(const detail::region& r, std::size_t offset, std::string_view text) {
    return std::memcmp(r.data() + offset, text.data(), text.size()) == 0;
}

void strings_are_exact_and_zeroed_when_freed() {
    auto r = detail::region::owned(4096, mebibyte);
    if (!BOOST_TEST(r.has_value())) {
        return;
    }
    auto& h = detail::heap_of(*r);
    auto s = detail::make_string(h, bytes_of("a secret of 32 bytes, exactly...."));
    if (!BOOST_TEST(s.has_value())) {
        return;
    }
    BOOST_TEST_EQ(s->size, 33U);
    BOOST_TEST(s->chars() == "a secret of 32 bytes, exactly....");
    BOOST_TEST(contains(image_of(*r), "a secret of 32 bytes"));
    detail::free_string(h, *s);
    BOOST_TEST_EQ(s->size, 0U);
    BOOST_TEST(s->view().empty());
    BOOST_TEST(!contains(image_of(*r), "a secret of 32 bytes"));
}

// The heap writes its links over a freed block's first bytes, which would hide a short secret left
// there: this one lies past them.
void a_long_secret_leaves_no_byte() {
    auto r = detail::region::owned(4096, mebibyte);
    if (!BOOST_TEST(r.has_value())) {
        return;
    }
    auto& h = detail::heap_of(*r);
    std::array<char, 256> text{};
    text.fill('.');
    const std::string_view secret = "the secret lies at the end";
    std::memcpy(text.data() + text.size() - secret.size(), secret.data(), secret.size());
    auto s = detail::make_string(h, std::as_bytes(std::span(text)));
    if (!BOOST_TEST(s.has_value())) {
        return;
    }
    BOOST_TEST(contains(image_of(*r), secret));
    detail::free_string(h, *s);
    BOOST_TEST(!contains(image_of(*r), secret));
    BOOST_TEST(!contains(image_of(*r), "........"));
}

void an_empty_string_allocates_nothing() {
    auto r = detail::region::owned(4096, 4096);
    if (!BOOST_TEST(r.has_value())) {
        return;
    }
    auto& h = detail::heap_of(*r);
    const auto free_before = h.get_free_memory();
    auto s = detail::make_string(h, {});
    if (!BOOST_TEST(s.has_value())) {
        return;
    }
    BOOST_TEST(s->data.get() == nullptr);
    BOOST_TEST(s->view().empty());
    BOOST_TEST(s->chars().empty());
    BOOST_TEST(s->units().empty());
    BOOST_TEST_EQ(h.get_free_memory(), free_before);
    detail::free_string(h, *s);
    BOOST_TEST_EQ(h.get_free_memory(), free_before);
}

void strings_read_as_code_units() {
    auto r = detail::region::owned(4096, 4096);
    if (!BOOST_TEST(r.has_value())) {
        return;
    }
    const std::u16string_view text = u"café";
    auto s = detail::make_string(detail::heap_of(*r), std::as_bytes(std::span(text)));
    if (!BOOST_TEST(s.has_value())) {
        return;
    }
    BOOST_TEST_EQ(s->size, 8U);
    BOOST_TEST(s->units() == text);
}

void a_full_heap_says_so_and_grows() {
    auto r = detail::region::owned(4096, mebibyte);
    if (!BOOST_TEST(r.has_value())) {
        return;
    }
    // Offsets, not the strings' own pointers: an offset_ptr copied out of the image points at the
    // old buffer once the region has moved.
    std::vector<std::size_t> kept;
    const std::string_view block = "0123456789abcdef0123456789abcdef";
    for (;;) {
        auto s = detail::make_string(detail::heap_of(*r), bytes_of(block));
        if (!s) {
            BOOST_TEST(s.error() == wd::errc::capacity);
            break;
        }
        kept.push_back(offset_in(*r, s->data.get()));
    }
    if (!BOOST_TEST(!kept.empty())) {
        return;
    }
    const std::uint64_t before = r->size();
    BOOST_TEST(r->grow(before + 1));
    BOOST_TEST_GE(r->size(), 2 * before);
    BOOST_TEST_EQ(r->size() % 16, 0U);
    // What was there moved with the image and reads the same through its offsets.
    for (const std::size_t at : kept) {
        BOOST_TEST(reads_at(*r, at, block));
    }
    BOOST_TEST(detail::heap_of(*r).check_sanity());
    BOOST_TEST(detail::make_string(detail::heap_of(*r), bytes_of(block)).has_value());
    // The heap took the whole new end: nothing past the old end was allocated before.
    BOOST_TEST_GT(detail::heap_of(*r).get_free_memory(), before / 2);
}

void growth_takes_what_is_asked() {
    auto r = detail::region::owned(4096, mebibyte);
    if (!BOOST_TEST(r.has_value())) {
        return;
    }
    BOOST_TEST(r->grow(20000));
    BOOST_TEST_EQ(r->size(), 20000U);
    BOOST_TEST(r->grow(20001));
    BOOST_TEST_EQ(r->size(), 40000U);
    BOOST_TEST(r->grow(90001));
    BOOST_TEST_EQ(r->size(), 90016U);
    BOOST_TEST(detail::heap_of(*r).check_sanity());
}

void an_owned_region_stops_at_its_limit() {
    auto r = detail::region::owned(4096, 8192);
    if (!BOOST_TEST(r.has_value())) {
        return;
    }
    BOOST_TEST(r->grow(8192));
    BOOST_TEST(!r->grow(8193));
    BOOST_TEST_EQ(r->size(), 8192U);
}

void doubling_stops_at_the_limit() {
    auto r = detail::region::owned(4096, 6008);
    if (!BOOST_TEST(r.has_value())) {
        return;
    }
    BOOST_TEST_EQ(r->max_bytes(), 6008U);
    // Twice the size is past the limit, what is asked is not: the region takes the limit, rounded
    // down to 16.
    BOOST_TEST(r->grow(5000));
    BOOST_TEST_EQ(r->size(), 6000U);
    BOOST_TEST(!r->grow(5000));
    BOOST_TEST(detail::heap_of(*r).check_sanity());
}

void an_owned_region_needs_room_for_its_heap() {
    BOOST_TEST(detail::region::owned(64, mebibyte).error() == wd::errc::capacity);
    BOOST_TEST(detail::region::owned(8192, 4096).error() == wd::errc::capacity);
    auto r = detail::region::owned(4096, 4096);
    if (!BOOST_TEST(r.has_value())) {
        return;
    }
    BOOST_TEST(r->is_owned());
    BOOST_TEST_EQ(r->size(), 4096U);
    BOOST_TEST_EQ(reinterpret_cast<std::uintptr_t>(r->data()) % 16, 0U);
    // Nothing is written before the heap: the header is the tree's.
    BOOST_TEST(std::all_of(r->data(), r->data() + detail::header_bytes,
                           [](std::byte b) { return b == std::byte{0}; }));
    BOOST_TEST(detail::heap_of(*r).check_sanity());
}

void an_external_region_never_grows() {
    alignas(16) std::array<std::byte, 4096> buffer{};
    auto r = detail::region::external(buffer);
    BOOST_TEST(!r.is_owned());
    BOOST_TEST(!r.grow(4097));
    BOOST_TEST(r.data() == buffer.data());
}

void an_external_empty_region() {
    alignas(16) std::array<std::byte, 4096> buffer{};
    buffer.fill(std::byte{0xa5});
    auto r = detail::region::external_empty(buffer);
    if (!BOOST_TEST(r.has_value())) {
        return;
    }
    BOOST_TEST(!r->is_owned());
    BOOST_TEST(r->data() == buffer.data());
    BOOST_TEST_EQ(r->size(), 4096U);
    BOOST_TEST(std::all_of(buffer.data(), buffer.data() + detail::header_bytes,
                           [](std::byte b) { return b == std::byte{0}; }));
    BOOST_TEST(detail::heap_of(*r).check_sanity());
    BOOST_TEST(detail::make_string(detail::heap_of(*r), bytes_of("in place")).has_value());
    BOOST_TEST(contains(buffer, "in place"));
    BOOST_TEST(!r->grow(4097));

    BOOST_TEST(detail::region::external_empty(std::span(buffer).first(63)).error() ==
               wd::errc::capacity);
    BOOST_TEST(detail::region::external_empty(std::span(buffer).subspan(1, 4000)).error() ==
               wd::errc::capacity);
}

void probe_answers_without_allocating() {
    auto r = detail::region::owned(4096, 4096);
    if (!BOOST_TEST(r.has_value())) {
        return;
    }
    auto& h = detail::heap_of(*r);
    const auto free_before = h.get_free_memory();
    const std::array<std::uint64_t, 2> small{100, 200};
    const std::array<std::uint64_t, 1> large{8192};
    BOOST_TEST(detail::probe(h, small));
    BOOST_TEST(!detail::probe(h, large));
    BOOST_TEST_EQ(h.get_free_memory(), free_before);
}

// A probe holds every block at once: each fits alone, all of them do not.
void probe_asks_for_all_at_once() {
    auto r = detail::region::owned(4096, 4096);
    if (!BOOST_TEST(r.has_value())) {
        return;
    }
    auto& h = detail::heap_of(*r);
    const auto free_before = h.get_free_memory();
    const std::array<std::uint64_t, 1> one{1500};
    const std::array<std::uint64_t, 3> three{1500, 1500, 1500};
    const std::array<std::uint64_t, 4> tiny{0, 1, 0, 1};
    BOOST_TEST(detail::probe(h, one));
    BOOST_TEST(!detail::probe(h, three));
    BOOST_TEST(detail::probe(h, tiny));
    BOOST_TEST(detail::probe(h, {}));
    BOOST_TEST_EQ(h.get_free_memory(), free_before);
    BOOST_TEST(h.check_sanity());
}

// A probe links the blocks it holds by their addresses, and zeroes the links before it frees them.
void probes_leave_the_same_bytes_at_any_address() {
    auto a = detail::region::owned(4096, 4096);
    auto b = detail::region::owned(4096, 4096);
    if (!BOOST_TEST(a.has_value() && b.has_value())) {
        return;
    }
    const std::array<std::uint64_t, 4> sizes{100, 1500, 7, 900};
    BOOST_TEST(detail::probe(detail::heap_of(*a), sizes));
    BOOST_TEST(detail::probe(detail::heap_of(*b), sizes));
    BOOST_TEST(a->data() != b->data());
    BOOST_TEST(std::memcmp(a->data(), b->data(), 4096) == 0);
}

void the_same_operations_give_the_same_bytes_at_any_address() {
    auto a = detail::region::owned(4096, mebibyte);
    auto b = detail::region::owned(4096, mebibyte);
    if (!BOOST_TEST(a.has_value() && b.has_value())) {
        return;
    }
    for (const auto* r : {&*a, &*b}) {
        auto& h = detail::heap_of(*r);
        auto x = detail::make_string(h, bytes_of("first"));
        const auto y = detail::make_string(h, bytes_of("second, longer"));
        detail::free_string(h, *x);
        BOOST_TEST(detail::make_string(h, bytes_of("third")).has_value());
        static_cast<void>(y);
    }
    BOOST_TEST(a->data() != b->data());
    BOOST_TEST(std::memcmp(a->data() + detail::header_bytes, b->data() + detail::header_bytes,
                           static_cast<std::size_t>(a->size() - detail::header_bytes)) == 0);
}

void an_owned_copy_reads_the_same() {
    auto r = detail::region::owned(4096, mebibyte);
    if (!BOOST_TEST(r.has_value())) {
        return;
    }
    auto s = detail::make_string(detail::heap_of(*r), bytes_of("kept in the copy"));
    if (!BOOST_TEST(s.has_value())) {
        return;
    }
    const std::size_t at = offset_in(*r, s->data.get());
    auto copy = detail::region::owned_copy(image_of(*r), mebibyte);
    if (!BOOST_TEST(copy.has_value())) {
        return;
    }
    BOOST_TEST(copy->is_owned());
    BOOST_TEST(copy->data() != r->data());
    BOOST_TEST_EQ(copy->size(), r->size());
    BOOST_TEST_EQ(copy->max_bytes(), mebibyte);
    BOOST_TEST(std::memcmp(copy->data(), r->data(), 4096) == 0);
    BOOST_TEST(reads_at(*copy, at, "kept in the copy"));
    // The copy's heap is the image's, and it allocates and grows on its own.
    auto& h = detail::heap_of(*copy);
    BOOST_TEST(h.check_sanity());
    BOOST_TEST(detail::make_string(h, bytes_of("only in the copy")).has_value());
    BOOST_TEST(!contains(image_of(*r), "only in the copy"));
    BOOST_TEST(copy->grow(8192));
    BOOST_TEST(reads_at(*copy, at, "kept in the copy"));
    BOOST_TEST(detail::heap_of(*copy).check_sanity());

    BOOST_TEST(detail::region::owned_copy(image_of(*r), 4095).error() == wd::errc::capacity);
}

void a_region_moves() {
    auto r = detail::region::owned(4096, mebibyte);
    if (!BOOST_TEST(r.has_value())) {
        return;
    }
    const std::byte* const data = r->data();
    detail::region moved(std::move(*r));
    BOOST_TEST(moved.data() == data);
    BOOST_TEST(moved.is_owned());
    BOOST_TEST(r->data() == nullptr);  // NOLINT(bugprone-use-after-move): a moved-from region
    BOOST_TEST_EQ(r->size(), 0U);
    BOOST_TEST(!r->is_owned());
    BOOST_TEST(!r->grow(8192));

    auto other = detail::region::owned(4096, mebibyte);
    if (!BOOST_TEST(other.has_value())) {
        return;
    }
    *other = std::move(moved);
    BOOST_TEST(other->data() == data);
    BOOST_TEST(moved.data() == nullptr);  // NOLINT(bugprone-use-after-move): a moved-from region
    BOOST_TEST(detail::heap_of(*other).check_sanity());

    const detail::region empty;
    BOOST_TEST(empty.data() == nullptr);
    BOOST_TEST_EQ(empty.size(), 0U);
}

void shrink_to() {
    auto r = detail::region::owned(16384, mebibyte);
    if (!BOOST_TEST(r.has_value())) {
        return;
    }
    auto& h = detail::heap_of(*r);
    auto s = detail::make_string(h, bytes_of("kept when shrunk"));
    if (!BOOST_TEST(s.has_value())) {
        return;
    }
    const std::size_t at = offset_in(*r, s->data.get());
    h.shrink_to_fit();
    const std::uint64_t end = detail::header_bytes + h.get_size();
    BOOST_TEST_LT(end, 16384U);
    const std::vector<std::byte> before(r->data(), r->data() + end);

    BOOST_TEST(r->shrink_to(end));
    BOOST_TEST_EQ(r->size(), end);
    BOOST_TEST(std::equal(before.begin(), before.end(), r->data()));
    BOOST_TEST(reads_at(*r, at, "kept when shrunk"));
    BOOST_TEST(detail::heap_of(*r).check_sanity());
    // A shrunk region grows again.
    BOOST_TEST(r->grow(end + 1));
    BOOST_TEST(reads_at(*r, at, "kept when shrunk"));
    BOOST_TEST(detail::make_string(detail::heap_of(*r), bytes_of("after")).has_value());
    BOOST_TEST(detail::heap_of(*r).check_sanity());
    BOOST_TEST(!r->shrink_to(r->size() + 16));

    alignas(16) std::array<std::byte, 4096> buffer{};
    auto e = detail::region::external_empty(buffer);
    if (!BOOST_TEST(e.has_value())) {
        return;
    }
    BOOST_TEST(!e->shrink_to(2048));
    BOOST_TEST_EQ(e->size(), 4096U);
}

void the_allocator_zeroes() {
    auto r = detail::region::owned(4096, 4096);
    if (!BOOST_TEST(r.has_value())) {
        return;
    }
    auto& h = detail::heap_of(*r);
    constexpr std::uint64_t count = 128;
    constexpr std::size_t bytes = count * sizeof(std::uint64_t);
    // A block dirtied and freed by the heap itself, which zeroes nothing; the best fit hands the
    // same block to the allocator next.
    void* dirty = h.allocate(bytes);
    if (!BOOST_TEST(dirty != nullptr)) {
        return;
    }
    std::memset(dirty, 0xa5, bytes);
    h.deallocate(dirty);

    detail::allocator<std::uint64_t> a(&h);
    BOOST_TEST(a.get_heap() == &h);
    const detail::allocator<char> rebound(a);
    BOOST_TEST(rebound.get_heap() == &h);
    BOOST_TEST(a == detail::allocator<std::uint64_t>(&h));

    const auto p = a.allocate(count);
    BOOST_TEST(static_cast<void*>(p.get()) == dirty);
    BOOST_TEST(std::all_of(p.get(), p.get() + count, [](std::uint64_t v) { return v == 0; }));

    const std::string_view pattern = "zeroed when free";
    for (std::uint64_t i = 0; i < count; i += 2) {
        std::memcpy(p.get() + i, pattern.data(), pattern.size());
    }
    BOOST_TEST(contains(image_of(*r), pattern));
    a.deallocate(p, count);
    BOOST_TEST(!contains(image_of(*r), pattern));
    // The heap writes its free block's links at the block's start; past them the block is zero.
    const auto* freed = static_cast<const std::byte*>(dirty);
    BOOST_TEST(
        std::all_of(freed + 64, freed + bytes, [](std::byte b) { return b == std::byte{0}; }));
    BOOST_TEST(h.check_sanity());
}

// Every defect of a header is refused before anything else is read.
void headers_are_checked() {
    alignas(16) std::array<std::byte, 256> image{};
    const auto write = [&](auto&& change) {
        wd::image_header h{};
        h.magic = wd::image_magic;
        h.version = wd::format_version;
        h.byte_order = 0x01020304;
        h.reserved_4 = 0;
        h.layout = 42;
        h.size = image.size();
        change(h);
        std::memcpy(image.data(), &h, sizeof h);
    };
    write([](wd::image_header&) {});
    BOOST_TEST(wd::check_image(image, 42).has_value());
    BOOST_TEST(wd::check_image(std::span(image).first(63), 42).error() == wd::errc::not_a_document);
    write([](wd::image_header& h) { h.magic[0] = 'X'; });
    BOOST_TEST(wd::check_image(image, 42).error() == wd::errc::not_a_document);
    write([&](wd::image_header& h) { h.size = image.size() + 1; });
    BOOST_TEST(wd::check_image(image, 42).error() == wd::errc::not_a_document);
    write([](wd::image_header& h) { h.size = 63; });
    BOOST_TEST(wd::check_image(image, 42).error() == wd::errc::not_a_document);
    write([](wd::image_header& h) { h.version = 2; });
    BOOST_TEST(wd::check_image(image, 42).error() == wd::errc::incompatible_document);
    write([](wd::image_header& h) { h.byte_order = 0x04030201; });
    BOOST_TEST(wd::check_image(image, 42).error() == wd::errc::incompatible_document);
    // The four reserved bytes are kept for a future version: an image that sets one is refused.
    write([](wd::image_header& h) { h.reserved_4 = 1; });
    BOOST_TEST(wd::check_image(image, 42).error() == wd::errc::incompatible_document);
    write([](wd::image_header& h) { h.reserved_4 = 0x80000000U; });
    BOOST_TEST(wd::check_image(image, 42).error() == wd::errc::incompatible_document);
    write([](wd::image_header& h) { h.version = wd::format_version + 1; });
    BOOST_TEST(wd::check_image(image, 42).error() == wd::errc::incompatible_document);
    // The reserved bytes sit where the Boost version sat: the other fields do not move.
    static_assert(offsetof(wd::image_header, reserved_4) == 16);
    static_assert(offsetof(wd::image_header, layout) == 20);
    write([](wd::image_header&) {});
    BOOST_TEST(wd::check_image(image, 43).error() == wd::errc::incompatible_document);
}

// An image of another byte order or format version is told apart before its size is trusted: the
// size such an image states is read in another order or means something else.
void a_foreign_image_is_incompatible() {
    alignas(16) std::array<std::byte, 256> image{};
    wd::image_header h{};
    h.magic = wd::image_magic;
    h.version = wd::format_version;
    h.byte_order = 0x04030201;
    h.reserved_4 = 0;
    h.layout = 42;
    h.size = std::uint64_t{256} << 56U;
    std::memcpy(image.data(), &h, sizeof h);
    BOOST_TEST(wd::check_image(image, 42).error() == wd::errc::incompatible_document);
    h.byte_order = wd::byte_order_mark;
    h.version = wd::format_version + 1;
    std::memcpy(image.data(), &h, sizeof h);
    BOOST_TEST(wd::check_image(image, 42).error() == wd::errc::incompatible_document);
    // An image at an odd address is read as well: the header is copied, never cast.
    h.version = wd::format_version;
    h.size = 128;
    alignas(16) std::array<std::byte, 257> shifted{};
    std::memcpy(shifted.data() + 1, &h, sizeof h);
    BOOST_TEST(wd::check_image(std::span(shifted).subspan(1, 128), 42).has_value());
}

}  // namespace

int main() {
    static_assert(sizeof(wd::image_header) == 64);
    strings_are_exact_and_zeroed_when_freed();
    a_long_secret_leaves_no_byte();
    an_empty_string_allocates_nothing();
    strings_read_as_code_units();
    a_full_heap_says_so_and_grows();
    growth_takes_what_is_asked();
    an_owned_region_stops_at_its_limit();
    doubling_stops_at_the_limit();
    an_owned_region_needs_room_for_its_heap();
    an_external_region_never_grows();
    an_external_empty_region();
    probe_answers_without_allocating();
    probe_asks_for_all_at_once();
    probes_leave_the_same_bytes_at_any_address();
    the_same_operations_give_the_same_bytes_at_any_address();
    an_owned_copy_reads_the_same();
    a_region_moves();
    shrink_to();
    the_allocator_zeroes();
    headers_are_checked();
    a_foreign_image_is_incompatible();
    return boost::report_errors();
}
