// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Compact images: a document of equal content gives byte-identical bytes whatever edits made it
// (parsed, built by sets, erases and splices, values replaced, an insertion that failed for want
// of room); compacting twice changes nothing; the text, the order and the equality are kept; the
// copy opens, seals and verifies; compact_size is the exact size of the copy and shrink_to_fit has
// nothing left to take from it; a document that lost most of its content is smaller compacted than
// shrunk; the copy goes into a caller's buffer or says it does not fit; empty documents, scalars
// and a moved-from document.

#include <webcpp/document/document.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/stringify.hpp>

#include <boost/core/lightweight_test.hpp>

#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wd = webcpp::document;

namespace {

/** Default traits whose objects keep their keys in insertion order. */
struct insertion_traits : wd::default_traits {
    using object = wd::insertion_order;
};

/** Default traits whose objects sort their keys. */
struct sorted_traits : wd::default_traits {
    using object = wd::sorted;
};

constexpr std::size_t kibibyte = 1024;

/** JavaScript's traits: UTF-16 text. */
using utf16_traits = wd::js_traits;

// Exactly `size` bytes at a 16-aligned address, `offset` bytes past it, filled with `fill`, so that
// a sanitizer sees a read or a write past them.
class exact_buffer {
public:
    explicit exact_buffer(std::size_t size, std::byte fill = std::byte{0}, std::size_t offset = 0)
        : raw_(static_cast<std::byte*>(::operator new(size + offset, std::align_val_t{16}))),
          bytes_(raw_ + offset, size),
          total_(size + offset) {
        std::fill_n(raw_, total_, fill);
    }

    exact_buffer(const exact_buffer&) = delete;
    exact_buffer& operator=(const exact_buffer&) = delete;
    exact_buffer(exact_buffer&&) = delete;
    exact_buffer& operator=(exact_buffer&&) = delete;

    ~exact_buffer() { ::operator delete(raw_, std::align_val_t{16}); }

