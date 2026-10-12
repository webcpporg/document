// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The streaming parser: every text of the corpus split at every byte, and written one byte at a
// time, gives the document a whole parse gives; a syntax error has the same offset however the
// text is split; a failed parser stays failed until reset; a parser takes no text after its
// finish, nor a second text after its first; max_bytes counts every chunk; the default image limit
// of 64 MiB holds however the text is split; and an external parser builds its document in place.

#include <webcpp/document/parser.hpp>
#include <webcpp/document/stringify.hpp>

#include <boost/core/lightweight_test.hpp>
#include <boost/json/error.hpp>

#include "parse_corpus.hpp"
#include "require.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wd = webcpp::document;

namespace {

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

// The document a parser builds from `chunks`, written in order, as stringify writes it, or
// "error " and the error's message and offset.
template <class Traits>
std::string streamed(std::span<const std::string_view> chunks) {
    wd::basic_parser<Traits> p;
    const auto refused = [&p](const boost::system::error_code& e) {
        return "error " + e.message() + " at " + std::to_string(p.offset());
    };
    for (const std::string_view chunk : chunks) {
        if (const auto written = p.write(chunk); !written.has_value()) {
            return refused(written.error());
        }
    }
    auto made = p.finish();
    return made.has_value() ? wd::stringify(*made) : refused(made.error());
}

// Every split of every text of the corpus, and one byte at a time, under the traits, gives what
// the text whole gives: the same document, or the same error at the same offset.
template <class Traits>
void every_split_under() {
    for (const std::string& text : webcpp::test::parse_corpus()) {
        const std::string_view view(text);
        const std::array<std::string_view, 1> one{view};
        const std::string expected = streamed<Traits>(one);
        auto whole = wd::parse<Traits>(text);
        BOOST_TEST_EQ(whole.has_value() ? wd::stringify(*whole) : expected, expected);
        for (std::size_t p = 0; p <= text.size(); ++p) {
            const std::array<std::string_view, 2> halves{view.substr(0, p), view.substr(p)};
            BOOST_TEST_EQ(streamed<Traits>(halves), expected);
        }
        std::vector<std::string_view> bytes;
        bytes.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            bytes.push_back(view.substr(i, 1));
        }
        BOOST_TEST_EQ(streamed<Traits>(bytes), expected);
    }
}

// Each split gives a document equal to the whole text's, compared node by node.
void every_split_is_equal() {
    for (const std::string& text : webcpp::test::parse_corpus()) {
        auto whole = wd::parse(text);
        if (!BOOST_TEST(whole.has_value())) {
            continue;
        }
        for (std::size_t p = 0; p <= text.size(); ++p) {
            wd::parser split;
            BOOST_TEST(split.write(std::string_view(text).substr(0, p)).has_value());
            BOOST_TEST(split.write(std::string_view(text).substr(p)).has_value());
            auto made = split.finish();
            BOOST_TEST(made.has_value() && *made == *whole);
        }
    }
}

// A chunk boundary inside a token, a character, an escape, a number, a literal or a key, changes
// nothing.
void every_split_gives_the_same_document() {
    BOOST_TEST_EQ(webcpp::test::parse_corpus().size(), 40U);
    every_split_under<wd::default_traits>();
    every_split_under<wd::js_traits>();
    every_split_is_equal();
}

void a_syntax_error_has_the_same_offset_however_split() {
    constexpr std::string_view text = "[1,2,}";
    wd::parser whole;
    BOOST_TEST(whole.write(text).error() == wd::errc::syntax);
    BOOST_TEST_EQ(whole.offset(), 5U);
    for (std::size_t p = 0; p <= text.size(); ++p) {
        wd::parser split;
        const auto first = split.write(text.substr(0, p));
        const auto second = split.write(text.substr(p));
        BOOST_TEST(first.has_value() || first.error() == wd::errc::syntax);
        BOOST_TEST(second.error() == wd::errc::syntax);
        BOOST_TEST(split.finish().error() == wd::errc::syntax);
        BOOST_TEST_EQ(split.offset(), 5U);
        BOOST_TEST(split.cause() == whole.cause());
    }
}

