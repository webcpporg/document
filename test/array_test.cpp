// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// JavaScript's array methods on a document's handles, each result Node 26's: splice in both forms,
// slice, push, pop, shift, unshift, reverse, concat, at and the length set, with JavaScript's index
// rules; the methods on an empty array and on anything but an array; a handle kept while pushes
// make the image grow; and a splice into external buffers of every size, which completes or
// changes nothing.

#include <webcpp/document/document.hpp>

#include <boost/core/lightweight_test.hpp>

#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wd = webcpp::document;

namespace {

constexpr std::int64_t int64_min = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t int64_max = std::numeric_limits<std::int64_t>::max();

wd::document make_document() {
    auto made = wd::document::create();
    webcpp::test::require(BOOST_TEST(made.has_value()));
    return std::move(*made);
}

// A document holding `v`, set at its root.
template <class T>
wd::document document_of(const T& v) {
    wd::document d = make_document();
    wd::value root = d.root();
    webcpp::test::require(BOOST_TEST(root.set(v).has_value()));
    return d;
}

// The array each row of the table starts from.
wd::document five() {
    return document_of(wd::array{0, 1, 2, 3, 4});
}

// NOLINTBEGIN(misc-no-recursion): a value is written by writing its children
// Compact JSON of the kinds this test stores (null, booleans, integers, ASCII strings, arrays and
// objects), as JSON.stringify writes them; empty for undefined.
template <class Storage>
void write(const wd::basic_const_value<wd::default_traits, Storage>& v, std::string& out) {
    switch (v.kind()) {
        case wd::kind::undefined: break;
        case wd::kind::null: out += "null"; break;
        case wd::kind::boolean: out += v.value_or(false) ? "true" : "false"; break;
        case wd::kind::number: out += std::to_string(v.template get<std::int64_t>().value()); break;
        case wd::kind::string: {
            out += '"';
            const std::string_view chars = v.template get<std::string_view>().value();
            for (const char c : chars) {
                if (c == '"' || c == '\\') {
                    out += '\\';
                }
                out += c;
            }
            out += '"';
            break;
        }
        case wd::kind::array: {
            out += '[';
            bool first = true;
            for (const auto e : v) {
                if (!first) {
                    out += ',';
                }
                first = false;
                write(e, out);
            }
            out += ']';
            break;
        }
        case wd::kind::object: {
            out += '{';
            bool first = true;
            for (const auto [key, member] : v.entries()) {
                out += first ? "\"" : ",\"";
                first = false;
                out += key;
                out += "\":";
                write(member, out);
            }
            out += '}';
            break;
        }
        default: out += '?'; break;
    }
}

// NOLINTEND(misc-no-recursion)

template <class Storage>
std::string text(const wd::basic_const_value<wd::default_traits, Storage>& v) {
    std::string out;
    write(v, out);
    return out;
}

std::string text(const wd::document& d) {
    return text(d.root());
}

// A returned document's text, or "error" when the method failed.
std::string text(const wd::result<wd::document>& r) {
    return r.has_value() ? text(*r) : std::string("error");
}

// A buffer of `size` bytes at a 16-aligned address, as a browser's memory would be.
class aligned_buffer {
public:
    explicit aligned_buffer(std::size_t size) : raw_(size + 16) {
        void* p = raw_.data();
        std::size_t space = raw_.size();
        std::align(16, size, p, space);
        bytes_ = {static_cast<std::byte*>(p), size};
    }

