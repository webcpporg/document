// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A document's image: copied to another address and opened there, in place or as an owned copy
// that grows; formatted into a buffer; refused when it is no image or one of other traits; the same
// bytes for the same operations at any address; no byte left of an erased value; an owned image
// shrunk to its content; an external buffer that is full; a buffer that is not 16-aligned; and a
// header whose container offset lies.

#include <webcpp/document/document.hpp>

#include <boost/core/lightweight_test.hpp>

#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wd = webcpp::document;

namespace {

constexpr std::uint64_t kibibyte = 1024;
constexpr std::uint64_t mebibyte = kibibyte * kibibyte;

// A buffer of `size` bytes at a 16-aligned address, `offset` bytes past it.
class aligned_buffer {
public:
    explicit aligned_buffer(std::size_t size, std::size_t offset = 0) : raw_(size + offset + 16) {
        void* p = raw_.data();
        std::size_t space = raw_.size();
        std::align(16, size + offset, p, space);
        bytes_ = {static_cast<std::byte*>(p) + offset, size};
    }

    [[nodiscard]] std::span<std::byte> bytes() const { return bytes_; }

private:
    std::vector<std::byte> raw_;
    std::span<std::byte> bytes_;
};

// Exactly `size` bytes at a 16-aligned address, so that a sanitizer sees a read past them.
class exact_buffer {
public:
    explicit exact_buffer(std::span<const std::byte> bytes)
        : bytes_(static_cast<std::byte*>(::operator new(bytes.size(), std::align_val_t{16})),
                 bytes.size()) {
        std::ranges::copy(bytes, bytes_.begin());
    }

    exact_buffer(const exact_buffer&) = delete;
    exact_buffer& operator=(const exact_buffer&) = delete;
    exact_buffer(exact_buffer&&) = delete;
    exact_buffer& operator=(exact_buffer&&) = delete;

    ~exact_buffer() { ::operator delete(bytes_.data(), std::align_val_t{16}); }

