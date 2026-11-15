// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The writer's working state past its own room in the document's allocator: stringify_to of a
// pmr::document, of a pmr document under js_traits and of an external pmr document under
// base64url, into a std::pmr::string on the document's own arena, at every indent and chunk, a
// document nested 10,000 levels deep among them, makes no call of the global operator new and
// gives back to the arena all it took; the writer allocates the frames of its stack past the 16 of
// its own room and a piece larger than its own 4,096 bytes alone, as often as their rooms double,
// so that a dozen small texts written in the example's 64 KiB arena take none of it; stringify asks
// the document's allocator for nothing, and calls the global operator new exactly as often as its
// std::string grows and its stack's deeper frames need; quote allocates through its output alone;
// and, with exceptions, a resource that fails at any step is errc::capacity for stringify_to, the
// sink not called again, its text a prefix and the document unchanged, while a sink's own
// std::bad_alloc propagates.

#include <webcpp/document.hpp>

#include <boost/core/lightweight_test.hpp>

#include "counting.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wd = webcpp::document;

namespace {

using webcpp::test::arena;
using webcpp::test::global_count;
using webcpp::test::require;

constexpr std::size_t mebibyte = std::size_t{1} << 20U;

/**
 What an arena holds besides a document's image: a monotonic arena never reuses what it took, so
 it holds the writer's stacks, about 512 KB for the deepest document, and the texts' strings, as
 they grew, of every case written in it.
*/
constexpr std::size_t working_room = std::size_t{16} << 20U;

/** The levels of the deepest document: arrays and objects in turn, the innermost empty. */
constexpr std::size_t depth = 10'000;

/** The frames the writer's stack holds at once for it: every level but the innermost, empty. */
constexpr std::uint64_t deep_frames = depth - 1;

/** The arrays nested under the sample's "deep" member, the innermost empty. */
constexpr std::size_t sample_levels = 40;

/** The frames the writer's stack holds at once for the sample: its root and the arrays but one. */
constexpr std::uint64_t sample_frames = 1 + sample_levels - 1;

/** The default traits, but bytes written as base64url. */
struct url_traits : wd::default_traits {
    /** RFC 4648 section 5, no padding. */
    using bytes = wd::base64url;
};

/** A block of an external document's buffer, which is 16-aligned. */
struct alignas(16) block {
    /** Its bytes. */
    std::array<std::byte, 16> bytes{};
};

/**
 Every escape QuoteJSONString writes and characters of two, three and four bytes in UTF-8, U+2028
 among them: 20 bytes.
*/
constexpr std::string_view escaped_run =
    "x\"\\\b\f\n\r\t\x01\x1f/"
    "\xc3\xa9"
    "\xe2\x80\xa8"
    "\xf0\x9f\x98\x80";

/** `count` copies of `run`. */
std::string repeated(std::string_view run, std::size_t count) {
    std::string text;
    for (std::size_t i = 0; i < count; ++i) {
        text += run;
    }
    return text;
}

/** An empty document of `Traits`, in the standard allocator. */
template <class Traits>
wd::basic_document<Traits> make_document() {
    auto made = wd::basic_document<Traits>::create();
    require(BOOST_TEST(made.has_value()));
    return std::move(*made);
}

/**
 Sets `levels` containers from `top` down, each the only element or member of the one before:
 arrays, or with `alternate` arrays at even levels and objects of the key "k" at odd ones; the
 innermost is empty.
*/
template <class Traits>
void nest(wd::basic_value<Traits> top, std::size_t levels, bool alternate) {
    // A handle cannot be assigned without exceptions: the next one is constructed in place.
    std::optional<wd::basic_value<Traits>> at;
    at.emplace(std::move(top));
    for (std::size_t level = 0; level < levels; ++level) {
        const bool object = alternate && level % 2 == 1;
        require(BOOST_TEST((object ? at->set(wd::object()) : at->set(wd::array())).has_value()));
        if (level + 1 < levels) {
            wd::basic_value<Traits> child = object ? (*at)["k"] : (*at)[std::uint64_t{0}];
            at.emplace(std::move(child));
        }
    }
}

/** A document of `Traits` nested @ref depth levels deep, built in the standard allocator. */
template <class Traits>
wd::basic_document<Traits> nested() {
    wd::basic_document<Traits> doc = make_document<Traits>();
    nest(doc.root(), depth, true);
    return doc;
}

/**
 A document of `Traits` that holds what the writer writes: a key of 120 bytes and strings with
 escapes and characters past ASCII, a run of 10,000 bytes, numbers, dates, 301 bytes, empty
 containers, arrays nested @ref sample_levels deep, and under UTF-16 lone surrogates.
*/
template <class Traits>
wd::basic_document<Traits> sample() {
    wd::basic_document<Traits> doc = make_document<Traits>();
    require(BOOST_TEST(doc.root().set(wd::object()).has_value()));
    const std::string key = repeated(escaped_run, 6);
    const std::string text = repeated(escaped_run, 15);
    const std::string run(10000, 'z');
    std::vector<std::byte> bytes(301);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>((i * 7) % 256);
    }
    using std::chrono::milliseconds;
    require(BOOST_TEST(doc[key].set(std::string_view(text)).has_value()));
    require(BOOST_TEST(doc["run"].set(std::string_view(run)).has_value()));
    require(BOOST_TEST(doc["date"].set(wd::date_ms{milliseconds{1791774245678}}).has_value()));
    require(BOOST_TEST(doc["early"].set(wd::date_ms{milliseconds{-62198755200000}}).has_value()));
    require(BOOST_TEST(doc["bytes"].set(std::span<const std::byte>(bytes)).has_value()));
    require(BOOST_TEST(doc["list"]
                           .set(wd::array{1, -1, std::int64_t{1234567890123}, true, nullptr,
                                          "three", wd::literal(wd::array()), wd::object()})
                           .has_value()));
    nest(doc["deep"], sample_levels, false);
    if constexpr (std::same_as<typename Traits::string, wd::utf16>) {
        // A lone low surrogate, a lone high one, a pair, and a high one before a control.
        const std::u16string lone{u'a', 0xDC00, 0xD800, u'b', 0xD83D, 0xDE00, 0xD83D, u'\n'};
        require(BOOST_TEST(doc["lone"].set(std::u16string_view(lone)).has_value()));
    }
    return doc;
}