    [[nodiscard]] std::span<std::byte> bytes() const { return bytes_; }

private:
    std::byte* raw_;
    std::span<std::byte> bytes_;
    std::size_t total_;
};

template <class Document>
concept compacts_in_place = requires(Document& d) { d.compact(); };

static_assert(compacts_in_place<wd::document>);
static_assert(!compacts_in_place<wd::basic_document<wd::default_traits, wd::external>>);

template <class Traits>
using owned_document = wd::basic_document<Traits, wd::owned>;

template <class Traits>
owned_document<Traits> make_document() {
    auto made = owned_document<Traits>::create();
    webcpp::test::require(BOOST_TEST(made.has_value()));
    return std::move(*made);
}

template <class Traits>
owned_document<Traits> parsed(std::string_view text) {
    auto made = wd::parse<Traits>(text);
    webcpp::test::require(BOOST_TEST(made.has_value()));
    return std::move(*made);
}

template <class Traits, class Storage>
std::vector<std::byte> bytes_of(const wd::basic_document<Traits, Storage>& doc) {
    const std::span<const std::byte> image = doc.image();
    return {image.begin(), image.end()};
}

template <class Traits, class Storage>
bool identical(const wd::basic_document<Traits, Storage>& a,
               const wd::basic_document<Traits, Storage>& b) {
    return std::ranges::equal(a.image(), b.image());
}

// The content every document below ends with, as JSON text: the keys in the order the parser sees.
constexpr std::string_view content =
    R"({"a":1,"b":"two","c":[10,20,30,{"x":true,"y":null}],"d":{"e":"hello","f":[]},"5":"idx","last":"z"})";

// The same content, reached by a different history: values set and replaced by longer and shorter
// strings, keys created and erased, an array grown and spliced, a subtree set and set again.
template <class Traits>
owned_document<Traits> built_by_edits() {
    owned_document<Traits> doc = make_document<Traits>();
    auto root = doc.root();
    webcpp::test::require(BOOST_TEST(root.set(wd::object{}).has_value()));
    webcpp::test::require(BOOST_TEST(doc["a"].set(std::string(300, 'a')).has_value()));
    webcpp::test::require(BOOST_TEST(doc["b"].set(std::string(500, 'b')).has_value()));
    webcpp::test::require(BOOST_TEST(doc["tmp1"].set(wd::array{1, 2, 3, "x"}).has_value()));
    webcpp::test::require(
        BOOST_TEST(doc["c"].set(wd::array{"filler", "filler", 10, 20, "more"}).has_value()));
    webcpp::test::require(BOOST_TEST(doc["tmp2"].set(std::string(1000, 't')).has_value()));
    webcpp::test::require(BOOST_TEST(doc["d"].set(wd::object{{"e", "hi"}}).has_value()));
    webcpp::test::require(BOOST_TEST(root.erase("tmp1").has_value()));
    webcpp::test::require(BOOST_TEST(doc["5"].set(std::string(64, 'i')).has_value()));
    webcpp::test::require(BOOST_TEST(doc["last"].set(wd::array{1, 2, 3}).has_value()));
    webcpp::test::require(BOOST_TEST(root.erase("tmp2").has_value()));

    // a: a long string, then the number; b: shorter; c: spliced to its shape; d: set again.
    webcpp::test::require(BOOST_TEST(doc["a"].set(1).has_value()));
    webcpp::test::require(BOOST_TEST(doc["b"].set("two").has_value()));
    auto c = doc["c"];
    webcpp::test::require(BOOST_TEST(c.splice(0, 2).has_value()));
    webcpp::test::require(BOOST_TEST(c.splice(2).has_value()));
    webcpp::test::require(BOOST_TEST(c.push(30, wd::object{{"x", false}, {"z", 1}}).has_value()));
    webcpp::test::require(BOOST_TEST(doc["c"][3]["x"].set(true).has_value()));
    webcpp::test::require(BOOST_TEST(doc["c"][3].erase("z").has_value()));
    webcpp::test::require(BOOST_TEST(doc["c"][3]["y"].set(nullptr).has_value()));
    webcpp::test::require(
        BOOST_TEST(doc["d"].set(wd::object{{"e", "hello"}, {"f", 7}}).has_value()));
    webcpp::test::require(BOOST_TEST(doc["d"]["f"].set(wd::array{}).has_value()));
    webcpp::test::require(BOOST_TEST(doc["5"].set("idx").has_value()));
    webcpp::test::require(BOOST_TEST(doc["last"].set("z").has_value()));
    return doc;
}

// The same content again, made in a buffer too small for one of its edits: an insertion that
// fails for want of room leaves the heap's bookkeeping in free memory. Compacted into `out`.
template <class Traits>
owned_document<Traits> built_after_a_failed_insertion() {
    const exact_buffer buffer(6 * kibibyte);
    auto formatted = wd::format<Traits>(buffer.bytes());
    webcpp::test::require(BOOST_TEST(formatted.has_value()));
    auto& doc = *formatted;
    auto root = doc.root();
    webcpp::test::require(BOOST_TEST(root.set(wd::object{{"a", 1}}).has_value()));
    BOOST_TEST(doc["big"].set(std::string(10 * kibibyte, 'x')).error() == wd::errc::capacity);
    webcpp::test::require(BOOST_TEST(doc["b"].set("two").has_value()));
    BOOST_TEST(doc["c"].set(std::string(8 * kibibyte, 'y')).error() == wd::errc::capacity);
    webcpp::test::require(BOOST_TEST(doc["c"].set(wd::array{10, 20, 30}).has_value()));
    webcpp::test::require(BOOST_TEST(doc["c"].push(wd::object{{"x", true}, {"y", nullptr}})));
    webcpp::test::require(BOOST_TEST(doc["d"].set(wd::object{{"e", "hello"}, {"f", wd::array{}}})));
    webcpp::test::require(BOOST_TEST(doc["5"].set("idx").has_value()));
    webcpp::test::require(BOOST_TEST(doc["last"].set("z").has_value()));
    auto copy = wd::from_image<Traits>(doc.image());
    webcpp::test::require(BOOST_TEST(copy.has_value()));
    return std::move(*copy);
}

template <class Traits>
void equal_content_gives_equal_bytes(const char* name) {
    std::printf("equal content, %s\n", name);
    owned_document<Traits> a = parsed<Traits>(content);
    owned_document<Traits> b = built_by_edits<Traits>();
    owned_document<Traits> c = built_after_a_failed_insertion<Traits>();
    BOOST_TEST(a == b);
    BOOST_TEST(a == c);
    BOOST_TEST_EQ(a.stringify(), b.stringify());
    // The histories left their marks: the images are not the same before.
    BOOST_TEST(!identical(a, b));

    const std::string text = a.stringify();
    BOOST_TEST(a.compact().has_value());
    BOOST_TEST(b.compact().has_value());
    BOOST_TEST(c.compact().has_value());
    BOOST_TEST(identical(a, b));
    BOOST_TEST(identical(a, c));
    BOOST_TEST_EQ(a.stringify(), text);
    BOOST_TEST_EQ(b.stringify(), text);
    BOOST_TEST_EQ(c.stringify(), text);
    BOOST_TEST_EQ(a.capacity(), a.image().size());

    // A document built in a buffer of a caller's goes through compact_into, to the same bytes.
    const exact_buffer buffer(8 * kibibyte);
    auto formatted = wd::format<Traits>(buffer.bytes());
    webcpp::test::require(BOOST_TEST(formatted.has_value()));
    BOOST_TEST(formatted->root().set(wd::object{{"a", 1}, {"junk", std::string(900, 'j')}}));
    BOOST_TEST(formatted->root().erase("junk").has_value());
    BOOST_TEST(formatted->root().set(wd::object{{"a", 1}}).has_value());
    BOOST_TEST(formatted->root().assign_from(b.root()).has_value());
    const exact_buffer out(8 * kibibyte, std::byte{0x5a});
    auto into = formatted->compact_into(out.bytes());
    if (BOOST_TEST(into.has_value())) {
        BOOST_TEST(std::ranges::equal(into->image(), a.image()));
        BOOST_TEST_EQ(into->image().size(), formatted->compact_size());
        BOOST_TEST_EQ(into->stringify(), text);
    }
}

template <class Traits>
void idempotence_and_size(const char* name) {
    std::printf("idempotence and size, %s\n", name);
    owned_document<Traits> doc = built_by_edits<Traits>();
    const std::uint64_t promised = doc.compact_size();
    BOOST_TEST(doc.compact().has_value());
    BOOST_TEST_EQ(doc.image().size(), promised);
    BOOST_TEST_EQ(doc.capacity(), promised);
    BOOST_TEST_EQ(doc.compact_size(), promised);
    const std::vector<std::byte> once = bytes_of(doc);

    BOOST_TEST(doc.compact().has_value());
    BOOST_TEST(std::ranges::equal(once, doc.image()));

    // Nothing is left for shrink_to_fit to take.
    BOOST_TEST(doc.shrink_to_fit().has_value());
    BOOST_TEST(std::ranges::equal(once, doc.image()));

    // The image is an image: it opens in place and as a copy, seals and verifies.
    BOOST_TEST(!doc.sealed());
    BOOST_TEST(doc.seal().has_value());
    BOOST_TEST(doc.sealed());
    auto from = wd::from_image<Traits>(doc.image());
    if (BOOST_TEST(from.has_value())) {
        BOOST_TEST(*from == doc);
        BOOST_TEST(from->sealed());
    }
    const exact_buffer copy(doc.image().size());
    std::ranges::copy(doc.image(), copy.bytes().begin());
    auto opened = wd::open<Traits>(copy.bytes());
    if (BOOST_TEST(opened.has_value())) {
        BOOST_TEST_EQ(opened->stringify(), doc.stringify());
    }

    // A sealed document comes out of compact unsealed.
    BOOST_TEST(doc.compact().has_value());
    BOOST_TEST(!doc.sealed());
    BOOST_TEST(std::ranges::equal(once, doc.image()));

    // It still grows, and its handles work after the swap.
    BOOST_TEST(doc["new"].set(std::string(10 * kibibyte, 'n')).has_value());
    BOOST_TEST_EQ(doc["b"].template get<std::string_view>().value(), "two");
    BOOST_TEST(doc["new"].template get<std::string_view>().value() ==
               std::string(10 * kibibyte, 'n'));
}

// A xorshift generator: the same documents on every target.
class random_numbers {
public:
    std::uint64_t next() {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 7U;
        state_ ^= state_ << 17U;
        return state_;
    }

