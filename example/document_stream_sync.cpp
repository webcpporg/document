// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Streams of documents read by hand: NDJSON fed in odd-sized pieces and taken with next(),
// indented and concatenated JSON pushed to a callback, a bad NDJSON line skipped with its line
// reported, and rfc7464 records.

#include <webcpp/document.hpp>

#include <boost/system/error_code.hpp>

#include <array>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace wd = webcpp::document;

namespace {

// tag::report[]
/** Prints the error a write, a finish or a take returned, with the line it was found on. */
void report_error(const wd::stream_parser& p, const boost::system::error_code& error) {
    std::cout << "error at line " << p.error_line() << ": " << error.message() << '\n';
}

// end::report[]

// tag::take[]
/**
 Takes every document the parser holds, printing each as stringify writes it. Under
 on_error::skip a dropped document's error is printed and taking goes on; under on_error::stop the
 error comes back on every take, so taking ends at it.
*/
void take_ready(wd::stream_parser& p, wd::on_error policy) {
    for (;;) {
        wd::result<std::optional<wd::document>> taken = p.next();
        if (taken.has_error()) {
            report_error(p, taken.error());
            if (policy == wd::on_error::stop) {
                return;
            }
            continue;
        }
        const std::optional<wd::document>& held = *taken;
        if (!held.has_value()) {
            return;
        }
        std::cout << held->stringify() << '\n';
    }
}

// end::take[]

// tag::pieces[]
/**
 Writes `text` to the parser in pieces of 1, 5, 2 and 7 bytes in turn, as a socket might deliver
 it, and takes the documents each piece completes. Returns false when the stream stopped.
*/
bool write_in_pieces(wd::stream_parser& p, std::string_view text, wd::on_error policy) {
    constexpr std::array<std::size_t, 4> sizes{1, 5, 2, 7};
    for (std::size_t turn = 0; !text.empty(); ++turn) {
        const std::string_view piece = text.substr(0, sizes[turn % sizes.size()]);
        text.remove_prefix(piece.size());
        if (const wd::result<std::size_t> written = p.write(piece); written.has_error()) {
            report_error(p, written.error());
            return false;
        }
        take_ready(p, policy);
    }
    return true;
}

// end::pieces[]

// tag::lines[]
/**
 NDJSON: each document ends at its line's newline, so a line may end in CRLF, a line may be empty,
 and the last line may lack its newline, which finish closes.
*/
bool read_ndjson() {
    std::cout << "NDJSON, taken with next():\n";
    wd::stream_options options;
    options.framing = wd::framing::lines;
    wd::stream_parser p(options);
    constexpr std::string_view text =
        "{\"id\":1,\"tags\":[\"a\",\"b\"]}\n"
        "{\"id\":2,\"note\":\"first\\nsecond\"}\r\n"
        "\n"
        "[3,4.5]\n"
        "null";
    if (!write_in_pieces(p, text, options.on_error)) {
        return false;
    }
    if (const wd::result<std::size_t> finished = p.finish(); finished.has_error()) {
        report_error(p, finished.error());
        return false;
    }
    take_ready(p, options.on_error);
    return true;
}

// end::lines[]

// tag::concatenated[]
/**
 Concatenated JSON, with no separator or with indentation between documents, pushed to a callback:
 each document is handed over as soon as its last byte is consumed, so a callback sees it before
 the rest of the piece is read.
*/
bool push_concatenated() {
    std::cout << "concatenated, pushed to a callback:\n";
    wd::stream_parser p;
    constexpr std::string_view text =
        "{\"a\":1}    {\"b\":2}\n"
        "[1][2]\n"
        "{\n"
        "  \"c\": [\n"
        "    3\n"
        "  ]\n"
        "}";
    const auto print = [](wd::result<wd::document> made) {
        std::cout << (made.has_value() ? made->stringify() : made.error().message()) << '\n';
    };
    for (std::size_t at = 0; at < text.size(); at += 3) {
        if (const wd::result<std::size_t> written = p.write(text.substr(at, 3), print);
            written.has_error()) {
            report_error(p, written.error());
            return false;
        }
    }
    if (const wd::result<std::size_t> finished = p.finish(print); finished.has_error()) {
        report_error(p, finished.error());
        return false;
    }
    std::cout << p.documents() << " documents\n";
    return true;
}

// end::concatenated[]

// tag::skip[]
/**
 NDJSON with a bad line under on_error::skip: the bad line is reported with its line number, and the
 stream goes on from the next line.
*/
bool skip_bad_line() {
    std::cout << "NDJSON with a bad line, skipped:\n";
    wd::stream_options options;
    options.framing = wd::framing::lines;
    options.on_error = wd::on_error::skip;
    wd::stream_parser p(options);
    constexpr std::string_view text = "{\"ok\":1}\n{\"bad\":}\n[2]\n\"three\"";
    if (!write_in_pieces(p, text, options.on_error)) {
        return false;
    }
    if (const wd::result<std::size_t> finished = p.finish(); finished.has_error()) {
        report_error(p, finished.error());
        return false;
    }
    take_ready(p, options.on_error);
    return true;
}

// end::skip[]

// tag::rfc7464[]
/**
 JSON text sequences, RFC 7464: each record opens with the byte 0x1E, and its document may span
 several lines.
*/
bool read_rfc7464() {
    std::cout << "rfc7464 records:\n";
    wd::stream_options options;
    options.framing = wd::framing::rfc7464;
    wd::stream_parser p(options);
    constexpr std::string_view text =
        "\x1e{\"a\":1}\n"
        "\x1e{\n  \"b\": [\n    2\n  ]\n}\n"
        "\x1e\"three\"\n";
    if (!write_in_pieces(p, text, options.on_error)) {
        return false;
    }
    if (const wd::result<std::size_t> finished = p.finish(); finished.has_error()) {
        report_error(p, finished.error());
        return false;
    }
    take_ready(p, options.on_error);
    return true;
}

// end::rfc7464[]

}  // namespace

int main() {
    if (!read_ndjson() || !push_concatenated() || !skip_bad_line() || !read_rfc7464()) {
        return 1;
    }
}
