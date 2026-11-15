// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The parser's working state in the document's allocator. A text given whole, to parse, to
// parse_into or to a parser's finish, with selectors or without, under both traits and with the
// revivers, makes no call of the global operator new, and the resource gets back all it gave. A
// text fed in chunks calls it only for Boost.JSON's suspension stack, exactly as often as that
// stack's rule says. finish(text) is write(text) then finish(), its errors, offsets and causes
// included. With exceptions, an allocator that runs out at any step is errc::capacity, with no
// document, and a selector it cannot hold is not added.

#include <webcpp/document.hpp>

#include <boost/core/lightweight_test.hpp>

#include "counting.hpp"
#include "parse_corpus.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

namespace wd = webcpp::document;

namespace {

using webcpp::test::arena;
using webcpp::test::global_count;
using webcpp::test::require;

constexpr std::size_t mebibyte = std::size_t{1} << 20U;

/** A parser of the traits `Traits` whose working state is in a memory resource. */
template <class Traits>
using pmr_parser = wd::basic_parser<Traits, wd::pmr::owned>;

/** Whether two documents' images are the same bytes; it allocates nothing. */
template <class A, class B>
bool same_image(const A& a, const B& b) noexcept {
    return std::ranges::equal(a.image(), b.image());
}

/** Text as JSON writes it, and as a parse reads it. */
struct text_pair {
    /** The text in JSON, escapes among its characters. */
    std::string json;

    /** The text it reads as, in UTF-8. */
    std::string read;
};

/**
 A key or a string of more than 100 bytes, longer than any small-string buffer, whose JSON holds
 every kind of escape and raw multi-byte characters; `tag` tells keys of one object apart.
*/
text_pair long_text(char tag) {
    text_pair made;
    for (std::size_t i = 0; i < 4; ++i) {
        made.json += tag;
        made.json += R"( quote \" backslash \\ newline \n tab \t e é smile 😀 )";
        made.json += "raw \xc3\xa9\xe2\x82\xac ";
        made.read += tag;
        made.read += " quote \" backslash \\ newline \n tab \t e \xc3\xa9 smile \xf0\x9f\x98\x80 ";
        made.read += "raw \xc3\xa9\xe2\x82\xac ";
    }
    return made;
}

/** The selector of a member of the root by its key, as read: its `'` and `\` escaped. */
std::string member_selector(std::string_view key) {
    std::string selector = "$['";
    for (const char c : key) {
        if (c == '\'' || c == '\\') {
            selector += '\\';
        }
        selector += c;
    }
    selector += "']";
    return selector;
}

/** Arrays and objects in turn, `depth` of them, around a number: `[{"k":[{"k":...1...}]}]`. */
std::string nested(std::size_t depth) {
    std::string open;
    std::string close;
    for (std::size_t i = 0; i < depth; ++i) {
        if (i % 2 == 0) {
            open += '[';
            close.insert(close.begin(), ']');
        } else {
            open += R"({"k":)";
            close.insert(close.begin(), '}');
        }
    }
    return open + "1" + close;
}

/** Base64 text of `count` bytes, a multiple of three, which revive_bytes turns into bytes. */
std::string base64_of(std::size_t count) {
    // Every three bytes "foo" are "Zm9v".
    std::string text;
    for (std::size_t i = 0; i < count / 3; ++i) {
        text += "Zm9v";
    }
    return text;
}

/** An object of the shape JSON.stringify gives a Uint8Array of `count` bytes. */
std::string javascript_bytes_of(std::size_t count) {
    std::string text = "{";
    for (std::size_t i = 0; i < count; ++i) {
        text += i == 0 ? "\"" : ",\"";
        text += std::to_string(i);
        text += "\":";
        text += std::to_string(i % 256);
    }
    text += '}';
    return text;
}

/** A text to parse, with the options it is parsed with, and its name for a failure's message. */
struct sample {
    /** What the text holds. */
    std::string name;

    /** The text. */
    std::string text;