    std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }

private:
    std::uint64_t state_ = 0x9e3779b97f4a7c15ULL;
};

// Sets `key` in `doc` to a value of a random kind, which holds `text` where the kind has a string.
template <class Traits>
void set_random_value(owned_document<Traits>& doc, random_numbers& random, const std::string& key,
                      const std::string& text) {
    switch (random.below(5)) {
        case 0: BOOST_TEST(doc[key].set(text).has_value()); break;
        case 1: BOOST_TEST(doc[key].set(wd::array{text, 1, nullptr}).has_value()); break;
        case 2: BOOST_TEST(doc[key].set(wd::object{{text, true}, {"n", 1}}).has_value()); break;
        case 3:
            BOOST_TEST(doc[key].set(static_cast<std::int64_t>(random.below(100000))).has_value());
            break;
        default: BOOST_TEST(doc[key].set(nullptr).has_value()); break;
    }
}

// An object of a dozen members, each set to a value of a random kind and some erased again, with
// keys and texts of random lengths, the texts longer from the round 20 on.
template <class Traits>
owned_document<Traits> random_document(random_numbers& random, std::size_t round) {
    owned_document<Traits> doc = make_document<Traits>();
    auto root = doc.root();
    BOOST_TEST(root.set(wd::object{}).has_value());
    for (std::size_t i = 0; i < 12; ++i) {
        const std::string key = std::string(random.below(40), 'k') + std::to_string(i);
        const std::size_t length = random.below(round < 20 ? 40 : 300);
        set_random_value(doc, random, key, std::string(length, 'v'));
        if (random.below(4) == 0) {
            BOOST_TEST(root.erase(key).has_value());
        }
    }
    return doc;
}

