// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Streams of documents: each framing on its own rows, NDJSON's rules one by one, JSON text
// sequences with indented records and bad ones, errors skipped with their lines or stopping the
// stream until a reset, the limits held per document, and every stream of a corpus split at every
// byte and written one byte at a time giving what one write gives, by next() and by the push form
// alike.

#include <webcpp/document/parse.hpp>
#include <webcpp/document/stream.hpp>
#include <webcpp/document/stringify.hpp>

#include <boost/core/lightweight_test.hpp>
#include <boost/json/error.hpp>
#include <boost/system/errc.hpp>

#include "parse_corpus.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wd = webcpp::document;

namespace {

using outcomes = std::vector<std::string>;

constexpr wd::framing concatenated = wd::framing::concatenated;
constexpr wd::framing lines = wd::framing::lines;
constexpr wd::framing rfc7464 = wd::framing::rfc7464;
constexpr wd::on_error stop = wd::on_error::stop;
constexpr wd::on_error skip = wd::on_error::skip;

wd::stream_options options_of(wd::framing f, wd::on_error e) {
    wd::stream_options o;
    o.framing = f;
    o.on_error = e;
    return o;
}

// An error as the outcomes write it: its message, and the line and offset where it was found.
std::string error_at(const boost::system::error_code& e, const wd::stream_parser& p) {
    return "error " + e.message() + " at " + std::to_string(p.error_line()) + ":" +
           std::to_string(p.error_offset());
}

// What next() yields from `chunks` written in order, then finish: each document as stringify
// writes it, and each error; after an error that stops the stream, nothing more.
outcomes pulled(const wd::stream_options& o, std::span<const std::string_view> chunks) {
    wd::stream_parser p(o);
    outcomes out;
    // Takes every document ready; false once the stream stopped.
    const auto take = [&p, &out, &o] {
        for (;;) {
            auto taken = p.next();
            if (!taken.has_value()) {
                out.push_back(error_at(taken.error(), p));
                if (o.on_error == wd::on_error::stop || o.framing == wd::framing::concatenated) {
                    return false;
                }
                continue;
            }
            const std::optional<wd::document>& d = *taken;
            if (!d.has_value()) {
                return true;
            }
            out.push_back(wd::stringify(*d));
        }
    };
    for (const std::string_view chunk : chunks) {
        static_cast<void>(p.write(chunk));
        if (!take()) {
            return out;
        }
    }
    static_cast<void>(p.finish());
    take();
    return out;
}

// What the push form hands its callback from the same chunks, and the error that stopped it.
outcomes pushed(const wd::stream_options& o, std::span<const std::string_view> chunks) {
    wd::stream_parser p(o);
    outcomes out;
    const auto record = [&p, &out](wd::result<wd::document> d) {
        out.push_back(d.has_value() ? wd::stringify(*d) : error_at(d.error(), p));
    };
    for (const std::string_view chunk : chunks) {
        if (const auto written = p.write(chunk, record); !written.has_value()) {
            out.push_back(error_at(written.error(), p));
            return out;
        }
    }
    if (const auto finished = p.finish(record); !finished.has_value()) {
        out.push_back(error_at(finished.error(), p));
    }
    return out;
}

// The stream in one write.
outcomes whole(const wd::stream_options& o, std::string_view text) {
    const std::array<std::string_view, 1> one{text};
    return pulled(o, one);
}

// What next() took: the document as stringify writes it, "none", or "error".
template <class Traits>
std::string taken_text(const wd::result<std::optional<wd::basic_document<Traits>>>& taken) {
    if (!taken.has_value()) {
        return "error";
    }
    const std::optional<wd::basic_document<Traits>>& d = *taken;
    return d.has_value() ? wd::stringify(*d) : "none";
}

// The error outcome of a syntax error at a line and an offset.
std::string syntax_at(std::uint64_t line, std::uint64_t offset) {
    return "error malformed JSON text at " + std::to_string(line) + ":" + std::to_string(offset);
}

std::string capacity_at(std::uint64_t line, std::uint64_t offset) {
    return "error no room left at " + std::to_string(line) + ":" + std::to_string(offset);
}

// Whether `got` is `expected`; else each outcome got is printed, under the stream.
bool same(const outcomes& got, const outcomes& expected, std::string_view text) {
    if (BOOST_TEST(got == expected)) {
        return true;
    }
    BOOST_LIGHTWEIGHT_TEST_OSTREAM << "  stream: " << text << "\n";
    for (const std::string& o : got) {
        BOOST_LIGHTWEIGHT_TEST_OSTREAM << "  got: " << o << "\n";
    }
    return false;
}

// A stream, its options, and what it yields.
struct row {
    wd::stream_options options;
    std::string text;
    outcomes expected;
};

row make_row(const wd::stream_options& options, std::string text, outcomes expected) {
    return {.options = options, .text = std::move(text), .expected = std::move(expected)};
}

// The rows of the framings and the policies, which the corpus splits at every byte.
std::vector<row> stream_rows() {
    std::vector<row> rows{
        // concatenated: any whitespace or none between documents, indented documents, a top-level
        // number closed by the next byte or by finish.
        make_row(options_of(concatenated, stop), R"({"a":1}    {"b":2})",
                 {R"({"a":1})", R"({"b":2})"}),
        make_row(options_of(concatenated, stop), R"({"a":1}{"b":2})", {R"({"a":1})", R"({"b":2})"}),
        make_row(options_of(concatenated, stop), "[1][2]", {"[1]", "[2]"}),
        make_row(options_of(concatenated, stop), R"("x""y")", {R"("x")", R"("y")"}),
        make_row(options_of(concatenated, stop), "1 2", {"1", "2"}),
        make_row(options_of(concatenated, stop), "12 34\n5", {"12", "34", "5"}),
        make_row(options_of(concatenated, stop), "1[2]3", {"1", "[2]", "3"}),
        make_row(options_of(concatenated, stop), R"(true false null"s"[]{}-0.5)",
                 {"true", "false", "null", R"("s")", "[]", "{}", "-0.5"}),
        make_row(options_of(concatenated, stop),
                 "{\n  \"a\": [\n    1,\n    2\n  ]\n}\n{\n  \"b\": true\n}\n",
                 {R"({"a":[1,2]})", R"({"b":true})"}),
        make_row(options_of(concatenated, stop), "", {}),
        make_row(options_of(concatenated, stop), " \t\r\n ", {}),
        make_row(options_of(concatenated, stop), R"({"a":1} x [2])",
                 {R"({"a":1})", syntax_at(1, 8)}),
        make_row(options_of(concatenated, stop), "[1]\n[2,", {"[1]", syntax_at(2, 7)}),
        make_row(options_of(concatenated, stop), "[1] }", {"[1]", syntax_at(1, 4)}),

        // lines: \n, \r\n, whitespace then \n, empty lines; the last line without its \n.
        make_row(options_of(lines, stop), "{\"a\":1}\n{\"b\":2}\n", {R"({"a":1})", R"({"b":2})"}),
        make_row(options_of(lines, stop), "{\"a\":1}\r\n[2]\r\n", {R"({"a":1})", "[2]"}),
        make_row(options_of(lines, stop), "{\"a\":1}  \t \n", {R"({"a":1})"}),
        make_row(options_of(lines, stop), "\n\n{\"a\":1}\n\n\r\n \n[2]\n", {R"({"a":1})", "[2]"}),
        make_row(options_of(lines, stop), "{\"a\":1}\n[2]", {R"({"a":1})", "[2]"}),
        make_row(options_of(lines, stop), "1\n2", {"1", "2"}),
        make_row(options_of(lines, stop), "  [1]\n\t2 \n", {"[1]", "2"}),
        make_row(options_of(lines, stop), "\"a\\nb\"\n", {R"("a\nb")"}),
        make_row(options_of(lines, stop), "", {}),
        make_row(options_of(lines, stop), "\n\n", {}),
        // Two documents on one line, or a document not complete at its \n: an error for the line.
        make_row(options_of(lines, stop), "{\"a\":1} {\"b\":2}\n[3]\n", {syntax_at(1, 8)}),
        make_row(options_of(lines, skip), "{\"a\":1} {\"b\":2}\n[3]\n", {syntax_at(1, 8), "[3]"}),
        make_row(options_of(lines, skip), "1 2\n[3]", {syntax_at(1, 2), "[3]"}),
        make_row(options_of(lines, stop), "[1,\n2]\n", {syntax_at(1, 3)}),
        // Indented JSON is not NDJSON: each of its lines is an error of its own.
        make_row(options_of(lines, stop), "{\n\"a\":1\n}\n", {syntax_at(1, 1)}),
        make_row(options_of(lines, skip), "{\n\"a\":1\n}\n",
                 {syntax_at(1, 1), syntax_at(2, 5), syntax_at(3, 8)}),
        // on_error::skip names each bad line, and the stream goes on.
        make_row(options_of(lines, skip), "{\"a\":1}\n{bad}\n\n[2]\n1 2\n\"x\"",
                 {R"({"a":1})", syntax_at(2, 9), "[2]", syntax_at(5, 21), R"("x")"}),
        make_row(options_of(lines, skip), "[1]\n[2,", {"[1]", syntax_at(2, 7)}),
        make_row(options_of(lines, skip), "[1]\ntru\n[2]\n", {"[1]", syntax_at(2, 7), "[2]"}),

        // rfc7464: records opened by 0x1E, indented records, numbers closed by a record's end.
        make_row(options_of(rfc7464, stop), "\x1e{\"a\":1}\n\x1e[2]\n", {R"({"a":1})", "[2]"}),
        make_row(options_of(rfc7464, stop), "\x1e{\n  \"a\": 1\n}\n\x1e[\n  2\n]\n",
                 {R"({"a":1})", "[2]"}),
        make_row(options_of(rfc7464, stop),
                 "\x1e"
                 "1\n\x1e"
                 "2",
                 {"1", "2"}),
        make_row(options_of(rfc7464, stop),
                 "\x1e"
                 "12\x1e"
                 "3\n",
                 {"12", "3"}),
        make_row(options_of(rfc7464, stop), "\x1e\x1e\x1e[1]\n\x1e  \n", {"[1]"}),
        make_row(options_of(rfc7464, stop), "\n \x1e[1]\n", {"[1]"}),
        // A bad record is skipped up to the next 0x1E, or stops the stream.
        make_row(options_of(rfc7464, skip), "\x1e{\"a\":1}\n\x1e{bad\n\x1e[3]\n",
                 {R"({"a":1})", syntax_at(2, 11), "[3]"}),
        make_row(options_of(rfc7464, stop), "\x1e{\"a\":1}\n\x1e{bad\n\x1e[3]\n",
                 {R"({"a":1})", syntax_at(2, 11)}),
        make_row(options_of(rfc7464, skip), "\x1e[1] x\n\x1e[2]\n",
                 {"[1]", syntax_at(1, 5), "[2]"}),
        make_row(options_of(rfc7464, skip), "\x1e{\n\"a\":\n\x1e[2]\n", {syntax_at(3, 8), "[2]"}),
        // Bytes before the first 0x1E are an error.
        make_row(options_of(rfc7464, stop), "x\x1e[1]\n", {syntax_at(1, 0)}),
        make_row(options_of(rfc7464, skip), "x\x1e[1]\n", {syntax_at(1, 0), "[1]"}),
        // A record the stream's end cuts short.
        make_row(options_of(rfc7464, stop), "\x1e[1,2", {syntax_at(1, 5)}),
        make_row(options_of(rfc7464, skip), "\x1e[1]\n\x1e[1,2", {"[1]", syntax_at(2, 10)}),
    };
    return rows;
}

void each_row_yields_what_it_says() {
    for (const row& r : stream_rows()) {
        same(whole(r.options, r.text), r.expected, r.text);
    }
}

// The texts of the parse corpus as streams of each framing: one after another, one per line where
// a text holds no raw newline, and one per record; each yields its documents in order.
std::vector<row> corpus_rows() {
    std::string joined;
    std::string per_line;
    std::string records;
    outcomes all;
    outcomes on_lines;
    for (const std::string& text : webcpp::test::parse_corpus()) {
        auto parsed = wd::parse(text);
        if (!BOOST_TEST(parsed.has_value())) {
            continue;
        }
        const std::string written = wd::stringify(*parsed);
        all.push_back(written);
        joined += text;
        joined += '\n';
        records += '\x1e';
        records += text;
        records += '\n';
        if (text.find('\n') == std::string::npos) {
            on_lines.push_back(written);
            per_line += text;
            per_line += "\r\n";
        }
    }
    return {
        make_row(options_of(concatenated, stop), joined, all),
        make_row(options_of(lines, stop), per_line, on_lines),
        make_row(options_of(rfc7464, stop), records, all),
    };
}

// `a`, `between`, `b` and `after`, one after another.
std::string spliced(const std::string& a, std::string_view between, const std::string& b,
                    std::string_view after) {
    std::string text = a;
    text += between;
    text += b;
    text += after;
    return text;
}

// The streams of two neighbours of the parse corpus's fixed texts, in each framing: the boundary
// between two documents of every shape. The two long texts the corpus generates, which test a
// split inside a key or a string, are left to streaming_test.
std::vector<row> pair_rows() {
    const std::vector<std::string> corpus = webcpp::test::parse_corpus();
    std::vector<row> rows;
    for (std::size_t i = 0; i + 1 < corpus.size() - 2; ++i) {
        const std::string& a = corpus[i];
        const std::string& b = corpus[i + 1];
        auto first = wd::parse(a);
        auto second = wd::parse(b);
        if (!BOOST_TEST(first.has_value() && second.has_value())) {
            continue;
        }
        const outcomes both{wd::stringify(*first), wd::stringify(*second)};
        rows.push_back(make_row(options_of(concatenated, stop), spliced(a, "\n", b, ""), both));
        rows.push_back(
            make_row(options_of(rfc7464, skip), spliced("\x1e" + a, "\n\x1e", b, "\n"), both));
        if (a.find('\n') == std::string::npos && b.find('\n') == std::string::npos) {
            rows.push_back(make_row(options_of(lines, skip), spliced(a, "\r\n", b, ""), both));
        }
    }
    return rows;
}

void the_corpus_as_streams() {
    for (const row& r : corpus_rows()) {
        same(whole(r.options, r.text), r.expected, r.text);
        std::vector<std::string_view> bytes;
        bytes.reserve(r.text.size());
        for (std::size_t i = 0; i < r.text.size(); ++i) {
            bytes.push_back(std::string_view(r.text).substr(i, 1));
        }
        BOOST_TEST(pulled(r.options, bytes) == r.expected);
        BOOST_TEST(pushed(r.options, bytes) == r.expected);
    }
    for (const row& r : pair_rows()) {
        same(whole(r.options, r.text), r.expected, r.text);
    }
}

// However a stream is split, in two at every byte or one byte at a time, next() and the push form
// yield what one write yields.
void every_split_gives_the_same_outcomes() {
    std::vector<row> rows = stream_rows();
    for (row& r : pair_rows()) {
        rows.push_back(std::move(r));
    }
    for (const row& r : rows) {
        const std::string_view text(r.text);
        const std::array<std::string_view, 1> one{text};
        const outcomes expected = pulled(r.options, one);
        BOOST_TEST(pushed(r.options, one) == expected);
        for (std::size_t at = 0; at <= text.size(); ++at) {
            const std::array<std::string_view, 2> halves{text.substr(0, at), text.substr(at)};
            BOOST_TEST(pulled(r.options, halves) == expected);
            BOOST_TEST(pushed(r.options, halves) == expected);
        }
        std::vector<std::string_view> bytes;
        bytes.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            bytes.push_back(text.substr(i, 1));
        }
        BOOST_TEST(pulled(r.options, bytes) == expected);
        BOOST_TEST(pushed(r.options, bytes) == expected);
    }
}