    /** The options. */
    wd::parse_options options;
};

/** The options with dates revived. */
wd::parse_options reviving_dates() {
    wd::parse_options o;
    o.revive_dates = true;
    return o;
}

/** The options with bytes revived. */
wd::parse_options reviving_bytes() {
    wd::parse_options o;
    o.revive_bytes = true;
    return o;
}

/** The texts every parse of a whole text reads, under both traits. */
std::vector<sample> samples() {
    const text_pair key = long_text('k');
    const text_pair other = long_text('o');
    const text_pair value = long_text('v');
    std::vector<sample> made;
    made.push_back({
        .name = "an object",
        .text = R"({"name":"document","list":[1,2.5,"three",[true,null]],"x":{}})",
        .options = wd::parse_options(),
    });
    made.push_back({
        .name = "arrays",
        .text = R"([[],[{}],[1,[2,[3,[4]]]],"",0,[true,false,null]])",
        .options = wd::parse_options(),
    });
    made.push_back({
        .name = "nesting 50 deep",
        .text = nested(50),
        .options = wd::parse_options(),
    });
    made.push_back({
        .name = "numbers",
        .text = "[0,-0,1,-1,1.5,-2.25e-3,1e400,123456789012345678,"
                "-9223372036854775808,9223372036854775808,0.1,"
                "12345678901234567890123456789012]",
        .options = wd::parse_options(),
    });
    made.push_back({
        .name = "long strings and keys",
        .text = R"({")" + key.json + R"(":")" + value.json + R"(",")" + other.json + R"(":[")" +
                value.json + R"(",{")" + key.json + R"(":1}],")" + key.json + R"(":"replaced"})",
        .options = wd::parse_options(),
    });
    made.push_back({
        .name = "dates revived",
        .text = R"(["2026-10-12T03:04:05.678Z","2026-10-12",)"
                R"({"when":"1970-01-01T00:00:00.000Z"}])",
        .options = reviving_dates(),
    });
    made.push_back({
        .name = "bytes revived",
        .text = R"(["Zm9v","Zm9v!",")" + base64_of(150) + R"(",{"0":1}])",
        .options = reviving_bytes(),
    });
    made.push_back({
        .name = "javascript bytes revived",
        .text = R"({"u":{"0":102,"1":111,"2":111},"gap":{"0":1,"2":2},"big":{"0":256},)"
                R"("s":"Zm9v","long":)" +
                javascript_bytes_of(120) + "}",
        .options = reviving_bytes(),
    });
    return made;
}

/** Names the sample a failed check was about, outside any counted region. */
void name_on_failure(bool held, const sample& s) {
    if (!held) {
        std::cerr << "    in the sample: " << s.name << '\n';
    }
}

/** A buffer of an external document, 16-aligned, made before anything is counted. */
struct block {
    /** The bytes. */
    alignas(16) std::array<std::byte, std::size_t{512} * 1024> bytes;
};

// parse of a whole text in a resource calls no global operator new: the image and every byte of
// the parse's working state come from the resource, which gets all of it back once the document
// is gone; and the image is the one the default storage makes.
template <class Traits>
void parse_calls_no_global_new(const sample& s) {
    const auto reference = wd::parse<Traits>(s.text, s.options);
    require(BOOST_TEST(reference.has_value()));
    arena a(16 * mebibyte);
    bool held = true;
    std::uint64_t news = 0;
    {
        const global_count global;
        {
            const auto made = wd::parse<Traits>(s.text, s.options, a.allocator());
            held = BOOST_TEST(made.has_value()) && BOOST_TEST(same_image(*made, *reference));
        }
        news = global.news();
    }
    held = BOOST_TEST_EQ(news, 0U) && held;
    held = BOOST_TEST_GT(a.resource().allocations(), 0U) && held;
    name_on_failure(BOOST_TEST_EQ(a.resource().bytes_in_use(), 0U) && held, s);
}