// compact_size is the exact size of the copy, for texts of every length around the heap's
// alignment and nodes of every kind.
template <class Traits>
void the_size_is_exact(const char* name) {
    std::printf("exact size, %s\n", name);
    random_numbers random;
    for (std::size_t round = 0; round < 40; ++round) {
        owned_document<Traits> doc = random_document<Traits>(random, round);
        const std::string text = doc.stringify();
        const std::uint64_t promised = doc.compact_size();
        BOOST_TEST(doc.compact().has_value());
        BOOST_TEST_EQ(doc.image().size(), promised);
        BOOST_TEST_EQ(doc.stringify(), text);
        const std::vector<std::byte> once = bytes_of(doc);
        BOOST_TEST(doc.shrink_to_fit().has_value());
        BOOST_TEST(std::ranges::equal(once, doc.image()));
    }
}

// A small document with nine in ten of its members erased.
void a_small_document_that_lost_most() {
    owned_document<wd::default_traits> doc = make_document<wd::default_traits>();
    auto root = doc.root();
    BOOST_TEST(root.set(wd::object{}).has_value());
    for (std::size_t i = 0; i < 60; ++i) {
        BOOST_TEST(doc["k" + std::to_string(i)].set(std::string(40, 'x')).has_value());
    }
    for (std::size_t i = 0; i < 60; ++i) {
        if (i % 10 != 0) {
            BOOST_TEST(root.erase("k" + std::to_string(i)).has_value());
        }
    }
    auto shrunk = doc.clone();
    webcpp::test::require(BOOST_TEST(shrunk.has_value()));
    BOOST_TEST(shrunk->shrink_to_fit().has_value());
    const std::string text = doc.stringify();
    BOOST_TEST(doc.compact().has_value());
    std::printf("small document: shrink_to_fit %zu bytes, compact %zu bytes\n",
                static_cast<std::size_t>(shrunk->image().size()),
                static_cast<std::size_t>(doc.image().size()));
    BOOST_TEST_LT(doc.image().size(), shrunk->image().size());
    BOOST_TEST_EQ(doc.stringify(), text);
}