    [[nodiscard]] std::span<std::byte> bytes() const { return bytes_; }

private:
    std::span<std::byte> bytes_;
};

wd::document make_document() {
    auto made = wd::document::create();
    webcpp::test::require(BOOST_TEST(made.has_value()));
    return std::move(*made);
}

// Whether the bytes hold `needle` anywhere.
bool holds(std::span<const std::byte> bytes, std::string_view needle) {
    const auto found = std::ranges::search(bytes, std::as_bytes(std::span(needle)));
    return !found.empty();
}

// Whether two documents of any storage hold equal values.
template <class A, class B>
bool same(const A& a, const B& b) {
    const auto x = a.clone();
    const auto y = b.clone();
    return x.has_value() && y.has_value() && *x == *y;
}

void an_owned_copy_grows(const wd::document& doc);

void an_image_moves() {
    wd::document doc = make_document();
    if (!BOOST_TEST(doc.root().set(wd::object{{"k", "value"}, {"n", wd::array{1, 2, 3}}}))) {
        return;
    }
    const std::span<const std::byte> image = doc.image();
    BOOST_TEST_EQ(image.size() % 16, 0U);

    // Copied to another address, and to a larger buffer: the image is what its header says.
    const aligned_buffer copy(image.size() + 4096, 48);
    std::ranges::copy(image, copy.bytes().begin());
    auto opened = wd::open<wd::default_traits>(copy.bytes());
    if (!BOOST_TEST(opened.has_value())) {
        return;
    }
    BOOST_TEST(same(*opened, doc));
    BOOST_TEST_EQ(opened->image().size(), image.size());
    BOOST_TEST_EQ((*opened)["k"].get<std::string_view>().value(), "value");
    BOOST_TEST_EQ((*opened)["n"][2].get<std::int64_t>().value(), 3);

    // Opened in place, it is written in place, and opened again it has the change.
    BOOST_TEST((*opened)["more"].set(true).has_value());
    auto again = wd::open(copy.bytes());
    if (BOOST_TEST(again.has_value())) {
        BOOST_TEST((*again)["more"].get<bool>().value());
    }
    BOOST_TEST(!doc.root().contains("more"));
    an_owned_copy_grows(doc);
}

// An owned copy of an image grows, its own image moving while the original stays, up to the
// max_bytes it was given.
void an_owned_copy_grows(const wd::document& doc) {
    const std::span<const std::byte> image = doc.image();
    auto owned = wd::from_image(doc.image());
    if (!BOOST_TEST(owned.has_value())) {
        return;
    }
    BOOST_TEST(*owned == doc);
    for (std::uint64_t i = 0; i < 2000; ++i) {
        if (!BOOST_TEST((*owned)["n"][i + 3].set(std::string(64, 'g')).has_value())) {
            return;
        }
    }
    BOOST_TEST_GT(owned->image().size(), 16 * image.size());
    BOOST_TEST_EQ((*owned)["n"].length().value(), 2003U);
    BOOST_TEST_EQ(doc["n"].length().value(), 3U);
    BOOST_TEST_EQ((*owned)["k"].get<std::string_view>().value(), "value");

    // The owned copy may not grow past its max_bytes.
    auto small = wd::from_image(doc.image(), doc.image().size());
    if (BOOST_TEST(small.has_value())) {
        BOOST_TEST((*small)["big"].set(std::string(doc.image().size(), 'b')).error() ==
                   wd::errc::capacity);
        BOOST_TEST(*small == doc);
    }
    BOOST_TEST(wd::from_image(doc.image(), doc.image().size() - 16).error() == wd::errc::capacity);
}

void format_and_open() {
    const aligned_buffer buffer(64 * kibibyte);
    auto formatted = wd::format(buffer.bytes());
    if (!BOOST_TEST(formatted.has_value())) {
        return;
    }
    BOOST_TEST(formatted->root().is_null());
    BOOST_TEST_EQ(formatted->image().size(), 64 * kibibyte);
    BOOST_TEST(formatted->image().data() == buffer.bytes().data());
    BOOST_TEST(formatted->root().set(wd::array{"a"}).has_value());
    auto reopened = wd::open(buffer.bytes());
    if (BOOST_TEST(reopened.has_value())) {
        BOOST_TEST_EQ((*reopened)[0].get<std::string_view>().value(), "a");
    }

    const aligned_buffer blank(64 * kibibyte);
    BOOST_TEST(wd::open(blank.bytes()).error() == wd::errc::not_a_document);
    BOOST_TEST(wd::from_image(blank.bytes()).error() == wd::errc::not_a_document);
    BOOST_TEST(wd::open(buffer.bytes().first(63)).error() == wd::errc::not_a_document);
    BOOST_TEST(wd::open(buffer.bytes().first(1024)).error() == wd::errc::not_a_document);
    BOOST_TEST(wd::from_image(buffer.bytes().first(1024)).error() == wd::errc::not_a_document);
    BOOST_TEST(wd::open<wd::js_traits>(buffer.bytes()).error() == wd::errc::incompatible_document);
    BOOST_TEST(wd::from_image<wd::js_traits>(buffer.bytes()).error() ==
               wd::errc::incompatible_document);

    // A buffer too small for an empty document.
    const aligned_buffer tiny(256);
    BOOST_TEST(wd::format(tiny.bytes()).error() == wd::errc::capacity);
    BOOST_TEST(wd::document::create(256).error() == wd::errc::capacity);
}

// An image whose reserved header bytes are not zero, or of another format version, is refused by
// open and from_image alike; the same image with them as written is read.
void reserved_bytes_and_versions_are_refused() {
    wd::document doc = make_document();
    wd::value root = doc.root();
    webcpp::test::require(BOOST_TEST(root.set(wd::object{{"k", "v"}}).has_value()));
    const aligned_buffer buffer(doc.image().size());
    std::ranges::copy(doc.image(), buffer.bytes().begin());
    BOOST_TEST(wd::open(buffer.bytes()).has_value());

    wd::image_header header{};
    std::memcpy(&header, buffer.bytes().data(), sizeof header);
    BOOST_TEST_EQ(header.reserved_4, 0U);
    const auto refused_with = [&](auto&& change) {
        wd::image_header edited = header;
        change(edited);
        std::memcpy(buffer.bytes().data(), &edited, sizeof edited);
        BOOST_TEST(wd::open(buffer.bytes()).error() == wd::errc::incompatible_document);
        BOOST_TEST(wd::from_image(buffer.bytes()).error() == wd::errc::incompatible_document);
    };
    refused_with([](wd::image_header& h) { h.reserved_4 = 1; });
    refused_with([](wd::image_header& h) { h.reserved_4 = 0xffffffffU; });
    refused_with([](wd::image_header& h) { h.version = wd::format_version + 1; });
    std::memcpy(buffer.bytes().data(), &header, sizeof header);
    BOOST_TEST(wd::open(buffer.bytes()).has_value());
}

// The same calls, with growth, erasure and nesting, on a document of each storage.
template <class Document>
void build(Document& doc) {
    webcpp::test::require(BOOST_TEST(doc.root().set(wd::object()).has_value()));
    for (std::uint64_t i = 0; i < 100; ++i) {
        const std::string key = "k" + std::to_string(i);
        webcpp::test::require(BOOST_TEST(doc[key].set(wd::array{i, key, wd::object{{"i", i}}})));
    }
    for (std::uint64_t i = 0; i < 100; i += 3) {
        webcpp::test::require(BOOST_TEST(doc.root().erase("k" + std::to_string(i)).value()));
    }
    webcpp::test::require(BOOST_TEST(doc["k1"].set(doc["k2"]).has_value()));
    webcpp::test::require(BOOST_TEST(doc["k4"][1].set(std::string(3000, 'q')).has_value()));
}

void the_same_operations_give_the_same_image() {
    wd::document a = make_document();
    wd::document b = make_document();
    build(a);
    build(b);
    BOOST_TEST(a.image().data() != b.image().data());
    BOOST_TEST(std::ranges::equal(a.image(), b.image()));

    const aligned_buffer x(mebibyte);
    const aligned_buffer y(mebibyte, 4096);
    auto in_x = wd::format(x.bytes());
    auto in_y = wd::format(y.bytes());
    if (!BOOST_TEST(in_x.has_value() && in_y.has_value())) {
        return;
    }
    build(*in_x);
    build(*in_y);
    BOOST_TEST(std::ranges::equal(in_x->image(), in_y->image()));
    BOOST_TEST(same(*in_x, a));
}

void an_erased_secret_leaves_no_byte() {
    const std::string secret = "s3cr3t-0123456789-abcdefghijklmnopqrstuvwxyz-ABCDEFGHIJKLMNOPQRS";
    BOOST_TEST_EQ(secret.size(), 64U);
    wd::document doc = make_document();
    wd::value root = doc.root();
    BOOST_TEST(root.set(wd::object{{"user", "ana"}}).has_value());
    BOOST_TEST(doc["password"].set(secret).has_value());
    BOOST_TEST(doc["list"].set(wd::array{secret, wd::object{{secret, secret}}}).has_value());
    BOOST_TEST(doc["copy"].set(doc["list"]).has_value());
    BOOST_TEST(holds(doc.image(), secret));

    BOOST_TEST(root.erase("password").value());
    BOOST_TEST(doc["list"].set(nullptr).has_value());
    BOOST_TEST(doc["copy"].set(wd::undefined).has_value());
    for (std::size_t at = 0; at + 8 <= secret.size(); ++at) {
        BOOST_TEST(!holds(doc.image(), std::string_view(secret).substr(at, 8)));
    }
    BOOST_TEST_EQ(doc["user"].get<std::string_view>().value(), "ana");
}

void shrink_to_fit() {
    wd::document doc = make_document();
    wd::value root = doc.root();
    BOOST_TEST(root.set(wd::object{{"keep", "this"}}).has_value());
    const std::string text(1000, 'f');
    for (std::uint64_t i = 0; doc.image().size() < mebibyte; ++i) {
        if (!BOOST_TEST(doc["f" + std::to_string(i)].set(text).has_value())) {
            return;
        }
    }
    const std::uint64_t grown = doc.image().size();
    const std::vector<std::string> keys = [&doc] {
        std::vector<std::string> out;
        for (const std::string_view k : doc.root().keys()) {
            out.emplace_back(k);
        }
        return out;
    }();
    for (const std::string& key : keys) {
        if (key != "keep") {
            BOOST_TEST(root.erase(key).value());
        }
    }
    const auto before = doc.clone();
    if (!BOOST_TEST(before.has_value())) {
        return;
    }
    BOOST_TEST(doc.shrink_to_fit().has_value());
    BOOST_TEST_LT(doc.image().size(), grown / 16);
    BOOST_TEST(doc == *before);
    BOOST_TEST_EQ(doc.root().size(), 1U);

    // The shrunk image is an image, and grows again.
    auto copy = wd::from_image(doc.image());
    if (BOOST_TEST(copy.has_value())) {
        BOOST_TEST(*copy == doc);
    }
    BOOST_TEST(doc["again"].set(std::string(100 * kibibyte, 'a')).has_value());
    BOOST_TEST_GT(doc.image().size(), 100 * kibibyte);
    BOOST_TEST_EQ(doc["keep"].get<std::string_view>().value(), "this");
}

void an_external_document_says_full() {
    const aligned_buffer buffer(4 * kibibyte);
    auto made = wd::format(buffer.bytes());
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    auto& doc = *made;
    auto root = doc.root();
    BOOST_TEST(root.set(wd::object{{"a", 1}, {"b", wd::array{"x"}}}).has_value());
    const std::vector<std::byte> before(doc.image().begin(), doc.image().begin() + 64);
    const auto content = doc.clone();
    BOOST_TEST(doc["big"].set(std::string(10 * kibibyte, 'b')).error() == wd::errc::capacity);
    BOOST_TEST(doc["b"][5].set(std::string(10 * kibibyte, 'b')).error() == wd::errc::capacity);
    BOOST_TEST(doc.root().set(std::string(10 * kibibyte, 'b')).error() == wd::errc::capacity);
    // The header, and the content, are as they were; the heap's free blocks may differ.
    BOOST_TEST(std::ranges::equal(before, doc.image().first(64)));
    BOOST_TEST_EQ(doc.image().size(), 4 * kibibyte);
    BOOST_TEST(content.has_value() && same(doc, *content));
    BOOST_TEST(!doc.root().contains("big"));
    BOOST_TEST_EQ(doc["b"].length().value(), 1U);
    BOOST_TEST_EQ(doc["a"].get<std::int64_t>().value(), 1);
}

void a_misaligned_buffer() {
    const aligned_buffer misaligned(64 * kibibyte, 8);
    BOOST_TEST(wd::format(misaligned.bytes()).error() == wd::errc::capacity);

    wd::document doc = make_document();
    wd::value root = doc.root();
    BOOST_TEST(root.set(wd::array{1}).has_value());
    const aligned_buffer copy(doc.image().size(), 8);
    std::ranges::copy(doc.image(), copy.bytes().begin());
    BOOST_TEST(wd::open(copy.bytes()).error() == wd::errc::not_a_document);
    // An owned copy is made at an address of its own, so any address will do.
    auto owned = wd::from_image(std::span<const std::byte>(copy.bytes()));
    if (BOOST_TEST(owned.has_value())) {
        BOOST_TEST(*owned == doc);
    }
}

// The container is the heap's first block, at one offset in every image: a header that names
// another, inside the image or not, is refused before anything is read there.
void a_lying_container_offset() {
    wd::document doc = make_document();
    wd::value root = doc.root();
    BOOST_TEST(root.set(wd::object{{"a", wd::array{1, 2, 3}}, {"b", "hello"}}).has_value());
    BOOST_TEST(doc.shrink_to_fit().has_value());
    const std::span<const std::byte> image = doc.image();
    constexpr std::size_t at = offsetof(wd::image_header, container);
    std::uint64_t container = 0;
    std::memcpy(&container, image.data() + at, sizeof container);
    BOOST_TEST_EQ(container, wd::detail::container_offset);

    const exact_buffer intact(image);
    auto opened = wd::open(intact.bytes());
    BOOST_TEST(opened.has_value() && same(*opened, doc));
    const std::array<std::uint64_t, 5> lies{
        std::uint64_t{image.size() - 64},  // the end of the image
        std::uint64_t{64},                 // the heap's own header
        container + 16,
        container - 16,
        std::uint64_t{0},  // the image's header
    };
    for (const std::uint64_t lie : lies) {
        const exact_buffer copy(image);
        std::memcpy(copy.bytes().data() + at, &lie, sizeof lie);
        BOOST_TEST(wd::open(copy.bytes()).error() == wd::errc::not_a_document);
        BOOST_TEST(wd::from_image(std::span<const std::byte>(copy.bytes())).error() ==
                   wd::errc::not_a_document);
    }
}

}  // namespace

int main() {
    an_image_moves();
    format_and_open();
    reserved_bytes_and_versions_are_refused();
    the_same_operations_give_the_same_image();
    an_erased_secret_leaves_no_byte();
    shrink_to_fit();
    an_external_document_says_full();
    a_misaligned_buffer();
    a_lying_container_offset();
    return boost::report_errors();
}