/** What `stringify` writes of `source` at each of `indents`. */
template <class Document>
std::vector<std::string> texts_of(const Document& source, std::span<const std::uint32_t> indents) {
    std::vector<std::string> texts;
    for (const std::uint32_t indent : indents) {
        texts.push_back(wd::stringify(source, indent));
    }
    return texts;
}

/** Whether two documents' images are the same bytes; it allocates nothing. */
template <class A, class B>
bool same_image(const A& a, const B& b) noexcept {
    return std::ranges::equal(a.image(), b.image());
}

/** A copy of `source` that owns its image, in `resource`. */
template <class Traits>
wd::pmr::basic_document<Traits> owned_copy(const wd::basic_document<Traits>& source,
                                           std::pmr::memory_resource* resource) {
    auto made = wd::from_image<Traits>(source.image(),
                                       std::pmr::polymorphic_allocator<std::byte>(resource));
    require(BOOST_TEST(made.has_value()));
    return std::move(*made);
}

/** A buffer of 16-aligned blocks holding a copy of `source`'s image. */
template <class Traits>
std::vector<block> buffer_of(const wd::basic_document<Traits>& source) {
    std::vector<block> buffer((source.image().size() / sizeof(block)) + 1);
    std::ranges::copy(source.image(), std::as_writable_bytes(std::span(buffer)).begin());
    return buffer;
}

/** An external document over `buffer`, the working state of its operations in `resource`. */
template <class Traits>
wd::basic_document<Traits, wd::pmr::external> external_copy(std::vector<block>& buffer,
                                                            std::pmr::memory_resource* resource) {
    auto opened = wd::open<Traits>(std::as_writable_bytes(std::span(buffer)),
                                   std::pmr::polymorphic_allocator<std::byte>(resource));
    require(BOOST_TEST(opened.has_value()));
    return std::move(*opened);
}