    [[nodiscard]] std::span<std::byte> bytes() const { return bytes_; }

private:
    std::vector<std::byte> raw_;
    std::span<std::byte> bytes_;
};

void relative_index_follows_ecma_262() {
    using wd::detail::relative_index;
    BOOST_TEST_EQ(relative_index(0, 5), 0U);
    BOOST_TEST_EQ(relative_index(3, 5), 3U);
    BOOST_TEST_EQ(relative_index(5, 5), 5U);
    BOOST_TEST_EQ(relative_index(9, 5), 5U);
    BOOST_TEST_EQ(relative_index(-1, 5), 4U);
    BOOST_TEST_EQ(relative_index(-5, 5), 0U);
    BOOST_TEST_EQ(relative_index(-6, 5), 0U);
    BOOST_TEST_EQ(relative_index(int64_min, 5), 0U);
    BOOST_TEST_EQ(relative_index(int64_max, 5), 5U);
    BOOST_TEST_EQ(relative_index(-1, 0), 0U);
    BOOST_TEST_EQ(relative_index(0, 0), 0U);
    static_assert(wd::detail::relative_index(-2, 5) == 3);
}

void splice_removes_and_inserts() {
    {
        wd::document d = five();
        BOOST_TEST_EQ(text(d.root().splice(1, 2)), "[1,2]");
        BOOST_TEST_EQ(text(d), "[0,3,4]");
    }
    {
        wd::document d = five();
        BOOST_TEST_EQ(text(d.root().splice(1, 0, "x", "y")), "[]");
        BOOST_TEST_EQ(text(d), R"([0,"x","y",1,2,3,4])");
    }
    {
        wd::document d = five();
        BOOST_TEST_EQ(text(d.root().splice(-2)), "[3,4]");
        BOOST_TEST_EQ(text(d), "[0,1,2]");
    }
    {
        wd::document d = five();
        BOOST_TEST_EQ(text(d.root().splice(-9, 1)), "[0]");
        BOOST_TEST_EQ(text(d), "[1,2,3,4]");
    }
    {
        wd::document d = five();
        BOOST_TEST_EQ(text(d.root().splice(9, 1, "z")), "[]");
        BOOST_TEST_EQ(text(d), R"([0,1,2,3,4,"z"])");
    }
    {
        wd::document d = five();
        BOOST_TEST_EQ(text(d.root().splice(2, -1, "z")), "[]");
        BOOST_TEST_EQ(text(d), R"([0,1,"z",2,3,4])");
    }
    {
        wd::document d = five();
        BOOST_TEST_EQ(text(d.root().splice(2)), "[2,3,4]");
        BOOST_TEST_EQ(text(d), "[0,1]");
    }
    {
        wd::document d = five();
        BOOST_TEST_EQ(text(d.root().splice(1, 99)), "[1,2,3,4]");
        BOOST_TEST_EQ(text(d), "[0]");
    }
}

void splice_at_the_index_limits() {
    {
        wd::document d = five();
        BOOST_TEST_EQ(text(d.root().splice(int64_min, 1)), "[0]");
        BOOST_TEST_EQ(text(d), "[1,2,3,4]");
    }
    {
        wd::document d = five();
        BOOST_TEST_EQ(text(d.root().splice(int64_max, int64_max, 5)), "[]");
        BOOST_TEST_EQ(text(d), "[0,1,2,3,4,5]");
    }
    {
        wd::document d = five();
        BOOST_TEST_EQ(text(d.root().splice(3, int64_max)), "[3,4]");
        BOOST_TEST_EQ(text(d), "[0,1,2]");
    }
    {
        wd::document d = five();
        BOOST_TEST_EQ(text(d.root().splice(1, int64_min, wd::object{{"k", true}})), "[]");
        BOOST_TEST_EQ(text(d), R"([0,{"k":true},1,2,3,4])");
    }
}

// The removed elements are deep copies, and what is inserted is read before anything changes, so
// an array spliced into itself is the array as it was.
void splice_copies_before_it_changes() {
    wd::document d = five();
    wd::value root = d.root();
    BOOST_TEST_EQ(text(root.splice(0, 1, root)), "[0]");
    BOOST_TEST_EQ(text(d), "[[0,1,2,3,4],1,2,3,4]");

    wd::document nested = document_of(wd::array{wd::array{1, 2}, wd::object{{"a", 3}}});
    const auto removed = nested.root().splice(0, 2, nested[1]);
    BOOST_TEST_EQ(text(removed), R"([[1,2],{"a":3}])");
    BOOST_TEST_EQ(text(nested), R"([{"a":3}])");
}

void slice_copies() {
    wd::document d = five();
    const wd::document& c = d;
    BOOST_TEST_EQ(text(c.root().slice(1, 3)), "[1,2]");
    BOOST_TEST_EQ(text(c.root().slice(-2)), "[3,4]");
    BOOST_TEST_EQ(text(c.root().slice(3, 1)), "[]");
    BOOST_TEST_EQ(text(c.root().slice(-9, -4)), "[0]");
    BOOST_TEST_EQ(text(c.root().slice()), "[0,1,2,3,4]");
    BOOST_TEST_EQ(text(c.root().slice(int64_min, int64_max)), "[0,1,2,3,4]");
    BOOST_TEST_EQ(text(d.root().slice(4, 9)), "[4]");
    BOOST_TEST_EQ(text(d), "[0,1,2,3,4]");

    // The copy is its own: changing it leaves the array as it was.
    wd::document nested = document_of(wd::array{wd::literal(wd::array{1})});
    auto copy = nested.root().slice();
    if (!BOOST_TEST(copy.has_value())) {
        return;
    }
    BOOST_TEST(copy->root()[0].push(2).has_value());
    BOOST_TEST_EQ(text(*copy), "[[1,2]]");
    BOOST_TEST_EQ(text(nested), "[[1]]");
}

void push_pop_shift_unshift() {
    {
        wd::document d = five();
        wd::value root = d.root();
        BOOST_TEST_EQ(root.push(5, 6).value(), 7U);
        BOOST_TEST_EQ(text(d), "[0,1,2,3,4,5,6]");
    }
    {
        wd::document d = five();
        const auto popped = d.root().pop();
        BOOST_TEST_EQ(text(popped), "4");
        BOOST_TEST_EQ(text(d), "[0,1,2,3]");
    }
    {
        wd::document d = five();
        const auto shifted = d.root().shift();
        BOOST_TEST_EQ(text(shifted), "0");
        BOOST_TEST_EQ(text(d), "[1,2,3,4]");
    }
    {
        wd::document d = five();
        wd::value root = d.root();
        BOOST_TEST_EQ(root.unshift("a", "b").value(), 7U);
        BOOST_TEST_EQ(text(d), R"(["a","b",0,1,2,3,4])");
    }
    {
        // No value: the length, unchanged.
        wd::document d = five();
        wd::value root = d.root();
        BOOST_TEST_EQ(root.push().value(), 5U);
        BOOST_TEST_EQ(root.unshift().value(), 5U);
        BOOST_TEST_EQ(text(d), "[0,1,2,3,4]");
    }
    {
        // A container popped comes whole, and literals and copies are pushed.
        wd::document d = document_of(wd::array{wd::object{{"a", wd::array{1}}}});
        const wd::document other = document_of(wd::array{7});
        wd::value root = d.root();
        BOOST_TEST_EQ(root.push(wd::array{8}, other, other[0], nullptr, true).value(), 6U);
        BOOST_TEST_EQ(text(d), R"([{"a":[1]},[8],[7],7,null,true])");
        BOOST_TEST_EQ(text(root.shift()), R"({"a":[1]})");
        // An element that reads undefined is pushed as null, as JSON.stringify writes it.
        BOOST_TEST_EQ(root.push(other["absent"]).value(), 6U);
        BOOST_TEST_EQ(text(d), "[[8],[7],7,null,true,null]");
    }
}

void reverse_reverses() {
    wd::document d = five();
    wd::value root = d.root();
    BOOST_TEST(root.reverse().has_value());
    BOOST_TEST_EQ(text(d), "[4,3,2,1,0]");
    wd::document even = document_of(wd::array{1, wd::array{2}, "3", nullptr});
    wd::value even_root = even.root();
    BOOST_TEST(even_root.reverse().has_value());
    BOOST_TEST_EQ(text(even), R"([null,"3",[2],1])");
}

void concat_spreads_arrays() {
    const wd::document d = five();
    const wd::document& c = d;
    BOOST_TEST_EQ(text(c.root().concat(wd::array{5, wd::array{6}}, 7)), "[0,1,2,3,4,5,[6],7]");
    BOOST_TEST_EQ(text(d), "[0,1,2,3,4]");
    BOOST_TEST_EQ(text(c.root().concat()), "[0,1,2,3,4]");

    // A const_value holding an array spreads its elements; one holding an object is appended.
    const wd::document other = document_of(wd::object{{"list", wd::array{8, 9}}, {"k", 1}});
    BOOST_TEST_EQ(text(c.root().concat(other["list"], other.root())),
                  R"([0,1,2,3,4,8,9,{"list":[8,9],"k":1}])");
    // A document holding an array spreads too, and a literal holding one, as JavaScript's array.
    const wd::document pair = document_of(wd::array{"p", "q"});
    BOOST_TEST_EQ(text(c.root().concat(pair, wd::literal(wd::array{10}), wd::object{})),
                  R"([0,1,2,3,4,"p","q",10,{}])");
    // The array concatenated with itself.
    BOOST_TEST_EQ(text(c.root().concat(c.root())), "[0,1,2,3,4,0,1,2,3,4]");
    BOOST_TEST_EQ(text(d), "[0,1,2,3,4]");
}

void at_counts_from_the_end() {
    wd::document d = five();
    const wd::document& c = d;
    const wd::const_value read = c.root();
    BOOST_TEST_EQ(text(read.at(-1)), "4");
    BOOST_TEST_EQ(text(read.at(-3)), "2");
    BOOST_TEST_EQ(text(read.at(0)), "0");
    BOOST_TEST(read.at(5).is_undefined());
    BOOST_TEST(read.at(-6).is_undefined());
    BOOST_TEST(read.at(-5).value_or(std::int64_t{-1}) == 0);
    BOOST_TEST(read.at(int64_min).is_undefined());
    BOOST_TEST(read.at(int64_max).is_undefined());
    BOOST_TEST_EQ(text(d), "[0,1,2,3,4]");

    // Through a handle that writes, the element is written; out of range, nothing is.
    wd::value root = d.root();
    wd::value last = root.at(-1);
    BOOST_TEST(last.set(9).has_value());
    BOOST_TEST_EQ(text(d), "[0,1,2,3,9]");
    wd::value past_end = root.at(5);
    BOOST_TEST(past_end.set(9).error() == wd::errc::type);
    wd::value before_start = root.at(-6);
    BOOST_TEST(before_start.set(9).error() == wd::errc::type);
    BOOST_TEST_EQ(text(d), "[0,1,2,3,9]");
}

void set_length_truncates_and_fills() {
    {
        wd::document d = five();
        wd::value root = d.root();
        BOOST_TEST(root.set_length(2).has_value());
        BOOST_TEST_EQ(text(d), "[0,1]");
    }
    wd::document d = five();
    wd::value root = d.root();
    BOOST_TEST(root.set_length(7).has_value());
    BOOST_TEST_EQ(text(d), "[0,1,2,3,4,null,null]");
    BOOST_TEST(d[7].set(9).has_value());
    BOOST_TEST_EQ(text(d), "[0,1,2,3,4,null,null,9]");
    BOOST_TEST(root.set_length(0).has_value());
    BOOST_TEST_EQ(text(d), "[]");
}

void an_empty_array() {
    wd::document d = document_of(wd::array{});
    const auto popped = d.root().pop();
    if (BOOST_TEST(popped.has_value())) {
        BOOST_TEST(popped->root().is_undefined());
        BOOST_TEST_EQ(text(*popped), "");
    }
    const auto shifted = d.root().shift();
    if (BOOST_TEST(shifted.has_value())) {
        BOOST_TEST(shifted->root().is_undefined());
    }
    BOOST_TEST_EQ(text(d.root().splice(0, 1)), "[]");
    BOOST_TEST_EQ(text(d.root().splice(-1)), "[]");
    BOOST_TEST_EQ(text(d.root().slice(-1)), "[]");
    BOOST_TEST_EQ(text(d.root().concat()), "[]");
    wd::value root = d.root();
    BOOST_TEST(root.at(0).is_undefined());
    BOOST_TEST(root.at(-1).is_undefined());
    BOOST_TEST(root.reverse().has_value());
    BOOST_TEST(root.set_length(0).has_value());
    BOOST_TEST_EQ(text(d), "[]");
    BOOST_TEST_EQ(root.unshift(1).value(), 1U);
    BOOST_TEST_EQ(text(d), "[1]");
}

// Every method on a value that is not an array, and on a place that reads undefined, fails with
// type and changes nothing.
void anything_but_an_array_is_a_type_error() {
    wd::document d = document_of(wd::object{
        {"o", wd::object{{"k", 1}}},
        {"n", 1},
        {"s", "abc"},
        {"z", nullptr},
        {"b", true},
    });
    const std::string before = text(d);
    const auto refused = [&](wd::value v) {
        BOOST_TEST(v.push(1).error() == wd::errc::type);
        BOOST_TEST(v.pop().error() == wd::errc::type);
        BOOST_TEST(v.shift().error() == wd::errc::type);
        BOOST_TEST(v.unshift(1).error() == wd::errc::type);
        BOOST_TEST(v.splice(0, 1, 1).error() == wd::errc::type);
        BOOST_TEST(v.splice(0).error() == wd::errc::type);
        BOOST_TEST(v.reverse().error() == wd::errc::type);
        BOOST_TEST(v.set_length(1).error() == wd::errc::type);
        BOOST_TEST(v.slice().error() == wd::errc::type);
        BOOST_TEST(v.concat(wd::array{1}).error() == wd::errc::type);
        BOOST_TEST(v.at(0).is_undefined());
        BOOST_TEST(v.at(-1).is_undefined());
    };
    refused(d.root());
    refused(d["o"]);
    refused(d["n"]);
    refused(d["s"]);
    refused(d["z"]);
    refused(d["b"]);
    refused(d["absent"]);
    refused(d["o"]["k"]["deeper"]);
    BOOST_TEST_EQ(text(d), before);

    // A moved-from document has nothing to change.
    const wd::document taken = std::move(d);
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move): the state it leaves
    refused(d.root());
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    BOOST_TEST_EQ(text(taken), before);