// The text of an object of a few thousand members, each an object of a number, a string and an
// array.
std::string a_few_thousand_nodes() {
    std::string text = "{";
    for (std::size_t i = 0; i < 1500; ++i) {
        const std::string number = std::to_string(i);
        text += (i == 0 ? "" : ",");
        text += R"("member)";
        text += number;
        text += R"(":{"n":)";
        text += number;
        text += R"(,"s":"text of )";
        text += number;
        text += R"(","l":[1,2]})";
    }
    text += '}';
    return text;
}

// A few thousand nodes, nine in ten erased.
void a_large_document_that_lost_most() {
    owned_document<wd::default_traits> doc = parsed<wd::default_traits>(a_few_thousand_nodes());
    for (std::size_t i = 0; i < 1500; ++i) {
        if (i % 10 != 3) {
            BOOST_TEST(doc.root().erase("member" + std::to_string(i)).value());
        }
    }
    const std::uint64_t before = doc.image().size();
    auto shrunk = doc.clone();
    webcpp::test::require(BOOST_TEST(shrunk.has_value()));
    BOOST_TEST(shrunk->shrink_to_fit().has_value());
    const std::string kept = doc.stringify();
    BOOST_TEST(doc.compact().has_value());
    std::printf("large document: image %zu, shrink_to_fit %zu bytes, compact %zu bytes\n",
                static_cast<std::size_t>(before), static_cast<std::size_t>(shrunk->image().size()),
                static_cast<std::size_t>(doc.image().size()));
    BOOST_TEST_LT(doc.image().size(), shrunk->image().size());
    BOOST_TEST_EQ(doc.stringify(), kept);
    BOOST_TEST_EQ(doc.root().size(), 150U);

    // A document parsed whole is already near compact: its order is the parser's, and a copy of
    // the same parse in the same order is the same bytes.
    owned_document<wd::default_traits> twice = parsed<wd::default_traits>(kept);
    BOOST_TEST(twice.compact().has_value());
    BOOST_TEST(identical(twice, doc));
}

// A document that lost most of its content: compact takes the holes shrink_to_fit leaves.
void smaller_than_shrink_to_fit() {
    a_small_document_that_lost_most();
    a_large_document_that_lost_most();
}

// Empty documents, scalars at the root, and a deep nesting.
template <class Traits>
void roots() {
    const std::array<std::string_view, 11> texts{
        "null",      "true", "0",  "-12",         "1.5",     R"("")",
        R"("text")", "{}",   "[]", R"({"a":[]})", "[[],{}]",
    };
    for (const std::string_view text : texts) {
        owned_document<Traits> parsed_doc = parsed<Traits>(text);
        owned_document<Traits> edited = make_document<Traits>();
        BOOST_TEST(
            edited.root().set(wd::array{std::string(100, 'z'), wd::object{{"q", 1}}}).has_value());
        BOOST_TEST(edited.root().set(parsed_doc.root()).has_value());
        BOOST_TEST(edited.compact().has_value());
        BOOST_TEST(parsed_doc.compact().has_value());
        BOOST_TEST(identical(parsed_doc, edited));
        BOOST_TEST_EQ(parsed_doc.stringify(), std::string(text));
        BOOST_TEST_EQ(parsed_doc.capacity(), parsed_doc.compact_size());
    }

    // A new document, and a root that holds undefined.
    owned_document<Traits> fresh = make_document<Traits>();
    const std::uint64_t promised = fresh.compact_size();
    BOOST_TEST(fresh.compact().has_value());
    BOOST_TEST_EQ(fresh.image().size(), promised);
    BOOST_TEST_EQ(fresh.stringify(), "null");
    owned_document<Traits> nothing = make_document<Traits>();
    auto nothing_root = nothing.root();
    BOOST_TEST(nothing_root.set(wd::object{{"a", 1}}).has_value());
    BOOST_TEST(nothing_root.set(wd::undefined).has_value());
    BOOST_TEST(nothing.compact().has_value());
    BOOST_TEST_EQ(nothing.stringify(), "");
    BOOST_TEST_EQ(nothing.capacity(), nothing.compact_size());

    // A deep nesting: the walk keeps a stack of its own.
    constexpr std::size_t depth = 100;
    std::string deep(depth, '[');
    deep += '1';
    deep.append(depth, ']');
    owned_document<Traits> nested = parsed<Traits>(deep);
    BOOST_TEST(nested.compact().has_value());
    BOOST_TEST_EQ(nested.stringify(), deep);
}