/**
 The allocations of the writer's stack to hold `frames` frames at once: 16 in its own room, then,
 past them, room for 16 frames first and twice the room each time it is full.
*/
constexpr std::uint64_t stack_allocations(std::uint64_t frames) {
    const std::uint64_t deeper = frames > 16 ? frames - 16 : 0;
    std::uint64_t count = 0;
    for (std::uint64_t room = 0; room < deeper; room = room == 0 ? 16 : 2 * room) {
        ++count;
    }
    return count;
}

/**
 The allocations of the piece `stringify_to` holds back, for a text of `size` bytes written in
 pieces smaller than any room: none while it fits its own room of 4096 bytes, then twice that
 room, then twice the room, up to `chunk`, each time it is full.
*/
constexpr std::uint64_t piece_allocations(std::uint64_t chunk, std::uint64_t size) {
    const std::uint64_t most = std::min(chunk, size);
    std::uint64_t count = 0;
    for (std::uint64_t room = 4096; room < most; room = std::min(chunk, 2 * room)) {
        ++count;
    }
    return count;
}

/** An output of the writer that records the size of each piece it is handed. */
class piece_recorder {
public:
    /** A recorder that appends to `sizes`, which outlives it. */
    explicit piece_recorder(std::vector<std::size_t>& sizes) noexcept : sizes_(&sizes) {}

    /** Records the size of `text`. */
    wd::result<void> put(std::string_view text) {
        sizes_->push_back(text.size());
        return {};
    }

private:
    /** The sizes, in the order the pieces came. */
    std::vector<std::size_t>* sizes_;
};

/**
 stringify_to of `doc`, a copy of `source` whose allocator is `a`'s, into a std::pmr::string on `a`
 too, at each of `indents` and in pieces of 1, 7 and 4096 bytes: no call of the global operator
 new, the text of `expected` at that indent, every byte the writer and the string took given back,
 and the document unchanged.
*/
template <class Document, class Source>
void writes_in_its_arena(const Document& doc, const Source& source, arena& a,
                         std::span<const std::uint32_t> indents,
                         const std::vector<std::string>& expected) {
    const std::uint64_t held = a.resource().bytes_in_use();
    for (std::size_t i = 0; i < indents.size(); ++i) {
        for (const std::uint64_t chunk : {1U, 7U, 4096U}) {
            {
                // Its growth is the arena's too: nothing is reserved.
                std::pmr::string out(&a.resource());
                const global_count global;
                const wd::result<void> written = wd::stringify_to(
                    doc, [&out](std::string_view piece) { out.append(piece); }, indents[i], chunk);
                const std::uint64_t news = global.news();
                BOOST_TEST_EQ(news, 0U);
                BOOST_TEST(written.has_value());
                BOOST_TEST(std::string_view(out) == expected[i]);
            }
            BOOST_TEST_EQ(a.resource().bytes_in_use(), held);
            BOOST_TEST(same_image(doc, source));
        }
    }
}

/** @ref writes_in_its_arena for a copy of `source` that owns its image in an arena. */
template <class Traits>
void owned_in_arena(const wd::basic_document<Traits>& source,
                    std::span<const std::uint32_t> indents) {
    const std::vector<std::string> expected = texts_of(source, indents);
    arena a(source.image().size() + working_room);
    {
        const wd::pmr::basic_document<Traits> doc = owned_copy(source, &a.resource());
        BOOST_TEST(doc.get_allocator().resource() == &a.resource());
        writes_in_its_arena(doc, source, a, indents, expected);
    }
    BOOST_TEST_EQ(a.resource().bytes_in_use(), 0U);
}

/** @ref writes_in_its_arena for an external copy of `source`, its working state in an arena. */
template <class Traits>
void external_in_arena(const wd::basic_document<Traits>& source,
                       std::span<const std::uint32_t> indents) {
    const std::vector<std::string> expected = texts_of(source, indents);
    std::vector<block> buffer = buffer_of(source);
    arena a(working_room);
    {
        const auto doc = external_copy<Traits>(buffer, &a.resource());
        BOOST_TEST(doc.get_allocator().resource() == &a.resource());
        writes_in_its_arena(doc, source, a, indents, expected);
    }
    BOOST_TEST_EQ(a.resource().bytes_in_use(), 0U);
}