// A document is delivered as soon as the bytes that end it are consumed: at its value's close with
// concatenated and rfc7464, at its line's \n with lines; a top-level number waits for the byte
// that closes it.
void documents_are_delivered_as_they_complete() {
    wd::stream_parser p;
    BOOST_TEST_EQ(p.write(R"({"a":)").value(), 0U);
    BOOST_TEST_EQ(taken_text(p.next()), "none");
    BOOST_TEST_EQ(p.write("1}[").value(), 1U);
    BOOST_TEST_EQ(taken_text(p.next()), R"({"a":1})");
    BOOST_TEST_EQ(p.write("]12").value(), 1U);
    BOOST_TEST_EQ(p.write("3").value(), 0U);
    BOOST_TEST_EQ(p.write(" ").value(), 1U);
    BOOST_TEST_EQ(p.write("4").value(), 0U);
    BOOST_TEST_EQ(p.finish().value(), 1U);
    BOOST_TEST_EQ(taken_text(p.next()), "[]");
    BOOST_TEST_EQ(taken_text(p.next()), "123");
    BOOST_TEST_EQ(taken_text(p.next()), "4");
    BOOST_TEST_EQ(taken_text(p.next()), "none");
    BOOST_TEST_EQ(p.documents(), 4U);

    wd::stream_parser l(options_of(lines, stop));
    BOOST_TEST_EQ(l.write("[1]").value(), 0U);
    BOOST_TEST_EQ(l.write(" \r").value(), 0U);
    BOOST_TEST_EQ(l.write("\n").value(), 1U);
}