// parse_into of a whole text calls no global operator new: its working state comes from the
// resource, which gets all of it back.
template <class Traits>
void parse_into_calls_no_global_new(const sample& s) {
    const auto buffers = std::make_unique<std::array<block, 2>>();
    const auto reference = wd::parse_into<Traits>((*buffers)[0].bytes, s.text, s.options);
    require(BOOST_TEST(reference.has_value()));
    arena a(16 * mebibyte);
    bool held = true;
    std::uint64_t news = 0;
    {
        const global_count global;
        {
            const auto made =
                wd::parse_into<Traits>((*buffers)[1].bytes, s.text, s.options, a.allocator());
            held = BOOST_TEST(made.has_value()) && BOOST_TEST(same_image(*made, *reference));
        }
        news = global.news();
    }
    held = BOOST_TEST_EQ(news, 0U) && held;
    name_on_failure(BOOST_TEST_EQ(a.resource().bytes_in_use(), 0U) && held, s);
}

/** The selectors a parser registers, in the order it registers them. */
enum selector_index : std::uint8_t { items, meta, deep, members, long_member, selector_count };

/** What the selectors of a parser were given, counted without allocating. */
struct tally {
    /** The deliveries to each selector. */
    std::array<std::uint64_t, selector_count> deliveries{};

    /** The deliveries whose path or value was not the one expected. */
    std::uint64_t wrong = 0;

    friend bool operator==(const tally&, const tally&) = default;
};

/** The text the selectors read, and what they select, made before anything is counted. */
struct selected {
    /** The text. */
    std::string text;

    /** The long key, read. */
    std::string key;

    /** The selector of the root's member of the long key. */
    std::string long_member;

    /** The selector of the deep value under the long key. */
    std::string deep;
};

/** The text the selectors read: metadata, items, and a long key holding a deep value. */
selected selected_text() {
    const text_pair key = long_text('k');
    const text_pair value = long_text('v');
    selected made;
    made.text = R"({"meta":{"n":2,")" + key.json + R"(":")" + value.json +
                R"("},"items":[{"id":0,")" + key.json + R"(":[1,2]},{"id":1},{"id":2}],")" +
                key.json + R"(":{"deep":[[[7]]],"s":")" + value.json + R"("}})";
    made.key = key.read;
    made.long_member = member_selector(key.read);
    made.deep = made.long_member + ".deep[0][0][*]";
    return made;
}

/** Counts the one value the deep selector delivers, at the end of a path through the long key. */
template <class Value>
void count_deep(tally& t, const selected& s, const Value& v, wd::path_view where) {
    ++t.deliveries[deep];
    if (where.size() != 5 || where.key(0) != s.key || where.key(1) != "deep" ||
        where.index(4) != 0 || v.value_or(std::int64_t{0}) != 7) {
        ++t.wrong;
    }
}

/**
 Registers selectors of every kind on `p`, which count what they are given in `t`: `on` and
 `observe`, callbacks returning `void` and `result<void>`, selectors with a long key; returns
 which were added.
*/
template <class Parser>
std::array<bool, selector_count> subscribe(Parser& p, tally& t, const selected& s) {
    std::array<bool, selector_count> added{};
    added[items] =
        p.on("$.items[*]", [&t](const auto& v, wd::path_view where) {
             if (where.size() != 2 || where.key(0) != "items" ||
                 v["id"].value_or(std::int64_t{-1}) != static_cast<std::int64_t>(where.index(1))) {
                 ++t.wrong;
             }
             ++t.deliveries[items];
         }).has_value();
    added[meta] = p.observe("$.meta", [&t](const auto& v, wd::path_view) -> wd::result<void> {
                       if (v["n"].value_or(std::int64_t{0}) != 2) {
                           ++t.wrong;
                       }
                       ++t.deliveries[meta];
                       return {};
                   }).has_value();
    added[deep] = p.observe(s.deep, [&t, &s](const auto& v, wd::path_view where) {
                       count_deep(t, s, v, where);
                   }).has_value();
    added[members] = p.observe("$.*.*", [&t](const auto&, wd::path_view) {
                          ++t.deliveries[members];
                      }).has_value();
    added[long_member] = p.observe(s.long_member, [&t, &s](const auto&, wd::path_view where) {
                              if (where.key(0) != s.key) {
                                  ++t.wrong;
                              }
                              ++t.deliveries[long_member];
                          }).has_value();
    return added;
}