// After a failing write, write and finish return the same error, the offset and the cause
// unchanged, until reset, after which a new text parses.
void a_failed_parser_stays_failed() {
    wd::parser p;
    BOOST_TEST(p.write("[1,").has_value());
    BOOST_TEST(p.write("]").error() == wd::errc::syntax);
    const std::uint64_t offset = p.offset();
    const boost::system::error_code cause = p.cause();
    BOOST_TEST(p.write("2]").error() == wd::errc::syntax);
    BOOST_TEST(p.finish().error() == wd::errc::syntax);
    BOOST_TEST(p.write("").error() == wd::errc::syntax);
    BOOST_TEST_EQ(p.offset(), offset);
    BOOST_TEST(p.cause() == cause);

    p.reset();
    BOOST_TEST_EQ(p.offset(), 0U);
    BOOST_TEST(!p.cause());
    BOOST_TEST(p.write(R"({"a":)").has_value());
    BOOST_TEST(p.write("[1]}").has_value());
    auto done = p.finish();
    if (BOOST_TEST(done.has_value())) {
        BOOST_TEST_EQ(wd::stringify(*done), R"({"a":[1]})");
    }

    // A number past the traits fails the parser too, and so does a limit.
    wd::parse_options shallow;
    shallow.max_depth = 2;
    wd::parser q(shallow);
    BOOST_TEST(q.write("[1e99999999999,").error() == wd::errc::range);
    BOOST_TEST(q.write("1]").error() == wd::errc::range);
    BOOST_TEST(q.finish().error() == wd::errc::range);
    q.reset();
    BOOST_TEST(q.write("[[[").error() == wd::errc::capacity);
    BOOST_TEST(q.finish().error() == wd::errc::capacity);
    q.reset();
    BOOST_TEST(q.write("[[]]").has_value());
    BOOST_TEST(q.finish().has_value());
}

// A parser takes one text: after its finish, more text is malformed until reset.
void a_finished_parser_takes_no_more_text() {
    wd::parser p;
    BOOST_TEST(p.write("1").has_value());
    BOOST_TEST(p.finish().has_value());
    BOOST_TEST(p.write("2").error() == wd::errc::syntax);
    BOOST_TEST(p.finish().error() == wd::errc::syntax);
    p.reset();
    BOOST_TEST(p.write("[2]").has_value());
    auto again = p.finish();
    if (BOOST_TEST(again.has_value())) {
        BOOST_TEST_EQ(wd::stringify(*again), "[2]");
    }
    // An empty text is malformed, and whitespace alone too.
    wd::parser empty;
    BOOST_TEST(empty.finish().error() == wd::errc::syntax);
    wd::parser blank;
    BOOST_TEST(blank.write(" \n").has_value());
    BOOST_TEST(blank.finish().error() == wd::errc::syntax);
}

// A parser takes one text: whatever follows it but whitespace, in its chunk or a later one, is
// extra data, errc::syntax at the offset where it begins, and starts no other document.
void text_after_the_text_is_extra_data() {
    const auto refused_at = [](const wd::parser& p, std::uint64_t offset) {
        BOOST_TEST_EQ(p.offset(), offset);
        BOOST_TEST(p.cause() == boost::json::error::extra_data);
    };
    wd::parser p;
    BOOST_TEST(p.write("1 ").has_value());
    BOOST_TEST(p.write("2 ").error() == wd::errc::syntax);
    BOOST_TEST(p.write("3").error() == wd::errc::syntax);
    BOOST_TEST(p.finish().error() == wd::errc::syntax);
    refused_at(p, 2);

    // Whitespace after the text, in any chunk, is part of it.
    wd::parser blank;
    for (const std::string_view chunk : {"[1]", " ", "\n\t", " \r"}) {
        BOOST_TEST(blank.write(chunk).has_value());
    }
    auto made = blank.finish();
    if (BOOST_TEST(made.has_value())) {
        BOOST_TEST_EQ(wd::stringify(*made), "[1]");
    }

    // A selector sees the first text only: the second is refused before it begins.
    wd::parser observed;
    std::vector<std::string> delivered;
    const auto subscribed =
        observed.observe("$", [&delivered](const wd::const_value& v, const wd::path_view&) {
            delivered.push_back(wd::stringify(v));
        });
    BOOST_TEST(subscribed.has_value());
    BOOST_TEST(observed.write(R"({"a":1} )").has_value());
    BOOST_TEST(observed.write("[2] ").error() == wd::errc::syntax);
    BOOST_TEST(observed.write(R"({"evil":true})").error() == wd::errc::syntax);
    BOOST_TEST(observed.finish().error() == wd::errc::syntax);
    refused_at(observed, 8);
    BOOST_TEST((delivered == std::vector<std::string>{R"({"a":1})"}));

    // However the text is split, in two chunks or in three, the error and its offset are one.
    constexpr std::string_view two = "[1] 2";
    const std::array<std::string_view, 1> whole{two};
    const std::string expected = streamed<wd::default_traits>(whole);
    BOOST_TEST_EQ(expected, "error malformed JSON text at 4");
    for (std::size_t at = 0; at <= two.size(); ++at) {
        const std::array<std::string_view, 2> halves{two.substr(0, at), two.substr(at)};
        BOOST_TEST_EQ(streamed<wd::default_traits>(halves), expected);
        for (std::size_t second = at; second <= two.size(); ++second) {
            const std::array<std::string_view, 3> thirds{
                two.substr(0, at),
                two.substr(at, second - at),
                two.substr(second),
            };
            BOOST_TEST_EQ(streamed<wd::default_traits>(thirds), expected);
        }
    }
    wd::parser at_once;
    BOOST_TEST(at_once.write(two).error() == wd::errc::syntax);
    refused_at(at_once, 4);
}