// line() is the line of the last byte consumed, offset() the bytes consumed, documents() those
// delivered.
void lines_and_offsets() {
    wd::stream_parser p(options_of(lines, stop));
    BOOST_TEST_EQ(p.line(), 1U);
    BOOST_TEST_EQ(p.offset(), 0U);
    BOOST_TEST(p.write("[1]").has_value());
    BOOST_TEST_EQ(p.line(), 1U);
    BOOST_TEST(p.write("\n").has_value());
    BOOST_TEST_EQ(p.line(), 1U);
    BOOST_TEST(p.write("\n[2]\n3").has_value());
    BOOST_TEST_EQ(p.line(), 4U);
    BOOST_TEST_EQ(p.offset(), 10U);
    BOOST_TEST_EQ(p.documents(), 0U);
    BOOST_TEST(p.next().has_value());
    BOOST_TEST_EQ(p.documents(), 1U);
    BOOST_TEST_EQ(p.error_line(), 0U);
}

// With on_error::stop the error is sticky: the documents before it are taken, then every call
// returns it, until a reset.
void stop_is_sticky_until_reset() {
    wd::stream_parser p(options_of(lines, stop));
    BOOST_TEST(p.write("[1]\n[2]\n[3,}\n[4]\n").error() == wd::errc::syntax);
    BOOST_TEST_EQ(p.error_line(), 3U);
    BOOST_TEST_EQ(p.error_offset(), 11U);
    BOOST_TEST_EQ(p.offset(), 11U);
    BOOST_TEST(p.next().has_value());
    BOOST_TEST(p.next().has_value());
    BOOST_TEST(p.next().error() == wd::errc::syntax);
    BOOST_TEST(p.next().error() == wd::errc::syntax);
    BOOST_TEST(p.write("[5]\n").error() == wd::errc::syntax);
    BOOST_TEST(p.finish().error() == wd::errc::syntax);
    BOOST_TEST_EQ(p.error_line(), 3U);
    BOOST_TEST_EQ(p.documents(), 2U);

    p.reset();
    BOOST_TEST_EQ(p.line(), 1U);
    BOOST_TEST_EQ(p.offset(), 0U);
    BOOST_TEST_EQ(p.documents(), 0U);
    BOOST_TEST_EQ(p.write("[5]\n").value(), 1U);
    BOOST_TEST_EQ(taken_text(p.next()), "[5]");

    // The push form hands over the documents before the error, then returns it.
    wd::stream_parser q(options_of(rfc7464, stop));
    outcomes handed;
    const auto written = q.write("\x1e[1]\n\x1e[2\x1e[3]", [&handed](wd::result<wd::document> d) {
        handed.push_back(d.has_value() ? wd::stringify(*d) : "error");
    });
    BOOST_TEST(written.error() == wd::errc::syntax);
    BOOST_TEST((handed == outcomes{"[1]"}));
    BOOST_TEST_EQ(q.error_line(), 2U);
    BOOST_TEST_EQ(q.error_offset(), 8U);
}

