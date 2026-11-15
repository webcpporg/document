// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The handles' and the conversions' working state in the document's allocator: set of literals,
// the keys of missing members and their copies, erasure, Object.assign, deep equality, the array
// methods and the containers' views of a pmr document, of default_traits and of js_traits, and of
// an external one, make no call of the global operator new and give back to the resource all they
// took; the documents the array methods make are in the same resource; a change a place cannot
// take is refused first, with nothing allocated; and, with exceptions, an allocator that runs out
// at any step of set, push, concat, splice or Object.assign is errc::capacity with the document as
// it was, while an erasure asks the allocator for nothing and cannot fail.

#include <webcpp/document.hpp>

#include <boost/core/lightweight_test.hpp>

#include "counting.hpp"
#include "require.hpp"

#include <boost/decimal/decimal64_t.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace wd = webcpp::document;

namespace {

using webcpp::test::arena;
using webcpp::test::global_count;
using webcpp::test::require;

constexpr std::size_t mebibyte = std::size_t{1} << 20U;

/** A document of `Traits` whose image and working state are a memory resource's. */
template <class Traits>
using pmr_document = wd::basic_document<Traits, wd::pmr::owned>;

/** Text of 120 copies of `c` and an "é": longer than any small-string buffer, and not ASCII. */
std::string long_text(char c) {
    return std::string(120, c) + "\xC3\xA9";
}

/** The values the tests store, made before anything is counted: literals own their text. */
struct sample_values {
    /** A long key. */
    std::string key = long_text('k');

    /** Another long key. */
    std::string other_key = long_text('o');

    /** A long string. */
    std::string text = long_text('t');

    /** An object literal, with long keys and strings at two depths. */
    wd::object members{
        {key, text},
        {"list", wd::array{1, text, wd::object{{key, true}}}},
        {"none", nullptr},
    };

    /** An array literal, with a nested object and array. */
    wd::array elements{text, wd::object{{key, 2}}, wd::literal(wd::array{text, false})};
};

/** A document of `Traits` parsed from `json` in the standard allocator. */
template <class Traits>
wd::basic_document<Traits> parsed(std::string_view json) {
    auto made = wd::parse<Traits>(json);
    require(BOOST_TEST(made.has_value()));
    return std::move(*made);
}

/** The document `json` holds, in the arena `a`. */
template <class Traits>
pmr_document<Traits> in_arena(arena& a, std::string_view json) {
    const auto source = parsed<Traits>(json);
    auto made = wd::from_image<Traits>(source.image(), a.allocator());
    require(BOOST_TEST(made.has_value()));
    return std::move(*made);
}

/** The documents a scenario's array methods made, kept in place, which allocates nothing. */
template <class Owned>
struct kept_documents {
    /** The documents, in the order made. */
    std::array<std::optional<Owned>, 8> documents{};

    /** How many there are. */
    std::size_t count = 0;