// The buffer that is exactly enough: the copy is where the buffer is, full, and the bytes owned
// documents compact to.
void into_an_exact_buffer(const owned_document<wd::default_traits>& doc, std::uint64_t size,
                          const std::vector<std::byte>& before) {
    const exact_buffer exact(size);
    auto into = doc.compact_into(exact.bytes());
    if (BOOST_TEST(into.has_value())) {
        BOOST_TEST_EQ(into->image().size(), size);
        BOOST_TEST_EQ(into->capacity(), size);
        BOOST_TEST_EQ(into->stringify(), doc.stringify());
        BOOST_TEST(std::ranges::equal(into->image(), exact.bytes()));
        // The document is where the buffer is, and is full.
        BOOST_TEST(into->image().data() == exact.bytes().data());
        BOOST_TEST(into->root().set(std::string(1000, 'f')).error() == wd::errc::capacity);
        // Its image is the copy owned documents compact to.
        owned_document<wd::default_traits> compacted = built_by_edits<wd::default_traits>();
        BOOST_TEST(compacted.compact().has_value());
        BOOST_TEST(std::ranges::equal(into->image(), compacted.image()));
    }
    // The source is as it was.
    BOOST_TEST(std::ranges::equal(before, doc.image()));
}

// A buffer one block short: refused before anything is written.
void into_a_tight_buffer(const owned_document<wd::default_traits>& doc, std::uint64_t size,
                         const std::vector<std::byte>& before) {
    const exact_buffer tight(size - 16, std::byte{0xab});
    BOOST_TEST(doc.compact_into(tight.bytes()).error() == wd::errc::capacity);
    BOOST_TEST(
        std::ranges::all_of(tight.bytes(), [](std::byte b) { return b == std::byte{0xab}; }));
    BOOST_TEST(std::ranges::equal(before, doc.image()));
}

// A larger buffer: the image is first, the rest is zero.
void into_a_large_buffer(const owned_document<wd::default_traits>& doc, std::uint64_t size) {
    const exact_buffer large(size + 4096, std::byte{0xcd});
    auto into = doc.compact_into(large.bytes());
    if (BOOST_TEST(into.has_value())) {
        BOOST_TEST_EQ(into->image().size(), size);
        BOOST_TEST(std::ranges::all_of(large.bytes().subspan(size),
                                       [](std::byte b) { return b == std::byte{0}; }));
        BOOST_TEST(wd::open<wd::default_traits>(large.bytes()).has_value());
    }
}

// An external document, here one in a buffer, is a source too.
void from_an_external_document() {
    const exact_buffer source(16 * kibibyte);
    auto formatted = wd::format<wd::default_traits>(source.bytes());
    webcpp::test::require(BOOST_TEST(formatted.has_value()));
    BOOST_TEST(formatted->root().set(wd::object{{"a", wd::array{1, 2, 3}}, {"b", "text"}}));
    BOOST_TEST(formatted->root().erase("a").has_value());
    const exact_buffer out(formatted->compact_size());
    auto into = formatted->compact_into(out.bytes());
    if (BOOST_TEST(into.has_value())) {
        BOOST_TEST_EQ(into->stringify(), R"({"b":"text"})");
    }
}

