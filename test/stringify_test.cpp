// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// stringify and stringify_to write JSON.stringify's exact bytes, each expected text measured with
// Node 26: indentation, empty containers, nesting, QuoteJSONString's escapes, numbers, dates, bytes
// under each policy, a lone surrogate under js_traits, an undefined root, the members, pieces of
// every size to a sink, a sink that fails or throws, and a document nested 100,000 levels deep.

#include <webcpp/document/document.hpp>
#include <webcpp/document/stringify.hpp>

#include <boost/core/lightweight_test.hpp>
#include <boost/decimal/decimal128_t.hpp>

#include "require.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
#include <stdexcept>
#endif

namespace wd = webcpp::document;

namespace {

using boost::decimal::decimal128_t;
using std::chrono::milliseconds;

using js_document = wd::basic_document<wd::js_traits>;

/** The default traits, but bytes written as base64url. */
struct url_traits : wd::default_traits {
    /** RFC 4648 section 5, no padding. */
    using bytes = wd::base64url;
};

using url_document = wd::basic_document<url_traits>;

template <class Document = wd::document>
Document make_document() {
    auto made = Document::create();
    webcpp::test::require(BOOST_TEST(made.has_value()));
    return std::move(*made);
}

// A document holding `v`, set at its root.
template <class Document = wd::document, class T>
Document document_of(const T& v) {
    auto d = make_document<Document>();
    auto root = d.root();
    webcpp::test::require(BOOST_TEST(root.set(v).has_value()));
    return d;
}

// A decimal from its text, exactly, as a program reads one.
decimal128_t dec(std::string_view text) {
    const auto parsed = wd::number_codec<decimal128_t>::parse(text);
    webcpp::test::require(BOOST_TEST(parsed.has_value()));
    return *parsed;
}

// x"\<BS><FF><LF><CR><TAB><U+0001><U+001F>/é<U+2028>😀, in UTF-8.
constexpr std::string_view plain_text =
    "x\"\\\b\f\n\r\t\x01\x1f/"
    "\xc3\xa9"
    "\xe2\x80\xa8"
    "\xf0\x9f\x98\x80";

// What JSON.stringify writes of it, between its quotes.
constexpr std::string_view escaped_text = R"(x\"\\\b\f\n\r\t\u0001\u001f/)"
                                          "\xc3\xa9"
                                          "\xe2\x80\xa8"
                                          "\xf0\x9f\x98\x80";

// An object holding arrays, objects, a string of every escape, null and true, in a document of
// type `Document`.
template <class Document>
Document sample() {
    return document_of<Document>(wd::object{
        {"b", wd::array{1, wd::object{{"c", wd::array()}}, wd::object()}},
        {"a", plain_text},
        {"e", nullptr},
        {"t", true},
    });
}

std::string sample_compact() {
    return R"({"b":[1,{"c":[]},{}],"a":")" + std::string(escaped_text) + R"(","e":null,"t":true})";
}

std::string sample_indented() {
    return "{\n  \"b\": [\n    1,\n    {\n      \"c\": []\n    },\n    {}\n  ],\n  \"a\": \"" +
           std::string(escaped_text) + "\",\n  \"e\": null,\n  \"t\": true\n}";
}

// The pieces stringify_to hands a sink, with a sink returning void.
template <class Handle>
std::vector<std::string> pieces_of(const Handle& v, std::uint32_t indent, std::uint64_t chunk) {
    std::vector<std::string> pieces;
    const auto written = wd::stringify_to(
        v, [&pieces](std::string_view piece) { pieces.emplace_back(piece); }, indent, chunk);
    webcpp::test::require(BOOST_TEST(written.has_value()));
    return pieces;
}

void writes_as_json_stringify() {
    const auto doc = sample<wd::document>();
    BOOST_TEST_EQ(wd::stringify(doc), sample_compact());
    BOOST_TEST_EQ(wd::stringify(doc, 2), sample_indented());
    // An indent of 1 nests by one space per level: the first 12 characters show two levels.
    BOOST_TEST_EQ(wd::stringify(doc, 1).substr(0, 12), "{\n \"b\": [\n  ");
    // Clamped, as JavaScript does.
    BOOST_TEST_EQ(wd::stringify(doc, 12), wd::stringify(doc, 10));
    BOOST_TEST_EQ(wd::stringify(doc, std::numeric_limits<std::uint32_t>::max()),
                  wd::stringify(doc, 10));
    BOOST_TEST_EQ(wd::stringify(doc, 10).substr(0, 15), "{\n          \"b\"");
}

void js_traits_write_the_same() {
    const auto doc = sample<js_document>();
    BOOST_TEST_EQ(wd::stringify(doc), sample_compact());
    BOOST_TEST_EQ(wd::stringify(doc, 2), sample_indented());
}

void the_members() {
    const auto doc = sample<wd::document>();
    BOOST_TEST_EQ(doc.stringify(), sample_compact());
    BOOST_TEST_EQ(doc.stringify(2), sample_indented());
    BOOST_TEST_EQ(doc.root().stringify(), sample_compact());
    // A subtree is indented from its own level, as JSON.stringify(doc.b, null, 2) is.
    BOOST_TEST_EQ(doc["b"].stringify(), R"([1,{"c":[]},{}])");
    BOOST_TEST_EQ(doc["b"].stringify(2), "[\n  1,\n  {\n    \"c\": []\n  },\n  {}\n]");
    BOOST_TEST_EQ(wd::stringify(doc["b"][1]), R"({"c":[]})");
    BOOST_TEST_EQ(wd::stringify(doc["a"]), "\"" + std::string(escaped_text) + "\"");
    BOOST_TEST_EQ(wd::stringify(doc["t"], 4), "true");
    auto writable = sample<wd::document>();
    // A handle that writes is written as the read-only one is.
    BOOST_TEST_EQ(wd::stringify(writable["b"]), R"([1,{"c":[]},{}])");
    BOOST_TEST_EQ(writable["b"].stringify(), R"([1,{"c":[]},{}])");
}

void empty_containers() {
    for (const std::uint32_t indent : {0U, 1U, 2U, 10U}) {
        BOOST_TEST_EQ(wd::stringify(document_of(wd::array()), indent), "[]");
        BOOST_TEST_EQ(wd::stringify(document_of(wd::object()), indent), "{}");
    }
    // The inner array is a literal: a lone element of the array's own type is read as a copy
    // where the compiler applies CWG 1467 to every class (MSVC, Clang before 20).
    BOOST_TEST_EQ(wd::stringify(document_of(wd::array{wd::literal(wd::array())}), 2), "[\n  []\n]");
    BOOST_TEST_EQ(wd::stringify(document_of(wd::array{wd::literal(wd::array())})), "[[]]");
    BOOST_TEST_EQ(wd::stringify(document_of(wd::object{{"a", wd::object()}})), R"({"a":{}})");
}

void nesting() {
    const wd::document doc =
        document_of(wd::object{{"a", wd::object{{"b", wd::array{1, wd::array{2, wd::object()}}}}}});
    BOOST_TEST_EQ(wd::stringify(doc), R"({"a":{"b":[1,[2,{}]]}})");
    BOOST_TEST_EQ(wd::stringify(doc, 4),
                  "{\n    \"a\": {\n        \"b\": [\n            1,\n            [\n"
                  "                2,\n                {}\n            ]\n        ]\n    }\n}");
    // Keys in JavaScript's order: indices first, ascending, then the others as inserted.
    const wd::document keys = document_of(wd::object{
        {"1", 1},
        {"0", 0},
        {"a", wd::array{wd::literal(wd::array())}},
        {"b", wd::object()},
    });
    BOOST_TEST_EQ(wd::stringify(keys, 3),
                  "{\n   \"0\": 0,\n   \"1\": 1,\n   \"a\": [\n      []\n   ],\n   \"b\": {}\n}");
}

void escapes() {
    // Every unit below U+0020, then U+007F, which is not escaped.
    std::string controls;
    for (char c = 0; c < 0x20; ++c) {
        controls.push_back(c);
    }
    controls.push_back('\x7f');
    const std::string expected =
        R"("\u0000\u0001\u0002\u0003\u0004\u0005\u0006\u0007\b\t\n\u000b\f\r\u000e\u000f)"
        R"(\u0010\u0011\u0012\u0013\u0014\u0015\u0016\u0017\u0018\u0019\u001a\u001b\u001c)"
        R"(\u001d\u001e\u001f)"
        "\x7f\"";
    BOOST_TEST_EQ(wd::stringify(document_of(std::string_view(controls))), expected);
    BOOST_TEST_EQ(wd::stringify(document_of<js_document>(std::string_view(controls))), expected);
    // A key is quoted as a string is.
    // A raw string that holds a quote and a backslash stays out of the macros' arguments: MSVC 14.3
    // splits them without reading it as one, and its escapes then stand outside any literal.
    constexpr std::string_view keyed_text = R"({"a\"\n\u0001":1})";
    const wd::document keyed = document_of(wd::object{{"a\"\n\x01", 1}});
    BOOST_TEST_EQ(wd::stringify(keyed), keyed_text);
    BOOST_TEST_EQ(wd::stringify(document_of<js_document>(wd::object{{"a\"\n\x01", 1}})),
                  keyed_text);
    BOOST_TEST_EQ(wd::stringify(document_of("")), R"("")");
    // A long string, past any buffer the writer keeps.
    const std::string long_text(10000, 'z');
    BOOST_TEST_EQ(wd::stringify(document_of(std::string_view(long_text))), '"' + long_text + '"');
    BOOST_TEST_EQ(wd::stringify(document_of<js_document>(std::string_view(long_text))),
                  '"' + long_text + '"');
}