    /** Keeps `made`, which must be a document. */
    void keep(wd::result<Owned>&& made) {
        require(BOOST_TEST(made.has_value()));
        require(BOOST_TEST_LT(count, documents.size()));
        documents.at(count++).emplace(std::move(*made));
    }
};

/** Whether `a`'s and `b`'s values are written the same, compared outside any counting. */
template <class A, class B>
void same_value(const A& a, const B& b) {
    BOOST_TEST_EQ(a.root().stringify(), b.root().stringify());
}

// Set of literals, by a node, a missing member of a short and of a long key, and an element past
// the end, and a deep copy: under js_traits each string and key, given in UTF-8, is transcoded in
// the prepared value, which is the document's resource's.
template <class Document>
void set_scenario(Document& d, const sample_values& f) {
    BOOST_TEST(d["a"].set(f.members).has_value());
    BOOST_TEST(d["new"].set(f.elements).has_value());
    BOOST_TEST(d["c"][f.key].set(f.text).has_value());
    BOOST_TEST(d["b"][4].set(f.members).has_value());
    BOOST_TEST(d["c"]["copy"].set(d["a"]).has_value());
}

template <class Traits>
void set_literals() {
    arena a(16 * mebibyte);
    const sample_values f;
    constexpr std::string_view json = R"({"a":1,"b":[1,2],"c":{}})";
    auto reference = parsed<Traits>(json);
    set_scenario(reference, f);
    {
        auto d = in_arena<Traits>(a, json);
        const std::uint64_t before = a.resource().allocations();
        {
            const global_count global;
            set_scenario(d, f);
            BOOST_TEST_EQ(global.news(), 0U);
        }
        BOOST_TEST_GT(a.resource().allocations(), before);
        same_value(d, reference);
    }
    BOOST_TEST_EQ(a.resource().bytes_in_use(), 0U);
}

// A handle of a missing member holds its long key in the document's resource, and so do a copy of
// it and a handle assigned it, not in the default resource a pmr string's copy would ask; a move
// takes the key; a set through any of them creates the member the others then read.
template <class Traits>
void missing_member_handles() {
    arena a(16 * mebibyte);
    const sample_values f;
    constexpr std::string_view json = R"({"o":{}})";
    auto reference = parsed<Traits>(json);
    BOOST_TEST(reference["o"][f.key].set(f.members).has_value());
    {
        auto d = in_arena<Traits>(a, json);
        {
            const global_count global;
            const std::uint64_t start = a.resource().allocations();
            const auto missing = d["o"][f.key];
            BOOST_TEST(missing.is_undefined());
            BOOST_TEST_EQ(a.resource().allocations(), start + 1);
            auto copy = missing;
            BOOST_TEST_EQ(a.resource().allocations(), start + 2);
            wd::basic_const_value<Traits, wd::pmr::owned> assigned;
            assigned = copy;
            BOOST_TEST_EQ(a.resource().allocations(), start + 3);
            auto moved = std::move(copy);
            BOOST_TEST_EQ(a.resource().allocations(), start + 3);
            BOOST_TEST(moved.set(f.members).has_value());
            BOOST_TEST(missing.is_object());
            BOOST_TEST(assigned.is_object());
            BOOST_TEST(missing == assigned);
            BOOST_TEST_EQ(global.news(), 0U);
        }
        same_value(d, reference);
    }
    BOOST_TEST_EQ(a.resource().bytes_in_use(), 0U);
}

// set(undefined) erases a member by its node, whose key under js_traits is UTF-16 in the image, and
// by a missing member's handle, before and after the member exists.
template <class Document>
void erase_scenario(Document& d, const sample_values& f) {
    BOOST_TEST(d[f.key].set(wd::undefined).has_value());
    BOOST_TEST(d["short"].set(wd::undefined).has_value());
    BOOST_TEST(d["keep"][f.key].set(wd::undefined).has_value());
    auto missing = d[f.other_key];
    BOOST_TEST(missing.set(wd::undefined).has_value());
    BOOST_TEST(missing.set(1).has_value());
    BOOST_TEST(missing.is_number());
    BOOST_TEST(missing.set(wd::undefined).has_value());
    BOOST_TEST(missing.is_undefined());
}

template <class Traits>
void erase_through_undefined() {
    arena a(16 * mebibyte);
    const sample_values f;
    const std::string json =
        R"({")" + f.key + R"(":1,"short":2,"keep":{")" + f.key + R"(":3,"x":4}})";
    auto reference = parsed<Traits>(json);
    erase_scenario(reference, f);
    {
        auto d = in_arena<Traits>(a, json);
        {
            const global_count global;
            erase_scenario(d, f);
            BOOST_TEST_EQ(global.news(), 0U);
        }
        same_value(d, reference);
    }
    BOOST_TEST_EQ(a.resource().bytes_in_use(), 0U);
}

// Object.assign from an object of long keys, from an array, whose keys are its indices, from this
// document's own members and from a document of the standard allocator.
template <class Document, class Other>
void assign_scenario(Document& d, const Other& other) {
    BOOST_TEST(d["target"].assign_from(d["source"]).has_value());
    BOOST_TEST(d["target"].assign_from(d["list"]).has_value());
    BOOST_TEST(d["target"].assign_from(d["target"]["y"]).has_value());
    BOOST_TEST(d["target"].assign_from(other.root()).has_value());
}

template <class Traits>
void assign_members() {
    arena a(16 * mebibyte);
    const sample_values f;
    const std::string json = R"({"target":{"x":1},"source":{")" + f.key + R"(":")" + f.text +
                             R"(","y":[1,2]},"list":[10,20,30,40,50,60,70,80,90,100,110,120]})";
    const auto other = parsed<Traits>(R"({")" + f.other_key + R"(":{"deep":[1]},"x":2})");
    auto reference = parsed<Traits>(json);
    assign_scenario(reference, other);
    {
        auto d = in_arena<Traits>(a, json);
        {
            const global_count global;
            assign_scenario(d, other);
            BOOST_TEST_EQ(global.news(), 0U);
        }
        same_value(d, reference);
    }
    BOOST_TEST_EQ(a.resource().bytes_in_use(), 0U);
}