// stringify_to into a std::pmr::string on the document's own arena makes no call of the global
// operator new, for a pmr::document, a pmr document under js_traits and an external pmr document
// under base64url: the sample at indents 0, 2 and 10, and the document nested 10,000 levels deep
// compactly, since its indentation grows with the square of its depth (100 MB at an indent of 2).
void stringify_to_calls_no_global_new() {
    const std::array<std::uint32_t, 3> indents{0, 2, 10};
    const std::array<std::uint32_t, 1> compact{0};
    owned_in_arena(sample<wd::default_traits>(), indents);
    owned_in_arena(sample<wd::js_traits>(), indents);
    external_in_arena(sample<url_traits>(), indents);
    owned_in_arena(nested<wd::default_traits>(), compact);
    owned_in_arena(nested<wd::js_traits>(), compact);
    external_in_arena(nested<url_traits>(), compact);
}

/**
 stringify_to of `doc`, whose allocator is `docs`', into a std::pmr::string on `outs`, compactly, in
 pieces of `chunk` bytes: the document's resource makes the allocations of a stack of `frames`
 frames and of the piece held back, and takes back every one.
*/
template <class Document>
void allocates_its_stack_and_piece(const Document& doc, arena& docs, arena& outs,
                                   const std::string& expected, std::uint64_t frames,
                                   std::uint64_t chunk) {
    const std::uint64_t held = docs.resource().bytes_in_use();
    const std::uint64_t made = docs.resource().allocations();
    const std::uint64_t freed = docs.resource().deallocations();
    {
        std::pmr::string out(&outs.resource());
        const global_count global;
        const wd::result<void> written =
            wd::stringify_to(doc, [&out](std::string_view piece) { out.append(piece); }, 0, chunk);
        const std::uint64_t news = global.news();
        BOOST_TEST_EQ(news, 0U);
        BOOST_TEST(written.has_value());
        BOOST_TEST(std::string_view(out) == expected);
    }
    const std::uint64_t allocations =
        stack_allocations(frames) + piece_allocations(chunk, expected.size());
    BOOST_TEST_EQ(docs.resource().allocations() - made, allocations);
    BOOST_TEST_EQ(docs.resource().deallocations() - freed, allocations);
    BOOST_TEST_EQ(docs.resource().bytes_in_use(), held);
}

// The writer allocates, in the document's resource, the frames of its stack past its own room and
// a piece larger than its own alone, as often as their rooms double, and takes back all of it; the
// output's string, on another arena, holds all else. The deep text, about 40,000 bytes, fills
// pieces of 32,768 bytes and a piece of 65,536 bytes only in part; its pieces of 4096 bytes or
// fewer are held in the piece's own room.
void the_writer_allocates_its_stack_and_piece() {
    {
        const wd::document source = nested<wd::default_traits>();
        const std::string expected = wd::stringify(source);
        BOOST_TEST_GT(expected.size(), 32768U);
        BOOST_TEST_LT(expected.size(), 65536U);
        arena docs(source.image().size() + working_room);
        arena outs(working_room);
        const wd::pmr::document doc = owned_copy(source, &docs.resource());
        for (const std::uint64_t chunk : {1U, 7U, 4096U, 32768U, 65536U}) {
            allocates_its_stack_and_piece(doc, docs, outs, expected, deep_frames, chunk);
        }
    }
    {
        const auto source = nested<wd::js_traits>();
        const std::string expected = wd::stringify(source);
        std::vector<block> buffer = buffer_of(source);
        arena docs(working_room);
        arena outs(working_room);
        const auto doc = external_copy<wd::js_traits>(buffer, &docs.resource());
        allocates_its_stack_and_piece(doc, docs, outs, expected, deep_frames, 4096);
    }
    {
        // The run of 10,000 bytes reaches the sink without a piece larger than its own room.
        const auto source = sample<wd::js_traits>();
        const std::string expected = wd::stringify(source);
        arena docs(source.image().size() + working_room);
        arena outs(working_room);
        const auto doc = owned_copy(source, &docs.resource());
        for (const std::uint64_t chunk : {1U, 7U, 4096U}) {
            allocates_its_stack_and_piece(doc, docs, outs, expected, sample_frames, chunk);
        }
    }
}