    // Pushing a moved-from document fails too, and pushes nothing.
    wd::document list = five();
    wd::document gone = make_document();
    const wd::document holder = std::move(gone);
    wd::value list_root = list.root();
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move): the state it leaves
    BOOST_TEST(list_root.push(1, gone).error() == wd::errc::type);
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    BOOST_TEST_EQ(text(list), "[0,1,2,3,4]");
}

// A value that cannot be stored fails the whole call: nothing is pushed or spliced.
void a_value_refused_changes_nothing() {
    wd::document d = five();
    const std::string invalid("\xff");
    wd::value root = d.root();
    BOOST_TEST(root.push(5, invalid).error() == wd::errc::type);
    BOOST_TEST(root.unshift(invalid).error() == wd::errc::type);
    BOOST_TEST(root.splice(0, 2, 5, invalid).error() == wd::errc::type);
    BOOST_TEST(root.concat(5, invalid).error() == wd::errc::type);
    BOOST_TEST_EQ(text(d), "[0,1,2,3,4]");
}

// A handle kept while pushes make the owned image double, and move, many times.
void push_keeps_handles_across_growth() {
    wd::document d = document_of(wd::object{{"list", wd::array{}}, {"name", "x"}});
    wd::value list = d["list"];
    wd::value name = d["name"];
    const std::size_t first = d.image().size();
    constexpr std::uint64_t count = 20000;
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::string s = "item-" + std::to_string(i) + std::string(24, 'x');
        const auto pushed = list.push(s);
        if (!BOOST_TEST(pushed.has_value()) || !BOOST_TEST_EQ(*pushed, i + 1)) {
            return;
        }
    }
    BOOST_TEST_GT(d.image().size(), first * 64);
    BOOST_TEST_EQ(list.size(), count);
    BOOST_TEST_EQ(name.get<std::string_view>().value(), "x");
    BOOST_TEST_EQ(list.at(-1).get<std::string>().value(), "item-19999" + std::string(24, 'x'));
    BOOST_TEST_EQ(list[0].get<std::string>().value(), "item-0" + std::string(24, 'x'));
    // Plain literals in the macros' arguments: MSVC 14.3 splits those arguments without reading a
    // raw string as one, and an escape after it then stands outside any literal.
    BOOST_TEST_EQ(text(list.pop()), "\"item-19999" + std::string(24, 'x') + "\"");
    const auto removed = list.splice(1, 100);
    if (BOOST_TEST(removed.has_value())) {
        BOOST_TEST_EQ(removed->root().size(), 100U);
        BOOST_TEST_EQ(removed->root().at(-1).get<std::string>().value(),
                      "item-100" + std::string(24, 'x'));
    }
    BOOST_TEST_EQ(list.size(), count - 101);
    BOOST_TEST(list.set_length(2).has_value());
    BOOST_TEST_EQ(text(list), "[\"item-0" + std::string(24, 'x') + "\",\"item-101" +
                                  std::string(24, 'x') + "\"]");
    BOOST_TEST(name.set("y").has_value());
    BOOST_TEST_EQ(text(d["name"]), R"("y")");
}