// == compares deep values, members in any order, with no stack: it allocates nothing at all, from
// the resource or the global heap, equal or not.
template <class Traits>
void deep_equality() {
    arena a(16 * mebibyte);
    const sample_values f;
    const std::string member = R"(")" + f.key + R"(":")" + f.text + R"(")";
    const std::string x_json =
        R"({"a":[1,{)" + member + R"(,"b":[true,null,[[]]]}],"c":{"d":{"e":2.5,"f":{}}},"g":""})";
    const std::string y_json =
        R"({"g":"","c":{"d":{"f":{},"e":2.5}},"a":[1,{"b":[true,null,[[]]],)" + member + "}]}";
    const std::string deeper =
        R"({"a":[1,{)" + member + R"(,"b":[true,null,[[0]]]}],"c":{"d":{"e":2.5,"f":{}}},"g":""})";
    const std::string renamed =
        R"({"a":[1,{)" + member + R"(,"b":[true,null,[[]]]}],"c":{"d":{"e":2.5,"h":{}}},"g":""})";
    const std::string reordered =
        R"({"a":[{)" + member + R"(,"b":[true,null,[[]]]},1],"c":{"d":{"e":2.5,"f":{}}},"g":""})";
    {
        const auto x = in_arena<Traits>(a, x_json);
        const auto y = in_arena<Traits>(a, y_json);
        const auto z = in_arena<Traits>(a, deeper);
        const auto w = in_arena<Traits>(a, renamed);
        const auto v = in_arena<Traits>(a, reordered);
        const std::uint64_t before = a.resource().allocations();
        const global_count global;
        BOOST_TEST(x.root() == y.root());
        BOOST_TEST(y.root() == x.root());
        BOOST_TEST(x["a"][1] == y["a"][1]);
        BOOST_TEST(x["c"]["d"] == y["c"]["d"]);
        BOOST_TEST(!(x.root() == z.root()));
        BOOST_TEST(!(z.root() == x.root()));
        BOOST_TEST(!(x.root() == w.root()));
        BOOST_TEST(!(x.root() == v.root()));
        BOOST_TEST(!(x["a"] == x["c"]));
        BOOST_TEST(x["missing"] == y["absent"]);
        BOOST_TEST_EQ(global.news(), 0U);
        BOOST_TEST_EQ(a.resource().allocations(), before);
    }
    BOOST_TEST_EQ(a.resource().bytes_in_use(), 0U);
}

// The array methods with literal arguments: push, unshift, concat (of literals and of the array
// itself), slice, pop, shift and splice; the documents they make are in the source's resource.
template <class Document, class Kept>
void array_scenario(Document& d, const sample_values& f, Kept& kept) {
    auto list = d["list"];
    const auto pushed = list.push(f.members, f.elements, f.text);
    BOOST_TEST(pushed.has_value() && *pushed == 8);
    const auto unshifted = list.unshift(f.elements, f.key);
    BOOST_TEST(unshifted.has_value() && *unshifted == 10);
    kept.keep(list.concat(f.elements, f.members, f.text, d["list"]));
    kept.keep(list.slice(1, 4));
    kept.keep(list.pop());
    kept.keep(list.shift());
    kept.keep(list.splice(1, 2, f.members, f.elements));
    kept.keep(list.splice(5));
}