/** Whether every selector was added. */
bool all_added(const std::array<bool, selector_count>& added) noexcept {
    return std::ranges::all_of(added, [](bool b) { return b; });
}

/** What the selectors are given while a parser of the default storage reads the selected text. */
template <class Traits>
tally selected_tally() {
    const selected s = selected_text();
    tally t;
    wd::basic_parser<Traits> p;
    require(BOOST_TEST(all_added(subscribe(p, t, s))));
    require(BOOST_TEST(p.finish(s.text).has_value()));
    // The items, taken; the metadata; the deep value; the members of meta and of the long key's
    // object; the long key's object.
    BOOST_TEST((t == tally{.deliveries = {3, 1, 1, 4, 1}, .wrong = 0}));
    return t;
}

// A pmr parser registers selectors and is given a whole text by finish(text): registration,
// parsing and deliveries call no global operator new; the selectors, their callbacks, the path and
// its long keys are in the resource, which gets all of it back.
template <class Traits>
void selectors_call_no_global_new() {
    const selected s = selected_text();
    const tally expected = selected_tally<Traits>();
    tally reference_tally;
    wd::basic_parser<Traits> reference_parser;
    require(BOOST_TEST(all_added(subscribe(reference_parser, reference_tally, s))));
    const auto reference = reference_parser.finish(s.text);
    require(BOOST_TEST(reference.has_value()));

    arena a(16 * mebibyte);
    tally t;
    std::uint64_t news = 0;
    {
        const global_count global;
        {
            pmr_parser<Traits> p({}, a.allocator());
            BOOST_TEST(all_added(subscribe(p, t, s)));
            const auto made = p.finish(s.text);
            BOOST_TEST(made.has_value() && same_image(*made, *reference));
        }
        news = global.news();
    }
    BOOST_TEST_EQ(news, 0U);
    BOOST_TEST(t == expected);
    BOOST_TEST_GT(a.resource().allocations(), 0U);
    BOOST_TEST_EQ(a.resource().bytes_in_use(), 0U);
}

/**
 How many times Boost.JSON allocates its suspension stack, in the global heap, while a parser is
 written `text` in chunks of `size` bytes.

 Boost.JSON's parser suspends at the end of each chunk, and reserves its stack for the depth it
 suspends at whenever the stack it holds is smaller: so once at the first suspension, then at each
 suspension deeper than all before it. Its depth is that of the arrays and objects open at the
 chunk's end, a bracket inside a string being none.
*/
std::uint64_t stack_allocations(std::string_view text, std::size_t size) {
    std::uint64_t allocations = 0;
    std::int64_t deepest = -1;
    std::int64_t depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (escaped) {
            escaped = false;
        } else if (quoted) {
            escaped = c == '\\';
            quoted = c != '"';
        } else if (c == '"') {
            quoted = true;
        } else if (c == '[' || c == '{') {
            ++depth;
        } else if (c == ']' || c == '}') {
            --depth;
        }
        const bool chunk_ends = (i + 1) % size == 0 || i + 1 == text.size();
        if (chunk_ends && depth > deepest) {
            ++allocations;
            deepest = depth;
        }
    }
    return allocations;
}

/**
 Writes `text` to `p` in chunks of `size` bytes, then finishes it; returns whether every write
 succeeded and the document it finished with is `reference`.
*/
template <class Parser, class Document>
bool write_in_chunks(Parser& p, std::string_view text, std::size_t size,
                     const Document& reference) {
    for (std::size_t at = 0; at < text.size(); at += size) {
        if (!p.write(text.substr(at, size)).has_value()) {
            return false;
        }
    }
    const auto made = p.finish();
    return made.has_value() && same_image(*made, reference);
}