// The error a callback returns stops the stream with that error.
void a_failing_callback_stops_the_stream() {
    wd::stream_parser p(options_of(lines, skip));
    std::size_t calls = 0;
    const auto refusing = [&calls](const wd::result<wd::document>&) -> wd::result<void> {
        ++calls;
        return wd::errc::type;
    };
    BOOST_TEST(p.write("[1]\n[2]\n", refusing).error() == wd::errc::type);
    BOOST_TEST_EQ(calls, 1U);
    BOOST_TEST(p.write("[3]\n").error() == wd::errc::type);
    BOOST_TEST(p.next().error() == wd::errc::type);
}

// The push form hands over first what next() has not taken, in order.
void the_push_form_drains_the_queue_first() {
    wd::stream_parser p(options_of(lines, stop));
    BOOST_TEST_EQ(p.write("[1]\n[2]\n").value(), 2U);
    outcomes handed;
    const auto record = [&handed](wd::result<wd::document> d) {
        handed.push_back(d.has_value() ? wd::stringify(*d) : "error");
    };
    BOOST_TEST_EQ(p.write("[3]\n", record).value(), 1U);
    BOOST_TEST((handed == outcomes{"[1]", "[2]", "[3]"}));
    BOOST_TEST_EQ(taken_text(p.next()), "none");
    BOOST_TEST_EQ(p.finish(record).value(), 0U);
    BOOST_TEST_EQ(p.documents(), 3U);
}