template <class Traits>
void array_methods() {
    arena a(16 * mebibyte);
    const sample_values f;
    constexpr std::string_view json = R"({"list":[1,2,3,4,5]})";
    auto reference = parsed<Traits>(json);
    kept_documents<wd::basic_document<Traits>> expected;
    array_scenario(reference, f, expected);
    {
        auto d = in_arena<Traits>(a, json);
        kept_documents<pmr_document<Traits>> made;
        {
            const global_count global;
            array_scenario(d, f, made);
            BOOST_TEST_EQ(global.news(), 0U);
        }
        same_value(d, reference);
        BOOST_TEST_EQ(made.count, expected.count);
        for (std::size_t i = 0; i < made.count; ++i) {
            const auto& document = made.documents.at(i);
            const auto& wanted = expected.documents.at(i);
            if (!document.has_value() || !wanted.has_value()) {
                BOOST_ERROR("a document the array methods made is missing");
                continue;
            }
            BOOST_TEST(document->get_allocator().resource() == &a.resource());
            same_value(*document, *wanted);
        }
    }
    BOOST_TEST_EQ(a.resource().bytes_in_use(), 0U);
}

// The views' writing members: an object's insert, insert_or_assign, try_emplace, a deep copy, its
// bounds by key, erase by key and by iterator and clear; an array's push_back, emplace_back,
// insert, erase, resize and pop_back.
template <class Document>
void views_scenario(Document& d, const sample_values& f) {
    auto o = d["o"].as_object();
    require(BOOST_TEST(o.has_value()));
    BOOST_TEST(o->insert(f.key, f.members).has_value());
    BOOST_TEST(o->insert_or_assign("a", f.elements).has_value());
    BOOST_TEST(o->try_emplace(f.other_key, f.text).has_value());
    BOOST_TEST(o->insert("copy", d["l"]).has_value());
    const auto keys = o->by_key();
    // By UTF-16 code units: "a", "copy", the key of 'k's, the key of 'o's.
    BOOST_TEST((*keys.lower_bound("b")).second == d["o"]["copy"]);
    BOOST_TEST((*keys.lower_bound(f.key)).second == d["o"][f.key]);
    BOOST_TEST((*keys.upper_bound(f.key)).second == d["o"][f.other_key]);
    BOOST_TEST(keys.upper_bound(f.other_key) == keys.end());
    BOOST_TEST(o->erase(f.other_key).has_value());
    // In insertion order the key of 'k's is followed by "a".
    BOOST_TEST(o->erase(o->find(f.key)) == o->find("a"));
    auto p = d["p"].as_object();
    require(BOOST_TEST(p.has_value()));
    p->clear();

    auto l = d["l"].as_array();
    require(BOOST_TEST(l.has_value()));
    BOOST_TEST(l->push_back(f.members).has_value());
    BOOST_TEST(l->emplace_back(f.elements).has_value());
    BOOST_TEST(l->insert(l->begin(), f.text).has_value());
    l->erase(l->begin());
    BOOST_TEST(l->resize(10).has_value());
    l->pop_back();
}

template <class Traits>
void view_members() {
    arena a(16 * mebibyte);
    const sample_values f;
    const std::string json =
        R"({"o":{},"l":[1,2],"p":{")" + f.key + R"(":1,")" + f.other_key + R"(":2,"z":3}})";
    auto reference = parsed<Traits>(json);
    views_scenario(reference, f);
    {
        auto d = in_arena<Traits>(a, json);
        {
            const global_count global;
            views_scenario(d, f);
            BOOST_TEST_EQ(global.news(), 0U);
        }
        same_value(d, reference);
    }
    BOOST_TEST_EQ(a.resource().bytes_in_use(), 0U);
}