void numbers() {
    BOOST_TEST_EQ(wd::stringify(document_of(dec("1.5"))), "1.5");
    BOOST_TEST_EQ(wd::stringify(document_of(dec("1e21"))), "1e+21");
    BOOST_TEST_EQ(wd::stringify(document_of(std::numeric_limits<std::int64_t>::min())),
                  "-9223372036854775808");
    BOOST_TEST_EQ(wd::stringify(document_of(std::numeric_limits<std::uint64_t>::max())),
                  "18446744073709551615");
    BOOST_TEST_EQ(wd::stringify(document_of(dec("-0"))), "0");
    BOOST_TEST_EQ(wd::stringify(document_of(dec("0.0000001"))), "1e-7");
    BOOST_TEST_EQ(wd::stringify(document_of(wd::array{0, -1, dec("123.45")}), 1),
                  "[\n 0,\n -1,\n 123.45\n]");
    BOOST_TEST_EQ(wd::stringify(document_of(false)), "false");
    BOOST_TEST_EQ(wd::stringify(document_of(nullptr)), "null");

    // With js_traits, numbers are doubles, and an integer is written as its double is.
    BOOST_TEST_EQ(wd::stringify(document_of<js_document>(1.5)), "1.5");
    BOOST_TEST_EQ(wd::stringify(document_of<js_document>(1e21)), "1e+21");
    BOOST_TEST_EQ(wd::stringify(document_of<js_document>(-0.0)), "0");
    BOOST_TEST_EQ(wd::stringify(document_of<js_document>(std::numeric_limits<std::int64_t>::min())),
                  "-9223372036854776000");
    BOOST_TEST_EQ(wd::stringify(document_of<js_document>(std::int64_t{4'611'686'018'427'387'904})),
                  "4611686018427388000");
    BOOST_TEST_EQ(wd::stringify(document_of<js_document>(std::int64_t{123456789012345678})),
                  "123456789012345680");
    BOOST_TEST_EQ(wd::stringify(document_of<js_document>(std::int64_t{42})), "42");
    BOOST_TEST_EQ(wd::stringify(document_of<js_document>(wd::array{
                      std::numeric_limits<double>::quiet_NaN(),
                      std::numeric_limits<double>::infinity(),
                  })),
                  "[null,null]");
}