/**
 Feeds `text` in chunks of 1, 7 and 64 bytes to a parser in a resource, with the selectors or
 without, and checks that it calls the global operator new exactly as often as Boost.JSON's
 suspension stack is reserved, frees each of those but the last at the next and the last with the
 parser, and has everything else in the resource, which gets all of it back; the selectors are
 given what they are given for the whole text.
*/
template <class Traits>
void chunks_call_only_the_stack(std::string_view text, bool selectors) {
    const selected s = selected_text();
    tally reference_tally;
    wd::basic_parser<Traits> reference_parser;
    if (selectors) {
        require(BOOST_TEST(all_added(subscribe(reference_parser, reference_tally, s))));
    }
    const auto reference = reference_parser.finish(text);
    require(BOOST_TEST(reference.has_value()));
    for (const std::size_t size : {std::size_t{1}, std::size_t{7}, std::size_t{64}}) {
        const std::uint64_t expected = stack_allocations(text, size);
        arena a(64 * mebibyte);
        tally t;
        std::uint64_t news = 0;
        std::uint64_t deletes_while_parsing = 0;
        std::uint64_t deletes = 0;
        {
            const global_count global;
            {
                pmr_parser<Traits> p({}, a.allocator());
                if (selectors) {
                    BOOST_TEST(all_added(subscribe(p, t, s)));
                }
                BOOST_TEST(write_in_chunks(p, text, size, *reference));
                deletes_while_parsing = global.deletes();
            }
            news = global.news();
            deletes = global.deletes();
        }
        BOOST_TEST_EQ(news, expected);
        BOOST_TEST_EQ(deletes_while_parsing, expected - 1);
        BOOST_TEST_EQ(deletes, expected);
        BOOST_TEST(t == reference_tally);
        BOOST_TEST_GT(a.resource().allocations(), 0U);
        BOOST_TEST_EQ(a.resource().bytes_in_use(), 0U);
    }
}

// The rule of the stack, on the text nested 50 deep, 101 bytes: in chunks of 1 byte the parser
// suspends at a new depth 50 times; in chunks of 7 at depths 7, 14, ..., 49 before the text turns
// back; in chunks of 64 once, at depth 37. A text of one level in chunks of 1 byte: once; brackets
// in strings count for nothing.
void the_stack_rule() {
    const std::string deep = std::string(50, '[') + "1" + std::string(50, ']');
    BOOST_TEST_EQ(stack_allocations(deep, 1), 50U);
    BOOST_TEST_EQ(stack_allocations(deep, 7), 7U);
    BOOST_TEST_EQ(stack_allocations(deep, 64), 1U);
    BOOST_TEST_EQ(stack_allocations("[1,2,3]", 1), 1U);
    BOOST_TEST_EQ(stack_allocations(R"(["[[[",{"a":"]\"]"}])", 1), 2U);
    chunks_call_only_the_stack<wd::default_traits>(deep, false);
}

/**
 Checks that `finish(text)` gives what `write(text)` then `finish()` give: the same document or
 error, offset and cause, and the same answer to a later call.
*/
template <class Traits>
void finish_is_write_then_finish(std::string_view text, const wd::parse_options& o) {
    wd::basic_parser<Traits> whole(o);
    const auto at_once = whole.finish(text);
    wd::basic_parser<Traits> split(o);
    const wd::result<void> written = split.write(text);
    const auto in_two = written.has_value()
                            ? split.finish()
                            : wd::result<wd::basic_document<Traits>>(written.error());
    bool held = BOOST_TEST_EQ(at_once.has_value(), in_two.has_value());
    if (held && at_once.has_value()) {
        held = BOOST_TEST(same_image(*at_once, *in_two));
    } else if (held) {
        held = BOOST_TEST(at_once.error() == in_two.error());
    }
    held = BOOST_TEST_EQ(whole.offset(), split.offset()) && held;
    held = BOOST_TEST(whole.cause() == split.cause()) && held;
    held = BOOST_TEST(whole.write("1").error() == split.write("1").error()) && held;
    if (!held) {
        std::cerr << "    in the text: " << text << '\n';
    }
}

// finish(text) is write(text) then finish(), for every prefix of every text of the corpus, which
// makes every error of a text that ends early, and for texts that fail otherwise: malformed, a
// second text, a number past the traits, past the depth, past the bytes.
template <class Traits>
void finish_is_write_then_finish() {
    for (const std::string& text : webcpp::test::parse_corpus()) {
        for (std::size_t length = 0; length <= text.size(); ++length) {
            finish_is_write_then_finish<Traits>(std::string_view(text).substr(0, length), {});
        }
    }
    for (const std::string_view text : {
             "[1,]",
             "[1,2",
             "1 2",
             R"({"a":1}{"b":2})",
             "1x",
             "[1] ]",
             "",
             " ",
             "nul",
             "tru",
             R"("\ud800")",
             R"(["\u12)",
             "[1e999999999999]",
             "12345678901234567890123456789012345",
             "-",
             "[-]",
             "{\"a\"",
             "{\"a\":",
             "[1,2,3,456]",
             "[[[[[1]]]]]",
         }) {
        finish_is_write_then_finish<Traits>(text, {});
    }
    wd::parse_options limited;
    limited.max_bytes = 10;
    limited.max_depth = 4;
    for (const std::string_view text :
         {"[1,2,3,45]", "[1,2,3,456]", "[[[[1]]]]", "[[[[[1]]]]]", "[[[[1]]]] ", "[1,2,3,4] x"}) {
        finish_is_write_then_finish<Traits>(text, limited);
    }
}

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS

/** An allocator of `resource`. */
std::pmr::polymorphic_allocator<std::byte> allocator_of(std::pmr::memory_resource& resource) {
    return {&resource};
}

/** More allocations than any parse here makes. */
constexpr std::uint64_t plenty = std::uint64_t{1} << 40U;

// A parse whose allocator runs out at any allocation is errc::capacity, with no document and
// every block given back; given room for every allocation it parses the text, refusing none.
template <class Traits>
void parse_runs_out(const sample& s) {
    const auto reference = wd::parse<Traits>(s.text, s.options);
    require(BOOST_TEST(reference.has_value()));
    std::uint64_t failed = 0;
    for (std::uint64_t budget = 0;; ++budget) {
        webcpp::test::failing_resource failing(std::pmr::new_delete_resource(), budget);
        bool parsed = false;
        bool held = true;
        {
            const auto made = wd::parse<Traits>(s.text, s.options, allocator_of(failing));
            if (made.has_value()) {
                parsed = true;
                held = BOOST_TEST(same_image(*made, *reference));
                held = BOOST_TEST_EQ(failing.refusals(), 0U) && held;
            } else {
                ++failed;
                held = BOOST_TEST(made.error() == wd::errc::capacity);
                held = BOOST_TEST_GT(failing.refusals(), 0U) && held;
            }
        }
        name_on_failure(BOOST_TEST_EQ(failing.bytes_in_use(), 0U) && held, s);
        if (parsed) {
            break;
        }
    }
    name_on_failure(BOOST_TEST_GT(failed, 0U), s);
}

// parse_into whose allocator runs out at any allocation is errc::capacity, with no document and
// every block given back.
template <class Traits>
void parse_into_runs_out(const sample& s) {
    const auto buffers = std::make_unique<std::array<block, 2>>();
    const auto reference = wd::parse_into<Traits>((*buffers)[0].bytes, s.text, s.options);
    require(BOOST_TEST(reference.has_value()));
    std::uint64_t failed = 0;
    for (std::uint64_t budget = 0;; ++budget) {
        webcpp::test::failing_resource failing(std::pmr::new_delete_resource(), budget);
        bool parsed = false;
        bool held = true;
        {
            const auto made = wd::parse_into<Traits>((*buffers)[1].bytes, s.text, s.options,
                                                     allocator_of(failing));
            if (made.has_value()) {
                parsed = true;
                held = BOOST_TEST(same_image(*made, *reference));
            } else {
                ++failed;
                held = BOOST_TEST(made.error() == wd::errc::capacity);
            }
        }
        name_on_failure(BOOST_TEST_EQ(failing.bytes_in_use(), 0U) && held, s);
        if (parsed) {
            break;
        }
    }
    name_on_failure(BOOST_TEST_GT(failed, 0U), s);
}