// A document's const reads ask its resource for nothing, as a standard container's do, so that any
// number of threads may make them at once whatever the resource: a lookup by a key of any length,
// a missing member of a short key, get, ==, iteration, the views, image, compact_size, stringify,
// and stringify_to of a value nested 16 deep in chunks of 4096 bytes. A missing member of a long
// key is one of the reads that allocate: its handle holds the key, once.
template <class Traits>
void const_reads_ask_nothing() {
    arena a(16 * mebibyte);
    const sample_values f;
    const std::string json = R"({"a":1,")" + f.key + R"(":")" + f.text +
                             R"(","list":[1,[2,{"x":3}]],"deep":)" + std::string(15, '[') +
                             std::string(15, ']') + "}";
    const auto d = in_arena<Traits>(a, json);
    const auto other = in_arena<Traits>(a, json);
    const std::string long_missing(200, 'm');
    std::string written;
    const std::uint64_t before = a.resource().allocations();
    {
        BOOST_TEST(d[f.key].is_string());
        BOOST_TEST_EQ(d["a"].template get<std::int64_t>().value(), 1);
        BOOST_TEST(d["k"].is_undefined());
        BOOST_TEST(d == other);
        std::uint64_t elements = 0;
        for (const auto element : d["list"]) {
            elements += element.is_undefined() ? 0 : 1;
        }
        BOOST_TEST_EQ(elements, 2U);
        const auto o = d.root().as_object();
        require(BOOST_TEST(o.has_value()));
        BOOST_TEST_EQ(o->size(), 4U);
        BOOST_TEST(o->contains(f.key));
        BOOST_TEST(!d.image().empty());
        BOOST_TEST_GT(d.compact_size(), 0U);
        BOOST_TEST_EQ(wd::stringify(d, 2), wd::stringify(other, 2));
        BOOST_TEST(wd::stringify_to(
                       d, [&written](std::string_view piece) { written.append(piece); }, 2)
                       .has_value());
    }
    BOOST_TEST_EQ(a.resource().allocations(), before);
    BOOST_TEST_EQ(written, parsed<Traits>(json).root().stringify(2));
    {
        const auto missing = d[long_missing];
        BOOST_TEST(missing.is_undefined());
        BOOST_TEST_EQ(a.resource().allocations(), before + 1);
    }
}

// Under js_traits, a bound by key compares a UTF-8 key with the stored UTF-16 ones as it is
// transcoded: U+10000's high surrogate comes before U+E000, and a prefix before its extensions.
void bounds_by_utf16_units() {
    arena a(mebibyte);
    const std::string private_use = "\xEE\x80\x80";
    const std::string supplementary = "\xF0\x90\x80\x80";
    auto d = in_arena<wd::js_traits>(
        a, R"({")" + private_use + R"(":1,")" + supplementary + R"(":2,"ab":3,"abc":4,"b":5})");
    const global_count global;
    const auto o = d.root().as_object();
    require(BOOST_TEST(o.has_value()));
    const auto keys = o->by_key();
    BOOST_TEST((*keys.lower_bound(supplementary)).second == d[supplementary]);
    BOOST_TEST((*keys.upper_bound(supplementary)).second == d[private_use]);
    BOOST_TEST((*keys.lower_bound(private_use)).second == d[private_use]);
    BOOST_TEST(keys.upper_bound(private_use) == keys.end());
    BOOST_TEST((*keys.lower_bound("ab")).second == d["ab"]);
    BOOST_TEST((*keys.upper_bound("ab")).second == d["abc"]);
    BOOST_TEST((*keys.lower_bound("abb")).second == d["abc"]);
    BOOST_TEST((*keys.lower_bound("abcd")).second == d["b"]);
    BOOST_TEST((*keys.lower_bound("")).second == d["ab"]);
    BOOST_TEST_EQ(global.news(), 0U);
}