// compact_into: the buffer that is exactly enough, one that is not, one past the image, one that is
// not 16-aligned or overlaps the image, and the source left as it was.
void into_a_buffer() {
    const owned_document<wd::default_traits> doc = built_by_edits<wd::default_traits>();
    const std::uint64_t size = doc.compact_size();
    const std::vector<std::byte> before = bytes_of(doc);

    into_an_exact_buffer(doc, size, before);
    into_a_tight_buffer(doc, size, before);
    into_a_large_buffer(doc, size);
    {
        // Not 16-aligned.
        const exact_buffer misaligned(size + 16, std::byte{0}, 8);
        BOOST_TEST(doc.compact_into(misaligned.bytes()).error() == wd::errc::capacity);
    }
    {
        // Over its own image.
        const std::span<const std::byte> image = doc.image();
        const std::span<std::byte> over(const_cast<std::byte*>(image.data()), image.size());
        BOOST_TEST(doc.compact_into(over).error() == wd::errc::type);
        BOOST_TEST(std::ranges::equal(before, doc.image()));
    }
    from_an_external_document();
}

// The limit an owned document grows to is kept, and the document grows again after the swap.
void the_limit_is_kept() {
    constexpr std::uint64_t limit = 24 * kibibyte;
    auto made = wd::document::create(limit);
    webcpp::test::require(BOOST_TEST(made.has_value()));
    wd::document doc = std::move(*made);
    auto root = doc.root();
    BOOST_TEST(root.set(wd::object{{"keep", "me"}}).has_value());
    for (std::size_t i = 0; i < 8; ++i) {
        BOOST_TEST(doc["f" + std::to_string(i)].set(std::string(1000, 'f')).has_value());
    }
    for (std::size_t i = 0; i < 8; ++i) {
        BOOST_TEST(root.erase("f" + std::to_string(i)).value());
    }
    BOOST_TEST(doc.compact().has_value());
    BOOST_TEST_LT(doc.capacity(), limit);
    BOOST_TEST(doc["more"].set(std::string(8 * kibibyte, 'm')).has_value());
    BOOST_TEST(doc["past"].set(std::string(limit, 'p')).error() == wd::errc::capacity);
    BOOST_TEST_EQ(doc["keep"].get<std::string_view>().value(), "me");
}

// A document that was moved from has no image.
void moved_from() {
    owned_document<wd::default_traits> doc = built_by_edits<wd::default_traits>();
    owned_document<wd::default_traits> taken = std::move(doc);
    BOOST_TEST_EQ(doc.compact_size(), 0U);  // NOLINT(bugprone-use-after-move)
    BOOST_TEST(doc.compact().error() == wd::errc::type);
    const exact_buffer out(16 * kibibyte);
    BOOST_TEST(doc.compact_into(out.bytes()).error() == wd::errc::type);
    BOOST_TEST(taken.compact().has_value());
}

}  // namespace

int main() {
    equal_content_gives_equal_bytes<wd::default_traits>("default traits");
    equal_content_gives_equal_bytes<insertion_traits>("insertion order");
    equal_content_gives_equal_bytes<sorted_traits>("sorted");
    equal_content_gives_equal_bytes<utf16_traits>("UTF-16");
    idempotence_and_size<wd::default_traits>("default traits");
    idempotence_and_size<sorted_traits>("sorted");
    the_size_is_exact<wd::default_traits>("default traits");
    the_size_is_exact<utf16_traits>("UTF-16");
    the_size_is_exact<sorted_traits>("sorted");
    smaller_than_shrink_to_fit();
    roots<wd::default_traits>();
    roots<sorted_traits>();
    into_a_buffer();
    the_limit_is_kept();
    moved_from();
    return boost::report_errors();
}