/**
 stringify of a copy of `source` in an arena, at each of `indents`, calls the global operator new
 and operator delete exactly as often as a std::string that appends the writer's pieces of the same
 text does, made in the same count, and as the stack of `frames` frames needs: its frames past its
 own room, and what the standard library allocates for the list that holds them (counting.hpp's
 bookkeeping); the arena makes no allocation.
*/
template <class Traits>
void calls_new_for_its_string_alone(const wd::basic_document<Traits>& source, std::uint64_t frames,
                                    std::span<const std::uint32_t> indents) {
    arena a(source.image().size() + working_room);
    const wd::pmr::basic_document<Traits> doc = owned_copy(source, &a.resource());
    const std::uint64_t held = a.resource().bytes_in_use();
    for (const std::uint32_t indent : indents) {
        const std::string expected = wd::stringify(source, indent);
        std::vector<std::size_t> sizes;
        piece_recorder recorder(sizes);
        BOOST_TEST(wd::detail::write_json(*wd::detail::access::tree_of(source),
                                          wd::detail::access::node_of(source.root()), indent,
                                          recorder, std::allocator<std::byte>())
                       .has_value());
        std::uint64_t reference_news = 0;
        std::uint64_t reference_deletes = 0;
        {
            // Made in the count, as stringify's string is: what a standard library allocates for a
            // string itself is counted on both sides.
            const global_count global;
            std::string reference;
            std::size_t at = 0;
            for (const std::size_t size : sizes) {
                reference.append(std::string_view(expected).substr(at, size));
                at += size;
            }
            reference_news = global.news();
            reference_deletes = global.deletes();
            BOOST_TEST(reference == expected);
        }
        BOOST_TEST_GT(reference_news, 0U);
        const std::uint64_t deeper = stack_allocations(frames);
        const std::uint64_t stack = deeper == 0 ? 0 : deeper + webcpp::test::bookkeeping().blocks;
        const std::uint64_t made = a.resource().allocations();
        {
            const global_count global;
            const std::string text = wd::stringify(doc, indent);
            const std::uint64_t news = global.news();
            const std::uint64_t deletes = global.deletes();
            BOOST_TEST_EQ(news, reference_news + stack);
            BOOST_TEST_EQ(deletes, reference_deletes + stack);
            BOOST_TEST(text == expected);
        }
        BOOST_TEST_EQ(a.resource().allocations(), made);
        BOOST_TEST_EQ(a.resource().bytes_in_use(), held);
    }
}

// stringify returns a std::string, the program's value in the standard allocator, and keeps its
// stack in the same allocator: it calls the global operator new for that string and the stack's
// deeper frames alone, and asks the document's resource for nothing.
void stringify_calls_new_for_its_string_alone() {
    const std::array<std::uint32_t, 3> indents{0, 2, 10};
    const std::array<std::uint32_t, 1> compact{0};
    calls_new_for_its_string_alone(sample<wd::default_traits>(), sample_frames, indents);
    calls_new_for_its_string_alone(sample<wd::js_traits>(), sample_frames, indents);
    calls_new_for_its_string_alone(nested<wd::default_traits>(), deep_frames, compact);
}

// A dozen small texts written by stringify_to in a 64 KiB arena, the example's, take nothing of it:
// the stack and the piece fit their own room, and make no list a standard library would allocate
// for (counting.hpp's bookkeeping), and the arena holds the document alone, which stays as it was.
void small_texts_take_nothing() {
    arena a(std::size_t{64} * 1024);
    auto made = wd::parse(R"({"user":"Ada","langs":["Analytical","Note G"],"draft":true})",
                          wd::parse_options(), a.allocator());
    require(BOOST_TEST(made.has_value()));
    const std::uint64_t allocations = a.resource().all_allocations();
    const std::uint64_t held = a.resource().bytes_in_use();
    for (std::size_t call = 0; call < 12; ++call) {
        std::string out;
        BOOST_TEST(wd::stringify_to(*made, [&out](std::string_view piece) {
                       out.append(piece);
                   }).has_value());
        BOOST_TEST_EQ(out, R"({"user":"Ada","langs":["Analytical","Note G"],"draft":true})");
    }
    BOOST_TEST_EQ(a.resource().all_allocations(), allocations);
    BOOST_TEST_EQ(a.resource().bytes_in_use(), held);
}