// An external document, formatted in a buffer, keeps its working state in its allocator: set of
// literals, a missing member's handle and its copy, push and concat, whose document is owned in
// the same resource.
template <class Document, class Kept>
void external_scenario(Document& d, const sample_values& f, Kept& kept) {
    BOOST_TEST(d.root().set(f.members).has_value());
    const auto missing = d[f.other_key];
    auto copy = missing;
    BOOST_TEST(copy.set(f.elements).has_value());
    BOOST_TEST(missing.is_array());
    const auto pushed = d["list"].push(f.text, f.members);
    BOOST_TEST(pushed.has_value() && *pushed == 5);
    kept.keep(d["list"].concat(f.elements));
}

/** The buffer of an external document, 16-aligned. */
struct alignas(16) external_buffer {
    /** Its bytes. */
    std::array<std::byte, 65536> bytes{};
};

void external_document() {
    arena a(16 * mebibyte);
    const sample_values f;
    auto reference = parsed<wd::default_traits>("null");
    kept_documents<wd::document> expected;
    external_scenario(reference, f, expected);
    // On the heap, before anything is counted: Emscripten's stack is 64 KiB.
    const auto buffer = std::make_unique<external_buffer>();
    {
        auto made = wd::format(std::span(buffer->bytes), a.allocator());
        require(BOOST_TEST(made.has_value()));
        kept_documents<wd::pmr::document> kept;
        {
            const global_count global;
            external_scenario(*made, f, kept);
            BOOST_TEST_EQ(global.news(), 0U);
        }
        same_value(*made, reference);
        BOOST_TEST_EQ(kept.count, 1U);
        const auto& document = kept.documents[0];
        const auto& wanted = expected.documents[0];
        if (!document.has_value() || !wanted.has_value()) {
            BOOST_ERROR("the document concat made is missing");
        } else {
            BOOST_TEST(document->get_allocator().resource() == &a.resource());
            same_value(*document, *wanted);
        }
    }
    BOOST_TEST_EQ(a.resource().bytes_in_use(), 0U);
}

// A change a place cannot take is refused with errc::type before its value is prepared, ahead of
// the value's own errors, and allocates nothing, in the document's resource or the global heap: a
// handle of nothing, a missing member of what is no longer an object, a missing element of what is
// no longer an array, and a node erased since.
template <class Traits>
void refused_first() {
    arena a(mebibyte);
    const sample_values f;
    auto d = in_arena<Traits>(a, R"({"a":1,"o":{},"l":[],"gone":2})");
    auto nothing = d["missing"]["x"];
    auto member = d["o"]["new"];
    auto element = d["l"][std::uint64_t{3}];
    auto gone = d["gone"];
    BOOST_TEST(d["o"].set(1).has_value());
    BOOST_TEST(d["l"].set(2).has_value());
    const auto erased = d.root().erase("gone");
    BOOST_TEST(erased.has_value() && *erased);
    const std::uint64_t before = a.resource().allocations();
    const global_count global;
    BOOST_TEST(nothing.is_undefined());
    const wd::date_ms late{std::chrono::milliseconds{wd::detail::date_limit_ms + 1}};
    for (auto place : {nothing, member, element, gone}) {
        BOOST_TEST(
            place.set(std::numeric_limits<boost::decimal::decimal64_t>::infinity()).error() ==
            wd::errc::type);
        BOOST_TEST(place.set(late).error() == wd::errc::type);
        BOOST_TEST(place.set(std::string_view("\xFF")).error() == wd::errc::type);
        BOOST_TEST(place.set(1).error() == wd::errc::type);
        BOOST_TEST(place.set(f.text).error() == wd::errc::type);
        BOOST_TEST(place.set(f.members).error() == wd::errc::type);
        BOOST_TEST(place.set(f.elements).error() == wd::errc::type);
    }
    BOOST_TEST(nothing.set(d["a"]).error() == wd::errc::type);
    BOOST_TEST(nothing.set(wd::undefined).error() == wd::errc::type);
    BOOST_TEST_EQ(global.news(), 0U);
    BOOST_TEST_EQ(a.resource().allocations(), before);
    same_value(d, parsed<Traits>(R"({"a":1,"o":1,"l":2})"));
}

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS

/** What a resource that is never to run out allows. */
constexpr std::uint64_t unlimited = std::numeric_limits<std::uint64_t>::max();