void dates() {
    const auto date = [](std::int64_t ms) { return wd::date_ms{milliseconds{ms}}; };
    BOOST_TEST_EQ(wd::stringify(document_of(date(1791774245678))), R"("2026-10-12T03:04:05.678Z")");
    BOOST_TEST_EQ(wd::stringify(document_of(date(8'640'000'000'000'000))),
                  R"("+275760-09-13T00:00:00.000Z")");
    BOOST_TEST_EQ(wd::stringify(document_of(date(-8'640'000'000'000'000))),
                  R"("-271821-04-20T00:00:00.000Z")");
    BOOST_TEST_EQ(wd::stringify(document_of(date(-62198755200000))),
                  R"("-000001-01-01T00:00:00.000Z")");
    BOOST_TEST_EQ(wd::stringify(document_of<js_document>(date(-1))),
                  R"("1969-12-31T23:59:59.999Z")");
}

void bytes_by_policy() {
    const std::array<std::byte, 2> raw{std::byte{1}, std::byte{255}};
    const std::span<const std::byte> bytes(raw);
    BOOST_TEST_EQ(wd::stringify(document_of(bytes)), R"("Af8=")");
    BOOST_TEST_EQ(wd::stringify(document_of<url_document>(bytes)), R"("Af8")");
    BOOST_TEST_EQ(wd::stringify(document_of<js_document>(bytes)), R"({"0":1,"1":255})");

    // As any object is, with an indent, at its own level.
    auto doc = make_document<js_document>();
    BOOST_TEST(doc.root().set(wd::object()).has_value());
    BOOST_TEST(doc["b"].set(bytes).has_value());
    BOOST_TEST(doc["e"].set(std::span<const std::byte>()).has_value());
    BOOST_TEST_EQ(wd::stringify(doc, 2),
                  "{\n  \"b\": {\n    \"0\": 1,\n    \"1\": 255\n  },\n  \"e\": {}\n}");
    BOOST_TEST_EQ(wd::stringify(doc), R"({"b":{"0":1,"1":255},"e":{}})");
    BOOST_TEST_EQ(wd::stringify(document_of(std::span<const std::byte>())), R"("")");
    BOOST_TEST_EQ(wd::stringify(document_of<url_document>(std::span<const std::byte>())), R"("")");

    // Past any buffer the writer keeps, with each remainder of three.
    for (const std::size_t size : {299U, 300U, 301U, 1000U}) {
        std::vector<std::byte> many(size);
        for (std::size_t i = 0; i < size; ++i) {
            many[i] = static_cast<std::byte>((i * 7) % 256);
        }
        for (const bool url : {false, true}) {
            std::string expected(static_cast<std::size_t>(wd::detail::base64_size(size, !url)),
                                 '\0');
            wd::detail::write_base64(many, expected.data(), url);
            const std::string written =
                url ? wd::stringify(document_of<url_document>(std::span<const std::byte>(many)))
                    : wd::stringify(document_of(std::span<const std::byte>(many)));
            BOOST_TEST_EQ(written, '"' + expected + '"');
        }
    }
}

void a_lone_surrogate_under_js_traits() {
    const auto units = [](std::initializer_list<char16_t> list) { return std::u16string(list); };
    const auto written = [](const std::u16string& text) {
        return wd::stringify(document_of<js_document>(std::u16string_view(text)));
    };
    // Plain literals: MSVC 14.3 does not read a raw string in these macros' arguments as one.
    BOOST_TEST_EQ(written(units({0xD800})), "\"\\ud800\"");
    BOOST_TEST_EQ(written(units({0xDC00})), "\"\\udc00\"");
    BOOST_TEST_EQ(written(units({0xDC00, 0xD800})), "\"\\udc00\\ud800\"");
    BOOST_TEST_EQ(written(units({u'a', 0xDBFF, u'b'})), "\"a\\udbffb\"");
    BOOST_TEST_EQ(written(units({0xD83D, 0xDE00})), "\"\xf0\x9f\x98\x80\"");
    BOOST_TEST_EQ(written(units({0xD83D, 0xDE00, 0xD83D})), "\"\xf0\x9f\x98\x80\\ud83d\"");
    BOOST_TEST_EQ(written(units({0x7F, 0xE9, 0x20AC, 0xFFFF})),
                  "\"\x7f\xc3\xa9\xe2\x82\xac\xef\xbf\xbf\"");
}

void an_undefined_root() {
    wd::document doc = make_document();
    wd::value root = doc.root();
    BOOST_TEST(root.set(wd::undefined).has_value());
    BOOST_TEST_EQ(wd::stringify(doc), "");
    BOOST_TEST_EQ(wd::stringify(doc, 2), "");
    BOOST_TEST_EQ(doc.stringify(), "");
    BOOST_TEST(pieces_of(doc.root(), 0, 1).empty());

    // JSON.stringify(undefined), as a missing member and nothing read.
    const wd::document other = document_of(wd::object{{"a", 1}});
    BOOST_TEST_EQ(wd::stringify(other["missing"]), "");
    BOOST_TEST_EQ(wd::stringify(other["a"]["b"]), "");
    BOOST_TEST_EQ(wd::stringify(wd::const_value()), "");

    // A moved-from document holds nothing.
    wd::document from = document_of(wd::array{1});
    const wd::document to = std::move(from);
    BOOST_TEST_EQ(wd::stringify(from), "");  // NOLINT(bugprone-use-after-move)
    BOOST_TEST_EQ(wd::stringify(to), "[1]");
}

void stringify_to_every_chunk() {
    const auto doc = sample<wd::document>();
    for (const std::uint32_t indent : {0U, 2U}) {
        const std::string whole = wd::stringify(doc, indent);
        for (std::uint64_t chunk = 1; chunk <= 64; ++chunk) {
            const std::vector<std::string> pieces = pieces_of(doc.root(), indent, chunk);
            std::string joined;
            for (std::size_t i = 0; i < pieces.size(); ++i) {
                BOOST_TEST(!pieces[i].empty());
                BOOST_TEST_LE(pieces[i].size(), chunk);
                // Every piece but the last is full.
                if (i + 1 < pieces.size()) {
                    BOOST_TEST_EQ(pieces[i].size(), chunk);
                }
                joined += pieces[i];
            }
            BOOST_TEST_EQ(joined, whole);
        }
    }

    // A sink that returns a result, the document's overload, and the default chunk.
    std::string joined;
    std::size_t calls = 0;
    const auto written = wd::stringify_to(
        doc,
        [&](std::string_view piece) -> wd::result<void> {
            joined += piece;
            ++calls;
            return {};
        },
        2);
    BOOST_TEST(written.has_value());
    BOOST_TEST_EQ(joined, sample_indented());
    BOOST_TEST_EQ(calls, 1U);
    BOOST_TEST_EQ(wd::default_chunk, 4096U);

    // A chunk of 0 is taken as 1.
    const std::vector<std::string> ones = pieces_of(doc.root(), 0, 0);
    BOOST_TEST_EQ(ones.size(), sample_compact().size());

    // A long text in pieces of the default size.
    const std::string long_text(10000, 'q');
    const wd::document big = document_of(std::string_view(long_text));
    const std::vector<std::string> big_pieces = pieces_of(big.root(), 0, wd::default_chunk);
    BOOST_TEST_EQ(big_pieces.size(), 3U);
    std::string big_joined;
    for (const std::string& piece : big_pieces) {
        big_joined += piece;
    }
    BOOST_TEST_EQ(big_joined, '"' + long_text + '"');
}

void a_failing_sink() {
    const auto doc = sample<wd::document>();
    const std::string before = wd::stringify(doc);
    const std::vector<std::byte> image(doc.image().begin(), doc.image().end());
    std::size_t calls = 0;
    const auto written = wd::stringify_to(
        doc,
        [&calls](std::string_view) -> wd::result<void> {
            ++calls;
            if (calls == 3) {
                return wd::errc::capacity;
            }
            return {};
        },
        0, 4);
    BOOST_TEST(!written.has_value());
    BOOST_TEST(written.error() == wd::errc::capacity);
    // It stopped at the failure, and the document reads as before.
    BOOST_TEST_EQ(calls, 3U);
    BOOST_TEST_EQ(wd::stringify(doc), before);
    BOOST_TEST(std::vector<std::byte>(doc.image().begin(), doc.image().end()) == image);

    // A failure on the last piece, which only the final flush hands over.
    std::size_t total = 0;
    const auto last = wd::stringify_to(
        doc,
        [&total](std::string_view) -> wd::result<void> {
            ++total;
            return wd::errc::capacity;
        },
        0, 1000);
    BOOST_TEST(last.error() == wd::errc::capacity);
    BOOST_TEST_EQ(total, 1U);
}

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
void a_throwing_sink() {
    const auto doc = sample<wd::document>();
    const std::string before = wd::stringify(doc);
    std::size_t calls = 0;
    bool caught = false;
    try {
        (void)wd::stringify_to(
            doc,
            [&calls](std::string_view) {
                if (++calls == 2) {
                    throw std::runtime_error("sink");
                }
            },
            0, 4);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    BOOST_TEST(caught);
    BOOST_TEST_EQ(calls, 2U);
    BOOST_TEST_EQ(wd::stringify(doc), before);
}
#endif

void nested_100000_levels() {
    constexpr std::size_t depth = 100'000;
    wd::document doc = make_document();
    webcpp::test::require(BOOST_TEST(doc.root().set(wd::array()).has_value()));
    // A handle cannot be assigned without exceptions: the next one is constructed in place.
    std::optional<wd::value> at;
    at.emplace(doc.root());
    // Arrays at even levels and objects at odd ones, so that both kinds of frame are deep.
    for (std::size_t level = 1; level < depth; ++level) {
        const bool in_object = level % 2 == 0;
        wd::value child = in_object ? (*at)["k"] : (*at)[std::uint64_t{0}];
        const auto set = level % 2 == 1 ? child.set(wd::object()) : child.set(wd::array());
        webcpp::test::require(BOOST_TEST(set.has_value()));
        at.emplace(std::move(child));
    }
    // Every level opens but the innermost, an empty object, which JavaScript writes `{}`.
    std::string expected;
    std::string closers;
    for (std::size_t level = 0; level + 1 < depth; ++level) {
        expected += level % 2 == 1 ? R"({"k":)" : "[";
        closers += level % 2 == 1 ? '}' : ']';
    }
    expected += (depth - 1) % 2 == 1 ? "{}" : "[]";
    expected.append(closers.rbegin(), closers.rend());
    // Compared whole, without printing 800,000 characters on a failure.
    BOOST_TEST(wd::stringify(doc) == expected);
    std::string joined;
    const auto written =
        wd::stringify_to(doc, [&joined](std::string_view piece) { joined += piece; });
    BOOST_TEST(written.has_value());
    BOOST_TEST(joined == expected);
}

}  // namespace

int main() {
    writes_as_json_stringify();
    js_traits_write_the_same();
    the_members();
    empty_containers();
    nesting();
    escapes();
    numbers();
    dates();
    bytes_by_policy();
    a_lone_surrogate_under_js_traits();
    an_undefined_root();
    stringify_to_every_chunk();
    a_failing_sink();
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
    a_throwing_sink();
#endif
    nested_100000_levels();
    return boost::report_errors();
}