// The text of [0,...,99], with `inserted` in place of 50 to 59 when it holds any.
std::string hundred(std::span<const std::string> inserted) {
    std::string out = "[";
    for (std::int64_t i = 0; i < 100; ++i) {
        if (i == 50) {
            for (const std::string& s : inserted) {
                out += '"' + s + "\",";
            }
        }
        if (inserted.empty() || i < 50 || i >= 60) {
            out += std::to_string(i) + ',';
        }
    }
    out.back() = ']';
    return out;
}

template <class Storage, std::size_t... I>
wd::result<wd::document> splice_strings(wd::basic_value<wd::default_traits, Storage> v,
                                        const std::array<std::string, 20>& s,
                                        std::index_sequence<I...> /*unused*/) {
    return v.splice(50, 10, s[I]...);
}

// Every buffer from the smallest that holds [0,...,99] to the first where the splice completes:
// the splice either completes or fails with capacity, leaving the array and the image's header as
// they were.
void splice_in_every_size() {
    wd::document source = document_of(wd::array{});
    wd::value source_root = source.root();
    for (std::int64_t i = 0; i < 100; ++i) {
        webcpp::test::require(BOOST_TEST(source_root.push(i).has_value()));
    }
    std::array<std::string, 20> inserted;
    for (std::size_t i = 0; i < inserted.size(); ++i) {
        inserted[i] = std::string(64, static_cast<char>('a' + i));
    }
    const std::string expected_before = hundred({});
    const std::string expected_after = hundred(inserted);

    std::size_t held = 0;
    std::size_t failed = 0;
    bool spliced = false;
    for (std::size_t size = 64; size <= std::size_t{1024} * 1024; size += 16) {
        const aligned_buffer buffer(size);
        auto made = wd::format(buffer.bytes());
        if (!made) {
            BOOST_TEST(made.error() == wd::errc::capacity);
            continue;
        }
        wd::basic_document<wd::default_traits, wd::external>& doc = *made;
        auto root = doc.root();
        if (!root.set(source).has_value()) {
            BOOST_TEST_EQ(held, 0U);
            continue;
        }
        ++held;
        const std::vector<std::byte> header(doc.image().begin(), doc.image().begin() + 64);
        const auto removed =
            splice_strings(doc.root(), inserted, std::make_index_sequence<inserted.size()>());
        if (removed) {
            BOOST_TEST_EQ(text(*removed), "[50,51,52,53,54,55,56,57,58,59]");
            BOOST_TEST_EQ(text(doc.root()), expected_after);
            spliced = true;
            break;
        }
        ++failed;
        BOOST_TEST(removed.error() == wd::errc::capacity);
        BOOST_TEST_EQ(text(doc.root()), expected_before);
        BOOST_TEST(std::ranges::equal(header, doc.image().first(64)));
    }
    BOOST_TEST(spliced);
    BOOST_TEST_EQ(held, failed + 1);
    BOOST_TEST_GT(failed, 50U);
}

}  // namespace

int main() {
    relative_index_follows_ecma_262();
    splice_removes_and_inserts();
    splice_at_the_index_limits();
    splice_copies_before_it_changes();
    slice_copies();
    push_pop_shift_unshift();
    reverse_reverses();
    concat_spreads_arrays();
    at_counts_from_the_end();
    set_length_truncates_and_fills();
    an_empty_array();
    anything_but_an_array_is_a_type_error();
    a_value_refused_changes_nothing();
    push_keeps_handles_across_growth();
    splice_in_every_size();
    return boost::report_errors();
}