// Each document is held to the limits a parse holds one text to, not the stream.
void limits_are_per_document() {
    wd::stream_options shallow = options_of(lines, skip);
    shallow.max_depth = 2;
    BOOST_TEST(
        (whole(shallow, "[[1]]\n[[[1]]]\n[1]\n") == outcomes{"[[1]]", capacity_at(2, 8), "[1]"}));

    // max_bytes counts each document's bytes from its first: a stream of many passes.
    wd::stream_options short_texts = options_of(concatenated, stop);
    short_texts.max_bytes = 7;
    BOOST_TEST((whole(short_texts, "[1,2,3] [4,5,6]\n[7,8,9]") ==
                outcomes{"[1,2,3]", "[4,5,6]", "[7,8,9]"}));
    BOOST_TEST(
        (whole(short_texts, "[1,2,3] [4,5,6,7]") == outcomes{"[1,2,3]", capacity_at(1, 15)}));
    wd::stream_options short_lines = options_of(lines, skip);
    short_lines.max_bytes = 7;
    same(whole(short_lines, "[1,2,3]  \r\n[1,2,3,4]\n[5]"),
         outcomes{"[1,2,3]", capacity_at(2, 18), "[5]"}, "short lines");

    // max_document_bytes holds each image: twenty documents of 19 KB pass under 64 KiB, which
    // one of 190 KB does not.
    std::string hundred = "[0";
    for (std::size_t i = 1; i < 100; ++i) {
        hundred += ",0";
    }
    hundred += "]\n";
    std::string thousand = "[0";
    for (std::size_t i = 1; i < 1000; ++i) {
        thousand += ",0";
    }
    thousand += "]\n";
    wd::stream_options small = options_of(lines, skip);
    small.max_document_bytes = std::uint64_t{64} << 10U;
    std::string text;
    for (std::size_t i = 0; i < 20; ++i) {
        text += hundred;
    }
    text += thousand;
    text += "[1]\n";
    const outcomes got = whole(small, text);
    if (BOOST_TEST_EQ(got.size(), 22U)) {
        BOOST_TEST_EQ(got[0], got[19]);
        BOOST_TEST_EQ(got[20].substr(0, 18), "error no room left");
        BOOST_TEST_EQ(got[21], "[1]");
    }
}