// quote appends to a std::pmr::string on an arena, UTF-8 and UTF-16, long texts with escapes,
// characters past ASCII and lone surrogates, with no call of the global operator new; text that is
// not UTF-8 changes nothing.
void quote_allocates_through_its_output() {
    const std::string utf8 = repeated(escaped_run, 50);
    std::u16string utf16;
    for (std::size_t i = 0; i < 100; ++i) {
        utf16 += u"x\"\\\b\n\x01/\u00e9\u2028";
        // A pair, then a lone low surrogate, then a high one the next round's text follows.
        utf16 += std::u16string{0xD83D, 0xDE00, 0xDC00, 0xD800};
    }
    std::string expected;
    require(BOOST_TEST(wd::quote(std::string_view(utf8), expected).has_value()));
    wd::quote(std::u16string_view(utf16), expected);
    constexpr std::string_view invalid = "\xc3";
    arena a(mebibyte);
    {
        std::pmr::string out(&a.resource());
        const global_count global;
        const wd::result<void> quoted = wd::quote(std::string_view(utf8), out);
        wd::quote(std::u16string_view(utf16), out);
        const wd::result<void> refused = wd::quote(invalid, out);
        const std::uint64_t news = global.news();
        BOOST_TEST_EQ(news, 0U);
        BOOST_TEST(quoted.has_value());
        BOOST_TEST(refused.error() == wd::errc::type);
        BOOST_TEST(std::string_view(out) == expected);
        BOOST_TEST_GT(a.resource().allocations(), 0U);
    }
    BOOST_TEST_EQ(a.resource().bytes_in_use(), 0U);
}

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS

/**
 stringify_to of `doc`, whose working state `failing` holds, with every budget from 0 until it
 succeeds: each step that fails is errc::capacity, with the sink not called after the refusal, its
 text a prefix of the whole, the document `source`'s image and the resource back where it was; and
 as many steps fail as the writer allocates.
*/
template <class Document, class Source>
void capacity_at_every_step(const Document& doc, const Source& source,
                            webcpp::test::failing_resource& failing, std::uint32_t indent,
                            std::uint64_t chunk, std::uint64_t allocations) {
    const std::string expected = wd::stringify(source, indent);
    const std::uint64_t held = failing.bytes_in_use();
    const std::uint64_t refused = failing.refusals();
    std::uint64_t budget = 0;
    for (;; ++budget) {
        failing.allow(budget);
        std::string out;
        std::uint64_t late = 0;
        const std::uint64_t before = failing.refusals();
        const wd::result<void> written = wd::stringify_to(
            doc,
            [&out, &late, &failing, before](std::string_view piece) {
                late += failing.refusals() == before ? 0 : 1;
                out += piece;
            },
            indent, chunk);
        BOOST_TEST_EQ(late, 0U);
        BOOST_TEST(same_image(doc, source));
        BOOST_TEST_EQ(failing.bytes_in_use(), held);
        if (written.has_value()) {
            BOOST_TEST(out == expected);
            break;
        }
        BOOST_TEST(written.error() == wd::errc::capacity);
        BOOST_TEST(std::string_view(expected).starts_with(out));
        // A writer that kept failing would never end the loop.
        if (!BOOST_TEST_LT(budget, allocations)) {
            break;
        }
    }
    BOOST_TEST_EQ(budget, allocations);
    BOOST_TEST_EQ(failing.refusals() - refused, allocations);
}