/**
 Checks a parser whose registrations ran out: given room again, it reads the text, and each
 selector it added is given what it is given when all are, the others nothing.
*/
template <class Parser>
void refused_selectors_were_not_added(Parser& p, webcpp::test::failing_resource& failing,
                                      const std::array<bool, selector_count>& added, const tally& t,
                                      const tally& expected, std::string_view text) {
    failing.allow(plenty);
    BOOST_TEST(p.finish(text).has_value());
    for (std::size_t i = 0; i < selector_count; ++i) {
        BOOST_TEST_EQ(t.deliveries[i], added[i] ? expected.deliveries[i] : 0U);
    }
    BOOST_TEST_EQ(t.wrong, 0U);
}

/**
 Checks a parser whose allocator ran out while it read a text: it failed with errc::capacity and
 stays failed, whatever it is given, until it is reset.
*/
template <class Parser>
void a_parser_that_ran_out_stays_failed(Parser& p, webcpp::test::failing_resource& failing) {
    BOOST_TEST(p.write("").error() == wd::errc::capacity);
    BOOST_TEST(p.finish().error() == wd::errc::capacity);
    BOOST_TEST_GT(failing.refusals(), 0U);
    failing.allow(plenty);
    BOOST_TEST(p.write(" ").error() == wd::errc::capacity);
    BOOST_TEST(p.finish("[1]").error() == wd::errc::capacity);
    p.reset();
    BOOST_TEST(p.finish("[1]").has_value());
}

// A parser whose allocator runs out while it registers its selectors returns errc::capacity and
// adds nothing. Once they are registered, a parser whose allocator runs out while it reads the
// text, whole or in chunks of 7 bytes, is errc::capacity, and stays failed until reset.
template <class Traits>
void parser_runs_out(bool in_chunks) {
    const selected s = selected_text();
    const tally expected = selected_tally<Traits>();
    tally reference_tally;
    wd::basic_parser<Traits> reference_parser;
    require(BOOST_TEST(all_added(subscribe(reference_parser, reference_tally, s))));
    const auto reference = reference_parser.finish(s.text);
    require(BOOST_TEST(reference.has_value()));
    std::uint64_t failed = 0;
    for (std::uint64_t budget = 0;; ++budget) {
        webcpp::test::failing_resource failing(std::pmr::new_delete_resource(), budget);
        bool parsed = false;
        {
            tally t;
            pmr_parser<Traits> p({}, allocator_of(failing));
            const std::array<bool, selector_count> added = subscribe(p, t, s);
            if (!all_added(added)) {
                ++failed;
                refused_selectors_were_not_added(p, failing, added, t, expected, s.text);
            } else if (in_chunks ? write_in_chunks(p, s.text, 7, *reference)
                                 : p.finish(s.text).has_value()) {
                parsed = true;
                BOOST_TEST(t == expected);
                BOOST_TEST_EQ(failing.refusals(), 0U);
            } else {
                ++failed;
                a_parser_that_ran_out_stays_failed(p, failing);
            }
        }
        BOOST_TEST_EQ(failing.bytes_in_use(), 0U);
        if (parsed) {
            break;
        }
    }
    BOOST_TEST_GT(failed, 0U);
}

// A parse runs out at every allocation of each sample under both traits, and a parser at every
// allocation of its selectors and of its text, whole and in chunks.
void running_out() {
    for (const sample& s : samples()) {
        parse_runs_out<wd::default_traits>(s);
        parse_runs_out<wd::js_traits>(s);
        parse_into_runs_out<wd::default_traits>(s);
        parse_into_runs_out<wd::js_traits>(s);
    }
    parser_runs_out<wd::default_traits>(false);
    parser_runs_out<wd::js_traits>(false);
    parser_runs_out<wd::default_traits>(true);
    parser_runs_out<wd::js_traits>(true);
}

#endif  // WEBCPP_DOCUMENT_NO_EXCEPTIONS

}  // namespace