// max_bytes counts every chunk: the chunk that passes it fails, and the total may reach it.
void chunk_limits() {
    std::string text = "[";
    while (text.size() < 200) {
        text += "1234567,";
    }
    text += "0]";
    wd::parse_options limited;
    limited.max_bytes = 100;
    wd::parser p(limited);
    std::size_t failed_at = 0;
    for (std::size_t at = 0; at < text.size(); at += 7) {
        const auto written = p.write(std::string_view(text).substr(at, 7));
        if (!written.has_value()) {
            BOOST_TEST(written.error() == wd::errc::capacity);
            failed_at = at;
            break;
        }
    }
    // The chunk at 98 brings the total to 105.
    BOOST_TEST_EQ(failed_at, 98U);
    BOOST_TEST(p.write("1").error() == wd::errc::capacity);
    BOOST_TEST(p.finish().error() == wd::errc::capacity);

    std::string exact = "[ ";
    while (exact.size() < 99) {
        exact += "1,";
    }
    exact.resize(99);
    exact += ']';
    BOOST_TEST_EQ(exact.size(), 100U);
    wd::parser q(limited);
    for (std::size_t at = 0; at < exact.size(); at += 9) {
        BOOST_TEST(q.write(std::string_view(exact).substr(at, 9)).has_value());
    }
    BOOST_TEST(q.finish().has_value());
    BOOST_TEST(q.write("").error() == wd::errc::syntax);
}

// The image limit holds in chunks as it does whole: 400,000 zeros, 800 KB of text whose image would
// take 77 MB, written 64 KiB at a time, are errc::capacity under the default limit of 64 MiB and
// read under 128 MiB.
void the_default_image_limit_holds_in_chunks() {
    std::string text = "[";
    for (std::size_t i = 0; i < 400'000; ++i) {
        text += i == 0 ? "0" : ",0";
    }
    text += ']';
    const auto streamed_with = [&text](const wd::parse_options& o) -> wd::result<wd::document> {
        wd::parser p(o);
        constexpr std::size_t chunk = 65536;
        for (std::size_t at = 0; at < text.size(); at += chunk) {
            if (const auto written = p.write(std::string_view(text).substr(at, chunk));
                !written.has_value()) {
                return written.error();
            }
        }
        return p.finish();
    };
    const auto refused = streamed_with({});
    BOOST_TEST(!refused.has_value() && refused.error() == wd::errc::capacity);
    wd::parse_options raised;
    raised.max_document_bytes = std::uint64_t{128} << 20U;
    const auto made = streamed_with(raised);
    if (BOOST_TEST(made.has_value())) {
        BOOST_TEST_EQ(made->root().size(), 400'000U);
    }
}

// An external parser formats its buffer when the text begins and builds the document in place; a
// buffer too small is errc::capacity.
void an_external_parser() {
    constexpr std::string_view text = R"({"items":[{"id":1},{"id":2}],"name":"xé"})";
    const aligned_buffer buffer(16384);
    wd::basic_parser<wd::default_traits, wd::external> p(buffer.bytes());
    for (std::size_t at = 0; at < text.size(); at += 5) {
        BOOST_TEST(p.write(text.substr(at, 5)).has_value());
    }
    auto made = p.finish();
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    BOOST_TEST(made->image().data() == buffer.bytes().data());
    auto whole = wd::parse(text);
    if (BOOST_TEST(whole.has_value())) {
        BOOST_TEST_EQ(wd::stringify(*made), wd::stringify(*whole));
    }
    auto reopened = wd::open(buffer.bytes());
    if (BOOST_TEST(reopened.has_value())) {
        BOOST_TEST_EQ(wd::stringify(*reopened), wd::stringify(*whole));
    }

    const aligned_buffer tiny(1024);
    wd::basic_parser<wd::default_traits, wd::external> q(tiny.bytes());
    std::string many = "[";
    for (std::size_t i = 0; i < 100; ++i) {
        many += "\"a string that takes room\",";
    }
    many += "0]";
    BOOST_TEST(q.write(many).error() == wd::errc::capacity);
    BOOST_TEST(q.finish().error() == wd::errc::capacity);

    const aligned_buffer none(32);
    wd::basic_parser<wd::default_traits, wd::external> r(none.bytes());
    BOOST_TEST(r.write("1").error() == wd::errc::capacity);
}

}  // namespace

int main() {
    every_split_gives_the_same_document();
    a_syntax_error_has_the_same_offset_however_split();
    a_failed_parser_stays_failed();
    a_finished_parser_takes_no_more_text();
    text_after_the_text_is_extra_data();
    chunk_limits();
    the_default_image_limit_holds_in_chunks();
    an_external_parser();
    return boost::report_errors();
}