// on_error::skip with concatenated is refused: every call returns invalid_argument, a reset too.
void skip_with_concatenated_is_refused() {
    wd::stream_parser p(options_of(concatenated, skip));
    const boost::system::error_code refused =
        boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
    BOOST_TEST(p.write("[1]").error() == refused);
    BOOST_TEST(p.next().error() == refused);
    BOOST_TEST(p.finish().error() == refused);
    p.reset();
    BOOST_TEST(p.write("[1]").error() == refused);
    BOOST_TEST(p.write("[1]", [](const wd::result<wd::document>&) {}).error() == refused);
}

// After finish the stream takes no more bytes, until a reset; an empty write and another finish
// complete nothing.
void a_finished_stream_takes_no_more_bytes() {
    wd::stream_parser p;
    BOOST_TEST_EQ(p.write("[1] 2").value(), 1U);
    BOOST_TEST_EQ(p.finish().value(), 1U);
    BOOST_TEST_EQ(p.finish().value(), 0U);
    BOOST_TEST_EQ(taken_text(p.next()), "[1]");
    BOOST_TEST_EQ(taken_text(p.next()), "2");
    BOOST_TEST_EQ(taken_text(p.next()), "none");
    BOOST_TEST_EQ(p.write("").value(), 0U);
    BOOST_TEST(p.write("3").error() == wd::errc::syntax);
    BOOST_TEST(p.error_cause() == boost::json::error::extra_data);
    BOOST_TEST(p.next().error() == wd::errc::syntax);
    p.reset();
    BOOST_TEST_EQ(p.write("[3]").value(), 1U);
}