/**
 Runs `op` on a fresh copy of the document `json` in a resource that fails from its allocation
 `budget` on, for each budget from 0 until `op` succeeds: each failure must be errc::capacity with
 the document as it was. Returns how many budgets failed.
*/
template <class Traits, class Op>
std::uint64_t fails_softly(const std::string& json, Op op) {
    arena a(16 * mebibyte);
    const auto source = parsed<Traits>(json);
    const std::string before = source.root().stringify();
    webcpp::test::failing_resource failing(&a.resource(), unlimited);
    const std::pmr::polymorphic_allocator<std::byte> alloc(&failing);
    std::uint64_t failures = 0;
    for (std::uint64_t budget = 0; budget < 1000; ++budget) {
        failing.allow(unlimited);
        auto d = wd::from_image<Traits>(source.image(), alloc);
        require(BOOST_TEST(d.has_value()));
        failing.allow(budget);
        const boost::system::error_code done = op(*d);
        failing.allow(unlimited);
        if (!done) {
            BOOST_TEST_GT(failures, 0U);
            return failures;
        }
        BOOST_TEST(done == wd::errc::capacity);
        BOOST_TEST_EQ(d->root().stringify(), before);
        ++failures;
    }
    BOOST_TEST(false);
    return failures;
}

/**
 Runs `op`, an erasure, on the document `json` in a resource that refuses every allocation: it must
 succeed, change the document, and ask the resource for nothing.
*/
template <class Traits, class Op>
void never_fails(const std::string& json, Op op) {
    arena a(16 * mebibyte);
    const auto source = parsed<Traits>(json);
    const std::string before = source.root().stringify();
    webcpp::test::failing_resource failing(&a.resource(), unlimited);
    auto d = wd::from_image<Traits>(source.image(), std::pmr::polymorphic_allocator<>(&failing));
    require(BOOST_TEST(d.has_value()));
    failing.allow(0);
    BOOST_TEST(!op(*d));
    BOOST_TEST_EQ(failing.refusals(), 0U);
    failing.allow(unlimited);
    BOOST_TEST_NE(d->root().stringify(), before);
}

/** The error of `made`; none when it holds a document in `resource`. */
template <class Document>
boost::system::error_code error_of(const wd::result<Document>& made,
                                   const std::pmr::memory_resource* resource) {
    if (!made.has_value()) {
        return made.error();
    }
    BOOST_TEST(made->get_allocator().resource() == resource);
    return {};
}

// A handle of a missing member whose long key the resource had no room for reads undefined, and
// a change through it or a copy of it is errc::capacity, with nothing changed; a short key fits
// the string's own buffer, so its handle needs no allocation.
template <class Traits>
void unheld_key() {
    arena a(16 * mebibyte);
    const sample_values f;
    const auto source = parsed<Traits>(R"({"o":{}})");
    webcpp::test::failing_resource failing(&a.resource(), unlimited);
    auto made = wd::from_image<Traits>(source.image(), std::pmr::polymorphic_allocator<>(&failing));
    require(BOOST_TEST(made.has_value()));
    auto& d = *made;
    failing.allow(0);
    auto missing = d["o"][f.key];
    auto small = d["o"]["k"];
    failing.allow(unlimited);
    BOOST_TEST_EQ(failing.refusals(), 1U);
    BOOST_TEST(missing.is_undefined());
    BOOST_TEST(missing.set(f.members).error() == wd::errc::capacity);
    BOOST_TEST(missing.set(wd::undefined).error() == wd::errc::capacity);
    auto copy = missing;
    BOOST_TEST(copy.set(1).error() == wd::errc::capacity);
    BOOST_TEST_EQ(d["o"].size(), 0U);
    BOOST_TEST(small.set(1).has_value());
    BOOST_TEST_EQ(d["o"].size(), 1U);
}

