// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Streaming in both directions: JSON text fed to the parser in chunks split inside a string and a
// number, a syntax error and the offset where it was found, a parser that stays failed or finished
// until it is reset, the limits of a parse, and a document written in pieces to a sink.

#include <webcpp/document.hpp>

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace wd = webcpp::document;

namespace {

/** Prints what a write or a finish gave: nothing on success, the error otherwise. */
void report(std::string_view call, const wd::result<void>& r, const wd::parser& p) {
    std::cout << call << ": "
              << (r.has_value() ? "ok" : r.error().message() + " at " + std::to_string(p.offset()))
              << '\n';
}

}  // namespace

int main() {
    // tag::chunks[]
    wd::parser p;
    // The text {"name":"Ada","tags":["math"],"n":1.5e3}, split inside a string and a number.
    for (const std::string_view chunk :
         {R"({"name":"Ad)", R"(a","tags":["ma)", R"(th"],"n":1.)", R"(5e3})"}) {
        if (const wd::result<void> written = p.write(chunk); written.has_error()) {
            std::cout << "error: " << written.error().message() << '\n';
            return 1;
        }
    }
    wd::result<wd::document> made = p.finish();
    if (made.has_error()) {
        std::cout << "error: " << made.error().message() << '\n';
        return 1;
    }
    std::cout << made->stringify() << '\n';
    // end::chunks[]

    // tag::errors[]
    p.reset();
    report("write [1,2", p.write("[1,2"), p);
    report("write ,]", p.write(",]"), p);  // the error, at the byte where it was found
    std::cout << "cause: " << p.cause().message() << '\n';
    report("write 3]", p.write("3]"), p);  // a failed parser stays failed
    p.reset();
    report("write [1]", p.write("[1]"), p);
    const wd::result<wd::document> one = p.finish();
    std::cout << "finish: " << (one.has_value() ? one->stringify() : one.error().message()) << '\n';
    report("write [2]", p.write("[2]"), p);  // a finished parser takes no more text
    // end::errors[]

    // tag::limits[]
    wd::parse_options limits;
    limits.max_depth = 2;
    limits.max_bytes = 8;
    wd::parser limited(limits);
    report("write [[1],", limited.write("[[1],"), limited);
    report("write [2]]", limited.write("[2]]"), limited);  // 9 bytes in all
    std::cout << "[[[1]]]: " << wd::parse("[[[1]]]", limits).error().message() << '\n';
    // end::limits[]

    // tag::stringify_to[]
    // The same bytes as stringify, in pieces of at most 8 bytes, to a sink that prints each.
    const wd::result<void> written =
        wd::stringify_to(*made, [](std::string_view piece) { std::cout << piece << '|'; }, 0, 8);
    if (written.has_error()) {
        return 1;
    }
    std::cout << '\n';

    // A sink that returns a failure stops the writing, and stringify_to returns that failure.
    std::string kept;
    const wd::result<void> stopped = wd::stringify_to(
        *made,
        [&kept](std::string_view piece) -> wd::result<void> {
            if (!kept.empty()) {
                return wd::errc::capacity;
            }
            kept = piece;
            return {};
        },
        0, 8);
    std::cout << kept << " then " << stopped.error().message() << '\n';
    // end::stringify_to[]
}