/**
 stringify of `doc`, whose working state `failing` holds, with a budget of 0: it asks the resource
 for nothing, so it writes the whole text, refused nothing.
*/
template <class Document, class Source>
void stringify_asks_nothing(const Document& doc, const Source& source,
                            webcpp::test::failing_resource& failing, std::uint32_t indent) {
    const std::uint64_t refused = failing.refusals();
    failing.allow(0);
    const std::string text = wd::stringify(doc, indent);
    BOOST_TEST(text == wd::stringify(source, indent));
    BOOST_TEST_EQ(failing.refusals(), refused);
}

// A sink's own std::bad_alloc is the program's: it propagates, as anything a sink throws does, and
// is not taken for the writer's errc::capacity; the writer gives back what it took.
void a_sinks_bad_alloc_propagates() {
    const wd::document source = nested<wd::default_traits>();
    arena a(source.image().size() + working_room);
    const wd::pmr::document doc = owned_copy(source, &a.resource());
    const std::uint64_t held = a.resource().bytes_in_use();
    std::size_t calls = 0;
    bool thrown = false;
    try {
        (void)wd::stringify_to(
            doc,
            [&calls](std::string_view) {
                if (++calls == 2) {
                    throw std::bad_alloc();
                }
            },
            0, 7);
    } catch (const std::bad_alloc&) {
        thrown = true;
    }
    BOOST_TEST(thrown);
    BOOST_TEST_EQ(calls, 2U);
    BOOST_TEST_EQ(a.resource().bytes_in_use(), held);
    BOOST_TEST(same_image(doc, source));
}

/** As many allocations as a test may need, for a document's image and its copies. */
constexpr std::uint64_t unlimited = std::uint64_t{1} << 40U;

// With exceptions, a resource that has no room for the writer's stack or piece, at any step, makes
// stringify_to errc::capacity, and changes nothing, while stringify, which asks it for nothing,
// writes: for the document nested 10,000 levels deep, owned under the default traits and js_traits
// and external under base64url, and for the sample with an indent.
void a_resource_with_no_room() {
    webcpp::test::failing_resource failing(std::pmr::new_delete_resource(), unlimited);
    {
        const wd::document source = nested<wd::default_traits>();
        const std::uint64_t size = wd::stringify(source).size();
        const wd::pmr::document doc = owned_copy(source, &failing);
        for (const std::uint64_t chunk : {1U, 4096U, 32768U}) {
            capacity_at_every_step(doc, source, failing, 0, chunk,
                                   stack_allocations(deep_frames) + piece_allocations(chunk, size));
        }
        stringify_asks_nothing(doc, source, failing, 0);
        failing.allow(unlimited);
    }
    {
        const auto source = nested<wd::js_traits>();
        const std::uint64_t size = wd::stringify(source).size();
        const auto doc = owned_copy(source, &failing);
        capacity_at_every_step(doc, source, failing, 0, 4096,
                               stack_allocations(deep_frames) + piece_allocations(4096, size));
        failing.allow(unlimited);
    }
    {
        const auto source = nested<url_traits>();
        const std::uint64_t size = wd::stringify(source).size();
        std::vector<block> buffer = buffer_of(source);
        const auto doc = external_copy<url_traits>(buffer, &failing);
        capacity_at_every_step(doc, source, failing, 0, 4096,
                               stack_allocations(deep_frames) + piece_allocations(4096, size));
        failing.allow(unlimited);
    }
    {
        const auto source = sample<wd::js_traits>();
        const std::uint64_t size = wd::stringify(source, 2).size();
        const auto doc = owned_copy(source, &failing);
        capacity_at_every_step(doc, source, failing, 2, 4096,
                               stack_allocations(sample_frames) + piece_allocations(4096, size));
        stringify_asks_nothing(doc, source, failing, 2);
        failing.allow(unlimited);
    }
    BOOST_TEST_EQ(failing.bytes_in_use(), 0U);
}

#endif  // WEBCPP_DOCUMENT_NO_EXCEPTIONS

}  // namespace

int main() {
    stringify_to_calls_no_global_new();
    the_writer_allocates_its_stack_and_piece();
    stringify_calls_new_for_its_string_alone();
    small_texts_take_nothing();
    quote_allocates_through_its_output();
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
    a_resource_with_no_room();
    a_sinks_bad_alloc_propagates();
#endif
    return boost::report_errors();
}