// A dropped document's error names its cause: Boost.JSON's, extra_data for a second document on a
// line.
void errors_name_their_cause() {
    wd::stream_parser p(options_of(lines, skip));
    BOOST_TEST_EQ(p.write("[1] [2]\n[1,}\n").value(), 2U);
    BOOST_TEST(p.next().error() == wd::errc::syntax);
    BOOST_TEST(p.error_cause() == boost::json::error::extra_data);
    BOOST_TEST(p.next().error() == wd::errc::syntax);
    BOOST_TEST(p.error_cause() == boost::json::error::syntax);
    BOOST_TEST_EQ(p.error_line(), 2U);
    BOOST_TEST_EQ(p.error_offset(), 11U);
}

// Other traits: a number past default_traits' 34 digits, read by js_traits as a double.
void a_stream_of_other_traits() {
    wd::basic_stream_parser<wd::js_traits> p(options_of(lines, stop));
    BOOST_TEST_EQ(p.write("0.1\n123456789012345678901234567890123456789\n").value(), 2U);
    BOOST_TEST_EQ(taken_text(p.next()), "0.1");
    BOOST_TEST_EQ(taken_text(p.next()), "1.2345678901234568e+38");
}

}  // namespace

int main() {
    each_row_yields_what_it_says();
    the_corpus_as_streams();
    every_split_gives_the_same_outcomes();
    documents_are_delivered_as_they_complete();
    lines_and_offsets();
    stop_is_sticky_until_reset();
    a_failing_callback_stops_the_stream();
    the_push_form_drains_the_queue_first();
    limits_are_per_document();
    skip_with_concatenated_is_refused();
    a_finished_stream_takes_no_more_bytes();
    errors_name_their_cause();
    a_stream_of_other_traits();
    return boost::report_errors();
}