/** The allocations of a parse of `text` in an arena, with `revive_bytes` or without. */
template <class Traits>
std::uint64_t parse_allocations(std::string_view text, bool revive_bytes) {
    arena a(16 * mebibyte);
    wd::parse_options o;
    o.revive_bytes = revive_bytes;
    const auto made = wd::parse<Traits>(text, o, a.allocator());
    require(BOOST_TEST(made.has_value()));
    return a.resource().allocations();
}

// A parse allocates what a value needs only once it needs it: reviving bytes, text that is not
// base64 and objects of another shape than a Uint8Array's take no buffer, so that a parse that
// revives bytes allocates as often as one that does not; and the parser's prepared value keeps its
// buffers, so that strings transcoded to UTF-16 one after another allocate as often as the longest
// grows, not once each.
void allocations_when_needed() {
    std::string strings = "[";
    std::string objects = "[";
    for (std::size_t i = 0; i < 64; ++i) {
        strings += std::string(i == 0 ? "" : ",") + R"("not base64, )" + std::string(i, 'x') + '"';
        objects += std::string(i == 0 ? "" : ",") + R"({"a":1,"0":2})";
    }
    strings += "]";
    objects += "]";
    BOOST_TEST_EQ(parse_allocations<wd::default_traits>(strings, true),
                  parse_allocations<wd::default_traits>(strings, false));
    BOOST_TEST_EQ(parse_allocations<wd::js_traits>(objects, true),
                  parse_allocations<wd::js_traits>(objects, false));

    arena a(mebibyte);
    wd::detail::prepared<wd::js_traits, std::pmr::polymorphic_allocator<std::byte>> p(
        a.allocator());
    const std::string longest(200, 'u');
    BOOST_TEST(wd::detail::prepare_string(longest, false, false, p).has_value());
    const std::uint64_t first = a.resource().allocations();
    BOOST_TEST_GT(first, 0U);
    for (std::size_t size = 0; size <= longest.size(); size += 10) {
        BOOST_TEST(
            wd::detail::prepare_string(std::string_view(longest).substr(0, size), false, false, p)
                .has_value());
        BOOST_TEST(p.value().text.size() == size * sizeof(char16_t));
    }
    BOOST_TEST_EQ(a.resource().allocations(), first);

    wd::detail::prepared<wd::default_traits, std::pmr::polymorphic_allocator<std::byte>> b(
        a.allocator());
    const std::uint64_t before = a.resource().allocations();
    BOOST_TEST(wd::detail::prepare_string("not base64!", false, true, b).has_value());
    BOOST_TEST(b.value().kind == wd::kind::string);
    BOOST_TEST_EQ(a.resource().allocations(), before);
    BOOST_TEST(wd::detail::prepare_string("aGVsbG8=", false, true, b).has_value());
    BOOST_TEST(b.value().kind == wd::kind::bytes);
    BOOST_TEST_EQ(b.value().text.size(), 5U);
    const std::uint64_t decoded = a.resource().allocations();
    BOOST_TEST(wd::detail::prepare_string("d29ybGQ=", false, true, b).has_value());
    BOOST_TEST_EQ(b.value().text.size(), 5U);
    BOOST_TEST_EQ(a.resource().allocations(), decoded);
}

int main() {
    const std::vector<sample> texts = samples();
    for (const sample& s : texts) {
        parse_calls_no_global_new<wd::default_traits>(s);
        parse_calls_no_global_new<wd::js_traits>(s);
        parse_into_calls_no_global_new<wd::default_traits>(s);
        parse_into_calls_no_global_new<wd::js_traits>(s);
    }
    selectors_call_no_global_new<wd::default_traits>();
    selectors_call_no_global_new<wd::js_traits>();
    const selected s = selected_text();
    chunks_call_only_the_stack<wd::default_traits>(s.text, true);
    chunks_call_only_the_stack<wd::js_traits>(s.text, true);
    chunks_call_only_the_stack<wd::default_traits>(texts[2].text, false);
    chunks_call_only_the_stack<wd::js_traits>(texts[4].text, false);
    the_stack_rule();
    finish_is_write_then_finish<wd::default_traits>();
    finish_is_write_then_finish<wd::js_traits>();
    allocations_when_needed();
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
    running_out();
#endif
    return boost::report_errors();
}