template <class Traits>
void failures_change_nothing() {
    const sample_values f;
    const std::string json =
        R"({"a":1,"list":[1,2,3,4],"o":{")" + f.key + R"(":{"deep":[1,2]},"x":1}})";
    // A set through a node, and through a missing member's handle made while the resource fails.
    fails_softly<Traits>(json, [&](auto& d) { return d["a"].set(f.members).error(); });
    fails_softly<Traits>(json, [&](auto& d) { return d[f.key].set(f.members).error(); });
    fails_softly<Traits>(json, [&](auto& d) { return d["list"].push(f.elements, f.text).error(); });
    fails_softly<Traits>(json, [&](auto& d) {
        return error_of(d["list"].concat(f.elements, f.members, f.text),
                        d.get_allocator().resource());
    });
    fails_softly<Traits>(json, [&](auto& d) {
        return error_of(d["list"].splice(1, 2, f.members, f.elements),
                        d.get_allocator().resource());
    });
    fails_softly<Traits>(json, [&](auto& d) { return d["o"].assign_from(d["list"]).error(); });
    // set(undefined) of an element stores null in its place, a change, not an erasure.
    fails_softly<Traits>(json, [&](auto& d) { return d["list"][1].set(wd::undefined).error(); });
}

// Erasures ask the allocator for nothing, so they succeed with none to give: set(undefined) of a
// member of a long key, delete by key, each of the views' erasures and a truncation.
template <class Traits>
void erasures_never_fail() {
    const sample_values f;
    const std::string json =
        R"({"a":1,"list":[1,[2,{"x":3}],3,4],"o":{")" + f.key + R"(":{"deep":[1,2]},"x":1}})";
    const boost::system::error_code none;
    never_fails<Traits>(json, [&](auto& d) { return d["o"][f.key].set(wd::undefined).error(); });
    never_fails<Traits>(json, [&](auto& d) { return d["o"].erase(f.key).error(); });
    never_fails<Traits>(json, [&](auto& d) {
        const auto o = d["o"].as_object();
        const auto next = o->erase(o->find(f.key));
        BOOST_TEST(next == o->find("x"));
        return none;
    });
    never_fails<Traits>(json, [&](auto& d) {
        d["o"].as_object()->clear();
        return none;
    });
    never_fails<Traits>(json, [&](auto& d) {
        const auto l = d["list"].as_array();
        const auto next = l->erase(l->begin());
        BOOST_TEST(next == l->begin());
        return none;
    });
    never_fails<Traits>(json, [&](auto& d) {
        const auto l = d["list"].as_array();
        BOOST_TEST(l->erase(std::next(l->begin()), l->end()) == l->end());
        return none;
    });
    never_fails<Traits>(json, [&](auto& d) {
        d["list"].as_array()->pop_back();
        return none;
    });
    never_fails<Traits>(json, [&](auto& d) {
        d["list"].as_array()->clear();
        return none;
    });
    never_fails<Traits>(json, [&](auto& d) { return d["list"].as_array()->resize(1).error(); });
}

#endif  // WEBCPP_DOCUMENT_NO_EXCEPTIONS

}  // namespace

int main() {
    set_literals<wd::default_traits>();
    set_literals<wd::js_traits>();
    missing_member_handles<wd::default_traits>();
    missing_member_handles<wd::js_traits>();
    erase_through_undefined<wd::default_traits>();
    erase_through_undefined<wd::js_traits>();
    assign_members<wd::default_traits>();
    assign_members<wd::js_traits>();
    deep_equality<wd::default_traits>();
    deep_equality<wd::js_traits>();
    array_methods<wd::default_traits>();
    array_methods<wd::js_traits>();
    view_members<wd::default_traits>();
    view_members<wd::js_traits>();
    const_reads_ask_nothing<wd::default_traits>();
    const_reads_ask_nothing<wd::js_traits>();
    bounds_by_utf16_units();
    external_document();
    refused_first<wd::default_traits>();
    refused_first<wd::js_traits>();
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
    failures_change_nothing<wd::default_traits>();
    failures_change_nothing<wd::js_traits>();
    erasures_never_fail<wd::default_traits>();
    erasures_never_fail<wd::js_traits>();
    unheld_key<wd::default_traits>();
    unheld_key<wd::js_traits>();
#endif
    return boost::report_errors();
}
